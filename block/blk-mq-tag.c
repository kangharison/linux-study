// SPDX-License-Identifier: GPL-2.0
/*
 * Tag allocation using scalable bitmaps. Uses active queue tracking to support
 * fairer distribution of tags between multiple submitters when a shared tag map
 * is used.
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */

/*
 * [한국어] blk-mq tag 할당·해제 엔진 (block/blk-mq-tag.c)
 *
 * === 파일의 역할 ===
 * blk-mq 레이어에서 "tag"를 할당·해제하는 핵심 엔진이다. tag 는 0..(깊이-1)
 * 범위의 정수 하나로, 이것을 쥐고 있는 동안만 요청이 장치로 내려갈 수 있다.
 * 즉 tag 는 단순한 식별자가 아니라 **동시 진행(in-flight) IO 수를 제한하는
 * 자원 티켓**이다. tag 가 없으면 요청은 여기서 잠들어 기다린다. 할당/해제는
 * lib/sbitmap.c 의 sbitmap_queue(계층형 lock-free 비트맵) 위에서 이루어져,
 * 수십 개 CPU 가 동시에 tag 를 다투어도 단일 락 병목이 생기지 않는다.
 * 같은 tag 값이 `tags->rqs[tag]` 배열의 인덱스로도 쓰이므로, 장치가 완료를
 * 보고할 때 정수 하나만으로 원래 request 포인터를 O(1)에 복원할 수 있다.
 *
 * === NVMe 독자를 위한 정확한 대응 관계 (흔한 오해 교정) ===
 * "blk-mq tag == NVMe Command ID" 라는 설명이 널리 퍼져 있으나 부정확하다.
 * drivers/nvme/host/nvme.h 의 실제 정의는 다음과 같다:
 *
 *     CID(16bit) = | gen(4bit) | blk-mq tag(12bit) |
 *     nvme_cid(rq) = nvme_cid_install_genctr(nvme_req(rq)->genctr) | rq->tag
 *
 * 상위 4비트는 세대 카운터(genctr)다. tag 는 반납 즉시 재사용되므로, 늦게
 * 도착한 이전 세대의 CQE 가 새 요청을 잘못 완료시킬 수 있다. 세대 니블이
 * 이 stale 완료를 걸러낸다(nvme_find_rq 가 불일치 시 NULL 반환).
 * NVME_QUIRK_SKIP_CID_GEN 장치만 gen 없이 tag 를 그대로 쓴다.
 *
 * 이 12비트 폭이 실제 제약으로 나타난 곳이 drivers/nvme/host/pci.c 다:
 *     #define NVME_PCI_MAX_QUEUE_SIZE 4095      (= 0xfff)
 *     MODULE_PARM_DESC(io_queue_depth, "should >= 2 and < 4096")
 * 즉 **NVMe PCIe 의 I/O 큐 깊이 상한 4095 는 이 파일이 발급하는 tag 가
 * 12비트에 들어가야 하기 때문에 생긴 값**이다(기본값 1024).
 *
 * 또 하나 흔한 오해: "tag 가 SQ 슬롯 번호와 1:1 대응한다"는 서술도 틀렸다.
 * SQE 가 기록되는 위치는 링의 생산자 인덱스로 정해진다:
 *     memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes), cmd, 64);
 * tag 와 sq_tail 은 서로 독립적이다. tag 는 "몇 개까지 동시에 띄울 수
 * 있는가"를 정하고, sq_tail 은 "이번 것을 링 어디에 쓸 것인가"를 정한다.
 * 두 값의 상한이 같은 q_depth 로 맞춰져 있을 뿐이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 제출 경로 (모두 블록 계층 내부 호출):
 *   blk_mq_submit_bio → blk_mq_get_new_requests → __blk_mq_alloc_requests
 *   → blk_mq_get_tag [본 파일] → (tag 확보 후) blk_mq_dispatch_rq_list
 *   → mq_ops->queue_rq
 * 여기서 mq_ops->queue_rq 는 **함수 포인터 간접 호출**이며, NVMe PCIe 가
 * 붙어 있을 때 그 실체가 nvme_queue_rq 다. 블록 계층 코드가 nvme_* 심볼을
 * 직접 부르는 곳은 한 군데도 없다.
 * 완료 경로:
 *   (NVMe PCIe 기준) nvme_irq → nvme_poll_cq → nvme_handle_cqe
 *   → nvme_try_complete_req → blk_mq_complete_request → __blk_mq_end_request
 *   → blk_mq_put_driver_tag → blk_mq_put_tag [본 파일] → sbitmap_queue_clear
 *   → tag 를 기다리며 잠든 제출자 wakeup
 * 실행 컨텍스트: 제출 경로는 프로세스 컨텍스트(잠들 수 있음; BLK_MQ_REQ_NOWAIT
 * 이면 잠들지 않고 즉시 실패). 완료 경로는 장치 인터럽트 핸들러에서
 * 시작하지만, blk_mq_complete_request 가 완료 CPU 정책(BLK_MQ_F_BLOCKING,
 * IPI/softirq 위임)에 따라 다른 컨텍스트로 넘길 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - lib/sbitmap.c: sbitmap_queue 자료구조 본체. 실제 비트 탐색/해제/대기열
 *     wakeup 로직은 전부 여기에 있고, 본 파일은 정책(예약 영역 분리, 공정
 *     분배, hctx 활성 상태 확인)만 얹는다.
 *   - block/blk-mq.c: 요청 할당 경로가 blk_mq_get_tag()를, 완료/해제 경로가
 *     blk_mq_put_tag()를 호출한다.
 *   - block/blk-mq-sched.h: IO 스케줄러가 붙으면 shallow_depth 로 한 큐가
 *     드라이버 태그를 독점하지 못하게 제한한다.
 * 피의존 모듈:
 *   - 모든 blk-mq 드라이버(nvme, scsi, virtio-blk 등)가 blk_mq_tag_to_rq()로
 *     완료 시 정수 → request 복원을 수행한다. NVMe 는 nvme_find_rq() 안에서
 *     이 함수를 부른다.
 *   - block/blk-mq-tag.h: blk_mq_tags 구조체 및 공개 API 선언.
 * 공유 자료구조:
 *   - struct blk_mq_tags: tag pool 전체 상태(비트맵, 예약 영역, rqs 배열 등).
 *   - struct blk_mq_alloc_data: tag 할당 시 CPU/hctx/플래그 컨텍스트 전달.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_mq_get_tag()      : tag 할당 진입점; 고갈 시 sbitmap 대기열에서 sleep.
 * blk_mq_put_tag()      : 완료 후 tag 반납; 비트 클리어 + waiter wakeup.
 * blk_mq_init_tags()    : blk_mq_tags 구조체 할당 및 sbitmap_queue 초기화.
 * blk_mq_free_tags()    : tag pool 해제; 참조 중인 page_list는 RCU 후 해제.
 * blk_mq_tagset_busy_iter(): tag set의 모든 started request 순회(timeout/abort).
 * blk_mq_queue_tag_busy_iter(): 특정 request_queue의 driver tag request 순회.
 * blk_mq_tag_resize_shared_tags(): shared tag pool depth 동적 재조정.
 */

#include <linux/kernel.h>   /* [한국어] pr_err, WARN_ON_ONCE 등 커널 기본 매크로 제공 */
#include <linux/module.h>   /* [한국어] EXPORT_SYMBOL — blk_mq_tagset_busy_iter 등을 드라이버에 공개 */
#include <linux/slab.h>     /* [한국어] kzalloc_node/kfree — NUMA 로컬 blk_mq_tags 동적 할당 */
#include <linux/mm.h>       /* [한국어] __free_pages — request pool 페이지 반납에 사용 */
#include <linux/kmemleak.h> /* [한국어] kmemleak_free — request pool 페이지 해제 시 가짜 누수 경고 제거 */

#include <linux/delay.h>    /* [한국어] msleep — blk_mq_tagset_wait_completed_request의 폴링 대기 */
#include "blk.h"            /* [한국어] blk_queue_exit 등 blk 공통 내부 API */
#include "blk-mq.h"         /* [한국어] blk_mq_hw_ctx, blk_mq_tags, blk_mq_alloc_data 등 핵심 구조체 */
#include "blk-mq-sched.h"   /* [한국어] hctx_may_queue — IO 스케줄러 연동 시 quota 판단 함수 */

/*
 * [한국어] struct blk_mq_tags 주요 필드 — tag 회계 관점 (include/linux/blk-mq.h 정의)
 *
 * nr_tags:
 *   전체 tag 수(예약 + 일반). NVMe 라면 nvme_alloc_io_tag_set() 이 넣는
 *   min(ctrl->sqsize, BLK_MQ_MAX_DEPTH-1) 이 여기 들어오므로 SQ 깊이와
 *   같은 값이 되지만, 개념적으로는 "동시 인플라이트 상한"이지 링 크기가 아니다.
 *   동시에 진행 가능한 tag의 총 개수를 제한한다.
 *   설정자: blk_mq_init_tags(). 읽는 자: blk_mq_get_tag(), bt_alloc().
 *   값 범위: 1 ~ BLK_MQ_TAG_MAX. 동기화: 초기화 후 불변.
 *
 * nr_reserved_tags:
 *   예약 tag 수. NVMe admin/flush 등 긴급 명령이 일반 IO에 밀려 starvation되지
 *   않도록 별도 예약된 tag 영역이다.
 *   설정자: blk_mq_init_tags(). 읽는 자: blk_mq_get_tag(), blk_mq_put_tag().
 *   값 범위: 0 ~ nr_tags - 1. 동기화: 초기화 후 불변.
 *
 * active_queues:
 *   현재 IO를 제출 중인 hardware queue(hctx) 수. SQ 사용률에 따라 wakeup batch를
 *   재조정하여 공정한 tag 분배를 지원한다.
 *   설정자: __blk_mq_tag_busy()/__blk_mq_tag_idle() — tags->lock 하에 WRITE_ONCE.
 *   읽는 자: blk_mq_update_wake_batch(). 동기화: tags->lock 스핀락.
 *
 * bitmap_tags:
 *   일반 IO 요청용 sbitmap_queue. NVMe 로 치면 이 큐로 동시에 띄울 수 있는 명령 수를 정한다.
 *   설정자: bt_alloc(). 할당: __blk_mq_get_tag(). 해제: blk_mq_put_tag().
 *   동기화: sbitmap 내부 atomic 연산으로 lock-free 처리.
 *
 * breserved_tags:
 *   예약 요청(BLK_MQ_REQ_RESERVED)용 sbitmap_queue. 표준 NVMe PCIe 에서는 크기 0 이라 사실상 쓰이지 않는다.
 *   설정자: bt_alloc(). 읽는 자: blk_mq_get_tag() — REQ_RESERVED 경로.
 *   동기화: sbitmap 내부 atomic 연산.
 *
 * rqs[tag]:
 *   tag에 매핑된 struct request 포인터 배열. tag→request 역조회에 사용되며,
 *   NVMe 는 CQE 의 command_id 에서 하위 12비트(nvme_tag_from_cid)를 뽑아
 *   이 배열을 인덱싱한다 — 장치가 돌려주는 것은 정수뿐이므로, 완료를 원래
 *   request 에 다시 붙이는 유일한 수단이 이 배열이다.
 *   설정자: blk_mq_rq_ctx_init() — bit set 이후 할당(race 가능).
 *   읽는 자: blk_mq_find_and_get_req(). 동기화: req_ref 원자적 참조 카운트.
 *
 * static_rqs[tag]:
 *   정적으로 사전 할당된 request 배열. NVMe reset/recovery 단계처럼 rqs[]가
 *   아직 완전히 초기화되지 않은 상황에서도 안전하게 참조할 수 있다.
 *   설정자: blk_mq_alloc_rqs(). 읽는 자: bt_tags_iter() — BT_TAG_ITER_STATIC_RQS.
 *   동기화: 초기화 후 불변(read-only).
 *
 * lock:
 *   active_queues/wake_batch 갱신 시 사용하는 스핀락. shared tag set 에서 큐 간 공정 분배를
 *   위한 짧은 임계 구간만 보호한다.
 *   사용처: __blk_mq_tag_busy(), __blk_mq_tag_idle(), blk_mq_update_wake_batch().
 */

/*
 * Recalculate wakeup batch when tag is shared by hctx.
 */
/*
 * [한국어]
 * blk_mq_update_wake_batch - shared tag pool의 wakeup batch 값을 재계산한다.
 *
 * @tags:  wakeup batch를 조정할 blk_mq_tags (tag pool 전체 상태 포함).
 * @users: 현재 active 상태인 hardware queue(hctx) 수.
 * @return: 없음.
 *
 * 여러 hctx가 하나의 sbitmap_queue를 공유할 때, active user 수에 맞게 wakeup
 * batch 크기를 재계산하여 thundering herd 현상을 완화한다. 예를 들어 NVMe
 * namespace 간 shared tag pool을 사용하는 경우, user 수가 늘수록 한 번에 깨어나는
 * waiter 수를 줄여 tag 경쟁을 분산한다.
 * 실행 컨텍스트: tags->lock 스핀락을 보유한 상태에서 호출된다.
 * 호출자: __blk_mq_tag_busy(), __blk_mq_tag_idle().
 * 피호출자: sbitmap_queue_recalculate_wake_batch() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   __blk_mq_tag_busy/__blk_mq_tag_idle → [blk_mq_update_wake_batch]
 *   → sbitmap_queue_recalculate_wake_batch
 */
