// SPDX-License-Identifier: GPL-2.0
/*
 * blk-mq scheduling framework
 *
 * Copyright (C) 2016 Jens Axboe
 */
/*
 * [한국어 설명] blk-mq IO 스케줄링 프레임워크 (blk-mq-sched.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 블록 멀티큐(blk-mq) 계층의 IO 스케줄링 프레임워크를 구현한다.
 * bio → request 변환 이후, mq-deadline·BFQ·kyber 등의 IO 스케줄러(elevator)와
 * blk-mq dispatch 엔진 사이의 연결 고리 역할을 한다. 스케줄러가 있으면 정렬된
 * request를 꺼내 NVMe SQ(Submission Queue)로 전달하고, 없으면("none") per-CPU
 * sw queue에서 곧바로 dispatch한다. 스케줄러 태그 풀(shadow CID pool)의 할당·해제,
 * bio merge, hctx restart 메커니즘도 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IO 경로에서 이 파일은 blk-mq와 드라이버(nvme_queue_rq) 사이의 dispatch 조율자이다:
 *
 *   [응용] write(2) → submit_bio() → blk_mq_submit_bio()
 *       ↓
 *   [blk-mq-sched] blk_mq_sched_bio_merge()  ← bio를 기존 request에 합치기 시도
 *       ↓
 *   [blk-mq-sched] blk_mq_sched_dispatch_requests()
 *       ↓ elevator 있음              ↓ elevator 없음("none")
 *   blk_mq_do_dispatch_sched()   blk_mq_do_dispatch_ctx()
 *       ↓                              ↓
 *   blk_mq_dispatch_rq_list() → nvme_queue_rq() → SQ doorbell
 *
 * 실행 컨텍스트: 주로 process context(blk_mq_run_hw_queue work); dispatch 경로는
 * softirq에서도 호출될 수 있다. hctx 단위로 동작하며 각 hctx는 NVMe SQ/CQ 쌍 대응.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-mq.c: blk_mq_dispatch_rq_list(), blk_mq_run_hw_queue() — dispatch 실행;
 *     blk_mq_dequeue_from_ctx()로 sw queue에서 request 추출
 *   - block/elevator.c: elevator_alloc() — elevator_queue 할당;
 *     e->type->ops.dispatch_request()로 정렬된 request 획득
 *   - block/blk-mq-tag.c: blk_mq_alloc_map_and_rqs(), blk_mq_free_map_and_rqs() —
 *     scheduler shadow CID(tag) pool 할당/해제
 *   - drivers/nvme/host/pci.c: blk_mq_dispatch_rq_list() 경유로 nvme_queue_rq() 호출
 * 공유 자료구조:
 *   - struct blk_mq_hw_ctx (blk-mq.h): dispatch(잔여 list), sched_tags(shadow tag),
 *     dispatch_busy(SQ 혼잡), ctx_map(sw queue 비트맵), state(SCHED_RESTART)
 *   - struct elevator_queue / elevator_type (elevator.h): ops vtable
 *   - struct elevator_tags (blk-mq-sched.h): nr_hw_queues, nr_requests, tags[]
 *
 * === 주요 함수/구조체 요약 ===
 * blk_mq_sched_dispatch_requests()   - dispatch 최상위; stopped/quiesced 체크 후 위임
 * __blk_mq_sched_dispatch_requests() - residual(잔여) 처리 후 elevator/ctx 경로 분기
 * __blk_mq_do_dispatch_sched()       - elevator에서 request 뽑아 CID 확보 후 SQ dispatch
 * blk_mq_do_dispatch_ctx()           - none 스케줄러 경로; sw queue 라운드 로빈 dispatch
 * blk_mq_sched_bio_merge()           - bio를 sw queue 기존 request에 merge (PRP/SGL 절약)
 * blk_mq_init_sched()                - elevator_queue 할당 + hctx sched_tags 연결 + init_sched
 * blk_mq_exit_sched()                - elevator 제거; exit_hctx/exit_sched + sched_tags 정리
 * blk_mq_alloc_sched_tags()          - scheduler shadow CID pool(elevator_tags) 할당
 * blk_mq_sched_mark_restart_hctx()   - BLK_MQ_S_SCHED_RESTART 비트 세팅; SQ 재출발 예약
 */
/* [한국어] 커널 기본 매크로(KERN_ERR 등)와 타입(size_t 등) */
#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL, module_init/exit 등 모듈 인프라 */
#include <linux/module.h>
/* [한국어] list_sort(): request를 hctx 기준으로 정렬해 doorbell batching 효율 향상 */
#include <linux/list_sort.h>

/* [한국어] blktrace block 이벤트: dispatch/insert/complete 추적용 (NVMe 성능 분석) */
#include <trace/events/block.h>

/* [한국어] block layer 내부 헤더: blk_queue_*, queue_limits 등 공통 정의 */
#include "blk.h"
/* [한국어] blk-mq 핵심 구조체(blk_mq_hw_ctx, blk_mq_ctx)와 내부 API */
#include "blk-mq.h"
/* [한국어] blk_mq_debugfs_register_sched() 등 debugfs 등록 API */
#include "blk-mq-debugfs.h"
/* [한국어] elevator_tags, blk_mq_init_sched(), blk_mq_exit_sched() 선언 */
#include "blk-mq-sched.h"
/* [한국어] writeback throttling: NVMe 쓰기 폭주 시 QoS 제어 (blk_wbt_*) */
#include "blk-wbt.h"

/*
 * [한국어]
 * blk_mq_sched_mark_restart_hctx - hctx에 SCHED_RESTART 비트를 세워 재출발 예약
 *
 * @hctx: NVMe SQ/CQ 쌍에 대응하는 hardware context
 *
 * NVMe 컨트롤러가 일시적으로 SQ를 더 받을 수 없거나 budget/tag 고갈로 dispatch를
 * 중단해야 할 때 이 함수로 재출발을 표시한다. BLK_MQ_S_SCHED_RESTART 비트가
 * 세워지면 __blk_mq_sched_restart()가 나중에 이 비트를 클리어하고 hctx를 깨운다.
 * 이미 비트가 세워져 있으면 중복 set을 피하기 위해 early return한다.
 *
 * 호출 체인:
 *   __blk_mq_sched_dispatch_requests → [blk_mq_sched_mark_restart_hctx]
 *   blk_mq_dispatch_rq_list         → [blk_mq_sched_mark_restart_hctx]
 *   → (나중에) __blk_mq_sched_restart → blk_mq_run_hw_queue
 */
void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] 이미 SCHED_RESTART 비트가 세워져 있으면 중복 set 불필요 — early return */
	if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
		return;

	/* [한국어] BLK_MQ_S_SCHED_RESTART 비트 세팅: 이 hctx(NVMe SQ)를 나중에 재가동 예약;
	 * __blk_mq_sched_restart()가 이 비트를 클리어하고 blk_mq_run_hw_queue()를 호출 */
	set_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_mark_restart_hctx);

/*
 * [한국어]
 * __blk_mq_sched_restart - SCHED_RESTART 비트를 클리어하고 hctx(NVMe SQ)를 재가동
 *
 * @hctx: 재가동할 NVMe SQ/CQ hardware context
 *
 * blk_mq_sched_mark_restart_hctx()로 예약된 재출발을 실제로 수행한다. 먼저
 * BLK_MQ_S_SCHED_RESTART 비트를 클리어한 뒤, smp_mb()로 메모리 배리어를 놓아
 * "dispatch list에 새 request가 추가됨"을 blk_mq_run_hw_queue()가 반드시 볼 수
 * 있도록 순서를 보장한다. 배리어 없이 클리어 + dispatch 확인 순서가 역전되면
 * 새 request가 누락될 수 있다. 마지막으로 blk_mq_run_hw_queue(hctx, true)를
 * 호출해 dispatch work를 async로 예약, doorbell까지 이어진다.
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list (dispatch 실패 시) → [__blk_mq_sched_restart]
 *   → blk_mq_run_hw_queue → blk_mq_sched_dispatch_requests
 */
void __blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] SCHED_RESTART 클리어: 이 hctx(NVMe SQ)가 재출발 대기 상태에서 벗어남 */
	clear_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);

	/*
	 * Order clearing SCHED_RESTART and list_empty_careful(&hctx->dispatch)
	 * in blk_mq_run_hw_queue(). Its pair is the barrier in
	 * blk_mq_dispatch_rq_list(). So dispatch code won't see SCHED_RESTART,
	 * meantime new request added to hctx->dispatch is missed to check in
	 * blk_mq_run_hw_queue().
	 */
	/* [한국어] smp_mb(): SCHED_RESTART 클리어 ↔ dispatch list 확인 간 메모리 순서 강제;
	 * 이 배리어가 없으면 컴파일러/CPU가 순서를 바꿔 새 request를 놓칠 수 있다.
	 * 페어 배리어: blk_mq_dispatch_rq_list() 내의 smp_mb()와 쌍을 이룬다 */
	smp_mb();

	/* [한국어] blk_mq_run_hw_queue(async=true): dispatch work 예약 → 재 dispatch 후 doorbell */
	blk_mq_run_hw_queue(hctx, true);
}

/*
 * [한국어]
 * sched_rq_cmp - list_sort()용 request 정렬 비교 함수 (hctx 기준)
 *
 * @priv: 미사용 (list_sort 인터페이스 일관성용)
 * @a:    첫 번째 request의 queuelist 노드
 * @b:    두 번째 request의 queuelist 노드
 * @return: rqa->mq_hctx > rqb->mq_hctx이면 양수; 같은 hctx끼리 인접하도록 정렬
 *
 * blk_mq_dispatch_hctx_list()가 list_sort()를 호출해 같은 NVMe SQ(hctx)로 가는
 * request들을 연속 배치한다. 이렇게 하면 같은 SQ로 가는 request들을 한 번에
 * dispatch할 수 있어 doorbell 발행 횟수와 CPU 캐시 miss를 줄인다.
 *
 * 호출 체인:
 *   __blk_mq_do_dispatch_sched → list_sort(sched_rq_cmp) → blk_mq_dispatch_hctx_list
 */
