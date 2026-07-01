// SPDX-License-Identifier: GPL-2.0
/*
 * The Kyber I/O scheduler. Controls latency by throttling queue depths using
 * scalable techniques.
 *
 * Copyright (C) 2017 Facebook
 *
 * ============================================================================
 * NVMe 관점 파일 요약 (추가 주석)
 * ----------------------------------------------------------------------------
 * block/kyber-iosched.c는 Linux blk-mq(block multi-queue) 기반의 Kyber I/O
 * 스케줄러를 구현한다. NVMe SSD 관점에서 본다면, 이 파일은 커널 상위 계층에서
 * 생성된 bio/request를 NVMe 하드웨어 큐(nvme_queue/SQ/CQ)로 날리기 전에
 * 얼마나 많은 명령을 동시에 날릴지(=queue depth/SQ 진행 개수)를 조절하는
 * "출입구 관리자" 역할을 한다.
 *
 * 대략적인 흐름:
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd
 *                       (doorbell/SQ tail/CID 할당)
 *
 * Kyber는 read/write/discard/other 네 개 도메인별로 별도의 queue depth 한도
 * (domain_tokens)를 두고, 각 도메인의 완료 지연(latency)을 측정하여 queue
 * depth를 동적으로 조절함으로써 NVMe SSD가 최적의 병렬성을 유지하면서도
 * 지연 시간 목표(latency target)를 넘지 않도록 한다.
 *
 * 연관 파일: blk-mq.c/blk-mq.h(하드웨어 큐/컨텍스트 관리),
 *          elevator.c/elevator.h(엘리베이터 프레임워크 연결),
 *          drivers/nvme/host/pci.c 등에서 nvme_queue_rq가 호출된다.
 * ============================================================================
 */

#include <linux/kernel.h>
#include <linux/blkdev.h>	/* blk-mq request_queue, bio, request 정의: NVMe SQ/CQ 상위 계층 */
#include <linux/module.h>
#include <linux/sbitmap.h>	/* domain_tokens/kcq_map에 사용: NVMe in-flight CID/entry 할당/해제 */

#include <trace/events/block.h>

#include "elevator.h"		/* elevator_type 콜백: blk-mq와 NVMe 드라이버 사이 스케줄러 인터페이스 */
#include "blk.h"
#include "blk-mq.h"		/* blk_mq_hw_ctx, blk_mq_ctx: NVMe SQ 선택의 핵심 자료구조 */
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"	/* blk-mq 스케줄러 헬퍼: dispatch/insert/requeue 경로 정의 */

#define CREATE_TRACE_POINTS
#include <trace/events/kyber.h>

/*
 * Scheduling domains: the device is divided into multiple domains based on the
 * request type.
 */
enum {
	KYBER_READ,
	KYBER_WRITE,
	KYBER_DISCARD,
	KYBER_OTHER,
	KYBER_NUM_DOMAINS,
};

static const char *kyber_domain_names[] = {
	[KYBER_READ] = "READ",
	[KYBER_WRITE] = "WRITE",
	[KYBER_DISCARD] = "DISCARD",
	[KYBER_OTHER] = "OTHER",
};

enum {
	/*
	 * In order to prevent starvation of synchronous requests by a flood of
	 * asynchronous requests, we reserve 25% of requests for synchronous
	 * operations.
	 */
	KYBER_DEFAULT_ASYNC_PERCENT = 75,
};
/*
 * Maximum device-wide depth for each scheduling domain.
 *
 * Even for fast devices with lots of tags like NVMe, you can saturate the
 * device with only a fraction of the maximum possible queue depth. So, we cap
 * these to a reasonable value.
 */
static const unsigned int kyber_depth[] = {
	[KYBER_READ] = 256,	/* NVMe read SQ in-flight 상한: 256개 CID/entry 동시 진행 가능 */
	[KYBER_WRITE] = 128,	/* NVMe write SQ in-flight 상한: write amplification/flush 고려해 read보다 작게 */
	[KYBER_DISCARD] = 64,	/* NVMe DSM/TRIM SQ in-flight 상한: SSD 낶부 GC 부하 제어 */
	[KYBER_OTHER] = 16,	/* NVMe FLUSH/vendor 등 기타 명령 SQ in-flight 상한 */
};

/*
 * Default latency targets for each scheduling domain.
 */
static const u64 kyber_latency_targets[] = {
	[KYBER_READ] = 2ULL * NSEC_PER_MSEC,	/* NVMe read 목표 지연: 2ms (low-latency SSD 기준) */
	[KYBER_WRITE] = 10ULL * NSEC_PER_MSEC,	/* NVMe write 목표 지연: 10ms (flush/buffer 고려) */
	[KYBER_DISCARD] = 5ULL * NSEC_PER_SEC,	/* NVMe DSM/TRIM 목표 지연: 5s (background GC에 관대) */
};

/*
 * Batch size (number of requests we'll dispatch in a row) for each scheduling
 * domain.
 */
static const unsigned int kyber_batch_size[] = {
	[KYBER_READ] = 16,	/* read를 16개씩 연속 NVMe SQ에 채워 doorbell/coalescing 효율 극대화 */
	[KYBER_WRITE] = 8,	/* write를 8개씩 batch: flush/fsync 지연과의 균형 */
	[KYBER_DISCARD] = 1,	/* discard는 SSD GC에 영향이 크므로 한 번에 1개씩만 dispatch */
	[KYBER_OTHER] = 1,	/* FLUSH 등 기타 명령은 1개씩 처리하여 ordering 보장 */
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
enum {
	/*
	 * The width of the latency histogram buckets is
	 * 1 / (1 << KYBER_LATENCY_SHIFT) * target latency.
	 */
	KYBER_LATENCY_SHIFT = 2,
	/*
	 * The first (1 << KYBER_LATENCY_SHIFT) buckets are <= target latency,
	 * thus, "good".
	 */
	KYBER_GOOD_BUCKETS = 1 << KYBER_LATENCY_SHIFT,
	/* There are also (1 << KYBER_LATENCY_SHIFT) "bad" buckets. */
	KYBER_LATENCY_BUCKETS = 2 << KYBER_LATENCY_SHIFT,
};

/*
 * We measure both the total latency and the I/O latency (i.e., latency after
 * submitting to the device).
 */
enum {
	KYBER_TOTAL_LATENCY,
	KYBER_IO_LATENCY,
};

static const char *kyber_latency_type_names[] = {
	[KYBER_TOTAL_LATENCY] = "total",
	[KYBER_IO_LATENCY] = "I/O",
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
	struct list_head rq_list[KYBER_NUM_DOMAINS];
} ____cacheline_aligned_in_smp;

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
	dev_t dev;

	/*
	 * Each scheduling domain has a limited number of in-flight requests
	 * device-wide, limited by these tokens.
	 */
	struct sbitmap_queue domain_tokens[KYBER_NUM_DOMAINS];

	struct kyber_cpu_latency __percpu *cpu_latency;

	/* Timer for stats aggregation and adjusting domain tokens. */
	struct timer_list timer;

	unsigned int latency_buckets[KYBER_OTHER][2][KYBER_LATENCY_BUCKETS];

	unsigned long latency_timeout[KYBER_OTHER];

	int domain_p99[KYBER_OTHER];

	/* Target latencies in nanoseconds. */
	u64 latency_targets[KYBER_OTHER];
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
	struct list_head rqs[KYBER_NUM_DOMAINS];
	unsigned int cur_domain;
	unsigned int batching;
	struct kyber_ctx_queue *kcqs;
	struct sbitmap kcq_map[KYBER_NUM_DOMAINS];
	struct sbq_wait domain_wait[KYBER_NUM_DOMAINS];
	struct sbq_wait_state *domain_ws[KYBER_NUM_DOMAINS];
	atomic_t wait_index[KYBER_NUM_DOMAINS];
};

