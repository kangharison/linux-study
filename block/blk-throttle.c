// SPDX-License-Identifier: GPL-2.0
/*
 * Interface for controlling IO bandwidth on a request queue
 *
 * Copyright (C) 2010 Vivek Goyal <vgoyal@redhat.com>
 */

/*
 * NVMe 관점 파일 개요:
 * 이 파일은 cgroup 기반 IO bps/IOPS throttling을 구현한다. 응용 프로그램의
 * submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request() 경로를 따라
 * 날라온 bio가 nvme_queue_rq()에 의해 SQ에 CID를 할당받고 doorbell을 치기
 * 전, 이 계층에서 rate limit을 검사하여 과도한 NVMe 명령 유입을 억제한다.
 * blk-throttle.h의 throtl_grp/service_queue/qnode를 사용하며, blk-cgroup
 * 다음, blk-mq/NVMe 드라이버 이전 단계에서 동작하는 소프트웨어 관문이다.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include "blk.h"
#include "blk-cgroup-rwstat.h"
#include "blk-throttle.h"

/*
 * 주요 구조체와 NVMe 동작 연결 (자세한 정의는 blk-throttle.h 참고):
 *
 * struct throtl_data:
 *  - service_queue: 장치(request_queue) 단위 최상위 서비스 큐. NVMe의 한
 *    namespace/queue에 대응되며 모든 throtl_grp이 최종 이 큐를 거쳐
 *    디스패치된다.
 *  - queue: 이 throttle 상태가 연결된 request_queue. NVMe에서는 nvme_ns
 *    혹은 multipath namespace의 request_queue를 가리킨다.
 *  - nr_queued[2]: READ/WRITE로 대기 중인 bio 수. NVMe SQ 유입량 제한을
 *    위해 카운트한다.
 *  - dispatch_work: 제한을 통과한 bio를 submit_bio_noacct_nocheck() 경로로
 *    다시 본래 흐름에 태우는 workqueue 항목.
 *
 * struct throtl_service_queue:
 *  - parent_sq: 상위 cgroup의 service_queue.階層 구조를 따라 bio가
 *    bottom-up으로 이동한다.
 *  - queued[2]: throtl_qnode 리스트. NVMe 입장에서 아직 SQ에 넣지 않은
 *    대기 bio들이다.
 *  - nr_queued_bps/iops[2]: bps/iops 큐에 쌓인 bio 수.
 *  - pending_tree: 활성 throtl_grp을 disptime 기준으로 정렬한 RB tree.
 *    다음에 어느 cgroup의 bio를 NVMe 쪽으로 풀어줄지 결정한다.
 *  - pending_timer: 첫 번째 pending 그룹의 disptime에 만료되는 타이머.
 *
 * struct throtl_grp:
 *  - bps[2], iops[2]: cgroup별 bps/IOPS 상한. NVMe SSD가 소화할 수 있는
 *    대역폭/초당 명령 수를 소프트웨어적으로 제한한다.
 *  - bytes_disp[2], io_disp[2]: 현재 slice에서 이미 디스패치한 바이트/IO
 *    수. NVMe doorbell 회수와 연결되는 실제 디스패치량을 추적한다.
 *  - slice_start/end[2]: throttle slice 시작/끝. HZ/10 단위로 NVMe 평균
 *    처리율을 제어하기 위한 시간 윈도우.
 *  - disptime: 이 그룹이 다시 디스패치 가능해지는 시점(jiffies).
 *  - qnode_on_self[2], qnode_on_parent[2]: 자신/부모 큐에 bio를 담는
 *    qnode. Round-robin으로 NVMe 쪽에 공정하게 전달하기 위해 사용.
 *
 * struct throtl_qnode:
 *  - bios_bps: bps 제한 때문에 대기 중인 bio 리스트.
 *  - bios_iops: iops 제한 때문에 대기 중인 bio 리스트. NVMe에서는 이
 *    리스트를 거쳐야 SQ/CQ에 도달할 수 있다.
 *  - tg: 이 qnode가 속한 throtl_grp. blkg reference 관리에 사용.
 */

/* Max dispatch from a group in 1 round */
#define THROTL_GRP_QUANTUM 8

/* Total max dispatch from all groups in one round */
#define THROTL_QUANTUM 32

/* Throttling is performed over a slice and after that slice is renewed */
#define DFL_THROTL_SLICE (HZ / 10)

/* A workqueue to queue throttle related work */
static struct workqueue_struct *kthrotld_workqueue;

#define rb_entry_tg(node)	rb_entry((node), struct throtl_grp, rb_node)

struct throtl_data
{
	/* service tree for active throtl groups */
	struct throtl_service_queue service_queue; /* NVMe namespace/queue 단위 최상위 service_queue: 모든 하위 cgroup이 이 큐를 거쳐 SQ로 향함 */

	struct request_queue *queue; /* 이 throttle이 연결된 NVMe namespace의 request_queue */

	/* Total Number of queued bios on READ and WRITE lists */
	unsigned int nr_queued[2]; /* NVMe SQ 진입이 지연된 READ/WRITE bio 수 (소프트웨어 Queue Depth 제어 지표) */

	/* Work for dispatching throttled bios */
	struct work_struct dispatch_work; /* throttle 통과 후 bio를 submit_bio_noacct_nocheck() -> blk-mq -> NVMe 드라이버로 보내는 work */
};

static void throtl_pending_timer_fn(struct timer_list *t);

static inline struct blkcg_gq *tg_to_blkg(struct throtl_grp *tg)
{
	return pd_to_blkg(&tg->pd); /* throtl_grp -> blkcg_gq 매핑: NVMe 장치의 cgroup별 queue 상태 접근 */
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
	if (sq && sq->parent_sq) /* service_queue가 throtl_grp에 내장된 경우 NVMe 제어 흐름의 하위 cgroup 반환 */
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
	struct throtl_grp *tg = sq_to_tg(sq);

	if (tg)
		return tg->td; /* 상위 service_queue에서 throtl_data 획득: NVMe namespace 단위 throttle 상태 */
	else
		return container_of(sq, struct throtl_data, service_queue);
}

static uint64_t tg_bps_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg);

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* root cgroup은 하위에 제한을 상속시키기 위해 bps 제한을 무제한으로 둠 */
		return U64_MAX; /* bps 무제한: NVMe SQ 유입 제한 없음 */

	return tg->bps[rw];
}

static unsigned int tg_iops_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg);

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* root cgroup iops 무제한: NVMe 초당 명령 제한 없음 */
		return UINT_MAX;

	return tg->iops[rw];
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

static inline unsigned int throtl_bio_data_size(struct bio *bio)
{
	/* assume it's one sector */
	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) /* REQ_OP_DISCARD는 논리 블록 512B로 계산; NVMe Deallocate/Write Zeroes 명령 크기/PRP 구성에 반영 */
		return 512;
	return bio->bi_iter.bi_size; /* bio 실제 크기; NVMe PRP/SGL entry 수 및 DMA segment 수 계산의 입력값 */
}

/*
 * throtl_qnode_init: throtl_qnode를 초기화한다.
 * NVMe 관점: qnode는 아직 SQ에 진입하지 못하고 throtl 큐에 묶인 bio들을
 * 모아두는 그릇이며, blkg reference를 유지해 bio가 NVMe 쪽으로 디스패치될
 * 때까지 cgroup 객체가 해제되지 않도록 한다.
 */
static void throtl_qnode_init(struct throtl_qnode *qn, struct throtl_grp *tg)
{
	INIT_LIST_HEAD(&qn->node); /* qnode를 service_queue의 queued[rw] 리스트에 연결하기 위한 list head 초기화 */
	bio_list_init(&qn->bios_bps); /* bps 제한으로 NVMe SQ 진입이 지연된 bio 리스트 초기화 */
	bio_list_init(&qn->bios_iops); /* iops 제한으로 NVMe SQ 진입이 지연된 bio 리스트 초기화 */
	qn->tg = tg; /* bio가 NVMe로 디스패치될 때까지 cgroup(blkg) 참조 유지 */
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
	bool rw = bio_data_dir(bio); /* bio의 READ/WRITE 방향; NVMe SQ/CQ에서의 데이터 방향과 일치 */

	/*
	 * Split bios have already been throttled by bps, so they are
	 * directly queued into the iops path.
	 */
	if (bio_flagged(bio, BIO_TG_BPS_THROTTLED) || /* 분할된 bio는 이미 bps 제한을 통과했으므로 iops 경로로 직접 진입 (NVMe CID 할당 대기) */
	    bio_flagged(bio, BIO_BPS_THROTTLED)) {
		bio_list_add(&qn->bios_iops, bio); /* NVMe SQ 진입 전 iops 제한 대기열 */
		sq->nr_queued_iops[rw]++; /* iops 제한 대기열 카운트 증가: NVMe 초당 명령 한도 초과 여부 추적 */
	} else {
		bio_list_add(&qn->bios_bps, bio); /* bps 제한으로 인한 NVMe 유입 지연 */
		sq->nr_queued_bps[rw]++; /* bps 제한 대기열 카운트 증가: NVMe 대역폭 한도 초과 여부 추적 */
	}

	if (list_empty(&qn->node)) { /* qnode가 처음 활성화될 때만 list에 추가; NVMe 진입 전 cgroup 큐에 편입 */
		list_add_tail(&qn->node, &sq->queued[rw]); /* bio를 throtl service_queue queued[rw]에 추가; NVMe 드라이버 이전 소프트웨어 대기열 */
		blkg_get(tg_to_blkg(qn->tg)); /* NVMe 디스패치 전까지 blkg 유지 */
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
static struct bio *throtl_peek_queued(struct list_head *queued)
{
	struct throtl_qnode *qn;
	struct bio *bio;

	if (list_empty(queued)) /* 대기열이 비면 NVMe로 보낼 bio 없음 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* round-robin 순서에서 첫 번째 cgroup qnode 선택 */
	bio = bio_list_peek(&qn->bios_iops); /* iops 큐에서 먼저 peek: NVMe 초당 명령 수 제한 우선 적용 */
	if (!bio)
		bio = bio_list_peek(&qn->bios_bps); /* iops 큐가 비면 bps 큐에서 peek: 대역폭 제한 bio를 NVMe로 */
	WARN_ON_ONCE(!bio); /* 양쪽 큐 모두 비어 있으면 throttle 상태 비일관성 (버그) */
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
	struct list_head *queued = &sq->queued[rw]; /* READ/WRITE 큐 중 하나를 선택하여 NVMe 진입 후보 bio 탐색 */
	struct throtl_qnode *qn;
	struct bio *bio;

	if (list_empty(queued)) /* 선택한 방향의 throtl 큐가 비어 있으면 디스패치 불가 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* round-robin 위치의 qnode에서 bio 꺼냄 */
	bio = bio_list_pop(&qn->bios_iops); /* NVMe iops 제한을 우선적으로 소진 */
	if (bio) {
		sq->nr_queued_iops[rw]--; /* iops 큐에서 bio를 꺼낸 경우 NVMe 초당 명령 카운트 감소 */
	} else {
		bio = bio_list_pop(&qn->bios_bps); /* bps 제한 bio를 이제 NVMe로 */
		if (bio) /* bps 큐에서 bio를 꺼내 NVMe 대역폭 예산 소모 */
			sq->nr_queued_bps[rw]--; /* bps 큐 카운트 감소; 남은 bps 예산은 이후 bio에 영향 */
	}
	WARN_ON_ONCE(!bio);

	if (bio_list_empty(&qn->bios_bps) && bio_list_empty(&qn->bios_iops)) { /* qnode의 두 큐가 모두 비면 cgroup이 더 이상 NVMe 진입 후보가 아님 */
		list_del_init(&qn->node); /* qnode를 service_queue에서 제거; NVMe dispatch candidate 해제 */
		if (tg_to_put)
			*tg_to_put = qn->tg; /* blkg reference 해제는 bio 전달 완료 후 호출자가 담당 */
		else
			blkg_put(tg_to_blkg(qn->tg)); /* qnode 제거 시점에 blkg reference 해제; NVMe 진입 전 메모리 누수 방지 */
	} else {
		list_move_tail(&qn->node, queued); /* 다음 cgroup에게 NVMe 디스패치 기회 부여 */
	}

	return bio;
}

/* init a service_queue, assumes the caller zeroed it */
static void throtl_service_queue_init(struct throtl_service_queue *sq)
{
	INIT_LIST_HEAD(&sq->queued[READ]); /* READ 방향 throtl 대기열 초기화: NVMe SQ 진입 전 READ bio 큐 */
	INIT_LIST_HEAD(&sq->queued[WRITE]); /* WRITE 방향 throtl 대기열 초기화: NVMe SQ 진입 전 WRITE bio 큐 */
	sq->pending_tree = RB_ROOT_CACHED; /* pending_tree 초기화: 다음 NVMe doorbell 시점이 가까운 cgroup 정렬 */
	timer_setup(&sq->pending_timer, throtl_pending_timer_fn, 0); /* pending_timer 초기화: NVMe SQ로 bio를 풀어줄 시점을 지연시키는 소프트웨어 타이머 */
}

/*
 * throtl_pd_alloc: blkcg policy data를 할당하고 throtl_grp을 초기화한다.
 * 호출 경로: blkcg_activate_policy() -> pd_alloc_fn -> throtl_pd_alloc.
 * NVMe 연결: 새 cgroup이 NVMe 장치에 대해 활성화될 때마다 생성되며,
 * bps/iops 상한을 기본 무제한으로 시작한다.
 */
static struct blkg_policy_data *throtl_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct throtl_grp *tg;
	int rw;

	tg = kzalloc_node(sizeof(*tg), gfp, disk->node_id); /* per-node 메모리 할당; NVMe namespace와 동일 NUMA 노드 선호 */
	if (!tg) /* 메모리 부족 시 NVMe 장치에 대한 throttle 초기화 실패 (abort) */
		return NULL;

	if (blkg_rwstat_init(&tg->stat_bytes, gfp)) /* bps 통계용 rwstat 초기화; NVMe 대역폭 사용량 집계 */
		goto err_free_tg;

	if (blkg_rwstat_init(&tg->stat_ios, gfp)) /* iops 통계용 rwstat 초기화; NVMe 초당 명령 수 집계 */
		goto err_exit_stat_bytes;

	throtl_service_queue_init(&tg->service_queue);

	for (rw = READ; rw <= WRITE; rw++) { /* READ/WRITE 양쪽에 대해 NVMe SQ 진입 전 대기 qnode 준비 */
		throtl_qnode_init(&tg->qnode_on_self[rw], tg); /* 자신의 service_queue용 qnode; NVMe 진입 지연 bio 수용 */
		throtl_qnode_init(&tg->qnode_on_parent[rw], tg); /* 부모 service_queue용 qnode; 상위 cgroup으로 NVMe 진입 후보 전달 */
	}

	RB_CLEAR_NODE(&tg->rb_node);
	tg->bps[READ] = U64_MAX; /* READ bps 기본 무제한; NVMe SQ READ 대역폭 제한 없음 */
	tg->bps[WRITE] = U64_MAX; /* WRITE bps 기본 무제한; NVMe SQ WRITE 대역폭 제한 없음 */
	tg->iops[READ] = UINT_MAX; /* READ iops 기본 무제한; NVMe SQ READ 초당 명령 제한 없음 */
	tg->iops[WRITE] = UINT_MAX; /* WRITE iops 기본 무제한; NVMe SQ WRITE 초당 명령 제한 없음 */

	return &tg->pd;

err_exit_stat_bytes:
	blkg_rwstat_exit(&tg->stat_bytes);
err_free_tg:
	kfree(tg);
	return NULL;
}

/*
 * throtl_pd_init: 할당된 policy data를 부모 service_queue에 연결한다.
 * NVMe 연결: cgroup階層이 곧 NVMe SQ에 도달하기 전의 IO 우선순위/제한
 * 트리가 된다. v1(default hierarchy)에서는 root 바로 아래가 최상위,
 * v2에서는 상속 구조가 반영된다.
 */
static void throtl_pd_init(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	struct blkcg_gq *blkg = tg_to_blkg(tg);
	struct throtl_data *td = blkg->q->td; /* blkcg_gq -> request_queue -> throtl_data 연결; NVMe namespace 단위 throttle */
	struct throtl_service_queue *sq = &tg->service_queue;

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
	sq->parent_sq = &td->service_queue; /* v1(non-dfl)에서는 모든 throtl_grp이 throtl_data의 최상위 service_queue 아래로 평탄화 */
	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && blkg->parent) /* v2(dfl)에서 parent가 있으면 상위 cgroup의 service_queue를 부모로 설정 (계층적 NVMe QoS) */
		sq->parent_sq = &blkg_to_tg(blkg->parent)->service_queue;
	tg->td = td; /* throtl_data 역참조; NVMe namespace 단위 dispatch_work/pending_timer 접근용 */
}

