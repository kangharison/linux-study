/* SPDX-License-Identifier: GPL-2.0
 *
 * IO cost model based controller.
 *
 * Copyright (C) 2019 Tejun Heo <tj@kernel.org>
 * Copyright (C) 2019 Andy Newell <newella@fb.com>
 * Copyright (C) 2019 Facebook
 *
 * One challenge of controlling IO resources is the lack of trivially
 * observable cost metric.  This is distinguished from CPU and memory where
 * wallclock time and the number of bytes can serve as accurate enough
 * approximations.
 *
 * Bandwidth and iops are the most commonly used metrics for IO devices but
 * depending on the type and specifics of the device, different IO patterns
 * easily lead to multiple orders of magnitude variations rendering them
 * useless for the purpose of IO capacity distribution.  While on-device
 * time, with a lot of clutches, could serve as a useful approximation for
 * non-queued rotational devices, this is no longer viable with modern
 * devices, even the rotational ones.
 *
 * While there is no cost metric we can trivially observe, it isn't a
 * complete mystery.  For example, on a rotational device, seek cost
 * dominates while a contiguous transfer contributes a smaller amount
 * proportional to the size.  If we can characterize at least the relative
 * costs of these different types of IOs, it should be possible to
 * implement a reasonable work-conserving proportional IO resource
 * distribution.
 *
 * 1. IO Cost Model
 *
 * IO cost model estimates the cost of an IO given its basic parameters and
 * history (e.g. the end sector of the last IO).  The cost is measured in
 * device time.  If a given IO is estimated to cost 10ms, the device should
 * be able to process ~100 of those IOs in a second.
 *
 * Currently, there's only one builtin cost model - linear.  Each IO is
 * classified as sequential or random and given a base cost accordingly.
 * On top of that, a size cost proportional to the length of the IO is
 * added.  While simple, this model captures the operational
 * characteristics of a wide varienty of devices well enough.  Default
 * parameters for several different classes of devices are provided and the
 * parameters can be configured from userspace via
 * /sys/fs/cgroup/io.cost.model.
 *
 * If needed, tools/cgroup/iocost_coef_gen.py can be used to generate
 * device-specific coefficients.
 *
 * 2. Control Strategy
 *
 * The device virtual time (vtime) is used as the primary control metric.
 * The control strategy is composed of the following three parts.
 *
 * 2-1. Vtime Distribution
 *
 * When a cgroup becomes active in terms of IOs, its hierarchical share is
 * calculated.  Please consider the following hierarchy where the numbers
 * inside parentheses denote the configured weights.
 *
 *           root
 *         /       \
 *      A (w:100)  B (w:300)
 *      /       \
 *  A0 (w:100)  A1 (w:100)
 *
 * If B is idle and only A0 and A1 are actively issuing IOs, as the two are
 * of equal weight, each gets 50% share.  If then B starts issuing IOs, B
 * gets 300/(100+300) or 75% share, and A0 and A1 equally splits the rest,
 * 12.5% each.  The distribution mechanism only cares about these flattened
 * shares.  They're called hweights (hierarchical weights) and always add
 * upto 1 (WEIGHT_ONE).
 *
 * A given cgroup's vtime runs slower in inverse proportion to its hweight.
 * For example, with 12.5% weight, A0's time runs 8 times slower (100/12.5)
 * against the device vtime - an IO which takes 10ms on the underlying
 * device is considered to take 80ms on A0.
 *
 * This constitutes the basis of IO capacity distribution.  Each cgroup's
 * vtime is running at a rate determined by its hweight.  A cgroup tracks
 * the vtime consumed by past IOs and can issue a new IO if doing so
 * wouldn't outrun the current device vtime.  Otherwise, the IO is
 * suspended until the vtime has progressed enough to cover it.
 *
 * 2-2. Vrate Adjustment
 *
 * It's unrealistic to expect the cost model to be perfect.  There are too
 * many devices and even on the same device the overall performance
 * fluctuates depending on numerous factors such as IO mixture and device
 * internal garbage collection.  The controller needs to adapt dynamically.
 *
 * This is achieved by adjusting the overall IO rate according to how busy
 * the device is.  If the device becomes overloaded, we're sending down too
 * many IOs and should generally slow down.  If there are waiting issuers
 * but the device isn't saturated, we're issuing too few and should
 * generally speed up.
 *
 * To slow down, we lower the vrate - the rate at which the device vtime
 * passes compared to the wall clock.  For example, if the vtime is running
 * at the vrate of 75%, all cgroups added up would only be able to issue
 * 750ms worth of IOs per second, and vice-versa for speeding up.
 *
 * Device business is determined using two criteria - rq wait and
 * completion latencies.
 *
 * When a device gets saturated, the on-device and then the request queues
 * fill up and a bio which is ready to be issued has to wait for a request
 * to become available.  When this delay becomes noticeable, it's a clear
 * indication that the device is saturated and we lower the vrate.  This
 * saturation signal is fairly conservative as it only triggers when both
 * hardware and software queues are filled up, and is used as the default
 * busy signal.
 *
 * As devices can have deep queues and be unfair in how the queued commands
 * are executed, solely depending on rq wait may not result in satisfactory
 * control quality.  For a better control quality, completion latency QoS
 * parameters can be configured so that the device is considered saturated
 * if N'th percentile completion latency rises above the set point.
 *
 * The completion latency requirements are a function of both the
 * underlying device characteristics and the desired IO latency quality of
 * service.  There is an inherent trade-off - the tighter the latency QoS,
 * the higher the bandwidth lossage.  Latency QoS is disabled by default
 * and can be set through /sys/fs/cgroup/io.cost.qos.
 *
 * 2-3. Work Conservation
 *
 * Imagine two cgroups A and B with equal weights.  A is issuing a small IO
 * periodically while B is sending out enough parallel IOs to saturate the
 * device on its own.  Let's say A's usage amounts to 100ms worth of IO
 * cost per second, i.e., 10% of the device capacity.  The naive
 * distribution of half and half would lead to 60% utilization of the
 * device, a significant reduction in the total amount of work done
 * compared to free-for-all competition.  This is too high a cost to pay
 * for IO control.
 *
 * To conserve the total amount of work done, we keep track of how much
 * each active cgroup is actually using and yield part of its weight if
 * there are other cgroups which can make use of it.  In the above case,
 * A's weight will be lowered so that it hovers above the actual usage and
 * B would be able to use the rest.
 *
 * As we don't want to penalize a cgroup for donating its weight, the
 * surplus weight adjustment factors in a margin and has an immediate
 * snapback mechanism in case the cgroup needs more IO vtime for itself.
 *
 * Note that adjusting down surplus weights has the same effects as
 * accelerating vtime for other cgroups and work conservation can also be
 * implemented by adjusting vrate dynamically.  However, squaring who can
 * donate and should take back how much requires hweight propagations
 * anyway making it easier to implement and understand as a separate
 * mechanism.
 *
 * 3. Monitoring
 *
 * Instead of debugfs or other clumsy monitoring mechanisms, this
 * controller uses a drgn based monitoring script -
 * tools/cgroup/iocost_monitor.py.  For details on drgn, please see
 * https://github.com/osandov/drgn.  The output looks like the following.
 *
 *  sdb RUN   per=300ms cur_per=234.218:v203.695 busy= +1 vrate= 62.12%
 *                 active      weight      hweight% inflt% dbt  delay usages%
 *  test/a              *    50/   50  33.33/ 33.33  27.65   2  0*041 033:033:033
 *  test/b              *   100/  100  66.67/ 66.67  17.56   0  0*000 066:079:077
 *
 * - per	: Timer period
 * - cur_per	: Internal wall and device vtime clock
 * - vrate	: Device virtual time rate against wall clock
 * - weight	: Surplus-adjusted and configured weights
 * - hweight	: Surplus-adjusted and configured hierarchical weights
 * - inflt	: The percentage of in-flight IO cost at the end of last period
 * - del_ms	: Deferred issuer delay induction level and duration
 * - usages	: Usage history
 */

/*
 * [한국어 설명] IO 비용 모델 기반 cgroup IO 컨트롤러 (blk-iocost.c)
 *
 * === 파일의 역할 ===
 * blk-iocost는 blk-mq 요청 큐 위에 앉는 rq-qos 계층으로, cgroup 트리의 가중치
 * 비율에 따라 각 cgroup의 IO 처리량을 공정하게 분배한다. "비용 모델"이란 iops나
 * 대역폭 대신 디바이스 가상 시간(vtime)을 공통 단위로 삼아 순차/랜덤 IO,
 * 크기 차이를 하나의 척도로 통일하는 것을 의미한다. 주기적 타이머(ioc_timer_fn)가
 * 100~300ms 주기로 실제 장치 포화도를 측정해 vrate(가상 시간 진행 속도)와
 * 각 cgroup의 inuse 가중치를 동적으로 조정하며, 잉여 가중치는 타 cgroup에 양도해
 * 유휴 용량을 낭비하지 않는 Work Conservation을 달성한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 IO 경로에서 blk-iocost는 다음 위치에 삽입된다:
 *
 *   [응용] write(2) / io_uring
 *       ↓
 *   [VFS/파일시스템] submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓  ← rq_qos_throttle() 훅: ioc_rqos_throttle() 호출
 *   [iocost] vtime 예산 검사 → 초과 시 iocg_kick_delay()로 발급자 지연
 *       ↓  ← vtime 예산 확보 후
 *   [blk-mq] blk_mq_get_request() → 드라이버 큐에 request 삽입
 *       ↓
 *   [NVMe 드라이버] nvme_queue_rq() → doorbell 기입 → SQ/CQ
 *       ↓  ← 완료 인터럽트
 *   [blk-mq] blk_mq_complete_request()
 *       ↓  ← rq_qos_done() 훅: ioc_rqos_done() 호출
 *   [iocost] rq_wait_ns / 완료 지연 누적 → 다음 주기 vrate 조정 입력
 *
 * 실행 컨텍스트: ioc_rqos_throttle()은 프로세스 컨텍스트(bio 제출 경로),
 * ioc_timer_fn()은 softirq 타이머 컨텍스트에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-rq-qos.c: rq_qos 훅 프레임워크 (ioc_rqos_ops 등록)
 *   - block/blk-cgroup.c: blkcg_policy 등록(blkcg_policy_iocost), blkg별 ioc_gq
 *     생성/소멸 콜백(pd_alloc_fn/pd_free_fn)
 *   - block/blk-mq.c: blk_mq_submit_bio() → rq_qos_throttle() 경로로 훅 진입
 *   - include/linux/blk-cgroup.h: blkcg_gq, blkcg_policy_data 구조체
 * 의존받는 모듈:
 *   - 사용자 공간: /sys/fs/cgroup/io.cost.model, io.cost.qos 로 파라미터 제어
 *   - tools/cgroup/iocost_monitor.py: drgn 기반 모니터링 스크립트
 * 공유 자료구조:
 *   - struct ioc: 디바이스(request_queue)당 하나, vrate·타이머·활성 iocg 리스트
 *   - struct ioc_gq: cgroup × 디바이스 교차점, vtime 예산·부채·stat 보유
 *
 * === 주요 함수/구조체 요약 ===
 * ioc_rqos_throttle()   - bio 제출 시 vtime 예산 검사, 초과 cgroup 지연/대기
 * ioc_timer_fn()        - 주기 타이머: rq_wait/완료지연 측정 → vrate 조정 → 가중치 재계산
 * ioc_adjust_base_vrate() - 장치 포화도(rq_wait_pct, missed latency)로 vrate 상하 조정
 * propagate_weights()   - hweight(계층적 가중치) 트리 전파, inuse 서플러스 조정
 * iocg_activate()       - 첫 IO 시 비활성 cgroup 활성화, 초기 vtime/가중치 설정
 * transfer_surpluses()  - 잉여 가중치를 수요 있는 cgroup에 양도 (Work Conservation)
 * iocg_kick_waitq()     - 예산 회복 시 대기 중인 bio 발급자들을 깨워 재시도
 * struct ioc            - 디바이스당 컨트롤러 상태: vtime, vrate, 타이머, active_iocgs
 * struct ioc_gq         - cgroup별 IO 상태: vtime 예산, 부채(debt), 지연 레벨, wait 큐
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/time64.h>
#include <linux/parser.h>
#include <linux/sched/signal.h>
#include <asm/local.h>
#include <asm/local64.h>
#include "blk-rq-qos.h"
#include "blk-stat.h"
#include "blk-wbt.h"
#include "blk-cgroup.h"

#ifdef CONFIG_TRACEPOINTS

/* copied from TRACE_CGROUP_PATH, see cgroup-internal.h */
#define TRACE_IOCG_PATH_LEN 1024
static DEFINE_SPINLOCK(trace_iocg_path_lock);
static char trace_iocg_path[TRACE_IOCG_PATH_LEN];

#define TRACE_IOCG_PATH(type, iocg, ...)					\
	do {									\
		unsigned long flags;						\
		if (trace_iocost_##type##_enabled()) {				\
			spin_lock_irqsave(&trace_iocg_path_lock, flags);	\
			cgroup_path(iocg_to_blkg(iocg)->blkcg->css.cgroup,	\
				    trace_iocg_path, TRACE_IOCG_PATH_LEN);	\
			trace_iocost_##type(iocg, trace_iocg_path,		\
					      ##__VA_ARGS__);			\
			spin_unlock_irqrestore(&trace_iocg_path_lock, flags);	\
		}								\
	} while (0)

#else	/* CONFIG_TRACE_POINTS */
#define TRACE_IOCG_PATH(type, iocg, ...)	do { } while (0)
#endif	/* CONFIG_TRACE_POINTS */

enum {
	MILLION			= 1000000,	/* NVMe latency QoS 백분율/ppm 계산 기준 */

	/* timer period is calculated from latency requirements, bound it */
	MIN_PERIOD		= USEC_PER_MSEC,	/* NVMe 주기 타이머 최소 1ms: 너무 짧으면 doorbell storm 유발(추정) */
	MAX_PERIOD		= USEC_PER_SEC,	/* NVMe 주기 타이머 최대 1s: 너무 길면 CQ 포화 반응 지연(추정) */

	/*
	 * iocg->vtime is targeted at 50% behind the device vtime, which
	 * serves as its IO credit buffer.  Surplus weight adjustment is
	 * immediately canceled if the vtime margin runs below 10%.
	 */
	MARGIN_MIN_PCT		= 10,	/* NVMe SQ 포화 직전 최소 예산 여유(%) */
	MARGIN_LOW_PCT		= 20,	/* NVMe 제출률 저하를 검토할 예산 여유(%) */
	MARGIN_TARGET_PCT	= 50,	/* NVMe SQ/CQ에 무리가 가지 않을 목표 예산 버퍼(%) */

	INUSE_ADJ_STEP_PCT	= 25,	/* cgroup이 NVMe 시간 할당분을 회복할 때 inuse 증가폭(%) */

	/* Have some play in timer operations */
	TIMER_SLACK_PCT		= 1,	/* waitq 타이머 해상도 여유: NVMe doorbell 타이밍 민감도 완화(추정) */

	/* 1/64k is granular enough and can easily be handled w/ u32 */
	WEIGHT_ONE		= 1 << 16,	/* cgroup NVMe 시간 분배 정밀도(1.0) */
};

enum {
	/*
	 * As vtime is used to calculate the cost of each IO, it needs to
	 * be fairly high precision.  For example, it should be able to
	 * represent the cost of a single page worth of discard with
	 * suffificient accuracy.  At the same time, it should be able to
	 * represent reasonably long enough durations to be useful and
	 * convenient during operation.
	 *
	 * 1s worth of vtime is 2^37.  This gives us both sub-nanosecond
	 * granularity and days of wrap-around time even at extreme vrates.
	 */
	VTIME_PER_SEC_SHIFT	= 37,	/* NVMe 명령 단위 비용의 sub-nsec 정밀도 확보 */
	VTIME_PER_SEC		= 1LLU << VTIME_PER_SEC_SHIFT,	/* 1초를 vtime 단위로 변환한 값 */
	VTIME_PER_USEC		= VTIME_PER_SEC / USEC_PER_SEC,	/* wallclock 1us를 NVMe vtime으로 환산 */
	VTIME_PER_NSEC		= VTIME_PER_SEC / NSEC_PER_SEC,	/* NVMe CQ 타임스탬프와 직접 비교 가능 */

	/* bound vrate adjustments within two orders of magnitude */
	VRATE_MIN_PPM		= 10000,	/* 1%: NVMe에 내릴 수 있는 최소 상대 IO 압력 */
	VRATE_MAX_PPM		= 100000000,	/* 10000%: NVMe에 내릴 수 있는 최대 상대 IO 압력 */

	VRATE_MIN		= VTIME_PER_USEC * VRATE_MIN_PPM / MILLION,	/* vrate 절대 하한 */
	VRATE_CLAMP_ADJ_PCT	= 4,	/* NVMe QoS 경계 돌파 시 vrate를 4%씩 완화해 안정화 */

	/* switch iff the conditions are met for longer than this */
	AUTOP_CYCLE_NSEC	= 10LLU * NSEC_PER_SEC,	/* NVMe 프로파일 전환까지 10초 지속 조건 */
};

enum {
	/* if IOs end up waiting for requests, issue less */
	RQ_WAIT_BUSY_PCT	= 5,	/* NVMe request(tag) 할당 대기가 주기의 5% 초과 시 포화로 판단 */

	/* unbusy hysterisis */
	UNBUSY_THR_PCT		= 75,	/* NVMe latency QoS 목표를 75% 수준으로 달성하면 unbusy */

	/*
	 * The effect of delay is indirect and non-linear and a huge amount of
	 * future debt can accumulate abruptly while unthrottled. Linearly scale
	 * up delay as debt is going up and then let it decay exponentially.
	 * This gives us quick ramp ups while delay is accumulating and long
	 * tails which can help reducing the frequency of debt explosions on
	 * unthrottle. The parameters are experimentally determined.
	 *
	 * The delay mechanism provides adequate protection and behavior in many
	 * cases. However, this is far from ideal and falls shorts on both
	 * fronts. The debtors are often throttled too harshly costing a
	 * significant level of fairness and possibly total work while the
	 * protection against their impacts on the system can be choppy and
	 * unreliable.
	 *
	 * The shortcoming primarily stems from the fact that, unlike for page
	 * cache, the kernel doesn't have well-defined back-pressure propagation
	 * mechanism and policies for anonymous memory. Fully addressing this
	 * issue will likely require substantial improvements in the area.
	 */
	MIN_DELAY_THR_PCT	= 500,	/* vtime 초과 500%부터 blkcg use_delay 유도 → NVMe 제출 억제 */
	MAX_DELAY_THR_PCT	= 25000,	/* vtime 초과 25000%에서 최대 지연 도달 */
	MIN_DELAY		= 250,	/* 최소 250us 지연: NVMe SQ doorbell 간격 확보(추정) */
	MAX_DELAY		= 250 * USEC_PER_MSEC,	/* 최대 250ms 지연: 극단적 NVMe 과부하 완화 */

	/* halve debts if avg usage over 100ms is under 50% */
	DFGV_USAGE_PCT		= 50,	/* NVMe 사용률 50% 이하 시 부채 탕감 */
	DFGV_PERIOD		= 100 * USEC_PER_MSEC,	/* 100ms 단위 NVMe idle 측정 */

	/* don't let cmds which take a very long time pin lagging for too long */
	MAX_LAGGING_PERIODS	= 10,	/* 10주기 이상 완료되지 않은 NVMe 명령은 lagging으로 처리 */

	/*
	 * Count IO size in 4k pages.  The 12bit shift helps keeping
	 * size-proportional components of cost calculation in closer
	 * numbers of digits to per-IO cost components.
	 */
	IOC_PAGE_SHIFT		= 12,	/* NVMe PRP/SGL entry 크기(4KB) 기준 */
	IOC_PAGE_SIZE		= 1 << IOC_PAGE_SHIFT,	/* NVMe DMA 페이지 단위 */
	IOC_SECT_TO_PAGE_SHIFT	= IOC_PAGE_SHIFT - SECTOR_SHIFT,	/* 섹터 -> 4KB 페이지 변환: NVMe 명령 크기 계산용 */

	/* if apart further than 16M, consider randio for linear model */
	LCOEF_RANDIO_PAGES	= 4096,	/* 16MB 이상 떨어지면 NVMe random seek로 간주 */
};

enum ioc_running {
	IOC_IDLE,	/* NVMe IO 미발행 상태, 타이머 정지 */
	IOC_RUNNING,	/* NVMe IO 활성, 주기 타이머 동작 */
	IOC_STOP,	/* blk-iocost 종료, NVMe 경로 해제 중 */
};

/* io.cost.qos controls including per-dev enable of the whole controller */
enum {
	QOS_ENABLE,
	QOS_CTRL,
	NR_QOS_CTRL_PARAMS,
};

/* io.cost.qos params */
enum {
	QOS_RPPM,
	QOS_RLAT,
	QOS_WPPM,
	QOS_WLAT,
	QOS_MIN,
	QOS_MAX,
	NR_QOS_PARAMS,
};

/* io.cost.model controls */
enum {
	COST_CTRL,
	COST_MODEL,
	NR_COST_CTRL_PARAMS,
};

/* builtin linear cost model coefficients */
enum {
	I_LCOEF_RBPS,
	I_LCOEF_RSEQIOPS,
	I_LCOEF_RRANDIOPS,
	I_LCOEF_WBPS,
	I_LCOEF_WSEQIOPS,
	I_LCOEF_WRANDIOPS,
	NR_I_LCOEFS,
};

enum {
	LCOEF_RPAGE,
	LCOEF_RSEQIO,
	LCOEF_RRANDIO,
	LCOEF_WPAGE,
	LCOEF_WSEQIO,
	LCOEF_WRANDIO,
	NR_LCOEFS,
};

enum {
	AUTOP_INVALID,
	AUTOP_HDD,
	AUTOP_SSD_QD1,
	AUTOP_SSD_DFL,
	AUTOP_SSD_FAST,
};

/*
 * ioc_params: NVMe 장치별 비용/ QoS 파라미터 집합.
 * autop[] 테이블에서 장치 유형(HDD, SSD QD1, SSD 기본/고속)에 따라
 * 초기화되며, NVMe의 sequential/random IOPS와 대역폭 특성을 반영한다.
 */
struct ioc_params {
	u32				qos[NR_QOS_PARAMS];
	u64				i_lcoefs[NR_I_LCOEFS];
	u64				lcoefs[NR_LCOEFS];
	u32				too_fast_vrate_pct;
	u32				too_slow_vrate_pct;
};

/*
 * ioc_margins: cgroup vtime가 device vtime보다 얼마나 뒤처질 수 있는지를
 * 나타내는 여유분. NVMe SQ/CQ가 포화되기 전에 사전에 쓰로틀링할 버퍼 역할.
 */
struct ioc_margins {
	s64				min;
	s64				low;
	s64				target;
};

struct ioc_missed {
	local_t				nr_met;
	local_t				nr_missed;
	u32				last_met;
	u32				last_missed;
};

