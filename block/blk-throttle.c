// SPDX-License-Identifier: GPL-2.0
/*
 * Interface for controlling IO bandwidth on a request queue
 *
 * Copyright (C) 2010 Vivek Goyal <vgoyal@redhat.com>
 */

/*
 * [한국어] blk-throttle: cgroup 기반 IO 대역폭/IOPS 조율 계층 (blk-throttle.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 커널의 cgroup 기반 블록 IO 쓰로틀링(throttling) 계층을
 * 구현한다. 응용 프로그램의 bio가 NVMe SQ(Submission Queue)/blk-mq에
 * 도달하기 전, bps(bytes per second)와 iops(IO per second) 제한을 적용해
 * cgroup별 대역폭/초당 명령 수를 소프트웨어적으로 제어한다.
 * 토큰 버킷(Token Bucket) 알고리즘을 100ms 슬라이스 윈도우 단위로 구현하며,
 * 제한을 초과한 bio는 throtl_grp 큐에 보관 후 pending_timer가 disptime에
 * 도달하면 상위 service_queue로 전달한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   submit_bio()
 *     → blk_mq_submit_bio()
 *       → __blk_throtl_bio()          ← 이 파일의 핵심 진입점
 *         [제한 초과 시: throtl 큐에 보관]
 *         → throtl_pending_timer_fn() [disptime 도달 시]
 *           → throtl_select_dispatch() → tg_dispatch_one_bio()
 *             → (최상위 service_queue 도달)
 *               → blk_throtl_dispatch_work_fn() [kthrotld workqueue]
 *                 → submit_bio_noacct_nocheck()
 *                   → blk_mq_submit_bio()
 *                     → nvme_queue_rq() → nvme_submit_cmd(doorbell)
 * 실행 컨텍스트: 소프트웨어 커널 컨텍스트 (프로세스 컨텍스트 + softirq +
 * kworker). queue_lock(spinlock)으로 동시성 제어.
 *
 * === 타 모듈과의 연결 ===
 * - blk-cgroup (blk-cgroup.c, blkcg_gq, blkcg_policy): cgroup별 blkg를
 *   할당하고 blkcg_policy_throtl를 등록해 throtl_grp(pd)를 관리.
 * - blk-mq (blk-mq.c): __blk_throtl_bio()가 bio를 제어하며, throttle 통과
 *   후 submit_bio_noacct_nocheck()로 blk-mq에 재진입.
 * - NVMe 드라이버 (nvme-core.c, nvme-pci.c): doorbell 시점 이전에 throttle이
 *   먼저 bio를 차단. throtl_dispatch_work_fn()에서 풀린 bio만 NVMe CID/SQ에
 *   접근.
 * - blk-throttle.h: throtl_grp, throtl_service_queue, throtl_qnode, 플래그
 *   정의 포함.
 * - cgroupfs io.max / blkio.throttle.*: 사용자 공간에서 bps/iops 상한 설정.
 * 데이터 흐름: bio → __blk_throtl_bio() → throtl_grp 큐 → pending_timer →
 * td->service_queue → kthrotld → blk-mq → NVMe SQ/CQ.
 *
 * === 주요 함수/구조체 요약 ===
 * __blk_throtl_bio()         : 핵심 진입점. bio가 bps/iops 제한 내이면 통과,
 *                              초과 시 throtl 큐에 보관 후 true 반환.
 * tg_dispatch_time()         : bio가 현재 slice에서 기다려야 할 jiffies 계산.
 *                              bps → iops 순서로 검사.
 * tg_dispatch_one_bio()      : bio를 현재 tg 큐에서 꺼내 부모 service_queue로
 *                              이동. 최상위 도달 시 dispatch_work 트리거.
 * throtl_pending_timer_fn()  : pending_timer 핸들러. disptime 도래한 tg를
 *                              순회해 bio를 상위로 전파.
 * blk_throtl_dispatch_work_fn(): kthrotld에서 실행. 최상위 service_queue의
 *                              bio를 submit_bio_noacct_nocheck()로 blk-mq에 전달.
 * struct throtl_grp (tg)     : per-(cgroup, queue) 상태. bps[2]/iops[2] 제한,
 *                              bytes_disp/io_disp 사용량, slice_start/end 윈도우.
 * struct throtl_data (td)    : per-request_queue 컨트롤러. 최상위 service_queue
 *                              와 dispatch_work 포함.
 */

#include <linux/module.h>      /* [한국어] 커널 모듈 초기화/해제 (module_init) */
#include <linux/slab.h>        /* [한국어] kzalloc_node/kfree: NUMA-aware 메모리 할당 */
#include <linux/blkdev.h>      /* [한국어] request_queue, gendisk: 블록 장치 핵심 구조체 */
#include <linux/bio.h>         /* [한국어] struct bio, bio_list: IO 요청 단위 */
#include <linux/blktrace_api.h>/* [한국어] blk_add_trace_msg: blktrace 디버그 메시지 */
#include "blk.h"               /* [한국어] 블록 레이어 내부 API: submit_bio_noacct_nocheck 등 */
#include "blk-cgroup-rwstat.h" /* [한국어] blkg_rwstat: cgroup별 IO 통계 집계 */
#include "blk-throttle.h"      /* [한국어] throtl_grp/service_queue/qnode/플래그 정의 */

/* Max dispatch from a group in 1 round */
#define THROTL_GRP_QUANTUM 8    /* [한국어] 한 라운드에서 하나의 cgroup이 디스패치할 최대 bio 수; READ 6 + WRITE 2 비율로 분배 */

/* Total max dispatch from all groups in one round */
#define THROTL_QUANTUM 32       /* [한국어] 한 라운드에서 전체 cgroup 합산 최대 디스패치 bio 수; NVMe SQ batch 크기 상한 */

/* Throttling is performed over a slice and after that slice is renewed */
#define DFL_THROTL_SLICE (HZ / 10) /* [한국어] 기본 슬라이스: 100ms(HZ/10). 이 윈도우 내 bps/iops 평균을 제한 */

/* A workqueue to queue throttle related work */
static struct workqueue_struct *kthrotld_workqueue; /* [한국어] throttle 통과 bio를 blk-mq로 전달하는 전용 워크큐; blk_throtl_dispatch_work_fn()이 여기서 실행됨 */

#define rb_entry_tg(node)	rb_entry((node), struct throtl_grp, rb_node) /* [한국어] rb_node → throtl_grp 변환 매크로; pending_tree 순회 시 사용 */

struct throtl_data
{
	/* service tree for active throtl groups */
	struct throtl_service_queue service_queue;
	/* [한국어] 장치(request_queue) 단위 최상위 service_queue.
	 * 모든 하위 cgroup의 bio가 최종적으로 이 큐에 도달하며,
	 * blk_throtl_dispatch_work_fn()이 여기서 bio를 꺼내 blk-mq로 전달.
	 * 설정자: throtl_service_queue_init()이 blk_throtl_init()에서 초기화.
	 * 읽는 자: throtl_pending_timer_fn(), blk_throtl_dispatch_work_fn().
	 * 동기화: queue_lock(spinlock) 보호. */

	struct request_queue *queue;
	/* [한국어] 이 throttle 상태가 연결된 request_queue.
	 * NVMe에서는 namespace의 request_queue를 가리킴.
	 * blk_throtl_init()에서 할당 후 q->td로 역참조됨.
	 * 설정자: blk_throtl_init().
	 * 읽는 자: throtl_pending_timer_fn(), blk_throtl_dispatch_work_fn().
	 * 동기화: 초기화 이후 불변; queue_lock 없이 읽기 가능. */

	/* Total Number of queued bios on READ and WRITE lists */
	unsigned int nr_queued[2];
	/* [한국어] READ[0] / WRITE[1] 방향별로 throtl 큐에 대기 중인 bio 총 수.
	 * __blk_throtl_bio()에서 증가, tg_dispatch_one_bio()에서 감소.
	 * 소프트웨어 Queue Depth 제어 지표; queue_lock 보호.
	 * 설정자: __blk_throtl_bio()(증가), tg_dispatch_one_bio()(감소).
	 * 읽는 자: tg_dispatch_one_bio()가 BUG_ON 검증에 사용.
	 * 동기화: queue_lock(spinlock). */

	/* Work for dispatching throttled bios */
	struct work_struct dispatch_work;
	/* [한국어] throttle 통과 bio를 submit_bio_noacct_nocheck()로 전달하는
	 * kthrotld workqueue work item.
	 * throtl_pending_timer_fn()에서 queue_work()로 예약.
	 * 핸들러: blk_throtl_dispatch_work_fn().
	 * 설정자: INIT_WORK()가 blk_throtl_init()에서 초기화.
	 * 동기화: workqueue 내부 직렬화. */
};

static void throtl_pending_timer_fn(struct timer_list *t);

/*
 * [한국어]
 * tg_to_blkg - throtl_grp에서 blkcg_gq 포인터를 반환한다.
 * @tg: 변환할 throtl_grp
 * @return: 연결된 blkcg_gq 포인터
 *
 * throtl_grp은 blkcg_gq의 policy data(pd)로 내장되므로,
 * pd_to_blkg()로 역참조한다.
 * 호출 체인: 다양한 함수 → [tg_to_blkg] → blkcg_gq 사용
 */
static inline struct blkcg_gq *tg_to_blkg(struct throtl_grp *tg)
{
	return pd_to_blkg(&tg->pd); /* [한국어] throtl_grp -> blkcg_gq 매핑: NVMe 장치의 cgroup별 queue 상태 접근 */
}

/**
 * sq_to_tg - return the throl_grp the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * Return the throtl_grp @sq belongs to.  If @sq is the top-level one
 * embedded in throtl_data, %NULL is returned.
 */
static struct throtl_grp *sq_to_tg(struct throtl_service_queue *sq)
{
	if (sq && sq->parent_sq) /* [한국어] service_queue가 throtl_grp에 내장된 경우 NVMe 제어 흐름의 하위 cgroup 반환 */
		return container_of(sq, struct throtl_grp, service_queue);
	else
		return NULL;
}

/**
 * sq_to_td - return throtl_data the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * A service_queue can be embedded in either a throtl_grp or throtl_data.
 * Determine the associated throtl_data accordingly and return it.
 */
static struct throtl_data *sq_to_td(struct throtl_service_queue *sq)
{
	struct throtl_grp *tg = sq_to_tg(sq); /* [한국어] sq가 throtl_grp에 속하는지 확인; 최상위면 NULL */

	if (tg)
		return tg->td; /* [한국어] 상위 service_queue에서 throtl_data 획득: NVMe namespace 단위 throttle 상태 */
	else
		return container_of(sq, struct throtl_data, service_queue);
}

/*
 * [한국어]
 * tg_bps_limit - cgroup의 READ 또는 WRITE bps 상한을 반환한다.
 * @tg: 조회할 throtl_grp
 * @rw: READ(0) 또는 WRITE(1)
 * @return: bps 제한값(바이트/초). 제한 없으면 U64_MAX.
 *
 * v2(default hierarchy)에서 root cgroup은 자식의 제한을 상속받으므로
 * 자신의 제한을 U64_MAX(무제한)로 반환한다. 그 외 cgroup은 tg->bps[rw]를
 * 반환한다.
 * 호출 체인: tg_within_bps_limit(), tg_dispatch_bps_time() → [tg_bps_limit]
 */
static uint64_t tg_bps_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 여부 확인용 */

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* [한국어] root cgroup은 하위에 제한을 상속시키기 위해 bps 제한을 무제한으로 둠 */
		return U64_MAX; /* [한국어] bps 무제한: NVMe SQ 유입 제한 없음 */

	return tg->bps[rw]; /* [한국어] 해당 방향(READ/WRITE)의 bps 상한 반환 */
}

/*
 * [한국어]
 * tg_iops_limit - cgroup의 READ 또는 WRITE iops 상한을 반환한다.
 * @tg: 조회할 throtl_grp
 * @rw: READ(0) 또는 WRITE(1)
 * @return: iops 제한값(회/초). 제한 없으면 UINT_MAX.
 *
 * v2(default hierarchy)에서 root cgroup은 무제한(UINT_MAX) 반환.
 * 호출 체인: tg_within_iops_limit(), tg_dispatch_iops_time() → [tg_iops_limit]
 */
static unsigned int tg_iops_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 여부 확인용 */

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* [한국어] root cgroup iops 무제한: NVMe 초당 명령 제한 없음 */
		return UINT_MAX; /* [한국어] iops 무제한; NVMe SQ 초당 명령 제한 없음 */

	return tg->iops[rw]; /* [한국어] 해당 방향(READ/WRITE)의 iops 상한 반환 */
}

/**
 * throtl_log - log debug message via blktrace
 * @sq: the service_queue being reported
 * @fmt: printf format string
 * @args: printf args
 *
 * The messages are prefixed with "throtl BLKG_NAME" if @sq belongs to a
 * throtl_grp; otherwise, just "throtl".
 */
#define throtl_log(sq, fmt, args...)	do {				\
	struct throtl_grp *__tg = sq_to_tg((sq));			\
	struct throtl_data *__td = sq_to_td((sq));			\
									\
	(void)__td;							\
	if (likely(!blk_trace_note_message_enabled(__td->queue)))	\
		break;							\
	if ((__tg)) {							\
		blk_add_cgroup_trace_msg(__td->queue,			\
			&tg_to_blkg(__tg)->blkcg->css, "throtl " fmt, ##args);\
	} else {							\
		blk_add_trace_msg(__td->queue, "throtl " fmt, ##args);	\
	}								\
} while (0)

/*
 * [한국어]
 * throtl_bio_data_size - bps 계산에 사용할 bio 크기를 반환한다.
 * @bio: 크기를 계산할 bio
 * @return: bps 카운트에 사용할 바이트 수
 *
 * DISCARD 명령은 실제 데이터 전송이 없으므로 512B로 간주한다.
 * 그 외 bio는 bi_iter.bi_size를 사용한다.
 * 호출 체인: tg_within_bps_limit(), throtl_charge_bps_bio() → [throtl_bio_data_size]
 */
static inline unsigned int throtl_bio_data_size(struct bio *bio)
{
	/* assume it's one sector */
	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) /* [한국어] REQ_OP_DISCARD는 논리 블록 512B로 계산; NVMe Deallocate/Write Zeroes 명령 크기/PRP 구성에 반영 */
		return 512; /* [한국어] DISCARD bio는 512바이트로 bps 카운트; 실제 데이터 이동 없음 */
	return bio->bi_iter.bi_size; /* [한국어] bio 실제 크기; NVMe PRP/SGL entry 수 및 DMA segment 수 계산의 입력값 */
}

/*
 * throtl_qnode_init: throtl_qnode를 초기화한다.
 * NVMe 관점: qnode는 아직 SQ에 진입하지 못하고 throtl 큐에 묶인 bio들을
 * 모아두는 그릇이며, blkg reference를 유지해 bio가 NVMe 쪽으로 디스패치될
 * 때까지 cgroup 객체가 해제되지 않도록 한다.
 */
/*
 * [한국어]
 * throtl_qnode_init - throtl_qnode를 초기화한다.
 * @qn: 초기화할 qnode
 * @tg: 이 qnode가 속할 throtl_grp
 *
 * qnode는 아직 NVMe SQ에 진입하지 못하고 throtl 큐에 묶인 bio들을
 * 모아두는 그릇이며, blkg reference를 유지해 bio가 NVMe 쪽으로 디스패치될
 * 때까지 cgroup 객체가 해제되지 않도록 한다.
 * 호출 체인: throtl_pd_alloc() → [throtl_qnode_init]
 */
static void throtl_qnode_init(struct throtl_qnode *qn, struct throtl_grp *tg)
{
	INIT_LIST_HEAD(&qn->node); /* [한국어] qnode를 service_queue의 queued[rw] 리스트에 연결하기 위한 list head 초기화 */
	bio_list_init(&qn->bios_bps); /* [한국어] bps 제한으로 NVMe SQ 진입이 지연된 bio 리스트 초기화 */
	bio_list_init(&qn->bios_iops); /* [한국어] iops 제한으로 NVMe SQ 진입이 지연된 bio 리스트 초기화 */
	qn->tg = tg; /* [한국어] bio가 NVMe로 디스패치될 때까지 cgroup(blkg) 참조 유지 */
}

/**
 * throtl_qnode_add_bio - add a bio to a throtl_qnode and activate it
 * @bio: bio being added
 * @qn: qnode to add bio to
 * @sq: the service_queue @qn belongs to
 *
 * Add @bio to @qn and put @qn on @sq->queued if it's not already on.
 * @qn->tg's reference count is bumped when @qn is activated.  See the
 * comment on top of throtl_qnode definition for details.
 *
 * NVMe 관점:
 * bio가 rate limit을 초과하면 이 함수를 통해 대기 큐에 들어간다. bps
 * 제한을 이미 통과한 분할 bio는 iops 큐(bios_iops)로 직접 이동하고,
 * 그 외는 bps 큐(bios_bps)에 머무른다. 이후 pending_tree에 따라
 * throtl_select_dispatch() -> tg_dispatch_one_bio() -> submit_bio_noacct_nocheck()
 * -> blk_mq_submit_bio() -> blk_mq_get_request() -> nvme_queue_rq() 경로로
 * NVMe SQ/CQ에 도달한다.
 */
static void throtl_qnode_add_bio(struct bio *bio, struct throtl_qnode *qn,
				 struct throtl_service_queue *sq)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향; NVMe SQ/CQ에서의 데이터 방향과 일치 */

	/*
	 * Split bios have already been throttled by bps, so they are
	 * directly queued into the iops path.
	 */
	if (bio_flagged(bio, BIO_TG_BPS_THROTTLED) || /* [한국어] 분할된 bio는 이미 bps 제한을 통과했으므로 iops 경로로 직접 진입 (NVMe CID 할당 대기) */
	    bio_flagged(bio, BIO_BPS_THROTTLED)) {
		bio_list_add(&qn->bios_iops, bio); /* [한국어] NVMe SQ 진입 전 iops 제한 대기열 */
		sq->nr_queued_iops[rw]++; /* [한국어] iops 제한 대기열 카운트 증가: NVMe 초당 명령 한도 초과 여부 추적 */
	} else {
		bio_list_add(&qn->bios_bps, bio); /* [한국어] bps 제한으로 인한 NVMe 유입 지연 */
		sq->nr_queued_bps[rw]++; /* [한국어] bps 제한 대기열 카운트 증가: NVMe 대역폭 한도 초과 여부 추적 */
	}

	if (list_empty(&qn->node)) { /* [한국어] qnode가 처음 활성화될 때만 list에 추가; NVMe 진입 전 cgroup 큐에 편입 */
		list_add_tail(&qn->node, &sq->queued[rw]); /* [한국어] bio를 throtl service_queue queued[rw]에 추가; NVMe 드라이버 이전 소프트웨어 대기열 */
		blkg_get(tg_to_blkg(qn->tg)); /* [한국어] NVMe 디스패치 전까지 blkg 유지 */
	}
}

/**
 * throtl_peek_queued - peek the first bio on a qnode list
 * @queued: the qnode list to peek
 *
 * Always take a bio from the head of the iops queue first. If the queue is
 * empty, we then take it from the bps queue to maintain the overall idea of
 * fetching bios from the head.
 */
/*
 * [한국어]
 * throtl_peek_queued - qnode 리스트의 첫 번째 bio를 peek(꺼내지 않고 확인)한다.
 * @queued: 확인할 qnode list_head (sq->queued[rw])
 * @return: 첫 번째 bio 포인터. 큐가 비면 NULL.
 *
 * iops 큐를 먼저 확인하고, 비어 있으면 bps 큐를 확인한다.
 * tg_dispatch_time()에서 현재 상태 확인용으로 사용.
 * 호출 체인: tg_dispatch_time(), tg_update_disptime() → [throtl_peek_queued]
 */
static struct bio *throtl_peek_queued(struct list_head *queued)
{
	struct throtl_qnode *qn;
	struct bio *bio;

	if (list_empty(queued)) /* [한국어] 대기열이 비면 NVMe로 보낼 bio 없음 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* [한국어] round-robin 순서에서 첫 번째 cgroup qnode 선택 */
	bio = bio_list_peek(&qn->bios_iops); /* [한국어] iops 큐에서 먼저 peek: NVMe 초당 명령 수 제한 우선 적용 */
	if (!bio)
		bio = bio_list_peek(&qn->bios_bps); /* [한국어] iops 큐가 비면 bps 큐에서 peek: 대역폭 제한 bio를 NVMe로 */
	WARN_ON_ONCE(!bio); /* [한국어] 양쪽 큐 모두 비어 있으면 throttle 상태 비일관성 (버그) */
	return bio;
}