/*
 * Set has_rules[] if @tg or any of its parents have limits configured.
 * This doesn't require walking up to the top of the hierarchy as the
 * parent's has_rules[] is guaranteed to be correct.
 *
 * NVMe 관점: 자신이나 조상에게 bps/iops 제한이 있는지 표시한다. 제한이
 * 전혀 없는 cgroup은 blk-throttle의 모든 검사를 생략하고 곧바로
 * nvme_queue_rq() 쪽으로 흘려볼 수 있다.
 */
static void tg_update_has_rules(struct throtl_grp *tg)
{
	struct throtl_grp *parent_tg = sq_to_tg(tg->service_queue.parent_sq); /* 부모 cgroup의 제한 상태를 상속받아 NVMe SQ 유입 제한 범위 결정 */
	int rw;

	for (rw = READ; rw <= WRITE; rw++) { /* READ/WRITE 양쪽에 대해 NVMe SQ/CQ 방향별 규칙 존재 여부 갱신 */
		tg->has_rules_iops[rw] = /* iops 규칙: 부모 또는 자신에게 제한이 있으면 NVMe 초당 명령 제한 적용 */
			(parent_tg && parent_tg->has_rules_iops[rw]) ||
			tg_iops_limit(tg, rw) != UINT_MAX;
		tg->has_rules_bps[rw] = /* bps 규칙: 부모 또는 자신에게 제한이 있으면 NVMe 대역폭 제한 적용 */
			(parent_tg && parent_tg->has_rules_bps[rw]) ||
			tg_bps_limit(tg, rw) != U64_MAX;
	}
}

static void throtl_pd_online(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	/*
	 * We don't want new groups to escape the limits of its ancestors.
	 * Update has_rules[] after a new group is brought online.
	 */
	tg_update_has_rules(tg); /* cgroup online 시 제한 상태 갱신; 이후 bio부터 NVMe SQ 유입 제한 결정 */
}

static void throtl_pd_free(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd);

	timer_delete_sync(&tg->service_queue.pending_timer); /* throtl_grp 소멸 시 pending_timer 정지; 더 이상 NVMe doorbell 지연 예약 불필요 */
	blkg_rwstat_exit(&tg->stat_bytes);
	blkg_rwstat_exit(&tg->stat_ios);
	kfree(tg); /* throtl_grp 메모리 해제; NVMe namespace throttle 계층에서 제거 */
}

static struct throtl_grp *
throtl_rb_first(struct throtl_service_queue *parent_sq)
{
	struct rb_node *n;

	n = rb_first_cached(&parent_sq->pending_tree); /* pending_tree에서 dispatch 시점이 가장 이른 cgroup 탐색 */
	WARN_ON_ONCE(!n); /* pending_tree가 비어 있으면 상태 비일관성 (버그) */
	if (!n) /* NVMe로 풀어줄 pending cgroup이 없음 */
		return NULL;
	return rb_entry_tg(n);
}

static void throtl_rb_erase(struct rb_node *n,
			    struct throtl_service_queue *parent_sq)
{
	rb_erase_cached(n, &parent_sq->pending_tree); /* disptime이 도래한 cgroup을 pending_tree에서 제거; NVMe dispatch 라운드 완료 */
	RB_CLEAR_NODE(n);
}

static void update_min_dispatch_time(struct throtl_service_queue *parent_sq)
{
	struct throtl_grp *tg;

	tg = throtl_rb_first(parent_sq);
	if (!tg)
		return;

	parent_sq->first_pending_disptime = tg->disptime; /* 가장 임박한 disptime을 부모 service_queue에 기록; NVMe doorbell 타이머 만료 시점 결정 */
}

static void tg_service_queue_add(struct throtl_grp *tg)
{
	struct throtl_service_queue *parent_sq = tg->service_queue.parent_sq; /* 부모 service_queue의 pending_tree에 이 cgroup을 삽입 */
	struct rb_node **node = &parent_sq->pending_tree.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct throtl_grp *__tg;
	unsigned long key = tg->disptime; /* disptime을 key로 사용; NVMe SQ로 bio를 풀어줄 시간 기준 정렬 */
	bool leftmost = true; /* leftmost 후보 여부; NVMe doorbell 타이머의 다음 만료 대상 판단 */

	while (*node != NULL) { /* RB tree 삽입 순회; disptime 기준으로 NVMe dispatch 우선순위 확정 */
		parent = *node;
		__tg = rb_entry_tg(parent);

		if (time_before(key, __tg->disptime)) /* 더 이른 disptime이면 왼쪽 서브트리; NVMe doorbell을 먼저 칠 cgroup */
			node = &parent->rb_left;
		else {
			node = &parent->rb_right; /* 같거나 늦으면 오른쪽 서브트리; NVMe doorbell 시점이 미래인 cgroup */
			leftmost = false;
		}
	}

	rb_link_node(&tg->rb_node, parent, node); /* cgroup 노드를 pending_tree에 연결; NVMe dispatch 예약 트리 구성 */
	rb_insert_color_cached(&tg->rb_node, &parent_sq->pending_tree, /* RB tree 균형 복원; 다음 NVMe SQ 진입 시점 탐색 트리 유지 */
			       leftmost);
}

static void throtl_enqueue_tg(struct throtl_grp *tg)
{
	if (!(tg->flags & THROTL_TG_PENDING)) { /* THROTL_TG_PENDING 비트 테스트; 이미 NVMe dispatch 예약된 cgroup인지 확인 */
		tg_service_queue_add(tg);
		tg->flags |= THROTL_TG_PENDING; /* pending_tree에 등록됨 표시; NVMe doorbell 타이머 대상이 됨 */
		tg->service_queue.parent_sq->nr_pending++; /* 부모 service_queue의 pending cgroup 수 증가; NVMe SQ로 풀어줄 후보 증가 */
	}
}

static void throtl_dequeue_tg(struct throtl_grp *tg)
{
	if (tg->flags & THROTL_TG_PENDING) { /* THROTL_TG_PENDING 비트 테스트; NVMe dispatch 예약 해제 대상 확인 */
		struct throtl_service_queue *parent_sq =
			tg->service_queue.parent_sq;

		throtl_rb_erase(&tg->rb_node, parent_sq); /* pending_tree에서 제거; 더 이상 NVMe doorbell 타이머의 후보가 아님 */
		--parent_sq->nr_pending; /* 부모 pending cgroup 수 감소; NVMe SQ 진입 후보 감소 */
		tg->flags &= ~THROTL_TG_PENDING; /* pending 플래그 클리어; NVMe doorbell 예약 해제 완료 */
	}
}

/* Call with queue lock held */
static void throtl_schedule_pending_timer(struct throtl_service_queue *sq,
					  unsigned long expires)
{
	unsigned long max_expire = jiffies + 8 * DFL_THROTL_SLICE; /* pending_timer 최대 만료 시점을 8 slice로 제한; NVMe doorbell 지연 과다 방지 */

	/*
	 * Since we are adjusting the throttle limit dynamically, the sleep
	 * time calculated according to previous limit might be invalid. It's
	 * possible the cgroup sleep time is very long and no other cgroups
	 * have IO running so notify the limit changes. Make sure the cgroup
	 * doesn't sleep too long to avoid the missed notification.
	 */
	if (time_after(expires, max_expire)) /* 동적 제한 변경 시 과도한 NVMe doorbell 지연을 방지하기 위해 만료 시점 상한 조정 */
		expires = max_expire;
	mod_timer(&sq->pending_timer, expires); /* pending_timer 갱신; 실제 NVMe SQ로 bio를 풀어줄 시점 지연/예약 */
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
	if (!sq->nr_pending) /* pending_tree에 후보가 없으면 NVMe doorbell 예약 불필요 */
		return true;

	update_min_dispatch_time(sq); /* 가장 이른 disptime 갱신; NVMe doorbell 다음 시점 계산 */

	/* is the next dispatch time in the future? */
	if (force || time_after(sq->first_pending_disptime, jiffies)) { /* force 또는 disptime이 미래면 NVMe doorbell을 그 시점까지 지연 */
		throtl_schedule_pending_timer(sq, sq->first_pending_disptime); /* NVMe SQ로 bio를 풀어줄 시점에 pending_timer arm */
		return true;
	}

	/* tell the caller to continue dispatching */
	return false; /* dispatch window가 열려 있음; NVMe SQ로 즉시 추가 bio 전달 가능 */
}

