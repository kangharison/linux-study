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
#include <linux/kernel.h>	/* [한국어] container_of, ALIGN, DIV_ROUND_UP 등 커널 공통 매크로 */
#include <linux/module.h>	/* [한국어] MODULE_LICENSE, module_init/exit: iocost를 적재 가능 모듈로 등록 */
#include <linux/timer.h>	/* [한국어] timer_list, add_timer 등: ioc_timer_fn 주기 타이머 */
#include <linux/time64.h>	/* [한국어] NSEC_PER_SEC, USEC_PER_SEC 등 시간 단위 상수 */
#include <linux/parser.h>	/* [한국어] match_token/match_int: io.cost.qos, io.cost.model sysfs 파싱 */
#include <linux/sched/signal.h>	/* [한국어] signal_pending(): waitq 대기 중 시그널 수신 여부 확인 */
#include <asm/local.h>		/* [한국어] local_t: per-CPU 카운터(nr_met, nr_missed) — 락 없이 CPU-local 증감 */
#include <asm/local64.h>	/* [한국어] local64_t: per-CPU 64비트 카운터(abs_vusage, rq_wait_ns) — 64비트 원자성 보장 */
#include "blk-rq-qos.h"		/* [한국어] rq_qos, rq_qos_ops: iocost가 등록하는 rq-qos 훅 프레임워크 */
#include "blk-stat.h"		/* [한국어] blk_stat_*: 블록 요청 완료 지연 측정 인프라 */
#include "blk-wbt.h"		/* [한국어] wbt_disabled(): writeback throttle 비활성화 여부 확인 */
#include "blk-cgroup.h"		/* [한국어] blkcg_policy, blkg_policy_data: cgroup별 ioc_gq 관리 기반 */

#ifdef CONFIG_TRACEPOINTS
/* [한국어] CONFIG_TRACEPOINTS가 켜진 빌드에서만 트레이스 경로 활성화 */

/* copied from TRACE_CGROUP_PATH, see cgroup-internal.h */
/* [한국어] cgroup 경로를 담을 임시 버퍼 크기: "/sys/fs/cgroup/..." 전체 경로 수용 */
#define TRACE_IOCG_PATH_LEN 1024
static DEFINE_SPINLOCK(trace_iocg_path_lock);	/* [한국어] trace_iocg_path 버퍼를 보호하는 스핀락 — IRQ 컨텍스트에서도 호출되므로 irqsave 필요 */
static char trace_iocg_path[TRACE_IOCG_PATH_LEN];	/* [한국어] cgroup_path() 결과를 담는 전역 버퍼 — trace_iocg_path_lock으로 직렬화 */

/* [한국어] TRACE_IOCG_PATH - iocg의 cgroup 경로를 trace 이벤트에 첨부하는 헬퍼 매크로.
 * trace_iocost_##type##_enabled() 로 먼저 트레이스 활성 여부를 확인해 불필요한
 * cgroup_path() 호출(느림)을 방지한 뒤, 경로 문자열과 함께 trace_iocost_##type() 호출. */
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
/* [한국어] 트레이스포인트 비활성 빌드에서는 빈 문으로 컴파일아웃 */
#define TRACE_IOCG_PATH(type, iocg, ...)	do { } while (0)
#endif	/* CONFIG_TRACE_POINTS */

enum {
	MILLION			= 1000000,	/* [한국어] ppm(parts per million) 계산 기준: QoS 파라미터와 vrate를 백분율 단위로 표현할 때 분모로 사용 */

	/* timer period is calculated from latency requirements, bound it */
	MIN_PERIOD		= USEC_PER_MSEC,	/* [한국어] 주기 타이머 최소 1ms: 너무 짧으면 softirq 폭풍 및 불필요한 vrate 재계산 유발 */
	MAX_PERIOD		= USEC_PER_SEC,		/* [한국어] 주기 타이머 최대 1s: 너무 길면 장치 포화 반응이 지연돼 latency QoS 위반 증가 */

	/*
	 * iocg->vtime is targeted at 50% behind the device vtime, which
	 * serves as its IO credit buffer.  Surplus weight adjustment is
	 * immediately canceled if the vtime margin runs below 10%.
	 */
	MARGIN_MIN_PCT		= 10,	/* [한국어] vtime 여유 10% 미만: 서플러스 가중치 조정을 즉시 취소해 IO 버짓 고갈 방지 */
	MARGIN_LOW_PCT		= 20,	/* [한국어] vtime 여유 20% 미만: inuse 하향 조정 검토 시작점 */
	MARGIN_TARGET_PCT	= 50,	/* [한국어] 목표 vtime 버퍼 50%: 이 수준을 유지해야 burst IO를 지연 없이 흡수 가능 */

	INUSE_ADJ_STEP_PCT	= 25,	/* [한국어] cgroup이 vtime 예산을 회복할 때 inuse를 25%씩 증가 — 너무 크면 오실레이션, 너무 작으면 회복 지연 */

	/* Have some play in timer operations */
	TIMER_SLACK_PCT		= 1,	/* [한국어] waitq 타이머 해상도 여유 1%: hrtimer 정확도 한계를 보정해 조기 만료 방지 */

	/* 1/64k is granular enough and can easily be handled w/ u32 */
	WEIGHT_ONE		= 1 << 16,	/* [한국어] 정규화된 가중치 1.0을 나타내는 고정소수점 값(65536): u32로 충분한 정밀도 확보 */
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
	VTIME_PER_SEC_SHIFT	= 37,	/* [한국어] vtime 1초 = 2^37: sub-nanosecond 정밀도와 수일 단위 wrap-around 시간 동시 확보 */
	VTIME_PER_SEC		= 1LLU << VTIME_PER_SEC_SHIFT,	/* [한국어] 1초를 vtime 단위로 표현한 값 — vtime/VTIME_PER_SEC = 경과 초 수 */
	VTIME_PER_USEC		= VTIME_PER_SEC / USEC_PER_SEC,	/* [한국어] 1마이크로초를 vtime 단위로 환산 — wallclock us를 vtime과 비교할 때 사용 */
	VTIME_PER_NSEC		= VTIME_PER_SEC / NSEC_PER_SEC,	/* [한국어] 1나노초를 vtime 단위로 환산 — ktime_get_ns() 결과와 직접 연산 가능 */

	/* bound vrate adjustments within two orders of magnitude */
	VRATE_MIN_PPM		= 10000,	/* [한국어] vrate 최소 0.01(1%): 장치가 매우 느려도 이 값 이하로 내리지 않아 vtime 진행 정지 방지 */
	VRATE_MAX_PPM		= 100000000,	/* [한국어] vrate 최대 100(10000%): 고속 SSD에서 burst 허용 상한 */

	VRATE_MIN		= VTIME_PER_USEC * VRATE_MIN_PPM / MILLION,	/* [한국어] vrate 절대 하한을 vtime/ns 단위로 미리 계산한 값 */
	VRATE_CLAMP_ADJ_PCT	= 4,	/* [한국어] vrate가 min/max 경계에 걸릴 때 4%씩 완화해 급격한 vrate 진동 방지 */

	/* switch iff the conditions are met for longer than this */
	AUTOP_CYCLE_NSEC	= 10LLU * NSEC_PER_SEC,	/* [한국어] HDD/SSD 자동 프로파일 전환 조건이 10초 연속 충족돼야 실제 전환 — 일시적 부하 변동에 의한 오전환 방지 */
};

enum {
	/* if IOs end up waiting for requests, issue less */
	RQ_WAIT_BUSY_PCT	= 5,	/* [한국어] 한 주기 내 request(tag) 할당 대기 시간이 5% 이상이면 장치 포화로 판단해 vrate 하강 */

	/* unbusy hysterisis */
	UNBUSY_THR_PCT		= 75,	/* [한국어] latency QoS 달성률이 75% 이상이면 포화 아님으로 판단 — busy/unbusy 히스테리시스 */

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
	MIN_DELAY_THR_PCT	= 500,	/* [한국어] vtime 초과가 500%(5배)를 넘으면 blkcg use_delay 유도 시작 — 선형 지연 증가 구간 시작점 */
	MAX_DELAY_THR_PCT	= 25000,	/* [한국어] vtime 초과 25000%(250배)에서 최대 지연 도달 — 이 이상은 지수 감쇠로 전환 */
	MIN_DELAY		= 250,		/* [한국어] 최소 유도 지연 250us: 지연 인가 시 최소 억제 효과를 보장 */
	MAX_DELAY		= 250 * USEC_PER_MSEC,	/* [한국어] 최대 유도 지연 250ms: 과도한 지연으로 인한 응용 타임아웃 방지 */

	/* halve debts if avg usage over 100ms is under 50% */
	DFGV_USAGE_PCT		= 50,	/* [한국어] 100ms 평균 사용률 50% 미만이면 부채를 절반으로 탕감 — 유휴 cgroup이 과거 부채에 묶이지 않도록 */
	DFGV_PERIOD		= 100 * USEC_PER_MSEC,	/* [한국어] 부채 탕감 평가 단위 시간 100ms: 이 창에서 사용률을 측정해 탕감 여부 결정 */

	/* don't let cmds which take a very long time pin lagging for too long */
	MAX_LAGGING_PERIODS	= 10,	/* [한국어] 10주기 이상 완료되지 않은 IO는 lagging 상태로 처리 — 극단적 장수명 IO가 vtime 계산을 왜곡하는 것을 방지 */

	/*
	 * Count IO size in 4k pages.  The 12bit shift helps keeping
	 * size-proportional components of cost calculation in closer
	 * numbers of digits to per-IO cost components.
	 */
	IOC_PAGE_SHIFT		= 12,	/* [한국어] IO 비용을 4KB 단위로 환산: 12비트 shift로 크기 비례 항과 per-IO 기본 비용의 자릿수를 맞춤 */
	IOC_PAGE_SIZE		= 1 << IOC_PAGE_SHIFT,	/* [한국어] 4096바이트: 선형 비용 모델에서 IO 크기를 세는 기본 단위 */
	IOC_SECT_TO_PAGE_SHIFT	= IOC_PAGE_SHIFT - SECTOR_SHIFT,	/* [한국어] 512B 섹터를 4KB 페이지로 변환하는 shift 값(=3): bio->bi_iter.bi_size>>9 로 얻은 섹터 수를 페이지 수로 변환 */

	/* if apart further than 16M, consider randio for linear model */
	LCOEF_RANDIO_PAGES	= 4096,	/* [한국어] 직전 IO 끝 섹터에서 4096 페이지(16MB) 이상 떨어지면 랜덤 IO로 분류 — 선형 비용 모델에서 seek 비용 항 적용 */
};

enum ioc_running {
	IOC_IDLE,	/* [한국어] IO 미발행 상태: 활성 iocg 없음, 주기 타이머 정지 — 첫 bio 도착 시 IOC_RUNNING으로 전환 */
	IOC_RUNNING,	/* [한국어] IO 활성 상태: ioc_timer_fn이 주기적으로 vrate/가중치 재계산 수행 */
	IOC_STOP,	/* [한국어] 컨트롤러 종료 중: iocost_exit() 호출 후 타이머·waitq 정리 진행 중 */
};

/* io.cost.qos controls including per-dev enable of the whole controller */
/* [한국어] io.cost.qos sysfs 항목의 제어 파라미터 인덱스 */
enum {
	QOS_ENABLE,		/* [한국어] iocost 컨트롤러 활성화 여부(0=off, 1=on) — 장치별 독립 설정 */
	QOS_CTRL,		/* [한국어] QoS 제어 모드(auto=자동 프로파일, absolute=직접 지정) */
	NR_QOS_CTRL_PARAMS,	/* [한국어] QoS 제어 파라미터 총 개수 — 배열 크기 정의용 */
};

/* io.cost.qos params */
/* [한국어] io.cost.qos sysfs 항목의 상세 파라미터 인덱스 */
enum {
	QOS_RPPM,	/* [한국어] 읽기 완료 지연 QoS 목표 백분위수(ppm: 1000000=100%) */
	QOS_RLAT,	/* [한국어] 읽기 완료 지연 목표값(us): 이 분위에서 초과 시 장치 포화로 판단 */
	QOS_WPPM,	/* [한국어] 쓰기 완료 지연 QoS 목표 백분위수(ppm) */
	QOS_WLAT,	/* [한국어] 쓰기 완료 지연 목표값(us) */
	QOS_MIN,	/* [한국어] vrate 허용 최솟값(ppm) — 기본값 VRATE_MIN_PPM(10000=1%) */
	QOS_MAX,	/* [한국어] vrate 허용 최댓값(ppm) — 기본값 VRATE_MAX_PPM(100000000=10000%) */
	NR_QOS_PARAMS,	/* [한국어] QoS 파라미터 총 개수 — ioc_params.qos[] 배열 크기 */
};

/* io.cost.model controls */
/* [한국어] io.cost.model sysfs 항목의 제어 파라미터 인덱스 */
enum {
	COST_CTRL,		/* [한국어] 비용 모델 제어 모드(auto=자동 프로파일 선택, linear=직접 계수 지정) */
	COST_MODEL,		/* [한국어] 사용할 비용 모델 종류(현재 linear만 지원) */
	NR_COST_CTRL_PARAMS,	/* [한국어] 비용 제어 파라미터 총 개수 */
};

/* builtin linear cost model coefficients */
/* [한국어] 사용자 입력 선형 비용 계수 인덱스 (i_lcoefs: 장치 최대 IOPS/BPS, ioc_autop_idx_to_params()에서 lcoefs로 변환) */
enum {
	I_LCOEF_RBPS,		/* [한국어] 읽기 최대 대역폭(bytes/s): 순차 읽기 최대 처리량 */
	I_LCOEF_RSEQIOPS,	/* [한국어] 읽기 순차 최대 IOPS: IO당 고정 vtime 비용 계산 기준 */
	I_LCOEF_RRANDIOPS,	/* [한국어] 읽기 랜덤 최대 IOPS: seek 포함 IO당 vtime 비용 기준 */
	I_LCOEF_WBPS,		/* [한국어] 쓰기 최대 대역폭(bytes/s) */
	I_LCOEF_WSEQIOPS,	/* [한국어] 쓰기 순차 최대 IOPS */
	I_LCOEF_WRANDIOPS,	/* [한국어] 쓰기 랜덤 최대 IOPS */
	NR_I_LCOEFS,		/* [한국어] 입력 계수 총 개수 — ioc_params.i_lcoefs[] 크기 */
};

/* [한국어] 내부 선형 비용 계수 인덱스 (lcoefs: VTIME_PER_SEC/[BPS or IOPS] 단위, ioc_cost_model()에서 직접 사용) */
enum {
	LCOEF_RPAGE,	/* [한국어] 읽기 1페이지(4KB)의 vtime 비용: VTIME_PER_SEC/I_LCOEF_RBPS×pagesize 로 유도 */
	LCOEF_RSEQIO,	/* [한국어] 읽기 순차 IO 1회의 기본 vtime 비용: VTIME_PER_SEC/I_LCOEF_RSEQIOPS */
	LCOEF_RRANDIO,	/* [한국어] 읽기 랜덤 IO 1회의 기본 vtime 비용: VTIME_PER_SEC/I_LCOEF_RRANDIOPS */
	LCOEF_WPAGE,	/* [한국어] 쓰기 1페이지(4KB)의 vtime 비용 */
	LCOEF_WSEQIO,	/* [한국어] 쓰기 순차 IO 1회의 기본 vtime 비용 */
	LCOEF_WRANDIO,	/* [한국어] 쓰기 랜덤 IO 1회의 기본 vtime 비용 */
	NR_LCOEFS,	/* [한국어] 내부 계수 총 개수 — ioc_params.lcoefs[] 크기 */
};

/* [한국어] 장치 유형별 자동 프로파일 인덱스 (autop[] 테이블의 첨자, ioc_autop_idx()가 장치 성능 측정 후 결정) */
enum {
	AUTOP_INVALID,	/* [한국어] 미초기화: 아직 장치 유형이 결정되지 않은 상태 — ioc 생성 시 초기값 */
	AUTOP_HDD,	/* [한국어] 회전 디스크: seek 비용 지배, 대역폭 낮음, latency 목표 250ms */
	AUTOP_SSD_QD1,	/* [한국어] SSD 큐 깊이 1 운용 모드: 병렬 IO 없이 순차 발행, latency 목표 25ms */
	AUTOP_SSD_DFL,	/* [한국어] SSD 기본 프로파일: too_fast_vrate_pct=500, latency 목표 25ms */
	AUTOP_SSD_FAST,	/* [한국어] 고속 SSD/NVMe: 높은 IOPS, too_slow_vrate_pct=10, latency 목표 5ms */
};

/*
 * [한국어] ioc_params: 장치별 비용/QoS 파라미터 집합.
 * autop[] 테이블에서 장치 유형(HDD, SSD QD1, SSD 기본/고속)에 따라
 * 초기화되며, sequential/random IOPS와 대역폭 특성을 반영한다.
 * 설정자: ioc_autop_idx_to_params()에서 i_lcoefs→lcoefs 변환 후 저장.
 * 읽는 자: ioc_cost_model()에서 bio당 abs_cost 계산 시 lcoefs 참조.
 */
struct ioc_params {
	u32				qos[NR_QOS_PARAMS];
	/* [한국어] QoS 파라미터 배열(QOS_RPPM/RLAT/WPPM/WLAT/MIN/MAX).
	 * 설정자: io.cost.qos sysfs 쓰기 → ioc_qos_write() → 이 배열 갱신.
	 * 읽는 자: ioc_adjust_base_vrate()가 latency 달성률 계산 시 참조.
	 * 값 범위: ppm(0~1000000) 또는 us(0~UINT_MAX). 0=비활성.
	 * 동기화: ioc->lock을 잡고 읽고 쓴다. */
	u64				i_lcoefs[NR_I_LCOEFS];
	/* [한국어] 사용자가 지정한 원시 계수(BPS/IOPS 단위).
	 * 설정자: io.cost.model sysfs 쓰기 → ioc_cost_model_write().
	 * 읽는 자: ioc_autop_idx_to_params()에서 lcoefs[]로 변환.
	 * 값 범위: bytes/s 또는 IO/s; 0이면 해당 계수 미사용.
	 * 동기화: ioc->lock 보호. */
	u64				lcoefs[NR_LCOEFS];
	/* [한국어] 실제 비용 계산에 쓰이는 내부 계수(vtime/page 또는 vtime/IO).
	 * 설정자: ioc_autop_idx_to_params()가 i_lcoefs에서 변환해 저장.
	 * 읽는 자: ioc_cost_model()이 bio당 abs_cost = lcoef_page*pages + lcoef_io.
	 * 값 범위: VTIME_PER_SEC 기반 고정소수점 값.
	 * 동기화: ioc->lock 보호. */
	u32				too_fast_vrate_pct;
	/* [한국어] 이 백분율(%)을 초과하는 vrate에서 장치가 "너무 빠름"으로 판단.
	 * AUTOP_SSD_DFL=500(5x), AUTOP_SSD_FAST=0(미사용).
	 * 설정자: ioc_autop_idx_to_params(). 읽는 자: ioc_adjust_base_vrate().
	 * 동기화: ioc->lock 보호. */
	u32				too_slow_vrate_pct;
	/* [한국어] 이 백분율(%) 미만의 vrate에서 장치가 "너무 느림"으로 판단.
	 * AUTOP_SSD_FAST=10(0.1x), 나머지=0(미사용).
	 * 설정자: ioc_autop_idx_to_params(). 읽는 자: ioc_adjust_base_vrate().
	 * 동기화: ioc->lock 보호. */
};

/*
 * [한국어] ioc_margins: cgroup vtime이 device vtime보다 뒤처질 수 있는 여유분(vtime 단위).
 * 장치 포화 전에 선제적으로 throttle할 IO 크레딧 버퍼 역할을 한다.
 * 설정자: ioc_margins_calc()가 period_us와 vrate로부터 계산.
 * 읽는 자: iocg_activate(), current_hweight() 등 vbudget 검사 경로.
 */
struct ioc_margins {
	s64				min;
	/* [한국어] MARGIN_MIN_PCT(10%) 수준의 여유분: 이 이하로 내려가면
	 * 서플러스 가중치 조정을 즉시 취소해 vtime 고갈을 방지.
	 * 설정자: ioc_margins_calc(). 읽는 자: iocg_kick_delay().
	 * 값 범위: 양수(vtime 단위). 동기화: ioc->lock 보호. */
	s64				low;
	/* [한국어] MARGIN_LOW_PCT(20%) 수준의 여유분: 이 이하에서 inuse
	 * 하향 조정 검토 시작. min보다 크고 target보다 작음.
	 * 설정자: ioc_margins_calc(). 읽는 자: transfer_surpluses().
	 * 동기화: ioc->lock 보호. */
	s64				target;
	/* [한국어] MARGIN_TARGET_PCT(50%) 수준의 목표 버퍼: iocg->vtime이
	 * device vtime 대비 이 여유분을 유지하는 것을 목표로 함.
	 * 설정자: ioc_margins_calc(). 읽는 자: iocg_activate().
	 * 동기화: ioc->lock 보호. */
};

/*
 * [한국어] ioc_missed: 한 주기 내 latency QoS 달성/미달성 카운터.
 * NVMe CQ 완료 인터럽트에서 per-CPU로 갱신되며, 타이머 주기마다
 * 집계해 미달성 비율로 vrate 조정 여부를 결정한다.
 * 설정자: ioc_rqos_done_bio()가 CQ 완료 시 CPU-local로 증가.
 * 읽는 자: ioc_timer_fn()이 period마다 전 CPU를 집계.
 */
struct ioc_missed {
	local_t				nr_met;
	/* [한국어] latency 목표를 달성한 IO 수(per-CPU local_t).
	 * 설정자: ioc_rqos_done_bio()에서 local_inc(&missed->nr_met).
	 * 읽는 자: ioc_timer_fn()에서 ioc_missed_update()로 전 CPU 합산.
	 * 동기화: local_t는 단일 CPU 접근만 가정; 집계 시 READ_ONCE. */
	local_t				nr_missed;
	/* [한국어] latency 목표를 초과한 IO 수(per-CPU local_t).
	 * 설정자: ioc_rqos_done_bio()에서 local_inc(&missed->nr_missed).
	 * 읽는 자: ioc_timer_fn()에서 nr_missed/(nr_met+nr_missed)로 미달성률 계산.
	 * 동기화: nr_met와 동일. */
	u32				last_met;
	/* [한국어] 직전 집계 주기의 nr_met 스냅샷.
	 * 이번 주기 증분 = 현재 합산값 - last_met.
	 * 설정자/읽는 자: ioc_missed_update()에서 갱신 및 참조.
	 * 동기화: ioc->lock 보호(집계는 타이머 컨텍스트). */
	u32				last_missed;
	/* [한국어] 직전 집계 주기의 nr_missed 스냅샷.
	 * 이번 주기 증분 = 현재 합산값 - last_missed.
	 * 설정자/읽는 자: ioc_missed_update().
	 * 동기화: ioc->lock 보호. */
};

/*
 * [한국어] ioc_pcpu_stat: 장치 단위 per-CPU IO 통계.
 * NVMe CQ 완료 인터럽트가 발생하는 CPU에서 lock 없이 갱신되며,
 * ioc_timer_fn() 타이머가 주기마다 전 CPU를 순회해 집계한다.
 * 설정자: ioc_rqos_done_bio() (완료 인터럽트), ioc_rqos_throttle() (rq_wait).
 * 읽는 자: ioc_timer_fn()의 ioc_missed_update(), ioc_rq_wait_update().
 */
struct ioc_pcpu_stat {
	struct ioc_missed		missed[2];
	/* [한국어] 읽기(missed[0])·쓰기(missed[1]) 방향별 latency QoS 달성/미달 카운터.
	 * 설정자: ioc_rqos_done_bio()에서 bio 방향에 따라 missed[0] 또는 missed[1] 갱신.
	 * 읽는 자: ioc_timer_fn()이 양 방향 미달성률을 계산해 vrate 하강 여부 판단.
	 * 동기화: local_t이므로 CPU-local write; 집계는 타이머가 READ_ONCE로 읽음. */

	local64_t			rq_wait_ns;
	/* [한국어] request(blk-mq tag) 할당 대기에 소요된 누적 시간(ns).
	 * 설정자: ioc_rqos_throttle()에서 rq 할당 전후 ktime 차이를 local64_add().
	 * 읽는 자: ioc_timer_fn()에서 주기당 rq_wait_pct = delta_wait/period_ns×100.
	 * 동기화: local64_t는 CPU-local 원자적 64비트 연산. */
	u64				last_rq_wait_ns;
	/* [한국어] 직전 집계 주기의 rq_wait_ns 스냅샷(비원자 u64).
	 * 이번 주기 증분 = 현재 합산값 - last_rq_wait_ns.
	 * 설정자/읽는 자: ioc_rq_wait_update()에서 갱신 및 참조.
	 * 동기화: ioc->lock 보호(집계는 단일 타이머 컨텍스트). */
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

	struct ioc_params		params;
	/* [한국어] 현재 장치에 적용 중인 비용/QoS 파라미터 집합.
	 * 설정자: ioc_qos_write(), ioc_cost_model_write(), ioc_autop_idx_to_params().
	 * 읽는 자: ioc_cost_model()(lcoefs), ioc_adjust_base_vrate()(qos).
	 * 동기화: ioc->lock 보호. */
	struct ioc_margins		margins;
	/* [한국어] period_us와 vrate로부터 계산된 vtime 여유분(min/low/target).
	 * 설정자: ioc_margins_calc()가 타이머 주기마다 재계산.
	 * 읽는 자: iocg_activate(), iocg_kick_delay() 등 vbudget 검사 경로.
	 * 동기화: ioc->lock 보호. */
	u32				period_us;
	/* [한국어] 타이머 주기 길이(us). latency QoS 목표로부터 계산되며
	 * MIN_PERIOD(1ms)~MAX_PERIOD(1s) 범위로 clamp됨.
	 * 설정자: ioc_calc_period()가 QoS 파라미터 변경 시 갱신.
	 * 읽는 자: ioc_timer_fn(), ioc_margins_calc().
	 * 동기화: ioc->lock 보호. */
	u32				timer_slack_ns;
	/* [한국어] hrtimer 슬랙(ns). TIMER_SLACK_PCT(1%)×period_us를 ns로 변환한 값.
	 * hrtimer_start() 호출 시 slack으로 전달해 CPU wake-up 집중화(timer coalescing) 허용.
	 * 설정자: ioc_calc_period(). 읽는 자: ioc_arm_waitq_timer().
	 * 동기화: ioc->lock 보호. */
	u64				vrate_min;
	/* [한국어] vrate 허용 최솟값(vtime/ns 단위). QOS_MIN에서 변환.
	 * 이 값 이하로는 vrate를 내리지 않아 vtime 진행 완전 정지 방지.
	 * 설정자: ioc_qos_write(). 읽는 자: ioc_adjust_base_vrate().
	 * 동기화: ioc->lock 보호. */
	u64				vrate_max;
	/* [한국어] vrate 허용 최댓값(vtime/ns 단위). QOS_MAX에서 변환.
	 * 설정자: ioc_qos_write(). 읽는 자: ioc_adjust_base_vrate().
	 * 동기화: ioc->lock 보호. */

	spinlock_t			lock;
	/* [한국어] ioc 구조체 전체를 보호하는 스핀락.
	 * ioc_timer_fn() (softirq), ioc_rqos_throttle() (프로세스 컨텍스트) 등
	 * 다중 경로에서 동시 접근하므로 irqsave 변형 사용 필수.
	 * 주의: 이 락을 잡은 상태에서 슬립 금지. */
	struct timer_list		timer;
	/* [한국어] ioc 주기 타이머: ioc_timer_fn()을 softirq로 호출.
	 * vrate 조정, 가중치 재계산, waitq 깨우기를 period_us 주기로 수행.
	 * 설정자: ioc_start_period()가 add_timer()로 등록.
	 * 동기화: timer 자체는 lock-free지만 콜백 내에서 ioc->lock 획득. */
	struct list_head		active_iocgs;	/* active cgroups */
	/* [한국어] 현재 IO를 발행 중인 ioc_gq들의 연결 리스트.
	 * iocg->active_list가 여기에 연결됨.
	 * 설정자: iocg_activate()가 list_add(). iocg_idle()이 list_del().
	 * 읽는 자: ioc_timer_fn()이 리스트를 순회해 가중치/vtime 재계산.
	 * 동기화: ioc->lock 보호. */
	struct ioc_pcpu_stat __percpu	*pcpu_stat;
	/* [한국어] per-CPU 완료 지연·rq_wait 통계 배열.
	 * 각 CPU의 CQ 완료 인터럽트에서 lock 없이 업데이트되며,
	 * 타이머 주기마다 ioc_pcpu_stat_sum()으로 집계.
	 * 설정자: kmalloc_percpu()로 iocost_init() 시 할당.
	 * 동기화: 쓰기는 local_t/local64_t, 읽기는 집계 시 READ_ONCE. */

	enum ioc_running		running;
	/* [한국어] 컨트롤러 동작 상태(IOC_IDLE/IOC_RUNNING/IOC_STOP).
	 * 설정자: ioc_start_period()(→RUNNING), iocost_exit()(→STOP), 마지막 iocg 비활성화(→IDLE).
	 * 읽는 자: ioc_timer_fn()이 진입 시 상태 확인.
	 * 동기화: ioc->lock 보호. */
	atomic64_t			vtime_rate;
	/* [한국어] 현재 vrate를 vtime/ns 단위로 저장한 atomic 값.
	 * 설정자: ioc_adjust_base_vrate()가 타이머 주기마다 갱신.
	 * 읽는 자: ioc_vtime_rate() 인라인, iocg_activate() 등 빈번히 읽힘.
	 * 값 범위: VRATE_MIN~vrate_max. 동기화: atomic64 읽기/쓰기. */
	u64				vtime_base_rate;
	/* [한국어] ioc->lock을 잡은 상태에서 참조하는 vrate 캐시.
	 * vtime_rate atomic과 동일한 값이지만 락 보호 경로에서 성능을 위해 사용.
	 * 설정자: ioc_adjust_base_vrate(). 읽는 자: current_hweight() 등.
	 * 동기화: ioc->lock 보호. */
	s64				vtime_err;
	/* [한국어] 목표 vrate와 실제 관측된 장치 사용률 간의 누적 오차.
	 * 양수면 vtime이 너무 빠르게 진행 중(장치 과부하), 음수면 여유.
	 * 설정자: ioc_adjust_base_vrate(). 읽는 자: 동일 함수 내 PI 제어 루프.
	 * 동기화: ioc->lock 보호. */

	seqcount_spinlock_t		period_seqcount;
	/* [한국어] period_at/period_at_vtime의 일관된 읽기를 위한 seqcount.
	 * 타이머 시작 시 write_seqcount_begin(), 종료 시 end(). 독자는
	 * read_seqcount_begin()/retry()로 torn read 없이 두 값을 함께 읽음.
	 * 동기화: ioc->lock을 inner로 사용하는 seqcount_spinlock_t. */
	u64				period_at;	/* wallclock starttime */
	/* [한국어] 현재 주기의 시작 시각(ktime_get_ns(), ns 단위).
	 * 설정자: ioc_start_period(). 읽는 자: ioc_timer_fn()이 경과 시간 계산.
	 * 동기화: period_seqcount로 period_at_vtime과 함께 원자적으로 갱신. */
	u64				period_at_vtime; /* vtime starttime */
	/* [한국어] 현재 주기 시작 시점의 device vtime 값.
	 * 설정자: ioc_start_period(). 읽는 자: ioc_at_period_vtime() 등.
	 * 동기화: period_seqcount로 period_at과 함께 원자적으로 갱신. */

	atomic64_t			cur_period;	/* inc'd each period */
	/* [한국어] 현재 타이머 주기 번호(단조 증가). 각 iocg의 active_period와
	 * 비교해 stale vtime을 감지한다. 새 주기마다 atomic64_inc().
	 * 설정자: ioc_start_period(). 읽는 자: iocg_activate(), current_hweight().
	 * 동기화: atomic64 연산. */
	int				busy_level;	/* saturation history */
	/* [한국어] 장치 포화도 누적 지표: 포화 시 증가, 여유 시 감소.
	 * vrate_adj_pct[] 테이블의 인덱스로 사용해 vrate 조정폭을 결정.
	 * 설정자: ioc_adjust_base_vrate(). 읽는 자: 동일 함수.
	 * 동기화: ioc->lock 보호. */

	bool				weights_updated;
	/* [한국어] 이번 주기에 가중치가 갱신됐는지를 나타내는 플래그.
	 * 설정자: propagate_weights()가 가중치를 재계산할 때 true로 설정.
	 * 읽는 자: ioc_timer_fn()이 불필요한 재계산을 건너뛸 때 확인.
	 * 동기화: ioc->lock 보호. */
	atomic_t			hweight_gen;	/* for lazy hweights */
	/* [한국어] hweight(계층적 가중치) 세대 번호. 증가 시 모든 iocg의
	 * hweight 캐시를 stale로 만들어 다음 접근 시 재계산 강제.
	 * 설정자: __propagate_weights(). 읽는 자: current_hweight().
	 * 동기화: atomic 연산. */

	/* debt forgivness */
	u64				dfgv_period_at;
	/* [한국어] 마지막 부채 탕감 평가 시작 시각(ktime_get_ns()).
	 * DFGV_PERIOD(100ms) 단위로 부채 탕감 여부를 평가하기 위한 기준점.
	 * 설정자: ioc_forgive_debts(). 동기화: ioc->lock 보호. */
	u64				dfgv_period_rem;
	/* [한국어] 부채 탕감 평가 주기의 잔여 시간(ns).
	 * period 경계를 넘칠 때 나머지를 누적해 정확한 100ms 창을 유지.
	 * 설정자: ioc_forgive_debts(). 동기화: ioc->lock 보호. */
	u64				dfgv_usage_us_sum;
	/* [한국어] 부채 탕감 평가 창 내 누적 IO 사용량(us).
	 * DFGV_USAGE_PCT(50%) 미만이면 active cgroup들의 부채를 절반으로 탕감.
	 * 설정자: ioc_forgive_debts(). 동기화: ioc->lock 보호. */

	u64				autop_too_fast_at;
	/* [한국어] 장치가 "너무 빠름" 조건을 처음 만족한 시각(ktime_get_ns()).
	 * AUTOP_CYCLE_NSEC(10s) 동안 지속되면 더 느린 프로파일로 전환.
	 * 설정자: ioc_adjust_base_vrate(). 동기화: ioc->lock 보호. */
	u64				autop_too_slow_at;
	/* [한국어] 장치가 "너무 느림" 조건을 처음 만족한 시각(ktime_get_ns()).
	 * AUTOP_CYCLE_NSEC(10s) 동안 지속되면 더 빠른 프로파일로 전환.
	 * 설정자: ioc_adjust_base_vrate(). 동기화: ioc->lock 보호. */
	int				autop_idx;
	/* [한국어] 현재 적용 중인 자동 프로파일 인덱스(AUTOP_HDD/SSD_QD1/SSD_DFL/SSD_FAST).
	 * 설정자: ioc_autop_idx()가 vrate 관측값으로 결정. 읽는 자: ioc_autop_idx_to_params().
	 * 동기화: ioc->lock 보호. */
	bool				user_qos_params:1;
	/* [한국어] 사용자가 직접 QoS 파라미터를 지정했음을 나타내는 플래그.
	 * true이면 자동 프로파일 전환(ioc_autop_idx_to_params)이 QoS 파라미터를 덮어쓰지 않음.
	 * 설정자: ioc_qos_write()가 절대값 모드로 설정 시 true.
	 * 동기화: ioc->lock 보호. */
	bool				user_cost_model:1;
	/* [한국어] 사용자가 직접 비용 모델 계수를 지정했음을 나타내는 플래그.
	 * true이면 자동 프로파일이 lcoefs를 덮어쓰지 않음.
	 * 설정자: ioc_cost_model_write()가 linear 모드로 설정 시 true.
	 * 동기화: ioc->lock 보호. */
};

/*
 * [한국어] iocg_pcpu_stat: cgroup별 per-CPU 절대 비용 사용량.
 * bio 발행 시점에 각 CPU에서 lock 없이 abs_cost를 누적하며,
 * 타이머 주기마다 전 CPU를 합산해 이번 주기 사용량을 계산한다.
 * 설정자: iocg_pay_debt()·ioc_rqos_throttle()에서 local64_add().
 * 읽는 자: ioc_timer_fn()의 iocg_stat_update()에서 합산.
 */