static void blk_mq_update_wake_batch(struct blk_mq_tags *tags,
		unsigned int users)
{
	if (!users)
		/* [한국어] active_queues가 0이면 재계산 불필요 — 모든 NVMe queue pair가 idle 상태임 */
		return;

	sbitmap_queue_recalculate_wake_batch(&tags->bitmap_tags,
			users);	/* [한국어] 일반 tag pool(bitmap_tags)의 wakeup batch 재계산 — user 증가 시 batch 축소로 thundering herd 완화 */
	sbitmap_queue_recalculate_wake_batch(&tags->breserved_tags,
			users);	/* [한국어] 예약 tag pool(breserved_tags)도 동일하게 batch 재계산 — flush/admin 명령 경쟁도 분산 */
}

/*
 * If a previously inactive queue goes active, bump the active user count.
 * We need to do this before try to allocate driver tag, then even if fail
 * to get tag when first time, the other shared-tag users could reserve
 * budget for it.
 */
/*
 * [한국어]
 * __blk_mq_tag_busy - 비활성 hctx가 IO 제출을 재개할 때 active user count 증가.
 *
 * @hctx: 활성화되는 hardware queue 컨텍스트(NVMe SQ/CQ 쌍에 해당).
 * @return: 없음.
 *
 * 이전에 idle 상태였던 hctx가 다시 IO를 제출하기 시작하면 호출된다. tag 할당을
 * 시도하기 전에 active_queues를 증가시켜야, 첫 번째 할당이 실패하더라도 shared
 * tag pool의 다른 user들이 이 hctx를 위한 예산을 확보해 줄 수 있다.
 * shared tag 모드(BLK_MQ_F_TAG_QUEUE_SHARED)에서는 request_queue 단위 플래그를,
 * per-hctx 모드에서는 hctx->state 비트를 사용하여 중복 호출을 막는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 softirq; spin_lock_irqsave로 보호.
 * 호출자: blk_mq_tag_busy() (blk-mq.h 인라인).
 * 피호출자: blk_mq_update_wake_batch().
 *
 * 호출 체인:
 *   blk_mq_tag_busy → [__blk_mq_tag_busy] → blk_mq_update_wake_batch
 */
void __blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	unsigned int users;		/* [한국어] 갱신 후의 활성 큐 수 — wake_batch 재계산 입력값 */
	unsigned long flags;		/* [한국어] tags->lock 을 irqsave 로 잡을 때 저장할 인터럽트 상태.
					 * 완료 경로가 인터럽트 컨텍스트에서 이 락을 건드릴 수 있어 spin_lock 만으로는 부족하다 */
	struct blk_mq_tags *tags = hctx->tags;	/* [한국어] 이 hctx가 사용하는 tag pool — 이 hctx 전용 pool, 또는 shared tag set 이면 큐 전체가 공유하는 pool */

	/*
	 * calling test_bit() prior to test_and_set_bit() is intentional,
	 * it avoids dirtying the cacheline if the queue is already active.
	 */
	if (blk_mq_is_shared_tags(hctx->flags)) {	/* [한국어] 공유 구성에서는 "활성"의 단위가 hctx 가 아니라 request_queue 다.
						 * 같은 큐의 hctx 여럿이 각자 카운트를 올리면 한 큐가 몫을 여러 번 받는 셈이 되므로,
						 * 큐 단위 플래그를 두어 큐당 정확히 한 번만 세도록 만든다. */
		struct request_queue *q = hctx->queue;	/* [한국어] 플래그를 걸어 둘 대상 — hctx 가 아니라 그 상위 큐 */

		/* [한국어] shared tag 모드: request_queue 단위 HCTX_ACTIVE 플래그 검사 후 원자적 세트
		 * test_bit()로 캐시라인 오염 없이 먼저 확인한 뒤, test_and_set_bit()로 중복 카운트 방지 */
		if (test_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags) ||
		    test_and_set_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags))
			return;	/* [한국어] 이미 active 상태이면 active_queues를 다시 올리지 않음 */
	} else {
		/* [한국어] per-hctx tag 모드: hctx->state의 BLK_MQ_S_TAG_ACTIVE 비트 검사 후 원자적 세트
		 * 동일 hctx에서 중복 호출되는 경우를 차단 */
		if (test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state) ||
		    test_and_set_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return;	/* [한국어] 이미 TAG_ACTIVE 상태이면 반환 */
	}

	spin_lock_irqsave(&tags->lock, flags);	/* [한국어] active_queues/wake_batch 동시 갱신 보호 — 인터럽트까지 막아 NVMe ISR와의 경쟁 방지 */
	users = tags->active_queues + 1;	/* [한국어] 새로 활성화된 NVMe queue(hctx)를 공유 풀 user 카운트에 추가 */
	WRITE_ONCE(tags->active_queues, users);	/* [한국어] 컴파일러/CPU 재배치 방지 — 다른 CPU에서 읽는 active_queues가 즉시 갱신된 값을 보도록 보장 */
	blk_mq_update_wake_batch(tags, users);	/* [한국어] active queue 수 변화에 맞춰 wakeup batch 재조정 — 공정한 tag 분배 */
	spin_unlock_irqrestore(&tags->lock, flags);	/* [한국어] 임계 구간 종료 및 인터럽트 복원 */
}

/*
 * Wakeup all potentially sleeping on tags
 */
/*
 * [한국어]
 * blk_mq_tag_wakeup_all - tag를 기다리며 잠든 모든 waiter를 깨운다.
 *
 * @tags:            wakeup 대상 tag pool.
 * @include_reserve: true이면 예약 tag pool(breserved_tags)의 waiter도 깨운다.
 * @return: 없음.
 *
 * tag 가 없어 blk_mq_get_tag() 안에서 io_schedule() 로 잠든 제출자들을
 * 모두 깨운다. tag가 회수되거나 pool 크기가 변경되어 빈 slot이 생겼을 때
 * 호출되며, 깨어난 submitter는 다시 __blk_mq_get_tag()를 시도한다.
 * 실행 컨텍스트: NVMe 완료 인터럽트 후 softirq, 또는 프로세스 컨텍스트.
 * 호출자: __blk_mq_tag_idle(), blk_mq_tag_resize_shared_tags() 등.
 * 피호출자: sbitmap_queue_wake_all() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   __blk_mq_tag_idle → [blk_mq_tag_wakeup_all] → sbitmap_queue_wake_all
 */
void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool include_reserve)
{
	sbitmap_queue_wake_all(&tags->bitmap_tags);	/* [한국어] 일반 tag pool 대기자 전원 깨움 — tag 가 생겼으니 다시 시도해 보라고 알리는 것 */
	if (include_reserve)	/* [한국어] 예약 tag pool 깨움 필요 여부 확인 — __blk_mq_tag_idle은 false, 외부 resize는 true 전달 */
		sbitmap_queue_wake_all(&tags->breserved_tags);	/* [한국어] 예약 tag pool(flush/admin) 대기자도 깨움 */
}

/*
 * If a previously busy queue goes inactive, potential waiters could now
 * be allowed to queue. Wake them up and check.
 */
/*
 * [한국어]
 * __blk_mq_tag_idle - busy였던 hctx가 idle로 전환될 때 active user count 감소.
 *
 * @hctx: idle로 전환되는 hardware queue 컨텍스트(NVMe SQ/CQ 쌍).
 * @return: 없음.
 *
 * 이전에 busy(active) 상태였던 hctx가 더 이상 IO를 제출하지 않으면 호출된다.
 * active_queues를 감소시켜 공유 tag pool에서 사용 중이던 예산을 반납하고,
 * 다른 hctx가 더 많은 tag를 할당받을 수 있도록 wakeup batch를 재조정한다.
 * 이후 대기 중인 모든 submitter를 깨워 tag 재할당을 시도하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트; spin_lock_irq로 ISR와의 경쟁 방지.
 * 호출자: blk_mq_tag_idle() (blk-mq.h 인라인).
 * 피호출자: blk_mq_update_wake_batch(), blk_mq_tag_wakeup_all().
 *
 * 호출 체인:
 *   blk_mq_tag_idle → [__blk_mq_tag_idle]
 *   → blk_mq_update_wake_batch, blk_mq_tag_wakeup_all
 */
void __blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;	/* [한국어] 이 hctx가 사용하는 tag pool — per-hctx 또는 shared */
	unsigned int users;			/* [한국어] 감소 후의 활성 큐 수 — 0 이 될 수도 있다 */

	if (blk_mq_is_shared_tags(hctx->flags)) {	/* [한국어] busy 쪽과 대칭 — 활성 해제도 큐 단위로 정확히 한 번만 일어나야 한다 */
		struct request_queue *q = hctx->queue;	/* [한국어] 플래그가 걸려 있는 상위 큐 */

		/* [한국어] shared tag 모드: QUEUE_FLAG_HCTX_ACTIVE 비트를 원자적으로 클리어
		 * 이미 inactive면 중복 감소를 막기 위해 즉시 반환 */
		if (!test_and_clear_bit(QUEUE_FLAG_HCTX_ACTIVE,
					&q->queue_flags))
			return;	/* [한국어] 이미 inactive였으면 active_queues를 다시 내리지 않음 */
	} else {
		/* [한국어] per-hctx 모드: BLK_MQ_S_TAG_ACTIVE 비트 클리어 — NVMe queue pair idle 전환 */
		if (!test_and_clear_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return;	/* [한국어] 이미 비활성 상태이면 반환 */
	}

	spin_lock_irq(&tags->lock);	/* [한국어] active_queues/wake_batch 갱신 보호 — NVMe ISR와의 경쟁 차단 */
	users = tags->active_queues - 1;	/* [한국어] idle로 전환된 hctx를 공유 풀 user 카운트에서 제거 */
	WRITE_ONCE(tags->active_queues, users);	/* [한국어] 컴파일러/CPU 재배치 방지 — 다른 CPU에서 즉시 갱신된 값이 보이도록 */
	blk_mq_update_wake_batch(tags, users);	/* [한국어] user 수 감소에 맞춰 wakeup batch 재조정 — 남은 active queue가 더 많은 tag 확보 가능 */
	spin_unlock_irq(&tags->lock);	/* [한국어] 임계 구간 종료 */

	blk_mq_tag_wakeup_all(tags, false);	/* [한국어] 예약 tag 제외한 일반 대기자 전원 깨움 — idle 전환으로 생긴 tag 여유를 다른 submitter에 재분배 */
}

/*
 * [한국어]
 * __blk_mq_get_tag - sbitmap_queue에서 tag 하나를 즉시(non-blocking) 할당한다.
 *
 * @data: tag 할당 컨텍스트 — hctx, flags(REQ_RESERVED/NOWAIT), shallow_depth 포함.
 * @bt:   할당 대상 sbitmap_queue — bitmap_tags(일반) 또는 breserved_tags(예약).
 * @return: 할당된 local tag 값(0 이상), 또는 BLK_MQ_NO_TAG(실패).
 *
 * blk_mq_get_tag()의 내부 fast-path. IO 스케줄러 없이 shared tag pool을 사용하는
 * 경우 hctx_may_queue()로 이 hctx의 quota를 확인한 뒤, sbitmap에서 빈 비트를
 * 원자적으로 획득한다. shallow_depth가 설정된 경우(IO 스케줄러가 queue depth를
 * 임시 제한) sbitmap_queue_get_shallow()로 제한된 범위 내에서만 할당한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 softirq; lock-free(sbitmap atomic ops).
 * 호출자: blk_mq_get_tag().
 * 피호출자: hctx_may_queue(), sbitmap_queue_get_shallow(), __sbitmap_queue_get().
 *
 * 호출 체인:
 *   blk_mq_get_tag → [__blk_mq_get_tag]
 *   → hctx_may_queue / sbitmap_queue_get_shallow / __sbitmap_queue_get
 */
static int __blk_mq_get_tag(struct blk_mq_alloc_data *data,
			    struct sbitmap_queue *bt)
{
	/* [한국어] IO 스케줄러 없고 예약 요청도 아닌데 이 hctx의 quota가 초과됐으면 즉시 실패
	 * — shared tag pool 공정 분배 보장; quota 초과 hctx는 대기 후 재시도 */
	if (!data->q->elevator && !(data->flags & BLK_MQ_REQ_RESERVED) &&
			!hctx_may_queue(data->hctx, bt))
		return BLK_MQ_NO_TAG;	/* [한국어] tag 할당 실패 반환 — 상위 blk_mq_get_tag가 sleep 또는 NOWAIT 처리 결정 */

	if (data->shallow_depth)	/* [한국어] IO 스케줄러가 shallow_depth로 queue depth를 임시 제한한 상태 */
		return sbitmap_queue_get_shallow(bt, data->shallow_depth);	/* [한국어] sbitmap 전체가 아니라 앞쪽 shallow_depth 범위에서만 비트를 찾는다.
									 * IO 스케줄러가 한 큐/한 cgroup 이 드라이버 태그를 전부 채가지 못하도록
									 * 상한을 거는 경로다. 태그를 다 뺏기면 스케줄러 큐에 재정렬할 후보가
									 * 남지 않아 정책 자체가 무력화되기 때문이다. */
	else
		return __sbitmap_queue_get(bt);	/* [한국어] 제한 없이 sbitmap에서 빈 비트 하나를 원자적으로 획득 */
}

/*
 * [한국어]
 * blk_mq_get_tags - 여러 tag를 한 번에 batch로 할당한다.
 *
 * @data:    tag 할당 컨텍스트 — hctx, flags, shallow_depth 포함.
 * @nr_tags: 한 번에 할당하려는 tag 수.
 * @offset:  할당된 tag들의 sbitmap 내 시작 offset을 반환(nr_reserved_tags 보정 포함).
 * @return:  할당된 tag 집합을 나타내는 비트맵(unsigned long); 0이면 실패.
 *
 * plug-merge 이후 여러 요청을 일괄 제출할 때 tag 여러 개를 한 번의 atomic
 * 연산으로 예약한다. shallow_depth 제한, 예약 tag 요청, shared tag pool 사용 시에는
 * batch 경로를 쓸 수 없으므로 0을 반환하여 개별 blk_mq_get_tag() 경로로 fallback.
 * 실행 컨텍스트: 프로세스 컨텍스트; lock-free(sbitmap atomic ops).
 * 호출자: blk-mq 내부 다중 요청 할당 경로.
 * 피호출자: blk_mq_tags_from_data(), __sbitmap_queue_get_batch().
 *
 * 호출 체인:
 *   blk_mq_alloc_requests(다중) → [blk_mq_get_tags]
 *   → __sbitmap_queue_get_batch
 */
