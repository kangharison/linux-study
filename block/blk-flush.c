// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어] PREFLUSH/FUA 시퀀싱 핵심 계층 (block/blk-flush.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 상위 파일시스템/블록 계층이 발행한 REQ_PREFLUSH | REQ_FUA 쓰기
 * 요청을 NVMe 컨트롤러가 이해하는 명령으로 분해·시퀀싱한다. NVMe Flush
 * 명령(Opcode 0x00)과 FUA(Force Unit Access) Write 명령의 순서(PREFLUSH →
 * DATA → POSTFLUSH)를 이 파일이 결정하며, 여러 요청을 하나의 NVMe Flush로
 * 묶어 발행하는 더블 버퍼링 기법(flush_pending_idx / flush_running_idx)을
 * 구현한다. 상위로부터 PREFLUSH/FUA 요청이 내려오는 모든 경로의 관문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 블록 계층 실행 흐름에서 blk-mq (request 할당·dispatch) 바로 아래에
 * 위치하며, NVMe 드라이버(nvme_queue_rq) 직전 계층이다:
 *   submit_bio
 *     → blk_mq_submit_bio (block/blk-mq.c)
 *       → blk_insert_flush (이 파일: REQ_PREFLUSH/FUA 시퀀스 결정)
 *         → blk_kick_flush (이 파일: NVMe Flush 명령 발행)
 *           → blk_mq_kick_requeue_list → nvme_queue_rq → doorbell
 *   NVMe CQ 완료: nvme_complete_rq → flush_end_io (이 파일: 다음 단계 전이)
 * 실행 컨텍스트: 커널 소프트웨어(submit 경로)와 NVMe CQ 인터럽트/폴링 양쪽
 * 에서 호출된다. fq->mq_flush_lock 으로 두 경로의 경쟁을 직렬화한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - block/blk-mq.c: blk_mq_end_request(), blk_mq_kick_requeue_list(),
 *     blk_mq_put_driver_tag() 등 request 생명주기 API
 *   - block/blk-mq-sched.h: blk_mq_sched_restart() (hctx 재시작)
 *   - block/blk.h: struct blk_flush_queue 자료구조 정의
 * 이 모듈에 의존하는 모듈:
 *   - block/blk-mq.c: blk_insert_flush() 를 요청 할당 경로에서 호출
 *   - NVMe 드라이버: flush_end_io() 를 CQ 완료 핸들러로 사용
 * 데이터 흐름:
 *   blk_insert_flush (분해·상태머신 진입)
 *     → fq->flush_queue[pending_idx] (PRE/POSTFLUSH 대기열)
 *     → blk_kick_flush → flush_rq → NVMe SQ → CQ
 *     → flush_end_io → blk_flush_complete_seq (다음 단계)
 *     → blk_mq_end_request (bio 최종 완료)
 *
 * === 주요 함수/구조체 요약 ===
 * blk_insert_flush()        : PREFLUSH/FUA 요청을 분석해 시퀀스 결정, 상태머신 진입
 * blk_kick_flush()          : 조건 충족 시 pending 대기열을 하나의 NVMe Flush로 발행
 * blk_flush_complete_seq()  : 시퀀스 단계 완료 기록 + 다음 단계 전이
 * flush_end_io()            : NVMe CQ에서 Flush 완료 시 호출되는 completion handler
 * mq_flush_data_end_io()    : NVMe Write(FUA) DATA 완료 시 POSTFLUSH 전이
 * blk_alloc_flush_queue()   : per-hctx flush 상태머신 자료구조 할당·초기화
 * struct blk_flush_queue (blk.h):
 *   mq_flush_lock           : flush 상태머신 직렬화 spinlock (IRQ-safe)
 *   flush_queue[2]          : 더블 버퍼 PRE/POSTFLUSH 대기열
 *   flush_pending_idx       : 현재 대기 중인 버퍼 인덱스
 *   flush_running_idx       : 현재 in-flight NVMe Flush가 묶인 버퍼 인덱스
 *   flush_data_in_flight    : DATA(FUA Write) in-flight 요청 수 (C2 조건용)
 *   flush_rq                : NVMe Flush 명령에 재활용되는 날것의 request
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

#include <linux/kernel.h>	/* [한국어] 커널 기본 타입/매크로: BUG_ON, WARN_ON_ONCE, container_of */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL: blk_insert_flush 등 공개 API 등록 */
#include <linux/bio.h>		/* [한국어] struct bio, bio_init(), submit_bio_wait() */
#include <linux/blkdev.h>	/* [한국어] struct block_device, blkdev_issue_flush() 프로토타입 */
#include <linux/gfp.h>		/* [한국어] kzalloc_node()에 전달할 GFP 플래그 */
#include <linux/part_stat.h>	/* [한국어] part_stat_lock/unlock/inc/add: NVMe Flush 완료 통계 기록 */

#include "blk.h"		/* [한국어] struct blk_flush_queue 정의, blk_rq_init() */
#include "blk-mq.h"		/* [한국어] blk_mq_end_request(), blk_mq_kick_requeue_list(), blk_mq_put_driver_tag() */
#include "blk-mq-sched.h"	/* [한국어] blk_mq_sched_restart(): DATA 완료 후 hctx 재시작 */

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
/* [한국어] NVMe Flush/FUA 시퀀스 단계를 나타내는 비트마스크 상수 */
enum {
	REQ_FSEQ_PREFLUSH	= (1 << 0), /* [한국어] pre-flushing 진행 중: NVMe Flush 명령(Opcode 0x00)이 SQ→CQ */
	REQ_FSEQ_DATA		= (1 << 1), /* [한국어] data write 진행 중: NVMe Write(FUA 포함) 명령이 SQ→CQ */
	REQ_FSEQ_POSTFLUSH	= (1 << 2), /* [한국어] post-flushing 진행 중: FUA 미지원 시 Write 후 추가 NVMe Flush */
	REQ_FSEQ_DONE		= (1 << 3), /* [한국어] 시퀀스 완료: 에러 발생 시에도 이 상태로 즉시 전이 */

	REQ_FSEQ_ACTIONS	= REQ_FSEQ_PREFLUSH | REQ_FSEQ_DATA |
				  REQ_FSEQ_POSTFLUSH, /* [한국어] 실제 동작이 필요한 단계 마스크: DONE은 제외 */

	/*
	 * If flush has been pending longer than the following timeout,
	 * it's issued even if flush_data requests are still in flight.
	 */
	/* [한국어] C3 조건: FUA 전용 요청 스트림에서 starvation 방지를 위해
	 * DATA in-flight 중에도 5*HZ 이상 대기 중인 PRE/POSTFLUSH는 강제 발행 */
	FLUSH_PENDING_TIMEOUT	= 5 * HZ, /* [한국어] 5 jiffies second: C2 지연 무시 임계값 */
};

