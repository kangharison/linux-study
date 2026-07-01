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
 * [NVMe] nvme_queue_rq() → SQ doorbell → CQ 완료 인터럽트
 *       ↓
 * [완료] nvme_irq() → blk_mq_complete_request() → blk_done_softirq()
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
 *   drivers/nvme/host/pci.c : ops->queue_rq 콜백 등록자이자 완료 통보자
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
 * 호출 체인:
 *   nvme_reset_work / blk_mq_update_nr_requests → [blk_freeze_queue_start]
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
 * 메모리 저장(memcg) 없는 단순 freeze 경로. NVMe reset, error recovery, teardown 등
 * 컨텍스트에서 큐를 완전히 멈추기 위해 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sleep 가능).
 *
 * 호출 체인:
 *   nvme_dev_disable → blk_mq_freeze_queue → [blk_mq_freeze_queue_nomemsave]
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
 * 호출 체인:
 *   nvme_dev_enable → blk_mq_unfreeze_queue → [blk_mq_unfreeze_queue_nomemrestore]
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
 * 호출 체인:
 *   blk_mq_quiesce_queue → [blk_mq_quiesce_queue_nowait]
 *   nvme_auth_stop_dhchap → [blk_mq_quiesce_queue_nowait]
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
		synchronize_srcu(set->srcu);
	else
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
 * 호출 체인:
 *   nvme_dev_disable / blk_mq_update_nr_requests → [blk_mq_quiesce_queue]
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
 * 호출 체인:
 *   nvme_dev_enable → blk_mq_unquiesce_queue → blk_mq_run_hw_queues
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
		;
	} else if (!--q->quiesce_depth) {
		/* [한국어] 깊이가 0이 됐을 때만 QUIESCED 플래그를 해제. */
		blk_queue_flag_clear(QUEUE_FLAG_QUIESCED, q);
		/* [한국어] 플래그 해제 직후 run_hw_queues 를 예약해야 함을 표시 */
		run_queue = true;
	}
	spin_unlock_irqrestore(&q->queue_lock, flags);

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
 * NVMe multipath 나 dm 처럼 하나의 tag_set 이 여러 namespace/큐를 관리할 때 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   nvme_mpath_start_freeze → [blk_mq_quiesce_tagset]
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
 * 호출 체인:
 *   nvme_mpath_end_freeze → [blk_mq_unquiesce_tagset]
 */
void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	/* [한국어] tag_list RCU 순회 보호 */
	rcu_read_lock();
	list_for_each_entry_rcu(q, &set->tag_list, tag_set_list) {
		/* [한국어] 각 큐의 quiesce_depth 감소 및 QUIESCED 플래그 해제 */
		if (!blk_queue_skip_tagset_quiesce(q))
			blk_mq_unquiesce_queue(q);
	}
	rcu_read_unlock();
}
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
 * NVMe 관점: request 는 NVMe SQ 명령 슬롯(CID)에 1:1 대응한다. 이 함수는
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
	/* [한국어] tag = BLK_MQ_NO_TAG(-1): 아직 NVMe CID 미할당.
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
	else
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
 * @tag:  할당받은 태그 번호 (= NVMe CID, 0 ~ queue_depth-1)
 * @return: 초기화 완료된 struct request 포인터
 *
 * tags->static_rqs[tag] 에서 사전 할당된 request 슬롯을 가져와
 * 현재 hctx(SQ), ctx(CPU sw 큐), cmd_flags(IO 타입), rq_flags,
 * tag 등을 기록한다. scheduler 사용 시 prepare_request 콜백도 호출.
 * NVMe 관점: rq->tag 가 CID 에 직접 대응하며, nvme_queue_rq() 에서
 * 이 값을 NVMe 명령 SQ Entry 의 CID 필드에 넣는다.
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

	/* [한국어] BLK_MQ_REQ_PM: 전원 관리 관련 request — nvme_submit_cmd 에서
	 * 전원 상태 확인 로직을 건너뛸 수 있다. */
	if (data->flags & BLK_MQ_REQ_PM)
		data->rq_flags |= RQF_PM;
	/* [한국어] rq_flags: RQF_IO_STAT, RQF_USE_SCHED, RQF_PM 등 복합 플래그 */
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
	unsigned int tag, tag_offset;
	struct blk_mq_tags *tags;
	struct request *rq;
	/* [한국어] 한 번의 blk_mq_get_tags 호출로 얻은 tag 비트마스크 */
	unsigned long tag_mask;
	int i, nr = 0;     /* nr: 이번 배치에서 할당한 총 tag 수 */

	do {
		/* [한국어] blk_mq_get_tags: sbitmap 에서 최대 (nr_tags - nr) 개의
		 * 연속 tag 를 비트마스크로 한 번에 획득 시도. tag_offset 은 시작 태그 번호. */
		tag_mask = blk_mq_get_tags(data, data->nr_tags - nr, &tag_offset);
		if (unlikely(!tag_mask)) {
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
 * [한국어]
 * __blk_mq_alloc_requests - hctx(SQ) 와 tag(CID) 를 할당받아 request 를 생성
 *
 * @data: 할당 컨텍스트 — q, flags, cmd_flags, nr_tags, cached_rqs 를 포함
 * @return: 초기화된 request; 고갈 + NOWAIT 시 NULL
 *
 * NVMe 관점: NVMe SQ(hctx)에서 빈 CID 슬롯(tag)을 sbitmap 으로 확보하고
 * blk_mq_rq_ctx_init 으로 request 를 완성한다. 이 함수가 반환한 request 의
 * tag 필드가 곧 NVMe SQ Entry 에 기록될 CID 이다.
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
	struct request_queue *q = data->q;
	/* [한국어] alloc_time_ns: tag 대기를 포함한 전체 할당 지연 측정용 시작 시각 */
	u64 alloc_time_ns = 0;
	struct request *rq;
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
		if (rq) {
			/* [한국어] 배치 성공: 첫 번째 request 에 alloc 시각 기록 후 반환 */
			blk_mq_rq_time_init(rq, alloc_time_ns);
			return rq;
		}
		/* [한국어] 배치 실패: 단일 tag 할당으로 fallback */
		data->nr_tags = 1;
	}

	/*
	 * Waiting allocations only fail because of an inactive hctx.  In that
	 * case just retry the hctx assignment and tag allocation as CPU hotplug
	 * should have migrated us to an online CPU by now.
	 */
	/* [한국어] blk_mq_get_tag: sbitmap 에서 빈 CID 하나를 원자적으로 획득.
	 * 고갈 시 BLK_MQ_NO_TAG(-1) 반환 — 대기 또는 실패 처리 필요. */
	tag = blk_mq_get_tag(data); /* NVMe SQ 의 빈 CID(slot) 확보 */
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
		goto retry;
	}

	/* [한국어] RQF_SCHED_TAGS 가 없으면 driver tag → hctx 활성 request 수 1 증가.
	 * blk_mq_dispatch_rq_list 의 budget 계산에서 사용된다. */
	if (!(data->rq_flags & RQF_SCHED_TAGS))
		blk_mq_inc_active_requests(data->hctx); /* hctx(SQ) 의 활성 CID 카운트 증가 */
	/* [한국어] 획득한 tag 로 request 초기화 후 alloc_time 기록 → 반환 */
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
		blk_mq_put_tag(hctx->tags, ctx, rq->tag); /* NVMe SQ slot(CID) 반납 */
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
		if (rq->end_io(rq, error, NULL) == RQ_END_IO_FREE)
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
 *   nvme_irq / nvme_poll → nvme_process_cq → blk_mq_complete_request_batch
 *     → [blk_mq_end_request_batch]
 */