unsigned long blk_mq_get_tags(struct blk_mq_alloc_data *data, int nr_tags,
			      unsigned int *offset)
{
	struct blk_mq_tags *tags = blk_mq_tags_from_data(data);	/* [한국어] 이 hctx에 해당하는 blk_mq_tags(tag pool) 획득 */
	struct sbitmap_queue *bt = &tags->bitmap_tags;	/* [한국어] 일반 IO용 tag pool 선택 — batch 할당은 일반 pool에서만 지원 */
	unsigned long ret;				/* [한국어] 획득한 tag 들의 비트마스크. 0 이면 하나도 못 얻은 것이고,
							 * 그 외에는 *offset 부터 시작하는 연속 구간 안에서 어느 비트를 얻었는지를 나타낸다.
							 * 연속 확보를 노리는 이유는 request 초기화를 한 루프로 돌리기 위해서다. */

	/* [한국어] batch 할당 불가 조건 확인: shallow_depth 제한, 예약 tag 요청, shared tag pool 사용 시
	 * 각각 공정성·예약성·복잡성 문제로 개별 할당 경로로 fallback */
	if (data->shallow_depth ||data->flags & BLK_MQ_REQ_RESERVED ||
	    data->hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		return 0;	/* [한국어] 0 반환 시 호출자가 개별 blk_mq_get_tag()로 재시도 */
	ret = __sbitmap_queue_get_batch(bt, nr_tags, offset);	/* [한국어] sbitmap에서 nr_tags개의 tag를 원자적으로 일괄 획득; *offset에 sbitmap 내 시작 위치 저장 */
	*offset += tags->nr_reserved_tags;	/* [한국어] nr_reserved_tags 크기만큼 offset 보정 — 예약 영역 다음에 일반 tag가 시작하므로 */
	return ret;	/* [한국어] 할당된 tag 비트맵 반환 — 0이면 sbitmap 여유 없음, 호출자가 개별 할당으로 재시도 */
}

/*
 * [한국어]
 * blk_mq_get_tag - tag 할당의 핵심 진입점; SQ 가득 차면 sleep 대기.
 *
 * @data: tag 할당 컨텍스트 — q, hctx, ctx, flags(REQ_RESERVED/NOWAIT), shallow_depth.
 * @return: 할당된 tag 값(0 이상, tag_offset 포함), 또는 BLK_MQ_NO_TAG(실패/NOWAIT).
 *
 * blk_mq_get_request()에서 호출되어 request에 고유한 tag를 부여한다.
 * fast-path: __blk_mq_get_tag()로 즉시 할당 시도.
 * slow-path: 실패 시 blk_mq_run_hw_queue()로 SQ doorbell → CQ 완료 유도 → 재시도.
 * sleep-path: 여전히 실패면 io_schedule()로 대기, tag 반납 시 wakeup 후 재시도.
 * sleep 중 CPU가 변경될 수 있어 매 루프에서 ctx/hctx/tags/bt를 재획득한다.
 * hctx가 변경되면 이전 hctx waiter를 가짜 wakeup하여 starvation을 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(블로킹 가능); NOWAIT이면 non-blocking.
 * 호출자: blk_mq_get_request() → blk_mq_submit_bio 경로.
 * 피호출자: __blk_mq_get_tag(), blk_mq_run_hw_queue(), io_schedule(),
 *           sbitmap_prepare_to_wait(), sbitmap_finish_wait(), blk_mq_put_tag().
 *
 * 호출 체인:
 *   blk_mq_submit_bio → __blk_mq_alloc_requests → [blk_mq_get_tag]
 *   → __blk_mq_get_tag / blk_mq_run_hw_queue / io_schedule
 * tag 를 손에 넣은 뒤에야 요청이 디스패치되어 mq_ops->queue_rq(간접 호출;
 * NVMe PCIe 라면 nvme_queue_rq)로 내려간다. 이 함수 자체는 드라이버를
 * 전혀 알지 못하며, 오직 sbitmap 에서 비트 하나를 얻어올 뿐이다.
 */
unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data)
{
	struct blk_mq_tags *tags = blk_mq_tags_from_data(data);	/* [한국어] 이 hctx의 tag pool 획득 — per-hctx 또는 shared */
	struct sbitmap_queue *bt;	/* [한국어] 실제 할당할 sbitmap_queue — 일반(bitmap_tags) 또는 예약(breserved_tags) */
	struct sbq_wait_state *ws;	/* [한국어] sbitmap wait state — tag 대기 큐 항목, 루프마다 hctx별로 재획득 */
	DEFINE_SBQ_WAIT(wait);		/* [한국어] 스택에 sbq_wait 구조체 선언+초기화 — tag 대기 시 waiter로 등록 */
	unsigned int tag_offset;	/* [한국어] 최종 tag = local_tag + tag_offset; 예약 영역 이후에 일반 영역이 위치하므로 필요 */
	int tag;			/* [한국어] sbitmap에서 획득한 local tag 값(0 ~ depth-1); BLK_MQ_NO_TAG이면 실패 */

	/* [한국어] 예약 요청(BLK_MQ_REQ_RESERVED)이면 breserved_tags에서, 일반이면 bitmap_tags에서 할당 */
	if (data->flags & BLK_MQ_REQ_RESERVED) {
		if (unlikely(!tags->nr_reserved_tags)) {
			WARN_ON_ONCE(1);	/* [한국어] 예약 tag 없이 REQ_RESERVED 요청 — 드라이버 설정 버그 */
			return BLK_MQ_NO_TAG;	/* [한국어] 예약 tag가 없으면 할당 불가 */
		}
		bt = &tags->breserved_tags;	/* [한국어] 예약 tag pool 선택 — flush, admin command 등 우선 처리 명령용 */
		tag_offset = 0;			/* [한국어] 예약 영역은 tag 0번부터 시작 — offset 보정 불필요 */
	} else {
		bt = &tags->bitmap_tags;	/* [한국어] 일반 IO tag pool 선택 */
		tag_offset = tags->nr_reserved_tags;	/* [한국어] 일반 요청의 전역 번호는 예약 영역 다음부터다.
						 * 예약/일반이 서로 다른 sbitmap 이지만 번호 공간은 하나로 이어 붙여
						 * tags->rqs[] 한 배열로 양쪽을 모두 인덱싱할 수 있게 만든다. */
	}

	tag = __blk_mq_get_tag(data, bt);	/* [한국어] fast-path: sbitmap에서 즉시 비트 하나 획득 시도 */
	if (tag != BLK_MQ_NO_TAG)		/* [한국어] BLK_MQ_NO_TAG(-1)가 아니면 성공. 부하가 낮을 때는 거의 항상 여기서 끝난다 */
		goto found_tag;			/* [한국어] 경합 없이 비트를 얻었다 — 대기 준비 없이 곧장 found_tag 로 */

	if (data->flags & BLK_MQ_REQ_NOWAIT)	/* [한국어] 잠들면 안 되는 호출자 — io_uring IOSQE_ASYNC 미사용 제출, O_NONBLOCK, 원자적 컨텍스트 등 */
		return BLK_MQ_NO_TAG;		/* [한국어] 아래 대기 루프에 진입하지 않고 즉시 실패. 호출자는 -EAGAIN 으로 되돌린다 */

	ws = bt_wait_ptr(bt, data->hctx);	/* [한국어] 이 hctx에 대응하는 sbitmap wait state 획득 — hctx별로 분산된 대기 큐 */
	do {
		struct sbitmap_queue *bt_prev;	/* [한국어] 이번 회차에 기다렸던 sbitmap 을 기억해 둔다.
						 * 잠든 사이 CPU 가 바뀌면(다른 CPU 에서 깨어나면) 매핑되는 hctx 가 달라져
						 * 대기할 sbitmap 자체가 바뀔 수 있다. 그때 이전 대기열에 남은 다른 waiter 를
						 * 깨워 주지 않으면, 반납된 tag 를 아무도 가져가지 않는 상황이 생긴다. */

		/*
		 * We're out of tags on this hardware queue, kick any
		 * pending IO submits before going to sleep waiting for
		 * some to complete.
		 */
		/* [한국어] tag 가 고갈됐다. 잠들기 전에 반드시 큐를 한 번 돌려야 한다.
		 * 이유: 소프트웨어 큐에 요청이 남은 채 아무도 디스패치하지 않았다면
		 * 장치는 할 일이 없어 완료를 만들지 않고, 완료가 없으니 tag 도 돌아오지
		 * 않는다. "내가 잠들면 나를 깨워 줄 사건이 영영 일어나지 않는" 교착이
		 * 성립한다. 여기서 밀어 넣어야 장치가 일을 시작하고 완료가 흘러나온다. */
		blk_mq_run_hw_queue(data->hctx, false);	/* [한국어] async=false — 워커에 미루지 않고 지금 이 컨텍스트에서 바로 디스패치한다.
						 * NVMe PCIe 라면 이 호출이 mq_ops->queue_rq(=nvme_queue_rq) → SQ 기록 →
						 * doorbell 로 이어지고, 그 완료가 blk_mq_put_tag 를 불러 나를 깨운다. */

		/*
		 * Retry tag allocation after running the hardware queue,
		 * as running the queue may also have found completions.
		 */
		tag = __blk_mq_get_tag(data, bt);	/* [한국어] 큐를 돌린 뒤, 그 사이 완료로 반납된 tag 가 있는지 다시 시도 */
		if (tag != BLK_MQ_NO_TAG)
			break;	/* [한국어] 재시도 성공 — sleep 없이 루프 탈출 */

		sbitmap_prepare_to_wait(bt, ws, &wait, TASK_UNINTERRUPTIBLE);
		/* [한국어] sleep 준비: waitqueue에 등록 후 task 상태를 UNINTERRUPTIBLE로 변경
		 * 이후 tag 반납 시 sbitmap_queue_wake_up()이 이 waiter를 깨움 */

		tag = __blk_mq_get_tag(data, bt);	/* [한국어] sleep 직전 마지막 재시도 — prepare_to_wait 이후 완료된 tag 놓치지 않도록 */
		if (tag != BLK_MQ_NO_TAG)
			break;	/* [한국어] 마지막 재시도 성공 — sbitmap_finish_wait로 정리 후 탈출 */

		bt_prev = bt;				/* [한국어] sleep 중 hctx가 변경될 수 있으므로 현재 bt 보관 */
		io_schedule();				/* [한국어] 실제로 잠드는 지점. 평범한 schedule() 이 아니라 io_schedule() 인 이유는
						 * 이 대기를 "IO 대기"로 회계 처리하기 위해서다 — 커널이 이 시간을 iowait 로
						 * 집계하고, 잠든 동안 CPU 가 깊은 절전 상태로 내려가지 않도록 힌트를 준다.
						 * 깨어나는 조건은 단 하나: 다른 요청이 완료되어 blk_mq_put_tag →
						 * sbitmap_queue_clear 가 이 waiter 를 깨우는 것. tag 는 완료 없이는 돌아오지 않는다. */

		sbitmap_finish_wait(bt, ws, &wait);	/* [한국어] 깨어난 후 waitqueue에서 waiter 제거, task 상태 정상화 */

		/* [한국어] sleep 동안 CPU 마이그레이션 가능 — 현재 CPU에 맞는 ctx/hctx/tags/bt를 재획득해야 함 */
		data->ctx = blk_mq_get_ctx(data->q);		/* [한국어] 현재 CPU의 software queue(blk_mq_ctx) 재획득 */
		data->hctx = blk_mq_map_queue(data->cmd_flags, data->ctx);	/* [한국어] CPU/IRQ affinity에 따른 NVMe hctx 재매핑 */
		tags = blk_mq_tags_from_data(data);		/* [한국어] 새 hctx의 tag pool 재조회 */
		if (data->flags & BLK_MQ_REQ_RESERVED)
			bt = &tags->breserved_tags;		/* [한국어] 예약 tag pool 재선택 */
		else
			bt = &tags->bitmap_tags;		/* [한국어] 일반 tag pool 재선택 */

		/*
		 * If destination hw queue is changed, fake wake up on
		 * previous queue for compensating the wake up miss, so
		 * other allocations on previous queue won't be starved.
		 */
		/* [한국어] hctx가 변경되어 bt가 달라진 경우, 이전 bt의 waiter 하나를 가짜 wakeup
		 * — 이전 hctx에서 기다리는 다른 submitter가 starvation되지 않도록 보상 */
		if (bt != bt_prev)
			sbitmap_queue_wake_up(bt_prev, 1);	/* [한국어] 이전 NVMe hctx의 대기자 1명 깨움 — wake-up miss 보상 */

		ws = bt_wait_ptr(bt, data->hctx);		/* [한국어] 새 hctx에 대응하는 sbitmap wait state 획득 */
	} while (1);

	sbitmap_finish_wait(bt, ws, &wait);			/* [한국어] 루프 성공 탈출 후 최종 waitqueue 정리 */

found_tag:
	/*
	 * Give up this allocation if the hctx is inactive.  The caller will
	 * retry on an active hctx.
	 */
	/* [한국어] tag 를 이미 얻었더라도 그 사이에 hctx 가 BLK_MQ_S_INACTIVE 로
	 * 바뀌었으면 즉시 반납하고 실패를 반환한다.
	 * INACTIVE 는 이 hctx 에 대응하던 CPU 가 오프라인되어(blk_mq_hctx_notify_offline)
	 * 더 이상 새 요청을 받으면 안 되는 상태다. 여기서 반납하지 않으면 아무도
	 * 완료시켜 주지 않는 tag 가 비트맵에 영원히 남아 풀이 조금씩 줄어든다.
	 * 확인 순서가 중요하다 — 먼저 tag 를 얻고 나서 검사한다. 반대로 하면
	 * "검사 통과 → (그 사이 INACTIVE 전환) → tag 획득" 경쟁이 열린다. */
	if (unlikely(test_bit(BLK_MQ_S_INACTIVE, &data->hctx->state))) {
		blk_mq_put_tag(tags, data->ctx, tag + tag_offset);	/* [한국어] 방금 얻은 tag 를 그대로 반납 — 반납에도 전역값(offset 포함)을 넘겨야 짝이 맞는다 */
		return BLK_MQ_NO_TAG;					/* [한국어] 호출자(__blk_mq_alloc_requests)는 살아 있는 hctx 로 다시 매핑해 재시도한다 */
	}
	return tag + tag_offset;	/* [한국어] 전역 tag = sbitmap 안에서의 지역 인덱스 + tag_offset.
					 * tag_offset 은 예약(reserved) 영역을 건너뛰기 위한 보정이다: 일반 요청은
					 * breserved_tags 다음 번호부터 시작하므로 nr_reserved_tags 를 더하고,
					 * 예약 요청(BLK_MQ_REQ_RESERVED)은 0번부터 쓰므로 보정이 0이다.
					 * 이렇게 두 비트맵을 하나의 연속 번호 공간으로 합쳐 놓기 때문에
					 * tags->rqs[tag] 인덱싱이 두 영역 모두에 그대로 통한다. */
}