/*
 * ioc_pcpu_stat: 장치 단위 per-CPU 통계. NVMe 명령이 CQ로 돌아올 때
 * 완료 지연(latency QoS)과 rq 할당 대기 시간을 수집한다.
 */
struct ioc_pcpu_stat {
	struct ioc_missed		missed[2];

	local64_t			rq_wait_ns;
	u64				last_rq_wait_ns;
};

/*
 * ioc: 장치별 iocost 컨트롤러. NVMe 큐 한 세트(SQ/CQ)에 대응되며,
 * 전체 장치의 가상 시간 축(vtime)과 주기 타이머를 관리한다.
 *
 * - params: NVMe 장치별 선형 비용 계수(시퀀셜/랜덤 IOPS, 대역폭)
 * - vtime_rate: NVMe에 실제로 날아가는 bio 속도에 대한 보정률(vrate)
 * - active_iocgs: 현재 IO를 활발히 제출 중인 cgroup 목록
 * - pcpu_stat: CQ 완료 시 측정된 latency QoS 및 rq_wait 통계
 * - busy_level: NVMe 장치/소프트웨어 큐 포화 정도의 누적 지표
 */
/* per device */
struct ioc {
	struct rq_qos			rqos;

	bool				enabled;

	struct ioc_params		params;	/* NVMe 장치별 seq/rand IOPS/대역폭 계수 */
	struct ioc_margins		margins;	/* NVMe SQ 포화 전 예산 버퍼 */
	u32				period_us;
	u32				timer_slack_ns;
	u64				vrate_min;
	u64				vrate_max;

	spinlock_t			lock;
	struct timer_list		timer;	/* iocost 주기 타이머: NVMe 완료 지연 피드백 주기 */
	struct list_head		active_iocgs;	/* active cgroups */
	struct ioc_pcpu_stat __percpu	*pcpu_stat;	/* per-CPU NVMe CQ 완료 지연/ rq_wait 통계 */

	enum ioc_running		running;	/* NVMe 제어 타이머 상태 */
	atomic64_t			vtime_rate;
	u64				vtime_base_rate;
	s64				vtime_err;

	seqcount_spinlock_t		period_seqcount;	/* period_at/period_at_vtime 일관성: NVMe 타이머 시계 동기화 */
	u64				period_at;	/* wallclock starttime */
	u64				period_at_vtime; /* vtime starttime */

	atomic64_t			cur_period;	/* inc'd each period */
	int				busy_level;	/* saturation history */

	bool				weights_updated;
	atomic_t			hweight_gen;	/* for lazy hweights */

	/* debt forgivness */
	u64				dfgv_period_at;
	u64				dfgv_period_rem;
	u64				dfgv_usage_us_sum;

	u64				autop_too_fast_at;
	u64				autop_too_slow_at;
	int				autop_idx;
	bool				user_qos_params:1;
	bool				user_cost_model:1;
};

/*
 * iocg_pcpu_stat: cgroup별 per-CPU 절대 비용 사용량.
 * NVMe에 내린 명령들의 누적 vtime 사용량을 CPU 단위로 집계한다.
 */
struct iocg_pcpu_stat {
	local64_t			abs_vusage;
};

/*
 * iocg_stat: cgroup별 누적 시간 통계. NVMe 제출/대기/부채/지연 상태를
 * microseconds로 기록해 모니터링(tools/cgroup/iocost_monitor.py)에 제공.
 */
struct iocg_stat {
	u64				usage_us;
	u64				wait_us;
	u64				indebt_us;
	u64				indelay_us;
};

/*
 * ioc_gq: 장치-cgroup 쌍별 상태. NVMe에 실제 제출될 bio 한 개 단위의
 * 예산(vtime)과 계층적 가중치를 관리한다.
 *
 * - vtime: 이 cgroup이 NVMe에 내린 명령들의 누적 비용(issued 기준)
 * - done_vtime: NVMe CQ 완료로 돌아온 명령들의 누적 비용(completed 기준)
 * - cursor: 직전 bio의 마지막 섹터; NVMe sequential vs random 판별에 사용
 * - waitq: 예산 부족으로 블록된 issuer 대기열
 * - hweight_active/hweight_inuse: cgroup 계층에서 이 cgroup의 NVMe 시간
 *   할당 비율. hweight_inuse가 낮을수록 동일한 bio도 더 비싸게 계산됨
 * - abs_vdebt: root cgroup 등 우선 발행된 IO의 미지급 절대 비용
 */
/* per device-cgroup pair */
struct ioc_gq {
	struct blkg_policy_data		pd;
	struct ioc			*ioc;

	/*
	 * A iocg can get its weight from two sources - an explicit
	 * per-device-cgroup configuration or the default weight of the
	 * cgroup.  `cfg_weight` is the explicit per-device-cgroup
	 * configuration.  `weight` is the effective considering both
	 * sources.
	 *
	 * When an idle cgroup becomes active its `active` goes from 0 to
	 * `weight`.  `inuse` is the surplus adjusted active weight.
	 * `active` and `inuse` are used to calculate `hweight_active` and
	 * `hweight_inuse`.
	 *
	 * `last_inuse` remembers `inuse` while an iocg is idle to persist
	 * surplus adjustments.
	 *
	 * `inuse` may be adjusted dynamically during period. `saved_*` are used
	 * to determine and track adjustments.
	 */
	u32				cfg_weight;
	u32				weight;
	u32				active;
	u32				inuse;

	u32				last_inuse;
	s64				saved_margin;

	sector_t			cursor;		/* to detect randio */

	/*
	 * `vtime` is this iocg's vtime cursor which progresses as IOs are
	 * issued.  If lagging behind device vtime, the delta represents
	 * the currently available IO budget.  If running ahead, the
	 * overage.
	 *
	 * `vtime_done` is the same but progressed on completion rather
	 * than issue.  The delta behind `vtime` represents the cost of
	 * currently in-flight IOs.
	 */
	atomic64_t			vtime;
	atomic64_t			done_vtime;
	u64				abs_vdebt;

	/* current delay in effect and when it started */
	u64				delay;
	u64				delay_at;

	/*
	 * The period this iocg was last active in.  Used for deactivation
	 * and invalidating `vtime`.
	 */
	atomic64_t			active_period;
	struct list_head		active_list;	/* ioc->active_iocgs: NVMe 제출 중인 cgroup 연결 */

	/* see __propagate_weights() and current_hweight() for details */
	u64				child_active_sum;
	u64				child_inuse_sum;
	u64				child_adjusted_sum;
	int				hweight_gen;
	u32				hweight_active;
	u32				hweight_inuse;
	u32				hweight_donating;
	u32				hweight_after_donation;

	struct list_head		walk_list;
	struct list_head		surplus_list;

	struct wait_queue_head		waitq;	/* NVMe 예산 부족 bio 대기열 */
	struct hrtimer			waitq_timer;	/* 예산 회복 시 NVMe 제출 재개 타이머 */

	/* timestamp at the latest activation */
	u64				activated_at;

	/* statistics */
	struct iocg_pcpu_stat __percpu	*pcpu_stat;
	struct iocg_stat		stat;
	struct iocg_stat		last_stat;
	u64				last_stat_abs_vusage;
	u64				usage_delta_us;
	u64				wait_since;
	u64				indebt_since;
	u64				indelay_since;

	/* this iocg's depth in the hierarchy and ancestors including self */
	int				level;
	struct ioc_gq			*ancestors[];
};

/*
 * ioc_cgrp: cgroup별 기본 가중치. 하위 NVMe 장치들에 대해 설정값이
 * 없을 때 사용되는 기본 cgroup weight를 저장한다.
 */
/* per cgroup */
struct ioc_cgrp {
	struct blkcg_policy_data	cpd;
	unsigned int			dfl_weight;
};

/*
 * ioc_now: 한 시점의 wallclock/가상 시간 스냅샷. NVMe 타이머 주기에서
 * 동일한 시점을 여러 경로에서 일관되게 참조하기 위해 사용.
 */
struct ioc_now {
	u64				now_ns;
	u64				now;
	u64				vnow;
};

/*
 * iocg_wait: 예산 부족으로 대기 중인 bio 하나. waitq에 연결되며
 * iocg_wake_fn()에서 깨어날 때 abs_cost를 현재 hweight_inuse로 환산해
 * NVMe 제출 예산에서 차감한다.
 */
struct iocg_wait {
	struct wait_queue_entry		wait;
	struct bio			*bio;
	u64				abs_cost;
	bool				committed;
};

/*
 * iocg_wake_ctx: waitq 깨우기 시 사용하는 임시 컨텍스트.
 * 현재 사용 가능한 vbudget과 hweight_inuse를 전달해, 깨어날 bio가
 * NVMe 제출에 필요한 cost를 초과하지 않는지 검사한다.
 */
struct iocg_wake_ctx {
	struct ioc_gq			*iocg;
	u32				hw_inuse;
	s64				vbudget;
};

static const struct ioc_params autop[] = {
	[AUTOP_HDD] = {
		.qos				= {
			[QOS_RLAT]		=        250000, /* 250ms */
			[QOS_WLAT]		=        250000,
			[QOS_MIN]		= VRATE_MIN_PPM,
			[QOS_MAX]		= VRATE_MAX_PPM,
		},
		.i_lcoefs			= {
			[I_LCOEF_RBPS]		=     174019176,
			[I_LCOEF_RSEQIOPS]	=         41708,
			[I_LCOEF_RRANDIOPS]	=           370,
			[I_LCOEF_WBPS]		=     178075866,
			[I_LCOEF_WSEQIOPS]	=         42705,
			[I_LCOEF_WRANDIOPS]	=           378,
		},
	},
	[AUTOP_SSD_QD1] = {
		.qos				= {
			[QOS_RLAT]		=         25000, /* 25ms */
			[QOS_WLAT]		=         25000,
			[QOS_MIN]		= VRATE_MIN_PPM,
			[QOS_MAX]		= VRATE_MAX_PPM,
		},
		.i_lcoefs			= {
			[I_LCOEF_RBPS]		=     245855193,
			[I_LCOEF_RSEQIOPS]	=         61575,
			[I_LCOEF_RRANDIOPS]	=          6946,
			[I_LCOEF_WBPS]		=     141365009,
			[I_LCOEF_WSEQIOPS]	=         33716,
			[I_LCOEF_WRANDIOPS]	=         26796,
		},
	},
	[AUTOP_SSD_DFL] = {
		.qos				= {
			[QOS_RLAT]		=         25000, /* 25ms */
			[QOS_WLAT]		=         25000,
			[QOS_MIN]		= VRATE_MIN_PPM,
			[QOS_MAX]		= VRATE_MAX_PPM,
		},
		.i_lcoefs			= {
			[I_LCOEF_RBPS]		=     488636629,
			[I_LCOEF_RSEQIOPS]	=          8932,
			[I_LCOEF_RRANDIOPS]	=          8518,
			[I_LCOEF_WBPS]		=     427891549,
			[I_LCOEF_WSEQIOPS]	=         28755,
			[I_LCOEF_WRANDIOPS]	=         21940,
		},
		.too_fast_vrate_pct		=           500,
	},
	[AUTOP_SSD_FAST] = {
		.qos				= {
			[QOS_RLAT]		=          5000, /* 5ms */
			[QOS_WLAT]		=          5000,
			[QOS_MIN]		= VRATE_MIN_PPM,
			[QOS_MAX]		= VRATE_MAX_PPM,
		},
		.i_lcoefs			= {
			[I_LCOEF_RBPS]		=    3102524156LLU,
			[I_LCOEF_RSEQIOPS]	=        724816,
			[I_LCOEF_RRANDIOPS]	=        778122,
			[I_LCOEF_WBPS]		=    1742780862LLU,
			[I_LCOEF_WSEQIOPS]	=        425702,
			[I_LCOEF_WRANDIOPS]	=	 443193,
		},
		.too_slow_vrate_pct		=            10,
	},
};

/*
 * vrate adjust percentages indexed by ioc->busy_level.  We adjust up on
 * vtime credit shortage and down on device saturation.
 */
static const u32 vrate_adj_pct[] =
	{ 0, 0, 0, 0,
	  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	  4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 16 };

static struct blkcg_policy blkcg_policy_iocost;

/* accessors and helpers */
static struct ioc *rqos_to_ioc(struct rq_qos *rqos)
{
	return container_of(rqos, struct ioc, rqos);
}

static struct ioc *q_to_ioc(struct request_queue *q)
{
	return rqos_to_ioc(rq_qos_id(q, RQ_QOS_COST));
}

static const char __maybe_unused *ioc_name(struct ioc *ioc)
{
	struct gendisk *disk = ioc->rqos.disk;

	if (!disk)
		return "<unknown>";
	return disk->disk_name;
}

static struct ioc_gq *pd_to_iocg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct ioc_gq, pd) : NULL;
}

static struct ioc_gq *blkg_to_iocg(struct blkcg_gq *blkg)
{
	return pd_to_iocg(blkg_to_pd(blkg, &blkcg_policy_iocost));
}

static struct blkcg_gq *iocg_to_blkg(struct ioc_gq *iocg)
{
	return pd_to_blkg(&iocg->pd);
}

static struct ioc_cgrp *blkcg_to_iocc(struct blkcg *blkcg)
{
	return container_of(blkcg_to_cpd(blkcg, &blkcg_policy_iocost),
			    struct ioc_cgrp, cpd);
}

/*
 * Scale @abs_cost to the inverse of @hw_inuse.  The lower the hierarchical
 * weight, the more expensive each IO.  Must round up.
 */
static u64 abs_cost_to_cost(u64 abs_cost, u32 hw_inuse) /* hw_inuse가 낮을수록 동일 NVMe 명령 비용 증가 */
{
	return DIV64_U64_ROUND_UP(abs_cost * WEIGHT_ONE, hw_inuse);	/* hweight_inuse 반비례 NVMe cost 환산 */
}

/*
 * The inverse of abs_cost_to_cost().  Must round up.
 */
static u64 cost_to_abs_cost(u64 cost, u32 hw_inuse) /* vtime을 NVMe 절대 비용으로 역환산 */
{
	return DIV64_U64_ROUND_UP(cost * hw_inuse, WEIGHT_ONE);	/* hweight_inuse 비례 NVMe 절대 비용 복원 */
}

static void iocg_commit_bio(struct ioc_gq *iocg, struct bio *bio,
			    u64 abs_cost, u64 cost)
{
	struct iocg_pcpu_stat *gcs;

	bio->bi_iocost_cost = cost;	/* bio 단위 NVMe 비용 기록 → CQ 완료 시 done_vtime 차감 */
	atomic64_add(cost, &iocg->vtime);	/* atomic: 다중 CPU에서 NVMe 제출 경쟁 시에도 vtime 일관 */

	gcs = get_cpu_ptr(iocg->pcpu_stat);	/* 현재 CPU의 NVMe 사용량 통계 획득 */
	local64_add(abs_cost, &gcs->abs_vusage);	/* per-CPU local64: NVMe 사용량 누적, 캐시 일관성 최소화 */
	put_cpu_ptr(gcs);	/* preemption 복원: 다른 CPU로 이주 시에도 NVMe 통계 정확성 */
}

static void iocg_lock(struct ioc_gq *iocg, bool lock_ioc, unsigned long *flags)
{
	if (lock_ioc) {	/* debt 처리 시 ioc->lock + waitq.lock 중첩: NVMe 예산/부채 동시 변경 방지 */
		spin_lock_irqsave(&iocg->ioc->lock, *flags);	/* ioc 레벨 lock: vrate/weight/주기 보호 */
		spin_lock(&iocg->waitq.lock);	/* waitq lock: NVMe 예산 대기자 상태 보호 */
	} else {
		spin_lock_irqsave(&iocg->waitq.lock, *flags);
	}
}

static void iocg_unlock(struct ioc_gq *iocg, bool unlock_ioc, unsigned long *flags)
{
	if (unlock_ioc) {
		spin_unlock(&iocg->waitq.lock);
		spin_unlock_irqrestore(&iocg->ioc->lock, *flags);
	} else {
		spin_unlock_irqrestore(&iocg->waitq.lock, *flags);
	}
}

#define CREATE_TRACE_POINTS
#include <trace/events/iocost.h>

/*
 * ioc_refresh_margins - 주기와 vrate를 기준으로 cgroup 여유분 재계산
 *
 * NVMe 타이머 주기가 바뀌거나 vrate 조정 시, 각 cgroup이 device vtime
 * 대비 얼마나 뒤처질 수 있는지(min/low/target)를 갱신한다.
 */
static void ioc_refresh_margins(struct ioc *ioc)
{
	struct ioc_margins *margins = &ioc->margins;
	u32 period_us = ioc->period_us;	/* NVMe latency QoS에서 유도된 현재 제어 주기 */
	u64 vrate = ioc->vtime_base_rate;	/* 현재 NVMe IO 속도 보정값 */

	margins->min = (period_us * MARGIN_MIN_PCT / 100) * vrate;	/* NVMe SQ 포화 직전 최소 vtime 여유 */
	margins->low = (period_us * MARGIN_LOW_PCT / 100) * vrate;	/* NVMe 제출률 검토 임계 vtime */
	margins->target = (period_us * MARGIN_TARGET_PCT / 100) * vrate;	/* NVMe SQ/CQ 안정 목표 vtime 버퍼 */
}

/* latency Qos params changed, update period_us and all the dependent params */
/*
 * ioc_refresh_period_us - latency QoS에 따라 타이머 주기 설정
 *
 * NVMe 읽기/쓰기 목표 완료 지연(QOS_RLAT/WLAT) 중 큰 쪽을 기준으로
 * ioc 타이머 주기(period_us)를 산출한다. 주기는 너무 짧으면 제어가
 * 불안정하고, 너무 길면 NVMe 포화 반응이 늦어진다.
 */
static void ioc_refresh_period_us(struct ioc *ioc)
{
	u32 ppm, lat, multi, period_us;

	lockdep_assert_held(&ioc->lock);

	/* pick the higher latency target */
	/* NVMe read/write 중 느린 쪽이 병목 결정 */
	if (ioc->params.qos[QOS_RLAT] >= ioc->params.qos[QOS_WLAT]) {	/* NVMe read QoS가 write보다 느리면 read 기준 */
		ppm = ioc->params.qos[QOS_RPPM];	/* read latency QoS 백분위수 */
		lat = ioc->params.qos[QOS_RLAT];	/* read latency 목표(μs): NVMe CQ ISR 처리 목표 */
	} else {
		ppm = ioc->params.qos[QOS_WPPM];	/* write latency QoS 백분위수 */
		lat = ioc->params.qos[QOS_WLAT];	/* write latency 목표(μs): NVMe CQ ISR 처리 목표 */
	}

	/*
	 * We want the period to be long enough to contain a healthy number
	 * of IOs while short enough for granular control.  Define it as a
	 * multiple of the latency target.  Ideally, the multiplier should
	 * be scaled according to the percentile so that it would nominally
	 * contain a certain number of requests.  Let's be simpler and
	 * scale it linearly so that it's 2x >= pct(90) and 10x at pct(50).
	 */
	if (ppm)
		multi = max_t(u32, (MILLION - ppm) / 50000, 2);	/* 백분위수가 낮을수록(예: p50) NVMe 샘플 주기를 길게 */
	else
		multi = 2;
	period_us = multi * lat;	/* NVMe 완료 지연의 배수로 타이머 주기 산출 */
	period_us = clamp_t(u32, period_us, MIN_PERIOD, MAX_PERIOD);	/* NVMe 제어 반응성/안정성 균형 */

	/* calculate dependent params */
	ioc->period_us = period_us;
	ioc->timer_slack_ns = div64_u64(	/* waitq 타이머 batching: NVMe doorbell storm 방지용 여유(추정) */
		(u64)period_us * NSEC_PER_USEC * TIMER_SLACK_PCT,
		100);
	ioc_refresh_margins(ioc);
}

/*
 *  ioc->rqos.disk isn't initialized when this function is called from
 *  the init path.
 */
/*
 * ioc_autop_idx - 디바이스 특성에 맞는 자동 파라미터 프로파일 선택
 *
 * blk_queue_rot()으로 HDD를 감지하고, blk_queue_depth()==1로
 * NCQ가 깨진 SATA SSD(QD1)를 구분한다. 일반 NVMe SSD는 AUTOP_SSD_DFL
 * 또는 AUTOP_SSD_FAST 중 vrate 추이에 따라 선택된다.
 */
static int ioc_autop_idx(struct ioc *ioc, struct gendisk *disk)
{
	int idx = ioc->autop_idx;
	const struct ioc_params *p = &autop[idx];
	u32 vrate_pct;
	u64 now_ns;

	/* rotational? */	/* blk_queue_rot: NVMe가 아닌 회전 미디어(HDD) 분기 → seek cost 모델 */
	if (blk_queue_rot(disk->queue))	/* NVMe 장치가 아닌 HDD면 AUTOP_HDD */
		return AUTOP_HDD;

	/* handle SATA SSDs w/ broken NCQ */	/* queue depth 1: NVMe SQ/CQ 깊이가 1인 edge case(추정) */
	if (blk_queue_depth(disk->queue) == 1)	/* NCQ 비활성화/깊이 1이면 AUTOP_SSD_QD1 */
		return AUTOP_SSD_QD1;

	/* use one of the normal ssd sets */
	if (idx < AUTOP_SSD_DFL)	/* 이전 프로파일이 HDD/QD1이었으면 기본 SSD 프로파일로 전이 */
		return AUTOP_SSD_DFL;

	/* if user is overriding anything, maintain what was there */
	if (ioc->user_qos_params || ioc->user_cost_model)	/* 사용자가 NVMe QoS/모델을 오버라이드하면 자동 전환 금지 */
		return idx;

	/* step up/down based on the vrate */
	vrate_pct = div64_u64(ioc->vtime_base_rate * 100, VTIME_PER_USEC);	/* NVMe에 대한 현재 상대 IO 속도(%) */
	now_ns = blk_time_get_ns();	/* NVMe CQ/타이머와 동일한 monotonic 시계 */

	if (p->too_fast_vrate_pct && p->too_fast_vrate_pct <= vrate_pct) {
		if (!ioc->autop_too_fast_at)
			ioc->autop_too_fast_at = now_ns;
		if (now_ns - ioc->autop_too_fast_at >= AUTOP_CYCLE_NSEC)
			return idx + 1;
	} else {
		ioc->autop_too_fast_at = 0;
	}

	if (p->too_slow_vrate_pct && p->too_slow_vrate_pct >= vrate_pct) {
		if (!ioc->autop_too_slow_at)
			ioc->autop_too_slow_at = now_ns;
		if (now_ns - ioc->autop_too_slow_at >= AUTOP_CYCLE_NSEC)
			return idx - 1;
	} else {
		ioc->autop_too_slow_at = 0;
	}

	return idx;
}