/* [한국어] 전방 선언: blk_flush_complete_seq 내에서 blk_kick_flush를 호출하므로 필요 */
static void blk_kick_flush(struct request_queue *q,
			   struct blk_flush_queue *fq, blk_opf_t flags);

/*
 * [한국어]
 * blk_get_flush_queue - blk_mq_ctx에서 해당 hctx의 flush 상태머신을 가져온다
 *
 * @ctx: 요청이 제출된 소프트웨어 컨텍스트 (CPU ↔ hctx 매핑을 담음)
 * @return: ctx가 매핑된 hctx의 struct blk_flush_queue 포인터
 *
 * blk_mq_ctx는 per-CPU 소프트웨어 큐이고, blk_mq_map_queue(REQ_OP_FLUSH)
 * 로 해당 CPU의 FLUSH 전용 hctx(NVMe SQ/CQ 쌍)를 얻는다. flush 상태머신은
 * per-hctx로 분리되어 있어 서로 다른 NVMe SQ/CQ 쌍이 독립적으로 flush를
 * 관리할 수 있다. NULL을 반환하는 경우는 없다(hctx는 항상 fq를 갖는다).
 * 실행 컨텍스트: submit 경로(blk_insert_flush)와 CQ 완료 경로(flush_end_io)
 * 양쪽에서 호출된다.
 *
 * 호출 체인:
 *   blk_insert_flush / flush_end_io / mq_flush_data_end_io
 *     → [blk_get_flush_queue] → blk_mq_map_queue
 */
static inline struct blk_flush_queue *
blk_get_flush_queue(struct blk_mq_ctx *ctx)
{
	return blk_mq_map_queue(REQ_OP_FLUSH, ctx)->fq; /* [한국어] REQ_OP_FLUSH로 매핑된 hctx를 얻어 fq 반환: NVMe SQ/CQ 쌍 단위로 flush 상태 분리 */
}

/*
 * [한국어]
 * blk_flush_cur_seq - rq->flush.seq에서 현재 수행해야 할 시퀀스 단계를 반환
 *
 * @rq: flush 시퀀스가 진행 중인 요청
 * @return: 아직 완료되지 않은 최하위 REQ_FSEQ_* 비트값 (1<<ffz(seq))
 *
 * rq->flush.seq의 최하위 0비트 위치를 ffz()로 찾아 1<<pos 를 반환한다.
 * 이미 완료된 단계는 seq에 1이 세팅되고, 다음 단계는 seq의 최하위 0비트다.
 * 예: seq=0b000 → PREFLUSH(bit0), seq=0b001 → DATA(bit1),
 *    seq=0b011 → POSTFLUSH(bit2), seq=0b111 → DONE(bit3).
 *
 * 호출 체인:
 *   blk_flush_complete_seq / flush_end_io
 *     → [blk_flush_cur_seq] → ffz
 *
 * NVMe 연결: seq 비트가 0인 최하위 단계가 아직 완료되지 않은 단계.
 * 예: PREFLUSH가 끝나면 DATA, DATA가 끝나면 POSTFLUSH 순으로 진행.
 */
static unsigned int blk_flush_cur_seq(struct request *rq)
{
	return 1 << ffz(rq->flush.seq); /* [한국어] ffz(rq->flush.seq): 아직 완료되지 않은 최하위 단계의 비트 번호 → PREFLUSH/DATA/POSTFLUSH/DONE 중 현재 단계 결정 */
}

/*
 * [한국어]
 * blk_flush_restore_request - flush 시퀀스가 끝난 request를 일반 형태로 복원
 *
 * @rq: DONE 단계에 도달한 flush 시퀀스 request
 *
 * DATA 완료 후 최종 완료를 위해 request를 일반 요청 형태로 복원한다.
 * RQF_FLUSH_SEQ 플래그를 제거하고, mq_flush_data_end_io가 교체했던
 * end_io를 saved_end_io에서 복원한다. 또한 DATA 경로에서 NULL이 됐던
 * rq->bio를 rq->biotail에서 복원하여 blk_mq_end_request가 bio submitter에게
 * 최종 완료를 알릴 수 있게 한다.
 *
 * 호출 체인:
 *   flush_end_io → blk_flush_complete_seq(REQ_FSEQ_DONE)
 *     → [blk_flush_restore_request] → blk_mq_end_request
 */
static void blk_flush_restore_request(struct request *rq)
{
	/*
	 * After flush data completion, @rq->bio is %NULL but we need to
	 * complete the bio again.  @rq->biotail is guaranteed to equal the
	 * original @rq->bio.  Restore it.
	 */
	rq->bio = rq->biotail;		/* [한국어] NVMe FUA Write 완료 후 bio 재설정: 상위 bio submitter에게 최종 완료를 알릴 수 있도록 */
	if (rq->bio)
		rq->__sector = rq->bio->bi_iter.bi_sector; /* [한국어] bio의 시작 sector 복원: 상위 계층의 sector 정보를 request와 동기화 */

	/* make @rq a normal request */
	rq->rq_flags &= ~RQF_FLUSH_SEQ; /* [한국어] flush 시퀀스 플래그 해제: req_bio_endio에서 일반 완료 경로로 처리되도록 */
	rq->end_io = rq->flush.saved_end_io; /* [한국어] 원래 completion handler 복원: blk_rq_init_flush에서 교체했던 mq_flush_data_end_io를 원복 */
}

/*
 * [한국어]
 * blk_account_io_flush - NVMe Flush 명령 완료 통계 기록
 *
 * @rq: 방금 완료된 flush_rq (NVMe Flush 명령에 사용된 request)
 *
 * flush_end_io에서 호출되어 NVMe Flush 명령의 완료 횟수(ios[STAT_FLUSH])와
 * 소요 시간(nsecs[STAT_FLUSH])을 part_stat에 기록한다. part_stat_lock/unlock
 * 으로 per-CPU 통계를 직렬화한다.
 *
 * 호출 체인:
 *   flush_end_io → [blk_account_io_flush] → part_stat_{lock,inc,add,unlock}
 */
