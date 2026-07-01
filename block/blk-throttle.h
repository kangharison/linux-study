/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_THROTTLE_H
#define BLK_THROTTLE_H

/*
 * NVMe 관점 파일 개요:
 * block/blk-throttle.h는 cgroup 기반 IO throttling의 핵심 자료구조와
 * 인라인 판단 함수를 정의한다. 응용 프로그램의 submit_bio() ->
 * blk_mq_submit_bio() -> blk_should_throtl() -> blk_throtl_bio() ->
 * __blk_throtl_bio() 경로에서, bio가 blk-mq request 할당과
 * nvme_queue_rq() -> nvme_submit_cmd(doorbell) 이전에 bps/IOPS 기준으로
 * 제한된다. blk-throttle.c 구현의 헤더이며, blk-cgroup-rwstat.h의
 * blkg_rwstat 등을 바탕으로 동작한다.
 */

#include "blk-cgroup-rwstat.h" /* NVMe namespace별 READ/WRITE 바이트/IO 통계 기반. */

/*
 * To implement hierarchical throttling, throtl_grps form a tree and bios
 * are dispatched upwards level by level until they reach the top and get
 * issued.  When dispatching bios from the children and local group at each
 * level, if the bios are dispatched into a single bio_list, there's a risk
 * of a local or child group which can queue many bios at once filling up
 * the list starving others.
 *
 * To avoid such starvation, dispatched bios are queued separately
 * according to where they came from.  When they are again dispatched to
 * the parent, they're popped in round-robin order so that no single source
 * hogs the dispatch window.
 *
 * throtl_qnode is used to keep the queued bios separated by their sources.
 * Bios are queued to throtl_qnode which in turn is queued to
 * throtl_service_queue and then dispatched in round-robin order.
 *
 * It's also used to track the reference counts on blkg's.  A qnode always
 * belongs to a throtl_grp and gets queued on itself or the parent, so
 * incrementing the reference of the associated throtl_grp when a qnode is
 * queued and decrementing when dequeued is enough to keep the whole blkg
 * tree pinned while bios are in flight.
 */

/*
 * NVMe 동작 연결 (struct throtl_qnode):
 *  - node: service_queue->queued[]에 연결되어 parent/self 간 디스패치 순서를
 *    관리한다. NVMe SQ에 들어가기 전에 여러 cgroup의 bio 순서를 round-robin으로
 *    보장한다.
 *  - bios_bps: bps 제한에 의해 대기 중인 bio들이다. NVMe 입장에서는 아직
 *    blk_mq_get_request()를 거쳐 struct request로 변환되지 않은, SQ doorbell
 *    이전 단계의 요청들이다 (추정).
 *  - bios_iops: iops 제한에 의해 대기 중인 bio들이다. NVMe SQ에 들어갈
 *    명령 개수(CID 소비량)를 조절하기 위해 사용된다.
 *  - tg: 이 qnode가 속한 throtl_grp(cgroup)을 가리킨다.
 */
struct throtl_qnode {
	struct list_head	node;		/* service_queue->queued[] */
	struct bio_list		bios_bps;	/* queued bios for bps limit */
	struct bio_list		bios_iops;	/* queued bios for iops limit */
	struct throtl_grp	*tg;		/* tg this qnode belongs to */
};

/*
 * NVMe 동작 연결 (struct throtl_service_queue):
 *  - parent_sq: 계층적 cgroup 트리에서 부모 service_queue를 가리킨다. bio는
 *    leaf에서 root로 거슬러 올라가 최종적으로 request_queue에 도달한 뒤
 *    nvme_queue_rq()가 호출된다.
 *  - queued[2]: READ/WRITE별 throtl_qnode 연결 리스트. NVMe SQ 유입 순서를
 *    round-robin으로 유지하여 특정 cgroup이 SQ를 독점하는 기아를 방지한다.
 *  - nr_queued_bps[2]/nr_queued_iops[2]: NVMe SQ로 아직 본류에 태우지 못한
 *    bio/바이트 수를 카운트한다.
 *  - pending_tree: 활성화된 throtl_grp을 disptime 기준으로 정렬한 RB 트리.
 *    NVMe SQ에 진입할 다음 후보 그룹을 시간 순서로 선택할 때 사용된다.
 *  - first_pending_disptime/pending_timer: 가장 먼저 throttle이 풀릴 그룹의
 *    시점을 관리하며, 타이머가 만료되면 blk_throtl_dispatch_work_fn()을
 *    통해 bio를 다시 blk_mq_submit_bio() 경로로 복귀시킨다.
 */