/*
 * Take the followings as input
 *
 *  @bps	maximum sequential throughput
 *  @seqiops	maximum sequential 4k iops
 *  @randiops	maximum random 4k iops
 *
 * and calculate the linear model cost coefficients.
 *
 *  *@page	per-page cost		1s / (@bps / 4096)
 *  *@seqio	base cost of a seq IO	max((1s / @seqiops) - *@page, 0)
 *  @randiops	base cost of a rand IO	max((1s / @randiops) - *@page, 0)
 */
static void calc_lcoefs(u64 bps, u64 seqiops, u64 randiops,
			u64 *page, u64 *seqio, u64 *randio)
{
	u64 v;

	*page = *seqio = *randio = 0;

	if (bps) {
		u64 bps_pages = DIV_ROUND_UP_ULL(bps, IOC_PAGE_SIZE);

		if (bps_pages)
			*page = DIV64_U64_ROUND_UP(VTIME_PER_SEC, bps_pages);
		else
			*page = 1;
	}

	if (seqiops) {
		v = DIV64_U64_ROUND_UP(VTIME_PER_SEC, seqiops);
		if (v > *page)
			*seqio = v - *page;
	}

	if (randiops) {
		v = DIV64_U64_ROUND_UP(VTIME_PER_SEC, randiops);
		if (v > *page)
			*randio = v - *page;
	}
}

static void ioc_refresh_lcoefs(struct ioc *ioc)
{
	u64 *u = ioc->params.i_lcoefs;
	u64 *c = ioc->params.lcoefs;

	calc_lcoefs(u[I_LCOEF_RBPS], u[I_LCOEF_RSEQIOPS], u[I_LCOEF_RRANDIOPS],	/* read 방향 NVMe seq/rand/대역폭 계수 */
		    &c[LCOEF_RPAGE], &c[LCOEF_RSEQIO], &c[LCOEF_RRANDIO]);
	calc_lcoefs(u[I_LCOEF_WBPS], u[I_LCOEF_WSEQIOPS], u[I_LCOEF_WRANDIOPS],	/* write 방향 NVMe seq/rand/대역폭 계수 */
		    &c[LCOEF_WPAGE], &c[LCOEF_WSEQIO], &c[LCOEF_WRANDIO]);
}

/*
 * struct gendisk is required as an argument because ioc->rqos.disk
 * is not properly initialized when called from the init path.
 */
/*
 * ioc_refresh_params_disk - NVMe 장치에 맞는 파라미터 프로파일 적용
 *
 * ioc_autop_idx()가 반환한 프로파일에 따라 qos(latency QoS)와
 * i_lcoefs(입력 지표)를 복사하고, 이를 바탕으로 period_us, lcoefs,
 * vrate_min/max를 갱신한다. NVMe 큐 깊이나 장치 성능 변화에 대응.
 */
static bool ioc_refresh_params_disk(struct ioc *ioc, bool force,
				    struct gendisk *disk)
{
	const struct ioc_params *p;
	int idx;

	lockdep_assert_held(&ioc->lock);

	idx = ioc_autop_idx(ioc, disk);
	p = &autop[idx];

	if (idx == ioc->autop_idx && !force)	/* NVMe 프로파일 변경 없으면 skip */
		return false;	/* NVMe 제출 억제 상태 유지 */

	if (idx != ioc->autop_idx) {	/* NVMe 장치 프로파일 전환 시 vrate 리셋 */
		atomic64_set(&ioc->vtime_rate, VTIME_PER_USEC);	/* atomic: 새 프로파일의 NVMe 기준 속도로 갱신 */
		ioc->vtime_base_rate = VTIME_PER_USEC;
	}

	ioc->autop_idx = idx;
	ioc->autop_too_fast_at = 0;
	ioc->autop_too_slow_at = 0;

	if (!ioc->user_qos_params)	/* 사용자 미지정 시 자동 NVMe latency QoS 적용 */
		memcpy(ioc->params.qos, p->qos, sizeof(p->qos));
	if (!ioc->user_cost_model)	/* 사용자 미지정 시 자동 NVMe 비용 계수 적용 */
		memcpy(ioc->params.i_lcoefs, p->i_lcoefs, sizeof(p->i_lcoefs));

	ioc_refresh_period_us(ioc);
	ioc_refresh_lcoefs(ioc);

	ioc->vrate_min = DIV64_U64_ROUND_UP((u64)ioc->params.qos[QOS_MIN] *	/* NVMe 최소 vrate 절대값 */
					    VTIME_PER_USEC, MILLION);
	ioc->vrate_max = DIV64_U64_ROUND_UP((u64)ioc->params.qos[QOS_MAX] *	/* NVMe 최대 vrate 절대값 */
					    VTIME_PER_USEC, MILLION);

	return true;
}

static bool ioc_refresh_params(struct ioc *ioc, bool force)
{
	return ioc_refresh_params_disk(ioc, force, ioc->rqos.disk);
}

/*
 * When an iocg accumulates too much vtime or gets deactivated, we throw away
 * some vtime, which lowers the overall device utilization. As the exact amount
 * which is being thrown away is known, we can compensate by accelerating the
 * vrate accordingly so that the extra vtime generated in the current period
 * matches what got lost.
 */
/*
 * ioc_refresh_vrate - vtime 오차를 보정하는 현재 주기 vrate 설정
 *
 * cgroup 비활성화 등으로 버려진 vtime 예산(vtime_err)을 현재 주기의
 * 남은 시간 안에서 보정할 수 있도록 vtime_rate를 조정한다.
 */
static void ioc_refresh_vrate(struct ioc *ioc, struct ioc_now *now)
{
	s64 pleft = ioc->period_at + ioc->period_us - now->now;	/* 현재 NVMe 주기 잔여 시간 */
	s64 vperiod = ioc->period_us * ioc->vtime_base_rate;	/* 현재 주기의 NVMe 가상 시간 총량 */
	s64 vcomp, vcomp_min, vcomp_max;

	lockdep_assert_held(&ioc->lock);

	/* we need some time left in this period */
	if (pleft <= 0)	/* 주기가 이미 끝났으면 vrate 보정 불가 */
		goto done;

	/*
	 * Calculate how much vrate should be adjusted to offset the error.
	 * Limit the amount of adjustment and deduct the adjusted amount from
	 * the error.
	 */
	vcomp = -div64_s64(ioc->vtime_err, pleft);	/* 남은 주기 안에 NVMe 예산 오차를 상쇄할 vrate 보정량 */
	vcomp_min = -(ioc->vtime_base_rate >> 1);
	vcomp_max = ioc->vtime_base_rate;
	vcomp = clamp(vcomp, vcomp_min, vcomp_max);

	ioc->vtime_err += vcomp * pleft;

	atomic64_set(&ioc->vtime_rate, ioc->vtime_base_rate + vcomp);	/* atomic: NVMe 제출 경로가 읽는 vrate 갱신 */
done:
	/* bound how much error can accumulate */
	ioc->vtime_err = clamp(ioc->vtime_err, -vperiod, vperiod);
}

/*
 * ioc_adjust_base_vrate - 장치 포화도에 따른 기본 vrate 조정
 *
 * NVMe rq_wait_pct(큐 대기 비율)와 latency QoS 미달 비율(missed_ppm)
 * 에 따라 busy_level을 상승/하강시키고, 이에 비례해 vrate를 조정한다.
 * 장치가 포화되면 vrate를 낮추고, 여유 있으면 높인다.
 */
static void ioc_adjust_base_vrate(struct ioc *ioc, u32 rq_wait_pct,
				  int nr_lagging, int nr_shortages,
				  int prev_busy_level, u32 *missed_ppm)
{
	u64 vrate = ioc->vtime_base_rate;
	u64 vrate_min = ioc->vrate_min, vrate_max = ioc->vrate_max;

	if (!ioc->busy_level || (ioc->busy_level < 0 && nr_lagging)) {	/* NVMe 포화/지연 상태가 명확하지 않으면 vrate 유지 */
		if (ioc->busy_level != prev_busy_level || nr_lagging)
			trace_iocost_ioc_vrate_adj(ioc, vrate,
						   missed_ppm, rq_wait_pct,
						   nr_lagging, nr_shortages);

		return;
	}

	/*
	 * If vrate is out of bounds, apply clamp gradually as the
	 * bounds can change abruptly.  Otherwise, apply busy_level
	 * based adjustment.
	 */
	if (vrate < vrate_min) {	/* NVMe vrate가 사용자 하한 아래면 점진 상승 */
		vrate = div64_u64(vrate * (100 + VRATE_CLAMP_ADJ_PCT), 100);
		vrate = min(vrate, vrate_min);
	} else if (vrate > vrate_max) {	/* NVMe vrate가 사용자 상한 위면 점진 하강 */
		vrate = div64_u64(vrate * (100 - VRATE_CLAMP_ADJ_PCT), 100);
		vrate = max(vrate, vrate_max);
	} else {
		int idx = min_t(int, abs(ioc->busy_level),	/* busy_level에 따른 NVMe vrate 조정폭 인덱스 */
				ARRAY_SIZE(vrate_adj_pct) - 1);
		u32 adj_pct = vrate_adj_pct[idx];

		if (ioc->busy_level > 0)	/* NVMe 장치 포화 → vrate 감소(쓰로틀 강화) */
			adj_pct = 100 - adj_pct;	/* busy_level > 0이면 NVMe 제출률을 adj_pct만큼 감소 */
		else
			adj_pct = 100 + adj_pct;	/* busy_level < 0이면 NVMe 제출률을 adj_pct만큼 증가 */

		vrate = clamp(DIV64_U64_ROUND_UP(vrate * adj_pct, 100),
			      vrate_min, vrate_max);
	}

	trace_iocost_ioc_vrate_adj(ioc, vrate, missed_ppm, rq_wait_pct,
				   nr_lagging, nr_shortages);

	ioc->vtime_base_rate = vrate;
	ioc_refresh_margins(ioc);
}

/* take a snapshot of the current [v]time and vrate */
/*
 * ioc_now - 현재 시점의 wallclock/가상 시간 스냅샷 획득
 *
 * seqcount로 보호되는 period_at/period_at_vtime을 읽어,
 * 현재 device vtime(vnow)을 계산한다. NVMe 주기 타이머와 waitq 타이머
 * 모두에서 일관된 시간 기준으로 사용된다.
 */
static void ioc_now(struct ioc *ioc, struct ioc_now *now)
{
	unsigned seq;
	u64 vrate;

	now->now_ns = blk_time_get_ns();	/* NVMe CQ/타이머와 동일한 monotonic ns 시계 */
	now->now = ktime_to_us(now->now_ns);	/* iocost 남은 주기 계산용 μs */
	vrate = atomic64_read(&ioc->vtime_rate);	/* atomic: NVMe 제출률 보정값 읽기 */

	/*
	 * The current vtime is
	 *
	 *   vtime at period start + (wallclock time since the start) * vrate
	 *
	 * As a consistent snapshot of `period_at_vtime` and `period_at` is
	 * needed, they're seqcount protected.
	 */
	do {	/* NVMe 예산 내로 bio가 들어올 때까지 inuse 증가 */
		seq = read_seqcount_begin(&ioc->period_seqcount);	/* seqcount: NVMe 주기 경계와 vtime 일관성 확보 */
		now->vnow = ioc->period_at_vtime +	/* 주기 시작 vtime + 경과 wallclock * vrate */
			(now->now - ioc->period_at) * vrate;	/* 현재 device vtime: NVMe에 이론적으로 허용된 IO 비용 */
	} while (read_seqcount_retry(&ioc->period_seqcount, seq));	/* seqcount retry: 타이머가 period_at을 갱신한 경우 재시도 */
}

/*
 * ioc_start_period - 새로운 ioc 주기 시작 및 타이머 재설정
 */
static void ioc_start_period(struct ioc *ioc, struct ioc_now *now)
{
	WARN_ON_ONCE(ioc->running != IOC_RUNNING);	/* 타이머는 NVMe 활성 상태에서만 동작해야 함 */

	write_seqcount_begin(&ioc->period_seqcount);	/* seqcount: NVMe 주기 경계 쓰기 시작 */
	ioc->period_at = now->now;
	ioc->period_at_vtime = now->vnow;
	write_seqcount_end(&ioc->period_seqcount);

	ioc->timer.expires = jiffies + usecs_to_jiffies(ioc->period_us);	/* 다음 NVMe 피드백 주기 타이머 설정 */
	add_timer(&ioc->timer);	/* ioc 타이머 재시작 → NVMe CQ 지연 주기적 측정 */
}

/*
 * Update @iocg's `active` and `inuse` to @active and @inuse, update level
 * weight sums and propagate upwards accordingly. If @save, the current margin
 * is saved to be used as reference for later inuse in-period adjustments.
 */
/*
 * __propagate_weights - cgroup의 active/inuse 변경을 상위로 전파
 *
 * 하위 cgroup의 가중치 변경이 상위의 child_active_sum, child_inuse_sum
 * 에 반영되도록 거슬러 올라가며, hweight_inuse 재계산을 위한 플래그를
 * 설정한다. NVMe 시간을 여러 cgroup에 계층적으로 분배하는 핵심 로직.
 */
static void __propagate_weights(struct ioc_gq *iocg, u32 active, u32 inuse,
				bool save, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	int lvl;

	lockdep_assert_held(&ioc->lock);

	/*
	 * For an active leaf node, its inuse shouldn't be zero or exceed
	 * @active. An active internal node's inuse is solely determined by the
	 * inuse to active ratio of its children regardless of @inuse.
	 */
	if (list_empty(&iocg->active_list) && iocg->child_active_sum) {	/* 낮부 node가 자식을 통해 NVMe IO 활성 */
		inuse = DIV64_U64_ROUND_UP(active * iocg->child_inuse_sum,	/* 자식 cgroup의 NVMe 사용 비율에 맞춤 */
					   iocg->child_active_sum);	/* 자식 cgroup의 NVMe 활성 비율로 정규화 */
	} else {
		/*
		 * It may be tempting to turn this into a clamp expression with
		 * a lower limit of 1 but active may be 0, which cannot be used
		 * as an upper limit in that situation. This expression allows
		 * active to clamp inuse unless it is 0, in which case inuse
		 * becomes 1.
		 */
		inuse = min(inuse, active) ?: 1;	/* NVMe 시간 가중치는 최소 1 유지 */
	}

	iocg->last_inuse = iocg->inuse;	/* 잉여 복원 시 참조할 이전 NVMe 사용 가중치 */
	if (save)	/* inuse 변경 시점의 vtime margin 저장: 이후 NVMe 예산 회복 기준 */
		iocg->saved_margin = now->vnow - atomic64_read(&iocg->vtime);	/* atomic: 현재 NVMe 예산 여분 스냅샷 */

	if (active == iocg->active && inuse == iocg->inuse)	/* NVMe 가중치 변화 없으면 상위 전파 skip */
		return;

	for (lvl = iocg->level - 1; lvl >= 0; lvl--) {	/* cgroup 계층을 따라 NVMe 가중치 상위 전파 */
		struct ioc_gq *parent = iocg->ancestors[lvl];
		struct ioc_gq *child = iocg->ancestors[lvl + 1];
		u32 parent_active = 0, parent_inuse = 0;

		/* update the level sums */
		parent->child_active_sum += (s32)(active - child->active);	/* 형제 cgroup 간 NVMe 활성 합 갱신 */
		parent->child_inuse_sum += (s32)(inuse - child->inuse);	/* 형제 cgroup 간 NVMe 사용 합 갱신 */
		/* apply the updates */
		child->active = active;	/* 해당 레벨의 NVMe 활성 가중치 확정 */
		child->inuse = inuse;	/* 해당 레벨의 NVMe 사용 가중치 확정 */

		/*
		 * The delta between inuse and active sums indicates that
		 * much of weight is being given away.  Parent's inuse
		 * and active should reflect the ratio.
		 */
		if (parent->child_active_sum) {		/* 부모에 활성 자식이 있을 때만 NVMe 비율 재계산 */
			parent_active = parent->weight;
			parent_inuse = DIV64_U64_ROUND_UP(
				parent_active * parent->child_inuse_sum,
				parent->child_active_sum);				/* 형제 active 합으로 정규화 */
		}

		/* do we need to keep walking up? */
		if (parent_active == parent->active &&
		    parent_inuse == parent->inuse)
			break;			/* blk-mq/NVMe 제출 진행 */

		active = parent_active;
		inuse = parent_inuse;
	}

	ioc->weights_updated = true;
}

static void commit_weights(struct ioc *ioc)
{
	lockdep_assert_held(&ioc->lock);

	if (ioc->weights_updated) {	/* NVMe cgroup 가중치 변경 시 hweight 캐시 무효화 */
		/* paired with rmb in current_hweight(), see there */		/* commit_weights() smp_wmb와 짝: NVMe hweight 캐시 일관성 */
		smp_wmb();		/* weight 갱신 값이 hweight_gen 증가 전에 관측되도록: NVMe 예산 계산 race 방지 */
		atomic_inc(&ioc->hweight_gen);		/* atomic: 모든 CPU의 NVMe hweight 캐시 재계산 유도 */
		ioc->weights_updated = false;
	}
}

static void propagate_weights(struct ioc_gq *iocg, u32 active, u32 inuse,
			      bool save, struct ioc_now *now)
{
	__propagate_weights(iocg, active, inuse, save, now);
	commit_weights(iocg->ioc);
}

/*
 * current_hweight - 현재 cgroup의 계층적 가중치 캐시 계산
 *
 * 조상들의 active/inuse 합계를 따라 내려가며, 이 cgroup이 전체 NVMe
 * 시간 중 어느 비율을 차지하는지(hweight_active/hweight_inuse)를
 * 계산한다. 성능을 위해 hweight_gen으로 캐시 무효화를 판단한다.
 */
static void current_hweight(struct ioc_gq *iocg, u32 *hw_activep, u32 *hw_inusep)
{
	struct ioc *ioc = iocg->ioc;
	int lvl;
	u32 hwa, hwi;
	int ioc_gen;

	/* hot path - if uptodate, use cached */
	ioc_gen = atomic_read(&ioc->hweight_gen);	/* atomic: NVMe 가중치 세대 번호 읽기 */
	if (ioc_gen == iocg->hweight_gen)	/* 캐시된 NVMe 할당 비율이 최신이면 재사용 */
		goto out;	/* hot path: NVMe 예산 계산 부하 감소 */

	/*
	 * Paired with wmb in commit_weights(). If we saw the updated
	 * hweight_gen, all the weight updates from __propagate_weights() are
	 * visible too.
	 *
	 * We can race with weight updates during calculation and get it
	 * wrong.  However, hweight_gen would have changed and a future
	 * reader will recalculate and we're guaranteed to discard the
	 * wrong result soon.
	 */
	smp_rmb();	/* hweight_gen 증가 후 weight 값이 NVMe 예산 경로에 보이도록 */

	hwa = hwi = WEIGHT_ONE;
	for (lvl = 0; lvl <= iocg->level - 1; lvl++) {	/* root에서 leaf까지 낮아가며 NVMe 할당 비율 누적 */
		struct ioc_gq *parent = iocg->ancestors[lvl];
		struct ioc_gq *child = iocg->ancestors[lvl + 1];
		u64 active_sum = READ_ONCE(parent->child_active_sum);	/* READ_ONCE: race 중 NVMe 가중치 합 일관성 */
		u64 inuse_sum = READ_ONCE(parent->child_inuse_sum);	/* READ_ONCE: race 중 NVMe 사용 합 일관성 */
		u32 active = READ_ONCE(child->active);	/* READ_ONCE: race 중 자식 활성 가중치 읽기 */
		u32 inuse = READ_ONCE(child->inuse);	/* READ_ONCE: race 중 자식 사용 가중치 읽기 */

		/* we can race with deactivations and either may read as zero */
		if (!active_sum || !inuse_sum)		/* 비활성화 race 시 NVMe 비율 계산 skip */
			continue;			/* 다음 레벨로: 잘못된 NVMe hweight 방지 */

		active_sum = max_t(u64, active, active_sum);		/* 자식 active가 합보다 크면 큰 값으로 NVMe 비율 보정 */
		hwa = div64_u64((u64)hwa * active, active_sum);		/* 부모로부터 상속된 NVMe 활성 비율 */

		inuse_sum = max_t(u64, inuse, inuse_sum);		/* 자식 inuse가 합보다 크면 큰 값으로 NVMe 비율 보정 */
		hwi = div64_u64((u64)hwi * inuse, inuse_sum);		/* 부모로부터 상속된 NVMe 사용 비율 */
	}

	iocg->hweight_active = max_t(u32, hwa, 1);	/* NVMe 활성 비율은 최소 1 유지 */
	iocg->hweight_inuse = max_t(u32, hwi, 1);	/* NVMe 사용 비율은 최소 1 유지 → cost 유한 */
	iocg->hweight_gen = ioc_gen;	/* 캐시된 NVMe 가중치 세대 동기화 */
out:
	if (hw_activep)
		*hw_activep = iocg->hweight_active;
	if (hw_inusep)
		*hw_inusep = iocg->hweight_inuse;
}

/*
 * Calculate the hweight_inuse @iocg would get with max @inuse assuming all the
 * other weights stay unchanged.
 */
static u32 current_hweight_max(struct ioc_gq *iocg)
{
	u32 hwm = WEIGHT_ONE;
	u32 inuse = iocg->active;
	u64 child_inuse_sum;
	int lvl;

	lockdep_assert_held(&iocg->ioc->lock);

	for (lvl = iocg->level - 1; lvl >= 0; lvl--) {
		struct ioc_gq *parent = iocg->ancestors[lvl];
		struct ioc_gq *child = iocg->ancestors[lvl + 1];

		child_inuse_sum = parent->child_inuse_sum + inuse - child->inuse;
		hwm = div64_u64((u64)hwm * inuse, child_inuse_sum);
		inuse = DIV64_U64_ROUND_UP(parent->active * child_inuse_sum,
					   parent->child_active_sum);
	}

	return max_t(u32, hwm, 1);
}

static void weight_updated(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	struct blkcg_gq *blkg = iocg_to_blkg(iocg);
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkg->blkcg);
	u32 weight;

	lockdep_assert_held(&ioc->lock);

	weight = iocg->cfg_weight ?: iocc->dfl_weight;
	if (weight != iocg->weight && iocg->active)
		propagate_weights(iocg, weight, iocg->inuse, true, now);
	iocg->weight = weight;
}

/*
 * iocg_activate - cgroup을 active 목록에 등록하고 예산 초기화
 *
 * bio가 처음 제출되거나 주기 경계에서 다시 활성화될 때 호출된다.
 * ioc->active_iocgs에 추가하고, vtime을 target margin만큼 뒤로 설정해
 * NVMe 제출을 위한 초기 예산을 부여한다. 첫 활성화 시 ioc 타이머를
 * 시작한다.
 */
