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
 * [한국어 설명] 블록 cgroup 공통 제어 인프라 (blk-cgroup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux block layer 의 cgroup(제어 그룹) 통합 핵심 인프라를 구현한다.
 * bio(Block I/O) 가 제출될 때 어느 cgroup 에 속하는지를 결정하고(bio_associate_blkg),
 * per-cgroup IO 통계(read/write/discard 바이트 및 횟수)를 per-cpu lockless 방식으로
 * 집계하며, IO 정책(throttle, BFQ, ioprio 등)을 blkg(blkcg_gq) 단위로 연결·관리한다.
 * cgroup 계층의 생성(blkcg_css_alloc)·온라인(blkcg_css_online)·오프라인(blkcg_css_offline)·
 * 해제(blkcg_css_free) 라이프사이클 콜백도 여기서 구현된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (IO 제출):
 *   userspace write() → submit_bio() → bio_associate_blkg() [이 파일]
 *     → blk_mq_submit_bio() → blk_mq_get_request() → nvme_queue_rq() → doorbell
 *
 * 호출 체인 (IO 통계):
 *   blk_cgroup_bio_start() [이 파일] → per-cpu lockless list 등록
 *     → cgroup rstat flush → blkcg_rstat_flush() → __blkcg_rstat_flush() [이 파일]
 *
 * 호출 체인 (정책 등록):
 *   blk_throtl_init() / bfq_init() → blkcg_policy_register() [이 파일]
 *     → blkcg_activate_policy() [이 파일] → blkg->pd[] 연결
 *
 * 실행 컨텍스트: 커널 스레드(kworker), 태스크 컨텍스트, 소프트IRQ(blkg 통계 완료)
 * 이 파일은 block layer (block/blk-*.c) 와 cgroup 서브시스템(kernel/cgroup/) 사이에 위치한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blk-cgroup.h : blkcg_gq, blkcg, blkcg_policy 등 핵심 구조체 정의
 *   - block/blk-throttle.c (blk_throtl_*) : IO 처리량 제한 정책 구현체
 *   - block/blk-ioprio.c : IO 우선순위 정책 구현체
 *   - block/blk.h : block layer 공통 헬퍼
 *   - kernel/cgroup/rstat.c : cgroup 통계 flush 프레임워크 (css_rstat_flush)
 *
 * 의존받는 모듈:
 *   - block/blk-mq.c : blk_mq_submit_bio 에서 bio->bi_blkg 참조
 *   - block/bio.c : bio_associate_blkg, bio_clone_blkg_association 호출
 *   - drivers/nvme/host/*.c : request_queue 의 blkg 기반 cgroup 분류 결과 사용
 *   - mm/page-writeback.c : blkcg_cgwb_*, writeback cgroup 연동
 *
 * 데이터 흐름:
 *   bio → bi_blkg(blkcg_gq) → iostat_cpu(per-cpu) → lhead(lockless list)
 *     → __blkcg_rstat_flush → blkg->iostat.cur(global) → cgroupfs io.stat 출력
 *
 * === 주요 함수/구조체 요약 ===
 * bio_associate_blkg()        - bio 제출 시 bio->bi_blkg 를 현재 태스크 cgroup 의 blkg 로 설정
 * blkg_lookup_create()        - (cgroup, request_queue) 쌍에 대한 blkg 를 조회하거나 생성
 * blkcg_activate_policy()     - gendisk 에 blkcg 정책(throtl/bfq/ioprio) 활성화; blkg->pd[] 할당
 * blkcg_deactivate_policy()   - gendisk 에서 blkcg 정책 비활성화; blkg->pd[] 해제
 * blkcg_policy_register()     - 정책 모듈 초기화 시 blkcg_policy[] 테이블에 정책 전역 등록
 * blk_cgroup_bio_start()      - bio 시작 시 per-cpu IO 통계 누적 및 lockless list 등록
 * __blkcg_rstat_flush()       - per-cpu lockless list 를 drain 하여 global blkg 통계에 반영
 * blkcg_css_alloc/online/offline/free() - cgroup 라이프사이클 콜백, blkcg 생성·소멸 관리
 * blkg_alloc/blkg_create/blkg_destroy() - blkg 라이프사이클; request_queue-cgroup 연결 생성·삭제
 * blkcg_maybe_throttle_current() - user space 복귀 시 delay_nsec 기반 태스크 throttle 적용
 *
 * 핵심 자료구조:
 *   struct blkcg_gq (blkg): (cgroup, request_queue) 1:1 연결체; pd[]로 정책 데이터, iostat_cpu로 통계 보유
 *   struct blkcg: 한 cgroup의 block subsystem 상태; blkg_tree(radix)·blkg_list·lhead(per-cpu lockless) 포함
 *   struct blkcg_policy: throtl/bfq/ioprio 등 정책 인터페이스; blkcg_policy[] 테이블에 최대 BLKCG_MAX_POLS 개 등록
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
	/* [한국어] policy 등록/해제와 activate/deactivate 사이의 nesting 보호; NVMe queue 정책 활성화 시 경쟁 방지 */
static DEFINE_MUTEX(blkcg_pol_mutex);
	/* [한국어] blkcg_policy[] 및 policy on/off 를 보호; nvme_queue_rq() 가 참조하는 blkg->pd[] 일관성 확보 */

struct blkcg blkcg_root;
	/* [한국어] root cgroup 은 시스템 전체 NVMe IO 의 fallback cgroup */
EXPORT_SYMBOL_GPL(blkcg_root);

struct cgroup_subsys_state * const blkcg_root_css = &blkcg_root.css;
	/* [한국어] root cgroup 의 css; bio_associate_blkg() 경로에서 root blkg 매핑의 기준점 */
EXPORT_SYMBOL_GPL(blkcg_root_css);

static struct blkcg_policy *blkcg_policy[BLKCG_MAX_POLS];
	/* [한국어] throtl/BFQ/ioprio 같은 cgroup 정책 포인터 테이블; NVMe request_queue 의 q->blkcg_pols 와 연결됨 */

static LIST_HEAD(all_blkcgs);		/* protected by blkcg_pol_mutex */
	/* [한국어] 시스템 전체 blkcg 리스트; 정책 등록 시 모든 cgroup 의 cpd 할당 대상 */

bool blkcg_debug_stats = false;
	/* [한국어] debug stats 출력 시 use_delay/delay_nsec 같은 NVMe queue 지연 상태를 노출 */

static DEFINE_RAW_SPINLOCK(blkg_stat_lock);
	/* [한국어] per-cpu lockless list flush 시 reordering 방지용 raw spinlock; NVMe CQ 완료 통계 집계 직렬화 */

#define BLKG_DESTROY_BATCH_SIZE  64
	/* [한국어] NVMe namespace 제거 시 blkg 일괄 제거 배치 크기; 락 장기 점유로 인한 softlockup 방지 */

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
	/* [한국어] 가능한 모든 CPU 에 대해 lhead 초기화 반복 */

	blkcg->lhead = alloc_percpu_gfp(struct llist_head, GFP_KERNEL);
	/* [한국어] per-cpu lockless list 할당; namespace 가 많아도 전체 blkg 순회 대신 갱신된 CPU 만 추적 */
	/* [한국어] per-cpu lockless list, NVMe namespace 가 많아도 전체 blkg 순회 없이 통계 갱신 추적 */
	if (!blkcg->lhead)
		/* [한국어] 메모리 부족 시 NVMe cgroup 통계 인프라 할당 실패 */
		return -ENOMEM;

	for_each_possible_cpu(cpu)
	/* [한국어] 모든 CPU 코어에 대한 lockless 통계 헤드 초기화; NVMe 멀티 큐 완료 경로의 per-cpu 집계 준비 */
		init_llist_head(per_cpu_ptr(blkcg->lhead, cpu));
		/* [한국어] 각 CPU 별 통계 flush 대기열(lhead) 초기화 */
		/* [한국어] 각 CPU 별 통계 flush 엔트리 초기화 */
	return 0;
	/* [한국어] per-cpu lhead 초기화 완료 후 반환 */
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
	/* [한국어] kthread 혹은 current task 의 cgroup 상태 포인터 */

	css = kthread_blkcg();
	/* [한국어] kthread 가 NVMe IO 를 대신 발행할 때 kthread 의 blkcg 우선 사용 */
	if (css)
		/* [한국어] kthread blkcg 가 명시 지정되어 있으면 이를 채택 */
		return css;
	return task_css(current, io_cgrp_id);
	/* [한국어] 일반 태스크라면 io cgroup 의 css 를 반환 -> NVMe SQ/CQ batching 의 cgroup 기준 */
}

/*
 * [한국어]
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
	/* [한국어] work 구조체에서 blkg 객체 복원; NVMe request_queue 와의 연결 해제 직전 단계 */
					     free_work);
	struct request_queue *q = blkg->q;
	/* [한국어] 이 blkg 가 속한 NVMe namespace 의 request_queue */
	int i;

	/*
	 * pd_free_fn() can also be called from blkcg_deactivate_policy(),
	 * in order to make sure pd_free_fn() is called in order, the deletion
	 * of the list blkg->q_node is delayed to here from blkg_destroy(), and
	 * blkcg_mutex is used to synchronize blkg_free_workfn() and
	 * blkcg_deactivate_policy().
	 */
	mutex_lock(&q->blkcg_mutex);
	/* [한국어] blkg 해제와 policy deactivate 간 동기화; NVMe queue 의 cgroup 정책 상태 보호 */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* [한국어] throtl/BFQ/ioprio 등 활성화된 정책 데이터를 순서대로 해제 */
		if (blkg->pd[i])
		/* [한국어] 이 blkg 에 할당된 policy private data 가 있을 때만 해제 */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
			/* [한국어] throtl_data/bfq_queue 해제; 이후 nvme_queue_rq() 에서 해당 정책 상태 참조 불가 */
	if (blkg->parent)
		blkg_put(blkg->parent);
	/* [한국어] 계층 구조상 부모 blkg 참조를 해제하여 cgroup 트리 일관성 유지 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] NVMe request_queue 의 blkg_list 와 queue_lock 보호 */
	list_del_init(&blkg->q_node);
		/* [한국어] NVMe request_queue 의 blkg list 에서 제거; 이후 IO 는 root blkg 로 spill */
	spin_unlock_irq(&q->queue_lock);
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] policy 해제 완료 후 mutex 해제 */

	blk_put_queue(q);
	/* [한국어] request_queue(NVMe namespace q) 참조 해제 */
	free_percpu(blkg->iostat_cpu);
	/* [한국어] per-cpu IO 통계 버퍼 반납 */
	percpu_ref_exit(&blkg->refcnt);
	/* [한국어] blkg 참조 카운터 정리; RCU 해제 이후 최종 자원 반납 */
	kfree(blkg);
	/* [한국어] blkg 객체 반납; NVMe cgroup 분류 엔트리 소멸 */
}

/**
 * blkg_free - free a blkg
 * @blkg: blkg to free
 *
 * Free @blkg which may be partially allocated.
 */
/*
 * [한국어]
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
	/* [한국어] NULL blkg 에 대한 방어적 체크 */
		return;

	/*
	 * Both ->pd_free_fn() and request queue's release handler may
	 * sleep, so free us by scheduling one work func
	 */
	INIT_WORK(&blkg->free_work, blkg_free_workfn);
	/* [한국어] blkg 해제 work 초기화; NVMe namespace cleanup 비동기 수행 */
	schedule_work(&blkg->free_work);
	/* [한국어] workqueue 에 blkg 해제 예약; NVMe queue 완료 경로와 분리된 컨텍스트에서 실행 */
}

/*
 * [한국어]
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
	/* [한국어] RCU 콜백으로부터 blkg 복원 */
	struct blkcg *blkcg = blkg->blkcg;
	/* [한국어] 이 blkg 가 속한 cgroup; rstat flush 의 대상 cgroup */
	int cpu;

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
/* [한국어] kthread 우회 제출이 활성화된 경우에만 async bio 관련 필드 초기화 */
	WARN_ON(!bio_list_empty(&blkg->async_bios));
	/* [한국어] kthread 우회 제출(async_bios)이 남아있으면 버그; NVMe IO 누락 방지 */
#endif
	/*
	 * Flush all the non-empty percpu lockless lists before releasing
	 * us, given these stat belongs to us.
	 *
	 * blkg_stat_lock is for serializing blkg stat update
	 */
	for_each_possible_cpu(cpu)
	/* [한국어] NVMe 멀티 코어 CQ 완료가 기록한 per-cpu 통계를 모두 global 로 flush */
		__blkcg_rstat_flush(blkcg, cpu);
		/* [한국어] 각 CPU 의 lockless list 를 drain 하여 NVMe IO 통계를 상위 cgroup 으로 전파 */

	/* release the blkcg and parent blkg refs this blkg has been holding */
	css_put(&blkg->blkcg->css);
	/* [한국어] blkg 생성 시 획득한 cgroup css 참조 반납 */
	blkg_free(blkg);
	/* [한국어] 실제 메모리 해제를 workqueue 에 예약 */
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
 * [한국어]
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
	/* [한국어] percpu_ref 로부터 blkg 복원 */

	call_rcu(&blkg->rcu_head, __blkg_release);
		/* [한국어] RCU grace period 후 메모리 해제; NVMe CQ/ISR 경로의 read-side 보장 */
}

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
static struct workqueue_struct *blkcg_punt_bio_wq;

/*
 * [한국어]
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
	/* [한국어] work 로부터 blkg 복원 */
					     async_bio_work);
	struct bio_list bios = BIO_EMPTY_LIST;
	/* [한국어] 비동기 제출 대기 중인 bio 들의 임시 리스트 */
	struct bio *bio;
	struct blk_plug plug;
	/* [한국어] plug/batch 시작점; NVMe multi-queue parallelism 를 위한 제출 배치링 */
	bool need_plug = false;
	/* [한국어] bio 가 2개 이상일 때만 plug 를 시작하여 doorbell batching 효과 극대화 */

	/* as long as there are pending bios, @blkg can't go away */
	spin_lock(&blkg->async_bio_lock);
	/* [한국어] async_bios 리스트와 work 경쟁 보호 */
	bio_list_merge_init(&bios, &blkg->async_bios);
	/* [한국어] lock 보호 하에 대기 bio 들을 로컬 리스트로 이동 */
	spin_unlock(&blkg->async_bio_lock);
	/* [한국어] 리스트 이동 완료 후 lock 해제; 이후 submit_bio 는 queue_lock 등 다른 lock 과 교차 가능 */

	/* start plug only when bio_list contains at least 2 bios */
	if (bios.head && bios.head->bi_next) {
		/* [한국어] bio 가 2개 이상이면 plug 시작 -> NVMe SQ batch submit 및 doorbell 최소화 */
		need_plug = true;
		blk_start_plug(&plug);
		/* [한국어] plug 구조체 초기화; 이 구간의 submit_bio 가 NVMe multi-queue scheduler plug list 로 모임 */
	}
	while ((bio = bio_list_pop(&bios)))
	/* [한국어] 대기 bio 리스트를 순회하며 submit_bio; NVMe SQ/CQ CID 태그 할당의 시작점 */
		submit_bio(bio);
		/* [한국어] bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
	if (need_plug)
		blk_finish_plug(&plug);
		/* [한국어] plug 종료; NVMe hctx dispatch 로의 일괄 제출 유도 */
}

/*
 * When a shared kthread issues a bio for a cgroup, doing so synchronously can
 * lead to priority inversions as the kthread can be trapped waiting for that
 * cgroup.  Use this helper instead of submit_bio to punt the actual issuing to
 * a dedicated per-blkcg work item to avoid such priority inversions.
 */
/*
 * [한국어]
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
	/* [한국어] bio 에 연결된 cgroup context; NVMe SQ/CQ 선택의 상위 기준 */

	if (blkg->parent) {
		/* [한국어] root 가 아닌 cgroup 의 bio 만 비동기 우회 제출 */
		spin_lock(&blkg->async_bio_lock);
		/* [한국어] async_bios 리스트 보호 */
		bio_list_add(&blkg->async_bios, bio);
		/* [한국어] bio 를 async 대기열에 추가; 실제 NVMe doorbell 은 workqueue 에서 지연 */
		spin_unlock(&blkg->async_bio_lock);
		queue_work(blkcg_punt_bio_wq, &blkg->async_bio_work);
		/* [한국어] workqueue 에서 blkg_async_bio_workfn 실행 -> submit_bio -> NVMe 경로 */
	} else {
		/* never bounce for the root cgroup */
		submit_bio(bio);
		/* [한국어] root cgroup bio 는 직접 제출; 추가 지연 없이 NVMe SQ 로 진입 */
	}
}
EXPORT_SYMBOL_GPL(blkcg_punt_bio_submit);

/*
 * [한국어]
 * blkcg_punt_bio_init - kthread 우회 bio 제출용 전용 워크큐를 만든다
 *
 * @return: 0 성공, -ENOMEM 실패
 *
 * === 왜 별도 워크큐가 필요한가 ===
 * 어떤 bio는 제출 스레드에서 바로 내보내면 안 된다. 대표적으로 btrfs 같은
 * 파일시스템이 압축/체크섬 작업을 위해 kthread에서 bio를 만드는 경우인데,
 * 그 kthread는 특정 cgroup에 속하지 않는다. 그대로 제출하면 I/O가 root
 * cgroup 몫으로 계산되어 cgroup 대역폭 제한이 우회된다.
 * 그래서 원래 요청자의 cgroup 정보를 실어 이 워크큐로 넘기고, 워커가
 * 그 cgroup의 컨텍스트에서 대신 제출한다("punt" = 넘긴다).
 *
 * === 워크큐 플래그의 의미 ===
 *   WQ_MEM_RECLAIM - 메모리 회수 경로에서도 진행이 보장되어야 한다.
 *     write-back I/O가 이 워크큐를 거치는데, 메모리가 부족할 때 이 워커가
 *     막히면 회수가 끝나지 않아 시스템 전체가 멈춘다. 이 플래그가
 *     전용 rescuer 스레드를 붙여 그 상황을 방지한다.
 *   WQ_FREEZABLE - 시스템 suspend 시 이 워커를 멈춰, 얼어붙은 장치로
 *     I/O가 나가지 않게 한다.
 *   WQ_UNBOUND - 특정 CPU에 묶지 않는다. 제출 CPU 지역성보다 지연 없이
 *     실행되는 것이 중요하기 때문이다.
 *   WQ_SYSFS - /sys/bus/workqueue/devices/blkcg_punt_bio로 노출해
 *     관리자가 nice 값이나 CPU 마스크를 조정할 수 있게 한다.
 *
 * 실행 컨텍스트: subsys_initcall — 부팅 중 블록 계층 초기화 시점.
 *
 * 호출 체인:
 *   subsys_initcall → [blkcg_punt_bio_init] → alloc_workqueue
 */
