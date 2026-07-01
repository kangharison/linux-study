// SPDX-License-Identifier: GPL-2.0
/*
 * blk-mq scheduling framework
 *
 * Copyright (C) 2016 Jens Axboe
 *
 * =======================================================================
 * NVMe SSD 관점 파일 개요
 * =======================================================================
 * 본 파일은 blk-mq(multiqueue block layer)의 I/O 스케줄링 프레임워크를
 * 구현한다. 응용 프로그램이 날린 bio가 request로 변환된 뒤, 어느
 * blk_mq_hw_ctx(hctx)를 거쳐 NVMe 컨트롤러의 Submission Queue(SQ)로
 * 날아갈지를 조율하는 단계를 담당한다.
 *
 * 대표적인 호출 경로:
 * blk_mq_submit_bio -> blk_mq_get_request -> ...
 * -> blk_mq_sched_dispatch_requests -> __blk_mq_sched_dispatch_requests
 * -> blk_mq_dispatch_rq_list -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * 따라서 NVMe SSD doorbell을 울리기 직전, 요청의 정렬/합병/태그 할당을
 * 수행하는 핵심 단계라 할 수 있다.
 *
 * 연관 파일:
 *   block/blk-mq.c        - request 할당/해제, hctx 관리
 *   block/blk-mq.h        - blk_mq_hw_ctx, blk_mq_ctx 구조체 정의
 *   block/blk-mq-debugfs.c- debugfs 등록
 *   drivers/nvme/host/pci.c (또는 rdma/tcp) - nvme_queue_rq, doorbell
 * =======================================================================
 */
#include <linux/kernel.h>          // 커널 기본 헤더
#include <linux/module.h>          // EXPORT_SYMBOL_GPL 등
#include <linux/list_sort.h>       // list_sort(), SQ batching 정렬용

#include <trace/events/block.h>    // blktrace 이벤트 (NVMe dispatch 추적)

#include "blk.h"                   // block layer 낶부 헤더
#include "blk-mq.h"                // blk-mq 핵심 구조체/함수
#include "blk-mq-debugfs.h"        // debugfs 등록
#include "blk-mq-sched.h"          // scheduler 인터페이스
#include "blk-wbt.h"               // writeback throttling (NVMe QoS 관련)

/*
 * Mark a hardware queue as needing a restart.
 */
/*
 * blk_mq_sched_mark_restart_hctx()
 *   목적: hctx를 다시 실행해야 함을 표시한다.
 *   NVMe 연결: hctx는 NVMe의 하나의 SQ/CQ 쌍에 대응한다. 이 함수는
 *              컨트롤러가 일시적으로 처리 여유가 없어 doorbell 발행을
 *              미뤄야 할 때 사용된다.
 *   호출 경로: __blk_mq_sched_dispatch_requests -> blk_mq_sched_mark_restart_hctx
 *             -> __blk_mq_sched_restart -> blk_mq_run_hw_queue
 */
void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx) // hctx == NVMe SQ/CQ 쌍
{
	if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state)) // 이미 restart pending이면 early return
		return;

	set_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state); // NVMe SQ 재출발 예약 (doorbell 지연 후 재시도)
}
EXPORT_SYMBOL_GPL(blk_mq_sched_mark_restart_hctx);

/*
 * __blk_mq_sched_restart()
 *   목적: SCHED_RESTART 플래그를 클리어하고 hctx를 실제로 재가동한다.
 *   NVMe 연결: hctx가 멈춰진 NVMe SQ를 다시 깨워 doorbell을 울릴 수 있게 한다.
 *   메모리 배리어: SCHED_RESTART 클리어와 hctx->dispatch 비어있음 확인
 *                 사이의 순서를 보장하여, 새 request가 dispatch list에
 *                 추가되었음을 놓치지 않는다.
 *   호출 경로: __blk_mq_sched_restart -> blk_mq_run_hw_queue
 *             -> blk_mq_sched_dispatch_requests
 */
void __blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx) // hctx -> NVMe SQ/CQ 쌍 재깨우기
{
	clear_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state); // restart pending 해제

	/*
	 * Order clearing SCHED_RESTART and list_empty_careful(&hctx->dispatch)
	 * in blk_mq_run_hw_queue(). Its pair is the barrier in
	 * blk_mq_dispatch_rq_list(). So dispatch code won't see SCHED_RESTART,
	 * meantime new request added to hctx->dispatch is missed to check in
	 * blk_mq_run_hw_queue().
	 */
	smp_mb(); /* NVMe doorbell 재발행 전 메모리 순서 보장: dispatch list의 신규 request가 보이도록 */

	blk_mq_run_hw_queue(hctx, true); // NVMe SQ dispatch work 재스케줄 -> doorbell 발행까지 이어짐
}

/*
 * sched_rq_cmp()
 *   목적: request list를 hctx 단위로 정렬하기 위한 비교 함수.
 *   NVMe 연결: 여러 NVMe SQ로 갈 수 있는 request들을 같은 hctx끼리
 *              모아 batch로 처리하면 캐시 효율과 doorbell 횟수를 줄일 수 있다.
 */
static int sched_rq_cmp(void *priv, const struct list_head *a,
			const struct list_head *b)
{
	struct request *rqa = container_of(a, struct request, queuelist); // a가 속한 request (NVMe 명령 후보)
	struct request *rqb = container_of(b, struct request, queuelist); // b가 속한 request (NVMe 명령 후보)

	return rqa->mq_hctx > rqb->mq_hctx; // 같은 NVMe SQ/hctx끼리 모으기 위한 대소 비교
}

/*
 * blk_mq_dispatch_hctx_list()
 *   목적: rq_list에서 같은 hctx를 가진 request들을 묶어 한 번에 dispatch.
 *   NVMe 연결: 서로 다른 NVMe SQ로 향하는 request들을 분리하여,
 *              각 SQ에 맞는 batch를 nvme_queue_rq로 전달한다.
 *   호출 경로: __blk_mq_do_dispatch_sched -> blk_mq_dispatch_hctx_list
 *             -> blk_mq_dispatch_rq_list -> nvme_queue_rq
 */