static void blk_account_io_flush(struct request *rq)
{
	struct block_device *part = rq->q->disk->part0; /* [한국어] 디스크 파티션 0(전체 디스크) 통계 객체 */

	part_stat_lock(); /* [한국어] per-CPU 통계 직렬화 시작 */
	part_stat_inc(part, ios[STAT_FLUSH]); /* [한국어] NVMe Flush 명령 완료 횟수 증가: /sys/block/<dev>/stat의 flush 항목에 반영 */
	part_stat_add(part, nsecs[STAT_FLUSH],
		      blk_time_get_ns() - rq->start_time_ns); /* [한국어] NVMe Flush 완료 지연 누적: blk_kick_flush 발행 시각(start_time_ns)부터 CQ 완료까지 */
	part_stat_unlock(); /* [한국어] per-CPU 통계 직렬화 종료 */
}

/**
 * [한국어]
 * blk_flush_complete_seq - flush 시퀀스의 한 단계 완료를 기록하고 다음 단계로 전이
 *
 * @rq: PREFLUSH/FUA 요청 (flush 시퀀스가 진행 중인 request)
 * @fq: 요청이 속한 hctx의 flush 상태머신
 * @seq: 방금 완료된 단계의 REQ_FSEQ_* 비트마스크 (0이면 진입 후 현재 단계 계산)
 * @error: NVMe 명령 완료 에러 상태 (BLK_STS_OK이면 정상)
 *
 * rq가 seq 단계를 마쳤음을 rq->flush.seq에 기록하고, 다음에 수행해야 할
 * 단계로 전이시킨다. 에러가 있으면 남은 단계를 건너뛰고 즉시 DONE으로 간다.
 * PRE/POSTFLUSH 단계는 pending 대기열에 넣어 blk_kick_flush로 묶어 발행하고,
 * DATA 단계는 requeue_list로 옮겨 nvme_queue_rq에서 NVMe Write/FUA로 처리한다.
 * fq->mq_flush_lock을 보유한 상태에서 호출되어야 한다(IRQ disabled).
 *
 * 호출 체인:
 *   blk_insert_flush / flush_end_io / mq_flush_data_end_io
 *     → [blk_flush_complete_seq] → blk_kick_flush / blk_mq_end_request
 */
static void blk_flush_complete_seq(struct request *rq,
				   struct blk_flush_queue *fq,
				   unsigned int seq, blk_status_t error)
{
	struct request_queue *q = rq->q; /* [한국어] flush_rq가 속한 request_queue: requeue_list와 flush_list에 접근하기 위해 */
	struct list_head *pending = &fq->flush_queue[fq->flush_pending_idx]; /* [한국어] PRE/POSTFLUSH 대기 중인 요청 리스트: 다음 NVMe Flush 명령에 묶일 후보들 */
	blk_opf_t cmd_flags; /* [한국어] 원 요청의 cmd_flags 임시 저장용: blk_kick_flush에 전달할 REQ_DRV/FAILFAST 플래그 */

	BUG_ON(rq->flush.seq & seq); /* [한국어] 동일 단계 중복 완료 방지: state machine 버그 시 즉시 커널 패닉 */
	rq->flush.seq |= seq; /* [한국어] 완료한 단계의 비트를 seq에 기록: 다음 blk_flush_cur_seq() 호출 시 다음 단계 결정 */
	cmd_flags = rq->cmd_flags; /* [한국어] cmd_flags 저장: 이 함수 내에서 request 상태가 바뀐 후에도 플래그가 필요하므로 복사 */

	if (likely(!error))
		seq = blk_flush_cur_seq(rq); /* [한국어] 에러 없으면 다음 단계 계산: PREFLUSH→DATA→POSTFLUSH→DONE 중 현재 미완료 최하위 단계 */
	else
		seq = REQ_FSEQ_DONE; /* [한국어] 에러 발생 시 즉시 DONE: 남은 NVMe Flush/Write 단계를 모두 건너뜀 */

	switch (seq) {
	case REQ_FSEQ_PREFLUSH:
	case REQ_FSEQ_POSTFLUSH:
		/* queue for flush */
		if (list_empty(pending))
			fq->flush_pending_since = jiffies; /* [한국어] 대기열이 비어있을 때 현재 시각 기록: C3 기아 방지 timeout 판정 시작점 */
		list_add_tail(&rq->queuelist, pending); /* [한국어] PRE/POSTFLUSH 요청을 pending 리스트에 추가: blk_kick_flush에서 한 번에 NVMe Flush 명령으로 발행 */
		break;

	case REQ_FSEQ_DATA:
		fq->flush_data_in_flight++; /* [한국어] NVMe Write(FUA) 발행 예정이므로 in-flight DATA 카운트 증가: C2에서 POSTFLUSH 지연 여부 판단 */
		spin_lock(&q->requeue_lock); /* [한국어] requeue_list 보호: NVMe dispatch 경로(blk_mq_run_hw_queue)와 경쟁 */
		list_move(&rq->queuelist, &q->requeue_list); /* [한국어] flush 시퀀스 DATA 단계를 requeue_list로 이동: nvme_queue_rq에서 NVMe Write/FUA로 처리 */
		spin_unlock(&q->requeue_lock);
		blk_mq_kick_requeue_list(q); /* [한국어] hardware queue 깨우기: hctx->run_work 예약 → nvme_queue_rq → doorbell 경로 */
		break;

	case REQ_FSEQ_DONE:
		/*
		 * @rq was previously adjusted by blk_insert_flush() for
		 * flush sequencing and may already have gone through the
		 * flush data request completion path.  Restore @rq for
		 * normal completion and end it.
		 */
		list_del_init(&rq->queuelist); /* [한국어] flush 상태머신 리스트에서 제거: 시퀀스 종료이므로 더 이상 flush 경로에서 관리되지 않음 */
		blk_flush_restore_request(rq); /* [한국어] request를 일반 완료 형태로 복원: RQF_FLUSH_SEQ 해제, bio/sector/end_io 복원 */
		blk_mq_end_request(rq, error); /* [한국어] bio submitter에게 최종 완료 보고: NVMe status(blk_status_t) 상위 전파, 완료 통계 기록 */
		break;

	default:
		BUG(); /* [한국어] 알 수 없는 시퀀스 단계: flush state machine 버그, 즉시 패닉 */
	}

	/*
	 * PRE/POSTFLUSH 대기열이 꽉 차거나 조건이 맞으면 실제 NVMe Flush
	 * 명령을 발행하도록 시도한다.
	 */
	blk_kick_flush(q, fq, cmd_flags); /* [한국어] 조건 충족 시 NVMe Flush 명령 발행: pending 대기열의 요청들을 하나의 flush_rq로 묶어 doorbell 경로로 전달 */
}

