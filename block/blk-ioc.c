// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to io context handling
 *
 * NVMe 관점 요약: 이 파일은 프로세스별 io_context와 request_queue 간의
 * 연결 구조인 io_cq를 관리한다. NVMe SSD로 가는 I/O 경로에서 bio가
 * blk-mq request로 전환될 때, 어느 ioprio와 스케줄러 콘텍스트를 사용할지
 * 결정하는 중간 관문 역할을 한다.
 *
 * 주요 호출 경로:
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *      -> blk_mq_sched_dispatch -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * 관련 파일: block/blk-mq.c, block/elevator.c, block/blk-mq-sched.h
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/security.h>
#include <linux/sched/task.h>

#include "blk.h"
#include "blk-mq-sched.h"

/*
 * For io context allocations
 *
 * io_context 객체를 위한 slab 캐시. NVMe 입장에서는 태스크 하나가
 * 생성하는 모든 I/O의 메타데이터(우선순위, icq 목록 등)를 담는
 * 컨테이너가 여기서 할당된다.
 */
static struct kmem_cache *iocontext_cachep;

#ifdef CONFIG_BLK_ICQ
/**
 * get_io_context - increment reference count to io_context
 * @ioc: io_context to get
 *
 * Increment reference count to @ioc.
 *
 * NVMe 연결:
 *   - io_context는 태스크가 NVMe 장치에 I/O를 날릴 때마다 참조된다.
 *   - refcount가 0이 되면 iocontext_cachep로 반환되므로, NVMe I/O 진행
 *     중에는 refcount가 유지되어야 한다.
 */
static void get_io_context(struct io_context *ioc)
{
	BUG_ON(atomic_long_read(&ioc->refcount) <= 0); /* NVMe: refcount <= 0이면 io_context가 이미 해제되어 CID/tag 연결 상태가 오염될 수 있음 */
	atomic_long_inc(&ioc->refcount); /* NVMe: submit_bio -> blk_mq_get_request -> elevator에서 태스크의 io_context 참조를 증가 */
}

/*
 * Exit an icq. Called with ioc locked for blk-mq, and with both ioc
 * and queue locked for legacy.
 *
 * NVMe 연결:
 *   - io_cq는 io_context와 request_queue를 잇는 브리지 구조체다.
 *   - NVMe 큐(nvme_queue)가 아닌, blk-mq의 request_queue(논리 큐)와
 *     연결되며, elevator(BFQ/MQ-Deadline 등)가 icq를 통해 태스크별
 *     스케줄링 상태를 유지한다.
 *   - et->ops.exit_icq 호출로 스케줄러가 태스크별 NVMe I/O 통계를
 *     정리할 기회를 준다.
 */
static void ioc_exit_icq(struct io_cq *icq)
{
	struct elevator_type *et = icq->q->elevator->type; /* NVMe: et->ops.exit_icq는 BFQ/MQ-Deadline이 태스크별 nvme_queue 우선순위/엔티티를 정리하는 콜백 */

	if (icq->flags & ICQ_EXITED) /* NVMe: 이미 스케줄러가 NVMe I/O 콘텍스트를 정리했으면 중복 실행 방지 */
		return;

	if (et->ops.exit_icq)
		et->ops.exit_icq(icq); /* NVMe: scheduler -> icq 해제 시 태스크별 queue depth/weight/time_slice 누적치 초기화 */

	icq->flags |= ICQ_EXITED; /* NVMe: NVMe I/O 발행 경로에서 재사용되지 않도록 플래그 원자적 설정 */
}

static void ioc_exit_icqs(struct io_context *ioc)
{
	struct io_cq *icq;

	spin_lock_irq(&ioc->lock); /* NVMe: 태스크가 여러 NVMe 장치에 대한 icq_list를 순회하므로 IRQ OFF로 보호 */
	hlist_for_each_entry(icq, &ioc->icq_list, ioc_node) /* NVMe: 태스크당 하나의 io_context가 여러 NVMe/블록 장치의 icq를 링크드 리스트로 연결 */
		ioc_exit_icq(icq); /* NVMe: 각 장치별 elevator가 NVMe SQ/CQ 선택에 사용한 상태를 정리 */
	spin_unlock_irq(&ioc->lock);
}

/*
 * Release an icq. Called with ioc locked for blk-mq, and with both ioc
 * and queue locked for legacy.
 *
 * NVMe 연결:
 *   - icq는 io_context<->request_queue 간 1:1 매핑(태스크-디바이스 쌍)을
 *     나타낸다. NVMe를 사용하는 프로세스가 종료되거나 큐가 재초기화될
 *     때 이 매핑이 해제된다.
 *   - radix_tree_delete(&ioc->icq_tree, icq->q->id): 태스크가 여러
 *     NVMe/블록 장치에 접근할 때 각 장치별 icq를 빠르게 찾기 위한
 *     radix tree에서 제거한다.
 *   - hlist_del_init(&icq->ioc_node): io_context의 icq_list에서 분리.
 *   - list_del_init(&icq->q_node): request_queue의 icq_list에서 분리.
 */
