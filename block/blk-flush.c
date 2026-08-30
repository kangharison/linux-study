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
 * === 이 상태 기계를 켜고 끄는 스위치: NVMe VWC (실물 확인) ===
 * 이 파일 전체가 하는 일은 결국 두 개의 큐 기능 비트에 좌우된다:
 *   BLK_FEAT_WRITE_CACHE — 장치에 휘발성 쓰기 캐시가 있는가
 *   BLK_FEAT_FUA         — 장치가 쓰기 명령 하나로 내구성을 보장할 수 있는가
 * NVMe 는 이 두 비트를 다음 코드로 한꺼번에 정한다(core.c):
 *
 *   ctrl->vwc = id->vwc;                       // Identify Controller 의 VWC 필드
 *   info->no_vwc = id->nsfeat & NVME_NS_VWC_NOT_PRESENT;   // 네임스페이스별 무효화
 *   if ((ns->ctrl->vwc & NVME_CTRL_VWC_PRESENT) && !info->no_vwc)
 *           lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA;
 *   else
 *           lim.features &= ~(BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA);
 *
 * 여기서 읽어야 할 중요한 사실이 둘 있다.
 * (1) NVMe 에서 WRITE_CACHE 와 FUA 는 **함께 켜지고 함께 꺼진다.** 따라서
 *     blk_insert_flush() 의 `(REQ_FUA && !supports_fua) → POSTFLUSH` 분기는
 *     NVMe 에서는 사실상 발생하지 않는다. 그 분기는 FUA 없이 캐시만 있는
 *     장치(일부 SCSI/MMC 등)를 위한 것이다.
 * (2) 전력 손실 보호(PLP)를 갖춘 엔터프라이즈 SSD 처럼 VWC 를 광고하지 않는
 *     장치에서는 blk_queue_write_cache()가 거짓이 되어 policy 가 0 이 되고,
 *     Flush 요청이 **명령을 하나도 발행하지 않고** blk_mq_end_request(rq, 0)
 *     으로 즉시 끝난다. fsync 비용이 장치에 따라 극단적으로 달라지는 이유가
 *     바로 이 지점이다.
 *
 * 실제 발행되는 명령:
 *   PREFLUSH/POSTFLUSH → nvme_setup_flush(): opcode = nvme_cmd_flush (0x00),
 *                        nsid 만 채우고 나머지 SQE 는 0. 데이터 전송 없음.
 *   DATA(FUA)          → nvme_setup_rw(): `if (req->cmd_flags & REQ_FUA)
 *                        control |= NVME_RW_FUA;` — 별도 명령이 아니라
 *                        Write 명령의 control 필드 비트다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * blk-mq 의 요청 삽입 경로 안에 끼어 있다. 주의: 이 파일은 드라이버를
 * 직접 부르지 않는다. 아래 화살표 중 드라이버로 넘어가는 단계는 전부
 * mq_ops->queue_rq 함수 포인터를 통한 간접 호출이다.
 *   submit_bio
 *     → blk_mq_submit_bio (block/blk-mq.c)
 *       → blk_insert_flush (이 파일: REQ_PREFLUSH/FUA 시퀀스 결정)
 *         → blk_kick_flush (이 파일: flush_rq 를 하나 만들어 대기열에 넣음)
 *           → blk_mq_run_hw_queue / blk_mq_kick_requeue_list
 *             → blk_mq_dispatch_rq_list → mq_ops->queue_rq
 *                                         (NVMe PCIe 라면 nvme_queue_rq)
 *   완료: 장치 완료 → blk_mq_end_request → rq->end_io = flush_end_io
 *         (이 파일: 다음 단계 전이)
 * 실행 컨텍스트: 제출 경로(프로세스 컨텍스트)와 완료 경로(인터럽트에서
 * 시작해 IPI/softirq 로 넘어갈 수 있음) 양쪽에서 진입한다. 그래서
 * fq->mq_flush_lock 은 반드시 irqsave 계열로 잡는다.
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
 *     → blk_kick_flush → flush_rq → 디스패치 → 완료
 *     → flush_end_io → blk_flush_complete_seq (다음 단계)
 *     → blk_mq_end_request (bio 최종 완료)
 *
 * === 주요 함수/구조체 요약 ===
 * blk_insert_flush()        : PREFLUSH/FUA 요청을 분석해 시퀀스 결정, 상태머신 진입
 * blk_kick_flush()          : 조건 충족 시 pending 대기열을 하나의 Flush 로 묶어 발행
 * blk_flush_complete_seq()  : 시퀀스 단계 완료 기록 + 다음 단계 전이
 * flush_end_io()            : Flush 요청 완료 시 불리는 end_io 콜백 — 여기서 다음 단계로 전이한다
 * mq_flush_data_end_io()    : DATA 단계 완료 시 POSTFLUSH 또는 DONE 으로 전이
 * blk_alloc_flush_queue()   : per-hctx flush 상태머신 자료구조 할당·초기화
 * struct blk_flush_queue (blk.h):
 *   mq_flush_lock           : flush 상태머신 직렬화 spinlock (IRQ-safe)
 *   flush_queue[2]          : 더블 버퍼 PRE/POSTFLUSH 대기열
 *   flush_pending_idx       : 현재 대기 중인 버퍼 인덱스
 *   flush_running_idx       : 현재 발행되어 진행 중인 Flush 에 묶인 버퍼 인덱스
 *   flush_data_in_flight    : DATA(FUA Write) in-flight 요청 수 (C2 조건용)
 *   flush_rq                : 큐마다 하나씩 미리 잡아 두고 Flush 발행 때마다 재활용하는 전용 request
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
 *   - mq_flush_lock: 제출 경로와 완료 경로가 같은 상태 기계를 건드리므로 필요한 직렬화 (per-hctx)
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
	REQ_FSEQ_PREFLUSH	= (1 << 0), /* [한국어] pre-flushing 진행 중: Flush 명령이 진행 중 (NVMe 라면 opcode 0x00) */
	REQ_FSEQ_DATA		= (1 << 1), /* [한국어] data write 진행 중: 데이터 쓰기가 진행 중 (FUA 비트가 실렸을 수 있다) */
	REQ_FSEQ_POSTFLUSH	= (1 << 2), /* [한국어] post-flushing 진행 중: FUA 미지원 장치에서 Write 뒤에 덧붙이는 Flush. NVMe 는 WRITE_CACHE 와 FUA 가 함께 켜지므로 이 단계에 거의 오지 않는다 */
	REQ_FSEQ_DONE		= (1 << 3),
	/* [한국어] 시퀀스가 끝났다는 표시.
	 * 다른 셋과 성격이 다르다 — PREFLUSH/DATA/POSTFLUSH 는 "할 일"이지만 이것은
	 * "더 할 일이 없다"는 종착점이다. 에러가 나면 남은 단계를 건너뛰고 곧장
	 * 이 상태로 넘어가므로, 실패한 flush 가 중간 단계에 갇히는 일이 없다. */

	REQ_FSEQ_ACTIONS	= REQ_FSEQ_PREFLUSH | REQ_FSEQ_DATA |
				  REQ_FSEQ_POSTFLUSH,
	/* [한국어] "실제로 할 일이 있는" 세 단계만 묶은 마스크(DONE 제외).
	 * 쓰임새: blk_insert_flush() 가 `REQ_FSEQ_ACTIONS & ~policy` 로 넘겨
	 *   "이번에 하지 않을 단계는 이미 끝난 것으로 쳐라"를 한 번에 표현한다.
	 *   DONE 이 여기 포함되면 그 표현이 "시퀀스가 이미 끝났다"가 되어 버린다. */

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
 * 로 해당 CPU 에 매핑된 FLUSH 용 hctx 를 얻는다. flush 상태머신은
 * per-hctx로 분리되어 있어 서로 다른 하드웨어 큐가 독립적으로 flush 를
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
	return blk_mq_map_queue(REQ_OP_FLUSH, ctx)->fq; /* [한국어] REQ_OP_FLUSH로 매핑된 hctx를 얻어 fq 반환: 하드웨어 큐 단위로 flush 상태 분리 */
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
	rq->bio = rq->biotail;		/* [한국어] 완료 후 bio 포인터 재설정: 상위 bio submitter에게 최종 완료를 알릴 수 있도록 */
	if (rq->bio)
		rq->__sector = rq->bio->bi_iter.bi_sector; /* [한국어] bio의 시작 sector 복원: 상위 계층의 sector 정보를 request와 동기화 */

	/* make @rq a normal request */
	rq->rq_flags &= ~RQF_FLUSH_SEQ; /* [한국어] flush 시퀀스 플래그 해제: req_bio_endio에서 일반 완료 경로로 처리되도록 */
	rq->end_io = rq->flush.saved_end_io; /* [한국어] 원래 completion handler 복원: blk_rq_init_flush에서 교체했던 mq_flush_data_end_io를 원복 */
}

