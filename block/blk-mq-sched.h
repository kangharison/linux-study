/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_MQ_SCHED_H
#define BLK_MQ_SCHED_H

#include "elevator.h"
#include "blk-mq.h"

/*
 * blk-mq-sched.h - Multi-Queue Block Layer Scheduler Interface
 *
 * 파일 개요:
 *   이 헤더는 blk-mq I/O 스케줄러의 핵심 인터페이스를 정의한다.
 *   elevator.h에서 제공하는 범용 I/O 스케줄러 프레임워크를 blk-mq
 *   (Multi-Queue) 환경에 연결하며, 요청 합병(bio merge), 디스패치
 *   (dispatch), 재시작(restart), 스케줄러 자원 할당/해제 등을
 *   조율한다.
 *
 *   NVMe SSD 관점에서 볼 때, 이 파일은 상위 elevator / schedulers
 *   (mq-deadline, bfq, kyber 등)와 하위 NVMe 드라이버 사이의
 *   중재 계층이다. NVMe queue, SQ/CQ, doorbell, CID, PRP/SGL 등의
 *   실제 하드웨어 동작은 drivers/nvme/에서 처리하지만, 이 파일은
 *   언제 어느 request를 NVMe에 날릴지 결정하는 정책 레이어의
 *   연결고리 역할을 한다.
 *
 *   상/하위 연결:
 *     blk-mq.c / blk-mq-tag.c  ->  blk-mq-sched.h  ->  elevator.h
 *     blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_sched_try_merge
 *     -> (scheduler 선택) -> blk_mq_sched_dispatch_requests
 *     -> blk_mq_run_hw_queue -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */

/*
 * MAX_SCHED_RQ:
 *   스케줄러가 한 번에 관리할 수 있는 최대 request 수.
 *   NVMe에서 이 값은 여러 SQ(Submission Queue) 엔트리를
 *   추상화한 software-side 요청 풀의 상한과 연관된다.
 */
#define MAX_SCHED_RQ (16 * BLKDEV_DEFAULT_RQ) /* NVMe 다중 SQ 엔트리를 커버하는 software-side request pool 상한 */

/*
 * blk_mq_sched_try_merge():
 *   bio를 기존 request에 합병할 수 있는지 시도한다.
 *   NVMe 관점: 연속한 LBA의 두 NVMe read/write command를
 *   하나의 PRP/SGL list로 묶어 SQ 엔트리 수를 아낄 수 있다.
 *   호출 경로 (추정):
 *     blk_mq_submit_bio -> blk_mq_bio_list_merge -> blk_mq_sched_try_merge
 *     -> (성공 시) request 길이 확장 -> nvme_queue_rq
 */
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs, struct request **merged_request); /* bio merge 성공 시 NVMe PRP/SGL list 확장, CID 소모 없이 SQ 엔트리 재활용 */

/*
 * blk_mq_sched_bio_merge():
 *   상위에서 bio merge 가능 여부를 스케줄러에 문의한다.
 *   NVMe 연결: bio merge가 허용되면 CID 소모 없이 기존 request를
 *   재활용하므로 NVMe queue depth를 효율적으로 쓸 수 있다.
 */
bool blk_mq_sched_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs); /* merge 가능 여부 -> CID/tag 사용량 및 NVMe queue depth 효율 결정 */

/*
 * blk_mq_sched_try_insert_merge():
 *   request를 스케줄러 큐에 삽입하면서 동시에 merge 후보를 탐색한다.
 *   NVMe 연결: NVMe I/O가 완료되어 풀린 request slot에 다시 채울 때
 *   스케줄러 낸 request들을 합쳐서 SQ에 밀어넣는 경로의 일부다.
 */
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq,
				   struct list_head *free); /* scheduler 삽입+merge -> SQ에 밀어넣기 전 후보 묶음 */

/*
 * blk_mq_sched_mark_restart_hctx():
 *   hctx(hardware queue context)에 BLK_MQ_S_SCHED_RESTART 플래그를 세워
 *   나중에 dispatch가 다시 시도되도록 표시한다.
 *   NVMe 관점: NVMe SQ가 가득 차 doorbell 발행이 막혔을 때,
 *   CQ completion이 도착해 SQ 엔트리가 비면 hctx를 restart하여
 *   대기 중인 request를 다시 NVMe에 전달한다.
 */