/*
 * [한국어]
 * flush_end_io - NVMe Flush 명령(REQ_OP_FLUSH) CQ 완료 핸들러
 *
 * @flush_rq: NVMe Flush 명령에 사용된 flush_rq (blk_kick_flush가 발행한 것)
 * @error: NVMe CQ에서 얻은 완료 상태 (BLK_STS_OK이면 정상)
 * @iob: completion batch (현재 이 함수에서는 미사용)
 * @return: RQ_END_IO_NONE (flush_rq를 free하지 않고 재활용하므로)
 *
 * NVMe Flush 명령이 CQ를 통해 완료될 때 호출된다. 이 함수가 하는 일:
 * 1. flush_rq의 마지막 참조자인지 refcount로 확인 (timeout 경로와 경쟁)
 * 2. NVMe Flush 완료 통계 기록 (blk_account_io_flush)
 * 3. flush_rq를 IDLE 상태로 전환하고 tag/internal_tag를 반납
 * 4. running 대기열에 묶인 PRE/POSTFLUSH 요청을 순회해 다음 단계로 전이
 * 실행 컨텍스트: NVMe CQ 인터럽트/polling 완료 경로.
 * fq->mq_flush_lock을 IRQ disabled로 보유하므로 blk_insert_flush와 직렬화.
 *
 * 호출 체인:
 *   nvme_poll_cq → nvme_process_cq → nvme_complete_rq
 *     → blk_mq_complete_request → [flush_end_io]
 *     → blk_flush_complete_seq → (다음 단계)
 */
static enum rq_end_io_ret flush_end_io(struct request *flush_rq,
				       blk_status_t error,
				       const struct io_comp_batch *iob)
{
	struct request_queue *q = flush_rq->q; /* [한국어] flush_rq가 속한 request_queue: tag 반납, elevator 여부 확인에 사용 */
	struct list_head *running; /* [한국어] 방금 완료된 NVMe Flush에 묶인 요청 리스트 */
	struct request *rq, *n; /* [한국어] list_for_each_entry_safe 순회용: safe 버전은 순회 중 list_del 허용 */
	unsigned long flags = 0; /* [한국어] spin_lock_irqsave 플래그: IRQ 상태 저장용 */
	struct blk_flush_queue *fq = blk_get_flush_queue(flush_rq->mq_ctx); /* [한국어] flush_rq의 mq_ctx에서 per-hctx flush 상태머신 획득 */

	/* release the tag's ownership to the req cloned from */
	spin_lock_irqsave(&fq->mq_flush_lock, flags); /* [한국어] per-hctx flush 상태 보호: NVMe CQ 완료와 blk_insert_flush 간 경쟁 직렬화 */

	if (!req_ref_put_and_test(flush_rq)) {
		/* [한국어] flush_rq의 마지막 참조자가 아님: timeout 경로에서도 호출되므로 use-after-free 방지 */
		fq->rq_status = error; /* [한국어] 보류 에러 저장: 마지막 참조 해제 시 이 status를 사용해 NVMe 에러 전파 */
		spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
		return RQ_END_IO_NONE;
	}

	blk_account_io_flush(flush_rq); /* [한국어] NVMe Flush 명령 완료 통계 기록: ios[STAT_FLUSH] + nsecs[STAT_FLUSH] 갱신 */
	/*
	 * Flush request has to be marked as IDLE when it is really ended
	 * because its .end_io() is called from timeout code path too for
	 * avoiding use-after-free.
	 */
	WRITE_ONCE(flush_rq->state, MQ_RQ_IDLE); /* [한국어] WRITE_ONCE: NVMe timeout 경로가 state를 보고 flush_rq가 재활용 가능함을 인지하도록 */
	if (fq->rq_status != BLK_STS_OK) {
		/* [한국어] 이전에 보류된 에러가 있으면: refcount 경쟁에서 먼저 도착한 에러를 최종 반영 */
		error = fq->rq_status; /* [한국어] 보류 에러를 최종 error로 채택: NVMe flush 결과를 대기 요청들에게 전파 */
		fq->rq_status = BLK_STS_OK; /* [한국어] 보류 상태 초기화: 다음 NVMe Flush 명령 사이클을 위해 리셋 */
	}

	if (!q->elevator) {
		flush_rq->tag = BLK_MQ_NO_TAG; /* [한국어] non-scheduler 모드: blk_kick_flush에서 first_rq에서 빌린 tag 반납, sbitmap 슬롯 재사용 가능 */
	} else {
		blk_mq_put_driver_tag(flush_rq); /* [한국어] scheduler 모드: hardware driver tag(NVMe SQ slot) 반납 → 다음 NVMe 명령에서 slot 재활용 */
		flush_rq->internal_tag = BLK_MQ_NO_TAG; /* [한국어] scheduler internal tag 반납: scheduler tag bitmap 해제 */
	}

	running = &fq->flush_queue[fq->flush_running_idx]; /* [한국어] flush_running_idx 버퍼: 방금 완료된 NVMe Flush에 묶인 PRE/POSTFLUSH 요청 리스트 */
	BUG_ON(fq->flush_pending_idx == fq->flush_running_idx); /* [한국어] 양쪽 idx가 같으면 NVMe Flush in-flight 상태 위반: C1 조건 파괴, 즉시 패닉 */

	/* account completion of the flush request */
	fq->flush_running_idx ^= 1; /* [한국어] running_idx 토글: 완료된 NVMe Flush 버퍼를 해제하고, 다음 대기 요청이 들어올 빈 슬롯이 됨 */

	/* and push the waiting requests to the next stage */
	list_for_each_entry_safe(rq, n, running, queuelist) {
		/* [한국어] 완료된 NVMe Flush에 묶인 모든 PRE/POSTFLUSH 요청을 다음 단계로 전이 */
		unsigned int seq = blk_flush_cur_seq(rq); /* [한국어] 각 요청의 다음 단계: PREFLUSH 완료→DATA, POSTFLUSH 완료→DONE */

		BUG_ON(seq != REQ_FSEQ_PREFLUSH && seq != REQ_FSEQ_POSTFLUSH); /* [한국어] running 리스트에는 PRE/POSTFLUSH 요청만 있어야 함: 다른 단계면 state machine 버그 */
		list_del_init(&rq->queuelist); /* [한국어] running 리스트에서 제거: 이후 blk_flush_complete_seq에서 다음 리스트/경로로 이동 */
		blk_flush_complete_seq(rq, fq, seq, error); /* [한국어] PREFLUSH완료→DATA(NVMe Write/FUA), POSTFLUSH완료→DONE(최종 완료 보고) */
	}

	spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
	return RQ_END_IO_NONE; /* [한국어] flush_rq는 free 하지 않고 blk_flush_queue에서 재활용: blk_kick_flush가 다음 NVMe Flush에 같은 rq를 사용 */
}

