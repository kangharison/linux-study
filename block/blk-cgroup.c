// SPDX-License-Identifier: GPL-2.0
/*
 * Common Block IO controller cgroup interface
 *
 * Based on ideas and code from CFQ, CFS and BFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2009 Vivek Goyal <vgoyal@redhat.com>
 * 	              Nauman Rafique <nauman@google.com>
 *
 * For policy-specific per-blkcg data:
 * Copyright (C) 2015 Paolo Valente <paolo.valente@unimore.it>
 *                    Arianna Avanzini <avanzini.arianna@gmail.com>
 */
/*
 * NVMe SSD 관점 파일 상단 요약
 * 본 파일은 block cgroup(blkcg)의 공통 인프라로 bio/request 의 cgroup 분류,
 * IO 통계, 지연 누적, 정책 활성화/비활성화를 담당한다. NVMe 경로에서
 * submit_bio -> bio_associate_blkg -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 의 상단에서
 * cgroup context(bi_blkg)가 결정된다. blk-mq, elevator, IO 스케줄러(BFQ 등),
 * blk-throttle.c 와 협력하며 NVMe SQ/CQ/CID/PRP/SGL 보다는 cgroup 레이블과
 * 제어 정보를 다룬다.
 */

#include <linux/ioprio.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/wait_bit.h>
#include <linux/atomic.h>
#include <linux/ctype.h>
#include <linux/resume_user_mode.h>
#include <linux/psi.h>
#include <linux/part_stat.h>
#include "blk.h"
#include "blk-cgroup.h"
#include "blk-ioprio.h"
#include "blk-throttle.h"

static void __blkcg_rstat_flush(struct blkcg *blkcg, int cpu);

/*
 * blkcg_pol_mutex protects blkcg_policy[] and policy [de]activation.
 * blkcg_pol_register_mutex nests outside of it and synchronizes entire
 * policy [un]register operations including cgroup file additions /
 * removals.  Putting cgroup file registration outside blkcg_pol_mutex
 * allows grabbing it from cgroup callbacks.
 */
static DEFINE_MUTEX(blkcg_pol_register_mutex);
	/* NVMe: policy 등록/해제와 activate/deactivate 사이의 nesting 보호; NVMe queue 정책 활성화 시 경쟁 방지 */
static DEFINE_MUTEX(blkcg_pol_mutex);
	/* NVMe: blkcg_policy[] 및 policy on/off 를 보호; nvme_queue_rq() 가 참조하는 blkg->pd[] 일관성 확보 */

struct blkcg blkcg_root;
	/* NVMe: root cgroup 은 시스템 전체 NVMe IO 의 fallback cgroup */
EXPORT_SYMBOL_GPL(blkcg_root);

struct cgroup_subsys_state * const blkcg_root_css = &blkcg_root.css;
	/* NVMe: root cgroup 의 css; bio_associate_blkg() 경로에서 root blkg 매핑의 기준점 */
EXPORT_SYMBOL_GPL(blkcg_root_css);

static struct blkcg_policy *blkcg_policy[BLKCG_MAX_POLS];
	/* NVMe: throtl/BFQ/ioprio 같은 cgroup 정책 포인터 테이블; NVMe request_queue 의 q->blkcg_pols 와 연결됨 */

static LIST_HEAD(all_blkcgs);		/* protected by blkcg_pol_mutex */
	/* NVMe: 시스템 전체 blkcg 리스트; 정책 등록 시 모든 cgroup 의 cpd 할당 대상 */

bool blkcg_debug_stats = false;
	/* NVMe: debug stats 출력 시 use_delay/delay_nsec 같은 NVMe queue 지연 상태를 노출 */

static DEFINE_RAW_SPINLOCK(blkg_stat_lock);
	/* NVMe: per-cpu lockless list flush 시 reordering 방지용 raw spinlock; NVMe CQ 완료 통계 집계 직렬화 */

#define BLKG_DESTROY_BATCH_SIZE  64
	/* NVMe: NVMe namespace 제거 시 blkg 일괄 제거 배치 크기; 락 장기 점유로 인한 softlockup 방지 */

/*
 * Lockless lists for tracking IO stats update
 *
 * New IO stats are stored in the percpu iostat_cpu within blkcg_gq (blkg).
 * There are multiple blkg's (one for each block device) attached to each
 * blkcg. The rstat code keeps track of which cpu has IO stats updated,
 * but it doesn't know which blkg has the updated stats. If there are many
 * block devices in a system, the cost of iterating all the blkg's to flush
 * out the IO stats can be high. To reduce such overhead, a set of percpu
 * lockless lists (lhead) per blkcg are used to track the set of recently
 * updated iostat_cpu's since the last flush. An iostat_cpu will be put
 * onto the lockless list on the update side [blk_cgroup_bio_start()] if
 * not there yet and then removed when being flushed [blkcg_rstat_flush()].
 * References to blkg are gotten and then put back in the process to
 * protect against blkg removal.
 *
 * Return: 0 if successful or -ENOMEM if allocation fails.
 */
/*
 * init_blkcg_llists - blkcg 의 per-cpu lockless 통계 리스트 초기화
 *
 * 호출 경로: blkcg_css_alloc() -> init_blkcg_llists()
 * NVMe 연결점: NVMe namespace(request_queue)가 많을 때 모든 blkg 를 순회하지
 *   않고, IO 가 발생한 CPU 의 lockless list(lhead)만 추적해 통계 flush 비용을
 *   줄인다. 이는 고성능 NVMe SSD 에서 cgroup 통계 오버헤드를 최소화한다.
 */

static int init_blkcg_llists(struct blkcg *blkcg)
{
	int cpu;
	/* NVMe: 가능한 모든 CPU 에 대해 lhead 초기화 반복 */

	blkcg->lhead = alloc_percpu_gfp(struct llist_head, GFP_KERNEL);
	/* NVMe: per-cpu lockless list 할당; namespace 가 많아도 전체 blkg 순회 대신 갱신된 CPU 만 추적 */
	/* NVMe: per-cpu lockless list, NVMe namespace 가 많아도 전체 blkg 순회 없이 통계 갱신 추적 */
	if (!blkcg->lhead)
		/* NVMe: 메모리 부족 시 NVMe cgroup 통계 인프라 할당 실패 */
		return -ENOMEM;

	for_each_possible_cpu(cpu)
	/* NVMe: 모든 CPU 코어에 대한 lockless 통계 헤드 초기화; NVMe 멀티 큐 완료 경로의 per-cpu 집계 준비 */
		init_llist_head(per_cpu_ptr(blkcg->lhead, cpu));
		/* NVMe: 각 CPU 별 통계 flush 대기열(lhead) 초기화 */
		/* NVMe: 각 CPU 별 통계 flush 엔트리 초기화 */
	return 0;
	/* NVMe: per-cpu lhead 초기화 완료 후 반환 */
}

/**
 * blkcg_css - find the current css
 *
 * Find the css associated with either the kthread or the current task.
 * This may return a dying css, so it is up to the caller to use tryget logic
 * to confirm it is alive and well.
 */
/*
 * blkcg_css - 현재 태스크(또는 kthread)가 속한 blkcg 의 css 반환
 *
 * 호출 경로: bio_associate_blkg() -> blkcg_css()
 *            blkcg_maybe_throttle_current() -> blkcg_css()
 * NVMe 연결점: NVMe IO 를 발행하는 태스크의 cgroup 을 식별해 이후
 *   blk_mq_submit_bio -> nvme_queue_rq 경로에서 적용할 cgroup context 를
 *   결정한다. kthread 가 bio 를 발행하는 경우 kthread_blkcg() 를 우선 확인한다.
 */

static struct cgroup_subsys_state *blkcg_css(void)
{
	struct cgroup_subsys_state *css;
	/* NVMe: kthread 혹은 current task 의 cgroup 상태 포인터 */

	css = kthread_blkcg();
	/* NVMe: kthread 가 NVMe IO 를 대신 발행할 때 kthread 의 blkcg 우선 사용 */
	if (css)
		/* NVMe: kthread blkcg 가 명시 지정되어 있으면 이를 채택 */
		return css;
	return task_css(current, io_cgrp_id);
	/* NVMe: 일반 태스크라면 io cgroup 의 css 를 반환 -> NVMe SQ/CQ batching 의 cgroup 기준 */
}

/*
 * blkg_free_workfn - workqueue 에서 blkg 메모리를 해제
 *
 * 호출 경로: blkg_destroy() -> blkg_put() -> blkg_release() -> call_rcu() ->
 *            __blkg_release() -> blkg_free() -> schedule_work() ->
 *            blkg_free_workfn()
 * NVMe 연결점: NVMe namespace 가 제거되거나 cgroup 이 off-line 될 때 해당
 *   request_queue(q)와 연결된 blkg 를 정리한다. pd_free_fn() 으로 throtl/BFQ
 *   정책 데이터도 함께 해제되어 NVMe queue 에 적용되던 cgroup 정책이 제거된다.
 */

static void blkg_free_workfn(struct work_struct *work)
{
	struct blkcg_gq *blkg = container_of(work, struct blkcg_gq,
	/* NVMe: work 구조체에서 blkg 객체 복원; NVMe request_queue 와의 연결 해제 직전 단계 */
					     free_work);
	struct request_queue *q = blkg->q;
	/* NVMe: 이 blkg 가 속한 NVMe namespace 의 request_queue */
	int i;

	/*
	 * pd_free_fn() can also be called from blkcg_deactivate_policy(),
	 * in order to make sure pd_free_fn() is called in order, the deletion
	 * of the list blkg->q_node is delayed to here from blkg_destroy(), and
	 * blkcg_mutex is used to synchronize blkg_free_workfn() and
	 * blkcg_deactivate_policy().
	 */
	mutex_lock(&q->blkcg_mutex);
	/* NVMe: blkg 해제와 policy deactivate 간 동기화; NVMe queue 의 cgroup 정책 상태 보호 */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* NVMe: throtl/BFQ/ioprio 등 활성화된 정책 데이터를 순서대로 해제 */
		if (blkg->pd[i])
		/* NVMe: 이 blkg 에 할당된 policy private data 가 있을 때만 해제 */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
			/* NVMe: throtl_data/bfq_queue 해제; 이후 nvme_queue_rq() 에서 해당 정책 상태 참조 불가 */
	if (blkg->parent)
		blkg_put(blkg->parent);
	/* NVMe: 계층 구조상 부모 blkg 참조를 해제하여 cgroup 트리 일관성 유지 */
	spin_lock_irq(&q->queue_lock);
	/* NVMe: NVMe request_queue 의 blkg_list 와 queue_lock 보호 */
	list_del_init(&blkg->q_node);
		/* NVMe: NVMe request_queue 의 blkg list 에서 제거; 이후 IO 는 root blkg 로 spill */
	spin_unlock_irq(&q->queue_lock);
	mutex_unlock(&q->blkcg_mutex);
	/* NVMe: policy 해제 완료 후 mutex 해제 */

	blk_put_queue(q);
	/* NVMe: request_queue(NVMe namespace q) 참조 해제 */
	free_percpu(blkg->iostat_cpu);
	/* NVMe: per-cpu IO 통계 버퍼 반납 */
	percpu_ref_exit(&blkg->refcnt);
	/* NVMe: blkg 참조 카운터 정리; RCU 해제 이후 최종 자원 반납 */
	kfree(blkg);
	/* NVMe: blkg 객체 반납; NVMe cgroup 분류 엔트리 소멸 */
}

/**
 * blkg_free - free a blkg
 * @blkg: blkg to free
 *
 * Free @blkg which may be partially allocated.
 */
/*
 * blkg_free - blkg 해제를 workqueue 에 예약
 *
 * 호출 경로: blkg_create() 실패 / blkg_destroy() -> blkg_free()
 * NVMe 연결점: request_queue(q)의 release 핸들러가 sleep 할 수 있으므로
 *   비동기 work 로 해제한다. NVMe 드라이버 입장에서는 blkg 가 사라지면 더 이상
 *   해당 cgroup 의 IO 흐름을 구분할 수 없게 된다.
 */

static void blkg_free(struct blkcg_gq *blkg)
{
	if (!blkg)
	/* NVMe: NULL blkg 에 대한 방어적 체크 */
		return;

	/*
	 * Both ->pd_free_fn() and request queue's release handler may
	 * sleep, so free us by scheduling one work func
	 */
	INIT_WORK(&blkg->free_work, blkg_free_workfn);
	/* NVMe: blkg 해제 work 초기화; NVMe namespace cleanup 비동기 수행 */
	schedule_work(&blkg->free_work);
	/* NVMe: workqueue 에 blkg 해제 예약; NVMe queue 완료 경로와 분리된 컨텍스트에서 실행 */
}

/*
 * __blkg_release - RCU grace period 이후 blkg 정리
 *
 * 호출 경로: blkg_put() -> blkg_release() -> call_rcu() -> __blkg_release()
 * NVMe 연결점: blkg 를 참조하던 모든 NVMe IO 경로(CPU, CQ 처리 등)가 RCU
 *   grace period 를 지난 후에만 메모리를 해제한다. 해제 전 __blkcg_rstat_flush()
 *   로 per-cpu 통계를 모두 global 로 반영한다.
 */

static void __blkg_release(struct rcu_head *rcu)
{
	struct blkcg_gq *blkg = container_of(rcu, struct blkcg_gq, rcu_head);
	/* NVMe: RCU 콜백으로부터 blkg 복원 */
	struct blkcg *blkcg = blkg->blkcg;
	/* NVMe: 이 blkg 가 속한 cgroup; rstat flush 의 대상 cgroup */
	int cpu;

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
/* NVMe: kthread 우회 제출이 활성화된 경우에만 async bio 관련 필드 초기화 */
	WARN_ON(!bio_list_empty(&blkg->async_bios));
	/* NVMe: kthread 우회 제출(async_bios)이 남아있으면 버그; NVMe IO 누락 방지 */
#endif
	/*
	 * Flush all the non-empty percpu lockless lists before releasing
	 * us, given these stat belongs to us.
	 *
	 * blkg_stat_lock is for serializing blkg stat update
	 */
	for_each_possible_cpu(cpu)
	/* NVMe: NVMe 멀티 코어 CQ 완료가 기록한 per-cpu 통계를 모두 global 로 flush */
		__blkcg_rstat_flush(blkcg, cpu);
		/* NVMe: 각 CPU 의 lockless list 를 drain 하여 NVMe IO 통계를 상위 cgroup 으로 전파 */

	/* release the blkcg and parent blkg refs this blkg has been holding */
	css_put(&blkg->blkcg->css);
	/* NVMe: blkg 생성 시 획득한 cgroup css 참조 반납 */
	blkg_free(blkg);
	/* NVMe: 실제 메모리 해제를 workqueue 에 예약 */
}

/*
 * A group is RCU protected, but having an rcu lock does not mean that one
 * can access all the fields of blkg and assume these are valid.  For
 * example, don't try to follow throtl_data and request queue links.
 *
 * Having a reference to blkg under an rcu allows accesses to only values
 * local to groups like group stats and group rate limits.
 */
/*
 * blkg_release - blkg 의 percpu_ref 가 0이 되면 RCU 해제 예약
 *
 * 호출 경로: blkg_put() -> percpu_ref_put() -> blkg_release()
 * NVMe 연결점: NVMe IO 완료 후 request 가 반납되면서 blkg 참조가 감소한다.
 *   참조 카운트가 0이 되면 blkg 구조체를 안전하게 해제하기 위해 RCU 콜백을
 *   등록한다.
 */

static void blkg_release(struct percpu_ref *ref)
{
	struct blkcg_gq *blkg = container_of(ref, struct blkcg_gq, refcnt);
	/* NVMe: percpu_ref 로부터 blkg 복원 */

	call_rcu(&blkg->rcu_head, __blkg_release);
		/* NVMe: RCU grace period 후 메모리 해제; NVMe CQ/ISR 경로의 read-side 보장 */
}

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
static struct workqueue_struct *blkcg_punt_bio_wq;

/*
 * blkg_async_bio_workfn - punted bio 들을 실제 submit_bio 로 발행
 *
 * 호출 경로: blkcg_punt_bio_submit() -> queue_work() ->
 *            blkg_async_bio_workfn()
 * NVMe 연결점: 공유 kthread 가 NVMe IO(bio)를 동기적으로 발행하면 우선순위
 *   역전(priority inversion)이 발생할 수 있다. workqueue 로 비동기 발행하여
 *   submit_bio -> blk_mq_submit_bio -> nvme_queue_rq 로 전달되도록 한다.
 */

static void blkg_async_bio_workfn(struct work_struct *work)
{
	struct blkcg_gq *blkg = container_of(work, struct blkcg_gq,
	/* NVMe: work 로부터 blkg 복원 */
					     async_bio_work);
	struct bio_list bios = BIO_EMPTY_LIST;
	/* NVMe: 비동기 제출 대기 중인 bio 들의 임시 리스트 */
	struct bio *bio;
	struct blk_plug plug;
	/* NVMe: plug/batch 시작점; NVMe multi-queue parallelism 를 위한 제출 배치링 */
	bool need_plug = false;
	/* NVMe: bio 가 2개 이상일 때만 plug 를 시작하여 doorbell batching 효과 극대화 */

	/* as long as there are pending bios, @blkg can't go away */
	spin_lock(&blkg->async_bio_lock);
	/* NVMe: async_bios 리스트와 work 경쟁 보호 */
	bio_list_merge_init(&bios, &blkg->async_bios);
	/* NVMe: lock 보호 하에 대기 bio 들을 로컬 리스트로 이동 */
	spin_unlock(&blkg->async_bio_lock);
	/* NVMe: 리스트 이동 완료 후 lock 해제; 이후 submit_bio 는 queue_lock 등 다른 lock 과 교차 가능 */

	/* start plug only when bio_list contains at least 2 bios */
	if (bios.head && bios.head->bi_next) {
		/* NVMe: bio 가 2개 이상이면 plug 시작 -> NVMe SQ batch submit 및 doorbell 최소화 */
		need_plug = true;
		blk_start_plug(&plug);
		/* NVMe: plug 구조체 초기화; 이 구간의 submit_bio 가 NVMe multi-queue scheduler plug list 로 모임 */
	}
	while ((bio = bio_list_pop(&bios)))
	/* NVMe: 대기 bio 리스트를 순회하며 submit_bio; NVMe SQ/CQ CID 태그 할당의 시작점 */
		submit_bio(bio);
		/* NVMe: bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
	if (need_plug)
		blk_finish_plug(&plug);
		/* NVMe: plug 종료; NVMe hctx dispatch 로의 일괄 제출 유도 */
}

/*
 * When a shared kthread issues a bio for a cgroup, doing so synchronously can
 * lead to priority inversions as the kthread can be trapped waiting for that
 * cgroup.  Use this helper instead of submit_bio to punt the actual issuing to
 * a dedicated per-blkcg work item to avoid such priority inversions.
 */
