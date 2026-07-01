// SPDX-License-Identifier: GPL-2.0
/*
 * Block multiqueue core code
 *
 * Copyright (C) 2013-2014 Jens Axboe
 * Copyright (C) 2013-2014 Christoph Hellwig
 */
/*
 * NVMe SSD 관점 요약:
 *   본 파일은 Linux 멀티큐 블록 계층(block multi-queue, blk-mq)의 핵심 구현체로,
 *   상위 계층의 bio 를 request 로 변환하고, 하드웨어 큐(blk_mq_hw_ctx)에
 *   분배(dispatch)한 뒤, 디바이스 드라이버(예: NVMe)의 queue_rq 콜백으로
 *   전달하는 역할을 한다.
 *   NVMe 에서는 blk_mq_submit_bio -> blk_mq_get_new_requests ->
 *   __blk_mq_alloc_requests -> blk_mq_dispatch_rq_list -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 경로로 이어져 SQ 에 CID 를 할당하고 doorbell 을
 *   울려 컨트롤러에 명령을 제출한다.
 *   완료는 컨트롤러가 CQ 에 항목을 기록하면 인터럽트 핸들러나 폴리 함수가
 *   blk_mq_complete_request 를 타고, end_io -> blk_mq_end_request 를 거쳐
 *   상위로 회귀한다.
 *   blk-mq 는 blk-core.c 의 큐 진입점 뒤, NVMe 드라이버(예: drivers/nvme/host/pci.c)
 *   앞에 위치하며, blk-mq-sched.c / blk-mq-tag.c 와 함께 동작한다.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include <linux/kmemleak.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/llist.h>
#include <linux/cpu.h>
#include <linux/cache.h>
#include <linux/sched/topology.h>
#include <linux/sched/signal.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/crash_dump.h>
#include <linux/prefetch.h>
#include <linux/blk-crypto.h>
#include <linux/part_stat.h>
#include <linux/sched/isolation.h>

#include <trace/events/block.h>

#include <linux/t10-pi.h>
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-pm.h"
#include "blk-stat.h"
#include "blk-mq-sched.h"
#include "blk-rq-qos.h"

static DEFINE_PER_CPU(struct llist_head, blk_cpu_done);
// per-CPU 완료 리스트: NVMe CQ 항목을 다른 CPU로 라우팅하기 전 임시 보관
static DEFINE_PER_CPU(call_single_data_t, blk_cpu_csd);
// per-CPU CSD: NVMe 완료 IPI 발송용 콜백 데이터
static DEFINE_MUTEX(blk_mq_cpuhp_lock);
// CPU hotplug lock: NVMe SQ affinity 변경 시 hctx 재매핑 보호

static void blk_mq_insert_request(struct request *rq, blk_insert_t flags);
static void blk_mq_request_bypass_insert(struct request *rq,
		blk_insert_t flags);
static void blk_mq_try_issue_list_directly(struct blk_mq_hw_ctx *hctx,
		struct list_head *list);
static int blk_hctx_poll(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			 struct io_comp_batch *iob, unsigned int flags);

/*
 * Check if any of the ctx, dispatch list or elevator
 * have pending work in this hardware queue.
 */
/*
 * blk_mq_hctx_has_pending: 주어진 blk_mq_hw_ctx 에 처리할 request 가
 *   남아있는지 확인한다.
 *   NVMe 관점: 이 hctx 가 담당하는 SQ 에 새 명령을 더 넣을 수 있는지,
 *   또는 아직 dispatch 되지 않은 요청이 있는지 판단한다.
 *   호출 경로: blk_mq_hw_queue_need_run -> blk_mq_run_hw_queue.
 */
static bool blk_mq_hctx_has_pending(struct blk_mq_hw_ctx *hctx)
{
	return !list_empty_careful(&hctx->dispatch) ||
// hctx->dispatch 가 비어있지 않으면 아직 NVMe SQ 로 날아가지 않은 명령 존재
		sbitmap_any_bit_set(&hctx->ctx_map) ||
// sbitmap_any_bit_set: 이 NVMe SQ 에 매핑된 sw queue 중 pending 인 CPU가 있는지
			blk_mq_sched_has_work(hctx);
// scheduler 큐에도 NVMe 컨트롤러로 발행할 IO가 남아있을 수 있음
}

/*
 * Mark this ctx as having pending work in this hardware queue
 */
static void blk_mq_hctx_mark_pending(struct blk_mq_hw_ctx *hctx,
				     struct blk_mq_ctx *ctx)
{
	const int bit = ctx->index_hw[hctx->type];
// ctx->index_hw[hctx->type]: 이 CPU가 이 NVMe SQ 의 몇 번째 sw queue 인지

	if (!sbitmap_test_bit(&hctx->ctx_map, bit))
// 이미 pending 으로 표시되어 있으면 중복 set 방지
		sbitmap_set_bit(&hctx->ctx_map, bit);
// 해당 NVMe SQ 의 ctx_map 에 pending 표시 -> run_hw_queue() 때 처리
}

static void blk_mq_hctx_clear_pending(struct blk_mq_hw_ctx *hctx,
				      struct blk_mq_ctx *ctx)
{
	const int bit = ctx->index_hw[hctx->type];

	sbitmap_clear_bit(&hctx->ctx_map, bit);
// NVMe SQ 의 ctx_map 에서 해당 sw queue pending 비트 해제
}

/*
 * struct mq_inflight: 진행 중인 I/O 를 집계하기 위한 보조 구조체.
 *   part: 대상 block_device (NVMe namespace).
 *   inflight[2]: READ/WRITE 방향별로 NVMe SQ 에 아직 완료되지 않은
 *              명령(CID) 수를 카운트한다.
 */
struct mq_inflight {
	struct block_device *part; /* NVMe namespace 의 block_device */
	unsigned int inflight[2]; /* READ/WRITE 방향별 진행 중 명령(CID) 수 */
};

static bool blk_mq_check_in_driver(struct request *rq, void *priv)
{
	struct mq_inflight *mi = priv;

	if (rq->rq_flags & RQF_IO_STAT &&
// RQF_IO_STAT 가 설정된 NVMe IO 명령만 in_flight 로 집계
	    (!bdev_is_partition(mi->part) || rq->part == mi->part) &&
	    blk_mq_rq_state(rq) == MQ_RQ_IN_FLIGHT)
// MQ_RQ_IN_FLIGHT == NVMe SQ 에 제출되어 CQ 완료 대기 중인 CID
		mi->inflight[rq_data_dir(rq)]++;
// READ/WRITE 방향별로 아직 완료되지 않은 NVMe CID 개수 누적

	return true;
}

void blk_mq_in_driver_rw(struct block_device *part, unsigned int inflight[2])
{
	struct mq_inflight mi = { .part = part };

	blk_mq_queue_tag_busy_iter(bdev_get_queue(part), blk_mq_check_in_driver,
// tag 전체를 순회하며 hctx(SQ) 별 진행 중인 CID 수를 집계
				   &mi);
	inflight[READ] = mi.inflight[READ];
	inflight[WRITE] = mi.inflight[WRITE];
}

#ifdef CONFIG_LOCKDEP
static bool blk_freeze_set_owner(struct request_queue *q,
				 struct task_struct *owner)
{
	if (!owner)
		return false;

	if (!q->mq_freeze_depth) {
// freeze 깊이 추적: NVMe 컨트롤러 quiesce 와 유사
		q->mq_freeze_owner = owner;
		q->mq_freeze_owner_depth = 1;
		q->mq_freeze_disk_dead = !q->disk ||
// GD_DEAD 상태는 NVMe 컨트롤러가 사라진 경우에 대응
			test_bit(GD_DEAD, &q->disk->state) ||
			!blk_queue_registered(q);
		q->mq_freeze_queue_dying = blk_queue_dying(q);
// QUEUE_FLAG_DYING 이면 NVMe 컨트롤러 shutdown/shutdown_warn 단계로 진행 중
		return true;
	}

	if (owner == q->mq_freeze_owner)
		q->mq_freeze_owner_depth += 1;
	return false;
}

/* verify the last unfreeze in owner context */
static bool blk_unfreeze_check_owner(struct request_queue *q)
{
	if (q->mq_freeze_owner != current)
		return false;
	if (--q->mq_freeze_owner_depth == 0) {
		q->mq_freeze_owner = NULL;
		return true;
	}
	return false;
}

#else

static bool blk_freeze_set_owner(struct request_queue *q,
				 struct task_struct *owner)
{
	return false;
}

static bool blk_unfreeze_check_owner(struct request_queue *q)
{
	return false;
}
#endif

bool __blk_freeze_queue_start(struct request_queue *q,
			      struct task_struct *owner)
{
	bool freeze;

	mutex_lock(&q->mq_freeze_lock);
	freeze = blk_freeze_set_owner(q, owner);
	if (++q->mq_freeze_depth == 1) {
// freeze 시작: 진행 중인 request(CID) 참조가 0이 될 때까지 대기
		percpu_ref_kill(&q->q_usage_counter);
		mutex_unlock(&q->mq_freeze_lock);
		if (queue_is_mq(q))
// mq queue 면 pending NVMe SQ dispatch 를 일단 실행시켜 완료 유도
			blk_mq_run_hw_queues(q, false);
	} else {
		mutex_unlock(&q->mq_freeze_lock);
	}

	return freeze;
}

void blk_freeze_queue_start(struct request_queue *q)
{
	if (__blk_freeze_queue_start(q, current))
		blk_freeze_acquire_lock(q);
}
EXPORT_SYMBOL_GPL(blk_freeze_queue_start);

void blk_mq_freeze_queue_wait(struct request_queue *q)
{
	wait_event(q->mq_freeze_wq, percpu_ref_is_zero(&q->q_usage_counter));
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait);

int blk_mq_freeze_queue_wait_timeout(struct request_queue *q,
				     unsigned long timeout)
{
	return wait_event_timeout(q->mq_freeze_wq,
					percpu_ref_is_zero(&q->q_usage_counter),
					timeout);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait_timeout);

void blk_mq_freeze_queue_nomemsave(struct request_queue *q)
{
	blk_freeze_queue_start(q);
	blk_mq_freeze_queue_wait(q);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_nomemsave);

bool __blk_mq_unfreeze_queue(struct request_queue *q, bool force_atomic)
{
	bool unfreeze;

	mutex_lock(&q->mq_freeze_lock);
	if (force_atomic)
		q->q_usage_counter.data->force_atomic = true;
	q->mq_freeze_depth--;
// freeze_depth 감소: NVMe 컨트롤러 재개 전 단계 (추정)
	WARN_ON_ONCE(q->mq_freeze_depth < 0);
	if (!q->mq_freeze_depth) {
		percpu_ref_resurrect(&q->q_usage_counter);
// q_usage_counter 부활: 새로운 NVMe IO 제출 허용
		wake_up_all(&q->mq_freeze_wq);
	}
	unfreeze = blk_unfreeze_check_owner(q);
	mutex_unlock(&q->mq_freeze_lock);

	return unfreeze;
}

void blk_mq_unfreeze_queue_nomemrestore(struct request_queue *q)
{
	if (__blk_mq_unfreeze_queue(q, false))
		blk_unfreeze_release_lock(q);
}
EXPORT_SYMBOL_GPL(blk_mq_unfreeze_queue_nomemrestore);

/*
 * non_owner variant of blk_freeze_queue_start
 *
 * Unlike blk_freeze_queue_start, the queue doesn't need to be unfrozen
 * by the same task.  This is fragile and should not be used if at all
 * possible.
 */
void blk_freeze_queue_start_non_owner(struct request_queue *q)
{
	__blk_freeze_queue_start(q, NULL);
}
EXPORT_SYMBOL_GPL(blk_freeze_queue_start_non_owner);

/* non_owner variant of blk_mq_unfreeze_queue */
void blk_mq_unfreeze_queue_non_owner(struct request_queue *q)
{
	__blk_mq_unfreeze_queue(q, false);
}
EXPORT_SYMBOL_GPL(blk_mq_unfreeze_queue_non_owner);

/*
 * FIXME: replace the scsi_internal_device_*block_nowait() calls in the
 * mpt3sas driver such that this function can be removed.
 */
void blk_mq_quiesce_queue_nowait(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(&q->queue_lock, flags);
	if (!q->quiesce_depth++)
		blk_queue_flag_set(QUEUE_FLAG_QUIESCED, q);
// QUEUE_FLAG_QUIESCED 설정: NVMe SQ dispatch 를 일시 중단
	spin_unlock_irqrestore(&q->queue_lock, flags);
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_queue_nowait);

/**
 * blk_mq_wait_quiesce_done() - wait until in-progress quiesce is done
 * @set: tag_set to wait on
 *
 * Note: it is driver's responsibility for making sure that quiesce has
 * been started on or more of the request_queues of the tag_set.  This
 * function only waits for the quiesce on those request_queues that had
 * the quiesce flag set using blk_mq_quiesce_queue_nowait.
 */
void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set)
{
	if (set->flags & BLK_MQ_F_BLOCKING)
// BLK_MQ_F_BLOCKING 이면 SRCU, 아니면 RCU grace period 대기
		synchronize_srcu(set->srcu);
	else
		synchronize_rcu();
// submit 경로의 readers(hctx/tags 참조)가 완료되기까지 대기 -> NVMe SQ 안전 정지
}
EXPORT_SYMBOL_GPL(blk_mq_wait_quiesce_done);

/**
 * blk_mq_quiesce_queue() - wait until all ongoing dispatches have finished
 * @q: request queue.
 *
 * Note: this function does not prevent that the struct request end_io()
 * callback function is invoked. Once this function is returned, we make
 * sure no dispatch can happen until the queue is unquiesced via
 * blk_mq_unquiesce_queue().
 */
void blk_mq_quiesce_queue(struct request_queue *q)
{
	blk_mq_quiesce_queue_nowait(q);
	/* nothing to wait for non-mq queues */
	if (queue_is_mq(q))
// queue_is_mq(q) 검사: NVMe mq queue 일 때만 quiesce 완료 대기
		blk_mq_wait_quiesce_done(q->tag_set);
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_queue);

/*
 * blk_mq_unquiesce_queue() - counterpart of blk_mq_quiesce_queue()
 * @q: request queue.
 *
 * This function recovers queue into the state before quiescing
 * which is done by blk_mq_quiesce_queue.
 */
void blk_mq_unquiesce_queue(struct request_queue *q)
{
	unsigned long flags;
	bool run_queue = false;

	spin_lock_irqsave(&q->queue_lock, flags);
	if (WARN_ON_ONCE(q->quiesce_depth <= 0)) {
		;
	} else if (!--q->quiesce_depth) {
		blk_queue_flag_clear(QUEUE_FLAG_QUIESCED, q);
// QUEUE_FLAG_QUIESCED 해제: NVMe SQ dispatch 재개
		run_queue = true;
	}
	spin_unlock_irqrestore(&q->queue_lock, flags);

	/* dispatch requests which are inserted during quiescing */
	if (run_queue)
		blk_mq_run_hw_queues(q, true);
// quiesce 동안 쌓인 request 들을 NVMe SQ 로 다시 dispatch
}
EXPORT_SYMBOL_GPL(blk_mq_unquiesce_queue);

void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	rcu_read_lock();
	list_for_each_entry_rcu(q, &set->tag_list, tag_set_list) {
// RCU read lock: tag_set->tag_list 의 request_queue(NVMe namespace) 순회 보호
		if (!blk_queue_skip_tagset_quiesce(q))
			blk_mq_quiesce_queue_nowait(q);
	}
	rcu_read_unlock();

	blk_mq_wait_quiesce_done(set);
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_tagset);

void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	rcu_read_lock();
	list_for_each_entry_rcu(q, &set->tag_list, tag_set_list) {
		if (!blk_queue_skip_tagset_quiesce(q))
			blk_mq_unquiesce_queue(q);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(blk_mq_unquiesce_tagset);

void blk_mq_wake_waiters(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		if (blk_mq_hw_queue_mapped(hctx))
			blk_mq_tag_wakeup_all(hctx->tags, true);
// tag 반납으로 깨어나야 할 sleep 중인 submitter 가 있으면 모두 wakeup
}

/*
 * blk_rq_init: request 구조체를 초기화한다.
 *   NVMe 관점: NVMe 명령 슬롯에 해당하는 request 의 tag 를
 *   BLK_MQ_NO_TAG 로 초기화하고, 이후 blk_mq_rq_ctx_init 에서
 *   실제 CID(tag) 가 할당된다.
 *   호출 경로: blk_mq_alloc_rqs 의 request 풀 초기화.
 */
void blk_rq_init(struct request_queue *q, struct request *rq)
{
	memset(rq, 0, sizeof(*rq));
// request 구조체 영역 초기화: 이후 NVMe CID/tag 가 할당됨

	INIT_LIST_HEAD(&rq->queuelist);
	rq->q = q;
	rq->__sector = (sector_t) -1;
	rq->phys_gap_bit = 0;
	INIT_HLIST_NODE(&rq->hash);
	RB_CLEAR_NODE(&rq->rb_node);
	rq->tag = BLK_MQ_NO_TAG;
// tag = BLK_MQ_NO_TAG: 아직 NVMe SQ slot(CID) 이 할당되지 않음
	rq->internal_tag = BLK_MQ_NO_TAG;
// internal_tag = BLK_MQ_NO_TAG: scheduler 미사용 상태
	rq->start_time_ns = blk_time_get_ns();
	blk_crypto_rq_set_defaults(rq);
}
EXPORT_SYMBOL(blk_rq_init);

/* Set start and alloc time when the allocated request is actually used */
static inline void blk_mq_rq_time_init(struct request *rq, u64 alloc_time_ns)
{
#ifdef CONFIG_BLK_RQ_ALLOC_TIME
	if (blk_queue_rq_alloc_time(rq->q))
		rq->alloc_time_ns = alloc_time_ns;
// request 할당 시간 측정: NVMe IO latency 분석용
	else
		rq->alloc_time_ns = 0;
#endif
}

static inline void blk_mq_bio_issue_init(struct request_queue *q,
					 struct bio *bio)
{
#ifdef CONFIG_BLK_CGROUP
	if (test_bit(QUEUE_FLAG_BIO_ISSUE_TIME, &q->queue_flags))
		bio->issue_time_ns = blk_time_get_ns();
// bio 발행 시각 기록: NVMe end-to-end latency 추적 기반
#endif
}

/*
 * blk_mq_rq_ctx_init: 할당된 tag 를 받아 request 를 완성한다.
 *   NVMe 관점: rq->tag 에 할당받은 CID 를 기록하고, rq->mq_hctx 에
 *   SQ 를 연결한다. rq->cmd_flags 는 NVMe 명령의 opcode/플래그로
 *   변환되는 기초 정보를 담는다.
 *   호출 경로: __blk_mq_alloc_requests -> ... -> nvme_queue_rq.
 */
static struct request *blk_mq_rq_ctx_init(struct blk_mq_alloc_data *data,
		struct blk_mq_tags *tags, unsigned int tag)
{
	struct blk_mq_ctx *ctx = data->ctx;
	struct blk_mq_hw_ctx *hctx = data->hctx;
	struct request_queue *q = data->q;
	struct request *rq = tags->static_rqs[tag];

	rq->q = q;
// rq->q = q: 이 request 가 속한 NVMe namespace 의 request_queue
	rq->mq_ctx = ctx; /* 제출 CPU 와 매핑된 software queue */
	rq->mq_hctx = hctx; /* NVMe SQ 에 해당하는 hardware queue */
	rq->cmd_flags = data->cmd_flags; /* NVMe opcode/flags 의 기초 */

	if (data->flags & BLK_MQ_REQ_PM)
// BLK_MQ_REQ_PM: NVMe power management 관련 명령임을 표시
		data->rq_flags |= RQF_PM;
	rq->rq_flags = data->rq_flags;
// rq->rq_flags: NVMe passthrough, flush, poll 등 특수 명령 플래그 복사

	if (data->rq_flags & RQF_SCHED_TAGS) {
		rq->tag = BLK_MQ_NO_TAG;
		rq->internal_tag = tag; /* scheduler 가 사용하는 내부 tag */
	} else {
		rq->tag = tag; /* NVMe SQ slot 번호, 즉 CID 로 사용 */
		rq->internal_tag = BLK_MQ_NO_TAG;
// scheduler 를 쓰지 않을 때 internal_tag 는 사용되지 않음
	}
	rq->timeout = 0;

	rq->part = NULL;
	rq->io_start_time_ns = 0;
// rq->part: NVMe namespace 의 block_device, account 와 partition 통계용
	rq->stats_sectors = 0;
	rq->nr_phys_segments = 0;
// rq->nr_phys_segments: NVMe PRP/SGL entry 수 계산의 기초 데이터
	rq->nr_integrity_segments = 0;
	rq->end_io = NULL;
// rq->end_io: NVMe 명령 완료 콜백(nvme_complete_rq 등) 등록 대기
	rq->end_io_data = NULL;

	blk_crypto_rq_set_defaults(rq);
	INIT_LIST_HEAD(&rq->queuelist);
	/* tag was already set */
	WRITE_ONCE(rq->deadline, 0);
// deadline 0 으로 초기화: timeout 타이머 재설정 대기
	req_ref_set(rq, 1);
// request 참조 카운트 1: NVMe 명령 생명주기 시작

	if (rq->rq_flags & RQF_USE_SCHED) {
		struct elevator_queue *e = data->q->elevator;

		INIT_HLIST_NODE(&rq->hash);
		RB_CLEAR_NODE(&rq->rb_node);

		if (e->type->ops.prepare_request)
			e->type->ops.prepare_request(rq);
	}

	return rq;
}

static inline struct request *
__blk_mq_alloc_requests_batch(struct blk_mq_alloc_data *data)
{
	unsigned int tag, tag_offset;
	struct blk_mq_tags *tags;
	struct request *rq;
	unsigned long tag_mask;
	int i, nr = 0;

	do {
// 한 번에 여러 NVMe CID(slot) 를 batch 로 할당해 SQ 진입 오버헤드 감소
		tag_mask = blk_mq_get_tags(data, data->nr_tags - nr, &tag_offset);
		if (unlikely(!tag_mask)) {
// tag pool 고갈: NVMe SQ depth 가 꽉 찼거나 경쟁이 심함
			if (nr == 0)
				return NULL;
			break;
		}
		tags = blk_mq_tags_from_data(data);
		for (i = 0; tag_mask; i++) {
// 획득한 bitset 을 순회하며 CID 마다 request 를 초기화
			if (!(tag_mask & (1UL << i)))
				continue;
			tag = tag_offset + i;
			prefetch(tags->static_rqs[tag]);
// CID 에 대응하는 static_rqs[] 를 prefetch: cache 효율 향상
			tag_mask &= ~(1UL << i);
			rq = blk_mq_rq_ctx_init(data, tags, tag);
			rq_list_add_head(data->cached_rqs, rq);
// 초기화된 request 를 plug cache list 앞에 쌓아두기
			nr++;
		}
	} while (data->nr_tags > nr);

	if (!(data->rq_flags & RQF_SCHED_TAGS))
		blk_mq_add_active_requests(data->hctx, nr);
// scheduler tag 가 아니면 hctx(NVMe SQ) 의 활성 CID 카운트 증가
	/* caller already holds a reference, add for remainder */
	percpu_ref_get_many(&data->q->q_usage_counter, nr - 1);
// queue 사용 카운트를 batch 만큼 증가: CID 사용 기간 동안 queue 생존 보장
	data->nr_tags -= nr;

	return rq_list_pop(data->cached_rqs);
}

static void blk_mq_limit_depth(struct blk_mq_alloc_data *data)
{
	struct elevator_mq_ops *ops;

	/* If no I/O scheduler has been configured, don't limit requests */
	if (!data->q->elevator) {
// elevator 미사용 시 NVMe SQ depth 만큼 tag 할당 허용
		blk_mq_tag_busy(data->hctx);
		return;
	}

	/*
	 * All requests use scheduler tags when an I/O scheduler is
	 * enabled for the queue.
	 */
	data->rq_flags |= RQF_SCHED_TAGS;
// scheduler 사용 시 모든 request 는 sched tag 를 거침

	/*
	 * Flush/passthrough requests are special and go directly to the
	 * dispatch list, they are not subject to the async_depth limit.
	 */
	if ((data->cmd_flags & REQ_OP_MASK) == REQ_OP_FLUSH ||
	    blk_op_is_passthrough(data->cmd_flags))
// flush/passthrough 는 NVMe admin/vendor 명령처럼 async_depth 제한 예외
		return;

	WARN_ON_ONCE(data->flags & BLK_MQ_REQ_RESERVED);
	data->rq_flags |= RQF_USE_SCHED;
// RQF_USE_SCHED: NVMe IO scheduler(예: mq-deadline, bfq) 경유 표시

	/*
	 * By default, sync requests have no limit, and async requests are
	 * limited to async_depth.
	 */
	ops = &data->q->elevator->type->ops;
	if (ops->limit_depth)
// IO scheduler 의 limit_depth(): NVMe SQ depth/queue depth 제한 정책 적용
		ops->limit_depth(data->cmd_flags, data);
}

/*
 * __blk_mq_alloc_requests: hctx 와 tag(CID) 를 할당받아 request 를 생성.
 *   NVMe 관점: NVMe SQ 의 빈 slot(CID) 을 sbitmap 으로 확보한다.
 *   tag 가 부족하면 대기하거나 BLK_MQ_REQ_NOWAIT 시 NULL 을 반환.
 *   호출 경로: blk_mq_get_new_requests -> __blk_mq_alloc_requests.
 */
static struct request *__blk_mq_alloc_requests(struct blk_mq_alloc_data *data)
{
	struct request_queue *q = data->q;
	u64 alloc_time_ns = 0;
	struct request *rq;
	unsigned int tag;

	/* alloc_time includes depth and tag waits */
// alloc_time: CID/tag 대기 시간을 포함한 NVMe request 할당 latency
	if (blk_queue_rq_alloc_time(q))
		alloc_time_ns = blk_time_get_ns();

	if (data->cmd_flags & REQ_NOWAIT)
// REQ_NOWAIT: NVMe 명령 할당이 blocking 되지 않아야 함(EAGAIN 기대)
		data->flags |= BLK_MQ_REQ_NOWAIT;

retry:
	data->ctx = blk_mq_get_ctx(q);
// blk_mq_get_ctx(): 제출 CPU 에 해당하는 software queue 획득
	data->hctx = blk_mq_map_queue(data->cmd_flags, data->ctx); /* bio/opcode 를 기반으로 NVMe SQ 선택 */

	blk_mq_limit_depth(data);
	if (data->flags & BLK_MQ_REQ_RESERVED)
// BLK_MQ_REQ_RESERVED: NVMe admin queue 등 예약 tag 사용
		data->rq_flags |= RQF_RESV;

	/*
	 * Try batched alloc if we want more than 1 tag.
	 */
	if (data->nr_tags > 1) {
// nr_tags > 1 이면 batch CID 할당으로 NVMe SQ slot 확보 효율 향상
		rq = __blk_mq_alloc_requests_batch(data);
		if (rq) {
			blk_mq_rq_time_init(rq, alloc_time_ns);
			return rq;
		}
		data->nr_tags = 1;
// batch 실패 시 단일 CID 할당으로 fallback
	}

	/*
	 * Waiting allocations only fail because of an inactive hctx.  In that
	 * case just retry the hctx assignment and tag allocation as CPU hotplug
	 * should have migrated us to an online CPU by now.
	 */
	tag = blk_mq_get_tag(data); /* NVMe SQ 의 빈 CID(slot) 확보 */
	if (tag == BLK_MQ_NO_TAG) {
		if (data->flags & BLK_MQ_REQ_NOWAIT)
// BLK_MQ_REQ_NOWAIT 이고 tag 없으면 즉시 NULL 반환(EAGAIN)
			return NULL;
		/*
		 * Give up the CPU and sleep for a random short time to
		 * ensure that thread using a realtime scheduling class
		 * are migrated off the CPU, and thus off the hctx that
		 * is going away.
		 */
		msleep(3);
// CID 확보 실패 시 3ms sleep 후 CPU/hctx 재선택 시도
		goto retry;
	}

	if (!(data->rq_flags & RQF_SCHED_TAGS))
		blk_mq_inc_active_requests(data->hctx); /* hctx(SQ) 의 활성 CID 카운트 증가 */
// CID 확보 성공 시 hctx(SQ) 의 active CID 카운트 증가
	rq = blk_mq_rq_ctx_init(data, blk_mq_tags_from_data(data), tag);
	blk_mq_rq_time_init(rq, alloc_time_ns);
	return rq;
}

static struct request *blk_mq_rq_cache_fill(struct request_queue *q,
					    struct blk_plug *plug,
					    blk_opf_t opf,
					    blk_mq_req_flags_t flags)
{
	struct blk_mq_alloc_data data = {
// .nr_tags = plug->nr_ios: plug 에 캐싱할 NVMe CID 개수 지정
		.q		= q,
		.flags		= flags,
		.shallow_depth	= 0,
		.cmd_flags	= opf,
		.rq_flags	= 0,
		.nr_tags	= plug->nr_ios,
		.cached_rqs	= &plug->cached_rqs,
		.ctx		= NULL,
		.hctx		= NULL
	};
	struct request *rq;

	if (blk_queue_enter(q, flags))
// queue 사용 카운트 획득: NVMe request 할당 중 queue 생존 보장
		return NULL;

	plug->nr_ios = 1;
// plug cache 채운 후에는 이후 요청당 1개씩 사용

	rq = __blk_mq_alloc_requests(&data);
	if (unlikely(!rq))
		blk_queue_exit(q);
	return rq;
}

static struct request *blk_mq_alloc_cached_request(struct request_queue *q,
						   blk_opf_t opf,
						   blk_mq_req_flags_t flags)
{
	struct blk_plug *plug = current->plug;
	struct request *rq;

	if (!plug)
		return NULL;

	if (rq_list_empty(&plug->cached_rqs)) {
// plug cache 가 비어있으면 새로 NVMe CID batch 할당
		if (plug->nr_ios == 1)
			return NULL;
// plug->nr_ios == 1 이면 cache fill 을 시도하지 않음
		rq = blk_mq_rq_cache_fill(q, plug, opf, flags);
// blk_mq_rq_cache_fill(): plug 에 쌓일 NVMe CID batch 할당
		if (!rq)
			return NULL;
	} else {
		rq = rq_list_peek(&plug->cached_rqs);
		if (!rq || rq->q != q)
// cached request 의 queue 가 다륾면 NVMe namespace 교차 사용 불가
			return NULL;

		if (blk_mq_get_hctx_type(opf) != rq->mq_hctx->type)
// hctx type(read/poll/default) 불일치 시 다른 NVMe SQ 사용 필요
			return NULL;
		if (op_is_flush(rq->cmd_flags) != op_is_flush(opf))
			return NULL;

		rq_list_pop(&plug->cached_rqs);
		blk_mq_rq_time_init(rq, blk_time_get_ns());
	}

	rq->cmd_flags = opf;
// rq->cmd_flags = opf: NVMe opcode/플래그 갱신
	INIT_LIST_HEAD(&rq->queuelist);
	return rq;
}

/*
 * blk_mq_alloc_request: 상위 계층이 직접 request 를 할당할 때 사용.
 *   NVMe 관점: ioctl/passthrough 등에서 NVMe Admin/IO 명령용
 *   request(CID slot) 을 확보한다.
 */
struct request *blk_mq_alloc_request(struct request_queue *q, blk_opf_t opf,
		blk_mq_req_flags_t flags)
{
	struct request *rq;

	rq = blk_mq_alloc_cached_request(q, opf, flags);
// 먼저 plug cache 에서 재사용 가능한 NVMe request 를 찾음
	if (!rq) {
		struct blk_mq_alloc_data data = {
			.q		= q,
			.flags		= flags,
			.shallow_depth	= 0,
			.cmd_flags	= opf,
			.rq_flags	= 0,
			.nr_tags	= 1,
			.cached_rqs	= NULL,
			.ctx		= NULL,
			.hctx		= NULL
		};
		int ret;

		ret = blk_queue_enter(q, flags);
// queue 진입: NVMe namespace 의 request_queue 사용 허가 획득
		if (ret)
			return ERR_PTR(ret);

		rq = __blk_mq_alloc_requests(&data);
// 신규 NVMe CID 를 할당받아 request 생성
		if (!rq)
			goto out_queue_exit;
	}
	rq->__data_len = 0;
// __data_len = 0: 아직 bio 가 연결되지 않은 초기 상태
	rq->phys_gap_bit = 0;
	rq->__sector = (sector_t) -1;
	rq->bio = rq->biotail = NULL;
	return rq;
out_queue_exit:
	blk_queue_exit(q);
// queue 사용 카운트 반납: NVMe request 할당 실패 시
	return ERR_PTR(-EWOULDBLOCK);
}
EXPORT_SYMBOL(blk_mq_alloc_request);

/*
 * blk_mq_alloc_request_hctx: 특정 hctx(특정 NVMe SQ) 에 바인딩된
 *   request 를 할당.
 *   NVMe 관점: 특정 nvme_queue 의 SQ slot 을 직접 지정하여
 *   affinity 를 강제할 때 사용.
 */
struct request *blk_mq_alloc_request_hctx(struct request_queue *q,
	blk_opf_t opf, blk_mq_req_flags_t flags, unsigned int hctx_idx)
{
	struct blk_mq_alloc_data data = {
		.q		= q,
		.flags		= flags,
		.shallow_depth	= 0,
		.cmd_flags	= opf,
		.rq_flags	= 0,
		.nr_tags	= 1,
		.cached_rqs	= NULL,
		.ctx		= NULL,
		.hctx		= NULL
	};
	u64 alloc_time_ns = 0;
	struct request *rq;
	unsigned int cpu;
	unsigned int tag;
	int ret;

	/* alloc_time includes depth and tag waits */
	if (blk_queue_rq_alloc_time(q))
		alloc_time_ns = blk_time_get_ns();

	/*
	 * If the tag allocator sleeps we could get an allocation for a
	 * different hardware context.  No need to complicate the low level
	 * allocator for this for the rare use case of a command tied to
	 * a specific queue.
	 */
	if (WARN_ON_ONCE(!(flags & BLK_MQ_REQ_NOWAIT)) ||
// 특정 hctx 지정은 NOWAIT+RESERVED 조합에서만 지원(희귀 NVMe passthrough)
	    WARN_ON_ONCE(!(flags & BLK_MQ_REQ_RESERVED)))
		return ERR_PTR(-EINVAL);

	if (hctx_idx >= q->nr_hw_queues)
// hctx_idx 가 NVMe SQ 개수를 벗어나면 오류
		return ERR_PTR(-EIO);

	ret = blk_queue_enter(q, flags);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * Check if the hardware context is actually mapped to anything.
	 * If not tell the caller that it should skip this queue.
	 */
	ret = -EXDEV;
	data.hctx = q->queue_hw_ctx[hctx_idx];
// q->queue_hw_ctx[hctx_idx]: 직접 지정한 NVMe SQ(hctx)
	if (!blk_mq_hw_queue_mapped(data.hctx))
		goto out_queue_exit;
	cpu = cpumask_first_and(data.hctx->cpumask, cpu_online_mask);
	if (cpu >= nr_cpu_ids)
// hctx cpumask 에서 online CPU 선택: NVMe SQ 의 제출 CPU 결정
		goto out_queue_exit;
	data.ctx = __blk_mq_get_ctx(q, cpu);

	if (q->elevator)
		data.rq_flags |= RQF_SCHED_TAGS;
// elevator 가 있으면 scheduler tag 경유
	else
		blk_mq_tag_busy(data.hctx);

	if (flags & BLK_MQ_REQ_RESERVED)
		data.rq_flags |= RQF_RESV;

	ret = -EWOULDBLOCK;
	tag = blk_mq_get_tag(&data);
// 특정 NVMe SQ 의 빈 CID(slot) 확보
	if (tag == BLK_MQ_NO_TAG)
		goto out_queue_exit;
	if (!(data.rq_flags & RQF_SCHED_TAGS))
		blk_mq_inc_active_requests(data.hctx);
// scheduler tag 가 아닌 driver tag 이면 active CID 카운트 증가
	rq = blk_mq_rq_ctx_init(&data, blk_mq_tags_from_data(&data), tag);
	blk_mq_rq_time_init(rq, alloc_time_ns);
	rq->__data_len = 0;
// passthrough/admin request 의 데이터 필드 초기화
	rq->phys_gap_bit = 0;
	rq->__sector = (sector_t) -1;
	rq->bio = rq->biotail = NULL;
	return rq;

out_queue_exit:
	blk_queue_exit(q);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(blk_mq_alloc_request_hctx);

static void blk_mq_finish_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	blk_zone_finish_request(rq);

	if (rq->rq_flags & RQF_USE_SCHED) {
		q->elevator->type->ops.finish_request(rq);
		/*
		 * For postflush request that may need to be
		 * completed twice, we should clear this flag
		 * to avoid double finish_request() on the rq.
		 */
		rq->rq_flags &= ~RQF_USE_SCHED;
	}
}

/*
 * __blk_mq_free_request: request 의 tag(CID) 와 참조를 해제.
 *   NVMe 관점: 완료된 NVMe 명령의 CID 를 SQ 의 free bitmap 에
 *   반납하고, active count 를 감소시켜 재사용 가능하게 한다.
 *   호출 경로: blk_mq_free_request -> __blk_mq_free_request.
 */
static void __blk_mq_free_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	const int sched_tag = rq->internal_tag;

	blk_crypto_free_request(rq);
// blk_crypto_free_request(): NVMe encryption keyslot 해제
	blk_pm_mark_last_busy(rq);
// blk_pm_mark_last_busy(): NVMe power management idle 갱신
	rq->mq_hctx = NULL;

	if (rq->tag != BLK_MQ_NO_TAG) { /* 유효한 CID 가 할당된 경우만 반납 */
		blk_mq_dec_active_requests(hctx); /* hctx(SQ) 의 활성 CID 카운트 감소 */
		blk_mq_put_tag(hctx->tags, ctx, rq->tag); /* NVMe SQ slot(CID) 반납 */
	}
	if (sched_tag != BLK_MQ_NO_TAG)
		blk_mq_put_tag(hctx->sched_tags, ctx, sched_tag);
	blk_mq_sched_restart(hctx);
// scheduler restart: NVMe SQ 에 다시 dispatch 할 여지가 있음을 표시
	blk_queue_exit(q);
// queue 사용 카운트 감소: request 생명주기 종료
}