struct throtl_service_queue {
	struct throtl_service_queue *parent_sq;	/* the parent service_queue */

	/*
	 * Bios queued directly to this service_queue or dispatched from
	 * children throtl_grp's.
	 */
	struct list_head	queued[2];	/* throtl_qnode [READ/WRITE] */
	unsigned int		nr_queued_bps[2];	/* number of queued bps bios */
	unsigned int		nr_queued_iops[2];	/* number of queued iops bios */

	/*
	 * RB tree of active children throtl_grp's, which are sorted by
	 * their ->disptime.
	 */
	struct rb_root_cached	pending_tree;	/* RB tree of active tgs */
	unsigned int		nr_pending;	/* # queued in the tree */
	unsigned long		first_pending_disptime;	/* disptime of the first tg */
	struct timer_list	pending_timer;	/* fires on first_pending_disptime */
};

enum tg_state_flags {
	THROTL_TG_PENDING		= 1 << 0,	/* on parent's pending tree */
	/* NVMe: 이 그룹이 곧 SQ로 bio를 낼 수 있도록 pending_tree에 예약됨. */
	THROTL_TG_WAS_EMPTY		= 1 << 1,	/* bio_lists[] became non-empty */
	/* NVMe: bps/iops 큐에 bio가 새로 추가되어 디스패치 후보가 된 상태. */
	/*
	 * The sq's iops queue is empty, and a bio is about to be enqueued
	 * to the first qnode's bios_iops list.
	 */
	THROTL_TG_IOPS_WAS_EMPTY	= 1 << 2,
	/* NVMe: IOPS 큐가 비어있다가 채워지면 새 슬라이스 기준으로 CID 유입률 계산 시작. */
	THROTL_TG_CANCELING		= 1 << 3,	/* starts to cancel bio */
	/* NVMe: controller reset/namespace teardown 시 bio를 취소하고 SQ를 drain 중. */
};

/*
 * NVMe 동작 연결 (struct throtl_grp):
 *  - pd: blk-cgroup policy data. blkg_to_tg() 등에서 throtl_grp으로 변환할
 *    때 사용되며, blk-cgroup 계층과 NVMe queue를 연결한다.
 *  - rb_node: service_queue->pending_tree에 연결될 때 사용.
 *  - td: 이 그룹이 속한 throtl_data(하나의 request_queue/namespace 단위).
 *    NVMe에서는 보통 하나의 nvme_ns queue에 대응된다 (추정).
 *  - service_queue: 이 cgroup의 자체 서비스 큐. 자식 그룹이나 직접 도착한
 *    bio를 NVMe SQ 이전에 임시 대기시킨다.
 *  - qnode_on_self[2]/qnode_on_parent[2]: 자체/부모 service_queue에 bio를
 *    분리 큐잉하여 round-robin으로 디스패치할 수 있게 한다.
 *  - disptime: 이 그룹의 throttle이 풀리고 NVMe SQ로 bio를 낼 수 있는
 *    예상 시점(jiffies)이다.
 *  - flags: THROTL_TG_PENDING 등 상태. NVMe SQ 진행 가능 시점을 스케줄링
 *    하는 데 참조된다.
 *  - has_rules_bps[2]/has_rules_iops[2]: READ/WRITE 방향에 bps/IOPS 규칙이
 *    있는지 표시. 있을 때만 blk_should_throtl()에서 제한을 검사한다.
 *  - bps[2]/iops[2]: 설정된 bps/IOPS 상한값. NVMe SQ로의 바이트/명령
 *    유입률을 이 값 이하로 억제한다.
 *  - bytes_disp[2]/io_disp[2]: 현재 슬라이스에서 이미 디스패치된
 *    바이트/IO 수. 설정이 변경되면 carryover를 음수로 반영하여 새로운
 *    bps/iops 기준에서 대기 시간을 재계산한다.
 *  - last_check_time: 마지막으로 rate limit을 점검한 시간.
 *  - slice_start[2]/slice_end[2]: 현재 제한 윈도우의 시작/종료 시점.
 *  - stat_bytes/stat_ios: blk-cgroup-rwstat 기반 통계. NVMe namespace별
 *    READ/WRITE 바이트 및 IO 개수를 cgroup별로 집계한다.
 */
