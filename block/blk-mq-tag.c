// SPDX-License-Identifier: GPL-2.0
/*
 * Tag allocation using scalable bitmaps. Uses active queue tracking to support
 * fairer distribution of tags between multiple submitters when a shared tag map
 * is used.
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */

/*
 * ============================================================================
 * 파일 상단 요약 (NVMe SSD 관점)
 * ============================================================================
 * 본 파일(block/blk-mq-tag.c)은 blk-mq의 tag 할당/해제 핵심 엔진이다.
 * 요청이 하드웨어로 날아가기 전에 고유한 tag(CID와 1:1로 매핑됨)를
 * 할당하고, 완료 시 회수하는 역할을 수행한다.
 * NVMe 스택에서의 위치:
 *   blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_get_tag
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   -> NVMe SQ/CQ 완료 인터럽트 -> blk_mq_complete_request -> blk_mq_put_tag
 * tag는 NVMe의 CID(Command ID)가 될 수 있으며, SQ 엔트리 수, CQ 엔트리 수,
 * 그리고 동시 진행 가능한 IO 수를 제한하는 핵심 자원이다.
 *
 
 관련
  파일: block/blk-mq.h(선언), block/blk-mq-tag.c(구현),
 *           drivers/nvme/host/pci.c(NVMe 드라이버 rq 처리)
 * ============================================================================
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/kmemleak.h>

#include <linux/delay.h>
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"

/*
 * struct blk_mq_tags 주요 필드의 NVMe 의미 (include/linux/blk-mq.h에 정의):
 *   - nr_tags          : 전체 tag 수. NVMe SQ의 최대 엔트리 수(queue depth)와
 *                        대응되며, 동시에 진행 가능한 CID의 총 개수를 의미한다.
 *   - nr_reserved_tags : 예약 tag 수. NVMe admin/flush 등 긴급 명령이 starving
 *                        되지 않도록 예약된 CID 영역이다.
 *   - active_queues    : 현재 활성 상태인 hardware queue 수. SQ 사용률에 따라
 *                        wakeup batch를 재조정하여 공정한 tag 분배를 지원한다.
 *   - bitmap_tags      : 일반 요청용 sbitmap. NVMe SQ의 일반 CID pool이다.
 *   - breserved_tags   : 예약 요청용 sbitmap. NVMe SQ의 예약 CID pool이다.
 *   - rqs[tag]         : tag에 매핑된 struct request 포인터. CID -> request
 *                        역조회 및 CQ 완료 시 해당 request를 찾는 데 사용된다.
 *   - static_rqs[tag]  : 정적으로 할당된 request. NVMe reset/recovery 후에도
 *                        안정적으로 참조할 수 있는 기본 request 배열이다.
 *   - lock             : active_queues/wake_batch 갱신 시 사용. SQ 상태 동기화.
 * ============================================================================
 */

/*
 * Recalculate wakeup batch when tag is shared by hctx.
 */
/*
 * blk_mq_update_wake_batch()
 * 목적: tag가 여러 blk_mq_hw_ctx 간에 공유될 때 wakeup batch를 재계산한다.
 * 호출 경로:
 *   __blk_mq_tag_busy -> blk_mq_update_wake_batch
 *   __blk_mq_tag_idle -> blk_mq_update_wake_batch
 * NVMe 연결:
 *   - NVMe controller가 여러 queue를 공유하는 상황(예: namespace 간 shared tag)
 *     에서 공정한 tag 분배를 위해 사용된다.
 *   - users가 늘어나면 각 submitter가 깨어나는 단위(batch)를 조정하여
 *     thundering herd를 완화한다(추정).
 */
static void blk_mq_update_wake_batch(struct blk_mq_tags *tags,
		unsigned int users)
{
	if (!users)
		return;		/* active_queues가 0이면 wake_batch 재계산 불필요 -> NVMe queue pair 전원이 꺼진 상태(추정) */

	sbitmap_queue_recalculate_wake_batch(&tags->bitmap_tags,
			users);	/* NVMe 일반 CID pool의 wakeup batch 조정: users 증가 시 thundering herd 완화(추정) */
	sbitmap_queue_recalculate_wake_batch(&tags->breserved_tags,
			users);	/* NVMe 예약 CID pool(flush/admin 등)의 wakeup batch도 동일하게 조정 */
}

/*
 * If a previously inactive queue goes active, bump the active user count.
 * We need to do this before try to allocate driver tag, then even if fail
 * to get tag when first time, the other shared-tag users could reserve
 * budget for it.
 */
/*
 * __blk_mq_tag_busy()
 * 목적: 특정 blk_mq_hw_ctx가 다시 IO를 제출하기 시작하면 active user count를
 *       증가시킨다.
 * 호출 경로:
 *   blk_mq_tag_busy(hctx) -> __blk_mq_tag_busy(hctx)
 *   -> blk_mq_get_tag() 낙관적 경로 또는 dispatch 시작 시
 * NVMe 연결:
 *   - nvme_queue가 busy 상태로 전환되면 해당 hctx의 tag 예산을 공유 풀에서
 *     확보하기 위해 active_queues를 늘린다.
 *   - BLK_MQ_F_TAG_QUEUE_SHARED 플래그가 설정된 NVMe 장치에서 여러 queue가
 *     하나의 tag pool을 공유할 때 공정 분배의 기초가 된다.
 */
void __blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	unsigned int users;
	unsigned long flags;
	struct blk_mq_tags *tags = hctx->tags;	/* 이 hctx의 tag pool -> NVMe SQ의 CID pool(또는 shared pool) */

	/* 캐시라인 오염을 막기 위해 이미 active인지 먼저 확인한다. */
	/*
	 * calling test_bit() prior to test_and_set_bit() is intentional,
	 * it avoids dirtying the cacheline if the queue is already active.
	 */
	if (blk_mq_is_shared_tags(hctx->flags)) {
		struct request_queue *q = hctx->queue;

		/* shared tag pool 사용 시 queue 단위 active 플래그를 검사/설정 -> 여러 NVMe namespace가 하나의 CID pool을 공유할 때 경쟁 조건 회피 */
		if (test_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags) ||
		    test_and_set_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags))
			return;
	} else {
		/* per-hctx tag pool 사용 시 hctx->state의 BLK_MQ_S_TAG_ACTIVE 비트 검사/설정 -> NVMe queue pair 단위 활성화 추적 */
		if (test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state) ||
		    test_and_set_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return;
	}

	spin_lock_irqsave(&tags->lock, flags);	/* active_queues/wake_batch 동시 갱신 보호 -> NVMe SQ 예산 분배의 일관성 유지 */
	users = tags->active_queues + 1;	/* 새로운 active NVMe queue(hctx)를 공유 풀의 user 카운트에 추가 */
	WRITE_ONCE(tags->active_queues, users);	/* 컴파일러/CPU 재배치 방지: 다른 hctx에서 읽는 active_queues 값이 일관되게 보이도록(추정) */
	blk_mq_update_wake_batch(tags, users);	/* active queue 수 변화에 따라 wakeup batch 재조정 -> 공정한 NVMe CID 분배 */
	spin_unlock_irqrestore(&tags->lock, flags);
}

/*
 * Wakeup all potentially sleeping on tags
 */