/*
 * throtl_start_new_slice_with_credit: 이전 slice가 만료되었을 때 새 slice를
 * 시작하고, 미사용 대역폭을 credit으로 이월한다.
 * NVMe 연결: NVMe SSD가 일시 유휴 후 burst를 허용해야 할 때 credit을
 * 부여해 평균 bps/IOPS는 유지하면서도 순간적인 SQ 채움을 가능하게 한다.
 */
static inline void throtl_start_new_slice_with_credit(struct throtl_grp *tg,
		bool rw, unsigned long start)
{
	tg->bytes_disp[rw] = 0; /* 새 slice 시작 시 NVMe 대역폭 사용량 초기화 */
	tg->io_disp[rw] = 0; /* 새 slice 시작 시 NVMe 초당 명령 사용량 초기화 */

	/*
	 * Previous slice has expired. We must have trimmed it after last
	 * bio dispatch. That means since start of last slice, we never used
	 * that bandwidth. Do try to make use of that bandwidth while giving
	 * credit.
	 */
	if (time_after(start, tg->slice_start[rw])) /* 이전 slice 미사용 기간을 credit으로 이월; NVMe 유휴 후 burst 허용 */
		tg->slice_start[rw] = start;

	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* 새로운 시간 윈도우 종료 시점; 이 구간 내에서 NVMe 평균 rate 제한 */
	throtl_log(&tg->service_queue,
		   "[%c] new slice with credit start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

static inline void throtl_start_new_slice(struct throtl_grp *tg, bool rw,
					  bool clear)
{
	if (clear) {
		tg->bytes_disp[rw] = 0; /* slice 사용량 초기화; NVMe SQ로 보낼을 수 있는 바이트 예산 리셋 */
		tg->io_disp[rw] = 0; /* slice 사용량 초기화; NVMe SQ로 보낼을 수 있는 명령 수 예산 리셋 */
	}
	tg->slice_start[rw] = jiffies; /* slice 시작 시점; NVMe doorbell rate limit 기준 시간 */
	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* slice 종료 시점; 이 시점까지 NVMe 평균 bps/iops 제한 유효 */

	throtl_log(&tg->service_queue,
		   "[%c] new slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

static inline void throtl_set_slice_end(struct throtl_grp *tg, bool rw,
					unsigned long jiffy_end)
{
	tg->slice_end[rw] = roundup(jiffy_end, DFL_THROTL_SLICE); /* slice 종료 시점을 DFL_THROTL_SLICE 단위로 정렬; NVMe rate limit 윈도우 경계 맞춤 */
}

static inline void throtl_extend_slice(struct throtl_grp *tg, bool rw,
				       unsigned long jiffy_end)
{
	if (!time_before(tg->slice_end[rw], jiffy_end)) /* slice가 이미 충분히 길면 NVMe rate limit 윈도우 연장 불필요 */
		return;

	throtl_set_slice_end(tg, rw, jiffy_end); /* NVMe rate limit 시간 윈도우 연장; 다음 bio가 더 늦게 SQ에 도달해도 허용 */
	throtl_log(&tg->service_queue,
		   "[%c] extend slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/* Determine if previously allocated or extended slice is complete or not */
static bool throtl_slice_used(struct throtl_grp *tg, bool rw)
{
	if (time_in_range(jiffies, tg->slice_start[rw], tg->slice_end[rw])) /* 현재 jiffy가 slice 범위 내에 있으면 NVMe rate limit 윈도우가 아직 유효 */
		return false;

	return true;
}

static unsigned int sq_queued(struct throtl_service_queue *sq, int type)
{
	return sq->nr_queued_bps[type] + sq->nr_queued_iops[type]; /* bps/iops 대기열 합산; NVMe SQ 진입 지연 중인 총 bio 수 */
}

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

	tmp = (u64)iops_limit * jiffy_elapsed; /* iops_limit * 경과 시간; NVMe 초당 명령 허용량 계산 */
	do_div(tmp, HZ); /* HZ로 나눠 초 단위 변환; NVMe IOPS를 jiffy 기준으로 환산 */

	if (tmp > UINT_MAX)
		io_allowed = UINT_MAX; /* 허용량이 UINT_MAX를 넘으면 NVMe IOPS 제한 사실상 무제한 */
	else
		io_allowed = tmp;

	return io_allowed;
}

static u64 calculate_bytes_allowed(u64 bps_limit, unsigned long jiffy_elapsed)
{
	/*
	 * Can result be wider than 64 bits?
	 * We check against 62, not 64, due to ilog2 truncation.
	 */
	if (ilog2(bps_limit) + ilog2(jiffy_elapsed) - ilog2(HZ) > 62)
		return U64_MAX; /* bps*경과시간이 64bit를 넘을 수 있으면 NVMe 대역폭 제한 무제한 처리 */
	return mul_u64_u64_div_u64(bps_limit, (u64)jiffy_elapsed, (u64)HZ); /* bps * 경과 시간 / HZ; NVMe 대역폭 허용량(바이트) 계산 */
}

/*
 * throtl_trim_bps: 경과 시간만큼의 bps 예산을 slice 사용량에서 차감한다.
 * NVMe 연결: NVMe SSD로 실제 디스패치된 바이트만큼 남은 전송 가능량을
 * 줄여, 평균 bps 상한을 넘지 않도록 한다.
 */
static long long throtl_trim_bps(struct throtl_grp *tg, bool rw,
				 unsigned long time_elapsed)
{
	u64 bps_limit = tg_bps_limit(tg, rw); /* cgroup의 READ/WRITE bps 상한; NVMe PCIe/낸드 대역폭 소프트웨어 제한 */
	long long bytes_trim;

	if (bps_limit == U64_MAX) /* bps 제한이 없으면 NVMe 대역폭 예산 차감 불필요 */
		return 0;

	/* Need to consider the case of bytes_allowed overflow. */
	bytes_trim = calculate_bytes_allowed(bps_limit, time_elapsed); /* 경과 시간 동안 허용된 바이트; NVMe SQ로 이미 디스패치된 양 대비 차감 */
	if (bytes_trim <= 0 || tg->bytes_disp[rw] < bytes_trim) { /* 예산 부족 또는 초과 시 NVMe 대역폭 사용량을 0으로 클리어 */
		bytes_trim = tg->bytes_disp[rw]; /* 실제 차감량은 현재 사용량으로 제한; NVMe rate 계산 안정화 */
		tg->bytes_disp[rw] = 0;
	} else {
		tg->bytes_disp[rw] -= bytes_trim; /* 사용량에서 허용량 차감; 남은 NVMe 대역폭 예산 감소 */
	}

	return bytes_trim;
}

/*
 * throtl_trim_iops: 경과 시간만큼의 iops 예산을 slice 사용량에서 차감한다.
 * NVMe 연결: NVMe SQ에 들어간 명령 수만큼 남은 초당 명령 한도를 줄인다.
 */
static int throtl_trim_iops(struct throtl_grp *tg, bool rw,
			    unsigned long time_elapsed)
{
	u32 iops_limit = tg_iops_limit(tg, rw); /* cgroup의 READ/WRITE iops 상한; NVMe SQ 초당 명령 수 제한 */
	int io_trim;

	if (iops_limit == UINT_MAX) /* iops 제한이 없으면 NVMe 초당 명령 예산 차감 불필요 */
		return 0;

	/* Need to consider the case of io_allowed overflow. */
	io_trim = calculate_io_allowed(iops_limit, time_elapsed); /* 경과 시간 동안 허용된 IO 수; NVMe CID/tag 할당 예산 계산 */
	if (io_trim <= 0 || tg->io_disp[rw] < io_trim) { /* 예산 부족 시 NVMe 초당 명령 사용량 0 클리어 */
		io_trim = tg->io_disp[rw];
		tg->io_disp[rw] = 0;
	} else {
		tg->io_disp[rw] -= io_trim; /* 사용량에서 허용 IO 수 차감; 남은 NVMe 초당 명령 예산 감소 */
	}

	return io_trim;
}

/* Trim the used slices and adjust slice start accordingly */
/*
 * throtl_trim_slice: 오래된 slice 사용량을 정리해 평균 rate를 재조정한다.
 * 호출 경로: tg_dispatch_one_bio(), __blk_throtl_bio() 등에서 bio 디스패치
 * 직후 또는 직접 통과 후 호출.
 * NVMe 연결: NVMe SSD에 doorbell을 친 후 실제 전송/완료에 상관없이
 * 디스패치 시점에 계산하므로, slice가 과장되지 않게 자른다. (추정) 실제
 * NVMe 완료(CQ entry) 시점이 아니라 디스패치 시점에 계산하므로, 큐잉
 * 지연이 클 경우 실제 SSD 처리율과는 약간 차이가 날 수 있다.
 */
static inline void throtl_trim_slice(struct throtl_grp *tg, bool rw)
{
	unsigned long time_elapsed;
	long long bytes_trim;
	int io_trim;

	BUG_ON(time_before(tg->slice_end[rw], tg->slice_start[rw])); /* slice_end가 slice_start보다 과거면 버그; NVMe rate limit 상태 비일관 */

	/*
	 * If bps are unlimited (-1), then time slice don't get
	 * renewed. Don't try to trim the slice if slice is used. A new
	 * slice will start when appropriate.
	 */
	if (throtl_slice_used(tg, rw)) /* slice가 만료되면 차감하지 않고 새 slice 시작 시점을 기다림; NVMe rate 윈도우 재설정 */
		return;

	/*
	 * A bio has been dispatched. Also adjust slice_end. It might happen
	 * that initially cgroup limit was very low resulting in high
	 * slice_end, but later limit was bumped up and bio was dispatched
	 * sooner, then we need to reduce slice_end. A high bogus slice_end
	 * is bad because it does not allow new slice to start.
	 */
	throtl_set_slice_end(tg, rw, jiffies + DFL_THROTL_SLICE); /* bio가 디스패치되었으므로 slice 종료 시점을 현재 기준으로 재조정; NVMe rate limit 윈도우 보정 */

	time_elapsed = rounddown(jiffies - tg->slice_start[rw], /* 경과 시간을 slice 단위로 내림; NVMe rate 계산의 과대 추정 방지 */
				 DFL_THROTL_SLICE);
	/* Don't trim slice until at least 2 slices are used */
	if (time_elapsed < DFL_THROTL_SLICE * 2) /* 2 slice 이상 사용된 경우에만 정리; 짧은 NVMe burst를 불필요하게 자르지 않음 */
		return;

	/*
	 * The bio submission time may be a few jiffies more than the expected
	 * waiting time, due to 'extra_bytes' can't be divided in
	 * tg_within_bps_limit(), and also due to timer wakeup delay. In this
	 * case, adjust slice_start will discard the extra wait time, causing
	 * lower rate than expected. Therefore, other than the above rounddown,
	 * one extra slice is preserved for deviation.
	 */
	time_elapsed -= DFL_THROTL_SLICE; /* 오차 보정을 위해 한 slice 유지; NVMe doorbell 간 실제 지연과 rate 계산 오차 완충 */
	bytes_trim = throtl_trim_bps(tg, rw, time_elapsed); /* bps 예산 정리; NVMe 대역폭 사용량을 시간 경과에 맞춤 */
	io_trim = throtl_trim_iops(tg, rw, time_elapsed); /* iops 예산 정리; NVMe 초당 명령 사용량을 시간 경과에 맞춤 */
	if (!bytes_trim && !io_trim) /* bps/iops 모두 정리할 양이 없으면 NVMe rate limit 보정 불필요 */
		return;

	tg->slice_start[rw] += time_elapsed; /* slice 시작을 앞당겨 남은 예산 재계산; NVMe SQ 유입 rate 재조정 */

	throtl_log(&tg->service_queue,
		   "[%c] trim slice nr=%lu bytes=%lld io=%d start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', time_elapsed / DFL_THROTL_SLICE,
		   bytes_trim, io_trim, tg->slice_start[rw], tg->slice_end[rw],
		   jiffies);
}

/*
 * __tg_update_carryover: 설정 변경 시 이전 제한 하에서 기다린 양을 계산해
 * 새 slice의 시작값(음수)으로 반영한다.
 * NVMe 연결: 운영자가 cgroup의 bps/iops를 낮추거나 높일 때, 이미 NVMe
 * SQ에 들어가지 못하고 대기한 bio들의 손해/이익을 새 기준에 반영한다.
 */
static void __tg_update_carryover(struct throtl_grp *tg, bool rw,
				  long long *bytes, int *ios)
{
	unsigned long jiffy_elapsed = jiffies - tg->slice_start[rw]; /* 현재 slice에서 경과한 jiffy; NVMe rate limit 변경 시 누적 대기량 산정 */
	u64 bps_limit = tg_bps_limit(tg, rw); /* 새 bps 기준 하에서 이미 기다린 시간 환산; NVMe SQ 진입 지연 손실 보정 */
	u32 iops_limit = tg_iops_limit(tg, rw); /* 새 iops 기준 하에서 이미 기다린 시간 환산; NVMe 초당 명령 지연 보정 */
	long long bytes_allowed;
	int io_allowed;

	/*
	 * If the queue is empty, carryover handling is not needed. In such cases,
	 * tg->[bytes/io]_disp should be reset to 0 to avoid impacting the dispatch
	 * of subsequent bios. The same handling applies when the previous BPS/IOPS
	 * limit was set to max.
	 */
	if (sq_queued(&tg->service_queue, rw) == 0) { /* 큐가 비어 있으면 carryover 불필요; NVMe SQ로 진입 예정인 bio가 없음 */
		tg->bytes_disp[rw] = 0; /* bps 사용량 초기화; 새 NVMe rate limit 기준에서 예산 재시작 */
		tg->io_disp[rw] = 0; /* iops 사용량 초기화; 새 NVMe rate limit 기준에서 예산 재시작 */
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
		bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed); /* 새 bps 기준 경과 시간 허용량; NVMe 대역폭 제한 변경 후 잔여/초과량 계산 */
		if (bytes_allowed > 0)
			*bytes = bytes_allowed - tg->bytes_disp[rw];
	}
	if (iops_limit != UINT_MAX) {
		io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed); /* 새 iops 기준 경과 시간 허용량; NVMe 초당 명령 제한 변경 후 잔여/초과량 계산 */
		if (io_allowed > 0)
			*ios = io_allowed - tg->io_disp[rw];
	}

	tg->bytes_disp[rw] = -*bytes; /* 새 bps 기준에서의 잔여/초과 허용량 */
	tg->io_disp[rw] = -*ios;      /* 새 iops 기준에서의 잔여/초과 허용량 */
}

/*
 * tg_update_carryover: READ/WRITE 양쪽에 대해 carryover를 갱신한다.
 * 호출 경로: tg_set_conf(), tg_set_limit() 등 cgroup limit 변경 시.
 */
static void tg_update_carryover(struct throtl_grp *tg)
{
	long long bytes[2] = {0};
	int ios[2] = {0};

	__tg_update_carryover(tg, READ, &bytes[READ], &ios[READ]);
	__tg_update_carryover(tg, WRITE, &bytes[WRITE], &ios[WRITE]);

	/* see comments in struct throtl_grp for meaning of carryover. */
	throtl_log(&tg->service_queue, "%s: %lld %lld %d %d\n", __func__,
		   bytes[READ], bytes[WRITE], ios[READ], ios[WRITE]);
}

/*
 * tg_within_iops_limit: bio가 현재 iops slice 안에 들어갈 수 있는지 검사.
 * 반환값: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 * NVMe 연결: NVMe SQ의 Queue Depth보다 상위에서 초당 명령 수를 제어.
 * CID 할당과 doorbell 횟수를 간접적으로 제한한다.
 */
static unsigned long tg_within_iops_limit(struct throtl_grp *tg, struct bio *bio,
				 u32 iops_limit)
{
	bool rw = bio_data_dir(bio); /* bio의 READ/WRITE 방향; NVMe SQ/CQ 방향과 일치 */
	int io_allowed;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;

	jiffy_elapsed = jiffies - tg->slice_start[rw]; /* 현재 slice에서 경과한 시간; NVMe IOPS 윈도우 내 허용 명령 수 계산 */

	/* Round up to the next throttle slice, wait time must be nonzero */
	jiffy_elapsed_rnd = roundup(jiffy_elapsed + 1, DFL_THROTL_SLICE); /* 다음 slice 경계로 올림; NVMe IOPS 계산에서 0으로 나누기 및 무한대 방지 */
	io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed_rnd); /* 현재까지 허용된 IO 수; NVMe CID 할당 가능 여부 판단 */
	if (io_allowed > 0 && tg->io_disp[rw] + 1 <= io_allowed) /* 사용량+1이 허용량 이하면 이 bio를 즉시 NVMe SQ로 보낼 수 있음 */
		return 0;

	/* Calc approx time to dispatch */
	jiffy_wait = jiffy_elapsed_rnd - jiffy_elapsed; /* 다음 slice 경계까지 남은 시간; NVMe doorbell 지연 시간 */

	/* make sure at least one io can be dispatched after waiting */
	jiffy_wait = max(jiffy_wait, HZ / iops_limit + 1); /* 최소 1 IO 이후 디스패치 보장; NVMe IOPS 제한이 매우 낮을 때도 진행 */
	return jiffy_wait;
}

/*
 * tg_within_bps_limit: bio가 현재 bps slice 안에 들어갈 수 있는지 검사.
 * 반환값: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 * NVMe 연결: NVMe SSD의 PCIe 대역폭/낸드 대역폭을 소프트웨어적으로
 * 제한. bio 크기에 따라 doorbell 간 평균 전송률을 조절한다.
 */
static unsigned long tg_within_bps_limit(struct throtl_grp *tg, struct bio *bio,
				u64 bps_limit)
{
	bool rw = bio_data_dir(bio); /* bio 방향; NVMe SQ/CQ READ/WRITE 구분 */
	long long bytes_allowed;
	u64 extra_bytes;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;
	unsigned int bio_size = throtl_bio_data_size(bio); /* bio 크기; NVMe PRP/SGL segment 수와 doorbell 당 전송량에 영향 */

	jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw]; /* slice 시작부터 경과 시간; NVMe 대역폭 윈도우 계산 */

	/* Slice has just started. Consider one slice interval */
	if (!jiffy_elapsed) /* slice 시작 직후면 한 slice 간격으로 간주; NVMe 초기 burst 처리 */
		jiffy_elapsed_rnd = DFL_THROTL_SLICE;

	jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, DFL_THROTL_SLICE); /* slice 경계로 올림; NVMe bps 계산에서 안정적 시간 윈도우 확보 */
	bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed_rnd); /* 현재 윈도우에서 허용된 총 바이트; NVMe 대역폭 예산 */
	/* Need to consider the case of bytes_allowed overflow. */
	if ((bytes_allowed > 0 && tg->bytes_disp[rw] + bio_size <= bytes_allowed) /* bio 추가가 허용량 내이면 즉시 NVMe SQ로 디스패치 가능 */
	    || bytes_allowed < 0)
		return 0;

	/* Calc approx time to dispatch */
	extra_bytes = tg->bytes_disp[rw] + bio_size - bytes_allowed; /* 허용량을 초과하는 바이트; NVMe 대역폭 제한 때문에 기다려야 할 양 */
	jiffy_wait = div64_u64(extra_bytes * HZ, bps_limit); /* 초과 바이트 / bps = 대기 시간; NVMe doorbell 지연 시간 계산 */

	if (!jiffy_wait) /* 최소 1 jiffy 대기; NVMe rate limit이 극단적일 때도 제약 유지 */
		jiffy_wait = 1;

	/*
	 * This wait time is without taking into consideration the rounding
	 * up we did. Add that time also.
	 */
	jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed); /* slice 올림분을 추가; NVMe rate limit 시점 정렬에 따른 지연 반영 */
	return jiffy_wait;
}