/*
 * blkcg_punt_bio_submit - kthread 발행 bio 를 workqueue 로 우회
 *
 * 호출 경로: block layer 의 shared kthread submit 경로 ->
 *            blkcg_punt_bio_submit()
 * NVMe 연결점: root cgroup 에는 bounce 하지 않고, 하위 cgroup 의 bio 는
 *   async_bios 리스트에 연결한 뒤 workqueue 에서 순차적으로 submit_bio 한다.
 *   이로 인해 NVMe queue 로의 실제 제출 시점이 지연되지만 우선순위 역전을
 *   방지한다.
 */

void blkcg_punt_bio_submit(struct bio *bio)
{
	struct blkcg_gq *blkg = bio->bi_blkg;
	/* NVMe: bio 에 연결된 cgroup context; NVMe SQ/CQ 선택의 상위 기준 */

	if (blkg->parent) {
		/* NVMe: root 가 아닌 cgroup 의 bio 만 비동기 우회 제출 */
		spin_lock(&blkg->async_bio_lock);
		/* NVMe: async_bios 리스트 보호 */
		bio_list_add(&blkg->async_bios, bio);
		/* NVMe: bio 를 async 대기열에 추가; 실제 NVMe doorbell 은 workqueue 에서 지연 */
		spin_unlock(&blkg->async_bio_lock);
		queue_work(blkcg_punt_bio_wq, &blkg->async_bio_work);
		/* NVMe: workqueue 에서 blkg_async_bio_workfn 실행 -> submit_bio -> NVMe 경로 */
	} else {
		/* never bounce for the root cgroup */
		submit_bio(bio);
		/* NVMe: root cgroup bio 는 직접 제출; 추가 지연 없이 NVMe SQ 로 진입 */
	}
}
EXPORT_SYMBOL_GPL(blkcg_punt_bio_submit);

static int __init blkcg_punt_bio_init(void)
{
	blkcg_punt_bio_wq = alloc_workqueue("blkcg_punt_bio",
	/* NVMe: unbound workqueue; kthread 우회 bio 가 NVMe submit 경로로 진입할 때 CPU affinity 와 무관하게 스케줄링 */
					    WQ_MEM_RECLAIM | WQ_FREEZABLE |
					    WQ_UNBOUND | WQ_SYSFS, 0);
	if (!blkcg_punt_bio_wq)
	/* NVMe: workqueue 할당 실패 시 NVMe kthread 우회 제출 인프라 초기화 실패 */
		return -ENOMEM;
	return 0;
}
subsys_initcall(blkcg_punt_bio_init);
	/* NVMe: 서브시스템 초기화 시 workqueue 등록; NVMe IO 우회 경로 준비 */
#endif /* CONFIG_BLK_CGROUP_PUNT_BIO */

/**
 * bio_blkcg_css - return the blkcg CSS associated with a bio
 * @bio: target bio
 *
 * This returns the CSS for the blkcg associated with a bio, or %NULL if not
 * associated. Callers are expected to either handle %NULL or know association
 * has been done prior to calling this.
 */
/*
 * bio_blkcg_css - bio 에 연결된 blkcg 의 css 반환
 *
 * 호출 경로: blk_cgroup_mergeable() 등
 * NVMe 연결점: bio->bi_blkg 를 통해 NVMe IO 가 속한 cgroup 을 조회한다.
 *   merge 가능 여부 판단 등에서 cgroup 이 일치해야 같은 SQ batching 대상으로
 *   볼 수 있다.
 */

struct cgroup_subsys_state *bio_blkcg_css(struct bio *bio)
{
	if (!bio || !bio->bi_blkg)
	/* NVMe: bio 가 없거나 cgroup 미연결 시 NULL; NVMe passthrough/admin 명령 등 */
		return NULL;
	return &bio->bi_blkg->blkcg->css;
	/* NVMe: bio->bi_blkg 경로로 cgroup css 반환 */
}
EXPORT_SYMBOL_GPL(bio_blkcg_css);

/**
 * blkcg_parent - get the parent of a blkcg
 * @blkcg: blkcg of interest
 *
 * Return the parent blkcg of @blkcg.  Can be called anytime.
 */
static inline struct blkcg *blkcg_parent(struct blkcg *blkcg)
{
	return css_to_blkcg(blkcg->css.parent);
	/* NVMe: css.parent 를 blkcg 로 변환; NVMe queue 의 cgroup 트리 탐색 */
}

/**
 * blkg_alloc - allocate a blkg
 * @blkcg: block cgroup the new blkg is associated with
 * @disk: gendisk the new blkg is associated with
 * @gfp_mask: allocation mask to use
 *
 * Allocate a new blkg associating @blkcg and @disk.
 */
/*
 * blkg_alloc - blkcg 와 request_queue(disk)를 연결하는 blkg 할당
 *
 * 호출 경로: blkcg_init_disk() -> blkg_alloc()
 *            blkg_conf_prep() -> blkg_alloc()
 * NVMe 연결점: NVMe namespace 의 gendisk 와 cgroup 을 연결하는 blkg 를
 *   생성한다. 이후 pd_alloc_fn() 으로 throtl/BFQ/ioprio 정책 데이터를 할당해
 *   nvme_queue_rq() 호출 시 적용할 cgroup 단위 상태를 준비한다.
 */

static struct blkcg_gq *blkg_alloc(struct blkcg *blkcg, struct gendisk *disk,
				   gfp_t gfp_mask)
{
	struct blkcg_gq *blkg;
	/* NVMe: 할당될 blkcg_gq 객체; NVMe request_queue 와 cgroup 의 1:1 연결체 */
	int i, cpu;

	/* alloc and init base part */
	blkg = kzalloc_node(sizeof(*blkg), gfp_mask, disk->queue->node);
	/* NVMe: NVMe namespace q 의 NUMA node 에 blkg 할당; 메모리 지역성으로 NVMe CQ 완료 경로 성능 향상(추정) */
	if (!blkg)
		/* NVMe: blkg 할당 실패 시 NVMe cgroup 분류 불가; 상위에서 root blkg 로 fallback */
		return NULL;
	if (percpu_ref_init(&blkg->refcnt, blkg_release, 0, gfp_mask))
	/* NVMe: percpu_ref 초기화; NVMe IO 가 blkg 를 참조하는 동안 메모리 유지 */
		goto out_free_blkg;
	blkg->iostat_cpu = alloc_percpu_gfp(struct blkg_iostat_set, gfp_mask);
	/* NVMe: NVMe 멀티 코어 CQ 완료용 per-cpu 통계 영역 할당 */
	if (!blkg->iostat_cpu)
		goto out_exit_refcnt;
		/* NVMe: per-cpu 통계 할당 실패 시 blkg 할당 롤백 */
	if (!blk_get_queue(disk->queue))
	/* NVMe: request_queue(NVMe namespace q) 참조 획득 실패 시 롤백; queue 가 dying 상태면 실패할 수 있음 */
		goto out_free_iostat;

	blkg->q = disk->queue;
	/* NVMe: blkg 가 속한 NVMe request_queue 설정 */
	INIT_LIST_HEAD(&blkg->q_node);
	/* NVMe: request_queue->blkg_list 연결 준비 */
	blkg->blkcg = blkcg;
	/* NVMe: blkg 가 대표하는 cgroup 설정 */
	blkg->iostat.blkg = blkg;
	/* NVMe: global 통계가 역참조할 blkg 설정 */
#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	spin_lock_init(&blkg->async_bio_lock);
	/* NVMe: kthread 우회 bio 리스트 보호 */
	bio_list_init(&blkg->async_bios);
	/* NVMe: kthread 우회 대기 bio 리스트 초기화 */
	INIT_WORK(&blkg->async_bio_work, blkg_async_bio_workfn);
#endif

	u64_stats_init(&blkg->iostat.sync);
	/* NVMe: global 통계의 u64_stats_sync 초기화 (32bit 배경) */
	for_each_possible_cpu(cpu) {
	/* NVMe: 모든 CPU 에 대해 per-cpu 통계 초기화; NVMe 멀티 큐 완료 경로의 lockless 집계 준비 */
		u64_stats_init(&per_cpu_ptr(blkg->iostat_cpu, cpu)->sync);
		/* NVMe: per-cpu 통계의 seqlock/u64_stats_sync 초기화 */
		per_cpu_ptr(blkg->iostat_cpu, cpu)->blkg = blkg;
		/* NVMe: per-cpu 통계가 역참조할 blkg 설정 */
	}

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* NVMe: 활성화된 모든 cgroup 정책에 대해 private data 할당/연결 */
	/* NVMe: queue 제거 시 모든 활성화된 cgroup 정책 비트 클리어 */
		struct blkcg_policy *pol = blkcg_policy[i];
		/* NVMe: i 번째 정책(throtl/BFQ/ioprio) 포인터 */
		struct blkg_policy_data *pd;
		/* NVMe: 정책별 private data 포인터; bfq_queue/throtl_data 등 */

		if (!blkcg_policy_enabled(disk->queue, pol))
		/* NVMe: 해당 queue(NVMe namespace)에서 이 정책이 켜져 있을 때만 pd 할당 */
		/* NVMe: 해당 queue(NVMe namespace)에서 이 정책이 켜져 있을 때만 할당 */
			continue;

		/* alloc per-policy data and attach it to blkg */
		pd = pol->pd_alloc_fn(disk, blkcg, gfp_mask);
		/* NVMe: 정책별 private data 할당 (예: throtl_data, bfq_queue); NVMe queue depth/latency 제어 상태 */
		/* NVMe: 정책별 private data 할당 (예: throtl_data, bfq_queue) */
		if (!pd)
		/* NVMe: pd 할당 실패 시 지금까지 할당한 pd 롤백 */
			goto out_free_pds;
		blkg->pd[i] = pd;
		/* NVMe: blkg 에 정책 데이터 연결; nvme_queue_rq() 시 이 데이터로 SQ/CQ 선택/제한 */
		pd->blkg = blkg;
		/* NVMe: pd 가 역참조할 blkg 설정 */
		pd->plid = i;
		/* NVMe: policy id 기록; q->blkcg_pols 비트와 대응 */
		pd->online = false;
		/* NVMe: 아직 online 콜백 전; IO 경로에서 pd 사용은 online 이후 */
	}

	return blkg;

out_free_pds:
	while (--i >= 0)
	/* NVMe: 할당 실패 시 역순으로 이미 할당한 정책 데이터 해제 */
		if (blkg->pd[i])
		/* NVMe: i 번째 pd 가 존재하면 해제 */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
			/* NVMe: throtl/BFQ 상태 해제; NVMe queue 의 cgroup 제어 상태 복구 */
	blk_put_queue(disk->queue);
out_free_iostat:
	free_percpu(blkg->iostat_cpu);
out_exit_refcnt:
	percpu_ref_exit(&blkg->refcnt);
out_free_blkg:
	kfree(blkg);
	return NULL;
}

/*
 * If @new_blkg is %NULL, this function tries to allocate a new one as
 * necessary using %GFP_NOWAIT.  @new_blkg is always consumed on return.
 */
/*
 * blkg_create - blkg 를 생성하고 radix tree/list 에 등록
 *
 * 호출 경로: blkcg_init_disk() -> blkg_create()
 *            blkg_lookup_create() -> blkg_create()
 * NVMe 연결점: root blkg 부터 타겟 blkcg 까지 부모를 따라 남겨가며 생성해
 *   하위 cgroup 이 항상 상위 blkg->parent 를 참조할 수 있게 한다. 등록 후
 *   q->blkg_list 에 추가되어 NVMe request_queue 의 cgroup 분류 체계가 완성된다.
 */

static struct blkcg_gq *blkg_create(struct blkcg *blkcg, struct gendisk *disk,
				    struct blkcg_gq *new_blkg)
{
	struct blkcg_gq *blkg;
	/* NVMe: 생성/등록될 blkg */
	int i, ret;

	lockdep_assert_held(&disk->queue->queue_lock);
	/* NVMe: queue_lock 이 잡힌 상태에서만 blkg 생성; NVMe request_queue 상태 일관성 보호 */

	/* request_queue is dying, do not create/recreate a blkg */
	if (blk_queue_dying(disk->queue)) {
	/* NVMe: queue 가 제거 중이면 새 blkg 를 만들지 않음; NVMe controller reset/remove 경로 */
	/* NVMe: queue 가 제거 중이면 새 blkg 를 만들지 않음 */
		ret = -ENODEV;
		/* NVMe: dying queue 에 대한 blkg 생성 거부 */
		goto err_free_blkg;
	}

	/* blkg holds a reference to blkcg */
	if (!css_tryget_online(&blkcg->css)) {
	/* NVMe: cgroup 이 online 상태가 아니면 blkg 생성 실패; cgroup 소멸 중일 때 NVMe IO spill 방지 */
	/* NVMe: cgroup 이 online 상태가 아니면 blkg 생성 실패 */
		ret = -ENODEV;
		goto err_free_blkg;
	}

	/* allocate */
	if (!new_blkg) {
	/* NVMe: 호출자가 미리 할당한 blkg 가 없으면 GFP_NOWAIT 로 시도 */
		new_blkg = blkg_alloc(blkcg, disk, GFP_NOWAIT);
		/* NVMe: IO 경로에서 락을 잡은 채로 빠르게 blkg 할당 시도 */
		if (unlikely(!new_blkg)) {
		/* NVMe: GFP_NOWAIT 실패 시 -ENOMEM; 상위 blkg 로 fallback 가능 */
			ret = -ENOMEM;
			goto err_put_css;
		}
	}
	blkg = new_blkg;
	/* NVMe: 할당받은 blkg 를 실제 등록 대상으로 설정 */

	/* link parent */
	if (blkcg_parent(blkcg)) {
	/* NVMe: root 가 아닌 cgroup 이면 부모 blkg 와 연결 */
		blkg->parent = blkg_lookup(blkcg_parent(blkcg), disk->queue);
		/* NVMe: 상위 cgroup blkg 를 찾아 계층 구조 연결; throttle/통계 전파 경로 */
		/* NVMe: 상위 cgroup blkg 를 찾아 계층 구조 연결 */
		if (WARN_ON_ONCE(!blkg->parent)) {
		/* NVMe: 부모 blkg 가 없으면 계층 구조 파괴; 버그로 간주 */
			ret = -ENODEV;
			goto err_put_css;
		}
		blkg_get(blkg->parent);
		/* NVMe: 부모 참조 획득; 자식 blkg 수명 동안 부모 유지 */
	}

	/* invoke per-policy init */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* NVMe: 생성된 blkg 의 정책 데이터 초기화; throtl/bfq 상태 기본값 설정 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (blkg->pd[i] && pol->pd_init_fn)
		/* NVMe: pd 가 할당되고 init 콜백이 있을 때만 초기화 */
		/* NVMe: 정책 초기화 콜백 (throtl/bfq 상태 기본값 설정) */
			pol->pd_init_fn(blkg->pd[i]);
	}

	/* insert */
	spin_lock(&blkcg->lock);
	/* NVMe: blkcg lock 획득; radix tree/list 와 rstat 간 동기화 */
	ret = radix_tree_insert(&blkcg->blkg_tree, disk->queue->id, blkg);
	/* NVMe: queue id(NVMe namespace 식별자)로 blkg 색인 추가 */
	if (likely(!ret)) {
	/* NVMe: radix tree 삽입 성공 시 list 에도 등록 */
		hlist_add_head_rcu(&blkg->blkcg_node, &blkcg->blkg_list);
		/* NVMe: RCU read-side(blkg_lookup)에서 볼 수 있게 list 에 추가 */
		list_add(&blkg->q_node, &disk->queue->blkg_list);
		/* NVMe: request_queue 의 blkg list 에 등록 */

		for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* NVMe: list 등록 후 정책 online; 이 시점부터 IO 경로에서 pd 참조 가능 */
			struct blkcg_policy *pol = blkcg_policy[i];

			if (blkg->pd[i]) {
			/* NVMe: 할당된 정책 데이터가 있으면 online 처리 */
				if (pol->pd_online_fn)
				/* NVMe: 정책 online 콜백, 이제 IO 경로에서 참조 가능 */
					pol->pd_online_fn(blkg->pd[i]);
				/* NVMe: 정책 online 콜백, 이제 IO 경로에서 참조 가능 */
				blkg->pd[i]->online = true;
				/* NVMe: pd online 상태 표시 */
			}
		}
	}
	blkg->online = true;
	/* NVMe: IO 경로에서 이 blkg 를 사용할 수 있음을 표시 */
	spin_unlock(&blkcg->lock);

	if (!ret)
	/* NVMe: 등록 성공; bio_associate_blkg 에서 사용 가능 */
		return blkg;

	/* @blkg failed fully initialized, use the usual release path */
	blkg_put(blkg);
		/* NVMe: 초기화 실패 시 blkg 참조 반납 -> blkg_free_workfn 으로 해제 */
	return ERR_PTR(ret);

err_put_css:
	css_put(&blkcg->css);
		/* NVMe: css_tryget_online 으로 획득한 css 참조 반납 */
err_free_blkg:
	if (new_blkg)
	/* NVMe: 할당된 new_blkg 메모리 해제 예약 */
		blkg_free(new_blkg);
	return ERR_PTR(ret);
}

/**
 * blkg_lookup_create - lookup blkg, try to create one if not there
 * @blkcg: blkcg of interest
 * @disk: gendisk of interest
 *
 * Lookup blkg for the @blkcg - @disk pair.  If it doesn't exist, try to
 * create one.  blkg creation is performed recursively from blkcg_root such
 * that all non-root blkg's have access to the parent blkg.  This function
 * should be called under RCU read lock and takes @disk->queue->queue_lock.
 *
 * Returns the blkg or the closest blkg if blkg_create() fails as it walks
 * down from root.
 */
/*
 * blkg_lookup_create - blkg 를 찾고 없으면 생성
 *
 * 호출 경로: bio_associate_blkg() -> blkg_lookup_create()
 * NVMe 연결점: submit_bio -> bio_associate_blkg -> blkg_lookup_create ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell). 이 함수에서 bio 의 cgroup context 를 확정한다.
 *   없으면 GFP_NOWAIT 으로 생성을 시도하고 실패하면 가장 가까운 부모 blkg 로
 *   fallback 한다.
 */