/*
 * blk_mq_tag_wakeup_all()
 * 목적: tag를 기다리며 잠든 모든 waiter를 깨운다.
 * 호출 경로:
 *   __blk_mq_tag_idle -> blk_mq_tag_wakeup_all
 *   blk_mq_tag_resize_shared_tags 등 tag 가용성 변화 시
 * NVMe 연결:
 *   - NVMe SQ가 가득 차서 IO submit이 차단되었을 때, 완료가 일어나 tag가
 *     회수되면 대기 중인 submitter를 깨워 다음 doorbell 기회를 준다.
 *   - include_reserve가 true이면 예약된 tag 영역도 깨운다.
 */
void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool include_reserve)
{
	sbitmap_queue_wake_all(&tags->bitmap_tags);	/* NVMe 일반 CID pool에서 대기 중인 submitter 깨움 -> SQ slot이 생겼으므로 doorbell 기회 제공 */
	if (include_reserve)
		sbitmap_queue_wake_all(&tags->breserved_tags);	/* 필요 시 NVMe 예약 CID(flush/admin) waiter도 깨움 */
}

/*
 * If a previously busy queue goes inactive, potential waiters could now
 * be allowed to queue. Wake them up and check.
 */
/*
 * __blk_mq_tag_idle()
 * 목적: 특정 blk_mq_hw_ctx가 일정 시간 IO를 제출하지 않으면 active user
 *       count를 감소시키고, 대기 중인 다른 submitter에게 기회를 넘긴다.
 * 호출 경로:
 *   blk_mq_tag_idle(hctx) -> __blk_mq_tag_idle(hctx)
 * NVMe 연결:
 *   - nvme_queue가 idle 상태가 되면 공유 tag pool에서 사용 중이던 예산을
 *     반납하여 다른 nvme_queue가 더 많은 CID를 할당받을 수 있게 한다.
 *   - active_queues가 감소하면 blk_mq_update_wake_batch에서 wakeup batch가
 *     다시 조정된다.
 */
void __blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;	/* 이 hctx가 사용하는 tag pool -> NVMe SQ의 CID pool */
	unsigned int users;

	if (blk_mq_is_shared_tags(hctx->flags)) {
		struct request_queue *q = hctx->queue;

		/* shared tag pool: queue의 HCTX_ACTIVE 비트를 원자적으로 클리어 -> NVMe queue pair가 idle로 전환 */
		if (!test_and_clear_bit(QUEUE_FLAG_HCTX_ACTIVE,
					&q->queue_flags))
			return;
	} else {
		/* per-hctx tag pool: hctx->state의 TAG_ACTIVE 비트 클리어 -> 해당 NVMe queue pair의 CID 사용 중단 */
		if (!test_and_clear_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return;
	}

	spin_lock_irq(&tags->lock);	/* active_queues/wake_batch 갱신 보호 */
	users = tags->active_queues - 1;	/* idle로 전환된 NVMe queue를 공유 풀 user 카운트에서 제거 */
	WRITE_ONCE(tags->active_queues, users);	/* 컴파일러/CPU 재배치 방지: 다른 CPU에서 일관된 active_queues 값 관측(추정) */
	blk_mq_update_wake_batch(tags, users);	/* user 수 감소에 따른 wakeup batch 재조정 -> 남은 NVMe queue가 더 많은 CID 사용 가능 */
	spin_unlock_irq(&tags->lock);

	blk_mq_tag_wakeup_all(tags, false);	/* 예약 tag 영역 제외하고 대기 중인 submitter 깨움 -> NVMe SQ slot 회수 후 재분배 */
}

/*
 * __blk_mq_get_tag()
 * 목적: sbitmap_queue에서 하나의 tag를 즉시 할당받는다.
 * 호출 경로:
 *   blk_mq_get_tag -> __blk_mq_get_tag
 * NVMe 연결:
 *   - CID 하나를 NVMe SQ에서 빈 slot으로 할당하는 것과 대응한다.
 *   - hctx_may_queue()는 공유 tag pool에서 이 nvme_queue가 더 많은 tag를
 *     가져가도 되는지를 판단한다(공정 분배).
 *   - BLK_MQ_REQ_RESERVED 플래그가 있으면 예약된 tag 영역에서 할당한다.
 */
static int __blk_mq_get_tag(struct blk_mq_alloc_data *data,
			    struct sbitmap_queue *bt)
{
	/* elevator가 없고 예약 tag가 아니며 공유 풀의 허용량이 초과되면 실패 */
	if (!data->q->elevator && !(data->flags & BLK_MQ_REQ_RESERVED) &&
			!hctx_may_queue(data->hctx, bt))	/* 공유 NVMe CID pool에서 이 hctx의 quota 초과 여부 판단 -> 공정 분배 */
		return BLK_MQ_NO_TAG;	/* NVMe SQ에 빈 slot이 없거나 quota 초과: CID 할당 실패, dispatch 재시도 또는 대기 */

	/* shallow_depth가 지정되면 NVMe SQ의 남은 공간 등을 고려해 얕게 할당 */
	if (data->shallow_depth)
		return sbitmap_queue_get_shallow(bt, data->shallow_depth);	/* NVMe SQ tail/head 간격 등으로 인한 임시 queue depth 제한(추정) */
	else
		return __sbitmap_queue_get(bt);	/* NVMe CID pool에서 하나의 빈 bit(tag)를 원자적으로 획득 */
}

/*
 * blk_mq_get_tags()
 * 목적: 여러 tag를 한 번에 batch로 할당한다.
 * 호출 경로:
 *   (blk-mq 낮부 다중 요청 할당 경로, 예: plug merge 후 일괄 할당)
 *   -> __sbitmap_queue_get_batch
 * NVMe 연결:
 *   - 여러 CID를 한 번에 예약하여 NVMe SQ 엔트리를 연속적으로 채울 때
 *     유리하다(추정).
 *   - 공유 tag pool이거나 예약 tag, shallow_depth가 설정된 경우에는
 *     batch 할당을 하지 않고 0을 반환하여 개별 할당 경로로 돌아간다.
 */