/*
 * throtl_charge_bps_bio: bio의 바이트를 bps slice에 기록한다.
 * NVMe 연결: bio가 NVMe SQ로 디스패치되기 전/후 bps 사용량을 차감.
 * 분할된 bio는 중복 계산하지 않는다.
 */
static void throtl_charge_bps_bio(struct throtl_grp *tg, struct bio *bio)
{
	unsigned int bio_size = throtl_bio_data_size(bio); /* bio 크기; NVMe PRP/SGL 및 DMA 전송량에 직접 연결 */

	/* Charge the bio to the group */
	if (!bio_flagged(bio, BIO_BPS_THROTTLED) && /* 이미 bps 제한을 통과한 분할 bio는 중복 차감하지 않음 (NVMe 명령 중복 계산 방지) */
	    !bio_flagged(bio, BIO_TG_BPS_THROTTLED)) {
		bio_set_flag(bio, BIO_TG_BPS_THROTTLED); /* bps 제한 통과 표시; 이후 iops 경로로만 NVMe SQ 진입 검사 */
		tg->bytes_disp[bio_data_dir(bio)] += bio_size; /* NVMe로 풀려나는 바이트 기록 */
	}
}

/*
 * throtl_charge_iops_bio: bio를 iops slice에 기록한다.
 * NVMe 연결: 하나의 bio는 NVMe SQ에 들어가는 하나의 명령으로 간주되며,
 * 초당 명령 수 카운트를 증가시킨다.
 */
static void throtl_charge_iops_bio(struct throtl_grp *tg, struct bio *bio)
{
	bio_clear_flag(bio, BIO_TG_BPS_THROTTLED); /* bps 통과 플래그 클리어; iops 단계에서 NVMe 초당 명령 수만 계산 */
	tg->io_disp[bio_data_dir(bio)]++; /* NVMe CID 하나에 해당하는 IO 사용량 */
}

/*
 * tg_update_slice: bio 디스패치 직전 slice 상태를 갱신한다.
 * NVMe 연결: NVMe 명령을 SQ에 밀어넣기 전에 현재 시간 윈도우가 유효한지
 * 확인하고, 필요하면 새 slice를 시작하거나 기존 slice를 연장한다.
 */
static void tg_update_slice(struct throtl_grp *tg, bool rw)
{
	if (throtl_slice_used(tg, rw) && /* slice가 만료되고 큐가 비면 새로운 NVMe rate limit 윈도우 시작 */
	    sq_queued(&tg->service_queue, rw) == 0)
		throtl_start_new_slice(tg, rw, true); /* 새 slice에서 예산 초기화; NVMe SQ 유입 rate 재설정 */
	else
		throtl_extend_slice(tg, rw, jiffies + DFL_THROTL_SLICE); /* 현재 slice를 연장; NVMe rate limit 윈도우 내에서 계속 bio 허용 */
}

/*
 * tg_dispatch_bps_time: bio가 bps 제한을 만족하는지 검사하고 대기 시간 반환.
 * 반환 0이면 bps 제한 통과.
 * NVMe 연결: bio를 NVMe로 본격적으로 본래 흐름에 태우기 전 마지막 bps 검문.
 */
static unsigned long tg_dispatch_bps_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio);
	u64 bps_limit = tg_bps_limit(tg, rw); /* cgroup의 READ/WRITE bps 상한; NVMe PCIe/낸드 대역폭 제한 */
	unsigned long bps_wait;

	/* no need to throttle if this bio's bytes have been accounted */
	if (bps_limit == U64_MAX || tg->flags & THROTL_TG_CANCELING || /* bps 무제한/취소 중/이미 통과한 bio는 bps 검사 생략 후 NVMe SQ 진입 */
	    bio_flagged(bio, BIO_BPS_THROTTLED) ||
	    bio_flagged(bio, BIO_TG_BPS_THROTTLED))
		return 0;

	tg_update_slice(tg, rw); /* slice 상태 갱신; NVMe rate limit 시간 윈도우 준비 */
	bps_wait = tg_within_bps_limit(tg, bio, bps_limit); /* bps 제한 위반 시 대기 시간; NVMe doorbell 지연량 */
	throtl_extend_slice(tg, rw, jiffies + bps_wait); /* 대기 시간만큼 slice 종료 연장; NVMe SQ 유입 시점 지연 */

	return bps_wait;
}

/*
 * tg_dispatch_iops_time: bio가 iops 제한을 만족하는지 검사하고 대기 시간 반환.
 * 반환 0이면 iops 제한 통과.
 * NVMe 연결: bio 하나가 NVMe 명령 하나로 변환되므로, 이 함수는 SQ/CQ
 * 초당 트랜잭션 수를 직접적으로 억제한다.
 */
