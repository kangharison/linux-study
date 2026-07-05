// SPDX-License-Identifier: GPL-2.0
/*
 * The Kyber I/O scheduler. Controls latency by throttling queue depths using
 * scalable techniques.
 *
 * Copyright (C) 2017 Facebook
 */

/*
 * [한국어 설명] Kyber I/O 스케줄러 구현 (kyber-iosched.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux blk-mq(block multi-queue) 프레임워크 위에서 동작하는
 * Kyber I/O 스케줄러의 전체 구현을 담는다. Kyber는 저지연(low-latency) NVMe
 * SSD를 위해 Facebook이 설계한 멀티큐 스케줄러로, I/O 요청을 read/write/
 * discard/other 네 개의 "스케줄링 도메인"으로 분류하고 각 도메인에 독립적인
 * token-bucket(sbitmap_queue 기반) 방식의 in-flight 한도를 부과한다.
 * 도메인별 완료 지연(latency)을 CoDel-유사 방식으로 측정하여, p90/p99가
 * 목표를 초과하면 해당 도메인의 queue depth를 줄이고 목표 이내이면 늘리는
 * 피드백 루프를 통해 NVMe SSD의 병렬성(대역폭)과 지연 시간을 동시에 최적화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 I/O 스택 계층 구조에서 이 파일은 blk-mq와 NVMe 드라이버 사이에 위치한다:
 *
 *   VFS / page cache
 *        ↓  bio 생성
 *   generic_make_request / blk_mq_submit_bio
 *        ↓  request 할당 (kyber_limit_depth: async shallow depth 제한)
 *   [Kyber 스케줄러] ← 이 파일
 *     - kyber_bio_merge:      bio를 kcq(per-ctx 큐)의 기존 request에 merge
 *     - kyber_insert_requests: request를 kcq/domain 대기열에 삽입
 *     - kyber_dispatch_request: domain_tokens 획득 후 hctx dispatch 리스트에 request 선택
 *        ↓  dispatch된 request
 *   nvme_queue_rq → nvme_submit_cmd → NVMe SQ tail 업데이트 (doorbell)
 *        ↓  NVMe SSD 처리 완료 후 CQ entry 생성
 *   nvme_irq → nvme_complete_rq → blk_mq_end_request
 *        ↓  kyber_completed_request: per-cpu latency histogram 기록
 *   kyber_timer_fn: 100ms마다 p90/p99 집계 후 domain_tokens depth 재조정
 *
 * 실행 컨텍스트: 이 파일의 함수들은 (1) 프로세스 컨텍스트(bio 제출, dispatch
 * 루프), (2) 소프트 IRQ/인터럽트(NVMe CQ 완료 처리), (3) 타이머 컨텍스트
 * (kyber_timer_fn)에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위 의존: blk-mq.c/blk-mq-sched.c — request 생성/dispatch/완료 훅 등록.
 *   elevator.c — elevator_type 등록 및 콜백 호출.
 * - 하위 호출: sbitmap/sbitmap_queue — token-bucket 구현체. 비트맵 기반으로
 *   lock-free에 가깝게 in-flight 토큰을 관리한다.
 * - NVMe 드라이버(drivers/nvme/host/pci.c 등)는 blk-mq를 통해 이 스케줄러를
 *   간접적으로 사용한다. nvme_queue_rq가 Kyber가 선택한 request를 받아 SQ에
 *   삽입한다.
 * - 데이터 흐름: bio→kcq.rq_list→khd.rqs[]→dispatch→nvme_submit_cmd.
 *   완료 지연은 CQ ISR → per_cpu(cpu_latency) → kyber_timer_fn → domain_tokens
 *   피드백 루프로 흐른다.
 * - 공유 자료구조: kyber_queue_data(request_queue별), kyber_hctx_data(hctx별),
 *   kyber_ctx_queue(software ctx별).
 *
 * === 주요 함수/구조체 요약 ===
 * kyber_dispatch_request()  : blk-mq가 호출하는 핵심 dispatch 함수. 현재 도메인의
 *                             domain_tokens를 획득하여 NVMe SQ로 보낼 request 반환.
 * kyber_timer_fn()          : 100ms 주기 타이머. per-cpu latency를 집계하고 p90/p99
 *                             기반으로 domain_tokens depth를 동적 조절.
 * kyber_completed_request() : NVMe CQ 완료 시 per-cpu histogram에 지연 기록.
 * kyber_get_domain_token()  : domain_tokens에서 in-flight 허가 토큰 획득. 없으면
 *                             wait queue 등록(NVMe CQ 완료 시 kyber_domain_wake로 깨어남).
 * kyber_limit_depth()       : request 할당 시 비동기 요청의 tag/CID shallow depth 제한.
 * struct kyber_queue_data   : request_queue 단위 스케줄러 상태. domain_tokens,
 *                             cpu_latency, timer, latency_targets 포함.
 * struct kyber_hctx_data    : hctx(NVMe SQ) 단위 상태. kcqs, kcq_map, cur_domain,
 *                             batching, domain_wait 포함.
 * struct kyber_ctx_queue    : software ctx 단위 대기열. 도메인별 rq_list와 lock 포함.
 */

#include <linux/kernel.h>	/* [한국어] printk, min/max, ARRAY_SIZE 등 커널 기본 매크로 */
#include <linux/blkdev.h>	/* [한국어] blk-mq request_queue, bio, request 정의: NVMe SQ/CQ 상위 계층 */
#include <linux/module.h>	/* [한국어] module_init/exit, MODULE_* 매크로: Kyber를 로드 가능한 모듈로 등록 */
#include <linux/sbitmap.h>	/* [한국어] sbitmap_queue — domain_tokens의 token-bucket 구현체. NVMe in-flight CID/entry 할당·해제를 원자적으로 처리 */

#include <trace/events/block.h>	/* [한국어] trace_block_rq_insert 등 블록 계층 트레이스 이벤트 */

#include "elevator.h"		/* [한국어] elevator_type, elevator_queue: blk-mq와 NVMe 드라이버 사이 스케줄러 인터페이스 */
#include "blk.h"		/* [한국어] blk_stat_enable_accounting 등 블록 계층 내부 헬퍼 */
#include "blk-mq.h"		/* [한국어] blk_mq_hw_ctx, blk_mq_ctx, blk_mq_run_hw_queue: NVMe SQ 선택의 핵심 자료구조 */
#include "blk-mq-debugfs.h"	/* [한국어] blk_mq_debugfs_attr: debugfs를 통해 hctx별 큐 상태 노출 */
#include "blk-mq-sched.h"	/* [한국어] blk-mq 스케줄러 헬퍼: dispatch/insert/requeue/merge 경로 정의 */

#define CREATE_TRACE_POINTS	/* [한국어] 이 파일에서 kyber 트레이스 포인트를 최초로 정의함 (다른 파일은 include만) */
#include <trace/events/kyber.h>	/* [한국어] trace_kyber_latency/throttled/adjust 이벤트: perf/ftrace로 Kyber 동작 관찰 */

/*
 * Scheduling domains: the device is divided into multiple domains based on the
 * request type.
 */
/* [한국어] 스케줄링 도메인 열거형 — I/O 요청을 성격에 따라 4개 버킷으로 분류한다.
 * 각 도메인은 독립적인 domain_tokens(sbitmap_queue)와 latency_target을 가진다.
 * 도메인 인덱스는 kyber_depth[], kyber_latency_targets[], kyber_batch_size[]의
 * 배열 인덱스로 사용된다. */
enum {
	KYBER_READ,		/* [한국어] REQ_OP_READ 요청 — NVMe 관점에서 NVME_CMD_READ에 매핑.
				 * 목표 지연 2ms, 최대 in-flight depth 256, batch 크기 16. */
	KYBER_WRITE,		/* [한국어] REQ_OP_WRITE 요청 — NVME_CMD_WRITE에 매핑.
				 * 목표 지연 10ms, 최대 in-flight depth 128, batch 크기 8.
				 * write amplification / buffer flush를 고려해 read보다 작게 설정. */
	KYBER_DISCARD,		/* [한국어] REQ_OP_DISCARD 요청 — NVME_CMD_DSM(Dataset Management)/TRIM에 매핑.
				 * 목표 지연 5s, 최대 in-flight depth 64, batch 크기 1.
				 * SSD 내부 GC 부하를 유발하므로 가장 보수적으로 제한. */
	KYBER_OTHER,		/* [한국어] REQ_OP_FLUSH, vendor-specific, admin passthrough 등 기타.
				 * latency target이 없어 histogram 기록 대상에서 제외.
				 * 최대 in-flight depth 16, batch 크기 1. */
	KYBER_NUM_DOMAINS,	/* [한국어] 도메인 총 개수 = 4. 배열 크기 선언에 사용. */
};

/* [한국어] 도메인 인덱스 → 문자열 매핑 테이블. trace_kyber_ 계열 트레이스포인트와 debugfs 출력에 사용.
 * 지정 초기화(designated initializer)로 인덱스와 이름이 항상 일치하도록 보장. */
static const char *kyber_domain_names[] = {
	[KYBER_READ] = "READ",		/* [한국어] trace_kyber_throttled/adjust 에서 "READ" 도메인 식별자 */
	[KYBER_WRITE] = "WRITE",	/* [한국어] trace_kyber_throttled/adjust 에서 "WRITE" 도메인 식별자 */
	[KYBER_DISCARD] = "DISCARD",	/* [한국어] trace_kyber_throttled/adjust 에서 "DISCARD" 도메인 식별자 */
	[KYBER_OTHER] = "OTHER",	/* [한국어] trace_kyber_throttled/adjust 에서 "OTHER" 도메인 식별자 */
};

enum {
	/*
	 * In order to prevent starvation of synchronous requests by a flood of
	 * asynchronous requests, we reserve 25% of requests for synchronous
	 * operations.
	 */
	/* [한국어] 비동기 요청(async write 등)이 점유할 수 있는 tag/CID의 최대 비율(%).
	 * 전체 nr_requests의 75%만 async에 허용 → 나머지 25%는 sync read 전용으로 예약.
	 * kyber_init_sched에서 q->async_depth = nr_requests * 75 / 100으로 계산되어
	 * kyber_limit_depth()에서 비동기 요청의 shallow_depth로 사용된다.
	 * 이 비율이 낮을수록 sync read가 유리하지만 async write 처리량이 감소한다. */
	KYBER_DEFAULT_ASYNC_PERCENT = 75,
};
/*
 * Maximum device-wide depth for each scheduling domain.
 *
 * Even for fast devices with lots of tags like NVMe, you can saturate the
 * device with only a fraction of the maximum possible queue depth. So, we cap
 * these to a reasonable value.
 */
/* [한국어] 도메인별 장치 전체(device-wide) in-flight 상한 — domain_tokens sbitmap의 최대 깊이.
 * NVMe SSD는 SQ 크기(보통 1024~4096)보다 훨씬 작은 in-flight 수로도 성능이 포화된다.
 * kyber_queue_data_alloc()에서 sbitmap_queue_init_node()의 depth 인자로 사용.
 * kyber_resize_domain()이 이 값을 상한으로 clamp하여 depth를 동적 조절한다. */
static const unsigned int kyber_depth[] = {
	/* [한국어] KYBER_READ 도메인 최대 in-flight 256.
	 * NVMe read SQ에서 동시에 진행할 수 있는 CID(Command ID)/entry의 상한.
	 * 실험적으로 256개가 고성능 NVMe를 포화시키기에 충분하다고 알려져 있다. */
	[KYBER_READ] = 256,
	/* [한국어] KYBER_WRITE 도메인 최대 in-flight 128.
	 * write amplification과 flush/fsync 연동 지연을 고려해 read보다 작게 설정.
	 * 쓰기는 SSD 내부에서 버퍼링 및 FTL 처리가 필요해 지연 분산이 크다. */
	[KYBER_WRITE] = 128,
	/* [한국어] KYBER_DISCARD 도메인 최대 in-flight 64.
	 * NVMe DSM(Dataset Management, TRIM) 명령은 SSD GC를 유발하므로
	 * in-flight를 64로 제한하여 GC로 인한 지연 스파이크를 억제한다. */
	[KYBER_DISCARD] = 64,
	/* [한국어] KYBER_OTHER 도메인 최대 in-flight 16.
	 * FLUSH, vendor-specific, passthrough 명령은 ordering 보장이 중요하므로
	 * 가장 작은 16으로 제한하여 병렬 발행을 최소화한다. */
	[KYBER_OTHER] = 16,
};

/*
 * Default latency targets for each scheduling domain.
 */
/* [한국어] 도메인별 기본 목표 지연 시간 (나노초 단위).
 * kyber_queue_data_alloc()에서 kqd->latency_targets[]로 복사되며,
 * sysfs(read_lat_nsec, write_lat_nsec)를 통해 런타임에 사용자가 조정 가능.
 * KYBER_OTHER는 latency target이 없어 이 배열의 인덱스 범위 밖(KYBER_OTHER=3)이므로
 * 배열 크기가 KYBER_OTHER(=3)까지만 정의된다.
 * add_latency_sample()에서 target 대비 latency 비율로 histogram bucket을 결정한다. */
static const u64 kyber_latency_targets[] = {
	/* [한국어] KYBER_READ 목표 지연 = 2ms.
	 * PCIe Gen3/4 NVMe SSD의 read 지연은 보통 50~200µs이므로, 2ms는
	 * 소프트웨어 큐 대기(queue wait) 시간을 포함해 여유있게 설정한 값이다.
	 * 이 값을 초과하는 p99가 관찰되면 kyber_timer_fn이 depth를 줄인다. */
	[KYBER_READ] = 2ULL * NSEC_PER_MSEC,
	/* [한국어] KYBER_WRITE 목표 지연 = 10ms.
	 * write는 SSD 내부 쓰기 버퍼링, write amplification, flush 동기화로
	 * read보다 지연이 크므로 목표를 느슨하게 10ms로 설정. */
	[KYBER_WRITE] = 10ULL * NSEC_PER_MSEC,
	/* [한국어] KYBER_DISCARD 목표 지연 = 5s.
	 * TRIM/DSM은 SSD GC와 연동되어 수초 이상 걸릴 수 있으므로,
	 * 5s의 매우 관대한 목표를 두어 GC 지연이 throttle을 유발하지 않도록 한다. */
	[KYBER_DISCARD] = 5ULL * NSEC_PER_SEC,
};

/*
 * Batch size (number of requests we'll dispatch in a row) for each scheduling
 * domain.
 */
/* [한국어] 도메인 전환 전에 한 도메인에서 연속으로 dispatch할 최대 request 수.
 * kyber_dispatch_request()에서 khd->batching이 이 값에 도달하면 cur_domain을
 * 다음 도메인으로 전환한다. batch가 클수록 NVMe SQ 내 동종 명령이 몰려 doorbell
 * coalescing 효율이 높아지지만, 다른 도메인의 starvation 위험이 증가한다. */
static const unsigned int kyber_batch_size[] = {
	/* [한국어] KYBER_READ 배치 크기 = 16.
	 * read를 16개씩 연속 NVMe SQ에 채워 NVMe 컨트롤러의 read coalescing과
	 * doorbell interrupt coalescing 효율을 극대화한다. */
	[KYBER_READ] = 16,
	/* [한국어] KYBER_WRITE 배치 크기 = 8.
	 * write를 8개씩 배치: read보다 작게 하여 flush/fsync 지연과의 균형을 맞추고
	 * write가 SQ를 과도하게 독점하지 않도록 한다. */
	[KYBER_WRITE] = 8,
	/* [한국어] KYBER_DISCARD 배치 크기 = 1.
	 * TRIM/DSM은 SSD 내부 GC를 트리거하므로 한 번에 1개씩만 dispatch하여
	 * GC 부하를 분산하고 다른 도메인의 지연 스파이크를 방지한다. */
	[KYBER_DISCARD] = 1,
	/* [한국어] KYBER_OTHER 배치 크기 = 1.
	 * FLUSH 등 기타 명령은 ordering이 중요하므로 1개씩 처리하여
	 * 이전 write가 완전히 완료된 후 FLUSH가 발행되도록 보장한다. */
	[KYBER_OTHER] = 1,
};

/*
 * Requests latencies are recorded in a histogram with buckets defined relative
 * to the target latency:
 *
 * <= 1/4 * target latency
 * <= 1/2 * target latency
 * <= 3/4 * target latency
 * <= target latency
 * <= 1 1/4 * target latency
 * <= 1 1/2 * target latency
 * <= 1 3/4 * target latency
 * > 1 3/4 * target latency
 */
/* [한국어] latency histogram 관련 상수 — NVMe 완료 지연을 상대적 bucket으로 분류.
 * 총 8개의 bucket이 있으며, 처음 4개(bucket 0~3)는 "good"(목표 이내),
 * 나머지 4개(bucket 4~7)는 "bad"(목표 초과)이다.
 * bucket 인덱스 계산: div64_u64(latency - 1, target >> KYBER_LATENCY_SHIFT).
 * 예: target=2ms이면 bucket 경계는 500µs, 1ms, 1.5ms, 2ms, 2.5ms, 3ms, 3.5ms, >3.5ms. */
enum {
	/*
	 * The width of the latency histogram buckets is
	 * 1 / (1 << KYBER_LATENCY_SHIFT) * target latency.
	 */
	/* [한국어] KYBER_LATENCY_SHIFT = 2 → 각 bucket 너비 = target / 4.
	 * 이 비트 시프트 값으로 add_latency_sample()에서 bucket 인덱스를 계산.
	 * 또한 kyber_timer_fn에서 depth 조정 시 p99 비율을 shift로 적용한다:
	 * new_depth = old_depth * (p99 + 1) >> KYBER_LATENCY_SHIFT. */
	KYBER_LATENCY_SHIFT = 2,
	/*
	 * The first (1 << KYBER_LATENCY_SHIFT) buckets are <= target latency,
	 * thus, "good".
	 */
	/* [한국어] KYBER_GOOD_BUCKETS = 4 — bucket 0~3이 "good" 영역.
	 * kyber_timer_fn에서 p90 >= KYBER_GOOD_BUCKETS이면 "bad"로 판정하여
	 * congestion 여부를 결정한다. (bucket 0~3 = 목표 이내, 4~7 = 목표 초과) */
	KYBER_GOOD_BUCKETS = 1 << KYBER_LATENCY_SHIFT,
	/* There are also (1 << KYBER_LATENCY_SHIFT) "bad" buckets. */
	/* [한국어] KYBER_LATENCY_BUCKETS = 8 — 전체 bucket 수.
	 * kyber_cpu_latency.buckets[domain][type][8] 배열의 마지막 차원 크기.
	 * 마지막 bucket(인덱스 7)은 "> 1.75 * target"인 모든 요청을 수용한다. */
	KYBER_LATENCY_BUCKETS = 2 << KYBER_LATENCY_SHIFT,
};

/*
 * We measure both the total latency and the I/O latency (i.e., latency after
 * submitting to the device).
 */
/* [한국어] latency 측정 유형 열거형 — 두 시점에서 지연을 구분 측정한다.
 * 두 값의 차이로 소프트웨어 큐 대기 시간(= TOTAL - IO)을 간접 계산할 수 있다. */
enum {
	/* [한국어] KYBER_TOTAL_LATENCY — bio 생성(rq->start_time_ns)부터
	 * NVMe CQ 완료(now)까지의 전체 지연. 소프트웨어 큐 대기 + 하드웨어 처리 시간 포함.
	 * kyber_completed_request()에서 (now - rq->start_time_ns)로 계산. */
	KYBER_TOTAL_LATENCY,
	/* [한국어] KYBER_IO_LATENCY — NVMe SQ 제출(rq->io_start_time_ns)부터
	 * NVMe CQ 완료(now)까지의 디바이스 처리 지연만 측정.
	 * kyber_completed_request()에서 (now - rq->io_start_time_ns)로 계산.
	 * kyber_timer_fn에서 p90을 이용해 SSD 내부 congestion 여부를 판단한다. */
	KYBER_IO_LATENCY,
};

/* [한국어] latency 유형 인덱스 → 문자열 매핑. trace_kyber_latency 이벤트에서 사용. */
static const char *kyber_latency_type_names[] = {
	[KYBER_TOTAL_LATENCY] = "total",	/* [한국어] 전체 지연 유형 식별 문자열 */
	[KYBER_IO_LATENCY] = "I/O",		/* [한국어] 디바이스 처리 지연 유형 식별 문자열 */
};

/*
 * Per-cpu latency histograms: total latency and I/O latency for each scheduling
 * domain except for KYBER_OTHER.
 *
 * NVMe 연관 설명:
 *   - 각 CPU별로 histogram을 별도 두어 cache contention 없이 완료 지연을
 *     기록한다. NVMe CQ ISR이 완료 시 kyber_completed_request()에서
 *     이 per-cpu bucket을 업데이트한다.
 *   - [KYBER_OTHER][2][...]가 아닌 [KYBER_OTHER]까지 도메인 인덱스가 범위에
 *     포함되며, OTHER 도메인은 latency target이 없어 별도 집계 대상에서
 *     제외된다.
 */