/*
 * [한국어]
 * is_flush_rq - 주어진 request가 blk_flush_queue의 flush_rq인지 판별
 *
 * @rq: 판별 대상 request
 * @return: true이면 NVMe Flush 명령용 flush_rq, false이면 일반 request
 *
 * rq->end_io가 flush_end_io를 가리키면 blk_kick_flush가 발행한 flush_rq다.
 * NVMe 드라이버(nvme_queue_rq)는 flush_rq를 일반 데이터 경로와 동일하게
 * 처리하지만, 완료 시 flush_end_io가 호출되어 상태머신 전이가 일어난다.
 *
 * 호출 체인:
 *   blk_mq_rq_ctx_init / timeout 경로 → [is_flush_rq]
 */
bool is_flush_rq(struct request *rq)
{
	return rq->end_io == flush_end_io; /* [한국어] end_io가 flush_end_io를 가리키면 NVMe Flush 명령용 flush_rq: CQ 완료 시 상태머신으로 재진입 */
}

/*
 * [한국어]
 * blk_kick_flush - 조건 충족 시 NVMe Flush 명령 발행
 *
 * @q: 대상 request_queue (flush_list, requeue_list 접근용)
 * @fq: 대상 hctx의 flush 상태머신
 * @flags: 원본 request의 cmd_flags (REQ_DRV, REQ_FAILFAST_* 전파용)
 *
 * fq->flush_queue[flush_pending_idx]에 쌓인 PRE/POSTFLUSH 요청들을 하나의
 * NVMe Flush 명령으로 묶어 발행한다. 발행 여부는 C1/C2/C3 조건으로 결정:
 * C1: 이미 NVMe Flush in-flight이면 skip (pending_idx == running_idx)
 * C2: DATA(FUA Write) in-flight이 있으면 POSTFLUSH 지연
 * C3: FLUSH_PENDING_TIMEOUT 초과 대기 중이면 C2 무시하고 강제 발행
 * 발행 시 first_rq의 mq_ctx/mq_hctx/tag를 flush_rq에 복사해 NVMe SQ 경로를
 * 빌린다. fq->mq_flush_lock을 보유한 상태에서 호출되어야 한다.
 *
 * 호출 체인:
 *   blk_flush_complete_seq → [blk_kick_flush]
 *     → blk_mq_kick_requeue_list → nvme_queue_rq → doorbell
 */
static void blk_kick_flush(struct request_queue *q, struct blk_flush_queue *fq,
			   blk_opf_t flags)
{
	struct list_head *pending = &fq->flush_queue[fq->flush_pending_idx]; /* [한국어] 현재 NVMe Flush 명령 발행 후보 대기열: pending_idx 버퍼 */
	struct request *first_rq =
		list_first_entry(pending, struct request, queuelist); /* [한국어] 대기열의 첫 번째 요청: flush_rq에 빌려줄 mq_hctx/mq_ctx/tag 제공자 */
	struct request *flush_rq = fq->flush_rq; /* [한국어] per-hctx NVMe Flush 명령 전용 request: blk_alloc_flush_queue에서 사전 할당된 재사용 rq */

	/* C1 described at the top of this file */
	if (fq->flush_pending_idx != fq->flush_running_idx || list_empty(pending))
		/* [한국어] C1: NVMe Flush in-flight(pending!=running) 이거나 대기열이 비어있으면 발행 금지 */
		return;

	/* C2 and C3 */
	if (fq->flush_data_in_flight &&
	    time_before(jiffies,
			fq->flush_pending_since + FLUSH_PENDING_TIMEOUT))
		/* [한국어] C2: FUA Write in-flight 있고 C3 timeout(5*HZ) 미초과 → POSTFLUSH 지연: 공유 PREFLUSH 최적화 */
		return;

	fq->flush_pending_idx ^= 1; /* [한국어] pending_idx 토글: 이제 pending_idx != running_idx → NVMe Flush in-flight 표시, 반대편이 새 대기열 */

	blk_rq_init(q, flush_rq); /* [한국어] flush_rq 초기화: 이전 NVMe Flush 명령의 잔여 상태(tag, end_io 등) 완전히 제거 */

	/*
	 * In case of none scheduler, borrow tag from the first request
	 * since they can't be in flight at the same time. And acquire
	 * the tag's ownership for flush req.
	 *
	 * In case of IO scheduler, flush rq need to borrow scheduler tag
	 * just for cheating put/get driver tag.
	 */
	flush_rq->mq_ctx = first_rq->mq_ctx; /* [한국어] NVMe SQ/CQ에 연결될 소프트웨어 컨텍스트 복사: CPU→hctx 매핑 상속 */
	flush_rq->mq_hctx = first_rq->mq_hctx; /* [한국어] NVMe SQ/CQ 쌍(hctx) 상속: doorbell 주소와 CQ 인터럽트 벡터 결정 */

	if (!q->elevator)
		flush_rq->tag = first_rq->tag; /* [한국어] non-scheduler: first_rq의 NVMe SQ 슬롯(CID)을 flush_rq에 빌림. first_rq는 flush 완료까지 stall */
	else
		flush_rq->internal_tag = first_rq->internal_tag; /* [한국어] scheduler: first_rq의 scheduler internal_tag를 빌림. blk_mq_get_driver_tag에서 실제 NVMe 슬롯 획득 */

	flush_rq->cmd_flags = REQ_OP_FLUSH | REQ_PREFLUSH; /* [한국어] NVMe Flush 명령(Opcode 0x00)으로 설정: nvme_queue_rq에서 nvme_setup_flush()로 처리 */
	flush_rq->cmd_flags |= (flags & REQ_DRV) | (flags & REQ_FAILFAST_MASK); /* [한국어] 원 요청의 driver 플래그와 failfast 정책 상속: NVMe 타임아웃/중단 동작에 영향 */
	flush_rq->rq_flags |= RQF_FLUSH_SEQ; /* [한국어] flush 시퀀스 요청 표시: req_bio_endio에서 bio 최종 완료를 지연시키는 역할 */
	flush_rq->end_io = flush_end_io; /* [한국어] NVMe CQ 완료 시 flush_end_io 호출: nvme_complete_request → rq->end_io → flush_end_io */
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

	blk_mq_kick_requeue_list(q); /* [한국어] hardware queue 깨우기: flush_list에 추가한 flush_rq를 nvme_queue_rq → doorbell 경로로 전달 */
}