static struct blkcg_gq *blkg_lookup_create(struct blkcg *blkcg,
		struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* NVMe: bio 의 대상 NVMe namespace request_queue */
	struct blkcg_gq *blkg;
	/* NVMe: 검색/생성 결과 blkg */
	unsigned long flags;

	WARN_ON_ONCE(!rcu_read_lock_held());
	/* NVMe: RCU read-side 필요; blkg_lookup() 과 radix tree 접근 보호 */

	blkg = blkg_lookup(blkcg, q);
	/* NVMe: radix tree 로 기존 blkg 검색; O(1) cgroup 분류 */
	if (blkg)
		/* NVMe: 기존 blkg 반환; NVMe SQ/CQ 선택에 사용 */
		return blkg;

	spin_lock_irqsave(&q->queue_lock, flags);
	/* NVMe: queue_lock 획득; blkg 생성과 동시 제출 경쟁 보호 */
	blkg = blkg_lookup(blkcg, q);
	/* NVMe: 락 획득 후 재확인; 다른 CPU 가 이미 생성했을 수 있음 */
	if (blkg) {
		if (blkcg != &blkcg_root &&
		/* NVMe: root 가 아니고 hint 가 다륾면 hint 갱신; 이후 bio 제출 시 탐색 가속 */
		    blkg != rcu_dereference(blkcg->blkg_hint))
			rcu_assign_pointer(blkcg->blkg_hint, blkg);
			/* NVMe: RCU 배리어 내장; hint 가 일관되게 보이도록 설정 */
		goto found;
	}

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.  Returns the closest
	 * blkg to the intended blkg should blkg_create() fail.
	 */
	while (true) {
	/* NVMe: root 에서 목표 cgroup 까지 부모를 따라 남겨가며 blkg 생성; cgroup 계층 무결성 */
	/* NVMe: root 에서 목표 cgroup 까지 부모를 따라 남겨가며 blkg 생성 */
		struct blkcg *pos = blkcg;
		/* NVMe: 현재 생성해야 할 cgroup 위치 */
		struct blkcg *parent = blkcg_parent(blkcg);
		/* NVMe: pos 의 부모 cgroup */
		struct blkcg_gq *ret_blkg = q->root_blkg;
		/* NVMe: 생성 실패 시 root blkg 로 fallback 준비 */

		while (parent) {
		/* NVMe: 부모 중 가장 가까운 존재하는 blkg 를 찾아 fallback 지점 확보 */
			blkg = blkg_lookup(parent, q);
		/* NVMe: 부모 cgroup 의 blkg 검색 */
			if (blkg) {
				/* remember closest blkg */
				ret_blkg = blkg;
			/* NVMe: 생성 실패 시 이 blkg 로 IO 를 spill 할 수 있음 */
				break;
			}
			pos = parent;
		/* NVMe: 아직 blkg 가 없는 가장 가까운 조상으로 이동 */
			parent = blkcg_parent(parent);
		/* NVMe: 한 단계 더 위의 조상 탐색 */
		}

		blkg = blkg_create(pos, disk, NULL);
		/* NVMe: pos cgroup 의 blkg 생성; NVMe queue 의 cgroup 분류 노드 추가 */
		if (IS_ERR(blkg)) {
		/* NVMe: 생성 실패 시 가장 가까운 부모 blkg 로 fallback; NVMe IO 누락 방지 */
			blkg = ret_blkg;
			break;
		}
		if (pos == blkcg)
			break;
	}

found:
	spin_unlock_irqrestore(&q->queue_lock, flags);
	/* NVMe: queue_lock 해제; 이후 submit_bio 가 blkg 를 참조 가능 */
	return blkg;
}

/*
 * blkg_destroy - blkg 를 tree/list 에서 제거하고 refcnt 를 종료
 *
 * 호출 경로: blkcg_destroy_blkgs() -> blkg_destroy()
 *            blkg_destroy_all() -> blkg_destroy()
 * NVMe 연결점: NVMe namespace 제거 또는 cgroup 삭제 시 해당 cgroup 에 대한
 *   IO 분류/정책을 중단한다. percpu_ref_kill() 로 참조 카운트를 감소시키고
 *   blkg_free_workfn() 에서 최종 메모리 해제가 일어난다.
 */

static void blkg_destroy(struct blkcg_gq *blkg)
{
	struct blkcg *blkcg = blkg->blkcg;
	/* NVMe: blkg 가 속한 cgroup */
	int i;

	lockdep_assert_held(&blkg->q->queue_lock);
	/* NVMe: queue_lock 보호 하에서만 blkg 제거; NVMe request_queue 상태와 동기화 */
	lockdep_assert_held(&blkcg->lock);
	/* NVMe: blkcg lock 보호 하에서만 radix tree/list 조작 */

	/*
	 * blkg stays on the queue list until blkg_free_workfn(), see details in
	 * blkg_free_workfn(), hence this function can be called from
	 * blkcg_destroy_blkgs() first and again from blkg_destroy_all() before
	 * blkg_free_workfn().
	 */
	if (hlist_unhashed(&blkg->blkcg_node))
	/* NVMe: 이미 제거된 blkg 는 다시 destroy 하지 않음; 중복 제거 방지 */
	/* NVMe: 이미 제거된 blkg 는 다시 destroy 하지 않음 */
		return;

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* NVMe: 활성화된 정책들을 offline -> free 경로로 전환 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (blkg->pd[i] && blkg->pd[i]->online) {
		/* NVMe: online 상태인 pd 만 offline 처리; NVMe IO 경로에서 pd 접근 차단 */
			blkg->pd[i]->online = false;
			/* NVMe: pd offline 표시 */
			if (pol->pd_offline_fn)
				pol->pd_offline_fn(blkg->pd[i]);
		}
	}

	blkg->online = false;
	/* NVMe: blkg 를 IO 경로에서 사용 불가로 표시; 이후 bio 는 상위 blkg 로 spill */

	radix_tree_delete(&blkcg->blkg_tree, blkg->q->id);
	/* NVMe: queue id 기반 radix tree 색인 제거 */
	hlist_del_init_rcu(&blkg->blkcg_node);
	/* NVMe: RCU read-side 가 완료될 때까지 메모리는 유지; NVMe CQ 완료 경로 안전성 */

	/*
	 * Both setting lookup hint to and clearing it from @blkg are done
	 * under queue_lock.  If it's not pointing to @blkg now, it never
	 * will.  Hint assignment itself can race safely.
	 */
	if (rcu_access_pointer(blkcg->blkg_hint) == blkg)
	/* NVMe: hint 가 제거 대상 blkg 를 가리키면 NULL 로 클리어 */
		rcu_assign_pointer(blkcg->blkg_hint, NULL);
		/* NVMe: RCU 배리어를 통해 hint 일관성 유지 */

	/*
	 * Put the reference taken at the time of creation so that when all
	 * queues are gone, group can be destroyed.
	 */
	percpu_ref_kill(&blkg->refcnt);
	/* NVMe: blkg 참조 카운트 종료, 이후 IO 는 root blkg 로 spill; percpu_ref 가 0이 되면 RCU 해제 */
	/* NVMe: blkg 참조 카운트 종료, 이후 IO 는 root blkg 로 spill */
}

/*
 * blkg_destroy_all - 디스크의 모든 blkg 를 일괄 제거
 *
 * 호출 경로: blkcg_exit_disk() -> blkg_destroy_all()
 * NVMe 연결점: NVMe namespace 가 사라질 때 해당 request_queue 의 모든 cgroup
 *   연결을 해제한다. BLKG_DESTROY_BATCH_SIZE 단위로 락을 풀어 softlockup 을
 *   방지한다.
 */

static void blkg_destroy_all(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* NVMe: 제거 대상 NVMe namespace request_queue */
	struct blkcg_gq *blkg;
	int count = BLKG_DESTROY_BATCH_SIZE;
	/* NVMe: 한 번에 제거할 blkg 개수; NVMe queue lock 장기 점유 방지 */
	int i;

restart:
	spin_lock_irq(&q->queue_lock);
	/* NVMe: queue_lock 획득; blkg_list 순회 보호 */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* NVMe: queue 의 모든 blkg 를 순회하며 제거 */
	/* NVMe: 메모리 부족 시 이미 추가된 정책 데이터 모두 롤백 */
		struct blkcg *blkcg = blkg->blkcg;
		/* NVMe: 현재 blkg 의 cgroup */

		if (hlist_unhashed(&blkg->blkcg_node))
		/* NVMe: 이미 제거된 blkg 는 스킵 */
			continue;

		spin_lock(&blkcg->lock);
		/* NVMe: blkcg lock 추가 획득; radix tree/list 동기화 */
		blkg_destroy(blkg);
		/* NVMe: blkg 제거 및 refcnt 종료 */
		spin_unlock(&blkcg->lock);

		/*
		 * in order to avoid holding the spin lock for too long, release
		 * it when a batch of blkgs are destroyed.
		 */
		if (!(--count)) {
		/* NVMe: 배치 단위로 queue_lock 해제; softirq/NVMe ISR 응답 지연 방지 */
			count = BLKG_DESTROY_BATCH_SIZE;
			spin_unlock_irq(&q->queue_lock);
			/* NVMe: 스케줄링 양보 */
			cond_resched();
			goto restart;
		}
	}

	/*
	 * Mark policy deactivated since policy offline has been done, and
	 * the free is scheduled, so future blkcg_deactivate_policy() can
	 * be bypassed
	 */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* NVMe: queue 에서 모든 cgroup 정책 비트 클리어 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (pol)
		/* NVMe: 이 queue(NVMe namespace)에서 해당 정책 비활성화 표시 */
			__clear_bit(pol->plid, q->blkcg_pols);
			/* NVMe: queue 에서 정책 비활성화 표시 */
			/* NVMe: 이 queue 에서 해당 정책 비활성화 표시 */
	}

	q->root_blkg = NULL;
	/* NVMe: root blkg 제거 완료, 이후 disk rebind 가능 */
	spin_unlock_irq(&q->queue_lock);

	wake_up_var(&q->root_blkg);
	/* NVMe: root_blkg NULL 대기 중인 blkcg_init_disk() 깨우기 */
}

static void blkg_iostat_set(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* NVMe: BLKG_IOSTAT_READ/WRITE/DISCARD 세 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* NVMe: read/write/discard 세 항목에 대해 복사; NVMe opcode 별 통계 분류 */
	/* NVMe: read/write/discard 세 항목에 대해 복사 */
		dst->bytes[i] = src->bytes[i];
		/* NVMe: i 유형(read/write/discard) 바이트 복사 */
		dst->ios[i] = src->ios[i];
		/* NVMe: i 유형 IO 횟수 복사 */
	}
}

static void __blkg_clear_stat(struct blkg_iostat_set *bis)
{
	struct blkg_iostat cur = {0};
	/* NVMe: 0 으로 초기화할 임시 통계 구조체 */
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* NVMe: u64_stats_seqlock 진입; 32bit NVMe 통계 업데이트의 readers/writers 동기화 */
	blkg_iostat_set(&bis->cur, &cur);
	/* NVMe: cur 통계 클리어 */
	blkg_iostat_set(&bis->last, &cur);
	/* NVMe: last 통계 클리어; delta 계산 기준점 재설정 */
	u64_stats_update_end_irqrestore(&bis->sync, flags);
	/* NVMe: seqlock 해제; NVMe 통계 readers 에게 일관된 값 공개 */
}

static void blkg_clear_stat(struct blkcg_gq *blkg)
{
	int cpu;

	for_each_possible_cpu(cpu) {
	/* NVMe: NVMe 멀티 코어 CQ 완료가 사용한 모든 per-cpu 통계 영역 초기화 */
		struct blkg_iostat_set *s = per_cpu_ptr(blkg->iostat_cpu, cpu);
		/* NVMe: cpu 번호의 per-cpu blkg_iostat_set 획득 */

		__blkg_clear_stat(s);
	}
	__blkg_clear_stat(&blkg->iostat);
}

/*
 * blkcg_reset_stats - cgroup 의 blkio 통계를 초기화
 *
 * 호출 경로: cgroup legacy reset_stats 쓰기 -> blkcg_reset_stats()
 * NVMe 연결점: 해당 cgroup 의 NVMe IO 누적 통계(bytes/ios)와 각 정책의
 *   통계를 초기화한다.
 */

static int blkcg_reset_stats(struct cgroup_subsys_state *css,
			     struct cftype *cftype, u64 val)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	/* NVMe: 대상 cgroup */
	struct blkcg_gq *blkg;
	int i;

	pr_info_once("blkio.%s is deprecated\n", cftype->name);
	mutex_lock(&blkcg_pol_mutex);
	/* NVMe: 정책 등록/해제와 reset 경쟁 보호 */
	spin_lock_irq(&blkcg->lock);
	/* NVMe: blkcg 의 blkg_list 순회 보호 */

	/*
	 * Note that stat reset is racy - it doesn't synchronize against
	 * stat updates.  This is a debug feature which shouldn't exist
	 * anyway.  If you get hit by a race, retry.
	 */
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
	/* NVMe: 이 cgroup 의 모든 NVMe namespace blkg 순회 */
		blkg_clear_stat(blkg);
		for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* NVMe: 각 정책별 통계 리셋 콜백 순회 */
			struct blkcg_policy *pol = blkcg_policy[i];

			if (blkg->pd[i] && pol->pd_reset_stats_fn)
			/* NVMe: 정책별 통계 리셋 함수 호출; throtl/BFQ NVMe 통계 초기화 */
				pol->pd_reset_stats_fn(blkg->pd[i]);
		}
	}

	spin_unlock_irq(&blkcg->lock);
	mutex_unlock(&blkcg_pol_mutex);
	return 0;
}

const char *blkg_dev_name(struct blkcg_gq *blkg)
{
	if (!blkg->q->disk)
	/* NVMe: disk 가 없으면 이름 없음; 미연결 queue */
		return NULL;
	return bdi_dev_name(blkg->q->disk->bdi);
	/* NVMe: bdi 이름 반환; NVMe namespace stat 출력용 */
}

/**
 * blkcg_print_blkgs - helper for printing per-blkg data
 * @sf: seq_file to print to
 * @blkcg: blkcg of interest
 * @prfill: fill function to print out a blkg
 * @pol: policy in question
 * @data: data to be passed to @prfill
 * @show_total: to print out sum of prfill return values or not
 *
 * This function invokes @prfill on each blkg of @blkcg if pd for the
 * policy specified by @pol exists.  @prfill is invoked with @sf, the
 * policy data and @data and the matching queue lock held.  If @show_total
 * is %true, the sum of the return values from @prfill is printed with
 * "Total" label at the end.
 *
 * This is to be used to construct print functions for
 * cftype->read_seq_string method.
 */
void blkcg_print_blkgs(struct seq_file *sf, struct blkcg *blkcg,
		       u64 (*prfill)(struct seq_file *,
				     struct blkg_policy_data *, int),
		       const struct blkcg_policy *pol, int data,
		       bool show_total)
{
	struct blkcg_gq *blkg;
	/* NVMe: 순회 중인 blkg */
	u64 total = 0;
	/* NVMe: 출력값 합산; NVMe namespace 간 cgroup 통계 집계 */

	rcu_read_lock();
	/* NVMe: blkg_list RCU read-side 보호 */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
	/* NVMe: cgroup 의 모든 NVMe namespace blkg 를 RCU 로 순회 */
		spin_lock_irq(&blkg->q->queue_lock);
		/* NVMe: blkg 출력 시 해당 NVMe queue lock 획득 */
		if (blkcg_policy_enabled(blkg->q, pol))
		/* NVMe: 해당 queue 에서 정책이 활성화된 blkg 만 출력 */
			total += prfill(sf, blkg->pd[pol->plid], data);
			/* NVMe: 정책별 출력 함수 호출; NVMe queue 별 throtl/BFQ 상태 노출 */
		spin_unlock_irq(&blkg->q->queue_lock);
	}
	rcu_read_unlock();

	if (show_total)
	/* NVMe: namespace 간 NVMe cgroup 통계 합계 출력 */
		seq_printf(sf, "Total %llu\n", (unsigned long long)total);
}
EXPORT_SYMBOL_GPL(blkcg_print_blkgs);

/**
 * __blkg_prfill_u64 - prfill helper for a single u64 value
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @v: value to print
 *
 * Print @v to @sf for the device associated with @pd.
 */
u64 __blkg_prfill_u64(struct seq_file *sf, struct blkg_policy_data *pd, u64 v)
{
	const char *dname = blkg_dev_name(pd->blkg);
	/* NVMe: blkg 의 NVMe 장치 이름 */

	if (!dname)
	/* NVMe: 장치명이 없으면 출력 불가 */
		return 0;

	seq_printf(sf, "%s %llu\n", dname, (unsigned long long)v);
	return v;
}
EXPORT_SYMBOL_GPL(__blkg_prfill_u64);

/**
 * blkg_conf_init - initialize a blkg_conf_ctx
 * @ctx: blkg_conf_ctx to initialize
 * @input: input string
 *
 * Initialize @ctx which can be used to parse blkg config input string @input.
 * Once initialized, @ctx can be used with blkg_conf_open_bdev() and
 * blkg_conf_prep(), and must be cleaned up with blkg_conf_exit().
 */
void blkg_conf_init(struct blkg_conf_ctx *ctx, char *input)
{
	*ctx = (struct blkg_conf_ctx){ .input = input };
	/* NVMe: 입력 문자열 저장; MAJ:MIN 파싱 시작점 */
}
EXPORT_SYMBOL_GPL(blkg_conf_init);

/**
 * blkg_conf_open_bdev - parse and open bdev for per-blkg config update
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Parse the device node prefix part, MAJ:MIN, of per-blkg config update from
 * @ctx->input and get and store the matching bdev in @ctx->bdev. @ctx->body is
 * set to point past the device node prefix.
 *
 * This function may be called multiple times on @ctx and the extra calls become
 * NOOPs. blkg_conf_prep() implicitly calls this function. Use this function
 * explicitly if bdev access is needed without resolving the blkcg / policy part
 * of @ctx->input. Returns -errno on error.
 */
int blkg_conf_open_bdev(struct blkg_conf_ctx *ctx)
{
	char *input = ctx->input;
	/* NVMe: 파싱 중인 입력 문자열 */
	unsigned int major, minor;
	/* NVMe: NVMe block 장치 major/minor 번호 */
	struct block_device *bdev;
	int key_len;

	if (ctx->bdev)
	/* NVMe: 이미 bdev 가 열린 경우 NOOP */
		return 0;

	if (sscanf(input, "%u:%u%n", &major, &minor, &key_len) != 2)
	/* NVMe: MAJ:MIN 파싱 실패 */
		return -EINVAL;

	input += key_len;
	/* NVMe: MAJ:MIN 뒤로 포인터 이동 */
	if (!isspace(*input))
	/* NVMe: MAJ:MIN 뒤에 공백이 없으면 형식 오류 */
		return -EINVAL;
	input = skip_spaces(input);
	/* NVMe: 선행 공백 스킵; 정책별 본문 시작점 */

	bdev = blkdev_get_no_open(MKDEV(major, minor), false);
	/* NVMe: major:minor 에 해당하는 block_device 를 연다(NVMe namespace bdev) */
	if (!bdev)
	/* NVMe: 장치를 찾을 수 없음; NVMe namespace 미존재 */
		return -ENODEV;
	if (bdev_is_partition(bdev)) {
	/* NVMe: partition 은 직접 설정 불가; NVMe namespace 전체에 적용 */
		blkdev_put_no_open(bdev);
		return -ENODEV;
	}

	mutex_lock(&bdev->bd_queue->rq_qos_mutex);
	/* NVMe: request_queue QoS lock 획득; NVMe queue 정책 상태 보호 */
	if (!disk_live(bdev->bd_disk)) {
	/* NVMe: disk 가 live 상태가 아니면 설정 거부; NVMe controller offline/remove */
		blkdev_put_no_open(bdev);
		mutex_unlock(&bdev->bd_queue->rq_qos_mutex);
		return -ENODEV;
	}

	ctx->body = input;
	ctx->bdev = bdev;
	return 0;
}
/*
 * Similar to blkg_conf_open_bdev, but additionally freezes the queue,
 * ensures the correct locking order between freeze queue and q->rq_qos_mutex.
 *
 * This function returns negative error on failure. On success it returns
 * memflags which must be saved and later passed to blkg_conf_exit_frozen
 * for restoring the memalloc scope.
 */