static int kyber_domain_wake(wait_queue_entry_t *wait, unsigned mode, int flags,
			     void *key);

/*
 * kyber_sched_domain()
 *   목적: request(bio)의 opcode를 보고 Kyber 도메인(read/write/discard/other)
 *         중 하나를 선택한다.
 *   호출 경로:
 *     blk_mq_submit_bio -> ... -> kyber_bio_merge / kyber_insert_requests /
 *     kyber_dispatch_request -> kyber_sched_domain
 *   NVMe 연결: opcode에 따라 NVMe 명령(NVME_CMD_READ, NVME_CMD_WRITE,
 *             NVME_CMD_DSM 등)으로 변환될 그룹을 사전 분류한다.
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
 * flush_latency_buckets()
 *   목적: 한 CPU의 per-cpu histogram bucket을 중앙 집계 버킷으로 합산한다.
 *   호출 경로: kyber_timer_fn -> flush_latency_buckets
 *   NVMe 연결: NVMe ISR에서 cpu_latency에 기록된 완료 지연을 타이머 주기로
 *             모아서 p90/p99 percentile 계산에 사용한다.
 */
static void flush_latency_buckets(struct kyber_queue_data *kqd,
				  struct kyber_cpu_latency *cpu_latency,
				  unsigned int sched_domain, unsigned int type)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];
	atomic_t *cpu_buckets = cpu_latency->buckets[sched_domain][type];
	unsigned int bucket;

	/* NVMe CQ ISR이 각 CPU에 기록한 완료 지연을 타이머 주기로 중앙 버킷에 합산 */
	for (bucket = 0; bucket < KYBER_LATENCY_BUCKETS; bucket++)
		/* atomic_xchg: per-cpu bucket을 0으로 리셋하면서 값을 원자적으로 가져옴 (NVMe ISR와 race 방지) */
		buckets[bucket] += atomic_xchg(&cpu_buckets[bucket], 0);
}

/*
 * calculate_percentile()
 *   목적: 집계된 histogram에서 주어진 percentile에 해당하는 bucket을
 *         반환한다. 샘플이 부족하면 -1을 반환.
 *   호출 경로: kyber_timer_fn -> calculate_percentile
 *   NVMe 연결: NVMe SQ/CQ를 통해 관측된 p90/p99 latency가 target보다 크면
 *             queue depth(domain_tokens)를 조정하는 근거로 사용된다.
 */
static int calculate_percentile(struct kyber_queue_data *kqd,
				unsigned int sched_domain, unsigned int type,
				unsigned int percentile)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];
	unsigned int bucket, samples = 0, percentile_samples;

	/* NVMe SQ/CQ를 통해 수집된 완료 샘플의 총 개수를 집계 (CQ entry 수 == samples) */
	for (bucket = 0; bucket < KYBER_LATENCY_BUCKETS; bucket++)
		samples += buckets[bucket];

	if (!samples)
		return -1;

	/*
	 * We do the calculation once we have 500 samples or one second passes
	 * since the first sample was recorded, whichever comes first.
	 */
	/* 첫 샘플 시점부터 1초(HZ) 후 또는 500개 NVMe CQ 완료 entry가 모일 때까지 percentile 계산 보류 */
	if (!kqd->latency_timeout[sched_domain])
		kqd->latency_timeout[sched_domain] = max(jiffies + HZ, 1UL);
	if (samples < 500 &&
	    time_is_after_jiffies(kqd->latency_timeout[sched_domain])) {
		return -1;
	}
	kqd->latency_timeout[sched_domain] = 0;

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
 * kyber_resize_domain()
 *   목적: 특정 도메인의 domain_tokens 깊이를 조정한다. 하한 1, 상한
 *         kyber_depth[sched_domain] 사이에서 clamp.
 *   호출 경로: kyber_timer_fn -> kyber_resize_domain
 *   NVMe 연결: 이 깊이가 해당 도메인의 NVMe SQ in-flight 한도로 작용한다.
 *             깊이가 줄면 NVMe doorbell 발행 빈도가 간접적으로 제한된다.
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
				   depth);
	}
}

/*
 * kyber_timer_fn()
 *   목적: 주기적으로 per-cpu latency histogram을 집계하고, p90/p99 latency에
 *         따라 각 도메인의 queue depth를 조절한다.
 *   호출 경로: 타이머 만료 시 자동 호출. kyber_completed_request에서
 *             timer_reduce()로 간격 조정.
 *   NVMe 연결:
 *     - NVMe CQ 완료 ISR이 기록한 latency를 토대로 NVMe SQ queue depth를
 *       동적으로 늘리거나 줄인다.
 *     - p90이 target을 초과하면 congestion으로 보고, latency가 양호한
 *       도메인을 스로틀한다.
 */
static void kyber_timer_fn(struct timer_list *t)
{
	struct kyber_queue_data *kqd = timer_container_of(kqd, t, timer);
	unsigned int sched_domain;
	int cpu;
	bool bad = false;

	/* Sum all of the per-cpu latency histograms. */
	/* NVMe CQ ISR가 각 CPU에 남긴 완료 지연을 온라인 CPU별로 순회하며 중앙 집계 */
	for_each_online_cpu(cpu) {
		struct kyber_cpu_latency *cpu_latency;

		cpu_latency = per_cpu_ptr(kqd->cpu_latency, cpu);
		/* read/write/discard 도메인(OTHER 제외)의 TOTAL/IO latency를 모두 flush */
		for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KYBER_TOTAL_LATENCY);
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KYBER_IO_LATENCY);
		}
	}

	/*
	 * Check if any domains have a high I/O latency, which might indicate
	 * congestion in the device. Note that we use the p90; we don't want to
	 * be too sensitive to outliers here.
	 */
	/* NVMe SSD 낶부 congestion 판단: p90 IO latency가 target을 초과하면 bad 플래그 설정 */
	for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
		int p90;

		p90 = calculate_percentile(kqd, sched_domain, KYBER_IO_LATENCY,
					   90);
		if (p90 >= KYBER_GOOD_BUCKETS)
			bad = true;
	}

	/*
	 * Adjust the scheduling domain depths. If we determined that there was
	 * congestion, we throttle all domains with good latencies. Either way,
	 * we ease up on throttling domains with bad latencies.
	 */
	/* NVMe SQ queue depth(domain_tokens)를 p99 latency 기반으로 동적으로 조절 */
	for (sched_domain = 0; sched_domain < KYBER_OTHER; sched_domain++) {
		unsigned int orig_depth, depth;
		int p99;

		p99 = calculate_percentile(kqd, sched_domain,
					   KYBER_TOTAL_LATENCY, 99);
		/*
		 * This is kind of subtle: different domains will not
		 * necessarily have enough samples to calculate the latency
		 * percentiles during the same window, so we have to remember
		 * the p99 for the next time we observe congestion; once we do,
		 * we don't want to throttle again until we get more data, so we
		 * reset it to -1.
		 */
		if (bad) {
			/* congestion 시 샘플 부족하면 직전에 저장핑 p99를 fallback으로 사용 */
			if (p99 < 0)
				p99 = kqd->domain_p99[sched_domain];
			kqd->domain_p99[sched_domain] = -1;
		} else if (p99 >= 0) {
			/* congestion이 아닐 때 p99를 보존, 다음 congestion 윈도우에서 재사용 */
			kqd->domain_p99[sched_domain] = p99;
		}
		if (p99 < 0)
			continue;

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
		if (bad || p99 >= KYBER_GOOD_BUCKETS) {
			orig_depth = kqd->domain_tokens[sched_domain].sb.depth;
			/* p99 bucket에 비례하여 NVMe SQ in-flight 한도를 조정 (shift로 target 비율 반영) */
			depth = (orig_depth * (p99 + 1)) >> KYBER_LATENCY_SHIFT;
			kyber_resize_domain(kqd, sched_domain, depth);
		}
	}
}