struct iocg_pcpu_stat {
	local64_t			abs_vusage;
	/* [한국어] 이 CPU에서 이 cgroup이 소비한 절대 vtime 비용 누계(MILLION 단위).
	 * abs_cost = cost × WEIGHT_ONE / hweight_inuse 공식의 결과를 여기에 누적.
	 * 설정자: iocg_commit_bio()에서 local64_add(&pcpu_stat->abs_vusage, abs_cost).
	 * 읽는 자: iocg_abs_vusage()가 전 CPU local64_read()를 합산.
	 * 동기화: local64_t이므로 단일 CPU 쓰기만 허용; 읽기는 READ_ONCE 기반. */
};

/*
 * [한국어] iocg_stat: cgroup별 누적 IO 시간 통계(us 단위).
 * 타이머 주기마다 iocg_stat_update()가 갱신하며,
 * io.stat cgroup 인터페이스와 iocost_monitor.py에 노출된다.
 * 설정자: iocg_stat_update()가 period마다 증분을 더함.
 * 읽는 자: iocg_stat_show(), iocost_monitor.py.
 * 동기화: ioc->lock 보호.
 */
struct iocg_stat {
	u64				usage_us;
	/* [한국어] 이 cgroup이 실제로 소비한 누적 IO 시간(us).
	 * abs_vusage를 us 단위로 환산해 누적. 설정자: iocg_stat_update().
	 * iocost_monitor.py의 "usages%" 필드에 표시. */
	u64				wait_us;
	/* [한국어] vtime 예산 부족으로 waitq에서 대기한 누적 시간(us).
	 * wait_since 타임스탬프로부터 계산. 설정자: iocg_stat_update().
	 * iocost_monitor.py의 "wait" 정보에 반영. */
	u64				indebt_us;
	/* [한국어] abs_vdebt > 0인(부채 상태) 상태로 있었던 누적 시간(us).
	 * indebt_since 타임스탬프로부터 계산. 설정자: iocg_stat_update(). */
	u64				indelay_us;
	/* [한국어] delay > 0인(지연 인가) 상태로 있었던 누적 시간(us).
	 * indelay_since 타임스탬프로부터 계산. 설정자: iocg_stat_update(). */
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
/* [한국어] ioc_gq: 장치-cgroup 쌍별 IO 비용 상태. blkg_policy_data를 상속해
 * blkcg 프레임워크가 cgroup별로 자동 생성/소멸한다. */
struct ioc_gq {
	struct blkg_policy_data		pd;
	/* [한국어] blkcg 정책 데이터 기반 구조체. blkg_to_pd(blkg, &blkcg_policy_iocost)로
	 * blkg에서 ioc_gq를 찾을 때 사용. container_of로 ioc_gq 복원.
	 * 설정자: blkcg 프레임워크가 pd_alloc_fn/pd_init_fn 콜백으로 초기화.
	 * 동기화: blkcg 락(blkcg_pol_mutex) + ioc->lock 조합. */
	struct ioc			*ioc;
	/* [한국어] 이 iocg가 속한 장치의 ioc 컨트롤러 역참조 포인터.
	 * pd_alloc_fn에서 설정, pd_free_fn까지 유효. NULL 불가.
	 * 동기화: 초기화 후 불변(read-only). */

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
	/* [한국어] 이 장치-cgroup 쌍에 대해 io.weight로 직접 설정된 가중치.
	 * 0이면 cgroup 기본값(ioc_cgrp->dfl_weight) 사용.
	 * 설정자: ioc_set_weight(). 읽는 자: iocg_activate()에서 weight 결정.
	 * 동기화: ioc->lock 보호. */
	u32				weight;
	/* [한국어] cfg_weight와 dfl_weight 중 유효한 값으로 결정된 실효 가중치.
	 * 설정자: iocg_weight_updated(). 읽는 자: __propagate_weights().
	 * 동기화: ioc->lock 보호. */
	u32				active;
	/* [한국어] 이 iocg가 활성 상태일 때의 가중치(= weight). 비활성 시 0.
	 * 설정자: iocg_activate()(weight로 설정), iocg_idle()(0으로 설정).
	 * hweight_active 계산에 사용. 동기화: ioc->lock 보호. */
	u32				inuse;
	/* [한국어] 서플러스 조정이 적용된 실제 사용 가중치. active 이하.
	 * inuse가 낮을수록 동일 bio의 vtime 비용이 더 비싸게 계산됨
	 * (cost = abs_cost × WEIGHT_ONE / hweight_inuse).
	 * 설정자: __propagate_weights(), transfer_surpluses(). 동기화: ioc->lock 보호. */

	u32				last_inuse;
	/* [한국어] 이 iocg가 비활성화될 때 저장한 inuse 값.
	 * 재활성화 시 복원해 서플러스 조정 상태를 유지.
	 * 설정자: iocg_idle()에서 last_inuse = inuse로 저장.
	 * 읽는 자: iocg_activate()에서 inuse = last_inuse로 복원.
	 * 동기화: ioc->lock 보호. */
	s64				saved_margin;
	/* [한국어] inuse 조정 전의 vtime 여유분 스냅샷.
	 * 조정이 필요한지 판단하고 과도한 반복 조정을 방지하는 데 사용.
	 * 설정자: ioc_timer_fn()에서 조정 직전 저장.
	 * 동기화: ioc->lock 보호. */

	sector_t			cursor;		/* to detect randio */
	/* [한국어] 직전 bio의 마지막 섹터 주소. 다음 bio의 시작 섹터와 비교해
	 * LCOEF_RANDIO_PAGES(16MB) 이상 떨어지면 random IO로 분류.
	 * 설정자: iocg_commit_bio()에서 bio 처리 후 갱신.
	 * 읽는 자: ioc_cost_model()에서 순차/랜덤 분류.
	 * 동기화: 단일 iocg는 단일 blkcg(단일 잡 스레드)에서만 접근 — 별도 락 불필요. */

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
	/* [한국어] 이 cgroup의 IO 발행 기준 가상 시간 커서(atomic64).
	 * 값 범위: device vtime - margin.target ~ device vtime + overage.
	 * 설정자: iocg_commit_bio()가 cost 단위로 원자적 증가.
	 * 읽는 자: iocg_is_behind(), ioc_rqos_throttle() 등 빈번히 읽힘.
	 * 동기화: atomic64 연산 (락 없이 빠른 경로에서 읽기 가능). */
	atomic64_t			done_vtime;
	/* [한국어] CQ 완료 기준 가상 시간 커서. vtime과 동일 구조이나
	 * bio 제출이 아닌 blk_mq 완료 콜백(ioc_rqos_done_bio)에서 증가.
	 * vtime - done_vtime = 현재 in-flight IO들의 누적 vtime 비용.
	 * 설정자: ioc_rqos_done_bio(). 동기화: atomic64 연산. */
	u64				abs_vdebt;
	/* [한국어] 이 cgroup의 절대 vtime 부채(MILLION 단위).
	 * 예산 없이 IO를 발행할 때 누적되는 미지급 비용.
	 * 설정자: iocg_pay_debt()가 부채 증가/감소. 읽는 자: ioc_rqos_throttle().
	 * 동기화: ioc->lock 보호. */

	/* current delay in effect and when it started */
	u64				delay;
	/* [한국어] 현재 인가 중인 IO 발행 지연(us). 0이면 지연 없음.
	 * 이 값이 양수이면 blkcg use_delay 메커니즘으로 bio 제출을 늦춤.
	 * 설정자: iocg_set_delay(). 읽는 자: iocg_check_delay().
	 * 동기화: ioc->lock 보호. */
	u64				delay_at;
	/* [한국어] 현재 지연이 설정된 시각(ktime_get_ns()).
	 * delay 값이 0으로 초기화됐을 때 언제 만료할지 계산에 사용.
	 * 설정자: iocg_set_delay(). 읽는 자: iocg_check_delay().
	 * 동기화: ioc->lock 보호. */

	/*
	 * The period this iocg was last active in.  Used for deactivation
	 * and invalidating `vtime`.
	 */
	atomic64_t			active_period;
	/* [한국어] 이 iocg가 마지막으로 활성이었던 주기 번호(ioc->cur_period 기준).
	 * ioc->cur_period와 비교해 오래된 iocg를 비활성화하거나 vtime을 재설정.
	 * 설정자: iocg_activate()에서 cur_period 값으로 설정.
	 * 읽는 자: ioc_timer_fn()의 비활성 판정 로직.
	 * 동기화: atomic64 연산. */
	struct list_head		active_list;	/* ioc->active_iocgs: NVMe 제출 중인 cgroup 연결 */
	/* [한국어] ioc->active_iocgs 리스트의 연결 노드.
	 * 설정자: iocg_activate()가 list_add(). iocg_idle()이 list_del().
	 * 읽는 자: ioc_timer_fn()이 active_iocgs를 순회할 때.
	 * 동기화: ioc->lock 보호. */

	/* see __propagate_weights() and current_hweight() for details */
	u64				child_active_sum;
	/* [한국어] 자식 iocg들의 active 가중치 합계. hweight_active 계산에 사용.
	 * 설정자: __propagate_weights(). 읽는 자: current_hweight().
	 * 동기화: ioc->lock 보호. */
	u64				child_inuse_sum;
	/* [한국어] 자식 iocg들의 inuse 가중치 합계. hweight_inuse 계산에 사용.
	 * 설정자: __propagate_weights(). 읽는 자: current_hweight().
	 * 동기화: ioc->lock 보호. */
	u64				child_adjusted_sum;
	/* [한국어] 서플러스 조정 후 자식들의 inuse 합계. transfer_surpluses()에서
	 * 잉여 가중치 계산의 분모로 사용.
	 * 설정자: transfer_surpluses(). 동기화: ioc->lock 보호. */
	int				hweight_gen;
	/* [한국어] 이 iocg의 hweight 캐시 세대 번호. ioc->hweight_gen과
	 * 다르면 hweight_active/inuse 캐시가 stale → current_hweight()가 재계산.
	 * 설정자: __propagate_weights()가 ioc->hweight_gen으로 갱신.
	 * 동기화: ioc->lock 보호. */
	u32				hweight_active;
	/* [한국어] active 가중치 기준 계층적 가중치(0~WEIGHT_ONE).
	 * = (부모의 hweight_active) × (active / child_active_sum).
	 * 설정자: __propagate_weights(). 읽는 자: iocg_activate() 등.
	 * 동기화: ioc->lock + hweight_gen 캐시 무효화. */
	u32				hweight_inuse;
	/* [한국어] inuse 가중치 기준 계층적 가중치(0~WEIGHT_ONE).
	 * bio의 실제 vtime 비용 = abs_cost × WEIGHT_ONE / hweight_inuse.
	 * 값이 작을수록 이 cgroup에 IO 비용이 비싸게 부과됨.
	 * 설정자: __propagate_weights(). 읽는 자: iocg_commit_bio().
	 * 동기화: ioc->lock + hweight_gen 캐시 무효화. */
	u32				hweight_donating;
	/* [한국어] 서플러스 가중치를 기부하기 전의 hweight_inuse.
	 * transfer_surpluses()가 기부 전·후 hweight를 비교하는 데 사용.
	 * 설정자: transfer_surpluses(). 동기화: ioc->lock 보호. */
	u32				hweight_after_donation;
	/* [한국어] 서플러스 가중치를 기부한 후 예상 hweight_inuse.
	 * 기부량을 결정할 때 기부 후 hweight가 min 이하로 내려가지 않는지 검증.
	 * 설정자: transfer_surpluses(). 동기화: ioc->lock 보호. */

	struct list_head		walk_list;
	/* [한국어] propagate_weights()가 cgroup 트리를 DFS 순회할 때 사용하는 임시 연결 노드.
	 * ioc->lock 보호 하에 순회 중에만 유효. */
	struct list_head		surplus_list;
	/* [한국어] transfer_surpluses()가 잉여 가중치를 가진 iocg들을 모으는 임시 리스트.
	 * 타이머 주기 내에서만 사용되며 ioc->lock 보호. */

	struct wait_queue_head		waitq;
	/* [한국어] vtime 예산 부족으로 대기 중인 bio 발급자들의 대기열.
	 * ioc_rqos_throttle()이 wait_event()로 진입, iocg_kick_waitq()가 wake_up()으로 해제.
	 * 동기화: waitq 자체의 내부 스핀락(wait_queue_head.lock). */
	struct hrtimer			waitq_timer;
	/* [한국어] waitq 대기 bio들을 예산 회복 시점에 깨우는 hrtimer.
	 * ioc_arm_waitq_timer()가 다음 vtime 충전 예상 시각에 설정.
	 * 콜백: iocg_waitq_timer_fn() → iocg_kick_waitq() 호출.
	 * 동기화: hrtimer 자체 락; 콜백 내에서 ioc->lock 획득. */

	/* timestamp at the latest activation */
	u64				activated_at;
	/* [한국어] 이 iocg가 가장 최근에 활성화된 시각(ktime_get_ns()).
	 * 설정자: iocg_activate(). 읽는 자: iocost_monitor.py 모니터링.
	 * 동기화: ioc->lock 보호. */

	/* statistics */
	struct iocg_pcpu_stat __percpu	*pcpu_stat;
	/* [한국어] per-CPU abs_vusage 누적 카운터 배열.
	 * 설정자: iocg_commit_bio()에서 local64_add().
	 * 읽는 자: iocg_stat_update()에서 iocg_abs_vusage()로 합산.
	 * 동기화: local64_t 기반 CPU-local 쓰기; 읽기는 타이머 경로. */
	struct iocg_stat		stat;
	/* [한국어] 현재까지의 누적 IO 시간 통계(usage_us/wait_us/indebt_us/indelay_us).
	 * 설정자: iocg_stat_update(). 읽는 자: io.stat sysfs, iocost_monitor.py.
	 * 동기화: ioc->lock 보호. */
	struct iocg_stat		last_stat;
	/* [한국어] 직전 period의 stat 스냅샷. 이번 period 증분 = stat - last_stat.
	 * 설정자: iocg_stat_update(). 동기화: ioc->lock 보호. */
	u64				last_stat_abs_vusage;
	/* [한국어] 직전 period의 abs_vusage 합산값. 증분 계산의 기준점.
	 * 설정자: iocg_stat_update(). 동기화: ioc->lock 보호. */
	u64				usage_delta_us;
	/* [한국어] 이번 period의 IO 사용량 증분(us). debt 탕감 판단에 사용.
	 * 설정자: iocg_stat_update(). 읽는 자: ioc_forgive_debts().
	 * 동기화: ioc->lock 보호. */
	u64				wait_since;
	/* [한국어] waitq에서 대기가 시작된 시각(ktime_get_ns(), ns 단위).
	 * 0이면 현재 대기 중 아님. 설정자: ioc_rqos_throttle().
	 * 읽는 자: iocg_stat_update()가 wait_us 계산.
	 * 동기화: ioc->lock 보호. */
	u64				indebt_since;
	/* [한국어] abs_vdebt > 0 상태(부채 존재)가 시작된 시각(ktime_get_ns()).
	 * 설정자: iocg_pay_debt()가 부채 발생 시 기록.
	 * 읽는 자: iocg_stat_update()가 indebt_us 계산.
	 * 동기화: ioc->lock 보호. */
	u64				indelay_since;
	/* [한국어] delay > 0 상태(지연 인가)가 시작된 시각(ktime_get_ns()).
	 * 설정자: iocg_set_delay()가 지연 설정 시 기록.
	 * 읽는 자: iocg_stat_update()가 indelay_us 계산.
	 * 동기화: ioc->lock 보호. */