void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx); /* SQ full 시 doorbell 재발행을 위한 restart flag 설정 (추정) */

/*
 * __blk_mq_sched_restart():
 *   hctx restart의 실제 처리를 수행한다.
 *   호출 경로:
 *     blk_mq_sched_restart() -> __blk_mq_sched_restart(hctx)
 *     -> blk_mq_run_hw_queue -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
void __blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx); /* hctx restart -> blk_mq_run_hw_queue -> nvme_queue_rq -> doorbell */

/*
 * blk_mq_sched_dispatch_requests():
 *   hctx에 대해 스케줄러가 관리하는 request들을 NVMe queue로
 *   디스패치한다. NVMe 관점에서 이 함수는 SQ 엔트리를 채우기 직전
 *   마지막 software-side 정책 결정 지점이다.
 *   호출 경로 (추정):
 *     blk_mq_run_hw_queue -> blk_mq_sched_dispatch_requests
 *     -> e->type->ops.dispatch -> nvme_queue_rq -> nvme_submit_cmd
 */
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx); /* scheduler -> NVMe SQ 엔트리 채우기 직전 마지막 정책 결정 */

/*
 * blk_mq_init_sched() / blk_mq_exit_sched():
 *   request_queue에 elevator 스케줄러를 attach/detach한다.
 *   NVMe 연결: NVMe namespace가 처음 열릴 때 queue setup 중
 *   elevator를 초기화하며, teardown 시 정리한다.
 */
int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e,
		struct elevator_resources *res); /* elevator attach: NVMe namespace open 시 queue setup 단계 */
void blk_mq_exit_sched(struct request_queue *q, struct elevator_queue *e); /* elevator detach: NVMe namespace close/teardown 시 */

/*
 * blk_mq_sched_free_rqs():
 *   스케줄러가 할당한 request들을 해제한다.
 */
void blk_mq_sched_free_rqs(struct request_queue *q); /* scheduler 할당 request 해제 -> NVMe queue drain 이후 자원 정리 */

/*
 * blk_mq_alloc_sched_tags() / blk_mq_free_sched_tags():
 *   스케줄러 전용 tag bitmap을 할당/해제한다.
 *   NVMe 연결: struct blk_mq_hw_ctx의 sched_tags는 NVMe queue depth에
 *   대응하는 software-side tag pool로, CID(Command Identifier) 할당의
 *   한 단계 위에서 동작한다.
 */
struct elevator_tags *blk_mq_alloc_sched_tags(struct blk_mq_tag_set *set,
		unsigned int nr_hw_queues, unsigned int nr_requests); /* sched_tags 할당: NVMe queue depth 대응 tag pool 생성 */
int blk_mq_alloc_sched_res(struct request_queue *q,
		struct elevator_type *type,
		struct elevator_resources *res,
		unsigned int nr_hw_queues); /* scheduler 리소스 할당 (추정) */
int blk_mq_alloc_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set, unsigned int nr_hw_queues); /* elevator 리소스 batch 할당 (추정) */
int blk_mq_alloc_sched_ctx_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set); /* scheduler context batch 할당 (추정) */
void blk_mq_free_sched_ctx_batch(struct xarray *elv_tbl); /* scheduler context batch 해제 */
void blk_mq_free_sched_tags(struct elevator_tags *et,
		struct blk_mq_tag_set *set); /* sched_tags 해제: NVMe queue depth 대응 tag pool 제거 */
void blk_mq_free_sched_res(struct elevator_resources *res,
		struct elevator_type *type,
		struct blk_mq_tag_set *set); /* scheduler 리소스 해제 */
void blk_mq_free_sched_res_batch(struct xarray *et_table,
		struct blk_mq_tag_set *set); /* elevator 리소스 batch 해제 */

