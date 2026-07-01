// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe 관점 파일 개요:
 *
 * bio 단위 IO 지연을 cgroup(io.latency) 수준에서 제어하는 block layer
 * rq-qos 구현체. blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로의 상단에서 목표 지연을 넘기지 않도록
 * 큐 깊이(max_depth)를 조절하거나 인위 지연을 가한다.
 *
 * block/blk-rq-qos.h의 rq_qos_ops 프레임워크 기반이며, block/blk-wbt.c와
 * 비슷한 위치에서 동작한다. NVMe SQ/CQ, CID, PRP/SGL 등의 실제 전송은
 * 하위 드라이버가 담당하고, 이 파일은 bio 흐름의 평균 지연과 inflight 수를 다룬다.
 */
/*
 * Block rq-qos base io controller
 *
 * This works similar to wbt with a few exceptions
 *
 * - It's bio based, so the latency covers the whole block layer in addition to
 *   the actual io.
 * - We will throttle all IO that comes in here if we need to.
 * - We use the mean latency over the 100ms window.  This is because writes can
 *   be particularly fast, which could give us a false sense of the impact of
 *   other workloads on our protected workload.
 * - By default there's no throttling, we set the queue_depth to UINT_MAX so
 *   that we can have as many outstanding bio's as we're allowed to.  Only at
 *   throttle time do we pay attention to the actual queue depth.
 *
 * The hierarchy works like the cpu controller does, we track the latency at
 * every configured node, and each configured node has it's own independent
 * queue depth.  This means that we only care about our latency targets at the
 * peer level.  Some group at the bottom of the hierarchy isn't going to affect
 * a group at the end of some other path if we're only configred at leaf level.
 *
 * Consider the following
 *
 *                   root blkg
 *             /                     \
 *        fast (target=5ms)     slow (target=10ms)
 *         /     \                  /        \
 *       a        b          normal(15ms)   unloved
 *
 * "a" and "b" have no target, but their combined io under "fast" cannot exceed
 * an average latency of 5ms.  If it does then we will throttle the "slow"
 * group.  In the case of "normal", if it exceeds its 15ms target, we will
 * throttle "unloved", but nobody else.
 *
 * In this example "fast", "slow", and "normal" will be the only groups actually
 * accounting their io latencies.  We have to walk up the heirarchy to the root
 * on every submit and complete so we can do the appropriate stat recording and
 * adjust the queue depth of ourselves if needed.
 *
 * There are 2 ways we throttle IO.
 *
 * 1) Queue depth throttling.  As we throttle down we will adjust the maximum
 * number of IO's we're allowed to have in flight.  This starts at (u64)-1 down
 * to 1.  If the group is only ever submitting IO for itself then this is the
 * only way we throttle.
 *
 * 2) Induced delay throttling.  This is for the case that a group is generating
 * IO that has to be issued by the root cg to avoid priority inversion. So think
 * REQ_META or REQ_SWAP.  If we are already at qd == 1 and we're getting a lot of
 * work done for us on behalf of the root cg and are asked to scale down more
 * then we induce a latency at userspace return.  We accumulate the total amount
 * of time we need to be punished by doing
 *
 * total_time += min_lat_nsec - actual_io_completion
 *
 * and then at throttle time will do
 *
 * throttle_time = min(total_time, NSEC_PER_SEC)
 *
 * This induced delay will throttle back the activity that is generating the
 * root cg issued io's, wethere that's some metadata intensive operation or the
 * group is using so much memory that it is pushing us into swap.
 *
 * Copyright (C) 2018 Josef Bacik
 */
#include <linux/kernel.h> /* 커널 기본 타입/매크로. NVMe driver 공유 */
#include <linux/blk_types.h> /* bio, request op flags. REQ_SWAP/REQ_META 등 NVMe 명령 특성 판별 */
#include <linux/backing-dev.h>
#include <linux/module.h>
#include <linux/timer.h> /* blkiolatency_timer_fn용. NVMe qd 회복 타이머 */
#include <linux/memcontrol.h> /* blkcg_use_delay/add_delay. NVMe 지연을 memory.delay에 연결 */
#include <linux/sched/loadavg.h> /* calc_load()용. NVMe 지연 이동 평균 계산 */
#include <linux/sched/signal.h> /* fatal_signal_pending(). OOM kill 시 NVMe 스로틀 우회 */
#include <trace/events/block.h>
#include <linux/blk-mq.h> /* blk_mq_freeze/unfreeze_queue, hctx 관련. NVMe mq 태그 동기화 기반 */
#include "blk-rq-qos.h" /* rq_qos_ops, rq_qos_wait. NVMe bio path 상단 콜백 등록 */
#include "blk-stat.h" /* blk_rq_stat. NVMe 완료 시각 통계 */
#include "blk-cgroup.h" /* blkcg_gq, blkg_policy_data. NVMe cgroup 계층 탐색 */
#include "blk.h" /* blk_queue_rot, QUEUE_FLAG_*. NVMe queue 상태/플래그 */

#define DEFAULT_SCALE_COOKIE 1000000U
/* DEFAULT_SCALE_COOKIE: scale_cookie의 기본값. 이 값이면 NVMe SQ depth
 * 조정이 없는 상태(추정). */

static struct blkcg_policy blkcg_policy_iolatency; /* blkcg policy 등록자. NVMe 디스크마다 적용 */
struct iolatency_grp; /* 전방 선언. NVMe cgroup당 지연 제어 객체 */

struct blk_iolatency {
	struct rq_qos rqos;
	/* rq_qos: block/blk-rq-qos.h의 기반 구조체. NVMe IO path에서
	 * .throttle과 .done_bio 콜백이 호출되는 지점을 등록한다. */
	struct timer_list timer;
	/* timer: 1초(HZ) 간격으로 동작. NVMe 지연 회복이 멈춘 경우
	 * scale_cookie를 되돌려 스로틀링을 완화한다. */

	/*
	 * ->enabled is the master enable switch gating the throttling logic and
	 * inflight tracking. The number of cgroups which have iolat enabled is
	 * tracked in ->enable_cnt, and ->enable is flipped on/off accordingly
	 * from ->enable_work with the request_queue frozen. For details, See
	 * blkiolatency_enable_work_fn().
	 */
	bool enabled;
	atomic_t enable_cnt;
	struct work_struct enable_work;
	/* enabled: NVMe IO 지연 추적 및 스로틀 마스터 스위치. enable_cnt가
	 * 0이면 관련 통계 수집을 끈다(추정). enable_work는 queue freeze를 통해
	 * race 없이 토글한다. */
};

static inline struct blk_iolatency *BLKIOLATENCY(struct rq_qos *rqos)
{
	return container_of(rqos, struct blk_iolatency, rqos);
	/* rq_qos 포인터에서 blk_iolatency 추출. NVMe queue당 하나의 iolatency 인스턴스 매핑 */
}

struct child_latency_info {
	spinlock_t lock;
	/* lock: 부모 cgroup이 여러 자식 iolatency_grp의 scale_cookie를
	 * 조정할 때 동기화. NVMe queue depth 변경 결정의 직렬화에 해당. */

	/* Last time we adjusted the scale of everybody. */
	u64 last_scale_event;
	/* last_scale_event: 마지막 qd scale 조정 시각. NVMe SQ가 너무 짧아지거나
	 * 길어지는 변화를 제어하는 디바운스 타이머 역할. */

	/* The latency that we missed. */
	u64 scale_lat;
	/* scale_lat: 현재 기준이 된 지연 목표값. NVMe 장치의 목표 평균 응답
	 * 시간을 초과했는지 판단하는 threshold. */

	/* Total io's from all of our children for the last summation. */
	u64 nr_samples;
	/* nr_samples: 모든 자식 cgroup에서 집계한 최근 IO 샘플 수. NVMe IO
	 * 완료 이벤트(CQ 항목) 수를 상위에서 집계한 값(추정). */

	/* The guy who actually changed the latency numbers. */
	struct iolatency_grp *scale_grp;
	/* scale_grp: 지연 목표를 위반해 scale down을 유발한 자식 그룹. NVMe
	 * queue depth 감소의 원인이 된 cgroup을 가리킨다. */

	/* Cookie to tell if we need to scale up or down. */
	atomic_t scale_cookie;
	/* scale_cookie: 전역/로컬 qd 조정 방향을 나타내는 쿠키. DEFAULT보다
	 * 작으면 NVMe SQ depth를 줄이고, 커지면 확대 방향으로 자식들에게 전파. */
};

struct percentile_stats {
	u64 total;
	/* total: NVMe 완료(CQ)로 집계된 전체 IO 개수. */
	u64 missed;
	/* missed: min_lat_nsec를 초과한 NVMe IO 완료 개수. */
};

struct latency_stat {
/* latency_stat: NVMe SSD와 HDD를 위한 통계 union. */
	union {
		struct percentile_stats ps;
		/* percentile_stats: NVMe SSD 등 비회전식 미디어용. 목표 지연을
		 * 넘은 비율(missed/total)로 판단. */
		struct blk_rq_stat rqs;
		/* blk_rq_stat: HDD 등 회전식 미디어용. 평균/분산 등의 전통 통계. */
	};
};