static bool blk_mq_dispatch_hctx_list(struct list_head *rq_list) // rq_list: NVMe SQ 후보 request들
{
	struct blk_mq_hw_ctx *hctx =
		list_first_entry(rq_list, struct request, queuelist)->mq_hctx; // 첫 request의 목적 SQ/hctx
	struct request *rq;            // 순회용 포인터, 곧 NVMe 명령으로 변환될 request
	LIST_HEAD(hctx_list);          // 단일 NVMe SQ로 향하는 request들의 부분 리스트

	list_for_each_entry(rq, rq_list, queuelist) { // NVMe SQ 후보들을 순회
		if (rq->mq_hctx != hctx) { // 다른 NVMe SQ를 만나면 cut
			list_cut_before(&hctx_list, rq_list, &rq->queuelist); // 같은 SQ 집합 분리
			goto dispatch;         // 분리된 batch를 해당 SQ로 dispatch
		}
	}
	list_splice_tail_init(rq_list, &hctx_list); // 모두 같은 SQ면 전체를 hctx_list로 이동

/*
 * 여기서부터 hctx_list는 단일 NVMe SQ로 향하는 request들만 담고 있다.
 */
dispatch:
	return blk_mq_dispatch_rq_list(hctx, &hctx_list, false); // 단일 NVMe SQ batch -> nvme_queue_rq
}

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
 * __blk_mq_do_dispatch_sched()
 *   목적: IO scheduler(elevator)가 관리하는 큐에서 request를 꺼내
 *         NVMe로 dispatch할 준비를 한다.
 *   NVMe 연결:
 *     - q->elevator: mq-deadline, bfq, kyber 등 NVMe SQ로 가기 전의
 *                    재정렬/병합 정책을 담당.
 *     - blk_mq_get_dispatch_budget(): NVMe 컨트롤러가 감당할 수 있는
 *                                     동시 처리 개수(budget)를 확보.
 *     - blk_mq_get_driver_tag(): 실제 NVMe CID(command identifier)에
 *                                대응하는 driver tag 할당.
 *   구조체 필드 연관:
 *     - hctx->dispatch_busy: NVMe SQ가 가득 찼음을 알리는 backpressure
 *                            신호. true이면 max_dispatch=1로 제한.
 *     - hctx->queue->nr_requests: 이 큐에서 한 번에 dispatch할 수 있는
 *                                 상한. NVMe SQ depth와 관련된다.
 *     - rq->mq_hctx: 이 request가 최종적으로 탑승할 NVMe SQ/hctx.
 *     - rq->queuelist: scheduler 안에 request를 연결하는 리스트.
 *   호출 경로:
 *     blk_mq_sched_dispatch_requests -> blk_mq_do_dispatch_sched
 *     -> __blk_mq_do_dispatch_sched -> blk_mq_dispatch_rq_list
 *     -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
static int __blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx) // hctx == NVMe SQ/CQ
{
	struct request_queue *q = hctx->queue;      // request_queue, NVMe namespace 큐
	struct elevator_queue *e = q->elevator;     // IO scheduler (mq-deadline/bfq/kyber)
	bool multi_hctxs = false, run_queue = false; // multi_hctxs: 여러 NVMe SQ로 흩어짐 여부
	bool dispatched = false, busy = false;      // busy: hctx->dispatch 잔여 작업 존재
	unsigned int max_dispatch;                  // 이번 루프에서 NVMe SQ로 별낼 최대 request 수
	LIST_HEAD(rq_list);                         // scheduler에서 뽑은 NVMe 명령 후보들
	int count = 0;                              // 실제로 뽑은 request/CID 후보 수

	/*
	 * NVMe SQ가 바쁘면 한 번에 하나씩만 날려 혼잡을 완화한다.
	 */
	if (hctx->dispatch_busy)                    // NVMe SQ full backpressure 감지
		max_dispatch = 1;                       // SQ 여유 불확실 -> conservative 1개만
	else
		max_dispatch = hctx->queue->nr_requests; // SQ가 여유로우면 queue depth까지 batch

	do {
		struct request *rq;                     // 이번에 dispatch할 NVMe 명령 후보
		int budget_token;                       // NVMe 컨트롤러 동시처리 슬롯 토큰

		/*
		 * scheduler가 처리할 작업이 없으면 중단.
		 */
		if (e->type->ops.has_work && !e->type->ops.has_work(hctx)) // scheduler 큐에 NVMe 후보 없음
			break;

		/*
		 * hctx->dispatch에 남은 request가 있으면 먼저 처리해야
		 * flush 등이 기아(starvation)되지 않는다.
		 */
		if (!list_empty_careful(&hctx->dispatch)) { // 이전에 NVMe SQ가 거부한 잔여 request 존재
			busy = true;
			break;
		}

		/*
		 * NVMe 컨트롤러가 받아들일 수 있는 budget 토큰을 확보.
		 * SCSI 외에는 대부분 .get_budget 미구현이므로 성공으로 본다
		 * (추정).
		 */
		budget_token = blk_mq_get_dispatch_budget(q); // NVMe 동시처리 용량 확보 (SCSI 외 no-op 추정)
		if (budget_token < 0)                   // 컨트롤러 용량 초과
			break;

		/*
		 * elevator로부터 다음에 dispatch할 request를 꺼낸다.
		 */
		rq = e->type->ops.dispatch_request(hctx); // scheduler가 고른 다음 NVMe 명령
		if (!rq) {
			blk_mq_put_dispatch_budget(q, budget_token); // CID 할당 전 실패 -> budget 반납
			/*
			 * We're releasing without dispatching. Holding the
			 * budget could have blocked any "hctx"s with the
			 * same queue and if we didn't dispatch then there's
			 * no guarantee anyone will kick the queue.  Kick it
			 * ourselves.
			 */
			run_queue = true;                   // NVMe SQ를 다시 깨워 재시도 요청
			break;
		}

		/*
		 * request가 budget을 소유. 만약 뒤에서 실제로 driver에
		 * queue되지 않으면 blk_mq_dispatch_rq_list()에서 반납된다.
		 */
		blk_mq_set_rq_budget_token(rq, budget_token); // request가 NVMe 동시처리 슬롯 소유

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		list_add_tail(&rq->queuelist, &rq_list); // NVMe 명령 후보 리스트에 추가
		count++;
		if (rq->mq_hctx != hctx)                // 이 request가 다른 NVMe SQ로 가면 batch 분리 필요
			multi_hctxs = true;

		/*
		 * If we cannot get tag for the request, stop dequeueing
		 * requests from the IO scheduler. We are unlikely to be able
		 * to submit them anyway and it creates false impression for
		 * scheduling heuristics that the device can take more IO.
		 *
		 * NVMe 관점: driver tag는 NVMe CID에 해당한다. CID가 없으면
		 *           이 request를 실제 SQ에 넣을 수 없으므로 더 이상
		 *           스케줄러에서 뽑지 않는다.
		 */
		if (!blk_mq_get_driver_tag(rq))         // NVMe CID(tag) 할당 실패 -> SQ full 또는 tag 고갈
			break;
	} while (count < max_dispatch);             // NVMe SQ depth 또는 congestion 상한까지 반복

	if (!count) {
		if (run_queue)
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY); // 3ms 후 NVMe SQ 재가동
	} else if (multi_hctxs) {
		/*
		 * Requests from different hctx may be dequeued from some
		 * schedulers, such as bfq and deadline.
		 *
		 * Sort the requests in the list according to their hctx,
		 * dispatch batching requests from same hctx at a time.
		 *
		 * NVMe 관점: bfq/deadline 같은 스케줄러는 여러 NVMe SQ로
		 *           흩어질 수 있는 request를 한 리스트에 담는다.
		 *           같은 SQ로 가는 request끼리 모아 doorbell 횟수를
		 *           줄이고 CPU 캐시 효율을 높인다.
		 */
		list_sort(NULL, &rq_list, sched_rq_cmp); // SQ별로 request 재배열 -> doorbell batching
		do {
			dispatched |= blk_mq_dispatch_hctx_list(&rq_list); // 한 SQ씩 batch dispatch
		} while (!list_empty(&rq_list));        // 모든 NVMe SQ 후보가 소진될 때까지
	} else {
		dispatched = blk_mq_dispatch_rq_list(hctx, &rq_list, false); // 단일 NVMe SQ로 일괄 dispatch
	}

	if (busy)
		return -EAGAIN;                         // hctx->dispatch 잔존 -> NVMe flush 기아 방지 재호출
	return !!dispatched;                        // NVMe SQ로 1개 이상 별냈는지 반환
}