static int __init blkcg_punt_bio_init(void)
{
	blkcg_punt_bio_wq = alloc_workqueue("blkcg_punt_bio",
	/* [한국어] unbound workqueue; kthread 우회 bio 가 NVMe submit 경로로 진입할 때 CPU affinity 와 무관하게 스케줄링 */
					    WQ_MEM_RECLAIM | WQ_FREEZABLE |
					    WQ_UNBOUND | WQ_SYSFS, 0);
	if (!blkcg_punt_bio_wq)
	/* [한국어] workqueue 할당 실패 시 NVMe kthread 우회 제출 인프라 초기화 실패 */
		return -ENOMEM;
	return 0;
}
subsys_initcall(blkcg_punt_bio_init);
	/* [한국어] 서브시스템 초기화 시 workqueue 등록; NVMe IO 우회 경로 준비 */
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
 * [한국어]
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
	/* [한국어] bio 가 없거나 cgroup 미연결 시 NULL; NVMe passthrough/admin 명령 등 */
		return NULL;
	return &bio->bi_blkg->blkcg->css;
	/* [한국어] bio->bi_blkg 경로로 cgroup css 반환 */
}
EXPORT_SYMBOL_GPL(bio_blkcg_css);

/**
 * blkcg_parent - get the parent of a blkcg
 * @blkcg: blkcg of interest
 *
 * Return the parent blkcg of @blkcg.  Can be called anytime.
 */
/*
 * [한국어]
 * blkcg_parent - 부모 blkcg를 반환 (cgroup 트리 상향 탐색)
 *
 * @blkcg: 기준 blkcg
 * @return: 부모 blkcg. root cgroup이면 NULL.
 *
 * blkcg는 cgroup 코어의 css(cgroup_subsys_state)를 내장하고 있고, 트리 구조
 * 자체는 css가 관리한다. 따라서 부모를 찾으려면 css의 parent를 따라간 뒤
 * 다시 blkcg로 되돌리면 된다.
 *
 * 이 상향 탐색이 필요한 이유는 blk-cgroup의 여러 정책이 계층적이기 때문이다.
 * 자식 cgroup에 설정이 없으면 부모의 설정을 물려받고(상속), 통계는 자식에서
 * 부모로 합산되어 올라간다(recursive sum). 그 두 방향의 순회가 모두 이
 * 함수를 쓴다.
 *
 * root cgroup에서는 css.parent가 NULL이므로 이 함수도 NULL을 반환한다 —
 * 호출자는 그것을 "트리 꼭대기에 도달했다"는 종료 조건으로 쓴다.
 *
 * 실행 컨텍스트: 어디서든(단순 포인터 역참조). 다만 blkcg 자체의 생존은
 * 호출자가 참조나 RCU로 보장해야 한다.
 *
 * 호출 체인:
 *   blkg_alloc / blkcg_css_online / 정책별 상속 로직 → [blkcg_parent]
 */
static inline struct blkcg *blkcg_parent(struct blkcg *blkcg)
{
	return css_to_blkcg(blkcg->css.parent);
	/* [한국어] css.parent 를 blkcg 로 변환; NVMe queue 의 cgroup 트리 탐색 */
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
 * [한국어]
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
	/* [한국어] 할당될 blkcg_gq 객체; NVMe request_queue 와 cgroup 의 1:1 연결체 */
	int i, cpu;

	/* alloc and init base part */
	blkg = kzalloc_node(sizeof(*blkg), gfp_mask, disk->queue->node);
	/* [한국어] NVMe namespace q 의 NUMA node 에 blkg 할당; 메모리 지역성으로 NVMe CQ 완료 경로 성능 향상(추정) */
	if (!blkg)
		/* [한국어] blkg 할당 실패 시 NVMe cgroup 분류 불가; 상위에서 root blkg 로 fallback */
		return NULL;
	if (percpu_ref_init(&blkg->refcnt, blkg_release, 0, gfp_mask))
	/* [한국어] percpu_ref 초기화; NVMe IO 가 blkg 를 참조하는 동안 메모리 유지 */
		goto out_free_blkg;
	blkg->iostat_cpu = alloc_percpu_gfp(struct blkg_iostat_set, gfp_mask);
	/* [한국어] NVMe 멀티 코어 CQ 완료용 per-cpu 통계 영역 할당 */
	if (!blkg->iostat_cpu)
		goto out_exit_refcnt;
		/* [한국어] per-cpu 통계 할당 실패 시 blkg 할당 롤백 */
	if (!blk_get_queue(disk->queue))
	/* [한국어] request_queue(NVMe namespace q) 참조 획득 실패 시 롤백; queue 가 dying 상태면 실패할 수 있음 */
		goto out_free_iostat;

	blkg->q = disk->queue;
	/* [한국어] blkg 가 속한 NVMe request_queue 설정 */
	INIT_LIST_HEAD(&blkg->q_node);
	/* [한국어] request_queue->blkg_list 연결 준비 */
	blkg->blkcg = blkcg;
	/* [한국어] blkg 가 대표하는 cgroup 설정 */
	blkg->iostat.blkg = blkg;
	/* [한국어] global 통계가 역참조할 blkg 설정 */
#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	spin_lock_init(&blkg->async_bio_lock);
	/* [한국어] kthread 우회 bio 리스트 보호 */
	bio_list_init(&blkg->async_bios);
	/* [한국어] kthread 우회 대기 bio 리스트 초기화 */
	INIT_WORK(&blkg->async_bio_work, blkg_async_bio_workfn);
#endif

	u64_stats_init(&blkg->iostat.sync);
	/* [한국어] global 통계의 u64_stats_sync 초기화 (32bit 배경) */
	for_each_possible_cpu(cpu) {
	/* [한국어] 모든 CPU 에 대해 per-cpu 통계 초기화; NVMe 멀티 큐 완료 경로의 lockless 집계 준비 */
		u64_stats_init(&per_cpu_ptr(blkg->iostat_cpu, cpu)->sync);
		/* [한국어] per-cpu 통계의 seqlock/u64_stats_sync 초기화 */
		per_cpu_ptr(blkg->iostat_cpu, cpu)->blkg = blkg;
		/* [한국어] per-cpu 통계가 역참조할 blkg 설정 */
	}

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* [한국어] 활성화된 모든 cgroup 정책에 대해 private data 할당/연결 */
	/* [한국어] queue 제거 시 모든 활성화된 cgroup 정책 비트 클리어 */
		struct blkcg_policy *pol = blkcg_policy[i];
		/* [한국어] i 번째 정책(throtl/BFQ/ioprio) 포인터 */
		struct blkg_policy_data *pd;
		/* [한국어] 정책별 private data 포인터; bfq_queue/throtl_data 등 */

		if (!blkcg_policy_enabled(disk->queue, pol))
		/* [한국어] 해당 queue(NVMe namespace)에서 이 정책이 켜져 있을 때만 pd 할당 */
		/* [한국어] 해당 queue(NVMe namespace)에서 이 정책이 켜져 있을 때만 할당 */
			continue;

		/* alloc per-policy data and attach it to blkg */
		pd = pol->pd_alloc_fn(disk, blkcg, gfp_mask);
		/* [한국어] 정책별 private data 할당 (예: throtl_data, bfq_queue); NVMe queue depth/latency 제어 상태 */
		/* [한국어] 정책별 private data 할당 (예: throtl_data, bfq_queue) */
		if (!pd)
		/* [한국어] pd 할당 실패 시 지금까지 할당한 pd 롤백 */
			goto out_free_pds;
		blkg->pd[i] = pd;
		/* [한국어] blkg 에 정책 데이터 연결; nvme_queue_rq() 시 이 데이터로 SQ/CQ 선택/제한 */
		pd->blkg = blkg;
		/* [한국어] pd 가 역참조할 blkg 설정 */
		pd->plid = i;
		/* [한국어] policy id 기록; q->blkcg_pols 비트와 대응 */
		pd->online = false;
		/* [한국어] 아직 online 콜백 전; IO 경로에서 pd 사용은 online 이후 */
	}

	return blkg;

out_free_pds:
	while (--i >= 0)
	/* [한국어] 할당 실패 시 역순으로 이미 할당한 정책 데이터 해제 */
		if (blkg->pd[i])
		/* [한국어] i 번째 pd 가 존재하면 해제 */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
			/* [한국어] throtl/BFQ 상태 해제; NVMe queue 의 cgroup 제어 상태 복구 */
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
 * [한국어]
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
	/* [한국어] 생성/등록될 blkg */
	int i, ret;

	lockdep_assert_held(&disk->queue->queue_lock);
	/* [한국어] queue_lock 이 잡힌 상태에서만 blkg 생성; NVMe request_queue 상태 일관성 보호 */

	/* request_queue is dying, do not create/recreate a blkg */
	if (blk_queue_dying(disk->queue)) {
	/* [한국어] queue 가 제거 중이면 새 blkg 를 만들지 않음; NVMe controller reset/remove 경로 */
	/* [한국어] queue 가 제거 중이면 새 blkg 를 만들지 않음 */
		ret = -ENODEV;
		/* [한국어] dying queue 에 대한 blkg 생성 거부 */
		goto err_free_blkg;
	}

	/* blkg holds a reference to blkcg */
	if (!css_tryget_online(&blkcg->css)) {
	/* [한국어] cgroup 이 online 상태가 아니면 blkg 생성 실패; cgroup 소멸 중일 때 NVMe IO spill 방지 */
	/* [한국어] cgroup 이 online 상태가 아니면 blkg 생성 실패 */
		ret = -ENODEV;
		goto err_free_blkg;
	}

	/* allocate */
	if (!new_blkg) {
	/* [한국어] 호출자가 미리 할당한 blkg 가 없으면 GFP_NOWAIT 로 시도 */
		new_blkg = blkg_alloc(blkcg, disk, GFP_NOWAIT);
		/* [한국어] IO 경로에서 락을 잡은 채로 빠르게 blkg 할당 시도 */
		if (unlikely(!new_blkg)) {
		/* [한국어] GFP_NOWAIT 실패 시 -ENOMEM; 상위 blkg 로 fallback 가능 */
			ret = -ENOMEM;
			goto err_put_css;
		}
	}
	blkg = new_blkg;
	/* [한국어] 할당받은 blkg 를 실제 등록 대상으로 설정 */

	/* link parent */
	if (blkcg_parent(blkcg)) {
	/* [한국어] root 가 아닌 cgroup 이면 부모 blkg 와 연결 */
		blkg->parent = blkg_lookup(blkcg_parent(blkcg), disk->queue);
		/* [한국어] 상위 cgroup blkg 를 찾아 계층 구조 연결; throttle/통계 전파 경로 */
		/* [한국어] 상위 cgroup blkg 를 찾아 계층 구조 연결 */
		if (WARN_ON_ONCE(!blkg->parent)) {
		/* [한국어] 부모 blkg 가 없으면 계층 구조 파괴; 버그로 간주 */
			ret = -ENODEV;
			goto err_put_css;
		}
		blkg_get(blkg->parent);
		/* [한국어] 부모 참조 획득; 자식 blkg 수명 동안 부모 유지 */
	}

	/* invoke per-policy init */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* [한국어] 생성된 blkg 의 정책 데이터 초기화; throtl/bfq 상태 기본값 설정 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (blkg->pd[i] && pol->pd_init_fn)
		/* [한국어] pd 가 할당되고 init 콜백이 있을 때만 초기화 */
		/* [한국어] 정책 초기화 콜백 (throtl/bfq 상태 기본값 설정) */
			pol->pd_init_fn(blkg->pd[i]);
	}

	/* insert */
	spin_lock(&blkcg->lock);
	/* [한국어] blkcg lock 획득; radix tree/list 와 rstat 간 동기화 */
	ret = radix_tree_insert(&blkcg->blkg_tree, disk->queue->id, blkg);
	/* [한국어] queue id(NVMe namespace 식별자)로 blkg 색인 추가 */
	if (likely(!ret)) {
	/* [한국어] radix tree 삽입 성공 시 list 에도 등록 */
		hlist_add_head_rcu(&blkg->blkcg_node, &blkcg->blkg_list);
		/* [한국어] RCU read-side(blkg_lookup)에서 볼 수 있게 list 에 추가 */
		list_add(&blkg->q_node, &disk->queue->blkg_list);
		/* [한국어] request_queue 의 blkg list 에 등록 */

		for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] list 등록 후 정책 online; 이 시점부터 IO 경로에서 pd 참조 가능 */
			struct blkcg_policy *pol = blkcg_policy[i];

			if (blkg->pd[i]) {
			/* [한국어] 할당된 정책 데이터가 있으면 online 처리 */
				if (pol->pd_online_fn)
				/* [한국어] 정책 online 콜백, 이제 IO 경로에서 참조 가능 */
					pol->pd_online_fn(blkg->pd[i]);
				/* [한국어] 정책 online 콜백, 이제 IO 경로에서 참조 가능 */
				blkg->pd[i]->online = true;
				/* [한국어] pd online 상태 표시 */
			}
		}
	}
	blkg->online = true;
	/* [한국어] IO 경로에서 이 blkg 를 사용할 수 있음을 표시 */
	spin_unlock(&blkcg->lock);

	if (!ret)
	/* [한국어] 등록 성공; bio_associate_blkg 에서 사용 가능 */
		return blkg;

	/* @blkg failed fully initialized, use the usual release path */
	blkg_put(blkg);
		/* [한국어] 초기화 실패 시 blkg 참조 반납 -> blkg_free_workfn 으로 해제 */
	return ERR_PTR(ret);

err_put_css:
	css_put(&blkcg->css);
		/* [한국어] css_tryget_online 으로 획득한 css 참조 반납 */
err_free_blkg:
	if (new_blkg)
	/* [한국어] 할당된 new_blkg 메모리 해제 예약 */
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
 * [한국어]
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
	/* [한국어] bio 의 대상 NVMe namespace request_queue */
	struct blkcg_gq *blkg;
	/* [한국어] 검색/생성 결과 blkg */
	unsigned long flags;

	WARN_ON_ONCE(!rcu_read_lock_held());
	/* [한국어] RCU read-side 필요; blkg_lookup() 과 radix tree 접근 보호 */

	blkg = blkg_lookup(blkcg, q);
	/* [한국어] radix tree 로 기존 blkg 검색; O(1) cgroup 분류 */
	if (blkg)
		/* [한국어] 기존 blkg 반환; NVMe SQ/CQ 선택에 사용 */
		return blkg;

	spin_lock_irqsave(&q->queue_lock, flags);
	/* [한국어] queue_lock 획득; blkg 생성과 동시 제출 경쟁 보호 */
	blkg = blkg_lookup(blkcg, q);
	/* [한국어] 락 획득 후 재확인; 다른 CPU 가 이미 생성했을 수 있음 */
	if (blkg) {
		if (blkcg != &blkcg_root &&
		/* [한국어] root 가 아니고 hint 가 다륾면 hint 갱신; 이후 bio 제출 시 탐색 가속 */
		    blkg != rcu_dereference(blkcg->blkg_hint))
			rcu_assign_pointer(blkcg->blkg_hint, blkg);
			/* [한국어] RCU 배리어 내장; hint 가 일관되게 보이도록 설정 */
		goto found;
	}

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.  Returns the closest
	 * blkg to the intended blkg should blkg_create() fail.
	 */
	while (true) {
	/* [한국어] root 에서 목표 cgroup 까지 부모를 따라 남겨가며 blkg 생성; cgroup 계층 무결성 */
	/* [한국어] root 에서 목표 cgroup 까지 부모를 따라 남겨가며 blkg 생성 */
		struct blkcg *pos = blkcg;
		/* [한국어] 현재 생성해야 할 cgroup 위치 */
		struct blkcg *parent = blkcg_parent(blkcg);
		/* [한국어] pos 의 부모 cgroup */
		struct blkcg_gq *ret_blkg = q->root_blkg;
		/* [한국어] 생성 실패 시 root blkg 로 fallback 준비 */

		while (parent) {
		/* [한국어] 부모 중 가장 가까운 존재하는 blkg 를 찾아 fallback 지점 확보 */
			blkg = blkg_lookup(parent, q);
		/* [한국어] 부모 cgroup 의 blkg 검색 */
			if (blkg) {
				/* remember closest blkg */
				ret_blkg = blkg;
			/* [한국어] 생성 실패 시 이 blkg 로 IO 를 spill 할 수 있음 */
				break;
			}
			pos = parent;
		/* [한국어] 아직 blkg 가 없는 가장 가까운 조상으로 이동 */
			parent = blkcg_parent(parent);
		/* [한국어] 한 단계 더 위의 조상 탐색 */
		}

		blkg = blkg_create(pos, disk, NULL);
		/* [한국어] pos cgroup 의 blkg 생성; NVMe queue 의 cgroup 분류 노드 추가 */
		if (IS_ERR(blkg)) {
		/* [한국어] 생성 실패 시 가장 가까운 부모 blkg 로 fallback; NVMe IO 누락 방지 */
			blkg = ret_blkg;
			break;
		}
		if (pos == blkcg)
			break;
	}

found:
	spin_unlock_irqrestore(&q->queue_lock, flags);
	/* [한국어] queue_lock 해제; 이후 submit_bio 가 blkg 를 참조 가능 */
	return blkg;
}