struct kyber_cpu_latency {
	atomic_t buckets[KYBER_OTHER][2][KYBER_LATENCY_BUCKETS];
	/* [한국어] 3차원 per-cpu 원자 카운터 배열.
	 * - 1차 인덱스 [KYBER_OTHER = 3]: 스케줄링 도메인(READ=0, WRITE=1, DISCARD=2).
	 *   KYBER_OTHER 도메인은 latency target이 없으므로 이 배열에 기록하지 않는다.
	 * - 2차 인덱스 [2]: KYBER_TOTAL_LATENCY(0)와 KYBER_IO_LATENCY(1) 구분.
	 * - 3차 인덱스 [KYBER_LATENCY_BUCKETS = 8]: target 대비 지연 구간 bucket.
	 *   bucket 0: ≤ 1/4 target, ..., bucket 3: ≤ target, bucket 4~7: > target.
	 * 설정자: kyber_completed_request() → add_latency_sample() → atomic_inc.
	 *   NVMe CQ ISR 컨텍스트(softirq)에서 현재 CPU에만 기록한다.
	 * 읽는 자: kyber_timer_fn() → flush_latency_buckets() → atomic_xchg로 0으로 리셋하며 읽음.
	 * 값 범위: 각 bucket은 해당 구간에 속하는 NVMe 완료 요청 수의 누적 카운트.
	 * 동기화: per-cpu 변수이므로 동일 CPU 안에서는 락 불필요. ISR(softirq)와
	 *   타이머 컨텍스트(softirq) 간에 atomic_xchg로 race-free하게 교환. */
};

/*
 * There is a same mapping between ctx & hctx and kcq & khd,
 * we use request->mq_ctx->index_hw to index the kcq in khd.
 *
 * NVMe 연관 설명:
 *   - blk-mq에서 request->mq_ctx(소프트웨어 컨텍스트)와 blk_mq_hw_ctx(하드웨어
 *     컨텍스트) 간 매핑은 NVMe 드라이버의 nvme_queue 선택과 밀접하다.
 *   - ctx->index_hw[hctx->type]을 인덱스로 사용하므로, 동일 CPU/동일 hctx
 *     그룹 내 요청은 동일 kcq.rq_list에 삽입된다. (추정) 이는 NVMe SQ당
 *     도착 순서를 유지하면서 도메인별 재정렬을 가능하게 한다.
 */
struct kyber_ctx_queue {
	/*
	 * Used to ensure operations on rq_list and kcq_map to be an atmoic one.
	 * Also protect the rqs on rq_list when merge.
	 */
	spinlock_t lock;
	/* [한국어] per-ctx 스핀락.
	 * 설정자/읽는 자: kyber_bio_merge(), kyber_insert_requests(), flush_busy_kcq()가
	 *   rq_list 접근 시 취득. blk-mq 인터럽트/softirq 컨텍스트에서도 사용 가능.
	 * 보호 대상: rq_list[KYBER_NUM_DOMAINS] 삽입/삭제/splice 및 kcq_map 비트 조작.
	 *   merge 시 rq_list의 request에 대한 동시 접근 방지.
	 * 값 범위: spinlock 상태(unlocked/locked).
	 * 동기화: ____cacheline_aligned_in_smp로 인접 kcq와 캐시 라인을 분리하여
	 *   false sharing을 방지한다. */

	struct list_head rq_list[KYBER_NUM_DOMAINS];
	/* [한국어] 도메인별 request 대기 리스트 — NVMe SQ로 가기 전의 per-ctx 스테이징 큐.
	 * - rq_list[KYBER_READ]:    REQ_OP_READ request 대기열.
	 * - rq_list[KYBER_WRITE]:   REQ_OP_WRITE request 대기열.
	 * - rq_list[KYBER_DISCARD]: REQ_OP_DISCARD(TRIM) request 대기열.
	 * - rq_list[KYBER_OTHER]:   FLUSH 등 기타 request 대기열.
	 * 설정자: kyber_insert_requests()가 list_move_tail/list_move로 request 삽입.
	 *   kyber_bio_merge()가 blk_bio_list_merge()로 기존 request에 bio를 merge.
	 * 읽는 자: flush_busy_kcq()가 list_splice_tail_init으로 khd->rqs[]로 이동.
	 * 값 범위: 0개 이상의 request. 비어있으면 kcq_map 해당 비트는 클리어.
	 * 동기화: lock(spinlock)으로 보호. */
} ____cacheline_aligned_in_smp;
/* [한국어] ____cacheline_aligned_in_smp: SMP 환경에서 각 kcq가 독립된 캐시 라인을
 * 점유하도록 정렬. 서로 다른 CPU에서 다른 kcq에 접근할 때 false sharing 방지. */

/*
 * Per-request_queue Kyber 스케줄러 전역 데이터.
 *
 * NVMe 연관 필드 설명:
 *   - q: 연결된 blk-mq request_queue. NVMe 드라이버는 이 queue를 통해
 *        bio/request를 받는다.
 *   - domain_tokens[KYBER_NUM_DOMAINS]: 도메인별 NVMe "in-flight 명령 수"
 *     한도를 나타내는 token pool. token을 획득해야 request가 dispatch되어
 *     nvme_submit_cmd(doorbell)로 이어질 수 있다. 이 token이 NVMe SQ의
 *     실제 CID/entry 가용 개수보다 더 제한적으로 동작하여 Kyber의 스로틀링
 *     포인트가 된다.
 *   - cpu_latency: per-cpu 완료 지연 히스토그램. NVMe ISR 경로에서 업데이트.
 *   - timer: 100ms 간격으로 히스토그램을 집계하고 domain_tokens 깊이를
 *     조정하는 타이머. NVMe queue depth를 latency feedback에 따라 재조정.
 *   - latency_buckets[][][]: 타이머가 cpu_latency를 flush하여 합산하는
 *     중앙 집계 버킷.
 *   - latency_timeout[]: 샘플 수/시간이 충분히 쌓였는지 판단.
 *   - domain_p99[]: p99 percentile 값을 congestion 발생 시까지 보존.
 *   - latency_targets[]: 도메인별 목표 지연 시간. NVMe read는 2ms, write는
 *     10ms 등으로 설정되어 있다.
 */
struct kyber_queue_data {
	struct request_queue *q;
	/* [한국어] 이 kqd가 속한 blk-mq request_queue 포인터.
	 * 설정자: kyber_queue_data_alloc()에서 초기화.
	 * 읽는 자: kyber_exit_sched(), kyber_depth_updated(), kyber_init_sched().
	 * 값 범위: 유효한 request_queue 포인터 (NULL 불가).
	 * 동기화: 스케줄러 수명 동안 불변. */

	dev_t dev;
	/* [한국어] 이 큐가 속한 디스크의 디바이스 번호(major:minor).
	 * 설정자: kyber_queue_data_alloc()에서 disk_devt(q->disk)로 초기화.
	 * 읽는 자: trace_kyber_latency(), trace_kyber_throttled(), trace_kyber_adjust()에서
	 *   어느 NVMe 장치의 이벤트인지 식별하는 데 사용.
	 * 값 범위: 커널 dev_t 타입. 예: nvme0n1이면 259:0.
	 * 동기화: 스케줄러 수명 동안 불변. */

	/*
	 * Each scheduling domain has a limited number of in-flight requests
	 * device-wide, limited by these tokens.
	 */
	struct sbitmap_queue domain_tokens[KYBER_NUM_DOMAINS];
	/* [한국어] 도메인별 in-flight request 토큰 풀 — Kyber throttling의 핵심 자료구조.
	 * - domain_tokens[KYBER_READ]:    최대 256개 read in-flight 토큰.
	 * - domain_tokens[KYBER_WRITE]:   최대 128개 write in-flight 토큰.
	 * - domain_tokens[KYBER_DISCARD]: 최대 64개 discard in-flight 토큰.
	 * - domain_tokens[KYBER_OTHER]:   최대 16개 기타 in-flight 토큰.
	 * 설정자: kyber_queue_data_alloc()에서 sbitmap_queue_init_node()로 초기화.
	 *   kyber_resize_domain()이 sbitmap_queue_resize()로 depth를 동적 조정.
	 * 읽는 자: kyber_get_domain_token()이 __sbitmap_queue_get()으로 토큰 획득.
	 *   rq_clear_domain_token()이 sbitmap_queue_clear()로 토큰 반환.
	 * 값 범위: depth는 1 ~ kyber_depth[domain] 사이에서 동적 변화.
	 * 동기화: sbitmap_queue 내부 atomic 연산으로 lock-free하게 관리. */

	struct kyber_cpu_latency __percpu *cpu_latency;
	/* [한국어] per-cpu latency histogram 포인터.
	 * 설정자: kyber_queue_data_alloc()에서 alloc_percpu_gfp()로 할당.
	 * 읽는 자: kyber_completed_request()가 get_cpu_ptr()로 현재 CPU 버킷에 기록.
	 *   kyber_timer_fn()이 for_each_online_cpu()로 모든 CPU 버킷을 flush.
	 * 값 범위: 각 CPU의 atomic_t 버킷 배열. 읽기/쓰기 모두 원자적으로 수행.
	 * 동기화: per-cpu이므로 동일 CPU 내에서는 lock 불필요.
	 *   타이머와 ISR 간에는 atomic_xchg를 통해 race-free하게 교환. */

	/* Timer for stats aggregation and adjusting domain tokens. */
	struct timer_list timer;
	/* [한국어] latency 집계 및 domain_tokens 깊이 조정 타이머.
	 * 설정자: kyber_queue_data_alloc()에서 timer_setup(&kqd->timer, kyber_timer_fn, 0).
	 *   kyber_completed_request()가 timer_reduce()로 100ms 뒤 만료되도록 설정.
	 * 읽는 자: 타이머 만료 시 kyber_timer_fn()이 실행.
	 * 값 범위: 만료 예정 jiffies 값. HZ/10(100ms) 후로 설정.
	 * 동기화: timer_reduce()는 이미 예정된 시간보다 앞당길 때만 timer를 갱신하므로
	 *   중복 호출에 안전하다. timer_shutdown_sync()로 스케줄러 종료 시 안전 정지. */

	unsigned int latency_buckets[KYBER_OTHER][2][KYBER_LATENCY_BUCKETS];
	/* [한국어] 중앙 집계 latency histogram 버킷 — flush_latency_buckets()가 per-cpu 값을 합산.
	 * - 1차 [KYBER_OTHER=3]: 도메인 인덱스 (READ=0, WRITE=1, DISCARD=2).
	 * - 2차 [2]: KYBER_TOTAL_LATENCY(0), KYBER_IO_LATENCY(1).
	 * - 3차 [KYBER_LATENCY_BUCKETS=8]: 목표 대비 지연 구간 bucket.
	 * 설정자: flush_latency_buckets()가 cpu_latency에서 atomic_xchg로 읽어 더함.
	 * 읽는 자: calculate_percentile()이 p90/p99 계산에 사용. 계산 후 memset으로 초기화.
	 * 값 범위: 0 이상의 request 완료 카운트. 계산 후 0으로 리셋됨.
	 * 동기화: kyber_timer_fn 내에서만 접근. 타이머는 단일 인스턴스로 실행되므로 락 불필요. */

	unsigned long latency_timeout[KYBER_OTHER];
	/* [한국어] 도메인별 percentile 계산 유효성 판단용 타임스탬프(jiffies).
	 * calculate_percentile()에서 첫 샘플 도착 후 1초(HZ)가 경과하거나 500개 이상의
	 * 샘플이 모일 때까지 percentile 계산을 미룬다. 이 필드는 그 만료 시각을 저장.
	 * 설정자: calculate_percentile()에서 첫 샘플 도착 시 max(jiffies + HZ, 1UL) 설정.
	 *   계산 후 0으로 리셋.
	 * 읽는 자: calculate_percentile()에서 time_is_after_jiffies()로 비교.
	 * 값 범위: 0(미초기화/리셋 상태) 또는 미래 jiffies 값.
	 * 동기화: kyber_timer_fn 내에서만 접근. */

	int domain_p99[KYBER_OTHER];
	/* [한국어] 도메인별 p99 latency bucket 인덱스 — congestion 간 지속 보존용.
	 * calculate_percentile()이 샘플 부족으로 p99를 계산 못할 때,
	 * kyber_timer_fn이 이전에 저장된 값을 fallback으로 사용한다.
	 * 설정자: kyber_queue_data_alloc()에서 -1(미계산)으로 초기화.
	 *   kyber_timer_fn()에서 congestion이 아닐 때 calculate_percentile() 결과를 저장.
	 *   congestion 시작 시 -1로 리셋하여 다음 샘플이 쌓일 때까지 재계산을 미룸.
	 * 읽는 자: kyber_timer_fn()에서 p99 < 0일 때 이 값을 사용.
	 * 값 범위: -1(미계산) 또는 0 ~ KYBER_LATENCY_BUCKETS-1.
	 * 동기화: kyber_timer_fn 내에서만 접근. */

	/* Target latencies in nanoseconds. */
	u64 latency_targets[KYBER_OTHER];
	/* [한국어] 도메인별 목표 지연 시간(나노초) — add_latency_sample()의 기준값.
	 * 설정자: kyber_queue_data_alloc()에서 kyber_latency_targets[]로 초기화.
	 *   sysfs kyber_read_lat_store()/kyber_write_lat_store()를 통해 런타임 변경 가능.
	 * 읽는 자: kyber_completed_request()에서 add_latency_sample() 호출 시 전달.
	 * 값 범위: 양의 정수 나노초 값. 기본값: READ=2ms, WRITE=10ms, DISCARD=5s.
	 * 동기화: sysfs store는 프로세스 컨텍스트, 읽기는 softirq. 64비트 단일 대입은
	 *   64비트 아키텍처에서 원자적이지만, 갱신 중 일시적으로 일관성이 깨질 수 있다
	 *   (설계상 허용: target 변경은 즉각 정확성보다 점진적 반영이 목적). */
};

/*
 * Per-blk_mq_hw_ctx Kyber 데이터.
 *
 * NVMe 연관 필드 설명:
 *   - lock: hctx 단위 스핀락. 동일 nvme_queue/hctx에서 dispatch 순서를
 *           직렬화한다.
 *   - rqs[KYBER_NUM_DOMAINS]: dispatch 직전에 kcq로부터 모아온 request list.
 *   - cur_domain: 현재 배치/디스패치 중인 도메인. NVMe SQ에 날아갈 다음
 *                 요청이 read/write/discard/other 중 어느 그룹에 속하는지
 *                 결정한다.
 *   - batching: 현재 도메인에서 연속 dispatch한 개수. batch_size에 도달하면
 *               도메인을 전환하여 NVMe SQ에 다른 종류의 명령을 섞어준다.
 *   - kcqs: per-ctx 큐 배열. bio가 처음 도착하면 이 리스트에 들어간다.
 *   - kcq_map[KYBER_NUM_DOMAINS]: 어느 kcq에 요청이 있는지를 나타내는
 *     비트맵. sbitmap을 사용하여 빠르게 비어있는 큐를 걸러낸다.
 *   - domain_wait[] / domain_ws[] / wait_index[]: domain_tokens가 바닥났을
 *     때 기다리는 wait queue. token이 해제되면(=NVMe CQ 완료) 해당 hctx를
 *     깨워서 다시 dispatch하게 한다.
 */
struct kyber_hctx_data {
	spinlock_t lock;
	/* [한국어] 이 hctx(하나의 NVMe SQ에 대응) 전용 스핀락.
	 * 설정자/획득자: kyber_dispatch_request()가 dispatch 시작 시 획득,
	 *   kyber_init_hctx()에서 spin_lock_init()으로 초기화.
	 *   debugfs의 kyber_*_rqs_start/stop()도 rqs[] 순회 시 획득.
	 * 보호 대상: cur_domain, batching, rqs[], domain_wait[]/domain_ws[]에
	 *   대한 동시 접근 — 동일 hctx에서 dispatch가 여러 스레드에서 겹치는
	 *   것을 막아 NVMe SQ에 대한 dispatch 순서를 직렬화한다.
	 * 값 범위: spinlock 상태(unlocked/locked).
	 * 동기화: hctx 단위로 독립적이므로 서로 다른 NVMe SQ 간에는 락 경합이 없다. */

	struct list_head rqs[KYBER_NUM_DOMAINS];
	/* [한국어] kcq에서 flush되어 NVMe SQ dispatch를 기다리는 도메인별 request 리스트.
	 * 설정자: kyber_dispatch_cur_domain()이 kyber_flush_busy_kcqs()를 통해
	 *   kcq->rq_list[]의 내용을 list_splice_tail_init으로 이 리스트로 옮김.
	 * 읽는 자: kyber_dispatch_cur_domain()이 list_first_entry_or_null로 맨 앞
	 *   request를 꺼내 token 획득 후 NVMe SQ dispatch 대상으로 반환.
	 *   kyber_has_work()가 list_empty_careful로 잔여 여부 확인.
	 * 값 범위: 0개 이상의 request. 이미 flush됐지만 아직 token을 못 받은
	 *   request가 여기 머무를 수 있다.
	 * 동기화: khd->lock으로 보호. */

	unsigned int cur_domain;
	/* [한국어] 현재 batching 중인 스케줄링 도메인 인덱스 (KYBER_READ 등).
	 * 설정자: kyber_init_hctx()에서 0(KYBER_READ)으로 초기화.
	 *   kyber_dispatch_request()가 batch_size 도달 또는 요청/토큰 소진 시
	 *   다음 도메인으로 순환(round-robin) 전환.
	 * 읽는 자: kyber_dispatch_cur_domain(), kyber_get_domain_token(),
	 *   debugfs의 kyber_cur_domain_show().
	 * 값 범위: 0 ~ KYBER_NUM_DOMAINS-1 (READ/WRITE/DISCARD/OTHER).
	 * 동기화: khd->lock으로 보호되어 dispatch 스레드 간 경쟁 없음. */

	unsigned int batching;
	/* [한국어] cur_domain에서 연속으로 dispatch에 성공한 request 개수.
	 * 설정자: kyber_init_hctx()에서 0으로 초기화. kyber_dispatch_cur_domain()이
	 *   dispatch 성공 시마다 1씩 증가. kyber_dispatch_request()가 도메인
	 *   전환 시 0으로 리셋.
	 * 읽는 자: kyber_dispatch_request()가 kyber_batch_size[cur_domain]과
	 *   비교하여 batch 지속 여부 판단. debugfs kyber_batching_show().
	 * 값 범위: 0 ~ kyber_batch_size[cur_domain] (READ=16, WRITE=8 등).
	 * 동기화: khd->lock으로 보호. */

	struct kyber_ctx_queue *kcqs;
	/* [한국어] 이 hctx에 속한 모든 software ctx(hctx->nr_ctx개) 각각에 대응하는
	 * per-ctx 큐 배열 — bio가 최초 도착하는 지점(NVMe SQ 이전 스테이징 큐).
	 * 설정자: kyber_init_hctx()에서 kmalloc_array_node로 할당 후
	 *   kyber_ctx_queue_init()으로 각 원소 초기화.
	 * 읽는 자: kyber_bio_merge(), kyber_insert_requests()가
	 *   ctx->index_hw[hctx->type]로 인덱싱하여 접근.
	 *   flush_busy_kcq()가 bitnr(=ctx 인덱스)로 접근.
	 * 값 범위: hctx->nr_ctx개의 kyber_ctx_queue 배열. NULL 불가(할당 실패 시
	 *   kyber_init_hctx가 -ENOMEM 반환).
	 * 동기화: 각 kcqs[i].lock으로 개별 보호(struct kyber_ctx_queue 참고). */

	struct sbitmap kcq_map[KYBER_NUM_DOMAINS];
	/* [한국어] 도메인별로 "어느 kcq(ctx 인덱스)에 request가 있는지"를 나타내는 비트맵.
	 * 설정자: kyber_insert_requests()가 sbitmap_set_bit()으로 세트.
	 *   flush_busy_kcq()가 sbitmap_clear_bit()으로 클리어.
	 * 읽는 자: kyber_flush_busy_kcqs()가 sbitmap_for_each_set()으로 순회하여
	 *   non-empty kcq만 빠르게 찾아 flush.
	 *   kyber_dispatch_cur_domain()/kyber_has_work()가 sbitmap_any_bit_set()으로 존재 확인.
	 * 값 범위: 비트 수 = hctx->nr_ctx, 각 비트는 해당 ctx 인덱스의 kcq에
	 *   해당 도메인 request가 1개 이상 있음을 의미.
	 * 동기화: sbitmap 내부 원자 연산으로 lock-free. 단, 개별 kcq의
	 *   rq_list 접근 자체는 kcq->lock으로 별도 보호. */

	struct sbq_wait domain_wait[KYBER_NUM_DOMAINS];
	/* [한국어] domain_tokens가 고갈됐을 때 이 hctx가 등록되는 도메인별 wait 항목.
	 * 설정자: kyber_init_hctx()에서 init_waitqueue_func_entry(kyber_domain_wake)로
	 *   콜백 등록. kyber_get_domain_token()이 sbitmap_add_wait_queue()로
	 *   토큰 부족 시 등록.
	 * 읽는 자: kyber_domain_wake()가 NVMe CQ 완료로 token 반환 시
	 *   sbitmap_queue_clear() 내부에서 wake 콜백으로 호출.
	 * 값 범위: wait.private에는 이 hctx 포인터가 고정 저장됨.
	 * 동기화: 대응하는 domain_tokens의 sbq_wait_state.wait.lock으로
	 *   등록/제거가 보호됨 (khd->lock과는 별개). */