/*
 * [한국어]
 * blk_account_io_flush - Flush 완료 통계 기록
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
	part_stat_inc(part, ios[STAT_FLUSH]); /* [한국어] Flush 완료 횟수 증가: /sys/block/<dev>/stat의 flush 항목에 반영 */
	part_stat_add(part, nsecs[STAT_FLUSH],
		      blk_time_get_ns() - rq->start_time_ns); /* [한국어] Flush 지연 누적: blk_kick_flush 발행 시각(start_time_ns)부터 CQ 완료까지 */
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
 * DATA 단계는 requeue_list 로 옮겨 일반 쓰기와 같은 경로로 디스패치된다.
 * NVMe 라면 nvme_setup_rw() 가 REQ_FUA 를 Write 명령의 control 필드
 * NVME_RW_FUA 비트로 옮겨 담는다 — 별도 명령이 아니다.
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
		seq = REQ_FSEQ_DONE; /* [한국어] 에러 발생 시 즉시 DONE: 남은 단계를 모두 건너뛴다 */

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
		list_move(&rq->queuelist, &q->requeue_list); /* [한국어] flush 시퀀스의 DATA 단계를 requeue_list 로 이동 — 이제부터는 평범한 쓰기 요청과 같은 경로를 탄다 */
		spin_unlock(&q->requeue_lock);
		blk_mq_kick_requeue_list(q); /* [한국어] hardware queue 깨우기 — requeue_list 에 넣기만 해서는 아무도 가져가지 않으므로 디스패치를 명시적으로 예약한다 */
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
 * @error: 장치가 보고한 완료 상태 (BLK_STS_OK이면 정상)
 * @iob: completion batch (현재 이 함수에서는 미사용)
 * @return: RQ_END_IO_NONE (flush_rq를 free하지 않고 재활용하므로)
 *
 * NVMe Flush 명령이 CQ를 통해 완료될 때 호출된다. 이 함수가 하는 일:
 * 1. flush_rq의 마지막 참조자인지 refcount로 확인 (timeout 경로와 경쟁)
 * 2. NVMe Flush 완료 통계 기록 (blk_account_io_flush)
 * 3. flush_rq를 IDLE 상태로 전환하고 tag/internal_tag를 반납
 * 4. running 대기열에 묶인 PRE/POSTFLUSH 요청을 순회해 다음 단계로 전이
 * 실행 컨텍스트: 완료 경로. 장치 인터럽트에서 시작하지만 IPI/softirq 로 넘어갈 수 있어
 * 어느 쪽이든 잠들 수 없다고 보고 락을 irqsave 로 잡는다.
 * fq->mq_flush_lock을 IRQ disabled로 보유하므로 blk_insert_flush와 직렬화.
 *
 * 호출 체인:
 *   장치 완료 → blk_mq_end_request → rq->end_io
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
	spin_lock_irqsave(&fq->mq_flush_lock, flags); /* [한국어] per-hctx flush 상태 보호: 완료 경로와 제출 경로(blk_insert_flush) 간 경쟁 직렬화 */

	if (!req_ref_put_and_test(flush_rq)) {
		/* [한국어] flush_rq 는 큐당 하나뿐이고 재활용되기 때문에, 완료 경로와 타임아웃
		 * 경로가 **동시에** 이 함수에 들어올 수 있다. 둘 중 나중에 도착한 쪽만
		 * 실제 정리를 수행해야 한다 — 참조 카운트로 그 "마지막 한 명"을 가린다.
		 * 먼저 도착한 쪽은 자기가 본 에러만 fq->rq_status 에 남기고 물러난다.
		 * 이 검사를 빼면 두 경로가 같은 flush_rq 를 두 번 정리해 use-after-free 가 난다. */
		fq->rq_status = error; /* [한국어] 내가 본 에러를 남겨 둔다. 나는 물러나지만 이 에러는 잃으면 안 되고,
					 * 마지막에 도착하는 쪽이 아래에서 이 값을 집어 최종 결과로 삼는다.
					 * 원래 주석: 마지막 참조 해제 시 이 status를 사용해 NVMe 에러 전파 */
		spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
		return RQ_END_IO_NONE;
	}

	blk_account_io_flush(flush_rq); /* [한국어] Flush 완료 통계 기록: ios[STAT_FLUSH] + nsecs[STAT_FLUSH] 갱신 */
	/*
	 * Flush request has to be marked as IDLE when it is really ended
	 * because its .end_io() is called from timeout code path too for
	 * avoiding use-after-free.
	 */
	WRITE_ONCE(flush_rq->state, MQ_RQ_IDLE); /* [한국어] WRITE_ONCE 로 쓰는 이유: 타임아웃 경로가 락 없이 이 state 를 읽어 flush_rq 가 재활용 가능함을 인지하도록 */
	if (fq->rq_status != BLK_STS_OK) {
		/* [한국어] 이전에 보류된 에러가 있으면: refcount 경쟁에서 먼저 도착한 에러를 최종 반영 */
		error = fq->rq_status; /* [한국어] 보류 에러를 최종 error로 채택: flush 결과를 대기 중이던 요청들에게 전파 */
		fq->rq_status = BLK_STS_OK; /* [한국어] 보류 상태 초기화: 다음 Flush 사이클을 위해 리셋 */
	}

	if (!q->elevator) {
		flush_rq->tag = BLK_MQ_NO_TAG; /* [한국어] non-scheduler 모드: blk_kick_flush에서 first_rq에서 빌린 tag 반납, sbitmap 슬롯 재사용 가능 */
	} else {
		blk_mq_put_driver_tag(flush_rq); /* [한국어] scheduler 모드: 드라이버 태그 반납 → 다른 요청이 이 태그를 다시 쓸 수 있게 됨. 재활용 */
		flush_rq->internal_tag = BLK_MQ_NO_TAG; /* [한국어] scheduler internal tag 반납: scheduler tag bitmap 해제 */
	}

	running = &fq->flush_queue[fq->flush_running_idx]; /* [한국어] 방금 끝난 Flush 하나에 묶여 있던 요청들 전부.
					 * 이 파일의 최적화가 결실을 맺는 지점이다 — 명령은 하나만 나갔는데
					 * 여기 매달린 요청은 여럿일 수 있고, 그 전부가 한꺼번에 다음 단계로 간다.
					 * 원래 주석: 방금 완료된 NVMe Flush에 묶인 PRE/POSTFLUSH 요청 리스트 */
	BUG_ON(fq->flush_pending_idx == fq->flush_running_idx); /* [한국어] 양쪽 idx 가 같다면 in-flight Flush 가 없다는 뜻이므로 여기 올 수 없다 — 상태 기계 위반: C1 조건 파괴, 즉시 패닉 */

	/* account completion of the flush request */
	fq->flush_running_idx ^= 1; /* [한국어] running_idx 토글 — 이 시점부터 pending_idx == running_idx 가 되어 C1 이 열리고 다음 Flush 발행이 가능해진다. 대기 요청이 들어올 빈 슬롯이 됨 */

	/* and push the waiting requests to the next stage */
	list_for_each_entry_safe(rq, n, running, queuelist) {
		/* [한국어] 이 Flush 하나에 묶여 있던 PRE/POSTFLUSH 요청 전부를 다음 단계로 전이시킨다 */
		unsigned int seq = blk_flush_cur_seq(rq); /* [한국어] 각 요청의 다음 단계: PREFLUSH 완료→DATA, POSTFLUSH 완료→DONE */

		BUG_ON(seq != REQ_FSEQ_PREFLUSH && seq != REQ_FSEQ_POSTFLUSH); /* [한국어] running 리스트에는 PRE/POSTFLUSH 요청만 있어야 함: 다른 단계면 state machine 버그 */
		list_del_init(&rq->queuelist); /* [한국어] running 리스트에서 제거: 이후 blk_flush_complete_seq에서 다음 리스트/경로로 이동 */
		blk_flush_complete_seq(rq, fq, seq, error); /* [한국어] PREFLUSH 완료→DATA, POSTFLUSH 완료→DONE(최종 완료 보고) */
	}

	spin_unlock_irqrestore(&fq->mq_flush_lock, flags);
	return RQ_END_IO_NONE; /* [한국어] flush_rq는 free 하지 않고 blk_flush_queue 가 계속 들고 있다가 다음 발행에 재사용한다. 원래 주석: blk_kick_flush가 다음 NVMe Flush에 같은 rq를 사용 */
}