static bool iocg_activate(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	u64 __maybe_unused last_period, cur_period;
	u64 vtime, vtarget;
	int i;

	/*
	 * If seem to be already active, just update the stamp to tell the
	 * timer that we're still active.  We don't mind occassional races.
	 */
	if (!list_empty(&iocg->active_list)) {	/* blkg 소멸 시 NVMe 활성 상태면 정리 */
		ioc_now(ioc, now);	/* 현재 NVMe 시계 갱신 */
		cur_period = atomic64_read(&ioc->cur_period);	/* atomic: 현재 NVMe 제어 주기 */
		if (atomic64_read(&iocg->active_period) != cur_period)	/* atomic: 주기가 바뀌면 활성 스탬프 갱신 */
			atomic64_set(&iocg->active_period, cur_period);		/* atomic: 이번 주기 NVMe 제출 기록 */
		return true;		/* NVMe 제출이 지연 상태로 들어감 */
	}

	/* racy check on internal node IOs, treat as root level IOs */	/* NVMe 제출이 낮부 node에 직접 도달하면 root로 취급(추정) */
	if (iocg->child_active_sum)	/* 자식이 NVMe IO 중이면 leaf 활성화 불가 */
		return false;

	spin_lock_irq(&ioc->lock);	/* NVMe 활성화/주기 시작 동기화 */

	ioc_now(ioc, now);

	/* update period */
	cur_period = atomic64_read(&ioc->cur_period);	/* atomic: 다시 NVMe 주기 확인 */
	last_period = atomic64_read(&iocg->active_period);	/* atomic: 마지막 NVMe 활성 주기 */
	atomic64_set(&iocg->active_period, cur_period);	/* atomic: 이번 주기 NVMe 활성화 기록 */

	/* already activated or breaking leaf-only constraint? */
	if (!list_empty(&iocg->active_list))
		goto succeed_unlock;
	for (i = iocg->level - 1; i > 0; i--)	/* 조상 중 낮부 node가 NVMe 활성이면 leaf 활성화 불가 */
		if (!list_empty(&iocg->ancestors[i]->active_list))
			goto fail_unlock;		/* activate 실패: NVMe 예산은 자식에게 양도 */

	if (iocg->child_active_sum)
		goto fail_unlock;

	/*
	 * Always start with the target budget. On deactivation, we throw away
	 * anything above it.
	 */
	vtarget = now->vnow - ioc->margins.target;	/* NVMe 제출을 위한 초기 예산 버퍼 설정 */
	vtime = atomic64_read(&iocg->vtime);	/* atomic: 현재 cgroup의 NVMe 누적 vtime */

	atomic64_add(vtarget - vtime, &iocg->vtime);	/* atomic: issued vtime을 target margin으로 리셋 */
	atomic64_add(vtarget - vtime, &iocg->done_vtime);	/* atomic: completed vtime 동일폭 조정 → in-flight 0 유지 */
	vtime = vtarget;	/* NVMe 예산 버퍼 설정 완료 */

	/*
	 * Activate, propagate weight and start period timer if not
	 * running.  Reset hweight_gen to avoid accidental match from
	 * wrapping.
	 */
	iocg->hweight_gen = atomic_read(&ioc->hweight_gen) - 1;	/* atomic: 강제 hweight 재계산 유도 */
	list_add(&iocg->active_list, &ioc->active_iocgs);	/* NVMe IO 활성 cgroup 목록에 등록 */

	propagate_weights(iocg, iocg->weight,
			  iocg->last_inuse ?: iocg->weight, true, now);

	TRACE_IOCG_PATH(iocg_activate, iocg, now,
			last_period, cur_period, vtime);

	iocg->activated_at = now->now;

	if (ioc->running == IOC_IDLE) {	/* 첫 NVMe IO 발생 시 타이머 시작 */
		ioc->running = IOC_RUNNING;		/* NVMe 제어 활성 상태로 전이 */
		ioc->dfgv_period_at = now->now;		/* NVMe idle/부채 측정 기준 시점 */
		ioc->dfgv_period_rem = 0;
		ioc_start_period(ioc, now);		/* 첫 NVMe 주기 타이머 시작 */
	}

succeed_unlock:
	spin_unlock_irq(&ioc->lock);	/* NVMe 주기 조정 완료 */
	return true;

fail_unlock:
	spin_unlock_irq(&ioc->lock);
	return false;
}

/*
 * iocg_kick_delay - 예산 초과로 인한 cgroup 지연(delay) 갱신
 *
 * cgroup의 vtime이 device vtime보다 너무 앞서면, blkcg의 use_delay
 * 메커니즘을 통해 추가 IO 발행을 억제한다. 지연은 시간이 지남에 따라
 * 절반씩 감소(exponential decay)한다.
 */
static bool iocg_kick_delay(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	struct blkcg_gq *blkg = iocg_to_blkg(iocg);
	u64 tdelta, delay, new_delay, shift;
	s64 vover, vover_pct;
	u32 hwa;

	lockdep_assert_held(&iocg->waitq.lock);

	/*
	 * If the delay is set by another CPU, we may be in the past. No need to
	 * change anything if so. This avoids decay calculation underflow.
	 */
	if (time_before64(now->now, iocg->delay_at))	/* 다른 CPU가 설정한 blkcg use_delay가 아직 유효 */
		return false;

	/* calculate the current delay in effect - 1/2 every second */
	tdelta = now->now - iocg->delay_at;	/* 지연 경과 시간: NVMe doorbell 간격 완화 시간 감소 */
	shift = div64_u64(tdelta, USEC_PER_SEC);	/* 1초마다 지연 절반 감소 */
	if (iocg->delay && shift < BITS_PER_LONG)	/* 지연 값이 있고 오버플로우 안 된 경우 */
		delay = iocg->delay >> shift;		/* exponential decay: NVMe 제출 재개 점진적 */
	else
		delay = 0;

	/* calculate the new delay from the debt amount */
	current_hweight(iocg, &hwa, NULL);	/* 현재 NVMe 활성 비율로 부채 환산 */
	vover = atomic64_read(&iocg->vtime) +	/* atomic: 현재 NVMe issued vtime */
		abs_cost_to_cost(iocg->abs_vdebt, hwa) - now->vnow;
	vover_pct = div64_s64(100 * vover,	/* NVMe 예산 초과 비율(%) */
			      ioc->period_us * ioc->vtime_base_rate);

	if (vover_pct <= MIN_DELAY_THR_PCT)	/* NVMe 예산 초과가 미미하면 지연 해제 */
		new_delay = 0;		/* NVMe 제출 억제 해제 */
	else if (vover_pct >= MAX_DELAY_THR_PCT)	/* NVMe 예산 심각 초과 시 최대 지연 */
		new_delay = MAX_DELAY;		/* 최대 250ms NVMe 제출 봉쇄 */
	else
		new_delay = MIN_DELAY +
			div_u64((MAX_DELAY - MIN_DELAY) *
				(vover_pct - MIN_DELAY_THR_PCT),
				MAX_DELAY_THR_PCT - MIN_DELAY_THR_PCT);

	/* pick the higher one and apply */
	if (new_delay > delay) {
		iocg->delay = new_delay;
		iocg->delay_at = now->now;
		delay = new_delay;
	}

	if (delay >= MIN_DELAY) {	/* 유의미한 NVMe 제출 지연 적용 */
		if (!iocg->indelay_since)
			iocg->indelay_since = now->now;
		blkcg_set_delay(blkg, delay * NSEC_PER_USEC);		/* blkcg use_delay 설정: 이후 bio 제출 지연 → NVMe doorbell 완화(추정) */
		return true;
	} else {
		if (iocg->indelay_since) {
			iocg->stat.indelay_us += now->now - iocg->indelay_since;
			iocg->indelay_since = 0;
		}
		iocg->delay = 0;
		blkcg_clear_delay(blkg);		/* blkcg use_delay 해제 → NVMe 제출 재개 */
		return false;
	}
}

/*
 * iocg_incur_debt - 우선 발행된 IO에 대한 부채 기록
 *
 * root cgroup이나 fatal signal을 받은 태스크의 IO는 블록시키면
 * 우선순위 역전이 생길 수 있으므로, abs_vdebt로 미지급 비용을 기록하고
 * 즉시 NVMe로 발행한다. 이후 주기 타이머가 예산에서 차감한다.
 */
static void iocg_incur_debt(struct ioc_gq *iocg, u64 abs_cost,
			    struct ioc_now *now)
{
	struct iocg_pcpu_stat *gcs;

	lockdep_assert_held(&iocg->ioc->lock);
	lockdep_assert_held(&iocg->waitq.lock);
	WARN_ON_ONCE(list_empty(&iocg->active_list));

	/*
	 * Once in debt, debt handling owns inuse. @iocg stays at the minimum
	 * inuse donating all of it share to others until its debt is paid off.
	 */
	if (!iocg->abs_vdebt && abs_cost) {	/* 신규 부채 발생: NVMe 우선 발행 비용 기록 */
		iocg->indebt_since = now->now;		/* NVMe 부채 누적 시작 시각 */
		propagate_weights(iocg, iocg->active, 0, false, now);		/* 부채 상환 전까지 다른 cgroup에 NVMe 시간 양도 */
	}

	iocg->abs_vdebt += abs_cost;

	gcs = get_cpu_ptr(iocg->pcpu_stat);
	local64_add(abs_cost, &gcs->abs_vusage);
	put_cpu_ptr(gcs);
}

/*
 * iocg_pay_debt - 주기 타이머에서 부채 상환
 *
 * 사용 가능한 vbudget만큼 abs_vdebt를 줄이고, 상환 완료 시 inuse를
 * 복원하여 정상적인 NVMe 제출 예산 계산을 재개한다.
 */
static void iocg_pay_debt(struct ioc_gq *iocg, u64 abs_vpay,
			  struct ioc_now *now)
{
	lockdep_assert_held(&iocg->ioc->lock);
	lockdep_assert_held(&iocg->waitq.lock);

	/*
	 * make sure that nobody messed with @iocg. Check iocg->pd.online
	 * to avoid warn when removing blkcg or disk.
	 */
	WARN_ON_ONCE(list_empty(&iocg->active_list) && iocg->pd.online);
	WARN_ON_ONCE(iocg->inuse > 1);	/* 부채 상환 중 inuse는 최소 유지: NVMe 시간 양도 상태 확인 */

	iocg->abs_vdebt -= min(abs_vpay, iocg->abs_vdebt);	/* 사용 가능한 NVMe 예산으로 부채 상환 */

	/* if debt is paid in full, restore inuse */
	if (!iocg->abs_vdebt) {	/* NVMe 부채 완납 */
		iocg->stat.indebt_us += now->now - iocg->indebt_since;
		iocg->indebt_since = 0;

		propagate_weights(iocg, iocg->active, iocg->last_inuse,		/* NVMe 시간 할당분 복원 */
				  false, now);
	}
}

/*
 * iocg_wake_fn - waitq에서 깨어날 bio가 예산 내인지 검사
 *
 * ctx->vbudget에서 bio의 abs_cost를 현재 hweight_inuse로 환산한 값을
 * 차감한다. 예산이 음수가 되면 더 이상 깨우지 않고, 아니면
 * iocg_commit_bio()로 NVMe 제출을 허가한다.
 */
static int iocg_wake_fn(struct wait_queue_entry *wq_entry, unsigned mode,
			int flags, void *key)
{
	struct iocg_wait *wait = container_of(wq_entry, struct iocg_wait, wait);
	struct iocg_wake_ctx *ctx = key;
	u64 cost = abs_cost_to_cost(wait->abs_cost, ctx->hw_inuse);	/* 현재 NVMe 사용 비율로 대기 bio 비용 환산 */

	ctx->vbudget -= cost;	/* 남은 NVMe 예산에서 bio 차감 */

	if (ctx->vbudget < 0)	/* NVMe 예산 소진: 더 이상 대기자 깨우지 않음 */
		return -1;		/* waitq 탐색 중단: NVMe 제출 한도 도달 */

	iocg_commit_bio(ctx->iocg, wait->bio, wait->abs_cost, cost);	/* NVMe 제출 예산 확정, vtime 전진 */
	wait->committed = true;	/* issuer 깨어나 blk-mq/NVMe 경로로 진행 허가 */

	/*
	 * autoremove_wake_function() removes the wait entry only when it
	 * actually changed the task state. We want the wait always removed.
	 * Remove explicitly and use default_wake_function(). Note that the
	 * order of operations is important as finish_wait() tests whether
	 * @wq_entry is removed without grabbing the lock.
	 */
	default_wake_function(wq_entry, mode, flags, key);	/* issuer 깨움 → blk_mq_submit_bio/NVMe로 재진입 */
	list_del_init_careful(&wq_entry->entry);	/* waitq에서 제거: NVMe 예산 경쟁 방지 */
	return 0;
}

/*
 * Calculate the accumulated budget, pay debt if @pay_debt and wake up waiters
 * accordingly. When @pay_debt is %true, the caller must be holding ioc->lock in
 * addition to iocg->waitq.lock.
 */
/*
 * iocg_kick_waitq - waitq의 대기자들을 예산에 맞춰 깨움
 *
 * 주기 타이머나 waitq 타이머 만료 시 호출된다. 먼저 부채를 상환하고,
 * 남은 vbudget으로 순서대로 대기 bio를 깨운다. 다음 깨어날 시점을
 * 현재 vrate로 역산해 hrtimer를 설정한다.
 */
static void iocg_kick_waitq(struct ioc_gq *iocg, bool pay_debt,
			    struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	struct iocg_wake_ctx ctx = { .iocg = iocg };
	u64 vshortage, expires, oexpires;
	s64 vbudget;
	u32 hwa;

	lockdep_assert_held(&iocg->waitq.lock);

	current_hweight(iocg, &hwa, NULL);
	vbudget = now->vnow - atomic64_read(&iocg->vtime);	/* atomic: 현재 사용 가능한 NVMe vtime 예산 */

	/* pay off debt */
	if (pay_debt && iocg->abs_vdebt && vbudget > 0) {	/* NVMe 예산으로 부채부터 상환 */
		u64 abs_vbudget = cost_to_abs_cost(vbudget, hwa);		/* vtime을 NVMe 절대 비용으로 역환산 */
		u64 abs_vpay = min_t(u64, abs_vbudget, iocg->abs_vdebt);		/* 상환 가능한 NVMe 부채량 */
		u64 vpay = abs_cost_to_cost(abs_vpay, hwa);		/* NVMe vtime으로 상환량 환산 */

		lockdep_assert_held(&ioc->lock);

		atomic64_add(vpay, &iocg->vtime);		/* atomic: issued vtime에 부채 상환 반영 */
		atomic64_add(vpay, &iocg->done_vtime);		/* atomic: completed vtime 동기화 → in-flight 불변 */
		iocg_pay_debt(iocg, abs_vpay, now);		/* NVMe 부채 잔액 갱신 및 inuse 복원 */
		vbudget -= vpay;		/* 상환 후 남은 NVMe 예산 */
	}

	if (iocg->abs_vdebt || iocg->delay)	/* 부채/지연 상태면 NVMe 제출 억제 재평가 */
		iocg_kick_delay(iocg, now);		/* blkcg use_delay 갱신: NVMe doorbell 간격 조절(추정) */

	/*
	 * Debt can still be outstanding if we haven't paid all yet or the
	 * caller raced and called without @pay_debt. Shouldn't wake up waiters
	 * under debt. Make sure @vbudget reflects the outstanding amount and is
	 * not positive.
	 */
	if (iocg->abs_vdebt) {	/* 미상환 부채가 남아있으면 예산 차감 */
		s64 vdebt = abs_cost_to_cost(iocg->abs_vdebt, hwa);		/* 남은 NVMe 부채를 vtime으로 환산 */
		vbudget = min_t(s64, 0, vbudget - vdebt);		/* 부채를 제외한 실제 NVMe 제출 예산 */
	}

	/*
	 * Wake up the ones which are due and see how much vtime we'll need for
	 * the next one. As paying off debt restores hw_inuse, it must be read
	 * after the above debt payment.
	 */
	ctx.vbudget = vbudget;	/* wake_fn에 전달할 남은 NVMe 예산 */
	current_hweight(iocg, NULL, &ctx.hw_inuse);

	__wake_up_locked_key(&iocg->waitq, TASK_NORMAL, &ctx);	/* 예산 내 대기 bio를 깨워 NVMe 제출 재개 */

	if (!waitqueue_active(&iocg->waitq)) {	/* NVMe 예산 대기자가 모두 처리됨 */
		if (iocg->wait_since) {		/* NVMe 예산 대기 통계 종료 */
			iocg->stat.wait_us += now->now - iocg->wait_since;
			iocg->wait_since = 0;
		}
		return;
	}

	if (!iocg->wait_since)	/* 새로운 NVMe 예산 대기 시작 */
		iocg->wait_since = now->now;		/* NVMe 예산 대기 시작 시각 기록 */

	if (WARN_ON_ONCE(ctx.vbudget >= 0))	/* 대기자가 남았는데 예산이 양수면 버그 */
		return;

	/* determine next wakeup, add a timer margin to guarantee chunking */
	vshortage = -ctx.vbudget;	/* 다음 NVMe 제출까지 필요한 vtime 부족분 */
	expires = now->now_ns +	/* 다음 NVMe 예산 회복 시점 */
		DIV64_U64_ROUND_UP(vshortage, ioc->vtime_base_rate) *
		NSEC_PER_USEC;
	expires += ioc->timer_slack_ns;	/* 타이머 batching: NVMe doorbell 타이밍 완화(추정) */

	/* if already active and close enough, don't bother */
	oexpires = ktime_to_ns(hrtimer_get_softexpires(&iocg->waitq_timer));	/* 기존 NVMe 재개 타이머 만료 시각 */
	if (hrtimer_is_queued(&iocg->waitq_timer) &&	/* 이미 NVMe 재개 타이머가 있고 */
	    abs(oexpires - expires) <= ioc->timer_slack_ns)	/* slack 내에 있으면 재스케줄 생략 */
		return;

	hrtimer_start_range_ns(&iocg->waitq_timer, ns_to_ktime(expires),	/* NVMe 예산 회복 시점에 issuer 깨움 */
			       ioc->timer_slack_ns, HRTIMER_MODE_ABS);
}

static enum hrtimer_restart iocg_waitq_timer_fn(struct hrtimer *timer)
{
	struct ioc_gq *iocg = container_of(timer, struct ioc_gq, waitq_timer);
	bool pay_debt = READ_ONCE(iocg->abs_vdebt);	/* READ_ONCE: NVMe 부채 존재 여부를 lock 없이 확인 */
	struct ioc_now now;	/* NVMe 제어 시계 스냅샷 */
	unsigned long flags;

	ioc_now(iocg->ioc, &now);

	iocg_lock(iocg, pay_debt, &flags);	/* 부채 있으면 ioc->lock도 획득: NVMe 예산/부채 동시 보호 */
	iocg_kick_waitq(iocg, pay_debt, &now);	/* NVMe 예산 회복 시 waitq 처리 */
	iocg_unlock(iocg, pay_debt, &flags);

	return HRTIMER_NORESTART;	/* hrtimer는 one-shot: 필요시 iocg_kick_waitq가 재시작 */
}

/*
 * ioc_lat_stat - 주기별 NVMe 완료 지연 및 rq 대기 통계 집계
 *
 * per-CPU missed[] 카운터와 rq_wait_ns를 CPU 순회하며 합산한다.
 * missed_ppm은 latency QoS 목표를 놓친 NVMe 명령의 비율(ppm),
 * rq_wait_pct는 request 할당 대기 시간의 주기 대비 비율이다.
 */
static void ioc_lat_stat(struct ioc *ioc, u32 *missed_ppm_ar, u32 *rq_wait_pct_p,
			 u32 *nr_done)
{
	u32 nr_met[2] = { };
	u32 nr_missed[2] = { };
	u64 rq_wait_ns = 0;
	int cpu, rw;

	for_each_online_cpu(cpu) {	/* per-CPU NVMe CQ 통계를 CPU 순회하며 집계 */
		struct ioc_pcpu_stat *stat = per_cpu_ptr(ioc->pcpu_stat, cpu);		/* 해당 CPU의 NVMe 완료/대기 통계 */
		u64 this_rq_wait_ns;

		for (rw = READ; rw <= WRITE; rw++) {		/* read/write NVMe 완료 지연 각각 집계 */
			u32 this_met = local_read(&stat->missed[rw].nr_met);			/* local: 해당 CPU에서 NVMe latency QoS 달성 횟수 */
			u32 this_missed = local_read(&stat->missed[rw].nr_missed);			/* local: 해당 CPU에서 NVMe latency QoS 미달 횟수 */

			nr_met[rw] += this_met - stat->missed[rw].last_met;			/* 주기 간 NVMe QoS 달성 증가량 */
			nr_missed[rw] += this_missed - stat->missed[rw].last_missed;			/* 주기 간 NVMe QoS 미달 증가량 */
			stat->missed[rw].last_met = this_met;			/* 다음 주기 NVMe QoS 집계 기준 */
			stat->missed[rw].last_missed = this_missed;			/* 다음 주기 NVMe QoS 집계 기준 */
		}

		this_rq_wait_ns = local64_read(&stat->rq_wait_ns);		/* local64: 해당 CPU의 NVMe request(tag) 할당 대기 시간 */
		rq_wait_ns += this_rq_wait_ns - stat->last_rq_wait_ns;		/* 주기 간 NVMe rq_wait 증가량 */
		stat->last_rq_wait_ns = this_rq_wait_ns;		/* 다음 주기 NVMe rq_wait 집계 기준 */
	}

	for (rw = READ; rw <= WRITE; rw++) {
		if (nr_met[rw] + nr_missed[rw])		/* 해당 방향 NVMe 완료가 있을 때만 ppm 계산 */
			missed_ppm_ar[rw] =
				DIV64_U64_ROUND_UP((u64)nr_missed[rw] * MILLION,
						   nr_met[rw] + nr_missed[rw]);
		else
			missed_ppm_ar[rw] = 0;
	}

	*rq_wait_pct_p = div64_u64(rq_wait_ns * 100,	/* NVMe request 할당 대기 시간을 주기 대비 %로 */
				   ioc->period_us * NSEC_PER_USEC);

	*nr_done = nr_met[READ] + nr_met[WRITE] + nr_missed[READ] + nr_missed[WRITE];	/* 이번 주기 NVMe 완료 총수 */
}

/* was iocg idle this period? */
static bool iocg_is_idle(struct ioc_gq *iocg)
{
	struct ioc *ioc = iocg->ioc;

	/* did something get issued this period? */
	if (atomic64_read(&iocg->active_period) ==	/* atomic: 이번 NVMe 주기에 제출했는가 */
	    atomic64_read(&ioc->cur_period))	/* atomic: 현재 NVMe 주기 번호 */
		return false;

	/* is something in flight? */
	if (atomic64_read(&iocg->done_vtime) != atomic64_read(&iocg->vtime))	/* atomic: NVMe in-flight 명령이 남아있으면 idle 아님 */
		return false;

	return true;
}

/*
 * Call this function on the target leaf @iocg's to build pre-order traversal
 * list of all the ancestors in @inner_walk. The inner nodes are linked through
 * ->walk_list and the caller is responsible for dissolving the list after use.
 */
static void iocg_build_inner_walk(struct ioc_gq *iocg,
				  struct list_head *inner_walk)
{
	int lvl;

	WARN_ON_ONCE(!list_empty(&iocg->walk_list));