/*
 * blk_mq_free_request: request 의 수명을 종료.
 *   NVMe 관점: NVMe 명령 완료 후 해당 CID slot 을 회수하여
 *   동일 CID 의 재사용이 가능하게 한다.
 *   호출 경로: blk_mq_end_request -> blk_mq_free_request.
 */
void blk_mq_free_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	blk_mq_finish_request(rq);
// scheduler finish_request 수행(NVMe passthrough 가 아닌 일반 IO)

	rq_qos_done(q, rq);
// rq_qos_done(): NVMe IO QoS(latency/iocost) 완료 통보

	WRITE_ONCE(rq->state, MQ_RQ_IDLE);
// rq->state = MQ_RQ_IDLE: NVMe CID 가 이제 재할당 가능함
	if (req_ref_put_and_test(rq))
// 참조 카운트가 0이 되면 __blk_mq_free_request() 로 CID 반납
		__blk_mq_free_request(rq);
}
EXPORT_SYMBOL_GPL(blk_mq_free_request);

void blk_mq_free_plug_rqs(struct blk_plug *plug)
{
	struct request *rq;

	while ((rq = rq_list_pop(&plug->cached_rqs)) != NULL)
// plug cache 에 남은 request 들을 해제(CID 반납)
		blk_mq_free_request(rq);
}

void blk_dump_rq_flags(struct request *rq, char *msg)
{
	printk(KERN_INFO "%s: dev %s: flags=%llx\n", msg,
		rq->q->disk ? rq->q->disk->disk_name : "?",
		(__force unsigned long long) rq->cmd_flags);

	printk(KERN_INFO "  sector %llu, nr/cnr %u/%u\n",
	       (unsigned long long)blk_rq_pos(rq),
	       blk_rq_sectors(rq), blk_rq_cur_sectors(rq));
	printk(KERN_INFO "  bio %p, biotail %p, len %u\n",
	       rq->bio, rq->biotail, blk_rq_bytes(rq));
}
EXPORT_SYMBOL(blk_dump_rq_flags);

static void blk_account_io_completion(struct request *req, unsigned int bytes)
{
	if (req->rq_flags & RQF_IO_STAT) {
// IO 통계 수집: NVMe namespace 별 sectors 완료량 기록
		const int sgrp = op_stat_group(req_op(req));

		part_stat_lock();
		part_stat_add(req->part, sectors[sgrp], bytes >> 9);
// part_stat_add(sectors): NVMe namespace 에 완료한 sector 수 누적
		part_stat_unlock();
	}
}

static void blk_print_req_error(struct request *req, blk_status_t status)
{
	printk_ratelimited(KERN_ERR
		"%s error, dev %s, sector %llu op 0x%x:(%s) flags 0x%x "
		"phys_seg %u prio class %u\n",
		blk_status_to_str(status),
		req->q->disk ? req->q->disk->disk_name : "?",
		blk_rq_pos(req), (__force u32)req_op(req),
		blk_op_str(req_op(req)),
		(__force u32)(req->cmd_flags & ~REQ_OP_MASK),
		req->nr_phys_segments,
		IOPRIO_PRIO_CLASS(req_get_ioprio(req)));
}

/*
 * Fully end IO on a request. Does not support partial completions, or
 * errors.
 */
static void blk_complete_request(struct request *req)
{
	const bool is_flush = (req->rq_flags & RQF_FLUSH_SEQ) != 0;
	int total_bytes = blk_rq_bytes(req);
	struct bio *bio = req->bio;

	trace_block_rq_complete(req, BLK_STS_OK, total_bytes);

	if (!bio)
// bio 가 없으면 상위 계층으로 전달할 데이터 없음(NVMe passthrough 등)
		return;

	if (blk_integrity_rq(req) && req_op(req) == REQ_OP_READ)
// READ + integrity 시 NVMe PI(Protection Information) 완료 처리
		blk_integrity_complete(req, total_bytes);

	/*
	 * Upper layers may call blk_crypto_evict_key() anytime after the last
	 * bio_endio().  Therefore, the keyslot must be released before that.
	 */
	blk_crypto_rq_put_keyslot(req);
// blk_crypto keyslot 해제: NVMe encryption 명령 종료 후 즉시 반납

	blk_account_io_completion(req, total_bytes);

	do {
// request 에 연결된 모든 bio 를 순회하며 완료
		struct bio *next = bio->bi_next;

		/* Completion has already been traced */
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);

		if (blk_req_bio_is_zone_append(req, bio))
			blk_zone_append_update_request_bio(req, bio);

		if (!is_flush)
// flush sequence 를 제외하고 bio_endio() 로 상위로 완료 전달
			bio_endio(bio);
		bio = next;
	} while (bio);

	/*
	 * Reset counters so that the request stacking driver
	 * can find how many bytes remain in the request
	 * later.
	 */
	if (!req->end_io) {
		req->bio = NULL;
// 완료 후 request 의 데이터/sector 카운터 초기화
		req->__data_len = 0;
	}
}

/**
 * blk_update_request - Complete multiple bytes without completing the request
 * @req:      the request being processed
 * @error:    block status code
 * @nr_bytes: number of bytes to complete for @req
 *
 * Description:
 *     Ends I/O on a number of bytes attached to @req, but doesn't complete
 *     the request structure even if @req doesn't have leftover.
 *     If @req has leftover, sets it up for the next range of segments.
 *
 *     Passing the result of blk_rq_bytes() as @nr_bytes guarantees
 *     %false return from this function.
 *
 * Note:
 *	The RQF_SPECIAL_PAYLOAD flag is ignored on purpose in this function
 *      except in the consistency check at the end of this function.
 *
 * Return:
 *     %false - this request doesn't have any more data
 *     %true  - this request has more data
 **/
/*
 * blk_update_request: request 의 일부 바이트만 완료 처리.
 *   NVMe 관점: 대용량 PRP/SGL 전송이 여러 bio 로 구성될 때
 *   일부 섹터만 완료되면 나머지를 다음 단계로 재설정한다.
 */
bool blk_update_request(struct request *req, blk_status_t error,
		unsigned int nr_bytes)
{
	bool is_flush = req->rq_flags & RQF_FLUSH_SEQ;
// RQF_FLUSH_SEQ: NVMe flush 명령 시퀀스 중인지 확인
	bool quiet = req->rq_flags & RQF_QUIET;
	int total_bytes;

	trace_block_rq_complete(req, error, nr_bytes);

	if (!req->bio)
// bio 가 없으면 더 이상 완료할 세그먼트 없음
		return false;

	if (blk_integrity_rq(req) && req_op(req) == REQ_OP_READ &&
// integrity READ 가 성공하면 NVMe PI 검증 데이터 복사
	    error == BLK_STS_OK)
		blk_integrity_complete(req, nr_bytes);

	/*
	 * Upper layers may call blk_crypto_evict_key() anytime after the last
	 * bio_endio().  Therefore, the keyslot must be released before that.
	 */
	if (blk_crypto_rq_has_keyslot(req) && nr_bytes >= blk_rq_bytes(req))
// 모든 바이트 완료 시 encryption keyslot 해제
		__blk_crypto_rq_put_keyslot(req);

	if (unlikely(error && !blk_rq_is_passthrough(req) && !quiet) &&
// 오류 발생 시 NVMe 명령 실패 로그 출력(디스크가 살아있을 때)
	    !test_bit(GD_DEAD, &req->q->disk->state)) {
		blk_print_req_error(req, error);
		trace_block_rq_error(req, error, nr_bytes);
	}

	blk_account_io_completion(req, nr_bytes);

	total_bytes = 0;
	while (req->bio) {
		struct bio *bio = req->bio;
		unsigned bio_bytes = min(bio->bi_iter.bi_size, nr_bytes);
// 이번에 완료할 바이트 수 = min(남은 bio 크기, nr_bytes)

		if (unlikely(error))
// NVMe 명령 실패 시 상위 bio 에 error status 전파
			bio->bi_status = error;

		if (bio_bytes == bio->bi_iter.bi_size) {
			req->bio = bio->bi_next;
// bio 전체가 완료되면 다음 bio 로 진행
		} else if (bio_is_zone_append(bio) && error == BLK_STS_OK) {
			/*
			 * Partial zone append completions cannot be supported
			 * as the BIO fragments may end up not being written
			 * sequentially.
			 */
			bio->bi_status = BLK_STS_IOERR;
		}

		/* Completion has already been traced */
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);
		if (unlikely(quiet))
			bio_set_flag(bio, BIO_QUIET);

		bio_advance(bio, bio_bytes);
// bio_advance(): NVMe PRP/SGL 의 다음 세그먼트로 iterator 이동

		/* Don't actually finish bio if it's part of flush sequence */
		if (!bio->bi_iter.bi_size) {
			if (blk_req_bio_is_zone_append(req, bio))
				blk_zone_append_update_request_bio(req, bio);
			if (!is_flush)
				bio_endio(bio);
// flush sequence 가 아닌 일반 NVMe IO bio 완료
		}

		total_bytes += bio_bytes;
		nr_bytes -= bio_bytes;

		if (!nr_bytes)
			break;
	}

	/*
	 * completely done
	 */
	if (!req->bio) {
		/*
		 * Reset counters so that the request stacking driver
		 * can find how many bytes remain in the request
		 * later.
		 */
		req->__data_len = 0;
// 모든 bio 완료 후 request 의 data_len 0 으로 초기화
		return false;
	}

	req->__data_len -= total_bytes;
// 일부만 완료되면 남은 data_len 감소

	/* update sector only for requests with clear definition of sector */
	if (!blk_rq_is_passthrough(req))
		req->__sector += total_bytes >> 9;
// sector 갱신: NVMe LBA offset 이 다음 미완료 영역을 가리킴

	/* mixed attributes always follow the first bio */
	if (req->rq_flags & RQF_MIXED_MERGE) {
		req->cmd_flags &= ~REQ_FAILFAST_MASK;
		req->cmd_flags |= req->bio->bi_opf & REQ_FAILFAST_MASK;
	}

	if (!(req->rq_flags & RQF_SPECIAL_PAYLOAD)) {
		/*
		 * If total number of sectors is less than the first segment
		 * size, something has gone terribly wrong.
		 */
		if (blk_rq_bytes(req) < blk_rq_cur_bytes(req)) {
			blk_dump_rq_flags(req, "request botched");
			req->__data_len = blk_rq_cur_bytes(req);
		}

		/* recalculate the number of segments */
		req->nr_phys_segments = blk_recalc_rq_segments(req);
// 남은 세그먼트 수 재계산: NVMe PRP/SGL entry 수 보정
	}

	return true;
}
EXPORT_SYMBOL_GPL(blk_update_request);

static inline void blk_account_io_done(struct request *req, u64 now)
{
	trace_block_io_done(req);

	/*
	 * Account IO completion.  flush_rq isn't accounted as a
	 * normal IO on queueing nor completion.  Accounting the
	 * containing request is enough.
	 */
	if ((req->rq_flags & (RQF_IO_STAT|RQF_FLUSH_SEQ)) == RQF_IO_STAT) {
// flush_rq 를 제외한 일반 NVMe IO 만 통계 집계
		const int sgrp = op_stat_group(req_op(req));

		part_stat_lock();
		update_io_ticks(req->part, jiffies, true);
		part_stat_inc(req->part, ios[sgrp]);
		part_stat_add(req->part, nsecs[sgrp], now - req->start_time_ns);
// 완료까지 소요된 nsec 누적: NVMe IO latency 통계
		part_stat_local_dec(req->part,
				    in_flight[op_is_write(req_op(req))]);
// in_flight 카운트 감소: NVMe SQ 에서 나간 CID 반영
		part_stat_unlock();
	}
}

static inline bool blk_rq_passthrough_stats(struct request *req)
{
	struct bio *bio = req->bio;

	if (!blk_queue_passthrough_stat(req->q))
		return false;

	/* Requests without a bio do not transfer data. */
	if (!bio)
		return false;

	/*
	 * Stats are accumulated in the bdev, so must have one attached to a
	 * bio to track stats. Most drivers do not set the bdev for passthrough
	 * requests, but nvme is one that will set it.
	 */
	if (!bio->bi_bdev)
		return false;

	/*
	 * We don't know what a passthrough command does, but we know the
	 * payload size and data direction. Ensuring the size is aligned to the
	 * block size filters out most commands with payloads that don't
	 * represent sector access.
	 */
	if (blk_rq_bytes(req) & (bdev_logical_block_size(bio->bi_bdev) - 1))
		return false;
	return true;
}

static inline void blk_account_io_start(struct request *req)
{
	trace_block_io_start(req);

	if (!blk_queue_io_stat(req->q))
// IO 통계 미수집 queue 면 account 생략
		return;
	if (blk_rq_is_passthrough(req) && !blk_rq_passthrough_stats(req))
		return;

	req->rq_flags |= RQF_IO_STAT;
	req->start_time_ns = blk_time_get_ns();
// request 시작 시각 기록: NVMe latency 측정 시작점

	/*
	 * All non-passthrough requests are created from a bio with one
	 * exception: when a flush command that is part of a flush sequence
	 * generated by the state machine in blk-flush.c is cloned onto the
	 * lower device by dm-multipath we can get here without a bio.
	 */
	if (req->bio)
		req->part = req->bio->bi_bdev;
// bio->bi_bdev: NVMe namespace block_device 와 연결
	else
		req->part = req->q->disk->part0;

	part_stat_lock();
	update_io_ticks(req->part, jiffies, false);
	part_stat_local_inc(req->part, in_flight[op_is_write(req_op(req))]);
// in_flight 카운트 증가: NVMe SQ 로 들어간 CID 반영
	part_stat_unlock();
}

static inline void __blk_mq_end_request_acct(struct request *rq, u64 now)
{
	if (rq->rq_flags & RQF_STATS)
		blk_stat_add(rq, now);
// RQF_STATS: NVMe IO latency histogram/blktrace 기록

	blk_mq_sched_completed_request(rq, now);
	blk_account_io_done(rq, now);
}

/*
 * __blk_mq_end_request: request 의 최종 완료 후 정리.
 *   NVMe 관점: NVMe 컨트롤러가 CQ 항목을 기록한 뒤,
 *   nvme_complete_rq -> blk_mq_end_request -> __blk_mq_end_request
 *   순으로 호출되어 상위 계층으로 완료를 전달.
 */
inline void __blk_mq_end_request(struct request *rq, blk_status_t error)
{
	if (blk_mq_need_time_stamp(rq))
// 타임스탬프 필요 시 완료 시각 기록
		__blk_mq_end_request_acct(rq, blk_time_get_ns());

	blk_mq_finish_request(rq);

	if (rq->end_io) {
// rq->end_io: NVMe 명령별 완료 콜백이 있으면 호출
		rq_qos_done(rq->q, rq);
		if (rq->end_io(rq, error, NULL) == RQ_END_IO_FREE)
// 콜백이 RQ_END_IO_FREE 를 반환하면 request/CID 해제
			blk_mq_free_request(rq);
	} else {
		blk_mq_free_request(rq);
	}
}
EXPORT_SYMBOL(__blk_mq_end_request);

/*
 * blk_mq_end_request: request 의 모든 바이트를 완료하고 정리.
 *   NVMe 관점: 하나의 NVMe 명령(CID) 이 전체 완료되면 호출.
 *   호출 경로: nvme_complete_rq -> blk_mq_end_request.
 */
void blk_mq_end_request(struct request *rq, blk_status_t error)
{
	if (blk_update_request(rq, error, blk_rq_bytes(rq)))
// request 의 모든 바이트를 완료 처리
		BUG();
	__blk_mq_end_request(rq, error);
}
EXPORT_SYMBOL(blk_mq_end_request);

#define TAG_COMP_BATCH		32

static inline void blk_mq_flush_tag_batch(struct blk_mq_hw_ctx *hctx,
					  int *tag_array, int nr_tags)
{
	struct request_queue *q = hctx->queue;

	blk_mq_sub_active_requests(hctx, nr_tags);
// batch 완료 시 hctx(NVMe SQ) 의 active CID 카운트 일괄 감소

	blk_mq_put_tags(hctx->tags, tag_array, nr_tags);
// 획득했던 NVMe CID 들을 batch 로 반납
	percpu_ref_put_many(&q->q_usage_counter, nr_tags);
// queue 사용 카운트를 batch 만큼 감소
}

/*
 * blk_mq_end_request_batch: 여러 request 를 일괄 완료.
 *   NVMe 관점: CQ 에서 여러 완료 항목을 한 번에 처리할 때
 *   tag 를 batch 로 반납하여 오버헤드를 줄인다.
 */
