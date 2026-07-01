// SPDX-License-Identifier: GPL-2.0
/*
 * block/blk-flush.c: 블록 레이어의 PREFLUSH/FUA 시퀀싱 핵심 계층
 *
 * 이 파일은 NVMe SSD 관점에서 볼 때, 상위 파일시스템/블록 계층이 날린
 * REQ_PREFLUSH | REQ_FUA 쓰기 요청을, NVMe 컨트롤러가 이해할 수 있는
 * Flush 명령(Opcode 0x00)과 FUA(Force Unit Access) Write 명령으로
 * 분해/시퀀싱하는 역할을 한다.
 *
 * 주요 호출 경로:
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> blk_insert_flush
 * -> (필요시) blk_kick_flush -> blk_mq_kick_requeue_list
 * -> nvme_queue_rq -> nvme_submit_cmd(doorbell, CID, SQ)
 *
 * 연관 파일:
 * - block/blk-mq.c: 멀티큐 IO 스케줄링 및 nvme_queue_rq 로의 dispatch
 * - block/blk-merge.c: flush 요청은 merge 금지
 * - block/blk.h: struct blk_flush_queue 정의
 * - include/linux/blk-mq.h: struct request 및 flush 필드 정의
 */

/*
 * Functions to sequence PREFLUSH and FUA writes.
 *
 * Copyright (C) 2011		Max Planck Institute for Gravitational Physics
 * Copyright (C) 2011		Tejun Heo <tj@kernel.org>
 *
 * REQ_{PREFLUSH|FUA} requests are decomposed to sequences consisted of three
 * optional steps - PREFLUSH, DATA and POSTFLUSH - according to the request
 * properties and hardware capability.
 *
 * If a request doesn't have data, only REQ_PREFLUSH makes sense, which
 * indicates a simple flush request.  If there is data, REQ_PREFLUSH indicates
 * that the device cache should be flushed before the data is executed, and
 * REQ_FUA means that the data must be on non-volatile media on request
 * completion.
 *
 * If the device doesn't have writeback cache, PREFLUSH and FUA don't make any
 * difference.  The requests are either completed immediately if there's no data
 * or executed as normal requests otherwise.
 *
 * If the device has writeback cache and supports FUA, REQ_PREFLUSH is
 * translated to PREFLUSH but REQ_FUA is passed down directly with DATA.
 *
 * If the device has writeback cache and doesn't support FUA, REQ_PREFLUSH
 * is translated to PREFLUSH and REQ_FUA to POSTFLUSH.
 *
 * The actual execution of flush is double buffered.  Whenever a request
 * needs to execute PRE or POSTFLUSH, it queues at
 * fq->flush_queue[fq->flush_pending_idx].  Once certain criteria are met, a
 * REQ_OP_FLUSH is issued and the pending_idx is toggled.  When the flush
 * completes, all the requests which were pending are proceeded to the next
 * step.  This allows arbitrary merging of different types of PREFLUSH/FUA
 * requests.
 *
 * Currently, the following conditions are used to determine when to issue
 * flush.
 *
 * C1. At any given time, only one flush shall be in progress.  This makes
 *     double buffering sufficient.
 *
 * C2. Flush is deferred if any request is executing DATA of its sequence.
 *     This avoids issuing separate POSTFLUSHes for requests which shared
 *     PREFLUSH.
 *
 * C3. The second condition is ignored if there is a request which has
 *     waited longer than FLUSH_PENDING_TIMEOUT.  This is to avoid
 *     starvation in the unlikely case where there are continuous stream of
 *     FUA (without PREFLUSH) requests.
 *
 * For devices which support FUA, it isn't clear whether C2 (and thus C3)
 * is beneficial.
 *
 * Note that a sequenced PREFLUSH/FUA request with DATA is completed twice.
 * Once while executing DATA and again after the whole sequence is
 * complete.  The first completion updates the contained bio but doesn't
 * finish it so that the bio submitter is notified only after the whole
 * sequence is complete.  This is implemented by testing RQF_FLUSH_SEQ in
 * req_bio_endio().
 *
 * The above peculiarity requires that each PREFLUSH/FUA request has only one
 * bio attached to it, which is guaranteed as they aren't allowed to be
 * merged in the usual way.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/gfp.h>
#include <linux/part_stat.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"

/*
 * 주요 자료구조와 NVMe 연결:
 *
 * struct blk_flush_queue (block/blk.h):
 *   - mq_flush_lock: NVMe SQ/CQ 경쟁 상태 보호 (per-hctx flush 동기화)
 *   - flush_pending_idx/running_idx: 더블 버퍼링 인덱스.
 *     NVMe Flush 명령은 한 번에 하나만 in-flight 가능하도록 관리.
 *   - flush_queue[2]: PRE/POSTFLUSH 대기열. 같은 NVMe Flush 명령으로
 *     여러 요청을 묶어 발행할 수 있게 한다.
 *   - flush_data_in_flight: DATA 단계가 진행 중인 요청 수.
 *     NVMe FUA Write가 완료될 때까지 POSTFLUSH를 미루는 C2 조건에 사용.
 *   - flush_rq: 실제 NVMe Flush 명령으로 재활용되는 날것의 request.
 *     first_rq로부터 tag, mq_ctx, mq_hctx를 빌려온다.
 *   - rq_status: flush_rq 완료 시 커널 드라이버가 보고한 상태.
 *
 * struct request::flush (include/linux/blk-mq.h):
 *   - seq: REQ_FSEQ_* 비트마스크. NVMe 입장에서 PREFLUSH/DATA/POSTFLUSH
 *     중 어떤 단계를 수행해야 하는지 추적.
 *   - saved_end_io: DATA 완료 후 원래 completion handler 복원용.
 */