struct throtl_grp {
	/* must be the first member */
	struct blkg_policy_data pd;

	/* active throtl group service_queue member */
	struct rb_node rb_node;

	/* throtl_data this group belongs to */
	struct throtl_data *td;

	/* this group's service queue */
	struct throtl_service_queue service_queue;

	/*
	 * qnode_on_self is used when bios are directly queued to this
	 * throtl_grp so that local bios compete fairly with bios
	 * dispatched from children.  qnode_on_parent is used when bios are
	 * dispatched from this throtl_grp into its parent and will compete
	 * with the sibling qnode_on_parents and the parent's
	 * qnode_on_self.
	 */
	struct throtl_qnode qnode_on_self[2];
	struct throtl_qnode qnode_on_parent[2];

	/*
	 * Dispatch time in jiffies. This is the estimated time when group
	 * will unthrottle and is ready to dispatch more bio. It is used as
	 * key to sort active groups in service tree.
	 */
	unsigned long disptime;

	unsigned int flags;

	/* are there any throtl rules between this group and td? */
	bool has_rules_bps[2];
	bool has_rules_iops[2];

	/* bytes per second rate limits */
	uint64_t bps[2];

	/* IOPS limits */
	unsigned int iops[2];

	/*
	 * Number of bytes/bio's dispatched in current slice.
	 * When new configuration is submitted while some bios are still throttled,
	 * first calculate the carryover: the amount of bytes/IOs already waited
	 * under the previous configuration. Then, [bytes/io]_disp are represented
	 * as the negative of the carryover, and they will be used to calculate the
	 * wait time under the new configuration.
	 */
	int64_t bytes_disp[2];
	int io_disp[2];

	unsigned long last_check_time;

	/* When did we start a new slice */
	unsigned long slice_start[2];
	unsigned long slice_end[2];

	struct blkg_rwstat stat_bytes;
	struct blkg_rwstat stat_ios;
};

extern struct blkcg_policy blkcg_policy_throtl;

/*
 * pd_to_tg:
 *  목적: struct blkg_policy_data 포인터를 포함하는 struct throtl_grp로
 *        변환한다.
 *  호출 경로: blkg_to_tg() -> pd_to_tg() -> container_of(pd, ...).
 *  NVMe 연결: blk-cgroup이 연결된 NVMe request_queue에서 bio가 속한 cgroup의
 *            throtl_grp을 찾을 때 사용된다.
 */
static inline struct throtl_grp *pd_to_tg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct throtl_grp, pd) : NULL;
}

/*
 * blkg_to_tg:
 *  목적: struct blkcg_gq로부터 해당 cgroup의 throtl_grp을 조회한다.
 *  호출 경로: blk_should_throtl() -> blkg_to_tg(bio->bi_blkg).
 *  NVMe 연결: bio->bi_blkg를 통해 NVMe queue에 도달한 bio가 어느 cgroup
 *            그룹에 속하는지 확인하고, 그 그룹의 bps/iops 규칙을 적용한다.
 */
static inline struct throtl_grp *blkg_to_tg(struct blkcg_gq *blkg)
{
	return pd_to_tg(blkg_to_pd(blkg, &blkcg_policy_throtl));
}

/*
 * Internal throttling interface
 */