	struct sbq_wait_state *domain_ws[KYBER_NUM_DOMAINS];
	/* [한국어] domain_wait[]가 등록된 sbq_wait_state(sbitmap_queue 내부 wait
	 * 라운드로빈 슬롯) 포인터 — wait 해제 시 어느 슬롯에서 제거할지 기억.
	 * 설정자: kyber_get_domain_token()이 sbq_wait_ptr()로 얻은 슬롯을 저장.
	 * 읽는 자: kyber_get_domain_token()이 재시도 후 token을 얻으면 이 포인터로
	 *   ws->wait.lock을 잠그고 sbitmap_del_wait_queue()로 제거.
	 * 값 범위: 유효한 sbq_wait_state 포인터 (등록 전에는 사용되지 않음).
	 * 동기화: khd->lock 하에서만 갱신되므로 동시 갱신 없음. */

	atomic_t wait_index[KYBER_NUM_DOMAINS];
	/* [한국어] 도메인별로 다음에 사용할 sbq_wait_state 라운드로빈 인덱스.
	 * 설정자: kyber_init_hctx()에서 atomic_set(0)으로 초기화.
	 *   sbq_wait_ptr()(sbitmap 내부 구현)가 호출될 때마다 내부적으로 증가.
	 * 읽는 자: kyber_get_domain_token()이 sbq_wait_ptr(domain_tokens,
	 *   &khd->wait_index[sched_domain]) 호출 시 전달하여 슬롯 선택에 사용.
	 * 값 범위: 0 이상, sbq_wait_state 배열 크기로 모듈러(mod) 순환.
	 * 동기화: atomic_t로 lock-free 증가. 여러 hctx가 공유하는 domain_tokens의
	 *   wait 슬롯 선택을 라운드로빈으로 분산시키는 목적. */
};

/* [한국어] kyber_domain_wake()의 전방 선언 — kyber_init_hctx()가 이 함수 포인터를
 * domain_wait[].wait에 콜백으로 등록해야 하므로, 실제 정의(파일 뒷부분)보다
 * 앞서 시그니처가 필요하다. 정의부의 상세 함수 주석은 실제 정의 위치를 참고. */
static int kyber_domain_wake(wait_queue_entry_t *wait, unsigned mode, int flags,
			     void *key);

/*
 * [한국어]
 * kyber_sched_domain() - request(bio)의 opcode로 Kyber 스케줄링 도메인 결정
 *
 * @opf: 분류 대상 request/bio의 op-flags (rq->cmd_flags 또는 bio->bi_opf).
 *       REQ_OP_MASK로 하위 opcode 비트만 추출해 판단에 사용한다.
 * @return: KYBER_READ/KYBER_WRITE/KYBER_DISCARD/KYBER_OTHER 중 하나의
 *          도메인 인덱스 (항상 유효한 값 — default case가 KYBER_OTHER로 수렴).
 *
 * Kyber는 read/write/discard/other 네 도메인마다 독립적인 domain_tokens와
 * latency_target을 두므로, 모든 진입 경로(merge/insert/dispatch/completed)에서
 * 동일한 분류 기준이 필요하다. 이 함수가 그 단일 진실 공급원(single source of
 * truth) 역할을 하여, 어디서 호출되어도 같은 request가 항상 같은 도메인으로
 * 분류되도록 보장한다. opcode에 따라 이후 NVMe 명령(NVME_CMD_READ,
 * NVME_CMD_WRITE, NVME_CMD_DSM 등)으로 변환될 그룹을 사전 분류하는 셈이다.
 * 프로세스 컨텍스트(bio 제출)와 softirq 컨텍스트(완료 처리) 양쪽에서 모두
 * 호출되지만, 순수 함수(부작용 없음)이므로 재진입/동시 호출에 안전하다.
 *
 * 호출 체인:
 *   kyber_bio_merge() / kyber_insert_requests() / kyber_dispatch_cur_domain() /
 *   kyber_completed_request() → [kyber_sched_domain] → (반환값을 도메인
 *   인덱스로 사용, 하위 호출 없음)
 */
static unsigned int kyber_sched_domain(blk_opf_t opf)
{
	/* bio->bi_opf 하위 REQ_OP_MASK만 남겨 NVMe 명령 유형과 1:1 매핑할 opcode 추출 */
	switch (opf & REQ_OP_MASK) {
	case REQ_OP_READ:
		return KYBER_READ;	/* NVME_CMD_READ 그룹: SQ CID/도메인 토큰을 read pool에서 할당 */
	case REQ_OP_WRITE:
		return KYBER_WRITE;	/* NVME_CMD_WRITE 그룹: write pool에서 토큰 및 SQ 진행 자리 할당 */
	case REQ_OP_DISCARD:
		return KYBER_DISCARD;	/* NVME_CMD_DSM/TRIM 그룹: discard 전용 queue depth/token 사용 */
	default:
		return KYBER_OTHER;	/* NVME_CMD_FLUSH, vendor, admin passthrough 등 기타 명령 */
	}
}

/*
 * [한국어]
 * flush_latency_buckets() - 한 CPU의 per-cpu histogram을 중앙 집계 버킷에 합산
 *
 * @kqd: 이 request_queue의 Kyber 전역 데이터. latency_buckets[]가 갱신 대상.
 * @cpu_latency: 합산 대상이 되는 특정 CPU의 per-cpu histogram 포인터
 *               (per_cpu_ptr()로 얻은 값).
 * @sched_domain: 합산할 스케줄링 도메인 인덱스 (KYBER_READ/WRITE/DISCARD).
 * @type: 합산할 latency 유형 (KYBER_TOTAL_LATENCY 또는 KYBER_IO_LATENCY).
 * @return: 없음(void). kqd->latency_buckets[sched_domain][type][]를 직접 갱신.
 *
 * NVMe CQ(Completion Queue) ISR은 완료 지연을 "현재 실행 중인 CPU"의
 * per-cpu 버킷에만 원자적으로 기록하므로(cache contention 회피), 전체
 * percentile을 계산하려면 모든 CPU의 값을 한 곳에 모아야 한다. 이 함수는
 * kyber_timer_fn()이 100ms 주기로 온라인 CPU를 순회하며 호출하는 헬퍼로,
 * 각 bucket에 대해 atomic_xchg로 per-cpu 카운터를 0으로 리셋하면서 동시에
 * 그 값을 읽어와 중앙 버킷에 누적한다 — 이 xchg 원자성 덕분에 ISR이
 * 같은 순간 카운터를 증가시키더라도 값 유실 없이 안전하게 교환된다.
 * 타이머(softirq) 컨텍스트에서 실행되며, 인터럽트를 끌 필요는 없다.
 *
 * 호출 체인:
 *   kyber_timer_fn() → [flush_latency_buckets] → atomic_xchg (하위 호출 없음)
 */
static void flush_latency_buckets(struct kyber_queue_data *kqd,
				  struct kyber_cpu_latency *cpu_latency,
				  unsigned int sched_domain, unsigned int type)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];	/* [한국어] 이번 호출로 누적될 중앙 집계 버킷 배열(8개) 포인터 */
	atomic_t *cpu_buckets = cpu_latency->buckets[sched_domain][type];	/* [한국어] 리셋하며 읽어올 해당 CPU의 원자 카운터 배열(8개) 포인터 */
	unsigned int bucket;	/* [한국어] 0~7 순회용 bucket 인덱스 */

	/* NVMe CQ ISR이 각 CPU에 기록한 완료 지연을 타이머 주기로 중앙 버킷에 합산 */
	for (bucket = 0; bucket < KYBER_LATENCY_BUCKETS; bucket++)
		/* atomic_xchg: per-cpu bucket을 0으로 리셋하면서 값을 원자적으로 가져옴 (NVMe ISR와 race 방지) */
		buckets[bucket] += atomic_xchg(&cpu_buckets[bucket], 0);
}

/*
 * [한국어]
 * calculate_percentile() - 집계된 histogram에서 percentile에 해당하는 bucket 계산
 *
 * @kqd: Kyber 전역 데이터. latency_buckets[]/latency_timeout[]을 읽고 갱신.
 * @sched_domain: 대상 스케줄링 도메인 인덱스 (KYBER_READ/WRITE/DISCARD).
 * @type: 대상 latency 유형 (KYBER_TOTAL_LATENCY 또는 KYBER_IO_LATENCY).
 * @percentile: 계산할 백분위수 (예: 90, 99).
 * @return: 0 ~ KYBER_LATENCY_BUCKETS-1 중 percentile에 해당하는 bucket 인덱스.
 *          샘플이 아직 부족하면(0개이거나, 500개 미만이면서 1초가 지나지
 *          않았으면) -1을 반환해 "아직 판단 불가"를 알린다.
 *
 * NVMe SSD의 완료 지연 분포에서 이상치(outlier) 하나만으로 queue depth를
 * 흔들지 않기 위해, 평균이 아니라 p90/p99 같은 percentile을 사용한다.
 * 샘플이 너무 적은 상태에서 계산하면 노이즈에 취약하므로, 첫 샘플 도착
 * 후 1초가 지나거나 500개가 쌓일 때까지 계산을 보류한다(latency_timeout
 * 필드로 이 유예 기간을 추적). 조건이 충족되면 histogram을 낮은 bucket부터
 * 누적하며 percentile*samples/100 번째 샘플이 속한 bucket을 선형 탐색으로
 * 찾고, 계산이 끝난 buckets 배열은 memset으로 0 초기화해 다음 100ms
 * 윈도우의 샘플만 새로 반영되게 한다. 타이머(softirq) 컨텍스트에서
 * kyber_timer_fn()에 의해서만 호출되므로 재진입/동시 호출 걱정이 없다.
 *
 * 호출 체인:
 *   kyber_timer_fn() → [calculate_percentile] → trace_kyber_latency() (하위 호출)
 */
static int calculate_percentile(struct kyber_queue_data *kqd,
				unsigned int sched_domain, unsigned int type,
				unsigned int percentile)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];	/* [한국어] percentile 계산 대상 중앙 집계 histogram(8개 bucket) */
	unsigned int bucket, samples = 0, percentile_samples;	/* [한국어] bucket: 순회 인덱스, samples: 전체 완료 샘플 수, percentile_samples: percentile 경계에 해당하는 누적 샘플 수 */

	/* NVMe SQ/CQ를 통해 수집된 완료 샘플의 총 개수를 집계 (CQ entry 수 == samples) */
	for (bucket = 0; bucket < KYBER_LATENCY_BUCKETS; bucket++)
		samples += buckets[bucket];

	if (!samples)	/* [한국어] 이번 100ms 윈도우에 완료된 NVMe request가 하나도 없으면 계산 불가 */
		return -1;	/* [한국어] "샘플 없음"을 -1로 알려 kyber_timer_fn이 이전 p99를 fallback으로 쓰게 함 */

	/*
	 * We do the calculation once we have 500 samples or one second passes
	 * since the first sample was recorded, whichever comes first.
	 */
	/* 첫 샘플 시점부터 1초(HZ) 후 또는 500개 NVMe CQ 완료 entry가 모일 때까지 percentile 계산 보류 */
	if (!kqd->latency_timeout[sched_domain])
		kqd->latency_timeout[sched_domain] = max(jiffies + HZ, 1UL);
	if (samples < 500 &&	/* [한국어] 아직 500개 미만이고 */
	    time_is_after_jiffies(kqd->latency_timeout[sched_domain])) {	/* [한국어] 유예 시각(첫 샘플+1초)도 아직 안 지났으면 */
		return -1;	/* [한국어] 표본이 통계적으로 불충분 -> 이번 윈도우는 계산 보류 */
	}
	kqd->latency_timeout[sched_domain] = 0;	/* [한국어] 계산을 실제로 수행하므로 유예 타임스탬프를 리셋(다음 첫 샘플 때 재설정) */

	/* p90/p99에 해당하는 NVMe 완료 지연 bucket을 선형 탐색으로 찾음 */
	percentile_samples = DIV_ROUND_UP(samples * percentile, 100);
	for (bucket = 0; bucket < KYBER_LATENCY_BUCKETS - 1; bucket++) {
		if (buckets[bucket] >= percentile_samples)
			break;
		percentile_samples -= buckets[bucket];
	}
	/* 집계 완료 후 bucket을 초기화하여 다음 NVMe CQ 주기의 latency 분포를 새로 측정 */
	memset(buckets, 0, sizeof(kqd->latency_buckets[sched_domain][type]));

	trace_kyber_latency(kqd->dev, kyber_domain_names[sched_domain],
			    kyber_latency_type_names[type], percentile,
			    bucket + 1, 1 << KYBER_LATENCY_SHIFT, samples);

	return bucket;
}

/*
 * [한국어]
 * kyber_resize_domain() - 도메인의 domain_tokens(in-flight 상한) 깊이 조정
 *
 * @kqd: Kyber 전역 데이터. domain_tokens[sched_domain]의 depth를 갱신.
 * @sched_domain: 조정할 스케줄링 도메인 인덱스 (KYBER_READ/WRITE/DISCARD).
 * @depth: 새로 적용하고자 하는 목표 depth (clamp 전 값 — 호출자가 계산한
 *         p99 비례 값이므로 범위를 벗어날 수 있음).
 * @return: 없음(void). kqd->domain_tokens[sched_domain]의 sbitmap depth를
 *          직접 갱신하거나(변경 있을 시) 그대로 둔다(변경 없을 시).
 *
 * kyber_timer_fn()이 계산한 새 depth 후보는 이론상 범위를 벗어날 수 있으므로
 * (0이 되면 해당 도메인이 영원히 dispatch 불가), 이 함수가 [1,
 * kyber_depth[sched_domain]] 범위로 clamp하여 안전망 역할을 한다. 이 depth는
 * 곧 해당 도메인이 NVMe SQ에 동시에 발행할 수 있는 in-flight 명령 수의
 * 상한이므로, 값이 실제로 바뀔 때만 sbitmap_queue_resize()를 호출해
 * 불필요한 재할당/재배치 비용을 피한다. 타이머(softirq) 컨텍스트에서
 * kyber_timer_fn()에 의해서만 호출되므로 동시 호출 걱정이 없다.
 *
 * 호출 체인:
 *   kyber_timer_fn() → [kyber_resize_domain] → sbitmap_queue_resize(),
 *   trace_kyber_adjust()
 */
static void kyber_resize_domain(struct kyber_queue_data *kqd,
				unsigned int sched_domain, unsigned int depth)
{
	/* NVMe SQ in-flight 한도를 [1, kyber_depth[]] 범위로 clamp: 0이면 doorbell 발행 자체가 멈춤 */
	depth = clamp(depth, 1U, kyber_depth[sched_domain]);
	/* 실제 sbitmap depth가 바뀔 때만 NVMe queue depth 조정을 적용 (불필요한 resize 회피) */
	if (depth != kqd->domain_tokens[sched_domain].sb.depth) {
		/* domain_tokens 깊이 변경 -> NVMe SQ로 동시에 진행 가능한 CID/entry 수 간접 제한 */
		sbitmap_queue_resize(&kqd->domain_tokens[sched_domain], depth);
		trace_kyber_adjust(kqd->dev, kyber_domain_names[sched_domain],
				   depth);	/* [한국어] ftrace/perf로 depth 변경 이벤트 노출: 관찰자가 어느 도메인이 언제 얼마나 조정됐는지 추적 가능 */
	}
}

/*
 * [한국어]
 * kyber_timer_fn() - 100ms 주기 latency 집계 및 domain_tokens depth 조절 타이머
 *
 * @t: 만료된 timer_list. timer_container_of()로 이를 포함하는 kyber_queue_data(kqd)를
 *     역산해 얻는다 (kqd->timer가 이 t와 동일 주소).
 * @return: 없음(void). kqd->domain_tokens[]의 depth를 조건에 따라 조정.
 *
 * Kyber의 피드백 루프 핵심 함수. NVMe CQ ISR이 kyber_completed_request()를
 * 통해 계속 쌓아온 per-cpu latency 샘플을 이 타이머가 100ms마다 모아 percentile을
 * 계산하고, 그 결과로 각 도메인의 NVMe SQ in-flight 상한(domain_tokens depth)을
 * 늘리거나 줄인다. 동작 3단계: (1) 모든 온라인 CPU의 histogram을 중앙
 * latency_buckets[]에 flush, (2) I/O latency(디바이스 처리 시간만)의 p90이
 * 목표를 넘는 도메인이 하나라도 있으면 전역 congestion(bad)으로 판정 —
 * outlier에 덜 민감하도록 p90(p99가 아님)을 사용, (3) 도메인별 total latency의
 * p99를 target과 비교해 depth를 선형 비례로 재조정: bad일 때는 "이미 좋은"
 * 도메인을 죄어 congestion을 완화하고, 반대로 latency가 나쁜 도메인은 항상
 * (bad 여부 무관) 완화한다. p99 계산이 샘플 부족으로 실패하면 domain_p99[]에
 * 저장해둔 직전 값을 재사용해 congestion 판정의 연속성을 유지한다. 타이머
 * (softirq) 컨텍스트에서 실행되며 단일 인스턴스이므로 재진입 없음.
 *
 * 호출 체인:
 *   (타이머 만료, kyber_completed_request()의 timer_reduce()가 예약) →
 *   [kyber_timer_fn] → flush_latency_buckets(), calculate_percentile(),
 *   kyber_resize_domain()
 */
static void kyber_timer_fn(struct timer_list *t)
{
	struct kyber_queue_data *kqd = timer_container_of(kqd, t, timer);	/* [한국어] 만료된 timer_list t로부터 이를 감싸는 kyber_queue_data 포인터 역산 */
	unsigned int sched_domain;	/* [한국어] 아래 세 루프에서 공용으로 재사용하는 도메인 순회 인덱스 */
	int cpu;	/* [한국어] for_each_online_cpu 순회용 CPU 번호 */
	bool bad = false;	/* [한국어] 어느 도메인이든 p90 IO latency가 목표를 초과하면 true — 장치 전역 congestion 신호 */

	/* Sum all of the per-cpu latency histograms. */
	/* NVMe CQ ISR가 각 CPU에 남긴 완료 지연을 온라인 CPU별로 순회하며 중앙 집계 */
	for_each_online_cpu(cpu) {
		struct kyber_cpu_latency *cpu_latency;	/* [한국어] 현재 순회 중인 CPU의 per-cpu histogram 포인터 */

		cpu_latency = per_cpu_ptr(kqd->cpu_latency, cpu);	/* [한국어] percpu 영역에서 해당 cpu 번호의 인스턴스 주소 계산 */
		/* read/write/discard 도메인(OTHER 제외)의 TOTAL/IO latency를 모두 flush */
		for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KYBER_TOTAL_LATENCY);	/* [한국어] bio 제출~CQ 완료 전체 지연 histogram 합산 */
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KYBER_IO_LATENCY);	/* [한국어] NVMe SQ 제출~CQ 완료 디바이스 지연 histogram 합산 */
		}
	}

	/*
	 * Check if any domains have a high I/O latency, which might indicate
	 * congestion in the device. Note that we use the p90; we don't want to
	 * be too sensitive to outliers here.
	 */
	/* NVMe SSD 내부 congestion 판단: p90 IO latency가 target을 초과하면 bad 플래그 설정 */
	for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
		int p90;	/* [한국어] 이 도메인의 IO latency p90 bucket (-1이면 샘플 부족) */

		p90 = calculate_percentile(kqd, sched_domain, KYBER_IO_LATENCY,
					   90);	/* [한국어] 디바이스 처리 지연만으로 SSD 자체 혼잡 여부 판단 (SW 큐 대기 배제) */
		if (p90 >= KYBER_GOOD_BUCKETS)	/* [한국어] bucket 4 이상 = "target 초과" 영역에 p90이 들어왔다는 뜻 */
			bad = true;	/* [한국어] 이 장치는 지금 congestion 상태 -> 아래 루프에서 양호한 도메인들을 죌 근거 */
	}

	/*
	 * Adjust the scheduling domain depths. If we determined that there was
	 * congestion, we throttle all domains with good latencies. Either way,
	 * we ease up on throttling domains with bad latencies.
	 */
	/* NVMe SQ queue depth(domain_tokens)를 p99 latency 기반으로 동적으로 조절 */
	for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
		unsigned int orig_depth, depth;	/* [한국어] orig_depth: 조정 전 현재 depth, depth: 새로 계산된 목표 depth */
		int p99;	/* [한국어] 이 도메인의 total latency p99 bucket (-1이면 샘플 부족) */

		p99 = calculate_percentile(kqd, sched_domain,
					   KYBER_TOTAL_LATENCY, 99);	/* [한국어] SW 큐 대기 포함 전체 지연의 p99 — depth 조정 기준 */
		/*
		 * This is kind of subtle: different domains will not
		 * necessarily have enough samples to calculate the latency
		 * percentiles during the same window, so we have to remember
		 * the p99 for the next time we observe congestion; once we do,
		 * we don't want to throttle again until we get more data, so we
		 * reset it to -1.
		 */
		if (bad) {	/* [한국어] 장치 전역 congestion 상태일 때 */
			/* congestion 시 샘플 부족하면 직전에 저장된 p99를 fallback으로 사용 */
			if (p99 < 0)	/* [한국어] 이번 윈도우에 이 도메인 샘플이 부족했다면 */
				p99 = kqd->domain_p99[sched_domain];	/* [한국어] 지난번 계산해 둔 p99로 대체 -> congestion 판정 지속성 확보 */
			kqd->domain_p99[sched_domain] = -1;	/* [한국어] 이번 값을 소비했으니 리셋 -> 다음 congestion까지 새 샘플 누적 대기 */
		} else if (p99 >= 0) {	/* [한국어] congestion이 아니고 이번에 유효한 p99를 얻었다면 */
			/* congestion이 아닐 때 p99를 보존, 다음 congestion 윈도우에서 재사용 */
			kqd->domain_p99[sched_domain] = p99;	/* [한국어] 다음 번 congestion 시 fallback으로 쓰기 위해 저장 */
		}
		if (p99 < 0)	/* [한국어] fallback까지 포함해도 유효한 p99가 없으면(둘 다 -1) */
			continue;	/* [한국어] 이 도메인은 이번 주기에 depth 조정을 건너뜀 (판단 근거 부족) */

		/*
		 * If this domain has bad latency, throttle less. Otherwise,
		 * throttle more iff we determined that there is congestion.
		 *
		 * The new depth is scaled linearly with the p99 latency vs the
		 * latency target. E.g., if the p99 is 3/4 of the target, then
		 * we throttle down to 3/4 of the current depth, and if the p99
		 * is 2x the target, then we double the depth.
		 */
		/* latency가 나쁘면 NVMe queue depth를 늘리고, congestion 상황의 양호한 도메인은 throttling */
		if (bad || p99 >= KYBER_GOOD_BUCKETS) {	/* [한국어] (전역 congestion) 또는 (이 도메인 자체가 target 초과)일 때만 depth 재조정 수행 */
			orig_depth = kqd->domain_tokens[sched_domain].sb.depth;	/* [한국어] 현재 sbitmap에 설정된 NVMe in-flight 상한 읽기 */
			/* p99 bucket에 비례하여 NVMe SQ in-flight 한도를 조정 (shift로 target 비율 반영) */
			depth = (orig_depth * (p99 + 1)) >> KYBER_LATENCY_SHIFT;	/* [한국어] (p99+1)/4 배로 스케일: p99가 target 근처(bucket 3)면 거의 1배 유지, 초과할수록 확대 */
			kyber_resize_domain(kqd, sched_domain, depth);	/* [한국어] 계산된 depth를 clamp 후 sbitmap에 실제 반영 */
		}
	}
}