/*
 * kyber_queue_data_alloc()
 *   목적: request_queue 단위 Kyber 스케줄러 데이터(kqd)를 할당하고 초기화.
 *   호출 경로: kyber_alloc_sched_data -> kyber_queue_data_alloc
 *   NVMe 연결:
 *     - domain_tokens를 초기화: NVMe 도메인별 in-flight token pool 생성.
 *     - latency_targets 초기값은 kyber_latency_targets[]에서 가져옴.
 */
static struct kyber_queue_data *kyber_queue_data_alloc(struct request_queue *q)
{
	struct kyber_queue_data *kqd;
	int ret = -ENOMEM;
	int i;

	kqd = kzalloc_node(sizeof(*kqd), GFP_KERNEL, q->node);
	if (!kqd)
		goto err;

	kqd->q = q;			/* 이 blk-mq request_queue는 NVMe 드라이버의 nvme_ns->queue와 연결됨 */
	kqd->dev = disk_devt(q->disk);	/* nvme0n1 등 디스크 디바이스 번호, trace/debugfs에 사용 */

	kqd->cpu_latency = alloc_percpu_gfp(struct kyber_cpu_latency,
					    GFP_KERNEL | __GFP_ZERO);
	if (!kqd->cpu_latency)
		goto err_kqd;

	timer_setup(&kqd->timer, kyber_timer_fn, 0);

	/* read/write/discard/other 도메인별 NVMe in-flight token pool 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		WARN_ON(!kyber_depth[i]);
		WARN_ON(!kyber_batch_size[i]);
		ret = sbitmap_queue_init_node(&kqd->domain_tokens[i],
					      kyber_depth[i], -1, false,
					      GFP_KERNEL, q->node);
		if (ret) {
			while (--i >= 0)
				sbitmap_queue_free(&kqd->domain_tokens[i]);
			goto err_buckets;
		}
	}

	for (i = 0; i < KYBER_OTHER; i++) {
		kqd->domain_p99[i] = -1;	/* p99 percentile 아직 계산되지 않음을 표시 */
		kqd->latency_targets[i] = kyber_latency_targets[i];	/* read 2ms, write 10ms, discard 5s */
	}

	return kqd;

err_buckets:
	free_percpu(kqd->cpu_latency);
err_kqd:
	kfree(kqd);
err:
	return ERR_PTR(ret);
}

/*
 * kyber_depth_updated()
 *   목적: async queue depth 한도가 갱신되었을 때 blk-mq에 shallow depth를
 *         다시 설정한다.
 *   호출 경로: kyber_init_sched / depth 변경 시 -> kyber_depth_updated
 *   NVMe 연결: 비동기 쓰기 플러드가 NVMe SQ를 monopolize하지 못하도록
 *             동기 요청을 위해 공간을 확보한다.
 */
static void kyber_depth_updated(struct request_queue *q)
{
	/* NVMe tag/CID 할당 시 비동기 쓰기가 async_depth 이상으로 tag를 독점하지 못하도록 얕은 한도 설정 */
	blk_mq_set_min_shallow_depth(q, q->async_depth);
}

/*
 * kyber_init_sched()
 *   목적: elevator_queue 등록 시 request_queue 플래그 및 async depth를
 *         초기화한다.
 *   호출 경로: elevator.c(elv_iosched_store 등) -> kyber_init_sched
 *   NVMe 연결: single-queue 스케줄러가 아님을 표시. NVMe는 필수적으로
 *             blk-mq(multi-queue)를 사용한다.
 */
static int kyber_init_sched(struct request_queue *q, struct elevator_queue *eq)
{
	blk_stat_enable_accounting(q);

	/* NVMe는 multi-queue이므로 single-queue 스케줄러 플래그를 해제 */
	blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	/* 동기 요청을 위해 전체 request pool의 (100 - 75)% = 25%를 예약: NVMe SQ의 sync read 우선 보장 */
	q->async_depth = q->nr_requests * KYBER_DEFAULT_ASYNC_PERCENT / 100;
	kyber_depth_updated(q);

	return 0;
}

/*
 * kyber_alloc_sched_data()
 *   목적: elevator_ops.alloc_sched_data 콜백. kqd를 할당.
 *   호출 경로: elevator.c -> kyber_alloc_sched_data
 */
static void *kyber_alloc_sched_data(struct request_queue *q)
{
	struct kyber_queue_data *kqd;

	kqd = kyber_queue_data_alloc(q);
	if (IS_ERR(kqd))
		return NULL;

	/* 할당된 kqd는 elevator_data로 등록되어 NVMe request 생명주기 낂 동안 domain_tokens 관리에 사용 */
	return kqd;
}

/*
 * kyber_exit_sched()
 *   목적: 스케줄러 종료 시 타이머를 정리하고 latency 통계 수집을 중지.
 *   호출 경로: elevator.c -> kyber_exit_sched
 */
static void kyber_exit_sched(struct elevator_queue *e)
{
	struct kyber_queue_data *kqd = e->elevator_data;

	/* NVMe 컨트롤러 제거/스케줄러 교체 시 latency 집계 타이머를 안전히 종료 */
	timer_shutdown_sync(&kqd->timer);
	blk_stat_disable_accounting(kqd->q);
}

/*
 * kyber_free_sched_data()
 *   목적: kqd 및 도메인 토큰, per-cpu latency 메모리 해제.
 *   호출 경로: elevator.c -> kyber_free_sched_data
 */