/*
 * [한국어]
 * is_flush_rq - 주어진 request가 blk_flush_queue의 flush_rq인지 판별
 *
 * @rq: 판별 대상 request
 * @return: true이면 NVMe Flush 명령용 flush_rq, false이면 일반 request
 *
 * rq->end_io가 flush_end_io를 가리키면 blk_kick_flush가 발행한 flush_rq다.
 * 드라이버는 flush_rq 를 일반 요청과 똑같이
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
 * 발행 시 first_rq의 mq_ctx/mq_hctx/tag를 flush_rq 에 복사해 디스패치 경로를
 * 빌린다. fq->mq_flush_lock을 보유한 상태에서 호출되어야 한다.
 *
 * 호출 체인:
 *   blk_flush_complete_seq → [blk_kick_flush]
 *     → blk_mq_kick_requeue_list → (dispatch) → mq_ops->queue_rq
 */
static void blk_kick_flush(struct request_queue *q, struct blk_flush_queue *fq,
			   blk_opf_t flags)
{
	struct list_head *pending = &fq->flush_queue[fq->flush_pending_idx]; /* [한국어] 현재 NVMe Flush 명령 발행 후보 대기열: pending_idx 버퍼 */
	struct request *first_rq =		/* [한국어] 대기열 맨 앞 요청. 이 하나가 flush_rq 에게 mq_ctx/mq_hctx/tag 를 빌려 준다.
						 * "빌린다"는 표현이 정확한 이유: flush_rq 는 자기 태그를 따로 얻지 않고,
						 * first_rq 가 flush 완료를 기다리며 멈춰 있는 동안 그 태그를 대신 쓴다.
						 * 둘이 동시에 인플라이트일 수 없으므로 충돌하지 않는다. */
		list_first_entry(pending, struct request, queuelist); /* [한국어] 대기열의 첫 번째 요청: flush_rq에 빌려줄 mq_hctx/mq_ctx/tag 제공자 */
	struct request *flush_rq = fq->flush_rq; /* [한국어] per-hctx NVMe Flush 명령 전용 request: blk_alloc_flush_queue에서 사전 할당된 재사용 rq */

	/* C1 described at the top of this file */
	if (fq->flush_pending_idx != fq->flush_running_idx || list_empty(pending))
		/* [한국어] C1 — 두 인덱스가 다르다는 것은 곧 "Flush 가 이미 하나 떠 있다"는 뜻이다.
		 * 이 파일이 인덱스 두 개로 in-flight 여부를 표현하는 이유: 별도의 bool 을 두는 대신
		 * 대기 버퍼와 발행 버퍼를 번갈아 쓰면(더블 버퍼링), Flush 가 떠 있는 동안 새로 들어온
		 * 요청들이 다른 버퍼에 자연스럽게 쌓여 다음 Flush 한 번으로 함께 처리된다.
		 * 즉 이 조건은 단순한 배제가 아니라 배칭 그 자체다.
		 * pending 이 비었으면 발행할 것이 없으므로 역시 반환. */
		return;

	/* C2 and C3 */
	if (fq->flush_data_in_flight &&			/* [한국어] C2 — 아직 끝나지 않은 데이터 쓰기가 있다 */
	    time_before(jiffies,			/* [한국어] C3 — 그리고 기다린 지 아직 오래되지 않았다 */
			fq->flush_pending_since + FLUSH_PENDING_TIMEOUT))
		/* [한국어] 일부러 늦추는 구간이다. 지금 당장 Flush 를 쏘면 진행 중인 데이터 쓰기는
		 * 그 Flush 에 포함되지 않아, 그것들을 위해 곧 또 한 번 Flush 를 쏴야 한다.
		 * 조금 기다렸다가 한 번에 묶는 편이 총 Flush 횟수를 줄인다.
		 * 다만 무한정 기다리면 지연이 늘어나므로 C3(FLUSH_PENDING_TIMEOUT)가 상한을 준다.
		 * 처리량과 지연 사이의 명시적 절충이며, 이 파일에서 유일하게 "시간"을 보는 곳이다. */
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
	flush_rq->mq_ctx = first_rq->mq_ctx; /* [한국어] 소프트웨어 컨텍스트(제출 CPU) 복사 — flush_rq 는 원래 어느 CPU 것도 아니므로 첫 요청의 것을 물려받는다 */
	flush_rq->mq_hctx = first_rq->mq_hctx; /* [한국어] 하드웨어 큐(hctx) 상속. NVMe PCIe 에서 hctx 는 SQ/CQ 한 쌍에 대응하므로,
						 * 이 선택이 곧 어느 doorbell 을 울리고 어느 MSI-X 벡터로 완료를 받을지를 정한다 */

	if (!q->elevator)				/* [한국어] 스케줄러가 없으면 요청이 드라이버 태그를 직접 들고 있다 */
		flush_rq->tag = first_rq->tag; /* [한국어] non-scheduler: first_rq 의 드라이버 태그를 flush_rq 가 빌려 쓴다. flush_rq 는 자기 태그를 따로 얻지 않는다. first_rq는 flush 완료까지 stall */
	else						/* [한국어] 스케줄러가 있으면 요청이 든 것은 스케줄러 태그이고, 드라이버 태그는 디스패치 직전에 따로 얻는다 */
		flush_rq->internal_tag = first_rq->internal_tag; /* [한국어] scheduler: first_rq의 scheduler internal_tag를 빌림. blk_mq_get_driver_tag에서 실제 NVMe 슬롯 획득 */

	flush_rq->cmd_flags = REQ_OP_FLUSH | REQ_PREFLUSH; /* [한국어] 이 요청은 데이터 없는 순수 배리어다. NVMe 라면 nvme_setup_flush() 가 opcode 0x00(nvme_cmd_flush)과 nsid 만 채운 SQE 로 만든다 */
	flush_rq->cmd_flags |= (flags & REQ_DRV) | (flags & REQ_FAILFAST_MASK); /* [한국어] 원 요청의 driver 플래그와 failfast 정책 상속: NVMe 타임아웃/중단 동작에 영향 */
	flush_rq->rq_flags |= RQF_FLUSH_SEQ; /* [한국어] flush 시퀀스 요청 표시: req_bio_endio에서 bio 최종 완료를 지연시키는 역할 */
	flush_rq->end_io = flush_end_io; /* [한국어] 완료 시 blk_mq_end_request 가 rq->end_io 를 부르고, 그 경로로 상태 기계가 다음 단계로 넘어간다 */
	/*
	 * Order WRITE ->end_io and WRITE rq->ref, and its pair is the one
	 * implied in refcount_inc_not_zero() called from
	 * blk_mq_find_and_get_req(), which orders WRITE/READ flush_rq->ref
	 * and READ flush_rq->end_io
	 */
	smp_wmb(); /* memory barrier: end_io/rq_flags/tag 쓰기가 refcount 관찰 전에 완료·타임아웃 경로에서 보이도록 */
	req_ref_set(flush_rq, 1); /* flush_rq 참조 카운트 설정: timeout 경로에서도 안전한 completion 처리 */

	/*
	 * requeue_list/flush_list로 넣어 blk_mq_run_hw_queue를 통해
	 * mq_ops->queue_rq 간접 호출을 통해 드라이버로 내려간다.
	 */
	spin_lock(&q->requeue_lock); /* flush_list 보호: NVMe dispatch 경로와 동기화 */
	list_add_tail(&flush_rq->queuelist, &q->flush_list); /* flush_rq를 hardware queue의 flush_list에 추가: 디스패치되면 REQ_OP_FLUSH 로서 드라이버에 전달된다 명령 생성 */
	spin_unlock(&q->requeue_lock);

	blk_mq_kick_requeue_list(q); /* [한국어] hardware queue 깨우기: flush_list 에 넣기만 해서는 아무도 가져가지 않으므로 디스패치를 명시적으로 예약한다 */
}