/*
 * [한국어]
 * blk_mq_put_tag - 완료된 request의 tag를 sbitmap_queue에 반납한다.
 *
 * @tags: tag pool — 반납할 sbitmap_queue를 포함.
 * @ctx:  완료 처리 CPU의 blk_mq_ctx — sbitmap 내 CPU 힌트 제공으로 cache 효율 향상.
 * @tag:  반납할 전역 tag 값(tag_offset 포함). 예약 영역이면 nr_reserved_tags 미만.
 * @return: 없음.
 *
 * tag 값이 예약 영역인지(nr_reserved_tags 미만)에 따라 breserved_tags 또는
 * bitmap_tags 중 적절한 sbitmap 을 골라 비트를 클리어한다. 클리어 자체보다
 * 중요한 것은 그 뒤에 일어나는 wakeup 이다 — sbitmap_queue_clear() 내부가
 * 대기열의 waiter 를 깨우고, 그때서야 blk_mq_get_tag() 에서 잠들어 있던
 * 제출자가 다시 뛴다. 이 파일에서 tag 공급이 재개되는 유일한 지점이다.
 *
 * @ctx 를 받는 이유: sbitmap 은 해제된 비트의 위치를 해제한 CPU 쪽 힌트로
 * 기억해 둔다. 다음 할당이 같은 CPU 에서 일어나면 같은 워드를 다시 만나
 * 캐시 라인이 살아 있고, 다른 CPU 의 워드를 건드리지 않아 false sharing 도
 * 준다. 완료 CPU 와 제출 CPU 를 맞추는 것(NVMe 라면 MSI-X 인터럽트 어피니티)이
 * 성능에 영향을 주는 이유가 여기 있다.
 *
 * === NVMe PCIe 기준 실제 완료 경로 (drivers/nvme/host 에서 확인) ===
 *   nvme_irq → nvme_poll_cq → nvme_handle_cqe
 *     → nvme_find_rq: CQE 의 command_id 에서 gen/tag 를 분해하고 세대 검증,
 *                     blk_mq_tag_to_rq(tags, tag) 로 request 복원
 *     → nvme_try_complete_req → blk_mq_complete_request_remote
 *        (완료를 보고할 CPU 가 다르면 IPI 로 넘김)
 *     → mq_ops->complete = nvme_pci_complete_rq  (DMA unmap)
 *     → nvme_complete_rq (core) → blk_mq_end_request
 *     → blk_mq_put_driver_tag → [blk_mq_put_tag] → sbitmap_queue_clear
 * 주의: nvme_mq_ops 에 등록된 .complete 는 nvme_pci_complete_rq 이고,
 * core 의 nvme_complete_rq 는 그 안에서 불린다. 둘은 다른 함수다.
 *
 * 실행 컨텍스트: 장치 인터럽트 핸들러에서 시작하지만, 위 IPI/softirq 위임
 * 때문에 실제로 이 함수가 도는 컨텍스트는 구성에 따라 달라진다. 어느 쪽이든
 * 잠들 수 없는 컨텍스트일 수 있으므로 여기서 blocking 연산을 해서는 안 된다.
 * 피호출자: sbitmap_queue_clear() (lib/sbitmap.c).
 */
void blk_mq_put_tag(struct blk_mq_tags *tags, struct blk_mq_ctx *ctx,
		    unsigned int tag)
{
	if (!blk_mq_tag_is_reserved(tags, tag)) {	/* [한국어] tag < nr_reserved_tags이면 예약 영역 — 일반 영역(bitmap_tags)에 반납 */
		const int real_tag = tag - tags->nr_reserved_tags;
		/* [한국어] 전역 tag에서 예약 영역 offset 제거 → bitmap_tags 내 local 인덱스 계산 */

		BUG_ON(real_tag >= tags->nr_tags);	/* [한국어] local 인덱스가 일반 pool 범위 초과 — tag 손상 또는 double-put 등 치명적 버그 */
		sbitmap_queue_clear(&tags->bitmap_tags, real_tag, ctx->cpu);
		/* [한국어] 일반 tag pool의 해당 비트 클리어 → 이 tag를 다음 IO에 재사용 가능
		 * ctx->cpu 힌트로 같은 CPU의 캐시에서 처리하여 성능 향상 */
	} else {
		sbitmap_queue_clear(&tags->breserved_tags, tag, ctx->cpu);
		/* [한국어] 예약 tag pool 비트 클리어 → flush/admin 명령용 tag 반납
		 * 반납 직후 sbitmap 내부에서 대기 중인 waiter를 wakeup */
	}
}

/*
 * [한국어]
 * blk_mq_put_tags - 여러 tag를 한 번에 batch로 sbitmap_queue에 반납한다.
 *
 * @tags:      tag pool.
 * @tag_array: 반납할 전역 tag 값들의 배열(tag_offset 포함).
 * @nr_tags:   반납할 tag 수.
 * @return: 없음.
 *
 * NVMe CQ에서 한 인터럽트 핸들러 내에 여러 완료 엔트리를 처리한 후, 해당 tag 들을
 * 개별 sbitmap_queue_clear() 대신 단일 batch 호출로 일괄 클리어한다. cache line을
 * 여러 번 dirty하는 비용을 줄이고 waiter wakeup도 한 번에 처리한다.
 * 실행 컨텍스트: 하드웨어 인터럽트 또는 softirq(NVMe CQ 처리 경로).
 * 호출자: blk-mq 내부 다중 완료 경로.
 * 피호출자: sbitmap_queue_clear_batch() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   NVMe CQ 일괄 완료 → [blk_mq_put_tags]
 *   → sbitmap_queue_clear_batch
 */
void blk_mq_put_tags(struct blk_mq_tags *tags, int *tag_array, int nr_tags)
{
	sbitmap_queue_clear_batch(&tags->bitmap_tags, tags->nr_reserved_tags,
					tag_array, nr_tags);
	/* [한국어] 여러 tag를 한 번의 batch 연산으로 bitmap에 반납
	 * nr_reserved_tags를 base offset으로 전달해 local tag 인덱스 자동 보정
	 * — 개별 clear보다 cache 효율이 높고 waiter wakeup도 일괄 처리 */
}

/*
 * [한국어] struct bt_iter_data — bt_for_each() 순회 시 콜백 컨텍스트 전달 구조체.
 * sbitmap_for_each_set()의 void *data 인자로 전달되어 bt_iter()가 참조한다.
 */
struct bt_iter_data {
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 순회 대상 NVMe hardware queue(hctx).
	 * 설정자: bt_for_each() 호출자. 읽는 자: bt_iter() — rq->mq_hctx 필터링에 사용.
	 * NULL이면 q 기준으로만 필터링(shared tag 모드). 동기화: 읽기 전용 구조체. */

	struct request_queue *q;
	/* [한국어] 순회 대상 NVMe namespace의 request_queue.
	 * 설정자: bt_for_each() 호출자. 읽는 자: bt_iter() — rq->q와 비교하여 대상 namespace 필터링.
	 * shared tag 모드에서는 다른 namespace의 request도 같은 tag pool을 공유하므로 필수 필터. */

	busy_tag_iter_fn *fn;
	/* [한국어] 각 유효한 request에 대해 호출할 콜백 함수 포인터.
	 * 설정자: bt_for_each() 호출자. 읽는 자: bt_iter().
	 * 예: nvme_timeout에서 abort 처리, blk_mq_tagset_busy_iter의 사용자 콜백.
	 * @fn(rq, data) 형태로 호출; true 반환 시 순회 계속, false 반환 시 중단. */

	void *data;
	/* [한국어] 콜백 fn에 전달할 private 데이터.
	 * 설정자: bt_for_each() 호출자. 읽는 자: bt_iter()가 fn에 그대로 전달.
	 * 예: nvme_timeout 상태 구조체 포인터. */

	bool reserved;
	/* [한국어] 현재 순회 중인 pool이 예약 tag 영역(breserved_tags)인지 여부.
	 * true: breserved_tags 순회 — 예약 영역. 표준 NVMe PCIe 는 이 영역 크기가 0 이다.
	 * false: bitmap_tags 순회 — 일반 IO 영역. NVMe 에서 실제로 쓰이는 쪽은 거의 전부 이쪽이다.
	 * 설정자: bt_for_each(). 읽는 자: bt_iter() — bitnr offset 보정에 사용. */
};

/*
 * [한국어]
 * blk_mq_find_and_get_req - tag로 rqs[] 배열에서 request를 찾고 참조를 획득한다.
 *
 * @tags:  조회할 tag pool — rqs[] 배열 포함.
 * @bitnr: 전역 tag 값(tag_offset 포함) — sbitmap에서 set된 비트 번호.
 * @return: 유효한 struct request 포인터(참조 카운트 1 증가됨), NULL이면 유효하지 않음.
 *
 * sbitmap에서 set된 비트(사용 중인 tag)에 대응하는 struct request를 찾는다.
 * tag 할당 시 sbitmap bit가 먼저 set된 후 rqs[bitnr]에 request가 기록되므로,
 * 두 사이의 race로 rqs[bitnr]이 NULL이거나 rq->tag가 bitnr와 다를 수 있다.
 * req_ref_inc_not_zero()로 참조를 획득하여 caller가 사용하는 동안 request가
 * 해제되지 않도록 보장한다. 사용 후 반드시 blk_mq_put_rq_ref()로 해제해야 한다.
 * 실행 컨텍스트: bt_iter()/bt_tags_iter() 내부에서 호출; sbitmap 순회 컨텍스트.
 * 호출자: bt_iter(), bt_tags_iter().
 * 피호출자: req_ref_inc_not_zero() (atomic reference 연산).
 *
 * 호출 체인:
 *   sbitmap_for_each_set → bt_iter/bt_tags_iter → [blk_mq_find_and_get_req]
 */
static struct request *blk_mq_find_and_get_req(struct blk_mq_tags *tags,
		unsigned int bitnr)
{
	struct request *rq;

	rq = tags->rqs[bitnr];		/* [한국어] bitnr(= 전역 tag)로 rqs 배열 역조회 — 완료 보고에 실린 정수를 request 포인터로 되돌리는 그 동작 */
	if (!rq || rq->tag != bitnr || !req_ref_inc_not_zero(rq))
	/* [한국어] 세 가지 유효성 검사:
	 * 1) rq==NULL: bit set 직후 rqs[] 미기록 race 상태
	 * 2) rq->tag != bitnr: 이미 완료되어 다른 request가 같은 slot 재사용 중
	 * 3) req_ref_inc_not_zero 실패: request가 막 해제 중 — 참조 불가 */
		rq = NULL;		/* [한국어] 유효하지 않은 tag — 이 tag는 skip */
	return rq;			/* [한국어] 유효하면 참조 카운트 1 증가된 request 반환; NULL이면 skip */
}

/*
 * [한국어]
 * bt_iter - sbitmap의 각 set bit(사용 중인 tag)에 대해 콜백을 호출하는 sbitmap 순회 콜백.
 *
 * @bitmap: 현재 순회 중인 sbitmap (bitmap_tags.sb 또는 breserved_tags.sb).
 * @bitnr:  sbitmap 내 set된 비트 번호(local tag 인덱스).
 * @data:   struct bt_iter_data 포인터 — hctx, q, fn, data, reserved 포함.
 * @return: true이면 순회 계속, false이면 즉시 중단.
 *
 * sbitmap_for_each_set()에 의해 set된 비트마다 호출된다. shared/per-hctx tag pool을
 * 구분하여 올바른 blk_mq_tags를 선택하고, bitnr에 reserved offset을 보정한 뒤
 * blk_mq_find_and_get_req()로 request 참조를 획득한다. 필터링 조건(q, hctx)을
 * 통과한 request에만 콜백 fn을 호출하고, 반드시 blk_mq_put_rq_ref()로 참조 해제.
 * 실행 컨텍스트: sbitmap_for_each_set() 내에서 동기 호출; 프로세스 또는 softirq.
 * 호출자: sbitmap_for_each_set() ← bt_for_each() ← blk_mq_queue_tag_busy_iter().
 * 피호출자: blk_mq_find_and_get_req(), iter_data->fn(), blk_mq_put_rq_ref().
 *
 * 호출 체인:
 *   blk_mq_queue_tag_busy_iter → bt_for_each
 *   → sbitmap_for_each_set → [bt_iter]
 *   → blk_mq_find_and_get_req → fn(rq, data) → blk_mq_put_rq_ref
 */