static void ioc_destroy_icq(struct io_cq *icq)
{
	struct io_context *ioc = icq->ioc; /* NVMe: 이 icq를 소유한 태스크의 io_context (refcount 보호 아래) */
	struct request_queue *q = icq->q; /* NVMe: NVMe 장치의 blk-mq request_queue; nvme_alloc_queue()가 생성한 논리 큐와 매핑 */
	struct elevator_type *et = q->elevator->type; /* NVMe: elevator(BFQ/MQ-Deadline)의 icq_cache/slab 이름 결정 */

	lockdep_assert_held(&ioc->lock); /* NVMe: io_context->icq_tree/icq_list 변경 시 ioc->lock이 잡혀 있어야 함; lockdep으로 교착/데이터 경쟁 조기 발견 */
	lockdep_assert_held(&q->queue_lock); /* NVMe: request_queue->icq_list 변경 시 queue_lock이 잡혀 있어야 함; NVMe I/O 완료 경로와의 동기화 보장 */

	if (icq->flags & ICQ_DESTROYED) /* NVMe: 이미 제거된 icq면 중복 free로 인한 use-after-free 방지 */
		return;

	radix_tree_delete(&ioc->icq_tree, icq->q->id); /* NVMe: request_queue->id로 인덱싱된 radix tree에서 태스크-장치 매핑 제거 */
	hlist_del_init(&icq->ioc_node); /* NVMe: io_context->icq_list 분리; 이후 ioc_lookup_icq()에서 탐색 불가 */
	list_del_init(&icq->q_node); /* NVMe: request_queue->icq_list 분리; 큐 재초기화 시 더 이상 순회 대상 아님 */

	/*
	 * Both setting lookup hint to and clearing it from @icq are done
	 * under queue_lock.  If it's not pointing to @icq now, it never
	 * will.  Hint assignment itself can race safely.
	 *
	 * NVMe 연결:
	 *   - ioc->icq_hint는 마지막으로 사용한 request_queue의 icq를
	 *     캐싱하여, 반복적인 NVMe I/O 경로에서 radix tree 검색을
	 *     피하게 한다.
	 */
	if (rcu_access_pointer(ioc->icq_hint) == icq) /* NVMe: hot cache가 곧 해제될 icq를 가리키면 NULL로 클리어 */
		rcu_assign_pointer(ioc->icq_hint, NULL); /* NVMe: RCU reader(ioc_lookup_icq)가 stale icq를 보지 않도록 배리어 포함 할당 */

	ioc_exit_icq(icq); /* NVMe: scheduler별 cleanup -> NVMe SQ 우선순위/엔티티 제거 */

	/*
	 * @icq->q might have gone away by the time RCU callback runs
	 * making it impossible to determine icq_cache.  Record it in @icq.
	 *
	 * NVMe 연결:
	 *   - NVMe 장치 제거(hotplug) 등으로 request_queue가 먼저 소멸할
	 *     수 있으므로, RCU callback 시점에 사용할 slab 캐시를 미리
	 *     icq에 저장해 둔다.
	 */
	icq->__rcu_icq_cache = et->icq_cache; /* NVMe: RCU grace period 이후 kfree_rcu()로 반환할 slab 캐시를 미리 기록 */
	icq->flags |= ICQ_DESTROYED; /* NVMe: 제거 완료 표시; concurrent ioc_lookup_icq가 이 icq를 새로 생성하는 것을 방지 */
	kfree_rcu(icq, __rcu_head); /* NVMe: 모든 RCU reader가 ioc_lookup_icq를 빠져나간 후 메모리 해제 */
}

/*
 * Slow path for ioc release in put_io_context().  Performs double-lock
 * dancing to unlink all icq's and then frees ioc.
 *
 * NVMe 연결:
 *   - 태스크가 종료되어 io_context의 마지막 참조가 해제될 때 실행된다.
 *   - 이 태스크가 여러 NVMe 장치에 대해 생성한 모든 icq를 정리한다.
 *   - queue_lock과 ioc->lock 획득 순서를 맞추기 위해 trylock 실패 시
	 *     RCU read-side 임계구간 안에서 lock 순서를 재조정한다.
 */