/*
 * blk_mq_do_dispatch_sched()
 *   목적: scheduler dispatch를 1초(HZ) 동안 반복하여 NVMe SQ에 최대한
 *         많은 request를 밀어넣는다.
 *   NVMe 연결: NVMe SQ depth가 허용하는 한, 계속해서 CID를 할당받아
 *              doorbell까지 이르는 요청을 발행한다.
 */
static int blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx) // hctx -> NVMe SQ/CQ
{
	unsigned long end = jiffies + HZ;           // 최대 1초간 NVMe SQ 채우기 시도
	int ret;

	do {
		ret = __blk_mq_do_dispatch_sched(hctx); // scheduler -> NVMe SQ dispatch 시도
		if (ret != 1)                           // SQ full/budget 부족/작업 없음
			break;
		if (need_resched() || time_is_before_jiffies(end)) { // 1초 경과 또는 선점 필요
			blk_mq_delay_run_hw_queue(hctx, 0); // 다음 스케줄 시점에 NVMe SQ 재시도
			break;
		}
	} while (1);

	return ret;
}

/*
 * blk_mq_next_ctx()
 *   목적: 같은 hctx에 속한 software context(ctx)들을 라운드 로빈으로
 *         순회한다.
 *   NVMe 연결: NVMe 드라이버는 보통 CPU당 sw queue(ctx)를 두고, 이를
 *              순회하며 공정하게 NVMe SQ로 옮긴다.
 *   구조체 필드:
 *     - ctx->index_hw[hctx->type]: 이 ctx가 hctx 내에서 갖는 인덱스.
 *     - hctx->nr_ctx: 이 hctx에 매핑된 sw queue 개수.
 *     - hctx->ctxs[]: sw queue 배열.
 */
static struct blk_mq_ctx *blk_mq_next_ctx(struct blk_mq_hw_ctx *hctx,
					  struct blk_mq_ctx *ctx) // 현재 CPU sw queue -> 다음 sw queue
{
	unsigned short idx = ctx->index_hw[hctx->type]; // 현재 ctx의 hctx 내 인덱스

	if (++idx == hctx->nr_ctx)                  // 마지막 ctx였으면 처음으로 wrap
		idx = 0;