static unsigned long tg_dispatch_iops_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio);
	u32 iops_limit = tg_iops_limit(tg, rw); /* cgroup의 READ/WRITE iops 상한; NVMe SQ 초당 명령 수 제한 */
	unsigned long iops_wait;

	if (iops_limit == UINT_MAX || tg->flags & THROTL_TG_CANCELING) /* iops 무제한/취소 중이면 iops 검사 생략 후 NVMe SQ 진입 */
		return 0;

	tg_update_slice(tg, rw);
	iops_wait = tg_within_iops_limit(tg, bio, iops_limit); /* iops 제한 위반 시 대기 시간; NVMe doorbell 지연량 */
	throtl_extend_slice(tg, rw, jiffies + iops_wait); /* 대기 시간만큼 slice 연장; NVMe 초당 명령 유입 지연 */

	return iops_wait;
}

/*
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
	BUG_ON(sq_queued(&tg->service_queue, rw) && /* qnode의 첫 bio가 아니면 tg_dispatch_time() 상태 가정이 깨짐; NVMe dispatch 순서 보장 */
	       bio != throtl_peek_queued(&tg->service_queue.queued[rw]));

	wait = tg_dispatch_bps_time(tg, bio); /* 먼저 bps 제한 검사; 통과하면 iops 제한 검사로 NVMe 명령 수 제한 */
	if (wait != 0) /* bps 대기 시간이 0이 아니면 NVMe SQ 진입 지연 */
		return wait;

	/*
	 * Charge bps here because @bio will be directly placed into the
	 * iops queue afterward.
	 */
	throtl_charge_bps_bio(tg, bio); /* bps 통과 후 사용량 기록; 이제 iops 제한만 남음 */

	return tg_dispatch_iops_time(tg, bio); /* iops 제한 통과 시 0, 실패 시 NVMe doorbell 지연 시간 */
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
	if (sq_queued(sq, rw) == 0) /* 같은 방향의 큐가 비어 있었다면 첫 bio 도착; NVMe dispatch 시점 재계산 필요 */
		tg->flags |= THROTL_TG_WAS_EMPTY; /* NVMe로 갈 bio가 생겼음을 표시 */

	throtl_qnode_add_bio(bio, qn, sq); /* bio를 throtl 큐에 추가; NVMe SQ로의 유입을 일시 지연 */

	/*
	 * Since we have split the queues, when the iops queue is
	 * previously empty and a new @bio is added into the first @qn,
	 * we also need to update the @tg->disptime.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED) && /* 분할 bio가 bps 큐에서 iops 큐로 넘어올 때 첫 bio라면 dispatch 시점 갱신 */
	    bio == throtl_peek_queued(&sq->queued[rw]))
		tg->flags |= THROTL_TG_IOPS_WAS_EMPTY; /* iops 큐도 갱신 필요 */

	throtl_enqueue_tg(tg); /* disptime 기준 pending_tree에 등록 */
}

/*
 * tg_update_disptime: 그룹의 다음 디스패치 시각을 계산하고 pending_tree를
 * 재정렬한다.
 * 호출 경로: __blk_throtl_bio() (WAS_EMPTY/IOPS_WAS_EMPTY 시),
 *           throtl_dispatch_tg() (bio 잔여 시),
 *           tg_flush_bios(), throtl_pending_timer_fn() -> ...
 * NVMe 연결: READ/WRITE 큐의 첫 bio에 대해 tg_dispatch_time()을 호출.
 * 반환된 대기 시간만큼 NVMe doorbell을 미룬다. disptime이 가장 작은
 * 그룹이 pending_tree의 leftmost가 되어 다음 타이머 만료 시점이 된다.
 */
static void tg_update_disptime(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	unsigned long read_wait = -1, write_wait = -1, min_wait, disptime; /* READ/WRITE 큐의 첫 bio별 대기 시간; NVMe SQ 진입 지연 시간 */
	struct bio *bio;

	bio = throtl_peek_queued(&sq->queued[READ]); /* READ 큐의 첫 bio; NVMe READ doorbell 시점 결정 */
	if (bio)
		read_wait = tg_dispatch_time(tg, bio); /* READ 큐 첫 bio의 NVMe 진입 대기 시간 */

	bio = throtl_peek_queued(&sq->queued[WRITE]); /* WRITE 큐의 첫 bio; NVMe WRITE doorbell 시점 결정 */
	if (bio)
		write_wait = tg_dispatch_time(tg, bio); /* WRITE 큐 첫 bio의 NVMe 진입 대기 시간 */

	min_wait = min(read_wait, write_wait); /* READ/WRITE 중 더 짧은 대기 시간; 다음 NVMe doorbell 시점 */
	disptime = jiffies + min_wait; /* 현재 jiffies + min_wait; 실제 NVMe SQ로 bio를 풀어줄 시점 */

	/* Update dispatch time */
	throtl_rb_erase(&tg->rb_node, tg->service_queue.parent_sq); /* pending_tree에서 기존 위치 제거 후 disptime 갱신; NVMe doorbell 예약 재정렬 */
	tg->disptime = disptime; /* cgroup의 다음 NVMe SQ 진입 시각 확정 */
	tg_service_queue_add(tg); /* 갱신된 disptime으로 pending_tree 재삽입; NVMe dispatch 우선순위 갱신 */

	/* see throtl_add_bio_tg() */
	tg->flags &= ~THROTL_TG_WAS_EMPTY; /* WAS_EMPTY 플래그 클리어; NVMe dispatch 시점 재계산 완료 */
	tg->flags &= ~THROTL_TG_IOPS_WAS_EMPTY; /* IOPS_WAS_EMPTY 플래그 클리어; NVMe iops 타이머 재계산 완료 */
}