/**
 * throtl_pop_queued - pop the first bio form a qnode list
 * @sq: the service_queue to pop a bio from
 * @tg_to_put: optional out argument for throtl_grp to put
 * @rw: read/write
 *
 * Pop the first bio from the qnode list @sq->queued. Note that we firstly
 * focus on the iops list because bios are ultimately dispatched from it.
 * After popping, the first qnode is removed from @sq->queued if empty or moved
 * to the end of @sq->queued so that the popping order is round-robin.
 *
 * When the first qnode is removed, its associated throtl_grp should be put
 * too.  If @tg_to_put is NULL, this function automatically puts it;
 * otherwise, *@tg_to_put is set to the throtl_grp to put and the caller is
 * responsible for putting it.
 *
 * NVMe 관점:
 * iops 큐에서 먼저 꺼내 NVMe의 초당 명령 수 제한을 준수한다. qnode가
 * 비면 blkg reference를 낮추며, round-robin 이동은 여러 cgroup 간 NVMe
 * 대역폭/Queue Depth가 한쪽에 쏠리지 않도록 한다.
 */
static struct bio *throtl_pop_queued(struct throtl_service_queue *sq,
				     struct throtl_grp **tg_to_put, bool rw)
{
	struct list_head *queued = &sq->queued[rw]; /* [한국어] READ/WRITE 큐 중 하나를 선택하여 NVMe 진입 후보 bio 탐색 */
	struct throtl_qnode *qn;
	struct bio *bio;

	if (list_empty(queued)) /* [한국어] 선택한 방향의 throtl 큐가 비어 있으면 디스패치 불가 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* [한국어] round-robin 위치의 qnode에서 bio 꺼냄 */
	bio = bio_list_pop(&qn->bios_iops); /* [한국어] NVMe iops 제한을 우선적으로 소진 */
	if (bio) {
		sq->nr_queued_iops[rw]--; /* [한국어] iops 큐에서 bio를 꺼낸 경우 NVMe 초당 명령 카운트 감소 */
	} else {
		bio = bio_list_pop(&qn->bios_bps); /* [한국어] bps 제한 bio를 이제 NVMe로 */
		if (bio) /* [한국어] bps 큐에서 bio를 꺼내 NVMe 대역폭 예산 소모 */
			sq->nr_queued_bps[rw]--; /* [한국어] bps 큐 카운트 감소; 남은 bps 예산은 이후 bio에 영향 */
	}
	WARN_ON_ONCE(!bio); /* [한국어] 양쪽 큐가 모두 비어 있으면 throtl 상태 비일관성 (버그) */

	if (bio_list_empty(&qn->bios_bps) && bio_list_empty(&qn->bios_iops)) { /* [한국어] qnode의 두 큐가 모두 비면 cgroup이 더 이상 NVMe 진입 후보가 아님 */
		list_del_init(&qn->node); /* [한국어] qnode를 service_queue에서 제거; NVMe dispatch candidate 해제 */
		if (tg_to_put)
			*tg_to_put = qn->tg; /* [한국어] blkg reference 해제는 bio 전달 완료 후 호출자가 담당 */
		else
			blkg_put(tg_to_blkg(qn->tg)); /* [한국어] qnode 제거 시점에 blkg reference 해제; NVMe 진입 전 메모리 누수 방지 */
	} else {
		list_move_tail(&qn->node, queued); /* [한국어] 다음 cgroup에게 NVMe 디스패치 기회 부여 */
	}

	return bio;
}

/*
 * [한국어]
 * throtl_service_queue_init - throtl_service_queue를 초기화한다.
 * @sq: 초기화할 service_queue (호출자가 0으로 초기화했다고 가정)
 *
 * queued[READ/WRITE] 리스트, pending_tree RB 트리, pending_timer를 초기화.
 * pending_timer 핸들러는 throtl_pending_timer_fn()이며 disptime 도달 시 발동.
 * 호출 체인: throtl_pd_alloc(), blk_throtl_init() → [throtl_service_queue_init]
 */
/* init a service_queue, assumes the caller zeroed it */
static void throtl_service_queue_init(struct throtl_service_queue *sq)
{
	INIT_LIST_HEAD(&sq->queued[READ]); /* [한국어] READ 방향 throtl 대기열 초기화: NVMe SQ 진입 전 READ bio 큐 */
	INIT_LIST_HEAD(&sq->queued[WRITE]); /* [한국어] WRITE 방향 throtl 대기열 초기화: NVMe SQ 진입 전 WRITE bio 큐 */
	sq->pending_tree = RB_ROOT_CACHED; /* [한국어] pending_tree 초기화: 다음 NVMe doorbell 시점이 가까운 cgroup 정렬 */
	timer_setup(&sq->pending_timer, throtl_pending_timer_fn, 0); /* [한국어] pending_timer 초기화: NVMe SQ로 bio를 풀어줄 시점을 지연시키는 소프트웨어 타이머 */
}

/*
 * [한국어]
 * throtl_pd_alloc - blkcg policy data를 할당하고 throtl_grp을 초기화한다.
 * @disk: 대상 gendisk
 * @blkcg: 대상 blkcg
 * @gfp: 메모리 할당 플래그
 * @return: 초기화된 blkg_policy_data 포인터, 실패 시 NULL
 *
 * 새 cgroup이 NVMe 장치에 대해 활성화될 때마다 생성되며,
 * bps/iops 상한을 기본 무제한(U64_MAX/UINT_MAX)으로 시작한다.
 * 실패 시 이미 할당된 rwstat을 해제하고 NULL 반환.
 * 호출 체인: blkcg_activate_policy() → pd_alloc_fn → [throtl_pd_alloc]
 */
static struct blkg_policy_data *throtl_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct throtl_grp *tg;
	int rw;

	tg = kzalloc_node(sizeof(*tg), gfp, disk->node_id); /* [한국어] per-node 메모리 할당; NVMe namespace와 동일 NUMA 노드 선호 */
	if (!tg) /* [한국어] 메모리 부족 시 NVMe 장치에 대한 throttle 초기화 실패 (abort) */
		return NULL;

	if (blkg_rwstat_init(&tg->stat_bytes, gfp)) /* [한국어] bps 통계용 rwstat 초기화; NVMe 대역폭 사용량 집계 */
		goto err_free_tg;

	if (blkg_rwstat_init(&tg->stat_ios, gfp)) /* [한국어] iops 통계용 rwstat 초기화; NVMe 초당 명령 수 집계 */
		goto err_exit_stat_bytes;

	throtl_service_queue_init(&tg->service_queue); /* [한국어] tg 자신의 service_queue 초기화; 자식 cgroup의 bio 대기 구조 */

	for (rw = READ; rw <= WRITE; rw++) { /* [한국어] READ/WRITE 양쪽에 대해 NVMe SQ 진입 전 대기 qnode 준비 */
		throtl_qnode_init(&tg->qnode_on_self[rw], tg); /* [한국어] 자신의 service_queue용 qnode; NVMe 진입 지연 bio 수용 */
		throtl_qnode_init(&tg->qnode_on_parent[rw], tg); /* [한국어] 부모 service_queue용 qnode; 상위 cgroup으로 NVMe 진입 후보 전달 */
	}

	RB_CLEAR_NODE(&tg->rb_node); /* [한국어] rb_node 초기화; pending_tree에 아직 미삽입 상태 표시 */
	tg->bps[READ] = U64_MAX; /* [한국어] READ bps 기본 무제한; NVMe SQ READ 대역폭 제한 없음 */
	tg->bps[WRITE] = U64_MAX; /* [한국어] WRITE bps 기본 무제한; NVMe SQ WRITE 대역폭 제한 없음 */
	tg->iops[READ] = UINT_MAX; /* [한국어] READ iops 기본 무제한; NVMe SQ READ 초당 명령 제한 없음 */
	tg->iops[WRITE] = UINT_MAX; /* [한국어] WRITE iops 기본 무제한; NVMe SQ WRITE 초당 명령 제한 없음 */

	return &tg->pd; /* [한국어] 초기화된 blkg_policy_data 반환; blkcg_gq에 내장됨 */

err_exit_stat_bytes:
	blkg_rwstat_exit(&tg->stat_bytes); /* [한국어] stat_bytes rwstat 해제; 실패 경로 정리 */
err_free_tg:
	kfree(tg); /* [한국어] throtl_grp 메모리 해제; 초기화 실패 정리 */
	return NULL;
}

/*
 * [한국어]
 * throtl_pd_init - 할당된 policy data를 부모 service_queue에 연결한다.
 * @pd: 초기화할 blkg_policy_data (throtl_grp)
 *
 * cgroup 계층이 NVMe SQ에 도달하기 전의 IO 우선순위/제한 트리가 된다.
 * v1(non-dfl)에서는 모든 tg가 throtl_data 최상위 service_queue 바로 아래로
 * 평탄화되고, v2(dfl)에서는 실제 계층 구조를 따라 부모 tg의 service_queue를
 * 부모로 설정한다.
 * 호출 체인: blkcg_activate_policy() → pd_init_fn → [throtl_pd_init]
 */
static void throtl_pd_init(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 확인용 */
	struct throtl_data *td = blkg->q->td; /* [한국어] blkcg_gq -> request_queue -> throtl_data 연결; NVMe namespace 단위 throttle */
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg 자신의 service_queue; 부모 연결 대상 */

	/*
	 * If on the default hierarchy, we switch to properly hierarchical
	 * behavior where limits on a given throtl_grp are applied to the
	 * whole subtree rather than just the group itself.  e.g. If 16M
	 * read_bps limit is set on a parent group, summary bps of
	 * parent group and its subtree groups can't exceed 16M for the
	 * device.
	 *
	 * If not on the default hierarchy, the broken flat hierarchy
	 * behavior is retained where all throtl_grps are treated as if
	 * they're all separate root groups right below throtl_data.
	 * Limits of a group don't interact with limits of other groups
	 * regardless of the position of the group in the hierarchy.
	 */
	sq->parent_sq = &td->service_queue; /* [한국어] v1(non-dfl)에서는 모든 throtl_grp이 throtl_data의 최상위 service_queue 아래로 평탄화 */
	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && blkg->parent) /* [한국어] v2(dfl)에서 parent가 있으면 상위 cgroup의 service_queue를 부모로 설정 (계층적 NVMe QoS) */
		sq->parent_sq = &blkg_to_tg(blkg->parent)->service_queue; /* [한국어] 부모 blkg의 throtl_grp service_queue를 이 tg의 부모로 설정 */
	tg->td = td; /* [한국어] throtl_data 역참조; NVMe namespace 단위 dispatch_work/pending_timer 접근용 */
}

/*
 * Set has_rules[] if @tg or any of its parents have limits configured.
 * This doesn't require walking up to the top of the hierarchy as the
 * parent's has_rules[] is guaranteed to be correct.
 *
 * [한국어]
 * tg_update_has_rules - 자신이나 조상에게 bps/iops 제한이 있는지 표시한다.
 * @tg: 갱신할 throtl_grp
 *
 * 제한이 전혀 없는 cgroup은 blk-throttle의 모든 검사를 생략하고 곧바로
 * nvme_queue_rq() 쪽으로 흘려볼 수 있다.
 * 호출 체인: throtl_pd_online(), tg_conf_updated() → [tg_update_has_rules]
 */
static void tg_update_has_rules(struct throtl_grp *tg)
{
	struct throtl_grp *parent_tg = sq_to_tg(tg->service_queue.parent_sq); /* [한국어] 부모 cgroup의 제한 상태를 상속받아 NVMe SQ 유입 제한 범위 결정 */
	int rw;

	for (rw = READ; rw <= WRITE; rw++) { /* [한국어] READ/WRITE 양쪽에 대해 NVMe SQ/CQ 방향별 규칙 존재 여부 갱신 */
		tg->has_rules_iops[rw] = /* [한국어] iops 규칙: 부모 또는 자신에게 제한이 있으면 NVMe 초당 명령 제한 적용 */
			(parent_tg && parent_tg->has_rules_iops[rw]) ||
			tg_iops_limit(tg, rw) != UINT_MAX;
		tg->has_rules_bps[rw] = /* [한국어] bps 규칙: 부모 또는 자신에게 제한이 있으면 NVMe 대역폭 제한 적용 */
			(parent_tg && parent_tg->has_rules_bps[rw]) ||
			tg_bps_limit(tg, rw) != U64_MAX;
	}
}

/*
 * [한국어]
 * throtl_pd_online - cgroup이 online될 때 has_rules[]를 갱신한다.
 * @pd: online된 blkg_policy_data (throtl_grp)
 *
 * 새 cgroup이 조상의 제한을 벗어나지 않도록 has_rules[]를 갱신.
 * 이후 이 cgroup으로 들어오는 bio는 갱신된 규칙으로 rate limit 적용.
 * 호출 체인: blkcg_activate_policy() → pd_online_fn → [throtl_pd_online]
 */
static void throtl_pd_online(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	/*
	 * We don't want new groups to escape the limits of its ancestors.
	 * Update has_rules[] after a new group is brought online.
	 */
	tg_update_has_rules(tg); /* [한국어] cgroup online 시 제한 상태 갱신; 이후 bio부터 NVMe SQ 유입 제한 결정 */
}

/*
 * [한국어]
 * throtl_pd_free - throtl_grp에 할당된 모든 자원을 해제한다.
 * @pd: 해제할 blkg_policy_data (throtl_grp)
 *
 * pending_timer 동기적 삭제 → rwstat 해제 → kfree 순서로 정리.
 * 호출 체인: blkcg_policy.pd_free_fn → [throtl_pd_free]
 */
static void throtl_pd_free(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */

	timer_delete_sync(&tg->service_queue.pending_timer); /* [한국어] throtl_grp 소멸 시 pending_timer 정지; 더 이상 NVMe doorbell 지연 예약 불필요 */
	blkg_rwstat_exit(&tg->stat_bytes); /* [한국어] bps 통계 rwstat 해제 */
	blkg_rwstat_exit(&tg->stat_ios); /* [한국어] iops 통계 rwstat 해제 */
	kfree(tg); /* [한국어] throtl_grp 메모리 해제; NVMe namespace throttle 계층에서 제거 */
}

/*
 * [한국어]
 * throtl_rb_first - pending_tree에서 disptime이 가장 이른 throtl_grp을 반환.
 * @parent_sq: 탐색할 상위 service_queue
 * @return: leftmost throtl_grp 포인터. 트리가 비면 NULL.
 *
 * pending_tree는 disptime 기준으로 정렬된 RB 트리이므로 leftmost가
 * 다음 dispatch 대상이다.
 * 호출 체인: throtl_select_dispatch(), update_min_dispatch_time() → [throtl_rb_first]
 */
static struct throtl_grp *
throtl_rb_first(struct throtl_service_queue *parent_sq)
{
	struct rb_node *n;

	n = rb_first_cached(&parent_sq->pending_tree); /* [한국어] pending_tree에서 dispatch 시점이 가장 이른 cgroup 탐색 */
	WARN_ON_ONCE(!n); /* [한국어] pending_tree가 비어 있으면 상태 비일관성 (버그) */
	if (!n) /* [한국어] NVMe로 풀어줄 pending cgroup이 없음 */
		return NULL;
	return rb_entry_tg(n); /* [한국어] rb_node → throtl_grp 변환 후 반환 */
}

/*
 * [한국어]
 * throtl_rb_erase - pending_tree에서 throtl_grp 노드를 제거한다.
 * @n: 제거할 rb_node
 * @parent_sq: 대상 service_queue
 *
 * disptime이 갱신되거나 cgroup이 비면 호출해 트리를 정리한다.
 * 호출 체인: throtl_dequeue_tg(), tg_update_disptime() → [throtl_rb_erase]
 */
static void throtl_rb_erase(struct rb_node *n,
			    struct throtl_service_queue *parent_sq)
{
	rb_erase_cached(n, &parent_sq->pending_tree); /* [한국어] disptime이 도래한 cgroup을 pending_tree에서 제거; NVMe dispatch 라운드 완료 */
	RB_CLEAR_NODE(n); /* [한국어] rb_node 초기화; 이미 트리에서 제거된 노드 표시 */
}

/*
 * [한국어]
 * update_min_dispatch_time - parent_sq의 first_pending_disptime을 갱신한다.
 * @parent_sq: 갱신할 service_queue
 *
 * pending_tree의 leftmost tg의 disptime을 first_pending_disptime에 복사.
 * pending_timer 만료 시점을 이 값 기준으로 arm한다.
 * 호출 체인: throtl_schedule_next_dispatch() → [update_min_dispatch_time]
 */
static void update_min_dispatch_time(struct throtl_service_queue *parent_sq)
{
	struct throtl_grp *tg;

	tg = throtl_rb_first(parent_sq); /* [한국어] pending_tree의 leftmost(가장 이른 disptime) tg 획득 */
	if (!tg) /* [한국어] pending_tree가 비면 갱신 불필요 */
		return;

	parent_sq->first_pending_disptime = tg->disptime; /* [한국어] 가장 임박한 disptime을 부모 service_queue에 기록; NVMe doorbell 타이머 만료 시점 결정 */
}

/*
 * [한국어]
 * tg_service_queue_add - throtl_grp을 부모 service_queue의 pending_tree에 삽입.
 * @tg: 삽입할 throtl_grp (tg->disptime이 RB tree 키)
 *
 * disptime 오름차순 RB tree에 삽입. leftmost면 pending_timer 만료 후보.
 * 호출 체인: throtl_enqueue_tg(), tg_update_disptime() → [tg_service_queue_add]
 */
static void tg_service_queue_add(struct throtl_grp *tg)
{
	struct throtl_service_queue *parent_sq = tg->service_queue.parent_sq; /* [한국어] 부모 service_queue의 pending_tree에 이 cgroup을 삽입 */
	struct rb_node **node = &parent_sq->pending_tree.rb_root.rb_node; /* [한국어] RB tree 삽입 위치 탐색 포인터 */
	struct rb_node *parent = NULL; /* [한국어] 삽입 위치의 부모 노드 */
	struct throtl_grp *__tg;
	unsigned long key = tg->disptime; /* [한국어] disptime을 key로 사용; NVMe SQ로 bio를 풀어줄 시간 기준 정렬 */
	bool leftmost = true; /* [한국어] leftmost 후보 여부; NVMe doorbell 타이머의 다음 만료 대상 판단 */

	while (*node != NULL) { /* [한국어] RB tree 삽입 순회; disptime 기준으로 NVMe dispatch 우선순위 확정 */
		parent = *node;
		__tg = rb_entry_tg(parent);

		if (time_before(key, __tg->disptime)) /* [한국어] 더 이른 disptime이면 왼쪽 서브트리; NVMe doorbell을 먼저 칠 cgroup */
			node = &parent->rb_left;
		else {
			node = &parent->rb_right; /* [한국어] 같거나 늦으면 오른쪽 서브트리; NVMe doorbell 시점이 미래인 cgroup */
			leftmost = false; /* [한국어] leftmost 아님; 이 tg보다 더 이른 disptime이 존재 */
		}
	}

	rb_link_node(&tg->rb_node, parent, node); /* [한국어] cgroup 노드를 pending_tree에 연결; NVMe dispatch 예약 트리 구성 */
	rb_insert_color_cached(&tg->rb_node, &parent_sq->pending_tree, /* [한국어] RB tree 균형 복원; 다음 NVMe SQ 진입 시점 탐색 트리 유지 */
			       leftmost);
}

/*
 * [한국어]
 * throtl_enqueue_tg - throtl_grp을 pending_tree에 등록한다.
 * @tg: 등록할 throtl_grp
 *
 * THROTL_TG_PENDING 플래그가 없는 경우에만 tg_service_queue_add()를 호출.
 * 이미 등록된 tg를 중복 삽입하지 않도록 보호.
 * 호출 체인: throtl_add_bio_tg() → [throtl_enqueue_tg] → tg_service_queue_add()
 */
static void throtl_enqueue_tg(struct throtl_grp *tg)
{
	if (!(tg->flags & THROTL_TG_PENDING)) { /* [한국어] THROTL_TG_PENDING 비트 테스트; 이미 NVMe dispatch 예약된 cgroup인지 확인 */
		tg_service_queue_add(tg); /* [한국어] pending_tree에 disptime 기준으로 삽입 */
		tg->flags |= THROTL_TG_PENDING; /* [한국어] pending_tree에 등록됨 표시; NVMe doorbell 타이머 대상이 됨 */
		tg->service_queue.parent_sq->nr_pending++; /* [한국어] 부모 service_queue의 pending cgroup 수 증가; NVMe SQ로 풀어줄 후보 증가 */
	}
}