/* PREFLUSH/FUA sequences */
enum {
	REQ_FSEQ_PREFLUSH	= (1 << 0), /* pre-flushing in progress */
	REQ_FSEQ_DATA		= (1 << 1), /* data write in progress */
	REQ_FSEQ_POSTFLUSH	= (1 << 2), /* post-flushing in progress */
	REQ_FSEQ_DONE		= (1 << 3),

	REQ_FSEQ_ACTIONS	= REQ_FSEQ_PREFLUSH | REQ_FSEQ_DATA |
				  REQ_FSEQ_POSTFLUSH,

	/*
	 * If flush has been pending longer than the following timeout,
	 * it's issued even if flush_data requests are still in flight.
	 */
	FLUSH_PENDING_TIMEOUT	= 5 * HZ,
};

static void blk_kick_flush(struct request_queue *q,
			   struct blk_flush_queue *fq, blk_opf_t flags);

/*
 * blk_get_flush_queue: 요청이 속한 blk_mq_ctx로부터 해당 hctx의
 * struct blk_flush_queue를 가져온다.
 *
 * NVMe 연결: blk_mq_ctx는 소프트웨어 컨텍스트이고, 매핑된 hctx는
 * NVMe 하드웨어 큐(nvme_queue / SQ/CQ 쌍)에 대응한다. 따라서 flush
 * 상태머신도 hctx 단위로 분리되어 있다.
 */
static inline struct blk_flush_queue *
blk_get_flush_queue(struct blk_mq_ctx *ctx)
{
	return blk_mq_map_queue(REQ_OP_FLUSH, ctx)->fq; /* REQ_OP_FLUSH로 매핑된 hctx의 fq 반환: NVMe SQ/CQ 쌍 단위로 flush 상태 분리 */
}

/*
 * blk_flush_cur_seq: rq->flush.seq 의 0번 비트 위치를 찾아 현재
 * 수행해야 할 시퀀스를 반환한다.
 *
 * NVMe 연결: seq 비트가 0인 최하위 단계가 아직 완료되지 않은 단계.
 * 예: PREFLUSH가 끝나면 DATA, DATA가 끝나면 POSTFLUSH 순으로 진행.
 */
static unsigned int blk_flush_cur_seq(struct request *rq)
{
	return 1 << ffz(rq->flush.seq); /* ffz(rq->flush.seq): 아직 완료되지 않은 최하위 단계의 비트 번호 -> PREFLUSH/DATA/POSTFLUSH/DONE 중 현재 단계 */
}

/*
 * blk_flush_restore_request: DATA 완료 후 최종 완료를 위해 request를
 * 일반 요청 형태로 복원한다.
 *
 * 호출 경로:
 * flush_end_io(CQ 완료) -> blk_flush_complete_seq(..., REQ_FSEQ_DONE)
 * -> blk_flush_restore_request -> blk_mq_end_request
 */
static void blk_flush_restore_request(struct request *rq)
{
	/*
	 * After flush data completion, @rq->bio is %NULL but we need to
	 * complete the bio again.  @rq->biotail is guaranteed to equal the
	 * original @rq->bio.  Restore it.
	 */
	rq->bio = rq->biotail;		/* NVMe FUA Write 완료 후 bio 재설정: 상위 bio submitter에게 최종 완료 알리기 위해 */
	if (rq->bio)
		rq->__sector = rq->bio->bi_iter.bi_sector; /* bio의 시작 sector 복원: NVMe PRP/SGL 주소와 상위 계층의 sector 정보 동기화 */

	/* make @rq a normal request */
	rq->rq_flags &= ~RQF_FLUSH_SEQ; /* flush 시퀀스 종료 플래그 해제: 이후 req_bio_endio에서 최종 완료 처리 */
	rq->end_io = rq->flush.saved_end_io; /* 원래 completion handler 복원: NVMe 드라이버/스케줄러 정상 경로로 복귀 */
}

/*
 * blk_account_io_flush: NVMe Flush 명령이 완료된 후 통계를 기록한다.
 * ios[STAT_FLUSH]와 소요 시간(nsecs)을 갱신.
 */
static void blk_account_io_flush(struct request *rq)
{
	struct block_device *part = rq->q->disk->part0;

	part_stat_lock();
	part_stat_inc(part, ios[STAT_FLUSH]); /* NVMe Flush 명령 완료 횟수 증가 */
	part_stat_add(part, nsecs[STAT_FLUSH],
		      blk_time_get_ns() - rq->start_time_ns); /* NVMe Flush 완료 지연 측정: doorbell -> CQ 완료까지의 시간 */
	part_stat_unlock();
}

/**
 * blk_flush_complete_seq - complete flush sequence
 * @rq: PREFLUSH/FUA request being sequenced
 * @fq: flush queue
 * @seq: sequences to complete (mask of %REQ_FSEQ_*, can be zero)
 * @error: whether an error occurred
 *
 * @rq just completed @seq part of its flush sequence, record the
 * completion and trigger the next step.
 *
 * CONTEXT:
 * spin_lock_irq(fq->mq_flush_lock)
 *
 * NVMe 연결:
 *   - flush 시퀀스의 각 단계(PREFLUSH/DATA/POSTFLUSH/DONE)를 기록하고
 *     다음 단계로 전이시킨다.
 *   - PRE/POSTFLUSH 단계에서는 요청을 pending 대기열에 넣고
 *     blk_kick_flush()를 통해 단일 NVMe Flush 명령을 발행한다.
 *   - DATA 단계에서는 요청을 requeue_list로 옮겨 실제 NVMe Write(FUA)
 *     명령이 처리되도록 한다.
 *   - DONE 단계에서는 blk_mq_end_request()로 상위 계층에 최종 완료.
 *
 * 호출 경로:
 *   blk_insert_flush -> blk_flush_complete_seq
 *   flush_end_io(CQ 핸들러) -> blk_flush_complete_seq
 *   mq_flush_data_end_io(FUA Write 완료) -> blk_flush_complete_seq
 */