static void start_parent_slice_with_credit(struct throtl_grp *child_tg,
					struct throtl_grp *parent_tg, bool rw)
{
	if (throtl_slice_used(parent_tg, rw)) { /* 부모 slice가 만료되면 credit과 함께 새 slice 시작; NVMe 대역폭 상속 */
		throtl_start_new_slice_with_credit(parent_tg, rw, /* 자식 slice 시작 시점을 credit 기준으로 부모 slice 시작; NVMe QoS 상속 */
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
	struct throtl_service_queue *sq = &tg->service_queue;
	struct throtl_service_queue *parent_sq = sq->parent_sq; /* 현재 cgroup의 service_queue */
	struct throtl_grp *parent_tg = sq_to_tg(parent_sq); /* 부모 service_queue가 throtl_grp이면 상위 cgroup, 없으면 최상위(장치) */
	struct throtl_grp *tg_to_put = NULL;
	struct bio *bio;

	/*
	 * @bio is being transferred from @tg to @parent_sq.  Popping a bio
	 * from @tg may put its reference and @parent_sq might end up
	 * getting released prematurely.  Remember the tg to put and put it
	 * after @bio is transferred to @parent_sq.
	 */
	bio = throtl_pop_queued(sq, &tg_to_put, rw); /* bio를 현재 cgroup 큐에서 꺼냄; NVMe SQ 진입 후보 확정 */

	throtl_charge_iops_bio(tg, bio); /* iops 사용량 기록; NVMe SQ에 들어갈 하나의 명령으로 과금 */

	/*
	 * If our parent is another tg, we just need to transfer @bio to
	 * the parent using throtl_add_bio_tg().  If our parent is
	 * @td->service_queue, @bio is ready to be issued.  Put it on its
	 * bio_lists[] and decrease total number queued.  The caller is
	 * responsible for issuing these bios.
	 */
	if (parent_tg) { /* 상위 cgroup이 있으면 그곳으로 전달; NVMe SQ 진입 전 추가 제한 검사 */
		throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg); /* 부모 service_queue의 qnode로 bio 추가; 상위 cgroup의 NVMe rate limit 큐로 */
		start_parent_slice_with_credit(tg, parent_tg, rw); /* 부모 slice에 credit 이월; NVMe 대역폭 제한의 하위 cgroup 합산 반영 */
	} else {
		bio_set_flag(bio, BIO_BPS_THROTTLED); /* 최상위 도달: bps 제한 통과 표시 */
		throtl_qnode_add_bio(bio, &tg->qnode_on_parent[rw],
				     parent_sq); /* 최상위 throtl 큐에 추가; NVMe SQ 진입 직전 대기열 */
		BUG_ON(tg->td->nr_queued[rw] <= 0); /* 최상위 대기 카운트가 0 이하이면 상태 비일관 (버그) */
		tg->td->nr_queued[rw]--; /* NVMe SQ로 나가기 직전의 대기 카운트 감소 */
	}

	throtl_trim_slice(tg, rw); /* bio 전달 후 slice 정리; NVMe rate limit 시간 윈도우 보정 */

	if (tg_to_put) /* tg_to_put이 설정되면 bio 전달 후 blkg reference 해제 */
		blkg_put(tg_to_blkg(tg_to_put)); /* blkg reference 해제; NVMe 진행 중이 아닌 cgroup 자원 정리 */
}

/*
 * throtl_dispatch_tg: 한 그룹에서 제한을 통과한 bio들을 부모로 이동.
 * NVMe 연결: READ 75%, WRITE 25% 비율로 한 번에 최대 THROTL_GRP_QUANTUM개
 * bio를 상위로 보낸다. 이 비율은 NVMe SSD의 READ/WRITE 혼합 부하에서
 * starvation을 줄이기 위한 휴리스틱이다.
 */
static int throtl_dispatch_tg(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	unsigned int nr_reads = 0, nr_writes = 0; /* 한 라운드에서 디스패치할 READ/WRITE 카운터; NVMe SQ/CQ 방향별 limit */
	unsigned int max_nr_reads = THROTL_GRP_QUANTUM * 3 / 4; /* 한 라운드 최대 READ 수: 75%; NVMe READ가 WRITE보다 우선권을 갖는 휴리스틱 */
	unsigned int max_nr_writes = THROTL_GRP_QUANTUM - max_nr_reads; /* WRITE는 나머지 25%; NVMe WRITE SQ starvation 완화 */
	struct bio *bio;

	/* Try to dispatch 75% READS and 25% WRITES */

	while ((bio = throtl_peek_queued(&sq->queued[READ])) && /* READ 큐의 첫 bio를 순회; NVMe READ SQ 후보 bio 나열 */
	       tg_dispatch_time(tg, bio) == 0) { /* tg_dispatch_time이 0이면 READ bio를 NVMe SQ로 보낼 준비 완료 */

		tg_dispatch_one_bio(tg, READ); /* READ bio를 NVMe 경로쪽으로 한 단계 전진 */
		nr_reads++;

		if (nr_reads >= max_nr_reads) /* READ quantum 소진 시 중단; NVMe READ SQ가 한 cgroup에 쏠리지 않도록 */
			break;
	}

	while ((bio = throtl_peek_queued(&sq->queued[WRITE])) && /* WRITE 큐의 첫 bio를 순회; NVMe WRITE SQ 후보 bio 나열 */
	       tg_dispatch_time(tg, bio) == 0) { /* tg_dispatch_time이 0이면 WRITE bio를 NVMe SQ로 보낼 준비 완료 */

		tg_dispatch_one_bio(tg, WRITE); /* WRITE bio를 NVMe 경로쪽으로 한 단계 전진 */
		nr_writes++;

		if (nr_writes >= max_nr_writes) /* WRITE quantum 소진 시 중단; NVMe WRITE SQ가 한 cgroup에 쏠리지 않도록 */
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
	unsigned int nr_disp = 0; /* 이번 라운드 총 디스패치 카운트; NVMe SQ batch 크기 추적 */

	while (1) { /* disptime이 도래한 cgroup을 순회하며 NVMe SQ로 bio 풀기 */
		struct throtl_grp *tg;
		struct throtl_service_queue *sq;

		if (!parent_sq->nr_pending) /* pending_tree에 후보가 없으면 NVMe SQ로 보낼 bio 없음 */
			break;

		tg = throtl_rb_first(parent_sq); /* disptime이 가장 이른 cgroup 획득; 다음 NVMe doorbell 대상 */
		if (!tg) /* 예외적으로 pending_tree가 비면 중단 */
			break;

		if (time_before(jiffies, tg->disptime)) /* disptime이 아직 미래면 NVMe doorbell을 아직 치지 않음 */
			break; /* 아직 NVMe로 풀어줄 시각이 아님 */

		nr_disp += throtl_dispatch_tg(tg); /* 해당 cgroup에서 제한을 통과한 bio를 상위/최상위로 이동; NVMe SQ 배치 구성 */

		sq = &tg->service_queue;
		if (sq_queued(sq, READ) || sq_queued(sq, WRITE)) /* READ나 WRITE 큐에 bio가 남아 있으면 다음 NVMe doorbell 시점 재계산 필요 */
			tg_update_disptime(tg); /* 잔여 bio가 있으면 다음 NVMe doorbell 시각 갱신 */
		else /* 큐가 비면 pending_tree에서 제거; 더 이상 NVMe doorbell 대상 아님 */
			throtl_dequeue_tg(tg); /* 큐가 비었으면 pending_tree에서 제거 */

		if (nr_disp >= THROTL_QUANTUM) /* 한 라운드 총 bio 수가 THROTL_QUANTUM에 도달하면 중단; NVMe SQ batch 크기 제한 */
			break;
	}

	return nr_disp; /* 이번 라운드에서 NVMe SQ로 풀어준 총 bio 수 반환 */
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
	if (tg) /* throtl_grp이 있으면 그 queue 사용; 최상위면 throtl_data->queue 사용 */
		q = tg->pd.blkg->q;
	else
		q = td->queue;

	spin_lock_irq(&q->queue_lock); /* request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl 상태 동시 접근 보호 */

	if (!q->root_blkg) /* root_blkg가 없으면 request_queue가 dying/quiesced 상태; NVMe 컨트롤러 정리 중 */
		goto out_unlock;

again:
	parent_sq = sq->parent_sq; /* 부모 service_queue; NVMe QoS 계층에서 한 단계 위로 전파 */
	dispatched = false; /* 이번 타이머 실행에서 bio를 실제로 풀었는지 추적 */

	while (true) { /* dispatch window가 열려 있는 동안 계속 NVMe SQ로 bio 전달 */
		unsigned int __maybe_unused bio_cnt_r = sq_queued(sq, READ); /* READ 대기 bio 수; NVMe READ SQ 진입 지연 상태 */
		unsigned int __maybe_unused bio_cnt_w = sq_queued(sq, WRITE); /* WRITE 대기 bio 수; NVMe WRITE SQ 진입 지연 상태 */

		throtl_log(sq, "dispatch nr_queued=%u read=%u write=%u",
			   bio_cnt_r + bio_cnt_w, bio_cnt_r, bio_cnt_w);

		ret = throtl_select_dispatch(sq); /* disptime이 도래한 cgroup부터 NVMe SQ로 bio 풀기 */
		if (ret) { /* bio를 디스패치했다면 NVMe SQ로 전달된 양 기록 */
			throtl_log(sq, "bios disp=%u", ret);
			dispatched = true; /* NVMe 쪽으로 풀어줄 bio가 있음 */
		}

		if (throtl_schedule_next_dispatch(sq, false)) /* 다음 NVMe doorbell 시점을 예약하고 window가 닫혔으면 중단 */
			break; /* 다음 dispatch window까지 대기; NVMe doorbell 지연 */

		/* this dispatch windows is still open, relax and repeat */
		spin_unlock_irq(&q->queue_lock); /* lock을 풀고 cpu_relax: 다른 CPU가 NVMe 완료/CQ 처리를 진행할 기회 부여 (추정) */
		cpu_relax(); /* 짧은 busy-wait; NVMe CQ ISR과 경쟁하지 않도록 스핀 완화 */
		spin_lock_irq(&q->queue_lock); /* lock 재획득; NVMe SQ/CQ 상태와 throtl 상태 재동기화 */
	}

	if (!dispatched) /* 이번 타이머에서 bio를 하나도 풀지 못하면 정리 종료 */
		goto out_unlock;

	if (parent_sq) {
		/* @parent_sq is another throl_grp, propagate dispatch */
		if (tg->flags & THROTL_TG_WAS_EMPTY || /* 자식 cgroup에 새 bio가 추가되면 부모의 NVMe dispatch 시점도 갱신 필요 */
		    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) {
			tg_update_disptime(tg); /* 부모 pending_tree에서의 disptime 재계산; NVMe doorbell 예약 재정렬 */
			if (!throtl_schedule_next_dispatch(parent_sq, false)) { /* 부모의 dispatch window가 열려 있으면 즉시 상위로 전파; NVMe SQ 유입 연쇄 */
				/* window is already open, repeat dispatching */
				sq = parent_sq; /* 부모 service_queue를 현재 기준으로 삼고 다시 dispatch */
				tg = sq_to_tg(sq); /* 부모 service_queue의 throtl_grp 획득; NVMe QoS 계층 전파 */
				goto again; /* 부모 계층에서 다시 NVMe SQ로 bio 풀기 시도 */
			}
		}
	} else {
		/* reached the top-level, queue issuing */
		queue_work(kthrotld_workqueue, &td->dispatch_work); /* NVMe SQ 최종 진입 workqueue 예약 */
	}
out_unlock:
	spin_unlock_irq(&q->queue_lock);
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
	struct throtl_data *td = container_of(work, struct throtl_data, /* throtl_data에서 dispatch_work 역참조; NVMe namespace 단위 work item */
					      dispatch_work);
	struct throtl_service_queue *td_sq = &td->service_queue; /* throtl_data의 최상위 service_queue; NVMe SQ 진입 직전 bio 모음 */
	struct request_queue *q = td->queue; /* NVMe namespace의 request_queue; dispatch_work가 이 queue에 속함 */
	struct bio_list bio_list_on_stack; /* throttle 통과한 bio를 임시로 모을 bio_list; NVMe SQ로의 배치 전달 준비 */
	struct bio *bio;
	struct blk_plug plug; /* blk_plug: user-space batch를 유지해 NVMe multi-queue의 hctx 분산을 효율적으로 사용 */
	int rw; /* READ/WRITE 양쪽 큐 순회; NVMe SQ/CQ 방향별 bio 수집 */

	bio_list_init(&bio_list_on_stack); /* bio_list_on_stack 초기화; NVMe SQ로 보낼을 bio 버퍼 준비 */

	spin_lock_irq(&q->queue_lock); /* request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl bio list 동시 접근 보호 */
	for (rw = READ; rw <= WRITE; rw++) /* READ/WRITE 방향별로 NVMe SQ 진입 후보 bio를 모두 꺼냄 */
		while ((bio = throtl_pop_queued(td_sq, NULL, rw))) /* throtl 큐에서 bio를 pop; NVMe SQ로 보낼 bio 수집 */
			bio_list_add(&bio_list_on_stack, bio); /* NVMe로 내보낼 bio를 모음 */
	spin_unlock_irq(&q->queue_lock); /* lock 해제; NVMe 드라이버가 request 할당/CID 배정/doorbell을 진행 가능 */

	if (!bio_list_empty(&bio_list_on_stack)) { /* throttle 통과 bio가 있을 때만 plug 시작; 불필요한 NVMe dispatch 오버헤드 방지 */
		blk_start_plug(&plug); /* blk_plug 시작; NVMe multi-queue에서 인접 bio를 같은 hctx/hardware queue로 batch 처리 */
		while ((bio = bio_list_pop(&bio_list_on_stack))) /* bio_list에서 하나씩 pop; NVMe 드라이버가 개별 request/CID를 할당 */
			submit_bio_noacct_nocheck(bio, false); /* blk-mq -> NVMe 드라이버로 전달 */
		blk_finish_plug(&plug); /* blk_plug 종료; plug된 bio들이 NVMe hardware queue에 분산 dispatch됨 */
	}
}

static u64 tg_prfill_conf_u64(struct seq_file *sf, struct blkg_policy_data *pd,
			      int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	u64 v = *(u64 *)((void *)tg + off);

	if (v == U64_MAX)
		return 0;
	return __blkg_prfill_u64(sf, pd, v);
}

static u64 tg_prfill_conf_uint(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	unsigned int v = *(unsigned int *)((void *)tg + off);

	if (v == UINT_MAX)
		return 0;
	return __blkg_prfill_u64(sf, pd, v);
}

static int tg_print_conf_u64(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_u64,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false);
	return 0;
}

static int tg_print_conf_uint(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_uint,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false);
	return 0;
}

/*
 * tg_conf_updated: cgroup의 bps/iops 설정이 변경된 후 관련 상태를 갱신.
 * 호출 경로: tg_set_conf() -> tg_conf_updated();
 *           tg_set_limit() -> tg_conf_updated().
 * NVMe 연결: 설정 변경 후 has_rules 플래그를 갱신하고 slice를 재시작.
 * 제한이 갑자기 낮아지면 이미 NVMe SQ로 내보내진 IO는 새 기준에서
 * 추적하지만, 새로 들어오는 IO부터는 낮은 rate로 제한된다.
 */
static void tg_conf_updated(struct throtl_grp *tg, bool global)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	struct cgroup_subsys_state *pos_css;
	struct blkcg_gq *blkg;

	throtl_log(&tg->service_queue,
		   "limit change rbps=%llu wbps=%llu riops=%u wiops=%u",
		   tg_bps_limit(tg, READ), tg_bps_limit(tg, WRITE),
		   tg_iops_limit(tg, READ), tg_iops_limit(tg, WRITE));

	rcu_read_lock(); /* RCU read lock; blkcg_gq hierarchy와 NVMe queue 간 관계 보호 */
	/*
	 * Update has_rules[] flags for the updated tg's subtree.  A tg is
	 * considered to have rules if either the tg itself or any of its
	 * ancestors has rules.  This identifies groups without any
	 * restrictions in the whole hierarchy and allows them to bypass
	 * blk-throttle.
	 */
	blkg_for_each_descendant_pre(blkg, pos_css, /* subtree의 blkcg_gq 순회; NVMe 장치에 대한 모든 cgroup limit 재계산 */
			global ? tg->td->queue->root_blkg : tg_to_blkg(tg)) {
		struct throtl_grp *this_tg = blkg_to_tg(blkg); /* 하위 cgroup의 throtl_grp; NVMe SQ 유입 제한 상속 여부 갱신 */

		tg_update_has_rules(this_tg); /* has_rules 갱신; NVMe SQ rate limit을 적용할지 여부 결정 */
		/* ignore root/second level */
		if (!cgroup_subsys_on_dfl(io_cgrp_subsys) || !blkg->parent || /* root/second level은 통계 집계 제외; NVMe namespace 루트 cgroup 제외 */
		    !blkg->parent->parent)
			continue;
	}
	rcu_read_unlock(); /* RCU read unlock; NVMe queue/cgroup 관계 참조 종료 */

	/*
	 * We're already holding queue_lock and know @tg is valid.  Let's
	 * apply the new config directly.
	 *
	 * Restart the slices for both READ and WRITES. It might happen
	 * that a group's limit are dropped suddenly and we don't want to
	 * account recently dispatched IO with new low rate.
	 */
	throtl_start_new_slice(tg, READ, false); /* READ slice 재시작; 새 NVMe rate limit 윈도우 적용 */
	throtl_start_new_slice(tg, WRITE, false); /* WRITE slice 재시작; 새 NVMe rate limit 윈도우 적용 */

	if (tg->flags & THROTL_TG_PENDING) { /* pending 상태면 disptime 재계산 필요; NVMe doorbell 예약 갱신 */
		tg_update_disptime(tg); /* 새 제한 하에서 다음 NVMe SQ 진입 시점 재계산 */
		throtl_schedule_next_dispatch(sq->parent_sq, true); /* 부모 service_queue의 pending_timer 재예약; NVMe doorbell 지연 시점 갱신 */
	}
}

/*
 * blk_throtl_init: gendisk의 request_queue에 blk-throttle을 활성화.
 * 호출 경로: tg_set_conf()/tg_set_limit() -> blk_throtl_init();
 *           blkcg_activate_policy() -> pd_alloc_fn/pd_init_fn.
 * NVMe 연결: NVMe namespace 장치에 대해 blkcg_policy_throtl를 등록.
 * 이후 해당 queue로 들어오는 bio는 __blk_throtl_bio()에서 rate limit
 * 검사를 받게 된다.
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
 * tg_set_conf: cgroup sysfs를 통해 bps/iops 한도를 설정한다.
 * 호출 경로: kernfs write -> tg_set_conf_u64()/tg_set_conf_uint() ->
 *           tg_set_conf().
 * NVMe 연결: 사용자가 echo 값 > cgroup.throttle.*_bps_device 등으로
 * 설정하면, 이 함수가 NVMe 장치에 대한 소프트웨어 rate limit을 갱신.
 * (추정) NVMe의 Queue Depth나 컨트롤러 레벨 제한은 여기서 직접
 * 변경되지 않고, bio 흐름 제어만 수행한다.
 */
