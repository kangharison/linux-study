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
 * request를 꺼내 드라이버로 전달하고, 없으면("none") per-CPU sw queue에서
 * 곧바로 dispatch한다. 스케줄러 태그 풀의 할당·해제, bio merge 진입점,
 * hctx restart 메커니즘도 이 파일에 있다.
 *
 * 이 파일은 장치 종류와 무관한 일반 코드다. nvme·scsi 같은 드라이버 식별자가
 * 하나도 없고, 드라이버에 닿는 유일한 통로는 blk_mq_dispatch_rq_list()가
 * 수행하는 간접 호출 q->mq_ops->queue_rq()뿐이다. 아래의 NVMe 언급은 모두
 * "이 일반 코드가 NVMe 장치에서 어떻게 보이는가"를 설명하는 맥락이다.
 *
 * === 스케줄러가 있을 때와 없을 때, 요청 경로가 어떻게 갈라지는가 ===
 * 이 파일의 핵심 주제다. 갈림길은 딱 두 군데이고, 판정 기준은 둘 다
 * q->elevator가 NULL인지 하나뿐이다.
 *
 *   (A) 제출 시 병합 — blk_mq_sched_bio_merge()
 *       스케줄러 있음: e->type->ops.bio_merge() 콜백에 위임한다.
 *                      (mq-deadline이면 dd_bio_merge → blk_mq_sched_try_merge)
 *       스케줄러 없음: 이 CPU의 sw queue(ctx->rq_lists[type])만 훑어
 *                      blk_bio_list_merge()로 병합을 시도한다. 훨씬 좁은
 *                      범위지만 락도 그 sw queue 하나(ctx->lock)만 잡는다.
 *
 *   (B) 디스패치 — __blk_mq_sched_dispatch_requests()
 *       스케줄러 있음: blk_mq_do_dispatch_sched()
 *                      → ops.dispatch_request()로 한 개씩 꺼낸다.
 *       스케줄러 없음 & 이전에 바빴음: blk_mq_do_dispatch_ctx()
 *                      → sw queue들을 라운드 로빈으로 하나씩 꺼낸다.
 *       스케줄러 없음 & 한가함: blk_mq_flush_busy_ctxs()로 모든 sw queue를
 *                      한 번에 비워 통째로 dispatch한다(가장 빠른 경로).
 *
 * 멀티큐 NVMe의 기본값은 "none"이므로, 실제로 NVMe에서 도는 것은 대부분
 * 위의 "스케줄러 없음" 경로다(근거는 block/elevator.c의 elevator_set_default).
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [응용] write(2) → submit_bio() → blk_mq_submit_bio()
 *       ↓
 *   [blk-mq-sched] blk_mq_sched_bio_merge()  ← bio를 기존 request에 합치기 시도
 *       ↓
 *   [blk-mq-sched] blk_mq_sched_dispatch_requests()
 *       ↓ elevator 있음              ↓ elevator 없음("none")
 *   blk_mq_do_dispatch_sched()   blk_mq_do_dispatch_ctx() / flush_busy_ctxs
 *       ↓                              ↓
 *   blk_mq_dispatch_rq_list()  (block/blk-mq.c)
 *       ↓
 *   q->mq_ops->queue_rq()  ← 간접 호출. PCIe NVMe라면 이 함수 포인터가
 *                             nvme_queue_rq다(drivers/nvme/host/pci.c의
 *                             nvme_mq_ops.queue_rq).
 *
 * 실행 컨텍스트: 주로 프로세스 컨텍스트(blk_mq_run_hw_queue의 워크큐 실행 또는
 * 제출 스레드 직접 실행). hctx 단위로 동작하며, PCIe NVMe에서 hctx 하나는
 * I/O 큐 하나(SQ/CQ 쌍)에 대응한다 — nvme_alloc_io_tag_set()이
 * set->nr_hw_queues를 I/O 큐 개수로 잡기 때문이다.
 *
 * === 스케줄러 태그와 드라이버 태그의 분리 (NVMe 독자 필독) ===
 * 이 파일이 다루는 태그는 두 종류이고, 둘을 혼동하면 NVMe 동작을 잘못 이해하게 된다.
 *
 *   드라이버 태그 (rq->tag, tag_set->tags[])
 *     "실제로 장치에 떠 있을 수 있는 커맨드"마다 하나씩 배정되는 번호.
 *     NVMe에서 이 번호가 커맨드의 Command ID로 그대로 들어간다:
 *       nvme_cid(rq) = (genctr << 12) | rq->tag        (drivers/nvme/host/nvme.h)
 *       cmd->common.command_id = nvme_cid(req)         (drivers/nvme/host/core.c)
 *       nvme_tag_from_cid(cid) = cid & 0xfff           (완료 시 역변환)
 *     즉 CID의 하위 12비트가 곧 드라이버 태그이고, 상위 4비트는 태그 재사용
 *     오검출을 막는 세대 카운터다. 완료 인터럽트에서 CQE의 command_id로
 *     nvme_find_rq()가 request를 되찾는 것도 이 대응 덕분이다.
 *
 *   스케줄러 태그 (rq->internal_tag, hctx->sched_tags)
 *     "스케줄러 큐에 대기시켜 둘 수 있는 request"마다 하나씩 배정되는 번호.
 *     장치에는 전혀 보이지 않는 순수 소프트웨어 슬롯이다. NVMe Command ID가
 *     아니며, 컨트롤러는 이 번호의 존재조차 모른다.
 *
 * 왜 나누는가: 스케줄러가 재정렬을 하려면 "장치에 보낼 수 있는 것보다 많은"
 * 요청을 손에 쥐고 있어야 한다. 그런데 드라이버 태그를 미리 다 잡아 버리면
 * 그 태그들이 장치 큐를 점유한 것처럼 되어, 나중에 도착한 더 급한 요청이
 * 태그를 못 받는다. 그래서 스케줄러 단계에서는 논리적 슬롯만 잡고,
 * 실제로 내보내는 순간(__blk_mq_do_dispatch_sched의 blk_mq_get_driver_tag)에
 * 드라이버 태그를 따로 획득한다.
 * 크기: blk_mq_alloc_sched_res()가 blk_mq_default_nr_requests(set)
 *   = 2 * min(set->queue_depth, BLKDEV_DEFAULT_RQ=128)을 쓴다(block/blk-mq.h).
 *   큐 깊이가 큰 NVMe(보통 1023)에서는 2*128 = 256으로, 드라이버 태그보다 적다.
 * 스케줄러가 없으면("none") 이 분리 자체가 사라진다 — request 할당 시점에
 * 드라이버 태그를 바로 받고 internal_tag는 BLK_MQ_NO_TAG로 남는다
 * (block/blk-mq.c의 태그 할당 경로 참고).
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-mq.c: blk_mq_dispatch_rq_list(), blk_mq_run_hw_queue() — dispatch 실행;
 *     blk_mq_dequeue_from_ctx()로 sw queue에서 request 추출;
 *     blk_mq_get_driver_tag()로 드라이버 태그 획득
 *   - block/elevator.c: elevator_alloc() — elevator_queue 할당;
 *     e->type->ops.dispatch_request()로 정렬된 request 획득
 *   - block/blk-mq-tag.c: blk_mq_alloc_map_and_rqs(), blk_mq_free_map_and_rqs() —
 *     스케줄러 태그 풀 할당/해제
 *   - block/blk-merge.c: blk_mq_sched_try_merge() — 스케줄러가 부르는 병합 헬퍼
 *   - block/mq-deadline.c, bfq-iosched.c, kyber-iosched.c: ops 콜백 제공자
 * 공유 자료구조:
 *   - struct blk_mq_hw_ctx (blk-mq.h): dispatch(잔여 list), sched_tags(스케줄러 태그),
 *     dispatch_busy(드라이버가 BLK_STS_*RESOURCE로 밀어낸 정도), ctx_map(sw queue
 *     비트맵), state(BLK_MQ_S_SCHED_RESTART)
 *   - struct elevator_queue / elevator_type (elevator.h): ops vtable
 *   - struct elevator_tags (blk-mq-sched.h): nr_hw_queues, nr_requests, tags[]
 *
 * === 주요 함수/구조체 요약 ===
 * blk_mq_sched_dispatch_requests()   - dispatch 최상위; stopped/quiesced 체크 후 위임
 * __blk_mq_sched_dispatch_requests() - 잔여(hctx->dispatch) 처리 후 스케줄러/none 경로 분기
 * __blk_mq_do_dispatch_sched()       - 스케줄러에서 request를 꺼내 예산·드라이버 태그를
 *                                      확보한 뒤 드라이버로 넘긴다(이 파일의 핵심)
 * blk_mq_do_dispatch_ctx()           - "none" 경로; sw queue 라운드 로빈 dispatch
 * blk_mq_sched_bio_merge()           - 제출 경로의 bio 병합 진입점; 스케줄러 유무로 갈림
 * blk_mq_init_sched()                - elevator_queue 할당 + hctx sched_tags 연결 + init_sched
 * blk_mq_exit_sched()                - elevator 제거; exit_hctx/exit_sched + sched_tags 정리
 * blk_mq_alloc_sched_tags()          - 스케줄러 태그 풀(elevator_tags) 할당
 * blk_mq_sched_mark_restart_hctx()   - BLK_MQ_S_SCHED_RESTART 비트 세팅; 재가동 예약
 */
/* [한국어] 커널 기본 매크로(KERN_ERR 등)와 타입(size_t 등) */
#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL, module_init/exit 등 모듈 인프라 */
#include <linux/module.h>
/* [한국어] list_sort(): 여러 hctx로 흩어진 request 목록을 hctx 기준으로 묶어
 * 정렬할 때 쓴다(sched_rq_cmp). 같은 하드웨어 큐로 갈 것들을 붙여 놓아야
 * blk_mq_dispatch_rq_list()를 hctx당 한 번씩만 부를 수 있다. */
