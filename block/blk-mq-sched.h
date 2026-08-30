/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_MQ_SCHED_H
#define BLK_MQ_SCHED_H

/* [한국어] elevator.h: elevator_type, elevator_queue, elv_change_ctx 등 공통 스케줄러 타입 */
#include "elevator.h"
/* [한국어] blk-mq.h: blk_mq_hw_ctx, blk_mq_ctx, blk_mq_tag_set 등 멀티큐 핵심 구조체 */
#include "blk-mq.h"

/*
 * [한국어 설명] blk-mq IO 스케줄러 인터페이스 헤더 (blk-mq-sched.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 blk-mq(Multi-Queue Block Layer)의 IO 스케줄러 프레임워크가 외부에
 * 제공하는 공개 API와 내부 인라인 함수들을 선언한다. elevator(mq-deadline·BFQ·kyber)와
 * blk-mq dispatch 엔진 사이의 인터페이스를 정의하며, bio merge·dispatch·restart·
 * tag pool 할당/해제 등의 함수 프로토타입을 담는다. 구현은 blk-mq-sched.c에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더를 include하는 파일들이 blk-mq-sched.c의 기능을 사용하는 계층이다:
 *
 *   [blk-mq.c] blk_mq_submit_bio()
 *       ↓ #include "blk-mq-sched.h"
 *   blk_mq_sched_bio_merge()         ← bio를 기존 request에 합치기 시도
 *       ↓
 *   blk_mq_sched_dispatch_requests() ← hctx dispatch 최상위 진입점
 *       ↓
 *   [blk-mq-sched.c 구현] → nvme_queue_rq() → SQ doorbell
 *
 * 실행 컨텍스트: 선언만 담긴 헤더이므로 컨텍스트는 각 함수 구현에서 결정.
 * inline 함수들은 호출 지점의 컨텍스트를 그대로 따른다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더를 include하는 모듈:
 *   - block/blk-mq.c: submit_bio, complete, requeue 경로에서 sched API 호출
 *   - block/elevator.c: elevator_change_done에서 blk_mq_init_sched() 호출
 *   - block/mq-deadline.c, bfq-iosched.c, kyber-iosched.c: elevator 구현체가
 *     blk_mq_sched_dispatch_requests() 등을 통해 dispatch 예약
 * 의존 헤더:
 *   - elevator.h: elevator_type(ops vtable), elevator_queue, elv_change_ctx
 *   - blk-mq.h: blk_mq_hw_ctx(dispatch·sched_tags·ctx_map), blk_mq_tag_set
 *
 * === 주요 함수/구조체 요약 ===
 * blk_mq_sched_bio_merge()        - bio를 sw queue 기존 request에 merge 시도
 * blk_mq_sched_try_insert_merge() - request 삽입 시 elevator hash에서 merge 시도
 * blk_mq_sched_dispatch_requests()- hctx dispatch 최상위 진입점
 * blk_mq_sched_mark_restart_hctx()- SCHED_RESTART 비트 설정 (SQ full 재출발 예약)
 * blk_mq_sched_restart()          - SCHED_RESTART 확인 후 조건부 __blk_mq_sched_restart
 * blk_mq_sched_allow_merge()      - elevator의 allow_merge ops 위임
 * blk_mq_sched_completed_request()- CQ 완료 시 elevator에 latency 피드백
 * blk_mq_init_sched()             - elevator 초기화 + hctx sched_tags 연결
 * blk_mq_alloc_sched_tags()       - scheduler shadow CID pool(elevator_tags) 할당
 * MAX_SCHED_RQ                    - scheduler shadow tag pool 최대 크기 (16×default)
 */

/* [한국어] MAX_SCHED_RQ: scheduler shadow tag pool 최대 크기;
 * BLKDEV_DEFAULT_RQ×16 = 여러 NVMe SQ depth를 커버하는 software-side request pool 상한;
 * blk_mq_alloc_sched_tags()에서 shared tags 모드의 pool 크기로 사용 */
#define MAX_SCHED_RQ (16 * BLKDEV_DEFAULT_RQ)

/*
 * [한국어]
 * blk_mq_sched_try_merge - bio를 request_queue의 기존 request에 merge 시도
 *
 * @q:              NVMe namespace request_queue
 * @bio:            merge를 시도할 신규 bio
 * @nr_segs:        bio의 물리 세그먼트 수 (PRP/SGL 엔트리 복잡도 지표)
 * @merged_request: back-merge 시 기존 request를 역할 병합한 request 반환 (out)
 * @return:         merge 성공이면 true
 *
 * blk_mq_submit_bio()에서 elevator의 merge hash/RB-tree를 통해 back/front/discard
 * merge를 시도한다. 성공하면 NVMe CID(driver tag)를 새로 소모하지 않고 기존
 * request의 PRP/SGL list를 연장해 SQ 엔트리 수를 절약한다.
 */
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs, struct request **merged_request);