static void kyber_free_sched_data(void *elv_data)
{
	struct kyber_queue_data *kqd = elv_data;
	int i;

	if (!kqd)
		return;

	/* read/write/discard/other NVMe in-flight token bitmap 해제 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		sbitmap_queue_free(&kqd->domain_tokens[i]);
	free_percpu(kqd->cpu_latency);
	kfree(kqd);
}

/*
 * kyber_ctx_queue_init()
 *   목적: 하나의 kcq(ctx 큐)를 초기화한다.
 *   호출 경로: kyber_init_hctx -> kyber_ctx_queue_init
 */
static void kyber_ctx_queue_init(struct kyber_ctx_queue *kcq)
{
	unsigned int i;

	/* per-ctx 큐 보호: 동일 kcq에 bio merge와 dispatch가 동시에 접근 가능 (NVMe SQ 직전 단계) */
	spin_lock_init(&kcq->lock);
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		INIT_LIST_HEAD(&kcq->rq_list[i]);
}

/*
 * kyber_init_hctx()
 *   목적: blk_mq_hw_ctx 생성 시 per-hctx Kyber 데이터(khd)를 할당/초기화.
 *   호출 경로: blk-mq -> kyber_init_hctx
 *   NVMe 연결:
 *     - 각 hctx는 NVMe 드라이버의 nvme_queue(또는 해당 hctx에 매핑된 SQ)
 *       에 대응한다. (추정)
 *     - kcq_map과 kcqs를 초기화하여 NVMe SQ로 본격 dispatch되기 전의
 *       per-ctx 대기열을 관리한다.
 */
static int kyber_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kyber_hctx_data *khd;
	int i;

	/* (추정) 이 hctx는 NVMe 컨트롤러의 한 SQ(또는 SQ 그룹)에 매핑됨 */
	khd = kmalloc_node(sizeof(*khd), GFP_KERNEL, hctx->numa_node);
	if (!khd)
		return -ENOMEM;

	khd->kcqs = kmalloc_array_node(hctx->nr_ctx,
				       sizeof(struct kyber_ctx_queue),
				       GFP_KERNEL, hctx->numa_node);
	if (!khd->kcqs)
		goto err_khd;

	/* hctx에 속한 모든 software ctx에 대해 NVMe SQ 직전 per-ctx 대기열 초기화 */
	for (i = 0; i < hctx->nr_ctx; i++)
		kyber_ctx_queue_init(&khd->kcqs[i]);

	/* 각 도메인별로 어느 kcq에 request가 있는지를 빠르게 스캔할 sbitmap 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		if (sbitmap_init_node(&khd->kcq_map[i], hctx->nr_ctx,
				      ilog2(8), GFP_KERNEL, hctx->numa_node,
				      false, false)) {
			while (--i >= 0)
				sbitmap_free(&khd->kcq_map[i]);
			goto err_kcqs;
		}
	}

	/* 동일 NVMe SQ/hctx에서 dispatch 순서와 batching 상태를 직렬화 */
	spin_lock_init(&khd->lock);

	/* domain별 dispatch 대기열(rqs)과 domain_tokens 대기(wait queue) 초기화 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		INIT_LIST_HEAD(&khd->rqs[i]);
		khd->domain_wait[i].sbq = NULL;
		init_waitqueue_func_entry(&khd->domain_wait[i].wait,
					  kyber_domain_wake);
		khd->domain_wait[i].wait.private = hctx;
		INIT_LIST_HEAD(&khd->domain_wait[i].wait.entry);
		atomic_set(&khd->wait_index[i], 0);
	}

	khd->cur_domain = 0;
	khd->batching = 0;

	hctx->sched_data = khd;

	return 0;

err_kcqs:
	kfree(khd->kcqs);
err_khd:
	kfree(khd);
	return -ENOMEM;
}

/*
 * kyber_exit_hctx()
 *   목적: hctx 해제 시 khd 메모리 정리.
 *   호출 경로: blk-mq -> kyber_exit_hctx
 */
static void kyber_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kyber_hctx_data *khd = hctx->sched_data;
	int i;

	/* NVMe SQ에 대응하는 hctx가 소멸할 때 domain별 kcq_map bitmap 해제 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++)
		sbitmap_free(&khd->kcq_map[i]);
	kfree(khd->kcqs);
	kfree(hctx->sched_data);
}

/*
 * rq_get_domain_token()
 *   목적: request의 elv.priv[0]에 저장된 domain token 번호를 읽는다.
 *   NVMe 연결: 이 token이 -1이면 아직 NVMe SQ로 dispatch되지 않은 상태거나
 *             token이 없음을 의미한다.
 */
static int rq_get_domain_token(struct request *rq)
{
	/* request->elv.priv[0]에 domain token 번호 저장: -1이면 아직 NVMe SQ로 dispatch되지 않음 */
	return (long)rq->elv.priv[0];
}

static void rq_set_domain_token(struct request *rq, int token)
{
	/* domain token을 request에 기록 -> 이제 해당 request는 NVMe SQ in-flight 한도에 포함됨 */
	rq->elv.priv[0] = (void *)(long)token;
}

/*
 * rq_clear_domain_token()
 *   목적: request 완료/종료 시 domain token을 sbitmap_queue에 반환.
 *   호출 경로: kyber_finish_request -> rq_clear_domain_token
 *   NVMe 연결:
 *     - NVMe CQ ISR에서 request 완료 시 호출되어, NVMe SQ의 in-flight
 *       자리를 비운다. 이후 kyber_domain_wake()를 통해 대기 중인 hctx를
 *       깨워 다시 dispatch하게 한다.
 */
static void rq_clear_domain_token(struct kyber_queue_data *kqd,
				  struct request *rq)
{
	unsigned int sched_domain;
	int nr;

	nr = rq_get_domain_token(rq);
	/* token이 할당된 상태에서만 반환: NVMe CQ ISR 완료 시 SQ in-flight 자리 회수 */
	if (nr != -1) {
		sched_domain = kyber_sched_domain(rq->cmd_flags);
		/* 동일 CPU에서의 sbitmap clear는 cache locality 향상 (NVMe CQ affinity 고려) */
		sbitmap_queue_clear(&kqd->domain_tokens[sched_domain], nr,
				    rq->mq_ctx->cpu);
	}
}

/*
 * kyber_limit_depth()
 *   목적: request 할당 시 비동기 요청의 shallow depth를 async_depth로 제한.
 *   호출 경로: blk_mq_get_request -> ... -> kyber_limit_depth
 *   NVMe 연결: 동기 요청이 NVMe SQ 자리(=tag/CID)를 비동기 쓰기 폭주에
 *             빼앗기지 않도록 방어한다.
 */
static void kyber_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	/* 동기 read가 아닌 요청(비동기 write 등)의 tag/CID 할당 깊이를 async_depth로 제한 */
	if (!blk_mq_is_sync_read(opf))
		data->shallow_depth = data->q->async_depth;
}

/*
 * kyber_bio_merge()
 *   목적: bio가 도착했을 때 동일 ctx/domain 리스트 내 기존 request와 merge
 *         시도.
 *   호출 경로:
 *     blk_mq_submit_bio -> blk_attempt_bio_merge -> ... -> kyber_bio_merge
 *   NVMe 연결: merge가 성공하면 NVMe SQ에 별도의 새 CID를 소모하지 않고
 *             기존 request의 PRP/SGL만 확장하여 NVMe doorbell 횟수를 줄인다.
 */