	/* find the first ancestor which hasn't been visited yet */
	for (lvl = iocg->level - 1; lvl >= 0; lvl--) {
		if (!list_empty(&iocg->ancestors[lvl]->walk_list))
			break;
	}

	/* walk down and visit the inner nodes to get pre-order traversal */
	while (++lvl <= iocg->level - 1) {	/* 미방문 낮부 노드를 순서대로 NVMe 통계 트리에 추가 */
		struct ioc_gq *inner = iocg->ancestors[lvl];

		/* record traversal order */
		list_add_tail(&inner->walk_list, inner_walk);
	}
}

/* propagate the deltas to the parent */
static void iocg_flush_stat_upward(struct ioc_gq *iocg)
{
	if (iocg->level > 0) {	/* root가 아니면 상위 cgroup으로 NVMe 통계 전파 */
		struct iocg_stat *parent_stat =
			&iocg->ancestors[iocg->level - 1]->stat;

		parent_stat->usage_us +=		/* 상위 cgroup NVMe 사용량 누적 */
			iocg->stat.usage_us - iocg->last_stat.usage_us;			/* 주기 간 NVMe 사용량 증가분 */
		parent_stat->wait_us +=
			iocg->stat.wait_us - iocg->last_stat.wait_us;
		parent_stat->indebt_us +=
			iocg->stat.indebt_us - iocg->last_stat.indebt_us;
		parent_stat->indelay_us +=
			iocg->stat.indelay_us - iocg->last_stat.indelay_us;
	}

	iocg->last_stat = iocg->stat;
}

/* collect per-cpu counters and propagate the deltas to the parent */
/*
 * iocg_flush_stat_leaf - leaf cgroup의 per-CPU 사용량을 수집하고 상위로 전파
 */
static void iocg_flush_stat_leaf(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	u64 abs_vusage = 0;
	u64 vusage_delta;
	int cpu;

	lockdep_assert_held(&iocg->ioc->lock);

	/* collect per-cpu counters */
	for_each_possible_cpu(cpu) {	/* leaf cgroup의 per-CPU NVMe 사용량을 모두 합산 */
		abs_vusage += local64_read(		/* local64: 해당 CPU에서 NVMe에 소진된 절대 vtime */
				per_cpu_ptr(&iocg->pcpu_stat->abs_vusage, cpu));			/* per-CPU 포인터로 NVMe 사용량 읽기 */
	}
	vusage_delta = abs_vusage - iocg->last_stat_abs_vusage;	/* 주기 간 NVMe 절대 사용량 증가분 */
	iocg->last_stat_abs_vusage = abs_vusage;

	iocg->usage_delta_us = div64_u64(vusage_delta, ioc->vtime_base_rate);	/* NVMe vtime을 wallclock μs로 환산 */
	iocg->stat.usage_us += iocg->usage_delta_us;

	iocg_flush_stat_upward(iocg);
}

/* get stat counters ready for reading on all active iocgs */
static void iocg_flush_stat(struct list_head *target_iocgs, struct ioc_now *now)
{
	LIST_HEAD(inner_walk);
	struct ioc_gq *iocg, *tiocg;	/* NVMe 활성 cgroup 순회용 */

	/* flush leaves and build inner node walk list */
	list_for_each_entry(iocg, target_iocgs, active_list) {	/* NVMe 활성 cgroup 전체 통계 플러시 */
		iocg_flush_stat_leaf(iocg, now);		/* leaf cgroup의 per-CPU NVMe 사용량 집계 */
		iocg_build_inner_walk(iocg, &inner_walk);		/* NVMe 통계 상위 전파용 트리 구축 */
	}

	/* keep flushing upwards by walking the inner list backwards */
	list_for_each_entry_safe_reverse(iocg, tiocg, &inner_walk, walk_list) {	/* leaf에서 root로 NVMe 통계 전파 */
		iocg_flush_stat_upward(iocg);
		list_del_init(&iocg->walk_list);
	}
}

/*
 * Determine what @iocg's hweight_inuse should be after donating unused
 * capacity. @hwm is the upper bound and used to signal no donation. This
 * function also throws away @iocg's excess budget.
 */
/*
 * hweight_after_donation - work-conservation을 위한 잉여 share 기부량 계산
 *
 * 이 cgroup이 실제로 사용하지 않는 NVMe 시간 할당분을 다른 cgroup에
 * 넘길 수 있도록, 기부 후 target hweight_inuse를 계산한다.
 */
static u32 hweight_after_donation(struct ioc_gq *iocg, u32 old_hwi, u32 hwm,
				  u32 usage, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	u64 vtime = atomic64_read(&iocg->vtime);
	s64 excess, delta, target, new_hwi;

	/* debt handling owns inuse for debtors */
	if (iocg->abs_vdebt)	/* 부채 cgroup은 NVMe 시간 기부 불가 */
		return 1;

	/* see whether minimum margin requirement is met */
	if (waitqueue_active(&iocg->waitq) ||	/* NVMe 예산 대기자가 있으면 기부하지 않음 */
	    time_after64(vtime, now->vnow - ioc->margins.min))	/* 최소 margin 이하로 NVMe 예산이 줄면 기부 금지 */
		return hwm;

	/* throw away excess above target */
	excess = now->vnow - vtime - ioc->margins.target;	/* NVMe target margin 초과 예산 */
	if (excess > 0) {	/* NVMe 예산이 target 이상 남아있으면 버림 */
		atomic64_add(excess, &iocg->vtime);		/* atomic: 초과 NVMe issued vtime 버림 */
		atomic64_add(excess, &iocg->done_vtime);		/* atomic: 초과 completed vtime도 동기화 */
		vtime += excess;
		ioc->vtime_err -= div64_u64(excess * old_hwi, WEIGHT_ONE);		/* 버려진 NVMe 예산을 vrate로 보정 */
	}

	/*
	 * Let's say the distance between iocg's and device's vtimes as a
	 * fraction of period duration is delta. Assuming that the iocg will
	 * consume the usage determined above, we want to determine new_hwi so
	 * that delta equals MARGIN_TARGET at the end of the next period.
	 *
	 * We need to execute usage worth of IOs while spending the sum of the
	 * new budget (1 - MARGIN_TARGET) and the leftover from the last period
	 * (delta):
	 *
	 *   usage = (1 - MARGIN_TARGET + delta) * new_hwi
	 *
	 * Therefore, the new_hwi is:
	 *
	 *   new_hwi = usage / (1 - MARGIN_TARGET + delta)
	 */
	delta = div64_s64(WEIGHT_ONE * (now->vnow - vtime),
			  now->vnow - ioc->period_at_vtime);
	target = WEIGHT_ONE * MARGIN_TARGET_PCT / 100;
	new_hwi = div64_s64(WEIGHT_ONE * usage, WEIGHT_ONE - target + delta);

	return clamp_t(s64, new_hwi, 1, hwm);
}

/*
 * For work-conservation, an iocg which isn't using all of its share should
 * donate the leftover to other iocgs. There are two ways to achieve this - 1.
 * bumping up vrate accordingly 2. lowering the donating iocg's inuse weight.
 *
 * #1 is mathematically simpler but has the drawback of requiring synchronous
 * global hweight_inuse updates when idle iocg's get activated or inuse weights
 * change due to donation snapbacks as it has the possibility of grossly
 * overshooting what's allowed by the model and vrate.
 *
 * #2 is inherently safe with local operations. The donating iocg can easily
 * snap back to higher weights when needed without worrying about impacts on
 * other nodes as the impacts will be inherently correct. This also makes idle
 * iocg activations safe. The only effect activations have is decreasing
 * hweight_inuse of others, the right solution to which is for those iocgs to
 * snap back to higher weights.
 *
 * So, we go with #2. The challenge is calculating how each donating iocg's
 * inuse should be adjusted to achieve the target donation amounts. This is done
 * using Andy's method described in the following pdf.
 *
 *   https://drive.google.com/file/d/1PsJwxPFtjUnwOY1QJ5AeICCcsL7BM3bo
 *
 * Given the weights and target after-donation hweight_inuse values, Andy's
 * method determines how the proportional distribution should look like at each
 * sibling level to maintain the relative relationship between all non-donating
 * pairs. To roughly summarize, it divides the tree into donating and
 * non-donating parts, calculates global donation rate which is used to
 * determine the target hweight_inuse for each node, and then derives per-level
 * proportions.
 *
 * The following pdf shows that global distribution calculated this way can be
 * achieved by scaling inuse weights of donating leaves and propagating the
 * adjustments upwards proportionally.
 *
 *   https://drive.google.com/file/d/1vONz1-fzVO7oY5DXXsLjSxEtYYQbOvsE
 *
 * Combining the above two, we can determine how each leaf iocg's inuse should
 * be adjusted to achieve the target donation.
 *
 *   https://drive.google.com/file/d/1WcrltBOSPN0qXVdBgnKm4mdp9FhuEFQN
 *
 * The inline comments use symbols from the last pdf.
 *
 *   b is the sum of the absolute budgets in the subtree. 1 for the root node.
 *   f is the sum of the absolute budgets of non-donating nodes in the subtree.
 *   t is the sum of the absolute budgets of donating nodes in the subtree.
 *   w is the weight of the node. w = w_f + w_t
 *   w_f is the non-donating portion of w. w_f = w * f / b
 *   w_b is the donating portion of w. w_t = w * t / b
 *   s is the sum of all sibling weights. s = Sum(w) for siblings
 *   s_f and s_t are the non-donating and donating portions of s.
 *
 * Subscript p denotes the parent's counterpart and ' the adjusted value - e.g.
 * w_pt is the donating portion of the parent's weight and w'_pt the same value
 * after adjustments. Subscript r denotes the root node's values.
 */
/*
 * transfer_surpluses - 잉여 NVMe 시간 할당분을 필요한 cgroup으로 이동
 *
 * hweight_after_donation이 활성 가중치보다 낮은 기부자(donor)들로부터
 * 필요한 수혜자에게 inuse를 재분배한다. 이를 통해 NVMe 장치가 idle
 * 상태가 되지 않도록 work-conserving을 유지한다.
 */
static void transfer_surpluses(struct list_head *surpluses, struct ioc_now *now)
{
	LIST_HEAD(over_hwa);
	LIST_HEAD(inner_walk);
	struct ioc_gq *iocg, *tiocg, *root_iocg;
	u32 after_sum, over_sum, over_target, gamma;

	/*
	 * It's pretty unlikely but possible for the total sum of
	 * hweight_after_donation's to be higher than WEIGHT_ONE, which will
	 * confuse the following calculations. If such condition is detected,
	 * scale down everyone over its full share equally to keep the sum below
	 * WEIGHT_ONE.
	 */
	after_sum = 0;
	over_sum = 0;
	list_for_each_entry(iocg, surpluses, surplus_list) {	/* NVMe 시간 잉여 cgroup 순회 */
		u32 hwa;

		current_hweight(iocg, &hwa, NULL);
		after_sum += iocg->hweight_after_donation;		/* 기부 후 NVMe 사용 비율 합 */

		if (iocg->hweight_after_donation > hwa) {
			over_sum += iocg->hweight_after_donation;
			list_add(&iocg->walk_list, &over_hwa);
		}
	}

	if (after_sum >= WEIGHT_ONE) {
		/*
		 * The delta should be deducted from the over_sum, calculate
		 * target over_sum value.
		 */
		u32 over_delta = after_sum - (WEIGHT_ONE - 1);
		WARN_ON_ONCE(over_sum <= over_delta);
		over_target = over_sum - over_delta;
	} else {
		over_target = 0;
	}

	list_for_each_entry_safe(iocg, tiocg, &over_hwa, walk_list) {
		if (over_target)
			iocg->hweight_after_donation =
				div_u64((u64)iocg->hweight_after_donation *
					over_target, over_sum);
		list_del_init(&iocg->walk_list);
	}

	/*
	 * Build pre-order inner node walk list and prepare for donation
	 * adjustment calculations.
	 */
	list_for_each_entry(iocg, surpluses, surplus_list) {
		iocg_build_inner_walk(iocg, &inner_walk);
	}

	root_iocg = list_first_entry(&inner_walk, struct ioc_gq, walk_list);
	WARN_ON_ONCE(root_iocg->level > 0);

	list_for_each_entry(iocg, &inner_walk, walk_list) {	/* NVMe 기부 비율 검증/보정 */
		iocg->child_adjusted_sum = 0;
		iocg->hweight_donating = 0;
		iocg->hweight_after_donation = 0;
	}

	/*
	 * Propagate the donating budget (b_t) and after donation budget (b'_t)
	 * up the hierarchy.
	 */
	list_for_each_entry(iocg, surpluses, surplus_list) {
		struct ioc_gq *parent = iocg->ancestors[iocg->level - 1];

		parent->hweight_donating += iocg->hweight_donating;
		parent->hweight_after_donation += iocg->hweight_after_donation;
	}

	list_for_each_entry_reverse(iocg, &inner_walk, walk_list) {	/* 낮부에서 root로 NVMe 기부량 전파 */
		if (iocg->level > 0) {		/* root가 아닌 NVMe 기부자면 부모로 누적 */
			struct ioc_gq *parent = iocg->ancestors[iocg->level - 1];

			parent->hweight_donating += iocg->hweight_donating;
			parent->hweight_after_donation += iocg->hweight_after_donation;
		}
	}

	/*
	 * Calculate inner hwa's (b) and make sure the donation values are
	 * within the accepted ranges as we're doing low res calculations with
	 * roundups.
	 */
	list_for_each_entry(iocg, &inner_walk, walk_list) {
		if (iocg->level) {		/* root 제외 NVMe 기부자 비율 재계산 */
			struct ioc_gq *parent = iocg->ancestors[iocg->level - 1];

			iocg->hweight_active = DIV64_U64_ROUND_UP(			/* 부모로부터 상속된 NVMe 활성 비율 */
				(u64)parent->hweight_active * iocg->active,				/* 부모 NVMe 비율 * 자식 active */
				parent->child_active_sum);

		}

		iocg->hweight_donating = min(iocg->hweight_donating,
					     iocg->hweight_active);
		iocg->hweight_after_donation = min(iocg->hweight_after_donation,
						   iocg->hweight_donating - 1);
		if (WARN_ON_ONCE(iocg->hweight_active <= 1 ||
				 iocg->hweight_donating <= 1 ||
				 iocg->hweight_after_donation == 0)) {
			pr_warn("iocg: invalid donation weights in ");
			pr_cont_cgroup_path(iocg_to_blkg(iocg)->blkcg->css.cgroup);
			pr_cont(": active=%u donating=%u after=%u\n",
				iocg->hweight_active, iocg->hweight_donating,
				iocg->hweight_after_donation);
		}
	}

	/*
	 * Calculate the global donation rate (gamma) - the rate to adjust
	 * non-donating budgets by.
	 *
	 * No need to use 64bit multiplication here as the first operand is
	 * guaranteed to be smaller than WEIGHT_ONE (1<<16).
	 *
	 * We know that there are beneficiary nodes and the sum of the donating
	 * hweights can't be whole; however, due to the round-ups during hweight
	 * calculations, root_iocg->hweight_donating might still end up equal to
	 * or greater than whole. Limit the range when calculating the divider.
	 *
	 * gamma = (1 - t_r') / (1 - t_r)
	 */
	gamma = DIV_ROUND_UP(	/* 전역 NVMe 시간 기부율(추정) */
		(WEIGHT_ONE - root_iocg->hweight_after_donation) * WEIGHT_ONE,		/* 기부 후 비기부자 NVMe 비율 */
		WEIGHT_ONE - min_t(u32, root_iocg->hweight_donating, WEIGHT_ONE - 1));		/* 기부 전 기부자 NVMe 비율(0 나눔 방지) */

	/*
	 * Calculate adjusted hwi, child_adjusted_sum and inuse for the inner
	 * nodes.
	 */
	list_for_each_entry(iocg, &inner_walk, walk_list) {
		struct ioc_gq *parent;
		u32 inuse, wpt, wptp;
		u64 st, sf;

		if (iocg->level == 0) {		/* root: 1st level 자식들의 NVMe adjusted 합 계산 */
			/* adjusted weight sum for 1st level: s' = s * b_pf / b'_pf */
			iocg->child_adjusted_sum = DIV64_U64_ROUND_UP(
				iocg->child_active_sum * (WEIGHT_ONE - iocg->hweight_donating),
				WEIGHT_ONE - iocg->hweight_after_donation);
			continue;
		}

		parent = iocg->ancestors[iocg->level - 1];		/* NVMe 기부 조정 시 부모 cgroup 참조 */

		/* b' = gamma * b_f + b_t' */
		iocg->hweight_inuse = DIV64_U64_ROUND_UP(
			(u64)gamma * (iocg->hweight_active - iocg->hweight_donating),
			WEIGHT_ONE) + iocg->hweight_after_donation;

		/* w' = s' * b' / b'_p */
		inuse = DIV64_U64_ROUND_UP(		/* NVMe 기부 후 자식의 새 inuse */
			(u64)parent->child_adjusted_sum * iocg->hweight_inuse,
			parent->hweight_inuse);

		/* adjusted weight sum for children: s' = s_f + s_t * w'_pt / w_pt */
		st = DIV64_U64_ROUND_UP(
			iocg->child_active_sum * iocg->hweight_donating,
			iocg->hweight_active);
		sf = iocg->child_active_sum - st;
		wpt = DIV64_U64_ROUND_UP(
			(u64)iocg->active * iocg->hweight_donating,
			iocg->hweight_active);
		wptp = DIV64_U64_ROUND_UP(
			(u64)inuse * iocg->hweight_after_donation,
			iocg->hweight_inuse);

		iocg->child_adjusted_sum = sf + DIV64_U64_ROUND_UP(st * wptp, wpt);
	}

	/*
	 * All inner nodes now have ->hweight_inuse and ->child_adjusted_sum and
	 * we can finally determine leaf adjustments.
	 */
	list_for_each_entry(iocg, surpluses, surplus_list) {
		struct ioc_gq *parent = iocg->ancestors[iocg->level - 1];
		u32 inuse;

		/*
		 * In-debt iocgs participated in the donation calculation with
		 * the minimum target hweight_inuse. Configuring inuse
		 * accordingly would work fine but debt handling expects
		 * @iocg->inuse stay at the minimum and we don't wanna
		 * interfere.
		 */
		if (iocg->abs_vdebt) {		/* 부채 cgroup은 NVMe 기부에서 제외, inuse 최소 유지 */
			WARN_ON_ONCE(iocg->inuse > 1);
			continue;
		}

		/* w' = s' * b' / b'_p, note that b' == b'_t for donating leaves */
		inuse = DIV64_U64_ROUND_UP(
			parent->child_adjusted_sum * iocg->hweight_after_donation,
			parent->hweight_inuse);

		TRACE_IOCG_PATH(inuse_transfer, iocg, now,
				iocg->inuse, inuse,
				iocg->hweight_inuse,
				iocg->hweight_after_donation);

		__propagate_weights(iocg, iocg->active, inuse, true, now);
	}

	/* walk list should be dissolved after use */
	list_for_each_entry_safe(iocg, tiocg, &inner_walk, walk_list)
		list_del_init(&iocg->walk_list);
}

/*
 * A low weight iocg can amass a large amount of debt, for example, when
 * anonymous memory gets reclaimed aggressively. If the system has a lot of
 * memory paired with a slow IO device, the debt can span multiple seconds or
 * more. If there are no other subsequent IO issuers, the in-debt iocg may end
 * up blocked paying its debt while the IO device is idle.
 *
 * The following protects against such cases. If the device has been
 * sufficiently idle for a while, the debts are halved and delays are
 * recalculated.
 */
/*
 * ioc_forgive_debts - 장치가 한가할 때 부채 일부 탕감
 *
 * NVMe 장치 사용률이 DFGV_USAGE_PCT(50%) 이하로 100ms 이상 지속되면,
 * 누적된 abs_vdebt와 delay를 절반으로 줄인다. 메모리 회수 등으로
 * 생긴 과도한 부채가 NVMe를 idle 상태로 만들지 않도록 방지한다.
 */
static void ioc_forgive_debts(struct ioc *ioc, u64 usage_us_sum, int nr_debtors,
			      struct ioc_now *now)
{
	struct ioc_gq *iocg;
	u64 dur, usage_pct, nr_cycles, nr_cycles_shift;

	/* if no debtor, reset the cycle */
	if (!nr_debtors) {	/* NVMe 부채 cgroup이 없으면 idle 측정 리셋 */
		ioc->dfgv_period_at = now->now;
		ioc->dfgv_period_rem = 0;
		ioc->dfgv_usage_us_sum = 0;
		return;
	}

	/*
	 * Debtors can pass through a lot of writes choking the device and we
	 * don't want to be forgiving debts while the device is struggling from
	 * write bursts. If we're missing latency targets, consider the device
	 * fully utilized.
	 */
	if (ioc->busy_level > 0)	/* NVMe 장치가 포화면 사용률을 100%로 간주, 부채 탕감 억제 */
		usage_us_sum = max_t(u64, usage_us_sum, ioc->period_us);		/* NVMe busy 시 최소 사용량 보정 */

	ioc->dfgv_usage_us_sum += usage_us_sum;
	if (time_before64(now->now, ioc->dfgv_period_at + DFGV_PERIOD))	/* 100ms 미만이면 NVMe idle 판단 보류 */
		return;

	/*
	 * At least DFGV_PERIOD has passed since the last period. Calculate the
	 * average usage and reset the period counters.
	 */
	dur = now->now - ioc->dfgv_period_at;	/* NVMe idle 측정 구간 */
	usage_pct = div64_u64(100 * ioc->dfgv_usage_us_sum, dur);	/* NVMe 사용률(%) */

	ioc->dfgv_period_at = now->now;
	ioc->dfgv_usage_us_sum = 0;

	/* if was too busy, reset everything */
	if (usage_pct > DFGV_USAGE_PCT) {	/* NVMe 사용률이 50% 초과면 부채 탕감 중단 */
		ioc->dfgv_period_rem = 0;
		return;
	}

	/*
	 * Usage is lower than threshold. Let's forgive some debts. Debt
	 * forgiveness runs off of the usual ioc timer but its period usually
	 * doesn't match ioc's. Compensate the difference by performing the
	 * reduction as many times as would fit in the duration since the last
	 * run and carrying over the left-over duration in @ioc->dfgv_period_rem
	 * - if ioc period is 75% of DFGV_PERIOD, one out of three consecutive
	 * reductions is doubled.
	 */
	nr_cycles = dur + ioc->dfgv_period_rem;
	ioc->dfgv_period_rem = do_div(nr_cycles, DFGV_PERIOD);