/*
 * [한국어]
 * kyber_queue_data_alloc() - request_queue 단위 Kyber 전역 데이터(kqd) 할당/초기화
 *
 * @q: Kyber를 elevator로 선택한 blk-mq request_queue. NUMA 노드 힌트(q->node)와
 *     디바이스 번호(q->disk) 획득에 사용.
 * @return: 성공 시 완전히 초기화된 kyber_queue_data 포인터. 실패 시
 *          ERR_PTR(-ENOMEM) 등 에러 포인터 (IS_ERR()로 판별).
 *
 * elevator가 "kyber"로 설정될 때(echo kyber > .../scheduler) 최초 한 번
 * 호출되어, 이 큐가 평생 사용할 도메인별 token pool(domain_tokens),
 * per-cpu latency histogram(cpu_latency), 집계 타이머(timer)를 준비한다.
 * 실패 시 이미 할당된 자원을 역순으로 해제하는 goto 기반 에러 처리를 사용:
 * domain_tokens 일부만 초기화된 상태에서 실패하면 그보다 앞서 성공한
 * 것들만 sbitmap_queue_free로 해제한다. 프로세스 컨텍스트(sysfs write)에서
 * 실행되며, 아직 아무도 kqd를 참조하지 않으므로 락이 필요 없다.
 *
 * 호출 체인:
 *   kyber_alloc_sched_data() → [kyber_queue_data_alloc] → kzalloc_node(),
 *   alloc_percpu_gfp(), timer_setup(), sbitmap_queue_init_node()
 */
static struct kyber_queue_data *kyber_queue_data_alloc(struct request_queue *q)
{
	struct kyber_queue_data *kqd;	/* [한국어] 새로 할당할 이 큐의 Kyber 전역 데이터 포인터 */
	int ret = -ENOMEM;	/* [한국어] 에러 경로 공용 반환값. 기본값 -ENOMEM(메모리 부족)으로 초기화 */
	int i;	/* [한국어] 아래 두 초기화 루프에서 공용으로 쓰는 도메인 순회 인덱스 */

	kqd = kzalloc_node(sizeof(*kqd), GFP_KERNEL, q->node);	/* [한국어] q와 같은 NUMA 노드에 kqd를 0으로 채워 할당: cache locality 확보 */
	if (!kqd)	/* [한국어] 할당 실패 시 */
		goto err;	/* [한국어] 아직 아무 것도 할당하지 않았으므로 바로 ret(-ENOMEM) 반환 경로로 */

	kqd->q = q;			/* 이 blk-mq request_queue는 NVMe 드라이버의 nvme_ns->queue와 연결됨 */
	kqd->dev = disk_devt(q->disk);	/* nvme0n1 등 디스크 디바이스 번호, trace/debugfs에 사용 */

	kqd->cpu_latency = alloc_percpu_gfp(struct kyber_cpu_latency,
					    GFP_KERNEL | __GFP_ZERO);	/* [한국어] CPU마다 독립된 histogram 인스턴스를 0으로 채워 할당 - ISR이 cache contention 없이 기록 */
	if (!kqd->cpu_latency)	/* [한국어] percpu 할당 실패 시 */
		goto err_kqd;	/* [한국어] 앞서 할당한 kqd 자체를 해제하는 경로로 */

	timer_setup(&kqd->timer, kyber_timer_fn, 0);	/* [한국어] 100ms 주기 집계 타이머 콜백 등록 (flags=0, 아직 arm하지 않음 - 첫 완료 시 timer_reduce로 시작) */

	/* read/write/discard/other 도메인별 NVMe in-flight token pool 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		WARN_ON(!kyber_depth[i]);	/* [한국어] 모든 도메인은 depth > 0이어야 함 - 0이면 해당 도메인이 영원히 dispatch 불가능한 설정 오류 */
		WARN_ON(!kyber_batch_size[i]);	/* [한국어] 모든 도메인은 batch_size > 0이어야 함 - 0이면 kyber_dispatch_request의 batching 조건이 항상 거짓 */
		ret = sbitmap_queue_init_node(&kqd->domain_tokens[i],
					      kyber_depth[i], -1, false,
					      GFP_KERNEL, q->node);	/* [한국어] 이 도메인의 NVMe in-flight token sbitmap 생성: shift=-1(자동), round_robin=false */
		if (ret) {	/* [한국어] i번째 도메인의 sbitmap 초기화 실패 시 */
			while (--i >= 0)	/* [한국어] 이미 성공적으로 초기화된 이전 도메인들(0..i-1)을 역순으로 */
				sbitmap_queue_free(&kqd->domain_tokens[i]);	/* [한국어] 해제하여 메모리 누수 방지 */
			goto err_buckets;	/* [한국어] cpu_latency percpu 메모리를 해제하는 경로로 */
		}
	}

	for (i = 0; i < KYBER_OTHER; i++) {
		kqd->domain_p99[i] = -1;	/* p99 percentile 아직 계산되지 않음을 표시 */
		kqd->latency_targets[i] = kyber_latency_targets[i];	/* read 2ms, write 10ms, discard 5s */
	}

	return kqd;	/* [한국어] 모든 초기화 성공 - 완전히 구성된 kqd를 kyber_alloc_sched_data로 반환 */

err_buckets:
	free_percpu(kqd->cpu_latency);	/* [한국어] domain_tokens 초기화 실패 시 이미 할당된 per-cpu histogram 메모리 회수 */
err_kqd:
	kfree(kqd);	/* [한국어] cpu_latency 할당 실패 시 kqd 구조체 자체 메모리 회수 */
err:
	return ERR_PTR(ret);	/* [한국어] 실패를 나타내는 에러 포인터 반환 - 호출자는 IS_ERR()로 판별 */
}

/*
 * [한국어]
 * kyber_depth_updated() - async 요청의 shallow depth를 async_depth로 재설정
 *
 * @q: 대상 request_queue. q->async_depth 값을 blk-mq shallow-depth 메커니즘에 반영.
 * @return: 없음(void).
 *
 * q->nr_requests(전체 tag 수)나 async_depth가 바뀔 때마다 blk-mq에 "비동기
 * 요청은 이 얕은 깊이까지만 tag를 받아라"는 제약을 다시 알려야 한다.
 * elevator_ops.depth_updated 콜백으로 등록되어 blk-mq가 알아서 적절한
 * 시점(초기화, nr_requests sysfs 변경 등)에 호출해준다. NVMe 관점에서는
 * 이 shallow depth가 비동기 쓰기 플러드로부터 동기 read용 NVMe SQ 슬롯을
 * 지키는 방어선이다. 프로세스 컨텍스트(sysfs write 또는 init 경로)에서
 * 실행된다.
 *
 * 호출 체인:
 *   kyber_init_sched() / blk-mq nr_requests 변경 → [kyber_depth_updated] →
 *   blk_mq_set_min_shallow_depth()
 */
static void kyber_depth_updated(struct request_queue *q)
{
	/* NVMe tag/CID 할당 시 비동기 쓰기가 async_depth 이상으로 tag를 독점하지 못하도록 얕은 한도 설정 */
	blk_mq_set_min_shallow_depth(q, q->async_depth);
}

/*
 * [한국어]
 * kyber_init_sched() - elevator_ops.init_sched 콜백: 큐 플래그/async_depth 초기화
 *
 * @q: Kyber로 전환되는 request_queue.
 * @eq: elevator.c가 미리 할당한 elevator_queue (elevator_data는 아직 비어있음
 *      — 이 함수 이후 kyber_alloc_sched_data가 채운다).
 * @return: 항상 0(성공). Kyber는 이 단계에서 실패할 만한 자원 할당을 하지 않는다.
 *
 * elevator.c가 "kyber"를 elevator_type으로 확정한 직후 호출되어, (1) 이
 * 큐에 대한 blk-stat 지연 계정(accounting)을 켜고(kyber_completed_request가
 * 쓰는 start_time_ns/io_start_time_ns 채우기용), (2) Kyber는 multi-queue
 * 전용 설계이므로 QUEUE_FLAG_SQ_SCHED(single-queue 스케줄러) 플래그를
 * 지우며, (3) q->nr_requests의 75%만 비동기 요청에 허용하도록 async_depth를
 * 계산해 kyber_depth_updated()로 즉시 반영한다. 프로세스 컨텍스트(elevator
 * 전환 syscall/sysfs 경로)에서 실행되며 동시 호출은 elevator.c의 상위
 * 락으로 직렬화된다.
 *
 * 호출 체인:
 *   elevator.c(elevator_switch 등) → [kyber_init_sched] →
 *   blk_stat_enable_accounting(), kyber_depth_updated()
 */
static int kyber_init_sched(struct request_queue *q, struct elevator_queue *eq)
{
	blk_stat_enable_accounting(q);	/* [한국어] rq->start_time_ns/io_start_time_ns 기록을 활성화: kyber_completed_request의 latency 계산 전제조건 */

	/* NVMe는 multi-queue이므로 single-queue 스케줄러 플래그를 해제 */
	blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;	/* [한국어] 이 큐의 활성 elevator를 kyber로 확정 (이후 dispatch/insert 등 콜백이 kyber_sched.ops를 통해 호출됨) */
	/* 동기 요청을 위해 전체 request pool의 (100 - 75)% = 25%를 예약: NVMe SQ의 sync read 우선 보장 */
	q->async_depth = q->nr_requests * KYBER_DEFAULT_ASYNC_PERCENT / 100;
	kyber_depth_updated(q);	/* [한국어] 방금 계산한 async_depth를 blk-mq shallow-depth 메커니즘에 즉시 반영 */

	return 0;	/* [한국어] Kyber는 이 단계에서 실패할 자원 할당이 없어 항상 성공 */
}

/*
 * [한국어]
 * kyber_alloc_sched_data() - elevator_ops.alloc_sched_data 콜백: kqd 할당
 *
 * @q: Kyber elevator_data(kqd)가 필요한 request_queue.
 * @return: 성공 시 kyber_queue_data 포인터(void*로 캐스팅되어 e->elevator_data에
 *          저장됨). 실패 시 NULL (elevator_data alloc_sched_data 계약은 void*
 *          반환이라 ERR_PTR을 그대로 넘기지 않고 NULL로 정규화한다).
 *
 * elevator.c가 elevator_data 슬롯을 채우기 위해 호출하는 얇은 래퍼로,
 * 실제 할당/초기화 로직은 kyber_queue_data_alloc()에 위임한다. 이 함수가
 * 별도로 존재하는 이유는 elevator_ops의 콜백 시그니처(반환 타입 void*)와
 * kyber_queue_data_alloc()의 시그니처(반환 타입 kyber_queue_data*, 에러
 * 포인터 사용)를 어댑팅하기 위해서다. 프로세스 컨텍스트에서 실행.
 *
 * 호출 체인:
 *   elevator.c(elevator_switch) → [kyber_alloc_sched_data] →
 *   kyber_queue_data_alloc()
 */
static void *kyber_alloc_sched_data(struct request_queue *q)
{
	struct kyber_queue_data *kqd;	/* [한국어] kyber_queue_data_alloc()이 반환할 새 전역 데이터 포인터 */

	kqd = kyber_queue_data_alloc(q);	/* [한국어] 실제 할당/초기화 수행 (domain_tokens, cpu_latency, timer 등) */
	if (IS_ERR(kqd))	/* [한국어] 에러 포인터(-ENOMEM 등)이면 */
		return NULL;	/* [한국어] alloc_sched_data 콜백 계약에 맞춰 NULL로 정규화하여 실패 통지 */

	/* 할당된 kqd는 elevator_data로 등록되어 NVMe request 생명주기 내내 domain_tokens 관리에 사용 */
	return kqd;
}

/*
 * [한국어]
 * kyber_exit_sched() - elevator_ops.exit_sched 콜백: 타이머 정지 및 계정 해제
 *
 * @e: 종료 중인 elevator_queue. e->elevator_data가 kyber_queue_data(kqd).
 * @return: 없음(void).
 *
 * 스케줄러가 다른 elevator로 교체되거나 큐가 제거될 때 호출되어, 아직 실행
 * 중일 수 있는 kyber_timer_fn 타이머를 안전하게 정지시킨다(타이머가 kqd를
 * 참조하는 채로 남아 있으면 이후 kyber_free_sched_data가 kqd를 해제할 때
 * use-after-free가 발생하므로 반드시 이 시점에 타이머를 멈춰야 한다).
 * 이어서 blk-stat 지연 계정을 꺼서 더 이상 start_time_ns/io_start_time_ns를
 * 갱신하지 않게 한다. 프로세스 컨텍스트(elevator 전환/큐 제거 경로)에서
 * 실행되며, timer_shutdown_sync()가 내부적으로 타이머 콜백이 다른 CPU에서
 * 진행 중이면 완료될 때까지 대기한다.
 *
 * 호출 체인:
 *   elevator.c(elevator_switch/blk_cleanup_queue) → [kyber_exit_sched] →
 *   timer_shutdown_sync(), blk_stat_disable_accounting()
 */
static void kyber_exit_sched(struct elevator_queue *e)
{
	struct kyber_queue_data *kqd = e->elevator_data;	/* [한국어] 이 elevator에 연결된 Kyber 전역 데이터 획득 */

	/* NVMe 컨트롤러 제거/스케줄러 교체 시 latency 집계 타이머를 안전히 종료 */
	timer_shutdown_sync(&kqd->timer);	/* [한국어] 타이머 콜백이 다른 CPU에서 실행 중이면 완료까지 대기 후 재예약 금지 상태로 전환 - 이후 kqd 해제와의 race 차단 */
	blk_stat_disable_accounting(kqd->q);	/* [한국어] kyber_init_sched에서 켰던 지연 계정을 꺼서 start_time_ns 등 추가 기록 중지 */
}

/*
 * [한국어]
 * kyber_free_sched_data() - elevator_ops.free_sched_data 콜백: kqd 메모리 해제
 *
 * @elv_data: 해제할 elevator_data. 실제 타입은 kyber_queue_data* 이지만
 *            콜백 시그니처가 void*이므로 캐스팅해서 받는다. NULL일 수 있음
 *            (kyber_alloc_sched_data가 실패해 NULL을 반환한 경우).
 * @return: 없음(void).
 *
 * kyber_exit_sched()으로 타이머가 이미 안전히 정지된 뒤, elevator.c가
 * elevator_queue 자체를 해제하는 과정에서 호출되어 kqd가 소유한 나머지
 * 자원(도메인별 domain_tokens sbitmap, per-cpu latency histogram, kqd
 * 구조체 자신)을 순서대로 해제한다. NULL 체크가 있는 이유는
 * kyber_alloc_sched_data 실패 경로에서도 elevator.c가 이 free 콜백을
 * 호출할 수 있기 때문(대칭적인 free 보장). 프로세스 컨텍스트(elevator
 * 전환/큐 제거 경로)에서 실행되며, 이 시점엔 이미 아무도 kqd를 참조하지
 * 않으므로 락이 필요 없다.
 *
 * 호출 체인:
 *   elevator.c(elevator_switch/blk_cleanup_queue, kyber_exit_sched 이후) →
 *   [kyber_free_sched_data] → sbitmap_queue_free(), free_percpu(), kfree()
 */
static void kyber_free_sched_data(void *elv_data)
{
	struct kyber_queue_data *kqd = elv_data;	/* [한국어] void* 콜백 인자를 실제 타입으로 캐스팅 */
	int i;	/* [한국어] 도메인 순회 인덱스 */

	if (!kqd)	/* [한국어] alloc 단계에서 실패해 NULL이 저장돼 있었던 경우 */
		return;	/* [한국어] 해제할 것이 없으므로 즉시 반환 */

	/* read/write/discard/other NVMe in-flight token bitmap 해제 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		sbitmap_queue_free(&kqd->domain_tokens[i]);	/* [한국어] 도메인별 sbitmap이 내부적으로 든 비트맵 배열/wait 상태 메모리 회수 */
	free_percpu(kqd->cpu_latency);	/* [한국어] 모든 CPU 인스턴스의 per-cpu histogram 메모리 일괄 회수 */
	kfree(kqd);	/* [한국어] kqd 구조체 자체 메모리 회수 - 이후 이 포인터는 무효 */
}

/*
 * [한국어]
 * kyber_ctx_queue_init() - kcq(software ctx별 대기열) 하나를 초기 상태로 설정
 *
 * @kcq: 초기화할 kyber_ctx_queue. khd->kcqs[] 배열의 한 원소.
 * @return: 없음(void).
 *
 * kyber_init_hctx()가 hctx->nr_ctx개의 kcq를 할당한 직후, 각 원소에 대해
 * 이 함수를 호출해 락과 도메인별 빈 리스트를 준비시킨다. 이 초기화가
 * 없으면 rq_list의 list_head가 미정의 상태로 남아 최초 list_add 시
 * 커널 패닉(포인터 미초기화)이 발생한다. 프로세스 컨텍스트(hctx 초기화
 * 경로)에서 실행되며, 아직 이 kcq는 외부에 노출되지 않았으므로 동시성
 * 걱정이 없다.
 *
 * 호출 체인:
 *   kyber_init_hctx() → [kyber_ctx_queue_init] → spin_lock_init(),
 *   INIT_LIST_HEAD()
 */
static void kyber_ctx_queue_init(struct kyber_ctx_queue *kcq)
{
	unsigned int i;	/* [한국어] KYBER_NUM_DOMAINS(4)개 도메인 리스트 순회 인덱스 */

	/* per-ctx 큐 보호: 동일 kcq에 bio merge와 dispatch가 동시에 접근 가능 (NVMe SQ 직전 단계) */
	spin_lock_init(&kcq->lock);
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		INIT_LIST_HEAD(&kcq->rq_list[i]);	/* [한국어] 도메인별 대기 리스트를 빈 순환 리스트(자기 자신을 가리킴)로 초기화 */
}

/*
 * [한국어]
 * kyber_init_hctx() - elevator_ops.init_hctx 콜백: per-hctx Kyber 데이터(khd) 생성
 *
 * @hctx: 새로 초기화되는 blk_mq_hw_ctx. (추정) NVMe 컨트롤러의 한 SQ(또는
 *        SQ 그룹)에 대응. hctx->nr_ctx, hctx->numa_node를 khd 크기/NUMA
 *        배치 결정에 사용.
 * @hctx_idx: 이 hctx의 전역 인덱스 (Kyber 로직에서는 사용하지 않고 콜백
 *            시그니처 호환을 위해서만 받음).
 * @return: 성공 시 0. 메모리 할당 실패 시 -ENOMEM.
 *
 * blk-mq가 hctx(하드웨어 컨텍스트, NVMe SQ에 대응)를 만들 때마다 호출되어
 * 이 hctx 전용 Kyber 데이터(khd)를 준비한다: khd 자체, per-ctx 큐 배열
 * (kcqs, hctx->nr_ctx개), 도메인별 kcq 존재 여부 비트맵(kcq_map), dispatch
 * 대기열(rqs)과 domain_tokens 대기용 wait 엔트리(domain_wait)를 모두
 * 초기화한다. 에러 처리는 goto로 역순 해제: kcq_map 초기화 중 실패하면
 * 그보다 앞서 성공한 kcq_map[]들만 해제 후 kcqs로, kcqs 할당 자체가
 * 실패하면 khd로 넘어간다. 프로세스 컨텍스트(hctx 생성 경로)에서
 * 실행되며, 아직 hctx->sched_data에 연결되지 않았으므로 이 khd에 대한
 * 동시 접근은 없다.
 *
 * 호출 체인:
 *   blk-mq(hctx 할당 경로) → [kyber_init_hctx] → kyber_ctx_queue_init(),
 *   sbitmap_init_node(), init_waitqueue_func_entry()
 */