static void ioc_release_fn(struct work_struct *work)
{
	struct io_context *ioc = container_of(work, struct io_context,
					      release_work); /* NVMe: release_work는 io_context->release_work에서 변환; 태스크 종료 후 지연 해제 */
	spin_lock_irq(&ioc->lock); /* NVMe: icq_list 순회 및 제거 중 interrupt로 인한 재진입 방지 */

	while (!hlist_empty(&ioc->icq_list)) { /* NVMe: 태스크가 생성한 모든 장치별 icq가 제거될 때까지 반복 */
		struct io_cq *icq = hlist_entry(ioc->icq_list.first,
						struct io_cq, ioc_node); /* NVMe: 첫 번째 icq를 꺼내 해당 request_queue(NVMe 장치)와의 연결 해제 */
		struct request_queue *q = icq->q; /* NVMe: 제거 대상 request_queue; nvme_reset_work 등에서 큐가 소멸될 수 있음 */

		if (spin_trylock(&q->queue_lock)) { /* NVMe: queue_lock 획득 성공 시 icq 즉시 분리; 실패하면 아래에서 lock 순서 재조정 */
			ioc_destroy_icq(icq); /* NVMe: request_queue->icq_list 및 io_context->icq_tree에서 icq 제거 */
			spin_unlock(&q->queue_lock);
		} else {
			/* Make sure q and icq cannot be freed. */
			rcu_read_lock(); /* NVMe: q와 icq가 RCU grace period 전까지 해제되지 않도록 보호 */

			/* Re-acquire the locks in the correct order. */
			spin_unlock(&ioc->lock); /* NVMe: lockdep 순서(queue_lock -> ioc->lock)를 맞추기 위해 일단 ioc->lock 해제 */
			spin_lock(&q->queue_lock); /* NVMe: NVMe 큐의 queue_lock 먼저 획득 -> NVMe I/O 완료/재배열과 동기화 */
			spin_lock(&ioc->lock); /* NVMe: ioc->lock 재획득 후 icq 제거 진행 */

			ioc_destroy_icq(icq); /* NVMe: 재획득 후 icq 해제; NVMe SQ/CQ 선택에 사용하던 scheduler 상태 정리 */

			spin_unlock(&q->queue_lock);
			rcu_read_unlock();
		}
	}

	spin_unlock_irq(&ioc->lock); /* NVMe: 모든 icq 제거 완료; io_context 자체 해제는 여기서 허용 */

	kmem_cache_free(iocontext_cachep, ioc); /* NVMe: 모든 icq 제거 후 io_context 반환; 이후 동일 태스크의 NVMe I/O는 새 io_context 생성 */
}

/*
 * Releasing icqs requires reverse order double locking and we may already be
 * holding a queue_lock.  Do it asynchronously from a workqueue.
 *
 * NVMe 연결:
 *   - NVMe I/O 완료 경로 등에서 이미 queue_lock을 잡은 상태에서
 *     io_context를 해제해야 할 때 비동기 워크큐로 미룬다.
 *   - system_power_efficient_wq를 사용해 전력 효율적인 코어에서
 *     지연 해제를 처리한다 (추정: 배터리 중심 시스템에서 유리).
 */
static bool ioc_delay_free(struct io_context *ioc)
{
	unsigned long flags;

	spin_lock_irqsave(&ioc->lock, flags); /* NVMe: 현재 컨텍스트가 NVMe I/O 완료/abort 경로 등에서 queue_lock을 잡고 있을 수 있음 */
	if (!hlist_empty(&ioc->icq_list)) { /* NVMe: 아직 연결된 NVMe 장치가 남아 있으면 워크큐로 지연 해제 */
		queue_work(system_power_efficient_wq, &ioc->release_work); /* NVMe: 전력 효율 워크큐에서 ioc_release_fn 실행; (추정) 배터리 중심 모바일/NVMe 환경에서 유리 */
		spin_unlock_irqrestore(&ioc->lock, flags);
		return true; /* NVMe: io_context 해제를 워크큐에 위임; refcount 0 상태에서도 메모리는 남아 있음 */
	}
	spin_unlock_irqrestore(&ioc->lock, flags);
	return false; /* NVMe: 연결된 icq가 없으면 즉시 kmem_cache_free로 io_context 반환 */
}

/**
 * ioc_clear_queue - break any ioc association with the specified queue
 * @q: request_queue being cleared
 *
 * Walk @q->icq_list and exit all io_cq's.
 *
 * NVMe 연결:
 *   - NVMe 컨트롤러가 재설정되거나 큐가 재초기화될 때, 해당
 *     request_queue에 연결된 모든 태스크의 icq를 강제로 끊는다.
 *   - 이후 태스크가 다시 이 NVMe 장치로 I/O를 보낼 때
 *     ioc_create_icq()를 통해 새 icq가 생성된다.
 */