	list_for_each_entry(iocg, &ioc->active_iocgs, active_list) {	/* NVMe 활성 cgroup별 사용량/잉여 계산 */
		u64 __maybe_unused old_debt, __maybe_unused old_delay;

		if (!iocg->abs_vdebt && !iocg->delay)
			continue;

		spin_lock(&iocg->waitq.lock);

		old_debt = iocg->abs_vdebt;
		old_delay = iocg->delay;

		nr_cycles_shift = min_t(u64, nr_cycles, BITS_PER_LONG - 1);		/* shift 오버플로우 방지 */
		if (iocg->abs_vdebt)			/* NVMe 부채가 있으면 절반으로 탕감 */
			iocg->abs_vdebt = iocg->abs_vdebt >> nr_cycles_shift ?: 1;			/* 부채 절반 감소, 최소 1 유지 */

		if (iocg->delay)			/* NVMe 제출 지연도 절반 감소 */
			iocg->delay = iocg->delay >> nr_cycles_shift ?: 1;			/* 지연 절반 감소 → NVMe doorbell 간격 점진적 복원 */

		iocg_kick_waitq(iocg, true, now);			/* 부채 탕감 후 NVMe 예산으로 waitq 처리 */

		TRACE_IOCG_PATH(iocg_forgive_debt, iocg, now, usage_pct,
				old_debt, iocg->abs_vdebt,
				old_delay, iocg->delay);

		spin_unlock(&iocg->waitq.lock);
	}
}

/*
 * Check the active iocgs' state to avoid oversleeping and deactive
 * idle iocgs.
 *
 * Since waiters determine the sleep durations based on the vrate
 * they saw at the time of sleep, if vrate has increased, some
 * waiters could be sleeping for too long. Wake up tardy waiters
 * which should have woken up in the last period and expire idle
 * iocgs.
 */
/*
 * ioc_check_iocgs - 활성 cgroup 상태 점검 및 비활성화
 *
 * 주기 타이머가 각 cgroup의 waitq/debt/idle 상태를 검사한다.
 * vrate 상승으로 인해 waitq 타이머가 과도하게 길게 설정된 경우
 * 즉시 깨우고, 일정 기간 IO가 없으면 active_iocgs에서 제거하여
 * NVMe 제출 예산 집계에서 빼낸다.
 */
static int ioc_check_iocgs(struct ioc *ioc, struct ioc_now *now)
{
	int nr_debtors = 0;
	struct ioc_gq *iocg, *tiocg;

	list_for_each_entry_safe(iocg, tiocg, &ioc->active_iocgs, active_list) {	/* NVMe 활성 cgroup 전체 점검 */
		if (!waitqueue_active(&iocg->waitq) && !iocg->abs_vdebt &&
		    !iocg->delay && !iocg_is_idle(iocg))
			continue;

		spin_lock(&iocg->waitq.lock);

		/* flush wait and indebt stat deltas */
		if (iocg->wait_since) {
			iocg->stat.wait_us += now->now - iocg->wait_since;
			iocg->wait_since = now->now;
		}
		if (iocg->indebt_since) {
			iocg->stat.indebt_us +=
				now->now - iocg->indebt_since;
			iocg->indebt_since = now->now;
		}
		if (iocg->indelay_since) {
			iocg->stat.indelay_us +=
				now->now - iocg->indelay_since;
			iocg->indelay_since = now->now;
		}

		if (waitqueue_active(&iocg->waitq) || iocg->abs_vdebt ||
		    iocg->delay) {
			/* might be oversleeping vtime / hweight changes, kick */
			iocg_kick_waitq(iocg, true, now);
			if (iocg->abs_vdebt || iocg->delay)
				nr_debtors++;
		} else if (iocg_is_idle(iocg)) {
			/* no waiter and idle, deactivate */
			u64 vtime = atomic64_read(&iocg->vtime);			/* atomic: NVMe issued vtime */
			s64 excess;

			/*
			 * @iocg has been inactive for a full duration and will
			 * have a high budget. Account anything above target as
			 * error and throw away. On reactivation, it'll start
			 * with the target budget.
			 */
			excess = now->vnow - vtime - ioc->margins.target;			/* NVMe target margin 초과 예산 */
			if (excess > 0) {				/* 비활성화 시 초과 NVMe 예산 버림 */
				u32 old_hwi;

				current_hweight(iocg, NULL, &old_hwi);					/* 버려진 예산에 대한 NVMe vrate 보정용 */
				ioc->vtime_err -= div64_u64(excess * old_hwi,
							    WEIGHT_ONE);
			}

			TRACE_IOCG_PATH(iocg_idle, iocg, now,
					atomic64_read(&iocg->active_period),
					atomic64_read(&ioc->cur_period), vtime);
			__propagate_weights(iocg, 0, 0, false, now);
			list_del_init(&iocg->active_list);
		}

		spin_unlock(&iocg->waitq.lock);
	}

	commit_weights(ioc);	/* 비활성화로 변경된 NVMe 가중치 캐시 갱신 */
	return nr_debtors;
}

/*
 * ioc_timer_fn - iocost 주기 타이머 핸들러
 *
 * 주기마다 호출되어 다음을 수행한다:
 * 1) ioc_lat_stat()으로 NVMe 완료 지연/ rq_wait 통계 수집
 * 2) ioc_check_iocgs()로 활성 cgroup 정리
 * 3) iocg_flush_stat()으로 사용량 집계
 * 4) 잉여 share 기부/이전(transfer_surpluses)
 * 5) busy_level에 따른 vrate 조정(ioc_adjust_base_vrate)
 * 6) 부채 탕감(ioc_forgive_debts)
 * 7) 다음 주기 시작(ioc_start_period)
 */
static void ioc_timer_fn(struct timer_list *timer)
{
	struct ioc *ioc = container_of(timer, struct ioc, timer);
	struct ioc_gq *iocg, *tiocg;
	struct ioc_now now;
	LIST_HEAD(surpluses);	/* NVMe 시간 잉여 cgroup 임시 목록 */
	int nr_debtors, nr_shortages = 0, nr_lagging = 0;	/* NVMe 포화상태 지표 초기화 */
	u64 usage_us_sum = 0;	/* NVMe 사용량 합계 */
	u32 ppm_rthr;	/* read latency QoS 목표를 벗어날 NVMe 명령 비율 임계 */
	u32 ppm_wthr;	/* write latency QoS 목표를 벗어날 NVMe 명령 비율 임계 */
	u32 missed_ppm[2], rq_wait_pct, nr_done;	/* NVMe 완료 지연/ rq_wait/처리량 통계 */
	u64 period_vtime;	/* 이번 NVMe 주기의 가상 시간 총량 */
	int prev_busy_level;	/* 이전 NVMe 포화 수준 */

	/* how were the latencies during the period? */
	ioc_lat_stat(ioc, missed_ppm, &rq_wait_pct, &nr_done);	/* 이번 주기 NVMe CQ 완료/대기 통계 수집 */

	/* take care of active iocgs */
	spin_lock_irq(&ioc->lock);

	ppm_rthr = MILLION - ioc->params.qos[QOS_RPPM];	/* read latency QoS 미달 허용 ppm */
	ppm_wthr = MILLION - ioc->params.qos[QOS_WPPM];	/* write latency QoS 미달 허용 ppm */
	ioc_now(ioc, &now);	/* NVMe 타이머 기준 시계 획득 */

	period_vtime = now.vnow - ioc->period_at_vtime;	/* 현재 NVMe 주기에서 소진된 가상 시간 */
	if (WARN_ON_ONCE(!period_vtime)) {	/* 주기 길이가 0이면 NVMe vrate 계산 불가 */
		spin_unlock_irq(&ioc->lock);		/* NVMe 타이머 lock 해제 */
		return;
	}

	nr_debtors = ioc_check_iocgs(ioc, &now);	/* NVMe 활성 cgroup 정리 및 debtor 집계 */

	/*
	 * Wait and indebt stat are flushed above and the donation calculation
	 * below needs updated usage stat. Let's bring stat up-to-date.
	 */
	iocg_flush_stat(&ioc->active_iocgs, &now);	/* NVMe 사용량 통계 플러시 */

	/* calc usage and see whether some weights need to be moved around */
	list_for_each_entry(iocg, &ioc->active_iocgs, active_list) {
		u64 vdone, vtime, usage_us;
		u32 hw_active, hw_inuse;

		/*
		 * Collect unused and wind vtime closer to vnow to prevent
		 * iocgs from accumulating a large amount of budget.
		 */
		vdone = atomic64_read(&iocg->done_vtime);		/* atomic: NVMe CQ 완료 vtime */
		vtime = atomic64_read(&iocg->vtime);		/* atomic: NVMe 제출 vtime */
		current_hweight(iocg, &hw_active, &hw_inuse);		/* 현재 NVMe 활성/사용 비율 */

		/*
		 * Latency QoS detection doesn't account for IOs which are
		 * in-flight for longer than a period.  Detect them by
		 * comparing vdone against period start.  If lagging behind
		 * IOs from past periods, don't increase vrate.
		 */
		if ((ppm_rthr != MILLION || ppm_wthr != MILLION) &&		/* latency QoS가 설정되어 있고 */
		    !atomic_read(&iocg_to_blkg(iocg)->use_delay) &&		/* blkcg delay로 인한 지연이 아니며 */
		    time_after64(vtime, vdone) &&		/* NVMe in-flight 명령이 있고 */
		    time_after64(vtime, now.vnow -		/* NVMe issued vtime이 너무 앞서 있고 */
				 MAX_LAGGING_PERIODS * period_vtime) &&
		    time_before64(vdone, now.vnow - period_vtime))		/* completed vtime이 한 주기 이상 뒤처짐 */
			nr_lagging++;			/* NVMe CQ 완료가 지척되는 장기 in-flight 명령 카운트 */

		/*
		 * Determine absolute usage factoring in in-flight IOs to avoid
		 * high-latency completions appearing as idle.
		 */
		usage_us = iocg->usage_delta_us;		/* 이번 주기 NVMe wallclock 사용량 */
		usage_us_sum += usage_us;

		/* see whether there's surplus vtime */
		WARN_ON_ONCE(!list_empty(&iocg->surplus_list));
		if (hw_inuse < hw_active ||		/* 잉여 기부 중이거나 */
		    (!waitqueue_active(&iocg->waitq) &&		/* 대기자 없이 NVMe 예산이 low margin 이상 남아있거나 */
		     time_before64(vtime, now.vnow - ioc->margins.low))) {		/* low margin 이상 NVMe 예산 잉여 */
			u32 hwa, old_hwi, hwm, new_hwi, usage;
			u64 usage_dur;

			if (vdone != vtime) {			/* NVMe in-flight 명령이 있으면 사용량에 보정 */
				u64 inflight_us = DIV64_U64_ROUND_UP(
					cost_to_abs_cost(vtime - vdone, hw_inuse),
					ioc->vtime_base_rate);

				usage_us = max(usage_us, inflight_us);
			}

			/* convert to hweight based usage ratio */
			if (time_after64(iocg->activated_at, ioc->period_at))			/* 이번 주기에 활성화된 NVMe cgroup */
				usage_dur = max_t(u64, now.now - iocg->activated_at, 1);
			else
				usage_dur = max_t(u64, now.now - ioc->period_at, 1);

			usage = clamp(DIV64_U64_ROUND_UP(usage_us * WEIGHT_ONE, usage_dur),
				      1, WEIGHT_ONE);					/* 최소 1, 최대 100% */

			/*
			 * Already donating or accumulated enough to start.
			 * Determine the donation amount.
			 */
			current_hweight(iocg, &hwa, &old_hwi);				/* 기부 전 NVMe 활성/사용 비율 */
			hwm = current_hweight_max(iocg);				/* inuse를 최대로 했을 때 NVMe 사용 비율 */
			new_hwi = hweight_after_donation(iocg, old_hwi, hwm,				/* 기부 후 목표 NVMe 사용 비율 */
							 usage, &now);
			/*
			 * Donation calculation assumes hweight_after_donation
			 * to be positive, a condition that a donor w/ hwa < 2
			 * can't meet. Don't bother with donation if hwa is
			 * below 2. It's not gonna make a meaningful difference
			 * anyway.
			 */
			if (new_hwi < hwm && hwa >= 2) {				/* NVMe 시간 기부가 의미 있을 때만 */
				iocg->hweight_donating = hwa;
				iocg->hweight_after_donation = new_hwi;
				list_add(&iocg->surplus_list, &surpluses);
			} else if (!iocg->abs_vdebt) {				/* 기부할 만큼 NVMe 예산이 없고 부채 아님 */
				/*
				 * @iocg doesn't have enough to donate. Reset
				 * its inuse to active.
				 *
				 * Don't reset debtors as their inuse's are
				 * owned by debt handling. This shouldn't affect
				 * donation calculuation in any meaningful way
				 * as @iocg doesn't have a meaningful amount of
				 * share anyway.
				 */
				TRACE_IOCG_PATH(inuse_shortage, iocg, &now,
						iocg->inuse, iocg->active,
						iocg->hweight_inuse, new_hwi);

				__propagate_weights(iocg, iocg->active,					/* inuse를 active로 복원해 NVMe 예산 확보 */
						    iocg->active, true, &now);
				nr_shortages++;				/* NVMe 예산 부족 cgroup 수 증가 */
			}
		} else {			/* 진짜 NVMe 예산 부족: 기부 여력 없음 */
			/* genuinely short on vtime */
			nr_shortages++;
		}
	}

	if (!list_empty(&surpluses) && nr_shortages)	/* 잉여와 부족이 모두 있으면 NVMe 시간 이전 */
		transfer_surpluses(&surpluses, &now);		/* NVMe 시간 잉여 → 부족 cgroup 재분배 */

	commit_weights(ioc);

	/* surplus list should be dissolved after use */
	list_for_each_entry_safe(iocg, tiocg, &surpluses, surplus_list)	/* NVMe 잉여 목록 정리 */
		list_del_init(&iocg->surplus_list);

	/*
	 * If q is getting clogged or we're missing too much, we're issuing
	 * too much IO and should lower vtime rate.  If we're not missing
	 * and experiencing shortages but not surpluses, we're too stingy
	 * and should increase vtime rate.
	 */
	prev_busy_level = ioc->busy_level;	/* NVMe 포화 상태 변화 추적 */
	if (!nr_done && nr_lagging) {	/* NVMe 완료는 없지만 장기 in-flight 명령이 있으면 busy_level 유지 */
		/*
		 * When there are lagging IOs but no completions, we don't
		 * know if the IO latency will meet the QoS targets. The
		 * disk might be saturated or not. We should not reset
		 * busy_level to 0 (which would prevent vrate from scaling
		 * up or down), but rather to keep it unchanged.
		 */
	} else if (rq_wait_pct > RQ_WAIT_BUSY_PCT ||	/* rq_wait_pct > 5%: NVMe tag(CID) 할당 병목 또는 software queue 포화(추정) */
		   missed_ppm[READ] > ppm_rthr ||	/* read NVMe latency QoS 미달 비율 초과 */
		   missed_ppm[WRITE] > ppm_wthr) {	/* write NVMe latency QoS 미달 비율 초과 */
		/* clearly missing QoS targets, slow down vrate */		/* NVMe SQ/CQ 과부하 → IO 압력 감소 */
		ioc->busy_level = max(ioc->busy_level, 0);
		ioc->busy_level++;
	} else if (rq_wait_pct <= RQ_WAIT_BUSY_PCT * UNBUSY_THR_PCT / 100 &&	/* rq_wait가 3.75% 이하: NVMe tag/queue 여유 있음 */
		   missed_ppm[READ] <= ppm_rthr * UNBUSY_THR_PCT / 100 &&	/* read NVMe QoS 목표를 75% 수준으로 여유 달성 */
		   missed_ppm[WRITE] <= ppm_wthr * UNBUSY_THR_PCT / 100) {	/* write NVMe QoS 목표를 75% 수준으로 여유 달성 */
		/* QoS targets are being met with >25% margin */
		if (nr_shortages) {
			/*
			 * We're throttling while the device has spare
			 * capacity.  If vrate was being slowed down, stop.
			 */
			ioc->busy_level = min(ioc->busy_level, 0);

			/*
			 * If there are IOs spanning multiple periods, wait
			 * them out before pushing the device harder.
			 */
			if (!nr_lagging)
				ioc->busy_level--;
		} else {
			/*
			 * Nobody is being throttled and the users aren't
			 * issuing enough IOs to saturate the device.  We
			 * simply don't know how close the device is to
			 * saturation.  Coast.
			 */
			ioc->busy_level = 0;
		}
	} else {
		/* inside the hysterisis margin, we're good */
		ioc->busy_level = 0;
	}

	ioc->busy_level = clamp(ioc->busy_level, -1000, 1000);	/* NVMe busy_level 안전 범위 제한 */

	ioc_adjust_base_vrate(ioc, rq_wait_pct, nr_lagging, nr_shortages,	/* NVMe 포화상태에 따른 기본 vrate 조정 */
			      prev_busy_level, missed_ppm);

	ioc_refresh_params(ioc, false);	/* NVMe autop 프로파일 재평가 */

	ioc_forgive_debts(ioc, usage_us_sum, nr_debtors, &now);	/* NVMe idle 시 부채 탕감 */

	/*
	 * This period is done.  Move onto the next one.  If nothing's
	 * going on with the device, stop the timer.
	 */
	atomic64_inc(&ioc->cur_period);	/* atomic: 다음 NVMe 제어 주기로 진행 */

	if (ioc->running != IOC_STOP) {	/* NVMe 컨트롤러 종료 중이 아니면 */
		if (!list_empty(&ioc->active_iocgs)) {		/* NVMe 활성 cgroup이 있으면 다음 주기 시작 */
			ioc_start_period(ioc, &now);			/* 다음 NVMe 피드백 주기 타이머 설정 */
		} else {
			ioc->busy_level = 0;
			ioc->vtime_err = 0;
			ioc->running = IOC_IDLE;
		}

		ioc_refresh_vrate(ioc, &now);		/* 다음 주기 NVMe vrate 보정 */
	}

	spin_unlock_irq(&ioc->lock);
}

/*
 * adjust_inuse_and_calc_cost - bio 발행 직전 예산 내이면 cost 확정,
 *                               아니면 inuse를 늘려 재시도
 *
 * margin이 저하되고 inuse가 active에 도달하지 않은 경우, NVMe 시간
 * 할당분을 회복(inuse 증가)하면서 bio 비용이 현재 vnow 이하로 들어올
 * 때까지 반복한다. 이는 cgroup이 갑자기 많은 IO를 요구할 때 예산을
 * 동적으로 확보하게 한다.
 */
static u64 adjust_inuse_and_calc_cost(struct ioc_gq *iocg, u64 vtime,
				      u64 abs_cost, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc;
	struct ioc_margins *margins = &ioc->margins;
	u32 __maybe_unused old_inuse = iocg->inuse, __maybe_unused old_hwi;
	u32 hwi, adj_step;
	s64 margin;
	u64 cost, new_inuse;
	unsigned long flags;

	current_hweight(iocg, NULL, &hwi);	/* 현재 NVMe 사용 비율로 bio 비용 환산 */
	old_hwi = hwi;	/* inuse 조정 전 NVMe 사용 비율 기록 */
	cost = abs_cost_to_cost(abs_cost, hwi);	/* NVMe 사용 비율에 따른 bio cost */
	margin = now->vnow - vtime - cost;	/* bio 발행 후 남을 NVMe 예산 여유 */

	/* debt handling owns inuse for debtors */
	if (iocg->abs_vdebt)
		return cost;

	/*
	 * We only increase inuse during period and do so if the margin has
	 * deteriorated since the previous adjustment.
	 */
	if (margin >= iocg->saved_margin || margin >= margins->low ||	/* NVMe 예산 여유가 충분하면 조정 불필요 */
	    iocg->inuse == iocg->active)	/* 이미 최대 NVMe 사용 가중치면 조정 불가 */
		return cost;

	spin_lock_irqsave(&ioc->lock, flags);	/* NVMe 가중치 변경 보호 */

	/* we own inuse only when @iocg is in the normal active state */
	if (iocg->abs_vdebt || list_empty(&iocg->active_list)) {	/* lock 획득 후 상태 변화 확인 */
		spin_unlock_irqrestore(&ioc->lock, flags);
		return cost;
	}

	/*
	 * Bump up inuse till @abs_cost fits in the existing budget.
	 * adj_step must be determined after acquiring ioc->lock - we might
	 * have raced and lost to another thread for activation and could
	 * be reading 0 iocg->active before ioc->lock which will lead to
	 * infinite loop.
	 */
	new_inuse = iocg->inuse;	/* NVMe 사용 가중치 조정 시작점 */
	adj_step = DIV_ROUND_UP(iocg->active * INUSE_ADJ_STEP_PCT, 100);	/* inuse 25% 단위 NVMe 시간 회복 */
	do {
		new_inuse = new_inuse + adj_step;		/* NVMe 사용 가중치 단계적 증가 */
		propagate_weights(iocg, iocg->active, new_inuse, true, now);		/* 상위로 NVMe 사용 가중치 전파 */
		current_hweight(iocg, NULL, &hwi);		/* 증가된 NVMe 사용 비율로 cost 재계산 */
		cost = abs_cost_to_cost(abs_cost, hwi);		/* 새로운 NVMe 사용 비율로 bio cost */
	} while (time_after64(vtime + cost, now->vnow) &&	/* bio 발행이 NVMe vnow를 초과하면 반복 */
		 iocg->inuse != iocg->active);

	spin_unlock_irqrestore(&ioc->lock, flags);

	TRACE_IOCG_PATH(inuse_adjust, iocg, now,
			old_inuse, iocg->inuse, old_hwi, hwi);

	return cost;
}

/*
 * calc_vtime_cost_builtin - bio의 선형 비용 모델 기반 vtime 비용 계산
 *
 * bio의 READ/WRITE, 섹터 수, 직전 cursor와의 거리를 이용해
 * NVMe에서 처리될 예상 시간을 vtime 단위로 산출한다.
 * seek 거리가 LCOEF_RANDIO_PAGES(4096페이지=16MB)를 초과하면
 * random IO로 간주해 더 높은 coef_randio를 적용한다(추정).
 */
static void calc_vtime_cost_builtin(struct bio *bio, struct ioc_gq *iocg,
				    bool is_merge, u64 *costp)
{
	struct ioc *ioc = iocg->ioc;
	u64 coef_seqio, coef_randio, coef_page;
	u64 pages = max_t(u64, bio_sectors(bio) >> IOC_SECT_TO_PAGE_SHIFT, 1);	/* bio 크기를 NVMe PRP/SGL 4KB 페이지 수로 */
	u64 seek_pages = 0;
	u64 cost = 0;

	/* Can't calculate cost for empty bio */
	if (!bio->bi_iter.bi_size)	/* 0-byte bio는 NVMe 명령 비용 0 */
		goto out;

	switch (bio_op(bio)) {
	case REQ_OP_READ:	/* read bio: NVMe read 명령 coef 적용 */
		coef_seqio	= ioc->params.lcoefs[LCOEF_RSEQIO];
		coef_randio	= ioc->params.lcoefs[LCOEF_RRANDIO];
		coef_page	= ioc->params.lcoefs[LCOEF_RPAGE];
		break;
	case REQ_OP_WRITE:	/* write bio: NVMe write/fused 명령 coef 적용 */
		coef_seqio	= ioc->params.lcoefs[LCOEF_WSEQIO];
		coef_randio	= ioc->params.lcoefs[LCOEF_WRANDIO];
		coef_page	= ioc->params.lcoefs[LCOEF_WPAGE];
		break;
	default:	/* discard/write-zeroes/flush 등: NVMe admin/io opcode 중 비용 모델 미지원(추정) */
		goto out;
	}

	if (iocg->cursor) {	/* 직전 bio 끝이 있으면 NVMe seq/rand 판별 */
		seek_pages = abs(bio->bi_iter.bi_sector - iocg->cursor);		/* 논리적 LBA 거리: NVMe NS 단위 offset 차이(추정) */
		seek_pages >>= IOC_SECT_TO_PAGE_SHIFT;
	}

	if (!is_merge) {	/* 신규 NVMe 명령이면 seq/rand 기본 비용 추가 */
		if (seek_pages > LCOEF_RANDIO_PAGES) {		/* 16MB 이상 seek: NVMe random IO latency 반영 */
			cost += coef_randio;
		} else {
			cost += coef_seqio;
		}
	}
	cost += pages * coef_page;
out:
	*costp = cost;
}