unsigned long __must_check blkg_conf_open_bdev_frozen(struct blkg_conf_ctx *ctx)
{
	int ret;
	unsigned long memflags;

	if (ctx->bdev)
	/* NVMe: 이미 열린 bdev 가 있으면 오류 */
		return -EINVAL;

	ret = blkg_conf_open_bdev(ctx);
	/* NVMe: bdev 열기 및 live 검증 */
	if (ret < 0)
		return ret;
	/*
	 * At this point, we haven’t started protecting anything related to QoS,
	 * so we release q->rq_qos_mutex here, which was first acquired in blkg_
	 * conf_open_bdev. Later, we re-acquire q->rq_qos_mutex after freezing
	 * the queue to maintain the correct locking order.
	 */
	mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);
	/* NVMe: freeze 전 lock 해제; 올바른 lock ordering 유지 */

	memflags = blk_mq_freeze_queue(ctx->bdev->bd_queue);
	/* NVMe: blk-mq queue freeze; QUEUE_FLAG_QUIESCED 와 유사하게 NVMe IO 제출/완료 일시 정지 */
	mutex_lock(&ctx->bdev->bd_queue->rq_qos_mutex);
	/* NVMe: freeze 후 다시 QoS lock 획득 */

	return memflags;
}

/**
 * blkg_conf_prep - parse and prepare for per-blkg config update
 * @blkcg: target block cgroup
 * @pol: target policy
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Parse per-blkg config update from @ctx->input and initialize @ctx
 * accordingly. On success, @ctx->body points to the part of @ctx->input
 * following MAJ:MIN, @ctx->bdev points to the target block device and
 * @ctx->blkg to the blkg being configured.
 *
 * blkg_conf_open_bdev() may be called on @ctx beforehand. On success, this
 * function returns with queue lock held and must be followed by
 * blkg_conf_exit().
 */
int blkg_conf_prep(struct blkcg *blkcg, const struct blkcg_policy *pol,
		   struct blkg_conf_ctx *ctx)
	__acquires(&bdev->bd_queue->queue_lock)
{
	struct gendisk *disk;
	/* NVMe: 대상 NVMe namespace 의 gendisk */
	struct request_queue *q;
	/* NVMe: 대상 NVMe request_queue */
	struct blkcg_gq *blkg;
	/* NVMe: 설정 대상 blkg */
	int ret;

	ret = blkg_conf_open_bdev(ctx);
	/* NVMe: bdev 열기 */
	if (ret)
	/* NVMe: bdev 열기 실패 시 즉시 반환 */
		return ret;

	disk = ctx->bdev->bd_disk;
	/* NVMe: bdev 로부터 gendisk 획득 */
	q = disk->queue;
	/* NVMe: gendisk 의 request_queue 획득; NVMe namespace queue */

	/* Prevent concurrent with blkcg_deactivate_policy() */
	mutex_lock(&q->blkcg_mutex);
	/* NVMe: blkcg_deactivate_policy() 와 동기화 */
	spin_lock_irq(&q->queue_lock);
	/* NVMe: queue_lock 획득; blkg 생성/조회 보호 */

	if (!blkcg_policy_enabled(q, pol)) {
		ret = -EOPNOTSUPP;
		goto fail_unlock;
	}

	blkg = blkg_lookup(blkcg, q);
	/* NVMe: 기존 blkg 검색 */
	if (blkg)
	/* NVMe: 기존 blkg 를 설정 대상으로 사용 */
		goto success;

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.
	 */
	while (true) {
	/* NVMe: root 에서 목표 cgroup 까지 부모 blkg 를 생성하며 남겨감 */
		struct blkcg *pos = blkcg;
		struct blkcg *parent;
		struct blkcg_gq *new_blkg;

		parent = blkcg_parent(blkcg);
		/* NVMe: pos 의 부모 cgroup */
		while (parent && !blkg_lookup(parent, q)) {
		/* NVMe: 아직 blkg 가 없는 가장 가까운 부모를 찾음 */
			pos = parent;
			parent = blkcg_parent(parent);
		}

		/* Drop locks to do new blkg allocation with GFP_KERNEL. */
		spin_unlock_irq(&q->queue_lock);
		/* NVMe: GFP_KERNEL 할당을 위해 락 해제; NVMe IO 제출이 잠시 재개될 수 있음 */

		new_blkg = blkg_alloc(pos, disk, GFP_NOIO);
		/* NVMe: GFP_NOIO 로 blkg 할당; NVMe IO 경로에서 락 해제 상태이므로 NOIO 로 재귀적 IO 방지 */
		if (unlikely(!new_blkg)) {
		/* NVMe: blkg 할당 실패; 설정 중단 */
			ret = -ENOMEM;
			goto fail_exit;
		}

		if (radix_tree_preload(GFP_KERNEL)) {
		/* NVMe: radix tree preload 실패; 설정 중단 */
			blkg_free(new_blkg);
			ret = -ENOMEM;
			goto fail_exit;
		}

		spin_lock_irq(&q->queue_lock);
		/* NVMe: 다시 queue_lock 획득 */

		if (!blkcg_policy_enabled(q, pol)) {
		/* NVMe: 락 해제 중 정책이 비활성화되면 실패 처리 */
			blkg_free(new_blkg);
			ret = -EOPNOTSUPP;
			goto fail_preloaded;
		}

		blkg = blkg_lookup(pos, q);
		/* NVMe: 재확인; 다른 경로에서 이미 생성했을 수 있음 */
		if (blkg) {
		/* NVMe: 이미 존재하면 미리 할당한 blkg 해제 */
			blkg_free(new_blkg);
		} else {
			blkg = blkg_create(pos, disk, new_blkg);
		/* NVMe: blkg 생성 및 tree/list 에 등록 */
			if (IS_ERR(blkg)) {
				ret = PTR_ERR(blkg);
				goto fail_preloaded;
			}
		}

		radix_tree_preload_end();
		/* NVMe: radix tree preload 종료 */

		if (pos == blkcg)
			goto success;
	}
success:
	mutex_unlock(&q->blkcg_mutex);
	ctx->blkg = blkg;
	/* NVMe: 설정 대상 blkg 확정; blkg_conf_prep() 호출자가 queue_lock 해제 */
	return 0;

fail_preloaded:
	radix_tree_preload_end();
	/* NVMe: preload 상태 정리 */
fail_unlock:
	spin_unlock_irq(&q->queue_lock);
	/* NVMe: queue_lock 해제 */
fail_exit:
	mutex_unlock(&q->blkcg_mutex);
	/* NVMe: blkcg_mutex 해제 */
	/*
	 * If queue was bypassing, we should retry.  Do so after a
	 * short msleep().  It isn't strictly necessary but queue
	 * can be bypassing for some time and it's always nice to
	 * avoid busy looping.
	 */
	if (ret == -EBUSY) {
	/* NVMe: queue bypass 중이면 잠시 대기 후 재시도; NVMe reset/recovery 대기 */
		msleep(10);
	/* NVMe: 시스템 콜 재시작 */
		ret = restart_syscall();
	}
	return ret;
}
EXPORT_SYMBOL_GPL(blkg_conf_prep);

/**
 * blkg_conf_exit - clean up per-blkg config update
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Clean up after per-blkg config update. This function must be called on all
 * blkg_conf_ctx's initialized with blkg_conf_init().
 */
void blkg_conf_exit(struct blkg_conf_ctx *ctx)
	__releases(&ctx->bdev->bd_queue->queue_lock)
	__releases(&ctx->bdev->bd_queue->rq_qos_mutex)
{
	if (ctx->blkg) {
	/* NVMe: blkg_conf_prep() 에서 잡은 queue_lock 해제; NVMe IO 경로 재개 */
		spin_unlock_irq(&bdev_get_queue(ctx->bdev)->queue_lock);
		ctx->blkg = NULL;
	}

	if (ctx->bdev) {
	/* NVMe: QoS lock 해제 */
		mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);
		blkdev_put_no_open(ctx->bdev);
	/* NVMe: bdev 참조 반낑; NVMe namespace bdev */
		ctx->body = NULL;
		ctx->bdev = NULL;
	}
}
EXPORT_SYMBOL_GPL(blkg_conf_exit);

/*
 * Similar to blkg_conf_exit, but also unfreezes the queue. Should be used
 * when blkg_conf_open_bdev_frozen is used to open the bdev.
 */
void blkg_conf_exit_frozen(struct blkg_conf_ctx *ctx, unsigned long memflags)
{
	if (ctx->bdev) {
	/* NVMe: 대상 NVMe request_queue */
		struct request_queue *q = ctx->bdev->bd_queue;

		blkg_conf_exit(ctx);
		blk_mq_unfreeze_queue(q, memflags);
	/* NVMe: queue freeze 해제; NVMe IO 제출/완료 재개 */
	}
}

static void blkg_iostat_add(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* NVMe: read/write/discard 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* NVMe: 세 IO 유형별로 합산 */
		dst->bytes[i] += src->bytes[i];
		/* NVMe: i 유형 바이트 누적 */
		dst->ios[i] += src->ios[i];
		/* NVMe: i 유형 IO 횟수 누적 */
	}
}

static void blkg_iostat_sub(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* NVMe: read/write/discard 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* NVMe: 세 IO 유형별로 차감 */
		dst->bytes[i] -= src->bytes[i];
		/* NVMe: i 유형 바이트 차감 */
		dst->ios[i] -= src->ios[i];
		/* NVMe: i 유형 IO 횟수 차감 */
	}
}

/*
 * blkcg_iostat_update - per-cpu 통계 delta 를 global blkg 통계에 반영
 *
 * 호출 경로: __blkcg_rstat_flush() -> blkcg_iostat_update()
 * NVMe 연결점: NVMe CQ 완료 등으로 이미 per-cpu 에 누적된 read/write/discard
 *   바이트/IO 수를 blkg->iostat.cur 로 합산한다. 상위 cgroup 으로의 전파는
 *   __blkcg_rstat_flush() 에서 수행한다.
 */

static void blkcg_iostat_update(struct blkcg_gq *blkg, struct blkg_iostat *cur,
				struct blkg_iostat *last)
{
	struct blkg_iostat delta;
	/* NVMe: per-cpu 와 last 사이의 delta */
	unsigned long flags;

	/* propagate percpu delta to global */
	flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
	/* NVMe: global 통계 seqlock 진입; NVMe read/write/discard 누적값 보호 */
	blkg_iostat_set(&delta, cur);
	/* NVMe: 현재 per-cpu 값을 delta 로 복사 */
	blkg_iostat_sub(&delta, last);
	/* NVMe: last 를 뺌으로써 실제 delta 산출 */
	blkg_iostat_add(&blkg->iostat.cur, &delta);
	/* NVMe: global cur 에 delta 누적; NVMe namespace 별 cgroup 통계 업데이트 */
	blkg_iostat_add(last, &delta);
	/* NVMe: last 를 현재값으로 갱신; 다음 delta 계산 기준 */
	u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
	/* NVMe: seqlock 해제; NVMe 통계 readers 에게 일관된 값 공개 */
}

/*
 * 주요 구조체와 NVMe 동작 연관성
 *
 * struct blkcg_gq (blkg): request_queue 와 blkcg 의 1:1 연결체. NVMe SSD 에서
 *   하나의 namespace 는 하나의 request_queue(q)를 가지며, 여러 cgroup 의 IO
 *   를 이 q 위에서 분류할 때 blkg 가 사용된다. pd[] 는 throtl, BFQ, ioprio 등
 *   정책별 private data 를 담아 nvme_queue_rq() 호출 시 큐 선택/제한/우선순위
 *   결정에 반영된다. iostat_cpu 는 NVMe 완료(CQ)에서 blk_cgroup_bio_start()로
 *   집계되는 per-cpu 통계이며, use_delay/delay_nsec/delay_start 는 cgroup 단위
 *   IO 지연 누적으로 NVMe queue depth 완화를 유도한다.
 *
 * struct blkcg: cgroup 하위 시스템 상태(css)와 함께 해당 cgroup 의 모든 blkg
 *   들을 blkg_tree/blkg_list 로 관리한다. NVMe 장치가 다수 namespace 를 가지면
 *   하나의 blkcg 는 namespace 개수만큼 blkg 를 가진다. lhead 는 per-cpu 통계
 *   갱신을 lazy flush 하기 위한 lockless list 의 헤드이다.
 *
 * struct blkg_iostat_set: read/write/discard 바이트/IO 수를 per-cpu(cur)와
 *   전역(blkg->iostat.cur) 두 벌로 유지한다. NVMe 명령어(OPC) 중 read, write,
 *   discard 를 구분해 통계를 집계하며, CID 단위로 SQ 에 기록된 후 CQ 완료
 *   시점에 누적된다(추정).
 */

/*
 * __blkcg_rstat_flush - lockless list 에 대기 중인 per-cpu 통계를 flush
 *
 * 호출 경로: cgroup rstat flush -> blkcg_rstat_flush() -> __blkcg_rstat_flush()
 * NVMe 연결점: NVMe SSD 가 멀티 코어에서 동시에 IO 완료(CQ)를 처리하면
 *   per-cpu blkg_iostat_set 의 갱신이 lockless list 에 쌓인다. 이 함수는 해당
 *   리스트를 순회하여 delta 를 blkg->iostat.cur 로 반영하고, 부모 cgroup 까지
 *   전파한다. smp_mb() 와 lqueued 플래그로 reordering 을 방지한다.
 */

static void __blkcg_rstat_flush(struct blkcg *blkcg, int cpu)
{
	struct llist_head *lhead = per_cpu_ptr(blkcg->lhead, cpu);
	/* NVMe: 대상 CPU 의 lockless list 헤드 */
	struct llist_node *lnode;
	/* NVMe: lockless list 의 첫 노드 */
	struct blkg_iostat_set *bisc, *next_bisc;
	/* NVMe: 순회 중인 per-cpu 통계 노드와 다음 노드 */
	unsigned long flags;

	rcu_read_lock();
	/* NVMe: blkg 객체 및 계층 포인터 접근을 RCU 로 보호 */

	lnode = llist_del_all(lhead);
	/* NVMe: 해당 CPU 의 lockless list 전체를 분리; NVMe 통계 업데이트 노드들을 한꺼번에 가져옴 */
	if (!lnode)
	/* NVMe: flush 할 통계 노드가 없음 */
		goto out;

	/*
	 * For covering concurrent parent blkg update from blkg_release().
	 *
	 * When flushing from cgroup, the subsystem rstat lock is always held,
	 * so this lock won't cause contention most of time.
	 */
	raw_spin_lock_irqsave(&blkg_stat_lock, flags);
	/* NVMe: 부모 blkg update 와의 경쟁 보호; NVMe 통계 상위 전파 직렬화 */

	/*
	 * Iterate only the iostat_cpu's queued in the lockless list.
	 */
	llist_for_each_entry_safe(bisc, next_bisc, lnode, lnode) {
	/* NVMe: lockless list 의 per-cpu 통계 노드를 순회; NVMe CQ 완료별 누적 처리 */
		struct blkcg_gq *blkg = bisc->blkg;
		/* NVMe: 통계가 속한 blkg; NVMe namespace 와 cgroup 의 연결체 */
		struct blkcg_gq *parent = blkg->parent;
		/* NVMe: 통계를 전파할 부모 blkg */
		struct blkg_iostat cur;
		/* NVMe: per-cpu 통계 스냅샷 */
		unsigned int seq;
		/* NVMe: u64_stats_seqlock 의 sequence 번호 */

		/*
		 * Order assignment of `next_bisc` from `bisc->lnode.next` in
		 * llist_for_each_entry_safe and clearing `bisc->lqueued` for
		 * avoiding to assign `next_bisc` with new next pointer added
		 * in blk_cgroup_bio_start() in case of re-ordering.
		 *
		 * The pair barrier is implied in llist_add() in blk_cgroup_bio_start().
		 */
		smp_mb();
		/* NVMe: llist_for_each_entry_safe 의 next 포인터 로드와 lqueued 클리어 사이의 reordering 방지; NVMe 통계 노드 안전성 */

		WRITE_ONCE(bisc->lqueued, false);
		/* NVMe: 배리어와 함께 list 등록 상태 클리어; 이후 blk_cgroup_bio_start() 에서 재등록 가능 */
		if (bisc == &blkg->iostat)
		/* NVMe: global 통계 노드는 부모로만 전파; per-cpu 통계는 먼저 global 에 합산 */
			goto propagate_up; /* propagate up to parent only */

		/* fetch the current per-cpu values */
		do {
		/* NVMe: u64_stats_seqlock 시작; 32bit 에서 NVMe 통계 reader/writer race 회피 */
			seq = u64_stats_fetch_begin(&bisc->sync);
		/* NVMe: per-cpu 통계 스냅샷 복사 */
			blkg_iostat_set(&cur, &bisc->cur);
		} while (u64_stats_fetch_retry(&bisc->sync, seq));
		/* NVMe: seqlock 갱신 시 재시도; NVMe 통계 일관성 확보 */

		blkcg_iostat_update(blkg, &cur, &bisc->last);
		/* NVMe: per-cpu delta 를 global blkg 통계에 반영 */

propagate_up:
		/* propagate global delta to parent (unless that's root) */
		if (parent && parent->parent) {
		/* NVMe: root 의 직계 자식이 아니면 부모에게 통계 전파; cgroup 계층별 NVMe IO 집계 */
			blkcg_iostat_update(parent, &blkg->iostat.cur,
			/* NVMe: 부모 blkg 의 global 통계에 delta 누적 */
					    &blkg->iostat.last);
			/*
			 * Queue parent->iostat to its blkcg's lockless
			 * list to propagate up to the grandparent if the
			 * iostat hasn't been queued yet.
			 */
			if (!parent->iostat.lqueued) {
			/* NVMe: 부모 통계 노드가 아직 list 에 없으면 등록; 상위 cgroup 으로 재귀 전파 준비 */
				struct llist_head *plhead;

				plhead = per_cpu_ptr(parent->blkcg->lhead, cpu);
				/* NVMe: 부모 cgroup 의 동일 CPU lockless list 헤드 */
				llist_add(&parent->iostat.lnode, plhead);
				/* NVMe: 부모 통계 노드를 lockless list 에 추가; 이후 상위 flush 에 의해 처리 */
				parent->iostat.lqueued = true;
				/* NVMe: 부모 노드 list 등록 상태 표시 */
			}
		}
	}
	raw_spin_unlock_irqrestore(&blkg_stat_lock, flags);
	/* NVMe: blkg 통계 전파 lock 해제 */
out:
	rcu_read_unlock();
	/* NVMe: RCU read-side 종료 */
}