static bool kyber_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	/* bio를 제출한 CPU의 software ctx 획득: NVMe SQ 선택과 무관하게 blk-mq 낶의 소프트웨어 분류 */
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
	/* bio->bi_opf와 ctx로 hctx 선택 -> NVMe 관점에서는 이 bio가 어느 nvme_queue/SQ로 갈지의 전단계 */
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(bio->bi_opf, ctx);
	struct kyber_hctx_data *khd = hctx->sched_data;
	/* 동일 hctx 내에서 ctx->index_hw를 사용해 per-ctx 대기열 선택 (도착 순서/merge 단위 보존) */
	struct kyber_ctx_queue *kcq = &khd->kcqs[ctx->index_hw[hctx->type]];
	unsigned int sched_domain = kyber_sched_domain(bio->bi_opf);
	struct list_head *rq_list = &kcq->rq_list[sched_domain];
	bool merged;

	/* kcq lock: NVMe SQ로 가기 전 bio merge와 insert를 직렬화 */
	spin_lock(&kcq->lock);
	/* merge 성공 시 기존 request의 PRP/SGL만 확장되고 새 CID는 소모되지 않음.
	 * (DMA scatter-gather list/PRP list 확장 -> nvme_setup_cmd 시 SGL/PRP entry 추가) */
	merged = blk_bio_list_merge(hctx->queue, rq_list, bio, nr_segs);
	spin_unlock(&kcq->lock);

	return merged;
}

/*
 * kyber_prepare_request()
 *   목적: request 초기화 시 domain token을 "없음"(-1)으로 설정.
 *   호출 경로: blk_mq_get_request -> ... -> kyber_prepare_request
 */
static void kyber_prepare_request(struct request *rq)
{
	/* request 생성 시점에는 아직 NVMe SQ에 할당되지 않았으므로 domain token을 무효값(-1)으로 초기화 */
	rq_set_domain_token(rq, -1);
}

/*
 * kyber_insert_requests()
 *   목적: request를 hctx의 적절한 kcq/domain 리스트에 삽입.
 *   호출 경로:
 *     blk_mq_sched_insert_request / blk_mq_sched_insert_requests -> ...
 *     -> kyber_insert_requests
 *   NVMe 연결: 아직 NVMe SQ로 가지 않고 Kyber의 per-ctx 대기열에 머물며,
 *             merge 가능성을 유지한다. kcq_map 비트를 세트하여 나중에
 *             flush_busy_kcq()가 빠르게 찾을 수 있게 한다.
 */
static void kyber_insert_requests(struct blk_mq_hw_ctx *hctx,
				  struct list_head *rq_list,
				  blk_insert_t flags)
{
	struct kyber_hctx_data *khd = hctx->sched_data;
	struct request *rq, *next;

	/* 재배치(requeue)나 plug flush로 인해 도착한 request들을 per-ctx/domain 리스트에 삽입 */
	list_for_each_entry_safe(rq, next, rq_list, queuelist) {
		unsigned int sched_domain = kyber_sched_domain(rq->cmd_flags);
		struct kyber_ctx_queue *kcq = &khd->kcqs[rq->mq_ctx->index_hw[hctx->type]];
		struct list_head *head = &kcq->rq_list[sched_domain];

		spin_lock(&kcq->lock);
		trace_block_rq_insert(rq);
		/* BLK_MQ_INSERT_AT_HEAD: 우선순위가 높은 request를 리스트 앞에 배치 (NVMe SQ에 먼저 도달) */
		if (flags & BLK_MQ_INSERT_AT_HEAD)
			list_move(&rq->queuelist, head);
		else
			list_move_tail(&rq->queuelist, head);
		/* 이 kcq에 해당 도메인 request가 있음을 표시 -> flush_busy_kcq에서 빠르게 스캔 */
		sbitmap_set_bit(&khd->kcq_map[sched_domain],
				rq->mq_ctx->index_hw[hctx->type]);
		spin_unlock(&kcq->lock);
	}
}

/*
 * kyber_finish_request()
 *   목적: request가 완료/재배치(requeue)될 때 token을 반환.
 *   호출 경로: blk-mq -> kyber_finish_request / .requeue_request
 */
static void kyber_finish_request(struct request *rq)
{
	struct kyber_queue_data *kqd = rq->q->elevator->elevator_data;

	/* request 완료/재배치 시 domain token 반환 -> NVMe SQ in-flight 자리 해제 및 잠자는 hctx 깨움 */
	rq_clear_domain_token(kqd, rq);
}

/*
 * add_latency_sample()
 *   목적: 완료 지연을 target latency에 상대적인 bucket에 기록.
 *   호출 경로: kyber_completed_request -> add_latency_sample
 *   NVMe 연결: NVMe CQ ISR에서 request 완료 시간(now)과 request 시작 시간
 *             (start_time_ns / io_start_time_ns) 차이를 bucket에 누적.
 */
static void add_latency_sample(struct kyber_cpu_latency *cpu_latency,
			       unsigned int sched_domain, unsigned int type,
			       u64 target, u64 latency)
{
	unsigned int bucket;
	u64 divisor;

	if (latency > 0) {
		/* target latency를 1/4 단위로 나눠 NVMe 완료 시간이 target 대비 어느 구간인지 계산 */
		divisor = max_t(u64, target >> KYBER_LATENCY_SHIFT, 1);
		bucket = min_t(unsigned int, div64_u64(latency - 1, divisor),
			       KYBER_LATENCY_BUCKETS - 1);
	} else {
		bucket = 0;
	}

	/* NVMe CQ ISR와 timer_fn이 동시에 접근 가능하므로 atomic_inc로 per-cpu bucket 안전 증가 */
	atomic_inc(&cpu_latency->buckets[sched_domain][type][bucket]);
}

/*
 * kyber_completed_request()
 *   목적: request 완료 시 latency 샘플을 per-cpu histogram에 기록하고
 *         aggregation timer를 앞당긴다.
 *   호출 경로:
 *     NVMe CQ ISR -> nvme_irq -> nvme_complete_rq -> blk_mq_end_request ->
 *     ... -> kyber_completed_request
 *   NVMe 연결:
 *     - NVMe SSD가 CQ에 완료 entry를 넣으면, 그 시점(now)에서
 *       TOTAL_LATENCY(소프트웨어 큐 대기 포함)와 IO_LATENCY(디바이스에
 *       제출된 후)를 구분해 측정한다.
 *     - timer_reduce()로 100ms 내에 queue depth 재조정이 일어나도록
 *       촉발한다.
 */
static void kyber_completed_request(struct request *rq, u64 now)
{
	struct kyber_queue_data *kqd = rq->q->elevator->elevator_data;
	struct kyber_cpu_latency *cpu_latency;
	unsigned int sched_domain;
	u64 target;

	sched_domain = kyber_sched_domain(rq->cmd_flags);
	/* OTHER 도메인(NVMe FLUSH 등)은 latency target이 없어 histogram 기록 제외 */
	if (sched_domain == KYBER_OTHER)
		return;

	/* 현재 CPU의 per-cpu histogram 포인터 획득 (NVMe CQ ISR가 실행 중인 CPU) */
	cpu_latency = get_cpu_ptr(kqd->cpu_latency);
	target = kqd->latency_targets[sched_domain];
	/* TOTAL_LATENCY: bio 생성(rq->start_time_ns)부터 NVMe CQ 완료(now)까지 전체 시간 */
	add_latency_sample(cpu_latency, sched_domain, KYBER_TOTAL_LATENCY,
			   target, now - rq->start_time_ns);
	/* IO_LATENCY: NVMe SQ 제출(rq->io_start_time_ns)부터 CQ 완료까지의 디바이스 시간 */
	add_latency_sample(cpu_latency, sched_domain, KYBER_IO_LATENCY, target,
			   now - rq->io_start_time_ns);
	put_cpu_ptr(kqd->cpu_latency);

	/* 완료가 발생했으므로 100ms 내에 queue depth 재조정 타이머를 앞당김 */
	timer_reduce(&kqd->timer, jiffies + HZ / 10);
}