static int sched_rq_cmp(void *priv, const struct list_head *a,
			const struct list_head *b)
{
	/* [한국어] container_of: queuelist 노드 포인터 → request 포인터로 역참조 */
	struct request *rqa = container_of(a, struct request, queuelist);
	/* [한국어] container_of: queuelist 노드 포인터 → request 포인터로 역참조 */
	struct request *rqb = container_of(b, struct request, queuelist);

	/* [한국어] mq_hctx 포인터 값 비교: 같은 NVMe SQ(hctx)끼리 인접하게 정렬됨;
	 * 정렬 후 blk_mq_dispatch_hctx_list()가 동일 hctx 구간을 잘라 batch dispatch */
	return rqa->mq_hctx > rqb->mq_hctx;
}

/*
 * [한국어]
 * blk_mq_dispatch_hctx_list - rq_list 앞부분의 동일 hctx request들을 한 번에 dispatch
 *
 * @rq_list: sched_rq_cmp()로 정렬된 request 리스트 (여러 hctx가 섞일 수 있음)
 * @return: blk_mq_dispatch_rq_list() 반환값 — 1개 이상 SQ에 들어갔으면 true
 *
 * list_sort() 후 호출되며, rq_list 앞쪽의 같은 hctx(NVMe SQ)에 해당하는 request들을
 * hctx_list로 잘라낸 뒤 blk_mq_dispatch_rq_list()로 일괄 전달한다. 호출자는 rq_list가
 * 빌 때까지 이 함수를 반복 호출한다. 다른 hctx를 만나는 순간 현재 hctx 구간만 처리하고
 * 나머지는 rq_list에 남겨 다음 호출에서 처리하게 한다.
 *
 * 호출 체인:
 *   __blk_mq_do_dispatch_sched → list_sort → [blk_mq_dispatch_hctx_list]
 *   → blk_mq_dispatch_rq_list → nvme_queue_rq → SQ doorbell
 */
static bool blk_mq_dispatch_hctx_list(struct list_head *rq_list)
{
	/* [한국어] 리스트 첫 request의 mq_hctx(NVMe SQ/CQ)를 현재 처리할 hctx로 선택 */
	struct blk_mq_hw_ctx *hctx =
		list_first_entry(rq_list, struct request, queuelist)->mq_hctx;
	/* [한국어] 순회용 request 포인터 — 다음 hctx 경계를 찾는 데 사용 */
	struct request *rq;
	/* [한국어] 현재 hctx에 해당하는 request들만 담을 부분 리스트 */
	LIST_HEAD(hctx_list);

	/* [한국어] rq_list 순회: 다른 hctx(NVMe SQ)를 만나면 경계에서 분리 */
	list_for_each_entry(rq, rq_list, queuelist) {
		if (rq->mq_hctx != hctx) {
			/* [한국어] list_cut_before: rq 바로 앞까지(동일 hctx 구간)를 hctx_list로 이동;
			 * rq_list에는 나머지(다음 hctx 이후)가 남아 다음 호출에서 처리된다 */
			list_cut_before(&hctx_list, rq_list, &rq->queuelist);
			goto dispatch;
		}
	}
	/* [한국어] 모든 request가 같은 hctx(NVMe SQ)면 전체를 hctx_list로 이동 후 dispatch */
	list_splice_tail_init(rq_list, &hctx_list);

dispatch:
	/* [한국어] 동일 hctx(NVMe SQ)로 가는 request batch를 일괄 dispatch → nvme_queue_rq → doorbell */
	return blk_mq_dispatch_rq_list(hctx, &hctx_list, false);
}

/* [한국어] BLK_MQ_BUDGET_DELAY: budget 확보 실패 후 NVMe SQ 재가동 전 대기 시간(밀리초);
 * 너무 짧으면 CPU를 낭비하고 너무 길면 latency가 늘어나므로 3ms로 설정 */
#define BLK_MQ_BUDGET_DELAY	3		/* ms units */

/*
 * Only SCSI implements .get_budget and .put_budget, and SCSI restarts
 * its queue by itself in its completion handler, so we don't need to
 * restart queue if .get_budget() fails to get the budget.
 *
 * Returns -EAGAIN if hctx->dispatch was found non-empty and run_work has to
 * be run again.  This is necessary to avoid starving flushes.
 */
/*
 * [한국어]
 * __blk_mq_do_dispatch_sched - elevator에서 request를 꺼내 NVMe SQ로 dispatch
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 * @return: 1 = 1개 이상 dispatch 성공; 0 = 아무것도 dispatch 못함; -EAGAIN = 잔여 residual 존재
 *
 * IO scheduler(elevator)가 관리하는 큐에서 request를 반복적으로 꺼내 NVMe SQ로
 * dispatch한다. 각 반복에서: (1) has_work 확인, (2) residual dispatch list 체크,
 * (3) budget(컨트롤러 처리 용량) 확보, (4) dispatch_request()로 request 추출,
 * (5) driver tag(NVMe CID) 확보까지 완료해야 rq_list에 추가된다. 여러 hctx로
 * 흩어지는 request들은 list_sort()로 hctx별로 모아 batch dispatch한다.
 *
 * 호출 체인:
 *   blk_mq_do_dispatch_sched → [__blk_mq_do_dispatch_sched]
 *   → blk_mq_dispatch_rq_list → nvme_queue_rq → SQ doorbell
 */
static int __blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] request_queue: 이 hctx가 속한 NVMe namespace 큐 */
	struct request_queue *q = hctx->queue;
	/* [한국어] elevator_queue: mq-deadline/BFQ/kyber 스케줄러 상태; ops.dispatch_request로 request 추출 */
	struct elevator_queue *e = q->elevator;
	/* [한국어] multi_hctxs: 뽑은 request들이 여러 NVMe SQ로 흩어지면 true — list_sort 필요 */
	bool multi_hctxs = false;
	/* [한국어] run_queue: budget 확보 후 dispatch_request()가 NULL을 반환했을 때 SQ 재가동 필요 표시 */
	bool run_queue = false;
	/* [한국어] dispatched: 1개 이상 NVMe SQ로 전달 성공 여부 */
	bool dispatched = false;
	/* [한국어] busy: hctx->dispatch에 잔여 request가 있어 flush 기아 위험 표시 */
	bool busy = false;
	/* [한국어] max_dispatch: 이번 루프에서 NVMe SQ로 보낼 최대 request 수
	 * dispatch_busy이면 1(conservative)로 제한, 아니면 queue depth까지 허용 */
	unsigned int max_dispatch;
	/* [한국어] rq_list: elevator에서 뽑아 driver tag(CID)까지 확보한 NVMe 명령 후보 목록 */
	LIST_HEAD(rq_list);
	/* [한국어] count: 이번 루프에서 실제로 rq_list에 추가된 request 수 */
	int count = 0;

	/* [한국어] dispatch_busy: NVMe SQ full/backpressure 신호; true이면 보수적으로 1개씩만 dispatch */
	if (hctx->dispatch_busy)
		/* [한국어] SQ 혼잡 상태 — 1개만 보내서 컨트롤러 부담 완화 */
		max_dispatch = 1;
	else
		/* [한국어] SQ 여유 있음 — queue depth(nr_requests)까지 한 번에 batch dispatch */
		max_dispatch = hctx->queue->nr_requests;

	do {
		/* [한국어] 이번 반복에서 처리할 NVMe 명령 후보 (elevator에서 추출 예정) */
		struct request *rq;
		/* [한국어] budget_token: NVMe 컨트롤러 동시처리 슬롯 — SCSI 외에는 대부분 no-op */
		int budget_token;

		/*
		 * If we cannot get tag for the request, stop dequeueing
		 * requests from the IO scheduler. We are unlikely to be able
		 * to submit them anyway and it creates false impression for
		 * scheduling heuristics that the device can take more IO.
		 */
		/* [한국어] has_work(): elevator 큐에 NVMe SQ로 보낼 request가 있는지 확인;
		 * 없으면 루프 종료 — 불필요한 budget 소비 방지 */
		if (e->type->ops.has_work && !e->type->ops.has_work(hctx))
			break;

		/* [한국어] hctx->dispatch가 비어있지 않으면 residual(이전 SQ 거부) 처리 우선;
		 * busy=true로 표시해 -EAGAIN 반환 → 호출자가 residual을 먼저 처리하게 함 */
		if (!list_empty_careful(&hctx->dispatch)) {
			busy = true;
			break;
		}

		/* [한국어] blk_mq_get_dispatch_budget(): NVMe 컨트롤러 동시처리 용량 확보;
		 * SCSI만 구현하며 NVMe에서는 no-op이어서 항상 성공(budget_token >= 0) */
		budget_token = blk_mq_get_dispatch_budget(q);
		/* [한국어] budget_token < 0: 컨트롤러 처리 용량 초과 — 더 이상 뽑을 수 없음 */
		if (budget_token < 0)
			break;

		/* [한국어] dispatch_request(): elevator가 다음에 NVMe SQ로 보낼 최적 request 선택;
		 * 반환 값이 NULL이면 scheduler 큐가 비었거나 dispatch 조건 불만족 */
		rq = e->type->ops.dispatch_request(hctx);
		if (!rq) {
			/* [한국어] budget만 가져가고 dispatch하지 못한 경우 즉시 반납 — 다른 hctx가 사용할 수 있게 */
			blk_mq_put_dispatch_budget(q, budget_token);
			/*
			 * We're releasing without dispatching. Holding the
			 * budget could have blocked any "hctx"s with the
			 * same queue and if we didn't dispatch then there's
			 * no guarantee anyone will kick the queue.  Kick it
			 * ourselves.
			 */
			/* [한국어] run_queue=true: 루프 종료 후 NVMe SQ를 3ms 딜레이로 재가동 예약 */
			run_queue = true;
			break;
		}

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		/* [한국어] rq가 budget_token 소유 — blk_mq_dispatch_rq_list()에서 실제 queue_rq()에
		 * 실패하면 그 안에서 budget을 반납함 */
		blk_mq_set_rq_budget_token(rq, budget_token);

		/* [한국어] rq_list 뒤에 추가: budget과 driver tag 확보 후 일괄 dispatch 대기 */
		list_add_tail(&rq->queuelist, &rq_list);
		count++;
		/* [한국어] rq->mq_hctx != hctx: 이 request가 다른 NVMe SQ로 가야 함 → list_sort 필요 */
		if (rq->mq_hctx != hctx)
			multi_hctxs = true;

		/*
		 * If we cannot get tag for the request, stop dequeueing
		 * requests from the IO scheduler. We are unlikely to be able
		 * to submit them anyway and it creates false impression for
		 * scheduling heuristics that the device can take more IO.
		 */
		/* [한국어] blk_mq_get_driver_tag(): NVMe CID(Command Identifier) = driver tag 할당;
		 * 실패하면 SQ가 가득 차거나 tag 고갈 — 더 이상 elevator에서 뽑지 않음 */
		if (!blk_mq_get_driver_tag(rq))
			break;
	} while (count < max_dispatch); /* [한국어] max_dispatch 한계까지 반복 (SQ depth 또는 1) */

	if (!count) {
		/* [한국어] 아무것도 dispatch하지 못한 경우 */
		if (run_queue)
			/* [한국어] 3ms 후 모든 hctx를 재가동 — budget 반납 후 다른 hctx가 먼저 처리할 기회 */
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY);
	} else if (multi_hctxs) {
		/*
		 * Requests from different hctx may be dequeued from some
		 * schedulers, such as bfq and deadline.
		 *
		 * Sort the requests in the list according to their hctx,
		 * dispatch batching requests from same hctx at a time.
		 */
		/* [한국어] list_sort(sched_rq_cmp): 여러 NVMe SQ로 흩어진 request를 hctx별로 정렬;
		 * 같은 SQ끼리 모아야 doorbell 횟수를 최소화하고 CPU 캐시 효율을 높일 수 있다 */
		list_sort(NULL, &rq_list, sched_rq_cmp);
		do {
			/* [한국어] 한 hctx(NVMe SQ) 구간씩 잘라 batch dispatch; rq_list가 빌 때까지 반복 */
			dispatched |= blk_mq_dispatch_hctx_list(&rq_list);
		} while (!list_empty(&rq_list));
	} else {
		/* [한국어] 모든 request가 같은 hctx(NVMe SQ)면 바로 일괄 dispatch */
		dispatched = blk_mq_dispatch_rq_list(hctx, &rq_list, false);
	}

	/* [한국어] busy=true: hctx->dispatch에 잔여 request 존재 → -EAGAIN으로 flush 기아 방지 재호출 */
	if (busy)
		return -EAGAIN;
	/* [한국어] 1개 이상 dispatch 성공이면 1, 아니면 0 반환 */
	return !!dispatched;
}