#include <linux/list_sort.h>

/* [한국어] blktrace의 block 이벤트 정의. 이 파일에서 직접 쓰지는 않지만
 * 포함된 blk-mq 헤더들의 추적점이 이 정의를 필요로 한다. */
#include <trace/events/block.h>

/* [한국어] block layer 내부 헤더: blk_queue_*, queue_limits 등 공통 정의 */
#include "blk.h"
/* [한국어] blk-mq 핵심 구조체(blk_mq_hw_ctx, blk_mq_ctx)와 내부 API */
#include "blk-mq.h"
/* [한국어] blk_mq_debugfs_register_sched() 등 debugfs 등록 API */
#include "blk-mq-debugfs.h"
/* [한국어] elevator_tags, blk_mq_init_sched(), blk_mq_exit_sched() 선언 */
#include "blk-mq-sched.h"
/* [한국어] writeback throttling(blk_wbt_*) 선언. 버퍼드 쓰기가 읽기 지연을
 * 밀어내지 않도록 제출 쪽에서 조절하는 기능으로, 스케줄러 교체 시 함께
 * 다뤄지기 때문에 포함된다. 장치 종류와 무관한 일반 기능이다. */
#include "blk-wbt.h"

/*
 * [한국어]
 * blk_mq_sched_mark_restart_hctx - hctx에 SCHED_RESTART 비트를 세워 재출발 예약
 *
 * @hctx: 대상 하드웨어 큐 컨텍스트(PCIe NVMe라면 I/O 큐 하나 = SQ/CQ 쌍에 대응)
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

	/* [한국어] BLK_MQ_S_SCHED_RESTART 비트 세팅: 이 hctx(하드웨어 큐)를 나중에 재가동 예약;
	 * __blk_mq_sched_restart()가 이 비트를 클리어하고 blk_mq_run_hw_queue()를 호출 */
	set_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_mark_restart_hctx);

/*
 * [한국어]
 * __blk_mq_sched_restart - SCHED_RESTART 비트를 클리어하고 hctx(하드웨어 큐)를 재가동
 *
 * @hctx: 재가동할 하드웨어 큐 컨텍스트
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
	/* [한국어] SCHED_RESTART 클리어: 이 hctx(하드웨어 큐)가 재출발 대기 상태에서 벗어남 */
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
 * blk_mq_dispatch_hctx_list()가 list_sort()를 호출해 같은 하드웨어 큐(hctx)로 가는
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

	/* [한국어] mq_hctx 포인터 값 비교: 같은 하드웨어 큐(hctx)끼리 인접하게 정렬됨;
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
 * list_sort() 후 호출되며, rq_list 앞쪽의 같은 hctx(하드웨어 큐)에 해당하는 request들을
 * hctx_list로 잘라낸 뒤 blk_mq_dispatch_rq_list()로 일괄 전달한다. 호출자는 rq_list가
 * 빌 때까지 이 함수를 반복 호출한다. 다른 hctx를 만나는 순간 현재 hctx 구간만 처리하고
 * 나머지는 rq_list에 남겨 다음 호출에서 처리하게 한다.
 *
 * 호출 체인:
 *   __blk_mq_do_dispatch_sched → list_sort → [blk_mq_dispatch_hctx_list]
 *   → blk_mq_dispatch_rq_list → q->mq_ops->queue_rq()(간접 호출)
 */
static bool blk_mq_dispatch_hctx_list(struct list_head *rq_list)
{
	/* [한국어] 리스트 첫 request의 mq_hctx(하드웨어 큐; NVMe라면 SQ/CQ 쌍 하나)를 현재 처리할 hctx로 선택 */
	struct blk_mq_hw_ctx *hctx =
		list_first_entry(rq_list, struct request, queuelist)->mq_hctx;
	/* [한국어] 순회용 request 포인터 — 다음 hctx 경계를 찾는 데 사용 */
	struct request *rq;
	/* [한국어] 현재 hctx에 해당하는 request들만 담을 부분 리스트 */
	LIST_HEAD(hctx_list);

	/* [한국어] rq_list 순회: 다른 hctx(하드웨어 큐)를 만나면 경계에서 분리 */
	list_for_each_entry(rq, rq_list, queuelist) {
		if (rq->mq_hctx != hctx) {
			/* [한국어] list_cut_before: rq 바로 앞까지(동일 hctx 구간)를 hctx_list로 이동;
			 * rq_list에는 나머지(다음 hctx 이후)가 남아 다음 호출에서 처리된다 */
			list_cut_before(&hctx_list, rq_list, &rq->queuelist);
			/* [한국어] 잘라낸 구간만 들고 아래 dispatch 라벨로 뛴다.
			 * break로는 "전체를 옮기는" 아래 splice까지 실행되므로
			 * goto로 그 줄을 건너뛰어야 한다. */
			goto dispatch;
		}
	}
	/* [한국어] 모든 request가 같은 hctx(하드웨어 큐)면 전체를 hctx_list로 이동 후 dispatch */
	list_splice_tail_init(rq_list, &hctx_list);

dispatch:
	/* [한국어] 동일 hctx(하드웨어 큐)로 가는 request batch를 일괄 dispatch(→ blk_mq_dispatch_rq_list → mq_ops->queue_rq 간접 호출) */
	return blk_mq_dispatch_rq_list(hctx, &hctx_list, false);
}

/* [한국어] BLK_MQ_BUDGET_DELAY: budget 확보 실패 후 하드웨어 큐 재가동 전 대기 시간(밀리초);
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
 * __blk_mq_do_dispatch_sched - elevator에서 request를 꺼내 드라이버로 dispatch
 *
 * @hctx: 처리할 하드웨어 큐 컨텍스트
 * @return: 1 = 1개 이상 dispatch 성공; 0 = 아무것도 dispatch 못함; -EAGAIN = 잔여 residual 존재
 *
 * 스케줄러가 붙어 있을 때의 디스패치 본체다. 스케줄러 큐에서 request를 하나씩
 * 꺼내 배치(batch)를 만든 뒤, 그 배치를 드라이버로 넘긴다.
 *
 * === 한 반복(iteration)의 5단계와 그 순서가 중요한 이유 ===
 *   1) e->type->ops.has_work(hctx)
 *      스케줄러에 내보낼 것이 있는지 먼저 물어본다. 없으면 아래 단계의
 *      비용(예산 획득 등)을 전혀 치르지 않고 빠져나간다.
 *   2) hctx->dispatch가 비어 있는지 확인
 *      이전에 드라이버가 거절해 되돌아온 request(잔여 목록)가 있으면 그쪽이
 *      우선이다. busy=true로 표시하고 -EAGAIN을 돌려 호출자가 잔여부터
 *      처리하게 만든다. 이것이 없으면 FLUSH 같은 요청이 굶는다(영문 주석 참고).
 *   3) blk_mq_get_dispatch_budget(q)  ← 예산 먼저
 *   4) e->type->ops.dispatch_request(hctx)  ← 그 다음에 request를 꺼낸다
 *   5) blk_mq_get_driver_tag(rq)  ← 마지막에 드라이버 태그
 *
 * 순서가 (예산 → request → 드라이버 태그)인 데는 각각 이유가 있다.
 *   - 예산을 request보다 먼저 잡는 이유: request를 스케줄러에서 빼낸 뒤에
 *     예산이 없어 못 보내면, 그 request를 스케줄러 큐에 도로 넣어야 한다.
 *     되돌리기는 스케줄러의 정렬 상태를 흐트러뜨리고 코드도 복잡해진다.
 *     예산을 먼저 확보하면 "꺼냈으면 반드시 보낸다"가 성립한다.
 *   - 드라이버 태그를 마지막에 잡는 이유: 드라이버 태그는 장치가 실제로
 *     동시에 처리할 수 있는 커맨드 수라는 희소 자원이다. 스케줄러 큐에서
 *     순서를 기다리는 동안 이것을 붙들고 있으면, 나중에 도착한 더 급한
 *     요청이 태그를 못 받는다. 그래서 정말 내보내기 직전에 잡는다.
 *     여기서 실패하면(태그 고갈) 루프를 즉시 끝낸다 — 위 영문 주석이
 *     설명하듯, 더 꺼내 봐야 못 보내고 스케줄러에게 "장치가 더 받을 수 있다"는
 *     잘못된 인상만 주기 때문이다.
 *
 * NVMe 관점:
 *   - 3단계 예산은 mq_ops->get_budget/put_budget 콜백인데, 이 트리에서 NVMe
 *     드라이버는 이 콜백을 구현하지 않는다(drivers/nvme/host/에 get_budget이
 *     없다). 구현하지 않으면 blk_mq_get_dispatch_budget()이 항상 성공하므로,
 *     NVMe에서 3단계는 사실상 통과 지점이다. SCSI가 호스트 단위 동시 커맨드
 *     수를 제한하려고 쓰는 기능이다.
 *   - 5단계에서 얻는 드라이버 태그(rq->tag)가 NVMe Command ID의 하위 12비트가
 *     된다: nvme_cid(rq) = (genctr << 12) | rq->tag (drivers/nvme/host/nvme.h),
 *     cmd->common.command_id = nvme_cid(req) (drivers/nvme/host/core.c).
 *     즉 이 줄이 "이 request가 어떤 CID로 컨트롤러에 나갈지"를 정하는 순간이다.
 *     반면 스케줄러가 이미 갖고 있던 rq->internal_tag는 CID와 무관하다.
 *   - 다만 멀티큐 NVMe의 기본값은 "none"이라 이 함수 자체가 호출되지 않는다.
 *     사용자가 sysfs로 mq-deadline 등을 붙였을 때만 이 경로가 돈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(제출 스레드 직접 실행 또는 kblockd 워커).
 * 락은 잡지 않지만, 스케줄러 콜백 내부에서 스케줄러 락(dd->lock 등)을 잡는다.
 * 이 함수가 여러 CPU에서 동시에 같은 hctx에 대해 도는 것은 blk_mq_run_hw_queue
 * 쪽의 BLK_MQ_S_SCHED_RESTART/run_work 처리로 조절된다.
 *
 * 에러 경로: 예산만 잡고 request를 못 얻으면 즉시 예산을 반납하고
 * run_queue=true로 표시해, 루프 종료 후 3ms 뒤 재가동을 예약한다. 반납하지
 * 않으면 같은 큐의 다른 hctx가 예산을 못 받아 멈출 수 있다.
 *
 * 호출 체인:
 *   blk_mq_do_dispatch_sched → [__blk_mq_do_dispatch_sched]
 *     → ops.dispatch_request → blk_mq_get_driver_tag
 *     → blk_mq_dispatch_rq_list → q->mq_ops->queue_rq()(간접 호출)
 */