/*
 * [한국어]
 * mq_flush_data_end_io - NVMe Write/FUA DATA 단계 완료 핸들러
 *
 * @rq: NVMe Write(FUA) 명령이 완료된 request
 * @error: NVMe CQ 완료 상태 (BLK_STS_OK이면 정상)
 * @iob: completion batch (이 함수에서는 미사용)
 * @return: RQ_END_IO_NONE (request를 free하지 않음)
 *
 * blk_rq_init_flush에서 rq->end_io를 이 함수로 교체했으므로, FUA Write가
 * NVMe CQ를 통해 완료되면 이 함수가 호출된다. 처리 순서:
 * 1. scheduler 모드이면 NVMe driver tag 반납
 * 2. flush_data_in_flight 감소 (C2 조건 해제)
 * 3. blk_flush_complete_seq(REQ_FSEQ_DATA)로 POSTFLUSH/DONE 전이
 * 4. hctx 재시작 (blk_mq_sched_restart)
 * 실행 컨텍스트: NVMe CQ 인터럽트/polling 완료 경로.
 *
 * 호출 체인:
 *   nvme_poll_cq → nvme_process_cq → nvme_complete_rq
 *     → blk_mq_complete_request → [mq_flush_data_end_io]
 *     → blk_flush_complete_seq(DATA) → blk_kick_flush
 */
static enum rq_end_io_ret mq_flush_data_end_io(struct request *rq,
					       blk_status_t error,
					       const struct io_comp_batch *iob)
{
	struct request_queue *q = rq->q; /* [한국어] 요청이 속한 request_queue */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx; /* [한국어] NVMe SQ/CQ 쌍: DATA 완료 후 이 hctx를 재시작해 후속 NVMe 명령 허용 */
	struct blk_mq_ctx *ctx = rq->mq_ctx; /* [한국어] 요청이 제출된 per-CPU 소프트웨어 컨텍스트 */
	unsigned long flags; /* [한국어] spin_lock_irqsave 플래그 */
	struct blk_flush_queue *fq = blk_get_flush_queue(ctx); /* [한국어] hctx에서 per-hctx flush 상태머신 획득 */

	if (q->elevator) {
		WARN_ON(rq->tag < 0); /* [한국어] scheduler 모드에서 driver tag가 음수면 NVMe SQ slot 누수 경고 */
		blk_mq_put_driver_tag(rq); /* [한국어] NVMe hardware tag(SQ 슬롯) 반납: 완료된 Write의 CID를 sbitmap에 반환 */
	}

	/*
	 * After populating an empty queue, kick it to avoid stall.  Read
	 * the comment in flush_end_io().
	 */
	spin_lock_irqsave(&fq->mq_flush_lock, flags); /* [한국어] per-hctx flush 상태 보호: flush_end_io와의 경쟁 직렬화 */
	fq->flush_data_in_flight--; /* [한국어] NVMe FUA Write 완료: in-flight DATA 카운트 감소, C2 POSTFLUSH 지연 조건 재평가 */
	/*
	 * May have been corrupted by rq->rq_next reuse, we need to
	 * re-initialize rq->queuelist before reusing it here.
	 */
	INIT_LIST_HEAD(&rq->queuelist); /* [한국어] queuelist 재초기화: NVMe completion batch의 rq_next 재사용으로 list 포인터가 오염될 수 있으므로 */
	blk_flush_complete_seq(rq, fq, REQ_FSEQ_DATA, error); /* [한국어] DATA 단계 완료 기록: POSTFLUSH가 남았으면 pending 대기열, 없으면 DONE으로 전이 */
	spin_unlock_irqrestore(&fq->mq_flush_lock, flags);

	blk_mq_sched_restart(hctx); /* [한국어] NVMe hctx 재시작: DATA Write 완료로 비어난 SQ 슬롯에 후속 NVMe 명령 dispatch 허용 */
	return RQ_END_IO_NONE; /* [한국어] request를 free하지 않음: flush 시퀀스에서 계속 사용 */
}

/*
 * [한국어]
 * blk_rq_init_flush - request를 flush 시퀀스 상태머신에 등록
 *
 * @rq: flush 시퀀스에 참여시킬 request (DATA 또는 POSTFLUSH가 필요한 것)
 *
 * rq->end_io를 mq_flush_data_end_io로 교체하여 NVMe Write/FUA 완료 시
 * flush 상태머신으로 재진입하도록 준비한다. 원래 end_io는 saved_end_io에
 * 저장해 두어, DONE 단계에서 blk_flush_restore_request가 복원할 수 있게 한다.
 * RQF_FLUSH_SEQ 플래그를 설정해 req_bio_endio가 최종 완료를 지연시키도록 한다.
 *
 * 호출 체인:
 *   blk_insert_flush(REQ_FSEQ_DATA|POSTFLUSH 또는 default case)
 *     → [blk_rq_init_flush]
 */
static void blk_rq_init_flush(struct request *rq)
{
	rq->flush.seq = 0; /* [한국어] 시퀀스 비트 전부 초기화: 아무 단계도 완료되지 않은 상태에서 시작 */
	rq->rq_flags |= RQF_FLUSH_SEQ; /* [한국어] flush 시퀀스 요청 표시: req_bio_endio에서 DATA 완료 시 bio를 종료하지 않고 지연 */
	rq->flush.saved_end_io = rq->end_io; /* Usually NULL */ /* [한국어] 기존 end_io 저장 (보통 NULL): DONE 단계에서 blk_flush_restore_request가 복원 */
	rq->end_io = mq_flush_data_end_io; /* [한국어] NVMe Write(FUA) DATA 완료 핸들러로 교체: CQ 완료 → mq_flush_data_end_io → POSTFLUSH/DONE */
}

/*
 * [한국어]
 * blk_insert_flush - PREFLUSH/FUA 요청을 flush 시퀀스로 분해해 상태머신에 진입
 *
 * @rq: REQ_PREFLUSH 또는 REQ_FUA가 설정된 request
 * @return: true이면 flush 상태머신이 요청을 소비 (caller는 추가 처리 불필요),
 *          false이면 일반 NVMe Write/FUA 경로로 처리해야 함
 *
 * 요청의 REQ_PREFLUSH/REQ_FUA 플래그와 NVMe 컨트롤러의 VWC/FUA 지원 여부를
 * 조합해 실행해야 할 시퀀스(policy)를 결정한다:
 *   policy == 0                   : VWC 없음 → 즉시 완료
 *   policy == DATA                : 데이터만, flush 불필요 → 일반 경로
 *   policy == DATA | POSTFLUSH    : FUA 미지원 → DATA 후 NVMe Flush
 *   policy == PREFLUSH | DATA | … : VWC+PREFLUSH → NVMe Flush → DATA → (POSTFLUSH)
 * REQ_PREFLUSH 플래그는 시퀀스 결정 후 NVMe 드라이버에 전달되기 전에 제거된다.
 * 이 함수는 blk-mq의 request 할당 경로에서 호출되며, fq->mq_flush_lock을
 * 자체적으로 획득/해제한다.
 *
 * 호출 체인:
 *   submit_bio → blk_mq_submit_bio → blk_mq_get_request
 *     → [blk_insert_flush] → blk_flush_complete_seq / blk_mq_end_request
 */