void ioc_clear_queue(struct request_queue *q)
{
	spin_lock_irq(&q->queue_lock); /* NVMe: request_queue를 사용하는 NVMe 컨트롤러가 재설정/제거될 때 queue_lock으로 보호 */
	while (!list_empty(&q->icq_list)) { /* NVMe: 이 request_queue에 연결된 모든 태스크의 icq를 순회하며 제거 */
		struct io_cq *icq =
			list_first_entry(&q->icq_list, struct io_cq, q_node); /* NVMe: q->icq_list의 첫 번째 태스크-장치 매핑을 가져옴 */

		/*
		 * Other context won't hold ioc lock to wait for queue_lock, see
		 * details in ioc_release_fn().
		 */
		spin_lock(&icq->ioc->lock); /* NVMe: ioc->lock 획득; ioc_release_fn과의 교착 상태를 피하기 위해 queue_lock 먼저 획득 */
		ioc_destroy_icq(icq); /* NVMe: NVMe 장치에 대한 태스크별 scheduler 상태(BFQ entity 등)를 정리하고 매핑 제거 */
		spin_unlock(&icq->ioc->lock);
	}
	spin_unlock_irq(&q->queue_lock); /* NVMe: 큐 초기화 완료; 이후 태스크가 이 NVMe 장치로 I/O를 본나면 새 icq 생성 */
}
#else /* CONFIG_BLK_ICQ */
static inline void ioc_exit_icqs(struct io_context *ioc)
{
}
static inline bool ioc_delay_free(struct io_context *ioc)
{
	return false;
}
#endif /* CONFIG_BLK_ICQ */

/**
 * put_io_context - put a reference of io_context
 * @ioc: io_context to put
 *
 * Decrement reference count of @ioc and release it if the count reaches
 * zero.
 *
 * NVMe 연결:
 *   - NVMe I/O가 끝난 request에서 io_context 참조를 해제할 때 호출.
 *   - refcount가 0이 되면 iocontext_cachep로 반환되거나, 아직 연결된
 *     icq가 있으면 ioc_delay_free()를 통해 워크큐에서 지연 해제한다.
 */
void put_io_context(struct io_context *ioc)
{
	BUG_ON(atomic_long_read(&ioc->refcount) <= 0); /* NVMe: refcount 언더플로우 방지; 음수면 이미 해제되어 NVMe request에 매달린 io_context가 아님 */
	if (atomic_long_dec_and_test(&ioc->refcount) && !ioc_delay_free(ioc)) /* NVMe: 마지막 참조 해제 시 icq가 남아 있으면 워크큐 지연, 없으면 즉시 반환 */
		kmem_cache_free(iocontext_cachep, ioc); /* NVMe: io_context 해제; 이 태스크의 NVMe I/O는 새 io_context를 할당받아 재시작 */
}
EXPORT_SYMBOL_GPL(put_io_context);

/* Called by the exiting task
 *
 * NVMe 연결:
 *   - 프로세스 종료 시 task_struct->io_context를 NULL로 만든 후,
 *     active_ref를 감소시킨다.
 *   - active_ref가 0이 되면 해당 태스크와 관련된 모든 NVMe icq를
 *     정리하고 io_context를 해제한다.
 */
void exit_io_context(struct task_struct *task)
{
	struct io_context *ioc;

	task_lock(task); /* NVMe: task_struct->io_context 접근을 직렬화; 종료 중에도 NVMe I/O가 진행될 수 있음 */
	ioc = task->io_context; /* NVMe: 종료하는 태스크의 io_context를 가져옴; NULL이면 더 이상 NVMe I/O를 발행하지 않음 */
	task->io_context = NULL; /* NVMe: 이 태스크는 더 이상 NVMe I/O를 submit_bio로 제출하지 않도록 설정 */
	task_unlock(task);

	if (atomic_dec_and_test(&ioc->active_ref)) { /* NVMe: active_ref가 0이면 이 태스크의 모든 NVMe I/O 발행이 중단됨을 의미 */
		ioc_exit_icqs(ioc); /* NVMe: 태스크가 생성한 모든 장치별 icq의 scheduler 상태(NVMe queue 가중치 등) 정리 */
		put_io_context(ioc); /* NVMe: active_ref 감소에 따른 io_context 최종 해제 또는 지연 해제 */
	}
}

static struct io_context *alloc_io_context(gfp_t gfp_flags, int node)
{
	struct io_context *ioc;

	ioc = kmem_cache_alloc_node(iocontext_cachep, gfp_flags | __GFP_ZERO,
				    node); /* NVMe: 태스크별 I/O 문맥을 위한 메모리; NVMe I/O 빈도가 높을수록 캐시 히트 중요 */
	if (unlikely(!ioc)) /* NVMe: 메모리 부족 시 io_context 할당 실패; 이 태스크의 NVMe I/O는 우선순위 없이 진행되거나 -ENOMEM 반환 */
		return NULL;