/*
 * [한국어]
 * throtl_dequeue_tg - throtl_grp을 pending_tree에서 제거한다.
 * @tg: 제거할 throtl_grp
 *
 * THROTL_TG_PENDING 플래그가 있을 때만 동작. tg의 큐가 비거나 flush 시 호출.
 * 호출 체인: throtl_select_dispatch(), tg_flush_bios() → [throtl_dequeue_tg]
 */
static void throtl_dequeue_tg(struct throtl_grp *tg)
{
	if (tg->flags & THROTL_TG_PENDING) { /* [한국어] THROTL_TG_PENDING 비트 테스트; NVMe dispatch 예약 해제 대상 확인 */
		struct throtl_service_queue *parent_sq =
			tg->service_queue.parent_sq; /* [한국어] 부모 service_queue 획득; pending_tree가 여기에 있음 */

		throtl_rb_erase(&tg->rb_node, parent_sq); /* [한국어] pending_tree에서 제거; 더 이상 NVMe doorbell 타이머의 후보가 아님 */
		--parent_sq->nr_pending; /* [한국어] 부모 pending cgroup 수 감소; NVMe SQ 진입 후보 감소 */
		tg->flags &= ~THROTL_TG_PENDING; /* [한국어] pending 플래그 클리어; NVMe doorbell 예약 해제 완료 */
	}
}

/*
 * [한국어]
 * throtl_schedule_pending_timer - pending_timer를 지정 만료 시점으로 arm한다.
 * @sq: 타이머를 가진 service_queue
 * @expires: 타이머 만료 jiffies
 *
 * 최대 8 슬라이스(800ms) 제한을 두어 동적 limit 변경 시 과도한 지연 방지.
 * queue_lock 보유 상태에서 호출.
 * 호출 체인: throtl_schedule_next_dispatch(), tg_flush_bios() → [throtl_schedule_pending_timer]
 */
/* Call with queue lock held */
static void throtl_schedule_pending_timer(struct throtl_service_queue *sq,
					  unsigned long expires)
{
	unsigned long max_expire = jiffies + 8 * DFL_THROTL_SLICE; /* [한국어] pending_timer 최대 만료 시점을 8 slice로 제한; NVMe doorbell 지연 과다 방지 */

	/*
	 * Since we are adjusting the throttle limit dynamically, the sleep
	 * time calculated according to previous limit might be invalid. It's
	 * possible the cgroup sleep time is very long and no other cgroups
	 * have IO running so notify the limit changes. Make sure the cgroup
	 * doesn't sleep too long to avoid the missed notification.
	 */
	if (time_after(expires, max_expire)) /* [한국어] 동적 제한 변경 시 과도한 NVMe doorbell 지연을 방지하기 위해 만료 시점 상한 조정 */
		expires = max_expire; /* [한국어] 최대 8 슬라이스로 클리핑; 제한 변경 알림을 놓치지 않도록 */
	mod_timer(&sq->pending_timer, expires); /* [한국어] pending_timer 갱신; 실제 NVMe SQ로 bio를 풀어줄 시점 지연/예약 */
	throtl_log(sq, "schedule timer. delay=%lu jiffies=%lu",
		   expires - jiffies, jiffies);
}

/**
 * throtl_schedule_next_dispatch - schedule the next dispatch cycle
 * @sq: the service_queue to schedule dispatch for
 * @force: force scheduling
 *
 * Arm @sq->pending_timer so that the next dispatch cycle starts on the
 * dispatch time of the first pending child.  Returns %true if either timer
 * is armed or there's no pending child left.  %false if the current
 * dispatch window is still open and the caller should continue
 * dispatching.
 *
 * If @force is %true, the dispatch timer is always scheduled and this
 * function is guaranteed to return %true.  This is to be used when the
 * caller can't dispatch itself and needs to invoke pending_timer
 * unconditionally.  Note that forced scheduling is likely to induce short
 * delay before dispatch starts even if @sq->first_pending_disptime is not
 * in the future and thus shouldn't be used in hot paths.
 *
 * NVMe 관점:
 * 다음 NVMe 명령을 SQ에 넣을 시점을 예약한다. pending_tree의 첫 그룹
 * disptime이 미래면 타이머를 arm하고, 현재 jiffies를 지났으면 즉시
 * dispatch를 계속한다. 이 타이머는 컨트롤러의 doorbell 치는 시점을
 * 강제로 늦추는 소프트웨어 타이머 역할을 한다.
 */
static bool throtl_schedule_next_dispatch(struct throtl_service_queue *sq,
					  bool force)
{
	/* any pending children left? */
	if (!sq->nr_pending) /* [한국어] pending_tree에 후보가 없으면 NVMe doorbell 예약 불필요 */
		return true;

	update_min_dispatch_time(sq); /* [한국어] 가장 이른 disptime 갱신; NVMe doorbell 다음 시점 계산 */

	/* is the next dispatch time in the future? */
	if (force || time_after(sq->first_pending_disptime, jiffies)) { /* [한국어] force 또는 disptime이 미래면 NVMe doorbell을 그 시점까지 지연 */
		throtl_schedule_pending_timer(sq, sq->first_pending_disptime); /* [한국어] NVMe SQ로 bio를 풀어줄 시점에 pending_timer arm */
		return true;
	}

	/* tell the caller to continue dispatching */
	return false; /* [한국어] dispatch window가 열려 있음; NVMe SQ로 즉시 추가 bio 전달 가능 */
}

/*
 * [한국어]
 * throtl_start_new_slice_with_credit - 이전 슬라이스 미사용분을 credit으로
 * 이월하며 새 슬라이스를 시작한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @start: credit 시작 기준 jiffies
 *
 * 이전 슬라이스에서 대역폭을 다 쓰지 않은 경우 credit 이월을 통해
 * 유휴 후 burst를 허용. bytes_disp/io_disp는 0으로 초기화.
 * 호출 체인: start_parent_slice_with_credit() → [throtl_start_new_slice_with_credit]
 */
static inline void throtl_start_new_slice_with_credit(struct throtl_grp *tg,
		bool rw, unsigned long start)
{
	tg->bytes_disp[rw] = 0; /* [한국어] 새 slice 시작 시 NVMe 대역폭 사용량 초기화 */
	tg->io_disp[rw] = 0; /* [한국어] 새 slice 시작 시 NVMe 초당 명령 사용량 초기화 */

	/*
	 * Previous slice has expired. We must have trimmed it after last
	 * bio dispatch. That means since start of last slice, we never used
	 * that bandwidth. Do try to make use of that bandwidth while giving
	 * credit.
	 */
	if (time_after(start, tg->slice_start[rw])) /* [한국어] 이전 slice 미사용 기간을 credit으로 이월; NVMe 유휴 후 burst 허용 */
		tg->slice_start[rw] = start; /* [한국어] slice_start를 credit 기준으로 앞당김; 미사용 시간을 예산으로 전환 */

	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* [한국어] 새로운 시간 윈도우 종료 시점; 이 구간 내에서 NVMe 평균 rate 제한 */
	throtl_log(&tg->service_queue,
		   "[%c] new slice with credit start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_start_new_slice - 새로운 rate limit 슬라이스를 시작한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @clear: 사용량(bytes_disp/io_disp)을 0으로 초기화할지 여부
 *
 * slice_start를 현재 jiffies로, slice_end를 jiffies + DFL_THROTL_SLICE로 설정.
 * 호출 체인: tg_update_slice(), tg_conf_updated() → [throtl_start_new_slice]
 */
static inline void throtl_start_new_slice(struct throtl_grp *tg, bool rw,
					  bool clear)
{
	if (clear) {
		tg->bytes_disp[rw] = 0; /* [한국어] slice 사용량 초기화; NVMe SQ로 보낼 수 있는 바이트 예산 리셋 */
		tg->io_disp[rw] = 0; /* [한국어] slice 사용량 초기화; NVMe SQ로 보낼 수 있는 명령 수 예산 리셋 */
	}
	tg->slice_start[rw] = jiffies; /* [한국어] slice 시작 시점; NVMe doorbell rate limit 기준 시간 */
	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* [한국어] slice 종료 시점; 이 시점까지 NVMe 평균 bps/iops 제한 유효 */

	throtl_log(&tg->service_queue,
		   "[%c] new slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_set_slice_end - 슬라이스 종료 시점을 DFL_THROTL_SLICE 단위로 정렬.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @jiffy_end: 목표 종료 jiffies (slice 단위로 올림)
 *
 * 호출 체인: throtl_trim_slice(), throtl_extend_slice(), tg_dispatch_bps_time()
 *           → [throtl_set_slice_end]
 */
static inline void throtl_set_slice_end(struct throtl_grp *tg, bool rw,
					unsigned long jiffy_end)
{
	tg->slice_end[rw] = roundup(jiffy_end, DFL_THROTL_SLICE); /* [한국어] slice 종료 시점을 DFL_THROTL_SLICE 단위로 정렬; NVMe rate limit 윈도우 경계 맞춤 */
}

/*
 * [한국어]
 * throtl_extend_slice - 슬라이스 종료 시점을 jiffy_end까지 연장한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @jiffy_end: 연장할 목표 jiffies
 *
 * 이미 충분히 길면 연장하지 않음. bio 대기 시간이 현재 슬라이스를 넘어서면
 * slice_end를 확장해 다음 슬라이스 시작을 늦춘다.
 * 호출 체인: tg_dispatch_bps_time(), tg_dispatch_iops_time() → [throtl_extend_slice]
 */
static inline void throtl_extend_slice(struct throtl_grp *tg, bool rw,
				       unsigned long jiffy_end)
{
	if (!time_before(tg->slice_end[rw], jiffy_end)) /* [한국어] slice가 이미 충분히 길면 NVMe rate limit 윈도우 연장 불필요 */
		return;

	throtl_set_slice_end(tg, rw, jiffy_end); /* [한국어] NVMe rate limit 시간 윈도우 연장; 다음 bio가 더 늦게 SQ에 도달해도 허용 */
	throtl_log(&tg->service_queue,
		   "[%c] extend slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_slice_used - 현재 슬라이스가 만료되었는지 판단한다.
 * @tg: 확인할 throtl_grp
 * @rw: READ 또는 WRITE
 * @return: 슬라이스가 만료되었으면 true, 아직 유효하면 false
 *
 * jiffies가 [slice_start, slice_end] 범위 안에 있으면 false.
 * 호출 체인: throtl_trim_slice(), tg_update_slice() → [throtl_slice_used]
 */
/* Determine if previously allocated or extended slice is complete or not */
static bool throtl_slice_used(struct throtl_grp *tg, bool rw)
{
	if (time_in_range(jiffies, tg->slice_start[rw], tg->slice_end[rw])) /* [한국어] 현재 jiffy가 slice 범위 내에 있으면 NVMe rate limit 윈도우가 아직 유효 */
		return false;

	return true; /* [한국어] slice 범위 밖이면 만료; 새 slice 시작 필요 */
}

/*
 * [한국어]
 * sq_queued - service_queue의 특정 방향(type)에 대기 중인 bio 총 수를 반환.
 * @sq: 조회할 service_queue
 * @type: READ 또는 WRITE
 * @return: bps 큐 + iops 큐 합산 대기 bio 수
 *
 * 호출 체인: tg_within_limit(), tg_dispatch_time(), throtl_dispatch_tg() → [sq_queued]
 */
static unsigned int sq_queued(struct throtl_service_queue *sq, int type)
{
	return sq->nr_queued_bps[type] + sq->nr_queued_iops[type]; /* [한국어] bps/iops 대기열 합산; NVMe SQ 진입 지연 중인 총 bio 수 */
}

/*
 * [한국어]
 * calculate_io_allowed - 경과 시간 동안 허용되는 IO 수를 계산한다.
 * @iops_limit: iops 상한 (회/초)
 * @jiffy_elapsed: 경과 jiffies
 * @return: 허용 IO 수 (UINT_MAX면 사실상 무제한)
 *
 * iops_limit * jiffy_elapsed / HZ. 오버플로 방지를 위해 do_div 사용.
 * 호출 체인: tg_within_iops_limit(), throtl_trim_iops() → [calculate_io_allowed]
 */
static unsigned int calculate_io_allowed(u32 iops_limit,
					 unsigned long jiffy_elapsed)
{
	unsigned int io_allowed;
	u64 tmp;

	/*
	 * jiffy_elapsed should not be a big value as minimum iops can be
	 * 1 then at max jiffy elapsed should be equivalent of 1 second as we
	 * will allow dispatch after 1 second and after that slice should
	 * have been trimmed.
	 */

	tmp = (u64)iops_limit * jiffy_elapsed; /* [한국어] iops_limit * 경과 시간; NVMe 초당 명령 허용량 계산 */
	do_div(tmp, HZ); /* [한국어] HZ로 나눠 초 단위 변환; NVMe IOPS를 jiffy 기준으로 환산 */

	if (tmp > UINT_MAX)
		io_allowed = UINT_MAX; /* [한국어] 허용량이 UINT_MAX를 넘으면 NVMe IOPS 제한 사실상 무제한 */
	else
		io_allowed = tmp; /* [한국어] 허용량을 unsigned int로 저장 */

	return io_allowed;
}

/*
 * [한국어]
 * calculate_bytes_allowed - 경과 시간 동안 허용되는 바이트 수를 계산한다.
 * @bps_limit: bps 상한 (바이트/초)
 * @jiffy_elapsed: 경과 jiffies
 * @return: 허용 바이트 수 (U64_MAX면 사실상 무제한)
 *
 * bps_limit * jiffy_elapsed / HZ. ilog2 합이 62 초과 시 U64_MAX 반환.
 * 호출 체인: tg_within_bps_limit(), throtl_trim_bps(), __tg_update_carryover()
 *           → [calculate_bytes_allowed]
 */
static u64 calculate_bytes_allowed(u64 bps_limit, unsigned long jiffy_elapsed)
{
	/*
	 * Can result be wider than 64 bits?
	 * We check against 62, not 64, due to ilog2 truncation.
	 */
	if (ilog2(bps_limit) + ilog2(jiffy_elapsed) - ilog2(HZ) > 62)
		return U64_MAX; /* [한국어] bps*경과시간이 64bit를 넘을 수 있으면 NVMe 대역폭 제한 무제한 처리 */
	return mul_u64_u64_div_u64(bps_limit, (u64)jiffy_elapsed, (u64)HZ); /* [한국어] bps * 경과 시간 / HZ; NVMe 대역폭 허용량(바이트) 계산 */
}

/*
 * [한국어]
 * throtl_trim_bps - 경과 시간만큼의 bps 예산을 slice 사용량에서 차감한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @time_elapsed: 차감 기준 경과 jiffies
 * @return: 실제로 차감된 바이트 수
 *
 * bps 제한이 U64_MAX(무제한)이면 0 반환. 경과 시간 허용량과 실제 사용량을
 * 비교해 bytes_disp[rw]를 줄인다.
 * 호출 체인: throtl_trim_slice() → [throtl_trim_bps]
 */
static long long throtl_trim_bps(struct throtl_grp *tg, bool rw,
				 unsigned long time_elapsed)
{
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] cgroup의 READ/WRITE bps 상한; NVMe PCIe/낸드 대역폭 소프트웨어 제한 */
	long long bytes_trim;

	if (bps_limit == U64_MAX) /* [한국어] bps 제한이 없으면 NVMe 대역폭 예산 차감 불필요 */
		return 0;

	/* Need to consider the case of bytes_allowed overflow. */
	bytes_trim = calculate_bytes_allowed(bps_limit, time_elapsed); /* [한국어] 경과 시간 동안 허용된 바이트; NVMe SQ로 이미 디스패치된 양 대비 차감 */
	if (bytes_trim <= 0 || tg->bytes_disp[rw] < bytes_trim) { /* [한국어] 예산 부족 또는 초과 시 NVMe 대역폭 사용량을 0으로 클리어 */
		bytes_trim = tg->bytes_disp[rw]; /* [한국어] 실제 차감량은 현재 사용량으로 제한; NVMe rate 계산 안정화 */
		tg->bytes_disp[rw] = 0; /* [한국어] bps 사용량 0으로 리셋 */
	} else {
		tg->bytes_disp[rw] -= bytes_trim; /* [한국어] 사용량에서 허용량 차감; 남은 NVMe 대역폭 예산 감소 */
	}

	return bytes_trim; /* [한국어] 실제 차감된 바이트 수 반환 */
}

/*
 * [한국어]
 * throtl_trim_iops - 경과 시간만큼의 iops 예산을 slice 사용량에서 차감한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @time_elapsed: 차감 기준 경과 jiffies
 * @return: 실제로 차감된 IO 수
 *
 * iops 제한이 UINT_MAX이면 0 반환. io_disp[rw]를 경과 시간 허용량만큼 줄인다.
 * 호출 체인: throtl_trim_slice() → [throtl_trim_iops]
 */
static int throtl_trim_iops(struct throtl_grp *tg, bool rw,
			    unsigned long time_elapsed)
{
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] cgroup의 READ/WRITE iops 상한; NVMe SQ 초당 명령 수 제한 */
	int io_trim;

	if (iops_limit == UINT_MAX) /* [한국어] iops 제한이 없으면 NVMe 초당 명령 예산 차감 불필요 */
		return 0;

	/* Need to consider the case of io_allowed overflow. */
	io_trim = calculate_io_allowed(iops_limit, time_elapsed); /* [한국어] 경과 시간 동안 허용된 IO 수; NVMe CID/tag 할당 예산 계산 */
	if (io_trim <= 0 || tg->io_disp[rw] < io_trim) { /* [한국어] 예산 부족 시 NVMe 초당 명령 사용량 0 클리어 */
		io_trim = tg->io_disp[rw]; /* [한국어] 실제 차감량은 현재 사용량으로 제한 */
		tg->io_disp[rw] = 0; /* [한국어] iops 사용량 0으로 리셋 */
	} else {
		tg->io_disp[rw] -= io_trim; /* [한국어] 사용량에서 허용 IO 수 차감; 남은 NVMe 초당 명령 예산 감소 */
	}

	return io_trim; /* [한국어] 실제 차감된 IO 수 반환 */
}

/*
 * [한국어]
 * throtl_trim_slice - 오래된 slice 사용량을 정리해 평균 rate를 재조정한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * bio 디스패치 직후 또는 직접 통과 후 호출. 2 슬라이스 이상 경과했을 때만
 * 실제 차감. slice_start를 앞당겨 남은 예산을 재계산한다.
 * 호출 체인: __blk_throtl_bio(), tg_dispatch_one_bio() → [throtl_trim_slice]
 */
/* Trim the used slices and adjust slice start accordingly */
static inline void throtl_trim_slice(struct throtl_grp *tg, bool rw)
{
	unsigned long time_elapsed;
	long long bytes_trim;
	int io_trim;

	BUG_ON(time_before(tg->slice_end[rw], tg->slice_start[rw])); /* [한국어] slice_end가 slice_start보다 과거면 버그; NVMe rate limit 상태 비일관 */

	/*
	 * If bps are unlimited (-1), then time slice don't get
	 * renewed. Don't try to trim the slice if slice is used. A new
	 * slice will start when appropriate.
	 */
	if (throtl_slice_used(tg, rw)) /* [한국어] slice가 만료되면 차감하지 않고 새 slice 시작 시점을 기다림; NVMe rate 윈도우 재설정 */
		return;

	/*
	 * A bio has been dispatched. Also adjust slice_end. It might happen
	 * that initially cgroup limit was very low resulting in high
	 * slice_end, but later limit was bumped up and bio was dispatched
	 * sooner, then we need to reduce slice_end. A high bogus slice_end
	 * is bad because it does not allow new slice to start.
	 */
	throtl_set_slice_end(tg, rw, jiffies + DFL_THROTL_SLICE); /* [한국어] bio가 디스패치되었으므로 slice 종료 시점을 현재 기준으로 재조정; NVMe rate limit 윈도우 보정 */

	time_elapsed = rounddown(jiffies - tg->slice_start[rw], /* [한국어] 경과 시간을 slice 단위로 내림; NVMe rate 계산의 과대 추정 방지 */
				 DFL_THROTL_SLICE);
	/* Don't trim slice until at least 2 slices are used */
	if (time_elapsed < DFL_THROTL_SLICE * 2) /* [한국어] 2 slice 이상 사용된 경우에만 정리; 짧은 NVMe burst를 불필요하게 자르지 않음 */
		return;

	/*
	 * The bio submission time may be a few jiffies more than the expected
	 * waiting time, due to 'extra_bytes' can't be divided in
	 * tg_within_bps_limit(), and also due to timer wakeup delay. In this
	 * case, adjust slice_start will discard the extra wait time, causing
	 * lower rate than expected. Therefore, other than the above rounddown,
	 * one extra slice is preserved for deviation.
	 */
	time_elapsed -= DFL_THROTL_SLICE; /* [한국어] 오차 보정을 위해 한 slice 유지; NVMe doorbell 간 실제 지연과 rate 계산 오차 완충 */
	bytes_trim = throtl_trim_bps(tg, rw, time_elapsed); /* [한국어] bps 예산 정리; NVMe 대역폭 사용량을 시간 경과에 맞춤 */
	io_trim = throtl_trim_iops(tg, rw, time_elapsed); /* [한국어] iops 예산 정리; NVMe 초당 명령 사용량을 시간 경과에 맞춤 */
	if (!bytes_trim && !io_trim) /* [한국어] bps/iops 모두 정리할 양이 없으면 NVMe rate limit 보정 불필요 */
		return;

	tg->slice_start[rw] += time_elapsed; /* [한국어] slice 시작을 앞당겨 남은 예산 재계산; NVMe SQ 유입 rate 재조정 */

	throtl_log(&tg->service_queue,
		   "[%c] trim slice nr=%lu bytes=%lld io=%d start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', time_elapsed / DFL_THROTL_SLICE,
		   bytes_trim, io_trim, tg->slice_start[rw], tg->slice_end[rw],
		   jiffies);
}

/*
 * [한국어]
 * __tg_update_carryover - 설정 변경 시 이전 제한 하에서 대기한 양을 보정한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @bytes: 출력: bps carryover 바이트 수
 * @ios: 출력: iops carryover IO 수
 *
 * cgroup의 bps/iops 설정이 바뀔 때 이미 대기 중인 bio들의 손해/이익을
 * 새 기준에 반영. 큐가 비었으면 bytes_disp/io_disp를 0으로 초기화.
 * 호출 체인: tg_update_carryover() → [__tg_update_carryover]
 */
static void __tg_update_carryover(struct throtl_grp *tg, bool rw,
				  long long *bytes, int *ios)
{
	unsigned long jiffy_elapsed = jiffies - tg->slice_start[rw]; /* [한국어] 현재 slice에서 경과한 jiffy; NVMe rate limit 변경 시 누적 대기량 산정 */
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] 새 bps 기준 하에서 이미 기다린 시간 환산; NVMe SQ 진입 지연 손실 보정 */
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] 새 iops 기준 하에서 이미 기다린 시간 환산; NVMe 초당 명령 지연 보정 */
	long long bytes_allowed;
	int io_allowed;

	/*
	 * If the queue is empty, carryover handling is not needed. In such cases,
	 * tg->[bytes/io]_disp should be reset to 0 to avoid impacting the dispatch
	 * of subsequent bios. The same handling applies when the previous BPS/IOPS
	 * limit was set to max.
	 */
	if (sq_queued(&tg->service_queue, rw) == 0) { /* [한국어] 큐가 비어 있으면 carryover 불필요; NVMe SQ로 진입 예정인 bio가 없음 */
		tg->bytes_disp[rw] = 0; /* [한국어] bps 사용량 초기화; 새 NVMe rate limit 기준에서 예산 재시작 */
		tg->io_disp[rw] = 0; /* [한국어] iops 사용량 초기화; 새 NVMe rate limit 기준에서 예산 재시작 */
		return;
	}

	/*
	 * If config is updated while bios are still throttled, calculate and
	 * accumulate how many bytes/ios are waited across changes. And use the
	 * calculated carryover (@bytes/@ios) to update [bytes/io]_disp, which
	 * will be used to calculate new wait time under new configuration.
	 * And we need to consider the case of bytes/io_allowed overflow.
	 */
	if (bps_limit != U64_MAX) {
		bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed); /* [한국어] 새 bps 기준 경과 시간 허용량; NVMe 대역폭 제한 변경 후 잔여/초과량 계산 */
		if (bytes_allowed > 0)
			*bytes = bytes_allowed - tg->bytes_disp[rw]; /* [한국어] 허용량 - 실제 사용량 = carryover; 양수면 이득, 음수면 빚 */
	}
	if (iops_limit != UINT_MAX) {
		io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed); /* [한국어] 새 iops 기준 경과 시간 허용량; NVMe 초당 명령 제한 변경 후 잔여/초과량 계산 */
		if (io_allowed > 0)
			*ios = io_allowed - tg->io_disp[rw]; /* [한국어] 허용 IO - 실제 IO = carryover */
	}

	tg->bytes_disp[rw] = -*bytes; /* [한국어] 새 bps 기준에서의 잔여/초과 허용량; 음수면 이미 초과 사용 */
	tg->io_disp[rw] = -*ios;      /* [한국어] 새 iops 기준에서의 잔여/초과 허용량 */
}