/*
 * [한국어]
 * mq_flush_data_end_io - NVMe Write/FUA DATA 단계 완료 핸들러
 *
 * @rq: NVMe Write(FUA) 명령이 완료된 request
 * @error: 완료 상태 (BLK_STS_OK 이면 정상)
 * @iob: completion batch (이 함수에서는 미사용)
 * @return: RQ_END_IO_NONE (request를 free하지 않음)
 *
 * blk_rq_init_flush에서 rq->end_io를 이 함수로 교체했으므로, FUA Write가
 * 데이터 쓰기가 완료되면 이 함수가 호출된다. 처리 순서:
 * 1. scheduler 모드이면 NVMe driver tag 반납
 * 2. flush_data_in_flight 감소 (C2 조건 해제)
 * 3. blk_flush_complete_seq(REQ_FSEQ_DATA)로 POSTFLUSH/DONE 전이
 * 4. hctx 재시작 (blk_mq_sched_restart)
 * 실행 컨텍스트: 완료 경로. 장치 인터럽트에서 시작하지만 IPI/softirq 로 넘어갈 수 있어
 * 어느 쪽이든 잠들 수 없다고 보고 락을 irqsave 로 잡는다.
 *
 * 호출 체인:
 *   장치 완료 → blk_mq_end_request → rq->end_io
 *     → blk_mq_complete_request → [mq_flush_data_end_io]
 *     → blk_flush_complete_seq(DATA) → blk_kick_flush
 */