	return hctx->ctxs[idx];                     // 다음 CPU sw queue 반환 -> NVMe SQ로 공정 이동
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
 * blk_mq_do_dispatch_ctx()
 *   목적: IO scheduler가 없을 때, per-CPU sw queue(ctx)에서 직접 request를
 *         꺼내 NVMe SQ로 dispatch한다.
 *   NVMe 연결:
 *     - elevator가 없는 none 스케줄러(nvme 등에서 흔히 사용) 상황에서
 *       software queue에서 곧바로 NVMe 명령을 만들어 본다.
 *     - hctx->ctx_map: 어느 ctx에 처리할 request가 있는지 비트맵.
 *     - blk_mq_dequeue_from_ctx(): ctx->rq_lists에서 request 추출.
 *     - round robin으로 ctx를 순회해 여러 CPU의 요청을 공정하게 NVMe
 *       SQ에 전달.
 *   호출 경로:
 *     blk_mq_sched_dispatch_requests -> __blk_mq_sched_dispatch_requests
 *     -> blk_mq_do_dispatch_ctx -> blk_mq_dispatch_rq_list
 *     -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
static int blk_mq_do_dispatch_ctx(struct blk_mq_hw_ctx *hctx) // hctx == NVMe SQ/CQ, scheduler 없음
{
	struct request_queue *q = hctx->queue;      // NVMe namespace request queue
	LIST_HEAD(rq_list);                         // 단일 sw queue에서 뽑은 NVMe 명령 후보
	struct blk_mq_ctx *ctx = READ_ONCE(hctx->dispatch_from); // 마지막으로 dispatch한 CPU sw queue
	int ret = 0;                                // -EAGAIN 시 flush 기아 방지 재호출 신호
	struct request *rq;

	do {
		int budget_token;                       // NVMe 동시처리 슬롯 토큰

		if (!list_empty_careful(&hctx->dispatch)) { // NVMe SQ가 반납한 잔여 request 우선
			ret = -EAGAIN;
			break;
		}

		/*
		 * 처리할 sw queue(ctx)가 남아있지 않으면 중단.
		 */
		if (!sbitmap_any_bit_set(&hctx->ctx_map)) // 모든 CPU sw queue가 비어 있음
			break;

		budget_token = blk_mq_get_dispatch_budget(q); // NVMe 컨트롤러 수용량 확보
		if (budget_token < 0)                   // 동시처리 한계
			break;

		/*
		 * 현재 ctx의 sw queue에서 request를 하나 꺼낸다.
		 */
		rq = blk_mq_dequeue_from_ctx(hctx, ctx); // CPU sw queue -> NVMe 명령 후보
		if (!rq) {
			blk_mq_put_dispatch_budget(q, budget_token); // CID 할당 전 실패 -> 반납
			/*
			 * We're releasing without dispatching. Holding the
			 * budget could have blocked any "hctx"s with the
			 * same queue and if we didn't dispatch then there's
			 * no guarantee anyone will kick the queue.  Kick it
			 * ourselves.
			 */
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY); // 3ms 후 NVMe SQ 재가동
			break;
		}

		blk_mq_set_rq_budget_token(rq, budget_token); // request가 NVMe 동시처리 슬롯 소유

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		list_add(&rq->queuelist, &rq_list);     // 단일 NVMe SQ 후보 리스트 추가

		/* round robin for fair dispatch */
		ctx = blk_mq_next_ctx(hctx, rq->mq_ctx); // 다음 CPU sw queue로 이동 -> 공정성

	} while (blk_mq_dispatch_rq_list(rq->mq_hctx, &rq_list, false)); // NVMe SQ에 삽입 성공 시 반복

	WRITE_ONCE(hctx->dispatch_from, ctx);       // 다음 dispatch 시작 ctx 저장 (lockless RCU 경쟁 완화)
	return ret;
}

/*
 * __blk_mq_sched_dispatch_requests()
 *   목적: hctx 하나에 대해 residual dispatch list 처리 후, scheduler 또는
 *         sw queue에서 request를 받아 NVMe SQ로 본 dispatch를 수행.
 *   NVMe 연결:
 *     - hctx->dispatch: 이전 dispatch 시도에서 NVMe SQ가 받지 못하고
 *                       남은 request들. NVMe controller queue full 등으로
 *                       인해 되돌아온 request라고 볼 수 있다.
 *     - hctx->dispatch_busy: SQ full 등의 혼잡 상태.
 *     - hctx->queue->elevator: scheduler 존재 여부.
 *   호출 경로:
 *     blk_mq_sched_dispatch_requests -> __blk_mq_sched_dispatch_requests
 *     -> (blk_mq_do_dispatch_sched | blk_mq_do_dispatch_ctx)
 *     -> blk_mq_dispatch_rq_list -> nvme_queue_rq -> nvme_submit_cmd
 */
static int __blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx) // hctx == NVMe SQ/CQ
{
	bool need_dispatch = false;                 // 추가 NVMe SQ dispatch 필요 여부
	LIST_HEAD(rq_list);                         // 이번에 NVMe SQ로 별낼 request들

	/*
	 * If we have previous entries on our dispatch list, grab them first for
	 * more fair dispatch.
	 */
	/*
	 * 먼저 hctx->dispatch에 남은 request를 처리한다.
	 * NVMe 관점: 이전에 NVMe SQ가 가득 차서 되돌아온 요청들을 우선 처리해
	 *           순서를 보존하고 flush 기아를 방지한다.
	 */
	if (!list_empty_careful(&hctx->dispatch)) { // lockless로 residual list 존재 확인 (NVMe abort/requeue 잔여물)
		spin_lock(&hctx->lock);                 // hctx->dispatch 접근 직렬화
		if (!list_empty(&hctx->dispatch))
			list_splice_init(&hctx->dispatch, &rq_list); // 잔여 request -> 이번 dispatch 리스트
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
	if (!list_empty(&rq_list)) {                // residual request가 splice됨
		/*
		 * 남은 request가 처리되었으므로, scheduler에서 더 뽑기 위해
		 * hctx 재시작을 표시한다.
		 */
		blk_mq_sched_mark_restart_hctx(hctx);   // NVMe SQ 재출발 예약
		if (!blk_mq_dispatch_rq_list(hctx, &rq_list, true)) // SQ로 batch 삽입
			return 0;                       // SQ가 전부를 받지 못함 -> 다음 restart 때 처리
		need_dispatch = true;                   // residual 처리 성공 -> scheduler에서 추가 확보
	} else {
		need_dispatch = hctx->dispatch_busy;    // SQ 혼잡 시에도 계속 dispatch 시도
	}

	/*
	 * elevator(mq-deadline/bfq/kyber)가 있으면 scheduler 경로를,
	 * 없으면 per-ctx sw queue 경로를 탄다.
	 */
	if (hctx->queue->elevator)                  // NVMe용 scheduler 사용 중이면
		return blk_mq_do_dispatch_sched(hctx);  // scheduler 정렬 후 NVMe SQ로

	/* dequeue request one by one from sw queue if queue is busy */
	if (need_dispatch)                          // NVMe SQ가 바쁘거나 residual 처리됨
		return blk_mq_do_dispatch_ctx(hctx);    // per-CPU sw queue -> NVMe SQ
	blk_mq_flush_busy_ctxs(hctx, &rq_list);     // NVMe SQ가 한가로우면 flush한꺼번에 처리
	blk_mq_dispatch_rq_list(hctx, &rq_list, true); // sw queue flush batch -> NVMe SQ
	return 0;
}

/*
 * blk_mq_sched_dispatch_requests()
 *   목적: hctx 단위 dispatch의 최상위 진입점. hctx가 멈추지 않았고
 *         queue가 quiesced되지 않았는지 확인한 후 dispatch를 시작.
 *   NVMe 연결:
 *     - stopped/quiesced 상태면 NVMe 컨트롤러로 명령을 병렬 발행하는
 *       것을 방지하기 위해 즉시 리턴.
 *     - -EAGAIN은 hctx->dispatch에 아직 처리할 request가 남았음을
 *       의미하며, flush 기아를 막기 위해 한 번 더 dispatch를 시도.
 *   호출 경로:
 *     blk_mq_run_hw_queue -> blk_mq_sched_dispatch_requests
 *     -> __blk_mq_sched_dispatch_requests -> ... -> nvme_queue_rq
 */
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx) // hctx == NVMe SQ/CQ
{
	struct request_queue *q = hctx->queue;      // NVMe namespace request queue

	/* RCU or SRCU read lock is needed before checking quiesced flag */
	if (unlikely(blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(q))) // NVMe controller reset/remove 중이면
		return;                             // SQ doorbell 발행 금지 (abort/drain 상태)

	/*
	 * A return of -EAGAIN is an indication that hctx->dispatch is not
	 * empty and we must run again in order to avoid starving flushes.
	 */
	if (__blk_mq_sched_dispatch_requests(hctx) == -EAGAIN) { // NVMe SQ 잔여 request 존재
		if (__blk_mq_sched_dispatch_requests(hctx) == -EAGAIN) // 한 번 더 residual 처리
			blk_mq_run_hw_queue(hctx, true); // NVMe SQ restart work 예약
	}
}