/*
 * [한국어]
 * blk_mq_do_dispatch_sched - elevator 경로 dispatch 루프: 최대 1초간 반복 dispatch
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 * @return: __blk_mq_do_dispatch_sched()의 마지막 반환값
 *          (0=아무것도 못 함; 1=성공; -EAGAIN=잔여)
 *
 * __blk_mq_do_dispatch_sched()를 반복 호출해 elevator에서 가능한 한 많은 request를
 * NVMe SQ로 dispatch한다. 한 번 성공(ret==1)하면 계속 시도하고, 선점 요청이 오거나
 * 1초(HZ jiffies)가 지나면 blk_mq_delay_run_hw_queue(hctx, 0)로 즉시 재가동 예약
 * 후 루프를 탈출해 CPU를 양보한다. 이는 SQ가 매우 바쁠 때 CPU 독점을 방지하면서도
 * throughput을 최대화하는 절충안이다.
 *
 * 호출 체인:
 *   __blk_mq_sched_dispatch_requests → [blk_mq_do_dispatch_sched]
 *   → __blk_mq_do_dispatch_sched → blk_mq_dispatch_rq_list → nvme_queue_rq
 */
static int blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] end: 루프 종료 시한 — jiffies + HZ = 현재 시각 + 1초;
	 * 1초를 넘으면 CPU를 양보하고 재가동 예약 */
	unsigned long end = jiffies + HZ;
	int ret;

	do {
		/* [한국어] elevator에서 request 추출 후 NVMe SQ dispatch 시도 */
		ret = __blk_mq_do_dispatch_sched(hctx);
		/* [한국어] ret != 1: SQ full/budget 부족/elevator 작업 없음 → 루프 종료 */
		if (ret != 1)
			break;
		/* [한국어] need_resched(): 더 높은 우선순위 태스크가 대기 중 → CPU 양보 필요;
		 * time_is_before_jiffies(end): 1초 경과 — 장시간 CPU 독점 방지 */
		if (need_resched() || time_is_before_jiffies(end)) {
			/* [한국어] delay=0으로 즉시 재가동 예약: CPU 양보 후 바로 이어서 dispatch */
			blk_mq_delay_run_hw_queue(hctx, 0);
			break;
		}
	} while (1);

	return ret;
}

/*
 * [한국어]
 * blk_mq_next_ctx - 같은 hctx 내에서 현재 ctx 다음의 sw queue를 라운드 로빈으로 반환
 *
 * @hctx: NVMe SQ/CQ hardware context — 매핑된 ctx들의 컨테이너
 * @ctx:  현재 CPU의 software queue (per-CPU request 큐)
 * @return: 다음 CPU sw queue 포인터 (마지막이면 첫 번째로 wrap)
 *
 * blk_mq_do_dispatch_ctx()가 per-CPU sw queue들을 공정하게 순회할 때 사용한다.
 * ctx->index_hw[hctx->type]은 이 ctx가 hctx 내에서 몇 번째인지를 나타내며,
 * hctx->ctxs[] 배열에서 다음 인덱스의 ctx를 반환한다. 마지막 인덱스에서는
 * 0으로 wrap-around해 모든 CPU sw queue에 골고루 기회를 준다.
 *
 * 호출 체인:
 *   blk_mq_do_dispatch_ctx → [blk_mq_next_ctx] (라운드 로빈 포인터 전진)
 */
static struct blk_mq_ctx *blk_mq_next_ctx(struct blk_mq_hw_ctx *hctx,
					  struct blk_mq_ctx *ctx)
{
	/* [한국어] index_hw[type]: 이 ctx가 hctx 내의 ctxs[] 배열에서 몇 번째인지 인덱스 */
	unsigned short idx = ctx->index_hw[hctx->type];

	/* [한국어] idx 선증가 후 nr_ctx(hctx 내 총 ctx 수)와 비교; 마지막이면 0으로 wrap */
	if (++idx == hctx->nr_ctx)
		idx = 0;

	/* [한국어] hctx->ctxs[idx]: 다음 CPU sw queue 반환 — 라운드 로빈으로 공정 분산 */
	return hctx->ctxs[idx];
}

/*
 * Only SCSI implements .get_budget and .put_budget, and SCSI restarts
 * its queue by itself in its completion handler, so we don't need to
 * restart queue if .get_budget() fails to get the budget.
 *
 * Returns -EAGAIN if hctx->dispatch was found non-empty and run_work has to
 * be run again.  This is necessary to avoid starving flushes.
 */
/*
 * [한국어]
 * blk_mq_do_dispatch_ctx - elevator 없음("none" 스케줄러) 경로: sw queue 라운드 로빈 dispatch
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 * @return: 0 = 정상 종료; -EAGAIN = hctx->dispatch에 잔여 request 존재 (flush 기아 방지)
 *
 * IO 스케줄러가 없는 상황("none", NVMe에서 흔히 사용)에서 각 CPU의 sw queue(ctx)에서
 * request를 하나씩 꺼내 NVMe SQ로 dispatch한다. per-CPU ctx를 라운드 로빈으로 순회해
 * 공정성을 보장한다. 각 반복에서 (1) residual 확인, (2) ctx_map 비트맵 확인,
 * (3) budget 확보, (4) dequeue_from_ctx()로 request 추출, (5) dispatch_rq_list()로
 * SQ 전달까지 진행한다. dispatch_from을 갱신해 다음 호출에서 같은 지점에서 재개한다.
 *
 * 호출 체인:
 *   __blk_mq_sched_dispatch_requests (elevator 없음 경로)
 *   → [blk_mq_do_dispatch_ctx] → blk_mq_dispatch_rq_list → nvme_queue_rq
 */