static int __blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] request_queue: 이 hctx가 속한 request_queue(디스크 하나에 하나) */
	struct request_queue *q = hctx->queue;
	/* [한국어] elevator_queue: mq-deadline/BFQ/kyber 스케줄러 상태; ops.dispatch_request로 request 추출 */
	struct elevator_queue *e = q->elevator;
	/* [한국어] multi_hctxs: 뽑은 request들이 여러 하드웨어 큐로 흩어지면 true.
	 *   왜 그런 일이 생기는가 — mq-deadline과 bfq는 큐 전체를 하나의 정렬된
	 *   목록으로 관리하므로, hctx A를 처리하다가 hctx B로 갈 request를
	 *   내주기도 한다. 그래서 배치를 hctx별로 다시 묶어야 한다.
	 * run_queue: 예산은 잡았는데 dispatch_request()가 NULL을 돌려준 경우,
	 *   루프 종료 후 이 큐를 딜레이 재가동해야 함을 표시. */
	bool multi_hctxs = false, run_queue = false;
	/* [한국어] dispatched: 1개 이상 드라이버로 전달 성공 여부
	 * busy: hctx->dispatch에 잔여 request가 있어 flush 기아 위험 표시 */
	bool dispatched = false, busy = false;
	/* [한국어] max_dispatch: 이번 루프에서 드라이버로 보낼 최대 request 수
	 * dispatch_busy이면 1(conservative)로 제한, 아니면 queue depth까지 허용 */
	unsigned int max_dispatch;
	/* [한국어] rq_list: elevator에서 뽑아 driver tag(CID)까지 확보한 request 후보 목록 */
	LIST_HEAD(rq_list);
	/* [한국어] count: 이번 루프에서 실제로 rq_list에 추가된 request 수 */
	int count = 0;

	/* [한국어] dispatch_busy: 드라이버가 최근에 BLK_STS_RESOURCE/DEV_RESOURCE로
	 * 요청을 되돌려 보낸 정도를 지수이동평균으로 추적한 값(block/blk-mq.c가 갱신).
	 * 0이 아니면 "드라이버가 바쁘다"는 뜻이라 한 번에 하나씩만 시도해
	 * 되돌려받는 낭비를 줄인다.
	 * NVMe 관점: nvme_queue_rq는 태그를 이미 받은 요청을 거의 거절하지 않으므로
	 * 이 값이 0에 머무는 것이 보통이다. 주로 SCSI 같은 트랜스포트에서 의미가 있다 */
	if (hctx->dispatch_busy)
		/* [한국어] 드라이버가 바쁜 상태 — 하나만 만들어 보내 본다. 많이 만들어
		 * 봐야 대부분 거절당해 되돌아오고, 그 되돌리기 비용이 더 크다. */
		max_dispatch = 1;
	/* [한국어] 최근에 거절당한 적이 없다면 마음껏 배치를 만든다. */
	else
		/* [한국어] 큐의 nr_requests를 상한으로 쓴다. 스케줄러가 붙어 있을 때
		 * 이 값은 스케줄러 태그 수(blk_mq_init_sched에서 et->nr_requests로
		 * 설정)이므로, 큐에 담긴 것보다 많이 뽑으려 하지 않게 된다.
		 * 실제로는 이 상한에 닿기 전에 has_work가 거짓이 되거나
		 * 드라이버 태그가 떨어져 루프가 끝나는 경우가 대부분이다. */
		max_dispatch = hctx->queue->nr_requests;

	do {
		/* [한국어] 이번 반복에서 처리할 request 후보 (elevator에서 추출 예정) */
		struct request *rq;
		/* [한국어] budget_token: mq_ops->get_budget()이 돌려주는 예산 토큰.
		 * 음수면 "지금은 더 보낼 수 없다"는 뜻이다. 이 콜백을 구현하지 않는
		 * 드라이버(NVMe 포함)에서는 항상 성공하는 상수로 처리된다.
		 * SCSI가 호스트/타깃 단위 동시 커맨드 수를 지키려고 쓰는 장치다. */
		int budget_token;

		/*
		 * If we cannot get tag for the request, stop dequeueing
		 * requests from the IO scheduler. We are unlikely to be able
		 * to submit them anyway and it creates false impression for
		 * scheduling heuristics that the device can take more IO.
		 */
		/* [한국어] has_work(): elevator 큐에 드라이버로 보낼 request가 있는지 확인;
		 * 없으면 루프 종료 — 불필요한 budget 소비 방지 */
		if (e->type->ops.has_work && !e->type->ops.has_work(hctx))
			break;

		/* [한국어] hctx->dispatch는 "드라이버가 거절해 되돌아온" request들의
		 * 잔여 목록이다. 그것이 비어 있지 않으면 스케줄러에서 새로 꺼내는
		 * 것보다 먼저 처리해야 한다 — 잔여를 계속 뒤로 미루면 그 안의
		 * FLUSH 같은 요청이 영원히 굶기 때문이다(영문 주석의 "starving flushes").
		 * _careful 변형은 락 없이 읽어도 되도록 리스트 포인터를 안전하게
		 * 검사하는 헬퍼다. busy=true → 아래에서 -EAGAIN을 반환한다. */
		if (!list_empty_careful(&hctx->dispatch)) {
			busy = true;
			break;
		}

		/* [한국어] ★ 순서 1: 예산 먼저 ★
		 * mq_ops->get_budget()을 부른다. request를 스케줄러에서 빼내기 "전에"
		 * 잡아야, 나중에 못 보내서 스케줄러 큐로 되돌려 넣는 상황을 피할 수 있다.
		 * 이 트리의 NVMe 드라이버는 get_budget을 구현하지 않으므로 항상 성공한다.
		 * 위 영문 주석이 밝히듯, SCSI는 실패 시 자기 완료 핸들러에서 큐를
		 * 다시 돌리므로 여기서 재가동을 예약해 줄 필요가 없다. */
		budget_token = blk_mq_get_dispatch_budget(q);
		/* [한국어] 음수 = 예산 없음. 아직 request를 꺼내지 않았으므로 되돌릴
		 * 것 없이 그냥 루프를 끝낸다. */
		if (budget_token < 0)
			break;

		/* [한국어] ★ 순서 2: 스케줄러가 다음 request를 고른다 ★
		 * 스케줄러별 정책이 여기서 발현된다:
		 *   mq-deadline - dd_dispatch_request(). 만료된 요청이 있으면 그것을,
		 *                 없으면 LBA 순서대로 고른다.
		 *   kyber       - kyber_dispatch_request(). 도메인별 토큰이 남아 있는
		 *                 쪽에서 고른다.
		 *   bfq         - bfq_dispatch_request(). 가중치 기반으로 서비스할
		 *                 bfq_queue를 고른다.
		 * 이 호출 안에서 스케줄러 락을 잡았다 푼다. */
		rq = e->type->ops.dispatch_request(hctx);
		/* [한국어] NULL = 내줄 것이 없다. has_work가 참이었는데도 NULL이 나올
		 * 수 있다 — 그 사이에 다른 CPU가 가져갔거나, 스케줄러가 지금은
		 * 내보내지 않기로 결정했기 때문이다(kyber의 토큰 소진 등). */
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
			/* [한국어] run_queue=true: 루프 종료 후 이 하드웨어 큐를 3ms 딜레이로 재가동 예약 */
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

		/* [한국어] 배치 목록 끝에 붙인다. tail에 넣어야 스케줄러가 정한 순서가
		 * 그대로 보존된다 — 스케줄러가 애써 만든 순서를 여기서 뒤집으면 안 된다. */
		list_add_tail(&rq->queuelist, &rq_list);
		/* [한국어] 배치 크기 증가. 루프 조건(count < max_dispatch)의 기준이다. */
		count++;
		/* [한국어] rq->mq_hctx != hctx: 이 request가 다른 하드웨어 큐로 가야 함 → list_sort 필요 */
		if (rq->mq_hctx != hctx)
			multi_hctxs = true;

		/*
		 * If we cannot get tag for the request, stop dequeueing
		 * requests from the IO scheduler. We are unlikely to be able
		 * to submit them anyway and it creates false impression for
		 * scheduling heuristics that the device can take more IO.
		 */
		/* [한국어] ★ 순서 3: 드라이버 태그를 마지막에 ★
		 * rq->tag에 드라이버 태그를 배정한다(rq->internal_tag는 이미 스케줄러
		 * 태그로 채워져 있다 — 둘은 별개의 번호다).
		 * NVMe 관점: 여기서 정해진 rq->tag가 곧 이 커맨드의 Command ID
		 * 하위 12비트가 된다. nvme_cid(rq) = (genctr << 12) | rq->tag이고
		 * 완료 시 nvme_find_rq()가 CQE의 command_id에서 이 태그를 되뽑아
		 * request를 찾는다(drivers/nvme/host/nvme.h).
		 * 실패 = 드라이버 태그 고갈(장치가 받을 수 있는 만큼 이미 나가 있음).
		 * 이때 더 꺼내 봐야 보낼 수 없고, 스케줄러에게 "장치가 여유롭다"는
		 * 잘못된 신호만 주므로 루프를 끝낸다(영문 주석 참고).
		 * 참고: 태그를 못 받은 이 rq도 이미 rq_list에 들어가 있다. 배치를
		 * 넘겨받은 blk_mq_dispatch_rq_list()가 태그 없는 request를 만나면
		 * 거기서 다시 태그를 시도하고, 안 되면 hctx->dispatch로 되돌린다. */
		if (!blk_mq_get_driver_tag(rq))
			break;
	/* [한국어] 배치가 상한에 찰 때까지 반복. 위의 break 조건들(내줄 것 없음,
	 * 잔여 존재, 예산 없음, 태그 고갈) 중 하나로 먼저 끝나는 것이 보통이다. */
	} while (count < max_dispatch);

	/* [한국어] 한 건도 만들지 못한 경우의 처리. */
	if (!count) {
		/* [한국어] 아무것도 dispatch하지 못한 경우 */
		if (run_queue)
			/* [한국어] 3ms 후 모든 hctx를 재가동 — budget 반납 후 다른 hctx가 먼저 처리할 기회 */
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY);
	/* [한국어] 배치가 여러 하드웨어 큐에 걸쳐 있는 경우 — 정렬 후 구간별 전달. */
	} else if (multi_hctxs) {
		/*
		 * Requests from different hctx may be dequeued from some
		 * schedulers, such as bfq and deadline.
		 *
		 * Sort the requests in the list according to their hctx,
		 * dispatch batching requests from same hctx at a time.
		 */
		/* [한국어] 여러 하드웨어 큐로 흩어진 request들을 hctx 포인터 값 기준으로
		 * 정렬해, 같은 하드웨어 큐로 갈 것들을 인접하게 만든다. 그래야 아래
		 * 루프가 hctx당 blk_mq_dispatch_rq_list()를 한 번씩만 부를 수 있다.
		 * 정렬 없이 섞인 채로 보내면 hctx가 바뀔 때마다 호출을 새로 해야 해서
		 * 배치의 이점이 사라진다.
		 * list_sort는 병합 정렬 기반이라 안정(stable)하므로, 같은 hctx 안에서는
		 * 스케줄러가 정한 순서가 그대로 유지된다 — 이것이 중요하다. */
		list_sort(NULL, &rq_list, sched_rq_cmp);
		do {
			/* [한국어] 한 hctx(하드웨어 큐) 구간씩 잘라 batch dispatch; rq_list가 빌 때까지 반복 */
			dispatched |= blk_mq_dispatch_hctx_list(&rq_list);
		} while (!list_empty(&rq_list));
	} else {
		/* [한국어] 모든 request가 같은 hctx(하드웨어 큐)면 바로 일괄 dispatch */
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
 * @hctx: 처리할 하드웨어 큐 컨텍스트
 * @return: __blk_mq_do_dispatch_sched()의 마지막 반환값
 *          (0=아무것도 못 함; 1=성공; -EAGAIN=잔여)
 *
 * __blk_mq_do_dispatch_sched()를 반복 호출해 elevator에서 가능한 한 많은 request를
 * 드라이버로 dispatch한다. 한 번 성공(ret==1)하면 계속 시도하고, 선점 요청이 오거나
 * 1초(HZ jiffies)가 지나면 blk_mq_delay_run_hw_queue(hctx, 0)로 즉시 재가동 예약
 * 후 루프를 탈출해 CPU를 양보한다. 큐가 매우 바쁠 때 이 스레드가 CPU를
 * 독점하는 것을 막으면서도 처리량을 최대한 뽑아내는 절충안이다.
 * (배치 하나를 만들 때마다 워커를 다시 깨우면 지연이 커지므로 되도록 이어서
 *  돌리되, 무한정 돌지는 않게 상한을 둔 것이다.)
 *
 * 호출 체인:
 *   __blk_mq_sched_dispatch_requests → [blk_mq_do_dispatch_sched]
 *   → __blk_mq_do_dispatch_sched → blk_mq_dispatch_rq_list → mq_ops->queue_rq()
 */
static int blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] end: 루프 종료 시한 — jiffies + HZ = 현재 시각 + 1초;
	 * 1초를 넘으면 CPU를 양보하고 재가동 예약 */
	unsigned long end = jiffies + HZ;
	/* [한국어] 안쪽 함수의 반환값. 마지막 값이 그대로 호출자에게 전달되므로
	 * -EAGAIN(잔여 존재)이 밖으로 새어 나가 재시도를 유발할 수 있다. */
	int ret;

	do {
		/* [한국어] 배치 한 묶음을 만들어 드라이버로 넘긴다. */
		ret = __blk_mq_do_dispatch_sched(hctx);
		/* [한국어] 1이 아니면(0 = 아무것도 못 보냄, -EAGAIN = 잔여 우선 처리
		 * 필요) 더 반복할 이유가 없다. 1일 때만 "아직 일이 남았을 수 있다"고
		 * 보고 계속 돈다. */
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
 * @hctx: 하드웨어 큐 컨텍스트 — 여기에 매핑된 sw queue(ctx)들의 컨테이너
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
 * @hctx: 처리할 하드웨어 큐 컨텍스트
 * @return: 0 = 정상 종료; -EAGAIN = hctx->dispatch에 잔여 request 존재 (flush 기아 방지)
 *
 * IO 스케줄러가 없는 상황("none", NVMe에서 흔히 사용)에서 각 CPU의 sw queue(ctx)에서
 * request를 하나씩 꺼내 드라이버로 dispatch한다. per-CPU ctx를 라운드 로빈으로 순회해
 * 공정성을 보장한다. 각 반복에서 (1) residual 확인, (2) ctx_map 비트맵 확인,
 * (3) budget 확보, (4) dequeue_from_ctx()로 request 추출, (5) dispatch_rq_list()로
 * SQ 전달까지 진행한다. dispatch_from을 갱신해 다음 호출에서 같은 지점에서 재개한다.
 *
 * 호출 체인:
 *   __blk_mq_sched_dispatch_requests (elevator 없음 경로)
 *   → [blk_mq_do_dispatch_ctx] → blk_mq_dispatch_rq_list → mq_ops->queue_rq()
 */
static int blk_mq_do_dispatch_ctx(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] request_queue(디스크 하나에 하나) — budget/tag 확보에 사용 */
	struct request_queue *q = hctx->queue;
	/* [한국어] rq_list: 현재 ctx에서 뽑아 드라이버로 보낼 request 1개 임시 보관 */
	LIST_HEAD(rq_list);
	/* [한국어] dispatch_from: 라운드 로빈 시작점 — 이전 dispatch가 멈춘 ctx에서 재개;
	 * READ_ONCE: 다른 CPU의 WRITE_ONCE와 경쟁 — 원자적 읽기 필요 */
	struct blk_mq_ctx *ctx = READ_ONCE(hctx->dispatch_from);
	/* [한국어] 0: 정상 종료; -EAGAIN으로 설정되면 flush 기아 방지 재호출 신호 */
	int ret = 0;
	/* [한국어] sw queue에서 꺼낸 request. 루프 밖에서 선언한 이유가 있다 —
	 * do/while 조건식에서 rq->mq_hctx를 참조하기 때문이다. */
	struct request *rq;

	do {
		/* [한국어] budget_token: mq_ops->get_budget()의 예산 토큰. NVMe는 이
		 * 콜백을 구현하지 않아 항상 성공한다. SCSI 전용 장치라고 보면 된다. */
		int budget_token;

		/* [한국어] 드라이버가 거절해 되돌아온 잔여 request가 있으면 그쪽이
		 * 우선이다. 여기서는 __blk_mq_do_dispatch_sched와 달리 곧바로
		 * ret = -EAGAIN을 설정해 호출자에게 재시도를 요구한다. */
		if (!list_empty_careful(&hctx->dispatch)) {
			ret = -EAGAIN;
			break;
		}

		/* [한국어] ctx_map: 처리할 request가 있는 sw queue를 나타내는 비트맵;
		 * 모든 비트가 0이면 모든 CPU sw queue가 비어 있음 → 루프 종료 */
		if (!sbitmap_any_bit_set(&hctx->ctx_map))
			break;

		/* [한국어] 여기서도 request를 꺼내기 전에 예산을 먼저 잡는다.
		 * 이유는 __blk_mq_do_dispatch_sched와 같다 — 꺼낸 뒤에 못 보내면
		 * 되돌려 넣어야 하기 때문이다. */
		budget_token = blk_mq_get_dispatch_budget(q);
		/* [한국어] 음수 = 예산 없음. 아직 아무것도 꺼내지 않았으므로 그냥 종료. */
		if (budget_token < 0)
			break;

		/* [한국어] blk_mq_dequeue_from_ctx(): 현재 ctx(CPU sw queue)에서 request 1개 추출;
		 * ctx_map 비트는 설정되어 있었으나 다른 CPU가 이미 뽑아갔을 수 있음 → NULL 체크 필요 */
		rq = blk_mq_dequeue_from_ctx(hctx, ctx);
		/* [한국어] NULL = 이 sw queue가 비어 있다. ctx_map 비트를 보고 왔지만
		 * 그 사이 다른 CPU가 먼저 가져갔을 수 있어 반드시 확인해야 한다. */
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

		/* [한국어] rq_list에 추가: dispatch_rq_list()가 드라이버로 전달할 단일 request */
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
 * @hctx: 처리할 하드웨어 큐 컨텍스트
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
 *   → blk_mq_dispatch_rq_list → q->mq_ops->queue_rq()(간접 호출)
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
		/* [한국어] 잔여 목록을 통째로 가져왔으므로 락 해제. 이후 조작은
		 * 지역 리스트(rq_list) 위에서만 일어나 락이 필요 없다. */
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
	/* [한국어] 잔여 목록에서 뭔가 가져왔다면 그것부터 처리한다.
	 * 위 영문 주석의 논리가 핵심이다: 장치 큐 깊이가 얕을 때 스케줄러에서
	 * 미리 다 꺼내 버리면, 꺼낸 순간부터 그 request들은 더 이상 병합·정렬
	 * 대상이 아니게 된다("we can no longer merge or sort them").
	 * 그래서 되도록 스케줄러 안에 오래 두고, 잔여가 있을 때는 스케줄러를
	 * 건드리지 않는다. */
	if (!list_empty(&rq_list)) {
		/* [한국어] "이번에 스케줄러에서 못 꺼냈으니 나중에 다시 돌려 달라"는
		 * 예약을 남긴다. 이 비트가 없으면, 잔여 처리 후 아무도 큐를 다시
		 * 깨우지 않아 스케줄러에 쌓인 request가 그대로 멈춰 버린다. */
		blk_mq_sched_mark_restart_hctx(hctx);
		/* [한국어] 잔여 배치를 드라이버로 넘긴다. 세 번째 인자 true는
		 * "이 목록은 잔여(dispatch list)에서 온 것"이라는 표시로,
		 * 실패 시 되돌려 넣는 처리와 dispatch_busy 갱신 방식이 달라진다. */
		if (!blk_mq_dispatch_rq_list(hctx, &rq_list, true))
			/* [한국어] 전부 넘기지 못했다 = 드라이버가 여전히 바쁘다.
			 * 스케줄러에서 더 꺼내 봐야 소용없으므로 여기서 끝낸다.
			 * 남은 것은 blk_mq_dispatch_rq_list가 다시 hctx->dispatch로
			 * 되돌려 놓았고, 위에서 세운 SCHED_RESTART가 재시도를 부른다. */
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
	/* [한국어] sw queue 전체 flush batch를 드라이버로 한 번에 전달 (from_sched=true) */
	blk_mq_dispatch_rq_list(hctx, &rq_list, true);
	return 0;
}

/*
 * [한국어]
 * blk_mq_sched_dispatch_requests - hctx dispatch 최상위 진입점
 *
 * @hctx: 처리할 하드웨어 큐 컨텍스트
 *
 * blk_mq_run_hw_queue()에서 호출되는 dispatch 최상위 함수. hctx가 stopped 상태이거나
 * 큐가 quiesced 상태이면 즉시 리턴해, 드라이버로 아무것도 내보내지 않는다.
 * (quiesce는 스케줄러 교체, 컨트롤러 리셋, 디스크 제거처럼 "지금 요청이 나가면
 *  안 되는" 상황에서 걸린다. NVMe라면 그 구간 동안 nvme_queue_rq가 불리지
 *  않으므로 새 커맨드가 SQ에 실리지 않는다.)
 * __blk_mq_sched_dispatch_requests()를 호출하고, -EAGAIN이 반환되면(hctx->dispatch에
 * 잔여 request 존재) 한 번 더 시도한다. 두 번째도 -EAGAIN이면 async work로 예약해
 * flush 기아를 방지한다. 총 최대 2번 시도 후 work 예약으로 보장한다.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue (work queue/softirq) → [blk_mq_sched_dispatch_requests]
 *   → __blk_mq_sched_dispatch_requests → ... → q->mq_ops->queue_rq()(간접 호출)
 */
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] request_queue(디스크 하나에 하나): quiesced 상태 확인에 사용 */
	struct request_queue *q = hctx->queue;

	/* RCU or SRCU read lock is needed before checking quiesced flag */
	/* [한국어] 두 가지 정지 상태를 함께 확인한다.
	 *   blk_mq_hctx_stopped() — 이 하드웨어 큐 하나에만 걸린 BLK_MQ_S_STOPPED.
	 *     드라이버가 blk_mq_stop_hw_queue()로 "이 큐는 지금 받을 수 없다"고
	 *     선언한 상태다(자원 부족 등).
	 *   blk_queue_quiesced() — 큐 전체에 걸린 정지. 스케줄러 교체
	 *     (elevator_switch의 blk_mq_quiesce_queue), 컨트롤러 리셋, 디스크
	 *     제거 등에서 걸린다.
	 * unlikely()로 감싼 이유: 정상 동작 중에는 거의 항상 거짓이라, 분기
	 * 예측이 통과 쪽으로 최적화되어야 핫패스가 빠르다.
	 * 위 영문 주석대로 이 검사는 RCU/SRCU read 락 안에서 이루어져야 한다 —
	 * 호출자 blk_mq_run_hw_queue가 그 락을 쥐고 부른다. */
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
 * @q:      request_queue(디스크 하나에 하나)
 * @bio:    merge 시도할 신규 bio
 * @nr_segs: 이 bio의 물리 세그먼트 수. 병합 판정 시 결과 request가
 *           큐 한계(max_segments 등)를 넘지 않는지 검사하는 데 쓰인다.
 * @return: merge 성공이면 true, 실패이면 false
 *
 * blk_mq_submit_bio()에서 bio를 request로 변환하기 전에 호출된다. elevator가 있으면
 * e->type->ops.bio_merge()로 scheduler의 merge 정책을 사용하고, 없으면("none") per-CPU
 * sw queue(ctx->rq_lists[type])에서 역방향으로 최대 8개의 request를 검사해 merge한다.
 * merge에 성공하면 request 하나가 줄고, NVMe에서는 그만큼 발행되는 커맨드가
 * 하나 줄어든다(SQ 엔트리·Command ID·완료 CQ 엔트리 각각 하나씩 절약).
 * 주의: 줄어드는 것은 커맨드 "개수"이며, 남은 커맨드가 더 많은 데이터를
 * 담게 되므로 그 커맨드의 PRP/SGL 엔트리 수는 오히려 늘어난다.
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
	/* [한국어] bio가 매핑될 하드웨어 큐 컨텍스트 */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] merge 성공 여부 초기화 */
	bool ret = false;
	/* [한국어] hctx 타입: default/read/poll — ctx->rq_lists[type] 인덱스로 사용 */
	enum hctx_type type;

	/* [한국어] elevator가 있고 bio_merge ops가 있으면 스케줄러에게 merge 위임 */
	if (e && e->type->ops.bio_merge) {
		/* [한국어] elevator 내부 자료구조(해시/정렬 rb-tree)를 이용한 merge 시도.
		 * 스케줄러가 큐 전체를 보므로 아래 sw queue 경로보다 병합 범위가 넓다 */
		ret = e->type->ops.bio_merge(q, bio, nr_segs);
		/* [한국어] 스케줄러가 판정을 끝냈으므로 아래 sw queue 경로는 건너뛴다.
		 * 두 경로를 모두 시도하지 않는 이유: 스케줄러가 붙어 있으면 request는
		 * 스케줄러 큐에 있지 sw queue에 있지 않아, 뒤져 봐야 항상 비어 있다. */
		goto out_put;
	}

	/* [한국어] blk_mq_get_ctx(): 현재 CPU의 sw queue 포인터 획득 (preempt-safe) */
	ctx = blk_mq_get_ctx(q);
	/* [한국어] blk_mq_map_queue(): bio->bi_opf 플래그 기반으로 적합한 hctx(하드웨어 큐) 선택 */
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
	 * merge에 성공하면 발행될 request가 하나 줄어든다
	 * (NVMe라면 커맨드 하나, Command ID 하나, 완료 하나가 절약된다) */
	if (blk_bio_list_merge(q, &ctx->rq_lists[type], bio, nr_segs))
		/* [한국어] 병합 성공 — 호출자 blk_mq_submit_bio는 새 request를
		 * 만들지 않고 그대로 돌아간다. */
		ret = true;

	/* [한국어] sw queue 조작 끝 — 락 해제. */
	spin_unlock(&ctx->lock);