static enum rq_end_io_ret mq_flush_data_end_io(struct request *rq,
					       blk_status_t error,
					       const struct io_comp_batch *iob)
{
	struct request_queue *q = rq->q; /* [한국어] 요청이 속한 request_queue */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx; /* [한국어] DATA 완료 후 이 hctx 를 재시작한다 — flush 대기 때문에 멈춰 세웠던 디스패치를 다시 연다 */
	struct blk_mq_ctx *ctx = rq->mq_ctx; /* [한국어] 요청이 제출된 per-CPU 소프트웨어 컨텍스트 */
	unsigned long flags; /* [한국어] spin_lock_irqsave 플래그 */
	struct blk_flush_queue *fq = blk_get_flush_queue(ctx); /* [한국어] hctx에서 per-hctx flush 상태머신 획득 */

	if (q->elevator) {
		WARN_ON(rq->tag < 0); /* [한국어] 스케줄러 모드에서 드라이버 태그가 음수면 태그 회계가 깨진 것 */
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
	bool supports_fua = q->limits.features & BLK_FEAT_FUA; /* [한국어] 장치가 FUA 를 지원하는가. NVMe 는 이 비트를 BLK_FEAT_WRITE_CACHE 와 함께 VWC 로부터 설정하므로 둘이 항상 같이 켜진다 */
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

	/* [한국어] 여기서 policy 네 갈래가 이 파일의 전부다. 각 갈래의 반환값이
	 * 서로 다른 의미를 갖는다는 점에 주의:
	 *   true  = "이 요청은 내가 삼켰다. 호출자는 잊어라."
	 *   false = "나는 손대지 않았다. 평소대로 디스패치하라."
	 * 상태 기계에 들어가는 것은 true 를 돌려주는 두 갈래뿐이다. */
	switch (policy) {
	case 0:					/* [한국어] 할 일 없음 — 데이터도 없고 캐시도 없다 */
		/*
		 * An empty flush handed down from a stacking driver may
		 * translate into nothing if the underlying device does not
		 * advertise a write-back cache.  In this case, simply
		 * complete the request.
		 */
		blk_mq_end_request(rq, 0); /* [한국어] 장치에 명령을 하나도 보내지 않고 성공으로 끝낸다.
					 * NVMe 에서 이 경로에 들어오는 대표적인 경우가 VWC 를 광고하지 않는
					 * 엔터프라이즈 SSD(전력 손실 보호 내장)다 — 캐시가 휘발성이 아니니
					 * 배출할 것이 없고, Flush 는 정의상 이미 만족되어 있다.
					 * fsync 비용이 장치에 따라 극단적으로 갈리는 지점이 바로 여기다. */
		return true; /* [한국어] 요청을 소비했다 — 호출자는 추가 디스패치를 하지 않는다 */
	case REQ_FSEQ_DATA:			/* [한국어] 데이터는 있는데 앞뒤로 Flush 가 필요 없다 */
		/*
		 * If there's data, but no flush is necessary, the request can
		 * be processed directly without going through flush machinery.
		 * Queue for normal execution.
		 */
		return false; /* [한국어] 상태 기계를 아예 거치지 않고 평범한 쓰기로 내보낸다.
			   * REQ_FUA 가 남아 있다면 드라이버가 그것을 그대로 쓰기 명령에 실어 보낸다
			   * (NVMe: nvme_setup_rw 의 control |= NVME_RW_FUA). 명령 하나로 끝나는
			   * 가장 빠른 길이며, FUA 를 지원하는 장치에서 이 경로가 중요한 이유다. */
	case REQ_FSEQ_DATA | REQ_FSEQ_POSTFLUSH:	/* [한국어] 쓰고 나서 Flush 로 마무리해야 하는 경우 (FUA 미지원 장치) */
		/*
		 * Initialize the flush fields and completion handler to trigger
		 * the post flush, and then just pass the command on.
		 */
		blk_rq_init_flush(rq); /* [한국어] flush 시퀀스 등록: DATA 완료 후 mq_flush_data_end_io → POSTFLUSH */
		rq->flush.seq |= REQ_FSEQ_PREFLUSH; /* [한국어] 하지도 않을 PREFLUSH 를 "이미 끝난 것"으로 표시해 둔다.
					 * blk_flush_cur_seq() 가 seq 에서 가장 낮은 미완료 비트를 다음 단계로 고르기
					 * 때문에, 건너뛸 단계는 미리 완료로 칠해 두어야 DATA 부터 시작한다.
					 * 아래 default 갈래가 ~policy 를 통째로 넘기는 것도 같은 기법이다.
					 * 원래 주석: blk_flush_cur_seq가 DATA를 첫 단계로 반환하도록 */
		spin_lock_irq(&fq->mq_flush_lock);
		fq->flush_data_in_flight++; /* [한국어] 쓰기를 내보내기 **전에** 미리 센다. 순서가 중요하다 —
					 * 내보낸 뒤에 세면 그 사이에 blk_kick_flush() 가 C2 를 통과해
					 * 이 쓰기를 포함하지 않은 Flush 를 쏴 버릴 수 있다.
					 * 원래 주석: C2 조건(POSTFLUSH 지연)이 즉시 적용되도록 */
		spin_unlock_irq(&fq->mq_flush_lock);
		return false; /* [한국어] 쓰기 자체는 평범한 경로로 내보낸다. 다만 위에서 end_io 를
			   * mq_flush_data_end_io 로 바꿔 두었으므로, 완료가 돌아오는 순간
			   * 상태 기계가 이어받아 POSTFLUSH 로 넘어간다. */
	default:				/* [한국어] PREFLUSH 가 필요한 모든 조합 — 상태 기계를 처음부터 태운다 */
		/*
		 * Mark the request as part of a flush sequence and submit it
		 * for further processing to the flush state machine.
		 */
		blk_rq_init_flush(rq); /* [한국어] flush 시퀀스 등록: PREFLUSH → DATA → POSTFLUSH 전체 경로 */
		spin_lock_irq(&fq->mq_flush_lock);
		/* [한국어] 실행하지 않는 단계(~policy)를 이미 완료한 것처럼 마킹 후 현재 첫 단계로 전이 */
		blk_flush_complete_seq(rq, fq, REQ_FSEQ_ACTIONS & ~policy, 0);	/* [한국어] "policy 에 없는 단계는 전부 이미 끝난 것으로 쳐라"를
									 * 한 번의 호출로 표현한 것이다. 그러면 함수 내부의 다음 단계 계산이
									 * 자동으로 policy 의 첫 단계를 가리키게 되어, 진입점을 따로 분기할 필요가 없다. */
		spin_unlock_irq(&fq->mq_flush_lock);
		return true; /* [한국어] flush 상태머신이 요청을 소비함: PREFLUSH부터 시작 */
	}
}

/*
 * [한국어]
 * blkdev_issue_flush - 상위 계층/사용자공간의 명시적 flush 요청 처리
 *
 * @bdev: flush를 발행할 블록 디바이스
 * @return: 0이면 성공, 음수이면 에러
 *
 * fsync, fdatasync, sync 등 상위 계층에서 명시적 flush가 필요할 때 호출된다.
 * 내부적으로 REQ_OP_WRITE | REQ_PREFLUSH bio를 생성해 submit_bio_wait로
 * 동기 대기한다. blk_insert_flush 가 이 bio 를 상태 기계에 태우고, 최종적으로 드라이버가 Flush 명령으로 만든다. NVMe 라면 opcode 0x00. 이어서
 * 완료까지 호출자가 블록된다.
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
 * 각 하드웨어 큐(hctx)마다 독립적인 flush 상태머신이 필요하므로
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

	spin_lock_init(&fq->mq_flush_lock); /* [한국어] per-hctx flush lock 초기화: 완료/타임아웃/제출 세 경로 간 직렬화 */

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
 *   nvme_init_hctx / nvme_init_hctx
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
 * - struct blk_flush_queue는 per-hctx 로 존재하며 (NVMe PCIe 라면 SQ/CQ 쌍 단위),
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