/*
 * [한국어]
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
	/* [한국어] blkg 가 속한 cgroup */
	int i;

	lockdep_assert_held(&blkg->q->queue_lock);
	/* [한국어] queue_lock 보호 하에서만 blkg 제거; NVMe request_queue 상태와 동기화 */
	lockdep_assert_held(&blkcg->lock);
	/* [한국어] blkcg lock 보호 하에서만 radix tree/list 조작 */

	/*
	 * blkg stays on the queue list until blkg_free_workfn(), see details in
	 * blkg_free_workfn(), hence this function can be called from
	 * blkcg_destroy_blkgs() first and again from blkg_destroy_all() before
	 * blkg_free_workfn().
	 */
	if (hlist_unhashed(&blkg->blkcg_node))
	/* [한국어] 이미 제거된 blkg 는 다시 destroy 하지 않음; 중복 제거 방지 */
	/* [한국어] 이미 제거된 blkg 는 다시 destroy 하지 않음 */
		return;

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* [한국어] 활성화된 정책들을 offline -> free 경로로 전환 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (blkg->pd[i] && blkg->pd[i]->online) {
		/* [한국어] online 상태인 pd 만 offline 처리; NVMe IO 경로에서 pd 접근 차단 */
			blkg->pd[i]->online = false;
			/* [한국어] pd offline 표시 */
			if (pol->pd_offline_fn)
				pol->pd_offline_fn(blkg->pd[i]);
		}
	}

	blkg->online = false;
	/* [한국어] blkg 를 IO 경로에서 사용 불가로 표시; 이후 bio 는 상위 blkg 로 spill */

	radix_tree_delete(&blkcg->blkg_tree, blkg->q->id);
	/* [한국어] queue id 기반 radix tree 색인 제거 */
	hlist_del_init_rcu(&blkg->blkcg_node);
	/* [한국어] RCU read-side 가 완료될 때까지 메모리는 유지; NVMe CQ 완료 경로 안전성 */

	/*
	 * Both setting lookup hint to and clearing it from @blkg are done
	 * under queue_lock.  If it's not pointing to @blkg now, it never
	 * will.  Hint assignment itself can race safely.
	 */
	if (rcu_access_pointer(blkcg->blkg_hint) == blkg)
	/* [한국어] hint 가 제거 대상 blkg 를 가리키면 NULL 로 클리어 */
		rcu_assign_pointer(blkcg->blkg_hint, NULL);
		/* [한국어] RCU 배리어를 통해 hint 일관성 유지 */

	/*
	 * Put the reference taken at the time of creation so that when all
	 * queues are gone, group can be destroyed.
	 */
	percpu_ref_kill(&blkg->refcnt);
	/* [한국어] blkg 참조 카운트 종료, 이후 IO 는 root blkg 로 spill; percpu_ref 가 0이 되면 RCU 해제 */
	/* [한국어] blkg 참조 카운트 종료, 이후 IO 는 root blkg 로 spill */
}

/*
 * [한국어]
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
	/* [한국어] 제거 대상 NVMe namespace request_queue */
	struct blkcg_gq *blkg;
	int count = BLKG_DESTROY_BATCH_SIZE;
	/* [한국어] 한 번에 제거할 blkg 개수; NVMe queue lock 장기 점유 방지 */
	int i;

restart:
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득; blkg_list 순회 보호 */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* [한국어] queue 의 모든 blkg 를 순회하며 제거 */
	/* [한국어] 메모리 부족 시 이미 추가된 정책 데이터 모두 롤백 */
		struct blkcg *blkcg = blkg->blkcg;
		/* [한국어] 현재 blkg 의 cgroup */

		if (hlist_unhashed(&blkg->blkcg_node))
		/* [한국어] 이미 제거된 blkg 는 스킵 */
			continue;

		spin_lock(&blkcg->lock);
		/* [한국어] blkcg lock 추가 획득; radix tree/list 동기화 */
		blkg_destroy(blkg);
		/* [한국어] blkg 제거 및 refcnt 종료 */
		spin_unlock(&blkcg->lock);

		/*
		 * in order to avoid holding the spin lock for too long, release
		 * it when a batch of blkgs are destroyed.
		 */
		if (!(--count)) {
		/* [한국어] 배치 단위로 queue_lock 해제; softirq/NVMe ISR 응답 지연 방지 */
			count = BLKG_DESTROY_BATCH_SIZE;
			spin_unlock_irq(&q->queue_lock);
			/* [한국어] 스케줄링 양보 */
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
	/* [한국어] queue 에서 모든 cgroup 정책 비트 클리어 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (pol)
		/* [한국어] 이 queue(NVMe namespace)에서 해당 정책 비활성화 표시 */
			__clear_bit(pol->plid, q->blkcg_pols);
			/* [한국어] queue 에서 정책 비활성화 표시 */
			/* [한국어] 이 queue 에서 해당 정책 비활성화 표시 */
	}

	q->root_blkg = NULL;
	/* [한국어] root blkg 제거 완료, 이후 disk rebind 가능 */
	spin_unlock_irq(&q->queue_lock);

	wake_up_var(&q->root_blkg);
	/* [한국어] root_blkg NULL 대기 중인 blkcg_init_disk() 깨우기 */
}

/*
 * [한국어]
 * blkg_iostat_set - I/O 통계 구조체를 통째로 복사(대입)
 *
 * @dst: 복사 대상
 * @src: 복사 원본
 * @return: 없음
 *
 * blkg_iostat은 read/write/discard 세 방향의 bytes[]와 ios[] 배열을 담은
 * 구조체다. 구조체 대입(*dst = *src) 대신 필드를 하나씩 도는 이유는,
 * 이 구조체가 u64_stats_sync로 보호되는 seqlock 영역 안에서 다뤄지기 때문에
 * 컴파일러가 임의로 최적화하거나 재배치하지 않도록 명시적으로 쓰기 위해서다.
 *
 * 실행 컨텍스트: u64_stats_update_begin/end 구간 안(호출자가 보장).
 *
 * 호출 체인:
 *   __blkg_clear_stat / blkcg_iostat_update / blkg_iostat_add 등 → [blkg_iostat_set]
 */
static void blkg_iostat_set(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* [한국어] BLKG_IOSTAT_READ/WRITE/DISCARD 세 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* [한국어] read/write/discard 세 항목을 순회하며 복사한다. 이 세 분류는
	 * op_stat_group()이 REQ_OP_*에서 도출하며, NVMe에서는 각각
	 * Read(0x02) / Write(0x01)·Write Zeroes(0x08) / DSM(0x09)에 대응한다. */
		dst->bytes[i] = src->bytes[i];
		/* [한국어] i 유형(read/write/discard) 바이트 복사 */
		dst->ios[i] = src->ios[i];
		/* [한국어] i 유형 IO 횟수 복사 */
	}
}

/*
 * [한국어]
 * __blkg_clear_stat - per-CPU 통계 슬롯 하나를 0으로 초기화
 *
 * @bis: 초기화할 per-CPU 통계 집합(cur + last + seqlock)
 * @return: 없음
 *
 * blkg_iostat_set 구조체는 두 개의 통계를 갖는다:
 *   cur  - 지금까지 이 CPU에서 누적된 값
 *   last - 마지막으로 상위(부모 blkg)로 전파할 때의 스냅숏
 * 둘의 차이가 "아직 전파하지 않은 증분"이며, 그래서 초기화할 때 둘 다
 * 0으로 맞춰야 한다. cur만 지우면 last가 남아 다음 전파에서 음수 증분이
 * 계산된다.
 *
 * u64_stats_update_begin_irqsave/end_irqrestore로 감싸는 이유: 32비트
 * 아키텍처에서 u64 값은 두 번의 32비트 쓰기로 나뉘어, 그 사이에 읽는
 * 쪽이 앞뒤가 섞인 값(torn read)을 볼 수 있다. seqlock이 읽는 쪽에
 * 재시도를 시켜 그것을 막는다. 64비트에서는 이 매크로가 IRQ 비활성화만
 * 남기고 사실상 사라진다.
 *
 * 실행 컨텍스트: cgroup 통계 리셋 경로(프로세스 컨텍스트). IRQ를 끄고 진행.
 *
 * 호출 체인:
 *   blkg_clear_stat → [__blkg_clear_stat] → blkg_iostat_set
 */
static void __blkg_clear_stat(struct blkg_iostat_set *bis)
{
	struct blkg_iostat cur = {0};
	/* [한국어] 0 으로 초기화할 임시 통계 구조체 */
	unsigned long flags;

	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* [한국어] u64_stats_seqlock 진입; 32bit NVMe 통계 업데이트의 readers/writers 동기화 */
	blkg_iostat_set(&bis->cur, &cur);
	/* [한국어] cur 통계 클리어 */
	blkg_iostat_set(&bis->last, &cur);
	/* [한국어] last 통계 클리어; delta 계산 기준점 재설정 */
	u64_stats_update_end_irqrestore(&bis->sync, flags);
	/* [한국어] seqlock 해제; NVMe 통계 readers 에게 일관된 값 공개 */
}

/*
 * [한국어]
 * blkg_clear_stat - 한 blkg의 모든 CPU 통계와 전역 통계를 초기화
 *
 * @blkg: 초기화할 blkcg_gq(= cgroup × 디스크 조합 하나)
 * @return: 없음
 *
 * 통계가 per-CPU에 흩어져 있으므로 모든 CPU의 슬롯을 순회해야 한다.
 * for_each_possible_cpu를 쓰는 이유가 중요하다 — online CPU만 돌면,
 * 지금 오프라인이지만 과거에 값을 쌓아 둔 CPU의 통계가 남는다. 그 CPU가
 * 나중에 온라인되면 지워졌어야 할 값이 되살아난다.
 *
 * 사용 시점: 사용자가 cgroup의 io.stat을 리셋하거나, blkg가 새로 만들어질 때
 * 이전 사용의 잔재를 없앤다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blkcg_reset_stats(cgroupfs write) → [blkg_clear_stat] → __blkg_clear_stat
 */
static void blkg_clear_stat(struct blkcg_gq *blkg)
{
	int cpu;

	for_each_possible_cpu(cpu) {
	/* [한국어] NVMe 멀티 코어 CQ 완료가 사용한 모든 per-cpu 통계 영역 초기화 */
		struct blkg_iostat_set *s = per_cpu_ptr(blkg->iostat_cpu, cpu);
		/* [한국어] cpu 번호의 per-cpu blkg_iostat_set 획득 */

		__blkg_clear_stat(s);
	}
	__blkg_clear_stat(&blkg->iostat);
}

/*
 * [한국어]
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
	/* [한국어] 대상 cgroup */
	struct blkcg_gq *blkg;
	int i;

	pr_info_once("blkio.%s is deprecated\n", cftype->name);
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 등록/해제와 reset 경쟁 보호 */
	spin_lock_irq(&blkcg->lock);
	/* [한국어] blkcg 의 blkg_list 순회 보호 */

	/*
	 * Note that stat reset is racy - it doesn't synchronize against
	 * stat updates.  This is a debug feature which shouldn't exist
	 * anyway.  If you get hit by a race, retry.
	 */
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
	/* [한국어] 이 cgroup 의 모든 NVMe namespace blkg 순회 */
		blkg_clear_stat(blkg);
		for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] 각 정책별 통계 리셋 콜백 순회 */
			struct blkcg_policy *pol = blkcg_policy[i];

			if (blkg->pd[i] && pol->pd_reset_stats_fn)
			/* [한국어] 정책별 통계 리셋 함수 호출; throtl/BFQ NVMe 통계 초기화 */
				pol->pd_reset_stats_fn(blkg->pd[i]);
		}
	}

	spin_unlock_irq(&blkcg->lock);
	mutex_unlock(&blkcg_pol_mutex);
	return 0;
}

/*
 * [한국어]
 * blkg_dev_name - blkg가 가리키는 블록 장치의 이름 문자열을 얻는다
 *
 * @blkg: 이름을 알고 싶은 blkcg_gq
 * @return: "nvme0n1" 같은 장치 이름. 디스크가 아직 없으면 NULL.
 *
 * cgroupfs의 io.stat, io.max 등은 "장치이름 값" 형식으로 출력되므로 blkg마다
 * 대응하는 장치 이름이 필요하다. bdi(backing_dev_info)의 이름을 쓰는 이유는
 * 그것이 파티션이 아닌 디스크 단위 이름을 주기 때문이다.
 *
 * NULL이 반환될 수 있는 상황: blkg는 request_queue 단위로 만들어지는데, 큐가
 * gendisk보다 먼저 존재할 수 있다(큐를 만든 뒤 디스크를 붙이는 순서).
 * 그 짧은 구간에 통계를 출력하려 하면 이름이 없고, 호출자는 그 blkg를
 * 출력에서 건너뛴다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   __blkg_prfill_u64 / blkcg_print_one_stat 등 → [blkg_dev_name]
 */