/*
 * [한국어]
 * blk_mq_sched_bio_merge - elevator 또는 sw queue에서 bio merge 가능 여부 확인
 *
 * @q:      NVMe namespace request_queue
 * @bio:    merge를 시도할 신규 bio
 * @nr_segs: bio의 물리 세그먼트 수
 * @return: merge 성공이면 true
 *
 * elevator가 있으면 e->ops.bio_merge()에 위임하고, 없으면 per-CPU sw queue에서
 * 역방향 8개를 검사한다. 구현: blk-mq-sched.c::blk_mq_sched_bio_merge()
 */
bool blk_mq_sched_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs);

/*
 * [한국어]
 * blk_mq_sched_try_insert_merge - request 삽입 시 elevator hash에서 merge 시도
 *
 * @q:    NVMe namespace request_queue
 * @rq:   elevator에 삽입하려는 request
 * @free: merge로 흡수되어 해제할 request 목록 (out)
 * @return: merge 성공이면 true
 *
 * rq_mergeable() 기본 조건 확인 후 elv_attempt_insert_merge()를 호출한다.
 * 성공 시 NVMe PRP/SGL 체인이 길어지고 SQ 엔트리 수가 감소한다.
 */
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq,
				   struct list_head *free);

/*
 * [한국어]
 * blk_mq_sched_mark_restart_hctx - hctx에 SCHED_RESTART 비트 설정 (SQ 재출발 예약)
 *
 * @hctx: NVMe SQ/CQ hardware context
 *
 * budget·tag 고갈 또는 dispatch 중단 시 이 비트를 세워 나중에 SQ가 비면
 * __blk_mq_sched_restart()가 재가동을 트리거하도록 예약한다.
 */
void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * __blk_mq_sched_restart - SCHED_RESTART 클리어 + smp_mb() + blk_mq_run_hw_queue
 *
 * @hctx: 재가동할 NVMe SQ/CQ hardware context
 *
 * blk_mq_sched_restart()에서 SCHED_RESTART가 세워진 경우에만 호출된다.
 * 메모리 배리어로 dispatch list 가시성을 보장한 후 async work 예약.
 */
void __blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * blk_mq_sched_dispatch_requests - hctx dispatch 최상위 진입점
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 *
 * blk_mq_run_hw_queue()에서 호출. stopped/quiesced 확인 후
 * __blk_mq_sched_dispatch_requests()를 최대 2회 시도한다.
 */
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * blk_mq_init_sched / blk_mq_exit_sched - elevator attach/detach
 *
 * init: elevator_queue 할당 + hctx sched_tags 연결 + init_sched/init_hctx ops 호출
 * exit: exit_hctx/exit_sched ops 호출 + sched_tags 정리 + ELEVATOR_FLAG_DYING 설정
 */
int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e,
		struct elevator_resources *res);
/* [한국어] blk_mq_exit_sched: elevator 제거; exit_hctx + exit_sched + sched_tags teardown */
void blk_mq_exit_sched(struct request_queue *q, struct elevator_queue *e);

/* [한국어] blk_mq_sched_free_rqs: scheduler shadow tag pool에 남은 request 해제
 * (queue cleanup 또는 elevator switch 시 tagset 보유 하에 호출) */
void blk_mq_sched_free_rqs(struct request_queue *q);

/*
 * [한국어]
 * blk_mq_alloc_sched_tags / blk_mq_free_sched_tags
 *   scheduler shadow CID pool(elevator_tags) 할당/해제.
 *   hctx->sched_tags는 NVMe CID shadow로 동작하며 dispatch 시 driver tag 확보 전 사용.
 */
struct elevator_tags *blk_mq_alloc_sched_tags(struct blk_mq_tag_set *set,
		unsigned int nr_hw_queues, unsigned int nr_requests);
/* [한국어] blk_mq_alloc_sched_res: 단일 queue에 대해 elevator_tags + private data 할당 */
int blk_mq_alloc_sched_res(struct request_queue *q,
		struct elevator_type *type,
		struct elevator_resources *res,
		unsigned int nr_hw_queues);