static int blk_mq_do_dispatch_ctx(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] NVMe namespace request_queue — budget/tag 확보에 사용 */
	struct request_queue *q = hctx->queue;
	/* [한국어] rq_list: 현재 ctx에서 뽑아 NVMe SQ로 보낼 request 1개 임시 보관 */
	LIST_HEAD(rq_list);
	/* [한국어] dispatch_from: 라운드 로빈 시작점 — 이전 dispatch가 멈춘 ctx에서 재개;
	 * READ_ONCE: 다른 CPU의 WRITE_ONCE와 경쟁 — 원자적 읽기 필요 */
	struct blk_mq_ctx *ctx = READ_ONCE(hctx->dispatch_from);
	/* [한국어] 0: 정상 종료; -EAGAIN으로 설정되면 flush 기아 방지 재호출 신호 */
	int ret = 0;
	struct request *rq;

	do {
		/* [한국어] budget_token: NVMe 컨트롤러 동시처리 슬롯 (SCSI 외 no-op) */
		int budget_token;

		/* [한국어] hctx->dispatch에 이전 SQ 거부 잔여 request 존재 → 우선 처리 필요;
		 * -EAGAIN 반환으로 호출자가 residual을 먼저 처리하도록 지시 */
		if (!list_empty_careful(&hctx->dispatch)) {
			ret = -EAGAIN;
			break;
		}

		/* [한국어] ctx_map: 처리할 request가 있는 sw queue를 나타내는 비트맵;
		 * 모든 비트가 0이면 모든 CPU sw queue가 비어 있음 → 루프 종료 */
		if (!sbitmap_any_bit_set(&hctx->ctx_map))
			break;

		/* [한국어] NVMe 컨트롤러 동시처리 용량 확보 (NVMe에서는 보통 no-op, 항상 성공) */
		budget_token = blk_mq_get_dispatch_budget(q);
		/* [한국어] budget_token < 0: 컨트롤러 처리 한계 초과 → 루프 종료 */
		if (budget_token < 0)
			break;

		/* [한국어] blk_mq_dequeue_from_ctx(): 현재 ctx(CPU sw queue)에서 request 1개 추출;
		 * ctx_map 비트는 설정되어 있었으나 다른 CPU가 이미 뽑아갔을 수 있음 → NULL 체크 필요 */
		rq = blk_mq_dequeue_from_ctx(hctx, ctx);
		if (!rq) {
			/* [한국어] dequeue 실패 — budget만 가져가고 dispatch 못 함 → 즉시 반납 */
			blk_mq_put_dispatch_budget(q, budget_token);
			/*
			 * We're releasing without dispatching. Holding the
			 * budget could have blocked any "hctx"s with the
			 * same queue and if we didn't dispatch then there's
			 * no guarantee anyone will kick the queue.  Kick it
			 * ourselves.
			 */
			/* [한국어] 3ms 후 모든 관련 hctx를 재가동 — 다른 CPU의 sw queue 처리 재개 */
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY);
			break;
		}

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		/* [한국어] rq가 budget_token 소유; dispatch_rq_list에서 queue_rq 실패 시 자동 반납 */
		blk_mq_set_rq_budget_token(rq, budget_token);

		/* [한국어] rq_list에 추가: dispatch_rq_list()가 NVMe SQ에 전달할 단일 request */
		list_add(&rq->queuelist, &rq_list);

		/* round robin for fair dispatch */
		/* [한국어] 다음 ctx로 전진: 라운드 로빈으로 모든 CPU sw queue에 공정한 기회 부여 */
		ctx = blk_mq_next_ctx(hctx, rq->mq_ctx);

	} while (blk_mq_dispatch_rq_list(rq->mq_hctx, &rq_list, false));
	/* [한국어] dispatch_rq_list 성공(true 반환) 시 다음 반복으로 계속 뽑기;
	 * SQ full이면 false 반환 → 루프 종료 */

	/* [한국어] WRITE_ONCE: 다음 dispatch 시작점 ctx 저장;
	 * READ_ONCE/WRITE_ONCE 쌍으로 data race 없이 라운드 로빈 위치 공유 */
	WRITE_ONCE(hctx->dispatch_from, ctx);
	return ret;
}

/*
 * [한국어]
 * __blk_mq_sched_dispatch_requests - hctx 단위 dispatch 핵심 로직
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 * @return: 0 = 정상; -EAGAIN = hctx->dispatch 잔여로 재호출 필요
 *
 * 두 단계로 동작한다:
 * 1) hctx->dispatch(이전 SQ 거부 잔여)가 있으면 먼저 처리. 이후 SCHED_RESTART 표시.
 *    SQ가 다 받지 못하면 0을 반환해 다음 restart를 기다린다.
 * 2) elevator가 있으면 blk_mq_do_dispatch_sched()로, 없으면 blk_mq_do_dispatch_ctx()로 분기.
 *    elevator가 없고 SQ가 한가로운 경우 blk_mq_flush_busy_ctxs()로 sw queue 전체를 한꺼번에 처리.
 *
 * 핵심 불변식: scheduler에서 꺼낸 request는 더 이상 merge/sort 불가 → 가능한 한 오래
 * scheduler 안에 두는 것이 좋다. residual이 있을 때만 SCHED_RESTART를 표시하는 이유다.
 *
 * 호출 체인:
 *   blk_mq_sched_dispatch_requests → [__blk_mq_sched_dispatch_requests]
 *   → blk_mq_do_dispatch_sched OR blk_mq_do_dispatch_ctx
 *   → blk_mq_dispatch_rq_list → nvme_queue_rq → SQ doorbell
 */
static int __blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] need_dispatch: residual 처리 성공 또는 SQ 혼잡 시 추가 dispatch가 필요함을 표시 */
	bool need_dispatch = false;
	/* [한국어] rq_list: hctx->dispatch(residual) 또는 flush_busy_ctxs에서 가져올 request 임시 목록 */
	LIST_HEAD(rq_list);

	/*
	 * If we have previous entries on our dispatch list, grab them first for
	 * more fair dispatch.
	 */
	/* [한국어] list_empty_careful(): 락 없이 원자적으로 residual 존재 여부 확인;
	 * 거짓 양성은 허용 — 아래 spin_lock 후 재확인 */
	if (!list_empty_careful(&hctx->dispatch)) {
		/* [한국어] hctx->lock: hctx->dispatch 리스트 접근 직렬화 */
		spin_lock(&hctx->lock);
		/* [한국어] 락 획득 후 재확인 — 다른 CPU가 먼저 비웠을 수 있음 */
		if (!list_empty(&hctx->dispatch))
			/* [한국어] splice_init: hctx->dispatch의 모든 잔여 request를 rq_list로 이동 후 초기화 */
			list_splice_init(&hctx->dispatch, &rq_list);
		spin_unlock(&hctx->lock);
	}

	/*
	 * Only ask the scheduler for requests, if we didn't have residual
	 * requests from the dispatch list. This is to avoid the case where
	 * we only ever dispatch a fraction of the requests available because
	 * of low device queue depth. Once we pull requests out of the IO
	 * scheduler, we can no longer merge or sort them. So it's best to
	 * leave them there for as long as we can. Mark the hw queue as
	 * needing a restart in that case.
	 *
	 * We want to dispatch from the scheduler if there was nothing
	 * on the dispatch list or we were able to dispatch from the
	 * dispatch list.
	 */
	if (!list_empty(&rq_list)) {
		/* [한국어] residual request가 있음 → SCHED_RESTART 표시: 이후 scheduler에서도 더 뽑을 것임을 예약 */
		blk_mq_sched_mark_restart_hctx(hctx);
		/* [한국어] residual batch를 NVMe SQ에 전달; 'from_sched=true'는 scheduler 큐에서 온 것처럼 처리 */
		if (!blk_mq_dispatch_rq_list(hctx, &rq_list, true))
			/* [한국어] SQ가 일부를 받지 못함 → 0 반환, SCHED_RESTART가 나중에 재시도를 트리거 */
			return 0;
		/* [한국어] residual 처리 성공 → scheduler에서 추가 request를 더 뽑아야 함 */
		need_dispatch = true;
	} else {
		/* [한국어] residual 없음 → dispatch_busy 상태이면 계속 dispatch 시도 필요 */
		need_dispatch = hctx->dispatch_busy;
	}

	/* [한국어] elevator(mq-deadline/BFQ/kyber) 있으면 scheduler 정렬 경로로 dispatch */
	if (hctx->queue->elevator)
		return blk_mq_do_dispatch_sched(hctx);

	/* dequeue request one by one from sw queue if queue is busy */
	/* [한국어] elevator 없음("none"): need_dispatch이면 per-ctx 라운드 로빈 dispatch */
	if (need_dispatch)
		return blk_mq_do_dispatch_ctx(hctx);
	/* [한국어] SQ가 여유롭고 need_dispatch 아님: busy ctx들을 한꺼번에 flush해 rq_list로 수집 */
	blk_mq_flush_busy_ctxs(hctx, &rq_list);
	/* [한국어] sw queue 전체 flush batch를 NVMe SQ로 한 번에 전달 (from_sched=true) */
	blk_mq_dispatch_rq_list(hctx, &rq_list, true);
	return 0;
}

/*
 * [한국어]
 * blk_mq_sched_dispatch_requests - hctx dispatch 최상위 진입점
 *
 * @hctx: 처리할 NVMe SQ/CQ hardware context
 *
 * blk_mq_run_hw_queue()에서 호출되는 dispatch 최상위 함수. hctx가 stopped 상태이거나
 * queue가 quiesced(NVMe reset/remove 중)이면 즉시 리턴해 SQ doorbell 발행을 막는다.
 * __blk_mq_sched_dispatch_requests()를 호출하고, -EAGAIN이 반환되면(hctx->dispatch에
 * 잔여 request 존재) 한 번 더 시도한다. 두 번째도 -EAGAIN이면 async work로 예약해
 * flush 기아를 방지한다. 총 최대 2번 시도 후 work 예약으로 보장한다.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue (work queue/softirq) → [blk_mq_sched_dispatch_requests]
 *   → __blk_mq_sched_dispatch_requests → ... → nvme_queue_rq → SQ doorbell
 */
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] NVMe namespace request_queue: quiesced 상태 확인에 사용 */
	struct request_queue *q = hctx->queue;

	/* RCU or SRCU read lock is needed before checking quiesced flag */
	/* [한국어] blk_mq_hctx_stopped(): BLK_MQ_S_STOPPED 비트 — NVMe controller 멈춤/드레인 중;
	 * blk_queue_quiesced(): queue를 동결(quiesce)해 새 I/O 금지 상태 (elevator switch 등);
	 * 두 경우 모두 SQ doorbell 발행을 막아야 함 */
	if (unlikely(blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(q)))
		return;

	/*
	 * A return of -EAGAIN is an indication that hctx->dispatch is not
	 * empty and we must run again in order to avoid starving flushes.
	 */
	/* [한국어] 첫 번째 dispatch 시도: -EAGAIN이면 hctx->dispatch에 잔여 있음 */
	if (__blk_mq_sched_dispatch_requests(hctx) == -EAGAIN) {
		/* [한국어] 두 번째 dispatch 시도: residual을 소진 */
		if (__blk_mq_sched_dispatch_requests(hctx) == -EAGAIN)
			/* [한국어] 두 번 시도에도 잔여 → async work 예약으로 flush 기아 방지 */
			blk_mq_run_hw_queue(hctx, true);
	}
}