struct flush_kcq_data {
	struct kyber_hctx_data *khd;
	unsigned int sched_domain;
	struct list_head *list;
};

/*
 * flush_busy_kcq()
 *   목적: kcq_map에 표시된 특정 kcq에서 해당 domain의 request를 모두
 *         hctx의 rqs[]로 옮긴다.
 *   호출 경로: kyber_flush_busy_kcqs -> sbitmap_for_each_set -> flush_busy_kcq
 */
static bool flush_busy_kcq(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	struct flush_kcq_data *flush_data = data;
	/* bitnr은 ctx 인덱스: 해당 kcq에서 도메인별 request를 hctx dispatch 리스트로 이동 */
	struct kyber_ctx_queue *kcq = &flush_data->khd->kcqs[bitnr];

	spin_lock(&kcq->lock);
	/* per-ctx 대기열의 해당 도메인 request를 hctx->rqs[]로 옮김 (NVMe SQ 직전 단계) */
	list_splice_tail_init(&kcq->rq_list[flush_data->sched_domain],
			      flush_data->list);
	/* flush 완료 후 kcq_map 비트 클리어 -> 빈 kcq는 다음 스캔에서 스킵 */
	sbitmap_clear_bit(sb, bitnr);
	spin_unlock(&kcq->lock);

	return true;
}

/*
 * kyber_flush_busy_kcqs()
 *   목적: 현재 도메인에 대해 모든 non-empty kcq를 hctx rqs[]로 flush.
 *   호출 경로: kyber_dispatch_cur_domain -> kyber_flush_busy_kcqs
 */
static void kyber_flush_busy_kcqs(struct kyber_hctx_data *khd,
				  unsigned int sched_domain,
				  struct list_head *list)
{
	struct flush_kcq_data data = {
		.khd = khd,
		.sched_domain = sched_domain,
		.list = list,
	};

	/* kcq_map에 표시된 모든 non-empty ctx 큐를 순회하며 도메인별 request를 hctx 리스트로 이동 */
	sbitmap_for_each_set(&khd->kcq_map[sched_domain],
			     flush_busy_kcq, &data);
}

/*
 * kyber_domain_wake()
 *   목적: domain token이 반환될 때 대기 중인 hctx를 깨워 다시 dispatch.
 *   호출 경로: sbitmap_queue_clear -> wake_up -> kyber_domain_wake
 *   NVMe 연결: NVMe CQ 완료로 인해 token이 해제되면, 해당 hctx의
 *             blk_mq_run_hw_queue()를 호출하여 SQ에 새 명령을 채운다.
 */
static int kyber_domain_wake(wait_queue_entry_t *wqe, unsigned mode, int flags,
			     void *key)
{
	/* READ_ONCE: wqe->private가 kyber_init_hctx에서 hctx로 설정된 후 변경되지 않음을 보장 */
	struct blk_mq_hw_ctx *hctx = READ_ONCE(wqe->private);
	struct sbq_wait *wait = container_of(wqe, struct sbq_wait, wait);

	sbitmap_del_wait_queue(wait);
	/* NVMe CQ 완료로 token이 해제되면 해당 hctx를 깨워 SQ에 새 명령을 채우도록 dispatch 유발 */
	blk_mq_run_hw_queue(hctx, true);
	return 1;
}

/*
 * kyber_get_domain_token()
 *   목적: 현재 도메인(khd->cur_domain)의 domain_tokens에서 token 하나 획득.
 *         token이 없으면 wait queue에 등록.
 *   호출 경로: kyber_dispatch_cur_domain -> kyber_get_domain_token
 *   NVMe 연결:
 *     - token 획득 = "이제 NVMe SQ에 하나 더 넣어도 된다"는 허가.
 *     - token이 없으면 해당 hctx는 sleep/wait하고, NVMe CQ 완료가
 *       발생하면 kyber_domain_wake로 깨어난다. 따라서 Kyber는 NVMe
 *       SQ queue depth(domain_tokens.depth)를 초과하지 않는다.
 */
static int kyber_get_domain_token(struct kyber_queue_data *kqd,
				  struct kyber_hctx_data *khd,
				  struct blk_mq_hw_ctx *hctx)
{
	unsigned int sched_domain = khd->cur_domain;
	struct sbitmap_queue *domain_tokens = &kqd->domain_tokens[sched_domain];
	struct sbq_wait *wait = &khd->domain_wait[sched_domain];
	struct sbq_wait_state *ws;
	int nr;

	/* 현재 도메인의 NVMe SQ in-flight token 하나를 할당 시도 (CID/entry 진행 자리 허가) */
	nr = __sbitmap_queue_get(domain_tokens);

	/*
	 * If we failed to get a domain token, make sure the hardware queue is
	 * run when one becomes available. Note that this is serialized on
	 * khd->lock, but we still need to be careful about the waker.
	 */
	/* token이 없으면 이 hctx를 domain_tokens의 wait queue에 등록: NVMe CQ 완료 시 깨어남 */
	if (nr < 0 && list_empty_careful(&wait->wait.entry)) {
		ws = sbq_wait_ptr(domain_tokens,
				  &khd->wait_index[sched_domain]);
		khd->domain_ws[sched_domain] = ws;
		sbitmap_add_wait_queue(domain_tokens, ws, wait);

		/*
		 * Try again in case a token was freed before we got on the wait
		 * queue.
		 */
		/* wait queue 등록 사이에 NVMe CQ ISR가 token을 반환했을 수 있으므로 재시도 */
		nr = __sbitmap_queue_get(domain_tokens);
	}

	/*
	 * If we got a token while we were on the wait queue, remove ourselves
	 * from the wait queue to ensure that all wake ups make forward
	 * progress. It's possible that the waker already deleted the entry
	 * between the !list_empty_careful() check and us grabbing the lock, but
	 * list_del_init() is okay with that.
	 */
	/* wait queue에 등록된 상태에서 token을 획득하면 불필요한 wake-up을 막기 위해 대기 항목 제거 */
	if (nr >= 0 && !list_empty_careful(&wait->wait.entry)) {
		ws = khd->domain_ws[sched_domain];
		spin_lock_irq(&ws->wait.lock);
		sbitmap_del_wait_queue(wait);
		spin_unlock_irq(&ws->wait.lock);
	}

	return nr;
}