/*
 * [한국어]
 * tg_update_carryover - READ/WRITE 양쪽에 대해 carryover를 갱신한다.
 * @tg: 대상 throtl_grp
 *
 * tg_set_conf() 또는 tg_set_limit()에서 limit 변경 직전에 호출.
 * 호출 체인: tg_set_conf(), tg_set_limit() → [tg_update_carryover] → __tg_update_carryover()
 */
static void tg_update_carryover(struct throtl_grp *tg)
{
	long long bytes[2] = {0}; /* [한국어] READ/WRITE bps carryover 임시 저장 */
	int ios[2] = {0}; /* [한국어] READ/WRITE iops carryover 임시 저장 */

	__tg_update_carryover(tg, READ, &bytes[READ], &ios[READ]); /* [한국어] READ 방향 carryover 계산 및 bytes_disp/io_disp 업데이트 */
	__tg_update_carryover(tg, WRITE, &bytes[WRITE], &ios[WRITE]); /* [한국어] WRITE 방향 carryover 계산 및 bytes_disp/io_disp 업데이트 */

	/* see comments in struct throtl_grp for meaning of carryover. */
	throtl_log(&tg->service_queue, "%s: %lld %lld %d %d\n", __func__,
		   bytes[READ], bytes[WRITE], ios[READ], ios[WRITE]); /* [한국어] carryover 결과 blktrace 로그 */
}

/*
 * [한국어]
 * tg_within_iops_limit - bio 하나가 현재 iops slice 안에 들어갈 수 있는지 검사.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @iops_limit: iops 상한
 * @return: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 *
 * io_disp[rw] + 1 <= 허용 IO 수이면 0 반환. 초과 시 다음 슬라이스 경계까지의
 * 시간을 반환. 최소 1 IO 보장 로직 포함.
 * 호출 체인: tg_dispatch_iops_time(), tg_within_limit() → [tg_within_iops_limit]
 */
static unsigned long tg_within_iops_limit(struct throtl_grp *tg, struct bio *bio,
				 u32 iops_limit)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향; NVMe SQ/CQ 방향과 일치 */
	int io_allowed;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;

	jiffy_elapsed = jiffies - tg->slice_start[rw]; /* [한국어] 현재 slice에서 경과한 시간; NVMe IOPS 윈도우 내 허용 명령 수 계산 */

	/* Round up to the next throttle slice, wait time must be nonzero */
	jiffy_elapsed_rnd = roundup(jiffy_elapsed + 1, DFL_THROTL_SLICE); /* [한국어] 다음 slice 경계로 올림; NVMe IOPS 계산에서 0으로 나누기 및 무한대 방지 */
	io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed_rnd); /* [한국어] 현재까지 허용된 IO 수; NVMe CID 할당 가능 여부 판단 */
	if (io_allowed > 0 && tg->io_disp[rw] + 1 <= io_allowed) /* [한국어] 사용량+1이 허용량 이하면 이 bio를 즉시 NVMe SQ로 보낼 수 있음 */
		return 0;

	/* Calc approx time to dispatch */
	jiffy_wait = jiffy_elapsed_rnd - jiffy_elapsed; /* [한국어] 다음 slice 경계까지 남은 시간; NVMe doorbell 지연 시간 */

	/* make sure at least one io can be dispatched after waiting */
	jiffy_wait = max(jiffy_wait, HZ / iops_limit + 1); /* [한국어] 최소 1 IO 이후 디스패치 보장; NVMe IOPS 제한이 매우 낮을 때도 진행 */
	return jiffy_wait;
}

/*
 * [한국어]
 * tg_within_bps_limit - bio 하나가 현재 bps slice 안에 들어갈 수 있는지 검사.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @bps_limit: bps 상한
 * @return: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 *
 * bytes_disp[rw] + bio_size <= 허용 바이트이면 0 반환. 초과 시 대기 시간 계산.
 * 슬라이스 시작 직후에는 DFL_THROTL_SLICE를 기준으로 계산.
 * 호출 체인: tg_dispatch_bps_time(), tg_within_limit() → [tg_within_bps_limit]
 */
static unsigned long tg_within_bps_limit(struct throtl_grp *tg, struct bio *bio,
				u64 bps_limit)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio 방향; NVMe SQ/CQ READ/WRITE 구분 */
	long long bytes_allowed;
	u64 extra_bytes;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;
	unsigned int bio_size = throtl_bio_data_size(bio); /* [한국어] bio 크기; NVMe PRP/SGL segment 수와 doorbell 당 전송량에 영향 */

	jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw]; /* [한국어] slice 시작부터 경과 시간; NVMe 대역폭 윈도우 계산 */

	/* Slice has just started. Consider one slice interval */
	if (!jiffy_elapsed) /* [한국어] slice 시작 직후면 한 slice 간격으로 간주; NVMe 초기 burst 처리 */
		jiffy_elapsed_rnd = DFL_THROTL_SLICE; /* [한국어] 최소 100ms 윈도우 사용; 0으로 나누기 방지 */

	jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, DFL_THROTL_SLICE); /* [한국어] slice 경계로 올림; NVMe bps 계산에서 안정적 시간 윈도우 확보 */
	bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed_rnd); /* [한국어] 현재 윈도우에서 허용된 총 바이트; NVMe 대역폭 예산 */
	/* Need to consider the case of bytes_allowed overflow. */
	if ((bytes_allowed > 0 && tg->bytes_disp[rw] + bio_size <= bytes_allowed) /* [한국어] bio 추가가 허용량 내이면 즉시 NVMe SQ로 디스패치 가능 */
	    || bytes_allowed < 0) /* [한국어] bytes_allowed 오버플로(음수)이면 사실상 무제한으로 처리 */
		return 0;

	/* Calc approx time to dispatch */
	extra_bytes = tg->bytes_disp[rw] + bio_size - bytes_allowed; /* [한국어] 허용량을 초과하는 바이트; NVMe 대역폭 제한 때문에 기다려야 할 양 */
	jiffy_wait = div64_u64(extra_bytes * HZ, bps_limit); /* [한국어] 초과 바이트 / bps = 대기 시간; NVMe doorbell 지연 시간 계산 */

	if (!jiffy_wait) /* [한국어] 최소 1 jiffy 대기; NVMe rate limit이 극단적일 때도 제약 유지 */
		jiffy_wait = 1;

	/*
	 * This wait time is without taking into consideration the rounding
	 * up we did. Add that time also.
	 */
	jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed); /* [한국어] slice 올림분을 추가; NVMe rate limit 시점 정렬에 따른 지연 반영 */
	return jiffy_wait;
}

/*
 * [한국어]
 * throtl_charge_bps_bio - bio의 바이트를 bps 사용량(bytes_disp)에 기록한다.
 * @tg: 대상 throtl_grp
 * @bio: 과금할 bio
 *
 * BIO_BPS_THROTTLED 또는 BIO_TG_BPS_THROTTLED가 이미 설정된 분할 bio는
 * 중복 과금하지 않음. BIO_TG_BPS_THROTTLED 플래그를 설정해 iops 경로로만 가도록.
 * 호출 체인: tg_dispatch_time(), tg_within_limit(), tg_dispatch_one_bio()
 *           → [throtl_charge_bps_bio]
 */
static void throtl_charge_bps_bio(struct throtl_grp *tg, struct bio *bio)
{
	unsigned int bio_size = throtl_bio_data_size(bio); /* [한국어] bio 크기; NVMe PRP/SGL 및 DMA 전송량에 직접 연결 */

	/* Charge the bio to the group */
	if (!bio_flagged(bio, BIO_BPS_THROTTLED) && /* [한국어] 이미 bps 제한을 통과한 분할 bio는 중복 차감하지 않음 (NVMe 명령 중복 계산 방지) */
	    !bio_flagged(bio, BIO_TG_BPS_THROTTLED)) {
		bio_set_flag(bio, BIO_TG_BPS_THROTTLED); /* [한국어] bps 제한 통과 표시; 이후 iops 경로로만 NVMe SQ 진입 검사 */
		tg->bytes_disp[bio_data_dir(bio)] += bio_size; /* [한국어] NVMe로 풀려나는 바이트 기록 */
	}
}

/*
 * [한국어]
 * throtl_charge_iops_bio - bio 하나를 iops 사용량(io_disp)에 기록한다.
 * @tg: 대상 throtl_grp
 * @bio: 과금할 bio
 *
 * io_disp[rw]를 1 증가시키고 BIO_TG_BPS_THROTTLED 플래그를 클리어.
 * 호출 체인: tg_dispatch_one_bio(), __blk_throtl_bio() → [throtl_charge_iops_bio]
 */
static void throtl_charge_iops_bio(struct throtl_grp *tg, struct bio *bio)
{
	bio_clear_flag(bio, BIO_TG_BPS_THROTTLED); /* [한국어] bps 통과 플래그 클리어; iops 단계에서 NVMe 초당 명령 수만 계산 */
	tg->io_disp[bio_data_dir(bio)]++; /* [한국어] NVMe CID 하나에 해당하는 IO 사용량 증가 */
}

/*
 * [한국어]
 * tg_update_slice - bio 디스패치 직전 슬라이스 상태를 갱신한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * 슬라이스가 만료되고 큐가 비면 새 슬라이스 시작, 그 외엔 연장.
 * 호출 체인: tg_dispatch_bps_time(), tg_dispatch_iops_time() → [tg_update_slice]
 */
static void tg_update_slice(struct throtl_grp *tg, bool rw)
{
	if (throtl_slice_used(tg, rw) && /* [한국어] slice가 만료되고 큐가 비면 새로운 NVMe rate limit 윈도우 시작 */
	    sq_queued(&tg->service_queue, rw) == 0)
		throtl_start_new_slice(tg, rw, true); /* [한국어] 새 slice에서 예산 초기화; NVMe SQ 유입 rate 재설정 */
	else
		throtl_extend_slice(tg, rw, jiffies + DFL_THROTL_SLICE); /* [한국어] 현재 slice를 연장; NVMe rate limit 윈도우 내에서 계속 bio 허용 */
}

/*
 * [한국어]
 * tg_dispatch_bps_time - bio가 bps 제한을 만족하는지 검사하고 대기 시간 반환.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @return: 대기 jiffies. 0이면 bps 제한 통과.
 *
 * bps 무제한/THROTL_TG_CANCELING/이미 통과한 bio는 즉시 0 반환.
 * 슬라이스 갱신 후 tg_within_bps_limit()으로 검사.
 * 호출 체인: tg_dispatch_time(), tg_within_limit() → [tg_dispatch_bps_time]
 */
static unsigned long tg_dispatch_bps_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향 */
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] cgroup의 READ/WRITE bps 상한; NVMe PCIe/낸드 대역폭 제한 */
	unsigned long bps_wait;

	/* no need to throttle if this bio's bytes have been accounted */
	if (bps_limit == U64_MAX || tg->flags & THROTL_TG_CANCELING || /* [한국어] bps 무제한/취소 중/이미 통과한 bio는 bps 검사 생략 후 NVMe SQ 진입 */
	    bio_flagged(bio, BIO_BPS_THROTTLED) ||
	    bio_flagged(bio, BIO_TG_BPS_THROTTLED))
		return 0;

	tg_update_slice(tg, rw); /* [한국어] slice 상태 갱신; NVMe rate limit 시간 윈도우 준비 */
	bps_wait = tg_within_bps_limit(tg, bio, bps_limit); /* [한국어] bps 제한 위반 시 대기 시간; NVMe doorbell 지연량 */
	throtl_extend_slice(tg, rw, jiffies + bps_wait); /* [한국어] 대기 시간만큼 slice 종료 연장; NVMe SQ 유입 시점 지연 */

	return bps_wait;
}

/*
 * [한국어]
 * tg_dispatch_iops_time - bio가 iops 제한을 만족하는지 검사하고 대기 시간 반환.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @return: 대기 jiffies. 0이면 iops 제한 통과.
 *
 * iops 무제한/THROTL_TG_CANCELING이면 즉시 0 반환.
 * 슬라이스 갱신 후 tg_within_iops_limit()으로 검사.
 * 호출 체인: tg_dispatch_time(), tg_within_limit() → [tg_dispatch_iops_time]
 */
static unsigned long tg_dispatch_iops_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향 */
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] cgroup의 READ/WRITE iops 상한; NVMe SQ 초당 명령 수 제한 */
	unsigned long iops_wait;

	if (iops_limit == UINT_MAX || tg->flags & THROTL_TG_CANCELING) /* [한국어] iops 무제한/취소 중이면 iops 검사 생략 후 NVMe SQ 진입 */
		return 0;

	tg_update_slice(tg, rw); /* [한국어] slice 상태 갱신; NVMe iops rate limit 시간 윈도우 준비 */
	iops_wait = tg_within_iops_limit(tg, bio, iops_limit); /* [한국어] iops 제한 위반 시 대기 시간; NVMe doorbell 지연량 */
	throtl_extend_slice(tg, rw, jiffies + iops_wait); /* [한국어] 대기 시간만큼 slice 연장; NVMe 초당 명령 유입 지연 */

	return iops_wait;
}

/*
 * [한국어]
 * tg_dispatch_time - bio가 현재 tg에서 디스패치 가능할 때까지의 대기 시간 계산.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio (큐의 첫 번째 bio여야 함)
 * @return: 대기 jiffies. 0이면 즉시 디스패치 가능.
 *
 * bps를 먼저 검사하고 통과 시 bps 과금 후 iops를 검사. 큐에 bio가 있으면
 * 큐의 첫 bio와 동일한 bio인지 BUG_ON으로 검증.
 * 호출 체인: throtl_dispatch_tg(), tg_update_disptime() → [tg_dispatch_time]
 *   → tg_dispatch_bps_time() → tg_dispatch_iops_time()
 *
 * tg_dispatch_time: bio가 디스패치 가능한 시점까지의 대기 시간을 계산.
 * 호출 경로: tg_update_disptime() -> tg_dispatch_time();
 *           throtl_dispatch_tg() -> tg_dispatch_time().
 * NVMe 연결: bio -> request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * 경로로 가기 전, bps 제한을 먼저 검사하고 통과하면 iops 제한을 검사.
 * 둘 중 하나라도 초과하면 jiffies 단위 대기 시간을 반환해 NVMe 명령
 * 생성을 늦춘다.
 */
static unsigned long tg_dispatch_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio);
	unsigned long wait;

	/*
 	 * Currently whole state machine of group depends on first bio
	 * queued in the group bio list. So one should not be calling
	 * this function with a different bio if there are other bios
	 * queued.
	 */
	BUG_ON(sq_queued(&tg->service_queue, rw) && /* [한국어] qnode의 첫 bio가 아니면 tg_dispatch_time() 상태 가정이 깨짐; NVMe dispatch 순서 보장 */
	       bio != throtl_peek_queued(&tg->service_queue.queued[rw]));

	wait = tg_dispatch_bps_time(tg, bio); /* [한국어] 먼저 bps 제한 검사; 통과하면 iops 제한 검사로 NVMe 명령 수 제한 */
	if (wait != 0) /* [한국어] bps 대기 시간이 0이 아니면 NVMe SQ 진입 지연 */
		return wait;

	/*
	 * Charge bps here because @bio will be directly placed into the
	 * iops queue afterward.
	 */
	throtl_charge_bps_bio(tg, bio); /* [한국어] bps 통과 후 사용량 기록; 이제 iops 제한만 남음 */

	return tg_dispatch_iops_time(tg, bio); /* [한국어] iops 제한 통과 시 0, 실패 시 NVMe doorbell 지연 시간 */
}