static void blk_flush_complete_seq(struct request *rq,
				   struct blk_flush_queue *fq,
				   unsigned int seq, blk_status_t error)
{
	struct request_queue *q = rq->q;
	struct list_head *pending = &fq->flush_queue[fq->flush_pending_idx]; /* PRE/POSTFLUSH 대기 중인 요청 리스트: 같은 NVMe Flush 명령에 묶일 후보 */
	blk_opf_t cmd_flags;

	BUG_ON(rq->flush.seq & seq); /* 동일 단계 중복 완료 방지: NVMe flush/state machine 오류 시 즉시 패닉 */
	rq->flush.seq |= seq; /* 완료한 단계의 비트를 rq->flush.seq에 기록: 다음 blk_flush_cur_seq()에서 다음 단계 결정 */
	cmd_flags = rq->cmd_flags; /* 원래 요청의 cmd_flags 저장: blk_kick_flush()에서 flush_rq에 REQ_DRV/FAILFAST 플래그 전달용 */

	if (likely(!error))
		seq = blk_flush_cur_seq(rq); /* 에러 없으면 다음 NVMe flush 단계 계산: PREFLUSH -> DATA -> POSTFLUSH -> DONE */
	else
		seq = REQ_FSEQ_DONE; /* 에러 발생 시 즉시 DONE으로 전이: NVMe 명령 실패 시 후속 flush 단계 중단 */

	switch (seq) {
	case REQ_FSEQ_PREFLUSH:
	case REQ_FSEQ_POSTFLUSH:
		/* queue for flush */
		if (list_empty(pending))
			fq->flush_pending_since = jiffies; /* 대기열이 비어있을 때 timestamp 기록: C3 timeout 조건 판정용 */
		/* NVMe Flush 명령에 묶여 같이 처리될 후보 요청을 pending 대기열에 추가 */
		list_add_tail(&rq->queuelist, pending); /* PRE/POSTFLUSH 요청을 pending 리스트에 append: 추후 blk_kick_flush에서 한 번에 NVMe Flush 명령으로 발행 */
		break;

	case REQ_FSEQ_DATA:
		fq->flush_data_in_flight++; /* 현재 NVMe Write(FUA)로 날아간 DATA 단계 요청 수 증가: C2에서 POSTFLUSH 지연 여부 판단 */
		spin_lock(&q->requeue_lock); /* requeue_list 보호: NVMe SQ dispatch 경로와 경쟁 */
		/* 실제 NVMe Write(FUA) 처리를 위해 requeue_list로 이동 */
		list_move(&rq->queuelist, &q->requeue_list); /* flush 시퀀스의 DATA 단계를 hardware queue dispatch 대기열로 이동 -> nvme_queue_rq에서 NVMe Write/FUA 처리 */
		spin_unlock(&q->requeue_lock);
		blk_mq_kick_requeue_list(q); /* requeue_list에 추가된 요청을 hardware queue로 밀어 넣음: nvme_queue_rq -> nvme_submit_cmd(doorbell) 호출 유도 */
		break;

	case REQ_FSEQ_DONE:
		/*
		 * @rq was previously adjusted by blk_insert_flush() for
		 * flush sequencing and may already have gone through the
		 * flush data request completion path.  Restore @rq for
		 * normal completion and end it.
		 */
		list_del_init(&rq->queuelist); /* flush 상태머신의 리스트에서 제거: NVMe flush 시퀀스 완전 종료 */
		blk_flush_restore_request(rq); /* request를 일반 완료 형태로 복원 */
		/* NVMe Flush/FUA 시퀀스가 모두 끝났으므로 상위에 최종 완료 보고 */
		blk_mq_end_request(rq, error); /* bio submitter에게 최종 완료: NVMe status -> blk_status_t 변환 후 완료 통계 기록 */
		break;

	default:
		BUG(); /* 알 수 없는 시퀀스: NVMe flush state machine 버그 */
	}

	/*
	 * PRE/POSTFLUSH 대기열이 꽉 차거나 조건이 맞으면 실제 NVMe Flush
	 * 명령을 발행하도록 시도한다.
	 */
	blk_kick_flush(q, fq, cmd_flags); /* 조건 충족 시 NVMe Flush 명령 발행: pending 대기열의 요청들을 하나의 flush_rq로 묶어 doorbell */
}

/*
 * flush_end_io: NVMe Flush 명령(REQ_OP_FLUSH)이 CQ를 통해 완료되었을 때
 * 호출되는 completion handler.
 *
 * 역할:
 *   - flush_rq의 refcount가 0이 될 때까지 기다린다 (timeout 경로에서도
 *     호출되므로 use-after-free 방지).
 *   - flush 통계를 기록하고 flush_rq 상태를 IDLE로 되돌린다.
 *   - 대기 중이던 PRE/POSTFLUSH 요청들을 다음 단계로 진행시킨다.
 *
 * 호출 경로 (추정):
 *   nvme_poll_cq -> nvme_process_cq -> nvme_complete_rq -> blk_mq_complete_request
 *   -> flush_rq->end_io (flush_end_io)
 */