const char *blkg_dev_name(struct blkcg_gq *blkg)
{
	if (!blkg->q->disk)
	/* [한국어] disk 가 없으면 이름 없음; 미연결 queue */
		return NULL;
	return bdi_dev_name(blkg->q->disk->bdi);
	/* [한국어] bdi 이름 반환; NVMe namespace stat 출력용 */
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
/*
 * [한국어] (위 영문 kernel-doc 참고)
 * blkcg_print_blkgs - 한 cgroup에 속한 모든 blkg를 순회하며 출력 콜백을 호출
 *
 * @sf:         출력 대상 seq_file
 * @blkcg:      순회 기준이 되는 cgroup
 * @prfill:     blkg마다 호출될 출력 콜백. 정책이 자기 형식에 맞게 제공한다.
 * @pol:        대상 정책. 이 정책의 policy_data가 없는 blkg는 건너뛴다.
 * @data:       prfill에 그대로 전달되는 값(보통 policy_data 안의 필드 오프셋)
 * @show_total: true면 모든 prfill 반환값의 합을 마지막에 "Total"로 출력
 * @return: 없음
 *
 * cgroupfs의 정책별 통계/설정 파일을 읽을 때 공통으로 쓰는 순회 골격이다.
 * 각 정책(blk-throttle, blk-iolatency, BFQ)은 출력 형식만 prfill 콜백으로
 * 제공하고, "어떤 blkg들을 어떤 순서로, 어떤 락 아래에서 도는가"라는 공통
 * 문제는 이 함수가 한 번에 해결한다.
 *
 * @pol의 policy_data가 없는 blkg를 건너뛰는 이유: 정책은 장치마다 개별적으로
 * 활성화되므로, 같은 cgroup 안에서도 어떤 디스크에는 blk-throttle이 붙어
 * 있고 어떤 디스크에는 없을 수 있다. 없는 쪽은 출력할 값 자체가 없다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트). 각 blkg의 큐 락을 잡은
 * 상태로 prfill을 호출하므로, 콜백 안에서 잠들면 안 된다.
 *
 * 호출 체인:
 *   cgroupfs read → 정책의 seq_show 콜백 → [blkcg_print_blkgs]
 *     → prfill (예: __blkg_prfill_u64, blkg_prfill_rwstat)
 */
void blkcg_print_blkgs(struct seq_file *sf, struct blkcg *blkcg,
		       u64 (*prfill)(struct seq_file *,
				     struct blkg_policy_data *, int),
		       const struct blkcg_policy *pol, int data,
		       bool show_total)
{
	struct blkcg_gq *blkg;
	/* [한국어] 순회 중인 blkg */
	u64 total = 0;
	/* [한국어] 출력값 합산; NVMe namespace 간 cgroup 통계 집계 */

	rcu_read_lock();
	/* [한국어] blkg_list RCU read-side 보호 */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
	/* [한국어] cgroup 의 모든 NVMe namespace blkg 를 RCU 로 순회 */
		spin_lock_irq(&blkg->q->queue_lock);
		/* [한국어] blkg 출력 시 해당 NVMe queue lock 획득 */
		if (blkcg_policy_enabled(blkg->q, pol))
		/* [한국어] 해당 queue 에서 정책이 활성화된 blkg 만 출력 */
			total += prfill(sf, blkg->pd[pol->plid], data);
			/* [한국어] 정책별 출력 함수 호출; NVMe queue 별 throtl/BFQ 상태 노출 */
		spin_unlock_irq(&blkg->q->queue_lock);
	}
	rcu_read_unlock();

	if (show_total)
	/* [한국어] namespace 간 NVMe cgroup 통계 합계 출력 */
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
/*
 * [한국어]
 * __blkg_prfill_u64 - "장치이름 값" 한 줄을 seq_file에 출력하는 공용 prfill
 *
 * @sf: 출력 대상 seq_file(cgroupfs 파일 읽기 버퍼)
 * @pd: 출력 중인 blkg의 정책 데이터. 여기서 장치 이름을 얻는다.
 * @v:  출력할 값
 * @return: 출력한 값 v. 장치 이름이 없어 출력을 건너뛰었으면 0.
 *
 * blkcg_print_blkgs()가 blkg마다 호출하는 콜백의 가장 단순한 구현이다.
 * 정책들(blk-throttle, blk-iolatency 등)은 자기 값을 꺼내 이 함수에 넘기기만
 * 하면 되므로 출력 형식이 통일된다.
 *
 * 반환값이 "출력한 값"인 이유: blkcg_print_blkgs()가 show_total 옵션일 때
 * 모든 blkg의 반환값을 더해 마지막에 "Total"로 찍는다. 출력하지 않은
 * 경우 0을 반환해야 그 합계가 왜곡되지 않는다.
 *
 * 실행 컨텍스트: cgroupfs read. 호출자가 큐 락을 쥔 상태다.
 *
 * 호출 체인:
 *   blkcg_print_blkgs → (정책의 prfill 콜백) → [__blkg_prfill_u64]
 *     → blkg_dev_name → seq_printf
 */
u64 __blkg_prfill_u64(struct seq_file *sf, struct blkg_policy_data *pd, u64 v)
{
	const char *dname = blkg_dev_name(pd->blkg);
	/* [한국어] blkg 의 NVMe 장치 이름 */

	if (!dname)
	/* [한국어] 장치명이 없으면 출력 불가 */
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
/*
 * [한국어]
 * blkg_conf_init - cgroup 설정 문자열 파싱 컨텍스트를 초기화
 *
 * @ctx:   초기화할 컨텍스트(호출자 스택에 있는 경우가 많다)
 * @input: 사용자가 cgroupfs에 쓴 문자열. 예: "259:0 rbps=1048576 wbps=max"
 * @return: 없음
 *
 * === 왜 3단계(init → open_bdev → prep) 구조인가 ===
 * cgroup 설정 쓰기는 여러 자원을 순서대로 잡아야 한다: 문자열 파싱 →
 * 대상 블록 장치 열기 → 큐 락 획득 → blkg 찾기/생성. 이 과정에서 실패할
 * 수 있는 지점이 여러 곳이라, 어디서 실패하든 이미 잡은 것만 정확히
 * 되돌려야 한다.
 * ctx에 진행 상태를 모아 두고 blkg_conf_exit()이 "채워진 것만" 정리하는
 * 구조로 만들면, 각 단계가 실패 처리를 중복 구현하지 않아도 된다.
 *
 * 이 함수는 그 시작점으로, 구조체를 통째로 0으로 만들고 입력 문자열만
 * 심는다. 지정 초기화자(designated initializer)로 대입하면 나머지 필드가
 * 자동으로 0/NULL이 되어, 나중에 exit이 "NULL이면 건너뛴다" 규칙으로
 * 안전하게 정리할 수 있다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   정책의 write 핸들러(tg_set_limit, iolatency_set_limit 등)
 *     → [blkg_conf_init] → blkg_conf_open_bdev → blkg_conf_prep
 *     → ... → blkg_conf_exit
 */
void blkg_conf_init(struct blkg_conf_ctx *ctx, char *input)
{
	*ctx = (struct blkg_conf_ctx){ .input = input };
	/* [한국어] 입력 문자열 저장; MAJ:MIN 파싱 시작점 */
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
/*
 * [한국어]
 * blkg_conf_open_bdev - 설정 문자열 앞머리의 "MAJ:MIN"을 파싱해 블록 장치를 연다
 *
 * @ctx: blkg_conf_init()으로 초기화된 컨텍스트. 성공 시 ctx->bdev가 채워지고
 *       ctx->body가 MAJ:MIN 뒤의 나머지 문자열을 가리킨다.
 * @return: 0 성공, 음수 errno(형식 오류 -EINVAL, 장치 없음 -ENODEV 등)
 *
 * cgroup의 I/O 설정은 항상 "어느 장치에 대한 설정인가"로 시작한다.
 * 예: "259:0 rbps=1048576"에서 259:0이 /dev/nvme0n1의 major:minor다.
 * 장치 이름이 아니라 번호를 쓰는 이유는 이름이 부팅마다 바뀔 수 있는 반면
 * major:minor는 커널 내부에서 장치를 유일하게 식별하는 값이기 때문이다.
 *
 * 위 영문 주석대로 이 함수는 여러 번 호출해도 안전하다(두 번째부터는 no-op).
 * blkg_conf_prep()이 내부적으로 이 함수를 부르므로, 정책이 blkg 조회 없이
 * 장치만 필요한 경우에만 따로 호출하면 된다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). 장치 열기가 잠들 수 있다.
 *
 * 에러 경로: ctx->bdev는 실패 시 NULL로 남으므로 blkg_conf_exit()이
 * 안전하게 건너뛴다.
 *
 * 호출 체인:
 *   정책의 write 핸들러 또는 blkg_conf_prep → [blkg_conf_open_bdev]
 *     → blkdev_get_by_dev
 */
int blkg_conf_open_bdev(struct blkg_conf_ctx *ctx)
{
	char *input = ctx->input;
	/* [한국어] 파싱 중인 입력 문자열 */
	unsigned int major, minor;
	/* [한국어] NVMe block 장치 major/minor 번호 */
	struct block_device *bdev;
	int key_len;

	if (ctx->bdev)
	/* [한국어] 이미 bdev 가 열린 경우 NOOP */
		return 0;

	/* [한국어] 입력 앞머리의 "MAJ:MIN"을 파싱한다. %n은 "여기까지 몇 글자를
	 * 읽었는가"를 key_len에 기록하는 지시자로, 파싱 후 나머지 문자열의
	 * 시작점을 알아내는 데 쓴다. 반환값 2는 major와 minor 두 개를 모두
	 * 성공적으로 읽었다는 뜻이다(%n은 변환 개수에 세지 않는다). */
	if (sscanf(input, "%u:%u%n", &major, &minor, &key_len) != 2)
		return -EINVAL;

	/* [한국어] 읽은 만큼 포인터를 전진시킨다. */
	input += key_len;
	/* [한국어] 형식 검증 — MAJ:MIN 뒤에는 반드시 공백이 와야 한다.
	 * 이 검사가 없으면 "259:01048576" 같은 입력에서 minor를 01048576으로
	 * 잘못 읽고도 통과해 엉뚱한 장치에 설정이 걸린다. */
	if (!isspace(*input))
		return -EINVAL;
	/* [한국어] 공백을 건너뛰어 정책별 값 부분("rbps=1048576" 등)의 시작을 얻는다. */
	input = skip_spaces(input);

	/* [한국어] major:minor를 dev_t로 조합해 block_device를 얻는다.
	 * _no_open 변형은 "장치를 실제로 여는" 절차(파티션 스캔, holder 등록,
	 * fops->open 호출)를 건너뛰고 참조만 얻는다. 설정을 바꾸려는 것뿐이라
	 * 전체 open 절차가 불필요하고, 그 절차가 잠들거나 다른 락을 잡으면
	 * 여기서 원하지 않는 부작용이 생기기 때문이다. */
	bdev = blkdev_get_no_open(MKDEV(major, minor), false);
	if (!bdev)
		return -ENODEV;
	/* [한국어] 파티션에는 cgroup I/O 설정을 걸 수 없다.
	 * cgroup 제한은 request_queue 단위로 동작하는데, 파티션들은 디스크 하나의
	 * 큐를 공유하므로 "이 파티션만 100MB/s"라는 제한이 성립하지 않는다.
	 * 사용자가 파티션을 지정하면 조용히 디스크 전체에 적용하는 대신
	 * 명시적으로 거부해, 의도와 다른 결과를 막는다. */
	if (bdev_is_partition(bdev)) {
		blkdev_put_no_open(bdev);
		return -ENODEV;
	}

	/* [한국어] rq-qos 정책 목록을 보호하는 뮤텍스. 설정 적용 도중 다른
	 * 스레드가 정책을 붙이거나 떼면 자료구조가 꼬이므로 직렬화한다.
	 * 이 락은 blkg_conf_exit()이 해제한다 — 이 함수가 락을 쥔 채로
	 * 반환하는 계약이다. */
	mutex_lock(&bdev->bd_queue->rq_qos_mutex);
	/* [한국어] 디스크가 아직 살아 있는지 확인한다. NVMe 컨트롤러가 뽑히거나
	 * 네임스페이스가 제거되는 중이면 설정을 걸어도 곧 사라진다.
	 * 락을 잡은 뒤에 확인하는 순서가 중요하다 — 락 밖에서 확인하면
	 * 확인과 사용 사이에 상태가 바뀔 수 있다. */
	if (!disk_live(bdev->bd_disk)) {
		blkdev_put_no_open(bdev);
		mutex_unlock(&bdev->bd_queue->rq_qos_mutex);
		return -ENODEV;
	}

	/* [한국어] 파싱 결과를 컨텍스트에 기록한다. body는 정책이 이어서 파싱할
	 * 값 부분이고, bdev는 대상 장치다. 이 둘이 채워졌다는 사실 자체가
	 * blkg_conf_exit()에게 "여기까지 진행됐으니 이만큼 정리하라"는 신호가 된다. */
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
/*
 * [한국어]
 * blkg_conf_open_bdev_frozen - 장치를 열고 큐를 freeze한 상태로 만든다
 *
 * @ctx: blkg_conf_init()으로 초기화된 컨텍스트
 * @return: 성공 시 memflags(나중에 blkg_conf_exit_frozen()에 그대로 넘겨야 함),
 *          실패 시 음수 errno. __must_check이므로 반환값을 무시하면 컴파일 경고.
 *
 * 일부 정책 설정은 진행 중인 I/O가 없는 상태에서만 안전하게 바꿀 수 있다.
 * 예를 들어 rq-qos 정책을 큐에 붙이거나 떼는 작업은, 그 정책을 참조하는
 * request가 살아 있으면 해제된 자료구조를 건드리게 된다.
 *
 * === 락 순서가 이 함수의 핵심 ===
 * 큐 freeze는 진행 중인 I/O의 완료를 기다리는데, 그 I/O가 rq_qos_mutex를
 * 필요로 할 수 있다. 따라서 rq_qos_mutex를 쥔 채 freeze하면 데드락이다.
 * 그래서 이 함수는 "락 해제 → freeze → 락 재획득" 순서를 밟는다.
 *
 * memflags를 반환하는 이유: freeze 구간에서는 메모리 할당이 I/O를 유발하면
 * 안 되므로 PF_MEMALLOC_NOIO가 설정되는데, 그 이전 상태를 복원하려면
 * 저장해 두어야 한다. 반환값을 잃으면 태스크의 메모리 할당 컨텍스트가
 * 영구히 잘못된 상태로 남는다 — __must_check이 붙은 이유다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). freeze가 잠든다.
 *
 * 호출 체인:
 *   rq-qos 계열 정책의 write 핸들러 → [blkg_conf_open_bdev_frozen]
 *     → blkg_conf_open_bdev → blk_mq_freeze_queue
 *   해제: blkg_conf_exit_frozen(ctx, memflags)
 */
unsigned long __must_check blkg_conf_open_bdev_frozen(struct blkg_conf_ctx *ctx)
{
	int ret;
	unsigned long memflags;

	if (ctx->bdev)
	/* [한국어] 이미 열린 bdev 가 있으면 오류 */
		return -EINVAL;

	ret = blkg_conf_open_bdev(ctx);
	/* [한국어] bdev 열기 및 live 검증 */
	if (ret < 0)
		return ret;
	/*
	 * At this point, we haven’t started protecting anything related to QoS,
	 * so we release q->rq_qos_mutex here, which was first acquired in blkg_
	 * conf_open_bdev. Later, we re-acquire q->rq_qos_mutex after freezing
	 * the queue to maintain the correct locking order.
	 */
	mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);
	/* [한국어] freeze 전 lock 해제; 올바른 lock ordering 유지 */

	memflags = blk_mq_freeze_queue(ctx->bdev->bd_queue);
	/* [한국어] blk-mq queue freeze; QUEUE_FLAG_QUIESCED 와 유사하게 NVMe IO 제출/완료 일시 정지 */
	mutex_lock(&ctx->bdev->bd_queue->rq_qos_mutex);
	/* [한국어] freeze 후 다시 QoS lock 획득 */

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
/*
 * [한국어] (위 영문 kernel-doc 참고)
 * blkg_conf_prep - 설정 문자열을 끝까지 해석해 대상 blkg를 확보하고 락을 잡는다
 *
 * @blkcg: 설정을 적용할 cgroup
 * @pol:   대상 정책(blk-throttle, blk-iolatency 등)
 * @ctx:   blkg_conf_init()으로 초기화된 컨텍스트.
 *         성공 시 ctx->bdev(장치), ctx->blkg(대상 blkg), ctx->body(값 부분
 *         문자열)가 모두 채워진다.
 * @return: 0 성공, 음수 errno
 *
 * 3단계 설정 흐름(init → open_bdev → prep)의 마지막 단계다. 하는 일:
 *   1) 아직 장치를 열지 않았으면 blkg_conf_open_bdev()로 연다.
 *   2) 큐 락을 잡는다(이후 blkg 트리를 안전하게 조회하기 위해).
 *   3) (cgroup, 장치) 조합의 blkg를 찾고, 없으면 새로 만든다.
 *      blkg는 "이 cgroup이 이 장치에 대해 갖는 상태"이므로, 사용자가
 *      처음 설정하는 조합이면 이 시점에 생성된다.
 *
 * 락을 잡은 채로 반환하는 것이 이 함수의 계약이다(__acquires 주석). 호출자는
 * 설정을 적용한 뒤 반드시 blkg_conf_exit()으로 락을 풀어야 한다. 이렇게
 * 설계한 이유는 "blkg를 찾은 시점부터 설정을 적용할 때까지" 그 blkg가
 * 사라지지 않아야 하기 때문이다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). blkg 생성이 잠들 수
 * 있으므로 락을 잡기 전에 미리 할당(preload)하는 패턴을 쓴다.
 *
 * 호출 체인:
 *   정책의 write 핸들러 → [blkg_conf_prep]
 *     → blkg_conf_open_bdev → blkg_lookup_check / blkg_create
 */
int blkg_conf_prep(struct blkcg *blkcg, const struct blkcg_policy *pol,
		   struct blkg_conf_ctx *ctx)
	__acquires(&bdev->bd_queue->queue_lock)
{
	struct gendisk *disk;
	/* [한국어] 대상 NVMe namespace 의 gendisk */
	struct request_queue *q;
	/* [한국어] 대상 NVMe request_queue */
	struct blkcg_gq *blkg;
	/* [한국어] 설정 대상 blkg */
	int ret;

	ret = blkg_conf_open_bdev(ctx);
	/* [한국어] bdev 열기 */
	if (ret)
	/* [한국어] bdev 열기 실패 시 즉시 반환 */
		return ret;

	disk = ctx->bdev->bd_disk;
	/* [한국어] bdev 로부터 gendisk 획득 */
	q = disk->queue;
	/* [한국어] gendisk 의 request_queue 획득; NVMe namespace queue */

	/* Prevent concurrent with blkcg_deactivate_policy() */
	mutex_lock(&q->blkcg_mutex);
	/* [한국어] blkcg_deactivate_policy() 와 동기화 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득; blkg 생성/조회 보호 */

	if (!blkcg_policy_enabled(q, pol)) {
		ret = -EOPNOTSUPP;
		goto fail_unlock;
	}

	blkg = blkg_lookup(blkcg, q);
	/* [한국어] 기존 blkg 검색 */
	if (blkg)
	/* [한국어] 기존 blkg 를 설정 대상으로 사용 */
		goto success;

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.
	 */
	/* [한국어] ★ 왜 루프인가: 부모 blkg가 먼저 존재해야 한다 ★
	 * blkg는 (cgroup × 디스크) 조합마다 하나씩 만들어지는데, 자식 blkg는
	 * 통계 전파와 설정 상속을 위해 부모 blkg를 참조한다. 그런데 사용자가
	 * 중간 cgroup을 건너뛰고 깊은 자식에만 설정을 걸 수 있어, 그 경로의
	 * 부모 blkg들이 아직 없을 수 있다.
	 * 그래서 root 쪽으로 거슬러 올라가 "가장 가까운 없는 조상"부터 하나씩
	 * 만들어 내려온다. 한 번에 하나씩 만드는 이유는 아래에서 보듯 할당을
	 * 위해 락을 놓았다 잡아야 하기 때문이다. */
	while (true) {
		/* [한국어] 이번 반복에서 만들 대상. 아래 탐색으로 조상 쪽으로 밀린다. */
		struct blkcg *pos = blkcg;
		struct blkcg *parent;
		struct blkcg_gq *new_blkg;

		parent = blkcg_parent(blkcg);
		/* [한국어] blkg가 없는 가장 가까운 조상을 찾아 pos를 그쪽으로 옮긴다.
		 * 루프가 끝나면 pos는 "지금 만들어야 할 가장 위쪽 blkg"가 된다.
		 * parent가 NULL이면 root에 도달한 것이고, root blkg는 큐 생성 시
		 * 이미 만들어져 있으므로 탐색이 거기서 멈춘다. */
		while (parent && !blkg_lookup(parent, q)) {
			pos = parent;
			parent = blkcg_parent(parent);
		}

		/* Drop locks to do new blkg allocation with GFP_KERNEL. */
		/* [한국어] 스핀락을 쥔 채로는 잠들 수 있는 할당을 할 수 없으므로
		 * 일시적으로 놓는다. 이 틈에 다른 스레드가 같은 blkg를 만들 수
		 * 있는데, 아래에서 다시 조회해 그 경쟁을 처리한다. */
		spin_unlock_irq(&q->queue_lock);

		/* [한국어] GFP_NOIO로 할당한다. GFP_KERNEL이 아닌 이유: 이 할당이
		 * 메모리 회수를 유발하고 그 회수가 이 디스크로의 write-back을
		 * 필요로 하면, 그 I/O가 다시 blkg를 찾으려다 교착에 빠질 수 있다. */
		new_blkg = blkg_alloc(pos, disk, GFP_NOIO);
		if (unlikely(!new_blkg)) {
			ret = -ENOMEM;
			goto fail_exit;
		}

		/* [한국어] blkg는 radix tree에 등록되는데, 그 삽입이 내부적으로
		 * 노드를 할당할 수 있다. 그런데 삽입은 스핀락 안에서 해야 하므로
		 * 그때는 할당이 불가능하다.
		 * radix_tree_preload()는 미리 per-CPU 캐시에 노드를 채워 두어,
		 * 락 안의 삽입이 할당 없이 성공하도록 보장한다. 이후
		 * radix_tree_preload_end()까지 preemption이 비활성화된다. */
		if (radix_tree_preload(GFP_KERNEL)) {
			blkg_free(new_blkg);
			ret = -ENOMEM;
			goto fail_exit;
		}

		spin_lock_irq(&q->queue_lock);

		/* [한국어] 락을 놓은 사이에 상황이 변했을 수 있다. 정책이 이 큐에서
		 * 비활성화되었다면(다른 스레드가 blkcg_deactivate_policy 실행)
		 * 더 진행할 이유가 없다. 락을 놓았다 잡는 코드에서 이런 재확인은
		 * 선택이 아니라 필수다. */
		if (!blkcg_policy_enabled(q, pol)) {
			blkg_free(new_blkg);
			ret = -EOPNOTSUPP;
			goto fail_preloaded;
		}

		/* [한국어] 같은 이유로 blkg도 다시 조회한다. 락을 놓은 틈에 다른
		 * 스레드가 같은 (cgroup, 디스크) 조합을 이미 만들었을 수 있다. */
		blkg = blkg_lookup(pos, q);
		if (blkg) {
			/* [한국어] 경쟁에서 졌다 — 상대가 만든 것을 쓰고 내가 준비한
			 * 것은 버린다. 오류가 아니라 정상적인 결과다. */
			blkg_free(new_blkg);
		} else {
			/* [한국어] 내가 만든다. blkg_create()가 radix tree와 blkcg의
			 * 리스트에 등록하고, 부모 blkg와의 연결도 맺는다. */
			blkg = blkg_create(pos, disk, new_blkg);
			if (IS_ERR(blkg)) {
				ret = PTR_ERR(blkg);
				goto fail_preloaded;
			}
		}

		/* [한국어] preload 구간을 닫아 preemption을 다시 허용한다.
		 * 이 호출을 빠뜨리면 preemption이 영구히 비활성화되어 시스템이 멈춘다. */
		radix_tree_preload_end();

		/* [한국어] 목표 cgroup까지 도달했으면 완료. 아니면 다음 반복에서
		 * 그다음 자손을 만든다. 매 반복마다 조상이 하나씩 채워지므로
		 * 반드시 유한 횟수 안에 끝난다. */
		if (pos == blkcg)
			goto success;
	}
success:
	mutex_unlock(&q->blkcg_mutex);
	ctx->blkg = blkg;
	/* [한국어] 설정 대상 blkg 확정; blkg_conf_prep() 호출자가 queue_lock 해제 */
	return 0;

fail_preloaded:
	radix_tree_preload_end();
	/* [한국어] preload 상태 정리 */
fail_unlock:
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] queue_lock 해제 */
fail_exit:
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] blkcg_mutex 해제 */
	/*
	 * If queue was bypassing, we should retry.  Do so after a
	 * short msleep().  It isn't strictly necessary but queue
	 * can be bypassing for some time and it's always nice to
	 * avoid busy looping.
	 */
	if (ret == -EBUSY) {
	/* [한국어] queue bypass 중이면 잠시 대기 후 재시도; NVMe reset/recovery 대기 */
		msleep(10);
	/* [한국어] 시스템 콜 재시작 */
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
/*
 * [한국어]
 * blkg_conf_exit - 설정 파싱 과정에서 잡은 자원을 역순으로 모두 반납
 *
 * @ctx: 정리할 컨텍스트
 * @return: 없음
 *
 * blkg_conf_init/open_bdev/prep이 단계적으로 잡은 것들(큐 락, rq_qos_mutex,
 * blkg 참조, block_device 참조)을 반대 순서로 풀어 준다.
 *
 * 이 함수의 설계상 중요한 성질은 "부분적으로 진행된 상태에서도 안전하다"는
 * 것이다. blkg_conf_init()이 구조체를 전부 0으로 만들어 두었기 때문에, 각
 * 필드가 NULL인지 확인하는 것만으로 "그 단계까지 갔는지"를 알 수 있다.
 * 덕분에 어느 단계에서 실패하든 호출자는 이 함수 하나만 부르면 된다.
 *
 * __releases() 주석은 sparse 정적 분석기에게 "이 함수가 락을 해제한 채로
 * 반환한다"고 알리는 표시다. 이것이 없으면 sparse가 락 불균형으로 오인해
 * 경고를 낸다.
 *
 * 실행 컨텍스트: cgroupfs write의 마무리(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   정책의 write 핸들러(성공/실패 무관) → [blkg_conf_exit]
 *     → spin_unlock_irq(queue_lock) → mutex_unlock(rq_qos_mutex)
 *     → blkg_put → blkdev_put
 */
void blkg_conf_exit(struct blkg_conf_ctx *ctx)
	__releases(&ctx->bdev->bd_queue->queue_lock)
	__releases(&ctx->bdev->bd_queue->rq_qos_mutex)
{
	if (ctx->blkg) {
	/* [한국어] blkg_conf_prep() 에서 잡은 queue_lock 해제; NVMe IO 경로 재개 */
		spin_unlock_irq(&bdev_get_queue(ctx->bdev)->queue_lock);
		ctx->blkg = NULL;
	}

	if (ctx->bdev) {
	/* [한국어] QoS lock 해제 */
		mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);
		blkdev_put_no_open(ctx->bdev);
	/* [한국어] bdev 참조 반낑; NVMe namespace bdev */
		ctx->body = NULL;
		ctx->bdev = NULL;
	}
}
EXPORT_SYMBOL_GPL(blkg_conf_exit);

/*
 * Similar to blkg_conf_exit, but also unfreezes the queue. Should be used
 * when blkg_conf_open_bdev_frozen is used to open the bdev.
 */
/*
 * [한국어]
 * blkg_conf_exit_frozen - freeze된 상태로 시작한 설정 작업의 뒷정리
 *
 * @ctx:      정리할 컨텍스트
 * @memflags: blkg_conf_open_bdev_frozen()이 반환했던 값. 반드시 그대로 전달해야 한다.
 * @return: 없음
 *
 * blkg_conf_exit()의 freeze 버전이다. 일반 정리에 더해 큐 unfreeze와
 * 메모리 할당 컨텍스트 복원을 수행한다.
 *
 * 순서가 중요하다: unfreeze를 먼저 해야 그 뒤에 오는 자원 해제 과정에서
 * 메모리 할당이 필요해져도 I/O가 막혀 있지 않다. 반대로 하면 해제 도중
 * 자기가 막아 놓은 큐를 기다리는 데드락이 될 수 있다.
 *
 * 실행 컨텍스트: cgroupfs write의 마무리(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   rq-qos 계열 정책의 write 핸들러 → [blkg_conf_exit_frozen]
 *     → blk_mq_unfreeze_queue(memflags) → blkg_conf_exit
 */
void blkg_conf_exit_frozen(struct blkg_conf_ctx *ctx, unsigned long memflags)
{
	if (ctx->bdev) {
	/* [한국어] 대상 NVMe request_queue */
		struct request_queue *q = ctx->bdev->bd_queue;

		blkg_conf_exit(ctx);
		blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] queue freeze 해제; NVMe IO 제출/완료 재개 */
	}
}

/*
 * [한국어]
 * blkg_iostat_add - 통계 구조체를 항목별로 더한다 (dst += src)
 *
 * @dst: 누적 대상
 * @src: 더할 값
 * @return: 없음
 *
 * 두 곳에서 쓰인다:
 *   1) per-CPU 통계를 blkg 전역 통계로 모을 때 — 각 CPU의 증분을 합산
 *   2) 자식 blkg의 통계를 부모로 전파할 때 — cgroup 트리 상향 누적
 * 둘 다 "지금까지의 합계에 새 값을 더한다"는 같은 연산이라 하나의 함수를 공유한다.
 *
 * 실행 컨텍스트: 통계 집계 경로. 호출자가 u64_stats seqlock 또는 적절한
 * 락으로 보호한다.
 *
 * 호출 체인:
 *   __blkcg_rstat_flush → [blkg_iostat_add]
 */
static void blkg_iostat_add(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* [한국어] read/write/discard 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* [한국어] 세 IO 유형별로 합산 */
		dst->bytes[i] += src->bytes[i];
		/* [한국어] i 유형 바이트 누적 */
		dst->ios[i] += src->ios[i];
		/* [한국어] i 유형 IO 횟수 누적 */
	}
}

/*
 * [한국어]
 * blkg_iostat_sub - 통계 구조체를 항목별로 뺀다 (dst -= src)
 *
 * @dst: 차감 대상
 * @src: 뺄 값
 * @return: 없음
 *
 * "증분"을 계산하는 데 쓴다. per-CPU 통계는 단조 증가하는 누적값이므로,
 * "지난번 전파 이후 얼마나 늘었는가"를 알려면 현재값(cur)에서 마지막
 * 스냅숏(last)을 빼야 한다:
 *   delta = cur - last;  부모에 delta를 더함;  last = cur;
 * 이 패턴 덕분에 같은 값을 두 번 전파하지 않으면서도 CPU별 카운터를
 * 초기화할 필요가 없다.
 *
 * 결과가 음수가 되면 안 되는데, cur >= last가 항상 성립하기 때문이다
 * (통계는 감소하지 않는다). 다만 blkg_clear_stat()으로 리셋할 때 둘을
 * 함께 0으로 맞추는 이유가 바로 이 불변식을 지키기 위해서다.
 *
 * 실행 컨텍스트: 통계 집계 경로.
 *
 * 호출 체인:
 *   __blkcg_rstat_flush → [blkg_iostat_sub]
 */
static void blkg_iostat_sub(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	int i;
	/* [한국어] read/write/discard 항목 순회 */

	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
	/* [한국어] 세 IO 유형별로 차감 */
		dst->bytes[i] -= src->bytes[i];
		/* [한국어] i 유형 바이트 차감 */
		dst->ios[i] -= src->ios[i];
		/* [한국어] i 유형 IO 횟수 차감 */
	}
}