/**
 * throtl_add_bio_tg - add a bio to the specified throtl_grp
 * @bio: bio to add
 * @qn: qnode to use
 * @tg: the target throtl_grp
 *
 * Add @bio to @tg's service_queue using @qn.  If @qn is not specified,
 * tg->qnode_on_self[] is used.
 *
 * NVMe 관점:
 * rate limit을 초과한 bio는 이 함수를 통해 throtl 큐에 들어가며, NVMe
 * SQ에 도달하는 시점이 지연된다. THROTL_TG_WAS_EMPTY 플래그는 비어있던
 * 큐에 첫 bio가 들어왔음을 표시해, disptime을 즉시 다시 계산하고
 * pending_timer를 재설정하게 한다.
 */
static void throtl_add_bio_tg(struct bio *bio, struct throtl_qnode *qn,
			      struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	bool rw = bio_data_dir(bio); /* bio의 READ/WRITE 방향; NVMe SQ/CQ 방향 */

	if (!qn) /* 호출자가 특정 qnode를 지정하지 않으면 자신의 qnode 사용 */
		qn = &tg->qnode_on_self[rw]; /* 자신의 service_queue qnode; NVMe 진입 전 bio 대기 위치 */

	/*
	 * If @tg doesn't currently have any bios queued in the same
	 * direction, queueing @bio can change when @tg should be
	 * dispatched.  Mark that @tg was empty.  This is automatically
	 * cleared on the next tg_update_disptime().
	 */
	if (sq_queued(sq, rw) == 0) /* [한국어] 같은 방향의 큐가 비어 있었다면 첫 bio 도착; NVMe dispatch 시점 재계산 필요 */
		tg->flags |= THROTL_TG_WAS_EMPTY; /* [한국어] NVMe로 갈 bio가 생겼음을 표시 */

	throtl_qnode_add_bio(bio, qn, sq); /* [한국어] bio를 throtl 큐에 추가; NVMe SQ로의 유입을 일시 지연 */

	/*
	 * Since we have split the queues, when the iops queue is
	 * previously empty and a new @bio is added into the first @qn,
	 * we also need to update the @tg->disptime.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED) && /* [한국어] 분할 bio가 bps 큐에서 iops 큐로 넘어올 때 첫 bio라면 dispatch 시점 갱신 */
	    bio == throtl_peek_queued(&sq->queued[rw]))
		tg->flags |= THROTL_TG_IOPS_WAS_EMPTY; /* [한국어] iops 큐도 갱신 필요 */

	throtl_enqueue_tg(tg); /* [한국어] disptime 기준 pending_tree에 등록 */
}

/*
 * [한국어]
 * tg_update_disptime - 그룹의 다음 디스패치 시각을 계산하고 pending_tree를
 * 재정렬한다.
 * @tg: 대상 throtl_grp
 *
 * READ/WRITE 큐의 첫 bio에 대해 tg_dispatch_time()을 호출.
 * 반환된 대기 시간만큼 NVMe doorbell을 미룬다. disptime이 가장 작은
 * 그룹이 pending_tree의 leftmost가 되어 다음 타이머 만료 시점이 된다.
 * 호출 체인: __blk_throtl_bio(), throtl_dispatch_tg(), tg_flush_bios()
 *           → [tg_update_disptime] → tg_dispatch_time()
 */
static void tg_update_disptime(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; READ/WRITE 큐 포함 */
	unsigned long read_wait = -1, write_wait = -1, min_wait, disptime; /* [한국어] READ/WRITE 큐의 첫 bio별 대기 시간; NVMe SQ 진입 지연 시간 */
	struct bio *bio;

	bio = throtl_peek_queued(&sq->queued[READ]); /* [한국어] READ 큐의 첫 bio; NVMe READ doorbell 시점 결정 */
	if (bio)
		read_wait = tg_dispatch_time(tg, bio); /* [한국어] READ 큐 첫 bio의 NVMe 진입 대기 시간 */

	bio = throtl_peek_queued(&sq->queued[WRITE]); /* [한국어] WRITE 큐의 첫 bio; NVMe WRITE doorbell 시점 결정 */
	if (bio)
		write_wait = tg_dispatch_time(tg, bio); /* [한국어] WRITE 큐 첫 bio의 NVMe 진입 대기 시간 */

	min_wait = min(read_wait, write_wait); /* [한국어] READ/WRITE 중 더 짧은 대기 시간; 다음 NVMe doorbell 시점 */
	disptime = jiffies + min_wait; /* [한국어] 현재 jiffies + min_wait; 실제 NVMe SQ로 bio를 풀어줄 시점 */

	/* Update dispatch time */
	throtl_rb_erase(&tg->rb_node, tg->service_queue.parent_sq); /* [한국어] pending_tree에서 기존 위치 제거 후 disptime 갱신; NVMe doorbell 예약 재정렬 */
	tg->disptime = disptime; /* [한국어] cgroup의 다음 NVMe SQ 진입 시각 확정 */
	tg_service_queue_add(tg); /* [한국어] 갱신된 disptime으로 pending_tree 재삽입; NVMe dispatch 우선순위 갱신 */

	/* see throtl_add_bio_tg() */
	tg->flags &= ~THROTL_TG_WAS_EMPTY; /* [한국어] WAS_EMPTY 플래그 클리어; NVMe dispatch 시점 재계산 완료 */
	tg->flags &= ~THROTL_TG_IOPS_WAS_EMPTY; /* [한국어] IOPS_WAS_EMPTY 플래그 클리어; NVMe iops 타이머 재계산 완료 */
}

/*
 * [한국어]
 * start_parent_slice_with_credit - 부모 tg의 슬라이스를 credit 포함 재시작한다.
 * @child_tg: 자식 throtl_grp (슬라이스 시작 시점 참고용)
 * @parent_tg: 부모 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * 부모 슬라이스가 만료됐을 때만 credit 이월 재시작. 자식의 slice_start를
 * 부모의 새 시작 시점으로 사용.
 * 호출 체인: tg_dispatch_one_bio() → [start_parent_slice_with_credit]
 */
static void start_parent_slice_with_credit(struct throtl_grp *child_tg,
					struct throtl_grp *parent_tg, bool rw)
{
	if (throtl_slice_used(parent_tg, rw)) { /* [한국어] 부모 slice가 만료되면 credit과 함께 새 slice 시작; NVMe 대역폭 상속 */
		throtl_start_new_slice_with_credit(parent_tg, rw, /* [한국어] 자식 slice 시작 시점을 credit 기준으로 부모 slice 시작; NVMe QoS 상속 */
				child_tg->slice_start[rw]);
	}

}

/*
 * tg_dispatch_one_bio: 한 bio를 현재 throtl_grp에서 부모 service_queue로
 * 전달한다.
 * 호출 경로: throtl_dispatch_tg() -> tg_dispatch_one_bio() ->
 *           throtl_add_bio_tg() 또는 throtl_qnode_add_bio().
 * NVMe 연결: 자식 cgroup에서 상위 cgroup으로 bio가 올라가며, 최상위
 * (td->service_queue)에 도달해야만 blk_throtl_dispatch_work_fn()을 통해
 * submit_bio_noacct_nocheck() -> blk_mq_submit_bio() -> ... ->
 * nvme_queue_rq() -> nvme_submit_cmd(doorbell) 경로로 진입.
 */
static void tg_dispatch_one_bio(struct throtl_grp *tg, bool rw)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; bio pop 위치 */
	struct throtl_service_queue *parent_sq = sq->parent_sq; /* [한국어] 현재 cgroup의 부모 service_queue */
	struct throtl_grp *parent_tg = sq_to_tg(parent_sq); /* [한국어] 부모 service_queue가 throtl_grp이면 상위 cgroup, 없으면 최상위(장치) */
	struct throtl_grp *tg_to_put = NULL; /* [한국어] bio 전달 후 blkg_put()할 tg; pop 중 참조 해제 방지 */
	struct bio *bio;

	/*
	 * @bio is being transferred from @tg to @parent_sq.  Popping a bio
	 * from @tg may put its reference and @parent_sq might end up
	 * getting released prematurely.  Remember the tg to put and put it
	 * after @bio is transferred to @parent_sq.
	 */
	bio = throtl_pop_queued(sq, &tg_to_put, rw); /* [한국어] bio를 현재 cgroup 큐에서 꺼냄; NVMe SQ 진입 후보 확정 */

	throtl_charge_iops_bio(tg, bio); /* [한국어] iops 사용량 기록; NVMe SQ에 들어갈 하나의 명령으로 과금 */

	/*
	 * If our parent is another tg, we just need to transfer @bio to
	 * the parent using throtl_add_bio_tg().  If our parent is
	 * @td->service_queue, @bio is ready to be issued.  Put it on its
	 * bio_lists[] and decrease total number queued.  The caller is
	 * responsible for issuing these bios.
	 */
	if (parent_tg) { /* [한국어] 상위 cgroup이 있으면 그곳으로 전달; NVMe SQ 진입 전 추가 제한 검사 */
		throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg); /* [한국어] 부모 service_queue의 qnode로 bio 추가; 상위 cgroup의 NVMe rate limit 큐로 */
		start_parent_slice_with_credit(tg, parent_tg, rw); /* [한국어] 부모 slice에 credit 이월; NVMe 대역폭 제한의 하위 cgroup 합산 반영 */
	} else {
		bio_set_flag(bio, BIO_BPS_THROTTLED); /* [한국어] 최상위 도달: bps 제한 통과 표시 */
		throtl_qnode_add_bio(bio, &tg->qnode_on_parent[rw],
				     parent_sq); /* [한국어] 최상위 throtl 큐에 추가; NVMe SQ 진입 직전 대기열 */
		BUG_ON(tg->td->nr_queued[rw] <= 0); /* [한국어] 최상위 대기 카운트가 0 이하이면 상태 비일관 (버그) */
		tg->td->nr_queued[rw]--; /* [한국어] NVMe SQ로 나가기 직전의 대기 카운트 감소 */
	}

	throtl_trim_slice(tg, rw); /* [한국어] bio 전달 후 slice 정리; NVMe rate limit 시간 윈도우 보정 */

	if (tg_to_put) /* [한국어] tg_to_put이 설정되면 bio 전달 후 blkg reference 해제 */
		blkg_put(tg_to_blkg(tg_to_put)); /* [한국어] blkg reference 해제; NVMe 진행 중이 아닌 cgroup 자원 정리 */
}

/*
 * [한국어]
 * throtl_dispatch_tg - 한 throtl_grp에서 제한을 통과한 bio들을 부모로 이동.
 * @tg: 대상 throtl_grp
 * @return: 이번 라운드에서 총 디스패치한 bio 수
 *
 * READ 75%(max 6개), WRITE 25%(max 2개) 비율로 한 라운드 최대
 * THROTL_GRP_QUANTUM(8)개 bio를 tg_dispatch_one_bio()로 상위로 전달.
 * tg_dispatch_time()이 0인 bio만 디스패치.
 * 호출 체인: throtl_select_dispatch() → [throtl_dispatch_tg] → tg_dispatch_one_bio()
 */
static int throtl_dispatch_tg(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; READ/WRITE 대기열 접근 */
	unsigned int nr_reads = 0, nr_writes = 0; /* [한국어] 한 라운드에서 디스패치할 READ/WRITE 카운터; NVMe SQ/CQ 방향별 limit */
	unsigned int max_nr_reads = THROTL_GRP_QUANTUM * 3 / 4; /* [한국어] 한 라운드 최대 READ 수: 75%; NVMe READ가 WRITE보다 우선권을 갖는 휴리스틱 */
	unsigned int max_nr_writes = THROTL_GRP_QUANTUM - max_nr_reads; /* [한국어] WRITE는 나머지 25%; NVMe WRITE SQ starvation 완화 */
	struct bio *bio;

	/* Try to dispatch 75% READS and 25% WRITES */

	while ((bio = throtl_peek_queued(&sq->queued[READ])) && /* [한국어] READ 큐의 첫 bio를 순회; NVMe READ SQ 후보 bio 나열 */
	       tg_dispatch_time(tg, bio) == 0) { /* [한국어] tg_dispatch_time이 0이면 READ bio를 NVMe SQ로 보낼 준비 완료 */

		tg_dispatch_one_bio(tg, READ); /* [한국어] READ bio를 NVMe 경로쪽으로 한 단계 전진 */
		nr_reads++; /* [한국어] READ 디스패치 카운터 증가 */

		if (nr_reads >= max_nr_reads) /* [한국어] READ quantum 소진 시 중단; NVMe READ SQ가 한 cgroup에 쏠리지 않도록 */
			break;
	}

	while ((bio = throtl_peek_queued(&sq->queued[WRITE])) && /* [한국어] WRITE 큐의 첫 bio를 순회; NVMe WRITE SQ 후보 bio 나열 */
	       tg_dispatch_time(tg, bio) == 0) { /* [한국어] tg_dispatch_time이 0이면 WRITE bio를 NVMe SQ로 보낼 준비 완료 */

		tg_dispatch_one_bio(tg, WRITE); /* [한국어] WRITE bio를 NVMe 경로쪽으로 한 단계 전진 */
		nr_writes++; /* [한국어] WRITE 디스패치 카운터 증가 */

		if (nr_writes >= max_nr_writes) /* [한국어] WRITE quantum 소진 시 중단; NVMe WRITE SQ가 한 cgroup에 쏠리지 않도록 */
			break;
	}

	return nr_reads + nr_writes; /* 이번 라운드에서 총 디스패치한 bio 수; NVMe SQ 유입 배치 크기 */
}

/*
 * throtl_select_dispatch: pending_tree에 있는 그룹들 중 disptime이 지난
 * 그룹을 순회하며 bio를 디스패치.
 * NVMe 연결: disptime이 도래한 cgroup부터 NVMe에 보낼 bio를 뽑아내어
 * 한 라운드에 최대 THROTL_QUANTUM개 bio를 상위로 전달. 이는 NVMe SQ가
 * 특정 cgroup에 의해 독점되는 것을 막는 round-robin 스케줄링 역할을 한다.
 */
static int throtl_select_dispatch(struct throtl_service_queue *parent_sq)
{
	unsigned int nr_disp = 0; /* [한국어] 이번 라운드 총 디스패치 카운트; NVMe SQ batch 크기 추적 */

	while (1) { /* [한국어] disptime이 도래한 cgroup을 순회하며 NVMe SQ로 bio 풀기 */
		struct throtl_grp *tg;
		struct throtl_service_queue *sq;

		if (!parent_sq->nr_pending) /* [한국어] pending_tree에 후보가 없으면 NVMe SQ로 보낼 bio 없음 */
			break;

		tg = throtl_rb_first(parent_sq); /* [한국어] disptime이 가장 이른 cgroup 획득; 다음 NVMe doorbell 대상 */
		if (!tg) /* [한국어] 예외적으로 pending_tree가 비면 중단 */
			break;

		if (time_before(jiffies, tg->disptime)) /* [한국어] disptime이 아직 미래면 NVMe doorbell을 아직 치지 않음 */
			break; /* [한국어] 아직 NVMe로 풀어줄 시각이 아님 */

		nr_disp += throtl_dispatch_tg(tg); /* [한국어] 해당 cgroup에서 제한을 통과한 bio를 상위/최상위로 이동; NVMe SQ 배치 구성 */

		sq = &tg->service_queue; /* [한국어] tg의 service_queue 접근 */
		if (sq_queued(sq, READ) || sq_queued(sq, WRITE)) /* [한국어] READ나 WRITE 큐에 bio가 남아 있으면 다음 NVMe doorbell 시점 재계산 필요 */
			tg_update_disptime(tg); /* [한국어] 잔여 bio가 있으면 다음 NVMe doorbell 시각 갱신 */
		else /* [한국어] 큐가 비면 pending_tree에서 제거; 더 이상 NVMe doorbell 대상 아님 */
			throtl_dequeue_tg(tg); /* [한국어] 큐가 비었으면 pending_tree에서 제거 */

		if (nr_disp >= THROTL_QUANTUM) /* [한국어] 한 라운드 총 bio 수가 THROTL_QUANTUM에 도달하면 중단; NVMe SQ batch 크기 제한 */
			break;
	}

	return nr_disp; /* [한국어] 이번 라운드에서 NVMe SQ로 풀어준 총 bio 수 반환 */
}

/**
 * throtl_pending_timer_fn - timer function for service_queue->pending_timer
 * @t: the pending_timer member of the throtl_service_queue being serviced
 *
 * This timer is armed when a child throtl_grp with active bio's become
 * pending and queued on the service_queue's pending_tree and expires when
 * the first child throtl_grp should be dispatched.  This function
 * dispatches bio's from the children throtl_grps to the parent
 * service_queue.
 *
 * If the parent's parent is another throtl_grp, dispatching is propagated
 * by either arming its pending_timer or repeating dispatch directly.  If
 * the top-level service_tree is reached, throtl_data->dispatch_work is
 * kicked so that the ready bio's are issued.
 *
 * NVMe 관점:
 * disptime에 도달하면 이 타이머 핸들러가 실행되어 대기 중인 bio를
 * 상위로 전파한다. 최상위 service_queue에 도달하면 kthrotld workqueue를
 * 통해 blk_throtl_dispatch_work_fn()이 실행되고, 그 안에서
 * submit_bio_noacct_nocheck() -> blk_mq_submit_bio() -> ... ->
 * nvme_queue_rq() -> nvme_submit_cmd(doorbell) 순으로 NVMe SQ에 bio가
 * 최종적으로 밀려 들어간다.
 */
static void throtl_pending_timer_fn(struct timer_list *t)
{
	struct throtl_service_queue *sq = timer_container_of(sq, t,
							     pending_timer);
	struct throtl_grp *tg = sq_to_tg(sq); /* 타이머가 속한 service_queue의 throtl_grp (최상위면 NULL) */
	struct throtl_data *td = sq_to_td(sq); /* service_queue에 해당하는 throtl_data; NVMe namespace 단위 dispatch_work */
	struct throtl_service_queue *parent_sq;
	struct request_queue *q;
	bool dispatched;
	int ret;

	/* throtl_data may be gone, so figure out request queue by blkg */
	if (tg) /* [한국어] throtl_grp이 있으면 그 queue 사용; 최상위면 throtl_data->queue 사용 */
		q = tg->pd.blkg->q;
	else
		q = td->queue; /* [한국어] 최상위 service_queue이면 throtl_data->queue 사용 */

	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl 상태 동시 접근 보호 */

	if (!q->root_blkg) /* [한국어] root_blkg가 없으면 request_queue가 dying/quiesced 상태; NVMe 컨트롤러 정리 중 */
		goto out_unlock;

again:
	parent_sq = sq->parent_sq; /* [한국어] 부모 service_queue; NVMe QoS 계층에서 한 단계 위로 전파 */
	dispatched = false; /* [한국어] 이번 타이머 실행에서 bio를 실제로 풀었는지 추적 */

	while (true) { /* [한국어] dispatch window가 열려 있는 동안 계속 NVMe SQ로 bio 전달 */
		unsigned int __maybe_unused bio_cnt_r = sq_queued(sq, READ); /* [한국어] READ 대기 bio 수; NVMe READ SQ 진입 지연 상태 */
		unsigned int __maybe_unused bio_cnt_w = sq_queued(sq, WRITE); /* [한국어] WRITE 대기 bio 수; NVMe WRITE SQ 진입 지연 상태 */

		throtl_log(sq, "dispatch nr_queued=%u read=%u write=%u",
			   bio_cnt_r + bio_cnt_w, bio_cnt_r, bio_cnt_w);

		ret = throtl_select_dispatch(sq); /* [한국어] disptime이 도래한 cgroup부터 NVMe SQ로 bio 풀기 */
		if (ret) { /* [한국어] bio를 디스패치했다면 NVMe SQ로 전달된 양 기록 */
			throtl_log(sq, "bios disp=%u", ret);
			dispatched = true; /* [한국어] NVMe 쪽으로 풀어줄 bio가 있음 */
		}

		if (throtl_schedule_next_dispatch(sq, false)) /* [한국어] 다음 NVMe doorbell 시점을 예약하고 window가 닫혔으면 중단 */
			break; /* [한국어] 다음 dispatch window까지 대기; NVMe doorbell 지연 */

		/* this dispatch windows is still open, relax and repeat */
		spin_unlock_irq(&q->queue_lock); /* [한국어] lock을 풀고 cpu_relax: 다른 CPU가 NVMe 완료/CQ 처리를 진행할 기회 부여 */
		cpu_relax(); /* [한국어] 짧은 busy-wait; NVMe CQ ISR과 경쟁하지 않도록 스핀 완화 */
		spin_lock_irq(&q->queue_lock); /* [한국어] lock 재획득; NVMe SQ/CQ 상태와 throtl 상태 재동기화 */
	}

	if (!dispatched) /* [한국어] 이번 타이머에서 bio를 하나도 풀지 못하면 정리 종료 */
		goto out_unlock;

	if (parent_sq) {
		/* @parent_sq is another throl_grp, propagate dispatch */
		if (tg->flags & THROTL_TG_WAS_EMPTY || /* [한국어] 자식 cgroup에 새 bio가 추가되면 부모의 NVMe dispatch 시점도 갱신 필요 */
		    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) {
			tg_update_disptime(tg); /* [한국어] 부모 pending_tree에서의 disptime 재계산; NVMe doorbell 예약 재정렬 */
			if (!throtl_schedule_next_dispatch(parent_sq, false)) { /* [한국어] 부모의 dispatch window가 열려 있으면 즉시 상위로 전파; NVMe SQ 유입 연쇄 */
				/* window is already open, repeat dispatching */
				sq = parent_sq; /* [한국어] 부모 service_queue를 현재 기준으로 삼고 다시 dispatch */
				tg = sq_to_tg(sq); /* [한국어] 부모 service_queue의 throtl_grp 획득; NVMe QoS 계층 전파 */
				goto again; /* [한국어] 부모 계층에서 다시 NVMe SQ로 bio 풀기 시도 */
			}
		}
	} else {
		/* reached the top-level, queue issuing */
		queue_work(kthrotld_workqueue, &td->dispatch_work); /* [한국어] NVMe SQ 최종 진입 workqueue 예약 */
	}
out_unlock:
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제 */
}