#ifndef CONFIG_BLK_DEV_THROTTLING
static inline void blk_throtl_exit(struct gendisk *disk) { }
/* NVMe: throtl 미빌드 시 namespace 해제에서 할 일 없음 -> doorbell/SQ 영향 없음. */
static inline bool blk_throtl_bio(struct bio *bio) { return false; }
/* NVMe: throtl 미빌드 시 bio가 nvme_queue_rq()로 직행함. */
static inline void blk_throtl_cancel_bios(struct gendisk *disk) { }
/* NVMe: throtl 미빌드 시 abort/queue drain 중 별도 취소 큐 없음. */
#else /* CONFIG_BLK_DEV_THROTTLING */
void blk_throtl_exit(struct gendisk *disk);

/*
 * __blk_throtl_bio:
 *  목적: bio가 throtl 계층을 통과할 수 있는지 실제로 판단하고, 필요하면
 *        service_queue에 큐잉하며 디스패치 타이머를 예약한다.
 *  호출 경로: blk_throtl_bio() -> __blk_throtl_bio().
 *  NVMe 연결: bio가 blk_mq_get_request() -> nvme_queue_rq() ->
 *            nvme_submit_cmd(doorbell)로 흘러가기 전, bps/iops 한도에 따라
 *            지연시키거나 즉시 통과시킨다. 지연된 bio는
 *            blk_throtl_dispatch_work_fn()을 통해 submit_bio_noacct_nocheck()
 *            -> blk_mq_submit_bio() 경로로 재진입한다.
 */
bool __blk_throtl_bio(struct bio *bio);
void blk_throtl_cancel_bios(struct gendisk *disk);

/*
 * blk_throtl_activated:
 *  목적: request_queue에서 blk-throttle 정책이 활성화되어 있는지 확인한다.
 *  호출 경로: blk_should_throtl() -> blk_throtl_activated(bio->bi_bdev->bd_queue).
 *  NVMe 연결: NVMe namespace의 request_queue(q->td)에 throtl이 연결되어 있고
 *            blkcg_policy_throtl이 활성화된 경우에만 제한이 적용된다.
 */
static inline bool blk_throtl_activated(struct request_queue *q)
{
	/*
	 * q->td guarantees that the blk-throttle module is already loaded,
	 * and the plid of blk-throttle is assigned.
	 * blkcg_policy_enabled() guarantees that the policy is activated
	 * in the request_queue.
	 */
	/* q->td != NULL: NVMe queue가 throtl_data를 갖춰 Sq/Cq와 연결된 상태. */
	return q->td != NULL && blkcg_policy_enabled(q, &blkcg_policy_throtl);
	/* blkcg_policy_enabled: cgroup policy이 켜져 있어야만 NVMe 유입 제한 활성. */
}

/*
 * blk_should_throtl:
 *  목적: bio가 blk-throttle에 의해 제한되어야 하는지 사전 판단한다.
 *  호출 경로: blk_mq_submit_bio() -> blk_throtl_bio() -> blk_should_throtl().
 *  NVMe 연결: bio가 NVMe SQ에 도달하기 전에 bps/iops 규칙을 검사하여,
 *            불필요한 blk_mq_get_request() 및 nvme_queue_rq() 호출을
 *            막는다. IOPS 규칙은 항상 우선적으로 판단된다.
 */