	/* this iocg's depth in the hierarchy and ancestors including self */
	int				level;
	/* [한국어] 이 cgroup의 cgroup 트리 깊이(root=0). ancestors[] 배열 크기 결정.
	 * 설정자: pd_alloc_fn(). 읽는 자: __propagate_weights() DFS 순회.
	 * 동기화: 초기화 후 불변. */
	struct ioc_gq			*ancestors[];
	/* [한국어] root부터 이 iocg까지의 조상 포인터 배열(자기 자신 포함, level+1 개).
	 * propagate_weights()가 트리를 루트에서 리프 방향으로 순회할 때 사용.
	 * 설정자: pd_alloc_fn()에서 cgroup 트리를 거슬러 올라가며 채움.
	 * 동기화: 초기화 후 불변. */
};

/*
 * [한국어] ioc_cgrp: cgroup별 기본 가중치 저장소.
 * ioc_gq에 장치별 명시 가중치(cfg_weight)가 없을 때 폴백으로 사용되며,
 * blkcg_policy_data를 상속해 cgroup당 하나가 자동 관리된다.
 * 설정자: ioc_set_weight()가 io.weight 쓰기 시 갱신.
 * 읽는 자: iocg_activate()가 cfg_weight==0일 때 이 값 사용.
 */
/* per cgroup */
struct ioc_cgrp {
	struct blkcg_policy_data	cpd;
	/* [한국어] blkcg 정책 데이터 기반 구조체. cpd_to_blkcg(cpd)로 blkcg 역참조.
	 * 설정자: blkcg 프레임워크의 cpd_alloc_fn 콜백. 동기화: blkcg 락. */
	unsigned int			dfl_weight;
	/* [한국어] 이 cgroup의 기본 IO 가중치(WEIGHT_ONE 단위로 정규화됨).
	 * 설정자: ioc_set_weight()가 echo N > io.weight 시 갱신.
	 * 읽는 자: iocg_activate()가 cfg_weight==0인 iocg에 적용.
	 * 값 범위: CGROUP_WEIGHT_MIN(1)~CGROUP_WEIGHT_MAX(10000).
	 * 동기화: blkcg 락 보호. */
};

/*
 * [한국어] ioc_now: 한 시점의 wallclock/가상 시간 스냅샷.
 * ioc_timer_fn() 진입 시 한 번 채워지고 이후 경로들이 동일 시각을 참조하므로
 * ktime_get_ns()를 여러 번 호출할 때의 시계 진행 오차를 제거한다.
 * 설정자: ioc_now_fn()이 seqcount로 period_at/period_at_vtime을 읽어 vnow 계산.
 * 읽는 자: ioc_timer_fn() 내 대부분의 계산 경로.
 */
struct ioc_now {
	u64				now_ns;
	/* [한국어] 스냅샷 시각(ktime_get_ns() 반환값, ns 단위).
	 * ioc_now_fn() 호출 시점의 단조 시계값. */
	u64				now;
	/* [한국어] 스냅샷 시각을 us 단위로 변환한 값(= now_ns/NSEC_PER_USEC).
	 * period 경과 계산, delay 계산 등 us 단위가 필요한 경로에서 사용. */
	u64				vnow;
	/* [한국어] 스냅샷 시각에 대응하는 device virtual time 값.
	 * = period_at_vtime + (now_ns - period_at) × vrate.
	 * cgroup의 vtime과 비교해 예산(budget)이나 초과량(overage)을 계산. */
};

/*
 * [한국어] iocg_wait: vtime 예산 부족으로 waitq에 대기 중인 bio 1개를 표현하는 구조체.
 * ioc_rqos_throttle()이 스택에 할당해 waitq에 추가하고,
 * iocg_wake_fn()이 깨울 때 abs_cost를 현재 hweight_inuse로 환산해 vtime을 차감한다.
 * 생명주기: ioc_rqos_throttle() 스택 → waitq → iocg_wake_fn() 종료 후 소멸.
 */
struct iocg_wait {
	struct wait_queue_entry		wait;
	/* [한국어] waitq 연결 노드(wait_queue_entry). init_waitqueue_entry()로 초기화.
	 * 설정자: ioc_rqos_throttle(). 읽는 자: waitq 내부 인프라.
	 * 동기화: waitq 내부 스핀락. */
	struct bio			*bio;
	/* [한국어] 대기 중인 bio 포인터. 깨어난 후 실제 IO 제출에 사용.
	 * 설정자: ioc_rqos_throttle()에서 현재 bio로 설정.
	 * 읽는 자: iocg_wake_fn()이 bio 재제출 경로에서 참조.
	 * 값 범위: 유효한 bio 포인터 (NULL 불가). */
	u64				abs_cost;
	/* [한국어] 이 bio의 절대 IO 비용(MILLION 단위, hweight 미적용).
	 * 깨어날 때 hweight_inuse로 나눠 vtime 차감량을 결정.
	 * 설정자: ioc_rqos_throttle()이 ioc_cost_model()로 계산.
	 * 읽는 자: iocg_wake_fn(). */
	bool				committed;
	/* [한국어] 이 wait 항목이 vtime을 이미 차감(commit)했는지를 나타내는 플래그.
	 * true이면 iocg_wake_fn()이 다시 차감하지 않음 — 이중 차감 방지.
	 * 설정자: iocg_wake_fn()이 first wake 시 true로 설정.
	 * 동기화: waitq 내부 스핀락. */
};

/*
 * [한국어] iocg_wake_ctx: iocg_kick_waitq()가 waitq를 순회할 때 전달하는 컨텍스트.
 * 현재 vbudget 잔량과 hweight_inuse를 wake 함수(iocg_wake_fn())에 전달해
 * 깨울 수 있는 bio와 깨울 수 없는 bio를 구분한다.
 * 생명주기: iocg_kick_waitq() 스택에서 선언, wake_up_all_locked() 호출 후 소멸.
 */
struct iocg_wake_ctx {
	struct ioc_gq			*iocg;
	/* [한국어] 대상 iocg 역참조. iocg_wake_fn()이 vtime 차감 대상을 알기 위해 사용.
	 * 설정자: iocg_kick_waitq(). 동기화: waitq 내부 스핀락으로 간접 보호. */
	u32				hw_inuse;
	/* [한국어] 현재 hweight_inuse 스냅샷. cost = abs_cost × WEIGHT_ONE / hw_inuse 계산용.
	 * 설정자: iocg_kick_waitq()가 current_hweight()로 획득.
	 * 읽는 자: iocg_wake_fn(). */
	s64				vbudget;
	/* [한국어] 현재 사용 가능한 vtime 예산(signed). 양수이면 IO 발행 가능.
	 * iocg_wake_fn()이 bio를 깨울 때마다 해당 cost를 차감하고,
	 * 0 이하가 되면 나머지 waitq 항목들은 깨우지 않음.
	 * 설정자: iocg_kick_waitq(). 읽는 자: iocg_wake_fn().
	 * 동기화: waitq 내부 스핀락. */
};

/*
 * [한국어] autop[]: 장치 유형별 자동 프로파일 파라미터 테이블.
 * ioc_autop_idx()가 장치 성능 관측값(vrate 추이)으로 프로파일을 선택하면
 * ioc_autop_idx_to_params()가 이 테이블에서 QoS·lcoef 파라미터를 복사한다.
 * 사용자가 io.cost.qos/model로 직접 설정하지 않은 한 이 값이 적용된다.
 * 읽는 자: ioc_autop_idx_to_params().
 * 동기화: 읽기 전용 const 테이블 — 초기화 후 불변.
 */
static const struct ioc_params autop[] = {
	[AUTOP_HDD] = {		/* [한국어] 회전 디스크 프로파일: seek IOPS=370으로 낮고 latency 목표 250ms */
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
	[AUTOP_SSD_QD1] = {	/* [한국어] SSD 큐깊이-1 프로파일: rand IOPS=6946, latency 목표 25ms */
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
	[AUTOP_SSD_DFL] = {	/* [한국어] SSD 기본 프로파일: rand IOPS=8518, too_fast_vrate=500%, latency 목표 25ms */
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
	[AUTOP_SSD_FAST] = {	/* [한국어] 고속 SSD/NVMe 프로파일: rand IOPS=778122, too_slow_vrate=10%, latency 목표 5ms */
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
/* [한국어] vrate_adj_pct[]: busy_level 인덱스에 대응하는 vrate 조정 퍼센트 테이블.
 * busy_level이 높을수록(포화 심할수록) 더 큰 폭으로 vrate를 낮추고,
 * 낮을수록(여유 있을수록) 더 큰 폭으로 vrate를 높인다.
 * 앞 4개(0%)는 히스테리시스 구간, 이후 지수적으로 조정폭 증가.
 * 읽는 자: ioc_adjust_base_vrate()가 busy_level을 인덱스로 참조.
 * 동기화: 읽기 전용 const 테이블. */
static const u32 vrate_adj_pct[] =
	{ 0, 0, 0, 0,
	  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	  4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 16 };

/* [한국어] blkcg_policy_iocost: iocost의 blkcg 정책 디스크립터.
 * blkcg_activate_policy()에 등록해 cgroup 트리의 각 blkg에
 * ioc_gq(pd)와 cgroup 레벨의 ioc_cgrp(cpd)를 자동 생성/소멸하게 한다.
 * 등록자: ioc_init()의 blkcg_policy_register().
 * 읽는 자: blkg_to_pd(), blkg_to_cpd() 등 cgroup 정책 조회 경로.
 * 동기화: blkcg_pol_mutex로 등록/해제 직렬화. */
static struct blkcg_policy blkcg_policy_iocost;

/* accessors and helpers */
/*
 * [한국어]
 * rqos_to_ioc - rq_qos 포인터로부터 ioc 컨트롤러 구조체를 복원하는 접근자
 *
 * @rqos: blk-rq-qos 프레임워크가 관리하는 rq_qos 포인터.
 *        rq_qos_id(q, RQ_QOS_COST)가 반환하는 값이 전달된다.
 * @return: @rqos를 내부 필드로 포함하는 struct ioc 포인터.
 *          NULL이 전달될 경우 동작이 정의되지 않으므로 호출자가 보장해야 한다.
 *
 * blk-iocost는 struct ioc 안에 struct rq_qos rqos 필드를 포함한다.
 * blk-rq-qos 레이어는 rq_qos* 만 알고 있으므로, 콜백에서 실제 ioc로
 * 복원할 때 container_of 패턴을 사용한다. 이 함수는 그 변환을 한 곳에서
 * 캡슐화하여 타입 안전성을 보장한다.
 *
 * 실행 컨텍스트: 슬리핑 불가 (rq_qos 콜백은 IRQ-safe 경로에서 호출됨).
 * 호출자: q_to_ioc(), iocg_activate(), iocost_rqos_ops 콜백 전체.
 * 호출 대상: container_of 매크로 (컴파일 타임 오프셋 계산, 런타임 비용 없음).
 *
 * 호출 체인:
 *   rq_qos_ops 콜백 (iocost_throttle/done/exit 등) → [rqos_to_ioc] → struct ioc 접근
 */
static struct ioc *rqos_to_ioc(struct rq_qos *rqos)
{
	return container_of(rqos, struct ioc, rqos); /* [한국어] rq_qos 포인터로부터 포함 구조체 ioc를 역산 — container_of는 컴파일타임 오프셋을 이용한 무비용 캐스트 */
}

/*
 * [한국어]
 * q_to_ioc - request_queue로부터 ioc 컨트롤러를 가져오는 편의 접근자
 *
 * @q: 블록 IO를 처리하는 request_queue. 큐에 iocost 플러그인이 등록되어
 *     있어야 하며, 그렇지 않으면 rq_qos_id()가 NULL을 반환하여 oops 발생.
 * @return: 이 큐에 연결된 struct ioc 포인터. 큐에 iocost가 없으면 NULL.
 *
 * iocost 콜백은 request_queue 포인터만 주어지는 경우가 많다. 이 함수는
 * rq_qos_id()로 RQ_QOS_COST 슬롯의 rq_qos를 찾은 뒤 rqos_to_ioc()로
 * 변환하는 두 단계를 하나로 묶어 코드 중복을 방지한다.
 *
 * 실행 컨텍스트: rqos_to_ioc()와 동일, IRQ-safe 경로에서 호출 가능.
 * 호출자: iocost_exit(), iocost_iolatency_update(), ioc_looking_at_rt_budget().
 * 호출 대상: rq_qos_id() → rqos_to_ioc().
 *
 * 호출 체인:
 *   iocost 외부 진입점 (큐 종료/업데이트 등) → [q_to_ioc] → rqos_to_ioc → struct ioc
 */
static struct ioc *q_to_ioc(struct request_queue *q)
{
	return rqos_to_ioc(rq_qos_id(q, RQ_QOS_COST)); /* [한국어] 큐에서 RQ_QOS_COST 슬롯의 rq_qos를 찾아 ioc로 변환 — 슬롯 번호는 iocost 등록 시 결정됨 */
}

/*
 * [한국어]
 * ioc_name - 디버그/트레이스용 디스크 이름 문자열 반환
 *
 * @ioc: 이름을 조회할 ioc 컨트롤러.
 * @return: 디스크 이름 문자열 ("sda", "nvme0n1" 등).
 *          disk가 아직 초기화되지 않았으면 "<unknown>" 고정 문자열 반환.
 *
 * iocost는 커널 트레이스포인트(trace/events/iocost.h)와 pr_debug()에서
 * 장치를 식별할 때 이 함수를 쓴다. __maybe_unused 속성은 CONFIG_TRACING이
 * 꺼진 빌드에서 사용되지 않아도 컴파일러 경고가 나지 않도록 한다.
 *
 * 실행 컨텍스트: 트레이스포인트/디버그 출력 경로; 락 없이 disk 포인터만 읽음.
 * 호출자: 트레이스 매크로, iocost 내부 pr_debug 경로.
 * 호출 대상: 직접 포인터 역참조만 수행 (syscall/락 없음).
 *
 * 호출 체인:
 *   trace_iocost_*() 매크로 → [ioc_name] → disk->disk_name
 */
static const char __maybe_unused *ioc_name(struct ioc *ioc)
{
	struct gendisk *disk = ioc->rqos.disk; /* [한국어] ioc가 제어하는 블록 디바이스의 gendisk 포인터 — init 경로에서는 아직 NULL일 수 있음 */

	if (!disk) /* [한국어] 초기화 전(init 경로) 또는 디스크가 없는 경우 — 안전한 fallback 문자열 반환 */
		return "<unknown>";
	return disk->disk_name; /* [한국어] "sda", "nvme0n1" 같은 커널 블록 디바이스 이름 반환 */
}

/*
 * [한국어]
 * pd_to_iocg - blkg_policy_data 포인터로부터 ioc_gq 구조체를 복원하는 접근자
 *
 * @pd: blkcg_gq에 연결된 정책별 데이터 포인터. blkg_to_pd()가 반환하는 값.
 *      NULL이면 NULL을 반환하여 호출자가 직접 검사할 수 있도록 한다.
 * @return: @pd를 내부 필드로 포함하는 struct ioc_gq 포인터. @pd가 NULL이면 NULL.
 *
 * blkcg 프레임워크는 정책별 데이터를 blkg_policy_data* 타입으로 관리한다.
 * iocost는 struct ioc_gq 안에 struct blkg_policy_data pd 필드를 포함시켜
 * blkcg 프레임워크와 연결한다. 이 함수는 프레임워크가 반환한 pd 포인터에서
 * 실제 ioc_gq를 복원하는 역할을 한다.
 *
 * 실행 컨텍스트: blkcg 정책 콜백, 락 또는 RCU 보호 하에서 호출됨.
 * 호출자: blkg_to_iocg(), ioc_pd_alloc(), ioc_pd_init() 등.
 * 호출 대상: container_of 매크로 (NULL 체크 후 오프셋 계산).
 *
 * 호출 체인:
 *   blkcg 정책 콜백 → [pd_to_iocg] → struct ioc_gq 접근
 */
static struct ioc_gq *pd_to_iocg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct ioc_gq, pd) : NULL; /* [한국어] pd가 NULL이면 NULL 반환, 유효하면 ioc_gq 구조체 시작 주소로 복원 — NULL 전파로 호출자 중복 체크 방지 */
}

/*
 * [한국어]
 * blkg_to_iocg - blkcg_gq(블록 cgroup 큐 노드)에서 ioc_gq를 가져오는 접근자
 *
 * @blkg: cgroup과 request_queue의 교차점을 나타내는 blkcg_gq 구조체.
 *        blkcg 계층구조의 각 노드가 갖는 per-(cgroup×device) 객체.
 * @return: 이 blkg에 연결된 iocost 정책 데이터(ioc_gq). 등록 전이면 NULL.
 *
 * blkg_to_pd()는 blkcg_policy 포인터로 정책을 특정하여 blkg_policy_data를
 * 반환한다. 이를 pd_to_iocg()로 ioc_gq로 변환하는 두 단계를 합친 래퍼.
 * iocost 전역에서 blkg가 주어진 컨텍스트에서 ioc_gq로 진입하는 주된 경로.
 *
 * 실행 컨텍스트: blkcg 정책 콜백 (RCU/락 보호 하).
 * 호출자: iocost 내 blkg 순회 로직, ioc_weight_show/store.
 * 호출 대상: blkg_to_pd() → pd_to_iocg().
 *
 * 호출 체인:
 *   cgroup 정책 콜백 → [blkg_to_iocg] → blkg_to_pd → pd_to_iocg → ioc_gq
 */
static struct ioc_gq *blkg_to_iocg(struct blkcg_gq *blkg)
{
	return pd_to_iocg(blkg_to_pd(blkg, &blkcg_policy_iocost)); /* [한국어] iocost 정책 식별자로 blkg에서 정책 데이터를 추출한 뒤 ioc_gq로 변환 */
}

/*
 * [한국어]
 * iocg_to_blkg - ioc_gq에서 blkcg_gq 포인터를 역으로 가져오는 접근자
 *
 * @iocg: iocost per-cgroup-per-device 상태를 담는 ioc_gq 구조체.
 * @return: 이 iocg를 포함하는 blkcg_gq 포인터.
 *          blkcg 프레임워크에 iocg를 등록/해제할 때 필요.
 *
 * iocg → blkcg_gq 역방향 변환이 필요한 경우(예: blkg_get/put으로 참조 카운트
 * 조작, blkg 순회 컨텍스트에서 blkcg API 호출)에 사용된다.
 * struct ioc_gq는 struct blkg_policy_data pd 필드를 통해 blkcg 프레임워크에
 * 연결되며, pd_to_blkg()가 pd → blkg_policy_data → blkcg_gq 역산을 수행한다.
 *
 * 실행 컨텍스트: blkcg 정책 콜백 또는 blkg 참조 카운트 관리 경로.
 * 호출자: iocg_activate(), iocg_kick_waitq(), iocg_pay_debt().
 * 호출 대상: pd_to_blkg() (blkcg 내부 역산).
 *
 * 호출 체인:
 *   iocg 내부 로직 → [iocg_to_blkg] → pd_to_blkg → blkcg_gq
 */
static struct blkcg_gq *iocg_to_blkg(struct ioc_gq *iocg)
{
	return pd_to_blkg(&iocg->pd); /* [한국어] iocg의 blkg_policy_data 주소로 pd_to_blkg()를 호출하여 blkcg_gq 복원 */
}

/*
 * [한국어]
 * blkcg_to_iocc - blkcg(블록 cgroup)에서 ioc_cgrp(iocost cgroup 정책 데이터)를 가져오는 접근자
 *
 * @blkcg: blkcg 계층의 cgroup 노드를 나타내는 blkcg 구조체.
 * @return: 이 blkcg에 연결된 iocost cgroup 레벨 정책 데이터(ioc_cgrp).
 *          초기화 전에는 호출하지 않도록 호출자가 보장해야 한다.
 *
 * blkcg 정책 프레임워크는 cgroup별 공통 정책 데이터를 blkcg_policy_data(cpd)로
 * 관리한다. iocost는 struct ioc_cgrp 안에 struct blkcg_policy_data cpd 필드를
 * 포함하며, blkcg_to_cpd()로 cpd를 얻은 뒤 container_of로 ioc_cgrp로 복원한다.
 * ioc_cgrp에는 weight(가중치) 등 cgroup 레벨 설정이 저장된다.
 *
 * 실행 컨텍스트: cgroup 파일시스템 read/write 콜백 (프로세스 컨텍스트, 슬리핑 가능).
 * 호출자: ioc_weight_show(), ioc_weight_store(), ioc_cpd_alloc(), ioc_cpd_init().
 * 호출 대상: blkcg_to_cpd() → container_of.
 *
 * 호출 체인:
 *   cgroupfs weight 읽기/쓰기 → [blkcg_to_iocc] → blkcg_to_cpd → ioc_cgrp
 */
static struct ioc_cgrp *blkcg_to_iocc(struct blkcg *blkcg)
{
	return container_of(blkcg_to_cpd(blkcg, &blkcg_policy_iocost), /* [한국어] iocost 정책 식별자로 blkcg에서 cgroup별 정책 데이터(cpd)를 추출 */
			    struct ioc_cgrp, cpd); /* [한국어] cpd 필드 오프셋으로 ioc_cgrp 구조체 시작 주소를 역산 */
}

/*
 * Scale @abs_cost to the inverse of @hw_inuse.  The lower the hierarchical
 * weight, the more expensive each IO.  Must round up.
 */
/*
 * [한국어]
 * abs_cost_to_cost - 절대 비용(abs_cost)을 계층 가중치(hw_inuse)로 스케일하여 vtime 비용으로 변환
 *
 * @abs_cost: WEIGHT_ONE(65536) 기준의 장치 절대 IO 비용. 장치 선형 모델(lcoefs)로
 *            계산된 값으로, 모든 cgroup에 동일하게 적용되는 장치 공통 기준값.
 * @hw_inuse: 이 cgroup의 계층적 유효 가중치 (0 ~ WEIGHT_ONE=65536).
 *            부모 cgroup의 가중치까지 반영한 실효 가중치. 낮을수록 비싸진다.
 * @return: vtime 단위로 환산된 실제 IO 비용. 올림(round up)하여 과소 계상을 방지.
 *
 * iocost의 핵심 공식: cost = abs_cost * WEIGHT_ONE / hw_inuse.
 * hw_inuse가 WEIGHT_ONE이면(가중치 100%) abs_cost == cost.
 * hw_inuse가 작을수록(낮은 우선순위 cgroup) 동일한 IO에 더 많은 vtime 비용이 부과되어
 * 가중치 비례로 장치 자원을 분배한다. 올림을 보장하는 이유는 vtime 예산 소진 시
 * 버짓 부족 판정에서 cgroup이 소량의 무료 IO를 얻지 못하도록 보수적으로 계산하기 위함.
 *
 * 실행 컨텍스트: IO 제출 경로 (iocg_commit_bio 등), IRQ-safe.
 * 호출자: iocg_commit_bio(), ioc_rqos_throttle(), ioc_looking_at_rt_budget().
 * 호출 대상: DIV64_U64_ROUND_UP (올림 나눗셈 매크로).
 *
 * 호출 체인:
 *   bio 제출 경로 → [abs_cost_to_cost] → DIV64_U64_ROUND_UP → vtime cost
 */
static u64 abs_cost_to_cost(u64 abs_cost, u32 hw_inuse) /* hw_inuse가 낮을수록 동일 NVMe 명령 비용 증가 */
{
	return DIV64_U64_ROUND_UP(abs_cost * WEIGHT_ONE, hw_inuse);	/* hweight_inuse 반비례 NVMe cost 환산 */
}

/*
 * The inverse of abs_cost_to_cost().  Must round up.
 */
/*
 * [한국어]
 * cost_to_abs_cost - vtime 비용을 절대 비용(abs_cost)으로 역변환
 *
 * @cost: iocg의 vtime에서 사용된 비용 단위. abs_cost_to_cost()의 반환값.
 * @hw_inuse: 이 cgroup의 계층적 유효 가중치 (0 ~ WEIGHT_ONE=65536).
 * @return: WEIGHT_ONE 기준 장치 절대 비용. 올림(round up)하여 과소 계상 방지.
 *
 * abs_cost_to_cost()의 역산: abs_cost = cost * hw_inuse / WEIGHT_ONE.
 * 주로 iocg가 소비한 vtime cost를 abs_vusage(절대 사용량 통계)로 기록하거나,
 * vtime 부채를 장치 단위의 절대값으로 환산할 때 사용된다.
 * 마찬가지로 올림을 보장하여 비용 역산 시 실제 소비보다 적게 나타나지 않도록 한다.
 *
 * 실행 컨텍스트: 타이머/통계 경로 또는 IO 제출 직후.
 * 호출자: iocg_commit_bio()의 abs_vusage 누적 경로, ioc_adjust_base_vrate().
 * 호출 대상: DIV64_U64_ROUND_UP.
 *
 * 호출 체인:
 *   IO 완료/통계 수집 경로 → [cost_to_abs_cost] → DIV64_U64_ROUND_UP → abs_cost
 */
static u64 cost_to_abs_cost(u64 cost, u32 hw_inuse) /* vtime을 NVMe 절대 비용으로 역환산 */
{
	return DIV64_U64_ROUND_UP(cost * hw_inuse, WEIGHT_ONE);	/* hweight_inuse 비례 NVMe 절대 비용 복원 */
}

/*
 * [한국어]
 * iocg_commit_bio - bio에 IO 비용을 확정하고 iocg의 vtime과 통계를 갱신
 *
 * @iocg: 비용을 부과할 cgroup-device 교차 상태 구조체. bio를 제출하는 프로세스가
 *        속한 cgroup에 대응하는 ioc_gq.
 * @bio: 비용을 기록할 bio 구조체. bi_iocost_cost 필드에 vtime 비용을 저장하여
 *       완료 시 ioc_rqos_done()이 이 값으로 done_vtime을 갱신할 수 있게 한다.
 * @abs_cost: WEIGHT_ONE 기준의 장치 절대 비용. per-CPU 절대 사용량 통계에 누적.
 * @cost: hw_inuse로 스케일된 실제 vtime 비용. iocg->vtime에 원자적으로 더해진다.
 * @return: 없음 (void).
 *
 * bio가 실제로 IO 큐에 진입할 때 호출되어 세 가지 상태를 갱신한다:
 * 1) bio->bi_iocost_cost: bio별 비용 기록 → 완료 콜백에서 사용.
 * 2) iocg->vtime: 이 cgroup의 가상 시간 소비 누적 (atomic64, 다중 CPU 안전).
 * 3) iocg->pcpu_stat->abs_vusage: per-CPU 절대 사용량 누적 (local64, false sharing 방지).
 *
 * 실행 컨텍스트: bio 제출 경로 (ioc_rqos_throttle 또는 직접 commit), 선점 비활성화 중.
 * 호출자: ioc_rqos_throttle(), ioc_rqos_merge().
 * 호출 대상: atomic64_add(), get_cpu_ptr(), local64_add(), put_cpu_ptr().
 *
 * 호출 체인:
 *   ioc_rqos_throttle/merge → [iocg_commit_bio] → atomic64_add(iocg->vtime),
 *                                                    local64_add(abs_vusage)
 */
static void iocg_commit_bio(struct ioc_gq *iocg, struct bio *bio,
			    u64 abs_cost, u64 cost)
{
	struct iocg_pcpu_stat *gcs; /* [한국어] 현재 CPU에 바인딩된 per-CPU 통계 포인터 — false sharing 없이 사용량을 누적하기 위한 변수 */

	bio->bi_iocost_cost = cost;	/* bio 단위 NVMe 비용 기록 → CQ 완료 시 done_vtime 차감 */
	atomic64_add(cost, &iocg->vtime);	/* atomic: 다중 CPU에서 NVMe 제출 경쟁 시에도 vtime 일관 */

	gcs = get_cpu_ptr(iocg->pcpu_stat);	/* 현재 CPU의 NVMe 사용량 통계 획득 */
	local64_add(abs_cost, &gcs->abs_vusage);	/* per-CPU local64: NVMe 사용량 누적, 캐시 일관성 최소화 */
	put_cpu_ptr(gcs);	/* preemption 복원: 다른 CPU로 이주 시에도 NVMe 통계 정확성 */
}

/*
 * [한국어]
 * iocg_lock - ioc와 iocg의 락을 IRQ 비활성화와 함께 획득
 *
 * @iocg: 락을 걸 대상 ioc_gq. waitq.lock이 항상 획득되며,
 *        lock_ioc가 참이면 상위 ioc->lock도 함께 획득된다.
 * @lock_ioc: true이면 ioc->lock + iocg->waitq.lock 순서로 중첩 락 획득.
 *            false이면 iocg->waitq.lock만 획득 (빠른 경로).
 * @flags: IRQ 상태 저장용. spin_lock_irqsave()가 현재 IRQ 마스크를 저장.
 *         iocg_unlock()에 그대로 전달해야 한다.
 * @return: 없음 (void).
 *
 * iocost에는 두 레벨의 락이 있다:
 * - ioc->lock: 장치 전역 상태(vrate, period, active_iocgs 목록 등) 보호.
 * - iocg->waitq.lock: 개별 cgroup의 예산 대기 큐와 부채 상태 보호.
 * 부채(debt) 처리나 vrate 조정처럼 두 상태를 동시에 변경할 때는 lock_ioc=true로
 * 두 락을 항상 ioc→waitq 순서로 획득하여 deadlock을 방지한다.
 * IRQ save는 타이머 콜백(ioc_timer_fn)이 동일한 락을 획득하는 인터럽트 경로와의
 * 경쟁을 방지하기 위해 필수다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 softirq. IRQ 비활성화로 인터럽트 안전.
 * 호출자: iocg_activate(), iocg_kick_waitq(), ioc_rqos_throttle() 내 부채 처리.
 * 호출 대상: spin_lock_irqsave(), spin_lock().
 *
 * 호출 체인:
 *   부채 처리/waitq 조작 → [iocg_lock] → spin_lock_irqsave(ioc->lock),
 *                                          spin_lock(waitq.lock)
 */
static void iocg_lock(struct ioc_gq *iocg, bool lock_ioc, unsigned long *flags)
{
	if (lock_ioc) {	/* debt 처리 시 ioc->lock + waitq.lock 중첩: NVMe 예산/부채 동시 변경 방지 */
		spin_lock_irqsave(&iocg->ioc->lock, *flags);	/* ioc 레벨 lock: vrate/weight/주기 보호 */
		spin_lock(&iocg->waitq.lock);	/* waitq lock: NVMe 예산 대기자 상태 보호 */
	} else { /* [한국어] waitq.lock만 필요한 빠른 경로 — ioc 전역 상태를 건드리지 않는 경우 */
		spin_lock_irqsave(&iocg->waitq.lock, *flags); /* [한국어] waitq.lock을 IRQ 비활성화와 함께 획득 — 타이머 콜백과의 경쟁 방지 */
	}
}

/*
 * [한국어]
 * iocg_unlock - iocg_lock()으로 획득한 락을 역순으로 해제
 *
 * @iocg: 락을 해제할 ioc_gq. iocg_lock()과 동일한 인스턴스여야 한다.
 * @unlock_ioc: true이면 waitq.lock → ioc->lock 역순으로 두 락 해제.
 *              false이면 waitq.lock만 해제.
 * @flags: iocg_lock()이 저장한 IRQ 상태. irqrestore로 복원된다.
 * @return: 없음 (void).
 *
 * iocg_lock()의 대칭 해제 함수. 락 획득 역순(waitq → ioc)으로 해제하여
 * 락 순서 규칙을 유지한다. unlock_ioc 플래그는 iocg_lock()과 반드시 일치해야 한다.
 * IRQ restore는 iocg_lock() 진입 시점의 인터럽트 활성화 상태를 정확히 복원한다.
 *
 * 실행 컨텍스트: iocg_lock()과 동일한 컨텍스트 (짝을 이뤄 호출).
 * 호출자: iocg_activate(), iocg_kick_waitq(), ioc_rqos_throttle() 부채 처리.
 * 호출 대상: spin_unlock(), spin_unlock_irqrestore().
 *
 * 호출 체인:
 *   부채 처리/waitq 조작 완료 → [iocg_unlock] → spin_unlock(waitq.lock),
 *                                                  spin_unlock_irqrestore(ioc->lock)
 */
static void iocg_unlock(struct ioc_gq *iocg, bool unlock_ioc, unsigned long *flags)
{
	if (unlock_ioc) { /* [한국어] ioc->lock도 잡혀 있는 경우 — 역순으로 waitq.lock 먼저 해제 */
		spin_unlock(&iocg->waitq.lock); /* [한국어] waitq.lock 먼저 해제 — 락 획득 역순 원칙 준수 */
		spin_unlock_irqrestore(&iocg->ioc->lock, *flags); /* [한국어] ioc->lock 해제와 동시에 IRQ 상태 복원 */
	} else { /* [한국어] waitq.lock만 잡혀 있는 빠른 경로 */
		spin_unlock_irqrestore(&iocg->waitq.lock, *flags); /* [한국어] waitq.lock 해제와 IRQ 상태 복원을 한 번에 */
	}
}

#define CREATE_TRACE_POINTS
#include <trace/events/iocost.h>

/*
 * [한국어]
 * ioc_refresh_margins - 타이머 주기와 vrate를 바탕으로 cgroup vtime 여유분 재계산
 *
 * @ioc: 여유분을 갱신할 ioc 컨트롤러. ioc->period_us와 ioc->vtime_base_rate를
 *       읽어 ioc->margins.(min/low/target)을 덮어쓴다.
 * @return: 없음 (void). ioc->margins 구조체를 직접 갱신한다.
 *
 * iocost는 각 cgroup이 장치 vtime 대비 얼마나 뒤처질 수 있는지를 세 임계값으로
 * 관리한다: min(최소 허용 지연), low(속도 감시 시작), target(이상적 목표 지연).
 * 이 임계값들은 타이머 주기(period_us)의 특정 비율(MARGIN_*_PCT)에 vrate를 곱하여
 * vtime 단위로 표현된다. period_us나 vrate가 변경될 때마다 이 함수가 호출되어
 * 마진 값들을 최신 상태로 유지한다. 마진이 올바르지 않으면 cgroup을 너무 일찍
 * 또는 너무 늦게 조절(throttle)하는 문제가 발생한다.
 *
 * 실행 컨텍스트: ioc->lock을 보유한 상태에서 호출됨 (period/vrate 갱신 직후).
 * 호출자: ioc_refresh_period_us(), ioc_adjust_base_vrate(), ioc_refresh_params_disk().
 * 호출 대상: 산술 연산만 수행 (함수 호출 없음).
 *
 * 호출 체인:
 *   ioc_refresh_period_us / ioc_adjust_base_vrate → [ioc_refresh_margins] →
 *   ioc->margins.(min/low/target) 갱신
 */
static void ioc_refresh_margins(struct ioc *ioc)
{
	struct ioc_margins *margins = &ioc->margins; /* [한국어] 갱신 대상 마진 구조체 포인터 — ioc 내 임베디드 필드 */
	u32 period_us = ioc->period_us;	/* NVMe latency QoS에서 유도된 현재 제어 주기 */
	u64 vrate = ioc->vtime_base_rate;	/* 현재 NVMe IO 속도 보정값 */

	margins->min = (period_us * MARGIN_MIN_PCT / 100) * vrate;	/* NVMe SQ 포화 직전 최소 vtime 여유 */
	margins->low = (period_us * MARGIN_LOW_PCT / 100) * vrate;	/* NVMe 제출률 검토 임계 vtime */
	margins->target = (period_us * MARGIN_TARGET_PCT / 100) * vrate;	/* NVMe SQ/CQ 안정 목표 vtime 버퍼 */
}

/* latency Qos params changed, update period_us and all the dependent params */
/*
 * [한국어]
 * ioc_refresh_period_us - latency QoS 목표값으로부터 ioc 타이머 주기(period_us) 계산
 *
 * @ioc: 타이머 주기를 갱신할 ioc 컨트롤러. ioc->params.qos[]에서 latency 목표를
 *       읽어 ioc->period_us, ioc->timer_slack_ns를 갱신하고 margins도 재계산한다.
 * @return: 없음 (void). ioc->period_us 및 파생 파라미터를 직접 변경한다.
 *
 * iocost 타이머 주기는 읽기/쓰기 latency QoS 목표(QOS_RLAT/WLAT) 중 큰 값과
 * 해당 백분위수(QOS_RPPM/WPPM)로부터 결정된다. 백분위수가 낮을수록(예: p50 = 500000ppm)
 * 타이머 주기를 더 길게 잡아 충분한 IO 샘플을 포함시킨다. 이 주기가 너무 짧으면
 * 제어가 불안정해지고 너무 길면 장치 포화 반응이 느려진다. 주기는
 * [MIN_PERIOD, MAX_PERIOD] 범위로 clamp하여 극단값을 방지한다.
 * 타이머 주기가 확정되면 timer_slack_ns(타이머 오차 허용값)와 margins도 함께 갱신된다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (lockdep_assert_held로 확인).
 * 호출자: ioc_refresh_params_disk() - QoS 파라미터가 변경될 때마다 호출.
 * 호출 대상: ioc_refresh_margins().
 *
 * 호출 체인:
 *   ioc_refresh_params_disk → [ioc_refresh_period_us] → ioc_refresh_margins
 */
static void ioc_refresh_period_us(struct ioc *ioc)
{
	u32 ppm, lat, multi, period_us; /* [한국어] ppm: 백분위수(parts-per-million), lat: latency 목표(μs), multi: 배율, period_us: 최종 주기 */

	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep가 경고 — period 갱신은 락 하에서만 안전 */

	/* pick the higher latency target */
	/* NVMe read/write 중 느린 쪽이 병목 결정 */
	if (ioc->params.qos[QOS_RLAT] >= ioc->params.qos[QOS_WLAT]) {	/* NVMe read QoS가 write보다 느리면 read 기준 */
		ppm = ioc->params.qos[QOS_RPPM];	/* read latency QoS 백분위수 */
		lat = ioc->params.qos[QOS_RLAT];	/* read latency 목표(μs): NVMe CQ ISR 처리 목표 */
	} else { /* [한국어] write latency가 read보다 엄격한 경우 write 기준으로 주기 결정 */
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
	if (ppm) /* [한국어] 유효한 백분위수가 있으면 — ppm=0은 QoS 목표 미설정을 의미 */
		multi = max_t(u32, (MILLION - ppm) / 50000, 2);	/* 백분위수가 낮을수록(예: p50) NVMe 샘플 주기를 길게 */
	else
		multi = 2; /* [한국어] ppm=0(백분위수 미설정): 기본 배율 2로 안전한 최솟값 사용 */
	period_us = multi * lat;	/* NVMe 완료 지연의 배수로 타이머 주기 산출 */
	period_us = clamp_t(u32, period_us, MIN_PERIOD, MAX_PERIOD);	/* NVMe 제어 반응성/안정성 균형 */

	/* calculate dependent params */
	ioc->period_us = period_us; /* [한국어] 확정된 주기를 ioc에 저장 — 이후 ioc_refresh_margins가 이 값을 참조 */
	ioc->timer_slack_ns = div64_u64(	/* [한국어] 대기 큐 타이머의 허용 오차(slack). 여러 타이머를 근접 시각에
					 * 몰아 한 번에 처리하게 해 타이머 인터럽트 횟수를 줄인다. */
		(u64)period_us * NSEC_PER_USEC * TIMER_SLACK_PCT,
		100);
	ioc_refresh_margins(ioc); /* [한국어] 새 period_us 기반으로 min/low/target 마진 재계산 */
}

/*
 *  ioc->rqos.disk isn't initialized when this function is called from
 *  the init path.
 */
/*
 * [한국어]
 * ioc_autop_idx - 장치 특성과 현재 vrate 추이를 바탕으로 자동 파라미터 프로파일 인덱스 선택
 *
 * @ioc: 현재 적용 중인 프로파일 인덱스(autop_idx), vtime_base_rate, 사용자 오버라이드
 *       플래그(user_qos_params/user_cost_model), 전환 시작 타임스탬프를 읽고 쓴다.
 * @disk: 장치 특성(회전/큐깊이)을 조회할 gendisk. init 경로에서는 ioc->rqos.disk가
 *        아직 NULL일 수 있으므로 별도 인수로 받는다.
 * @return: AUTOP_HDD, AUTOP_SSD_QD1, AUTOP_SSD_DFL, AUTOP_SSD_FAST 중 하나.
 *          현재 장치와 vrate 상황에 가장 적합한 프로파일 인덱스.
 *
 * iocost는 장치 유형(HDD/SSD)과 성능 특성에 따라 네 가지 파라미터 세트를 미리 정의한다.
 * 이 함수는 다음 순서로 적합한 프로파일을 결정한다:
 * 1) blk_queue_rot()으로 HDD 여부 판별 → AUTOP_HDD 즉시 반환.
 * 2) queue depth == 1이면 NCQ 미지원/깨진 SATA SSD → AUTOP_SSD_QD1 반환.
 * 3) 이전 프로파일이 HDD/QD1이었으면 기본 SSD 프로파일(AUTOP_SSD_DFL)로 초기화.
 * 4) 사용자가 QoS/비용 모델을 오버라이드하면 현재 프로파일 유지.
 * 5) vrate_pct(현재 vtime 속도 대비 기준 속도 비율)로 too_fast/too_slow 경계를 검사.
 *    특정 시간(AUTOP_CYCLE_NSEC) 이상 지속되면 상위/하위 프로파일로 전환.
 *    이 이력(hysteresis) 메커니즘은 프로파일 간 떨림(oscillation)을 방지한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태에서 호출됨 (ioc_refresh_params_disk 내부).
 * 호출자: ioc_refresh_params_disk().
 * 호출 대상: blk_queue_rot(), blk_queue_depth(), div64_u64(), blk_time_get_ns().
 *
 * 호출 체인:
 *   ioc_refresh_params_disk → [ioc_autop_idx] → blk_queue_rot/depth, blk_time_get_ns
 */
static int ioc_autop_idx(struct ioc *ioc, struct gendisk *disk)
{
	int idx = ioc->autop_idx; /* [한국어] 현재 적용 중인 프로파일 인덱스 — 변경 여부를 비교하는 기준값 */
	const struct ioc_params *p = &autop[idx]; /* [한국어] 현재 프로파일의 too_fast/too_slow 임계 vrate_pct 참조 */
	u32 vrate_pct; /* [한국어] 현재 vtime_base_rate를 백분율로 환산한 값 — 프로파일 전환 판단에 사용 */
	u64 now_ns; /* [한국어] 현재 monotonic 시각(nanoseconds) — 프로파일 전환 유지 시간 계산용 */

	/* rotational? */	/* blk_queue_rot: NVMe가 아닌 회전 미디어(HDD) 분기 → seek cost 모델 */
	if (blk_queue_rot(disk->queue))	/* NVMe 장치가 아닌 HDD면 AUTOP_HDD */
		return AUTOP_HDD;

	/* handle SATA SSDs w/ broken NCQ */	/* [한국어] 원본 주석대로 NCQ가 고장난 SATA SSD 대응 —
						 * 큐 깊이가 1이면 병렬성이 없어 비용 모델을 다르게 잡아야 한다. */
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

	if (p->too_fast_vrate_pct && p->too_fast_vrate_pct <= vrate_pct) { /* [한국어] 현재 프로파일에 too_fast 임계가 정의되어 있고 현재 vrate가 그 이상이면 — 장치가 더 빠른 프로파일로 올릴 여력이 있음 */
		if (!ioc->autop_too_fast_at) /* [한국어] too_fast 상태 진입 시각 미기록이면 지금을 시작점으로 기록 */
			ioc->autop_too_fast_at = now_ns; /* [한국어] 이 타임스탬프로부터 AUTOP_CYCLE_NSEC 이상 유지되면 프로파일 업 */
		if (now_ns - ioc->autop_too_fast_at >= AUTOP_CYCLE_NSEC) /* [한국어] 충분히 오래(hysteresis) too_fast 상태가 유지되었으면 상위 프로파일로 전환 */
			return idx + 1; /* [한국어] 상위 프로파일 인덱스 반환 — 호출자 ioc_refresh_params_disk가 실제 전환 수행 */
	} else { /* [한국어] too_fast 조건 미충족 또는 임계 미정의 — 카운터 초기화 */
		ioc->autop_too_fast_at = 0; /* [한국어] too_fast 이력 리셋 — 잠깐 회복되어도 처음부터 다시 측정 */
	}

	if (p->too_slow_vrate_pct && p->too_slow_vrate_pct >= vrate_pct) { /* [한국어] 현재 프로파일에 too_slow 임계가 정의되어 있고 현재 vrate가 그 이하이면 — 장치가 현 프로파일에 비해 느림 */
		if (!ioc->autop_too_slow_at) /* [한국어] too_slow 시작 시각 미기록이면 지금을 기록 */
			ioc->autop_too_slow_at = now_ns; /* [한국어] AUTOP_CYCLE_NSEC 이상 지속되면 하위 프로파일로 강등 */
		if (now_ns - ioc->autop_too_slow_at >= AUTOP_CYCLE_NSEC) /* [한국어] 충분히 오래 too_slow 상태가 유지되었으면 하위 프로파일로 전환 */
			return idx - 1; /* [한국어] 하위 프로파일 인덱스 반환 — 더 보수적인 파라미터 세트 적용 */
	} else { /* [한국어] too_slow 조건 미충족 또는 임계 미정의 — 카운터 초기화 */
		ioc->autop_too_slow_at = 0; /* [한국어] too_slow 이력 리셋 — 조건 해소 시 즉시 카운터 클리어 */
	}

	return idx; /* [한국어] 전환 조건 미충족 — 현재 프로파일 인덱스 유지 */
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
/*
 * [한국어]
 * calc_lcoefs - 장치 최대 성능 수치(bps/seqiops/randiops)에서 선형 비용 모델 계수 계산
 *
 * @bps: 장치의 최대 순차 처리량(bytes/sec). 이 값으로 4096바이트 페이지당 vtime 비용을 계산.
 *       0이면 bps 기반 page 계수는 0으로 설정 (seqiops/randiops 만으로 비용 결정).
 * @seqiops: 장치의 최대 순차 4K IOPS. 순차 IO의 고정 오버헤드(page 비용 제외)를 계산.
 * @randiops: 장치의 최대 랜덤 4K IOPS. 랜덤 IO의 고정 오버헤드(page 비용 제외)를 계산.
 * @page: [출력] 4096바이트(1 IOC_PAGE_SIZE)당 vtime 비용. = VTIME_PER_SEC / (bps/4096).
 * @seqio: [출력] 순차 IO 1회의 기본 오버헤드 비용(page 비용 제외). = max(VTIME_PER_SEC/seqiops - *page, 0).
 * @randio: [출력] 랜덤 IO 1회의 기본 오버헤드 비용(page 비용 제외). = max(VTIME_PER_SEC/randiops - *page, 0).
 * @return: 없음 (void). *page, *seqio, *randio에 결과를 저장.
 *
 * iocost의 선형 비용 모델: IO 비용 = size_pages * page_cost + seqio_cost (또는 randio_cost).
 * 이 모델은 IO 크기에 비례하는 전송 비용(page)과 크기와 무관한 고정 오버헤드(seqio/randio)를
 * 분리한다. 고정 오버헤드가 page 비용보다 작으면 0으로 클램프하여 음수를 방지한다.
 * 모든 계수는 VTIME_PER_SEC 단위로 표현되어 vtime과 직접 비교 가능하다.
 * 올림(round up)을 사용하는 이유: 실제 장치 성능보다 비용을 약간 높게 잡아
 * 예산 초과 시 보수적으로 조절하기 위함이다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (ioc_refresh_lcoefs 호출 시).
 * 호출자: ioc_refresh_lcoefs() — read/write 각각 한 번씩 총 두 번 호출.
 * 호출 대상: DIV_ROUND_UP_ULL(), DIV64_U64_ROUND_UP().
 *
 * 호출 체인:
 *   ioc_refresh_lcoefs → [calc_lcoefs] (read용, write용) →
 *   ioc->params.lcoefs[LCOEF_RPAGE/RSEQIO/RRANDIO/WPAGE/WSEQIO/WRANDIO]
 */
static void calc_lcoefs(u64 bps, u64 seqiops, u64 randiops,
			u64 *page, u64 *seqio, u64 *randio)
{
	u64 v; /* [한국어] VTIME_PER_SEC / iops로 계산한 IO당 전체 vtime 비용을 임시 저장 — page 비용과의 차이를 구하기 위한 중간값 */

	*page = *seqio = *randio = 0; /* [한국어] 출력 계수를 0으로 초기화 — bps/seqiops/randiops가 0인 경우 비용 미설정으로 처리 */

	if (bps) { /* [한국어] bps가 설정된 경우에만 page(전송) 비용 계산 — bps=0은 장치 처리량 무제한을 의미 */
		u64 bps_pages = DIV_ROUND_UP_ULL(bps, IOC_PAGE_SIZE); /* [한국어] bps를 IOC_PAGE_SIZE(4096바이트)로 나누어 초당 처리 페이지 수 계산 — 올림으로 과소 계상 방지 */

		if (bps_pages) /* [한국어] 정상 계산 경로: bps_pages > 0이면 페이지당 vtime 비용 산출 */
			*page = DIV64_U64_ROUND_UP(VTIME_PER_SEC, bps_pages); /* [한국어] page 비용 = 1초의 vtime / 초당 처리 페이지 수 — 페이지 전송에 걸리는 vtime */
		else
			*page = 1; /* [한국어] bps_pages가 0이 되는 극단적 경우(bps가 매우 크지만 올림 후 0) — 최소 비용 1로 설정해 0 나눗셈 방지 */
	}

	if (seqiops) { /* [한국어] 순차 IOPS가 설정된 경우에만 순차 IO 고정 오버헤드 계산 */
		v = DIV64_U64_ROUND_UP(VTIME_PER_SEC, seqiops); /* [한국어] IO 1회당 전체 vtime 비용 = 1초의 vtime / 초당 IO 횟수 */
		if (v > *page) /* [한국어] 전체 비용에서 page(전송) 비용을 뺀 나머지가 순차 오버헤드 — 음수 방지를 위해 v > *page 조건 확인 */
			*seqio = v - *page; /* [한국어] 순차 IO 기본 오버헤드 = IO당 총 비용 - 전송 비용 (예: seek time 없지만 명령 제출/완료 오버헤드) */
	}

	if (randiops) { /* [한국어] 랜덤 IOPS가 설정된 경우에만 랜덤 IO 고정 오버헤드 계산 */
		v = DIV64_U64_ROUND_UP(VTIME_PER_SEC, randiops); /* [한국어] 랜덤 IO 1회당 전체 vtime 비용 — 순차보다 크며 seek 비용 포함 */
		if (v > *page) /* [한국어] 랜덤 오버헤드가 page 전송 비용보다 클 때만 유효 */
			*randio = v - *page; /* [한국어] 랜덤 IO 기본 오버헤드 = IO당 총 비용 - 전송 비용 (예: HDD seek time, SSD 내부 경합) */
	}
}

/*
 * [한국어]
 * ioc_refresh_lcoefs - 입력 지표(i_lcoefs)에서 선형 비용 모델 계수(lcoefs)를 갱신
 *
 * @ioc: 계수를 갱신할 ioc 컨트롤러. ioc->params.i_lcoefs[]에서 사용자 입력값을 읽어
 *       calc_lcoefs()를 통해 ioc->params.lcoefs[]를 덮어쓴다.
 * @return: 없음 (void).
 *
 * iocost는 사용자 설정 또는 자동 프로파일에서 얻은 원시 성능 수치(i_lcoefs: 입력용)와
 * 실제 IO 비용 계산에 쓰이는 계수(lcoefs: 출력용)를 분리한다. 이 함수는 두 배열 사이의
 * 변환을 read/write 방향으로 각각 수행한다. i_lcoefs 변경(사용자 설정 또는 자동 프로파일
 * 전환) 때마다 호출되어 lcoefs를 최신 상태로 유지한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (ioc_refresh_params_disk 내부).
 * 호출자: ioc_refresh_params_disk().
 * 호출 대상: calc_lcoefs() (read/write 방향 각 1회).
 *
 * 호출 체인:
 *   ioc_refresh_params_disk → [ioc_refresh_lcoefs] → calc_lcoefs (×2)
 *   → ioc->params.lcoefs[LCOEF_RPAGE/RSEQIO/RRANDIO/WPAGE/WSEQIO/WRANDIO]
 */
static void ioc_refresh_lcoefs(struct ioc *ioc)
{
	u64 *u = ioc->params.i_lcoefs; /* [한국어] 입력 지표 배열 포인터 — 사용자 설정 또는 autop 프로파일이 제공한 원시 성능 수치(bps/seqiops/randiops) */
	u64 *c = ioc->params.lcoefs; /* [한국어] 출력 계수 배열 포인터 — calc_lcoefs()가 계산한 page/seqio/randio 비용 계수를 저장할 대상 */

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
 * [한국어]
 * ioc_refresh_params_disk - 자동 프로파일 전환을 포함한 ioc 파라미터 전면 갱신
 *
 * @ioc: 파라미터를 갱신할 ioc 컨트롤러. autop_idx, vtime_rate, params.qos,
 *       params.i_lcoefs, period_us, lcoefs, vrate_min/max가 모두 변경될 수 있다.
 * @force: true이면 프로파일이 바뀌지 않아도 파라미터를 강제 재계산.
 *         초기화 경로(ioc_create)에서 최초 설정 시 사용된다.
 * @disk: 장치 특성(회전/큐깊이) 조회에 사용할 gendisk. init 경로에서
 *        ioc->rqos.disk가 아직 NULL이므로 별도로 받는다.
 * @return: true이면 파라미터가 실제로 변경됨 (호출자가 마진 재계산 등 후처리 필요).
 *          false이면 프로파일/파라미터 변경 없음 (force=false이고 idx 동일).
 *
 * iocost 파라미터 갱신의 중앙 진입점. 다음 순서로 동작한다:
 * 1) ioc_autop_idx()로 현재 장치에 맞는 프로파일 인덱스 결정.
 * 2) 프로파일이 바뀐 경우 vtime_rate/vtime_base_rate를 VTIME_PER_USEC로 리셋.
 * 3) autop_idx 및 too_fast/too_slow 이력 타임스탬프 갱신.
 * 4) 사용자 오버라이드가 없으면 프로파일의 qos와 i_lcoefs를 ioc->params에 복사.
 * 5) ioc_refresh_period_us()로 타이머 주기 재계산.
 * 6) ioc_refresh_lcoefs()로 비용 계수 재계산.
 * 7) vrate_min/vrate_max를 QOS_MIN/MAX에서 vtime 단위로 환산하여 저장.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (lockdep_assert_held로 확인).
 * 호출자: ioc_refresh_params(), ioc_create(), ioc_qos_read/write 등.
 * 호출 대상: ioc_autop_idx(), atomic64_set(), memcpy(), ioc_refresh_period_us(),
 *            ioc_refresh_lcoefs(), DIV64_U64_ROUND_UP().
 *
 * 호출 체인:
 *   ioc_refresh_params / ioc_create → [ioc_refresh_params_disk] →
 *   ioc_autop_idx, ioc_refresh_period_us, ioc_refresh_lcoefs
 */
static bool ioc_refresh_params_disk(struct ioc *ioc, bool force,
				    struct gendisk *disk)
{
	const struct ioc_params *p; /* [한국어] 선택된 autop 프로파일의 파라미터 포인터 — qos/i_lcoefs 복사 소스 */
	int idx; /* [한국어] ioc_autop_idx()가 반환한 새 프로파일 인덱스 — ioc->autop_idx와 비교하여 전환 여부 판단 */

	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — 전역 상태 변경은 락 하에서만 안전 */

	idx = ioc_autop_idx(ioc, disk); /* [한국어] 장치 특성과 vrate 추이를 바탕으로 최적 프로파일 인덱스 결정 */
	p = &autop[idx]; /* [한국어] 결정된 프로파일의 파라미터 세트 포인터 — qos[], i_lcoefs[] 복사 소스 */

	if (idx == ioc->autop_idx && !force)	/* NVMe 프로파일 변경 없으면 skip */
		return false;	/* NVMe 제출 억제 상태 유지 */

	if (idx != ioc->autop_idx) {	/* NVMe 장치 프로파일 전환 시 vrate 리셋 */
		atomic64_set(&ioc->vtime_rate, VTIME_PER_USEC);	/* atomic: 새 프로파일의 NVMe 기준 속도로 갱신 */
		ioc->vtime_base_rate = VTIME_PER_USEC; /* [한국어] 비원자적 base_rate도 동기화 — 타이머 경로가 이 값을 마진 계산에 사용 */
	}

	ioc->autop_idx = idx; /* [한국어] 현재 적용 중인 프로파일 인덱스 업데이트 — 다음 호출 때 비교 기준 */
	ioc->autop_too_fast_at = 0; /* [한국어] 프로파일 전환(또는 강제 갱신) 후 too_fast 이력 초기화 — 새 프로파일 기준으로 다시 측정 */
	ioc->autop_too_slow_at = 0; /* [한국어] 프로파일 전환(또는 강제 갱신) 후 too_slow 이력 초기화 — hysteresis 카운터 리셋 */

	if (!ioc->user_qos_params)	/* 사용자 미지정 시 자동 NVMe latency QoS 적용 */
		memcpy(ioc->params.qos, p->qos, sizeof(p->qos));
	if (!ioc->user_cost_model)	/* 사용자 미지정 시 자동 NVMe 비용 계수 적용 */
		memcpy(ioc->params.i_lcoefs, p->i_lcoefs, sizeof(p->i_lcoefs));