out_put:
	/* [한국어] 공통 반환 지점. 라벨 이름이 out_put인 것은 예전에 여기서
	 * blk_mq_put_ctx()로 preempt를 풀던 흔적이다(현재 구현에는 그 호출이 없다).
	 * true면 bio가 기존 request에 흡수되었고, false면 호출자가 새 request를
	 * 할당해 이 bio를 담는다. */
	return ret;
}

/*
 * [한국어]
 * blk_mq_sched_try_insert_merge - scheduler에 request를 삽입할 때 merge 시도
 *
 * @q:    request_queue(디스크 하나에 하나)
 * @rq:   scheduler에 삽입하려는 request
 * @free: merge로 인해 해제할 request들을 담는 리스트
 * @return: merge 성공이면 true (rq는 기존 request에 흡수됨); 실패이면 false
 *
 * blk-mq가 request를 elevator에 삽입하기 직전, 기존 request와 merge 가능한지 확인한다.
 * rq_mergeable()로 기본 조건을 검사한 후 elv_attempt_insert_merge()로 elevator의
 * merge hash를 사용해 back-merge를 시도한다. 성공하면 드라이버로 내려갈
 * request가 하나 줄어든다(NVMe라면 커맨드 하나가 절약된다).
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
 * @q:     request_queue(디스크 하나에 하나)
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
	/* [한국어] 순회 커서로 쓸 하드웨어 큐 포인터. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] queue_for_each_hw_ctx가 쓰는 인덱스(xarray 순회용). */
	unsigned long i;

	/* [한국어] 모든 hctx에서 스케줄러 태그 포인터를 떼어 낸다. 메모리를 여기서
	 * 해제하지 않는 것이 중요하다 — 실제 해제는 blk_mq_free_sched_tags()가
	 * 별도로 하며, 그 시점은 큐를 녹인 뒤다. 여기서는 "더 이상 이 태그를
	 * 쓰지 않는다"는 연결 끊기만 한다. */
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
 * @q: request_queue(디스크 하나에 하나)
 *
 * /sys/kernel/debug/block/<disk>/sched/ 하위에 scheduler 진단 정보를 등록한다.
 * queue 전체용 debugfs와 hctx별 debugfs를 함께 등록해 하드웨어 큐별 latency·
 * dispatch 통계를 확인할 수 있게 한다. blk_debugfs_lock()으로 직렬화.
 */