/*
 * blkcg_rstat_flush - cgroup rstat 콜백, root 가 아니면 flush 수행
 *
 * 호출 경로: cgroup rstat framework -> blkcg_rstat_flush()
 * NVMe 연결점: root cgroup 은 시스템 전체 disk_stats 를 사용하고, 그 외
 *   cgroup 은 NVMe queue 별 blkg 통계를 flush 한다.
 */

static void blkcg_rstat_flush(struct cgroup_subsys_state *css, int cpu)
{
	/* Root-level stats are sourced from system-wide IO stats */
	if (cgroup_parent(css->cgroup))
		__blkcg_rstat_flush(css_to_blkcg(css), cpu);
		/* NVMe: 특정 cgroup 의 CPU 별 NVMe 통계를 global 로 반영 */
}

/*
 * We source root cgroup stats from the system-wide stats to avoid
 * tracking the same information twice and incurring overhead when no
 * cgroups are defined. For that reason, css_rstat_flush in
 * blkcg_print_stat does not actually fill out the iostat in the root
 * cgroup's blkcg_gq.
 *
 * However, we would like to re-use the printing code between the root and
 * non-root cgroups to the extent possible. For that reason, we simulate
 * flushing the root cgroup's stats by explicitly filling in the iostat
 * with disk level statistics.
 */
/*
 * blkcg_fill_root_iostats - root cgroup 통계를 시스템 전체 disk_stats 로 채움
 *
 * 호출 경로: blkcg_print_stat() -> blkcg_fill_root_iostats()
 * NVMe 연결점: root cgroup 은 모든 NVMe namespace/장치의 disk_stats 를
 *   집계해 read/write/discard 바이트/IO 수를 시뮬레이션한다. sector 단위를
 *   << 9 로 바이트로 변환한다.
 */

static void blkcg_fill_root_iostats(void)
{
	struct class_dev_iter iter;
	/* NVMe: block_class 장치 순회자 */
	struct device *dev;
	/* NVMe: 순회 중인 block 장치 */

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
	/* NVMe: 시스템의 모든 block 장치(NVMe namespace 포함)를 순회 */
		struct block_device *bdev = dev_to_bdev(dev);
		struct blkcg_gq *blkg = bdev->bd_disk->queue->root_blkg;
		/* NVMe: 장치에 해당하는 block_device */
		struct blkg_iostat tmp;
		/* NVMe: 해당 NVMe namespace 의 root blkg */
		int cpu;
		/* NVMe: disk_stats 누적 임시 버퍼 */
		unsigned long flags;
		/* NVMe: per-cpu disk_stats 순회 */

		/* NVMe: u64_stats_update irqsave 플래그 */
		memset(&tmp, 0, sizeof(tmp));
		/* NVMe: 누적 버퍼 초기화 */
		for_each_possible_cpu(cpu) {
		/* NVMe: 모든 CPU 의 disk_stats 를 합산; NVMe 멀티 코어 완료 통계 집계 */
			struct disk_stats *cpu_dkstats;

			cpu_dkstats = per_cpu_ptr(bdev->bd_stats, cpu);
			/* NVMe: CPU 별 disk_stats 획득 */
			tmp.ios[BLKG_IOSTAT_READ] +=
			/* NVMe: read IO 횟수 누적; NVMe read opcode 와 대응 */
				cpu_dkstats->ios[STAT_READ];
			tmp.ios[BLKG_IOSTAT_WRITE] +=
			/* NVMe: write IO 횟수 누적; NVMe write opcode 와 대응 */
				cpu_dkstats->ios[STAT_WRITE];
			tmp.ios[BLKG_IOSTAT_DISCARD] +=
			/* NVMe: discard IO 횟수 누적; NVMe DSM/discard 와 대응 */
				cpu_dkstats->ios[STAT_DISCARD];
			// convert sectors to bytes
			tmp.bytes[BLKG_IOSTAT_READ] +=
			/* NVMe: sector(512B) 를 byte 로 변환해 read 바이트 누적 */
			/* NVMe: sector(512B) 를 byte 로 변환해 누적 */
				cpu_dkstats->sectors[STAT_READ] << 9;
			/* NVMe: write 바이트 누적; NVMe PRP/SGL 로 전송된 총량(추정) */
			tmp.bytes[BLKG_IOSTAT_WRITE] +=
				cpu_dkstats->sectors[STAT_WRITE] << 9;
			/* NVMe: discard 바이트 누적; NVMe DSM range 와 대응 */
			tmp.bytes[BLKG_IOSTAT_DISCARD] +=
				cpu_dkstats->sectors[STAT_DISCARD] << 9;
		}

		flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
		/* NVMe: root blkg global 통계 seqlock 진입 */
		blkg_iostat_set(&blkg->iostat.cur, &tmp);
		/* NVMe: 집계된 disk_stats 를 root blkg 통계로 복사 */
		u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
		/* NVMe: seqlock 해제 */
	}
	class_dev_iter_exit(&iter);
	/* NVMe: 장치 순회 종료 */
}

static void blkcg_print_one_stat(struct blkcg_gq *blkg, struct seq_file *s)
{
	struct blkg_iostat_set *bis = &blkg->iostat;
	/* NVMe: 출력할 blkg 의 global 통계 세트 */
	u64 rbytes, wbytes, rios, wios, dbytes, dios;
	/* NVMe: read/write/discard 의 bytes/ios 스냅샷 */
	const char *dname;
	/* NVMe: NVMe 장치 이름 */
	unsigned seq;
	/* NVMe: u64_stats_seqlock sequence */
	int i;

	if (!blkg->online)
	/* NVMe: offline blkg 는 통계 미출력; 제거 중인 NVMe cgroup */
		return;

	dname = blkg_dev_name(blkg);
	/* NVMe: 장치명 획득 */
	if (!dname)
	/* NVMe: 장치명 없으면 출력 불가 */
		return;

	seq_printf(s, "%s ", dname);

	do {
	/* NVMe: u64_stats_seqlock 시작; NVMe 통계 reader/writer race 회피 */
		seq = u64_stats_fetch_begin(&bis->sync);
		/* NVMe: read/write/discard 바이트 스냅샷 */

		rbytes = bis->cur.bytes[BLKG_IOSTAT_READ];
		wbytes = bis->cur.bytes[BLKG_IOSTAT_WRITE];
		dbytes = bis->cur.bytes[BLKG_IOSTAT_DISCARD];
		rios = bis->cur.ios[BLKG_IOSTAT_READ];
		wios = bis->cur.ios[BLKG_IOSTAT_WRITE];
		dios = bis->cur.ios[BLKG_IOSTAT_DISCARD];
	} while (u64_stats_fetch_retry(&bis->sync, seq));
	/* NVMe: seqlock 갱신 시 재시도; NVMe 통계 일관성 확보 */

	if (rbytes || wbytes || rios || wios) {
	/* NVMe: 통계가 0 이 아닐 때만 출력; NVMe IO 가 실제 발생한 장치 */
		seq_printf(s, "rbytes=%llu wbytes=%llu rios=%llu wios=%llu dbytes=%llu dios=%llu",
			rbytes, wbytes, rios, wios,
			dbytes, dios);
	}

	if (blkcg_debug_stats && atomic_read(&blkg->use_delay)) {
	/* NVMe: debug 모드에서 use_delay/delay_nsec 출력; NVMe queue 지연/스로틀 상태 */
		seq_printf(s, " use_delay=%d delay_nsec=%llu",
			atomic_read(&blkg->use_delay),
			atomic64_read(&blkg->delay_nsec));
	}

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* NVMe: 등록된 정책별 추가 통계 출력; throtl/BFQ/ioprio NVMe 상태 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (!blkg->pd[i] || !pol->pd_stat_fn)
		/* NVMe: pd 없거나 stat 콜백 없으면 스킵 */
			continue;

		pol->pd_stat_fn(blkg->pd[i], s);
	}

	seq_puts(s, "\n");
}

/*
 * blkcg_print_stat - cgroup 의 blkcg.stat 파일 출력
 *
 * 호출 경로: cgroup 파일 read -> blkcg_print_stat()
 * NVMe 연결점: NVMe namespace 별 blkg 의 read/write/discard 바이트/IO,
 *   use_delay, delay_nsec 등을 출력한다. root cgroup 은 전체 NVMe 장치
 *   통계를 합산해 보여준다.
 */

static int blkcg_print_stat(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	/* NVMe: 출력 대상 cgroup */
	struct blkcg_gq *blkg;
	/* NVMe: 순회 중인 blkg */

	if (!seq_css(sf)->parent)
	/* NVMe: root cgroup 은 시스템 전체 disk_stats 에서 채움 */
		blkcg_fill_root_iostats();
	else
		css_rstat_flush(&blkcg->css);
		/* NVMe: non-root cgroup 은 per-cpu blkg 통계를 flush */

	rcu_read_lock();
	/* NVMe: blkg_list RCU read-side 보호 */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
	/* NVMe: cgroup 의 모든 NVMe namespace blkg 순회 */
		spin_lock_irq(&blkg->q->queue_lock);
		/* NVMe: blkg 통계 출력 중 NVMe queue 상태 변경 방지 */
		blkcg_print_one_stat(blkg, sf);
		spin_unlock_irq(&blkg->q->queue_lock);
		/* NVMe: queue_lock 해제; NVMe IO 경로 재개 */
	}
	rcu_read_unlock();
	return 0;
}

static struct cftype blkcg_files[] = {
	{
		.name = "stat",
		.seq_show = blkcg_print_stat,
	},
	{ }	/* terminate */
};

static struct cftype blkcg_legacy_files[] = {
	{
		.name = "reset_stats",
		.write_u64 = blkcg_reset_stats,
	},
	{ }	/* terminate */
};

#ifdef CONFIG_CGROUP_WRITEBACK
struct list_head *blkcg_get_cgwb_list(struct cgroup_subsys_state *css)
{
	return &css_to_blkcg(css)->cgwb_list;
}
#endif

/*
 * blkcg destruction is a three-stage process.
 *
 * 1. Destruction starts.  The blkcg_css_offline() callback is invoked
 *    which offlines writeback.  Here we tie the next stage of blkg destruction
 *    to the completion of writeback associated with the blkcg.  This lets us
 *    avoid punting potentially large amounts of outstanding writeback to root
 *    while maintaining any ongoing policies.  The next stage is triggered when
 *    the nr_cgwbs count goes to zero.
 *
 * 2. When the nr_cgwbs count goes to zero, blkcg_destroy_blkgs() is called
 *    and handles the destruction of blkgs.  Here the css reference held by
 *    the blkg is put back eventually allowing blkcg_css_free() to be called.
 *    This work may occur in cgwb_release_workfn() on the cgwb_release
 *    workqueue.  Any submitted ios that fail to get the blkg ref will be
 *    punted to the root_blkg.
 *
 * 3. Once the blkcg ref count goes to zero, blkcg_css_free() is called.
 *    This finally frees the blkcg.
 */

/**
 * blkcg_destroy_blkgs - responsible for shooting down blkgs
 * @blkcg: blkcg of interest
 *
 * blkgs should be removed while holding both q and blkcg locks.  As blkcg lock
 * is nested inside q lock, this function performs reverse double lock dancing.
 * Destroying the blkgs releases the reference held on the blkcg's css allowing
 * blkcg_css_free to eventually be called.
 *
 * This is the blkcg counterpart of ioc_release_fn().
 */
/*
 * blkcg_destroy_blkgs - blkcg 의 모든 blkg 를 제거
 *
 * 호출 경로: blkcg_unpin_online() -> blkcg_destroy_blkgs()
 * NVMe 연결점: cgroup 이 제거되면 해당 cgroup 이 NVMe request_queue 들에
 *   남긴 blkg 를 모두 정리한다. blkcg lock 과 queue lock 의 lock ordering 을
 *   맞추기 위해 역순으로 락을 잡는다.
 */

static void blkcg_destroy_blkgs(struct blkcg *blkcg)
{
	might_sleep();
	/* NVMe: cond_resched() 사용 가능 표시 */

	spin_lock_irq(&blkcg->lock);
	/* NVMe: blkcg 의 blkg_list 보호 */

	while (!hlist_empty(&blkcg->blkg_list)) {
	/* NVMe: 모든 blkg 가 제거될 때까지 반복 */
		struct blkcg_gq *blkg = hlist_entry(blkcg->blkg_list.first,
		/* NVMe: blkg_list 의 첫 번째 blkg 획득 */
						struct blkcg_gq, blkcg_node);
		struct request_queue *q = blkg->q;
		/* NVMe: blkg 가 속한 NVMe request_queue */

		if (need_resched() || !spin_trylock(&q->queue_lock)) {
		/* NVMe: 스케줄링 필요 또는 queue_lock 획득 실패 시 락 해제 후 재시도; softlockup 방지 */
			/*
			 * Given that the system can accumulate a huge number
			 * of blkgs in pathological cases, check to see if we
			 * need to rescheduling to avoid softlockup.
			 */
			spin_unlock_irq(&blkcg->lock);
			/* NVMe: 스케줄링 양보 */
			cond_resched();
			spin_lock_irq(&blkcg->lock);
			/* NVMe: blkg_list 가 변경되었을 수 있으므로 처음부터 재시도 */
			continue;
		}

		blkg_destroy(blkg);
		/* NVMe: blkg 제거 및 refcnt 종료; NVMe IO 는 root blkg 로 spill */
		spin_unlock(&q->queue_lock);
		/* NVMe: queue_lock 해제 */
	}

	spin_unlock_irq(&blkcg->lock);
	/* NVMe: blkcg lock 해제 */
}

/**
 * blkcg_pin_online - pin online state
 * @blkcg_css: blkcg of interest
 *
 * While pinned, a blkcg is kept online.  This is primarily used to
 * impedance-match blkg and cgwb lifetimes so that blkg doesn't go offline
 * while an associated cgwb is still active.
 */
void blkcg_pin_online(struct cgroup_subsys_state *blkcg_css)
{
	refcount_inc(&css_to_blkcg(blkcg_css)->online_pin);
	/* NVMe: online_pin 증가; cgroup 이 NVMe blkg 제거 지연 */
}

/**
 * blkcg_unpin_online - unpin online state
 * @blkcg_css: blkcg of interest
 *
 * This is primarily used to impedance-match blkg and cgwb lifetimes so
 * that blkg doesn't go offline while an associated cgwb is still active.
 * When this count goes to zero, all active cgwbs have finished so the
 * blkcg can continue destruction by calling blkcg_destroy_blkgs().
 */
/*
 * blkcg_unpin_online - online_pin 카운트가 0이면 blkg 제거 시작
 *
 * 호출 경로: blkcg_css_offline() -> blkcg_unpin_online()
 * NVMe 연결점: cgroup 의 writeback(cgwb) 등이 모두 끝나면 NVMe 장치와의
 *   blkg 연결을 해제한다. 부모 cgroup 으로 재귀적으로 처리한다.
 */

void blkcg_unpin_online(struct cgroup_subsys_state *blkcg_css)
{
	struct blkcg *blkcg = css_to_blkcg(blkcg_css);
	/* NVMe: unpin 할 cgroup */

	do {
		struct blkcg *parent;

		if (!refcount_dec_and_test(&blkcg->online_pin))
		/* NVMe: pin 카운트가 아직 남아있으면 제거 대기 */
			break;

		parent = blkcg_parent(blkcg);
		/* NVMe: 부모 cgroup; offline 은 root 방향으로 진행 */
		blkcg_destroy_blkgs(blkcg);
		/* NVMe: 이 cgroup 의 모든 NVMe blkg 제거 */
		blkcg = parent;
	} while (blkcg);
}

/**
 * blkcg_css_offline - cgroup css_offline callback
 * @css: css of interest
 *
 * This function is called when @css is about to go away.  Here the cgwbs are
 * offlined first and only once writeback associated with the blkcg has
 * finished do we start step 2 (see above).
 */
/*
 * blkcg_css_offline - cgroup offline 콜백
 *
 * 호출 경로: cgroup offline -> blkcg_css_offline()
 * NVMe 연결점: 더 이상 태스크가 이 cgroup 에 attach/migrate 되지 않도록
 *   막고, writeback 종료 후 blkg 파괴를 시작한다. NVMe IO 는 남아있는 request
 *   들이 root blkg 로 spill 될 수 있다.
 */

static void blkcg_css_offline(struct cgroup_subsys_state *css)
{
	/* this prevents anyone from attaching or migrating to this blkcg */
	wb_blkcg_offline(css);
	/* NVMe: writeback 종료 후 blkg 파괴 단계로 진행 */

	/* put the base online pin allowing step 2 to be triggered */
	blkcg_unpin_online(css);
	/* NVMe: online_pin 을 낮춰 blkg 제거 트리거 */
}

/*
 * blkcg_css_free - blkcg 구조체 최종 해제
 *
 * 호출 경로: cgroup free -> blkcg_css_free()
 * NVMe 연결점: cpd_free_fn() 으로 per-cgroup 정책 데이터를 해제하고
 *   lhead(per-cpu lockless list)를 반납한다. 모든 NVMe queue 와의 연결이
 *   사라진 후 호출된다.
 */

static void blkcg_css_free(struct cgroup_subsys_state *css)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	/* NVMe: 해제할 cgroup */
	int i;

	mutex_lock(&blkcg_pol_mutex);
	/* NVMe: 정책 테이블과 cpd 해제 동기화 */

	list_del(&blkcg->all_blkcgs_node);
	/* NVMe: 시스템 전체 blkcg 리스트에서 제거 */

	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* NVMe: per-cgroup 정책 데이터(cpds) 해제 */
		if (blkcg->cpd[i])
		/* NVMe: cpd 가 할당되어 있으면 해제 */
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);

	mutex_unlock(&blkcg_pol_mutex);

	free_percpu(blkcg->lhead);
	/* NVMe: per-cpu lockless 통계 리스트 반낑 */
	kfree(blkcg);
	/* NVMe: blkcg 객체 반낑 */
}

static struct cgroup_subsys_state *
/*
 * blkcg_css_alloc - 새 cgroup 생성 시 blkcg 할당/초기화
 *
 * 호출 경로: cgroup create -> blkcg_css_alloc()
 * NVMe 연결점: cgroup 이 생성되면 해당 cgroup 의 cpd[] 를 할당하고
 *   init_blkcg_llists() 로 per-cpu lockless list 를 준비한다. 이후 NVMe
 *   namespace 가 추가될 때 이 blkcg 를 위한 blkg 가 생성된다.
 */