	ioc_refresh_period_us(ioc); /* [한국어] 새 qos[] 값으로 타이머 주기 및 마진 재계산 */
	ioc_refresh_lcoefs(ioc); /* [한국어] 새 i_lcoefs[] 값으로 선형 비용 모델 계수 재계산 */

	ioc->vrate_min = DIV64_U64_ROUND_UP((u64)ioc->params.qos[QOS_MIN] *	/* NVMe 최소 vrate 절대값 */
					    VTIME_PER_USEC, MILLION);
	ioc->vrate_max = DIV64_U64_ROUND_UP((u64)ioc->params.qos[QOS_MAX] *	/* NVMe 최대 vrate 절대값 */
					    VTIME_PER_USEC, MILLION);

	return true; /* [한국어] 파라미터 실제 변경됨 — 호출자(ioc_refresh_params 등)가 후속 처리(예: 활성 cgroup 재스케줄) 필요 */
}

/*
 * [한국어]
 * ioc_refresh_params - ioc->rqos.disk를 사용하는 ioc_refresh_params_disk() 편의 래퍼
 *
 * @ioc: 파라미터를 갱신할 ioc 컨트롤러. ioc->rqos.disk가 유효한 상태(초기화 완료 후)
 *       여야 하며, init 경로에서는 ioc_refresh_params_disk()를 직접 호출해야 한다.
 * @force: ioc_refresh_params_disk()의 force 인수로 그대로 전달.
 *         true이면 프로파일 미변경 시에도 강제 재계산.
 * @return: ioc_refresh_params_disk()의 반환값을 그대로 전달.
 *          true이면 파라미터 실제 변경됨, false이면 변경 없음.
 *
 * init 경로가 아닌 일반 런타임 경로(타이머 콜백, sysfs 설정 변경 등)에서는
 * ioc->rqos.disk가 항상 유효하므로 disk 인수를 직접 넘길 필요가 없다.
 * 이 래퍼는 ioc->rqos.disk를 자동으로 사용함으로써 호출 코드를 단순화한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (ioc_refresh_params_disk 내 lockdep 검사가 확인).
 * 호출자: ioc_timer_fn(), ioc_qos_write(), ioc_cost_model_write().
 * 호출 대상: ioc_refresh_params_disk().
 *
 * 호출 체인:
 *   타이머/sysfs 경로 → [ioc_refresh_params] → ioc_refresh_params_disk →
 *   ioc_autop_idx, ioc_refresh_period_us, ioc_refresh_lcoefs
 */
static bool ioc_refresh_params(struct ioc *ioc, bool force)
{
	return ioc_refresh_params_disk(ioc, force, ioc->rqos.disk); /* [한국어] 초기화 완료 후 ioc->rqos.disk는 유효 — disk 인수 없이 편리하게 파라미터 갱신 */
}

/*
 * When an iocg accumulates too much vtime or gets deactivated, we throw away
 * some vtime, which lowers the overall device utilization. As the exact amount
 * which is being thrown away is known, we can compensate by accelerating the
 * vrate accordingly so that the extra vtime generated in the current period
 * matches what got lost.
 */
/*
 * [한국어]
 * ioc_refresh_vrate - 버려진 vtime 예산 오차를 현재 주기 내에서 보정하기 위해 vtime_rate 재조정
 *
 * @ioc: vtime_rate를 조정할 ioc 컨트롤러.
 *       읽는 필드: ioc->period_at(주기 시작 wallclock μs), ioc->period_us(주기 길이 μs),
 *                  ioc->vtime_base_rate(목표 vrate), ioc->vtime_err(누적 오차).
 *       쓰는 필드: atomic64 ioc->vtime_rate(제출 경로가 실시간으로 읽는 vrate),
 *                  ioc->vtime_err(보정 후 잔여 오차).
 * @now: 현재 wallclock 스냅샷(now->now, μs). 주기 잔여 시간(pleft) 계산에 사용.
 * @return: 없음 (void). atomic64 vtime_rate와 vtime_err를 직접 변경한다.
 *
 * iocg가 너무 많은 vtime을 쌓거나 비활성화될 때 일부 vtime 예산이 버려지며,
 * 이로 인해 장치 활용도가 낮아진다. 버려진 양(vtime_err)은 정확히 알 수 있으므로,
 * 현재 주기의 남은 시간(pleft) 동안 vtime_rate를 일시 가속하거나 감속하여
 * 손실된 vtime을 같은 주기 안에서 보충한다.
 * 가속폭(vcomp)은 [-base_rate/2, +base_rate] 범위로 제한하여 과보정을 방지한다.
 * 주기가 이미 끝난 경우(pleft <= 0)에는 보정 없이 done 레이블로 건너뛰어
 * vtime_err만 ±vperiod 범위로 클램프한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태에서 호출됨 (lockdep_assert_held로 확인).
 *   ioc_timer_fn()의 타이머 콜백(softirq) 컨텍스트에서 실행된다.
 * 호출자: ioc_timer_fn() — 매 타이머 주기마다 vtime_err를 처리할 때 호출.
 * 호출 대상: div64_s64(), clamp(), atomic64_set().
 *
 * 호출 체인:
 *   ioc_timer_fn → [ioc_refresh_vrate] → atomic64_set(ioc->vtime_rate)
 */
static void ioc_refresh_vrate(struct ioc *ioc, struct ioc_now *now)
{
	s64 pleft = ioc->period_at + ioc->period_us - now->now;	/* [한국어] 현재 제어 주기의 잔여 시간(μs) — 이 시간 안에 vtime 오차를 보정해야 함 */
	s64 vperiod = ioc->period_us * ioc->vtime_base_rate;	/* [한국어] 한 주기 전체의 가상 시간 총량 — vtime_err의 상하한 클램프 기준으로 사용 */
	s64 vcomp, vcomp_min, vcomp_max; /* [한국어] vcomp: 최종 vrate 보정량; vcomp_min/max: 보정량의 허용 범위 */

	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — vtime_rate 변경은 락 하에서만 안전 */

	/* we need some time left in this period */
	if (pleft <= 0) /* [한국어] 주기가 이미 끝났으면 남은 시간이 없어 보정 불가 — done으로 건너뜀 */
		goto done; /* [한국어] 보정 없이 vtime_err 클램프만 수행하는 done 레이블로 이동 */

	/*
	 * Calculate how much vrate should be adjusted to offset the error.
	 * Limit the amount of adjustment and deduct the adjusted amount from
	 * the error.
	 */
	vcomp = -div64_s64(ioc->vtime_err, pleft); /* [한국어] 오차를 남은 주기 시간으로 나누어 보정에 필요한 vrate 증감량 계산 — 부호 반전: 양의 오차(과다 vtime)면 vrate 감소 */
	vcomp_min = -(ioc->vtime_base_rate >> 1); /* [한국어] vrate를 최대 기준값의 절반까지만 낮출 수 있음 — 지나친 감속 방지 */
	vcomp_max = ioc->vtime_base_rate; /* [한국어] vrate를 기준값의 2배까지만 높일 수 있음 — 지나친 가속 방지 */
	vcomp = clamp(vcomp, vcomp_min, vcomp_max); /* [한국어] 보정량을 허용 범위 내로 클램프 — 과도한 vrate 변동으로 인한 불안정 방지 */

	ioc->vtime_err += vcomp * pleft; /* [한국어] 이번에 보정한 vtime 양(vcomp * 남은시간)을 오차에서 차감 — 잔여 오차만 다음 주기로 이월 */

	atomic64_set(&ioc->vtime_rate, ioc->vtime_base_rate + vcomp); /* [한국어] atomic: 기준 vrate에 보정량을 더해 실제 vtime 전진 속도 갱신 — 제출 경로(ioc_now)가 이 값을 실시간으로 읽음 */
done:
	/* bound how much error can accumulate */
	ioc->vtime_err = clamp(ioc->vtime_err, -vperiod, vperiod); /* [한국어] 오차가 한 주기 vtime을 초과하면 클램프 — 누적 오차가 지나치게 커져 vrate 계산이 폭주하는 것을 방지 */
}

/*
 * [한국어]
 * ioc_adjust_base_vrate - 장치 포화도(busy_level)를 기반으로 기본 vrate(vtime_base_rate) 피드백 조정
 *
 * @ioc: vtime_base_rate를 갱신할 ioc 컨트롤러.
 *       읽는 필드: ioc->busy_level(포화 단계), ioc->vtime_base_rate(현재 기준 vrate),
 *                  ioc->vrate_min/vrate_max(사용자 설정 vrate 범위).
 *       쓰는 필드: ioc->vtime_base_rate(갱신 후 기준 vrate), ioc->margins(vrate 갱신 후 재계산).
 * @rq_wait_pct: 이번 주기 동안 IO 요청이 큐에서 대기한 비율(0~100 %). 트레이스용.
 * @nr_lagging: 현재 lag 상태(vtime이 너무 뒤처진)의 iocg 수. 포화 판단에 사용.
 * @nr_shortages: 이번 주기에 예산 부족으로 대기한 iocg 수. 트레이스용.
 * @prev_busy_level: 직전 주기의 busy_level. 변경 여부 판단 및 트레이스에 사용.
 * @missed_ppm: 읽기/쓰기 latency QoS 목표를 놓친 비율(parts-per-million) 배열 포인터.
 *              트레이스 이벤트에 전달.
 * @return: 없음 (void). ioc->vtime_base_rate와 ioc->margins를 직접 변경한다.
 *
 * iocost의 vrate 피드백 루프 핵심 함수. ioc_timer_fn()이 매 주기 끝에 busy_level을
 * 계산한 뒤 이 함수를 호출하여 기준 vrate를 조정한다. 동작 순서:
 * 1) busy_level이 0이거나 음수(여유)이면서 lag 중인 cgroup이 있으면 조정 보류.
 * 2) vrate가 사용자 범위 밖이면 VRATE_CLAMP_ADJ_PCT씩 점진적으로 범위 안으로 끌어당김.
 *    이렇게 점진적으로 처리하는 이유: 범위 경계가 갑자기 바뀔 수 있으므로 한 번에
 *    큰 폭으로 변경하면 제어가 불안정해질 수 있다.
 * 3) 범위 내이면 busy_level의 크기에 대응하는 vrate_adj_pct[]를 조회하여
 *    busy_level > 0(포화)이면 vrate 감소, busy_level < 0(여유)이면 vrate 증가.
 * 4) 새 vrate로 ioc->vtime_base_rate 갱신 후 ioc_refresh_margins()로 마진 재계산.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태의 타이머 콜백(softirq). ioc_timer_fn() 내부에서만 호출.
 * 호출자: ioc_timer_fn().
 * 호출 대상: div64_u64(), min/max(), clamp(), DIV64_U64_ROUND_UP(),
 *            trace_iocost_ioc_vrate_adj(), ioc_refresh_margins().
 *
 * 호출 체인:
 *   ioc_timer_fn → [ioc_adjust_base_vrate] → ioc_refresh_margins
 */
static void ioc_adjust_base_vrate(struct ioc *ioc, u32 rq_wait_pct,
				  int nr_lagging, int nr_shortages,
				  int prev_busy_level, u32 *missed_ppm)
{
	u64 vrate = ioc->vtime_base_rate; /* [한국어] 현재 기준 vrate를 지역 변수로 복사 — 계산 중 원본은 변경하지 않고 최종 결과만 기록 */
	u64 vrate_min = ioc->vrate_min, vrate_max = ioc->vrate_max; /* [한국어] 사용자 QoS 파라미터(QOS_MIN/MAX)에서 변환된 vrate 허용 범위 */

	if (!ioc->busy_level || (ioc->busy_level < 0 && nr_lagging)) { /* [한국어] busy_level이 0(중립)이거나, 여유 상태(음수)이면서 vtime 뒤처진 cgroup이 존재하면 vrate 조정 보류 */
		if (ioc->busy_level != prev_busy_level || nr_lagging) /* [한국어] busy_level이 바뀌었거나 lag 중인 cgroup이 있으면 트레이스 이벤트 기록 — 상태 변화 추적용 */
			trace_iocost_ioc_vrate_adj(ioc, vrate,
						   missed_ppm, rq_wait_pct,
						   nr_lagging, nr_shortages); /* [한국어] 현재 vrate, 미달 ppm, 큐 대기율, 지연/부족 cgroup 수를 트레이스에 기록 */

		return; /* [한국어] vrate 조정 없이 반환 — 포화 신호가 불명확하면 현 vrate 유지 */
	}

	/*
	 * If vrate is out of bounds, apply clamp gradually as the
	 * bounds can change abruptly.  Otherwise, apply busy_level
	 * based adjustment.
	 */
	if (vrate < vrate_min) { /* [한국어] vrate가 사용자 설정 하한 아래에 있으면 점진적으로 상승 */
		vrate = div64_u64(vrate * (100 + VRATE_CLAMP_ADJ_PCT), 100); /* [한국어] VRATE_CLAMP_ADJ_PCT%씩 vrate를 증가 — 한 번에 하한까지 점프하지 않아 급격한 변동 방지 */
		vrate = min(vrate, vrate_min); /* [한국어] 상승한 vrate가 하한을 넘지 않도록 최솟값으로 클램프 */
	} else if (vrate > vrate_max) { /* [한국어] vrate가 사용자 설정 상한 위에 있으면 점진적으로 하강 */
		vrate = div64_u64(vrate * (100 - VRATE_CLAMP_ADJ_PCT), 100); /* [한국어] VRATE_CLAMP_ADJ_PCT%씩 vrate를 감소 — 상한 초과 시에도 점진적으로 범위 안으로 복귀 */
		vrate = max(vrate, vrate_max); /* [한국어] 하강한 vrate가 상한 아래로 넘어가지 않도록 최댓값으로 클램프 */
	} else { /* [한국어] vrate가 허용 범위 내에 있으면 busy_level 기반 조정 적용 */
		int idx = min_t(int, abs(ioc->busy_level), /* [한국어] busy_level의 절댓값을 vrate_adj_pct[] 인덱스로 사용 — 포화/여유 정도에 따른 조정폭 선택 */
				ARRAY_SIZE(vrate_adj_pct) - 1); /* [한국어] 인덱스가 배열 경계를 벗어나지 않도록 클램프 */
		u32 adj_pct = vrate_adj_pct[idx]; /* [한국어] 해당 busy_level에 대응하는 vrate 조정 백분율(예: 1%, 3%, 5%, ...) */

		if (ioc->busy_level > 0) /* [한국어] busy_level 양수 = 장치 포화 → vrate를 adj_pct만큼 낮춤으로써 cgroup에 허용되는 vtime 속도 감소 */
			adj_pct = 100 - adj_pct; /* [한국어] 예: adj_pct=3이면 97% → 3% 감소 */
		else /* [한국어] busy_level 음수 = 장치 여유 → vrate를 adj_pct만큼 높여 더 많은 IO 허용 */
			adj_pct = 100 + adj_pct; /* [한국어] 예: adj_pct=3이면 103% → 3% 증가 */

		vrate = clamp(DIV64_U64_ROUND_UP(vrate * adj_pct, 100), /* [한국어] 올림 나눗셈으로 새 vrate 계산 후 [vrate_min, vrate_max] 범위 내로 클램프 */
			      vrate_min, vrate_max);
	}

	trace_iocost_ioc_vrate_adj(ioc, vrate, missed_ppm, rq_wait_pct,
				   nr_lagging, nr_shortages); /* [한국어] 조정 후 새 vrate, 미달 ppm, 큐 대기율 등을 트레이스에 기록 — ftrace/perf로 제어 동작 관찰 가능 */

	ioc->vtime_base_rate = vrate; /* [한국어] 계산된 새 기준 vrate를 ioc에 저장 — ioc_refresh_vrate()가 이 값을 보정의 기준으로 사용 */
	ioc_refresh_margins(ioc); /* [한국어] 새 vtime_base_rate 기반으로 min/low/target 마진 재계산 — cgroup 조절 임계값 갱신 */
}

/* take a snapshot of the current [v]time and vrate */
/*
 * [한국어]
 * ioc_now - 현재 wallclock 및 장치 가상 시간(vnow)의 일관된 스냅샷 획득
 *
 * @ioc: 현재 vtime 상태를 읽을 ioc 컨트롤러.
 *       읽는 필드: ioc->period_at(주기 시작 wallclock μs, seqcount 보호),
 *                  ioc->period_at_vtime(주기 시작 vtime, seqcount 보호),
 *                  atomic64 ioc->vtime_rate(현재 vtime 진행 속도).
 * @now: [출력] 채워질 스냅샷 구조체.
 *       now->now_ns: 현재 monotonic 시각(nanoseconds).
 *       now->now:    now_ns를 마이크로초로 변환한 값.
 *       now->vnow:   현재 장치 가상 시간 = period_at_vtime + (now - period_at) * vrate.
 * @return: 없음 (void). *now 구조체를 직접 채운다.
 *
 * iocost의 모든 vtime 계산은 "현재 장치 vtime이 얼마인가"라는 기준에서 출발한다.
 * 장치 vtime은 "주기 시작 vtime + (경과 wallclock) × vrate"로 정의된다.
 * period_at_vtime과 period_at은 ioc_start_period()에서 seqcount를 통해 쌍으로 갱신되므로,
 * 일관성 있는 스냅샷을 얻으려면 read_seqcount_begin/retry 루프가 필요하다.
 * vtime_rate는 atomic64로 관리되므로 단순 atomic64_read로 안전하게 읽을 수 있다.
 * 이 스냅샷은 iocg_activate(), iocg_kick_delay(), iocg_pay_debt(), ioc_timer_fn() 등
 * 대부분의 iocost 제어 경로에서 시간 기준점으로 사용된다.
 *
 * 실행 컨텍스트: ioc->lock 보유 여부와 무관하게 호출 가능.
 *   타이머 콜백(softirq), 제출 경로(process 컨텍스트), waitq 콜백 등 다양한 컨텍스트에서 사용.
 * 호출자: iocg_activate(), iocg_kick_delay(), ioc_timer_fn() 등 거의 모든 iocost 제어 경로.
 * 호출 대상: blk_time_get_ns(), ktime_to_us(), atomic64_read(),
 *            read_seqcount_begin(), read_seqcount_retry().
 *
 * 호출 체인:
 *   iocg_activate / ioc_timer_fn / waitq 경로 → [ioc_now] → seqcount 루프 →
 *   now->{now_ns, now, vnow} 채움
 */
static void ioc_now(struct ioc *ioc, struct ioc_now *now)
{
	unsigned seq; /* [한국어] seqcount 시작값 — read_seqcount_retry()가 이 값으로 기간 내 경쟁 쓰기 여부를 확인 */
	u64 vrate; /* [한국어] 현재 vtime 진행 속도 — atomic64_read로 제출 경로의 최신값을 읽음 */

	now->now_ns = blk_time_get_ns(); /* [한국어] 블록 계층 공용 monotonic 시계에서 현재 시각(ns) 읽기 — CLOCK_MONOTONIC 기반, 중단 없이 단조 증가 */
	now->now = ktime_to_us(now->now_ns); /* [한국어] ns를 μs로 변환 — iocost 내부 시간 단위가 μs이므로 pleft/period_at 계산에 직접 사용 가능 */
	vrate = atomic64_read(&ioc->vtime_rate); /* [한국어] atomic 읽기: ioc_refresh_vrate()가 갱신한 현재 vtime 속도 — 제출 경로와 타이머 경로가 공유 */

	/*
	 * The current vtime is
	 *
	 *   vtime at period start + (wallclock time since the start) * vrate
	 *
	 * As a consistent snapshot of `period_at_vtime` and `period_at` is
	 * needed, they're seqcount protected.
	 */
	do { /* [한국어] seqcount 재시도 루프 — ioc_start_period()가 period_at/period_at_vtime을 동시에 갱신할 수 있으므로 일관된 쌍을 읽을 때까지 반복 */
		seq = read_seqcount_begin(&ioc->period_seqcount); /* [한국어] seqcount 시작: 현재 시퀀스 번호를 읽어 저장 — 홀수이면 쓰기 중이므로 루프 재실행 */
		now->vnow = ioc->period_at_vtime + /* [한국어] 주기 시작 가상 시간에 경과 wallclock × vrate를 더해 현재 장치 vtime 계산 */
			(now->now - ioc->period_at) * vrate; /* [한국어] (현재 μs - 주기 시작 μs) * vrate = 이 주기에서 전진한 vtime */
	} while (read_seqcount_retry(&ioc->period_seqcount, seq)); /* [한국어] seqcount 재시도: seq가 변경되었으면(ioc_start_period가 쓴 경우) 처음부터 재시도 — 오래된 period_at을 사용한 vnow 계산 폐기 */
}

/*
 * [한국어]
 * ioc_start_period - 새 제어 주기를 시작하고 다음 타이머 만료를 예약
 *
 * @ioc: 주기를 시작할 ioc 컨트롤러.
 *       읽는 필드: ioc->running(IOC_RUNNING 상태 확인), ioc->period_us(타이머 간격 μs).
 *       쓰는 필드: ioc->period_at(새 주기의 wallclock 시작점, seqcount 보호),
 *                  ioc->period_at_vtime(새 주기의 vtime 시작점, seqcount 보호),
 *                  ioc->timer(다음 만료 시각).
 * @now: 현재 wallclock/vnow 스냅샷. 새 주기의 시작점을 기록하는 데 사용.
 * @return: 없음 (void). ioc->period_at, period_at_vtime, timer.expires를 갱신한다.
 *
 * iocost 제어 루프는 타이머 기반으로 동작한다. 이 함수는 매 타이머 만료 시
 * ioc_timer_fn()에서 호출되어 새 주기를 시작한다. 또한 첫 iocg가 활성화될 때
 * iocg_activate()에서도 호출되어 타이머를 최초로 시작한다.
 * period_at/period_at_vtime은 ioc_now()가 경쟁 없이 일관된 스냅샷을 읽을 수 있도록
 * write_seqcount_begin/end로 감싸 원자적으로 갱신된다.
 * add_timer()로 다음 만료를 예약하면 ioc_timer_fn()이 period_us μs 후 다시 호출된다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태.
 *   ioc_timer_fn(): 타이머 콜백(softirq) 컨텍스트.
 *   iocg_activate(): 첫 활성화 시 process 컨텍스트(spin_lock_irq 하).
 * 호출자: ioc_timer_fn(), iocg_activate().
 * 호출 대상: WARN_ON_ONCE(), write_seqcount_begin/end(), usecs_to_jiffies(), add_timer().
 *
 * 호출 체인:
 *   ioc_timer_fn / iocg_activate → [ioc_start_period] → add_timer(ioc->timer)
 */
static void ioc_start_period(struct ioc *ioc, struct ioc_now *now)
{
	WARN_ON_ONCE(ioc->running != IOC_RUNNING); /* [한국어] IOC_IDLE이나 IOC_STOP 상태에서 타이머가 시작되면 버그 — 실행 중 상태에서만 새 주기 시작 가능 */

	write_seqcount_begin(&ioc->period_seqcount); /* [한국어] seqcount 쓰기 시작: period_at/period_at_vtime을 동시에 갱신하는 임계 구간 진입 — ioc_now()의 읽기 루프가 재시도하도록 시퀀스 홀수로 만듦 */
	ioc->period_at = now->now; /* [한국어] 새 주기의 wallclock 시작점(μs) 기록 — ioc_now()가 경과 시간 계산에 사용 */
	ioc->period_at_vtime = now->vnow; /* [한국어] 새 주기의 vtime 시작점 기록 — ioc_now()가 vnow = period_at_vtime + 경과*vrate 계산에 사용 */
	write_seqcount_end(&ioc->period_seqcount); /* [한국어] seqcount 쓰기 완료: 시퀀스 짝수로 복원 — ioc_now()의 read_seqcount_retry가 성공 조건 충족 */

	ioc->timer.expires = jiffies + usecs_to_jiffies(ioc->period_us); /* [한국어] 다음 타이머 만료 시각 계산 — period_us μs 후에 ioc_timer_fn()이 다시 호출되도록 설정 */
	add_timer(&ioc->timer); /* [한국어] 커널 타이머 큐에 등록 — 만료 시 ioc_timer_fn()이 softirq 컨텍스트에서 실행되어 새 주기 피드백 루프를 처리 */
}

/*
 * Update @iocg's `active` and `inuse` to @active and @inuse, update level
 * weight sums and propagate upwards accordingly. If @save, the current margin
 * is saved to be used as reference for later inuse in-period adjustments.
 */
/*
 * [한국어]
 * __propagate_weights - iocg의 active/inuse 변경을 cgroup 계층 상위로 재귀적으로 전파
 *
 * @iocg: 가중치를 변경할 iocg. 이 노드의 active/inuse가 새 값으로 갱신되고,
 *        last_inuse/saved_margin이 저장된다. level/ancestors[] 정보를 통해 루트까지 전파.
 * @active: 이 iocg에 설정할 새 active 가중치. 활성화 여부와 무관한 명목 가중치.
 * @inuse: 이 iocg에 설정할 새 inuse 가중치. 실제 사용 비율을 반영. 리프 노드이면
 *         [1, active] 범위로 클램프되고, 내부 노드이면 자식 비율로 결정된다.
 * @save: true이면 현재 vtime margin(예산 여분)을 iocg->saved_margin에 저장.
 *        나중에 주기 내 inuse 재조정 시 기준점으로 사용된다.
 * @now: 현재 vnow 스냅샷. @save=true일 때 saved_margin 계산에 사용.
 * @return: 없음 (void). iocg 및 조상들의 active/inuse/child_active_sum/child_inuse_sum을 갱신.
 *          ioc->weights_updated를 true로 설정하여 commit_weights()가 hweight_gen을 증가시키도록 한다.
 *
 * iocost의 계층적 가중치 분배 메커니즘의 핵심. cgroup 트리에서 하위 노드의 가중치 변경이
 * 부모 노드의 child_active_sum/child_inuse_sum에 반영되어야 current_hweight()의 hweight 계산이
 * 올바르게 동작한다. 동작 순서:
 * 1) 대상 iocg가 내부 노드(자식을 통해 활성)이면 인수로 받은 inuse 대신 자식 비율에서 계산.
 *    리프 노드이면 inuse를 [1, active] 범위로 클램프.
 * 2) last_inuse를 이전 inuse로 저장 (이후 부채 상환 시 복원용).
 *    @save=true이면 현재 vnow와 vtime의 차이(예산 여분)를 saved_margin에 저장.
 * 3) active/inuse가 변경된 경우에만 루트 방향으로 반복 순회.
 *    각 레벨에서 부모의 child_active_sum/child_inuse_sum에 델타를 가산하고
 *    child->active/inuse를 갱신. 부모의 active/inuse도 자식 비율로 재계산.
 *    부모의 값이 변하지 않으면 조기 종료.
 * 4) ioc->weights_updated = true 설정 — commit_weights()가 smp_wmb + hweight_gen 증가.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태에서만 호출 (lockdep_assert_held로 확인).
 * 호출자: propagate_weights() (래퍼), iocg_incur_debt(), iocg_pay_debt(), weight_updated() 등.
 * 호출 대상: list_empty(), DIV64_U64_ROUND_UP(), min(), atomic64_read().
 *
 * 호출 체인:
 *   propagate_weights / weight_updated / iocg_incur_debt → [__propagate_weights] →
 *   iocg->active/inuse, parent->child_*_sum 갱신 → commit_weights() → hweight_gen 증가
 */
static void __propagate_weights(struct ioc_gq *iocg, u32 active, u32 inuse,
				bool save, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc; /* [한국어] iocg가 속한 ioc 컨트롤러 포인터 — lock 검증과 weights_updated 플래그 설정에 사용 */
	int lvl; /* [한국어] 루트 방향 순회 인덱스 — iocg->level-1(부모)부터 0(루트)까지 역방향 이동 */

	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — active/inuse 변경은 락 하에서만 원자적 */

	/*
	 * For an active leaf node, its inuse shouldn't be zero or exceed
	 * @active. An active internal node's inuse is solely determined by the
	 * inuse to active ratio of its children regardless of @inuse.
	 */
	if (list_empty(&iocg->active_list) && iocg->child_active_sum) { /* [한국어] active_list 비어 있으나 자식 합계가 있으면 내부 노드 — 직접 IO를 제출하지 않고 자식을 통해 활성 */
		inuse = DIV64_U64_ROUND_UP(active * iocg->child_inuse_sum, /* [한국어] 내부 노드의 inuse는 자식들의 inuse/active 비율로 결정 — 인수 @inuse는 무시 */
					   iocg->child_active_sum); /* [한국어] 올림 나눗셈: 자식 active 합 대비 자식 inuse 합의 비율에 이 노드의 active를 곱해 inuse 계산 */
	} else {
		/*
		 * It may be tempting to turn this into a clamp expression with
		 * a lower limit of 1 but active may be 0, which cannot be used
		 * as an upper limit in that situation. This expression allows
		 * active to clamp inuse unless it is 0, in which case inuse
		 * becomes 1.
		 */
		inuse = min(inuse, active) ?: 1; /* [한국어] 리프 노드: inuse는 active를 초과할 수 없음 — active=0이면 min()이 0을 반환하므로 ?: 1로 최솟값 보장 */
	}

	iocg->last_inuse = iocg->inuse; /* [한국어] 현재 inuse를 last_inuse에 보존 — 부채 상환 완료 후 iocg_pay_debt()가 이 값으로 inuse를 복원 */
	if (save) /* [한국어] @save=true이면 현재 vtime 예산 여분을 기준점으로 저장 — 주기 내 inuse 재조정 시 얼마나 앞서 있었는지 기억 */
		iocg->saved_margin = now->vnow - atomic64_read(&iocg->vtime); /* [한국어] 현재 장치 vtime - 이 iocg의 누적 issued vtime = 남은 예산 여분(양수: 아직 여유, 음수: 초과) */

	if (active == iocg->active && inuse == iocg->inuse) /* [한국어] 실제 값 변화가 없으면 부모 방향 전파 불필요 — 조기 반환으로 불필요한 순회 방지 */
		return;

	for (lvl = iocg->level - 1; lvl >= 0; lvl--) { /* [한국어] 이 iocg의 직속 부모(level-1)부터 루트(level 0)까지 역방향 순회 — 각 단계에서 부모의 합계 갱신 */
		struct ioc_gq *parent = iocg->ancestors[lvl]; /* [한국어] 현재 레벨의 조상 노드 — child_active_sum/child_inuse_sum을 갱신할 대상 */
		struct ioc_gq *child = iocg->ancestors[lvl + 1]; /* [한국어] 현재 레벨의 바로 아래 조상(= 이전 반복의 parent) — 갱신 전 이전 값(child->active/inuse)을 델타 계산에 사용 */
		u32 parent_active = 0, parent_inuse = 0; /* [한국어] 이번 반복에서 부모에 설정할 새 active/inuse — 자식 비율로 재계산되거나 0(비활성)으로 유지 */

		/* update the level sums */
		parent->child_active_sum += (s32)(active - child->active); /* [한국어] 이 자식의 active 델타를 부모의 합계에 반영 — (새 값 - 이전 값)의 부호 있는 차이를 더함 */
		parent->child_inuse_sum += (s32)(inuse - child->inuse); /* [한국어] 이 자식의 inuse 델타를 부모의 합계에 반영 — 형제 합계는 변하지 않으므로 델타 연산으로 충분 */
		/* apply the updates */
		child->active = active; /* [한국어] 이 레벨에서의 자식 노드 active를 새 값으로 확정 — 다음 반복에서 이 child가 새 차이 계산의 기준이 됨 */
		child->inuse = inuse; /* [한국어] 이 레벨에서의 자식 노드 inuse를 새 값으로 확정 */

		/*
		 * The delta between inuse and active sums indicates that
		 * much of weight is being given away.  Parent's inuse
		 * and active should reflect the ratio.
		 */
		if (parent->child_active_sum) { /* [한국어] 부모에 아직 활성 자식이 남아 있으면 부모의 active/inuse를 자식 비율로 재계산 */
			parent_active = parent->weight; /* [한국어] 부모의 active = 부모 자신의 설정 가중치(weight) */
			parent_inuse = DIV64_U64_ROUND_UP( /* [한국어] 부모의 inuse = 부모 weight × (자식 inuse 합 / 자식 active 합) — 자식들이 실제 사용하는 비율만큼만 부모 inuse 인정 */
				parent_active * parent->child_inuse_sum,
				parent->child_active_sum); /* [한국어] 올림 나눗셈으로 최소 1이 되도록 보장 */
		}
		/* else: parent_active/inuse는 0 유지 — 자식이 모두 비활성화된 경우 부모도 비활성 처리 */

		/* do we need to keep walking up? */
		if (parent_active == parent->active &&
		    parent_inuse == parent->inuse) /* [한국어] 부모의 active/inuse가 변하지 않으면 그 위 조상들도 변하지 않으므로 순회 조기 종료 */
			break; /* [한국어] 가중치 전파 완료 — 상위 노드는 이미 최신 상태 */

		active = parent_active; /* [한국어] 다음 반복에서 이 부모가 자식이 되므로 새 active 값을 전달 */
		inuse = parent_inuse; /* [한국어] 다음 반복에서 이 부모의 새 inuse 값을 전달 */
	}

	ioc->weights_updated = true; /* [한국어] 가중치 변경 완료 표시 — commit_weights()가 smp_wmb + atomic_inc(hweight_gen)으로 모든 CPU의 hweight 캐시를 무효화 */
}

/*
 * [한국어]
 * commit_weights - weights_updated 플래그를 확인하고 hweight_gen을 증가시켜 캐시 무효화
 *
 * @ioc: 가중치 변경을 커밋할 ioc 컨트롤러.
 *       읽고 쓰는 필드: ioc->weights_updated(변경 여부 플래그),
 *                       atomic ioc->hweight_gen(hweight 캐시 세대 카운터).
 * @return: 없음 (void). weights_updated가 true인 경우에만 hweight_gen을 원자적으로 증가.
 *
 * __propagate_weights()가 active/inuse 값을 변경하면 ioc->weights_updated가 true로 설정된다.
 * 이 함수는 그 변경을 "공표(commit)"하는 역할을 한다: smp_wmb()로 메모리 순서를 보장한 뒤
 * hweight_gen을 원자적으로 증가시킴으로써, 다른 CPU의 current_hweight()가 다음 호출 시
 * 캐시가 무효화되었음을 감지하고 hweight_active/hweight_inuse를 재계산하도록 유도한다.
 * smp_wmb()는 current_hweight()의 smp_rmb()와 쌍을 이루어 가중치 갱신 값이
 * hweight_gen 증가 이전에 관측 가능함을 보장한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (lockdep_assert_held로 확인).
 * 호출자: propagate_weights() — __propagate_weights() 직후 반드시 호출.
 * 호출 대상: smp_wmb(), atomic_inc().
 *
 * 호출 체인:
 *   propagate_weights → __propagate_weights → [commit_weights] → atomic_inc(hweight_gen)
 *   current_hweight() smp_rmb() ↔ [commit_weights] smp_wmb() (메모리 순서 보장 쌍)
 */
static void commit_weights(struct ioc *ioc)
{
	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — hweight_gen 증가는 락 하에서만 안전 */

	if (ioc->weights_updated) { /* [한국어] __propagate_weights()에서 설정된 변경 플래그 확인 — false이면 실제 변경 없으므로 캐시 무효화 불필요 */
		/* paired with rmb in current_hweight(), see there */
		/* [한국어] current_hweight()의 smp_rmb()와 쌍을 이루는 쓰기 메모리 장벽:
		 * hweight_gen 증가(아래 atomic_inc) 이전에 모든 weight 갱신 값이
		 * 다른 CPU에 관측 가능하도록 순서 보장 */
		smp_wmb(); /* [한국어] StoreStore 장벽: 이 장벽 이전의 stores(active/inuse 갱신)가 이 장벽 이후의 store(hweight_gen 증가) 이전에 관측됨을 보장 */
		atomic_inc(&ioc->hweight_gen); /* [한국어] atomic: 세대 카운터 1 증가 — 이후 current_hweight() 호출 시 ioc_gen != iocg->hweight_gen이 되어 캐시 재계산 유도 */
		ioc->weights_updated = false; /* [한국어] 변경 플래그 클리어 — 이미 커밋된 변경을 중복으로 처리하지 않도록 */
	}
}