void blk_mq_end_request_batch(struct io_comp_batch *iob)
{
	int tags[TAG_COMP_BATCH], nr_tags = 0;
	struct blk_mq_hw_ctx *cur_hctx = NULL;
	struct request *rq;
	u64 now = 0;

	if (iob->need_ts)
		now = blk_time_get_ns();

	while ((rq = rq_list_pop(&iob->req_list)) != NULL) {
		prefetch(rq->bio);
// 다음 완료될 rq->bio 와 rq_next 를 prefetch
		prefetch(rq->rq_next);

		blk_complete_request(rq);
// bio 완료 처리: NVMe CQ 항목 1개 상위 전달
		if (iob->need_ts)
			__blk_mq_end_request_acct(rq, now);
// 타임스탬프 수집 시 정확한 NVMe 완료 latency 기록

		blk_mq_finish_request(rq);

		rq_qos_done(rq->q, rq);

		/*
		 * If end_io handler returns NONE, then it still has
		 * ownership of the request.
		 */
		if (rq->end_io && rq->end_io(rq, 0, iob) == RQ_END_IO_NONE)
// end_io 콜백이 RQ_END_IO_NONE 이면 request 소유권은 아직 드라이버에 있음
			continue;

		WRITE_ONCE(rq->state, MQ_RQ_IDLE);
// MQ_RQ_IDLE: NVMe CID 재사용 가능
		if (!req_ref_put_and_test(rq))
			continue;

		blk_crypto_free_request(rq);
		blk_pm_mark_last_busy(rq);

		if (nr_tags == TAG_COMP_BATCH || cur_hctx != rq->mq_hctx) {
// hctx 가 바뀌거나 batch 가 차면 지금까지 모은 CID 반납
			if (cur_hctx)
				blk_mq_flush_tag_batch(cur_hctx, tags, nr_tags);
			nr_tags = 0;
			cur_hctx = rq->mq_hctx;
		}
		tags[nr_tags++] = rq->tag;
// 완료된 request 의 CID 를 batch 버퍼에 저장
	}

	if (nr_tags)
		blk_mq_flush_tag_batch(cur_hctx, tags, nr_tags);
}
EXPORT_SYMBOL_GPL(blk_mq_end_request_batch);

static void blk_complete_reqs(struct llist_head *list)
{
	struct llist_node *entry = llist_reverse_order(llist_del_all(list));
// llist_reverse_order(): NVMe CQ 처리 순서 보장
	struct request *rq, *next;

	llist_for_each_entry_safe(rq, next, entry, ipi_list)
		rq->q->mq_ops->complete(rq); /* nvme_complete_rq 로 CQ 항목 처리 */
}

static __latent_entropy void blk_done_softirq(void)
{
	blk_complete_reqs(this_cpu_ptr(&blk_cpu_done));
// this_cpu_ptr(blk_cpu_done): 현재 CPU 에 도착한 NVMe 완료 항목 처리
}

static int blk_softirq_cpu_dead(unsigned int cpu)
{
	blk_complete_reqs(&per_cpu(blk_cpu_done, cpu));
	return 0;
}

static void __blk_mq_complete_request_remote(void *data)
{
	__raise_softirq_irqoff(BLOCK_SOFTIRQ);
}

static inline bool blk_mq_complete_need_ipi(struct request *rq)
{
	int cpu = raw_smp_processor_id();

	if (!IS_ENABLED(CONFIG_SMP) ||
	    !test_bit(QUEUE_FLAG_SAME_COMP, &rq->q->queue_flags))
		return false;
	/*
	 * With force threaded interrupts enabled, raising softirq from an SMP
	 * function call will always result in waking the ksoftirqd thread.
	 * This is probably worse than completing the request on a different
	 * cache domain.
	 */
	if (force_irqthreads())
		return false;

	/* same CPU or cache domain and capacity?  Complete locally */
	if (cpu == rq->mq_ctx->cpu ||
	    (!test_bit(QUEUE_FLAG_SAME_FORCE, &rq->q->queue_flags) &&
	     cpus_share_cache(cpu, rq->mq_ctx->cpu) &&
	     cpus_equal_capacity(cpu, rq->mq_ctx->cpu)))
		return false;

	/* don't try to IPI to an offline CPU */
	return cpu_online(rq->mq_ctx->cpu);
}

static void blk_mq_complete_send_ipi(struct request *rq)
{
	unsigned int cpu;

	cpu = rq->mq_ctx->cpu;
	if (llist_add(&rq->ipi_list, &per_cpu(blk_cpu_done, cpu)))
// llist_add(): NVMe 완료 request 를 대상 CPU 의 softirq list 에 삽입
		smp_call_function_single_async(cpu, &per_cpu(blk_cpu_csd, cpu));
// smp_call_function_single_async(): 대상 CPU 에 softirq/IPI 발송
}

static void blk_mq_raise_softirq(struct request *rq)
{
	struct llist_head *list;

	preempt_disable();
	list = this_cpu_ptr(&blk_cpu_done);
	if (llist_add(&rq->ipi_list, list))
// 현재 CPU 의 blk_cpu_done 리스트에 완료 request 추가
		raise_softirq(BLOCK_SOFTIRQ);
// BLOCK_SOFTIRQ 발동: NVMe CQ 인터럽트 bottom-half 시작
	preempt_enable();
}

/*
 * blk_mq_complete_request_remote: 완료를 다른 CPU 로 발송(IPI)할지 결정.
 *   NVMe 관점: NVMe CQ 인터럽트가 발생한 CPU 와 bio 를 제출한 CPU 가
 *   다를 경우, cache affinity 를 위해 softirq/IPI 를 통해 해당 CPU 로
 *   완료를 라우팅한다.
 */
bool blk_mq_complete_request_remote(struct request *rq)
{
	WRITE_ONCE(rq->state, MQ_RQ_COMPLETE);
// MQ_RQ_COMPLETE: NVMe CQ 가 이 request 를 보고했음

	/*
	 * For request which hctx has only one ctx mapping,
	 * or a polled request, always complete locally,
	 * it's pointless to redirect the completion.
	 */
	if ((rq->mq_hctx->nr_ctx == 1 &&
// hctx 가 ctx 를 1개만 갖고 같은 CPU 면 굳이 IPI 안 함
	     rq->mq_ctx->cpu == raw_smp_processor_id()) ||
	     rq->cmd_flags & REQ_POLLED)
		return false;

	if (blk_mq_complete_need_ipi(rq)) {
// cache affinity 를 위해 NVMe 완료를 다른 CPU 로 IPI 발송
		blk_mq_complete_send_ipi(rq);
		return true;
	}

	if (rq->q->nr_hw_queues == 1) {
// hw queue 가 1개면 local softirq 로 완료 라우팅
		blk_mq_raise_softirq(rq);
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(blk_mq_complete_request_remote);

/**
 * blk_mq_complete_request - end I/O on a request
 * @rq:		the request being processed
 *
 * Description:
 *	Complete a request by scheduling the ->complete_rq operation.
 **/
/*
 * blk_mq_complete_request: NVMe 완료 인터럽트의 상위 진입점.
 *   NVMe 관점: nvme_irq / nvme_poll  -> blk_mq_complete_request
 *   -> mq_ops->complete (nvme_complete_rq) 로 이어진다.
 *   remote complete 가 필요하면 IPI/softirq 를, 아니면 즉시
 *   driver 의 complete 콜백을 호출.
 */
void blk_mq_complete_request(struct request *rq)
{
	if (!blk_mq_complete_request_remote(rq)) /* 완료 CPU 라우팅 결정 */
		rq->q->mq_ops->complete(rq); /* nvme_complete_rq 로 CQ 항목 처리 */
}
EXPORT_SYMBOL(blk_mq_complete_request);

/**
 * blk_mq_start_request - Start processing a request
 * @rq: Pointer to request to be started
 *
 * Function used by device drivers to notify the block layer that a request
 * is going to be processed now, so blk layer can do proper initializations
 * such as starting the timeout timer.
 */
/*
 * blk_mq_start_request: driver 가 request 를 실제로 처리하기 직전에 호출.
 *   NVMe 관점: nvme_queue_rq 가 SQ 에 명령을 기록하고 doorbell 을
 *   울리기 전, rq->state 를 MQ_RQ_IN_FLIGHT 로 변경하고 timeout
 *   timer 를 시작. tags->rqs[tag] 에 rq 를 등록하여 완료 시 CID 로
 *   request 를 역참조할 수 있게 한다.
 */
void blk_mq_start_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	trace_block_rq_issue(rq);

	if (test_bit(QUEUE_FLAG_STATS, &q->queue_flags) &&
// 통계 수집 queue 에서만 NVMe IO 세부 latency 기록
	    !blk_rq_is_passthrough(rq)) {
		rq->io_start_time_ns = blk_time_get_ns();
		rq->stats_sectors = blk_rq_sectors(rq);
		rq->rq_flags |= RQF_STATS;
		rq_qos_issue(q, rq);
	}

	WARN_ON_ONCE(blk_mq_rq_state(rq) != MQ_RQ_IDLE);

	blk_add_timer(rq); /* NVMe 명령 deadline 타이머 시작 */
	WRITE_ONCE(rq->state, MQ_RQ_IN_FLIGHT); /* NVMe 명령이 SQ 에 제출됨 */
	rq->mq_hctx->tags->rqs[rq->tag] = rq; /* CID 로 request 역참조 가능하게 매핑 */

	if (blk_integrity_rq(rq) && req_op(rq) == REQ_OP_WRITE)
// WRITE 일 때 integrity(PI) metadata 준비
		blk_integrity_prepare(rq);

	if (rq->bio && rq->bio->bi_opf & REQ_POLLED)
// REQ_POLLED: 인터럽트 없이 NVMe poll queue CQ 를 직접 폴리
	        WRITE_ONCE(rq->bio->bi_cookie, rq->mq_hctx->queue_num);
}
EXPORT_SYMBOL(blk_mq_start_request);

/*
 * Allow 2x BLK_MAX_REQUEST_COUNT requests on plug queue for multiple
 * queues. This is important for md arrays to benefit from merging
 * requests.
 */
static inline unsigned short blk_plug_max_rq_count(struct blk_plug *plug)
{
	if (plug->multiple_queues)
// multiple_queues 시 plug 크기 2배: 여러 NVMe SQ 병렬 활용
		return BLK_MAX_REQUEST_COUNT * 2;
	return BLK_MAX_REQUEST_COUNT;
}

static void blk_add_rq_to_plug(struct blk_plug *plug, struct request *rq)
{
	struct request *last = rq_list_peek(&plug->mq_list);

	if (!plug->rq_count) {
		trace_block_plug(rq->q);
	} else if (plug->rq_count >= blk_plug_max_rq_count(plug) ||
// plug 용량 초과 또는 큰 IO 가 쌓이면 plug flush -> NVMe SQ 로 발행
		   (!blk_queue_nomerges(rq->q) &&
		    blk_rq_bytes(last) >= BLK_PLUG_FLUSH_SIZE)) {
		blk_mq_flush_plug_list(plug, false);
		last = NULL;
		trace_block_plug(rq->q);
	}

	if (!plug->multiple_queues && last && last->q != rq->q)
// 서로 다른 queue(bio)가 섞이면 multiple_queues 표시
		plug->multiple_queues = true;
	/*
	 * Any request allocated from sched tags can't be issued to
	 * ->queue_rqs() directly
	 */
	if (!plug->has_elevator && (rq->rq_flags & RQF_SCHED_TAGS))
// sched tag 를 사용하는 request 가 있으면 elevator 경유 표시
		plug->has_elevator = true;
	rq_list_add_tail(&plug->mq_list, rq);
	plug->rq_count++;
}

/**
 * blk_execute_rq_nowait - insert a request to I/O scheduler for execution
 * @rq:		request to insert
 * @at_head:    insert request at head or tail of queue
 *
 * Description:
 *    Insert a fully prepared request at the back of the I/O scheduler queue
 *    for execution.  Don't wait for completion.
 *
 * Note:
 *    This function will invoke @done directly if the queue is dead.
 */
/*
 * blk_execute_rq_nowait: passthrough request 를 큐에 삽입.
 *   NVMe 관점: NVMe ioctl/admin 명령을 scheduler 를 우회하거나
 *   거쳐서 NVMe SQ 로 전달.
 */
void blk_execute_rq_nowait(struct request *rq, bool at_head)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	WARN_ON(irqs_disabled());
	WARN_ON(!blk_rq_is_passthrough(rq));

	blk_account_io_start(rq);

	if (current->plug && !at_head) {
// plug 가 있으면 NVMe IO 를 모아두고 merge/일괄 발행
		blk_add_rq_to_plug(current->plug, rq);
		return;
	}

	blk_mq_insert_request(rq, at_head ? BLK_MQ_INSERT_AT_HEAD : 0);
// passthrough/admin request 를 NVMe SQ dispatch list 에 삽입
	blk_mq_run_hw_queue(hctx, hctx->flags & BLK_MQ_F_BLOCKING);
// hctx->flags & BLK_MQ_F_BLOCKING 에 따라 async/sync run 선택
}
EXPORT_SYMBOL_GPL(blk_execute_rq_nowait);

/*
 * struct blk_rq_wait: 동기 passthrough request 의 완료 대기 구조체.
 *   done: NVMe admin/vendor 명령 완료를 기다리는 completion.
 *   ret: NVMe 명령의 최종 status (SCSI/NVMe 상태가 아닌 blk_status_t).
 */
struct blk_rq_wait {
	struct completion done; /* 동기 passthrough 완료 대기 */
	blk_status_t ret;
};

static enum rq_end_io_ret blk_end_sync_rq(struct request *rq, blk_status_t ret,
					  const struct io_comp_batch *iob)
{
	struct blk_rq_wait *wait = rq->end_io_data;

	wait->ret = ret;
	complete(&wait->done);
	return RQ_END_IO_NONE;
}

bool blk_rq_is_poll(struct request *rq)
{
	if (!rq->mq_hctx)
		return false;
	if (rq->mq_hctx->type != HCTX_TYPE_POLL)
		return false;
	return true;
}
EXPORT_SYMBOL_GPL(blk_rq_is_poll);

static void blk_rq_poll_completion(struct request *rq, struct completion *wait)
{
	do {
		blk_hctx_poll(rq->q, rq->mq_hctx, NULL, BLK_POLL_ONESHOT);
		cond_resched();
	} while (!completion_done(wait));
}

/**
 * blk_execute_rq - insert a request into queue for execution
 * @rq:		request to insert
 * @at_head:    insert request at head or tail of queue
 *
 * Description:
 *    Insert a fully prepared request at the back of the I/O scheduler queue
 *    for execution and wait for completion.
 * Return: The blk_status_t result provided to blk_mq_end_request().
 */
/*
 * blk_execute_rq: 동기 passthrough request 실행.
 *   NVMe 관점: NVMe Identify, Get Features 등 admin/vendor 명령을
 *   동기적으로 제출하고 CQ 완료를 기다린다.
 */
blk_status_t blk_execute_rq(struct request *rq, bool at_head)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct blk_rq_wait wait = {
		.done = COMPLETION_INITIALIZER_ONSTACK(wait.done),
	};

	WARN_ON(irqs_disabled());
	WARN_ON(!blk_rq_is_passthrough(rq));

	rq->end_io_data = &wait;
// rq->end_io_data = &wait: NVMe admin 명령 동기 완료용 completion 연결
	rq->end_io = blk_end_sync_rq;

	blk_account_io_start(rq);
	blk_mq_insert_request(rq, at_head ? BLK_MQ_INSERT_AT_HEAD : 0);
// NVMe admin/vendor 명령을 hctx->dispatch 또는 scheduler 에 삽입
	blk_mq_run_hw_queue(hctx, false);

	if (blk_rq_is_poll(rq))
// poll queue 면 인터럽트 없이 CQ 폴링으로 완료 대기
		blk_rq_poll_completion(rq, &wait.done);
	else
		blk_wait_io(&wait.done);

	return wait.ret;
}
EXPORT_SYMBOL(blk_execute_rq);

static void __blk_mq_requeue_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	blk_mq_put_driver_tag(rq);
// NVMe driver tag(CID) 반납 후 재삽입 준비

	trace_block_rq_requeue(rq);
	rq_qos_requeue(q, rq);

	if (blk_mq_request_started(rq)) {
		WRITE_ONCE(rq->state, MQ_RQ_IDLE);
// requeue 전에 MQ_RQ_IDLE 로 되돌려 CID 재할당 가능하게 함
		rq->rq_flags &= ~RQF_TIMED_OUT;
	}
}

void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list)
{
	struct request_queue *q = rq->q;
	unsigned long flags;

	__blk_mq_requeue_request(rq);

	/* this request will be re-inserted to io scheduler queue */
	blk_mq_sched_requeue_request(rq);

	spin_lock_irqsave(&q->requeue_lock, flags);
	list_add_tail(&rq->queuelist, &q->requeue_list);
// requeue_list 에 추가: NVMe SQ dispatch 보류 queue
	spin_unlock_irqrestore(&q->requeue_lock, flags);

	if (kick_requeue_list)
		blk_mq_kick_requeue_list(q);
}
EXPORT_SYMBOL(blk_mq_requeue_request);

static void blk_mq_requeue_work(struct work_struct *work)
{
	struct request_queue *q =
		container_of(work, struct request_queue, requeue_work.work);
	LIST_HEAD(rq_list);
	LIST_HEAD(flush_list);
	struct request *rq;

	spin_lock_irq(&q->requeue_lock);
	list_splice_init(&q->requeue_list, &rq_list);
// requeue_list 의 request 들을 모두 꺼냄
	list_splice_init(&q->flush_list, &flush_list);
	spin_unlock_irq(&q->requeue_lock);

	while (!list_empty(&rq_list)) {
		rq = list_entry(rq_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		/*
		 * If RQF_DONTPREP is set, the request has been started by the
		 * driver already and might have driver-specific data allocated
		 * already.  Insert it into the hctx dispatch list to avoid
		 * block layer merges for the request.
		 */
		if (rq->rq_flags & RQF_DONTPREP)
// RQF_DONTPREP: 이미 driver 가 준비한 NVMe 명령이면 scheduler 우회
			blk_mq_request_bypass_insert(rq, 0);
		else
			blk_mq_insert_request(rq, BLK_MQ_INSERT_AT_HEAD);
	}

	while (!list_empty(&flush_list)) {
		rq = list_entry(flush_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		blk_mq_insert_request(rq, 0);
	}

	blk_mq_run_hw_queues(q, false);
}

void blk_mq_kick_requeue_list(struct request_queue *q)
{
	kblockd_mod_delayed_work_on(WORK_CPU_UNBOUND, &q->requeue_work, 0);
// kblockd workqueue 에서 requeue_list 를 다시 dispatch 시도
}
EXPORT_SYMBOL(blk_mq_kick_requeue_list);

void blk_mq_delay_kick_requeue_list(struct request_queue *q,
				    unsigned long msecs)
{
	kblockd_mod_delayed_work_on(WORK_CPU_UNBOUND, &q->requeue_work,
				    msecs_to_jiffies(msecs));
}
EXPORT_SYMBOL(blk_mq_delay_kick_requeue_list);

static bool blk_is_flush_data_rq(struct request *rq)
{
	return (rq->rq_flags & RQF_FLUSH_SEQ) && !is_flush_rq(rq);
}

static bool blk_mq_rq_inflight(struct request *rq, void *priv)
{
	/*
	 * If we find a request that isn't idle we know the queue is busy
	 * as it's checked in the iter.
	 * Return false to stop the iteration.
	 *
	 * In case of queue quiesce, if one flush data request is completed,
	 * don't count it as inflight given the flush sequence is suspended,
	 * and the original flush data request is invisible to driver, just
	 * like other pending requests because of quiesce
	 */
	if (blk_mq_request_started(rq) && !(blk_queue_quiesced(rq->q) &&
				blk_is_flush_data_rq(rq) &&
				blk_mq_request_completed(rq))) {
		bool *busy = priv;

		*busy = true;
		return false;
	}

	return true;
}

bool blk_mq_queue_inflight(struct request_queue *q)
{
	bool busy = false;

	blk_mq_queue_tag_busy_iter(q, blk_mq_rq_inflight, &busy);
	return busy;
}
EXPORT_SYMBOL_GPL(blk_mq_queue_inflight);

static void blk_mq_rq_timed_out(struct request *req)
{
	req->rq_flags |= RQF_TIMED_OUT;
// RQF_TIMED_OUT: NVMe 명령이 deadline 초과되었음 표시
	if (req->q->mq_ops->timeout) {
		enum blk_eh_timer_return ret;

		ret = req->q->mq_ops->timeout(req);
// mq_ops->timeout == nvme_timeout: 컨트롤러 reset/abort 시도
		if (ret == BLK_EH_DONE)
			return;
		WARN_ON_ONCE(ret != BLK_EH_RESET_TIMER);
	}

	blk_add_timer(req);
// timeout handler 가 BLK_EH_RESET_TIMER 이면 타이머 재설정
}

/*
 * struct blk_expired_data: timeout 검사 중 사용하는 상태 구조체.
 *   has_timedout_rq: timeout 이 발생한 CID(request) 존재 여부.
 *   next: 가장 가까운 다음 timeout deadline.
 *   timeout_start: 이번 timeout work 의 시작 시점.
 */
struct blk_expired_data {
	bool has_timedout_rq; /* timeout 된 request(CID) 존재 여부 */
	unsigned long next; /* 다음 timeout deadline */
	unsigned long timeout_start; /* timeout 검사 시작 시각 */
};

static bool blk_mq_req_expired(struct request *rq, struct blk_expired_data *expired)
{
	unsigned long deadline;

	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
// MQ_RQ_IN_FLIGHT 상태인 NVMe CID 만 만료 검사
		return false;
	if (rq->rq_flags & RQF_TIMED_OUT)
		return false;

	deadline = READ_ONCE(rq->deadline);
// READ_ONCE(rq->deadline): 타이머 갱신과의 race 방지
	if (time_after_eq(expired->timeout_start, deadline))
// timeout_start 가 deadline 을 넘었으면 NVMe 명령 만료
		return true;

	if (expired->next == 0)
		expired->next = deadline;
	else if (time_after(expired->next, deadline))
		expired->next = deadline;
	return false;
}

void blk_mq_put_rq_ref(struct request *rq)
{
	if (is_flush_rq(rq)) {
		if (rq->end_io(rq, 0, NULL) == RQ_END_IO_FREE)
			blk_mq_free_request(rq);
	} else if (req_ref_put_and_test(rq)) {
		__blk_mq_free_request(rq);
	}
}

static bool blk_mq_check_expired(struct request *rq, void *priv)
{
	struct blk_expired_data *expired = priv;

	/*
	 * blk_mq_queue_tag_busy_iter() has locked the request, so it cannot
	 * be reallocated underneath the timeout handler's processing, then
	 * the expire check is reliable. If the request is not expired, then
	 * it was completed and reallocated as a new request after returning
	 * from blk_mq_check_expired().
	 */
	if (blk_mq_req_expired(rq, expired)) {
// 만료된 CID 발견: timeout_work 가 상세 처리를 위해 재순회
		expired->has_timedout_rq = true;
		return false;
	}
	return true;
}

static bool blk_mq_handle_expired(struct request *rq, void *priv)
{
	struct blk_expired_data *expired = priv;

	if (blk_mq_req_expired(rq, expired))
		blk_mq_rq_timed_out(rq);
	return true;
}

/*
 * blk_mq_timeout_work: timeout 된 request 를 검사하고 처리.
 *   NVMe 관점: NVMe 명령이 CQ 완료 없이 deadline 을 넘기면,
 *   mq_ops->timeout (nvme_timeout) 을 호출하여 controller reset
 *   이나 abort 를 시도.
 */
static void blk_mq_timeout_work(struct work_struct *work)
{
	struct request_queue *q =
		container_of(work, struct request_queue, timeout_work);
	struct blk_expired_data expired = {
		.timeout_start = jiffies,
	};
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* A deadlock might occur if a request is stuck requiring a
	 * timeout at the same time a queue freeze is waiting
	 * completion, since the timeout code would not be able to
	 * acquire the queue reference here.
	 *
	 * That's why we don't use blk_queue_enter here; instead, we use
	 * percpu_ref_tryget directly, because we need to be able to
	 * obtain a reference even in the short window between the queue
	 * starting to freeze, by dropping the first reference in
	 * blk_freeze_queue_start, and the moment the last request is
	 * consumed, marked by the instant q_usage_counter reaches
	 * zero.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter))
// freeze 와 deadlock 방지를 위해 percpu_ref_tryget() 만 사용
		return;

	/* check if there is any timed-out request */
	blk_mq_queue_tag_busy_iter(q, blk_mq_check_expired, &expired);
// 모든 tag 를 순회하며 만료된 NVMe CID(request) 검출
	if (expired.has_timedout_rq) {
		/*
		 * Before walking tags, we must ensure any submit started
		 * before the current time has finished. Since the submit
		 * uses srcu or rcu, wait for a synchronization point to
		 * ensure all running submits have finished
		 */
		blk_mq_wait_quiesce_done(q->tag_set);
// submitter 들이 tags/hctx 접근을 마칠 때까지 SRCU/RCU 대기

		expired.next = 0;
		blk_mq_queue_tag_busy_iter(q, blk_mq_handle_expired, &expired);
// 실제 timeout 처리: nvme_timeout 콜백 호출
	}

	if (expired.next != 0) {
		mod_timer(&q->timeout, expired.next);
// 다음 만료 시각까지 timeout 타이머 재설정
	} else {
		/*
		 * Request timeouts are handled as a forward rolling timer. If
		 * we end up here it means that no requests are pending and
		 * also that no request has been pending for a while. Mark
		 * each hctx as idle.
		 */
		queue_for_each_hw_ctx(q, hctx, i) {
// pending NVMe CID 가 없으면 각 hctx 를 idle 로 표시
			/* the hctx may be unmapped, so check it here */
			if (blk_mq_hw_queue_mapped(hctx))
				blk_mq_tag_idle(hctx);
// blk_mq_tag_idle(): NVMe SQ 의 sbitmap wakeup 상태 idle 처리
		}
	}
	blk_queue_exit(q);
}

/*
 * struct flush_busy_ctx_data: busy software queue 의 request 를
 *   hctx dispatch list 로 이동할 때 사용.
 *   hctx: 대상 NVMe SQ 에 해당하는 hardware queue.
 *   list: 이동할 request list.
 */
struct flush_busy_ctx_data {
	struct blk_mq_hw_ctx *hctx;
	struct list_head *list;
};

static bool flush_busy_ctx(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	struct flush_busy_ctx_data *flush_data = data;
	struct blk_mq_hw_ctx *hctx = flush_data->hctx;
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];
	enum hctx_type type = hctx->type;

	spin_lock(&ctx->lock);
	list_splice_tail_init(&ctx->rq_lists[type], flush_data->list);
// ctx->rq_lists[type] 의 request 들을 hctx->dispatch 로 이동
	sbitmap_clear_bit(sb, bitnr);
// 이동 완료된 sw queue 는 ctx_map 비트 클리어
	spin_unlock(&ctx->lock);
	return true;
}

/*
 * Process software queues that have been marked busy, splicing them
 * to the for-dispatch
 */
void blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx, struct list_head *list)
{
	struct flush_busy_ctx_data data = {
		.hctx = hctx,
		.list = list,
	};

	sbitmap_for_each_set(&hctx->ctx_map, flush_busy_ctx, &data);
// hctx->ctx_map 의 set bit 순회: pending 인 sw queue 모두 flush
}

/*
 * struct dispatch_rq_data: software queue 로부터 request 를
 *   하나 꺼낼 때 사용하는 보조 구조체.
 *   hctx: 대상 NVMe SQ.
 *   rq: 꺼낸 NVMe request(CID 포함).
 */
struct dispatch_rq_data {
	struct blk_mq_hw_ctx *hctx;
	struct request *rq;
};

static bool dispatch_rq_from_ctx(struct sbitmap *sb, unsigned int bitnr,
		void *data)
{
	struct dispatch_rq_data *dispatch_data = data;
	struct blk_mq_hw_ctx *hctx = dispatch_data->hctx;
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];
	enum hctx_type type = hctx->type;

	spin_lock(&ctx->lock);
	if (!list_empty(&ctx->rq_lists[type])) {
		dispatch_data->rq = list_entry_rq(ctx->rq_lists[type].next);
// sw queue 맨 앞 request 를 꺼내 NVMe SQ dispatch 준비
		list_del_init(&dispatch_data->rq->queuelist);
		if (list_empty(&ctx->rq_lists[type]))
			sbitmap_clear_bit(sb, bitnr);
// sw queue 가 비면 ctx_map 에서 해당 CPU pending 제거
	}
	spin_unlock(&ctx->lock);

	return !dispatch_data->rq;
}