/**
 * blk_throtl_dispatch_work_fn - work function for throtl_data->dispatch_work
 * @work: work item being executed
 *
 * This function is queued for execution when bios reach the bio_lists[]
 * of throtl_data->service_queue.  Those bios are ready and issued by this
 * function.
 *
 * NVMe 관점:
 * throttle을 통과한 bio들은 이 work 함수에서 request_queue lock을 잡고
 * bio_list_on_stack으로 모은 뒤, plug를 시작하고
 * submit_bio_noacct_nocheck()를 호출한다. 이후 bio는 blk-mq를 거쳐
 * blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
 * 순서로 NVMe SQ/CQ에 진입하며, CID가 할당되고 완료 인터럽트가 도착하면
 * nvme_process_cq()를 통해 CQ entry가 처리된다.
 */
static void blk_throtl_dispatch_work_fn(struct work_struct *work)
{
	struct throtl_data *td = container_of(work, struct throtl_data, /* [한국어] throtl_data에서 dispatch_work 역참조; NVMe namespace 단위 work item */
					      dispatch_work);
	struct throtl_service_queue *td_sq = &td->service_queue; /* [한국어] throtl_data의 최상위 service_queue; NVMe SQ 진입 직전 bio 모음 */
	struct request_queue *q = td->queue; /* [한국어] NVMe namespace의 request_queue; dispatch_work가 이 queue에 속함 */
	struct bio_list bio_list_on_stack; /* [한국어] throttle 통과한 bio를 임시로 모을 bio_list; NVMe SQ로의 배치 전달 준비 */
	struct bio *bio;
	struct blk_plug plug; /* [한국어] blk_plug: user-space batch를 유지해 NVMe multi-queue의 hctx 분산을 효율적으로 사용 */
	int rw; /* [한국어] READ/WRITE 양쪽 큐 순회; NVMe SQ/CQ 방향별 bio 수집 */

	bio_list_init(&bio_list_on_stack); /* [한국어] bio_list_on_stack 초기화; NVMe SQ로 보낼 bio 버퍼 준비 */

	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl bio list 동시 접근 보호 */
	for (rw = READ; rw <= WRITE; rw++) /* [한국어] READ/WRITE 방향별로 NVMe SQ 진입 후보 bio를 모두 꺼냄 */
		while ((bio = throtl_pop_queued(td_sq, NULL, rw))) /* [한국어] throtl 큐에서 bio를 pop; NVMe SQ로 보낼 bio 수집 */
			bio_list_add(&bio_list_on_stack, bio); /* [한국어] NVMe로 내보낼 bio를 모음 */
	spin_unlock_irq(&q->queue_lock); /* [한국어] lock 해제; NVMe 드라이버가 request 할당/CID 배정/doorbell을 진행 가능 */

	if (!bio_list_empty(&bio_list_on_stack)) { /* [한국어] throttle 통과 bio가 있을 때만 plug 시작; 불필요한 NVMe dispatch 오버헤드 방지 */
		blk_start_plug(&plug); /* [한국어] blk_plug 시작; NVMe multi-queue에서 인접 bio를 같은 hctx/hardware queue로 batch 처리 */
		while ((bio = bio_list_pop(&bio_list_on_stack))) /* [한국어] bio_list에서 하나씩 pop; NVMe 드라이버가 개별 request/CID를 할당 */
			submit_bio_noacct_nocheck(bio, false); /* [한국어] blk-mq -> NVMe 드라이버로 전달 */
		blk_finish_plug(&plug); /* [한국어] blk_plug 종료; plug된 bio들이 NVMe hardware queue에 분산 dispatch됨 */
	}
}

/*
 * [한국어]
 * tg_prfill_conf_u64 - cgroup sysfs에 u64 설정값을 출력한다.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: throtl_grp 내 u64 필드 오프셋
 * @return: 0 (U64_MAX면 출력 생략)
 *
 * 호출 체인: tg_print_conf_u64() → [tg_prfill_conf_u64]
 */
static u64 tg_prfill_conf_u64(struct seq_file *sf, struct blkg_policy_data *pd,
			      int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	u64 v = *(u64 *)((void *)tg + off); /* [한국어] 오프셋으로 u64 필드 직접 접근; bps 상한값 읽기 */

	if (v == U64_MAX) /* [한국어] 무제한이면 출력 생략 */
		return 0;
	return __blkg_prfill_u64(sf, pd, v); /* [한국어] seq_file에 "devname value\n" 형식 출력 */
}

/*
 * [한국어]
 * tg_prfill_conf_uint - cgroup sysfs에 unsigned int 설정값을 출력한다.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: throtl_grp 내 uint 필드 오프셋
 * @return: 0 (UINT_MAX면 출력 생략)
 *
 * 호출 체인: tg_print_conf_uint() → [tg_prfill_conf_uint]
 */
static u64 tg_prfill_conf_uint(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	unsigned int v = *(unsigned int *)((void *)tg + off); /* [한국어] 오프셋으로 uint 필드 직접 접근; iops 상한값 읽기 */

	if (v == UINT_MAX) /* [한국어] 무제한이면 출력 생략 */
		return 0;
	return __blkg_prfill_u64(sf, pd, v); /* [한국어] seq_file에 값 출력 */
}

/*
 * [한국어]
 * tg_print_conf_u64 - 전체 cgroup 계층의 u64 설정값을 seq_file에 출력한다.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * blkcg_print_blkgs()로 모든 blkg를 순회해 tg_prfill_conf_u64()를 호출.
 * 호출 체인: cftype.seq_show → [tg_print_conf_u64]
 */
static int tg_print_conf_u64(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_u64,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false); /* [한국어] 모든 blkg를 순회해 bps 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_print_conf_uint - 전체 cgroup 계층의 uint 설정값을 seq_file에 출력한다.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_conf_uint]
 */
static int tg_print_conf_uint(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_uint,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false); /* [한국어] 모든 blkg를 순회해 iops 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_conf_updated - cgroup의 bps/iops 설정 변경 후 하위 트리 전체 상태를 갱신한다.
 * @tg: 설정이 바뀐 throtl_grp
 * @global: true면 루트 blkg부터 전체 서브트리 순회; false면 tg 서브트리만
 * @return: 없음 (void)
 *
 * tg_set_conf() 또는 tg_set_limit()이 새 bps/iops 한도를 tg에 기록한 뒤 호출된다.
 * 이 함수는 (1) 변경된 tg 서브트리의 has_rules[] 플래그를 갱신해 blk-throttle
 * 우회 여부를 재결정하고, (2) READ/WRITE slice를 재시작해 갑작스러운 rate 하향이
 * 이미 진행 중인 NVMe SQ IO에 소급 적용되지 않도록 한다.
 * (3) pending 상태면 disptime을 재계산하고 부모 pending_timer를 재예약한다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트); queue->queue_lock 보유 상태.
 *
 * 호출 체인:
 *   tg_set_conf() / tg_set_limit() → [tg_conf_updated] → tg_update_has_rules(),
 *   throtl_start_new_slice(), tg_update_disptime(), throtl_schedule_next_dispatch()
 */
static void tg_conf_updated(struct throtl_grp *tg, bool global)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; pending_timer 재예약 시 부모 sq 참조용 */
	struct cgroup_subsys_state *pos_css; /* [한국어] blkg_for_each_descendant_pre 순회를 위한 css 커서 */
	struct blkcg_gq *blkg; /* [한국어] 순회 중 현재 blkcg_gq 포인터 */

	throtl_log(&tg->service_queue, /* [한국어] 새 bps/iops 한도를 blktrace로 기록; 디버깅/tracing 목적 */
		   "limit change rbps=%llu wbps=%llu riops=%u wiops=%u",
		   tg_bps_limit(tg, READ), tg_bps_limit(tg, WRITE),
		   tg_iops_limit(tg, READ), tg_iops_limit(tg, WRITE));

	rcu_read_lock(); /* [한국어] RCU read lock; blkcg_gq hierarchy와 NVMe queue 간 관계 보호 */
	/*
	 * Update has_rules[] flags for the updated tg's subtree.  A tg is
	 * considered to have rules if either the tg itself or any of its
	 * ancestors has rules.  This identifies groups without any
	 * restrictions in the whole hierarchy and allows them to bypass
	 * blk-throttle.
	 */
	blkg_for_each_descendant_pre(blkg, pos_css, /* [한국어] subtree의 blkcg_gq 순회; NVMe 장치에 대한 모든 cgroup limit 재계산 */
			global ? tg->td->queue->root_blkg : tg_to_blkg(tg)) {
		struct throtl_grp *this_tg = blkg_to_tg(blkg); /* [한국어] 하위 cgroup의 throtl_grp; NVMe SQ 유입 제한 상속 여부 갱신 */

		tg_update_has_rules(this_tg); /* [한국어] has_rules 갱신; NVMe SQ rate limit을 적용할지 여부 결정 */
		/* ignore root/second level */
		if (!cgroup_subsys_on_dfl(io_cgrp_subsys) || !blkg->parent || /* [한국어] root/second level은 통계 집계 제외; NVMe namespace 루트 cgroup 제외 */
		    !blkg->parent->parent)
			continue;
	}
	rcu_read_unlock(); /* [한국어] RCU read unlock; NVMe queue/cgroup 관계 참조 종료 */

	/*
	 * We're already holding queue_lock and know @tg is valid.  Let's
	 * apply the new config directly.
	 *
	 * Restart the slices for both READ and WRITES. It might happen
	 * that a group's limit are dropped suddenly and we don't want to
	 * account recently dispatched IO with new low rate.
	 */
	throtl_start_new_slice(tg, READ, false); /* [한국어] READ slice 재시작; 새 NVMe rate limit 윈도우 적용 */
	throtl_start_new_slice(tg, WRITE, false); /* [한국어] WRITE slice 재시작; 새 NVMe rate limit 윈도우 적용 */

	if (tg->flags & THROTL_TG_PENDING) { /* [한국어] pending 상태면 disptime 재계산 필요; NVMe doorbell 예약 갱신 */
		tg_update_disptime(tg); /* [한국어] 새 제한 하에서 다음 NVMe SQ 진입 시점 재계산 */
		throtl_schedule_next_dispatch(sq->parent_sq, true); /* [한국어] 부모 service_queue의 pending_timer 재예약; NVMe doorbell 지연 시점 갱신 */
	}
}

/*
 * [한국어]
 * blk_throtl_init - gendisk의 request_queue에 blk-throttle 계층을 활성화한다.
 * @disk: 대상 gendisk; NVMe namespace에 대응하는 block 장치
 * @return: 0(성공) 또는 음수 에러 코드; 실패 시 throtl_data 해제 후 반환
 *
 * cgroup sysfs에서 처음으로 bps/iops 한도가 기록될 때 (tg_set_conf/tg_set_limit)
 * 아직 초기화되지 않은 NVMe namespace에 대해 호출된다.
 * throtl_data를 NUMA-aware하게 할당하고, dispatch_work와 최상위 service_queue를
 * 초기화한 뒤 blkcg_policy_throtl를 장치에 등록한다.
 * 등록 이후 해당 queue로 submit되는 모든 bio는 __blk_throtl_bio()에서 rate limit 검사를 받는다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트); freeze/quiesce로 request_queue 동결.
 *
 * 호출 체인:
 *   tg_set_conf() / tg_set_limit() → [blk_throtl_init] → blkcg_activate_policy()
 *   (→ pd_alloc_fn → throtl_pd_alloc() / pd_init_fn → throtl_pd_init())
 */
static int blk_throtl_init(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct throtl_data *td;
	unsigned int memflags;
	int ret;

	td = kzalloc_node(sizeof(*td), GFP_KERNEL, q->node); /* throtl_data 할당; NVMe namespace 단위 throttle 상태 생성 */
	if (!td) /* 메모리 부족 시 NVMe 장치 throttle 초기화 실패 (abort) */
		return -ENOMEM;

	INIT_WORK(&td->dispatch_work, blk_throtl_dispatch_work_fn); /* dispatch_work 등록; throttle 통과 bio를 NVMe 경로로 보내는 work handler */
	throtl_service_queue_init(&td->service_queue); /* 최상위 service_queue 초기화; NVMe SQ 진입 전 bio 대기 구조 준비 */

	memflags = blk_mq_freeze_queue(disk->queue); /* request_queue 동결; NVMe controller reset/quiesce에 대응하는 상태 전환 */
	blk_mq_quiesce_queue(disk->queue); /* request_queue quiesce; NVMe SQ에 새 doorbell 금지 및 진행 중 IO 완료 대기 */

	q->td = td; /* request_queue에 throtl_data 연결; 이후 bio는 NVMe SQ 진입 전 rate limit 검사 */
	td->queue = q; /* throtl_data가 연결된 NVMe request_queue 역참조 */

	/* activate policy, blk_throtl_activated() will return true */
	ret = blkcg_activate_policy(disk, &blkcg_policy_throtl); /* blkcg_policy_throtl 활성화; NVMe 장치에 cgroup 기반 throttle 정책 등록 */
	if (ret) { /* 정책 등록 실패 시 NVMe 장치에 대한 throttle 상태 rollback */
		q->td = NULL; /* throtl_data 연결 해제; NVMe SQ 진입 제어 계층 비활성화 */
		kfree(td); /* throtl_data 메모리 해제; NVMe namespace throttle 상태 제거 */
	}

	blk_mq_unquiesce_queue(disk->queue); /* quiesce 해제; NVMe SQ doorbell을 다시 허용하고 새 IO 수용 */
	blk_mq_unfreeze_queue(disk->queue, memflags); /* request_queue 동결 해제; NVMe controller가 정상 IO 처리 재개 */

	return ret;
}


/*
 * [한국어]
 * tg_set_conf - cgroup sysfs write를 통해 bps 또는 iops 한도를 갱신한다.
 * @of: kernfs_open_file; cgroup 경로와 cftype의 private 오프셋 포함
 * @buf: 사용자 공간에서 write된 문자열 (예: "8:0 104857600")
 * @nbytes: 입력 바이트 수
 * @off: kernfs 파일 오프셋 (미사용)
 * @is_u64: true면 bps(u64), false면 iops(unsigned int)
 * @return: nbytes(성공) 또는 음수 에러 코드
 *
 * 사용자가 "echo 10485760 > /sys/fs/cgroup/.../io.throttle.read_bps_device" 형식으로
 * NVMe 장치별 rate limit을 설정하면 kernfs를 통해 이 함수가 호출된다.
 * bdev 획득 → throttle 초기화(미활성 시) → blkg 준비 → 값 파싱 → tg 필드 기록
 * → tg_conf_updated() 순서로 동작한다.
 * 0 입력은 무제한(U64_MAX)으로 변환된다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트).
 *
 * 호출 체인:
 *   tg_set_conf_u64() / tg_set_conf_uint() → [tg_set_conf] → blk_throtl_init(),
 *   blkg_conf_prep(), tg_conf_updated()
 */
static ssize_t tg_set_conf(struct kernfs_open_file *of,
			   char *buf, size_t nbytes, loff_t off, bool is_u64)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of)); /* [한국어] kernfs cgroup css → blkcg; 호출한 cgroup 식별 */
	struct blkg_conf_ctx ctx; /* [한국어] blkg_conf_prep/exit에 전달할 컨텍스트 (bdev, blkg 포함) */
	struct throtl_grp *tg; /* [한국어] 설정 대상 cgroup의 throtl_grp */
	int ret; /* [한국어] 에러 코드; 0이면 성공 */
	u64 v; /* [한국어] 파싱된 rate limit 값 (bps 또는 iops) */

	blkg_conf_init(&ctx, buf); /* [한국어] blkg_conf_ctx 초기화; 버퍼와 bdev 정보 설정 준비 */

	ret = blkg_conf_open_bdev(&ctx); /* cgroup 설정에서 장치 번호 열기; NVMe namespace bdev 획득 */
	if (ret) /* bdev 열기 실패 시 NVMe 장치 throttle 설정 중단 (abort) */
		goto out_finish;

	if (!blk_throtl_activated(ctx.bdev->bd_queue)) { /* 해당 NVMe request_queue에 throttle이 아직 활성화되지 않았는지 확인 */
		ret = blk_throtl_init(ctx.bdev->bd_disk); /* throttle 초기화; NVMe namespace에 blk-throttle 계층 추가 */
		if (ret) /* 초기화 실패 시 NVMe QoS 설정 abort */
			goto out_finish;
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, &ctx); /* blkcg_policy_throtl에 맞는 blkg 준비; NVMe 장치 cgroup 연결 */
	if (ret) /* blkg 준비 실패 시 NVMe throttle 설정 중단 (abort) */
		goto out_finish;

	ret = -EINVAL;
	if (sscanf(ctx.body, "%llu", &v) != 1) /* 사용자 입력에서 limit 값 파싱; NVMe bps/iops 상한 설정값 */
		goto out_finish; /* 형식 오류 시 NVMe throttle 설정 거부 (abort) */
	if (!v) /* 0은 무제한 의미; NVMe SQ 유입 제한 해제 */
		v = U64_MAX; /* 0을 U64_MAX로 변환; NVMe bps/iops 제한 없음 */

	tg = blkg_to_tg(ctx.blkg); /* 대상 cgroup의 throtl_grp 획득; NVMe 장치별 rate limit 객체 */
	tg_update_carryover(tg); /* 설정 변경 전 누적 대기량 carryover; NVMe SQ 진입 지연 손실/이익 보정 */

	if (is_u64) /* [한국어] bps(u64) 설정 경로 */
		*(u64 *)((void *)tg + of_cft(of)->private) = v; /* [한국어] u64 필드(bps)에 새 limit 기록; NVMe 대역폭 제한 갱신 */
	else /* [한국어] iops(uint) 설정 경로 */
		*(unsigned int *)((void *)tg + of_cft(of)->private) = v; /* [한국어] uint 필드(iops)에 새 limit 기록; NVMe 초당 명령 제한 갱신 */

	tg_conf_updated(tg, false); /* [한국어] 설정 변경 후 limit 적용 및 slice 재시작; NVMe SQ 유입 rate 갱신 */
	ret = 0; /* [한국어] 성공 */
out_finish:
	blkg_conf_exit(&ctx); /* [한국어] blkg_conf_ctx 정리; bdev 참조 해제 */
	return ret ?: nbytes; /* [한국어] 성공이면 nbytes, 실패면 에러 코드 반환 */
}

/*
 * [한국어]
 * tg_set_conf_u64 - cftype write 핸들러; tg_set_conf()를 u64(bps) 모드로 호출.
 * @of: kernfs open file
 * @buf: 입력 문자열
 * @nbytes, @off: kernfs 표준 파라미터
 * @return: tg_set_conf() 반환값 그대로
 *
 * 호출 체인: cftype.write → [tg_set_conf_u64] → tg_set_conf()
 */
static ssize_t tg_set_conf_u64(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, true); /* [한국어] bps(u64) 경로로 tg_set_conf 위임 */
}

/*
 * [한국어]
 * tg_set_conf_uint - cftype write 핸들러; tg_set_conf()를 uint(iops) 모드로 호출.
 * @of, @buf, @nbytes, @off: kernfs 표준 파라미터
 * @return: tg_set_conf() 반환값 그대로
 *
 * 호출 체인: cftype.write → [tg_set_conf_uint] → tg_set_conf()
 */