static u64 calc_vtime_cost(struct bio *bio, struct ioc_gq *iocg, bool is_merge)
{
	u64 cost;

	calc_vtime_cost_builtin(bio, iocg, is_merge, &cost);
	return cost;
}

static void calc_size_vtime_cost_builtin(struct request *rq, struct ioc *ioc,
					 u64 *costp)
{
	unsigned int pages = blk_rq_stats_sectors(rq) >> IOC_SECT_TO_PAGE_SHIFT;	/* request의 NVMe PRP/SGL 페이지 수 */

	switch (req_op(rq)) {
	case REQ_OP_READ:
		*costp = pages * ioc->params.lcoefs[LCOEF_RPAGE];
		break;
	case REQ_OP_WRITE:
		*costp = pages * ioc->params.lcoefs[LCOEF_WPAGE];
		break;
	default:
		*costp = 0;
	}
}

static u64 calc_size_vtime_cost(struct request *rq, struct ioc *ioc)
{
	u64 cost;

	calc_size_vtime_cost_builtin(rq, ioc, &cost);
	return cost;
}

/*
 * ioc_rqos_throttle - bio가 blk-mq/NVMe로 날아가기 전 예산 검사 및 쓰로틀
 * @rqos: RQ_QOS_COST 핸들
 * @bio: 사용자가 방금 제출한 bio
 *
 * 호출 경로: submit_bio_noacct -> blk_mq_submit_bio -> rq_qos_throttle ->
 *           ioc_rqos_throttle -> (budget OK) -> blk_mq_get_request ->
 *           nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * bio의 방향(READ/WRITE), 크기, cursor와의 거리를 바탕으로 NVMe 처리
 * 예상 시간(abs_cost)을 산출한다. cgroup의 vtime 예산이 충분하면
 * bio를 커밋하고 blk-mq로 통과시킨다. 예산이 부족하면 issuer를
 * waitq에 잠재우거나, root/신호 수신 bio는 debt로 처리해 우선순위
 * 역전을 방지한다.
 */
static void ioc_rqos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg = bio->bi_blkg;	/* bio의 blk-cgroup: NVMe SQ 제출률 분리 기준 */
	struct ioc *ioc = rqos_to_ioc(rqos);	/* 이 bio가 속한 NVMe 장치의 iocost 컨트롤러 */
	struct ioc_gq *iocg = blkg_to_iocg(blkg);	/* 장치-cgroup 쌍의 NVMe 예산 상태 */
	struct ioc_now now;
	struct iocg_wait wait;	/* NVMe 예산 부족 시 잠들 wait 엔트리 */
	u64 abs_cost, cost, vtime;	/* abs_cost=NVMe 절대 비용, cost=할당비율 적용 비용, vtime=현재 issued */
	bool use_debt, ioc_locked;	/* debt 사용 시 ioc->lock 필요: NVMe 부채/주기 보호 */
	unsigned long flags;

	/* bypass IOs if disabled, still initializing, or for root cgroup */
	if (!ioc->enabled || !iocg || !iocg->level)	/* iocost 미활성/root cgroup은 NVMe throttling bypass */
		return;

	/* calculate the absolute vtime cost */
	abs_cost = calc_vtime_cost(bio, iocg, false);	/* bio의 NVMe 예상 처리 시간(절대값) */
	if (!abs_cost)	/* 0이면 NVMe throttling 대상 외 bio */
		return;

	if (!iocg_activate(iocg, &now))	/* NVMe 활성화 실패(낮부 node IO) 시 bypass */
		return;

	iocg->cursor = bio_end_sector(bio);	/* 다음 bio의 NVMe seq/rand 판별 기준 갱신 */
	vtime = atomic64_read(&iocg->vtime);
	cost = adjust_inuse_and_calc_cost(iocg, vtime, abs_cost, &now);	/* NVMe 예산 내이면 cost 확정, 아니면 inuse 회복 시도 */

	/*
	 * If no one's waiting and within budget, issue right away.  The
	 * tests are racy but the races aren't systemic - we only miss once
	 * in a while which is fine.
	 */
	if (!waitqueue_active(&iocg->waitq) && !iocg->abs_vdebt &&	/* NVMe 예산 대기/부채/지연/idle 모두 없으면 skip */
	    time_before_eq64(vtime + cost, now.vnow)) {	/* NVMe vtime 예산 내 */
		iocg_commit_bio(iocg, bio, abs_cost, cost);		/* NVMe 제출 예산 확정 → blk_mq_get_request 진행 */
		return;
	}

	/*
	 * We're over budget. This can be handled in two ways. IOs which may
	 * cause priority inversions are punted to @ioc->aux_iocg and charged as
	 * debt. Otherwise, the issuer is blocked on @iocg->waitq. Debt handling
	 * requires @ioc->lock, waitq handling @iocg->waitq.lock. Determine
	 * whether debt handling is needed and acquire locks accordingly.
	 */
	use_debt = bio_issue_as_root_blkg(bio) || fatal_signal_pending(current);	/* root/신호 수신 bio: NVMe 제출을 차단하면 시스템 데드락(추정) */
	ioc_locked = use_debt || READ_ONCE(iocg->abs_vdebt);	/* READ_ONCE: NVMe 부채 존재 시 ioc->lock 획득 결정 */
retry_lock:	/* NVMe 가중치/부채 변경에 따라 lock 범위 재조정 후 재시도 */
	iocg_lock(iocg, ioc_locked, &flags);	/* NVMe 예산/부채/대기 상태 보호 */

	/*
	 * @iocg must stay activated for debt and waitq handling. Deactivation
	 * is synchronized against both ioc->lock and waitq.lock and we won't
	 * get deactivated as long as we're waiting or has debt, so we're good
	 * if we're activated here. In the unlikely cases that we aren't, just
	 * issue the IO.
	 */
	if (unlikely(list_empty(&iocg->active_list))) {	/* race로 NVMe 비활성화되면 그냥 통과 */
		iocg_unlock(iocg, ioc_locked, &flags);		/* NVMe 예산 lock 해제 */
		iocg_commit_bio(iocg, bio, abs_cost, cost);
		return;
	}

	/*
	 * We're over budget. If @bio has to be issued regardless, remember
	 * the abs_cost instead of advancing vtime. iocg_kick_waitq() will pay
	 * off the debt before waking more IOs.
	 *
	 * This way, the debt is continuously paid off each period with the
	 * actual budget available to the cgroup. If we just wound vtime, we
	 * would incorrectly use the current hw_inuse for the entire amount
	 * which, for example, can lead to the cgroup staying blocked for a
	 * long time even with substantially raised hw_inuse.
	 *
	 * An iocg with vdebt should stay online so that the timer can keep
	 * deducting its vdebt and [de]activate use_delay mechanism
	 * accordingly. We don't want to race against the timer trying to
	 * clear them and leave @iocg inactive w/ dangling use_delay heavily
	 * penalizing the cgroup and its descendants.
	 */
	if (use_debt) {	/* root/fatal signal: NVMe 부채로 처리해 우선 발행 */
		iocg_incur_debt(iocg, abs_cost, &now);		/* NVMe 부채 누적, vtime은 미리 차감 안 함 */
		if (iocg_kick_delay(iocg, &now))		/* 부채가 너무 커지면 blkcg use_delay로 NVMe 제출 억제 */
			blkcg_schedule_throttle(rqos->disk,			/* process-schedule 시점에 NVMe 제출 지연(추정) */
					(bio->bi_opf & REQ_SWAP) == REQ_SWAP);
		iocg_unlock(iocg, ioc_locked, &flags);
		return;
	}

	/* guarantee that iocgs w/ waiters have maximum inuse */	/* NVMe 예산 대기 중인 cgroup은 최대 할당분 사용 */
	if (!iocg->abs_vdebt && iocg->inuse != iocg->active) {	/* 부채 아니면 active까지 inuse 복원 */
		if (!ioc_locked) {		/* ioc->lock 없이는 NVMe 가중치 변경 불가 */
			iocg_unlock(iocg, false, &flags);			/* waitq.lock만 해제 */
			ioc_locked = true;			/* ioc->lock 획득 후 NVMe 가중치 조정 재시도 */
			goto retry_lock;			/* NVMe 가중치 변경을 위해 lock 다시 획득 */
		}
		propagate_weights(iocg, iocg->active, iocg->active, true,		/* inuse=active로 NVMe 예산 최대화 */
				  &now);
	}

	/*
	 * Append self to the waitq and schedule the wakeup timer if we're
	 * the first waiter.  The timer duration is calculated based on the
	 * current vrate.  vtime and hweight changes can make it too short
	 * or too long.  Each wait entry records the absolute cost it's
	 * waiting for to allow re-evaluation using a custom wait entry.
	 *
	 * If too short, the timer simply reschedules itself.  If too long,
	 * the period timer will notice and trigger wakeups.
	 *
	 * All waiters are on iocg->waitq and the wait states are
	 * synchronized using waitq.lock.
	 */
	init_wait_func(&wait.wait, iocg_wake_fn);	/* wait 엔트리: 예산 회복 시 iocg_wake_fn -> blk-mq/NVMe 재진입 */
	wait.bio = bio;	/* NVMe 제출 대기 중인 bio */
	wait.abs_cost = abs_cost;	/* 깨어날 때 NVMe 절대 비용으로 cost 재계산 */
	wait.committed = false;	/* will be set true by waker */	/* waker가 NVMe 예산 확정 시 true */

	__add_wait_queue_entry_tail(&iocg->waitq, &wait.wait);	/* NVMe 예산 부족 bio를 FIFO 순서로 대기 */
	iocg_kick_waitq(iocg, ioc_locked, &now);	/* 기존 대기자 처리 및 waitq_timer 재설정 */

	iocg_unlock(iocg, ioc_locked, &flags);

	while (true) {	/* NVMe 예산 확정까지 issuer 대기 */
		set_current_state(TASK_UNINTERRUPTIBLE);		/* signal에도 깨지 않음: NVMe 제출 순서 보장 */
		if (wait.committed)		/* waker가 NVMe 예산 확정 완료 */
			break;
		io_schedule();		/* IO scheduler에 양보: 다른 태스크의 NVMe 제출 기회 확보 */
	}

	/* waker already committed us, proceed */
	finish_wait(&iocg->waitq, &wait.wait);	/* waitq 정리 후 blk-mq -> nvme_queue_rq 진행 */
}

/*
 * ioc_rqos_merge - bio가 기존 request에 병합될 때 비용 처리
 *
 * bio가 blk-mq의 기존 request와 병합되면 추가 크기/seek 비용만큼
 * vtime을 차감한다. 예산이 부족하면 debt로 처리하여 NVMe SQ에
 * 이미 들어간 request 병합은 계속 허용한다.
 */
static void ioc_rqos_merge(struct rq_qos *rqos, struct request *rq,
			   struct bio *bio)
{
	struct ioc_gq *iocg = blkg_to_iocg(bio->bi_blkg);	/* 병합 bio의 blk-cgroup NVMe 예산 상태 */
	struct ioc *ioc = rqos_to_ioc(rqos);
	sector_t bio_end = bio_end_sector(bio);	/* 병합 후 NVMe LBA 끝: cursor 갱신용 */
	struct ioc_now now;
	u64 vtime, abs_cost, cost;
	unsigned long flags;

	/* bypass if disabled, still initializing, or for root cgroup */
	if (!ioc->enabled || !iocg || !iocg->level)
		return;

	abs_cost = calc_vtime_cost(bio, iocg, true);	/* 병합으로 추가된 NVMe 비용만 계산 */
	if (!abs_cost)
		return;

	ioc_now(ioc, &now);

	vtime = atomic64_read(&iocg->vtime);
	cost = adjust_inuse_and_calc_cost(iocg, vtime, abs_cost, &now);

	/* update cursor if backmerging into the request at the cursor */
	if (blk_rq_pos(rq) < bio_end &&	/* back-merge 범위 확인 */
	    blk_rq_pos(rq) + blk_rq_sectors(rq) == iocg->cursor)	/* request 끝이 cursor와 일치하면 sequential 확장 */
		iocg->cursor = bio_end;		/* NVMe sequential cursor 확장 */

	/*
	 * Charge if there's enough vtime budget and the existing request has
	 * cost assigned.
	 */
	if (rq->bio && rq->bio->bi_iocost_cost &&	/* 기존 request가 NVMe 예산을 차지 중이면 병합 비용도 차감 */
	    time_before_eq64(atomic64_read(&iocg->vtime) + cost, now.vnow)) {	/* 병합 후에도 NVMe 예산 내 */
		iocg_commit_bio(iocg, bio, abs_cost, cost);
		return;
	}

	/*
	 * Otherwise, account it as debt if @iocg is online, which it should
	 * be for the vast majority of cases. See debt handling in
	 * ioc_rqos_throttle() for details.
	 */
	spin_lock_irqsave(&ioc->lock, flags);
	spin_lock(&iocg->waitq.lock);	/* 각 cgroup의 NVMe 예산/대기 상태 보호 */

	if (likely(!list_empty(&iocg->active_list))) {	/* 활성 상태면 병합 bio를 NVMe 부채로 처리 */
		iocg_incur_debt(iocg, abs_cost, &now);
		if (iocg_kick_delay(iocg, &now))
			blkcg_schedule_throttle(rqos->disk,
					(bio->bi_opf & REQ_SWAP) == REQ_SWAP);
	} else {
		iocg_commit_bio(iocg, bio, abs_cost, cost);
	}

	spin_unlock(&iocg->waitq.lock);
	spin_unlock_irqrestore(&ioc->lock, flags);
}

/*
 * ioc_rqos_done_bio - bio 완료 시 done_vtime 전진
 *
 * NVMe CQ 처리 경로에서 bio 단위 완료 시 호출된다. bio에 기록된
 * bi_iocost_cost만큼 done_vtime을 증가시켜 in-flight 비용을 감소.
 */
static void ioc_rqos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct ioc_gq *iocg = blkg_to_iocg(bio->bi_blkg);

	if (iocg && bio->bi_iocost_cost)	/* bio가 NVMe 비용을 가지고 있으면 */
		atomic64_add(bio->bi_iocost_cost, &iocg->done_vtime);		/* atomic: NVMe CQ 완료로 in-flight 비용 감소 */
}

/*
 * ioc_rqos_done - request 완료 시 latency QoS 및 rq_wait 통계 기록
 *
 * NVMe CQ에서 request 완료 시 호출된다. rq->alloc_time_ns와
 * rq->start_time_ns 차이로 request 할당 대기 시간(rq_wait_ns)을,
 * 현재 시각과 alloc_time_ns 차이로 NVMe 큐+디바이스 상 체류 시간을
 * 측정한다. 설정된 latency QoS 목표를 초과하면 nr_missed를 증가.
 */
static void ioc_rqos_done(struct rq_qos *rqos, struct request *rq)
{
	struct ioc *ioc = rqos_to_ioc(rqos);
	struct ioc_pcpu_stat *ccs;	/* NVMe CQ 완료 CPU의 per-CPU 통계 */
	u64 on_q_ns, rq_wait_ns, size_nsec;	/* on_q_ns=NVMe 큐+디바이스 체류, rq_wait_ns=request 할당 대기 */
	int pidx, rw;	/* read/write NVMe latency QoS 인덱스 */

	if (!ioc->enabled || !rq->alloc_time_ns || !rq->start_time_ns)	/* iocost 비활성이거나 rq 시간 미기록 시 skip */
		return;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		pidx = QOS_RLAT;
		rw = READ;		/* read 방향 NVMe 통계 */
		break;
	case REQ_OP_WRITE:
		pidx = QOS_WLAT;
		rw = WRITE;		/* write 방향 NVMe 통계 */
		break;
	default:
		return;
	}

	on_q_ns = blk_time_get_ns() - rq->alloc_time_ns;	/* request 할당(NVMe tag CID) ~ CQ 완료 총시간 */
	rq_wait_ns = rq->start_time_ns - rq->alloc_time_ns;	/* request 할당 대기: NVMe tag/sbitmap 대기 시간(추정) */
	size_nsec = div64_u64(calc_size_vtime_cost(rq, ioc), VTIME_PER_NSEC);	/* request 크기에 비례하는 NVMe 기본 전송 시간 */

	ccs = get_cpu_ptr(ioc->pcpu_stat);	/* 현재 CPU(추정 NVMe CQ 처리 CPU)의 통계 */

	if (on_q_ns <= size_nsec ||	/* 데이터 전송 시간 이하이거나(추정) */
	    on_q_ns - size_nsec <= ioc->params.qos[pidx] * NSEC_PER_USEC)	/* NVMe latency QoS 목표 이내 */
		local_inc(&ccs->missed[rw].nr_met);		/* local: NVMe latency QoS 달성 카운트 */
	else
		local_inc(&ccs->missed[rw].nr_missed);		/* local: NVMe latency QoS 미달 카운트 */

	local64_add(rq_wait_ns, &ccs->rq_wait_ns);	/* local64: NVMe request 할당 대기 시간 누적 */

	put_cpu_ptr(ccs);	/* preemption 복원: NVMe 통계 per-CPU 일관성 */
}

/*
 * ioc_rqos_queue_depth_changed - 큐 깊이 변경 시 파라미터 재조정
 *
 * NVMe 큐 깊이가 바뀌면(예: nr_hw_queues 변경) ioc_refresh_params()를
 * 다시 호출해 AUTOP_SSD_QD1 등 프로파일을 재선택한다.
 */
static void ioc_rqos_queue_depth_changed(struct rq_qos *rqos)
{
	struct ioc *ioc = rqos_to_ioc(rqos);

	spin_lock_irq(&ioc->lock);
	ioc_refresh_params(ioc, false);
	spin_unlock_irq(&ioc->lock);
}

/*
 * ioc_rqos_exit - iocost 컨트롤러 종료
 */
static void ioc_rqos_exit(struct rq_qos *rqos)
{
	struct ioc *ioc = rqos_to_ioc(rqos);

	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_iocost);	/* blk-cgroup에서 iocost 분리 */

	spin_lock_irq(&ioc->lock);
	ioc->running = IOC_STOP;	/* NVMe 제어 타이머 정지 예고 */
	spin_unlock_irq(&ioc->lock);

	timer_shutdown_sync(&ioc->timer);	/* NVMe 주기 타이머 동기 종료 */
	free_percpu(ioc->pcpu_stat);
	kfree(ioc);
}

static const struct rq_qos_ops ioc_rqos_ops = {
	.throttle = ioc_rqos_throttle,
	.merge = ioc_rqos_merge,
	.done_bio = ioc_rqos_done_bio,
	.done = ioc_rqos_done,
	.queue_depth_changed = ioc_rqos_queue_depth_changed,
	.exit = ioc_rqos_exit,
};

/*
 * blk_iocost_init - 디스크에 iocost 컨트롤러 초기화 및 RQ_QOS_COST 등록
 *
 * gendisk별로 ioc를 할당하고, autop 프로파일을 선택한 뒤
 * rq_qos_add()로 blk-mq의 RQ_QOS_COST 체인에 등록한다.
 * 이후 blkcg_activate_policy()로 cgroup별 pd 초기화를 연결한다.
 */
static int blk_iocost_init(struct gendisk *disk)
{
	struct ioc *ioc;
	int i, cpu, ret;

	ioc = kzalloc_obj(*ioc);	/* NVMe 장치당 하나의 iocost 컨트롤러 */
	if (!ioc)
		return -ENOMEM;

	ioc->pcpu_stat = alloc_percpu(struct ioc_pcpu_stat);	/* per-CPU NVMe CQ 완료/대기 통계 */
	if (!ioc->pcpu_stat) {
		kfree(ioc);
		return -ENOMEM;
	}

	for_each_possible_cpu(cpu) {
		struct ioc_pcpu_stat *ccs = per_cpu_ptr(ioc->pcpu_stat, cpu);

		for (i = 0; i < ARRAY_SIZE(ccs->missed); i++) {
			local_set(&ccs->missed[i].nr_met, 0);
			local_set(&ccs->missed[i].nr_missed, 0);
		}
		local64_set(&ccs->rq_wait_ns, 0);		/* NVMe rq_wait_ns 0 */
	}

	spin_lock_init(&ioc->lock);
	timer_setup(&ioc->timer, ioc_timer_fn, 0);	/* NVMe 주기 타이머 핸들러 등록 */
	INIT_LIST_HEAD(&ioc->active_iocgs);	/* NVMe 활성 cgroup 목록 초기화 */

	ioc->running = IOC_IDLE;
	ioc->vtime_base_rate = VTIME_PER_USEC;
	atomic64_set(&ioc->vtime_rate, VTIME_PER_USEC);
	seqcount_spinlock_init(&ioc->period_seqcount, &ioc->lock);	/* NVMe 주기 시계 seqcount 초기화 */
	ioc->period_at = ktime_to_us(blk_time_get());
	atomic64_set(&ioc->cur_period, 0);	/* NVMe 주기 번호 0 */
	atomic_set(&ioc->hweight_gen, 0);	/* NVMe hweight 캐시 세대 0 */

	spin_lock_irq(&ioc->lock);
	ioc->autop_idx = AUTOP_INVALID;
	ioc_refresh_params_disk(ioc, true, disk);
	spin_unlock_irq(&ioc->lock);

	/*
	 * rqos must be added before activation to allow ioc_pd_init() to
	 * lookup the ioc from q. This means that the rqos methods may get
	 * called before policy activation completion, can't assume that the
	 * target bio has an iocg associated and need to test for NULL iocg.
	 */
	ret = rq_qos_add(&ioc->rqos, disk, RQ_QOS_COST, &ioc_rqos_ops);	/* blk-mq RQ_QOS_COST 체인에 등록: bio -> ioc_rqos_throttle -> NVMe */
	if (ret)
		goto err_free_ioc;

	ret = blkcg_activate_policy(disk, &blkcg_policy_iocost);	/* blk-cgroup과 연결: cgroup별 NVMe 예산 할당 */
	if (ret)
		goto err_del_qos;
	return 0;

err_del_qos:
	rq_qos_del(&ioc->rqos);
err_free_ioc:
	free_percpu(ioc->pcpu_stat);
	kfree(ioc);
	return ret;
}