/*
 * Insert a PREFLUSH/FUA request into the flush state machine.
 * Returns true if the request has been consumed by the flush state machine,
 * or false if the caller should continue to process it.
 */
bool blk_insert_flush(struct request *rq)
{
	struct request_queue *q = rq->q; /* [한국어] 요청이 속한 request_queue: q->limits에서 NVMe VWC/FUA 지원 여부 확인 */
	struct blk_flush_queue *fq = blk_get_flush_queue(rq->mq_ctx); /* [한국어] 요청이 매핑된 hctx의 flush 상태머신 */
	bool supports_fua = q->limits.features & BLK_FEAT_FUA; /* [한국어] NVMe 컨트롤러 FUA 지원 여부: BLK_FEAT_FUA는 nvme_id_ctrl.vwc 기반으로 설정 */
	unsigned int policy = 0; /* [한국어] 실행해야 할 시퀀스 단계 마스크: REQ_FSEQ_* 조합으로 결정 */

	/* FLUSH/FUA request must never be merged */
	WARN_ON_ONCE(rq->bio != rq->biotail); /* [한국어] bio가 정확히 하나임을 강제: merge 시 시퀀싱/bio 완료 로직이 깨짐 */

	if (blk_rq_sectors(rq))
		policy |= REQ_FSEQ_DATA; /* [한국어] 데이터 섹터가 있으면 DATA 단계 포함: NVMe Write(FUA 포함) 명령 필요 */

	/*
	 * Check which flushes we need to sequence for this operation.
	 */
	if (blk_queue_write_cache(q)) {
		/* [한국어] NVMe volatile write cache(VWC)가 있으면 Flush가 의미 있음: 없으면 Flush 발행 불필요 */
		if (rq->cmd_flags & REQ_PREFLUSH)
			policy |= REQ_FSEQ_PREFLUSH; /* [한국어] 쓰기 전 NVMe Flush 명령 필요: VWC의 dirty 데이터를 미디어에 배출 */
		if ((rq->cmd_flags & REQ_FUA) && !supports_fua)
			policy |= REQ_FSEQ_POSTFLUSH; /* [한국어] FUA 미지원: Write 후 NVMe Flush로 비휘발성 확인 (FUA 비트 대체) */
	}

	/*
	 * @policy now records what operations need to be done.  Adjust
	 * REQ_PREFLUSH and FUA for the driver.
	 */
	rq->cmd_flags &= ~REQ_PREFLUSH; /* [한국어] PREFLUSH는 시퀀스로 처리하므로 NVMe Write 명령에는 전달 안 함 */
	if (!supports_fua)
		rq->cmd_flags &= ~REQ_FUA; /* [한국어] FUA 미지원: Write에서 FUA 비트 제거, POSTFLUSH 시퀀스가 대신 처리 */

	/*
	 * REQ_PREFLUSH|REQ_FUA implies REQ_SYNC, so if we clear any
	 * of those flags, we have to set REQ_SYNC to avoid skewing
	 * the request accounting.
	 */
	rq->cmd_flags |= REQ_SYNC; /* [한국어] REQ_PREFLUSH/FUA 제거 보정: sync 요청으로 표시해 scheduler/stat 계산에서 누락 방지 */

	switch (policy) {
	case 0:
		/*
		 * An empty flush handed down from a stacking driver may
		 * translate into nothing if the underlying device does not
		 * advertise a write-back cache.  In this case, simply
		 * complete the request.
		 */
		blk_mq_end_request(rq, 0); /* [한국어] NVMe VWC 없음: Flush 불필요, 상위에 즉시 완료 보고 */
		return true; /* [한국어] flush 상태머신이 요청을 소비함: caller는 추가 dispatch 없음 */
	case REQ_FSEQ_DATA:
		/*
		 * If there's data, but no flush is necessary, the request can
		 * be processed directly without going through flush machinery.
		 * Queue for normal execution.
		 */
		return false; /* [한국어] 데이터만 있고 flush 불필요: 일반 NVMe Write/FUA Write 경로로 dispatch */
	case REQ_FSEQ_DATA | REQ_FSEQ_POSTFLUSH:
		/*
		 * Initialize the flush fields and completion handler to trigger
		 * the post flush, and then just pass the command on.
		 */
		blk_rq_init_flush(rq); /* [한국어] flush 시퀀스 등록: DATA 완료 후 mq_flush_data_end_io → POSTFLUSH */
		rq->flush.seq |= REQ_FSEQ_PREFLUSH; /* [한국어] PREFLUSH 단계가 없는 경우 완료 마킹: blk_flush_cur_seq가 DATA를 첫 단계로 반환하도록 */
		spin_lock_irq(&fq->mq_flush_lock);
		fq->flush_data_in_flight++; /* [한국어] NVMe Write 발행 전 in-flight 선증가: C2 조건(POSTFLUSH 지연)이 즉시 적용되도록 */
		spin_unlock_irq(&fq->mq_flush_lock);
		return false; /* [한국어] NVMe Write를 일반 경로로 dispatch: 완료 후 mq_flush_data_end_io에서 POSTFLUSH 전이 */
	default:
		/*
		 * Mark the request as part of a flush sequence and submit it
		 * for further processing to the flush state machine.
		 */
		blk_rq_init_flush(rq); /* [한국어] flush 시퀀스 등록: PREFLUSH → DATA → POSTFLUSH 전체 경로 */
		spin_lock_irq(&fq->mq_flush_lock);
		/* [한국어] 실행하지 않는 단계(~policy)를 이미 완료한 것처럼 마킹 후 현재 첫 단계로 전이 */
		blk_flush_complete_seq(rq, fq, REQ_FSEQ_ACTIONS & ~policy, 0);
		spin_unlock_irq(&fq->mq_flush_lock);
		return true; /* [한국어] flush 상태머신이 요청을 소비함: PREFLUSH부터 시작 */
	}
}