static ssize_t tg_set_conf(struct kernfs_open_file *of,
			   char *buf, size_t nbytes, loff_t off, bool is_u64)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkg_conf_ctx ctx;
	struct throtl_grp *tg;
	int ret;
	u64 v;

	blkg_conf_init(&ctx, buf);

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

	if (is_u64)
		*(u64 *)((void *)tg + of_cft(of)->private) = v; /* u64 필드(bps)에 새 limit 기록; NVMe 대역폭 제한 갱신 */
	else
		*(unsigned int *)((void *)tg + of_cft(of)->private) = v; /* uint 필드(iops)에 새 limit 기록; NVMe 초당 명령 제한 갱신 */

	tg_conf_updated(tg, false); /* 설정 변경 후 limit 적용 및 slice 재시작; NVMe SQ 유입 rate 갱신 */
	ret = 0;
out_finish:
	blkg_conf_exit(&ctx);
	return ret ?: nbytes;
}

static ssize_t tg_set_conf_u64(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, true);
}

static ssize_t tg_set_conf_uint(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, false);
}

static int tg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  blkg_prfill_rwstat, &blkcg_policy_throtl,
			  seq_cft(sf)->private, true);
	return 0;
}

static u64 tg_prfill_rwstat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample sum;

	blkg_rwstat_recursive_sum(pd_to_blkg(pd), &blkcg_policy_throtl, off,
				  &sum);
	return __blkg_prfill_rwstat(sf, pd, &sum);
}

static int tg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  tg_prfill_rwstat_recursive, &blkcg_policy_throtl,
			  seq_cft(sf)->private, true);
	return 0;
}

static struct cftype throtl_legacy_files[] = {
	{
		.name = "throttle.read_bps_device",
		.private = offsetof(struct throtl_grp, bps[READ]),
		.seq_show = tg_print_conf_u64,
		.write = tg_set_conf_u64,
	},
	{
		.name = "throttle.write_bps_device",
		.private = offsetof(struct throtl_grp, bps[WRITE]),
		.seq_show = tg_print_conf_u64,
		.write = tg_set_conf_u64,
	},
	{
		.name = "throttle.read_iops_device",
		.private = offsetof(struct throtl_grp, iops[READ]),
		.seq_show = tg_print_conf_uint,
		.write = tg_set_conf_uint,
	},
	{
		.name = "throttle.write_iops_device",
		.private = offsetof(struct throtl_grp, iops[WRITE]),
		.seq_show = tg_print_conf_uint,
		.write = tg_set_conf_uint,
	},
	{
		.name = "throttle.io_service_bytes",
		.private = offsetof(struct throtl_grp, stat_bytes),
		.seq_show = tg_print_rwstat,
	},
	{
		.name = "throttle.io_service_bytes_recursive",
		.private = offsetof(struct throtl_grp, stat_bytes),
		.seq_show = tg_print_rwstat_recursive,
	},
	{
		.name = "throttle.io_serviced",
		.private = offsetof(struct throtl_grp, stat_ios),
		.seq_show = tg_print_rwstat,
	},
	{
		.name = "throttle.io_serviced_recursive",
		.private = offsetof(struct throtl_grp, stat_ios),
		.seq_show = tg_print_rwstat_recursive,
	},
	{ }	/* terminate */
};

static u64 tg_prfill_limit(struct seq_file *sf, struct blkg_policy_data *pd,
			 int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	const char *dname = blkg_dev_name(pd->blkg);
	u64 bps_dft;
	unsigned int iops_dft;

	if (!dname)
		return 0;

	bps_dft = U64_MAX;
	iops_dft = UINT_MAX;

	if (tg->bps[READ] == bps_dft &&
	    tg->bps[WRITE] == bps_dft &&
	    tg->iops[READ] == iops_dft &&
	    tg->iops[WRITE] == iops_dft)
		return 0;

	seq_printf(sf, "%s", dname);
	if (tg->bps[READ] == U64_MAX)
		seq_printf(sf, " rbps=max");
	else
		seq_printf(sf, " rbps=%llu", tg->bps[READ]);

	if (tg->bps[WRITE] == U64_MAX)
		seq_printf(sf, " wbps=max");
	else
		seq_printf(sf, " wbps=%llu", tg->bps[WRITE]);

	if (tg->iops[READ] == UINT_MAX)
		seq_printf(sf, " riops=max");
	else
		seq_printf(sf, " riops=%u", tg->iops[READ]);

	if (tg->iops[WRITE] == UINT_MAX)
		seq_printf(sf, " wiops=max");
	else
		seq_printf(sf, " wiops=%u", tg->iops[WRITE]);

	seq_printf(sf, "\n");
	return 0;
}

static int tg_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_limit,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false);
	return 0;
}

/*
 * tg_set_limit: v2 cgroup 인터페이스에서 rbps/wbps/riops/wiops를 한꺼번에
 * 설정.
 * NVMe 연결: NVMe 장치에 대한 4가지 limit을 atomically 갱신. (추정) 이
 * 인터페이스는 컨테이너 단위로 NVMe QoS를 조율할 때 주로 사용된다.
 */
static ssize_t tg_set_limit(struct kernfs_open_file *of,
			  char *buf, size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkg_conf_ctx ctx;
	struct throtl_grp *tg;
	u64 v[4];
	int ret;

	blkg_conf_init(&ctx, buf);

	ret = blkg_conf_open_bdev(&ctx); /* v2 인터페이스에서 NVMe namespace bdev 열기 */
	if (ret) /* bdev 열기 실패 시 NVMe QoS 설정 abort */
		goto out_finish;

	if (!blk_throtl_activated(ctx.bdev->bd_queue)) { /* throttle 미활성화 시 NVMe namespace에 blk-throttle 초기화 */
		ret = blk_throtl_init(ctx.bdev->bd_disk); /* NVMe namespace throttle 계층 생성 */
		if (ret) /* 초기화 실패 시 NVMe QoS 설정 abort */
			goto out_finish;
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, &ctx);
	if (ret)
		goto out_finish;

	tg = blkg_to_tg(ctx.blkg); /* 대상 cgroup의 throtl_grp; NVMe 장치별 rate limit 객체 */
	tg_update_carryover(tg); /* 설정 변경 전 carryover 계산; NVMe SQ 진입 지연 보정 */

	v[0] = tg->bps[READ]; /* 기존 READ bps 제한 백업; 새 값과 비교/복원용 */
	v[1] = tg->bps[WRITE]; /* 기존 WRITE bps 제한 백업 */
	v[2] = tg->iops[READ]; /* 기존 READ iops 제한 백업 */
	v[3] = tg->iops[WRITE]; /* 기존 WRITE iops 제한 백업 */

	while (true) { /* "rbps=... wbps=... riops=... wiops=..." 토큰 순회; NVMe 4가지 limit 파싱 */
		char tok[27];	/* wiops=18446744073709551616 */
		char *p;
		u64 val = U64_MAX;
		int len;

		if (sscanf(ctx.body, "%26s%n", tok, &len) != 1) /* 다음 토큰 파싱; NVMe QoS 설정 항목 하나 */
			break; /* 더 이상 토큰이 없으면 파싱 종료 */
		if (tok[0] == '\0') /* 빈 토큰이면 종료 */
			break;
		ctx.body += len; /* 파싱 위치 진행; 다음 NVMe QoS 토큰으로 */

		ret = -EINVAL;
		p = tok;
		strsep(&p, "="); /* "key=value" 분리; NVMe limit 항목 이름과 값 분리 */
		if (!p || (sscanf(p, "%llu", &val) != 1 && strcmp(p, "max"))) /* 값 파싱 실패 또는 "max"가 아니면 설정 거부 (abort) */
			goto out_finish;

		ret = -ERANGE;
		if (!val) /* 값이 0이면 ERANGE; NVMe limit은 0 불허(무제한은 max) */
			goto out_finish;

		ret = -EINVAL;
		if (!strcmp(tok, "rbps")) /* READ bps limit 설정; NVMe READ 대역폭 QoS */
			v[0] = val;
		else if (!strcmp(tok, "wbps")) /* WRITE bps limit 설정; NVMe WRITE 대역폭 QoS */
			v[1] = val;
		else if (!strcmp(tok, "riops")) /* READ iops limit 설정; NVMe READ 초당 명령 QoS */
			v[2] = min_t(u64, val, UINT_MAX);
		else if (!strcmp(tok, "wiops")) /* WRITE iops limit 설정; NVMe WRITE 초당 명령 QoS */
			v[3] = min_t(u64, val, UINT_MAX);
		else
			goto out_finish;
	}

	tg->bps[READ] = v[0]; /* READ bps 최종 적용; NVMe SQ READ 대역폭 제한 */
	tg->bps[WRITE] = v[1]; /* WRITE bps 최종 적용; NVMe SQ WRITE 대역폭 제한 */
	tg->iops[READ] = v[2]; /* READ iops 최종 적용; NVMe SQ READ 초당 명령 제한 */
	tg->iops[WRITE] = v[3]; /* WRITE iops 최종 적용; NVMe SQ WRITE 초당 명령 제한 */

	tg_conf_updated(tg, false); /* 4가지 limit 변경 후 slice 재시작; NVMe SQ 유입 rate 즉시 재조정 */
	ret = 0;
out_finish:
	blkg_conf_exit(&ctx);
	return ret ?: nbytes;
}

static struct cftype throtl_files[] = {
	{
		.name = "max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = tg_print_limit,
		.write = tg_set_limit,
	},
	{ }	/* terminate */
};

static void throtl_shutdown_wq(struct request_queue *q)
{
	struct throtl_data *td = q->td;

	cancel_work_sync(&td->dispatch_work); /* kthrotld workqueue 정리; NVMe SQ로 bio를 보내는 work item 취소 */
}

/*
 * tg_flush_bios: cgroup이 offline 되거나 장치가 해제될 때 throttle 큐에
 * 남아 있던 bio를 강제로 디스패치.
 * NVMe 연결: THROTL_TG_CANCELING 플래그를 설정해 더 이상 새로운 제한
 * 검사를 하지 않고, pending_tree에 등록된 경우 disptime을 즉시로 만들어
 * NVMe SQ로 빠르게 흘려보낸다. del_gendisk() 이후 inflight IO를
 * 방지하기 위한 안전장치다.
 */
static void tg_flush_bios(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue;

	if (tg->flags & THROTL_TG_CANCELING) /* 이미 취소 중이면 중복 flush 방지; NVMe namespace 제거 중 */
		return;
	/*
	 * Set the flag to make sure throtl_pending_timer_fn() won't
	 * stop until all throttled bios are dispatched.
	 */
	tg->flags |= THROTL_TG_CANCELING; /* THROTL_TG_CANCELING 설정; NVMe controller reset/quiesce 시 제한 검사 우회 */

	/*
	 * Do not dispatch cgroup without THROTL_TG_PENDING or cgroup
	 * will be inserted to service queue without THROTL_TG_PENDING
	 * set in tg_update_disptime below. Then IO dispatched from
	 * child in tg_dispatch_one_bio will trigger double insertion
	 * and corrupt the tree.
	 */
	if (!(tg->flags & THROTL_TG_PENDING)) /* pending_tree에 없으면 disptime 갱신/타이머 예약 불필요 */
		return;

	/*
	 * Update disptime after setting the above flag to make sure
	 * throtl_select_dispatch() won't exit without dispatching.
	 */
	tg_update_disptime(tg); /* disptime을 즉시로 만들어 NVMe SQ로 남은 bio를 강제 디스패치 */

	throtl_schedule_pending_timer(sq, jiffies + 1); /* 1 jiffy 후 pending_timer 만료; NVMe SQ로 남은 bio를 빠르게 흘림 */
}

static void throtl_pd_offline(struct blkg_policy_data *pd)
{
	tg_flush_bios(pd_to_tg(pd));
}

struct blkcg_policy blkcg_policy_throtl = {
	.dfl_cftypes		= throtl_files,
	.legacy_cftypes		= throtl_legacy_files,

	.pd_alloc_fn		= throtl_pd_alloc,
	.pd_init_fn		= throtl_pd_init,
	.pd_online_fn		= throtl_pd_online,
	.pd_offline_fn		= throtl_pd_offline,
	.pd_free_fn		= throtl_pd_free,
};

/*
 * blk_throtl_cancel_bios: 디스크 해제 시 모든 하위 cgroup의 bio를 flush.
 * 호출 경로: del_gendisk() -> blk_throtl_cancel_bios() -> tg_flush_bios().
 * NVMe 연결: NVMe namespace가 제거되기 전, 아직 throtl 큐에 남아
 * 있던 bio들을 모두 NVMe 쪽으로 내보내거나 상위로 전파하여 메모리
 * 누수/inflight IO 없이 정리한다.
 */