static enum rq_end_io_ret flush_end_io(struct request *flush_rq,
				       blk_status_t error,
				       const struct io_comp_batch *iob)
{
	struct request_queue *q = flush_rq->q;
	struct list_head *running;
	struct request *rq, *n;
	unsigned long flags = 0;
	struct blk_flush_queue *fq = blk_get_flush_queue(flush_rq->mq_ctx);

	/* release the tag's ownership to the req cloned from */
	spin_lock_irqsave(&fq->mq_flush_lock, flags); /* per-hctx flush 상태 보호: NVMe CQ 핸들러와 blk_insert_flush 경쟁 방지 */

	/* timeout 경로에서도 호출될 수 있으므로 refcount 기반 종료 판정 */
	if (!req_ref_put_and_test(flush_rq)) { /* flush_rq의 마지막 참조자가 아니면 종료 연기: timeout/aborted NVMe flush에서 안전한 해제 */
		fq->rq_status = error; /* 완료 보류 중 에러 상태 저장: 마지막 참조 종료 시 NVMe status 전파 */
		spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
		return RQ_END_IO_NONE;
	}

	blk_account_io_flush(flush_rq); /* NVMe Flush 명령 완료 통계 기록 */
	/*
	 * Flush request has to be marked as IDLE when it is really ended
	 * because its .end_io() is called from timeout code path too for
	 * avoiding use-after-free.
	 */
	WRITE_ONCE(flush_rq->state, MQ_RQ_IDLE); /* request 상태를 IDLE로 기록: NVMe timeout 경로에서 재발행 가능한 상태로 전환 */
	if (fq->rq_status != BLK_STS_OK) { /* 이전에 보류된 에러가 있으면 */
		error = fq->rq_status; /* NVMe flush 완료 status를 보류된 에러로 덮어씀 */
		fq->rq_status = BLK_STS_OK; /* 보류 상태 초기화: 다음 NVMe Flush 명령을 위해 */
	}

	/*
	 * scheduler 유무에 따라 NVMe SQ에 내걸었던 tag/scheduler tag를
	 * 반납한다. 이 태그는 원래 first_rq로부터 빌려온 것이다.
	 */
	if (!q->elevator) {
		flush_rq->tag = BLK_MQ_NO_TAG; /* non-scheduler 모드: SQ slot(CID) 반납 표시, sbitmap 태그 재할당 가능 */
	} else {
		blk_mq_put_driver_tag(flush_rq); /* scheduler 모드: NVMe hardware tag(driver tag) 반납 -> nvme_sq가 doorbell 없이 slot 재사용 가능 */
		flush_rq->internal_tag = BLK_MQ_NO_TAG; /* scheduler internal tag 반납: scheduler tag bitmap 해제 */
	}

	/*
	 * running 대기열은 flush_pending_idx ^ 1, 즉 방금 완료된 NVMe Flush
	 * 명령에 묶였던 요청들이다.
	 */
	running = &fq->flush_queue[fq->flush_running_idx]; /* 방금 완료된 NVMe Flush 명령에 묶인 요청 리스트 */
	BUG_ON(fq->flush_pending_idx == fq->flush_running_idx); /* pending과 running이 같으면 NVMe Flush in-flight 위반: C1 조건 깨짐 */

	/* account completion of the flush request */
	fq->flush_running_idx ^= 1; /* running_idx toggle: 완료된 flush 제거, 반대편 대기열이 다음 flush 발행 후보가 됨 */

	/* and push the waiting requests to the next stage */
	list_for_each_entry_safe(rq, n, running, queuelist) { /* 완료된 NVMe Flush 명령에 묶인 모든 요청 순회: 각각 다음 단계로 전이 */
		unsigned int seq = blk_flush_cur_seq(rq); /* 각 요청의 현재 단계 확인: PREFLUSH 완료 후면 DATA, POSTFLUSH 완료 후면 DONE */

		BUG_ON(seq != REQ_FSEQ_PREFLUSH && seq != REQ_FSEQ_POSTFLUSH); /* running 리스트의 요청은 PRE/POSTFLUSH 중 하나여야 함: NVMe flush state 위반 시 패닉 */
		list_del_init(&rq->queuelist); /* running 리스트에서 제거: 다음 NVMe flush 시퀀스 단계로 이동 */
		/*
		 * PREFLUSH 완료 후에는 DATA로, POSTFLUSH 완료 후에는 DONE으로
		 * 다음 NVMe 시퀀스 단계로 전이.
		 */
		blk_flush_complete_seq(rq, fq, seq, error); /* PREFLUSH -> NVMe Write(FUA), POSTFLUSH -> 최종 완료 */
	}

	spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
	return RQ_END_IO_NONE;
}

/*
 * is_flush_rq: 주어진 request가 blk_flush_queue가 날것의 flush_rq인지
 * 판별. NVMe 드라이버는 flush_rq에 대해 일반 데이터 경로와 동일하게
 * 처리하지만, completion은 flush_end_io로 간다.
 */
bool is_flush_rq(struct request *rq)
{
	return rq->end_io == flush_end_io; /* end_io가 flush_end_io면 이 request는 NVMe Flush 명령용 flush_rq */
}