/*
 * blk_mq_sched_bio_merge()
 *   목적: bio가 scheduler나 sw queue에 들어가기 전, 기존 request와
 *         merge될 수 있는지 검사.
 *   NVMe 연결:
 *     - NVMe PRP/SGL entry 수를 줄이기 위해 인접한 sector의 bio를
 *       하나의 request로 합치는 것이 중요.
 *     - ctx->rq_lists[type]: per-CPU sw queue의 request list.
 *     - blk_bio_list_merge(): software queue에서 역방향으로 8개 정도
 *       검사하며 merge 가능 여부 확인.
 *   호출 경로:
 *     blk_mq_submit_bio -> blk_mq_sched_bio_merge
 *     -> (elevator bio_merge | blk_bio_list_merge)
 */
bool blk_mq_sched_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)               // bio의 segment 수 -> NVMe PRP/SGL 복잡도
{
	struct elevator_queue *e = q->elevator;     // scheduler 병합 정책
	struct blk_mq_ctx *ctx;                     // 현재 CPU sw queue
	struct blk_mq_hw_ctx *hctx;                 // bio가 매핑될 NVMe SQ/hctx
	bool ret = false;                           // merge 성공 여부
	enum hctx_type type;                        // hctx 타입 (default/read/poll 등)

	if (e && e->type->ops.bio_merge) {
		ret = e->type->ops.bio_merge(q, bio, nr_segs); // scheduler 낶에서 NVMe PRP/SGL 최적화 merge
		goto out_put;
	}

	ctx = blk_mq_get_ctx(q);                    // 현재 CPU의 sw queue 레퍼런스 획득
	hctx = blk_mq_map_queue(bio->bi_opf, ctx);  // bio -> NVMe SQ/hctx 매핑
	type = hctx->type;                          // poll hctx 등 구분
	if (list_empty_careful(&ctx->rq_lists[type])) // merge 대상 request 없음
		goto out_put;

	/* default per sw-queue merge */
	spin_lock(&ctx->lock);                      // sw queue 병합 직렬화
	/*
	 * Reverse check our software queue for entries that we could
	 * potentially merge with. Currently includes a hand-wavy stop
	 * count of 8, to not spend too much time checking for merges.
	 *
	 * NVMe 관점: NVMe I/O는 PRP/SGL로 변환되므로 segment 수가 많을수록
	 *           메모리 접근 비용이 증가. bio merge를 통해 segment를 줄이면
	 *           PRP list/SGL 크기가 감소한다 (추정).
	 */
	if (blk_bio_list_merge(q, &ctx->rq_lists[type], bio, nr_segs)) // 역방향 8개 검사 -> NVMe segment 최적화
		ret = true;

	spin_unlock(&ctx->lock);
out_put:
	return ret;
}

/*
 * blk_mq_sched_try_insert_merge()
 *   목적: request를 scheduler에 삽입할 때 merge를 시도.
 *   NVMe 연결: NVMe SQ에 넣기 전 마지막으로 인접한 request를 합쳐
 *             PRP/SGL entry 수를 줄인다.
 */
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq,
				   struct list_head *free)
{
	return rq_mergeable(rq) && elv_attempt_insert_merge(q, rq, free); // mergeable 검사 후 scheduler 삽입 병합 -> NVMe PRP/SGL 축소
}
EXPORT_SYMBOL_GPL(blk_mq_sched_try_insert_merge);

/* called in queue's release handler, tagset has gone away */
/*
 * blk_mq_sched_tags_teardown()
 *   목적: request_queue 해제 시 scheduler tag mapping을 제거.
 *   NVMe 연결: hctx->sched_tags는 NVMe SQ로 가기 전 scheduler가 사용하는
 *             shadow tag set. queue가 사라지면 정리.
 */