struct request *blk_mq_dequeue_from_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *start)
{
	unsigned off = start ? start->index_hw[hctx->type] : 0;
// start ctx 부터 순회: round-robin SQ feed (추정)
	struct dispatch_rq_data data = {
		.hctx = hctx,
		.rq   = NULL,
	};

	__sbitmap_for_each_set(&hctx->ctx_map, off,
			       dispatch_rq_from_ctx, &data);

	return data.rq;
}

bool __blk_mq_alloc_driver_tag(struct request *rq)
{
	struct sbitmap_queue *bt = &rq->mq_hctx->tags->bitmap_tags;
// 기본적으로 비예약 tag pool 에서 NVMe CID 할당
	unsigned int tag_offset = rq->mq_hctx->tags->nr_reserved_tags;
// reserved_tags 수만큼 offset: 예약 CID 와 일반 CID 구분
	int tag;

	blk_mq_tag_busy(rq->mq_hctx);

	if (blk_mq_tag_is_reserved(rq->mq_hctx->sched_tags, rq->internal_tag)) {
// scheduler reserved tag 면 breserved_tags 에서 CID 할당
		bt = &rq->mq_hctx->tags->breserved_tags;
		tag_offset = 0;
	} else {
		if (!hctx_may_queue(rq->mq_hctx, bt))
// hctx_may_queue(): NVMe SQ queue depth 예산 확인
			return false;
	}

	tag = __sbitmap_queue_get(bt);
// __sbitmap_queue_get(): NVMe SQ slot(CID) 1개 확보
	if (tag == BLK_MQ_NO_TAG)
		return false;

	rq->tag = tag + tag_offset;
// rq->tag = tag + tag_offset: 실제 NVMe CID 값 설정
	blk_mq_inc_active_requests(rq->mq_hctx);
	return true;
}

static int blk_mq_dispatch_wake(wait_queue_entry_t *wait, unsigned mode,
				int flags, void *key)
{
	struct blk_mq_hw_ctx *hctx;

	hctx = container_of(wait, struct blk_mq_hw_ctx, dispatch_wait);

	spin_lock(&hctx->dispatch_wait_lock);
	if (!list_empty(&wait->entry)) {
		struct sbitmap_queue *sbq;

		list_del_init(&wait->entry);
		sbq = &hctx->tags->bitmap_tags;
		atomic_dec(&sbq->ws_active);
// dispatch waitqueue 활성화 개수 감소
	}
	spin_unlock(&hctx->dispatch_wait_lock);

	blk_mq_run_hw_queue(hctx, true);
	return 1;
}

/*
 * Mark us waiting for a tag. For shared tags, this involves hooking us into
 * the tag wakeups. For non-shared tags, we can simply mark us needing a
 * restart. For both cases, take care to check the condition again after
 * marking us as waiting.
 */
/*
 * blk_mq_mark_tag_wait: tag(CID) 가 부족할 때 대기 큐에 등록.
 *   NVMe 관점: NVMe SQ 가 꽉 차면(CID 고갈), hctx 의 waitqueue 에
 *   등록하여 다른 I/O 가 CID 를 반납할 때 깨어난다.
 */
static bool blk_mq_mark_tag_wait(struct blk_mq_hw_ctx *hctx,
				 struct request *rq)
{
	struct sbitmap_queue *sbq;
	struct wait_queue_head *wq;
	wait_queue_entry_t *wait;
	bool ret;

	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) &&
	    !(blk_mq_is_shared_tags(hctx->flags))) {
		blk_mq_sched_mark_restart_hctx(hctx);
// tag 부족 시 RESTART 표시: CID 반납 시 큐 rerun 유도

		/*
		 * It's possible that a tag was freed in the window between the
		 * allocation failure and adding the hardware queue to the wait
		 * queue.
		 *
		 * Don't clear RESTART here, someone else could have set it.
		 * At most this will cost an extra queue run.
		 */
		return blk_mq_get_driver_tag(rq);
// 다른 경쟁자가 tag 를 반납했을 수 있어 즉시 재시도
	}

	wait = &hctx->dispatch_wait;
	if (!list_empty_careful(&wait->entry))
		return false;

	if (blk_mq_tag_is_reserved(rq->mq_hctx->sched_tags, rq->internal_tag))
		sbq = &hctx->tags->breserved_tags;
	else
		sbq = &hctx->tags->bitmap_tags;
	wq = &bt_wait_ptr(sbq, hctx)->wait;
// bt_wait_ptr(): 이 NVMe SQ tag pool 의 wait queue 선택

	spin_lock_irq(&wq->lock);
	spin_lock(&hctx->dispatch_wait_lock);
	if (!list_empty(&wait->entry)) {
		spin_unlock(&hctx->dispatch_wait_lock);
		spin_unlock_irq(&wq->lock);
		return false;
	}

	atomic_inc(&sbq->ws_active);
// waitqueue 활성화 카운트 증가
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	__add_wait_queue(wq, wait);

	/*
	 * Add one explicit barrier since blk_mq_get_driver_tag() may
	 * not imply barrier in case of failure.
	 *
	 * Order adding us to wait queue and allocating driver tag.
	 *
	 * The pair is the one implied in sbitmap_queue_wake_up() which
	 * orders clearing sbitmap tag bits and waitqueue_active() in
	 * __sbitmap_queue_wake_up(), since waitqueue_active() is lockless
	 *
	 * Otherwise, re-order of adding wait queue and getting driver tag
	 * may cause __sbitmap_queue_wake_up() to wake up nothing because
	 * the waitqueue_active() may not observe us in wait queue.
	 */
	smp_mb();
// smp_mb(): waitqueue 등록과 tag 재할당 사이 순서 보장

	/*
	 * It's possible that a tag was freed in the window between the
	 * allocation failure and adding the hardware queue to the wait
	 * queue.
	 */
	ret = blk_mq_get_driver_tag(rq);
// waitqueue 등록 직후 다시 NVMe CID 확보 시도
	if (!ret) {
		spin_unlock(&hctx->dispatch_wait_lock);
		spin_unlock_irq(&wq->lock);
		return false;
	}

	/*
	 * We got a tag, remove ourselves from the wait queue to ensure
	 * someone else gets the wakeup.
	 */
	list_del_init(&wait->entry);
	atomic_dec(&sbq->ws_active);
	spin_unlock(&hctx->dispatch_wait_lock);
	spin_unlock_irq(&wq->lock);

	return true;
}

#define BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT  8
#define BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR  4
/*
 * Update dispatch busy with the Exponential Weighted Moving Average(EWMA):
 * - EWMA is one simple way to compute running average value
 * - weight(7/8 and 1/8) is applied so that it can decrease exponentially
 * - take 4 as factor for avoiding to get too small(0) result, and this
 *   factor doesn't matter because EWMA decreases exponentially
 */
static void blk_mq_update_dispatch_busy(struct blk_mq_hw_ctx *hctx, bool busy)
{
	unsigned int ewma;

	ewma = hctx->dispatch_busy;
// dispatch_busy EWMA: NVMe SQ 자원 부족 빈도를 부드럽게 추적

	if (!ewma && !busy)
		return;

	ewma *= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT - 1;
	if (busy)
		ewma += 1 << BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR;
	ewma /= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT;

	hctx->dispatch_busy = ewma;
}

#define BLK_MQ_RESOURCE_DELAY	3		/* ms units */

static void blk_mq_handle_dev_resource(struct request *rq,
				       struct list_head *list)
{
	list_add(&rq->queuelist, list);
// NVMe controller 가 BLK_STS_RESOURCE 를 반환하면 dispatch list 로 재삽입
	__blk_mq_requeue_request(rq);
}

enum prep_dispatch {
	PREP_DISPATCH_OK,
	PREP_DISPATCH_NO_TAG,
	PREP_DISPATCH_NO_BUDGET,
};

static enum prep_dispatch blk_mq_prep_dispatch_rq(struct request *rq,
						  bool need_budget)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	int budget_token = -1;

	if (need_budget) {
		budget_token = blk_mq_get_dispatch_budget(rq->q);
// NVMe SQ budget(token) 획득: queue depth/stride 제한
		if (budget_token < 0) {
			blk_mq_put_driver_tag(rq);
// budget 실패 시 이미 획득한 driver tag(CID) 반납
			return PREP_DISPATCH_NO_BUDGET;
		}
		blk_mq_set_rq_budget_token(rq, budget_token);
	}

	if (!blk_mq_get_driver_tag(rq)) {
// NVMe CID(driver tag) 확보: SQ slot 점유
		/*
		 * The initial allocation attempt failed, so we need to
		 * rerun the hardware queue when a tag is freed. The
		 * waitqueue takes care of that. If the queue is run
		 * before we add this entry back on the dispatch list,
		 * we'll re-run it below.
		 */
		if (!blk_mq_mark_tag_wait(hctx, rq)) {
// CID 부족 시 dispatch wait queue 등록 후 재시도
			/*
			 * All budgets not got from this function will be put
			 * together during handling partial dispatch
			 */
			if (need_budget)
				blk_mq_put_dispatch_budget(rq->q, budget_token);
// tag 확보 실패 시 budget token 도 반납
			return PREP_DISPATCH_NO_TAG;
		}
	}

	return PREP_DISPATCH_OK;
}

/* release all allocated budgets before calling to blk_mq_dispatch_rq_list */
static void blk_mq_release_budgets(struct request_queue *q,
		struct list_head *list)
{
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {
		int budget_token = blk_mq_get_rq_budget_token(rq);

		if (budget_token >= 0)
			blk_mq_put_dispatch_budget(q, budget_token);
	}
}

/*
 * blk_mq_commit_rqs will notify driver using bd->last that there is no
 * more requests. (See comment in struct blk_mq_ops for commit_rqs for
 * details)
 * Attention, we should explicitly call this in unusual cases:
 *  1) did not queue everything initially scheduled to queue
 *  2) the last attempt to queue a request failed
 */
static void blk_mq_commit_rqs(struct blk_mq_hw_ctx *hctx, int queued,
			      bool from_schedule)
{
	if (hctx->queue->mq_ops->commit_rqs && queued) {
// driver 의 commit_rqs(): batch submit 후 NVMe doorbell 기록 유도
		trace_block_unplug(hctx->queue, queued, !from_schedule);
		hctx->queue->mq_ops->commit_rqs(hctx);
// q->mq_ops->commit_rqs == nvme_commit_rqs (추정)
	}
}

/*
 * Returns true if we did some work AND can potentially do more.
 */
/*
 * blk_mq_dispatch_rq_list: hctx 의 dispatch list 에 있는 request 를
 *   driver 의 queue_rq 콜백으로 전달.
 *   NVMe 관점: blk-mq 의 핵심 dispatch 경로.
 *   blk_mq_run_hw_queue -> blk_mq_sched_dispatch_requests ->
 *   blk_mq_dispatch_rq_list -> q->mq_ops->queue_rq(hctx, &bd)
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 로 이어진다.
 *   budget 과 tag(CID) 를 확보한 후에만 driver 로 전달.
 */
bool blk_mq_dispatch_rq_list(struct blk_mq_hw_ctx *hctx, struct list_head *list,
			     bool get_budget)
{
	enum prep_dispatch prep;
	struct request_queue *q = hctx->queue;
	struct request *rq;
	int queued;
	blk_status_t ret = BLK_STS_OK;
	bool needs_resource = false;

	if (list_empty(list))
// dispatch list 가 비었으면 할 일 없음
		return false;

	/*
	 * Now process all the entries, sending them to the driver.
	 */
	queued = 0;
	do {
		struct blk_mq_queue_data bd;

		rq = list_first_entry(list, struct request, queuelist);

// list 의 첫 request 를 꺼내 NVMe driver 로 전달
		WARN_ON_ONCE(hctx != rq->mq_hctx);
		prep = blk_mq_prep_dispatch_rq(rq, get_budget);
// budget(CID 할당 전 단계) 와 driver tag(CID) 확보
		if (prep != PREP_DISPATCH_OK)
			break;

		list_del_init(&rq->queuelist);
// driver 전달 성공 시 dispatch list 에서 제거

		bd.rq = rq;
// bd.rq: NVMe 명령으로 날아갈 request
		bd.last = list_empty(list);
// bd.last: 이 request 이후 list 에 남은 것이 없으면 true

		ret = q->mq_ops->queue_rq(hctx, &bd); /* nvme_queue_rq 호출: SQ 에 명령 기록 */
		switch (ret) {
		case BLK_STS_OK: /* NVMe 명령이 SQ 에 성공적으로 배치됨 */
// BLK_STS_OK: NVMe SQ 에 성공적으로 배치, doorbell 은 driver 에서
			queued++;
			break;
		case BLK_STS_RESOURCE: /* NVMe SQ/PRP/SGL 자원 부족, 재시도 예약 */
// BLK_STS_RESOURCE: NVMe SQ/PRP/SGL 용량 부족
			needs_resource = true;
			fallthrough;
		case BLK_STS_DEV_RESOURCE: /* NVMe 컨트롤러 내부 자원 부족 */
			blk_mq_handle_dev_resource(rq, list); /* 자원 부족 시 dispatch list 로 재삽입 */
			goto out;
		default:
			blk_mq_end_request(rq, ret);
// 그 외 오류: NVMe 명령 즉시 완료(상위로 error 전파)
		}
	} while (!list_empty(list));
out:
	/* If we didn't flush the entire list, we could have told the driver
	 * there was more coming, but that turned out to be a lie.
	 */
	if (!list_empty(list) || ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false); /* batch submit 마무리 (NVMe doorbell 유도, 추정) */

	/*
	 * Any items that need requeuing? Stuff them into hctx->dispatch,
	 * that is where we will continue on next queue run.
	 */
	if (!list_empty(list)) {
		bool needs_restart;
		/* For non-shared tags, the RESTART check will suffice */
		bool no_tag = prep == PREP_DISPATCH_NO_TAG &&
// shared tag 사용 시 tag 부족은 별도 wait/재시작 처리 필요
			((hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) ||
			blk_mq_is_shared_tags(hctx->flags));

		/*
		 * If the caller allocated budgets, free the budgets of the
		 * requests that have not yet been passed to the block driver.
		 */
		if (!get_budget)
// budget 을 caller 가 이미 할당했다면 남은 request 의 budget 반납
			blk_mq_release_budgets(q, list);

		spin_lock(&hctx->lock);
		list_splice_tail_init(list, &hctx->dispatch);
// 남은 request 들을 hctx->dispatch 에 연결하여 다음 run 때 재시도
		spin_unlock(&hctx->lock);

		/*
		 * Order adding requests to hctx->dispatch and checking
		 * SCHED_RESTART flag. The pair of this smp_mb() is the one
		 * in blk_mq_sched_restart(). Avoid restart code path to
		 * miss the new added requests to hctx->dispatch, meantime
		 * SCHED_RESTART is observed here.
		 */
		smp_mb();
// smp_mb(): hctx->dispatch 삽입과 SCHED_RESTART 플래그 체크 사이 순서 보장

		/*
		 * If SCHED_RESTART was set by the caller of this function and
		 * it is no longer set that means that it was cleared by another
		 * thread and hence that a queue rerun is needed.
		 *
		 * If 'no_tag' is set, that means that we failed getting
		 * a driver tag with an I/O scheduler attached. If our dispatch
		 * waitqueue is no longer active, ensure that we run the queue
		 * AFTER adding our entries back to the list.
		 *
		 * If no I/O scheduler has been configured it is possible that
		 * the hardware queue got stopped and restarted before requests
		 * were pushed back onto the dispatch list. Rerun the queue to
		 * avoid starvation. Notes:
		 * - blk_mq_run_hw_queue() checks whether or not a queue has
		 *   been stopped before rerunning a queue.
		 * - Some but not all block drivers stop a queue before
		 *   returning BLK_STS_RESOURCE. Two exceptions are scsi-mq
		 *   and dm-rq.
		 *
		 * If driver returns BLK_STS_RESOURCE and SCHED_RESTART
		 * bit is set, run queue after a delay to avoid IO stalls
		 * that could otherwise occur if the queue is idle.  We'll do
		 * similar if we couldn't get budget or couldn't lock a zone
		 * and SCHED_RESTART is set.
		 */
		needs_restart = blk_mq_sched_needs_restart(hctx);
// SCHED_RESTART: NVMe SQ 가 멈췄다가 다시 dispatch 해야 함
		if (prep == PREP_DISPATCH_NO_BUDGET)
// PREP_DISPATCH_NO_BUDGET: NVMe queue depth 예산 부족
			needs_resource = true;
		if (!needs_restart ||
		    (no_tag && list_empty_careful(&hctx->dispatch_wait.entry)))
			blk_mq_run_hw_queue(hctx, true);
// 즉시 blk_mq_run_hw_queue(): NVMe SQ rerun
		else if (needs_resource)
			blk_mq_delay_run_hw_queue(hctx, BLK_MQ_RESOURCE_DELAY);
// RESOURCE 발생 시 짧은 지연 후 rerun: NVMe controller 여유 생성

		blk_mq_update_dispatch_busy(hctx, true);
		return false;
	}

	blk_mq_update_dispatch_busy(hctx, false);
// dispatch list 소진 성공: NVMe SQ idle 로 간주 가능
	return true;
}

static inline int blk_mq_first_mapped_cpu(struct blk_mq_hw_ctx *hctx)
{
	int cpu = cpumask_first_and(hctx->cpumask, cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		cpu = cpumask_first(hctx->cpumask);
	return cpu;
}

/*
 * ->next_cpu is always calculated from hctx->cpumask, so simply use
 * it for speeding up the check
 */
static bool blk_mq_hctx_empty_cpumask(struct blk_mq_hw_ctx *hctx)
{
        return hctx->next_cpu >= nr_cpu_ids;
}

/*
 * It'd be great if the workqueue API had a way to pass
 * in a mask and had some smarts for more clever placement.
 * For now we just round-robin here, switching for every
 * BLK_MQ_CPU_WORK_BATCH queued items.
 */
static int blk_mq_hctx_next_cpu(struct blk_mq_hw_ctx *hctx)
{
	bool tried = false;
	int next_cpu = hctx->next_cpu;

	/* Switch to unbound if no allowable CPUs in this hctx */
	if (hctx->queue->nr_hw_queues == 1 || blk_mq_hctx_empty_cpumask(hctx))
// NVMe SQ 가 1개이거나 cpumask 가 비면 unbound kworker 사용
		return WORK_CPU_UNBOUND;

	if (--hctx->next_cpu_batch <= 0) {
select_cpu:
		next_cpu = cpumask_next_and(next_cpu, hctx->cpumask,
// cpumask_next_and(): 다음 NVMe SQ 제출 CPU round-robin 선택
				cpu_online_mask);
		if (next_cpu >= nr_cpu_ids)
			next_cpu = blk_mq_first_mapped_cpu(hctx);
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}

	/*
	 * Do unbound schedule if we can't find a online CPU for this hctx,
	 * and it should only happen in the path of handling CPU DEAD.
	 */
	if (!cpu_online(next_cpu)) {
		if (!tried) {
			tried = true;
			goto select_cpu;
		}

		/*
		 * Make sure to re-select CPU next time once after CPUs
		 * in hctx->cpumask become online again.
		 */
		hctx->next_cpu = next_cpu;
		hctx->next_cpu_batch = 1;
		return WORK_CPU_UNBOUND;
	}

	hctx->next_cpu = next_cpu;
	return next_cpu;
}

/**
 * blk_mq_delay_run_hw_queue - Run a hardware queue asynchronously.
 * @hctx: Pointer to the hardware queue to run.
 * @msecs: Milliseconds of delay to wait before running the queue.
 *
 * Run a hardware queue asynchronously with a delay of @msecs.
 */
/*
 * blk_mq_delay_run_hw_queue: hctx 를 지연 후 비동기 실행.
 *   NVMe 관점: kblockd workqueue 를 통해 SQ dispatch 를 예약.
 *   BLK_STS_RESOURCE 반환 후 재시도할 때 사용.
 */
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs)
{
	if (unlikely(blk_mq_hctx_stopped(hctx)))
// BLK_MQ_S_STOPPED 가 설정된 NVMe SQ 는 run 예약하지 않음
		return;
	kblockd_mod_delayed_work_on(blk_mq_hctx_next_cpu(hctx), &hctx->run_work,
// kblockd_mod_delayed_work_on(): 지정된 CPU 의 kblockd 에 SQ dispatch 예약
				    msecs_to_jiffies(msecs));
}
EXPORT_SYMBOL(blk_mq_delay_run_hw_queue);

static inline bool blk_mq_hw_queue_need_run(struct blk_mq_hw_ctx *hctx)
{
	bool need_run;

	/*
	 * When queue is quiesced, we may be switching io scheduler, or
	 * updating nr_hw_queues, or other things, and we can't run queue
	 * any more, even blk_mq_hctx_has_pending() can't be called safely.
	 *
	 * And queue will be rerun in blk_mq_unquiesce_queue() if it is
	 * quiesced.
	 */
	__blk_mq_run_dispatch_ops(hctx->queue, false,
		need_run = !blk_queue_quiesced(hctx->queue) &&
// quiesced 상태가 아니고 pending request 가 있을 때만 run
		blk_mq_hctx_has_pending(hctx));
	return need_run;
}

/**
 * blk_mq_run_hw_queue - Start to run a hardware queue.
 * @hctx: Pointer to the hardware queue to run.
 * @async: If we want to run the queue asynchronously.
 *
 * Check if the request queue is not in a quiesced state and if there are
 * pending requests to be sent. If this is true, run the queue to send requests
 * to hardware.
 */
/*
 * blk_mq_run_hw_queue: hctx 에 pending 한 request 가 있으면 실행.
 *   NVMe 관점: NVMe SQ 에 명령을 채우기 위한 dispatch 를
 *   시작. sync 이면 현재 컨텍스트에서, async 이면 kblockd 에서
 *   blk_mq_sched_dispatch_requests 를 호출.
 */
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
	bool need_run;

	/*
	 * We can't run the queue inline with interrupts disabled.
	 */
	WARN_ON_ONCE(!async && in_interrupt());
// 인터럽트 비활성화 상태에서는 sync run 불가

	might_sleep_if(!async && hctx->flags & BLK_MQ_F_BLOCKING);

	need_run = blk_mq_hw_queue_need_run(hctx);
// lockless 로 pending 상태 확인 후 필요시 재확인
	if (!need_run) {
		unsigned long flags;

		/*
		 * Synchronize with blk_mq_unquiesce_queue(), because we check
		 * if hw queue is quiesced locklessly above, we need the use
		 * ->queue_lock to make sure we see the up-to-date status to
		 * not miss rerunning the hw queue.
		 */
		spin_lock_irqsave(&hctx->queue->queue_lock, flags);
		need_run = blk_mq_hw_queue_need_run(hctx);
		spin_unlock_irqrestore(&hctx->queue->queue_lock, flags);

		if (!need_run)
			return;
	}

	if (async || !cpumask_test_cpu(raw_smp_processor_id(), hctx->cpumask)) {
// async 또는 현재 CPU 가 hctx cpumask 에 없으면 kblockd 로 위임
		blk_mq_delay_run_hw_queue(hctx, 0);
		return;
	}

	blk_mq_run_dispatch_ops(hctx->queue,
// sync run: 현재 컨텍스트에서 blk_mq_sched_dispatch_requests() 호출
				blk_mq_sched_dispatch_requests(hctx));
}
EXPORT_SYMBOL(blk_mq_run_hw_queue);

/*
 * Return prefered queue to dispatch from (if any) for non-mq aware IO
 * scheduler.
 */
static struct blk_mq_hw_ctx *blk_mq_get_sq_hctx(struct request_queue *q)
{
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
	/*
	 * If the IO scheduler does not respect hardware queues when
	 * dispatching, we just don't bother with multiple HW queues and
	 * dispatch from hctx for the current CPU since running multiple queues
	 * just causes lock contention inside the scheduler and pointless cache
	 * bouncing.
	 */
	struct blk_mq_hw_ctx *hctx = ctx->hctxs[HCTX_TYPE_DEFAULT];

	if (!blk_mq_hctx_stopped(hctx))
		return hctx;
	return NULL;
}

/**
 * blk_mq_run_hw_queues - Run all hardware queues in a request queue.
 * @q: Pointer to the request queue to run.
 * @async: If we want to run the queue asynchronously.
 */
/*
 * blk_mq_run_hw_queues: request_queue 의 모든 hctx 를 실행.
 *   NVMe 관점: NVMe 컨트롤러의 여러 SQ (예: IO queues, poll queues)
 *   에 걸쳐 pending 명령을 모두 dispatch.
 */
void blk_mq_run_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx, *sq_hctx;
	unsigned long i;

	sq_hctx = NULL;
	if (blk_queue_sq_sched(q))
// IO scheduler 가 single-queue 방식이면 기본 hctx 하나만 사용
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {
// request_queue 의 모든 NVMe SQ(hctx) 순회
		if (blk_mq_hctx_stopped(hctx))
// stopped 된 NVMe SQ 는 skip
			continue;
		/*
		 * Dispatch from this hctx either if there's no hctx preferred
		 * by IO scheduler or if it has requests that bypass the
		 * scheduler.
		 */
		if (!sq_hctx || sq_hctx == hctx ||
// scheduler 우회 dispatch list 가 있으면 해당 SQ 도 run
		    !list_empty_careful(&hctx->dispatch))
			blk_mq_run_hw_queue(hctx, async);
	}
}
EXPORT_SYMBOL(blk_mq_run_hw_queues);

/**
 * blk_mq_delay_run_hw_queues - Run all hardware queues asynchronously.
 * @q: Pointer to the request queue to run.
 * @msecs: Milliseconds of delay to wait before running the queues.
 */
void blk_mq_delay_run_hw_queues(struct request_queue *q, unsigned long msecs)
{
	struct blk_mq_hw_ctx *hctx, *sq_hctx;
	unsigned long i;

	sq_hctx = NULL;
	if (blk_queue_sq_sched(q))
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {
		if (blk_mq_hctx_stopped(hctx))
			continue;
		/*
		 * If there is already a run_work pending, leave the
		 * pending delay untouched. Otherwise, a hctx can stall
		 * if another hctx is re-delaying the other's work
		 * before the work executes.
		 */
		if (delayed_work_pending(&hctx->run_work))
// 이미 run_work 가 pending 이면 지연 시간을 덮어쓰지 않음
			continue;
		/*
		 * Dispatch from this hctx either if there's no hctx preferred
		 * by IO scheduler or if it has requests that bypass the
		 * scheduler.
		 */
		if (!sq_hctx || sq_hctx == hctx ||
		    !list_empty_careful(&hctx->dispatch))
			blk_mq_delay_run_hw_queue(hctx, msecs);
	}
}
EXPORT_SYMBOL(blk_mq_delay_run_hw_queues);

/*
 * This function is often used for pausing .queue_rq() by driver when
 * there isn't enough resource or some conditions aren't satisfied, and
 * BLK_STS_RESOURCE is usually returned.
 *
 * We do not guarantee that dispatch can be drained or blocked
 * after blk_mq_stop_hw_queue() returns. Please use
 * blk_mq_quiesce_queue() for that requirement.
 */
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	cancel_delayed_work(&hctx->run_work);
// cancel_delayed_work: 예약된 NVMe SQ dispatch 취소

	set_bit(BLK_MQ_S_STOPPED, &hctx->state);
// BLK_MQ_S_STOPPED 설정: NVMe SQ dispatch 일시 정지
}
EXPORT_SYMBOL(blk_mq_stop_hw_queue);

/*
 * This function is often used for pausing .queue_rq() by driver when
 * there isn't enough resource or some conditions aren't satisfied, and
 * BLK_STS_RESOURCE is usually returned.
 *
 * We do not guarantee that dispatch can be drained or blocked
 * after blk_mq_stop_hw_queues() returns. Please use
 * blk_mq_quiesce_queue() for that requirement.
 */