struct iolatency_grp {
	struct blkg_policy_data pd;
	/* pd: blkcg policy data. blkcg_gq와 이 iolatency_grp을 연결. */
	struct latency_stat __percpu *stats;
	/* stats: per-CPU IO 지연 통계. NVMe 완료 인터럽트가 도달하는 CPU별로
	 * CQ 완료 시각을 누적(추정). */
	struct latency_stat cur_stat;
	/* cur_stat: 현재 윈도우 동안 누적 중인 통계. NVMe 100ms~1s 윈도우의
	 * 지연 샘플 집계 버퍼. */
	struct blk_iolatency *blkiolat;
	/* blkiolat: 상위 blk_iolatency. 디스크/큐 전체 설정과 타이머 참조. */
	unsigned int max_depth;
	/* max_depth: 이 cgroup의 최대 inflight IO 수. NVMe SQ(submission queue)
	 * 깊이에 대응하며, 스로틀 시 줄어든다. */
	struct rq_wait rq_wait;
	/* rq_wait: max_depth 초과 시 대기할 wait queue. NVMe SQ가 꽉 찼을 때
	 * bio를 block하는 동기화 객체. */
	atomic64_t window_start;
	/* window_start: 현재 100ms~1s 통계 윈도우 시작 시각. NVMe IO 완료
	 * 시점이 윈도우를 넘어가면 iolatency_check_latencies()가 발동(추정). */
	atomic_t scale_cookie;
	/* scale_cookie: 부모 child_latency_info의 scale_cookie 사본. 부모가
	 * NVMe qd를 조정하면 이 값을 따라간다. */
	u64 min_lat_nsec;
	/* min_lat_nsec: 이 cgroup이 목표로 하는 IO 완료 지연(nsec). NVMe 평균
	 * 응답 시간 SLO에 해당. */
	u64 cur_win_nsec;
	/* cur_win_nsec: 현재 통계 윈도우 크기. min_lat_nsec에 비례하며,
	 * NVMe SSD의 빠른 IO에 맞춰 최소 100ms로 클램프. */

	/* total running average of our io latency. */
	u64 lat_avg;
	/* lat_avg: calc_load() 기반 장기 평균 지연. HDD 모드에서 NVMe IO 트렌드
	 * 를 부드럽게 추적. */

	/* Our current number of IO's for the last summation. */
	u64 nr_samples;
	/* nr_samples: 최근 윈도우의 샘플 수. 부모가 전체 자식 중 이 cgroup의
	 * 기여도를 판단할 때 사용. */

	bool ssd;
	/* ssd: 비회전식 미디어(SSD/NVMe) 여부. true면 percentile(miss 비율)
	 * 방식 사용, false면 평균 방식 사용. */
	struct child_latency_info child_lat;
	/* child_lat: 자식 cgroup들의 지연/scale 상태. NVMe qd 조정의 전파
	 * 메커니즘. */
};

#define BLKIOLATENCY_MIN_WIN_SIZE (100 * NSEC_PER_MSEC)
/* NVMe SSD는 IO가 매우 빨라 100ms 미만 윈도우는 의미 없는 샘플이 될 수 있다. */
#define BLKIOLATENCY_MAX_WIN_SIZE NSEC_PER_SEC
/* 1초를 넘는 윈도우는 지연 회복이 너무 느려지므로 상한. */
/*
 * These are the constants used to fake the fixed-point moving average
 * calculation just like load average.  The call to calc_load() folds
 * (FIXED_1 (2048) - exp_factor) * new_sample into lat_avg.  The sampling
 * window size is bucketed to try to approximately calculate average
 * latency such that 1/exp (decay rate) is [1 min, 2.5 min) when windows
 * elapse immediately.  Note, windows only elapse with IO activity.  Idle
 * periods extend the most recent window.
 */
#define BLKIOLATENCY_NR_EXP_FACTORS 5
/* calc_load()용 고정 소수점 decay 계수. NVMe 평균 지연의 급격한 변화를
 * 완화하기 위한 지수 이동 평균(추정). */
#define BLKIOLATENCY_EXP_BUCKET_SIZE (BLKIOLATENCY_MAX_WIN_SIZE / \
				      (BLKIOLATENCY_NR_EXP_FACTORS - 1))
static const u64 iolatency_exp_factors[BLKIOLATENCY_NR_EXP_FACTORS] = {
	2045, // exp(1/600) - 600 samples
	2039, // exp(1/240) - 240 samples
	2031, // exp(1/120) - 120 samples
	2023, // exp(1/80)  - 80 samples
	2014, // exp(1/60)  - 60 samples
};

static inline struct iolatency_grp *pd_to_lat(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct iolatency_grp, pd) : NULL;
	/* pd가 NULL이면 NVMe iolatency policy가 아직 초기화되지 않은 blkg(추정) */
}

static inline struct iolatency_grp *blkg_to_lat(struct blkcg_gq *blkg)
{
	return pd_to_lat(blkg_to_pd(blkg, &blkcg_policy_iolatency));
	/* blkcg_gq -> blkg_policy_data -> iolatency_grp 매핑. NVMe queue 내
	 * cgroup별 제어 상태 접근 */
}

static inline struct blkcg_gq *lat_to_blkg(struct iolatency_grp *iolat)
{
	return pd_to_blkg(&iolat->pd);
	/* iolatency_grp -> blkcg_gq 역변환. 부모/자식 cgroup 계층 탐색 시 사용 */
}

/*
 * latency_stat_init - per-CPU 또는 임시 latency_stat 초기화.
 *
 * NVMe SSD(ssd==true)일 경우 missed/total 카운터를 0으로 초기화하고,
 * 그 외에는 blk_rq_stat_init()로 평균 통계를 초기화한다.
 * blkcg_iolatency_done_bio -> iolatency_record_time -> latency_stat_record_time
 * 에서 완료 시점마다 호출되기 전에 통계 구조체를 깨끗이 만든다.
 */
static inline void latency_stat_init(struct iolatency_grp *iolat,
				     struct latency_stat *stat)
{
	if (iolat->ssd) {
		stat->ps.total = 0;
		/* NVMe SSD: 완료된 CQ 항목 수를 0으로 리셋 */
		stat->ps.missed = 0;
		/* NVMe SSD: 목표 지연을 초과한 CQ 항목 수를 0으로 리셋 */
	} else
		blk_rq_stat_init(&stat->rqs);
		/* HDD: 평균/분산 통계 버퍼 초기화. NVMe path에서는 사용 안 함 */
}

/*
 * latency_stat_sum - 두 latency_stat 구조체를 합산.
 *
 * NVMe SSD는 missed/total 누적, 나머지는 blk_rq_stat_sum(). 다중 CPU에서
 * 수집된 NVMe CQ 완료 샘플을 하나의 윈도우 집계로 병합할 때 사용.
 */
static inline void latency_stat_sum(struct iolatency_grp *iolat,
				    struct latency_stat *sum,
				    struct latency_stat *stat)
{
	if (iolat->ssd) {
		sum->ps.total += stat->ps.total;
		/* NVMe: 다중 CPU의 CQ 완료 횟수 누적. CID/tag별 완료 이벤트 집계(추정) */
		sum->ps.missed += stat->ps.missed;
		/* NVMe: 다중 CPU에서 min_lat_nsec 초과 완료 수 누적 */
	} else
		blk_rq_stat_sum(&sum->rqs, &stat->rqs);
		/* HDD: 평균 통계 병합. NVMe SSD path와는 무관 */
}

/*
 * latency_stat_record_time - 한 개의 IO 완료에 대한 지연 기록.
 *
 * req_time이 min_lat_nsec 이상이면 NVMe SSD에서 missed++.
 * 이는 NVMe SQ에 제출된 명령(CID)이 완료되어 CQ entry가 소비된 시점의
 * 지연을 기록하는 것과 동일하다(추정).
 */
static inline void latency_stat_record_time(struct iolatency_grp *iolat,
					    u64 req_time)
{
	struct latency_stat *stat = get_cpu_ptr(iolat->stats);
	/* 현재 CPU의 per-CPU 통계 버퍼 획득. NVMe CQ 완료 인터럽트가 도달한 CPU와
	 * 일치하도록 get_cpu_ptr()로 고정(추정) */
	if (iolat->ssd) {
		if (req_time >= iolat->min_lat_nsec)
			stat->ps.missed++;
		/* NVMe SSD: 목표 지연 SLO 초과 시 miss 카운터 증가. 이 비율이
		 * qd scale down trigger가 됨 */
		stat->ps.total++;
		/* NVMe SSD: 총 완료(CQ 항목) 카운터 증가. total이 0이면
		 * latency_sum_ok()에서 thresh=1로 보정됨 */
	} else
		blk_rq_stat_add(&stat->rqs, req_time);
		/* HDD: 평균/분산에 req_time 샘플 추가. NVMe path와는 무관 */
	put_cpu_ptr(stat);
	/* per-CPU 버퍼 해제. 완료 경로의 CPU 마이그레이션 방지를 위한 배리어
	 * 의미 포함(추정) */
}

/*
 * latency_sum_ok - 현재 통계가 목표 지연을 만족하는지 확인.
 *
 * NVMe SSD: missed < total/10(최소 1)이면 OK.
 * HDD: rqs.mean <= min_lat_nsec이면 OK.
 * iolatency_check_latencies()에서 qd scale up/down 여부 판단의 기준.
 */
static inline bool latency_sum_ok(struct iolatency_grp *iolat,
				  struct latency_stat *stat)
{
	if (iolat->ssd) {
		u64 thresh = div64_u64(stat->ps.total, 10);
		/* NVMe: total의 10%를 miss 허용 threshold로 계산. SLO 위반율이
		 * 10% 미만이면 OK(추정) */
		thresh = max(thresh, 1ULL);
		/* total이 0~9이면 threshold를 1로 강제. NVMe IO가 아주 적을 때도
		 * 최소 한 개 miss는 scale down 유발 가능 */
		return stat->ps.missed < thresh;
		/* NVMe: missed가 threshold 미만이면 SQ depth를 유지/확대,
		 * 이상이면 scale down */
	}
	return stat->rqs.mean <= iolat->min_lat_nsec;
	/* HDD: 평균 지연이 목표 이하이면 OK. NVMe SSD는 percentile 방식 사용 */
}