/*
 * [한국어]
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
	/* [한국어] per-cpu 와 last 사이의 delta */
	unsigned long flags;

	/* propagate percpu delta to global */
	flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
	/* [한국어] global 통계 seqlock 진입; NVMe read/write/discard 누적값 보호 */
	blkg_iostat_set(&delta, cur);
	/* [한국어] 현재 per-cpu 값을 delta 로 복사 */
	blkg_iostat_sub(&delta, last);
	/* [한국어] last 를 뺌으로써 실제 delta 산출 */
	blkg_iostat_add(&blkg->iostat.cur, &delta);
	/* [한국어] global cur 에 delta 누적; NVMe namespace 별 cgroup 통계 업데이트 */
	blkg_iostat_add(last, &delta);
	/* [한국어] last 를 현재값으로 갱신; 다음 delta 계산 기준 */
	u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
	/* [한국어] seqlock 해제; NVMe 통계 readers 에게 일관된 값 공개 */
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
 * [한국어]
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
	/* [한국어] 대상 CPU 의 lockless list 헤드 */
	struct llist_node *lnode;
	/* [한국어] lockless list 의 첫 노드 */
	struct blkg_iostat_set *bisc, *next_bisc;
	/* [한국어] 순회 중인 per-cpu 통계 노드와 다음 노드 */
	unsigned long flags;

	rcu_read_lock();
	/* [한국어] blkg 객체 및 계층 포인터 접근을 RCU 로 보호 */

	lnode = llist_del_all(lhead);
	/* [한국어] 해당 CPU 의 lockless list 전체를 분리; NVMe 통계 업데이트 노드들을 한꺼번에 가져옴 */
	if (!lnode)
	/* [한국어] flush 할 통계 노드가 없음 */
		goto out;

	/*
	 * For covering concurrent parent blkg update from blkg_release().
	 *
	 * When flushing from cgroup, the subsystem rstat lock is always held,
	 * so this lock won't cause contention most of time.
	 */
	raw_spin_lock_irqsave(&blkg_stat_lock, flags);
	/* [한국어] 부모 blkg update 와의 경쟁 보호; NVMe 통계 상위 전파 직렬화 */

	/*
	 * Iterate only the iostat_cpu's queued in the lockless list.
	 */
	llist_for_each_entry_safe(bisc, next_bisc, lnode, lnode) {
	/* [한국어] lockless list 의 per-cpu 통계 노드를 순회; NVMe CQ 완료별 누적 처리 */
		struct blkcg_gq *blkg = bisc->blkg;
		/* [한국어] 통계가 속한 blkg; NVMe namespace 와 cgroup 의 연결체 */
		struct blkcg_gq *parent = blkg->parent;
		/* [한국어] 통계를 전파할 부모 blkg */
		struct blkg_iostat cur;
		/* [한국어] per-cpu 통계 스냅샷 */
		unsigned int seq;
		/* [한국어] u64_stats_seqlock 의 sequence 번호 */

		/*
		 * Order assignment of `next_bisc` from `bisc->lnode.next` in
		 * llist_for_each_entry_safe and clearing `bisc->lqueued` for
		 * avoiding to assign `next_bisc` with new next pointer added
		 * in blk_cgroup_bio_start() in case of re-ordering.
		 *
		 * The pair barrier is implied in llist_add() in blk_cgroup_bio_start().
		 */
		smp_mb();
		/* [한국어] llist_for_each_entry_safe 의 next 포인터 로드와 lqueued 클리어 사이의 reordering 방지; NVMe 통계 노드 안전성 */

		WRITE_ONCE(bisc->lqueued, false);
		/* [한국어] 배리어와 함께 list 등록 상태 클리어; 이후 blk_cgroup_bio_start() 에서 재등록 가능 */
		if (bisc == &blkg->iostat)
		/* [한국어] global 통계 노드는 부모로만 전파; per-cpu 통계는 먼저 global 에 합산 */
			goto propagate_up; /* propagate up to parent only */

		/* fetch the current per-cpu values */
		do {
		/* [한국어] u64_stats_seqlock 시작; 32bit 에서 NVMe 통계 reader/writer race 회피 */
			seq = u64_stats_fetch_begin(&bisc->sync);
		/* [한국어] per-cpu 통계 스냅샷 복사 */
			blkg_iostat_set(&cur, &bisc->cur);
		} while (u64_stats_fetch_retry(&bisc->sync, seq));
		/* [한국어] seqlock 갱신 시 재시도; NVMe 통계 일관성 확보 */

		blkcg_iostat_update(blkg, &cur, &bisc->last);
		/* [한국어] per-cpu delta 를 global blkg 통계에 반영 */

propagate_up:
		/* propagate global delta to parent (unless that's root) */
		if (parent && parent->parent) {
		/* [한국어] root 의 직계 자식이 아니면 부모에게 통계 전파; cgroup 계층별 NVMe IO 집계 */
			blkcg_iostat_update(parent, &blkg->iostat.cur,
			/* [한국어] 부모 blkg 의 global 통계에 delta 누적 */
					    &blkg->iostat.last);
			/*
			 * Queue parent->iostat to its blkcg's lockless
			 * list to propagate up to the grandparent if the
			 * iostat hasn't been queued yet.
			 */
			if (!parent->iostat.lqueued) {
			/* [한국어] 부모 통계 노드가 아직 list 에 없으면 등록; 상위 cgroup 으로 재귀 전파 준비 */
				struct llist_head *plhead;

				plhead = per_cpu_ptr(parent->blkcg->lhead, cpu);
				/* [한국어] 부모 cgroup 의 동일 CPU lockless list 헤드 */
				llist_add(&parent->iostat.lnode, plhead);
				/* [한국어] 부모 통계 노드를 lockless list 에 추가; 이후 상위 flush 에 의해 처리 */
				parent->iostat.lqueued = true;
				/* [한국어] 부모 노드 list 등록 상태 표시 */
			}
		}
	}
	raw_spin_unlock_irqrestore(&blkg_stat_lock, flags);
	/* [한국어] blkg 통계 전파 lock 해제 */
out:
	rcu_read_unlock();
	/* [한국어] RCU read-side 종료 */
}

/*
 * [한국어]
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
		/* [한국어] 특정 cgroup 의 CPU 별 NVMe 통계를 global 로 반영 */
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
 * [한국어]
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
	/* [한국어] block_class 장치 순회자 */
	struct device *dev;
	/* [한국어] 순회 중인 block 장치 */

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
	/* [한국어] 시스템의 모든 block 장치(NVMe namespace 포함)를 순회 */
		struct block_device *bdev = dev_to_bdev(dev);
		struct blkcg_gq *blkg = bdev->bd_disk->queue->root_blkg;
		/* [한국어] 장치에 해당하는 block_device */
		struct blkg_iostat tmp;
		/* [한국어] 해당 NVMe namespace 의 root blkg */
		int cpu;
		/* [한국어] disk_stats 누적 임시 버퍼 */
		unsigned long flags;
		/* [한국어] per-cpu disk_stats 순회 */

		/* [한국어] u64_stats_update irqsave 플래그 */
		memset(&tmp, 0, sizeof(tmp));
		/* [한국어] 누적 버퍼 초기화 */
		for_each_possible_cpu(cpu) {
		/* [한국어] 모든 CPU 의 disk_stats 를 합산; NVMe 멀티 코어 완료 통계 집계 */
			struct disk_stats *cpu_dkstats;

			cpu_dkstats = per_cpu_ptr(bdev->bd_stats, cpu);
			/* [한국어] CPU 별 disk_stats 획득 */
			tmp.ios[BLKG_IOSTAT_READ] +=
			/* [한국어] read IO 횟수 누적; NVMe read opcode 와 대응 */
				cpu_dkstats->ios[STAT_READ];
			tmp.ios[BLKG_IOSTAT_WRITE] +=
			/* [한국어] write IO 횟수 누적; NVMe write opcode 와 대응 */
				cpu_dkstats->ios[STAT_WRITE];
			tmp.ios[BLKG_IOSTAT_DISCARD] +=
			/* [한국어] discard IO 횟수 누적; NVMe DSM/discard 와 대응 */
				cpu_dkstats->ios[STAT_DISCARD];
			// convert sectors to bytes
			tmp.bytes[BLKG_IOSTAT_READ] +=
			/* [한국어] sector(512B) 를 byte 로 변환해 read 바이트 누적 */
			/* [한국어] sector(512B) 를 byte 로 변환해 누적 */
				cpu_dkstats->sectors[STAT_READ] << 9;
			/* [한국어] write 바이트 누적; NVMe PRP/SGL 로 전송된 총량(추정) */
			tmp.bytes[BLKG_IOSTAT_WRITE] +=
				cpu_dkstats->sectors[STAT_WRITE] << 9;
			/* [한국어] discard 바이트 누적; NVMe DSM range 와 대응 */
			tmp.bytes[BLKG_IOSTAT_DISCARD] +=
				cpu_dkstats->sectors[STAT_DISCARD] << 9;
		}

		flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
		/* [한국어] root blkg global 통계 seqlock 진입 */
		blkg_iostat_set(&blkg->iostat.cur, &tmp);
		/* [한국어] 집계된 disk_stats 를 root blkg 통계로 복사 */
		u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
		/* [한국어] seqlock 해제 */
	}
	class_dev_iter_exit(&iter);
	/* [한국어] 장치 순회 종료 */
}

/*
 * [한국어]
 * blkcg_print_one_stat - blkg 하나의 io.stat 한 줄을 출력
 *
 * @blkg: 출력할 blkcg_gq
 * @s:    출력 대상 seq_file
 * @return: 없음
 *
 * cgroup v2의 io.stat 파일에서 한 장치에 해당하는 줄을 만든다. 출력 형식:
 *   259:0 rbytes=... wbytes=... rios=... wios=... dbytes=... dios=...
 *
 * 값을 읽을 때 u64_stats_fetch_begin/retry 루프를 쓰는 이유: 통계는
 * per-CPU에서 갱신되는 u64 값이라, 32비트 아키텍처에서는 상위/하위 32비트가
 * 따로 쓰여 그 사이에 읽으면 앞뒤가 섞인 값을 본다. seqlock이 갱신 중임을
 * 감지하면 읽기를 재시도시킨다.
 *
 * 등록된 정책들의 pd_stat_fn 콜백도 함께 호출해, 정책별 추가 통계
 * (blk-iocost의 cost.stat 등)를 같은 줄에 이어 붙인다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cgroupfs read(io.stat) → blkcg_print_stat → [blkcg_print_one_stat]
 *     → u64_stats_fetch_begin/retry → 정책의 pd_stat_fn
 */