static ssize_t tg_set_conf_uint(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, false); /* [한국어] iops(uint) 경로로 tg_set_conf 위임 */
}

/*
 * [한국어]
 * tg_print_rwstat - cgroup 계층의 rwstat(read/write 통계)를 seq_file에 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_rwstat]
 */
static int tg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), /* [한국어] 모든 blkg를 순회해 rwstat 출력 */
			  blkg_prfill_rwstat, &blkcg_policy_throtl,
			  seq_cft(sf)->private, true);
	return 0;
}

/*
 * [한국어]
 * tg_prfill_rwstat_recursive - 한 cgroup의 rwstat 누적값을 seq_file에 출력.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: rwstat 필드 오프셋
 * @return: 출력 바이트 수
 *
 * 호출 체인: tg_print_rwstat_recursive() → [tg_prfill_rwstat_recursive]
 */
static u64 tg_prfill_rwstat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample sum; /* [한국어] 하위 cgroup까지 누적한 rwstat 샘플 */

	blkg_rwstat_recursive_sum(pd_to_blkg(pd), &blkcg_policy_throtl, off, /* [한국어] 서브트리 합산 */
				  &sum);
	return __blkg_prfill_rwstat(sf, pd, &sum); /* [한국어] seq_file에 누적 통계 출력 */
}

/*
 * [한국어]
 * tg_print_rwstat_recursive - 전체 cgroup 계층의 누적 rwstat를 seq_file에 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_rwstat_recursive]
 */
static int tg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), /* [한국어] 모든 blkg를 순회해 누적 rwstat 출력 */
			  tg_prfill_rwstat_recursive, &blkcg_policy_throtl,
			  seq_cft(sf)->private, true);
	return 0;
}

/* [한국어] throtl_legacy_files - v1 cgroup 인터페이스(blkcg.legacy_cftypes)에서 노출하는 파일 목록.
 * 각 파일은 cgroup/blkio.throttle.<방향>_<단위>_device 형식으로 노출되어
 * 장치별 bps/iops 한도를 개별적으로 설정한다.
 * v2 io.max와 달리 4가지 한도를 별도 파일로 관리한다. */
static struct cftype throtl_legacy_files[] = {
	{
		.name = "throttle.read_bps_device", /* [한국어] READ bps 한도 파일; 읽기 대역폭(바이트/초) 제한 */
		.private = offsetof(struct throtl_grp, bps[READ]), /* [한국어] throtl_grp.bps[READ] 오프셋 */
		.seq_show = tg_print_conf_u64, /* [한국어] cat 시 현재 READ bps 한도 출력 */
		.write = tg_set_conf_u64, /* [한국어] echo 시 새 READ bps 한도 설정 */
	},
	{
		.name = "throttle.write_bps_device", /* [한국어] WRITE bps 한도 파일; 쓰기 대역폭(바이트/초) 제한 */
		.private = offsetof(struct throtl_grp, bps[WRITE]), /* [한국어] throtl_grp.bps[WRITE] 오프셋 */
		.seq_show = tg_print_conf_u64,
		.write = tg_set_conf_u64,
	},
	{
		.name = "throttle.read_iops_device", /* [한국어] READ iops 한도 파일; 읽기 초당 IO 수 제한 */
		.private = offsetof(struct throtl_grp, iops[READ]), /* [한국어] throtl_grp.iops[READ] 오프셋 */
		.seq_show = tg_print_conf_uint,
		.write = tg_set_conf_uint,
	},
	{
		.name = "throttle.write_iops_device", /* [한국어] WRITE iops 한도 파일; 쓰기 초당 IO 수 제한 */
		.private = offsetof(struct throtl_grp, iops[WRITE]), /* [한국어] throtl_grp.iops[WRITE] 오프셋 */
		.seq_show = tg_print_conf_uint,
		.write = tg_set_conf_uint,
	},
	{
		.name = "throttle.io_service_bytes", /* [한국어] 이 cgroup이 서비스한 바이트 수 통계 */
		.private = offsetof(struct throtl_grp, stat_bytes),
		.seq_show = tg_print_rwstat,
	},
	{
		.name = "throttle.io_service_bytes_recursive", /* [한국어] 하위 cgroup 포함 누적 바이트 통계 */
		.private = offsetof(struct throtl_grp, stat_bytes),
		.seq_show = tg_print_rwstat_recursive,
	},
	{
		.name = "throttle.io_serviced", /* [한국어] 이 cgroup이 처리한 IO 수 통계 */
		.private = offsetof(struct throtl_grp, stat_ios),
		.seq_show = tg_print_rwstat,
	},
	{
		.name = "throttle.io_serviced_recursive", /* [한국어] 하위 cgroup 포함 누적 IO 수 통계 */
		.private = offsetof(struct throtl_grp, stat_ios),
		.seq_show = tg_print_rwstat_recursive,
	},
	{ }	/* terminate */
};

/*
 * [한국어]
 * tg_prfill_limit - v2 cgroup io.max 형식으로 한 blkg의 4가지 limit을 출력.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: 사용 안 함 (cftype private 오프셋, 여기서는 불필요)
 * @return: 0 (아무것도 출력 안 함 포함)
 *
 * "major:minor rbps=N wbps=N riops=N wiops=N\n" 형식 또는 "max" 문자열 출력.
 * 4가지 모두 무제한이면 출력 생략.
 * 호출 체인: tg_print_limit() → blkcg_print_blkgs() → [tg_prfill_limit]
 */
static u64 tg_prfill_limit(struct seq_file *sf, struct blkg_policy_data *pd,
			 int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	const char *dname = blkg_dev_name(pd->blkg); /* [한국어] NVMe namespace 장치 이름 (예: "8:0") 획득 */
	u64 bps_dft; /* [한국어] bps 무제한 기본값 (U64_MAX) */
	unsigned int iops_dft; /* [한국어] iops 무제한 기본값 (UINT_MAX) */

	if (!dname) /* [한국어] 장치 이름 없으면 출력 생략 (장치 미연결) */
		return 0;

	bps_dft = U64_MAX; /* [한국어] bps 무제한 기본값 설정 */
	iops_dft = UINT_MAX; /* [한국어] iops 무제한 기본값 설정 */

	if (tg->bps[READ] == bps_dft && /* [한국어] READ bps가 무제한이고 */
	    tg->bps[WRITE] == bps_dft && /* [한국어] WRITE bps가 무제한이고 */
	    tg->iops[READ] == iops_dft && /* [한국어] READ iops가 무제한이고 */
	    tg->iops[WRITE] == iops_dft) /* [한국어] WRITE iops도 무제한이면 */
		return 0; /* [한국어] 모두 무제한이면 출력 생략; 기본값과 동일 */

	seq_printf(sf, "%s", dname); /* [한국어] "major:minor" 장치 식별자 출력 */
	if (tg->bps[READ] == U64_MAX) /* [한국어] READ bps 무제한이면 "max" 출력 */
		seq_printf(sf, " rbps=max");
	else /* [한국어] 설정된 READ bps 값 출력 */
		seq_printf(sf, " rbps=%llu", tg->bps[READ]);

	if (tg->bps[WRITE] == U64_MAX) /* [한국어] WRITE bps 무제한이면 "max" 출력 */
		seq_printf(sf, " wbps=max");
	else /* [한국어] 설정된 WRITE bps 값 출력 */
		seq_printf(sf, " wbps=%llu", tg->bps[WRITE]);

	if (tg->iops[READ] == UINT_MAX) /* [한국어] READ iops 무제한이면 "max" 출력 */
		seq_printf(sf, " riops=max");
	else /* [한국어] 설정된 READ iops 값 출력 */
		seq_printf(sf, " riops=%u", tg->iops[READ]);

	if (tg->iops[WRITE] == UINT_MAX) /* [한국어] WRITE iops 무제한이면 "max" 출력 */
		seq_printf(sf, " wiops=max");
	else /* [한국어] 설정된 WRITE iops 값 출력 */
		seq_printf(sf, " wiops=%u", tg->iops[WRITE]);

	seq_printf(sf, "\n"); /* [한국어] 행 종료 문자 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_print_limit - v2 cgroup io.max의 seq_show 핸들러; 전체 계층 limit 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_limit] → blkcg_print_blkgs() → tg_prfill_limit()
 */
static int tg_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_limit, /* [한국어] 모든 blkg를 순회해 io.max 형식 limit 출력 */
			  &blkcg_policy_throtl, seq_cft(sf)->private, false);
	return 0;
}

/*
 * [한국어]
 * tg_set_limit - v2 cgroup io.max write 핸들러; rbps/wbps/riops/wiops를 한꺼번에 설정.
 * @of: kernfs_open_file; io.max cftype
 * @buf: 입력 문자열 (예: "8:0 rbps=10485760 wbps=max riops=100 wiops=max")
 * @nbytes: 입력 바이트 수
 * @off: 사용 안 함
 * @return: nbytes(성공) 또는 음수 에러 코드
 *
 * v2 통합 인터페이스로 NVMe 장치의 cgroup별 읽기/쓰기 bps/iops를 원자적으로 갱신.
 * 기존 4가지 값을 백업한 뒤 파싱한 새 값을 적용하고 tg_conf_updated()로 slice 재시작.
 * "max" 키워드는 U64_MAX로 해석(무제한).
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cftype.write → [tg_set_limit] → blk_throtl_init(), blkg_conf_prep(),
 *   tg_update_carryover(), tg_conf_updated()
 */
static ssize_t tg_set_limit(struct kernfs_open_file *of,
			  char *buf, size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of)); /* [한국어] kernfs cgroup css → blkcg; 호출한 cgroup 식별 */
	struct blkg_conf_ctx ctx; /* [한국어] blkg_conf 작업에 필요한 bdev/blkg 컨텍스트 */
	struct throtl_grp *tg; /* [한국어] 설정 대상 cgroup의 throtl_grp */
	u64 v[4]; /* [한국어] [0]=rbps [1]=wbps [2]=riops [3]=wiops; 기존값 백업 후 새 값 적용 */
	int ret; /* [한국어] 에러 코드 */

	blkg_conf_init(&ctx, buf); /* [한국어] blkg_conf_ctx 초기화; 입력 버퍼와 bdev 준비 */

	ret = blkg_conf_open_bdev(&ctx); /* [한국어] v2 인터페이스에서 NVMe namespace bdev 열기 */
	if (ret) /* [한국어] bdev 열기 실패 시 NVMe QoS 설정 abort */
		goto out_finish;

	if (!blk_throtl_activated(ctx.bdev->bd_queue)) { /* [한국어] throttle 미활성화 시 NVMe namespace에 blk-throttle 초기화 */
		ret = blk_throtl_init(ctx.bdev->bd_disk); /* [한국어] NVMe namespace throttle 계층 생성 */
		if (ret) /* [한국어] 초기화 실패 시 NVMe QoS 설정 abort */
			goto out_finish;
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, &ctx); /* [한국어] blkcg_policy_throtl에 맞는 blkg 준비; NVMe 장치 cgroup 연결 */
	if (ret) /* [한국어] blkg 준비 실패 시 NVMe throttle 설정 abort */
		goto out_finish;

	tg = blkg_to_tg(ctx.blkg); /* [한국어] 대상 cgroup의 throtl_grp; NVMe 장치별 rate limit 객체 */
	tg_update_carryover(tg); /* [한국어] 설정 변경 전 carryover 계산; NVMe SQ 진입 지연 보정 */

	v[0] = tg->bps[READ]; /* [한국어] 기존 READ bps 제한 백업; 새 값과 비교/복원용 */
	v[1] = tg->bps[WRITE]; /* [한국어] 기존 WRITE bps 제한 백업 */
	v[2] = tg->iops[READ]; /* [한국어] 기존 READ iops 제한 백업 */
	v[3] = tg->iops[WRITE]; /* [한국어] 기존 WRITE iops 제한 백업 */

	while (true) { /* [한국어] "rbps=... wbps=... riops=... wiops=..." 토큰 순회; NVMe 4가지 limit 파싱 */
		char tok[27];	/* wiops=18446744073709551616 */
		char *p;
		u64 val = U64_MAX; /* [한국어] 파싱된 limit 값; "max"이면 U64_MAX */
		int len; /* [한국어] sscanf가 소비한 바이트 수 */

		if (sscanf(ctx.body, "%26s%n", tok, &len) != 1) /* [한국어] 다음 토큰 파싱; NVMe QoS 설정 항목 하나 */
			break; /* [한국어] 더 이상 토큰이 없으면 파싱 종료 */
		if (tok[0] == '\0') /* [한국어] 빈 토큰이면 종료 */
			break;
		ctx.body += len; /* [한국어] 파싱 위치 진행; 다음 NVMe QoS 토큰으로 */

		ret = -EINVAL; /* [한국어] 기본 에러: 형식 오류 */
		p = tok; /* [한국어] 토큰 문자열을 strsep으로 분리하기 위한 임시 포인터 */
		strsep(&p, "="); /* [한국어] "key=value" 분리; NVMe limit 항목 이름과 값 분리 */
		if (!p || (sscanf(p, "%llu", &val) != 1 && strcmp(p, "max"))) /* [한국어] 값 파싱 실패 또는 "max"가 아니면 설정 거부 (abort) */
			goto out_finish;

		ret = -ERANGE; /* [한국어] 에러: 범위 초과 (0은 불허) */
		if (!val) /* [한국어] 값이 0이면 ERANGE; NVMe limit은 0 불허(무제한은 max) */
			goto out_finish;

		ret = -EINVAL; /* [한국어] 에러: 알 수 없는 키 이름 */
		if (!strcmp(tok, "rbps")) /* [한국어] READ bps limit 설정; NVMe READ 대역폭 QoS */
			v[0] = val;
		else if (!strcmp(tok, "wbps")) /* [한국어] WRITE bps limit 설정; NVMe WRITE 대역폭 QoS */
			v[1] = val;
		else if (!strcmp(tok, "riops")) /* [한국어] READ iops limit 설정; NVMe READ 초당 명령 QoS */
			v[2] = min_t(u64, val, UINT_MAX);
		else if (!strcmp(tok, "wiops")) /* [한국어] WRITE iops limit 설정; NVMe WRITE 초당 명령 QoS */
			v[3] = min_t(u64, val, UINT_MAX);
		else /* [한국어] 알 수 없는 키이면 설정 거부 */
			goto out_finish;
	}

	tg->bps[READ] = v[0]; /* [한국어] READ bps 최종 적용; NVMe SQ READ 대역폭 제한 */
	tg->bps[WRITE] = v[1]; /* [한국어] WRITE bps 최종 적용; NVMe SQ WRITE 대역폭 제한 */
	tg->iops[READ] = v[2]; /* [한국어] READ iops 최종 적용; NVMe SQ READ 초당 명령 제한 */
	tg->iops[WRITE] = v[3]; /* [한국어] WRITE iops 최종 적용; NVMe SQ WRITE 초당 명령 제한 */

	tg_conf_updated(tg, false); /* [한국어] 4가지 limit 변경 후 slice 재시작; NVMe SQ 유입 rate 즉시 재조정 */
	ret = 0; /* [한국어] 성공 */
out_finish:
	blkg_conf_exit(&ctx); /* [한국어] blkg_conf_ctx 정리; bdev 참조 해제 */
	return ret ?: nbytes; /* [한국어] 성공이면 nbytes, 실패면 에러 코드 반환 */
}

/* [한국어] throtl_files - v2 cgroup io.max 파일 정의 (blkcg.dfl_cftypes).
 * "io.max" 파일 하나로 rbps/wbps/riops/wiops를 동시에 설정할 수 있는 통합 인터페이스.
 * CFTYPE_NOT_ON_ROOT: root cgroup에는 노출하지 않음 (root는 제한 없음이 기본). */
static struct cftype throtl_files[] = {
	{
		.name = "max", /* [한국어] v2 io.max 파일; "rbps=N wbps=N riops=N wiops=N" 형식 */
		.flags = CFTYPE_NOT_ON_ROOT, /* [한국어] root cgroup에는 노출 안 함; root는 무제한이 기본 */
		.seq_show = tg_print_limit, /* [한국어] cat 시 현재 4가지 limit 출력 */
		.write = tg_set_limit, /* [한국어] echo 시 4가지 limit 파싱 및 적용 */
	},
	{ }	/* terminate */
};

/*
 * [한국어]
 * throtl_shutdown_wq - blk-throttle dispatch_work를 동기적으로 취소한다.
 * @q: 대상 request_queue
 * @return: 없음 (void)
 *
 * blk_throtl_exit() 또는 blk_throtl_cancel_bios() 경로에서 호출되어
 * kthrotld workqueue의 dispatch_work가 완료될 때까지 대기한 뒤 취소한다.
 * 이 호출 이후에는 더 이상 throttle에서 NVMe SQ로 bio가 흘러가지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (cancel_work_sync가 슬립 가능).
 *
 * 호출 체인:
 *   blk_throtl_exit() → [throtl_shutdown_wq] → cancel_work_sync()
 */
static void throtl_shutdown_wq(struct request_queue *q)
{
	struct throtl_data *td = q->td; /* [한국어] request_queue에 연결된 throtl_data; dispatch_work 소유 */

	cancel_work_sync(&td->dispatch_work); /* [한국어] kthrotld workqueue 정리; NVMe SQ로 bio를 보내는 work item 취소 */
}

/*
 * [한국어]
 * tg_flush_bios - cgroup offline 또는 장치 해제 시 throttle 큐의 bio를 강제 디스패치한다.
 * @tg: flush 대상 throtl_grp
 * @return: 없음 (void)
 *
 * THROTL_TG_CANCELING 플래그를 설정해 새 rate limit 검사를 건너뛰고,
 * pending_tree에 등록된 경우 disptime을 즉시로 만들어 NVMe SQ로 빠르게 흘려보낸다.
 * del_gendisk() 이후 inflight IO가 남지 않도록 보장하는 안전장치.
 * THROTL_TG_PENDING가 없으면 early return하여 pending_tree 이중 삽입을 방지한다.
 * 실행 컨텍스트: spin_lock_irq 보유 상태 (IRQ disable).
 *
 * 호출 체인:
 *   throtl_pd_offline() / blk_throtl_cancel_bios() → [tg_flush_bios]
 *   → tg_update_disptime(), throtl_schedule_pending_timer()
 */
static void tg_flush_bios(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; pending_timer 재예약 대상 */

	if (tg->flags & THROTL_TG_CANCELING) /* [한국어] 이미 취소 중이면 중복 flush 방지; NVMe namespace 제거 중 */
		return;
	/*
	 * Set the flag to make sure throtl_pending_timer_fn() won't
	 * stop until all throttled bios are dispatched.
	 */
	tg->flags |= THROTL_TG_CANCELING; /* [한국어] THROTL_TG_CANCELING 설정; NVMe controller reset/quiesce 시 제한 검사 우회 */

	/*
	 * Do not dispatch cgroup without THROTL_TG_PENDING or cgroup
	 * will be inserted to service queue without THROTL_TG_PENDING
	 * set in tg_update_disptime below. Then IO dispatched from
	 * child in tg_dispatch_one_bio will trigger double insertion
	 * and corrupt the tree.
	 */
	if (!(tg->flags & THROTL_TG_PENDING)) /* [한국어] pending_tree에 없으면 disptime 갱신/타이머 예약 불필요 */
		return;

	/*
	 * Update disptime after setting the above flag to make sure
	 * throtl_select_dispatch() won't exit without dispatching.
	 */
	tg_update_disptime(tg); /* [한국어] disptime을 즉시로 만들어 NVMe SQ로 남은 bio를 강제 디스패치 */

	throtl_schedule_pending_timer(sq, jiffies + 1); /* [한국어] 1 jiffy 후 pending_timer 만료; NVMe SQ로 남은 bio를 빠르게 흘림 */
}

/*
 * [한국어]
 * throtl_pd_offline - blkcg_policy의 pd_offline_fn 콜백; cgroup offline 시 호출.
 * @pd: offline되는 cgroup의 blkg_policy_data
 * @return: 없음 (void)
 *
 * cgroup이 offline될 때 blk-cgroup 코어가 이 함수를 호출한다.
 * tg_flush_bios()로 해당 cgroup의 throttle 큐에 남은 bio를 강제 디스패치한다.
 * 실행 컨텍스트: IRQ disable, queue_lock 보유 상태.
 *
 * 호출 체인:
 *   blkcg offline → blkg_destroy() → pd_offline_fn → [throtl_pd_offline] → tg_flush_bios()
 */