blkcg_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct blkcg *blkcg;
	/* NVMe: 할당/초기화될 blkcg */
	int i;

	mutex_lock(&blkcg_pol_mutex);
	/* NVMe: 정책 테이블 보호 */

	if (!parent_css) {
		/* NVMe: 최상위 root cgroup 은 정적 blkcg_root 사용 */
		blkcg = &blkcg_root;
	} else {
		/* NVMe: 일반 cgroup 용 blkcg 동적 할당 */
		blkcg = kzalloc_obj(*blkcg);
		/* NVMe: root 가 아닌 cgroup 에 대한 blkcg 메모리 동적 할당; NVMe cgroup 트리의 신규 노드 */
		/* NVMe: 일반 cgroup 용 blkcg 동적 할당 */
		if (!blkcg)
			goto unlock;
	}

	if (init_blkcg_llists(blkcg))
	/* NVMe: per-cpu lockless 통계 리스트 초기화 실패 시 롤백 */
	/* NVMe: per-cpu lockless 통계 리스트 초기화 */
		goto free_blkcg;

	for (i = 0; i < BLKCG_MAX_POLS ; i++) {
	/* NVMe: 등록된 정책별 cpd 할당 */
		struct blkcg_policy *pol = blkcg_policy[i];
		struct blkcg_policy_data *cpd;

		/*
		 * If the policy hasn't been attached yet, wait for it
		 * to be attached before doing anything else. Otherwise,
		 * check if the policy requires any specific per-cgroup
		 * data: if it does, allocate and initialize it.
		 */
		if (!pol || !pol->cpd_alloc_fn)
		/* NVMe: 정책 미등록 또는 cpd 필요 없음 */
			continue;

		cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		/* NVMe: per-cgroup 정책 데이터 할당; throtl/BFQ/ioprio 전역 상태 */
		if (!cpd)
		/* NVMe: cpd 할당 실패 시 롤백 */
			goto free_pd_blkcg;

		blkcg->cpd[i] = cpd;
		/* NVMe: blkcg 에 정책 데이터 연결 */
		cpd->blkcg = blkcg;
		/* NVMe: cpd 가 역참조할 blkcg 설정 */
		cpd->plid = i;
		/* NVMe: policy id 기록 */
	}

	spin_lock_init(&blkcg->lock);
	/* NVMe: blkcg lock 초기화 */
	refcount_set(&blkcg->online_pin, 1);
	/* NVMe: 초기 online_pin 설정; cgroup 온라인 상태 유지 */
	INIT_RADIX_TREE(&blkcg->blkg_tree, GFP_NOWAIT);
	/* NVMe: queue id -> blkg radix tree 초기화 */
	INIT_HLIST_HEAD(&blkcg->blkg_list);
	/* NVMe: blkg list 초기화; NVMe namespace 별 blkg 들의 RCU list */
#ifdef CONFIG_CGROUP_WRITEBACK
	INIT_LIST_HEAD(&blkcg->cgwb_list);
#endif
	list_add_tail(&blkcg->all_blkcgs_node, &all_blkcgs);
	/* NVMe: 시스템 전체 blkcg 리스트에 추가 */

	mutex_unlock(&blkcg_pol_mutex);
	return &blkcg->css;

free_pd_blkcg:
	for (i--; i >= 0; i--)
	/* NVMe: 할당 실패 시 역순으로 cpd 해제 */
		if (blkcg->cpd[i])
		/* NVMe: cpd 가 할당되어 있으면 해제 */
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);
	free_percpu(blkcg->lhead);
	/* NVMe: per-cpu lockless list 반낑 */
free_blkcg:
	if (blkcg != &blkcg_root)
	/* NVMe: 동적 할당된 blkcg 만 해제 */
		kfree(blkcg);
unlock:
	mutex_unlock(&blkcg_pol_mutex);
	return ERR_PTR(-ENOMEM);
}

/*
 * blkcg_css_online - cgroup online 콜백
 *
 * 호출 경로: cgroup online -> blkcg_css_online()
 * NVMe 연결점: 부모 cgroup 을 pin 하여 offline 이 항상 root 방향으로
 *   진행되도록 한다. NVMe IO 흐름에서 cgroup 계층의 수명을 안정적으로 만든다.
 */

static int blkcg_css_online(struct cgroup_subsys_state *css)
{
	struct blkcg *parent = blkcg_parent(css_to_blkcg(css));
	/* NVMe: 부모 cgroup */

	/*
	 * blkcg_pin_online() is used to delay blkcg offline so that blkgs
	 * don't go offline while cgwbs are still active on them.  Pin the
	 * parent so that offline always happens towards the root.
	 */
	if (parent)
	/* NVMe: 부모 online_pin 증가; NVMe cgroup 계층 수명 안정성 확보 */
		blkcg_pin_online(&parent->css);
	return 0;
}

/*
 * blkg_init_queue - request_queue 의 blkcg 관련 필드 초기화
 *
 * 호출 경로: blk_alloc_queue() -> blkg_init_queue()
 * NVMe 연결점: NVMe namespace 의 request_queue 가 생성될 때 blkg_list 와
 *   blkcg_mutex 를 초기화한다. 이후 blkcg_init_disk() 에서 root blkg 가
 *   이 queue 에 연결된다.
 */

void blkg_init_queue(struct request_queue *q)
{
	INIT_LIST_HEAD(&q->blkg_list);
	/* NVMe: 이 queue 의 blkg list 초기화 */
	mutex_init(&q->blkcg_mutex);
	/* NVMe: blkg 생성/해제와 policy deactivate 동기화 mutex 초기화 */
}

/*
 * blkcg_init_disk - 디스크별 root blkg 생성
 *
 * 호출 경로: disk setup -> blkcg_init_disk()
 * NVMe 연결점: NVMe namespace(gendisk)가 생길 때 root cgroup 에 대한 blkg
 *   를 생성해 q->root_blkg 를 설정한다. 이후 bio 의 cgroup context 는 이
 *   root_blkg 를 기준으로 트리를 탐색한다.
 */

int blkcg_init_disk(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* NVMe: root blkg 를 생성할 NVMe request_queue */
	struct blkcg_gq *new_blkg, *blkg;
	/* NVMe: 새 blkg 및 등록 결과 */
	bool preloaded;
	/* NVMe: radix tree preload 상태 */

	/*
	 * If the queue is shared across disk rebind (e.g., SCSI), the
	 * previous disk's blkcg state is cleaned up asynchronously via
	 * disk_release() -> blkcg_exit_disk(). Wait for that cleanup to
	 * finish (indicated by root_blkg becoming NULL) before setting up
	 * new blkcg state. Otherwise, we may overwrite q->root_blkg while
	 * the old one is still alive, and radix_tree_insert() in
	 * blkg_create() will fail with -EEXIST because the old entries
	 * still occupy the same queue id slot in blkcg->blkg_tree.
	 */
	wait_var_event(&q->root_blkg, !READ_ONCE(q->root_blkg));
	/* NVMe: 이전 disk 의 root_blkg 정리가 끝날 때까지 대기; NVMe namespace rebind 안전성 */
	/* NVMe: 이전 disk 의 root_blkg 정리가 끝날 때까지 대기 */

	new_blkg = blkg_alloc(&blkcg_root, disk, GFP_KERNEL);
	/* NVMe: root cgroup 용 blkg 할당 */
	if (!new_blkg)
	/* NVMe: root blkg 할당 실패 시 NVMe namespace 초기화 실패 */
		return -ENOMEM;

	preloaded = !radix_tree_preload(GFP_KERNEL);
	/* NVMe: radix tree 삽입을 위한 preload 수행 */

	/* Make sure the root blkg exists. */
	/* spin_lock_irq can serve as RCU read-side critical section. */
	spin_lock_irq(&q->queue_lock);
	/* NVMe: queue_lock 획득 */
	blkg = blkg_create(&blkcg_root, disk, new_blkg);
	/* NVMe: root blkg 생성 및 등록 */
	if (IS_ERR(blkg))
	/* NVMe: root blkg 생성 실패 */
		goto err_unlock;
	q->root_blkg = blkg;
	/* NVMe: queue 의 root blkg 설정; bio 제출의 최종 fallback */
	spin_unlock_irq(&q->queue_lock);

	if (preloaded)
	/* NVMe: radix tree preload 종료 */
		radix_tree_preload_end();

	return 0;

err_unlock:
	/* NVMe: queue_lock 해제 */
	spin_unlock_irq(&q->queue_lock);
	if (preloaded)
	/* NVMe: 실패 시에도 preload 종료 */
		radix_tree_preload_end();
	return PTR_ERR(blkg);
}

/*
 * blkcg_exit_disk - 디스크 제거 시 blkcg 자원 정리
 *
 * 호출 경로: disk_release() -> blkcg_exit_disk()
 * NVMe 연결점: NVMe namespace 가 사라지면 모든 blkg 를 제거하고 throtl
 *   자원도 정리한다. q->root_blkg 가 NULL 이 될 때까지 후속 disk rebind 는
 *   대기한다.
 */

void blkcg_exit_disk(struct gendisk *disk)
{
	blkg_destroy_all(disk);
	/* NVMe: 모든 blkg 제거; NVMe queue 의 cgroup 분류 체계 소멸 */
	blk_throtl_exit(disk);
	/* NVMe: throtl 자원 정리; NVMe queue depth throttle 상태 해제 */
}

static void blkcg_exit(struct task_struct *tsk)
{
	if (tsk->throttle_disk)
	/* NVMe: throttle_disk 참조 반낑 */
		put_disk(tsk->throttle_disk);
	tsk->throttle_disk = NULL;
	/* NVMe: 포인터 클리어 */
}

struct cgroup_subsys io_cgrp_subsys = {
	.css_alloc = blkcg_css_alloc,
	.css_online = blkcg_css_online,
	.css_offline = blkcg_css_offline,
	.css_free = blkcg_css_free,
	.css_rstat_flush = blkcg_rstat_flush,
	.dfl_cftypes = blkcg_files,
	.legacy_cftypes = blkcg_legacy_files,
	.legacy_name = "blkio",
	.exit = blkcg_exit,
#ifdef CONFIG_MEMCG
	/*
	 * This ensures that, if available, memcg is automatically enabled
	 * together on the default hierarchy so that the owner cgroup can
	 * be retrieved from writeback pages.
	 */
	.depends_on = 1 << memory_cgrp_id,
#endif
};
EXPORT_SYMBOL_GPL(io_cgrp_subsys);

/**
 * blkcg_activate_policy - activate a blkcg policy on a gendisk
 * @disk: gendisk of interest
 * @pol: blkcg policy to activate
 *
 * Activate @pol on @disk.  Requires %GFP_KERNEL context.  @disk goes through
 * bypass mode to populate its blkgs with policy_data for @pol.
 *
 * Activation happens with @disk bypassed, so nobody would be accessing blkgs
 * from IO path.  Update of each blkg is protected by both queue and blkcg
 * locks so that holding either lock and testing blkcg_policy_enabled() is
 * always enough for dereferencing policy data.
 *
 * The caller is responsible for synchronizing [de]activations and policy
 * [un]registerations.  Returns 0 on success, -errno on failure.
 */
/*
 * blkcg_activate_policy - gendisk 에 blkcg 정책 활성화
 *
 * 호출 경로: elevator/throtl 등록 -> blkcg_activate_policy()
 * NVMe 연결점: NVMe queue 에 BFQ/throtl/ioprio 같은 정책을 적용한다.
 *   queue 를 freeze(blk_mq_freeze_queue) 한 상태에서 모든 기존 blkg 에
 *   pd_alloc_fn() 으로 정책 데이터를 할당한다. NVMe IO 경로는 이후
 *   blkg->pd[plid] 를 통해 정책 상태를 참조한다.
 */

int blkcg_activate_policy(struct gendisk *disk, const struct blkcg_policy *pol)
{
	struct request_queue *q = disk->queue;
	/* NVMe: 정책을 활성화할 NVMe request_queue */
	struct blkg_policy_data *pd_prealloc = NULL;
	/* NVMe: GFP_KERNEL 로 미리 할당한 pd; 락 해제 후 재시도용 */
	struct blkcg_gq *blkg, *pinned_blkg = NULL;
	/* NVMe: 순회 중인 blkg 및 미리 할당된 blkg */
	unsigned int memflags;
	/* NVMe: blk_mq_freeze_queue 의 memalloc 상태 저장 */
	int ret;

	if (blkcg_policy_enabled(q, pol))
	/* NVMe: 이미 활성화된 정책은 중복 활성화하지 않음 */
		return 0;

	/*
	 * Policy is allowed to be registered without pd_alloc_fn/pd_free_fn,
	 * for example, ioprio. Such policy will work on blkcg level, not disk
	 * level, and don't need to be activated.
	 */
	if (WARN_ON_ONCE(!pol->pd_alloc_fn || !pol->pd_free_fn))
	/* NVMe: alloc/free 함수 쌍이 맞아야 메모리 누수/부패 방지 */
		return -EINVAL;

	if (queue_is_mq(q))
	/* NVMe: NVMe queue freeze 해제 */
	/* NVMe: NVMe 는 일반적으로 blk-mq 이므로 queue freeze */
		memflags = blk_mq_freeze_queue(q);
		/* NVMe: blk-mq queue freeze; NVMe IO 제출/완료 일시 정지 */
retry:
	spin_lock_irq(&q->queue_lock);
	/* NVMe: queue_lock 획득; blkg_list 순회 보호 */

	/* blkg_list is pushed at the head, reverse walk to initialize parents first */
	list_for_each_entry_reverse(blkg, &q->blkg_list, q_node) {
	/* NVMe: 부모 blkg 부터 먼저 초기화하기 위해 역순 순회 */
		struct blkg_policy_data *pd;
		/* NVMe: 할당될 정책별 private data */

		if (blkg->pd[pol->plid])
		/* NVMe: 이미 pd 가 있으면 스킵 */
			continue;

		/* If prealloc matches, use it; otherwise try GFP_NOWAIT */
		if (blkg == pinned_blkg) {
		/* NVMe: 미리 GFP_KERNEL 로 할당해 둔 pd 사용 */
			pd = pd_prealloc;
			pd_prealloc = NULL;
		} else {
			pd = pol->pd_alloc_fn(disk, blkg->blkcg,
		/* NVMe: 락을 잡은 상태에서 빠른 pd 할당 시도 */
					      GFP_NOWAIT);
		}

		if (!pd) {
		/* NVMe: GFP_NOWAIT 실패; 락을 풀고 GFP_KERNEL 로 재시도 */
			/*
			 * GFP_NOWAIT failed.  Free the existing one and
			 * prealloc for @blkg w/ GFP_KERNEL.
			 */
			if (pinned_blkg)
			/* NVMe: 이전 pinned blkg 참조 반낑 */
				blkg_put(pinned_blkg);
			blkg_get(blkg);
			/* NVMe: 현재 blkg 참조 획득; 락 해제 후에도 유효 */
			pinned_blkg = blkg;

			/* NVMe: 락 해제 후 이 blkg 용 pd 를 재할당 */
			spin_unlock_irq(&q->queue_lock);
			/* NVMe: GFP_KERNEL 할당을 위해 queue_lock 해제 */

			if (pd_prealloc)
			/* NVMe: 이전 prealloc 해제 */
				pol->pd_free_fn(pd_prealloc);
			pd_prealloc = pol->pd_alloc_fn(disk, blkg->blkcg,
			/* NVMe: GFP_KERNEL 로 pd 재할당 */
						       GFP_KERNEL);
			if (pd_prealloc)
			/* NVMe: 성공하면 다시 queue_lock 잡고 등록 */
				goto retry;
			else
				goto enomem;
		}

		spin_lock(&blkg->blkcg->lock);
		/* NVMe: blkg 등록/online 시 blkcg lock 추가 획득 */

		pd->blkg = blkg;
		/* NVMe: pd 가 역참조할 blkg 설정 */
		pd->plid = pol->plid;
		/* NVMe: policy id 설정 */
		blkg->pd[pol->plid] = pd;
		/* NVMe: blkg 에 정책 데이터 연결; nvme_queue_rq() 에서 참조 가능 */

		if (pol->pd_init_fn)
		/* NVMe: pd 초기화 콜백 */
			pol->pd_init_fn(pd);

		if (pol->pd_online_fn)
		/* NVMe: pd online 콜백; IO 경로 활성화 */
			pol->pd_online_fn(pd);
		pd->online = true;
		/* NVMe: pd online 상태 표시 */

		spin_unlock(&blkg->blkcg->lock);
	}

	__set_bit(pol->plid, q->blkcg_pols);
	/* NVMe: 이 queue(NVMe namespace)에서 해당 cgroup 정책 활성화 표시 */
	/* NVMe: 이 queue 에서 해당 cgroup 정책 활성화 표시 */
	ret = 0;

	spin_unlock_irq(&q->queue_lock);
	/* NVMe: queue_lock 해제 */
out:
	if (queue_is_mq(q))
		blk_mq_unfreeze_queue(q, memflags);
	/* NVMe: queue freeze 해제; NVMe IO 제출/완료 재개 */
	if (pinned_blkg)
	/* NVMe: pinned blkg 참조 반낑 */
		blkg_put(pinned_blkg);
	if (pd_prealloc)
	/* NVMe: 미사용 prealloc pd 해제 */
		pol->pd_free_fn(pd_prealloc);
	return ret;

enomem:
	/* alloc failed, take down everything */
	spin_lock_irq(&q->queue_lock);
	/* NVMe: 할당 실패 시 롤백; queue_lock 재획득 */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* NVMe: 이미 추가된 pd 들을 정순으로 제거 */
		struct blkcg *blkcg = blkg->blkcg;
		struct blkg_policy_data *pd;

		spin_lock(&blkcg->lock);
		/* NVMe: blkcg lock 획득 */
		pd = blkg->pd[pol->plid];
		/* NVMe: 제거할 정책 데이터 */
		if (pd) {
			if (pd->online && pol->pd_offline_fn)
		/* NVMe: online 상태면 offline 처리 */
				pol->pd_offline_fn(pd);
			pd->online = false;
		/* NVMe: pd offline 표시 */
			pol->pd_free_fn(pd);
		/* NVMe: pd 메모리 해제 */
			blkg->pd[pol->plid] = NULL;
		/* NVMe: blkg 에서 pd 연결 제거; nvme_queue_rq() 가 참조하지 않음 */
		}
		spin_unlock(&blkcg->lock);
	}
	spin_unlock_irq(&q->queue_lock);
	ret = -ENOMEM;
	goto out;
}
EXPORT_SYMBOL_GPL(blkcg_activate_policy);

/**
 * blkcg_deactivate_policy - deactivate a blkcg policy on a gendisk
 * @disk: gendisk of interest
 * @pol: blkcg policy to deactivate
 *
 * Deactivate @pol on @disk.  Follows the same synchronization rules as
 * blkcg_activate_policy().
 */
/*
 * blkcg_deactivate_policy - gendisk 에서 blkcg 정책 비활성화
 *
 * 호출 경로: 정책 제거/queue 종료 -> blkcg_deactivate_policy()
 * NVMe 연결점: NVMe queue 에서 해당 cgroup 정책을 제거한다. q->blkcg_pols
 *   비트를 클리어하고 모든 blkg 의 pd[] 를 해제하여 nvme_queue_rq() 에서
 *   더 이상 정책을 참조하지 않게 한다.
 */