void blk_mq_sched_reg_debugfs(struct request_queue *q)
{
	/* [한국어] 각 하드웨어 큐(hctx) debugfs 등록에 사용할 순회 커서 */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] memflags: blk_debugfs_lock()이 NOIO 컨텍스트로 전환하며 저장해 주는
	 * 이전 memalloc 상태. unlock 시 그대로 돌려줘야 원복된다.
	 * irq 플래그가 아니다 — debugfs 생성이 fs reclaim을 유발하고 그 reclaim이
	 * 다시 이 (얼어 있을 수도 있는) 큐로 I/O를 내려보내는 재귀를 막기 위한 것이다. */
	unsigned int memflags;
	/* [한국어] queue_for_each_hw_ctx가 쓰는 인덱스. */
	unsigned long i;

	/* [한국어] debugfs_mutex 획득 + NOIO 전환. 등록/해제가 동시에 일어나는 것을
	 * 막고, 위에서 설명한 reclaim 재귀 데드락도 함께 막는다. */
	memflags = blk_debugfs_lock(q);
	/* [한국어] queue 수준 scheduler debugfs 등록: dispatch 통계, elevator 파라미터 등 */
	blk_mq_debugfs_register_sched(q);
	/* [한국어] 하드웨어 큐마다 별도의 debugfs 항목을 만든다. 큐별로 나뉘어야
	 * 어느 하드웨어 큐에서 요청이 밀리는지 구분해 볼 수 있다.
	 * (스케줄러가 큐 전역 락으로 직렬화된다는 사실도 여기서 관찰 가능하다.) */
	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_debugfs_register_sched_hctx(q, hctx);
	/* [한국어] 락 해제 + memflags로 memalloc 컨텍스트 원복. */
	blk_debugfs_unlock(q, memflags);
}