/*
 * kyber_dispatch_cur_domain()
 *   목적: 현재 도메인(khd->cur_domain)에서 하나의 request를 dispatch.
 *         이미 flush된 rqs[]에 요청이 있으면 token만 획득, 없으면 kcq로부터
 *         flush한 뒤 token 획득.
 *   호출 경로: kyber_dispatch_request -> kyber_dispatch_cur_domain
 *   NVMe 연결:
 *     - dispatch된 request는 곧 nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *       로 전달되어 CID 할당 및 SQ tail 업데이트가 이루어진다.
 *     - token이 없으면 trace_kyber_throttled로 throttle 이벤트를 기록하고
 *       NULL을 반환: NVMe SQ로의 추가 발행이 막힌다.
 */
static struct request *
kyber_dispatch_cur_domain(struct kyber_queue_data *kqd,
			  struct kyber_hctx_data *khd,
			  struct blk_mq_hw_ctx *hctx)
{
	struct list_head *rqs;
	struct request *rq;
	int nr;

	rqs = &khd->rqs[khd->cur_domain];

	/*
	 * If we already have a flushed request, then we just need to get a
	 * token for it. Otherwise, if there are pending requests in the kcqs,
	 * flush the kcqs, but only if we can get a token. If not, we should
	 * leave the requests in the kcqs so that they can be merged. Note that
	 * khd->lock serializes the flushes, so if we observed any bit set in
	 * the kcq_map, we will always get a request.
	 */
	/* 이미 kcq로부터 flush된 request가 있으면 token만 획득하여 NVMe SQ로 본격 dispatch */
	rq = list_first_entry_or_null(rqs, struct request, queuelist);
	if (rq) {
		nr = kyber_get_domain_token(kqd, khd, hctx);
		if (nr >= 0) {
			khd->batching++;	/* 현재 도메인 연속 dispatch 카운트 증가 */
			rq_set_domain_token(rq, nr);	/* request에 NVMe SQ 진행 token 기록 */
			list_del_init(&rq->queuelist);
			return rq;	/* 반환된 rq는 nvme_queue_rq -> nvme_submit_cmd(doorbell)로 전달됨 */
		} else {
			trace_kyber_throttled(kqd->dev,
					      kyber_domain_names[khd->cur_domain]);
		}
	} else if (sbitmap_any_bit_set(&khd->kcq_map[khd->cur_domain])) {
		/* kcq에 대기 중인 request가 있으면 token 확보 후 flush하여 dispatch 준비 */
		nr = kyber_get_domain_token(kqd, khd, hctx);
		if (nr >= 0) {
			kyber_flush_busy_kcqs(khd, khd->cur_domain, rqs);
			rq = list_first_entry(rqs, struct request, queuelist);
			khd->batching++;
			rq_set_domain_token(rq, nr);
			list_del_init(&rq->queuelist);
			return rq;
		} else {
			trace_kyber_throttled(kqd->dev,
					      kyber_domain_names[khd->cur_domain]);
		}
	}

	/* There were either no pending requests or no tokens. */
	return NULL;
}

/*
 * kyber_dispatch_request()
 *   목적: blk-mq가 hctx에서 dispatch할 다음 request를 반환.
 *   호출 경로:
 *     blk_mq_run_hw_queue -> __blk_mq_run_hw_queue ->
 *     blk_mq_sched_dispatch_requests -> kyber_dispatch_request
 *   NVMe 연결:
 *     - 반환된 request는 NVMe 드라이버의 nvme_queue_rq()로 전달되어
 *       nvme_submit_cmd(doorbell)를 거쳐 NVMe SQ에 삽입된다.
 *     - batching 정책으로 현재 도메인을 우선 채우되, batch_size에 도달하면
 *       다른 도메인으로 전환. 이는 NVMe SQ 내 read/write/discard 명령의
 *       균형을 맞추고 단일 도메인의 독점을 막는다.
 */
static struct request *kyber_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct kyber_queue_data *kqd = hctx->queue->elevator->elevator_data;
	struct kyber_hctx_data *khd = hctx->sched_data;
	struct request *rq;
	int i;

	/* 동일 NVMe SQ/hctx에서 dispatch 순서와 도메인 전환 상태를 직렬화 */
	spin_lock(&khd->lock);

	/*
	 * First, if we are still entitled to batch, try to dispatch a request
	 * from the batch.
	 */
	/* 현재 도메인의 batch_size에 도달하지 않았으면 같은 도메인을 우선 채워 NVMe SQ doorbell 횟수를 줄임 */
	if (khd->batching < kyber_batch_size[khd->cur_domain]) {
		rq = kyber_dispatch_cur_domain(kqd, khd, hctx);
		if (rq)
			goto out;
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
	khd->batching = 0;
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		if (khd->cur_domain == KYBER_NUM_DOMAINS - 1)
			khd->cur_domain = 0;
		else
			khd->cur_domain++;

		rq = kyber_dispatch_cur_domain(kqd, khd, hctx);
		if (rq)
			goto out;
	}

	rq = NULL;
out:
	spin_unlock(&khd->lock);
	return rq;
}

/*
 * kyber_has_work()
 *   목적: hctx에 아직 dispatch할 요청이 남아있는지 확인.
 *   호출 경로: blk-mq 평가 루틴 -> kyber_has_work
 *   NVMe 연결: work가 있으면 blk_mq_run_hw_queue가 다시 호출되어 NVMe SQ를
 *             채운다.
 */
static bool kyber_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct kyber_hctx_data *khd = hctx->sched_data;
	int i;

	/* 모든 도메인을 스캔하여 NVMe SQ로 별볼 request가 flush 대기열이나 kcq에 남아있는지 확인 */
	for (i = 0; i < KYBER_NUM_DOMAINS; i++) {
		if (!list_empty_careful(&khd->rqs[i]) ||
		    sbitmap_any_bit_set(&khd->kcq_map[i]))
			return true;
	}

	return false;
}

#define KYBER_LAT_SHOW_STORE(domain, name)				\
static ssize_t kyber_##name##_lat_show(struct elevator_queue *e,	\
				       char *page)			\
{									\
	struct kyber_queue_data *kqd = e->elevator_data;		\
									\
	/* sysfs를 통해 NVMe read/write 도메인의 목표 지연 시간(nsec)을 노출 */	\
	return sprintf(page, "%llu\n", kqd->latency_targets[domain]);	\
}									\
									\
static ssize_t kyber_##name##_lat_store(struct elevator_queue *e,	\
					const char *page, size_t count)	\
{									\
	struct kyber_queue_data *kqd = e->elevator_data;		\
	unsigned long long nsec;					\
	int ret;							\
									\
	ret = kstrtoull(page, 10, &nsec);				\
	if (ret)							\
		return ret;						\
									\
	/* 사용자가 설정한 목표 지연으로 NVMe queue depth 조정의 기준이 변경됨 */	\
	kqd->latency_targets[domain] = nsec;				\
									\
	return count;							\
}
KYBER_LAT_SHOW_STORE(KYBER_READ, read);
KYBER_LAT_SHOW_STORE(KYBER_WRITE, write);
#undef KYBER_LAT_SHOW_STORE

#define KYBER_LAT_ATTR(op) __ATTR(op##_lat_nsec, 0644, kyber_##op##_lat_show, kyber_##op##_lat_store)
static const struct elv_fs_entry kyber_sched_attrs[] = {
	KYBER_LAT_ATTR(read),
	KYBER_LAT_ATTR(write),
	__ATTR_NULL
};
#undef KYBER_LAT_ATTR