/*
 * latency_stat_samples - 사용 중인 샘플 개수 반환.
 *
 * NVMe SSD는 total, HDD는 rqs.nr_samples.
 */
static inline u64 latency_stat_samples(struct iolatency_grp *iolat,
				       struct latency_stat *stat)
{
	if (iolat->ssd)
		return stat->ps.total;
		/* NVMe SSD: 완료된 CQ 항목 수를 샘플 수로 사용 */
	return stat->rqs.nr_samples;
	/* HDD: 통계에 저장된 샘플 수. NVMe path와 무관 */
}

/*
 * iolat_update_total_lat_avg - HDD 모드에서 장기 지연 평균 갱신.
 *
 * calc_load()를 사용해 IO 빈도에 따라 decay 계수를 선택.
 * NVMe SSD는 percentile 방식이라 이 함수는 동작하지 않는다.
 */
static inline void iolat_update_total_lat_avg(struct iolatency_grp *iolat,
					      struct latency_stat *stat)
{
	int exp_idx;

	if (iolat->ssd)
		return;
		/* NVMe SSD는 percentile 방식이므로 장기 평균 갱신 불필요 */

	/*
	 * calc_load() takes in a number stored in fixed point representation.
	 * Because we are using this for IO time in ns, the values stored
	 * are significantly larger than the FIXED_1 denominator (2048).
	 * Therefore, rounding errors in the calculation are negligible and
	 * can be ignored.
	 */
	exp_idx = min_t(int, BLKIOLATENCY_NR_EXP_FACTORS - 1,
			div64_u64(iolat->cur_win_nsec,
				  BLKIOLATENCY_EXP_BUCKET_SIZE));
	/* 윈도우 크기에 따른 decay 계수 선택. NVMe라면 cur_win_nsec이 작아
	 * 빠른 decay 계수가 선택될 수 있으나, ssd==true이면 도달하지 않음(추정) */
	iolat->lat_avg = calc_load(iolat->lat_avg,
				   iolatency_exp_factors[exp_idx],
				   stat->rqs.mean);
	/* HDD: load average와 유사한 지수 이동 평균으로 IO 트렌드 추적.
	 * NVMe SSD는 missed/total로 직접 판단 */
}

/*
 * iolat_cleanup_cb - rq_qos_wait() 대기가 취소될 때 inflight 복원.
 *
 * NVMe SQ에 진입하지 못하고 대기하던 bio가 abort될 때 카운트를 되돌린다.
 */
static void iolat_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	atomic_dec(&rqw->inflight);
	/* 대기 중 abort된 bio가 차지하던 NVMe SQ 슬롯(inflight) 반환. CID/tag
	 * 할당 전 취소된 경우에 해당(추정) */
	wake_up(&rqw->wait);
	/* 대기 중인 다른 bio에게 NVMe SQ 여유가 생겼음을 알림. hctx dispatch
	 * 병렬성과 연결(추정) */
}

/*
 * iolat_acquire_inflight - max_depth(SQ depth) 여유가 있으면 inflight 획득.
 *
 * rq_wait_inc_below()가 true면 bio가 NVMe submission path로 진행 가능.
 */
static bool iolat_acquire_inflight(struct rq_wait *rqw, void *private_data)
{
	struct iolatency_grp *iolat = private_data;
	return rq_wait_inc_below(rqw, iolat->max_depth);
	/* atomic_cmpxchg() 기반으로 inflight < max_depth이면 슬롯 획득.
	 * NVMe SQ가 꽉 찼으면 false를 반환해 bio를 rq_wait 큐에 대기시킴(추정) */
}

/*
 * __blkcg_iolatency_throttle - 실제 cgroup별 스로틀링.
 *
 * blkcg_iolatency_throttle -> __blkcg_iolatency_throttle.
 * issue_as_root(REQ_META/REQ_SWAP 등)나 OOM kill 시그널이면 NVMe SQ 진입을
 * 막지 않고 통과. 그 외에는 rq_qos_wait()로 max_depth가 여유 있을 때까지
 * 대기. use_memdelay가 true면 memory.delay cgroup 계정에 지연을 예약.
 */
static void __blkcg_iolatency_throttle(struct rq_qos *rqos,
				       struct iolatency_grp *iolat,
				       bool issue_as_root,
				       bool use_memdelay)
{
	struct rq_wait *rqw = &iolat->rq_wait;
	unsigned use_delay = atomic_read(&lat_to_blkg(iolat)->use_delay);
	/* blkcg의 현재 지연 누적량 확인. NVMe qd=1에서 추가 scale down 시
	 * 사용자 공간 지연을 예약하기 위한 값(추정) */

	if (use_delay)
		blkcg_schedule_throttle(rqos->disk, use_memdelay);
		/* memory.delay를 예약. NVMe SQ depth를 더 줄일 수 없을 때 IO
		 * submitter를 직접 지연시키는 경로(추정) */

	/*
	 * To avoid priority inversions we want to just take a slot if we are
	 * issuing as root.  If we're being killed off there's no point in
	 * delaying things, we may have been killed by OOM so throttling may
	 * make recovery take even longer, so just let the IO's through so the
	 * task can go away.
	 */
	if (issue_as_root || fatal_signal_pending(current)) {
		atomic_inc(&rqw->inflight);
		/* root 발행이나 OOM kill 시 NVMe SQ 슬롯을 강제로 차지. CID/tag
		 * 할당은 하위 nvme_queue_rq에서 진행(추정) */
		return;
	}

	rq_qos_wait(rqw, iolat, iolat_acquire_inflight, iolat_cleanup_cb);
	/* max_depth 여유가 생길까지 대기. 이 대기가 길어지면 NVMe doorbell
	 * 발행 시점이 지연되어 평균 지연 목표 달성(추정) */
}

#define SCALE_DOWN_FACTOR 2
/* NVMe qd scale down 시 오른쪽 시프트 비트 수. 빠르게 깊이를 줄이기 위한 값 */
#define SCALE_UP_FACTOR 4
/* NVMe qd scale up 시 오른쪽 시프트 비트 수. 천천히 깊이를 회복하기 위한 값 */

/*
 * scale_amount - qd scale 한 단계 크기 계산.
 *
 * qd >> SCALE_UP_FACTOR(4) 또는 qd >> SCALE_DOWN_FACTOR(2) 중 큰 값.
 * NVMe SQ depth를 빠르게 줄이고 천천히 늘리도록 비대칭 설계.
 */
static inline unsigned long scale_amount(unsigned long qd, bool up)
{
	return max(up ? qd >> SCALE_UP_FACTOR : qd >> SCALE_DOWN_FACTOR, 1UL);
	// up: qd/16 만큼 늘리고, down: qd/4 만큼 줄인다. 최소 1 유지.
	/* NVMe SQ depth 변화폭. scale down은 1/4로 가파르게, scale up은 1/16으로
	 * 완만하게 조정해 진동 방지(추정) */
}

/*
 * We scale the qd down faster than we scale up, so we need to use this helper
 * to adjust the scale_cookie accordingly so we don't prematurely get
 * scale_cookie at DEFAULT_SCALE_COOKIE and unthrottle too much.
 *
 * Each group has their own local copy of the last scale cookie they saw, so if
 * the global scale cookie goes up or down they know which way they need to go
 * based on their last knowledge of it.
 */
/*
 * scale_cookie_change - 부모 child_latency_info의 scale_cookie 갱신.
 *
 * 전체 큐의 nr_requests를 기준으로 qd 조정폭(scale)을 계산.
 * DEFAULT_SCALE_COOKIE보다 작아지면 NVMe SQ depth 축소 방향, 커지면
 * 확대 방향으로 자식들에게 전파.
 */
static void scale_cookie_change(struct blk_iolatency *blkiolat,
				struct child_latency_info *lat_info,
				bool up)
{
	unsigned long qd = blkiolat->rqos.disk->queue->nr_requests;
	/* 큐 전체 nr_requests를 기준 NVMe SQ depth 상한으로 삼음. 실제 NVMe
	 * 드라이버의 queue depth는 이 값보다 작을 수 있음(추정) */
	unsigned long scale = scale_amount(qd, up);
	unsigned long old = atomic_read(&lat_info->scale_cookie);
	unsigned long max_scale = qd << 1;
	// qd의 2배를 최대 스로틀 깊이로 제한(추정).
	unsigned long diff = 0;

	if (old < DEFAULT_SCALE_COOKIE)
		diff = DEFAULT_SCALE_COOKIE - old;
	// 현재 쿠키가 기본보다 낮을 때만 회복해야 할 양을 계산.
	/* scale_cookie가 DEFAULT 미만이면 현재 스로틀 중인 양(diff) 계산.
	 * NVMe SQ depth가 얼마나 줄어있는지를 나타냄(추정) */

	if (up) {
		if (scale + old > DEFAULT_SCALE_COOKIE)
			atomic_set(&lat_info->scale_cookie,
				   DEFAULT_SCALE_COOKIE);
		/* scale up으로 기본 쿠키를 넘어서면 DEFAULT로 클램프. NVMe qd
		 * 제한 해제 상태로 복귀(추정) */
		else if (diff > qd)
			atomic_inc(&lat_info->scale_cookie);
		/* 스로틀 양이 qd보다 크면 천천히 1씩 회복. 너무 급격한 NVMe SQ
		 * depth 확대를 억제 */
		else
			atomic_add(scale, &lat_info->scale_cookie);
		/* scale 만큼 쿠키 증가. 자식 check_scale_change()에서 max_depth
		 * 확대로 반영(추정) */
	} else {
		// up: 쿠키가 DEFAULT에 도달하면 NVMe qd 축소 해제.
		/*
		 * We don't want to dig a hole so deep that it takes us hours to
		 * dig out of it.  Just enough that we don't throttle/unthrottle
		 * with jagged workloads but can still unthrottle once pressure
		 * has sufficiently dissipated.
		 */
		// down: 너무 깊은 구멍을 파지 않도록 max_scale로 제한.
		if (diff > qd) {
			if (diff < max_scale)
				atomic_dec(&lat_info->scale_cookie);
			/* diff가 max_scale 미만일 때만 1씩 추가 축소. NVMe SQ
			 * depth를 극단적으로 줄이지 않도록 제한(추정) */
		} else {
			atomic_sub(scale, &lat_info->scale_cookie);
			/* scale 만큼 쿠키 감소. 자식들의 max_depth가 scale_change()
			 * 통해 줄어듦(추정) */
		}
	}
}