static bool bt_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_iter_data *iter_data = data;
	struct blk_mq_hw_ctx *hctx = iter_data->hctx;	/* [한국어] 순회 대상 NVMe hctx — NULL이면 q 기준만 필터링 */
	struct request_queue *q = iter_data->q;		/* [한국어] NVMe namespace request_queue — namespace 필터링 기준 */
	struct blk_mq_tag_set *set = q->tag_set;	/* [한국어] NVMe controller의 tag_set — shared/per-hctx 모드 판단에 사용 */
	struct blk_mq_tags *tags;	/* [한국어] 이 비트가 속한 tag pool — shared 여부에 따라 바로 아래에서 결정 */
	struct request *rq;		/* [한국어] 비트에서 복원한 요청. 참조를 얻은 뒤에만 콜백에 넘긴다 */
	bool ret = true;		/* [한국어] 순회 계속 여부. 콜백이 false 를 주면 그대로 돌려보내 전체 순회를 끊는다 */

	if (blk_mq_is_shared_tags(set->flags))	/* [한국어] tag set 하나를 여러 request_queue 가 함께 쓰는 구성인지 */
		tags = set->shared_tags;	/* [한국어] 공유 구성 — pool 이 tag set 에 하나뿐이다.
					 * NVMe 로 치면 한 컨트롤러의 여러 네임스페이스(/dev/nvme0n1, n2 …)가
					 * 같은 하드웨어 큐를 쓰므로 tag 도 함께 나눠 쓰는 경우. */
	else
		tags = hctx->tags;		/* [한국어] 비공유 구성 — hctx(하드웨어 큐)마다 자기 pool 을 갖는다.
					 * NVMe PCIe 의 hctx 는 SQ/CQ 한 쌍에 대응한다. */

	if (!iter_data->reserved)
		bitnr += tags->nr_reserved_tags;
	/* [한국어] 일반 tag 순회 시 bitnr에 예약 영역 크기를 더해 전역 tag 값으로 변환
	 * — rqs[] 배열은 전역 tag(예약+일반 통합) 인덱스를 사용하므로 offset 보정 필수 */

	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assigning ->rqs[].
	 */
	rq = blk_mq_find_and_get_req(tags, bitnr);	/* [한국어] 전역 tag 로 rqs[] 역조회 및 참조 획득 — race로 NULL 가능 */
	if (!rq)
		return true;	/* [한국어] 유효한 request 없음(race 또는 미기록) — 다음 set bit로 진행 */

	if (rq->q == q && (!hctx || rq->mq_hctx == hctx))
	/* [한국어] 이중 필터링: 같은 NVMe namespace이고 지정 hctx에 속한 request만 콜백 대상
	 * — shared tag pool에서 다른 namespace의 request가 섞여있을 수 있으므로 필수 */
		ret = iter_data->fn(rq, iter_data->data);	/* [한국어] timeout/abort/complete 콜백 실행 */
	blk_mq_put_rq_ref(rq);		/* [한국어] blk_mq_find_and_get_req에서 증가시킨 참조 해제 — 반드시 호출해야 메모리 누수 없음 */
	return ret;			/* [한국어] fn이 false 반환하면 전체 순회 중단; true면 계속 */
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
 * [한국어]
 * bt_for_each - 주어진 sbitmap_queue의 모든 set bit를 순회하며 bt_iter를 호출한다.
 *
 * @hctx:     순회 대상 NVMe hardware queue; NULL이면 q 기준 필터링만 수행.
 * @q:        NVMe namespace의 request_queue.
 * @bt:       순회할 sbitmap_queue(bitmap_tags 또는 breserved_tags).
 * @fn:       각 유효한 request에 호출할 콜백.
 * @data:     fn에 전달할 private 데이터.
 * @reserved: true이면 예약 tag 영역 순회; false이면 일반 tag 영역.
 * @return: 없음.
 *
 * bt_iter_data 구조체로 컨텍스트를 패키징한 뒤 sbitmap_for_each_set()을 통해
 * bt->sb에서 set된 비트(사용 중인 tag)마다 bt_iter()를 호출한다. blk_mq_queue_tag_busy_iter()
 * 에서 shared/per-hctx 모드에 따라 적절한 bt와 hctx를 조합하여 호출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(timeout/reset 경로).
 * 호출자: blk_mq_queue_tag_busy_iter().
 * 피호출자: sbitmap_for_each_set() → bt_iter().
 *
 * 호출 체인:
 *   blk_mq_queue_tag_busy_iter → [bt_for_each]
 *   → sbitmap_for_each_set → bt_iter
 */
static void bt_for_each(struct blk_mq_hw_ctx *hctx, struct request_queue *q,
			struct sbitmap_queue *bt, busy_tag_iter_fn *fn,
			void *data, bool reserved)
{
	struct bt_iter_data iter_data = {
		.hctx = hctx,		/* [한국어] 순회 대상 NVMe hctx — bt_iter가 rq->mq_hctx와 비교 */
		.fn = fn,		/* [한국어] timeout/abort/complete 처리 콜백 */
		.data = data,		/* [한국어] 콜백 private(예: nvme_timeout 상태 구조체) */
		.reserved = reserved,	/* [한국어] 예약 tag 순회 여부 — bitnr offset 보정에 사용 */
		.q = q,			/* [한국어] NVMe namespace request_queue — rq->q 필터링 기준 */
	};

	sbitmap_for_each_set(&bt->sb, bt_iter, &iter_data);
	/* [한국어] bt->sb(sbitmap)에서 set된 bit(사용 중인 tag)를 하나씩 찾아 bt_iter 호출
	 * — 스캔 대상은 장치의 SQ 링이 아니라 blk-mq 의 tag 비트맵이다.
	 * 호스트는 장치 큐 내용을 읽지 않는다; "지금 발급되어 있는 tag" 집합이
	 * 곧 "인플라이트 요청" 집합이라는 등식으로 진행 중인 IO 를 파악한다 */
}

/*
 * [한국어] struct bt_tags_iter_data — bt_tags_for_each() 순회 시 콜백 컨텍스트 전달 구조체.
 * bt_iter_data와 달리 hctx/q 필터 없이 tag pool 전체를 대상으로 순회한다.
 */
struct bt_tags_iter_data {
	struct blk_mq_tags *tags;
	/* [한국어] 순회할 tag pool(blk_mq_tags).
	 * 설정자: bt_tags_for_each() 호출자. 읽는 자: bt_tags_iter().
	 * rqs[]/static_rqs[] 배열과 nr_reserved_tags 등을 담고 있음. */

	busy_tag_iter_fn *fn;
	/* [한국어] 각 유효한 request에 호출할 콜백 함수 포인터.
	 * 설정자: __blk_mq_all_tag_iter() 또는 bt_tags_for_each() 호출자.
	 * @fn(rq, data) 형태; true 반환 시 계속, false 반환 시 중단. */

	void *data;
	/* [한국어] fn에 전달할 private 데이터(예: 완료 카운터 포인터). */

	unsigned int flags;
	/* [한국어] BT_TAG_ITER_* 플래그 조합으로 순회 동작 제어.
	 * BT_TAG_ITER_RESERVED(1<<0): 현재 순회 중인 pool이 예약 tag 영역 — bitnr offset 보정 생략.
	 * BT_TAG_ITER_STARTED(1<<1): blk_mq_request_started()인 request만 콜백 대상.
	 * BT_TAG_ITER_STATIC_RQS(1<<2): rqs[] 대신 static_rqs[] 사용 — NVMe reset/초기화 중 안전한 순회. */
};

#define BT_TAG_ITER_RESERVED		(1 << 0)	/* [한국어] 예약 tag 영역(breserved_tags) 순회 중임을 표시 — bitnr offset 보정 제어 */
#define BT_TAG_ITER_STARTED		(1 << 1)	/* [한국어] NVMe에 실제 제출된(started) request만 순회 — abort/timeout 대상 선별 */
#define BT_TAG_ITER_STATIC_RQS		(1 << 2)	/* [한국어] static_rqs[] 사용 — rqs[]가 미설정인 초기화/reset 단계에서도 안전하게 순회 */

/*
 * [한국어]
 * bt_tags_iter - tag map 전체를 대상으로 각 set bit마다 콜백을 호출하는 sbitmap 순회 콜백.
 *
 * @bitmap: 순회 중인 sbitmap.
 * @bitnr:  sbitmap 내 set된 비트 번호(local tag 인덱스).
 * @data:   struct bt_tags_iter_data 포인터.
 * @return: true이면 계속, false이면 중단.
 *
 * bt_iter()와 달리 hctx/q 필터 없이 지정된 tag pool 전체를 대상으로 순회한다.
 * BT_TAG_ITER_STATIC_RQS 플래그가 설정된 경우 rqs[] 대신 static_rqs[]에서 직접
 * 참조하며(참조 카운트 불필요), 그렇지 않으면 blk_mq_find_and_get_req()로 안전하게
 * 참조를 획득한다. BT_TAG_ITER_STARTED 필터는 tag 만 잡아 두고 아직
 * mq_ops->queue_rq 로 내려가지 않은 요청을 걸러낸다 — 아직 장치가 알지도
 * 못하는 요청까지 취소·타임아웃 대상에 넣으면 안 되기 때문이다.
 * 실행 컨텍스트: sbitmap_for_each_set() 내에서 동기 호출.
 * 호출자: sbitmap_for_each_set() ← bt_tags_for_each() ← __blk_mq_all_tag_iter().
 * 피호출자: blk_mq_find_and_get_req(), blk_mq_request_started(), fn(), blk_mq_put_rq_ref().
 *
 * 호출 체인:
 *   __blk_mq_all_tag_iter → bt_tags_for_each
 *   → sbitmap_for_each_set → [bt_tags_iter] → fn(rq, data)
 */
static bool bt_tags_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_tags_iter_data *iter_data = data;
	struct blk_mq_tags *tags = iter_data->tags;	/* [한국어] 순회 대상 tag pool */
	struct request *rq;		/* [한국어] 이 비트에 대응하는 요청 */
	bool ret = true;		/* [한국어] 기본은 순회 계속 */
	bool iter_static_rqs = !!(iter_data->flags & BT_TAG_ITER_STATIC_RQS);
	/* [한국어] static_rqs[] 사용 여부 — NVMe reset/초기화 단계에서도 안전하게 접근하기 위한 플래그 */

	if (!(iter_data->flags & BT_TAG_ITER_RESERVED))
		bitnr += tags->nr_reserved_tags;
	/* [한국어] 일반 tag 영역 순회 시 예약 영역 크기를 더해 전역 tag 로 변환
	 * — RESERVED 플래그가 있으면 이미 breserved_tags 영역이므로 보정 불필요 */

	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assigning ->rqs[].
	 */
	if (iter_static_rqs)
		rq = tags->static_rqs[bitnr];
		/* [한국어] static_rqs[]는 사전 할당된 고정 배열 — rqs[]와 달리 참조 카운트 불필요
		 * NVMe controller 초기화/복구 단계에서도 안전하게 참조 가능 */
	else
		rq = blk_mq_find_and_get_req(tags, bitnr);
		/* [한국어] 동적 rqs[] 배열에서 tag에 해당하는 request를 참조 카운트와 함께 획득
		 * bit set 후 rqs[] 기록 전 race로 NULL 가능 */
	if (!rq)
		return true;	/* [한국어] 유효하지 않은 tag — 다음 set bit로 진행 */

	if (!(iter_data->flags & BT_TAG_ITER_STARTED) ||
	    blk_mq_request_started(rq))
	/* [한국어] 두 조건 중 하나: (1) STARTED 필터 없음 — 모든 active request 대상
	 *                           (2) STARTED 필터 있고 실제로 NVMe에 제출된 request */
		ret = iter_data->fn(rq, iter_data->data);	/* [한국어] timeout/abort/complete 콜백 실행 */
	if (!iter_static_rqs)
		blk_mq_put_rq_ref(rq);	/* [한국어] blk_mq_find_and_get_req로 증가시킨 참조 해제 — static_rqs 경로는 참조 획득 없으므로 제외 */
	return ret;			/* [한국어] true: 다음 tag 계속 순회, false: 전체 순회 즉시 중단 */
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
 * [한국어]
 * bt_tags_for_each - 특정 sbitmap_queue의 모든 active tag에 대해 bt_tags_iter를 호출한다.
 *
 * @tags:  순회할 tag pool(blk_mq_tags) — rqs[], static_rqs[], nr_reserved_tags 포함.
 * @bt:    순회할 sbitmap_queue(bitmap_tags 또는 breserved_tags).
 * @fn:    각 request에 호출할 콜백.
 * @data:  fn에 전달할 private 데이터.
 * @flags: BT_TAG_ITER_* 플래그 조합 — reserved/started/static_rqs 동작 제어.
 * @return: 없음.
 *
 * BT_TAG_ITER_STATIC_RQS 플래그가 설정된 경우 rqs[]가 NULL이어도 static_rqs[]로
 * 순회 가능하지만, 그 외에는 tags->rqs가 초기화된 경우에만 순회를 수행한다.
 * 실행 컨텍스트: __blk_mq_all_tag_iter() 내에서 호출; 프로세스 컨텍스트.
 * 호출자: __blk_mq_all_tag_iter().
 * 피호출자: sbitmap_for_each_set() → bt_tags_iter().
 *
 * 호출 체인:
 *   __blk_mq_all_tag_iter → [bt_tags_for_each]
 *   → sbitmap_for_each_set → bt_tags_iter
 */