#ifdef CONFIG_BLK_DEBUG_FS
#define KYBER_DEBUGFS_DOMAIN_ATTRS(domain, name)			\
static int kyber_##name##_tokens_show(void *data, struct seq_file *m)	\
{									\
	struct request_queue *q = data;					\
	struct kyber_queue_data *kqd = q->elevator->elevator_data;	\
									\
	/* debugfs: 현재 NVMe 도메인별 사용 중/남은 in-flight token 수 출력 */	\
	sbitmap_queue_show(&kqd->domain_tokens[domain], m);		\
	return 0;							\
}									\
									\
static void *kyber_##name##_rqs_start(struct seq_file *m, loff_t *pos)	\
	__acquires(&khd->lock)						\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kyber_hctx_data *khd = hctx->sched_data;			\
									\
	/* debugfs 열 때 hctx lock 획득: NVMe SQ 직전 dispatch 대기열 보호 */	\
	spin_lock(&khd->lock);						\
	return seq_list_start(&khd->rqs[domain], *pos);			\
}									\
									\
static void *kyber_##name##_rqs_next(struct seq_file *m, void *v,	\
				     loff_t *pos)			\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kyber_hctx_data *khd = hctx->sched_data;			\
									\
	/* debugfs: 다음 NVMe SQ dispatch 대기 request 순회 */		\
	return seq_list_next(v, &khd->rqs[domain], pos);		\
}									\
									\
static void kyber_##name##_rqs_stop(struct seq_file *m, void *v)	\
	__releases(&khd->lock)						\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kyber_hctx_data *khd = hctx->sched_data;			\
									\
	/* debugfs 닫을 때 hctx lock 해제 */					\
	spin_unlock(&khd->lock);					\
}									\
									\
static const struct seq_operations kyber_##name##_rqs_seq_ops = {	\
	.start	= kyber_##name##_rqs_start,				\
	.next	= kyber_##name##_rqs_next,				\
	.stop	= kyber_##name##_rqs_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int kyber_##name##_waiting_show(void *data, struct seq_file *m)	\
{									\
	struct blk_mq_hw_ctx *hctx = data;				\
	struct kyber_hctx_data *khd = hctx->sched_data;			\
	wait_queue_entry_t *wait = &khd->domain_wait[domain].wait;	\
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

static int kyber_cur_domain_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;
	struct kyber_hctx_data *khd = hctx->sched_data;

	seq_printf(m, "%s\n", kyber_domain_names[khd->cur_domain]);
	return 0;
}

static int kyber_batching_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;
	struct kyber_hctx_data *khd = hctx->sched_data;

	seq_printf(m, "%u\n", khd->batching);
	return 0;
}

#define KYBER_QUEUE_DOMAIN_ATTRS(name)	\
	{#name "_tokens", 0400, kyber_##name##_tokens_show}
static const struct blk_mq_debugfs_attr kyber_queue_debugfs_attrs[] = {
	KYBER_QUEUE_DOMAIN_ATTRS(read),
	KYBER_QUEUE_DOMAIN_ATTRS(write),
	KYBER_QUEUE_DOMAIN_ATTRS(discard),
	KYBER_QUEUE_DOMAIN_ATTRS(other),
	{},
};
#undef KYBER_QUEUE_DOMAIN_ATTRS

#define KYBER_HCTX_DOMAIN_ATTRS(name)					\
	{#name "_rqs", 0400, .seq_ops = &kyber_##name##_rqs_seq_ops},	\
	{#name "_waiting", 0400, kyber_##name##_waiting_show}
static const struct blk_mq_debugfs_attr kyber_hctx_debugfs_attrs[] = {
	KYBER_HCTX_DOMAIN_ATTRS(read),
	KYBER_HCTX_DOMAIN_ATTRS(write),
	KYBER_HCTX_DOMAIN_ATTRS(discard),
	KYBER_HCTX_DOMAIN_ATTRS(other),
	{"cur_domain", 0400, kyber_cur_domain_show},
	{"batching", 0400, kyber_batching_show},
	{},
};
#undef KYBER_HCTX_DOMAIN_ATTRS
#endif

static struct elevator_type kyber_sched = {
	.ops = {
		/* NVMe 관점 콜백 흐름:
		 *   bio 제출: bio_merge -> prepare_request -> insert_requests
		 *   dispatch:  has_work -> dispatch_request -> nvme_queue_rq -> nvme_submit_cmd
		 *   완료:      nvme_irq -> completed_request -> finish_request
		 *   제한:      limit_depth(async), depth_updated
		 */
		.init_sched = kyber_init_sched,
		.exit_sched = kyber_exit_sched,
		.init_hctx = kyber_init_hctx,		/* NVMe SQ에 대응하는 hctx 초기화 */
		.exit_hctx = kyber_exit_hctx,		/* NVMe SQ에 대응하는 hctx 정리 */
		.alloc_sched_data = kyber_alloc_sched_data,	/* NVMe request_queue별 domain_tokens 할당 */
		.free_sched_data = kyber_free_sched_data,
		.limit_depth = kyber_limit_depth,	/* async 요청의 NVMe tag/CID 얕은 한도 */
		.bio_merge = kyber_bio_merge,		/* NVMe SQ doorbell 횟수 줄이는 bio merge */
		.prepare_request = kyber_prepare_request,	/* domain token 초기화(-1) */
		.insert_requests = kyber_insert_requests,	/* NVMe SQ 직전 per-ctx 대기열 삽입 */
		.finish_request = kyber_finish_request,	/* NVMe CQ 완료/재배치 시 token 반환 */
		.requeue_request = kyber_finish_request,	/* NVMe timeout/abort -> blk_mq_requeue_request -> token 반환 -> SQ in-flight 자리 회수 */
		.completed_request = kyber_completed_request,	/* NVMe CQ ISR에서 latency 기록 */
		.dispatch_request = kyber_dispatch_request,	/* NVMe SQ로 별볼 다음 request 선택 */
		.has_work = kyber_has_work,		/* NVMe SQ를 추가로 채울 request 존재 여부 */
		.depth_updated = kyber_depth_updated,	/* async_depth 갱신 시 NVMe tag 한도 재설정 */
	},
#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = kyber_queue_debugfs_attrs,
	.hctx_debugfs_attrs = kyber_hctx_debugfs_attrs,
#endif
	.elevator_attrs = kyber_sched_attrs,
	.elevator_name = "kyber",
	.elevator_owner = THIS_MODULE,
};

static int __init kyber_init(void)
{
	/* Kyber 스케줄러를 blk-mq elevator 프레임워크에 등록: NVMe 장치에서 선택 가능해짐 */
	return elv_register(&kyber_sched);
}

static void __exit kyber_exit(void)
{
	/* 모듈 제거 시 NVMe 장치의 elevator 옵션에서 Kyber 제거 */
	elv_unregister(&kyber_sched);
}

module_init(kyber_init);
module_exit(kyber_exit);

MODULE_AUTHOR("Omar Sandoval");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kyber I/O scheduler");

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