static int kyber_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kyber_hctx_data *khd;	/* [한국어] 새로 할당할 이 hctx 전용 Kyber 데이터 포인터 */
	int i;	/* [한국어] 아래 여러 초기화 루프에서 공용으로 쓰는 순회 인덱스 */

	/* (추정) 이 hctx는 NVMe 컨트롤러의 한 SQ(또는 SQ 그룹)에 매핑됨 */
	khd = kmalloc_node(sizeof(*khd), GFP_KERNEL, hctx->numa_node);	/* [한국어] hctx와 같은 NUMA 노드에 khd 할당 - NVMe SQ 처리 CPU와 cache locality 확보 */
	if (!khd)	/* [한국어] 할당 실패 시 */
		return -ENOMEM;	/* [한국어] 아직 아무 것도 추가 할당하지 않았으므로 바로 실패 반환 */

	khd->kcqs = kmalloc_array_node(hctx->nr_ctx,
				       sizeof(struct kyber_ctx_queue),
				       GFP_KERNEL, hctx->numa_node);	/* [한국어] software ctx 개수만큼 per-ctx 큐 배열 할당 */
	if (!khd->kcqs)	/* [한국어] kcqs 배열 할당 실패 시 */
		goto err_khd;	/* [한국어] 앞서 할당한 khd를 해제하는 경로로 */

	/* hctx에 속한 모든 software ctx에 대해 NVMe SQ 직전 per-ctx 대기열 초기화 */
	for (i = 0; i < hctx->nr_ctx; i++)
		kyber_ctx_queue_init(&khd->kcqs[i]);	/* [한국어] 각 kcq의 lock과 도메인별 rq_list를 빈 상태로 설정 */

	/* 각 도메인별로 어느 kcq에 request가 있는지를 빠르게 스캔할 sbitmap 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		if (sbitmap_init_node(&khd->kcq_map[i], hctx->nr_ctx,
				      ilog2(8), GFP_KERNEL, hctx->numa_node,
				      false, false)) {	/* [한국어] 비트 수=nr_ctx, shift=log2(8)(워드당 8비트 그룹), round_robin=false, alloc_hint=false */
			while (--i >= 0)	/* [한국어] 이미 성공한 이전 도메인들(0..i-1)의 kcq_map을 역순으로 */
				sbitmap_free(&khd->kcq_map[i]);	/* [한국어] 해제하여 메모리 누수 방지 */
			goto err_kcqs;	/* [한국어] kcqs 배열을 해제하는 경로로 */
		}
	}

	/* 동일 NVMe SQ/hctx에서 dispatch 순서와 batching 상태를 직렬화 */
	spin_lock_init(&khd->lock);

	/* domain별 dispatch 대기열(rqs)과 domain_tokens 대기(wait queue) 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		INIT_LIST_HEAD(&khd->rqs[i]);	/* [한국어] flush된 request를 담을 도메인별 dispatch 대기열을 빈 리스트로 초기화 */
		khd->domain_wait[i].sbq = NULL;	/* [한국어] 아직 어느 sbitmap_queue에도 등록되지 않은 상태로 표시 */
		init_waitqueue_func_entry(&khd->domain_wait[i].wait,
					  kyber_domain_wake);	/* [한국어] token 반환 시 호출될 wake 콜백을 kyber_domain_wake로 고정 등록 */
		khd->domain_wait[i].wait.private = hctx;	/* [한국어] wake 콜백이 어느 hctx를 재개(run_hw_queue)할지 알 수 있도록 저장 */
		INIT_LIST_HEAD(&khd->domain_wait[i].wait.entry);	/* [한국어] wait queue 리스트 노드를 빈 상태로 초기화(등록 전 list_empty 판정 가능하게) */
		atomic_set(&khd->wait_index[i], 0);	/* [한국어] sbq_wait_state 라운드로빈 인덱스를 0부터 시작 */
	}

	khd->cur_domain = 0;	/* [한국어] 최초 dispatch는 KYBER_READ(인덱스 0) 도메인부터 시작 */
	khd->batching = 0;	/* [한국어] 아직 어떤 도메인도 batch를 시작하지 않은 상태 */

	hctx->sched_data = khd;	/* [한국어] 초기화 완료된 khd를 hctx에 연결 - 이 시점부터 다른 콜백들이 hctx->sched_data로 접근 가능 */

	return 0;	/* [한국어] 모든 초기화 성공 */

err_kcqs:
	kfree(khd->kcqs);	/* [한국어] kcq_map 초기화 실패 시 앞서 할당한 kcqs 배열 회수 */
err_khd:
	kfree(khd);	/* [한국어] kcqs 할당 실패 시 khd 구조체 자체 회수 */
	return -ENOMEM;	/* [한국어] 자원 부족으로 이 hctx는 Kyber를 사용할 수 없음을 blk-mq에 통지 */
}

/*
 * [한국어]
 * kyber_exit_hctx() - elevator_ops.exit_hctx 콜백: khd 메모리 정리
 *
 * @hctx: 소멸 중인 blk_mq_hw_ctx. hctx->sched_data가 해제 대상 khd.
 * @hctx_idx: 이 hctx의 전역 인덱스 (사용하지 않음, 시그니처 호환용).
 * @return: 없음(void).
 *
 * hctx가 제거될 때(CPU hotplug로 인한 queue 재매핑, 큐 해제 등) 호출되어
 * kyber_init_hctx()가 할당했던 자원들을 정확히 역순으로 해제한다:
 * kcq_map 비트맵들, kcqs 배열, khd 자체. 프로세스 컨텍스트에서 실행되며,
 * 이 시점엔 이미 blk-mq가 이 hctx로의 새 dispatch를 막았다고 가정하므로
 * 별도 락 없이 해제해도 안전하다.
 *
 * 호출 체인:
 *   blk-mq(hctx 해제 경로) → [kyber_exit_hctx] → sbitmap_free(), kfree()
 */
static void kyber_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 해제 대상 khd 획득 */
	int i;	/* [한국어] 도메인 순회 인덱스 */

	/* NVMe SQ에 대응하는 hctx가 소멸할 때 domain별 kcq_map bitmap 해제 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		sbitmap_free(&khd->kcq_map[i]);	/* [한국어] 도메인별 kcq 존재 비트맵의 내부 배열 메모리 회수 */
	kfree(khd->kcqs);	/* [한국어] per-ctx 큐 배열 메모리 회수 */
	kfree(hctx->sched_data);	/* [한국어] khd 구조체 자체 메모리 회수 - 이후 hctx->sched_data는 무효 포인터가 됨(blk-mq가 곧 NULL 처리) */
}

/*
 * [한국어]
 * rq_get_domain_token() - request에 저장된 domain token 번호 읽기
 *
 * @rq: token 번호를 조회할 request. rq->elv.priv[0]에 token이 저장돼 있음
 *      (elv.priv는 elevator별로 자유롭게 쓸 수 있는 private 슬롯).
 * @return: 획득된 domain token 번호(0 이상, sbitmap 내 슬롯 인덱스) 또는
 *          -1(아직 NVMe SQ로 dispatch되지 않았거나 token 미보유).
 *
 * request의 elv.priv[0]는 포인터 타입이라 token(int)을 (void*)로 캐스팅해
 * 저장해 둔 것을 다시 long/int로 역캐스팅해 꺼내는 단순 접근자(accessor)다.
 * 이 값이 -1인지 여부로 "이 request가 지금 NVMe SQ in-flight 한도에
 * 포함돼 있는지"를 판별할 수 있다. 어떤 컨텍스트에서 호출되든 해당
 * request는 그 순간 단일 스레드만 다루므로 동기화가 필요 없다.
 *
 * 호출 체인:
 *   rq_clear_domain_token() → [rq_get_domain_token] → (단순 필드 접근,
 *   하위 호출 없음)
 */
static int rq_get_domain_token(struct request *rq)
{
	/* request->elv.priv[0]에 domain token 번호 저장: -1이면 아직 NVMe SQ로 dispatch되지 않음 */
	return (long)rq->elv.priv[0];
}

/*
 * [한국어]
 * rq_set_domain_token() - request에 domain token 번호 기록
 *
 * @rq: token을 기록할 request.
 * @token: 기록할 domain token 번호. kyber_get_domain_token()이 획득한
 *         sbitmap 슬롯 인덱스(0 이상) 또는 초기화 값 -1.
 * @return: 없음(void).
 *
 * rq_get_domain_token()의 대응(setter) 함수로, token(int)을 (void*)로
 * 캐스팅해 rq->elv.priv[0]에 저장한다. -1로 설정하면 "아직 NVMe SQ
 * in-flight에 포함되지 않음"을, 0 이상 값으로 설정하면 "이제 이 request가
 * domain_tokens의 해당 슬롯을 점유 중"임을 의미한다. 단일 request에 대해
 * 특정 시점에는 한 스레드만 호출하므로 락이 필요 없다.
 *
 * 호출 체인:
 *   kyber_prepare_request() / kyber_dispatch_cur_domain() →
 *   [rq_set_domain_token] → (단순 필드 대입, 하위 호출 없음)
 */
static void rq_set_domain_token(struct request *rq, int token)
{
	/* domain token을 request에 기록 -> 이제 해당 request는 NVMe SQ in-flight 한도에 포함됨 */
	rq->elv.priv[0] = (void *)(long)token;
}

/*
 * [한국어]
 * rq_clear_domain_token() - request 완료/재배치 시 domain token을 sbitmap에 반환
 *
 * @kqd: 이 request가 속한 request_queue의 Kyber 전역 데이터. domain_tokens[]가
 *       반환 대상 sbitmap_queue.
 * @rq: token을 반환할 request. rq->cmd_flags로 도메인을 재확인하고,
 *      rq->mq_ctx->cpu로 반환 시 선호 CPU 힌트를 제공.
 * @return: 없음(void).
 *
 * request가 완료되거나 재배치(requeue)될 때, 이전에 kyber_get_domain_token()이
 * 부여했던 domain token을 다시 sbitmap_queue로 되돌려주는 짝 함수다. 이
 * 반환이 있어야 sbitmap_queue_clear() 내부에서 대기 중인 hctx의 wait
 * 콜백(kyber_domain_wake)이 깨어나 dispatch를 재개할 수 있다. token이
 * -1(아직 할당된 적 없음)이면 아무 것도 하지 않는다 — 아직 dispatch되지
 * 않은 request가 취소되는 경우가 이에 해당한다. CPU 힌트를 반환 시
 * 함께 전달하는 이유는 sbitmap이 해당 CPU의 캐시 라인 근처 비트를
 * 재사용하도록 유도해, 다음 dispatch에서 캐시 지역성을 높이기 위함이다.
 * NVMe CQ ISR(softirq) 또는 프로세스 컨텍스트(requeue) 양쪽에서 호출될
 * 수 있으나, 단일 request에 대해서는 항상 순차적으로만 호출된다.
 *
 * 호출 체인:
 *   kyber_finish_request() → [rq_clear_domain_token] →
 *   rq_get_domain_token(), sbitmap_queue_clear()
 */
static void rq_clear_domain_token(struct kyber_queue_data *kqd,
				  struct request *rq)
{
	unsigned int sched_domain;	/* [한국어] token이 속했던 스케줄링 도메인 인덱스 */
	int nr;	/* [한국어] 이 request가 보유 중이던 token 번호 (-1이면 미보유) */

	nr = rq_get_domain_token(rq);	/* [한국어] request에 기록된 token 번호 조회 */
	/* token이 할당된 상태에서만 반환: NVMe CQ ISR 완료 시 SQ in-flight 자리 회수 */
	if (nr != -1) {
		sched_domain = kyber_sched_domain(rq->cmd_flags);	/* [한국어] 이 request가 어느 domain_tokens 풀에서 왔는지 재확인 */
		/* 동일 CPU에서의 sbitmap clear는 cache locality 향상 (NVMe CQ affinity 고려) */
		sbitmap_queue_clear(&kqd->domain_tokens[sched_domain], nr,
				    rq->mq_ctx->cpu);	/* [한국어] nr번 슬롯을 반환하고, 완료 처리 중인 현재 CPU를 힌트로 전달 -> 대기 중인 hctx의 wake 콜백을 유발할 수 있음 */
	}
}

/*
 * [한국어]
 * kyber_limit_depth() - elevator_ops.limit_depth 콜백: 비동기 요청의 shallow depth 제한
 *
 * @opf: 할당하려는 request의 op-flags. blk_mq_is_sync_read()로 "동기 read인지"
 *       판별하는 데 사용.
 * @data: blk_mq_get_request()가 넘기는 tag 할당 컨텍스트. data->shallow_depth를
 *        설정하면 blk-mq가 tag 비트맵에서 그 깊이까지만 탐색하도록 제한된다.
 * @return: 없음(void). data->shallow_depth를 조건부로 덮어씀.
 *
 * NVMe tag(CID)는 request_queue 전체가 공유하는 유한 자원이다. 비동기
 * 쓰기가 폭주하면 모든 tag를 소진해 지연에 민감한 동기 read가 아예 tag를
 * 못 받는 starvation이 발생할 수 있다. 이를 막기 위해 동기 read가
 * 아닌 모든 요청(비동기 write, 비동기 read 등)에 대해 tag 탐색 범위를
 * q->async_depth(전체의 75%)로 좁혀, 나머지 25%의 tag 공간은 사실상
 * 동기 read 전용으로 남겨둔다. blk_mq_get_request()가 tag 할당 직전에
 * 호출하는 콜백이라 프로세스 컨텍스트에서 실행되며 재진입 걱정이 없다.
 *
 * 호출 체인:
 *   blk_mq_get_request() → [kyber_limit_depth] → blk_mq_is_sync_read()
 */
static void kyber_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	/* 동기 read가 아닌 요청(비동기 write 등)의 tag/CID 할당 깊이를 async_depth로 제한 */
	if (!blk_mq_is_sync_read(opf))
		data->shallow_depth = data->q->async_depth;
}

/*
 * [한국어]
 * kyber_bio_merge() - elevator_ops.bio_merge 콜백: bio를 기존 request와 merge 시도
 *
 * @q: bio가 제출된 request_queue.
 * @bio: 새로 도착한 bio. 아직 request로 변환되지 않은 상태 — merge에 성공하면
 *       request 할당 자체를 건너뛸 수 있다.
 * @nr_segs: 이 bio의 세그먼트(물리적으로 연속된 페이지 조각) 개수.
 *           blk_bio_list_merge()가 병합 가능성 판단에 사용.
 * @return: true면 기존 request에 병합 성공(새 request 불필요), false면
 *          병합 대상을 찾지 못해 호출자가 새 request를 만들어야 함.
 *
 * bio가 도착할 때마다 최적화를 위해, request로 변환하기 전에 먼저 인접한
 * 기존 request와 합칠 수 있는지 시도한다. 병합이 성공하면 NVMe SQ에
 * 새 CID(Command ID)를 소모하지 않고 기존 request의 PRP(Physical Region
 * Page) 또는 SGL(Scatter-Gather List)만 확장되므로, 결과적으로 NVMe
 * doorbell(SQ tail 갱신) 횟수 자체가 줄어 처리량이 향상된다. bio의 CPU
 * ctx와 opcode로 정확히 같은 kcq/도메인 리스트를 찾아야 병합 후보가
 * 일치하므로, kyber_insert_requests()와 동일한 kcq 선택 로직
 * (ctx->index_hw[hctx->type])을 사용한다. 프로세스 컨텍스트(bio 제출
 * 경로)에서 실행되며 kcq->lock으로 동시 merge/insert를 직렬화한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio() → blk_attempt_bio_merge() → ... →
 *   [kyber_bio_merge] → blk_bio_list_merge()
 */
static bool kyber_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	/* bio를 제출한 CPU의 software ctx 획득: NVMe SQ 선택과 무관하게 blk-mq 내부의 소프트웨어 분류 */
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
	/* bio->bi_opf와 ctx로 hctx 선택 -> NVMe 관점에서는 이 bio가 어느 nvme_queue/SQ로 갈지의 전단계 */
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(bio->bi_opf, ctx);
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 선택된 hctx의 Kyber 데이터 - kcqs 배열 접근용 */
	/* 동일 hctx 내에서 ctx->index_hw를 사용해 per-ctx 대기열 선택 (도착 순서/merge 단위 보존) */
	struct kyber_ctx_queue *kcq = &khd->kcqs[ctx->index_hw[hctx->type]];
	unsigned int sched_domain = kyber_sched_domain(bio->bi_opf);	/* [한국어] bio의 opcode로 병합을 시도할 도메인(READ/WRITE/DISCARD/OTHER) 결정 */
	struct list_head *rq_list = &kcq->rq_list[sched_domain];	/* [한국어] 병합 후보 request들이 있는 도메인별 대기 리스트 */
	bool merged;	/* [한국어] blk_bio_list_merge()의 병합 성공 여부 결과 */

	/* kcq lock: NVMe SQ로 가기 전 bio merge와 insert를 직렬화 */
	spin_lock(&kcq->lock);
	/* merge 성공 시 기존 request의 PRP/SGL만 확장되고 새 CID는 소모되지 않음.
	 * (DMA scatter-gather list/PRP list 확장 -> nvme_setup_cmd 시 SGL/PRP entry 추가) */
	merged = blk_bio_list_merge(hctx->queue, rq_list, bio, nr_segs);
	spin_unlock(&kcq->lock);

	return merged;	/* [한국어] 호출자(blk_attempt_bio_merge)에게 병합 여부 통지 - false면 새 request 생성 경로로 진행 */
}

/*
 * [한국어]
 * kyber_prepare_request() - elevator_ops.prepare_request 콜백: domain token 초기화
 *
 * @rq: 방금 할당된 request. 아직 어떤 elevator 상태도 설정되지 않은 상태.
 * @return: 없음(void).
 *
 * request가 새로 할당되면 아직 NVMe SQ dispatch 단계에 이르지 않았으므로
 * domain token 슬롯을 "미보유" 상태(-1)로 명시적으로 표시해야 한다. 이
 * 초기화가 없으면 elv.priv[0]에 남아있는 이전 값(메모리 재사용 시 다른
 * request의 token 번호)이 우연히 유효한 것처럼 오인되어, 나중에
 * rq_clear_domain_token()이 실제로는 보유하지 않은 token을 잘못
 * 반환(double-free 유사 버그)할 위험이 있다. 프로세스 컨텍스트(request
 * 할당 경로)에서 실행된다.
 *
 * 호출 체인:
 *   blk_mq_get_request() → ... → [kyber_prepare_request] →
 *   rq_set_domain_token()
 */
static void kyber_prepare_request(struct request *rq)
{
	/* request 생성 시점에는 아직 NVMe SQ에 할당되지 않았으므로 domain token을 무효값(-1)으로 초기화 */
	rq_set_domain_token(rq, -1);
}

/*
 * [한국어]
 * kyber_insert_requests() - elevator_ops.insert_requests 콜백: kcq에 request 삽입
 *
 * @hctx: 삽입 대상 hctx. hctx->sched_data가 khd.
 * @rq_list: 삽입할 request들의 임시 리스트 (blk-mq가 requeue나 plug flush로
 *           모아온 목록). 이 함수가 소비하며 각 원소를 자신의 kcq로 옮긴다.
 * @flags: BLK_MQ_INSERT_AT_HEAD 등 삽입 방식 플래그. AT_HEAD면 우선순위
 *         높은 request를 리스트 맨 앞에 두어 다음 dispatch에서 먼저 나가게 함.
 * @return: 없음(void).
 *
 * bio 병합에 실패했거나 requeue/plug-flush로 도착한 request들을 각자의
 * 도메인(kyber_sched_domain)과 소프트웨어 ctx(rq->mq_ctx->index_hw)에 맞는
 * kcq->rq_list[]로 옮겨 담는다. 이 시점의 request는 아직 NVMe SQ로 가지
 * 않고 Kyber의 per-ctx 스테이징 큐에 머무르며 추가 bio 병합 기회를
 * 유지한다. 삽입 후에는 kcq_map의 해당 비트를 세트하여
 * kyber_flush_busy_kcqs()/flush_busy_kcq()가 이후 dispatch 시점에 이
 * kcq를 빠르게 찾아낼 수 있게 한다. 프로세스 컨텍스트(bio 제출/requeue
 * 경로)에서 실행되며 kcq->lock으로 동시 merge와의 경쟁을 방지한다.
 *
 * 호출 체인:
 *   blk_mq_sched_insert_request() / blk_mq_sched_insert_requests() → ... →
 *   [kyber_insert_requests] → kyber_sched_domain(), sbitmap_set_bit()
 */