static void bt_tags_for_each(struct blk_mq_tags *tags, struct sbitmap_queue *bt,
			     busy_tag_iter_fn *fn, void *data, unsigned int flags)
{
	struct bt_tags_iter_data iter_data = {
		.tags = tags,	/* [한국어] 순회할 tag pool */
		.fn = fn,	/* [한국어] timeout/abort/complete 처리 콜백 */
		.data = data,	/* [한국어] 콜백 private(예: 완료 카운터, abort 상태) */
		.flags = flags,	/* [한국어] BT_TAG_ITER_*: reserved/started/static_rqs 동작 제어 */
	};

	if (tags->rqs)	/* [한국어] 동적 rqs[] 배열이 초기화된 경우에만 순회 — 미초기화 시 NULL deref 방지 */
		sbitmap_for_each_set(&bt->sb, bt_tags_iter, &iter_data);
		/* [한국어] bt->sb에서 set된 비트(사용 중인 tag)마다 bt_tags_iter 호출
		 * — 마찬가지로 tag 비트맵을 훑는다. 타임아웃 처리나 컨트롤러 리셋 시
		 * "아직 안 끝난 요청 전부"를 찾아 실패 처리할 때 쓰는 경로다 */
}

/*
 * [한국어]
 * __blk_mq_all_tag_iter - 예약 tag 영역과 일반 tag 영역을 모두 순회하는 내부 공통 함수.
 *
 * @tags:  순회할 tag pool.
 * @fn:    각 request에 호출할 콜백.
 * @priv:  fn에 전달할 private 데이터.
 * @flags: BT_TAG_ITER_* 플래그(RESERVED 제외) — 호출자가 설정, 이 함수가 RESERVED 추가.
 * @return: 없음.
 *
 * 전체 tag 번호 공간(예약 영역 + 일반 영역)를 빠짐없이 커버하는 공통 순회 함수.
 * 상위에서 BT_TAG_ITER_RESERVED를 미리 지정하면 이 함수의 예약/일반 분기 로직이
 * 중복되어 혼란을 야기하므로 WARN_ON_ONCE로 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(blk_mq_tagset_busy_iter/blk_mq_all_tag_iter에서 호출).
 * 호출자: blk_mq_all_tag_iter(), blk_mq_tagset_busy_iter().
 * 피호출자: bt_tags_for_each().
 *
 * 호출 체인:
 *   blk_mq_tagset_busy_iter / blk_mq_all_tag_iter
 *   → [__blk_mq_all_tag_iter] → bt_tags_for_each (×2: reserved + normal)
 */
static void __blk_mq_all_tag_iter(struct blk_mq_tags *tags,
		busy_tag_iter_fn *fn, void *priv, unsigned int flags)
{
	WARN_ON_ONCE(flags & BT_TAG_ITER_RESERVED);
	/* [한국어] 호출자가 RESERVED를 미리 설정하면 안 됨 — 이 함수가 내부에서 RESERVED를 추가하여 분리 순회하므로 중복 금지 */

	if (tags->nr_reserved_tags)
		bt_tags_for_each(tags, &tags->breserved_tags, fn, priv,
				 flags | BT_TAG_ITER_RESERVED);
	/* [한국어] 예약 tag 영역(breserved_tags) 순회 — 표준 NVMe PCIe 에서는 크기 0 이라 대개 빈 순회
	 * RESERVED 플래그 추가로 bt_tags_iter가 bitnr offset 보정을 건너뜀 */
	bt_tags_for_each(tags, &tags->bitmap_tags, fn, priv, flags);
	/* [한국어] 일반 IO tag 영역(bitmap_tags) 순회 — 두 영역 합쳐 전체 tag 공간 커버 */
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
 * [한국어]
 * blk_mq_all_tag_iter - 특정 tag map의 모든 request를 static_rqs 포함하여 순회한다.
 *
 * @tags: 순회할 tag pool.
 * @fn:   각 request에 호출할 콜백.
 * @priv: fn에 전달할 private 데이터.
 * @return: 없음.
 *
 * BT_TAG_ITER_STATIC_RQS 플래그를 고정 전달하여 rqs[] 초기화 여부와 무관하게
 * static_rqs[]로 안전하게 접근한다. NVMe controller 초기화/reset 단계에서
 * 아직 rqs[]가 설정되지 않은 상태에서도 진행 중인 명령을 스캔할 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화/복구/진단 경로).
 * 호출자: NVMe 드라이버 진단/복구 경로 등.
 * 피호출자: __blk_mq_all_tag_iter().
 *
 * 호출 체인:
 *   (NVMe 초기화/복구 경로) → [blk_mq_all_tag_iter]
 *   → __blk_mq_all_tag_iter → bt_tags_for_each → bt_tags_iter
 */
void blk_mq_all_tag_iter(struct blk_mq_tags *tags, busy_tag_iter_fn *fn,
		void *priv)
{
	__blk_mq_all_tag_iter(tags, fn, priv, BT_TAG_ITER_STATIC_RQS);
	/* [한국어] STATIC_RQS 플래그: rqs[] 대신 static_rqs[] 사용
	 * — NVMe 초기화/reset 중에도 안전하게 전체 tag 공간 스캔 가능 */
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
 * [한국어]
 * blk_mq_tagset_busy_iter - tag set 전체에서 NVMe에 제출된(started) request를 순회한다.
 *
 * @tagset: 순회할 tag set. NVMe 라면 한 컨트롤러의 IO 큐 전체(또는 admin 큐)에 해당.
 * @fn:     각 started request에 호출할 콜백; true 반환 시 계속, false 반환 시 중단.
 * @priv:   fn에 전달할 private 데이터.
 * @return: 없음.
 *
 * "지금 진행 중인 요청 전부"를 열거하는 수단이다. 드라이버는 장치 큐를
 * 읽어서 이를 알아낼 수 없으므로, 발급된 tag 비트 집합을 훑는 이 함수에
 * 의존한다. shared_tags 모드에서는 tags[0] 하나만, per-hctx 모드에서는
 * nr_hw_queues 만큼 반복한다.
 * tags[] 배열은 __blk_mq_update_nr_hw_queues()가 큐 freeze 중에 갈아치우므로
 * srcu_read_lock() 으로 그 교체와의 경쟁을 막는다.
 *
 * NVMe 에서의 실제 사용처(drivers/nvme/host/core.c 확인):
 *   nvme_cancel_tagset(ctrl)       → blk_mq_tagset_busy_iter(ctrl->tagset,
 *                                       nvme_cancel_request, ctrl)
 *   nvme_cancel_admin_tagset(ctrl) → 같은 것을 admin_tagset 에 대해
 * 컨트롤러 리셋·연결 해제 시 "장치가 영영 완료해 주지 않을 요청들"을 전부
 * 찾아 강제로 실패 처리하는 용도다. 이어서 호출되는
 * blk_mq_tagset_wait_completed_request() 가 완료 콜백이 끝나기를 기다린다.
 * 흔한 오해와 달리 nvme_timeout() 이 이 함수를 부르지는 않는다 —
 * nvme_timeout 은 문제가 된 그 요청 하나만 다룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 콜백이 잠들 수 있어 RCU 가 아닌 SRCU 를 쓴다.
 * 피호출자: __blk_mq_all_tag_iter().
 *
 * 호출 체인:
 *   nvme_cancel_tagset → [blk_mq_tagset_busy_iter]
 *   → __blk_mq_all_tag_iter → bt_tags_for_each → bt_tags_iter → fn(rq, priv)
 */
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *tagset,
		busy_tag_iter_fn *fn, void *priv)
{
	unsigned int flags = tagset->flags;	/* [한국어] tag set 플래그 — shared tag pool 여부 판단 */
	int i, nr_tags, srcu_idx;	/* [한국어] i: 순회 중인 tags[] 인덱스, nr_tags: 훑을 pool 개수
					 * (shared 면 1, 아니면 nr_hw_queues), srcu_idx: unlock 에 돌려줄 값 */

	srcu_idx = srcu_read_lock(&tagset->tags_srcu);
	/* [한국어] SRCU(Sleepable RCU) read lock 획득 — tags[] 배열 보호
	 * __blk_mq_update_nr_hw_queues()가 queue freeze 중 tags[]를 갱신할 수 있으므로 race 방지 */

	nr_tags = blk_mq_is_shared_tags(flags) ? 1 : tagset->nr_hw_queues;
	/* [한국어] shared tag 모드: tags[0] 하나만 순회(모든 queue가 같은 tag pool 공유)
	 * per-hctx 모드: NVMe queue pair 수만큼 순회 */

	for (i = 0; i < nr_tags; i++) {
		if (tagset->tags && tagset->tags[i])	/* [한국어] tags 배열과 해당 tag pool이 유효한지 확인 — 초기화 중이거나 이미 해제된 경우 skip */
			__blk_mq_all_tag_iter(tagset->tags[i], fn, priv,
					      BT_TAG_ITER_STARTED);
		/* [한국어] STARTED 플래그: tag 만 잡아 두고 아직 드라이버로 내려가지 않은 요청은 제외하고,
		 * 실제로 mq_ops->queue_rq 를 통과한 것만 순회
		 * — 아직 dispatch 전인 request는 제외하여 false-positive abort 방지 */
	}
	srcu_read_unlock(&tagset->tags_srcu, srcu_idx);	/* [한국어] SRCU read lock 해제 — tags[] 보호 종료 */
}
EXPORT_SYMBOL(blk_mq_tagset_busy_iter);	/* [한국어] NVMe 드라이버에서 직접 호출하므로 외부에 공개 */

/*
 * [한국어]
 * blk_mq_tagset_count_completed_rqs - 완료됐지만 아직 정리 안 된 request를 카운트한다.
 *
 * @rq:   blk_mq_tagset_busy_iter가 순회 중 발견한 started request.
 * @data: unsigned int 포인터 — 완료 대기 중인 request 수를 누적할 카운터.
 * @return: 항상 true — 모든 started request를 끝까지 순회.
 *
 * blk_mq_tagset_wait_completed_request()의 콜백으로 사용된다.
 * NVMe CQ가 완료를 보고했지만 blk-mq 레이어의 completion 처리(blk_mq_complete_request)가
 * 아직 끝나지 않은 request를 카운트하여, 드레인 완료 여부를 판단한다.
 * 실행 컨텍스트: blk_mq_tagset_busy_iter() 내에서 동기 호출; 프로세스 컨텍스트.
 * 호출자: blk_mq_tagset_busy_iter() ← blk_mq_tagset_wait_completed_request().
 *
 * 호출 체인:
 *   blk_mq_tagset_wait_completed_request → blk_mq_tagset_busy_iter
 *   → __blk_mq_all_tag_iter → bt_tags_iter → [blk_mq_tagset_count_completed_rqs]
 */
static bool blk_mq_tagset_count_completed_rqs(struct request *rq, void *data)
{
	unsigned *count = data;	/* [한국어] 완료됐지만 미정리 request 수를 기록할 카운터 포인터 */

	if (blk_mq_request_completed(rq))	/* [한국어] NVMe CQ 완료로 표시됐지만 blk-mq completion work가 아직 미완료인 request */
		(*count)++;			/* [한국어] 드레인 대기 중인 request 수 누적 */
	return true;				/* [한국어] 항상 true — 전체 tag space를 빠짐없이 스캔 */
}

/**
 * blk_mq_tagset_wait_completed_request - Wait until all scheduled request
 * completions have finished.
 * @tagset:	Tag set to drain completed request
 *
 * Note: This function has to be run after all IO queues are shutdown
 */
/*
 * [한국어]
 * blk_mq_tagset_wait_completed_request - CQ 완료된 request의 소프트웨어 정리가 끝날 때까지 폴링한다.
 *
 * @tagset: 드레인 대상 tag set.
 * @return: 없음.
 *
 * NVMe controller 제거 또는 reset 시 queue shutdown 이후 호출된다. NVMe CQ가 완료를
 * 보고했지만 blk-mq 레이어의 completion work(blk_mq_complete_request 등)가 아직
 * 소프트웨어적으로 처리되지 않은 request가 남아있는 동안 5ms 간격으로 폴링한다.
 * 모든 completion이 끝나야 queue와 tag pool을 안전하게 해제할 수 있다.
 * 반드시 모든 IO 큐가 shutdown된 후에 호출해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(NVMe remove/reset 경로); sleep 가능.
 * 호출자: NVMe 드라이버 remove/reset 경로.
 * 피호출자: blk_mq_tagset_busy_iter(), msleep().
 *
 * 호출 체인:
 *   nvme_remove / nvme_reset_work → [blk_mq_tagset_wait_completed_request]
 *   → blk_mq_tagset_busy_iter → blk_mq_tagset_count_completed_rqs
 */
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *tagset)
{
	while (true) {
		unsigned count = 0;	/* [한국어] 이번 폴링에서 발견된 미완료 completion 수 */

		blk_mq_tagset_busy_iter(tagset,
				blk_mq_tagset_count_completed_rqs, &count);
		/* [한국어] NVMe 전체 queue pair에서 CQ 완료됐지만 소프트웨어 정리 미완 request 카운트 */
		if (!count)
			break;	/* [한국어] 모든 completion 처리 완료 — tag pool/queue 해제 가능 */
		msleep(5);	/* [한국어] 아직 미완료 completion 존재 — 5ms 대기 후 재폴링; NVMe ISR/completion work 소진 기다림 */
	}
}
EXPORT_SYMBOL(blk_mq_tagset_wait_completed_request);	/* [한국어] NVMe 드라이버에서 직접 호출하므로 외부 공개 */

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
 * [한국어]
 * blk_mq_queue_tag_busy_iter - 특정 request_queue에서 driver tag 보유 request를 순회한다.
 *
 * @q:    순회 대상 NVMe namespace의 request_queue.
 * @fn:   각 request에 호출할 콜백(timeout/abort 처리 등).
 * @priv: fn에 전달할 private 데이터.
 * @return: 없음.
 *
 * NVMe namespace 단위 timeout/abort 처리에서 해당 queue의 모든 진행 중인 IO를
 * 스캔한다. shared_tags 모드에서는 같은 tag_set을 공유하는 다른 namespace의
 * request도 함께 순회되므로 bt_iter() 내부의 rq->q 필터링이 필수이다.
 * q_usage_counter로 queue 제거 중인 경우를 차단하고, SRCU로 tags[] 배열 변경과의
 * race를 방지한다. per-hctx 모드에서는 CPU/IRQ affinity에 매핑된 hctx만 순회한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(timeout 워크큐, abort 경로 등).
 * 호출자: blk-mq timeout/abort 처리, NVMe 드라이버.
 * 피호출자: bt_for_each().
 *
 * 호출 체인:
 *   NVMe timeout/abort → [blk_mq_queue_tag_busy_iter]
 *   → bt_for_each → sbitmap_for_each_set → bt_iter → fn(rq, priv)
 */