void blk_mq_stop_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_stop_hw_queue(hctx);
}
EXPORT_SYMBOL(blk_mq_stop_hw_queues);

void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	clear_bit(BLK_MQ_S_STOPPED, &hctx->state);
// BLK_MQ_S_STOPPED 해제 후 NVMe SQ dispatch 재개

	blk_mq_run_hw_queue(hctx, hctx->flags & BLK_MQ_F_BLOCKING);
}
EXPORT_SYMBOL(blk_mq_start_hw_queue);

void blk_mq_start_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_start_hw_queue(hctx);
}
EXPORT_SYMBOL(blk_mq_start_hw_queues);

void blk_mq_start_stopped_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
	if (!blk_mq_hctx_stopped(hctx))
// 이미 stopped 가 아니면 아무것도 안 함
		return;

	clear_bit(BLK_MQ_S_STOPPED, &hctx->state);
// STOPPED 플래그 클리어
	/*
	 * Pairs with the smp_mb() in blk_mq_hctx_stopped() to order the
	 * clearing of BLK_MQ_S_STOPPED above and the checking of dispatch
	 * list in the subsequent routine.
	 */
	smp_mb__after_atomic();
// smp_mb__after_atomic(): STOPPED 클리어와 dispatch list 검사 사이 순서 보장
	blk_mq_run_hw_queue(hctx, async);
}
EXPORT_SYMBOL_GPL(blk_mq_start_stopped_hw_queue);

void blk_mq_start_stopped_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_start_stopped_hw_queue(hctx, async ||
					(hctx->flags & BLK_MQ_F_BLOCKING));
}
EXPORT_SYMBOL(blk_mq_start_stopped_hw_queues);

static void blk_mq_run_work_fn(struct work_struct *work)
{
	struct blk_mq_hw_ctx *hctx =
		container_of(work, struct blk_mq_hw_ctx, run_work.work);

	blk_mq_run_dispatch_ops(hctx->queue,
				blk_mq_sched_dispatch_requests(hctx));
}

/**
 * blk_mq_request_bypass_insert - Insert a request at dispatch list.
 * @rq: Pointer to request to be inserted.
 * @flags: BLK_MQ_INSERT_*
 *
 * Should only be used carefully, when the caller knows we want to
 * bypass a potential IO scheduler on the target device.
 */
static void blk_mq_request_bypass_insert(struct request *rq, blk_insert_t flags)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	spin_lock(&hctx->lock);
	if (flags & BLK_MQ_INSERT_AT_HEAD)
		list_add(&rq->queuelist, &hctx->dispatch);
	else
		list_add_tail(&rq->queuelist, &hctx->dispatch);
	spin_unlock(&hctx->lock);
}

static void blk_mq_insert_requests(struct blk_mq_hw_ctx *hctx,
		struct blk_mq_ctx *ctx, struct list_head *list,
		bool run_queue_async)
{
	struct request *rq;
	enum hctx_type type = hctx->type;

	/*
	 * Try to issue requests directly if the hw queue isn't busy to save an
	 * extra enqueue & dequeue to the sw queue.
	 */
	if (!hctx->dispatch_busy && !run_queue_async) {
// hctx 가 바쁘지 않고 sync 경로면 NVMe SQ 로 즉시 발행 시도
		blk_mq_run_dispatch_ops(hctx->queue,
			blk_mq_try_issue_list_directly(hctx, list));
		if (list_empty(list))
			goto out;
	}

	/*
	 * preemption doesn't flush plug list, so it's possible ctx->cpu is
	 * offline now
	 */
	list_for_each_entry(rq, list, queuelist) {
// plug list 의 request 들이 모두 동일 ctx 에 속하는지 검증
		BUG_ON(rq->mq_ctx != ctx);
		trace_block_rq_insert(rq);
		if (rq->cmd_flags & REQ_NOWAIT)
// REQ_NOWAIT 포함 시 async 로 run_hw_queue 호출
			run_queue_async = true;
	}

	spin_lock(&ctx->lock);
	list_splice_tail_init(list, &ctx->rq_lists[type]);
// sw queue(ctx->rq_lists[type]) 로 request 연결
	blk_mq_hctx_mark_pending(hctx, ctx);
// hctx pending 비트 설정 -> run_hw_queue 시 NVMe SQ 로 이동
	spin_unlock(&ctx->lock);
out:
	blk_mq_run_hw_queue(hctx, run_queue_async);
}

/*
 * blk_mq_insert_request: request 를 hctx 의 dispatch list 나 scheduler
 *   큐에 삽입.
 *   NVMe 관점: SQ 바로 dispatch 가 불가능할 때(예: scheduler 사용,
 *   resource 부족) hctx->dispatch 에 임시 저장.
 */
static void blk_mq_insert_request(struct request *rq, blk_insert_t flags)
{
	struct request_queue *q = rq->q;
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	if (blk_rq_is_passthrough(rq)) {
// passthrough(예: NVMe admin) 는 scheduler 를 무조건 우회
		/*
		 * Passthrough request have to be added to hctx->dispatch
		 * directly.  The device may be in a situation where it can't
		 * handle FS request, and always returns BLK_STS_RESOURCE for
		 * them, which gets them added to hctx->dispatch.
		 *
		 * If a passthrough request is required to unblock the queues,
		 * and it is added to the scheduler queue, there is no chance to
		 * dispatch it given we prioritize requests in hctx->dispatch.
		 */
		blk_mq_request_bypass_insert(rq, flags);
	} else if (req_op(rq) == REQ_OP_FLUSH) {
// flush request 는 dispatch queue 맨 앞에 배치(우선 순위)
		/*
		 * Firstly normal IO request is inserted to scheduler queue or
		 * sw queue, meantime we add flush request to dispatch queue(
		 * hctx->dispatch) directly and there is at most one in-flight
		 * flush request for each hw queue, so it doesn't matter to add
		 * flush request to tail or front of the dispatch queue.
		 *
		 * Secondly in case of NCQ, flush request belongs to non-NCQ
		 * command, and queueing it will fail when there is any
		 * in-flight normal IO request(NCQ command). When adding flush
		 * rq to the front of hctx->dispatch, it is easier to introduce
		 * extra time to flush rq's latency because of S_SCHED_RESTART
		 * compared with adding to the tail of dispatch queue, then
		 * chance of flush merge is increased, and less flush requests
		 * will be issued to controller. It is observed that ~10% time
		 * is saved in blktests block/004 on disk attached to AHCI/NCQ
		 * drive when adding flush rq to the front of hctx->dispatch.
		 *
		 * Simply queue flush rq to the front of hctx->dispatch so that
		 * intensive flush workloads can benefit in case of NCQ HW.
		 */
		blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD);
	} else if (q->elevator) {
		LIST_HEAD(list);

		WARN_ON_ONCE(rq->tag != BLK_MQ_NO_TAG);
// scheduler 사용 시 rq->tag 는 아직 할당되지 않음(sched tag 사용)

		list_add(&rq->queuelist, &list);
		q->elevator->type->ops.insert_requests(hctx, &list, flags);
// scheduler 의 insert_requests(): NVMe IO reordering/merge 시작
	} else {
		trace_block_rq_insert(rq);

		spin_lock(&ctx->lock);
		if (flags & BLK_MQ_INSERT_AT_HEAD)
// elevator 미사용 시 sw queue 맨 앞에 삽입
			list_add(&rq->queuelist, &ctx->rq_lists[hctx->type]);
		else
			list_add_tail(&rq->queuelist,
				      &ctx->rq_lists[hctx->type]);
		blk_mq_hctx_mark_pending(hctx, ctx);
		spin_unlock(&ctx->lock);
	}
}

/*
 * blk_mq_bio_to_request: bio 의 정보를 request 에 복사.
 *   NVMe 관점: bio 의 sector/length 를 NVMe LBA/length 로 변환하기
 *   위한 전 단계. nr_phys_segments 는 PRP/SGL entry 수 계산의
 *   기초가 된다.
 */
static void blk_mq_bio_to_request(struct request *rq, struct bio *bio,
		unsigned int nr_segs)
{
	int err;

	if (bio->bi_opf & REQ_RAHEAD)
// REQ_RAHEAD 는 NVMe read-ahead 로 failfast 마스크 설정
		rq->cmd_flags |= REQ_FAILFAST_MASK;

	rq->bio = rq->biotail = bio;
	rq->__sector = bio->bi_iter.bi_sector;
// bio sector -> NVMe LBA 시작 주소
	rq->__data_len = bio->bi_iter.bi_size;
// bio size -> NVMe transfer length
	rq->phys_gap_bit = bio->bi_bvec_gap_bit;

	rq->nr_phys_segments = nr_segs;
// nr_phys_segments: NVMe PRP/SGL 리스트 크기 결정
	if (bio_integrity(bio))
		rq->nr_integrity_segments = blk_rq_count_integrity_sg(rq->q,
								      bio);
// integrity segments: NVMe PI 메타데이터 scatterlist 계산

	/* This can't fail, since GFP_NOIO includes __GFP_DIRECT_RECLAIM. */
	err = blk_crypto_rq_bio_prep(rq, bio, GFP_NOIO);
// blk_crypto_rq_bio_prep(): NVMe encryption keyslot 및 DUN 설정
	WARN_ON_ONCE(err);

	blk_account_io_start(rq);
// IO account 시작: NVMe namespace 통계 및 latency 측정
}

static blk_status_t __blk_mq_issue_directly(struct blk_mq_hw_ctx *hctx,
					    struct request *rq, bool last)
{
	struct request_queue *q = rq->q;
	struct blk_mq_queue_data bd = {
		.rq = rq,
		.last = last,
	};
	blk_status_t ret;

	/*
	 * For OK queue, we are done. For error, caller may kill it.
	 * Any other error (busy), just add it to our list as we
	 * previously would have done.
	 */
	ret = q->mq_ops->queue_rq(hctx, &bd); /* nvme_queue_rq 호출: SQ 에 명령 기록 */
	switch (ret) {
	case BLK_STS_OK: /* NVMe 명령이 SQ 에 성공적으로 배치됨 */
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	case BLK_STS_RESOURCE: /* NVMe SQ/PRP/SGL 자원 부족, 재시도 예약 */
	case BLK_STS_DEV_RESOURCE: /* NVMe 컨트롤러 내부 자원 부족 */
		blk_mq_update_dispatch_busy(hctx, true);
		__blk_mq_requeue_request(rq);
// request 를 재삽입 후 다음 queue run 때 재시도
		break;
	default:
		blk_mq_update_dispatch_busy(hctx, false);
// BLK_STS_IOERR 등 기타 오류: NVMe 명령 즉시 종료
		break;
	}

	return ret;
}

/*
 * blk_mq_get_budget_and_tag: dispatch budget 과 driver tag(CID) 를
 *   동시 확보.
 *   NVMe 관점: NVMe SQ slot(CID) 을 획득. budget 부족이나 tag
 *   부족 시 false 를 반환하고 큐에 재삽입.
 */
static bool blk_mq_get_budget_and_tag(struct request *rq)
{
	int budget_token;

	budget_token = blk_mq_get_dispatch_budget(rq->q);
// dispatch budget 획득: NVMe queue depth 제한 내에서만 진행
	if (budget_token < 0)
		return false;
	blk_mq_set_rq_budget_token(rq, budget_token);
	if (!blk_mq_get_driver_tag(rq)) {
// driver tag(CID) 획득: 실제 NVMe SQ slot 점유
		blk_mq_put_dispatch_budget(rq->q, budget_token);
		return false;
	}
	return true;
}

/**
 * blk_mq_try_issue_directly - Try to send a request directly to device driver.
 * @hctx: Pointer of the associated hardware queue.
 * @rq: Pointer to request to be sent.
 *
 * If the device has enough resources to accept a new request now, send the
 * request directly to device driver. Else, insert at hctx->dispatch queue, so
 * we can try send it another time in the future. Requests inserted at this
 * queue have higher priority.
 */
/*
 * blk_mq_try_issue_directly: plug 없이 request 를 즉시 driver 로 전달.
 *   NVMe 관점: bio 제출 경로에서 scheduler 가 없고 자원이 충분하면
 *   q->mq_ops->queue_rq -> nvme_queue_rq 로 바로 이어진다.
 */
static void blk_mq_try_issue_directly(struct blk_mq_hw_ctx *hctx,
		struct request *rq)
{
	blk_status_t ret;

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {
// SQ 가 stopped/quiesced 이면 request 를 대기열에 넣고 예약
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);
		return;
	}

	if ((rq->rq_flags & RQF_USE_SCHED) || !blk_mq_get_budget_and_tag(rq)) {
// scheduler 사용 또는 budget/tag 실패 시 sw queue/scheduler 로 위임
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, rq->cmd_flags & REQ_NOWAIT);
		return;
	}

	ret = __blk_mq_issue_directly(hctx, rq, true);
// 자원 충분하면 즉시 nvme_queue_rq 로 NVMe SQ 제출
	switch (ret) {
	case BLK_STS_OK: /* NVMe 명령이 SQ 에 성공적으로 배치됨 */
		break;
	case BLK_STS_RESOURCE: /* NVMe SQ/PRP/SGL 자원 부족, 재시도 예약 */
	case BLK_STS_DEV_RESOURCE: /* NVMe 컨트롤러 내부 자원 부족 */
		blk_mq_request_bypass_insert(rq, 0);
// RESOURCE/DEV_RESOURCE 시 hctx->dispatch 로 재삽입 후 rerun
		blk_mq_run_hw_queue(hctx, false);
		break;
	default:
// 기타 오류는 NVMe 명령 완료(error 전파)
		blk_mq_end_request(rq, ret);
		break;
	}
}

static blk_status_t blk_mq_request_issue_directly(struct request *rq, bool last)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {
// passthrough 요청도 stopped/quiesced 상태면 대기
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);
		return BLK_STS_OK;
	}

	if (!blk_mq_get_budget_and_tag(rq))
// budget + CID 확보 실패 시 BLK_STS_RESOURCE 반환
		return BLK_STS_RESOURCE;
	return __blk_mq_issue_directly(hctx, rq, last);
}

static void blk_mq_issue_direct(struct rq_list *rqs)
{
	struct blk_mq_hw_ctx *hctx = NULL;
	struct request *rq;
	int queued = 0;
	blk_status_t ret = BLK_STS_OK;

	while ((rq = rq_list_pop(rqs))) {
		bool last = rq_list_empty(rqs);

		if (hctx != rq->mq_hctx) {
// hctx(SQ) 가 바뀌면 이전 SQ 의 batch commit_rqs 호출
			if (hctx) {
				blk_mq_commit_rqs(hctx, queued, false); /* batch submit 마무리 (NVMe doorbell 유도, 추정) */
				queued = 0;
			}
			hctx = rq->mq_hctx;
		}

		ret = blk_mq_request_issue_directly(rq, last);
		switch (ret) {
		case BLK_STS_OK: /* NVMe 명령이 SQ 에 성공적으로 배치됨 */
			queued++;
			break;
		case BLK_STS_RESOURCE: /* NVMe SQ/PRP/SGL 자원 부족, 재시도 예약 */
		case BLK_STS_DEV_RESOURCE: /* NVMe 컨트롤러 내부 자원 부족 */
			blk_mq_request_bypass_insert(rq, 0);
// RESOURCE 시 현재 request 를 bypass dispatch list 에 넣고 rerun
			blk_mq_run_hw_queue(hctx, false);
			goto out;
		default:
			blk_mq_end_request(rq, ret);
			break;
		}
	}

out:
	if (ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false); /* batch submit 마무리 (NVMe doorbell 유도, 추정) */
}

static void __blk_mq_flush_list(struct request_queue *q, struct rq_list *rqs)
{
	if (blk_queue_quiesced(q))
// quiesced 상태면 NVMe SQ 로 내리지 않음
		return;
	q->mq_ops->queue_rqs(rqs);
// queue_rqs(): driver 가 여러 request 를 한 번에 처리(추정)
}

static unsigned blk_mq_extract_queue_requests(struct rq_list *rqs,
					      struct rq_list *queue_rqs)
{
	struct request *rq = rq_list_pop(rqs);
	struct request_queue *this_q = rq->q;
	struct request **prev = &rqs->head;
	struct rq_list matched_rqs = {};
	struct request *last = NULL;
	unsigned depth = 1;

	rq_list_add_tail(&matched_rqs, rq);
	while ((rq = *prev)) {
		if (rq->q == this_q) {
// 동일 request_queue(NVMe namespace) request 만 묶음
			/* move rq from rqs to matched_rqs */
			*prev = rq->rq_next;
			rq_list_add_tail(&matched_rqs, rq);
			depth++;
		} else {
			/* leave rq in rqs */
			prev = &rq->rq_next;
			last = rq;
		}
	}

	rqs->tail = last;
	*queue_rqs = matched_rqs;
	return depth;
}

static void blk_mq_dispatch_queue_requests(struct rq_list *rqs, unsigned depth)
{
	struct request_queue *q = rq_list_peek(rqs)->q;

	trace_block_unplug(q, depth, true);
// trace: plug list 의 batch unplug 기록

	/*
	 * Peek first request and see if we have a ->queue_rqs() hook.
	 * If we do, we can dispatch the whole list in one go.
	 * We already know at this point that all requests belong to the
	 * same queue, caller must ensure that's the case.
	 */
	if (q->mq_ops->queue_rqs) {
// driver 가 queue_rqs() 를 제공하면 batch 로 NVMe SQ 제출
		blk_mq_run_dispatch_ops(q, __blk_mq_flush_list(q, rqs));
		if (rq_list_empty(rqs))
			return;
	}

	blk_mq_run_dispatch_ops(q, blk_mq_issue_direct(rqs));
// 그렇지 않으면 개별 issue_direct 로 NVMe SQ 제출
}

static void blk_mq_dispatch_list(struct rq_list *rqs, bool from_sched)
{
	struct blk_mq_hw_ctx *this_hctx = NULL;
	struct blk_mq_ctx *this_ctx = NULL;
	struct rq_list requeue_list = {};
	unsigned int depth = 0;
	bool is_passthrough = false;
	LIST_HEAD(list);

	do {
		struct request *rq = rq_list_pop(rqs);

		if (!this_hctx) {
// 같은 hctx/ctx/passthrough 여부인 request 들끼리 묶음
			this_hctx = rq->mq_hctx;
			this_ctx = rq->mq_ctx;
			is_passthrough = blk_rq_is_passthrough(rq);
		} else if (this_hctx != rq->mq_hctx || this_ctx != rq->mq_ctx ||
// hctx/ctx/passthrough 속성이 다륾면 requeue_list 로 분리
			   is_passthrough != blk_rq_is_passthrough(rq)) {
			rq_list_add_tail(&requeue_list, rq);
			continue;
		}
		list_add_tail(&rq->queuelist, &list);
		depth++;
	} while (!rq_list_empty(rqs));

	*rqs = requeue_list;
	trace_block_unplug(this_hctx->queue, depth, !from_sched);

	percpu_ref_get(&this_hctx->queue->q_usage_counter);
// queue 사용 카운트 획득: dispatch 동안 queue 생존 보장
	/* passthrough requests should never be issued to the I/O scheduler */
	if (is_passthrough) {
// passthrough 는 항상 hctx->dispatch 로 직접 삽입
		spin_lock(&this_hctx->lock);
		list_splice_tail_init(&list, &this_hctx->dispatch);
		spin_unlock(&this_hctx->lock);
		blk_mq_run_hw_queue(this_hctx, from_sched);
	} else if (this_hctx->queue->elevator) {
// elevator 사용 시 scheduler 큐로 insert_requests
		this_hctx->queue->elevator->type->ops.insert_requests(this_hctx,
				&list, 0);
		blk_mq_run_hw_queue(this_hctx, from_sched);
	} else {
		blk_mq_insert_requests(this_hctx, this_ctx, &list, from_sched);
// elevator 미사용 시 sw queue 로 insert_requests
	}
	percpu_ref_put(&this_hctx->queue->q_usage_counter);
}

static void blk_mq_dispatch_multiple_queue_requests(struct rq_list *rqs)
{
	do {
		struct rq_list queue_rqs;
		unsigned depth;

		depth = blk_mq_extract_queue_requests(rqs, &queue_rqs);
// 같은 queue(NVMe namespace) 의 request 만 추출
		blk_mq_dispatch_queue_requests(&queue_rqs, depth);
		while (!rq_list_empty(&queue_rqs))
			blk_mq_dispatch_list(&queue_rqs, false);
	} while (!rq_list_empty(rqs));
}

void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule)
{
	unsigned int depth;

	/*
	 * We may have been called recursively midway through handling
	 * plug->mq_list via a schedule() in the driver's queue_rq() callback.
	 * To avoid mq_list changing under our feet, clear rq_count early and
	 * bail out specifically if rq_count is 0 rather than checking
	 * whether the mq_list is empty.
	 */
	if (plug->rq_count == 0)
// plug->rq_count == 0 이면 이미 flush 된 것으로 간주
		return;
	depth = plug->rq_count;
	plug->rq_count = 0;

	if (!plug->has_elevator && !from_schedule) {
		if (plug->multiple_queues) {
			blk_mq_dispatch_multiple_queue_requests(&plug->mq_list);
// 여러 NVMe SQ 에 걸친 request 들은 큐별로 분리 발행
			return;
		}

		blk_mq_dispatch_queue_requests(&plug->mq_list, depth);
// 단일 queue 면 batch unplug 로 NVMe SQ 발행
		if (rq_list_empty(&plug->mq_list))
			return;
	}

	do {
		blk_mq_dispatch_list(&plug->mq_list, from_schedule);
	} while (!rq_list_empty(&plug->mq_list));
}

static void blk_mq_try_issue_list_directly(struct blk_mq_hw_ctx *hctx,
		struct list_head *list)
{
	int queued = 0;
	blk_status_t ret = BLK_STS_OK;

	while (!list_empty(list)) {
		struct request *rq = list_first_entry(list, struct request,
				queuelist);

		list_del_init(&rq->queuelist);
		ret = blk_mq_request_issue_directly(rq, list_empty(list));
// list 의 request 를 하나씩 NVMe driver 로 즉시 발행
		switch (ret) {
		case BLK_STS_OK: /* NVMe 명령이 SQ 에 성공적으로 배치됨 */
			queued++;
			break;
		case BLK_STS_RESOURCE: /* NVMe SQ/PRP/SGL 자원 부족, 재시도 예약 */
		case BLK_STS_DEV_RESOURCE: /* NVMe 컨트롤러 내부 자원 부족 */
			blk_mq_request_bypass_insert(rq, 0);
// RESOURCE 시 bypass dispatch list 로 재삽입
			if (list_empty(list))
				blk_mq_run_hw_queue(hctx, false);
			goto out;
		default:
			blk_mq_end_request(rq, ret);
			break;
		}
	}

out:
	if (ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false); /* batch submit 마무리 (NVMe doorbell 유도, 추정) */
}

static bool blk_mq_attempt_bio_merge(struct request_queue *q,
				     struct bio *bio, unsigned int nr_segs)
{
	if (!blk_queue_nomerges(q) && bio_mergeable(bio)) {
// queue 가 merge 금지 상태가 아니고 bio 가 merge 가능할 때
		if (blk_attempt_plug_merge(q, bio, nr_segs))
// plug merge: 동일 thread 의 NVMe IO 병합 시도
			return true;
		if (blk_mq_sched_bio_merge(q, bio, nr_segs))
// scheduler merge: elevator 가 NVMe IO reorder/merge 시도
			return true;
	}
	return false;
}

/*
 * blk_mq_get_new_requests: bio 를 위한 request 를 할당.
 *   NVMe 관점: NVMe 명령(slot)을 확보하기 위해 context, hctx, tag(CID)
 *   를 순차적으로 할당.
 */
static struct request *blk_mq_get_new_requests(struct request_queue *q,
					       struct blk_plug *plug,
					       struct bio *bio)
{
	struct blk_mq_alloc_data data = {
		.q		= q,
		.flags		= 0,
		.shallow_depth	= 0,
		.cmd_flags	= bio->bi_opf,
		.rq_flags	= 0,
		.nr_tags	= 1,
		.cached_rqs	= NULL,
		.ctx		= NULL,
		.hctx		= NULL
	};
	struct request *rq;

	rq_qos_throttle(q, bio);
// rq_qos_throttle(): NVMe IO QoS throttle (예: iocost)

	if (plug) {
// plug 있을 때는 batch CID 할당을 시도
		data.nr_tags = plug->nr_ios;
		plug->nr_ios = 1;
		data.cached_rqs = &plug->cached_rqs;
	}

	rq = __blk_mq_alloc_requests(&data);
// NVMe CID(tag) 와 hctx(SQ) 할당
	if (unlikely(!rq))
		rq_qos_cleanup(q, bio);
// 할당 실패 시 QoS cleanup
	return rq;
}

/*
 * Check if there is a suitable cached request and return it.
 */
static struct request *blk_mq_peek_cached_request(struct blk_plug *plug,
		struct request_queue *q, blk_opf_t opf)
{
	enum hctx_type type = blk_mq_get_hctx_type(opf);
	struct request *rq;

	if (!plug)
		return NULL;
	rq = rq_list_peek(&plug->cached_rqs);
	if (!rq || rq->q != q)
		return NULL;
	if (type != rq->mq_hctx->type &&
	    (type != HCTX_TYPE_READ || rq->mq_hctx->type != HCTX_TYPE_DEFAULT))
		return NULL;
	if (op_is_flush(rq->cmd_flags) != op_is_flush(opf))
		return NULL;
	return rq;
}

static void blk_mq_use_cached_rq(struct request *rq, struct blk_plug *plug,
		struct bio *bio)
{
	if (rq_list_pop(&plug->cached_rqs) != rq)
		WARN_ON_ONCE(1);

	/*
	 * If any qos ->throttle() end up blocking, we will have flushed the
	 * plug and hence killed the cached_rq list as well. Pop this entry
	 * before we throttle.
	 */
	rq_qos_throttle(rq->q, bio);

	blk_mq_rq_time_init(rq, blk_time_get_ns());
	rq->cmd_flags = bio->bi_opf;
	INIT_LIST_HEAD(&rq->queuelist);
}

static bool bio_unaligned(const struct bio *bio, struct request_queue *q)
{
	unsigned int bs_mask = queue_logical_block_size(q) - 1;

	/* .bi_sector of any zero sized bio need to be initialized */
	if ((bio->bi_iter.bi_size & bs_mask) ||
	    ((bio->bi_iter.bi_sector << SECTOR_SHIFT) & bs_mask))
		return true;
	return false;
}

/**
 * blk_mq_submit_bio - Create and send a request to block device.
 * @bio: Bio pointer.
 *
 * Builds up a request structure from @q and @bio and send to the device. The
 * request may not be queued directly to hardware if:
 * * This request can be merged with another one
 * * We want to place request at plug queue for possible future merging
 * * There is an IO scheduler active at this queue
 *
 * It will not queue the request if there is an error with the bio, or at the
 * request creation.
 */
/*
 * blk_mq_submit_bio: 파일시스템/페이지캐시로부터 받은 bio 의 상위
 *   진입점.
 *   NVMe 관점: submit_bio -> blk_mq_submit_bio -> blk_mq_get_new_requests
 *   -> __blk_mq_alloc_requests(tag/CID 할당) -> blk_mq_bio_to_request
 *   -> blk_mq_try_issue_directly / blk_mq_insert_request ->
 *   blk_mq_run_hw_queue -> blk_mq_dispatch_rq_list ->
 *   q->mq_ops->queue_rq -> nvme_queue_rq -> nvme_submit_cmd(doorbell).
 *   merge, split, plug, scheduler 처리를 모두 수행.
 */