static void blkcg_print_one_stat(struct blkcg_gq *blkg, struct seq_file *s)
{
	struct blkg_iostat_set *bis = &blkg->iostat;
	/* [한국어] 출력할 blkg 의 global 통계 세트 */
	u64 rbytes, wbytes, rios, wios, dbytes, dios;
	/* [한국어] read/write/discard 의 bytes/ios 스냅샷 */
	const char *dname;
	/* [한국어] NVMe 장치 이름 */
	unsigned seq;
	/* [한국어] u64_stats_seqlock sequence */
	int i;

	if (!blkg->online)
	/* [한국어] offline blkg 는 통계 미출력; 제거 중인 NVMe cgroup */
		return;

	dname = blkg_dev_name(blkg);
	/* [한국어] 장치명 획득 */
	if (!dname)
	/* [한국어] 장치명 없으면 출력 불가 */
		return;

	seq_printf(s, "%s ", dname);

	do {
	/* [한국어] u64_stats_seqlock 시작; NVMe 통계 reader/writer race 회피 */
		seq = u64_stats_fetch_begin(&bis->sync);
		/* [한국어] read/write/discard 바이트 스냅샷 */

		rbytes = bis->cur.bytes[BLKG_IOSTAT_READ];
		wbytes = bis->cur.bytes[BLKG_IOSTAT_WRITE];
		dbytes = bis->cur.bytes[BLKG_IOSTAT_DISCARD];
		rios = bis->cur.ios[BLKG_IOSTAT_READ];
		wios = bis->cur.ios[BLKG_IOSTAT_WRITE];
		dios = bis->cur.ios[BLKG_IOSTAT_DISCARD];
	} while (u64_stats_fetch_retry(&bis->sync, seq));
	/* [한국어] seqlock 갱신 시 재시도; NVMe 통계 일관성 확보 */

	if (rbytes || wbytes || rios || wios) {
	/* [한국어] 통계가 0 이 아닐 때만 출력; NVMe IO 가 실제 발생한 장치 */
		seq_printf(s, "rbytes=%llu wbytes=%llu rios=%llu wios=%llu dbytes=%llu dios=%llu",
			rbytes, wbytes, rios, wios,
			dbytes, dios);
	}

	if (blkcg_debug_stats && atomic_read(&blkg->use_delay)) {
	/* [한국어] debug 모드에서 use_delay/delay_nsec 출력; NVMe queue 지연/스로틀 상태 */
		seq_printf(s, " use_delay=%d delay_nsec=%llu",
			atomic_read(&blkg->use_delay),
			atomic64_read(&blkg->delay_nsec));
	}

	for (i = 0; i < BLKCG_MAX_POLS; i++) {
	/* [한국어] 등록된 정책별 추가 통계 출력; throtl/BFQ/ioprio NVMe 상태 */
		struct blkcg_policy *pol = blkcg_policy[i];

		if (!blkg->pd[i] || !pol->pd_stat_fn)
		/* [한국어] pd 없거나 stat 콜백 없으면 스킵 */
			continue;

		pol->pd_stat_fn(blkg->pd[i], s);
	}

	seq_puts(s, "\n");
}

/*
 * [한국어]
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
	/* [한국어] 출력 대상 cgroup */
	struct blkcg_gq *blkg;
	/* [한국어] 순회 중인 blkg */

	/* [한국어] ★ root와 non-root의 통계 출처가 다르다 ★
	 * root cgroup은 "cgroup에 속하지 않은 I/O를 포함한 전부"를 보여야 한다.
	 * 그런데 blkg 통계는 cgroup을 명시적으로 거친 I/O만 집계하므로, 커널
	 * 스레드가 낸 I/O 등이 빠진다. 그래서 root는 blkg 통계 대신
	 * /proc/diskstats와 같은 소스(디스크별 part_stat)에서 채운다. */
	if (!seq_css(sf)->parent)
		blkcg_fill_root_iostats();
	else
		/* [한국어] non-root는 per-CPU rstat에 흩어져 누적된 통계를 먼저
		 * 상위로 flush한다. cgroup의 rstat 인프라는 갱신 비용을 줄이려고
		 * CPU마다 값을 쌓아 두고, 읽을 때만 트리를 따라 합산한다.
		 * 이 호출이 없으면 방금 발생한 I/O가 통계에 반영되지 않는다. */
		css_rstat_flush(&blkcg->css);

	/* [한국어] blkg 목록은 RCU로 보호된다. 순회 도중 다른 스레드가 blkg를
	 * 제거할 수 있는데, RCU 유예 해제 덕분에 이 구간에서는 안전하게 읽는다. */
	rcu_read_lock();
	/* [한국어] 이 cgroup에 속한 모든 blkg를 순회한다. blkg는 (cgroup × 디스크)
	 * 조합이므로, 시스템에 NVMe 네임스페이스가 여러 개면 각각에 대해
	 * 한 줄씩 출력된다. */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
		/* [한국어] 큐 락을 잡는다. blkcg_print_one_stat()이 정책별 통계
		 * (pd_stat_fn)까지 출력하는데, 그 정책 데이터가 큐 락으로 보호되기
		 * 때문이다. blkg마다 잡았다 놓아, 한 blkg 출력이 다른 디스크의
		 * I/O를 막지 않게 한다. */
		spin_lock_irq(&blkg->q->queue_lock);
		blkcg_print_one_stat(blkg, sf);
		spin_unlock_irq(&blkg->q->queue_lock);
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
/*
 * [한국어]
 * blkcg_get_cgwb_list - 이 cgroup에 속한 cgroup writeback 구조체 목록을 반환
 *
 * @css: blkcg의 cgroup_subsys_state
 * @return: blkcg->cgwb_list의 주소(항상 유효)
 *
 * cgroup writeback(cgwb)은 "어느 cgroup의 더티 페이지를 어느 장치로
 * write-back할 것인가"를 cgroup별로 분리해 관리하는 메커니즘이다. 그
 * 구조체들은 blkcg마다 리스트로 매달려 있고, mm 계층(mm/backing-dev.c)이
 * 그 리스트를 순회해야 할 때가 있다.
 *
 * 이 함수가 존재하는 이유는 계층 분리다. mm 계층은 struct blkcg의 정의를
 * 알지 못하므로(블록 계층 내부 타입), css 포인터만 넘겨받아 리스트 주소를
 * 얻는 접근자를 블록 계층이 제공한다.
 *
 * 실행 컨텍스트: mm의 writeback 경로. 리스트 자체의 동기화는 호출자가
 * blkcg->lock 등으로 처리한다.
 *
 * 호출 체인:
 *   mm/backing-dev.c의 cgwb 정리 경로 → [blkcg_get_cgwb_list]
 */
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
 * [한국어]
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
	/* [한국어] cond_resched() 사용 가능 표시 */

	spin_lock_irq(&blkcg->lock);
	/* [한국어] blkcg 의 blkg_list 보호 */

	while (!hlist_empty(&blkcg->blkg_list)) {
	/* [한국어] 모든 blkg 가 제거될 때까지 반복 */
		struct blkcg_gq *blkg = hlist_entry(blkcg->blkg_list.first,
		/* [한국어] blkg_list 의 첫 번째 blkg 획득 */
						struct blkcg_gq, blkcg_node);
		struct request_queue *q = blkg->q;
		/* [한국어] blkg 가 속한 NVMe request_queue */

		if (need_resched() || !spin_trylock(&q->queue_lock)) {
		/* [한국어] 스케줄링 필요 또는 queue_lock 획득 실패 시 락 해제 후 재시도; softlockup 방지 */
			/*
			 * Given that the system can accumulate a huge number
			 * of blkgs in pathological cases, check to see if we
			 * need to rescheduling to avoid softlockup.
			 */
			spin_unlock_irq(&blkcg->lock);
			/* [한국어] 스케줄링 양보 */
			cond_resched();
			spin_lock_irq(&blkcg->lock);
			/* [한국어] blkg_list 가 변경되었을 수 있으므로 처음부터 재시도 */
			continue;
		}

		blkg_destroy(blkg);
		/* [한국어] blkg 제거 및 refcnt 종료; NVMe IO 는 root blkg 로 spill */
		spin_unlock(&q->queue_lock);
		/* [한국어] queue_lock 해제 */
	}

	spin_unlock_irq(&blkcg->lock);
	/* [한국어] blkcg lock 해제 */
}

/**
 * blkcg_pin_online - pin online state
 * @blkcg_css: blkcg of interest
 *
 * While pinned, a blkcg is kept online.  This is primarily used to
 * impedance-match blkg and cgwb lifetimes so that blkg doesn't go offline
 * while an associated cgwb is still active.
 */
/*
 * [한국어]
 * blkcg_pin_online - blkcg가 offline 처리되지 않도록 참조를 건다
 *
 * @blkcg_css: 고정할 blkcg의 css
 * @return: 없음
 *
 * === online_pin이 필요한 이유 ===
 * 사용자가 cgroup 디렉터리를 지우면 cgroup 코어가 offline 절차를 시작한다.
 * 그런데 그 시점에 아직 그 cgroup에 속한 write-back I/O가 남아 있을 수 있다.
 * offline이 진행되면 정책 데이터가 해제되어, 뒤늦게 완료되는 I/O가 이미
 * 사라진 자료구조를 참조하게 된다.
 *
 * online_pin은 그것을 막는 별도의 참조 카운터다. 일반적인 css 참조와 달리
 * "구조체가 살아 있는가"가 아니라 "offline 콜백을 미룰 것인가"를 제어한다.
 * blkcg_unpin_online()으로 마지막 참조가 풀릴 때 비로소 실제 offline
 * 처리(정책 데이터 해제)가 진행된다.
 *
 * 주 사용처는 cgroup writeback으로, 더티 페이지가 남아 있는 동안 cgroup을
 * 붙잡아 둔다.
 *
 * 실행 컨텍스트: 어디서든(refcount 원자적 증가).
 *
 * 호출 체인:
 *   mm의 cgroup writeback 초기화 → [blkcg_pin_online]
 *   해제: blkcg_unpin_online → blkcg_css_offline 실제 수행
 */
void blkcg_pin_online(struct cgroup_subsys_state *blkcg_css)
{
	refcount_inc(&css_to_blkcg(blkcg_css)->online_pin);
	/* [한국어] online_pin 증가; cgroup 이 NVMe blkg 제거 지연 */
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
	/* [한국어] unpin 할 cgroup */

	do {
		struct blkcg *parent;

		if (!refcount_dec_and_test(&blkcg->online_pin))
		/* [한국어] pin 카운트가 아직 남아있으면 제거 대기 */
			break;

		parent = blkcg_parent(blkcg);
		/* [한국어] 부모 cgroup; offline 은 root 방향으로 진행 */
		blkcg_destroy_blkgs(blkcg);
		/* [한국어] 이 cgroup 의 모든 NVMe blkg 제거 */
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
 * [한국어]
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
	/* [한국어] writeback 종료 후 blkg 파괴 단계로 진행 */

	/* put the base online pin allowing step 2 to be triggered */
	blkcg_unpin_online(css);
	/* [한국어] online_pin 을 낮춰 blkg 제거 트리거 */
}

/*
 * [한국어]
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
	/* [한국어] 해제할 cgroup */
	int i;

	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블과 cpd 해제 동기화 */

	list_del(&blkcg->all_blkcgs_node);
	/* [한국어] 시스템 전체 blkcg 리스트에서 제거 */

	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* [한국어] per-cgroup 정책 데이터(cpds) 해제 */
		if (blkcg->cpd[i])
		/* [한국어] cpd 가 할당되어 있으면 해제 */
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);

	mutex_unlock(&blkcg_pol_mutex);

	free_percpu(blkcg->lhead);
	/* [한국어] per-cpu lockless 통계 리스트 반낑 */
	kfree(blkcg);
	/* [한국어] blkcg 객체 반낑 */
}

static struct cgroup_subsys_state *
/*
 * [한국어]
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
	/* [한국어] 할당/초기화될 blkcg */
	int i;

	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블 보호 */

	if (!parent_css) {
		/* [한국어] 최상위 root cgroup 은 정적 blkcg_root 사용 */
		blkcg = &blkcg_root;
	} else {
		/* [한국어] 일반 cgroup 용 blkcg 동적 할당 */
		blkcg = kzalloc_obj(*blkcg);
		/* [한국어] root 가 아닌 cgroup 에 대한 blkcg 메모리 동적 할당; NVMe cgroup 트리의 신규 노드 */
		/* [한국어] 일반 cgroup 용 blkcg 동적 할당 */
		if (!blkcg)
			goto unlock;
	}

	if (init_blkcg_llists(blkcg))
	/* [한국어] per-cpu lockless 통계 리스트 초기화 실패 시 롤백 */
	/* [한국어] per-cpu lockless 통계 리스트 초기화 */
		goto free_blkcg;

	for (i = 0; i < BLKCG_MAX_POLS ; i++) {
	/* [한국어] 등록된 정책별 cpd 할당 */
		struct blkcg_policy *pol = blkcg_policy[i];
		struct blkcg_policy_data *cpd;

		/*
		 * If the policy hasn't been attached yet, wait for it
		 * to be attached before doing anything else. Otherwise,
		 * check if the policy requires any specific per-cgroup
		 * data: if it does, allocate and initialize it.
		 */
		if (!pol || !pol->cpd_alloc_fn)
		/* [한국어] 정책 미등록 또는 cpd 필요 없음 */
			continue;

		cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		/* [한국어] per-cgroup 정책 데이터 할당; throtl/BFQ/ioprio 전역 상태 */
		if (!cpd)
		/* [한국어] cpd 할당 실패 시 롤백 */
			goto free_pd_blkcg;

		blkcg->cpd[i] = cpd;
		/* [한국어] blkcg 에 정책 데이터 연결 */
		cpd->blkcg = blkcg;
		/* [한국어] cpd 가 역참조할 blkcg 설정 */
		cpd->plid = i;
		/* [한국어] policy id 기록 */
	}

	spin_lock_init(&blkcg->lock);
	/* [한국어] blkcg lock 초기화 */
	refcount_set(&blkcg->online_pin, 1);
	/* [한국어] 초기 online_pin 설정; cgroup 온라인 상태 유지 */
	INIT_RADIX_TREE(&blkcg->blkg_tree, GFP_NOWAIT);
	/* [한국어] queue id -> blkg radix tree 초기화 */
	INIT_HLIST_HEAD(&blkcg->blkg_list);
	/* [한국어] blkg list 초기화; NVMe namespace 별 blkg 들의 RCU list */
#ifdef CONFIG_CGROUP_WRITEBACK
	INIT_LIST_HEAD(&blkcg->cgwb_list);
#endif
	list_add_tail(&blkcg->all_blkcgs_node, &all_blkcgs);
	/* [한국어] 시스템 전체 blkcg 리스트에 추가 */

	mutex_unlock(&blkcg_pol_mutex);
	return &blkcg->css;

free_pd_blkcg:
	for (i--; i >= 0; i--)
	/* [한국어] 할당 실패 시 역순으로 cpd 해제 */
		if (blkcg->cpd[i])
		/* [한국어] cpd 가 할당되어 있으면 해제 */
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);
	free_percpu(blkcg->lhead);
	/* [한국어] per-cpu lockless list 반낑 */
free_blkcg:
	if (blkcg != &blkcg_root)
	/* [한국어] 동적 할당된 blkcg 만 해제 */
		kfree(blkcg);
unlock:
	mutex_unlock(&blkcg_pol_mutex);
	return ERR_PTR(-ENOMEM);
}

/*
 * [한국어]
 * blkcg_css_online - cgroup online 콜백
 *
 * 호출 경로: cgroup online -> blkcg_css_online()
 * NVMe 연결점: 부모 cgroup 을 pin 하여 offline 이 항상 root 방향으로
 *   진행되도록 한다. NVMe IO 흐름에서 cgroup 계층의 수명을 안정적으로 만든다.
 */

static int blkcg_css_online(struct cgroup_subsys_state *css)
{
	struct blkcg *parent = blkcg_parent(css_to_blkcg(css));
	/* [한국어] 부모 cgroup */

	/*
	 * blkcg_pin_online() is used to delay blkcg offline so that blkgs
	 * don't go offline while cgwbs are still active on them.  Pin the
	 * parent so that offline always happens towards the root.
	 */
	if (parent)
	/* [한국어] 부모 online_pin 증가; NVMe cgroup 계층 수명 안정성 확보 */
		blkcg_pin_online(&parent->css);
	return 0;
}

/*
 * [한국어]
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
	/* [한국어] 이 queue 의 blkg list 초기화 */
	mutex_init(&q->blkcg_mutex);
	/* [한국어] blkg 생성/해제와 policy deactivate 동기화 mutex 초기화 */
}

/*
 * [한국어]
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
	/* [한국어] root blkg 를 생성할 NVMe request_queue */
	struct blkcg_gq *new_blkg, *blkg;
	/* [한국어] 새 blkg 및 등록 결과 */
	bool preloaded;
	/* [한국어] radix tree preload 상태 */

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
	/* [한국어] 이전 disk 의 root_blkg 정리가 끝날 때까지 대기; NVMe namespace rebind 안전성 */
	/* [한국어] 이전 disk 의 root_blkg 정리가 끝날 때까지 대기 */

	new_blkg = blkg_alloc(&blkcg_root, disk, GFP_KERNEL);
	/* [한국어] root cgroup 용 blkg 할당 */
	if (!new_blkg)
	/* [한국어] root blkg 할당 실패 시 NVMe namespace 초기화 실패 */
		return -ENOMEM;

	preloaded = !radix_tree_preload(GFP_KERNEL);
	/* [한국어] radix tree 삽입을 위한 preload 수행 */

	/* Make sure the root blkg exists. */
	/* spin_lock_irq can serve as RCU read-side critical section. */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득 */
	blkg = blkg_create(&blkcg_root, disk, new_blkg);
	/* [한국어] root blkg 생성 및 등록 */
	if (IS_ERR(blkg))
	/* [한국어] root blkg 생성 실패 */
		goto err_unlock;
	q->root_blkg = blkg;
	/* [한국어] queue 의 root blkg 설정; bio 제출의 최종 fallback */
	spin_unlock_irq(&q->queue_lock);

	if (preloaded)
	/* [한국어] radix tree preload 종료 */
		radix_tree_preload_end();

	return 0;