static inline bool blk_should_throtl(struct bio *bio)
{
	struct throtl_grp *tg;		/* bio가 속한 cgroup의 throtl_grp (NVMe SQ 진입 관문). */
	int rw = bio_data_dir(bio);	/* READ/WRITE -> NVMe opcode방향(NVME_CMD_READ/WRITE)과 CID 소비 분리. */

	if (!blk_throtl_activated(bio->bi_bdev->bd_queue))
		/* throtl 비활성 시 bio는 nvme_queue_rq()로 직행 -> doorbell 가능. */
		return false;

	tg = blkg_to_tg(bio->bi_blkg);	/* blk-cgroup 계층에서 이 bio의 throtl_grp 획득. */
	/* v1 cgroup 계층에서만 bio별 통계를 직접 누적한다 (v2는 blkg 기반). */
	if (!cgroup_subsys_on_dfl(io_cgrp_subsys)) {
		/* 동일 bio의 바이트 통계가 중복 집계되지 않도록 플래그를 검사한다. */
		if (!bio_flagged(bio, BIO_CGROUP_ACCT)) {
			bio_set_flag(bio, BIO_CGROUP_ACCT);	/* NVMe: bio당 한 번만 계산하여 PRP/SGL 길이 중복 방지. */
			blkg_rwstat_add(&tg->stat_bytes, bio->bi_opf,
					bio->bi_iter.bi_size);
			/* NVMe: bi_size는 향후 PRP/SGL entry 개수와 doorbell batch 크기에 영향. */
		}
		/* IO 개수를 누적하여 NVMe SQ로 유입될 명령 수 추이를 측정한다. */
		blkg_rwstat_add(&tg->stat_ios, bio->bi_opf, 1);
		/* NVMe: 명령 1개는 CID 1개와 tag 1개를 소모 -> queue depth 추이 반영. */
	}

	/* iops limit is always counted */
	/* IOPS 규칙이 있으면 반드시 throtl 판단을 수행한다. */
	if (tg->has_rules_iops[rw])
		/* NVMe: iops 상한이 설정된 방향 -> SQ CID 유입률 제어 필요. */
		return true;

	/* bps 규칙이 있고 아직 bps 제한 큐를 거치지 않은 bio만 다시 검사한다. */
	if (tg->has_rules_bps[rw] && !bio_flagged(bio, BIO_BPS_THROTTLED))
		/* NVMe: bps 제한 -> DMA/PRP/SGL로 전송될 총 바이트량 제어; 중복 큐잉 방지 플래그 확인. */
		return true;

	return false;
}

/*
 * blk_throtl_bio:
 *  목적: blk-throttle 정책이 활성화된 경우에만 __blk_throtl_bio()를 호출하여
 *        bio를 제한 경로로 본낸다.
 *  호출 경로: submit_bio() -> blk_mq_submit_bio() -> blk_throtl_bio().
 *  NVMe 연결: 제한이 필요 없으면 bio는 곧바로 blk_mq_get_request() ->
 *            nvme_queue_rq() -> nvme_submit_cmd(doorbell)로 진행되고,
 *            필요하면 throtl service_queue에서 대기한 뒤 재진입한다.
 */
static inline bool blk_throtl_bio(struct bio *bio)
{
	/*
	 * block throttling takes effect if the policy is activated
	 * in the bio's request_queue.
	 */
	/* 제한 조건을 만족하지 않으면 NVMe 경로로 통과시킨다. */
	if (!blk_should_throtl(bio))
		return false;

	/* throtl 계층에서 bio를 큐잉하거나 즉시 디스패치한다. */
	return __blk_throtl_bio(bio);
}
#endif /* CONFIG_BLK_DEV_THROTTLING */

/*
 * NVMe 관점 핵심 요약
 *
 * - blk-throttle은 submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request()
 *   -> nvme_queue_rq() -> nvme_submit_cmd(doorbell) 사이에서 bps/IOPS 기반으로
 *   NVMe SQ 유입량을 제어하는 소프트웨어 관문이다.
 * - struct throtl_grp/service_queue/qnode는 cgroup별 대기 bio를 NVMe 명령
 *   변환 이전에 관리하며, round-robin으로 여러 그룹 간 기아를 방지한다.
 * - blk_should_throtl()은 IOPS 규칙을 항상 검사하고, bps 규칙은
 *   BIO_BPS_THROTTLED 플래그를 확인하여 중복 큐잉을 피한다.
 * - blk_throtl_dispatch_work_fn() 등을 통해 제한을 통과한 bio는 다시
 *   submit_bio_noacct_nocheck() -> blk_mq_submit_bio() 경로로 재진입한다.
 * - 이 헤더는 blk-cgroup-rwstat.h의 blkg_rwstat 기반을 사용하고,
 *   blk-throttle.c에서 실제 큐잉/디스패치 로직이 구현된다.
 */

#endif