void blk_throtl_cancel_bios(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct cgroup_subsys_state *pos_css;
	struct blkcg_gq *blkg;

	if (!blk_throtl_activated(q)) /* throttle이 비활성화면 flush 할 것 없음; NVMe SQ 진입 제어 계층 없음 */
		return;

	spin_lock_irq(&q->queue_lock); /* request_queue_lock 획득; NVMe SQ/CQ와 throtl 구조 동시 보호 */
	/*
	 * queue_lock is held, rcu lock is not needed here technically.
	 * However, rcu lock is still held to emphasize that following
	 * path need RCU protection and to prevent warning from lockdep.
	 */
	rcu_read_lock(); /* RCU read lock; blkcg_gq hierarchy와 NVMe queue 관계 보호 */
	blkg_for_each_descendant_post(blkg, pos_css, q->root_blkg) { /* 하위 cgroup 순회; NVMe namespace에 연결된 모든 cgroup의 throttle 큐 처리 */
		/*
		 * disk_release will call pd_offline_fn to cancel bios.
		 * However, disk_release can't be called if someone get
		 * the refcount of device and issued bios which are
		 * inflight after del_gendisk.
		 * Cancel bios here to ensure no bios are inflight after
		 * del_gendisk.
		 */
		tg_flush_bios(blkg_to_tg(blkg)); /* 각 cgroup의 throttle 큐 flush; NVMe SQ로 남은 bio 강제 전달 또는 폐기 준비 */
	}
	rcu_read_unlock(); /* RCU read unlock; NVMe queue/cgroup 관계 참조 종료 */
	spin_unlock_irq(&q->queue_lock); /* request_queue_lock 해제; NVMe SQ/CQ 처리 재개 */
}

/*
 * tg_within_limit: bio가 현재 throtl_grp의 bps/iops 제한 안에 있는지
 * 판단.
 * NVMe 연결: bps 큐가 비었고 bio가 bps 제한을 통과하면 iops 큐로 직접
 * 넘어간다. 분할 bio(BIO_BPS_THROTTLED)는 iops 검사만 수행. 제한을
 * 초과하면 false를 반환해 __blk_throtl_bio()에서 큐잉한다.
 */
static bool tg_within_limit(struct throtl_grp *tg, struct bio *bio, bool rw)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* 현재 cgroup의 service_queue; NVMe SQ 진입 전 bio 대기 상태 */

	/*
	 * For a split bio, we need to specifically distinguish whether the
	 * iops queue is empty.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED)) /* 분할 bio는 bps가 이미 처리됨; NVMe에서는 iops 제한만 추가 검사 */
		return sq->nr_queued_iops[rw] == 0 && /* iops 큐가 비어 있고 iops 제한 통과 시 즉시 NVMe SQ 진입 가능 */
				tg_dispatch_iops_time(tg, bio) == 0;

	/*
	 * Throtl is FIFO - if bios are already queued, should queue.
	 * If the bps queue is empty and @bio is within the bps limit, charge
	 * bps here for direct placement into the iops queue.
	 */
	if (sq_queued(&tg->service_queue, rw)) { /* 동일 방향에 이미 대기 bio가 있으면 FIFO 순서로 NVMe SQ 진입 지연 */
		if (sq->nr_queued_bps[rw] == 0 && /* bps 큐가 비어 있고 bio가 bps 제한 내이면 bps 사용량 먼저 기록 */
		    tg_dispatch_bps_time(tg, bio) == 0)
			throtl_charge_bps_bio(tg, bio); /* bps 사용량을 선차감; 이후 iops 큐에서 NVMe 초당 명령 제한만 검사 */

		return false; /* FIFO: 이미 대기 중인 bio가 있으면 현재 bio도 NVMe SQ 진입 지연 */
	}

	return tg_dispatch_time(tg, bio) == 0; /* bps/iops 모두 통과하면 0, 아니면 NVMe doorbell 지연 시간 */
}

/*
 * __blk_throtl_bio: bio가 blk-throttle 계층을 통과할 수 있는지 검사.
 * 호출 경로: submit_bio() -> blk_mq_submit_bio() -> blk_should_throtl() ->
 *           blk_throtl_bio() -> __blk_throtl_bio().
 *           (CONFIG_BLK_DEV_THROTTLING=y 일 때)
 * NVMe 연결: bio가 NVMe SQ에 도달하기 전 마지막 cgroup 기반 rate limit
 * 검사. 제한을 초과하면 throtl 큐에 들어가 pending_timer에 의해
 * disptime 이후 다시 디스패치. 모든 cgroup階層을 bottom-up으로 통과해야
 * 하며, 최상위에서 throttled=false로 빠져나가야만
 * blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)로
 * 전달된다.
 */
bool __blk_throtl_bio(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev); /* bio가 속한 NVMe namespace의 request_queue 획득 */
	struct blkcg_gq *blkg = bio->bi_blkg; /* bio의 blkcg_gq; NVMe 장치별 cgroup queue 상태 */
	struct throtl_qnode *qn = NULL; /* 부모로 전달할 qnode 포인터; NVMe SQ 진입 전 cgroup 계층 이동용 */
	struct throtl_grp *tg = blkg_to_tg(blkg); /* bio가 속한 cgroup의 throtl_grp; NVMe rate limit 상태 */
	struct throtl_service_queue *sq; /* 현재 검사 중인 service_queue; NVMe SQ 진입 전 관문 */
	bool rw = bio_data_dir(bio); /* bio의 READ/WRITE 방향; NVMe SQ/CQ 방향 */
	bool throttled = false; /* bio가 throttle되어 NVMe SQ 진입이 지연되었는지 결과 */
	struct throtl_data *td = tg->td; /* throtl_data; NVMe namespace 단위 dispatch_work/pending_timer */

	rcu_read_lock(); /* RCU read lock; bio->bi_blkg 및 cgroup hierarchy가 해제되지 않도록 보호 */
	spin_lock_irq(&q->queue_lock); /* request_queue_lock 획득; NVMe SQ/CQ 구조와 throtl 상태 동시 보호 */
	sq = &tg->service_queue; /* 현재 cgroup의 service_queue; NVMe SQ 진입 전 대기열 */

	while (true) { /* cgroup hierarchy를 bottom-up으로 순회; 모든 NVMe rate limit 통과 필요 */
		if (tg_within_limit(tg, bio, rw)) { /* 현재 cgroup의 bps/iops 제한 안에 있으면 NVMe SQ 진입 가능 */
			/* within limits, let's charge and dispatch directly */
			throtl_charge_iops_bio(tg, bio); /* iops 사용량 기록; NVMe SQ에 들어갈 하나의 명령(CID)으로 카운트 */

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
			throtl_trim_slice(tg, rw); /* slice 정리; NVMe rate limit 시간 윈도우 보정 */
		} else if (bio_issue_as_root_blkg(bio)) { /* root 권한 IO는 우선 처리; NVMe 컨트롤러 우선순위 역전 방지용 예외 */
			/*
			 * IOs which may cause priority inversions are
			 * dispatched directly, even if they're over limit.
			 *
			 * Charge and dispatch directly, and our throttle
			 * control algorithm is adaptive, and extra IO bytes
			 * will be throttled for paying the debt
			 */
			throtl_charge_bps_bio(tg, bio); /* root IO에도 bps 사용량 기록; NVMe 대역폭 제한 추적 */
			throtl_charge_iops_bio(tg, bio); /* root IO에도 iops 사용량 기록; NVMe 초당 명령 추적 (나중에 상환) */
		} else {
			/* if above limits, break to queue */
			break; /* 제한 초과: NVMe SQ 진입 중단하고 throtl 큐에 bio 적재 */
		}

		/*
		 * @bio passed through this layer without being throttled.
		 * Climb up the ladder.  If we're already at the top, it
		 * can be executed directly.
		 */
		qn = &tg->qnode_on_parent[rw]; /* 부모 service_queue의 qnode 선택; NVMe rate limit 관문을 한 단계 올라감 */
		sq = sq->parent_sq; /* parent_sq로 이동; NVMe SQ 진입 전 상위 cgroup 제한 검사 */
		tg = sq_to_tg(sq); /* 상위 service_queue의 throtl_grp 획득; 최상위면 NULL */
		if (!tg) { /* 최상위 service_queue에 도달하면 모든 NVMe rate limit 통과 */
			bio_set_flag(bio, BIO_BPS_THROTTLED); /* 모든 throtl_grp 통과, NVMe로 진입 가능 */
			goto out_unlock; /* BIO_BPS_THROTTLED 설정; blk-mq -> NVMe 드라이버로 직접 전달 가능 */
		}
	}

	/* out-of-limit, queue to @tg */
	throtl_log(sq, "[%c] bio. bdisp=%llu sz=%u bps=%llu iodisp=%u iops=%u queued=%d/%d",
		   rw == READ ? 'R' : 'W',
		   tg->bytes_disp[rw], bio->bi_iter.bi_size,
		   tg_bps_limit(tg, rw),
		   tg->io_disp[rw], tg_iops_limit(tg, rw),
		   sq_queued(sq, READ), sq_queued(sq, WRITE));

	td->nr_queued[rw]++; /* NVMe SQ 진입이 지연된 bio 수 증가 */
	throtl_add_bio_tg(bio, qn, tg); /* bio를 throtl 큐에 추가; NVMe SQ 유입을 지연시키는 소프트웨어 관문 */
	throttled = true; /* bio가 throttled 됨; NVMe SQ로 즉시 진입하지 않음 */

	/*
	 * Update @tg's dispatch time and force schedule dispatch if @tg
	 * was empty before @bio, or the iops queue is empty and @bio will
	 * add to.  The forced scheduling isn't likely to cause undue
	 * delay as @bio is likely to be dispatched directly if its @tg's
	 * disptime is not in the future.
	 */
	if (tg->flags & THROTL_TG_WAS_EMPTY || /* 큐가 비어 있었거나 iops 큐가 비어 있으면 dispatch 시점 갱신 필요 */
	    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) {
		tg_update_disptime(tg); /* 새 bio에 대해 다음 NVMe SQ 진입 시각(disptime) 재계산 */
		throtl_schedule_next_dispatch(tg->service_queue.parent_sq, true); /* 부모 service_queue의 pending_timer 강제 예약; NVMe doorbell 지연/갱신 */
	}

out_unlock:
	spin_unlock_irq(&q->queue_lock); /* request_queue_lock 해제; NVMe 드라이버가 CQ/ISR 처리 진행 가능 */

	rcu_read_unlock(); /* RCU read unlock; bio/cgroup 구조 참조 종료 */
	return throttled; /* throttled 여부 반환; true면 NVMe SQ 진입 지연, false면 즉시 진입 */
}

/*
 * blk_throtl_exit: gendisk에 연결된 blk-throttle 상태를 해제.
 * 호출 경로: del_gendisk() 이후.
 * NVMe 연결: NVMe namespace 제거 시 throtl_data와 pending_timer,
 * dispatch_work를 정리. 이후 해당 request_queue로 들어오는 bio는 더
 * 이상 blk-throttle을 거치지 않는다.
 */
void blk_throtl_exit(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;

	/*
	 * blkg_destroy_all() already deactivate throtl policy, just check and
	 * free throtl data.
	 */
	if (!q->td) /* throtl_data가 없으면 정리할 것 없음; NVMe namespace에 throttle 계층 없음 */
		return;

	timer_delete_sync(&q->td->service_queue.pending_timer); /* pending_timer 삭제; NVMe doorbell 예약 중지 */
	throtl_shutdown_wq(q); /* kthrotld dispatch_work 취소; NVMe SQ로의 bio 전달 중단 */
	kfree(q->td); /* throtl_data 메모리 해제; NVMe namespace throttle 상태 제거 */
}

/*
 * throtl_init: blk-throttle 모듈 초기화.
 * 호출 경로: module_init -> throtl_init -> blkcg_policy_register().
 * NVMe 연결: kthrotld workqueue를 생성하고 blkcg_policy_throtl를 등록.
 * 이후 NVMe 장치라도 blkcg_activate_policy()를 통해 활성화되지 않으면
 * bio는 throttling 없이 지나간다.
 */
static int __init throtl_init(void)
{
	kthrotld_workqueue = alloc_workqueue("kthrotld", WQ_MEM_RECLAIM, 0); /* kthrotld workqueue 생성; NVMe SQ로 bio를 전달하는 데몬 */
	if (!kthrotld_workqueue) /* workqueue 생성 실패는 치명적; NVMe throttle 계층 없이 부팅 진행 불가 */
		panic("Failed to create kthrotld\n");

	return blkcg_policy_register(&blkcg_policy_throtl); /* blk-throttle 정책 등록; NVMe 장치 포함 모든 block 장치에서 사용 가능 */
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