/*
 * Change the queue depth of the iolatency_grp.  We add 1/16th of the
 * queue depth at a time so we don't get wild swings and hopefully dial in to
 * fairer distribution of the overall queue depth.  We halve the queue depth
 * at a time so we can scale down queue depth quickly from default unlimited
 * to target.
 */
/*
 * scale_change - iolatency_grp의 max_depth 직접 조정.
 *
 * scale up: max_depth에 scale을 더하고 qd 상한으로 클램프, 대기 큐 깨움.
 * scale down: max_depth를 절반으로 줄이고 1로 플로어.
 * NVMe SQ depth가 1까지 줄면 blkcg_use_delay()로 추가 지연 유발.
 */
static void scale_change(struct iolatency_grp *iolat, bool up)
{
	unsigned long qd = iolat->blkiolat->rqos.disk->queue->nr_requests;
	/* 큐 전체 nr_requests를 NVMe SQ depth 상한으로 가져옴 */
	unsigned long scale = scale_amount(qd, up);
	unsigned long old = iolat->max_depth;

	if (old > qd)
	// NVMe SQ depth는 큐 전체 nr_requests를 넘을 수 없음.
		old = qd;
		/* max_depth가 nr_requests보다 크면 상한으로 재조정. NVMe
		 * 드라이버가 실제로 사용하는 tags 수보다는 넉넉한 값(추정) */

	if (up) {
		if (old == 1 && blkcg_unuse_delay(lat_to_blkg(iolat)))
			return;
		/* qd가 1에서 더 줄어들 수 없으면 지연 계정부터 회복.
		 * NVMe SQ depth=1 상태에서 memory.delay 해제 시도(추정) */

		if (old < qd) {
			old += scale;
			/* NVMe SQ depth를 scale만큼 확대. hctx 병렬 dispatch
			 * 여유가 늘어남(추정) */
			old = min(old, qd);
			/* nr_requests 상한으로 클램프. NVMe 태그 풀(tag map)을
			 * 초과하지 않도록 제한(추정) */
			iolat->max_depth = old;
			wake_up_all(&iolat->rq_wait.wait);
			/* 대기 중인 bio들을 깨워 NVMe SQ 재진입 시도. scheduler
			 * plug/batch 해제와 연결(추정) */
		}
	} else {
		old >>= 1;
		// scale down은 절반으로 가파르게 감소.
		/* NVMe SQ depth를 절반으로 급감. CID/tag 할당 가능 수가
		 * 줄어들어 doorbell 빈도 감소(추정) */
		iolat->max_depth = max(old, 1UL);
		/* 최소 1 유지. NVMe SQ가 완전히 막히지 않도록 보장 */
	}
}

/* Check our parent and see if the scale cookie has changed. */
/*
 * check_scale_change - 부모의 scale_cookie 변화를 감지해 qd를 조정.
 *
 * blkcg_iolatency_throttle()이 submit 경로에서 호출.
 * 부모가 결정한 scale 방향을 따라 scale_change() 수행.
 * 자신이 scale_grp이고 기여도가 5% 이하면 억울한 scale down을 회피.
 */
static void check_scale_change(struct iolatency_grp *iolat)
{
	struct iolatency_grp *parent;
	struct child_latency_info *lat_info;
	unsigned int cur_cookie;
	unsigned int our_cookie = atomic_read(&iolat->scale_cookie);
	/* 자신이 마지막으로 본 부모 scale_cookie. NVMe qd 조정 이벤트의
	 * 로컬 버퍼(추정) */
	u64 scale_lat;
	int direction = 0;

	parent = blkg_to_lat(lat_to_blkg(iolat)->parent);
	/* 부모 cgroup의 iolatency_grp 획득. NVMe qd 조정은 부모가 자식들에게
	 * 전파하는 구조(추정) */
	if (!parent)
		return;
	// 부모가 없으면 root cgroup이므로 scale 전파 받지 않음.

	lat_info = &parent->child_lat;
	cur_cookie = atomic_read(&lat_info->scale_cookie);
	scale_lat = READ_ONCE(lat_info->scale_lat);
	/* 부모가 기록한 기준 지연값. READ_ONCE로 컴파일러 재배치 방지. NVMe
	 * completion ordering과 직접 관련은 없으나 타이머/스로틀 race 완화(추정) */

	if (cur_cookie < our_cookie)
		direction = -1;
	/* 부모 쿠키가 감소: NVMe SQ depth 축소 요청 */
	else if (cur_cookie > our_cookie)
		direction = 1;
	/* 부모 쿠키가 증가: NVMe SQ depth 확대 요청 */
	else
		return;
	// 부모 scale_cookie가 감소하면 qd 축소(-1), 증가면 확대(+1).

	if (!atomic_try_cmpxchg(&iolat->scale_cookie, &our_cookie, cur_cookie)) {
		/* Somebody beat us to the punch, just bail. */
		return;
	}
	/* atomic_try_cmpxchg로 로컬 쿠키 갱신. 다른 CPU가 먼저 처리한 경우
	 * 중복 scale 방지. NVMe qd 조정의 직렬화(추정) */

	if (direction < 0 && iolat->min_lat_nsec) {
		u64 samples_thresh;

		if (!scale_lat || iolat->min_lat_nsec <= scale_lat)
			return;
		/* 자신의 목표가 이미 scale_lat보다 높으면 영향받지 않음.
		 * 더 낮은 SLO를 가진 NVMe cgroup이 먼저 제한받아야 함(추정) */

		/*
		 * Sometimes high priority groups are their own worst enemy, so
		 * instead of taking it out on some poor other group that did 5%
		 * or less of the IO's for the last summation just skip this
		 * scale down event.
		 */
		samples_thresh = lat_info->nr_samples * 5;
		samples_thresh = max(1ULL, div64_u64(samples_thresh, 100));
		/* 전체 자식 샘플의 5% 임계값 계산. NVMe IO 기여도가 극소수면
		 * 다른 cgroup의 책임으로 간주(추정) */
		if (iolat->nr_samples <= samples_thresh)
			return;
		// 자신이 전체 IO의 5% 이하로 기여하면 남의 죄를 떠넘기지 않음.
	}

	/* We're as low as we can go. */
	if (iolat->max_depth == 1 && direction < 0) {
		blkcg_use_delay(lat_to_blkg(iolat));
		return;
	}
		// NVMe SQ depth를 1 이하로 내릴 수 없으므로 blkcg_use_delay()로
		// 사용자 공간 반환 지연 추가.
		/* max_depth=1에서 추가 scale down 요청이면 memory.delay 누적.
		 * NVMe SQ는 1개 슬롯만 허용하고, 나머지 지연은 submitter에게
		 * 직접 부과(추정) */

	/* We're back to the default cookie, unthrottle all the things. */
	if (cur_cookie == DEFAULT_SCALE_COOKIE) {
		blkcg_clear_delay(lat_to_blkg(iolat));
		iolat->max_depth = UINT_MAX;
		/* NVMe SQ depth 제한 해제. 실제 하드웨어 한도까지 발행 가능 */
		wake_up_all(&iolat->rq_wait.wait);
		return;
	}
		// scale_cookie가 기본으로 복귀하면 NVMe qd 제한 해제.
		/* 지연 누적을 제거하고 대기 중인 모든 bio를 깨움. NVMe mq의
		 * 다중 hctx가 동시에 dispatch 재개 가능(추정) */

	scale_change(iolat, direction > 0);
}

/*
 * blkcg_iolatency_throttle - bio 제출 시 상위 cgroup을 순회하며 스로틀.
 *
 * submit_bio -> blk_mq_submit_bio -> ... -> rq_qos_throttle() ->
 * blkcg_iolatency_throttle.
 * bio->bi_blkg에서 시작해 root blkg까지 각 iolatency_grp에 대해
 * check_scale_change()와 __blkcg_iolatency_throttle() 호출.
 * REQ_SWAP이면 use_memdelay를 true로 memory delay를 예약.
 */