	atomic_long_set(&ioc->refcount, 1); /* NVMe: io_context 수명 시작; NVMe request가 이 io_context를 참조할 때마다 증가 */
	atomic_set(&ioc->active_ref, 1); /* NVMe: 태스크가 살아 있는 동안 1 유지; 종료 시 0으로 감소 후 icq 정리 */
#ifdef CONFIG_BLK_ICQ
	spin_lock_init(&ioc->lock); /* NVMe: icq_tree/icq_list 보호; NVMe I/O 발행 경로(ioc_lookup_icq)와 종료 경로가 경합 */
	INIT_RADIX_TREE(&ioc->icq_tree, GFP_ATOMIC); /* NVMe: request_queue->id -> icq 매핑; 여러 NVMe 장치에 대한 O(log n) 탐색 */
	INIT_HLIST_HEAD(&ioc->icq_list); /* NVMe: 태스크가 접근한 모든 NVMe/블록 장치의 icq를 연결하는 헤드 */
	INIT_WORK(&ioc->release_work, ioc_release_fn); /* NVMe: queue_lock을 잡은 채 io_context를 해제해야 할 때 사용되는 워크 항목 */
#endif
	/*
	 * ioprio는 NVMe의 I/O 우선순위를 결정하는 중요한 힌트다.
	 * (추정) 일부 NVMe 드라이버는 ioprio를 기반으로 SQ(Submission Queue)
	 * 우선순위나 WRR(Weighted Round Robin) 가중치를 조정할 수 있다.
	 */
	ioc->ioprio = IOPRIO_DEFAULT; /* NVMe: 기본 우선순위; 이후 set_task_ioprio()로 NVMe SQ 우선순위 조정 가능 (추정) */

	return ioc;
}

int set_task_ioprio(struct task_struct *task, int ioprio)
{
	int err;
	const struct cred *cred = current_cred(), *tcred; /* NVMe: 권한 검사; 다른 태스크의 NVMe I/O 우선순위를 임의로 변경하지 못하도록 제한 */

	rcu_read_lock(); /* NVMe: task credentials를 안전하게 읽기 위한 RCU read lock */
	tcred = __task_cred(task); /* NVMe: 대상 태스크의 cred 획득; RCU로 보호되어 종료 중인 태스크도 안전하게 접근 */
	if (!uid_eq(tcred->uid, cred->euid) &&
	    !uid_eq(tcred->uid, cred->uid) && !capable(CAP_SYS_NICE)) { /* NVMe: 권한 없는 ioprio 변경 거부; 잘못된 우선순위 조작으로 NVMe SQ 기아 방지 */
		rcu_read_unlock();
		return -EPERM;
	}
	rcu_read_unlock();

	err = security_task_setioprio(task, ioprio); /* NVMe: LSM 보안 모듈이 NVMe I/O 우선순위 변경을 추가로 제어할 수 있음 */
	if (err)
		return err;

	task_lock(task); /* NVMe: task->io_context 접근 보호; 동시에 여러 NVMe I/O 경로가 ioprio를 읽지 않도록 */
	if (unlikely(!task->io_context)) { /* NVMe: 아직 io_context가 없으면 우선순위를 저장할 공간을 할당 */
		struct io_context *ioc;

		task_unlock(task);

		ioc = alloc_io_context(GFP_ATOMIC, NUMA_NO_NODE); /* NVMe: GFP_ATOMIC으로 NVMe I/O 경로에서도 안전하게 io_context 할당 */
		if (!ioc)
			return -ENOMEM; /* NVMe: 메모리 부족으로 우선순위 설정 불가; NVMe I/O는 기본 우선순위로 진행 */

		task_lock(task);
		if (task->flags & PF_EXITING) { /* NVMe: 할당 중 태스크가 종료되면 io_context를 사용하지 않고 즉시 반환 */
			kmem_cache_free(iocontext_cachep, ioc);
			goto out;
		}
		if (task->io_context) /* NVMe: 다른 컨텍스트가 이미 io_context를 설정했으면 중복 할당 방지를 위해 반환 */
			kmem_cache_free(iocontext_cachep, ioc);
		else
			task->io_context = ioc; /* NVMe: 새 io_context를 태스크에 연결; 이후 NVMe I/O에서 ioprio 참조 가능 */
	}
	/*
	 * NVMe 연결:
	 *   - ioprio 변경은 이 태스크가 이후 NVMe 장치로 제출하는 모든
	 *     request에 즉시 반영된다.
	 *   - ioc_lookup_icq -> elevator path에서 우선순위를 참조하여
	 *     SQ 선택이나 타임슬라이스 배분에 영향을 줄 수 있다 (추정).
	 */
	task->io_context->ioprio = ioprio; /* NVMe: 태스크 우선순위 갱신; 이후 submit_bio -> blk_mq_get_request -> elevator에서 NVMe SQ 선택에 반영 */
out:
	task_unlock(task);
	return 0;
}
EXPORT_SYMBOL_GPL(set_task_ioprio);