/**
 * blk_kick_flush - consider issuing flush request
 * @q: request_queue being kicked
 * @fq: flush queue
 * @flags: cmd_flags of the original request
 *
 * Flush related states of @q have changed, consider issuing flush request.
 * Please read the comment at the top of this file for more info.
 *
 * CONTEXT:
 * spin_lock_irq(fq->mq_flush_lock)
 *
 * NVMe 연결:
 *   - fq->flush_queue[fq->flush_pending_idx]에 쌓인 PRE/POSTFLUSH
 *     요청들을 하나의 NVMe Flush 명령으로 묶어 발행한다.
 *   - first_rq로부터 tag/mq_ctx/mq_hctx를 복사해 flush_rq가 실제
 *     NVMe SQ slot과 doorbell 경로를 빌려 쓸 수 있게 한다.
 *   - 발행 후 pending_idx를 toggle하여 running_idx와 다르게 만든다.
 *     이는 NVMe Flush 명령이 SQ에 들어가 있음(in-flight)을 표시.
 *
 * 호출 경로:
 *   blk_flush_complete_seq -> blk_kick_flush
 *   -> blk_mq_kick_requeue_list -> blk_mq_run_hw_queue
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
static void blk_kick_flush(struct request_queue *q, struct blk_flush_queue *fq,
			   blk_opf_t flags)
{
	struct list_head *pending = &fq->flush_queue[fq->flush_pending_idx]; /* 현재 NVMe Flush 명령 발행 후보 대기열 */
	struct request *first_rq =
		list_first_entry(pending, struct request, queuelist); /* 대기열의 첫 번째 요청: flush_rq가 빌릴 tag, mq_hctx, mq_ctx 제공 */
	struct request *flush_rq = fq->flush_rq; /* NVMe Flush 명령에 재활용되는 날것의 request */

	/* C1 described at the top of this file */
	/* C1: NVMe Flush 명령은 한 번에 하나만 in-flight 가능 */
	if (fq->flush_pending_idx != fq->flush_running_idx || list_empty(pending)) /* flush 이미 in-flight이거나 대기열이 비어있으면 발행 금지: NVMe Flush 동시 발행 방지 */
		return;

	/* C2 and C3 */
	/*
	 * C2: DATA 단계(FUA Write)가 아직 비행 중이면 POSTFLUSH를 미룬다.
	 * C3: 단, FLUSH_PENDING_TIMEOUT(5*HZ) 이상 대기한 요청이 있으면
	 *     강제로 NVMe Flush 명령을 발행해 기아(starvation) 방지.
	 */
	if (fq->flush_data_in_flight && /* DATA 단계(FUA Write)가 NVMe SQ/CQ에서 아직 진행 중 */
	    time_before(jiffies,
			fq->flush_pending_since + FLUSH_PENDING_TIMEOUT)) /* 5*HZ 이내 대기 중이면 timeout 아님: C3 조건 미충족 */
		return;

	/*
	 * Issue flush and toggle pending_idx.  This makes pending_idx
	 * different from running_idx, which means flush is in flight.
	 */
	fq->flush_pending_idx ^= 1; /* NVMe Flush 명령 발행 직후 pending toggle: flush in-flight 표시, 반대편 대기열을 새 pending으로 */

	blk_rq_init(q, flush_rq); /* flush_rq를 깨끗이 초기화: 이전 NVMe Flush 명령의 residual 상태 제거 */

	/*
	 * In case of none scheduler, borrow tag from the first request
	 * since they can't be in flight at the same time. And acquire
	 * the tag's ownership for flush req.
	 *
	 * In case of IO scheduler, flush rq need to borrow scheduler tag
	 * just for cheating put/get driver tag.
	 */
	/*
	 * NVMe 입장: flush_rq는 first_rq의 mq_ctx(소프트웨어 컨텍스트),
	 * mq_hctx(하드웨어 큐), tag(SQ 내 CID에 대응)를 빌려온다.
	 * scheduler 미사용 시 tag를, scheduler 사용 시 internal_tag를 복사.
	 */
	flush_rq->mq_ctx = first_rq->mq_ctx; /* NVMe SQ/CQ에 연결될 소프트웨어 컨텍스트 복사 */
	flush_rq->mq_hctx = first_rq->mq_hctx; /* NVMe SQ/CQ 쌍(hctx) 빌림: doorbell과 CQ 매핑 결정 */

	if (!q->elevator)
		flush_rq->tag = first_rq->tag; /* non-scheduler: NVMe SQ slot(CID에 대응)을 first_rq로부터 빌림. first_rq는 flush 완료 전까지 stall */
	else
		flush_rq->internal_tag = first_rq->internal_tag; /* scheduler: scheduler tag를 빌림. blk_mq_get_driver_tag 시 실제 NVMe SQ slot 할당받도록 */

	/* NVMe Flush 명령으로 사용할 operation과 flag 설정 */
	flush_rq->cmd_flags = REQ_OP_FLUSH | REQ_PREFLUSH; /* NVMe Flush 명령(Opcode 0x00)임을 표시: 상위에서 REQ_OP_FLUSH로 인식 */
	flush_rq->cmd_flags |= (flags & REQ_DRV) | (flags & REQ_FAILFAST_MASK); /* 원 요청의 driver-specific 플래그와 failfast 정책 상속: NVMe timeout/abort 정책에 영향 */
	flush_rq->rq_flags |= RQF_FLUSH_SEQ; /* flush 시퀀스 요청임을 표시: req_bio_endio에서 최종 완료 지연 처리 */
	flush_rq->end_io = flush_end_io; /* NVMe CQ 완료 시 flush_end_io 호출: nvme_complete_request -> rq->end_io */
	/*
	 * Order WRITE ->end_io and WRITE rq->ref, and its pair is the one
	 * implied in refcount_inc_not_zero() called from
	 * blk_mq_find_and_get_req(), which orders WRITE/READ flush_rq->ref
	 * and READ flush_rq->end_io
	 */
	smp_wmb(); /* memory barrier: end_io/rq_flags/tag 쓰기가 refcount 관찰 전에 NVMe CQ/timeout 경로에 보이도록 */
	req_ref_set(flush_rq, 1); /* flush_rq 참조 카운트 설정: timeout 경로에서도 안전한 completion 처리 */

	/*
	 * requeue_list/flush_list로 넣어 blk_mq_run_hw_queue를 통해
	 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 진입.
	 */
	spin_lock(&q->requeue_lock); /* flush_list 보호: NVMe dispatch 경로와 동기화 */
	list_add_tail(&flush_rq->queuelist, &q->flush_list); /* flush_rq를 hardware queue의 flush_list에 추가: nvme_queue_rq에서 REQ_OP_FLUSH로 NVMe Flush 명령 생성 */
	spin_unlock(&q->requeue_lock);

	blk_mq_kick_requeue_list(q); /* hardware queue喚醒: hctx->run_work 예약 -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 타이밍 */
}