static void blkcg_iolatency_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);
	/* 현재 NVMe queue의 blk_iolatency 인스턴스 획득 */
	struct blkcg_gq *blkg = bio->bi_blkg;
	/* bio가 속한 blkcg_gq. NVMe SQ/CQ 선택보다는 cgroup 경로 탐색용 */
	bool issue_as_root = bio_issue_as_root_blkg(bio);
	/* REQ_META/REQ_SWAP 등 root cgroup이 대신 발행해야 하는지 확인. NVMe
	 * CID/tag 할당 우선순위 inversion 방지용(추정) */

	if (!blkiolat->enabled)
		return;
	// io.latency가 활성화되지 않은 NVMe queue는 바로 통과.

	while (blkg && blkg->parent) {
	// bio가 속한 cgroup에서 root까지 거슬러 올라가며 제한 검사.
		struct iolatency_grp *iolat = blkg_to_lat(blkg);
		if (!iolat) {
			blkg = blkg->parent;
			continue;
		}

		check_scale_change(iolat);
		/* 부모로부터 NVMe qd 조정 이벤트 수신. submit 시점에 매번
		 * 확인하여 지연 목표 반영(추정) */
		__blkcg_iolatency_throttle(rqos, iolat, issue_as_root,
				     (bio->bi_opf & REQ_SWAP) == REQ_SWAP);
		/* bio->bi_opf & REQ_SWAP: swap IO 플래그 비트 테스트. swap
		 * IO인 경우 memory.delay에 추가 지연 예약(추정) */
		blkg = blkg->parent;
	}
	if (!timer_pending(&blkiolat->timer))
		mod_timer(&blkiolat->timer, jiffies + HZ);
	/* 스로틀 활동 시 1초 후 타이머 재설정. NVMe qd가 지속적으로 축소된
	 * 상태에서도 회복 기회를 마련(추정) */
}

/*
 * iolatency_record_time - 완료된 bio의 지연을 기록.
 *
 * blkcg_iolatency_done_bio -> iolatency_record_time.
 * issue_as_root이고 max_depth가 제한 중이면, root cgroup이 대신 내준
 * NVMe IO의 지연만큼 자식에게 delay를 부과.
 */
static void iolatency_record_time(struct iolatency_grp *iolat, u64 start,
				  u64 now, bool issue_as_root)
{
	u64 req_time;

	if (now <= start)
	// 시간 역전는 무시(IRQ 지연 등으로 인한 비정상 값).
		return;

	req_time = now - start;
	/* bio->issue_time_ns부터 현재까지 경과. NVMe SQ doorbell 시점부터
	 * CQ ISR 완료 시점까지의 대략적 지연(추정) */

	/*
	 * We don't want to count issue_as_root bio's in the cgroups latency
	 * statistics as it could skew the numbers downwards.
	 */
	if (unlikely(issue_as_root && iolat->max_depth != UINT_MAX)) {
		u64 sub = iolat->min_lat_nsec;
		if (req_time < sub)
			blkcg_add_delay(lat_to_blkg(iolat), now, sub - req_time);
		/* root가 대신 발행한 IO의 지연 부족분을 해당 cgroup에 지연
		 * 누적. NVMe SQ depth가 제한 중일 때만 의미 있음(추정) */
		return;
	}
		// root cgroup이 대신 발행한 IO(메타데이터 등)는 통계에 포함하지 않고
		// 대신 해당 cgroup에 delay를 추가.

	latency_stat_record_time(iolat, req_time);
	/* per-CPU 통계에 기록. NVMe SSD면 missed/total, HDD면 평균 누적 */
}

#define BLKIOLATENCY_MIN_ADJUST_TIME (500 * NSEC_PER_MSEC)
/* NVMe qd scale 조정 최소 간격. 500ms 이내 연속 조정 방지로 진동 억제 */
#define BLKIOLATENCY_MIN_GOOD_SAMPLES 5
/* scale up 허용 최소 양호 샘플 수. NVMe IO가 너무 적을 때 무리한 회복 방지 */

/*
 * iolatency_check_latencies - 윈도우 단위로 지연 통계를 평가하고 qd 조정.
 *
 * blkcg_iolatency_done_bio -> iolatency_check_latencies.
 * per-CPU 통계를 집계하고 목표 지연을 위반하면 부모 child_latency_info의
 * scale_cookie를 변경(scale_cookie_change), 이후 다른 자식들의
 * check_scale_change()에서 NVMe max_depth가 조정됨.
 */
static void iolatency_check_latencies(struct iolatency_grp *iolat, u64 now)
{
	struct blkcg_gq *blkg = lat_to_blkg(iolat);
	struct iolatency_grp *parent;
	struct child_latency_info *lat_info;
	struct latency_stat stat;
	unsigned long flags;
	int cpu;

	latency_stat_init(iolat, &stat);
	preempt_disable();
	for_each_online_cpu(cpu) {
		struct latency_stat *s;
		s = per_cpu_ptr(iolat->stats, cpu);
		/* 각 online CPU의 per-CPU 통계 접근. NVMe CQ 인터럽트가 도달한
		 * CPU별 완료 이벤트 수집(추정) */
		latency_stat_sum(iolat, &stat, s);
		latency_stat_init(iolat, s);
	}
	// 모든 online CPU의 per-CPU NVMe 통계를 윈도우 집계로 합산.
	preempt_enable();

	parent = blkg_to_lat(blkg->parent);
	if (!parent)
		return;

	lat_info = &parent->child_lat;

	iolat_update_total_lat_avg(iolat, &stat);

	/* Everything is ok and we don't need to adjust the scale. */
	if (latency_sum_ok(iolat, &stat) &&
	    atomic_read(&lat_info->scale_cookie) == DEFAULT_SCALE_COOKIE)
		return;
	// 목표 지연을 만족하고 scale_cookie가 기본이면 아무 것도 안 함.
	/* NVMe SLO를 만족하고 스로틀 중이 아니면 SQ depth 유지 */

	/* Somebody beat us to the punch, just bail. */
	spin_lock_irqsave(&lat_info->lock, flags);
	/* 부모의 child_latency_info 보호. 다중 CPU/다중 hctx에서 동시에
	 * scale_cookie를 변경하는 race 방지(추정) */

	latency_stat_sum(iolat, &iolat->cur_stat, &stat);
	/* 누적 중인 cur_stat에 이번 윈도우 합산. NVMe IO 샘플 누적 버퍼 갱신 */
	lat_info->nr_samples -= iolat->nr_samples;
	/* 이전에 기록한 이 cgroup의 샘플 수를 전체에서 제거. 원자적이지
	 * 않으나 spinlock으로 보호(추정) */
	lat_info->nr_samples += latency_stat_samples(iolat, &iolat->cur_stat);
	/* 갱신된 샘플 수를 전체에 다시 추가. 부모가 각 자식의 NVMe IO 기여도
	 * 평가(추정) */
	iolat->nr_samples = latency_stat_samples(iolat, &iolat->cur_stat);
	/* 자신의 최신 샘플 수 저장. scale_grp 5% 임계값 계산에 사용 */

	if ((lat_info->last_scale_event >= now ||
	    now - lat_info->last_scale_event < BLKIOLATENCY_MIN_ADJUST_TIME))
		goto out;
	// 500ms 이내에는 연속 scale 조정을 하지 않아 진동 방지.
	/* NVMe qd 조정 디바운스. 너무 빈번한 SQ depth 변화로 인한 성능
	 * 진동 방지(추정) */

	if (latency_sum_ok(iolat, &iolat->cur_stat) &&
	    latency_sum_ok(iolat, &stat)) {
		if (latency_stat_samples(iolat, &iolat->cur_stat) <
		    BLKIOLATENCY_MIN_GOOD_SAMPLES)
			goto out;
		// 충분한 양호한 샘플(5개)이 있어야 회복 scale up 허용.
		/* NVMe IO 샘플이 5개 미만이면 scale up 신뢰 불가. SQ depth를
		 * 쉽게 늘리지 않음(추정) */
		if (lat_info->scale_grp == iolat) {
			lat_info->last_scale_event = now;
			scale_cookie_change(iolat->blkiolat, lat_info, true);
		}
		/* 자신이 scale down의 원인이었고 이제 SLO를 만족하면 scale_cookie
		 * 회복 시작. NVMe SQ depth를 점진적으로 확대(추정) */
	} else if (lat_info->scale_lat == 0 ||
		   lat_info->scale_lat >= iolat->min_lat_nsec) {
		lat_info->last_scale_event = now;
		if (!lat_info->scale_grp ||
		    lat_info->scale_lat > iolat->min_lat_nsec) {
			WRITE_ONCE(lat_info->scale_lat, iolat->min_lat_nsec);
			/* scale_lat 갱신. WRITE_ONCE로 타이머/스로틀 path의
			 * 컴파일러 최적화/관찰 일관성 확보(추정) */
			lat_info->scale_grp = iolat;
			/* 현재 가장 낮은 SLO를 가진 cgroup을 scale_grp으로 지정.
			 * NVMe qd 감소의 책임자 기록(추정) */
		}
		scale_cookie_change(iolat->blkiolat, lat_info, false);
	}
		// 지연 목표 위반 시 scale_grp을 감신하고 scale_cookie를 감소시킨다.
		/* NVMe SLO 위반 시 부모의 scale_cookie를 감소시켜 자식들의
		 * max_depth가 scale down되도록 전파(추정) */
	latency_stat_init(iolat, &iolat->cur_stat);
	/* cur_stat 리셋. 다음 NVMe IO 완료 윈도우를 위해 통계 버퍼 초기화 */
out:
	spin_unlock_irqrestore(&lat_info->lock, flags);
}