/*
 * [한국어]
 * blkdev_issue_flush - 상위 계층/사용자공간의 명시적 flush 요청 처리
 *
 * @bdev: flush를 발행할 블록 디바이스
 * @return: 0이면 성공, 음수이면 에러 (NVMe CQ 에러 코드 변환)
 *
 * fsync, fdatasync, sync 등 상위 계층에서 명시적 flush가 필요할 때 호출된다.
 * 내부적으로 REQ_OP_WRITE | REQ_PREFLUSH bio를 생성해 submit_bio_wait로
 * 동기 대기한다. blk_insert_flush에서 이 bio가 NVMe Flush 명령으로 변환되어
 * NVMe CQ 완료까지 caller가 블록된다.
 *
 * 호출 체인:
 *   fsync/sync → [blkdev_issue_flush]
 *     → submit_bio_wait → blk_insert_flush → NVMe Flush → CQ 완료 → 반환
 */
int blkdev_issue_flush(struct block_device *bdev)
{
	struct bio bio; /* [한국어] 스택에 할당된 bio: NVMe Flush 명령용, 완료 시 자동 해제 */

	bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH); /* [한국어] 데이터 없는 PREFLUSH bio 생성: blk_insert_flush에서 NVMe Flush 명령으로 변환됨 */
	return submit_bio_wait(&bio); /* [한국어] bio 제출 후 CQ 완료까지 동기 대기: → blk_insert_flush → NVMe Flush → flush_end_io → 반환 */
}
EXPORT_SYMBOL(blkdev_issue_flush);

/*
 * [한국어]
 * blk_alloc_flush_queue - per-hctx flush 상태머신 자료구조 할당·초기화
 *
 * @node: NUMA 노드 번호 (hctx와 같은 노드에 할당)
 * @cmd_size: NVMe 드라이버의 driver-private 명령 크기 (flush_rq에 포함)
 * @flags: kzalloc_node에 전달할 GFP 플래그
 * @return: 초기화된 blk_flush_queue 포인터, 실패 시 NULL
 *
 * 각 NVMe SQ/CQ 쌍(hctx)마다 독립적인 flush 상태머신이 필요하므로
 * per-hctx로 할당된다. flush_rq는 NVMe Flush 명령용으로 재활용되는
 * request로, cmd_size만큼의 드라이버 사설 공간을 포함한다.
 * flush_queue[0/1]은 더블 버퍼링 PRE/POSTFLUSH 대기열이다.
 *
 * 호출 체인:
 *   blk_mq_alloc_hctx → [blk_alloc_flush_queue]
 */
struct blk_flush_queue *blk_alloc_flush_queue(int node, int cmd_size,
					      gfp_t flags)
{
	struct blk_flush_queue *fq; /* [한국어] 할당할 per-hctx flush 상태머신 */
	int rq_sz = sizeof(struct request); /* [한국어] flush_rq 기본 크기: driver cmd 공간 추가 전 */

	fq = kzalloc_node(sizeof(*fq), flags, node); /* [한국어] per-hctx flush 상태머신 구조체 할당: NUMA 노드 친화적 */
	if (!fq)
		goto fail; /* [한국어] 메모리 부족: fq 할당 실패 */

	spin_lock_init(&fq->mq_flush_lock); /* [한국어] per-hctx flush lock 초기화: NVMe CQ 완료/timeout/blk_insert_flush 간 직렬화 */

	rq_sz = round_up(rq_sz + cmd_size, cache_line_size()); /* [한국어] request + driver cmd_size를 캐시 라인 단위로 정렬: false sharing 방지 */
	fq->flush_rq = kzalloc_node(rq_sz, flags, node); /* [한국어] NVMe Flush 명령 재활용 request 할당: blk_kick_flush가 이 rq를 반복 사용 */
	if (!fq->flush_rq)
		goto fail_rq; /* [한국어] flush_rq 할당 실패: fq 해제 후 NULL 반환 */

	INIT_LIST_HEAD(&fq->flush_queue[0]); /* [한국어] flush 더블 버퍼 0 초기화: 첫 번째 PRE/POSTFLUSH 대기열 */
	INIT_LIST_HEAD(&fq->flush_queue[1]); /* [한국어] flush 더블 버퍼 1 초기화: 두 번째 PRE/POSTFLUSH 대기열 (ping-pong) */

	return fq;

 fail_rq:
	kfree(fq); /* [한국어] flush_rq 할당 실패 시 fq 해제 */
 fail:
	return NULL; /* [한국어] 할당 실패: caller(blk_mq_alloc_hctx)가 에러 처리 */
}

/*
 * [한국어]
 * blk_free_flush_queue - per-hctx flush 상태머신 자료구조 해제
 *
 * @fq: 해제할 blk_flush_queue (NULL이면 아무것도 하지 않음)
 *
 * blk_alloc_flush_queue의 역함수. bio-based queue에는 flush queue가 없으므로
 * NULL 체크가 필수다. flush_rq를 먼저 해제한 뒤 fq를 해제한다.
 *
 * 호출 체인:
 *   blk_mq_free_hctx → [blk_free_flush_queue]
 */
void blk_free_flush_queue(struct blk_flush_queue *fq)
{
	/* bio based request queue hasn't flush queue */
	if (!fq)
		return; /* [한국어] bio-only queue에는 flush 상태머신이 없으므로 skip */

	kfree(fq->flush_rq); /* [한국어] NVMe Flush 명령 재활용 request 해제 */
	kfree(fq); /* [한국어] per-hctx flush 상태머신 구조체 해제 */
}

/*
 * [한국어]
 * blk_mq_hctx_set_fq_lock_class - per-hctx flush lock에 별도 lockdep class 지정
 *
 * @hctx: 대상 하드웨어 큐 컨텍스트
 * @key: 드라이버가 제공하는 lockdep lock class key
 *
 * nvme-loop 같은 드라이버에서 flush_end_io가 재귀 호출될 수 있어 lockdep이
 * "possible recursive locking"을 경고한다. 모든 blk_flush_queue 인스턴스가
 * 같은 mq_flush_lock class를 공유하기 때문이다. 이 함수로 per-hctx 별도 class
 * 를 지정하면 false positive를 피할 수 있다. 동적 per-fq key 할당은 SCSI 프로브
 * 에서 synchronize_rcu 대기로 30분 이상 지연이 발생하므로 정적 key를 사용한다.
 *
 * 호출 체인:
 *   nvme_pci_init_hctx / nvme_loop_init_hctx
 *     → [blk_mq_hctx_set_fq_lock_class]
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
	lockdep_set_class(&hctx->fq->mq_flush_lock, key); /* [한국어] per-hctx flush lock에 별도 lockdep class 지정: nvme-loop 재귀 완료 경로의 false positive 억제 */
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