/* [한국어] blk_mq_alloc_sched_res_batch: tagset 모든 queue에 일괄 할당 (update_nr_hwq_lock write 하에) */
int blk_mq_alloc_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set, unsigned int nr_hw_queues);
/* [한국어] blk_mq_alloc_sched_ctx_batch: elv_tbl에 elv_change_ctx를 queue별로 일괄 생성 */
int blk_mq_alloc_sched_ctx_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set);
/* [한국어] blk_mq_free_sched_ctx_batch: elv_tbl의 모든 elv_change_ctx를 xa_erase + kfree */
void blk_mq_free_sched_ctx_batch(struct xarray *elv_tbl);
/* [한국어] blk_mq_free_sched_tags: elevator_tags(nr_hw_queues개 tag map) 해제 후 kfree(et) */
void blk_mq_free_sched_tags(struct elevator_tags *et,
		struct blk_mq_tag_set *set);
/* [한국어] blk_mq_free_sched_res: elevator_resources(et + data)의 두 필드를 각각 해제 */
void blk_mq_free_sched_res(struct elevator_resources *res,
		struct elevator_type *type,
		struct blk_mq_tag_set *set);
/* [한국어] blk_mq_free_sched_res_batch: tagset 모든 queue의 elevator 자원 일괄 해제 */
void blk_mq_free_sched_res_batch(struct xarray *et_table,
		struct blk_mq_tag_set *set);

/*
 * [한국어]
 * blk_mq_alloc_sched_data - elevator_type의 alloc_sched_data() 콜백 래퍼
 *
 * @q: NVMe namespace request_queue
 * @e: elevator_type (mq-deadline/BFQ/kyber 등)
 * @return: 할당된 private data 포인터; 불필요 시 NULL; 실패 시 ERR_PTR(-ENOMEM)
 *
 * e->ops.alloc_sched_data()를 호출해 스케줄러별 상태 구조체를 생성한다.
 * "none" 스케줄러거나 alloc 콜백이 없으면 NULL을 반환한다.
 */
static inline void *blk_mq_alloc_sched_data(struct request_queue *q,
		struct elevator_type *e)
{
	/* [한국어] sched_data: 스케줄러별 private 상태 포인터 (mq-deadline rb-tree, bfq_data 등) */
	void *sched_data;

	/* [한국어] elevator 없거나 alloc 콜백 미등록(none 스케줄러 또는 불필요) → NULL 반환 */
	if (!e || !e->ops.alloc_sched_data)
		return NULL;

	/* [한국어] alloc_sched_data(): 스케줄러 내부 자료구조 초기화 + 반환 */
	sched_data = e->ops.alloc_sched_data(q);
	/* [한국어] NULL 반환은 ENOMEM으로 변환: 호출자가 IS_ERR()로 실패 구분 */
	return (sched_data) ?: ERR_PTR(-ENOMEM);
}

/*
 * [한국어]
 * blk_mq_free_sched_data - elevator_type의 free_sched_data() 콜백 래퍼
 *
 * @e:    elevator_type
 * @data: blk_mq_alloc_sched_data()로 할당된 private 상태 포인터
 *
 * blk_mq_free_sched_res()에서 호출되어 스케줄러 private 구조체를 해제한다.
 */
static inline void blk_mq_free_sched_data(struct elevator_type *e, void *data)
{
	/* [한국어] elevator와 free 콜백이 모두 유효할 때만 해제 */
	if (e && e->ops.free_sched_data)
		/* [한국어] free_sched_data(): 스케줄러 private 상태 구조체 해제 */
		e->ops.free_sched_data(data);
}

/*
 * [한국어]
 * blk_mq_sched_restart - SCHED_RESTART 비트 확인 후 조건부 재가동
 *
 * @hctx: NVMe SQ/CQ hardware context
 *
 * SQ 완료(CQ entry) 처리 후 호출되어, SCHED_RESTART가 세워진 hctx만
 * 재가동한다. 비트가 없으면 함수 체인 없이 즉시 반환 — 불필요한 dispatch 방지.
 */
static inline void blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] BLK_MQ_S_SCHED_RESTART 비트 원자적 확인: 세워진 경우에만 재가동 */
	if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
		/* [한국어] clear_bit + smp_mb() + blk_mq_run_hw_queue() → 재 dispatch → doorbell */
		__blk_mq_sched_restart(hctx);
}