/*
 * blkcg_iolatency_done_bio - bio 완료 시 지연 계산 및 inflight 감소.
 *
 * NVMe ISR/CQ 처리 경로에서 rq_qos_done_bio()를 통해 호출(추정).
 * bio->issue_time_ns부터 blk_time_get_ns()까지의 경과 시간이
 * NVMe 명령(CID)이 SQ에서 doorbell까지 나간 시점부터 CQ entry가
 * 완료된 시점까지의 지연에 해당(추정).
 */
static void blkcg_iolatency_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct iolatency_grp *iolat;
	u64 window_start;
	u64 now;
	bool issue_as_root = bio_issue_as_root_blkg(bio);
	int inflight = 0;

	blkg = bio->bi_blkg;
	if (!blkg || !bio_flagged(bio, BIO_QOS_THROTTLED))
		return;
	// io.latency를 거치지 않은 NVMe 완료는 무시.
	/* BIO_QOS_THROTTLED 플래그가 없으면 iolatency가 inflight를 증가시킨
	 * 적 없으므로 완료 처리 불필요. NVMe ISR/CQ 핸들링과 무관(추정) */

	iolat = blkg_to_lat(bio->bi_blkg);
	if (!iolat)
		return;

	if (!iolat->blkiolat->enabled)
		return;
	/* iolatency 마스터 스위치 off 시 완료 통계 미수집. NVMe queue가
	 * freeze되거나 비활성화 중일 때 대응(추정) */

	now = blk_time_get_ns();
	while (blkg && blkg->parent) {
		iolat = blkg_to_lat(blkg);
		if (!iolat) {
			blkg = blkg->parent;
			continue;
		}
		rqw = &iolat->rq_wait;

		inflight = atomic_dec_return(&rqw->inflight);
		/* NVMe SQ/CQ에서 명령 하나가 완료되었으므로 inflight 감소.
		 * 반환값으로 음수 경고(WARN_ON_ONCE) 검사 */
		WARN_ON_ONCE(inflight < 0);
		// NVMe SQ/CQ에서 명령 하나가 완료되었으므로 inflight 감소.
		/* inflight가 음수이면 완료/제출 불일치. NVMe CQ ISR에서 중복
		 * 완료나 누락된 제출이 있음을 시사(추정) */
		/*
		 * If bi_status is BLK_STS_AGAIN, the bio wasn't actually
		 * submitted, so do not account for it.
		 */
		if (iolat->min_lat_nsec && bio->bi_status != BLK_STS_AGAIN) {
		// 실제로 NVMe controller에 제출된 명령만 지연 통계에 반영.
			iolatency_record_time(iolat, bio->issue_time_ns, now,
				      issue_as_root);
			/* bio->issue_time_ns: bio가 NVMe submission path로 들어간
			 * 시점(추정). bio->bi_status != BLK_STS_AGAIN으로 실제
			 * doorbell이 나간 명령만 통계에 포함(추정) */
			window_start = atomic64_read(&iolat->window_start);
			/* 현재 통계 윈도우 시작 시각 로드. atomic64_read로 64bit
			 * 값 일관성 확보(추정) */
			if (now > window_start &&
			    (now - window_start) >= iolat->cur_win_nsec) {
				if (atomic64_try_cmpxchg(&iolat->window_start,
							 &window_start, now))
					iolatency_check_latencies(iolat, now);
			}
			// 통계 윈도우가 경과하면 iolatency_check_latencies()에서
			// NVMe qd scale 결정.
			/* atomic64_try_cmpxchg로 윈도우 시작 시각 갱신. 다른 CPU가
			 * 먼저 갱신한 경우 중복 평가 방지. NVMe 완료 인터럽트가
			 * 여러 CPU에서 동시에 도달할 수 있음(추정) */
		}
		wake_up(&rqw->wait);
		/* inflight가 감소했으므로 대기 중인 submitter를 깨움. NVMe SQ
		 * 슬롯이 하나 비었음을 알림(추정) */
		blkg = blkg->parent;
	}
}

/*
 * blkcg_iolatency_exit - io.latency QoS를 제거하고 정리.
 *
 * 타이머 종료, enable_work 종료, blkcg policy 비활성화 후 메모리 해제.
 * NVMe queue가 소멸될 때 함께 호출.
 */
static void blkcg_iolatency_exit(struct rq_qos *rqos)
{
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);

	timer_shutdown_sync(&blkiolat->timer);
	/* 타이머 동기 종료. NVMe queue 제거 시 회복 타이머가 발동하지 않도록
	 * 보장(추정) */
	flush_work(&blkiolat->enable_work);
	/* enable_work가 완료될 때까지 대기. queue freeze 중 enabled 토글의
	 * race 방지(추정) */
	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_iolatency);
	/* blkcg policy 비활성화. NVMe 디스크와의 policy 연결 해제 */
	kfree(blkiolat);
}

static const struct rq_qos_ops blkcg_iolatency_ops = {
	.throttle = blkcg_iolatency_throttle,
	/* submit_bio -> blk_mq_submit_bio 상단에서 호출. NVMe doorbell 이전
	 * 지연 제어 콜백(추정) */
	.done_bio = blkcg_iolatency_done_bio,
	/* bio 완료 시 호출. NVMe CQ ISR 경로에서 rq_qos_done_bio()를 통해
	 * 연결(추정) */
	.exit = blkcg_iolatency_exit,
};

/*
 * blkiolatency_timer_fn - 1초 타이머로 scale_cookie 회복.
 *
 * 스로틀이 지속되면 NVMe SQ depth가 1에 갇힐 수 있으므로, 일정 시간 경과
 * 후 scale_grp을 지우고 scale_cookie를 점진적으로 되돌림.
 */
static void blkiolatency_timer_fn(struct timer_list *t)
{
	struct blk_iolatency *blkiolat = timer_container_of(blkiolat, t,
						    timer);
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	u64 now = blk_time_get_ns();

	rcu_read_lock();
	/* blkg 계층 순회 동안 RCU 보호. NVMe queue freeze 없이 안전하게
	 * blkcg_gq 참조(추정) */
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkiolat->rqos.disk->queue->root_blkg) {
		/* root_blkg에서 시작해 모든 자식 blkcg_gq 순회. NVMe queue 내
		 * 모든 cgroup의 qd 상태 회복 기회 제공(추정) */
		struct iolatency_grp *iolat;
		struct child_latency_info *lat_info;
		unsigned long flags;
		u64 cookie;

		/*
		 * We could be exiting, don't access the pd unless we have a
		 * ref on the blkg.
		 */
		if (!blkg_tryget(blkg))
			continue;
		/* blkg reference 획득 실패 시 skip. NVMe queue나 cgroup이
		 * 해제 중일 수 있음(추정) */

		iolat = blkg_to_lat(blkg);
		if (!iolat)
			goto next;

		lat_info = &iolat->child_lat;
		cookie = atomic_read(&lat_info->scale_cookie);

		if (cookie >= DEFAULT_SCALE_COOKIE)
			goto next;
		/* 이미 DEFAULT 이상이면 NVMe qd 제한 해제 상태. 회복 불필요 */

		spin_lock_irqsave(&lat_info->lock, flags);
		if (lat_info->last_scale_event >= now)
			goto next_lock;
		/* last_scale_event가 미래 시점이면 시간 역전. timer가 과도하게
		 * 자주 실행된 경우 skip(추정) */

		/*
		 * We scaled down but don't have a scale_grp, scale up and carry
		 * on.
		 */
		if (lat_info->scale_grp == NULL) {
			scale_cookie_change(iolat->blkiolat, lat_info, true);
			goto next_lock;
		}
		/* scale_grp이 없으면 책임자를 알 수 없으므로 안전하게 scale up.
		 * NVMe SQ depth를 점진적으로 회복(추정) */

		/*
		 * It's been 5 seconds since our last scale event, clear the
		 * scale grp in case the group that needed the scale down isn't
		 * doing any IO currently.
		 */
		if (now - lat_info->last_scale_event >=
		    ((u64)NSEC_PER_SEC * 5))
			lat_info->scale_grp = NULL;
		/* 5초 이상 지연 조정이 없으면 scale_grp 초기화. 더 이상 IO를
		 * 내지 않는 cgroup이 NVMe qd를 계속 제한하는 것 방지(추정) */
next_lock:
		spin_unlock_irqrestore(&lat_info->lock, flags);
next:
		blkg_put(blkg);
	}
	rcu_read_unlock();
}

/**
 * blkiolatency_enable_work_fn - Enable or disable iolatency on the device
 * @work: enable_work of the blk_iolatency of interest
 *
 * iolatency needs to keep track of the number of in-flight IOs per cgroup. This
 * is relatively expensive as it involves walking up the hierarchy twice for
 * every IO. Thus, if iolatency is not enabled in any cgroup for the device, we
 * want to disable the in-flight tracking.
 *
 * We have to make sure that the counting is balanced - we don't want to leak
 * the in-flight counts by disabling accounting in the completion path while IOs
 * are in flight. This is achieved by ensuring that no IO is in flight by
 * freezing the queue while flipping ->enabled. As this requires a sleepable
 * context, ->enabled flipping is punted to this work function.
 */
/*
 * (한국어 요약) io.latency 사용 cgroup 수(enable_cnt)에 따라 enabled 스위치를
 * 전환. NVMe IO 통계 추적의 on/off를 queue freeze 상태에서 안전하게 처리.
 */