int __copy_io(u64 clone_flags, struct task_struct *tsk)
{
	struct io_context *ioc = current->io_context; /* NVMe: 부모 태스크의 io_context; fork/clone 시 자식에게 NVMe I/O 문맥을 물려줌 */

	/*
	 * Share io context with parent, if CLONE_IO is set
	 *
	 * NVMe 연결:
	 *   - CLONE_IO가 설정된 스레드/프로세스는 부모의 io_context를
	 *     공유하므로, 동일한 NVMe 장치에 대한 icq도 공유한다.
	 *   - 이는 동일한 태스크 그룹의 I/O가 동일한 스케줄러 콘텍스트로
	 *     묶여 NVMe 큐에 도달함을 의미한다.
	 */
	if (clone_flags & CLONE_IO) { /* NVMe: CLONE_IO 시 부모-자식이 동일 io_context 공유 -> 동일 NVMe 장치의 icq도 공유 */
		atomic_inc(&ioc->active_ref); /* NVMe: 공유 io_context의 active_ref 증가; 자식 종료 시에도 부모의 NVMe I/O 문맥 유지 */
		tsk->io_context = ioc; /* NVMe: 자식 태스크가 부모의 io_context를 직접 참조; NVMe SQ/CQ 선택 시 동일 scheduler entity 사용 */
	} else if (ioprio_valid(ioc->ioprio)) { /* NVMe: io_context를 공유하지 않더라도 ioprio는 상속 -> NVMe 우선순위 유지 */
		tsk->io_context = alloc_io_context(GFP_KERNEL, NUMA_NO_NODE); /* NVMe: 자식 전용 io_context 할당; 독립적인 NVMe I/O 스케줄링 가능 */
		if (!tsk->io_context)
			return -ENOMEM; /* NVMe: io_context 할당 실패 시 자식은 NVMe I/O 우선순위 없이 실행될 수 있음 */
		tsk->io_context->ioprio = ioc->ioprio; /* NVMe: 부모의 ioprio를 복사; 자식의 NVMe I/O도 동일한 SQ 우선순위 상속 (추정) */
	}

	return 0;
}

#ifdef CONFIG_BLK_ICQ
/**
 * ioc_lookup_icq - lookup io_cq from ioc in io issue path
 * @q: the associated request_queue
 *
 * Look up io_cq associated with @ioc - @q pair from @ioc.  Must be called
 * from io issue path, either return NULL if current issue io to @q for the
 * first time, or return a valid icq.
 *
 * NVMe 연결:
 *   - NVMe I/O 발행 경로(예: blk_mq_get_request 내부)에서 현재 태스크의
 *     io_context로부터 대상 request_queue @q에 해당하는 icq를 찾는다.
 *   - icq는 elevator가 request를 NVMe SQ에 보내기 전에 타임슬라이스,
 *     큐 깊이, 우선순위 등을 계산하는 데 사용된다.
 */
struct io_cq *ioc_lookup_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* NVMe: 현재 submit_bio를 호출한 태스크의 io_context; 없으면 ioc_find_get_icq에서 할당 */
	struct io_cq *icq;

	/*
	 * icq's are indexed from @ioc using radix tree and hint pointer,
	 * both of which are protected with RCU, io issue path ensures that
	 * both request_queue and current task are valid, the found icq
	 * is guaranteed to be valid until the io is done.
	 *
	 * NVMe 연결:
	 *   - icq_hint은 hot cache 역할: 동일 NVMe 장치에 대한 반복 I/O가
	 *     radix tree 순회 없이 O(1)로 icq를 얻는다.
	 *   - 이는 고주파수 NVMe I/O(예: 4KB 랜덤 읽기)에서 지연을 줄인다.
	 */
	rcu_read_lock(); /* NVMe: ioc->icq_hint와 icq_tree가 RCU로 보호됨; NVMe I/O 발행 경로에서 lockless 조회 가능 */
	icq = rcu_dereference(ioc->icq_hint); /* NVMe: 마지막으로 사용한 request_queue의 icq를 lockless로 읽음; rcu_dereference로 메모리 순서 보장 */
	if (icq && icq->q == q) /* NVMe: hint가 대상 request_queue(NVMe 장치의 blk-mq 큐)와 일치하면 바로 사용 */
		goto out; /* NVMe: radix tree 탐색 없이 O(1)로 icq 획득; 고주파 NVMe I/O 지연 감소 */

	icq = radix_tree_lookup(&ioc->icq_tree, q->id); /* NVMe: hint miss 시 request_queue->id로 radix tree에서 icq 검색 */
	if (icq && icq->q == q) /* NVMe: radix tree에서 찾은 icq가 아직 동일 request_queue를 가리키는지 검증 */
		rcu_assign_pointer(ioc->icq_hint, icq);	/* allowed to race */ /* NVMe: 다음 NVMe I/O를 위해 hint 업데이트; race는 harmless */
	else
		icq = NULL; /* NVMe: icq가 없으면 ioc_create_icq()로 새로 생성; NVMe SQ/CQ 선택에 사용할 scheduler 상태 초기화 */
out:
	rcu_read_unlock(); /* NVMe: RCU read-side 종료; 이후 icq는 request 완료 전까지 유효함이 보장됨 */
	return icq; /* NVMe: 반환된 icq는 blk_mq_get_request -> elevator에서 NVMe queue 우선순위/타임슬라이스 계산에 사용 */
}
EXPORT_SYMBOL(ioc_lookup_icq);