/*
 * [한국어]
 * propagate_weights - __propagate_weights()와 commit_weights()를 묶은 공개 래퍼
 *
 * @iocg: 가중치를 변경할 iocg. __propagate_weights()에 그대로 전달.
 * @active: 새 active 가중치. __propagate_weights()에 전달.
 * @inuse: 새 inuse 가중치. __propagate_weights()에 전달.
 * @save: true이면 현재 vtime margin을 saved_margin에 저장. __propagate_weights()에 전달.
 * @now: 현재 시간 스냅샷. __propagate_weights()에 전달.
 * @return: 없음 (void).
 *
 * iocost 코드 대부분은 __propagate_weights()를 직접 호출하지 않고 이 래퍼를 사용한다.
 * __propagate_weights()는 ioc->weights_updated만 설정하고 hweight_gen을 직접 증가시키지
 * 않는다. hweight_gen 증가(commit_weights)는 여러 propagate 호출을 배치로 처리한 뒤
 * 한 번만 수행할 수도 있다. 이 래퍼는 매 호출 후 즉시 commit하는 일반적인 패턴을 제공한다.
 * 여러 iocg의 가중치를 한꺼번에 바꾸는 특수 경로(ioc_timer_fn 일부)에서는
 * __propagate_weights()를 직접 반복 호출한 뒤 마지막에 commit_weights()를 한 번 호출한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (__propagate_weights/commit_weights의 lockdep 검사 확인).
 * 호출자: weight_updated(), iocg_activate(), iocg_incur_debt(), iocg_pay_debt() 등.
 * 호출 대상: __propagate_weights(), commit_weights().
 *
 * 호출 체인:
 *   weight_updated / iocg_activate / ... → [propagate_weights] →
 *   __propagate_weights → commit_weights → atomic_inc(hweight_gen)
 */
static void propagate_weights(struct ioc_gq *iocg, u32 active, u32 inuse,
			      bool save, struct ioc_now *now)
{
	__propagate_weights(iocg, active, inuse, save, now); /* [한국어] 계층 순회하며 active/inuse 갱신 및 weights_updated = true 설정 */
	commit_weights(iocg->ioc); /* [한국어] smp_wmb + hweight_gen 증가로 모든 CPU의 current_hweight() 캐시 무효화 */
}

/*
 * [한국어]
 * current_hweight - 이 iocg의 계층적 가중치(hweight_active/hweight_inuse)를 세대 기반 캐시로 읽기
 *
 * @iocg: 계층적 가중치를 조회할 iocg.
 *        읽는 필드: iocg->hweight_gen(캐시 세대), iocg->hweight_active/hweight_inuse(캐시값),
 *                   iocg->level, iocg->ancestors[](계층 경로).
 *        쓰는 필드: 캐시 미스 시 iocg->hweight_active/hweight_inuse/hweight_gen 갱신.
 * @hw_activep: [출력, 선택] hweight_active를 저장할 포인터. NULL이면 무시.
 * @hw_inusep:  [출력, 선택] hweight_inuse를 저장할 포인터. NULL이면 무시.
 * @return: 없음 (void). *hw_activep와 *hw_inusep에 결과를 저장.
 *
 * hweight(계층적 가중치)는 이 iocg가 장치 전체 IO 용량 중 몇 분의 몇을 차지하는지를
 * WEIGHT_ONE(65536)을 기준으로 나타낸다. hweight_active는 활성 가중치 비율,
 * hweight_inuse는 실제 사용 가중치 비율이다. IO 비용 계산 시
 * cost = abs_cost * WEIGHT_ONE / hweight_inuse 공식에 사용된다.
 *
 * 성능을 위해 세대 번호(hweight_gen) 기반 캐시를 사용한다:
 * 1) ioc->hweight_gen == iocg->hweight_gen이면 캐시 히트 → 저장된 값 반환.
 * 2) 캐시 미스 시 smp_rmb() 후 루트에서 이 iocg까지 계층을 순회하며
 *    hwa/hwi = WEIGHT_ONE에서 시작해 각 단계의 active/inuse 비율을 곱해 누적.
 * 3) 순회 중 비활성화 경쟁이 발생하면 READ_ONCE로 읽어 torn read를 방지하고,
 *    sum이 0인 레벨은 건너뜀. sum보다 child 값이 크면 max로 보정.
 * 4) 결과를 iocg->hweight_*에 저장하고 hweight_gen을 현재 세대로 동기화.
 *
 * 실행 컨텍스트: ioc->lock 보유 여부 무관 (락 없이도 호출 가능한 세대 캐시 패턴).
 *   경쟁 중 잘못된 hweight를 계산해도 이후 세대가 바뀌면 재계산되므로 안전.
 * 호출자: iocg_kick_delay(), iocg_wake_fn(), ioc_timer_fn() 등 비용 계산이 필요한 모든 경로.
 * 호출 대상: atomic_read(), smp_rmb(), READ_ONCE(), max_t(), div64_u64().
 *
 * 호출 체인:
 *   iocg_kick_delay / iocg_wake_fn / ioc_timer_fn → [current_hweight] →
 *   hweight_active/hweight_inuse (캐시 히트 또는 루트→리프 순회 재계산)
 */
static void current_hweight(struct ioc_gq *iocg, u32 *hw_activep, u32 *hw_inusep)
{
	struct ioc *ioc = iocg->ioc; /* [한국어] iocg가 속한 ioc 컨트롤러 — hweight_gen 읽기와 락 검증에 사용 */
	int lvl; /* [한국어] 루트에서 이 iocg까지의 계층 순회 인덱스 (0: 루트, iocg->level-1: 직속 부모) */
	u32 hwa, hwi; /* [한국어] hwa: 누적 active 비율, hwi: 누적 inuse 비율 — 루트에서 WEIGHT_ONE 시작 후 각 레벨 비율 곱산 */
	int ioc_gen; /* [한국어] 현재 ioc의 hweight_gen 세대 번호 — 캐시 유효성 비교 및 갱신 후 동기화에 사용 */

	/* hot path - if uptodate, use cached */
	ioc_gen = atomic_read(&ioc->hweight_gen); /* [한국어] atomic 읽기: commit_weights()가 마지막으로 설정한 세대 번호 */
	if (ioc_gen == iocg->hweight_gen) /* [한국어] 캐시 히트: 이 iocg의 캐시가 현재 세대와 일치하면 재계산 불필요 */
		goto out; /* [한국어] 저장된 hweight_active/hweight_inuse를 바로 반환 — hot path 성능 최적화 */

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
	smp_rmb(); /* [한국어] LoadLoad 장벽: commit_weights()의 smp_wmb()와 쌍. hweight_gen을 읽은 후 이 장벽이 실행되면, 그 이전에 __propagate_weights()가 쓴 active/inuse 값들이 모두 이 CPU에 보임을 보장 */

	hwa = hwi = WEIGHT_ONE; /* [한국어] 루트에서 WEIGHT_ONE(65536 = 100%)으로 시작 — 각 레벨에서 이 비율을 자식/합 비율로 줄여나감 */
	for (lvl = 0; lvl <= iocg->level - 1; lvl++) { /* [한국어] 루트(0)에서 이 iocg의 직속 부모(level-1)까지 순방향 순회 — 각 단계에서 자식 비율을 hwa/hwi에 누적 */
		struct ioc_gq *parent = iocg->ancestors[lvl]; /* [한국어] 현재 레벨의 조상 노드 — child_active_sum/child_inuse_sum 비율 계산에 사용 */
		struct ioc_gq *child = iocg->ancestors[lvl + 1]; /* [한국어] 이 iocg 방향으로 한 단계 아래의 조상 — 이 자식이 부모 합계 중 얼마를 차지하는지 계산 */
		u64 active_sum = READ_ONCE(parent->child_active_sum); /* [한국어] READ_ONCE: 비활성화 경쟁 중 torn read 방지 — 컴파일러가 여러 번 읽지 않도록 보장 */
		u64 inuse_sum = READ_ONCE(parent->child_inuse_sum); /* [한국어] READ_ONCE: inuse_sum도 동일하게 atomic-like로 읽기 */
		u32 active = READ_ONCE(child->active); /* [한국어] READ_ONCE: 이 자식의 현재 active 가중치 — __propagate_weights()와 경쟁 가능 */
		u32 inuse = READ_ONCE(child->inuse); /* [한국어] READ_ONCE: 이 자식의 현재 inuse 가중치 */

		/* we can race with deactivations and either may read as zero */
		if (!active_sum || !inuse_sum) /* [한국어] 비활성화 경쟁으로 합계가 0이 된 경우 이 레벨 건너뜀 — 0으로 나누기 방지 및 잘못된 비율 계산 방지 */
			continue; /* [한국어] 이 레벨의 비율 계산 스킵, hwa/hwi 유지 */

		active_sum = max_t(u64, active, active_sum); /* [한국어] active가 active_sum보다 크면 active_sum으로 보정 — 경쟁 중 자식 값이 합계보다 일시적으로 커질 수 있어 hwa가 1을 초과하지 않도록 방어 */
		hwa = div64_u64((u64)hwa * active, active_sum); /* [한국어] hwa = hwa × (이 자식의 active) / (부모의 자식 active 합) — 루트에서 여기까지의 active 비율 누적 */

		inuse_sum = max_t(u64, inuse, inuse_sum); /* [한국어] inuse가 inuse_sum보다 크면 inuse_sum으로 보정 — 동일한 방어 로직 */
		hwi = div64_u64((u64)hwi * inuse, inuse_sum); /* [한국어] hwi = hwi × (이 자식의 inuse) / (부모의 자식 inuse 합) — 루트에서 여기까지의 inuse 비율 누적 */
	}

	iocg->hweight_active = max_t(u32, hwa, 1); /* [한국어] 계산된 active 비율 캐시에 저장 — 최솟값 1로 클램프해 0이 되어 비용 계산에서 0 나눗셈이 발생하지 않도록 */
	iocg->hweight_inuse = max_t(u32, hwi, 1); /* [한국어] 계산된 inuse 비율 캐시에 저장 — cost = abs_cost * WEIGHT_ONE / hweight_inuse 공식에서 분모가 0이 되는 것 방지 */
	iocg->hweight_gen = ioc_gen; /* [한국어] 계산에 사용한 세대 번호를 iocg에 동기화 — 다음 호출에서 캐시 히트 조건(ioc_gen == iocg->hweight_gen) 충족 */
out:
	if (hw_activep) /* [한국어] 호출자가 active 비율을 원하면 포인터를 통해 반환 */
		*hw_activep = iocg->hweight_active; /* [한국어] 캐시된 hweight_active 값을 호출자 버퍼에 복사 */
	if (hw_inusep) /* [한국어] 호출자가 inuse 비율을 원하면 포인터를 통해 반환 */
		*hw_inusep = iocg->hweight_inuse; /* [한국어] 캐시된 hweight_inuse 값을 호출자 버퍼에 복사 — IO 비용 계산의 핵심 입력 */
}

/*
 * Calculate the hweight_inuse @iocg would get with max @inuse assuming all the
 * other weights stay unchanged.
 */
/*
 * [한국어]
 * current_hweight_max - 이 iocg의 inuse를 최대(active)로 설정할 때 얻을 수 있는
 *                        최대 hweight_inuse 계산 (다른 iocg 가중치는 불변으로 가정)
 *
 * @iocg: 최대 hweight를 계산할 iocg. 현재 active와 계층 경로(ancestors[])를 읽는다.
 *        ioc->lock 보유 상태에서 호출해야 한다.
 * @return: 이 iocg가 inuse = active로 설정되었을 때 얻을 수 있는 최대 hweight_inuse.
 *          WEIGHT_ONE(65536 = 100%) 기준. 최솟값은 1.
 *
 * iocost는 부채 상환 후 inuse를 복원하거나, cgroup에 가능한 최대 예산을 추정할 때
 * "이 cgroup이 자신의 share를 최대한 사용하면 hweight_inuse가 얼마나 될까"를 알아야 한다.
 * 이 함수는 현재 다른 cgroup들의 inuse 합계(child_inuse_sum)를 바꾸지 않고,
 * 이 iocg의 inuse만 active로 가정하여 가상의 hweight_inuse를 계산한다.
 * 계산 방식: 이 iocg의 직속 부모부터 루트까지 역방향 순회하며,
 * - 이 자식의 inuse가 active로 변경된 경우의 새 child_inuse_sum을 계산
 * - hwm에 현 레벨 비율(inuse / child_inuse_sum)을 곱산
 * - 다음 레벨로 올라갈 때 부모의 새 inuse를 계산
 * current_hweight()와 달리 캐시를 사용하지 않는 단순 계산 함수이다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (lockdep_assert_held로 확인).
 * 호출자: ioc_timer_fn() — 부채/지연 해소 후 최대 허용 inuse 추정 시.
 * 호출 대상: div64_u64(), DIV64_U64_ROUND_UP(), max_t().
 *
 * 호출 체인:
 *   ioc_timer_fn → [current_hweight_max] → div64_u64 (루트까지 역순 비율 곱산)
 */
static u32 current_hweight_max(struct ioc_gq *iocg)
{
	u32 hwm = WEIGHT_ONE; /* [한국어] 최대 hweight 누적 변수 — WEIGHT_ONE(65536, 100%)에서 시작해 각 레벨 비율을 곱해 줄어듦 */
	u32 inuse = iocg->active; /* [한국어] 이 iocg의 최대 inuse 가정값 = active (자신의 share를 100% 사용하는 시나리오) */
	u64 child_inuse_sum; /* [한국어] 이 자식의 inuse를 active로 변경한 가상의 부모 child_inuse_sum — 현재 합계에서 이전 inuse를 빼고 새 값을 더해 계산 */
	int lvl; /* [한국어] 직속 부모(level-1)부터 루트(0)까지 역방향 순회 인덱스 */

	lockdep_assert_held(&iocg->ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — child_inuse_sum 읽기는 락 하에서만 일관성 보장 */

	for (lvl = iocg->level - 1; lvl >= 0; lvl--) { /* [한국어] 이 iocg의 직속 부모부터 루트까지 역방향으로 계층 순회 */
		struct ioc_gq *parent = iocg->ancestors[lvl]; /* [한국어] 현재 레벨의 조상 노드 — child_inuse_sum 읽기 대상 */
		struct ioc_gq *child = iocg->ancestors[lvl + 1]; /* [한국어] 이 방향의 자식 노드 — 이전 inuse 값(child->inuse)을 델타 계산에 사용 */

		child_inuse_sum = parent->child_inuse_sum + inuse - child->inuse; /* [한국어] 이 자식의 inuse를 새 값(inuse)으로 교체했을 때의 가상 부모 합계 — 실제 parent->child_inuse_sum은 변경 안 함 */
		hwm = div64_u64((u64)hwm * inuse, child_inuse_sum); /* [한국어] 이 레벨에서의 비율(inuse / 새 합계)을 hwm에 누적 — hwm이 점점 작아져 최종값이 이 iocg의 최대 hweight_inuse */
		inuse = DIV64_U64_ROUND_UP(parent->active * child_inuse_sum, /* [한국어] 부모가 이 새 비율로 자식들을 배분한다면 부모 자신의 inuse는 얼마인지 계산 */
					   parent->child_active_sum); /* [한국어] 다음 반복(더 위 레벨)에서 이 값이 자식 inuse 역할을 함 */
	}

	return max_t(u32, hwm, 1); /* [한국어] 계산된 최대 hweight_inuse 반환 — 최솟값 1 보장으로 0 반환 방지 */
}

/*
 * [한국어]
 * weight_updated - cgroup 가중치 설정 변경을 iocg에 적용하고 계층으로 전파
 *
 * @iocg: 가중치 변경을 적용할 iocg. cfg_weight(사용자 설정 가중치) 또는
 *        부모 blkcg의 dfl_weight(기본 가중치)에서 새 weight를 결정한다.
 * @now: 현재 시간 스냅샷. propagate_weights()의 @save 경로에서 saved_margin 계산에 사용.
 * @return: 없음 (void). iocg->weight와 계층의 active/inuse 합계를 갱신한다.
 *
 * 사용자가 cgroup의 가중치를 변경하거나, 이 cgroup이 처음 iocost에 등록될 때
 * 이 함수가 호출된다. 동작 순서:
 * 1) iocg->cfg_weight(직접 설정값)가 있으면 사용, 없으면 blkcg의 기본값(dfl_weight) 사용.
 * 2) 새 weight가 현재 iocg->weight와 다르고, 이 iocg가 활성 상태이면
 *    propagate_weights()로 계층에 변경 전파 (inuse는 현재 값 유지, save=true).
 * 3) iocg->weight를 새 값으로 갱신.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (lockdep_assert_held로 확인).
 * 호출자: iocg가 활성화될 때, 또는 사용자가 cgroup.weight를 변경할 때.
 * 호출 대상: iocg_to_blkg(), blkcg_to_iocc(), propagate_weights().
 *
 * 호출 체인:
 *   weight 변경 통보 경로 → [weight_updated] → propagate_weights →
 *   __propagate_weights, commit_weights
 */
static void weight_updated(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc; /* [한국어] iocg가 속한 ioc 컨트롤러 포인터 — lock 검증에 사용 */
	struct blkcg_gq *blkg = iocg_to_blkg(iocg); /* [한국어] iocg에서 상위 blkg 포인터 획득 — blkcg(cgroup 정책 데이터)에 접근하기 위한 경로 */
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkg->blkcg); /* [한국어] blkcg에서 ioc_cgrp 포인터 획득 — dfl_weight(기본 가중치) 참조용 */
	u32 weight; /* [한국어] 이 iocg에 적용할 최종 가중치 — cfg_weight 우선, 없으면 dfl_weight */

	lockdep_assert_held(&ioc->lock); /* [한국어] ioc->lock 없이 호출 시 lockdep 경고 — weight 변경과 계층 전파는 락 하에서만 원자적 */

	weight = iocg->cfg_weight ?: iocc->dfl_weight; /* [한국어] 이 iocg에 명시적 가중치가 설정되었으면 사용, 없으면(0) 부모 blkcg의 기본 가중치 상속 */
	if (weight != iocg->weight && iocg->active) /* [한국어] 실제 가중치 변화가 있고, 이 iocg가 현재 활성 상태인 경우에만 계층 전파 필요 — 비활성 시 다음 활성화 때 적용됨 */
		propagate_weights(iocg, weight, iocg->inuse, true, now); /* [한국어] 새 active(=weight)로 계층 갱신, inuse는 현재 값 유지, save=true로 현재 margin 저장 */
	iocg->weight = weight; /* [한국어] iocg->weight를 새 값으로 확정 — 다음 propagate_weights에서 parent->weight로 읽힘 */
}

/*
 * [한국어]
 * iocg_activate - iocg를 active 목록에 등록하고 초기 vtime 예산을 설정
 *
 * @iocg: 활성화할 iocg. active_list, vtime, done_vtime, hweight_gen,
 *        active_period, activated_at 등을 갱신한다.
 * @now: [입출력] 현재 시간 스냅샷. 이미 활성 중이면 ioc_now()로 갱신하여 반환.
 *       첫 활성화 시 vtarget, ioc_start_period() 등에 사용.
 * @return: true — 활성화 성공(이미 활성이었거나 새로 활성화됨).
 *          false — 활성화 실패(내부 노드에 직접 IO, 조상 리프 충돌, 자식 충돌).
 *
 * bio가 처음 제출되거나 비활성 후 재활성화될 때 호출되어 이 iocg를 ioc의
 * active_iocgs 목록에 등록한다. 동작 순서:
 * 1) 락 없이 active_list 비어 있는지 확인 (race 가능하지만 무방). 이미 활성이면
 *    ioc_now()로 시간을 갱신하고 active_period만 업데이트 후 true 반환.
 * 2) 내부 노드(자식 있는 노드)에서 직접 IO 시도 시 false 반환 (락 없는 빠른 경로).
 * 3) ioc->lock 획득 후 재확인: 조상 중 active 리프가 있거나, 자식 활성이 있으면 실패.
 * 4) 초기 예산: vtime/done_vtime을 (vnow - target_margin)으로 설정.
 *    이렇게 하면 target_margin만큼의 vtime 예산이 즉시 주어지며,
 *    비활성화 시 target_margin을 초과하는 vtime은 버려진다.
 * 5) hweight_gen을 현재보다 1 낮게 설정해 강제 캐시 미스 → 즉시 hweight 재계산.
 * 6) active_list에 등록 후 propagate_weights()로 계층 가중치 갱신.
 * 7) 타이머가 아직 시작되지 않았으면(IOC_IDLE) ioc_start_period()로 첫 주기 시작.
 *
 * 실행 컨텍스트: 처음에는 락 없이 빠른 경로 확인. 이후 spin_lock_irq(&ioc->lock)으로
 *   진입. process 컨텍스트에서 bio 제출 경로(ioc_rq_qos_throttle)에 의해 호출됨.
 * 호출자: ioc_rq_qos_throttle() — bio 제출 시 iocg가 비활성이면 활성화 시도.
 * 호출 대상: ioc_now(), atomic64_read/set/add(), list_empty(), list_add(),
 *            propagate_weights(), TRACE_IOCG_PATH(), ioc_start_period().
 *
 * 호출 체인:
 *   ioc_rq_qos_throttle → [iocg_activate] →
 *   propagate_weights → ioc_start_period (IOC_IDLE 시)
 */
static bool iocg_activate(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc; /* [한국어] iocg가 속한 ioc 컨트롤러 포인터 — lock 획득, cur_period, running 상태 등에 접근 */
	u64 __maybe_unused last_period, cur_period; /* [한국어] last_period: 마지막 활성 주기(트레이스용), cur_period: 현재 제어 주기 번호 */
	u64 vtime, vtarget; /* [한국어] vtime: 현재 iocg의 누적 issued vtime, vtarget: 새 초기 예산 기준점 */
	int i; /* [한국어] 조상 리프 검사 루프 인덱스 */

	/*
	 * If seem to be already active, just update the stamp to tell the
	 * timer that we're still active.  We don't mind occassional races.
	 */
	if (!list_empty(&iocg->active_list)) { /* [한국어] 락 없이 active_list 확인 — race 가능하지만 성능을 위해 허용. 활성 중이면 빠른 경로로 처리 */
		ioc_now(ioc, now); /* [한국어] 현재 시간 스냅샷 갱신 — 호출자가 이후 @now를 사용할 때 최신 vnow 필요 */
		cur_period = atomic64_read(&ioc->cur_period); /* [한국어] atomic 읽기: 현재 제어 주기 번호 확인 — 주기가 바뀌었는지 체크 */
		if (atomic64_read(&iocg->active_period) != cur_period) /* [한국어] atomic 읽기: 이 iocg의 마지막 활성 주기가 현재 주기와 다르면 스탬프 갱신 필요 */
			atomic64_set(&iocg->active_period, cur_period); /* [한국어] atomic 쓰기: 이번 주기에 활성임을 ioc_timer_fn()에 알림 — 갱신 없으면 타이머가 idle로 판단해 비활성화 */
		return true; /* [한국어] 이미 활성 상태이므로 추가 처리 불필요 */
	}

	/* racy check on internal node IOs, treat as root level IOs */
	/* [한국어] 락 없는 빠른 검사: 자식이 이미 활성이면 이 iocg(내부 노드)를 리프로 활성화할 수 없음 */
	if (iocg->child_active_sum) /* [한국어] iocost는 리프 노드만 IO 예산을 직접 갖고, 내부 노드의 가중치는 자식 비율에서 결정됨 */
		return false; /* [한국어] 내부 노드에 직접 IO 시도 → 루트 레벨 IO로 처리하라는 주석(위)에 따라 실패 반환 */

	spin_lock_irq(&ioc->lock); /* [한국어] IRQ를 비활성화하고 ioc->lock 획득 — active_list 추가, propagate_weights, ioc_start_period 등을 보호 */

	ioc_now(ioc, now); /* [한국어] 락 취득 후 현재 시간 스냅샷 다시 읽기 — vtarget 계산과 ioc_start_period()에 사용 */

	/* update period */
	cur_period = atomic64_read(&ioc->cur_period); /* [한국어] atomic 읽기: 락 하에서 현재 주기 재확인 */
	last_period = atomic64_read(&iocg->active_period); /* [한국어] atomic 읽기: 이전에 활성이었던 주기 번호 — 트레이스 이벤트에 기록 */
	atomic64_set(&iocg->active_period, cur_period); /* [한국어] atomic 쓰기: 이번 주기에 활성화됨을 기록 */

	/* already activated or breaking leaf-only constraint? */
	if (!list_empty(&iocg->active_list)) /* [한국어] 락 취득 전에 다른 CPU가 먼저 활성화한 경우 — 중복 처리 방지를 위해 바로 성공 경로로 이동 */
		goto succeed_unlock;
	for (i = iocg->level - 1; i > 0; i--) /* [한국어] 조상 리프 검사: 이 iocg의 직속 부모부터 루트 직하까지 순회 (level 0은 루트 자체이므로 > 0) */
		if (!list_empty(&iocg->ancestors[i]->active_list)) /* [한국어] 조상 중 하나가 리프로 활성이면 이 iocg의 활성화는 계층 제약 위반 */
			goto fail_unlock; /* [한국어] 활성화 실패 — 조상이 이미 리프로 동작 중이므로 이 iocg는 비활성 유지 */

	if (iocg->child_active_sum) /* [한국어] 락 획득 사이에 자식이 활성화된 경우 — 내부 노드를 리프로 활성화하면 두 노드가 동시에 예산을 가지는 불일치 발생 */
		goto fail_unlock; /* [한국어] 자식 활성화가 감지되면 이 iocg의 활성화를 포기 */

	/*
	 * Always start with the target budget. On deactivation, we throw away
	 * anything above it.
	 */
	vtarget = now->vnow - ioc->margins.target; /* [한국어] 초기 예산 기준점 = 현재 장치 vtime - target 마진 — 이 iocg의 vtime을 이 값으로 설정해 target_margin만큼의 예산 선불 지급 */
	vtime = atomic64_read(&iocg->vtime); /* [한국어] atomic 읽기: 이 iocg의 현재 누적 issued vtime — vtarget과의 차이를 계산해 조정량 결정 */

	atomic64_add(vtarget - vtime, &iocg->vtime); /* [한국어] atomic: vtime을 vtarget으로 조정 — (vtarget - vtime)이 음수면 뒤로(예산 감소), 양수면 앞으로(예산 증가) */
	atomic64_add(vtarget - vtime, &iocg->done_vtime); /* [한국어] atomic: done_vtime도 같은 폭으로 조정 — vtime과 done_vtime의 차이(in-flight 비용)가 변하지 않도록 유지 */
	vtime = vtarget; /* [한국어] 지역 변수 vtime도 vtarget으로 갱신 — 이후 TRACE_IOCG_PATH에 전달 */

	/*
	 * Activate, propagate weight and start period timer if not
	 * running.  Reset hweight_gen to avoid accidental match from
	 * wrapping.
	 */
	iocg->hweight_gen = atomic_read(&ioc->hweight_gen) - 1; /* [한국어] atomic 읽기 후 1 감산: hweight_gen을 현재 세대보다 낮게 설정해 current_hweight()에서 반드시 캐시 미스가 발생하도록 강제 */
	list_add(&iocg->active_list, &ioc->active_iocgs); /* [한국어] ioc->active_iocgs 목록의 앞에 추가 — ioc_timer_fn()이 이 목록을 순회하며 활성 iocg들을 처리 */

	propagate_weights(iocg, iocg->weight, /* [한국어] 새 active = iocg->weight (자신의 설정 가중치) */
			  iocg->last_inuse ?: iocg->weight, true, now); /* [한국어] 이전 비활성화 시 저장된 last_inuse로 복원 (없으면 weight 사용), save=true로 현재 margin 기록 */

	TRACE_IOCG_PATH(iocg_activate, iocg, now,
			last_period, cur_period, vtime); /* [한국어] 활성화 이벤트 트레이스 — 이전/현재 주기, 초기 vtime을 기록 */

	iocg->activated_at = now->now; /* [한국어] 활성화 시각 기록 — ioc_timer_fn()에서 활성화 이후 경과 시간 측정에 사용 */

	if (ioc->running == IOC_IDLE) { /* [한국어] 이전에 모든 iocg가 비활성이어서 타이머가 정지된 상태 — 첫 IO 발생이므로 타이머를 시작해야 함 */
		ioc->running = IOC_RUNNING; /* [한국어] ioc 상태를 IOC_RUNNING으로 전이 — 이후 ioc_timer_fn()이 매 주기 호출됨 */
		ioc->dfgv_period_at = now->now; /* [한국어] dfgv(demand-fairness grace vtime) 측정 시작점 기록 — 장치 idle/부채 계산의 기준 시각 */
		ioc->dfgv_period_rem = 0; /* [한국어] dfgv 주기 나머지 초기화 — 새 측정 사이클 시작 */
		ioc_start_period(ioc, now); /* [한국어] 첫 iocost 제어 주기 시작 및 타이머 등록 */
	}

succeed_unlock:
	spin_unlock_irq(&ioc->lock); /* [한국어] ioc->lock 해제 및 IRQ 복원 — 활성화 성공 경로 */
	return true; /* [한국어] 활성화 성공(새로 활성화되었거나 이미 활성이었음) */

fail_unlock:
	spin_unlock_irq(&ioc->lock); /* [한국어] ioc->lock 해제 및 IRQ 복원 — 활성화 실패 경로 */
	return false; /* [한국어] 활성화 실패 — 계층 제약 위반 또는 자식 충돌 */
}

/*
 * [한국어]
 * iocg_kick_delay - vtime 부채/초과분에 따른 blkcg use_delay 갱신 및 적용
 *
 * @iocg: 지연을 갱신할 iocg.
 *        읽는 필드: iocg->delay(현재 적용 중인 지연 μs), iocg->delay_at(지연 설정 시각),
 *                   iocg->abs_vdebt(부채 abs_cost), iocg->vtime(atomic, issued vtime),
 *                   iocg->indelay_since(지연 누적 통계용 시작 시각).
 *        쓰는 필드: iocg->delay, iocg->delay_at, iocg->indelay_since, iocg->stat.indelay_us.
 * @now: 현재 시간 스냅샷(now->now μs, now->vnow 장치 vtime). 지연 감쇠 및 vover 계산에 사용.
 * @return: true — 유의미한 지연이 설정되어 blkcg_set_delay()로 IO가 억제됨.
 *          false — 지연이 충분히 소멸되어 blkcg_clear_delay()로 억제 해제됨.
 *
 * iocg의 abs_vdebt(부채) 또는 vtime 초과분이 있으면 blkcg use_delay 메커니즘을 통해
 * 이 cgroup의 후속 bio 제출을 인위적으로 지연시킨다. 동작 순서:
 * 1) 다른 CPU가 더 최근에 지연을 설정했으면 감쇠 계산에서 음수 발생 위험 → 즉시 반환.
 * 2) 지수 감쇠: 현재 지연 값을 (경과 초 만큼 오른쪽 시프트) — 1초마다 절반 감소.
 * 3) 새 지연 계산: 현재 vtime + 부채 vtime - vnow = vover (초과분).
 *    vover_pct(한 주기 기준 초과 비율)에 따라 선형 보간으로 new_delay 계산.
 *    MIN_DELAY_THR_PCT 이하이면 0, MAX_DELAY_THR_PCT 이상이면 MAX_DELAY.
 * 4) 현재 감쇠된 delay와 new_delay 중 큰 값을 적용 — 최근에 지연이 필요하면 유지.
 * 5) delay >= MIN_DELAY이면 blkcg_set_delay()로 ns 단위 지연 설정 → true 반환.
 *    아니면 blkcg_clear_delay()로 지연 해제 → false 반환.
 *
 * 실행 컨텍스트: iocg->waitq.lock 보유 상태 (lockdep_assert_held로 확인).
 *   ioc_timer_fn()과 iocg_kick_waitq()에서 호출됨 (타이머/process 컨텍스트).
 * 호출자: ioc_timer_fn(), iocg_kick_waitq() — 매 주기 또는 waitq 처리 시 지연 갱신.
 * 호출 대상: time_before64(), div64_u64(), current_hweight(), atomic64_read(),
 *            abs_cost_to_cost(), div64_s64(), div_u64(), blkcg_set_delay(),
 *            blkcg_clear_delay().
 *
 * 호출 체인:
 *   ioc_timer_fn / iocg_kick_waitq → [iocg_kick_delay] →
 *   current_hweight, blkcg_set_delay/clear_delay
 */
static bool iocg_kick_delay(struct ioc_gq *iocg, struct ioc_now *now)
{
	struct ioc *ioc = iocg->ioc; /* [한국어] iocg가 속한 ioc 컨트롤러 — period_us, vtime_base_rate 등 지연 계산에 필요한 파라미터 참조 */
	struct blkcg_gq *blkg = iocg_to_blkg(iocg); /* [한국어] iocg에서 blkg 획득 — blkcg_set_delay/clear_delay 호출에 필요 */
	u64 tdelta, delay, new_delay, shift; /* [한국어] tdelta: 지연 경과 시간, delay: 감쇠 후 현재 지연, new_delay: 새로 계산된 지연, shift: 감쇠 횟수(초 단위) */
	s64 vover, vover_pct; /* [한국어] vover: vtime 초과분(음수면 예산 여유), vover_pct: 한 주기 기준 초과 비율(%) */
	u32 hwa; /* [한국어] current_hweight()로 얻은 이 iocg의 hweight_active — abs_vdebt를 vtime 단위로 환산하기 위해 사용 */

	lockdep_assert_held(&iocg->waitq.lock); /* [한국어] iocg->waitq.lock 없이 호출 시 lockdep 경고 — delay/delay_at 갱신은 waitq.lock 하에서만 안전 */

	/*
	 * If the delay is set by another CPU, we may be in the past. No need to
	 * change anything if so. This avoids decay calculation underflow.
	 */
	if (time_before64(now->now, iocg->delay_at)) /* [한국어] now->now가 delay_at보다 이전이면 다른 CPU가 더 최근에 지연을 설정한 것 — 이 경우 감쇠 계산이 언더플로우 가능하므로 건너뜀 */
		return false; /* [한국어] 다른 CPU의 더 최신 지연 설정을 그대로 유지 */

	/* calculate the current delay in effect - 1/2 every second */
	tdelta = now->now - iocg->delay_at; /* [한국어] 마지막 지연 설정 이후 경과 시간(μs) — 감쇠 횟수 계산의 기준 */
	shift = div64_u64(tdelta, USEC_PER_SEC); /* [한국어] 경과 시간을 초 단위로 변환 — 1초마다 지연을 1비트 오른쪽 시프트(절반 감소) */
	if (iocg->delay && shift < BITS_PER_LONG) /* [한국어] 현재 지연이 있고 shift가 비트 오버플로우 범위 내이면 감쇠 적용 */
		delay = iocg->delay >> shift; /* [한국어] 지수 감쇠: shift초 경과 후 남은 지연 = 원래 지연 / 2^shift — 자연스러운 지연 해소 */
	else
		delay = 0; /* [한국어] 지연 없거나 shift가 너무 크면(충분히 오래 지남) 지연 완전 소멸 */

	/* calculate the new delay from the debt amount */
	current_hweight(iocg, &hwa, NULL); /* [한국어] 현재 iocg의 계층적 active 비율(hwa) 읽기 — abs_vdebt를 실제 vtime 비용으로 환산하기 위해 필요 */
	vover = atomic64_read(&iocg->vtime) + /* [한국어] atomic: 현재 누적 issued vtime (이 값이 vnow보다 크면 예산 초과) */
		abs_cost_to_cost(iocg->abs_vdebt, hwa) - now->vnow; /* [한국어] vtime + 부채를 vtime으로 환산한 값 - 현재 장치 vtime = 총 초과분 */
	vover_pct = div64_s64(100 * vover, /* [한국어] 초과분을 한 주기 vtime(period_us * base_rate)에 대한 백분율로 변환 */
			      ioc->period_us * ioc->vtime_base_rate); /* [한국어] 한 주기 기준 vover 비율: 양수면 초과, 음수면 여유 */

	if (vover_pct <= MIN_DELAY_THR_PCT) /* [한국어] 초과 비율이 최소 임계(MIN_DELAY_THR_PCT%) 이하이면 지연 불필요 */
		new_delay = 0; /* [한국어] 지연 없음 — 예산 초과가 무시 가능한 수준 */
	else if (vover_pct >= MAX_DELAY_THR_PCT) /* [한국어] 초과 비율이 최대 임계(MAX_DELAY_THR_PCT%) 이상이면 최대 지연 적용 */
		new_delay = MAX_DELAY; /* [한국어] MAX_DELAY(250ms): 심각한 예산 초과 시 IO를 최대한 억제 */
	else /* [한국어] 중간 범위: MIN_DELAY에서 MAX_DELAY 사이를 선형 보간 */
		new_delay = MIN_DELAY +
			div_u64((MAX_DELAY - MIN_DELAY) *
				(vover_pct - MIN_DELAY_THR_PCT), /* [한국어] 임계 초과분에 비례한 추가 지연량 계산 */
				MAX_DELAY_THR_PCT - MIN_DELAY_THR_PCT); /* [한국어] 전체 임계 범위로 정규화 */

	/* pick the higher one and apply */
	if (new_delay > delay) { /* [한국어] 새로 계산된 지연이 감쇠된 현재 지연보다 크면 새 값으로 교체 — 지연은 항상 더 큰 값 방향으로만 즉시 갱신 */
		iocg->delay = new_delay; /* [한국어] 새 지연 값 저장 — 다음 호출에서 감쇠 계산의 시작점 */
		iocg->delay_at = now->now; /* [한국어] 새 지연 설정 시각 기록 — 다음 호출에서 tdelta 계산 기준 */
		delay = new_delay; /* [한국어] 지역 변수도 업데이트 — 아래 적용 단계에서 사용 */
	}

	if (delay >= MIN_DELAY) { /* [한국어] 적용할 지연이 최소 유효 지연(MIN_DELAY) 이상이면 blkcg 메커니즘으로 IO 억제 */
		if (!iocg->indelay_since) /* [한국어] 처음 지연 상태에 진입하는 경우 — 지연 누적 통계 시작 */
			iocg->indelay_since = now->now; /* [한국어] 지연 시작 시각 기록 — indelay_us 통계 계산의 기준점 */
		blkcg_set_delay(blkg, delay * NSEC_PER_USEC); /* [한국어] blkcg use_delay 설정: μs를 ns로 변환해 전달 — 이후 이 cgroup의 bio 제출 시 delay ns만큼 추가 대기 */
		return true; /* [한국어] 지연 설정됨 — 호출자에게 이 iocg가 현재 지연 중임을 알림 */
	} else { /* [한국어] 지연이 소멸되었거나 처음부터 불필요한 경우 */
		if (iocg->indelay_since) { /* [한국어] 이전에 지연 상태였으면 누적 통계를 기록 */
			iocg->stat.indelay_us += now->now - iocg->indelay_since; /* [한국어] 지연 상태였던 총 시간(μs)을 누적 — 사용자 공간 통계 파일에 반영 */
			iocg->indelay_since = 0; /* [한국어] 지연 종료 — 다음 지연 진입 시 다시 기록 시작 */
		}
		iocg->delay = 0; /* [한국어] 저장된 지연 값도 클리어 — 다음 호출에서 새 지연을 깨끗하게 계산 */
		blkcg_clear_delay(blkg); /* [한국어] blkcg use_delay 해제 — 이후 이 cgroup의 bio 제출이 즉시 허용됨 */
		return false; /* [한국어] 지연 없음 — 호출자에게 이 iocg가 정상 운영 중임을 알림 */
	}
}