void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_tag_iter_fn *fn,
		void *priv)
{
	int srcu_idx;			/* [한국어] srcu_read_unlock 에 되돌려 줄 인덱스 */

	/*
	 * __blk_mq_update_nr_hw_queues() updates nr_hw_queues and queue_hw_ctx
	 * while the queue is frozen. So we can use q_usage_counter to avoid
	 * racing with it.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter))
	/* [한국어] q_usage_counter 참조 획득 시도 — queue 제거 중(percpu_ref가 killed 상태)이면 0 반환하여 즉시 종료
	 * __blk_mq_update_nr_hw_queues()와의 race도 이 참조로 방지 */
		return;

	srcu_idx = srcu_read_lock(&q->tag_set->tags_srcu);
	/* [한국어] SRCU read lock — 순회 도중 __blk_mq_update_nr_hw_queues() 가 tags 를
	 * 교체·해제하는 것을 막는다. 해제는 call_srcu 로 미뤄지므로 이 구간 안에서는
	 * 포인터 유효성이 보장된다. RCU 가 아니라 SRCU 인 이유는 콜백 fn 이 잠들 수 있어서다. */
	if (blk_mq_is_shared_tags(q->tag_set->flags)) {
		struct blk_mq_tags *tags = q->tag_set->shared_tags;	/* [한국어] shared tag pool — 여러 NVMe namespace가 공유 */
		struct sbitmap_queue *bresv = &tags->breserved_tags;	/* [한국어] shared 예약 tag pool */
		struct sbitmap_queue *btags = &tags->bitmap_tags;	/* [한국어] shared 일반 tag pool */

		if (tags->nr_reserved_tags)
			bt_for_each(NULL, q, bresv, fn, priv, true);
			/* [한국어] shared tag set 의 예약 영역 순회; hctx=NULL이면 q 기준만 필터링 */
		bt_for_each(NULL, q, btags, fn, priv, false);
		/* [한국어] shared 일반 tag 순회 — 동일 tag_set 공유 다른 namespace request도 포함되므로 bt_iter의 rq->q 필터 필수 */
	} else {
		struct blk_mq_hw_ctx *hctx;
		unsigned long i;

		queue_for_each_hw_ctx(q, hctx, i) {
		/* [한국어] NVMe queue pair(hctx)를 하나씩 순회 — SQ/CQ 쌍 단위 스캔 */
			struct blk_mq_tags *tags = hctx->tags;		/* [한국어] 이 hctx의 per-hctx tag pool */
			struct sbitmap_queue *bresv = &tags->breserved_tags;	/* [한국어] per-hctx 예약 tag pool */
			struct sbitmap_queue *btags = &tags->bitmap_tags;	/* [한국어] per-hctx 일반 tag pool */

			/*
			 * If no software queues are currently mapped to this
			 * hardware queue, there's nothing to check
			 */
			if (!blk_mq_hw_queue_mapped(hctx))
				continue;	/* [한국어] CPU/IRQ affinity에 매핑된 software queue가 없는 hctx는 skip — 실제 IO가 없는 NVMe queue pair */

			if (tags->nr_reserved_tags)
				bt_for_each(hctx, q, bresv, fn, priv, true);	/* [한국어] hctx 별 예약 영역 순회 */
			bt_for_each(hctx, q, btags, fn, priv, false);	/* [한국어] per-hctx 일반 tag 순회 — timeout/abort 대상 request 수집 */
		}
	}
	srcu_read_unlock(&q->tag_set->tags_srcu, srcu_idx);	/* [한국어] SRCU read lock 해제 — tags[] 보호 종료 */
	blk_queue_exit(q);					/* [한국어] q_usage_counter 참조 해제 — NVMe namespace 제거 진행 허용 */
}

/*
 * [한국어]
 * bt_alloc - sbitmap_queue를 할당·초기화하여 지정된 depth만큼 tag를 관리하게 한다.
 *
 * @bt:          초기화할 sbitmap_queue 포인터.
 * @depth:       이 sbitmap 이 관리할 비트 수. NVMe 라면 결국 SQ 깊이와 같은 값이 되지만,
 *               의미는 "동시에 발급 가능한 tag 수"다.
 * @round_robin: true이면 bit 검색 시 CPU 간 round-robin — BLK_MQ_F_TAG_RR 플래그.
 * @node:        NUMA 노드 번호 — 이 노드의 로컬 메모리에 bitmap 할당.
 * @return: 0(성공), 음수(실패) — blk_mq_init_tags가 오류 경로 처리.
 *
 * blk_mq_init_tags()에서 bitmap_tags와 breserved_tags 각각에 대해 한 번씩 호출된다.
 * sbitmap_queue_init_node()는 bit 수(depth)에 맞는 비트맵 메모리를 NUMA 로컬로
 * 할당하고 wait queue 등 sbitmap_queue 내부 구조를 초기화한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(NVMe queue pair 생성 시); GFP_KERNEL 할당 가능.
 * 호출자: blk_mq_init_tags().
 * 피호출자: sbitmap_queue_init_node() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   blk_mq_init_tags → [bt_alloc] → sbitmap_queue_init_node
 */
static int bt_alloc(struct sbitmap_queue *bt, unsigned int depth,
		    bool round_robin, int node)
{
	return sbitmap_queue_init_node(bt, depth, -1, round_robin, GFP_KERNEL,	/* [한국어] 세 번째 인자 -1 은 shift 자동 계산 요청.
									 * sbitmap 이 depth 와 CPU 수를 보고 워드당 비트 수를 정한다.
									 * 너무 촘촘하면 CPU 들이 같은 워드를 두고 다투고, 너무 성기면 메모리를 낭비한다. */
				       node);					/* [한국어] NUMA 노드 — 비트맵을 해당 노드 로컬 메모리에 둔다.
									 * NVMe 는 nvme_alloc_io_tag_set 이 ctrl->numa_node 를 넘기므로
									 * 결국 그 장치가 붙은 PCIe 루트에 가까운 메모리가 선택된다. */
	/* [한국어] sbitmap_queue 초기화:
	 * depth: 이 sbitmap 이 관리할 비트 수 = 이 큐의 동시 진행 상한
	 * -1: shift 자동 계산(depth에 맞는 최적 bit 폭)
	 * round_robin: CPU 간 시작 위치 분산으로 cache 편중 완화
	 * GFP_KERNEL: 슬립 허용 메모리 할당
	 * node: NUMA 로컬 할당으로 접근 지연 최소화 */
}

/*
 * [한국어]
 * blk_mq_init_tags - blk_mq_tags 구조체를 할당하고 일반/예약 tag sbitmap을 초기화한다.
 *
 * @total_tags:    전체 tag 수(예약 + 일반).
 * @reserved_tags: 예약 영역 크기. NVMe PCIe 는 0, fabrics 는 connect 용 1, Apple(SHARED_TAGS) 만 admin 깊이만큼 잡는다.
 * @flags:         tag set 플래그 — BLK_MQ_F_TAG_RR(round-robin) 포함.
 * @node:          NUMA 노드 번호.
 * @return:        초기화된 blk_mq_tags 포인터, 실패 시 NULL.
 *
 * NVMe queue pair 생성 시(blk_mq_alloc_map_and_rqs) 호출된다.
 * total_tags - reserved_tags만큼 bitmap_tags(일반 영역)를, reserved_tags만큼
 * breserved_tags(예약 영역)를 별도로 초기화하여 두 영역을 독립적으로 관리한다.
 * BLK_MQ_TAG_MAX 초과 시 NVMe 드라이버 설정 오류로 판단하고 즉시 실패 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(NVMe probe/init 경로); GFP_KERNEL 슬립 가능.
 * 호출자: blk_mq_alloc_map_and_rqs().
 * 피호출자: kzalloc_node(), bt_alloc(), sbitmap_queue_free(), kfree().
 *
 * 호출 체인:
 *   blk_mq_alloc_map_and_rqs → [blk_mq_init_tags]
 *   → bt_alloc → sbitmap_queue_init_node
 */
struct blk_mq_tags *blk_mq_init_tags(unsigned int total_tags,
		unsigned int reserved_tags, unsigned int flags, int node)
{
	unsigned int depth = total_tags - reserved_tags;	/* [한국어] 일반 영역 크기 = 전체 tag 수 − 예약 영역 크기 */
	bool round_robin = flags & BLK_MQ_F_TAG_RR;		/* [한국어] CPU 간 round-robin 탐색 시작점 사용 여부 — 편중 방지 */
	struct blk_mq_tags *tags;			/* [한국어] 이번에 만들 tag pool */

	if (total_tags > BLK_MQ_TAG_MAX) {		/* [한국어] blk-mq 절대 상한 검사. NVMe 는 여기 닿기 전에 이미 두 번 잘린다:
							 * pci.c 가 io_queue_depth 를 4095 로, core 가 BLK_MQ_MAX_DEPTH-1 로. */
		pr_err("blk-mq: tag depth too large\n");	/* [한국어] NVMe controller의 SQ depth가 커널 지원 최대치 초과 — 드라이버 설정 오류 */
		return NULL;				/* [한국어] 초기화 실패 반환 — NVMe queue pair 생성 중단 */
	}

	tags = kzalloc_node(sizeof(*tags), GFP_KERNEL, node);	/* [한국어] 0 초기화 — 명시적으로 채우지 않는 필드(lock, active_queues 등)가
							 * 0 을 올바른 초깃값으로 갖는다. _node 판을 쓰는 이유는 이 구조체가
							 * 제출·완료 핫패스에서 매번 접근되어 원격 노드 접근을 피해야 하기 때문이다. */
	/* [한국어] NUMA 로컬 메모리로 blk_mq_tags 구조체 할당(zeroed)
	 * node 로컬 할당으로 NVMe 컨트롤러가 위치한 NUMA 노드와 메모리 친화성 유지 */
	if (!tags)
		return NULL;	/* [한국어] 메모리 할당 실패 */

	tags->nr_tags = total_tags;		/* [한국어] 이 tag set 이 발급할 수 있는 tag 총수 (예약 + 일반) */
	tags->nr_reserved_tags = reserved_tags;	/* [한국어] 예약 tag 수 설정 = flush/admin 우선 명령용 */
	spin_lock_init(&tags->lock);		/* [한국어] active_queues/wake_batch 보호 스핀락 초기화 */
	INIT_LIST_HEAD(&tags->page_list);	/* [한국어] request pool 페이지 리스트 초기화 — blk_mq_alloc_rqs에서 할당된 페이지들이 여기 연결됨 */

	if (bt_alloc(&tags->bitmap_tags, depth, round_robin, node))
		/* [한국어] 일반 IO tag pool sbitmap 초기화 — 실패 시 메모리 정리 경로로 */
		goto out_free_tags;
	if (bt_alloc(&tags->breserved_tags, reserved_tags, round_robin, node))
		/* [한국어] 예약 tag pool sbitmap 초기화 — 실패 시 bitmap_tags도 함께 정리 */
		goto out_free_bitmap_tags;

	return tags;	/* [한국어] 두 sbitmap 과 rqs 배열까지 준비 완료 — 이제 이 pool 로 tag 발급이 가능하다 */

out_free_bitmap_tags:
	sbitmap_queue_free(&tags->bitmap_tags);	/* [한국어] 이미 성공한 일반 tag bitmap 해제 */
out_free_tags:
	kfree(tags);	/* [한국어] tags 구조체 해제 — NVMe queue pair 생성 실패 */
	return NULL;
}

/*
 * [한국어]
 * blk_mq_free_tags_callback - SRCU grace period 후 request pool 페이지와 tags 구조체를 해제한다.
 *
 * @head: struct blk_mq_tags의 rcu_head 필드 포인터 — call_srcu()가 전달.
 * @return: 없음.
 *
 * blk_mq_free_tags()에서 call_srcu()로 등록된 SRCU 콜백이다. SRCU grace period가
 * 지나면 진행 중인 모든 blk_mq_tagset_busy_iter() 등의 읽기 측이 완료된 것이므로
 * tags->page_list에 연결된 request pool 페이지들을 안전하게 해제한다.
 * page->private에 order가 저장되어 있어 __free_pages()로 올바른 크기를 반납한다.
 * kmemleak_free()는 blk_mq_alloc_rqs()에서 kmemleak_alloc()으로 등록한 객체를
 * 추적에서 제거하여 false positive 메모리 누수 경고를 방지한다.
 * 실행 컨텍스트: SRCU 콜백 — softirq 또는 work queue 컨텍스트.
 * 호출자: call_srcu() ← blk_mq_free_tags().
 * 피호출자: list_first_entry(), list_del_init(), kmemleak_free(), __free_pages(), kfree().
 *
 * 호출 체인:
 *   blk_mq_free_tags → call_srcu → [blk_mq_free_tags_callback]
 *   → __free_pages (page_list 소진) → kfree(tags)
 */