/*
 * [한국어]
 * blk_mq_sched_bio_merge - bio를 scheduler 또는 sw queue의 기존 request에 merge 시도
 *
 * @q:      request_queue (NVMe namespace 큐)
 * @bio:    merge 시도할 신규 bio
 * @nr_segs: bio의 물리 세그먼트 수 (NVMe PRP/SGL 엔트리 복잡도 지표)
 * @return: merge 성공이면 true, 실패이면 false
 *
 * blk_mq_submit_bio()에서 bio를 request로 변환하기 전에 호출된다. elevator가 있으면
 * e->type->ops.bio_merge()로 scheduler의 merge 정책을 사용하고, 없으면("none") per-CPU
 * sw queue(ctx->rq_lists[type])에서 역방향으로 최대 8개의 request를 검사해 merge한다.
 * merge 성공 시 NVMe PRP/SGL 엔트리 수와 SQ doorbell 횟수가 줄어든다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → [blk_mq_sched_bio_merge]
 *   → e->ops.bio_merge (elevator 있음) OR blk_bio_list_merge (없음)
 */
bool blk_mq_sched_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	/* [한국어] elevator_queue: mq-deadline/BFQ/kyber 등 스케줄러; NULL이면 "none" 스케줄러 */
	struct elevator_queue *e = q->elevator;
	/* [한국어] 현재 CPU에 해당하는 software queue */
	struct blk_mq_ctx *ctx;
	/* [한국어] bio가 매핑될 NVMe SQ에 대응하는 hardware context */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] merge 성공 여부 초기화 */
	bool ret = false;
	/* [한국어] hctx 타입: default/read/poll — ctx->rq_lists[type] 인덱스로 사용 */
	enum hctx_type type;

	/* [한국어] elevator가 있고 bio_merge ops가 있으면 스케줄러에게 merge 위임 */
	if (e && e->type->ops.bio_merge) {
		/* [한국어] elevator 내부에서 LBA 정렬 기반 merge 시도 — NVMe PRP/SGL 최적화 */
		ret = e->type->ops.bio_merge(q, bio, nr_segs);
		goto out_put;
	}

	/* [한국어] blk_mq_get_ctx(): 현재 CPU의 sw queue 포인터 획득 (preempt-safe) */
	ctx = blk_mq_get_ctx(q);
	/* [한국어] blk_mq_map_queue(): bio->bi_opf 플래그 기반으로 적합한 hctx(NVMe SQ) 선택 */
	hctx = blk_mq_map_queue(bio->bi_opf, ctx);
	/* [한국어] hctx->type: HCTX_TYPE_DEFAULT / READ / POLL — 각 타입별 rq_lists 분리 */
	type = hctx->type;
	/* [한국어] sw queue에 merge 대상 request가 없으면 merge 불가 → 조기 종료 */
	if (list_empty_careful(&ctx->rq_lists[type]))
		goto out_put;

	/* default per sw-queue merge */
	/* [한국어] ctx->lock: sw queue rq_lists 접근 직렬화 (다른 CPU가 동시에 insert 가능) */
	spin_lock(&ctx->lock);
	/*
	 * Reverse check our software queue for entries that we could
	 * potentially merge with. Currently includes a hand-wavy stop
	 * count of 8, to not spend too much time checking for merges.
	 */
	/* [한국어] blk_bio_list_merge(): sw queue를 역방향으로 최대 8개 검사해 back/front merge;
	 * merge 성공 시 PRP/SGL 엔트리 감소 → NVMe SQ doorbell 효율 향상 */
	if (blk_bio_list_merge(q, &ctx->rq_lists[type], bio, nr_segs))
		ret = true;

	spin_unlock(&ctx->lock);
out_put:
	return ret;
}

/*
 * [한국어]
 * blk_mq_sched_try_insert_merge - scheduler에 request를 삽입할 때 merge 시도
 *
 * @q:    request_queue (NVMe namespace 큐)
 * @rq:   scheduler에 삽입하려는 request
 * @free: merge로 인해 해제할 request들을 담는 리스트
 * @return: merge 성공이면 true (rq는 기존 request에 흡수됨); 실패이면 false
 *
 * blk-mq가 request를 elevator에 삽입하기 직전, 기존 request와 merge 가능한지 확인한다.
 * rq_mergeable()로 기본 조건을 검사한 후 elv_attempt_insert_merge()로 elevator의
 * merge hash를 사용해 back-merge를 시도한다. 성공하면 NVMe SQ 엔트리와 doorbell 수를 줄인다.
 * merge된 request는 @free 리스트를 통해 호출자가 해제한다.
 *
 * 호출 체인:
 *   blk_mq_try_issue_directly / blk_mq_insert_request → [blk_mq_sched_try_insert_merge]
 *   → elv_attempt_insert_merge → ll_back_merge_fn → attempt_merge
 */
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq,
				   struct list_head *free)
{
	/* [한국어] rq_mergeable(): merge 기본 조건(REQ_NOMERGE 없음 등) 확인 후
	 * elv_attempt_insert_merge()로 elevator hash에서 back-merge 후보 탐색 및 병합 */
	return rq_mergeable(rq) && elv_attempt_insert_merge(q, rq, free);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_try_insert_merge);

/* called in queue's release handler, tagset has gone away */
/*
 * [한국어]
 * blk_mq_sched_tags_teardown - 모든 hctx에서 sched_tags 레퍼런스 제거
 *
 * @q:     NVMe namespace request_queue
 * @flags: blk_mq_tag_set의 flags (shared tags 여부 판단용)
 *
 * request_queue 해제 또는 elevator 종료 시 모든 hctx->sched_tags 포인터를 NULL로
 * 초기화한다. shared tags 모드이면 q->sched_shared_tags도 NULL로 설정한다.
 * 실제 메모리 해제는 blk_mq_free_sched_tags()에서 수행한다.
 *
 * 호출 체인:
 *   blk_mq_init_sched (실패 경로) → [blk_mq_sched_tags_teardown]
 *   blk_mq_exit_sched            → [blk_mq_sched_tags_teardown]
 */
static void blk_mq_sched_tags_teardown(struct request_queue *q, unsigned int flags)
{
	/* [한국어] 각 NVMe SQ/CQ 쌍에 대응하는 hctx 순회 */
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* [한국어] 모든 hctx의 sched_tags 포인터 NULL: 이후 dispatch 시 shadow tag에 접근하지 않음 */
	queue_for_each_hw_ctx(q, hctx, i)
		hctx->sched_tags = NULL;

	/* [한국어] shared tags 모드: 여러 hctx가 공유하는 sched_shared_tags도 NULL 처리 */
	if (blk_mq_is_shared_tags(flags))
		q->sched_shared_tags = NULL;
}

/*
 * [한국어]
 * blk_mq_sched_reg_debugfs - scheduler debugfs 항목 등록
 *
 * @q: NVMe namespace request_queue
 *
 * /sys/kernel/debug/block/<disk>/sched/ 하위에 scheduler 진단 정보를 등록한다.
 * queue 전체용 debugfs와 hctx별 debugfs를 함께 등록해 NVMe SQ별 latency·
 * dispatch 통계를 확인할 수 있게 한다. blk_debugfs_lock()으로 직렬화.
 */
void blk_mq_sched_reg_debugfs(struct request_queue *q)
{
	/* [한국어] 각 NVMe SQ(hctx) debugfs 등록에 사용할 포인터 */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] memflags: irq 상태 저장용 (blk_debugfs_lock 반환값) */
	unsigned int memflags;
	unsigned long i;

	/* [한국어] debugfs 등록 직렬화: 다른 CPU의 동시 등록/해제 방지 */
	memflags = blk_debugfs_lock(q);
	/* [한국어] queue 수준 scheduler debugfs 등록: dispatch 통계, elevator 파라미터 등 */
	blk_mq_debugfs_register_sched(q);
	/* [한국어] hctx(NVMe SQ)별 scheduler debugfs 등록: per-SQ dispatch 카운터 등 */
	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_debugfs_register_sched_hctx(q, hctx);
	blk_debugfs_unlock(q, memflags);
}

/*
 * [한국어]
 * blk_mq_sched_unreg_debugfs - scheduler debugfs 항목 제거
 *
 * @q: NVMe namespace request_queue
 *
 * elevator 종료 또는 queue 해제 시 hctx별 debugfs와 queue debugfs를 역순으로 제거한다.
 */
void blk_mq_sched_unreg_debugfs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* [한국어] nomemsave 변형: irq disable 없이 스핀락만 획득 (해제 경로에서 GFP 불가) */
	blk_debugfs_lock_nomemsave(q);
	/* [한국어] hctx(NVMe SQ)별 scheduler debugfs 제거 */
	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_debugfs_unregister_sched_hctx(hctx);
	/* [한국어] queue 수준 scheduler debugfs 제거 */
	blk_mq_debugfs_unregister_sched(q);
	blk_debugfs_unlock_nomemrestore(q);
}