/*
 * mq_flush_data_end_io: NVMe Write(FUA 포함) DATA 단계가 완료되면
 * 호출되는 completion handler.
 *
 * 역할:
 *   - FUA Write가 끝났으므로 flush_data_in_flight를 감소.
 *   - POSTFLUSH가 필요한 요청이면 다음 flush 시퀀스 단계로 전이.
 *   - hctx를 재시작해 대기 중인 다른 IO가 NVMe SQ로 들어갈 수 있게 함.
 *
 * 호출 경로 (추정):
 *   nvme_poll_cq -> nvme_process_cq -> nvme_complete_rq
 *   -> blk_mq_complete_request -> rq->end_io (mq_flush_data_end_io)
 */
static enum rq_end_io_ret mq_flush_data_end_io(struct request *rq,
					       blk_status_t error,
					       const struct io_comp_batch *iob)
{
	struct request_queue *q = rq->q;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx; /* NVMe SQ/CQ 쌍(hctx) 식별: 완료 후 동일 hctx 재시작 */
	struct blk_mq_ctx *ctx = rq->mq_ctx; /* 요청을 제출한 software context */
	unsigned long flags;
	struct blk_flush_queue *fq = blk_get_flush_queue(ctx); /* 해당 hctx의 flush 상태머신 가져옴 */

	if (q->elevator) {
		WARN_ON(rq->tag < 0); /* scheduler 모드에서 driver tag가 제대로 할당되었는지 확인: NVMe SQ slot 누수 방지 */
		blk_mq_put_driver_tag(rq); /* NVMe hardware tag 반납: CQ에서 완료된 NVMe Write의 SQ slot 회수 */
	}

	/*
	 * After populating an empty queue, kick it to avoid stall.  Read
	 * the comment in flush_end_io().
	 */
	spin_lock_irqsave(&fq->mq_flush_lock, flags); /* per-hctx flush 상태 동기화: flush_end_io와의 경쟁 방지 */
	fq->flush_data_in_flight--; /* NVMe FUA Write가 완료되었으므로 in-flight DATA 카운트 감소: C2 POSTFLUSH 지연 조건 해제 */
	/*
	 * May have been corrupted by rq->rq_next reuse, we need to
	 * re-initialize rq->queuelist before reusing it here.
	 */
	INIT_LIST_HEAD(&rq->queuelist); /* list 재사용 전 초기화: NVMe completion batch에서 rq_next 재사용으로 인한 list corruption 방지 */
	/*
	 * DATA(FUA Write)가 완료되었으므로, POSTFLUSH가 남아있다면
	 * flush_pending 대기열에 넣고 NVMe Flush 명령 발행을 시도.
	 */
	blk_flush_complete_seq(rq, fq, REQ_FSEQ_DATA, error); /* DATA 단계 완료 기록: POSTFLUSH가 필요하면 pending 대기열, 아니면 DONE으로 */
	spin_unlock_irqrestore(&fq->mq_flush_lock, flags);

	blk_mq_sched_restart(hctx); /* NVMe hctx 재시작: 완료로 비워진 SQ slot에 후속 NVMe 명령 dispatch 가능하도록 */
	return RQ_END_IO_NONE;
}

/*
 * blk_rq_init_flush: request를 flush 시퀀스 상태머신에 참여시키기 위해
 * 초기화. DATA 완료 시 mq_flush_data_end_io가 호출되도록 end_io를
 * 교체하고, 원래 end_io는 saved_end_io에 저장.
 */
static void blk_rq_init_flush(struct request *rq)
{
	rq->flush.seq = 0; /* flush 시퀀스 비트 초기화: 아무 단계도 완료되지 않은 상태 */
	rq->rq_flags |= RQF_FLUSH_SEQ; /* flush 시퀀스 요청임을 표시: req_bio_endio에서 중간 완료와 최종 완료 구분 */
	rq->flush.saved_end_io = rq->end_io; /* Usually NULL */ /* 원래 completion handler 저장: NVMe flush 완료 후 복원 */
	rq->end_io = mq_flush_data_end_io; /* NVMe FUA Write(DATA) 완료 시 mq_flush_data_end_io 호출되도록 교체 */
}

/*
 * NVMe 관점: 상위 계층에서 날아온 PREFLUSH/FUA 요청을 분석해 NVMe
 * 컨트롤러가 실제로 수행해야 할 시퀀스로 분해한다.
 *
 * 처리 정책:
 *   - 캐시 없음: flush가 의미 없으므로 즉시 완료하거나 일반 요청으로 처리.
 *   - 캐시 있고 FUA 지원: REQ_PREFLUSH -> PREFLUSH, REQ_FUA는 DATA에
 *     그대로 전달되어 NVMe Write 명령의 FUA 비트로 사용.
 *   - 캐시 있고 FUA 미지원: REQ_PREFLUSH -> PREFLUSH, REQ_FUA -> POSTFLUSH.
 *
 * 반환값:
 *   true  : flush 상태머신이 요청을 소비함 (상위 호출자가 더 이상 처리 안 함)
 *   false : 일반 데이터 경로로 처리해야 함
 *
 * 호출 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> blk_insert_flush
 */
/*
 * Insert a PREFLUSH/FUA request into the flush state machine.
 * Returns true if the request has been consumed by the flush state machine,
 * or false if the caller should continue to process it.
 */