static void blk_mq_free_tags_callback(struct rcu_head *head)
{
	struct blk_mq_tags *tags = container_of(head, struct blk_mq_tags,
						rcu_head);
	/* [한국어] rcu_head로부터 blk_mq_tags 구조체 포인터 복원 — container_of 패턴 */
	struct page *page;

	while (!list_empty(&tags->page_list)) {
	/* [한국어] page_list에 blk_mq_alloc_rqs()가 할당한 request pool 페이지들이 연결됨 */
		page = list_first_entry(&tags->page_list, struct page, lru);
		/* [한국어] 리스트 첫 번째 페이지 획득 — lru 필드로 page_list에 연결됨 */
		list_del_init(&page->lru);	/* [한국어] page_list에서 제거 및 초기화 */
		/*
		 * Remove kmemleak object previously allocated in
		 * blk_mq_alloc_rqs().
		 */
		kmemleak_free(page_address(page));
		/* [한국어] blk_mq_alloc_rqs()의 kmemleak_alloc()으로 등록된 추적 객체 제거
		 * — 해제 후 false positive 메모리 누수 경고 방지 */
		__free_pages(page, page->private);
		/* [한국어] request pool 페이지 반납; page->private에 저장된 order로 정확한 크기 해제
		 * — NVMe request(+PRP/SGL 버퍼 등) 메모리 반환 */
	}
	kfree(tags);	/* [한국어] page_list 소진 후 blk_mq_tags 구조체 자체 해제 — NVMe queue pair 메모리 정리 완료 */
}

/*
 * [한국어]
 * blk_mq_free_tags - tag sbitmap 을 해제하고 필요 시 SRCU를 통해 tags를 지연 해제한다.
 *
 * @set:  tag set — SRCU 보호 도메인(tags_srcu)을 제공.
 * @tags: 해제할 blk_mq_tags(tag pool).
 * @return: 없음.
 *
 * NVMe controller 제거 또는 queue pair 재구성 시(blk_mq_free_map_and_rqs) 호출된다.
 * sbitmap 자체는 즉시 해제하지만, page_list에 request pool 페이지가 남아있으면
 * blk_mq_tagset_busy_iter() 등이 여전히 tags를 참조할 수 있으므로 SRCU grace period
 * 이후 blk_mq_free_tags_callback()을 통해 안전하게 해제한다.
 * page_list가 비어있으면(IO가 전혀 없었거나 초기화 실패 복구 경로) 즉시 kfree.
 * 실행 컨텍스트: 프로세스 컨텍스트(NVMe remove/reset 경로).
 * 호출자: blk_mq_free_map_and_rqs().
 * 피호출자: sbitmap_queue_free(), kfree(), call_srcu().
 *
 * 호출 체인:
 *   blk_mq_free_map_and_rqs → [blk_mq_free_tags]
 *   → sbitmap_queue_free (×2) → call_srcu → blk_mq_free_tags_callback
 */
void blk_mq_free_tags(struct blk_mq_tag_set *set, struct blk_mq_tags *tags)
{
	sbitmap_queue_free(&tags->bitmap_tags);		/* [한국어] 일반 tag sbitmap 해제 — bitmap 메모리와 wait queue 정리 */
	sbitmap_queue_free(&tags->breserved_tags);	/* [한국어] 예약 tag sbitmap 해제 — flush/admin tag pool 정리 */

	/* if tags pages is not allocated yet, free tags directly */
	if (list_empty(&tags->page_list)) {
		/* [한국어] request pool 페이지가 없으면(IO 전혀 없었거나 초기화 실패 복구 경로)
		 * SRCU 지연 없이 즉시 해제 가능 */
		kfree(tags);	/* [한국어] tags 구조체 즉시 해제 */
		return;
	}

	call_srcu(&set->tags_srcu, &tags->rcu_head, blk_mq_free_tags_callback);
	/* [한국어] SRCU grace period 후 blk_mq_free_tags_callback 호출 등록
	 * — blk_mq_tagset_busy_iter() 등 읽기 측이 모두 완료된 후에야 page_list 페이지 해제
	 * set->tags_srcu: 이 tag set의 SRCU 도메인, rcu_head: 콜백 연결 고리 */
}

/*
 * [한국어]
 * blk_mq_tag_resize_shared_tags - shared tag pool 의 tag 수를 동적으로 재조정한다.
 *
 * @set:  shared_tags를 보유한 tag set.
 * @size: 새로운 전체 tag 수(예약 포함).
 * @return: 없음.
 *
 * NVMe 장치의 SQ queue depth가 런타임에 변경될 때(예: 장치 성능 상태 전환,
 * APST 등) shared tag pool의 일반 영역 bitmap 크기를 재조정한다.
 * 예약 tag 수(reserved_tags)는 불변이므로 새 depth에서 이를 뺀 값으로 조정한다.
 * sbitmap_queue_resize()는 내부적으로 기존 할당된 tag에 영향을 주지 않으면서
 * 가용 슬롯 수만 변경한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(queue depth 변경 경로).
 * 호출자: blk_mq_update_nr_requests() 또는 태그셋 depth 변경 경로.
 * 피호출자: sbitmap_queue_resize() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   blk_mq_update_nr_requests → [blk_mq_tag_resize_shared_tags]
 *   → sbitmap_queue_resize
 */
void blk_mq_tag_resize_shared_tags(struct blk_mq_tag_set *set, unsigned int size)
{
	struct blk_mq_tags *tags = set->shared_tags;	/* [한국어] 여러 NVMe queue/namespace가 공유하는 tag pool */

	sbitmap_queue_resize(&tags->bitmap_tags, size - set->reserved_tags);
	/* [한국어] 일반 tag pool 크기 재조정: 새 depth(size)에서 예약 tag 수를 빼 일반 영역만 변경
	 * — 예약 tag 는 breserved_tags에 고정이므로 bitmap_tags만 조정 */
}

/*
 * [한국어]
 * blk_mq_tag_update_sched_shared_tags - IO 스케줄러용 shared tag pool 크기를 재조정한다.
 *
 * @q:  대상 request_queue.
 * @nr: 새로운 요청 수(예약 포함) — blk_mq_update_nr_requests에서 산출.
 * @return: 없음.
 *
 * NVMe queue에 IO 스케줄러(mq-deadline, BFQ 등)가 연결된 상태에서 shared tag pool을
 * 사용할 때, 스케줄러 내부용 tag pool(sched_shared_tags)의 일반 영역 크기를 조정한다.
 * sched_shared_tags는 tag와는 별도의 스케줄러 내부 request 추적용 tag pool이며,
 * driver tag와 독립적으로 관리된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(nr_requests sysfs 쓰기 등).
 * 호출자: blk_mq_update_nr_requests().
 * 피호출자: sbitmap_queue_resize() (lib/sbitmap.c).
 *
 * 호출 체인:
 *   blk_mq_update_nr_requests → [blk_mq_tag_update_sched_shared_tags]
 *   → sbitmap_queue_resize
 */
void blk_mq_tag_update_sched_shared_tags(struct request_queue *q,
					 unsigned int nr)
{
	sbitmap_queue_resize(&q->sched_shared_tags->bitmap_tags,
			     nr - q->tag_set->reserved_tags);
	/* [한국어] IO 스케줄러 shared tag pool 일반 영역 크기 재조정
	 * nr: 새 전체 요청 수, reserved_tags 제외한 값이 스케줄러 일반 tag 수
	 * — NVMe driver tag와 별개; 스케줄러 내부 request 순서 관리에 사용 */
}

/**
 * blk_mq_unique_tag() - return a tag that is unique queue-wide
 * [한국어] blk_mq_unique_tag - queue 전체에서 고유한 tag 값을 반환한다.
 *
 * 상위 비트에 hctx->queue_num(NVMe queue pair 번호)을,
 * 하위 BLK_MQ_UNIQUE_TAG_BITS 비트에 per-queue tag를 결합하여
 * controller 전체에서 유일한 64비트 식별자를 생성한다.
 * 호출자: 디버깅, tracing, 또는 태그를 전역적으로 식별해야 하는 경로.
 * 피호출자: (없음, 단순 비트 연산).
 *
 * 호출 체인: 외부 호출자 → [blk_mq_unique_tag]
 */
u32 blk_mq_unique_tag(struct request *rq)
{
	return (rq->mq_hctx->queue_num << BLK_MQ_UNIQUE_TAG_BITS) |
	/* [한국어] NVMe queue pair 번호(queue_num)를 상위 비트에 배치 — controller 내 여러 SQ/CQ 쌍 구분 */
		(rq->tag & BLK_MQ_UNIQUE_TAG_MASK);
	/* [한국어] 하위 비트는 per-queue tag (NVMe 라면 CID 의 하위 12비트에 해당) — BLK_MQ_UNIQUE_TAG_MASK로 마스킹 */
}
EXPORT_SYMBOL(blk_mq_unique_tag);	/* [한국어] 드라이버/트레이싱 도구에서 직접 호출하므로 외부 공개 */


/*
 * [한국어] ===================================================================
 * NVMe 관점 핵심 요약 (blk-mq-tag.c)
 * ============================================================================
 * 아래는 전부 이 트리의 drivers/nvme/host/ 소스에서 직접 확인한 내용이다.
 *
 * 1) tag 와 Command ID 는 같은 것이 아니다.
 *      nvme_cid(rq) = (genctr & 0xf) << 12 | rq->tag      [nvme.h]
 *    하위 12비트만 이 파일이 발급한 tag 다. 상위 4비트 세대 니블은 tag 가
 *    재사용된 뒤 뒤늦게 도착한 CQE 를 걸러내는 용도이며, nvme_find_rq() 가
 *    불일치를 감지하면 그 완료를 버린다. tag 재사용이 안전한 이유가 이것이다.
 *
 * 2) 그 12비트가 NVMe 큐 깊이 상한의 실제 근거다.
 *      #define NVME_PCI_MAX_QUEUE_SIZE 4095      [pci.c]  (= 0xfff)
 *      io_queue_depth 기본값 1024, 유효 범위 [2, 4095]
 *      dev->q_depth = min(CAP.MQES + 1, io_queue_depth)
 *    장치가 MQES 로 더 깊은 큐를 광고해도 드라이버가 4095 에서 자른다.
 *
 * 3) tag 는 SQ 슬롯 번호가 아니다.
 *      memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes), cmd, 64);
 *    SQE 가 놓이는 자리는 링 생산자 인덱스 sq_tail 이 정한다. tag 는
 *    "동시에 몇 개까지 띄울 수 있는가"(자원 티켓), sq_tail 은 "이번 것을
 *    어디에 쓸 것인가"(링 위치)로 역할이 완전히 다르다.
 *
 * 4) 완료 시 정수 하나로 request 를 되찾는 경로가 이 파일의 존재 이유다.
 *      nvme_handle_cqe → nvme_find_rq → blk_mq_tag_to_rq(tags, tag)
 *    tags->rqs[tag] 한 번의 배열 접근으로 끝난다. 장치는 64비트 포인터를
 *    돌려주지 않고 16비트 정수만 돌려주므로, 이 인덱싱이 없으면 완료를
 *    원래 요청에 붙일 방법이 없다.
 *
 * 5) 예약 태그(breserved_tags)는 표준 NVMe PCIe 에서는 쓰이지 않는다.
 *    nvme_alloc_io_tag_set() 이 reserved_tags 를 설정하는 경우는 두 가지뿐이다:
 *      - NVME_QUIRK_SHARED_TAGS (Apple 계열): admin 과 태그 공간을 공유
 *      - NVME_F_FABRICS (RDMA/TCP/FC): connect 명령용으로 1개
 *    즉 "flush 용 예약 태그" 같은 것은 없다. blk-mq 의 flush 는 예약 태그가
 *    아니라 큐마다 따로 잡아 두는 flush_rq 로 처리된다(blk-flush.c).
 *
 * 6) shared tag 모드(BLK_MQ_F_TAG_QUEUE_SHARED)는 하나의 tag set 을 여러
 *    request_queue 가 공유할 때 켜진다. NVMe 에서는 한 컨트롤러의 여러
 *    네임스페이스(/dev/nvme0n1, n2, ...)가 같은 tag set 을 쓰는 구성이
 *    여기 해당한다 — 하드웨어 큐를 공유하니 tag 도 공유해야 한다.
 *    이때 hctx_may_queue() 가 active_queues 로 나눈 몫을 넘지 못하게 막아,
 *    한 네임스페이스가 태그를 독점해 다른 쪽을 굶기는 것을 방지한다.
 *
 * 7) tags_srcu: blk_mq_tagset_busy_iter() 가 tags[] 를 훑는 도중 큐 재구성
 *    (__blk_mq_update_nr_hw_queues)이 tags 를 교체·해제하는 경쟁을 막는다.
 *    해제는 call_srcu 로 미뤄져 순회가 끝난 뒤에 일어난다. RCU 가 아니라
 *    SRCU 인 이유는 순회 콜백이 잠들 수 있기 때문이다.
 *
 * 8) sbitmap_queue: tag 자체보다 "고갈됐을 때 어떻게 기다리는가"가 성능을
 *    좌우한다. 대기열을 여러 개(sbq_wait_state)로 쪼개 wakeup 시 thundering
 *    herd 를 줄이고, CPU 별 힌트로 캐시 라인 핑퐁을 줄인다.
 * ============================================================================
 */