/*
 * [한국어]
 * blk_mq_sched_unreg_debugfs - scheduler debugfs 항목 제거
 *
 * @q: request_queue(디스크 하나에 하나)
 *
 * elevator 종료 또는 queue 해제 시 hctx별 debugfs와 queue debugfs를 역순으로 제거한다.
 */
void blk_mq_sched_unreg_debugfs(struct request_queue *q)
{
	/* [한국어] 하드웨어 큐 순회 커서. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] queue_for_each_hw_ctx가 쓰는 인덱스. */
	unsigned long i;

	/* [한국어] _nomemsave 변형은 debugfs_mutex만 잡고 NOIO 전환은 하지 않는다.
	 * 제거 경로는 새 파일을 만들지 않아 fs reclaim을 유발할 여지가 없으므로,
	 * memalloc 상태를 건드릴 필요가 없기 때문이다(등록 경로와 대칭이 아니다). */
	blk_debugfs_lock_nomemsave(q);
	/* [한국어] hctx(하드웨어 큐)별 scheduler debugfs 제거 */
	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_debugfs_unregister_sched_hctx(hctx);
	/* [한국어] queue 수준 scheduler debugfs 제거 */
	blk_mq_debugfs_unregister_sched(q);
	/* [한국어] 락만 해제한다(복원할 memflags가 없다). */
	blk_debugfs_unlock_nomemrestore(q);
}

