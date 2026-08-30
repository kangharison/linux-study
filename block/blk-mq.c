// SPDX-License-Identifier: GPL-2.0
/*
 * Block multiqueue core code
 *
 * Copyright (C) 2013-2014 Jens Axboe
 * Copyright (C) 2013-2014 Christoph Hellwig
 */
/*
 * [한국어 설명] 블록 멀티큐(Multi-Queue) 핵심 구현체 (blk-mq.c)
 *
 * === 파일의 역할 ===
 * blk-mq.c 는 Linux 블록 레이어의 멀티큐(MQ) 아키텍처 핵심 파일이다. 상위 계층에서
 * 전달받은 bio(Block I/O) 를 request 로 변환하고, CPU 친화성을 고려한 소프트웨어
 * 큐(blk_mq_ctx)를 거쳐 하드웨어 큐(blk_mq_hw_ctx, 이하 hctx)에 분배(dispatch)한
 * 뒤 디바이스 드라이버의 queue_rq 콜백으로 전달하는 전 과정을 책임진다.
 * 요청의 완료(completion) 경로도 이 파일에서 처리한다 — 드라이버가 완료를 통보하면
 * softirq / IPI 를 경유해 블록 통계를 갱신하고 bio 완료를 상위로 전달한다.
 * request 생명주기(alloc → init → issue → complete → free) 전체가 이 파일 내에
 * 구현되어 있으며, plug/unplug 배치 처리, freeze/quiesce/timeout 등 큐 상태 관리도
 * 함께 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트: 프로세스 컨텍스트(submit_bio, ioctl), softirq(완료 처리),
 * 인터럽트 핸들러에서 간접 호출됨.
 *
 * [VFS/응용] write(2) / io_uring → submit_bio()
 *       ↓
 * [blk-core.c] blk_mq_submit_bio()  ← 이 파일의 주 진입점
 *       ↓  blk_mq_attempt_bio_merge()  : 기존 request 와 bio 병합 시도 (blk-merge.c)
 *       ↓  blk_mq_get_new_requests()   : 새 request 할당 (blk-mq-tag.c 협동)
 *       ↓  blk_add_rq_to_plug()        : plug 리스트에 축적, 나중에 일괄 flush
 *          또는 blk_mq_try_issue_directly(): 즉시 hctx 에 투입
 *       ↓
 * [dispatch] blk_mq_dispatch_rq_list()
 *       ↓  blk_mq_get_budget_and_tag() : budget 확인 + 드라이버 tag 할당
 *       ↓  ops->queue_rq()             : NVMe / SCSI 드라이버에 전달
 *       ↓
 * [NVMe] nvme_queue_rq() → nvme_prep_rq(SQE 빌드+DMA 매핑)
 *          → nvme_sq_copy_cmd(64B SQE 를 SQ 링에 memcpy)
 *          → nvme_write_sq_db(bd->last 이면 SQ tail doorbell MMIO)
 *       ↓
 * [완료] nvme_irq() → nvme_poll_cq → nvme_handle_cqe
 *          → nvme_try_complete_req → blk_mq_complete_request_remote()
 *            (다른 CPU 면 IPI → blk_done_softirq, 같은 CPU 면 false 반환)
 *          → nvme_pci_complete_rq / nvme_pci_complete_batch
 *       ↓  blk_mq_end_request()         : bio 완료 + tag/request 반환
 *       ↓  blk_account_io_done()        : 블록 통계 갱신
 *
 * === 타 모듈과의 연결 ===
 * 의존(호출):
 *   blk-mq-tag.c   : request 풀·tag 비트맵 할당/해제 (blk_mq_alloc_rq_map 등)
 *   blk-mq-sched.c : IO 스케줄러(elevator) 연동 — insert/dispatch 중개
 *   blk-merge.c    : bio/request 병합 (blk_attempt_bio_merge)
 *   blk-mq-cpumap.c: CPU → hctx 매핑, NUMA 친화성
 *   blk-core.c     : submit_bio 진입점, bio 기초 처리
 *   blk-cgroup.c   : cgroup blkio 통계 (blk_account_io_*)
 *   blk-wbt.c      : writeback throttle (rq_qos)
 *   blk-stat.c     : 요청 지연/크기 통계
 * 피의존(호출 받음):
 *   drivers/nvme/host/pci.c : nvme_mq_ops 로 queue_rq/queue_rqs/commit_rqs/
 *                             complete/timeout/poll/map_queues/init_hctx/
 *                             init_request 를 등록한다. 블록 계층은 이 파일의
 *                             어떤 nvme_* 심볼도 직접 부르지 않는다 —
 *                             전부 q->mq_ops 를 통한 간접 호출이다.
 *   drivers/scsi/*          : SCSI MQ 드라이버들
 * 핵심 공유 자료구조:
 *   request_queue   : 큐 상태(freeze/quiesce/dying), 스케줄러, QoS 체인
 *   blk_mq_tag_set  : 드라이버가 alloc 하는 공유 tag 풀
 *   blk_mq_hw_ctx   : 하드웨어 큐 1개 — SQ/CQ 1쌍에 대응
 *   blk_mq_ctx      : per-CPU 소프트웨어 큐 — CPU 별로 hctx 에 매핑
 *
 * === 주요 함수/구조체 요약 ===
 *   blk_mq_submit_bio()        : bio→request 변환 후 hctx dispatch 까지 책임지는 메인 경로
 *   blk_mq_dispatch_rq_list()  : hctx pending list 를 드라이버 queue_rq 로 전달
 *   blk_mq_end_request()       : 완료된 request 의 bio chain 완료 + tag 반환
 *   blk_mq_complete_request()  : 드라이버 완료 통보 수신 → softirq/IPI 라우팅
 *   blk_mq_alloc_request()     : request_queue 에서 request 직접 할당 (bdev 외부 API)
 *   blk_mq_alloc_tag_set()     : 드라이버 초기화 시 hctx·tag 풀·request 메모리 생성
 *   blk_add_rq_to_plug()       : plug 리스트에 request 추가 — flush 시 batch dispatch
 *   blk_mq_freeze_queue_*()    : 큐를 동결하여 새 IO 진입 차단 (reset/shutdown 전처리)
 *   blk_mq_quiesce_queue()     : 큐를 일시 중단 (in-flight 완료 대기 포함)
 *   blk_mq_alloc_map_and_rqs() : hctx 별 tag→request 배열 + DMA 메모리 할당
 */
#include <linux/kernel.h>        /* [한국어] printk, ARRAY_SIZE, container_of 등 커널 기반 매크로 */
#include <linux/module.h>        /* [한국어] EXPORT_SYMBOL_GPL, MODULE_* 매크로 — blk_mq API 익스포트 */
#include <linux/backing-dev.h>   /* [한국어] backing_dev_info — writeback 쓰로틀 연동 */
#include <linux/bio.h>           /* [한국어] struct bio, bio_vec, bio_alloc, bio_endio 등 — blk-mq 의 입력 단위 */
#include <linux/blkdev.h>        /* [한국어] struct request_queue, struct gendisk, blk_*() API 선언 */
#include <linux/blk-integrity.h> /* [한국어] T10 DIF/DIX 무결성 보호 헤더 — NVMe PI 태그 관리 */
#include <linux/kmemleak.h>      /* [한국어] kmemleak_not_leak(): DMA 메모리 오탐지 억제 */
#include <linux/mm.h>            /* [한국어] alloc_pages, kmap, virt_to_page 등 메모리 관리 */
#include <linux/init.h>          /* [한국어] __init, __exit 섹션 속성 — blk_mq_init() 에 사용 */
#include <linux/slab.h>          /* [한국어] kmalloc/kzalloc/kfree — request, tag, ctx 메모리 할당 */
#include <linux/workqueue.h>     /* [한국어] INIT_WORK, queue_work — requeue, timeout, run_hw_queue 비동기 처리 */
#include <linux/smp.h>           /* [한국어] smp_call_function_single(), get_cpu/put_cpu — IPI 완료 전송 */
#include <linux/interrupt.h>     /* [한국어] raise_softirq(BLOCK_SOFTIRQ) — 완료 softirq 발사 */
#include <linux/llist.h>         /* [한국어] llist_head/llist_add/llist_del_all — per-CPU 완료 lock-free 리스트 */
#include <linux/cpu.h>           /* [한국어] CPU hotplug 콜백 등록 (cpuhp_setup_state) */
#include <linux/cache.h>         /* [한국어] __cacheline_aligned, L1_CACHE_BYTES — false sharing 방지 */
#include <linux/sched/topology.h>/* [한국어] CPU topology: cpumask, NUMA 거리 — hctx-CPU 매핑에 사용 */
#include <linux/sched/signal.h>  /* [한국어] signal_pending() — freeze/quiesce 대기 중 시그널 처리 */
#include <linux/suspend.h>       /* [한국어] PM_SUSPEND_* — 전원 관리와 큐 freeze 연동 */
#include <linux/delay.h>         /* [한국어] msleep_interruptible() — timeout 대기 */
#include <linux/crash_dump.h>    /* [한국어] is_kdump_kernel() — kdump 환경에서 큐 초기화 단순화 */
#include <linux/prefetch.h>      /* [한국어] prefetch() — request 배열 prefetch 로 캐시 miss 감소 */
#include <linux/blk-crypto.h>    /* [한국어] 인라인 암호화 초기화/해제 — NVMe inline encryption */
#include <linux/part_stat.h>     /* [한국어] part_stat_add() — 파티션별 섹터/IO 통계 갱신 */
#include <linux/sched/isolation.h>/* [한국어] housekeeping_cpumask() — 격리 CPU 에서 hctx 제외 */

#include <trace/events/block.h>  /* [한국어] trace_block_rq_*, trace_block_bio_* tracepoint — perf/ftrace 계측 */

#include <linux/t10-pi.h>        /* [한국어] T10 PI(보호 정보) 계산 헬퍼 — integrity 검증용 */
#include "blk.h"                 /* [한국어] 블록 레이어 내부 공유 선언 (blk_queue_*, QUEUE_FLAG_* 등) */
#include "blk-mq.h"              /* [한국어] blk_mq_hw_ctx, blk_mq_ctx, blk_mq_tag 등 MQ 핵심 구조체 선언 */
#include "blk-mq-debugfs.h"      /* [한국어] debugfs 인터페이스 — hctx/ctx 상태 노출 */
#include "blk-pm.h"              /* [한국어] 런타임 전원 관리(PM) 통합 — 자동 suspend/resume */
#include "blk-stat.h"            /* [한국어] blk_stat_* — IO 지연·크기 이동 평균 통계 */
#include "blk-mq-sched.h"        /* [한국어] IO 스케줄러(elevator) 연동: elv_merge, dispatch 중개 */
#include "blk-rq-qos.h"          /* [한국어] rq_qos 체인 — WBT(writeback throttle), iolatency, iocost */

/* [한국어] per-CPU 완료 리스트: 드라이버가 완료를 통보한 request 를 다른 CPU 로
 * 라우팅하기 전에 임시 보관한다. llist(lock-free linked list) 로 구현되어
 * 완료 인터럽트 컨텍스트에서도 lock 없이 push 가능하다.
 * 설정자: blk_mq_complete_request_remote() 가 llist_add() 로 추가.
 * 읽는 자: blk_done_softirq() 가 llist_del_all() 로 일괄 처리.
 * 동기화: llist 자체가 atomic 연산 기반이라 별도 락 불필요. */
static DEFINE_PER_CPU(struct llist_head, blk_cpu_done);

/* [한국어] per-CPU call_single_data(CSD): 완료된 request 를 원래 CPU 로 보내기 위한
 * IPI(Inter-Processor Interrupt) 발송용 콜백 데이터 구조체.
 * blk_mq_complete_send_ipi() 가 smp_call_function_single_async() 를 호출할 때 사용.
 * CSD 는 per-CPU 로 1개만 유지되어 동일 CPU 에 대한 중복 IPI 를 억제한다.
 * 동기화: CSD_FLAG_LOCK 비트로 동시 발송 충돌 방지 (smp 내부 처리). */
static DEFINE_PER_CPU(call_single_data_t, blk_cpu_csd);

/* [한국어] CPU hotplug 전역 뮤텍스: CPU 온라인/오프라인 이벤트 처리 시
 * hctx 재매핑(blk_mq_hctx_notify_online/offline)을 직렬화한다.
 * NVMe 컨트롤러의 SQ→CPU affinity 를 다중 큐에 걸쳐 일관되게 갱신하기 위해 필요.
 * 잠금 범위: blk_mq_add/remove_hw_queues_cpuhp() 내부로 한정. */
static DEFINE_MUTEX(blk_mq_cpuhp_lock);

/* [한국어] 아래 4개는 전방 선언(forward declaration)이다. 이 파일 안에서
 * 정의보다 앞선 위치에서 호출되기 때문에, C 컴파일러가 시그니처를 미리 알도록
 * 여기에 선언만 해 둔다. 모두 static 이므로 이 파일 밖에서는 보이지 않는다. */

/* [한국어] request 를 elevator/sw 큐/dispatch 리스트 중 알맞은 곳에 넣는 라우터.
 * flags 로 head 삽입 여부 등을 지정한다. */
static void blk_mq_insert_request(struct request *rq, blk_insert_t flags);
/* [한국어] elevator 를 우회해 hctx->dispatch 에 직접 넣는다. passthrough
 * (nvme-cli 커맨드 등)와 flush 처럼 스케줄러에 맡기면 안 되는 요청용. */
static void blk_mq_request_bypass_insert(struct request *rq,
		blk_insert_t flags);
/* [한국어] 리스트의 request 들을 스케줄러를 거치지 않고 곧장 드라이버
 * queue_rq 로 밀어 넣는다(plug flush 의 빠른 경로). */
static void blk_mq_try_issue_list_directly(struct blk_mq_hw_ctx *hctx,
		struct list_head *list);
/* [한국어] 하나의 hctx 에 대해 mq_ops->poll 을 반복 호출하는 폴링 루프.
 * NVMe 라면 nvme_poll 이 불려 CQ 를 직접 읽는다. */
static int blk_hctx_poll(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			 struct io_comp_batch *iob, unsigned int flags);

/*
 * Check if any of the ctx, dispatch list or elevator
 * have pending work in this hardware queue.
 */
/*
 * [한국어]
 * blk_mq_hctx_has_pending - 이 hctx(하드웨어 큐)에 처리 대기 중인 request 가 있는지 검사
 *
 * @hctx: 확인할 blk_mq_hw_ctx — NVMe 1개 SQ/CQ 쌍에 대응
 * @return: true 이면 pending 작업 있음 → 호출자가 run_hw_queue() 를 예약해야 함
 *
 * 세 가지 소스에서 pending 여부를 확인한다:
 * 1) hctx->dispatch 리스트 — blk_mq_dispatch_rq_list 에서 드라이버 반환으로 남겨진 요청
 * 2) hctx->ctx_map 비트맵  — 매핑된 sw 큐(CPU) 중 아직 처리 안 된 request 를 가진 CPU
 * 3) 스케줄러(elevator) 큐 — mq-deadline/BFQ 내부에 남아있는 dispatch 대상 request
 * 실행 컨텍스트: 인터럽트/softirq/프로세스 컨텍스트 모두 가능 (lock-free 검사).
 * 이 함수 자체는 변경 없이 읽기만 수행한다.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue → blk_mq_hctx_has_pending 검사 후 dispatch 진행 결정
 *   blk_mq_dispatch_rq_list → pending 소진 시 has_pending==false 로 중단
 */
static bool blk_mq_hctx_has_pending(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] hctx->dispatch 가 비어있지 않으면 드라이버가 이전에 처리하지 못하고
	 * 돌려준 request 들이 남아 있는 것이다 — 재시도 dispatch 가 필요하다.
	 * list_empty_careful: 메모리 순서를 보장하는 안전한 empty 검사 */
	return !list_empty_careful(&hctx->dispatch) ||
	/* [한국어] sbitmap_any_bit_set(&hctx->ctx_map): 이 hctx 에 매핑된 CPU(sw 큐) 중
	 * request 를 삽입해 두고 아직 dispatch 하지 않은 CPU 가 하나라도 있으면 true.
	 * 각 CPU 가 hctx->ctx_map 에서 자신의 bit 를 set 할 때 blk_mq_hctx_mark_pending() 를 씀 */
		sbitmap_any_bit_set(&hctx->ctx_map) ||
	/* [한국어] blk_mq_sched_has_work: IO 스케줄러(elevator)가 dispatch 할 request 를
	 * 내부 큐(RB-tree 또는 list)에 보유하고 있는지 확인.
	 * elevator 가 없는 경우 항상 false 반환 */
			blk_mq_sched_has_work(hctx);
}

/*
 * Mark this ctx as having pending work in this hardware queue
 */
/*
 * [한국어]
 * blk_mq_hctx_mark_pending - 이 sw 큐(ctx)가 hctx 에 처리할 request 를 가지고 있음을 표시
 *
 * @hctx: 표시 대상 하드웨어 큐 (NVMe SQ 1개)
 * @ctx:  pending 상태로 표시할 CPU 의 소프트웨어 큐 (blk_mq_ctx)
 *
 * blk_mq_ctx 는 CPU 하나에 대응하는 sw 큐이다. CPU 가 request 를 sw 큐에 삽입했을 때
 * hctx->ctx_map 에 자신의 bit 를 set 해서 "나에게 처리할 request 가 있다"고 알린다.
 * blk_mq_hctx_has_pending() 이 ctx_map 을 읽어 run_hw_queue 를 유발한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_insert_request 경로).
 *
 * 호출 체인:
 *   blk_mq_insert_request → blk_mq_hctx_mark_pending → sbitmap_set_bit
 */
static void blk_mq_hctx_mark_pending(struct blk_mq_hw_ctx *hctx,
				     struct blk_mq_ctx *ctx)
{
	/* [한국어] ctx->index_hw[hctx->type]: 이 CPU 가 해당 hctx 에서 차지하는 슬롯 인덱스.
	 * hctx->type 은 HCTX_TYPE_DEFAULT/READ/POLL 중 하나 — NVMe 는 보통 DEFAULT. */
	const int bit = ctx->index_hw[hctx->type];

	/* [한국어] 이미 set 되어 있으면 중복 set 을 피한다.
	 * sbitmap_test_bit 는 atomic 읽기; set 사이의 경쟁은 무해하다
	 * (set 이 중복되어도 clear 가 없으면 bit 는 계속 set 상태를 유지). */
	if (!sbitmap_test_bit(&hctx->ctx_map, bit))
		/* [한국어] atomic bit set: 이 CPU 가 처리할 request 를 가지고 있음을 hctx 에 알림.
		 * run_hw_queue() 가 이 비트를 보고 flush_busy_ctx 를 호출해 sw 큐에서 꺼낸다. */
		sbitmap_set_bit(&hctx->ctx_map, bit);
}

/*
 * [한국어]
 * blk_mq_hctx_clear_pending - sw 큐(ctx)의 pending 비트를 hctx->ctx_map 에서 해제
 *
 * @hctx: 대상 하드웨어 큐
 * @ctx:  해제할 CPU 의 소프트웨어 큐
 *
 * dispatch 과정에서 sw 큐가 비워졌을 때 호출된다. ctx_map 에서 해당 CPU 의 비트를
 * clear 해 다음 has_pending() 검사에서 false 가 되도록 한다.
 * 실행 컨텍스트: blk_mq_dispatch_rq_list 내부, softirq 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   flush_busy_ctx → blk_mq_hctx_clear_pending → sbitmap_clear_bit
 */
static void blk_mq_hctx_clear_pending(struct blk_mq_hw_ctx *hctx,
				      struct blk_mq_ctx *ctx)
{
	/* [한국어] index_hw[type] 으로 비트 위치를 계산하고 atomic 으로 clear.
	 * clear 후 ctx_map 이 0이면 has_pending 이 false 를 반환해 run_hw_queue 를 중단. */
	const int bit = ctx->index_hw[hctx->type];

	/* [한국어] sbitmap_clear_bit: 해당 bit 를 원자적으로 0으로 만든다. */
	sbitmap_clear_bit(&hctx->ctx_map, bit);
}

/*
 * [한국어] struct mq_inflight — in-flight(진행 중) I/O 집계용 보조 구조체
 *
 * blk_mq_in_driver_rw() 가 blk_mq_check_in_driver() 콜백에 priv 로 전달해
 * tag 전체를 순회하며 MQ_RQ_IN_FLIGHT 상태의 request 수를 누적한다.
 * 이 구조체는 스택에 할당되어 한 번의 tag_busy_iter 호출 동안만 사용된다.
 */
struct mq_inflight {
	struct block_device *part;
	/* [한국어] 집계 대상 블록 디바이스 파티션 또는 NVMe namespace.
	 * 설정자: blk_mq_in_driver_rw() 가 { .part = part } 로 초기화.
	 * 읽는 자: blk_mq_check_in_driver() 가 파티션 여부 확인에 사용.
	 * bdev_is_partition(part) 가 true 면 rq->part == mi->part 인 경우만 집계. */

	unsigned int inflight[2];
	/* [한국어] 방향별(READ=0, WRITE=1) in-flight request 수.
	 * 설정자: blk_mq_check_in_driver() 가 MQ_RQ_IN_FLIGHT 상태 request 당 ++.
	 * 읽는 자: blk_mq_in_driver_rw() 가 inflight[READ]/inflight[WRITE] 를 호출자에 복사.
	 * 값 범위: 0 ~ hctx 의 총 tag 수(q_depth). 완료 전에도 drv 에서 다시 count 될 수 있음.
	 * 동기화: tag_busy_iter 가 태그별로 콜백을 직렬 호출하므로 추가 락 불필요. */
};

/*
 * [한국어]
 * blk_mq_check_in_driver - tag 순회 콜백: request 가 드라이버에서 처리 중이면 집계
 *
 * @rq:   현재 순회 중인 request — blk_mq_queue_tag_busy_iter 가 하나씩 전달
 * @priv: struct mq_inflight 포인터 — 누적 카운터와 대상 파티션 보유
 * @return: true — 순회를 계속 진행하도록 요청 (false 시 iter 조기 중단)
 *
 * 세 조건을 AND 로 검사한다:
 * (1) RQF_IO_STAT 플래그 — IO 통계 대상으로 표시된 request 만 집계.
 * (2) 파티션 필터 — 전체 디스크이면 모두 포함; 파티션이면 같은 파티션만.
 * (3) MQ_RQ_IN_FLIGHT — SQ 에 제출되어 CQ 완료를 기다리는 상태.
 * 실행 컨텍스트: 프로세스 컨텍스트 (tag_busy_iter 내에서 동기 순회).
 *
 * 호출 체인:
 *   blk_mq_in_driver_rw → blk_mq_queue_tag_busy_iter → [blk_mq_check_in_driver]
 */
static bool blk_mq_check_in_driver(struct request *rq, void *priv)
{
	/* [한국어] priv 를 mq_inflight 구조체로 캐스팅 — 집계 결과를 여기에 쌓는다. */
	struct mq_inflight *mi = priv;

	/* [한국어] 세 조건을 모두 만족하는 request 만 in-flight 로 집계한다:
	 * RQF_IO_STAT: 이 request 가 IO 통계 대상임을 나타내는 플래그
	 * (!bdev_is_partition || rq->part == mi->part): 파티션 필터
	 * MQ_RQ_IN_FLIGHT: 드라이버에 넘어간 뒤 완료 전인 상태 */
	if (rq->rq_flags & RQF_IO_STAT &&
	    (!bdev_is_partition(mi->part) || rq->part == mi->part) &&
	    blk_mq_rq_state(rq) == MQ_RQ_IN_FLIGHT)
		/* [한국어] rq_data_dir: READ(0) 또는 WRITE(1) — 방향별로 카운터 증가.
		 * inflight[0]++ 은 READ, inflight[1]++ 은 WRITE in-flight 수를 증가시킨다. */
		mi->inflight[rq_data_dir(rq)]++;

	/* [한국어] true 반환 → 다음 tag 로 순회 계속. false 시 iter 조기 종료. */
	return true;
}

/*
 * [한국어]
 * blk_mq_in_driver_rw - 특정 블록 디바이스의 READ/WRITE in-flight 수를 반환
 *
 * @part:     대상 블록 디바이스 (파티션 또는 전체 디스크)
 * @inflight: READ/WRITE in-flight 수를 담을 2-원소 배열 (출력 매개변수)
 *
 * 내부적으로 해당 request_queue 의 모든 busy tag 를 순회하며
 * MQ_RQ_IN_FLIGHT 상태의 request 를 방향별로 카운트한다.
 * iostat 등의 통계 수집 경로에서 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs 읽기, iostat 폴링 등).
 *
 * 호출 체인:
 *   blk_inflight_rw (blk-sysfs.c) → [blk_mq_in_driver_rw]
 *     → blk_mq_queue_tag_busy_iter → blk_mq_check_in_driver
 */
void blk_mq_in_driver_rw(struct block_device *part, unsigned int inflight[2])
{
	/* [한국어] part 를 집계 필터로 사용하는 mq_inflight 구조체를 스택에 초기화 */
	struct mq_inflight mi = { .part = part };

	/* [한국어] blk_mq_queue_tag_busy_iter: request_queue 의 모든 busy tag 를 순회하며
	 * blk_mq_check_in_driver 콜백을 호출한다. mi 에 집계 결과가 쌓인다. */
	blk_mq_queue_tag_busy_iter(bdev_get_queue(part), blk_mq_check_in_driver,
				   &mi);
	/* [한국어] 누적된 READ in-flight 수를 출력 배열에 복사 */
	inflight[READ] = mi.inflight[READ];
	/* [한국어] 누적된 WRITE in-flight 수를 출력 배열에 복사 */
	inflight[WRITE] = mi.inflight[WRITE];
}

/*
 * [한국어] CONFIG_LOCKDEP 활성 시: freeze 소유권 추적 함수. LOCKDEP 가 없으면
 * 빈 stub 로 대체된다. 소유권 추적은 디버그 목적으로, 마지막 unfreeze 가 반드시
 * freeze 를 시작한 태스크에 의해 수행되는지 검증한다.
 */
#ifdef CONFIG_LOCKDEP
/*
 * [한국어]
 * blk_freeze_set_owner - 큐 freeze 소유권을 현재 태스크로 설정 (LOCKDEP 전용)
 *
 * @q:     동결할 request_queue
 * @owner: freeze 소유자 태스크 (보통 current); NULL 이면 비소유 freeze
 * @return: true 이면 이 태스크가 freeze 소유자로 새로 등록됨
 *
 * mq_freeze_depth 가 0일 때만 소유자를 새로 설정한다. 같은 태스크의 재진입
 * (재귀 freeze) 는 깊이만 증가시킨다. 다른 태스크가 owner 일 때는 아무것도 안 한다.
 * 실행 컨텍스트: mq_freeze_lock 을 잡은 상태에서 호출됨.
 *
 * 호출 체인:
 *   __blk_freeze_queue_start → [blk_freeze_set_owner]
 */
static bool blk_freeze_set_owner(struct request_queue *q,
				 struct task_struct *owner)
{
	/* [한국어] NULL owner = non-owner freeze (blk_freeze_queue_start_non_owner 경로). */
	if (!owner)
		return false;

	/* [한국어] 첫 번째 freeze (깊이가 0): 이 태스크를 소유자로 등록하고
	 * 큐와 디스크의 죽음 상태 스냅샷을 기록한다 (blk_unfreeze_release_lock 에서 검증). */
	if (!q->mq_freeze_depth) {
		/* [한국어] 소유자 태스크 포인터 기록 */
		q->mq_freeze_owner = owner;
		/* [한국어] 재귀 깊이 1로 초기화 */
		q->mq_freeze_owner_depth = 1;
		/* [한국어] 디스크가 없거나 GD_DEAD 상태면 죽은 디스크로 표시.
		 * GD_DEAD: 디스크가 삭제 중인 상태 — NVMe 컨트롤러 분리 시 세팅. */
		q->mq_freeze_disk_dead = !q->disk ||
			test_bit(GD_DEAD, &q->disk->state) ||
			/* [한국어] blk_queue_registered: 큐가 정상적으로 등록된 상태인지 확인 */
			!blk_queue_registered(q);
		/* [한국어] QUEUE_FLAG_DYING: 큐가 shutdown 단계에 있음을 표시하는 플래그.
		 * NVMe remove 시 blk_cleanup_queue() 가 이 플래그를 세팅한다. */
		q->mq_freeze_queue_dying = blk_queue_dying(q);
		return true;
	}

	/* [한국어] 같은 소유자의 재귀 freeze: 깊이만 증가시키고 소유권은 그대로 유지. */
	if (owner == q->mq_freeze_owner)
		q->mq_freeze_owner_depth += 1;
	/* [한국어] false 반환: 소유자 등록 없음 — lock 획득 불필요 */
	return false;
}

/* verify the last unfreeze in owner context */
/*
 * [한국어]
 * blk_unfreeze_check_owner - 마지막 unfreeze 가 freeze 소유자에 의한 것인지 검증
 *
 * @q: unfreeze 중인 request_queue
 * @return: true 이면 이 태스크가 마지막 unfreeze 를 수행하는 소유자
 *
 * mq_freeze_owner_depth 를 1씩 줄이고, 0이 되면 소유자를 NULL 로 초기화한다.
 * 소유자가 아닌 태스크가 unfreeze 하면 false 를 반환해 락 해제를 막는다.
 * 실행 컨텍스트: mq_freeze_lock 을 잡은 상태에서 호출됨.
 *
 * 호출 체인:
 *   __blk_mq_unfreeze_queue → [blk_unfreeze_check_owner]
 */
static bool blk_unfreeze_check_owner(struct request_queue *q)
{
	/* [한국어] 현재 태스크가 freeze 소유자가 아니면 false 반환 */
	if (q->mq_freeze_owner != current)
		return false;
	/* [한국어] 재귀 깊이를 줄인다. 0이 되면 소유자를 해제하고 true 반환 */
	if (--q->mq_freeze_owner_depth == 0) {
		/* [한국어] 마지막 unfreeze: 소유자 필드 초기화 */
		q->mq_freeze_owner = NULL;
		return true;
	}
	/* [한국어] 아직 재귀 중: 소유권을 유지하고 false 반환 */
	return false;
}

#else

/* [한국어] LOCKDEP 비활성 시 stub: 소유권 추적 불필요 → 항상 false */
static bool blk_freeze_set_owner(struct request_queue *q,
				 struct task_struct *owner)
{
	return false;
}

/* [한국어] LOCKDEP 비활성 시 stub */
static bool blk_unfreeze_check_owner(struct request_queue *q)
{
	return false;
}
#endif

/*
 * [한국어]
 * __blk_freeze_queue_start - 큐 동결(freeze) 시작: 새 IO 진입을 차단
 *
 * @q:     동결할 request_queue
 * @owner: freeze 소유자 태스크; NULL 이면 non-owner freeze
 * @return: true 이면 이 호출이 소유자 freeze 를 시작했음 (lock 획득 필요)
 *
 * "freeze" 는 큐에 새로운 IO 가 진입하지 못하도록 하는 메커니즘이다.
 * percpu_ref_kill(&q->q_usage_counter) 로 q_usage_counter 를 종료하면
 * 이후 percpu_ref_tryget 이 실패해 submit_bio 경로가 차단된다.
 * 이미 진행 중인 request 는 계속 완료될 수 있으며, 완료가 끝나면
 * percpu_ref_is_zero 조건이 true 가 되어 freeze_wait 를 깨운다.
 * 같은 태스크의 중첩 freeze 는 mq_freeze_depth 로 추적되며 idempotent 하다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (반드시 잠 들 수 있는 컨텍스트여야 함).
 *
 * 호출 체인:
 *   blk_freeze_queue_start → [__blk_freeze_queue_start]
 *     → percpu_ref_kill → blk_mq_run_hw_queues (pending flush)
 */
bool __blk_freeze_queue_start(struct request_queue *q,
			      struct task_struct *owner)
{
	/* [한국어] 소유자 여부 판단 (LOCKDEP 없으면 항상 false) */
	bool freeze;

	/* [한국어] mq_freeze_lock: freeze_depth 변경을 직렬화하는 뮤텍스 */
	mutex_lock(&q->mq_freeze_lock);
	/* [한국어] 소유권 등록 시도 (LOCKDEP 시 유효, 아니면 false) */
	freeze = blk_freeze_set_owner(q, owner);
	/* [한국어] mq_freeze_depth 를 1 증가시킨다. 처음 1이 되는 시점에만
	 * percpu_ref_kill 로 q_usage_counter 를 종료해 새 IO 진입을 차단한다.
	 * 이후에는 깊이만 증가시키고 카운터는 그대로 둔다. */
	if (++q->mq_freeze_depth == 1) {
		/* [한국어] percpu_ref_kill: q_usage_counter 를 "dying" 상태로 전환.
		 * 이후 percpu_ref_tryget(&q->q_usage_counter) 가 false 를 반환하여
		 * blk_queue_enter() 실패 → submit_bio 가 -ENODEV 로 조기 반환. */
		percpu_ref_kill(&q->q_usage_counter);
		/* [한국어] 락 해제 후 (다른 CPU 의 진행을 허용하기 위해 먼저 해제) */
		mutex_unlock(&q->mq_freeze_lock);
		/* [한국어] MQ 큐이면 현재 pending 중인 request 를 모두 드라이버에
		 * 투입(run)하여 완료를 가속한다. blk_mq_freeze_queue_wait 가 더 빨리 깨어나도록. */
		if (queue_is_mq(q))
			blk_mq_run_hw_queues(q, false);
	} else {
		/* [한국어] 이미 freeze 중: 깊이만 증가, 추가 액션 없음 */
		mutex_unlock(&q->mq_freeze_lock);
	}

	/* [한국어] 소유자 freeze 이면 true 반환 → 호출자가 blk_freeze_acquire_lock 호출 */
	return freeze;
}

/*
 * [한국어]
 * blk_freeze_queue_start - 소유자 기반 큐 동결 시작 (공개 API)
 *
 * @q: 동결할 request_queue
 *
 * __blk_freeze_queue_start 를 current 를 소유자로 호출한다.
 * LOCKDEP 활성 시 소유자이면 freeze lock 을 획득해 blk_mq_unfreeze 가
 * 같은 태스크에서 수행됨을 보장한다.
 * 드라이버의 error recovery, sysfs change, teardown 에서 호출된다.
 *
 * 호출 체인 (이 트리에서 grep 으로 확인한 실제 호출자만):
 *   blk_mq_freeze_queue_nomemsave (block/blk-mq.c)     → [blk_freeze_queue_start]
 *   blk_pre_runtime_suspend       (block/blk-pm.c)     → [blk_freeze_queue_start]
 *   nvme_mpath_start_freeze (drivers/nvme/host/multipath.c) → [blk_freeze_queue_start]
 *
 * NVMe 접점: 멀티패스 head 디스크(/dev/nvmeXnY)를 가진 서브시스템에서
 * 경로 목록을 바꾸기 전에 nvme_mpath_start_freeze() 가 모든 head 큐에
 * 이 함수를 걸어 신규 I/O 유입을 끊는다. 반면 컨트롤러 리셋 경로의
 * nvme_start_freeze() 는 이 함수가 아니라 blk_freeze_queue_start_non_owner()
 * 를 쓴다 — freeze 를 시작한 태스크와 푸는 태스크가 다르기 때문이다.
 */
void blk_freeze_queue_start(struct request_queue *q)
{
	/* [한국어] __blk_freeze_queue_start 가 true 반환 시 (소유자 freeze 시작)
	 * blk_freeze_acquire_lock 으로 lockdep freeze lock 을 획득 */
	if (__blk_freeze_queue_start(q, current))
		blk_freeze_acquire_lock(q);
}
EXPORT_SYMBOL_GPL(blk_freeze_queue_start);

/*
 * [한국어]
 * blk_mq_freeze_queue_wait - q_usage_counter 가 0이 될 때까지 대기
 *
 * @q: 대기할 request_queue
 *
 * percpu_ref_kill 호출 후 in-flight request 가 모두 완료되어
 * q_usage_counter 가 0이 될 때까지 블록한다. mq_freeze_wq 웨이트큐에서
 * percpu_ref 소멸 콜백이 wake_up_all 을 호출하면 이 함수가 반환된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sleep 가능).
 *
 * 호출 체인:
 *   blk_mq_freeze_queue_nomemsave → [blk_mq_freeze_queue_wait]
 */
void blk_mq_freeze_queue_wait(struct request_queue *q)
{
	/* [한국어] wait_event: percpu_ref_is_zero 조건이 true 가 될 때까지 sleep.
	 * q_usage_counter 가 0이 되면 percpu_ref 내부에서 mq_freeze_wq 를 wake 한다. */
	wait_event(q->mq_freeze_wq, percpu_ref_is_zero(&q->q_usage_counter));
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait);

/*
 * [한국어]
 * blk_mq_freeze_queue_wait_timeout - timeout 을 두고 q_usage_counter 소멸 대기
 *
 * @q:       대기할 request_queue
 * @timeout: 최대 대기 시간 (jiffies)
 * @return:  0이면 timeout 만료; 양수이면 조건 충족, 음수이면 인터럽트
 *
 * blk_mq_freeze_queue_wait 의 timeout 버전. dm(device-mapper) 등에서
 * 강제 reset 후 일정 시간만 기다리는 경우에 사용된다.
 *
 * 호출 체인:
 *   dm_table_set_restrictions → [blk_mq_freeze_queue_wait_timeout]
 */
int blk_mq_freeze_queue_wait_timeout(struct request_queue *q,
				     unsigned long timeout)
{
	/* [한국어] wait_event_timeout: timeout 내에 percpu_ref_is_zero 가 true 이면 양수 반환 */
	return wait_event_timeout(q->mq_freeze_wq,
					percpu_ref_is_zero(&q->q_usage_counter),
					timeout);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait_timeout);

/*
 * [한국어]
 * blk_mq_freeze_queue_nomemsave - 큐를 동결하고 in-flight 가 소진될 때까지 대기
 *
 * @q: 동결할 request_queue
 *
 * blk_freeze_queue_start 와 blk_mq_freeze_queue_wait 를 연속 호출하는 편의 함수.
 * "nomemsave" 는 memalloc_noio_save() 를 하지 않는다는 뜻이다. 공개 래퍼인
 * blk_mq_freeze_queue()(include/linux/blk-mq.h 의 inline)는 memalloc_noio_save()
 * 로 현재 태스크의 할당 컨텍스트를 저장한 뒤 이 함수를 부른다 — freeze 대기
 * 중에 이 큐로 되돌아오는 I/O(스왑/파일시스템 writeback)를 유발하면 데드락이
 * 되기 때문이다. 이미 noio 컨텍스트임이 확실한 호출자만 이 함수를 직접 쓴다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sleep 가능).
 *
 * 호출 체인 (이 트리에서 확인한 직접 호출자):
 *   blk_mq_update_nr_requests / __blk_mq_update_nr_hw_queues (같은 파일)
 *     → [blk_mq_freeze_queue_nomemsave]
 *   blk_mq_freeze_queue (inline 래퍼, include/linux/blk-mq.h)
 *     → [blk_mq_freeze_queue_nomemsave]
 *
 * NVMe 접점: nvme_update_ns_info_block() 등 네임스페이스 포맷/용량이 바뀌는
 * 경로가 blk_mq_freeze_queue(ns->disk->queue) 로 이 함수에 도달한다. 반면
 * 컨트롤러 리셋 경로(nvme_start_freeze/nvme_wait_freeze/nvme_unfreeze)는
 * *_non_owner 변형을 쓰므로 이 함수를 거치지 않는다.
 */
void blk_mq_freeze_queue_nomemsave(struct request_queue *q)
{
	/* [한국어] percpu_ref_kill 로 새 IO 진입 차단 시작 */
	blk_freeze_queue_start(q);
	/* [한국어] in-flight 가 모두 완료되어 q_usage_counter 가 0이 될 때까지 sleep */
	blk_mq_freeze_queue_wait(q);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_nomemsave);

/*
 * [한국어]
 * __blk_mq_unfreeze_queue - 큐 동결 해제 내부 구현
 *
 * @q:            해제할 request_queue
 * @force_atomic: true 이면 percpu_ref 를 atomic 모드로 강제 전환 후 부활
 * @return:       true 이면 소유자의 마지막 unfreeze (lock 해제 필요)
 *
 * mq_freeze_depth 를 1 감소시킨다. 0이 되면 percpu_ref_resurrect 로
 * q_usage_counter 를 살려서 새로운 IO 진입을 허용하고 freeze_wq 를 깨운다.
 * force_atomic 은 메모리 절약(memcg percpu) 경로에서 atomic fallback 시 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트 (mq_freeze_lock 획득).
 *
 * 호출 체인:
 *   blk_mq_unfreeze_queue_nomemrestore → [__blk_mq_unfreeze_queue]
 *     → percpu_ref_resurrect → wake_up_all(&mq_freeze_wq)
 */
bool __blk_mq_unfreeze_queue(struct request_queue *q, bool force_atomic)
{
	/* [한국어] 소유자 여부를 담을 변수 */
	bool unfreeze;

	/* [한국어] freeze_depth 변경을 직렬화 */
	mutex_lock(&q->mq_freeze_lock);
	/* [한국어] force_atomic: percpu_ref 를 atomic 모드로 전환 후 부활해
	 * 향후 percpu_ref_tryget 이 항상 atomic fast path 를 탈 수 있게 한다. */
	if (force_atomic)
		q->q_usage_counter.data->force_atomic = true;
	/* [한국어] freeze 깊이 감소 — 재진입 freeze 였다면 아직 양수 */
	q->mq_freeze_depth--;
	/* [한국어] 깊이가 음수이면 대칭적 freeze/unfreeze 위반 — 버그 경고 */
	WARN_ON_ONCE(q->mq_freeze_depth < 0);
	/* [한국어] 깊이가 0이 됐으면 완전히 unfreeze: q_usage_counter 부활 */
	if (!q->mq_freeze_depth) {
		/* [한국어] percpu_ref_resurrect: dying 상태에서 복귀.
		 * 이후 blk_queue_enter 에서 percpu_ref_tryget 이 성공하여
		 * 새 IO 가 큐에 진입할 수 있게 된다. */
		percpu_ref_resurrect(&q->q_usage_counter);
		/* [한국어] freeze 대기 중이던 모든 웨이터를 깨운다.
		 * 사실 이 시점은 unfreeze 이므로 waiters 가 없어야 정상이지만
		 * race 방지를 위해 깨운다 — 이미 0이었던 counter 에 다시 부활. */
		wake_up_all(&q->mq_freeze_wq);
	}
	/* [한국어] LOCKDEP: 소유자 소멸 여부 확인 */
	unfreeze = blk_unfreeze_check_owner(q);
	/* [한국어] freeze_depth 갱신과 소유자 판정이 끝났으므로 직렬화 뮤텍스 해제.
	 * 이 뮤텍스는 동시 freeze/unfreeze 가 depth 를 어긋나게 만드는 것을 막는다. */
	mutex_unlock(&q->mq_freeze_lock);

	/* [한국어] 소유자의 마지막 unfreeze 이면 true: 호출자가 freeze lock 해제 */
	return unfreeze;
}

/*
 * [한국어]
 * blk_mq_unfreeze_queue_nomemrestore - 메모리 복원 없는 큐 동결 해제 (공개 API)
 *
 * @q: 해제할 request_queue
 *
 * __blk_mq_unfreeze_queue(q, false) 를 호출하여 freeze_depth 를 감소시키고,
 * 소유자의 마지막 unfreeze 이면 freeze lock 을 해제한다.
 * blk_mq_freeze_queue_nomemsave 에 대응하는 해제 함수.
 *
 * 호출 체인 (이 트리에서 확인한 직접 호출자):
 *   blk_mq_unfreeze_queue (inline 래퍼, include/linux/blk-mq.h — memalloc_noio_restore 후)
 *   blk_post_runtime_resume (block/blk-pm.c)
 *   elevator_change (block/elevator.c)
 *   nvme_mpath_unfreeze (drivers/nvme/host/multipath.c)
 *     → [blk_mq_unfreeze_queue_nomemrestore]
 *
 * NVMe 접점: 멀티패스 head 큐를 nvme_mpath_start_freeze 로 얼려 두었다가
 * 경로 갱신이 끝나면 nvme_mpath_unfreeze 가 이 함수로 되돌린다.
 */
void blk_mq_unfreeze_queue_nomemrestore(struct request_queue *q)
{
	/* [한국어] unfreeze 수행; 소유자 마지막이면 freeze lock 해제 */
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
/*
 * [한국어]
 * blk_freeze_queue_start_non_owner - 비소유자 큐 동결 시작
 *
 * @q: 동결할 request_queue
 *
 * owner=NULL 로 __blk_freeze_queue_start 를 호출한다. 소유권 추적이 없으므로
 * 어느 태스크에서도 unfreeze 할 수 있다. 단, 비대칭 사용이 가능해 취약하다.
 * 레거시 SCSI 드라이버(mpt3sas 등) 의 내부 block 경로에서 사용된다.
 *
 * 호출 체인:
 *   scsi_internal_device_block_nowait → [blk_freeze_queue_start_non_owner]
 */
void blk_freeze_queue_start_non_owner(struct request_queue *q)
{
	/* [한국어] owner=NULL → blk_freeze_set_owner 가 즉시 false 반환 (소유권 없음) */
	__blk_freeze_queue_start(q, NULL);
}
EXPORT_SYMBOL_GPL(blk_freeze_queue_start_non_owner);

/* non_owner variant of blk_mq_unfreeze_queue */
/*
 * [한국어]
 * blk_mq_unfreeze_queue_non_owner - 비소유자 큐 동결 해제
 *
 * @q: 해제할 request_queue
 *
 * blk_freeze_queue_start_non_owner 에 대응. force_atomic=false 로 내부 unfreeze.
 * 소유권 없이 freeze 했으므로 반환값은 항상 false.
 *
 * 호출 체인:
 *   scsi_internal_device_unblock_nowait → [blk_mq_unfreeze_queue_non_owner]
 */
void blk_mq_unfreeze_queue_non_owner(struct request_queue *q)
{
	/* [한국어] 소유권 없는 unfreeze: freeze_depth 감소 후 반환값 무시 */
	__blk_mq_unfreeze_queue(q, false);
}
EXPORT_SYMBOL_GPL(blk_mq_unfreeze_queue_non_owner);

/*
 * FIXME: replace the scsi_internal_device_*block_nowait() calls in the
 * mpt3sas driver such that this function can be removed.
 */
/*
 * [한국어]
 * blk_mq_quiesce_queue_nowait - 큐를 즉시 quiesce 시작 (대기 없음)
 *
 * @q: quiesce 할 request_queue
 *
 * QUEUE_FLAG_QUIESCED 플래그를 설정하여 이후 blk_mq_dispatch_rq_list 등에서
 * dispatch 를 중단하게 한다. in-flight 완료를 기다리지 않는다.
 * quiesce_depth 로 재진입 횟수를 추적한다 (unquiesce 와 대칭).
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 인터럽트 비활성 시 (irqsave 락).
 *
 * 호출 체인 (이 트리에서 확인한 직접 호출자):
 *   blk_mq_quiesce_queue  → [blk_mq_quiesce_queue_nowait]
 *   blk_mq_quiesce_tagset → [blk_mq_quiesce_queue_nowait]  (큐마다 플래그만 세우고
 *                            grace period 는 tagset 단위로 한 번만 대기)
 * NVMe 드라이버에서 이 함수를 직접 부르는 곳은 없다 — 항상 위 두 래퍼를 거친다.
 */
void blk_mq_quiesce_queue_nowait(struct request_queue *q)
{
	/* [한국어] queue_lock 은 QUEUE_FLAG_* 조작을 IRQ safe 하게 보호 */
	unsigned long flags;

	/* [한국어] spin_lock_irqsave: 인터럽트를 비활성화하면서 큐 락 획득 */
	spin_lock_irqsave(&q->queue_lock, flags);
	/* [한국어] quiesce_depth 가 0→1 로 처음 증가할 때만 QUIESCED 플래그 세팅.
	 * 재진입 시에는 플래그 중복 set 없이 깊이만 증가한다. */
	if (!q->quiesce_depth++)
		/* [한국어] QUEUE_FLAG_QUIESCED: dispatch loop 에서 이 플래그를 보고
		 * blk_mq_dispatch_rq_list 진입을 건너뛴다 → SQ 에 새 명령 투입 중단. */
		blk_queue_flag_set(QUEUE_FLAG_QUIESCED, q);
	/* [한국어] spin_unlock_irqrestore: 인터럽트를 복원하며 락 해제 */
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
/*
 * [한국어]
 * blk_mq_wait_quiesce_done - 진행 중인 dispatch 가 모두 끝날 때까지 대기
 *
 * @set: 대기할 blk_mq_tag_set
 *
 * QUEUE_FLAG_QUIESCED 플래그를 세팅한 후에도 현재 dispatch 중인 RCU read-side
 * 섹션이 완전히 종료될 때까지 기다린다. blk_mq_dispatch_rq_list 등은
 * rcu_read_lock 내에서 hctx/tags 에 접근하므로, grace period 가 끝나면
 * 이후에는 새로운 dispatch 가 일어나지 않음이 보장된다.
 * BLK_MQ_F_BLOCKING: 큐가 sleep 가능 컨텍스트에서 접근되므로 SRCU 를 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트 (synchronize_rcu 는 sleep 가능).
 *
 * 호출 체인:
 *   blk_mq_quiesce_queue / blk_mq_quiesce_tagset → [blk_mq_wait_quiesce_done]
 */
void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set)
{
	/* [한국어] BLK_MQ_F_BLOCKING: dm 등 sleep 허용 드라이버는 SRCU grace period 사용.
	 * SRCU 는 RCU 보다 더 긴 critical section 허용 (sleep 가능 섹션 포함). */
	if (set->flags & BLK_MQ_F_BLOCKING)
		/* [한국어] 잠들 수 있는 드라이버(NVMe over TCP 등)는 dispatch 를 SRCU 로
		 * 보호하므로, 진행 중인 dispatch 가 끝났음을 SRCU grace period 로 확인한다. */
		synchronize_srcu(set->srcu);
	else	/* [한국어] 비블로킹 드라이버는 dispatch 를 일반 RCU 로 보호하므로 여기로 온다 */
		/* [한국어] synchronize_rcu(): 모든 CPU 가 현재 RCU read-side critical section 을
		 * 빠져나올 때까지 대기. 이후 dispatch_rq_list 내 rcu_read_lock 섹션이 모두 완료. */
		synchronize_rcu();
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
/*
 * [한국어]
 * blk_mq_quiesce_queue - dispatch 를 중단하고 진행 중 dispatch 가 끝날 때까지 대기
 *
 * @q: quiesce 할 request_queue
 *
 * 두 단계로 작동한다:
 * 1) blk_mq_quiesce_queue_nowait: QUEUE_FLAG_QUIESCED 플래그 설정 → 새 dispatch 차단
 * 2) blk_mq_wait_quiesce_done: RCU/SRCU grace period 대기 → 이미 시작된 dispatch 완료
 * 이 함수 반환 후에는 dispatch 가 없음이 보장된다. end_io 는 여전히 호출될 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인 (이 트리에서 확인한 직접 호출자):
 *   blk_mq_update_nr_requests (같은 파일), elevator_change (block/elevator.c),
 *   wbt/iocost/throttle 의 정책 변경 경로, queue sysfs store
 *     → [blk_mq_quiesce_queue]
 *   nvme_quiesce_admin_queue (drivers/nvme/host/core.c) → [blk_mq_quiesce_queue]
 *
 * NVMe 접점: 컨트롤러 리셋/셧다운 중 admin 큐를 멈출 때
 * nvme_quiesce_admin_queue() 가 NVME_CTRL_ADMIN_Q_STOPPED 비트를 세운 뒤
 * 이 함수를 호출한다. I/O 큐 쪽은 큐가 여러 개(네임스페이스마다 하나)이므로
 * 개별 호출 대신 blk_mq_quiesce_tagset() 을 쓴다(nvme_quiesce_io_queues).
 */
void blk_mq_quiesce_queue(struct request_queue *q)
{
	/* [한국어] QUEUE_FLAG_QUIESCED 설정: 이후 dispatch_rq_list 가 quiesce 를 보고 중단 */
	blk_mq_quiesce_queue_nowait(q);
	/* nothing to wait for non-mq queues */
	/* [한국어] MQ 큐이면 RCU/SRCU grace period 대기로 현재 dispatch 완료를 보장 */
	if (queue_is_mq(q))
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
/*
 * [한국어]
 * blk_mq_unquiesce_queue - quiesced 큐를 다시 활성화하고 pending request dispatch
 *
 * @q: 재개할 request_queue
 *
 * quiesce_depth 를 감소시키고, 0이 되면 QUEUE_FLAG_QUIESCED 를 해제하여
 * dispatch 를 재개한다. quiesce 동안 쌓여 있던 request 들을 blk_mq_run_hw_queues 로
 * 즉시 드라이버에 투입한다. end_io 는 quiesce 중에도 계속 호출될 수 있었으므로
 * 재개 시 추가 request 가 없을 수도 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인 (이 트리에서 확인한 직접 호출자):
 *   blk_mq_update_nr_requests (같은 파일), elevator_change, wbt/iocost/throttle
 *   nvme_unquiesce_admin_queue (drivers/nvme/host/core.c)
 *     → [blk_mq_unquiesce_queue] → blk_mq_run_hw_queues
 */
void blk_mq_unquiesce_queue(struct request_queue *q)
{
	unsigned long flags;
	/* [한국어] quiesce_depth 가 0이 될 때 실제 run 이 필요한지를 기록 */
	bool run_queue = false;

	/* [한국어] queue_lock: QUEUE_FLAG_* 조작의 IRQ safe 보호 */
	spin_lock_irqsave(&q->queue_lock, flags);
	/* [한국어] quiesce_depth 가 0 이하이면 unquiesce 과잉 호출 — 버그 경고 */
	if (WARN_ON_ONCE(q->quiesce_depth <= 0)) {
		/* [한국어] 아무것도 하지 않는다 — 이미 경고를 찍었고, depth 를 음수로
		 * 떨어뜨리면 이후 quiesce 가 영영 균형을 못 맞추기 때문이다. */
		;
	/* [한국어] 전위 감소로 깊이를 하나 내리고, 그 결과가 0 일 때만 실제로 푼다.
	 * 중첩 quiesce 는 가장 바깥 것이 풀릴 때만 dispatch 를 재개해야 한다. */
	} else if (!--q->quiesce_depth) {
		/* [한국어] 깊이가 0이 됐을 때만 QUIESCED 플래그를 해제. */
		blk_queue_flag_clear(QUEUE_FLAG_QUIESCED, q);
		/* [한국어] 플래그 해제 직후 run_hw_queues 를 예약해야 함을 표시 */
		run_queue = true;
	}
	spin_unlock_irqrestore(&q->queue_lock, flags);	/* [한국어] quiesce_depth 갱신과 QUEUE_FLAG_QUIESCED 조작이 끝났으므로 IRQ 상태를 복원하며 락 해제 */

	/* dispatch requests which are inserted during quiescing */
	/* [한국어] QUEUE_FLAG_QUIESCED 가 해제된 뒤 run_hw_queues 호출.
	 * async=true 로 전달해 work_queue 에서 dispatch 실행 → 현재 컨텍스트를 블록 안 함. */
	if (run_queue)
		blk_mq_run_hw_queues(q, true);
}
EXPORT_SYMBOL_GPL(blk_mq_unquiesce_queue);

/*
 * [한국어]
 * blk_mq_quiesce_tagset - tag_set 에 속한 모든 큐를 quiesce
 *
 * @set: quiesce 할 blk_mq_tag_set
 *
 * tag_set->tag_list 에 연결된 모든 request_queue 에 대해 quiesce_nowait 를 호출하고,
 * 이후 RCU/SRCU grace period 로 진행 중인 dispatch 가 모두 완료될 때까지 대기한다.
 * 하나의 tag_set 을 여러 request_queue 가 공유할 때 쓰인다. NVMe PCIe 에서는
 * 컨트롤러 하나의 I/O tagset(nvme_ctrl->tagset)에 그 컨트롤러의 모든
 * 네임스페이스 큐(/dev/nvme0n1, n2, ...)가 매달려 있으므로, 리셋/셧다운 시
 * 이 함수 한 번으로 전부 멈춘다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (grace period 대기 때문에 sleep 가능).
 *
 * 호출 체인 (이 트리에서 확인한 실제 호출자):
 *   nvme_quiesce_io_queues (drivers/nvme/host/core.c)
 *     — NVME_CTRL_STOPPED 비트를 test_and_set 으로 세운 경우에만 호출하고,
 *       이미 세워져 있으면 blk_mq_wait_quiesce_done() 만 부른다(이중 quiesce 안전).
 *     → [blk_mq_quiesce_tagset]
 */
void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	/* [한국어] RCU read lock: tag_list 는 RCU 로 보호되므로 rcu_read_lock 필요 */
	rcu_read_lock();
	/* [한국어] list_for_each_entry_rcu: tag_set 에 속한 모든 request_queue 순회 */
	list_for_each_entry_rcu(q, &set->tag_list, tag_set_list) {
		/* [한국어] blk_queue_skip_tagset_quiesce: 이 큐가 tagset quiesce 에서 제외되어야
		 * 하는지 확인 — 일부 큐는 quiesce 를 건너뛸 수 있음 */
		if (!blk_queue_skip_tagset_quiesce(q))
			/* [한국어] 각 큐에 대해 즉시 QUIESCED 플래그 설정 */
			blk_mq_quiesce_queue_nowait(q);
	}
	/* [한국어] 순회 종료 — tag_list 를 더 이상 읽지 않으므로 RCU 읽기 구간을 닫는다.
	 * 여기서 닫아야 아래의 grace period 대기가 스스로를 기다리는 교착이 되지 않는다. */
	rcu_read_unlock();

	/* [한국어] 모든 큐의 QUIESCED 플래그 설정 후 한 번의 grace period 대기로
	 * 진행 중인 모든 dispatch 완료를 보장한다 (set 단위 한 번으로 효율적). */
	blk_mq_wait_quiesce_done(set);
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_tagset);

/*
 * [한국어]
 * blk_mq_unquiesce_tagset - tag_set 에 속한 모든 큐를 unquiesce
 *
 * @set: unquiesce 할 blk_mq_tag_set
 *
 * blk_mq_quiesce_tagset 의 반대 동작. 각 큐의 unquiesce 는 내부에서
 * blk_mq_run_hw_queues 를 호출해 pending request 를 dispatch 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인 (이 트리에서 확인한 실제 호출자):
 *   nvme_unquiesce_io_queues (drivers/nvme/host/core.c)
 *     — NVME_CTRL_STOPPED 를 test_and_clear 로 내린 경우에만 호출.
 *     → [blk_mq_unquiesce_tagset]
 */
void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	/* [한국어] tag_list RCU 순회 보호 */
	rcu_read_lock();
	list_for_each_entry_rcu(q, &set->tag_list, tag_set_list) {	/* [한국어] tag_set 에 매달린 모든 request_queue 를 RCU 안전하게 순회 — 컨트롤러 하나의 전 네임스페이스 큐가 여기 해당 */
		/* [한국어] 각 큐의 quiesce_depth 감소 및 QUIESCED 플래그 해제 */
		if (!blk_queue_skip_tagset_quiesce(q))
			blk_mq_unquiesce_queue(q);
	}
	/* [한국어] 순회 종료 — RCU 읽기 구간 해제. quiesce 쪽과 달리 여기서는
	 * grace period 대기가 없다(재개는 기다릴 것이 없다). */
	rcu_read_unlock();
}
/* [한국어] NVMe 등 트랜스포트 드라이버가 모듈에서 쓰므로 GPL 심볼로 공개.
 * nvme_unquiesce_io_queues() 가 이 심볼을 호출한다. */
EXPORT_SYMBOL_GPL(blk_mq_unquiesce_tagset);

/*
 * [한국어]
 * blk_mq_wake_waiters - tag 고갈로 잠든 submitter 들을 모두 깨움
 *
 * @q: wakeup 대상 request_queue
 *
 * 모든 hctx(하드웨어 큐) 에서 tag 가 반납되면 sleep 중인 blk_mq_alloc_request
 * 대기자들이 있을 수 있다. 이 함수는 매핑된 모든 hctx 의 wq 를 깨워
 * 새로운 tag 를 얻으려는 submitter 가 재시도할 수 있게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 완료 softirq.
 *
 * 호출 체인:
 *   blk_mq_unfreeze_queue / blk_mq_unquiesce_queue → [blk_mq_wake_waiters]
 *     → blk_mq_tag_wakeup_all (hctx 별 waitqueue 모두 wake)
 */
void blk_mq_wake_waiters(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] queue_for_each_hw_ctx 루프 인덱스 */
	unsigned long i;

	/* [한국어] queue_for_each_hw_ctx: q 의 모든 하드웨어 큐를 순회
	 * blk_mq_hw_queue_mapped: CPU 가 매핑된 활성 hctx 인지 확인
	 * 미매핑 hctx(핫플러그 제거된 CPU 담당)는 건너뜀 */
	queue_for_each_hw_ctx(q, hctx, i)
		if (blk_mq_hw_queue_mapped(hctx))
			/* [한국어] blk_mq_tag_wakeup_all(tags, true): hctx 의 태그 웨이트큐에서
			 * 대기 중인 모든 submitter 를 깨운다. true 는 reserved tag 대기자도 포함. */
			blk_mq_tag_wakeup_all(hctx->tags, true);
}

/*
 * [한국어]
 * blk_rq_init - struct request 를 기본 초기 상태로 설정
 *
 * @q:  이 request 가 속할 request_queue
 * @rq: 초기화할 request 포인터 (blk_mq_alloc_rqs 의 사전 할당 슬롯)
 *
 * NVMe 관점: request 하나가 곧 인플라이트 명령 하나다. 흔한 오해와 달리
 * request 가 SQ 링의 특정 슬롯에 고정되는 것은 아니다 — SQE 가 놓이는 자리는
 * nvme_sq_copy_cmd() 가 sq_tail(링 생산자 인덱스)로 정하며 tag 와 무관하다.
 * tag 는 "동시에 몇 개까지"를, sq_tail 은 "이번 것을 어디에"를 정한다. 이 함수는
 * request 를 드라이버에 전달하기 전 클린 상태로 만드는 역할을 한다.
 * tag 는 BLK_MQ_NO_TAG(-1)로 초기화되고, 이후 blk_mq_rq_ctx_init 에서
 * blk_mq_get_tag() 가 실제 tag(=CID)를 할당한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (request 풀 초기화 또는 request 재사용).
 *
 * 호출 체인:
 *   blk_mq_alloc_rqs (풀 초기화) → [blk_rq_init]
 *   blk_mq_rq_ctx_init (매 request 발행 전) → [blk_rq_init] (일부 필드만 재초기화)
 */
void blk_rq_init(struct request_queue *q, struct request *rq)
{
	/* [한국어] memset(0): request 구조체 전체를 0으로 클리어.
	 * 이전 I/O 의 잔재 데이터가 남지 않도록 한다. */
	memset(rq, 0, sizeof(*rq));

	/* [한국어] INIT_LIST_HEAD: queuelist 를 자기 자신 가리키도록 초기화.
	 * dispatch list, plug list 등에 연결되기 전 안전한 초기 상태. */
	INIT_LIST_HEAD(&rq->queuelist);
	/* [한국어] rq->q: 이 request 가 발행될 큐를 역참조용으로 기록 */
	rq->q = q;
	/* [한국어] __sector = -1: 아직 유효한 LBA 가 지정되지 않은 상태.
	 * blk_mq_bio_to_request 에서 bio->bi_iter.bi_sector 로 채워진다. */
	rq->__sector = (sector_t) -1;
	/* [한국어] phys_gap_bit: 물리 연속성 gap 비트 초기화 (merge 시 사용) */
	rq->phys_gap_bit = 0;
	/* [한국어] INIT_HLIST_NODE(&rq->hash): elevator 가 back-merge 탐색에 쓰는
	 * 해시 노드 초기화 — elevator.c 의 hash table 연결 전 안전한 상태. */
	INIT_HLIST_NODE(&rq->hash);
	/* [한국어] RB_CLEAR_NODE(&rq->rb_node): elevator 의 LBA 정렬 RB-tree 연결 전 초기화 */
	RB_CLEAR_NODE(&rq->rb_node);
	/* [한국어] tag = BLK_MQ_NO_TAG(-1): 아직 드라이버 태그를 얻지 못한 상태.
	 * blk_mq_get_tag() 호출 후 실제 tag 번호로 바뀐다. */
	rq->tag = BLK_MQ_NO_TAG;
	/* [한국어] internal_tag: IO 스케줄러 전용 태그 (scheduler 없으면 BLK_MQ_NO_TAG).
	 * BFQ/mq-deadline 은 driver tag 와 별도로 scheduler 내부 순서를 관리한다. */
	rq->internal_tag = BLK_MQ_NO_TAG;
	/* [한국어] start_time_ns: request 초기화 시각 — blk_account_io_done 에서
	 * IO 지연 계산의 기준이 된다 (blk_time_get_ns 는 ktime_get_ns 래퍼). */
	rq->start_time_ns = blk_time_get_ns();
	/* [한국어] blk_crypto_rq_set_defaults: 인라인 암호화 컨텍스트 초기화.
	 * NVMe inline encryption 이 비활성화면 no-op. */
	blk_crypto_rq_set_defaults(rq);
}
EXPORT_SYMBOL(blk_rq_init);

/* Set start and alloc time when the allocated request is actually used */
/*
 * [한국어]
 * blk_mq_rq_time_init - request 의 alloc_time_ns 를 설정 (CONFIG_BLK_RQ_ALLOC_TIME 시)
 *
 * @rq:           시각을 기록할 request
 * @alloc_time_ns: tag 를 할당한 시점의 타임스탬프 (ns)
 *
 * BLK_RQ_ALLOC_TIME 가 활성화된 빌드에서만 유효하다. alloc_time_ns 는
 * request 가 tag 를 얻은 시점부터 드라이버에 전달될 때까지의 지연을
 * blk_account_io_done 에서 계산하는 데 쓰인다 (allocation latency 관찰).
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_rq_ctx_init 내부).
 *
 * 호출 체인:
 *   blk_mq_rq_ctx_init → [blk_mq_rq_time_init]
 */
static inline void blk_mq_rq_time_init(struct request *rq, u64 alloc_time_ns)
{
#ifdef CONFIG_BLK_RQ_ALLOC_TIME
	/* [한국어] QUEUE_FLAG_RQ_ALLOC_TIME: 이 큐가 request alloc time 을 기록하도록
	 * 설정된 경우에만 alloc_time_ns 를 저장한다. blktrace/io.stat 분석용. */
	if (blk_queue_rq_alloc_time(rq->q))
		/* [한국어] alloc_time_ns 저장: tag 할당 시점 — complete 시 이 값 빼서 지연 계산 */
		rq->alloc_time_ns = alloc_time_ns;
	else	/* [한국어] 측정이 꺼진 빌드/큐에서는 쓰레기 값이 남지 않도록 명시적으로 0 을 넣는다 */
		/* [한국어] 측정 비활성 시 0으로 초기화 */
		rq->alloc_time_ns = 0;
#endif
}

/*
 * [한국어]
 * blk_mq_bio_issue_init - bio 의 issue_time_ns 를 현재 시각으로 기록
 *
 * @q:   bio 가 발행될 request_queue
 * @bio: 시각을 기록할 bio
 *
 * CONFIG_BLK_CGROUP 활성 + QUEUE_FLAG_BIO_ISSUE_TIME 설정 시에만 동작한다.
 * blkcg 통계에서 bio 발행부터 완료까지의 지연을 측정하는 데 쓰인다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_submit_bio 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_get_new_requests → [blk_mq_bio_issue_init]
 */
static inline void blk_mq_bio_issue_init(struct request_queue *q,
					 struct bio *bio)
{
#ifdef CONFIG_BLK_CGROUP
	/* [한국어] QUEUE_FLAG_BIO_ISSUE_TIME: blkcg 가 bio 지연 측정을 요청한 경우.
	 * bio->issue_time_ns 는 bio_issue_time 관련 blkcg 통계 함수에서 참조된다. */
	if (test_bit(QUEUE_FLAG_BIO_ISSUE_TIME, &q->queue_flags))
		/* [한국어] blk_time_get_ns(): 고해상도 타이머 기반 현재 시각 (ns) */
		bio->issue_time_ns = blk_time_get_ns();
#endif
}

/*
 * [한국어]
 * blk_mq_rq_ctx_init - 할당된 tag 를 바탕으로 request 를 초기화 (내부 구현)
 *
 * @data: request 할당에 필요한 컨텍스트 (큐, hctx, ctx, cmd_flags, rq_flags)
 * @tags: 이 hctx 에서 사용하는 blk_mq_tags (driver tag 풀)
 * @tag:  할당받은 태그 번호 (0 ~ queue_depth-1). NVMe 에서는 이 값이
 *        CID 의 하위 12비트가 된다
 * @return: 초기화 완료된 struct request 포인터
 *
 * tags->static_rqs[tag] 에서 사전 할당된 request 슬롯을 가져와
 * 현재 hctx(SQ), ctx(CPU sw 큐), cmd_flags(IO 타입), rq_flags,
 * tag 등을 기록한다. scheduler 사용 시 prepare_request 콜백도 호출.
 * NVMe 관점: nvme_setup_cmd() 가 cmd->common.command_id = nvme_cid(req) 로
 * 이 값을 SQE 의 CID 필드에 심는다(하위 12비트 = rq->tag, 상위 4비트 = genctr).
 * 완료 시 nvme_find_rq() 가 CQE 의 command_id 를 다시 태그로 분해해
 * blk_mq_tag_to_rq(tags, tag) 로 이 request 를 되찾는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (request 발행 경로).
 *
 * 호출 체인:
 *   __blk_mq_alloc_requests / __blk_mq_alloc_requests_batch
 *     → [blk_mq_rq_ctx_init] → rq 반환 → blk_mq_submit_bio
 */
static struct request *blk_mq_rq_ctx_init(struct blk_mq_alloc_data *data,
		struct blk_mq_tags *tags, unsigned int tag)
{
	/* [한국어] 할당 요청 시의 CPU sw 큐 — rq 에 기록해 통계/추적에 사용 */
	struct blk_mq_ctx *ctx = data->ctx;
	/* [한국어] 할당 요청 시의 hw 큐 (SQ) — rq->mq_hctx 에 기록 */
	struct blk_mq_hw_ctx *hctx = data->hctx;
	/* [한국어] request 가 속할 request_queue */
	struct request_queue *q = data->q;
	/* [한국어] tag 번호로 사전 할당된 request 슬롯을 찾는다.
	 * static_rqs 는 blk_mq_alloc_rqs 에서 한 번만 kmalloc 된 고정 배열이다. */
	struct request *rq = tags->static_rqs[tag];

	/* [한국어] rq->q: 나중에 end_io, 통계, freeze 확인 등에서 역참조 */
	rq->q = q;
	/* [한국어] rq->mq_ctx: submit 한 CPU 의 sw 큐 포인터 */
	rq->mq_ctx = ctx; /* 제출 CPU 와 매핑된 software queue */
	/* [한국어] rq->mq_hctx: 이 request 를 처리할 SQ(hw 큐) 포인터 */
	rq->mq_hctx = hctx; /* NVMe SQ 에 해당하는 hardware queue */
	/* [한국어] cmd_flags: REQ_OP_READ/WRITE/FLUSH 등 IO 타입 + 속성 비트.
	 * nvme_queue_rq 에서 NVMe 명령 opcode 로 변환된다. */
	rq->cmd_flags = data->cmd_flags; /* NVMe opcode/flags 의 기초 */

	/* [한국어] BLK_MQ_REQ_PM: 전원 관리(runtime PM) 자체를 위한 request 라는 표시.
	 * 이 플래그가 붙으면 RQF_PM 이 서고, blk_queue_enter()/blk-pm 이 "큐가
	 * suspend 중이라도 이 요청만은 통과시킨다"고 판단한다. 없으면 suspend 중인
	 * 큐에서 잠들어 버려, 큐를 깨우려는 명령 자체가 교착에 빠진다. */
	if (data->flags & BLK_MQ_REQ_PM)
		data->rq_flags |= RQF_PM;
	/* [한국어] 누적된 rq_flags를 확정한다. RQF_IO_STAT(통계 집계 대상),
	 * RQF_USE_SCHED(스케줄러 경유), RQF_PM(전원 관리 명령이라 큐가 suspend
	 * 상태여도 통과), RQF_SCHED_TAGS, RQF_RESV 등이 여기 담긴다. 이후
	 * 제출·완료 경로의 거의 모든 분기가 이 필드를 본다. */
	rq->rq_flags = data->rq_flags;

	/* [한국어] ★ 태그 이중 구조 ★
	 * blk-mq는 두 종류의 태그를 쓴다:
	 *   internal_tag(스케줄러 태그) - 큐에 담아 두기 위한 논리적 슬롯.
	 *     드라이버 태그보다 넉넉하게(보통 2배) 잡아 스케줄러가 재정렬할
	 *     여지를 만든다. 하드웨어와는 무관한 번호다.
	 *   tag(드라이버 태그)          - 실제 하드웨어 슬롯. NVMe에서는 이 번호가
	 *     커맨드 CID(Command Identifier)의 하위 12비트가 되어(상위 4비트는
	 *     드라이버가 붙이는 세대 카운터), SQ에 실린 커맨드와 CQ로 돌아온 완료를
	 *     짝짓는다. 자세한 관계는 blk_mq_rq_cache_fill() 위 주석 참고.
	 * 스케줄러가 있으면 지금은 internal_tag만 잡고, 드라이버 태그는 실제
	 * dispatch 직전에 따로 획득한다. 그래야 귀한 하드웨어 슬롯이 큐에서
	 * 대기하는 동안 낭비되지 않는다. */
	if (data->rq_flags & RQF_SCHED_TAGS) {
		/* [한국어] 드라이버 태그는 아직 없음 — dispatch 시점에 채워진다. */
		rq->tag = BLK_MQ_NO_TAG;
		/* [한국어] 방금 획득한 것은 스케줄러 태그다. */
		rq->internal_tag = tag;
	} else {
		/* [한국어] 스케줄러가 없으면(NVMe 기본 "none") 처음부터 드라이버 태그를
		 * 잡는다. 이 번호가 NVMe CID 의 하위 12비트가 된다. 스케줄러가 없다는 것은
		 * "큐에서 대기시킬 이유가 없다"는 뜻이므로, 논리 슬롯과 하드웨어 슬롯을
		 * 나눌 필요 없이 처음부터 하드웨어 슬롯을 잡아 한 단계를 줄인다. */
		rq->tag = tag;
		/* [한국어] 스케줄러 태그는 쓰지 않으므로 무효값으로 둔다. */
		rq->internal_tag = BLK_MQ_NO_TAG;
	}
	/* [한국어] 타임아웃 값 0 = "큐 기본값(q->rq_timeout)을 쓰라". 드라이버가
	 * 특정 명령에만 다른 타임아웃을 주고 싶으면 제출 전에 덮어쓴다. NVMe는
	 * admin 명령과 I/O 명령에 서로 다른 타임아웃(admin_timeout / io_timeout
	 * 모듈 파라미터)을 적용한다. */
	rq->timeout = 0;

	/* [한국어] 통계를 누적할 파티션(block_device). 아직 미정이며
	 * blk_account_io_start()가 bio의 bi_bdev를 보고 채운다. */
	rq->part = NULL;
	/* [한국어] I/O가 실제로 장치에 발행된 시각. blk_mq_start_request()가
	 * 채우며, start_time_ns(할당 시각)와 구분된다 — 둘의 차이가 "큐에서
	 * 대기한 시간"이다. */
	rq->io_start_time_ns = 0;
	/* [한국어] 통계용 섹터 수 스냅숏. 부분 완료로 blk_rq_sectors()가 줄어들어도
	 * 원래 크기를 알 수 있도록 blk_mq_start_request()가 따로 보관한다. */
	rq->stats_sectors = 0;
	/* [한국어] 물리 세그먼트 수 — NVMe PRP 엔트리 또는 SGL 디스크립터 개수의
	 * 근거가 된다. bio가 붙을 때 blk_mq_bio_to_request()가 채운다. */
	rq->nr_phys_segments = 0;
	/* [한국어] 무결성(PI) 메타데이터용 세그먼트 수. NVMe에서 메타데이터는
	 * 데이터와 별도의 포인터(MPTR 또는 메타데이터 SGL)로 전달되므로
	 * 카운터가 분리되어 있다. */
	rq->nr_integrity_segments = 0;
	/* [한국어] 완료 콜백. passthrough나 flush 상태 기계가 자기 후처리를
	 * 등록하며, 일반 bio 기반 I/O에서는 NULL로 남아 bio_endio 경로를 탄다. */
	rq->end_io = NULL;
	/* [한국어] 완료 콜백에 넘길 사용자 데이터 포인터. */
	rq->end_io_data = NULL;

	/* [한국어] 인라인 암호화 관련 필드(키, DUN, keyslot)를 기본값으로 초기화한다.
	 * 재사용된 request에 이전 I/O의 키가 남아 있으면 잘못된 키로 암복호하게
	 * 되므로 반드시 필요하다. */
	blk_crypto_rq_set_defaults(rq);
	/* [한국어] 큐 삽입용 리스트 헤드를 자기 자신을 가리키는 빈 상태로 만든다.
	 * 재사용 시 이전 리스트의 잔재가 남아 있으면 삽입할 때 리스트가 꼬인다. */
	INIT_LIST_HEAD(&rq->queuelist);
	/* tag was already set */
	/* [한국어] deadline 0: timeout 타이머 재설정 전 초기 상태 */
	WRITE_ONCE(rq->deadline, 0);
	/* [한국어] req_ref_set(1): NVMe 명령 생명주기 참조 카운트 시작 */
	req_ref_set(rq, 1);

	/* [한국어] IO 스케줄러 경로 request: hash 와 rb_node 를 초기화해야
	 * elevator 의 back-merge 해시/RB-tree 에 안전하게 연결될 수 있다. */
	if (rq->rq_flags & RQF_USE_SCHED) {
		/* [한국어] elevator_queue: 이 큐의 IO 스케줄러 (mq-deadline, BFQ 등) */
		struct elevator_queue *e = data->q->elevator;

		/* [한국어] INIT_HLIST_NODE: elevator back-merge 해시 노드 초기화 */
		INIT_HLIST_NODE(&rq->hash);
		/* [한국어] RB_CLEAR_NODE: elevator LBA-정렬 RB-tree 노드 초기화 */
		RB_CLEAR_NODE(&rq->rb_node);

		/* [한국어] prepare_request: 스케줄러 내부 상태 초기화 콜백.
		 * mq-deadline: dl_rq.fifo_time 등 초기화.
		 * BFQ: rq 와 bfqq 연결 (bfq_prepare_request). */
		if (e->type->ops.prepare_request)
			e->type->ops.prepare_request(rq);
	}

	/* [한국어] 완전히 초기화된 request 반환 — 호출자(alloc_requests/batch)가 사용 */
	return rq;
}

/*
 * [한국어]
 * __blk_mq_alloc_requests_batch - 여러 tag 를 한 번에 배치 할당
 *
 * @data: 할당 컨텍스트 (nr_tags, hctx, q, rq_flags, cached_rqs 등)
 * @return: 배치 중 첫 번째로 초기화된 request; 고갈 시 NULL
 *
 * blk_mq_get_tags 로 여러 tag 를 한 번에 비트마스크로 획득해
 * 각 tag 마다 blk_mq_rq_ctx_init 으로 초기화한 후 data->cached_rqs 에 쌓는다.
 * 첫 번째 request 는 직접 반환하고 나머지는 plug cache 에 남겨 다음 할당에 재사용.
 * 이 방식은 SQ depth 가 큰 NVMe 에서 batch_size 만큼 tag sbitmap 락을 줄여
 * 제출 CPU 당 오버헤드를 감소시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_submit_bio 경로).
 *
 * 호출 체인:
 *   blk_mq_get_new_requests → __blk_mq_alloc_requests → [__blk_mq_alloc_requests_batch]
 *     → blk_mq_get_tags (sbitmap batch acquire) → blk_mq_rq_ctx_init
 */
static inline struct request *
__blk_mq_alloc_requests_batch(struct blk_mq_alloc_data *data)
{
	/* [한국어] 배치 할당 결과 변수들 */
	/* [한국어] tag = 이번에 처리 중인 태그 번호,
	 * tag_offset = 이번 배치가 시작하는 태그 번호. sbitmap이 연속 구간을
	 * 통째로 잡아 주므로 시작 번호 + 비트 위치로 실제 번호를 구한다. */
	unsigned int tag, tag_offset;
	/* [한국어] 태그 번호로 request 객체를 찾을 수 있는 태그 세트. 스케줄러
	 * 태그인지 드라이버 태그인지에 따라 다른 세트를 가리킨다. */
	struct blk_mq_tags *tags;
	/* [한국어] 루프 안에서 매 태그마다 초기화된 request 를 잠시 담아 두는 임시 포인터.
	 * 곧바로 data->cached_rqs 리스트로 옮겨지므로 루프 밖에서는 의미가 없다. */
	struct request *rq;
	/* [한국어] 한 번의 blk_mq_get_tags() 호출로 얻은 태그들의 비트마스크.
	 * 비트 하나가 태그 하나에 대응하며, unsigned long이므로 64비트 시스템에서
	 * 한 번에 최대 64개까지 잡을 수 있다. */
	unsigned long tag_mask;
	/* [한국어] i = 비트마스크 순회 인덱스, nr = 지금까지 실제로 확보한 태그 수.
	 * nr이 목표(data->nr_tags)에 도달하거나 태그가 고갈될 때까지 반복한다. */
	int i, nr = 0;     /* nr: 이번 배치에서 할당한 총 tag 수 */

	do {
		/* [한국어] blk_mq_get_tags: sbitmap 에서 최대 (nr_tags - nr) 개의
		 * 연속 tag 를 비트마스크로 한 번에 획득 시도. tag_offset 은 시작 태그 번호. */
		tag_mask = blk_mq_get_tags(data, data->nr_tags - nr, &tag_offset);
		if (unlikely(!tag_mask)) {	/* [한국어] 비트마스크가 0 = 이번 시도에서 한 개도 못 잡았다는 뜻 */
			/* [한국어] tag 풀 고갈: 첫 번째 시도에서 실패 시 NULL 반환.
			 * 이미 일부를 얻었으면(nr > 0) 그걸로 계속 진행. */
			if (nr == 0)
				return NULL;
			break;
		}
		/* [한국어] 비트마스크에 해당하는 hctx 의 blk_mq_tags 구조체 */
		tags = blk_mq_tags_from_data(data);
		/* [한국어] 비트마스크를 비트별로 스캔해 각 tag 를 처리 */
		for (i = 0; tag_mask; i++) {
			/* [한국어] 이 비트가 set 되지 않았으면 건너뜀 */
			if (!(tag_mask & (1UL << i)))
				continue;
			/* [한국어] 실제 tag 번호 = 시작 오프셋 + 비트 위치 */
			tag = tag_offset + i;
			/* [한국어] prefetch: CPU 캐시에 static_rqs[tag] 를 미리 적재.
			 * 다음 blk_mq_rq_ctx_init 호출 전에 캐시에 들어와 레이턴시 감소. */
			prefetch(tags->static_rqs[tag]);
			/* [한국어] 처리한 비트를 마스크에서 제거 */
			tag_mask &= ~(1UL << i);
			/* [한국어] tag 에 대응하는 request 를 초기화 */
			rq = blk_mq_rq_ctx_init(data, tags, tag);
			/* [한국어] 초기화된 request 를 plug cached_rqs 앞에 넣는다.
			 * 맨 마지막 rq 가 리스트 맨 앞에 오게 되며 나중에 pop 해서 사용. */
			rq_list_add_head(data->cached_rqs, rq);
			/* [한국어] 이번 배치에서 실제로 확보·초기화한 개수를 하나 올린다.
			 * 루프 종료 조건(data->nr_tags > nr)과 아래의 active_requests /
			 * q_usage_counter 보정량이 모두 이 값을 기준으로 계산된다. */
			nr++;
		}
	} while (data->nr_tags > nr);

	/* [한국어] RQF_SCHED_TAGS 가 없으면 driver tag 사용 — hctx active_requests 증가.
	 * scheduler 사용 시에는 scheduler tag 에서 별도로 추적하므로 여기서 하지 않음. */
	if (!(data->rq_flags & RQF_SCHED_TAGS))
		blk_mq_add_active_requests(data->hctx, nr);
	/* caller already holds a reference, add for remainder */
	/* [한국어] percpu_ref_get_many: q_usage_counter 를 (nr-1) 만큼 추가 증가.
	 * 호출자가 이미 1을 보유 중이므로 나머지 (nr-1) 개를 추가로 참조 획득.
	 * request 가 완료될 때마다 percpu_ref_put 으로 1씩 반환한다. */
	percpu_ref_get_many(&data->q->q_usage_counter, nr - 1);
	/* [한국어] 할당 요청 수에서 실제 할당된 수를 뺀다 (나머지가 아직 필요한 경우) */
	data->nr_tags -= nr;

	/* [한국어] cached_rqs 맨 앞의 request 를 꺼내 직접 반환.
	 * 나머지는 cached_rqs 에 남아 다음 alloc_cached_request 에서 소비된다. */
	return rq_list_pop(data->cached_rqs);
}

/*
 * [한국어]
 * blk_mq_limit_depth - 태그 할당 전에 스케줄러의 깊이 제한 정책을 적용
 *
 * @data: 할당 컨텍스트. rq_flags에 RQF_SCHED_TAGS/RQF_USE_SCHED를 추가하고,
 *        스케줄러가 shallow_depth를 설정할 수 있다.
 * @return: 없음
 *
 * === 왜 깊이를 제한하는가 ===
 * 태그를 선착순으로 나눠 주면 문제가 생긴다. 백그라운드의 대량 비동기 쓰기
 * (페이지 캐시 write-back)가 태그를 전부 점유하면, 사용자가 기다리고 있는
 * 동기 읽기가 태그를 못 얻어 굶는다. 장치는 여전히 바쁘지만 체감 응답성은
 * 무너진다.
 * 그래서 스케줄러는 "비동기 요청은 전체 깊이의 일부까지만"이라는 제한을 둔다.
 * mq-deadline의 dd_limit_depth()가 async 요청에 dd->async_depth를 적용하는 것이
 * 그 예다. 이 제한은 sbitmap의 shallow_depth 기능으로 구현되어, 태그를 잡을 때
 * 비트맵의 앞부분만 쓰도록 강제한다.
 *
 * === 제외되는 요청들 ===
 * flush와 passthrough는 제한 대상이 아니다. 둘 다 "사용자나 파일시스템이
 * 명시적으로 지금 필요하다고 요구한" 명령이라 지연시킬 이유가 없고, 개수도
 * 적어 태그를 독점할 위험이 없다. NVMe의 admin 명령과 nvme-cli passthrough가
 * 여기 해당한다.
 *
 * 실행 컨텍스트: 태그 할당 직전(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   __blk_mq_alloc_requests → [blk_mq_limit_depth]
 *     → blk_mq_tag_busy (스케줄러 없을 때)
 *     → ops->limit_depth (dd_limit_depth / bfq_limit_depth)
 */
static void blk_mq_limit_depth(struct blk_mq_alloc_data *data)
{
	/* [한국어] 스케줄러 콜백 테이블. 스케줄러가 있을 때만 사용한다. */
	struct elevator_mq_ops *ops;

	/* If no I/O scheduler has been configured, don't limit requests */
	/* [한국어] 스케줄러가 없으면(NVMe 기본 "none") 제한할 주체가 없다.
	 * 태그 풀 전체를 선착순으로 쓴다. NVMe에서 이것이 문제가 되지 않는 이유는
	 * 하드웨어 큐가 여러 개라 한 CPU의 대량 쓰기가 다른 CPU의 읽기를
	 * 직접 막지 않고, 태그 수(보통 1024)도 넉넉하기 때문이다. */
	if (!data->q->elevator) {
		/* [한국어] 이 하드웨어 큐가 활성 상태임을 표시한다. 여러 큐가 태그
		 * 풀을 공유하는 구성(BLK_MQ_F_TAG_QUEUE_SHARED)에서, 활성 큐 수로
		 * 나눠 각 큐가 공평한 몫만 쓰도록 하는 근거가 된다. NVMe PCIe는
		 * 큐마다 독립 태그 풀을 가지므로 이 계산이 의미를 갖지 않지만,
		 * 태그를 공유하는 fabrics 구성에서는 중요하다. */
		blk_mq_tag_busy(data->hctx);
		return;
	}

	/*
	 * All requests use scheduler tags when an I/O scheduler is
	 * enabled for the queue.
	 */
	/* [한국어] scheduler 사용 시 모든 request 는 sched tag 를 경유 */
	data->rq_flags |= RQF_SCHED_TAGS;

	/*
	 * Flush/passthrough requests are special and go directly to the
	 * dispatch list, they are not subject to the async_depth limit.
	 */
	/* [한국어] flush와 passthrough는 깊이 제한에서 면제한다(위 함수 주석 참고).
	 * cmd_flags에서 연산 코드만 추출해 비교하는 이유는 상위 비트에 FUA 등
	 * 다른 플래그가 섞여 있기 때문이다. 여기서 반환하면 RQF_USE_SCHED가
	 * 설정되지 않아, 이 요청은 스케줄러를 거치지 않고 hctx->dispatch로
	 * 직행하게 된다. */
	if ((data->cmd_flags & REQ_OP_MASK) == REQ_OP_FLUSH ||
	    blk_op_is_passthrough(data->cmd_flags))
		return;

	/* [한국어] 여기 도달한 요청은 일반 I/O이므로 예약 태그를 요구해서는 안 된다.
	 * 예약 태그는 flush나 복구용 명령을 위한 것인데, 그것들은 위에서 이미
	 * 반환되었기 때문이다. 위반은 호출자의 논리 오류이므로 경고를 남긴다. */
	WARN_ON_ONCE(data->flags & BLK_MQ_REQ_RESERVED);
	/* [한국어] "이 request는 스케줄러를 경유한다"고 표시한다. 이 플래그가 있으면
	 * blk_mq_submit_bio()가 직접 발행 대신 스케줄러 삽입 경로를 택하고,
	 * 완료 시에도 스케줄러에 통지가 간다. */
	data->rq_flags |= RQF_USE_SCHED;

	/*
	 * By default, sync requests have no limit, and async requests are
	 * limited to async_depth.
	 */
	/* [한국어] 스케줄러의 콜백 테이블을 꺼낸다. */
	ops = &data->q->elevator->type->ops;
	/* [한국어] limit_depth 콜백이 있으면 호출해 스케줄러가 data->shallow_depth를
	 * 설정하게 한다. 위 영문 주석이 밝힌 기본 정책은 "동기 요청은 무제한,
	 * 비동기 요청은 async_depth까지"다.
	 *   mq-deadline: dd_limit_depth()가 async 요청에 dd->async_depth 적용.
	 *     그 값은 /sys/.../iosched/async_depth로 조정 가능하다.
	 *   BFQ        : bfq_limit_depth()가 프로세스별 상태까지 반영해 더 정교하게
	 *     제한한다.
	 * 콜백이 없는 스케줄러(kyber 등)는 자체 방식으로 지연을 제어하므로
	 * 태그 단계의 제한이 필요 없다. */
	if (ops->limit_depth)
		ops->limit_depth(data->cmd_flags, data);
}

/*
 * [한국어]
 * __blk_mq_alloc_requests - hctx 와 드라이버 태그를 확보해 request 를 만든다
 *
 * @data: 할당 컨텍스트 — q, flags, cmd_flags, nr_tags, cached_rqs 를 포함
 * @return: 초기화된 request; 고갈 + NOWAIT 시 NULL
 *
 * NVMe 관점: hctx(NVMe PCIe 라면 SQ/CQ 한 쌍)의 sbitmap 에서 빈 tag 를 확보하고
 * blk_mq_rq_ctx_init 으로 request 를 완성한다. 이 함수가 반환한 request 의
 * rq->tag 가 나중에 SQE 의 CID 하위 12비트로 들어간다(상위 4비트는 genctr).
 * nr_tags > 1 이면 batch 할당을 먼저 시도해 sbitmap 락 횟수를 줄인다.
 * tag 가 없으면 3ms sleep 후 hctx 를 재선택해 재시도한다 (CPU hotplug 대응).
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_submit_bio 경로, sleep 가능).
 *
 * 호출 체인:
 *   blk_mq_get_new_requests → [__blk_mq_alloc_requests]
 *     → blk_mq_get_ctx / blk_mq_map_queue (CPU→hctx 선택)
 *     → blk_mq_limit_depth (scheduler depth 제한)
 *     → blk_mq_get_tag / __blk_mq_alloc_requests_batch (tag/CID 확보)
 *     → blk_mq_rq_ctx_init (request 초기화)
 */
static struct request *__blk_mq_alloc_requests(struct blk_mq_alloc_data *data)
{
	/* [한국어] 할당 컨텍스트에서 대상 큐를 꺼내 둔다. 아래에서 큐 플래그 조회와
	 * q_usage_counter 조작에 반복 사용하므로 지역 변수로 캐시한다. */
	struct request_queue *q = data->q;
	/* [한국어] alloc_time_ns: tag 대기를 포함한 전체 할당 지연 측정용 시작 시각 */
	u64 alloc_time_ns = 0;
	/* [한국어] 초기화를 마치고 호출자에게 돌려줄 request. 실패 경로에서는 NULL. */
	struct request *rq;
	/* [한국어] sbitmap 에서 받아 온 태그 번호. BLK_MQ_NO_TAG(-1)이면 고갈이다.
	 * NVMe 라면 이 번호가 CID 의 하위 12비트가 된다. */
	unsigned int tag;

	/* alloc_time includes depth and tag waits */
	/* [한국어] BLK_RQ_ALLOC_TIME 활성 시 현재 시각을 기록해 나중에 alloc 지연 계산. */
	if (blk_queue_rq_alloc_time(q))
		alloc_time_ns = blk_time_get_ns();

	/* [한국어] REQ_NOWAIT: O_NONBLOCK 또는 io_uring 의 비블로킹 I/O.
	 * 이 플래그가 있으면 tag 고갈 시 sleep 하지 않고 NULL 을 반환한다. */
	if (data->cmd_flags & REQ_NOWAIT)
		data->flags |= BLK_MQ_REQ_NOWAIT;

retry:
	/* [한국어] blk_mq_get_ctx(): 현재 CPU 의 소프트웨어 큐를 반환.
	 * CPU affinity 유지를 위해 preemption 이 비활성화된 상태에서 조회. */
	data->ctx = blk_mq_get_ctx(q);
	/* [한국어] blk_mq_map_queue: cmd_flags(READ/WRITE/POLL 등)와 현재 CPU 를 기반으로
	 * 이 request 를 처리할 hctx(NVMe SQ)를 선택. POLL 이면 HCTX_TYPE_POLL 선택. */
	data->hctx = blk_mq_map_queue(data->cmd_flags, data->ctx); /* bio/opcode 를 기반으로 NVMe SQ 선택 */

	/* [한국어] blk_mq_limit_depth: scheduler 가 있으면 async_depth 제한 적용.
	 * NVMe 에서 BFQ/mq-deadline 의 queue_depth 정책이 여기서 반영된다. */
	blk_mq_limit_depth(data);
	/* [한국어] BLK_MQ_REQ_RESERVED: 예약 태그 풀 사용 요청.
	 * 플러시 등 특수 명령이 일반 I/O 가 tag 를 소진해도 확보되도록 보장. */
	if (data->flags & BLK_MQ_REQ_RESERVED)
		data->rq_flags |= RQF_RESV;

	/*
	 * Try batched alloc if we want more than 1 tag.
	 */
	/* [한국어] nr_tags > 1 이면 배치 할당 시도 — sbitmap 접근 횟수를 줄이는 최적화. */
	if (data->nr_tags > 1) {
		rq = __blk_mq_alloc_requests_batch(data);
		/* [한국어] 배치가 하나라도 잡혔으면 그 첫 request 를 돌려준다.
		 * 나머지는 이미 data->cached_rqs 에 쌓여 있어 plug 가 꺼내 쓴다. */
		if (rq) {
			/* [한국어] 배치 성공: 첫 번째 request 에 alloc 시각 기록 후 반환 */
			blk_mq_rq_time_init(rq, alloc_time_ns);
			return rq;	/* [한국어] 배치로 확보한 것 중 첫 request 를 반환. 나머지는 data->cached_rqs 에 남아 다음 I/O 가 꺼내 쓴다 */
		}
		/* [한국어] 배치 실패: 단일 tag 할당으로 fallback */
		/* [한국어] 배치 실패(연속 비트를 못 잡음) — 목표를 1개로 낮춰
		 * 아래의 단일 태그 경로로 내려간다. 이 값을 줄여 두지 않으면
		 * 재시도(goto retry) 때 또 배치를 시도해 같은 실패를 반복한다. */
		data->nr_tags = 1;
	}

	/*
	 * Waiting allocations only fail because of an inactive hctx.  In that
	 * case just retry the hctx assignment and tag allocation as CPU hotplug
	 * should have migrated us to an online CPU by now.
	 */
	/* [한국어] blk_mq_get_tag: sbitmap 에서 빈 비트 하나를 원자적으로 획득.
	 * 고갈 시 BLK_MQ_NO_TAG(-1) 반환 — 대기 또는 실패 처리 필요. */
	tag = blk_mq_get_tag(data); /* [한국어] 이 하나를 얻어야만 요청이 장치로 내려갈 수 있다.
				     * 고갈 시 NOWAIT 이 아니면 여기서 잠든다. */
	/* [한국어] 태그 고갈. 위 영문 주석이 말하듯, 대기를 허용한 할당이 실패하는
	 * 유일한 이유는 hctx 가 비활성(inactive)이 된 경우다 — CPU 핫플러그로
	 * 이 hctx 가 사라지는 중이라는 뜻이므로, 잠시 양보한 뒤 hctx 를 다시 골라야 한다. */
	if (tag == BLK_MQ_NO_TAG) {
		/* [한국어] NOWAIT: 즉시 실패 반환 — 상위에서 -EAGAIN 으로 변환 */
		if (data->flags & BLK_MQ_REQ_NOWAIT)
			return NULL;
		/*
		 * Give up the CPU and sleep for a random short time to
		 * ensure that thread using a realtime scheduling class
		 * are migrated off the CPU, and thus off the hctx that
		 * is going away.
		 */
		/* [한국어] 3ms sleep: 실시간 스케줄링 태스크가 CPU 를 떠나
		 * 핫플러그 중인 hctx 에서 벗어나도록 양보한다. 재시도 전 hctx 재선택. */
		msleep(3);
		goto retry;	/* [한국어] hctx 선택부터 다시 — 핫플러그로 옮겨간 CPU 기준으로 살아 있는 hctx 를 새로 고른다 */
	}

	/* [한국어] RQF_SCHED_TAGS 가 없으면 driver tag → hctx 활성 request 수 1 증가.
	 * blk_mq_dispatch_rq_list 의 budget 계산에서 사용된다. */
	if (!(data->rq_flags & RQF_SCHED_TAGS))
		blk_mq_inc_active_requests(data->hctx); /* hctx 의 인플라이트 요청 수 증가 — shared tag 구성에서 큐별 공정 분배(hctx_may_queue)의 입력이 된다 */
	/* [한국어] 획득한 tag 로 request 초기화 후 alloc_time 기록 → 반환 */
	rq = blk_mq_rq_ctx_init(data, blk_mq_tags_from_data(data), tag);
	blk_mq_rq_time_init(rq, alloc_time_ns);	/* [한국어] 태그를 기다린 시간까지 포함된 할당 지연의 시작 시각을 request 에 기록 */
	return rq;	/* [한국어] 태그 확보 + 초기화가 끝난 request 를 호출자에게 반환 */
}

/*
 * [한국어]
 * blk_mq_rq_cache_fill - plug의 request 캐시를 한 번에 여러 개 채운다(태그 일괄 획득)
 *
 * @q:     대상 request_queue
 * @plug:  현재 태스크의 plug. nr_ios(예상 I/O 개수)를 읽고 cached_rqs를 채운다.
 * @opf:   요청 연산 플래그(REQ_OP_READ 등). 어느 종류의 하드웨어 큐를 쓸지 결정한다.
 * @flags: 할당 동작 플래그(BLK_MQ_REQ_NOWAIT 등)
 * @return: 캐시에서 꺼내 쓸 첫 request. 실패 시 NULL.
 *
 * === 왜 일괄 할당인가: 태그 = NVMe Command ID ===  ★ 이 파일의 CID 서술 기준 ★
 * blk-mq의 "driver tag"는 sbitmap(확장 가능한 비트맵)에서 비트 하나를 잡는 것이다.
 * NVMe 는 이 비트 번호를 커맨드의 CID(Command Identifier)로 쓴다. CID 는 SQ 에
 * 실린 커맨드와 CQ 로 돌아온 완료를 짝짓는 유일한 열쇠이므로, 동시에 진행 중인
 * 커맨드마다 고유해야 한다. 태그 개수 상한이 곧 큐 깊이다.
 *
 * 정확히 말하면 "tag == CID" 는 아니고, drivers/nvme/host/nvme.h 를 직접 보면
 *   #define nvme_tag_from_cid(cid)       (cid & 0xfff)
 *   #define nvme_cid_install_genctr(gen) (nvme_genctr_mask(gen) << 12)
 *   nvme_cid(rq) = nvme_cid_install_genctr(nvme_req(rq)->genctr) | rq->tag
 * 즉 16비트 CID 중 **하위 12비트가 blk-mq 의 rq->tag** 이고, 상위 4비트는
 * NVMe 드라이버가 붙이는 세대 카운터(genctr)다. 그래서 blk-mq 태그의 상한은
 * 12비트(4096)를 넘을 수 없고, 실제로 nvme_alloc_io_tag_set() 은
 * queue_depth 를 min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1) 로 자른다.
 *
 * genctr 이 왜 필요한가: 태그는 완료되면 즉시 재사용된다. 컨트롤러가 어떤 이유로
 * 이미 완료 처리된 명령의 CQE 를 뒤늦게 한 번 더 올리면, 그 사이에 같은 태그를
 * 물려받은 **다른** 요청이 잘못 완료되어 데이터가 깨진다. 드라이버는 완료할
 * 때마다 genctr 을 올리고(nvme_try_complete_req), 완료 시
 * nvme_find_rq() 가 CQE 의 상위 4비트와 현재 genctr 을 대조해 불일치하면
 * "request %#x genctr mismatch" 를 찍고 그 CQE 를 버린다.
 * (NVME_QUIRK_SKIP_CID_GEN 인 컨트롤러에서는 이 세대 증가를 하지 않는다.)
 *
 * 이 함수 아래로는 서술의 간결함을 위해 "tag(CID)" 라고 줄여 쓰지만,
 * 정확한 관계는 항상 위와 같다.
 *
 * sbitmap에서 비트를 잡는 것은 원자적 연산이라 CPU 간 경합이 있을 수 있다.
 * 한 번에 하나씩 잡으면 I/O 개수만큼 경합이 발생하지만, 앞으로 몇 개를 쓸지
 * 미리 알고 있다면(io_uring이 blk_start_plug_nr_ios()로 알려준다) 한 번의
 * 연산으로 연속된 비트 여러 개를 잡을 수 있다. 그것이 이 함수의 목적이다.
 *
 * 고성능 NVMe에서 제출 경로의 CPU 사이클이 IOPS 한계를 좌우하므로, 이런
 * 배치 최적화가 실측 성능에 직접 반영된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O 제출 경로). blk_queue_enter()가 잠들 수
 * 있으므로 원자적 컨텍스트에서 호출 불가(단 BLK_MQ_REQ_NOWAIT면 잠들지 않는다).
 *
 * 에러 경로: 태그 획득 실패 시 blk_queue_enter()로 얻은 큐 참조를 반납하고 NULL.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_peek_cached_request →
 *   blk_mq_alloc_cached_request → [blk_mq_rq_cache_fill]
 *     → blk_queue_enter → __blk_mq_alloc_requests → blk_mq_get_tags(sbitmap)
 */
static struct request *blk_mq_rq_cache_fill(struct request_queue *q,
					    struct blk_plug *plug,
					    blk_opf_t opf,
					    blk_mq_req_flags_t flags)
{
	/* [한국어] 할당 파라미터를 한 구조체에 모아 __blk_mq_alloc_requests()에 넘긴다.
	 * 인자가 많아 구조체로 묶는 커널의 흔한 패턴이다. */
	struct blk_mq_alloc_data data = {
		/* [한국어] 태그를 잡을 대상 큐. */
		.q		= q,
		/* [한국어] NOWAIT 여부 등 할당 동작 제어 플래그. */
		.flags		= flags,
		/* [한국어] 0 = 깊이 제한 없음. I/O 스케줄러가 특정 종류의 요청에
		 * 태그를 일부만 쓰도록 제한할 때(async 쓰기 억제 등) 0이 아닌 값이 온다. */
		.shallow_depth	= 0,
		/* [한국어] 연산 플래그. 이 값으로 default/read/poll 중 어느 hctx 타입을
		 * 쓸지 결정된다. NVMe는 이 세 종류의 큐 맵을 가질 수 있다. */
		.cmd_flags	= opf,
		/* [한국어] 초기 rq_flags. 스케줄러 사용 여부 등이 __blk_mq_alloc_requests
		 * 내부에서 채워진다. */
		.rq_flags	= 0,
		/* [한국어] ★ 핵심 — 한 번에 잡을 태그 개수. plug->nr_ios는 상위가
		 * blk_start_plug_nr_ios()로 "이번에 대략 N개의 I/O를 낼 것"이라고 알려준
		 * 값이다. io_uring이 SQE 개수를 세어 넘긴다. */
		.nr_tags	= plug->nr_ios,
		/* [한국어] 여분으로 할당된 request들이 쌓일 곳. 첫 번째만 반환되고
		 * 나머지는 여기 남아 다음 I/O들이 꺼내 쓴다. */
		.cached_rqs	= &plug->cached_rqs,
		/* [한국어] NULL로 두면 __blk_mq_alloc_requests()가 현재 CPU 기준으로
		 * 소프트웨어 컨텍스트를 고른다. */
		.ctx		= NULL,
		/* [한국어] NULL로 두면 ctx와 cmd_flags로부터 하드웨어 컨텍스트를 고른다.
		 * NVMe에서는 이 선택이 "어느 SQ/CQ 쌍을 쓸 것인가"를 결정한다. */
		.hctx		= NULL
	};
	struct request *rq;	/* [한국어] 배치 할당의 첫 request 를 받아 둘 지역 변수 — 실패 시 NULL */

	/* [한국어] 큐 사용 참조를 획득한다. 이 참조는 request가 완료되어 반납될 때까지
	 * 유지되며, 그동안 큐가 freeze되거나 해제되지 못하게 막는다. NVMe 컨트롤러
	 * 리셋이나 네임스페이스 제거가 진행 중이면 여기서 블록되거나 실패한다. */
	if (blk_queue_enter(q, flags))
		return NULL;

	/* [한국어] 캐시를 채웠으므로 힌트를 1로 되돌린다. 다음번에 캐시가 비면
	 * 그때는 배치가 아니라 단건 할당 경로를 타게 하려는 것이다.
	 * nr_ios는 "예상 개수"일 뿐이라 한 번만 신뢰하고 이후에는 실제 소비 패턴에
	 * 맡긴다. */
	plug->nr_ios = 1;

	/* [한국어] 실제 태그 획득 + request 초기화. nr_tags개를 시도하되, sbitmap에
	 * 연속 공간이 부족하면 더 적게 잡힐 수도 있다(실패는 아니다). */
	rq = __blk_mq_alloc_requests(&data);
	/* [한국어] 하나도 못 잡았으면 위에서 얻은 큐 참조를 반드시 반납해야 한다.
	 * 반납하지 않으면 참조 카운트가 새어 이후 큐 freeze가 영원히 완료되지 않고,
	 * NVMe 컨트롤러 리셋이나 장치 제거가 행에 걸린다. */
	if (unlikely(!rq))
		blk_queue_exit(q);
	return rq;	/* [한국어] 성공하면 첫 request, 실패하면 NULL. 실패 시 큐 참조는 위에서 이미 반납했다 */
}

/*
 * [한국어]
 * blk_mq_alloc_cached_request - plug 캐시에서 재사용 가능한 request를 꺼낸다
 *
 * @q:     대상 request_queue
 * @opf:   새 I/O의 연산 플래그
 * @flags: 할당 동작 플래그
 * @return: 재사용 가능한 request, 없으면 NULL(호출자가 정식 할당 경로로 감)
 *
 * === 캐시된 request를 그대로 쓸 수 있는 조건 ===
 * 캐시에 남은 request 는 이미 태그와 하드웨어 큐가 정해진 상태다.
 * 따라서 새 I/O가 그 배정을 그대로 써도 되는지 세 가지를 확인해야 한다:
 *   1) 같은 request_queue인가 — 태그는 큐(정확히는 tag_set)에 종속이다. NVMe
 *      캐시된 request 는 특정 큐에 묶여 있으므로 다른 큐의 요청에는 재사용할 수 없다.
 *   2) 같은 hctx 타입인가 — blk-mq는 default / read / poll 세 종류의 큐 맵을
 *      가질 수 있고, NVMe는 write_queues/poll_queues 모듈 파라미터로 이를
 *      실제 하드웨어 큐 그룹에 대응시킨다. 읽기 전용 큐로 배정된 태그를
 *      쓰기에 쓰면 잘못된 SQ로 커맨드가 들어간다.
 *   3) flush 여부가 같은가 — flush request는 blk-flush 상태 기계가 쓰는
 *      전용 태그 영역(flush_rq)을 사용하므로 일반 request와 섞을 수 없다.
 *
 * 하나라도 어긋나면 NULL을 반환해 정식 할당 경로로 보낸다. 캐시된 request는
 * 그대로 남아 조건이 맞는 다음 I/O가 쓰게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O 제출 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_peek_cached_request
 *     → [blk_mq_alloc_cached_request] → blk_mq_rq_cache_fill
 */
static struct request *blk_mq_alloc_cached_request(struct request_queue *q,
						   blk_opf_t opf,
						   blk_mq_req_flags_t flags)
{
	/* [한국어] 현재 태스크의 plug. 없으면 캐시 자체가 존재하지 않는다. */
	struct blk_plug *plug = current->plug;
	struct request *rq;	/* [한국어] 캐시에서 꺼냈거나 새로 채운 request. 조건이 맞지 않으면 NULL 로 반환된다 */

	/* [한국어] plug를 쓰지 않는 경로(blk_start_plug 미호출)면 캐시가 없다. */
	if (!plug)
		return NULL;

	if (rq_list_empty(&plug->cached_rqs)) {	/* [한국어] 캐시가 비어 있는가 — 비었으면 채울지 결정하고, 남아 있으면 재사용 가능한지 검사한다 */
		/* [한국어] 캐시가 비었다 — 채울지 말지 결정한다. */
		if (plug->nr_ios == 1)
			/* [한국어] 상위가 "I/O 하나만 낼 것"이라고 알렸다. 배치 할당은
			 * 여분 태그를 잡았다가 안 쓰면 낭비이므로, 단건이면 캐시를
			 * 채우지 않고 일반 경로로 보낸다. blk_start_plug()의 기본값이
			 * nr_ios = 1이므로 대부분의 경로가 여기로 온다. */
			return NULL;
		/* [한국어] nr_ios > 1 — 여러 개를 쓸 예정이므로 한 번에 잡아 둔다.
		 * io_uring이 blk_start_plug_nr_ios(&plug, N)으로 알려준 경우다. */
		rq = blk_mq_rq_cache_fill(q, plug, opf, flags);
		if (!rq)	/* [한국어] 배치 채우기 실패(태그 고갈 등) — 캐시 경로를 포기하고 호출자가 정식 할당을 하게 한다 */
			return NULL;	/* [한국어] NULL 반환. blk_mq_rq_cache_fill 이 이미 큐 참조를 정리했으므로 여기서 할 일은 없다 */
	} else {
		/* [한국어] 캐시에 남은 것이 있다 — 꺼내기 전에 호환성을 확인한다.
		 * peek은 리스트에서 제거하지 않고 들여다보기만 하므로, 조건이 맞지
		 * 않으면 그대로 남겨 둘 수 있다. */
		rq = rq_list_peek(&plug->cached_rqs);
		/* [한국어] 조건 1 — 같은 큐인가. 다른 NVMe 네임스페이스나 다른 장치의
		 * 태그는 이 큐에서 유효하지 않다. */
		if (!rq || rq->q != q)
			return NULL;

		/* [한국어] 조건 2 — 하드웨어 큐 타입이 같은가.
		 * blk_mq_get_hctx_type(opf)는 연산 플래그에서 HCTX_TYPE_DEFAULT /
		 * HCTX_TYPE_READ / HCTX_TYPE_POLL 중 하나를 도출한다. NVMe에서
		 * poll 큐는 인터럽트 없이 폴링으로 완료를 확인하는 별도 하드웨어 큐라,
		 * 타입이 다르면 완료 처리 방식 자체가 달라진다. */
		if (blk_mq_get_hctx_type(opf) != rq->mq_hctx->type)
			return NULL;
		/* [한국어] 조건 3 — flush 여부가 같은가. flush request는 전용 예약
		 * 태그를 쓰는 별도 경로이므로 일반 request와 교환할 수 없다. */
		if (op_is_flush(rq->cmd_flags) != op_is_flush(opf))
			return NULL;

		/* [한국어] 모든 조건 통과 — 이제 실제로 리스트에서 꺼낸다. */
		rq_list_pop(&plug->cached_rqs);
		/* [한국어] 시작 시각을 지금으로 다시 찍는다. 캐시에 머문 시간은 이
		 * I/O의 지연이 아니므로, 미리 할당된 시점이 아니라 실제로 쓰이는
		 * 시점을 기준으로 삼아야 iostat의 await가 정확해진다. */
		blk_mq_rq_time_init(rq, blk_time_get_ns());
	}

	/* [한국어] 연산 플래그를 새 I/O의 것으로 교체한다. 이 값이 나중에
	 * nvme_setup_cmd()에서 NVMe opcode(Read 0x02 / Write 0x01 등)로 변환된다. */
	rq->cmd_flags = opf;
	/* [한국어] 큐 삽입용 리스트 헤드를 초기화한다. 캐시에 있는 동안 다른 리스트에
	 * 연결되어 있었을 수 있으므로, 재사용 전에 깨끗한 상태로 되돌린다. */
	INIT_LIST_HEAD(&rq->queuelist);
	return rq;	/* [한국어] 검증을 통과한 재사용 request 또는 새로 채운 request 를 반환 */
}

/*
 * [한국어]
 * blk_mq_alloc_request - bio 없이 request(= NVMe Command ID 슬롯)를 직접 할당
 *
 * @q:     대상 request_queue. NVMe에서는 네임스페이스 큐 또는 admin 큐다.
 * @opf:   연산 플래그. passthrough라면 REQ_OP_DRV_IN / REQ_OP_DRV_OUT이 온다.
 * @flags: BLK_MQ_REQ_NOWAIT(태그가 없으면 즉시 실패),
 *         BLK_MQ_REQ_RESERVED(예약 태그 영역 사용, 아래 설명 참고) 등
 * @return: 할당된 request. 실패 시 ERR_PTR(-EWOULDBLOCK 또는 blk_queue_enter의 오류)
 *
 * === bio 경로와 무엇이 다른가 ===
 * blk_mq_submit_bio()는 파일시스템이 만든 bio를 request로 바꾸지만, 이 함수는
 * bio 없이 빈 request만 만든다. 커널 내부나 사용자 도구가 "블록 I/O가 아닌
 * 명령"을 장치에 보내려 할 때 쓰는 입구다.
 *
 * NVMe에서의 주 사용처:
 *   - nvme-cli의 ioctl: nvme_submit_user_cmd() → blk_mq_alloc_request()로
 *     request를 얻고, 그 안에 Identify / Get Log Page / Format 등 임의의
 *     64바이트 커맨드를 채워 넣는다.
 *   - 드라이버 내부 admin 커맨드: __nvme_submit_sync_cmd()가 Set Features,
 *     Create I/O Queue, Keep Alive 등을 보낼 때.
 *   - 컨트롤러 복구: nvme_timeout()이 admin 큐에 Abort 커맨드를 보낼 때
 *     (drivers/nvme/host/pci.c — blk_mq_alloc_request(dev->ctrl.admin_q,
 *      nvme_req_op(&cmd), BLK_MQ_REQ_NOWAIT); 타임아웃 핸들러라 sleep 금지).
 *   - I/O 큐 해체: nvme_delete_queue()가 Delete SQ/CQ admin 커맨드를 보낼 때.
 *   - Keep Alive: nvme_keep_alive_work()가 주기적 KA 커맨드를 보낼 때.
 *
 * === 예약 태그(BLK_MQ_REQ_RESERVED)가 중요한 이유 ===
 * 일반 I/O가 모든 태그를 소진한 상태에서 컨트롤러가 응답을 멈추면, 그것을
 * 되살릴 커맨드조차 태그를 못 얻어 교착에 빠질 수 있다. set->reserved_tags 로
 * 태그 공간 앞쪽을 떼어 두면 BLK_MQ_REQ_RESERVED 를 준 요청만 그 영역을 쓴다.
 *
 * 다만 이 트리의 NVMe 를 직접 확인해 보면, reserved_tags 를 쓰는 곳은
 * (drivers/nvme/host/core.c)
 *   - Fabrics: admin tagset 에 2개(Connect + Keep Alive), I/O tagset 에 1개(Connect)
 *   - Apple 컨트롤러 quirk(NVME_QUIRK_SHARED_TAGS): admin 과 I/O 가 태그 공간을
 *     공유해야 해서 I/O tagset 앞 NVME_AQ_DEPTH 개를 예약
 * 이 두 가지뿐이다. 일반 PCIe NVMe 는 reserved_tags = 0 이고, Abort 는 I/O
 * tagset 이 아니라 별도의 admin 큐(ctrl->admin_q)에서 태그를 받으므로
 * I/O 태그 고갈과 무관하게 발행된다.
 *
 * === 할당된 request의 초기 상태 ===
 * bio가 없으므로 데이터 관련 필드를 모두 "빈 값"으로 초기화한다. 이후 호출자가
 * blk_rq_map_user() 등으로 사용자 버퍼를 붙이면 그때 bio가 생기고 필드가 채워진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. BLK_MQ_REQ_NOWAIT가 없으면 태그를 기다리며
 * 잠들 수 있다.
 *
 * 에러 경로: blk_queue_enter() 실패(큐가 죽었거나 freeze 중)면 그 오류를 그대로,
 * 태그 획득 실패면 -EWOULDBLOCK을 ERR_PTR로 반환한다. 후자는 NOWAIT 요청에서만
 * 발생하며, 호출자가 재시도 여부를 결정한다.
 *
 * 호출 체인:
 *   nvme_alloc_user_request (nvme-cli ioctl 경로, drivers/nvme/host/ioctl.c)
 *   __nvme_submit_sync_cmd  (드라이버 내부 동기 커맨드, core.c)
 *   nvme_timeout / nvme_delete_queue (pci.c, 둘 다 BLK_MQ_REQ_NOWAIT)
 *     → [blk_mq_alloc_request]
 *       → blk_mq_alloc_cached_request (plug 캐시 재사용 시도)
 *       → blk_queue_enter → __blk_mq_alloc_requests → blk_mq_get_tag(sbitmap)
 */
struct request *blk_mq_alloc_request(struct request_queue *q, blk_opf_t opf,
		blk_mq_req_flags_t flags)
{
	struct request *rq;

	/* [한국어] 먼저 plug cache 에서 재사용 가능한 NVMe request 탐색 */
	rq = blk_mq_alloc_cached_request(q, opf, flags);
	if (!rq) {	/* [한국어] 캐시에서 못 얻었다 — 아래 블록에서 큐 참조를 잡고 정식 태그 할당을 수행한다 */
		/* [한국어] 캐시가 비었거나 조건이 맞지 않았다 — 정식 할당 경로로 간다.
		 * 할당 파라미터를 구조체에 모아 __blk_mq_alloc_requests()에 넘긴다. */
		struct blk_mq_alloc_data data = {
			/* [한국어] 태그를 잡을 대상 큐. */
			.q		= q,
			/* [한국어] NOWAIT/RESERVED 등 호출자가 지정한 동작 플래그를 그대로 전달. */
			.flags		= flags,
			/* [한국어] 0 = 깊이 제한 없음. bio 경로에서는 스케줄러가 async 쓰기를
			 * 억제하려고 0이 아닌 값을 넣기도 하지만, passthrough는 사용자가
			 * 명시적으로 보낸 명령이라 인위적으로 제한하지 않는다. */
			.shallow_depth	= 0,
			/* [한국어] 연산 플래그. passthrough라면 REQ_OP_DRV_IN/OUT이며,
			 * 이 값으로 default/read/poll 중 어느 hctx 타입을 쓸지 결정된다. */
			.cmd_flags	= opf,
			/* [한국어] 초기 rq_flags. 스케줄러 사용 여부(RQF_SCHED_TAGS),
			 * 예약 태그 여부(RQF_RESV) 등이 할당 함수 내부에서 채워진다. */
			.rq_flags	= 0,
			/* [한국어] 1개만 할당한다. 여기는 bio 제출 경로가 아니라 단건
			 * 명령 경로이므로 배치 할당의 이득이 없다. */
			.nr_tags	= 1,
			/* [한국어] 캐시에 여분을 쌓지 않는다(nr_tags가 1이므로 의미도 없다). */
			.cached_rqs	= NULL,
			/* [한국어] NULL → 현재 CPU 기준으로 소프트웨어 컨텍스트를 자동 선택. */
			.ctx		= NULL,
			/* [한국어] NULL → ctx와 cmd_flags로부터 하드웨어 큐를 자동 선택.
			 * 특정 큐를 지정해야 하면 blk_mq_alloc_request_hctx()를 쓴다. */
			.hctx		= NULL
		};
		int ret;	/* [한국어] blk_queue_enter 의 반환 코드를 담을 변수. 0 이 아니면 큐가 죽었거나 freeze 중이다 */

		/* [한국어] blk_queue_enter: NVMe namespace queue 사용 허가 획득 */
		ret = blk_queue_enter(q, flags);
		if (ret)	/* [한국어] 큐 진입 실패 — 큐가 dying(-ENODEV)이거나 NOWAIT 인데 freeze 중(-EAGAIN) */
			return ERR_PTR(ret);	/* [한국어] 오류 코드를 ERR_PTR 로 포장해 반환. 호출자는 IS_ERR 로 판별한다 */

		/* [한국어] __blk_mq_alloc_requests: 캐시에 없으므로 sbitmap 에서 새 태그를 잡아 request 를 만든다 */
		rq = __blk_mq_alloc_requests(&data);
		if (!rq)	/* [한국어] 태그 고갈 — NOWAIT 요청이었다는 뜻이다 */
			goto out_queue_exit;	/* [한국어] 큐 참조를 반납하는 공통 정리 경로로 점프. 참조를 흘리면 freeze 가 영영 안 끝난다 */
	}
	/* [한국어] 여기부터는 두 경로(캐시 재사용/신규 할당)가 합류해 공통 초기화를 한다.
	 * 전송할 데이터 길이 0 — 아직 버퍼가 붙지 않았다. 호출자가 blk_rq_map_user()나
	 * blk_rq_map_kern()으로 버퍼를 붙이면 그때 실제 길이가 채워진다. */
	rq->__data_len = 0;
	/* [한국어] 세그먼트 정렬 요약 비트 초기화. 버퍼가 없으니 제약도 없다(0 = 제약 없음).
	 * 나중에 blk_rq_append_bio()가 버퍼를 붙이며 실제 값을 계산한다. */
	rq->phys_gap_bit = 0;
	/* [한국어] 시작 섹터를 -1(전 비트 1)로 둔다. 0이 아니라 -1을 쓰는 이유는
	 * "유효한 LBA 0"과 "설정되지 않음"을 구분하기 위해서다. passthrough 커맨드는
	 * LBA 개념이 없거나 커맨드 본문에 따로 담기므로 이 필드를 쓰지 않는다. */
	rq->__sector = (sector_t) -1;
	/* [한국어] bio 사슬의 head와 tail을 모두 비운다. 캐시에서 재사용한 request라면
	 * 이전 I/O의 잔재가 남아 있을 수 있어 반드시 지워야 한다. */
	rq->bio = rq->biotail = NULL;
	return rq;	/* [한국어] 데이터 필드가 모두 초기화된 빈 request 반환. 호출자가 커맨드와 버퍼를 채운다 */
out_queue_exit:
	/* [한국어] 태그를 못 얻었다 — 위에서 획득한 큐 사용 참조를 반드시 반납한다.
	 * 반납을 빠뜨리면 참조가 새어 이후 컨트롤러 리셋이나 장치 제거 시
	 * blk_mq_freeze_queue()가 영원히 반환하지 않는다. */
	blk_queue_exit(q);
	/* [한국어] -EWOULDBLOCK = "지금은 태그가 없다". BLK_MQ_REQ_NOWAIT 요청에서만
	 * 도달하며(그 플래그가 없으면 __blk_mq_alloc_requests가 기다린다), 호출자는
	 * 나중에 재시도하거나 사용자에게 EAGAIN을 전달한다. */
	return ERR_PTR(-EWOULDBLOCK);
}
EXPORT_SYMBOL(blk_mq_alloc_request);

/*
 * [한국어]
 * blk_mq_alloc_request_hctx - 하드웨어 큐를 명시적으로 지정해 request를 할당
 *
 * @q:        대상 request_queue
 * @opf:      연산 플래그
 * @flags:    반드시 BLK_MQ_REQ_NOWAIT와 BLK_MQ_REQ_RESERVED를 모두 포함해야 한다.
 * @hctx_idx: 사용할 하드웨어 큐 인덱스. NVMe에서는 특정 SQ/CQ 쌍을 지정하는 것.
 * @return: 지정한 큐에 묶인 request. 실패 시 ERR_PTR
 *          (-EINVAL 플래그 오류, -EIO 인덱스 범위 초과,
 *           -EXDEV 그 큐가 어떤 CPU에도 매핑되지 않음, -EWOULDBLOCK 태그 없음)
 *
 * === 왜 큐를 직접 골라야 하는 경우가 있는가 ===
 * 보통은 blk-mq가 현재 CPU를 기준으로 큐를 자동 선택하는 것이 최적이다.
 * 그런데 "이 큐를 통해서만 보낼 수 있는 명령"이 존재한다. 대표적인 예가
 * NVMe over Fabrics의 연결 설정이다 — nvmf_connect_io_queue()가 각 I/O 큐마다
 * Connect 커맨드를 보내야 하는데, 그 커맨드는 반드시 대상 큐 자신을 통해
 * 나가야 큐가 활성화된다. 자동 선택에 맡기면 엉뚱한 큐로 나가 버린다.
 *
 * === NOWAIT + RESERVED를 강제하는 이유 ===
 * 위 영문 주석이 설명하는 바가 핵심이다. 태그 할당기가 잠들어 대기하면,
 * 깨어났을 때 다른 CPU에서 실행될 수 있고 그 결과 다른 하드웨어 큐의 태그를
 * 받게 된다. 그러면 "특정 큐 지정"이라는 목적 자체가 무너진다.
 * 이 드문 용도를 위해 태그 할당기를 복잡하게 만드는 대신, 호출자에게
 * NOWAIT(잠들지 않음)를 강제해 문제를 원천 차단했다.
 * RESERVED를 함께 요구하는 것은, 이런 연결/복구용 명령이 일반 I/O가 태그를
 * 모두 소진한 상황에서도 반드시 나갈 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. NOWAIT이므로 잠들지 않는다.
 *
 * 에러 경로: 모든 실패가 ERR_PTR로 반환된다. 특히 -EXDEV는 "이 큐는 지금
 * 어떤 온라인 CPU에도 매핑되어 있지 않다"는 뜻으로, 호출자(fabrics 연결 코드)는
 * 그 큐를 건너뛰라는 신호로 해석한다. CPU 핫플러그로 큐가 고립될 수 있어
 * 정상적으로 발생할 수 있는 상황이다.
 *
 * 호출 체인:
 *   nvmf_connect_io_queue (drivers/nvme/host/fabrics.c)
 *     → [blk_mq_alloc_request_hctx]
 *       → blk_queue_enter → blk_mq_get_tag → blk_mq_rq_ctx_init
 */
struct request *blk_mq_alloc_request_hctx(struct request_queue *q,
	blk_opf_t opf, blk_mq_req_flags_t flags, unsigned int hctx_idx)
{
	/* [한국어] 할당 파라미터. hctx는 NULL로 시작하지만 아래에서 hctx_idx로
	 * 직접 지정한 큐를 넣는다 — 이 함수의 핵심 차이점이다. */
	struct blk_mq_alloc_data data = {
		/* [한국어] 대상 큐. */
		.q		= q,
		/* [한국어] NOWAIT|RESERVED가 반드시 포함되어야 한다(아래에서 검증). */
		.flags		= flags,
		/* [한국어] 깊이 제한 없음 — 연결/복구용 명령을 인위적으로 막지 않는다. */
		.shallow_depth	= 0,
		/* [한국어] 연산 플래그. fabrics Connect는 REQ_OP_DRV_OUT을 쓴다. */
		.cmd_flags	= opf,
		/* [한국어] 아래에서 RQF_SCHED_TAGS / RQF_RESV가 추가된다. */
		.rq_flags	= 0,
		/* [한국어] 단건 할당. */
		.nr_tags	= 1,
		/* [한국어] plug 캐시를 쓰지 않는다 — 특정 큐 바인딩이 목적이므로
		 * 캐시에서 꺼낸 임의의 큐 소속 request는 쓸 수 없다. */
		.cached_rqs	= NULL,
		/* [한국어] 아래에서 hctx의 cpumask에 속한 CPU로 직접 설정한다. */
		.ctx		= NULL,
		/* [한국어] 아래에서 q->queue_hw_ctx[hctx_idx]로 직접 설정한다. */
		.hctx		= NULL
	};
	/* [한국어] 할당에 걸린 시간을 재기 위한 시작 시각. 0이면 측정하지 않는다. */
	u64 alloc_time_ns = 0;
	struct request *rq;	/* [한국어] 초기화 후 호출자에게 돌려줄 request. 실패 경로에서는 ERR_PTR 이 대신 반환된다 */
	/* [한국어] 지정한 하드웨어 큐를 담당하는 CPU 중 하나. 소프트웨어 컨텍스트를
	 * 고르는 데 쓴다. */
	unsigned int cpu;
	/* [한국어] 획득한 태그 번호 = NVMe Command ID. */
	unsigned int tag;
	int ret;	/* [한국어] 실패 코드 보관 변수. out_queue_exit 에서 ERR_PTR(ret) 로 포장해 반환한다 */

	/* alloc_time includes depth and tag waits */
	/* [한국어] QUEUE_FLAG_RQ_ALLOC_TIME이 켜진 큐(주로 blk-iocost 사용 시)에서만
	 * 할당 시작 시각을 기록한다. 이 시각을 기준으로 삼는 이유는 영문 주석대로
	 * "큐 깊이 제한과 태그 대기에 쓴 시간"까지 I/O 지연에 포함시키기 위해서다.
	 * 태그를 못 얻어 기다린 시간도 사용자 입장에서는 I/O 지연이기 때문이다.
	 * 플래그가 꺼져 있으면 clock 읽기 비용(수십 ns)조차 아낀다. */
	if (blk_queue_rq_alloc_time(q))
		alloc_time_ns = blk_time_get_ns();

	/*
	 * If the tag allocator sleeps we could get an allocation for a
	 * different hardware context.  No need to complicate the low level
	 * allocator for this for the rare use case of a command tied to
	 * a specific queue.
	 */
	/* [한국어] 특정 hctx 지정: NOWAIT+RESERVED 조합만 지원 (NVMe passthrough 등 특수 경우) */
	if (WARN_ON_ONCE(!(flags & BLK_MQ_REQ_NOWAIT)) ||
	    WARN_ON_ONCE(!(flags & BLK_MQ_REQ_RESERVED)))
		return ERR_PTR(-EINVAL);

	/* [한국어] hctx_idx 가 NVMe SQ 총 개수를 벗어나면 오류 */
	if (hctx_idx >= q->nr_hw_queues)
		return ERR_PTR(-EIO);

	/* [한국어] 큐 사용 참조를 획득한다. 이후 모든 실패 경로는 out_queue_exit로
	 * 가서 이 참조를 반납해야 한다. */
	ret = blk_queue_enter(q, flags);
	if (ret)	/* [한국어] 큐 진입 실패 — dying 이거나 NOWAIT 인데 freeze 중 */
		return ERR_PTR(ret);	/* [한국어] 아직 아무 자원도 잡지 않았으므로 정리 없이 바로 오류 반환 */

	/*
	 * Check if the hardware context is actually mapped to anything.
	 * If not tell the caller that it should skip this queue.
	 */
	/* [한국어] 이 지점 이후의 실패는 모두 -EXDEV로 보고한다. 미리 설정해 두고
	 * goto로 빠지는 커널의 흔한 오류 처리 관용구다. */
	ret = -EXDEV;
	/* [한국어] 요청받은 인덱스의 하드웨어 컨텍스트를 꺼낸다. NVMe에서 이
	 * hctx의 driver_data에는 해당 nvme_queue(SQ/CQ 쌍) 포인터가 들어 있다. */
	data.hctx = q->queue_hw_ctx[hctx_idx];
	/* [한국어] 이 하드웨어 큐에 매핑된 CPU가 하나라도 있는지 확인한다.
	 * CPU 핫플러그로 이 큐를 담당하던 CPU들이 전부 오프라인되면 매핑이 비어
	 * 있을 수 있다. 그런 큐로 명령을 보내면 완료 인터럽트를 받을 CPU가 없어
	 * 영원히 대기하게 되므로, 호출자에게 건너뛰라고 알린다. */
	if (!blk_mq_hw_queue_mapped(data.hctx))
		goto out_queue_exit;
	/* [한국어] 이 하드웨어 큐를 담당하는 CPU들 중 지금 온라인인 첫 번째를 고른다.
	 * cpumask_first_and는 두 마스크의 교집합에서 최소 비트 번호를 돌려준다. */
	cpu = cpumask_first_and(data.hctx->cpumask, cpu_online_mask);
	/* [한국어] nr_cpu_ids 이상이면 "교집합이 비었다"는 뜻 — 매핑은 있지만
	 * 그 CPU들이 모두 오프라인이다. 위와 같은 이유로 -EXDEV. */
	if (cpu >= nr_cpu_ids)
		goto out_queue_exit;
	/* [한국어] 고른 CPU의 소프트웨어 컨텍스트를 가져온다. 현재 실행 중인 CPU가
	 * 아니라 "그 큐에 속한" CPU를 쓰는 것이 이 함수의 특징이다. */
	data.ctx = __blk_mq_get_ctx(q, cpu);

	if (q->elevator)	/* [한국어] 이 큐에 I/O 스케줄러(mq-deadline/BFQ 등)가 붙어 있는가 */
		/* [한국어] 스케줄러가 붙어 있으면 스케줄러 태그를 쓴다. 실제 드라이버
		 * 태그(NVMe CID)는 dispatch 시점에 따로 획득한다. */
		data.rq_flags |= RQF_SCHED_TAGS;
	else	/* [한국어] 스케줄러가 없는 경우(NVMe 기본 none) */
		/* [한국어] 스케줄러가 없으면 드라이버 태그를 직접 쓴다. blk_mq_tag_busy()는
		 * 이 하드웨어 큐가 활성 상태임을 표시해, 여러 큐가 태그 풀을 공유할 때
		 * (BLK_MQ_F_TAG_QUEUE_SHARED) 공평하게 나눠 갖도록 한다. */
		blk_mq_tag_busy(data.hctx);

	/* [한국어] 예약 태그 영역을 쓰겠다고 표시한다. 위에서 이 플래그를 강제했으므로
	 * 이 조건은 항상 참이지만, 플래그 전달 경로를 명시적으로 남겨 둔 것이다.
	 * 예약 태그는 일반 I/O가 태그를 모두 소진해도 연결/복구 명령이 나갈 수 있게
	 * 남겨 둔 몫이다. */
	if (flags & BLK_MQ_REQ_RESERVED)
		data.rq_flags |= RQF_RESV;

	/* [한국어] 이후 실패는 -EWOULDBLOCK(태그 없음)으로 보고한다. */
	ret = -EWOULDBLOCK;
	/* [한국어] blk_mq_get_tag: 특정 NVMe SQ 의 빈 CID sbitmap 슬롯 확보 */
	tag = blk_mq_get_tag(&data);
	if (tag == BLK_MQ_NO_TAG)	/* [한국어] 태그 고갈 — 이 함수는 항상 NOWAIT 이므로 기다리지 않고 즉시 실패한다 */
		goto out_queue_exit;	/* [한국어] 위에서 세팅한 -EWOULDBLOCK 을 들고 큐 참조 반납 경로로 간다 */
	if (!(data.rq_flags & RQF_SCHED_TAGS))	/* [한국어] 스케줄러 태그가 아니라 드라이버 태그를 잡은 경우에만 */
		/* [한국어] driver tag 이면 active CID 카운트 증가 (scheduler tag 는 별도 계산) */
		blk_mq_inc_active_requests(data.hctx);
	/* [한국어] 획득한 태그로 request 구조체를 초기화한다. blk_mq_tags_from_data()는
	 * 스케줄러 태그 세트와 드라이버 태그 세트 중 이번에 쓴 쪽을 골라 준다.
	 * 이 함수 안에서 rq->mq_hctx = data.hctx가 설정되어, 우리가 지정한
	 * 하드웨어 큐에 request가 확정적으로 묶인다. */
	rq = blk_mq_rq_ctx_init(&data, blk_mq_tags_from_data(&data), tag);
	/* [한국어] 위에서 기록한 할당 시작 시각을 request에 반영한다. 측정을 안 했다면
	 * 0이 들어가고, 그때는 이 함수가 현재 시각을 대신 채운다. */
	blk_mq_rq_time_init(rq, alloc_time_ns);
	/* [한국어] 아래 네 줄은 bio 없는 request의 공통 초기화다(blk_mq_alloc_request와
	 * 동일). 데이터 길이 0 — 아직 버퍼가 붙지 않았다. */
	rq->__data_len = 0;
	/* [한국어] 세그먼트 정렬 요약 비트 초기화(버퍼가 없으니 제약 없음). */
	rq->phys_gap_bit = 0;
	/* [한국어] 시작 섹터를 -1로 두어 "설정되지 않음"을 표시한다. */
	rq->__sector = (sector_t) -1;
	/* [한국어] bio 사슬을 비운다. */
	rq->bio = rq->biotail = NULL;
	return rq;	/* [한국어] 지정한 hctx 에 확정적으로 묶인 request 반환 — fabrics Connect 가 큐마다 하나씩 필요로 하는 것 */

out_queue_exit:
	/* [한국어] 공통 실패 처리 — 위에서 획득한 큐 사용 참조를 반납한다.
	 * 반납하지 않으면 참조가 새어 이후 컨트롤러 리셋이나 장치 제거가
	 * 영원히 완료되지 않는다. */
	blk_queue_exit(q);
	/* [한국어] 미리 설정해 둔 오류 코드(-EXDEV 또는 -EWOULDBLOCK)를
	 * ERR_PTR로 감싸 반환한다. */
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(blk_mq_alloc_request_hctx);

/*
 * [한국어]
 * blk_mq_finish_request - request 완료 직전 스케줄러/zoned 정리
 *
 * @rq: 완료 처리 중인 request
 *
 * 스케줄러의 finish_request 콜백을 호출해 scheduler 내부 상태를 정리하고,
 * zoned storage (ZNS NVMe) 관련 마무리를 수행한다.
 * RQF_USE_SCHED 플래그 해제로 postflush request 의 이중 finish_request 방지.
 * 실행 컨텍스트: softirq 또는 프로세스 컨텍스트 (blk_mq_free_request 경로).
 *
 * 호출 체인:
 *   blk_mq_free_request → [blk_mq_finish_request]
 *     → blk_zone_finish_request (ZNS NVMe 쓰기 포인터 갱신)
 *     → elevator.finish_request (scheduler 내부 상태 정리)
 */
static void blk_mq_finish_request(struct request *rq)
{
	/* [한국어] rq->q: 이 request 가 속한 NVMe namespace 의 request_queue */
	struct request_queue *q = rq->q;

	/* [한국어] ZNS NVMe: 쓰기 완료 시 zone 의 write pointer 를 갱신.
	 * 일반 NVMe (CMB, namespace 가 zoned 아님) 에서는 no-op. */
	blk_zone_finish_request(rq);

	/* [한국어] IO 스케줄러를 경유한 request 만 finish_request 콜백 호출.
	 * BFQ: bfq_finish_request (bfqq 와의 연결 해제).
	 * mq-deadline: 내부 타이머/통계 정리. */
	if (rq->rq_flags & RQF_USE_SCHED) {
		q->elevator->type->ops.finish_request(rq);
		/*
		 * For postflush request that may need to be
		 * completed twice, we should clear this flag
		 * to avoid double finish_request() on the rq.
		 */
		/* [한국어] postflush: 쓰기 → flush → 쓰기 순서 보장을 위해 두 번 완료될 수 있다.
		 * 플래그를 clear 해 두 번째 완료 시 finish_request 가 재호출되지 않도록. */
		rq->rq_flags &= ~RQF_USE_SCHED;
	}
}

/*
 * [한국어]
 * __blk_mq_free_request - tag(CID) 와 q_usage_counter 참조를 반납하는 최종 해제
 *
 * @rq: 해제할 request (참조 카운트가 이미 0이 된 상태)
 *
 * 완료된 NVMe 명령의 CID 를 hctx->tags sbitmap 에 반납하고,
 * scheduler tag 가 있으면 sched_tags 에도 반납한다.
 * 이후 blk_mq_sched_restart 로 새 dispatch 가 가능함을 알리고,
 * blk_queue_exit 로 q_usage_counter 를 1 감소시킨다.
 * 실행 컨텍스트: softirq 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_free_request → (req_ref_put_and_test == true) → [__blk_mq_free_request]
 *     → blk_mq_put_tag (CID 반납 → sbitmap 비트 clear → wakeup submitters)
 *     → blk_queue_exit (percpu_ref_put)
 */
static void __blk_mq_free_request(struct request *rq)
{
	/* [한국어] rq->q: blk_queue_exit 에 전달할 큐 포인터 (rq 해제 전에 저장) */
	struct request_queue *q = rq->q;
	/* [한국어] rq->mq_ctx: CID 반납 시 per-CPU 통계 갱신에 필요 */
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	/* [한국어] rq->mq_hctx: tag 가 속한 SQ(hctx)를 NULL 전에 저장 */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	/* [한국어] internal_tag: scheduler tag 번호 (사용 시 별도 반납 필요) */
	const int sched_tag = rq->internal_tag;

	/* [한국어] blk_crypto_free_request: NVMe inline encryption keyslot 반환.
	 * 암호화 미사용 시 no-op. */
	blk_crypto_free_request(rq);
	/* [한국어] blk_pm_mark_last_busy: 런타임 PM 의 idle 타이머를 현재 시각으로 갱신.
	 * NVMe 컨트롤러가 idle 상태로 인식되어 자동 suspend 로 진입 가능하게 준비. */
	blk_pm_mark_last_busy(rq);
	/* [한국어] rq->mq_hctx = NULL: 이 request 가 더 이상 SQ 에 속하지 않음을 표시 */
	rq->mq_hctx = NULL;

	/* [한국어] tag != BLK_MQ_NO_TAG: driver tag(CID)가 실제 할당된 경우만 반납.
	 * driver tag 없이 sched tag 만 있을 수도 있다 (flush passthrough 등). */
	if (rq->tag != BLK_MQ_NO_TAG) { /* 유효한 CID 가 할당된 경우만 반납 */
		/* [한국어] hctx 의 active request 수 감소 → dispatch budget 반환 */
		blk_mq_dec_active_requests(hctx); /* hctx(SQ) 의 활성 CID 카운트 감소 */
		/* [한국어] blk_mq_put_tag: sbitmap 에서 이 tag(CID) 비트를 clear.
		 * 비트 해제 후 tag 를 기다리던 submitter 들에게 wakeup 신호 전송. */
		blk_mq_put_tag(hctx->tags, ctx, rq->tag); /* [한국어] 드라이버 태그 반납 — 이 순간 대기 중이던 다른 제출자가 깨어난다 */
	}
	/* [한국어] scheduler tag 가 있으면 sched_tags 에도 반납.
	 * BFQ/mq-deadline 은 driver tag 와 별도 sched_tags sbitmap 을 사용한다. */
	if (sched_tag != BLK_MQ_NO_TAG)
		blk_mq_put_tag(hctx->sched_tags, ctx, sched_tag);
	/* [한국어] blk_mq_sched_restart: hctx 에 tag 여유가 생겼음을 알려
	 * dispatch 를 재시도하도록 hctx 의 run 플래그를 세우거나 work 를 예약. */
	blk_mq_sched_restart(hctx);
	/* [한국어] blk_queue_exit: q_usage_counter 를 1 감소 (percpu_ref_put).
	 * freeze 대기 중이라면 이 감소가 percpu_ref_is_zero 조건을 충족시킬 수 있다. */
	blk_queue_exit(q);
}

/*
 * [한국어]
 * blk_mq_free_request - request 생명주기 종료: 스케줄러/QoS 정리 후 tag 반납
 *
 * @rq: 완료 처리된 request
 *
 * request 의 최종 처리 순서:
 * 1) blk_mq_finish_request: scheduler.finish_request + zone 정리
 * 2) rq_qos_done: writeback throttle, iolatency, iocost 완료 통보
 * 3) state = MQ_RQ_IDLE: 이 CID 가 이제 재할당 가능함을 표시
 * 4) req_ref_put_and_test: 참조 카운트 감소; 0이 되면 __blk_mq_free_request
 * 실행 컨텍스트: softirq(완료 경로) 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_end_request → [blk_mq_free_request] → __blk_mq_free_request
 */
void blk_mq_free_request(struct request *rq)
{
	/* [한국어] rq->q: rq_qos_done 전달에 필요 */
	struct request_queue *q = rq->q;

	/* [한국어] 스케줄러 finish_request 콜백 및 zone 완료 처리 */
	blk_mq_finish_request(rq);

	/* [한국어] rq_qos_done: QoS 체인(WBT, iolatency, iocost)에 완료를 통보.
	 * writeback throttle 은 여기서 IO 토큰을 반납한다. */
	rq_qos_done(q, rq);

	/* [한국어] WRITE_ONCE: rq->state 를 MQ_RQ_IDLE 로 원자적으로 변경.
	 * 다른 CPU 가 state 를 polling 할 경우를 위한 순서 보장. */
	WRITE_ONCE(rq->state, MQ_RQ_IDLE);
	/* [한국어] req_ref_put_and_test: 참조 카운트 --; 0이 되면 true 반환.
	 * 다중 end_io 경로가 있을 때 마지막 반납자만 tag 를 실제로 해제한다. */
	if (req_ref_put_and_test(rq))
		__blk_mq_free_request(rq);
}
EXPORT_SYMBOL_GPL(blk_mq_free_request);

/*
 * [한국어]
 * blk_mq_free_plug_rqs - plug cache 에 남은 미사용 request 를 모두 해제
 *
 * @plug: 해제할 cached_rqs 를 보유한 blk_plug
 *
 * 태스크가 unplug 되거나 io_schedule 에서 plug 를 제거할 때, 미리 할당해 두었지만
 * 실제 bio 에 사용되지 않은 request(CID) 를 반납한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_finish_plug / blk_flush_plug → [blk_mq_free_plug_rqs]
 */
void blk_mq_free_plug_rqs(struct blk_plug *plug)
{
	struct request *rq;

	/* [한국어] rq_list_pop: cached_rqs 에서 하나씩 꺼내 blk_mq_free_request 로 반납.
	 * cached_rqs 가 빌 때까지 반복. */
	while ((rq = rq_list_pop(&plug->cached_rqs)) != NULL)
		blk_mq_free_request(rq);
}

/*
 * [한국어]
 * blk_dump_rq_flags - request의 내부 상태를 커널 로그에 덤프(디버깅용)
 *
 * @rq:  덤프할 request
 * @msg: 로그 앞에 붙일 문맥 문자열(예: "request botched")
 * @return: 없음
 *
 * 블록 계층의 불변식이 깨졌을 때 무엇이 잘못됐는지 남기는 진단 도구다.
 * 정상 경로에서는 절대 호출되지 않으며, blk_update_request()의 정합성 검사가
 * 실패하거나 드라이버가 이상 상태를 발견했을 때만 실행된다.
 *
 * 출력하는 세 줄의 의미:
 *   1행 - 어느 장치의 request이고 cmd_flags(연산 종류 + REQ_* 플래그)가 무엇인가
 *   2행 - 시작 섹터와 전체/현재 섹터 수. nr != cnr이면 부분 완료 중이라는 뜻
 *   3행 - bio 사슬의 head/tail 포인터와 남은 바이트 수
 *
 * NVMe 디버깅에서의 쓰임: 커맨드가 이상하게 실패할 때 이 덤프의 sector와
 * cmd_flags를 nvme_setup_cmd()가 만든 SLBA/opcode와 대조하면, 블록 계층에서
 * 이미 잘못된 것인지 드라이버 변환에서 잘못된 것인지 구분할 수 있다.
 *
 * 실행 컨텍스트: 어디서든(오류 경로). printk는 IRQ 컨텍스트에서도 안전하다.
 *
 * 호출 체인:
 *   blk_update_request (정합성 검사 실패) → [blk_dump_rq_flags]
 */
void blk_dump_rq_flags(struct request *rq, char *msg)
{
	/* [한국어] 1행 — 문맥 메시지, 디스크 이름, cmd_flags(16진수).
	 * disk가 NULL일 수 있는 이유: 아직 디스크에 연결되지 않은 큐(부팅 중)나
	 * 디스크 없이 큐만 있는 경우(fabrics admin 큐)가 존재한다. 삼항 연산자로
	 * "?"를 대신 출력해 NULL 역참조를 피한다.
	 * __force는 sparse 정적 분석기에게 blk_opf_t(비트 타입)를 정수로 캐스팅하는
	 * 것이 의도된 것임을 알려 경고를 억제한다. */
	printk(KERN_INFO "%s: dev %s: flags=%llx\n", msg,
		rq->q->disk ? rq->q->disk->disk_name : "?",
		(__force unsigned long long) rq->cmd_flags);

	/* [한국어] 2행 — 위치와 크기.
	 *   blk_rq_pos          : 시작 섹터(512B 단위). NVMe SLBA로 변환될 값.
	 *   blk_rq_sectors      : request 전체에 남은 섹터 수
	 *   blk_rq_cur_sectors  : 현재 첫 bio에 남은 섹터 수
	 * 두 값이 다르면 여러 bio가 붙어 있거나 부분 완료가 진행 중이라는 뜻이다.
	 * "전체 < 현재"라는 모순이 보이면 그것이 바로 버그의 증거다. */
	printk(KERN_INFO "  sector %llu, nr/cnr %u/%u\n",
	       (unsigned long long)blk_rq_pos(rq),
	       blk_rq_sectors(rq), blk_rq_cur_sectors(rq));
	/* [한국어] 3행 — bio 사슬의 양 끝 포인터와 남은 바이트 수.
	 * 포인터를 그대로 찍는 이유는 crash 덤프나 다른 로그의 포인터와
	 * 대조해 같은 bio인지 확인하기 위해서다. bio == biotail이면 bio가
	 * 하나뿐이고, bio == NULL인데 len != 0이면 심각한 불일치다. */
	printk(KERN_INFO "  bio %p, biotail %p, len %u\n",
	       rq->bio, rq->biotail, blk_rq_bytes(rq));
}
EXPORT_SYMBOL(blk_dump_rq_flags);

/*
 * [한국어]
 * blk_account_io_completion - 완료된 바이트 수를 파티션 통계에 누적
 *
 * @req:   완료 처리 중인 request
 * @bytes: 이번에 완료된 바이트 수(부분 완료면 그 일부)
 * @return: 없음
 *
 * /proc/diskstats의 "읽은 섹터 수 / 쓴 섹터 수" 항목을 갱신한다. iostat의
 * rkB/s와 wkB/s가 이 값의 시간당 변화율로 계산되므로, NVMe 대역폭 측정의
 * 최종 근거가 되는 지점이다.
 *
 * 부분 완료마다 호출되기 때문에 "실제로 전송이 끝난 만큼"만 정확히 누적된다.
 * request 단위로 한 번에 세면 부분 완료 상황에서 과대 계상된다.
 *
 * 실행 컨텍스트: 완료 경로 — 하드 IRQ이거나 softirq일 수 있다.
 * part_stat_lock()은 preempt_disable() 수준의 per-CPU 보호라 IRQ 컨텍스트에서도
 * 안전하다.
 *
 * 호출 체인:
 *   blk_update_request / blk_complete_request → [blk_account_io_completion]
 */
static void blk_account_io_completion(struct request *req, unsigned int bytes)
{
	/* [한국어] 통계 대상 request인지 확인한다. nvme-cli의 passthrough처럼
	 * 디스크 통계에 넣으면 안 되는 요청에는 이 플래그가 없다. */
	if (req->rq_flags & RQF_IO_STAT) {
		/* [한국어] 통계 그룹 인덱스를 구한다. STAT_READ / STAT_WRITE /
		 * STAT_DISCARD / STAT_FLUSH 중 하나로, 이 인덱스 덕분에 iostat이
		 * 읽기와 쓰기를 분리해 보여줄 수 있다. */
		const int sgrp = op_stat_group(req_op(req));

		/* [한국어] per-CPU 통계 영역 보호(내부적으로 preempt_disable). */
		part_stat_lock();
		/* [한국어] 완료 섹터 수를 누적한다. bytes >> 9는 바이트를 512B 섹터로
		 * 변환하는 것으로, 4Kn NVMe라도 통계는 항상 512B 단위다.
		 * req->part는 이 I/O가 향한 파티션의 통계 블록이며, 파티션 통계는
		 * 상위 디스크 통계에도 함께 반영된다. */
		part_stat_add(req->part, sectors[sgrp], bytes >> 9);
		part_stat_unlock();	/* [한국어] per-CPU 통계 갱신 종료 — preempt 재활성화(part_stat_lock 과 짝) */
	}
}

/*
 * [한국어]
 * blk_print_req_error - I/O 실패를 사람이 읽을 수 있는 형태로 커널 로그에 출력
 *
 * @req:    실패한 request
 * @status: 실패 원인을 나타내는 blk_status_t
 * @return: 없음
 *
 * dmesg에서 흔히 보는 다음과 같은 줄을 만드는 함수다:
 *   "critical medium error, dev nvme0n1, sector 12345678 op 0x0:(READ)
 *    flags 0x0 phys_seg 4 prio class 0"
 *
 * === printk_ratelimited를 쓰는 이유 ===
 * NVMe 컨트롤러가 오동작하거나 링크가 끊기면 진행 중인 수백~수천 개의 커맨드가
 * 한꺼번에 실패한다. 일반 printk로는 로그가 폭주해 정작 중요한 첫 오류가
 * 스크롤로 밀려나고, 로그 출력 자체가 CPU를 잡아먹어 복구가 늦어진다.
 * ratelimited 변형은 기본적으로 5초에 10줄로 제한하고 초과분은
 * "callbacks suppressed"로 요약한다.
 *
 * === 각 필드를 출력하는 이유 ===
 *   status  - 실패 종류(medium error / I/O error / target 없음 ...).
 *             NVMe에서는 nvme_error_status()가 CQ 엔트리의 Status Field를
 *             변환한 결과이므로, 이 문자열을 역추적하면 컨트롤러가 보낸
 *             원래 상태 코드를 짐작할 수 있다.
 *   sector  - 실패한 위치. 반복 실패 시 특정 LBA에 집중되는지(매체 불량)
 *             흩어지는지(컨트롤러/링크 문제)로 원인을 구분한다.
 *   op      - 읽기인지 쓰기인지. 읽기만 실패하면 매체 문제일 가능성이 높다.
 *   flags   - FUA, PREFLUSH, RAHEAD 등. read-ahead 실패는 무해할 수 있다.
 *   phys_seg- 세그먼트 수. 특정 세그먼트 수 이상에서만 실패하면 드라이버의
 *             PRP/SGL 구성이나 IOMMU 매핑을 의심할 근거가 된다.
 *   prio    - I/O 우선순위 클래스.
 *
 * 실행 컨텍스트: 완료 경로(IRQ 가능). printk는 IRQ에서도 안전하다.
 *
 * 호출 체인:
 *   blk_update_request (오류이면서 조용히 하라는 지시가 없을 때)
 *     → [blk_print_req_error]
 */
static void blk_print_req_error(struct request *req, blk_status_t status)
{
	/* [한국어] 속도 제한이 걸린 오류 로그 출력. 포맷 문자열이 두 줄로
	 * 나뉘어 있지만 C의 인접 문자열 리터럴 연결로 한 줄이 된다. */
	printk_ratelimited(KERN_ERR
		"%s error, dev %s, sector %llu op 0x%x:(%s) flags 0x%x "
		"phys_seg %u prio class %u\n",
		/* [한국어] blk_status_t를 "critical medium error" 같은 문자열로 변환. */
		blk_status_to_str(status),
		/* [한국어] 디스크 이름. 아직 디스크가 없는 큐면 "?"를 출력한다. */
		req->q->disk ? req->q->disk->disk_name : "?",
		/* [한국어] 실패한 시작 섹터와 연산 코드(숫자). */
		blk_rq_pos(req), (__force u32)req_op(req),
		/* [한국어] 같은 연산 코드를 "READ"/"WRITE" 같은 문자열로도 함께 출력해
		 * 사람이 16진수를 해석하지 않아도 되게 한다. */
		blk_op_str(req_op(req)),
		/* [한국어] cmd_flags에서 연산 코드 비트를 제외한 나머지 플래그.
		 * REQ_OP_MASK를 반전해 AND하면 FUA/PREFLUSH/RAHEAD 등만 남는다. */
		(__force u32)(req->cmd_flags & ~REQ_OP_MASK),
		/* [한국어] 물리 세그먼트 수 = NVMe PRP/SGL 디스크립터 개수. */
		req->nr_phys_segments,
		/* [한국어] I/O 우선순위에서 클래스 부분(RT/BE/IDLE)만 추출. */
		IOPRIO_PRIO_CLASS(req_get_ioprio(req)));
}

/*
 * Fully end IO on a request. Does not support partial completions, or
 * errors.
 */
/*
 * [한국어]
 * blk_complete_request - 오류 없는 전체 완료를 위한 빠른 경로
 *
 * @req: 성공적으로 완료된 request
 * @return: 없음
 *
 * === 왜 blk_update_request()와 별도로 존재하는가 ===
 * blk_update_request()는 부분 완료, 오류 전파, 세그먼트 재계산, mixed merge
 * 처리 등 모든 경우를 다루느라 무겁다. 그런데 NVMe 같은 장치의 압도적
 * 다수 완료는 "오류 없이 전부 끝남"이라는 가장 단순한 경우다.
 * 이 함수는 그 경우만 처리하는 대신 다음을 통째로 생략한다:
 *   - 바이트 배분 계산(어차피 전부이므로 min() 불필요)
 *   - 오류 상태 전파(오류가 없으므로)
 *   - __data_len/__sector 갱신과 세그먼트 재계산(남는 것이 없으므로)
 *   - 정합성 검사
 * 위 영문 주석의 "Does not support partial completions, or errors"가 이 계약이다.
 * 호출자 blk_mq_end_request()가 error == BLK_STS_OK인지 확인하고 이쪽으로 보낸다.
 *
 * NVMe 관점: CQ 인터럽트 하나에 여러 커맨드가 성공 완료로 들어오는 상황에서,
 * 이 빠른 경로가 완료 처리 비용을 눈에 띄게 줄인다. 완료 경로의 CPU 비용은
 * 곧 IOPS 한계이므로 의미 있는 최적화다.
 *
 * 실행 컨텍스트: 완료 경로(하드 IRQ 또는 softirq/IPI 이후).
 *
 * 호출 체인:
 *   nvme_irq → nvme_handle_cqe → blk_mq_complete_request
 *     → nvme_pci_complete_rq → nvme_complete_rq → blk_mq_end_request
 *     → [blk_complete_request] → bio_endio
 */
static void blk_complete_request(struct request *req)
{
	/* [한국어] blk-flush 상태 기계가 관리하는 request인지 확인한다. 그렇다면
	 * 아래에서 bio_endio()를 건너뛴다 — 데이터는 전송됐어도 POST_FLUSH가
	 * 남아 지속성이 아직 보장되지 않았기 때문이다. */
	const bool is_flush = (req->rq_flags & RQF_FLUSH_SEQ) != 0;
	/* [한국어] 전체 완료이므로 "완료 바이트 = request 전체 크기"다. 부분 완료를
	 * 지원하지 않으므로 인자로 받지 않고 여기서 직접 구한다. */
	int total_bytes = blk_rq_bytes(req);
	/* [한국어] bio 사슬의 머리. 아래 루프가 이 포인터부터 순회한다. */
	struct bio *bio = req->bio;

	/* [한국어] blktrace 완료 이벤트 기록. 상태는 무조건 BLK_STS_OK다
	 * (이 함수는 성공 경로 전용이므로). */
	trace_block_rq_complete(req, BLK_STS_OK, total_bytes);

	/* [한국어] bio가 없는 request — nvme-cli passthrough처럼 커널이 직접 만든
	 * 명령이거나, 데이터 없는 flush다. 상위에 알릴 bio가 없으므로 여기서 끝낸다.
	 * 완료 통지는 rq->end_io 콜백이나 동기 대기자가 따로 받는다. */
	if (!bio)
		return;

	/* [한국어] 읽기이면서 무결성(PI) 메타데이터가 붙어 있으면 검증 후처리를 한다.
	 * 쓰기는 제출 전에 PI를 생성하므로 완료 시 할 일이 없고, 읽기만 완료 후
	 * 검증이 필요하다. 오류 검사가 없는 이유는 이 함수가 성공 경로 전용이기
	 * 때문이다(blk_update_request는 error == BLK_STS_OK 조건을 추가로 본다). */
	if (blk_integrity_rq(req) && req_op(req) == REQ_OP_READ)
		blk_integrity_complete(req, total_bytes);

	/*
	 * Upper layers may call blk_crypto_evict_key() anytime after the last
	 * bio_endio().  Therefore, the keyslot must be released before that.
	 */
	/* [한국어] blk_crypto keyslot 해제: NVMe encryption 명령 종료 후 즉시 반납
	 * (bio_endio 이전에 반납해야 상위 레이어의 evict_key 와 race 방지) */
	blk_crypto_rq_put_keyslot(req);

	/* [한국어] 전송 완료 섹터 수를 파티션 통계에 누적한다(iostat의 rkB/s, wkB/s). */
	blk_account_io_completion(req, total_bytes);

	/* [한국어] bio 사슬을 끝까지 순회하며 전부 완료시킨다. blk_update_request()의
	 * 루프와 달리 바이트를 배분하지 않고 무조건 전부 끝내므로 훨씬 단순하다.
	 * do-while인 이유: 위에서 bio != NULL을 이미 확인했으므로 첫 반복이 항상 유효하다. */
	do {
		/* [한국어] 다음 bio 포인터를 먼저 저장한다. bio_endio()가 호출되면
		 * 그 bio는 해제될 수 있어 이후 bi_next를 읽으면 use-after-free다.
		 * 이 한 줄이 순회의 안전을 보장하는 핵심이다. */
		struct bio *next = bio->bi_next;

		/* Completion has already been traced */
		/* [한국어] request 단위 완료를 위에서 이미 기록했으므로, bio_endio()가
		 * bio 단위로 다시 기록하지 않도록 플래그를 지운다. blkparse 출력에
		 * 같은 완료가 중복으로 나타나는 것을 막는다. */
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);

		/* [한국어] Zone Append였다면 장치가 실제로 기록한 LBA를 bio에 반영한다.
		 * ZNS에서 쓸 위치는 장치가 정하므로, 상위가 "어디에 쓰였는지" 알려면
		 * 완료 시점에 결과 LBA를 되돌려 주어야 한다. */
		if (blk_req_bio_is_zone_append(req, bio))
			blk_zone_append_update_request_bio(req, bio);

		/* [한국어] flush 시퀀스 중이 아니라면 bio를 최종 완료시킨다. bi_end_io
		 * 콜백이 실행되어 파일시스템의 완료 처리나 대기 프로세스 깨우기가 일어난다.
		 * flush 시퀀스 중이라면 blk-flush 상태 기계가 전 단계를 마친 뒤 직접
		 * 완료시키므로 여기서는 건너뛴다. */
		if (!is_flush)
			bio_endio(bio);
		/* [한국어] 미리 저장해 둔 다음 bio로 이동. */
		bio = next;
	} while (bio);

	/*
	 * Reset counters so that the request stacking driver
	 * can find how many bytes remain in the request
	 * later.
	 */
	/* [한국어] end_io 콜백이 없는 request만 카운터를 초기화한다.
	 * 왜 조건이 붙는가: end_io 콜백을 가진 request(주로 passthrough나 flush
	 * 상태 기계)는 콜백 안에서 blk_rq_bytes()로 "얼마나 전송됐는지"를 읽어야
	 * 하는 경우가 있다. 미리 0으로 지워 버리면 그 정보가 사라진다.
	 * 콜백이 없다면 아무도 읽지 않으므로, 위 영문 주석대로 스택형 드라이버가
	 * "남은 바이트 0"을 보고 완료를 판단할 수 있도록 깨끗이 비운다. */
	if (!req->end_io) {
		/* [한국어] bio 사슬 포인터를 끊는다. bio들은 이미 전부 완료되어
		 * 해제되었을 수 있으므로 남겨 두면 위험한 dangling 포인터가 된다. */
		req->bio = NULL;
		/* [한국어] 남은 데이터 길이를 0으로. blk_rq_bytes()가 0을 반환하게 되어
		 * "더 처리할 것이 없다"는 계약이 성립한다. */
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
 * [한국어]
 * blk_update_request - request의 앞부분 N바이트를 완료 처리하고 나머지를 재설정
 *
 * @req:      완료 처리할 request
 * @error:    완료 상태. BLK_STS_OK면 정상, 그 외에는 이 범위의 bio들에 전파된다.
 *            NVMe에서는 nvme_error_status()가 CQ 엔트리의 Status Field를
 *            blk_status_t로 변환한 값이 들어온다.
 * @nr_bytes: 완료된 바이트 수. 전체 완료라면 blk_rq_bytes(req)를 넘긴다.
 * @return: false = 이 request에 남은 데이터가 없다(호출자가 request를 끝내야 함)
 *          true  = 아직 데이터가 남았다(부분 완료, request는 살아 있다)
 *
 * === 이 함수가 하는 일의 본질 ===
 * request는 여러 bio가 사슬로 엮인 구조다. 이 함수는 그 사슬을 앞에서부터
 * nr_bytes만큼 "소비"하면서, 완전히 소비된 bio는 bio_endio()로 상위 계층에
 * 완료를 알리고, 부분만 소비된 bio는 이터레이터를 전진시켜 남겨 둔다.
 * 마지막에 request 자체의 위치(__sector)와 길이(__data_len), 세그먼트 수를
 * "남은 부분 기준"으로 다시 맞춘다.
 *
 * === 왜 부분 완료가 존재하는가 ===
 * NVMe PCIe의 일반적인 I/O에서는 커맨드 하나가 통째로 성공하거나 실패하므로
 * 대개 nr_bytes == blk_rq_bytes(req)로 한 번에 끝난다. 부분 완료가 의미를 갖는
 * 경우는 다음과 같다:
 *   - dm/md 같은 스택형 드라이버가 request를 잘라 하위 장치로 나눠 보내고
 *     조각별로 완료를 보고할 때
 *   - blk-flush 상태 기계가 FLUSH → DATA → POST_FLUSH 단계를 진행하며
 *     데이터 부분만 먼저 완료 처리할 때 (RQF_FLUSH_SEQ)
 *   - SCSI처럼 잔여 바이트(residual)를 보고하는 전송 계층
 *
 * === 완료 경로에서의 위치 ===
 *   NVMe CQ 인터럽트 → nvme_irq → nvme_handle_cqe
 *     → nvme_try_complete_req → blk_mq_complete_request
 *     → (IPI/softirq 경유 가능) → nvme_pci_complete_rq → nvme_complete_rq
 *     → blk_mq_end_request → [blk_update_request] → bio_endio
 *       → 파일시스템/DIO의 bi_end_io 콜백 → 대기 중인 프로세스 깨우기
 *
 * 실행 컨텍스트: 완료 경로. 하드 IRQ 컨텍스트일 수도 있고(드라이버가 IRQ에서
 * 바로 완료 처리하는 경우), softirq/IPI를 거친 다른 CPU일 수도 있다. 따라서
 * 잠들 수 있는 연산을 해서는 안 된다.
 *
 * 에러 경로: @error가 0이 아니면 소비되는 모든 bio의 bi_status에 그 값을 실어
 * 상위로 전파한다. 재시도 여부는 이미 nvme_decide_disposition()에서 결정되어,
 * 여기까지 왔다는 것은 "더 재시도하지 않고 실패로 확정"되었다는 뜻이다.
 *
 * 호출 체인:
 *   blk_mq_end_request / blk_mq_end_request_batch / blk_flush_complete_seq
 *     → [blk_update_request]
 *       → blk_integrity_complete / __blk_crypto_rq_put_keyslot
 *       → blk_account_io_completion / bio_advance / bio_endio
 *       → blk_recalc_rq_segments
 */
bool blk_update_request(struct request *req, blk_status_t error,
		unsigned int nr_bytes)
{
	/* [한국어] 이 request가 blk-flush 상태 기계(block/blk-flush.c)의 관리를 받고
	 * 있는가. FLUSH → DATA → POST_FLUSH 단계를 오가는 동안 bio를 실제로 끝내면
	 * 안 되므로(아직 지속성이 보장되지 않았다) 아래에서 bio_endio를 건너뛰는
	 * 판단에 쓰인다. NVMe에서 FUA 쓰기나 fsync()가 이 경로를 탄다. */
	bool is_flush = req->rq_flags & RQF_FLUSH_SEQ;
	/* [한국어] RQF_QUIET: 실패해도 커널 로그를 남기지 말라는 표시. 상위가 실패를
	 * 예상하고 있는 경우(장치 탐색, 미디어 없음 확인 등)에 로그 폭주를 막는다. */
	bool quiet = req->rq_flags & RQF_QUIET;
	/* [한국어] 이번 호출에서 실제로 소비한 총 바이트. 루프에서 누적해 마지막에
	 * __data_len과 __sector를 갱신하는 데 쓴다. nr_bytes와 다를 수 있다 —
	 * bio가 부족하면 요청받은 것보다 적게 소비될 수 있기 때문이다. */
	int total_bytes;

	/* [한국어] blktrace의 완료 이벤트("C") 기록. blkparse에서 제출("D")과 짝지어
	 * 장치 서비스 시간을 계산하는 근거가 된다. NVMe 지연 분석의 출발점. */
	trace_block_rq_complete(req, error, nr_bytes);

	/* [한국어] bio 사슬이 이미 비었다면 소비할 것이 없다. false를 반환해
	 * 호출자가 request를 최종 완료시키게 한다. */
	if (!req->bio)
		return false;

	/* [한국어] 무결성(T10 PI / NVMe End-to-End Data Protection) 후처리.
	 * 세 조건이 모두 필요하다:
	 *   blk_integrity_rq  - PI 메타데이터가 실제로 붙어 있는 request인가
	 *   REQ_OP_READ       - 읽기일 때만. 쓰기는 제출 전에 PI를 "생성"하지만
	 *                       읽기는 완료 후에 "검증"해야 하므로 시점이 다르다.
	 *   error == BLK_STS_OK - 실패한 I/O의 메타데이터는 검증할 의미가 없다.
	 * 하드웨어가 PI를 검사하는 구성이라면 컨트롤러가 이미 검증해 오류 시
	 * Guard Check Error 등의 상태 코드를 돌려주고, 소프트웨어 검증 구성이라면
	 * 여기서 guard/apptag/reftag를 대조한다. */
	if (blk_integrity_rq(req) && req_op(req) == REQ_OP_READ &&
	    error == BLK_STS_OK)
		blk_integrity_complete(req, nr_bytes);

	/*
	 * Upper layers may call blk_crypto_evict_key() anytime after the last
	 * bio_endio().  Therefore, the keyslot must be released before that.
	 */
	/* [한국어] 인라인 암호화 keyslot 반납. 조건이 "전체 완료(nr_bytes가 request
	 * 전체 크기 이상)"인 이유는 부분 완료 상태에서 반납하면 남은 데이터를 처리할
	 * 키가 사라지기 때문이다.
	 * 위 영문 주석이 밝히듯 반납 시점이 중요하다: 상위 계층은 마지막 bio_endio()가
	 * 끝나는 즉시 blk_crypto_evict_key()로 키를 폐기할 수 있으므로, 그 전에
	 * 슬롯을 돌려놓지 않으면 이미 폐기된 키를 참조하게 된다. 그래서 아래 bio
	 * 완료 루프보다 먼저 배치되어 있다. */
	if (blk_crypto_rq_has_keyslot(req) && nr_bytes >= blk_rq_bytes(req))
		__blk_crypto_rq_put_keyslot(req);

	/* [한국어] 오류 로그 출력 조건 — 네 가지를 모두 만족해야 한다.
	 *   error                  : 실제로 실패했는가
	 *   !blk_rq_is_passthrough : passthrough(nvme-cli 등)는 사용자 도구가 직접
	 *                            상태 코드를 해석하므로 커널이 로그를 남길 필요가
	 *                            없다. 오히려 정상적인 탐색 실패까지 로그에 남아
	 *                            혼란을 준다.
	 *   !quiet                 : 상위가 로그를 원하지 않는다고 명시하지 않았는가
	 *   !GD_DEAD               : 디스크가 이미 죽었다고 표시된 상태면, 이후의
	 *                            모든 I/O가 실패하므로 로그가 수만 줄 쏟아진다.
	 *                            NVMe 컨트롤러 제거(surprise removal)나 리셋
	 *                            실패 후 이 비트가 켜져 로그 폭주를 막는다. */
	if (unlikely(error && !blk_rq_is_passthrough(req) && !quiet) &&
	    !test_bit(GD_DEAD, &req->q->disk->state)) {
		/* [한국어] "critical medium error, dev nvme0n1, sector ..." 형태로
		 * 콘솔에 출력한다. blk_status_t를 사람이 읽을 문자열로 바꾼다. */
		blk_print_req_error(req, error);
		/* [한국어] 오류 전용 tracepoint. 로그와 달리 항상 기록되지 않고 이 조건
		 * 아래에서만 발생하므로, eBPF로 오류만 골라 추적할 때 쓴다. */
		trace_block_rq_error(req, error, nr_bytes);
	}

	/* [한국어] /proc/diskstats의 완료 섹터 수와 누적 서비스 시간을 갱신한다.
	 * iostat의 rkB/s, wkB/s가 여기서 나온다. 부분 완료마다 호출되므로 실제
	 * 전송된 만큼만 집계된다. */
	blk_account_io_completion(req, nr_bytes);

	/* [한국어] 소비 누적 카운터 초기화 후 bio 사슬 순회를 시작한다. */
	total_bytes = 0;
	/* [한국어] bio 사슬을 앞에서부터 훑으며 nr_bytes를 소진할 때까지 소비한다.
	 * 매 반복은 bio 하나를 "전부" 또는 "일부" 소비한다. */
	while (req->bio) {
		struct bio *bio = req->bio;
		/* [한국어] 이 bio에서 소비할 양. bio에 남은 크기와 아직 배분할 nr_bytes 중
		 * 작은 쪽이다. 같으면 이 bio가 완전히 끝나고, bio 쪽이 크면 부분 소비다. */
		unsigned bio_bytes = min(bio->bi_iter.bi_size, nr_bytes);

		/* [한국어] 실패라면 이 bio에 상태 코드를 새긴다. bio_endio() 시점에
		 * 상위 계층(파일시스템, DIO 완료 핸들러)이 이 값을 읽어 errno로 변환한다.
		 * unlikely로 표시된 대로 정상 경로에서는 실행되지 않는다. */
		if (unlikely(error))
			bio->bi_status = error;

		if (bio_bytes == bio->bi_iter.bi_size) {	/* [한국어] 이 bio 를 통째로 소비하는가 — 그렇다면 request 의 bio 사슬에서 떼어낸다 */
			/* [한국어] 이 bio가 완전히 소비된다 — request의 head를 다음 bio로
			 * 옮긴다. 주의: bio 자체를 아직 끝내지는 않았다. 아래 bio_advance
			 * 이후 bi_size가 0이 되면 그때 bio_endio()가 호출된다. */
			req->bio = bio->bi_next;
		} else if (bio_is_zone_append(bio) && error == BLK_STS_OK) {	/* [한국어] Zone Append 의 부분 완료는 지원 불가 — 조각들이 순차적으로 기록되지 않을 수 있어서다(영문 주석 참고). ZNS NVMe 의 Zone Append(opcode 0x7d)가 이 경우다 */
			/*
			 * Partial zone append completions cannot be supported
			 * as the BIO fragments may end up not being written
			 * sequentially.
			 */
			/* [한국어] Zone Append의 부분 완료는 원리적으로 지원할 수 없다.
			 * Zone Append(NVMe ZNS의 opcode 0x7D)는 "쓸 위치를 장치가 정해
			 * 알려주는" 연산인데, 요청이 쪼개져 부분 완료되면 각 조각이 서로
			 * 다른(그리고 연속이 아닐 수 있는) 위치에 기록된다. 그러면 상위가
			 * 받은 시작 LBA 하나로 전체 데이터를 가리킬 수 없게 된다.
			 * 조용히 틀린 결과를 주는 것보다 명시적 오류가 낫기에
			 * BLK_STS_IOERR로 강제 실패시킨다. */
			bio->bi_status = BLK_STS_IOERR;
		}

		/* Completion has already been traced */
		/* [한국어] BIO_TRACE_COMPLETION을 지운다. 위에서 trace_block_rq_complete로
		 * request 단위 완료를 이미 기록했으므로, bio_endio()가 bio 단위로 다시
		 * 기록하면 같은 완료가 두 번 나타난다. blkparse 출력이 중복되는 것을 막는다. */
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);
		/* [한국어] request 수준의 "조용히" 요구를 bio에도 전파한다. bio_endio()
		 * 경로에서 별도의 오류 로그를 남기지 않게 한다. */
		if (unlikely(quiet))
			bio_set_flag(bio, BIO_QUIET);

		/* [한국어] bio의 이터레이터(bi_iter)를 bio_bytes만큼 전진시킨다.
		 * bi_sector가 증가하고 bi_size가 감소하며, bvec 인덱스와 오프셋도 함께
		 * 갱신된다. 부분 완료된 bio가 나중에 재제출되면 이 위치부터 이어서
		 * 처리되므로, 남은 부분의 PRP/SGL도 여기서부터 다시 만들어진다. */
		bio_advance(bio, bio_bytes);

		/* Don't actually finish bio if it's part of flush sequence */
		/* [한국어] bi_size가 0이 되었다 = 이 bio는 완전히 처리되었다. */
		if (!bio->bi_iter.bi_size) {
			/* [한국어] Zone Append였다면 장치가 실제로 기록한 위치를 bio에
			 * 반영해야 한다. request에 담겨 온 결과 LBA(NVMe CQ 엔트리의
			 * Command Specific 필드로 전달됨)를 bio->bi_iter.bi_sector에
			 * 써 넣어, 상위가 "어디에 쓰였는지"를 알 수 있게 한다. */
			if (blk_req_bio_is_zone_append(req, bio))
				blk_zone_append_update_request_bio(req, bio);
			/* [한국어] flush 시퀀스 중이 아니라면 지금 bio를 최종 완료시킨다.
			 * bi_end_io 콜백이 실행되어 파일시스템의 완료 핸들러나 DIO의
			 * 대기 프로세스 깨우기가 일어난다.
			 * flush 시퀀스(is_flush) 중이라면 끝내지 않는다 — 데이터는 전송되었어도
			 * 아직 POST_FLUSH가 남아 지속성이 보장되지 않았기 때문이다.
			 * blk-flush 상태 기계가 전 단계를 마친 뒤 직접 완료시킨다. */
			if (!is_flush)
				bio_endio(bio);
		}

		/* [한국어] 소비량을 누적하고 남은 배분량을 줄인다. */
		total_bytes += bio_bytes;
		nr_bytes -= bio_bytes;	/* [한국어] 남은 배분량에서 이번 bio 가 가져간 몫을 뺀다 — 0 이 되면 루프를 끝낸다 */

		/* [한국어] 요청받은 바이트를 다 배분했으면 종료. bio 사슬에 아직 bio가
		 * 남아 있어도 이번 완료의 몫은 여기까지다(부분 완료). */
		if (!nr_bytes)
			break;
	}

	/*
	 * completely done
	 */
	/* [한국어] bio 사슬이 완전히 비었다 = 이 request는 할 일이 끝났다. */
	if (!req->bio) {
		/*
		 * Reset counters so that the request stacking driver
		 * can find how many bytes remain in the request
		 * later.
		 */
		/* [한국어] 남은 길이를 0으로 명시한다. dm/md 같은 스택형 드라이버가
		 * blk_rq_bytes()로 "얼마나 남았는지"를 조회할 때 0을 보고 완료를
		 * 판단할 수 있게 하는 계약이다. */
		req->__data_len = 0;
		/* [한국어] false = "남은 데이터 없음". 호출자 blk_mq_end_request()가
		 * 이 값을 보고 __blk_mq_end_request()로 진행해 통계를 마감하고
		 * driver tag(NVMe CID)를 반납한다. */
		return false;
	}

	/* [한국어] 여기부터는 부분 완료 처리 — request가 살아남아 나머지를 처리한다. */

	/* [한국어] 남은 길이를 이번에 소비한 만큼 줄인다. blk_rq_bytes()가 이 값을
	 * 반환하므로, 이후 이 request를 다시 발행하면 남은 크기만큼만 전송된다. */
	req->__data_len -= total_bytes;

	/* update sector only for requests with clear definition of sector */
	/* [한국어] 시작 LBA를 소비한 만큼 앞으로 밀어, 다음 발행이 이어지는 위치에서
	 * 시작하게 한다. >> 9는 바이트를 512B 섹터로 변환하는 것이다.
	 * passthrough를 제외하는 이유: nvme-cli가 보낸 커맨드의 __sector는 LBA가
	 * 아니라 의미 없는 값이거나 커맨드 고유의 필드라, 임의로 더하면 손상된다. */
	if (!blk_rq_is_passthrough(req))
		req->__sector += total_bytes >> 9;

	/* mixed attributes always follow the first bio */
	/* [한국어] mixed merge 상태라면 request의 대표 failfast 값을 새로운 첫 bio의
	 * 것으로 갱신한다. 앞쪽 bio들이 완료되어 사슬의 머리가 바뀌었으므로,
	 * "첫 bio의 속성을 따른다"는 규약을 유지하려면 다시 복사해야 한다.
	 * 이걸 빼먹으면 이미 사라진 bio의 재시도 정책이 남은 데이터에 잘못 적용된다. */
	if (req->rq_flags & RQF_MIXED_MERGE) {
		/* [한국어] 기존 failfast 비트를 지우고 */
		req->cmd_flags &= ~REQ_FAILFAST_MASK;
		/* [한국어] 새 첫 bio의 비트로 교체한다. */
		req->cmd_flags |= req->bio->bi_opf & REQ_FAILFAST_MASK;
	}

	/* [한국어] RQF_SPECIAL_PAYLOAD인 request는 제외한다. 이 플래그는 "bio의
	 * 데이터가 아니라 드라이버가 따로 만든 특수 페이로드를 전송한다"는 뜻으로,
	 * discard의 DSM range 배열이 대표적이다. 그런 request의 bio는 실제 전송
	 * 버퍼가 아니므로 세그먼트를 다시 세면 엉뚱한 값이 나온다. */
	if (!(req->rq_flags & RQF_SPECIAL_PAYLOAD)) {
		/*
		 * If total number of sectors is less than the first segment
		 * size, something has gone terribly wrong.
		 */
		/* [한국어] 방어적 정합성 검사. request 전체 길이가 "현재 첫 bio의 길이"
		 * 보다 작다는 것은 있을 수 없는 상태다(전체는 부분보다 크거나 같아야 한다).
		 * 드라이버가 실제 전송량보다 큰 값을 완료 보고했을 때 발생할 수 있다. */
		if (blk_rq_bytes(req) < blk_rq_cur_bytes(req)) {
			/* [한국어] request의 플래그와 상태를 통째로 커널 로그에 덤프해
			 * 어느 드라이버가 규약을 어겼는지 추적할 단서를 남긴다. */
			blk_dump_rq_flags(req, "request botched");
			/* [한국어] 크래시 대신 값을 보정해 진행한다. 데이터는 이미 잘못되었을
			 * 가능성이 높지만, 여기서 커널 패닉을 내는 것보다 오류를 상위로
			 * 전달하며 살아남는 편이 진단에 유리하다. */
			req->__data_len = blk_rq_cur_bytes(req);
		}

		/* recalculate the number of segments */
		/* [한국어] 남은 bio들만으로 물리 세그먼트 수를 다시 센다. 앞부분이
		 * 완료되면서 bvec 일부가 소비되었으므로 기존 nr_phys_segments는 과대
		 * 계상 상태다. 이 값이 갱신되지 않으면 재발행 시 nvme_queue_rq()가
		 * 실제보다 많은 PRP/SGL 디스크립터를 준비하려 해 잘못된 커맨드가
		 * 만들어지거나 max_segments 검사에서 불필요하게 걸린다. */
		req->nr_phys_segments = blk_recalc_rq_segments(req);
	}

	/* [한국어] true = "아직 데이터가 남았다". 호출자는 request를 끝내지 않고,
	 * 스택형 드라이버라면 남은 부분을 다시 하위 장치로 발행한다. */
	return true;
}
EXPORT_SYMBOL_GPL(blk_update_request);

/*
 * [한국어]
 * blk_account_io_done - request가 완전히 끝났을 때 디스크 통계를 마감
 *
 * @req: 완료된 request
 * @now: 완료 시각(ns). 호출자가 한 번 측정해 공유한다.
 * @return: 없음
 *
 * blk_account_io_completion()이 "전송량"을 누적했다면, 이 함수는 "요청 하나가
 * 끝났다"는 사건을 기록한다. 세 가지를 갱신한다:
 *   ios[]      - 완료된 I/O 개수 → iostat의 r/s, w/s (즉 IOPS)
 *   nsecs[]    - 누적 서비스 시간 → iostat의 r_await, w_await 계산 근거
 *   in_flight  - 처리 중인 요청 수 감소 → iostat의 aqu-sz
 *
 * RQF_FLUSH_SEQ를 제외하는 이유는 위 영문 주석이 설명한다: blk-flush 상태
 * 기계가 만든 내부 flush request는 사용자가 발행한 I/O가 아니라 그것을
 * 처리하기 위한 보조 요청이다. 이를 따로 세면 같은 논리적 I/O가 두 번
 * 집계되어 IOPS가 부풀려진다. 원래 request만 세면 충분하다.
 *
 * 실행 컨텍스트: 완료 경로(하드 IRQ 또는 softirq/IPI 이후).
 *
 * 호출 체인:
 *   blk_mq_end_request → __blk_mq_end_request_acct → [blk_account_io_done]
 */
static inline void blk_account_io_done(struct request *req, u64 now)
{
	trace_block_io_done(req);

	/*
	 * Account IO completion.  flush_rq isn't accounted as a
	 * normal IO on queueing nor completion.  Accounting the
	 * containing request is enough.
	 */
	/* [한국어] RQF_IO_STAT 있고 RQF_FLUSH_SEQ 없는 일반 NVMe IO 만 통계 집계 */
	if ((req->rq_flags & (RQF_IO_STAT|RQF_FLUSH_SEQ)) == RQF_IO_STAT) {
		/* [한국어] 읽기/쓰기/discard/flush를 구분하는 통계 그룹 인덱스. */
		const int sgrp = op_stat_group(req_op(req));

		/* [한국어] per-CPU 통계 보호(preempt_disable 수준). */
		part_stat_lock();
		/* [한국어] io_ticks 갱신 — "장치가 바빴던 시간"을 밀리초 단위로 누적한다.
		 * iostat의 %util이 이 값에서 계산된다. 세 번째 인자 true는 "완료
		 * 시점"임을 뜻해, in_flight가 0이 되는 순간까지를 바쁜 구간으로 계산한다.
		 * NVMe처럼 큐 깊이가 깊은 장치에서 %util은 포화도를 뜻하지 않는다는
		 * 점에 주의해야 한다 — 커맨드 하나만 진행 중이어도 100%로 표시된다. */
		update_io_ticks(req->part, jiffies, true);
		/* [한국어] 완료된 I/O 개수를 센다. iostat의 r/s, w/s가 이 값이며,
		 * 곧 NVMe IOPS다. */
		part_stat_inc(req->part, ios[sgrp]);
		/* [한국어] nsecs[sgrp]: 완료까지 소요된 ns 누적 — NVMe IO latency 히스토그램 */
		part_stat_add(req->part, nsecs[sgrp], now - req->start_time_ns);
		/* [한국어] in_flight 감소: NVMe SQ 에서 완료된 CID 반영 */
		part_stat_local_dec(req->part,
				    in_flight[op_is_write(req_op(req))]);
		part_stat_unlock();	/* [한국어] per-CPU 완료 통계 갱신 종료 — preempt 재활성화 */
	}
}

/*
 * [한국어]
 * blk_rq_passthrough_stats - passthrough 명령을 디스크 통계에 넣어도 되는지 판단
 *
 * @req: 검사할 passthrough request
 * @return: true = 통계에 반영, false = 제외
 *
 * === 왜 이런 판단이 필요한가 ===
 * passthrough 명령(nvme-cli의 ioctl 등)은 원래 통계에 넣지 않는 것이 기본이다.
 * Identify나 Get Log Page 같은 관리 명령까지 "읽기 I/O"로 집계되면 iostat이
 * 실제 데이터 전송량을 왜곡해 보여주기 때문이다.
 * 그런데 nvme-cli로 대량의 읽기/쓰기를 수행하는 경우(예: 벤치마크 도구가
 * passthrough로 I/O를 내는 경우)에는 통계에 잡혀야 유용하다. 그래서
 * 큐 단위 옵션(/sys/block/nvme0n1/queue/iostats_passthrough)을 켜면
 * "데이터 전송처럼 보이는" 명령만 골라 집계한다.
 *
 * === 네 단계 필터 ===
 * 실제 데이터 접근인지 판별할 확실한 방법이 없으므로(커널은 벤더 고유
 * 명령이 무엇을 하는지 알 수 없다) 휴리스틱을 쓴다. 위 영문 주석이 그 한계를
 * 솔직히 밝히고 있다: "We don't know what a passthrough command does, but we
 * know the payload size and data direction."
 *
 * NVMe가 특별히 언급되는 이유: 대부분의 드라이버는 passthrough request에
 * bi_bdev를 설정하지 않는데, NVMe 드라이버는 설정한다. 그래서 이 기능이
 * 실질적으로 NVMe에서만 동작한다.
 *
 * 실행 컨텍스트: I/O 제출 경로(blk_account_io_start 안).
 *
 * 호출 체인:
 *   blk_mq_start_request → blk_account_io_start → [blk_rq_passthrough_stats]
 */
static inline bool blk_rq_passthrough_stats(struct request *req)
{
	struct bio *bio = req->bio;

	/* [한국어] 필터 1 — 큐에서 이 기능이 켜져 있는가.
	 * 기본값은 꺼짐이며, sysfs의 iostats_passthrough로 켤 수 있다. */
	if (!blk_queue_passthrough_stat(req->q))
		return false;

	/* Requests without a bio do not transfer data. */
	/* [한국어] 필터 2 — bio가 있는가. bio가 없다는 것은 데이터 버퍼가 없다는
	 * 뜻이고, 그런 명령(Flush, Format 등)은 전송량이 0이라 집계할 것이 없다. */
	if (!bio)
		return false;

	/*
	 * Stats are accumulated in the bdev, so must have one attached to a
	 * bio to track stats. Most drivers do not set the bdev for passthrough
	 * requests, but nvme is one that will set it.
	 */
	/* [한국어] 필터 3 — 어느 블록 장치의 통계에 넣을지 알 수 있는가.
	 * 통계는 block_device 단위로 누적되므로 대상이 없으면 기록할 곳이 없다.
	 * 영문 주석대로 NVMe 드라이버만 passthrough에서 bi_bdev를 채워 준다. */
	if (!bio->bi_bdev)
		return false;

	/*
	 * We don't know what a passthrough command does, but we know the
	 * payload size and data direction. Ensuring the size is aligned to the
	 * block size filters out most commands with payloads that don't
	 * represent sector access.
	 */
	/* [한국어] 필터 4 — 전송 크기가 논리 블록의 배수인가.
	 * 이것이 핵심 휴리스틱이다. 실제 데이터 읽기/쓰기라면 반드시 논리 블록
	 * (NVMe 의 논리 블록 크기 = 현재 선택된 LBA Format 의 LBADS 지수로
	 *  2^ds 바이트, include/linux/nvme.h 의 struct nvme_lbaf.ds — 보통
	 *  ds=9(512B) 또는 ds=12(4096B)) 단위여야 한다. 반면 Identify는
	 * 4096B로 우연히 맞을 수 있지만 Get Log Page나 벤더 명령은 임의 크기라
	 * 대개 걸러진다. 완벽하지는 않지만 "대부분(most commands)"을 거른다는
	 * 영문 주석의 표현이 이 한계를 정확히 인정하고 있다.
	 * AND 마스크로 나머지를 구하는 것은 블록 크기가 항상 2의 거듭제곱이라
	 * 가능한 최적화다. */
	if (blk_rq_bytes(req) & (bdev_logical_block_size(bio->bi_bdev) - 1))
		return false;
	/* [한국어] 네 필터를 모두 통과 — 데이터 접근으로 간주해 통계에 반영한다. */
	return true;
}

/*
 * [한국어]
 * blk_account_io_start - request가 장치로 나갈 때 디스크 통계를 시작
 *
 * @req: 발행 직전의 request
 * @return: 없음
 *
 * 이 함수가 하는 일은 세 가지다:
 *   1) 이 request를 통계 대상으로 표시(RQF_IO_STAT) — 완료 시점의 함수들이
 *      이 플래그를 보고 집계 여부를 결정하므로, 제출과 완료가 반드시 짝을
 *      이뤄야 in_flight가 어긋나지 않는다.
 *   2) 시작 시각 기록 — 완료 시각과의 차이가 곧 서비스 시간(iostat의 await).
 *   3) in_flight 증가와 io_ticks 갱신 — "지금부터 장치가 바쁘다"는 표시.
 *
 * passthrough를 조건부로 제외하는 정책은 blk_rq_passthrough_stats()가 판단한다.
 *
 * 실행 컨텍스트: 제출 경로. blk_mq_start_request()에서 호출되므로 드라이버로
 * 내려보내기 직전이다.
 *
 * 호출 체인:
 *   blk_mq_start_request → [blk_account_io_start]
 *     → blk_rq_passthrough_stats / update_io_ticks
 */
static inline void blk_account_io_start(struct request *req)
{
	trace_block_io_start(req);

	/* [한국어] IO 통계 미수집 queue (blk_queue_io_stat 미설정): account 생략 */
	if (!blk_queue_io_stat(req->q))
		return;
	/* [한국어] passthrough인데 위 휴리스틱을 통과하지 못하면 통계에서 제외한다.
	 * 일반 I/O(bio에서 만들어진 request)는 이 조건에 걸리지 않고 항상 집계된다. */
	if (blk_rq_is_passthrough(req) && !blk_rq_passthrough_stats(req))
		return;

	/* [한국어] "이 request는 통계 대상"이라고 표시한다. 완료 시점의
	 * blk_account_io_done()과 blk_account_io_completion()이 이 플래그를 보고
	 * 집계 여부를 결정하므로, 제출과 완료의 짝이 반드시 맞아야 한다.
	 * 표시하지 않으면 in_flight 증가와 감소가 어긋나 iostat이 영원히
	 * "처리 중"으로 표시된다. */
	req->rq_flags |= RQF_IO_STAT;
	/* [한국어] start_time_ns: NVMe 명령 제출 시각 — IO latency 측정 시작점 */
	req->start_time_ns = blk_time_get_ns();

	/*
	 * All non-passthrough requests are created from a bio with one
	 * exception: when a flush command that is part of a flush sequence
	 * generated by the state machine in blk-flush.c is cloned onto the
	 * lower device by dm-multipath we can get here without a bio.
	 */
	/* [한국어] 통계를 누적할 대상 block_device를 정한다. bio가 있으면 그
	 * bio가 향한 파티션(bi_bdev)에 기록해, 파티션별 통계가 정확히 분리된다. */
	if (req->bio)
		req->part = req->bio->bi_bdev;
	else	/* [한국어] bio 가 없는 request */
		/* [한국어] bio가 없는 예외 상황 — 위 영문 주석이 설명하는 유일한 경우다.
		 * blk-flush 상태 기계가 만든 flush 명령을 dm-multipath가 하위 장치로
		 * 복제할 때 bio 없는 request가 여기 도달할 수 있다. 그때는 파티션을
		 * 특정할 수 없으므로 디스크 전체(part0)에 기록한다. */
		req->part = req->q->disk->part0;

	/* [한국어] per-CPU 통계 보호. */
	part_stat_lock();
	/* [한국어] io_ticks 갱신. 세 번째 인자 false는 "제출 시점"을 뜻하며,
	 * 이때부터 장치가 바쁜 것으로 계산되기 시작한다. 완료 시점의 true 호출과
	 * 짝을 이뤄 바쁜 구간의 길이를 만든다. */
	update_io_ticks(req->part, jiffies, false);
	/* [한국어] in_flight 증가 — "지금 장치에서 처리 중인 요청 수". NVMe 관점에서는
	 * SQ에 제출되어 아직 CQ로 완료가 돌아오지 않은 커맨드 수에 해당한다.
	 * iostat의 aqu-sz(평균 큐 깊이)가 이 값에서 나온다.
	 * 읽기(0)/쓰기(1) 슬롯을 op_is_write()로 구분한다.
	 * _local_ 접두사는 원자적 연산 없이 per-CPU 카운터를 직접 조작한다는
	 * 뜻으로, 제출 경로의 비용을 최소화한다(읽을 때 전 CPU를 합산한다). */
	part_stat_local_inc(req->part, in_flight[op_is_write(req_op(req))]);
	part_stat_unlock();	/* [한국어] per-CPU in_flight 증가 구간 종료 — preempt 재활성화 */
}

/*
 * [한국어]
 * __blk_mq_end_request_acct - request 완료 시 세 종류의 계정 처리를 묶어 수행
 *
 * @rq:  완료된 request
 * @now: 완료 시각(ns). 호출자가 한 번 측정해 넘겨 여러 소비자가 공유한다.
 * @return: 없음
 *
 * @now를 인자로 받는 이유가 중요하다. 아래 세 소비자가 각자 시각을 읽으면
 * blk_time_get_ns()를 세 번 호출하게 되는데, 완료 경로는 IOPS만큼 실행되는
 * 극한 핫패스라 이 비용이 무시할 수 없다. 게다가 각자 다른 시각을 쓰면
 * 통계 간 미세한 불일치가 생긴다. 한 번 재서 공유하는 것이 빠르고 정확하다.
 *
 * 세 소비자:
 *   1) blk_stat_add            - 지연 시간 히스토그램. blk-wbt가 쓰기 지연을
 *      제어하고 blk-iolatency가 목표 지연을 지키는 근거가 된다.
 *   2) blk_mq_sched_completed_request - 스케줄러에게 완료 통지. mq-deadline이
 *      다음 배치를 시작하거나, BFQ가 서비스 시간을 청구하는 데 쓴다.
 *   3) blk_account_io_done     - /proc/diskstats 갱신(iostat의 근거).
 *
 * 실행 컨텍스트: 완료 경로(하드 IRQ 또는 softirq/IPI 이후).
 *
 * 호출 체인:
 *   blk_mq_end_request / blk_mq_end_request_batch
 *     → [__blk_mq_end_request_acct]
 */
static inline void __blk_mq_end_request_acct(struct request *rq, u64 now)
{
	/* [한국어] RQF_STATS는 "이 request의 지연 시간을 통계에 넣으라"는 표시다.
	 * QUEUE_FLAG_STATS가 켜진 큐(blk-wbt나 blk-iolatency가 활성화된 경우)에서만
	 * 설정되므로, 아무도 지연 통계를 필요로 하지 않으면 이 비용조차 들지 않는다. */
	if (rq->rq_flags & RQF_STATS)
		blk_stat_add(rq, now);

	/* [한국어] 스케줄러에 완료를 알린다. 스케줄러가 없으면(NVMe 기본 "none")
	 * 내부에서 즉시 반환하는 저비용 경로다. */
	blk_mq_sched_completed_request(rq, now);
	/* [한국어] 디스크 통계를 마감한다 — 완료 개수, 누적 지연, in_flight 감소. */
	blk_account_io_done(rq, now);
}

/*
 * [한국어]
 * __blk_mq_end_request - request 최종 완료 처리 (통계 수집 + free 또는 end_io 호출)
 *
 * @rq:    완료할 request
 * @error: NVMe CQ status → 블록 레이어 상태 코드 (BLK_STS_*)
 *
 * NVMe 컨트롤러가 CQ 에 완료 항목을 기록한 뒤 드라이버(nvme_complete_rq)가
 * 이 함수를 호출한다. 완료 흐름:
 * 1) 타임스탬프 필요 시 IO 통계(latency, iops) 갱신
 * 2) blk_mq_finish_request: scheduler/zone 정리
 * 3) end_io 콜백 호출(있을 경우) 또는 blk_mq_free_request 로 CID 반납
 * 실행 컨텍스트: softirq(BLOCK_SOFTIRQ) 또는 poll 컨텍스트.
 *
 * 호출 체인:
 *   nvme_irq / nvme_poll → nvme_complete_rq → blk_mq_end_request
 *     → blk_update_request → [__blk_mq_end_request] → blk_mq_free_request
 */
inline void __blk_mq_end_request(struct request *rq, blk_status_t error)
{
	/* [한국어] blk_mq_need_time_stamp: RQF_IO_STAT, RQF_STATS 중 하나라도 있으면 true.
	 * 타임스탬프 기반 latency 계산이 필요한 request 에만 비용을 지불한다. */
	if (blk_mq_need_time_stamp(rq))
		/* [한국어] blk_time_get_ns(): 완료 시각 측정 후 통계/histogram 업데이트 */
		__blk_mq_end_request_acct(rq, blk_time_get_ns());

	/* [한국어] scheduler finish_request + zone 완료 정리 */
	blk_mq_finish_request(rq);

	/* [한국어] end_io 콜백이 있으면 호출:
	 * - blk_rq_prep_clone (dm) 처럼 상위 레이어가 추가 처리를 원할 때 사용.
	 * - RQ_END_IO_FREE 반환 시 blk_mq_free_request 로 CID 반납.
	 * - RQ_END_IO_NONE 반환 시 소유권이 콜백에게 남아있음 (아직 사용 중). */
	if (rq->end_io) {
		/* [한국어] rq_qos_done: WBT/iolatency/iocost 에 완료 통보 */
		rq_qos_done(rq->q, rq);
		if (rq->end_io(rq, error, NULL) == RQ_END_IO_FREE)	/* [한국어] end_io 콜백을 실제로 호출하고, 반환값이 RQ_END_IO_FREE 인지 확인한다. error 는 콜백에 그대로 전달되어 상위(dm 등)가 재시도/실패를 판단한다 */
			/* [한국어] end_io 가 소유권을 반납했으므로 tag/CID 해제 */
			blk_mq_free_request(rq);
	} else {
		/* [한국어] end_io 없음: blk_mq_free_request 로 직접 CID 반납 */
		blk_mq_free_request(rq);
	}
}
EXPORT_SYMBOL(__blk_mq_end_request);

/*
 * [한국어]
 * blk_mq_end_request - NVMe 명령(CID) 전체 완료 처리 공개 API
 *
 * @rq:    완료할 request
 * @error: 완료 상태 (BLK_STS_OK = 0 정상, 그 외 오류)
 *
 * blk_update_request 로 request 의 모든 바이트를 완료 처리한 뒤
 * __blk_mq_end_request 로 통계/정리/반납을 수행한다.
 * blk_update_request 가 true 를 반환하면 아직 남은 데이터가 있다는 의미인데
 * 전체 바이트를 전달했으므로 이 경우는 버그다.
 * 실행 컨텍스트: softirq 또는 poll 컨텍스트.
 *
 * 호출 체인:
 *   nvme_complete_rq → [blk_mq_end_request] → __blk_mq_end_request
 */
void blk_mq_end_request(struct request *rq, blk_status_t error)
{
	/* [한국어] blk_rq_bytes(rq): 이 request 의 전체 바이트 수.
	 * blk_update_request 에 전체 바이트를 넘기면 false(더 이상 없음)가 반환돼야 함. */
	if (blk_update_request(rq, error, blk_rq_bytes(rq)))
		/* [한국어] 전체 바이트를 넘겼는데 true 반환 = 버그 (남은 데이터가 있음) */
		BUG();
	/* [한국어] 통계 갱신 + scheduler/zone 정리 + CID 반납 */
	__blk_mq_end_request(rq, error);
}
EXPORT_SYMBOL(blk_mq_end_request);

/* [한국어] TAG_COMP_BATCH: 한 번에 batch 처리할 최대 tag 수 (= 32).
 * blk_mq_flush_tag_batch 에서 hctx 의 active count 와 q_usage_counter 를
 * 32개씩 묶어 한 번에 감소시켜 atomic 오버헤드를 줄인다. */
#define TAG_COMP_BATCH		32

/*
 * [한국어]
 * blk_mq_flush_tag_batch - 여러 tag(CID)를 한 번에 반납하고 카운터를 일괄 감소
 *
 * @hctx:      tag 가 속한 하드웨어 큐
 * @tag_array: 반납할 tag 번호 배열
 * @nr_tags:   반납할 tag 수 (최대 TAG_COMP_BATCH)
 *
 * blk_mq_end_request_batch 에서 호출. TAG_COMP_BATCH 단위로 모아
 * blk_mq_put_tags 로 sbitmap 에 한 번에 돌려주고,
 * percpu_ref_put_many 로 q_usage_counter 를 nr_tags 만큼 일괄 감소시킨다.
 * 실행 컨텍스트: softirq(BLOCK_SOFTIRQ) 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_end_request_batch → [blk_mq_flush_tag_batch]
 */
static inline void blk_mq_flush_tag_batch(struct blk_mq_hw_ctx *hctx,
					  int *tag_array, int nr_tags)
{
	/* [한국어] hctx 의 request_queue 역참조 (q_usage_counter 감소에 필요) */
	struct request_queue *q = hctx->queue;

	/* [한국어] blk_mq_sub_active_requests: hctx->active_requests 를 nr_tags 만큼 감소.
	 * 이 값이 0이 되면 dispatch budget 여유가 생겨 run_hw_queue 를 유발할 수 있다. */
	blk_mq_sub_active_requests(hctx, nr_tags);

	/* [한국어] blk_mq_put_tags: sbitmap 에서 tag_array 의 각 태그 비트를 batch 로 clear.
	 * 각 비트 해제마다 해당 wq 를 깨워 대기 submitter 가 tag 를 재획득할 수 있게 한다. */
	blk_mq_put_tags(hctx->tags, tag_array, nr_tags);
	/* [한국어] percpu_ref_put_many: q_usage_counter 를 nr_tags 만큼 감소.
	 * 한 번에 여러 개를 감소시켜 percpu_ref atomic 비용을 분산시킨다. */
	percpu_ref_put_many(&q->q_usage_counter, nr_tags);
}

/*
 * [한국어]
 * blk_mq_end_request_batch - IO 완료 배치(io_comp_batch)의 모든 request 를 일괄 처리
 *
 * @iob: 완료된 request 목록 (NVMe poll/irq 에서 CQ 항목 여러 개를 담은 배치)
 *
 * NVMe 인터럽트 핸들러가 한 번의 인터럽트에서 여러 CQ 항목을 처리할 때
 * 각 request 를 개별적으로 end_request 하는 대신 배치로 처리해 성능을 높인다.
 * tag 반납은 TAG_COMP_BATCH(32) 단위로 묶어 blk_mq_flush_tag_batch 로 한 번에 처리.
 * 실행 컨텍스트: softirq(BLOCK_SOFTIRQ) 또는 nvme_irq 컨텍스트.
 *
 * 호출 체인:
 *   nvme_irq / nvme_poll → nvme_poll_cq → nvme_handle_cqe
 *     → blk_mq_add_to_batch(req, iob, ..., nvme_pci_complete_batch)  [적재]
 *   nvme_irq → nvme_pci_complete_batch(&iob) → blk_mq_end_request_batch  [flush]
 *     → [blk_mq_end_request_batch]
 *
 * NVMe PCIe 의 실제 배치 구조(drivers/nvme/host/pci.c 에서 확인):
 * nvme_irq 가 스택에 DEFINE_IO_COMP_BATCH(iob) 를 만들고, nvme_poll_cq 가
 * CQ 를 드레인하며 CQE 하나마다 nvme_handle_cqe 를 부른다. 각 요청은
 * blk_mq_add_to_batch() 로 iob->req_list 에 쌓이고, CQ 드레인이 끝나면
 * nvme_pci_complete_batch(&iob) 가 한 번에 DMA unmap 후 이 함수를 부른다.
 * 즉 CQ 인터럽트 1회당 tag 반납/통계/bio 완료가 배치로 한 번에 일어난다.
 */
void blk_mq_end_request_batch(struct io_comp_batch *iob)
{
	/* [한국어] tags[]: hctx 가 동일한 request 들의 tag 를 모아두는 배열.
	 * hctx 가 바뀌면 먼저 flush 후 새 배열에 누적한다. */
	int tags[TAG_COMP_BATCH], nr_tags = 0;
	/* [한국어] cur_hctx: 현재 누적 중인 hctx — 다른 hctx 로 바뀌면 flush 트리거 */
	struct blk_mq_hw_ctx *cur_hctx = NULL;
	struct request *rq;	/* [한국어] 루프에서 하나씩 꺼낸 request 를 담을 커서 */
	/* [한국어] now: 타임스탬프 수집 시 배치 내 모든 request 에 동일한 완료 시각 사용 */
	u64 now = 0;

	/* [한국어] iob->need_ts: 배치 내 어느 request 라도 타임스탬프가 필요하면 미리 기록 */
	if (iob->need_ts)
		now = blk_time_get_ns();

	while ((rq = rq_list_pop(&iob->req_list)) != NULL) {	/* [한국어] iob 의 완료 리스트에서 request 를 하나씩 꺼내 리스트가 빌 때까지 반복. rq_list_pop 은 head 를 떼어 내며 리스트를 전진시킨다 */
		/* [한국어] 다음 반복에서 사용할 rq->bio 와 rq_next 를 캐시에 미리 로드.
		 * NVMe high-IOPS 에서 cache miss 를 숨기는 핵심 최적화. */
		prefetch(rq->bio);
		prefetch(rq->rq_next);	/* [한국어] 다음 반복에서 볼 rq->rq_next 도 미리 캐시에 적재 — 링크드 리스트 순회의 포인터 체이싱 지연을 감춘다 */

		/* [한국어] blk_complete_request: request 에 연결된 bio chain 을 모두 완료
		 * bio_endio 로 상위(파일시스템/VFS) 에 결과를 전달한다. */
		blk_complete_request(rq);
		/* [한국어] 타임스탬프가 필요한 배치면 각 request 의 IO 통계 갱신 */
		if (iob->need_ts)
			__blk_mq_end_request_acct(rq, now);

		/* [한국어] scheduler finish_request + zone 완료 정리 */
		blk_mq_finish_request(rq);

		/* [한국어] rq_qos_done: WBT/iolatency/iocost 완료 통보 */
		rq_qos_done(rq->q, rq);

		/*
		 * If end_io handler returns NONE, then it still has
		 * ownership of the request.
		 */
		/* [한국어] end_io 콜백이 있고 NONE 을 반환하면 소유권이 콜백에 있음 → skip.
		 * iob 를 전달하여 콜백이 배치 완료 후 처리할 수 있게 한다. */
		if (rq->end_io && rq->end_io(rq, 0, iob) == RQ_END_IO_NONE)
			continue;

		/* [한국어] state = MQ_RQ_IDLE: 이 CID 가 이제 재할당 가능함을 원자적으로 표시 */
		WRITE_ONCE(rq->state, MQ_RQ_IDLE);
		/* [한국어] 참조 카운트가 0이 되지 않았으면 아직 다른 참조자가 있음 → tag 반납 미뤄짐 */
		if (!req_ref_put_and_test(rq))
			continue;

		blk_crypto_free_request(rq);	/* [한국어] inline 암호화를 썼다면 keyslot 을 반납. 안 썼으면 no-op */
		blk_pm_mark_last_busy(rq);	/* [한국어] runtime PM 의 "마지막 사용 시각"을 갱신 — 이 값이 오래되면 장치를 autosuspend 시킬 수 있다 */

		/* [한국어] batch 버퍼가 가득 찼거나(TAG_COMP_BATCH=32) hctx 가 바뀌면
		 * 지금까지 모은 tag 들을 한 번에 반납(flush)한다.
		 * hctx 가 다르면 같은 tags sbitmap 에 속하지 않으므로 반드시 분리 flush. */
		if (nr_tags == TAG_COMP_BATCH || cur_hctx != rq->mq_hctx) {
			if (cur_hctx)
				/* [한국어] 지금까지 누적된 tag 배열을 hctx 단위로 일괄 반납 */
				blk_mq_flush_tag_batch(cur_hctx, tags, nr_tags);
			/* [한국어] 배열 초기화 후 새 hctx 시작 */
			nr_tags = 0;
			cur_hctx = rq->mq_hctx;	/* [한국어] 새 hctx 로 누적 대상을 전환. 다음 반복부터는 이 hctx 의 tags sbitmap 에 모은다 */
		}
		/* [한국어] 이 request 의 driver tag(CID)를 batch 버퍼에 추가 */
		tags[nr_tags++] = rq->tag;
	}

	/* [한국어] 루프 종료 후 남은 tag 들을 최종 flush */
	if (nr_tags)
		blk_mq_flush_tag_batch(cur_hctx, tags, nr_tags);
}
EXPORT_SYMBOL_GPL(blk_mq_end_request_batch);

/*
 * [한국어]
 * blk_complete_reqs - llist 에 모인 완료 request 들을 드라이버 complete 콜백으로 처리
 *
 * @list: 처리할 per-CPU llist (blk_cpu_done 또는 핫플러그 dead CPU 의 것)
 *
 * llist_del_all 로 리스트를 원자적으로 비우고, llist_reverse_order 로
 * 삽입 순서대로 뒤집은 뒤 각 request 의 mq_ops->complete 콜백을 호출한다.
 * complete 는 nvme_complete_rq 등 드라이버가 등록한 함수이며,
 * 최종적으로 blk_mq_end_request 를 호출해 상위로 완료를 전달한다.
 * 실행 컨텍스트: BLOCK_SOFTIRQ 또는 CPU hotplug 핸들러.
 *
 * 호출 체인:
 *   blk_done_softirq → [blk_complete_reqs] → nvme_complete_rq → blk_mq_end_request
 */
static void blk_complete_reqs(struct llist_head *list)
{
	/* [한국어] llist_del_all: 리스트 전체를 원자적으로 분리 (다른 CPU 의 push 와 race 없음).
	 * llist_reverse_order: llist 는 LIFO 이므로 FIFO 순서로 뒤집어 삽입 순서 복원. */
	struct llist_node *entry = llist_reverse_order(llist_del_all(list));
	struct request *rq, *next;	/* [한국어] rq = 지금 처리할 request, next = llist_for_each_entry_safe 가 순회 중 rq 를 완료(해제)해도 안전하도록 미리 잡아 두는 다음 노드 */

	/* [한국어] ipi_list: blk_mq_complete_send_ipi 에서 llist_add 로 연결한 노드.
	 * 각 request 의 드라이버 complete 콜백을 호출해 최종 blk_mq_end_request 진행. */
	llist_for_each_entry_safe(rq, next, entry, ipi_list)
		rq->q->mq_ops->complete(rq); /* nvme_complete_rq 로 CQ 항목 처리 */
}

/*
 * [한국어]
 * blk_done_softirq - BLOCK_SOFTIRQ 핸들러: 이 CPU 의 완료 큐를 처리
 *
 * BLOCK_SOFTIRQ 가 raise 되면 이 함수가 호출된다. 현재 CPU 의 blk_cpu_done
 * llist 에 쌓인 모든 완료 request 를 blk_complete_reqs 로 처리한다.
 * __latent_entropy: 엔트로피 취약점 완화를 위한 컴파일러 속성.
 * 실행 컨텍스트: BLOCK_SOFTIRQ 컨텍스트 (인터럽트 비활성 하에서 실행).
 *
 * 호출 체인:
 *   raise_softirq(BLOCK_SOFTIRQ) → [blk_done_softirq] → blk_complete_reqs
 */
static __latent_entropy void blk_done_softirq(void)
{
	/* [한국어] this_cpu_ptr(blk_cpu_done): 현재 CPU 에 할당된 완료 llist 포인터.
	 * blk_mq_complete_send_ipi 가 llist_add 로 추가한 request 들을 여기서 처리. */
	blk_complete_reqs(this_cpu_ptr(&blk_cpu_done));
}

/*
 * [한국어]
 * blk_softirq_cpu_dead - CPU 오프라인 시 해당 CPU 의 완료 큐를 현재 CPU 에서 처리
 *
 * @cpu: 오프라인된 CPU 번호
 * @return: 0 (항상 성공)
 *
 * CPU 핫플러그 오프라인 이벤트에서 호출된다. 오프라인 CPU 의 blk_cpu_done llist 에
 * 아직 처리되지 않은 완료 request 가 있을 수 있으므로 현재 CPU 에서 drain 한다.
 * 실행 컨텍스트: CPU 핫플러그 notifier.
 *
 * 호출 체인:
 *   CPU offline notifier → [blk_softirq_cpu_dead] → blk_complete_reqs
 */
static int blk_softirq_cpu_dead(unsigned int cpu)
{
	/* [한국어] per_cpu(blk_cpu_done, cpu): 오프라인된 CPU 의 완료 llist 처리 */
	blk_complete_reqs(&per_cpu(blk_cpu_done, cpu));
	return 0;
}

/*
 * [한국어]
 * __blk_mq_complete_request_remote - 원격 CPU 에서 BLOCK_SOFTIRQ 를 raise
 *
 * @data: IPI smp_call_function_single_async 에서 전달된 CSD 데이터 (미사용)
 *
 * smp_call_function_single_async 의 콜백 함수. 원래 요청 CPU 에서 IPI 를 받아
 * BLOCK_SOFTIRQ 를 발사(raise)하면 blk_done_softirq 가 실행된다.
 * 실행 컨텍스트: IPI 핸들러 (인터럽트 컨텍스트).
 *
 * 호출 체인:
 *   smp_call_function_single_async → [__blk_mq_complete_request_remote]
 *     → raise_softirq(BLOCK_SOFTIRQ) → blk_done_softirq
 */
static void __blk_mq_complete_request_remote(void *data)
{
	/* [한국어] __raise_softirq_irqoff: 인터럽트가 이미 비활성화된 컨텍스트에서
	 * BLOCK_SOFTIRQ 를 발사한다. 이후 인터럽트 복원 시 softirq 가 처리된다. */
	__raise_softirq_irqoff(BLOCK_SOFTIRQ);
}

/*
 * [한국어]
 * blk_mq_complete_need_ipi - 완료를 원래 요청 CPU 로 보내야 하는지 판단
 *
 * @rq: 완료 처리할 request
 * @return: true 이면 IPI 로 원래 CPU 에서 완료 처리, false 이면 현재 CPU 에서 처리
 *
 * QUEUE_FLAG_SAME_COMP 가 설정된 큐(NVMe 등)에서 완료 인터럽트가 다른 CPU 에서
 * 발생했을 때 원래 요청 CPU 로 완료를 "귀환"시킬지 결정한다.
 * 같은 CPU 이거나, 같은 캐시 도메인이고 capacity 가 같으면 현재 CPU 에서 처리해도
 * cache locality 가 충분하다. 오프라인 CPU 로는 IPI 불가.
 * 실행 컨텍스트: 인터럽트 핸들러(nvme_irq 등).
 *
 * 호출 체인:
 *   blk_mq_complete_request_remote → [blk_mq_complete_need_ipi]
 */
static inline bool blk_mq_complete_need_ipi(struct request *rq)
{
	/* [한국어] 현재 완료 처리 중인 CPU */
	int cpu = raw_smp_processor_id();

	/* [한국어] SMP 비활성 또는 SAME_COMP 플래그 없으면 IPI 불필요 — 현재 CPU 에서 처리 */
	if (!IS_ENABLED(CONFIG_SMP) ||
	    !test_bit(QUEUE_FLAG_SAME_COMP, &rq->q->queue_flags))
		return false;
	/*
	 * With force threaded interrupts enabled, raising softirq from an SMP
	 * function call will always result in waking the ksoftirqd thread.
	 * This is probably worse than completing the request on a different
	 * cache domain.
	 */
	/* [한국어] force_irqthreads: 소프트IRQ 가 스레드로 처리되어 오히려 느릴 수 있음 → IPI 포기 */
	if (force_irqthreads())
		return false;

	/* same CPU or cache domain and capacity?  Complete locally */
	/* [한국어] 원래 요청 CPU 와 현재 CPU 가 같으면 IPI 불필요 (이미 같은 CPU).
	 * SAME_FORCE 가 없고 캐시 공유 + capacity 동일이면 locality 충분 → 현재 CPU 처리. */
	if (cpu == rq->mq_ctx->cpu ||
	    (!test_bit(QUEUE_FLAG_SAME_FORCE, &rq->q->queue_flags) &&
	     cpus_share_cache(cpu, rq->mq_ctx->cpu) &&
	     cpus_equal_capacity(cpu, rq->mq_ctx->cpu)))
		return false;

	/* don't try to IPI to an offline CPU */
	/* [한국어] 원래 요청 CPU 가 온라인인 경우에만 IPI 가능; 오프라인이면 현재 CPU 처리 */
	return cpu_online(rq->mq_ctx->cpu);
}

/*
 * [한국어]
 * blk_mq_complete_send_ipi - 완료를 원래 요청 CPU 로 IPI 를 통해 전달
 *
 * @rq: 완료 처리할 request (rq->mq_ctx->cpu 가 대상 CPU)
 *
 * rq 를 대상 CPU 의 blk_cpu_done llist 에 추가하고, 리스트가 비어있었다면
 * (이전에 추가된 request 가 없었다면) smp_call_function_single_async 로
 * 해당 CPU 에 IPI 를 보내 BLOCK_SOFTIRQ 를 raise 하게 한다.
 * 이미 리스트에 항목이 있다면 IPI 는 이미 보냈으므로 추가 IPI 불필요.
 * 실행 컨텍스트: 인터럽트 핸들러.
 *
 * 호출 체인:
 *   blk_mq_complete_request_remote → [blk_mq_complete_send_ipi]
 *     → smp_call_function_single_async → __blk_mq_complete_request_remote
 *     → raise_softirq(BLOCK_SOFTIRQ) → blk_done_softirq
 */
static void blk_mq_complete_send_ipi(struct request *rq)
{
	/* [한국어] 원래 요청 CPU 번호 (rq->mq_ctx 는 submit 한 CPU 의 sw 큐) */
	unsigned int cpu;

	cpu = rq->mq_ctx->cpu;	/* [한국어] 완료를 되돌려 보낼 목적지 CPU — bio 를 제출했던 그 CPU 다. 그 CPU 의 캐시에 request/bio 가 아직 살아 있을 가능성이 높다 */
	/* [한국어] llist_add: 대상 CPU 의 blk_cpu_done 에 rq 를 원자적으로 추가.
	 * 반환값 true = 리스트가 비어있었음 → 이 추가가 처음이므로 IPI 필요.
	 * false = 이미 항목이 있었음 → IPI 는 이미 진행 중이므로 추가 불필요. */
	if (llist_add(&rq->ipi_list, &per_cpu(blk_cpu_done, cpu)))
		/* [한국어] smp_call_function_single_async: 지정 CPU 에 비동기 IPI 전송.
		 * 콜백(blk_cpu_csd 의 func)이 그 CPU 에서 __blk_mq_complete_request_remote 를 실행.
		 * async 이므로 현재 CPU 는 블록 없이 즉시 반환한다. */
		smp_call_function_single_async(cpu, &per_cpu(blk_cpu_csd, cpu));
}

/*
 * [한국어]
 * blk_mq_raise_softirq - 현재 CPU 의 blk_cpu_done 에 rq 를 추가하고 BLOCK_SOFTIRQ raise
 *
 * @rq: softirq 에서 완료 처리할 request
 *
 * 현재 CPU 에서 바로 softirq 를 통해 완료를 처리할 때 사용.
 * llist_add 로 blk_cpu_done 에 추가하고 리스트가 비었었다면(처음 추가)
 * raise_softirq 로 BLOCK_SOFTIRQ 를 발사한다.
 * preempt_disable/enable 으로 CPU 전환을 막아 this_cpu_ptr 안전성 보장.
 * 실행 컨텍스트: 인터럽트 핸들러.
 *
 * 호출 체인:
 *   blk_mq_complete_request_remote → [blk_mq_raise_softirq]
 *     → raise_softirq(BLOCK_SOFTIRQ) → blk_done_softirq
 */
static void blk_mq_raise_softirq(struct request *rq)
{
	struct llist_head *list;

	/* [한국어] preempt_disable: this_cpu_ptr 호출 동안 CPU 를 고정.
	 * CPU 가 바뀌면 다른 CPU 의 리스트에 추가될 수 있으므로 방지. */
	preempt_disable();
	/* [한국어] 현재 CPU 의 완료 llist 포인터 획득 */
	list = this_cpu_ptr(&blk_cpu_done);
	/* [한국어] llist_add: request 를 llist 에 lock-free 로 추가.
	 * 반환 true = 리스트가 처음으로 비어있지 않게 됨 → softirq 발사 필요. */
	if (llist_add(&rq->ipi_list, list))
		/* [한국어] raise_softirq(BLOCK_SOFTIRQ): blk_done_softirq 를 예약.
		 * 인터럽트 복원 후 소프트IRQ 체크 시점에 blk_done_softirq 가 실행됨. */
		raise_softirq(BLOCK_SOFTIRQ);
	/* [한국어] 프리엠션 복원 */
	preempt_enable();
}

/*
 * [한국어]
 * blk_mq_complete_request_remote - NVMe 완료를 원래 CPU 로 라우팅할지 결정
 *
 * @rq: 완료 처리할 request
 * @return: true 이면 IPI/softirq 로 완료를 다른 경로에 위임했음, false 이면 직접 처리
 *
 * NVMe CQ 인터럽트가 발생한 CPU 와 bio 를 submit 한 CPU 가 다를 경우,
 * cache affinity 를 높이기 위해 완료를 원래 요청 CPU 로 돌려보낸다.
 * 라우팅 결정 기준:
 * 1) ctx가 1개이고 같은 CPU면 또는 polled request → 현재 CPU 에서 직접 처리 (false)
 * 2) blk_mq_complete_need_ipi: 다른 CPU, 다른 캐시 도메인 → IPI 전송 (true)
 * 3) nr_hw_queues == 1: softirq 로 현재 CPU 에서 처리 (true)
 * 4) 기타 → false (호출자가 직접 처리)
 * 실행 컨텍스트: 인터럽트 핸들러(nvme_irq) 또는 폴링 컨텍스트(nvme_poll).
 *
 * 호출 체인 (NVMe PCIe 실물 확인):
 *   nvme_irq / nvme_poll → nvme_poll_cq → nvme_handle_cqe
 *     → nvme_try_complete_req (drivers/nvme/host/nvme.h)
 *       → [blk_mq_complete_request_remote] → blk_mq_complete_send_ipi 또는 false
 *
 * 주의: NVMe PCIe 핫패스는 blk_mq_complete_request() 래퍼를 거치지 않고
 * 이 함수를 **직접** 호출한다. 반환값이 false 면 nvme_handle_cqe 가 이어서
 * blk_mq_add_to_batch() 또는 nvme_pci_complete_rq() 로 스스로 완료를 마친다.
 * blk_mq_complete_request() 쪽은 NVMe 에서는 취소 경로(nvme_cancel_request)
 * 에서만 쓰인다.
 */
bool blk_mq_complete_request_remote(struct request *rq)
{
	/* [한국어] WRITE_ONCE: state 를 MQ_RQ_COMPLETE 로 원자적 변경.
	 * poll loop 에서 이 state 를 보고 완료 여부를 판단한다. */
	WRITE_ONCE(rq->state, MQ_RQ_COMPLETE);

	/*
	 * For request which hctx has only one ctx mapping,
	 * or a polled request, always complete locally,
	 * it's pointless to redirect the completion.
	 */
	/* [한국어] nr_ctx == 1 이고 현재 CPU 가 그 ctx 의 CPU 이면 IPI 불필요.
	 * REQ_POLLED: poll 요청은 호출자가 직접 완료를 확인하므로 라우팅 불필요. */
	if ((rq->mq_hctx->nr_ctx == 1 &&
	     rq->mq_ctx->cpu == raw_smp_processor_id()) ||
	     rq->cmd_flags & REQ_POLLED)
		return false;

	/* [한국어] cache affinity 를 위해 원래 요청 CPU 로 IPI 를 통해 완료 전달 */
	if (blk_mq_complete_need_ipi(rq)) {
		blk_mq_complete_send_ipi(rq);
		return true;
	}

	/* [한국어] nr_hw_queues == 1: 단일 SQ 큐 — IPI 대신 로컬 softirq 로 처리.
	 * 대부분의 SCSI HBA나 virtio-blk 등이 이 경로를 사용한다. */
	if (rq->q->nr_hw_queues == 1) {
		blk_mq_raise_softirq(rq);
		return true;
	}
	/* [한국어] 위 조건 모두 해당 없음: 호출자가 mq_ops->complete 로 직접 처리 */
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
 * [한국어]
 * blk_mq_complete_request - 드라이버가 호출하는 NVMe 완료 상위 API
 *
 * @rq: 완료된 request
 *
 * blk_mq_complete_request_remote 로 완료 라우팅을 시도한다:
 * - false 반환(현재 CPU 에서 처리): mq_ops->complete(rq) 를 직접 호출
 * - true 반환(IPI/softirq 로 위임): complete 는 원래 CPU 에서 비동기 실행
 * 실행 컨텍스트: 인터럽트 핸들러 또는 poll 컨텍스트.
 *
 * NVMe 접점 (실물 확인): NVMe PCIe 의 완료 핫패스는 이 래퍼를 쓰지 **않는다**.
 * nvme_try_complete_req() 가 blk_mq_complete_request_remote() 를 직접 부르고,
 * false 가 나오면 nvme_handle_cqe() 가 배치 적재 또는 nvme_pci_complete_rq()
 * 로 마무리한다(래퍼가 하는 일을 드라이버가 직접 펼쳐 놓은 형태).
 * NVMe 에서 이 함수를 쓰는 곳은 컨트롤러 리셋/연결 손실 시 인플라이트 명령을
 * 강제 종료하는 nvme_cancel_request() (core.c) 같은 취소 경로다.
 *
 * 호출 체인:
 *   드라이버 완료 경로 → [blk_mq_complete_request]
 *     → blk_mq_complete_request_remote → mq_ops->complete 또는 IPI/softirq
 */
void blk_mq_complete_request(struct request *rq)
{
	/* [한국어] blk_mq_complete_request_remote: 라우팅 필요시 IPI/softirq 로 위임.
	 * false 이면 현재 CPU 에서 즉시 드라이버 complete 콜백 실행. */
	if (!blk_mq_complete_request_remote(rq)) /* 완료 CPU 라우팅 결정 */
		/* [한국어] mq_ops->complete: nvme_complete_rq 등 드라이버 등록 완료 함수.
		 * 최종적으로 blk_mq_end_request 를 호출해 tag/CID 반납까지 처리. */
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
 * [한국어]
 * blk_mq_start_request - 드라이버가 request 를 SQ 에 투입하기 직전에 호출
 *
 * @rq: 시작할 request
 *
 * 이 함수가 호출된 후에 드라이버(nvme_queue_rq)는 SQ Entry 를 기록하고 doorbell 을 울린다.
 * 주요 동작:
 * 1) trace_block_rq_issue: perf/ftrace 에 request 발행 이벤트 기록
 * 2) io_start_time_ns 기록 + rq_qos_issue: QoS 정책에 발행 통보
 * 3) blk_add_timer: 타임아웃 타이머 시작 (nvme timeout handler 의 근거)
 * 4) state = MQ_RQ_IN_FLIGHT: 이제 이 CID 는 "처리 중" 상태
 * 5) tags->rqs[tag] = rq: CID 로 request 역참조 가능하게 등록
 *    (nvme 완료 인터럽트에서 CID → request 를 찾는 경로)
 * 실행 컨텍스트: 프로세스 컨텍스트 (dispatch 경로, nvme_queue_rq 에서 직접 호출).
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list → nvme_queue_rq → [blk_mq_start_request]
 *     → blk_add_timer / state MQ_RQ_IN_FLIGHT 설정
 */
void blk_mq_start_request(struct request *rq)
{
	/* [한국어] q: 통계 플래그 확인 및 QoS issue 통보에 사용 */
	struct request_queue *q = rq->q;

	/* [한국어] trace_block_rq_issue: blktrace/ftrace "D" (driver) 이벤트 기록.
	 * io_uring, fio 등의 latency 추적에서 이 이벤트가 드라이버 처리 시작점이 됨. */
	trace_block_rq_issue(rq);

	/* [한국어] QUEUE_FLAG_STATS: wbt(writeback throttle), iolatency 등의
	 * 통계 수집이 활성화된 큐. passthrough(NVMe admin 명령 등)는 제외. */
	if (test_bit(QUEUE_FLAG_STATS, &q->queue_flags) &&
	    !blk_rq_is_passthrough(rq)) {
		/* [한국어] io_start_time_ns: 드라이버 발행 시각 (latency 히스토그램 시작점) */
		rq->io_start_time_ns = blk_time_get_ns();
		/* [한국어] stats_sectors: 발행 당시의 sector 수 (부분 완료 추적용) */
		rq->stats_sectors = blk_rq_sectors(rq);
		/* [한국어] RQF_STATS: end_request 에서 blk_stat_add 를 트리거하도록 표시 */
		rq->rq_flags |= RQF_STATS;
		/* [한국어] rq_qos_issue: wbt/iolatency/iocost 에 request 발행 통보.
		 * wbt 는 여기서 발행 토큰을 소비한다. */
		rq_qos_issue(q, rq);
	}

	/* [한국어] 발행 시점에 state 가 MQ_RQ_IDLE 이어야 함 — 버그 감지 */
	WARN_ON_ONCE(blk_mq_rq_state(rq) != MQ_RQ_IDLE);

	/* [한국어] blk_add_timer: hrtimer 로 request 타임아웃을 예약.
	 * NVMe 타임아웃은 nvme_timeout 핸들러로 연결되어 abort/reset 을 시도. */
	blk_add_timer(rq); /* NVMe 명령 deadline 타이머 시작 */
	/* [한국어] WRITE_ONCE: state 를 MQ_RQ_IN_FLIGHT 로 원자적 전환.
	 * timeout handler 와 completion handler 모두 이 state 를 보고 처리 여부를 판단. */
	WRITE_ONCE(rq->state, MQ_RQ_IN_FLIGHT); /* NVMe 명령이 SQ 에 제출됨 */
	/* [한국어] tags->rqs[tag]: 드라이버 tag(CID) → request 역매핑 등록.
	 * NVMe 인터럽트 핸들러가 CQ Entry 의 CID 로 이 배열을 조회해 request 를 찾는다. */
	rq->mq_hctx->tags->rqs[rq->tag] = rq; /* CID 로 request 역참조 가능하게 매핑 */

	/* [한국어] NVMe Protection Information(PI): WRITE 명령에 대해
	 * T10 DIF/DIX 데이터를 bio 에 첨부하는 사전 준비 수행. */
	if (blk_integrity_rq(rq) && req_op(rq) == REQ_OP_WRITE)
		blk_integrity_prepare(rq);

	/* [한국어] REQ_POLLED: 인터럽트 없이 poll 하는 NVMe queue 에서 사용.
	 * bi_cookie 에 hctx->queue_num 을 기록해 poll 시 어느 CQ 를 확인할지 알린다. */
	if (rq->bio && rq->bio->bi_opf & REQ_POLLED)
	        WRITE_ONCE(rq->bio->bi_cookie, rq->mq_hctx->queue_num);
}
EXPORT_SYMBOL(blk_mq_start_request);

/*
 * Allow 2x BLK_MAX_REQUEST_COUNT requests on plug queue for multiple
 * queues. This is important for md arrays to benefit from merging
 * requests.
 */
/*
 * [한국어]
 * blk_plug_max_rq_count - plug 리스트의 최대 request 수 반환
 *
 * @plug: 대상 blk_plug
 * @return: 최대 허용 request 수
 *
 * 여러 큐(NVMe 여러 namespace 또는 md 등)에서 오는 request 가 섞이면
 * merge 기회를 높이기 위해 두 배로 늘린다.
 * BLK_MAX_REQUEST_COUNT 를 초과하면 flush 하여 batch dispatch 를 유발.
 */
static inline unsigned short blk_plug_max_rq_count(struct blk_plug *plug)
{
	/* [한국어] multiple_queues: 둘 이상의 request_queue 의 request 가 섞인 경우.
	 * md RAID 처럼 여러 NVMe namespace 에 걸친 IO 를 merge 하기 위해 2배 허용. */
	if (plug->multiple_queues)
		return BLK_MAX_REQUEST_COUNT * 2;
	return BLK_MAX_REQUEST_COUNT;	/* [한국어] 단일 큐면 기본 상한(BLK_MAX_REQUEST_COUNT)만 허용 — 너무 오래 모아 두면 지연이 커진다 */
}

/*
 * [한국어]
 * blk_add_rq_to_plug - request 를 plug 리스트에 추가 (배치 dispatch 대기열)
 *
 * @plug: 현재 태스크의 blk_plug (blk_start_plug 로 설정)
 * @rq:   plug 에 넣을 request
 *
 * "Plugging" 은 IO 를 바로 드라이버에 보내지 않고 누적했다가 한 번에 flush 하는
 * 배치 최적화다. NVMe 같이 고속 장치에서는 batch dispatch 가 doorbell 횟수를 줄인다.
 * 용량 초과 또는 큰 IO 가 쌓이면 blk_mq_flush_plug_list 로 즉시 dispatch 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → [blk_add_rq_to_plug]
 *   blk_finish_plug → blk_mq_flush_plug_list (unplug 시 전체 dispatch)
 */
static void blk_add_rq_to_plug(struct blk_plug *plug, struct request *rq)
{
	/* [한국어] 현재 plug 리스트의 마지막 request (merge/flush 조건 확인용) */
	struct request *last = rq_list_peek(&plug->mq_list);

	if (!plug->rq_count) {	/* [한국어] plug 가 비어 있으면 이번이 첫 request */
		/* [한국어] 첫 번째 request 추가: block_plug tracepoint 기록 */
		trace_block_plug(rq->q);
	} else if (plug->rq_count >= blk_plug_max_rq_count(plug) ||	/* [한국어] 이미 충분히 모였거나(상한 도달) 마지막 request 가 충분히 커진 경우 — 더 모으는 대신 지금 내보내는 편이 낫다 */
		   /* [한국어] nomerges 가 아니고 마지막 request 가 BLK_PLUG_FLUSH_SIZE
		    * 이상이면 이미 큰 IO 가 쌓였으므로 지금 flush 하는 것이 낫다. */
		   (!blk_queue_nomerges(rq->q) &&
		    blk_rq_bytes(last) >= BLK_PLUG_FLUSH_SIZE)) {
		/* [한국어] 현재 plug 를 flush: 쌓인 request 들을 모두 hctx 에 dispatch */
		blk_mq_flush_plug_list(plug, false);
		/* [한국어] flush 후 last 는 무효 — trace 를 새로 기록 */
		last = NULL;
		trace_block_plug(rq->q);	/* [한국어] flush 로 리스트가 비었으므로 새 plug 구간의 시작을 blktrace 에 다시 기록 */
	}

	/* [한국어] 서로 다른 request_queue 의 request 가 섞이면 multiple_queues 표시.
	 * 이후 blk_plug_max_rq_count 가 2배를 허용한다. */
	if (!plug->multiple_queues && last && last->q != rq->q)
		plug->multiple_queues = true;
	/*
	 * Any request allocated from sched tags can't be issued to
	 * ->queue_rqs() directly
	 */
	/* [한국어] sched_tags 를 사용하는 request 가 하나라도 있으면 has_elevator = true.
	 * flush_plug_list 에서 ->queue_rqs() 를 건너뛰고 elevator 경로를 사용한다. */
	if (!plug->has_elevator && (rq->rq_flags & RQF_SCHED_TAGS))
		plug->has_elevator = true;
	/* [한국어] plug 리스트 끝에 request 추가 (FIFO 순서 유지) */
	rq_list_add_tail(&plug->mq_list, rq);
	/* [한국어] plug 리스트의 총 request 수 증가 */
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
 * [한국어]
 * blk_execute_rq_nowait - passthrough request 를 큐에 비동기로 삽입
 *
 * @rq:      실행할 passthrough request
 *           (NVMe 라면 nvme_alloc_user_request() 또는 __nvme_submit_sync_cmd()
 *            가 blk_mq_alloc_request() 로 만들어 커맨드를 채워 둔 것)
 * @at_head: true 면 dispatch list 맨 앞에, false 면 맨 뒤에 삽입
 *
 * NVMe ioctl/admin 명령을 비동기로 발행하는 진입점.
 * scheduler 가 있으면 elevator 를 통해, 없으면 hctx->dispatch 리스트로 바로 간다.
 * plug 가 활성화된 경우 완료 콜백이 호출될 때까지 request 를 plug 에 축적한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl/admin 명령 제출 경로).
 *
 * 호출 체인:
 *   nvme_execute_rq 계열의 비동기 제출(예: nvme_uring_cmd_io, ioctl.c) 및
 *   nvme_keep_alive_work → [blk_execute_rq_nowait]
 *   → blk_mq_insert_request → blk_mq_run_hw_queue
 *   → (mq_ops->queue_rq == nvme_queue_rq)
 */
void blk_execute_rq_nowait(struct request *rq, bool at_head)
{
	/* [한국어] rq->mq_hctx: 이 request 가 발행될 NVMe SQ 에 대응하는 hctx */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	/* [한국어] 인터럽트 비활성 컨텍스트에서는 호출 불가 — spin 이 아닌 completion 대기 필요 */
	WARN_ON(irqs_disabled());
	/* [한국어] blk_rq_is_passthrough: NVMe passthrough 가 아닌 FS I/O request 는 금지 */
	WARN_ON(!blk_rq_is_passthrough(rq));

	/* [한국어] IO 통계 기록 시작 (iostat, blk-cgroup 계정) */
	blk_account_io_start(rq);

	if (current->plug && !at_head) {	/* [한국어] plug 가 열려 있고 head 삽입 요구가 아니면 — 우선순위가 없는 요청은 모아서 한 번에 내보내는 편이 doorbell/락 비용 면에서 유리하다 */
		/* [한국어] plug 가 열려 있으면 즉시 dispatch 하지 않고 plug 에 모아둔다.
		 * blk_finish_plug 시점에 blk_mq_flush_plug_list 가 일괄 dispatch 한다.
		 * at_head 요청은 plug 에 넣지 않고 바로 insert — 우선순위가 있기 때문. */
		blk_add_rq_to_plug(current->plug, rq);
		return;
	}

	/* [한국어] BLK_MQ_INSERT_AT_HEAD: at_head 시 hctx->dispatch 맨 앞에 삽입
	 * → dispatch 시 가장 먼저 nvme_queue_rq 로 전달됨 */
	blk_mq_insert_request(rq, at_head ? BLK_MQ_INSERT_AT_HEAD : 0);
	/* [한국어] BLK_MQ_F_BLOCKING: true 면 동기 run (blk_mq_run_hw_queue 내부에서 sleep 허용)
	 * false(일반 경우) 면 asyn kblockd workqueue 에 디스패치 */
	blk_mq_run_hw_queue(hctx, hctx->flags & BLK_MQ_F_BLOCKING);
}
EXPORT_SYMBOL_GPL(blk_execute_rq_nowait);

/*
 * [한국어]
 * struct blk_rq_wait - 동기 passthrough request 완료 대기 구조체
 *
 * blk_execute_rq 에서 스택에 할당하여 NVMe admin/passthrough 명령의
 * 동기 완료를 기다린다. blk_end_sync_rq 콜백이 done 을 signal 하면
 * 호출자가 깨어나 ret 으로 결과를 가져간다.
 */
struct blk_rq_wait {
	struct completion done;
	/* [한국어] NVMe admin 명령 완료를 기다리는 completion.
	 * 설정자: blk_end_sync_rq 가 complete(&wait->done) 으로 signal.
	 * 읽는 자: blk_execute_rq 가 blk_wait_io 로 대기.
	 * 초기화: COMPLETION_INITIALIZER_ONSTACK — 스택 변수이므로 정적 초기화 불가. */

	blk_status_t ret;
	/* [한국어] NVMe 명령의 최종 blk_status_t 결과 코드.
	 * 설정자: blk_end_sync_rq 가 completion 전에 저장.
	 * 읽는 자: blk_execute_rq 가 done 대기 후 이 값을 반환.
	 * 값 범위: BLK_STS_OK (성공), BLK_STS_IOERR, BLK_STS_TIMEOUT 등. */
};

/*
 * [한국어]
 * blk_end_sync_rq - 동기 passthrough request 의 end_io 콜백
 *
 * @rq:  완료된 NVMe passthrough request
 * @ret: NVMe CQ 에서 읽어온 완료 상태 (blk_status_t)
 * @iob: batch completion 컨텍스트 (이 함수에서는 사용 안 함)
 * @return: RQ_END_IO_NONE (request 를 free 하지 않음 — 호출자가 스택에서 관리)
 *
 * blk_execute_rq 가 rq->end_io 에 등록하는 콜백.
 * NVMe 인터럽트 완료 경로(blk_mq_end_request)에서 불린다.
 * completion 을 signal 하여 blk_execute_rq 의 대기를 깨운다.
 *
 * 호출 체인:
 *   nvme_irq → blk_mq_end_request → [blk_end_sync_rq]
 */
static enum rq_end_io_ret blk_end_sync_rq(struct request *rq, blk_status_t ret,
					  const struct io_comp_batch *iob)
{
	/* [한국어] rq->end_io_data: blk_execute_rq 가 스택에 생성한 blk_rq_wait 포인터 */
	struct blk_rq_wait *wait = rq->end_io_data;

	/* [한국어] 완료 상태를 대기 중인 호출자에게 전달 */
	wait->ret = ret;
	/* [한국어] complete: blk_execute_rq 의 blk_wait_io 를 깨움 */
	complete(&wait->done);
	/* [한국어] RQ_END_IO_NONE: request 를 여기서 free 하지 않음.
	 * blk_execute_rq 가 wait 를 읽은 뒤 호출자가 blk_mq_free_request 로 해제. */
	return RQ_END_IO_NONE;
}

/*
 * [한국어]
 * blk_rq_is_poll - request 가 poll (인터럽트리스) hctx 에 할당되었는지 확인
 *
 * @rq:    검사할 request
 * @return: true 이면 HCTX_TYPE_POLL 큐에 대기 중 — 폴링으로 완료 대기 가능
 *
 * NVMe 접점: nvme 모듈 파라미터 poll_queues > 0 이면 마지막 N 개의 I/O 큐가
 * MSI-X 벡터 없이 만들어지고(HCTX_TYPE_POLL 맵), 그 큐로 간 요청은 인터럽트가
 * 오지 않으므로 반드시 폴링으로 거둬야 한다. 요청이 이 맵으로 가려면 제출 측이
 * REQ_POLLED 를 세워야 하며(io_uring 은 링을 IORING_SETUP_IOPOLL 로 만들었을 때
 * 그렇게 한다), blk_mq_map_queue() 가 그 플래그를 보고 HCTX_TYPE_POLL 을 고른다.
 * 이후 blk_rq_poll → blk_hctx_poll → mq_ops->poll(== nvme_poll)이 CQ 를 읽는다.
 */
bool blk_rq_is_poll(struct request *rq)
{
	/* [한국어] mq_hctx 가 없으면 아직 hctx 에 할당되기 전 — poll 불가 */
	if (!rq->mq_hctx)
		return false;
	/* [한국어] HCTX_TYPE_POLL: NVMe 드라이버가 poll 전용으로 등록한 SQ 타입 */
	if (rq->mq_hctx->type != HCTX_TYPE_POLL)
		return false;
	return true;
}
EXPORT_SYMBOL_GPL(blk_rq_is_poll);

/*
 * [한국어]
 * blk_rq_poll_completion - poll hctx 의 request 완료를 폴링으로 대기
 *
 * @rq:   poll queue 에 발행된 request
 * @wait: 완료 시 signal 될 completion (blk_rq_wait.done)
 *
 * 인터럽트를 사용하지 않는 NVMe poll queue 에서 CQ 를 반복 조회한다.
 * io_uring 의 IORING_SETUP_IOPOLL 모드에서 사용.
 * cond_resched 로 CPU 점유를 제어해 완료 전에 다른 태스크 실행 기회 제공.
 */
static void blk_rq_poll_completion(struct request *rq, struct completion *wait)
{
	do {
		/* [한국어] blk_hctx_poll: NVMe poll SQ 의 CQ 를 한 번 조회 (BLK_POLL_ONESHOT)
		 * 완료된 CQ Entry 가 있으면 blk_mq_end_request → blk_end_sync_rq 까지 실행 */
		blk_hctx_poll(rq->q, rq->mq_hctx, NULL, BLK_POLL_ONESHOT);
		/* [한국어] 완료가 없으면 scheduler 에 CPU 를 양보하여 스핀 방지 */
		cond_resched();
	} while (!completion_done(wait)); /* [한국어] signal 될 때까지 반복 */
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
 * [한국어]
 * blk_execute_rq - passthrough request 를 동기로 실행하고 완료를 기다림
 *
 * @rq:      실행할 passthrough request
 *           (NVMe 라면 nvme_alloc_user_request() 또는 __nvme_submit_sync_cmd()
 *            가 blk_mq_alloc_request() 로 만들어 커맨드를 채워 둔 것)
 * @at_head: true 면 dispatch list 맨 앞에 삽입 (우선순위 처리용)
 * @return:  NVMe 명령 결과 (BLK_STS_OK, BLK_STS_IOERR, BLK_STS_TIMEOUT 등)
 *
 * NVMe Identify, Get/Set Features, Format NVM 등 동기 admin/passthrough 명령을
 * 실행하고 완료까지 block 한다. 내부적으로 스택에 blk_rq_wait 를 두고,
 * end_io 콜백(blk_end_sync_rq)이 complete 를 signal 할 때까지 대기한다.
 * poll queue 면 인터럽트 없이 blk_rq_poll_completion 으로 CQ 폴링.
 * 실행 컨텍스트: 프로세스 컨텍스트 (블록 가능, interruptible 아님).
 *
 * 호출 체인:
 *   nvme_submit_sync_cmd / nvme_sec_submit → [blk_execute_rq]
 *   → blk_mq_insert_request → blk_mq_run_hw_queue → nvme_queue_rq → SQ doorbell
 *   → nvme_irq → blk_mq_end_request → blk_end_sync_rq → complete(&wait.done)
 *   → [blk_execute_rq] 반환
 */
blk_status_t blk_execute_rq(struct request *rq, bool at_head)
{
	/* [한국어] rq->mq_hctx: 이 request 가 발행될 NVMe SQ 에 대응하는 hctx */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	/* [한국어] blk_rq_wait: 스택 할당 완료 대기 구조체.
	 * COMPLETION_INITIALIZER_ONSTACK: 스택 변수는 정적 초기화 불가하므로
	 * 이 매크로로 런타임에 초기화한다. */
	struct blk_rq_wait wait = {
		.done = COMPLETION_INITIALIZER_ONSTACK(wait.done),
	};

	/* [한국어] 인터럽트 비활성 컨텍스트에서 sleep 이 불가하므로 금지 */
	WARN_ON(irqs_disabled());
	/* [한국어] passthrough 가 아닌 일반 FS I/O request 는 여기로 오면 안 됨 */
	WARN_ON(!blk_rq_is_passthrough(rq));

	/* [한국어] rq->end_io_data: 완료 콜백(blk_end_sync_rq)이 result 를 써넣을 구조체 */
	rq->end_io_data = &wait;
	/* [한국어] blk_end_sync_rq: NVMe 완료 인터럽트 경로에서 호출될 콜백 */
	rq->end_io = blk_end_sync_rq;

	/* [한국어] IO 통계 기록 시작 */
	blk_account_io_start(rq);
	/* [한국어] hctx->dispatch 또는 elevator 에 request 삽입 */
	blk_mq_insert_request(rq, at_head ? BLK_MQ_INSERT_AT_HEAD : 0);
	/* [한국어] false: non-blocking run — kblockd workqueue 에서 async dispatch */
	blk_mq_run_hw_queue(hctx, false);

	if (blk_rq_is_poll(rq))	/* [한국어] 이 요청이 폴링 큐(HCTX_TYPE_POLL)로 갔는가 — 그렇다면 완료 인터럽트가 오지 않으므로 직접 CQ 를 돌려야 한다 */
		/* [한국어] poll queue: CQ 를 직접 반복 조회하여 완료 확인 */
		blk_rq_poll_completion(rq, &wait.done);
	else	/* [한국어] 인터럽트 큐로 간 일반 요청 */
		/* [한국어] 일반 인터럽트 방식: 완료 인터럽트가 signal 할 때까지 sleep */
		blk_wait_io(&wait.done);

	return wait.ret;	/* [한국어] blk_end_sync_rq 콜백이 wait->ret 에 남긴 완료 상태를 호출자에게 돌려준다 */
}
EXPORT_SYMBOL(blk_execute_rq);

/*
 * [한국어]
 * __blk_mq_requeue_request - request 재발행 준비 (tag 반납, 상태 초기화)
 *
 * @rq: 재발행할 request
 *
 * request 를 requeue_list 에 넣기 전에 드라이버 tag(NVMe CID) 를 반납하고
 * 상태를 MQ_RQ_IDLE 로 되돌려 재할당 가능하게 한다.
 * blk_mq_requeue_request 의 내부 헬퍼.
 */
static void __blk_mq_requeue_request(struct request *rq)
{
	/* [한국어] q: request 가 속한 request_queue (requeue_lock 보호 대상) */
	struct request_queue *q = rq->q;

	/* [한국어] NVMe CID(driver tag) 를 tag_set 으로 반납 — 다른 request 가 재사용 가능 */
	blk_mq_put_driver_tag(rq);

	/* [한국어] block_rq_requeue tracepoint: blktrace/bpf 도구가 requeue 를 추적 */
	trace_block_rq_requeue(rq);
	/* [한국어] rq_qos_requeue: cgroup blkio 계정/throttle 에 requeue 통보 */
	rq_qos_requeue(q, rq);

	if (blk_mq_request_started(rq)) {	/* [한국어] 이미 blk_mq_start_request 를 거쳐 하드웨어에 넘어간 요청인가 — 그렇다면 상태 기계를 되감아야 한다 */
		/* [한국어] MQ_RQ_IN_FLIGHT → MQ_RQ_IDLE: 이미 발행된 request 를
		 * 재발행 가능 상태로 되돌린다. WRITE_ONCE 로 state machine 원자적 전환. */
		WRITE_ONCE(rq->state, MQ_RQ_IDLE);
		/* [한국어] RQF_TIMED_OUT 플래그 제거: requeue 후 타임아웃 카운터 초기화 */
		rq->rq_flags &= ~RQF_TIMED_OUT;
	}
}

/*
 * [한국어]
 * blk_mq_requeue_request - request 를 requeue_list 에 넣어 재발행 대기
 *
 * @rq:               재발행할 request
 * @kick_requeue_list: true 면 즉시 blk_mq_kick_requeue_list 로 kblockd 워크 예약
 *
 * NVMe 드라이버가 queue full(BLK_STS_RESOURCE) 등의 이유로 request 를 처리하지 못할 때
 * 재발행 대기열에 넣는다. requeue_list 는 requeue_lock 으로 보호.
 * 이후 blk_mq_requeue_work 가 kblockd workqueue 에서 재삽입을 처리.
 *
 * 호출 체인:
 *   nvme_queue_rq (BLK_STS_RESOURCE 반환) → blk_mq_dispatch_rq_list
 *   → [blk_mq_requeue_request] → blk_mq_kick_requeue_list
 *   → kblockd → blk_mq_requeue_work → blk_mq_run_hw_queues
 */
void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list)
{
	/* [한국어] q->requeue_list 와 requeue_lock 에 접근하기 위한 큐 참조 */
	struct request_queue *q = rq->q;
	unsigned long flags;	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. requeue_list 는 완료 인터럽트 컨텍스트에서도 만져지므로 IRQ 를 막고 잡아야 한다 */

	/* [한국어] tag 반납, 상태 초기화 등 재발행 준비 */
	__blk_mq_requeue_request(rq);

	/* this request will be re-inserted to io scheduler queue */
	/* [한국어] elevator 가 있으면 sched 내부 requeue 처리 (RQF_SOFTBARRIER 등) */
	blk_mq_sched_requeue_request(rq);

	/* [한국어] IRQ 비활성화 후 spinlock: requeue_list 는 인터럽트 컨텍스트에서도 접근 가능 */
	spin_lock_irqsave(&q->requeue_lock, flags);
	/* [한국어] requeue_list 끝에 추가: FIFO 순서로 재발행 */
	list_add_tail(&rq->queuelist, &q->requeue_list);
	spin_unlock_irqrestore(&q->requeue_lock, flags);	/* [한국어] requeue_list 삽입이 끝났으므로 락 해제 + 이전 IRQ 상태 복원 */

	/* [한국어] kick_requeue_list: true 면 즉시 kblockd workqueue 에 재발행 작업 예약 */
	if (kick_requeue_list)
		blk_mq_kick_requeue_list(q);
}
EXPORT_SYMBOL(blk_mq_requeue_request);

/*
 * [한국어]
 * blk_mq_requeue_work - requeue_list 의 request 들을 재삽입하는 kblockd work
 *
 * @work: request_queue 의 requeue_work (delayed_work 임베디드)
 *
 * blk_mq_kick_requeue_list 가 kblockd workqueue 에 예약한 delayed work 핸들러.
 * requeue_list 와 flush_list 의 모든 request 를 꺼내 다시 hctx 에 삽입한 뒤
 * 모든 hctx 를 run 하여 NVMe SQ 에 발행 시도한다.
 * 실행 컨텍스트: kblockd workqueue 스레드 (sleep 가능).
 */
static void blk_mq_requeue_work(struct work_struct *work)
{
	/* [한국어] container_of: work 포인터에서 request_queue 를 역산
	 * requeue_work 는 q 에 embedded delayed_work */
	struct request_queue *q =
		container_of(work, struct request_queue, requeue_work.work);
	/* [한국어] 로컬 리스트: spinlock 구간을 최소화하기 위해 일괄 이동 후 처리 */
	LIST_HEAD(rq_list);
	LIST_HEAD(flush_list);	/* [한국어] flush 시퀀스 request 를 옮겨 담을 두 번째 로컬 리스트. requeue 대상과 처리 방식이 달라 분리한다 */
	struct request *rq;	/* [한국어] 리스트에서 하나씩 꺼낼 커서 */

	/* [한국어] requeue_lock: requeue_list/flush_list 접근을 IRQ 차단 후 잠금 */
	spin_lock_irq(&q->requeue_lock);
	/* [한국어] requeue_list 전체를 로컬 rq_list 로 이동 — lock 구간 최소화 */
	list_splice_init(&q->requeue_list, &rq_list);
	/* [한국어] flush_list(flush sequence 관련 request) 도 동일하게 이동 */
	list_splice_init(&q->flush_list, &flush_list);
	spin_unlock_irq(&q->requeue_lock);	/* [한국어] 두 리스트를 로컬로 옮겼으므로 락 해제 — 이후 재삽입 작업은 락 없이 수행해 임계구역을 짧게 유지한다 */

	while (!list_empty(&rq_list)) {	/* [한국어] 로컬로 옮긴 requeue 대상들을 비워질 때까지 하나씩 재삽입 */
		/* [한국어] rq_list 앞에서 하나씩 꺼내 재삽입 */
		rq = list_entry(rq_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);	/* [한국어] 리스트에서 떼어내고 노드를 자기 자신을 가리키도록 초기화 — 재삽입 시 이전 링크가 남아 리스트가 꼬이는 것을 막는다 */
		/*
		 * If RQF_DONTPREP is set, the request has been started by the
		 * driver already and might have driver-specific data allocated
		 * already.  Insert it into the hctx dispatch list to avoid
		 * block layer merges for the request.
		 */
		if (rq->rq_flags & RQF_DONTPREP)	/* [한국어] 드라이버가 이미 이 요청의 커맨드/DMA 를 준비해 둔 경우 */
			/* [한국어] RQF_DONTPREP: 드라이버가 이미 NVMe SQE 를 준비한 경우.
			 * elevator merge 를 우회하고 hctx->dispatch 에 직접 삽입. */
			blk_mq_request_bypass_insert(rq, 0);
		else	/* [한국어] 아직 드라이버가 손대지 않은 요청 */
			/* [한국어] 일반 requeue: 큐 앞머리에 삽입하여 다음 run 에서 우선 발행 */
			blk_mq_insert_request(rq, BLK_MQ_INSERT_AT_HEAD);
	}

	while (!list_empty(&flush_list)) {	/* [한국어] flush 시퀀스 request 들도 비워질 때까지 재삽입 */
		/* [한국어] flush_list: blk-flush.c 의 flush sequence request — 일반 삽입 */
		rq = list_entry(flush_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);	/* [한국어] 리스트에서 떼어내고 노드 초기화 */
		blk_mq_insert_request(rq, 0);	/* [한국어] flush 는 blk_mq_insert_request 안에서 REQ_OP_FLUSH 로 판별되어 hctx->dispatch 앞머리로 간다 */
	}

	/* [한국어] 모든 hctx 를 async run — NVMe SQ doorbell 을 통해 재발행 시도 */
	blk_mq_run_hw_queues(q, false);
}

/*
 * [한국어]
 * blk_mq_kick_requeue_list - requeue work 를 즉시 kblockd 에 예약
 *
 * @q: 대상 request_queue
 *
 * NVMe 드라이버가 BLK_STS_RESOURCE 로 request 를 돌려보낸 뒤 또는
 * device 가 다시 사용 가능해졌을 때 호출하여 requeue_work 를 예약한다.
 * delay 0 으로 예약하므로 kblockd 스레드가 가능한 빨리 실행한다.
 */
void blk_mq_kick_requeue_list(struct request_queue *q)
{
	/* [한국어] WORK_CPU_UNBOUND: 특정 CPU 에 묶이지 않은 kblockd workqueue 에 예약
	 * delay 0: 즉시 (다음 kblockd 스케줄 시점에) 실행 */
	kblockd_mod_delayed_work_on(WORK_CPU_UNBOUND, &q->requeue_work, 0);
}
EXPORT_SYMBOL(blk_mq_kick_requeue_list);

/*
 * [한국어]
 * blk_mq_delay_kick_requeue_list - 지연 후 requeue work 를 kblockd 에 예약
 *
 * @q:     대상 request_queue
 * @msecs: 지연 시간 (밀리초)
 *
 * NVMe queue full 상태가 해소될 때까지 일정 시간 기다린 뒤
 * requeue_work 를 실행하려 할 때 사용.
 */
void blk_mq_delay_kick_requeue_list(struct request_queue *q,
				    unsigned long msecs)
{
	/* [한국어] msecs_to_jiffies: ms → jiffies 변환 후 delayed work 예약 */
	kblockd_mod_delayed_work_on(WORK_CPU_UNBOUND, &q->requeue_work,
				    msecs_to_jiffies(msecs));
}
EXPORT_SYMBOL(blk_mq_delay_kick_requeue_list);

/*
 * [한국어]
 * blk_is_flush_data_rq - flush sequence 중의 data transfer request 인지 확인
 *
 * @rq:    검사할 request
 * @return: true 이면 flush sequence 내의 data request (is_flush_rq 는 아님)
 *
 * queue quiesce 중 flush data request 가 완료된 경우를 inflight 에서 제외하기 위해 사용.
 * flush sequence: PRE_FLUSH → DATA → POST_FLUSH 단계에서 DATA 단계 request.
 */
static bool blk_is_flush_data_rq(struct request *rq)
{
	/* [한국어] RQF_FLUSH_SEQ: flush sequence 에 속하는 request 표시
	 * is_flush_rq: PRE/POST_FLUSH request 이면 true — DATA request 는 false */
	return (rq->rq_flags & RQF_FLUSH_SEQ) && !is_flush_rq(rq);
}

/*
 * [한국어]
 * blk_mq_rq_inflight - inflight request 탐색 콜백 (하나라도 발견하면 조기 종료)
 *
 * @rq:   순회 중인 request
 * @priv: bool *busy 포인터 (inflight 발견 시 true 로 설정)
 * @return: false 이면 busy=true 로 설정 후 순회 조기 종료
 *
 * blk_mq_queue_inflight 에서 blk_mq_queue_tag_busy_iter 의 콜백으로 사용.
 * MQ_RQ_IN_FLIGHT 상태인 request 를 발견하면 큐가 바쁘다고 판단.
 * 단, queue quiesce 중 완료된 flush data request 는 inflight 로 보지 않는다.
 */
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
	/* [한국어] blk_mq_request_started: MQ_RQ_IN_FLIGHT 또는 MQ_RQ_COMPLETE 상태
	 * quiesce 중 완료된 flush data request 는 inflight 로 세지 않음 */
	if (blk_mq_request_started(rq) && !(blk_queue_quiesced(rq->q) &&
				blk_is_flush_data_rq(rq) &&
				blk_mq_request_completed(rq))) {
		bool *busy = priv;

		/* [한국어] inflight request 발견: busy=true 로 표시 */
		*busy = true;
		/* [한국어] false 반환: 이후 순회를 멈춤 — 하나라도 있으면 충분 */
		return false;
	}

	/* [한국어] IDLE 상태이거나 quiesce 제외 조건 충족: 계속 순회 */
	return true;
}

/*
 * [한국어]
 * blk_mq_queue_inflight - queue 에 inflight request 가 있는지 확인
 *
 * @q:     검사할 request_queue
 * @return: inflight request 가 하나라도 있으면 true
 *
 * NVMe reset/shutdown 전 drain 이 완료되었는지 확인하는 데 사용.
 * 모든 tag 의 request 를 순회하며 하나라도 MQ_RQ_IN_FLIGHT 이면 true 반환.
 */
bool blk_mq_queue_inflight(struct request_queue *q)
{
	/* [한국어] false 초기값: 만료 시 반환 (inflight 없음) */
	bool busy = false;

	/* [한국어] blk_mq_queue_tag_busy_iter: 모든 busy tag 의 request 를 순회
	 * blk_mq_rq_inflight 콜백이 첫 번째 inflight request 발견 시 busy=true */
	blk_mq_queue_tag_busy_iter(q, blk_mq_rq_inflight, &busy);
	return busy;	/* [한국어] true 면 아직 하드웨어에 남아 있는 요청이 있다는 뜻 — 드라이버는 이 값이 false 가 될 때까지 teardown 을 미룬다 */
}
EXPORT_SYMBOL_GPL(blk_mq_queue_inflight);

/*
 * [한국어]
 * blk_mq_rq_timed_out - timeout 만료된 request 를 드라이버 timeout handler 로 전달
 *
 * @req: timeout 된 NVMe request (MQ_RQ_IN_FLIGHT 상태)
 *
 * blk_mq_timeout_work 에서 만료된 request 에 대해 호출된다.
 * mq_ops->timeout(nvme_timeout) 을 통해 컨트롤러 abort/reset 을 시도하고,
 * BLK_EH_RESET_TIMER 이면 타이머를 재설정하여 다음 기회를 준다.
 *
 * 호출 체인:
 *   blk_mq_timeout_work → blk_mq_handle_expired → [blk_mq_rq_timed_out]
 *   → q->mq_ops->timeout (NVMe PCIe: nvme_timeout)
 *
 * NVMe PCIe 의 nvme_timeout() 이 실제로 하는 판단 (pci.c 실물 확인):
 *   - PCI 링크가 끊겼거나 컨트롤러 상태가 종단(DELETING/DEAD): 컨트롤러를
 *     즉시 disable 하고 BLK_EH_DONE (인플라이트는 전부 강제 완료된다).
 *   - 상태가 RESETTING: 이미 복구 중이므로 BLK_EH_RESET_TIMER 로 더 기다린다.
 *   - admin 큐(qid 0)이거나 이 명령이 이미 한 번 Abort 되었는데도 안 돌아옴:
 *     Abort 를 더 시도하지 않고 컨트롤러 리셋 경로로 간다.
 *   - 그 외 일반 I/O 첫 타임아웃: admin 큐에 Abort Command(opcode 0x08)를
 *     대상 CID/SQID 를 지정해 발행하고 BLK_EH_RESET_TIMER 를 돌려준다.
 *     ctrl->abort_limit 로 동시 Abort 수를 제한한다.
 * 이 함수(블록 계층)는 위 어느 것도 직접 부르지 않는다 — mq_ops->timeout
 * 함수 포인터를 통한 간접 호출뿐이다.
 */
static void blk_mq_rq_timed_out(struct request *req)
{
	/* [한국어] RQF_TIMED_OUT: 이 request 가 만료되었음 표시
	 * blk_mq_req_expired 에서 두 번 만료 처리되는 것을 방지 */
	req->rq_flags |= RQF_TIMED_OUT;
	if (req->q->mq_ops->timeout) {	/* [한국어] 드라이버가 timeout 콜백을 등록했는가. 등록하지 않은 드라이버(loop 등)는 아래로 내려가 타이머만 다시 건다 */
		enum blk_eh_timer_return ret;	/* [한국어] 드라이버의 판정 결과를 받을 변수 — BLK_EH_DONE 또는 BLK_EH_RESET_TIMER */

		/* [한국어] nvme_timeout: NVMe 드라이버의 timeout error handler.
		 * abort 성공 → BLK_EH_DONE (완료로 처리)
		 * reset 필요 → BLK_EH_RESET_TIMER (타이머 재설정) */
		ret = req->q->mq_ops->timeout(req);
		if (ret == BLK_EH_DONE)	/* [한국어] 드라이버가 이 요청을 직접 끝냈다고 답한 경우 */
			/* [한국어] timeout handler 가 직접 request 를 완료 처리 — 종료 */
			return;
		WARN_ON_ONCE(ret != BLK_EH_RESET_TIMER);	/* [한국어] 규약상 나머지 값은 BLK_EH_RESET_TIMER 뿐이다. 다른 값이 오면 드라이버 버그이므로 한 번만 경고한다 */
	}

	/* [한국어] BLK_EH_RESET_TIMER: 컨트롤러가 리셋 중이므로 타이머 재설정.
	 * 이 request 는 컨트롤러 복구 후 재완료 될 예정. */
	blk_add_timer(req);
}

/*
 * [한국어]
 * struct blk_expired_data - timeout 검사를 위한 상태 집합
 *
 * blk_mq_timeout_work 가 스택에 할당하여 blk_mq_queue_tag_busy_iter 콜백에 전달.
 * 두 단계로 사용: 1) blk_mq_check_expired 에서 만료 여부만 확인,
 *                 2) blk_mq_handle_expired 에서 실제 timeout 처리.
 */
struct blk_expired_data {
	bool has_timedout_rq;
	/* [한국어] timeout 된 request 가 하나라도 발견되면 true.
	 * 설정자: blk_mq_check_expired 가 만료 request 발견 시 true 로 설정.
	 * 읽는 자: blk_mq_timeout_work 가 두 번째 순회(handle) 실행 여부 결정.
	 * 초기값: false (스택 초기화) */

	unsigned long next;
	/* [한국어] 다음 timeout 이 발생할 가장 이른 deadline (jiffies).
	 * 설정자: blk_mq_req_expired 에서 만료 안 된 request 의 deadline 중 최솟값.
	 * 읽는 자: blk_mq_timeout_work 가 mod_timer 로 타이머 재설정.
	 * 초기값: 0 (아직 설정 안 됨) */

	unsigned long timeout_start;
	/* [한국어] 이번 timeout 검사를 시작한 시각 (jiffies).
	 * 설정자: blk_mq_timeout_work 가 jiffies 로 초기화.
	 * 읽는 자: blk_mq_req_expired 가 time_after_eq 로 만료 여부 판단.
	 * 목적: 검사 도중 jiffies 가 변해도 일관된 기준 시점 유지. */
};

/*
 * [한국어]
 * blk_mq_req_expired - request 의 timeout deadline 이 지났는지 확인
 *
 * @rq:      검사할 request
 * @expired: timeout 검사 상태 (timeout_start, next 업데이트)
 * @return:  만료되었으면 true
 *
 * MQ_RQ_IN_FLIGHT 상태이고 RQF_TIMED_OUT 이 없는 request 만 검사.
 * 만료되지 않은 경우 expired->next 를 가장 이른 deadline 으로 업데이트한다.
 */
static bool blk_mq_req_expired(struct request *rq, struct blk_expired_data *expired)
{
	unsigned long deadline;

	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)	/* [한국어] MQ_RQ_IN_FLIGHT 가 아니면 아직 하드웨어에 넘어가지 않았다는 뜻 */
		/* [한국어] 아직 드라이버에 발행되지 않은 request 는 timeout 대상 아님 */
		return false;
	if (rq->rq_flags & RQF_TIMED_OUT)	/* [한국어] 이미 만료 판정을 받은 요청인가 */
		/* [한국어] 이미 timeout 처리된 request 는 중복 처리 방지 */
		return false;

	/* [한국어] READ_ONCE: blk_add_timer 가 다른 컨텍스트에서 deadline 을 갱신하는
	 * 것과의 torn read 를 방지 */
	deadline = READ_ONCE(rq->deadline);
	if (time_after_eq(expired->timeout_start, deadline))	/* [한국어] 순회 시작 시각이 이 요청의 deadline 을 넘어섰는가(time_after_eq 는 jiffies 랩어라운드를 고려한 비교) */
		/* [한국어] timeout_start >= deadline: 현재 시각이 deadline 을 넘었으므로 만료 */
		return true;

	/* [한국어] 아직 만료 안 됨: expired->next 를 가장 이른 deadline 으로 갱신
	 * blk_mq_timeout_work 가 이 값으로 타이머를 재설정한다. */
	if (expired->next == 0)
		expired->next = deadline;
	else if (time_after(expired->next, deadline))	/* [한국어] 이미 기록된 next 보다 이 요청의 deadline 이 더 이르면 */
		expired->next = deadline;	/* [한국어] 가장 이른 만료 시각으로 갱신 — 타이머를 그 시점에 맞춰 다시 걸기 위함 */
	return false;
}

/*
 * [한국어]
 * blk_mq_put_rq_ref - request 의 ref count 를 감소하고 0 이 되면 해제
 *
 * @rq: ref count 를 감소할 request
 *
 * flush request 와 일반 request 두 가지 경로를 처리.
 * flush request 는 end_io 콜백을 통해 해제 여부를 결정.
 * 일반 request 는 req_ref_put_and_test 로 마지막 참조자 여부 확인.
 */
void blk_mq_put_rq_ref(struct request *rq)
{
	if (is_flush_rq(rq)) {
		/* [한국어] flush request: end_io 콜백이 RQ_END_IO_FREE 를 반환하면 해제 */
		if (rq->end_io(rq, 0, NULL) == RQ_END_IO_FREE)
			blk_mq_free_request(rq);
	} else if (req_ref_put_and_test(rq)) {	/* [한국어] 일반 request 는 참조를 하나 내리고 그것이 마지막이었는지 원자적으로 확인한다. 순회자가 잡아 둔 참조를 놓는 지점이다 */
		/* [한국어] req_ref_put_and_test: 마지막 참조자이면 true → request 해제 */
		__blk_mq_free_request(rq);
	}
}

/*
 * [한국어]
 * blk_mq_check_expired - 만료 request 존재 여부만 확인하는 1차 순회 콜백
 *
 * @rq:    순회 중인 request
 * @priv:  blk_expired_data 포인터
 * @return: false 이면 순회 중단 (만료 request 발견)
 *
 * blk_mq_timeout_work 의 1차 순회에서 사용.
 * 만료 request 발견 시 has_timedout_rq=true 로 설정하고 순회를 중단.
 * 이후 2차 순회(blk_mq_handle_expired)에서 실제 timeout 처리를 수행.
 * 두 단계로 나누는 이유: blk_mq_queue_tag_busy_iter 가 lock 을 유지한 채로
 * timeout 처리(abort/reset)까지 하면 교착 위험이 있기 때문.
 */
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
	if (blk_mq_req_expired(rq, expired)) {	/* [한국어] 만료되었는지 판정. 위 영문 주석대로 순회자가 request 참조를 잡고 있어 그 사이 태그가 재할당되지 않으므로, 이 판정은 신뢰할 수 있다 */
		/* [한국어] 만료된 CID 발견: 2차 순회(handle) 가 필요함을 표시 */
		expired->has_timedout_rq = true;
		/* [한국어] false: 순회 조기 종료 — 하나라도 있으면 충분 */
		return false;
	}
	return true;
}

/*
 * [한국어]
 * blk_mq_handle_expired - 만료 request 에 실제 timeout 처리를 수행하는 2차 콜백
 *
 * @rq:    순회 중인 request
 * @priv:  blk_expired_data 포인터 (next 업데이트용)
 * @return: 항상 true (모든 request 를 순회)
 *
 * blk_mq_timeout_work 의 2차 순회에서 사용.
 * 만료된 모든 request 에 blk_mq_rq_timed_out 을 호출하여
 * nvme_timeout 으로 abort/reset 을 시도한다.
 */
static bool blk_mq_handle_expired(struct request *rq, void *priv)
{
	struct blk_expired_data *expired = priv;

	if (blk_mq_req_expired(rq, expired))	/* [한국어] 2차 순회에서 다시 판정 — 1차 순회와 grace period 사이에 완료된 요청은 여기서 걸러진다 */
		/* [한국어] 만료된 request 마다 nvme_timeout(abort/reset) 호출 */
		blk_mq_rq_timed_out(rq);
	return true;
}

/*
 * [한국어]
 * blk_mq_timeout_work - 만료된 request 를 찾아 드라이버 timeout handler 를 호출하는 work
 *
 * @work: request_queue 에 embed 된 timeout_work (work_struct)
 *
 * q->timeout 타이머가 만료되면 kblockd workqueue 에서 이 함수가 실행된다.
 * 1차 순회(blk_mq_check_expired)로 만료 request 존재 여부 확인 후,
 * 2차 순회(blk_mq_handle_expired)로 nvme_timeout(abort/reset) 를 실제 호출한다.
 * 만료 request 가 없으면 다음 deadline 까지 타이머 재설정, 또는 hctx 를 idle 마킹.
 * freeze 와의 deadlock 방지를 위해 blk_queue_enter 대신 percpu_ref_tryget 를 사용.
 * 실행 컨텍스트: kblockd workqueue 스레드 (sleep 가능).
 *
 * 호출 체인:
 *   q->timeout 타이머 만료 → [blk_mq_timeout_work]
 *   → blk_mq_check_expired (1차: 만료 여부만) → blk_mq_handle_expired (2차: 처리)
 *   → blk_mq_rq_timed_out → nvme_timeout
 */
static void blk_mq_timeout_work(struct work_struct *work)
{
	/* [한국어] container_of: timeout_work 포인터에서 request_queue 를 역산 */
	struct request_queue *q =
		container_of(work, struct request_queue, timeout_work);
	/* [한국어] expired: 이번 timeout 순회의 기준 시각과 결과를 담는 구조체.
	 * timeout_start=jiffies: 순회 중 jiffies 변화와 무관하게 일관된 판단 기준 */
	struct blk_expired_data expired = {
		.timeout_start = jiffies,
	};
	/* [한국어] hctx/i: queue_for_each_hw_ctx 순회용 (idle 마킹 단계에서 사용) */
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 의 인덱스. 만료가 하나도 없을 때 hctx 를 idle 로 표시하는 단계에서만 쓰인다 */

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
	/* [한국어] percpu_ref_tryget: freeze 중에도 reference 를 시도 획득.
	 * blk_queue_enter 대신 사용하는 이유: freeze 가 진행 중일 때 blk_queue_enter 는
	 * 영구 block 될 수 있어 timeout workqueue 와 freeze 간 교착이 발생하기 때문.
	 * tryget 실패 시 freeze 중인 것이므로 timeout 처리를 건너뜀. */
	if (!percpu_ref_tryget(&q->q_usage_counter))
		return;

	/* check if there is any timed-out request */
	/* [한국어] 1차 순회: 모든 busy tag 를 순회하며 만료 request 존재 여부만 확인.
	 * 만료 발견 시 expired.has_timedout_rq = true, 순회 조기 종료 */
	blk_mq_queue_tag_busy_iter(q, blk_mq_check_expired, &expired);
	if (expired.has_timedout_rq) {	/* [한국어] 1차 순회에서 만료 요청을 하나라도 찾았는가 — 찾았을 때만 비싼 grace period 대기와 2차 순회를 수행한다 */
		/*
		 * Before walking tags, we must ensure any submit started
		 * before the current time has finished. Since the submit
		 * uses srcu or rcu, wait for a synchronization point to
		 * ensure all running submits have finished
		 */
		/* [한국어] blk_mq_wait_quiesce_done: RCU/SRCU grace period 대기.
		 * submit 경로가 tag 또는 hctx 접근을 마칠 때까지 기다린다.
		 * 이후 tag_busy_iter 가 안정된 상태에서 만료 request 를 처리 가능. */
		blk_mq_wait_quiesce_done(q->tag_set);

		/* [한국어] expired.next 를 0 으로 재설정: 2차 순회에서 새로 계산 */
		expired.next = 0;
		/* [한국어] 2차 순회: 모든 만료 request 에 blk_mq_rq_timed_out 호출
		 * nvme_timeout 이 abort/reset 를 실행 */
		blk_mq_queue_tag_busy_iter(q, blk_mq_handle_expired, &expired);
	}

	if (expired.next != 0) {	/* [한국어] 아직 만료되지 않은 요청 중 가장 이른 deadline 이 기록되었는가 */
		/* [한국어] 아직 만료 안 된 request 의 가장 이른 deadline 으로 타이머 재설정
		 * 다음 번 timeout_work 가 정확한 시점에 실행되도록 */
		mod_timer(&q->timeout, expired.next);
	} else {
		/*
		 * Request timeouts are handled as a forward rolling timer. If
		 * we end up here it means that no requests are pending and
		 * also that no request has been pending for a while. Mark
		 * each hctx as idle.
		 */
		/* [한국어] pending request 가 전혀 없음: 모든 hctx 를 idle 로 마킹.
		 * sbitmap 의 wakeup 상태를 정리하여 불필요한 wakeup 방지 */
		queue_for_each_hw_ctx(q, hctx, i) {
			/* the hctx may be unmapped, so check it here */
			/* [한국어] blk_mq_hw_queue_mapped: hctx 에 CPU 가 매핑되어 있어야
			 * idle 마킹 의미 있음. unmapped hctx 는 건너뜀 */
			if (blk_mq_hw_queue_mapped(hctx))
				/* [한국어] blk_mq_tag_idle: sbitmap_queue 의 wake_batch 재설정
				 * NVMe SQ 의 tag 할당 wakeup 배치 크기를 idle 기본값으로 복원 */
				blk_mq_tag_idle(hctx);
		}
	}
	/* [한국어] blk_queue_exit: percpu_ref_tryget 으로 획득한 참조 반납 */
	blk_queue_exit(q);
}

/*
 * [한국어]
 * struct flush_busy_ctx_data - sw queue 를 hctx dispatch list 로 flush 할 때의 컨텍스트
 *
 * blk_mq_flush_busy_ctxs 가 sbitmap_for_each_set 의 콜백(flush_busy_ctx)에
 * 전달하는 보조 구조체. hctx->ctx_map 의 set bit 마다 해당 sw queue 를
 * dispatch list 로 이동한다.
 */
struct flush_busy_ctx_data {
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 대상 hardware queue (NVMe SQ 에 대응).
	 * 설정자: blk_mq_flush_busy_ctxs 가 현재 dispatch 대상 hctx 로 초기화.
	 * 읽는 자: flush_busy_ctx 가 hctx->ctxs[bitnr] 로 sw queue 를 참조. */

	struct list_head *list;
	/* [한국어] sw queue 의 request 를 이동시킬 대상 list (보통 hctx->dispatch).
	 * 설정자: blk_mq_flush_busy_ctxs 가 dispatch 대기 list 로 초기화.
	 * 읽는 자: flush_busy_ctx 가 list_splice_tail_init 으로 rq_lists 를 여기에 append. */
};

/*
 * [한국어]
 * flush_busy_ctx - 하나의 busy sw queue 를 dispatch list 로 이동하는 sbitmap 콜백
 *
 * @sb:    hctx->ctx_map (per-CPU sw queue 의 pending 비트맵)
 * @bitnr: set 된 비트 번호 (= sw queue 의 인덱스)
 * @data:  flush_busy_ctx_data 포인터
 * @return: 항상 true (순회 계속)
 *
 * hctx->ctx_map 에서 set 된 비트(pending sw queue)마다 호출된다.
 * ctx->rq_lists[hctx->type] 의 모든 request 를 dispatch list 로 splice 하고
 * ctx_map 의 비트를 클리어하여 pending 상태를 해제한다.
 * ctx->lock 으로 sw queue 접근을 보호한다.
 */
static bool flush_busy_ctx(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	/* [한국어] flush_data: flush 대상 hctx 와 dispatch list 참조 */
	struct flush_busy_ctx_data *flush_data = data;
	/* [한국어] hctx: dispatch 대상 hw queue */
	struct blk_mq_hw_ctx *hctx = flush_data->hctx;
	/* [한국어] ctx: bitnr 에 해당하는 per-CPU sw queue — hctx 에 매핑된 CPU 의 큐 */
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];
	/* [한국어] type: hctx 의 타입 (DEFAULT/READ/POLL) — rq_lists 인덱스로 사용 */
	enum hctx_type type = hctx->type;

	/* [한국어] ctx->lock: sw queue 의 rq_lists 와 pending 비트를 보호 */
	spin_lock(&ctx->lock);
	/* [한국어] list_splice_tail_init: ctx->rq_lists[type] 의 모든 request 를
	 * flush_data->list 끝에 append 하고 rq_lists 를 빈 상태로 초기화 */
	list_splice_tail_init(&ctx->rq_lists[type], flush_data->list);
	/* [한국어] sbitmap_clear_bit: ctx_map 에서 bitnr 비트를 클리어.
	 * 이제 이 sw queue 는 더 이상 pending 이 없으므로 비트 해제 */
	sbitmap_clear_bit(sb, bitnr);
	spin_unlock(&ctx->lock);	/* [한국어] rq_lists 이동과 ctx_map 비트 클리어가 모두 끝났으므로 sw 큐 락 해제 */
	return true;
}

/*
 * Process software queues that have been marked busy, splicing them
 * to the for-dispatch
 */
/*
 * [한국어]
 * blk_mq_flush_busy_ctxs - hctx 에 pending 인 모든 sw queue 를 dispatch list 로 이동
 *
 * @hctx: 대상 hardware queue (NVMe SQ 에 대응)
 * @list: request 를 모을 dispatch 대기 list
 *
 * hctx->ctx_map 의 모든 set bit (= pending sw queue) 에 대해 flush_busy_ctx 를 호출.
 * blk_mq_dispatch_rq_list 가 dispatch 전에 이 함수로 sw queue 를 비운다.
 * NVMe 관점: 여러 CPU 가 submit 한 request 들이 이 함수에서 하나의 dispatch list 로 모임.
 */
void blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx, struct list_head *list)
{
	/* [한국어] flush_busy_ctx 에 전달할 컨텍스트 초기화 */
	struct flush_busy_ctx_data data = {
		.hctx = hctx,
		.list = list,	/* [한국어] 수집된 request 들을 담을 목적지 리스트 — 호출자(blk_mq_dispatch_rq_list)가 넘긴 dispatch 리스트다 */
	};

	/* [한국어] sbitmap_for_each_set: ctx_map 에서 set 된 비트마다 flush_busy_ctx 호출
	 * 결과: hctx 에 pending 된 모든 sw queue 의 request 가 list 에 모임 */
	sbitmap_for_each_set(&hctx->ctx_map, flush_busy_ctx, &data);
}

/*
 * [한국어]
 * struct dispatch_rq_data - sw queue 에서 request 하나를 꺼낼 때의 컨텍스트
 *
 * dispatch_rq_from_ctx 의 sbitmap 콜백에 전달하는 보조 구조체.
 * busy sw queue 에서 request 를 하나만 꺼내어 hctx 에 직접 dispatch 할 때 사용.
 */
struct dispatch_rq_data {
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 대상 hardware queue (NVMe SQ 에 대응).
	 * 설정자: blk_mq_dequeue_from_ctx 가 현재 hctx 로 초기화.
	 * 읽는 자: dispatch_rq_from_ctx 가 hctx->ctxs[bitnr] 접근에 사용. */

	struct request *rq;
	/* [한국어] 꺼낸 request 를 돌려주는 출력 필드.
	 * 설정자: dispatch_rq_from_ctx 가 rq_lists 에서 꺼낸 request 를 저장.
	 * 읽는 자: blk_mq_dequeue_from_ctx 가 이 포인터로 결과를 받아 반환.
	 * 값: NULL 이면 해당 sw queue 가 비어있음. */
};

/*
 * [한국어]
 * dispatch_rq_from_ctx - sw queue 에서 request 를 하나 꺼내는 sbitmap 콜백
 *
 * @sb:    hctx->ctx_map (pending sw queue 비트맵)
 * @bitnr: set 된 비트 번호 (sw queue 인덱스)
 * @data:  dispatch_rq_data 포인터
 * @return: rq 를 꺼냈으면 false (순회 중단), 아직 비어있으면 true (계속)
 *
 * bitnr 에 해당하는 sw queue 에서 rq_lists 의 맨 앞 request 를 하나 꺼낸다.
 * sw queue 가 이 request 로 인해 비게 되면 ctx_map 비트를 클리어.
 * blk_mq_dequeue_from_ctx 가 하나의 request 만 필요할 때 사용.
 */
static bool dispatch_rq_from_ctx(struct sbitmap *sb, unsigned int bitnr,
		void *data)
{
	/* [한국어] dispatch_data: 결과 request 를 저장할 출력 구조체 */
	struct dispatch_rq_data *dispatch_data = data;
	/* [한국어] hctx: dispatch 대상 hw queue */
	struct blk_mq_hw_ctx *hctx = dispatch_data->hctx;
	/* [한국어] ctx: bitnr 에 해당하는 per-CPU sw queue */
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];
	/* [한국어] type: hctx 타입 — rq_lists 인덱스 (DEFAULT/READ/POLL) */
	enum hctx_type type = hctx->type;

	/* [한국어] ctx->lock: rq_lists 수정 보호 */
	spin_lock(&ctx->lock);
	if (!list_empty(&ctx->rq_lists[type])) {	/* [한국어] 이 sw 큐에 실제로 대기 중인 request 가 있는가 — ctx_map 비트가 서 있어도 그 사이 비었을 수 있다 */
		/* [한국어] rq_lists 맨 앞 request 를 꺼냄 — FIFO 순서 유지 */
		dispatch_data->rq = list_entry_rq(ctx->rq_lists[type].next);
		/* [한국어] list_del_init: request 를 sw queue 에서 분리 */
		list_del_init(&dispatch_data->rq->queuelist);
		if (list_empty(&ctx->rq_lists[type]))	/* [한국어] 방금 하나를 뺐더니 이 sw 큐가 비었는가 */
			/* [한국어] sw queue 가 비었으면 ctx_map 비트 클리어 —
			 * 다음 flush_busy_ctxs 가 이 sw queue 를 순회하지 않음 */
			sbitmap_clear_bit(sb, bitnr);
	}
	spin_unlock(&ctx->lock);	/* [한국어] 리스트 조작이 끝났으므로 sw 큐 락 해제 */

	/* [한국어] rq 가 없으면 true (다음 set bit 로 계속), 있으면 false (조기 종료) */
	return !dispatch_data->rq;
}

/*
 * [한국어]
 * blk_mq_dequeue_from_ctx - hctx 의 sw queue 들에서 request 를 하나 꺼냄
 *
 * @hctx:  대상 hardware queue
 * @start: 순회 시작 sw queue (round-robin 공정성을 위해 매번 다른 ctx 부터)
 * @return: 꺼낸 request 포인터, 없으면 NULL
 *
 * blk_mq_dispatch_rq_list 의 budget/tag 가 부족할 때 sw queue 에서
 * 하나만 꺼내 dispatch 를 재시도하는 경로에서 사용.
 * start 부터 ctx_map 을 순회하여 첫 번째 non-empty sw queue 의 request 를 반환.
 */
struct request *blk_mq_dequeue_from_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *start)
{
	/* [한국어] off: start ctx 의 ctx_map 인덱스 — 0 이면 처음부터 순회
	 * round-robin 공정성: 매 dispatch 마다 다른 sw queue 부터 시작 */
	unsigned off = start ? start->index_hw[hctx->type] : 0;
	/* [한국어] data: dispatch_rq_from_ctx 에 전달할 컨텍스트 */
	struct dispatch_rq_data data = {
		.hctx = hctx,
		.rq   = NULL,	/* [한국어] 아직 아무것도 못 찾은 상태로 시작 — 콜백이 request 를 찾으면 여기에 채워 넣는다 */
	};

	/* [한국어] __sbitmap_for_each_set: off 부터 순회 (wrap-around 포함)
	 * dispatch_rq_from_ctx 가 request 를 찾으면 false 반환 → 순회 중단 */
	__sbitmap_for_each_set(&hctx->ctx_map, off,
			       dispatch_rq_from_ctx, &data);

	return data.rq;	/* [한국어] 찾은 request(없으면 NULL). 호출자는 이것으로 라운드로빈 dispatch 를 이어 간다 */
}

/*
 * [한국어]
 * __blk_mq_alloc_driver_tag - 드라이버 tag(NVMe CID) 를 sbitmap 에서 할당
 *
 * @rq:    tag 를 할당할 request
 * @return: 할당 성공 시 true, SQ full 시 false
 *
 * scheduler 내부 tag(internal_tag) 와 다른 드라이버 실제 tag(NVMe CID) 를 할당.
 * scheduler reserved tag 이면 breserved_tags 에서, 아니면 bitmap_tags 에서 할당.
 * rq->tag 에 최종 NVMe CID 를 설정하고 active request 수를 증가.
 *
 * 호출 체인:
 *   blk_mq_get_budget_and_tag → [__blk_mq_alloc_driver_tag]
 *   → __sbitmap_queue_get (lock-free 비트 할당) → rq->tag = CID
 */
bool __blk_mq_alloc_driver_tag(struct request *rq)
{
	/* [한국어] bitmap_tags: 비예약(일반) tag pool 의 sbitmap_queue.
	 * NVMe SQ depth 만큼의 slot 을 비트맵으로 관리 */
	struct sbitmap_queue *bt = &rq->mq_hctx->tags->bitmap_tags;
	/* [한국어] tag_offset: 예약 tag 수만큼 offset → 일반 CID 는 reserved 영역 이후부터 */
	unsigned int tag_offset = rq->mq_hctx->tags->nr_reserved_tags;
	int tag;	/* [한국어] __sbitmap_queue_get 이 돌려줄 비트 번호. 실패 시 BLK_MQ_NO_TAG(-1)이므로 부호 있는 int 여야 한다 */

	/* [한국어] blk_mq_tag_busy: hctx 를 busy 상태로 표시 — idle 타이머 방지 */
	blk_mq_tag_busy(rq->mq_hctx);

	if (blk_mq_tag_is_reserved(rq->mq_hctx->sched_tags, rq->internal_tag)) {	/* [한국어] 스케줄러 태그가 예약 영역에서 나온 것인가 — 그렇다면 드라이버 태그도 예약 풀에서 잡아야 예약의 의미가 유지된다 */
		/* [한국어] scheduler reserved tag 이면 별도의 reserved CID pool 에서 할당.
		 * offset 0: breserved_tags 는 비트 0 부터 직접 사용 */
		bt = &rq->mq_hctx->tags->breserved_tags;
		tag_offset = 0;	/* [한국어] breserved_tags 는 비트 0 부터가 곧 실제 태그 번호이므로 오프셋을 더하지 않는다 */
	} else {
		if (!hctx_may_queue(rq->mq_hctx, bt))	/* [한국어] 이 hctx 가 공유 태그 풀에서 자기 몫 이상을 쓰고 있지는 않은지 검사 */
			/* [한국어] hctx_may_queue: queue depth 예산 초과 확인.
			 * false 이면 SQ 가 허용 깊이에 도달 — CID 할당 거부 */
			return false;
	}

	/* [한국어] __sbitmap_queue_get: lock-free 원자적 비트 할당.
	 * 성공 시 비트 번호(0~depth-1), 실패 시 BLK_MQ_NO_TAG(==-1) */
	tag = __sbitmap_queue_get(bt);
	if (tag == BLK_MQ_NO_TAG)	/* [한국어] 태그 획득 실패 — 풀이 찼거나 다른 CPU 와의 경쟁에서 밀렸다 */
		/* [한국어] SQ 가 완전히 찼거나 경쟁으로 할당 실패 */
		return false;

	/* [한국어] rq->tag: 드라이버 관점의 NVMe CID.
	 * tag_offset 을 더해 reserved 영역을 건너뛴 실제 CID 값으로 설정 */
	rq->tag = tag + tag_offset;
	/* [한국어] blk_mq_inc_active_requests: hctx 의 active request 카운터 증가 */
	blk_mq_inc_active_requests(rq->mq_hctx);
	return true;
}

/*
 * [한국어]
 * blk_mq_dispatch_wake - sbitmap wakeup 콜백: tag 가 반납되면 hctx 를 깨워 재dispatch
 *
 * @wait:  hctx->dispatch_wait (wait_queue_entry)
 * @mode:  wakeup 모드 (사용 안 함)
 * @flags: wakeup 플래그 (사용 안 함)
 * @key:   sbitmap wakeup 키 (사용 안 함)
 * @return: 1 (wakeup 완료)
 *
 * NVMe 완료 인터럽트 → blk_mq_put_tag → sbitmap wakeup → 이 함수 호출.
 * hctx 를 wait queue 에서 제거하고 blk_mq_run_hw_queue 로 재dispatch 를 시도.
 *
 * 호출 체인:
 *   nvme_irq → blk_mq_end_request → blk_mq_put_tag → sbitmap_queue_wake
 *   → [blk_mq_dispatch_wake] → blk_mq_run_hw_queue
 */
static int blk_mq_dispatch_wake(wait_queue_entry_t *wait, unsigned mode,
				int flags, void *key)
{
	struct blk_mq_hw_ctx *hctx;

	/* [한국어] container_of: dispatch_wait 포인터에서 hctx 역산 */
	hctx = container_of(wait, struct blk_mq_hw_ctx, dispatch_wait);

	/* [한국어] dispatch_wait_lock: wait entry 제거와 sbq->ws_active 감소를 원자적으로 */
	spin_lock(&hctx->dispatch_wait_lock);
	if (!list_empty(&wait->entry)) {	/* [한국어] 아직 wait queue 에 등록된 상태인가 — 다른 CPU 가 먼저 떼어 갔으면 아무것도 하지 않는다 */
		struct sbitmap_queue *sbq;	/* [한국어] 어느 태그 풀의 wakeup 큐에 매달려 있었는지 계산해 둘 지역 변수 */

		/* [한국어] wait queue 에서 hctx 제거 — 중복 wakeup 방지 */
		list_del_init(&wait->entry);
		/* [한국어] bitmap_tags: 일반 tag pool sbitmap */
		sbq = &hctx->tags->bitmap_tags;
		/* [한국어] ws_active 감소: sbitmap wakeup 배치의 활성 카운터 감소 */
		atomic_dec(&sbq->ws_active);
	}
	spin_unlock(&hctx->dispatch_wait_lock);	/* [한국어] wait entry 제거와 ws_active 감소가 끝났으므로 락 해제 */

	/* [한국어] blk_mq_run_hw_queue(hctx, true): sync=true 로 즉시 dispatch 시도.
	 * NVMe CID 가 반납되었으므로 requeue 된 request 를 다시 발행 가능 */
	blk_mq_run_hw_queue(hctx, true);
	return 1;	/* [한국어] 1 을 반환해 wait queue 코어에 "이 엔트리는 깨움을 소비했다"고 알린다 */
}

/*
 * Mark us waiting for a tag. For shared tags, this involves hooking us into
 * the tag wakeups. For non-shared tags, we can simply mark us needing a
 * restart. For both cases, take care to check the condition again after
 * marking us as waiting.
 */
/*
 * [한국어]
 * blk_mq_mark_tag_wait - NVMe SQ 가 꽉 찼을 때(CID 고갈) hctx 를 대기 등록
 *
 * @hctx: tag 가 고갈된 hardware queue
 * @rq:   tag 를 기다리는 request
 * @return: true 이면 대기 등록 성공 (request 는 나중에 재dispatch 됨),
 *          false 이면 tag 가 이미 반납되었거나 대기 불필요
 *
 * NVMe SQ depth 가 가득 차면 blk_mq_dispatch_rq_list 가 이 함수를 호출한다.
 * 비공유 tag 이면 RESTART 플래그만 설정. 공유 tag 이면 sbitmap wakeup queue 에
 * hctx->dispatch_wait 를 등록하여 다른 CQ 완료가 CID 를 반납할 때 깨어난다.
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list (tag 없음) → [blk_mq_mark_tag_wait]
 *   → add_wait_queue 등록
 *   → (CID 반납 후) blk_mq_dispatch_wake → blk_mq_run_hw_queue
 */
static bool blk_mq_mark_tag_wait(struct blk_mq_hw_ctx *hctx,
				 struct request *rq)
{
	struct sbitmap_queue *sbq;
	struct wait_queue_head *wq;	/* [한국어] 태그 반납 시 우리를 깨워 줄 wakeup 큐의 head */
	wait_queue_entry_t *wait;	/* [한국어] hctx 에 내장된 대기 엔트리. hctx 당 하나뿐이라 중복 등록을 반드시 걸러야 한다 */
	bool ret;	/* [한국어] 등록 직후 재시도한 태그 획득 결과 */

	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) &&	/* [한국어] 태그 풀을 다른 큐와 공유하지 않는가 — 공유하지 않으면 태그를 반납할 주체가 이 hctx 자신뿐이라 */
	    !(blk_mq_is_shared_tags(hctx->flags))) {	/* [한국어] (같은 조건의 두 번째 형태: tagset 전역 공유 태그도 아닌 경우) */
		/* [한국어] 비공유 tag: wait queue 에 등록할 필요 없이 RESTART 표시만.
		 * CID 반납 시 __blk_mq_free_request → blk_mq_sched_restart_hctx 가
		 * hctx 를 재실행한다. */
		blk_mq_sched_mark_restart_hctx(hctx);

		/*
		 * It's possible that a tag was freed in the window between the
		 * allocation failure and adding the hardware queue to the wait
		 * queue.
		 *
		 * Don't clear RESTART here, someone else could have set it.
		 * At most this will cost an extra queue run.
		 */
		/* [한국어] RESTART 마킹 후 즉시 tag 재시도: 마킹 직전 다른 CPU 가
		 * CID 를 반납했을 가능성이 있으므로 한 번 더 시도. 성공 시 true 반환. */
		return blk_mq_get_driver_tag(rq);
	}

	/* [한국어] wait: hctx 에 내장된 dispatch wait queue entry */
	wait = &hctx->dispatch_wait;
	/* [한국어] list_empty_careful: 이미 wait queue 에 등록된 경우 중복 방지 */
	if (!list_empty_careful(&wait->entry))
		return false;

	/* [한국어] reserved tag 이면 breserved_tags, 아니면 bitmap_tags 의 wakeup queue 사용 */
	if (blk_mq_tag_is_reserved(rq->mq_hctx->sched_tags, rq->internal_tag))
		sbq = &hctx->tags->breserved_tags;
	else	/* [한국어] 일반 태그를 기다리는 경우 */
		sbq = &hctx->tags->bitmap_tags;	/* [한국어] 비예약 태그 풀의 wakeup 큐를 대상으로 삼는다 */
	/* [한국어] bt_wait_ptr: sbitmap 의 wakeup wait queue head 를 해시로 선택 */
	wq = &bt_wait_ptr(sbq, hctx)->wait;

	/* [한국어] wq->lock 먼저, dispatch_wait_lock 다음: 락 순서 고정 (역순 방지) */
	spin_lock_irq(&wq->lock);
	spin_lock(&hctx->dispatch_wait_lock);	/* [한국어] 두 락을 항상 wq->lock → dispatch_wait_lock 순으로 잡는다. blk_mq_dispatch_wake 도 같은 순서라 데드락이 없다 */
	if (!list_empty(&wait->entry)) {	/* [한국어] 락을 잡는 사이에 다른 CPU 가 먼저 등록했는가 */
		/* [한국어] 경쟁으로 다른 코드가 먼저 등록: 중복 등록 방지 후 실패 반환 */
		spin_unlock(&hctx->dispatch_wait_lock);
		spin_unlock_irq(&wq->lock);	/* [한국어] 바깥 락도 해제하고 IRQ 상태 복원 — 이미 누군가 등록했으므로 우리는 실패로 돌아간다 */
		return false;
	}

	/* [한국어] ws_active 증가: sbitmap 의 wakeup 배치가 이 wait entry 를 포함함을 기록 */
	atomic_inc(&sbq->ws_active);
	/* [한국어] WQ_FLAG_EXCLUSIVE 제거: 비배타적 wakeup — 여러 hctx 동시에 깨울 수 있음 */
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	/* [한국어] __add_wait_queue: wq 에 dispatch_wait 등록 (CID 반납 시 blk_mq_dispatch_wake 호출) */
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
	/* [한국어] smp_mb(): waitqueue 등록 ↔ CID(tag) 재할당 순서 보장.
	 * sbitmap_queue_wake_up 이 waitqueue_active 를 검사하기 전에
	 * 이 CPU 의 waitqueue 등록이 보여야 wakeup 누락 방지 */
	smp_mb();

	/*
	 * It's possible that a tag was freed in the window between the
	 * allocation failure and adding the hardware queue to the wait
	 * queue.
	 */
	/* [한국어] waitqueue 등록 직후 다시 NVMe CID(tag) 확보 시도 */
	ret = blk_mq_get_driver_tag(rq);
	if (!ret) {	/* [한국어] 재시도도 실패 — 등록은 남겨 둔 채(태그가 반납되면 깨워 줄 것) 락을 풀고 실패를 알린다 */
		spin_unlock(&hctx->dispatch_wait_lock);	/* [한국어] 안쪽 락 해제 */
		spin_unlock_irq(&wq->lock);	/* [한국어] 바깥 락 해제 + IRQ 상태 복원 */
		return false;
	}

	/*
	 * We got a tag, remove ourselves from the wait queue to ensure
	 * someone else gets the wakeup.
	 */
	/* [한국어] CID 를 얻었으므로 wait queue 에서 자기 자신 제거.
	 * 다른 wakeup 대상을 놓치지 않도록 배타적 wakeup 방지 */
	list_del_init(&wait->entry);
	/* [한국어] ws_active 감소: wakeup 배치에서 이 wait entry 제거 */
	atomic_dec(&sbq->ws_active);
	spin_unlock(&hctx->dispatch_wait_lock);	/* [한국어] 자기 제거와 ws_active 감소가 끝났으므로 안쪽 락 해제 */
	spin_unlock_irq(&wq->lock);	/* [한국어] 바깥 락 해제 + IRQ 상태 복원 */

	return true;
}

/* [한국어] BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT: EWMA 가중치 분모 (새 샘플 비율 = 1/8) */
#define BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT  8
/* [한국어] BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR: busy 시 더하는 값의 비트 시프트 (1<<4=16) */
#define BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR  4
/*
 * Update dispatch busy with the Exponential Weighted Moving Average(EWMA):
 * - EWMA is one simple way to compute running average value
 * - weight(7/8 and 1/8) is applied so that it can decrease exponentially
 * - take 4 as factor for avoiding to get too small(0) result, and this
 *   factor doesn't matter because EWMA decreases exponentially
 */
/*
 * [한국어]
 * blk_mq_update_dispatch_busy - hctx 의 dispatch busy 수준을 EWMA 로 갱신
 *
 * @hctx: 갱신할 hardware queue
 * @busy: 이번 dispatch 에서 자원 부족이 발생했으면 true
 *
 * dispatch_busy 가 높으면 blk_mq_dispatch_rq_list 가 single-dispatch 경로를 선택해
 * sw queue 를 미리 flush 하지 않는다 (자원이 없으면 overflow 만 늘어나므로).
 */
static void blk_mq_update_dispatch_busy(struct blk_mq_hw_ctx *hctx, bool busy)
{
	unsigned int ewma;

	/* [한국어] 현재 EWMA 값 로드 */
	ewma = hctx->dispatch_busy;

	/* [한국어] 이전에도, 지금도 busy 가 아니면 0 유지 (noop) */
	if (!ewma && !busy)
		return;

	/* [한국어] EWMA = (ewma * 7 + new_sample) / 8
	 * new_sample: busy=true → 16, busy=false → 0 */
	ewma *= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT - 1;
	if (busy)	/* [한국어] 이번 dispatch 에서 자원 부족이 있었다면 새 샘플로 1<<4 = 16 을 더한다 */
		ewma += 1 << BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR;	/* [한국어] busy 샘플 가산 — 이 상수가 클수록 한 번의 busy 가 EWMA 를 크게 밀어 올린다 */
	ewma /= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT;	/* [한국어] 가중치 8 로 나눠 EWMA 를 완성한다. 결과적으로 새 샘플이 1/8, 과거 값이 7/8 비중을 갖는다 */

	/* [한국어] 갱신된 EWMA 저장 — 다음 dispatch 경로 결정에 반영 */
	hctx->dispatch_busy = ewma;
}

/* [한국어] BLK_MQ_RESOURCE_DELAY - 드라이버가 자원 부족을 알렸을 때의 재시도 지연(ms).
 * 장치 종류와 무관한 블록 계층 공통 상수다. blk_mq_dispatch_rq_list() 가
 * BLK_STS_DEV_RESOURCE 등으로 dispatch 를 접었을 때, 이 값(3ms)만큼 뒤에
 * blk_mq_delay_run_hw_queue() 로 해당 hctx 를 다시 돌린다. "자원이 돌아오면
 * 누군가 깨워 준다"는 보장이 없는 경우의 안전망이므로, 너무 짧으면 CPU 를
 * 태우고 너무 길면 지연이 커진다. */
#define BLK_MQ_RESOURCE_DELAY	3		/* ms units */

/*
 * [한국어]
 * blk_mq_handle_dev_resource - NVMe controller 자원 부족(BLK_STS_DEV_RESOURCE) 처리
 *
 * @rq:   reject 된 request
 * @list: dispatch 재시도 대기 list
 *
 * nvme_queue_rq 가 BLK_STS_DEV_RESOURCE 를 반환하면 이 함수로 들어온다.
 * request 를 dispatch list 앞으로 돌려보내고 driver tag 를 반납한다.
 */
static void blk_mq_handle_dev_resource(struct request *rq,
				       struct list_head *list)
{
	/* [한국어] list_add: dispatch list 맨 앞에 삽입 — 다음 run 에서 우선 재발행 */
	list_add(&rq->queuelist, list);
	/* [한국어] driver tag(NVMe CID) 반납 및 request 상태 초기화 */
	__blk_mq_requeue_request(rq);
}

/*
 * [한국어]
 * enum prep_dispatch - blk_mq_prep_dispatch_rq 의 결과 코드
 *
 * dispatch 준비 단계에서 budget 또는 tag 획득 여부를 나타낸다.
 * blk_mq_dispatch_rq_list 가 이 값으로 request 처리 방식을 결정한다.
 */
enum prep_dispatch {
	PREP_DISPATCH_OK,        /* [한국어] budget 과 tag 모두 확보 성공 */
	PREP_DISPATCH_NO_TAG,    /* [한국어] NVMe CID 할당 실패 (SQ full) */
	PREP_DISPATCH_NO_BUDGET, /* [한국어] dispatch budget 고갈 (queue depth 제한) */
};

/*
 * [한국어]
 * blk_mq_prep_dispatch_rq - request 하나의 dispatch 를 위해 budget 과 tag 준비
 *
 * @rq:          준비할 request
 * @need_budget: true 이면 dispatch budget(token)도 획득 필요
 * @return:      PREP_DISPATCH_OK / NO_TAG / NO_BUDGET
 *
 * blk_mq_dispatch_rq_list 에서 request 하나마다 호출되는 준비 단계.
 * budget(SQ throttle token) → driver tag(CID) 순서로 획득.
 * 실패 시 이미 획득한 자원을 되돌리고 실패 이유를 반환한다.
 */
static enum prep_dispatch blk_mq_prep_dispatch_rq(struct request *rq,
						  bool need_budget)
{
	/* [한국어] hctx: 이 request 가 발행될 NVMe SQ 에 대응하는 hw queue */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	/* [한국어] budget_token: dispatch budget 획득 결과 (음수면 실패) */
	int budget_token = -1;

	if (need_budget) {	/* [한국어] 드라이버가 get_budget/put_budget 을 등록했고 아직 예산을 잡지 않았으면 — SCSI 처럼 호스트 전역 자원 한도가 있는 드라이버가 쓴다. NVMe PCIe 는 이 콜백이 없어 이 블록을 건너뛴다 */
		/* [한국어] blk_mq_get_dispatch_budget: queue depth / throttle 예산 확인 */
		budget_token = blk_mq_get_dispatch_budget(rq->q);
		if (budget_token < 0) {	/* [한국어] 예산 고갈 — 장치가 더 받을 수 없다는 뜻 */
			/* [한국어] budget 고갈: 이미 확보한 driver tag 반납 후 NO_BUDGET 반환 */
			blk_mq_put_driver_tag(rq);
			return PREP_DISPATCH_NO_BUDGET;	/* [한국어] 예산이 없어 발행하지 못했음을 호출자에게 알린다. 호출자는 이 경우 재실행을 예약하지 않는다(예산이 풀리면 그쪽에서 깨워 준다) */
		}
		/* [한국어] budget_token 을 request 에 저장 — 완료 시 put_budget 에서 반납 */
		blk_mq_set_rq_budget_token(rq, budget_token);
	}

	if (!blk_mq_get_driver_tag(rq)) {	/* [한국어] 실제 하드웨어 슬롯(NVMe 라면 CID 하위 12비트)을 잡는다. 스케줄러를 거친 요청은 여기서 처음 드라이버 태그를 받는다 */
		/* [한국어] NVMe CID 확보 실패: SQ 가 가득 찼음 */
		/*
		 * The initial allocation attempt failed, so we need to
		 * rerun the hardware queue when a tag is freed. The
		 * waitqueue takes care of that. If the queue is run
		 * before we add this entry back on the dispatch list,
		 * we'll re-run it below.
		 */
		if (!blk_mq_mark_tag_wait(hctx, rq)) {
			/* [한국어] 대기 등록 후에도 CID 없음: budget token 도 반납 후 NO_TAG */
			/*
			 * All budgets not got from this function will be put
			 * together during handling partial dispatch
			 */
			if (need_budget)
				/* [한국어] budget 반납 — 다음 재시도를 위해 */
				blk_mq_put_dispatch_budget(rq->q, budget_token);
			return PREP_DISPATCH_NO_TAG;	/* [한국어] 태그를 못 얻었고 대기 등록도 실패 — 호출자는 이 요청을 dispatch 리스트에 되돌려 놓는다 */
		}
	}

	return PREP_DISPATCH_OK;	/* [한국어] 예산과 드라이버 태그를 모두 확보 — 이제 mq_ops->queue_rq 를 부를 수 있다 */
}

/* release all allocated budgets before calling to blk_mq_dispatch_rq_list */
/*
 * [한국어]
 * blk_mq_release_budgets - list 의 모든 request 에 대해 dispatch budget 반납
 *
 * @q:    대상 request_queue
 * @list: budget 을 반납할 request 들의 list
 *
 * blk_mq_dispatch_rq_list 가 실패로 중단될 때 미리 확보한 budget token 들을 반납한다.
 */
static void blk_mq_release_budgets(struct request_queue *q,
		struct list_head *list)
{
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {	/* [한국어] 이번 배치에서 아직 발행되지 못한 request 들을 순회하며 예산을 되돌린다 */
		/* [한국어] 각 request 의 budget_token 을 읽어 put */
		int budget_token = blk_mq_get_rq_budget_token(rq);

		/* [한국어] budget_token < 0 이면 budget 없음 (passthrough 등) — 건너뜀 */
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
/*
 * [한국어]
 * blk_mq_commit_rqs - batch dispatch 완료를 드라이버에 통보
 *
 * @hctx:          완료 통보할 hardware queue
 * @queued:        이번 batch 에서 발행한 request 수
 * @from_schedule: kblockd workqueue 에서 실행 중이면 true
 *
 * mq_ops->commit_rqs 콜백을 호출해 드라이버에게 "이 묶음은 여기서 끝났다"고
 * 알린다. queued == 0 이면 아무것도 안 실렸다는 뜻이라 부르지 않는다.
 *
 * === 왜 이 함수가 존재하는가 ===
 * @last 규약은 "마지막 request 에 last=true 를 준다"이지만, 배치가 정상적으로
 * 끝나지 않는 경우가 있다: 예정한 것을 다 못 넣었거나(자원 부족으로 중단),
 * 마지막 시도가 실패했거나, 다음 request 가 다른 hctx 로 가는 경우다. 그러면
 * last=true 를 받은 request 가 하나도 없어, 드라이버가 미뤄 둔 알림을 영원히
 * 보내지 않고 요청이 하드웨어에서 시작되지 않는 정지 상태가 된다.
 * 이 함수가 그 구멍을 막는 명시적 플러시다.
 *
 * NVMe PCIe 접점 (drivers/nvme/host/pci.c 실물 확인):
 *   nvme_mq_ops.commit_rqs == nvme_commit_rqs
 *   nvme_commit_rqs(hctx) 는 hctx->driver_data 로 nvme_queue 를 꺼내
 *   sq_lock 을 잡고, sq_tail(지금까지 SQ 링에 쓴 위치)과 last_sq_tail
 *   (마지막으로 doorbell 로 알린 위치)이 다를 때만
 *   nvme_write_sq_db(nvmeq, true) 로 SQ tail doorbell MMIO 를 강제한다.
 *   두 값이 같으면 컨트롤러가 이미 다 알고 있다는 뜻이라 아무 일도 하지 않는다.
 *   즉 이 콜백은 멱등하고 값도 싸다.
 *
 * 참고: nvme_mq_admin_ops 에는 commit_rqs 가 없다. admin 큐는 배치 제출
 * (queue_rqs)을 하지 않아 미뤄 둔 doorbell 이 생기지 않기 때문이다.
 */
static void blk_mq_commit_rqs(struct blk_mq_hw_ctx *hctx, int queued,
			      bool from_schedule)
{
	if (hctx->queue->mq_ops->commit_rqs && queued) {
		/* [한국어] block_unplug tracepoint: blktrace/eBPF 가 batch dispatch 끝을 추적.
		 * !from_schedule: 동기 unplug 이면 true */
		trace_block_unplug(hctx->queue, queued, !from_schedule);
		/* [한국어] mq_ops->commit_rqs: 드라이버에게 batch 완료 통보.
		 * NVMe 드라이버에서 doorbell 쓰기를 여기서 일괄 처리할 수 있음 */
		hctx->queue->mq_ops->commit_rqs(hctx);
	}
}

/*
 * Returns true if we did some work AND can potentially do more.
 */
/*
 * [한국어]
 * blk_mq_dispatch_rq_list - dispatch list 의 request 들을 드라이버 queue_rq 로 전달
 *
 * @hctx:       dispatch 대상 hardware queue (NVMe SQ 에 대응)
 * @list:       dispatch 할 request 들의 list
 * @get_budget: true 이면 각 request 마다 dispatch budget 도 획득
 * @return:     true 이면 일부 또는 전부를 dispatch 했고 더 할 가능성 있음
 *
 * blk-mq dispatch 의 핵심 함수. budget 과 driver tag(CID) 를 확보한 후
 * mq_ops->queue_rq(hctx, &bd) → nvme_queue_rq → SQ doorbell 로 이어진다.
 * BLK_STS_RESOURCE/DEV_RESOURCE 시 지연 재시도(requeue_list), NO_TAG 시 대기 등록.
 * batch dispatch 마지막에 commit_rqs 로 NVMe doorbell 을 일괄 기록.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue → blk_mq_sched_dispatch_requests
 *   → [blk_mq_dispatch_rq_list] → mq_ops->queue_rq (NVMe PCIe: nvme_queue_rq)
 *   → nvme_sq_copy_cmd(SQE 기록) → nvme_write_sq_db(bd->last 이면 doorbell)
 */
bool blk_mq_dispatch_rq_list(struct blk_mq_hw_ctx *hctx, struct list_head *list,
			     bool get_budget)
{
	enum prep_dispatch prep;
	struct request_queue *q = hctx->queue;	/* [한국어] mq_ops 와 budget 콜백을 꺼내기 위한 큐 포인터 — 루프에서 반복 사용하므로 캐시해 둔다 */
	struct request *rq;	/* [한국어] 루프에서 하나씩 꺼내 드라이버에 넘길 request 커서 */
	int queued;	/* [한국어] 이번 호출에서 드라이버가 실제로 받아 간 개수. commit_rqs 를 부를지와 dispatch_busy 갱신에 쓰인다 */
	blk_status_t ret = BLK_STS_OK;	/* [한국어] 드라이버가 마지막으로 돌려준 상태. 루프를 빠져나온 뒤 재시도 정책을 정하는 데 쓰인다 */
	bool needs_resource = false;	/* [한국어] BLK_STS_RESOURCE 를 한 번이라도 봤는지 — 봤다면 아무도 안 깨워 줄 수 있으므로 타이머 기반 재시도가 필요하다 */

	/* [한국어] dispatch list 가 비었으면 할 일 없음 */
	if (list_empty(list))
		return false;

	/*
	 * Now process all the entries, sending them to the driver.
	 */
	/* [한국어] queued: 이번 batch 에서 성공적으로 발행한 request 수 */
	queued = 0;
	do {
		/* [한국어] bd: 드라이버(nvme_queue_rq)에 전달하는 dispatch 컨텍스트 */
		struct blk_mq_queue_data bd;

		/* [한국어] list 첫 번째 request 를 꺼내 dispatch 준비 */
		rq = list_first_entry(list, struct request, queuelist);

		/* [한국어] request 의 hctx 가 현재 hctx 와 일치해야 함 (코딩 오류 감지) */
		WARN_ON_ONCE(hctx != rq->mq_hctx);
		/* [한국어] blk_mq_prep_dispatch_rq: budget(SQ throttle) + tag(NVMe CID) 확보 */
		prep = blk_mq_prep_dispatch_rq(rq, get_budget);
		if (prep != PREP_DISPATCH_OK)	/* [한국어] 예산 또는 드라이버 태그를 못 얻었다 — 이 request 는 리스트에 그대로 둔 채 루프를 끝낸다 */
			/* [한국어] NO_TAG 또는 NO_BUDGET: 자원 부족으로 dispatch 중단 */
			break;

		/* [한국어] driver 에 전달할 request 를 dispatch list 에서 분리 */
		list_del_init(&rq->queuelist);

		/* [한국어] bd.rq: nvme_queue_rq 에 전달될 request (NVMe SQE 작성 대상) */
		bd.rq = rq;
		/* [한국어] bd.last: 이 request 다음 list 가 비면 true.
		 * 드라이버는 last==true 일 때 NVMe doorbell 을 기록한다. */
		bd.last = list_empty(list);

		/* [한국어] mq_ops->queue_rq: nvme_queue_rq → SQ entry 기록 → doorbell.
		 * 이 호출 이후 NVMe 컨트롤러가 명령을 실행하기 시작한다. */
		ret = q->mq_ops->queue_rq(hctx, &bd);
		switch (ret) {	/* [한국어] 드라이버가 돌려준 상태 코드에 따라 분기 */
		case BLK_STS_OK:	/* [한국어] 드라이버가 요청을 받아 갔다(NVMe 라면 SQ 링에 SQE 가 실렸다) */
			/* [한국어] NVMe SQ 에 명령 성공적 배치 — CQ 완료 인터럽트 대기 */
			queued++;
			break;
		case BLK_STS_RESOURCE:	/* [한국어] 드라이버 쪽 자원 부족 */
			/* [한국어] BLK_STS_RESOURCE — "드라이버 쪽" 자원 부족. 이 요청을
			 * 완료해 줄 다른 in-flight 요청이 없을 수도 있으므로, 아무도
			 * 깨워 주지 않을 가능성에 대비해 needs_resource = true 로 표시하고
			 * 나중에 BLK_MQ_RESOURCE_DELAY(3ms) 뒤 재실행을 예약한다.
			 * NVMe PCIe 에서 이 값이 나오는 실제 지점: nvme_map_data() 가
			 * PRP 리스트/SGL 디스크립터용 dma_pool 을 못 얻거나 DMA 매핑에
			 * 실패했을 때. 태그는 이미 받았으므로 태그 고갈은 원인이 아니다. */
			needs_resource = true;
			fallthrough;	/* [한국어] 아래 DEV_RESOURCE 와 처리(요청 되돌리기)는 같으므로 그대로 흘려보낸다 */
		case BLK_STS_DEV_RESOURCE:	/* [한국어] 장치 쪽 자원 부족 — 진행 중인 요청이 완료되면 반드시 풀린다 */
			/* [한국어] BLK_STS_DEV_RESOURCE — "장치 쪽" 자원 부족. RESOURCE 와
			 * 다른 점은 "지금 장치에서 처리 중인 요청이 완료되면 반드시 자원이
			 * 풀린다"는 보장이 있다는 것이다. 그래서 타이머로 재시도를 예약할
			 * 필요가 없고, 완료 경로가 자연스럽게 큐를 다시 돌려 준다.
			 * blk_mq_handle_dev_resource(): rq 를 list 맨 앞에 되돌려 놓고
			 * __blk_mq_requeue_request() 로 드라이버 태그를 반납해, 그 태그가
			 * 다른 요청의 진행에 쓰일 수 있게 한다. */
			blk_mq_handle_dev_resource(rq, list);
			goto out;	/* [한국어] 자원 부족으로 이번 배치를 중단 — 루프 밖 공통 마무리(commit_rqs, 재시도 예약)로 간다 */
		default:
			/* [한국어] 기타 오류 (BLK_STS_IOERR 등): 즉시 request 완료 처리.
			 * 상위 계층(VFS/io_uring)에 error 를 전파 */
			blk_mq_end_request(rq, ret);
		}
	} while (!list_empty(list));
out:
	/* If we didn't flush the entire list, we could have told the driver
	 * there was more coming, but that turned out to be a lie.
	 */
	/* [한국어] list 가 남거나 오류가 있으면 commit_rqs 로 batch 마감 통보.
	 * 드라이버에게 "더 이상 request 없음(bd.last 정정)" 을 알리고 doorbell 기록 유도 */
	if (!list_empty(list) || ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false);

	/*
	 * Any items that need requeuing? Stuff them into hctx->dispatch,
	 * that is where we will continue on next queue run.
	 */
	if (!list_empty(list)) {	/* [한국어] 리스트에 처리하지 못한 request 가 남았는가 — 남았다면 hctx->dispatch 로 되돌리고 재실행 전략을 정해야 한다 */
		bool needs_restart;	/* [한국어] 스케줄러가 RESTART 를 걸어 두었는지 — 걸려 있으면 태그/예산이 풀릴 때 자동으로 재실행된다 */
		/* For non-shared tags, the RESTART check will suffice */
		/* [한국어] no_tag: tag 부족이면서 공유 tag 를 사용하는 경우.
		 * 비공유 tag 에서는 RESTART 플래그만으로 충분하지만,
		 * 공유 tag 는 dispatch wait queue 도 체크해야 한다. */
		bool no_tag = prep == PREP_DISPATCH_NO_TAG &&
			((hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) ||
			blk_mq_is_shared_tags(hctx->flags));

		/*
		 * If the caller allocated budgets, free the budgets of the
		 * requests that have not yet been passed to the block driver.
		 */
		/* [한국어] get_budget==false: caller 가 이미 budget 을 확보했다면
		 * 미처 dispatch 하지 못한 request 의 budget 을 여기서 반납 */
		if (!get_budget)
			blk_mq_release_budgets(q, list);

		/* [한국어] hctx->dispatch: 다음 run 에서 재처리할 request list.
		 * hctx->lock 으로 보호 */
		spin_lock(&hctx->lock);
		/* [한국어] list_splice_tail_init: 남은 request 들을 hctx->dispatch 끝에 append */
		list_splice_tail_init(list, &hctx->dispatch);
		spin_unlock(&hctx->lock);	/* [한국어] dispatch 리스트에 되돌려 넣기가 끝났으므로 hctx 락 해제 */

		/*
		 * Order adding requests to hctx->dispatch and checking
		 * SCHED_RESTART flag. The pair of this smp_mb() is the one
		 * in blk_mq_sched_restart(). Avoid restart code path to
		 * miss the new added requests to hctx->dispatch, meantime
		 * SCHED_RESTART is observed here.
		 */
		/* [한국어] smp_mb(): hctx->dispatch 삽입과 SCHED_RESTART 플래그 체크 사이
		 * 메모리 순서 보장. blk_mq_sched_restart 의 smp_mb 와 쌍을 이룬다.
		 * 순서가 뒤집히면 restart 코드가 dispatch 에 추가된 request 를 놓칠 수 있다. */
		smp_mb();

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
		/* [한국어] SCHED_RESTART: elevator 가 이미 재시작을 예약했는지 확인 */
		needs_restart = blk_mq_sched_needs_restart(hctx);
		/* [한국어] NO_BUDGET 도 resource 부족으로 간주: 지연 재시도 필요 */
		if (prep == PREP_DISPATCH_NO_BUDGET)
			needs_resource = true;
		if (!needs_restart ||	/* [한국어] RESTART 가 걸려 있지 않거나 */
		    (no_tag && list_empty_careful(&hctx->dispatch_wait.entry)))	/* [한국어] 태그가 없어서 멈췄는데 대기 큐 등록조차 안 된 경우 — 둘 다 "아무도 깨워 주지 않는" 상황이므로 직접 재실행해야 한다 */
			/* [한국어] SCHED_RESTART 가 없거나 no_tag + dispatch_wait 비어있으면
			 * 즉시 hctx 를 rerun — dispatch 에 추가된 request 를 바로 처리 */
			blk_mq_run_hw_queue(hctx, true);
		else if (needs_resource)	/* [한국어] RESTART 는 있지만 드라이버 자원 부족이었던 경우 */
			/* [한국어] RESOURCE 부족이면 BLK_MQ_RESOURCE_DELAY(3ms) 후 rerun.
			 * NVMe 컨트롤러 자원이 회복될 시간을 준다 */
			blk_mq_delay_run_hw_queue(hctx, BLK_MQ_RESOURCE_DELAY);

		/* [한국어] dispatch busy EWMA 증가: 자원 부족 상황 기록 */
		blk_mq_update_dispatch_busy(hctx, true);
		return false;
	}

	/* [한국어] 전체 list 를 소진: dispatch busy EWMA 감소 */
	blk_mq_update_dispatch_busy(hctx, false);
	return true;
}

/*
 * [한국어]
 * blk_mq_first_mapped_cpu - hctx 의 cpumask 에서 첫 번째 온라인 CPU 반환
 *
 * @hctx: 대상 hw queue
 * @return: hctx->cpumask 에서 cpu_online_mask 와 교집합의 첫 번째 CPU.
 *          온라인 CPU 가 없으면 cpumask 의 첫 번째 CPU(offline 포함).
 *
 * blk_mq_delay_run_hw_queue / blk_mq_run_hw_queue 가 work 를 어느 CPU 에 예약할지
 * 결정할 때 사용한다. NVMe IRQ affinity 와 일치하는 CPU 에 work 를 보냄으로써
 * cache locality 를 높인다.
 */
static inline int blk_mq_first_mapped_cpu(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] cpumask_first_and: hctx->cpumask ∩ cpu_online_mask 의 첫 CPU */
	int cpu = cpumask_first_and(hctx->cpumask, cpu_online_mask);

	/* [한국어] 온라인 교집합이 없으면 cpumask 의 첫 CPU (offline 일 수도 있음) */
	if (cpu >= nr_cpu_ids)
		cpu = cpumask_first(hctx->cpumask);
	return cpu;	/* [한국어] hctx 에 매핑된 CPU 중 하나를 반환 — 폴백이라 오프라인일 수도 있다 */
}

/*
 * ->next_cpu is always calculated from hctx->cpumask, so simply use
 * it for speeding up the check
 */
/*
 * [한국어]
 * blk_mq_hctx_empty_cpumask - hctx 에 매핑된 CPU 가 없는지 확인
 *
 * @hctx: 검사할 hw queue
 * @return: true 이면 hctx->cpumask 가 비어있음 (unmapped hctx)
 *
 * hctx->next_cpu 는 항상 cpumask 에서 계산되므로 next_cpu >= nr_cpu_ids 이면
 * cpumask 가 비어있음을 의미한다.
 */
static bool blk_mq_hctx_empty_cpumask(struct blk_mq_hw_ctx *hctx)
{
        /* [한국어] next_cpu >= nr_cpu_ids: 유효한 CPU 없음 → cpumask 비어있음 */
        return hctx->next_cpu >= nr_cpu_ids;
}

/*
 * It'd be great if the workqueue API had a way to pass
 * in a mask and had some smarts for more clever placement.
 * For now we just round-robin here, switching for every
 * BLK_MQ_CPU_WORK_BATCH queued items.
 */
/*
 * [한국어]
 * blk_mq_hctx_next_cpu - 다음 dispatch work 를 실행할 CPU 를 round-robin 으로 선택
 *
 * @hctx: 대상 hardware queue
 * @return: 선택된 CPU 번호 또는 WORK_CPU_UNBOUND
 *
 * kblockd workqueue 에 dispatch work 를 예약할 때 어느 CPU 에 보낼지 결정한다.
 * hctx->cpumask 내에서 CPU 를 순환하며 NVMe IRQ affinity 와 cache locality 를 맞춘다.
 * BLK_MQ_CPU_WORK_BATCH 개마다 다음 CPU 로 이동하는 round-robin 방식.
 * 해당 CPU 가 오프라인이면 WORK_CPU_UNBOUND 를 반환하여 어떤 CPU 에서든 실행 가능.
 */
static int blk_mq_hctx_next_cpu(struct blk_mq_hw_ctx *hctx)
{
	bool tried = false;
	int next_cpu = hctx->next_cpu;	/* [한국어] 지난번에 고른 CPU 에서 이어서 순회한다. 매번 처음부터 돌면 앞쪽 CPU 에만 work 가 몰린다 */

	/* Switch to unbound if no allowable CPUs in this hctx */
	/* [한국어] SQ 가 1개이거나 cpumask 가 비어있으면 CPU 를 고집할 필요 없음 */
	if (hctx->queue->nr_hw_queues == 1 || blk_mq_hctx_empty_cpumask(hctx))
		return WORK_CPU_UNBOUND;

	if (--hctx->next_cpu_batch <= 0) {	/* [한국어] batch 카운터를 하나 내리고 0 이하가 되면 — 즉 같은 CPU 에서 BLK_MQ_CPU_WORK_BATCH 번 실행했으면 다음 CPU 로 옮긴다 */
select_cpu:
		/* [한국어] cpumask_next_and: hctx->cpumask ∩ cpu_online_mask 에서
		 * next_cpu 다음 CPU 를 선택 (round-robin, wrap-around 포함) */
		next_cpu = cpumask_next_and(next_cpu, hctx->cpumask,
				cpu_online_mask);
		if (next_cpu >= nr_cpu_ids)	/* [한국어] cpumask 끝을 넘었다 — 라운드로빈이므로 처음으로 되돌린다 */
			/* [한국어] cpumask 끝에 도달하면 첫 번째 CPU 로 돌아감 */
			next_cpu = blk_mq_first_mapped_cpu(hctx);
		/* [한국어] BLK_MQ_CPU_WORK_BATCH: 같은 CPU 에서 실행할 batch 크기 초기화 */
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}

	/*
	 * Do unbound schedule if we can't find a online CPU for this hctx,
	 * and it should only happen in the path of handling CPU DEAD.
	 */
	if (!cpu_online(next_cpu)) {	/* [한국어] 고른 CPU 가 오프라인인가. CPU DEAD 처리 도중에만 일어날 수 있다(위 영문 주석) */
		if (!tried) {	/* [한국어] 아직 재선택을 안 해 봤다면 한 번만 더 시도한다 — 무한 루프를 막기 위해 딱 한 번이다 */
			/* [한국어] 선택된 CPU 가 오프라인: 한 번 더 select 시도 */
			tried = true;
			goto select_cpu;	/* [한국어] 재선택 지점으로 되돌아간다 */
		}

		/*
		 * Make sure to re-select CPU next time once after CPUs
		 * in hctx->cpumask become online again.
		 */
		/* [한국어] 모든 cpumask CPU 가 오프라인: unbound 로 fallback.
		 * next_cpu_batch=1: 다음 호출 시 즉시 재선택하여 온라인 CPU 를 찾을 기회 */
		hctx->next_cpu = next_cpu;
		hctx->next_cpu_batch = 1;	/* [한국어] 다음 호출에서 곧바로 재선택하도록 batch 를 1 로 낮춘다. 그래야 CPU 가 다시 온라인이 되면 즉시 바인딩을 회복한다 */
		return WORK_CPU_UNBOUND;	/* [한국어] 바인딩을 포기하고 아무 CPU 에서나 실행하도록 워크큐에 맡긴다 */
	}

	/* [한국어] 선택된 CPU 저장: 다음 BLK_MQ_CPU_WORK_BATCH 개 work 에서 재사용 */
	hctx->next_cpu = next_cpu;
	return next_cpu;	/* [한국어] 이 CPU 에 work 를 바인딩한다. NVMe 라면 이 큐의 MSI-X affinity 와 같은 CPU 라 캐시 지역성이 유지된다 */
}

/**
 * blk_mq_delay_run_hw_queue - Run a hardware queue asynchronously.
 * @hctx: Pointer to the hardware queue to run.
 * @msecs: Milliseconds of delay to wait before running the queue.
 *
 * Run a hardware queue asynchronously with a delay of @msecs.
 */
/*
 * [한국어]
 * blk_mq_delay_run_hw_queue - hctx 의 dispatch 를 kblockd 에 지연 예약
 *
 * @hctx: 실행할 hardware queue
 * @msecs: 지연 시간 (밀리초; 0 이면 즉시 예약)
 *
 * blk_mq_run_hw_queue 의 async 버전. kblockd workqueue 에 hctx->run_work 를 예약한다.
 * BLK_STS_RESOURCE 후 BLK_MQ_RESOURCE_DELAY(3ms) 재시도, 또는 즉시 예약 (0ms) 에 사용.
 * BLK_MQ_S_STOPPED 상태이면 아무것도 예약하지 않는다. 이 비트는
 * blk_mq_stop_hw_queue()/blk_mq_start_hw_queue() 쌍으로만 오르내리며,
 * 그 사이에 지연 실행을 걸어 두면 정지 의도를 깨기 때문이다.
 */
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs)
{
	/* [한국어] BLK_MQ_S_STOPPED: 드라이버가 이 하드웨어 큐의 dispatch 를 명시적으로
	 * 멈춰 둔 상태. 이 상태에서는 재시작(blk_mq_start_hw_queue)이 있기 전까지
	 * 어떤 지연 실행도 예약하지 않는다.
	 * 참고: NVMe 는 리셋/셧다운 때 이 STOPPED 비트가 아니라 quiesce
	 * (nvme_quiesce_io_queues → blk_mq_quiesce_tagset)를 쓴다. 둘은 다른
	 * 메커니즘이다 — STOPPED 는 hctx 단위 dispatch 정지, quiesce 는 큐 단위
	 * 정지 + 진행 중 dispatch 의 grace period 대기까지 포함한다. */
	if (unlikely(blk_mq_hctx_stopped(hctx)))
		return;
	/* [한국어] blk_mq_hctx_next_cpu: round-robin 으로 target CPU 선택.
	 * hctx->run_work 를 해당 CPU 의 kblockd 에 msecs 후 예약 */
	kblockd_mod_delayed_work_on(blk_mq_hctx_next_cpu(hctx), &hctx->run_work,
				    msecs_to_jiffies(msecs));
}
EXPORT_SYMBOL(blk_mq_delay_run_hw_queue);

/*
 * [한국어]
 * blk_mq_hw_queue_need_run - hctx 가 dispatch 를 실행해야 하는지 확인
 *
 * @hctx: 검사할 hardware queue
 * @return: true 이면 quiesced 아니고 pending request 있음 → run 필요
 *
 * blk_mq_run_hw_queue 에서 불필요한 run 을 방지하기 위해 먼저 확인.
 * __blk_mq_run_dispatch_ops 로 dispatch ops 잠금 구간 내에서 safe 하게 체크.
 */
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
	/* [한국어] __blk_mq_run_dispatch_ops: dispatch ops 구간에서 안전하게 체크.
	 * false: read-only 체크 (dispatch 실행 안 함).
	 * quiesced 이면 hctx_has_pending 호출도 안전하지 않으므로 함께 검사 */
	__blk_mq_run_dispatch_ops(hctx->queue, false,
		need_run = !blk_queue_quiesced(hctx->queue) &&
		blk_mq_hctx_has_pending(hctx));
	return need_run;	/* [한국어] true 면 이 hctx 를 실제로 돌릴 가치가 있다는 뜻 */
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
 * [한국어]
 * blk_mq_run_hw_queue - hctx 에 pending request 가 있으면 dispatch 실행
 *
 * @hctx:  실행할 hardware queue (NVMe SQ 에 대응)
 * @async: true 이면 kblockd workqueue 에서 비동기 실행
 *
 * blk-mq dispatch 의 진입점 중 하나.
 * quiesce 상태나 pending 없으면 즉시 반환.
 * async=false 이고 현재 CPU 가 hctx->cpumask 에 있으면 바로 dispatch 실행.
 * 그 외에는 blk_mq_delay_run_hw_queue 로 kblockd 에 위임.
 * BLK_MQ_F_BLOCKING 이면 sleep 가능 (might_sleep_if).
 *
 * 호출 체인:
 *   blk_mq_submit_bio / blk_mq_dispatch_wake / requeue → [blk_mq_run_hw_queue]
 *   → blk_mq_sched_dispatch_requests → blk_mq_dispatch_rq_list → nvme_queue_rq
 */
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
	bool need_run;

	/*
	 * We can't run the queue inline with interrupts disabled.
	 */
	/* [한국어] sync run 은 인터럽트 비활성화 컨텍스트에서 불가 (sleep/spin 문제) */
	WARN_ON_ONCE(!async && in_interrupt());

	/* [한국어] BLK_MQ_F_BLOCKING: NVMe poll 큐 등에서 sleep 허용 표시 */
	might_sleep_if(!async && hctx->flags & BLK_MQ_F_BLOCKING);

	/* [한국어] 1차 lockless 확인: quiesced 아니고 pending 있어야 run */
	need_run = blk_mq_hw_queue_need_run(hctx);
	if (!need_run) {	/* [한국어] 1차 lockless 검사에서 "돌릴 필요 없음"이 나왔다 — 하지만 그 사이 unquiesce 가 일어났을 수 있어 락을 잡고 한 번 더 본다 */
		unsigned long flags;	/* [한국어] spin_lock_irqsave 가 저장할 IRQ 상태 */

		/*
		 * Synchronize with blk_mq_unquiesce_queue(), because we check
		 * if hw queue is quiesced locklessly above, we need the use
		 * ->queue_lock to make sure we see the up-to-date status to
		 * not miss rerunning the hw queue.
		 */
		/* [한국어] 2차 확인: queue_lock 으로 unquiesce_queue 와 동기화.
		 * lockless 체크 후 quiesce 가 해제되는 race 를 방지 */
		spin_lock_irqsave(&hctx->queue->queue_lock, flags);
		need_run = blk_mq_hw_queue_need_run(hctx);	/* [한국어] 락을 쥔 상태에서 재확인 — 이제 quiesce 해제와의 경쟁이 없다 */
		spin_unlock_irqrestore(&hctx->queue->queue_lock, flags);	/* [한국어] 재확인이 끝났으므로 락 해제 + IRQ 상태 복원 */

		if (!need_run)	/* [한국어] 락을 잡고 봐도 여전히 돌릴 것이 없으면 */
			return;
	}

	if (async || !cpumask_test_cpu(raw_smp_processor_id(), hctx->cpumask)) {	/* [한국어] async 요청이거나 현재 CPU 가 이 hctx 의 cpumask 밖이면 — 동기 실행하면 엉뚱한 CPU/NUMA 노드에서 돌게 되므로 kblockd 워커에 넘긴다 */
		/* [한국어] async 이거나 현재 CPU 가 hctx cpumask 밖이면 kblockd 에 위임.
		 * delay=0: 즉시 예약 */
		blk_mq_delay_run_hw_queue(hctx, 0);
		return;
	}

	/* [한국어] sync run: 현재 CPU 에서 바로 blk_mq_sched_dispatch_requests 호출.
	 * blk_mq_run_dispatch_ops: dispatch ops srcu lock 하에서 실행 */
	blk_mq_run_dispatch_ops(hctx->queue,
				blk_mq_sched_dispatch_requests(hctx));
}
EXPORT_SYMBOL(blk_mq_run_hw_queue);

/*
 * Return prefered queue to dispatch from (if any) for non-mq aware IO
 * scheduler.
 */
/*
 * [한국어]
 * blk_mq_get_sq_hctx - single-queue IO scheduler 를 위한 기본 dispatch hctx 반환
 *
 * @q: 대상 request_queue
 * @return: 현재 CPU 의 DEFAULT hctx (stopped 가 아니면), NULL 이면 scheduler 선택
 *
 * mq-aware 하지 않은 IO scheduler (예: CFQ 호환) 는 하나의 hctx 만 사용한다.
 * 여러 hctx 로 dispatch 하면 scheduler 내부에서 lock contention 과 cache bounce 가 발생.
 * 현재 CPU 의 HCTX_TYPE_DEFAULT hctx 를 반환하여 이 문제를 피한다.
 */
static struct blk_mq_hw_ctx *blk_mq_get_sq_hctx(struct request_queue *q)
{
	/* [한국어] blk_mq_get_ctx: 현재 CPU 의 per-CPU sw queue 반환 */
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
	/*
	 * If the IO scheduler does not respect hardware queues when
	 * dispatching, we just don't bother with multiple HW queues and
	 * dispatch from hctx for the current CPU since running multiple queues
	 * just causes lock contention inside the scheduler and pointless cache
	 * bouncing.
	 */
	/* [한국어] 현재 CPU 의 HCTX_TYPE_DEFAULT hctx: IO scheduler 에서 dispatch 할 대상 */
	struct blk_mq_hw_ctx *hctx = ctx->hctxs[HCTX_TYPE_DEFAULT];

	/* [한국어] stopped 상태이면 NULL 반환 → blk_mq_run_hw_queues 가 다른 hctx 사용 */
	if (!blk_mq_hctx_stopped(hctx))
		return hctx;
	return NULL;	/* [한국어] 기본 hctx 가 정지 상태 — 호출자가 전체 hctx 순회로 폴백하게 NULL 을 준다 */
}

/**
 * blk_mq_run_hw_queues - Run all hardware queues in a request queue.
 * @q: Pointer to the request queue to run.
 * @async: If we want to run the queue asynchronously.
 */
/*
 * [한국어]
 * blk_mq_run_hw_queues - request_queue 의 모든 hw queue (NVMe SQ) 를 실행
 *
 * @q:     대상 request_queue
 * @async: true 이면 kblockd workqueue 를 통해 비동기로 실행
 *
 * NVMe 컨트롤러의 여러 SQ(IO queue, poll queue 등)에 걸쳐 pending request 를 dispatch.
 * single-queue IO scheduler 면 기본 hctx 만, 그 외에는 모든 hctx 를 순회.
 * hctx->dispatch 리스트에 scheduler 우회 request 가 있으면 해당 hctx 도 run.
 */
void blk_mq_run_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx, *sq_hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	sq_hctx = NULL;	/* [한국어] 기본값 NULL = "선호 hctx 없음". 아래에서 single-queue 스케줄러일 때만 채워진다 */
	/* [한국어] blk_queue_sq_sched: IO scheduler 가 single-queue 방식이면 true */
	if (blk_queue_sq_sched(q))
		/* [한국어] 현재 CPU 의 기본 hctx 를 dispatch 대상으로 고정 */
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {	/* [한국어] 이 큐의 모든 하드웨어 큐를 순회 — NVMe 라면 I/O 큐 하나하나가 여기 해당한다 */
		if (blk_mq_hctx_stopped(hctx))	/* [한국어] 드라이버가 명시적으로 정지시킨 hctx 인가 */
			/* [한국어] BLK_MQ_S_STOPPED: reset/error 로 정지된 SQ 는 건너뜀 */
			continue;
		/*
		 * Dispatch from this hctx either if there's no hctx preferred
		 * by IO scheduler or if it has requests that bypass the
		 * scheduler.
		 */
		/* [한국어] sq_hctx 없거나, 이 hctx 가 sq_hctx 이거나,
		 * dispatch 리스트(scheduler 우회 request)가 있으면 run */
		if (!sq_hctx || sq_hctx == hctx ||
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
/*
 * [한국어]
 * blk_mq_delay_run_hw_queues - 모든 hw queue 에 지연 dispatch 예약
 *
 * @q:    대상 request_queue
 * @msecs: 지연 시간 (밀리초)
 *
 * blk_mq_run_hw_queues 의 지연 버전. 각 hctx 에 대해 blk_mq_delay_run_hw_queue 호출.
 * run_work 가 이미 pending 이면 지연 시간을 건드리지 않는다
 * (다른 hctx 가 이 hctx 의 work 지연을 덮어쓰면 stall 이 발생하기 때문).
 */
void blk_mq_delay_run_hw_queues(struct request_queue *q, unsigned long msecs)
{
	struct blk_mq_hw_ctx *hctx, *sq_hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	sq_hctx = NULL;	/* [한국어] 기본값 NULL = 선호 hctx 없음 */
	/* [한국어] single-queue scheduler 이면 기본 hctx 만 dispatch */
	if (blk_queue_sq_sched(q))
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {	/* [한국어] 모든 하드웨어 큐를 순회 */
		if (blk_mq_hctx_stopped(hctx))	/* [한국어] 정지된 hctx 는 지연 예약도 하지 않는다 */
			/* [한국어] STOPPED 상태이면 지연 예약 불가 */
			continue;
		/*
		 * If there is already a run_work pending, leave the
		 * pending delay untouched. Otherwise, a hctx can stall
		 * if another hctx is re-delaying the other's work
		 * before the work executes.
		 */
		/* [한국어] 이미 run_work 가 pending 이면 지연 시간을 덮어쓰지 않음.
		 * 다른 hctx 가 이 hctx 의 work 지연을 재설정하면 stall 이 발생 */
		if (delayed_work_pending(&hctx->run_work))
			continue;
		/*
		 * Dispatch from this hctx either if there's no hctx preferred
		 * by IO scheduler or if it has requests that bypass the
		 * scheduler.
		 */
		/* [한국어] sq_hctx 없거나 이 hctx 이거나 dispatch 리스트 있으면 예약 */
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
/*
 * [한국어]
 * blk_mq_stop_hw_queue - 하나의 hw queue (NVMe SQ) 를 정지
 *
 * @hctx: 정지할 hardware queue
 *
 * 드라이버가 BLK_STS_RESOURCE 를 반환하거나 error recovery 중에
 * 추가 dispatch 를 막기 위해 호출한다.
 * run_work 를 취소하고 BLK_MQ_S_STOPPED 를 설정한다.
 * drain 이나 quiesce 보장은 없음 — 완전한 중단이 필요하면 blk_mq_quiesce_queue 사용.
 */
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] 예약된 kblockd dispatch work 를 취소 — 새 dispatch 방지 */
	cancel_delayed_work(&hctx->run_work);

	/* [한국어] BLK_MQ_S_STOPPED: 이후 blk_mq_delay_run_hw_queue 가 이 hctx 를 건너뜀 */
	set_bit(BLK_MQ_S_STOPPED, &hctx->state);
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
/*
 * [한국어]
 * blk_mq_stop_hw_queues - 모든 hw queue 를 정지
 *
 * @q: 대상 request_queue
 *
 * NVMe reset/error 시 모든 SQ dispatch 를 일시 중단.
 * 각 hctx 에 blk_mq_stop_hw_queue 를 호출.
 */
void blk_mq_stop_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	/* [한국어] 모든 hw queue 를 순회하며 개별 stop */
	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_stop_hw_queue(hctx);
}
EXPORT_SYMBOL(blk_mq_stop_hw_queues);

/*
 * [한국어]
 * blk_mq_start_hw_queue - 정지된 hw queue (NVMe SQ) 를 재시작
 *
 * @hctx: 재시작할 hardware queue
 *
 * BLK_MQ_S_STOPPED 를 클리어하고 즉시 run 을 시작한다.
 * NVMe error recovery 완료 후 SQ 를 다시 활성화할 때 사용.
 */
void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] BLK_MQ_S_STOPPED 클리어: 이후 dispatch 예약이 가능해짐 */
	clear_bit(BLK_MQ_S_STOPPED, &hctx->state);

	/* [한국어] BLK_MQ_F_BLOCKING: poll 큐 등 sleep 허용 시 sync run 가능 */
	blk_mq_run_hw_queue(hctx, hctx->flags & BLK_MQ_F_BLOCKING);
}
EXPORT_SYMBOL(blk_mq_start_hw_queue);

/*
 * [한국어]
 * blk_mq_start_hw_queues - 모든 hw queue 를 재시작
 *
 * @q: 대상 request_queue
 *
 * NVMe 컨트롤러 reset 완료 후 모든 SQ 를 다시 활성화한다.
 */
void blk_mq_start_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	queue_for_each_hw_ctx(q, hctx, i)	/* [한국어] 이 큐의 모든 하드웨어 큐를 순회 */
		blk_mq_start_hw_queue(hctx);	/* [한국어] 각 hctx 의 BLK_MQ_S_STOPPED 를 내리고 즉시 재실행 — 정지 여부와 무관하게 무조건 시작한다 */
}
EXPORT_SYMBOL(blk_mq_start_hw_queues);

/*
 * [한국어]
 * blk_mq_start_stopped_hw_queue - STOPPED 상태의 hw queue 만 선택적으로 재시작
 *
 * @hctx:  재시작할 hardware queue
 * @async: true 이면 kblockd 비동기 run
 *
 * 이미 STOPPED 가 아닌 hctx 는 건드리지 않는다.
 * smp_mb__after_atomic: STOPPED 클리어와 dispatch list 검사 사이 메모리 순서 보장.
 * blk_mq_hctx_stopped 의 smp_mb 와 쌍을 이룬다.
 */
void blk_mq_start_stopped_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
	if (!blk_mq_hctx_stopped(hctx))
		/* [한국어] STOPPED 상태가 아니면 아무것도 안 함 */
		return;

	/* [한국어] BLK_MQ_S_STOPPED 클리어: 이후 dispatch 예약 허용 */
	clear_bit(BLK_MQ_S_STOPPED, &hctx->state);
	/*
	 * Pairs with the smp_mb() in blk_mq_hctx_stopped() to order the
	 * clearing of BLK_MQ_S_STOPPED above and the checking of dispatch
	 * list in the subsequent routine.
	 */
	/* [한국어] smp_mb__after_atomic: BIT_CLEAR 이후의 메모리 순서를 보장.
	 * blk_mq_run_hw_queue 가 dispatch list 를 읽기 전에 STOPPED 클리어가 보여야 함 */
	smp_mb__after_atomic();
	blk_mq_run_hw_queue(hctx, async);	/* [한국어] STOPPED 를 내린 직후 dispatch 를 다시 돌린다. 정지 동안 쌓인 request 가 여기서 나간다 */
}
EXPORT_SYMBOL_GPL(blk_mq_start_stopped_hw_queue);

/*
 * [한국어]
 * blk_mq_start_stopped_hw_queues - STOPPED 상태의 모든 hw queue 를 재시작
 *
 * @q:     대상 request_queue
 * @async: true 이면 kblockd 비동기 run (BLK_MQ_F_BLOCKING 이면 강제 async)
 *
 * NVMe reset 완료 후 stopped 된 모든 SQ 를 다시 시작한다.
 */
void blk_mq_start_stopped_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	queue_for_each_hw_ctx(q, hctx, i)	/* [한국어] 모든 하드웨어 큐를 순회하며 정지된 것만 골라 재시작 */
		/* [한국어] BLK_MQ_F_BLOCKING: poll 큐 등은 항상 async run */
		blk_mq_start_stopped_hw_queue(hctx, async ||
					(hctx->flags & BLK_MQ_F_BLOCKING));
}
EXPORT_SYMBOL(blk_mq_start_stopped_hw_queues);

/*
 * [한국어]
 * blk_mq_run_work_fn - kblockd workqueue 에서 실행되는 hctx dispatch work 핸들러
 *
 * @work: hctx->run_work (delayed_work 임베디드)
 *
 * blk_mq_delay_run_hw_queue 가 kblockd 에 예약한 dispatch work 의 실제 핸들러.
 * blk_mq_sched_dispatch_requests → blk_mq_dispatch_rq_list → nvme_queue_rq 로 이어진다.
 *
 * 호출 체인:
 *   kblockd workqueue → [blk_mq_run_work_fn]
 *   → blk_mq_sched_dispatch_requests → blk_mq_dispatch_rq_list → nvme_queue_rq
 */
static void blk_mq_run_work_fn(struct work_struct *work)
{
	/* [한국어] container_of: run_work 포인터에서 hctx 역산 */
	struct blk_mq_hw_ctx *hctx =
		container_of(work, struct blk_mq_hw_ctx, run_work.work);

	/* [한국어] blk_mq_run_dispatch_ops: dispatch ops srcu lock 하에서
	 * blk_mq_sched_dispatch_requests 를 실행 */
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
/*
 * [한국어]
 * blk_mq_request_bypass_insert - request 를 elevator 를 우회하여 hctx->dispatch 에 삽입
 *
 * @rq:    삽입할 request (passthrough, flush, requeue 등)
 * @flags: BLK_MQ_INSERT_AT_HEAD 이면 dispatch list 맨 앞에, 아니면 끝에
 *
 * IO scheduler 를 거치지 않고 hctx->dispatch 에 직접 추가한다.
 * passthrough request (NVMe admin), flush sequence, requeue 된 driver request 등에 사용.
 * hctx->lock 으로 dispatch list 접근을 보호.
 */
static void blk_mq_request_bypass_insert(struct request *rq, blk_insert_t flags)
{
	/* [한국어] rq->mq_hctx: 이 request 가 발행될 NVMe SQ 에 대응하는 hw queue */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	/* [한국어] hctx->lock: dispatch list 접근 보호 */
	spin_lock(&hctx->lock);
	if (flags & BLK_MQ_INSERT_AT_HEAD)	/* [한국어] 우선 처리해야 하는 요청인가(requeue 된 것, flush 등) */
		/* [한국어] 맨 앞 삽입: 다음 dispatch 에서 가장 먼저 처리됨 */
		list_add(&rq->queuelist, &hctx->dispatch);
	else	/* [한국어] 일반 bypass 삽입 */
		/* [한국어] 끝 삽입: FIFO 순서 유지 */
		list_add_tail(&rq->queuelist, &hctx->dispatch);
	spin_unlock(&hctx->lock);	/* [한국어] dispatch 리스트 삽입이 끝났으므로 hctx 락 해제 */
}

/*
 * [한국어]
 * blk_mq_insert_requests - plug 에서 꺼낸 request 들을 sw queue 또는 hctx 에 삽입
 *
 * @hctx:            대상 hardware queue
 * @ctx:             request 들이 속한 per-CPU sw queue
 * @list:            삽입할 request list (plug 에서 splice 된 것)
 * @run_queue_async: true 이면 blk_mq_run_hw_queue 를 async 로 호출
 *
 * blk_mq_flush_plug_list 에서 plug 를 비울 때 호출된다.
 * hctx 가 바쁘지 않으면 즉시 dispatch 를 시도하고, 그렇지 않으면 sw queue 에 삽입 후
 * run_hw_queue 를 호출한다.
 *
 * 호출 체인:
 *   blk_finish_plug / blk_mq_flush_plug_list → [blk_mq_insert_requests]
 *   → blk_mq_try_issue_list_directly 또는 ctx->rq_lists 삽입
 *   → blk_mq_run_hw_queue → blk_mq_dispatch_rq_list → nvme_queue_rq
 */
static void blk_mq_insert_requests(struct blk_mq_hw_ctx *hctx,
		struct blk_mq_ctx *ctx, struct list_head *list,
		bool run_queue_async)
{
	struct request *rq;
	/* [한국어] type: hctx 타입 — ctx->rq_lists 인덱스 (DEFAULT/READ/POLL) */
	enum hctx_type type = hctx->type;

	/*
	 * Try to issue requests directly if the hw queue isn't busy to save an
	 * extra enqueue & dequeue to the sw queue.
	 */
	/* [한국어] dispatch_busy == 0 이고 sync 경로이면 sw queue 를 건너뛰고 바로 발행.
	 * hctx 가 한가할 때 불필요한 sw queue → hctx flush 단계를 생략하는 fast path */
	if (!hctx->dispatch_busy && !run_queue_async) {
		blk_mq_run_dispatch_ops(hctx->queue,
			blk_mq_try_issue_list_directly(hctx, list));	/* [한국어] sw 큐를 거치지 않고 곧장 드라이버로 밀어 넣는다. 성공하면 리스트가 비워진다 */
		if (list_empty(list))	/* [한국어] 전부 발행되었는가 */
			/* [한국어] 전부 발행 성공: sw queue 삽입 없이 바로 완료 */
			goto out;
	}

	/*
	 * preemption doesn't flush plug list, so it's possible ctx->cpu is
	 * offline now
	 */
	/* [한국어] sw queue 삽입 전 각 request 를 순회하며 tracepoint 기록 및 REQ_NOWAIT 확인 */
	list_for_each_entry(rq, list, queuelist) {
		/* [한국어] ctx 일치 검증: plug 에서 꺼낸 request 는 모두 같은 ctx 에 속해야 함 */
		BUG_ON(rq->mq_ctx != ctx);
		trace_block_rq_insert(rq);	/* [한국어] blktrace/eBPF 가 "요청이 sw 큐에 들어갔다"를 관측하는 지점 */
		if (rq->cmd_flags & REQ_NOWAIT)	/* [한국어] 이 요청이 기다림을 허용하지 않는가(O_NONBLOCK / io_uring nowait) */
			/* [한국어] REQ_NOWAIT: 기다리지 않으므로 run_queue 는 async 로 */
			run_queue_async = true;
	}

	/* [한국어] ctx->lock: rq_lists 와 pending 비트 수정 보호 */
	spin_lock(&ctx->lock);
	/* [한국어] list 전체를 ctx->rq_lists[type] 끝에 append (sw queue 에 대기) */
	list_splice_tail_init(list, &ctx->rq_lists[type]);
	/* [한국어] hctx->ctx_map 에 이 sw queue 의 pending 비트 설정.
	 * 다음 blk_mq_flush_busy_ctxs 에서 이 sw queue 를 dispatch 대상으로 인식 */
	blk_mq_hctx_mark_pending(hctx, ctx);
	spin_unlock(&ctx->lock);	/* [한국어] sw 큐 리스트 삽입과 ctx_map 비트 설정이 끝났으므로 락 해제 */
out:
	/* [한국어] hw queue run: async 이면 kblockd, sync 이면 현재 CPU 에서 dispatch */
	blk_mq_run_hw_queue(hctx, run_queue_async);
}

/*
 * [한국어]
 * blk_mq_insert_request - request 를 dispatch list, elevator, 또는 sw queue 에 삽입
 *
 * @rq:    삽입할 request
 * @flags: BLK_MQ_INSERT_AT_HEAD, BLK_MQ_INSERT_FLUSH 등
 *
 * 세 가지 경로로 분기:
 * 1) passthrough (NVMe admin) → bypass_insert (hctx->dispatch 직접)
 * 2) flush request → bypass_insert (우선순위 삽입)
 * 3) 일반 FS request → elevator 또는 blk_mq_insert_requests (sw queue)
 * NVMe 관점: SQ 로 바로 발행하지 못할 때 임시 저장 위치 결정.
 */
static void blk_mq_insert_request(struct request *rq, blk_insert_t flags)
{
	struct request_queue *q = rq->q;
	struct blk_mq_ctx *ctx = rq->mq_ctx;	/* [한국어] 이 요청을 제출한 CPU 의 sw 큐 — 아래에서 rq_lists 에 넣을 때 쓴다 */
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;	/* [한국어] 이 요청이 향할 하드웨어 큐 — NVMe 라면 SQ/CQ 한 쌍 */

	if (blk_rq_is_passthrough(rq)) {	/* [한국어] passthrough(nvme-cli ioctl 등 커널이 직접 만든 커맨드)인가 */
		/* [한국어] passthrough (NVMe admin/vendor): elevator 를 항상 우회.
		 * device 가 FS request 를 BLK_STS_RESOURCE 로 거부하는 상황에서도
		 * admin 명령은 통과되어야 하므로 hctx->dispatch 에 직접 삽입 */
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
	} else if (req_op(rq) == REQ_OP_FLUSH) {	/* [한국어] FLUSH 요청인가 — 스케줄러에 맡기면 지연되므로 dispatch 앞머리로 보낸다 */
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
		/* [한국어] REQ_OP_FLUSH: PRE/POST_FLUSH request.
		 * hctx->dispatch 맨 앞에 삽입하여 일반 IO 보다 우선 발행.
		 * 위 영문 주석이 말하는 NCQ 는 SATA/AHCI 의 Native Command Queuing 이고,
		 * "flush 는 non-NCQ 명령이라 in-flight NCQ 명령이 있으면 큐잉이 실패한다"는
		 * 것도 AHCI 하드웨어의 제약이다. NVMe 에는 NCQ 라는 개념 자체가 없고,
		 * Flush(opcode 0x00)도 다른 I/O 명령과 동일하게 SQ 에 실리는 평범한 명령이라
		 * 이 하드웨어 제약을 받지 않는다.
		 * NVMe 에 여전히 유효한 부분은 뒤쪽 이유다 — hw queue 당 in-flight flush 를
		 * 하나로 제한하는 blk-flush 정책 덕분에, 앞에 넣어 빨리 내보낼수록 뒤따르는
		 * flush 들이 그 하나에 merge 될 기회가 늘어 컨트롤러로 가는 Flush 명령 수가
		 * 줄어든다(fsync 집약 워크로드에서 유효). */
		blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD);
	} else if (q->elevator) {	/* [한국어] I/O 스케줄러가 붙어 있는가(NVMe 기본값은 none 이라 대개 아래 else 로 간다) */
		/* [한국어] IO scheduler 가 있으면 elevator queue 에 삽입.
		 * elevator 가 나중에 dispatch 할 때 최적 순서로 꺼냄 */
		LIST_HEAD(list);

		/* [한국어] elevator 경로에서는 rq->tag 가 미할당 상태 (sched tag 만 사용) */
		WARN_ON_ONCE(rq->tag != BLK_MQ_NO_TAG);

		list_add(&rq->queuelist, &list);	/* [한국어] 단건이지만 elevator 의 insert_requests 는 리스트를 받으므로 임시 리스트에 담아 넘긴다 */
		/* [한국어] elevator->ops.insert_requests: scheduler 내부 자료구조에 삽입.
		 * mq-deadline: RB-tree, BFQ: B+ tree 등으로 IO 순서 결정 */
		q->elevator->type->ops.insert_requests(hctx, &list, flags);
	} else {
		/* [한국어] elevator 없는 직접 경로: sw queue (ctx->rq_lists) 에 삽입 */
		trace_block_rq_insert(rq);

		spin_lock(&ctx->lock);	/* [한국어] sw 큐의 rq_lists 와 ctx_map 비트를 함께 보호하는 락 */
		if (flags & BLK_MQ_INSERT_AT_HEAD)	/* [한국어] 앞머리 삽입 요구가 있는가(requeue 등) */
			/* [한국어] BLK_MQ_INSERT_AT_HEAD: sw queue 맨 앞에 삽입 */
			list_add(&rq->queuelist, &ctx->rq_lists[hctx->type]);
		else	/* [한국어] 일반 경로 */
			list_add_tail(&rq->queuelist,	/* [한국어] sw 큐 꼬리에 붙여 FIFO 순서를 유지한다 */
				      &ctx->rq_lists[hctx->type]);	/* [한국어] hctx->type(DEFAULT/READ/POLL)별로 리스트가 따로 있어, 같은 CPU 라도 큐 종류별로 분리 관리된다 */
		/* [한국어] hctx->ctx_map 에 pending 비트 설정 */
		blk_mq_hctx_mark_pending(hctx, ctx);
		spin_unlock(&ctx->lock);	/* [한국어] 삽입과 pending 비트 설정이 끝났으므로 sw 큐 락 해제 */
	}
}

/*
 * [한국어]
 * blk_mq_bio_to_request - bio 의 정보를 request 구조체에 복사
 *
 * @rq:      채울 request
 * @bio:     원본 bio
 * @nr_segs: bio 의 물리 segment 수 (PRP/SGL entry 계산 기반)
 *
 * submit_bio → blk_mq_submit_bio 경로에서 새 request 에 bio 정보를 설정한다.
 * sector/size → NVMe LBA/transfer length, nr_segs → PRP/SGL 리스트 크기.
 * blk_crypto: NVMe inline encryption keyslot 및 DUN 초기화.
 */
static void blk_mq_bio_to_request(struct request *rq, struct bio *bio,
		unsigned int nr_segs)
{
	int err;

	if (bio->bi_opf & REQ_RAHEAD)	/* [한국어] read-ahead bio 인가 — 투기적 읽기라 실패해도 치명적이지 않다 */
		/* [한국어] read-ahead: REQ_FAILFAST_MASK → 오류 시 재시도 없이 즉시 실패 */
		rq->cmd_flags |= REQ_FAILFAST_MASK;

	/* [한국어] rq->bio: bio chain 시작, rq->biotail: chain 끝 */
	rq->bio = rq->biotail = bio;
	/* [한국어] rq->__sector: NVMe LBA 시작 주소 (bi_sector) */
	rq->__sector = bio->bi_iter.bi_sector;
	/* [한국어] rq->__data_len: NVMe transfer length (바이트, bi_size) */
	rq->__data_len = bio->bi_iter.bi_size;
	/* [한국어] phys_gap_bit: bvec 간 물리 주소 gap 여부 (PRP 불연속 제약 관련) */
	rq->phys_gap_bit = bio->bi_bvec_gap_bit;

	/* [한국어] nr_phys_segments: NVMe PRP/SGL entry 수 결정 기반 */
	rq->nr_phys_segments = nr_segs;
	if (bio_integrity(bio))	/* [한국어] 이 bio 에 무결성(T10 PI / NVMe End-to-End Data Protection) 메타데이터가 붙어 있는가 */
		/* [한국어] NVMe PI(Protection Information): 별도 metadata scatter-gather 필요 */
		rq->nr_integrity_segments = blk_rq_count_integrity_sg(rq->q,
								      bio);

	/* This can't fail, since GFP_NOIO includes __GFP_DIRECT_RECLAIM. */
	/* [한국어] blk_crypto_rq_bio_prep: NVMe inline encryption keyslot + DUN 설정.
	 * GFP_NOIO 라 direct reclaim 가능 → 실패 없음 */
	err = blk_crypto_rq_bio_prep(rq, bio, GFP_NOIO);
	WARN_ON_ONCE(err);	/* [한국어] GFP_NOIO 에 __GFP_DIRECT_RECLAIM 이 포함되어 실패할 수 없다(위 영문 주석). 그래도 규약이 깨지면 한 번만 경고한다 */

	/* [한국어] IO 통계 기록 시작: iostat/blk-cgroup 계정, rq->start_time_ns */
	blk_account_io_start(rq);
}

/*
 * [한국어]
 * __blk_mq_issue_directly - 블록 계층이 드라이버를 호출하는 유일한 지점
 *
 * @hctx: 이 request를 담당하는 hardware context. NVMe에서는 nvme_queue 하나
 *        (즉 SQ/CQ 쌍 하나)와 1:1로 대응한다.
 * @rq:   발행할 request. 이미 driver tag(= NVMe Command ID)를 확보한 상태여야 한다.
 * @last: 이 request가 연속 발행 묶음의 마지막인가.
 *        false면 드라이버는 SQ에 엔트리만 써 두고 doorbell을 미룰 수 있다.
 *        true면 지금까지 쌓은 것을 포함해 doorbell을 반드시 쳐야 한다.
 * @return: BLK_STS_OK        = 드라이버가 요청을 받아 갔다(SQ에 실렸다)
 *          BLK_STS_RESOURCE  = 드라이버 측 자원 부족(재시도하면 될 수 있음)
 *          BLK_STS_DEV_RESOURCE = 장치 측 자원 부족(장치가 완료를 내야 풀림)
 *          그 외              = 이 요청은 실패로 끝내야 함
 *
 * === 이 함수가 blk-mq에서 갖는 위치 ===
 * 블록 계층의 모든 경로(직접 발행, plug 플러시, 스케줄러 dispatch, requeue 재시도)는
 * 결국 여기로 수렴해 q->mq_ops->queue_rq()를 호출한다. 그 함수 포인터가 NVMe에서는
 * nvme_queue_rq()이고, 그 안에서 다음이 일어난다:
 *   nvme_queue_rq(hctx, bd)                     [drivers/nvme/host/pci.c]
 *     nvmeq = hctx->driver_data            : init_hctx 때 심어 둔 nvme_queue
 *     → NVMEQ_ENABLED 미설정이면 BLK_STS_IOERR (죽어 가는 큐 드레인용)
 *     → nvme_check_ready()                 : 컨트롤러 상태가 LIVE 가 아니면
 *                                            nvme_fail_nonready_command() 로 처리
 *     → nvme_prep_rq(req)
 *         → nvme_setup_cmd()   : request → 64B SQE (opcode, NSID, SLBA, NLB,
 *                                그리고 command_id = nvme_cid(req) = rq->tag)
 *         → nvme_map_data()    : bvec → PRP 리스트 또는 SGL 디스크립터
 *         → nvme_map_metadata(): PI 메타데이터가 있으면 MPTR / meta SGL
 *         → nvme_start_request(): blk_mq_start_request() 를 감싼 것 —
 *                                여기서 타임아웃 타이머가 시작되고 rq 상태가
 *                                MQ_RQ_IN_FLIGHT 가 된다
 *     → spin_lock(&nvmeq->sq_lock)          : SQ tail 갱신 + doorbell 직렬화
 *     → nvme_sq_copy_cmd()     : SQ 링의 sq_tail 슬롯에 64B SQE memcpy, tail++
 *     → nvme_write_sq_db(nvmeq, bd->last)   : ★ 여기서 @last 가 쓰인다
 *     → spin_unlock(&nvmeq->sq_lock)
 *     → return BLK_STS_OK
 * 즉 이 한 줄의 함수 포인터 호출 이후로는 요청이 하드웨어의 손에 넘어간다.
 *
 * === @last와 doorbell batching ===
 * doorbell 쓰기는 MMIO라 비용이 크고(PCIe 트랜잭션), 컨트롤러 입장에서도 매번
 * 알림을 받는 것보다 여러 개를 모아 한 번에 받는 편이 효율적이다. 그래서 blk-mq는
 * 연속으로 발행할 request가 남아 있으면 last=false를 넘겨 드라이버가 doorbell을
 * 아끼게 하고, 마지막 하나에만 last=true를 넘긴다. NVMe PCIe 드라이버는
 * bd->last를 그대로 nvme_write_sq_db(nvmeq, bd->last) 에 넘긴다.
 *
 * nvme_write_sq_db(nvmeq, write_sq) 의 실제 동작(pci.c 실물 확인):
 *   write_sq == false 이면 "다음에 한 칸 더 써도 last_sq_tail 에 닿지 않는가"를
 *   보고, 여유가 있으면 **MMIO 를 생략하고 그냥 돌아간다**. 즉 last=false 는
 *   "울리지 마라"가 아니라 "링을 덮어쓸 위험이 없는 한 미뤄도 좋다"는 뜻이다.
 *   write_sq == true 이면 무조건 dbbuf 검사 후 writel(sq_tail, q_db) 로
 *   SQ tail doorbell 레지스터에 쓰고 last_sq_tail 을 갱신한다.
 * 미뤄 둔 doorbell 을 반드시 울리게 만드는 두 장치가 있다:
 *   (1) 마지막 request 에 last=true 를 주는 것 (이 함수의 @last)
 *   (2) 배치 중간에 hctx 가 바뀌는 등 last=true 를 줄 기회가 없었을 때
 *       blk_mq_commit_rqs() → mq_ops->commit_rqs == nvme_commit_rqs 가
 *       sq_tail != last_sq_tail 이면 nvme_write_sq_db(nvmeq, true) 로 강제 플러시
 *
 * === dispatch_busy EWMA ===
 * 반환값에 따라 hctx->dispatch_busy(지수 가중 이동 평균)를 갱신한다. 이 값은
 * "이 하드웨어 큐가 최근에 얼마나 자주 바빴는가"를 나타내며, blk_mq_submit_bio()가
 * "직접 발행할까, 소프트웨어 큐에 넣고 나중에 일괄 처리할까"를 고르는 근거가 된다.
 * 바쁜 큐에 직접 발행을 시도해 봐야 실패하고 되돌리는 비용만 들기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(직접 발행) 또는 kblockd 워커(지연 dispatch).
 * blk_mq_run_dispatch_ops() 안에서 호출되므로 SRCU 읽기 구간 또는 RCU 보호 하에
 * 있으며, 그 덕분에 발행 도중 큐가 사라지지 않음이 보장된다.
 *
 * 에러 경로: RESOURCE 계열이면 __blk_mq_requeue_request()로 request의 상태를
 * "발행 전"으로 되돌린다(start_time 등을 원복). 그 외 오류는 호출자가
 * blk_mq_end_request()로 요청을 실패 종료시킨다.
 *
 * 호출 체인:
 *   blk_mq_try_issue_directly / blk_mq_issue_direct / blk_mq_dispatch_rq_list
 *     → [__blk_mq_issue_directly]
 *       → q->mq_ops->queue_rq (NVMe PCIe: nvme_queue_rq)
 *         → nvme_prep_rq(nvme_setup_cmd + nvme_map_data)
 *         → nvme_sq_copy_cmd → nvme_write_sq_db(bd->last)
 *
 * 주의: 이 파일의 코드는 nvme_* 심볼을 하나도 직접 부르지 않는다. 위 체인은
 * 전부 q->mq_ops 함수 포인터를 통한 간접 호출이며, mq_ops 는 드라이버가
 * blk_mq_alloc_tag_set() 때 set->ops 로 등록한 테이블이다.
 */
static blk_status_t __blk_mq_issue_directly(struct blk_mq_hw_ctx *hctx,
					    struct request *rq, bool last)
{
	/* [한국어] mq_ops 테이블을 얻기 위해 큐 포인터를 꺼낸다. NVMe PCIe라면
	 * q->mq_ops는 drivers/nvme/host/pci.c의 nvme_mq_ops다. */
	struct request_queue *q = rq->q;
	/* [한국어] 드라이버에 넘길 dispatch 컨텍스트를 스택에 만든다. 구조체 하나로
	 * 감싸는 이유는 향후 필드가 늘어나도 콜백 시그니처를 바꾸지 않기 위해서다.
	 *   .rq   - 발행할 request. nvme_queue_rq()가 blk_mq_rq_to_pdu(rq)로
	 *           드라이버 전용 영역(struct nvme_iod)에 접근한다.
	 *   .last - doorbell을 지금 쳐야 하는지 여부(위 설명 참고). */
	struct blk_mq_queue_data bd = {
		.rq = rq,
		.last = last,	/* [한국어] 이 request 가 묶음의 마지막인지 그대로 전달. NVMe 라면 nvme_write_sq_db(nvmeq, bd->last) 로 doorbell 을 지금 칠지 미룰지가 결정된다 */
	};
	/* [한국어] 드라이버가 돌려준 상태 코드를 담을 변수. */
	blk_status_t ret;

	/*
	 * For OK queue, we are done. For error, caller may kill it.
	 * Any other error (busy), just add it to our list as we
	 * previously would have done.
	 */
	/* [한국어] ★ 블록 계층에서 드라이버로 제어가 넘어가는 지점 ★
	 * NVMe PCIe라면 nvme_queue_rq()가 실행되어 request를 64바이트 SQE로 변환하고
	 * SQ 링에 기록한다. 이 호출이 반환되는 시점에 (성공이라면) 커맨드는 이미
	 * 컨트롤러가 가져갈 수 있는 상태이며, 완료는 나중에 CQ 인터럽트로 통지된다.
	 * 동기적으로 데이터가 전송되는 것이 아니라는 점이 중요하다. */
	ret = q->mq_ops->queue_rq(hctx, &bd);
	switch (ret) {	/* [한국어] 드라이버가 돌려준 상태에 따라 EWMA 갱신과 후속 처리를 나눈다 */
	case BLK_STS_OK:	/* [한국어] 드라이버가 받아 갔다 */
		/* [한국어] 발행 성공. 이 큐는 여유가 있었다는 뜻이므로 dispatch_busy
		 * EWMA를 낮춘다. 값이 낮게 유지되면 이후 요청들도 소프트웨어 큐를
		 * 거치지 않고 직접 발행되어 지연이 줄어든다. */
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	case BLK_STS_RESOURCE:	/* [한국어] 호스트 측 자원 부족 */
	case BLK_STS_DEV_RESOURCE:	/* [한국어] 장치 측 자원 부족 — 아래 처리는 둘이 동일하다 */
		/* [한국어] 자원 부족으로 받아 가지 못했다. 두 코드의 차이:
		 *   BLK_STS_RESOURCE     - 호스트 측 자원 부족(예: DMA 매핑용 메모리
		 *     할당 실패). 잠시 후 재시도하면 성공할 수 있으므로 blk-mq가
		 *     타이머를 걸어 큐를 다시 돌린다.
		 *   BLK_STS_DEV_RESOURCE - 장치 측 자원 부족(예: SCSI 의 QUEUE FULL).
		 *     이미 제출된 커맨드가 완료되어야 자리가 생긴다는 보장이 있으므로,
		 *     타이머 대신 완료 시점에 큐가 다시 돌기를 기다린다(불필요한 폴링 방지).
		 *     NVMe PCIe 에서는 이 코드를 쓰지 않는다 — 드라이버 태그 수가 곧
		 *     SQ 깊이라 "SQ 가 꽉 차서" 거절되는 일이 구조적으로 없다.
		 * 어느 쪽이든 이 큐는 바빴으므로 EWMA를 올려, 이후 요청은 직접 발행
		 * 대신 소프트웨어 큐 경유 경로를 택하게 유도한다. */
		blk_mq_update_dispatch_busy(hctx, true);
		/* [한국어] request를 "아직 발행되지 않은" 상태로 되돌린다. 구체적으로는
		 * 타임아웃 추적에서 빼고 통계 시작 시각을 원복해, 나중에 재발행될 때
		 * 시간이 이중으로 계산되지 않게 한다. driver tag(NVMe CID)는 이 시점에
		 * 반납되지 않고 유지되어, 재시도 시 태그 재획득 비용을 아낀다. */
		__blk_mq_requeue_request(rq);
		break;
	default:
		/* [한국어] 그 외의 오류(BLK_STS_IOERR, BLK_STS_NOTSUPP 등)는 재시도해도
		 * 소용없는 실패다. 큐가 바빠서가 아니라 요청 자체가 문제이므로 EWMA는
		 * 낮춘다(이 큐를 바쁘다고 오판하면 안 된다). 호출자가 이 반환값을 보고
		 * blk_mq_end_request()로 요청을 에러 종료시킨다. */
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	}

	/* [한국어] 드라이버의 판정을 그대로 호출자에게 전달한다. 호출자는 OK면
	 * 다음 request로 넘어가고, RESOURCE면 발행을 중단하고 나중에 재시도하며,
	 * 그 외에는 이 request를 실패 종료시킨다. */
	return ret;
}

/*
 * [한국어]
 * blk_mq_get_budget_and_tag - dispatch budget 과 driver tag(NVMe CID) 를 동시 확보
 *
 * @rq:    확보할 request
 * @return: 두 가지 모두 성공하면 true
 *
 * blk_mq_try_issue_directly 등 direct dispatch 경로에서 호출.
 * budget 확보 후 tag 실패 시 budget 도 반납 (원자적 확보/반납).
 */
static bool blk_mq_get_budget_and_tag(struct request *rq)
{
	int budget_token;

	/* [한국어] blk_mq_get_dispatch_budget: queue depth throttle token 획득.
	 * 음수 반환 시 queue 가 depth limit 에 도달 → dispatch 불가 */
	budget_token = blk_mq_get_dispatch_budget(rq->q);
	if (budget_token < 0)	/* [한국어] 예산 획득 실패 — 장치가 더 받을 수 없다 */
		return false;
	/* [한국어] budget_token 을 request 에 저장 — 완료 시 put_budget 에서 반납 */
	blk_mq_set_rq_budget_token(rq, budget_token);
	if (!blk_mq_get_driver_tag(rq)) {	/* [한국어] 예산은 얻었지만 드라이버 태그를 못 얻은 경우 */
		/* [한국어] NVMe CID 확보 실패: 이미 획득한 budget 도 반납 */
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
 * [한국어]
 * blk_mq_try_issue_directly - plug 없이 request 를 즉시 driver 로 발행 시도
 *
 * @hctx: 발행할 hardware queue
 * @rq:   발행할 request
 *
 * blk_mq_submit_bio 에서 plug 없이 직접 dispatch 할 때 호출된다.
 * 자원(budget + CID)이 있으면 nvme_queue_rq → SQ doorbell 로 즉시 발행.
 * stopped/quiesced 이거나 자원 부족이면 insert_request + run_hw_queue.
 * RESOURCE 오류 시 hctx->dispatch 맨 끝에 삽입하여 우선 재시도.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → [blk_mq_try_issue_directly]
 *   → __blk_mq_issue_directly → mq_ops->queue_rq → nvme_queue_rq
 */
static void blk_mq_try_issue_directly(struct blk_mq_hw_ctx *hctx,
		struct request *rq)
{
	blk_status_t ret;

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {	/* [한국어] 드라이버가 정지시켰거나 큐가 quiesce 중인가 — 어느 쪽이든 지금 발행하면 안 된다 */
		/* [한국어] STOPPED 또는 quiesced: 즉시 발행 불가.
		 * insert 후 run_hw_queue 로 재시도 예약 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);	/* [한국어] 큐가 다시 열릴 때 처리되도록 비동기 실행을 예약해 둔다 */
		return;
	}

	if ((rq->rq_flags & RQF_USE_SCHED) || !blk_mq_get_budget_and_tag(rq)) {	/* [한국어] 스케줄러를 써야 하는 요청이거나, 예산/드라이버 태그를 확보하지 못한 경우 — 직접 발행을 포기한다 */
		/* [한국어] RQF_USE_SCHED: elevator 를 사용하는 request — 직접 발행 불가.
		 * 또는 budget/CID 확보 실패: sw queue/scheduler 로 위임 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, rq->cmd_flags & REQ_NOWAIT);	/* [한국어] NOWAIT 요청이면 여기서 기다릴 수 없으므로 비동기로 돌린다 */
		return;
	}

	/* [한국어] last=true: 이 request 가 이 batch 의 마지막 → doorbell 기록 */
	ret = __blk_mq_issue_directly(hctx, rq, true);
	switch (ret) {	/* [한국어] 직접 발행 결과에 따라 분기 */
	case BLK_STS_OK:	/* [한국어] 성공 — 완료는 CQ 인터럽트(또는 폴링)로 돌아온다 */
		/* [한국어] 발행 성공: 완료를 CQ 완료 인터럽트에서 기다림 */
		break;
	case BLK_STS_RESOURCE:	/* [한국어] 호스트 측 자원 부족 */
	case BLK_STS_DEV_RESOURCE:	/* [한국어] 장치 측 자원 부족 — 아래 처리는 동일하다 */
		/* [한국어] 자원 부족: hctx->dispatch 에 삽입 후 async run 예약.
		 * dispatch list 는 sw queue 보다 우선 처리됨 */
		blk_mq_request_bypass_insert(rq, 0);
		blk_mq_run_hw_queue(hctx, false);	/* [한국어] dispatch 리스트에 되돌려 놓았으므로 비동기 재실행을 예약한다 */
		break;
	default:
		/* [한국어] 기타 오류: 즉시 request 완료 (error 상위 전파) */
		blk_mq_end_request(rq, ret);
		break;
	}
}

/*
 * [한국어]
 * blk_mq_request_issue_directly - 하나의 request 를 직접 발행 (stopped 시 insert fallback)
 *
 * @rq:   발행할 request
 * @last: true 이면 batch 의 마지막 → doorbell 기록
 * @return: BLK_STS_OK 또는 오류 코드
 *
 * blk_mq_issue_direct 에서 request 하나마다 호출.
 * stopped/quiesced 이면 insert 후 run 예약하고 BLK_STS_OK 반환.
 */
static blk_status_t blk_mq_request_issue_directly(struct request *rq, bool last)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {	/* [한국어] 정지되었거나 quiesce 중이면 직접 발행 불가 */
		/* [한국어] 즉시 발행 불가: insert 후 async run 예약 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);	/* [한국어] 큐가 다시 열릴 때 나가도록 비동기 실행 예약 */
		return BLK_STS_OK;	/* [한국어] 호출자에게는 성공으로 보고한다 — 요청을 잃지 않고 큐에 넣어 두었기 때문이다 */
	}

	if (!blk_mq_get_budget_and_tag(rq))	/* [한국어] 예산과 드라이버 태그를 확보하지 못하면 */
		/* [한국어] budget/CID 확보 실패: 호출자가 재시도 처리 */
		return BLK_STS_RESOURCE;
	return __blk_mq_issue_directly(hctx, rq, last);	/* [한국어] 자원을 모두 확보했으므로 드라이버 queue_rq 로 직행한다 */
}

/*
 * [한국어]
 * blk_mq_issue_direct - plug에 쌓인 request들을 하나씩 드라이버로 발행하는 루프
 *
 * @rqs: 발행할 request들의 리스트(plug->mq_list에서 넘어온다). 이 함수가
 *       소비하면서 비운다.
 * @return: 없음. 발행하지 못한 request는 hctx->dispatch로 옮겨져 나중에 처리된다.
 *
 * === 이 함수가 존재하는 이유: doorbell 절약 ===
 * plug는 한 스레드가 연달아 발행한 request를 모아 두는 장치다. 그것들을 한꺼번에
 * 내보낼 때 매번 doorbell을 치면 MMIO 비용이 request 수만큼 든다. 그래서 이
 * 루프는 마지막 request에만 last=true를 넘겨, 드라이버가 SQ 엔트리는 계속 쌓되
 * doorbell은 한 번만 치도록 유도한다. NVMe에서 이 최적화의 효과는 크다 —
 * 4KiB 랜덤 읽기 같은 워크로드에서 doorbell MMIO가 제출 비용의 상당 부분을
 * 차지하기 때문이다.
 *
 * === hctx 전환 처리가 필요한 이유 ===
 * plug 리스트에는 여러 하드웨어 큐(= 여러 NVMe SQ)로 갈 request가 섞여 있을 수
 * 있다. 예를 들어 스레드가 CPU를 옮겨 다니며 I/O를 발행했거나, 여러 장치에
 * 동시에 쓰는 경우다. 하드웨어 큐가 바뀌는 순간 "이전 큐에 쌓아 둔 것"을 확정해야
 * 하므로 blk_mq_commit_rqs()로 이전 hctx의 doorbell을 치고 넘어간다. 이 처리를
 * 빠뜨리면 이전 SQ에 쓰인 엔트리를 컨트롤러가 영영 가져가지 않아 I/O가 멈춘다.
 *
 * === queue_rqs와의 관계 ===
 * 드라이버가 queue_rqs 콜백(여러 request를 한 번에 받는 인터페이스)을 지원하면
 * 호출자 blk_mq_flush_plug_list()가 __blk_mq_flush_list()를 대신 쓴다.
 * 이 함수는 queue_rqs가 없거나 쓸 수 없는 상황의 경로다. NVMe PCIe 드라이버는
 * nvme_queue_rqs를 구현하고 있어 보통은 그쪽이 쓰이지만, 스케줄러가 붙어 있거나
 * 큐가 섞이면 이 경로로 온다.
 *
 * 실행 컨텍스트: blk_finish_plug() 경로의 프로세스 컨텍스트, 또는 스케줄 아웃 시
 * blk_flush_plug()가 호출되는 컨텍스트. blk_mq_run_dispatch_ops() 보호 하에 실행.
 *
 * 에러 경로: RESOURCE면 해당 request를 hctx->dispatch로 옮기고 루프를 중단한다
 * (뒤의 것들도 어차피 실패할 가능성이 높으므로). 그 외 오류는 그 request만
 * 실패 종료시키고 루프를 계속한다.
 *
 * 호출 체인:
 *   blk_finish_plug → blk_mq_flush_plug_list → [blk_mq_issue_direct]
 *     → blk_mq_request_issue_directly → __blk_mq_issue_directly
 *       → nvme_queue_rq → SQ 엔트리 기록
 *     → blk_mq_commit_rqs → nvme_commit_rqs → SQ doorbell
 */
static void blk_mq_issue_direct(struct rq_list *rqs)
{
	/* [한국어] 직전에 처리한 request의 hardware context. NULL로 시작해 첫 반복에서
	 * 반드시 갱신되게 한다. 이 값이 바뀌는 순간이 곧 "SQ가 바뀌는 순간"이다. */
	struct blk_mq_hw_ctx *hctx = NULL;
	struct request *rq;	/* [한국어] 루프에서 하나씩 꺼낼 request 커서 */
	/* [한국어] 현재 hctx에 성공적으로 발행한 request 수. commit_rqs를 부를지
	 * 판단하는 데 쓰인다(0이면 쌓인 것이 없으므로 doorbell도 불필요). */
	int queued = 0;
	/* [한국어] 마지막 발행 결과. 루프를 정상 완주하면 BLK_STS_OK로 남아 아래
	 * out 라벨에서 중복 commit을 건너뛰게 한다. */
	blk_status_t ret = BLK_STS_OK;

	/* [한국어] 리스트에서 하나씩 꺼내며 소비한다. rq_list_pop()은 head를 떼어
	 * 반환하므로, 루프가 끝나면 rqs는 비거나(정상) 남은 것이 있다(중단). */
	while ((rq = rq_list_pop(rqs))) {
		/* [한국어] pop 이후에 리스트가 비었다면 이것이 마지막 request다.
		 * last=true가 드라이버에게 "이제 doorbell을 쳐라"고 알린다. pop을 먼저
		 * 하고 나서 비었는지 확인하는 순서가 중요하다. */
		bool last = rq_list_empty(rqs);

		/* [한국어] 하드웨어 큐가 바뀌었는지 확인한다. */
		if (hctx != rq->mq_hctx) {
			/* [한국어] 첫 반복이 아니라면(hctx != NULL) 이전 큐에 쌓아 둔
			 * 엔트리들을 확정해야 한다. commit_rqs가 NVMe에서는
			 * nvme_commit_rqs → nvme_write_sq_db로 이어져 doorbell을 친다.
			 * 이걸 빠뜨리면 이전 SQ의 엔트리를 컨트롤러가 인지하지 못해
			 * 해당 I/O들이 영원히 완료되지 않는다. */
			if (hctx) {
				blk_mq_commit_rqs(hctx, queued, false);
				/* [한국어] 새 큐를 위해 카운터를 초기화한다. */
				queued = 0;
			}
			/* [한국어] 현재 큐를 추적 대상으로 갱신. */
			hctx = rq->mq_hctx;
		}

		/* [한국어] budget과 driver tag(NVMe CID)를 확보한 뒤 실제 발행한다.
		 * 큐가 stopped/quiesced면 이 함수가 내부적으로 insert로 우회한다. */
		ret = blk_mq_request_issue_directly(rq, last);
		switch (ret) {	/* [한국어] 직접 발행 결과에 따라 분기 */
		case BLK_STS_OK:	/* [한국어] 드라이버가 받아 갔다 */
			/* [한국어] SQ에 실렸다. 아직 doorbell은 안 쳤을 수 있으므로
			 * 카운터만 올리고 계속 진행한다. */
			queued++;
			break;
		case BLK_STS_RESOURCE:	/* [한국어] 호스트 측 자원 부족 */
		case BLK_STS_DEV_RESOURCE:	/* [한국어] 장치 측 자원 부족 — 아래 처리는 동일하다 */
			/* [한국어] 자원 부족. 이 request를 hctx->dispatch 리스트로 옮긴다.
			 * bypass_insert는 스케줄러를 우회해 dispatch 리스트 앞쪽에 넣는데,
			 * 이 리스트는 소프트웨어 큐보다 먼저 처리되므로 "이미 한 번 시도해
			 * 밀린 요청"이 우선권을 갖는다(기아 방지). */
			blk_mq_request_bypass_insert(rq, 0);
			/* [한국어] 비동기로 큐를 다시 돌리도록 예약한다(두 번째 인자 false =
			 * 지금 당장 실행하지 말고 워커에 맡겨라). 지금 자원이 없는데 즉시
			 * 재시도해 봐야 다시 실패하므로 잠시 뒤로 미룬다. */
			blk_mq_run_hw_queue(hctx, false);
			/* [한국어] 루프를 중단한다. 자원이 없는 상태에서 남은 request를
			 * 계속 시도해도 실패할 확률이 높고, 실패마다 되돌리는 비용이 든다.
			 * 남은 것들은 rqs에 그대로 남아 호출자가 처리한다. */
			goto out;
		default:
			/* [한국어] 재시도 불가능한 오류. 이 request만 실패 종료시키고
			 * 다음 request로 넘어간다 — 하나가 잘못됐다고 나머지까지 막을
			 * 이유는 없다. blk_mq_end_request()가 bio들에 에러를 전파하고
			 * 태그(CID)를 반납한다. */
			blk_mq_end_request(rq, ret);
			break;
		}
	}

out:
	/* [한국어] 루프가 비정상 종료(ret != OK)했다면 마지막 hctx에 쌓아 둔 엔트리가
	 * doorbell 없이 남아 있을 수 있으므로 여기서 확정한다.
	 * 정상 완주했다면 마지막 request가 last=true로 발행되어 드라이버가 이미
	 * doorbell을 쳤으므로 중복 호출을 피한다. */
	if (ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false);
}

/*
 * [한국어]
 * __blk_mq_flush_list - rq_list 를 mq_ops->queue_rqs 로 batch 발행
 *
 * @q:   대상 request_queue
 * @rqs: 발행할 request list
 *
 * 드라이버가 queue_rqs 를 지원하면 여러 request 를 한 번에 처리한다.
 * NVMe 드라이버는 queue_rqs 에서 SQ entry 를 일괄 기록하고 doorbell 한 번만 기록.
 * quiesced 상태이면 아무것도 하지 않는다.
 */
static void __blk_mq_flush_list(struct request_queue *q, struct rq_list *rqs)
{
	if (blk_queue_quiesced(q))
		/* [한국어] quiesced: IO admission 차단 상태 — 발행 금지 */
		return;
	/* [한국어] mq_ops->queue_rqs: 드라이버에 request list 를 일괄 전달.
	 * NVMe: SQ entry 를 batch 기록 후 doorbell 한 번 */
	q->mq_ops->queue_rqs(rqs);
}

/*
 * [한국어]
 * blk_mq_extract_queue_requests - rq_list 에서 동일 request_queue 의 request 만 추출
 *
 * @rqs:       전체 request list (여러 queue 가 섞여있을 수 있음)
 * @queue_rqs: 추출된 동일 queue request 들을 담을 출력 list
 * @return:    추출한 request 수 (depth)
 *
 * plug 에 여러 queue (NVMe namespace, md device 등) 의 request 가 섞여있을 때
 * 첫 번째 request 의 queue 를 기준으로 같은 queue 의 request 들만 뽑아낸다.
 * 나머지는 rqs 에 그대로 남아 다음 호출에서 처리된다.
 */
static unsigned blk_mq_extract_queue_requests(struct rq_list *rqs,
					      struct rq_list *queue_rqs)
{
	/* [한국어] 첫 번째 request 를 꺼내 기준 queue 결정 */
	struct request *rq = rq_list_pop(rqs);
	struct request_queue *this_q = rq->q;	/* [한국어] 기준이 될 request_queue. 이 값과 같은 큐의 request 만 골라낸다 */
	struct request **prev = &rqs->head;	/* [한국어] 리스트에서 노드를 떼어낼 때 앞 노드의 next 포인터를 직접 고쳐야 하므로 포인터의 포인터로 순회한다(head 도 특수 처리 없이 다룰 수 있다) */
	struct rq_list matched_rqs = {};	/* [한국어] 같은 큐로 판정된 request 들을 모을 임시 리스트 */
	struct request *last = NULL;	/* [한국어] 남는(다른 큐) request 중 마지막 것 — rqs->tail 을 다시 세우는 데 필요하다 */
	/* [한국어] depth: 이 queue 에서 추출한 request 수 */
	unsigned depth = 1;

	rq_list_add_tail(&matched_rqs, rq);	/* [한국어] 기준이 된 첫 request 를 결과 리스트의 첫 원소로 넣는다 */
	while ((rq = *prev)) {	/* [한국어] 남은 리스트를 끝까지 순회하며 같은 큐인지 판정한다 */
		if (rq->q == this_q) {	/* [한국어] 기준 큐와 같은가 — NVMe 라면 같은 네임스페이스인가를 묻는 것이다 */
			/* [한국어] 같은 queue: matched_rqs 로 이동 */
			*prev = rq->rq_next;
			rq_list_add_tail(&matched_rqs, rq);
			depth++;	/* [한국어] 추출 개수 누적 — 호출자가 이 값으로 plug 깊이를 계산한다 */
		} else {
			/* leave rq in rqs */
			/* [한국어] 다른 queue: rqs 에 그대로 둠 */
			prev = &rq->rq_next;
			last = rq;	/* [한국어] 남겨 두는 쪽의 마지막 노드를 기억 — 순회가 끝나면 이것이 rqs 의 새 tail 이 된다 */
		}
	}

	/* [한국어] rqs->tail 갱신: 마지막으로 남은 request 포인터 */
	rqs->tail = last;
	*queue_rqs = matched_rqs;
	return depth;	/* [한국어] 이번에 뽑아낸 개수를 반환. 호출자는 이 값을 blk_mq_dispatch_queue_requests 에 넘긴다 */
}

/*
 * [한국어]
 * blk_mq_dispatch_queue_requests - 동일 queue 의 request list 를 batch dispatch
 *
 * @rqs:   dispatch 할 request list (모두 동일 request_queue)
 * @depth: list 의 request 수 (tracepoint 용)
 *
 * 드라이버가 queue_rqs 를 제공하면 __blk_mq_flush_list 로 일괄 처리.
 * 제공하지 않거나 남은 request 가 있으면 blk_mq_issue_direct 로 개별 발행.
 */
static void blk_mq_dispatch_queue_requests(struct rq_list *rqs, unsigned depth)
{
	struct request_queue *q = rq_list_peek(rqs)->q;

	/* [한국어] block_unplug tracepoint: blktrace/eBPF 가 unplug 이벤트 추적.
	 * true: 동기 unplug (from_schedule=false) */
	trace_block_unplug(q, depth, true);

	/*
	 * Peek first request and see if we have a ->queue_rqs() hook.
	 * If we do, we can dispatch the whole list in one go.
	 * We already know at this point that all requests belong to the
	 * same queue, caller must ensure that's the case.
	 */
	if (q->mq_ops->queue_rqs) {	/* [한국어] 드라이버가 배치 제출 콜백을 등록했는가. NVMe PCIe 는 nvme_queue_rqs 를 등록하므로 여기로 온다 */
		/* [한국어] queue_rqs hook 있음: 드라이버에 list 를 한 번에 전달.
		 * NVMe: SQ entry 를 일괄 기록 후 doorbell 한 번 → CPU 효율적 */
		blk_mq_run_dispatch_ops(q, __blk_mq_flush_list(q, rqs));
		if (rq_list_empty(rqs))	/* [한국어] 드라이버가 리스트를 전부 소비했는가 — 남은 것은 아래 개별 발행 경로가 처리한다 */
			/* [한국어] 모두 처리됨: 종료 */
			return;
	}

	/* [한국어] queue_rqs 없거나 일부 남음: blk_mq_issue_direct 로 개별 발행 */
	blk_mq_run_dispatch_ops(q, blk_mq_issue_direct(rqs));
}

/*
 * [한국어]
 * blk_mq_dispatch_list - 섞여 있는 request들을 동질 그룹으로 갈라 큐에 삽입
 *
 * @rqs:        [in,out] 처리할 request 리스트. 이번에 처리하지 못한 것들을
 *              그대로 되돌려 넣으므로, 호출자는 리스트가 빌 때까지 반복 호출한다.
 * @from_sched: kblockd 워커에서 호출되었는가. 큐를 비동기로 돌릴지, tracepoint를
 *              동기 unplug로 기록할지에 영향을 준다.
 * @return: 없음
 *
 * === 왜 "그룹으로 가르는" 작업이 필요한가 ===
 * plug 리스트에는 여러 하드웨어 큐(hctx), 여러 소프트웨어 큐(ctx), 그리고 일반
 * I/O와 passthrough가 뒤섞일 수 있다. 그런데 삽입 대상 자료구조는 그 조합마다
 * 다르고 각각 다른 락으로 보호된다. 따라서 섞인 채로 처리하면 request 하나마다
 * 락을 잡았다 풀어야 한다.
 * 이 함수는 리스트를 한 번 훑어 "첫 request와 같은 (hctx, ctx, passthrough 여부)"
 * 를 가진 것들만 뽑아내고, 나머지는 requeue_list에 모아 되돌려준다. 그 결과
 * 동질 묶음 하나를 락 한 번으로 통째로 삽입할 수 있다.
 * 호출자(blk_mq_flush_plug_list)가 rqs가 빌 때까지 이 함수를 반복 호출하므로,
 * 결과적으로 리스트가 그룹 수만큼의 라운드로 나뉘어 처리된다.
 *
 * === 세 갈래 삽입 경로 ===
 * 1) passthrough  → hctx->dispatch 에 직접(스케줄러 우회)
 *    NVMe에서는 nvme-cli의 admin/IO passthrough 커맨드가 여기 해당한다.
 *    스케줄러의 정렬·병합·지연 정책을 적용하면 사용자가 의도한 커맨드가
 *    변형되거나 지연되므로 우회한다.
 * 2) elevator 있음 → 스케줄러의 insert_requests 콜백
 *    mq-deadline이면 정렬 rb-tree와 FIFO 리스트에 넣는다.
 * 3) elevator 없음 → 소프트웨어 큐(ctx->rq_lists)
 *    NVMe 기본 설정(none 스케줄러)에서 가장 흔한 경로다. 스케줄러 오버헤드 없이
 *    CPU별 소프트웨어 큐에 넣었다가 dispatch 시 하드웨어 큐로 옮긴다.
 *
 * 실행 컨텍스트: blk_finish_plug 경로(프로세스) 또는 kblockd 워커.
 * percpu_ref로 큐 참조를 잡아 삽입 도중 큐가 해제되지 않도록 보장한다.
 *
 * 호출 체인:
 *   blk_mq_flush_plug_list → [blk_mq_dispatch_list]
 *     → (passthrough) hctx->dispatch + blk_mq_run_hw_queue
 *     → (elevator)    ops.insert_requests + blk_mq_run_hw_queue
 *     → (기본)        blk_mq_insert_requests
 *   이후 blk_mq_run_hw_queue → blk_mq_sched_dispatch_requests
 *     → blk_mq_dispatch_rq_list → nvme_queue_rq → SQ doorbell
 */
static void blk_mq_dispatch_list(struct rq_list *rqs, bool from_sched)
{
	/* [한국어] 이번 라운드의 기준이 될 하드웨어 큐. 첫 request가 정한다.
	 * NVMe에서 hctx 하나 = nvme_queue 하나 = SQ/CQ 쌍 하나다. */
	struct blk_mq_hw_ctx *this_hctx = NULL;
	/* [한국어] 기준 소프트웨어 큐(= per-CPU 컨텍스트). 같은 hctx라도 ctx가 다르면
	 * 삽입할 리스트(ctx->rq_lists[hctx_idx])가 달라 따로 처리해야 한다. */
	struct blk_mq_ctx *this_ctx = NULL;
	/* [한국어] 기준과 맞지 않아 다음 라운드로 미룰 request들을 모으는 리스트. */
	struct rq_list requeue_list = {};
	/* [한국어] 이번 라운드에 묶인 request 수. tracepoint에 넘겨 blkparse에서
	 * "한 번의 unplug로 몇 개가 나갔는가"를 볼 수 있게 한다. */
	unsigned int depth = 0;
	/* [한국어] 기준 그룹이 passthrough인지. 일반 I/O와 절대 섞으면 안 되므로
	 * 그룹 판별 조건에 포함된다. */
	bool is_passthrough = false;
	/* [한국어] 이번 라운드에 묶인 request들을 담을 지역 리스트. rq_list(단일 연결)와
	 * 달리 list_head(이중 연결)를 쓰는 이유는, 아래 삽입 API들이 전부
	 * list_splice 계열을 쓰는 struct list_head 인터페이스이기 때문이다. */
	LIST_HEAD(list);

	/* [한국어] rqs를 전부 소비할 때까지 순회하며 동질 그룹을 골라낸다.
	 * do-while인 이유: 호출자가 비어 있지 않은 리스트만 넘긴다는 계약이 있어
	 * 첫 pop이 항상 유효하기 때문이다. */
	do {
		struct request *rq = rq_list_pop(rqs);

		if (!this_hctx) {	/* [한국어] 아직 이번 라운드의 기준이 정해지지 않았는가(첫 반복) */
			/* [한국어] 첫 request가 이번 라운드의 기준을 정한다. 어떤 순서로
			 * 정렬하지 않고 "맨 앞의 것"을 기준으로 삼는 단순한 전략인데,
			 * 실제로는 plug 리스트 안의 request 대부분이 같은 CPU에서 발행되어
			 * 같은 hctx/ctx를 갖기 때문에 보통 한 라운드로 끝난다. */
			this_hctx = rq->mq_hctx;
			this_ctx = rq->mq_ctx;	/* [한국어] 기준 sw 큐 확정 — 이후 이 값과 다른 request 는 다음 라운드로 미룬다 */
			is_passthrough = blk_rq_is_passthrough(rq);	/* [한국어] 기준 종류(passthrough 인지 일반 I/O 인지) 확정 — 삽입 대상 자료구조가 완전히 달라서 섞을 수 없다 */
		} else if (this_hctx != rq->mq_hctx || this_ctx != rq->mq_ctx ||	/* [한국어] hctx 또는 ctx 가 기준과 다르거나 */
			   is_passthrough != blk_rq_is_passthrough(rq)) {	/* [한국어] passthrough 여부가 기준과 다르면 — 셋 중 하나라도 다르면 같은 라운드에 넣을 수 없다 */
			/* [한국어] 셋 중 하나라도 다르면 이번 라운드에 넣을 수 없다.
			 * requeue_list에 모아 두었다가 마지막에 rqs로 되돌려, 호출자가
			 * 다음 라운드에서 처리하게 한다. 순서를 보존하기 위해 tail에 붙인다. */
			rq_list_add_tail(&requeue_list, rq);
			continue;
		}
		/* [한국어] 기준과 일치 — 이번 라운드 리스트에 추가한다. */
		list_add_tail(&rq->queuelist, &list);
		depth++;	/* [한국어] 이번 라운드에 모은 개수 — 아래 trace_block_unplug 에 넘길 깊이 값이다 */
	} while (!rq_list_empty(rqs));

	/* [한국어] 미처리분을 호출자에게 되돌린다. 호출자는 rqs가 빌 때까지 이 함수를
	 * 다시 부른다. 그룹이 하나뿐이었다면 requeue_list는 비어 있어 루프가 끝난다. */
	*rqs = requeue_list;
	/* [한국어] blktrace에 unplug 사건 기록. 세 번째 인자가 "명시적(동기) unplug인가"로,
	 * from_sched(워커에서 온 지연 처리)의 반대값을 넘긴다. blkparse에서 "U"
	 * 이벤트로 나타나며 plug 효율을 관찰하는 지표다. */
	trace_block_unplug(this_hctx->queue, depth, !from_sched);

	/* [한국어] 큐 사용 참조를 획득한다. 삽입과 run이 진행되는 동안 다른 스레드가
	 * 큐를 freeze하거나 해제하지 못하게 막는 장치다. percpu_ref는 평상시
	 * per-CPU 카운터 증가라 거의 공짜이고, freeze가 시작될 때만 atomic 모드로
	 * 전환되어 비용을 낸다. NVMe에서 컨트롤러 리셋이나 네임스페이스 제거가
	 * 진행 중일 때 이 참조가 진행을 막아 안전을 보장한다. */
	percpu_ref_get(&this_hctx->queue->q_usage_counter);
	/* passthrough requests should never be issued to the I/O scheduler */
	if (is_passthrough) {	/* [한국어] 이번 라운드가 passthrough 그룹인가 */
		/* [한국어] 경로 1 — passthrough. 스케줄러를 완전히 우회해 hctx->dispatch에
		 * 직접 넣는다. nvme-cli가 보내는 Identify, Get Log Page, 벤더 고유 커맨드
		 * 등이 여기로 온다. 스케줄러의 정렬/병합은 LBA 기반 최적화라 이런 커맨드에
		 * 적용할 수 없고, 지연시키면 사용자 도구가 멈춘 것처럼 보인다.
		 * hctx->lock으로 dispatch 리스트를 보호한다 — 이 리스트는 여러 CPU가
		 * 동시에 접근할 수 있는 유일한 하드웨어 큐 단위 리스트다. */
		spin_lock(&this_hctx->lock);
		/* [한국어] 지역 리스트 전체를 dispatch 리스트 끝으로 O(1) 이동하고 지역
		 * 리스트를 비운다. 개별 삽입이 아니라 splice라서 request 수와 무관하게
		 * 락 보유 시간이 일정하다. */
		list_splice_tail_init(&list, &this_hctx->dispatch);
		spin_unlock(&this_hctx->lock);	/* [한국어] splice 가 끝났으므로 dispatch 리스트 락 해제 */
		/* [한국어] 큐를 돌려 방금 넣은 것들을 드라이버로 내보낸다. */
		blk_mq_run_hw_queue(this_hctx, from_sched);
	} else if (this_hctx->queue->elevator) {	/* [한국어] passthrough 가 아니고 I/O 스케줄러가 붙어 있는 경우 */
		/* [한국어] 경로 2 — I/O 스케줄러가 붙어 있다. 스케줄러의 insert_requests
		 * 콜백에 리스트를 통째로 넘긴다(mq-deadline이면 dd_insert_requests).
		 * 스케줄러가 자체 자료구조에 넣고 락도 스스로 관리하므로 여기서는
		 * 락을 잡지 않는다. 이후 dispatch 시 스케줄러가 순서를 정해 꺼낸다. */
		this_hctx->queue->elevator->type->ops.insert_requests(this_hctx,
				&list, 0);
		blk_mq_run_hw_queue(this_hctx, from_sched);	/* [한국어] 스케줄러 자료구조에 넣었으니 dispatch 를 돌려 실제로 드라이버까지 내보낸다 */
	} else {
		/* [한국어] 경로 3 — 스케줄러 없음(NVMe 기본값 none). CPU별 소프트웨어 큐에
		 * 넣는다. 이 함수는 삽입과 큐 실행을 함께 처리하므로 별도의
		 * blk_mq_run_hw_queue 호출이 없다.
		 * 소프트웨어 큐를 한 단계 두는 이유는 락 경합 분산이다 — 여러 CPU가
		 * 각자의 ctx에 넣으면 서로 부딪히지 않고, dispatch 시점에만 하나로
		 * 모아 하드웨어 큐로 보낸다. */
		blk_mq_insert_requests(this_hctx, this_ctx, &list, from_sched);
	}
	/* [한국어] 큐 사용 참조 반납. 이 시점 이후 freeze가 진행될 수 있다. */
	percpu_ref_put(&this_hctx->queue->q_usage_counter);
}

/*
 * [한국어]
 * blk_mq_dispatch_multiple_queue_requests - 여러 queue 의 request 를 queue 별로 분리 dispatch
 *
 * @rqs: 여러 request_queue 의 request 가 섞인 rq_list
 *
 * plug 에 md, 여러 NVMe namespace 등 다른 queue 의 request 가 섞여 있을 때 사용.
 * blk_mq_extract_queue_requests 로 동일 queue 묶음을 꺼내 각각 dispatch 한다.
 */
static void blk_mq_dispatch_multiple_queue_requests(struct rq_list *rqs)
{
	do {
		struct rq_list queue_rqs;	/* [한국어] 한 번에 뽑아낸 "같은 큐" request 들을 담을 리스트 */
		unsigned depth;	/* [한국어] 이번에 뽑아낸 개수 — 통계/추적용으로 dispatch 함수에 전달한다 */

		/* [한국어] 동일 queue 의 request 들을 queue_rqs 로 추출 */
		depth = blk_mq_extract_queue_requests(rqs, &queue_rqs);
		/* [한국어] queue_rqs 를 batch dispatch (queue_rqs hook 사용 가능) */
		blk_mq_dispatch_queue_requests(&queue_rqs, depth);
		/* [한국어] batch dispatch 에서 처리 못한 나머지를 blk_mq_dispatch_list 로 */
		while (!rq_list_empty(&queue_rqs))
			blk_mq_dispatch_list(&queue_rqs, false);
	} while (!rq_list_empty(rqs));
}

/*
 * [한국어]
 * blk_mq_flush_plug_list - plug 에 축적된 request 들을 일괄 dispatch (unplug)
 *
 * @plug:          flush 할 blk_plug
 * @from_schedule: blk_schedule_flush_plug 등 스케줄러에서 호출되면 true
 *
 * blk_finish_plug 또는 I/O scheduler 종료 시 호출된다.
 * elevator 없고 동기 경로면 batch dispatch (queue_rqs/issue_direct).
 * elevator 있거나 from_schedule 이면 blk_mq_dispatch_list 로 개별 insert + run.
 * 재귀 호출 방지를 위해 rq_count 를 먼저 0 으로 초기화한다.
 *
 * 호출 체인:
 *   blk_finish_plug → [blk_mq_flush_plug_list]
 *   → blk_mq_dispatch_queue_requests / blk_mq_dispatch_list
 *   → blk_mq_insert_requests / __blk_mq_flush_list → nvme_queue_rq
 */
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
	/* [한국어] rq_count == 0: 이미 flush 되었거나 재귀 호출 — 즉시 반환 */
	if (plug->rq_count == 0)
		return;
	/* [한국어] rq_count 를 먼저 0 으로 설정: 재귀 호출 방지 */
	depth = plug->rq_count;
	plug->rq_count = 0;	/* [한국어] 재귀 진입 방지 — 아래에서 부르는 dispatch 경로가 다시 flush 를 시도해도 위의 rq_count==0 검사에 걸려 즉시 돌아온다 */

	if (!plug->has_elevator && !from_schedule) {	/* [한국어] 스케줄러가 없고 동기 unplug 인 경우에만 배치 fast path 를 쓴다. 스케줄러가 있으면 순서 결정을 스케줄러에 맡겨야 하고, 워커 경로면 지연이 덜 민감하다 */
		/* [한국어] elevator 없고 동기 경로: batch dispatch fast path */
		if (plug->multiple_queues) {
			/* [한국어] 여러 queue 가 섞임: queue 별로 분리 후 batch dispatch */
			blk_mq_dispatch_multiple_queue_requests(&plug->mq_list);
			return;
		}

		/* [한국어] 단일 queue: 전체 list 를 한 번에 batch dispatch */
		blk_mq_dispatch_queue_requests(&plug->mq_list, depth);
		if (rq_list_empty(&plug->mq_list))	/* [한국어] 배치 경로가 전부 소비했는가 */
			return;
	}

	/* [한국어] elevator 있거나 from_schedule: dispatch_list 로 개별 처리.
	 * 남은 request 들이 있는 한 반복 */
	do {
		blk_mq_dispatch_list(&plug->mq_list, from_schedule);
	} while (!rq_list_empty(&plug->mq_list));
}

/*
 * [한국어]
 * blk_mq_try_issue_list_directly - list 의 request 를 동기 즉시 발행
 *
 * @hctx: 이 list 에 속하는 HW queue (NVMe SQ 에 대응)
 * @list: 발행할 struct request 의 queuelist 연결 목록
 *
 * blk_mq_insert_requests 의 dispatch_busy=0 fast path 에서 호출.
 * list 의 request 를 앞에서부터 하나씩 꺼내 blk_mq_request_issue_directly 로
 * 드라이버(nvme_queue_rq)에 전달한다. 자원 부족(BLK_STS_RESOURCE/DEV_RESOURCE)
 * 이면 bypass_insert 후 hctx->run 예약, 기타 오류는 즉시 완료 처리.
 *
 * 호출 체인:
 *   blk_mq_insert_requests → [blk_mq_try_issue_list_directly]
 *   → blk_mq_request_issue_directly → nvme_queue_rq
 */
static void blk_mq_try_issue_list_directly(struct blk_mq_hw_ctx *hctx,
		struct list_head *list)
{
	/* [한국어] queued: 이번 배치에서 성공적으로 SQ 에 삽입된 request 수 */
	int queued = 0;
	blk_status_t ret = BLK_STS_OK;	/* [한국어] 드라이버가 마지막으로 돌려준 상태 — 루프를 빠져나온 뒤 dispatch_busy 갱신에 쓰인다 */

	while (!list_empty(list)) {	/* [한국어] 리스트가 빌 때까지 하나씩 직접 발행 */
		/* [한국어] list 에서 첫 번째 request 를 꺼냄 */
		struct request *rq = list_first_entry(list, struct request,
				queuelist);

		list_del_init(&rq->queuelist);	/* [한국어] 리스트에서 떼어내고 노드를 초기화 — 아래에서 bypass_insert 로 다시 넣을 수 있으므로 링크를 깨끗이 해 둔다 */
		/* [한국어] last=list_empty(list): 마지막 request 이면 true → commit_rqs 유발 */
		ret = blk_mq_request_issue_directly(rq, list_empty(list));
		switch (ret) {	/* [한국어] 직접 발행 결과에 따라 분기 */
		case BLK_STS_OK:	/* [한국어] 드라이버가 받아 갔다 */
			/* [한국어] NVMe SQ 에 성공적으로 배치됨 */
			queued++;
			break;
		case BLK_STS_RESOURCE:	/* [한국어] 호스트 측 자원 부족 */
			/* [한국어] NVMe CID(tag)/SQ entry 자원 부족 — bypass 삽입 후 중단 */
		case BLK_STS_DEV_RESOURCE:
			/* [한국어] NVMe 컨트롤러 내부 자원 부족 — bypass 삽입 후 중단 */
			blk_mq_request_bypass_insert(rq, 0);
			/* [한국어] 남은 request 없으면 hctx run 예약하여 재시도 */
			if (list_empty(list))
				blk_mq_run_hw_queue(hctx, false);
			goto out;	/* [한국어] 자원 부족으로 이번 배치 중단 — 남은 request 는 호출자가 sw 큐로 넣는다 */
		default:
			/* [한국어] 기타 오류: request 를 즉시 에러 완료 처리 */
			blk_mq_end_request(rq, ret);
			break;
		}
	}

out:
	/* [한국어] 자원 부족으로 중단됐으면 commit_rqs: NVMe doorbell 일괄 처리 */
	if (ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued, false);
}

/*
 * [한국어]
 * blk_mq_attempt_bio_merge - bio 를 기존 request 에 병합 시도
 *
 * @q:      request_queue (NVMe 네임스페이스)
 * @bio:    병합하려는 신규 bio
 * @nr_segs: bio 의 segment 수
 * @return: true = 병합 성공(bio 소유권 이전됨), false = 새 request 필요
 *
 * blk_mq_submit_bio 의 초기 단계에서 호출.
 * merge 가 성공하면 새 request 할당(CID 획득) 없이 기존 NVMe 명령에 IO 가 합쳐진다.
 * 1) plug merge: 현재 스레드의 plug list 내 인접한 request 와 병합.
 * 2) scheduler merge: elevator (예: mq-deadline) 가 관리하는 RB-tree 에서 병합.
 */
static bool blk_mq_attempt_bio_merge(struct request_queue *q,
				     struct bio *bio, unsigned int nr_segs)
{
	if (!blk_queue_nomerges(q) && bio_mergeable(bio)) {
		/* [한국어] REQ_NOMERGE 플래그 없고 bio 가 merge 가능하면 시도 */
		if (blk_attempt_plug_merge(q, bio, nr_segs))
			/* [한국어] plug list 에서 병합 성공: bio 는 기존 request 로 흡수됨 */
			return true;
		if (blk_mq_sched_bio_merge(q, bio, nr_segs))	/* [한국어] plug 병합이 실패했으면 스케줄러가 들고 있는 요청들과의 병합을 시도한다(mq-deadline 의 RB-tree 등) */
			/* [한국어] elevator RB-tree 에서 병합 성공 */
			return true;
	}
	return false;
}

/*
 * [한국어]
 * blk_mq_get_new_requests - bio 에 대한 새 request(NVMe 명령 슬롯) 할당
 *
 * @q:    request_queue (NVMe 네임스페이스)
 * @plug: 현재 스레드의 blk_plug (배치 CID 사전 할당용)
 * @bio:  요청 원본 bio
 * @return: 할당된 request 포인터, 실패 시 NULL
 *
 * blk_mq_submit_bio 에서 cached_request 가 없을 때 호출.
 * QoS throttle → CID/tag 할당 → 실패 시 QoS cleanup 순으로 진행.
 * plug 가 있으면 nr_ios 만큼 sbitmap slot 을 미리 예약해 배치 효율을 높인다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → [blk_mq_get_new_requests]
 *   → rq_qos_throttle → __blk_mq_alloc_requests → blk_mq_get_tag
 */
static struct request *blk_mq_get_new_requests(struct request_queue *q,
					       struct blk_plug *plug,
					       struct bio *bio)
{
	struct blk_mq_alloc_data data = {
		.q		= q,	/* [한국어] 태그를 잡을 대상 큐 */
		.flags		= 0,	/* [한국어] 기본 동작 — NOWAIT/RESERVED 같은 특수 플래그 없음. NOWAIT 는 아래에서 bio->bi_opf 의 REQ_NOWAIT 를 보고 __blk_mq_alloc_requests 가 스스로 세운다 */
		.shallow_depth	= 0,	/* [한국어] 0 = 깊이 제한 없음. 스케줄러가 async 쓰기 등을 억제할 때 blk_mq_limit_depth 가 값을 채운다 */
		/* [한국어] cmd_flags: REQ_OP_READ/WRITE 등 bio 의 opcode */
		.cmd_flags	= bio->bi_opf,
		.rq_flags	= 0,
		/* [한국어] nr_tags: 기본 1, plug 있으면 nr_ios 만큼 일괄 예약 */
		.nr_tags	= 1,
		.cached_rqs	= NULL,
		/* [한국어] ctx/hctx: 할당 후 smp_processor_id 기준으로 채워짐 */
		.ctx		= NULL,
		.hctx		= NULL
	};
	struct request *rq;	/* [한국어] 할당된 request. 실패하면 NULL */

	/* [한국어] rq_qos_throttle: iocost/iolatency QoS 정책에 따라 이 bio 를 지연/제한 */
	rq_qos_throttle(q, bio);

	if (plug) {	/* [한국어] plug 가 열려 있으면 배치 예약을 시도한다 */
		/* [한국어] plug 배치 최적화: nr_ios 개 CID 를 한 번에 sbitmap 예약 */
		data.nr_tags = plug->nr_ios;
		plug->nr_ios = 1;	/* [한국어] 힌트를 1 로 되돌린다 — 배치 예약은 plug 당 한 번만 신뢰한다 */
		data.cached_rqs = &plug->cached_rqs;	/* [한국어] 여분으로 잡힌 request 들이 쌓일 곳을 plug 캐시로 지정 */
	}

	/* [한국어] __blk_mq_alloc_requests: ctx/hctx 선택 + sbitmap 에서 CID 획득 */
	rq = __blk_mq_alloc_requests(&data);
	if (unlikely(!rq))	/* [한국어] 태그 고갈 등으로 할당 실패 */
		/* [한국어] 할당 실패: throttle 에서 증가시킨 QoS 카운터를 되돌림 */
		rq_qos_cleanup(q, bio);
	return rq;	/* [한국어] 할당된 request(실패 시 NULL)를 반환 */
}

/*
 * Check if there is a suitable cached request and return it.
 */
/*
 * [한국어]
 * blk_mq_peek_cached_request - plug 의 pre-allocated request 를 재사용 가능한지 확인
 *
 * @plug: 현재 스레드의 blk_plug
 * @q:    요청 대상 request_queue (NVMe 네임스페이스)
 * @opf:  bio->bi_opf (REQ_OP_READ/WRITE 등)
 * @return: 재사용 가능한 request 포인터, 없으면 NULL
 *
 * blk_mq_get_new_requests 의 배치 CID 사전 할당 최적화의 소비 측.
 * cached_rqs 에 있는 request 가 (queue, hctx type, flush) 모두 일치하면
 * 새 CID 를 sbitmap 에서 획득하지 않고 기존 slot 을 재사용한다.
 * READ가 DEFAULT hctx type 에 매핑될 수 있음을 허용하는 예외 조건 포함.
 */
static struct request *blk_mq_peek_cached_request(struct blk_plug *plug,
		struct request_queue *q, blk_opf_t opf)
{
	/* [한국어] hctx type: DEFAULT(일반 read/write) / READ(전용 read 큐) / POLL */
	enum hctx_type type = blk_mq_get_hctx_type(opf);
	struct request *rq;	/* [한국어] plug 캐시의 첫 request 를 들여다볼 커서 */

	/* [한국어] plug 없으면 cached request 도 없음 */
	if (!plug)
		return NULL;
	/* [한국어] cached_rqs 의 첫 번째 request 를 들여다봄 (pop 하지 않음) */
	rq = rq_list_peek(&plug->cached_rqs);
	/* [한국어] 다른 queue (NVMe 네임스페이스) 의 request 면 재사용 불가 */
	if (!rq || rq->q != q)
		return NULL;
	/* [한국어] hctx type 불일치: READ 요청이 DEFAULT hctx 에 배치된 경우만 허용 */
	if (type != rq->mq_hctx->type &&
	    (type != HCTX_TYPE_READ || rq->mq_hctx->type != HCTX_TYPE_DEFAULT))
		return NULL;
	/* [한국어] flush 여부 불일치: flush request 는 flush request 캐시만 재사용 */
	if (op_is_flush(rq->cmd_flags) != op_is_flush(opf))
		return NULL;
	return rq;	/* [한국어] 네 가지 호환성 검사(같은 큐 / hctx 타입 / flush 여부)를 모두 통과한 재사용 후보 반환 */
}

/*
 * [한국어]
 * blk_mq_use_cached_rq - pre-allocated cached request 를 이 bio 에 바인딩
 *
 * @rq:   blk_mq_peek_cached_request 가 반환한 cached request
 * @plug: 현재 스레드의 blk_plug (cached_rqs 소유자)
 * @bio:  이 request 에 매핑할 bio
 *
 * QoS throttle 이 block 되면 plug 가 flush 되어 cached_rqs 가 사라지므로,
 * 반드시 pop(소유권 이전) 후에 throttle 을 호출해야 한다.
 * 이후 rq 의 시간/cmd_flags/queuelist 를 초기화하여 재사용 준비 완료.
 */
static void blk_mq_use_cached_rq(struct request *rq, struct blk_plug *plug,
		struct bio *bio)
{
	/* [한국어] cached_rqs 에서 rq 를 pop: 소유권을 호출자 측으로 이전 */
	if (rq_list_pop(&plug->cached_rqs) != rq)
		WARN_ON_ONCE(1);

	/*
	 * If any qos ->throttle() end up blocking, we will have flushed the
	 * plug and hence killed the cached_rq list as well. Pop this entry
	 * before we throttle.
	 */
	/* [한국어] QoS throttle: iocost/iolatency 가 이 bio 를 지연시킬 수 있음 */
	rq_qos_throttle(rq->q, bio);

	/* [한국어] rq 시작 시각 초기화: 타임아웃 계산의 기준 */
	blk_mq_rq_time_init(rq, blk_time_get_ns());
	/* [한국어] cmd_flags: 이 bio 의 opcode 로 갱신 */
	rq->cmd_flags = bio->bi_opf;
	/* [한국어] queuelist 초기화: plug list / scheduler queue 삽입 준비 */
	INIT_LIST_HEAD(&rq->queuelist);
}

/*
 * [한국어]
 * bio_unaligned - bio 의 LBA/length 가 논리 블록 크기 정렬을 위반하는지 검사
 *
 * @bio: 검사 대상 bio
 * @q:   request_queue (NVMe 네임스페이스의 logical_block_size 보유)
 * @return: true = 정렬 위반 (즉시 EIO), false = 정상
 *
 * NVMe 컨트롤러는 LBA 단위(통상 512B 또는 4KiB)로만 명령을 수용한다.
 * bs_mask = logical_block_size - 1: 하위 비트 마스크로 정렬 여부 확인.
 * bi_sector 는 512B sector 단위이므로 << SECTOR_SHIFT 로 byte 단위로 변환 후 검사.
 */
static bool bio_unaligned(const struct bio *bio, struct request_queue *q)
{
	/* [한국어] bs_mask: 논리 블록 크기 - 1. 이 비트가 켜지면 정렬 위반 */
	unsigned int bs_mask = queue_logical_block_size(q) - 1;

	/* .bi_sector of any zero sized bio need to be initialized */
	/* [한국어] bi_size 가 블록 크기 배수인지, bi_sector 바이트 오프셋이 정렬됐는지 확인 */
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
 * [한국어]
 * blk_mq_submit_bio - bio 를 NVMe SQ 까지 전달하는 중심 경로
 *
 * @bio: 파일시스템/페이지캐시가 제출한 I/O 요청 (원자적 bio 단위)
 *
 * 모든 블록 I/O 는 이 함수를 거친다.
 * 전처리(정렬/분할/무결성/merge/zone-write-plug) → request 할당(CID) →
 * 발행 경로 결정(plug batch / scheduler / 직접 dispatch) 의 흐름.
 *
 * NVMe 관점 전체 호출 체인:
 *   submit_bio (VFS/파일시스템) → [blk_mq_submit_bio]
 *   → blk_mq_get_new_requests → __blk_mq_alloc_requests (CID = sbitmap slot)
 *   → blk_mq_bio_to_request (bio → rq field 복사, LBA/length/PRP 기초)
 *   → (plug 있으면) blk_add_rq_to_plug → blk_mq_flush_plug_list
 *   → blk_mq_dispatch_rq_list → (mq_ops->queue_rq == nvme_queue_rq)
 *     → nvme_sq_copy_cmd(SQE 기록) → nvme_write_sq_db(SQ doorbell)
 *   → (직접) blk_mq_try_issue_directly → nvme_queue_rq → doorbell
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 (I/O 를 발행하는 유저/커널 스레드).
 * 인터럽트/softirq 에서 호출 금지.
 */
void blk_mq_submit_bio(struct bio *bio)
{
	/* [한국어] q: bdev 에서 request_queue 를 얻어옴 (NVMe 네임스페이스 큐) */
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	/* [한국어] plug: 현재 프로세스의 IO plug (배치 batch 최적화용) */
	struct blk_plug *plug = current->plug;
	/* [한국어] is_sync: REQ_OP_READ 등 동기 작업이면 true — 직접 dispatch 경로 결정 */
	const int is_sync = op_is_sync(bio->bi_opf);
	unsigned int integrity_action;	/* [한국어] bio_integrity_action() 의 판정 결과 — 무결성 버퍼를 할당만 할지, 0 으로 채울지, 체크섬까지 생성할지 */
	struct blk_mq_hw_ctx *hctx;	/* [한국어] 이 요청이 향할 하드웨어 큐. 아래 즉시 dispatch 경로에서만 쓰인다 */
	unsigned int nr_segs;	/* [한국어] __bio_split_to_limits 가 채워 주는 물리 세그먼트 수 — NVMe 라면 PRP 엔트리 또는 SGL 디스크립터 개수의 근거 */
	struct request *rq;	/* [한국어] 이 bio 를 실어 보낼 request. 캐시 재사용 또는 신규 할당으로 채워진다 */
	blk_status_t ret;	/* [한국어] blk_crypto_rq_get_keyslot 의 결과를 받을 변수 */

	/*
	 * If the plug has a cached request for this queue, try to use it.
	 */
	/* [한국어] cached_request 확인: plug 배치 사전 할당된 CID 재사용 가능하면 peek */
	rq = blk_mq_peek_cached_request(plug, q, bio->bi_opf);

	/*
	 * A BIO that was released from a zone write plug has already been
	 * through the preparation in this function, already holds a reference
	 * on the queue usage counter, and is the only write BIO in-flight for
	 * the target zone. Go straight to preparing a request for it.
	 */
	if (bio_zone_write_plugging(bio)) {	/* [한국어] zone write plug 에서 풀려난 bio 인가 — 이미 이 함수를 한 번 통과했으므로 준비 단계를 전부 건너뛴다(위 영문 주석) */
		/* [한국어] zoned NVMe zone write plug 에서 해제된 bio:
		 * q_usage_counter 이미 보유, 정렬/분할 처리 완료 → request 할당 직행 */
		nr_segs = bio->__bi_nr_segments;
		/* [한국어] cached_rq 는 q_usage_counter 를 보유하므로 미리 반납 */
		if (rq)
			blk_queue_exit(q);
		goto new_request;	/* [한국어] 분할·정렬·무결성 준비를 모두 건너뛰고 request 할당 지점으로 직행 */
	}

	/*
	 * The cached request already holds a q_usage_counter reference and we
	 * don't have to acquire a new one if we use it.
	 */
	if (!rq) {	/* [한국어] 캐시에서 재사용할 request 를 못 얻었으면 — 이 경로에서만 새로 큐 참조를 잡아야 한다 */
		/* [한국어] cached_rq 없으면 q_usage_counter 직접 획득:
		 * queue freeze/teardown 중이면 bio 에 EIO 반환 */
		if (unlikely(bio_queue_enter(bio)))
			return;
	}

	/*
	 * Device reconfiguration may change logical block size or reduce the
	 * number of poll queues, so the checks for alignment and poll support
	 * have to be done with queue usage counter held.
	 */
	/* [한국어] 정렬 검증. 위 영문 주석이 밝히듯 이 검사가 q_usage_counter를
	 * 쥔 뒤에 와야 하는 이유가 있다 — 장치 재구성으로 논리 블록 크기가
	 * 바뀔 수 있는데, 참조를 잡기 전에 검사하면 검사와 사용 사이에 기준이
	 * 달라진다.
	 * 정렬 위반은 분할로도 고칠 수 없는 오류다(어떻게 나눠도 시작이나 길이가
	 * 어긋난 채로 남는다). O_DIRECT 사용자가 정렬되지 않은 버퍼나 오프셋을
	 * 넘겼을 때 여기 도달하며, 최종적으로 EIO가 전달된다. */
	if (unlikely(bio_unaligned(bio, q))) {
		bio_io_error(bio);
		goto queue_exit;	/* [한국어] 큐 참조를 반납하는 공통 정리 경로로. bio 는 이미 bio_io_error 로 종료시켰다 */
	}

	/* [한국어] 폴링 요청인데 이 큐에 폴링 전용 하드웨어 큐가 없는 경우.
	 * NVMe에서 poll 큐는 nvme 모듈의 poll_queues 파라미터로 만들어지며,
	 * 인터럽트를 쓰지 않고 io_uring의 IOPOLL이 직접 CQ를 돌며 완료를
	 * 수거한다. 그 큐가 없으면 폴링 자체가 불가능하므로 명시적으로 거부한다.
	 * 이 검사도 큐 참조 하에 있어야 한다 — 재구성으로 poll 큐 수가 0이
	 * 될 수 있기 때문이다. */
	if ((bio->bi_opf & REQ_POLLED) && !blk_mq_can_poll(q)) {
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);	/* [한국어] BLK_STS_NOTSUPP 를 실어 bio 를 즉시 종료 — 상위는 EOPNOTSUPP 를 받는다 */
		goto queue_exit;	/* [한국어] 큐 참조 반납 경로로 */
	}

	/* [한국어] ★ 분할 지점 ★
	 * queue_limits를 초과하는 bio를 장치가 받을 수 있는 크기로 자른다.
	 * NVMe PCIe 에서 그 한계의 출처는 (drivers/nvme/host 에서 확인):
	 *   max_hw_sectors      ← Identify Controller 의 MDTS 필드
	 *                         (nvme_set_ctrl_limits: lim->max_hw_sectors = ctrl->max_hw_sectors)
	 *   max_segments        ← ctrl->max_segments = NVME_MAX_SEGS, 이는
	 *                         NVME_CTRL_PAGE_SIZE / sizeof(struct nvme_sgl_desc)
	 *                         = 디스크립터 페이지 하나에 들어가는 SGL 엔트리 수
	 *                         (4KiB 페이지 · 16B 엔트리면 256)
	 *   virt_boundary_mask  ← ctrl->ops->get_virt_boundary(). PCIe 구현
	 *                         nvme_pci_get_virt_boundary() 는 컨트롤러가 SGL 을
	 *                         지원하지 않거나 admin 큐이면 NVME_CTRL_PAGE_SIZE-1
	 *                         (= PRP 가 요구하는 페이지 정렬 제약)을, SGL 을 쓰는
	 *                         I/O 큐면 0(제약 없음)을 돌려준다.
	 * 반환값은 "이번에 처리할 앞부분"이고, 뒷부분은 이 함수 안에서 이미
	 * 큐 입구로 재제출되어 나중에 같은 경로를 다시 탄다.
	 * nr_segs에 세그먼트 수가 채워져 나오는데, 이 값이 곧 PRP 엔트리 또는
	 * SGL 디스크립터 개수가 된다(자세한 내용은 block/blk-merge.c). */
	bio = __bio_split_to_limits(bio, &q->limits, &nr_segs);
	if (!bio)	/* [한국어] 분할 함수가 NULL 을 돌려주면 bio 는 이미 종료된 상태다 */
		/* [한국어] 분할 불가(정렬 위반, 원자적 쓰기, NOWAIT 등)로 이미
		 * bio_endio까지 끝난 상태다. 우리가 할 일이 없다. */
		goto queue_exit;

	/* [한국어] 무결성(T10 PI / NVMe End-to-End Data Protection) 준비.
	 * bio_integrity_action()이 "이 bio에 무엇을 해야 하는가"를 판정한다 —
	 * 버퍼만 할당할지, 0으로 채울지, 체크섬까지 생성할지. 그 판정은
	 * 장치가 PI를 offload할 수 있는지(metadata_size == pi_tuple_size)에
	 * 달려 있다. 0이면 할 일이 없어 호출조차 하지 않는다. */
	integrity_action = bio_integrity_action(bio);
	if (integrity_action)	/* [한국어] 할 일이 있다고 판정된 경우에만 */
		bio_integrity_prep(bio, integrity_action);	/* [한국어] 무결성 메타데이터 버퍼를 준비/생성한다. NVMe 라면 이 메타데이터가 SQE 의 MPTR 또는 메타 SGL 로 실린다 */

	/* [한국어] bio에 발행 시각과 크기를 새긴다. blk-throttle과 blk-iolatency가
	 * 이 값을 기준으로 지연을 측정하므로, 병합이나 분할보다 먼저 찍어야
	 * "사용자가 요청한 시점"이 정확히 기록된다. */
	blk_mq_bio_issue_init(q, bio);
	/* [한국어] ★ 병합 시도 ★
	 * plug 리스트나 스케줄러의 기존 request에 이 bio를 붙일 수 있는지 본다.
	 * 성공하면 새 request도 새 태그(NVMe CID)도 필요 없다 — 기존 커맨드의
	 * NLB만 늘어난다. 순차 워크로드에서 이 경로의 적중률이 매우 높아,
	 * 같은 대역폭을 훨씬 적은 커맨드로 달성하게 해 준다. */
	if (blk_mq_attempt_bio_merge(q, bio, nr_segs))
		goto queue_exit;

	/* [한국어] ZNS 순차 쓰기 존에 대한 쓰기라면 zone write plug의 관리를
	 * 받아야 한다. 그 zone에 이미 진행 중인 쓰기가 있으면 true를 반환하는데,
	 * 이는 "실패"가 아니라 "plug가 이 bio를 큐에 넣고 나중에 제출하겠다"는
	 * 뜻이다. 그래야 zone write pointer 순서가 지켜진다.
	 * 이 처리가 병합 시도 "뒤"에 오는 이유: 병합에 성공했다면 기존 request가
	 * 이미 plug의 관리를 받고 있어 새로 등록할 필요가 없다. */
	if (bio_needs_zone_write_plugging(bio)) {
		if (blk_zone_plug_bio(bio, nr_segs))
			goto queue_exit;	/* [한국어] zone write plug 가 이 bio 를 자기 큐에 물어 두었다 — 나중에 풀려나면 위의 bio_zone_write_plugging 경로로 다시 들어온다 */
	}

new_request:
	if (rq) {	/* [한국어] 캐시에서 재사용할 request 를 확보한 경우 */
		/* [한국어] pre-allocated cached_rq 재사용: sbitmap 획득 없이 CID 재활용 */
		blk_mq_use_cached_rq(rq, plug, bio);
	} else {
		/* [한국어] 새 CID 할당: QoS throttle → sbitmap → hctx/ctx 선택 */
		rq = blk_mq_get_new_requests(q, plug, bio);
		if (unlikely(!rq)) {	/* [한국어] 태그 고갈 등으로 새 request 할당 실패 */
			/* [한국어] CID 고갈: REQ_NOWAIT 이면 EAGAIN, 아니면 block */
			if (bio->bi_opf & REQ_NOWAIT)
				bio_wouldblock_error(bio);
			goto queue_exit;	/* [한국어] 큐 참조 반납 경로로 */
		}
	}

	/* [한국어] block_getrq tracepoint: blktrace 가 request 생성 시각 기록 */
	trace_block_getrq(bio);

	/* [한국어] rq_qos_track: iocost/iolatency 가 이 request 에 QoS 어카운팅 시작 */
	rq_qos_track(q, rq, bio);

	/* [한국어] bio → rq 필드 복사: LBA(bi_iter), length(bi_size),
	 * nr_phys_segments, crypto context, io accounting 등 */
	blk_mq_bio_to_request(rq, bio, nr_segs);

	/* [한국어] NVMe inline encryption: keyslot manager 에서 키 슬롯 예약.
	 * 실패하면 bio 에 에러 반환 후 rq 해제 */
	ret = blk_crypto_rq_get_keyslot(rq);
	if (ret != BLK_STS_OK) {	/* [한국어] keyslot 확보 실패(inline 암호화 슬롯 부족 등) */
		bio->bi_status = ret;	/* [한국어] 오류를 bio 에 새겨 상위로 전달 */
		bio_endio(bio);	/* [한국어] bio 완료 — 상위가 errno 로 변환한다 */
		blk_mq_free_request(rq);	/* [한국어] request 를 해제하면 태그와 큐 참조가 함께 반납되므로 여기서는 goto queue_exit 를 쓰지 않는다 */
		return;
	}

	if (bio_zone_write_plugging(bio))	/* [한국어] zone write plug 에서 풀려난 bio 였다면 */
		/* [한국어] zone write plug 에서 해제된 bio: zone sequence 추적 초기화 */
		blk_zone_write_plug_init_request(rq);

	if (op_is_flush(bio->bi_opf) && blk_insert_flush(rq))	/* [한국어] FLUSH/FUA 요청이면 blk-flush 상태 기계에 넘긴다. true 는 "상태 기계가 이 request 의 소유권을 가져갔다"는 뜻 */
		/* [한국어] flush(FUA/FUA+WRITE_SAME): blk-flush 상태머신으로 전달.
		 * true = 상태머신이 rq 를 소유했음 → 여기서 바로 반환 */
		return;

	if (plug) {	/* [한국어] plug 가 열려 있으면 즉시 발행하지 않고 모아 둔다 */
		/* [한국어] plug 활성: request 를 plug->mq_list 에 누적.
		 * blk_finish_plug 시 blk_mq_flush_plug_list 로 일괄 dispatch */
		blk_add_rq_to_plug(plug, rq);
		return;
	}

	/* [한국어] plug 없음: 즉시 dispatch 경로 */
	hctx = rq->mq_hctx;
	if ((rq->rq_flags & RQF_USE_SCHED) ||	/* [한국어] 스케줄러를 써야 하는 요청이거나 */
	    (hctx->dispatch_busy && (q->nr_hw_queues == 1 || !is_sync))) {	/* [한국어] 이 hctx 가 최근 바빴고(단일 하드웨어 큐이거나 비동기 I/O 인 경우) — 직접 발행해 봐야 실패하고 되돌리는 비용만 들 가능성이 높다 */
		/* [한국어] scheduler 사용(RQF_USE_SCHED) 또는
		 * dispatch_busy=1 이면서 단일 HW queue 이거나 async I/O:
		 * sw queue 삽입 후 kblockd 에서 dispatch */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, true);	/* [한국어] 삽입했으니 비동기로 큐를 돌려 kblockd 가 내보내게 한다 */
	} else {
		/* [한국어] dispatch_busy 없고 sync + 다중 HW queue:
		 * 현재 CPU 에서 nvme_queue_rq 를 바로 호출 (doorbell) */
		blk_mq_run_dispatch_ops(q, blk_mq_try_issue_directly(hctx, rq));
	}
	return;

queue_exit:
	/*
	 * Don't drop the queue reference if we were trying to use a cached
	 * request and thus didn't acquire one.
	 */
	/* [한국어] cached_rq 를 사용하려 했다면 q_usage_counter 를 획득하지 않았으므로
	 * 반납하지 않음. 새로 획득한 경우에만 blk_queue_exit 호출 */
	if (!rq)
		blk_queue_exit(q);
}

#ifdef CONFIG_BLK_MQ_STACKING
/**
 * blk_insert_cloned_request - Helper for stacking drivers to submit a request
 * @rq: the request being queued
 */
/*
 * [한국어]
 * blk_insert_cloned_request - 스태킹 드라이버가 하위 큐에 cloned request 를 발행
 *
 * @rq: 상위 device 의 scheduler 를 거쳐 복제된 request
 * @return: BLK_STS_OK 성공, BLK_STS_NOTSUPP/IOERR 실패
 *
 * dm(device mapper), md(RAID) 등 스태킹 드라이버에서 사용.
 * 상위 device 의 scheduler 에서 내려온 rq 를 하위 NVMe 큐에 직접 삽입한다.
 * 하위 큐에는 scheduler 가 있더라도 반드시 bypass 해야 한다.
 * max_sectors/max_segments 초과 여부를 검사하여 EIO 반환.
 *
 * 호출 체인:
 *   dm/md 드라이버 → [blk_insert_cloned_request]
 *   → blk_mq_request_issue_directly → nvme_queue_rq → doorbell
 */
blk_status_t blk_insert_cloned_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	/* [한국어] max_sectors: 하위 NVMe namespace 의 최대 전송 sector 수 */
	unsigned int max_sectors = blk_queue_get_max_sectors(rq);
	/* [한국어] max_segments: 하위 NVMe namespace 의 최대 PRP/SGL entry 수 */
	unsigned int max_segments = blk_rq_get_max_segments(rq);
	blk_status_t ret;	/* [한국어] keyslot 확보와 직접 발행의 결과를 받을 변수 */

	if (blk_rq_sectors(rq) > max_sectors) {	/* [한국어] 하위 장치가 받을 수 있는 최대 전송 크기를 넘었는가 — 상위(dm/md)의 limit 가 하위와 다를 수 있어 반드시 재검사한다 */
		/* [한국어] sector 수 초과: 하위 NVMe max_sectors 보다 크면 EIO */
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
		/* [한국어] max_sectors == 0: 기능 미지원으로 큐 제한이 0이 됨 → NOTSUPP */
		if (max_sectors == 0)
			return BLK_STS_NOTSUPP;

		printk(KERN_ERR "%s: over max size limit. (%u > %u)\n",	/* [한국어] 한도 초과를 커널 로그에 남긴다 — 상위/하위 limit 불일치는 스택 구성 버그일 가능성이 높아 진단이 필요하다 */
			__func__, blk_rq_sectors(rq), max_sectors);	/* [한국어] 어느 함수에서 어떤 값이 한도를 넘었는지 함께 기록 */
		return BLK_STS_IOERR;	/* [한국어] 분할할 수 없는 clone 이므로 재시도 없이 I/O 오류로 끝낸다 */
	}

	/*
	 * The queue settings related to segment counting may differ from the
	 * original queue.
	 */
	/* [한국어] 하위 queue 의 segment 계산 방식으로 nr_phys_segments 재계산 */
	rq->nr_phys_segments = blk_recalc_rq_segments(rq);
	if (rq->nr_phys_segments > max_segments) {	/* [한국어] 하위 장치의 세그먼트 상한을 넘었는가 — NVMe 라면 SGL 디스크립터 페이지 하나에 담을 수 있는 개수(NVME_MAX_SEGS)를 넘은 것 */
		/* [한국어] segment 수 초과: NVMe PRP/SGL 리스트 용량 초과 → EIO */
		printk(KERN_ERR "%s: over max segments limit. (%u > %u)\n",
			__func__, rq->nr_phys_segments, max_segments);
		return BLK_STS_IOERR;	/* [한국어] 세그먼트 한도 초과 — I/O 오류로 끝낸다 */
	}

	/* [한국어] fault injection: 테스트 목적으로 강제 EIO 발생 */
	if (q->disk && should_fail_request(q->disk->part0, blk_rq_bytes(rq)))
		return BLK_STS_IOERR;

	/* [한국어] NVMe inline encryption: 하위 큐의 keyslot manager 에서 슬롯 예약 */
	ret = blk_crypto_rq_get_keyslot(rq);
	if (ret != BLK_STS_OK)	/* [한국어] keyslot 확보 실패 — 오류를 그대로 상위에 전파한다 */
		return ret;

	/* [한국어] IO accounting 시작: blk-cgroup, iostat 통계 기준 시각 기록 */
	blk_account_io_start(rq);

	/*
	 * Since we have a scheduler attached on the top device,
	 * bypass a potential scheduler on the bottom device for
	 * insert.
	 */
	/* [한국어] 상위에서 이미 스케줄링 된 request: 하위 scheduler 우회 직접 발행 */
	blk_mq_run_dispatch_ops(q,
			ret = blk_mq_request_issue_directly(rq, true));
	/* [한국어] 발행 실패하면 IO accounting 완료 처리 (에러 완료) */
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
/*
 * [한국어]
 * blk_rq_unprep_clone - cloned request 의 모든 bio 를 해제
 *
 * @rq: 정리할 cloned request
 *
 * blk_rq_prep_clone 실패 경로 또는 clone 완료 후 호출.
 * rq->bio 체인을 따라 bio_put 으로 모두 해제한다.
 */
void blk_rq_unprep_clone(struct request *rq)
{
	struct bio *bio;

	while ((bio = rq->bio) != NULL) {	/* [한국어] bio 체인이 빌 때까지 반복 */
		/* [한국어] bio 체인에서 다음 bio 로 이동 후 현재 bio 해제 */
		rq->bio = bio->bi_next;
		bio_put(bio);	/* [한국어] clone bio 의 참조를 내린다. 마지막 참조였다면 bio_set 으로 반환된다(페이지는 원본 소유라 여기서 해제되지 않는다) */
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
/*
 * [한국어]
 * blk_rq_prep_clone - 원본 request 를 복제하여 cloned request 준비
 *
 * @rq:      복제 대상 빈 request
 * @rq_src:  원본 request (상위 device 에서 내려온 것)
 * @bs:      clone bio 할당에 사용할 bio_set
 * @gfp_mask: 메모리 할당 플래그
 * @bio_ctr: 각 clone bio 에 대해 호출될 드라이버 초기화 콜백 (NULL 가능)
 * @data:    bio_ctr 에 전달할 private data
 * @return:  0 성공, -ENOMEM 실패
 *
 * dm, md 등에서 원본 bio 의 페이지를 공유하는 shallow clone 을 생성.
 * clone bio 가 먼저 완료되어야 원본 bio 해제 가능 (page 공유 때문).
 * 실패 시 blk_rq_unprep_clone 으로 부분 할당된 bio 들을 정리한다.
 */
int blk_rq_prep_clone(struct request *rq, struct request *rq_src,
		      struct bio_set *bs, gfp_t gfp_mask,
		      int (*bio_ctr)(struct bio *, struct bio *, void *),
		      void *data)
{
	struct bio *bio_src;

	/* [한국어] bs 미지정이면 기본 파일시스템 bio_set 사용 */
	if (!bs)
		bs = &fs_bio_set;

	__rq_for_each_bio(bio_src, rq_src) {	/* [한국어] 원본 request 의 bio 를 하나씩 순회하며 clone 을 만든다 */
		/* [한국어] bio_alloc_clone: 동일 page 를 참조하는 shallow clone bio 생성.
		 * 복사 없이 page pointer 만 공유 — zero-copy */
		struct bio *bio	 = bio_alloc_clone(rq->q->disk->part0, bio_src,
					gfp_mask, bs);
		if (!bio)	/* [한국어] clone bio 할당 실패 */
			goto free_and_out;	/* [한국어] 지금까지 만든 clone 들을 모두 정리하는 경로로 */

		/* [한국어] 드라이버 custom bio 초기화: dm_crypt 등이 여기서 추가 설정 */
		if (bio_ctr && bio_ctr(bio, bio_src, data)) {
			bio_put(bio);
			goto free_and_out;	/* [한국어] 드라이버 콜백이 실패했다 — 방금 만든 bio 를 먼저 되돌린 뒤 전체 정리로 간다 */
		}

		/* [한국어] rq->bio 체인에 clone bio 를 연결 (tail append) */
		if (rq->bio) {
			rq->biotail->bi_next = bio;
			rq->biotail = bio;	/* [한국어] 새 tail 갱신 */
		} else {
			rq->bio = rq->biotail = bio;	/* [한국어] 첫 clone 이면 head 와 tail 이 같다 */
		}
	}

	/* Copy attributes of the original request to the clone request. */
	/* [한국어] NVMe LBA 시작 주소 복사 */
	rq->__sector = blk_rq_pos(rq_src);
	/* [한국어] NVMe 전송 바이트 수 복사 */
	rq->__data_len = blk_rq_bytes(rq_src);
	/* [한국어] 특수 payload (discard, write-same): 플래그 + 벡터 복사 */
	if (rq_src->rq_flags & RQF_SPECIAL_PAYLOAD) {
		rq->rq_flags |= RQF_SPECIAL_PAYLOAD;
		rq->special_vec = rq_src->special_vec;	/* [한국어] discard/write-zeroes 처럼 데이터 대신 디스크립터 하나만 싣는 요청의 벡터를 그대로 복사한다 */
	}
	/* [한국어] NVMe PRP/SGL entry 수 복사 */
	rq->nr_phys_segments = rq_src->nr_phys_segments;
	/* [한국어] NVMe PI(T10 DIF/DIX) integrity segment 수 복사 */
	rq->nr_integrity_segments = rq_src->nr_integrity_segments;
	/* [한국어] phys_gap_bit: segment 간 물리 주소 비연속 여부 */
	rq->phys_gap_bit = rq_src->phys_gap_bit;

	/* [한국어] NVMe inline encryption: clone bio 에 crypto context 연결 */
	if (rq->bio && blk_crypto_rq_bio_prep(rq, rq->bio, gfp_mask) < 0)
		goto free_and_out;

	return 0;

free_and_out:
	/* [한국어] 실패: 부분 clone 된 bio 들 모두 해제 */
	blk_rq_unprep_clone(rq);

	return -ENOMEM;	/* [한국어] clone 준비에 실패한 유일한 이유는 메모리 부족이므로 -ENOMEM 고정 */
}
EXPORT_SYMBOL_GPL(blk_rq_prep_clone);
#endif /* CONFIG_BLK_MQ_STACKING */

/*
 * Steal bios from a request and add them to a bio list.
 * The request must not have been partially completed before.
 */
/*
 * [한국어]
 * blk_steal_bios - request 의 bio 들을 bio_list 로 빼앗아 재제출 준비
 *
 * @list: bio 들을 추가할 bio_list
 * @rq:   bio 를 빼앗길 request (부분 완료되지 않아야 함)
 *
 * dm-multipath 같은 스태킹 드라이버에서 I/O 경로 변경 시 사용.
 * rq 의 bio chain 을 list 로 이동하면서 새 queue 재제출에 맞게 플래그를 정리.
 * 재제출할 queue 가 frozen 일 수 있으므로 REQ_NOWAIT 를 반드시 제거해야 한다.
 */
void blk_steal_bios(struct bio_list *list, struct request *rq)
{
	struct bio *bio;

	for (bio = rq->bio; bio; bio = bio->bi_next) {	/* [한국어] bio 체인 전체를 순회하며 새 큐 재제출에 맞게 플래그를 정리한다 */
		if (bio->bi_opf & REQ_POLLED) {	/* [한국어] 폴링으로 제출되었던 bio 인가 */
			/* [한국어] REQ_POLLED 제거: 새 queue 의 poll 큐 매핑이 다를 수 있음.
			 * bi_cookie 도 초기화: 완료 poll 에 사용하는 식별자 */
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;	/* [한국어] 이전 큐에서 받은 폴링 쿠키를 무효화한다 — 새 큐에서는 의미가 없다 */
		}
		/*
		 * The alternate request queue that we may end up submitting
		 * the bio to may be frozen temporarily, in this case REQ_NOWAIT
		 * will fail the I/O immediately with EAGAIN to the issuer.
		 * We are not in the issuer context which cannot block. Clear
		 * the flag to avoid spurious EAGAIN I/O failures.
		 */
		/* [한국어] REQ_NOWAIT 제거: 재제출 큐 freeze 시 spurious EAGAIN 방지 */
		bio->bi_opf &= ~REQ_NOWAIT;
		/* [한국어] QoS 상태 초기화: 새 queue 의 iocost/iolatency 에서 다시 적용 */
		bio_clear_flag(bio, BIO_QOS_THROTTLED);
		bio_clear_flag(bio, BIO_QOS_MERGED);	/* [한국어] 병합 시 QoS 가 남긴 표시도 지운다 — 새 큐의 QoS 가 처음부터 다시 판단해야 한다 */
	}

	if (rq->bio) {	/* [한국어] 옮길 bio 가 실제로 있는가 */
		/* [한국어] rq->bio chain 을 bio_list 의 tail 에 연결 */
		if (list->tail)
			list->tail->bi_next = rq->bio;
		else	/* [한국어] bio_list 가 비어 있었다면 이 체인이 곧 head 가 된다 */
			list->head = rq->bio;	/* [한국어] bio_list 가 비어 있었다면 훔쳐 온 체인이 곧 head 다 */
		list->tail = rq->biotail;	/* [한국어] tail 은 언제나 훔쳐 온 체인의 마지막 bio 로 갱신된다 */

		/* [한국어] rq 의 bio 포인터 초기화: rq 는 이제 bio 없음 */
		rq->bio = NULL;
		rq->biotail = NULL;	/* [한국어] tail 포인터도 비워 request 가 bio 를 전혀 갖지 않은 상태로 만든다 */
	}

	rq->__data_len = 0;	/* [한국어] request 에 남은 전송 길이도 0 으로 — bio 를 모두 넘겼으므로 이 request 가 옮길 데이터는 없다 */
}
EXPORT_SYMBOL_GPL(blk_steal_bios);

/*
 * [한국어]
 * order_to_size - page order 를 바이트 크기로 변환
 *
 * @order: 페이지 order (2^order pages)
 * @return: 바이트 크기
 */
static size_t order_to_size(unsigned int order)
{
	/* [한국어] PAGE_SIZE << order = 2^order 페이지의 바이트 크기 */
	return (size_t)PAGE_SIZE << order;
}

/* called before freeing request pool in @tags */
/*
 * [한국어]
 * blk_mq_clear_rq_mapping - drv_tags->rqs[] 에서 해제될 page 범위의 매핑을 제거
 *
 * @drv_tags: 드라이버 tag set (rqs[] 역참조 테이블을 보유)
 * @tags:     해제될 tag pool (page_list 를 보유)
 *
 * tags->page_list 에 속한 page 의 주소 범위 내에 있는
 * drv_tags->rqs[i] 항목을 NULL 로 초기화한다.
 *
 * 왜 필요한가: 이제 곧 이 page 들을 __free_pages() 로 반납할 텐데,
 * drv_tags->rqs[] 안에 그 page 안쪽을 가리키는 포인터가 남아 있으면
 * 태그 순회자(blk_mq_queue_tag_busy_iter / blk_mq_tagset_busy_iter 가
 * tags->rqs[bitnr] 를 읽는 경로)가 해제된 메모리를 역참조하는 UAF 가 된다.
 * cmpxchg 를 쓰는 이유는 그 순회자가 동시에 같은 슬롯을 읽고 있을 수 있어
 * "내가 본 그 rq 였을 때만 NULL 로 바꾼다"는 원자적 교체가 필요하기 때문이다.
 * drv_tags == tags 이면 지금 해제 중인 풀 자신이므로 클리어할 이유가 없다.
 */
static void blk_mq_clear_rq_mapping(struct blk_mq_tags *drv_tags,
				    struct blk_mq_tags *tags)
{
	struct page *page;

	/*
	 * There is no need to clear mapping if driver tags is not initialized
	 * or the mapping belongs to the driver tags.
	 */
	/* [한국어] drv_tags 가 tags 와 동일하면 자기 자신을 클리어할 필요 없음 */
	if (!drv_tags || drv_tags == tags)
		return;

	list_for_each_entry(page, &tags->page_list, lru) {	/* [한국어] 이 tag pool 이 request 객체를 담아 둔 페이지들을 순회 */
		/* [한국어] 이 page 의 주소 범위 [start, end) */
		unsigned long start = (unsigned long)page_address(page);
		unsigned long end = start + order_to_size(page->private);	/* [한국어] page->private 에 저장해 둔 order 로 페이지 크기를 복원해 끝 주소를 구한다 */
		int i;	/* [한국어] drv_tags->rqs[] 순회 인덱스 */

		for (i = 0; i < drv_tags->nr_tags; i++) {	/* [한국어] 드라이버 태그 테이블 전체를 훑으며 이 페이지 범위를 가리키는 항목을 찾는다 */
			/* [한국어] drv_tags->rqs[i]: CID i 에 현재 할당된 request 포인터 */
			struct request *rq = drv_tags->rqs[i];
			unsigned long rq_addr = (unsigned long)rq;	/* [한국어] 포인터를 정수로 바꿔 페이지 범위 비교에 쓴다 */

			if (rq_addr >= start && rq_addr < end) {	/* [한국어] 이 request 포인터가 지금 해제하려는 페이지 안에 들어 있는가 */
				/* [한국어] 참조 카운트가 0 이어야 안전하게 매핑 해제 가능 */
				WARN_ON_ONCE(req_ref_read(rq) != 0);
				/* [한국어] cmpxchg: 완료 경로와의 race 없이 원자적으로 NULL 설정 */
				cmpxchg(&drv_tags->rqs[i], rq, NULL);
			}
		}
	}
}

/*
 * [한국어]
 * blk_mq_free_rqs - tag pool 의 request 객체들을 해제
 *
 * @set:      blk_mq_tag_set (NVMe 드라이버 전역 tag 설정)
 * @tags:     해제할 blk_mq_tags (특정 hctx 의 tag pool)
 * @hctx_idx: 해당 hctx 의 인덱스 (NVMe SQ 번호)
 *
 * page_list 가 비었으면 이미 해제된 것이므로 즉시 반환.
 * shared_tags 여부에 따라 drv_tags 를 선택한 후,
 * static_rqs[] 의 exit_request 콜백을 호출하고 매핑을 클리어.
 * 실제 page 해제는 SRCU callback 에서 수행.
 */
void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,
		     unsigned int hctx_idx)
{
	struct blk_mq_tags *drv_tags;

	/* [한국어] page_list 가 비어있으면 이미 해제된 tag pool — 중복 해제 방지 */
	if (list_empty(&tags->page_list))
		return;

	if (blk_mq_is_shared_tags(set->flags))	/* [한국어] tagset 전역으로 태그를 공유하는 구성인가 */
		/* [한국어] shared_tags: 여러 NVMe SQ 가 CID pool 을 공유하는 경우 */
		drv_tags = set->shared_tags;
	else	/* [한국어] hctx 마다 독립 태그 풀을 갖는 구성 */
		/* [한국어] 개별 tags: 각 NVMe SQ 가 독립적인 CID pool 보유 */
		drv_tags = set->tags[hctx_idx];

	if (tags->static_rqs && set->ops->exit_request) {	/* [한국어] request 객체 배열이 살아 있고 드라이버가 exit_request 를 등록했는가 — 드라이버가 request 뒤에 붙여 둔 자기 자원을 먼저 정리해야 한다 */
		int i;	/* [한국어] 태그 슬롯 순회 인덱스 */

		for (i = 0; i < tags->nr_tags; i++) {	/* [한국어] 이 풀의 모든 태그 슬롯을 순회 */
			/* [한국어] CID slot i 의 request 에 드라이버 cleanup 콜백 호출 */
			struct request *rq = tags->static_rqs[i];

			if (!rq)	/* [한국어] 이 슬롯에 request 가 배정되지 않았으면 건너뛴다 */
				continue;
			set->ops->exit_request(set, rq, hctx_idx);	/* [한국어] 드라이버 정리 콜백. NVMe PCIe 의 nvme_mq_ops 에는 exit_request 가 없어 이 경로가 실행되지 않는다 */
			tags->static_rqs[i] = NULL;	/* [한국어] 정리한 슬롯을 비워 이중 정리를 막는다 */
		}
	}

	/* [한국어] drv_tags->rqs[] 에서 이 tags 에 속한 매핑 항목들을 클리어 */
	blk_mq_clear_rq_mapping(drv_tags, tags);
	/*
	 * Free request pages in SRCU callback, which is called from
	 * blk_mq_free_tags().
	 */
	/* [한국어] page 실제 해제는 blk_mq_free_tags() 내 SRCU 콜백에서 수행 */
}

/*
 * [한국어]
 * blk_mq_free_rq_map - tags 의 rqs/static_rqs 배열과 sbitmap 해제
 *
 * @set:  blk_mq_tag_set
 * @tags: 해제할 blk_mq_tags
 *
 * blk_mq_free_rqs 호출 후 tag 자료구조 자체를 해제.
 */
void blk_mq_free_rq_map(struct blk_mq_tag_set *set, struct blk_mq_tags *tags)
{
	/* [한국어] rqs[]: CID별 request 역참조 테이블 해제 */
	kfree(tags->rqs);
	tags->rqs = NULL;	/* [한국어] 해제 후 포인터를 비워 use-after-free 를 막는다 */
	/* [한국어] static_rqs[]: CID별 request 객체 포인터 배열 해제 */
	kfree(tags->static_rqs);
	tags->static_rqs = NULL;	/* [한국어] 해제 후 포인터를 비운다 */

	/* [한국어] sbitmap + tags 구조체 자체 해제 */
	blk_mq_free_tags(set, tags);
}

/*
 * [한국어]
 * hctx_idx_to_type - hctx 인덱스로 hctx type 결정
 *
 * @set:      blk_mq_tag_set
 * @hctx_idx: 전역 hctx 인덱스
 * @return:   HCTX_TYPE_DEFAULT / READ / POLL 중 하나
 *
 * set->map[type].queue_offset ~ +nr_queues 범위로 어느 type 에 속하는지 판별.
 * NVMe 에서 READ/POLL 큐를 별도로 구성한 경우 해당 type 을 반환.
 */
static enum hctx_type hctx_idx_to_type(struct blk_mq_tag_set *set,
		unsigned int hctx_idx)
{
	int i;

	for (i = 0; i < set->nr_maps; i++) {	/* [한국어] DEFAULT/READ/POLL 맵을 차례로 보며 이 hctx 번호가 어느 맵 구간에 드는지 찾는다 */
		/* [한국어] map[i] 의 queue_offset..+nr_queues 범위에 속하면 type=i */
		unsigned int start = set->map[i].queue_offset;
		unsigned int end = start + set->map[i].nr_queues;	/* [한국어] 이 맵이 차지하는 hctx 번호 구간의 끝(배타적) */

		if (hctx_idx >= start && hctx_idx < end)	/* [한국어] 이 hctx 번호가 이 맵의 구간 안에 있으면 그 맵의 타입이 곧 답이다 */
			break;
	}

	/* [한국어] 매핑 실패: DEFAULT 로 fallback */
	if (i >= set->nr_maps)
		i = HCTX_TYPE_DEFAULT;

	return i;	/* [한국어] 찾은(또는 폴백된) 맵 인덱스 = hctx 타입 반환 */
}

/*
 * [한국어]
 * blk_mq_get_hctx_node - hctx 의 NUMA node 번호를 반환
 *
 * @set:      blk_mq_tag_set
 * @hctx_idx: hctx 인덱스
 * @return:   NUMA node 번호 (NUMA_NO_NODE 이면 set->numa_node 사용)
 *
 * NVMe SQ request 메모리를 NVMe 컨트롤러와 같은 NUMA node 에 배치하기 위해 사용.
 */
static int blk_mq_get_hctx_node(struct blk_mq_tag_set *set,
		unsigned int hctx_idx)
{
	enum hctx_type type = hctx_idx_to_type(set, hctx_idx);

	/* [한국어] blk_mq_hw_queue_to_node: map 의 queue_to_node[] 배열에서 조회 */
	return blk_mq_hw_queue_to_node(&set->map[type], hctx_idx);
}

/*
 * [한국어]
 * blk_mq_alloc_rq_map - 특정 hctx 를 위한 blk_mq_tags 구조체 할당
 *
 * @set:           blk_mq_tag_set
 * @hctx_idx:      대상 hctx 인덱스 (NVMe SQ 번호)
 * @nr_tags:       이 hctx 의 CID (Command ID) 슬롯 수
 * @reserved_tags: 예약 CID 수 (내부 admin 명령용)
 * @return:        blk_mq_tags 포인터, 실패 시 NULL
 *
 * sbitmap(CID 할당 비트맵) + rqs[]/static_rqs[] 배열을 NUMA-local 메모리에 할당.
 */
static struct blk_mq_tags *blk_mq_alloc_rq_map(struct blk_mq_tag_set *set,
					       unsigned int hctx_idx,
					       unsigned int nr_tags,
					       unsigned int reserved_tags)
{
	/* [한국어] NVMe SQ 메모리를 컨트롤러와 같은 NUMA node 에 배치 */
	int node = blk_mq_get_hctx_node(set, hctx_idx);
	struct blk_mq_tags *tags;	/* [한국어] sbitmap 과 두 request 배열을 담을 tags 구조체 */

	if (node == NUMA_NO_NODE)	/* [한국어] 호출자가 노드를 특정하지 않았으면 */
		node = set->numa_node;	/* [한국어] tag set 전체의 기본 노드를 쓴다 */

	/* [한국어] blk_mq_init_tags: sbitmap 초기화 — 각 bit 가 CID 하나를 나타냄 */
	tags = blk_mq_init_tags(nr_tags, reserved_tags, set->flags, node);
	if (!tags)	/* [한국어] sbitmap 초기화 실패 */
		return NULL;	/* [한국어] 아직 아무것도 할당하지 않았으므로 정리 없이 NULL 반환 */

	/* [한국어] ★ rqs[]와 static_rqs[] 두 배열이 왜 따로 있는가 ★ (코드로 확인한 사실)
	 * static_rqs[i] - 이 tags 풀의 태그 i에 "영구히" 배정된 request 객체 포인터.
	 *   blk_mq_alloc_rqs()가 페이지를 잡아 한 번 채워 두고, 풀이 해제될 때까지
	 *   바뀌지 않는다. 읽는 곳은 할당 경로다 —
	 *   blk_mq_rq_ctx_init()이 rq = tags->static_rqs[tag] 로 객체를 꺼낸다.
	 * rqs[i]        - 드라이버 태그 i를 "지금 하드웨어에서 쓰고 있는" request.
	 *   쓰는 곳은 단 하나, blk_mq_start_request() 의
	 *   rq->mq_hctx->tags->rqs[rq->tag] = rq 이다. 읽는 곳은 blk_mq_tag_to_rq()
	 *   와 태그 순회자(blk_mq_queue_tag_busy_iter / blk_mq_tagset_busy_iter)다.
	 *
	 * 왜 나뉘어 있나: I/O 스케줄러가 붙으면 태그가 두 벌이 된다. request 객체는
	 * 스케줄러 태그 풀(sched_tags)의 static_rqs[]에서 나오지만, 하드웨어에 실릴
	 * 때 받는 드라이버 태그는 전혀 다른 번호이고 다른 풀(hctx->tags)에 속한다.
	 * 즉 "드라이버 태그 i → 지금 그 슬롯을 쓰는 request" 는 static_rqs[] 로
	 * 표현할 수 없어서 rqs[] 가 따로 필요하다. flush request 처럼 static_rqs[]
	 * 에서 나오지 않는 request 가 드라이버 태그를 차지하는 경우도 마찬가지다.
	 *
	 * rqs[i] 는 완료 시에 지워지지 **않는다**. 지우는 곳은 해체 경로 두 군데뿐이며
	 * (blk_mq_clear_rq_mapping, blk_mq_clear_flush_rq_mapping), 둘 다 request
	 * 객체가 든 페이지를 free 하기 직전에 cmpxchg 로 NULL 을 넣는다. 목적은
	 * "늦은 완료 걸러내기"가 아니라, 태그 순회자가 이미 해제된 메모리를
	 * 역참조하는 UAF 를 막는 것이다.
	 *
	 * NVMe 접점: nvme_find_rq() → blk_mq_tag_to_rq(tags, tag) 가 바로 rqs[] 를
	 * 읽는 경로다. 태그 재사용으로 인한 "늦은 CQE" 문제는 blk-mq 가 아니라 NVMe
	 * 쪽이 CID 상위 4비트의 세대 카운터(genctr)로 걸러낸다.
	 *
	 * GFP 플래그 조합의 의미:
	 *   GFP_NOIO       - 이 할당이 I/O를 유발하면 안 된다(큐 초기화 중이라
	 *                    자기 자신을 기다리는 교착 위험).
	 *   __GFP_NOWARN   - 실패해도 커널 로그에 경고를 남기지 않는다. 아래
	 *                    호출자가 더 작은 크기로 재시도하는 경로가 있어,
	 *                    중간 실패는 정상적인 흐름이기 때문이다.
	 *   __GFP_NORETRY  - 회수를 반복하며 매달리지 않고 빨리 실패한다.
	 *                    같은 이유로 실패가 치명적이지 않다. */
	tags->rqs = kcalloc_node(nr_tags, sizeof(struct request *),
				 GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,
				 node);
	if (!tags->rqs)	/* [한국어] 드라이버 태그 역참조 배열 할당 실패 */
		goto err_free_tags;	/* [한국어] sbitmap 과 tags 구조체를 되돌리는 경로로 */

	tags->static_rqs = kcalloc_node(nr_tags, sizeof(struct request *),	/* [한국어] 태그별 request 객체 포인터 배열도 같은 정책으로 NUMA 지역 할당 */
					GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,	/* [한국어] rqs[] 와 동일한 GFP 조합 — I/O 를 유발하지 않고, 경고 없이, 빨리 실패한다 */
					node);	/* [한국어] 컨트롤러와 같은 NUMA 노드에 두어 제출·완료 경로의 원격 메모리 접근을 피한다 */
	if (!tags->static_rqs)	/* [한국어] static_rqs 배열 할당 실패 */
		goto err_free_rqs;	/* [한국어] 먼저 할당한 rqs[] 부터 되돌리는 경로로 */

	return tags;	/* [한국어] 두 배열과 sbitmap 이 모두 준비된 tags 반환 */

err_free_rqs:
	kfree(tags->rqs);	/* [한국어] 역순 해제 1단계 — rqs[] 반납 */
err_free_tags:
	blk_mq_free_tags(set, tags);	/* [한국어] 역순 해제 2단계 — sbitmap 과 tags 구조체 반납 */
	return NULL;	/* [한국어] 이 함수의 실패 원인은 메모리 부족뿐이므로 NULL 로만 알린다 */
}

/*
 * [한국어]
 * blk_mq_init_request - 개별 request 객체를 초기화
 *
 * @set:      blk_mq_tag_set
 * @rq:       초기화할 request
 * @hctx_idx: 이 request 가 속할 hctx 인덱스 (NVMe SQ 번호)
 * @node:     NUMA node
 * @return:   0 성공, 음수 에러
 *
 * 드라이버의 init_request 콜백(NVMe PCIe: nvme_pci_init_request) 을 호출해
 * NVMe 명령 payload 영역을 초기화한 뒤, request 상태를 MQ_RQ_IDLE 로 설정.
 */
static int blk_mq_init_request(struct blk_mq_tag_set *set, struct request *rq,
			       unsigned int hctx_idx, int node)
{
	int ret;

	/* [한국어] 드라이버가 request당 자기 데이터를 초기화할 기회를 준다.
	 * NVMe PCIe에서는 nvme_mq_ops.init_request == nvme_pci_init_request()가
	 * 호출되어, request 뒤에 붙은
	 * struct nvme_iod(blk_mq_rq_to_pdu로 접근)를 준비하고 해당 nvme_queue
	 * 포인터를 심는다. 이 초기화가 큐 생성 시점에 한 번만 이뤄지므로,
	 * I/O 핫패스에서는 이미 준비된 구조체를 쓰기만 하면 된다. */
	if (set->ops->init_request) {
		ret = set->ops->init_request(set, rq, hctx_idx, node);
		if (ret)	/* [한국어] 드라이버 초기화 실패(대개 -ENOMEM) */
			return ret;
	}

	/* [한국어] MQ_RQ_IDLE: sbitmap 에서 CID 를 재획득할 수 있는 완전 idle 상태 */
	WRITE_ONCE(rq->state, MQ_RQ_IDLE);
	return 0;
}

/*
 * [한국어]
 * blk_mq_alloc_rqs - tags 에 depth 개의 request 객체 메모리를 할당하고 초기화
 *
 * @set:      blk_mq_tag_set (cmd_size 정보 포함)
 * @tags:     할당할 tag pool (static_rqs[] 를 채울 대상)
 * @hctx_idx: hctx 인덱스 (NVMe SQ 번호)
 * @depth:    할당할 CID 슬롯 수 (= queue depth)
 * @return:   0 성공, -ENOMEM 실패
 *
 * request 객체와 드라이버 payload(NVMe submission queue entry) 를 한 덩어리로
 * page 단위로 할당하여 NUMA-local 메모리에 배치.
 * 각 request 는 rq_size 바이트(sizeof(struct request)+set->cmd_size, cacheline 정렬)
 * 를 차지하며, 가능한 큰 order(최대 4) 로 한 번에 할당해 단편화를 최소화.
 */
static int blk_mq_alloc_rqs(struct blk_mq_tag_set *set,
			    struct blk_mq_tags *tags,
			    unsigned int hctx_idx, unsigned int depth)
{
	unsigned int i, j, entries_per_page, max_order = 4;
	int node = blk_mq_get_hctx_node(set, hctx_idx);	/* [한국어] 이 hctx 가 속한 맵 타입으로부터 NUMA 노드를 정한다 — NVMe 라면 MSI-X affinity 를 따라간 노드가 된다 */
	size_t rq_size, left;	/* [한국어] rq_size = request 하나가 차지하는 바이트, left = 아직 할당해야 할 남은 바이트 */

	if (node == NUMA_NO_NODE)	/* [한국어] 노드가 정해지지 않았으면 */
		node = set->numa_node;	/* [한국어] tag set 전체의 기본 노드로 폴백 */

	/*
	 * rq_size is the size of the request plus driver payload, rounded
	 * to the cacheline size
	 */
	/* [한국어] rq_size: struct request + NVMe cmd payload, cacheline 정렬.
	 * false sharing 방지: 각 request 는 독립적인 cacheline 에 배치 */
	rq_size = round_up(sizeof(struct request) + set->cmd_size,
				cache_line_size());
	/* [한국어] left: 아직 할당해야 할 총 바이트 수 */
	left = rq_size * depth;

	for (i = 0; i < depth; ) {	/* [한국어] depth 개의 request 를 다 채울 때까지 페이지를 계속 할당한다. i 는 루프 본문 안에서 증가하므로 증감식이 비어 있다 */
		int this_order = max_order;	/* [한국어] 이번 페이지에 시도할 order — 실패하면 아래에서 하나씩 낮춘다 */
		struct page *page;	/* [한국어] 이번에 확보한 페이지 */
		int to_do;	/* [한국어] 이번 페이지에 실제로 배치할 request 개수 */
		void *p;	/* [한국어] 페이지 안에서 다음 request 가 놓일 위치를 가리키는 커서 */

		/* [한국어] left 가 담기기에 충분한 가장 작은 order 로 낮춤 */
		while (this_order && left < order_to_size(this_order - 1))
			this_order--;

		do {
			/* [한국어] GFP_NOIO: IO 대기 없이 할당 (IO 경로 진입 방지 데드락 방지).
			 * __GFP_NORETRY: 실패 시 order 를 낮춰 재시도.
			 * __GFP_ZERO: 초기화 보장 */
			page = alloc_pages_node(node,
				GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY | __GFP_ZERO,
				this_order);
			if (page)	/* [한국어] 할당 성공 — 재시도 루프 종료 */
				break;
			/* [한국어] 할당 실패: order 를 낮춰 재시도 */
			if (!this_order--)
				break;
			/* [한국어] order 낮춰도 request 하나도 못 담으면 포기 */
			if (order_to_size(this_order) < rq_size)
				break;
		} while (1);

		if (!page)	/* [한국어] order 를 끝까지 낮췄는데도 페이지를 못 얻었다 */
			goto fail;	/* [한국어] 지금까지 만든 것을 모두 되돌리는 실패 경로로 */

		/* [한국어] page->private 에 order 저장: 해제 시 order 복원에 사용 */
		page->private = this_order;
		/* [한국어] tags->page_list 에 등록: blk_mq_free_rqs 에서 page 해제에 사용 */
		list_add_tail(&page->lru, &tags->page_list);

		p = page_address(page);	/* [한국어] 페이지의 커널 가상 주소를 얻는다 — 이 주소부터 rq_size 간격으로 request 를 배치한다 */
		/*
		 * Allow kmemleak to scan these pages as they contain pointers
		 * to additional allocations like via ops->init_request().
		 */
		/* [한국어] kmemleak: 이 page 에 포인터가 있으므로 GC 추적 허용 */
		kmemleak_alloc(p, order_to_size(this_order), 1, GFP_NOIO);
		/* [한국어] 이 page 에 들어갈 request 수 계산 */
		entries_per_page = order_to_size(this_order) / rq_size;
		/* [한국어] 남은 depth 와 page 수용량 중 작은 것만 처리 */
		to_do = min(entries_per_page, depth - i);
		left -= to_do * rq_size;	/* [한국어] 이번 페이지가 담을 몫만큼 남은 바이트에서 뺀다 — 다음 반복의 order 결정에 쓰인다 */
		for (j = 0; j < to_do; j++) {	/* [한국어] 이 페이지 안에 to_do 개의 request 를 차례로 배치 */
			struct request *rq = p;	/* [한국어] 현재 커서 위치를 request 객체로 본다 — 페이지를 rq_size 격자로 잘라 쓰는 방식이다 */

			/* [한국어] CID slot i 에 request 객체 배정 */
			tags->static_rqs[i] = rq;
			/* [한국어] 드라이버 init_request: NVMe cmd 영역 초기화 */
			if (blk_mq_init_request(set, rq, hctx_idx, node)) {
				tags->static_rqs[i] = NULL;
				goto fail;	/* [한국어] 드라이버 초기화 실패 — 지금까지 만든 것을 전부 되돌린다 */
			}

			/* [한국어] 다음 request 객체로 포인터 이동 */
			p += rq_size;
			i++;	/* [한국어] 전역 태그 인덱스 증가 — 바깥 루프의 종료 조건이다 */
		}
	}
	return 0;

fail:
	/* [한국어] 실패: 지금까지 할당된 request 들 모두 해제 */
	blk_mq_free_rqs(set, tags, hctx_idx);
	return -ENOMEM;	/* [한국어] 이 함수의 실패 원인은 메모리 부족뿐이다 */
}

struct rq_iter_data {	/* [한국어] blk_mq_all_tag_iter 콜백에 넘길 검색 상태. "이 hctx 에 아직 살아 있는 request 가 있는가"만 알아내면 된다 */
	/* [한국어] hctx: 검사 대상 NVMe SQ — 이 hctx 에 속한 request 가 있는지 조사
	 * 설정자: blk_mq_hctx_has_requests 에서 초기화
	 * 읽는 자: blk_mq_has_request 가 rq->mq_hctx 와 비교
	 * 값 범위: NULL 불가 (유효한 hctx 포인터)
	 * 동기화: tags_srcu read lock 보호 하에서만 접근 */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] has_rq: 이 hctx 에 아직 미완료 request 가 하나라도 있으면 true
	 * 설정자: blk_mq_has_request 가 해당 hctx 의 request 를 발견하면 true 로 설정
	 * 읽는 자: blk_mq_hctx_has_requests 가 반환 전에 읽음
	 * 초기값: false (blk_mq_hctx_has_requests 에서 초기화)
	 * 동기화: 단일 스레드 접근 (CPU offline 핫플러그 경로) */
	bool has_rq;
};

/*
 * [한국어]
 * blk_mq_has_request - tag 순회 콜백: 이 hctx 에 속한 request 발견 시 순회 중단
 *
 * @rq:   현재 순회 중인 request
 * @data: struct rq_iter_data 포인터
 * @return: true=계속, false=중단
 *
 * blk_mq_all_tag_iter 의 콜백으로 호출.
 * rq->mq_hctx 가 iter_data->hctx 와 같으면 has_rq=true 로 설정하고 순회 중단.
 */
static bool blk_mq_has_request(struct request *rq, void *data)
{
	struct rq_iter_data *iter_data = data;

	/* [한국어] 이 CID 가 다른 hctx(NVMe SQ)에 속하면 계속 순회 */
	if (rq->mq_hctx != iter_data->hctx)
		return true;
	/* [한국어] 이 hctx 의 미완료 request 발견: 순회 중단 */
	iter_data->has_rq = true;
	return false;
}

/*
 * [한국어]
 * blk_mq_hctx_has_requests - 이 hctx 에 아직 미완료 request 가 있는지 확인
 *
 * @hctx: 검사할 NVMe SQ
 * @return: true = 미완료 request 있음, false = 없음
 *
 * CPU offline 핫플러그 시 이 SQ 를 비활성화하기 전에 모든 in-flight request 가
 * 완료될 때까지 대기하기 위해 호출.
 * sched_tags 가 있으면 scheduler 레이어의 tag 도 검사.
 * tags_srcu 로 hctx/tags 교체 race 를 방지.
 */
static bool blk_mq_hctx_has_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] sched_tags 우선: IO scheduler 가 보유한 request 도 포함 */
	struct blk_mq_tags *tags = hctx->sched_tags ?
			hctx->sched_tags : hctx->tags;
	struct rq_iter_data data = {	/* [한국어] 순회 콜백에 넘길 상태 초기화 */
		.hctx	= hctx,	/* [한국어] 이 hctx 에 속한 request 만 찾도록 기준을 심는다 */
	};
	int srcu_idx;	/* [한국어] srcu_read_lock 이 돌려주는 인덱스 — unlock 에 그대로 넘겨야 한다 */

	/* [한국어] tags_srcu read lock: blk_mq_update_nr_hw_queues 등 구조 변경 방지 */
	srcu_idx = srcu_read_lock(&hctx->queue->tag_set->tags_srcu);
	/* [한국어] 전체 CID slot 을 순회하며 이 hctx 의 미완료 request 검색 */
	blk_mq_all_tag_iter(tags, blk_mq_has_request, &data);
	srcu_read_unlock(&hctx->queue->tag_set->tags_srcu, srcu_idx);	/* [한국어] 순회가 끝났으므로 SRCU 읽기 구간 해제. 이 시점 이후 tag 구조가 재구성될 수 있다 */

	return data.has_rq;	/* [한국어] true 면 아직 이 hctx 에서 처리 중인 request 가 있다는 뜻 */
}

/*
 * [한국어]
 * blk_mq_hctx_has_online_cpu - 이 hctx 에 online 상태의 CPU 가 있는지 확인
 *
 * @hctx:     검사할 hctx
 * @this_cpu: 현재 offline 되는 CPU (제외 대상)
 * @return:   true = this_cpu 외의 online CPU 가 매핑돼 있음
 *
 * cpumask 는 isolated CPU 를 제외하므로 queue map 을 직접 조회해야 함.
 * this_cpu 가 마지막 online CPU 라면 false 반환 → hctx 비활성화 필요.
 */
static bool blk_mq_hctx_has_online_cpu(struct blk_mq_hw_ctx *hctx,
		unsigned int this_cpu)
{
	enum hctx_type type = hctx->type;
	int cpu;	/* [한국어] for_each_online_cpu 순회 변수 */

	/*
	 * hctx->cpumask has to rule out isolated CPUs, but userspace still
	 * might submit IOs on these isolated CPUs, so use the queue map to
	 * check if all CPUs mapped to this hctx are offline
	 */
	for_each_online_cpu(cpu) {	/* [한국어] 온라인 CPU 를 모두 훑는다. hctx->cpumask 대신 queue map 을 쓰는 이유는 위 영문 주석대로 isolated CPU 가 cpumask 에서 빠지기 때문이다 */
		/* [한국어] 각 online CPU 가 이 hctx 에 매핑되는지 queue map 에서 조회 */
		struct blk_mq_hw_ctx *h = blk_mq_map_queue_type(hctx->queue,
				type, cpu);

		if (h != hctx)	/* [한국어] 이 CPU 는 다른 hctx 로 매핑된다 — 관심 대상이 아니다 */
			continue;

		/* this hctx has at least one online CPU */
		/* [한국어] this_cpu 가 아닌 다른 online CPU 가 이 hctx 를 사용 → 유지 */
		if (this_cpu != cpu)
			return true;
	}

	return false;
}

/*
 * [한국어]
 * blk_mq_hctx_notify_offline - CPU offline 시 해당 hctx 비활성화 처리
 *
 * @cpu:  offline 되는 CPU 번호
 * @node: cpuhp_online hlist node (hctx 포인터로 복원)
 * @return: 0 성공, -EBUSY (PM wakeup 으로 인한 조기 종료)
 *
 * 이 CPU 가 마지막 online CPU 이고 nr_ctx > 0 이면 hctx 를 INACTIVE 로 표시.
 * 이후 모든 미완료 request 가 drain 되기를 기다린다 (5ms polling).
 * suspend 중 IRQ 없어 drain 이 불가능하면 -EBUSY 로 조기 종료.
 */
static int blk_mq_hctx_notify_offline(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx = hlist_entry_safe(node,
			struct blk_mq_hw_ctx, cpuhp_online);	/* [한국어] cpuhp 콜백은 hlist 노드를 주므로 container_of 로 hctx 를 복원한다 */
	int ret = 0;	/* [한국어] 0 = 오프라인 전환 허용, -EBUSY = 지금은 불가 */

	/* [한국어] ctx 없거나 다른 online CPU 가 있으면 비활성화 불필요 */
	if (!hctx->nr_ctx || blk_mq_hctx_has_online_cpu(hctx, cpu))
		return 0;

	/*
	 * Prevent new request from being allocated on the current hctx.
	 *
	 * The smp_mb__after_atomic() Pairs with the implied barrier in
	 * test_and_set_bit_lock in sbitmap_get().  Ensures the inactive flag is
	 * seen once we return from the tag allocator.
	 */
	/* [한국어] BLK_MQ_S_INACTIVE: 이 hctx 에서 새 CID 할당 차단 */
	set_bit(BLK_MQ_S_INACTIVE, &hctx->state);
	/* [한국어] smp_mb__after_atomic: INACTIVE 비트가 sbitmap_get 의 lock bit 보다
	 * 먼저 보이도록 순서 보장 — tag allocator 가 INACTIVE 를 놓치면 안 됨 */
	smp_mb__after_atomic();

	/*
	 * Try to grab a reference to the queue and wait for any outstanding
	 * requests.  If we could not grab a reference the queue has been
	 * frozen and there are no requests.
	 */
	if (percpu_ref_tryget(&hctx->queue->q_usage_counter)) {	/* [한국어] 큐 참조를 잡을 수 있는가 — 실패하면 큐가 이미 freeze 되었다는 뜻이고, 그렇다면 기다릴 in-flight 도 없다 */
		/* [한국어] q_usage_counter 획득 성공: in-flight request 가 drain 될 때까지 대기 */
		while (blk_mq_hctx_has_requests(hctx)) {
			/*
			 * The wakeup capable IRQ handler of block device is
			 * not called during suspend. Skip the loop by checking
			 * pm_wakeup_pending to prevent the deadlock and improve
			 * suspend latency.
			 */
			/* [한국어] suspend 중 IRQ 없으면 pm_wakeup_pending 으로 탈출 */
			if (pm_wakeup_pending()) {
				clear_bit(BLK_MQ_S_INACTIVE, &hctx->state);
				ret = -EBUSY;	/* [한국어] 오프라인 전환을 거부한다 — suspend 중이라 완료 인터럽트가 오지 않아 무한 대기가 될 수 있다 */
				break;
			}
			/* [한국어] 5ms 대기 후 재확인: in-flight request drain 대기 */
			msleep(5);
		}
		percpu_ref_put(&hctx->queue->q_usage_counter);	/* [한국어] drain 대기가 끝났으므로 큐 참조 반납 */
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
/*
 * [한국어]
 * blk_mq_cpu_mapped_to_hctx - 주어진 CPU 가 이 hctx 에 매핑되는지 확인
 *
 * @cpu:  확인할 CPU 번호
 * @hctx: 확인할 hctx
 * @return: true = 매핑됨, false = 아님
 *
 * cpumask 는 isolated CPU 를 제외하므로 queue map 을 직접 조회해야 함.
 */
static bool blk_mq_cpu_mapped_to_hctx(unsigned int cpu,
		const struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_hw_ctx *mapped_hctx = blk_mq_map_queue_type(hctx->queue,
			hctx->type, cpu);	/* [한국어] hctx 타입까지 함께 넘겨야 DEFAULT/READ/POLL 중 올바른 맵에서 조회된다 */

	return mapped_hctx == hctx;	/* [한국어] 조회 결과가 이 hctx 자신이면 그 CPU 는 이 hctx 를 쓴다는 뜻 */
}

/*
 * [한국어]
 * blk_mq_hctx_notify_online - CPU online 시 해당 hctx 를 다시 활성화
 *
 * @cpu:  online 된 CPU 번호
 * @node: cpuhp_online hlist node
 * @return: 항상 0
 *
 * CPU 가 online 되고 이 hctx 에 매핑되면 BLK_MQ_S_INACTIVE 비트를 클리어.
 * 이후 새 CID 할당이 다시 허용된다.
 */
static int blk_mq_hctx_notify_online(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx = hlist_entry_safe(node,
			struct blk_mq_hw_ctx, cpuhp_online);	/* [한국어] cpuhp 콜백은 hlist 노드를 주므로 container_of 로 hctx 복원 */

	/* [한국어] 이 CPU 가 이 hctx 에 매핑되면 INACTIVE 해제 → CID 할당 재허용 */
	if (blk_mq_cpu_mapped_to_hctx(cpu, hctx))
		clear_bit(BLK_MQ_S_INACTIVE, &hctx->state);
	return 0;
}

/*
 * 'cpu' is going away. splice any existing rq_list entries from this
 * software queue to the hw queue dispatch list, and ensure that it
 * gets run.
 */
/*
 * [한국어]
 * blk_mq_hctx_notify_dead - CPU dead 시 sw queue 의 잔여 request 를 hctx->dispatch 로 이관
 *
 * @cpu:  dead 된 CPU 번호
 * @node: cpuhp_dead hlist node
 * @return: 항상 0
 *
 * CPU 가 완전히 죽으면 그 CPU 의 per-CPU sw queue(ctx->rq_lists[type]) 에 남아 있는
 * request 들을 처리하는 CPU 가 없어진다. 이를 hctx->dispatch 로 옮겨
 * 다른 CPU 에서 kblockd 가 처리하도록 한다.
 * ctx->lock → hctx->lock 순서로 획득 (데드락 방지).
 */
static int blk_mq_hctx_notify_dead(unsigned int cpu, struct hlist_node *node)
{
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;	/* [한국어] 죽는 CPU 의 per-CPU 소프트웨어 큐 */
	LIST_HEAD(tmp);	/* [한국어] 락 보유 시간을 줄이기 위해 request 들을 한 번에 옮겨 담을 임시 리스트 */
	enum hctx_type type;	/* [한국어] rq_lists 인덱스로 쓸 hctx 타입 */

	hctx = hlist_entry_safe(node, struct blk_mq_hw_ctx, cpuhp_dead);	/* [한국어] cpuhp 콜백의 hlist 노드에서 hctx 복원 */
	/* [한국어] 이 CPU 가 이 hctx 에 매핑되지 않으면 처리 불필요 */
	if (!blk_mq_cpu_mapped_to_hctx(cpu, hctx))
		return 0;

	/* [한국어] 죽은 CPU 의 per-CPU sw queue 가져오기 */
	ctx = __blk_mq_get_ctx(hctx->queue, cpu);
	type = hctx->type;	/* [한국어] rq_lists 는 타입별로 분리되어 있으므로 인덱스를 먼저 확보 */

	spin_lock(&ctx->lock);	/* [한국어] sw 큐 리스트와 ctx_map 비트를 함께 보호하는 락 */
	if (!list_empty(&ctx->rq_lists[type])) {	/* [한국어] 죽는 CPU 의 sw 큐에 아직 처리 못 한 request 가 남아 있는가 */
		/* [한국어] sw queue 의 request 들을 tmp 로 이동 (locking 최소화) */
		list_splice_init(&ctx->rq_lists[type], &tmp);
		/* [한국어] ctx_map 의 pending 비트 클리어: 이 ctx 에 request 없음을 표시 */
		blk_mq_hctx_clear_pending(hctx, ctx);
	}
	spin_unlock(&ctx->lock);	/* [한국어] sw 큐 조작이 끝났으므로 락 해제 */

	if (list_empty(&tmp))	/* [한국어] 옮길 것이 없으면 여기서 끝 */
		return 0;

	spin_lock(&hctx->lock);	/* [한국어] dispatch 리스트를 보호하는 hctx 락 */
	/* [한국어] tmp 의 request 들을 hctx->dispatch 로 이관: 다른 CPU 에서 처리 */
	list_splice_tail_init(&tmp, &hctx->dispatch);
	spin_unlock(&hctx->lock);	/* [한국어] dispatch 리스트 이관이 끝났으므로 락 해제 */

	/* [한국어] async(true) 로 hctx run: kblockd 에서 이관된 request 처리 */
	blk_mq_run_hw_queue(hctx, true);
	return 0;
}

/*
 * [한국어]
 * __blk_mq_remove_cpuhp - hctx 의 CPU hotplug 콜백 등록 해제 (잠금 보유 상태에서)
 *
 * @hctx: 대상 hctx
 *
 * blk_mq_cpuhp_lock 보유 상태에서 호출. 스태킹 드라이버는 online 콜백 불필요.
 */
static void __blk_mq_remove_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&blk_mq_cpuhp_lock);

	if (!(hctx->flags & BLK_MQ_F_STACKING) &&	/* [한국어] dm/md 처럼 스택 위에 올라간 큐가 아니고 */
	    !hlist_unhashed(&hctx->cpuhp_online)) {	/* [한국어] online 콜백이 실제로 등록되어 있으면 — hlist_unhashed 가 false 라는 것이 곧 등록되어 있다는 표시다 */
		/* [한국어] CPUHP_AP_BLK_MQ_ONLINE: CPU online 시 INACTIVE 클리어 콜백 제거 */
		cpuhp_state_remove_instance_nocalls(CPUHP_AP_BLK_MQ_ONLINE,
						    &hctx->cpuhp_online);
		INIT_HLIST_NODE(&hctx->cpuhp_online);	/* [한국어] 노드를 다시 unhashed 상태로 초기화해 두 번 해제하지 않게 한다 */
	}

	if (!hlist_unhashed(&hctx->cpuhp_dead)) {	/* [한국어] dead 콜백이 등록되어 있는가 */
		/* [한국어] CPUHP_BLK_MQ_DEAD: CPU dead 시 sw queue drain 콜백 제거 */
		cpuhp_state_remove_instance_nocalls(CPUHP_BLK_MQ_DEAD,
						    &hctx->cpuhp_dead);
		INIT_HLIST_NODE(&hctx->cpuhp_dead);	/* [한국어] 노드를 unhashed 로 되돌려 이중 해제를 막는다 */
	}
}

/*
 * [한국어]
 * blk_mq_remove_cpuhp - hctx 의 CPU hotplug 콜백 등록 해제 (잠금 획득 포함)
 */
static void blk_mq_remove_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	mutex_lock(&blk_mq_cpuhp_lock);
	__blk_mq_remove_cpuhp(hctx);	/* [한국어] 실제 해제는 잠금 없는 내부 버전이 수행한다 */
	mutex_unlock(&blk_mq_cpuhp_lock);	/* [한국어] 해제가 끝났으므로 전역 cpuhp 뮤텍스 해제 */
}

/*
 * [한국어]
 * __blk_mq_add_cpuhp - hctx 의 CPU hotplug 콜백 등록 (잠금 보유 상태에서)
 *
 * @hctx: 대상 hctx
 *
 * CPUHP_AP_BLK_MQ_ONLINE(online 콜백) 과 CPUHP_BLK_MQ_DEAD(dead 콜백) 를 등록.
 * 스태킹 드라이버는 online 콜백 불필요 (BLK_MQ_F_STACKING 체크).
 */
static void __blk_mq_add_cpuhp(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&blk_mq_cpuhp_lock);

	if (!(hctx->flags & BLK_MQ_F_STACKING) &&	/* [한국어] 스택 드라이버가 아니고 */
	    hlist_unhashed(&hctx->cpuhp_online))	/* [한국어] 아직 online 콜백이 등록되지 않았으면(unhashed 가 true) */
		/* [한국어] CPU online 시 BLK_MQ_S_INACTIVE 클리어 콜백 등록 */
		cpuhp_state_add_instance_nocalls(CPUHP_AP_BLK_MQ_ONLINE,
				&hctx->cpuhp_online);

	if (hlist_unhashed(&hctx->cpuhp_dead))	/* [한국어] dead 콜백이 아직 등록되지 않았으면 */
		/* [한국어] CPU dead 시 sw queue drain 콜백 등록 */
		cpuhp_state_add_instance_nocalls(CPUHP_BLK_MQ_DEAD,
				&hctx->cpuhp_dead);
}

/*
 * [한국어]
 * __blk_mq_remove_cpuhp_list - hctx_list 의 모든 hctx 에서 cpuhp 콜백 해제
 */
static void __blk_mq_remove_cpuhp_list(struct list_head *head)
{
	struct blk_mq_hw_ctx *hctx;

	lockdep_assert_held(&blk_mq_cpuhp_lock);	/* [한국어] 호출자가 전역 cpuhp 뮤텍스를 쥐고 있음을 lockdep 으로 강제 — 등록/해제가 서로 엇갈리면 hlist 가 깨진다 */

	list_for_each_entry(hctx, head, hctx_list)	/* [한국어] 리스트의 모든 hctx 를 순회 */
		__blk_mq_remove_cpuhp(hctx);	/* [한국어] 각각의 cpuhp 콜백을 해제 */
}

/*
 * Unregister cpuhp callbacks from exited hw queues
 *
 * Safe to call if this `request_queue` is live
 */
/*
 * [한국어]
 * blk_mq_remove_hw_queues_cpuhp - unused_hctx_list 의 모든 hctx cpuhp 콜백 해제
 *
 * @q: request_queue
 *
 * queue 가 live 상태여도 안전하게 호출 가능.
 * unused_hctx_list 를 임시로 꺼내 잠금 없이 처리 후 되돌린다.
 */
static void blk_mq_remove_hw_queues_cpuhp(struct request_queue *q)
{
	LIST_HEAD(hctx_list);

	/* [한국어] unused_hctx_list 를 임시 hctx_list 로 이동 */
	spin_lock(&q->unused_hctx_lock);
	list_splice_init(&q->unused_hctx_list, &hctx_list);	/* [한국어] unused 리스트를 통째로 로컬로 옮긴다 — 아래 뮤텍스 구간에서 스핀락을 쥐고 있지 않기 위함이다 */
	spin_unlock(&q->unused_hctx_lock);	/* [한국어] 리스트 이동이 끝났으므로 스핀락 해제 */

	mutex_lock(&blk_mq_cpuhp_lock);	/* [한국어] cpuhp 등록/해제를 직렬화하는 전역 뮤텍스(잠들 수 있으므로 스핀락 밖에서 잡는다) */
	__blk_mq_remove_cpuhp_list(&hctx_list);	/* [한국어] 로컬 리스트의 모든 hctx 에서 콜백 해제 */
	mutex_unlock(&blk_mq_cpuhp_lock);	/* [한국어] 뮤텍스 해제 */

	/* [한국어] 처리 완료 후 unused_hctx_list 로 복원 */
	spin_lock(&q->unused_hctx_lock);
	list_splice(&hctx_list, &q->unused_hctx_list);	/* [한국어] 원래 자리로 되돌린다 — 이 hctx 들은 여전히 재사용 대기 상태다 */
	spin_unlock(&q->unused_hctx_lock);	/* [한국어] 복원이 끝났으므로 스핀락 해제 */
}

/*
 * Register cpuhp callbacks from all hw queues
 *
 * Safe to call if this `request_queue` is live
 */
/*
 * [한국어]
 * blk_mq_add_hw_queues_cpuhp - 모든 hctx 에 cpuhp 콜백 등록
 *
 * @q: request_queue
 *
 * blk_mq_init_allocated_queue 등 queue 초기화 시 호출.
 * 이후 CPU hotplug 이벤트에 반응할 수 있게 된다.
 */
static void blk_mq_add_hw_queues_cpuhp(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	mutex_lock(&blk_mq_cpuhp_lock);	/* [한국어] 등록/해제 직렬화를 위한 전역 뮤텍스 */
	queue_for_each_hw_ctx(q, hctx, i)	/* [한국어] 이 큐의 모든 하드웨어 큐를 순회 */
		__blk_mq_add_cpuhp(hctx);	/* [한국어] 각각에 cpuhp 콜백 등록 */
	mutex_unlock(&blk_mq_cpuhp_lock);	/* [한국어] cpuhp 등록이 끝났으므로 전역 뮤텍스 해제 */
}

/*
 * Before freeing hw queue, clearing the flush request reference in
 * tags->rqs[] for avoiding potential UAF.
 */
/*
 * [한국어]
 * blk_mq_clear_flush_rq_mapping - hctx 해제 전 flush_rq 의 tags->rqs[] 매핑 제거
 *
 * @tags:        대상 tag pool
 * @queue_depth: tag slot 수
 * @flush_rq:    제거할 flush request
 *
 * flush request 는 static_rqs[] 에서 나오지 않고 hctx->fq(flush queue)에
 * 따로 할당되지만, 발행될 때는 일반 request 처럼 드라이버 태그를 하나 잡고
 * blk_mq_start_request() 에서 tags->rqs[tag] 에 자기 포인터를 남긴다.
 * hctx 를 해체하면서 flush_rq 메모리를 반납하기 전에 그 흔적을 지우지 않으면,
 * 태그 순회자가 해제된 flush_rq 를 역참조하는 UAF 가 된다(위 영문 주석).
 * 어느 슬롯에 남아 있는지 모르므로 queue_depth 전체를 훑으며
 * "값이 flush_rq 인 슬롯만" cmpxchg 로 NULL 로 바꾼다.
 */
static void blk_mq_clear_flush_rq_mapping(struct blk_mq_tags *tags,
		unsigned int queue_depth, struct request *flush_rq)
{
	int i;

	/* The hw queue may not be mapped yet */
	/* [한국어] tags == NULL: 이 hctx 의 tag pool 이 아직 할당되지 않은 경우 */
	if (!tags)
		return;

	/* [한국어] 해제 전 flush_rq 참조 카운트가 0 이어야 안전 */
	WARN_ON_ONCE(req_ref_read(flush_rq) != 0);

	/* [한국어] 전체 tag slot 에서 flush_rq 에 대한 매핑을 원자적으로 NULL 로 교체 */
	for (i = 0; i < queue_depth; i++)
		cmpxchg(&tags->rqs[i], flush_rq, NULL);
}

/*
 * [한국어]
 * blk_free_flush_queue_callback - SRCU 콜백: flush queue 메모리 해제
 *
 * @head: rcu_head (blk_flush_queue 내 임베딩)
 *
 * blk_mq_exit_hctx 에서 call_srcu 로 등록된 콜백.
 * SRCU 임계 구간이 모두 종료된 후 호출되어 안전하게 flush queue 를 해제.
 */
static void blk_free_flush_queue_callback(struct rcu_head *head)
{
	struct blk_flush_queue *fq =
		container_of(head, struct blk_flush_queue, rcu_head);	/* [한국어] rcu_head 를 품고 있는 blk_flush_queue 를 복원 — call_srcu 는 rcu_head 포인터만 넘겨 주기 때문이다 */

	blk_free_flush_queue(fq);	/* [한국어] 이제 이 flush queue 를 보던 SRCU 독자가 모두 빠져나갔으므로 안전하게 해제 */
}

/* hctx->ctxs will be freed in queue's release handler */
/*
 * [한국어]
 * blk_mq_exit_hctx - 하나의 hctx (NVMe SQ) 를 종료 및 정리
 *
 * @q:        request_queue
 * @set:      blk_mq_tag_set
 * @hctx:     종료할 hctx
 * @hctx_idx: hctx 인덱스 (NVMe SQ 번호)
 *
 * tag_idle → flush_rq 매핑 해제 → exit_request/exit_hctx 콜백 →
 * flush_queue SRCU 해제 → unused_hctx_list 이동 순으로 처리.
 * flush queue 는 SRCU 를 통해 모든 reader 가 빠져나간 후 해제.
 */
static void blk_mq_exit_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct request *flush_rq = hctx->fq->flush_rq;

	/* [한국어] sw queue 가 매핑돼 있으면 tag idle 처리 (CID 해제 시도) */
	if (blk_mq_hw_queue_mapped(hctx))
		blk_mq_tag_idle(hctx);

	/* [한국어] queue 초기화 완료 후 진입한 경우에만 flush_rq 매핑 클리어 */
	if (blk_queue_init_done(q))
		blk_mq_clear_flush_rq_mapping(set->tags[hctx_idx],
				set->queue_depth, flush_rq);
	/* [한국어] 드라이버 exit_request: NVMe flush_rq 의 cmd payload 해제 */
	if (set->ops->exit_request)
		set->ops->exit_request(set, flush_rq, hctx_idx);

	/* [한국어] 드라이버 exit_hctx: NVMe SQ/CQ 드라이버 레벨 정리 */
	if (set->ops->exit_hctx)
		set->ops->exit_hctx(hctx, hctx_idx);

	/* [한국어] flush_queue 를 SRCU 콜백으로 해제: 모든 reader 퇴장 후 안전 해제 */
	call_srcu(&set->tags_srcu, &hctx->fq->rcu_head,
			blk_free_flush_queue_callback);
	hctx->fq = NULL;	/* [한국어] 포인터를 비워 둔다 — 실제 해제는 위 SRCU 콜백이 나중에 수행하므로, 여기서 남겨 두면 이중 해제 위험이 있다 */

	/* [한국어] hctx 를 unused_hctx_list 로 이동: 메모리는 queue release 시 해제 */
	spin_lock(&q->unused_hctx_lock);
	list_add(&hctx->hctx_list, &q->unused_hctx_list);	/* [한국어] hctx 구조체 자체는 여기서 free 하지 않고 재사용 풀에 넣는다. 큐 개수가 다시 늘 때 그대로 되살려 쓴다 */
	spin_unlock(&q->unused_hctx_lock);	/* [한국어] 리스트 삽입이 끝났으므로 락 해제 */
}

/*
 * [한국어]
 * blk_mq_exit_hw_queues - 첫 nr_queue 개의 hctx 를 종료
 *
 * @q:        request_queue
 * @set:      blk_mq_tag_set
 * @nr_queue: 종료할 hctx 수
 *
 * 부분 초기화 실패 시 이미 초기화된 hctx 들만 정리하기 위해 nr_queue 로 상한 지정.
 */
static void blk_mq_exit_hw_queues(struct request_queue *q,
		struct blk_mq_tag_set *set, int nr_queue)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	queue_for_each_hw_ctx(q, hctx, i) {	/* [한국어] 앞에서부터 hctx 를 순회 */
		if (i == nr_queue)	/* [한국어] 요청받은 개수에 도달했으면 중단 — 부분 초기화 실패 시 초기화된 만큼만 되돌리기 위한 장치다 */
			break;
		/* [한국어] cpuhp 콜백 해제 후 hctx 종료 */
		blk_mq_remove_cpuhp(hctx);
		blk_mq_exit_hctx(q, set, hctx, i);	/* [한국어] cpuhp 해제가 끝난 뒤에야 hctx 를 종료한다(순서가 반대면 콜백이 해체 중인 hctx 를 건드릴 수 있다) */
	}
}

/*
 * [한국어]
 * blk_mq_init_hctx - 하나의 hctx (NVMe SQ) 를 초기화
 *
 * @q:        request_queue
 * @set:      blk_mq_tag_set
 * @hctx:     초기화할 hctx
 * @hctx_idx: hctx 인덱스 (NVMe SQ 번호)
 * @return:   0 성공, -1 실패
 *
 * flush queue 할당 → queue_num/tags 설정 → 드라이버 init_hctx 콜백 →
 * flush_rq 초기화 순으로 진행. 실패 시 역순으로 정리.
 */
static int blk_mq_init_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned hctx_idx)
{
	gfp_t gfp = GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY;

	/* [한국어] flush queue: PRE_FLUSH/POST_FLUSH 명령용 flush_rq 포함 */
	hctx->fq = blk_alloc_flush_queue(hctx->numa_node, set->cmd_size, gfp);
	if (!hctx->fq)	/* [한국어] flush queue 할당 실패 */
		goto fail;	/* [한국어] 아직 아무것도 잡지 않았으므로 그대로 실패 반환 */

	/* [한국어] queue_num = hctx 인덱스 = NVMe SQ 번호 */
	hctx->queue_num = hctx_idx;

	/* [한국어] 이 hctx 의 CID pool 연결 */
	hctx->tags = set->tags[hctx_idx];

	if (set->ops->init_hctx &&	/* [한국어] 드라이버가 init_hctx 를 등록했고 그 호출이 실패하면 — NVMe PCIe 는 nvme_init_hctx 를 등록하며, 거기서 hctx->driver_data 에 nvme_queue 포인터가 심긴다 */
	    /* [한국어] nvme_init_hctx: nvme_queue 를 hctx 에 바인딩 */
	    set->ops->init_hctx(hctx, set->driver_data, hctx_idx))
		goto fail_free_fq;

	/* [한국어] flush_rq 초기화: 드라이버 init_request 콜백으로 NVMe cmd payload 준비 */
	if (blk_mq_init_request(set, hctx->fq->flush_rq, hctx_idx,
				hctx->numa_node))
		goto exit_hctx;

	return 0;

 exit_hctx:
	if (set->ops->exit_hctx)	/* [한국어] 드라이버가 init_hctx 로 잡아 둔 것이 있다면 */
		set->ops->exit_hctx(hctx, hctx_idx);	/* [한국어] 대칭되는 정리 콜백을 불러 되돌린다 */
 fail_free_fq:
	blk_free_flush_queue(hctx->fq);	/* [한국어] flush queue 해제 — 아직 SRCU 로 공개되기 전이라 즉시 해제해도 안전하다 */
	hctx->fq = NULL;	/* [한국어] 이중 해제 방지 */
 fail:
	return -1;	/* [한국어] 이 함수의 실패는 호출자가 개수만 보고 되감으므로 구체적 errno 대신 -1 을 쓴다 */
}

/*
 * [한국어]
 * blk_mq_alloc_hctx - 새 blk_mq_hw_ctx 구조체 할당 및 기본 초기화
 *
 * @q:    request_queue
 * @set:  blk_mq_tag_set
 * @node: NUMA node
 * @return: 할당된 hctx, 실패 시 NULL
 *
 * hctx 메모리, cpumask, ctxs[], ctx_map(sbitmap), dispatch_wait 초기화.
 * blk_mq_init_hctx 와 분리된 이유: 일부 필드는 queue 구조체 없이 미리 할당 가능.
 */
static struct blk_mq_hw_ctx *
blk_mq_alloc_hctx(struct request_queue *q, struct blk_mq_tag_set *set,
		int node)
{
	struct blk_mq_hw_ctx *hctx;
	gfp_t gfp = GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY;	/* [한국어] GFP_NOIO = 이 할당이 I/O 를 유발하면 자기 자신을 기다리는 교착이 된다. NOWARN/NORETRY = 실패해도 폴백 경로가 있으므로 조용히 빨리 실패한다 */

	hctx = kzalloc_node(sizeof(struct blk_mq_hw_ctx), gfp, node);	/* [한국어] hctx 구조체를 지정 NUMA 노드에 0 초기화 할당 — 이 hctx 를 쓰는 CPU 와 같은 노드여야 제출·완료 경로의 원격 접근이 없다 */
	if (!hctx)	/* [한국어] hctx 할당 실패 */
		goto fail_alloc_hctx;	/* [한국어] 아직 아무것도 잡지 않았으므로 그대로 NULL 반환 */

	/* [한국어] hctx->cpumask: 이 NVMe SQ 에 매핑된 CPU 집합 */
	if (!zalloc_cpumask_var_node(&hctx->cpumask, gfp, node))
		goto free_hctx;

	/* [한국어] nr_active: 이 hctx 에서 drv 에 전달된 CID 수 (budget tracking) */
	atomic_set(&hctx->nr_active, 0);
	if (node == NUMA_NO_NODE)	/* [한국어] 호출자가 노드를 특정하지 않았으면 */
		node = set->numa_node;	/* [한국어] tag set 의 기본 노드로 폴백 */
	hctx->numa_node = node;	/* [한국어] 이후 이 hctx 에 딸린 모든 할당(ctxs[], sbitmap, request 페이지)이 이 노드를 따른다 */

	/* [한국어] run_work: kblockd 에서 이 NVMe SQ 의 dispatch 를 비동기 실행 */
	INIT_DELAYED_WORK(&hctx->run_work, blk_mq_run_work_fn);
	spin_lock_init(&hctx->lock);	/* [한국어] hctx->dispatch 리스트를 보호하는 락 초기화 — 여러 CPU 가 동시에 접근하는 유일한 하드웨어 큐 단위 리스트다 */
	/* [한국어] dispatch: bypass 된 passthrough/requeue request 의 대기 list */
	INIT_LIST_HEAD(&hctx->dispatch);
	/* [한국어] cpuhp_dead/cpuhp_online: CPU hotplug hlist 노드 초기화 */
	INIT_HLIST_NODE(&hctx->cpuhp_dead);
	INIT_HLIST_NODE(&hctx->cpuhp_online);	/* [한국어] CPU online 콜백용 노드도 unhashed 로 초기화 */
	hctx->queue = q;	/* [한국어] 역참조용 소유 큐 포인터 — 콜백이 hctx 만 받았을 때 큐를 찾아가는 경로다 */
	/* [한국어] TAG_QUEUE_SHARED 는 나중에 add_queue_tag_set 에서 설정 */
	hctx->flags = set->flags & ~BLK_MQ_F_TAG_QUEUE_SHARED;

	/* [한국어] hctx_list: unused_hctx_list 연결용 */
	INIT_LIST_HEAD(&hctx->hctx_list);

	/*
	 * Allocate space for all possible cpus to avoid allocation at
	 * runtime
	 */
	/* [한국어] ctxs[]: 이 hctx 에 매핑된 per-CPU sw queue 포인터 배열 */
	hctx->ctxs = kmalloc_array_node(nr_cpu_ids, sizeof(void *),
			gfp, node);
	if (!hctx->ctxs)	/* [한국어] ctxs 배열 할당 실패 */
		goto free_cpumask;	/* [한국어] cpumask 부터 되돌리는 경로로 */

	/* [한국어] ctx_map: 이 hctx 에 pending request 가 있는 CPU 를 추적하는 sbitmap */
	if (sbitmap_init_node(&hctx->ctx_map, nr_cpu_ids, ilog2(8),
				gfp, node, false, false))
		goto free_ctxs;
	hctx->nr_ctx = 0;	/* [한국어] 아직 이 hctx 에 매핑된 sw 큐가 없다. blk_mq_map_swqueue 가 실제 개수를 채운다 */

	spin_lock_init(&hctx->dispatch_wait_lock);	/* [한국어] dispatch_wait 엔트리를 보호하는 락 — 중복 등록/해제 경쟁을 막는다 */
	/* [한국어] dispatch_wait: CID 고갈 시 tag 해제를 기다리는 waitqueue entry.
	 * blk_mq_dispatch_wake 가 wake 콜백으로 등록됨 */
	init_waitqueue_func_entry(&hctx->dispatch_wait, blk_mq_dispatch_wake);
	INIT_LIST_HEAD(&hctx->dispatch_wait.entry);	/* [한국어] 대기 엔트리를 아직 어느 wait queue 에도 매달지 않은 상태로 초기화 */

	/* [한국어] sysfs kobject 초기화: /sys/block/nvme0n1/mq/<hctx_idx>/ */
	blk_mq_hctx_kobj_init(hctx);

	return hctx;	/* [한국어] 완전히 초기화된 hctx 반환 */

 free_ctxs:
	kfree(hctx->ctxs);	/* [한국어] 역순 해제 1단계 — ctxs 배열 */
 free_cpumask:
	free_cpumask_var(hctx->cpumask);	/* [한국어] 역순 해제 2단계 — cpumask */
 free_hctx:
	kfree(hctx);	/* [한국어] 역순 해제 3단계 — hctx 구조체 자체 */
 fail_alloc_hctx:
	return NULL;	/* [한국어] 실패 원인은 메모리 부족뿐이므로 NULL 로만 알린다 */
}

/*
 * [한국어]
 * blk_mq_init_cpu_queues - 모든 possible CPU 에 대해 per-CPU sw queue 초기화
 *
 * @q:            request_queue
 * @nr_hw_queues: hctx 수 (NVMe SQ 수)
 *
 * 각 CPU 의 blk_mq_ctx 를 초기화하고 CPU → hctx NUMA node 를 설정.
 * 다중 HW queue 가 있을 때만 CPU local node 를 hctx 에 반영.
 */
static void blk_mq_init_cpu_queues(struct request_queue *q,
				   unsigned int nr_hw_queues)
{
	struct blk_mq_tag_set *set = q->tag_set;
	unsigned int i, j;	/* [한국어] i = CPU 번호, j = 맵(hctx 타입) 인덱스 */

	for_each_possible_cpu(i) {	/* [한국어] 온라인 여부와 무관하게 가능한 모든 CPU 를 초기화한다 — 나중에 핫플러그로 올라올 CPU 도 즉시 쓸 수 있어야 하기 때문이다 */
		/* [한국어] 이 CPU 의 per-CPU sw queue 초기화 */
		struct blk_mq_ctx *__ctx = per_cpu_ptr(q->queue_ctx, i);
		struct blk_mq_hw_ctx *hctx;	/* [한국어] 아래 맵 순회에서 조회한 hctx 를 담을 임시 포인터 */
		int k;	/* [한국어] rq_lists 유형 순회 인덱스 */

		__ctx->cpu = i;	/* [한국어] 이 sw 큐가 어느 CPU 것인지 스스로 기억하게 한다 — 완료를 원래 CPU 로 되돌릴 때 이 값을 쓴다 */
		spin_lock_init(&__ctx->lock);	/* [한국어] rq_lists 와 ctx_map 비트를 보호하는 per-CPU 락 초기화 */
		/* [한국어] rq_lists[type]: DEFAULT/READ/POLL 각 유형별 pending request list */
		for (k = HCTX_TYPE_DEFAULT; k < HCTX_MAX_TYPES; k++)
			INIT_LIST_HEAD(&__ctx->rq_lists[k]);

		__ctx->queue = q;	/* [한국어] 역참조용 소유 큐 포인터 */

		/*
		 * Set local node, IFF we have more than one hw queue. If
		 * not, we remain on the home node of the device
		 */
		for (j = 0; j < set->nr_maps; j++) {	/* [한국어] DEFAULT/READ/POLL 각 맵에서 이 CPU 가 쓰는 hctx 를 찾는다 */
			hctx = blk_mq_map_queue_type(q, j, i);	/* [한국어] CPU i 가 맵 j 에서 사용할 hctx 조회 */
			/* [한국어] 다중 HW queue: CPU 의 NUMA node 를 hctx 에 반영 */
			if (nr_hw_queues > 1 && hctx->numa_node == NUMA_NO_NODE)
				hctx->numa_node = cpu_to_node(i);
		}
	}
}

/*
 * [한국어]
 * blk_mq_alloc_map_and_rqs - hctx 를 위한 tag map 과 request 객체 일괄 할당
 *
 * @set:      blk_mq_tag_set
 * @hctx_idx: hctx 인덱스 (NVMe SQ 번호)
 * @depth:    CID 슬롯 수 (= queue depth)
 * @return:   blk_mq_tags 포인터, 실패 시 NULL
 */
struct blk_mq_tags *blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
					     unsigned int hctx_idx,
					     unsigned int depth)
{
	struct blk_mq_tags *tags;
	int ret;	/* [한국어] blk_mq_alloc_rqs 의 결과 코드 */

	/* [한국어] sbitmap + rqs[]/static_rqs[] 배열 할당 */
	tags = blk_mq_alloc_rq_map(set, hctx_idx, depth, set->reserved_tags);
	if (!tags)	/* [한국어] sbitmap/배열 할당 실패 */
		return NULL;	/* [한국어] 아직 할당한 것이 없으므로 NULL 반환 */

	/* [한국어] static_rqs[]: request 객체 + NVMe cmd payload 페이지 할당 */
	ret = blk_mq_alloc_rqs(set, tags, hctx_idx, depth);
	if (ret) {	/* [한국어] request 객체 페이지 할당 실패 */
		blk_mq_free_rq_map(set, tags);	/* [한국어] 앞 단계에서 만든 sbitmap 과 배열을 되돌린다 */
		return NULL;	/* [한국어] 부분 초기화 상태를 남기지 않고 실패를 알린다 */
	}

	return tags;	/* [한국어] sbitmap + 배열 + request 객체가 모두 준비된 tags 반환 */
}

/*
 * [한국어]
 * __blk_mq_alloc_map_and_rqs - shared tags 여부에 따라 tag 할당 또는 공유 연결
 */
static bool __blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
				       int hctx_idx)
{
	if (blk_mq_is_shared_tags(set->flags)) {
		/* [한국어] shared_tags: 별도 할당 없이 공유 pool 포인터만 복사 */
		set->tags[hctx_idx] = set->shared_tags;
		return true;
	}

	/* [한국어] 개별 tags: 이 hctx 전용 CID pool 과 request 객체 할당 */
	set->tags[hctx_idx] = blk_mq_alloc_map_and_rqs(set, hctx_idx,
						       set->queue_depth);

	return set->tags[hctx_idx];	/* [한국어] 새로 만든 tag pool 을 tag set 슬롯에 꽂고 그대로 반환(실패 시 NULL) */
}

/*
 * [한국어]
 * blk_mq_free_map_and_rqs - tag pool 의 request 객체와 map 해제
 */
void blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
			     struct blk_mq_tags *tags,
			     unsigned int hctx_idx)
{
	if (tags) {
		blk_mq_free_rqs(set, tags, hctx_idx);	/* [한국어] 먼저 request 객체와 그 페이지들을 해제 */
		blk_mq_free_rq_map(set, tags);	/* [한국어] 그다음 rqs[]/static_rqs[] 배열과 sbitmap 을 해제 — 역순이어야 이미 해제된 배열을 참조하지 않는다 */
	}
}

/*
 * [한국어]
 * __blk_mq_free_map_and_rqs - set->tags[hctx_idx] 해제 및 NULL 초기화
 *
 * shared_tags 는 공유 pool 이므로 직접 해제하지 않는다.
 */
static void __blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
				      unsigned int hctx_idx)
{
	/* [한국어] shared_tags 가 아닐 때만 개별 tag pool 해제 */
	if (!blk_mq_is_shared_tags(set->flags))
		blk_mq_free_map_and_rqs(set, set->tags[hctx_idx], hctx_idx);

	set->tags[hctx_idx] = NULL;	/* [한국어] 공유 풀이었든 개별 풀이었든 슬롯은 비운다 — 이후 재구성이 이 값을 보고 재할당 여부를 판단한다 */
}

/*
 * [한국어]
 * blk_mq_map_swqueue - CPU ↔ 하드웨어 큐(NVMe SQ/CQ) 매핑 테이블을 (재)구축
 *
 * @q: 매핑을 구축할 request_queue
 * @return: 없음
 *
 * === 이 함수가 만드는 것: blk-mq의 2단계 큐 구조 ===
 * blk-mq는 큐를 두 층으로 나눈다.
 *   소프트웨어 큐(ctx)  - CPU마다 하나. 락 경합 없이 request를 쌓는 곳.
 *   하드웨어 큐(hctx)   - 장치의 실제 큐마다 하나. NVMe에서는 SQ/CQ 쌍 하나.
 * 이 함수는 그 둘 사이의 대응 관계를 양방향으로 채운다:
 *   ctx->hctxs[type]  - "이 CPU가 이 종류의 I/O를 낼 때 쓸 하드웨어 큐"
 *   hctx->cpumask     - "이 하드웨어 큐를 쓰는 CPU들"
 *   hctx->ctxs[]      - "이 하드웨어 큐에 매달린 소프트웨어 큐들"
 *
 * === 매핑의 원천: set->map[type].mq_map[cpu] ===
 * 실제 대응 관계는 이 함수가 정하지 않는다. tag set을 만들 때 드라이버가
 * ->map_queues 콜백에서 이미 계산해 mq_map[] 배열에 채워 둔 것을 여기서 읽어
 * 자료구조에 반영할 뿐이다.
 * NVMe PCIe에서는 drivers/nvme/host/pci.c의 nvme_pci_map_queues()가 이를 맡으며,
 * 맵 종류에 따라 두 가지 방식을 쓴다:
 *   인터럽트를 쓰는 맵(DEFAULT/READ) → blk_mq_map_hw_queues(map, dev->dev, offset)
 *     각 하드웨어 큐의 MSI-X 벡터 affinity를 그대로 CPU 매핑으로 삼는다.
 *     offset은 admin 큐가 벡터 0을 쓰는 것을 보정하는 값이다.
 *   POLL 맵 → blk_mq_map_queues(map)
 *     폴링 큐는 인터럽트 자체가 없어 affinity가 존재하지 않으므로, 일반적인
 *     CPU 라운드로빈 방식으로 나눈다.
 *
 * 인터럽트 기반 맵에서 affinity를 그대로 따르는 결과, 다음 정렬이 만들어진다:
 *   CPU X가 커맨드를 제출한 SQ의 짝 CQ가, 완료 인터럽트를 다시 CPU X로 보낸다.
 * 이 정렬 덕분에 제출과 완료가 같은 CPU에서 일어나 캐시 라인이 CPU 간에
 * 이동하지 않고, request 구조체와 SQ/CQ 링 버퍼가 같은 NUMA 노드에 머문다.
 * NVMe가 수백만 IOPS를 내는 데 이 CPU-큐 정렬이 결정적인 이유다.
 *
 * === HCTX 타입(map 인덱스)의 의미 ===
 * set->nr_maps는 하드웨어 큐를 몇 종류로 나눌지를 뜻하며, NVMe는 최대 3종을 쓴다:
 *   HCTX_TYPE_DEFAULT - 기본(쓰기 포함). 항상 존재한다.
 *   HCTX_TYPE_READ    - 읽기 전용 큐. nvme 모듈 파라미터 write_queues > 0일 때
 *                       읽기/쓰기를 다른 하드웨어 큐로 분리해, 큰 쓰기가 읽기
 *                       지연을 밀어내는 현상을 줄인다.
 *   HCTX_TYPE_POLL    - 폴링 전용 큐. poll_queues > 0일 때 생기며, 이 큐들은
 *                       인터럽트를 아예 쓰지 않고 io_uring의 IOPOLL이 직접
 *                       CQ를 돌며 완료를 수거한다. 초저지연 경로다.
 * 어떤 타입에 큐가 배정되지 않았으면(nr_queues == 0) 그 타입의 요청은 DEFAULT
 * 큐로 폴백된다.
 *
 * === 언제 호출되는가 ===
 *   큐 최초 생성(blk_mq_init_allocated_queue), CPU 핫플러그, 그리고 NVMe
 *   컨트롤러 리셋 후 하드웨어 큐 수가 바뀌었을 때(blk_mq_update_nr_hw_queues).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐가 freeze된 상태여야 한다. 진행 중인
 * I/O가 있는데 매핑을 바꾸면 그 request의 hctx 포인터가 무효해지기 때문이다.
 *
 * 에러 경로: 특정 hctx의 태그 풀 할당에 실패하면 그 CPU를 hctx[0]으로 되돌린다.
 * hctx[0]은 절대 해제되지 않아 항상 유효한 폴백 대상임이 보장된다.
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue / blk_mq_update_nr_hw_queues /
 *   blk_mq_queue_reinit (CPU 핫플러그) → [blk_mq_map_swqueue]
 *     → blk_mq_map_queue_type (set->map[].mq_map[] 조회)
 *     → __blk_mq_alloc_map_and_rqs (태그 풀 확보)
 *     → sbitmap_resize (ctx_map 축소)
 */
static void blk_mq_map_swqueue(struct request_queue *q)
{
	/* [한국어] j = 큐 타입 인덱스(DEFAULT/READ/POLL), hctx_idx = 매핑 테이블에서
	 * 읽은 하드웨어 큐 번호. */
	unsigned int j, hctx_idx;
	/* [한국어] i는 두 용도로 쓰인다 — 앞의 hctx 순회 인덱스이자 뒤의 CPU 번호다.
	 * unsigned long인 이유는 cpumask 관련 매크로가 그 타입을 요구하기 때문이다. */
	unsigned long i;
	struct blk_mq_hw_ctx *hctx;	/* [한국어] 1단계 순회에서 쓸 hctx 커서 */
	struct blk_mq_ctx *ctx;	/* [한국어] 2단계에서 CPU 별 sw 큐를 가리킬 커서 */
	struct blk_mq_tag_set *set = q->tag_set;	/* [한국어] 맵 정보(map[type].mq_map[cpu])의 원천 — 드라이버가 map_queues 로 채워 둔 것이다 */

	/* [한국어] ★ 1단계: 기존 매핑을 모두 지운다 ★
	 * 이 함수는 최초 생성뿐 아니라 CPU 핫플러그나 큐 수 변경으로 재호출된다.
	 * 그때 옛 매핑이 남아 있으면 이제는 이 큐를 쓰지 않는 CPU가 cpumask에
	 * 남아, dispatch가 엉뚱한 CPU로 워커를 예약하게 된다. */
	queue_for_each_hw_ctx(q, hctx, i) {
		/* [한국어] 이 하드웨어 큐를 쓰는 CPU 집합을 비운다. */
		cpumask_clear(hctx->cpumask);
		/* [한국어] 매달린 소프트웨어 큐 개수를 0으로. 아래 루프가 다시 센다. */
		hctx->nr_ctx = 0;
		/* [한국어] 라운드로빈 dispatch의 다음 시작 위치도 초기화한다.
		 * 옛 포인터가 남으면 재구성 후 존재하지 않는 ctx를 가리킬 수 있다. */
		hctx->dispatch_from = NULL;
	}

	/*
	 * Map software to hardware queues.
	 *
	 * If the cpu isn't present, the cpu is mapped to first hctx.
	 */
	/* [한국어] ★ 2단계: 모든 CPU에 대해 타입별 매핑을 채운다 ★
	 * for_each_possible_cpu는 지금 오프라인인 CPU까지 포함한다. 나중에
	 * 온라인될 CPU도 매핑을 갖고 있어야, 그 CPU가 살아나는 순간 바로
	 * I/O를 낼 수 있기 때문이다. */
	for_each_possible_cpu(i) {
		/* [한국어] 이 CPU 전용 소프트웨어 큐. per-CPU 변수이므로 다른 CPU와
		 * 락 없이 독립적으로 쓰인다 — 이것이 blk-mq가 제출 경로의 락 경합을
		 * 없앤 핵심 구조다. */
		ctx = per_cpu_ptr(q->queue_ctx, i);
		for (j = 0; j < set->nr_maps; j++) {	/* [한국어] 이 CPU 에 대해 DEFAULT/READ/POLL 각 맵을 차례로 처리 */
			/* [한국어] 이 타입에 배정된 하드웨어 큐가 하나도 없는 경우.
			 * NVMe에서 write_queues=0이면 READ 맵이, poll_queues=0이면
			 * POLL 맵이 이 상태가 된다. 그런 타입의 요청은 DEFAULT 큐로
			 * 보내야 하므로, 미리 DEFAULT의 hctx를 채워 둔다.
			 * 이렇게 해 두면 제출 경로가 "타입별 큐가 있는가"를 매번
			 * 확인하지 않고 ctx->hctxs[type]을 바로 쓸 수 있다. */
			if (!set->map[j].nr_queues) {
				ctx->hctxs[j] = blk_mq_map_queue_type(q,
						HCTX_TYPE_DEFAULT, i);	/* [한국어] DEFAULT 맵의 hctx 를 폴백으로 채운다 */
				continue;
			}
			hctx_idx = set->map[j].mq_map[i];	/* [한국어] 드라이버의 map_queues 콜백이 채워 둔 "CPU i 가 맵 j 에서 쓸 hctx 번호". NVMe 라면 nvme_pci_map_queues 가 MSI-X affinity 를 보고 채운 값이다 */
			/* unmapped hw queue can be remapped after CPU topo changed */
			if (!set->tags[hctx_idx] &&	/* [한국어] 그 hctx 번호에 태그 풀이 아직 없고(CPU 토폴로지 변경으로 새로 매핑된 경우) */
			    /* [한국어] tag 할당 실패: hctx[0] 에 fallback 매핑 */
			    !__blk_mq_alloc_map_and_rqs(set, hctx_idx)) {
				/*
				 * If tags initialization fail for some hctx,
				 * that hctx won't be brought online.  In this
				 * case, remap the current ctx to hctx[0] which
				 * is guaranteed to always have tags allocated
				 */
				set->map[j].mq_map[i] = 0;	/* [한국어] 태그 풀을 못 만들었으므로 이 CPU 를 hctx 0 으로 되돌린다 — hctx 0 은 절대 해제되지 않아 언제나 유효한 폴백이다 */
			}

			hctx = blk_mq_map_queue_type(q, j, i);	/* [한국어] 확정된 매핑으로 실제 hctx 포인터를 조회 */
			/* [한국어] ctx->hctxs[j]: 이 CPU 가 type j 로 사용할 hctx(NVMe SQ) */
			ctx->hctxs[j] = hctx;
			/*
			 * If the CPU is already set in the mask, then we've
			 * mapped this one already. This can happen if
			 * devices share queues across queue maps.
			 */
			/* [한국어] 이미 이 CPU 가 이 hctx 에 매핑돼 있으면 중복 등록 생략 */
			if (cpumask_test_cpu(i, hctx->cpumask))
				continue;

			/* [한국어] hctx->cpumask 에 이 CPU 추가: SQ IRQ affinity 기반 */
			cpumask_set_cpu(i, hctx->cpumask);
			hctx->type = j;	/* [한국어] 이 hctx 가 어떤 맵 타입에 속하는지 기록 — 완료·삽입 경로가 ctx->rq_lists[type] 인덱싱에 쓴다 */
			/* [한국어] ctx->index_hw[j]: 이 CPU 가 hctx->ctxs[] 에서 몇 번째인지 */
			ctx->index_hw[hctx->type] = hctx->nr_ctx;
			/* [한국어] hctx->ctxs[nr_ctx++] 에 이 ctx 등록 */
			hctx->ctxs[hctx->nr_ctx++] = ctx;

			/*
			 * If the nr_ctx type overflows, we have exceeded the
			 * amount of sw queues we can support.
			 */
			BUG_ON(!hctx->nr_ctx);	/* [한국어] 방금 증가시켰으니 0 일 수 없다. 0 이면 nr_ctx 가 오버플로했다는 뜻이고(위 영문 주석), 그대로 두면 배열 범위를 벗어나므로 즉시 멈춘다 */
		}

		/* [한국어] 남은 type(j..HCTX_MAX_TYPES) 은 모두 DEFAULT hctx 로 설정 */
		for (; j < HCTX_MAX_TYPES; j++)
			ctx->hctxs[j] = blk_mq_map_queue_type(q,
					HCTX_TYPE_DEFAULT, i);
	}

	queue_for_each_hw_ctx(q, hctx, i) {	/* [한국어] 2단계 — 매핑이 끝난 뒤 각 hctx 를 활성/비활성으로 확정한다 */
		int cpu;	/* [한국어] isolated CPU 를 걸러내는 아래 루프의 순회 변수 */

		/*
		 * If no software queues are mapped to this hardware queue,
		 * disable it and free the request entries.
		 */
		if (!hctx->nr_ctx) {	/* [한국어] 이 hctx 에 매핑된 sw 큐가 하나도 없는가 — 쓰이지 않는 하드웨어 큐라는 뜻이다 */
			/* [한국어] sw queue 가 없는 NVMe SQ: tag pool 해제 후 비활성화.
			 * hctx[0] 는 fallback 으로 보존 */
			/* Never unmap queue 0.  We need it as a
			 * fallback in case of a new remap fails
			 * allocation
			 */
			if (i)
				__blk_mq_free_map_and_rqs(set, i);

			hctx->tags = NULL;	/* [한국어] 태그 풀 연결을 끊어 이 hctx 를 비활성으로 만든다 */
			continue;
		}

		/* [한국어] 활성화된 hctx 에 tag pool 연결 */
		hctx->tags = set->tags[i];
		WARN_ON(!hctx->tags);	/* [한국어] 여기까지 왔다면 태그 풀이 반드시 있어야 한다 — 없으면 위쪽 할당 로직에 구멍이 있다는 뜻이다 */

		/*
		 * Set the map size to the number of mapped software queues.
		 * This is more accurate and more efficient than looping
		 * over all possibly mapped software queues.
		 */
		/* [한국어] ctx_map 의 실제 크기를 매핑된 sw queue 수에 맞게 축소 */
		sbitmap_resize(&hctx->ctx_map, hctx->nr_ctx);

		/*
		 * Rule out isolated CPUs from hctx->cpumask to avoid
		 * running block kworker on isolated CPUs.
		 * FIXME: cpuset should propagate further changes to isolated CPUs
		 * here.
		 */
		rcu_read_lock();	/* [한국어] cpu_is_isolated() 가 cpuset 자료구조를 읽으므로 RCU 읽기 구간이 필요하다 */
		/* [한국어] isolated CPU 는 kblockd 스케줄링 방지를 위해 cpumask 제외 */
		for_each_cpu(cpu, hctx->cpumask) {
			if (cpu_is_isolated(cpu))
				cpumask_clear_cpu(cpu, hctx->cpumask);	/* [한국어] isolated CPU 를 cpumask 에서 뺀다 — 그 CPU 에 kblockd 워커를 예약하면 격리 목적이 깨진다. 단, 그 CPU 가 I/O 를 제출하는 것 자체는 막지 않는다 */
		}
		rcu_read_unlock();	/* [한국어] 순회가 끝났으므로 RCU 읽기 구간 해제 */

		/*
		 * Initialize batch roundrobin counts
		 */
		/* [한국어] next_cpu: kblockd work 가 실행될 CPU (round-robin 시작점) */
		hctx->next_cpu = blk_mq_first_mapped_cpu(hctx);
		/* [한국어] next_cpu_batch: round-robin 전환 전 같은 CPU 에서 처리할 request 수 */
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}
}

/*
 * Caller needs to ensure that we're either frozen/quiesced, or that
 * the queue isn't live yet.
 */
/*
 * [한국어]
 * queue_set_hctx_shared - 큐의 모든 hctx에 "태그 공유" 상태를 전파
 *
 * @q:      대상 request_queue
 * @shared: true면 태그 풀을 다른 큐와 공유하는 상태로 전환
 * @return: 없음
 *
 * === 태그 공유란 ===
 * 하나의 blk_mq_tag_set을 여러 request_queue가 함께 쓰는 구성이 있다.
 * NVMe에서는 컨트롤러 하나에 네임스페이스가 여러 개일 때가 그렇다 —
 * /dev/nvme0n1, /dev/nvme0n2가 같은 컨트롤러의 태그 풀(= CID 공간)을 나눠 쓴다.
 * 이때 한 네임스페이스가 태그를 독점하면 다른 쪽이 굶으므로, 활성 큐 수로
 * 나눠 각자의 몫만 쓰게 제한해야 한다. BLK_MQ_F_TAG_QUEUE_SHARED가 그
 * 제한 로직(blk_mq_tag_busy/idle, hctx_may_queue)을 활성화하는 스위치다.
 *
 * 이 함수는 tag_set에 큐가 추가/제거되어 공유 여부가 바뀔 때, 그 사실을
 * 큐에 속한 모든 hctx에 반영한다.
 *
 * unshared로 갈 때 blk_mq_tag_idle()을 먼저 부르는 순서가 중요하다.
 * 활성 큐 카운터를 정리하지 않고 플래그만 지우면, 그 카운터가 영원히
 * 남아 다음에 다시 공유 상태가 될 때 잘못된 몫 계산이 이뤄진다.
 *
 * 실행 컨텍스트: tag_set 변경 경로(프로세스 컨텍스트). 큐가 freeze된 상태.
 *
 * 호출 체인:
 *   blk_mq_update_tag_set_shared → [queue_set_hctx_shared] → blk_mq_tag_idle
 */
static void queue_set_hctx_shared(struct request_queue *q, bool shared)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	queue_for_each_hw_ctx(q, hctx, i) {	/* [한국어] 이 큐의 모든 하드웨어 큐에 같은 모드를 적용 */
		if (shared) {	/* [한국어] 공유 모드로 바꾸는가 */
			/* [한국어] shared 전환: BLK_MQ_F_TAG_QUEUE_SHARED 플래그 설정 */
			hctx->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
		} else {
			/* [한국어] unshared 전환: tag idle 후 플래그 제거 */
			blk_mq_tag_idle(hctx);
			hctx->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;	/* [한국어] 공유 해제 — 이제 이 hctx 가 태그 풀을 독점하므로 공유 시의 몫 제한(hctx_may_queue)을 걷어낸다 */
		}
	}
}

/*
 * [한국어]
 * blk_mq_update_tag_set_shared - tag_set 의 모든 queue 에 shared 모드 변경 적용
 *
 * @set:    blk_mq_tag_set
 * @shared: true = shared 모드로, false = unshared 모드로
 *
 * tag_list_lock 보유 상태에서 호출.
 * 각 queue 를 개별적으로 freeze → 모드 변경 → unfreeze.
 */
static void blk_mq_update_tag_set_shared(struct blk_mq_tag_set *set,
					 bool shared)
{
	struct request_queue *q;
	unsigned int memflags;	/* [한국어] blk_mq_freeze_queue 가 저장해 주는 memalloc_noio 상태 — unfreeze 에 그대로 돌려줘야 한다 */

	lockdep_assert_held(&set->tag_list_lock);	/* [한국어] 호출자가 tag_list_lock 을 쥐고 있어야 한다 — 순회 중 리스트가 바뀌면 안 되기 때문이다 */

	list_for_each_entry(q, &set->tag_list, tag_set_list) {	/* [한국어] 이 tag set 을 공유하는 모든 큐(NVMe 라면 한 컨트롤러의 전 네임스페이스)를 순회 */
		/* [한국어] queue freeze: in-flight request 완료 대기 후 shared 전환 */
		memflags = blk_mq_freeze_queue(q);
		queue_set_hctx_shared(q, shared);	/* [한국어] 이 큐의 모든 hctx 에 공유 플래그를 적용. freeze 중이라 in-flight 요청이 없어 안전하다 */
		blk_mq_unfreeze_queue(q, memflags);	/* [한국어] 변경이 끝났으므로 즉시 unfreeze — 큐 하나씩 처리해 전체 정지 시간을 줄인다 */
	}
}

/*
 * [한국어]
 * blk_mq_del_queue_tag_set - request_queue 를 tag_set 의 tag_list 에서 제거
 *
 * @q: 제거할 request_queue
 *
 * tag_list 의 마지막 queue 가 제거되면 shared 상태에서 unshared 로 전환.
 * RCU 를 사용하여 reader 와 race 없이 안전하게 제거.
 */
static void blk_mq_del_queue_tag_set(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;

	mutex_lock(&set->tag_list_lock);	/* [한국어] tag_list 수정과 shared 전환을 직렬화하는 뮤텍스 */
	/* [한국어] RCU list 제거: 이후 readers 는 이 q 를 보지 못함 */
	list_del_rcu(&q->tag_set_list);
	if (list_is_singular(&set->tag_list)) {	/* [한국어] 제거하고 나면 큐가 하나만 남는가 — 즉 공유할 상대가 사라지는가 */
		/* just transitioned to unshared */
		/* [한국어] 1개만 남음: unshared 로 전환 (CID pool 분리) */
		set->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;
		blk_mq_update_tag_set_shared(set, false);	/* [한국어] 남은 하나가 태그 풀을 독점하게 만든다 */
	}
	mutex_unlock(&set->tag_list_lock);	/* [한국어] 리스트 수정과 모드 전환이 끝났으므로 뮤텍스 해제 */
}

/*
 * [한국어]
 * blk_mq_add_queue_tag_set - request_queue 를 tag_set 의 tag_list 에 추가
 *
 * @set: blk_mq_tag_set
 * @q:   추가할 request_queue
 *
 * 2번째 queue 추가 시 shared 모드로 전환하여 모든 queue 가 CID pool 을 공유.
 * RCU 를 사용하여 reader 와 race 없이 안전하게 추가.
 */
static void blk_mq_add_queue_tag_set(struct blk_mq_tag_set *set,
				     struct request_queue *q)
{
	mutex_lock(&set->tag_list_lock);

	/*
	 * Check to see if we're transitioning to shared (from 1 to 2 queues).
	 */
	if (!list_empty(&set->tag_list) &&	/* [한국어] 이미 등록된 큐가 있고(즉 이번이 두 번째 이상이고) */
	    !(set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)) {	/* [한국어] 아직 공유 모드가 아니면 — 1개에서 2개로 넘어가는 전환 시점이다 */
		/* [한국어] 1→2 queue 전환: shared 모드 활성화 */
		set->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
		blk_mq_update_tag_set_shared(set, true);	/* [한국어] 기존 큐들에도 공유 플래그를 소급 적용한다 */
	}
	if (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)	/* [한국어] 이미 공유 모드였거나 방금 전환되었으면 */
		/* [한국어] 이미 shared 면 새 queue 에도 플래그 설정 */
		queue_set_hctx_shared(q, true);
	/* [한국어] RCU list 추가: reader 는 이 시점부터 새 q 를 볼 수 있음 */
	list_add_tail_rcu(&q->tag_set_list, &set->tag_list);

	mutex_unlock(&set->tag_list_lock);	/* [한국어] 리스트 추가와 모드 전환이 끝났으므로 뮤텍스 해제 */
}

/* All allocations will be freed in release handler of q->mq_kobj */
/*
 * [한국어]
 * blk_mq_alloc_ctxs - per-CPU blk_mq_ctx 배열 할당 및 초기화
 *
 * @q: request_queue
 * @return: 0 성공, -ENOMEM 실패
 *
 * blk_mq_ctxs 컨테이너와 per-CPU blk_mq_ctx 를 할당하고 q->queue_ctx 에 연결.
 * 모든 할당은 q->mq_kobj release handler 에서 해제된다.
 */
static int blk_mq_alloc_ctxs(struct request_queue *q)
{
	struct blk_mq_ctxs *ctxs;
	int cpu;	/* [한국어] for_each_possible_cpu 순회 변수 */

	/* [한국어] blk_mq_ctxs: kobject + per-CPU queue_ctx 를 묶는 컨테이너 */
	ctxs = kzalloc_obj(*ctxs);
	if (!ctxs)	/* [한국어] 컨테이너 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 잡은 것이 없으므로 그대로 실패 반환 */

	/* [한국어] alloc_percpu: 각 CPU 에 독립적인 blk_mq_ctx 할당 */
	ctxs->queue_ctx = alloc_percpu(struct blk_mq_ctx);
	if (!ctxs->queue_ctx)	/* [한국어] per-CPU 영역 할당 실패 */
		goto fail;	/* [한국어] 컨테이너부터 되돌리는 경로로 */

	/* [한국어] 각 CPU ctx 에 컨테이너 역참조 등록 */
	for_each_possible_cpu(cpu) {
		struct blk_mq_ctx *ctx = per_cpu_ptr(ctxs->queue_ctx, cpu);
		ctx->ctxs = ctxs;	/* [한국어] ctx 에서 컨테이너(그리고 그 kobject)를 거슬러 올라갈 수 있게 한다 — sysfs 경로에서 필요하다 */
	}

	/* [한국어] q->mq_kobj: sysfs kobject. q->queue_ctx: per-CPU ctx 배열 */
	q->mq_kobj = &ctxs->kobj;
	q->queue_ctx = ctxs->queue_ctx;	/* [한국어] 이후 제출 경로가 per_cpu_ptr(q->queue_ctx, cpu) 로 자기 CPU 의 sw 큐를 O(1) 로 찾는다 */

	return 0;
 fail:
	kfree(ctxs);	/* [한국어] per-CPU 할당에 실패했으므로 컨테이너만 되돌린다 */
	return -ENOMEM;	/* [한국어] 이 함수의 실패 원인은 메모리 부족뿐이다 */
}

/*
 * It is the actual release handler for mq, but we do it from
 * request queue's release handler for avoiding use-after-free
 * and headache because q->mq_kobj shouldn't have been introduced,
 * but we can't group ctx/kctx kobj without it.
 */
/*
 * [한국어]
 * blk_mq_release - blk-mq request_queue 의 실제 해제 처리
 *
 * @q: 해제할 request_queue
 *
 * request_queue release handler 에서 호출.
 * unused_hctx_list 의 모든 hctx kobject 해제 → queue_hw_ctx 배열 해제 →
 * mq_kobj(per-CPU ctx) 해제 순으로 처리.
 */
void blk_mq_release(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx, *next;
	unsigned long i;	/* [한국어] queue_for_each_hw_ctx 순회 인덱스 */

	/* [한국어] 모든 hctx 가 unused_hctx_list 에 있어야 함 (exit_hw_queues 완료 확인) */
	queue_for_each_hw_ctx(q, hctx, i)
		WARN_ON_ONCE(hctx && list_empty(&hctx->hctx_list));

	/* all hctx are in .unused_hctx_list now */
	/* [한국어] unused_hctx_list 의 모든 hctx kobject 해제 */
	list_for_each_entry_safe(hctx, next, &q->unused_hctx_list, hctx_list) {
		list_del_init(&hctx->hctx_list);
		kobject_put(&hctx->kobj);	/* [한국어] kobject 참조를 내린다 — 마지막 참조였다면 release 핸들러가 hctx 메모리를 실제로 해제한다 */
	}

	/* [한국어] queue_hw_ctx[]: hctx 포인터 배열 해제 */
	kfree(q->queue_hw_ctx);

	/*
	 * release .mq_kobj and sw queue's kobject now because
	 * both share lifetime with request queue.
	 */
	/* [한국어] mq_kobj(blk_mq_ctxs) 와 sw queue sysfs 객체 해제 */
	blk_mq_sysfs_deinit(q);
}

/*
 * [한국어]
 * blk_mq_alloc_queue - blk-mq request_queue 생성 (NVMe namespace 큐)
 *
 * @set:       blk_mq_tag_set (NVMe 드라이버가 미리 초기화한 tag set)
 * @lim:       queue 제한 (max_sectors, max_segments 등). NULL 이면 기본값
 * @queuedata: 드라이버 private data (nvme_ns 포인터 등)
 * @return:    생성된 request_queue, 실패 시 ERR_PTR
 *
 * blk_alloc_queue → blk_mq_init_allocated_queue 순으로 초기화.
 * NVMe namespace 등록 전에 호출되어 I/O 가 가능한 큐를 반환.
 */
struct request_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata)
{
	struct queue_limits default_lim = { };
	struct request_queue *q;	/* [한국어] 생성 중인 request_queue */
	int ret;	/* [한국어] blk_mq_init_allocated_queue 의 결과 코드 */

	if (!lim)	/* [한국어] 호출자가 limits 를 주지 않았으면 */
		lim = &default_lim;	/* [한국어] 스택의 빈 기본값을 쓴다 — 이후 드라이버가 queue_limits 갱신 API 로 실제 값을 채운다 */
	/* [한국어] IO_STAT: iostat/cgroup accounting, NOWAIT: REQ_NOWAIT 지원 */
	lim->features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT;
	if (set->nr_maps > HCTX_TYPE_POLL)	/* [한국어] POLL 맵까지 만들 만큼 맵이 많은가(nr_maps > 2) */
		/* [한국어] POLL queue 포함 시 polling 완료 지원 활성화 */
		lim->features |= BLK_FEAT_POLL;

	q = blk_alloc_queue(lim, set->numa_node);	/* [한국어] 큐 뼈대 할당 — 아직 hctx 나 tag 연결은 없다 */
	if (IS_ERR(q))	/* [한국어] 큐 할당 실패 */
		return q;	/* [한국어] ERR_PTR 를 그대로 전달 */
	/* [한국어] queuedata: nvme_ns 구조체 — nvme_queue_rq 에서 역참조 */
	q->queuedata = queuedata;
	ret = blk_mq_init_allocated_queue(set, q);	/* [한국어] hctx 배열 생성, CPU↔큐 매핑, tag set 등록까지 여기서 이뤄진다 */
	if (ret) {	/* [한국어] 초기화 실패 */
		blk_put_queue(q);	/* [한국어] 큐 참조를 내려 해제 경로로 보낸다 */
		return ERR_PTR(ret);	/* [한국어] 오류 코드를 ERR_PTR 로 포장해 반환 */
	}
	return q;	/* [한국어] 즉시 I/O 를 받을 수 있는 큐 반환 */
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
/*
 * [한국어]
 * blk_mq_destroy_queue - request_queue 를 종료하고 모든 자원 정리
 *
 * @q: 종료할 request_queue
 *
 * blk_mq_alloc_queue 로 생성된 queue 를 안전하게 종료.
 * DYING 플래그 → drain → freeze → sync → exit 순서로 진행.
 * 호출자는 blk_put_queue 로 마지막 참조를 해제해야 함.
 *
 * 실행 컨텍스트: sleep 가능 (might_sleep).
 */
void blk_mq_destroy_queue(struct request_queue *q)
{
	WARN_ON_ONCE(!queue_is_mq(q));
	WARN_ON_ONCE(blk_queue_registered(q));	/* [한국어] 아직 sysfs 에 등록된 상태로 파괴하면 안 된다 — 등록 해제가 선행되어야 한다 */

	might_sleep();	/* [한국어] 아래에서 freeze 대기와 SRCU 동기화로 잠들 수 있음을 명시 */

	/* [한국어] DYING: 이후 새 IO 는 모두 -ENODEV 로 실패 */
	blk_queue_flag_set(QUEUE_FLAG_DYING, q);
	/* [한국어] drain: 새 IO 진입 차단 + percpu_ref kill 시작 */
	blk_queue_start_drain(q);
	/* [한국어] freeze_wait: percpu_ref 카운트가 0 이 될 때까지 대기
	 * (모든 in-flight request 완료 확인) */
	blk_mq_freeze_queue_wait(q);

	/* [한국어] blk_sync_queue: timeout_work 등 동기화 */
	blk_sync_queue(q);
	/* [한국어] requeue_work / run_work 취소 */
	blk_mq_cancel_work_sync(q);
	/* [한국어] hctx 해제 + tag_set list 에서 제거 */
	blk_mq_exit_queue(q);
}
EXPORT_SYMBOL(blk_mq_destroy_queue);

/*
 * [한국어]
 * __blk_mq_alloc_disk - gendisk 와 blk-mq request_queue 를 함께 생성
 *
 * @set:       blk_mq_tag_set (NVMe 드라이버 tag set)
 * @lim:       queue 제한. NULL 이면 기본값
 * @queuedata: 드라이버 private data (nvme_ns 등)
 * @lkclass:   lock class key (lockdep 검증용)
 * @return:    gendisk 포인터, 실패 시 ERR_PTR
 *
 * NVMe namespace 를 /dev/nvme0n1 등의 블록 장치로 노출할 때 사용.
 * alloc_queue → alloc_disk 순으로 생성하며, disk 할당 실패 시 queue 도 정리.
 */
struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata,
		struct lock_class_key *lkclass)
{
	struct request_queue *q;
	struct gendisk *disk;	/* [한국어] 생성한 큐에 붙일 gendisk */

	/* [한국어] request_queue 생성: hctx/tag_set 연결 완료 */
	q = blk_mq_alloc_queue(set, lim, queuedata);
	if (IS_ERR(q))	/* [한국어] 큐 생성 실패 */
		return ERR_CAST(q);	/* [한국어] ERR_PTR 를 gendisk 포인터 타입으로 바꿔 전달 */

	/* [한국어] gendisk 할당: /dev 노드 생성 준비 */
	disk = __alloc_disk_node(q, set->numa_node, lkclass);
	if (!disk) {	/* [한국어] gendisk 할당 실패 */
		blk_mq_destroy_queue(q);	/* [한국어] 큐를 DYING 으로 만들고 in-flight 를 배출 */
		blk_put_queue(q);	/* [한국어] 큐 참조를 내려 실제 해제로 보낸다 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 이 경로의 실패 원인은 메모리 부족뿐이다 */
	}
	set_bit(GD_OWNS_QUEUE, &disk->state);	/* [한국어] gendisk 가 큐의 수명을 소유한다고 표시 — 이후 del_gendisk 가 큐까지 정리한다 */
	return disk;	/* [한국어] NVMe 라면 이 gendisk 가 /dev/nvme0n1 로 노출된다 */
}
EXPORT_SYMBOL(__blk_mq_alloc_disk);

/*
 * [한국어]
 * blk_mq_alloc_disk_for_queue - 이미 존재하는 큐에 gendisk를 추가로 붙인다
 *
 * @q:       이미 만들어져 있는 request_queue
 * @lkclass: lockdep 검증용 lock class key
 * @return: 새 gendisk, 실패 시 NULL
 *
 * __blk_mq_alloc_disk()가 "큐와 디스크를 함께 만드는" 함수라면, 이쪽은
 * "큐는 이미 있고 디스크만 새로 붙이는" 경우를 위한 것이다.
 *
 * 언제 필요한가: 하나의 request_queue에 여러 gendisk가 붙는 구성이다.
 * NVMe 멀티패스(CONFIG_NVME_MULTIPATH)가 대표적인데, 여러 경로로 보이는
 * 같은 네임스페이스를 하나의 /dev/nvmeXnY로 노출할 때 큐와 디스크의
 * 생명주기가 분리된다.
 *
 * 참조 관리가 이 함수의 핵심이다. 디스크가 큐를 참조하므로 blk_get_queue()로
 * 참조를 올리고, 디스크 할당에 실패하면 반드시 되돌려야 한다. 이 짝이
 * 어긋나면 큐가 영원히 해제되지 않거나(누수) 너무 일찍 해제된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 초기화).
 *
 * 호출 체인:
 *   드라이버(예: NVMe 멀티패스 헤드 디스크 생성) → [blk_mq_alloc_disk_for_queue]
 *     → blk_get_queue → __alloc_disk_node
 */
struct gendisk *blk_mq_alloc_disk_for_queue(struct request_queue *q,
		struct lock_class_key *lkclass)
{
	struct gendisk *disk;

	/* [한국어] 큐 참조를 올린다. 실패하면 큐가 이미 죽어가는 중이므로 포기한다. */
	if (!blk_get_queue(q))
		return NULL;
	/* [한국어] gendisk를 할당한다. NUMA_NO_NODE는 특정 노드를 지정하지 않고
	 * 커널에 맡긴다는 뜻으로, 큐와 달리 디스크 구조체는 I/O 핫패스에서
	 * 접근되지 않아 노드 지역성이 중요하지 않다. */
	disk = __alloc_disk_node(q, NUMA_NO_NODE, lkclass);
	/* [한국어] 할당 실패 — 위에서 올린 참조를 반드시 되돌린다. */
	if (!disk)
		blk_put_queue(q);
	return disk;	/* [한국어] 큐와 디스크가 연결된 gendisk 반환 */
}
EXPORT_SYMBOL(blk_mq_alloc_disk_for_queue);

/*
 * Only hctx removed from cpuhp list can be reused
 */
/*
 * [한국어]
 * blk_mq_hctx_is_reusable - 폐기된 hctx를 재사용해도 안전한지 확인
 *
 * @hctx: unused_hctx_list에 들어 있는 hctx 후보
 * @return: true = 재사용 가능, false = 아직 CPU 핫플러그 콜백에 걸려 있음
 *
 * hctx는 사용이 끝나도 즉시 해제하지 않고 q->unused_hctx_list에 보관했다가
 * 재사용한다(할당 비용과 NUMA 지역성 때문). 그런데 hctx는 CPU 핫플러그
 * 알림을 받기 위해 두 개의 전역 hlist에 등록되어 있고, 그 등록이 해제되기
 * 전에 재사용하면 핫플러그 콜백이 "이미 다른 용도로 쓰이는" hctx를 건드리게
 * 되어 리스트가 꼬이거나 use-after-free가 발생한다.
 *
 * hlist_unhashed()는 노드가 어떤 리스트에도 연결되어 있지 않은지를 검사한다.
 * 두 리스트 모두에서 빠져 있어야만 완전히 자유로운 상태다:
 *   cpuhp_online - CPU가 온라인/오프라인으로 전환될 때 알림받는 목록
 *   cpuhp_dead   - CPU가 완전히 죽을 때 알림받는 목록(진행 중 요청 재배치용)
 *
 * NVMe에서 이 재사용이 일어나는 상황: 컨트롤러 리셋 후 하드웨어 큐 개수가
 * 바뀌거나, CPU 핫플러그로 큐 매핑이 재계산될 때 hctx 배열이 재구성된다.
 *
 * 실행 컨텍스트: 큐 재구성 경로. q->unused_hctx_lock을 쥔 상태에서 호출된다.
 *
 * 호출 체인:
 *   blk_mq_alloc_and_init_hctx → [blk_mq_hctx_is_reusable]
 */
static bool blk_mq_hctx_is_reusable(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] 두 핫플러그 리스트 모두에서 빠져 있어야 true. 하나라도 연결되어
	 * 있으면 아직 콜백 대상이므로 다른 후보를 찾아야 한다. */
	return hlist_unhashed(&hctx->cpuhp_online) &&
		hlist_unhashed(&hctx->cpuhp_dead);
}

/*
 * [한국어]
 * blk_mq_alloc_and_init_hctx - 하드웨어 컨텍스트 하나를 확보하고 초기화
 *
 * @set:      tag set. NVMe에서는 nvme_ctrl의 tagset으로, 컨트롤러 단위 자원
 *            (태그 풀 = CID 공간, 큐 매핑 테이블)을 담고 있다.
 * @q:        이 hctx가 속할 request_queue (NVMe 네임스페이스 하나)
 * @hctx_idx: 하드웨어 큐 인덱스. NVMe에서는 nvme_queue 배열의 인덱스와 대응하며,
 *            결국 SQ/CQ 쌍 하나를 가리킨다.
 * @node:     이 hctx의 자료구조를 할당할 NUMA 노드. 해당 하드웨어 큐를 주로
 *            사용할 CPU들이 속한 노드를 고른다.
 * @return: 초기화된 hctx, 실패 시 NULL
 *
 * === hctx란 무엇인가 ===
 * blk-mq는 소프트웨어 큐(ctx, CPU마다 하나)와 하드웨어 큐(hctx, 장치의 실제
 * 큐마다 하나)를 분리한다. hctx는 "장치가 실제로 가진 큐 하나"를 커널 쪽에서
 * 대표하는 객체로, dispatch 리스트, 실행 워커, 태그 정보, CPU 매핑을 갖는다.
 * NVMe PCIe에서 hctx 하나는 nvme_queue 하나(= SQ 링 + CQ 링 + doorbell 레지스터
 * 쌍 + MSI-X 벡터 하나)에 대응한다. nvme_init_hctx()가 hctx->driver_data에
 * 그 nvme_queue 포인터를 심어 둔다.
 *
 * === 재사용을 먼저 시도하는 이유 ===
 * hctx는 per-CPU 자료구조와 kobject를 포함해 할당 비용이 크고, NUMA 노드에 맞춰
 * 배치해야 성능이 나온다. 큐 재구성이 반복되는 상황(컨트롤러 리셋, CPU 핫플러그)
 * 에서 매번 새로 할당/해제하면 메모리 단편화와 지연이 커지므로, 같은 NUMA 노드의
 * 폐기된 hctx가 있으면 그것을 되살려 쓴다.
 *
 * 실행 컨텍스트: 큐 초기화/재구성 경로(프로세스 컨텍스트, 잠들 수 있음).
 * NVMe에서는 nvme_alloc_io_tag_set() 또는 컨트롤러 리셋 후 큐 개수 갱신 시.
 *
 * 에러 경로: 할당 실패나 초기화 실패 시 NULL. 호출자
 * __blk_mq_realloc_hw_ctxs()가 이전 노드로 폴백을 재시도한다.
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue / blk_mq_update_nr_hw_queues
 *     → __blk_mq_realloc_hw_ctxs → [blk_mq_alloc_and_init_hctx]
 *       → blk_mq_alloc_hctx → blk_mq_init_hctx → set->ops->init_hctx
 *         (= nvme_init_hctx, hctx->driver_data = nvme_queue)
 */
static struct blk_mq_hw_ctx *blk_mq_alloc_and_init_hctx(
		struct blk_mq_tag_set *set, struct request_queue *q,
		int hctx_idx, int node)
{
	/* [한국어] hctx = 최종 선택될 객체(재사용 또는 새 할당), tmp = 순회용 커서. */
	struct blk_mq_hw_ctx *hctx = NULL, *tmp;

	/* reuse dead hctx first */
	/* [한국어] 폐기 목록을 보호하는 락. 큐 재구성은 여러 경로(핫플러그 콜백,
	 * sysfs를 통한 큐 수 변경, 컨트롤러 리셋)에서 동시에 시도될 수 있다. */
	spin_lock(&q->unused_hctx_lock);
	/* [한국어] 폐기된 hctx들을 훑으며 재사용 후보를 찾는다. */
	list_for_each_entry(tmp, &q->unused_hctx_list, hctx_list) {
		/* [한국어] 두 조건을 모두 만족해야 한다:
		 *   numa_node 일치 - 다른 노드의 메모리를 쓰면 이 하드웨어 큐를
		 *     담당할 CPU가 원격 노드 접근을 하게 되어 지연이 커진다. NVMe에서
		 *     SQ/CQ 링 버퍼와 hctx 자료구조가 같은 노드에 있어야 DMA와
		 *     완료 처리가 모두 로컬에서 끝난다.
		 *   is_reusable    - CPU 핫플러그 리스트에서 완전히 빠졌는가 */
		if (tmp->numa_node == node && blk_mq_hctx_is_reusable(tmp)) {
			hctx = tmp;
			break;
		}
	}
	/* [한국어] 후보를 찾았으면 폐기 목록에서 떼어낸다. _init 변형이라 노드가
	 * 자기 자신을 가리키는 초기 상태로 되돌아가, 나중에 다시 폐기될 때
	 * 안전하게 재삽입할 수 있다. */
	if (hctx)
		list_del_init(&hctx->hctx_list);
	spin_unlock(&q->unused_hctx_lock);	/* [한국어] 목록 조작이 끝나는 즉시 락을 놓는다.
						 * 아래 할당·초기화는 잠들 수 있어 스핀락을 쥔 채로는 할 수 없다. */

	/* [한국어] 재사용할 것이 없으면 새로 할당한다. 지정된 NUMA 노드에서
	 * hctx 본체와 per-CPU 카운터, ctx 매핑 배열 등을 확보한다. */
	if (!hctx)
		hctx = blk_mq_alloc_hctx(q, set, node);
	/* [한국어] 새 할당마저 실패 — 메모리 부족. 호출자가 다른 노드로 폴백한다. */
	if (!hctx)
		goto fail;

	/* [한국어] hctx를 이 큐의 hctx_idx번 하드웨어 큐로 초기화한다. 내부에서
	 * set->ops->init_hctx 콜백이 호출되는데, NVMe PCIe에서는 nvme_init_hctx()가
	 * 실행되어 hctx->driver_data에 해당 nvme_queue 포인터를 연결한다.
	 * 이후 nvme_queue_rq()가 그 포인터로 SQ 링과 doorbell에 접근한다. */
	if (blk_mq_init_hctx(q, set, hctx, hctx_idx))
		goto free_hctx;

	return hctx;	/* [한국어] 이 시점의 hctx 는 드라이버 콜백까지 끝나 즉시 요청을 받을 수 있는 상태다 */

 free_hctx:
	/* [한국어] 초기화 실패 — kobject 참조를 놓아 hctx를 해제한다. hctx는
	 * sysfs에 노출되는 kobject를 품고 있어 kfree가 아니라 kobject_put으로
	 * 참조 계수를 통해 해제해야 한다. */
	kobject_put(&hctx->kobj);
 fail:
	return NULL;	/* [한국어] 두 실패 경로 모두 NULL 하나로 수렴한다. 호출자
			 * (__blk_mq_realloc_hw_ctxs)는 이것을 보고 다른 NUMA 노드로
			 * 폴백하거나, 확장을 포기하고 기존 큐 수를 유지한다. */
}

/*
 * [한국어]
 * __blk_mq_realloc_hw_ctxs - 큐의 hctx 배열을 tag set의 하드웨어 큐 수에 맞게 재구성
 *
 * @set: 목표 상태를 담은 tag set. set->nr_hw_queues가 "있어야 할 하드웨어 큐 수"다.
 * @q:   재구성할 request_queue. q->nr_hw_queues가 "현재 개수"다.
 * @return: 없음. 실패해도 기존 상태를 유지하며 조용히 돌아간다.
 *
 * === 언제 호출되는가 (NVMe 관점) ===
 * 1) 큐 최초 생성: nvme_alloc_io_tag_set() → blk_mq_init_allocated_queue()
 * 2) 하드웨어 큐 수 변경: 컨트롤러 리셋 후 재협상한 큐 개수가 달라졌거나,
 *    CPU 핫플러그로 매핑이 바뀌었을 때 nvme_reset_work()가
 *    blk_mq_update_nr_hw_queues()를 호출한다. NVMe는 Set Features의
 *    Number of Queues(FID 0x07)로 컨트롤러와 큐 개수를 협상하는데, 이 결과가
 *    부팅 때와 리셋 후에 다를 수 있다.
 *
 * === 세 단계로 진행 ===
 * 1) 배열 확장 — 큐가 늘어났다면 포인터 배열 자체를 더 크게 다시 할당한다.
 * 2) 각 슬롯 재초기화 — 0..nr_hw_queues-1의 hctx를 (필요하면 NUMA 노드를 바꿔가며)
 *    새로 만든다.
 * 3) 남는 슬롯 정리 — 큐가 줄어들었거나 확장에 실패했다면 뒤쪽을 해제한다.
 *
 * === RCU가 쓰이는 이유 ===
 * q->queue_hw_ctx 배열은 dispatch 경로에서 락 없이 읽힌다(queue_for_each_hw_ctx).
 * 재할당 중에 옛 배열을 바로 kfree하면 그 순간 읽고 있던 CPU가 해제된 메모리를
 * 참조한다. 그래서 새 포인터는 rcu_assign_pointer()로 게시하고 옛 배열은
 * kfree_rcu로 유예 기간 후 해제한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 큐가 freeze된 상태에서 호출되어야 한다
 * (진행 중인 I/O가 없어야 hctx를 안전하게 해체할 수 있다).
 *
 * 에러 경로: 메모리 부족 시 배열 확장을 포기하고 그냥 반환하거나, 일부 hctx
 * 생성에 실패하면 성공한 개수만큼만 유지하고 q->nr_hw_queues를 올리지 않는다.
 * 큐 개수가 목표보다 적어도 동작에는 지장이 없다는 점을 이용한 우아한 저하다.
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue / blk_mq_update_nr_hw_queues
 *     → blk_mq_realloc_hw_ctxs → [__blk_mq_realloc_hw_ctxs]
 *       → blk_mq_alloc_and_init_hctx / blk_mq_exit_hctx
 */
static void __blk_mq_realloc_hw_ctxs(struct blk_mq_tag_set *set,
				     struct request_queue *q)
{
	/* [한국어] i = 재초기화 루프 인덱스(성공한 개수를 나타내며 아래 판정에 쓰인다),
	 * j/end = 정리 루프의 범위. */
	int i, j, end;
	/* [한국어] 현재 hctx 포인터 배열. 최초 생성 시에는 NULL일 수 있다. */
	struct blk_mq_hw_ctx **hctxs = q->queue_hw_ctx;

	/* [한국어] 1단계 — 하드웨어 큐가 늘어나는 경우에만 배열을 다시 할당한다.
	 * 줄어드는 경우는 배열을 그대로 두고 뒤쪽만 비우므로 재할당이 불필요하다
	 * (메모리를 조금 낭비하지만 재할당 위험을 피하는 선택). */
	if (q->nr_hw_queues < set->nr_hw_queues) {
		struct blk_mq_hw_ctx **new_hctxs;

		/* [한국어] 새 크기의 배열을 tag set의 NUMA 노드에 0으로 초기화해 할당한다.
		 * kcalloc이므로 새로 늘어난 슬롯은 NULL로 시작한다. */
		new_hctxs = kcalloc_node(set->nr_hw_queues,
				       sizeof(*new_hctxs), GFP_KERNEL,
				       set->numa_node);
		/* [한국어] 메모리 부족 — 확장을 포기하고 기존 큐 수를 유지한 채 반환한다.
		 * 큐가 적어도 I/O는 정상 동작하므로 실패를 상위로 전파하지 않는다. */
		if (!new_hctxs)
			return;
		/* [한국어] 기존 포인터들을 새 배열 앞부분으로 복사한다. hctx 객체 자체는
		 * 그대로 두고 포인터만 옮기는 것이라, 이 시점에는 두 배열이 같은 객체를
		 * 가리킨다. */
		if (hctxs)
			memcpy(new_hctxs, hctxs, q->nr_hw_queues *
			       sizeof(*hctxs));
		/* [한국어] 새 배열을 게시한다. rcu_assign_pointer는 대입 전에 메모리
		 * 배리어를 넣어, 다른 CPU가 포인터를 보는 시점에 배열 내용이 이미
		 * 완성되어 있음을 보장한다(memcpy가 재배치되어 뒤로 밀리는 것을 막는다). */
		rcu_assign_pointer(q->queue_hw_ctx, new_hctxs);
		/*
		 * Make sure reading the old queue_hw_ctx from other
		 * context concurrently won't trigger uaf.
		 */
		/* [한국어] 옛 배열은 즉시 해제하지 않고 RCU 유예 기간 후 해제한다.
		 * 지금 이 순간에도 다른 CPU가 dispatch 경로에서 옛 포인터로 배열을
		 * 읽고 있을 수 있기 때문이다(use-after-free 방지).
		 * _mightsleep 변형은 이 함수가 잠들 수 있는 컨텍스트임을 이용해
		 * 별도의 rcu_head 없이 동기 대기로 처리할 수 있게 한다. */
		kfree_rcu_mightsleep(hctxs);
		/* [한국어] 이후 루프가 새 배열을 대상으로 동작하도록 지역 포인터 갱신. */
		hctxs = new_hctxs;
	}

	/* [한국어] 2단계 — 0번부터 목표 개수까지 각 슬롯의 hctx를 재구성한다. */
	for (i = 0; i < set->nr_hw_queues; i++) {
		int old_node;
		/* [한국어] 이 하드웨어 큐를 어느 NUMA 노드에 둘지 계산한다.
		 * blk_mq_get_hctx_node()는 큐 매핑 테이블에서 이 큐를 담당할 CPU들을
		 * 찾아 그들의 노드를 고른다. NVMe에서 큐 매핑은
		 * blk_mq_map_hw_queues()(block/blk-mq-cpumap.c)가 MSI-X 벡터의 affinity를 그대로 따라
		 * 만들었으므로, 결과적으로 "인터럽트를 받는 CPU가 있는 노드"가 된다.
		 * 이 정렬 덕분에 커맨드 제출·완료·자료구조 접근이 모두 같은 노드에서
		 * 일어난다. */
		int node = blk_mq_get_hctx_node(set, i);
		struct blk_mq_hw_ctx *old_hctx = hctxs[i];

		if (old_hctx) {
			/* [한국어] 기존 hctx가 있으면 노드를 기억해 두고(폴백용) 해체한다.
			 * blk_mq_exit_hctx()는 set->ops->exit_hctx 가 등록돼 있으면 그것을
			 * 부르고 hctx를 unused_hctx_list로 보낸다. NVMe PCIe 의 nvme_mq_ops
			 * 에는 exit_hctx 가 없어(init_hctx 만 등록) 이 콜백은 건너뛴다 —
			 * hctx->driver_data 가 가리키던 nvme_queue 는 hctx 와 수명이 다르고
			 * 드라이버가 따로 관리하기 때문이다. */
			old_node = old_hctx->numa_node;
			blk_mq_exit_hctx(q, set, old_hctx, i);
		}

		/* [한국어] 계산된 노드에 hctx를 새로 만든다(재사용 가능하면 재사용). */
		hctxs[i] = blk_mq_alloc_and_init_hctx(set, q, i, node);
		if (!hctxs[i]) {
			/* [한국어] 실패 — 원래 hctx가 없었다면(순수 확장 중이었다면)
			 * 여기서 멈춘다. i가 성공한 개수를 담은 채 루프를 빠져나가
			 * 아래에서 "확장 실패"로 처리된다. */
			if (!old_hctx)
				break;
			/* [한국어] 원래 hctx가 있었다면 상황이 다르다 — 이미 해체했으므로
			 * 이 슬롯을 비워 둘 수 없다(배열에 구멍이 생기면 dispatch가 깨진다).
			 * 목표 노드에 메모리가 없을 뿐이므로, 원래 노드로 폴백해 재시도한다.
			 * 성능은 조금 손해지만 동작은 보장된다. */
			pr_warn("Allocate new hctx on node %d fails, fallback to previous one on node %d\n",
					node, old_node);
			hctxs[i] = blk_mq_alloc_and_init_hctx(set, q, i,
					old_node);
			/* [한국어] 폴백마저 실패하면 배열에 구멍이 남아 이후 dispatch가
			 * NULL 역참조를 일으킨다. 심각한 상황이므로 경고를 남긴다
			 * (ONCE라 반복 로그는 없다). */
			WARN_ON_ONCE(!hctxs[i]);
		}
	}
	/*
	 * Increasing nr_hw_queues fails. Free the newly allocated
	 * hctxs and keep the previous q->nr_hw_queues.
	 */
	/* [한국어] 3단계 — 정리 범위를 정한다. 두 시나리오로 갈린다. */
	if (i != set->nr_hw_queues) {
		/* [한국어] 루프가 중간에 break했다 = 확장 실패. 이 경우 큐 개수를
		 * 늘리지 않고(q->nr_hw_queues 그대로) 이번에 새로 만든 것들
		 * (q->nr_hw_queues ~ i 구간)을 되돌린다. 어중간하게 늘어난 상태보다
		 * 원래 상태를 유지하는 편이 안전하다. */
		j = q->nr_hw_queues;
		end = i;
	} else {
		/* [한국어] 목표까지 전부 성공. 이제 남는 뒤쪽(i ~ 기존 개수)을 정리한다.
		 * 큐가 줄어든 경우 이 구간이 비어 있지 않다. */
		j = i;
		end = q->nr_hw_queues;
		/* [한국어] 큐 개수를 목표값으로 확정한다. 이 대입 이후부터
		 * queue_for_each_hw_ctx() 같은 순회가 새 개수를 기준으로 동작한다. */
		q->nr_hw_queues = set->nr_hw_queues;
	}

	/* [한국어] 정해진 범위의 hctx를 해체하고 슬롯을 비운다. */
	for (; j < end; j++) {
		struct blk_mq_hw_ctx *hctx = hctxs[j];

		if (hctx) {
			/* [한국어] hctx를 해체해 unused_hctx_list로 보낸다(즉시 해제가
			 * 아니라 재사용 풀로 반납). */
			blk_mq_exit_hctx(q, set, hctx, j);
			/* [한국어] 배열 슬롯을 NULL로 비워, 이후 순회가 죽은 hctx를
			 * 만지지 않게 한다. */
			hctxs[j] = NULL;
		}
	}
}

/*
 * [한국어]
 * blk_mq_realloc_hw_ctxs - hctx 배열 재구성 + CPU 핫플러그 콜백 등록 갱신
 *
 * @set: 목표 하드웨어 큐 수를 담은 tag set
 * @q:   재구성할 request_queue
 * @return: 없음
 *
 * __blk_mq_realloc_hw_ctxs()를 감싸면서 CPU 핫플러그 등록 정리까지 책임지는
 * 얇은 래퍼다. 두 단계를 분리한 이유는 순서가 중요하기 때문이다:
 * 배열 재구성이 끝나기 전에 핫플러그 콜백을 등록하면, 아직 초기화되지 않은
 * hctx로 콜백이 들어올 수 있다. 그래서 반드시 재구성 → 해제 → 등록 순으로
 * 진행한다.
 *
 * 해제를 등록보다 먼저 하는 이유도 같다. hctx는 재사용될 수 있으므로,
 * 옛 등록을 먼저 지워야 blk_mq_hctx_is_reusable()이 참이 되어 재사용 판정이
 * 올바르게 동작한다.
 *
 * NVMe에서 CPU 핫플러그 처리가 중요한 이유: CPU가 오프라인되면 그 CPU에
 * 배정된 하드웨어 큐로 향하던 I/O를 다른 큐로 옮겨야 하고, 그 큐에 이미
 * 제출된 커맨드는 완료를 기다려야 한다. blk_mq_hctx_notify_dead()가 그
 * 재배치를 수행한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐가 freeze된 상태.
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue / blk_mq_update_nr_hw_queues
 *     → [blk_mq_realloc_hw_ctxs] → __blk_mq_realloc_hw_ctxs
 *       → blk_mq_remove_hw_queues_cpuhp → blk_mq_add_hw_queues_cpuhp
 */
static void blk_mq_realloc_hw_ctxs(struct blk_mq_tag_set *set,
				   struct request_queue *q)
{
	/* [한국어] 1) hctx 배열 자체를 목표 개수에 맞게 재구성한다. */
	__blk_mq_realloc_hw_ctxs(set, q);

	/* unregister cpuhp callbacks for exited hctxs */
	/* [한국어] 2) 위에서 해체된 hctx들의 핫플러그 등록을 해제한다. 이걸 먼저
	 * 해야 그 hctx들이 unused 풀에서 재사용 가능 상태가 된다. */
	blk_mq_remove_hw_queues_cpuhp(q);

	/* register cpuhp for new initialized hctxs */
	/* [한국어] 3) 새로 초기화된 hctx들을 핫플러그 알림 목록에 등록한다. 이제부터
	 * CPU 온/오프라인 전환 시 blk_mq_hctx_notify_online/dead 콜백이 이 hctx들에
	 * 대해 호출되어 I/O 재배치가 이루어진다. */
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
	/* [한국어] 드라이버의 연산 테이블을 연결한다. NVMe PCIe에서는
	 * drivers/nvme/host/pci.c의 nvme_mq_ops이며, 이 안의 queue_rq가
	 * 곧 nvme_queue_rq다. 영문 주석대로 "가능한 한 빨리" 설정하는 이유는
	 * queue_is_mq()가 이 필드로 판정하기 때문이다 — 아래 초기화 함수들이
	 * 그 판정에 의존한다. */
	q->mq_ops = set->ops;

	/*
	 * ->tag_set has to be setup before initialize hctx, which cpuphp
	 * handler needs it for checking queue mapping
	 */
	/* [한국어] tag set 연결. 영문 주석이 밝힌 순서 제약이 중요하다:
	 * 아래 blk_mq_realloc_hw_ctxs()가 hctx를 만들면서 CPU 핫플러그 콜백을
	 * 등록하는데, 그 콜백이 즉시 실행될 수 있고 실행되면 q->tag_set으로
	 * 큐 매핑을 확인한다. 먼저 설정하지 않으면 NULL 역참조가 된다. */
	q->tag_set = set;

	/* [한국어] CPU마다 하나씩인 소프트웨어 큐(ctx)를 할당한다. 실패하면
	 * 아직 아무것도 만들지 않았으므로 err_exit로 바로 나간다. */
	if (blk_mq_alloc_ctxs(q))
		goto err_exit;

	/* init q->mq_kobj and sw queues' kobjects */
	/* [한국어] /sys/block/<disk>/mq/ 아래에 노출될 kobject들을 초기화한다.
	 * 실제 sysfs 등록은 나중에 blk_register_queue()에서 이뤄지고, 여기서는
	 * 구조체만 준비한다. */
	blk_mq_sysfs_init(q);

	/* [한국어] 해체된 hctx를 재사용하기 위해 보관하는 목록과 그 보호 락.
	 * hctx는 per-CPU 자료구조를 포함해 할당 비용이 크므로, 큐 재구성 시
	 * 버리지 않고 여기 모았다가 같은 NUMA 노드의 요청에 되돌려 준다. */
	INIT_LIST_HEAD(&q->unused_hctx_list);
	spin_lock_init(&q->unused_hctx_lock);	/* [한국어] 이 락은 목록의 삽입·제거만 보호한다.
						 * 보호 구간이 짧고 인터럽트 문맥에서는 잡지 않으므로 irqsave 가 필요 없다. */

	/* [한국어] 하드웨어 컨텍스트 배열을 만든다. NVMe에서 hctx 하나가
	 * nvme_queue 하나(SQ/CQ 쌍 + MSI-X 벡터)에 대응하며, 내부에서
	 * set->ops->init_hctx == nvme_init_hctx가 호출되어 그 연결이 맺어진다. */
	blk_mq_realloc_hw_ctxs(set, q);
	/* [한국어] 하나도 못 만들었으면 이 큐로는 I/O를 할 수 없다. */
	if (!q->nr_hw_queues)
		goto err_hctxs;

	/* [한국어] ★ 타임아웃 인프라 ★
	 * blk_mq_timeout_work는 주기적으로 깨어나 deadline이 지난 request를
	 * 찾고, 발견하면 q->mq_ops->timeout(NVMe: nvme_timeout)을 호출한다.
	 * 그 콜백이 CSTS 확인 → Abort 커맨드 → 컨트롤러 리셋 순으로 복구를
	 * 시도한다. 즉 이 한 줄이 NVMe 에러 복구의 출발점을 설치하는 것이다. */
	INIT_WORK(&q->timeout_work, blk_mq_timeout_work);
	/* [한국어] 기본 타임아웃을 설정한다. 드라이버가 set->timeout을 지정했으면
	 * 그 값을, 아니면 30초를 쓴다. NVMe는 io_timeout 모듈 파라미터(기본 30초)를
	 * 넘기므로 결국 같은 값이 되는 경우가 많다.
	 * 이 값이 너무 짧으면 정상적으로 느린 I/O(대용량 discard 등)까지
	 * 타임아웃되어 불필요한 컨트롤러 리셋을 유발한다. */
	blk_queue_rq_timeout(q, set->timeout ? set->timeout : 30 * HZ);

	/* [한국어] blk-mq 큐의 기본 플래그 묶음(통계 수집, I/O 폴링 허용 등)을
	 * 한 번에 켠다. */
	q->queue_flags |= QUEUE_FLAG_MQ_DEFAULT;

	/* [한국어] ★ requeue 인프라 ★
	 * 드라이버가 BLK_STS_RESOURCE로 요청을 거부하면 그 request를
	 * requeue_list에 넣고 이 지연 워크가 나중에 다시 제출한다.
	 * NVMe PCIe 에서 이 상태가 나오는 실제 지점은 nvme_map_data() 계열이다 —
	 * PRP 리스트/SGL 디스크립터 페이지를 담는 dma_pool 이 고갈되거나 DMA 매핑에
	 * 실패하면 BLK_STS_RESOURCE 를 돌려준다. "SQ 가 꽉 차서"는 원인이 될 수
	 * 없다: 드라이버 태그 수가 곧 SQ 깊이라, 태그를 받은 시점에 이미 슬롯이
	 * 확보되어 있기 때문이다.
	 * DELAYED_WORK인 이유: 즉시 재시도하면 같은 이유로 다시 실패할 가능성이
	 * 높아, 약간의 지연을 두어 자원이 회복될 시간을 준다. */
	INIT_DELAYED_WORK(&q->requeue_work, blk_mq_requeue_work);
	/* [한국어] blk-flush 상태 기계가 쓰는 리스트 — FLUSH/FUA 요청이 여기서
	 * 단계별로 관리된다. */
	INIT_LIST_HEAD(&q->flush_list);
	/* [한국어] 재제출 대기 중인 request 목록과 그 보호 락. 완료 경로(IRQ)와
	 * 워커가 동시에 만지므로 스핀락이 필요하다. */
	INIT_LIST_HEAD(&q->requeue_list);
	spin_lock_init(&q->requeue_lock);	/* [한국어] requeue 는 완료 경로에서도 일어날 수 있다
						 * (예: 드라이버가 BLK_STS_RESOURCE 를 돌려주거나 nvme_cancel_request 가
						 * 요청을 되돌릴 때). 그래서 이 락을 잡는 쪽은 irqsave 계열을 쓴다. */

	/* [한국어] 큐가 담을 수 있는 request 수 = 드라이버 태그 개수.
	 * NVMe PCIe 에서 이 값의 출처는 nvme_alloc_io_tag_set() 의
	 * set->queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1) 이고,
	 * 그것이 다시 dev->q_depth(I/O 큐 하나의 링 엔트리 수)에서 유도된다.
	 * 곧 이 큐에서 동시에 진행 가능한 커맨드 수이자 쓸 수 있는 태그의 수다.
	 * /sys/block/nvme0n1/queue/nr_requests로 조회·조정할 수 있다. */
	q->nr_requests = set->queue_depth;
	/* [한국어] 비동기 요청 전용 상한. 스케줄러가 없는 상태에서는 제한할
	 * 이유가 없으므로 전체 깊이와 같게 둔다. 스케줄러를 붙이면
	 * elevator_switch()가 더 낮은 값으로 조정한다. */
	q->async_depth = set->queue_depth;

	/* [한국어] 각 CPU의 소프트웨어 큐를 초기화한다(리스트 헤드, CPU 번호 등). */
	blk_mq_init_cpu_queues(q, set->nr_hw_queues);
	/* [한국어] CPU ↔ 하드웨어 큐 매핑을 실제로 구축한다. NVMe에서는
	 * MSI-X affinity를 따라가므로, 이 호출 이후 "제출 CPU = 완료 CPU"
	 * 정렬이 성립한다. */
	blk_mq_map_swqueue(q);
	/* [한국어] 이 큐를 tag set의 공유 목록에 등록한다. 같은 컨트롤러의
	 * 다른 네임스페이스가 이미 등록되어 있으면, 이 시점에 양쪽 모두
	 * BLK_MQ_F_TAG_QUEUE_SHARED로 전환되어 태그를 공평하게 나눠 쓰게 된다. */
	blk_mq_add_queue_tag_set(set, q);
	return 0;

err_hctxs:
	blk_mq_release(q);	/* [한국어] 부분적으로 만들어진 hctx 배열과 부속 자료구조를 해제한다 */
err_exit:
	q->mq_ops = NULL;	/* [한국어] mq_ops 가 NULL 이라는 것이 "이 큐는 blk-mq 로 초기화되지 않았다"는 표식이다.
				 * 호출자의 정리 경로가 이 값을 보고 blk-mq 쪽 해제를 건너뛴다. */
	return -ENOMEM;
}
EXPORT_SYMBOL(blk_mq_init_allocated_queue);

/* tags can _not_ be used after returning from blk_mq_exit_queue */
/*
 * [한국어]
 * blk_mq_exit_queue - request_queue 의 blk-mq 자원 해제
 *
 * @q: 정리할 request_queue
 *
 * 모든 hctx(NVMe SQ) 를 종료하고 tag_set list 에서 제거.
 * blk_mq_exit_hw_queues 는 BLK_MQ_F_TAG_QUEUE_SHARED 를 검사.
 * blk_mq_del_queue_tag_set 은 BLK_MQ_F_TAG_QUEUE_SHARED 를 클리어할 수 있음.
 */
void blk_mq_exit_queue(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;

	/* Checks hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED. */
	/* [한국어] 모든 hctx 를 종료하고 cpuhp 콜백 해제 */
	blk_mq_exit_hw_queues(q, set, set->nr_hw_queues);
	/* May clear BLK_MQ_F_TAG_QUEUE_SHARED in hctx->flags. */
	/* [한국어] tag_set list 에서 이 queue 제거 (unshared 전환 포함) */
	blk_mq_del_queue_tag_set(q);
}

/*
 * [한국어]
 * __blk_mq_alloc_rq_maps - 모든 hctx 에 tag/request map 할당
 *
 * @set: blk_mq_tag_set
 * @return: 0 성공, -ENOMEM 실패
 *
 * shared_tags 면 공유 pool 을 먼저 할당하고, 각 hctx 에 연결.
 * 실패 시 이미 할당된 것을 역순 해제.
 */
static int __blk_mq_alloc_rq_maps(struct blk_mq_tag_set *set)
{
	int i;

	if (blk_mq_is_shared_tags(set->flags)) {
		/* [한국어] 공유 구성에서는 pool 이 tag set 에 하나뿐이다. 특정 hctx 에
		 * 속하지 않는다는 뜻으로 BLK_MQ_NO_HCTX_IDX 를 인덱스 자리에 넘긴다 —
		 * 그 값은 실제 인덱스가 아니라 "인덱스 없음"을 나타내는 표식이다. */
		set->shared_tags = blk_mq_alloc_map_and_rqs(set,
						BLK_MQ_NO_HCTX_IDX,
						set->queue_depth);
		if (!set->shared_tags)
			return -ENOMEM;	/* [한국어] 아직 아무 hctx 도 손대지 않았으므로 바로 반환 */
	}

	for (i = 0; i < set->nr_hw_queues; i++) {
		/* [한국어] 큐마다 sbitmap + request 배열을 만든다(공유 구성이면 포인터만 연결).
		 * 여기가 blk-mq 에서 가장 큰 메모리를 잡는 곳이다:
		 * queue_depth × 큐 수 × (request + 드라이버 PDU) 만큼을 **미리** 할당한다.
		 * NVMe 라면 PDU 가 nvme_request + nvme_iod 이고, 깊이 1024 에 큐 32개면
		 * 수십 MB 에 이른다. 이렇게 선할당하는 대가로 I/O 경로에서는
		 * 메모리 할당이 전혀 일어나지 않는다 — blk-mq 의 핵심 설계다. */
		if (!__blk_mq_alloc_map_and_rqs(set, i))
			goto out_unwind;
		cond_resched();	/* [한국어] 큐가 수십 개면 이 루프가 길다. 반복마다 양보 지점을 준다 */
	}

	return 0;

out_unwind:
	/* [한국어] --i 로 시작하는 이유: 진입 시 i 는 실패한 인덱스이고 그것은
	 * 할당되지 않았으므로 하나 앞부터 되감는다. 여기서는 하한이 0 이다 —
	 * 이 함수가 만든 것이 전부이므로 전부 되돌려야 한다
	 * (증설 경로인 blk_mq_prealloc_tag_set_tags 와 대비된다). */
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
/*
 * [한국어]
 * blk_mq_alloc_set_map_and_rqs - tag set 의 tag pool 을 메모리 상황에 맞게(필요하면 깊이를 줄여 가며) 할당
 *
 * @set: blk_mq_tag_set
 * @return: 0 성공, -ENOMEM 실패
 *
 * 요청된 queue_depth 로 시도하다 실패하면 절반씩 줄여 재시도.
 * reserved_tags + BLK_MQ_TAG_MIN 이하로 줄어들면 완전 실패.
 * 성공 시 set->queue_depth 가 실제 할당된 깊이로 갱신됨.
 */
static int blk_mq_alloc_set_map_and_rqs(struct blk_mq_tag_set *set)
{
	unsigned int depth;
	int err;

	/* [한국어] 요청 depth 기억: 실제 할당과 비교하여 축소 경고 출력 */
	depth = set->queue_depth;
	do {
		err = __blk_mq_alloc_rq_maps(set);
		if (!err)
			break;	/* [한국어] 요청한 깊이로 성공 — 대부분의 경우 첫 시도에서 여기로 빠진다 */

		/* [한국어] 메모리가 모자라면 **실패로 끝내지 않고 깊이를 절반으로 줄여
		 * 다시 시도한다.** 이 함수가 부팅·리셋 경로에서 불리기 때문이다 —
		 * 여기서 -ENOMEM 을 내면 장치 자체를 쓸 수 없게 된다. 깊이가 얕아지면
		 * 동시 진행 IO 수가 줄어 성능은 떨어지지만 장치는 정상 동작한다.
		 * "느린 장치"가 "없는 장치"보다 낫다는 판단이다. */
		set->queue_depth >>= 1;
		/* [한국어] 다만 무한정 줄일 수는 없다. 예약 영역을 떼고 나서도 일반
		 * 요청이 쓸 tag 가 최소한 남아야 하며, 그 아래로는 큐가 있으나마나다. */
		if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN) {
			err = -ENOMEM;
			break;
		}
	} while (set->queue_depth);	/* [한국어] >>= 1 이 0 에 닿으면 루프도 끝난다(이중 안전장치) */

	if (!set->queue_depth || err) {
		pr_err("blk-mq: failed to allocate request map\n");	/* [한국어] 절반씩 줄여도 안 됐다 — 진짜 메모리 고갈 */
		return -ENOMEM;
	}

	if (depth != set->queue_depth)
		pr_info("blk-mq: reduced tag depth (%u -> %u)\n",	/* [한국어] 성공했지만 요청보다 얕다는 사실을 반드시 알린다.
								 * 나중에 이 장치의 성능이 기대에 못 미칠 때 dmesg 의 이 한 줄이 원인을 알려 준다 */
						depth, set->queue_depth);

	return 0;
}

/*
 * [한국어]
 * blk_mq_update_queue_map - CPU → 하드웨어 큐 매핑 테이블 갱신
 *
 * @set: blk_mq_tag_set
 *
 * 드라이버의 map_queues 콜백 또는 기본 blk_mq_map_queues 를 호출하여
 * set->map[type].mq_map[cpu] 배열을 채운다.
 * 단일 map 이면 DEFAULT nr_queues 를 nr_hw_queues 로 설정.
 * 재매핑 시 stale 매핑 제거를 위해 먼저 테이블을 초기화.
 */
static void blk_mq_update_queue_map(struct blk_mq_tag_set *set)
{
	/*
	 * blk_mq_map_queues() and multiple .map_queues() implementations
	 * expect that set->map[HCTX_TYPE_DEFAULT].nr_queues is set to the
	 * number of hardware queues.
	 */
	if (set->nr_maps == 1)
		/* [한국어] 맵이 하나면 그 맵이 전체 큐를 다 가진다. 위 영문 주석대로
		 * 매핑 구현들이 이 값을 미리 채워진 것으로 가정하므로 여기서 넣어 준다. */
		set->map[HCTX_TYPE_DEFAULT].nr_queues = set->nr_hw_queues;

	if (set->ops->map_queues) {
		int i;	/* [한국어] 맵 종류 순회 인덱스 */

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
			/* [한국어] 반드시 먼저 지워야 한다. 위 영문 주석이 설명하듯
			 * 드라이버 구현은 "자기가 담당할 CPU 에만 쓰는" 방식이라, 어떤 CPU 가
			 * 이번에 어느 큐에도 배정되지 않으면 옛 값이 그대로 남는다.
			 * 그 CPU 는 이제 존재하지 않는 큐 번호를 가리키게 된다. */
			blk_mq_clear_mq_map(&set->map[i]);

		/* [한국어] 드라이버 콜백에 실제 매핑을 맡긴다. NVMe PCIe 는
		 * nvme_pci_map_queues() 에서 맵 종류(DEFAULT/READ/POLL)마다
		 *   map->nr_queues  = dev->io_queues[i];   // 타입별 큐 수
		 *   map->queue_offset = qoff;              // 전역 hctx 번호 시작점
		 *   blk_mq_map_hw_queues(map, dev->dev, offset);  // PCI IRQ affinity 반영
		 * 를 수행한다(pci.c:956~969). POLL 맵만 affinity 를 따르지 않는데,
		 * 폴링 큐는 인터럽트를 쓰지 않기 때문이다.
		 * affinity 를 그대로 따르는 결과가 "제출 CPU == 완료 인터럽트 CPU" 정렬이며,
		 * 이것이 NVMe 성능에서 캐시 지역성과 IPI 회피를 동시에 얻는 근거다. */
		set->ops->map_queues(set);
	} else {
		/* [한국어] 콜백이 없는 드라이버는 blk-mq 의 기본 매핑을 쓴다.
		 * 기본 매핑은 맵 하나만 다룰 수 있으므로 nr_maps > 1 은 구성 오류다. */
		BUG_ON(set->nr_maps > 1);
		blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);
	}
}

/*
 * [한국어]
 * blk_mq_prealloc_tag_set_tags - HW queue 수 증가를 위한 새 tag 배열 사전 할당
 *
 * @set:             blk_mq_tag_set
 * @new_nr_hw_queues: 늘릴 HW queue 목표 수
 * @return: 새 tags[] 배열 포인터, 증가 불필요 시 NULL, 실패 시 ERR_PTR
 *
 * __blk_mq_update_nr_hw_queues 에서 호출.
 * 기존 tags[] 를 복사한 후 새 SQ 를 위한 tag/request pool 을 추가 할당.
 * shared_tags 이면 포인터만 복사, 아니면 새로 할당.
 * 실패 시 새로 할당한 것만 역순 해제하고 kfree.
 */
static struct blk_mq_tags **blk_mq_prealloc_tag_set_tags(
				struct blk_mq_tag_set *set,
				int new_nr_hw_queues)
{
	struct blk_mq_tags **new_tags;	/* [한국어] 새로 만들 tags[] 배열. 성공 시 호출자가 옛 배열을 교체한다 */
	int i;				/* [한국어] 순회 인덱스. out_unwind 가 이 값을 그대로 받아 되감기 시작점으로 쓰므로 루프 밖에 선언돼 있다 */

	/* [한국어] 큐 수가 줄거나 그대로면 배열을 키울 필요가 없다. NULL 은 에러가
	 * 아니라 "할 일 없음"이라는 신호이며, 호출자는 IS_ERR() 로만 실패를 가린다.
	 * 큐 수가 줄어드는 경우 남는 tags[] 항목은 호출자가 따로 정리한다. */
	if (set->nr_hw_queues >= new_nr_hw_queues)
		return NULL;

	/* [한국어] 새 배열: new_nr_hw_queues 크기로 할당 */
	new_tags = kcalloc_node(new_nr_hw_queues, sizeof(struct blk_mq_tags *),
				GFP_KERNEL, set->numa_node);
	if (!new_tags)
		return ERR_PTR(-ENOMEM);	/* [한국어] 아직 아무것도 손대지 않았으므로 옛 set->tags 는 그대로 유효하다 */

	/* [한국어] 기존 포인터들을 앞부분에 그대로 옮긴다. 여기서 중요한 것은
	 * **복사되는 것이 포인터뿐**이라는 점이다 — 기존 큐들의 tag sbitmap 과
	 * request 배열은 재할당되지 않고 그대로 살아 있다. 그래서 큐를 늘리는
	 * 동안에도 기존 큐에 떠 있던 요청들의 tags->rqs[] 참조가 깨지지 않는다. */
	if (set->tags)
		memcpy(new_tags, set->tags, set->nr_hw_queues *
		       sizeof(*set->tags));

	/* [한국어] 새로 늘어난 인덱스 구간만 채운다. NVMe 에서 이 경로가 도는
	 * 대표적인 상황은 컨트롤러 리셋 후 I/O 큐 수가 늘어난 경우로,
	 * nvme_pci_update_nr_queues() → blk_mq_update_nr_hw_queues() 를 타고 온다. */
	for (i = set->nr_hw_queues; i < new_nr_hw_queues; i++) {
		if (blk_mq_is_shared_tags(set->flags)) {
			/* [한국어] 공유 구성에서는 pool 이 tag set 에 하나뿐이므로
			 * 모든 인덱스가 같은 것을 가리키게 둔다 — 새로 할당할 것이 없다. */
			new_tags[i] = set->shared_tags;
		} else {
			/* [한국어] 큐마다 자기 pool 을 갖는 구성. sbitmap 과
			 * queue_depth 개의 request 객체를 새로 할당한다. 큐 하나당
			 * 수백 KB 가 들 수 있어 실패 가능성이 실재한다. */
			new_tags[i] = blk_mq_alloc_map_and_rqs(set, i,
					set->queue_depth);
			if (!new_tags[i])
				goto out_unwind;
		}
		cond_resched();	/* [한국어] 큐가 수십 개면 이 루프가 길어진다. 선점 불가 커널에서도
				 * 다른 태스크가 굶지 않도록 반복마다 양보 지점을 준다.
				 * 이 함수는 프로세스 컨텍스트에서만 불리므로 안전하다. */
	}

	return new_tags;	/* [한국어] 호출자가 set->tags 를 이것으로 갈아 끼우고 옛 배열을 해제한다 */
out_unwind:
	/* [한국어] 되감기 범위가 정확해야 한다. 해제 대상은 **이번에 새로 만든 것뿐**이고,
	 * memcpy 로 옮겨 온 기존 pool(인덱스 < set->nr_hw_queues)은 여전히 사용 중이라
	 * 절대 건드리면 안 된다. 그래서 하한이 0 이 아니라 set->nr_hw_queues 다.
	 * --i 로 시작하는 이유: goto 로 들어올 때 i 는 실패한 인덱스를 가리키고,
	 * 그 항목은 할당되지 않았으므로 하나 앞부터 되감아야 한다. */
	while (--i >= set->nr_hw_queues) {
		if (!blk_mq_is_shared_tags(set->flags))
			blk_mq_free_map_and_rqs(set, new_tags[i], i);
	}
	kfree(new_tags);		/* [한국어] 배열 자체도 해제 */
	return ERR_PTR(-ENOMEM);	/* [한국어] set->tags 는 손대지 않았으므로 호출자는 옛 구성 그대로 계속 쓸 수 있다 */
}

/*
 * Alloc a tag set to be associated with one or more request queues.
 * May fail with EINVAL for various error conditions. May adjust the
 * requested depth down, if it's too large. In that case, the set
 * value will be stored in set->queue_depth.
 */
/*
 * [한국어]
 * blk_mq_alloc_tag_set - blk-mq 태그 집합 초기화 — 이 컨트롤러/장치가 쓸 tag pool 전체를 만든다
 *
 * @set: 초기화할 blk_mq_tag_set (드라이버가 ops/nr_hw_queues/queue_depth 등 미리 설정)
 * @return: 0 성공, 음수 에러코드
 *
 * NVMe 드라이버는 nvme_alloc_io_tag_set 에서 이 함수를 호출.
 * nr_hw_queues = NVMe SQ 수, queue_depth = CID 최대 수 = NVMe SQ queue depth.
 * 초기화 순서:
 *   1) 유효성 검사 (nr_hw_queues, queue_depth, ops->queue_rq)
 *   2) SRCU 구조체 초기화 (tags_srcu, blocking srcu)
 *   3) tags[] 포인터 배열 할당
 *   4) map[type].mq_map[] CPU→SQ 매핑 테이블 할당
 *   5) queue map 초기화 (map_queues or 기본 round-robin)
 *   6) 모든 SQ 의 CID pool 및 request 객체 할당
 *   7) tag_list 초기화
 *
 * 호출 체인:
 *   nvme_alloc_io_tag_set → [blk_mq_alloc_tag_set]
 *   → blk_mq_update_queue_map → blk_mq_alloc_set_map_and_rqs
 */
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
{
	int i, ret;

	/* [한국어] BLK_MQ_MAX_DEPTH: sbitmap unique tag 비트 수에 맞는 최대값 검증 */
	BUILD_BUG_ON(BLK_MQ_MAX_DEPTH > 1 << BLK_MQ_UNIQUE_TAG_BITS);

	/* [한국어] 하드웨어 큐가 0개면 요청을 내보낼 곳이 없다 — 드라이버 구성 오류.
	 * NVMe 는 nvme_alloc_io_tag_set() 에서 set->nr_hw_queues = ctrl->queue_count - 1
	 * (admin 큐를 뺀 I/O 큐 수)로 채운다. 컨트롤러가 I/O 큐를 하나도 만들지
	 * 못했다면 여기까지 오기 전에 다른 곳에서 실패한다. */
	if (!set->nr_hw_queues)
		return -EINVAL;
	/* [한국어] 큐당 발급 가능한 tag 가 0이면 어떤 요청도 진행할 수 없다.
	 * NVMe: queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1). */
	if (!set->queue_depth)
		return -EINVAL;
	/* [한국어] 예약 영역을 떼고 나면 일반 요청이 쓸 tag 가 최소 BLK_MQ_TAG_MIN 은
	 * 남아야 한다. 그렇지 않으면 예약 요청은 되는데 일반 IO 는 영영 진행하지
	 * 못하는 큐가 만들어진다. NVMe PCIe 는 reserved_tags 가 0 이라 이 검사에
	 * 걸리지 않고, fabrics(connect 용 1개)나 Apple(admin 깊이만큼)이 대상이다. */
	if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN)
		return -EINVAL;

	/* [한국어] queue_rq 는 블록 계층이 드라이버에 닿는 유일한 통로다. 이것이
	 * 없으면 요청을 만들어도 내보낼 방법이 없으므로 필수. NVMe PCIe 에서는
	 * nvme_mq_ops.queue_rq = nvme_queue_rq 가 여기 들어온다. */
	if (!set->ops->queue_rq)
		return -EINVAL;

	/* [한국어] budget 은 "드라이버가 지금 요청을 더 받을 수 있는가"를 태그와
	 * 별개로 묻는 훅이다(SCSI 의 device queue depth 처럼). 획득만 있고 반납이
	 * 없으면 예산이 새고, 반납만 있고 획득이 없으면 있지도 않은 예산을 돌려주게
	 * 되므로 짝이 맞아야 한다. XOR 로 "한쪽만 있는" 경우를 잡아낸다.
	 * NVMe 는 두 콜백을 모두 두지 않는다 — 태그 수 자체가 곧 SQ 깊이라 별도
	 * 예산 개념이 필요 없기 때문이다. */
	if (!set->ops->get_budget ^ !set->ops->put_budget)
		return -EINVAL;

	/* [한국어] queue_depth 상한 클램프. BLK_MQ_MAX_DEPTH 는 blk-mq 가 태그 번호를
	 * 인코딩할 수 있는 폭(BLK_MQ_UNIQUE_TAG_BITS)에서 나온 한계다. NVMe 는
	 * 이 클램프에 걸리기 전에 드라이버 쪽에서 이미
	 * queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1) 로 잘라 넘긴다
	 * (sqsize 는 Identify/Set Features 로 정해진 SQ 크기). */
	if (set->queue_depth > BLK_MQ_MAX_DEPTH) {
		pr_info("blk-mq: reduced tag depth to %u\n",
			BLK_MQ_MAX_DEPTH);
		set->queue_depth = BLK_MQ_MAX_DEPTH;
	}

	/* [한국어] nr_maps: 하드웨어 큐를 몇 "종류"로 나눌 것인가(DEFAULT/READ/POLL).
	 * 드라이버가 지정하지 않았으면 1 = DEFAULT 하나뿐이라는 뜻이다.
	 *
	 * NVMe PCIe 접점 (drivers/nvme/host/pci.c 실물 확인): 리셋 때
	 * nvme_alloc_io_tag_set(..., nvme_pci_nr_maps(dev), ...) 로 넘어오며,
	 * nvme_pci_nr_maps() 는 io_queues[HCTX_TYPE_POLL] 이 있으면 3,
	 * io_queues[HCTX_TYPE_READ] 가 있으면 2, 아니면 1 을 돌려준다. 즉
	 * poll_queues / write_queues 모듈 파라미터가 0 이면 맵이 자동으로 줄어든다. */
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
		/* [한국어] 크래시 덤프 커널은 예약된 소량의 메모리만 쓴다. tag 하나당
		 * request 구조체 + 드라이버 PDU(NVMe 면 nvme_request + iod)가 미리
		 * 할당되므로, 깊이 1024 를 그대로 두면 그 자체로 수 MB 를 먹어 덤프에
		 * 실패할 수 있다. 성능은 무의미하고 덤프를 기록해 내는 것만이 목적이라
		 * 64 로 잘라 낸다. */
		set->queue_depth = min(64U, set->queue_depth);

	/*
	 * There is no use for more h/w queues than cpus if we just have
	 * a single map
	 */
	/* [한국어] 맵이 하나뿐이면 CPU 수보다 많은 하드웨어 큐는 놀게 된다 —
	 * 한 CPU 는 한 맵에서 하나의 hctx 만 쓰기 때문이다. 그래서 상한을 건다.
	 * 맵이 여러 개면 같은 CPU 가 DEFAULT/READ/POLL 각각 다른 큐를 쓰므로
	 * nr_hw_queues 가 nr_cpu_ids 를 넘어도 낭비가 아니다.
	 *
	 * NVMe PCIe 에서 nr_hw_queues 의 출처: nvme_alloc_io_tag_set() 이
	 * set->nr_hw_queues = ctrl->queue_count - 1 로 채운다(-1 은 admin 큐 제외).
	 * queue_count 는 Set Features 의 Number of Queues(nvme_set_queue_count)로
	 * 컨트롤러와 협상한 값과 실제 확보한 MSI-X 벡터 수의 최솟값에서 온다.
	 * 리셋 때마다 달라질 수 있어 nvme_pci_update_nr_queues() 가
	 * blk_mq_update_nr_hw_queues() 로 갱신한다. */
	if (set->nr_maps == 1 && set->nr_hw_queues > nr_cpu_ids)
		set->nr_hw_queues = nr_cpu_ids;

	/* [한국어] ★ BLK_MQ_F_BLOCKING과 SRCU ★
	 * 이 플래그는 "드라이버의 queue_rq가 잠들 수 있다"는 선언이다.
	 * 그러면 dispatch 경로를 일반 RCU로 보호할 수 없다 — RCU 읽기 구간
	 * 안에서는 잠들 수 없기 때문이다. 그래서 잠들 수 있는 SRCU
	 * (Sleepable RCU)를 대신 쓴다.
	 * NVMe 에서 이 플래그를 세우는 트랜스포트는 **TCP 하나뿐**이다
	 * (drivers/nvme/host/tcp.c:2879 의 .flags = NVME_F_FABRICS | NVME_F_BLOCKING).
	 * 소켓 전송이 잠들 수 있기 때문이다. PCIe/RDMA/FC 는 세우지 않는다 —
	 * nvme_queue_rq() 가 SQ 에 64바이트를 복사하고 doorbell 을 쓰는 것이
	 * 전부라 잠들 여지가 없다. core.c:5481 이 이 플래그를 보고
	 * set->flags |= BLK_MQ_F_BLOCKING 으로 옮겨 담는다.
	 * 이 SRCU가 보호하는 것은 "dispatch 실행 중"과 "큐 quiesce" 사이의
	 * 경쟁으로, blk_mq_quiesce_queue()가 SRCU 유예 기간을 기다려
	 * 실행 중인 queue_rq가 모두 끝났음을 보장한다. */
	if (set->flags & BLK_MQ_F_BLOCKING) {
		set->srcu = kmalloc_obj(*set->srcu);	/* [한국어] 필요한 트랜스포트에만 동적 할당한다.
							 * srcu_struct 는 CPU 마다 카운터 배열을 갖는 큰 구조체라,
							 * 대부분(PCIe 포함)이 쓰지 않는 것을 tag set 마다 박아 두면 낭비다. */
		if (!set->srcu)
			return -ENOMEM;			/* [한국어] 아직 아무것도 초기화하지 않았으므로 정리 없이 바로 반환 */
		ret = init_srcu_struct(set->srcu);	/* [한국어] per-CPU 카운터 배열까지 실제로 초기화 — 이 자체가 실패할 수 있다 */
		if (ret)
			goto out_free_srcu;		/* [한국어] 방금 kmalloc 한 것만 되돌린다 */
	}
	/* [한국어] tags_srcu는 위와 별개의 SRCU로, 태그 세트 자체의 교체를
	 * 보호한다. 완료 경로가 태그 번호로 request를 역참조하는 동안
	 * (blk_mq_tag_to_rq) 태그 배열이 해제되면 안 되기 때문이다.
	 * 하드웨어 큐 수가 바뀌어 태그 세트가 재구성될 때 이 SRCU의 유예
	 * 기간이 옛 배열의 해제를 미룬다. */
	ret = init_srcu_struct(&set->tags_srcu);	/* [한국어] 이쪽은 조건 없이 항상 필요하다 — 태그 배열 교체는 모든 드라이버에서 일어난다 */
	if (ret)
		goto out_cleanup_srcu;			/* [한국어] 위에서 만든 set->srcu 가 있다면 그것을 먼저 정리해야 한다 */

	/* [한국어] 하드웨어 큐 수 변경을 직렬화하는 rwsem. 읽기 잠금은
	 * 스케줄러 변경 같은 "큐 수가 안 바뀌어야 하는" 작업이 잡고,
	 * 쓰기 잠금은 blk_mq_update_nr_hw_queues()가 잡는다. */
	init_rwsem(&set->update_nr_hwq_lock);

	/* [한국어] 이후 실패 경로는 모두 메모리 부족이므로 미리 설정해 둔다. */
	ret = -ENOMEM;
	/* [한국어] 하드웨어 큐 인덱스로 blk_mq_tags를 찾는 포인터 배열.
	 * NVMe에서 인덱스 하나가 nvme_queue 하나(SQ/CQ 쌍)에 대응하고,
	 * 그 blk_mq_tags 가 해당 큐의 tag 공간(sbitmap)을 관리한다.
	 * (그 tag 가 NVMe CID 의 하위 12비트가 된다 — 상위 4비트는 세대 카운터다.)
	 * _node 변형으로 tag set의 NUMA 노드에 할당해 접근 지역성을 확보한다. */
	set->tags = kcalloc_node(set->nr_hw_queues,
				 sizeof(struct blk_mq_tags *), GFP_KERNEL,
				 set->numa_node);
	if (!set->tags)
		goto out_cleanup_tags_srcu;	/* [한국어] 이 시점에는 SRCU 두 개가 초기화돼 있으므로 둘 다 되감아야 한다 */

	/* [한국어] 큐 타입(DEFAULT/READ/POLL)마다 CPU→하드웨어 큐 매핑 테이블을
	 * 할당한다. NVMe는 write_queues/poll_queues 모듈 파라미터에 따라
	 * 최대 3개의 맵을 쓴다. */
	for (i = 0; i < set->nr_maps; i++) {
		/* [한국어] CPU 번호로 인덱싱해 하드웨어 큐 번호를 얻는 배열.
		 * nr_cpu_ids(가능한 최대 CPU 수)만큼 잡는 이유는 CPU 핫플러그로
		 * 나중에 온라인될 CPU도 매핑을 가져야 하기 때문이다. */
		set->map[i].mq_map = kcalloc_node(nr_cpu_ids,
						  sizeof(set->map[i].mq_map[0]),
						  GFP_KERNEL, set->numa_node);
		if (!set->map[i].mq_map)
			goto out_free_mq_map;	/* [한국어] 앞선 반복에서 이미 할당한 맵들도 함께 해제해야 하므로 전용 라벨로 간다 */
		/* [한국어] 일단 전체 큐 수로 초기화한다. 드라이버의 map_queues
		 * 콜백(NVMe는 nvme_pci_map_queues)이 타입별 실제 개수로 덮어쓴다. */
		set->map[i].nr_queues = set->nr_hw_queues;
	}

	/* [한국어] 드라이버의 map_queues 콜백을 호출해 실제 매핑을 채운다.
	 * NVMe PCIe에서는 MSI-X affinity를 그대로 따라가므로, 결과적으로
	 * "제출 CPU = 완료 인터럽트를 받는 CPU"라는 정렬이 만들어진다. */
	blk_mq_update_queue_map(set);

	/* [한국어] 각 하드웨어 큐의 태그 sbitmap과 request 객체 배열을 실제로
	 * 할당한다. request 객체는 미리 전부 할당해 두고 태그로 인덱싱만 하므로,
	 * I/O 경로에서 메모리 할당이 일어나지 않는다 — blk-mq의 핵심 설계다.
	 * 크기는 queue_depth × 하드웨어 큐 수라 NVMe에서는 수 MB에 이른다. */
	ret = blk_mq_alloc_set_map_and_rqs(set);
	if (ret)
		goto out_free_mq_map;	/* [한국어] 부분 성공한 큐들의 태그/request 는 그 함수가 스스로 되돌린 뒤 반환한다 */

	/* [한국어] 이 tag set을 공유하는 request_queue들의 목록과 그 보호 락.
	 * NVMe에서 컨트롤러 하나의 여러 네임스페이스가 여기 등록되어, 태그를
	 * 공유하는 큐들이 서로를 알 수 있게 된다. */
	mutex_init(&set->tag_list_lock);	/* [한국어] 목록 변경은 드물고(네임스페이스 추가/삭제) 잠들 수 있는 문맥이라 spinlock 이 아니라 mutex 다 */
	INIT_LIST_HEAD(&set->tag_list);		/* [한국어] 빈 목록으로 시작 — blk_mq_init_allocated_queue 가 큐를 만들 때마다 여기 매단다 */

	return 0;				/* [한국어] 이제 이 tag set 으로 request_queue 를 만들 수 있다.
						 * NVMe 는 곧바로 blk_mq_alloc_queue 로 네임스페이스별 큐를 붙인다. */

	/* [한국어] ★ 역순 해제 사다리 ★
	 * 각 라벨이 "그 직전까지 성공한 자원"만 정리하고 아래로 흘러내린다. */
out_free_mq_map:
	for (i = 0; i < set->nr_maps; i++) {
		kfree(set->map[i].mq_map);
		/* [한국어] 포인터를 NULL로 지운다. tag set 구조체는 드라이버가
		 * 소유하고 있어 이 함수가 실패해도 살아남으므로, 해제된 주소를
		 * 남기면 이후 정리 코드가 이중 해제를 일으킨다. */
		set->map[i].mq_map = NULL;
	}
	kfree(set->tags);
	set->tags = NULL;
out_cleanup_tags_srcu:
	cleanup_srcu_struct(&set->tags_srcu);
out_cleanup_srcu:
	/* [한국어] BLOCKING이 아니면 애초에 srcu를 만들지 않았으므로 건너뛴다.
	 * 이 조건 검사가 없으면 초기화되지 않은 SRCU를 정리하려다 크래시한다. */
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
 * [한국어]
 * blk_mq_alloc_sq_tag_set - 단일 HW queue 용 tag_set 간편 초기화 래퍼
 *
 * @set:        blk_mq_tag_set
 * @ops:        드라이버 ops (queue_rq 등)
 * @queue_depth: CID 슬롯 수
 * @set_flags:  BLK_MQ_F_* 플래그
 * @return:     0 성공, 음수 에러
 *
 * nr_hw_queues=1, nr_maps=1 로 고정한 후 blk_mq_alloc_tag_set 호출.
 * 단순 블록 장치 (virtio-blk 등) 에서 사용.
 */
int blk_mq_alloc_sq_tag_set(struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int queue_depth,
		unsigned int set_flags)
{
	memset(set, 0, sizeof(*set));
	set->ops = ops;
	/* [한국어] 하드웨어 큐 하나짜리 구성 — SCSI 나 loop 처럼 큐를 여러 개 두지 않는 드라이버용 */
	set->nr_hw_queues = 1;
	set->nr_maps = 1;
	set->queue_depth = queue_depth;
	set->numa_node = NUMA_NO_NODE;
	set->flags = set_flags;
	return blk_mq_alloc_tag_set(set);
}
EXPORT_SYMBOL_GPL(blk_mq_alloc_sq_tag_set);

/*
 * [한국어]
 * blk_mq_free_tag_set - tag set 의 모든 tag pool 과 자료구조 해제
 *
 * @set: 해제할 blk_mq_tag_set
 *
 * 모든 hctx 의 tag pool → shared_tags → mq_map 배열 → SRCU →
 * srcu (blocking) 순으로 해제.
 * 이후 set 의 tags/srcu 필드는 모두 NULL 이 됨.
 */
void blk_mq_free_tag_set(struct blk_mq_tag_set *set)
{
	int i, j;

	/* [한국어] 큐마다 하나씩인 tag pool 과 그에 딸린 request 객체 배열을 해제한다.
	 * request 는 미리 전부 할당돼 있으므로 여기서 돌려주는 메모리가 가장 크다 */
	for (i = 0; i < set->nr_hw_queues; i++)
		__blk_mq_free_map_and_rqs(set, i);

	if (blk_mq_is_shared_tags(set->flags)) {
		/* [한국어] shared_tags 공유 pool 해제 */
		blk_mq_free_map_and_rqs(set, set->shared_tags,
					BLK_MQ_NO_HCTX_IDX);
	}

	for (j = 0; j < set->nr_maps; j++) {
		/* [한국어] CPU→하드웨어 큐 매핑 테이블 해제 */
		kfree(set->map[j].mq_map);
		set->map[j].mq_map = NULL;
	}

	/* [한국어] tags 포인터 배열 해제 */
	kfree(set->tags);
	set->tags = NULL;

	/* [한국어] SRCU grace period 완료 대기: tags_srcu 하에 동작 중인
	 * complete/submit 경로가 모두 빠져나올 때까지 blocking */
	srcu_barrier(&set->tags_srcu);
	cleanup_srcu_struct(&set->tags_srcu);
	if (set->flags & BLK_MQ_F_BLOCKING) {
		/* [한국어] blocking 드라이버용 per-set SRCU 해제 */
		cleanup_srcu_struct(set->srcu);
		kfree(set->srcu);
	}
}
EXPORT_SYMBOL(blk_mq_free_tag_set);

/*
 * [한국어]
 * blk_mq_update_nr_requests - 런타임에 queue depth(발급 가능한 tag 수) 변경
 *
 * @q:   변경 대상 request_queue
 * @et:  scheduler가 미리 할당한 새 elevator_tags (sched tags grow 케이스)
 * @nr:  새로운 queue depth
 * @return: 교체된 old elevator_tags (호출자가 해제), 또는 NULL
 *
 * NVMe 드라이버: /sys/block/nvme0n1/queue/nr_requests 쓰기가 이 경로를 탄다.
 * queue를 quiesce(dispatch 정지)한 후, tag pool의 sbitmap 유효 비트 수를
 * 줄이거나(shrink), 새 tags를 연결해(grow) depth를 변경한다.
 *
 * 케이스:
 *   1) shared_tags + elevator  → sched shared_tags 크기 조정
 *   2) shared_tags only        → 전체 tag pool sbitmap resize
 *   3) non-shared, no elev     → 각 hctx->tags sbitmap resize (shrink only)
 *   4) non-shared + elev, shrink → hctx->sched_tags sbitmap resize
 *   5) non-shared + elev, grow  → 새 et로 hctx->sched_tags 교체
 *
 * 호출 체인:
 *   queue_requests_store (sysfs) → [blk_mq_update_nr_requests]
 */
struct elevator_tags *blk_mq_update_nr_requests(struct request_queue *q,
						struct elevator_tags *et,
						unsigned int nr)
{
	struct blk_mq_tag_set *set = q->tag_set;
	struct elevator_tags *old_et = NULL;
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* [한국어] quiesce — sbitmap 의 유효 비트 수를 바꾸는 동안 아무도 tag 를 잡거나 놓지 않도록 디스패치를 멈춘다.
	 * 진행 중인 요청의 완료까지 기다리므로, 이 호출이 돌아온 뒤에는 인플라이트가 0 이다 */
	blk_mq_quiesce_queue(q);

	if (blk_mq_is_shared_tags(set->flags)) {
		/*
		 * Shared tags, for sched tags, we allocate max initially hence
		 * tags can't grow, see blk_mq_alloc_sched_tags().
		 */
		if (q->elevator)
			/* [한국어] shared_tags + scheduler: sched tag sbitmap 크기 조정 */
			blk_mq_tag_update_sched_shared_tags(q, nr);
		else
			/* [한국어] shared 구성: tag set 에 하나뿐인 pool 을 조정 */
			blk_mq_tag_resize_shared_tags(set, nr);
	} else if (!q->elevator) {
		/*
		 * Non-shared hardware tags, nr is already checked from
		 * queue_requests_store() and tags can't grow.
		 */
		queue_for_each_hw_ctx(q, hctx, i) {
			/* [한국어] 스케줄러 없음: 큐마다 자기 tag sbitmap 을 줄인다 */
			if (!hctx->tags)
				continue;
			/* [한국어] sbitmap_queue_resize 는 배열을 재할당하지 않고 "유효 비트 수"만 바꾼다 —
			 * 그래서 줄이는 것은 언제나 가능하지만 처음 할당한 크기 이상으로 늘릴 수는 없다.
			 * 예약 영역은 별도 sbitmap 이므로 nr 에서 빼고 넘긴다 */
			sbitmap_queue_resize(&hctx->tags->bitmap_tags,
				nr - hctx->tags->nr_reserved_tags);
		}
	} else if (nr <= q->elevator->et->nr_requests) {
		/* Non-shared sched tags, and tags don't grow. */
		/* [한국어] 줄이는 경우. sbitmap 은 유효 비트 수만 낮추면 되므로
		 * 메모리 할당이 전혀 필요 없고, 따라서 실패할 수도 없다. */
		queue_for_each_hw_ctx(q, hctx, i) {
			if (!hctx->sched_tags)
				continue;	/* [한국어] 이 hctx 에는 스케줄러 태그가 없다 — 조용히 건너뛴다 */
			sbitmap_queue_resize(&hctx->sched_tags->bitmap_tags,
				nr - hctx->sched_tags->nr_reserved_tags);	/* [한국어] 예약 영역은 별도 sbitmap 이라 nr 에서 빼고 넘긴다 */
		}
	} else {
		/* Non-shared sched tags, and tags grow */
		/* [한국어] 늘리는 경우. sbitmap 을 처음 할당한 크기 이상으로 키울 수는
		 * 없으므로 새 태그 묶음이 필요한데, **여기서 할당하지 않는다**.
		 * 호출자(sysfs 의 queue_requests_store)가 큐를 quiesce 하기 *전에*
		 * 미리 할당해 et 로 넘겨 준다. 이유: 지금은 큐가 멈춰 있고 인플라이트가
		 * 0 인 상태라, 이 안에서 GFP_KERNEL 할당을 하면 메모리 회수가 이 장치로의
		 * 쓰기를 시도하다 멈춘 큐에 막혀 교착이 된다. 할당은 밖에서, 교체만 안에서. */
		queue_for_each_hw_ctx(q, hctx, i)
			hctx->sched_tags = et->tags[i];	/* [한국어] 포인터만 갈아 끼운다 — 큐가 멈춰 있으니 원자성 걱정이 없다 */
		old_et =  q->elevator->et;		/* [한국어] 옛 것을 반환값으로 넘겨 호출자가 unquiesce 이후에 해제하게 한다 */
		q->elevator->et = et;
	}

	/*
	 * Preserve relative value, both nr and async_depth are at most 16 bit
	 * value, no need to worry about overflow.
	 */
	/* [한국어] async_depth 는 "비동기(대기하지 않는) 요청이 태그를 몇 개까지
	 * 차지해도 되는가"의 상한이다. 이것이 없으면 백그라운드 쓰기 같은 비동기
	 * 요청이 태그를 모두 채워 동기 읽기(사용자가 실제로 기다리는 것)를 굶긴다.
	 * 전체 depth 를 바꿨으니 같은 **비율**을 유지하도록 함께 조정한다.
	 * nr 로 나누기 전에 곱하는 순서라 정수 나눗셈 손실이 최소화되고,
	 * max(..., 1) 로 0 이 되는 것을 막는다(0 이면 비동기가 영영 진행 못 한다). */
	q->async_depth = max(q->async_depth * nr / q->nr_requests, 1);
	q->nr_requests = nr;	/* [한국어] 새 값 반영. 위 계산이 옛 q->nr_requests 를 쓰므로 반드시 그 뒤에 와야 한다 */
	if (q->elevator && q->elevator->type->ops.depth_updated)
		/* [한국어] 스케줄러는 depth 를 기준으로 자기 내부 한도를 잡아 둔다
		 * (예: mq-deadline 의 async 상한, kyber 의 도메인별 토큰 수).
		 * 알려 주지 않으면 옛 depth 기준 한도를 계속 써서 정책이 어긋난다. */
		q->elevator->type->ops.depth_updated(q);

	blk_mq_unquiesce_queue(q);	/* [한국어] 디스패치 재개 — 이 시점부터 새 depth 로 태그가 발급된다 */
	return old_et;			/* [한국어] 교체된 옛 태그 묶음. NULL 이 아니면 호출자가 해제해야 한다 */
}

/*
 * Switch back to the elevator type stored in the xarray.
 */
/*
 * [한국어]
 * blk_mq_elv_switch_back - HW queue 수 변경 완료 후 elevator 복원
 *
 * @q:       복원 대상 request_queue
 * @elv_tbl: elv_change_ctx 를 담은 xarray (queue id → ctx)
 *
 * __blk_mq_update_nr_hw_queues 의 switch_back 레이블에서 호출.
 * elv_update_nr_hw_queues 가 실제로 elevator 를 재초기화하며
 * queue 를 unfreeze 한다. elevator 모듈 참조는 이 시점에 반환.
 */
static void blk_mq_elv_switch_back(struct request_queue *q,
		struct xarray *elv_tbl)
{
	/* [한국어] elv_tbl 에서 이 queue 의 elevator 전환 컨텍스트 가져오기 */
	struct elv_change_ctx *ctx = xa_load(elv_tbl, q->id);

	if (WARN_ON_ONCE(!ctx))
		return;

	/* The elv_update_nr_hw_queues unfreezes the queue. */
	/* [한국어] elevator 재초기화 + queue unfreeze (hctx 수 변경 반영) */
	elv_update_nr_hw_queues(q, ctx);

	/* Drop the reference acquired in blk_mq_elv_switch_none. */
	/* [한국어] switch_none 에서 취득한 elevator 모듈 참조 반환 */
	if (ctx->type)
		elevator_put(ctx->type);
}

/*
 * Stores elevator name and type in ctx and set current elevator to none.
 */
/*
 * [한국어]
 * blk_mq_elv_switch_none - HW queue 수 변경 전 elevator 를 none 으로 교체
 *
 * @q:       대상 request_queue
 * @elv_tbl: elv_change_ctx 를 저장할 xarray
 * @return:  0 성공, -ENOENT ctx 없음
 *
 * nr_hw_queues 변경 중 elevator 가 구 hctx 포인터를 참조하지 않도록
 * 임시로 'none' 으로 교체. elevator 모듈 참조를 취득하여 변경 중
 * 모듈 제거를 막는다. switch_back 에서 복원.
 *
 * update_nr_hwq_lock 쓰기 컨텍스트에서만 호출 — elevator 전환 코드와
 * 같은 lock 을 읽기로 잡으므로 동시 실행 불가.
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
		/* [한국어] 이 queue 의 elevator 전환 컨텍스트 획득 */
		ctx = xa_load(elv_tbl, q->id);
		if (WARN_ON_ONCE(!ctx))
			return -ENOENT;

		/* [한국어] elevator 이름 저장 — switch_back 에서 재설정에 사용 */
		ctx->name = q->elevator->type->elevator_name;

		/*
		 * Before we switch elevator to 'none', take a reference to
		 * the elevator module so that while nr_hw_queue update is
		 * running, no one can remove elevator module. We'd put the
		 * reference to elevator module later when we switch back
		 * elevator.
		 */
		/* [한국어] elevator 모듈 참조 취득: 변경 중 모듈 제거 방지 */
		__elevator_get(q->elevator->type);

		/*
		 * Store elevator type so that we can release the reference
		 * taken above later.
		 */
		/* [한국어] type 저장: switch_back 에서 elevator_put 에 사용 */
		ctx->type = q->elevator->type;
		/* [한국어] elevator 를 none 으로 교체 — 이후 dispatch 는 직접 경로 */
		elevator_set_none(q);
	}
	return 0;
}

/*
 * [한국어]
 * __blk_mq_update_nr_hw_queues - NVMe HW queue(SQ/CQ) 수 런타임 변경 내부 구현
 *
 * @set:          대상 blk_mq_tag_set
 * @nr_hw_queues: 새로운 HW queue 수
 *
 * 이 함수는 tag_list_lock 을 이미 보유한 상태에서만 호출된다.
 * 변경 절차:
 *   1) nr_hw_queues 유효성 검사 및 상한 조정
 *   2) NOIO 메모리 할당 모드 설정 (I/O 중 재귀 방지)
 *   3) elevator scheduler 컨텍스트 사전 할당
 *   4) 기존 hctx 의 sysfs/debugfs 등록 해제
 *   5) elevator 를 none 으로 임시 교체 (구 hctx 참조 제거)
 *   6) 새 tags 배열 사전 할당
 *   7) 모든 request_queue freeze (새 dispatch 차단)
 *   8) set->nr_hw_queues 갱신, CPU→하드웨어 큐 매핑 재구성
 *   9) hctx 재할당 및 map_swqueue (CPU affinity 재매핑)
 *  10) elevator 복원 (queue unfreeze 포함)
 *  11) 새 hctx sysfs/debugfs 등록, cpuhp 핸들러 재등록
 *  12) 잉여 tags 해제 (shrink 시)
 *
 * fallback: hctx 재할당 실패 시 prev_nr_hw_queues 로 복귀
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues → [__blk_mq_update_nr_hw_queues]
 */
static void __blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set,
							int nr_hw_queues)
{
	struct request_queue *q;			/* [한국어] 이 tag set 을 공유하는 큐들을 순회할 커서. NVMe 면 컨트롤러의 각 네임스페이스 */
	int prev_nr_hw_queues = set->nr_hw_queues;	/* [한국어] 실패 시 되돌아갈 값. 이 함수는 중간에 실패해도 장치를 살려 두어야 한다 */
	unsigned int memflags;				/* [한국어] memalloc_noio_save 가 돌려주는 옛 플래그. 반드시 짝을 맞춰 복원해야 한다 */
	int i;
	struct xarray elv_tbl;				/* [한국어] 큐 id → "원래 어떤 스케줄러였는지" 기록. 재구성이 끝나면 이걸 보고 되돌린다 */
	struct blk_mq_tags **new_tags;			/* [한국어] 큐 수가 늘어날 때 미리 확보해 두는 새 tags[] 배열 */
	bool queues_frozen = false;			/* [한국어] 어느 실패 지점에서 빠져나가든 freeze 를 정확히 한 번만 풀기 위한 표시 */

	lockdep_assert_held(&set->tag_list_lock);	/* [한국어] tag_list 를 순회하므로 호출자가 이미 락을 쥐고 있어야 한다 */

	/* [한국어] 맵이 하나뿐이면 CPU 수보다 많은 큐는 아무도 쓰지 않는다 —
	 * 한 CPU 는 한 맵에서 hctx 하나에만 매핑되기 때문이다. 조용히 잘라 낸다. */
	if (set->nr_maps == 1 && nr_hw_queues > nr_cpu_ids)
		nr_hw_queues = nr_cpu_ids;
	/* [한국어] 큐가 0개면 I/O 를 내보낼 곳이 사라진다 — 요청을 무시하고 반환 */
	if (nr_hw_queues < 1)
		return;
	/* [한국어] 값이 그대로면 할 일이 없다. 이 조기 반환이 중요한 이유는 아래
	 * 전 과정이 모든 큐를 freeze 하고 스케줄러를 떼었다 붙이는 대공사이기 때문이다.
	 * 맵이 여러 개면 개수가 같아도 타입별 분배가 달라졌을 수 있어 그냥 진행한다. */
	if (set->nr_maps == 1 && nr_hw_queues == set->nr_hw_queues)
		return;

	/* [한국어] 지금부터 이 문맥의 모든 메모리 할당에서 __GFP_IO 를 뺀다.
	 * 왜 필요한가: 아래에서 큐를 freeze 하는데, 그 뒤 할당이 메모리 부족을 만나
	 * 회수를 시도하면 더티 페이지를 이 장치에 써 내보내려 한다. 그런데 그 큐는
	 * 방금 우리가 얼려 놓았으므로 영원히 진행하지 못한다 — 자기 자신을 기다리는
	 * 교착이다. NOIO 로 두면 회수기가 블록 I/O 를 일으키지 않고 다른 수단만 쓴다. */
	memflags = memalloc_noio_save();

	xa_init(&elv_tbl);	/* [한국어] 큐 수가 가변이라 고정 배열 대신 xarray 를 쓴다 */
	/* [한국어] 필요한 할당을 **freeze 전에 전부 끝내 두는** 것이 이 함수의 설계 원칙이다.
	 * 얼린 뒤에 할당하면 위에서 말한 교착 위험이 생기고, 실패 시 되돌리기도 훨씬 어렵다. */
	if (blk_mq_alloc_sched_ctx_batch(&elv_tbl, set) < 0)
		goto out_free_ctx;	/* [한국어] 아직 freeze 도 스케줄러 분리도 하지 않았으므로 되돌릴 것이 거의 없다 */

	/* [한국어] 새 큐 수에 맞는 스케줄러 태그까지 미리 확보한다 */
	if (blk_mq_alloc_sched_res_batch(&elv_tbl, set, nr_hw_queues) < 0)
		goto out_free_ctx;

	/* [한국어] sysfs/debugfs 노드는 hctx 를 직접 가리키므로, hctx 를 해제하기 전에
	 * 먼저 떼어 내야 한다. 순서가 반대면 사용자가 /sys 를 읽는 순간 해제된
	 * 메모리를 참조하게 된다. 등록 해제는 열려 있는 파일이 닫히기를 기다린다. */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		blk_mq_debugfs_unregister_hctxs(q);
		blk_mq_sysfs_unregister_hctxs(q);
	}

	/*
	 * Switch IO scheduler to 'none', cleaning up the data associated
	 * with the previous scheduler. We will switch back once we are done
	 * updating the new sw to hw queue mappings.
	 */
	/* [한국어] ★ 스케줄러를 일시적으로 떼어내는 이유 ★
	 * I/O 스케줄러는 하드웨어 큐마다 전용 태그 세트(sched_tags)를 갖고,
	 * 자기 자료구조 안에 hctx 포인터를 들고 있다. 그런데 지금부터 hctx
	 * 배열을 통째로 재구성할 것이므로, 그 포인터들이 전부 무효해진다.
	 * 안전하게 재구성하려면 스케줄러를 먼저 떼어 참조를 없애고, 재구성이
	 * 끝난 뒤 새 hctx로 다시 붙여야 한다.
	 * elv_tbl에 "원래 어떤 스케줄러였는지"를 기록해 두었다가
	 * blk_mq_elv_switch_back()이 복원한다. */
	list_for_each_entry(q, &set->tag_list, tag_set_list)
		if (blk_mq_elv_switch_none(q, &elv_tbl))
			goto switch_back;	/* [한국어] 일부 큐만 none 으로 바뀐 상태다. switch_back 이
						 * elv_tbl 을 보고 바뀐 것만 정확히 되돌린다 */

	/* [한국어] 새 nr_hw_queues 에 맞는 tags 배열 사전 할당 */
	new_tags = blk_mq_prealloc_tag_set_tags(set, nr_hw_queues);
	if (IS_ERR(new_tags))
		goto switch_back;

	/* [한국어] 이 tag set을 공유하는 "모든" 큐를 freeze한다. NVMe에서는
	 * 컨트롤러 하나의 모든 네임스페이스(/dev/nvme0n1, n2, ...)가 여기 해당한다.
	 * 하나라도 빠뜨리면 그 큐의 I/O가 재구성 중인 hctx를 참조해 크래시한다.
	 * freeze는 새 I/O 진입을 막고 진행 중인 것이 모두 끝나기를 기다린다. */
	list_for_each_entry(q, &set->tag_list, tag_set_list)
		blk_mq_freeze_queue_nomemsave(q);
	/* [한국어] 아래 switch_back 라벨에서 "이미 freeze했는가"를 판단하는 플래그.
	 * 오류로 중간에 점프해 왔을 때 중복 freeze를 피하기 위해 필요하다. */
	queues_frozen = true;
	/* [한국어] 큐 수가 늘어난 경우에만 새 tags 배열이 준비되어 있다.
	 * 줄어드는 경우는 기존 배열을 그대로 쓰고 뒤쪽만 비운다. */
	if (new_tags) {
		kfree(set->tags);
		set->tags = new_tags;
	}
	/* [한국어] 목표 큐 수를 확정한다. 이 시점부터 아래 재구성 코드가
	 * 이 값을 기준으로 동작한다. NVMe에서 이 값이 바뀌는 대표적 상황은
	 * 컨트롤러 리셋 후 큐 수 재협상 결과가 이전과 달라졌을 때다. 실물 경로:
	 *   nvme_set_queue_count() → nvme_set_features(ctrl, NVME_FEAT_NUM_QUEUES, …)
	 *   로 컨트롤러와 큐 개수를 합의하고,
	 *   nvme_pci_update_nr_queues() 가
	 *   blk_mq_update_nr_hw_queues(&dev->tagset, dev->online_queues - 1)
	 *   을 불러 이 함수까지 온다 (drivers/nvme/host/pci.c:4064).
	 * online_queues 에서 1 을 빼는 것은 admin 큐를 제외하기 위해서다. */
	set->nr_hw_queues = nr_hw_queues;

fallback:
	/* [한국어] CPU→하드웨어 큐 매핑 테이블을 새 큐 수 기준으로 다시 만든다.
	 * 드라이버의 map_queues 콜백이 여기서 불리며, NVMe PCIe 는
	 * nvme_pci_map_queues() 에서 blk_mq_map_hw_queues() 로 PCI MSI-X
	 * 인터럽트 어피니티를 그대로 따라간다 — 그 결과 "제출한 CPU 가 완료
	 * 인터럽트도 받는" 정렬이 재구성 후에도 유지된다. */
	blk_mq_update_queue_map(set);
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* [한국어] 각 request_queue 의 hctx 배열 재할당 */
		__blk_mq_realloc_hw_ctxs(set, q);

		/* [한국어] ★ 확장 실패 시의 우아한 저하 ★
		 * __blk_mq_realloc_hw_ctxs()는 메모리 부족으로 목표 개수만큼
		 * hctx를 만들지 못하면 q->nr_hw_queues를 올리지 않는다. 그 불일치를
		 * 여기서 감지한다.
		 * 이때 실패로 끝내지 않는 이유: 큐 수가 적어도 I/O는 정상 동작하므로,
		 * 장치를 못 쓰게 만드는 것보다 이전 개수로 되돌아가 계속 쓰는 편이
		 * 훨씬 낫다. NVMe 컨트롤러 리셋 도중 메모리가 빠듯할 때 실제로
		 * 발생할 수 있는 상황이다. */
		if (q->nr_hw_queues != set->nr_hw_queues) {
			int i = prev_nr_hw_queues;

			pr_warn("Increasing nr_hw_queues to %d fails, fallback to %d\n",
					nr_hw_queues, prev_nr_hw_queues);
			/* [한국어] 부분적으로 만들어진 초과분의 태그 세트를 해제한다.
			 * hctx는 못 만들었어도 태그는 앞서 할당되었을 수 있어 따로
			 * 정리해야 누수가 없다. */
			for (; i < set->nr_hw_queues; i++)
				__blk_mq_free_map_and_rqs(set, i);

			/* [한국어] 목표를 이전 개수로 낮추고 fallback 라벨로 되돌아가
			 * 매핑 재구성을 처음부터 다시 한다. 이번에는 이미 존재하는
			 * 개수이므로 반드시 성공한다 — 무한 루프가 되지 않는다. */
			set->nr_hw_queues = prev_nr_hw_queues;
			goto fallback;
		}
		/* [한국어] 위에서 만든 set 수준의 CPU→큐 매핑을 이 request_queue 의
		 * 소프트웨어 컨텍스트(ctx)들에 실제로 반영한다. 각 CPU 의 ctx 가
		 * 어느 hctx 에 매달릴지가 여기서 정해진다. */
		blk_mq_map_swqueue(q);
	}
switch_back:
	/* The blk_mq_elv_switch_back unfreezes queue for us. */
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* switch_back expects queue to be frozen */
		if (!queues_frozen)
			/* [한국어] 앞쪽에서 실패해 점프해 온 경로는 아직 얼지 않았다.
			 * 그런데 blk_mq_elv_switch_back() 은 "얼어 있는 큐"를 전제로
			 * 스케줄러를 붙이고 자기가 녹인다. 전제를 맞춰 주기 위해
			 * 여기서 얼린다 — 곧바로 녹일 것이지만 그 사이 새 I/O 가
			 * 반쯤 복원된 스케줄러를 만나는 일을 막는다. */
			blk_mq_freeze_queue_nomemsave(q);
		/* [한국어] 원래 스케줄러를 새 hctx 위에 다시 붙이고, 그 안에서 큐를 녹인다.
		 * 녹이는 책임이 이 함수에 있다는 점이 중요하다 — 여기서 따로 unfreeze 하면 이중 해제가 된다. */
		blk_mq_elv_switch_back(q, &elv_tbl);
	}

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		/* [한국어] 큐가 다시 살아난 뒤에야 사용자에게 노출한다.
		 * /sys/block/nvme0n1/mq/* 아래의 hctx 디렉터리가 여기서 되살아난다. */
		blk_mq_sysfs_register_hctxs(q);
		blk_mq_debugfs_register_hctxs(q);

		/* [한국어] CPU 핫플러그 콜백은 hctx 마다 등록된다. hctx 배열이 통째로
		 * 바뀌었으므로 옛 등록을 먼저 걷어 내고 새로 건다. 순서가 중요하다 —
		 * 먼저 add 하면 같은 hctx 에 콜백이 두 번 걸린다. */
		blk_mq_remove_hw_queues_cpuhp(q);
		blk_mq_add_hw_queues_cpuhp(q);
	}

out_free_ctx:
	/* [한국어] 스케줄러 복원용으로 잡아 둔 컨텍스트들을 해제한다.
	 * 성공 경로와 실패 경로가 모두 여기를 지나므로, 어느 쪽이든 누수가 없다. */
	blk_mq_free_sched_ctx_batch(&elv_tbl);
	xa_destroy(&elv_tbl);
	/* [한국어] NOIO 메모리 컨텍스트를 원복한다. 이 구간에서 I/O를 유발하는
	 * 회수를 막았던 제약이 풀린다. */
	memalloc_noio_restore(memflags);

	/* Free the excess tags when nr_hw_queues shrink. */
	/* [한국어] 큐 수가 줄어든 경우, 이제 쓰이지 않는 인덱스의 태그 세트를
	 * 해제한다. 루프 조건이 set->nr_hw_queues < prev_nr_hw_queues일 때만
	 * 도는 구조라, 늘어난 경우에는 자연히 아무 일도 하지 않는다.
	 * 여기까지 미룬 이유: 재구성 도중에 해제하면 fallback으로 되돌아갔을 때
	 * 필요한 태그가 이미 사라져 있게 된다. */
	for (i = set->nr_hw_queues; i < prev_nr_hw_queues; i++)
		__blk_mq_free_map_and_rqs(set, i);
}

/*
 * [한국어]
 * blk_mq_update_nr_hw_queues - 하드웨어 큐 수 런타임 변경 외부 API
 *
 * @set:          대상 blk_mq_tag_set
 * @nr_hw_queues: 새 하드웨어 큐 수
 *
 * 드라이버가 장치와 협상한 큐 개수가 바뀌었을 때 호출한다.
 * update_nr_hwq_lock 쓰기 잠금(elevator 전환과 배타)과 tag_list_lock 을
 * 모두 획득한 뒤 __blk_mq_update_nr_hw_queues 에 위임한다.
 *
 * NVMe 접점 (실물 확인): 컨트롤러 리셋 때마다 I/O 큐 개수가 다시 협상된다.
 * nvme_reset_work() → nvme_pci_update_nr_queues() 가
 * blk_mq_update_nr_hw_queues(&dev->tagset, dev->online_queues - 1) 을 부른다
 * (online_queues 는 admin 큐를 포함하므로 -1). 큐 개수는 Set Features의
 * Number of Queues(nvme_set_queue_count) 결과와 확보된 MSI-X 벡터 수의
 * 최솟값으로 정해지므로, 리셋 전후로 달라질 수 있다.
 * fabrics(rdma/tcp/fc)와 apple 드라이버도 각자의 재연결 경로에서 호출한다.
 *
 * 호출 체인:
 *   nvme_reset_work → nvme_pci_update_nr_queues → [blk_mq_update_nr_hw_queues]
 *   → __blk_mq_update_nr_hw_queues
 */
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues)
{
	/* [한국어] update_nr_hwq_lock 쓰기: elevator 전환과 배타적으로 진행 */
	down_write(&set->update_nr_hwq_lock);
	/* [한국어] tag_list_lock: set->tag_list 순회 보호 */
	mutex_lock(&set->tag_list_lock);
	__blk_mq_update_nr_hw_queues(set, nr_hw_queues);
	mutex_unlock(&set->tag_list_lock);
	up_write(&set->update_nr_hwq_lock);
}
EXPORT_SYMBOL_GPL(blk_mq_update_nr_hw_queues);

/*
 * [한국어]
 * blk_hctx_poll - 단일 hctx 에 대한 NVMe CQ 폴링 루프
 *
 * @q:     request_queue
 * @hctx:  폴링 대상 hctx (HCTX_TYPE_POLL 큐)
 * @iob:   완료된 request 를 배치로 수집하는 컨테이너
 * @flags: BLK_POLL_ONESHOT 등 폴링 제어 플래그
 * @return: 완료 수(>0), 오류(<0), 또는 0(타임아웃/재스케줄 필요)
 *
 * mq_ops->poll (NVMe: nvme_poll) 을 반복 호출하여 인터럽트 없이
 * CQ 항목을 소비한다. BLK_POLL_ONESHOT 이면 한 번만 폴링.
 * signal pending 이거나 재스케줄 필요 시 루프 종료.
 *
 * 호출 체인:
 *   blk_mq_poll / blk_rq_poll → [blk_hctx_poll] → mq_ops->poll (nvme_poll)
 */
static int blk_hctx_poll(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			 struct io_comp_batch *iob, unsigned int flags)
{
	int ret;

	do {
		/* [한국어] mq_ops->poll(== nvme_poll): NVMe CQ 항목 직접 소비 */
		ret = q->mq_ops->poll(hctx, iob);
		/* [한국어] 완료 항목이 있으면 즉시 반환 */
		if (ret > 0)
			return ret;
		/* [한국어] 시그널 대기 중이면 폴링 중단 (user 요청) */
		if (task_sigpending(current))
			return 1;
		/* [한국어] ONESHOT 플래그이거나 오류(-errno)면 루프 탈출 */
		if (ret < 0 || (flags & BLK_POLL_ONESHOT))
			break;
		/* [한국어] cpu_relax: CQ 완료 대기 busy-wait (pause 인스트럭션 등) */
		cpu_relax();
	} while (!need_resched());

	return 0;
}

/*
 * [한국어]
 * blk_mq_poll - cookie 로 특정 poll hctx 를 지정해 CQ 폴링
 *
 * @q:      request_queue
 * @cookie: submit_bio 반환값 — poll hctx 인덱스가 인코딩됨
 * @iob:    완료 배치 컨테이너
 * @flags:  BLK_POLL_* 플래그
 * @return: 완료 수
 *
 * blk_poll 이 호출하는 blk-mq 레벨 폴링 엔트리.
 * cookie 로 queue_hw_ctx 배열에서 poll hctx 를 찾아
 * blk_hctx_poll 에 위임.
 *
 * 호출 체인:
 *   blk_poll (vfs/io_uring) → [blk_mq_poll] → blk_hctx_poll
 */
int blk_mq_poll(struct request_queue *q, blk_qc_t cookie,
		struct io_comp_batch *iob, unsigned int flags)
{
	/* [한국어] poll queue 를 지원하지 않으면 즉시 0 반환 */
	if (!blk_mq_can_poll(q))
		return 0;
	/* [한국어] cookie = hctx 인덱스 → queue_hw_ctx[cookie] 로 poll hctx 선택 */
	return blk_hctx_poll(q, q->queue_hw_ctx[cookie], iob, flags);
}

/*
 * [한국어]
 * blk_rq_poll - 특정 request 의 poll hctx 를 직접 폴링
 *
 * @rq:         폴링 대상 NVMe request (REQ_POLLED 로 제출된 것)
 * @iob:        완료 배치 컨테이너
 * @poll_flags: BLK_POLL_* 플래그
 * @return:     완료 수
 *
 * io_uring 등에서 특정 request 의 완료를 인터럽트 없이 확인.
 * rq->mq_hctx 로 해당 NVMe CQ 를 직접 폴링.
 * percpu_ref 로 queue 생존을 보장한 후 poll 수행.
 *
 * 호출 체인:
 *   io_uring → [blk_rq_poll] → blk_hctx_poll → nvme_poll
 */
int blk_rq_poll(struct request *rq, struct io_comp_batch *iob,
		unsigned int poll_flags)
{
	struct request_queue *q = rq->q;
	int ret;

	/* [한국어] poll hctx 에 제출된 request 가 아니면 폴링 불필요 */
	if (!blk_rq_is_poll(rq))
		return 0;
	/* [한국어] q_usage_counter: queue 가 active 한지 확인 (dying 중이면 실패) */
	if (!percpu_ref_tryget(&q->q_usage_counter))
		return 0;

	ret = blk_hctx_poll(q, rq->mq_hctx, iob, poll_flags);
	/* [한국어] percpu_ref 반환 */
	blk_queue_exit(q);

	return ret;
}
EXPORT_SYMBOL_GPL(blk_rq_poll);

/*
 * [한국어]
 * blk_mq_rq_cpu - request 를 제출한 CPU 번호 반환
 *
 * @rq: 대상 request
 * @return: mq_ctx->cpu (제출 시점의 CPU)
 */
unsigned int blk_mq_rq_cpu(struct request *rq)
{
	/* [한국어] mq_ctx: request 를 큐잉한 per-CPU sw queue */
	return rq->mq_ctx->cpu;
}
EXPORT_SYMBOL(blk_mq_rq_cpu);

/*
 * [한국어]
 * blk_mq_cancel_work_sync - request_queue 의 모든 지연 work 동기적 취소
 *
 * @q: 대상 request_queue
 *
 * queue teardown 시 requeue_work(실패한 request 재제출) 와
 * 모든 hctx 의 run_work(dispatch 지연 실행) 를 취소.
 * cancel_delayed_work_sync 는 현재 실행 중인 work 가 완료될 때까지 blocking.
 */
void blk_mq_cancel_work_sync(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	/* [한국어] requeue_work: 실패 request 를 재제출하는 지연 work 취소 */
	cancel_delayed_work_sync(&q->requeue_work);

	/* [한국어] 각 hctx 의 run_work: NVMe SQ dispatch 지연 work 취소 */
	queue_for_each_hw_ctx(q, hctx, i)
		cancel_delayed_work_sync(&hctx->run_work);
}

/*
 * [한국어]
 * blk_mq_init - blk-mq 서브시스템 모듈 초기화 (커널 부팅 시 1회)
 *
 * @return: 0 성공
 *
 * 초기화 내용:
 *   1) per-CPU blk_cpu_done lock-free 리스트: NVMe CQ 인터럽트가 다른 CPU에서
 *      완료된 request 를 softirq 로 넘기기 위한 전달 큐
 *   2) per-CPU blk_cpu_csd: IPI(inter-processor interrupt) 콜백 구조체.
 *      NVMe CQ 인터럽트 핸들러가 완료를 다른 CPU 로 전달할 때 사용
 *   3) BLOCK_SOFTIRQ 등록: blk_done_softirq 가 blk_cpu_done 리스트를 드레인
 *   4) cpuhp 핸들러 등록:
 *      - CPUHP_BLOCK_SOFTIRQ_DEAD: CPU 오프라인 시 blk_cpu_done 드레인
 *      - CPUHP_BLK_MQ_DEAD: hctx 데드 처리 (blk_mq_hctx_notify_dead)
 *      - CPUHP_AP_BLK_MQ_ONLINE: hctx 온라인/오프라인 (CPU hotplug 매핑 갱신)
 *
 * 호출: subsys_initcall (커널 init 단계)
 */
static int __init blk_mq_init(void)
{
	int i;

	/* [한국어] blk_cpu_done: per-CPU lock-free llist (완료 전달 큐 초기화) */
	for_each_possible_cpu(i)
		init_llist_head(&per_cpu(blk_cpu_done, i));
	/* [한국어] blk_cpu_csd: IPI 콜백 구조체 초기화
	 * __blk_mq_complete_request_remote: 다른 CPU 로 완료 이벤트를 전달 */
	for_each_possible_cpu(i)
		INIT_CSD(&per_cpu(blk_cpu_csd, i),
			 __blk_mq_complete_request_remote, NULL);
	/* [한국어] BLOCK_SOFTIRQ 등록: blk_done_softirq 가 blk_cpu_done 드레인 */
	open_softirq(BLOCK_SOFTIRQ, blk_done_softirq);

	/* [한국어] CPU 오프라인 시 해당 CPU 의 미처리 완료 request 를 처리 */
	cpuhp_setup_state_nocalls(CPUHP_BLOCK_SOFTIRQ_DEAD,
				  "block/softirq:dead", NULL,
				  blk_softirq_cpu_dead);
	/* [한국어] hctx dead 핸들러: CPU 오프라인 후 hctx 를 dead 상태로 전환 */
	cpuhp_setup_state_multi(CPUHP_BLK_MQ_DEAD, "block/mq:dead", NULL,
				blk_mq_hctx_notify_dead);
	/* [한국어] hctx online/offline: CPU hotplug 시 CPU→SQ 매핑 갱신 */
	cpuhp_setup_state_multi(CPUHP_AP_BLK_MQ_ONLINE, "block/mq:online",
				blk_mq_hctx_notify_online,
				blk_mq_hctx_notify_offline);
	return 0;
}
subsys_initcall(blk_mq_init);

/*
 * NVMe 관점 핵심 요약
 * - blk-mq 의 request 하나가 인플라이트 명령 하나에 대응하고, rq->tag 가 NVMe CID 의
 *   하위 12비트가 된다. SQ 링의 슬롯 위치는 sq_tail 이 따로 정하므로 tag 와 무관하다.
 * - blk_mq_dispatch_rq_list() -> mq_ops->queue_rq() 가 NVMe 의 nvme_queue_rq
 *   로 연결되며, 실제 doorbell 기록은 드라이버 측에서 이루어진다.
 * - 완료 경로는 컨트롤러 CQ 인터럽트 -> nvme_irq -> blk_mq_complete_request()
 *   -> blk_mq_end_request() 순으로 흐른다.
 * - blk_mq_alloc_tag_set() / blk_mq_map_swqueue() 가 SQ/CQ 쌍과 CPU affinity
 *   를 초기화하는 기반이 된다.
 * - I/O scheduler 가 없으면 request 는 plug list 나 hctx->dispatch 를 거쳐
 *   직접 dispatch 되며, scheduler 사용 시에는 blk-mq-sched.c 로 위임된다.
 */