void blk_mq_submit_bio(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	struct blk_plug *plug = current->plug;
	const int is_sync = op_is_sync(bio->bi_opf);
	unsigned int integrity_action;
	struct blk_mq_hw_ctx *hctx;
	unsigned int nr_segs;
	struct request *rq;
	blk_status_t ret;

	/*
	 * If the plug has a cached request for this queue, try to use it.
	 */
	rq = blk_mq_peek_cached_request(plug, q, bio->bi_opf);

	/*
	 * A BIO that was released from a zone write plug has already been
	 * through the preparation in this function, already holds a reference
	 * on the queue usage counter, and is the only write BIO in-flight for
	 * the target zone. Go straight to preparing a request for it.
	 */
	if (bio_zone_write_plugging(bio)) {
// zone write plug bio 는 이미 준비된 상태 -> 바로 request 생성
		nr_segs = bio->__bi_nr_segments;
		if (rq)
			blk_queue_exit(q);
		goto new_request;
	}

	/*
	 * The cached request already holds a q_usage_counter reference and we
	 * don't have to acquire a new one if we use it.
	 */
	if (!rq) {
		if (unlikely(bio_queue_enter(bio)))
// queue 사용 카운드 획득 실패 시 bio error
			return;
	}

	/*
	 * Device reconfiguration may change logical block size or reduce the
	 * number of poll queues, so the checks for alignment and poll support
	 * have to be done with queue usage counter held.
	 */
	if (unlikely(bio_unaligned(bio, q))) {
// bio_unaligned: NVMe LBA/length 정렬 위반 시 즉시 오류
		bio_io_error(bio);
		goto queue_exit;
	}

	if ((bio->bi_opf & REQ_POLLED) && !blk_mq_can_poll(q)) {
// REQ_POLLED 설정 시 poll queue 지원 여부 확인
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		goto queue_exit;
	}

	bio = __bio_split_to_limits(bio, &q->limits, &nr_segs);
// __bio_split_to_limits(): NVMe max sectors/segments 제한에 맞게 분할
	if (!bio)
		goto queue_exit;

	integrity_action = bio_integrity_action(bio);
// bio_integrity_prep(): NVMe PI 메타데이터 연결
	if (integrity_action)
		bio_integrity_prep(bio, integrity_action);

	blk_mq_bio_issue_init(q, bio);
	if (blk_mq_attempt_bio_merge(q, bio, nr_segs))
// bio merge 성공 시 NVMe SQ 제출 없이 상위로 즉시 완료
		goto queue_exit;

	if (bio_needs_zone_write_plugging(bio)) {
// zone write plug: zoned NVMe 의 sequential write 제어
		if (blk_zone_plug_bio(bio, nr_segs))
			goto queue_exit;
	}

new_request:
	if (rq) {
		blk_mq_use_cached_rq(rq, plug, bio);
// cached request 재사용: 기존 NVMe CID 를 갱신하여 사용
	} else {
		rq = blk_mq_get_new_requests(q, plug, bio);
// 새로운 NVMe CID 할당
		if (unlikely(!rq)) {
			if (bio->bi_opf & REQ_NOWAIT)
// REQ_NOWAIT 이고 CID 없으면 EAGAIN
				bio_wouldblock_error(bio);
			goto queue_exit;
		}
	}

	trace_block_getrq(bio);

	rq_qos_track(q, rq, bio);
// rq_qos_track(): NVMe IO QoS accounting 시작

	blk_mq_bio_to_request(rq, bio, nr_segs);
// bio 데이터를 request 에 복사 -> NVMe LBA/length/PRP/SGL 기초

	ret = blk_crypto_rq_get_keyslot(rq);
// encryption keyslot 획득: NVMe inline encryption 명령
	if (ret != BLK_STS_OK) {
		bio->bi_status = ret;
		bio_endio(bio);
		blk_mq_free_request(rq);
		return;
	}

	if (bio_zone_write_plugging(bio))
// zoned NVMe request 초기화
		blk_zone_write_plug_init_request(rq);

	if (op_is_flush(bio->bi_opf) && blk_insert_flush(rq))
// flush request 는 blk-flush 상태머신으로 전달
		return;

	if (plug) {
		blk_add_rq_to_plug(plug, rq);
// plug 존재 시 request 를 plug list 에 쌓음
		return;
	}

	hctx = rq->mq_hctx;
	if ((rq->rq_flags & RQF_USE_SCHED) ||
// dispatch_busy 이거나 단일 SQ+async 면 scheduler/sw queue 경유
	    (hctx->dispatch_busy && (q->nr_hw_queues == 1 || !is_sync))) {
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, true);
	} else {
		blk_mq_run_dispatch_ops(q, blk_mq_try_issue_directly(hctx, rq));
// sync 경로: blk_mq_try_issue_directly -> nvme_queue_rq -> doorbell
	}
	return;

queue_exit:
	/*
	 * Don't drop the queue reference if we were trying to use a cached
	 * request and thus didn't acquire one.
	 */
	if (!rq)
		blk_queue_exit(q);
}

#ifdef CONFIG_BLK_MQ_STACKING
/**
 * blk_insert_cloned_request - Helper for stacking drivers to submit a request
 * @rq: the request being queued
 */
blk_status_t blk_insert_cloned_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	unsigned int max_sectors = blk_queue_get_max_sectors(rq);
	unsigned int max_segments = blk_rq_get_max_segments(rq);
	blk_status_t ret;

	if (blk_rq_sectors(rq) > max_sectors) {
// cloned request 의 sector 수가 NVMe max sectors 초과 시 거부
		/*
		 * SCSI device does not have a good way to return if
		 * Write Same/Zero is actually supported. If a device rejects
		 * a non-read/write command (discard, write same,etc.) the
		 * low-level device driver will set the relevant queue limit to
		 * 0 to prevent blk-lib from issuing more of the offending
		 * operations. Commands queued prior to the queue limit being
		 * reset need to be completed with BLK_STS_NOTSUPP to avoid I/O
		 * errors being propagated to upper layers.
		 */
		if (max_sectors == 0)
			return BLK_STS_NOTSUPP;

		printk(KERN_ERR "%s: over max size limit. (%u > %u)\n",
			__func__, blk_rq_sectors(rq), max_sectors);
		return BLK_STS_IOERR;
	}

	/*
	 * The queue settings related to segment counting may differ from the
	 * original queue.
	 */
	rq->nr_phys_segments = blk_recalc_rq_segments(rq);
// 하위 queue 의 segment 계산 방식에 맞게 nr_phys_segments 재계산
	if (rq->nr_phys_segments > max_segments) {
// nr_phys_segments > max_segments: NVMe PRP/SGL 용량 초과
		printk(KERN_ERR "%s: over max segments limit. (%u > %u)\n",
			__func__, rq->nr_phys_segments, max_segments);
		return BLK_STS_IOERR;
	}

	if (q->disk && should_fail_request(q->disk->part0, blk_rq_bytes(rq)))
		return BLK_STS_IOERR;

	ret = blk_crypto_rq_get_keyslot(rq);
// cloned passthrough 도 NVMe encryption keyslot 필요
	if (ret != BLK_STS_OK)
		return ret;

	blk_account_io_start(rq);

	/*
	 * Since we have a scheduler attached on the top device,
	 * bypass a potential scheduler on the bottom device for
	 * insert.
	 */
	blk_mq_run_dispatch_ops(q,
// cloned request 를 NVMe SQ 로 즉시 발행
			ret = blk_mq_request_issue_directly(rq, true));
	if (ret)
		blk_account_io_done(rq, blk_time_get_ns());
	return ret;
}
EXPORT_SYMBOL_GPL(blk_insert_cloned_request);

/**
 * blk_rq_unprep_clone - Helper function to free all bios in a cloned request
 * @rq: the clone request to be cleaned up
 *
 * Description:
 *     Free all bios in @rq for a cloned request.
 */
void blk_rq_unprep_clone(struct request *rq)
{
	struct bio *bio;

	while ((bio = rq->bio) != NULL) {
// clone request 의 bio 들을 해제
		rq->bio = bio->bi_next;

		bio_put(bio);
	}
}
EXPORT_SYMBOL_GPL(blk_rq_unprep_clone);

/**
 * blk_rq_prep_clone - Helper function to setup clone request
 * @rq: the request to be setup
 * @rq_src: original request to be cloned
 * @bs: bio_set that bios for clone are allocated from
 * @gfp_mask: memory allocation mask for bio
 * @bio_ctr: setup function to be called for each clone bio.
 *           Returns %0 for success, non %0 for failure.
 * @data: private data to be passed to @bio_ctr
 *
 * Description:
 *     Clones bios in @rq_src to @rq, and copies attributes of @rq_src to @rq.
 *     Also, pages which the original bios are pointing to are not copied
 *     and the cloned bios just point same pages.
 *     So cloned bios must be completed before original bios, which means
 *     the caller must complete @rq before @rq_src.
 */
int blk_rq_prep_clone(struct request *rq, struct request *rq_src,
		      struct bio_set *bs, gfp_t gfp_mask,
		      int (*bio_ctr)(struct bio *, struct bio *, void *),
		      void *data)
{
	struct bio *bio_src;

	if (!bs)
		bs = &fs_bio_set;

	__rq_for_each_bio(bio_src, rq_src) {
		struct bio *bio	 = bio_alloc_clone(rq->q->disk->part0, bio_src,
// bio_alloc_clone(): 동일한 페이지를 참조하는 cloned bio 생성
					gfp_mask, bs);
		if (!bio)
			goto free_and_out;

		if (bio_ctr && bio_ctr(bio, bio_src, data)) {
			bio_put(bio);
			goto free_and_out;
		}

		if (rq->bio) {
			rq->biotail->bi_next = bio;
			rq->biotail = bio;
		} else {
			rq->bio = rq->biotail = bio;
		}
	}

	/* Copy attributes of the original request to the clone request. */
	rq->__sector = blk_rq_pos(rq_src);
// 원본 request 의 sector/length 복사 -> NVMe LBA/length
	rq->__data_len = blk_rq_bytes(rq_src);
	if (rq_src->rq_flags & RQF_SPECIAL_PAYLOAD) {
		rq->rq_flags |= RQF_SPECIAL_PAYLOAD;
		rq->special_vec = rq_src->special_vec;
	}
	rq->nr_phys_segments = rq_src->nr_phys_segments;
// nr_phys_segments 복사: NVMe PRP/SGL entry 수 동일하게 유지
	rq->nr_integrity_segments = rq_src->nr_integrity_segments;
	rq->phys_gap_bit = rq_src->phys_gap_bit;

	if (rq->bio && blk_crypto_rq_bio_prep(rq, rq->bio, gfp_mask) < 0)
		goto free_and_out;

	return 0;

free_and_out:
	blk_rq_unprep_clone(rq);

	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(blk_rq_prep_clone);
#endif /* CONFIG_BLK_MQ_STACKING */

/*
 * Steal bios from a request and add them to a bio list.
 * The request must not have been partially completed before.
 */
void blk_steal_bios(struct bio_list *list, struct request *rq)
{
	struct bio *bio;

	for (bio = rq->bio; bio; bio = bio->bi_next) {
		if (bio->bi_opf & REQ_POLLED) {
// REQ_POLLED 플래그 해제: 새 queue 에 재제출 시 poll 설정 초기화
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;
		}
		/*
		 * The alternate request queue that we may end up submitting
		 * the bio to may be frozen temporarily, in this case REQ_NOWAIT
		 * will fail the I/O immediately with EAGAIN to the issuer.
		 * We are not in the issuer context which cannot block. Clear
		 * the flag to avoid spurious EAGAIN I/O failures.
		 */
		bio->bi_opf &= ~REQ_NOWAIT;
// REQ_NOWAIT 해제: issuer context 가 아니므로 EAGAIN 방지
		bio_clear_flag(bio, BIO_QOS_THROTTLED);
		bio_clear_flag(bio, BIO_QOS_MERGED);
	}

	if (rq->bio) {
// rq 의 bio 리스트를 다른 bio list 로 이동
		if (list->tail)
			list->tail->bi_next = rq->bio;
		else
			list->head = rq->bio;
		list->tail = rq->biotail;

		rq->bio = NULL;
		rq->biotail = NULL;
	}

	rq->__data_len = 0;
}
EXPORT_SYMBOL_GPL(blk_steal_bios);

static size_t order_to_size(unsigned int order)
{
	return (size_t)PAGE_SIZE << order;
}

/* called before freeing request pool in @tags */
static void blk_mq_clear_rq_mapping(struct blk_mq_tags *drv_tags,
				    struct blk_mq_tags *tags)
{
	struct page *page;

	/*
	 * There is no need to clear mapping if driver tags is not initialized
	 * or the mapping belongs to the driver tags.
	 */
	if (!drv_tags || drv_tags == tags)
// drv_tags 미초기화 또는 동일하면 매핑 클리어 불필요
		return;

	list_for_each_entry(page, &tags->page_list, lru) {
		unsigned long start = (unsigned long)page_address(page);
// tags page_list 를 순회하며 매핑된 request 주소 범위 확인
		unsigned long end = start + order_to_size(page->private);
		int i;

		for (i = 0; i < drv_tags->nr_tags; i++) {
			struct request *rq = drv_tags->rqs[i];
// drv_tags->rqs[i]: i 번째 NVMe CID slot 의 request 매핑
			unsigned long rq_addr = (unsigned long)rq;

			if (rq_addr >= start && rq_addr < end) {
				WARN_ON_ONCE(req_ref_read(rq) != 0);
				cmpxchg(&drv_tags->rqs[i], rq, NULL);
// 참조 카운트가 0이어야 매핑 해제 가능
			}
// cmpxchg 로 NULL 설정: 완료 경로와의 race 회피
		}
	}
}

void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,
		     unsigned int hctx_idx)
{
	struct blk_mq_tags *drv_tags;

	if (list_empty(&tags->page_list))
		return;
// page_list 가 비었으면 이미 해제된 tag pool

	if (blk_mq_is_shared_tags(set->flags))
// shared tags: 여러 NVMe SQ 가 하나의 CID pool 공유
		drv_tags = set->shared_tags;
	else
		drv_tags = set->tags[hctx_idx];

	if (tags->static_rqs && set->ops->exit_request) {
		int i;

		for (i = 0; i < tags->nr_tags; i++) {
// 모든 tag slot 의 request 에 대해 driver exit_request 호출
			struct request *rq = tags->static_rqs[i];

			if (!rq)
				continue;
			set->ops->exit_request(set, rq, hctx_idx);
			tags->static_rqs[i] = NULL;
		}
	}

	blk_mq_clear_rq_mapping(drv_tags, tags);
	/*
	 * Free request pages in SRCU callback, which is called from
	 * blk_mq_free_tags().
	 */
}

void blk_mq_free_rq_map(struct blk_mq_tag_set *set, struct blk_mq_tags *tags)
{
	kfree(tags->rqs);
	tags->rqs = NULL;
	kfree(tags->static_rqs);
	tags->static_rqs = NULL;

	blk_mq_free_tags(set, tags);
}

static enum hctx_type hctx_idx_to_type(struct blk_mq_tag_set *set,
		unsigned int hctx_idx)
{
	int i;

	for (i = 0; i < set->nr_maps; i++) {
		unsigned int start = set->map[i].queue_offset;
		unsigned int end = start + set->map[i].nr_queues;

		if (hctx_idx >= start && hctx_idx < end)
			break;
	}

	if (i >= set->nr_maps)
		i = HCTX_TYPE_DEFAULT;

	return i;
}

static int blk_mq_get_hctx_node(struct blk_mq_tag_set *set,
		unsigned int hctx_idx)
{
	enum hctx_type type = hctx_idx_to_type(set, hctx_idx);

	return blk_mq_hw_queue_to_node(&set->map[type], hctx_idx);
}

static struct blk_mq_tags *blk_mq_alloc_rq_map(struct blk_mq_tag_set *set,
					       unsigned int hctx_idx,
					       unsigned int nr_tags,
					       unsigned int reserved_tags)
{
	int node = blk_mq_get_hctx_node(set, hctx_idx);
// hctx 의 NUMA node 결정: NVMe SQ 메모리 배치
	struct blk_mq_tags *tags;

	if (node == NUMA_NO_NODE)
		node = set->numa_node;

	tags = blk_mq_init_tags(nr_tags, reserved_tags, set->flags, node);
// blk_mq_init_tags(): NVMe SQ slot(CID) bitmap 초기화
	if (!tags)
		return NULL;

	tags->rqs = kcalloc_node(nr_tags, sizeof(struct request *),
// tags->rqs[]: CID 별 request 역참조 테이블
				 GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,
				 node);
	if (!tags->rqs)
		goto err_free_tags;

	tags->static_rqs = kcalloc_node(nr_tags, sizeof(struct request *),
// tags->static_rqs[]: CID 별 request 객체 포인터
					GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,
					node);
	if (!tags->static_rqs)
		goto err_free_rqs;

	return tags;

err_free_rqs:
	kfree(tags->rqs);
err_free_tags:
	blk_mq_free_tags(set, tags);
	return NULL;
}

static int blk_mq_init_request(struct blk_mq_tag_set *set, struct request *rq,
			       unsigned int hctx_idx, int node)
{
	int ret;

	if (set->ops->init_request) {
// driver 의 init_request(): NVMe queue 를 위한 request 초기화
		ret = set->ops->init_request(set, rq, hctx_idx, node);
		if (ret)
			return ret;
	}

	WRITE_ONCE(rq->state, MQ_RQ_IDLE);
// request 상태를 IDLE 로 설정: CID 재할당 가능
	return 0;
}

static int blk_mq_alloc_rqs(struct blk_mq_tag_set *set,
			    struct blk_mq_tags *tags,
			    unsigned int hctx_idx, unsigned int depth)
{
	unsigned int i, j, entries_per_page, max_order = 4;
	int node = blk_mq_get_hctx_node(set, hctx_idx);
	size_t rq_size, left;

	if (node == NUMA_NO_NODE)
		node = set->numa_node;

	/*
	 * rq_size is the size of the request plus driver payload, rounded
	 * to the cacheline size
	 */
	rq_size = round_up(sizeof(struct request) + set->cmd_size,
// request 크기 + driver payload(NVMe cmd) 를 cacheline 정렬
				cache_line_size());
	left = rq_size * depth;

	for (i = 0; i < depth; ) {
		int this_order = max_order;
		struct page *page;
		int to_do;
		void *p;

		while (this_order && left < order_to_size(this_order - 1))
			this_order--;

		do {
			page = alloc_pages_node(node,
// GFP_NOIO | __GFP_NORETRY: NVMe SQ 메모리 할당 시 IO 대기 없음
				GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY | __GFP_ZERO,
				this_order);
			if (page)
				break;
			if (!this_order--)
				break;
			if (order_to_size(this_order) < rq_size)
				break;
		} while (1);

		if (!page)
			goto fail;

		page->private = this_order;
		list_add_tail(&page->lru, &tags->page_list);

		p = page_address(page);
		/*
		 * Allow kmemleak to scan these pages as they contain pointers
		 * to additional allocations like via ops->init_request().
		 */
		kmemleak_alloc(p, order_to_size(this_order), 1, GFP_NOIO);
		entries_per_page = order_to_size(this_order) / rq_size;
// 페이지당 들어갈 request 수 계산
		to_do = min(entries_per_page, depth - i);
		left -= to_do * rq_size;
		for (j = 0; j < to_do; j++) {
			struct request *rq = p;

			tags->static_rqs[i] = rq;
// static_rqs[i] 에 CID slot 별 request 배정
			if (blk_mq_init_request(set, rq, hctx_idx, node)) {
				tags->static_rqs[i] = NULL;
				goto fail;
			}

			p += rq_size;
			i++;
		}
	}
	return 0;

fail:
	blk_mq_free_rqs(set, tags, hctx_idx);
	return -ENOMEM;
}

/*
 * struct rq_iter_data: tag 전체를 순회하며 hctx 에 속한 request
 *   존재 여부를 검사할 때 사용.
 *   hctx: 검사할 NVMe SQ.
 *   has_rq: 해당 SQ 에 아직 완료되지 않은 CID(request) 존재 여부.
 */
struct rq_iter_data {
	struct blk_mq_hw_ctx *hctx;
	bool has_rq; /* hctx 에 속한 request 존재 여부 */
};

static bool blk_mq_has_request(struct request *rq, void *data)
{
	struct rq_iter_data *iter_data = data;

	if (rq->mq_hctx != iter_data->hctx)
// rq->mq_hctx != hctx 이면 이 CID 는 다른 NVMe SQ 에 속함
		return true;
	iter_data->has_rq = true;
	return false;
}

static bool blk_mq_hctx_has_requests(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->sched_tags ?
			hctx->sched_tags : hctx->tags;
	struct rq_iter_data data = {
		.hctx	= hctx,
	};
	int srcu_idx;

	srcu_idx = srcu_read_lock(&hctx->queue->tag_set->tags_srcu);
// tags_srcu read lock: hctx/tags 동적 변경으로부터 보호
	blk_mq_all_tag_iter(tags, blk_mq_has_request, &data);
// blk_mq_all_tag_iter(): 전체 CID slot 순회
	srcu_read_unlock(&hctx->queue->tag_set->tags_srcu, srcu_idx);
// srcu_read_unlock(): NVMe submit/complete 의 SRCU 임계 종료

	return data.has_rq;
}

static bool blk_mq_hctx_has_online_cpu(struct blk_mq_hw_ctx *hctx,
		unsigned int this_cpu)
{
	enum hctx_type type = hctx->type;
	int cpu;

	/*
	 * hctx->cpumask has to rule out isolated CPUs, but userspace still
	 * might submit IOs on these isolated CPUs, so use the queue map to
	 * check if all CPUs mapped to this hctx are offline
	 */
	for_each_online_cpu(cpu) {
// online CPU 중에서 이 hctx(NVMe SQ)에 매핑된 CPU 검색
		struct blk_mq_hw_ctx *h = blk_mq_map_queue_type(hctx->queue,
				type, cpu);

		if (h != hctx)
			continue;

		/* this hctx has at least one online CPU */
		if (this_cpu != cpu)
			return true;
	}

	return false;
}

static int blk_mq_hctx_notify_offline(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx = hlist_entry_safe(node,
			struct blk_mq_hw_ctx, cpuhp_online);
	int ret = 0;

	if (!hctx->nr_ctx || blk_mq_hctx_has_online_cpu(hctx, cpu))
		return 0;

	/*
	 * Prevent new request from being allocated on the current hctx.
	 *
	 * The smp_mb__after_atomic() Pairs with the implied barrier in
	 * test_and_set_bit_lock in sbitmap_get().  Ensures the inactive flag is
	 * seen once we return from the tag allocator.
	 */
	set_bit(BLK_MQ_S_INACTIVE, &hctx->state);
// BLK_MQ_S_INACTIVE: 이 NVMe SQ 에 더 이상 새 CID 할당 금지
	smp_mb__after_atomic();
// smp_mb__after_atomic(): INACTIVE 설정과 tag allocator 사이 순서 보장

	/*
	 * Try to grab a reference to the queue and wait for any outstanding
	 * requests.  If we could not grab a reference the queue has been
	 * frozen and there are no requests.
	 */
	if (percpu_ref_tryget(&hctx->queue->q_usage_counter)) {
		while (blk_mq_hctx_has_requests(hctx)) {
// 해당 NVMe SQ 에 남은 request 가 없어질 때까지 대기
			/*
			 * The wakeup capable IRQ handler of block device is
			 * not called during suspend. Skip the loop by checking
			 * pm_wakeup_pending to prevent the deadlock and improve
			 * suspend latency.
			 */
			if (pm_wakeup_pending()) {
				clear_bit(BLK_MQ_S_INACTIVE, &hctx->state);
				ret = -EBUSY;
				break;
			}
			msleep(5);
		}
		percpu_ref_put(&hctx->queue->q_usage_counter);
	}

	return ret;
}

/*
 * Check if one CPU is mapped to the specified hctx
 *
 * Isolated CPUs have been ruled out from hctx->cpumask, which is supposed
 * to be used for scheduling kworker only. For other usage, please call this
 * helper for checking if one CPU belongs to the specified hctx
 */
static bool blk_mq_cpu_mapped_to_hctx(unsigned int cpu,
		const struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_hw_ctx *mapped_hctx = blk_mq_map_queue_type(hctx->queue,
			hctx->type, cpu);

	return mapped_hctx == hctx;
}

static int blk_mq_hctx_notify_online(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx = hlist_entry_safe(node,
			struct blk_mq_hw_ctx, cpuhp_online);

	if (blk_mq_cpu_mapped_to_hctx(cpu, hctx))
		clear_bit(BLK_MQ_S_INACTIVE, &hctx->state);
	return 0;
}

/*
 * 'cpu' is going away. splice any existing rq_list entries from this
 * software queue to the hw queue dispatch list, and ensure that it
 * gets run.
 */
static int blk_mq_hctx_notify_dead(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;
	LIST_HEAD(tmp);
	enum hctx_type type;

	hctx = hlist_entry_safe(node, struct blk_mq_hw_ctx, cpuhp_dead);
	if (!blk_mq_cpu_mapped_to_hctx(cpu, hctx))
		return 0;

	ctx = __blk_mq_get_ctx(hctx->queue, cpu);
	type = hctx->type;

	spin_lock(&ctx->lock);
	if (!list_empty(&ctx->rq_lists[type])) {
// 죽은 CPU 의 sw queue 에 남은 request 를 임시 list 로 옮김
		list_splice_init(&ctx->rq_lists[type], &tmp);
		blk_mq_hctx_clear_pending(hctx, ctx);
// ctx_map 에서 죽은 CPU pending 비트 제거
	}
	spin_unlock(&ctx->lock);

	if (list_empty(&tmp))
		return 0;

	spin_lock(&hctx->lock);
	list_splice_tail_init(&tmp, &hctx->dispatch);
// 죽은 CPU 의 request 들을 hctx->dispatch 로 이동
	spin_unlock(&hctx->lock);

	blk_mq_run_hw_queue(hctx, true);
// 이동 후 NVMe SQ dispatch rerun
	return 0;
}

static void __blk_mq_remove_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&blk_mq_cpuhp_lock);

	if (!(hctx->flags & BLK_MQ_F_STACKING) &&
	    !hlist_unhashed(&hctx->cpuhp_online)) {
		cpuhp_state_remove_instance_nocalls(CPUHP_AP_BLK_MQ_ONLINE,
						    &hctx->cpuhp_online);
		INIT_HLIST_NODE(&hctx->cpuhp_online);
	}

	if (!hlist_unhashed(&hctx->cpuhp_dead)) {
		cpuhp_state_remove_instance_nocalls(CPUHP_BLK_MQ_DEAD,
						    &hctx->cpuhp_dead);
		INIT_HLIST_NODE(&hctx->cpuhp_dead);
	}
}

static void blk_mq_remove_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	mutex_lock(&blk_mq_cpuhp_lock);
	__blk_mq_remove_cpuhp(hctx);
	mutex_unlock(&blk_mq_cpuhp_lock);
}

static void __blk_mq_add_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&blk_mq_cpuhp_lock);

	if (!(hctx->flags & BLK_MQ_F_STACKING) &&
	    hlist_unhashed(&hctx->cpuhp_online))
		cpuhp_state_add_instance_nocalls(CPUHP_AP_BLK_MQ_ONLINE,
				&hctx->cpuhp_online);

	if (hlist_unhashed(&hctx->cpuhp_dead))
		cpuhp_state_add_instance_nocalls(CPUHP_BLK_MQ_DEAD,
				&hctx->cpuhp_dead);
}

static void __blk_mq_remove_cpuhp_list(struct list_head *head)
{
	struct blk_mq_hw_ctx *hctx;

	lockdep_assert_held(&blk_mq_cpuhp_lock);

	list_for_each_entry(hctx, head, hctx_list)
		__blk_mq_remove_cpuhp(hctx);
}

/*
 * Unregister cpuhp callbacks from exited hw queues
 *
 * Safe to call if this `request_queue` is live
 */