/**
 * ioc_create_icq - create and link io_cq
 * @q: request_queue of interest
 *
 * Make sure io_cq linking @ioc and @q exists.  If icq doesn't exist, they
 * will be created using @gfp_mask.
 *
 * The caller is responsible for ensuring @ioc won't go away and @q is
 * alive and will stay alive until this function returns.
 *
 * NVMe 연결:
 *   - 현재 태스크가 처음으로 특정 NVMe/블록 장치의 request_queue @q에
 *     I/O를 보낼 때 icq를 할당하고 연결한다.
 *   - elevator_type->icq_cache에서 스케줄러별 icq가 할당되며,
 *     init_icq 콜백으로 스케줄러별 초기화(BFQ의 엔티티, vdisktime 등)가
 *     이루어진다.
 */
static struct io_cq *ioc_create_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* NVMe: 현재 태스크의 io_context; caller가 수명을 보장해야 함 */
	struct elevator_type *et = q->elevator->type; /* NVMe: 장치에 장착된 I/O scheduler(BFQ/MQ-Deadline)의 타입; icq_cache와 init_icq 결정 */
	struct io_cq *icq;

	/* allocate stuff */
	icq = kmem_cache_alloc_node(et->icq_cache, GFP_ATOMIC | __GFP_ZERO,
				    q->node); /* NVMe: scheduler별 icq_cache에서 할당; q->node로 NUMA locality 최적화 (추정: NVMe queue와 동일 노드 선호) */
	if (!icq)
		return NULL; /* NVMe: icq 할당 실패; 이 태스크는 해당 NVMe 장치에 대해 scheduler별 우선순위 없이 I/O 진행 */

	if (radix_tree_maybe_preload(GFP_ATOMIC) < 0) { /* NVMe: radix tree 삽입을 위한 preload; GFP_ATOMIC으로 NVMe I/O 경로에서 안전 */
		kmem_cache_free(et->icq_cache, icq);
		return NULL;
	}

	icq->ioc = ioc; /* NVMe: 이 icq가 속한 io_context 연결; NVMe I/O 완료 후 put_io_context에서 참조 해제 */
	icq->q = q; /* NVMe: 대상 request_queue(NVMe 장치의 blk-mq 큐) 연결; 이후 doorbell/SQ 선택 시 사용 */
	INIT_LIST_HEAD(&icq->q_node); /* NVMe: request_queue->icq_list 연결 준비; NVMe 컨트롤러 재설정 시 순회 대상 */
	INIT_HLIST_NODE(&icq->ioc_node); /* NVMe: io_context->icq_list 연결 준비; 태스크 종료 시 일괄 정리 */

	/* lock both q and ioc and try to link @icq */
	spin_lock_irq(&q->queue_lock); /* NVMe: request_queue의 icq_list와 elevator 상태 보호; NVMe I/O 완료/재배열과 동기화 */
	spin_lock(&ioc->lock); /* NVMe: io_context의 icq_tree/icq_list 보호; lockdep 순서 queue_lock -> ioc->lock 준수 */

	if (likely(!radix_tree_insert(&ioc->icq_tree, q->id, icq))) { /* NVMe: request_queue->id로 radix tree에 태스크-장치 매핑 삽입; 성공이 일반적 */
		hlist_add_head(&icq->ioc_node, &ioc->icq_list); /* NVMe: io_context에 연결; 태스크 종료 시 모든 icq 순회 가능 */
		list_add(&icq->q_node, &q->icq_list); /* NVMe: request_queue에 연결; ioc_clear_queue()에서 일괄 제거 */
		if (et->ops.init_icq)
			et->ops.init_icq(icq); /* NVMe: scheduler별 초기화(BFQ entity, vdisktime 등); NVMe SQ에 도달하기 전 우선순위/슬라이스 계산 준비 */
	} else {
		kmem_cache_free(et->icq_cache, icq); /* NVMe: 다른 CPU가 먼저 동일 (ioc, q) 쌍의 icq를 삽입한 경우 중복 할당 반환 */
		icq = ioc_lookup_icq(q); /* NVMe: 이미 삽입된 icq를 다시 조회; NVMe I/O 발행을 계속 진행 */
		if (!icq)
			printk(KERN_ERR "cfq: icq link failed!\n"); /* NVMe: 조회까지 실패하면 심각한 불일치; NVMe I/O 경로에서 NULL 반환 가능 */
	}

	spin_unlock(&ioc->lock); /* NVMe: io_context 잠금 해제; lockdep 순서에 따라 queue_lock보다 먼저 해제 */
	spin_unlock_irq(&q->queue_lock); /* NVMe: queue_lock 해제; 이후 다른 CPU가 동일 NVMe 장치로 I/O를 dispatch할 수 있음 */
	radix_tree_preload_end(); /* NVMe: radix tree preload 상태 정리 */
	return icq; /* NVMe: 반환된 icq는 blk_mq_get_request -> elevator -> nvme_queue_rq로 전달되어 NVMe SQ 선택에 반영 */
}