/*
 * [한국어]
 * blk_mq_free_sched_tags - scheduler shadow tag pool(elevator_tags) 해제
 *
 * @et:  해제할 elevator_tags 구조체 (nr_hw_queues, tags[] 포함)
 * @set: NVMe 장치 blk_mq_tag_set (shared tags 여부 판단, request 해제에 사용)
 *
 * blk_mq_alloc_sched_tags()로 할당된 scheduler shadow CID pool을 해제한다.
 * shared tags 모드면 index 0의 tag map 하나만, per-SQ 모드면 nr_hw_queues개의
 * tag map을 순서대로 해제한다. 마지막으로 elevator_tags 구조체 자체를 kfree한다.
 *
 * 호출 체인:
 *   blk_mq_free_sched_res → [blk_mq_free_sched_tags]
 *   blk_mq_sched_tags_teardown 이후 — tags[] 포인터가 이미 NULL이 아닌지 확인 불필요
 */
void blk_mq_free_sched_tags(struct elevator_tags *et,
		struct blk_mq_tag_set *set)
{
	unsigned long i;

	/* Shared tags are stored at index 0 in @tags. */
	/* [한국어] shared tags: 모든 NVMe SQ가 하나의 tag pool 공유 → et->tags[0] 하나만 해제 */
	if (blk_mq_is_shared_tags(set->flags))
		/* [한국어] BLK_MQ_NO_HCTX_IDX: 특정 hctx에 귀속되지 않는 공유 pool을 나타내는 sentinel */
		blk_mq_free_map_and_rqs(set, et->tags[0], BLK_MQ_NO_HCTX_IDX);
	else {
		/* [한국어] per-SQ: 각 hctx(NVMe SQ)에 독립 tag map → nr_hw_queues개 순서대로 해제 */
		for (i = 0; i < et->nr_hw_queues; i++)
			blk_mq_free_map_and_rqs(set, et->tags[i], i);
	}

	/* [한국어] elevator_tags + flex array(tags[]) 구조체 자체 해제 */
	kfree(et);
}

/*
 * [한국어]
 * blk_mq_free_sched_res - elevator_resources(태그셋 + private data) 해제
 *
 * @res:  해제할 elevator_resources (et, data 포인터 포함)
 * @type: elevator_type (blk_mq_free_sched_data ops 호출용)
 * @set:  NVMe 장치 blk_mq_tag_set (tag pool 해제에 필요)
 *
 * elevator_resources는 elevator 전환 시 새 elevator를 위해 미리 할당한 자원 묶음이다.
 * et(elevator_tags)와 data(스케줄러 private data)를 각각 해제하고 포인터를 NULL로 초기화한다.
 */
void blk_mq_free_sched_res(struct elevator_resources *res,
		struct elevator_type *type,
		struct blk_mq_tag_set *set)
{
	/* [한국어] et가 있으면 scheduler shadow CID pool 해제 */
	if (res->et) {
		blk_mq_free_sched_tags(res->et, set);
		/* [한국어] 해제 후 NULL로 초기화 — double free 방지 */
		res->et = NULL;
	}
	/* [한국어] data가 있으면 scheduler private data(mq-deadline rb-tree, BFQ bfqd 등) 해제 */
	if (res->data) {
		blk_mq_free_sched_data(type, res->data);
		res->data = NULL;
	}
}

/*
 * [한국어]
 * blk_mq_free_sched_res_batch - tagset에 속한 모든 queue의 scheduler 자원 일괄 해제
 *
 * @elv_tbl: queue id → elv_change_ctx 매핑 xarray (elevator 전환 컨텍스트 테이블)
 * @set:     NVMe 장치 blk_mq_tag_set
 *
 * update_nr_hwq_lock write lock을 보유한 상태에서 호출된다. NVMe SQ 수 변경
 * (update_nr_hw_queues) 롤백 경로나 elevator 일괄 교체 종료 시 사용한다.
 * scheduler가 붙은 queue만 처리하며 elv_tbl에서 ctx를 찾아 free한다.
 */
void blk_mq_free_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	/* [한국어] tagset에 속한 NVMe namespace queue 순회용 포인터 */
	struct request_queue *q;
	/* [한국어] elv_tbl에서 로드할 elevator 전환 컨텍스트 */
	struct elv_change_ctx *ctx;

	/* [한국어] lockdep: update_nr_hwq_lock write lock 보유 확인 — 미보유 시 커널 경고 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		/* [한국어] elevator가 있는 NVMe queue만 해제 대상; 없는 queue("none" 스케줄러)는 건너뜀 */
		if (q->elevator) {
			/* [한국어] xa_load(): queue->id를 키로 변경 컨텍스트 찾기 */
			ctx = xa_load(elv_tbl, q->id);
			if (!ctx) {
				/* [한국어] elv_tbl에 없으면 프로그래밍 오류 — WARN 후 다음 queue 처리 */
				WARN_ON_ONCE(1);
				continue;
			}
			/* [한국어] ctx->res(et + data) 해제: elevator 전환 롤백 또는 정리 경로 */
			blk_mq_free_sched_res(&ctx->res, ctx->type, set);
		}
	}
}

/*
 * [한국어]
 * blk_mq_free_sched_ctx_batch - elv_tbl xarray의 모든 elv_change_ctx 해제
 *
 * @elv_tbl: queue id → elv_change_ctx xarray (elevator 전환 컨텍스트 테이블)
 *
 * elevator 전환 완료 또는 실패 롤백 후 전환 컨텍스트 테이블을 정리한다. xa_for_each로
 * 순회하면서 각 항목을 xarray에서 erase하고 kfree로 해제한다.
 */
void blk_mq_free_sched_ctx_batch(struct xarray *elv_tbl)
{
	unsigned long i;
	/* [한국어] xa_for_each로 꺼낸 elevator 전환 컨텍스트 */
	struct elv_change_ctx *ctx;

	/* [한국어] xa_for_each: xarray의 모든 유효 인덱스(i)와 값(ctx)을 순회 */
	xa_for_each(elv_tbl, i, ctx) {
		/* [한국어] xa_erase(): xarray에서 i 항목 제거 (이후 xa_load(elv_tbl, i) == NULL) */
		xa_erase(elv_tbl, i);
		/* [한국어] elv_change_ctx 구조체 해제 — res/type/name 등 포함 */
		kfree(ctx);
	}
}

/*
 * [한국어]
 * blk_mq_alloc_sched_ctx_batch - tagset의 모든 queue에 대해 elv_change_ctx를 일괄 할당
 *
 * @elv_tbl: queue id → elv_change_ctx 매핑 xarray (비어있는 상태로 전달)
 * @set:     NVMe 장치 blk_mq_tag_set
 * @return:  0 = 성공; -ENOMEM = 메모리 부족 (일부 할당 성공 상태로 반환 — 롤백은 호출자)
 *
 * elevator 전환(elevator_change) 시 미리 모든 queue의 컨텍스트를 할당한다. 이후
 * blk_mq_alloc_sched_res_batch()가 각 ctx에 elevator_tags와 private data를 채운다.
 * update_nr_hwq_lock write lock 하에서 호출되어야 한다.
 */
int blk_mq_alloc_sched_ctx_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	/* [한국어] tagset 소속 NVMe namespace queue 순회용 */
	struct request_queue *q;
	/* [한국어] 새로 할당할 elevator 전환 컨텍스트 */
	struct elv_change_ctx *ctx;

	/* [한국어] update_nr_hwq_lock write lock 보유 확인 — 미보유 시 lockdep 경고 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	/* [한국어] tagset에 속한 모든 NVMe namespace queue를 순회해 ctx 할당 */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* [한국어] kzalloc_obj: elv_change_ctx 구조체를 zero-init으로 할당 */
		ctx = kzalloc_obj(struct elv_change_ctx);
		if (!ctx)
			/* [한국어] 메모리 부족: 이미 할당된 ctx는 호출자가 blk_mq_free_sched_ctx_batch()로 해제 */
			return -ENOMEM;

		/* [한국어] xa_insert(): q->id를 키로 ctx를 elv_tbl에 삽입;
		 * 이미 같은 id가 있으면 실패 (-EBUSY) — 정상 흐름에서는 발생하지 않음 */
		if (xa_insert(elv_tbl, q->id, ctx, GFP_KERNEL)) {
			kfree(ctx);
			return -ENOMEM;
		}
	}
	return 0;
}

/*
 * [한국어]
 * blk_mq_alloc_sched_tags - scheduler shadow CID pool(elevator_tags) 할당
 *
 * @set:          NVMe 장치 blk_mq_tag_set
 * @nr_hw_queues: NVMe SQ(hctx) 수
 * @nr_requests:  scheduler가 관리할 최대 request 수 (NVMe SQ depth 근접)
 * @return:       할당된 elevator_tags 포인터; 실패 시 NULL
 *
 * scheduler가 driver tag(NVMe CID)를 shadow로 관리할 tag map과 request pool을 할당한다.
 * shared tags 모드면 모든 hctx가 tags[0] 하나를 공유하고(MAX_SCHED_RQ 한도),
 * per-SQ 모드면 hctx별로 독립된 tag map을 nr_requests 크기로 할당한다.
 * 할당 실패 시 out_unwind 경로에서 이미 할당된 tag map을 역순으로 해제한다.
 *
 * 구조체 field 역할:
 *   et->nr_requests:  이 elevator_tags가 관리하는 최대 request 수 (SQ depth와 맞춤)
 *   et->nr_hw_queues: hctx(NVMe SQ) 수; per-SQ 모드에서 해제 루프 상한
 *   et->tags[i]:      i번째 hctx용 blk_mq_tags (tag bitmap + request pool)
 *
 * 호출 체인:
 *   blk_mq_alloc_sched_res → [blk_mq_alloc_sched_tags]
 *   → blk_mq_alloc_map_and_rqs (per-hctx tag map 할당)
 */