/*
 * [한국어]
 * iocg_incur_debt - 우선 발행된 IO의 비용을 abs_vdebt(부채)로 기록
 *
 * @iocg: 부채를 기록할 iocg. abs_vdebt 증가, indebt_since 설정, 가중치 전파, pcpu 통계 갱신.
 * @abs_cost: 이번 IO의 abs_cost(hweight_inuse와 무관한 절대 vtime 비용).
 *            부채 기록에는 abs_cost를 사용하고, 실제 vtime 비용 계산은 현재 hweight로 환산.
 * @now: 현재 시간 스냅샷. indebt_since 초기화에 사용.
 * @return: 없음 (void). iocg->abs_vdebt와 pcpu 통계(abs_vusage)를 증가시킨다.
 *
 * root cgroup이나 fatal signal을 받은 프로세스의 IO는 waitq에서 블록시킬 수 없다.
 * (블록하면 우선순위 역전이나 OOM 처리 지연 등의 문제 발생)
 * 따라서 IO 비용을 abs_vdebt에 기록하고 즉시 장치에 발행한다.
 * 부채 발생 시 이 iocg의 inuse를 0으로 설정(propagate_weights)하여
 * 자신의 장치 시간 할당분을 모두 형제 cgroup에 양도한다.
 * 이후 ioc_timer_fn()이 iocg_pay_debt()를 호출해 여유 예산이 생길 때마다 부채를 상환하고,
 * 완납 시 last_inuse로 inuse를 복원한다.
 * abs_cost는 pcpu_stat->abs_vusage에도 누적하여 사용량 통계에 반영한다.
 *
 * 실행 컨텍스트: ioc->lock AND iocg->waitq.lock 동시 보유 상태 (양쪽 lockdep 확인).
 *   IO 제출 경로(이 iocg가 우선 발행으로 처리되는 특수 경로)에서 호출됨.
 * 호출자: ioc_rq_qos_throttle()의 우선 발행 경로 — root/fatal-signal IO의 비용 기록.
 * 호출 대상: propagate_weights(), get_cpu_ptr/put_cpu_ptr(), local64_add().
 *
 * 호출 체인:
 *   ioc_rq_qos_throttle (우선 발행) → [iocg_incur_debt] →
 *   propagate_weights(inuse=0) → pcpu abs_vusage 누적
 *   ↑ 이후: ioc_timer_fn → iocg_pay_debt → propagate_weights(inuse 복원)
 */
static void iocg_incur_debt(struct ioc_gq *iocg, u64 abs_cost,
			    struct ioc_now *now)
{
	struct iocg_pcpu_stat *gcs; /* [한국어] 현재 CPU의 per-CPU 통계 포인터 — abs_vusage 누적에 사용 */

	lockdep_assert_held(&iocg->ioc->lock); /* [한국어] ioc->lock 없이 호출 시 경고 — propagate_weights를 안전하게 수행하기 위해 필요 */
	lockdep_assert_held(&iocg->waitq.lock); /* [한국어] waitq.lock 없이 호출 시 경고 — abs_vdebt 변경은 waitq.lock 하에서만 일관성 보장 */
	WARN_ON_ONCE(list_empty(&iocg->active_list)); /* [한국어] 비활성 iocg에 부채를 기록하면 안 됨 — 활성 상태에서만 부채 처리가 의미 있음 */

	/*
	 * Once in debt, debt handling owns inuse. @iocg stays at the minimum
	 * inuse donating all of it share to others until its debt is paid off.
	 */
	if (!iocg->abs_vdebt && abs_cost) { /* [한국어] 이전에 부채가 없다가 새 부채가 발생하는 첫 진입 시점 — abs_cost=0이면 비용 없는 호출로 무시 */
		iocg->indebt_since = now->now; /* [한국어] 부채 시작 시각 기록 — iocg_pay_debt() 완납 후 indebt_us 통계를 계산하기 위한 기준점 */
		propagate_weights(iocg, iocg->active, 0, false, now); /* [한국어] inuse를 0으로 설정해 이 iocg의 장치 시간 할당분을 모두 형제에 양도 — 부채 상환 완료 전까지 자신은 IO를 제출하지 않는 것처럼 처리 */
	}

	iocg->abs_vdebt += abs_cost; /* [한국어] 이번 IO의 abs_cost를 누적 부채에 추가 — iocg_pay_debt()가 여유 예산만큼 차감하며 상환 */

	gcs = get_cpu_ptr(iocg->pcpu_stat); /* [한국어] 현재 CPU의 per-CPU 통계 구조체 획득 — preemption 비활성화하여 CPU 이탈 방지 */
	local64_add(abs_cost, &gcs->abs_vusage); /* [한국어] 이 iocg의 총 abs_vtime 사용량에 이번 비용 누적 — 주기마다 합산하여 ioc_timer_fn()이 사용 통계 계산에 활용 */
	put_cpu_ptr(gcs); /* [한국어] per-CPU 포인터 반납 및 preemption 재활성화 */
}

/*
 * [한국어]
 * iocg_pay_debt - 여유 vbudget으로 abs_vdebt를 상환하고 완납 시 inuse 복원
 *
 * @iocg: 부채를 상환할 iocg. abs_vdebt 감소, indebt_since/stat.indebt_us 갱신,
 *         완납 시 propagate_weights()로 inuse 복원.
 * @abs_vpay: 이번에 상환할 최대 abs_cost 양. 실제 상환량은 min(abs_vpay, abs_vdebt).
 * @now: 현재 시간 스냅샷. 완납 시 indebt_us 누적 통계 계산에 사용.
 * @return: 없음 (void). iocg->abs_vdebt를 줄이고, 완납 시 계층 가중치를 복원한다.
 *
 * iocg_incur_debt()가 기록한 부채를 ioc_timer_fn()이 매 주기마다 이 함수로 상환한다.
 * 부채 중인 iocg의 inuse가 0으로 설정되어 있으므로, 부채 금액만큼의 vtime 예산을
 * 다른 cgroup들이 사용하게 된다. 그 "초과 사용분"이 이 iocg의 부채 상환 재원이 된다.
 * 동작 순서:
 * 1) abs_vpay와 현재 abs_vdebt 중 작은 값만큼 상환 — 부채를 넘는 과상환 방지.
 * 2) 부채가 완전히 청산되면(abs_vdebt == 0):
 *    - indebt_since 이후 경과 시간을 stat.indebt_us에 누적
 *    - indebt_since를 0으로 클리어
 *    - propagate_weights()로 inuse를 last_inuse(부채 진입 전 값)로 복원
 *      → 이후 이 iocg가 다시 정상 예산 비율로 IO를 제출 가능
 *
 * 실행 컨텍스트: ioc->lock AND iocg->waitq.lock 동시 보유 상태 (양쪽 lockdep 확인).
 *   ioc_timer_fn()의 타이머 콜백(softirq)에서 호출됨.
 * 호출자: ioc_timer_fn() — 매 주기 활성 iocg를 순회하며 부채가 있는 경우 호출.
 * 호출 대상: min(), propagate_weights().
 *
 * 호출 체인:
 *   ioc_timer_fn → [iocg_pay_debt] → propagate_weights(inuse 복원)
 *   → __propagate_weights, commit_weights → hweight_gen 증가
 */
static void iocg_pay_debt(struct ioc_gq *iocg, u64 abs_vpay,
			  struct ioc_now *now)
{
	lockdep_assert_held(&iocg->ioc->lock); /* [한국어] ioc->lock 없이 호출 시 경고 — propagate_weights를 안전하게 수행하기 위해 필요 */
	lockdep_assert_held(&iocg->waitq.lock); /* [한국어] waitq.lock 없이 호출 시 경고 — abs_vdebt 변경은 waitq.lock 하에서만 일관성 보장 */

	/*
	 * make sure that nobody messed with @iocg. Check iocg->pd.online
	 * to avoid warn when removing blkcg or disk.
	 */
	WARN_ON_ONCE(list_empty(&iocg->active_list) && iocg->pd.online); /* [한국어] 비활성화된 iocg(active_list 비어 있고 아직 온라인)에 상환 시도는 버그 — blkcg/disk 제거 중이면 pd.online=false이므로 경고 억제 */
	WARN_ON_ONCE(iocg->inuse > 1); /* [한국어] 부채 중인 iocg의 inuse는 반드시 최솟값(0 또는 1) 유지 — iocg_incur_debt()가 inuse=0으로 설정했으므로, 여기서 >1이면 상태 불일치 버그 */

	iocg->abs_vdebt -= min(abs_vpay, iocg->abs_vdebt); /* [한국어] 상환량 = min(이번 상환 한도, 남은 부채) — abs_vpay가 abs_vdebt보다 크면 부채만큼만 상환(음수 방지) */

	/* if debt is paid in full, restore inuse */
	if (!iocg->abs_vdebt) { /* [한국어] 부채가 완전히 청산되었으면 inuse를 복원하여 정상 동작 재개 */
		iocg->stat.indebt_us += now->now - iocg->indebt_since; /* [한국어] 부채 상태였던 총 시간(μs)을 누적 — 사용자 공간 cgroup 통계에 반영 */
		iocg->indebt_since = 0; /* [한국어] 부채 시작 시각 클리어 — 다음 부채 진입 시 iocg_incur_debt()가 새로 설정 */

		propagate_weights(iocg, iocg->active, iocg->last_inuse, /* [한국어] inuse를 부채 진입 전 값(last_inuse)으로 복원 — 이제 이 iocg가 다시 자신의 가중치 비율로 IO 예산을 받음 */
				  false, now); /* [한국어] save=false: 이미 복원하는 단계이므로 새 margin 저장 불필요 */
	}
}

/*
 * [한국어]
 * iocg_wake_fn - waitq 순회 콜백: vbudget 내에서 대기 bio 하나를 깨움
 *
 * @wq_entry: 깨울 대기 항목 (struct iocg_wait에 내장된 wait_queue_entry)
 * @mode:     wake_up 모드 (TASK_NORMAL 등); default_wake_function에 그대로 전달
 * @flags:    wake_up 플래그; default_wake_function에 그대로 전달
 * @key:      struct iocg_wake_ctx 포인터 — ctx->vbudget(잔여 예산), ctx->hw_inuse 공유
 * @return:   0 = 탐색 계속(예산 내), -1 = 탐색 중단(vbudget 소진)
 *
 * __wake_up_locked_key()가 iocg->waitq를 순회할 때 항목마다 호출되는 콜백이다.
 * wait->abs_cost를 ctx->hw_inuse로 vtime 비용(cost)으로 환산하고 ctx->vbudget에서
 * 차감한다. 예산이 남으면 iocg_commit_bio()로 vtime을 전진시키고 bio 발행을
 * 허가(wait->committed = true)한 뒤 태스크를 깨운다. 예산 부족 시 -1을 반환해
 * __wake_up_locked_key()의 순회를 즉시 종료한다.
 * autoremove_wake_function()은 태스크 상태가 실제로 바뀔 때만 wait entry를 제거하므로,
 * 여기서는 default_wake_function() 후 list_del_init_careful()로 항상 제거한다.
 *
 * 실행 컨텍스트: iocg->waitq.lock 보유 상태에서 호출됨 (softirq or process context).
 * 에러 경로: 예산 소진 시 -1 반환, 상위 __wake_up_locked_key()가 순회를 중단.
 *
 * 호출 체인:
 *   iocg_kick_waitq → __wake_up_locked_key → [iocg_wake_fn] → iocg_commit_bio
 *                                                             → default_wake_function
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
 * [한국어]
 * iocg_kick_waitq - 누적 예산 계산 후 부채 상환, waitq 대기자 깨우기
 *
 * @iocg:     대상 ioc_gq — waitq와 vtime 예산/부채를 보유
 * @pay_debt: true이면 이 호출이 부채를 상환함; 이때 호출자는 ioc->lock도 보유해야 함
 * @now:      현재 vtime/wallclock 스냅샷
 * @return:   없음 (void)
 *
 * 현재 vnow - iocg->vtime으로 사용 가능한 vbudget을 계산한다.
 * pay_debt가 true이고 abs_vdebt가 있으며 vbudget이 양수인 경우,
 * 부채를 먼저 상환(iocg_pay_debt)하고 vtime/done_vtime을 전진시킨다.
 * 남은 vbudget으로 iocg_wake_fn 콜백을 통해 waitq의 대기 bio를 순서대로 깨운다.
 * 대기자가 남아있으면 부족분(vshortage)을 vtime_base_rate로 나눠 다음 wakeup
 * 시각을 계산하고 iocg->waitq_timer를 arm한다. 기존 타이머가 timer_slack_ns
 * 이내에 있으면 재스케줄을 생략해 불필요한 타이머 인터럽트를 방지한다.
 *
 * 실행 컨텍스트: iocg->waitq.lock 보유 필수; pay_debt=true 시 ioc->lock도 필수.
 *   - ioc_timer_fn, ioc_forgive_debts, iocg_waitq_timer_fn 등에서 호출.
 * 에러 경로: abs_vdebt 잔존 시 vbudget을 강제 음수로 설정해 waitq 깨우기를 억제.
 *
 * 호출 체인:
 *   ioc_timer_fn / ioc_forgive_debts / iocg_waitq_timer_fn
 *     → [iocg_kick_waitq] → iocg_pay_debt
 *                          → __wake_up_locked_key → iocg_wake_fn
 *                          → hrtimer_start_range_ns (waitq_timer)
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
		iocg_kick_delay(iocg, now);		/* [한국어] cgroup의 use_delay를 갱신 — 스로틀 대상 프로세스가
						 * 스케줄 아웃 시점에 지연을 부과받게 한다. */

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
	expires += ioc->timer_slack_ns;	/* [한국어] 만료 시각에 slack을 더해 인접 타이머와 함께 처리되도록 한다 */

	/* if already active and close enough, don't bother */
	oexpires = ktime_to_ns(hrtimer_get_softexpires(&iocg->waitq_timer));	/* 기존 NVMe 재개 타이머 만료 시각 */
	if (hrtimer_is_queued(&iocg->waitq_timer) &&	/* 이미 NVMe 재개 타이머가 있고 */
	    abs(oexpires - expires) <= ioc->timer_slack_ns)	/* slack 내에 있으면 재스케줄 생략 */
		return;

	hrtimer_start_range_ns(&iocg->waitq_timer, ns_to_ktime(expires),	/* NVMe 예산 회복 시점에 issuer 깨움 */
			       ioc->timer_slack_ns, HRTIMER_MODE_ABS);
}

/*
 * [한국어]
 * iocg_waitq_timer_fn - waitq hrtimer 만료 콜백: NVMe 예산 회복 시 대기 bio 재개
 *
 * @timer: 만료된 hrtimer (iocg->waitq_timer에 내장)
 * @return: HRTIMER_NORESTART — one-shot; 재암은 iocg_kick_waitq 내에서 수행
 *
 * iocg_kick_waitq()가 waitq 대기자가 남아있을 때 arm하는 one-shot hrtimer 콜백이다.
 * 주기 타이머(ioc_timer_fn)와 독립적으로, vrate에 따라 예산이 회복될 시점에 발화해
 * 대기 중인 bio 발행을 재개한다. abs_vdebt 존재 여부를 READ_ONCE로 락 없이 확인하고,
 * 부채가 있으면 ioc->lock도 함께 획득(iocg_lock)해 부채 상환과 waitq 처리를 원자적으로
 * 수행한다. 부채가 없으면 iocg->waitq.lock만 획득해 오버헤드를 최소화한다.
 *
 * 실행 컨텍스트: hrtimer softirq context. iocg->waitq.lock (+ 조건부 ioc->lock) 획득.
 * 에러 경로: 없음; iocg_kick_waitq 내에서 예산 부족 시 타이머를 재arm.
 *
 * 호출 체인:
 *   hrtimer 만료 → [iocg_waitq_timer_fn] → iocg_kick_waitq → iocg_wake_fn
 */
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
 * [한국어]
 * ioc_lat_stat - 주기별 NVMe 완료 지연(missed_ppm) 및 rq 대기(rq_wait_pct) 통계 집계
 *
 * @ioc:             대상 ioc 컨트롤러 (pcpu_stat 배열 포함)
 * @missed_ppm_ar:   [READ/WRITE] latency QoS 목표를 놓친 비율(ppm) 출력 배열
 * @rq_wait_pct_p:   이번 주기 대비 rq(request/tag) 할당 대기 시간 비율(%) 출력
 * @nr_done:         이번 주기 NVMe 완료 총수(met + missed) 출력
 * @return:          없음 (void)
 *
 * 모든 온라인 CPU의 ioc_pcpu_stat를 순회하며 nr_met[], nr_missed[], rq_wait_ns를
 * 누적한다. 각 per-CPU 값은 last_met/last_missed/last_rq_wait_ns와의 차분으로
 * 주기 간 증가분만 계산하고, 기준값을 현재 값으로 갱신한다.
 * missed_ppm[rw] = nr_missed[rw] * 1e6 / (nr_met[rw] + nr_missed[rw]) 으로 계산.
 * rq_wait_pct = rq_wait_ns * 100 / (period_us * NSEC_PER_USEC)으로 계산.
 * 결과는 ioc_timer_fn이 busy_level 조정 및 vrate 제어에 사용한다.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — softirq context, ioc->lock 보유 전에 호출.
 * 에러 경로: 완료가 없으면 missed_ppm을 0으로 설정.
 *
 * 호출 체인:
 *   ioc_timer_fn → [ioc_lat_stat] → (per_cpu_ptr, local_read, local64_read)
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

/*
 * [한국어]
 * iocg_is_idle - 이번 주기에 이 cgroup이 완전히 유휴(idle)인지 판별
 *
 * @iocg:   검사할 ioc_gq (per-cgroup×device 상태)
 * @return: true = idle (비활성화 대상), false = 아직 활성
 *
 * 두 가지 조건으로 idle 여부를 판단한다:
 *   1. active_period가 cur_period와 다르면 이번 주기에 제출된 IO가 없음.
 *   2. done_vtime == vtime 이면 in-flight IO가 없음.
 * 두 조건을 모두 만족하면 true를 반환한다. ioc_check_iocgs()가 이 결과로
 * iocg를 active_iocgs에서 제거(deactivate)해 weight 계산에서 배제한다.
 *
 * 실행 컨텍스트: ioc_check_iocgs() 내 — ioc->lock 보유 상태.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   ioc_check_iocgs → [iocg_is_idle]
 */
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
 * [한국어]
 * iocg_build_inner_walk - leaf iocg의 조상(ancestor)들을 pre-order로 inner_walk에 추가
 *
 * @iocg:       대상 leaf ioc_gq
 * @inner_walk: pre-order 순서로 inner 노드를 연결할 list_head
 * @return:     없음 (void)
 *
 * 통계 상위 전파(iocg_flush_stat) 및 기부량 계산(transfer_surpluses)을 위해
 * cgroup 계층 트리의 inner 노드(leaf가 아닌 중간 노드)를 pre-order로 순회할
 * 목록을 구성한다. 먼저 이미 walk_list에 등록된 가장 낮은 조상 레벨을 찾아
 * 중복 등록을 방지하고, 그 아래 레벨부터 leaf 바로 위까지 순서대로 추가한다.
 * inner 노드들은 walk_list로 연결되며, 사용 후 caller가 list_del_init으로 해제해야 함.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (iocg_flush_stat / transfer_surpluses 내).
 * 에러 경로: WARN_ON_ONCE로 이미 walk_list에 있는 leaf 노드 중복 등록을 방지.
 *
 * 호출 체인:
 *   iocg_flush_stat / transfer_surpluses → [iocg_build_inner_walk]
 *                                        → (list_add_tail으로 inner_walk에 추가)
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

/*
 * [한국어]
 * iocg_flush_stat_upward - 이 iocg의 stat 증가분을 부모 iocg로 전파
 *
 * @iocg:   stat을 전파할 ioc_gq (leaf 또는 inner 노드)
 * @return: 없음 (void)
 *
 * iocg->stat과 iocg->last_stat의 차분(usage_us, wait_us, indebt_us, indelay_us)을
 * 직접 부모 iocg(ancestors[level-1])의 stat에 누적한다. root(level == 0)이면
 * 부모가 없으므로 stat 전파는 건너뛰고 last_stat만 갱신한다.
 * iocg_flush_stat()에서 inner 노드들을 leaf에서 root 방향으로 역순 순회하며
 * 이 함수를 호출해 계층적 stat 집계를 완성한다.
 *
 * 실행 컨텍스트: ioc->lock 보유 상태 (iocg_flush_stat 내).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   iocg_flush_stat → iocg_flush_stat_leaf → [iocg_flush_stat_upward]
 *   iocg_flush_stat → list_for_each_entry_safe_reverse → [iocg_flush_stat_upward]
 */
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