/*
 * ioc_cpd_alloc - cgroup별 기본 weight 데이터 할당
 */
static struct blkcg_policy_data *ioc_cpd_alloc(gfp_t gfp)
{
	struct ioc_cgrp *iocc;

	iocc = kzalloc_obj(struct ioc_cgrp, gfp);
	if (!iocc)
		return NULL;

	iocc->dfl_weight = CGROUP_WEIGHT_DFL * WEIGHT_ONE;
	return &iocc->cpd;
}

static void ioc_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(container_of(cpd, struct ioc_cgrp, cpd));
}

/*
 * ioc_pd_alloc - 장치-cgroup 쌍별 ioc_gq 할당
 */
static struct blkg_policy_data *ioc_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	int levels = blkcg->css.cgroup->level + 1;
	struct ioc_gq *iocg;

	iocg = kzalloc_node(struct_size(iocg, ancestors, levels), gfp,	/* cgroup 계층 깊이만큼 NVMe 조상 포인터 할당 */
			    disk->node_id);			/* NVMe 장치 NUMA node에 맞춤 메모리 할당 */
	if (!iocg)
		return NULL;

	iocg->pcpu_stat = alloc_percpu_gfp(struct iocg_pcpu_stat, gfp);	/* cgroup별 per-CPU NVMe 사용량 */
	if (!iocg->pcpu_stat) {
		kfree(iocg);
		return NULL;
	}

	return &iocg->pd;
}

/*
 * ioc_pd_init - 새로 생성된 ioc_gq 초기화
 *
 * blkg 생성 시 호출되어, ioc_gq의 vtime/done_vtime을 현재 vnow로
 * 설정하고, cgroup 계층의 ancestors[]를 구성한 뒤 초기 weight를
 * 전파한다.
 */
static void ioc_pd_init(struct blkg_policy_data *pd)
{
	struct ioc_gq *iocg = pd_to_iocg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(&iocg->pd);
	struct ioc *ioc = q_to_ioc(blkg->q);
	struct ioc_now now;
	struct blkcg_gq *tblkg;
	unsigned long flags;

	ioc_now(ioc, &now);

	iocg->ioc = ioc;
	atomic64_set(&iocg->vtime, now.vnow);	/* atomic: 초기 NVMe issued vtime을 현재 vnow로 */
	atomic64_set(&iocg->done_vtime, now.vnow);	/* atomic: 초기 NVMe completed vtime 동기화 */
	atomic64_set(&iocg->active_period, atomic64_read(&ioc->cur_period));	/* atomic: 현재 NVMe 주기로 활성 스탬프 */
	INIT_LIST_HEAD(&iocg->active_list);
	INIT_LIST_HEAD(&iocg->walk_list);
	INIT_LIST_HEAD(&iocg->surplus_list);
	iocg->hweight_active = WEIGHT_ONE;	/* 단일 cgroup 시 100% NVMe 활성 비율 */
	iocg->hweight_inuse = WEIGHT_ONE;	/* 단일 cgroup 시 100% NVMe 사용 비율 */

	init_waitqueue_head(&iocg->waitq);
	hrtimer_setup(&iocg->waitq_timer, iocg_waitq_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);	/* NVMe 예산 회복 monotonic 타이머 */

	iocg->level = blkg->blkcg->css.cgroup->level;

	for (tblkg = blkg; tblkg; tblkg = tblkg->parent) {	/* blk-cgroup 계층을 따라 NVMe 조상 ioc_gq 포인터 저장 */
		struct ioc_gq *tiocg = blkg_to_iocg(tblkg);		/* 조상 cgroup의 NVMe 예산 상태 */
		iocg->ancestors[tiocg->level] = tiocg;		/* ancestors[]에 NVMe 계층 위치 기록 */
	}

	spin_lock_irqsave(&ioc->lock, flags);
	weight_updated(iocg, &now);	/* 초기 weight를 상위로 전파해 NVMe hweight 계산 준비 */
	spin_unlock_irqrestore(&ioc->lock, flags);
}

/*
 * ioc_pd_free - ioc_gq 해제
 *
 * blkg 소멸 시 active_list에 남아있으면 weight를 0으로 전파하고
 * 제거한다. waitq_timer를 취소한 뒤 메모리를 반환한다.
 */
static void ioc_pd_free(struct blkg_policy_data *pd)
{
	struct ioc_gq *iocg = pd_to_iocg(pd);
	struct ioc *ioc = iocg->ioc;
	unsigned long flags;

	if (ioc) {
		spin_lock_irqsave(&ioc->lock, flags);

		if (!list_empty(&iocg->active_list)) {
			struct ioc_now now;

			ioc_now(ioc, &now);
			propagate_weights(iocg, 0, 0, false, &now);
			list_del_init(&iocg->active_list);
		}

		WARN_ON_ONCE(!list_empty(&iocg->walk_list));
		WARN_ON_ONCE(!list_empty(&iocg->surplus_list));

		spin_unlock_irqrestore(&ioc->lock, flags);

		hrtimer_cancel(&iocg->waitq_timer);	/* NVMe 예산 회복 타이머 취소 */
	}
	free_percpu(iocg->pcpu_stat);
	kfree(iocg);
}

/*
 * ioc_pd_stat - cgroup 통계 출력
 */
static void ioc_pd_stat(struct blkg_policy_data *pd, struct seq_file *s)
{
	struct ioc_gq *iocg = pd_to_iocg(pd);
	struct ioc *ioc = iocg->ioc;

	if (!ioc->enabled)
		return;

	if (iocg->level == 0) {	/* root cgroup 출력: 전체 NVMe vrate */
		unsigned vp10k = DIV64_U64_ROUND_CLOSEST(
			ioc->vtime_base_rate * 10000,		/* vrate * 10000 */
			VTIME_PER_USEC);		/* 1.0 기준으로 정규화: NVMe 상대 IO 속도 */
		seq_printf(s, " cost.vrate=%u.%02u", vp10k / 100, vp10k % 100);
	}

	seq_printf(s, " cost.usage=%llu", iocg->last_stat.usage_us);

	if (blkcg_debug_stats)
		seq_printf(s, " cost.wait=%llu cost.indebt=%llu cost.indelay=%llu",
			iocg->last_stat.wait_us,
			iocg->last_stat.indebt_us,
			iocg->last_stat.indelay_us);
}

static u64 ioc_weight_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			     int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioc_gq *iocg = pd_to_iocg(pd);

	if (dname && iocg->cfg_weight)
		seq_printf(sf, "%s %u\n", dname, iocg->cfg_weight / WEIGHT_ONE);
	return 0;
}


/*
 * ioc_weight_show / ioc_weight_write - cgroup weight sysfs 인터페이스
 */
static int ioc_weight_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkcg);

	seq_printf(sf, "default %u\n", iocc->dfl_weight / WEIGHT_ONE);
	blkcg_print_blkgs(sf, blkcg, ioc_weight_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	return 0;
}

static ssize_t ioc_weight_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkcg);
	struct blkg_conf_ctx ctx;
	struct ioc_now now;
	struct ioc_gq *iocg;
	u32 v;
	int ret;

	if (!strchr(buf, ':')) {
		struct blkcg_gq *blkg;

		if (!sscanf(buf, "default %u", &v) && !sscanf(buf, "%u", &v))
			return -EINVAL;

		if (v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX)
			return -EINVAL;

		spin_lock_irq(&blkcg->lock);
		iocc->dfl_weight = v * WEIGHT_ONE;
		hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
			struct ioc_gq *iocg = blkg_to_iocg(blkg);

			if (iocg) {
				spin_lock(&iocg->ioc->lock);
				ioc_now(iocg->ioc, &now);
				weight_updated(iocg, &now);
				spin_unlock(&iocg->ioc->lock);
			}
		}
		spin_unlock_irq(&blkcg->lock);

		return nbytes;
	}

	blkg_conf_init(&ctx, buf);

	ret = blkg_conf_prep(blkcg, &blkcg_policy_iocost, &ctx);
	if (ret)
		goto err;

	iocg = blkg_to_iocg(ctx.blkg);

	if (!strncmp(ctx.body, "default", 7)) {
		v = 0;
	} else {
		if (!sscanf(ctx.body, "%u", &v))
			goto einval;
		if (v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX)
			goto einval;
	}

	spin_lock(&iocg->ioc->lock);
	iocg->cfg_weight = v * WEIGHT_ONE;
	ioc_now(iocg->ioc, &now);
	weight_updated(iocg, &now);
	spin_unlock(&iocg->ioc->lock);

	blkg_conf_exit(&ctx);
	return nbytes;

einval:
	ret = -EINVAL;
err:
	blkg_conf_exit(&ctx);
	return ret;
}

static u64 ioc_qos_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			  int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioc *ioc = pd_to_iocg(pd)->ioc;

	if (!dname)
		return 0;

	spin_lock(&ioc->lock);
	seq_printf(sf, "%s enable=%d ctrl=%s rpct=%u.%02u rlat=%u wpct=%u.%02u wlat=%u min=%u.%02u max=%u.%02u\n",
		   dname, ioc->enabled, ioc->user_qos_params ? "user" : "auto",
		   ioc->params.qos[QOS_RPPM] / 10000,
		   ioc->params.qos[QOS_RPPM] % 10000 / 100,
		   ioc->params.qos[QOS_RLAT],
		   ioc->params.qos[QOS_WPPM] / 10000,
		   ioc->params.qos[QOS_WPPM] % 10000 / 100,
		   ioc->params.qos[QOS_WLAT],
		   ioc->params.qos[QOS_MIN] / 10000,
		   ioc->params.qos[QOS_MIN] % 10000 / 100,
		   ioc->params.qos[QOS_MAX] / 10000,
		   ioc->params.qos[QOS_MAX] % 10000 / 100);
	spin_unlock(&ioc->lock);
	return 0;
}

/*
 * ioc_qos_show / ioc_qos_write - latency QoS 및 enable sysfs 인터페이스
 *
 * io.cost.qos를 통해 NVMe 완료 지연 목표(rpct/rlat, wpct/wlat)와
 * vrate 범위(min/max), 컨트롤러 enable을 설정한다.
 */
static int ioc_qos_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));

	blkcg_print_blkgs(sf, blkcg, ioc_qos_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	return 0;
}

static const match_table_t qos_ctrl_tokens = {
	{ QOS_ENABLE,		"enable=%u"	},
	{ QOS_CTRL,		"ctrl=%s"	},
	{ NR_QOS_CTRL_PARAMS,	NULL		},
};

static const match_table_t qos_tokens = {
	{ QOS_RPPM,		"rpct=%s"	},
	{ QOS_RLAT,		"rlat=%u"	},
	{ QOS_WPPM,		"wpct=%s"	},
	{ QOS_WLAT,		"wlat=%u"	},
	{ QOS_MIN,		"min=%s"	},
	{ QOS_MAX,		"max=%s"	},
	{ NR_QOS_PARAMS,	NULL		},
};

static ssize_t ioc_qos_write(struct kernfs_open_file *of, char *input,
			     size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;
	struct gendisk *disk;
	struct ioc *ioc;
	u32 qos[NR_QOS_PARAMS];
	bool enable, user;
	char *body, *p;
	unsigned long memflags;
	int ret;

	blkg_conf_init(&ctx, input);

	memflags = blkg_conf_open_bdev_frozen(&ctx);
	if (IS_ERR_VALUE(memflags)) {
		ret = memflags;
		goto err;
	}

	body = ctx.body;
	disk = ctx.bdev->bd_disk;
	if (!queue_is_mq(disk->queue)) {
		ret = -EOPNOTSUPP;
		goto err;
	}

	ioc = q_to_ioc(disk->queue);
	if (!ioc) {
		ret = blk_iocost_init(disk);
		if (ret)
			goto err;
		ioc = q_to_ioc(disk->queue);
	}

	blk_mq_quiesce_queue(disk->queue);	/* NVMe queue 일시 정지: qos 변경 중 race 방지 */

	spin_lock_irq(&ioc->lock);
	memcpy(qos, ioc->params.qos, sizeof(qos));
	enable = ioc->enabled;
	user = ioc->user_qos_params;

	while ((p = strsep(&body, " \t\n"))) {
		substring_t args[MAX_OPT_ARGS];
		char buf[32];
		int tok;
		s64 v;

		if (!*p)
			continue;

		switch (match_token(p, qos_ctrl_tokens, args)) {
		case QOS_ENABLE:
			if (match_u64(&args[0], &v))
				goto einval;
			enable = v;
			continue;
		case QOS_CTRL:
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (!strcmp(buf, "auto"))
				user = false;
			else if (!strcmp(buf, "user"))
				user = true;
			else
				goto einval;
			continue;
		}

		tok = match_token(p, qos_tokens, args);
		switch (tok) {
		case QOS_RPPM:
		case QOS_WPPM:
			if (match_strlcpy(buf, &args[0], sizeof(buf)) >=
			    sizeof(buf))
				goto einval;
			if (cgroup_parse_float(buf, 2, &v))
				goto einval;
			if (v < 0 || v > 10000)
				goto einval;
			qos[tok] = v * 100;
			break;
		case QOS_RLAT:
		case QOS_WLAT:
			if (match_u64(&args[0], &v))
				goto einval;
			qos[tok] = v;
			break;
		case QOS_MIN:
		case QOS_MAX:
			if (match_strlcpy(buf, &args[0], sizeof(buf)) >=
			    sizeof(buf))
				goto einval;
			if (cgroup_parse_float(buf, 2, &v))
				goto einval;
			if (v < 0)
				goto einval;
			qos[tok] = clamp_t(s64, v * 100,
					   VRATE_MIN_PPM, VRATE_MAX_PPM);
			break;
		default:
			goto einval;
		}
		user = true;
	}

	if (qos[QOS_MIN] > qos[QOS_MAX])
		goto einval;

	if (enable && !ioc->enabled) {	/* iocost 활성화: NVMe rq_alloc_time 계정 시작 */
		blk_stat_enable_accounting(disk->queue);		/* request 할당/완료 시간 측정 활성화 -> NVMe latency QoS 통계 정확도 향상 */
		blk_queue_flag_set(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);		/* NVMe tag/sbitmap 대기 시간 측정 플래그 설정 */
		ioc->enabled = true;
	} else if (!enable && ioc->enabled) {	/* iocost 비활성화: NVMe 통계 중지 */
		blk_stat_disable_accounting(disk->queue);		/* NVMe 완료/대기 시간 측정 중지 */
		blk_queue_flag_clear(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);		/* NVMe tag/sbitmap 대기 시간 측정 플래그 해제 */
		ioc->enabled = false;
	}

	if (user) {
		memcpy(ioc->params.qos, qos, sizeof(qos));
		ioc->user_qos_params = true;
	} else {
		ioc->user_qos_params = false;
	}

	ioc_refresh_params(ioc, true);
	spin_unlock_irq(&ioc->lock);

	if (enable)	/* iocost 켜지면 wbt는 중복 제어이므로 NVMe writeback throttling 비활성화 */
		wbt_disable_default(disk);		/* wbt 중복 제거: NVMe latency QoS 제어 단일화 */
	else
		wbt_enable_default(disk);		/* wbt 복원: NVMe 제어 없을 때 writeback 조절 */

	blk_mq_unquiesce_queue(disk->queue);	/* NVMe queue 재개: qos 변경 완료 */

	blkg_conf_exit_frozen(&ctx, memflags);
	return nbytes;
einval:
	spin_unlock_irq(&ioc->lock);
	blk_mq_unquiesce_queue(disk->queue);
	ret = -EINVAL;
err:
	blkg_conf_exit_frozen(&ctx, memflags);
	return ret;
}

static u64 ioc_cost_model_prfill(struct seq_file *sf,
				 struct blkg_policy_data *pd, int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioc *ioc = pd_to_iocg(pd)->ioc;
	u64 *u = ioc->params.i_lcoefs;

	if (!dname)
		return 0;

	spin_lock(&ioc->lock);
	seq_printf(sf, "%s ctrl=%s model=linear "
		   "rbps=%llu rseqiops=%llu rrandiops=%llu "
		   "wbps=%llu wseqiops=%llu wrandiops=%llu\n",
		   dname, ioc->user_cost_model ? "user" : "auto",
		   u[I_LCOEF_RBPS], u[I_LCOEF_RSEQIOPS], u[I_LCOEF_RRANDIOPS],
		   u[I_LCOEF_WBPS], u[I_LCOEF_WSEQIOPS], u[I_LCOEF_WRANDIOPS]);
	spin_unlock(&ioc->lock);
	return 0;
}

/*
 * ioc_cost_model_show / ioc_cost_model_write - 비용 모델 sysfs 인터페이스
 *
 * io.cost.model을 통해 NVMe 장치의 rbps/rseqiops/rrandiops/
 * wbps/wseqiops/wrandiops를 사용자가 지정할 수 있다.
 */
static int ioc_cost_model_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));

	blkcg_print_blkgs(sf, blkcg, ioc_cost_model_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	return 0;
}

static const match_table_t cost_ctrl_tokens = {
	{ COST_CTRL,		"ctrl=%s"	},
	{ COST_MODEL,		"model=%s"	},
	{ NR_COST_CTRL_PARAMS,	NULL		},
};

static const match_table_t i_lcoef_tokens = {
	{ I_LCOEF_RBPS,		"rbps=%u"	},
	{ I_LCOEF_RSEQIOPS,	"rseqiops=%u"	},
	{ I_LCOEF_RRANDIOPS,	"rrandiops=%u"	},
	{ I_LCOEF_WBPS,		"wbps=%u"	},
	{ I_LCOEF_WSEQIOPS,	"wseqiops=%u"	},
	{ I_LCOEF_WRANDIOPS,	"wrandiops=%u"	},
	{ NR_I_LCOEFS,		NULL		},
};

static ssize_t ioc_cost_model_write(struct kernfs_open_file *of, char *input,
				    size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;
	struct request_queue *q;
	unsigned int memflags;
	struct ioc *ioc;
	u64 u[NR_I_LCOEFS];
	bool user;
	char *body, *p;
	int ret;

	blkg_conf_init(&ctx, input);

	ret = blkg_conf_open_bdev(&ctx);
	if (ret)
		goto err;

	body = ctx.body;
	q = bdev_get_queue(ctx.bdev);
	if (!queue_is_mq(q)) {
		ret = -EOPNOTSUPP;
		goto err;
	}

	ioc = q_to_ioc(q);
	if (!ioc) {
		ret = blk_iocost_init(ctx.bdev->bd_disk);
		if (ret)
			goto err;
		ioc = q_to_ioc(q);
	}

	memflags = blk_mq_freeze_queue(q);	/* NVMe queue 동결: cost model 변경 중 IO 정지 */
	blk_mq_quiesce_queue(q);	/* NVMe queue 휴양: hctx dispatch 중단 */

	spin_lock_irq(&ioc->lock);
	memcpy(u, ioc->params.i_lcoefs, sizeof(u));
	user = ioc->user_cost_model;

	while ((p = strsep(&body, " \t\n"))) {
		substring_t args[MAX_OPT_ARGS];
		char buf[32];
		int tok;
		u64 v;

		if (!*p)
			continue;

		switch (match_token(p, cost_ctrl_tokens, args)) {
		case COST_CTRL:
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (!strcmp(buf, "auto"))
				user = false;
			else if (!strcmp(buf, "user"))
				user = true;
			else
				goto einval;
			continue;
		case COST_MODEL:
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (strcmp(buf, "linear"))
				goto einval;
			continue;
		}

		tok = match_token(p, i_lcoef_tokens, args);
		if (tok == NR_I_LCOEFS)
			goto einval;
		if (match_u64(&args[0], &v))
			goto einval;
		u[tok] = v;
		user = true;
	}

	if (user) {
		memcpy(ioc->params.i_lcoefs, u, sizeof(u));
		ioc->user_cost_model = true;
	} else {
		ioc->user_cost_model = false;
	}
	ioc_refresh_params(ioc, true);
	spin_unlock_irq(&ioc->lock);

	blk_mq_unquiesce_queue(q);	/* NVMe queue 재개 */
	blk_mq_unfreeze_queue(q, memflags);	/* NVMe queue 동결 해제 */

	blkg_conf_exit(&ctx);
	return nbytes;

einval:
	spin_unlock_irq(&ioc->lock);

	blk_mq_unquiesce_queue(q);
	blk_mq_unfreeze_queue(q, memflags);

	ret = -EINVAL;
err:
	blkg_conf_exit(&ctx);
	return ret;
}

static struct cftype ioc_files[] = {
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = ioc_weight_show,
		.write = ioc_weight_write,
	},
	{
		.name = "cost.qos",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = ioc_qos_show,
		.write = ioc_qos_write,
	},
	{
		.name = "cost.model",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = ioc_cost_model_show,
		.write = ioc_cost_model_write,
	},
	{}
};

static struct blkcg_policy blkcg_policy_iocost = {
	.dfl_cftypes	= ioc_files,
	.cpd_alloc_fn	= ioc_cpd_alloc,
	.cpd_free_fn	= ioc_cpd_free,
	.pd_alloc_fn	= ioc_pd_alloc,
	.pd_init_fn	= ioc_pd_init,
	.pd_free_fn	= ioc_pd_free,
	.pd_stat_fn	= ioc_pd_stat,
};

/*
 * ioc_init / ioc_exit - 모듈 등록/해제
 */
static int __init ioc_init(void)
{
	return blkcg_policy_register(&blkcg_policy_iocost);
}

static void __exit ioc_exit(void)
{
	blkcg_policy_unregister(&blkcg_policy_iocost);
}

module_init(ioc_init);
module_exit(ioc_exit);

/* NVMe 관점 핵심 요약 */
/*
 * - iocost는 blk-mq 상단(RQ_QOS_COST)에서 bio가 NVMe driver/SQ로 날아가기
 *   전에 vtime 예산을 검사해, NVMe SQ/CQ 포화와 latency QoS 저하를
 *   사전에 억제한다.
 * - calc_vtime_cost()는 bio의 READ/WRITE, 크기, cursor 간 거리를 바탕으로
 *   NVMe 처리 예상 시간을 산출하며, sequential/random을 구분해 coef를
 *   다르게 적용한다.
 * - ioc_rqos_done()은 NVMe CQ 완료 시점의 rq_wait_ns와 latency QoS
 *   달성 여부를 per-CPU 통계에 기록, ioc_timer_fn()이 이를 바탕으로
 *   vrate를 조정한다.
 * - busy_level은 rq_wait_pct(software/hardware queue 포화)와 missed_ppm
 *   (완료 지연 QoS 미달)을 조합해 산출되며, 이를 통해 NVMe에 제출되는
 *   전체 IO 압력을 증감한다.
 * - 이 파일은 blk-cgroup, blk-rq-qos, blk-wbt, blk-stat 등 block layer
 *   파일들의 위에서 동작하며, 특히 blk-mq의 rq_qos 체인과 밀접하게
 *   연결된다.
 */