struct elevator_tags *blk_mq_alloc_sched_tags(struct blk_mq_tag_set *set,
		unsigned int nr_hw_queues, unsigned int nr_requests)
{
	/* [한국어] nr_tags: 할당할 tag map 수 — shared면 1, per-SQ면 nr_hw_queues */
	unsigned int nr_tags;
	int i;
	/* [한국어] et: 반환할 elevator_tags (nr_hw_queues, nr_requests, tags[] 보관) */
	struct elevator_tags *et;
	/* [한국어] GFP_NOIO: I/O 경로 메모리 할당 시 재귀 I/O 방지;
	 * __GFP_ZERO: 제로 초기화; __GFP_NOWARN|__GFP_NORETRY: 실패해도 경고 없이 NULL 반환 */
	gfp_t gfp = GFP_NOIO | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY;

	/* [한국어] shared tags 모드: 모든 NVMe SQ가 하나의 tag pool 공유 → tag map 1개만 필요 */
	if (blk_mq_is_shared_tags(set->flags))
		nr_tags = 1;
	else
		/* [한국어] per-SQ: 각 hctx(NVMe SQ)마다 독립 tag map → nr_hw_queues개 필요 */
		nr_tags = nr_hw_queues;

	/* [한국어] kmalloc_flex: elevator_tags + tags[nr_tags] flexible array 한 번에 할당 */
	et = kmalloc_flex(*et, tags, nr_tags, gfp);
	if (!et)
		return NULL;

	/* [한국어] nr_requests: scheduler가 관리할 최대 request 수 기록 (SQ depth와 연동) */
	et->nr_requests = nr_requests;
	/* [한국어] nr_hw_queues: 해제 시 루프 상한으로 사용 */
	et->nr_hw_queues = nr_hw_queues;

	if (blk_mq_is_shared_tags(set->flags)) {
		/* Shared tags are stored at index 0 in @tags. */
		/* [한국어] 공유 tag map: BLK_MQ_NO_HCTX_IDX = 특정 hctx 미귀속; MAX_SCHED_RQ = 공유 pool 최대 크기 */
		et->tags[0] = blk_mq_alloc_map_and_rqs(set, BLK_MQ_NO_HCTX_IDX,
					MAX_SCHED_RQ);
		if (!et->tags[0])
			/* [한국어] 할당 실패: et만 해제 후 NULL 반환 */
			goto out;
	} else {
		/* [한국어] per-SQ: 각 hctx i에 대해 nr_requests 크기의 독립 tag map 할당 */
		for (i = 0; i < et->nr_hw_queues; i++) {
			et->tags[i] = blk_mq_alloc_map_and_rqs(set, i,
					et->nr_requests);
			if (!et->tags[i])
				/* [한국어] i번째 실패: 0~i-1 tag map 롤백 필요 */
				goto out_unwind;
		}
	}

	return et;
out_unwind:
	/* [한국어] out_unwind: 실패 직전까지 할당된 per-SQ tag map을 역순으로 해제 */
	while (--i >= 0)
		blk_mq_free_map_and_rqs(set, et->tags[i], i);
out:
	/* [한국어] elevator_tags 구조체 자체 해제 */
	kfree(et);
	return NULL;
}

/*
 * [한국어]
 * blk_mq_alloc_sched_res - 하나의 queue에 대한 elevator_resources 할당
 *
 * @q:            NVMe namespace request_queue
 * @type:         elevator_type (alloc_sched_data ops 호출 및 data 초기화)
 * @res:          결과를 저장할 elevator_resources (out parameter)
 * @nr_hw_queues: NVMe SQ 수 (tag pool 크기 계산에 사용)
 * @return:       0 = 성공; -ENOMEM = 메모리 부족
 *
 * elevator 전환 시 새 elevator를 위해 필요한 자원(tag pool + private data)을 미리 할당한다.
 * blk_mq_default_nr_requests(set)으로 SQ depth 기본값을 계산해 tag pool 크기를 결정한다.
 * private data 할당 실패 시 이미 할당한 tag pool을 롤백한다.
 */
int blk_mq_alloc_sched_res(struct request_queue *q,
		struct elevator_type *type,
		struct elevator_resources *res,
		unsigned int nr_hw_queues)
{
	/* [한국어] set: NVMe 장치 blk_mq_tag_set — shared tags 여부, default nr_requests 값 보유 */
	struct blk_mq_tag_set *set = q->tag_set;

	/* [한국어] blk_mq_default_nr_requests(set): tagset 기반 SQ depth 기본값으로 tag pool 크기 결정 */
	res->et = blk_mq_alloc_sched_tags(set, nr_hw_queues,
			blk_mq_default_nr_requests(set));
	if (!res->et)
		/* [한국어] tag pool 할당 실패: scheduler shadow CID pool 없음 → 전환 불가 */
		return -ENOMEM;

	/* [한국어] blk_mq_alloc_sched_data(): elevator_type->ops.alloc_data()로 private 구조체 할당
	 * (mq-deadline: dd_per_prio 배열; BFQ: bfq_data; kyber: kyber_queue) */
	res->data = blk_mq_alloc_sched_data(q, type);
	if (IS_ERR(res->data)) {
		/* [한국어] private data 실패: 이미 할당한 et(tag pool) 롤백 */
		blk_mq_free_sched_tags(res->et, set);
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * blk_mq_alloc_sched_res_batch - tagset의 모든 queue에 elevator 자원 일괄 할당
 *
 * @elv_tbl:      queue id → elv_change_ctx xarray (blk_mq_alloc_sched_ctx_batch로 미리 준비)
 * @set:          NVMe 장치 blk_mq_tag_set
 * @nr_hw_queues: 새 NVMe SQ 수 (tag pool 크기 계산)
 * @return:       0 = 성공; 음수 = 오류 (할당된 자원은 out_unwind에서 자동 해제)
 *
 * update_nr_hwq_lock write lock 하에 호출되며, elevator가 붙은 queue에 대해
 * blk_mq_alloc_sched_res()를 호출해 tag pool과 private data를 할당한다.
 * 중간에 실패하면 list_for_each_entry_continue_reverse로 역추적해 성공한 것들을 롤백한다.
 */
int blk_mq_alloc_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set, unsigned int nr_hw_queues)
{
	struct elv_change_ctx *ctx;
	/* [한국어] 순회 및 롤백에 사용할 NVMe namespace queue 포인터 */
	struct request_queue *q;
	/* [한국어] 초기값 -ENOMEM: queue가 없거나 첫 번째 queue에서 실패 시 이 값 반환 */
	int ret = -ENOMEM;

	/* [한국어] update_nr_hwq_lock write lock 보유 확인 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		/* [한국어] elevator가 있는 queue만 tag pool/private data 할당 */
		if (q->elevator) {
			/* [한국어] xa_load(): 이 queue의 elv_change_ctx 조회 */
			ctx = xa_load(elv_tbl, q->id);
			if (WARN_ON_ONCE(!ctx)) {
				/* [한국어] ctx가 없으면 alloc_sched_ctx_batch와 불일치 — 프로그래밍 오류 */
				ret = -ENOENT;
				goto out_unwind;
			}

			/* [한국어] 이 queue의 elevator type으로 tag pool + private data 할당 */
			ret = blk_mq_alloc_sched_res(q, q->elevator->type,
					&ctx->res, nr_hw_queues);
			if (ret)
				/* [한국어] 할당 실패: 이미 할당된 다른 queue들 롤백 */
				goto out_unwind;
		}
	}
	return 0;

out_unwind:
	/* [한국어] list_for_each_entry_continue_reverse: 실패한 q 이전까지 역방향으로 롤백 */
	list_for_each_entry_continue_reverse(q, &set->tag_list, tag_set_list) {
		if (q->elevator) {
			ctx = xa_load(elv_tbl, q->id);
			if (ctx)
				/* [한국어] 성공적으로 할당했던 자원 해제 */
				blk_mq_free_sched_res(&ctx->res,
						ctx->type, set);
		}
	}
	return ret;
}

/* caller must have a reference to @e, will grab another one if successful */
/*
 * [한국어]
 * blk_mq_init_sched - request_queue에 elevator(IO 스케줄러)를 초기화하고 연결
 *
 * @q:   NVMe namespace request_queue
 * @e:   초기화할 elevator_type (호출자가 참조를 이미 보유)
 * @res: blk_mq_alloc_sched_res()로 미리 할당된 elevator_resources (et + data)
 * @return: 0 = 성공; 음수 = 실패 (자원 롤백 포함)
 *
 * elevator 전환(elevator_switch)의 핵심 단계로 다음을 수행한다:
 * 1) elevator_alloc(q, e, res): elevator_queue 구조체 할당 + q->elevator 등록
 * 2) q->nr_requests = et->nr_requests: queue depth를 scheduler tag pool 크기로 갱신
 * 3) shared tags면 q->sched_shared_tags 설정 + blk_mq_tag_update_sched_shared_tags()
 * 4) 각 hctx->sched_tags에 per-SQ 또는 shared tag map 연결
 * 5) e->ops.init_sched(q, eq): scheduler 전역 초기화 (mq-deadline rb-tree, BFQ bfqd 등)
 * 6) e->ops.init_hctx(hctx, i): hctx별 scheduler context 초기화 (per-SQ 자원 설정)
 * 실패 시 out 경로에서 sched_tags 정리 + kobject_put + q->elevator = NULL.
 *
 * 호출 체인:
 *   elevator_switch → elevator_change_done → [blk_mq_init_sched]
 */
int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e,
		struct elevator_resources *res)
{
	/* [한국어] flags: shared tags 여부 등 tagset 설정 */
	unsigned int flags = q->tag_set->flags;
	/* [한국어] et: 미리 할당된 scheduler shadow CID pool */
	struct elevator_tags *et = res->et;
	/* [한국어] hctx: 각 NVMe SQ/CQ hardware context */
	struct blk_mq_hw_ctx *hctx;
	struct elevator_queue *eq;
	unsigned long i;
	int ret;