/*
 * [한국어]
 * blk_mq_free_sched_tags - scheduler shadow tag pool(elevator_tags) 해제
 *
 * @et:  해제할 elevator_tags 구조체 (nr_hw_queues, tags[] 포함)
 * @set: NVMe 장치 blk_mq_tag_set (shared tags 여부 판단, request 해제에 사용)
 *
 * blk_mq_alloc_sched_tags()로 할당된 스케줄러 태그 풀을 해제한다.
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
	/* [한국어] 비공유 모드에서 tags[] 배열을 순회할 인덱스. */
	unsigned long i;

	/* Shared tags are stored at index 0 in @tags. */
	/* [한국어] shared tags: 모든 하드웨어 큐가 태그 풀 하나를 공유 → et->tags[0] 하나만 해제 */
	if (blk_mq_is_shared_tags(set->flags))
		/* [한국어] BLK_MQ_NO_HCTX_IDX: 특정 hctx에 귀속되지 않는 공유 pool을 나타내는 sentinel */
		blk_mq_free_map_and_rqs(set, et->tags[0], BLK_MQ_NO_HCTX_IDX);
	else {
		/* [한국어] 비공유 모드: 하드웨어 큐마다 독립된 태그 맵이 있다 → nr_hw_queues개 순서대로 해제 */
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
 * @set:  이 큐가 속한 blk_mq_tag_set (태그 맵 해제에 필요)
 *
 * elevator_resources는 elevator 전환 시 새 elevator를 위해 미리 할당한 자원 묶음이다.
 * et(elevator_tags)와 data(스케줄러 private data)를 각각 해제하고 포인터를 NULL로 초기화한다.
 */
void blk_mq_free_sched_res(struct elevator_resources *res,
		struct elevator_type *type,
		struct blk_mq_tag_set *set)
{
	/* [한국어] et가 있으면 스케줄러 태그 풀 해제 */
	if (res->et) {
		blk_mq_free_sched_tags(res->et, set);
		/* [한국어] 해제 후 NULL로 초기화 — double free 방지 */
		res->et = NULL;
	}
	/* [한국어] data가 있으면 scheduler private data(mq-deadline의 deadline_data,
	 * BFQ의 bfq_data, kyber의 kyber_queue_data 등) 해제 */
	if (res->data) {
		/* [한국어] 스케줄러 타입별 해제 루틴에 위임한다. 어떤 구조체인지는
		 * 스케줄러만 알기 때문에 type을 함께 넘겨야 한다. */
		blk_mq_free_sched_data(type, res->data);
		/* [한국어] 마찬가지로 NULL로 지워 double free를 막는다. 이 함수는
		 * 전환 실패 경로와 정상 종료 경로 양쪽에서 불릴 수 있어 멱등성이 중요하다. */
		res->data = NULL;
	}
}

/*
 * [한국어]
 * blk_mq_free_sched_res_batch - tagset에 속한 모든 queue의 scheduler 자원 일괄 해제
 *
 * @elv_tbl: queue id → elv_change_ctx 매핑 xarray (elevator 전환 컨텍스트 테이블)
 * @set:     대상 blk_mq_tag_set (NVMe라면 컨트롤러 하나가 소유하고
 *           그 컨트롤러의 모든 네임스페이스 큐가 공유한다)
 *
 * update_nr_hwq_lock write lock을 보유한 상태에서 호출된다. 하드웨어 큐 수 변경
 * (update_nr_hw_queues) 롤백 경로나 elevator 일괄 교체 종료 시 사용한다.
 * scheduler가 붙은 queue만 처리하며 elv_tbl에서 ctx를 찾아 free한다.
 */
void blk_mq_free_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	/* [한국어] tagset에 속한 request_queue 순회용 포인터 */
	struct request_queue *q;
	/* [한국어] elv_tbl에서 로드할 elevator 전환 컨텍스트 */
	struct elv_change_ctx *ctx;

	/* [한국어] lockdep: update_nr_hwq_lock write lock 보유 확인 — 미보유 시 커널 경고 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	/* [한국어] 이 태그 세트를 공유하는 모든 request_queue를 순회한다.
	 * NVMe라면 한 컨트롤러의 모든 네임스페이스(nvme0n1, nvme0n2, ...)가
	 * tag_set 하나를 공유하므로 그 큐들이 전부 여기에 걸린다. */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		/* [한국어] 스케줄러가 붙은 큐만 해제 대상이다. "none"인 큐(멀티큐
		 * NVMe의 기본 상태)는 애초에 자원을 잡지 않았으므로 건너뛴다.
		 * 위 영문 주석대로, update_nr_hwq_lock을 쓰기 모드로 쥐고 있으면
		 * elevator_lock 없이 q->elevator를 읽어도 안전하다 — 스케줄러를
		 * 바꾸는 쪽은 같은 락을 읽기 모드로 잡아야 해서 동시에 못 돈다. */
		if (q->elevator) {
			/* [한국어] xa_load(): queue->id를 키로 변경 컨텍스트 찾기 */
			ctx = xa_load(elv_tbl, q->id);
			/* [한국어] 스케줄러가 붙은 큐라면 blk_mq_alloc_sched_ctx_batch()가
			 * 반드시 ctx를 만들어 두었어야 한다. 없다는 것은 두 함수가 보는
			 * 큐 목록이 어긋났다는 뜻이라 프로그래밍 오류다. */
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
 * @set:     대상 blk_mq_tag_set (NVMe라면 컨트롤러 하나가 소유하고
 *           그 컨트롤러의 모든 네임스페이스 큐가 공유한다)
 * @return:  0 = 성공; -ENOMEM = 메모리 부족 (일부 할당 성공 상태로 반환 — 롤백은 호출자)
 *
 * elevator 전환(elevator_change) 시 미리 모든 queue의 컨텍스트를 할당한다. 이후
 * blk_mq_alloc_sched_res_batch()가 각 ctx에 elevator_tags와 private data를 채운다.
 * update_nr_hwq_lock write lock 하에서 호출되어야 한다.
 */
int blk_mq_alloc_sched_ctx_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set)
{
	/* [한국어] tagset 소속 request_queue 순회용 */
	struct request_queue *q;
	/* [한국어] 새로 할당할 elevator 전환 컨텍스트 */
	struct elv_change_ctx *ctx;

	/* [한국어] update_nr_hwq_lock write lock 보유 확인 — 미보유 시 lockdep 경고 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	/* [한국어] tagset에 속한 모든 request_queue를 순회해 ctx 할당 */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* [한국어] kzalloc_obj: elv_change_ctx 구조체를 zero-init으로 할당 */
		ctx = kzalloc_obj(struct elv_change_ctx);
		/* [한국어] 할당 실패 — 여기서 롤백하지 않는다. */
		if (!ctx)
			/* [한국어] 이미 삽입된 ctx들은 호출자가
			 * blk_mq_free_sched_ctx_batch()로 한꺼번에 정리한다. */
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
 * blk_mq_alloc_sched_tags - 스케줄러 태그 풀(elevator_tags) 할당
 *
 * @set:          대상 blk_mq_tag_set. 태그 공유 여부(flags)와 queue_depth를 여기서 읽는다
 * @nr_hw_queues: 하드웨어 큐(hctx) 수
 * @nr_requests:  스케줄러가 대기시켜 둘 수 있는 최대 request 수.
 *               호출자 blk_mq_alloc_sched_res()가 blk_mq_default_nr_requests(set)
 *               = 2 * min(set->queue_depth, BLKDEV_DEFAULT_RQ=128)로 계산해 넘긴다.
 *               NVMe처럼 queue_depth가 큰 장치에서는 256으로 고정되어,
 *               오히려 드라이버 태그 수보다 적다.
 * @return:       할당된 elevator_tags 포인터; 실패 시 NULL
 *
 * scheduler가 대기 request를 담아 둘 태그 맵과 request 풀을 할당한다.
 * 이 태그는 드라이버 태그와 완전히 별개이며, NVMe Command ID가 아니다
 * (CID로 쓰이는 것은 dispatch 시점에 따로 잡는 드라이버 태그다).
 * shared tags 모드면 모든 hctx가 tags[0] 하나를 공유하고(MAX_SCHED_RQ 한도),
 * 비공유 모드면 하드웨어 큐별로 독립된 tag map을 nr_requests 크기로 할당한다.
 * 할당 실패 시 out_unwind 경로에서 이미 할당된 tag map을 역순으로 해제한다.
 *
 * 구조체 field 역할:
 *   et->nr_requests:  이 태그 풀이 담을 수 있는 최대 request 수.
 *                     blk_mq_init_sched()가 이 값을 q->nr_requests에 반영한다
 *   et->nr_hw_queues: hctx(하드웨어 큐) 수; 비공유 모드에서 해제 루프 상한
 *   et->tags[i]:      i번째 hctx용 blk_mq_tags (tag bitmap + request pool)
 *
 * 호출 체인:
 *   blk_mq_alloc_sched_res → [blk_mq_alloc_sched_tags]
 *   → blk_mq_alloc_map_and_rqs (per-hctx tag map 할당)
 */
struct elevator_tags *blk_mq_alloc_sched_tags(struct blk_mq_tag_set *set,
		unsigned int nr_hw_queues, unsigned int nr_requests)
{
	/* [한국어] nr_tags: 할당할 tag map 개수 — 태그 공유 모드면 1, 아니면 nr_hw_queues */
	unsigned int nr_tags;
	/* [한국어] tags[] 순회 인덱스. 실패 시 out_unwind에서 이 값으로 롤백
	 * 범위를 정하므로, 루프 밖에서 선언해야 한다(C89 스타일 유지). */
	int i;
	/* [한국어] et: 반환할 elevator_tags (nr_hw_queues, nr_requests, tags[] 보관) */
	struct elevator_tags *et;
	/* [한국어] GFP_NOIO: I/O 경로 메모리 할당 시 재귀 I/O 방지;
	 * __GFP_ZERO: 제로 초기화; __GFP_NOWARN|__GFP_NORETRY: 실패해도 경고 없이 NULL 반환 */
	gfp_t gfp = GFP_NOIO | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY;

	/* [한국어] shared tags 모드: 모든 하드웨어 큐가 태그 풀 하나를 공유 → tag map 1개만 필요 */
	if (blk_mq_is_shared_tags(set->flags))
		/* [한국어] 공유 모드에서는 태그 맵 하나를 모든 하드웨어 큐가 함께 쓴다.
		 * tags[0]에만 담고 나머지 인덱스는 쓰지 않는다. */
		nr_tags = 1;
	/* [한국어] NVMe를 포함한 대부분의 장치가 이쪽 — 하드웨어 큐마다 독립 태그 맵. */
	else
		/* [한국어] 비공유 모드: 하드웨어 큐마다 독립 tag map이 필요하다.
		 * NVMe가 이쪽이며, 하드웨어 큐 수만큼 배열이 만들어진다 */
		nr_tags = nr_hw_queues;

	/* [한국어] kmalloc_flex: elevator_tags + tags[nr_tags] flexible array 한 번에 할당 */
	et = kmalloc_flex(*et, tags, nr_tags, gfp);
	/* [한국어] 실패해도 경고를 내지 않는다(__GFP_NOWARN). 스케줄러 부착은
	 * 실패해도 "none"으로 계속 동작하는 선택적 기능이기 때문이다. */
	if (!et)
		/* [한국어] 아직 아무 tag map도 잡지 않았으므로 그냥 반환. */
		return NULL;

	/* [한국어] nr_requests 기록. 나중에 blk_mq_init_sched()가 q->nr_requests로 복사해,
	 * 디스패치 루프의 max_dispatch 상한으로도 쓰인다 */
	et->nr_requests = nr_requests;
	/* [한국어] nr_hw_queues: 해제 시 루프 상한으로 사용 */
	et->nr_hw_queues = nr_hw_queues;

	/* [한국어] 위에서 정한 nr_tags에 맞춰 실제 태그 맵을 만든다. 두 모드의
	 * 차이가 크기 인자에서도 드러난다 — 공유 모드는 MAX_SCHED_RQ(= 16*128
	 * = 2048)라는 전역 상한을 쓰고, 비공유 모드는 큐별 nr_requests를 쓴다. */
	if (blk_mq_is_shared_tags(set->flags)) {
		/* Shared tags are stored at index 0 in @tags. */
		/* [한국어] 공유 tag map: BLK_MQ_NO_HCTX_IDX = 특정 hctx 미귀속; MAX_SCHED_RQ = 공유 pool 최대 크기 */
		et->tags[0] = blk_mq_alloc_map_and_rqs(set, BLK_MQ_NO_HCTX_IDX,
					MAX_SCHED_RQ);
		/* [한국어] 공유 모드에서는 tag map이 하나뿐이라 롤백할 것이 없다. */
		if (!et->tags[0])
			/* [한국어] 할당 실패: et만 해제 후 NULL 반환 */
			goto out;
	} else {
		/* [한국어] 비공유 모드: 하드웨어 큐 i마다 nr_requests 크기의 독립 tag map을 만든다 */
		for (i = 0; i < et->nr_hw_queues; i++) {
			et->tags[i] = blk_mq_alloc_map_and_rqs(set, i,
					/* [한국어] 이 하드웨어 큐가 담을 수 있는 대기 request 수. */
					et->nr_requests);
			/* [한국어] 중간에 실패하면 앞서 성공한 0..i-1을 되돌려야 한다. */
			if (!et->tags[i])
				/* [한국어] i번째 실패: 0~i-1 tag map 롤백 필요 */
				goto out_unwind;
		}
	}

	/* [한국어] 모든 tag map 준비 완료 — 호출자가 res->et에 담아 두었다가
	 * elevator_alloc()에서 eq->et로 연결한다. */
	return et;
out_unwind:
	/* [한국어] out_unwind: 실패 직전까지 성공했던 tag map들을 역순으로 되돌린다.
	 * --i 로 시작하는 이유: 실패한 i번째는 할당되지 않았으므로 그 앞부터 지운다 */
	while (--i >= 0)
		blk_mq_free_map_and_rqs(set, et->tags[i], i);
out:
	/* [한국어] elevator_tags 구조체 자체 해제 */
	kfree(et);
	/* [한국어] 두 실패 경로(out, out_unwind)가 여기서 만나 NULL을 반환한다.
	 * 호출자 blk_mq_alloc_sched_res()가 -ENOMEM으로 바꿔 위로 올린다. */
	return NULL;
}

/*
 * [한국어]
 * blk_mq_alloc_sched_res - 하나의 queue에 대한 elevator_resources 할당
 *
 * @q:            request_queue(디스크 하나에 하나)
 * @type:         elevator_type (alloc_sched_data ops 호출 및 data 초기화)
 * @res:          결과를 저장할 elevator_resources (out parameter)
 * @nr_hw_queues: 하드웨어 큐 개수 (스케줄러 태그 배열 길이 결정)
 * @return:       0 = 성공; -ENOMEM = 메모리 부족
 *
 * elevator 전환 시 새 elevator를 위해 필요한 자원(tag pool + private data)을 미리 할당한다.
 * blk_mq_default_nr_requests(set) = 2 * min(set->queue_depth, 128)로 태그 풀 크기를
 * 결정한다. 큐 깊이가 큰 NVMe에서는 이 값이 256으로 고정되므로, 스케줄러가
 * 대기시킬 수 있는 request 수가 드라이버 태그 수보다 오히려 적어진다.
 * private data 할당 실패 시 이미 할당한 tag pool을 롤백한다.
 */
int blk_mq_alloc_sched_res(struct request_queue *q,
		struct elevator_type *type,
		struct elevator_resources *res,
		unsigned int nr_hw_queues)
{
	/* [한국어] set: 이 큐가 속한 blk_mq_tag_set — 태그 공유 여부와 queue_depth 보유 */
	struct blk_mq_tag_set *set = q->tag_set;

	/* [한국어] blk_mq_default_nr_requests(set) = 2 * min(set->queue_depth, BLKDEV_DEFAULT_RQ=128).
	 * 2배로 잡는 이유는 스케줄러가 재정렬할 여유를 주기 위해서지만, 128 상한 때문에
	 * 큐 깊이가 큰 장치에서는 256으로 잘린다 */
	res->et = blk_mq_alloc_sched_tags(set, nr_hw_queues,
			blk_mq_default_nr_requests(set));
	/* [한국어] 태그 풀이 없으면 스케줄러를 붙일 수 없다. */
	if (!res->et)
		/* [한국어] 전환을 포기한다. 호출자(elevator_change)는 이 시점에
		 * 아직 큐를 얼리지도 않았으므로 되돌릴 것이 없다. */
		return -ENOMEM;

	/* [한국어] blk_mq_alloc_sched_data(): elevator_type->ops.alloc_data()로 private 구조체 할당
	 * (mq-deadline: dd_per_prio 배열; BFQ: bfq_data; kyber: kyber_queue) */
	res->data = blk_mq_alloc_sched_data(q, type);
	/* [한국어] 이 함수는 NULL이 아니라 ERR_PTR을 돌려주므로 IS_ERR로 검사한다.
	 * (스케줄러에 따라 사설 데이터가 없어 NULL을 정상 반환할 수 있어,
	 *  NULL 검사로는 성공과 실패를 구분할 수 없기 때문이다.) */
	if (IS_ERR(res->data)) {
		/* [한국어] private data 실패: 이미 할당한 et(tag pool) 롤백 */
		blk_mq_free_sched_tags(res->et, set);
		/* [한국어] IS_ERR로 받은 실제 에러 코드 대신 -ENOMEM으로 통일한다.
		 * 이 경로의 실패 원인은 사실상 메모리 부족뿐이고, 호출자도
		 * 성공/실패만 구분하기 때문이다. */
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * blk_mq_alloc_sched_res_batch - tagset의 모든 queue에 elevator 자원 일괄 할당
 *
 * @elv_tbl:      queue id → elv_change_ctx xarray (blk_mq_alloc_sched_ctx_batch로 미리 준비)
 * @set:          대상 blk_mq_tag_set. 태그 공유 여부(flags)와 queue_depth를 여기서 읽는다
 * @nr_hw_queues: 새 하드웨어 큐 개수 (스케줄러 태그 배열 길이 결정)
 * @return:       0 = 성공; 음수 = 오류 (할당된 자원은 out_unwind에서 자동 해제)
 *
 * update_nr_hwq_lock write lock 하에 호출되며, elevator가 붙은 queue에 대해
 * blk_mq_alloc_sched_res()를 호출해 tag pool과 private data를 할당한다.
 * 중간에 실패하면 list_for_each_entry_continue_reverse로 역추적해 성공한 것들을 롤백한다.
 */
int blk_mq_alloc_sched_res_batch(struct xarray *elv_tbl,
		struct blk_mq_tag_set *set, unsigned int nr_hw_queues)
{
	/* [한국어] elv_tbl에서 꺼낸 큐별 전환 컨텍스트. */
	struct elv_change_ctx *ctx;
	/* [한국어] 순회 커서. 실패 시 이 포인터가 "실패한 큐"를 가리킨 채로
	 * out_unwind에 도달하고, _continue_reverse가 그 지점부터 거꾸로 돈다. */
	struct request_queue *q;
	/* [한국어] 초기값 -ENOMEM: queue가 없거나 첫 번째 queue에서 실패 시 이 값 반환 */
	int ret = -ENOMEM;

	/* [한국어] update_nr_hwq_lock write lock 보유 확인 */
	lockdep_assert_held_write(&set->update_nr_hwq_lock);

	/* [한국어] 이 태그 세트를 공유하는 모든 큐를 순회한다. 하드웨어 큐 수가
	 * 바뀌면 그 태그 세트를 쓰는 모든 큐의 스케줄러 자원을 새 크기로 다시
	 * 잡아야 하기 때문이다. */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/*
		 * Accessing q->elevator without holding q->elevator_lock is
		 * safe because we're holding here set->update_nr_hwq_lock in
		 * the writer context. So, scheduler update/switch code (which
		 * acquires the same lock but in the reader context) can't run
		 * concurrently.
		 */
		/* [한국어] 스케줄러가 붙은 큐만 대상. "none"인 큐(멀티큐 NVMe의
		 * 기본 상태)는 잡을 자원이 없다. */
		if (q->elevator) {
			/* [한국어] xa_load(): 이 queue의 elv_change_ctx 조회 */
			ctx = xa_load(elv_tbl, q->id);
			/* [한국어] WARN_ON_ONCE를 조건식 안에 둔 관용구 — 경고를 남기면서
			 * 동시에 그 조건으로 분기한다. */
			if (WARN_ON_ONCE(!ctx)) {
				/* [한국어] ctx가 없으면 alloc_sched_ctx_batch와 불일치 — 프로그래밍 오류 */
				/* [한국어] "그런 컨텍스트가 없다"를 -ENOENT로 알린다.
				 * -ENOMEM과 구분해야 원인을 추적할 수 있다. */
				ret = -ENOENT;
				/* [한국어] 이미 자원을 잡은 앞쪽 큐들을 되돌리러 간다. */
				goto out_unwind;
			}

			/* [한국어] 이 queue의 elevator type으로 tag pool + private data 할당 */
			ret = blk_mq_alloc_sched_res(q, q->elevator->type,
					&ctx->res, nr_hw_queues);
			/* [한국어] 한 큐라도 실패하면 전체를 되돌린다. 일부만 새 크기로
			 * 바뀐 중간 상태를 남기면 hctx 배열과 태그 배열의 길이가 어긋난다. */
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
			/* [한국어] 롤백 경로에서는 ctx가 없어도 경고하지 않고 조용히
			 * 넘어간다 — 이미 에러 처리 중이라 추가 경고가 소음이 된다. */
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
 * @q:   request_queue(디스크 하나에 하나)
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
	/* [한국어] et: 미리 할당된 스케줄러 태그 풀 */
	struct elevator_tags *et = res->et;
	/* [한국어] hctx: 각 하드웨어 큐 컨텍스트 */
	struct blk_mq_hw_ctx *hctx;
	struct elevator_queue *eq;
	unsigned long i;
	int ret;

	/* [한국어] elevator_alloc(): elevator_queue 할당 + q->elevator 등록 + kobject 초기화 */
	eq = elevator_alloc(q, e, res);
	if (!eq)
		return -ENOMEM;

	/* [한국어] q->nr_requests를 scheduler tag pool 크기로 설정:
	 * elevator가 이 많큼의 request를 동시에 관리할 수 있어야 하드웨어 큐를 놀리지 않고 채울 수 있다 */
	q->nr_requests = et->nr_requests;

	if (blk_mq_is_shared_tags(flags)) {
		/* Shared tags are stored at index 0 in @et->tags. */
		/* [한국어] 모든 hctx(하드웨어 큐)가 공유할 scheduler tag map 포인터 등록 */
		q->sched_shared_tags = et->tags[0];
		/* [한국어] 공유 tag map의 크기(nr_requests)를 실제 tag bitmap에 반영 */
		blk_mq_tag_update_sched_shared_tags(q, et->nr_requests);
	}

	/* [한국어] 모든 hctx(하드웨어 큐)에 scheduler tag map 연결:
	 * hctx->sched_tags는 dispatch 시 스케줄러 태그를 확보하는 데 사용 */
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

	/* [한국어] ops.init_hctx(): hctx별 elevator context 초기화; 하드웨어 큐마다 하나씩 필요한 스케줄러 사설 자료구조 설정 */
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
 * @q: request_queue(디스크 하나에 하나)
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
	/* [한국어] 각 하드웨어 큐(hctx) 순회용 */
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
 * @q: request_queue(디스크 하나에 하나)
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
	/* [한국어] 하드웨어 큐 컨텍스트 순회용 */
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;
	/* [한국어] flags: 마지막 hctx의 flags — shared tags 판단에 사용 */
	unsigned int flags = 0;

	/* [한국어] 모든 hctx(하드웨어 큐)에 대해 per-SQ scheduler context 해제 */
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
 *     ├─ [residual 있음] → dispatch_rq_list → mq_ops->queue_rq()
 *     ├─ [elevator 있음] → blk_mq_do_dispatch_sched
 *     │      └─ __blk_mq_do_dispatch_sched (budget → 드라이버 태그 → mq_ops->queue_rq)
 *     └─ [elevator 없음] → blk_mq_do_dispatch_ctx
 *            └─ 라운드 로빈 ctx → dispatch_rq_list → mq_ops->queue_rq()
 *
 * 핵심 자원 관계:
 *   elevator_tags (et)       ← 스케줄러 태그 풀
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