/*
 * blk_mq_alloc_sched_data() - 스케줄러별 private data를 할당한다.
 *
 * 목적:
 *   elevator_type(e)에 등록된 alloc_sched_data 콜백을 호출하여
 *   mq-deadline, bfq, kyber 등 각 스케줄러의 상태 구조체를 생성한다.
 *
 * 호출 경로 (추정):
 *   blk_mq_init_sched -> blk_mq_alloc_sched_data -> e->ops.alloc_sched_data
 *
 * NVMe 연결:
 *   반환된 sched_data는 NVMe queue 설정 시 request_queue에 붙으며,
 *   이후 nvme_queue_rq 전 스케줄러의 dispatch 정책을 결정하는 데
 *   사용된다.
 *
 * 반환:
 *   - 성공 시 할당된 포인터
 *   - 할당 불필요 시 NULL
 *   - 실패 시 ERR_PTR(-ENOMEM)
 */
static inline void *blk_mq_alloc_sched_data(struct request_queue *q, /* request_queue: NVMe namespace당 하나의 blk queue */
		struct elevator_type *e) /* elevator_type: mq-deadline/bfq/kyber 등 택일 */
{
	void *sched_data; /* 스케줄러별 private state 포인터 (NVMe 큐 정책 상태) */

	if (!e || !e->ops.alloc_sched_data) /* 스케줄러가 alloc 콜백을 제공하지 않으면 */
		return NULL; /* private data 없이 진행 (예: none 스케줄러) */

	sched_data = e->ops.alloc_sched_data(q); /* 스케줄러 내부 테이블/큐 초기화 -> NVMe dispatch 정책 준비 */
	return (sched_data) ?: ERR_PTR(-ENOMEM); /* 할당 실패 시 ENOMEM 강제 반환 (추정) */
}

/*
 * blk_mq_free_sched_data():
 *   blk_mq_alloc_sched_data()로 할당된 스케줄러 private data를 해제한다.
 *   NVMe teardown 경로에서 request_queue 해제 직전에 호출된다 (추정).
 */
static inline void blk_mq_free_sched_data(struct elevator_type *e, void *data) /* data: 스케줄러 private state */
{
	if (e && e->ops.free_sched_data) /* elevator_type과 free 콜백이 유효할 때만 */
		e->ops.free_sched_data(data); /* NVMe teardown 중 정책 상태 해제 */
}

/*
 * blk_mq_sched_restart():
 *   hctx->state에 SCHED_RESTART 플래그가 있을 때만 __blk_mq_sched_restart()
 *   를 호출하여 불필요한 dispatch 재시도를 방지한다.
 *
 *   NVMe 연결: NVMe doorbell 발행 후 SQ가 가득 찼다면, 이 함수가
 *   completion CQ 처리 후 남은 request를 다시 밀어넣는 trigger가 된다.
 */
static inline void blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx) /* hctx: NVMe SQ에 대응하는 hardware queue context */
{
	if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state)) /* atomic bit test: SQ full로 인해 pending된 restart 요청 존재 여부 */
		__blk_mq_sched_restart(hctx); /* 실제 restart 수행 -> blk_mq_run_hw_queue -> nvme_queue_rq -> doorbell */
}

/*
 * bio_mergeable():
 *   bio->bi_opf에 REQ_NOMERGE_FLAGS가 설정되어 있지 않으면
 *   merge가 가능하다고 판단한다.
 *
 *   NVMe 연결: REQ_NOMERGE가 설정된 bio는 NVMe 상에서도 독립된
 *   command(SQ 엔트리)로 전달되어야 하며, 상위에서 묶이지 않는다.
 */
static inline bool bio_mergeable(struct bio *bio) /* bio: NVMe command로 변환될 I/O 단위 */
{
	return !(bio->bi_opf & REQ_NOMERGE_FLAGS); /* REQ_NOMERGE/REQ_NOMERGE_FLAGS 미설정 시에만 merge 가능 -> SQ 엔트리 절약 가능 */
}

/*
 * blk_mq_sched_allow_merge():
 *   스케줄러가 rq와 bio의 merge를 허용하는지 확인한다.
 *
 *   호출 경로 (추정):
 *     blk_mq_sched_try_merge -> blk_mq_sched_allow_merge
 *     -> e->type->ops.allow_merge
 *
 *   NVMe 연결: merge가 거부되면 NVMe SQ에는 별도의 command로
 *   들어가며, 이는 SQ/CQ 자원을 더 소모하지만 일관성/성능 trade-off를
 *   스케줄러가 결정하는 지점이다.
 */