unsigned long blk_mq_get_tags(struct blk_mq_alloc_data *data, int nr_tags,
			      unsigned int *offset)
{
	struct blk_mq_tags *tags = blk_mq_tags_from_data(data);	/* CID pool이 속한 blk_mq_tags 획득 */
	struct sbitmap_queue *bt = &tags->bitmap_tags;	/* NVMe 일반 CID pool */
	unsigned long ret;

	/* shallow_depth/예약 tag/공유 tag pool인 경우 batch 경로 사용 불가 -> 개별 CID 할당으로 fallback */
	if (data->shallow_depth ||data->flags & BLK_MQ_REQ_RESERVED ||
	    data->hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		return 0;
	ret = __sbitmap_queue_get_batch(bt, nr_tags, offset);	/* NVMe SQ에 nr_tags개의 연속/비연속 CID를 일괄 할당(추정) */
	*offset += tags->nr_reserved_tags;	/* 예약 tag 영역 뒤에 일반 CID 영역이 위치하므로 offset 보정 */
	return ret;	/* 실제 할당된 tag 개수; 0이면 개별 blk_mq_get_tag 경로로 대체(추정) */
}

/*
 * blk_mq_get_tag()
 * 목적: blk-mq tag 할당의 핵심 진입점. tag가 없으면 IO 완료를 기다리며
 *       잠들 수 있다.
 * 호출 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_get_tag
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * NVMe 연결:
 *   - 이 함수가 반환하는 tag 값은 NVMe 드라이버에서 request->tag로 저장되고,
 *     이후 nvme_submit_cmd에서 CID로 사용된다.
 *   - NVMe SQ가 가득 차면 tag 할당이 실패하고, blk_mq_run_hw_queue()를
 *     통해 dispatch를 재시도하거나 io_schedule()로 대기한다.
 *   - 예약 tag 영역(breserved_tags)은 플러시/예약 명령 등에 사용된다.
 */
unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data)
{
	struct blk_mq_tags *tags = blk_mq_tags_from_data(data);	/* CID pool 획득: per-hctx 또는 shared */
	struct sbitmap_queue *bt;	/* 선택된 NVMe CID pool(일반/예약) */
	struct sbq_wait_state *ws;	/* sbitmap queue의 wait state -> CID 대기 큐 */
	DEFINE_SBQ_WAIT(wait);		/* tag(CID)를 기다리는 waiter 구조체 초기화 */
	unsigned int tag_offset;	/* 예약 tag 영역 크기만큼의 오프셋 -> NVMe CID 0은 예약 영역 기준 */
	int tag;			/* 할당받은 tag 값 -> NVMe CID 후보 */

	/* 예약 tag 영역인지 일반 tag 영역인지 선택한다. */
	if (data->flags & BLK_MQ_REQ_RESERVED) {
		if (unlikely(!tags->nr_reserved_tags)) {
			WARN_ON_ONCE(1);	/* 예약 tag가 없는데 BLK_MQ_REQ_RESERVED 요청이 들어온 버그 */
			return BLK_MQ_NO_TAG;	/* NVMe admin/flush 등 예약 CID가 없으면 할당 불가 */
		}
		bt = &tags->breserved_tags;	/* NVMe 예약 CID pool 선택(flush, admin command 등) */
		tag_offset = 0;			/* 예약 영역은 offset 0부터 시작 */
	} else {
		bt = &tags->bitmap_tags;	/* NVMe 일반 IO CID pool 선택 */
		tag_offset = tags->nr_reserved_tags;	/* 일반 CID는 예약 영역 다음 번호부터 시작 */
	}

	tag = __blk_mq_get_tag(data, bt);	/* NVMe CID 하나 즉시 할당 시도 */
	if (tag != BLK_MQ_NO_TAG)
		goto found_tag;			/* SQ slot 획득 성공 -> doorbell 단계로 진행 가능 */

	/* NOWAIT 요청은 즉시 실패를 반환한다. */
	if (data->flags & BLK_MQ_REQ_NOWAIT)
		return BLK_MQ_NO_TAG;		/* NVMe poll/제한 경로: SQ 가득 참, 상위에서 재시도 또는 -EAGAIN(추정) */

	ws = bt_wait_ptr(bt, data->hctx);	/* 이 hctx가 사용할 sbitmap wait state 획득 -> NVMe queue별 대기 큐 */
	do {
		struct sbitmap_queue *bt_prev;

		/*
		 * We're out of tags on this hardware queue, kick any
		 * pending IO submits before going to sleep waiting for
		 * some to complete.
		 */
		/* tag가 없으면 하드웨어 큐를 구동해 완료 가능성을 높인다. */
		blk_mq_run_hw_queue(data->hctx, false);	/* NVMe SQ에 쌓인 요청을 doorbell로 전달 -> CQ 완료 유도, CID 회수 가능성 증가 */

		/*
		 * Retry tag allocation after running the hardware queue,
		 * as running the queue may also have found completions.
		 */
		tag = __blk_mq_get_tag(data, bt);	/* doorbell 후 NVMe CQ 완료로 회수된 CID가 있으면 재할당 */
		if (tag != BLK_MQ_NO_TAG)
			break;

		sbitmap_prepare_to_wait(bt, ws, &wait, TASK_UNINTERRUPTIBLE);	/* CID 대기 상태로 진입 준비 */

		tag = __blk_mq_get_tag(data, bt);	/* sleep 직전 마지막 CID 재확인 -> race 방지 */
		if (tag != BLK_MQ_NO_TAG)
			break;

		bt_prev = bt;				/* hctx 변경 시 이전 queue를 깨우기 위해 보관 */
		io_schedule();				/* NVMe SQ가 꽉 차서 CID가 날 때까지 대기 -> 다른 태스크 CPU 양보 */

		sbitmap_finish_wait(bt, ws, &wait);	/* wait state 정리 */

		/* IO를 기다리는 동안 CPU가 옮겨졌을 수 있으므로, 다시 현재 CPU의 software queue와 hardware queue를 찾는다. */
		data->ctx = blk_mq_get_ctx(data->q);		/* 현재 CPU의 software queue 컨텍스트 재획득 -> NVMe SQ 선택에 영향 */
		data->hctx = blk_mq_map_queue(data->cmd_flags, data->ctx);	/* CPU/irq affinity에 따른 NVMe hctx 재매핑 */
		tags = blk_mq_tags_from_data(data);		/* 새 hctx에 해당하는 CID pool 재탐색 */
		if (data->flags & BLK_MQ_REQ_RESERVED)
			bt = &tags->breserved_tags;		/* 예약 CID pool로 전환 */
		else
			bt = &tags->bitmap_tags;		/* 일반 CID pool로 전환 */

		/*
		 * If destination hw queue is changed, fake wake up on
		 * previous queue for compensating the wake up miss, so
		 * other allocations on previous queue won't be starved.
		 */
		/* hctx가 바뀌어 이전 큐를 떠날 때, 이전 큐의 waiter가 깨어나도록 가짜 wake_up을 한 번 본다. */
		if (bt != bt_prev)
			sbitmap_queue_wake_up(bt_prev, 1);	/* 이전 NVMe queue의 waiter에게 CID 회수 이벤트 전파 -> starvation 방지 */

		ws = bt_wait_ptr(bt, data->hctx);		/* 새 NVMe queue의 sbitmap wait state 획득 */
	} while (1);

	sbitmap_finish_wait(bt, ws, &wait);			/* 최종 wait state 정리 */

found_tag:
	/*
	 * Give up this allocation if the hctx is inactive.  The caller will
	 * retry on an active hctx.
	 */
	/* hctx가 비활성화되었다면 할당받은 tag를 반납하고 재시도를 요청한다. */
	if (unlikely(test_bit(BLK_MQ_S_INACTIVE, &data->hctx->state))) {	/* NVMe queue pair가 제거/비활성화된 상태면 */
		blk_mq_put_tag(tags, data->ctx, tag + tag_offset);		/* 획득한 CID를 즉시 반납 -> SQ slot 누수 방지 */
		return BLK_MQ_NO_TAG;						/* 활성 hctx에서 재시도 요청 */
	}
	return tag + tag_offset;	/* 최종 NVMe CID = local tag + 예약 영역 offset */
}