static void blk_mq_sched_tags_teardown(struct request_queue *q, unsigned int flags) // flags: shared tags 여부
{
	struct blk_mq_hw_ctx *hctx;                 // 각 NVMe SQ/hctx
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)           // 모든 NVMe SQ/hctx 순회
		hctx->sched_tags = NULL;                // scheduler tag map 레퍼런스 해제

	if (blk_mq_is_shared_tags(flags))
		q->sched_shared_tags = NULL;            // 공유 scheduler tag set 정리
}

void blk_mq_sched_reg_debugfs(struct request_queue *q) // q: NVMe namespace queue
{
	struct blk_mq_hw_ctx *hctx;                 // NVMe SQ/hctx별 debugfs
	unsigned int memflags;
	unsigned long i;

	memflags = blk_debugfs_lock(q);             // debugfs 등록 직렬화
	blk_mq_debugfs_register_sched(q);           // scheduler debugfs 등록 (NVMe latency 추적)
	queue_for_each_hw_ctx(q, hctx, i)           // 각 NVMe SQ/hctx에 대해
		blk_mq_debugfs_register_sched_hctx(q, hctx); // hctx별 scheduler debugfs
	blk_debugfs_unlock(q, memflags);
}

void blk_mq_sched_unreg_debugfs(struct request_queue *q) // q: NVMe namespace queue
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	blk_debugfs_lock_nomemsave(q);              // debugfs 해제 직렬화
	queue_for_each_hw_ctx(q, hctx, i)           // NVMe SQ/hctx 순회
		blk_mq_debugfs_unregister_sched_hctx(hctx); // hctx별 scheduler debugfs 제거
	blk_mq_debugfs_unregister_sched(q);         // scheduler debugfs 제거
	blk_debugfs_unlock_nomemrestore(q);
}

/*
 * blk_mq_free_sched_tags()
 *   목적: scheduler를 위해 할당된 tag map과 request들을 해제.
 *   NVMe 연결:
 *     - et->tags[]: 각 NVMe SQ/hctx에 대응하는 scheduler tag set.
 *     - shared tags일 경우 index 0의 tag set만 사용.
 *   구조체 필드:
 *     - et->nr_hw_queues: hctx/SQ 개수.
 *     - et->tags[i]: i번째 hctx용 scheduler tag map.
 */
void blk_mq_free_sched_tags(struct elevator_tags *et,
		struct blk_mq_tag_set *set)         // NVMe 장치 tagset
{
	unsigned long i;

	/* Shared tags are stored at index 0 in @tags. */
	if (blk_mq_is_shared_tags(set->flags))      // 여러 NVMe SQ가 tag pool 공유
		blk_mq_free_map_and_rqs(set, et->tags[0], BLK_MQ_NO_HCTX_IDX); // 공유 scheduler tag set 해제
	else {
		for (i = 0; i < et->nr_hw_queues; i++)  // NVMe SQ/hctx 개수만큼 반복
			blk_mq_free_map_and_rqs(set, et->tags[i], i); // per-SQ scheduler tag map 해제
	}

	kfree(et);                                  // elevator_tags 구조체 해제
}

void blk_mq_free_sched_res(struct elevator_resources *res,
		struct elevator_type *type,
		struct blk_mq_tag_set *set)
{
	if (res->et) {
		blk_mq_free_sched_tags(res->et, set);   // scheduler tag set 해제 (NVMe CID pool shadow)
		res->et = NULL;
	}
	if (res->data) {
		blk_mq_free_sched_data(type, res->data); // scheduler private data 해제
		res->data = NULL;
	}
}

void blk_mq_free_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	struct request_queue *q;                    // tagset에 속한 NVMe namespace queue
	struct elv_change_ctx *ctx;

	lockdep_assert_held_write(&set->update_nr_hwq_lock); // NVMe SQ 수 변경 시 write lock

	list_for_each_entry(q, &set->tag_list, tag_set_list) { // tagset의 모든 NVMe queue 순회
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		if (q->elevator) {                      // scheduler가 활성화된 NVMe queue만
			ctx = xa_load(elv_tbl, q->id);  // 해당 queue의 변경 컨텍스트
			if (!ctx) {
				WARN_ON_ONCE(1);
				continue;
			}
			blk_mq_free_sched_res(&ctx->res, ctx->type, set); // scheduler 자원 해제
		}
	}
}

void blk_mq_free_sched_ctx_batch(struct xarray *elv_tbl)
{
	unsigned long i;
	struct elv_change_ctx *ctx;

	xa_for_each(elv_tbl, i, ctx) {              // xarray에 남은 모든 변경 컨텍스트 순회
		xa_erase(elv_tbl, i);                   // xarray에서 제거
		kfree(ctx);                             // 변경 컨텍스트 해제
	}
}

int blk_mq_alloc_sched_ctx_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	struct request_queue *q;                    // tagset에 속한 NVMe namespace queue
	struct elv_change_ctx *ctx;

	lockdep_assert_held_write(&set->update_nr_hwq_lock); // NVMe SQ 수 변경 보호

	list_for_each_entry(q, &set->tag_list, tag_set_list) { // 모든 NVMe queue에 대해
		ctx = kzalloc_obj(struct elv_change_ctx); // 변경 컨텍스트 할당
		if (!ctx)
			return -ENOMEM;                 // NVMe scheduler 전환 메모리 부족

		if (xa_insert(elv_tbl, q->id, ctx, GFP_KERNEL)) { // queue id -> ctx 매핑
			kfree(ctx);
			return -ENOMEM;
		}
	}
	return 0;
}

/*
 * blk_mq_alloc_sched_tags()
 *   목적: scheduler가 사용할 tag map과 request pool을 할당.
 *   NVMe 연결:
 *     - nr_hw_queues: NVMe SQ 개수와 일치 (보통 hctx 개수).
 *     - nr_requests: scheduler가 관리할 최대 request 수. NVMe SQ depth를
 *                    초과하지 않도록 설정.
 *     - MAX_SCHED_RQ: scheduler tag의 최대 개수.
 *   구조체 필드:
 *     - et->nr_requests: scheduler tag pool 크기.
 *     - et->nr_hw_queues: hctx/SQ 수.
 *     - et->tags[]: hctx별 또는 shared tag map.
 */