static void blkiolatency_enable_work_fn(struct work_struct *work)
{
	struct blk_iolatency *blkiolat = container_of(work, struct blk_iolatency,
						      enable_work);
	bool enabled;

	/*
	 * There can only be one instance of this function running for @blkiolat
	 * and it's guaranteed to be executed at least once after the latest
	 * ->enabled_cnt modification. Acting on the latest ->enable_cnt is
	 * sufficient.
	 *
	 * Also, we know @blkiolat is safe to access as ->enable_work is flushed
	 * in blkcg_iolatency_exit().
	 */
	enabled = atomic_read(&blkiolat->enable_cnt);
	if (enabled != blkiolat->enabled) {
		struct request_queue *q = blkiolat->rqos.disk->queue;
		unsigned int memflags;

		memflags = blk_mq_freeze_queue(blkiolat->rqos.disk->queue);
		/* queue freeze로 모든 NVMe hctx dispatch 중지. inflight 카운트가
		 * 0이 될 때까지 기다려 통계 누수 방지(추정) */
		blkiolat->enabled = enabled;
		if (enabled)
			blk_queue_flag_set(QUEUE_FLAG_BIO_ISSUE_TIME, q);
		else
			blk_queue_flag_clear(QUEUE_FLAG_BIO_ISSUE_TIME, q);
		/* QUEUE_FLAG_BIO_ISSUE_TIME 플래그 토글. bio->issue_time_ns
		 * 기록 여부를 결정. NVMe 완료 지연 측정의 시작점 제어(추정) */
		blk_mq_unfreeze_queue(blkiolat->rqos.disk->queue, memflags);
		/* queue unfreeze. NVMe hctx dispatch와 doorbell 발행 재개 */
	}
}

/*
 * blk_iolatency_init - 디스크에 io.latency QoS를 초기화.
 *
 * rq_qos_add()로 RQ_QOS_LATENCY 등록, blkcg_activate_policy()로 cgroup
 * policy 활성화. NVMe 디스크가 처음 io.latency 설정을 받을 때 호출.
 */
static int blk_iolatency_init(struct gendisk *disk)
{
	struct blk_iolatency *blkiolat;
	int ret;

	blkiolat = kzalloc_obj(*blkiolat);
	if (!blkiolat)
		return -ENOMEM;
	/* NVMe queue당 blk_iolatency 객체 할당 실패 시 -ENOMEM 반환 */

	ret = rq_qos_add(&blkiolat->rqos, disk, RQ_QOS_LATENCY,
			 &blkcg_iolatency_ops);
	if (ret)
		goto err_free;
	/* rq_qos 프레임워크에 등록. submit/complete 경로의 NVMe 상단 콜백
	 * 연결(추정) */
	ret = blkcg_activate_policy(disk, &blkcg_policy_iolatency);
	if (ret)
		goto err_qos_del;
	/* blkcg policy 활성화. NVMe 디스크의 각 cgroup에 iolatency_grp
	 * 할당(추정) */

	timer_setup(&blkiolat->timer, blkiolatency_timer_fn, 0);
	/* 1초 회복 타이머 설정. NVMe qd 스로틀이 지속될 때 자동 완화 */
	INIT_WORK(&blkiolat->enable_work, blkiolatency_enable_work_fn);
	/* enabled 토글 work 초기화. queue freeze 기반 NVMe 통계 스위치 */

	return 0;

err_qos_del:
	rq_qos_del(&blkiolat->rqos);
err_free:
	kfree(blkiolat);
	return ret;
}

/*
 * iolatency_set_min_lat_nsec - cgroup의 목표 지연 설정/해제.
 *
 * target 값이 0이면 비활성화, 아니면 min_lat_nsec/cur_win_nsec 설정.
 * 첫 활성화/마지막 비활성화 시 enable_work를 예약해 queue freeze 하에
 * enabled 플래그를 토글.
 */
static void iolatency_set_min_lat_nsec(struct blkcg_gq *blkg, u64 val)
{
	struct iolatency_grp *iolat = blkg_to_lat(blkg);
	struct blk_iolatency *blkiolat = iolat->blkiolat;
	u64 oldval = iolat->min_lat_nsec;

	iolat->min_lat_nsec = val;
	/* NVMe SLO(target latency) 설정. 0이면 제어 비활성화 */
	iolat->cur_win_nsec = max_t(u64, val << 4, BLKIOLATENCY_MIN_WIN_SIZE);
	/* 최소 16배 또는 100ms 중 큰 값을 통계 윈도우로. NVMe SSD의 빠른
	 * 응답에 충분한 샘플 수 확보(추정) */
	iolat->cur_win_nsec = min_t(u64, iolat->cur_win_nsec,
				    BLKIOLATENCY_MAX_WIN_SIZE);
	/* 1초 상한으로 클램프. NVMe 지연 회복 반응성 유지 */

	if (!oldval && val) {
		if (atomic_inc_return(&blkiolat->enable_cnt) == 1)
			schedule_work(&blkiolat->enable_work);
	}
	/* 처음으로 target이 설정되면 enable_cnt 증가. 첫 cgroup 활성화 시
	 * queue freeze 하에 enabled=true로 전환(추정) */
	if (oldval && !val) {
		blkcg_clear_delay(blkg);
		/* target 해제 시 지연 누적 클리어. NVMe qd 제한이 사라짐 */
		if (atomic_dec_return(&blkiolat->enable_cnt) == 0)
			schedule_work(&blkiolat->enable_work);
	}
	/* 마지막 cgroup이 비활성화되면 enable_cnt=0. queue freeze 하에
	 * enabled=false로 전환하여 불필요한 NVMe IO 통계 추적 중단(추정) */
}

/*
 * iolatency_clear_scaling - scale 상태 초기화.
 *
 * limit 값이 변경되면 기존 scale_cookie/scale_grp/scale_lat을 리셋.
 * NVMe qd 조정 상태를 새로운 목표 지연 기준으로 다시 시작.
 */
static void iolatency_clear_scaling(struct blkcg_gq *blkg)
{
	if (blkg->parent) {
		struct iolatency_grp *iolat = blkg_to_lat(blkg->parent);
		struct child_latency_info *lat_info;
		if (!iolat)
			return;

		lat_info = &iolat->child_lat;
		spin_lock(&lat_info->lock);
		atomic_set(&lat_info->scale_cookie, DEFAULT_SCALE_COOKIE);
		/* scale_cookie를 DEFAULT로 리셋. NVMe SQ depth 제한 해제 방향
		 * (초기화)(추정) */
		lat_info->last_scale_event = 0;
		/* scale 이벤트 시각 초기화. 새로운 NVMe 지연 목표에서 디바운스
		 * 재시작 */
		lat_info->scale_grp = NULL;
		/* 책임 cgroup 초기화. 이전 NVMe qd scale down의 원인 제거 */
		lat_info->scale_lat = 0;
		spin_unlock(&lat_info->lock);
	}
}

/*
 * iolatency_set_limit - sysfs "latency" 파일 쓰기 처리.
 *
 * 사용자가 "echo '259:0 target=5000' > latency" 형태로 NVMe 디스크의
 * 목표 지연을 설정. limit 변경 시 iolatency_clear_scaling()으로 기존
 * scale 상태를 초기화.
 */
static ssize_t iolatency_set_limit(struct kernfs_open_file *of, char *buf,
				   size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkcg_gq *blkg;
	struct blkg_conf_ctx ctx;
	struct iolatency_grp *iolat;
	char *p, *tok;
	u64 lat_val = 0;
	u64 oldval;
	int ret;

	blkg_conf_init(&ctx, buf);

	ret = blkg_conf_open_bdev(&ctx);
	if (ret)
		goto out;

	/*
	 * blk_iolatency_init() may fail after rq_qos_add() succeeds which can
	 * confuse iolat_rq_qos() test. Make the test and init atomic.
	 */
	lockdep_assert_held(&ctx.bdev->bd_queue->rq_qos_mutex);
	if (!iolat_rq_qos(ctx.bdev->bd_queue))
	// rq_qos_mutex 보호 하에서 NVMe queue당 최초 초기화 수행.
		ret = blk_iolatency_init(ctx.bdev->bd_disk);
	if (ret)
		goto out;

	ret = blkg_conf_prep(blkcg, &blkcg_policy_iolatency, &ctx);
	if (ret)
		goto out;

	iolat = blkg_to_lat(ctx.blkg);
	p = ctx.body;

	ret = -EINVAL;
	while ((tok = strsep(&p, " "))) {
		char key[16];
		char val[21];	/* 18446744073709551616 */

		if (sscanf(tok, "%15[^=]=%20s", key, val) != 2)
			goto out;

		if (!strcmp(key, "target")) {
			u64 v;

			if (!strcmp(val, "max"))
				lat_val = 0;
			/* "max"는 target 없음. NVMe SQ depth 무제한 의미 */
			else if (sscanf(val, "%llu", &v) == 1)
				lat_val = v * NSEC_PER_USEC;
			/* 사용자 입력 usec를 nsec로 변환. NVMe SLO 단위 조정 */
			else
				goto out;
		} else {
			goto out;
		}
	}

	/* Walk up the tree to see if our new val is lower than it should be. */
	blkg = ctx.blkg;
	oldval = iolat->min_lat_nsec;

	iolatency_set_min_lat_nsec(blkg, lat_val);
	if (oldval != iolat->min_lat_nsec)
		iolatency_clear_scaling(blkg);
	// 새 목표가 기존 scale 상태와 맞지 않을 수 있어 초기화.
	/* target이 변경되면 기존 NVMe qd scale 상태를 리셋. 새 SLO 기준으로
	 * 재평가(추정) */
	ret = 0;
out:
	blkg_conf_exit(&ctx);
	return ret ?: nbytes;
}