static void blk_mq_remove_hw_queues_cpuhp(struct request_queue *q)
{
	LIST_HEAD(hctx_list);

	spin_lock(&q->unused_hctx_lock);
	list_splice_init(&q->unused_hctx_list, &hctx_list);
	spin_unlock(&q->unused_hctx_lock);

	mutex_lock(&blk_mq_cpuhp_lock);
	__blk_mq_remove_cpuhp_list(&hctx_list);
	mutex_unlock(&blk_mq_cpuhp_lock);

	spin_lock(&q->unused_hctx_lock);
	list_splice(&hctx_list, &q->unused_hctx_list);
	spin_unlock(&q->unused_hctx_lock);
}

/*
 * Register cpuhp callbacks from all hw queues
 *
 * Safe to call if this `request_queue` is live
 */
static void blk_mq_add_hw_queues_cpuhp(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	mutex_lock(&blk_mq_cpuhp_lock);
	queue_for_each_hw_ctx(q, hctx, i)
		__blk_mq_add_cpuhp(hctx);
	mutex_unlock(&blk_mq_cpuhp_lock);
}

/*
 * Before freeing hw queue, clearing the flush request reference in
 * tags->rqs[] for avoiding potential UAF.
 */
static void blk_mq_clear_flush_rq_mapping(struct blk_mq_tags *tags,
		unsigned int queue_depth, struct request *flush_rq)
{
	int i;

	/* The hw queue may not be mapped yet */
	if (!tags)
		return;

	WARN_ON_ONCE(req_ref_read(flush_rq) != 0);

	for (i = 0; i < queue_depth; i++)
		cmpxchg(&tags->rqs[i], flush_rq, NULL);
}

static void blk_free_flush_queue_callback(struct rcu_head *head)
{
	struct blk_flush_queue *fq =
		container_of(head, struct blk_flush_queue, rcu_head);

	blk_free_flush_queue(fq);
}

/* hctx->ctxs will be freed in queue's release handler */
static void blk_mq_exit_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct request *flush_rq = hctx->fq->flush_rq;

	if (blk_mq_hw_queue_mapped(hctx))
		blk_mq_tag_idle(hctx);

	if (blk_queue_init_done(q))
		blk_mq_clear_flush_rq_mapping(set->tags[hctx_idx],
				set->queue_depth, flush_rq);
	if (set->ops->exit_request)
		set->ops->exit_request(set, flush_rq, hctx_idx);

	if (set->ops->exit_hctx)
		set->ops->exit_hctx(hctx, hctx_idx);

	call_srcu(&set->tags_srcu, &hctx->fq->rcu_head,
			blk_free_flush_queue_callback);
	hctx->fq = NULL;

	spin_lock(&q->unused_hctx_lock);
	list_add(&hctx->hctx_list, &q->unused_hctx_list);
	spin_unlock(&q->unused_hctx_lock);
}

static void blk_mq_exit_hw_queues(struct request_queue *q,
		struct blk_mq_tag_set *set, int nr_queue)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (i == nr_queue)
			break;
		blk_mq_remove_cpuhp(hctx);
		blk_mq_exit_hctx(q, set, hctx, i);
	}
}

static int blk_mq_init_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned hctx_idx)
{
	gfp_t gfp = GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY;

	hctx->fq = blk_alloc_flush_queue(hctx->numa_node, set->cmd_size, gfp);
	if (!hctx->fq)
		goto fail;

	hctx->queue_num = hctx_idx; /* NVMe queue index (SQ id) 설정 */

	hctx->tags = set->tag1s[hctx_idx]; /* 해당 NVMe SQ 의 tag pool 연결 */

	if (set->ops->init_hctx &&
// driver 의 init_hctx(): NVMe SQ/CQ 구조체 초기화
	    set->ops->init_hctx(hctx, set->driver_data, hctx_idx))
		goto fail_free_fq;

	if (blk_mq_init_request(set, hctx->fq->flush_rq, hctx_idx,
				hctx->numa_node))
		goto exit_hctx;

	return 0;

 exit_hctx:
	if (set->ops->exit_hctx)
		set->ops->exit_hctx(hctx, hctx_idx);
 fail_free_fq:
	blk_free_flush_queue(hctx->fq);
	hctx->fq = NULL;
 fail:
	return -1;
}

static struct blk_mq_hw_ctx *
blk_mq_alloc_hctx(struct request_queue *q, struct blk_mq_tag_set *set,
		int node)
{
	struct blk_mq_hw_ctx *hctx;
	gfp_t gfp = GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY;

	hctx = kzalloc_node(sizeof(struct blk_mq_hw_ctx), gfp, node);
	if (!hctx)
		goto fail_alloc_hctx;

	if (!zalloc_cpumask_var_node(&hctx->cpumask, gfp, node))
		goto free_hctx;

	atomic_set(&hctx->nr_active, 0);
// hctx->nr_active = 0: NVMe SQ 활성 CID 카운터 초기화
	if (node == NUMA_NO_NODE)
		node = set->numa_node;
	hctx->numa_node = node;

	INIT_DELAYED_WORK(&hctx->run_work, blk_mq_run_work_fn);
// run_work: kblockd 가 NVMe SQ dispatch 를 실행할 work
	spin_lock_init(&hctx->lock);
	INIT_LIST_HEAD(&hctx->dispatch);
// hctx->dispatch: NVMe SQ 로 곧바로 날아갈 request list
	INIT_HLIST_NODE(&hctx->cpuhp_dead);
	INIT_HLIST_NODE(&hctx->cpuhp_online);
	hctx->queue = q;
	hctx->flags = set->flags & ~BLK_MQ_F_TAG_QUEUE_SHARED;

	INIT_LIST_HEAD(&hctx->hctx_list);

	/*
	 * Allocate space for all possible cpus to avoid allocation at
	 * runtime
	 */
	hctx->ctxs = kmalloc_array_node(nr_cpu_ids, sizeof(void *),
			gfp, node);
	if (!hctx->ctxs)
		goto free_cpumask;

	if (sbitmap_init_node(&hctx->ctx_map, nr_cpu_ids, ilog2(8),
// ctx_map: 이 NVMe SQ 에 매핑된 CPU(sw queue) bitmap
				gfp, node, false, false))
		goto free_ctxs;
	hctx->nr_ctx = 0;

	spin_lock_init(&hctx->dispatch_wait_lock);
	init_waitqueue_func_entry(&hctx->dispatch_wait, blk_mq_dispatch_wake);
// dispatch_wait: 이 NVMe SQ 의 CID 대기 waitqueue entry
	INIT_LIST_HEAD(&hctx->dispatch_wait.entry);

	blk_mq_hctx_kobj_init(hctx);

	return hctx;

 free_ctxs:
	kfree(hctx->ctxs);
 free_cpumask:
	free_cpumask_var(hctx->cpumask);
 free_hctx:
	kfree(hctx);
 fail_alloc_hctx:
	return NULL;
}

static void blk_mq_init_cpu_queues(struct request_queue *q,
				   unsigned int nr_hw_queues)
{
	struct blk_mq_tag_set *set = q->tag_set;
	unsigned int i, j;

	for_each_possible_cpu(i) {
// 모든 가능한 CPU 에 대해 software queue 초기화
		struct blk_mq_ctx *__ctx = per_cpu_ptr(q->queue_ctx, i);
		struct blk_mq_hw_ctx *hctx;
		int k;

		__ctx->cpu = i;
		spin_lock_init(&__ctx->lock);
		for (k = HCTX_TYPE_DEFAULT; k < HCTX_MAX_TYPES; k++)
			INIT_LIST_HEAD(&__ctx->rq_lists[k]);

		__ctx->queue = q;

		/*
		 * Set local node, IFF we have more than one hw queue. If
		 * not, we remain on the home node of the device
		 */
		for (j = 0; j < set->nr_maps; j++) {
			hctx = blk_mq_map_queue_type(q, j, i);
// CPU -> hctx(NVMe SQ) 매핑에 따른 NUMA node 설정
			if (nr_hw_queues > 1 && hctx->numa_node == NUMA_NO_NODE)
				hctx->numa_node = cpu_to_node(i);
		}
	}
}

struct blk_mq_tags *blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
					     unsigned int hctx_idx,
					     unsigned int depth)
{
	struct blk_mq_tags *tags;
	int ret;

	tags = blk_mq_alloc_rq_map(set, hctx_idx, depth, set->reserved_tags);
// tag set 의 CID pool 과 request pool 할당
	if (!tags)
		return NULL;

	ret = blk_mq_alloc_rqs(set, tags, hctx_idx, depth);
	if (ret) {
		blk_mq_free_rq_map(set, tags);
		return NULL;
	}

	return tags;
}

static bool __blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
				       int hctx_idx)
{
	if (blk_mq_is_shared_tags(set->flags)) {
// shared tags: 모든 NVMe SQ 가 동일한 CID pool 공유
		set->tags[hctx_idx] = set->shared_tags;

		return true;
	}

	set->tags[hctx_idx] = blk_mq_alloc_map_and_rqs(set, hctx_idx,
						       set->queue_depth);

	return set->tags[hctx_idx];
}

void blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
			     struct blk_mq_tags *tags,
			     unsigned int hctx_idx)
{
	if (tags) {
		blk_mq_free_rqs(set, tags, hctx_idx);
		blk_mq_free_rq_map(set, tags);
	}
}

static void __blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
				      unsigned int hctx_idx)
{
	if (!blk_mq_is_shared_tags(set->flags))
// shared tags 가 아닐 때만 개별 tag pool 해제
		blk_mq_free_map_and_rqs(set, set->tags[hctx_idx], hctx_idx);

	set->tags[hctx_idx] = NULL;
}

/*
 * blk_mq_map_swqueue: CPU (software queue) 를 hctx (hardware queue)
 *   에 매핑.
 *   NVMe 관점: 각 CPU 가 어느 nvme_queue(SQ/CQ 쌍) 로 I/O 를
 *   볂낼지 결정. nr_hw_queues 가 NVMe SQ 개수와 대응.
 */
static void blk_mq_map_swqueue(struct request_queue *q)
{
	unsigned int j, hctx_idx;
	unsigned long i;
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;
	struct blk_mq_tag_set *set = q->tag_set;

	queue_for_each_hw_ctx(q, hctx, i) {
		cpumask_clear(hctx->cpumask);
// 매핑 전 기존 hctx cpumask 와 ctx_map 초기화
		hctx->nr_ctx = 0;
		hctx->dispatch_from = NULL;
	}

	/*
	 * Map software to hardware queues.
	 *
	 * If the cpu isn't present, the cpu is mapped to first hctx.
	 */
	for_each_possible_cpu(i) {
// 모든 CPU 에 대해 NVMe SQ 매핑 재계산

		ctx = per_cpu_ptr(q->queue_ctx, i);
		for (j = 0; j < set->nr_maps; j++) {
			if (!set->map[j].nr_queues) {
				ctx->hctxs[j] = blk_mq_map_queue_type(q,
						HCTX_TYPE_DEFAULT, i);
				continue;
			}
			hctx_idx = set->map[j].mq_map[i];
			/* unmapped hw queue can be remapped after CPU topo changed */
			if (!set->tags[hctx_idx] &&
// tag pool 할당 실패 시 queue 0 으로 fallback
			    !__blk_mq_alloc_map_and_rqs(set, hctx_idx)) {
				/*
				 * If tags initialization fail for some hctx,
				 * that hctx won't be brought online.  In this
				 * case, remap the current ctx to hctx[0] which
				 * is guaranteed to always have tags allocated
				 */
				set->map[j].mq_map[i] = 0;
			}

			hctx = blk_mq_map_queue_type(q, j, i);
// ctx->hctxs[j] 에 이 CPU 의 NVMe SQ(hctx) 저장
			ctx->hctxs[j] = hctx;
			/*
			 * If the CPU is already set in the mask, then we've
			 * mapped this one already. This can happen if
			 * devices share queues across queue maps.
			 */
			if (cpumask_test_cpu(i, hctx->cpumask))
				continue;

			cpumask_set_cpu(i, hctx->cpumask);
// hctx->cpumask 에 이 CPU 추가: NVMe SQ affinity
			hctx->type = j;
			ctx->index_hw[hctx->type] = hctx->nr_ctx;
// ctx->index_hw[]: 이 CPU가 hctx 내 몇 번째 sw queue 인지
			hctx->ctxs[hctx->nr_ctx++] = ctx;

			/*
			 * If the nr_ctx type overflows, we have exceeded the
			 * amount of sw queues we can support.
			 */
			BUG_ON(!hctx->nr_ctx);
		}

		for (; j < HCTX_MAX_TYPES; j++)
			ctx->hctxs[j] = blk_mq_map_queue_type(q,
					HCTX_TYPE_DEFAULT, i);
	}

	queue_for_each_hw_ctx(q, hctx, i) {
		int cpu;

		/*
		 * If no software queues are mapped to this hardware queue,
		 * disable it and free the request entries.
		 */
		if (!hctx->nr_ctx) {
// sw queue 가 매핑되지 않은 NVMe SQ 는 비활성화
			/* Never unmap queue 0.  We need it as a
			 * fallback in case of a new remap fails
			 * allocation
			 */
			if (i)
				__blk_mq_free_map_and_rqs(set, i);

			hctx->tags = NULL;
			continue;
		}

		hctx->tags = set->tags[i];
// hctx->tags: 활성화된 NVMe SQ 의 tag pool 재연결
		WARN_ON(!hctx->tags);

		/*
		 * Set the map size to the number of mapped software queues.
		 * This is more accurate and more efficient than looping
		 * over all possibly mapped software queues.
		 */
		sbitmap_resize(&hctx->ctx_map, hctx->nr_ctx);
// ctx_map 크기를 실제 매핑된 sw queue 수로 조정

		/*
		 * Rule out isolated CPUs from hctx->cpumask to avoid
		 * running block kworker on isolated CPUs.
		 * FIXME: cpuset should propagate further changes to isolated CPUs
		 * here.
		 */
		rcu_read_lock();
// rcu_read_lock: isolated CPU cpumask 수정 보호
		for_each_cpu(cpu, hctx->cpumask) {
			if (cpu_is_isolated(cpu))
// isolated CPU 는 hctx cpumask 에서 제외
				cpumask_clear_cpu(cpu, hctx->cpumask);
		}
		rcu_read_unlock();

		/*
		 * Initialize batch roundrobin counts
		 */
		hctx->next_cpu = blk_mq_first_mapped_cpu(hctx);
// next_cpu: kblockd work 를 실행할 NVMe SQ 제출 CPU
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}
}

/*
 * Caller needs to ensure that we're either frozen/quiesced, or that
 * the queue isn't live yet.
 */
static void queue_set_hctx_shared(struct request_queue *q, bool shared)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (shared) {
// shared tag pool 사용 시 플래그 설정
			hctx->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
		} else {
			blk_mq_tag_idle(hctx);
			hctx->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;
		}
	}
}

static void blk_mq_update_tag_set_shared(struct blk_mq_tag_set *set,
					 bool shared)
{
	struct request_queue *q;
	unsigned int memflags;

	lockdep_assert_held(&set->tag_list_lock);

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		memflags = blk_mq_freeze_queue(q);
// queue freeze 후 shared 모드 전환: NVMe SQ 간 tag 공유
		queue_set_hctx_shared(q, shared);
		blk_mq_unfreeze_queue(q, memflags);
	}
}

static void blk_mq_del_queue_tag_set(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;

	mutex_lock(&set->tag_list_lock);
	list_del_rcu(&q->tag_set_list);
// list_del_rcu: request_queue 를 tag_set list 에서 안전 제거
	if (list_is_singular(&set->tag_list)) {
		/* just transitioned to unshared */
		set->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;
		/* update existing queue */
		blk_mq_update_tag_set_shared(set, false);
	}
	mutex_unlock(&set->tag_list_lock);
}

static void blk_mq_add_queue_tag_set(struct blk_mq_tag_set *set,
				     struct request_queue *q)
{
	mutex_lock(&set->tag_list_lock);

	/*
	 * Check to see if we're transitioning to shared (from 1 to 2 queues).
	 */
	if (!list_empty(&set->tag_list) &&
	    !(set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)) {
		set->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
// tag_set 이 shared 상태로 전환: NVMe SQ 간 CID pool 공유
		/* update existing queue */
		blk_mq_update_tag_set_shared(set, true);
	}
	if (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		queue_set_hctx_shared(q, true);
	list_add_tail_rcu(&q->tag_set_list, &set->tag_list);
// list_add_tail_rcu: request_queue 를 tag_set list 에 추가

	mutex_unlock(&set->tag_list_lock);
}

/* All allocations will be freed in release handler of q->mq_kobj */
static int blk_mq_alloc_ctxs(struct request_queue *q)
{
	struct blk_mq_ctxs *ctxs;
	int cpu;

	ctxs = kzalloc_obj(*ctxs);
// blk_mq_ctxs: per-CPU software queue 컨테이너
	if (!ctxs)
		return -ENOMEM;

	ctxs->queue_ctx = alloc_percpu(struct blk_mq_ctx);
// per-CPU blk_mq_ctx 할당
	if (!ctxs->queue_ctx)
		goto fail;

	for_each_possible_cpu(cpu) {
		struct blk_mq_ctx *ctx = per_cpu_ptr(ctxs->queue_ctx, cpu);
		ctx->ctxs = ctxs;
	}

	q->mq_kobj = &ctxs->kobj;
	q->queue_ctx = ctxs->queue_ctx;

	return 0;
 fail:
	kfree(ctxs);
	return -ENOMEM;
}

/*
 * It is the actual release handler for mq, but we do it from
 * request queue's release handler for avoiding use-after-free
 * and headache because q->mq_kobj shouldn't have been introduced,
 * but we can't group ctx/kctx kobj without it.
 */
void blk_mq_release(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx, *next;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
// release 전 모든 hctx 가 unused list 에 있어야 함
		WARN_ON_ONCE(hctx && list_empty(&hctx->hctx_list));

	/* all hctx are in .unused_hctx_list now */
	list_for_each_entry_safe(hctx, next, &q->unused_hctx_list, hctx_list) {
		list_del_init(&hctx->hctx_list);
		kobject_put(&hctx->kobj);
	}

	kfree(q->queue_hw_ctx);

	/*
	 * release .mq_kobj and sw queue's kobject now because
	 * both share lifetime with request queue.
	 */
	blk_mq_sysfs_deinit(q);
}

/*
 * blk_mq_alloc_queue: blk-mq request_queue 를 생성.
 *   NVMe 관점: NVMe namespace 의 상위 I/O 큐를 생성하고 tag set 과
 *   연결.
 */
struct request_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata)
{
	struct queue_limits default_lim = { };
	struct request_queue *q;
	int ret;

	if (!lim)
		lim = &default_lim;
	lim->features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT;
// BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT: NVMe IO 통계/nowait 지원
	if (set->nr_maps > HCTX_TYPE_POLL)
		lim->features |= BLK_FEAT_POLL;
// POLL queue 지원 시 BLK_FEAT_POLL 추가

	q = blk_alloc_queue(lim, set->numa_node);
	if (IS_ERR(q))
		return q;
	q->queuedata = queuedata;
// queuedata: NVMe controller 구조체(nvme_ns 등) 연결
	ret = blk_mq_init_allocated_queue(set, q);
	if (ret) {
		blk_put_queue(q);
		return ERR_PTR(ret);
	}
	return q;
}
EXPORT_SYMBOL(blk_mq_alloc_queue);

/**
 * blk_mq_destroy_queue - shutdown a request queue
 * @q: request queue to shutdown
 *
 * This shuts down a request queue allocated by blk_mq_alloc_queue(). All future
 * requests will be failed with -ENODEV. The caller is responsible for dropping
 * the reference from blk_mq_alloc_queue() by calling blk_put_queue().
 *
 * Context: can sleep
 */
void blk_mq_destroy_queue(struct request_queue *q)
{
	WARN_ON_ONCE(!queue_is_mq(q));
	WARN_ON_ONCE(blk_queue_registered(q));

	might_sleep();

	blk_queue_flag_set(QUEUE_FLAG_DYING, q);
// QUEUE_FLAG_DYING: NVMe namespace 가 제거/종료 중
	blk_queue_start_drain(q);
// blk_queue_start_drain(): 진행 중 NVMe IO 를 완료/드레인 시작
	blk_mq_freeze_queue_wait(q);
// freeze_wait: 진행 중 request(CID) 참조 0 될 때까지 대기

	blk_sync_queue(q);
	blk_mq_cancel_work_sync(q);
// requeue/run work 취소: NVMe SQ dispatch 중단
	blk_mq_exit_queue(q);
}
EXPORT_SYMBOL(blk_mq_destroy_queue);

/*
 * blk_mq_alloc_disk: gendisk 를 생성하고 blk-mq queue 를 연결.
 *   NVMe 관점: NVMe namespace 를 블록 장치(/dev/nvme*) 로 등록할
 *   때 사용.
 */
struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata,
		struct lock_class_key *lkclass)
{
	struct request_queue *q;
	struct gendisk *disk;

	q = blk_mq_alloc_queue(set, lim, queuedata);
	if (IS_ERR(q))
		return ERR_CAST(q);

	disk = __alloc_disk_node(q, set->numa_node, lkclass);
	if (!disk) {
		blk_mq_destroy_queue(q);
		blk_put_queue(q);
		return ERR_PTR(-ENOMEM);
	}
	set_bit(GD_OWNS_QUEUE, &disk->state);
	return disk;
}
EXPORT_SYMBOL(__blk_mq_alloc_disk);

struct gendisk *blk_mq_alloc_disk_for_queue(struct request_queue *q,
		struct lock_class_key *lkclass)
{
	struct gendisk *disk;

	if (!blk_get_queue(q))
		return NULL;
	disk = __alloc_disk_node(q, NUMA_NO_NODE, lkclass);
	if (!disk)
		blk_put_queue(q);
	return disk;
}
EXPORT_SYMBOL(blk_mq_alloc_disk_for_queue);

/*
 * Only hctx removed from cpuhp list can be reused
 */
static bool blk_mq_hctx_is_reusable(struct blk_mq_hw_ctx *hctx)
{
	return hlist_unhashed(&hctx->cpuhp_online) &&
		hlist_unhashed(&hctx->cpuhp_dead);
}

static struct blk_mq_hw_ctx *blk_mq_alloc_and_init_hctx(
		struct blk_mq_tag_set *set, struct request_queue *q,
		int hctx_idx, int node)
{
	struct blk_mq_hw_ctx *hctx = NULL, *tmp;

	/* reuse dead hctx first */
	spin_lock(&q->unused_hctx_lock);
	list_for_each_entry(tmp, &q->unused_hctx_list, hctx_list) {
		if (tmp->numa_node == node && blk_mq_hctx_is_reusable(tmp)) {
			hctx = tmp;
			break;
		}
	}
	if (hctx)
		list_del_init(&hctx->hctx_list);
	spin_unlock(&q->unused_hctx_lock);

	if (!hctx)
		hctx = blk_mq_alloc_hctx(q, set, node);
	if (!hctx)
		goto fail;

	if (blk_mq_init_hctx(q, set, hctx, hctx_idx))
		goto free_hctx;

	return hctx;

 free_hctx:
	kobject_put(&hctx->kobj);
 fail:
	return NULL;
}

static void __blk_mq_realloc_hw_ctxs(struct blk_mq_tag_set *set,
				     struct request_queue *q)
{
	int i, j, end;
	struct blk_mq_hw_ctx **hctxs = q->queue_hw_ctx;

	if (q->nr_hw_queues < set->nr_hw_queues) {
		struct blk_mq_hw_ctx **new_hctxs;

		new_hctxs = kcalloc_node(set->nr_hw_queues,
				       sizeof(*new_hctxs), GFP_KERNEL,
				       set->numa_node);
		if (!new_hctxs)
			return;
		if (hctxs)
			memcpy(new_hctxs, hctxs, q->nr_hw_queues *
			       sizeof(*hctxs));
		rcu_assign_pointer(q->queue_hw_ctx, new_hctxs);
		/*
		 * Make sure reading the old queue_hw_ctx from other
		 * context concurrently won't trigger uaf.
		 */
		kfree_rcu_mightsleep(hctxs);
		hctxs = new_hctxs;
	}

	for (i = 0; i < set->nr_hw_queues; i++) {
		int old_node;
		int node = blk_mq_get_hctx_node(set, i);
		struct blk_mq_hw_ctx *old_hctx = hctxs[i];

		if (old_hctx) {
			old_node = old_hctx->numa_node;
			blk_mq_exit_hctx(q, set, old_hctx, i);
		}

		hctxs[i] = blk_mq_alloc_and_init_hctx(set, q, i, node);
		if (!hctxs[i]) {
			if (!old_hctx)
				break;
			pr_warn("Allocate new hctx on node %d fails, fallback to previous one on node %d\n",
					node, old_node);
			hctxs[i] = blk_mq_alloc_and_init_hctx(set, q, i,
					old_node);
			WARN_ON_ONCE(!hctxs[i]);
		}
	}
	/*
	 * Increasing nr_hw_queues fails. Free the newly allocated
	 * hctxs and keep the previous q->nr_hw_queues.
	 */
	if (i != set->nr_hw_queues) {
		j = q->nr_hw_queues;
		end = i;
	} else {
		j = i;
		end = q->nr_hw_queues;
		q->nr_hw_queues = set->nr_hw_queues;
	}

	for (; j < end; j++) {
		struct blk_mq_hw_ctx *hctx = hctxs[j];

		if (hctx) {
			blk_mq_exit_hctx(q, set, hctx, j);
			hctxs[j] = NULL;
		}
	}
}

static void blk_mq_realloc_hw_ctxs(struct blk_mq_tag_set *set,
				   struct request_queue *q)
{
	__blk_mq_realloc_hw_ctxs(set, q);

	/* unregister cpuhp callbacks for exited hctxs */
	blk_mq_remove_hw_queues_cpuhp(q);

	/* register cpuhp for new initialized hctxs */
	blk_mq_add_hw_queues_cpuhp(q);
}

/*
 * blk_mq_init_allocated_queue: request_queue 를 blk-mq 로 초기화.
 *   NVMe 관점: request_queue 가 NVMe SQ/CQ 의 상위 큐로 동작하도록
 *   hctx 를 할당하고 tag set 을 연결, timeout/requeue work 를
 *   설정한다.
 */
int blk_mq_init_allocated_queue(struct blk_mq_tag_set *set,
		struct request_queue *q)
{
	/* mark the queue as mq asap */
	q->mq_ops = set->ops; /* nvme_mq_ops (추정) 와 request_queue 연결 */

	/*
	 * ->tag_set has to be setup before initialize hctx, which cpuphp
	 * handler needs it for checking queue mapping
	 */
	q->tag_set = set; /* NVMe tag set(SQ slot pool) 연결 */

	if (blk_mq_alloc_ctxs(q))
		goto err_exit;

	/* init q->mq_kobj and sw queues' kobjects */
	blk_mq_sysfs_init(q);

	INIT_LIST_HEAD(&q->unused_hctx_list);
	spin_lock_init(&q->unused_hctx_lock);

	blk_mq_realloc_hw_ctxs(set, q); /* NVMe SQ/CQ 쌍에 해당하는 hctx 할당 */
	if (!q->nr_hw_queues)
		goto err_hctxs;

	INIT_WORK(&q->timeout_work, blk_mq_timeout_work);
// timeout_work: NVMe 명령 deadline 초과 검사 work
	blk_queue_rq_timeout(q, set->timeout ? set->timeout : 30 * HZ); /* NVMe 명령 timeout 설정 */

	q->queue_flags |= QUEUE_FLAG_MQ_DEFAULT;

	INIT_DELAYED_WORK(&q->requeue_work, blk_mq_requeue_work);
	INIT_LIST_HEAD(&q->flush_list);
	INIT_LIST_HEAD(&q->requeue_list);
	spin_lock_init(&q->requeue_lock);

	q->nr_requests = set->queue_depth;
// q->nr_requests: NVMe queue depth 설정
	q->async_depth = set->queue_depth;