bool blk_insert_flush(struct request *rq)
{
	struct request_queue *q = rq->q; /* 요청이 속한 request_queue: q->limits에서 NVMe FUA/write-cache 능력 확인 */
	struct blk_flush_queue *fq = blk_get_flush_queue(rq->mq_ctx); /* 요청이 매핑된 hctx(NVMe SQ/CQ 쌍)의 flush queue */
	bool supports_fua = q->limits.features & BLK_FEAT_FUA; /* NVMe 컨트롤러가 FUA를 지원하는지: Write 명령의 FUA 비트 사용 가능 여부 */
	unsigned int policy = 0;

	/* FLUSH/FUA request must never be merged */
	/* flush/FUA 요청은 bio가 정확히 하나여야 하며 merge 불가 */
	WARN_ON_ONCE(rq->bio != rq->biotail); /* bio가 정확히 하나임을 보장: merge되면 PREFLUSH/FUA 시퀀싱과 bio 완료 로직이 깨짐 */

	if (blk_rq_sectors(rq))
		policy |= REQ_FSEQ_DATA; /* 데이터 섹터가 있으면 DATA 단계 추가: NVMe Write(FUA) 명령 필요 */

	/*
	 * Check which flushes we need to sequence for this operation.
	 */
	/*
	 * NVMe 컨트롤러의 volatile write cache(VWC) 지원 여부와 FUA 지원
	 * 여부에 따라 시퀀스를 결정.
	 */
	if (blk_queue_write_cache(q)) { /* NVMe volatile write cache가 있으면 Flush 의미 있음 */
		if (rq->cmd_flags & REQ_PREFLUSH)
			policy |= REQ_FSEQ_PREFLUSH; /* 쓰기 전 NVMe Flush 명령 필요 */
		if ((rq->cmd_flags & REQ_FUA) && !supports_fua)
			policy |= REQ_FSEQ_POSTFLUSH; /* FUA 미지원 시 쓰기 후 NVMe Flush 명령으로 강제 배출 */
	}

	/*
	 * @policy now records what operations need to be done.  Adjust
	 * REQ_PREFLUSH and FUA for the driver.
	 */
	/*
	 * NVMe 드라이버에 전달하기 전 REQ_PREFLUSH/FUA 플래그를 정리.
	 * FUA를 지원하면 Write 명령의 FUA 비트로 그대로 사용.
	 */
	rq->cmd_flags &= ~REQ_PREFLUSH; /* REQ_PREFLUSH는 이미 시퀀스로 분핐으므로 NVMe Write 명령에는 전달하지 않음 */
	if (!supports_fua)
		rq->cmd_flags &= ~REQ_FUA; /* FUA 미지원 컨트롤러: REQ_FUA 플래그 제거, POSTFLUSH로 대체 */

	/*
	 * REQ_PREFLUSH|REQ_FUA implies REQ_SYNC, so if we clear any
	 * of those flags, we have to set REQ_SYNC to avoid skewing
	 * the request accounting.
	 */
	rq->cmd_flags |= REQ_SYNC; /* sync 요청으로 표시: NVMe scheduler/계정에서 우선순위/통계 보정 */

	switch (policy) {
	case 0:
		/*
		 * An empty flush handed down from a stacking driver may
		 * translate into nothing if the underlying device does not
		 * advertise a write-back cache.  In this case, simply
		 * complete the request.
		 */
		/*
		 * NVMe 컨트롤러가 write-back cache를 광고하지 않으면 Flush
		 * 명령이 필요 없으므로 즉시 완료.
		 */
		blk_mq_end_request(rq, 0); /* NVMe Flush 불필요: 상위에 즉시 완료 보고 */
		return true;
	case REQ_FSEQ_DATA:
		/*
		 * If there's data, but no flush is necessary, the request can
		 * be processed directly without going through flush machinery.
		 * Queue for normal execution.
		 */
		/*
		 * 데이터만 있고 flush가 필요 없으면 일반 NVMe Write 경로로
		 * 전달. FUA가 설정되어 있고 지원되면 NVMe Write FUA 비트로
		 * 그대로 사용됨.
		 */
		return false; /* 일반 NVMe Write/FUA 경로로 처리: blk_mq_get_request 호출자가 dispatch 계속 */
	case REQ_FSEQ_DATA | REQ_FSEQ_POSTFLUSH:
		/*
		 * Initialize the flush fields and completion handler to trigger
		 * the post flush, and then just pass the command on.
		 */
		/*
		 * FUA 미지원 컨트롤러: DATA Write 먼저 수행 후 POSTFLUSH
		 * (NVMe Flush) 발행. blk_rq_init_flush로 end_io를 교체.
		 */
		blk_rq_init_flush(rq); /* flush 시퀀스 등록: DATA 완료 후 mq_flush_data_end_io -> POSTFLUSH */
		rq->flush.seq |= REQ_FSEQ_PREFLUSH; /* PREFLUSH는 없지만 POSTFLUSH를 위해 PREFLUSH 단계를 이미 지난 것처럼 마킹 */
		spin_lock_irq(&fq->mq_flush_lock);
		fq->flush_data_in_flight++; /* DATA Write가 NVMe SQ로 발행되기 전에 in-flight 카운트 선증가: C2 POSTFLUSH 지연 준비 */
		spin_unlock_irq(&fq->mq_flush_lock);
		return false; /* DATA Write를 일반 경로로 dispatch: nvme_queue_rq에서 NVMe Write 처리 */
	default:
		/*
		 * Mark the request as part of a flush sequence and submit it
		 * for further processing to the flush state machine.
		 */
		/*
		 * PREFLUSH가 필요한 경우: flush 상태머신에 넣어 NVMe Flush
		 * 명령을 먼저 발행한 뒤 DATA 단계로 진행.
		 */
		blk_rq_init_flush(rq); /* flush 시퀀스 등록: PREFLUSH 완료 후 DATA, DATA 완료 후 POSTFLUSH */
		spin_lock_irq(&fq->mq_flush_lock);
		blk_flush_complete_seq(rq, fq, REQ_FSEQ_ACTIONS & ~policy, 0); /* 아직 수행되지 않은 단계를 완료된 것으로 마킹 후 현재 단계로 전이 */
		spin_unlock_irq(&fq->mq_flush_lock);
		return true;
	}
}

/**
 * blkdev_issue_flush - queue a flush
 * @bdev:	blockdev to issue flush for
 *
 * Description:
 *    Issue a flush for the block device in question.
 *
 * NVMe 연결: 사용자공간/상위 계층에서 명시적 flush(sync 등) 요청 시
 * submit_bio_wait를 통해 REQ_OP_WRITE | REQ_PREFLUSH bio를 생성,
 * 이후 blk_insert_flush에서 NVMe Flush 명령으로 변환.
 */