static void kyber_insert_requests(struct blk_mq_hw_ctx *hctx,
				  struct list_head *rq_list,
				  blk_insert_t flags)
{
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 이 hctx의 Kyber 데이터 - kcqs/kcq_map 접근용 */
	struct request *rq, *next;	/* [한국어] list_for_each_entry_safe 순회용: rq는 현재 원소, next는 미리 저장해둔 다음 원소(rq가 리스트에서 제거돼도 안전) */

	/* 재배치(requeue)나 plug flush로 인해 도착한 request들을 per-ctx/domain 리스트에 삽입 */
	list_for_each_entry_safe(rq, next, rq_list, queuelist) {
		unsigned int sched_domain = kyber_sched_domain(rq->cmd_flags);	/* [한국어] 이 request가 들어갈 도메인 결정 */
		struct kyber_ctx_queue *kcq = &khd->kcqs[rq->mq_ctx->index_hw[hctx->type]];	/* [한국어] request를 제출한 ctx에 대응하는 kcq 선택 - bio_merge와 동일한 인덱싱 규칙 */
		struct list_head *head = &kcq->rq_list[sched_domain];	/* [한국어] 실제로 request를 옮겨 담을 도메인별 리스트 헤드 */

		spin_lock(&kcq->lock);
		trace_block_rq_insert(rq);	/* [한국어] ftrace block:block_rq_insert 이벤트 발생 - 블록 계층 표준 트레이스포인트 */
		/* BLK_MQ_INSERT_AT_HEAD: 우선순위가 높은 request를 리스트 앞에 배치 (NVMe SQ에 먼저 도달) */
		if (flags & BLK_MQ_INSERT_AT_HEAD)
			list_move(&rq->queuelist, head);	/* [한국어] 리스트 맨 앞으로 이동 - 다음 flush 시 가장 먼저 dispatch됨 */
		else
			list_move_tail(&rq->queuelist, head);	/* [한국어] 리스트 맨 뒤로 이동 - 일반적인 FIFO 순서 유지 */
		/* 이 kcq에 해당 도메인 request가 있음을 표시 -> flush_busy_kcq에서 빠르게 스캔 */
		sbitmap_set_bit(&khd->kcq_map[sched_domain],
				rq->mq_ctx->index_hw[hctx->type]);
		spin_unlock(&kcq->lock);
	}
}

/*
 * [한국어]
 * kyber_finish_request() - elevator_ops.finish_request/.requeue_request 콜백
 *
 * @rq: 완료되었거나 재배치되는 request.
 * @return: 없음(void).
 *
 * request의 생애주기가 끝나는 두 갈래 경로(정상 완료, 또는 타임아웃/에러로
 * 인한 requeue) 모두에서 공통으로 해야 할 일 — domain token 반환 —을
 * 처리하는 단일 진입점이다. 두 콜백(finish_request, requeue_request)에
 * 동일한 함수가 등록된 이유는 "이 request가 더 이상 NVMe SQ in-flight를
 * 점유하지 않는다"는 사실이 두 경우 모두 동일하기 때문이다. 반환된
 * token은 sbitmap_queue_clear() 내부에서 대기 중인 hctx를 깨울 수 있다.
 * NVMe CQ ISR(softirq) 또는 에러 처리 경로(프로세스 컨텍스트)에서
 * 호출될 수 있다.
 *
 * 호출 체인:
 *   blk-mq(완료 경로) / blk_mq_requeue_request() → [kyber_finish_request] →
 *   rq_clear_domain_token()
 */
static void kyber_finish_request(struct request *rq)
{
	struct kyber_queue_data *kqd = rq->q->elevator->elevator_data;	/* [한국어] 이 request가 속한 큐의 Kyber 전역 데이터 획득 */

	/* request 완료/재배치 시 domain token 반환 -> NVMe SQ in-flight 자리 해제 및 잠자는 hctx 깨움 */
	rq_clear_domain_token(kqd, rq);
}

/*
 * [한국어]
 * add_latency_sample() - 완료 지연 하나를 target 대비 상대적 bucket에 기록
 *
 * @cpu_latency: 기록 대상 per-cpu histogram (현재 실행 중인 CPU의 인스턴스).
 * @sched_domain: 기록할 스케줄링 도메인 인덱스 (KYBER_READ/WRITE/DISCARD).
 * @type: 기록할 latency 유형 (KYBER_TOTAL_LATENCY 또는 KYBER_IO_LATENCY).
 * @target: 이 도메인의 목표 지연(나노초) — bucket 폭 계산 기준.
 * @latency: 이번 request의 실측 지연(나노초). now - start_time_ns 또는
 *           now - io_start_time_ns.
 * @return: 없음(void). cpu_latency->buckets[][][]을 원자적으로 1 증가.
 *
 * 실측 latency를 target 대비 몇 분의 1인지로 정규화해 8개 bucket 중
 * 하나에 분류한다(0: ≤target/4, 3: ≤target, 7: >1.75*target). latency가
 * 0(또는 이하)이면 시계 해상도 이슈 등으로 간주해 무조건 bucket 0으로
 * 처리한다. div64_u64로 64비트 나눗셈을 수행하는 이유는 나노초 단위
 * latency/target 값이 32비트 범위를 넘을 수 있기 때문이다. NVMe CQ
 * ISR(softirq)에서 호출되며, 동일 CPU 내에서는 인터럽트에 의해서만
 * 재진입될 수 있으므로 atomic_inc로 안전하게 카운터를 증가시킨다.
 *
 * 호출 체인:
 *   kyber_completed_request() → [add_latency_sample] → atomic_inc()
 */
static void add_latency_sample(struct kyber_cpu_latency *cpu_latency,
			       unsigned int sched_domain, unsigned int type,
			       u64 target, u64 latency)
{
	unsigned int bucket;	/* [한국어] 이번 latency가 속할 것으로 계산된 bucket 인덱스(0~7) */
	u64 divisor;	/* [한국어] bucket 하나의 폭 = target / 4(KYBER_LATENCY_SHIFT만큼 우측 시프트) */

	if (latency > 0) {	/* [한국어] 정상적인 양의 지연 값이면 */
		/* target latency를 1/4 단위로 나눠 NVMe 완료 시간이 target 대비 어느 구간인지 계산 */
		divisor = max_t(u64, target >> KYBER_LATENCY_SHIFT, 1);	/* [한국어] target/4 계산, 단 0이 되지 않도록 최소 1 보장(0으로 나누기 방지) */
		bucket = min_t(unsigned int, div64_u64(latency - 1, divisor),
			       KYBER_LATENCY_BUCKETS - 1);	/* [한국어] (latency-1)/divisor로 bucket 계산 후 마지막 bucket(7)을 넘지 않도록 clamp */
	} else {
		bucket = 0;	/* [한국어] latency가 0 이하인 비정상적 경우 가장 낮은 bucket으로 처리 */
	}

	/* NVMe CQ ISR와 timer_fn이 동시에 접근 가능하므로 atomic_inc로 per-cpu bucket 안전 증가 */
	atomic_inc(&cpu_latency->buckets[sched_domain][type][bucket]);
}

/*
 * [한국어]
 * kyber_completed_request() - elevator_ops.completed_request 콜백: 완료 지연 기록
 *
 * @rq: 방금 완료된 request. rq->cmd_flags로 도메인 판정, rq->start_time_ns/
 *      rq->io_start_time_ns로 두 종류의 지연을 계산.
 * @now: 완료 시각 (ktime, 나노초). NVMe CQ ISR이 완료를 처리하는 현재 시각.
 * @return: 없음(void). per-cpu histogram과 kqd->timer를 갱신.
 *
 * NVMe SSD가 CQ(Completion Queue)에 완료 entry를 채우면 blk-mq가 이
 * 콜백을 호출한다. KYBER_OTHER 도메인(FLUSH 등)은 애초에 latency_target이
 * 없으므로 즉시 반환해 histogram 오염을 막는다. 그 외 도메인에 대해서는
 * 두 가지 지연을 각각 기록한다: TOTAL_LATENCY(bio 생성부터 완료까지 —
 * 소프트웨어 큐 대기 시간 포함)와 IO_LATENCY(NVMe SQ 제출부터 완료까지 —
 * 순수 디바이스 처리 시간). get_cpu_ptr()/put_cpu_ptr()로 현재 CPU의
 * per-cpu histogram을 안전하게 획득/해제하여 preemption을 잠시 비활성화한
 * 채로 기록한다. 마지막으로 timer_reduce()를 호출해 100ms 이내에
 * kyber_timer_fn이 실행되도록 예약을 앞당긴다(이미 더 이른 시각으로
 * 예약돼 있으면 그대로 둠). NVMe CQ ISR(softirq) 컨텍스트에서 실행된다.
 *
 * 호출 체인:
 *   NVMe CQ ISR → nvme_irq() → nvme_complete_rq() → blk_mq_end_request() →
 *   ... → [kyber_completed_request] → add_latency_sample(), timer_reduce()
 */
static void kyber_completed_request(struct request *rq, u64 now)
{
	struct kyber_queue_data *kqd = rq->q->elevator->elevator_data;	/* [한국어] 이 request가 속한 큐의 Kyber 전역 데이터 획득 */
	struct kyber_cpu_latency *cpu_latency;	/* [한국어] 기록 대상이 될 현재 CPU의 per-cpu histogram 포인터 */
	unsigned int sched_domain;	/* [한국어] 이 request가 속한 스케줄링 도메인 */
	u64 target;	/* [한국어] 이 도메인의 목표 지연(나노초) - bucket 계산 기준값 */

	sched_domain = kyber_sched_domain(rq->cmd_flags);	/* [한국어] opcode로 도메인 재확인 */
	/* OTHER 도메인(NVMe FLUSH 등)은 latency target이 없어 histogram 기록 제외 */
	if (sched_domain == KYBER_OTHER)
		return;

	/* 현재 CPU의 per-cpu histogram 포인터 획득 (NVMe CQ ISR가 실행 중인 CPU) */
	cpu_latency = get_cpu_ptr(kqd->cpu_latency);	/* [한국어] percpu 포인터 획득과 동시에 preemption 비활성화(같은 CPU 내 재진입 방지) */
	target = kqd->latency_targets[sched_domain];	/* [한국어] 이 도메인의 사용자 설정/기본 목표 지연 읽기 */
	/* TOTAL_LATENCY: bio 생성(rq->start_time_ns)부터 NVMe CQ 완료(now)까지 전체 시간 */
	add_latency_sample(cpu_latency, sched_domain, KYBER_TOTAL_LATENCY,
			   target, now - rq->start_time_ns);
	/* IO_LATENCY: NVMe SQ 제출(rq->io_start_time_ns)부터 CQ 완료까지의 디바이스 시간 */
	add_latency_sample(cpu_latency, sched_domain, KYBER_IO_LATENCY, target,
			   now - rq->io_start_time_ns);
	put_cpu_ptr(kqd->cpu_latency);	/* [한국어] percpu 포인터 반환 및 preemption 재활성화 */

	/* 완료가 발생했으므로 100ms 내에 queue depth 재조정 타이머를 앞당김 */
	timer_reduce(&kqd->timer, jiffies + HZ / 10);
}

/* [한국어] flush_busy_kcq() 콜백에 전달되는 컨텍스트 — sbitmap_for_each_set()의
 * void *data 인자로 넘겨지는 클로저(closure) 역할을 한다. */
struct flush_kcq_data {
	struct kyber_hctx_data *khd;
	/* [한국어] flush 대상 kcq 배열(khd->kcqs)이 속한 hctx 데이터.
	 * 설정자: kyber_flush_busy_kcqs()가 지역 변수 data를 초기화할 때 대입.
	 * 읽는 자: flush_busy_kcq()가 flush_data->khd->kcqs[bitnr]로 kcq 접근.
	 * 값 범위: 유효한 kyber_hctx_data 포인터.
	 * 동기화: 스택에 위치한 임시 구조체라 별도 동기화 불필요 — 호출자가 이미
	 *   khd->lock을 쥔 상태에서 사용. */

	unsigned int sched_domain;
	/* [한국어] flush할 스케줄링 도메인 인덱스 (KYBER_READ 등).
	 * 설정자: kyber_flush_busy_kcqs()의 인자로 전달된 값을 그대로 대입.
	 * 읽는 자: flush_busy_kcq()가 kcq->rq_list[sched_domain]을 선택하는 데 사용.
	 * 값 범위: 0 ~ KYBER_NUM_DOMAINS-1.
	 * 동기화: 읽기 전용 값이므로 동기화 불필요. */

	struct list_head *list;
	/* [한국어] flush된 request들을 옮겨 담을 목적지 리스트 — 보통 khd->rqs[domain].
	 * 설정자: kyber_flush_busy_kcqs()가 호출자(kyber_dispatch_cur_domain)로부터
	 *   받은 rqs 포인터를 대입.
	 * 읽는 자: flush_busy_kcq()가 list_splice_tail_init(&kcq->rq_list[...], list)로
	 *   request들을 이 리스트 끝에 이어붙임.
	 * 값 범위: 유효한 list_head 포인터, 호출자가 이미 초기화한 리스트.
	 * 동기화: 별도 락 없음 — khd->lock 보호 하에 단일 스레드에서만 사용. */
};

/*
 * [한국어]
 * flush_busy_kcq() - sbitmap_for_each_set 콜백: 한 kcq의 도메인 request를 flush
 *
 * @sb: 순회 중인 kcq_map[sched_domain] 비트맵 (flush_data->khd->kcq_map[domain]과 동일).
 * @bitnr: 이번에 세트된 것으로 발견된 비트 번호 = ctx 인덱스(khd->kcqs[bitnr]).
 * @data: kyber_flush_busy_kcqs()가 넘긴 struct flush_kcq_data* (khd/도메인/목적지 리스트).
 * @return: 항상 true — sbitmap_for_each_set()에게 "순회를 계속하라"고 알림
 *          (false를 반환하면 조기 종료되지만 이 콜백은 모든 세트 비트를 처리해야 함).
 *
 * kyber_flush_busy_kcqs()가 kcq_map에서 세트된 비트(=request가 있는 kcq)를
 * 하나씩 찾을 때마다 호출되는 워커 함수. 실제로 해당 kcq의 도메인별
 * rq_list를 통째로 목적지 리스트(hctx->rqs[domain])로 스플라이스하고,
 * flush가 끝났으니 더 이상 request가 없다는 뜻으로 kcq_map 비트를
 * 클리어한다. kcq->lock으로 동시 bio_merge/insert와의 경쟁을 막는다.
 * 호출자(kyber_dispatch_cur_domain)가 이미 khd->lock을 쥔 상태이므로
 * 이 함수 자체는 재진입되지 않는다.
 *
 * 호출 체인:
 *   kyber_flush_busy_kcqs() → sbitmap_for_each_set() → [flush_busy_kcq] →
 *   list_splice_tail_init(), sbitmap_clear_bit()
 */
static bool flush_busy_kcq(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	struct flush_kcq_data *flush_data = data;	/* [한국어] void* 콜백 인자를 실제 타입으로 캐스팅 */
	/* bitnr은 ctx 인덱스: 해당 kcq에서 도메인별 request를 hctx dispatch 리스트로 이동 */
	struct kyber_ctx_queue *kcq = &flush_data->khd->kcqs[bitnr];

	spin_lock(&kcq->lock);
	/* per-ctx 대기열의 해당 도메인 request를 hctx->rqs[]로 옮김 (NVMe SQ 직전 단계) */
	list_splice_tail_init(&kcq->rq_list[flush_data->sched_domain],
			      flush_data->list);
	/* flush 완료 후 kcq_map 비트 클리어 -> 빈 kcq는 다음 스캔에서 스킵 */
	sbitmap_clear_bit(sb, bitnr);
	spin_unlock(&kcq->lock);

	return true;	/* [한국어] 이 kcq 처리 완료 - sbitmap_for_each_set에게 다음 세트 비트 계속 탐색 지시 */
}

/*
 * [한국어]
 * kyber_flush_busy_kcqs() - 현재 도메인의 모든 non-empty kcq를 hctx rqs[]로 flush
 *
 * @khd: flush 대상 hctx의 Kyber 데이터. kcq_map[]과 kcqs[] 소유자.
 * @sched_domain: flush할 스케줄링 도메인 인덱스.
 * @list: flush된 request들이 모일 목적지 리스트 (보통 khd->rqs[sched_domain]).
 * @return: 없음(void).
 *
 * kyber_dispatch_cur_domain()이 dispatch할 request가 rqs[]에 없지만
 * kcq_map에 남아있는 것을 발견했을 때, 실제 flush를 수행하기 위해 호출하는
 * 얇은 드라이버 함수다. sbitmap_for_each_set()에 flush_busy_kcq 콜백과
 * 스택에 만든 flush_kcq_data 클로저를 넘겨, kcq_map에 세트된 모든 비트
 * (=request가 있는 모든 kcq)에 대해 자동으로 flush_busy_kcq()가 호출되게
 * 한다. 이 방식으로 O(세트된 비트 수) 시간에 non-empty kcq만 정확히
 * 방문할 수 있다(전체 hctx->nr_ctx개를 선형 스캔하지 않음). 호출자가 이미
 * khd->lock을 쥐고 있으므로 이 함수 실행 중 도메인 전환이나 재진입은
 * 없다.
 *
 * 호출 체인:
 *   kyber_dispatch_cur_domain() → [kyber_flush_busy_kcqs] →
 *   sbitmap_for_each_set() → flush_busy_kcq()
 */
static void kyber_flush_busy_kcqs(struct kyber_hctx_data *khd,
				  unsigned int sched_domain,
				  struct list_head *list)
{
	struct flush_kcq_data data = {
		.khd = khd,	/* [한국어] flush_busy_kcq가 kcqs[bitnr] 접근에 쓸 hctx 데이터 */
		.sched_domain = sched_domain,	/* [한국어] 어느 도메인의 rq_list를 옮길지 */
		.list = list,	/* [한국어] 옮겨 담을 목적지 */
	};

	/* kcq_map에 표시된 모든 non-empty ctx 큐를 순회하며 도메인별 request를 hctx 리스트로 이동 */
	sbitmap_for_each_set(&khd->kcq_map[sched_domain],
			     flush_busy_kcq, &data);
}

/*
 * [한국어]
 * kyber_domain_wake() - domain token 반환 시 대기 중인 hctx를 깨우는 wait 콜백
 *
 * @wqe: 깨어난 wait_queue_entry_t. khd->domain_wait[domain].wait와 동일 주소이며,
 *       wqe->private에 kyber_init_hctx()가 저장해 둔 hctx 포인터가 들어있다.
 * @mode: wait queue 표준 콜백 인자(태스크 상태 모드) — Kyber는 사용하지 않음.
 * @flags: wait queue 표준 콜백 인자(wake flags) — Kyber는 사용하지 않음.
 * @key: sbitmap_queue_clear()가 전달하는 wake 관련 부가 데이터 — Kyber는 사용하지 않음.
 * @return: 1 (wake가 처리되어 exclusive wake 체인을 여기서 멈춤을 의미 —
 *          default_wake_function 계열의 반환 규약).
 *
 * NVMe CQ ISR에서 request가 완료되어 rq_clear_domain_token()이
 * sbitmap_queue_clear()로 token을 반환하면, sbitmap 내부가 대기 중인 wait
 * queue 항목들을 깨우며 이 콜백을 호출한다. 이 hctx는 token 부족으로
 * dispatch를 멈추고 잠들어 있었으므로, wait 리스트에서 자신을 제거한 뒤
 * blk_mq_run_hw_queue(hctx, true)로 강제 재실행을 요청해 새로 생긴
 * token으로 dispatch를 재개하게 한다. wait_queue 프레임워크 규약상 콜백은
 * NVMe CQ ISR과 동일한 컨텍스트(softirq, 혹은 sbitmap 구현에 따라
 * 프로세스 컨텍스트)에서 실행될 수 있다.
 *
 * 호출 체인:
 *   rq_clear_domain_token() → sbitmap_queue_clear() → (내부 wake 로직) →
 *   [kyber_domain_wake] → blk_mq_run_hw_queue()
 */
static int kyber_domain_wake(wait_queue_entry_t *wqe, unsigned mode, int flags,
			     void *key)
{
	/* READ_ONCE: wqe->private가 kyber_init_hctx에서 hctx로 설정된 후 변경되지 않음을 보장 */
	struct blk_mq_hw_ctx *hctx = READ_ONCE(wqe->private);
	struct sbq_wait *wait = container_of(wqe, struct sbq_wait, wait);	/* [한국어] wait_queue_entry_t를 감싸는 상위 sbq_wait 구조체 포인터 역산(container_of) */

	sbitmap_del_wait_queue(wait);	/* [한국어] 이미 깨어났으므로 wait queue에서 자신을 제거 - 중복 wake 방지 */
	/* NVMe CQ 완료로 token이 해제되면 해당 hctx를 깨워 SQ에 새 명령을 채우도록 dispatch 유발 */
	blk_mq_run_hw_queue(hctx, true);
	return 1;	/* [한국어] wake 처리 완료를 알림 (exclusive wake 시맨틱 준수) */
}

/*
 * [한국어]
 * kyber_get_domain_token() - 현재 도메인의 domain_tokens에서 token 획득 시도
 *
 * @kqd: Kyber 전역 데이터. domain_tokens[]가 획득 대상 sbitmap_queue.
 * @khd: 이 hctx의 Kyber 데이터. domain_wait[]/domain_ws[]/wait_index[]를 사용.
 * @hctx: 현재 dispatch 중인 hctx. token을 못 얻으면 이 hctx가 wait queue에
 *        등록되어 나중에 kyber_domain_wake()가 재개시킬 대상이 된다.
 * @return: 0 이상이면 획득한 token(sbitmap 슬롯) 번호. 음수(-1)이면 이번에는
 *          token을 얻지 못했고 wait queue에 등록됨을 의미.
 *
 * token 획득은 "이제 NVMe SQ에 명령을 하나 더 넣어도 된다"는 허가에
 * 해당한다. __sbitmap_queue_get()으로 즉시 시도하고, 실패하면(도메인이
 * depth만큼 이미 가득 참) 이 hctx를 domain_tokens의 wait queue에 등록해
 * 이후 token이 반환될 때 kyber_domain_wake()로 깨어나게 만든다. 등록과
 * 재시도 사이의 경쟁(등록 전에 다른 CPU가 token을 반환한 경우)을 막기
 * 위해 등록 직후 한 번 더 획득을 시도한다. 반대로 wait queue에 있는
 * 상태에서 token을 얻은 경우, 불필요한 미래의 wake-up을 막기 위해 자신을
 * wait queue에서 제거한다. 이 모든 과정은 호출자가 이미 쥐고 있는
 * khd->lock으로 직렬화되어 동일 hctx 내에서는 동시 호출이 없다.
 *
 * 호출 체인:
 *   kyber_dispatch_cur_domain() → [kyber_get_domain_token] →
 *   __sbitmap_queue_get(), sbitmap_add_wait_queue(), sbitmap_del_wait_queue()
 */