/**
 * ioc_find_get_icq - find or create io_cq for current task and @q
 * @q: target request_queue
 *
 * NVMe 연결:
 *   - NVMe I/O 발행 시 호출 경로:
 *     submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *          -> blk_mq_get_ctx -> (scheduler) -> ioc_find_get_icq(q)
 *   - current->io_context가 없으면 새로 할당하고, 있으면 refcount를
 *     증가시킨 후 @q에 대한 icq를 찾거나 생성한다.
 */
struct io_cq *ioc_find_get_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* NVMe: 현재 태스크의 io_context; 없으면 아래에서 할당 */
	struct io_cq *icq = NULL;

	if (unlikely(!ioc)) { /* NVMe: 첫 NVMe I/O 발행 시 io_context가 없음; GFP_ATOMIC으로 할당 */
		ioc = alloc_io_context(GFP_ATOMIC, q->node); /* NVMe: q->node로 NUMA locality 고려; NVMe queue와의 거리 최소화 (추정) */
		if (!ioc)
			return NULL; /* NVMe: io_context 할당 실패; 이 태스크의 NVMe I/O는 우선순위/scheduler 문맥 없이 처리 */

		task_lock(current); /* NVMe: current->io_context 갱신 경쟁 방지; 동시에 다른 스레드가 같은 태스크의 io_context를 설정할 수 있음 */
		if (current->io_context) { /* NVMe: 다른 경로에서 이미 io_context가 생성됨; 중복 할당 반환 */
			kmem_cache_free(iocontext_cachep, ioc);
			ioc = current->io_context;
		} else {
			current->io_context = ioc; /* NVMe: 새 io_context를 현재 태스크에 연결; 이후 NVMe I/O에서 재사용 */
		}

		get_io_context(ioc); /* NVMe: io_context refcount 증가; NVMe request 완료 시 put_io_context로 균형 맞춤 */
		task_unlock(current);
	} else {
		get_io_context(ioc); /* NVMe: 기존 io_context refcount 증가; NVMe I/O 진행 중에는 io_context가 해제되지 않도록 */
		icq = ioc_lookup_icq(q); /* NVMe: 기존 io_context에서 대상 request_queue(NVMe 장치)에 대한 icq 조회 */
	}

	if (!icq) { /* NVMe: 태스크가 처음 이 NVMe 장치로 I/O를 본나면 icq가 없음 */
		icq = ioc_create_icq(q); /* NVMe: 새 icq 할당 및 (io_context, request_queue)에 연결; scheduler 초기화 수행 */
		if (!icq) { /* NVMe: icq 할당/삽입 실패; NVMe I/O는 scheduler 문맥 없이 진행되거나 상위에서 -ENOMEM 처리 */
			put_io_context(ioc); /* NVMe: 위에서 증가시킨 io_context refcount 되돌림 */
			return NULL;
		}
	}
	return icq; /* NVMe: 호출자(elevator/blk-mq)가 이 icq를 통해 태스크별 NVMe SQ/CQ 선택 및 우선순위 계산 수행 */
}
EXPORT_SYMBOL_GPL(ioc_find_get_icq);
#endif /* CONFIG_BLK_ICQ */

static int __init blk_ioc_init(void)
{
	iocontext_cachep = kmem_cache_create("blkdev_ioc",
			sizeof(struct io_context), 0, SLAB_PANIC, NULL); /* NVMe: 부팅 시 io_context용 slab 캐시 생성; NVMe I/O를 발행하는 모든 태스크가 여기서 객체 할당 */
	return 0;
}
subsys_initcall(blk_ioc_init); /* NVMe: 서브시스템 초기화 단계에서 등록; NVMe 드라이버 로드 전에 io_context 캐시가 준비되어야 함 */

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 태스크-장치 쌍별 I/O 문맥(io_cq)을 관리하며, NVMe I/O가
 *   blk-mq를 통해 nvme_queue_rq()에 도달하기 전 스케줄러가 사용할
 *   상태(ioprio, 타임슬라이스 등)를 제공한다.
 * - 주요 호출 경로: submit_bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> ioc_find_get_icq -> elevator ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell).
 * - ioc->ioprio는 NVMe SQ 우선순위나 WRR 가중치에 영향을 줄 수 있는
 *   사용자 우선순위 힌트다 (추정).
 * - icq_hint과 radix_tree를 통해 동일 NVMe 장치에 대한 반복 I/O의
 *   문맥 조회를 가속화한다.
 * - block/elevator.c, block/blk-mq.c, block/blk-mq-sched.h와 긴밀히
 *   연결되어 있으며, CONFIG_BLK_ICQ가 꺼져 있으면 icq 관련 기능이
 *   컴파일에서 제외된다.
 */