err_unlock:
	/* [한국어] queue_lock 해제 */
	spin_unlock_irq(&q->queue_lock);
	if (preloaded)
	/* [한국어] 실패 시에도 preload 종료 */
		radix_tree_preload_end();
	return PTR_ERR(blkg);
}

/*
 * [한국어]
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
	/* [한국어] 모든 blkg 제거; NVMe queue 의 cgroup 분류 체계 소멸 */
	blk_throtl_exit(disk);
	/* [한국어] throtl 자원 정리; NVMe queue depth throttle 상태 해제 */
}

/*
 * [한국어]
 * blkcg_exit - 태스크가 종료될 때 남은 블록 계층 스로틀 상태를 정리
 *
 * @tsk: 종료 중인 태스크
 * @return: 없음
 *
 * cgroup_subsys의 exit 콜백으로 등록되어, 태스크가 사라질 때 호출된다.
 *
 * 정리 대상은 tsk->throttle_disk다. blk-throttle이나 blk-iocost가 이 태스크를
 * 스로틀하기로 결정하면, "스케줄 아웃될 때 지연을 부과하라"는 표시로 대상
 * 디스크를 태스크에 매달아 둔다(blkcg_schedule_throttle). 그런데 지연이
 * 부과되기 전에 태스크가 종료되면 그 참조가 남아 디스크가 해제되지 못한다.
 *
 * 여기서 참조를 놓고 포인터를 지워 그 누수를 막는다. 태스크가 이미 죽는
 * 중이므로 지연을 실제로 부과할 필요는 없다.
 *
 * 실행 컨텍스트: 태스크 종료 경로(do_exit). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   do_exit → cgroup_exit → io_cgrp_subsys.exit == [blkcg_exit]
 *     → put_disk
 */
static void blkcg_exit(struct task_struct *tsk)
{
	/* [한국어] 스로틀 예약이 걸려 있었다면 그 디스크 참조를 반납한다.
	 * NULL이면 스로틀 대상이 아니었으므로 할 일이 없다. */
	if (tsk->throttle_disk)
		put_disk(tsk->throttle_disk);
	/* [한국어] 포인터를 지운다. 태스크 구조체가 재사용될 수 있으므로
	 * 해제된 주소를 남기면 안 된다. */
	tsk->throttle_disk = NULL;
}

/*
 * [한국어] blk-cgroup을 cgroup 코어에 등록하는 서브시스템 기술자.
 * cgroup 코어가 cgroup 생성/삭제/태스크 이동 시 여기 등록된 콜백을 호출한다.
 * 이 구조체가 곧 "cgroup의 io 컨트롤러"의 정의다.
 */
struct cgroup_subsys io_cgrp_subsys = {
	/* [한국어] cgroup 디렉터리가 만들어질 때 struct blkcg를 할당한다.
	 * 이 시점에는 아직 어떤 디스크와도 연결되지 않는다 — blkg는 실제로
	 * 그 cgroup의 I/O가 발생하거나 설정이 걸릴 때 만들어진다. */
	.css_alloc = blkcg_css_alloc,
	/* [한국어] 할당 후 cgroup을 실제로 사용 가능하게 만드는 단계.
	 * 정책별 cpd(cgroup policy data) 초기화가 여기서 이뤄진다. */
	.css_online = blkcg_css_online,
	/* [한국어] cgroup 디렉터리가 지워질 때 호출. 다만 online_pin 참조가
	 * 남아 있으면(cgroup writeback이 더티 페이지를 들고 있는 등) 실제
	 * 정리는 그 참조가 풀릴 때까지 미뤄진다(blkcg_pin_online 참고). */
	.css_offline = blkcg_css_offline,
	/* [한국어] 모든 참조가 사라진 뒤 struct blkcg를 해제한다. */
	.css_free = blkcg_css_free,
	/* [한국어] ★ 통계 집계의 핵심 ★
	 * cgroup의 rstat 인프라가 "이 cgroup의 per-CPU 통계를 상위로 올려라"고
	 * 요청할 때 호출된다. I/O 완료 경로는 per-CPU 카운터만 갱신하고(락 없음),
	 * 실제 트리 합산은 io.stat을 읽을 때 이 콜백으로 지연 수행된다.
	 * 이 분리 덕분에 완료 경로가 cgroup 트리 락 경합에서 자유롭다. */
	.css_rstat_flush = blkcg_rstat_flush,
	/* [한국어] cgroup v2에서 노출할 파일 목록(io.stat, io.max, io.weight 등). */
	.dfl_cftypes = blkcg_files,
	/* [한국어] cgroup v1에서 노출할 파일 목록. v1은 blkio.* 이름을 쓴다. */
	.legacy_cftypes = blkcg_legacy_files,
	/* [한국어] v1에서의 서브시스템 이름. v2에서는 "io"지만 v1 호환을 위해
	 * "blkio"라는 옛 이름을 유지한다. */
	.legacy_name = "blkio",
	/* [한국어] 태스크 종료 시 스로틀 예약 정리(위 blkcg_exit). */
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
 * [한국어]
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
	/* [한국어] 정책을 활성화할 NVMe request_queue */
	struct blkg_policy_data *pd_prealloc = NULL;
	/* [한국어] GFP_KERNEL 로 미리 할당한 pd; 락 해제 후 재시도용 */
	struct blkcg_gq *blkg, *pinned_blkg = NULL;
	/* [한국어] 순회 중인 blkg 및 미리 할당된 blkg */
	unsigned int memflags;
	/* [한국어] blk_mq_freeze_queue 의 memalloc 상태 저장 */
	int ret;

	if (blkcg_policy_enabled(q, pol))
	/* [한국어] 이미 활성화된 정책은 중복 활성화하지 않음 */
		return 0;

	/*
	 * Policy is allowed to be registered without pd_alloc_fn/pd_free_fn,
	 * for example, ioprio. Such policy will work on blkcg level, not disk
	 * level, and don't need to be activated.
	 */
	if (WARN_ON_ONCE(!pol->pd_alloc_fn || !pol->pd_free_fn))
	/* [한국어] alloc/free 함수 쌍이 맞아야 메모리 누수/부패 방지 */
		return -EINVAL;

	if (queue_is_mq(q))
	/* [한국어] NVMe queue freeze 해제 */
	/* [한국어] NVMe 는 일반적으로 blk-mq 이므로 queue freeze */
		memflags = blk_mq_freeze_queue(q);
		/* [한국어] blk-mq queue freeze; NVMe IO 제출/완료 일시 정지 */
retry:
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득; blkg_list 순회 보호 */

	/* blkg_list is pushed at the head, reverse walk to initialize parents first */
	list_for_each_entry_reverse(blkg, &q->blkg_list, q_node) {
	/* [한국어] 부모 blkg 부터 먼저 초기화하기 위해 역순 순회 */
		struct blkg_policy_data *pd;
		/* [한국어] 할당될 정책별 private data */

		if (blkg->pd[pol->plid])
		/* [한국어] 이미 pd 가 있으면 스킵 */
			continue;

		/* If prealloc matches, use it; otherwise try GFP_NOWAIT */
		if (blkg == pinned_blkg) {
		/* [한국어] 미리 GFP_KERNEL 로 할당해 둔 pd 사용 */
			pd = pd_prealloc;
			pd_prealloc = NULL;
		} else {
			pd = pol->pd_alloc_fn(disk, blkg->blkcg,
		/* [한국어] 락을 잡은 상태에서 빠른 pd 할당 시도 */
					      GFP_NOWAIT);
		}

		if (!pd) {
		/* [한국어] GFP_NOWAIT 실패; 락을 풀고 GFP_KERNEL 로 재시도 */
			/*
			 * GFP_NOWAIT failed.  Free the existing one and
			 * prealloc for @blkg w/ GFP_KERNEL.
			 */
			if (pinned_blkg)
			/* [한국어] 이전 pinned blkg 참조 반낑 */
				blkg_put(pinned_blkg);
			blkg_get(blkg);
			/* [한국어] 현재 blkg 참조 획득; 락 해제 후에도 유효 */
			pinned_blkg = blkg;

			/* [한국어] 락 해제 후 이 blkg 용 pd 를 재할당 */
			spin_unlock_irq(&q->queue_lock);
			/* [한국어] GFP_KERNEL 할당을 위해 queue_lock 해제 */

			if (pd_prealloc)
			/* [한국어] 이전 prealloc 해제 */
				pol->pd_free_fn(pd_prealloc);
			pd_prealloc = pol->pd_alloc_fn(disk, blkg->blkcg,
			/* [한국어] GFP_KERNEL 로 pd 재할당 */
						       GFP_KERNEL);
			if (pd_prealloc)
			/* [한국어] 성공하면 다시 queue_lock 잡고 등록 */
				goto retry;
			else
				goto enomem;
		}

		spin_lock(&blkg->blkcg->lock);
		/* [한국어] blkg 등록/online 시 blkcg lock 추가 획득 */

		pd->blkg = blkg;
		/* [한국어] pd 가 역참조할 blkg 설정 */
		pd->plid = pol->plid;
		/* [한국어] policy id 설정 */
		blkg->pd[pol->plid] = pd;
		/* [한국어] blkg 에 정책 데이터 연결; nvme_queue_rq() 에서 참조 가능 */

		if (pol->pd_init_fn)
		/* [한국어] pd 초기화 콜백 */
			pol->pd_init_fn(pd);

		if (pol->pd_online_fn)
		/* [한국어] pd online 콜백; IO 경로 활성화 */
			pol->pd_online_fn(pd);
		pd->online = true;
		/* [한국어] pd online 상태 표시 */

		spin_unlock(&blkg->blkcg->lock);
	}

	__set_bit(pol->plid, q->blkcg_pols);
	/* [한국어] 이 queue(NVMe namespace)에서 해당 cgroup 정책 활성화 표시 */
	/* [한국어] 이 queue 에서 해당 cgroup 정책 활성화 표시 */
	ret = 0;

	spin_unlock_irq(&q->queue_lock);
	/* [한국어] queue_lock 해제 */
out:
	if (queue_is_mq(q))
		blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] queue freeze 해제; NVMe IO 제출/완료 재개 */
	if (pinned_blkg)
	/* [한국어] pinned blkg 참조 반낑 */
		blkg_put(pinned_blkg);
	if (pd_prealloc)
	/* [한국어] 미사용 prealloc pd 해제 */
		pol->pd_free_fn(pd_prealloc);
	return ret;

enomem:
	/* alloc failed, take down everything */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] 할당 실패 시 롤백; queue_lock 재획득 */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* [한국어] 이미 추가된 pd 들을 정순으로 제거 */
		struct blkcg *blkcg = blkg->blkcg;
		struct blkg_policy_data *pd;

		spin_lock(&blkcg->lock);
		/* [한국어] blkcg lock 획득 */
		pd = blkg->pd[pol->plid];
		/* [한국어] 제거할 정책 데이터 */
		if (pd) {
			if (pd->online && pol->pd_offline_fn)
		/* [한국어] online 상태면 offline 처리 */
				pol->pd_offline_fn(pd);
			pd->online = false;
		/* [한국어] pd offline 표시 */
			pol->pd_free_fn(pd);
		/* [한국어] pd 메모리 해제 */
			blkg->pd[pol->plid] = NULL;
		/* [한국어] blkg 에서 pd 연결 제거; nvme_queue_rq() 가 참조하지 않음 */
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
 * [한국어]
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
	/* [한국어] 정책을 비활성화할 NVMe request_queue */
	struct blkcg_gq *blkg;
	/* [한국어] 순회 중인 blkg */
	unsigned int memflags;

	if (!blkcg_policy_enabled(q, pol))
	/* [한국어] 이미 비활성화된 정책은 무시 */
	/* [한국어] deactivate 전 정책 활성화 여부 재확인 */
		return;

	if (queue_is_mq(q))
	/* [한국어] NVMe queue freeze; IO 경로 정지 후 정책 제거 */
		memflags = blk_mq_freeze_queue(q);
		/* [한국어] blk-mq queue freeze; NVMe IO 제출/완료 일시 정지 */

	mutex_lock(&q->blkcg_mutex);
	/* [한국어] blkg_free_workfn 과의 동기화 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득 */

	__clear_bit(pol->plid, q->blkcg_pols);
	/* [한국어] queue 의 정책 활성화 비트 클리어 */

	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* [한국어] 모든 blkg 의 pd 해제; NVMe queue 에서 cgroup 정책 제거 */
		struct blkcg *blkcg = blkg->blkcg;

		spin_lock(&blkcg->lock);
		/* [한국어] blkcg lock 획득 */
		if (blkg->pd[pol->plid]) {
		/* [한국어] pd 가 할당되어 있으면 해제 */
			if (blkg->pd[pol->plid]->online && pol->pd_offline_fn)
		/* [한국어] online 이면 offline 처리 */
				pol->pd_offline_fn(blkg->pd[pol->plid]);
			pol->pd_free_fn(blkg->pd[pol->plid]);
		/* [한국어] pd 메모리 해제 */
			blkg->pd[pol->plid] = NULL;
		/* [한국어] blkg 에서 pd 제거; nvme_queue_rq() 정책 참조 차단 */
		}
		spin_unlock(&blkcg->lock);
	}

	spin_unlock_irq(&q->queue_lock);
	/* [한국어] queue_lock 해제 */
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] blkcg_mutex 해제 */

	if (queue_is_mq(q))
	/* [한국어] queue freeze 해제; NVMe IO 재개 */
		blk_mq_unfreeze_queue(q, memflags);
}
EXPORT_SYMBOL_GPL(blkcg_deactivate_policy);

/*
 * [한국어]
 * blkcg_free_all_cpd - 모든 cgroup에서 이 정책의 cgroup-level 데이터를 해제
 *
 * @pol: 해제 대상 정책
 * @return: 없음
 *
 * blk-cgroup에는 두 종류의 정책 데이터가 있다:
 *   cpd (cgroup policy data) - cgroup마다 하나. 장치와 무관한 설정
 *                              (예: BFQ의 cgroup 가중치 기본값)
 *   pd  (policy data)        - (cgroup × 장치) 조합마다 하나. blkg에 붙는다.
 * 이 함수는 전자를 정리한다.
 *
 * 정책 등록 실패 시의 롤백과 정책 모듈 언로드 시에 호출된다. 모든 blkcg를
 * 순회해야 하므로 blkcg_pol_mutex 보호가 필요하며, 호출자가 이미 잡고 있다.
 *
 * 실행 컨텍스트: 정책 등록/해제 경로(프로세스 컨텍스트),
 * blkcg_pol_mutex 보유 상태.
 *
 * 호출 체인:
 *   blkcg_policy_register(실패 롤백) / blkcg_policy_unregister
 *     → [blkcg_free_all_cpd] → pol->cpd_free_fn
 */