static u64 iolatency_prfill_limit(struct seq_file *sf,
				  struct blkg_policy_data *pd, int off)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	const char *dname = blkg_dev_name(pd->blkg);

	if (!dname || !iolat->min_lat_nsec)
		return 0;
	seq_printf(sf, "%s target=%llu\n",
		   dname, div_u64(iolat->min_lat_nsec, NSEC_PER_USEC));
	return 0;
}

static int iolatency_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  iolatency_prfill_limit,
			  &blkcg_policy_iolatency, seq_cft(sf)->private, false);
	return 0;
}

/*
 * iolatency_ssd_stat - NVMe SSD 모드용 디버그/통계 출력.
 *
 * missed/total 비율과 현재 max_depth(SQ depth)를 seq_file로 노출.
 */
static void iolatency_ssd_stat(struct iolatency_grp *iolat, struct seq_file *s)
{
	struct latency_stat stat;
	int cpu;

	latency_stat_init(iolat, &stat);
	preempt_disable();
	for_each_online_cpu(cpu) {
		struct latency_stat *s;
		s = per_cpu_ptr(iolat->stats, cpu);
		latency_stat_sum(iolat, &stat, s);
	}
	/* 모든 online CPU의 NVMe CQ 완료 통계를 집계. /sys 디버깅용(추정) */
	preempt_enable();

	if (iolat->max_depth == UINT_MAX)
		seq_printf(s, " missed=%llu total=%llu depth=max",
			(unsigned long long)stat.ps.missed,
			(unsigned long long)stat.ps.total);
	else
		seq_printf(s, " missed=%llu total=%llu depth=%u",
			(unsigned long long)stat.ps.missed,
			(unsigned long long)stat.ps.total,
			iolat->max_depth);
}

/*
 * iolatency_pd_stat - blkcg debug stats 출력.
 *
 * SSD 모드는 iolatency_ssd_stat(), HDD 모드는 avg_lat/win/max_depth 출력.
 */
static void iolatency_pd_stat(struct blkg_policy_data *pd, struct seq_file *s)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	unsigned long long avg_lat;
	unsigned long long cur_win;

	if (!blkcg_debug_stats)
		return;

	if (iolat->ssd)
		return iolatency_ssd_stat(iolat, s);
		/* NVMe/SSD: missed/total/depth 출력. CID/tag 기반 완료 통계 */

	avg_lat = div64_u64(iolat->lat_avg, NSEC_PER_USEC);
	cur_win = div64_u64(iolat->cur_win_nsec, NSEC_PER_MSEC);
	if (iolat->max_depth == UINT_MAX)
		seq_printf(s, " depth=max avg_lat=%llu win=%llu",
			avg_lat, cur_win);
	else
		seq_printf(s, " depth=%u avg_lat=%llu win=%llu",
			iolat->max_depth, avg_lat, cur_win);
}

/*
 * iolatency_pd_alloc - 새로운 blkcg_gq에 대한 iolatency_grp 할당.
 *
 * kzalloc_node() 후 per-CPU latency_stat 배열 할당.
 * NVMe CQ 인터럽트가 도달할 각 CPU별 통계 버퍼 마련.
 */
static struct blkg_policy_data *iolatency_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct iolatency_grp *iolat;

	iolat = kzalloc_node(sizeof(*iolat), gfp, disk->node_id);
	if (!iolat)
		return NULL;
	/* NVMe 디스크 노드에 맞는 NUMA 노드로 할당. DMA/PRP/SGL 메모리
	 * 접근 지역성과 무관하나 cgroup metadata 할당 locality 고려(추정) */
	iolat->stats = __alloc_percpu_gfp(sizeof(struct latency_stat),
				       __alignof__(struct latency_stat), gfp);
	if (!iolat->stats) {
		kfree(iolat);
		return NULL;
	}
	return &iolat->pd;
}

/*
 * iolatency_pd_init - 할당된 iolatency_grp 초기화.
 *
 * blk_queue_rot()이 false면 ssd=true(NVMe/SSD)로 설정.
 * max_depth=UINT_MAX(무제한)로 시작하고 부모의 scale_cookie를 상속.
 * NVMe SQ depth는 초기에 제한이 없다가 지연 위반 시 줄어든다.
 */
static void iolatency_pd_init(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	struct blkcg_gq *blkg = lat_to_blkg(iolat);
	struct rq_qos *rqos = iolat_rq_qos(blkg->q);
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);
	u64 now = blk_time_get_ns();
	int cpu;

	iolat->ssd = !blk_queue_rot(blkg->q);
	// 회전하지 않는 큐(NVMe/SSD)는 percentile 방식, HDD는 평균 방식 사용.
	/* blk_queue_rot() false면 NVMe/SSD로 간주. 회전 미디어는 평균 지연,
	 * NVMe는 missed/total percentile 사용(추정) */

	for_each_possible_cpu(cpu) {
		struct latency_stat *stat;
		stat = per_cpu_ptr(iolat->stats, cpu);
		latency_stat_init(iolat, stat);
	}
	/* 모든 possible CPU의 통계 버퍼 초기화. NVMe CQ 완료 인터럽트가 도달할
	 * CPU를 미리 준비(추정) */

	latency_stat_init(iolat, &iolat->cur_stat);
	rq_wait_init(&iolat->rq_wait);
	spin_lock_init(&iolat->child_lat.lock);
	iolat->max_depth = UINT_MAX;
	/* 초기 NVMe SQ depth 제한 없음. 실제 제한은 하드웨어 queue depth */
	iolat->blkiolat = blkiolat;
	iolat->cur_win_nsec = 100 * NSEC_PER_MSEC;
	atomic64_set(&iolat->window_start, now);
	/* 첫 통계 윈도우 시작 시각 설정. NVMe IO 완료 시점 비교 기준 */

	/*
	 * We init things in list order, so the pd for the parent may not be
	 * init'ed yet for whatever reason.
	 */
	if (blkg->parent && blkg_to_pd(blkg->parent, &blkcg_policy_iolatency)) {
		struct iolatency_grp *parent = blkg_to_lat(blkg->parent);
		atomic_set(&iolat->scale_cookie,
			   atomic_read(&parent->child_lat.scale_cookie));
		/* 부모의 현재 scale_cookie 상속. NVMe qd 조정 상태를 자식이
		 * 초기부터 동기화(추정) */
	} else {
		atomic_set(&iolat->scale_cookie, DEFAULT_SCALE_COOKIE);
	}

	atomic_set(&iolat->child_lat.scale_cookie, DEFAULT_SCALE_COOKIE);
}

/*
 * iolatency_pd_offline - cgroup이 off-line 될 때 limit/scale 정리.
 */
static void iolatency_pd_offline(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	struct blkcg_gq *blkg = lat_to_blkg(iolat);

	iolatency_set_min_lat_nsec(blkg, 0);
	iolatency_clear_scaling(blkg);
}

/*
 * iolatency_pd_free - iolatency_grp과 per-CPU 통계 메모리 해제.
 */
static void iolatency_pd_free(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	free_percpu(iolat->stats);
	/* per-CPU NVMe 완료 통계 버퍼 해제 */
	kfree(iolat);
}

static struct cftype iolatency_files[] = {
	{
		.name = "latency",
		/* cgroupfs 파일명. NVMe SLO 설정/조회 인터페이스 */
		.flags = CFTYPE_NOT_ON_ROOT,
		/* root cgroup에서는 target 설정 불가. NVMe qd 조정은 하위
		 * cgroup에서만 의미 있음(추정) */
		.seq_show = iolatency_print_limit,
		.write = iolatency_set_limit,
	},
	{}
};

static struct blkcg_policy blkcg_policy_iolatency = {
	.dfl_cftypes	= iolatency_files,
	/* cgroup v2(default hierarchy)용 파일 테이블. NVMe io.latency sysfs
	 * 인터페이스(추정) */
	.pd_alloc_fn	= iolatency_pd_alloc,
	.pd_init_fn	= iolatency_pd_init,
	.pd_offline_fn	= iolatency_pd_offline,
	.pd_free_fn	= iolatency_pd_free,
	.pd_stat_fn	= iolatency_pd_stat,
};

/*
 * iolatency_init - 모듈 로드 시 io.latency blkcg policy 등록.
 */
static int __init iolatency_init(void)
{
	return blkcg_policy_register(&blkcg_policy_iolatency);
	/* blkcg policy 등록. 이후 NVMe 디스크에서 io.latency 사용 가능 */
}

/*
 * iolatency_exit - 모듈 제거 시 policy 해제.
 */
static void __exit iolatency_exit(void)
{
	blkcg_policy_unregister(&blkcg_policy_iolatency);
	/* blkcg policy 해제. NVMe 디스크의 io.latency 기능 비활성화 */
}

module_init(iolatency_init);
module_exit(iolatency_exit);
/* NVMe 관점 핵심 요약
 *
 * - io.latency는 blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로 상단에서 bio 단위
 *   평균 지연을 제어한다.
 * - NVMe SSD(ssd=true)는 missed/total percentile로, HDD는 평균(mean)으로
 *   목표 지연을 판단한다.
 * - max_depth는 NVMe SQ(submission queue) 깊이에 대응하며, 지연 위반 시
 *   scale down되어 큐가 짧아지고, 완화 시 scale up된다.
 * - max_depth가 1에 도달해도 지연이 초과되면 blkcg_use_delay()를 통해
 *   사용자 공간 반환에 추가 지연을 유발한다.
 * - block/blk-rq-qos.h의 rq_qos_ops 프레임워크 기반이며, block/blk-wbt.c와
 *   유사한 위치에서 동작한다.
 */