struct elevator_tags *blk_mq_alloc_sched_tags(struct blk_mq_tag_set *set,
		unsigned int nr_hw_queues, unsigned int nr_requests) // nr_hw_queues == NVMe SQ 수, nr_requests == SQ depth 상한
{
	unsigned int nr_tags;                       // 할당할 tag map 개수 (shared면 1)
	int i;
	struct elevator_tags *et;                   // scheduler tag set (NVMe CID shadow pool)
	gfp_t gfp = GFP_NOIO | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY; // I/O 중 메모리 할당 플래그

	if (blk_mq_is_shared_tags(set->flags))
		nr_tags = 1;                            // 모든 NVMe SQ가 하나의 tag pool 공유
	else
		nr_tags = nr_hw_queues;                 // NVMe SQ당 독립 tag map

	et = kmalloc_flex(*et, tags, nr_tags, gfp); // elevator_tags + tags[] 할당
	if (!et)
		return NULL;                            // NVMe scheduler tag pool 할당 실패

	et->nr_requests = nr_requests;              // scheduler가 관리할 최대 request 수 (NVMe SQ depth 근접)
	et->nr_hw_queues = nr_hw_queues;            // NVMe SQ/hctx 수 기록

	if (blk_mq_is_shared_tags(set->flags)) {
		/* Shared tags are stored at index 0 in @tags. */
		et->tags[0] = blk_mq_alloc_map_and_rqs(set, BLK_MQ_NO_HCTX_IDX,
					MAX_SCHED_RQ);          // 공유 scheduler tag map/CID shadow pool 할당
		if (!et->tags[0])
			goto out;                       // 할당 실패 시 et 해제
	} else {
		for (i = 0; i < et->nr_hw_queues; i++) { // NVMe SQ/hctx 수만큼
			et->tags[i] = blk_mq_alloc_map_and_rqs(set, i,
					et->nr_requests);       // per-SQ scheduler tag map/CID shadow pool 할당
			if (!et->tags[i])
				goto out_unwind;        // 실패 시 이미 할당한 tag map 해제
		}
	}

	return et;
out_unwind:
	while (--i >= 0)
		blk_mq_free_map_and_rqs(set, et->tags[i], i); // 할당된 per-SQ tag map 롤백
out:
	kfree(et);                                  // elevator_tags 해제
	return NULL;
}

int blk_mq_alloc_sched_res(struct request_queue *q,
		struct elevator_type *type,
		struct elevator_resources *res,
		unsigned int nr_hw_queues)              // NVMe SQ 수
{
	struct blk_mq_tag_set *set = q->tag_set;    // NVMe 장치 tagset

	res->et = blk_mq_alloc_sched_tags(set, nr_hw_queues,
			blk_mq_default_nr_requests(set)); // NVMe SQ depth 기본값으로 scheduler tag pool 할당
	if (!res->et)
		return -ENOMEM;                         // scheduler tag/CID shadow pool 부족

	res->data = blk_mq_alloc_sched_data(q, type); // scheduler private data (mq-deadline/bfq 등)
	if (IS_ERR(res->data)) {
		blk_mq_free_sched_tags(res->et, set);   // tag pool 롤백
		return -ENOMEM;
	}

	return 0;
}

int blk_mq_alloc_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set, unsigned int nr_hw_queues)
{
	struct elv_change_ctx *ctx;
	struct request_queue *q;
	int ret = -ENOMEM;

	lockdep_assert_held_write(&set->update_nr_hwq_lock); // NVMe SQ 수 변경 write lock

	list_for_each_entry(q, &set->tag_list, tag_set_list) { // tagset의 모든 NVMe queue
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		if (q->elevator) {                      // scheduler가 붙은 NVMe queue만
			ctx = xa_load(elv_tbl, q->id);  // 변경 컨텍스트
			if (WARN_ON_ONCE(!ctx)) {
				ret = -ENOENT;
				goto out_unwind;
			}

			ret = blk_mq_alloc_sched_res(q, q->elevator->type,
					&ctx->res, nr_hw_queues); // scheduler tag pool + private data 할당
			if (ret)
				goto out_unwind;        // NVMe scheduler 자원 할당 실패
		}
	}
	return 0;

out_unwind:
	list_for_each_entry_continue_reverse(q, &set->tag_list, tag_set_list) { // 할당된 것 롤백
		if (q->elevator) {
			ctx = xa_load(elv_tbl, q->id);
			if (ctx)
				blk_mq_free_sched_res(&ctx->res,
						ctx->type, set); // NVMe scheduler 자원 해제
		}
	}
	return ret;
}

/* caller must have a reference to @e, will grab another one if successful */
/*
 * blk_mq_init_sched()
 *   목적: request_queue에 IO scheduler(elevator)를 초기화하고,
 *         각 hctx에 sched_tags를 연결.
 *   NVMe 연결:
 *     - q->nr_requests: scheduler가 관리할 request 수.
 *     - q->sched_shared_tags: 여러 hctx/NVMe SQ가 공유하는 scheduler tag set.
 *     - hctx->sched_tags: 각 NVMe SQ/hctx에 연결된 scheduler tag map.
 *     - e->ops.init_sched / init_hctx: scheduler별 초기화. NVMe I/O 특성에
 *       맞춘 정렬/우선순위 설정이 이 단계에서 이루어진다.
 *   호출 경로:
 *     elevator_switch -> blk_mq_init_sched
 */
int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e,
		struct elevator_resources *res)
{
	unsigned int flags = q->tag_set->flags;     // NVMe tagset flags (shared tags 등)
	struct elevator_tags *et = res->et;         // scheduler tag set (NVMe CID shadow)
	struct blk_mq_hw_ctx *hctx;                 // NVMe SQ/hctx
	struct elevator_queue *eq;
	unsigned long i;
	int ret;

	eq = elevator_alloc(q, e, res);             // elevator_queue 구조체 할당
	if (!eq)
		return -ENOMEM;