	blk_mq_init_cpu_queues(q, set->nr_hw_queues); /* CPU 와 NVMe SQ affinity 초기화 */
	blk_mq_map_swqueue(q); /* CPU -> NVMe SQ 매핑 완료 */
	blk_mq_add_queue_tag_set(set, q);
	return 0;

err_hctxs:
	blk_mq_release(q);
err_exit:
	q->mq_ops = NULL;
	return -ENOMEM;
}
EXPORT_SYMBOL(blk_mq_init_allocated_queue);

/* tags can _not_ be used after returning from blk_mq_exit_queue */
void blk_mq_exit_queue(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;

	/* Checks hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED. */
	blk_mq_exit_hw_queues(q, set, set->nr_hw_queues);
// 모든 hctx(NVMe SQ) 종료 및 tag pool 해제
	/* May clear BLK_MQ_F_TAG_QUEUE_SHARED in hctx->flags. */
	blk_mq_del_queue_tag_set(q);
}

static int __blk_mq_alloc_rq_maps(struct blk_mq_tag_set *set)
{
	int i;

	if (blk_mq_is_shared_tags(set->flags)) {
// shared tags: 하나의 CID pool 을 모든 NVMe SQ 가 공유
		set->shared_tags = blk_mq_alloc_map_and_rqs(set,
						BLK_MQ_NO_HCTX_IDX,
						set->queue_depth);
		if (!set->shared_tags)
			return -ENOMEM;
	}

	for (i = 0; i < set->nr_hw_queues; i++) {
// 각 NVMe SQ(hctx) 별 CID pool 할당
		if (!__blk_mq_alloc_map_and_rqs(set, i))
			goto out_unwind;
		cond_resched();
	}

	return 0;

out_unwind:
	while (--i >= 0)
		__blk_mq_free_map_and_rqs(set, i);

	if (blk_mq_is_shared_tags(set->flags)) {
		blk_mq_free_map_and_rqs(set, set->shared_tags,
					BLK_MQ_NO_HCTX_IDX);
	}

	return -ENOMEM;
}

/*
 * Allocate the request maps associated with this tag_set. Note that this
 * may reduce the depth asked for, if memory is tight. set->queue_depth
 * will be updated to reflect the allocated depth.
 */
static int blk_mq_alloc_set_map_and_rqs(struct blk_mq_tag_set *set)
{
	unsigned int depth;
	int err;

	depth = set->queue_depth;
// 요청된 queue_depth 만큼 CID pool 할당 시도
	do {
		err = __blk_mq_alloc_rq_maps(set);
// __blk_mq_alloc_rq_maps(): NVMe SQ 별 tag/request pool 할당
		if (!err)
			break;

		set->queue_depth >>= 1;
// 메모리 부족 시 queue_depth 절반으로 줄여 재시도
		if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN) {
			err = -ENOMEM;
			break;
		}
	} while (set->queue_depth);

	if (!set->queue_depth || err) {
		pr_err("blk-mq: failed to allocate request map\n");
		return -ENOMEM;
	}

	if (depth != set->queue_depth)
		pr_info("blk-mq: reduced tag depth (%u -> %u)\n",
						depth, set->queue_depth);

	return 0;
}

static void blk_mq_update_queue_map(struct blk_mq_tag_set *set)
{
	/*
	 * blk_mq_map_queues() and multiple .map_queues() implementations
	 * expect that set->map[HCTX_TYPE_DEFAULT].nr_queues is set to the
	 * number of hardware queues.
	 */
	if (set->nr_maps == 1)
// 단일 map 이면 nr_queues 를 NVMe SQ 개수로 설정
		set->map[HCTX_TYPE_DEFAULT].nr_queues = set->nr_hw_queues;

	if (set->ops->map_queues) {
		int i;

		/*
		 * transport .map_queues is usually done in the following
		 * way:
		 *
		 * for (queue = 0; queue < set->nr_hw_queues; queue++) {
		 * 	mask = get_cpu_mask(queue)
		 * 	for_each_cpu(cpu, mask)
		 * 		set->map[x].mq_map[cpu] = queue;
		 * }
		 *
		 * When we need to remap, the table has to be cleared for
		 * killing stale mapping since one CPU may not be mapped
		 * to any hw queue.
		 */
		for (i = 0; i < set->nr_maps; i++)
// 기존 CPU->SQ 매핑 테이블 초기화
			blk_mq_clear_mq_map(&set->map[i]);

		set->ops->map_queues(set);
// driver 의 map_queues(): CPU affinity -> NVMe SQ 매핑 수행
	} else {
		BUG_ON(set->nr_maps > 1);
		blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);
	}
}

static struct blk_mq_tags **blk_mq_prealloc_tag_set_tags(
				struct blk_mq_tag_set *set,
				int new_nr_hw_queues)
{
	struct blk_mq_tags **new_tags;
	int i;

	if (set->nr_hw_queues >= new_nr_hw_queues)
		return NULL;

	new_tags = kcalloc_node(new_nr_hw_queues, sizeof(struct blk_mq_tags *),
// kcalloc: nr_hw_queues 개수만큼 tag 포인터 배열
				GFP_KERNEL, set->numa_node);
	if (!new_tags)
		return ERR_PTR(-ENOMEM);

	if (set->tags)
		memcpy(new_tags, set->tags, set->nr_hw_queues *
		       sizeof(*set->tags));

	for (i = set->nr_hw_queues; i < new_nr_hw_queues; i++) {
		if (blk_mq_is_shared_tags(set->flags)) {
			new_tags[i] = set->shared_tags;
// shared tags 이면 새 tag 할당 없이 기존 pool 공유
		} else {
			new_tags[i] = blk_mq_alloc_map_and_rqs(set, i,
					set->queue_depth);
			if (!new_tags[i])
				goto out_unwind;
		}
		cond_resched();
	}

	return new_tags;
out_unwind:
	while (--i >= set->nr_hw_queues) {
		if (!blk_mq_is_shared_tags(set->flags))
			blk_mq_free_map_and_rqs(set, new_tags[i], i);
	}
	kfree(new_tags);
	return ERR_PTR(-ENOMEM);
}

/*
 * Alloc a tag set to be associated with one or more request queues.
 * May fail with EINVAL for various error conditions. May adjust the
 * requested depth down, if it's too large. In that case, the set
 * value will be stored in set->queue_depth.
 */
/*
 * blk_mq_alloc_tag_set: blk-mq 드라이버가 tag set(request pool) 을
 *   초기화.
 *   NVMe 관점: NVMe 드라이버가 SQ/CQ 쌍 개수(nr_hw_queues) 와
 *   queue depth(최대 CID 수) 를 등록. 이 태그 집합이 NVMe SQ slot
 *   풀의 기반이 된다.
 */
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
{
	int i, ret;

	BUILD_BUG_ON(BLK_MQ_MAX_DEPTH > 1 << BLK_MQ_UNIQUE_TAG_BITS);
// BUILD_BUG_ON: NVMe SQ slot(CID) 최대 개수 제한

	if (!set->nr_hw_queues)
// nr_hw_queues == 0 이면 NVMe SQ 가 없는 것이므로 오류
		return -EINVAL;
	if (!set->queue_depth)
// queue_depth == 0 이면 NVMe SQ slot 이 없으므로 오류
		return -EINVAL;
	if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN)
		return -EINVAL;

	if (!set->ops->queue_rq) /* NVMe queue_rq 콜백 필수 등록 검사 */
		return -EINVAL;

	if (!set->ops->get_budget ^ !set->ops->put_budget)
		return -EINVAL;

	if (set->queue_depth > BLK_MQ_MAX_DEPTH) {
// BLK_MQ_MAX_DEPTH 초과 시 NVMe SQ depth 를 최대값으로 제한
		pr_info("blk-mq: reduced tag depth to %u\n",
			BLK_MQ_MAX_DEPTH);
		set->queue_depth = BLK_MQ_MAX_DEPTH;
	}

	if (!set->nr_maps)
		set->nr_maps = 1;
	else if (set->nr_maps > HCTX_MAX_TYPES)
		return -EINVAL;

	/*
	 * If a crashdump is active, then we are potentially in a very
	 * memory constrained environment. Limit us to  64 tags to prevent
	 * using too much memory.
	 */
	if (is_kdump_kernel())
// kdump 환경에서는 메모리 제한으로 CID 수 축소
		set->queue_depth = min(64U, set->queue_depth);

	/*
	 * There is no use for more h/w queues than cpus if we just have
	 * a single map
	 */
	if (set->nr_maps == 1 && set->nr_hw_queues > nr_cpu_ids)
// 단일 map 에서는 NVMe SQ 수를 CPU 수로 제한
		set->nr_hw_queues = nr_cpu_ids;

	if (set->flags & BLK_MQ_F_BLOCKING) {
// BLK_MQ_F_BLOCKING: SRCU 기반 NVMe submit 보호 사용
		set->srcu = kmalloc_obj(*set->srcu);
		if (!set->srcu)
			return -ENOMEM;
		ret = init_srcu_struct(set->srcu);
		if (ret)
			goto out_free_srcu;
	}
	ret = init_srcu_struct(&set->tags_srcu);
// tags_srcu: tag/hctx 구조체 동적 변경 보호
	if (ret)
		goto out_cleanup_srcu;

	init_rwsem(&set->update_nr_hwq_lock);

	ret = -ENOMEM;
	set->tags = kcalloc_node(set->nr_hw_queues,
// tags 포인터 배열: NVMe SQ 별 CID pool
				 sizeof(struct blk_mq_tags *), GFP_KERNEL,
				 set->numa_node);
	if (!set->tags)
		goto out_cleanup_tags_srcu;

	for (i = 0; i < set->nr_maps; i++) {
// map[i].mq_map: CPU -> NVMe SQ index 테이블
		set->map[i].mq_map = kcalloc_node(nr_cpu_ids,
						  sizeof(set->map[i].mq_map[0]),
						  GFP_KERNEL, set->numa_node);
		if (!set->map[i].mq_map)
			goto out_free_mq_map;
		set->map[i].nr_queues = set->nr_hw_queues;
	}

	blk_mq_update_queue_map(set);

	ret = blk_mq_alloc_set_map_and_rqs(set); /* NVMe SQ slot(CID) pool 할당 */
	if (ret)
		goto out_free_mq_map;

	mutex_init(&set->tag_list_lock);
	INIT_LIST_HEAD(&set->tag_list);

	return 0;

out_free_mq_map:
	for (i = 0; i < set->nr_maps; i++) {
		kfree(set->map[i].mq_map);
		set->map[i].mq_map = NULL;
	}
	kfree(set->tags);
	set->tags = NULL;
out_cleanup_tags_srcu:
	cleanup_srcu_struct(&set->tags_srcu);
out_cleanup_srcu:
	if (set->flags & BLK_MQ_F_BLOCKING)
		cleanup_srcu_struct(set->srcu);
out_free_srcu:
	if (set->flags & BLK_MQ_F_BLOCKING)
		kfree(set->srcu);
	return ret;
}
EXPORT_SYMBOL(blk_mq_alloc_tag_set);

/* allocate and initialize a tagset for a simple single-queue device */
/*
 * blk_mq_alloc_sq_tag_set: 단일 하드웨어 큐용 tag set 을 간편 초기화.
 *   NVMe 관점: 단일 SQ 를 가진 단순 NVMe 장치(또는 레거시) 용.
 */
int blk_mq_alloc_sq_tag_set(struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int queue_depth,
		unsigned int set_flags)
{
	memset(set, 0, sizeof(*set));
	set->ops = ops;
	set->nr_hw_queues = 1;
// nr_hw_queues = 1: 단일 NVMe SQ 모드
	set->nr_maps = 1;
	set->queue_depth = queue_depth;
	set->numa_node = NUMA_NO_NODE;
	set->flags = set_flags;
	return blk_mq_alloc_tag_set(set);
}
EXPORT_SYMBOL_GPL(blk_mq_alloc_sq_tag_set);

/*
 * blk_mq_free_tag_set: tag set 과 관련 request pool 을 해제.
 *   NVMe 관점: NVMe SQ/CQ slot 풀을 해제하고 SRCU grace period 를
 *   기다린다.
 */
void blk_mq_free_tag_set(struct blk_mq_tag_set *set)
{
	int i, j;

	for (i = 0; i < set->nr_hw_queues; i++)
// 각 NVMe SQ 의 tag pool 해제
		__blk_mq_free_map_and_rqs(set, i);

	if (blk_mq_is_shared_tags(set->flags)) {
// shared tags 해제
		blk_mq_free_map_and_rqs(set, set->shared_tags,
					BLK_MQ_NO_HCTX_IDX);
	}

	for (j = 0; j < set->nr_maps; j++) {
		kfree(set->map[j].mq_map);
		set->map[j].mq_map = NULL;
	}

	kfree(set->tags);
	set->tags = NULL;

	srcu_barrier(&set->tags_srcu);
// srcu_barrier(): NVMe submit/complete 의 SRCU grace period 완료 대기
	cleanup_srcu_struct(&set->tags_srcu);
	if (set->flags & BLK_MQ_F_BLOCKING) {
		cleanup_srcu_struct(set->srcu);
		kfree(set->srcu);
	}
}
EXPORT_SYMBOL(blk_mq_free_tag_set);

struct elevator_tags *blk_mq_update_nr_requests(struct request_queue *q,
						struct elevator_tags *et,
						unsigned int nr)
{
	struct blk_mq_tag_set *set = q->tag_set;
	struct elevator_tags *old_et = NULL;
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	blk_mq_quiesce_queue(q);
// quiesce: NVMe SQ dispatch 정지 후 queue depth 변경

	if (blk_mq_is_shared_tags(set->flags)) {
		/*
		 * Shared tags, for sched tags, we allocate max initially hence
		 * tags can't grow, see blk_mq_alloc_sched_tags().
		 */
		if (q->elevator)
			blk_mq_tag_update_sched_shared_tags(q, nr);
// shared tags + scheduler: sched shared tags 크기 갱신
		else
			blk_mq_tag_resize_shared_tags(set, nr);
// shared tags 사용 시 전체 CID pool 크기 조정
	} else if (!q->elevator) {
		/*
		 * Non-shared hardware tags, nr is already checked from
		 * queue_requests_store() and tags can't grow.
		 */
		queue_for_each_hw_ctx(q, hctx, i) {
// 각 NVMe SQ 의 일반 tag pool 크기 조정
			if (!hctx->tags)
				continue;
			sbitmap_queue_resize(&hctx->tags->bitmap_tags,
// sbitmap_queue_resize(): NVMe SQ bitmap 의 유효 CID 수 조정
				nr - hctx->tags->nr_reserved_tags);
		}
	} else if (nr <= q->elevator->et->nr_requests) {
		/* Non-shared sched tags, and tags don't grow. */
		queue_for_each_hw_ctx(q, hctx, i) {
// scheduler tag pool 크기 조정
			if (!hctx->sched_tags)
				continue;
			sbitmap_queue_resize(&hctx->sched_tags->bitmap_tags,
				nr - hctx->sched_tags->nr_reserved_tags);
		}
	} else {
		/* Non-shared sched tags, and tags grow */
		queue_for_each_hw_ctx(q, hctx, i)
			hctx->sched_tags = et->tags[i];
		old_et =  q->elevator->et;
		q->elevator->et = et;
	}

	/*
	 * Preserve relative value, both nr and async_depth are at most 16 bit
	 * value, no need to worry about overflow.
	 */
	q->async_depth = max(q->async_depth * nr / q->nr_requests, 1);
// async_depth 상대값 유지: NVMe async queue depth 조정
	q->nr_requests = nr;
	if (q->elevator && q->elevator->type->ops.depth_updated)
		q->elevator->type->ops.depth_updated(q);

	blk_mq_unquiesce_queue(q);
	return old_et;
}

/*
 * Switch back to the elevator type stored in the xarray.
 */
static void blk_mq_elv_switch_back(struct request_queue *q,
		struct xarray *elv_tbl)
{
	struct elv_change_ctx *ctx = xa_load(elv_tbl, q->id);
// elevator 전환 컨텍스트 복원

	if (WARN_ON_ONCE(!ctx))
		return;

	/* The elv_update_nr_hw_queues unfreezes the queue. */
	elv_update_nr_hw_queues(q, ctx);

	/* Drop the reference acquired in blk_mq_elv_switch_none. */
	if (ctx->type)
		elevator_put(ctx->type);
}

/*
 * Stores elevator name and type in ctx and set current elevator to none.
 */
static int blk_mq_elv_switch_none(struct request_queue *q,
		struct xarray *elv_tbl)
{
	struct elv_change_ctx *ctx;

	lockdep_assert_held_write(&q->tag_set->update_nr_hwq_lock);

	/*
	 * Accessing q->elevator without holding q->elevator_lock is safe here
	 * because we're called from nr_hw_queue update which is protected by
	 * set->update_nr_hwq_lock in the writer context. So, scheduler update/
	 * switch code (which acquires the same lock in the reader context)
	 * can't run concurrently.
	 */
	if (q->elevator) {
		ctx = xa_load(elv_tbl, q->id);
		if (WARN_ON_ONCE(!ctx))
			return -ENOENT;

		ctx->name = q->elevator->type->elevator_name;

		/*
		 * Before we switch elevator to 'none', take a reference to
		 * the elevator module so that while nr_hw_queue update is
		 * running, no one can remove elevator module. We'd put the
		 * reference to elevator module later when we switch back
		 * elevator.
		 */
		__elevator_get(q->elevator->type);
// elevator 모듈 참조 유지: 전환 중 제거 방지

		/*
		 * Store elevator type so that we can release the reference
		 * taken above later.
		 */
		ctx->type = q->elevator->type;
		elevator_set_none(q);
	}
	return 0;
}

static void __blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set,
							int nr_hw_queues)
{
	struct request_queue *q;
	int prev_nr_hw_queues = set->nr_hw_queues;
	unsigned int memflags;
	int i;
	struct xarray elv_tbl;
	struct blk_mq_tags **new_tags;
	bool queues_frozen = false;

	lockdep_assert_held(&set->tag_list_lock);

	if (set->nr_maps == 1 && nr_hw_queues > nr_cpu_ids)
// 단일 map 일 때 NVMe SQ 수를 CPU 수로 상한
		nr_hw_queues = nr_cpu_ids;
	if (nr_hw_queues < 1)
		return;
	if (set->nr_maps == 1 && nr_hw_queues == set->nr_hw_queues)
		return;

	memflags = memalloc_noio_save();
// 메모리 할당 NOIO 모드: NVMe SQ 재구성 중 IO 방지

	xa_init(&elv_tbl);
	if (blk_mq_alloc_sched_ctx_batch(&elv_tbl, set) < 0)
		goto out_free_ctx;

	if (blk_mq_alloc_sched_res_batch(&elv_tbl, set, nr_hw_queues) < 0)
		goto out_free_ctx;

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
// 기존 hctx sysfs/debugfs 등록 해제
		blk_mq_debugfs_unregister_hctxs(q);
		blk_mq_sysfs_unregister_hctxs(q);
	}

	/*
	 * Switch IO scheduler to 'none', cleaning up the data associated
	 * with the previous scheduler. We will switch back once we are done
	 * updating the new sw to hw queue mappings.
	 */
	list_for_each_entry(q, &set->tag_list, tag_set_list)
		if (blk_mq_elv_switch_none(q, &elv_tbl))
			goto switch_back;

	new_tags = blk_mq_prealloc_tag_set_tags(set, nr_hw_queues);
	if (IS_ERR(new_tags))
		goto switch_back;

	list_for_each_entry(q, &set->tag_list, tag_set_list)
		blk_mq_freeze_queue_nomemsave(q);
// 모든 request_queue(NVMe namespace) freeze
	queues_frozen = true;
	if (new_tags) {
		kfree(set->tags);
		set->tags = new_tags;
	}
// 새 tags 배열 설정
	set->nr_hw_queues = nr_hw_queues;
// set->nr_hw_queues: NVMe SQ 개수 갱신

fallback:
	blk_mq_update_queue_map(set);
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
// 각 request_queue 의 hctx(NVMe SQ) 재할당
		__blk_mq_realloc_hw_ctxs(set, q);

		if (q->nr_hw_queues != set->nr_hw_queues) {
			int i = prev_nr_hw_queues;

			pr_warn("Increasing nr_hw_queues to %d fails, fallback to %d\n",
					nr_hw_queues, prev_nr_hw_queues);
			for (; i < set->nr_hw_queues; i++)
				__blk_mq_free_map_and_rqs(set, i);

			set->nr_hw_queues = prev_nr_hw_queues;
			goto fallback;
		}
		blk_mq_map_swqueue(q); /* CPU -> NVMe SQ 매핑 완료 */
	}
switch_back:
	/* The blk_mq_elv_switch_back unfreezes queue for us. */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* switch_back expects queue to be frozen */
		if (!queues_frozen)
// switch_back 를 위해 queue 를 다시 freeze
			blk_mq_freeze_queue_nomemsave(q);
		blk_mq_elv_switch_back(q, &elv_tbl);
	}

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
// 새 hctx sysfs/debugfs 등록 및 cpuhp 갱신
		blk_mq_sysfs_register_hctxs(q);
		blk_mq_debugfs_register_hctxs(q);

		blk_mq_remove_hw_queues_cpuhp(q);
		blk_mq_add_hw_queues_cpuhp(q);
	}

out_free_ctx:
	blk_mq_free_sched_ctx_batch(&elv_tbl);
	xa_destroy(&elv_tbl);
	memalloc_noio_restore(memflags);

	/* Free the excess tags when nr_hw_queues shrink. */
	for (i = set->nr_hw_queues; i < prev_nr_hw_queues; i++)
		__blk_mq_free_map_and_rqs(set, i);
}

/*
 * blk_mq_update_nr_hw_queues: 런타임에 하드웨어 큐 개수를 변경.
 *   NVMe 관점: NVMe SQ/CQ 쌍 개수(nr_hw_queues) 를 동적으로
 *   재구성. queue freeze 와 CPU affinity 재매핑을 수행.
 */
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues)
{
	down_write(&set->update_nr_hwq_lock);
	mutex_lock(&set->tag_list_lock);
	__blk_mq_update_nr_hw_queues(set, nr_hw_queues);
	mutex_unlock(&set->tag_list_lock);
	up_write(&set->update_nr_hwq_lock);
}
EXPORT_SYMBOL_GPL(blk_mq_update_nr_hw_queues);

/*
 * blk_hctx_poll: polling hctx 의 완료를 폴리.
 *   NVMe 관점: NVMe poll queue 에 대해 mq_ops->poll (nvme_poll)
 *   을 반복 호출하여 CQ 항목을 소비.
 */
static int blk_hctx_poll(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			 struct io_comp_batch *iob, unsigned int flags)
{
	int ret;

	do {
		ret = q->mq_ops->poll(hctx, iob);
// q->mq_ops->poll == nvme_poll: CQ 항목 직접 소비
		if (ret > 0)
			return ret;
		if (task_sigpending(current))
// signal pending 시 poll 중단
			return 1;
		if (ret < 0 || (flags & BLK_POLL_ONESHOT))
// BLK_POLL_ONESHOT: 한 번만 NVMe CQ 폴링
			break;
		cpu_relax();
// cpu_relax(): NVMe CQ 가 채워지기를 busy-wait
	} while (!need_resched());

	return 0;
}

/*
 * blk_mq_poll: cookie 에 해당하는 hctx 를 폴리.
 *   NVMe 관점: blk_poll -> blk_mq_poll -> blk_hctx_poll ->
 *   nvme_poll 순으로 CQ 를 폴리.
 */
int blk_mq_poll(struct request_queue *q, blk_qc_t cookie,
		struct io_comp_batch *iob, unsigned int flags)
{
	if (!blk_mq_can_poll(q))
// blk_mq_can_poll(): poll queue 지원 여부 확인
		return 0;
	return blk_hctx_poll(q, q->queue_hw_ctx[cookie], iob, flags);
}

/*
 * blk_rq_poll: 특정 request 의 poll hctx 를 폴리.
 *   NVMe 관점: REQ_POLLED 로 제출된 NVMe 명령의 CQ 항목을
 *   인터럽트 없이 직접 폴리.
 */
int blk_rq_poll(struct request *rq, struct io_comp_batch *iob,
		unsigned int poll_flags)
{
	struct request_queue *q = rq->q;
	int ret;

	if (!blk_rq_is_poll(rq))
// poll hctx 가 아니면 0 반환
		return 0;
	if (!percpu_ref_tryget(&q->q_usage_counter))
// percpu_ref_tryget(): NVMe queue 사용 중 poll 가능
		return 0;

	ret = blk_hctx_poll(q, rq->mq_hctx, iob, poll_flags);
	blk_queue_exit(q);

	return ret;
}
EXPORT_SYMBOL_GPL(blk_rq_poll);

unsigned int blk_mq_rq_cpu(struct request *rq)
{
	return rq->mq_ctx->cpu;
}
EXPORT_SYMBOL(blk_mq_rq_cpu);

void blk_mq_cancel_work_sync(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	cancel_delayed_work_sync(&q->requeue_work);
// requeue work 취소: NVMe SQ 재시도 work 중단

	queue_for_each_hw_ctx(q, hctx, i)
// 모든 hctx 의 run_work 취소: NVMe SQ dispatch 중단
		cancel_delayed_work_sync(&hctx->run_work);
}

/*
 * blk_mq_init: blk-mq 모듈 초기화.
 *   NVMe 관점: per-CPU 완료 리스트(blk_cpu_done) 와 BLOCK_SOFTIRQ
 *   를 등록. NVMe CQ 인터럽트가 아닌 CPU 에서의 완료 처리
 *   인프라를 준비.
 */
static int __init blk_mq_init(void)
{
	int i;

	for_each_possible_cpu(i)
// per-CPU blk_cpu_done 완료 리스트 초기화
		init_llist_head(&per_cpu(blk_cpu_done, i));
	for_each_possible_cpu(i)
		INIT_CSD(&per_cpu(blk_cpu_csd, i),
// per-CPU CSD 초기화: NVMe 완료 IPI 콜백 연결
			 __blk_mq_complete_request_remote, NULL);
	open_softirq(BLOCK_SOFTIRQ, blk_done_softirq);
// BLOCK_SOFTIRQ 등록: NVMe CQ 인터럽트 bottom-half 처리

	cpuhp_setup_state_nocalls(CPUHP_BLOCK_SOFTIRQ_DEAD,
				  "block/softirq:dead", NULL,
				  blk_softirq_cpu_dead);
	cpuhp_setup_state_multi(CPUHP_BLK_MQ_DEAD, "block/mq:dead", NULL,
				blk_mq_hctx_notify_dead);
	cpuhp_setup_state_multi(CPUHP_AP_BLK_MQ_ONLINE, "block/mq:online",
				blk_mq_hctx_notify_online,
				blk_mq_hctx_notify_offline);
	return 0;
}
subsys_initcall(blk_mq_init);

/*
 * NVMe 관점 핵심 요약
 * - blk-mq 의 request 는 NVMe 의 SQ slot 을 추상화하며, rq->tag 가 CID 역할을 한다.
 * - blk_mq_dispatch_rq_list() -> mq_ops->queue_rq() 가 NVMe 의 nvme_queue_rq
 *   로 연결되며, 실제 doorbell 기록은 드라이버 측에서 이루어진다.
 * - 완료 경로는 컨트롤러 CQ 인터럽트 -> nvme_irq -> blk_mq_complete_request()
 *   -> blk_mq_end_request() 순으로 흐른다.
 * - blk_mq_alloc_tag_set() / blk_mq_map_swqueue() 가 SQ/CQ 쌍과 CPU affinity
 *   를 초기화하는 기반이 된다.
 * - I/O scheduler 가 없으면 request 는 plug list 나 hctx->dispatch 를 거쳐
 *   직접 dispatch 되며, scheduler 사용 시에는 blk-mq-sched.c 로 위임된다.
 */