/*
 * blk_mq_put_tag()
 * 목적: 사용이 끝난 tag를 sbitmap_queue에 반납한다.
 * 호출 경로:
 *   NVMe 완료 인터럽트 -> nvme_complete_rq -> blk_mq_complete_request
 *   -> __blk_mq_end_request -> blk_mq_put_driver_tag -> blk_mq_put_tag
 * NVMe 연결:
 *   - NVMe CQ가 명령을 완료 보고하면 해당 CID(tag)를 회수하여 이후
 *     nvme_submit_cmd에서 재사용할 수 있게 한다.
 *   - reserved tag인지 아닌지에 따라 서로 다른 sbitmap_queue에 반납한다.
 */
void blk_mq_put_tag(struct blk_mq_tags *tags, struct blk_mq_ctx *ctx,
		    unsigned int tag)
{
	if (!blk_mq_tag_is_reserved(tags, tag)) {	/* 일반 NVMe CID인지 확인 */
		const int real_tag = tag - tags->nr_reserved_tags;	/* 예약 영역 offset 제거 -> bitmap_tags 기준 local CID */

		BUG_ON(real_tag >= tags->nr_tags);	/* local CID가 일반 pool 범위를 벗어나면 치명적 버그 -> NVMe CID 손상 */
		sbitmap_queue_clear(&tags->bitmap_tags, real_tag, ctx->cpu);	/* NVMe 일반 CID pool에 bit 반납 -> 완료된 SQ slot 재사용 가능 */
	} else {
		sbitmap_queue_clear(&tags->breserved_tags, tag, ctx->cpu);	/* NVMe 예약 CID pool에 bit 반납 -> flush/admin slot 재사용 */
	}
}

/*
 * blk_mq_put_tags()
 * 목적: 여러 tag를 batch로 반납한다.
 * 호출 경로:
 *   다중 요청 일괄 완료 처리 경로
 * NVMe 연결:
 *   - NVMe CQ에서 여러 완료 엔트리를 한 번에 처리한 후, 해당 CID들을
 *     일괄 회수할 때 사용된다(추정).
 */
void blk_mq_put_tags(struct blk_mq_tags *tags, int *tag_array, int nr_tags)
{
	sbitmap_queue_clear_batch(&tags->bitmap_tags, tags->nr_reserved_tags,
					tag_array, nr_tags);	/* NVMe CQ 일괄 완료 후 여러 CID를 한 번에 bitmap에 반납 -> cache 효율 및 doorbell 지연 최소화(추정) */
}

/*
 * struct bt_iter_data
 * NVMe 관련 설명:
 *   - hctx: 현재 살펴 볼 NVMe queue에 해당하는 blk_mq_hw_ctx.
 *   - q: request_queue. NVMe namespace의 request_queue와 대응한다.
 *   - fn: 각 tag에 대해 호출할 콜백. 예를 들어 abort, timeout, reset 처리.
 *   - data: 콜백에 전달할 private 데이터.
 *   - reserved: true이면 breserved_tags(예약 tag)를 순회한다.
 *               NVMe 플러시나 관리 명령이 사용할 수 있는 예약 CID 영역.
 */
struct bt_iter_data {
	struct blk_mq_hw_ctx *hctx;
	struct request_queue *q;
	busy_tag_iter_fn *fn;
	void *data;
	bool reserved;
};

/*
 * blk_mq_find_and_get_req()
 * 목적: tag 번호로 tags->rqs[]에서 request 포인터를 찾고 reference count를
 *       증가시킨다.
 * 호출 경로:
 *   bt_iter -> blk_mq_find_and_get_req
 *   bt_tags_iter -> blk_mq_find_and_get_req
 * NVMe 연결:
 *   - NVMe CID(tag)로 해당하는 struct request를 찾아 timeout이나 abort
 *     처리를 준비한다.
 *   - rq->tag가 bitnr와 다르거나 reference를 잡을 수 없으면 NULL을 반환한다.
 */
static struct request *blk_mq_find_and_get_req(struct blk_mq_tags *tags,
		unsigned int bitnr)
{
	struct request *rq;

	rq = tags->rqs[bitnr];			/* tag(CID)로 request 배열 역조회 -> NVMe CQ 완료 엔트리의 CID에 해당하는 request 검색 */
	if (!rq || rq->tag != bitnr || !req_ref_inc_not_zero(rq))	/* race/재사용으로 tag 불일치 또는 reference 획득 실패 */
		rq = NULL;				/* 유효하지 않은 NVMe CID -> skip 또는 abort 대상이 아님 */
	return rq;					/* CID에 대응하는 struct request 포인터; timeout/abort/complete 처리에 사용 */
}

/*
 * bt_iter()
 * 목적: sbitmap의 각 set bit에 대해 대응하는 request를 찾아 사용자 콜백 fn을
 *       호출한다.
 * 호출 경로:
 *   blk_mq_queue_tag_busy_iter -> bt_for_each -> sbitmap_for_each_set -> bt_iter
 * NVMe 연결:
 *   - NVMe timeout, reset, abort 처리에서 현재 진행 중인 모든 request를
 *     순회할 때 사용된다.
 *   - shared_tags 모드에서는 q->tag_set->shared_tags를 사용하여 모든 NVMe
 *     namespace의 request를 한 번에 본다.
 */
static bool bt_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_iter_data *iter_data = data;
	struct blk_mq_hw_ctx *hctx = iter_data->hctx;	/* 대상 NVMe queue(hctx) */
	struct request_queue *q = iter_data->q;		/* NVMe namespace의 request_queue */
	struct blk_mq_tag_set *set = q->tag_set;	/* tag set -> NVMe controller의 queue pair 집합 */
	struct blk_mq_tags *tags;
	struct request *rq;
	bool ret = true;

	if (blk_mq_is_shared_tags(set->flags))
		tags = set->shared_tags;		/* shared tag pool: 여러 NVMe namespace/queue가 공유하는 CID pool */
	else
		tags = hctx->tags;			/* per-hctx tag pool: 특정 NVMe queue pair의 CID pool */

	if (!iter_data->reserved)
		bitnr += tags->nr_reserved_tags;	/* 일반 tag 순회 시 예약 영역 offset 추가 -> 실제 NVMe CID 계산 */
	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assigning ->rqs[].
	 */
	/* tag 할당은 sbitmap의 bit를 먼저 세팅한 후 rqs[]에 request를 기록하기 때문에 race로 rq가 NULL일 수 있다. */
	rq = blk_mq_find_and_get_req(tags, bitnr);	/* set된 bit(CID)에 대응하는 request 획득 및 reference 증가 */
	if (!rq)
		return true;				/* 유효한 request 없음 -> 다음 NVMe CID로 진행 */

	if (rq->q == q && (!hctx || rq->mq_hctx == hctx))	/* 동일 NVMe namespace이고 지정 hctx에 속하면 */
		ret = iter_data->fn(rq, iter_data->data);	/* timeout/abort/complete 콜백 호출 -> NVMe 명령별 후속 처리 */
	blk_mq_put_rq_ref(rq);				/* 앞서 증가시킨 request reference 해제 */
	return ret;					/* true면 계속 순회, false면 중단 -> NVMe 전체 스캔 제어 */
}