/*
 * [한국어]
 * iocg_flush_stat_leaf - leaf cgroup의 per-CPU abs_vusage를 집계하고 상위로 전파
 *
 * @iocg: 집계할 leaf ioc_gq
 * @now:  현재 vtime/wallclock 스냅샷 (현재는 직접 사용 안 함, 확장용)
 * @return: 없음 (void)
 *
 * 모든 가능한 CPU의 iocg->pcpu_stat->abs_vusage를 합산해 이번 주기의 절대 vtime
 * 사용량 증가분을 구한다. 증가분을 vtime_base_rate로 나눠 wallclock μs(usage_delta_us)로
 * 환산하고 iocg->stat.usage_us에 누적한다. 이후 iocg_flush_stat_upward()를 호출해
 * 증가분을 부모 cgroup으로 전파한다.
 * 이 함수는 leaf 노드에만 호출되며, inner 노드는 iocg_flush_stat_upward()로만 처리된다.
 *
 * 실행 컨텍스트: ioc->lock 보유 필수 (lockdep_assert_held로 검증).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   iocg_flush_stat → [iocg_flush_stat_leaf] → iocg_flush_stat_upward
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

/*
 * [한국어]
 * iocg_flush_stat - 모든 활성 iocg의 stat을 갱신하고 계층 상위로 전파
 *
 * @target_iocgs: 플러시할 활성 iocg 목록 (ioc->active_iocgs)
 * @now:          현재 vtime/wallclock 스냅샷
 * @return:       없음 (void)
 *
 * 두 단계로 통계를 최신화한다:
 *   1단계: target_iocgs의 각 leaf iocg에 iocg_flush_stat_leaf()를 호출해
 *          per-CPU 카운터를 집계하고, 동시에 iocg_build_inner_walk()로
 *          inner 노드 pre-order 목록(inner_walk)을 구성한다.
 *   2단계: inner_walk를 역순(leaf→root 방향)으로 순회하면서
 *          iocg_flush_stat_upward()로 각 inner 노드의 stat을 부모로 전파하고,
 *          walk_list를 해제한다.
 * 결과적으로 cgroup 계층 전체의 IO 사용량 통계가 root까지 누적된다.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — ioc->lock 보유 상태.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   ioc_timer_fn → [iocg_flush_stat] → iocg_flush_stat_leaf → iocg_flush_stat_upward
 *                                     → iocg_build_inner_walk
 */
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
 * [한국어]
 * hweight_after_donation - work-conservation을 위해 기부 후 iocg의 목표 hweight_inuse 계산
 *
 * @iocg:    기부자 ioc_gq
 * @old_hwi: 현재 hweight_inuse (기부 전)
 * @hwm:     hweight_inuse의 상한 (기부 불필요 시 반환값이기도 함)
 * @usage:   이번 주기 실제 사용 비율 (WEIGHT_ONE 기준)
 * @now:     현재 vtime/wallclock 스냅샷
 * @return:  기부 후 목표 hweight_inuse; 기부 불필요 시 hwm, 부채 있으면 1
 *
 * 기부 불가 조건을 먼저 판별한다:
 *   - abs_vdebt가 있으면 부채 처리가 inuse를 통제하므로 1 반환.
 *   - waitq에 대기자가 있거나 vtime이 min margin 이내면 hwm 반환(기부 안 함).
 * target margin(MARGIN_TARGET_PCT) 초과 예산은 버리고 vtime_err를 보정한다.
 * 기부량 공식:
 *   new_hwi = usage / (1 - MARGIN_TARGET + delta)
 *   여기서 delta = (vnow - vtime) / period_vtime
 * 반환값은 [1, hwm] 범위로 clamp된다. ioc_timer_fn이 이 값을 hweight_donating
 * 및 hweight_after_donation으로 저장하고 transfer_surpluses에서 사용한다.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — ioc->lock 보유 상태.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   ioc_timer_fn → [hweight_after_donation] → (atomic64_read, atomic64_add)
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
 * [한국어]
 * transfer_surpluses - 잉여 iocg들의 NVMe 시간 할당분을 부족한 iocg로 재분배
 *
 * @surpluses: 잉여가 있는 iocg 목록 (surplus_list로 연결)
 * @now:       현재 vtime/wallclock 스냅샷
 * @return:    없음 (void)
 *
 * Work-conservation을 달성하기 위해 NVMe 시간을 사용하지 않는 기부자(donor)
 * iocg들의 inuse를 낮춰, 부족한 수혜자(beneficiary) iocg들이 더 많은 NVMe
 * 시간을 할당받을 수 있도록 한다 (vrate 조정 없이 inuse만 조정하는 #2 방식).
 *
 * 알고리즘 개요 (Andy's method):
 *   1. after_sum이 WEIGHT_ONE 이상이면 비율을 down-scale.
 *   2. inner_walk를 pre-order로 구성해 계층 트리를 표현.
 *   3. 잉여 leaf들의 hweight_donating / hweight_after_donation을 부모로 전파.
 *   4. 전역 기부율 gamma = (1 - t_r') / (1 - t_r) 계산.
 *   5. inner 노드의 hweight_inuse와 child_adjusted_sum 계산 (pre-order).
 *   6. 각 기부 leaf의 새 inuse를 __propagate_weights로 적용.
 *
 * 상세 수학은 함수 위의 영문 블록 주석 및 Google Drive PDF 참조.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — ioc->lock 보유 상태.
 * 에러 경로: 무효한 기부 weight가 감지되면 WARN + pr_warn으로 경고.
 *
 * 호출 체인:
 *   ioc_timer_fn → [transfer_surpluses] → iocg_build_inner_walk
 *                                        → __propagate_weights
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
	gamma = DIV_ROUND_UP(	/* [한국어] 전역 시간 기부율(donation rate) 계산 */
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
 * [한국어]
 * ioc_forgive_debts - NVMe 장치가 충분히 한가할 때 부채 cgroup의 abs_vdebt/delay를 탕감
 *
 * @ioc:          대상 ioc 컨트롤러
 * @usage_us_sum: 이번 주기 전체 활성 iocg의 wallclock 사용량 합계 (μs)
 * @nr_debtors:   현재 abs_vdebt 또는 delay가 있는 iocg 수
 * @now:          현재 vtime/wallclock 스냅샷
 * @return:       없음 (void)
 *
 * 부채 탕감 배경: 낮은 weight의 cgroup이 메모리 회수(anonymous page reclaim)
 * 같은 상황에서 다수의 write를 발행하면 abs_vdebt가 수 초분까지 누적될 수 있다.
 * 이후 다른 IO 발행자가 없으면 부채 cgroup만 홀로 NVMe에 접근하면서 장치가 idle해진다.
 * 이를 방지하기 위해, 사용률이 DFGV_USAGE_PCT(50%) 이하인 상태가 DFGV_PERIOD(100ms)
 * 이상 지속되면 abs_vdebt와 delay를 nr_cycles번 반감(>>)한다.
 * busy_level > 0이면 device 포화로 간주해 usage_us_sum을 최소 period_us로 올림.
 * nr_cycles의 소수점 잔량은 dfgv_period_rem으로 이월해 이후 주기에서 보정한다.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — ioc->lock 보유 상태.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   ioc_timer_fn → [ioc_forgive_debts] → iocg_kick_waitq (부채 탕감 후 waitq 처리)
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
 * [한국어]
 * ioc_check_iocgs - 모든 활성 iocg의 waitq/debt/idle 상태를 점검하고 정리
 *
 * @ioc:    대상 ioc 컨트롤러
 * @now:    현재 vtime/wallclock 스냅샷
 * @return: abs_vdebt 또는 delay가 있는 iocg(debtor) 수
 *
 * active_iocgs를 list_for_each_entry_safe로 순회하며 세 가지 처리를 수행한다:
 *   1. wait_since/indebt_since/indelay_since로 누적 중인 stat을 현재 시각까지 flush.
 *   2. waitq 대기자, abs_vdebt, delay가 있으면 iocg_kick_waitq()로 즉시 재처리.
 *      vrate가 상승했다면 waitq 타이머 만료 전에도 깨울 수 있다.
 *      debtor이면 nr_debtors를 증가.
 *   3. idle 상태(iocg_is_idle())이면 초과 예산을 버리고 vtime_err를 보정한 뒤
 *      __propagate_weights(0, 0)으로 비활성화하고 active_iocgs에서 제거.
 * 마지막으로 commit_weights()로 변경된 weight 캐시를 갱신한다.
 *
 * 실행 컨텍스트: ioc_timer_fn 내 — ioc->lock 보유 상태. 각 iocg->waitq.lock도 획득.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   ioc_timer_fn → [ioc_check_iocgs] → iocg_kick_waitq
 *                                     → iocg_is_idle
 *                                     → __propagate_weights
 *                                     → commit_weights
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
 * [한국어]
 * ioc_timer_fn - iocost 피드백 제어 주기 타이머 핸들러 (메인 heartbeat)
 *
 * @timer: ioc->timer (struct timer_list)
 * @return: 없음 (void)
 *
 * blk-iocost의 핵심 제어 루프. period_us(기본 ~250μs)마다 발화해 다음을 순서대로 수행한다:
 *   1. ioc_lat_stat(): per-CPU missed[] / rq_wait_ns 집계 → missed_ppm, rq_wait_pct, nr_done
 *   2. ioc->lock 획득 후 ioc_now()로 현재 vtime 스냅샷
 *   3. ioc_check_iocgs(): 각 iocg의 waitq/debt/idle 점검, idle이면 비활성화
 *   4. iocg_flush_stat(): 활성 iocg의 per-CPU 사용량 집계 및 계층 상위 전파
 *   5. 각 활성 iocg의 vtime 여유 분석: 잉여이면 surpluses에 등록, 부족이면 nr_shortages++
 *   6. surpluses와 nr_shortages가 모두 있으면 transfer_surpluses()로 inuse 재분배
 *   7. missed_ppm/rq_wait_pct/nr_lagging으로 busy_level 갱신
 *   8. ioc_adjust_base_vrate(): busy_level에 따라 vtime_base_rate(vrate) 조정
 *   9. ioc_forgive_debts(): 장치 한가 시 abs_vdebt/delay 탕감
 *  10. cur_period 증가 → ioc_start_period()로 다음 주기 타이머 arm
 *
 * 실행 컨텍스트: softirq timer context. ioc->lock (spin_lock_irq) 보유 구간 존재.
 * 에러 경로: period_vtime == 0이면 즉시 반환 (WARN + unlock).
 *
 * 호출 체인:
 *   kernel timer → [ioc_timer_fn] → ioc_lat_stat → ioc_check_iocgs → iocg_flush_stat
 *                                 → transfer_surpluses → hweight_after_donation
 *                                 → ioc_adjust_base_vrate → ioc_forgive_debts
 *                                 → ioc_start_period
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
	} else if (rq_wait_pct > RQ_WAIT_BUSY_PCT ||	/* [한국어] request 할당 대기 비율이 임계치를 넘음 = 장치가 포화 상태 */
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
 * [한국어]
 * adjust_inuse_and_calc_cost - bio 발행 직전 예산 여유를 검사하고,
 *                               부족하면 inuse를 단계적으로 올려 cost 확정
 *
 * @iocg:     비용을 검사할 cgroup의 NVMe 예산 상태 (ioc_gq)
 * @vtime:    현재 iocg의 issued vtime (atomic64_read(&iocg->vtime))
 * @abs_cost: hwi-독립 절대 비용 (calc_vtime_cost가 반환한 값)
 * @now:      현재 주기의 vnow/vrate 스냅샷
 * @return:   hwi를 적용한 실제 vtime 비용 (cost); iocg_commit_bio/waitq에 전달됨
 *
 * ioc_rqos_throttle/ioc_rqos_merge에서 bio 비용을 확정하기 위해 호출된다.
 * 현재 margin(= now->vnow - vtime - cost)이 saved_margin보다 낮고 inuse가
 * active에 도달하지 않은 경우, INUSE_ADJ_STEP_PCT(25%) 단위로 inuse를
 * 늘리면서 bio 비용이 현재 예산 안으로 들어올 때까지 반복한다.
 * abs_vdebt가 있는 iocg는 debt handling이 inuse를 소유하므로 조정 없이 반환.
 *
 * 실행 컨텍스트: submit_bio 태스크 컨텍스트 (process context, preemptible).
 *               ioc->lock을 spin_lock_irqsave로 획득 가능.
 * 에러 경로: 없음 (항상 유효한 cost를 반환).
 *
 * 호출 체인:
 *   ioc_rqos_throttle / ioc_rqos_merge
 *     → [adjust_inuse_and_calc_cost]
 *     → current_hweight, abs_cost_to_cost, propagate_weights
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
 * [한국어]
 * calc_vtime_cost_builtin - bio의 선형(linear) 비용 모델로 vtime 비용 산출
 *
 * @bio:      비용을 계산할 bio (방향/크기/LBA 정보 포함)
 * @iocg:     이 bio를 제출하는 cgroup의 NVMe 예산 상태 (cursor 접근)
 * @is_merge: true이면 기존 request와 병합 — seek 비용(seqio/randio)을 건너뜀
 * @costp:    계산 결과 vtime 비용을 반환하는 출력 포인터
 * @return:   void (비용은 *costp에 저장)
 *
 * ioc->params.lcoefs[] 테이블(AUTOP 또는 사용자 설정)과 bio 크기, 직전
 * cursor와의 LBA 거리를 조합해 NVMe 처리 예상 vtime을 계산한다.
 * cursor 거리 > LCOEF_RANDIO_PAGES(4096 페이지 = 16 MB)이면 random IO
 * 계수(coef_randio)를, 이하이면 sequential IO 계수(coef_seqio)를 적용한다.
 * 크기 기반 비용은 (페이지 수 × coef_page)로 누적한다.
 * discard/flush 등 비용 모델 미지원 opcode는 cost=0으로 처리한다.
 *
 * 실행 컨텍스트: submit_bio 태스크 컨텍스트 (preemptible).
 *               is_merge=true이면 blk-mq softirq/hctx 스레드에서도 호출 가능.
 * 에러 경로: 없음. 0-byte bio나 미지원 opcode는 *costp=0으로 종료.
 *
 * 호출 체인:
 *   ioc_rqos_throttle / ioc_rqos_merge → calc_vtime_cost
 *     → [calc_vtime_cost_builtin]
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
	default:	/* [한국어] discard/write-zeroes/flush 등 — 선형 비용 모델을 적용할 수 없는 연산 */
		goto out;
	}

	if (iocg->cursor) {	/* 직전 bio 끝이 있으면 NVMe seq/rand 판별 */
		seek_pages = abs(bio->bi_iter.bi_sector - iocg->cursor);		/* [한국어] 직전 I/O 위치(cursor)와의 LBA 거리 — 탐색 비용 모델 입력 */
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

/*
 * [한국어]
 * calc_vtime_cost - bio의 vtime 비용을 계산하는 공개 래퍼
 *
 * @bio:      비용을 계산할 bio
 * @iocg:     이 bio를 제출하는 cgroup의 NVMe 예산 상태
 * @is_merge: true이면 seek 비용 제외 (bio 병합 경로)
 * @return:   hwi-독립 절대 vtime 비용 (abs_cost); 0이면 throttle 대상 외
 *
 * 현재는 calc_vtime_cost_builtin()만 호출하는 단순 래퍼이며,
 * 사용자 정의 비용 모델이 추가되면 여기서 분기한다.
 * ioc_rqos_throttle과 ioc_rqos_merge가 abs_cost를 얻기 위해 호출한다.
 *
 * 호출 체인:
 *   ioc_rqos_throttle / ioc_rqos_merge → [calc_vtime_cost]
 *     → calc_vtime_cost_builtin
 */
static u64 calc_vtime_cost(struct bio *bio, struct ioc_gq *iocg, bool is_merge)
{
	u64 cost;

	calc_vtime_cost_builtin(bio, iocg, is_merge, &cost);	/* [한국어] 선형 비용 모델로 vtime 비용 계산 */
	return cost;
}

/*
 * [한국어]
 * calc_size_vtime_cost_builtin - request 크기만으로 vtime 비용 산출 (seek 제외)
 *
 * @rq:    완료된 NVMe request (크기·방향 정보)
 * @ioc:   이 장치의 iocost 컨트롤러 (lcoefs 테이블 접근)
 * @costp: 계산 결과 비용을 반환하는 출력 포인터
 * @return: void (*costp에 비용 저장)
 *
 * ioc_rqos_done에서 request 완료 latency를 분석할 때 사용한다.
 * seek 비용(seqio/randio)은 제외하고 데이터 전송량(pages × coef_page)만으로
 * "이 request가 순수 전송에 걸릴 예상 vtime"을 반환한다.
 * 이 값을 기준으로 on_q_ns와 비교해 NVMe latency QoS 달성 여부를 판단한다.
 *
 * 실행 컨텍스트: NVMe CQ 완료 softirq 또는 NVMe completion 태스크.
 * 에러 경로: 없음. 미지원 opcode는 *costp=0.
 *
 * 호출 체인:
 *   ioc_rqos_done → calc_size_vtime_cost → [calc_size_vtime_cost_builtin]
 */
static void calc_size_vtime_cost_builtin(struct request *rq, struct ioc *ioc,
					 u64 *costp)
{
	unsigned int pages = blk_rq_stats_sectors(rq) >> IOC_SECT_TO_PAGE_SHIFT;	/* request의 NVMe PRP/SGL 페이지 수 */

	switch (req_op(rq)) {
	case REQ_OP_READ:	/* [한국어] read: 읽기 페이지당 lcoef_page 비용 */
		*costp = pages * ioc->params.lcoefs[LCOEF_RPAGE];
		break;
	case REQ_OP_WRITE:	/* [한국어] write: 쓰기 페이지당 lcoef_page 비용 */
		*costp = pages * ioc->params.lcoefs[LCOEF_WPAGE];
		break;
	default:	/* [한국어] discard/flush 등: 비용 0 (latency 측정 제외) */
		*costp = 0;
	}
}

/*
 * [한국어]
 * calc_size_vtime_cost - request 크기 기반 vtime 비용 계산 래퍼
 *
 * @rq:    완료된 NVMe request
 * @ioc:   이 장치의 iocost 컨트롤러
 * @return: 크기 기반 vtime 비용 (seek 제외); ioc_rqos_done에서 latency 기준선으로 사용
 *
 * calc_size_vtime_cost_builtin()의 단순 래퍼.
 * 반환값을 VTIME_PER_NSEC로 나누면 순수 전송 예상 nanosecond가 된다.
 *
 * 호출 체인:
 *   ioc_rqos_done → [calc_size_vtime_cost] → calc_size_vtime_cost_builtin
 */
static u64 calc_size_vtime_cost(struct request *rq, struct ioc *ioc)
{
	u64 cost;

	calc_size_vtime_cost_builtin(rq, ioc, &cost);	/* [한국어] seek 제외 크기 기반 비용 계산 */
	return cost;
}

/*
 * [한국어]
 * ioc_rqos_throttle - bio가 blk-mq/NVMe SQ로 날아가기 전 예산 검사 및 쓰로틀
 *
 * @rqos: ioc->rqos, RQ_QOS_COST로 등록된 핸들
 * @bio:  사용자 태스크가 방금 submit_bio()로 제출한 bio
 * @return: void (예산 소진 시 태스크를 waitq에서 블록킹)
 *
 * iocost의 핵심 throttle 진입점. bio당 abs_cost를 계산하고, vtime 예산이
 * 충분하면 iocg_commit_bio()로 즉시 통과시킨다. 부족하면 두 가지 경로:
 *   1) root/fatal_signal bio: iocg_incur_debt()로 debt 누적 후 즉시 통과.
 *      시스템 데드락 방지를 위해 blocking 금지.
 *   2) 일반 bio: iocg->waitq에 enqueue 후 TASK_UNINTERRUPTIBLE로 블록.
 *      waker(iocg_wake_fn)가 vtime 예산 확정 후 깨운다.
 * waitq 진입 시 iocg->inuse를 active까지 올려 예산 회복 속도를 최대화.
 *
 * 실행 컨텍스트: submit_bio 태스크 컨텍스트 (process context, preemptible).
 *               io_schedule()로 sleep 가능. IRQ-safe lock(iocg_lock) 사용.
 * 에러 경로: ioc 미활성/root cgroup/abs_cost=0/iocg_activate 실패 시 bypass.
 *
 * 호출 체인:
 *   submit_bio_noacct → blk_mq_submit_bio → rq_qos_throttle
 *     → [ioc_rqos_throttle]
 *     → calc_vtime_cost, adjust_inuse_and_calc_cost, iocg_commit_bio
 *     → (예산 부족) iocg_incur_debt / iocg_kick_waitq → io_schedule
 *     → (웨이크업) iocg_wake_fn → iocg_commit_bio
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
	use_debt = bio_issue_as_root_blkg(bio) || fatal_signal_pending(current);	/* [한국어] root cgroup의 I/O이거나 치명 시그널 대기 중이면 대기 대신
											     * "부채"로 처리한다 — 여기서 막으면 시스템 진행이 멈출 수 있다 */
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
			blkcg_schedule_throttle(rqos->disk,			/* [한국어] 스케줄 아웃 시점에 지연을 부과하도록 예약 */
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
 * [한국어]
 * ioc_rqos_merge - bio가 blk-mq 기존 request에 병합될 때 추가 비용 처리
 *
 * @rqos: ioc->rqos, RQ_QOS_COST 핸들
 * @rq:   병합 대상 blk-mq request (이미 SQ에 들어갈 수도 있음)
 * @bio:  rq에 병합되는 bio (추가 비용을 계산)
 * @return: void
 *
 * blk-mq가 bio를 기존 request와 병합(back/front-merge)하기 전에 호출된다.
 * is_merge=true로 calc_vtime_cost()를 호출해 seek 비용은 제외하고
 * 크기 기반 추가 비용만 계산한다. 예산이 충분하면 iocg_commit_bio()로
 * 정상 차감. 부족하면 iocg_incur_debt()로 debt 처리 — 이미 dispatch된
 * request에 병합되는 IO를 blocking하면 안 되므로 항상 허용한다.
 * back-merge로 cursor 범위가 확장되면 cursor도 갱신한다.
 *
 * 실행 컨텍스트: blk-mq dispatch 경로 (softirq 또는 hctx kthread).
 *               ioc->lock + iocg->waitq.lock 동시 획득.
 * 에러 경로: ioc 미활성/iocg NULL/root cgroup/abs_cost=0 시 bypass.
 *
 * 호출 체인:
 *   blk_mq_bio_list_merge / blk_mq_attempt_merge
 *     → rq_qos_merge → [ioc_rqos_merge]
 *     → calc_vtime_cost, adjust_inuse_and_calc_cost
 *     → iocg_commit_bio / iocg_incur_debt
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
 * [한국어]
 * ioc_rqos_done_bio - bio 완료 시 in-flight vtime 비용을 done_vtime에 반영
 *
 * @rqos: ioc->rqos, RQ_QOS_COST 핸들
 * @bio:  완료된 bio (bi_iocost_cost 필드에 할당된 비용 기록됨)
 * @return: void
 *
 * NVMe CQ 완료 처리 경로(blk_mq_end_request → rq_qos_done_bio)에서 호출.
 * bio가 iocg_commit_bio()에서 할당받은 bi_iocost_cost를 iocg->done_vtime에
 * 원자적으로 누적해, 현재 in-flight 비용(vtime - done_vtime)을 감소시킨다.
 * done_vtime은 iocg_is_idle() 등에서 참조해 실제 IO 완료 진행 상황을 파악.
 *
 * 실행 컨텍스트: NVMe CQ 완료 softirq 또는 NVMe completion kthread.
 *               bi_iocost_cost=0인 bio는 무시.
 *
 * 호출 체인:
 *   nvme_complete_rq → blk_mq_end_request → bio_endio
 *     → rq_qos_done_bio → [ioc_rqos_done_bio]
 *     → atomic64_add(&iocg->done_vtime)
 */
static void ioc_rqos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct ioc_gq *iocg = blkg_to_iocg(bio->bi_blkg);

	if (iocg && bio->bi_iocost_cost)	/* bio가 NVMe 비용을 가지고 있으면 */
		atomic64_add(bio->bi_iocost_cost, &iocg->done_vtime);		/* atomic: NVMe CQ 완료로 in-flight 비용 감소 */
}

/*
 * [한국어]
 * ioc_rqos_done - NVMe request 완료 시 latency QoS 통계와 rq_wait_ns 기록
 *
 * @rqos: ioc->rqos, RQ_QOS_COST 핸들
 * @rq:   완료된 NVMe request (alloc_time_ns, start_time_ns 기록됨)
 * @return: void
 *
 * NVMe CQ 완료 경로(blk_mq_end_request → rq_qos_done)에서 request 단위로
 * 호출된다. 다음 세 지표를 per-CPU ioc_pcpu_stat에 기록한다:
 *   - on_q_ns: rq->alloc_time_ns 기준 총 체류 시간 (큐 대기 + 장치 처리)
 *   - rq_wait_ns: NVMe tag/sbitmap 할당 대기 시간 (alloc → start 차이)
 *   - nr_met/nr_missed: (on_q_ns - size_nsec)이 QoS 목표 이내면 met, 초과면 missed
 * ioc_timer_fn()이 이 통계를 집계해 vrate와 busy_level을 조정한다.
 *
 * 실행 컨텍스트: NVMe CQ 완료 softirq 또는 completion kthread (CPU 고정).
 *               per-CPU 통계는 local_inc/local64_add로 lock-free 접근.
 * 에러 경로: ioc 미활성, alloc_time_ns=0, start_time_ns=0, 미지원 opcode 시 skip.
 *
 * 호출 체인:
 *   nvme_complete_rq → blk_mq_end_request → rq_qos_done
 *     → [ioc_rqos_done]
 *     → calc_size_vtime_cost, get_cpu_ptr(ioc->pcpu_stat)
 *     → local_inc(nr_met/nr_missed), local64_add(rq_wait_ns)
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
	rq_wait_ns = rq->start_time_ns - rq->alloc_time_ns;	/* [한국어] request 할당에 걸린 시간 = 태그(sbitmap) 대기 시간 */
	size_nsec = div64_u64(calc_size_vtime_cost(rq, ioc), VTIME_PER_NSEC);	/* request 크기에 비례하는 NVMe 기본 전송 시간 */

	ccs = get_cpu_ptr(ioc->pcpu_stat);	/* [한국어] 현재 CPU(완료 처리가 일어난 CPU)의 per-CPU 통계 */

	if (on_q_ns <= size_nsec ||	/* 데이터 전송 시간 이하이거나(추정) */
	    on_q_ns - size_nsec <= ioc->params.qos[pidx] * NSEC_PER_USEC)	/* NVMe latency QoS 목표 이내 */
		local_inc(&ccs->missed[rw].nr_met);		/* local: NVMe latency QoS 달성 카운트 */
	else
		local_inc(&ccs->missed[rw].nr_missed);		/* local: NVMe latency QoS 미달 카운트 */

	local64_add(rq_wait_ns, &ccs->rq_wait_ns);	/* local64: NVMe request 할당 대기 시간 누적 */

	put_cpu_ptr(ccs);	/* preemption 복원: NVMe 통계 per-CPU 일관성 */
}

/*
 * [한국어]
 * ioc_rqos_queue_depth_changed - NVMe 큐 깊이 변경 시 autop 프로파일 재선택
 *
 * @rqos: ioc->rqos, RQ_QOS_COST 핸들
 * @return: void
 *
 * NVMe 장치의 queue depth가 변경되면(예: nr_hw_queues 조정, 런타임 재설정)
 * blk-mq가 이 콜백을 호출한다. ioc_refresh_params()를 재호출해 현재 queue
 * depth에 맞는 autop 프로파일(AUTOP_SSD_QD1 / AUTOP_SSD_DEFAULT 등)을
 * 재선택하고 lcoefs를 갱신한다.
 *
 * 실행 컨텍스트: 큐 재설정 컨텍스트 (process context); ioc->lock으로 직렬화.
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues → rq_qos_queue_depth_changed
 *     → [ioc_rqos_queue_depth_changed] → ioc_refresh_params
 */
static void ioc_rqos_queue_depth_changed(struct rq_qos *rqos)
{
	struct ioc *ioc = rqos_to_ioc(rqos);	/* [한국어] rq_qos에서 iocost 컨트롤러 포인터 복원 */

	spin_lock_irq(&ioc->lock);	/* [한국어] autop 프로파일 재선택 보호 */
	ioc_refresh_params(ioc, false);	/* [한국어] 새 queue depth에 맞는 lcoefs/autop 재계산 */
	spin_unlock_irq(&ioc->lock);
}

/*
 * [한국어]
 * ioc_rqos_exit - iocost 컨트롤러를 장치에서 분리하고 자원 해제
 *
 * @rqos: ioc->rqos, RQ_QOS_COST 핸들
 * @return: void
 *
 * gendisk가 제거되거나 iocost가 명시적으로 비활성화될 때 호출된다.
 * blkcg_deactivate_policy()로 모든 cgroup의 ioc_gq(pd)를 해제하고,
 * ioc->running을 IOC_STOP으로 설정해 타이머가 자체 종료하도록 예고한다.
 * timer_shutdown_sync()로 ioc_timer_fn()이 완전히 종료됨을 보장한 뒤
 * per-CPU 통계 및 ioc 구조체를 해제한다.
 *
 * 실행 컨텍스트: 장치 제거 경로 (process context, sleeping 허용).
 * 에러 경로: 없음 (void).
 *
 * 호출 체인:
 *   disk_release / blk_cleanup_queue → rq_qos_exit
 *     → [ioc_rqos_exit]
 *     → blkcg_deactivate_policy, timer_shutdown_sync, free_percpu, kfree
 */
static void ioc_rqos_exit(struct rq_qos *rqos)
{
	struct ioc *ioc = rqos_to_ioc(rqos);	/* [한국어] rqos에서 iocost 컨트롤러 포인터 복원 */

	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_iocost);	/* blk-cgroup에서 iocost 분리 */

	spin_lock_irq(&ioc->lock);
	ioc->running = IOC_STOP;	/* NVMe 제어 타이머 정지 예고 */
	spin_unlock_irq(&ioc->lock);

	timer_shutdown_sync(&ioc->timer);	/* NVMe 주기 타이머 동기 종료 */
	free_percpu(ioc->pcpu_stat);	/* [한국어] per-CPU NVMe 완료/대기 통계 메모리 해제 */
	kfree(ioc);	/* [한국어] iocost 컨트롤러 구조체 해제 */
}

/*
 * [한국어]
 * ioc_rqos_ops - iocost가 blk-mq RQ_QOS 체인에 등록하는 콜백 테이블
 *
 * blk_iocost_init()에서 rq_qos_add()에 전달된다.
 * blk-mq는 각 IO 단계에서 체인의 모든 rq_qos를 순서대로 호출한다.
 */
static const struct rq_qos_ops ioc_rqos_ops = {
	.throttle = ioc_rqos_throttle,
	/* [한국어] bio → request 변환 전 예산 검사 및 대기; 핵심 throttle 진입점 */
	.merge = ioc_rqos_merge,
	/* [한국어] bio가 기존 request에 병합될 때 추가 비용 처리 */
	.done_bio = ioc_rqos_done_bio,
	/* [한국어] bio 단위 완료 시 done_vtime 누적 */
	.done = ioc_rqos_done,
	/* [한국어] request 완료 시 latency QoS 통계(nr_met/nr_missed, rq_wait_ns) 기록 */
	.queue_depth_changed = ioc_rqos_queue_depth_changed,
	/* [한국어] NVMe queue depth 변경 시 autop 프로파일 재선택 */
	.exit = ioc_rqos_exit,
	/* [한국어] 장치 제거 시 ioc 자원 정리 */
};

/*
 * [한국어]
 * blk_iocost_init - gendisk에 iocost 컨트롤러를 초기화하고 RQ_QOS_COST로 등록
 *
 * @disk:   iocost를 활성화할 블록 장치의 gendisk (NVMe 네임스페이스 단위)
 * @return: 0 성공, 음수 errno 실패
 *
 * 이 함수는 두 단계로 iocost를 장치에 연결한다:
 *   1단계: ioc 할당 및 초기화 — per-CPU 통계, 타이머, vtime_rate, seqcount,
 *          autop 프로파일 선택(ioc_refresh_params_disk)을 수행한다.
 *   2단계: rq_qos_add()로 blk-mq RQ_QOS 체인에 등록하고,
 *          blkcg_activate_policy()로 기존/신규 cgroup 각각에 ioc_pd_init()을
 *          통한 ioc_gq 초기화를 연결한다.
 * rq_qos 등록이 blkcg_activate_policy보다 먼저 되어야 ioc_pd_init()에서
 * q_to_ioc()로 ioc 포인터를 조회할 수 있다.
 *
 * 실행 컨텍스트: 장치 추가 경로 (process context, sleeping 허용).
 *               io.cost.qos write에서도 on-demand로 호출됨.
 * 에러 경로: -ENOMEM (ioc/pcpu_stat 할당 실패), rq_qos_add/blkcg_activate_policy
 *            실패 시 역순으로 정리 후 errno 반환.
 *
 * 호출 체인:
 *   disk_add_disk / ioc_qos_write
 *     → [blk_iocost_init]
 *     → kzalloc_obj, alloc_percpu, timer_setup, rq_qos_add
 *     → blkcg_activate_policy → ioc_pd_alloc → ioc_pd_init
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

	/* [한국어] 모든 CPU의 per-CPU NVMe latency QoS 통계를 0으로 초기화 */
	for_each_possible_cpu(cpu) {
		struct ioc_pcpu_stat *ccs = per_cpu_ptr(ioc->pcpu_stat, cpu);

		for (i = 0; i < ARRAY_SIZE(ccs->missed); i++) {
			local_set(&ccs->missed[i].nr_met, 0);	/* [한국어] latency QoS 달성 카운트 초기화 */
			local_set(&ccs->missed[i].nr_missed, 0);	/* [한국어] latency QoS 미달 카운트 초기화 */
		}
		local64_set(&ccs->rq_wait_ns, 0);		/* NVMe rq_wait_ns 0 */
	}

	spin_lock_init(&ioc->lock);
	timer_setup(&ioc->timer, ioc_timer_fn, 0);	/* NVMe 주기 타이머 핸들러 등록 */
	INIT_LIST_HEAD(&ioc->active_iocgs);	/* NVMe 활성 cgroup 목록 초기화 */

	ioc->running = IOC_IDLE;	/* [한국어] 초기 상태는 idle: 활성 iocg 없을 때 타이머 미실행 */
	ioc->vtime_base_rate = VTIME_PER_USEC;	/* [한국어] vtime_base_rate 초기값: 1.0 (1 vtime/usec) */
	atomic64_set(&ioc->vtime_rate, VTIME_PER_USEC);	/* [한국어] atomic vtime_rate 초기화 */
	seqcount_spinlock_init(&ioc->period_seqcount, &ioc->lock);	/* NVMe 주기 시계 seqcount 초기화 */
	ioc->period_at = ktime_to_us(blk_time_get());	/* [한국어] 현재 시각을 첫 주기 시작점으로 */
	atomic64_set(&ioc->cur_period, 0);	/* NVMe 주기 번호 0 */
	atomic_set(&ioc->hweight_gen, 0);	/* NVMe hweight 캐시 세대 0 */

	spin_lock_irq(&ioc->lock);
	ioc->autop_idx = AUTOP_INVALID;	/* [한국어] autop 인덱스를 무효 상태로 초기화: refresh 강제 실행 */
	ioc_refresh_params_disk(ioc, true, disk);	/* [한국어] disk queue depth 확인 후 autop 프로파일 선택 */
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
 * [한국어]
 * ioc_cpd_alloc - blkcg_policy_data(cpd) 즉 cgroup별 ioc_cgrp 할당
 *
 * @gfp:    메모리 할당 플래그 (blkcg_policy_register 경로에서 GFP_KERNEL)
 * @return: 성공 시 &iocc->cpd, 실패 시 NULL
 *
 * blkcg_policy_iocost가 새 cgroup에 연결될 때 blk-cgroup 코어가 호출한다.
 * ioc_cgrp는 이 cgroup의 모든 장치에 대한 기본 weight(dfl_weight)를 보관한다.
 * cfg_weight를 명시하지 않은 장치는 이 dfl_weight를 사용한다.
 *
 * 실행 컨텍스트: cgroup 생성 경로 (process context).
 *
 * 호출 체인:
 *   blkcg_css_alloc → blkcg_policy_register → [ioc_cpd_alloc]
 */
static struct blkcg_policy_data *ioc_cpd_alloc(gfp_t gfp)
{
	struct ioc_cgrp *iocc;

	iocc = kzalloc_obj(struct ioc_cgrp, gfp);	/* [한국어] cgroup별 ioc_cgrp 구조체 할당 */
	if (!iocc)
		return NULL;

	iocc->dfl_weight = CGROUP_WEIGHT_DFL * WEIGHT_ONE;	/* [한국어] 기본 weight 100 * WEIGHT_ONE으로 초기화 */
	return &iocc->cpd;	/* [한국어] blkcg_policy_data 포인터 반환: container_of로 ioc_cgrp 복원 가능 */
}

/*
 * [한국어]
 * ioc_cpd_free - cgroup별 ioc_cgrp(cpd) 해제
 *
 * @cpd: ioc_cpd_alloc()이 반환한 &iocc->cpd 포인터
 * @return: void
 *
 * cgroup이 소멸할 때 blk-cgroup 코어가 호출한다.
 * container_of()로 ioc_cgrp 포인터를 복원한 뒤 kfree()한다.
 *
 * 호출 체인:
 *   blkcg_css_free → blkcg_policy 콜백 → [ioc_cpd_free]
 */
static void ioc_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(container_of(cpd, struct ioc_cgrp, cpd));	/* [한국어] cpd를 포함하는 ioc_cgrp 전체 해제 */
}

/*
 * [한국어]
 * ioc_pd_alloc - 장치-cgroup 쌍(blkg)별 ioc_gq(policy data) 메모리 할당
 *
 * @disk:  iocost가 활성화된 gendisk (NUMA node 정보 참조)
 * @blkcg: 이 blkg를 소유하는 blk-cgroup
 * @gfp:   메모리 할당 플래그 (GFP_KERNEL 또는 GFP_NOWAIT)
 * @return: 성공 시 &iocg->pd, 실패 시 NULL
 *
 * blkcg_activate_policy() 또는 새 blkg 생성 시 blk-cgroup 코어가 호출한다.
 * ioc_gq는 가변 길이 ancestors[] 배열을 포함해, cgroup 계층 깊이(levels)에
 * 따라 struct_size()로 크기를 계산해 NUMA-aware 할당을 수행한다.
 * per-CPU iocg_pcpu_stat도 함께 할당해 usage_us 등을 lock-free로 누적한다.
 * 실제 초기화는 ioc_pd_init()이 담당한다.
 *
 * 실행 컨텍스트: blkg 생성 경로 (process context; GFP 플래그에 따라 sleep 가능).
 * 에러 경로: NULL 반환 시 blkg 생성 실패 → -ENOMEM으로 전파.
 *
 * 호출 체인:
 *   blkcg_activate_policy / blkg_alloc
 *     → blkcg_policy->pd_alloc_fn → [ioc_pd_alloc]
 */
static struct blkg_policy_data *ioc_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	int levels = blkcg->css.cgroup->level + 1;	/* [한국어] root(0)부터 이 cgroup까지의 계층 수 */
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

	return &iocg->pd;	/* [한국어] blkg_policy_data 포인터 반환; pd_to_iocg()로 역추적 가능 */
}

/*
 * [한국어]
 * ioc_pd_init - 새로 생성된 ioc_gq(policy data)를 현재 ioc 상태에 맞게 초기화
 *
 * @pd: ioc_pd_alloc()이 할당한 blkg_policy_data (&iocg->pd)
 * @return: void
 *
 * blkg가 생성되고 policy data가 연결된 직후 blk-cgroup 코어가 호출한다.
 * ioc_gq의 vtime/done_vtime을 현재 vnow로 설정해 "신규 cgroup이 과거
 * 누적 비용을 떠안지 않도록" 한다. ancestors[] 배열을 blkg 계층을 따라
 * 구성해 hweight 전파 경로를 확립하고, weight_updated()로 초기 weight를
 * 부모 방향으로 전파해 hweight 캐시를 유효 상태로 만든다.
 *
 * 실행 컨텍스트: blkg 생성 경로 (process context).
 *               ioc->lock을 spin_lock_irqsave로 획득.
 * 에러 경로: 없음 (ioc가 NULL이면 skip; q_to_ioc 참조).
 *
 * 호출 체인:
 *   blkg_alloc → blkcg_policy->pd_init_fn → [ioc_pd_init]
 *     → ioc_now, weight_updated → propagate_weights
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
 * [한국어]
 * ioc_pd_free - ioc_gq(policy data) 해제 및 weight 정리
 *
 * @pd: 해제할 blkg_policy_data (&iocg->pd)
 * @return: void
 *
 * blkg가 소멸할 때 blk-cgroup 코어가 호출한다.
 * active_list에 남아있으면 weight=0으로 propagate_weights()를 호출해
 * 부모의 hweight 합산에서 제거하고, active_list에서 탈퇴한다.
 * walk_list/surplus_list가 비어있어야 함을 WARN_ON_ONCE로 검증한다.
 * waitq_timer를 취소해 이미 예약된 wakeup 콜백을 중단한다.
 * 마지막으로 per-CPU 통계와 iocg 구조체를 해제한다.
 *
 * 실행 컨텍스트: blkg 소멸 경로 (process context).
 *               ioc->lock을 spin_lock_irqsave로 획득.
 * 에러 경로: ioc=NULL (초기화 이전 해제)이면 타이머 취소/메모리 해제만 수행.
 *
 * 호출 체인:
 *   blkg_put / blkcg_deactivate_policy
 *     → blkcg_policy->pd_free_fn → [ioc_pd_free]
 *     → propagate_weights, hrtimer_cancel, free_percpu, kfree
 */
static void ioc_pd_free(struct blkg_policy_data *pd)
{
	struct ioc_gq *iocg = pd_to_iocg(pd);	/* [한국어] blkg_policy_data에서 ioc_gq 복원 */
	struct ioc *ioc = iocg->ioc;	/* [한국어] 이 iocg가 속한 NVMe 장치 iocost 컨트롤러 */
	unsigned long flags;

	if (ioc) {	/* [한국어] ioc_pd_init()이 호출된 경우에만 정리 수행 */
		spin_lock_irqsave(&ioc->lock, flags);

		if (!list_empty(&iocg->active_list)) {	/* [한국어] 아직 활성 목록에 있으면 weight 0으로 정리 */
			struct ioc_now now;

			ioc_now(ioc, &now);	/* [한국어] 현재 vtime/vrate 스냅샷 취득 */
			propagate_weights(iocg, 0, 0, false, &now);	/* [한국어] active=0, inuse=0 → 부모 hweight에서 제거 */
			list_del_init(&iocg->active_list);	/* [한국어] active_list에서 안전하게 제거 */
		}

		WARN_ON_ONCE(!list_empty(&iocg->walk_list));	/* [한국어] 주기 타이머 walk 중이면 안 됨 */
		WARN_ON_ONCE(!list_empty(&iocg->surplus_list));	/* [한국어] surplus 분배 중이면 안 됨 */

		spin_unlock_irqrestore(&ioc->lock, flags);

		hrtimer_cancel(&iocg->waitq_timer);	/* NVMe 예산 회복 타이머 취소 */
	}
	free_percpu(iocg->pcpu_stat);	/* [한국어] per-CPU usage_us 통계 메모리 해제 */
	kfree(iocg);	/* [한국어] ioc_gq 구조체(ancestors[] 포함) 해제 */
}

/*
 * [한국어]
 * ioc_pd_stat - blkg 통계 파일(io.stat)에 iocost 통계 항목 출력
 *
 * @pd: 통계를 출력할 blkg_policy_data (&iocg->pd)
 * @s:  출력 대상 seq_file (io.stat 파일 read 경로)
 * @return: void
 *
 * blk-cgroup 통계 수집 경로에서 blkg마다 호출된다.
 * root cgroup(level=0)에서만 "cost.vrate=N.NN" 항목을 출력한다.
 * vrate는 VTIME_PER_USEC 기준 정규화값으로, 1.00이 장치 정격 속도.
 * 모든 cgroup에 대해 "cost.usage=N" (활성 IO 사용 시간, us 단위)을 출력.
 * blkcg_debug_stats가 활성화된 경우 wait/indebt/indelay도 추가 출력한다.
 *
 * 실행 컨텍스트: io.stat 읽기 경로 (process context, sleeping 허용).
 *               ioc->lock 없이 last_stat을 읽으므로 주기 갱신과 약간의 race 가능.
 *
 * 호출 체인:
 *   blkcg_print_stat → blkg_stat_recursive_sum
 *     → blkcg_policy->pd_stat_fn → [ioc_pd_stat] → seq_printf
 */
static void ioc_pd_stat(struct blkg_policy_data *pd, struct seq_file *s)
{
	struct ioc_gq *iocg = pd_to_iocg(pd);	/* [한국어] blkg_policy_data에서 ioc_gq 복원 */
	struct ioc *ioc = iocg->ioc;	/* [한국어] 이 iocg가 속한 NVMe 장치 iocost 컨트롤러 */

	if (!ioc->enabled)	/* [한국어] iocost 미활성 시 통계 항목 미출력 */
		return;

	if (iocg->level == 0) {	/* root cgroup 출력: 전체 NVMe vrate */
		unsigned vp10k = DIV64_U64_ROUND_CLOSEST(
			ioc->vtime_base_rate * 10000,		/* vrate * 10000 */
			VTIME_PER_USEC);		/* 1.0 기준으로 정규화: NVMe 상대 IO 속도 */
		seq_printf(s, " cost.vrate=%u.%02u", vp10k / 100, vp10k % 100);
		/* [한국어] vp10k/100.vp10k%100 형식: 예) 9876 → "98.76" (98.76% 속도) */
	}

	seq_printf(s, " cost.usage=%llu", iocg->last_stat.usage_us);
	/* [한국어] cost.usage: 이 cgroup이 실제 IO를 사용한 시간(us); 주기마다 ioc_timer_fn이 갱신 */

	if (blkcg_debug_stats)	/* [한국어] /sys/kernel/debug/blkcg_debug_stats 활성 시 추가 통계 출력 */
		seq_printf(s, " cost.wait=%llu cost.indebt=%llu cost.indelay=%llu",
			iocg->last_stat.wait_us,		/* [한국어] vtime 예산 부족으로 waitq에서 대기한 시간 */
			iocg->last_stat.indebt_us,		/* [한국어] abs_vdebt>0(부채 상태)으로 진행한 시간 */
			iocg->last_stat.indelay_us);	/* [한국어] use_delay(blkcg_schedule_throttle)로 지연된 시간 */
}

/*
 * [한국어]
 * ioc_weight_prfill - 장치별 명시 weight를 seq_file에 한 줄 출력
 *
 * @sf:  출력 대상 seq_file (weight cgroupfs 파일 read 경로)
 * @pd:  이 blkg의 blkg_policy_data
 * @off: blkcg_print_blkgs가 전달하는 private 오프셋 (미사용)
 * @return: 항상 0 (blkcg_print_blkgs 내부 순회 계속)
 *
 * blkcg_print_blkgs()의 prfill 콜백으로, 각 blkg마다 호출된다.
 * cfg_weight가 0이면(기본값 사용) 출력하지 않는다.
 * 장치 이름과 cfg_weight를 "devname N
" 형식으로 출력한다.
 *
 * 호출 체인:
 *   ioc_weight_show → blkcg_print_blkgs → [ioc_weight_prfill]
 */
static u64 ioc_weight_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			     int off)
{
	const char *dname = blkg_dev_name(pd->blkg);	/* [한국어] blkg가 가리키는 장치 이름 (예: "sda") */
	struct ioc_gq *iocg = pd_to_iocg(pd);	/* [한국어] blkg_policy_data에서 ioc_gq 복원 */

	if (dname && iocg->cfg_weight)	/* [한국어] 장치 이름이 있고 명시 weight가 설정된 경우만 출력 */
		seq_printf(sf, "%s %u\n", dname, iocg->cfg_weight / WEIGHT_ONE);
		/* [한국어] "devname weight
" 형식: WEIGHT_ONE(16)으로 나눠 user-visible 정수 출력 */
	return 0;
}


/*
 * [한국어]
 * ioc_weight_show - weight cgroupfs 파일 read: cgroup의 IO weight 출력
 *
 * @sf: seq_file (cgroup weight 파일 read 경로)
 * @v:  미사용
 * @return: 0 (성공)
 *
 * "default N
"으로 이 cgroup의 기본 weight(dfl_weight)를 출력한 뒤
 * blkcg_print_blkgs()로 장치별 명시 weight(cfg_weight)를 출력한다.
 *
 * 호출 체인:
 *   cgroupfs_read → kernfs_fop_read → seq_read
 *     → cftype.seq_show → [ioc_weight_show]
 *     → blkcg_print_blkgs → ioc_weight_prfill
 */
static int ioc_weight_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));	/* [한국어] seq_file에서 blk-cgroup 포인터 취득 */
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkcg);	/* [한국어] blkcg에서 ioc_cgrp(dfl_weight 보관) 취득 */

	seq_printf(sf, "default %u\n", iocc->dfl_weight / WEIGHT_ONE);
	/* [한국어] dfl_weight를 WEIGHT_ONE으로 나눠 user-visible 정수(100) 출력 */
	blkcg_print_blkgs(sf, blkcg, ioc_weight_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	/* [한국어] 모든 blkg를 순회해 cfg_weight가 있는 장치만 "devname N" 출력 */
	return 0;
}