static int kyber_get_domain_token(struct kyber_queue_data *kqd,
				  struct kyber_hctx_data *khd,
				  struct blk_mq_hw_ctx *hctx)
{
	unsigned int sched_domain = khd->cur_domain;	/* [한국어] 지금 batching 중인 도메인 - 이 도메인의 token만 시도 */
	struct sbitmap_queue *domain_tokens = &kqd->domain_tokens[sched_domain];	/* [한국어] 해당 도메인의 NVMe in-flight token pool */
	struct sbq_wait *wait = &khd->domain_wait[sched_domain];	/* [한국어] 이 hctx가 이 도메인에서 대기할 때 쓸 wait 엔트리 */
	struct sbq_wait_state *ws;	/* [한국어] wait queue 등록에 사용할 sbitmap 내부 라운드로빈 슬롯 */
	int nr;	/* [한국어] 획득 시도 결과 - 0 이상이면 슬롯 번호, 음수면 실패 */

	/* 현재 도메인의 NVMe SQ in-flight token 하나를 할당 시도 (CID/entry 진행 자리 허가) */
	nr = __sbitmap_queue_get(domain_tokens);

	/*
	 * If we failed to get a domain token, make sure the hardware queue is
	 * run when one becomes available. Note that this is serialized on
	 * khd->lock, but we still need to be careful about the waker.
	 */
	/* token이 없으면 이 hctx를 domain_tokens의 wait queue에 등록: NVMe CQ 완료 시 깨어남 */
	if (nr < 0 && list_empty_careful(&wait->wait.entry)) {	/* [한국어] 획득 실패했고 아직 wait queue에 등록돼 있지 않다면 */
		ws = sbq_wait_ptr(domain_tokens,
				  &khd->wait_index[sched_domain]);	/* [한국어] 라운드로빈으로 다음 wait 슬롯 선택 - 여러 hctx의 대기를 균등 분산 */
		khd->domain_ws[sched_domain] = ws;	/* [한국어] 나중에 wait queue에서 제거할 때 같은 슬롯을 찾을 수 있도록 저장 */
		sbitmap_add_wait_queue(domain_tokens, ws, wait);	/* [한국어] 이 hctx를 실제로 wait queue에 등록 - 이후 kyber_domain_wake 대상이 됨 */

		/*
		 * Try again in case a token was freed before we got on the wait
		 * queue.
		 */
		/* wait queue 등록 사이에 NVMe CQ ISR가 token을 반환했을 수 있으므로 재시도 */
		nr = __sbitmap_queue_get(domain_tokens);	/* [한국어] 등록 직후 즉시 재시도 - 등록 전 반환된 token을 놓치지 않기 위함(lost-wakeup 방지) */
	}

	/*
	 * If we got a token while we were on the wait queue, remove ourselves
	 * from the wait queue to ensure that all wake ups make forward
	 * progress. It's possible that the waker already deleted the entry
	 * between the !list_empty_careful() check and us grabbing the lock, but
	 * list_del_init() is okay with that.
	 */
	/* wait queue에 등록된 상태에서 token을 획득하면 불필요한 wake-up을 막기 위해 대기 항목 제거 */
	if (nr >= 0 && !list_empty_careful(&wait->wait.entry)) {	/* [한국어] 재시도로 token을 얻었는데 여전히 wait queue에 남아있다면 */
		ws = khd->domain_ws[sched_domain];	/* [한국어] 등록 시 저장해 둔 슬롯 재사용 */
		spin_lock_irq(&ws->wait.lock);	/* [한국어] wait queue 리스트 조작 보호 - IRQ 컨텍스트(NVMe CQ ISR)와의 경쟁 방지를 위해 irq 버전 사용 */
		sbitmap_del_wait_queue(wait);	/* [한국어] 이미 token을 얻었으므로 대기 목록에서 제거 - 나중에 불필요한 wake 방지 */
		spin_unlock_irq(&ws->wait.lock);
	}

	return nr;	/* [한국어] 획득한 token 번호(>=0) 또는 실패(-1)를 호출자(kyber_dispatch_cur_domain)에게 반환 */
}

/*
 * [한국어]
 * kyber_dispatch_cur_domain() - 현재 도메인에서 request 하나를 실제로 dispatch
 *
 * @kqd: Kyber 전역 데이터. domain_tokens 등에 접근하기 위해 하위 호출에 전달.
 * @khd: 이 hctx의 Kyber 데이터. rqs[]/kcq_map[]/batching 등을 다룸.
 * @hctx: 현재 dispatch 중인 hctx. token 대기 등록 시 필요.
 * @return: dispatch 대상으로 선택된 request 포인터. 대기 중인 request가
 *          없거나 token을 얻지 못하면 NULL.
 *
 * khd->cur_domain 도메인에 대해 두 가지 경로 중 하나를 취한다: (1) 이미
 * kcq에서 flush되어 khd->rqs[cur_domain]에 대기 중인 request가 있으면
 * token만 획득해 바로 반환, (2) rqs[]는 비었지만 kcq_map에 아직 request가
 * 남아있다고 표시돼 있으면, 먼저 token을 확보한 뒤에야 kcq를 flush한다
 * (token 없이 미리 flush하면 나중에 merge 기회를 잃을 수 있으므로 순서가
 * 중요 — 토큰이 없을 땐 request를 kcq에 그대로 둬서 병합 가능성을
 * 보존한다). 두 경로 모두 token을 못 얻으면 trace_kyber_throttled로
 * throttling 이벤트를 남기고 그 사실을 알린다(디버깅/모니터링용). 반환된
 * request는 곧 nvme_queue_rq() → nvme_submit_cmd()(NVMe doorbell/SQ tail
 * 갱신)로 이어진다. 호출자가 이미 khd->lock을 쥐고 있어 재진입 없음.
 *
 * 호출 체인:
 *   kyber_dispatch_request() → [kyber_dispatch_cur_domain] →
 *   kyber_get_domain_token(), kyber_flush_busy_kcqs(), rq_set_domain_token()
 */
static struct request *
kyber_dispatch_cur_domain(struct kyber_queue_data *kqd,
			  struct kyber_hctx_data *khd,
			  struct blk_mq_hw_ctx *hctx)
{
	struct list_head *rqs;	/* [한국어] 현재 도메인의 flush된(=이미 kcq에서 옮겨진) 대기 리스트 */
	struct request *rq;	/* [한국어] dispatch 대상으로 선택될 request */
	int nr;	/* [한국어] kyber_get_domain_token()의 획득 결과 (>=0: 성공한 token 번호, <0: 실패) */

	rqs = &khd->rqs[khd->cur_domain];	/* [한국어] 이번 dispatch 시도 대상이 되는 도메인별 리스트 고정 */

	/*
	 * If we already have a flushed request, then we just need to get a
	 * token for it. Otherwise, if there are pending requests in the kcqs,
	 * flush the kcqs, but only if we can get a token. If not, we should
	 * leave the requests in the kcqs so that they can be merged. Note that
	 * khd->lock serializes the flushes, so if we observed any bit set in
	 * the kcq_map, we will always get a request.
	 */
	/* 이미 kcq로부터 flush된 request가 있으면 token만 획득하여 NVMe SQ로 본격 dispatch */
	rq = list_first_entry_or_null(rqs, struct request, queuelist);	/* [한국어] rqs가 비어있으면 NULL, 아니면 맨 앞 request (list_head 자체를 건드리지 않고 조회만) */
	if (rq) {	/* [한국어] 이미 flush되어 대기 중인 request가 있는 경우 */
		nr = kyber_get_domain_token(kqd, khd, hctx);	/* [한국어] 이 request를 내보낼 NVMe in-flight token 시도 */
		if (nr >= 0) {	/* [한국어] token 획득 성공 */
			khd->batching++;	/* 현재 도메인 연속 dispatch 카운트 증가 */
			rq_set_domain_token(rq, nr);	/* request에 NVMe SQ 진행 token 기록 */
			list_del_init(&rq->queuelist);	/* [한국어] rqs 대기열에서 제거하고 자기참조로 재초기화 - 이 request는 이제 dispatch 소유로 전환 */
			return rq;	/* 반환된 rq는 nvme_queue_rq -> nvme_submit_cmd(doorbell)로 전달됨 */
		} else {	/* [한국어] token 획득 실패 - 이 도메인의 NVMe in-flight 한도가 이미 가득 참 */
			trace_kyber_throttled(kqd->dev,
					      kyber_domain_names[khd->cur_domain]);	/* [한국어] ftrace/perf에 throttle 이벤트 기록 - 관찰자가 어느 도메인이 언제 막혔는지 확인 가능 */
		}
	} else if (sbitmap_any_bit_set(&khd->kcq_map[khd->cur_domain])) {	/* [한국어] flush된 request는 없지만 kcq에 아직 남은 request가 있는 경우 */
		/* kcq에 대기 중인 request가 있으면 token 확보 후 flush하여 dispatch 준비 */
		nr = kyber_get_domain_token(kqd, khd, hctx);	/* [한국어] flush하기 전에 먼저 token부터 확보 - 토큰 없이 flush하면 merge 기회를 잃으므로 순서가 중요 */
		if (nr >= 0) {	/* [한국어] token 획득 성공 - 이제 flush해도 안전 */
			kyber_flush_busy_kcqs(khd, khd->cur_domain, rqs);	/* [한국어] kcq_map에 표시된 모든 kcq의 이 도메인 request를 rqs로 이동 */
			rq = list_first_entry(rqs, struct request, queuelist);	/* [한국어] khd->lock으로 직렬화되므로 kcq_map 비트를 봤다면 flush 후 반드시 request가 있음이 보장됨 */
			khd->batching++;	/* [한국어] 연속 dispatch 카운트 증가 */
			rq_set_domain_token(rq, nr);	/* [한국어] 획득한 token을 request에 기록 */
			list_del_init(&rq->queuelist);	/* [한국어] rqs에서 제거 - dispatch 소유로 전환 */
			return rq;	/* [한국어] nvme_queue_rq로 전달될 request 반환 */
		} else {	/* [한국어] token 획득 실패 - flush하지 않아 request들이 kcq에 남아 merge 가능성 유지 */
			trace_kyber_throttled(kqd->dev,
					      kyber_domain_names[khd->cur_domain]);	/* [한국어] throttle 이벤트 기록 */
		}
	}

	/* There were either no pending requests or no tokens. */
	return NULL;	/* [한국어] 이번 호출로는 dispatch할 것이 없음 - 호출자(kyber_dispatch_request)가 다른 도메인 시도 또는 종료 */
}

/*
 * [한국어]
 * kyber_dispatch_request() - elevator_ops.dispatch_request 콜백: 다음 request 선택
 *
 * @hctx: dispatch를 요청받은 hctx (NVMe SQ에 대응).
 * @return: dispatch할 request 포인터, 또는 더 이상 보낼 것이 없으면 NULL
 *          (NULL이면 blk-mq가 이 hctx의 dispatch 루프를 멈춘다).
 *
 * blk-mq가 hctx의 하드웨어 큐를 실행할 때마다 반복 호출하는 핵심 콜백이다.
 * 먼저 현재 batching 중인 도메인(cur_domain)이 아직 batch_size를 채우지
 * 못했다면 그 도메인에서 계속 dispatch를 시도한다 — 같은 종류의 명령을
 * 연속으로 NVMe SQ에 채워 doorbell coalescing 효율을 높이기 위함이다.
 * 그 시도가 실패하면(batch 다 채웠거나, 도메인에 요청/토큰이 없으면)
 * batching을 리셋하고 다음 도메인부터 KYBER_NUM_DOMAINS바퀴를 순회하며
 * dispatch 가능한 도메인을 찾는다 — 이 라운드로빈이 read/write/discard/
 * other 사이의 공정성을 보장하고 한 도메인의 SQ 독점을 막는다. 반환된
 * request는 곧 nvme_queue_rq() → nvme_submit_cmd(NVMe doorbell 갱신)로
 * 이어진다. khd->lock으로 이 hctx에 대한 dispatch 시도 전체를 직렬화한다.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue() → __blk_mq_run_hw_queue() →
 *   blk_mq_sched_dispatch_requests() → [kyber_dispatch_request] →
 *   kyber_dispatch_cur_domain()
 */
static struct request *kyber_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct kyber_queue_data *kqd = hctx->queue->elevator->elevator_data;	/* [한국어] 이 큐의 Kyber 전역 데이터 - domain_tokens 접근용 */
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 이 hctx의 Kyber 데이터 - cur_domain/batching/rqs 등 */
	struct request *rq;	/* [한국어] 선택된 dispatch 대상 (또는 NULL) */
	int i;	/* [한국어] 도메인 라운드로빈 순회 카운터 (최대 KYBER_NUM_DOMAINS번 시도) */

	/* 동일 NVMe SQ/hctx에서 dispatch 순서와 도메인 전환 상태를 직렬화 */
	spin_lock(&khd->lock);

	/*
	 * First, if we are still entitled to batch, try to dispatch a request
	 * from the batch.
	 */
	/* 현재 도메인의 batch_size에 도달하지 않았으면 같은 도메인을 우선 채워 NVMe SQ doorbell 횟수를 줄임 */
	if (khd->batching < kyber_batch_size[khd->cur_domain]) {	/* [한국어] 아직 이 도메인에서 더 batching할 여지가 있는지 확인 */
		rq = kyber_dispatch_cur_domain(kqd, khd, hctx);	/* [한국어] 현재 도메인에서 dispatch 시도 */
		if (rq)	/* [한국어] 성공했다면 */
			goto out;	/* [한국어] 도메인 전환 없이 바로 반환 경로로 */
	}

	/*
	 * Either,
	 * 1. We were no longer entitled to a batch.
	 * 2. The domain we were batching didn't have any requests.
	 * 3. The domain we were batching was out of tokens.
	 *
	 * Start another batch. Note that this wraps back around to the original
	 * domain if no other domains have requests or tokens.
	 */
	/* batch가 끝났거나 토큰/요청이 없으면 도메인을 순회하며 다른 NVMe 명령 유형으로 전환 */
	khd->batching = 0;	/* [한국어] 새 도메인에서 batch 카운트를 0부터 다시 시작 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {	/* [한국어] 최대 전체 도메인 수만큼만 순회 - 모든 도메인을 한 바퀴 돌면 원래 도메인으로 복귀 */
		if (khd->cur_domain == KYBER_NUM_DOMAINS - 1)	/* [한국어] 마지막 도메인(OTHER)이면 */
			khd->cur_domain = 0;	/* [한국어] 처음(READ)으로 순환 */
		else
			khd->cur_domain++;	/* [한국어] 다음 도메인으로 전진 */

		rq = kyber_dispatch_cur_domain(kqd, khd, hctx);	/* [한국어] 새로 선택된 도메인에서 dispatch 시도 */
		if (rq)	/* [한국어] 성공했다면 */
			goto out;	/* [한국어] 그 request로 반환 */
	}

	rq = NULL;	/* [한국어] 모든 도메인을 순회해도 dispatch할 것이 없음 - 이 hctx는 지금 유휴 상태이거나 전 도메인 throttle 상태 */
out:
	spin_unlock(&khd->lock);
	return rq;	/* [한국어] blk-mq에게 선택된 request(또는 NULL) 반환 - NULL이면 dispatch loop 종료 */
}

/*
 * [한국어]
 * kyber_has_work() - elevator_ops.has_work 콜백: dispatch할 작업이 남아있는지 확인
 *
 * @hctx: 확인 대상 hctx.
 * @return: true면 이 hctx에 아직 dispatch되지 않은 request가 있음(flush
 *          대기열 rqs[] 또는 kcq에 존재). false면 완전히 유휴 상태.
 *
 * blk-mq의 dispatch 평가 루틴이 "이 hctx를 다시 실행할 필요가 있는가"를
 * 판단하기 위해 호출한다. 모든 도메인에 대해 두 곳을 확인한다: (1) 이미
 * kcq에서 flush되어 khd->rqs[]에 남아있는 request, (2) 아직 kcq에 머물러
 * 있지만 kcq_map에 존재가 표시된 request. 이 함수가 false를 반환해야
 * blk-mq가 해당 hctx에 대한 불필요한 재실행을 피하고 CPU를 아낄 수 있다.
 * list_empty_careful()을 쓰는 이유는 락 없이(lockless) 호출될 수 있어
 * RCU 유사 안전성이 필요하기 때문이다.
 *
 * 호출 체인:
 *   blk-mq(dispatch 평가 루프) → [kyber_has_work] → list_empty_careful(),
 *   sbitmap_any_bit_set()
 */
static bool kyber_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 확인 대상 hctx의 Kyber 데이터 */
	int i;	/* [한국어] 도메인 순회 인덱스 */

	/* 모든 도메인을 스캔하여 NVMe SQ로 내보낼 request가 flush 대기열이나 kcq에 남아있는지 확인 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		if (!list_empty_careful(&khd->rqs[i]) ||	/* [한국어] 이미 flush된 대기열에 request가 있는지 (락 없이 안전하게 확인 가능한 버전) */
		    sbitmap_any_bit_set(&khd->kcq_map[i]))	/* [한국어] 또는 kcq에 아직 남아있는 request가 있는지 */
			return true;	/* [한국어] 하나라도 있으면 이 hctx는 아직 할 일이 있음 */
	}

	return false;	/* [한국어] 모든 도메인이 비어있음 - 이 hctx는 완전히 유휴 상태 */
}

/*
 * [한국어]
 * KYBER_LAT_SHOW_STORE(domain, name) - sysfs latency-target show/store 함수쌍 생성 매크로
 *
 * @domain: 대상 스케줄링 도메인 (KYBER_READ 또는 KYBER_WRITE — discard/other는
 *          sysfs로 노출하지 않음).
 * @name: 생성될 함수 이름과 sysfs 속성 이름에 쓰일 접미사 문자열(read/write).
 * @return: (매크로 자체는 값을 반환하지 않음 — 아래 두 함수를 정의)
 *
 * 이 매크로를 KYBER_READ/read, KYBER_WRITE/write 두 번 전개하여
 * kyber_read_lat_show()/kyber_read_lat_store()와 kyber_write_lat_show()/
 * kyber_write_lat_store() 네 함수를 생성한다. _show는 elevator_data(kqd)의
 * latency_targets[domain]을 나노초 정수 문자열로 sysfs read에 노출하고,
 * _store는 사용자가 쓴 문자열을 kstrtoull로 파싱해 반대로 반영한다.
 * 예: `echo 5000000 > /sys/block/nvme0n1/queue/iosched/read_lat_nsec`로
 * NVMe read 도메인의 목표 지연을 5ms로 즉시 변경할 수 있으며, 이 값은
 * 다음 kyber_timer_fn() 주기부터 add_latency_sample()/depth 조정의 기준이
 * 된다. 두 함수 모두 프로세스 컨텍스트(sysfs syscall)에서 실행된다.
 * discard/other가 빠진 이유는: discard는 target이 5초로 매우 커 사용자
 * 조정 필요성이 낮고, other는 애초에 latency_target 자체가 없기 때문이다.
 *
 * 호출 체인:
 *   sysfs read()/write() syscall → elevator_attr 처리 →
 *   [kyber_<name>_lat_show / kyber_<name>_lat_store] → sprintf(), kstrtoull()
 */
#define KYBER_LAT_SHOW_STORE(domain, name)				\
static ssize_t kyber_##name##_lat_show(struct elevator_queue *e,	\
				       char *page)			\
{									\
	struct kyber_queue_data *kqd = e->elevator_data;	/* [한국어] 이 큐의 Kyber 전역 데이터 획득 */ \
									\
	/* sysfs를 통해 NVMe read/write 도메인의 목표 지연 시간(nsec)을 노출 */	\
	return sprintf(page, "%llu\n", kqd->latency_targets[domain]);	\
}									\
									\
static ssize_t kyber_##name##_lat_store(struct elevator_queue *e,	\
					const char *page, size_t count)	\
{									\
	struct kyber_queue_data *kqd = e->elevator_data;	/* [한국어] 갱신 대상 Kyber 전역 데이터 획득 */ \
	unsigned long long nsec;	/* [한국어] 사용자가 쓴 새 목표 지연(나노초) 파싱 결과 저장 */ \
	int ret;	/* [한국어] kstrtoull 파싱 결과: 0=성공, 음수=errno */ \
									\
	ret = kstrtoull(page, 10, &nsec);	/* [한국어] 사용자 입력 문자열을 10진 부호없는 64비트 정수로 파싱 */ \
	if (ret)	/* [한국어] 파싱 실패(형식이 잘못된 입력 등) 시 */ \
		return ret;	/* [한국어] errno를 그대로 sysfs write() 호출자에게 반환 */ \
									\
	/* 사용자가 설정한 목표 지연으로 NVMe queue depth 조정의 기준이 변경됨 */	\
	kqd->latency_targets[domain] = nsec;				\
									\
	return count;	/* [한국어] sysfs write() 관례상 전체 바이트를 소비했음을 count 그대로 반환해 알림 */ \
}
KYBER_LAT_SHOW_STORE(KYBER_READ, read);
KYBER_LAT_SHOW_STORE(KYBER_WRITE, write);
#undef KYBER_LAT_SHOW_STORE