/**
 * bt_for_each - iterate over the requests associated with a hardware queue
 * @hctx:	Hardware queue to examine.
 * @q:		Request queue @hctx is associated with (@hctx->queue).
 * @bt:		sbitmap to examine. This is either the breserved_tags member
 *		or the bitmap_tags member of struct blk_mq_tags.
 * @fn:		Pointer to the function that will be called for each request
 *		associated with @hctx that has been assigned a driver tag.
 *		@fn will be called as follows: @fn(rq, @data) where rq is a
 *		pointer to a request. Return %true to continue iterating tags;
 *		%false to stop.
 * @data:	Will be passed as second argument to @fn.
 * @reserved:	Indicates whether @bt is the breserved_tags member or the
 *		bitmap_tags member of struct blk_mq_tags.
 */
/*
 * bt_for_each()
 * 목적: 주어진 sbitmap에 set된 bit들을 순회하며 bt_iter를 호출한다.
 * 호출 경로:
 *   blk_mq_queue_tag_busy_iter -> bt_for_each
 * NVMe 연결:
 *   - 특정 nvme_queue(hctx)에서 현재 사용 중인 tag(CID)와 연결된 request를
 *     모두 순회한다.
 *   - timeout 처리, controller reset, error recovery 등에 활용된다.
 */
static void bt_for_each(struct blk_mq_hw_ctx *hctx, struct request_queue *q,
			struct sbitmap_queue *bt, busy_tag_iter_fn *fn,
			void *data, bool reserved)
{
	struct bt_iter_data iter_data = {
		.hctx = hctx,		/* NVMe queue pair(hctx) */
		.fn = fn,		/* timeout/abort/complete 콜백 */
		.data = data,		/* 콜백 private(예: nvme_timeout의 상태) */
		.reserved = reserved,	/* 예약 CID 영역 여부 */
		.q = q,			/* NVMe namespace request_queue */
	};

	sbitmap_for_each_set(&bt->sb, bt_iter, &iter_data);	/* NVMe CID bitmap의 set bit를 하나씩 순회 -> 진행 중인 명령 전체 스캔 */
}

/*
 * struct bt_tags_iter_data
 * NVMe 관련 설명:
 *   - tags: 직접 지정된 blk_mq_tags. NVMe의 shared tag pool이거나
 *           특정 hctx의 tag pool이다.
 *   - fn: 각 request에 대해 호출할 콜백.
 *   - data: 콜백 private 데이터.
 *   - flags: BT_TAG_ITER_* 플래그 조합.
 *       BT_TAG_ITER_RESERVED: 예약 tag 영역 순회
 *       BT_TAG_ITER_STARTED: started 상태의 request만 순회
 *       BT_TAG_ITER_STATIC_RQS: static_rqs[]에서 request를 가져옴
 */
struct bt_tags_iter_data {
	struct blk_mq_tags *tags;
	busy_tag_iter_fn *fn;
	void *data;
	unsigned int flags;
};

#define BT_TAG_ITER_RESERVED		(1 << 0)
#define BT_TAG_ITER_STARTED		(1 << 1)
#define BT_TAG_ITER_STATIC_RQS		(1 << 2)

/*
 * bt_tags_iter()
 * 목적: tag map 전체를 순회하며 조건에 맞는 request에 콜백을 호출한다.
 * 호출 경로:
 *   __blk_mq_all_tag_iter -> bt_tags_for_each -> sbitmap_for_each_set -> bt_tags_iter
 * NVMe 연결:
 *   - NVMe controller reset 시 모든 진행 중인 IO를 한 번에 스캔한다.
 *   - static_rqs는 초기에 미리 할당된 request pool을 의미하며, tags가
 *     아직 완전히 초기화되지 않은 상태에서도 안전하게 순회할 수 있다.
 */
static bool bt_tags_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_tags_iter_data *iter_data = data;
	struct blk_mq_tags *tags = iter_data->tags;	/* CID pool */
	struct request *rq;
	bool ret = true;
	bool iter_static_rqs = !!(iter_data->flags & BT_TAG_ITER_STATIC_RQS);	/* static_rqs[] 사용 여부 -> NVMe reset 중에도 안전한 순회 */

	if (!(iter_data->flags & BT_TAG_ITER_RESERVED))
		bitnr += tags->nr_reserved_tags;	/* 일반 CID 영역 순회 시 예약 영역 offset 적용 */

	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assigning ->rqs[].
	 */
	if (iter_static_rqs)
		rq = tags->static_rqs[bitnr];		/* 초기 정적 request pool에서 직접 접근 -> NVMe controller 초기화/복구 단계에서 참조 안전 */
	else
		/* tag 할당은 sbitmap의 bit를 먼저 세팅한 후 rqs[]에 request를 기록하기 때문에 race로 rq가 NULL일 수 있다. */
		rq = blk_mq_find_and_get_req(tags, bitnr);	/* 동적 request 배열에서 CID에 해당하는 request 획득 */
	if (!rq)
		return true;				/* 유효하지 않은 NVMe CID -> 다음으로 진행 */

	if (!(iter_data->flags & BT_TAG_ITER_STARTED) ||
	    blk_mq_request_started(rq))			/* started 필터가 없거나, 실제로 NVMe에 제출된 명령이면 */
		ret = iter_data->fn(rq, iter_data->data);	/* timeout/abort/complete 콜백 실행 */
	if (!iter_static_rqs)
		blk_mq_put_rq_ref(rq);			/* reference 해제 */
	return ret;					/* true: 다음 NVMe CID, false: 순회 중단 */
}

/**
 * bt_tags_for_each - iterate over the requests in a tag map
 * @tags:	Tag map to iterate over.
 * @bt:		sbitmap to examine. This is either the breserved_tags member
 *		or the bitmap_tags member of struct blk_mq_tags.
 * @fn:		Pointer to the function that will be called for each started
 *		request. @fn will be called as follows: @fn(rq, @data) where rq
 *		is a pointer to a request. Return %true to continue iterating
 *		tags; %false to stop.
 * @data:	Will be passed as second argument to @fn.
 * @flags:	BT_TAG_ITER_*
 */
/*
 * bt_tags_for_each()
 * 목적: 특정 tag map(bitmap_tags 또는 breserved_tags)을 순회한다.
 * 호출 경로:
 *   __blk_mq_all_tag_iter -> bt_tags_for_each
 * NVMe 연결:
 *   - NVMe 장치의 전체 CID 공간 또는 예약 CID 공간을 순회하며, 진행 중인
 *     명령들에 대해 콜백 기반 작업(timeout, abort, complete)을 수행한다.
 */
static void bt_tags_for_each(struct blk_mq_tags *tags, struct sbitmap_queue *bt,
			     busy_tag_iter_fn *fn, void *data, unsigned int flags)
{
	struct bt_tags_iter_data iter_data = {
		.tags = tags,		/* CID pool */
		.fn = fn,		/* NVMe 명령별 콜백 */
		.data = data,		/* 콜백 private */
		.flags = flags,	/* BT_TAG_ITER_*: reserved/started/static_rqs */
	};

	if (tags->rqs)							/* 동적 request 배열이 초기화되어 있을 때만 순회 -> NVMe 정상 운영 중 */
		sbitmap_for_each_set(&bt->sb, bt_tags_iter, &iter_data);	/* NVMe CID bitmap의 set bit별로 콜백 호출 */
}