/*
 * [한국어]
 * bio_mergeable - bio에 REQ_NOMERGE_FLAGS가 없어 merge 가능한지 확인
 *
 * @bio: 확인할 bio
 * @return: merge 가능하면 true; REQ_NOMERGE/NOWAIT 등이 있으면 false
 *
 * REQ_NOMERGE_FLAGS: REQ_NOMERGE | REQ_PREFLUSH | REQ_FUA | REQ_SWAP 등;
 * 이 플래그가 있는 bio는 독립 NVMe 명령(SQ 엔트리)으로 전달되어야 한다.
 */
static inline bool bio_mergeable(struct bio *bio)
{
	/* [한국어] REQ_NOMERGE_FLAGS 미설정이면 merge 허용 → NVMe PRP/SGL 연장 가능 */
	return !(bio->bi_opf & REQ_NOMERGE_FLAGS);
}

/*
 * [한국어]
 * blk_mq_sched_allow_merge - elevator의 allow_merge ops 위임
 *
 * @q:   NVMe namespace request_queue
 * @rq:  merge 대상 기존 request
 * @bio: 합치려는 신규 bio
 * @return: merge 허용이면 true
 *
 * RQF_USE_SCHED 플래그로 스케줄러 관리 request인지 확인한 후 e->ops.allow_merge() 호출.
 * 스케줄러 미사용("none") 또는 콜백 없으면 기본적으로 true 반환.
 */
static inline bool
blk_mq_sched_allow_merge(struct request_queue *q, struct request *rq,
			 struct bio *bio)
{
	/* [한국어] RQF_USE_SCHED: 이 request가 elevator(shadow tag)로 관리되는지 표시 */
	if (rq->rq_flags & RQF_USE_SCHED) {
		/* [한국어] 현재 attach된 elevator 인스턴스 */
		struct elevator_queue *e = q->elevator;

		/* [한국어] allow_merge() ops가 있으면 스케줄러 정책에 위임 */
		if (e->type->ops.allow_merge)
			/* [한국어] merge 정책에 따라 허용/거부: 거부 시 별도 NVMe 명령으로 전달 */
			return e->type->ops.allow_merge(q, rq, bio);
	}
	/* [한국어] 스케줄러 미사용 또는 allow_merge 콜백 없음 → 기본 허용 */
	return true;
}

/*
 * [한국어]
 * blk_mq_sched_completed_request - NVMe CQ 완료 시 elevator에 latency 피드백
 *
 * @rq:  완료된 request
 * @now: 완료 시점 타임스탬프 (ktime_get_ns() 기반)
 *
 * NVMe CQ 엔트리 처리(blk_mq_complete_request) 경로에서 호출된다. elevator에게
 * 완료 latency를 피드백하여 mq-deadline·BFQ 등이 dispatch 우선순위를 조정하게 한다.
 */
static inline void blk_mq_sched_completed_request(struct request *rq, u64 now)
{
	/* [한국어] RQF_USE_SCHED: 스케줄러 관리 request만 피드백 대상 */
	if (rq->rq_flags & RQF_USE_SCHED) {
		/* [한국어] 이 request가 속한 namespace queue의 elevator */
		struct elevator_queue *e = rq->q->elevator;

		/* [한국어] completed_request() ops: latency 샘플 기록 → 향후 dispatch 우선순위 재조정 */
		if (e->type->ops.completed_request)
			e->type->ops.completed_request(rq, now);
	}
}

/*
 * [한국어]
 * blk_mq_sched_requeue_request - abort/timeout된 request를 elevator 큐에 재삽입
 *
 * @rq: NVMe timeout/reset으로 abort된 request
 *
 * blk_mq_requeue_request()에서 호출. elevator의 requeue_request() ops를 통해
 * 스케줄러 큐 맨 앞 등 우선 위치에 재삽입 → 이후 다시 nvme_queue_rq()로 전달.
 */
static inline void blk_mq_sched_requeue_request(struct request *rq)
{
	/* [한국어] RQF_USE_SCHED: 스케줄러(shadow tag) 관리 request만 처리 */
	if (rq->rq_flags & RQF_USE_SCHED) {
		/* [한국어] NVMe namespace request_queue */
		struct request_queue *q = rq->q;
		/* [한국어] 현재 attach된 elevator 인스턴스 */
		struct elevator_queue *e = q->elevator;

		/* [한국어] requeue_request() ops: 우선 위치에 재삽입 → CQ 복구 후 재전달 */
		if (e->type->ops.requeue_request)
			e->type->ops.requeue_request(rq);
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
						depth); /* [한국어] sbitmap 앞쪽 depth 범위에서만 비트를 찾는다. 한 큐가 드라이버 태그를
				 * 전부 채가지 못하게 막아, 스케줄러가 재정렬할 후보를 남겨 두기 위한 상한이다. */
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