static void blkcg_free_all_cpd(struct blkcg_policy *pol)
{
	struct blkcg *blkcg;
	/* [한국어] 순회 중인 cgroup */

	list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
	/* [한국어] 시스템 전체 blkcg 순회 */
		if (blkcg->cpd[pol->plid]) {
		/* [한국어] 해당 정책의 cpd 가 있으면 해제 */
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
 * [한국어]
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
	/* [한국어] cpd 할당 시 순회 중인 cgroup */
	int i, ret;
	/* [한국어] 정책 슬롯 인덱스와 반환값 */

	/*
	 * Make sure cpd/pd_alloc_fn and cpd/pd_free_fn in pairs, and policy
	 * without pd_alloc_fn/pd_free_fn can't be activated.
	 */
	if ((!pol->cpd_alloc_fn ^ !pol->cpd_free_fn) ||
	/* [한국어] alloc/free 함수 쌍이 맞아야 메모리 누수/부패 방지 */
	    (!pol->pd_alloc_fn ^ !pol->pd_free_fn))
		return -EINVAL;

	mutex_lock(&blkcg_pol_register_mutex);
	/* [한국어] 정책 등록/해제 전역 직렬화 */
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블 보호 */

	/* find an empty slot */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* [한국어] 빈 정책 슬롯 탐색 */
		if (!blkcg_policy[i])
			break;
	if (i >= BLKCG_MAX_POLS) {
	/* [한국어] 정책 슬롯 부족, 더 이상 NVMe queue 정책 추가 불가 */
		pr_warn("blkcg_policy_register: BLKCG_MAX_POLS too small\n");
		ret = -ENOSPC;
		goto err_unlock;
	}

	/* register @pol */
	pol->plid = i;
	/* [한국어] 정책 id 할당 */
	blkcg_policy[pol->plid] = pol;
	/* [한국어] 전역 정책 테이블에 등록 */

	/* allocate and install cpd's */
	if (pol->cpd_alloc_fn) {
	/* [한국어] 기존 모든 cgroup 에 cpd 할당 */
		list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
		/* [한국어] 모든 cgroup 에 대해 cpd 할당 */
			struct blkcg_policy_data *cpd;

			cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		/* [한국어] per-cgroup 정책 데이터 할당 */
			if (!cpd) {
		/* [한국어] cpd 할당 실패 시 롤백 */
				ret = -ENOMEM;
				goto err_free_cpds;
			}

			blkcg->cpd[pol->plid] = cpd;
		/* [한국어] cgroup 에 cpd 연결 */
			cpd->blkcg = blkcg;
		/* [한국어] cpd 가 역참조할 cgroup 설정 */
			cpd->plid = pol->plid;
		/* [한국어] policy id 설정 */
		}
	}

	mutex_unlock(&blkcg_pol_mutex);

	/* everything is in place, add intf files for the new policy */
	if (pol->dfl_cftypes == pol->legacy_cftypes) {
	/* [한국어] v2/v1 cgroup 파일이 동일하면 하나로 등록 */
		WARN_ON(cgroup_add_cftypes(&io_cgrp_subsys,
					   pol->dfl_cftypes));
	} else {
		WARN_ON(cgroup_add_dfl_cftypes(&io_cgrp_subsys,
		/* [한국어] cgroup v2 파일 추가; NVMe 설정 인터페이스 */
					       pol->dfl_cftypes));
		WARN_ON(cgroup_add_legacy_cftypes(&io_cgrp_subsys,
		/* [한국어] cgroup v1 파일 추가 */
						  pol->legacy_cftypes));
	}
	mutex_unlock(&blkcg_pol_register_mutex);
	return 0;

err_free_cpds:
	if (pol->cpd_free_fn)
	/* [한국어] 할당된 cpd 전부 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* [한국어] 정책 테이블에서 등록 취소 */
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
 * [한국어]
 * blkcg_policy_unregister - blkcg 정책 전역 등록 해제
 *
 * 호출 경로: policy module exit -> blkcg_policy_unregister()
 * NVMe 연결점: NVMe 장치에 적용되던 cgroup 정책 인터페이스를 제거한다.
 *   blkcg_policy[] 슬롯을 NULL 로 만들고 cpd 를 해제한다.
 */

void blkcg_policy_unregister(struct blkcg_policy *pol)
{
	mutex_lock(&blkcg_pol_register_mutex);
	/* [한국어] 정책 등록/해제 직렬화 */

	if (WARN_ON(blkcg_policy[pol->plid] != pol))
	/* [한국어] 슬롯 불일치 시 방어 */
		goto out_unlock;

	/* kill the intf files first */
	if (pol->dfl_cftypes)
	/* [한국어] cgroup v2 파일 제거 */
		cgroup_rm_cftypes(pol->dfl_cftypes);
	if (pol->legacy_cftypes)
	/* [한국어] cgroup v1 파일 제거 */
		cgroup_rm_cftypes(pol->legacy_cftypes);

	/* remove cpds and unregister */
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블 보호 */

	if (pol->cpd_free_fn)
	/* [한국어] 모든 cgroup 의 cpd 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* [한국어] 전역 정책 테이블에서 제거 */

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
 * [한국어]
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
	/* [한국어] 현재 delay_start 스냅샷; atomic read */

	/* negative use_delay means no scaling, see blkcg_set_delay() */
	if (atomic_read(&blkg->use_delay) < 0)
	/* [한국어] blkcg_set_delay() 모드에서는 decay 하지 않음 */
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
	/* [한국어] 1초 이상 지난 지연 예산을 decay; atomic CAS 로 경쟁하는 CPU 중 하나만 갱신 */
	/* [한국어] 1초 이상 지난 지연 예산을 decay */
	    atomic64_try_cmpxchg(&blkg->delay_start, &old, now)) {
		u64 cur = atomic64_read(&blkg->delay_nsec);
		/* [한국어] 현재 누적 지연량; atomic read */
		u64 sub = min_t(u64, blkg->last_delay, now - old);
		/* [한국어] 감소시킬 지연량 산출 */
		int cur_use = atomic_read(&blkg->use_delay);
		/* [한국어] 현재 use_delay 카운터; throttle 활성 여부 */

		/*
		 * We've been unthrottled, subtract a larger chunk of our
		 * accumulated delay.
		 */
		if (cur_use < blkg->last_use)
		/* [한국어] throttle 이 해제되면 더 많은 지연 예산을 감소 */
			sub = max_t(u64, sub, blkg->last_delay >> 1);

		/*
		 * This shouldn't happen, but handle it anyway.  Our delay_nsec
		 * should only ever be growing except here where we subtract out
		 * min(last_delay, 1 second), but lord knows bugs happen and I'd
		 * rather not end up with negative numbers.
		 */
		if (unlikely(cur < sub)) {
		/* [한국어] 음수 방지; 지연 예산 0 으로 클리어 */
			atomic64_set(&blkg->delay_nsec, 0);
			blkg->last_delay = 0;
		} else {
			atomic64_sub(sub, &blkg->delay_nsec);
		/* [한국어] 지연 예산 감소; atomic 연산 */
			blkg->last_delay = cur - sub;
		}
		blkg->last_use = cur_use;
		/* [한국어] last_use 갱신; 다음 decay 계산 기준 */
	}
}

/*
 * This is called when we want to actually walk up the hierarchy and check to
 * see if we need to throttle, and then actually throttle if there is some
 * accumulated delay.  This should only be called upon return to user space so
 * we're not holding some lock that would induce a priority inversion.
 */
/*
 * [한국어]
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
	/* [한국어] PSI memstall 플래그 */
	bool clamp;
	/* [한국어] delay 를 250ms 로 clamp 할지 여부 */
	u64 now = blk_time_get_ns();
	/* [한국어] 현재 시간; 지연 예산 정규화 기준 */
	u64 exp;
	/* [한국어] 깨어날 시간 */
	u64 delay_nsec = 0;
	/* [한국어] 계층에서 발견한 최대 지연량 */
	int tok;
	/* [한국어] io_schedule_prepare 토큰 */

	while (blkg->parent) {
	/* [한국어] 현재 blkg 에서 root 까지 계층을 따라 최대 지연 탐색 */
		int use_delay = atomic_read(&blkg->use_delay);
		/* [한국어] 이 cgroup/blkg 의 지연 예산 활성화 상태; atomic read */

		if (use_delay) {
		/* [한국어] 이 cgroup/blkg 에 지연 예산이 쌓여 있으면 throttle 검사 */
			u64 this_delay;

			blkcg_scale_delay(blkg, now);
			/* [한국어] 지연 예산을 현재 시간 기준으로 정규화 */
			this_delay = atomic64_read(&blkg->delay_nsec);
			/* [한국어] 정규화된 지연 예산; atomic read */
			if (this_delay > delay_nsec) {
			/* [한국어] 최대 지연 갱신; 양수 use_delay 이면 clamp */
				delay_nsec = this_delay;
				clamp = use_delay > 0;
			}
		}
		blkg = blkg->parent;
		/* [한국어] 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
	}

	if (!delay_nsec)
	/* [한국어] 지연 예산이 없으면 throttle 없음 */
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
	/* [한국어] 지나친 지연을 방지하기 위해 최대 250ms 로 clamp */
		delay_nsec = min_t(u64, delay_nsec, 250 * NSEC_PER_MSEC);

	if (use_memdelay)
	/* [한국어] PSI memory delay 기록 */
		psi_memstall_enter(&pflags);

	exp = ktime_add_ns(now, delay_nsec);
	/* [한국어] 깨어날 절대 시간 */
	tok = io_schedule_prepare();
	/* [한국어] IO 스케줄링 준비 */
	do {
		__set_current_state(TASK_KILLABLE);
		/* [한국어] kill 가능한 수면 상태로 전환; NVMe IO 대기 중 시그널 처리 */
		if (!schedule_hrtimeout(&exp, HRTIMER_MODE_ABS))
		/* [한국어] 지정 시간까지 수면; 시간 만료 시 깨어남 */
			break;
	} while (!fatal_signal_pending(current));
	io_schedule_finish(tok);
	/* [한국어] IO 스케줄링 종료 처리 */

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
 * [한국어]
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
	/* [한국어] throttle 할 NVMe disk */
	struct blkcg *blkcg;
	/* [한국어] 현재 태스크의 cgroup */
	struct blkcg_gq *blkg;
	/* [한국어] throttle 대상 blkg */
	bool use_memdelay = current->use_memdelay;
	/* [한국어] PSI memdelay 사용 여부 */

	if (!disk)
	/* [한국어] throttle 예약이 없으면 무시 */
		return;

	current->throttle_disk = NULL;
	/* [한국어] throttle_disk 클리어; 한 syscall 당 한 번만 throttle */
	current->use_memdelay = false;
	/* [한국어] memdelay 플래그 클리어 */

	rcu_read_lock();
	/* [한국어] blkg_lookup 및 css 접근을 RCU 로 보호 */
	blkcg = css_to_blkcg(blkcg_css());
	/* [한국어] 현재 태스크의 blkcg 획득 */
	if (!blkcg)
	/* [한국어] blkcg 가 없으면 throttle 불가 */
		goto out;
	blkg = blkg_lookup(blkcg, disk->queue);
	/* [한국어] NVMe disk 의 blkg 검색 */
	if (!blkg)
	/* [한국어] blkg 가 없으면 throttle 불가 */
		goto out;
	if (!blkg_tryget(blkg))
	/* [한국어] blkg 참조 획득 실패 시 throttle 불가; 제거 중일 수 있음 */
		goto out;
	rcu_read_unlock();
	/* [한국어] blkg_tryget 성공 후 RCU 종료; blkg 참조로 보호 */

	blkcg_maybe_throttle_blkg(blkg, use_memdelay);
	blkg_put(blkg);
	/* [한국어] throttle 완료 후 blkg 참조 반낑 */
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
 * [한국어]
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
	/* [한국어] kthread 는 user space 복귀가 없으므로 throttle 예약 안 함 */
		return;

	if (current->throttle_disk != disk) {
	/* [한국어] 다른 disk 를 가리키고 있거나 처음 설정 */
		if (test_bit(GD_DEAD, &disk->state))
		/* [한국어] 죽은 disk 이면 throttle 예약 안 함; NVMe namespace 제거 중 */
			return;
		get_device(disk_to_dev(disk));
		/* [한국어] disk 장치 참조 획득 */

		if (current->throttle_disk)
		/* [한국어] 이전 disk 참조 반낑 */
			put_disk(current->throttle_disk);
		current->throttle_disk = disk;
		/* [한국어] throttle 할 disk 설정 */
	}

	if (use_memdelay)
	/* [한국어] memdelay 플래그 설정 */
		current->use_memdelay = use_memdelay;
	set_notify_resume(current);
	/* [한국어] user space 복귀 시 blkcg_maybe_throttle_current() 실행 예약 */
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
 * [한국어]
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
	/* [한국어] set_delay 모드와 혼용되면 안 되는 경고 */
		return;
	blkcg_scale_delay(blkg, now);
	/* [한국어] 먼저 지연 예산을 시간에 따라 정규화 */
	atomic64_add(delta, &blkg->delay_nsec);
	/* [한국어] delta 를 atomic 으로 누적; NVMe 멀티 코어에서의 race 방지 */
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
 * [한국어]
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
	/* [한국어] 검색 중인 blkg 와 결과 */

	rcu_read_lock();
	/* [한국어] blkg_lookup_create 및 parent 체인 접근 보호 */
	blkg = blkg_lookup_create(css_to_blkcg(css), bio->bi_bdev->bd_disk);
	/* [한국어] bio 의 disk(NVMe namespace)에 대한 blkg 검색/생성 */
	while (blkg) {
	/* [한국어] blkg->parent 체인을 따라 올라가며 살아있는 blkg 탐색 */
		if (blkg_tryget(blkg)) {
		/* [한국어] 참조 획득 성공; IO 수명 동안 blkg 유지 */
			ret_blkg = blkg;
			break;
		}
		blkg = blkg->parent;
	/* [한국어] 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
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
 * [한국어]
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
		/* [한국어] 기존 blkg 참조 반낑 후 재연결 */
	/* [한국어] 기존 blkg 가 있으면 참조 해제 후 재연결 */
		blkg_put(bio->bi_blkg);

	if (css && css->parent) {
	/* [한국어] root 가 아닌 cgroup 이면 가장 가까운 blkg 검색 */
		bio->bi_blkg = blkg_tryget_closest(bio, css);
	} else {
		blkg_get(bdev_get_queue(bio->bi_bdev)->root_blkg);
		/* [한국어] root cgroup 이면 해당 NVMe queue 의 root_blkg 사용 */
		bio->bi_blkg = bdev_get_queue(bio->bi_bdev)->root_blkg;
		/* [한국어] root_blkg 를 bio->bi_blkg 에 설정; NVMe SQ/CQ 선택의 cgroup 기준 확정 */
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
 * [한국어]
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
	/* [한국어] bio 의 cgroup css */

	if (blk_op_is_passthrough(bio->bi_opf))
	/* [한국어] passthrough/admin 명령은 cgroup 연결 제외 */
		return;

	rcu_read_lock();
	/* [한국어] blkcg_css() 및 blkg 연결의 RCU 보호 */

	if (bio->bi_blkg)
	/* [한국어] 기존 blkg 의 css 재사용 */
		css = bio_blkcg_css(bio);
	else
		css = blkcg_css();
	/* [한국어] 현재 태스크의 cgroup css 획득 */

	bio_associate_blkg_from_css(bio, css);
	/* [한국어] css 에 맞는 blkg 로 bio 연결; NVMe SQ/CQ 선택의 cgroup 기준 확정 */

	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(bio_associate_blkg);

/**
 * bio_clone_blkg_association - clone blkg association from src to dst bio
 * @dst: destination bio
 * @src: source bio
 */
/*
 * [한국어]
 * bio_clone_blkg_association - src bio 의 blkg 연결을 dst bio 로 복제
 *
 * 호출 경로: bio_clone_* -> bio_clone_blkg_association()
 * NVMe 연결점: NVMe split/clone bio 가 원본과 동일한 cgroup context 를
 *   유지하도록 한다. CID/SQ 에 기록될 때 동일한 cgroup 정책이 적용된다.
 */

void bio_clone_blkg_association(struct bio *dst, struct bio *src)
{
	if (src->bi_blkg)
	/* [한국어] src bio 에 blkg 이 있을 때만 복제 */
		bio_associate_blkg_from_css(dst, bio_blkcg_css(src));
	/* [한국어] dst bio 에 동일한 cgroup css 적용; CID/SQ 기록 시 동일한 cgroup 정책 적용 */
}
EXPORT_SYMBOL_GPL(bio_clone_blkg_association);

/*
 * [한국어]
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
	/* [한국어] discard/flush 등은 BLKG_IOSTAT_DISCARD 로 분류 */
		return BLKG_IOSTAT_DISCARD;
	if (op_is_write(bio->bi_opf))
	/* [한국어] write 관련 opcode 를 BLKG_IOSTAT_WRITE 로 분류 */
		return BLKG_IOSTAT_WRITE;
	/* [한국어] 나머지는 read 로 분류; NVMe read opcode 와 대응 */
	return BLKG_IOSTAT_READ;
}

/*
 * [한국어]
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
	/* [한국어] bio 가 속한 cgroup */
	int rwd = blk_cgroup_io_type(bio), cpu;
	/* [한국어] IO 유형(read/write/discard)과 현재 CPU */
	struct blkg_iostat_set *bis;
	/* [한국어] per-cpu 통계 영역 포인터 */
	unsigned long flags;

	if (!cgroup_subsys_on_dfl(io_cgrp_subsys))
	/* [한국어] v1(hierarchy=legacy) cgroup 은 여기서 통계 집계 안 함 */
		return;

	/* Root-level stats are sourced from system-wide IO stats */
	if (!cgroup_parent(blkcg->css.cgroup))
	/* [한국어] root cgroup 통계는 시스템 전체 disk_stats 로 대체 */
		return;

	cpu = get_cpu();
	/* [한국어] 현재 CPU 의 per-cpu 통계 영역 사용; preempt disable 상태 */
	/* [한국어] 현재 CPU 의 per-cpu 통계 영역 사용 */
	bis = per_cpu_ptr(bio->bi_blkg->iostat_cpu, cpu);
	/* [한국어] bio 가 속한 blkg 의 per-cpu iostat_set 획득 */
	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* [한국어] per-cpu 통계 seqlock 진입; irqsave 로 NVMe ISR 컨텍스트에서도 안전 */

	/*
	 * If the bio is flagged with BIO_CGROUP_ACCT it means this is a split
	 * bio and we would have already accounted for the size of the bio.
	 */
	if (!bio_flagged(bio, BIO_CGROUP_ACCT)) {
	/* [한국어] split bio 는 이미 크기를 집계했으므로 중복 방지 */
		bio_set_flag(bio, BIO_CGROUP_ACCT);
		/* [한국어] 중복 집계 방지 플래그 설정 */
		bis->cur.bytes[rwd] += bio->bi_iter.bi_size;
		/* [한국어] bio 크기(바이트)를 read/write/discard 별로 누적; NVMe PRP/SGL 전송 크기(추정) */
		/* [한국어] bio 크기(바이트)를 read/write/discard 별로 누적 */
	}
	bis->cur.ios[rwd]++;
	/* [한국어] read/write/discard IO 횟수 증가; NVMe CID 단위 완료와 대응(추정) */
	/* [한국어] read/write/discard IO 횟수 증가 */

	/*
	 * If the iostat_cpu isn't in a lockless list, put it into the
	 * list to indicate that a stat update is pending.
	 */
	if (!READ_ONCE(bis->lqueued)) {
	/* [한국어] 아직 lockless list 에 없으면 flush 대기열에 등록 */
		struct llist_head *lhead = this_cpu_ptr(blkcg->lhead);
		/* [한국어] 현재 CPU 의 cgroup lockless list 헤드 */

		llist_add(&bis->lnode, lhead);
		/* [한국어] per-cpu 통계 노드를 cgroup 의 lockless list 에 추가; 이후 rstat flush 시 global 로 반영 */
		/* [한국어] per-cpu 통계 노드를 cgroup 의 lockless list 에 추가 */
		WRITE_ONCE(bis->lqueued, true);
		/* [한국어] list 등록 상태를 배리어와 함께 기록; __blkcg_rstat_flush 의 llist_del_all 과 동기화 */
		/* [한국어] list 등록 상태를 배리어와 함께 기록 */
	}

	u64_stats_update_end_irqrestore(&bis->sync, flags);
	/* [한국어] per-cpu 통계 seqlock 해제 */
	css_rstat_updated(&blkcg->css, cpu);
	/* [한국어] cgroup rstat framework 에 통계 갱신 알림; lazy flush 트리거 */
	/* [한국어] cgroup rstat framework 에 통계 갱신 알림 */
	put_cpu();
	/* [한국어] preempt enable 복원 */
}

/*
 * [한국어]
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
	/* [한국어] 현재 태스크의 cgroup */
	bool ret = false;
	/* [한국어] 혼잡 상태 반환값 */

	rcu_read_lock();
	/* [한국어] blkcg 계층 탐색의 RCU 보호 */
	for (blkcg = css_to_blkcg(blkcg_css()); blkcg;
	/* [한국어] 현재 cgroup 에서 root 까지 계층 순회 */
	     blkcg = blkcg_parent(blkcg)) {
		if (atomic_read(&blkcg->congestion_count)) {
		/* [한국어] congestion_count > 0 이면 NVMe queue 가 지연/스로틀 상태로 판단 */
			ret = true;
			break;
		}
	}
	rcu_read_unlock();
	/* [한국어] RCU read-side 종료 */
	return ret;
}

module_param(blkcg_debug_stats, bool, 0644);
	/* [한국어] debug stats 모듈 파라미터; NVMe queue 지연/통계 디버깅용 */
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