/*
 * __blk_mq_all_tag_iter()
 * 목적: 예약 tag 영역과 일반 tag 영역을 모두 순회한다.
 * 호출 경로:
 *   blk_mq_all_tag_iter -> __blk_mq_all_tag_iter
 *   blk_mq_tagset_busy_iter -> __blk_mq_all_tag_iter
 * NVMe 연결:
 *   - NVMe의 전체 CID 범위(예약 CID + 일반 CID)를 커버하여, 어떤 명령도
 *     누락되지 않도록 처리한다.
 */
static void __blk_mq_all_tag_iter(struct blk_mq_tags *tags,
		busy_tag_iter_fn *fn, void *priv, unsigned int flags)
{
	WARN_ON_ONCE(flags & BT_TAG_ITER_RESERVED);	/* 상위에서 RESERVED 플래그를 중복 지정하면 버그 */

	if (tags->nr_reserved_tags)
		bt_tags_for_each(tags, &tags->breserved_tags, fn, priv,
				 flags | BT_TAG_ITER_RESERVED);	/* NVMe 예약 CID 영역(flush/admin) 순회 */
	bt_tags_for_each(tags, &tags->bitmap_tags, fn, priv, flags);	/* NVMe 일반 IO CID 영역 순회 -> 전체 CID 공간 커버 */
}

/**
 * blk_mq_all_tag_iter - iterate over all requests in a tag map
 * @tags:	Tag map to iterate over.
 * @fn:		Pointer to the function that will be called for each
 *		request. @fn will be called as follows: @fn(rq, @priv) where rq
 *		is a pointer to a request. Return %true to continue iterating
 *		tags; %false to stop.
 * @priv:	Will be passed as second argument to @fn.
 *
 * Caller has to pass the tag map from which requests are allocated.
 */
/*
 * blk_mq_all_tag_iter()
 * 목적: 특정 tag map의 모든 request를 순회한다(static_rqs 포함).
 * 호출 경로:
 *   (낮부 디버깅/복구 경로) -> blk_mq_all_tag_iter
 * NVMe 연결:
 *   - NVMe 드라이버가 장치 상태를 진단할 때, 특정 queue의 전체 request를
 *     스캔하는 데 사용된다(추정).
 */
void blk_mq_all_tag_iter(struct blk_mq_tags *tags, busy_tag_iter_fn *fn,
		void *priv)
{
	__blk_mq_all_tag_iter(tags, fn, priv, BT_TAG_ITER_STATIC_RQS);	/* static_rqs 포함: NVMe 초기화/reset 단계에서도 안전하게 전체 스캔 */
}

/**
 * blk_mq_tagset_busy_iter - iterate over all started requests in a tag set
 * @tagset:	Tag set to iterate over.
 * @fn:		Pointer to the function that will be called for each started
 *		request. @fn will be called as follows: @fn(rq, @priv) where
 *		rq is a pointer to a request. Return true to continue iterating
 *		tags, false to stop.
 * @priv:	Will be passed as second argument to @fn.
 *
 * We grab one request reference before calling @fn and release it after
 * @fn returns.
 */
/*
 * blk_mq_tagset_busy_iter()
 * 목적: 전체 tag set에서 started 상태의 모든 request를 순회한다.
 * 호출 경로:
 *   nvme_timeout, nvme_reset_work 등에서 호출(추정)
 *   -> blk_mq_tagset_busy_iter
 * NVMe 연결:
 *   - NVMe controller 전체 또는 하나의 queue pair에 속한 모든 진행 중인
 *     명령을 대상으로 timeout, abort, complete 처리를 한다.
 *   - shared_tags 모드에서는 tagset 전체를 한 번에 커버한다.
 */
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *tagset,
		busy_tag_iter_fn *fn, void *priv)
{
	unsigned int flags = tagset->flags;		/* tag set 플래그 -> shared tag pool 사용 여부 */
	int i, nr_tags, srcu_idx;

	srcu_idx = srcu_read_lock(&tagset->tags_srcu);	/* tags[] 배열의 RCU 보호: NVMe queue pair 추가/제거와 race 방지 */

	nr_tags = blk_mq_is_shared_tags(flags) ? 1 : tagset->nr_hw_queues;	/* shared pool이면 1번만, 아니면 NVMe queue pair 수만큼 반복 */

	for (i = 0; i < nr_tags; i++) {
		if (tagset->tags && tagset->tags[i])					/* tags 배열과 해당 entry가 유효한 NVMe CID pool이면 */
			__blk_mq_all_tag_iter(tagset->tags[i], fn, priv,
					      BT_TAG_ITER_STARTED);	/* started 상태의 NVMe 명령만 순회 -> 제출된 명령 대상 timeout/abort */
	}
	srcu_read_unlock(&tagset->tags_srcu, srcu_idx);				/* RCU read lock 해제 */
}
EXPORT_SYMBOL(blk_mq_tagset_busy_iter);

/*
 * blk_mq_tagset_count_completed_rqs()
 * 목적: 순회 중 완료되었지만 아직 정리되지 않은 request의 수를 센다.
 * 호출 경로:
 *   blk_mq_tagset_wait_completed_request -> blk_mq_tagset_busy_iter
 *   -> blk_mq_tagset_count_completed_rqs
 * NVMe 연결:
 *   - NVMe CQ가 완료를 보고했으나 아직 상위 레이어로 완료 처리되지 않은
 *     request가 남아있는지 확인할 때 사용된다.
 */
static bool blk_mq_tagset_count_completed_rqs(struct request *rq, void *data)
{
	unsigned *count = data;

	if (blk_mq_request_completed(rq))		/* NVMe CQ가 완료로 표시했지만 아직 blk-mq 상에서 정리되지 않은 명령 */
		(*count)++;				/* 완료 대기 중인 request 수 누적 */
	return true;					/* 모든 started request를 계속 순회 */
}

/**
 * blk_mq_tagset_wait_completed_request - Wait until all scheduled request
 * completions have finished.
 * @tagset:	Tag set to drain completed request
 *
 * Note: This function has to be run after all IO queues are shutdown
 */
/*
 * blk_mq_tagset_wait_completed_request()
 * 목적: tag set의 모든 scheduled completion이 끝날 때까지 기다린다.
 * 호출 경로:
 *   NVMe remove/reset 시 queue shutdown 후 호출(추정)
 *   -> blk_mq_tagset_wait_completed_request
 * NVMe 연결:
 *   - NVMe controller가 제거되거나 reset될 때, 이미 CQ에 완료된 request의
 *     소프트웨어 정리가 끝나기를 기다린다.
 *   - queue를 완전히 해제하기 전에 호출되어야 한다.
 */
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *tagset)
{
	while (true) {
		unsigned count = 0;							/* 남은 완료 처리 개수 */

		blk_mq_tagset_busy_iter(tagset,
				blk_mq_tagset_count_completed_rqs, &count);	/* NVMe 전체 queue pair에서 완료됐지만 정리 안 된 명령 카운트 */
		if (!count)
			break;								/* 모든 NVMe CQ 완료 처리 완료 -> queue 해제 가능 */
		msleep(5);								/* 아직 남아있으면 5ms 대기 후 재확인 -> NVMe ISR/completion work 소진 기다림 */
	}
}
EXPORT_SYMBOL(blk_mq_tagset_wait_completed_request);