int blkdev_issue_flush(struct block_device *bdev)
{
	struct bio bio;

	bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH); /* 데이터 없는 PREFLUSH bio 생성: NVMe Flush 명령으로 변환됨 */
	return submit_bio_wait(&bio); /* bio 제출 및 완료 대기: -> blk_insert_flush -> NVMe Flush -> CQ 완료까지 블록 */
}
EXPORT_SYMBOL(blkdev_issue_flush);

/*
 * blk_alloc_flush_queue: hctx마다 하나씩 존재하는 struct blk_flush_queue와
 * 그 안의 flush_rq를 할당 및 초기화.
 *
 * NVMe 연결: 각 NVMe SQ/CQ 쌍(hctx)마다 독립적인 flush 상태머신이
 * 필요하므로 per-hctx로 할당된다.
 */
struct blk_flush_queue *blk_alloc_flush_queue(int node, int cmd_size,
					      gfp_t flags)
{
	struct blk_flush_queue *fq;
	int rq_sz = sizeof(struct request);

	fq = kzalloc_node(sizeof(*fq), flags, node); /* per-hctx NVMe flush 상태머신 메모리 할당 */
	if (!fq)
		goto fail;

	spin_lock_init(&fq->mq_flush_lock); /* per-hctx flush lock 초기화: NVMe CQ/timeout/insert 경쟁 보호 */

	rq_sz = round_up(rq_sz + cmd_size, cache_line_size()); /* request 크기 + driver cmd_size를 cache line 정렬: NVMe sqe/payload가 캐시 라인 단위로 배치됨 (추정) */
	fq->flush_rq = kzalloc_node(rq_sz, flags, node); /* NVMe Flush 명령 재활용용 request 할당 */
	if (!fq->flush_rq)
		goto fail_rq;

	INIT_LIST_HEAD(&fq->flush_queue[0]); /* PRE/POSTFLUSH 대기열 0 초기화 */
	INIT_LIST_HEAD(&fq->flush_queue[1]); /* PRE/POSTFLUSH 대기열 1 초기화: 더블 버퍼링 */

	return fq;

 fail_rq:
	kfree(fq);
 fail:
	return NULL;
}

/*
 * blk_free_flush_queue: blk_flush_queue와 flush_rq를 해제.
 * bio 기반 request queue에는 flush queue가 없을 수 있다.
 */
void blk_free_flush_queue(struct blk_flush_queue *fq)
{
	/* bio based request queue hasn't flush queue */
	if (!fq)
		return; /* bio-only queue에는 flush 상태머신이 없으므로 아무것도 하지 않음 */

	kfree(fq->flush_rq); /* NVMe Flush 명령용 request 해제 */
	kfree(fq); /* per-hctx flush 상태머신 해제 */
}

/*
 * NVMe 관점: nvme-loop 등에서 flush 완료 처리가 재귀적으로 호출될 수
 * 있어 lockdep이 "recursive locking"으로 오인할 수 있다. 이 함수는
 * fq->mq_flush_lock에 별도 lock class key를 지정해 해당 false positive를
 * 피한다.
 */
/*
 * Allow driver to set its own lock class to fq->mq_flush_lock for
 * avoiding lockdep complaint.
 *
 * flush_end_io() may be called recursively from some driver, such as
 * nvme-loop, so lockdep may complain 'possible recursive locking' because
 * all 'struct blk_flush_queue' instance share same mq_flush_lock lock class
 * key. We need to assign different lock class for these driver's
 * fq->mq_flush_lock for avoiding the lockdep warning.
 *
 * Use dynamically allocated lock class key for each 'blk_flush_queue'
 * instance is over-kill, and more worse it introduces horrible boot delay
 * issue because synchronize_rcu() is implied in lockdep_unregister_key which
 * is called for each hctx release. SCSI probing may synchronously create and
 * destroy lots of MQ request_queues for non-existent devices, and some robot
 * test kernel always enable lockdep option. It is observed that more than half
 * an hour is taken during SCSI MQ probe with per-fq lock class.
 */
void blk_mq_hctx_set_fq_lock_class(struct blk_mq_hw_ctx *hctx,
		struct lock_class_key *key)
{
	lockdep_set_class(&hctx->fq->mq_flush_lock, key); /* per-hctx flush lock에 별도 lockdep class 지정: nvme-loop 재귀 완료 경로의 false positive 회피 */
}
EXPORT_SYMBOL_GPL(blk_mq_hctx_set_fq_lock_class);

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 REQ_PREFLUSH/REQ_FUA를 NVMe Flush 명령(Opcode 0x00)과
 *   NVMe Write(FUA) 명령으로 분해/시퀀싱하는 블록 계층의 핵심 변환기이다.
 *
 * - struct blk_flush_queue는 per-hctx(NVMe SQ/CQ 쌍 단위)로 존재하며,
 *   flush_pending_idx/running_idx 더블 버퍼링을 통해 동시에 하나의
 *   NVMe Flush 명령만 in-flight되도록 보장한다.
 *
 * - blk_kick_flush는 대기 중인 PRE/POSTFLUSH 요청을 하나의 flush_rq에
 *   묶어 first_rq로부터 tag/mq_hctx를 빌려 NVMe doorbell 경로로 날린다.
 *
 * - FUA를 지원하는 NVMe 컨트롤러는 REQ_FUA를 NVMe Write 명령의 FUA
 *   비트로 직접 전달하며, POSTFLUSH가 필요 없다.
 *
 * - blk_insert_flush는 block/blk-mq.c의 요청 할당 경로와 연결되고,
 *   blkdev_issue_flush는 상위/사용자공간의 명시적 sync flush 요청을
 *   처리한다.
 */