void blkcg_deactivate_policy(struct gendisk *disk,
			     const struct blkcg_policy *pol)
{
	struct request_queue *q = disk->queue;
	/* NVMe: 정책을 비활성화할 NVMe request_queue */
	struct blkcg_gq *blkg;
	/* NVMe: 순회 중인 blkg */
	unsigned int memflags;

	if (!blkcg_policy_enabled(q, pol))
	/* NVMe: 이미 비활성화된 정책은 무시 */
	/* NVMe: deactivate 전 정책 활성화 여부 재확인 */
		return;

	if (queue_is_mq(q))
	/* NVMe: NVMe queue freeze; IO 경로 정지 후 정책 제거 */
		memflags = blk_mq_freeze_queue(q);
		/* NVMe: blk-mq queue freeze; NVMe IO 제출/완료 일시 정지 */

	mutex_lock(&q->blkcg_mutex);
	/* NVMe: blkg_free_workfn 과의 동기화 */
	spin_lock_irq(&q->queue_lock);
	/* NVMe: queue_lock 획득 */

	__clear_bit(pol->plid, q->blkcg_pols);
	/* NVMe: queue 의 정책 활성화 비트 클리어 */

	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* NVMe: 모든 blkg 의 pd 해제; NVMe queue 에서 cgroup 정책 제거 */
		struct blkcg *blkcg = blkg->blkcg;

		spin_lock(&blkcg->lock);
		/* NVMe: blkcg lock 획득 */
		if (blkg->pd[pol->plid]) {
		/* NVMe: pd 가 할당되어 있으면 해제 */
			if (blkg->pd[pol->plid]->online && pol->pd_offline_fn)
		/* NVMe: online 이면 offline 처리 */
				pol->pd_offline_fn(blkg->pd[pol->plid]);
			pol->pd_free_fn(blkg->pd[pol->plid]);
		/* NVMe: pd 메모리 해제 */
			blkg->pd[pol->plid] = NULL;
		/* NVMe: blkg 에서 pd 제거; nvme_queue_rq() 정책 참조 차단 */
		}
		spin_unlock(&blkcg->lock);
	}

	spin_unlock_irq(&q->queue_lock);
	/* NVMe: queue_lock 해제 */
	mutex_unlock(&q->blkcg_mutex);
	/* NVMe: blkcg_mutex 해제 */

	if (queue_is_mq(q))
	/* NVMe: queue freeze 해제; NVMe IO 재개 */
		blk_mq_unfreeze_queue(q, memflags);
}
EXPORT_SYMBOL_GPL(blkcg_deactivate_policy);

static void blkcg_free_all_cpd(struct blkcg_policy *pol)
{
	struct blkcg *blkcg;
	/* NVMe: 순회 중인 cgroup */

	list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
	/* NVMe: 시스템 전체 blkcg 순회 */
		if (blkcg->cpd[pol->plid]) {
		/* NVMe: 해당 정책의 cpd 가 있으면 해제 */
			pol->cpd_free_fn(blkcg->cpd[pol->plid]);
			blkcg->cpd[pol->plid] = NULL;
		}
	}
}

/**
 * blkcg_policy_register - register a blkcg policy
 * @pol: blkcg policy to register
 *
 * Register @pol with blkcg core.  Might sleep and @pol may be modified on
 * successful registration.  Returns 0 on success and -errno on failure.
 */
/*
 * blkcg_policy_register - blkcg 정책 전역 등록
 *
 * 호출 경로: policy module init -> blkcg_policy_register()
 * NVMe 연결점: throtl, BFQ, ioprio 등이 등록되며, 기존 모든 blkcg 의 cpd[]
 *   를 할당하고 sysfs cgroup 파일을 추가한다. NVMe queue 들은 이후
 *   blkcg_activate_policy() 로 개별적으로 활성화해야 한다.
 */

int blkcg_policy_register(struct blkcg_policy *pol)
{
	struct blkcg *blkcg;
	/* NVMe: cpd 할당 시 순회 중인 cgroup */
	int i, ret;
	/* NVMe: 정책 슬롯 인덱스와 반환값 */

	/*
	 * Make sure cpd/pd_alloc_fn and cpd/pd_free_fn in pairs, and policy
	 * without pd_alloc_fn/pd_free_fn can't be activated.
	 */
	if ((!pol->cpd_alloc_fn ^ !pol->cpd_free_fn) ||
	/* NVMe: alloc/free 함수 쌍이 맞아야 메모리 누수/부패 방지 */
	    (!pol->pd_alloc_fn ^ !pol->pd_free_fn))
		return -EINVAL;

	mutex_lock(&blkcg_pol_register_mutex);
	/* NVMe: 정책 등록/해제 전역 직렬화 */
	mutex_lock(&blkcg_pol_mutex);
	/* NVMe: 정책 테이블 보호 */

	/* find an empty slot */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* NVMe: 빈 정책 슬롯 탐색 */
		if (!blkcg_policy[i])
			break;
	if (i >= BLKCG_MAX_POLS) {
	/* NVMe: 정책 슬롯 부족, 더 이상 NVMe queue 정책 추가 불가 */
		pr_warn("blkcg_policy_register: BLKCG_MAX_POLS too small\n");
		ret = -ENOSPC;
		goto err_unlock;
	}

	/* register @pol */
	pol->plid = i;
	/* NVMe: 정책 id 할당 */
	blkcg_policy[pol->plid] = pol;
	/* NVMe: 전역 정책 테이블에 등록 */

	/* allocate and install cpd's */
	if (pol->cpd_alloc_fn) {
	/* NVMe: 기존 모든 cgroup 에 cpd 할당 */
		list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
		/* NVMe: 모든 cgroup 에 대해 cpd 할당 */
			struct blkcg_policy_data *cpd;

			cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		/* NVMe: per-cgroup 정책 데이터 할당 */
			if (!cpd) {
		/* NVMe: cpd 할당 실패 시 롤백 */
				ret = -ENOMEM;
				goto err_free_cpds;
			}

			blkcg->cpd[pol->plid] = cpd;
		/* NVMe: cgroup 에 cpd 연결 */
			cpd->blkcg = blkcg;
		/* NVMe: cpd 가 역참조할 cgroup 설정 */
			cpd->plid = pol->plid;
		/* NVMe: policy id 설정 */
		}
	}

	mutex_unlock(&blkcg_pol_mutex);

	/* everything is in place, add intf files for the new policy */
	if (pol->dfl_cftypes == pol->legacy_cftypes) {
	/* NVMe: v2/v1 cgroup 파일이 동일하면 하나로 등록 */
		WARN_ON(cgroup_add_cftypes(&io_cgrp_subsys,
					   pol->dfl_cftypes));
	} else {
		WARN_ON(cgroup_add_dfl_cftypes(&io_cgrp_subsys,
		/* NVMe: cgroup v2 파일 추가; NVMe 설정 인터페이스 */
					       pol->dfl_cftypes));
		WARN_ON(cgroup_add_legacy_cftypes(&io_cgrp_subsys,
		/* NVMe: cgroup v1 파일 추가 */
						  pol->legacy_cftypes));
	}
	mutex_unlock(&blkcg_pol_register_mutex);
	return 0;

err_free_cpds:
	if (pol->cpd_free_fn)
	/* NVMe: 할당된 cpd 전부 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* NVMe: 정책 테이블에서 등록 취소 */