static inline bool
blk_mq_sched_allow_merge(struct request_queue *q, struct request *rq, /* rq: NVMe command 후보 request */
			 struct bio *bio) /* bio: rq에 합칠 후보 bio */
{
	if (rq->rq_flags & RQF_USE_SCHED) { /* 이 request가 스케줄러 관리 대상일 때만 문의 */
		struct elevator_queue *e = q->elevator; /* q->elevator: 현재 attach된 scheduler 인스턴스 */

		if (e->type->ops.allow_merge) /* 스케줄러별 allow_merge 콜백 존재 시 */
			return e->type->ops.allow_merge(q, rq, bio); /* 정책에 따라 merge 허용/거부 -> SQ/CQ 자원 사용량 결정 */
	}
	return true; /* 스케줄러 미사용 시 기본적으로 merge 허용 -> NVMe PRP/SGL 연장 가능 */
}

/*
 * blk_mq_sched_completed_request():
 *   request 완료 시점에 스케줄러에게 통지한다.
 *
 *   호출 경로 (추정):
 *     nvme_irq -> nvme_process_cq -> blk_mq_complete_request
 *     -> blk_mq_sched_completed_request -> e->ops.completed_request
 *
 *   NVMe 연결: NVMe CQ 엔트리가 도착해 CID가 반환되면, 스케줄러는
 *   완료 시간을 피드백하여 향후 dispatch 우선순위를 조정한다.
 */
static inline void blk_mq_sched_completed_request(struct request *rq, u64 now) /* now: 완료 시점 timestamp (NVMe CQ entry 기반) */
{
	if (rq->rq_flags & RQF_USE_SCHED) { /* 스케줄러가 관리한 request만 피드백 */
		struct elevator_queue *e = rq->q->elevator; /* namespace queue의 elevator */

		if (e->type->ops.completed_request) /* 완료 콜백 등록 시 */
			e->type->ops.completed_request(rq, now); /* latency 샘플 갱신 -> 향후 NVMe dispatch 우선순위 재조정 */
	}
}

/*
 * blk_mq_sched_requeue_request():
 *   완료되지 않은 request를 스케줄러 큐의 맨 앞 등으로 재삽입한다.
 *
 *   호출 경로 (추정):
 *     nvme_timeout / nvme_reset -> blk_mq_requeue_request
 *     -> blk_mq_sched_requeue_request -> e->ops.requeue_request
 *
 *   NVMe 연결: NVMe controller timeout이나 reset으로 command가
 *   abort되면, 해당 request를 다시 스케줄링하여 SQ에 재전달한다.
 */
static inline void blk_mq_sched_requeue_request(struct request *rq) /* rq: NVMe timeout/abort로 반환된 request */
{
	if (rq->rq_flags & RQF_USE_SCHED) { /* scheduler 소유 tag를 가진 request일 때만 */
		struct request_queue *q = rq->q; /* NVMe namespace queue */
		struct elevator_queue *e = q->elevator; /* 현재 scheduler */

		if (e->type->ops.requeue_request) /* requeue 콜백이 있으면 */
			e->type->ops.requeue_request(rq); /* 스케줄러 큐 맨 앞 등으로 재삽입 -> 이후 다시 nvme_queue_rq로 전달 */
	}
}

/*
 * blk_mq_sched_has_work():
 *   스케줄러가 디스패치할 request를 가지고 있는지 확인한다.
 *
 *   NVMe 연결: 이 함수가 false면 NVMe doorbell을 발행할 필요가
 *   없으므로 불필요한 MMIO(write doorbell)를 피한다.
 */