/**
 * blk_mq_queue_tag_busy_iter - iterate over all requests with a driver tag
 * @q:		Request queue to examine.
 * @fn:		Pointer to the function that will be called for each request
 *		on @q. @fn will be called as follows: @fn(rq, @priv) where rq
 *		is a pointer to a request and hctx points to the hardware queue
 *		associated with the request.
 * @priv:	Will be passed as second argument to @fn.
 *
 * Note: if @q->tag_set is shared with other request queues then @fn will be
 * called for all requests on all queues that share that tag set and not only
 * for requests associated with @q.
 */
/*
 * blk_mq_queue_tag_busy_iter()
 * 목적: 특정 request_queue에서 driver tag를 가진 모든 request를 순회한다.
 * 호출 경로:
 *   (timeout, abort, reset 처리) -> blk_mq_queue_tag_busy_iter
 * NVMe 연결:
 *   - NVMe namespace의 request_queue에 대해 timeout이 발생하면, 해당 큐의
 *     모든 진행 중인 IO를 스캔하여 abort 또는 reset을 수행한다.
 *   - shared_tags 모드에서는 같은 tag_set을 공유하는 다른 namespace의
 *     request도 함께 순회하므로 주의가 필요하다.
 */
void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_tag_iter_fn *fn,
		void *priv)
{
	int srcu_idx;

	/*
	 * __blk_mq_update_nr_hw_queues() updates nr_hw_queues and queue_hw_ctx
	 * while the queue is frozen. So we can use q_usage_counter to avoid
	 * racing with it.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter))		/* NVMe namespace request_queue가 아직 살아있는지 확인; 제거 중이면 즉시 리턴 */
		return;

	srcu_idx = srcu_read_lock(&q->tag_set->tags_srcu);	/* tags 배열 보호: NVMe queue pair 재구성과 race 방지 */
	if (blk_mq_is_shared_tags(q->tag_set->flags)) {
		struct blk_mq_tags *tags = q->tag_set->shared_tags;		/* 여러 NVMe namespace가 공유하는 CID pool */
		struct sbitmap_queue *bresv = &tags->breserved_tags;		/* shared 예약 CID pool */
		struct sbitmap_queue *btags = &tags->bitmap_tags;		/* shared 일반 CID pool */

		if (tags->nr_reserved_tags)
			bt_for_each(NULL, q, bresv, fn, priv, true);		/* shared 예약 CID 영역(flush/admin) 순회 */
		bt_for_each(NULL, q, btags, fn, priv, false);			/* shared 일반 CID 영역 순회 -> 동일 tag_set 공유 namespace의 모든 IO 스캔 */
	} else {
		struct blk_mq_hw_ctx *hctx;
		unsigned long i;

		queue_for_each_hw_ctx(q, hctx, i) {				/* NVMe queue pair 하나씩 순회 -> SQ/CQ 쌍 단위 스캔 */
			struct blk_mq_tags *tags = hctx->tags;			/* 이 hctx의 CID pool */
			struct sbitmap_queue *bresv = &tags->breserved_tags;	/* per-hctx 예약 CID pool */
			struct sbitmap_queue *btags = &tags->bitmap_tags;	/* per-hctx 일반 CID pool */

			/* 소프트웨어 큐가 매핑되지 않은 hctx는 걸너뛴다. */
			/*
			 * If no software queues are currently mapped to this
			 * hardware queue, there's nothing to check
			 */
			if (!blk_mq_hw_queue_mapped(hctx))
				continue;						/* CPU/irq affinity에 매핑되지 않은 NVMe queue pair는 skip */

			if (tags->nr_reserved_tags)
				bt_for_each(hctx, q, bresv, fn, priv, true);	/* per-hctx 예약 CID(flush/admin) 순회 */
			bt_for_each(hctx, q, btags, fn, priv, false);		/* per-hctx 일반 CID 순회 -> timeout/abort 대상 수집 */
		}
	}
	srcu_read_unlock(&q->tag_set->tags_srcu, srcu_idx);			/* RCU 보호 해제 */
	blk_queue_exit(q);								/* q_usage_counter 참조 해제 -> NVMe namespace 제거 진행 허용 */
}

/*
 * bt_alloc()
 * 목적: sbitmap_queue를 초기화하여 주어진 depth만큼 tag를 관리할 수 있게 한다.
 * 호출 경로:
 *   blk_mq_init_tags -> bt_alloc
 * NVMe 연결:
 *   - NVMe SQ의 최대 엔트리 수(queue_depth)만큼 CID bitmap을 초기화한다.
 *   - round_robin 옵션은 CPU 간 공정성을 높인다(추정).
 */
static int bt_alloc(struct sbitmap_queue *bt, unsigned int depth,
		    bool round_robin, int node)
{
	return sbitmap_queue_init_node(bt, depth, -1, round_robin, GFP_KERNEL,
				       node);	/* depth만큼의 NVMe CID bitmap 초기화; round_robin으로 CPU 간 CID 분산(추정) */
}

/*
 * blk_mq_init_tags()
 * 목적: blk_mq_tags 구조체를 할당하고, 일반 tag bitmap과 예약 tag bitmap을
 *       초기화한다.
 * 호출 경로:
 *   blk_mq_alloc_map_and_rqs -> blk_mq_init_tags
 * NVMe 연결:
 *   - NVMe queue pair 생성 시 해당 queue의 CID pool을 만든다.
 *   - total_tags는 NVMe SQ depth, reserved_tags는 플러시 등 예약 명령용 CID
 *     수를 의미한다.
 *   - round_robin은 BLK_MQ_F_TAG_RR 플래그에 의해 결정된다.
 */
struct blk_mq_tags *blk_mq_init_tags(unsigned int total_tags,
		unsigned int reserved_tags, unsigned int flags, int node)
{
	unsigned int depth = total_tags - reserved_tags;	/* NVMe 일반 IO용 CID 수 = 전체 SQ depth - 예약 CID 수 */
	bool round_robin = flags & BLK_MQ_F_TAG_RR;		/* BLK_MQ_F_TAG_RR 플래그: CPU 간 round-robin CID 분배(추정) */
	struct blk_mq_tags *tags;

	if (total_tags > BLK_MQ_TAG_MAX) {
		pr_err("blk-mq: tag depth too large\n");		/* NVMe controller가 지원하는 SQ depth 초과 -> 초기화 실패 */
		return NULL;
	}

	tags = kzalloc_node(sizeof(*tags), GFP_KERNEL, node);	/* 노드 로컬 메모리로 blk_mq_tags 할당 -> NUMA 친화적 NVMe CID pool */
	if (!tags)
		return NULL;

	tags->nr_tags = total_tags;				/* 전체 tag 수 = NVMe SQ 최대 엔트리 수 */
	tags->nr_reserved_tags = reserved_tags;			/* 예약 tag 수 = NVMe flush/admin 등 우선 명령용 CID 수 */
	spin_lock_init(&tags->lock);				/* active_queues/wake_batch 보호용 lock */
	INIT_LIST_HEAD(&tags->page_list);			/* request pool 페이지 리스트 초기화 -> NVMe request 할당 지연 시 사용 */

	if (bt_alloc(&tags->bitmap_tags, depth, round_robin, node))		/* NVMe 일반 CID bitmap 초기화 */
		goto out_free_tags;
	if (bt_alloc(&tags->breserved_tags, reserved_tags, round_robin, node))	/* NVMe 예약 CID bitmap 초기화 */
		goto out_free_bitmap_tags;

	return tags;						/* CID pool 준비 완료 -> NVMe SQ와 1:1 매핑 가능 */

out_free_bitmap_tags:
	sbitmap_queue_free(&tags->bitmap_tags);			/* 일반 CID bitmap 해제 */
out_free_tags:
	kfree(tags);						/* tags 구조체 해제 -> NVMe queue pair 생성 실패 */
	return NULL;
}