/*
 * [한국어]
 * ioc_weight_write - weight cgroupfs 파일 write: IO weight 설정
 *
 * @of:     kernfs_open_file (cgroup weight 파일 write 경로)
 * @buf:    사용자 입력 문자열 ("default N" 또는 "devname:N")
 * @nbytes: 입력 바이트 수
 * @off:    파일 오프셋 (미사용)
 * @return: 성공 시 nbytes, 실패 시 음수 errno
 *
 * 두 가지 입력 형식을 처리한다:
 *   1) ':' 없음: "default N" 또는 "N" — dfl_weight를 설정하고
 *      이 cgroup의 모든 blkg에 weight_updated()를 호출해 hweight 재계산.
 *   2) ':' 있음: "devname:N" 또는 "devname:default" — 특정 장치의
 *      cfg_weight를 설정하고 해당 iocg에만 weight_updated() 호출.
 * 유효 범위: CGROUP_WEIGHT_MIN(1) ~ CGROUP_WEIGHT_MAX(10000).
 *
 * 실행 컨텍스트: cgroupfs write 태스크 (process context, sleeping 허용).
 *               blkcg->lock 또는 iocg->ioc->lock으로 직렬화.
 * 에러 경로: 파싱 실패/-EINVAL, blkg_conf_prep 실패 시 errno 반환.
 *
 * 호출 체인:
 *   cgroupfs_write → kernfs_fop_write → cftype.write
 *     → [ioc_weight_write] → weight_updated → propagate_weights
 */
static ssize_t ioc_weight_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));	/* [한국어] 쓰기 대상 blk-cgroup 포인터 취득 */
	struct ioc_cgrp *iocc = blkcg_to_iocc(blkcg);	/* [한국어] 이 cgroup의 ioc_cgrp(dfl_weight 보관) */
	struct blkg_conf_ctx ctx;	/* [한국어] "devname:N" 형식 파싱 컨텍스트 */
	struct ioc_now now;	/* [한국어] weight 갱신 시 현재 vtime/vrate 스냅샷 */
	struct ioc_gq *iocg;	/* [한국어] 특정 장치-cgroup 쌍의 iocost 상태 */
	u32 v;	/* [한국어] 파싱된 weight 값 */
	int ret;

	if (!strchr(buf, ':')) {	/* [한국어] ':' 없으면 전체 cgroup 기본 weight 설정 경로 */
		struct blkcg_gq *blkg;

		if (!sscanf(buf, "default %u", &v) && !sscanf(buf, "%u", &v))
			return -EINVAL;	/* [한국어] "default N" 또는 "N" 외 형식은 거부 */

		if (v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX)
			return -EINVAL;	/* [한국어] weight 범위 검사: 1~10000 */

		spin_lock_irq(&blkcg->lock);	/* [한국어] blkg_list 순회 보호 */
		iocc->dfl_weight = v * WEIGHT_ONE;	/* [한국어] 기본 weight 업데이트 (WEIGHT_ONE=16 단위) */
		hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
			/* [한국어] 이 cgroup의 모든 장치별 blkg에 weight 변경 전파 */
			struct ioc_gq *iocg = blkg_to_iocg(blkg);

			if (iocg) {	/* [한국어] iocost policy가 활성화된 blkg만 처리 */
				spin_lock(&iocg->ioc->lock);
				ioc_now(iocg->ioc, &now);
				weight_updated(iocg, &now);	/* [한국어] hweight 재계산 및 상위 전파 */
				spin_unlock(&iocg->ioc->lock);
			}
		}
		spin_unlock_irq(&blkcg->lock);

		return nbytes;
	}

	blkg_conf_init(&ctx, buf);	/* [한국어] "devname:N" 형식 파싱 컨텍스트 초기화 */

	ret = blkg_conf_prep(blkcg, &blkcg_policy_iocost, &ctx);	/* [한국어] 장치 이름으로 blkg 탐색 및 잠금 */
	if (ret)
		goto err;

	iocg = blkg_to_iocg(ctx.blkg);	/* [한국어] 파싱된 blkg에서 ioc_gq 취득 */

	if (!strncmp(ctx.body, "default", 7)) {	/* [한국어] "default" 키워드면 cfg_weight=0(기본값 사용) */
		v = 0;
	} else {
		if (!sscanf(ctx.body, "%u", &v))
			goto einval;	/* [한국어] 숫자 파싱 실패 */
		if (v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX)
			goto einval;	/* [한국어] 범위 초과 */
	}

	spin_lock(&iocg->ioc->lock);	/* [한국어] 이 장치의 iocost 컨트롤러 lock */
	iocg->cfg_weight = v * WEIGHT_ONE;	/* [한국어] 장치별 명시 weight 설정 */
	ioc_now(iocg->ioc, &now);	/* [한국어] 현재 vtime 스냅샷 */
	weight_updated(iocg, &now);	/* [한국어] hweight 재계산 및 상위 전파 */
	spin_unlock(&iocg->ioc->lock);

	blkg_conf_exit(&ctx);	/* [한국어] blkg 참조 해제 */
	return nbytes;

einval:
	ret = -EINVAL;
err:
	blkg_conf_exit(&ctx);
	return ret;
}

/*
 * [한국어]
 * ioc_qos_prfill - 장치별 QoS 파라미터를 seq_file에 한 줄 출력
 *
 * @sf:  출력 대상 seq_file (io.cost.qos cgroupfs 파일 read 경로)
 * @pd:  이 blkg의 blkg_policy_data
 * @off: blkcg_print_blkgs가 전달하는 private 오프셋 (미사용)
 * @return: 항상 0
 *
 * blkcg_print_blkgs()의 prfill 콜백. 장치별로 한 줄:
 * "devname enable=N ctrl=auto|user rpct=N.NN rlat=N wpct=N.NN wlat=N
 *  min=N.NN max=N.NN"
 * rpct/wpct/min/max는 PPM(100만분율)을 백분율로 변환해 출력.
 * ioc->lock을 짧게 획득해 파라미터 일관성을 보장한다.
 *
 * 호출 체인:
 *   ioc_qos_show → blkcg_print_blkgs → [ioc_qos_prfill]
 */
static u64 ioc_qos_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			  int off)
{
	const char *dname = blkg_dev_name(pd->blkg);	/* [한국어] blkg 장치 이름 (없으면 출력 skip) */
	struct ioc *ioc = pd_to_iocg(pd)->ioc;	/* [한국어] 이 blkg의 iocost 컨트롤러 */

	if (!dname)	/* [한국어] 장치 이름이 없으면 출력 skip */
		return 0;

	spin_lock(&ioc->lock);	/* [한국어] QoS 파라미터 일관성 보장 */
	seq_printf(sf, "%s enable=%d ctrl=%s rpct=%u.%02u rlat=%u wpct=%u.%02u wlat=%u min=%u.%02u max=%u.%02u\n",
		   dname, ioc->enabled, ioc->user_qos_params ? "user" : "auto",
		   ioc->params.qos[QOS_RPPM] / 10000,		/* [한국어] read 달성률 정수부 (PPM/10000) */
		   ioc->params.qos[QOS_RPPM] % 10000 / 100,	/* [한국어] read 달성률 소수점 2자리 */
		   ioc->params.qos[QOS_RLAT],				/* [한국어] read latency 목표 (us) */
		   ioc->params.qos[QOS_WPPM] / 10000,		/* [한국어] write 달성률 정수부 */
		   ioc->params.qos[QOS_WPPM] % 10000 / 100,	/* [한국어] write 달성률 소수점 2자리 */
		   ioc->params.qos[QOS_WLAT],				/* [한국어] write latency 목표 (us) */
		   ioc->params.qos[QOS_MIN] / 10000,		/* [한국어] vrate 하한(min) 정수부 */
		   ioc->params.qos[QOS_MIN] % 10000 / 100,	/* [한국어] vrate 하한 소수점 2자리 */
		   ioc->params.qos[QOS_MAX] / 10000,		/* [한국어] vrate 상한(max) 정수부 */
		   ioc->params.qos[QOS_MAX] % 10000 / 100);	/* [한국어] vrate 상한 소수점 2자리 */
	spin_unlock(&ioc->lock);
	return 0;
}

/*
 * [한국어]
 * ioc_qos_show - io.cost.qos cgroupfs read: 장치별 QoS 파라미터 출력
 *
 * @sf: seq_file (io.cost.qos 파일 read 경로)
 * @v:  미사용
 * @return: 0 (성공)
 *
 * blkcg_print_blkgs()로 모든 장치의 QoS 파라미터를 ioc_qos_prfill로 출력.
 * CFTYPE_ONLY_ON_ROOT 플래그로 root cgroup에서만 읽기 가능.
 *
 * 호출 체인:
 *   cgroupfs_read → cftype.seq_show → [ioc_qos_show]
 *     → blkcg_print_blkgs → ioc_qos_prfill
 */
static int ioc_qos_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));	/* [한국어] seq_file에서 blk-cgroup 포인터 취득 */

	blkcg_print_blkgs(sf, blkcg, ioc_qos_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	/* [한국어] 모든 blkg를 순회해 ioc_qos_prfill로 장치별 QoS 파라미터 출력 */
	return 0;
}

/* [한국어] qos_ctrl_tokens - io.cost.qos 제어 키워드 파싱 테이블 (match_token 사용) */
static const match_table_t qos_ctrl_tokens = {
	{ QOS_ENABLE,		"enable=%u"	},	/* [한국어] enable=0|1: iocost 활성화/비활성화 */
	{ QOS_CTRL,		"ctrl=%s"	},	/* [한국어] ctrl=auto|user: 파라미터 자동/수동 선택 */
	{ NR_QOS_CTRL_PARAMS,	NULL		},	/* [한국어] 파싱 종료 마커 */
};

/* [한국어] qos_tokens - io.cost.qos QoS 파라미터 키워드 파싱 테이블 */
static const match_table_t qos_tokens = {
	{ QOS_RPPM,		"rpct=%s"	},	/* [한국어] read 완료율 목표: 백분율 문자열 (예: "99.50") */
	{ QOS_RLAT,		"rlat=%u"	},	/* [한국어] read latency 목표: 마이크로초 정수 */
	{ QOS_WPPM,		"wpct=%s"	},	/* [한국어] write 완료율 목표: 백분율 문자열 */
	{ QOS_WLAT,		"wlat=%u"	},	/* [한국어] write latency 목표: 마이크로초 정수 */
	{ QOS_MIN,		"min=%s"	},	/* [한국어] vrate 하한: 백분율 문자열 (예: "50.00") */
	{ QOS_MAX,		"max=%s"	},	/* [한국어] vrate 상한: 백분율 문자열 (예: "150.00") */
	{ NR_QOS_PARAMS,	NULL		},	/* [한국어] 파싱 종료 마커 */
};

/*
 * [한국어]
 * ioc_qos_write - io.cost.qos cgroupfs write: QoS 파라미터 및 enable 설정
 *
 * @of:     kernfs_open_file (io.cost.qos 파일 write 경로)
 * @input:  사용자 입력: "devname enable=N ctrl=auto rpct=N.NN rlat=N ..."
 * @nbytes: 입력 바이트 수
 * @off:    파일 오프셋 (미사용)
 * @return: 성공 시 nbytes, 실패 시 음수 errno
 *
 * io.cost.qos에 쓰여진 파라미터를 파싱해 ioc->params.qos에 반영한다.
 * qos_ctrl_tokens(enable, ctrl)와 qos_tokens(rpct, rlat, wpct, wlat, min, max)
 * 두 단계로 파싱한다. enable=1이면 blk_stat_enable_accounting()과
 * QUEUE_FLAG_RQ_ALLOC_TIME을 활성화해 request latency 측정을 시작한다.
 * ctrl=user이면 ioc->user_qos_params=true로 autop 파라미터 덮어쓰기를 막는다.
 * 변경 중 IO race를 막기 위해 blk_mq_quiesce_queue()로 dispatch를 멈춘다.
 * iocost가 켜지면 wbt_disable_default()로 writeback throttling 중복 제어 제거.
 *
 * 실행 컨텍스트: cgroupfs write 태스크 (process context, sleeping 허용).
 *               blk_mq_quiesce_queue 상태에서 ioc->lock 획득.
 * 에러 경로: blkg 탐색 실패, 미지원 큐(non-mq), 파싱 오류 시 -errno 반환.
 *
 * 호출 체인:
 *   cgroupfs_write → cftype.write → [ioc_qos_write]
 *     → blk_iocost_init (첫 번째 활성화 시)
 *     → blk_mq_quiesce_queue, ioc_refresh_params
 *     → wbt_disable_default / wbt_enable_default
 */
static ssize_t ioc_qos_write(struct kernfs_open_file *of, char *input,
			     size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;	/* [한국어] "devname:params" 형식 파싱 컨텍스트 */
	struct gendisk *disk;	/* [한국어] 설정 대상 블록 장치 */
	struct ioc *ioc;	/* [한국어] 장치의 iocost 컨트롤러 */
	u32 qos[NR_QOS_PARAMS];	/* [한국어] 파싱 중인 QoS 파라미터 임시 복사본 */
	bool enable, user;	/* [한국어] enable: iocost 활성 여부, user: 수동 파라미터 여부 */
	char *body, *p;	/* [한국어] 파싱 포인터 */
	unsigned long memflags;	/* [한국어] blkg_conf_open_bdev_frozen 반환 플래그 */
	int ret;

	blkg_conf_init(&ctx, input);	/* [한국어] 입력 문자열로 blkg_conf_ctx 초기화 */

	memflags = blkg_conf_open_bdev_frozen(&ctx);	/* [한국어] bdev를 frozen 상태로 열기 (queue 동결 포함) */
	if (IS_ERR_VALUE(memflags)) {
		ret = memflags;	/* [한국어] bdev 열기 실패 */
		goto err;
	}

	body = ctx.body;	/* [한국어] "devname:" 이후의 파라미터 문자열 */
	disk = ctx.bdev->bd_disk;	/* [한국어] 설정 대상 gendisk */
	if (!queue_is_mq(disk->queue)) {	/* [한국어] iocost는 blk-mq 전용 */
		ret = -EOPNOTSUPP;
		goto err;
	}

	ioc = q_to_ioc(disk->queue);	/* [한국어] 이미 iocost가 초기화된 경우 기존 ioc 재사용 */
	if (!ioc) {	/* [한국어] 최초 io.cost.qos 쓰기 시 iocost 초기화 */
		ret = blk_iocost_init(disk);
		if (ret)
			goto err;
		ioc = q_to_ioc(disk->queue);
	}

	blk_mq_quiesce_queue(disk->queue);	/* NVMe queue 일시 정지: qos 변경 중 race 방지 */

	spin_lock_irq(&ioc->lock);	/* [한국어] QoS 파라미터 변경 직렬화 */
	memcpy(qos, ioc->params.qos, sizeof(qos));	/* [한국어] 현재 파라미터 복사 (파싱 중 수정 가능한 임시 버퍼) */
	enable = ioc->enabled;	/* [한국어] 현재 enable 상태 */
	user = ioc->user_qos_params;	/* [한국어] 현재 수동 파라미터 여부 */

	while ((p = strsep(&body, " \t\n"))) {	/* [한국어] 공백으로 구분된 "key=value" 토큰 순회 */
		substring_t args[MAX_OPT_ARGS];
		char buf[32];
		int tok;
		s64 v;

		if (!*p)	/* [한국어] 빈 토큰 skip */
			continue;

		switch (match_token(p, qos_ctrl_tokens, args)) {	/* [한국어] enable/ctrl 제어 키워드 먼저 파싱 */
		case QOS_ENABLE:	/* [한국어] "enable=0|1" */
			if (match_u64(&args[0], &v))
				goto einval;
			enable = v;	/* [한국어] enable 플래그 임시 저장 */
			continue;
		case QOS_CTRL:	/* [한국어] "ctrl=auto|user" */
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (!strcmp(buf, "auto"))
				user = false;	/* [한국어] autop가 파라미터 선택 */
			else if (!strcmp(buf, "user"))
				user = true;	/* [한국어] 사용자가 파라미터 고정 */
			else
				goto einval;
			continue;
		}

		tok = match_token(p, qos_tokens, args);	/* [한국어] QoS 파라미터 키워드 파싱 */
		switch (tok) {
		case QOS_RPPM:	/* [한국어] "rpct=N.NN": read 완료율 목표 (PPM = 백분율 * 10000) */
		case QOS_WPPM:	/* [한국어] "wpct=N.NN": write 완료율 목표 */
			if (match_strlcpy(buf, &args[0], sizeof(buf)) >=
			    sizeof(buf))
				goto einval;
			if (cgroup_parse_float(buf, 2, &v))	/* [한국어] "99.50" → 9950 */
				goto einval;
			if (v < 0 || v > 10000)	/* [한국어] 0.00%~100.00% 범위 */
				goto einval;
			qos[tok] = v * 100;	/* [한국어] 백분율 × 100 = PPM */
			break;
		case QOS_RLAT:	/* [한국어] "rlat=N": read latency 목표 (us) */
		case QOS_WLAT:	/* [한국어] "wlat=N": write latency 목표 (us) */
			if (match_u64(&args[0], &v))
				goto einval;
			qos[tok] = v;	/* [한국어] 마이크로초 그대로 저장 */
			break;
		case QOS_MIN:	/* [한국어] "min=N.NN": vrate 하한 백분율 */
		case QOS_MAX:	/* [한국어] "max=N.NN": vrate 상한 백분율 */
			if (match_strlcpy(buf, &args[0], sizeof(buf)) >=
			    sizeof(buf))
				goto einval;
			if (cgroup_parse_float(buf, 2, &v))
				goto einval;
			if (v < 0)	/* [한국어] 음수 vrate 범위 거부 */
				goto einval;
			qos[tok] = clamp_t(s64, v * 100,
					   VRATE_MIN_PPM, VRATE_MAX_PPM);
			/* [한국어] VRATE_MIN_PPM~VRATE_MAX_PPM 범위로 클램핑 */
			break;
		default:
			goto einval;	/* [한국어] 인식 불가 키워드 */
		}
		user = true;	/* [한국어] 어떤 파라미터든 명시되면 user 모드로 전환 */
	}

	if (qos[QOS_MIN] > qos[QOS_MAX])	/* [한국어] min > max는 유효하지 않은 vrate 범위 */
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

	if (user) {	/* [한국어] 수동 모드: 파싱된 파라미터를 ioc에 반영 */
		memcpy(ioc->params.qos, qos, sizeof(qos));
		ioc->user_qos_params = true;
	} else {	/* [한국어] auto 모드: ioc_refresh_params가 autop 프로파일로 덮어씀 */
		ioc->user_qos_params = false;
	}

	ioc_refresh_params(ioc, true);	/* [한국어] 새 파라미터로 lcoefs/margins 재계산 */
	spin_unlock_irq(&ioc->lock);

	if (enable)	/* iocost 켜지면 wbt는 중복 제어이므로 NVMe writeback throttling 비활성화 */
		wbt_disable_default(disk);		/* wbt 중복 제거: NVMe latency QoS 제어 단일화 */
	else
		wbt_enable_default(disk);		/* wbt 복원: NVMe 제어 없을 때 writeback 조절 */

	blk_mq_unquiesce_queue(disk->queue);	/* NVMe queue 재개: qos 변경 완료 */

	blkg_conf_exit_frozen(&ctx, memflags);	/* [한국어] bdev/frozen 상태 해제 */
	return nbytes;
einval:
	spin_unlock_irq(&ioc->lock);
	blk_mq_unquiesce_queue(disk->queue);
	ret = -EINVAL;
err:
	blkg_conf_exit_frozen(&ctx, memflags);
	return ret;
}

/*
 * [한국어]
 * ioc_cost_model_prfill - 장치별 비용 모델 파라미터를 seq_file에 한 줄 출력
 *
 * @sf:  출력 대상 seq_file (io.cost.model cgroupfs 파일 read 경로)
 * @pd:  이 blkg의 blkg_policy_data
 * @off: blkcg_print_blkgs가 전달하는 private 오프셋 (미사용)
 * @return: 항상 0
 *
 * blkcg_print_blkgs()의 prfill 콜백. 장치별로 한 줄:
 * "devname ctrl=auto|user model=linear rbps=N rseqiops=N rrandiops=N
 *  wbps=N wseqiops=N wrandiops=N"
 * i_lcoefs[]는 ioc_refresh_params()가 autop 또는 사용자 설정으로 계산한 값.
 *
 * 호출 체인:
 *   ioc_cost_model_show → blkcg_print_blkgs → [ioc_cost_model_prfill]
 */
static u64 ioc_cost_model_prfill(struct seq_file *sf,
				 struct blkg_policy_data *pd, int off)
{
	const char *dname = blkg_dev_name(pd->blkg);	/* [한국어] 장치 이름 (없으면 출력 skip) */
	struct ioc *ioc = pd_to_iocg(pd)->ioc;	/* [한국어] 이 blkg의 iocost 컨트롤러 */
	u64 *u = ioc->params.i_lcoefs;	/* [한국어] 사용자 정의 또는 autop 비용 계수 배열 */

	if (!dname)	/* [한국어] 장치 이름이 없으면 출력 skip */
		return 0;

	spin_lock(&ioc->lock);	/* [한국어] i_lcoefs 일관성 보장 */
	seq_printf(sf, "%s ctrl=%s model=linear "
		   "rbps=%llu rseqiops=%llu rrandiops=%llu "
		   "wbps=%llu wseqiops=%llu wrandiops=%llu\n",
		   dname, ioc->user_cost_model ? "user" : "auto",
		   u[I_LCOEF_RBPS],		/* [한국어] read 순차 대역폭 (bytes/sec) */
		   u[I_LCOEF_RSEQIOPS],	/* [한국어] read 순차 IOPS */
		   u[I_LCOEF_RRANDIOPS],	/* [한국어] read 랜덤 IOPS */
		   u[I_LCOEF_WBPS],		/* [한국어] write 순차 대역폭 (bytes/sec) */
		   u[I_LCOEF_WSEQIOPS],	/* [한국어] write 순차 IOPS */
		   u[I_LCOEF_WRANDIOPS]);	/* [한국어] write 랜덤 IOPS */
	spin_unlock(&ioc->lock);
	return 0;
}

/*
 * [한국어]
 * ioc_cost_model_show - io.cost.model cgroupfs read: 비용 모델 파라미터 출력
 *
 * @sf: seq_file (io.cost.model 파일 read 경로)
 * @v:  미사용
 * @return: 0 (성공)
 *
 * blkcg_print_blkgs()로 모든 장치의 비용 모델 파라미터를
 * ioc_cost_model_prfill로 출력한다. CFTYPE_ONLY_ON_ROOT.
 *
 * 호출 체인:
 *   cgroupfs_read → cftype.seq_show → [ioc_cost_model_show]
 *     → blkcg_print_blkgs → ioc_cost_model_prfill
 */
static int ioc_cost_model_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));	/* [한국어] seq_file에서 blk-cgroup 포인터 취득 */

	blkcg_print_blkgs(sf, blkcg, ioc_cost_model_prfill,
			  &blkcg_policy_iocost, seq_cft(sf)->private, false);
	/* [한국어] 모든 blkg를 순회해 비용 모델 파라미터 출력 */
	return 0;
}

/* [한국어] cost_ctrl_tokens - io.cost.model 제어 키워드 파싱 테이블 */
static const match_table_t cost_ctrl_tokens = {
	{ COST_CTRL,		"ctrl=%s"	},	/* [한국어] ctrl=auto|user: 비용 모델 자동/수동 선택 */
	{ COST_MODEL,		"model=%s"	},	/* [한국어] model=linear: 현재 선형 모델만 지원 */
	{ NR_COST_CTRL_PARAMS,	NULL		},	/* [한국어] 파싱 종료 마커 */
};

/* [한국어] i_lcoef_tokens - io.cost.model 비용 계수(i_lcoefs) 키워드 파싱 테이블 */
static const match_table_t i_lcoef_tokens = {
	{ I_LCOEF_RBPS,		"rbps=%u"	},	/* [한국어] read 순차 대역폭 목표 (bytes/sec) */
	{ I_LCOEF_RSEQIOPS,	"rseqiops=%u"	},	/* [한국어] read 순차 IOPS 목표 */
	{ I_LCOEF_RRANDIOPS,	"rrandiops=%u"	},	/* [한국어] read 랜덤 IOPS 목표 */
	{ I_LCOEF_WBPS,		"wbps=%u"	},	/* [한국어] write 순차 대역폭 목표 (bytes/sec) */
	{ I_LCOEF_WSEQIOPS,	"wseqiops=%u"	},	/* [한국어] write 순차 IOPS 목표 */
	{ I_LCOEF_WRANDIOPS,	"wrandiops=%u"	},	/* [한국어] write 랜덤 IOPS 목표 */
	{ NR_I_LCOEFS,		NULL		},	/* [한국어] 파싱 종료 마커 */
};

/*
 * [한국어]
 * ioc_cost_model_write - io.cost.model cgroupfs write: 비용 모델 파라미터 설정
 *
 * @of:     kernfs_open_file (io.cost.model 파일 write 경로)
 * @input:  사용자 입력: "devname ctrl=user model=linear rbps=N rseqiops=N ..."
 * @nbytes: 입력 바이트 수
 * @off:    파일 오프셋 (미사용)
 * @return: 성공 시 nbytes, 실패 시 음수 errno
 *
 * io.cost.model에 쓰여진 비용 모델 파라미터를 파싱해 ioc->params.i_lcoefs에
 * 반영한다. cost_ctrl_tokens(ctrl, model)와 i_lcoef_tokens(rbps~wrandiops)
 * 두 단계로 파싱한다. ctrl=user이면 ioc->user_cost_model=true로 설정해
 * autop가 i_lcoefs를 덮어쓰지 않게 한다. model=linear만 지원한다.
 * 변경 중 IO 일관성을 위해 blk_mq_freeze_queue + blk_mq_quiesce_queue로
 * queue를 완전히 정지한다 (qos_write보다 강력한 동결).
 *
 * 실행 컨텍스트: cgroupfs write 태스크 (process context, sleeping 허용).
 * 에러 경로: 미지원 큐, 파싱 오류, iocost 초기화 실패 시 -errno 반환.
 *
 * 호출 체인:
 *   cgroupfs_write → cftype.write → [ioc_cost_model_write]
 *     → blk_iocost_init (첫 번째 활성화 시)
 *     → blk_mq_freeze_queue, blk_mq_quiesce_queue
 *     → ioc_refresh_params → lcoefs 재계산
 */
static ssize_t ioc_cost_model_write(struct kernfs_open_file *of, char *input,
				    size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;	/* [한국어] "devname:params" 형식 파싱 컨텍스트 */
	struct request_queue *q;	/* [한국어] 설정 대상 블록 장치의 request_queue */
	unsigned int memflags;	/* [한국어] blk_mq_freeze_queue 반환 플래그 (freeze depth 등) */
	struct ioc *ioc;	/* [한국어] 장치의 iocost 컨트롤러 */
	u64 u[NR_I_LCOEFS];	/* [한국어] 파싱 중인 i_lcoefs 임시 복사본 */
	bool user;	/* [한국어] true이면 수동 비용 모델, false이면 autop 자동 선택 */
	char *body, *p;	/* [한국어] 파싱 포인터 */
	int ret;

	blkg_conf_init(&ctx, input);	/* [한국어] 입력 문자열로 blkg_conf_ctx 초기화 */

	ret = blkg_conf_open_bdev(&ctx);	/* [한국어] bdev 열기 (frozen 없음; freeze는 blk_mq_freeze_queue로) */
	if (ret)
		goto err;

	body = ctx.body;	/* [한국어] "devname:" 이후의 파라미터 문자열 */
	q = bdev_get_queue(ctx.bdev);	/* [한국어] bdev에서 request_queue 취득 */
	if (!queue_is_mq(q)) {	/* [한국어] iocost는 blk-mq 전용 */
		ret = -EOPNOTSUPP;
		goto err;
	}

	ioc = q_to_ioc(q);	/* [한국어] 기존 iocost 컨트롤러 탐색 */
	if (!ioc) {	/* [한국어] 최초 io.cost.model 쓰기 시 iocost 초기화 */
		ret = blk_iocost_init(ctx.bdev->bd_disk);
		if (ret)
			goto err;
		ioc = q_to_ioc(q);
	}

	memflags = blk_mq_freeze_queue(q);	/* NVMe queue 동결: cost model 변경 중 IO 정지 */
	blk_mq_quiesce_queue(q);	/* NVMe queue 휴양: hctx dispatch 중단 */

	spin_lock_irq(&ioc->lock);	/* [한국어] i_lcoefs 변경 직렬화 */
	memcpy(u, ioc->params.i_lcoefs, sizeof(u));	/* [한국어] 현재 i_lcoefs를 임시 버퍼에 복사 */
	user = ioc->user_cost_model;	/* [한국어] 현재 수동 모델 여부 */

	while ((p = strsep(&body, " \t\n"))) {	/* [한국어] 공백으로 구분된 토큰 순회 */
		substring_t args[MAX_OPT_ARGS];
		char buf[32];
		int tok;
		u64 v;

		if (!*p)	/* [한국어] 빈 토큰 skip */
			continue;

		switch (match_token(p, cost_ctrl_tokens, args)) {	/* [한국어] ctrl/model 제어 키워드 파싱 */
		case COST_CTRL:	/* [한국어] "ctrl=auto|user" */
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (!strcmp(buf, "auto"))
				user = false;	/* [한국어] autop가 i_lcoefs 자동 계산 */
			else if (!strcmp(buf, "user"))
				user = true;	/* [한국어] 사용자 지정 i_lcoefs 고정 */
			else
				goto einval;
			continue;
		case COST_MODEL:	/* [한국어] "model=linear" — 현재 linear만 지원 */
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (strcmp(buf, "linear"))
				goto einval;	/* [한국어] linear 외 모델은 거부 */
			continue;
		}

		tok = match_token(p, i_lcoef_tokens, args);	/* [한국어] rbps/rseqiops/... 파라미터 파싱 */
		if (tok == NR_I_LCOEFS)	/* [한국어] 인식 불가 키워드 */
			goto einval;
		if (match_u64(&args[0], &v))	/* [한국어] u64 값 파싱 실패 */
			goto einval;
		u[tok] = v;	/* [한국어] 해당 i_lcoef 인덱스에 값 저장 */
		user = true;	/* [한국어] 어떤 계수든 명시되면 user 모드로 전환 */
	}

	if (user) {	/* [한국어] 수동 모드: 파싱된 i_lcoefs를 ioc에 반영 */
		memcpy(ioc->params.i_lcoefs, u, sizeof(u));
		ioc->user_cost_model = true;
	} else {	/* [한국어] auto 모드: ioc_refresh_params가 autop로 덮어씀 */
		ioc->user_cost_model = false;
	}
	ioc_refresh_params(ioc, true);	/* [한국어] 새 i_lcoefs로 lcoefs/vtime 계수 재계산 */
	spin_unlock_irq(&ioc->lock);

	blk_mq_unquiesce_queue(q);	/* NVMe queue 재개 */
	blk_mq_unfreeze_queue(q, memflags);	/* NVMe queue 동결 해제 */

	blkg_conf_exit(&ctx);	/* [한국어] blkg 참조 해제 */
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

/*
 * [한국어]
 * ioc_files - iocost가 cgroupfs에 노출하는 파일(cftype) 배열
 *
 * blkcg_policy_iocost.dfl_cftypes에 연결되어 blk-cgroup 정책 활성화 시
 * 각 cgroup 디렉토리에 파일이 생성된다.
 * CFTYPE_NOT_ON_ROOT: root cgroup 제외 (weight는 non-root에서만 의미 있음)
 * CFTYPE_ONLY_ON_ROOT: root cgroup 전용 (장치별 QoS/비용 모델은 root에서 설정)
 */
static struct cftype ioc_files[] = {
	{
		/* [한국어] io.cost.weight: 이 cgroup의 IO weight(1~10000) 읽기/쓰기 */
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,	/* [한국어] root cgroup에는 없음 (root는 100% 사용) */
		.seq_show = ioc_weight_show,	/* [한국어] "default N
devname N
" 형식 출력 */
		.write = ioc_weight_write,	/* [한국어] "default N" 또는 "devname:N" 파싱 후 적용 */
	},
	{
		/* [한국어] io.cost.qos: 장치별 latency QoS 목표 및 enable 읽기/쓰기 */
		.name = "cost.qos",
		.flags = CFTYPE_ONLY_ON_ROOT,	/* [한국어] root cgroup 전용: 장치 전체에 영향 */
		.seq_show = ioc_qos_show,	/* [한국어] "devname enable=N ctrl=... rpct=... rlat=..." 출력 */
		.write = ioc_qos_write,		/* [한국어] QoS 파라미터 파싱 후 ioc->params.qos 업데이트 */
	},
	{
		/* [한국어] io.cost.model: 장치별 비용 모델 파라미터(rbps 등) 읽기/쓰기 */
		.name = "cost.model",
		.flags = CFTYPE_ONLY_ON_ROOT,	/* [한국어] root cgroup 전용: 장치 전체에 영향 */
		.seq_show = ioc_cost_model_show,	/* [한국어] "devname ctrl=... model=linear rbps=..." 출력 */
		.write = ioc_cost_model_write,		/* [한국어] i_lcoefs 파싱 후 ioc_refresh_params 호출 */
	},
	{}	/* [한국어] cftype 배열 종료 마커 */
};

/*
 * [한국어]
 * blkcg_policy_iocost - iocost blk-cgroup 정책 디스크립터
 *
 * blkcg_policy_register()에 전달되어 blk-cgroup 시스템에 iocost를 등록한다.
 * 이 구조체의 콜백들을 통해 cgroup 계층과 장치별 policy data(pd)가 관리된다.
 */
static struct blkcg_policy blkcg_policy_iocost = {
	.dfl_cftypes	= ioc_files,
	/* [한국어] cgroupfs에 등록할 파일 목록 (io.cost.weight, io.cost.qos, io.cost.model) */
	.cpd_alloc_fn	= ioc_cpd_alloc,
	/* [한국어] 새 cgroup 생성 시 ioc_cgrp(dfl_weight 보관) 할당 */
	.cpd_free_fn	= ioc_cpd_free,
	/* [한국어] cgroup 소멸 시 ioc_cgrp 해제 */
	.pd_alloc_fn	= ioc_pd_alloc,
	/* [한국어] 새 blkg(장치-cgroup 쌍) 생성 시 ioc_gq 할당 */
	.pd_init_fn	= ioc_pd_init,
	/* [한국어] blkg 초기화 시 ioc_gq의 vtime/ancestors/weight 설정 */
	.pd_free_fn	= ioc_pd_free,
	/* [한국어] blkg 소멸 시 ioc_gq 정리 및 해제 */
	.pd_stat_fn	= ioc_pd_stat,
	/* [한국어] io.stat 읽기 시 cost.vrate/cost.usage 등 통계 출력 */
};

/*
 * [한국어]
 * ioc_init - iocost 모듈 초기화: blk-cgroup 정책 등록
 *
 * @return: 0 성공, 음수 errno 실패
 *
 * 커널 모듈 로드(또는 빌트인 초기화) 시 호출된다.
 * blkcg_policy_register()로 blkcg_policy_iocost를 blk-cgroup 시스템에
 * 등록해 io.cost.weight/qos/model 파일과 pd 콜백을 활성화한다.
 *
 * 호출 체인:
 *   module_init → [ioc_init] → blkcg_policy_register
 */
static int __init ioc_init(void)
{
	return blkcg_policy_register(&blkcg_policy_iocost);
	/* [한국어] blk-cgroup 정책 등록: 이후 io.cost.qos 쓰기로 장치별 활성화 */
}

/*
 * [한국어]
 * ioc_exit - iocost 모듈 종료: blk-cgroup 정책 해제
 * @return: void
 *
 * 커널 모듈 언로드 시 호출된다.
 * blkcg_policy_unregister()로 등록된 정책을 제거하고,
 * 모든 장치의 ioc_rqos_exit()와 ioc_pd_free()를 연쇄 호출한다.
 *
 * 호출 체인:
 *   module_exit → [ioc_exit] → blkcg_policy_unregister
 *     → ioc_rqos_exit (장치별) → ioc_pd_free (blkg별)
 */
static void __exit ioc_exit(void)
{
	blkcg_policy_unregister(&blkcg_policy_iocost);
	/* [한국어] 정책 해제: 모든 장치/cgroup의 iocost 자원 정리 연쇄 호출 */
}

module_init(ioc_init);	/* [한국어] 커널 모듈 로드 시 ioc_init 자동 호출 등록 */
module_exit(ioc_exit);	/* [한국어] 커널 모듈 언로드 시 ioc_exit 자동 호출 등록 */

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