static void throtl_pd_offline(struct blkg_policy_data *pd)
{
	tg_flush_bios(pd_to_tg(pd)); /* [한국어] offline되는 cgroup의 throtl 큐 강제 flush; NVMe SQ로 남은 bio 방출 */
}

/* [한국어] blkcg_policy_throtl - blk-cgroup에 등록되는 blk-throttle 정책 기술자.
 * dfl_cftypes/legacy_cftypes: v2(io.max)/v1(throttle.*) sysfs 파일 집합.
 * pd_alloc_fn/pd_init_fn: blkg 생성 시 throtl_grp 할당 및 초기화.
 * pd_online_fn: blkg가 online될 때 throtl_grp 연결 완료 처리.
 * pd_offline_fn: cgroup offline 시 throttle 큐 flush.
 * pd_free_fn: blkg 해제 시 throtl_grp 메모리 정리. */
struct blkcg_policy blkcg_policy_throtl = {
	.dfl_cftypes		= throtl_files, /* [한국어] v2 cgroup io.max 파일; rbps/wbps/riops/wiops 통합 설정 */
	.legacy_cftypes		= throtl_legacy_files, /* [한국어] v1 cgroup throttle.{read,write}_{bps,iops}_device 파일 */

	.pd_alloc_fn		= throtl_pd_alloc, /* [한국어] blkg 생성 시 throtl_grp NUMA-aware 할당 */
	.pd_init_fn		= throtl_pd_init, /* [한국어] blkg 초기화 시 throtl_grp 연결 및 slice 기본값 설정 */
	.pd_online_fn		= throtl_pd_online, /* [한국어] blkg online 시 상위 service_queue 연결 완료 */
	.pd_offline_fn		= throtl_pd_offline, /* [한국어] cgroup offline 시 throttle 큐 강제 flush */
	.pd_free_fn		= throtl_pd_free, /* [한국어] blkg 해제 시 throtl_grp 메모리 반환 */
};

/*
 * [한국어]
 * blk_throtl_cancel_bios - 디스크 해제 시 모든 하위 cgroup의 throttle 큐 bio를 강제 flush.
 * @disk: 해제 중인 gendisk; NVMe namespace
 * @return: 없음 (void)
 *
 * del_gendisk() 경로에서 호출되어 NVMe namespace가 제거되기 전에 throtl 큐에 남아 있는
 * bio들을 전부 NVMe SQ 방향으로 흘려보내거나 상위로 전파한다.
 * blkg_for_each_descendant_post로 모든 하위 cgroup을 post-order로 순회하며
 * tg_flush_bios()를 호출한다. del_gendisk 이후 inflight IO가 없도록 보장.
 * 실행 컨텍스트: 프로세스 컨텍스트; spin_lock_irq로 IRQ disable.
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_cancel_bios] → tg_flush_bios()
 */
void blk_throtl_cancel_bios(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] NVMe namespace의 request_queue */
	struct cgroup_subsys_state *pos_css; /* [한국어] blkg 순회를 위한 css 커서 */
	struct blkcg_gq *blkg; /* [한국어] 순회 중 현재 blkcg_gq */

	if (!blk_throtl_activated(q)) /* [한국어] throttle이 비활성화면 flush 할 것 없음; NVMe SQ 진입 제어 계층 없음 */
		return;

	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; NVMe SQ/CQ와 throtl 구조 동시 보호 */
	/*
	 * queue_lock is held, rcu lock is not needed here technically.
	 * However, rcu lock is still held to emphasize that following
	 * path need RCU protection and to prevent warning from lockdep.
	 */
	rcu_read_lock(); /* [한국어] RCU read lock; blkcg_gq hierarchy와 NVMe queue 관계 보호 */
	blkg_for_each_descendant_post(blkg, pos_css, q->root_blkg) { /* [한국어] 하위 cgroup 순회; NVMe namespace에 연결된 모든 cgroup의 throttle 큐 처리 */
		/*
		 * disk_release will call pd_offline_fn to cancel bios.
		 * However, disk_release can't be called if someone get
		 * the refcount of device and issued bios which are
		 * inflight after del_gendisk.
		 * Cancel bios here to ensure no bios are inflight after
		 * del_gendisk.
		 */
		tg_flush_bios(blkg_to_tg(blkg)); /* [한국어] 각 cgroup의 throttle 큐 flush; NVMe SQ로 남은 bio 강제 전달 또는 폐기 준비 */
	}
	rcu_read_unlock(); /* [한국어] RCU read unlock; NVMe queue/cgroup 관계 참조 종료 */
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제; NVMe SQ/CQ 처리 재개 */
}

/*
 * [한국어]
 * tg_within_limit - bio가 현재 throtl_grp의 bps/iops 제한 안에 있는지 판단한다.
 * @tg: 검사 대상 throtl_grp
 * @bio: 검사할 bio
 * @rw: READ(0) 또는 WRITE(1)
 * @return: true면 NVMe SQ 진입 가능, false면 throtl 큐에 대기 필요
 *
 * bps/iops 두 단계 제한을 FIFO 순서로 검사한다.
 * BIO_BPS_THROTTLED가 설정된 분할 bio는 bps 단계를 건너뛰고 iops만 검사.
 * bps 큐가 비어 있고 bio가 bps 제한 내이면, bps 사용량을 선차감하고
 * iops 큐로 직접 보낸다.
 * 이미 대기 중인 bio가 있으면 FIFO 순서를 지키기 위해 항상 false 반환.
 * 실행 컨텍스트: spin_lock_irq(queue_lock) 보유 상태.
 *
 * 호출 체인:
 *   __blk_throtl_bio() → [tg_within_limit] → tg_dispatch_bps_time(),
 *   tg_dispatch_iops_time(), throtl_charge_bps_bio()
 */
static bool tg_within_limit(struct throtl_grp *tg, struct bio *bio, bool rw)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] 현재 cgroup의 service_queue; NVMe SQ 진입 전 bio 대기 상태 */

	/*
	 * For a split bio, we need to specifically distinguish whether the
	 * iops queue is empty.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED)) /* [한국어] 분할 bio는 bps가 이미 처리됨; NVMe에서는 iops 제한만 추가 검사 */
		return sq->nr_queued_iops[rw] == 0 && /* [한국어] iops 큐가 비어 있고 iops 제한 통과 시 즉시 NVMe SQ 진입 가능 */
				tg_dispatch_iops_time(tg, bio) == 0;

	/*
	 * Throtl is FIFO - if bios are already queued, should queue.
	 * If the bps queue is empty and @bio is within the bps limit, charge
	 * bps here for direct placement into the iops queue.
	 */
	if (sq_queued(&tg->service_queue, rw)) { /* [한국어] 동일 방향에 이미 대기 bio가 있으면 FIFO 순서로 NVMe SQ 진입 지연 */
		if (sq->nr_queued_bps[rw] == 0 && /* [한국어] bps 큐가 비어 있고 bio가 bps 제한 내이면 bps 사용량 먼저 기록 */
		    tg_dispatch_bps_time(tg, bio) == 0)
			throtl_charge_bps_bio(tg, bio); /* [한국어] bps 사용량을 선차감; 이후 iops 큐에서 NVMe 초당 명령 제한만 검사 */

		return false; /* [한국어] FIFO: 이미 대기 중인 bio가 있으면 현재 bio도 NVMe SQ 진입 지연 */
	}

	return tg_dispatch_time(tg, bio) == 0; /* [한국어] bps/iops 모두 통과하면 0(true), 아니면 NVMe doorbell 지연 시간(false) */
}

/*
 * [한국어]
 * __blk_throtl_bio - bio가 blk-throttle 계층을 통과할 수 있는지 검사하고, 초과 시 큐잉한다.
 * @bio: 검사 대상 bio
 * @return: true면 throttled(NVMe SQ 진입 지연), false면 즉시 진입 가능
 *
 * submit_bio() → blk_mq_submit_bio() → blk_throtl_bio() 경로로 호출되어
 * cgroup 계층을 bottom-up으로 순회하며 각 throtl_grp의 bps/iops 제한을 검사한다.
 * 모든 제한을 통과하면 bio에 BIO_BPS_THROTTLED를 설정하고 false(비throttle)를 반환,
 * 이후 blk_mq_get_request() → nvme_queue_rq() → nvme_submit_cmd(doorbell)로 진행된다.
 * 제한 초과 시 throtl 큐에 넣고 disptime 이후 pending_timer가 재디스패치한다.
 * root 권한 IO(bio_issue_as_root_blkg)는 rate 초과라도 즉시 통과(부채 추적).
 * 실행 컨텍스트: softirq 또는 프로세스 컨텍스트; spin_lock_irq + RCU read lock.
 *
 * 호출 체인:
 *   submit_bio() → blk_mq_submit_bio() → blk_throtl_bio() → [__blk_throtl_bio]
 *   → tg_within_limit(), throtl_add_bio_tg(), tg_update_disptime(),
 *   throtl_schedule_next_dispatch()
 */
bool __blk_throtl_bio(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev); /* [한국어] bio가 속한 NVMe namespace의 request_queue 획득 */
	struct blkcg_gq *blkg = bio->bi_blkg; /* [한국어] bio의 blkcg_gq; NVMe 장치별 cgroup queue 상태 */
	struct throtl_qnode *qn = NULL; /* [한국어] 부모로 전달할 qnode 포인터; NVMe SQ 진입 전 cgroup 계층 이동용 */
	struct throtl_grp *tg = blkg_to_tg(blkg); /* [한국어] bio가 속한 cgroup의 throtl_grp; NVMe rate limit 상태 */
	struct throtl_service_queue *sq; /* [한국어] 현재 검사 중인 service_queue; NVMe SQ 진입 전 관문 */
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향; NVMe SQ/CQ 방향 */
	bool throttled = false; /* [한국어] bio가 throttle되어 NVMe SQ 진입이 지연되었는지 결과 */
	struct throtl_data *td = tg->td; /* [한국어] throtl_data; NVMe namespace 단위 dispatch_work/pending_timer */

	rcu_read_lock(); /* [한국어] RCU read lock; bio->bi_blkg 및 cgroup hierarchy가 해제되지 않도록 보호 */
	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl 상태 동시 보호 */
	sq = &tg->service_queue; /* [한국어] 현재 cgroup의 service_queue; NVMe SQ 진입 전 대기열 */

	while (true) { /* [한국어] cgroup hierarchy를 bottom-up으로 순회; 모든 NVMe rate limit 통과 필요 */
		if (tg_within_limit(tg, bio, rw)) { /* [한국어] 현재 cgroup의 bps/iops 제한 안에 있으면 NVMe SQ 진입 가능 */
			/* within limits, let's charge and dispatch directly */
			throtl_charge_iops_bio(tg, bio); /* [한국어] iops 사용량 기록; NVMe SQ에 들어갈 하나의 명령(CID)으로 카운트 */

			/*
			 * We need to trim slice even when bios are not being
			 * queued otherwise it might happen that a bio is not
			 * queued for a long time and slice keeps on extending
			 * and trim is not called for a long time. Now if limits
			 * are reduced suddenly we take into account all the IO
			 * dispatched so far at new low rate and * newly queued
			 * IO gets a really long dispatch time.
			 *
			 * So keep on trimming slice even if bio is not queued.
			 */
			throtl_trim_slice(tg, rw); /* [한국어] slice 정리; NVMe rate limit 시간 윈도우 보정 */
		} else if (bio_issue_as_root_blkg(bio)) { /* [한국어] root 권한 IO는 우선 처리; NVMe 컨트롤러 우선순위 역전 방지용 예외 */
			/*
			 * IOs which may cause priority inversions are
			 * dispatched directly, even if they're over limit.
			 *
			 * Charge and dispatch directly, and our throttle
			 * control algorithm is adaptive, and extra IO bytes
			 * will be throttled for paying the debt
			 */
			throtl_charge_bps_bio(tg, bio); /* [한국어] root IO에도 bps 사용량 기록; NVMe 대역폭 제한 추적 */
			throtl_charge_iops_bio(tg, bio); /* [한국어] root IO에도 iops 사용량 기록; NVMe 초당 명령 추적 (나중에 상환) */
		} else {
			/* if above limits, break to queue */
			break; /* [한국어] 제한 초과: NVMe SQ 진입 중단하고 throtl 큐에 bio 적재 */
		}

		/*
		 * @bio passed through this layer without being throttled.
		 * Climb up the ladder.  If we're already at the top, it
		 * can be executed directly.
		 */
		qn = &tg->qnode_on_parent[rw]; /* [한국어] 부모 service_queue의 qnode 선택; NVMe rate limit 관문을 한 단계 올라감 */
		sq = sq->parent_sq; /* [한국어] parent_sq로 이동; NVMe SQ 진입 전 상위 cgroup 제한 검사 */
		tg = sq_to_tg(sq); /* [한국어] 상위 service_queue의 throtl_grp 획득; 최상위면 NULL */
		if (!tg) { /* [한국어] 최상위 service_queue에 도달하면 모든 NVMe rate limit 통과 */
			bio_set_flag(bio, BIO_BPS_THROTTLED); /* [한국어] 모든 throtl_grp 통과, NVMe로 진입 가능 */
			goto out_unlock; /* [한국어] BIO_BPS_THROTTLED 설정; blk-mq -> NVMe 드라이버로 직접 전달 가능 */
		}
	}

	/* out-of-limit, queue to @tg */
	throtl_log(sq, "[%c] bio. bdisp=%llu sz=%u bps=%llu iodisp=%u iops=%u queued=%d/%d", /* [한국어] 제한 초과 bio 정보 blktrace 기록; NVMe SQ 유입 지연 디버그 */
		   rw == READ ? 'R' : 'W',
		   tg->bytes_disp[rw], bio->bi_iter.bi_size,
		   tg_bps_limit(tg, rw),
		   tg->io_disp[rw], tg_iops_limit(tg, rw),
		   sq_queued(sq, READ), sq_queued(sq, WRITE));

	td->nr_queued[rw]++; /* [한국어] NVMe SQ 진입이 지연된 bio 수 증가 */
	throtl_add_bio_tg(bio, qn, tg); /* [한국어] bio를 throtl 큐에 추가; NVMe SQ 유입을 지연시키는 소프트웨어 관문 */
	throttled = true; /* [한국어] bio가 throttled 됨; NVMe SQ로 즉시 진입하지 않음 */

	/*
	 * Update @tg's dispatch time and force schedule dispatch if @tg
	 * was empty before @bio, or the iops queue is empty and @bio will
	 * add to.  The forced scheduling isn't likely to cause undue
	 * delay as @bio is likely to be dispatched directly if its @tg's
	 * disptime is not in the future.
	 */
	if (tg->flags & THROTL_TG_WAS_EMPTY || /* [한국어] 큐가 비어 있었거나 iops 큐가 비어 있으면 dispatch 시점 갱신 필요 */
	    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) {
		tg_update_disptime(tg); /* [한국어] 새 bio에 대해 다음 NVMe SQ 진입 시각(disptime) 재계산 */
		throtl_schedule_next_dispatch(tg->service_queue.parent_sq, true); /* [한국어] 부모 service_queue의 pending_timer 강제 예약; NVMe doorbell 지연/갱신 */
	}

out_unlock:
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제; NVMe 드라이버가 CQ/ISR 처리 진행 가능 */

	rcu_read_unlock(); /* [한국어] RCU read unlock; bio/cgroup 구조 참조 종료 */
	return throttled; /* [한국어] throttled 여부 반환; true면 NVMe SQ 진입 지연, false면 즉시 진입 */
}

/*
 * [한국어]
 * blk_throtl_exit - gendisk에 연결된 blk-throttle 상태를 해제하고 자원을 반환한다.
 * @disk: 해제 중인 gendisk; NVMe namespace
 * @return: 없음 (void)
 *
 * del_gendisk() 흐름에서 호출되어 NVMe namespace가 제거될 때 throtl_data,
 * pending_timer, dispatch_work를 정리한다.
 * blkg_destroy_all()이 먼저 정책을 비활성화하므로, 여기서는 throtl_data 존재 여부만
 * 확인하고 pending_timer 동기적 삭제 → dispatch_work 취소 → throtl_data 해제 순으로 처리.
 * 실행 컨텍스트: 프로세스 컨텍스트; timer_delete_sync/cancel_work_sync 슬립 가능.
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_exit] → timer_delete_sync(), throtl_shutdown_wq(), kfree()
 */
void blk_throtl_exit(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] NVMe namespace의 request_queue; throtl_data 연결점 */

	/*
	 * blkg_destroy_all() already deactivate throtl policy, just check and
	 * free throtl data.
	 */
	if (!q->td) /* [한국어] throtl_data가 없으면 정리할 것 없음; NVMe namespace에 throttle 계층 없음 */
		return;

	timer_delete_sync(&q->td->service_queue.pending_timer); /* [한국어] pending_timer 동기 삭제; NVMe doorbell 예약 중지 */
	throtl_shutdown_wq(q); /* [한국어] kthrotld dispatch_work 취소; NVMe SQ로의 bio 전달 중단 */
	kfree(q->td); /* [한국어] throtl_data 메모리 해제; NVMe namespace throttle 상태 제거 */
}

/*
 * [한국어]
 * throtl_init - blk-throttle 모듈 초기화; kthrotld workqueue 생성 및 정책 등록.
 * @return: blkcg_policy_register() 반환값; 0이면 성공
 *
 * 커널 부팅 시 module_init()을 통해 한 번 호출된다.
 * kthrotld(WQ_MEM_RECLAIM) workqueue를 생성하고 blkcg_policy_throtl를 등록한다.
 * kthrotld_workqueue에서 blk_throtl_dispatch_work_fn()이 실행되어 throttle된 bio를
 * NVMe SQ 방향으로 내보낸다.
 * workqueue 생성 실패 시 panic(); NVMe throttle 계층 없이는 안전한 부팅이 불가능.
 * 실행 컨텍스트: 커널 초기화 (프로세스 컨텍스트, 단일 CPU).
 *
 * 호출 체인:
 *   module_init → [throtl_init] → alloc_workqueue(), blkcg_policy_register()
 */
static int __init throtl_init(void)
{
	kthrotld_workqueue = alloc_workqueue("kthrotld", WQ_MEM_RECLAIM, 0); /* [한국어] kthrotld workqueue 생성; NVMe SQ로 bio를 전달하는 데몬 */
	if (!kthrotld_workqueue) /* [한국어] workqueue 생성 실패는 치명적; NVMe throttle 계층 없이 부팅 진행 불가 */
		panic("Failed to create kthrotld\n");

	return blkcg_policy_register(&blkcg_policy_throtl); /* [한국어] blk-throttle 정책 등록; NVMe 장치 포함 모든 block 장치에서 사용 가능 */
}

module_init(throtl_init);

/* NVMe 관점 핵심 요약
 *
 * - blk-throttle은 submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request()
 *   -> nvme_queue_rq() -> nvme_submit_cmd(doorbell) 경로에서, bio 단위로
 *   NVMe SQ/CQ에 도달하기 전에 bps/IOPS 제한을 적용하는 소프트웨어
 *   QoS 계층이다.
 *
 * - throtl_grp은 cgroup별 rate limit 상태이며, bytes_disp/io_disp는
 *   NVMe 명령으로 실제 전환(디스패치)되는 바이트/IO 수를 추적해 평균
 *   처리율을 제어한다. (추정) 이 값은 NVMe 완료(CQ entry) 시점이 아닌
 *   디스패치 시점에 갱신되므로, 깊은 큐잉 상황에서는 실제 SSD 완료
 *   처리율과 미세한 차이가 있을 수 있다.
 *
 * - throtl_service_queue의 pending_tree와 pending_timer는 NVMe doorbell
 *   치는 시점을 지연시키는 소프트웨어 타이머로 동작하며, disptime이 가장
 *   빠른 cgroup부터 라운드 로빈 방식으로 bio를 풀어준다.
 *
 * - blk_throtl_dispatch_work_fn()에서 throttle을 통과한 bio는
 *   submit_bio_noacct_nocheck()를 통해 blk-mq로 다시 들어가, 이후
 *   NVMe 드라이버의 request 할당, PRP/SGL 구성, CID 배정, doorbell 기록
 *   순서로 실제 플래시 접근으로 전환된다.
 *
 * - 이 파일은 blk-cgroup, blk-mq, NVMe 드라이버 사이의 중간 관문이며,
 *   blk-cgroup-rwstat.c의 통계, blk-iolatency 등 다른 blkcg 정책과
 *   병행될 수 있다. NVMe 컨트롤러 자체의 Queue Depth나 namespace 단위
 *   하드웨어 한도는 여기서 직접 조절하지 않는다.
 */