void blk_mq_end_request_batch(struct io_comp_batch *iob)
{
	/* [한국어] tags[]: hctx 가 동일한 request 들의 tag 를 모아두는 배열.
	 * hctx 가 바뀌면 먼저 flush 후 새 배열에 누적한다. */
	int tags[TAG_COMP_BATCH], nr_tags = 0;
	/* [한국어] cur_hctx: 현재 누적 중인 hctx — 다른 hctx 로 바뀌면 flush 트리거 */
	struct blk_mq_hw_ctx *cur_hctx = NULL;
	struct request *rq;
	/* [한국어] now: 타임스탬프 수집 시 배치 내 모든 request 에 동일한 완료 시각 사용 */
	u64 now = 0;

	/* [한국어] iob->need_ts: 배치 내 어느 request 라도 타임스탬프가 필요하면 미리 기록 */
	if (iob->need_ts)
		now = blk_time_get_ns();

	while ((rq = rq_list_pop(&iob->req_list)) != NULL) {
		/* [한국어] 다음 반복에서 사용할 rq->bio 와 rq_next 를 캐시에 미리 로드.
		 * NVMe high-IOPS 에서 cache miss 를 숨기는 핵심 최적화. */
		prefetch(rq->bio);
		prefetch(rq->rq_next);

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

		blk_crypto_free_request(rq);
		blk_pm_mark_last_busy(rq);

		/* [한국어] batch 버퍼가 가득 찼거나(TAG_COMP_BATCH=32) hctx 가 바뀌면
		 * 지금까지 모은 tag 들을 한 번에 반납(flush)한다.
		 * hctx 가 다르면 같은 tags sbitmap 에 속하지 않으므로 반드시 분리 flush. */
		if (nr_tags == TAG_COMP_BATCH || cur_hctx != rq->mq_hctx) {
			if (cur_hctx)
				/* [한국어] 지금까지 누적된 tag 배열을 hctx 단위로 일괄 반납 */
				blk_mq_flush_tag_batch(cur_hctx, tags, nr_tags);
			/* [한국어] 배열 초기화 후 새 hctx 시작 */
			nr_tags = 0;
			cur_hctx = rq->mq_hctx;
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
	struct request *rq, *next;

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

	cpu = rq->mq_ctx->cpu;
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
 * 실행 컨텍스트: 인터럽트 핸들러 (nvme_irq, nvme_poll_queue 등).
 *
 * 호출 체인:
 *   nvme_irq → nvme_complete_rq → blk_mq_complete_request
 *     → [blk_mq_complete_request_remote] → blk_mq_complete_send_ipi 또는 직접 처리
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
 * nvme_irq 에서는 이 함수를 통하지 않고 nvme_complete_rq 를 직접 호출하기도 한다.
 * 실행 컨텍스트: 인터럽트 핸들러 또는 poll 컨텍스트.
 *
 * 호출 체인:
 *   nvme_poll / nvme_irq → [blk_mq_complete_request]
 *     → blk_mq_complete_request_remote → nvme_complete_rq 또는 IPI
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
	return BLK_MAX_REQUEST_COUNT;
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

	if (!plug->rq_count) {
		/* [한국어] 첫 번째 request 추가: block_plug tracepoint 기록 */
		trace_block_plug(rq->q);
	} else if (plug->rq_count >= blk_plug_max_rq_count(plug) ||
		   /* [한국어] nomerges 가 아니고 마지막 request 가 BLK_PLUG_FLUSH_SIZE
		    * 이상이면 이미 큰 IO 가 쌓였으므로 지금 flush 하는 것이 낫다. */
		   (!blk_queue_nomerges(rq->q) &&
		    blk_rq_bytes(last) >= BLK_PLUG_FLUSH_SIZE)) {
		/* [한국어] 현재 plug 를 flush: 쌓인 request 들을 모두 hctx 에 dispatch */
		blk_mq_flush_plug_list(plug, false);
		/* [한국어] flush 후 last 는 무효 — trace 를 새로 기록 */
		last = NULL;
		trace_block_plug(rq->q);
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
 * @rq:      실행할 passthrough request (nvme_alloc_request 등으로 생성)
 * @at_head: true 면 dispatch list 맨 앞에, false 면 맨 뒤에 삽입
 *
 * NVMe ioctl/admin 명령을 비동기로 발행하는 진입점.
 * scheduler 가 있으면 elevator 를 통해, 없으면 hctx->dispatch 리스트로 바로 간다.
 * plug 가 활성화된 경우 완료 콜백이 호출될 때까지 request 를 plug 에 축적한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl/admin 명령 제출 경로).
 *
 * 호출 체인:
 *   nvme_submit_sync_cmd / nvme_userspace_io → [blk_execute_rq_nowait]
 *   → blk_mq_insert_request → blk_mq_run_hw_queue → nvme_queue_rq
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

	if (current->plug && !at_head) {
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
 * NVMe 에서 poll queue(io_uring IORING_OP_READ + IOSQE_FIXED_FILE + IORING_SETUP_IOPOLL)
 * 를 사용하는 경우 인터럽트 없이 CQ 를 직접 읽어 완료 확인.
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
 * @rq:      실행할 NVMe passthrough request (nvme_alloc_request 등으로 생성)
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

	if (blk_rq_is_poll(rq))
		/* [한국어] poll queue: CQ 를 직접 반복 조회하여 완료 확인 */
		blk_rq_poll_completion(rq, &wait.done);
	else
		/* [한국어] 일반 인터럽트 방식: 완료 인터럽트가 signal 할 때까지 sleep */
		blk_wait_io(&wait.done);

	return wait.ret;
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

	if (blk_mq_request_started(rq)) {
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
	unsigned long flags;

	/* [한국어] tag 반납, 상태 초기화 등 재발행 준비 */
	__blk_mq_requeue_request(rq);

	/* this request will be re-inserted to io scheduler queue */
	/* [한국어] elevator 가 있으면 sched 내부 requeue 처리 (RQF_SOFTBARRIER 등) */
	blk_mq_sched_requeue_request(rq);

	/* [한국어] IRQ 비활성화 후 spinlock: requeue_list 는 인터럽트 컨텍스트에서도 접근 가능 */
	spin_lock_irqsave(&q->requeue_lock, flags);
	/* [한국어] requeue_list 끝에 추가: FIFO 순서로 재발행 */
	list_add_tail(&rq->queuelist, &q->requeue_list);
	spin_unlock_irqrestore(&q->requeue_lock, flags);

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
	LIST_HEAD(flush_list);
	struct request *rq;

	/* [한국어] requeue_lock: requeue_list/flush_list 접근을 IRQ 차단 후 잠금 */
	spin_lock_irq(&q->requeue_lock);
	/* [한국어] requeue_list 전체를 로컬 rq_list 로 이동 — lock 구간 최소화 */
	list_splice_init(&q->requeue_list, &rq_list);
	/* [한국어] flush_list(flush sequence 관련 request) 도 동일하게 이동 */
	list_splice_init(&q->flush_list, &flush_list);
	spin_unlock_irq(&q->requeue_lock);

	while (!list_empty(&rq_list)) {
		/* [한국어] rq_list 앞에서 하나씩 꺼내 재삽입 */
		rq = list_entry(rq_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		/*
		 * If RQF_DONTPREP is set, the request has been started by the
		 * driver already and might have driver-specific data allocated
		 * already.  Insert it into the hctx dispatch list to avoid
		 * block layer merges for the request.
		 */
		if (rq->rq_flags & RQF_DONTPREP)
			/* [한국어] RQF_DONTPREP: 드라이버가 이미 NVMe SQE 를 준비한 경우.
			 * elevator merge 를 우회하고 hctx->dispatch 에 직접 삽입. */
			blk_mq_request_bypass_insert(rq, 0);
		else
			/* [한국어] 일반 requeue: 큐 앞머리에 삽입하여 다음 run 에서 우선 발행 */
			blk_mq_insert_request(rq, BLK_MQ_INSERT_AT_HEAD);
	}

	while (!list_empty(&flush_list)) {
		/* [한국어] flush_list: blk-flush.c 의 flush sequence request — 일반 삽입 */
		rq = list_entry(flush_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		blk_mq_insert_request(rq, 0);
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
	return busy;
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
 *   → nvme_timeout → nvme_abort_req 또는 nvme_reset_ctrl
 */
static void blk_mq_rq_timed_out(struct request *req)
{
	/* [한국어] RQF_TIMED_OUT: 이 request 가 만료되었음 표시
	 * blk_mq_req_expired 에서 두 번 만료 처리되는 것을 방지 */
	req->rq_flags |= RQF_TIMED_OUT;
	if (req->q->mq_ops->timeout) {
		enum blk_eh_timer_return ret;

		/* [한국어] nvme_timeout: NVMe 드라이버의 timeout error handler.
		 * abort 성공 → BLK_EH_DONE (완료로 처리)
		 * reset 필요 → BLK_EH_RESET_TIMER (타이머 재설정) */
		ret = req->q->mq_ops->timeout(req);
		if (ret == BLK_EH_DONE)
			/* [한국어] timeout handler 가 직접 request 를 완료 처리 — 종료 */
			return;
		WARN_ON_ONCE(ret != BLK_EH_RESET_TIMER);
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

	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
		/* [한국어] 아직 드라이버에 발행되지 않은 request 는 timeout 대상 아님 */
		return false;
	if (rq->rq_flags & RQF_TIMED_OUT)
		/* [한국어] 이미 timeout 처리된 request 는 중복 처리 방지 */
		return false;

	/* [한국어] READ_ONCE: blk_add_timer 가 다른 컨텍스트에서 deadline 을 갱신하는
	 * 것과의 torn read 를 방지 */
	deadline = READ_ONCE(rq->deadline);
	if (time_after_eq(expired->timeout_start, deadline))
		/* [한국어] timeout_start >= deadline: 현재 시각이 deadline 을 넘었으므로 만료 */
		return true;

	/* [한국어] 아직 만료 안 됨: expired->next 를 가장 이른 deadline 으로 갱신
	 * blk_mq_timeout_work 가 이 값으로 타이머를 재설정한다. */
	if (expired->next == 0)
		expired->next = deadline;
	else if (time_after(expired->next, deadline))
		expired->next = deadline;
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
	} else if (req_ref_put_and_test(rq)) {
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
	if (blk_mq_req_expired(rq, expired)) {
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

	if (blk_mq_req_expired(rq, expired))
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
	if (expired.has_timedout_rq) {
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

	if (expired.next != 0) {
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
	spin_unlock(&ctx->lock);
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
		.list = list,
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
	if (!list_empty(&ctx->rq_lists[type])) {
		/* [한국어] rq_lists 맨 앞 request 를 꺼냄 — FIFO 순서 유지 */
		dispatch_data->rq = list_entry_rq(ctx->rq_lists[type].next);
		/* [한국어] list_del_init: request 를 sw queue 에서 분리 */
		list_del_init(&dispatch_data->rq->queuelist);
		if (list_empty(&ctx->rq_lists[type]))
			/* [한국어] sw queue 가 비었으면 ctx_map 비트 클리어 —
			 * 다음 flush_busy_ctxs 가 이 sw queue 를 순회하지 않음 */
			sbitmap_clear_bit(sb, bitnr);
	}
	spin_unlock(&ctx->lock);

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
		.rq   = NULL,
	};

	/* [한국어] __sbitmap_for_each_set: off 부터 순회 (wrap-around 포함)
	 * dispatch_rq_from_ctx 가 request 를 찾으면 false 반환 → 순회 중단 */
	__sbitmap_for_each_set(&hctx->ctx_map, off,
			       dispatch_rq_from_ctx, &data);

	return data.rq;
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
	int tag;

	/* [한국어] blk_mq_tag_busy: hctx 를 busy 상태로 표시 — idle 타이머 방지 */
	blk_mq_tag_busy(rq->mq_hctx);

	if (blk_mq_tag_is_reserved(rq->mq_hctx->sched_tags, rq->internal_tag)) {
		/* [한국어] scheduler reserved tag 이면 별도의 reserved CID pool 에서 할당.
		 * offset 0: breserved_tags 는 비트 0 부터 직접 사용 */
		bt = &rq->mq_hctx->tags->breserved_tags;
		tag_offset = 0;
	} else {
		if (!hctx_may_queue(rq->mq_hctx, bt))
			/* [한국어] hctx_may_queue: queue depth 예산 초과 확인.
			 * false 이면 SQ 가 허용 깊이에 도달 — CID 할당 거부 */
			return false;
	}

	/* [한국어] __sbitmap_queue_get: lock-free 원자적 비트 할당.
	 * 성공 시 비트 번호(0~depth-1), 실패 시 BLK_MQ_NO_TAG(==-1) */
	tag = __sbitmap_queue_get(bt);
	if (tag == BLK_MQ_NO_TAG)
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
	if (!list_empty(&wait->entry)) {
		struct sbitmap_queue *sbq;

		/* [한국어] wait queue 에서 hctx 제거 — 중복 wakeup 방지 */
		list_del_init(&wait->entry);
		/* [한국어] bitmap_tags: 일반 tag pool sbitmap */
		sbq = &hctx->tags->bitmap_tags;
		/* [한국어] ws_active 감소: sbitmap wakeup 배치의 활성 카운터 감소 */
		atomic_dec(&sbq->ws_active);
	}
	spin_unlock(&hctx->dispatch_wait_lock);

	/* [한국어] blk_mq_run_hw_queue(hctx, true): sync=true 로 즉시 dispatch 시도.
	 * NVMe CID 가 반납되었으므로 requeue 된 request 를 다시 발행 가능 */
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
	struct wait_queue_head *wq;
	wait_queue_entry_t *wait;
	bool ret;

	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) &&
	    !(blk_mq_is_shared_tags(hctx->flags))) {
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
	else
		sbq = &hctx->tags->bitmap_tags;
	/* [한국어] bt_wait_ptr: sbitmap 의 wakeup wait queue head 를 해시로 선택 */
	wq = &bt_wait_ptr(sbq, hctx)->wait;

	/* [한국어] wq->lock 먼저, dispatch_wait_lock 다음: 락 순서 고정 (역순 방지) */
	spin_lock_irq(&wq->lock);
	spin_lock(&hctx->dispatch_wait_lock);
	if (!list_empty(&wait->entry)) {
		/* [한국어] 경쟁으로 다른 코드가 먼저 등록: 중복 등록 방지 후 실패 반환 */
		spin_unlock(&hctx->dispatch_wait_lock);
		spin_unlock_irq(&wq->lock);
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
	/* [한국어] CID 를 얻었으므로 wait queue 에서 자기 자신 제거.
	 * 다른 wakeup 대상을 놓치지 않도록 배타적 wakeup 방지 */
	list_del_init(&wait->entry);
	/* [한국어] ws_active 감소: wakeup 배치에서 이 wait entry 제거 */
	atomic_dec(&sbq->ws_active);
	spin_unlock(&hctx->dispatch_wait_lock);
	spin_unlock_irq(&wq->lock);

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
	if (busy)
		ewma += 1 << BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR;
	ewma /= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT;

	/* [한국어] 갱신된 EWMA 저장 — 다음 dispatch 경로 결정에 반영 */
	hctx->dispatch_busy = ewma;
}

/* [한국어] BLK_MQ_RESOURCE_DELAY: NVMe controller BLK_STS_RESOURCE 후 재시도 지연(ms) */
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

	if (need_budget) {
		/* [한국어] blk_mq_get_dispatch_budget: queue depth / throttle 예산 확인 */
		budget_token = blk_mq_get_dispatch_budget(rq->q);
		if (budget_token < 0) {
			/* [한국어] budget 고갈: 이미 확보한 driver tag 반납 후 NO_BUDGET 반환 */
			blk_mq_put_driver_tag(rq);
			return PREP_DISPATCH_NO_BUDGET;
		}
		/* [한국어] budget_token 을 request 에 저장 — 완료 시 put_budget 에서 반납 */
		blk_mq_set_rq_budget_token(rq, budget_token);
	}

	if (!blk_mq_get_driver_tag(rq)) {
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
			return PREP_DISPATCH_NO_TAG;
		}
	}

	return PREP_DISPATCH_OK;
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

	list_for_each_entry(rq, list, queuelist) {
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
 * mq_ops->commit_rqs (NVMe: nvme_commit_rqs) 를 호출하여
 * 드라이버에게 이 batch 의 마지막 request 가 발행되었음을 알린다.
 * NVMe 드라이버는 이 시점에 doorbell 을 기록한다.
 * batch dispatch 도중 실패했거나 비정상 종료 시에도 명시적으로 호출 필요.
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
 *   → [blk_mq_dispatch_rq_list] → mq_ops->queue_rq → nvme_queue_rq
 *   → nvme_submit_cmd (SQ doorbell 기록)
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
		if (prep != PREP_DISPATCH_OK)
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
		switch (ret) {
		case BLK_STS_OK:
			/* [한국어] NVMe SQ 에 명령 성공적 배치 — CQ 완료 인터럽트 대기 */
			queued++;
			break;
		case BLK_STS_RESOURCE:
			/* [한국어] BLK_STS_RESOURCE: NVMe SQ/PRP list/SGL 자원 부족.
			 * needs_resource = true 로 3ms 지연 후 재시도 예약 */
			needs_resource = true;
			fallthrough;
		case BLK_STS_DEV_RESOURCE:
			/* [한국어] BLK_STS_DEV_RESOURCE: NVMe 컨트롤러 내부 자원 고갈.
			 * blk_mq_handle_dev_resource: rq 를 list 앞으로 돌려보내고 CID 반납 */
			blk_mq_handle_dev_resource(rq, list);
			goto out;
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
	if (!list_empty(list)) {
		bool needs_restart;
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
		spin_unlock(&hctx->lock);

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
		if (!needs_restart ||
		    (no_tag && list_empty_careful(&hctx->dispatch_wait.entry)))
			/* [한국어] SCHED_RESTART 가 없거나 no_tag + dispatch_wait 비어있으면
			 * 즉시 hctx 를 rerun — dispatch 에 추가된 request 를 바로 처리 */
			blk_mq_run_hw_queue(hctx, true);
		else if (needs_resource)
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
	return cpu;
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
	int next_cpu = hctx->next_cpu;

	/* Switch to unbound if no allowable CPUs in this hctx */
	/* [한국어] SQ 가 1개이거나 cpumask 가 비어있으면 CPU 를 고집할 필요 없음 */
	if (hctx->queue->nr_hw_queues == 1 || blk_mq_hctx_empty_cpumask(hctx))
		return WORK_CPU_UNBOUND;

	if (--hctx->next_cpu_batch <= 0) {
select_cpu:
		/* [한국어] cpumask_next_and: hctx->cpumask ∩ cpu_online_mask 에서
		 * next_cpu 다음 CPU 를 선택 (round-robin, wrap-around 포함) */
		next_cpu = cpumask_next_and(next_cpu, hctx->cpumask,
				cpu_online_mask);
		if (next_cpu >= nr_cpu_ids)
			/* [한국어] cpumask 끝에 도달하면 첫 번째 CPU 로 돌아감 */
			next_cpu = blk_mq_first_mapped_cpu(hctx);
		/* [한국어] BLK_MQ_CPU_WORK_BATCH: 같은 CPU 에서 실행할 batch 크기 초기화 */
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}

	/*
	 * Do unbound schedule if we can't find a online CPU for this hctx,
	 * and it should only happen in the path of handling CPU DEAD.
	 */
	if (!cpu_online(next_cpu)) {
		if (!tried) {
			/* [한국어] 선택된 CPU 가 오프라인: 한 번 더 select 시도 */
			tried = true;
			goto select_cpu;
		}

		/*
		 * Make sure to re-select CPU next time once after CPUs
		 * in hctx->cpumask become online again.
		 */
		/* [한국어] 모든 cpumask CPU 가 오프라인: unbound 로 fallback.
		 * next_cpu_batch=1: 다음 호출 시 즉시 재선택하여 온라인 CPU 를 찾을 기회 */
		hctx->next_cpu = next_cpu;
		hctx->next_cpu_batch = 1;
		return WORK_CPU_UNBOUND;
	}

	/* [한국어] 선택된 CPU 저장: 다음 BLK_MQ_CPU_WORK_BATCH 개 work 에서 재사용 */
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
 * [한국어]
 * blk_mq_delay_run_hw_queue - hctx 의 dispatch 를 kblockd 에 지연 예약
 *
 * @hctx: 실행할 hardware queue
 * @msecs: 지연 시간 (밀리초; 0 이면 즉시 예약)
 *
 * blk_mq_run_hw_queue 의 async 버전. kblockd workqueue 에 hctx->run_work 를 예약한다.
 * BLK_STS_RESOURCE 후 BLK_MQ_RESOURCE_DELAY(3ms) 재시도, 또는 즉시 예약 (0ms) 에 사용.
 * BLK_MQ_S_STOPPED 상태이면 예약하지 않음 (nvme_start_queue 가 재시작 처리).
 */
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs)
{
	/* [한국어] BLK_MQ_S_STOPPED: NVMe SQ 가 reset/error 로 정지된 상태.
	 * 재시작 전에는 dispatch 예약 불가 */
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
	if (!need_run) {
		unsigned long flags;

		/*
		 * Synchronize with blk_mq_unquiesce_queue(), because we check
		 * if hw queue is quiesced locklessly above, we need the use
		 * ->queue_lock to make sure we see the up-to-date status to
		 * not miss rerunning the hw queue.
		 */
		/* [한국어] 2차 확인: queue_lock 으로 unquiesce_queue 와 동기화.
		 * lockless 체크 후 quiesce 가 해제되는 race 를 방지 */
		spin_lock_irqsave(&hctx->queue->queue_lock, flags);
		need_run = blk_mq_hw_queue_need_run(hctx);
		spin_unlock_irqrestore(&hctx->queue->queue_lock, flags);

		if (!need_run)
			return;
	}

	if (async || !cpumask_test_cpu(raw_smp_processor_id(), hctx->cpumask)) {
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
	return NULL;
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
	unsigned long i;

	sq_hctx = NULL;
	/* [한국어] blk_queue_sq_sched: IO scheduler 가 single-queue 방식이면 true */
	if (blk_queue_sq_sched(q))
		/* [한국어] 현재 CPU 의 기본 hctx 를 dispatch 대상으로 고정 */
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {
		if (blk_mq_hctx_stopped(hctx))
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
	unsigned long i;

	sq_hctx = NULL;
	/* [한국어] single-queue scheduler 이면 기본 hctx 만 dispatch */
	if (blk_queue_sq_sched(q))
		sq_hctx = blk_mq_get_sq_hctx(q);
	queue_for_each_hw_ctx(q, hctx, i) {
		if (blk_mq_hctx_stopped(hctx))
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
	unsigned long i;

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
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_start_hw_queue(hctx);
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
	blk_mq_run_hw_queue(hctx, async);
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
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
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
	if (flags & BLK_MQ_INSERT_AT_HEAD)
		/* [한국어] 맨 앞 삽입: 다음 dispatch 에서 가장 먼저 처리됨 */
		list_add(&rq->queuelist, &hctx->dispatch);
	else
		/* [한국어] 끝 삽입: FIFO 순서 유지 */
		list_add_tail(&rq->queuelist, &hctx->dispatch);
	spin_unlock(&hctx->lock);
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
			blk_mq_try_issue_list_directly(hctx, list));
		if (list_empty(list))
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
		trace_block_rq_insert(rq);
		if (rq->cmd_flags & REQ_NOWAIT)
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
	spin_unlock(&ctx->lock);
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
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

	if (blk_rq_is_passthrough(rq)) {
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
	} else if (req_op(rq) == REQ_OP_FLUSH) {
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
		 * NVMe NCQ 에서 flush 는 non-NCQ 명령이라 in-flight NCQ IO 가 없어야 발행 가능.
		 * 앞에 삽입하면 flush merge 기회 증가 → controller 에 전달되는 flush 수 감소 */
		blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD);
	} else if (q->elevator) {
		/* [한국어] IO scheduler 가 있으면 elevator queue 에 삽입.
		 * elevator 가 나중에 dispatch 할 때 최적 순서로 꺼냄 */
		LIST_HEAD(list);

		/* [한국어] elevator 경로에서는 rq->tag 가 미할당 상태 (sched tag 만 사용) */
		WARN_ON_ONCE(rq->tag != BLK_MQ_NO_TAG);

		list_add(&rq->queuelist, &list);
		/* [한국어] elevator->ops.insert_requests: scheduler 내부 자료구조에 삽입.
		 * mq-deadline: RB-tree, BFQ: B+ tree 등으로 IO 순서 결정 */
		q->elevator->type->ops.insert_requests(hctx, &list, flags);
	} else {
		/* [한국어] elevator 없는 직접 경로: sw queue (ctx->rq_lists) 에 삽입 */
		trace_block_rq_insert(rq);

		spin_lock(&ctx->lock);
		if (flags & BLK_MQ_INSERT_AT_HEAD)
			/* [한국어] BLK_MQ_INSERT_AT_HEAD: sw queue 맨 앞에 삽입 */
			list_add(&rq->queuelist, &ctx->rq_lists[hctx->type]);
		else
			list_add_tail(&rq->queuelist,
				      &ctx->rq_lists[hctx->type]);
		/* [한국어] hctx->ctx_map 에 pending 비트 설정 */
		blk_mq_hctx_mark_pending(hctx, ctx);
		spin_unlock(&ctx->lock);
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

	if (bio->bi_opf & REQ_RAHEAD)
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
	if (bio_integrity(bio))
		/* [한국어] NVMe PI(Protection Information): 별도 metadata scatter-gather 필요 */
		rq->nr_integrity_segments = blk_rq_count_integrity_sg(rq->q,
								      bio);

	/* This can't fail, since GFP_NOIO includes __GFP_DIRECT_RECLAIM. */
	/* [한국어] blk_crypto_rq_bio_prep: NVMe inline encryption keyslot + DUN 설정.
	 * GFP_NOIO 라 direct reclaim 가능 → 실패 없음 */
	err = blk_crypto_rq_bio_prep(rq, bio, GFP_NOIO);
	WARN_ON_ONCE(err);

	/* [한국어] IO 통계 기록 시작: iostat/blk-cgroup 계정, rq->start_time_ns */
	blk_account_io_start(rq);
}

/*
 * [한국어]
 * __blk_mq_issue_directly - 하나의 request 를 mq_ops->queue_rq 로 즉시 발행
 *
 * @hctx: 발행할 hardware queue
 * @rq:   발행할 request
 * @last: true 이면 이 request 가 batch 의 마지막 (드라이버가 doorbell 기록)
 * @return: queue_rq 결과 (BLK_STS_OK, BLK_STS_RESOURCE 등)
 *
 * blk_mq_try_issue_directly 의 내부 helper.
 * nvme_queue_rq → NVMe SQ entry 기록 → doorbell 로 이어진다.
 */
static blk_status_t __blk_mq_issue_directly(struct blk_mq_hw_ctx *hctx,
					    struct request *rq, bool last)
{
	struct request_queue *q = rq->q;
	/* [한국어] bd: nvme_queue_rq 에 전달하는 dispatch 컨텍스트 */
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
	/* [한국어] mq_ops->queue_rq: nvme_queue_rq → NVMe SQ entry 기록 + doorbell */
	ret = q->mq_ops->queue_rq(hctx, &bd);
	switch (ret) {
	case BLK_STS_OK:
		/* [한국어] 성공: dispatch_busy EWMA 감소 */
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	case BLK_STS_RESOURCE:
	case BLK_STS_DEV_RESOURCE:
		/* [한국어] 자원 부족: EWMA 증가 후 request 를 재발행 대기 상태로 */
		blk_mq_update_dispatch_busy(hctx, true);
		__blk_mq_requeue_request(rq);
		break;
	default:
		/* [한국어] 기타 오류: EWMA 는 감소 (busy 아님) */
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	}

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
	if (budget_token < 0)
		return false;
	/* [한국어] budget_token 을 request 에 저장 — 완료 시 put_budget 에서 반납 */
	blk_mq_set_rq_budget_token(rq, budget_token);
	if (!blk_mq_get_driver_tag(rq)) {
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

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {
		/* [한국어] STOPPED 또는 quiesced: 즉시 발행 불가.
		 * insert 후 run_hw_queue 로 재시도 예약 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);
		return;
	}

	if ((rq->rq_flags & RQF_USE_SCHED) || !blk_mq_get_budget_and_tag(rq)) {
		/* [한국어] RQF_USE_SCHED: elevator 를 사용하는 request — 직접 발행 불가.
		 * 또는 budget/CID 확보 실패: sw queue/scheduler 로 위임 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, rq->cmd_flags & REQ_NOWAIT);
		return;
	}

	/* [한국어] last=true: 이 request 가 이 batch 의 마지막 → doorbell 기록 */
	ret = __blk_mq_issue_directly(hctx, rq, true);
	switch (ret) {
	case BLK_STS_OK:
		/* [한국어] 발행 성공: 완료를 CQ 완료 인터럽트에서 기다림 */
		break;
	case BLK_STS_RESOURCE:
	case BLK_STS_DEV_RESOURCE:
		/* [한국어] 자원 부족: hctx->dispatch 에 삽입 후 async run 예약.
		 * dispatch list 는 sw queue 보다 우선 처리됨 */
		blk_mq_request_bypass_insert(rq, 0);
		blk_mq_run_hw_queue(hctx, false);
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

	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(rq->q)) {
		/* [한국어] 즉시 발행 불가: insert 후 async run 예약 */
		blk_mq_insert_request(rq, 0);
		blk_mq_run_hw_queue(hctx, false);
		return BLK_STS_OK;
	}

	if (!blk_mq_get_budget_and_tag(rq))
		/* [한국어] budget/CID 확보 실패: 호출자가 재시도 처리 */
		return BLK_STS_RESOURCE;
	return __blk_mq_issue_directly(hctx, rq, last);
}

/*
 * [한국어]
 * blk_mq_issue_direct - rq_list 의 request 들을 순서대로 직접 발행
 *
 * @rqs: 발행할 request list
 *
 * plug flush 또는 direct submit 경로에서 여러 request 를 직접 발행한다.
 * hctx 가 바뀔 때마다 이전 hctx 의 commit_rqs 로 batch 를 마무리한다.
 * RESOURCE 오류 시 남은 request 를 bypass_insert + run_hw_queue.
 *
 * 호출 체인:
 *   blk_mq_flush_plug_list → [blk_mq_issue_direct]
 *   → blk_mq_request_issue_directly → __blk_mq_issue_directly → nvme_queue_rq
 */
static void blk_mq_issue_direct(struct rq_list *rqs)
{
	struct blk_mq_hw_ctx *hctx = NULL;
	struct request *rq;
	int queued = 0;
	blk_status_t ret = BLK_STS_OK;

	while ((rq = rq_list_pop(rqs))) {
		/* [한국어] last: 이 rq 다음에 rqs 가 비면 batch 마지막 */
		bool last = rq_list_empty(rqs);

		if (hctx != rq->mq_hctx) {
			/* [한국어] hctx 가 바뀌면 이전 hctx 의 batch 를 commit (doorbell 유도) */
			if (hctx) {
				blk_mq_commit_rqs(hctx, queued, false);
				queued = 0;
			}
			hctx = rq->mq_hctx;
		}

		ret = blk_mq_request_issue_directly(rq, last);
		switch (ret) {
		case BLK_STS_OK:
			queued++;
			break;
		case BLK_STS_RESOURCE:
		case BLK_STS_DEV_RESOURCE:
			/* [한국어] 자원 부족: bypass_insert 후 run 예약하고 loop 종료.
			 * 남은 request 들은 다음 run 에서 처리 */
			blk_mq_request_bypass_insert(rq, 0);
			blk_mq_run_hw_queue(hctx, false);
			goto out;
		default:
			blk_mq_end_request(rq, ret);
			break;
		}
	}

out:
	/* [한국어] 오류 발생 시 지금까지 발행한 request 들의 batch commit */
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