	/* [한국어] elevator_alloc(): elevator_queue 할당 + q->elevator 등록 + kobject 초기화 */
	eq = elevator_alloc(q, e, res);
	if (!eq)
		return -ENOMEM;

	/* [한국어] q->nr_requests를 scheduler tag pool 크기로 설정:
	 * elevator가 이 많큼의 request를 동시에 관리할 수 있어야 NVMe SQ를 최대 활용 가능 */
	q->nr_requests = et->nr_requests;

	if (blk_mq_is_shared_tags(flags)) {
		/* Shared tags are stored at index 0 in @et->tags. */
		/* [한국어] 모든 hctx(NVMe SQ)가 공유할 scheduler tag map 포인터 등록 */
		q->sched_shared_tags = et->tags[0];
		/* [한국어] 공유 tag map의 크기(nr_requests)를 실제 tag bitmap에 반영 */
		blk_mq_tag_update_sched_shared_tags(q, et->nr_requests);
	}

	/* [한국어] 모든 hctx(NVMe SQ)에 scheduler tag map 연결:
	 * hctx->sched_tags는 dispatch 시 shadow CID를 확보하는 데 사용 */
	queue_for_each_hw_ctx(q, hctx, i) {
		if (blk_mq_is_shared_tags(flags))
			/* [한국어] shared: 모든 SQ가 같은 tag pool → q->sched_shared_tags 연결 */
			hctx->sched_tags = q->sched_shared_tags;
		else
			/* [한국어] per-SQ: 각 SQ 독립 tag pool → et->tags[i] 연결 */
			hctx->sched_tags = et->tags[i];
	}

	/* [한국어] ops.init_sched(): elevator 전역 자원 초기화
	 * (mq-deadline: 우선순위별 rb-tree; BFQ: bfq_data 구조체; kyber: kyber_queue) */
	ret = e->ops.init_sched(q, eq);
	if (ret)
		goto out;

	/* [한국어] ops.init_hctx(): hctx별 elevator context 초기화; NVMe SQ당 per-ctx 자료구조 설정 */
	queue_for_each_hw_ctx(q, hctx, i) {
		if (e->ops.init_hctx) {
			ret = e->ops.init_hctx(hctx, i);
			if (ret) {
				/* [한국어] init_hctx 실패: 이전에 성공한 hctx들은 blk_mq_exit_sched가 정리 */
				blk_mq_exit_sched(q, eq);
				/* [한국어] elevator_queue kobject 참조 해제 → 0이 되면 elevator_release() */
				kobject_put(&eq->kobj);
				return ret;
			}
		}
	}
	return 0;

out:
	/* [한국어] init_sched 실패 경로: hctx->sched_tags 및 shared_tags 포인터 초기화 */
	blk_mq_sched_tags_teardown(q, flags);
	/* [한국어] elevator_queue 참조 해제 → q->elevator가 NULL로 초기화되며 scheduler 미등록 상태로 */
	kobject_put(&eq->kobj);
	/* [한국어] q->elevator=NULL: kobject_put 내의 elevator_release()에서 설정됨;
	 * 명시적 NULL 대입은 이미 이루어짐 — 여기서 중복 설정하지 않음 */
	q->elevator = NULL;
	return ret;
}

/*
 * called in either blk_queue_cleanup or elevator_switch, tagset
 * is required for freeing requests
 */
/*
 * [한국어]
 * blk_mq_sched_free_rqs - scheduler shadow tag pool에 남은 request 해제
 *
 * @q: NVMe namespace request_queue
 *
 * elevator 종료(blk_mq_exit_sched) 또는 queue cleanup 시 scheduler shadow tag pool에
 * 아직 남아있는 request들을 해제한다. shared tags면 q->sched_shared_tags의 request들을,
 * per-SQ 모드면 각 hctx->sched_tags의 request들을 blk_mq_free_rqs()로 해제한다.
 * tag map 자체(elevator_tags)는 별도로 blk_mq_free_sched_tags()에서 해제한다.
 *
 * 호출 체인:
 *   blk_mq_exit_sched → [blk_mq_sched_free_rqs]
 *   blk_queue_cleanup → [blk_mq_sched_free_rqs]
 */
void blk_mq_sched_free_rqs(struct request_queue *q)
{
	/* [한국어] 각 NVMe SQ(hctx) 순회용 */
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* [한국어] shared tags: 공유 tag pool의 모든 잔여 request를 한 번에 해제 */
	if (blk_mq_is_shared_tags(q->tag_set->flags)) {
		blk_mq_free_rqs(q->tag_set, q->sched_shared_tags,
				BLK_MQ_NO_HCTX_IDX);
	} else {
		/* [한국어] per-SQ: 각 hctx->sched_tags에 남은 request 해제 */
		queue_for_each_hw_ctx(q, hctx, i) {
			/* [한국어] sched_tags가 NULL이면(teardown 완료 또는 미설정) 건너뜀 */
			if (hctx->sched_tags)
				blk_mq_free_rqs(q->tag_set,
						hctx->sched_tags, i);
		}
	}
}

/*
 * [한국어]
 * blk_mq_exit_sched - request_queue에서 elevator(IO 스케줄러)를 제거하고 자원 정리
 *
 * @q: NVMe namespace request_queue
 * @e: 제거할 elevator_queue
 *
 * elevator 전환이나 queue 종료 시 호출되며 다음 순서로 정리한다:
 * 1) 각 hctx에 대해 ops.exit_hctx()로 per-SQ scheduler context 해제, sched_data=NULL
 * 2) ops.exit_sched()로 scheduler 전역 자원(rb-tree, bfqd 등) 해제
 * 3) blk_mq_sched_tags_teardown()으로 hctx->sched_tags 포인터 NULL 처리
 * 4) ELEVATOR_FLAG_DYING 비트 설정: 새 request가 이 elevator에 삽입되지 않도록 차단
 * 5) q->elevator = NULL: queue가 더 이상 scheduler를 참조하지 않음
 *
 * 호출 체인:
 *   elevator_change_done(실패) OR blk_queue_cleanup → [blk_mq_exit_sched]
 */
void blk_mq_exit_sched(struct request_queue *q, struct elevator_queue *e)
{
	/* [한국어] NVMe SQ/CQ hardware context 순회용 */
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;
	/* [한국어] flags: 마지막 hctx의 flags — shared tags 판단에 사용 */
	unsigned int flags = 0;

	/* [한국어] 모든 hctx(NVMe SQ)에 대해 per-SQ scheduler context 해제 */
	queue_for_each_hw_ctx(q, hctx, i) {
		/* [한국어] exit_hctx: per-SQ 자원(mq-deadline per-prio 큐 등) 해제;
		 * sched_data != NULL 확인으로 중복 호출 방지 */
		if (e->type->ops.exit_hctx && hctx->sched_data) {
			e->type->ops.exit_hctx(hctx, i);
			/* [한국어] sched_data=NULL: 이중 해제 방지, dispatch 경로에서 이 hctx를 건너뜀 */
			hctx->sched_data = NULL;
		}
		/* [한국어] 마지막 hctx의 flags 보존: sched_tags_teardown에서 shared 여부 판단 */
		flags = hctx->flags;
	}

	/* [한국어] exit_sched: scheduler 전역 자원 해제
	 * (mq-deadline: dispatch_queue flush; BFQ: bfq_data 구조체; kyber: domain 해제) */
	if (e->type->ops.exit_sched)
		e->type->ops.exit_sched(e);
	/* [한국어] hctx->sched_tags 포인터 NULL 처리 — 이후 dispatch가 shadow tag를 참조하지 않도록 */
	blk_mq_sched_tags_teardown(q, flags);
	/* [한국어] ELEVATOR_FLAG_DYING: 새 request의 elevator 삽입을 차단하는 fence bit */
	set_bit(ELEVATOR_FLAG_DYING, &q->elevator->flags);
	/* [한국어] q->elevator=NULL: 이 queue가 더 이상 elevator를 사용하지 않음을 표시 */
	q->elevator = NULL;
}

/*
 * [한국어] blk-mq-sched.c 핵심 요약 (파일 끝)
 *
 * dispatch 흐름:
 *   blk_mq_submit_bio → blk_mq_sched_bio_merge (merge 시도)
 *                     ↓
 *   blk_mq_run_hw_queue → blk_mq_sched_dispatch_requests
 *                     ↓
 *   __blk_mq_sched_dispatch_requests
 *     ├─ [residual 있음] → dispatch_rq_list → nvme_queue_rq
 *     ├─ [elevator 있음] → blk_mq_do_dispatch_sched
 *     │      └─ __blk_mq_do_dispatch_sched (budget → CID → SQ)
 *     └─ [elevator 없음] → blk_mq_do_dispatch_ctx
 *            └─ 라운드 로빈 ctx → dispatch_rq_list → nvme_queue_rq
 *
 * 핵심 자원 관계:
 *   elevator_tags (et)       ← scheduler shadow CID pool
 *   hctx->sched_tags         ← et->tags[i] 또는 et->tags[0](shared)
 *   q->sched_shared_tags     ← et->tags[0] (shared 모드만)
 *   hctx->dispatch           ← SQ 거부된 residual request 목록
 *   hctx->ctx_map            ← sw queue 비트맵 (dispatch_ctx에서 사용)
 *
 * 동기화:
 *   hctx->lock              : hctx->dispatch 접근 직렬화
 *   ctx->lock               : ctx->rq_lists[type] 접근 직렬화
 *   update_nr_hwq_lock(write): nr_hw_queues 변경 시 배치 할당/해제 보호
 *   smp_mb() in restart     : SCHED_RESTART clear ↔ dispatch list 확인 순서 보장
 */