/* [한국어] "<op>_lat_nsec"라는 이름의 0644(rw-r--r--) sysfs 파일을 만들어
 * kyber_<op>_lat_show/_store에 연결하는 매크로. 0644이므로 root는 물론
 * 일반 사용자도 그룹/기타 read 권한으로 조회 가능하고, write는 보통 root
 * 권한이 필요하다(sysfs 표준 권한 처리에 위임). */
#define KYBER_LAT_ATTR(op) __ATTR(op##_lat_nsec, 0644, kyber_##op##_lat_show, kyber_##op##_lat_store)
static const struct elv_fs_entry kyber_sched_attrs[] = {
	KYBER_LAT_ATTR(read),	/* [한국어] /sys/block/<dev>/queue/iosched/read_lat_nsec - KYBER_READ 목표 지연 조회/설정 */
	KYBER_LAT_ATTR(write),	/* [한국어] /sys/block/<dev>/queue/iosched/write_lat_nsec - KYBER_WRITE 목표 지연 조회/설정 */
	__ATTR_NULL	/* [한국어] elv_fs_entry 배열의 끝을 나타내는 sentinel - elevator.c의 순회 코드가 이를 보고 등록을 종료 */
};
#undef KYBER_LAT_ATTR

#ifdef CONFIG_BLK_DEBUG_FS
/*
 * [한국어]
 * KYBER_DEBUGFS_DOMAIN_ATTRS(domain, name) - 도메인별 debugfs 노출 함수 5개 생성 매크로
 *
 * @domain: 대상 스케줄링 도메인 (KYBER_READ/WRITE/DISCARD/OTHER 전부 지원).
 * @name: 생성될 함수/속성 이름 접미사(read/write/discard/other).
 * @return: (매크로 자체는 값을 반환하지 않음 — 아래 함수/구조체들을 정의)
 *
 * 이 매크로가 도메인 4개 모두에 대해 전개되어(KYBER_READ/read ~
 * KYBER_OTHER/other) blk-mq debugfs(/sys/kernel/debug/block/<dev>/...) 아래
 * 다음을 노출하는 함수/구조체를 만든다:
 *   - kyber_<name>_tokens_show(data=q, m): request_queue 단위 —
 *     domain_tokens[domain]의 사용중/전체 token 수를 sbitmap_queue_show로 출력.
 *   - kyber_<name>_rqs_start/next/stop(m, ...): hctx 단위 — khd->rqs[domain]
 *     (flush되어 NVMe SQ dispatch를 기다리는 request들)을 seq_file 순회
 *     인터페이스로 노출. start/stop이 khd->lock을 잡고/풀어 순회 중 리스트
 *     변경을 막는다(__acquires/__releases는 sparse 락 검사용 애노테이션).
 *   - kyber_<name>_waiting_show(data=hctx, m): hctx 단위 — 이 도메인의
 *     domain_wait가 비어있지 않은지(=token 고갈로 대기 중인지)를 0/1로 출력.
 * 모두 debugfs read(2) syscall 컨텍스트(프로세스 컨텍스트)에서 실행되며,
 * 값을 변경하지 않는 읽기 전용(0400) 진단 인터페이스다.
 *
 * 호출 체인:
 *   debugfs read() → seq_file 인터페이스 → [kyber_<name>_tokens_show /
 *   kyber_<name>_rqs_start/next/stop / kyber_<name>_waiting_show] →
 *   sbitmap_queue_show(), seq_list_start/next(), seq_printf()
 */
#define KYBER_DEBUGFS_DOMAIN_ATTRS(domain, name)			\
static int kyber_##name##_tokens_show(void *data, struct seq_file *m)	\
{									\
	struct request_queue *q = data;	/* [한국어] debugfs 파일이 연결된 request_queue */ \
	struct kyber_queue_data *kqd = q->elevator->elevator_data;	/* [한국어] 이 큐의 Kyber 전역 데이터 획득 */ \
									\
	/* debugfs: 현재 NVMe 도메인별 사용 중/남은 in-flight token 수 출력 */	\
	sbitmap_queue_show(&kqd->domain_tokens[domain], m);		\
	return 0;	/* [한국어] seq_file show 콜백 관례상 항상 0 반환(에러는 seq_printf 실패로 별도 처리됨) */ \
}									\
									\
static void *kyber_##name##_rqs_start(struct seq_file *m, loff_t *pos)	\
	__acquires(&khd->lock)						\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;	/* [한국어] seq_file 오픈 시 등록해둔 대상 hctx */ \
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 순회 대상 rqs[domain]을 담고 있는 khd */ \
									\
	/* debugfs 열 때 hctx lock 획득: NVMe SQ 직전 dispatch 대기열 보호 */	\
	spin_lock(&khd->lock);						\
	return seq_list_start(&khd->rqs[domain], *pos);	/* [한국어] pos번째 request부터 순회 시작 위치 반환 (seq_file 표준 관례) */ \
}									\
									\
static void *kyber_##name##_rqs_next(struct seq_file *m, void *v,	\
				     loff_t *pos)			\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;	/* [한국어] 이전 호출과 동일한 hctx 재획득 */ \
	struct kyber_hctx_data *khd = hctx->sched_data;			\
									\
	/* debugfs: 다음 NVMe SQ dispatch 대기 request 순회 */		\
	return seq_list_next(v, &khd->rqs[domain], pos);	/* [한국어] 현재 원소 v의 다음 리스트 원소로 이동, pos 증가 */ \
}									\
									\
static void kyber_##name##_rqs_stop(struct seq_file *m, void *v)	\
	__releases(&khd->lock)						\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;	/* [한국어] 잠갔던 lock을 풀 대상 hctx 재획득 */ \
	struct kyber_hctx_data *khd = hctx->sched_data;			\
									\
	/* debugfs 닫을 때 hctx lock 해제 */					\
	spin_unlock(&khd->lock);					\
}									\
									\
static const struct seq_operations kyber_##name##_rqs_seq_ops = {	\
	.start	= kyber_##name##_rqs_start,	/* [한국어] 순회 시작 시 lock 획득 + 시작 위치 반환 */ \
	.next	= kyber_##name##_rqs_next,	/* [한국어] 다음 원소로 이동 */ \
	.stop	= kyber_##name##_rqs_stop,	/* [한국어] 순회 종료 시 lock 해제 */ \
	.show	= blk_mq_debugfs_rq_show,	/* [한국어] 각 request를 사람이 읽을 수 있는 형식으로 출력(blk-mq-debugfs.c 공용 헬퍼) */ \
};									\
									\
static int kyber_##name##_waiting_show(void *data, struct seq_file *m)	\
{									\
	struct blk_mq_hw_ctx *hctx = data;	/* [한국어] debugfs 파일이 연결된 hctx */ \
	struct kyber_hctx_data *khd = hctx->sched_data;			\
	wait_queue_entry_t *wait = &khd->domain_wait[domain].wait;	/* [한국어] 이 도메인의 token 대기용 wait 엔트리 */ \
									\
	/* debugfs: NVMe SQ in-flight token이 바닥나 대기 중인 hctx 여부 표시 */	\
	seq_printf(m, "%d\n", !list_empty_careful(&wait->entry));	\
	return 0;							\
}
KYBER_DEBUGFS_DOMAIN_ATTRS(KYBER_READ, read)
KYBER_DEBUGFS_DOMAIN_ATTRS(KYBER_WRITE, write)
KYBER_DEBUGFS_DOMAIN_ATTRS(KYBER_DISCARD, discard)
KYBER_DEBUGFS_DOMAIN_ATTRS(KYBER_OTHER, other)
#undef KYBER_DEBUGFS_DOMAIN_ATTRS

/*
 * [한국어]
 * kyber_cur_domain_show() - debugfs: 이 hctx가 현재 batching 중인 도메인 출력
 *
 * @data: debugfs 파일에 연결된 blk_mq_hw_ctx 포인터 (open 시 등록됨).
 * @m: seq_file 출력 버퍼. seq_printf로 결과를 씀.
 * @return: 항상 0 (seq_file show 콜백 관례).
 *
 * khd->cur_domain(정수 인덱스)을 kyber_domain_names[]로 사람이 읽을 수 있는
 * 문자열("READ"/"WRITE"/"DISCARD"/"OTHER")로 변환해 출력하는 순수 진단용
 * 함수다. 이 값을 보면 지금 이 NVMe SQ가 어떤 종류의 명령을 우선
 * batching하고 있는지 즉시 알 수 있다. debugfs read(2) 프로세스
 * 컨텍스트에서 실행되며 상태를 변경하지 않는다.
 *
 * 호출 체인:
 *   debugfs read() → seq_file → [kyber_cur_domain_show] → seq_printf()
 */
static int kyber_cur_domain_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;	/* [한국어] debugfs open 시 등록된 대상 hctx */
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 조회 대상 Kyber hctx 데이터 */

	seq_printf(m, "%s\n", kyber_domain_names[khd->cur_domain]);	/* [한국어] 현재 batching 도메인 인덱스를 문자열로 변환해 출력 */
	return 0;	/* [한국어] seq_file show 콜백 관례상 항상 0 반환 */
}

/*
 * [한국어]
 * kyber_batching_show() - debugfs: 현재 도메인에서 연속 dispatch한 개수 출력
 *
 * @data: debugfs 파일에 연결된 blk_mq_hw_ctx 포인터.
 * @m: seq_file 출력 버퍼.
 * @return: 항상 0 (seq_file show 콜백 관례).
 *
 * khd->batching 값을 그대로 출력하는 진단용 함수. 이 값과
 * kyber_batch_size[cur_domain]을 비교하면 다음 dispatch에서 도메인
 * 전환이 임박했는지 가늠할 수 있어, batching 정책이 의도대로 동작하는지
 * 디버깅할 때 유용하다. debugfs read(2) 프로세스 컨텍스트에서 실행된다.
 *
 * 호출 체인:
 *   debugfs read() → seq_file → [kyber_batching_show] → seq_printf()
 */
static int kyber_batching_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;	/* [한국어] debugfs open 시 등록된 대상 hctx */
	struct kyber_hctx_data *khd = hctx->sched_data;	/* [한국어] 조회 대상 Kyber hctx 데이터 */

	seq_printf(m, "%u\n", khd->batching);	/* [한국어] 현재 도메인 연속 dispatch 카운트를 그대로 출력 */
	return 0;	/* [한국어] seq_file show 콜백 관례상 항상 0 반환 */
}

/* [한국어] request_queue 단위 debugfs 속성 테이블(kyber_queue_debugfs_attrs[])의
 * 한 원소를 생성하는 매크로. name(read/write/discard/other)에 대해
 * "<name>_tokens"라는 읽기 전용(0400) 파일을 만들어 kyber_<name>_tokens_show를
 * 연결한다 - 이 파일을 cat하면 해당 도메인의 domain_tokens 사용 현황이 보인다. */
#define KYBER_QUEUE_DOMAIN_ATTRS(name)	\
	{#name "_tokens", 0400, kyber_##name##_tokens_show}
static const struct blk_mq_debugfs_attr kyber_queue_debugfs_attrs[] = {
	KYBER_QUEUE_DOMAIN_ATTRS(read),	/* [한국어] read_tokens 파일: KYBER_READ domain_tokens 상태 */
	KYBER_QUEUE_DOMAIN_ATTRS(write),	/* [한국어] write_tokens 파일: KYBER_WRITE domain_tokens 상태 */
	KYBER_QUEUE_DOMAIN_ATTRS(discard),	/* [한국어] discard_tokens 파일: KYBER_DISCARD domain_tokens 상태 */
	KYBER_QUEUE_DOMAIN_ATTRS(other),	/* [한국어] other_tokens 파일: KYBER_OTHER domain_tokens 상태 */
	{},	/* [한국어] 배열 종료를 나타내는 sentinel(속성 이름이 NULL) - blk_mq_debugfs_attr 순회 코드가 이를 보고 멈춤 */
};
#undef KYBER_QUEUE_DOMAIN_ATTRS

/* [한국어] hctx 단위 debugfs 속성 테이블(kyber_hctx_debugfs_attrs[])의 두 원소를
 * 한 번에 생성하는 매크로. "<name>_rqs"(seq_ops 기반 request 목록 순회 파일)와
 * "<name>_waiting"(token 대기 여부 0/1 표시 파일)을 함께 등록한다. */
#define KYBER_HCTX_DOMAIN_ATTRS(name)					\
	{#name "_rqs", 0400, .seq_ops = &kyber_##name##_rqs_seq_ops},	\
	{#name "_waiting", 0400, kyber_##name##_waiting_show}
static const struct blk_mq_debugfs_attr kyber_hctx_debugfs_attrs[] = {
	KYBER_HCTX_DOMAIN_ATTRS(read),		/* [한국어] read_rqs / read_waiting 파일 */
	KYBER_HCTX_DOMAIN_ATTRS(write),	/* [한국어] write_rqs / write_waiting 파일 */
	KYBER_HCTX_DOMAIN_ATTRS(discard),	/* [한국어] discard_rqs / discard_waiting 파일 */
	KYBER_HCTX_DOMAIN_ATTRS(other),	/* [한국어] other_rqs / other_waiting 파일 */
	{"cur_domain", 0400, kyber_cur_domain_show},	/* [한국어] 현재 batching 도메인 이름 출력 파일 */
	{"batching", 0400, kyber_batching_show},	/* [한국어] 현재 batching 카운트 출력 파일 */
	{},	/* [한국어] 배열 종료 sentinel */
};
#undef KYBER_HCTX_DOMAIN_ATTRS
#endif

/*
 * [한국어] kyber_sched - Kyber를 blk-mq elevator 프레임워크에 등록하는 서술자(descriptor).
 * elv_register()/elv_unregister()가 이 구조체를 통해 Kyber의 모든 콜백과
 * 메타데이터(이름 "kyber", sysfs/debugfs 속성)를 등록/해제한다. 사용자가
 * `echo kyber > /sys/block/<dev>/queue/scheduler`로 이 이름을 선택하면
 * elevator.c가 아래 .ops의 콜백들을 이 파일이 정의한 함수들로 연결한다.
 */
static struct elevator_type kyber_sched = {
	.ops = {
		/* NVMe 관점 콜백 흐름:
		 *   bio 제출: bio_merge -> prepare_request -> insert_requests
		 *   dispatch:  has_work -> dispatch_request -> nvme_queue_rq -> nvme_submit_cmd
		 *   완료:      nvme_irq -> completed_request -> finish_request
		 *   제한:      limit_depth(async), depth_updated
		 */
		.init_sched = kyber_init_sched,	/* [한국어] elevator 활성화 시 큐 플래그/async_depth 초기화 */
		.exit_sched = kyber_exit_sched,	/* [한국어] elevator 비활성화 시 타이머 정지 및 계정 해제 */
		.init_hctx = kyber_init_hctx,		/* NVMe SQ에 대응하는 hctx 초기화 */
		.exit_hctx = kyber_exit_hctx,		/* NVMe SQ에 대응하는 hctx 정리 */
		.alloc_sched_data = kyber_alloc_sched_data,	/* NVMe request_queue별 domain_tokens 할당 */
		.free_sched_data = kyber_free_sched_data,	/* [한국어] domain_tokens/cpu_latency/kqd 메모리 해제 */
		.limit_depth = kyber_limit_depth,	/* async 요청의 NVMe tag/CID 얕은 한도 */
		.bio_merge = kyber_bio_merge,		/* NVMe SQ doorbell 횟수 줄이는 bio merge */
		.prepare_request = kyber_prepare_request,	/* domain token 초기화(-1) */
		.insert_requests = kyber_insert_requests,	/* NVMe SQ 직전 per-ctx 대기열 삽입 */
		.finish_request = kyber_finish_request,	/* NVMe CQ 완료/재배치 시 token 반환 */
		.requeue_request = kyber_finish_request,	/* NVMe timeout/abort -> blk_mq_requeue_request -> token 반환 -> SQ in-flight 자리 회수 */
		.completed_request = kyber_completed_request,	/* NVMe CQ ISR에서 latency 기록 */
		.dispatch_request = kyber_dispatch_request,	/* NVMe SQ로 내보낼 다음 request 선택 */
		.has_work = kyber_has_work,		/* NVMe SQ를 추가로 채울 request 존재 여부 */
		.depth_updated = kyber_depth_updated,	/* async_depth 갱신 시 NVMe tag 한도 재설정 */
	},
#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = kyber_queue_debugfs_attrs,	/* [한국어] request_queue 단위 debugfs 파일들(도메인별 token 상태) */
	.hctx_debugfs_attrs = kyber_hctx_debugfs_attrs,	/* [한국어] hctx 단위 debugfs 파일들(rqs/waiting/cur_domain/batching) */
#endif
	.elevator_attrs = kyber_sched_attrs,	/* [한국어] sysfs 속성 테이블 - read_lat_nsec/write_lat_nsec 노출 */
	.elevator_name = "kyber",	/* [한국어] `echo kyber > .../scheduler`로 선택할 때 쓰는 문자열 식별자 */
	.elevator_owner = THIS_MODULE,	/* [한국어] 이 elevator_type을 소유하는 모듈 - 참조 카운트 관리(모듈 언로드 방지)에 사용 */
};

/*
 * [한국어]
 * kyber_init() - 모듈 초기화 진입점: Kyber를 blk-mq elevator로 등록
 *
 * @return: 성공 시 0. elv_register() 실패 시(중복 이름 등) 음수 errno.
 *
 * insmod/modprobe 또는 빌트인 커널 부팅 시 한 번 호출되어, kyber_sched
 * 서술자를 elevator.c의 전역 elevator_type 리스트에 등록한다. 이 등록이
 * 있어야 사용자가 특정 NVMe 블록 디바이스에서 "kyber"를 스케줄러로 선택할
 * 수 있게 된다. 프로세스 컨텍스트(모듈 로드 경로)에서 실행된다.
 *
 * 호출 체인:
 *   module_init() 매크로가 등록한 초기화 함수 → [kyber_init] →
 *   elv_register()
 */
static int __init kyber_init(void)
{
	/* Kyber 스케줄러를 blk-mq elevator 프레임워크에 등록: NVMe 장치에서 선택 가능해짐 */
	return elv_register(&kyber_sched);
}

/*
 * [한국어]
 * kyber_exit() - 모듈 종료 진입점: Kyber를 blk-mq elevator에서 해제
 *
 * @return: 없음(void).
 *
 * rmmod 등으로 모듈이 언로드될 때 호출되어, kyber_init()이 등록했던
 * elevator_type을 전역 리스트에서 제거한다. 이 시점에는 이미 이 elevator를
 * 사용 중인 큐가 없어야 하며(elevator_owner=THIS_MODULE 참조 카운트가
 * 이를 보장), 그렇지 않으면 모듈 언로드 자체가 거부된다. 프로세스
 * 컨텍스트(모듈 언로드 경로)에서 실행된다.
 *
 * 호출 체인:
 *   module_exit() 매크로가 등록한 종료 함수 → [kyber_exit] →
 *   elv_unregister()
 */
static void __exit kyber_exit(void)
{
	/* 모듈 제거 시 NVMe 장치의 elevator 옵션에서 Kyber 제거 */
	elv_unregister(&kyber_sched);
}

module_init(kyber_init);	/* [한국어] 커널/모듈 초기화 시점에 kyber_init()이 호출되도록 등록 (빌트인이면 do_initcalls, 모듈이면 insmod 시점) */
module_exit(kyber_exit);	/* [한국어] 모듈 언로드 시점에 kyber_exit()이 호출되도록 등록 (빌트인 커널에는 영향 없음) */

MODULE_AUTHOR("Omar Sandoval");	/* [한국어] 이 스케줄러의 원저자 메타데이터 - modinfo 등에 노출 */
MODULE_LICENSE("GPL");	/* [한국어] GPL 라이선스 명시 - GPL 전용 커널 심볼(EXPORT_SYMBOL_GPL) 사용 허용 조건 충족 */
MODULE_DESCRIPTION("Kyber I/O scheduler");	/* [한국어] modinfo에 표시될 한 줄 설명 */

/*
 * NVMe 관점 핵심 요약
 * ----------------------------------------------------------------------------
 *  - Kyber는 blk-mq와 NVMe 드라이버 사이에서, 도메인별로 NVMe SQ의
 *    in-flight 명령 수를 token으로 제한하는 latency 기반 queue depth 조절
 *    스케줄러이다.
 *  - 흐름: blk_mq_submit_bio -> blk_mq_get_request -> kyber_dispatch_request
 *    -> nvme_queue_rq -> nvme_submit_cmd(doorbell/SQ tail/CID 할당)
 *  - NVMe CQ 완료 시 kyber_completed_request()가 latency를 per-cpu
 *    histogram에 기록하고, 타이머가 이를 집계하여 domain_tokens의 깊이를
 *    늘리거나 줄인다.
 *  - 비동기 쓰기 폭주로부터 동기 요청을 보호하기 위해 async_depth 및
 *    domain별 batching을 사용한다.
 *  - (추정) 각 blk_mq_hw_ctx는 NVMe 컨트롤러의 한 SQ(또는 SQ 그룹)에
 *    대응하며, kcq는 동일 hctx 내에서 per-ctx로 도착 순서를 유지하면서
 *    merge/dispatch를 준비하는 버퍼 역할을 한다.
 */