/*
 * blk_mq_free_tags_callback()
 * 목적: RCU grace period가 지난 후 tags와 연결된 페이지들을 해제한다.
 * 호출 경로:
 *   blk_mq_free_tags -> call_srcu -> blk_mq_free_tags_callback
 * NVMe 연결:
 *   - NVMe queue pair가 제거될 때, 모든 진행 중인 IO가 완료된 후 CID pool과
 *     request pool 페이지를 안전하게 해제한다.
 */
static void blk_mq_free_tags_callback(struct rcu_head *head)
{
	struct blk_mq_tags *tags = container_of(head, struct blk_mq_tags,
						rcu_head);	/* RCU 콜백에서 tags 구조체 복원 */
	struct page *page;

	while (!list_empty(&tags->page_list)) {						/* request pool에 할당된 페이지가 남아있으면 */
		page = list_first_entry(&tags->page_list, struct page, lru);	/* 페이지 하나 꺼냄 */
		list_del_init(&page->lru);						/* NVMe request pool 리스트에서 제거 */
		/*
		 * Remove kmemleak object previously allocated in
		 * blk_mq_alloc_rqs().
		 */
		kmemleak_free(page_address(page));					/* kmemleak 객체 제거 -> 가짜 누수 방지 */
		__free_pages(page, page->private);					/* NVMe request pool 페이지 반납 -> DMA/PRP/SGL에 사용된 메모리 해제(추정) */
	}
	kfree(tags);									/* CID pool 구조체 최종 해제 -> NVMe queue pair 메모리 정리 완료 */
}

/*
 * blk_mq_free_tags()
 * 목적: tag bitmap을 해제하고, 필요하면 RCU를 통해 tags 구조체를 지연 해제한다.
 * 호출 경로:
 *   blk_mq_free_map_and_rqs -> blk_mq_free_tags
 * NVMe 연결:
 *   - NVMe controller 해제 또는 queue pair 재구성 시 tag pool을 정리한다.
 *   - page_list가 비어 있지 않으면(아직 참조 중인 request pool이 있으면)
 *     RCU grace period 후에 실제 메모리를 해제한다.
 */
void blk_mq_free_tags(struct blk_mq_tag_set *set, struct blk_mq_tags *tags)
{
	sbitmap_queue_free(&tags->bitmap_tags);					/* NVMe 일반 CID bitmap 해제 */
	sbitmap_queue_free(&tags->breserved_tags);					/* NVMe 예약 CID bitmap 해제 */

	/* request pool 페이지가 아직 할당되지 않았으면 즉시 해제한다. */
	/* if tags pages is not allocated yet, free tags directly */
	if (list_empty(&tags->page_list)) {						/* request pool이 아직 없으면(아무 IO도 없었거나 초기화 단계) */
		kfree(tags);								/* tags 즉시 해제 */
		return;
	}

	call_srcu(&set->tags_srcu, &tags->rcu_head, blk_mq_free_tags_callback);	/* RCU grace period 후 메모리 해제 -> NVMe 진행 중인 IO가 tags를 여전히 참조할 수 있으므로(추정) */
}

/*
 * blk_mq_tag_resize_shared_tags()
 * 목적: shared tag pool의 크기를 동적으로 조정한다.
 * 호출 경로:
 *   (tag_set의 queue_depth 변경 시) -> blk_mq_tag_resize_shared_tags
 * NVMe 연결:
 *   - NVMe 장치의 SQ depth가 런타임에 변경될 때(예: APST, 장치 성능 상태
 *     변화에 따른 depth 조정) shared CID pool 크기를 재설정한다(추정).
 */
void blk_mq_tag_resize_shared_tags(struct blk_mq_tag_set *set, unsigned int size)
{
	struct blk_mq_tags *tags = set->shared_tags;				/* shared CID pool */

	sbitmap_queue_resize(&tags->bitmap_tags, size - set->reserved_tags);	/* NVMe SQ depth 변경 시 일반 CID bitmap 크기 조정; 예약 영역 제외 */
}

/*
 * blk_mq_tag_update_sched_shared_tags()
 * 목적: IO scheduler가 사용하는 shared tag pool의 크기를 조정한다.
 * 호출 경로:
 *   blk_mq_update_nr_requests -> blk_mq_tag_update_sched_shared_tags
 * NVMe 연결:
 *   - NVMe queue에 scheduler(mq-deadline, bfq 등)가 붙어 있고, shared tag
 *     pool을 사용할 때 scheduler tag 영역의 크기를 조정한다.
 *   - scheduler tag는 NVMe CID와는 별개의 낮부 관리용 tag이다.
 */
void blk_mq_tag_update_sched_shared_tags(struct request_queue *q,
					 unsigned int nr)
{
	sbitmap_queue_resize(&q->sched_shared_tags->bitmap_tags,
			     nr - q->tag_set->reserved_tags);	/* IO scheduler용 shared tag pool 크기 조정; NVMe CID와는 별개의 scheduler internal tag(추정) */
}

/**
 * blk_mq_unique_tag() - return a tag that is unique queue-wide
	return (rq->mq_hctx->queue_num << BLK_MQ_UNIQUE_TAG_BITS) |	/* NVMe queue pair 번호를 상위 비트에 배치 -> 전역 unique ID 생성 */
		(rq->tag & BLK_MQ_UNIQUE_TAG_MASK);			/* 하위 비트는 per-queue CID(NVMe command id) */
}
EXPORT_SYMBOL(blk_mq_unique_tag);


/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - 본 파일은 NVMe CID(Command ID) 할당/회수의 blk-mq 측 핵심 엔진이다.
 *   요청 경로: blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_get_tag
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell).
 * - tag는 NVMe SQ slot과 1:1로 대응하며, SQ depth, CQ depth, 그리고 동시
 *   진행 가능한 IO 수를 제한하는 핵심 자원이다.
 * - 공유 tag pool(BLK_MQ_F_TAG_HCTX_SHARED)을 사용하면 여러 NVMe queue가
 *   하나의 CID pool을 나눠 쓰게 되며, active_queues 카운트와 hctx_may_queue
 *   를 통해 공정 분배를 시도한다.
 * - 예약 tag 영역(breserved_tags)은 플러시 등 우선 처리가 필요한 명령에
 *   사용되며, 일반 IO CID 영역(bitmap_tags)과 분리 관리된다.
 * - 완료 경로에서는 NVMe CQ가 보고한 CID에 해당하는 request를 찾아
 *   blk_mq_put_tag로 tag를 회수하고, 이후 다른 IO가 해당 CID를 재사용할
 *   수 있게 된다.
 * ============================================================================
 */