static inline bool blk_mq_sched_has_work(struct blk_mq_hw_ctx *hctx) /* hctx: NVMe SQ에 연결된 blk-mq hw queue */
{
	struct elevator_queue *e = hctx->queue->elevator; /* namespace queue의 elevator 인스턴스 */

	if (e && e->type->ops.has_work) /* scheduler attach되어 있고 has_work 콜백 있으면 */
		return e->type->ops.has_work(hctx); /* dispatch 대기 중인 request가 있는지 확인 -> false면 doorbell 발행 회피 */

	return false; /* scheduler 없으면 work 없음으로 처리 */
}

/*
 * blk_mq_sched_needs_restart():
 *   hctx에 SCHED_RESTART 플래그가 세워져 있는지 확인한다.
 *
 *   NVMe 연결: NVMe SQ가 가득 차 일시 중단된 dispatch 흐름이
 *   completion 이후 재개되어야 하는지 판단하는 데 쓰인다.
 */
static inline bool blk_mq_sched_needs_restart(struct blk_mq_hw_ctx *hctx) /* hctx: NVMe queue당 하나 */
{
	return test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state); /* atomic bit test: SQ full 등으로 pending된 restart 필요 여부 */
}

/*
 * blk_mq_set_min_shallow_depth():
 *   모든 hctx의 sched_tags bitmap에 대해 shallow depth 최소값을 설정한다.
 *
 *   NVMe 연결: NVMe queue depth보다 적은 수의 tag만 깊게 탐색하도록
 *   제한하여, 높은 동시성 하에서 tag allocation 지연을 줄인다 (추정).
 *   즉, NVMe SQ 엔트리(CID) 확보 경로 상에서 tag 검색 범위를 조정한다.
 */
static inline void blk_mq_set_min_shallow_depth(struct request_queue *q, /* q: NVMe namespace queue */
						unsigned int depth) /* shallow 탐색 최소 깊이 */
{
	struct blk_mq_hw_ctx *hctx; /* iteration 당 NVMe SQ/CQ pair에 대응 */
	unsigned long i; /* hctx 인덱스 = NVMe queue id mapping */

	queue_for_each_hw_ctx(q, hctx, i) /* queue의 모든 hctx(즉, NVMe queue들)에 적용 */
		sbitmap_queue_min_shallow_depth(&hctx->sched_tags->bitmap_tags, /* sched_tags bitmap: NVMe queue depth보다 상위인 tag pool */
						depth); /* depth보다 얕은 영역만 우선 탐색 -> CID/tag 확보 지연 감소 (추정) */
}

/*
 * blk_mq_is_sync_read():
 *   주어진 opf가 sync read인지 판단한다.
 *
 *   NVMe 연결: NVMe에는 separate read/write opcode가 있으며,
 *   sync read는 종종 latency-sensitive한 NVMe read command로
 *   식별되어 우선 처리될 수 있다.
 */
static inline bool blk_mq_is_sync_read(blk_opf_t opf) /* opf: bio/request의 operation flags */
{
	return op_is_sync(opf) && !op_is_write(opf); /* sync read면 true -> NVMe Read opcode로 latency-sensitive path 분류 */
}

#endif

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 elevator 스케줄러와 blk-mq 하드웨어 큐(hctx) 사이의
 *   접착제 역할을 하며, NVMe SQ/CQ에 command를 밀어넣기 직전 마지막
 *   software-side 정책 결정을 담당한다.
 *
 * - blk_mq_sched_dispatch_requests()와 blk_mq_sched_restart()는
 *   NVMe doorbell 발행 전후에 request 흐름을 조절하며, SQ 가득 참
 *   상태에서 completion 이후 dispatch를 재개하는 핵심 지점이다.
 *
 * - bio/request merge 허용/거부는 NVMe SQ 엔트리 수와 CID 사용량에
 *   직접적인 영향을 주며, blk_mq_sched_allow_merge()와
 *   blk_mq_sched_try_merge()에서 결정된다.
 *
 * - sched_tags는 struct blk_mq_hw_ctx에 속한 tag pool로, NVMe queue
 *   depth와 밀접하게 연관되며 CID 할당보다 상위에서 동작한다.
 *
 * - 완료/재큐 콜백(completed_request, requeue_request)은 NVMe CQ
 *   처리 및 timeout/reset 경로에서 호출되어 스케줄러 피드백 루프를
 *   완성한다.
 */