err_unlock:
	mutex_unlock(&blkcg_pol_mutex);
	mutex_unlock(&blkcg_pol_register_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(blkcg_policy_register);

/**
 * blkcg_policy_unregister - unregister a blkcg policy
 * @pol: blkcg policy to unregister
 *
 * Undo blkcg_policy_register(@pol).  Might sleep.
 */
/*
 * blkcg_policy_unregister - blkcg 정책 전역 등록 해제
 *
 * 호출 경로: policy module exit -> blkcg_policy_unregister()
 * NVMe 연결점: NVMe 장치에 적용되던 cgroup 정책 인터페이스를 제거한다.
 *   blkcg_policy[] 슬롯을 NULL 로 만들고 cpd 를 해제한다.
 */

void blkcg_policy_unregister(struct blkcg_policy *pol)
{
	mutex_lock(&blkcg_pol_register_mutex);
	/* NVMe: 정책 등록/해제 직렬화 */

	if (WARN_ON(blkcg_policy[pol->plid] != pol))
	/* NVMe: 슬롯 불일치 시 방어 */
		goto out_unlock;

	/* kill the intf files first */
	if (pol->dfl_cftypes)
	/* NVMe: cgroup v2 파일 제거 */
		cgroup_rm_cftypes(pol->dfl_cftypes);
	if (pol->legacy_cftypes)
	/* NVMe: cgroup v1 파일 제거 */
		cgroup_rm_cftypes(pol->legacy_cftypes);

	/* remove cpds and unregister */
	mutex_lock(&blkcg_pol_mutex);
	/* NVMe: 정책 테이블 보호 */

	if (pol->cpd_free_fn)
	/* NVMe: 모든 cgroup 의 cpd 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* NVMe: 전역 정책 테이블에서 제거 */

	mutex_unlock(&blkcg_pol_mutex);
out_unlock:
	mutex_unlock(&blkcg_pol_register_mutex);
}
EXPORT_SYMBOL_GPL(blkcg_policy_unregister);

/*
 * Scale the accumulated delay based on how long it has been since we updated
 * the delay.  We only call this when we are adding delay, in case it's been a
 * while since we added delay, and when we are checking to see if we need to
 * delay a task, to account for any delays that may have occurred.
 */
/*
 * blkcg_scale_delay - 누적된 cgroup IO 지연을 시간에 따라 감소
 *
 * 호출 경로: blkcg_add_delay() -> blkcg_scale_delay()
 *            blkcg_maybe_throttle_blkg() -> blkcg_scale_delay()
 * NVMe 연결점: NVMe SSD 의 IO 완료 지연이나 throttle 로 인해 쌓인
 *   delay_nsec 를 1초 단위로 decay 시킨다. queue depth 가 포화 상태일 때
 *   cgroup 별 제출 속도를 조절하는 데 사용된다.
 */

static void blkcg_scale_delay(struct blkcg_gq *blkg, u64 now)
{
	u64 old = atomic64_read(&blkg->delay_start);
	/* NVMe: 현재 delay_start 스냅샷; atomic read */

	/* negative use_delay means no scaling, see blkcg_set_delay() */
	if (atomic_read(&blkg->use_delay) < 0)
	/* NVMe: blkcg_set_delay() 모드에서는 decay 하지 않음 */
		return;

	/*
	 * We only want to scale down every second.  The idea here is that we
	 * want to delay people for min(delay_nsec, NSEC_PER_SEC) in a certain
	 * time window.  We only want to throttle tasks for recent delay that
	 * has occurred, in 1 second time windows since that's the maximum
	 * things can be throttled.  We save the current delay window in
	 * blkg->last_delay so we know what amount is still left to be charged
	 * to the blkg from this point onward.  blkg->last_use keeps track of
	 * the use_delay counter.  The idea is if we're unthrottling the blkg we
	 * are ok with whatever is happening now, and we can take away more of
	 * the accumulated delay as we've already throttled enough that
	 * everybody is happy with their IO latencies.
	 */
	if (time_before64(old + NSEC_PER_SEC, now) &&
	/* NVMe: 1초 이상 지난 지연 예산을 decay; atomic CAS 로 경쟁하는 CPU 중 하나만 갱신 */
	/* NVMe: 1초 이상 지난 지연 예산을 decay */
	    atomic64_try_cmpxchg(&blkg->delay_start, &old, now)) {
		u64 cur = atomic64_read(&blkg->delay_nsec);
		/* NVMe: 현재 누적 지연량; atomic read */
		u64 sub = min_t(u64, blkg->last_delay, now - old);
		/* NVMe: 감소시킬 지연량 산출 */
		int cur_use = atomic_read(&blkg->use_delay);
		/* NVMe: 현재 use_delay 카운터; throttle 활성 여부 */

		/*
		 * We've been unthrottled, subtract a larger chunk of our
		 * accumulated delay.
		 */
		if (cur_use < blkg->last_use)
		/* NVMe: throttle 이 해제되면 더 많은 지연 예산을 감소 */
			sub = max_t(u64, sub, blkg->last_delay >> 1);

		/*
		 * This shouldn't happen, but handle it anyway.  Our delay_nsec
		 * should only ever be growing except here where we subtract out
		 * min(last_delay, 1 second), but lord knows bugs happen and I'd
		 * rather not end up with negative numbers.
		 */
		if (unlikely(cur < sub)) {
		/* NVMe: 음수 방지; 지연 예산 0 으로 클리어 */
			atomic64_set(&blkg->delay_nsec, 0);
			blkg->last_delay = 0;
		} else {
			atomic64_sub(sub, &blkg->delay_nsec);
		/* NVMe: 지연 예산 감소; atomic 연산 */
			blkg->last_delay = cur - sub;
		}
		blkg->last_use = cur_use;
		/* NVMe: last_use 갱신; 다음 decay 계산 기준 */
	}
}

/*
 * This is called when we want to actually walk up the hierarchy and check to
 * see if we need to throttle, and then actually throttle if there is some
 * accumulated delay.  This should only be called upon return to user space so
 * we're not holding some lock that would induce a priority inversion.
 */
/*
 * blkcg_maybe_throttle_blkg - blkg 계층을 거슬러 올라가며 태스크 throttle
 *
 * 호출 경로: blkcg_maybe_throttle_current() -> blkcg_maybe_throttle_blkg()
 * NVMe 연결점: NVMe queue 의 IO 지연이 cgroup limit 을 초과하면 사용자 공간
 *   복귀 직전 태스크를 수면시켜 NVMe 로의 새로운 IO 제출을 줄인다.
 *   clamp 시 최대 250ms 로 제한한다.
 */

static void blkcg_maybe_throttle_blkg(struct blkcg_gq *blkg, bool use_memdelay)
{
	unsigned long pflags;
	/* NVMe: PSI memstall 플래그 */
	bool clamp;
	/* NVMe: delay 를 250ms 로 clamp 할지 여부 */
	u64 now = blk_time_get_ns();
	/* NVMe: 현재 시간; 지연 예산 정규화 기준 */
	u64 exp;
	/* NVMe: 깨어날 시간 */
	u64 delay_nsec = 0;
	/* NVMe: 계층에서 발견한 최대 지연량 */
	int tok;
	/* NVMe: io_schedule_prepare 토큰 */

	while (blkg->parent) {
	/* NVMe: 현재 blkg 에서 root 까지 계층을 따라 최대 지연 탐색 */
		int use_delay = atomic_read(&blkg->use_delay);
		/* NVMe: 이 cgroup/blkg 의 지연 예산 활성화 상태; atomic read */

		if (use_delay) {
		/* NVMe: 이 cgroup/blkg 에 지연 예산이 쌓여 있으면 throttle 검사 */
			u64 this_delay;

			blkcg_scale_delay(blkg, now);
			/* NVMe: 지연 예산을 현재 시간 기준으로 정규화 */
			this_delay = atomic64_read(&blkg->delay_nsec);
			/* NVMe: 정규화된 지연 예산; atomic read */
			if (this_delay > delay_nsec) {
			/* NVMe: 최대 지연 갱신; 양수 use_delay 이면 clamp */
				delay_nsec = this_delay;
				clamp = use_delay > 0;
			}
		}
		blkg = blkg->parent;
		/* NVMe: 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
	}

	if (!delay_nsec)
	/* NVMe: 지연 예산이 없으면 throttle 없음 */
		return;

	/*
	 * Let's not sleep for all eternity if we've amassed a huge delay.
	 * Swapping or metadata IO can accumulate 10's of seconds worth of
	 * delay, and we want userspace to be able to do _something_ so cap the
	 * delays at 0.25s. If there's 10's of seconds worth of delay then the
	 * tasks will be delayed for 0.25 second for every syscall. If
	 * blkcg_set_delay() was used as indicated by negative use_delay, the
	 * caller is responsible for regulating the range.
	 */
	if (clamp)
	/* NVMe: 지나친 지연을 방지하기 위해 최대 250ms 로 clamp */
		delay_nsec = min_t(u64, delay_nsec, 250 * NSEC_PER_MSEC);

	if (use_memdelay)
	/* NVMe: PSI memory delay 기록 */
		psi_memstall_enter(&pflags);

	exp = ktime_add_ns(now, delay_nsec);
	/* NVMe: 깨어날 절대 시간 */
	tok = io_schedule_prepare();
	/* NVMe: IO 스케줄링 준비 */
	do {
		__set_current_state(TASK_KILLABLE);
		/* NVMe: kill 가능한 수면 상태로 전환; NVMe IO 대기 중 시그널 처리 */
		if (!schedule_hrtimeout(&exp, HRTIMER_MODE_ABS))
		/* NVMe: 지정 시간까지 수면; 시간 만료 시 깨어남 */
			break;
	} while (!fatal_signal_pending(current));
	io_schedule_finish(tok);
	/* NVMe: IO 스케줄링 종료 처리 */

	if (use_memdelay)
		psi_memstall_leave(&pflags);
}

/**
 * blkcg_maybe_throttle_current - throttle the current task if it has been marked
 *
 * This is only called if we've been marked with set_notify_resume().  Obviously
 * we can be set_notify_resume() for reasons other than blkcg throttling, so we
 * check to see if current->throttle_disk is set and if not this doesn't do
 * anything.  This should only ever be called by the resume code, it's not meant
 * to be called by people willy-nilly as it will actually do the work to
 * throttle the task if it is setup for throttling.
 */
/*
 * blkcg_maybe_throttle_current - 현재 태스크의 blkcg throttle 조건 확인/수행
 *
 * 호출 경로: resume 코드 -> blkcg_maybe_throttle_current()
 * NVMe 연결점: current->throttle_disk 에 저장된 NVMe disk 를 찾아 해당
 *   cgroup 의 blkg 를 lookup 한 후 지연을 적용한다. syscall 당 한 번만
 *   throttle 한다.
 */

void blkcg_maybe_throttle_current(void)
{
	struct gendisk *disk = current->throttle_disk;
	/* NVMe: throttle 할 NVMe disk */
	struct blkcg *blkcg;
	/* NVMe: 현재 태스크의 cgroup */
	struct blkcg_gq *blkg;
	/* NVMe: throttle 대상 blkg */
	bool use_memdelay = current->use_memdelay;
	/* NVMe: PSI memdelay 사용 여부 */

	if (!disk)
	/* NVMe: throttle 예약이 없으면 무시 */
		return;

	current->throttle_disk = NULL;
	/* NVMe: throttle_disk 클리어; 한 syscall 당 한 번만 throttle */
	current->use_memdelay = false;
	/* NVMe: memdelay 플래그 클리어 */

	rcu_read_lock();
	/* NVMe: blkg_lookup 및 css 접근을 RCU 로 보호 */
	blkcg = css_to_blkcg(blkcg_css());
	/* NVMe: 현재 태스크의 blkcg 획득 */
	if (!blkcg)
	/* NVMe: blkcg 가 없으면 throttle 불가 */
		goto out;
	blkg = blkg_lookup(blkcg, disk->queue);
	/* NVMe: NVMe disk 의 blkg 검색 */
	if (!blkg)
	/* NVMe: blkg 가 없으면 throttle 불가 */
		goto out;
	if (!blkg_tryget(blkg))
	/* NVMe: blkg 참조 획득 실패 시 throttle 불가; 제거 중일 수 있음 */
		goto out;
	rcu_read_unlock();
	/* NVMe: blkg_tryget 성공 후 RCU 종료; blkg 참조로 보호 */

	blkcg_maybe_throttle_blkg(blkg, use_memdelay);
	blkg_put(blkg);
	/* NVMe: throttle 완료 후 blkg 참조 반낑 */
	put_disk(disk);
	return;
out:
	rcu_read_unlock();
	put_disk(disk);
}

/**
 * blkcg_schedule_throttle - this task needs to check for throttling
 * @disk: disk to throttle
 * @use_memdelay: do we charge this to memory delay for PSI
 *
 * This is called by the IO controller when we know there's delay accumulated
 * for the blkg for this task.  We do not pass the blkg because there are places
 * we call this that may not have that information, the swapping code for
 * instance will only have a block_device at that point.  This set's the
 * notify_resume for the task to check and see if it requires throttling before
 * returning to user space.
 *
 * We will only schedule once per syscall.  You can call this over and over
 * again and it will only do the check once upon return to user space, and only
 * throttle once.  If the task needs to be throttled again it'll need to be
 * re-set at the next time we see the task.
 */
/*
 * blkcg_schedule_throttle - 현재 태스크가 user space 복귀 시 throttle 검사
 *
 * 호출 경로: throtl/bfq 등 -> blkcg_schedule_throttle()
 * NVMe 연결점: NVMe disk 의 IO 지연이 발생했음을 알리고, 태스크가 user
 *   space 로 돌아갈 때 blkcg_maybe_throttle_current() 가 동작하도록
 *   set_notify_resume() 을 설정한다.
 */

void blkcg_schedule_throttle(struct gendisk *disk, bool use_memdelay)
{
	if (unlikely(current->flags & PF_KTHREAD))
	/* NVMe: kthread 는 user space 복귀가 없으므로 throttle 예약 안 함 */
		return;

	if (current->throttle_disk != disk) {
	/* NVMe: 다른 disk 를 가리키고 있거나 처음 설정 */
		if (test_bit(GD_DEAD, &disk->state))
		/* NVMe: 죽은 disk 이면 throttle 예약 안 함; NVMe namespace 제거 중 */
			return;
		get_device(disk_to_dev(disk));
		/* NVMe: disk 장치 참조 획득 */

		if (current->throttle_disk)
		/* NVMe: 이전 disk 참조 반낑 */
			put_disk(current->throttle_disk);
		current->throttle_disk = disk;
		/* NVMe: throttle 할 disk 설정 */
	}

	if (use_memdelay)
	/* NVMe: memdelay 플래그 설정 */
		current->use_memdelay = use_memdelay;
	set_notify_resume(current);
	/* NVMe: user space 복귀 시 blkcg_maybe_throttle_current() 실행 예약 */
}

/**
 * blkcg_add_delay - add delay to this blkg
 * @blkg: blkg of interest
 * @now: the current time in nanoseconds
 * @delta: how many nanoseconds of delay to add
 *
 * Charge @delta to the blkg's current delay accumulation.  This is used to
 * throttle tasks if an IO controller thinks we need more throttling.
 */
/*
 * blkcg_add_delay - blkg 에 delta 만큼의 IO 지연을 누적
 *
 * 호출 경로: throtl/bfq -> blkcg_add_delay()
 * NVMe 연결점: NVMe queue 의 latency 가 목표를 초과하면 해당 cgroup 의
 *   delay_nsec 에 초과분을 축적한다. 이 값은 blkcg_maybe_throttle_blkg() 에서
 *   태스크 수면 시간으로 변환된다.
 */

void blkcg_add_delay(struct blkcg_gq *blkg, u64 now, u64 delta)
{
	if (WARN_ON_ONCE(atomic_read(&blkg->use_delay) < 0))
	/* NVMe: set_delay 모드와 혼용되면 안 되는 경고 */
		return;
	blkcg_scale_delay(blkg, now);
	/* NVMe: 먼저 지연 예산을 시간에 따라 정규화 */
	atomic64_add(delta, &blkg->delay_nsec);
	/* NVMe: delta 를 atomic 으로 누적; NVMe 멀티 코어에서의 race 방지 */
}

/**
 * blkg_tryget_closest - try and get a blkg ref on the closet blkg
 * @bio: target bio
 * @css: target css
 *
 * As the failure mode here is to walk up the blkg tree, this ensure that the
 * blkg->parent pointers are always valid.  This returns the blkg that it ended
 * up taking a reference on or %NULL if no reference was taken.
 */
/*
 * blkg_tryget_closest - 가장 가까운 살아있는 blkg 에 대한 참조 획득 시도
 *
 * 호출 경로: bio_associate_blkg_from_css() -> blkg_tryget_closest()
 * NVMe 연결점: cgroup 이 소멸 중일 때 NVMe IO 는 상위(부모) blkg 로 spill
 *   된다. blkg->parent 체인을 따라 올라가며 유효한 참조를 얻어 IO 완료까지
 *   blkg 가 유지되도록 한다.
 */

static inline struct blkcg_gq *blkg_tryget_closest(struct bio *bio,
		struct cgroup_subsys_state *css)
{
	struct blkcg_gq *blkg, *ret_blkg = NULL;
	/* NVMe: 검색 중인 blkg 와 결과 */

	rcu_read_lock();
	/* NVMe: blkg_lookup_create 및 parent 체인 접근 보호 */
	blkg = blkg_lookup_create(css_to_blkcg(css), bio->bi_bdev->bd_disk);
	/* NVMe: bio 의 disk(NVMe namespace)에 대한 blkg 검색/생성 */
	while (blkg) {
	/* NVMe: blkg->parent 체인을 따라 올라가며 살아있는 blkg 탐색 */
		if (blkg_tryget(blkg)) {
		/* NVMe: 참조 획득 성공; IO 수명 동안 blkg 유지 */
			ret_blkg = blkg;
			break;
		}
		blkg = blkg->parent;
	/* NVMe: 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
	}
	rcu_read_unlock();

	return ret_blkg;
}

/**
 * bio_associate_blkg_from_css - associate a bio with a specified css
 * @bio: target bio
 * @css: target css
 *
 * Associate @bio with the blkg found by combining the css's blkg and the
 * request_queue of the @bio.  An association failure is handled by walking up
 * the blkg tree.  Therefore, the blkg associated can be anything between @blkg
 * and q->root_blkg.  This situation only happens when a cgroup is dying and
 * then the remaining bios will spill to the closest alive blkg.
 *
 * A reference will be taken on the blkg and will be released when @bio is
 * freed.
 */
/*
 * bio_associate_blkg_from_css - bio 를 지정한 css 의 blkg 에 연결
 *
 * 호출 경로: bio_associate_blkg() -> bio_associate_blkg_from_css()
 *            bio_clone_blkg_association() -> bio_associate_blkg_from_css()
 * NVMe 연결점: bio->bi_blkg 를 설정하여 이후 submit_bio ->
 *   blk_mq_submit_bio -> nvme_queue_rq 경로에서 사용할 cgroup context 를
 *   고정한다. root cgroup 이면 q->root_blkg 를 사용한다.
 */

void bio_associate_blkg_from_css(struct bio *bio,
				 struct cgroup_subsys_state *css)
{
	if (bio->bi_blkg)
		/* NVMe: 기존 blkg 참조 반낑 후 재연결 */
	/* NVMe: 기존 blkg 가 있으면 참조 해제 후 재연결 */
		blkg_put(bio->bi_blkg);

	if (css && css->parent) {
	/* NVMe: root 가 아닌 cgroup 이면 가장 가까운 blkg 검색 */
		bio->bi_blkg = blkg_tryget_closest(bio, css);
	} else {
		blkg_get(bdev_get_queue(bio->bi_bdev)->root_blkg);
		/* NVMe: root cgroup 이면 해당 NVMe queue 의 root_blkg 사용 */
		bio->bi_blkg = bdev_get_queue(bio->bi_bdev)->root_blkg;
		/* NVMe: root_blkg 를 bio->bi_blkg 에 설정; NVMe SQ/CQ 선택의 cgroup 기준 확정 */
	}
}
EXPORT_SYMBOL_GPL(bio_associate_blkg_from_css);

/**
 * bio_associate_blkg - associate a bio with a blkg
 * @bio: target bio
 *
 * Associate @bio with the blkg found from the bio's css and request_queue.
 * If one is not found, bio_lookup_blkg() creates the blkg.  If a blkg is
 * already associated, the css is reused and association redone as the
 * request_queue may have changed.
 */
/*
 * bio_associate_blkg - bio 의 cgroup 에 맞는 blkg 를 찾아 연결
 *
 * 호출 경로: submit_bio() -> bio_associate_blkg()
 * NVMe 연결점: NVMe IO 제출의 시작점에서 bio 가 속한 cgroup 을 결정한다.
 *   passthrough IO 는 제외한다. 이 함수 이후 bio 는
 *   submit_bio -> bio_associate_blkg -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 의
 *   경로를 타게 된다.
 */

void bio_associate_blkg(struct bio *bio)
{
	struct cgroup_subsys_state *css;
	/* NVMe: bio 의 cgroup css */

	if (blk_op_is_passthrough(bio->bi_opf))
	/* NVMe: passthrough/admin 명령은 cgroup 연결 제외 */
		return;

	rcu_read_lock();
	/* NVMe: blkcg_css() 및 blkg 연결의 RCU 보호 */

	if (bio->bi_blkg)
	/* NVMe: 기존 blkg 의 css 재사용 */
		css = bio_blkcg_css(bio);
	else
		css = blkcg_css();
	/* NVMe: 현재 태스크의 cgroup css 획득 */

	bio_associate_blkg_from_css(bio, css);
	/* NVMe: css 에 맞는 blkg 로 bio 연결; NVMe SQ/CQ 선택의 cgroup 기준 확정 */

	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(bio_associate_blkg);

/**
 * bio_clone_blkg_association - clone blkg association from src to dst bio
 * @dst: destination bio
 * @src: source bio
 */
/*
 * bio_clone_blkg_association - src bio 의 blkg 연결을 dst bio 로 복제
 *
 * 호출 경로: bio_clone_* -> bio_clone_blkg_association()
 * NVMe 연결점: NVMe split/clone bio 가 원본과 동일한 cgroup context 를
 *   유지하도록 한다. CID/SQ 에 기록될 때 동일한 cgroup 정책이 적용된다.
 */

void bio_clone_blkg_association(struct bio *dst, struct bio *src)
{
	if (src->bi_blkg)
	/* NVMe: src bio 에 blkg 이 있을 때만 복제 */
		bio_associate_blkg_from_css(dst, bio_blkcg_css(src));
	/* NVMe: dst bio 에 동일한 cgroup css 적용; CID/SQ 기록 시 동일한 cgroup 정책 적용 */
}
EXPORT_SYMBOL_GPL(bio_clone_blkg_association);

/*
 * blk_cgroup_io_type - bio 를 read/write/discard 로 분류
 *
 * 호출 경로: blk_cgroup_bio_start() -> blk_cgroup_io_type()
 * NVMe 연결점: NVMe 명령어 opcode(bi_opf)에 따라 read, write, discard
 *   통계 인덱스로 매핑한다. NVMe PRP/SGL 은 분류에 직접 사용되지 않고
 *   op 코드만 본다.
 */

static int blk_cgroup_io_type(struct bio *bio)
{
	if (op_is_discard(bio->bi_opf))
	/* NVMe: discard/flush 등은 BLKG_IOSTAT_DISCARD 로 분류 */
		return BLKG_IOSTAT_DISCARD;
	if (op_is_write(bio->bi_opf))
	/* NVMe: write 관련 opcode 를 BLKG_IOSTAT_WRITE 로 분류 */
		return BLKG_IOSTAT_WRITE;
	/* NVMe: 나머지는 read 로 분류; NVMe read opcode 와 대응 */
	return BLKG_IOSTAT_READ;
}

/*
 * blk_cgroup_bio_start - bio 의 cgroup IO 통계 및 상태 갱신
 *
 * 호출 경로: block layer IO 시작/완료 지점 -> blk_cgroup_bio_start()
 *            (rq_qos 또는 blk_account 경로를 통해 호출됨, 추정)
 * NVMe 연결점: NVMe IO 가 서비스 되거나 완료될 때 bio->bi_iter.bi_size 와
 *   ios[BLKG_IOSTAT_*] 를 per-cpu blkg_iostat_set 에 누적한다. BIO_CGROUP_ACCT
 *   플래그로 split bio 의 중복 집계를 방지하고, lockless list(lhead)에 등록해
 *   rstat flush 시점에 global 통계로 반영한다.
 */

void blk_cgroup_bio_start(struct bio *bio)
{
	struct blkcg *blkcg = bio->bi_blkg->blkcg;
	/* NVMe: bio 가 속한 cgroup */
	int rwd = blk_cgroup_io_type(bio), cpu;
	/* NVMe: IO 유형(read/write/discard)과 현재 CPU */
	struct blkg_iostat_set *bis;
	/* NVMe: per-cpu 통계 영역 포인터 */
	unsigned long flags;

	if (!cgroup_subsys_on_dfl(io_cgrp_subsys))
	/* NVMe: v1(hierarchy=legacy) cgroup 은 여기서 통계 집계 안 함 */
		return;

	/* Root-level stats are sourced from system-wide IO stats */
	if (!cgroup_parent(blkcg->css.cgroup))
	/* NVMe: root cgroup 통계는 시스템 전체 disk_stats 로 대체 */
		return;

	cpu = get_cpu();
	/* NVMe: 현재 CPU 의 per-cpu 통계 영역 사용; preempt disable 상태 */
	/* NVMe: 현재 CPU 의 per-cpu 통계 영역 사용 */
	bis = per_cpu_ptr(bio->bi_blkg->iostat_cpu, cpu);
	/* NVMe: bio 가 속한 blkg 의 per-cpu iostat_set 획득 */
	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* NVMe: per-cpu 통계 seqlock 진입; irqsave 로 NVMe ISR 컨텍스트에서도 안전 */

	/*
	 * If the bio is flagged with BIO_CGROUP_ACCT it means this is a split
	 * bio and we would have already accounted for the size of the bio.
	 */
	if (!bio_flagged(bio, BIO_CGROUP_ACCT)) {
	/* NVMe: split bio 는 이미 크기를 집계했으므로 중복 방지 */
		bio_set_flag(bio, BIO_CGROUP_ACCT);
		/* NVMe: 중복 집계 방지 플래그 설정 */
		bis->cur.bytes[rwd] += bio->bi_iter.bi_size;
		/* NVMe: bio 크기(바이트)를 read/write/discard 별로 누적; NVMe PRP/SGL 전송 크기(추정) */
		/* NVMe: bio 크기(바이트)를 read/write/discard 별로 누적 */
	}
	bis->cur.ios[rwd]++;
	/* NVMe: read/write/discard IO 횟수 증가; NVMe CID 단위 완료와 대응(추정) */
	/* NVMe: read/write/discard IO 횟수 증가 */

	/*
	 * If the iostat_cpu isn't in a lockless list, put it into the
	 * list to indicate that a stat update is pending.
	 */
	if (!READ_ONCE(bis->lqueued)) {
	/* NVMe: 아직 lockless list 에 없으면 flush 대기열에 등록 */
		struct llist_head *lhead = this_cpu_ptr(blkcg->lhead);
		/* NVMe: 현재 CPU 의 cgroup lockless list 헤드 */

		llist_add(&bis->lnode, lhead);
		/* NVMe: per-cpu 통계 노드를 cgroup 의 lockless list 에 추가; 이후 rstat flush 시 global 로 반영 */
		/* NVMe: per-cpu 통계 노드를 cgroup 의 lockless list 에 추가 */
		WRITE_ONCE(bis->lqueued, true);
		/* NVMe: list 등록 상태를 배리어와 함께 기록; __blkcg_rstat_flush 의 llist_del_all 과 동기화 */
		/* NVMe: list 등록 상태를 배리어와 함께 기록 */
	}

	u64_stats_update_end_irqrestore(&bis->sync, flags);
	/* NVMe: per-cpu 통계 seqlock 해제 */
	css_rstat_updated(&blkcg->css, cpu);
	/* NVMe: cgroup rstat framework 에 통계 갱신 알림; lazy flush 트리거 */
	/* NVMe: cgroup rstat framework 에 통계 갱신 알림 */
	put_cpu();
	/* NVMe: preempt enable 복원 */
}

/*
 * blk_cgroup_congested - 현재 cgroup 계층에 IO 혼잡이 있는지 확인
 *
 * 호출 경로: writeback/congestion 판단 -> blk_cgroup_congested()
 * NVMe 연결점: cgroup 의 congestion_count 가 0보다 크면 NVMe queue 가
 *   지연/스로틀 상태임을 나타낸다. writeback 등에서 추가 IO 제출을 억제하는
 *   데 활용된다.
 */

bool blk_cgroup_congested(void)
{
	struct blkcg *blkcg;
	/* NVMe: 현재 태스크의 cgroup */
	bool ret = false;
	/* NVMe: 혼잡 상태 반환값 */

	rcu_read_lock();
	/* NVMe: blkcg 계층 탐색의 RCU 보호 */
	for (blkcg = css_to_blkcg(blkcg_css()); blkcg;
	/* NVMe: 현재 cgroup 에서 root 까지 계층 순회 */
	     blkcg = blkcg_parent(blkcg)) {
		if (atomic_read(&blkcg->congestion_count)) {
		/* NVMe: congestion_count > 0 이면 NVMe queue 가 지연/스로틀 상태로 판단 */
			ret = true;
			break;
		}
	}
	rcu_read_unlock();
	/* NVMe: RCU read-side 종료 */
	return ret;
}

module_param(blkcg_debug_stats, bool, 0644);
	/* NVMe: debug stats 모듈 파라미터; NVMe queue 지연/통계 디버깅용 */
MODULE_PARM_DESC(blkcg_debug_stats, "True if you want debug stats, false if not");

/* NVMe 관점 핵심 요약
 *
 * - block/blk-cgroup.c 는 NVMe IO 경로의 최상단에서 bio(request) 가 어느
 *   cgroup 에 속하는지를 결정하고, blk-mq/NVMe 드라이버(nvme_queue_rq,
 *   nvme_submit_cmd, doorbell)로 전달되는 cgroup context(bi_blkg)를 관리한다.
 * - per-cpu blkg_iostat_set 과 lockless list(lhead)를 통해 NVMe SSD 의
 *   멀티코어 CQ 완료를 저렴하게 집계하며, cgroup 별 read/write/discard
 *   통계와 use_delay/delay_nsec 기반 스로틀링을 지원한다.
 * - throtl, BFQ, ioprio 같은 정책은 blkcg_policy 를 통해 등록/활성화되며,
 *   NVMe request_queue(q) 단위로 blkg->pd[] 를 할당받아 nvme_queue_rq()
 *   호출 시점에 큐 선택, 제한, 우선순위를 반영한다.
 * - blk-mq, elevator(bio), IO scheduler(bfq-iosched.c 등) 및 throttle
 *   (blk-throttle.c) 파일과 논리적으로 연결되며, NVMe 장치 드라이버
 *   (drivers/nvme/host/pci.c 등) 보다 상위에서 cgroup 단원 추상화를 제공한다.
 */