	q->nr_requests = et->nr_requests;           // queue depth를 scheduler tag pool 크기로 설정 (NVMe SQ depth 근접)

	if (blk_mq_is_shared_tags(flags)) {
		/* Shared tags are stored at index 0 in @et->tags. */
		q->sched_shared_tags = et->tags[0];     // 모든 NVMe SQ가 공유할 scheduler tag map
		blk_mq_tag_update_sched_shared_tags(q, et->nr_requests); // 공유 tag map 크기 갱신
	}

	/*
	 * 모든 hctx에 scheduler tag map을 연결한다.
	 * NVMe 관점: 각 NVMe SQ에 해당하는 hctx가 자신만의 tag map 또는
	 *           shared tag map을 갖게 된다.
	 */
	queue_for_each_hw_ctx(q, hctx, i) {         // NVMe SQ/hctx 순회
		if (blk_mq_is_shared_tags(flags))
			hctx->sched_tags = q->sched_shared_tags; // shared scheduler tag map 연결
		else
			hctx->sched_tags = et->tags[i];     // per-SQ scheduler tag map 연결
	}

	ret = e->ops.init_sched(q, eq);             // scheduler 전역 초기화 (mq-deadline/bfq 등)
	if (ret)
		goto out;

	queue_for_each_hw_ctx(q, hctx, i) {         // NVMe SQ/hctx별 초기화
		if (e->ops.init_hctx) {
			ret = e->ops.init_hctx(hctx, i); // per-SQ scheduler context 초기화
			if (ret) {
				blk_mq_exit_sched(q, eq);
				kobject_put(&eq->kobj);
				return ret;             // NVMe SQ scheduler 초기화 실패
			}
		}
	}
	return 0;

out:
	blk_mq_sched_tags_teardown(q, flags);       // hctx->sched_tags 정리
	kobject_put(&eq->kobj);                     // elevator_queue 참조 해제
	q->elevator = NULL;                         // scheduler 미설정 상태 복원
	return ret;
}

/*
 * called in either blk_queue_cleanup or elevator_switch, tagset
 * is required for freeing requests
 */
/*
 * blk_mq_sched_free_rqs()
 *   목적: scheduler가 관리하던 request들을 해제.
 *   NVMe 연결: NVMe 명령으로 변환되지 못하고 scheduler tag에 남아있던
 *             request들을 정리.
 */
void blk_mq_sched_free_rqs(struct request_queue *q) // q: NVMe namespace queue
{
	struct blk_mq_hw_ctx *hctx;                 // NVMe SQ/hctx
	unsigned long i;

	if (blk_mq_is_shared_tags(q->tag_set->flags)) {
		blk_mq_free_rqs(q->tag_set, q->sched_shared_tags,
				BLK_MQ_NO_HCTX_IDX);    // 공유 scheduler tag에 남은 request 해제
	} else {
		queue_for_each_hw_ctx(q, hctx, i) {     // NVMe SQ/hctx 순회
			if (hctx->sched_tags)
				blk_mq_free_rqs(q->tag_set,
						hctx->sched_tags, i); // per-SQ scheduler 잔여 request 해제
		}
	}
}

/*
 * blk_mq_exit_sched()
 *   목적: scheduler를 request_queue에서 제거하고 자원을 정리.
 *   NVMe 연결:
 *     - e->type->ops.exit_hctx: hctx/SQ별 scheduler 자원 해제.
 *     - e->type->ops.exit_sched: scheduler 전역 자원 해제.
 *     - ELEVATOR_FLAG_DYING 설정: 더 이상 새 NVMe 명령을 scheduler에
 *       넣지 않도록 표시.
 */
void blk_mq_exit_sched(struct request_queue *q, struct elevator_queue *e) // q: NVMe namespace queue
{
	struct blk_mq_hw_ctx *hctx;                 // NVMe SQ/hctx
	unsigned long i;
	unsigned int flags = 0;

	queue_for_each_hw_ctx(q, hctx, i) {         // NVMe SQ/hctx 순회
		if (e->type->ops.exit_hctx && hctx->sched_data) {
			e->type->ops.exit_hctx(hctx, i); // per-SQ scheduler context 해제
			hctx->sched_data = NULL;
		}
		flags = hctx->flags;
	}

	if (e->type->ops.exit_sched)
		e->type->ops.exit_sched(e);             // scheduler 전역 자원 해제
	blk_mq_sched_tags_teardown(q, flags);       // hctx->sched_tags 및 shared tags 정리
	set_bit(ELEVATOR_FLAG_DYING, &q->elevator->flags); // 새 NVMe 명령 scheduler 삽입 금지
	q->elevator = NULL;                         // queue에서 scheduler 제거
}

/* =======================================================================
 * NVMe 관점 핵심 요약
 * =======================================================================
 * - 본 파일은 blk-mq scheduler 프레임워크로, bio -> request 변환 후
 *   NVMe SQ로 전달되기 직전의 dispatch 단계를 제어한다.
 *   대표 호출 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> ...
 *   -> blk_mq_sched_dispatch_requests -> __blk_mq_sched_dispatch_requests
 *   -> blk_mq_dispatch_rq_list -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * - blk_mq_hw_ctx(hctx)는 NVMe의 하나의 SQ/CQ 쌍에 대응하며,
 *   hctx->dispatch는 NVMe SQ가 가득 차서 되돌아온 residual request들을
 *   담는다.
 *
 * - blk_mq_get_dispatch_budget()과 blk_mq_get_driver_tag()는 각각
 *   NVMe 컨트롤러의 동시 처리 용량과 CID(command identifier)에 해당하는
 *   자원을 확보하는 관문이다.
 *
 * - scheduler(mq-deadline/bfq/kyber)가 있으면 정렬 후 dispatch하고,
 *   없으면(none) per-CPU sw queue(ctx)에서 직접 NVMe SQ로 옮긴다.
 *
 * - bio merge는 NVMe PRP/SGL entry 수를 줄여 메모리 접근 비용을 낮추는
 *   데 기여한다 (추정).
 * ====================================================================== */
