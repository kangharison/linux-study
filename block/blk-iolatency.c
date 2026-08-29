// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] per-cgroup IO 지연 QoS 제어 (blk-iolatency.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux block layer의 rq-qos(Request Queue QoS) 프레임워크 위에서
 * cgroup 계층별 IO 완료 지연(latency)을 측정하고 목표 지연을 초과할 경우
 * max_depth(큐 깊이)를 줄이거나 사용자 공간 반환을 인위적으로 지연시켜
 * IO 처리량을 조절하는 구현체이다. 사용자는 cgroupfs의 io.latency 파일에
 * "devmaj:devmin target=<usec>" 형식으로 특정 블록 장치에 대한 지연 목표를
 * cgroup 단위로 설정하며, 이 파일이 런타임에 목표를 집행한다.
 *
 * === 목표 지연을 지키는 방식 ===
 * 핵심은 "위반한 cgroup을 조이는 것"이 아니라 "위반한 cgroup의 경쟁자를
 * 조이는 것"이다. io.latency는 상한(limit)이 아니라 보호(protection) 장치라서,
 * A가 목표를 못 지키면 A를 더 느리게 만들 이유가 없다 — 큐를 함께 쓰는
 * 다른 그룹이 물러나야 A가 목표를 회복한다. 그래서 위반이 감지되면 자기
 * max_depth를 건드리는 게 아니라, 부모가 들고 있는 공유 카운터
 * child_latency_info.scale_cookie를 한 단계 내린다. 같은 부모 아래 모든
 * 자식은 submit 때마다 check_scale_change()에서 이 쿠키를 자기 로컬 사본과
 * 비교하고, 내려갔으면 각자 max_depth를 절반으로 줄인다(올라갔으면 qd/16씩
 * 늘린다). 절반씩 줄이고 1/16씩 늘리는 비대칭은 의도된 것으로, 위반에는
 * 즉시 반응하고 회복은 천천히 해서 스로틀이 진동하지 않게 한다.
 * 쿠키를 내린 그룹은 scale_grp/scale_lat에 "이번 스로틀의 책임자"로 기록되며,
 * 그 책임자가 다시 목표를 만족하기 전까지는 아무도 쿠키를 올릴 수 없다.
 * max_depth가 1까지 내려가도 부족하면 두 번째 수단으로 넘어간다: 그 그룹의
 * IO가 우선순위 역전을 피하려고 root cgroup 명의로 발행되는 경우(REQ_META,
 * REQ_SWAP)에는 depth를 줄여 봐야 소용이 없으므로, blkcg_add_delay()로
 * "빚"을 쌓아 두었다가 그 태스크가 유저스페이스로 돌아갈 때 실제로 재운다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * block layer IO 흐름에서 submit_bio -> blk_mq_submit_bio -> rq_qos_throttle()
 * 단계에서 blkcg_iolatency_throttle()이 호출된다. request가 할당되기도 전,
 * bio 단계에서 개입하므로 여기서 재우는 태스크는 태그나 드라이버 자원을
 * 붙들고 있지 않다. bio 완료 후 rq_qos_done_bio() -> blkcg_iolatency_done_bio()
 * 가 호출되어 지연을 측정하고 통계를 갱신한다. 두 콜백 모두 bio->bi_blkg에서
 * 시작해 root까지 계층을 거슬러 올라가며 각 노드에 대해 같은 작업을 반복한다 —
 * 목표가 설정된 노드가 계층 어디에 있든 통계가 누락되지 않게 하기 위해서다.
 * 실행 컨텍스트: submit 경로는 사용자 태스크 컨텍스트(sleep 가능),
 * done_bio는 softirq/IRQ 컨텍스트일 수 있어 sleep 금지.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-rq-qos.h: rq_qos, rq_qos_ops, rq_qos_wait() — QoS 프레임워크 기반.
 *   blk-wbt.c와 동일한 레이어에서 동작하며 rq_qos_add()로 등록된다.
 * - block/blk-cgroup.h: blkcg_gq, blkg_policy_data, blkcg_use_delay(),
 *   blkcg_add_delay() — cgroup 계층 탐색과 메모리 딜레이 어카운팅 연결.
 * - block/blk-stat.h: blk_rq_stat — HDD 모드에서 평균/분산 지연 통계.
 * - linux/memcontrol.h: blkcg_schedule_throttle() — swap/meta IO 시 memory.delay
 *   cgroup에 지연 예약해 우선순위 역전 없이 root가 대신 발행한 IO를 조절.
 * - linux/sched/loadavg.h: calc_load() — CPU load average와 동일한 지수이동평균
 *   알고리즘으로 HDD IO 지연 장기 추세를 추적.
 * - 데이터 흐름: bio → (submit 경로에서 blkcg_iolatency_throttle이 max_depth 체크)
 *   → 장치 발행/완료 → blkcg_iolatency_done_bio → per-CPU 지연 통계 갱신
 *   → (윈도우 만료 시) iolatency_check_latencies → 부모 scale_cookie 조정
 *   → 형제 cgroup들의 check_scale_change → 각자 max_depth 조정.
 *
 * === 주요 함수/구조체 요약 ===
 * blkcg_iolatency_throttle()  — bio 제출 시 cgroup 계층을 순회하며 max_depth 대기.
 * blkcg_iolatency_done_bio()  — bio 완료 시 지연 기록, 윈도우 평가, inflight 감소.
 * iolatency_check_latencies() — 윈도우 단위 per-CPU 통계 집계 및 scale_cookie 갱신.
 * check_scale_change()        — 부모 scale_cookie 변화를 감지해 max_depth 조정.
 * blkiolatency_timer_fn()     — 1초 타이머로 스로틀 상태 점진적 회복.
 * struct iolatency_grp        — cgroup별 지연 추적 객체 (pd, stats, max_depth, rq_wait).
 * struct blk_iolatency        — 큐 전체 QoS 인스턴스 (rqos, timer, enabled).
 * struct child_latency_info   — 부모가 자식들의 scale 방향을 조율하는 공유 상태.
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
#include <linux/kernel.h> /* [한국어] 커널 기본 타입/매크로(min/max, u64, div64_u64 등). 이 파일의 통계 계산이 64비트 나눗셈을 쓴다 */
#include <linux/blk_types.h> /* [한국어] bio, request op 플래그(REQ_SWAP/REQ_META 등) 정의. 이 bio가 어떤 특성의
			       * IO인지(스왑/메타데이터) 판별하는 데 사용 */
#include <linux/backing-dev.h> /* [한국어] backing_dev_info 관련 정의. block layer의 writeback/dirty 상태
				 * 인프라와 연동되는 공용 헤더(이 파일에서는 간접 의존) */
#include <linux/module.h> /* [한국어] module_init/module_exit, MODULE_LICENSE 등 커널 모듈 등록 매크로.
			    * 이 정책이 빌트인/모듈 어느 쪽으로 빌드되든 초기화 진입점을 등록하기 위함 */
#include <linux/timer.h> /* [한국어] timer_list, timer_setup() 등. blkiolatency_timer_fn 기반 1초 회복
			   * 타이머 구현에 필요 */
#include <linux/memcontrol.h> /* [한국어] blkcg_use_delay/blkcg_add_delay/blkcg_schedule_throttle. IO
				* 지연을 memory.delay(memcg) 서브시스템과 연결하는 데 필요 */
#include <linux/sched/loadavg.h> /* [한국어] calc_load(). HDD 모드 장기 평균 지연(lat_avg) 계산에 CPU load
				   * average와 동일한 지수 이동 평균 알고리즘을 재사용하기 위함 */
#include <linux/sched/signal.h> /* [한국어] fatal_signal_pending(). OOM kill 등으로 죽어가는 태스크는
				  * 스로틀을 우회시켜 회복을 지연시키지 않기 위해 필요 */
#include <trace/events/block.h> /* [한국어] 블록 계층 tracepoint 정의. 이 파일이 직접 트레이스 이벤트를
				  * 발생시키지는 않지만 block core 공통 헤더로 포함된다 */
#include <linux/blk-mq.h> /* [한국어] blk_mq_freeze_queue/unfreeze_queue, hctx 관련 정의. enabled 플래그
			    * 토글 시 큐를 안전하게 멈추기 위해 필요 */
#include "blk-rq-qos.h" /* [한국어] rq_qos_ops(.throttle/.done_bio/.exit)와 rq_wait/rq_qos_wait().
			 * 이 파일은 rq_qos 체인의 한 노드로 등록된다 */
#include "blk-stat.h" /* [한국어] blk_rq_stat — 회전식 미디어 모드에서 평균/표본수 기반 지연 통계에 사용 */
#include "blk-cgroup.h" /* [한국어] blkcg_gq(= (cgroup, 디스크) 쌍)와 blkg_policy_data.
			 * 계층을 root까지 거슬러 올라가는 순회의 기반 자료구조 */
#include "blk.h" /* [한국어] blk_queue_rot()(회전식 여부 판별 → ssd 플래그 결정),
		 * nr_requests 등 큐 상태 접근 */

#define DEFAULT_SCALE_COOKIE 1000000U
/* DEFAULT_SCALE_COOKIE: scale_cookie의 기본값이자 상한. 이 값이면 스로틀이 전혀
 * 걸려 있지 않은 상태이고, 여기서 얼마나 내려와 있는지(DEFAULT - 현재값)가
 * 곧 "현재 얼마나 조여져 있는가"의 척도로 쓰인다. 100만이라는 큰 값을 쓰는
 * 이유는 qd/16 같은 작은 폭으로도 여러 단계를 표현할 해상도가 필요해서다. */

static struct blkcg_policy blkcg_policy_iolatency; /* [한국어] blkcg 정책 등록자. cgroup 계층 각 노드에 iolatency_grp을 붙이는 주체 */
struct iolatency_grp; /* [한국어] 전방 선언 — (cgroup, 디스크) 쌍마다 하나씩 존재하는 지연 제어 객체 */

struct blk_iolatency {
	struct rq_qos rqos;
	/* [한국어] rq-qos(Request Queue QoS) 프레임워크의 기반 구조체(block/blk-rq-qos.h).
	 * 설정자: blk_iolatency_init()에서 rq_qos_add(RQ_QOS_LATENCY, ...)로
	 *         등록하며 이때 rqos.disk/rqos.ops가 채워진다.
	 * 읽는 자: BLKIOLATENCY() 매크로가 container_of()로 이 필드 주소를 이용해
	 *          rq_qos 포인터로부터 blk_iolatency 전체 구조체를 역참조한다.
	 *          submit 경로(blkcg_iolatency_throttle)와 완료 경로
	 *          (blkcg_iolatency_done_bio)에서 매 bio마다 이 경로로 접근한다.
	 * 값 범위: rq_qos_add() 성공 이후 유효. .throttle/.done_bio/.exit 콜백이
	 *          blkcg_iolatency_ops로 고정되어 submit_bio 경로에서
	 *          훅 지점을 제공한다. 실제 completion은 bio 계층에서 일어나므로,
	 *          bio 완료 처리 경로와 연결된다.
	 * 동기화: 큐(disk) 생명주기와 함께 관리되며, rq_qos 리스트 자체의 조작은
	 *         q->rq_qos_mutex로 보호된다(blk-rq-qos.c 참고). */
	struct timer_list timer;
	/* [한국어] 1초(HZ) 주기로 만료되는 scale_cookie 회복 타이머.
	 * 설정자: blk_iolatency_init()에서 timer_setup(&timer, blkiolatency_timer_fn, 0)
	 *         으로 등록하고, blkcg_iolatency_throttle()이 스로틀을 수행할 때마다
	 *         mod_timer(jiffies + HZ)로 재무장한다.
	 * 읽는 자/실행자: blkiolatency_timer_fn()이 만료 시 softirq 타이머 컨텍스트에서
	 *               실행되어 root부터 모든 자식 blkcg_gq를 순회하며 scale_cookie를
	 *               점진적으로 회복시킨다.
	 * 값 범위: pending/미pending 두 상태를 오간다. IO 활동이 뜸해 submit 경로가
	 *          호출되지 않는 동안에도 이 타이머가 유일한 scale 회복 트리거가 된다.
	 * 동기화: blkcg_iolatency_exit()에서 timer_shutdown_sync()로 완전히 종료
	 *         (진행 중인 콜백 완료까지 대기)한 뒤에야 blkiolat 메모리를 해제한다. */

	/*
	 * ->enabled is the master enable switch gating the throttling logic and
	 * inflight tracking. The number of cgroups which have iolat enabled is
	 * tracked in ->enable_cnt, and ->enable is flipped on/off accordingly
	 * from ->enable_work with the request_queue frozen. For details, See
	 * blkiolatency_enable_work_fn().
	 */
	bool enabled;
	/* [한국어] iolatency 마스터 활성화 스위치.
	 * 설정자: blkiolatency_enable_work_fn()이 queue를 freeze한 상태에서 토글.
	 *         enable_cnt > 0이면 true, == 0이면 false.
	 * 읽는 자: blkcg_iolatency_throttle()과 blkcg_iolatency_done_bio()에서
	 *          IO마다 확인하여 false면 즉시 통과/무시.
	 * 값 범위: true(추적 활성) / false(추적 비활성).
	 * 동기화: queue freeze 중에만 변경하므로, 변경 중에 inflight IO가 없어
	 *         inflight 카운트 누수가 발생하지 않는다. */
	atomic_t enable_cnt;
	/* [한국어] io.latency가 활성화된 cgroup(min_lat_nsec > 0)의 수.
	 * 설정자: iolatency_set_min_lat_nsec()에서 target 설정 시 atomic_inc,
	 *         target 해제 시 atomic_dec.
	 * 읽는 자: blkiolatency_enable_work_fn()에서 enabled 플래그 전환 여부 결정.
	 * 값 범위: 0 이상 정수. 0이 되면 enable_work를 통해 enabled=false.
	 * 동기화: atomic_t — 다중 cgroup에서 동시에 target을 설정/해제할 때 안전. */
	struct work_struct enable_work;
	/* [한국어] enabled 플래그 토글을 위한 workqueue 작업.
	 * 설정자: iolatency_set_min_lat_nsec()에서 enable_cnt 변화 시 schedule_work().
	 * 읽는 자/실행자: blkiolatency_enable_work_fn() — queue를 freeze/unfreeze하며
	 *                 enabled 및 QUEUE_FLAG_BIO_ISSUE_TIME 플래그를 안전하게 전환.
	 * 값 범위: 스케줄됨/완료 두 상태.
	 * 동기화: blkcg_iolatency_exit()에서 flush_work()로 완전히 종료 대기. */
};

/*
 * [한국어]
 * BLKIOLATENCY - rq_qos 포인터로부터 blk_iolatency 전체 구조체를 역참조.
 *
 * @rqos: rq_qos_add(RQ_QOS_LATENCY, ...)로 등록된 이 큐의 rq_qos 인스턴스.
 *        blkcg_iolatency_ops의 콜백(.throttle/.done_bio/.exit)이 호출될 때
 *        프레임워크가 넘겨주는 포인터.
 * @return: rqos를 포함하는 struct blk_iolatency의 시작 주소.
 *
 * container_of()는 rqos가 blk_iolatency 구조체의 첫 번째 멤버로 배치되어
 * 있음을 이용해 포인터 산술만으로 상위 구조체를 얻는다. rq_qos 프레임워크는
 * QoS 정책마다(wbt, iolatency 등) 공통 rq_qos 헤더만 알고 있으므로, 각
 * 정책 구현체는 이 매크로 패턴으로 자신의 확장 데이터를 꺼내야 한다.
 * 실행 컨텍스트: submit 경로(사용자 태스크)와 done_bio 경로(softirq/IRQ)
 * 양쪽에서 모두 호출되는 인라인 함수이므로 별도 락이 필요 없다(단순 포인터 변환).
 *
 * 호출 체인:
 *   blkcg_iolatency_throttle()/blkcg_iolatency_done_bio() → [BLKIOLATENCY] → (container_of, 반환)
 */
static inline struct blk_iolatency *BLKIOLATENCY(struct rq_qos *rqos)
{
	return container_of(rqos, struct blk_iolatency, rqos);
	/* [한국어] rqos 필드의 오프셋만큼 주소를 빼서 blk_iolatency 시작 주소를 계산.
	 * gendisk당 하나씩 존재하는 iolatency 인스턴스를 얻는다. */
}

struct child_latency_info {
	spinlock_t lock;
	/* [한국어] 이 child_latency_info 구조체 전체(아래 4개 필드)를 보호하는 스핀락.
	 * 설정자: iolatency_pd_init()에서 spin_lock_init()으로 초기화.
	 * 읽는 자/잠금 주체: iolatency_check_latencies()가 통계 윈도우 평가와
	 *          scale_cookie/scale_grp/scale_lat 갱신 전체를 spin_lock_irqsave()로
	 *          감싸고, iolatency_clear_scaling()이 target 변경 시 리셋할 때도 사용.
	 * 값 범위: 잠김/풀림 두 상태.
	 * 동기화: irqsave 변형을 사용하는 이유는 blkcg_iolatency_done_bio()가
	 *         IRQ/softirq 컨텍스트(bio 완료 처리 경로)에서 호출될 수 있어
	 *         일반 spin_lock()만 쓰면 자기 자신과 데드락이 발생하기 때문이다. */

	/* Last time we adjusted the scale of everybody. */
	u64 last_scale_event;
	/* [한국어] 이 부모가 마지막으로 scale_cookie를 조정한 시각(blk_time_get_ns() 단위).
	 * 설정자: iolatency_check_latencies()가 scale_cookie_change() 호출 직전에
	 *         now 값으로 갱신하며, iolatency_clear_scaling()에서 target 변경 시 0으로 리셋.
	 * 읽는 자: iolatency_check_latencies()가 BLKIOLATENCY_MIN_ADJUST_TIME(500ms)
	 *          이내 재조정을 막는 디바운스 기준으로 비교하고, blkiolatency_timer_fn()이
	 *          5초 이상 경과 시 scale_grp을 지우는 기준으로 사용.
	 * 값 범위: 0(미조정) 또는 blk_time_get_ns() 스케일의 절대 시각.
	 * 동기화: lock으로 보호되는 영역 내에서만 읽고 쓴다. max_depth가
	 *         너무 자주 출렁이지 않도록 하는 디바운스 기준 역할. */

	/* The latency that we missed. */
	u64 scale_lat;
	/* [한국어] 현재 scale down의 근거가 된 자식의 목표 지연(min_lat_nsec) 값.
	 * 설정자: iolatency_check_latencies()가 SLO 위반을 감지했을 때, 기존
	 *         scale_grp이 없거나 자신의 min_lat_nsec이 더 낮으면(더 엄격하면)
	 *         WRITE_ONCE()로 갱신. iolatency_clear_scaling()에서 0으로 리셋.
	 * 읽는 자: check_scale_change()가 READ_ONCE()로 읽어, 자신의 min_lat_nsec이
	 *          이 값보다 높으면(덜 엄격하면) scale down 영향을 받지 않도록 조기 반환.
	 * 값 범위: 0(기준 없음) 또는 nsec 단위 목표 지연. 여러 자식 중 가장 낮은
	 *          (가장 엄격한) SLO 값이 저장된다.
	 * 동기화: 설정 측은 lock 보호 하에, 읽는 측(check_scale_change)은 lock 없이
	 *         READ_ONCE/WRITE_ONCE로 데이터 레이스를 완화(정확한 직렬화는
	 *         아니지만 단조 감소 추세만 필요하므로 허용). */

	/* Total io's from all of our children for the last summation. */
	u64 nr_samples;
	/* [한국어] 모든 자식 cgroup의 최근 통계 윈도우 샘플 수 합계.
	 * 설정자: iolatency_check_latencies()가 자신의 이전 기여분을 빼고
	 *         (lat_info->nr_samples -= iolat->nr_samples) 최신 기여분을 더해
	 *         (+= latency_stat_samples(...)) 갱신 — 즉 자식별 최신 값으로 치환.
	 * 읽는 자: check_scale_change()가 samples_thresh(전체의 5%) 계산의
	 *          분모로 사용해, 기여도가 낮은 자식이 억울하게 scale down되지
	 *          않도록 판단.
	 * 값 범위: 0 이상. 완료된 bio 수의 누적 합계.
	 * 동기화: lock으로 보호되는 영역 내에서만 갱신된다. */

	/* The guy who actually changed the latency numbers. */
	struct iolatency_grp *scale_grp;
	/* [한국어] 현재 scale down을 유발한 책임 자식 cgroup을 가리키는 포인터.
	 * 설정자: iolatency_check_latencies()가 SLO 위반 자식을 발견하면 자신으로
	 *         설정. iolatency_clear_scaling()에서 target 변경 시 NULL로 리셋.
	 *         blkiolatency_timer_fn()도 5초 이상 조정이 없으면 NULL로 리셋.
	 * 읽는 자: iolatency_check_latencies()가 scale_grp == iolat인지 비교해
	 *          "내가 원인 제공자였는데 이제 SLO를 만족하는지" 판단(회복 시작 조건).
	 *          blkiolatency_timer_fn()도 NULL이면 무조건 scale up을 시도.
	 * 값 범위: NULL(책임자 없음/모호) 또는 유효한 iolatency_grp 포인터.
	 *          이 포인터가 가리키는 자식이 소멸돼도 역참조하지 않고 비교만
	 *          하므로 use-after-free 위험은 없다.
	 * 동기화: lock 보호 하에서만 읽고 쓴다. */

	/* Cookie to tell if we need to scale up or down. */
	atomic_t scale_cookie;
	/* [한국어] 이 부모 아래 모든 자식이 공유하는 scale 방향 지시자(쿠키).
	 * 설정자: scale_cookie_change()가 scale_cookie_change(blkiolat, lat_info, up)
	 *         호출을 통해 atomic_inc/dec/add/sub/set으로 조정. iolatency_pd_init()이
	 *         자식 생성 시 DEFAULT_SCALE_COOKIE로 초기 설정.
	 * 읽는 자: 모든 자식의 check_scale_change()가 atomic_read()로 자신의 로컬
	 *          사본(iolat->scale_cookie)과 비교해 증가/감소 방향을 판단.
	 * 값 범위: 0 ~ DEFAULT_SCALE_COOKIE(1,000,000) 부근. DEFAULT_SCALE_COOKIE이면
	 *          제한 없음(max_depth를 줄일 이유가 없음), 작을수록 강한 스로틀을 의미.
	 * 동기화: atomic_t 및 atomic_try_cmpxchg(check_scale_change)로 다수의 자식
	 *         cgroup이 동시에 submit 경로에서 이 값을 읽고 자신의 사본을
	 *         갱신해도 안전하다. */
};

/* [한국어] percentile_stats — 비회전식 미디어에서 쓰는 "몇 %가 목표를 놓쳤나" 방식의
 * 통계. 평균이 아니라 위반 건수 비율을 보는 이유는, 쓰기가 유난히 빨리 끝나는
 * 장치에서 평균만 보면 소수의 느린 읽기가 평균에 묻혀 실제 압박을 놓치기
 * 때문이다(파일 상단 원문 주석의 설명). 필드가 두 개뿐이라 완료 경로에서
 * 갱신 비용이 사실상 증가 연산 두 번으로 끝난다. */
struct percentile_stats {
	u64 total;
	/* [한국어] 비회전식(ssd=true) 모드에서 이 cgroup의 전체 완료 IO 개수.
	 * 설정자: latency_stat_record_time()에서 per-CPU stat에 1씩 증가.
	 * 읽는 자: latency_sum_ok()에서 miss 허용 threshold(total/10) 계산에 사용.
	 *         latency_stat_samples()에서 샘플 수로 반환.
	 * 값 범위: 0 이상. 윈도우마다 latency_stat_init()으로 리셋.
	 * 동기화: per-CPU 버퍼에 쓰고, iolatency_check_latencies()에서 preempt_disable
	 *         상태로 모든 CPU를 순회하며 합산. 개별 쓰기는 비원자적이나
	 *         per-CPU 구조로 충돌 없음. */
	u64 missed;
	/* [한국어] 목표 지연(min_lat_nsec)을 초과한 IO 완료 개수.
	 * 설정자: latency_stat_record_time()에서 req_time >= min_lat_nsec 조건 시 증가.
	 * 읽는 자: latency_sum_ok()에서 missed < thresh(total/10) 비교로 SLO 판단.
	 * 값 범위: 0 이상, total 이하. missed/total 비율이 10% 초과하면 scale down.
	 * 동기화: total과 동일 — per-CPU 쓰기, preempt_disable 하에 집계. */
};

/* [한국어] 아래 union은 ssd 플래그 하나로 두 통계 방식을 배타적으로 고른다.
 * 이 구조체가 per-CPU 배열로 CPU 수만큼 복제되기 때문에 크기가 곧 비용이다. */
struct latency_stat {
/* [한국어] latency_stat: iolatency_grp->ssd 플래그에 따라 두 통계 방식 중
 * 하나만 유효하게 사용하는 union. 두 방식을 함께 사용하지 않으므로 메모리를
 * 공유해도 되며, 이는 per-CPU 배열(iolat->stats)의 크기를 절반으로 줄인다. */
	union {
		struct percentile_stats ps;
		/* [한국어] percentile_stats: 비회전식(non-rotational) 미디어용.
		 * 설정자/읽는 자: iolat->ssd == true일 때만 latency_stat_init/sum/
		 *          record_time/samples/ok 계열 헬퍼가 이 멤버를 사용.
		 * 값 범위: total/missed 카운터 쌍(struct percentile_stats 정의 참고).
		 * 동기화: union이므로 rqs와 메모리를 공유 — ssd 플래그가 런타임에
		 *         바뀌지 않는다는 전제(iolatency_pd_init에서 1회 결정) 하에 안전. */
		struct blk_rq_stat rqs;
		/* [한국어] blk_rq_stat: HDD 등 회전식(rotational) 미디어용 평균/분산 통계
		 * (block/blk-stat.h 정의, 커널 공통 IO 통계 헬퍼).
		 * 설정자/읽는 자: iolat->ssd == false일 때만 blk_rq_stat_init/add/sum이
		 *          이 멤버를 사용. SSD류는 ssd=true이므로 이 경로는 거의
		 *          사용되지 않는다(HDD 백엔드를 가진 blk-iolatency 사용자용).
		 * 값 범위: mean/nr_samples 등(blk_rq_stat 정의 참고).
		 * 동기화: ps와 동일 — union 멤버는 상호 배타적으로만 사용된다. */
	};
};

/* [한국어] iolatency_grp — (cgroup, 디스크) 쌍 하나에 대한 이 정책의 전체 상태.
 * blkcg 코어가 계층의 각 노드마다 하나씩 붙여 주며(pd_alloc/pd_init),
 * 목표 지연(min_lat_nsec), 집행 수단(max_depth, rq_wait), 관측 수단
 * (per-CPU stats, cur_stat), 그리고 자기 자식들을 조율하기 위한 공유 상태
 * (child_lat)를 한 덩어리로 들고 있다. 목표가 설정되지 않은 노드도 객체는
 * 존재하며(min_lat_nsec==0), 통계 순회의 통과 지점 역할만 한다. */
struct iolatency_grp {
	struct blkg_policy_data pd;
	/* [한국어] blkcg policy 공통 헤더(block/blk-cgroup.h). 이 필드를 통해
	 * blkcg_gq(하나의 (cgroup, 디스크) 쌍)와 이 iolatency_grp을 연결한다.
	 * 설정자: iolatency_pd_alloc()이 kzalloc_node()로 iolat 전체를 할당할 때
	 *         pd도 함께 확보되며, blkcg 코어가 pd.blkg/pd.plid 등을 채운다.
	 * 읽는 자: pd_to_lat()이 container_of()로 이 필드에서 iolatency_grp 전체를
	 *          역참조하고, lat_to_blkg()는 반대로 pd_to_blkg(&iolat->pd)로
	 *          blkcg_gq를 구한다.
	 * 값 범위: blkcg 코어가 관리하는 유효한 policy data. iolatency_pd_free()
	 *          호출 전까지 유효.
	 * 동기화: blkcg_gq의 참조 카운트(blkg_tryget/blkg_put)와 RCU에 의해
	 *         생명주기가 보호된다(blkg_for_each_descendant_pre 등). */
	struct latency_stat __percpu *stats;
	/* [한국어] CPU별로 독립된 latency_stat 배열(per-CPU 변수).
	 * 설정자: iolatency_pd_alloc()이 __alloc_percpu_gfp()로 할당하고,
	 *         iolatency_pd_init()이 for_each_possible_cpu()로 순회하며
	 *         latency_stat_init()으로 0 초기화.
	 * 읽는 자/쓰는 자: latency_stat_record_time()이 get_cpu_ptr()로 현재
	 *          실행 중인 CPU의 슬롯을 얻어 완료 지연을 기록하고,
	 *          iolatency_check_latencies()/iolatency_ssd_stat()이
	 *          for_each_online_cpu()로 전체 CPU를 순회하며 합산 후 리셋.
	 * 값 범위: 각 온라인 CPU마다 독립된 struct latency_stat 인스턴스.
	 *          완료 인터럽트가 어느 CPU에서 처리되는지에 따라 값이
	 *          분산되어 쌓인다 — 완료가 어느 CPU에서 처리되는지에 따라 갈리며,
	 *          보통 인터럽트 affinity 설정이 그것을 결정한다.
	 * 동기화: per-CPU 변수 자체가 CPU 간 경쟁을 없애며, get_cpu_ptr()은
	 *         호출 동안 preemption을 비활성화해 같은 CPU 내 마이그레이션을
	 *         막는다. 집계 측은 preempt_disable()로 순회 중 CPU 이동을 막는다. */
	struct latency_stat cur_stat;
	/* [한국어] 현재 통계 윈도우 동안 여러 per-CPU 슬롯을 합쳐 누적해 온
	 * "확정 전" 집계 버퍼(iolatency_check_latencies에서 이번 윈도우 값을
	 * 더해 나가다가 scale 판단 후 리셋).
	 * 설정자: iolatency_pd_init()이 latency_stat_init()으로 0 초기화,
	 *         iolatency_check_latencies()가 latency_stat_sum()으로 이번
	 *         윈도우 stat을 누적하고, scale 결정 후 다시 init으로 리셋.
	 * 읽는 자: iolatency_check_latencies()가 latency_sum_ok()/
	 *          latency_stat_samples()에 넘겨 "직전 + 이번" 두 윈도우 모두
	 *          SLO를 만족하는지(scale up 조건)를 판단.
	 * 값 범위: struct latency_stat 참고(ssd 여부에 따라 ps 또는 rqs 사용).
	 * 동기화: 부모의 child_lat.lock(spin_lock_irqsave)으로 보호되는 영역
	 *         안에서만 갱신된다 — done_bio(IRQ) 경로와의 경쟁을 막기 위함. */
	struct blk_iolatency *blkiolat;
	/* [한국어] 이 cgroup이 속한 디스크/큐 전체의 blk_iolatency 인스턴스.
	 * 설정자: iolatency_pd_init()이 iolat_rq_qos(blkg->q)로 rq_qos를 찾아
	 *         BLKIOLATENCY()로 역참조한 값을 저장.
	 * 읽는 자: scale_cookie_change()/scale_change()가 blkiolat->rqos.disk->
	 *          queue->nr_requests로 큐 전체 상한을 구하고, 여러 함수가
	 *          blkiolat->enabled/timer/enable_work에 접근할 때 사용.
	 * 값 범위: 큐가 존재하는 한 유효한 포인터(NULL 아님).
	 * 동기화: 별도 락 없음 — 포인터 자체는 iolatency_pd_init() 이후 불변. */
	unsigned int max_depth;
	/* [한국어] 이 cgroup이 동시에 inflight로 가질 수 있는 최대 IO 수.
	 * 장치 큐 깊이와는 무관한, 이 cgroup 전용의 순수 소프트웨어 상한이다.
	 * 설정자: iolatency_pd_init()이 UINT_MAX(무제한)로 초기화. scale_change()가
	 *         scale up/down 시 조정하고, check_scale_change()가 DEFAULT_SCALE_COOKIE
	 *         복귀 시 UINT_MAX로 되돌린다.
	 * 읽는 자: iolat_acquire_inflight()가 rq_wait_inc_below(rqw, max_depth)의
	 *          상한으로 사용해 inflight < max_depth일 때만 새 IO를 통과시킨다.
	 * 값 범위: 1 ~ UINT_MAX. 1이 최솟값(완전히 막지는 않음), 초과 스로틀
	 *          필요 시 blkcg_use_delay()로 별도 지연을 부과한다.
	 * 동기화: 값 자체는 단순 정수 대입/비교로 갱신되며, 정확한 원자성보다는
	 *         "대략적인 상한"으로 충분하다는 설계(rq_wait 내부의 atomic
	 *         inflight 카운터가 실제 동시성 제어를 담당). */
	struct rq_wait rq_wait;
	/* [한국어] max_depth 초과 시 초과분 bio가 대기하는 wait queue와
	 * inflight 카운터(block/blk-rq-qos.h 정의).
	 * 설정자: iolatency_pd_init()이 rq_wait_init()으로 초기화.
	 * 읽는 자/쓰는 자: __blkcg_iolatency_throttle()이 rq_qos_wait(rqw, ...)로
	 *          대기를 걸고, iolat_acquire_inflight()/iolat_cleanup_cb()가
	 *          inflight를 증감시키며, blkcg_iolatency_done_bio()가 완료 시
	 *          atomic_dec_return(&rqw->inflight) 후 wake_up()으로 대기자를 깨운다.
	 * 값 범위: rqw->inflight는 0 이상(WARN_ON_ONCE(inflight < 0)으로 불변식 검증).
	 * 동기화: inflight는 atomic_t, wait는 waitqueue_head — 다중 제출자/완료
	 *         인터럽트가 동시에 접근해도 안전하도록 설계된 rq_wait 자체의 책임. */
	atomic64_t window_start;
	/* [한국어] 현재 통계 윈도우(cur_win_nsec 길이)가 시작된 절대 시각.
	 * 설정자: iolatency_pd_init()이 now(blk_time_get_ns())로 초기화.
	 *         blkcg_iolatency_done_bio()가 윈도우 만료를 감지하면
	 *         atomic64_try_cmpxchg()로 새 시각으로 교체.
	 * 읽는 자: blkcg_iolatency_done_bio()가 (now - window_start) >= cur_win_nsec
	 *          조건으로 윈도우 만료를 판정해 iolatency_check_latencies() 호출 여부 결정.
	 * 값 범위: blk_time_get_ns() 단위의 단조 증가 시각.
	 * 동기화: atomic64_try_cmpxchg()로 CAS(Compare-And-Swap) 갱신 —
	 *         여러 CPU의 완료 인터럽트가 동시에 윈도우 만료를 감지해도
	 *         단 하나만 iolatency_check_latencies()를 실행하도록 보장. */
	atomic_t scale_cookie;
	/* [한국어] 부모 child_latency_info.scale_cookie의 로컬 사본(마지막으로
	 * 관찰한 값). 부모의 전역 쿠키와 비교해 증가/감소 방향을 판단하는 데 쓰인다.
	 * 설정자: iolatency_pd_init()이 부모의 현재 값(또는 DEFAULT)으로 초기화.
	 *         check_scale_change()가 atomic_try_cmpxchg()로 부모의 최신 값으로 갱신.
	 * 읽는 자: check_scale_change()가 atomic_read()로 부모의 cur_cookie와
	 *          비교해 direction(scale up/down 여부)을 결정.
	 * 값 범위: 0 ~ 부모의 DEFAULT_SCALE_COOKIE 부근 값.
	 * 동기화: atomic_try_cmpxchg()로 "부모 값을 아직 반영 안 한 자식만
	 *         갱신"하도록 해 같은 이벤트를 중복 처리하지 않는다. */
	u64 min_lat_nsec;
	/* [한국어] 이 cgroup이 목표로 선언한 IO 완료 지연 상한(nsec 단위).
	 * cgroupfs의 io.latency 파일에 "target=<usec>"로 사용자가 설정한 값이다.
	 * 설정자: iolatency_set_min_lat_nsec()이 iolatency_set_limit()(sysfs write)
	 *         또는 iolatency_pd_offline()(cgroup 제거 시 0으로)에서 호출되어 설정.
	 * 읽는 자: iolatency_record_time()이 통계 기록 여부/root 대납 시 delay 계산에,
	 *          latency_stat_record_time()이 SSD miss 판정 기준으로,
	 *          check_scale_change()가 scale_lat과 비교해 자신이 영향받는지 판단.
	 * 값 범위: 0(비활성, 이 cgroup은 목표 없음) 또는 양수 nsec 값. 빠른 장치의
	 *          전형적 목표는 수백 us ~ 수 ms 수준이다.
	 * 동기화: 별도 락 없이 갱신 — 갱신 시점에 iolatency_clear_scaling()으로
	 *         관련 scale 상태를 함께 리셋해 일관성을 맞춘다. */
	u64 cur_win_nsec;
	/* [한국어] 현재 통계 윈도우의 길이(nsec). min_lat_nsec에 비례해 결정된다.
	 * 설정자: iolatency_set_min_lat_nsec()이 max(min_lat_nsec << 4,
	 *         BLKIOLATENCY_MIN_WIN_SIZE)를 BLKIOLATENCY_MAX_WIN_SIZE로 클램프해
	 *         설정. iolatency_pd_init()은 초기값으로 100ms를 대입.
	 * 읽는 자: blkcg_iolatency_done_bio()가 윈도우 만료 판정에,
	 *          iolat_update_total_lat_avg()가 decay 계수 선택에 사용.
	 * 값 범위: BLKIOLATENCY_MIN_WIN_SIZE(100ms) ~ BLKIOLATENCY_MAX_WIN_SIZE(1s).
	 *          목표 지연이 매우 짧아도 최소 100ms는 보장해
	 *          충분한 샘플을 모은다.
	 * 동기화: min_lat_nsec과 동일한 시점에 갱신되며 별도 락 불필요. */

	/* total running average of our io latency. */
	u64 lat_avg;
	/* [한국어] calc_load()(linux/sched/loadavg.h, CPU load average와 동일한
	 * 지수 이동 평균 알고리즘) 기반 HDD 모드 전용 장기 평균 지연.
	 * 설정자: iolat_update_total_lat_avg()가 매 윈도우마다 calc_load()로 갱신
	 *         (ssd == true이면 조기 반환하여 갱신되지 않음).
	 * 읽는 자: iolatency_pd_stat()이 디버그 통계(avg_lat)로 seq_file에 출력.
	 * 값 범위: 0 ~ 임의의 nsec 값. HDD의 완만한 지연 추세를 반영.
	 * 동기화: iolatency_check_latencies() 호출 경로(child_lat.lock 보호 구간)
	 *         내에서만 갱신되어 경쟁이 없다. */

	/* Our current number of IO's for the last summation. */
	u64 nr_samples;
	/* [한국어] 이 cgroup이 최근 윈도우에서 기록한 샘플(완료 IO) 수.
	 * 설정자: iolatency_check_latencies()가 latency_stat_samples(cur_stat)로 갱신.
	 * 읽는 자: check_scale_change()가 samples_thresh(부모 전체의 5%)와 비교해
	 *          "내가 전체 IO의 5% 이하만 냈다면 억울한 scale down을 건너뛴다"는
	 *          판단에 사용. 부모의 child_lat.nr_samples 갱신 시 뺄셈/덧셈의 피연산자.
	 * 값 범위: 0 이상.
	 * 동기화: 부모의 child_lat.lock 보호 구간 내에서만 갱신. */

	bool ssd;
	/* [한국어] 이 cgroup이 속한 큐가 비회전식 미디어인지 여부.
	 * 설정자: iolatency_pd_init()이 !blk_queue_rot(blkg->q)로 1회 결정
	 *         (이후 변경되지 않음).
	 * 읽는 자: latency_stat_init/sum/record_time/samples, latency_sum_ok,
	 *          iolat_update_total_lat_avg, iolatency_pd_stat 등 거의 모든
	 *          통계 헬퍼가 분기 조건으로 사용 — true면 percentile(missed/total
	 *          비율) 방식, false면 평균(mean) 방식.
	 * 값 범위: true(SSD 등, percentile 방식) / false(회전식, 평균 방식).
	 * 동기화: 초기화 후 불변이므로 락 불필요. */
	struct child_latency_info child_lat;
	/* [한국어] 이 cgroup이 "부모" 입장에서 자신의 직계 자식들의 scale 상태를
	 * 조율하기 위해 갖는 공유 상태(스핀락 + scale_cookie/scale_grp/scale_lat 등).
	 * 설정자: iolatency_pd_init()이 spin_lock_init() 및 scale_cookie를
	 *         DEFAULT_SCALE_COOKIE로 초기화.
	 * 읽는 자: 이 iolatency_grp의 "자식"들이 blkg_to_lat(blkg->parent)로
	 *          이 구조체에 접근해 check_scale_change()/iolatency_check_latencies()
	 *          에서 scale 방향을 읽고 쓴다. struct child_latency_info의 필드별
	 *          주석 참고.
	 * 값 범위: struct child_latency_info 정의 참고.
	 * 동기화: 내부 lock 필드로 자체 보호. */
};

#define BLKIOLATENCY_MIN_WIN_SIZE (100 * NSEC_PER_MSEC)
/* [한국어] [한국어] 통계 윈도우의 최소 크기(100ms). 목표 지연이 매우 짧을 때도
 * 이보다 작은 윈도우는 표본이 너무 적어 통계적으로 의미가 없어질 수 있어 하한을 둔다. */
#define BLKIOLATENCY_MAX_WIN_SIZE NSEC_PER_SEC
/* [한국어] 통계 윈도우의 최대 크기(1초). 이보다 큰 윈도우는 SLO 위반을 감지하고
 * scale 조정을 하기까지의 반응 시간이 너무 느려지므로 상한을 둔다. */
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
/* [한국어] iolatency_exp_factors[] 배열의 원소 개수(=아래 decay 계수 테이블의 크기).
 * HDD 모드의 lat_avg 지수 이동 평균 계산에서, 윈도우 크기 구간별로 다른 감쇠율을
 * 적용하기 위한 버킷(bucket) 수. */
#define BLKIOLATENCY_EXP_BUCKET_SIZE (BLKIOLATENCY_MAX_WIN_SIZE / \
				      (BLKIOLATENCY_NR_EXP_FACTORS - 1))
/* [한국어] 1초(최대 윈도우)를 (버킷 수-1)등분한 구간 크기. iolat_update_total_lat_avg()가
 * cur_win_nsec을 이 값으로 나눠 iolatency_exp_factors[] 인덱스를 선택하는 데 사용. */
static const u64 iolatency_exp_factors[BLKIOLATENCY_NR_EXP_FACTORS] = {
	/* [한국어] calc_load()에 넘길 고정소수점(FIXED_1=2048 기준) decay 계수 테이블.
	 * 윈도우가 짧을수록(=IO가 잦을수록) 작은 계수를, 윈도우가 길수록 큰 계수를
	 * 선택해, 실제 경과 시간과 무관하게 대략 1~2.5분의 반감기를 갖도록 보정한 값
	 * (주석의 exp(1/N)은 N개 샘플에 걸친 지수 감쇠를 근사한다는 의미). */
	2045, // exp(1/600) - 600 samples
	2039, // exp(1/240) - 240 samples
	2031, // exp(1/120) - 120 samples
	2023, // exp(1/80)  - 80 samples
	2014, // exp(1/60)  - 60 samples
};

/*
 * [한국어]
 * pd_to_lat - blkg_policy_data 포인터로부터 iolatency_grp을 역참조.
 *
 * @pd: blkcg 코어(blkg_policy_data 배열)가 관리하는 policy data 포인터.
 *      해당 blkg에 iolatency policy가 아직 활성화되지 않았으면 NULL일 수 있다.
 * @return: pd가 NULL이 아니면 이를 포함하는 iolatency_grp의 주소, NULL이면 NULL.
 *
 * blkg_to_pd()가 반환하는 값은 policy가 비활성 상태인 blkg에 대해 NULL일 수
 * 있으므로(예: 이 큐에 아직 io.latency policy가 register/activate 되지 않은
 * 경우), 호출자가 매번 NULL 체크를 반복하지 않도록 삼항 연산자로 감싼다.
 * 실행 컨텍스트: 인라인 함수이며 submit/done_bio 등 모든 경로에서 호출 가능.
 *
 * 호출 체인:
 *   blkg_to_lat() → [pd_to_lat] → container_of()
 */
static inline struct iolatency_grp *pd_to_lat(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct iolatency_grp, pd) : NULL;
	/* [한국어] pd가 NULL이 아니면 pd 필드 오프셋을 이용해 iolatency_grp 시작
	 * 주소를 계산. pd가 NULL이면(이 blkg에 iolatency policy data 없음) NULL 반환. */
}

/*
 * [한국어]
 * blkg_to_lat - blkcg_gq(하나의 (cgroup, 디스크) 조합)로부터 iolatency_grp을 획득.
 *
 * @blkg: bio->bi_blkg 등에서 얻은, 특정 cgroup과 특정 디스크의 조합을 나타내는
 *        blkcg_gq 포인터. blkg_to_pd()가 policy ID로 policy data 배열을 인덱싱한다.
 * @return: 이 blkg에 대한 iolatency_grp 포인터, policy 미활성 시 NULL.
 *
 * blkcg_gq는 여러 policy(io.latency, io.weight 등)의 policy data를 배열로
 * 갖고 있으며, blkg_to_pd(blkg, &blkcg_policy_iolatency)로 이 파일이 등록한
 * policy 슬롯만 꺼낸 뒤 pd_to_lat()으로 최종 iolatency_grp까지 역참조한다.
 * 이 함수는 submit 경로(blkcg_iolatency_throttle), 완료 경로
 * (blkcg_iolatency_done_bio), scale 조정(check_scale_change 등)에서 cgroup
 * 계층을 순회하며 반복 호출되는 핵심 변환 함수이다.
 *
 * 호출 체인:
 *   blkcg_iolatency_throttle()/blkcg_iolatency_done_bio()/check_scale_change() 등
 *   → [blkg_to_lat] → blkg_to_pd() → pd_to_lat()
 */
static inline struct iolatency_grp *blkg_to_lat(struct blkcg_gq *blkg)
{
	return pd_to_lat(blkg_to_pd(blkg, &blkcg_policy_iolatency));
	/* [한국어] blkcg_gq -> (iolatency policy용) blkg_policy_data -> iolatency_grp
	 * 순서로 변환. 이 큐의 cgroup별 io.latency 제어 상태에 접근하는 유일한 경로. */
}

/*
 * [한국어]
 * lat_to_blkg - iolatency_grp으로부터 역으로 blkcg_gq를 획득.
 *
 * @iolat: 이 함수 호출 시점에 유효한(참조 카운트가 살아있는) iolatency_grp.
 * @return: iolat->pd가 속한 blkcg_gq 포인터.
 *
 * blkg_to_lat()의 반대 방향 변환. 부모/조상 cgroup으로 거슬러 올라갈 때
 * (blkg->parent 탐색) 또는 blkcg_add_delay()/blkcg_use_delay() 등 blkcg
 * 코어 API에 blkg를 넘겨야 할 때 사용한다.
 *
 * 호출 체인:
 *   __blkcg_iolatency_throttle()/scale_change()/check_scale_change() 등
 *   → [lat_to_blkg] → pd_to_blkg()
 */
static inline struct blkcg_gq *lat_to_blkg(struct iolatency_grp *iolat)
{
	return pd_to_blkg(&iolat->pd);
	/* [한국어] iolat->pd 필드 주소로 pd_to_blkg()를 호출해 blkcg_gq를 역산.
	 * 부모/자식 cgroup 계층 탐색이나 blkcg 코어 API 호출 시 사용된다. */
}

/*
 * [한국어]
 * latency_stat_init - per-CPU 슬롯 또는 임시 latency_stat을 0으로 초기화.
 *
 * @iolat: ssd 플래그를 참조하기 위한 이 cgroup의 iolatency_grp.
 * @stat: 초기화 대상 latency_stat(percpu 슬롯 또는 스택의 임시 변수, 또는
 *        iolat->cur_stat 등 어느 것이든 될 수 있다).
 * @return: 없음(void).
 *
 * ssd(비회전식 미디어) 여부에 따라 두 union 멤버 중 실제 사용 중인 쪽만
 * 초기화한다. 비회전식은 percentile_stats(total/missed 카운터 쌍)를,
 * HDD는 blk_rq_stat_init()으로 평균/분산 통계를 초기화한다. 새 통계
 * 윈도우를 시작하기 전(iolatency_pd_init, iolatency_check_latencies)이나
 * per-CPU 배열을 처음 할당한 직후(iolatency_pd_alloc 이후) 호출되어
 * 이전 윈도우의 잔여 값이 섞이지 않도록 보장한다.
 * 실행 컨텍스트: 인라인 함수로 어떤 컨텍스트에서도 호출 가능. 별도 락 없음
 * (호출자가 필요한 동기화를 책임진다).
 *
 * 호출 체인:
 *   iolatency_pd_init()/iolatency_check_latencies() → [latency_stat_init]
 */
static inline void latency_stat_init(struct iolatency_grp *iolat,
				     struct latency_stat *stat)
{
	if (iolat->ssd) {
		/* [한국어] 비회전식: 완료 건수 카운터를 0으로 리셋. 윈도우마다 통계를
		 * 통째로 비우는 이유는, 이 판정이 '지금까지 누적'이 아니라
		 * '최근 한 윈도우 동안'의 상태를 봐야 하기 때문이다 */
		stat->ps.total = 0;
		stat->ps.missed = 0;
		/* [한국어] 비회전식: 목표 지연(min_lat_nsec)을 초과한 완료 수 카운터를 0으로 리셋 */
	} else
		blk_rq_stat_init(&stat->rqs);
		/* [한국어] HDD: 평균/분산 통계 버퍼를 0으로 초기화(block/blk-stat.c 구현).
		 * ssd==true 경로에서는 이 분기를 타지 않는다. */
}

/*
 * [한국어]
 * latency_stat_sum - 두 latency_stat을 더해 sum에 누적.
 *
 * @iolat: ssd 플래그 판별용 iolatency_grp.
 * @sum: 결과가 누적될 대상(예: 여러 CPU의 통계를 합산 중인 임시 stat).
 * @stat: sum에 더해질 개별 통계(예: 특정 CPU의 per-CPU 슬롯).
 * @return: 없음(void). 결과는 sum에 누적된다(sum = sum + stat).
 *
 * per-CPU로 분산 기록된 통계를 하나의 윈도우 집계로 병합할 때 사용된다.
 * 비회전식은 missed/total을 단순 누적하고, 회전식은 blk_rq_stat_sum()으로
 * 평균/분산을 통계적으로 병합한다(단순 합산이 아니라 표본 수 가중 병합).
 *
 * 호출 체인:
 *   iolatency_check_latencies()/iolatency_ssd_stat() → [latency_stat_sum]
 */
static inline void latency_stat_sum(struct iolatency_grp *iolat,
				    struct latency_stat *sum,
				    struct latency_stat *stat)
{
	if (iolat->ssd) {
		/* [한국어] percentile 방식은 단순 카운터라 그냥 더하면 된다 —
		 * 평균처럼 표본 수 가중이 필요 없다 */
		sum->ps.total += stat->ps.total;
		/* [한국어] 여러 CPU에서 각각 집계된 완료 카운트를 합산.
		 * CPU별로 쌓인 값을 하나의 전역 합계로 모으는 단계다. */
		sum->ps.missed += stat->ps.missed;
		/* [한국어] 여러 CPU에서 각각 집계된 min_lat_nsec 초과 완료 수를 합산 */
	} else
		blk_rq_stat_sum(&sum->rqs, &stat->rqs);
		/* [한국어] 회전식: 평균 통계를 표본 수 가중으로 병합 (ssd 경로와는 배타적) */
}

/*
 * [한국어]
 * latency_stat_record_time - 완료된 IO 한 건의 지연(req_time)을 현재 CPU의
 * per-CPU 통계 슬롯에 기록.
 *
 * @iolat: 기록 대상 cgroup의 iolatency_grp. iolat->stats(per-CPU 배열)와
 *         iolat->min_lat_nsec(SLO 임계값)을 사용.
 * @req_time: 이 IO가 제출부터 완료까지 소요한 시간(nsec). 호출자
 *            (iolatency_record_time)가 bio->issue_time_ns와 완료 시각의
 *            차이로 계산해 전달한다.
 * @return: 없음(void).
 *
 * get_cpu_ptr()로 현재 실행 중인 CPU 전용 슬롯을 얻어 preemption을 끈 채
 * 갱신함으로써, 같은 CPU 내에서의 갱신 도중 다른 태스크로 선점되어 슬롯이
 * 바뀌는 것을 막는다(다른 CPU와의 경쟁은애초에 슬롯이 분리되어 있어 발생하지
 * 않는다). 비회전식은 req_time이 min_lat_nsec(목표 지연) 이상이면 missed를
 * 증가시켜 SLO 위반 여부를 기록하고, 항상 total을 증가시킨다. 이 missed/total
 * 비율이 이후 latency_sum_ok()의 판단 근거가 된다. HDD는 blk_rq_stat_add()로
 * 평균/분산 계산에 표본을 추가한다.
 * 실행 컨텍스트: blkcg_iolatency_done_bio()의 완료 경로(통상 softirq/IRQ
 * 컨텍스트 — 장치 완료 인터럽트에서 이어진 bio 완료 시점)에서 호출된다.
 *
 * 호출 체인:
 *   blkcg_iolatency_done_bio() → iolatency_record_time() → [latency_stat_record_time]
 */
static inline void latency_stat_record_time(struct iolatency_grp *iolat,
					    u64 req_time)
{
	struct latency_stat *stat = get_cpu_ptr(iolat->stats);
	/* [한국어] 현재 실행 중인 CPU의 per-CPU 통계 슬롯 포인터를 얻고 preemption을
	 * 비활성화한다. 이 사이 다른 CPU로 마이그레이션되면 잘못된 CPU의 카운터를
	 * 갱신하게 되므로 반드시 필요하다. */
	if (iolat->ssd) {
		/* [한국어] 비회전식: 목표 지연(SLO)을 초과했으므로 miss 카운터 증가.
		 * 이 비율이 10%를 넘으면 latency_sum_ok()가 false를 반환해 scale down 트리거가 됨.
		 * '>='인 이유: 목표와 정확히 같은 시간이 걸린 IO도 목표를 지킨 것으로
		 * 쳐 주지 않는다 — 경계에서 관대하게 굴면 목표가 사실상 목표+1이 된다 */
		if (req_time >= iolat->min_lat_nsec)
			stat->ps.missed++;
		stat->ps.total++;
		/* [한국어] 비회전식: 총 완료 카운터 증가. total이 여전히 0이면
		 * latency_sum_ok()에서 threshold가 1로 보정되어 division-by-zero를 피한다 */
	} else
		blk_rq_stat_add(&stat->rqs, req_time);
		/* [한국어] 회전식: 평균/분산 계산기에 req_time 표본 추가 (ssd 경로에서는 사용 안 함) */
	put_cpu_ptr(stat);
	/* [한국어] get_cpu_ptr()로 비활성화했던 preemption을 복원. 짧은 임계 구역
	 * 동안만 마이그레이션을 막는 표준 per-CPU 접근 패턴이다. */
}

/*
 * [한국어]
 * latency_sum_ok - 주어진 통계가 목표 지연(SLO)을 만족하는지 판정.
 *
 * @iolat: ssd 플래그와 min_lat_nsec(HDD 판정 기준)을 제공하는 iolatency_grp.
 * @stat: 판정 대상 통계(주로 한 윈도우 동안 합산된 latency_stat).
 * @return: true면 SLO 만족(스로틀 유지/완화 검토 가능), false면 SLO 위반
 *          (scale down 검토 대상).
 *
 * 비회전식은 missed(SLO 초과 건수)가 total(전체 건수)의 10% 미만이면 OK로
 * 판단하는 percentile 기반 판정을 쓴다. total이 10 미만이라 10%가 1보다
 * 작아지는 경우에도 threshold를 최소 1로 강제해, IO가 거의 없는 상황에서
 * 단 한 번의 miss로 성급하게 scale down되지 않게 한다(그러나 여전히 miss가
 * threshold 이상이면 위반으로 판단). HDD는 평균 지연이 목표 이하인지로
 * 단순 비교한다.
 *
 * 호출 체인:
 *   iolatency_check_latencies() → [latency_sum_ok]
 */
static inline bool latency_sum_ok(struct iolatency_grp *iolat,
				  struct latency_stat *stat)
{
	if (iolat->ssd) {
		/* [한국어] total의 10%를 miss 허용 임계값으로 계산. SLO 위반율이
		 * 10% 미만이면 OK로 간주한다는 설계다 — 목표 지연을 100% 지키라고
		 * 요구하면 꼬리 지연 한 건에도 스로틀이 걸려 실용성이 없어진다.
		 * div64_u64를 쓰는 이유는 32비트 아키텍처에서 u64 나눗셈이
		 * 컴파일되지 않기 때문(커널은 libgcc를 링크하지 않는다) */
		u64 thresh = div64_u64(stat->ps.total, 10);
		thresh = max(thresh, 1ULL);
		/* [한국어] total이 0~9여서 계산된 threshold가 0이면 1로 강제.
		 * IO가 극히 적을 때도 판정 로직이 division 결과 0으로 인해
		 * 무의미해지는 것을 막는다 */
		return stat->ps.missed < thresh;
		/* [한국어] missed가 threshold 미만이면 SLO 만족(true), 이상이면 위반(false) */
	}
	return stat->rqs.mean <= iolat->min_lat_nsec;
	/* [한국어] 회전식: 평균 지연이 목표(min_lat_nsec) 이하이면 OK. 비회전식은
	 * percentile 방식을 쓰므로 이 줄에 도달하지 않는다 */
}

/*
 * [한국어]
 * latency_stat_samples - 이 통계에 반영된 표본(완료 IO) 개수를 반환.
 *
 * @iolat: ssd 플래그 판별용 iolatency_grp.
 * @stat: 표본 수를 조회할 대상 latency_stat.
 * @return: 비회전식은 total(완료 건수), 회전식은 rqs.nr_samples.
 *
 * check_scale_change()의 5% 기여도 판정, iolatency_check_latencies()의
 * BLKIOLATENCY_MIN_GOOD_SAMPLES(5개) 최소 표본 판정 등, "이 cgroup이 최근
 * 윈도우에서 실제로 얼마나 많은 IO를 냈는가"를 알아야 하는 모든 곳에서 쓰인다.
 *
 * 호출 체인:
 *   iolatency_check_latencies() → [latency_stat_samples]
 */
static inline u64 latency_stat_samples(struct iolatency_grp *iolat,
				       struct latency_stat *stat)
{
	if (iolat->ssd) /* [한국어] 두 모드가 '표본 수'를 서로 다른 필드에 들고 있어 이 래퍼가 필요하다 */
		return stat->ps.total;
		/* [한국어] 비회전식: 완료된 건수(total)를 그대로 표본 수로 사용 */
	return stat->rqs.nr_samples;
	/* [한국어] 회전식: blk_rq_stat이 자체적으로 관리하는 표본 수 (ssd 경로와는 배타적) */
}

/*
 * [한국어]
 * iolat_update_total_lat_avg - HDD 모드 전용 장기 지연 평균(lat_avg) 갱신.
 *
 * @iolat: 갱신 대상 iolatency_grp. ssd==true면 즉시 반환.
 * @stat: 이번 윈도우에 집계된 통계(HDD 모드에서 stat->rqs.mean을 사용).
 * @return: 없음(void). iolat->lat_avg가 갱신된다.
 *
 * CPU load average와 동일한 지수 이동 평균 알고리즘(calc_load(), linux/
 * sched/loadavg.h)을 재사용해, 매 윈도우의 평균 지연(stat->rqs.mean)을
 * 장기 추세(lat_avg)에 반영한다. 윈도우 크기(cur_win_nsec)에 따라 decay
 * 계수(iolatency_exp_factors[])를 다르게 선택해, "1분 ~ 2.5분" 정도의
 * 반감기를 갖도록 설계되었다(주석 원문 참고). 비회전식은 percentile 방식을
 * 쓰므로 이 함수는 아무 일도 하지 않고 조기 반환한다.
 *
 * 호출 체인:
 *   iolatency_check_latencies() → [iolat_update_total_lat_avg] → calc_load()
 */
static inline void iolat_update_total_lat_avg(struct iolatency_grp *iolat,
					      struct latency_stat *stat)
{
	int exp_idx; /* [한국어] 아래에서 고를 감쇠 상수(EMA 계수) 테이블 인덱스 */

	if (iolat->ssd) /* [한국어] percentile 모드에는 장기 평균이라는 개념 자체가 없다 */
		return;
		/* [한국어] 비회전식은 percentile(missed/total) 방식으로 판단하므로
		 * 장기 평균(lat_avg) 갱신이 불필요 — 즉시 반환 */

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
	/* [한국어] 현재 윈도우 크기(cur_win_nsec)를 BLKIOLATENCY_EXP_BUCKET_SIZE
	 * 단위로 나눠 decay 계수 배열의 인덱스를 선택. 상한을 NR_EXP_FACTORS-1로
	 * 클램프해 배열 범위를 벗어나지 않게 한다. (회전식 전용 경로이므로 ssd 모드에서는
	 * 도달하지 않음) */
	iolat->lat_avg = calc_load(iolat->lat_avg,
				   iolatency_exp_factors[exp_idx],
				   stat->rqs.mean);
	/* [한국어] load average와 동일한 공식으로 lat_avg를 갱신:
	 * new_avg = old_avg * decay + mean * (FIXED_1 - decay) 형태의 지수 이동 평균.
	 * 급격한 순간 변동을 완만하게 흡수해 HDD IO 트렌드를 추적한다. */
}

/*
 * [한국어]
 * iolat_cleanup_cb - rq_qos_wait() 대기 중이던 요청이 정리(취소)될 때
 * inflight 카운트를 되돌리는 콜백.
 *
 * @rqw: 대기가 걸려 있던 rq_wait(이 cgroup의 iolat->rq_wait).
 * @private_data: rq_qos_wait() 호출 시 넘긴 값(iolat_acquire_inflight와 동일한
 *                iolatency_grp 포인터이나, 이 함수 자체는 사용하지 않는다).
 * @return: 없음(void).
 *
 * rq_qos_wait()(block/blk-rq-qos.c)의 프레임워크 계약: acquire_inflight_cb가
 * 실패해 대기열에 들어갔던 요청이 깨어날 때(다시 시도하기 위해), 이미
 * iolat_acquire_inflight()가 낙관적으로 증가시켰을 수 있는 inflight를 원복하기
 * 위해 호출된다. 대기하다 취소된 요청은 아직 아무것도 발행하지
 * 않았으므로 in-flight로 세면 안 되고, 그래서 감소 후 다른 대기자를 깨운다.
 * 실행 컨텍스트: rq_qos_wait() 내부 루프에서 호출되며, 제출 태스크 컨텍스트에서
 * 실행된다.
 *
 * 호출 체인:
 *   rq_qos_wait() → [iolat_cleanup_cb]
 */
static void iolat_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	atomic_dec(&rqw->inflight);
	/* [한국어] 대기 중 정리된 요청이 잠정적으로 차지했던 inflight 슬롯을 반환.
	 * 아직 장치에 제출되지 않은 상태이므로 카운트만 되돌린다 */
	wake_up(&rqw->wait);
	/* [한국어] inflight 여유가 하나 생겼으므로 rq_wait.wait에서 대기 중인
	 * 다른 bio(태스크)를 깨워 재시도 기회를 준다 */
}

/*
 * [한국어]
 * iolat_acquire_inflight - max_depth 여유가 있으면 inflight
 * 슬롯을 획득 시도하는 rq_qos_wait() 콜백.
 *
 * @rqw: 이 cgroup의 iolat->rq_wait.
 * @private_data: __blkcg_iolatency_throttle()이 rq_qos_wait()에 넘긴 iolatency_grp
 *                포인터. 여기서 max_depth를 읽기 위해 캐스팅한다.
 * @return: true면 슬롯 획득 성공(즉시 진행 가능), false면 실패(대기 계속).
 *
 * rq_wait_inc_below()(block/blk-rq-qos.h)가 원자적으로 "inflight < limit이면
 * inflight++하고 true, 아니면 false"를 수행한다. rq_qos_wait()는 이 콜백이
 * false를 반환하는 동안 태스크를 wait queue에 재워 두고, wake_up될 때마다
 * 다시 이 콜백을 호출해 재시도한다.
 *
 * 호출 체인:
 *   rq_qos_wait() → [iolat_acquire_inflight] → rq_wait_inc_below()
 */
static bool iolat_acquire_inflight(struct rq_wait *rqw, void *private_data)
{
	struct iolatency_grp *iolat = private_data;
	/* [한국어] max_depth를 매번 다시 읽는 것이 중요하다 — 대기 중에도
	 * check_scale_change()가 이 값을 늘려 줄 수 있고, 그러면 잠들었던
	 * 태스크가 깨어나 그 자리에서 통과하게 된다 */
	return rq_wait_inc_below(rqw, iolat->max_depth);
	/* [한국어] atomic_cmpxchg() 루프 기반으로 inflight < max_depth이면 슬롯을
	 * 획득(inflight++, true). 한도에 도달했으면(max_depth
	 * 이상) false를 반환해 rq_qos_wait()가 이 bio를 대기시킨다 */
}

/*
 * [한국어]
 * __blkcg_iolatency_throttle - 하나의 cgroup(iolatency_grp)에 대한 실제
 * 스로틀 로직 수행.
 *
 * @rqos: 이 큐의 rq_qos 인스턴스(rqos->disk로 blkcg_schedule_throttle에 전달).
 * @iolat: 스로틀을 적용할 대상 iolatency_grp(cgroup 계층의 한 노드).
 * @issue_as_root: bio_issue_as_root_blkg(bio)로 판별된, 이 bio가 우선순위
 *                 역전(priority inversion)을 피하기 위해 root cgroup 권한으로
 *                 발행되어야 하는지 여부(예: REQ_META).
 * @use_memdelay: bio가 REQ_SWAP이라 memory.delay 서브시스템에 지연을 추가로
 *                예약해야 하는지 여부.
 * @return: 없음(void).
 *
 * blkcg_iolatency_throttle()이 bio의 cgroup 계층을 root까지 순회하며 각
 * iolatency_grp 노드마다 이 함수를 호출한다. 먼저 이 cgroup에 이미 누적된
 * use_delay(블록 계층이 유발한 memory 지연 부채)가 있으면
 * blkcg_schedule_throttle()로 사용자 반환 시 지연을 예약한다. 그 다음
 * issue_as_root이거나 현재 태스크가 치명적 시그널(대개 OOM kill)을 받는
 * 중이면 대기 없이 즉시 inflight를 증가시키고 통과시킨다 — 우선순위 역전을
 * 피하거나(root가 대신 내는 IO), OOM으로 죽어가는 태스크의 회복을 지연시키지
 * 않기 위함이다. 그 외의 정상 경로는 rq_qos_wait()로 max_depth 여유가 생길
 * 때까지 잠들어 기다린다.
 * 실행 컨텍스트: submit_bio 경로의 사용자/커널 태스크 컨텍스트(sleep 가능).
 * 호출자(caller): blkcg_iolatency_throttle()이 cgroup 계층을 순회하며 매
 * 노드마다 호출.
 * 호출 대상(callee): blkcg_schedule_throttle(), rq_qos_wait()
 *          (내부적으로 iolat_acquire_inflight/iolat_cleanup_cb 콜백 사용).
 *
 * 호출 체인:
 *   blkcg_iolatency_throttle() → [__blkcg_iolatency_throttle] → rq_qos_wait()
 *     → iolat_acquire_inflight() / iolat_cleanup_cb()
 */
static void __blkcg_iolatency_throttle(struct rq_qos *rqos,
				       struct iolatency_grp *iolat,
				       bool issue_as_root,
				       bool use_memdelay)
{
	struct rq_wait *rqw = &iolat->rq_wait;
	/* [한국어] atomic_read인 이유: use_delay는 완료 경로(blkcg_add_delay)와
	 * 타이머(blkiolatency_timer_fn)가 락 없이 갱신하는 값이라, 여기서는
	 * 찢어지지 않은 스냅샷 하나만 얻으면 충분하다 */
	unsigned use_delay = atomic_read(&lat_to_blkg(iolat)->use_delay);
	/* [한국어] 이 blkcg에 현재 누적된 지연 사용량(use_delay 카운트)을 읽는다.
	 * max_depth가 1까지 줄었는데도 더 스로틀해야 할 때 이 카운트가 커진다 */

	if (use_delay)
		blkcg_schedule_throttle(rqos->disk, use_memdelay);
		/* [한국어] use_delay > 0이면 memory.delay 서브시스템에 지연 예약
		 * (linux/memcontrol.h). max_depth를 더 줄일 수 없을 때 사용자 공간
		 * 반환 시점에 직접 지연을 부과하는 2차 스로틀 수단 */

	/*
	 * To avoid priority inversions we want to just take a slot if we are
	 * issuing as root.  If we're being killed off there's no point in
	 * delaying things, we may have been killed by OOM so throttling may
	 * make recovery take even longer, so just let the IO's through so the
	 * task can go away.
	 */
	if (issue_as_root || fatal_signal_pending(current)) {
		atomic_inc(&rqw->inflight);
		/* [한국어] root 발행(우선순위 역전 방지) 또는 OOM kill 시그널 대기 중이면
		 * 대기 없이 즉시 inflight 슬롯을 강제로 차지하고 진행시킨다.
		 * 실제 request/태그 할당은 이후 하위 계층(blk-mq)에서 이뤄진다 */
		return;
	}

	/* [한국어] 여기서 잠드는 것이 이 정책의 유일한 '직접적인' 스로틀 행위다.
	 * 지연을 인위적으로 넣는 게 아니라, 동시 진행 bio 수를 묶어 두면
	 * 장치 큐에서의 경쟁이 줄고 그 결과로 지연이 내려간다 */
	rq_qos_wait(rqw, iolat, iolat_acquire_inflight, iolat_cleanup_cb);
	/* [한국어] max_depth 여유가 생길 때까지 잠들어 기다린다(iolat_acquire_inflight가
	 * true를 반환할 때까지). 이 대기 시간만큼 bio 제출이 늦춰져 평균 완료
	 * 지연을 목표치 이하로 되돌리는 것이 스로틀의 핵심 메커니즘이다. */
}

#define SCALE_DOWN_FACTOR 2
/* [한국어] scale down 시 사용할 오른쪽 시프트 비트 수(qd >> 2 = qd/4).
 * SCALE_UP_FACTOR보다 작은 시프트값이라 한 번에 더 크게 줄어든다 — "빠르게
 * 줄이고 천천히 늘린다"는 비대칭 설계의 핵심 상수. */
#define SCALE_UP_FACTOR 4
/* [한국어] scale up 시 사용할 오른쪽 시프트 비트 수(qd >> 4 = qd/16).
 * SCALE_DOWN_FACTOR보다 큰 시프트값이라 한 번에 더 작게 늘어난다. */

/*
 * [한국어]
 * scale_amount - 한 번의 scale up/down에서 적용할 변화량(step) 계산.
 *
 * @qd: 기준이 되는 큐 전체 nr_requests(상한 근사치).
 * @up: true면 scale up(qd/16), false면 scale down(qd/4) 계산.
 * @return: 계산된 변화량. 단, 최소 1은 보장(0이 되어 변화가 없어지는 것 방지).
 *
 * scale up은 1/16씩 천천히, scale down은 1/4씩 빠르게 조정되도록 시프트양을
 * 다르게 두어 비대칭 반응 곡선을 만든다. 이는 "허용 깊이를
 * 기본 무제한에서 목표치까지는 빠르게 줄이되, 회복은 신중하게 조금씩"이라는
 * 설계 의도를 구현한 것이다(원문 주석 "Change the queue depth..." 참고).
 *
 * 호출 체인:
 *   scale_cookie_change()/scale_change() → [scale_amount]
 */
static inline unsigned long scale_amount(unsigned long qd, bool up)
{
	return max(up ? qd >> SCALE_UP_FACTOR : qd >> SCALE_DOWN_FACTOR, 1UL);
	/* [한국어] up이면 qd/16, 아니면 qd/4를 계산하고 최소 1UL로 하한을 둔다.
	 * qd가 매우 작을 때(시프트 결과가 0) 변화량이 완전히 사라지는 것을 방지 —
	 * nr_requests가 아주 작아도(qd/16 = 0) scale이 멈추지 않게 함 */
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
 * [한국어]
 * scale_cookie_change - 부모 child_latency_info.scale_cookie를 한 단계
 * 증가/감소시켜 자식들에게 scale 방향을 전파할 준비를 한다.
 *
 * @blkiolat: 큐 전체 nr_requests(스케일 폭 계산의 기준값)를 얻기 위한 blk_iolatency.
 * @lat_info: 조정 대상 scale_cookie를 담고 있는 부모의 child_latency_info.
 * @up: true면 scale up(qd/16씩 증가 방향), false면 scale down(qd/4씩 감소 방향).
 * @return: 없음(void). lat_info->scale_cookie가 갱신된다.
 *
 * scale_cookie는 자식들이 각자 로컬 사본(iolat->scale_cookie)과 비교해
 * scale 방향을 판단하는 전역(부모 범위) 카운터다. up 방향에서는 scale up이
 * scale down보다 느리게 진행되는 비대칭성을 보정하기 위해, 이미 상당히
 * 스로틀된 상태(diff > qd)라면 scale보다 작은 폭(1)만 증가시켜 너무 이르게
 * DEFAULT_SCALE_COOKIE에 도달해 과도하게 unthrottle되는 것을 막는다. down
 * 방향에서는 반대로 스로틀 구덩이가 지나치게 깊어지지 않도록(diff < max_scale
 * 조건) 완만하게 추가 감소시킨다. 이 함수 자체는 특정 자식의 max_depth를
 * 바꾸지 않으며, 각 자식의 check_scale_change()가 다음 submit 시점에 이
 * scale_cookie 변화를 관찰하고 반영한다.
 * 실행 컨텍스트: iolatency_check_latencies()(완료 경로, child_lat.lock 보호 하)와
 * blkiolatency_timer_fn()(타이머 컨텍스트, 동일 lock 보호 하)에서 호출.
 *
 * 호출 체인:
 *   iolatency_check_latencies()/blkiolatency_timer_fn() → [scale_cookie_change] → scale_amount()
 */
static void scale_cookie_change(struct blk_iolatency *blkiolat,
				struct child_latency_info *lat_info,
				bool up)
{
	unsigned long qd = blkiolat->rqos.disk->queue->nr_requests;
	/* [한국어] 큐 전체 nr_requests를 스케일 폭의 기준으로 삼는다. 하드웨어
	 * 큐 깊이가 아니라 blk-mq가 허용하는 소프트웨어 request 수이며,
	 * 여기서는 "이 큐의 규모"를 나타내는 척도로만 쓰인다 */
	unsigned long scale = scale_amount(qd, up);
	/* [한국어] 이번에 적용할 변화 폭(up이면 qd/16, down이면 qd/4) 계산 */
	unsigned long old = atomic_read(&lat_info->scale_cookie);
	/* [한국어] 조정 전 현재 scale_cookie 값을 읽는다 */
	unsigned long max_scale = qd << 1;
	/* [한국어] qd의 2배를 "이 이상은 깊이 파고들지 않겠다"는 스로틀 하한
	 * 기준으로 사용 */
	unsigned long diff = 0;

	/* [한국어] 현재 쿠키가 DEFAULT보다 낮다면(스로틀 중이라면) 그 차이(diff)를
	 * "현재 얼마나 스로틀되어 있는가"의 척도로 계산. DEFAULT 이상이면 diff=0 유지.
	 * 이 diff 하나로 아래의 모든 완급 조절 분기가 갈린다 — 이미 깊이 눌려 있으면
	 * 조심스럽게, 얕게 눌려 있으면 정상 폭으로 움직인다 */
	if (old < DEFAULT_SCALE_COOKIE)
		diff = DEFAULT_SCALE_COOKIE - old;

	if (up) {
		/* [한국어] scale만큼 더하면 DEFAULT를 넘어서는 경우: DEFAULT로 클램프해
		 * 오버슈트 없이 정확히 "제한 없음" 상태로 복귀. DEFAULT를 넘겨 두면
		 * 다음 scale down이 실제 효과를 내기까지 헛돌게 된다 */
		if (scale + old > DEFAULT_SCALE_COOKIE)
			atomic_set(&lat_info->scale_cookie, /* [한국어] 덧셈 대신 set — 넘침 없이 정확히 상한에 안착시킨다 */
				   DEFAULT_SCALE_COOKIE);
		else if (diff > qd)
			atomic_inc(&lat_info->scale_cookie);
		/* [한국어] 아직 스로틀 양(diff)이 qd보다 크게 남아있다면(많이 눌려있는
		 * 상태) 1만큼만 천천히 회복 — scale down은 빨랐으니 scale up은
		 * 신중하게 진행한다는 비대칭 설계의 핵심 분기 */
		else
			atomic_add(scale, &lat_info->scale_cookie);
		/* [한국어] 스로틀 양이 이미 qd 이하로 충분히 회복된 상태라면 정상적으로
		 * scale만큼 증가. 자식들의 check_scale_change()가 다음 submit에서
		 * 이 변화를 감지해 max_depth를 확대한다 */
	} else {
		/*
		 * We don't want to dig a hole so deep that it takes us hours to
		 * dig out of it.  Just enough that we don't throttle/unthrottle
		 * with jagged workloads but can still unthrottle once pressure
		 * has sufficiently dissipated.
		 */
		if (diff > qd) { /* [한국어] 이미 꽤 눌려 있는 구간 — 여기서 또 scale만큼 내리면
				  * 구덩이가 너무 깊어져 압력이 사라진 뒤에도 회복에 한참 걸린다 */
			if (diff < max_scale) /* [한국어] qd*2가 사실상의 바닥 — 이 아래로는 더 내리지 않는다 */
				atomic_dec(&lat_info->scale_cookie);
			/* [한국어] 이미 상당히 스로틀 중(diff > qd)이지만 아직
			 * max_scale(qd*2) 미만이라면 1만큼만 추가로 조심스럽게 감소.
			 * diff가 max_scale 이상이면 이 조건에 걸리지 않아 더 이상
			 * 깊어지지 않도록 자연히 제한된다 */
		} else {
			atomic_sub(scale, &lat_info->scale_cookie);
			/* [한국어] 아직 크게 스로틀되지 않은 상태라면 정상적으로
			 * scale만큼 감소. 자식들의 max_depth가 scale_change()를
			 * 통해 절반씩 줄어드는 계기가 된다 */
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
 * [한국어]
 * scale_change - 한 cgroup(iolatency_grp)의 max_depth를
 * 실제로 증가 또는 감소시킨다.
 *
 * @iolat: max_depth를 변경할 대상 iolatency_grp.
 * @up: true면 scale up(1/16씩 확대), false면 scale down(절반으로 축소).
 * @return: 없음(void). iolat->max_depth가 갱신되고, scale up 시 대기자를 깨운다.
 *
 * check_scale_change()가 부모 scale_cookie의 변화 방향을 판정한 뒤 호출하는
 * 최종 실행 단계다. scale up 경로는 이미 max_depth가 1(가장 낮은 상태)이고
 * blkcg_unuse_delay()로 지연 부채를 성공적으로 줄일 수 있었다면 그것으로
 * 충분하다고 보고 max_depth 변경 없이 조기 반환한다(지연 부채부터 우선
 * 해소). 그렇지 않고 아직 큐 상한(qd) 미만이면 scale만큼 늘리고 상한으로
 * 클램프한 뒤, rq_wait에서 대기 중이던 bio들을 모두 깨워 재시도시킨다.
 * scale down 경로는 max_depth를 절반으로 급격히 줄이되 1 미만으로는
 * 내려가지 않게 한다(완전히 막아 버리면 forward progress가 사라지므로).
 * 실행 컨텍스트: check_scale_change()의 호출 경로와 동일(submit 경로,
 * 태스크 컨텍스트).
 *
 * 호출 체인:
 *   check_scale_change() → [scale_change] → scale_amount() / blkcg_unuse_delay() / wake_up_all()
 */
static void scale_change(struct iolatency_grp *iolat, bool up)
{
	unsigned long qd = iolat->blkiolat->rqos.disk->queue->nr_requests;
	/* [한국어] max_depth의 상한으로 큐 전체 nr_requests를 사용 — 그 이상은 의미가 없다 */
	unsigned long scale = scale_amount(qd, up);
	/* [한국어] 이번에 적용할 변화 폭 계산 */
	unsigned long old = iolat->max_depth;
	/* [한국어] 변경 전 현재 max_depth 값을 지역 변수에 저장(UINT_MAX일 수 있음) */

	if (old > qd)
		old = qd;
		/* [한국어] max_depth가 UINT_MAX(무제한)이거나 nr_requests보다 큰 경우,
		 * 계산의 기준값을 qd로 재조정 — 실제 상한을 넘는 값 위에서
		 * scale 계산이 왜곡되지 않도록 함 */

	if (up) {
		if (old == 1 && blkcg_unuse_delay(lat_to_blkg(iolat)))
			return;
		/* [한국어] 이미 최소 깊이(1)이고 blkcg_unuse_delay()가 지연 부채를
		 * 하나 해소하는 데 성공했다면(true 반환), 이번 scale up 이벤트는
		 * "지연 해소"로 소비하고 max_depth는 그대로 둔 채 반환한다.
		 * memory.delay 부채부터 먼저 갚는 우선순위 */

		if (old < qd) {
			old += scale;
			/* [한국어] 아직 상한 미만이면 scale만큼 max_depth 후보를 확대 */
			old = min(old, qd);
			/* [한국어] nr_requests 상한을 넘지 않도록 클램프 */
			iolat->max_depth = old;
			/* [한국어] 계산된 새 상한을 실제 max_depth 필드에 반영 */
			wake_up_all(&iolat->rq_wait.wait);
			/* [한국어] max_depth가 늘어났으므로 rq_wait에서 대기 중이던
			 * 모든 bio(태스크)를 깨워 iolat_acquire_inflight() 재시도를
			 * 유도한다 */
		}
	} else {
		old >>= 1;
		/* [한국어] scale down은 완만한 scale 값 대신 절반으로 가파르게 감소
		 * — "기본 무제한에서 목표까지 빠르게 줄인다"는 설계 의도 그대로 */
		iolat->max_depth = max(old, 1UL);
		/* [한국어] 0이 되지 않도록 최소 1로 하한을 둔다. 큐가 완전히
		 * 막히면 그 cgroup은 영원히 IO를 낼 수 없게 되므로 방지 */
	}
}

/* Check our parent and see if the scale cookie has changed. */
/*
 * [한국어]
 * check_scale_change - 부모의 scale_cookie 변화를 감지하고 필요하면
 * 이 cgroup의 max_depth를 조정한다.
 *
 * @iolat: 부모의 scale_cookie 변화를 확인하고 반영할 대상 iolatency_grp.
 * @return: 없음(void).
 *
 * blkcg_iolatency_throttle()이 submit 경로에서 cgroup 계층을 순회하며 매
 * 노드마다(즉 매 bio 제출마다) 이 함수를 호출한다. 부모의
 * child_lat.scale_cookie(전역 방향 지시자)와 자신의 로컬 사본
 * (iolat->scale_cookie)을 비교해 증가(scale up, direction=+1)/감소
 * (scale down, direction=-1)/변화없음(조기 반환) 중 하나를 판정한다.
 * atomic_try_cmpxchg()로 로컬 사본을 먼저 갱신해 여러 CPU가 동시에 같은
 * 이벤트를 중복 처리하지 않도록 한다. scale down 판정인 경우, 자신의
 * min_lat_nsec(목표 지연)이 이미 부모가 기록한 scale_lat(위반의 근거가 된
 * 지연값)보다 더 엄격하면 (즉 자신은 원인이 아니면) 영향을 받지 않으며,
 * 최근 윈도우에서 전체 자식 IO의 5% 이하만 기여했다면("억울한 희생양" 방지)
 * 역시 scale down을 건너뛴다. max_depth가 이미 1인데 추가 scale down이면
 * blkcg_use_delay()로 memory.delay 부채를 쌓아 2차 스로틀을 유도한다.
 * scale_cookie가 DEFAULT로 복귀했으면 모든 제한을 해제(max_depth=UINT_MAX)
 * 한다. 그 외의 경우 scale_change()로 실제 max_depth를 조정한다.
 * 실행 컨텍스트: submit 경로(사용자 태스크 컨텍스트), sleep 없음.
 *
 * 호출 체인:
 *   blkcg_iolatency_throttle() → [check_scale_change] → scale_change() /
 *     blkcg_use_delay() / blkcg_clear_delay()
 */
static void check_scale_change(struct iolatency_grp *iolat)
{
	struct iolatency_grp *parent; /* [한국어] 공유 쿠키를 들고 있는 쪽 — 판단 기준은 전부 부모에 있다 */
	struct child_latency_info *lat_info; /* [한국어] 부모가 자식들을 조율하려고 들고 있는 공유 상태 */
	unsigned int cur_cookie; /* [한국어] 부모의 '지금' 값 */
	unsigned int our_cookie = atomic_read(&iolat->scale_cookie);
	/* [한국어] 자신이 마지막으로 관찰했던 부모 scale_cookie 값(로컬 사본).
	 * 절대값이 아니라 '이전에 본 값과의 차이'로 방향을 정하기 때문에,
	 * 자식마다 이 사본을 따로 들고 있어야 한 번의 변화가 자식마다
	 * 정확히 한 번씩만 반영된다 */
	u64 scale_lat; /* [한국어] 이번 스로틀의 근거가 된 목표 지연 — 자신이 원인인지 판별하는 데 쓴다 */
	int direction = 0; /* [한국어] -1=조여라, +1=풀어라, 0=변화 없음 */

	parent = blkg_to_lat(lat_to_blkg(iolat)->parent);
	/* [한국어] 부모 cgroup의 iolatency_grp을 획득. scale 전파는 부모→자식
	 * 방향으로만 이루어지는 계층 구조 */
	if (!parent)
		return;
	/* [한국어] 부모가 없으면 이 iolat이 root cgroup이라는 의미이므로 전파받을
	 * scale_cookie 자체가 없다 — 즉시 반환 */

	lat_info = &parent->child_lat;
	/* [한국어] 락 없이 원자적으로만 읽는다 — 이 경로는 모든 bio 제출마다
	 * 실행되는 hot path라 스핀락을 잡을 수 없다. 값이 한 틱 낡아도
	 * 다음 제출에서 따라잡으므로 문제되지 않는다 */
	cur_cookie = atomic_read(&lat_info->scale_cookie);
	scale_lat = READ_ONCE(lat_info->scale_lat);
	/* [한국어] 부모가 기록한 "이번 scale down의 근거가 된 목표 지연" 값을
	 * READ_ONCE로 읽어 컴파일러의 재배치/캐싱 최적화를 방지(다른 CPU의
	 * WRITE_ONCE와 짝을 이루는 최소한의 가시성 보장) */

	if (cur_cookie < our_cookie)
		direction = -1;
	/* [한국어] 부모 쿠키가 자신이 아는 값보다 작아졌다 = scale down 지시 */
	else if (cur_cookie > our_cookie)
		direction = 1;
	/* [한국어] 부모 쿠키가 커졌다 = scale up 지시 */
	else
		return;
	/* [한국어] 변화가 없으면 할 일이 없으므로 조기 반환 */

	if (!atomic_try_cmpxchg(&iolat->scale_cookie, &our_cookie, cur_cookie)) {
		/* Somebody beat us to the punch, just bail. */
		return;
	}
	/* [한국어] CAS(Compare-And-Swap) 성공 시에만 아래로 진행. 실패하면(다른
	 * 스레드가 이미 our_cookie를 다른 값으로 바꿔놓았다는 뜻) 이미 누군가
	 * 이 변화를 처리했거나 처리 중이므로 중복 작업 없이 반환한다 */

	if (direction < 0 && iolat->min_lat_nsec) {
		u64 samples_thresh;

		/* [한국어] 아래 두 검사(원인 판별 + 5% 기여도)는 "누가 이 스로틀의
		 * 대가를 치러야 하는가"를 고르는 부분이다. 쿠키는 형제 전체에게
		 * 뿌려지지만, 실제로 물러나야 하는 것은 원인 제공자보다 느슨한
		 * 목표를 가졌고 IO도 실제로 많이 낸 그룹뿐이다 */
		if (!scale_lat || iolat->min_lat_nsec <= scale_lat)
			return;
		/* [한국어] scale_lat이 아직 설정되지 않았거나(0), 자신의 목표 지연이
		 * scale_lat 이하(즉 자신이 더 엄격하거나 같은 SLO)라면 이번 scale
		 * down의 원인이 자신이 아니라고 보고 영향받지 않는다 */

		/*
		 * Sometimes high priority groups are their own worst enemy, so
		 * instead of taking it out on some poor other group that did 5%
		 * or less of the IO's for the last summation just skip this
		 * scale down event.
		 */
		samples_thresh = lat_info->nr_samples * 5;
		/* [한국어] 부모 전체 자식 샘플 수의 5%를 임계값으로 계산(최소 1).
		 * *5 후 /100 순서인 이유는 정수 나눗셈에서 먼저 나누면 nr_samples가
		 * 100 미만일 때 전부 0이 되어 버리기 때문이다. max(1ULL, ...)은
		 * 임계값 0으로 인해 모든 그룹이 무조건 통과하는 것을 막는다 */
		samples_thresh = max(1ULL, div64_u64(samples_thresh, 100));
		if (iolat->nr_samples <= samples_thresh)
			return;
		/* [한국어] 자신의 최근 기여도가 전체의 5% 이하라면, 다른 cgroup의
		 * 문제로 인한 scale down에 억울하게 함께 스로틀되지 않도록 건너뛴다 */
	}

	/* We're as low as we can go. */
	if (iolat->max_depth == 1 && direction < 0) {
		blkcg_use_delay(lat_to_blkg(iolat));
		return;
	}
	/* [한국어] max_depth가 이미 최솟값(1)인데 추가로 scale down하라는
	 * 지시라면, max_depth로는 더 표현할 수 없으므로 blkcg_use_delay()로
	 * memory.delay 서브시스템에 지연 부채를 쌓아 사용자 공간 반환 시점에서
	 * 대신 지연을 부과한다(2차 스로틀 수단) */

	/* We're back to the default cookie, unthrottle all the things. */
	if (cur_cookie == DEFAULT_SCALE_COOKIE) {
		blkcg_clear_delay(lat_to_blkg(iolat));
		/* [한국어] scale_cookie가 DEFAULT로 완전히 복귀했으므로 지연 부채를
		 * 모두 지우고 max_depth를 무제한(UINT_MAX)으로 되돌린다.
		 * 점진적으로 늘려 가는 대신 한 번에 UINT_MAX로 놓는 이유는,
		 * 쿠키가 DEFAULT라는 것이 곧 "제한이 필요 없다"는 확정 신호이고
		 * 그 상태에서 유한한 상한을 유지하면 순손해이기 때문이다 */
		iolat->max_depth = UINT_MAX;
		wake_up_all(&iolat->rq_wait.wait);
		/* [한국어] 제한이 완전히 풀렸으므로 대기 중인 모든 bio를 깨운다 */
		return;
	}
	/* [한국어] DEFAULT로의 완전한 복귀는 아니지만 방향이 정해졌으므로 아래에서
	 * scale_change()로 점진적인 조정을 수행 */

	scale_change(iolat, direction > 0);
	/* [한국어] direction > 0(scale up)이면 true, direction < 0(scale down)이면
	 * false를 넘겨 실제 max_depth 조정을 위임 */
}

/*
 * [한국어]
 * blkcg_iolatency_throttle - bio 제출 시 cgroup 계층을 root까지 순회하며
 * 각 노드의 스로틀을 적용하는 rq_qos .throttle 콜백.
 *
 * @rqos: 이 큐에 등록된 rq_qos 인스턴스(RQ_QOS_LATENCY).
 * @bio: 제출 중인 bio. bio->bi_blkg로 시작 cgroup을, bio->bi_opf로
 *       REQ_SWAP 등의 특성을 판별.
 * @return: 없음(void). 필요 시 함수 내부에서 잠들어(sleep) 반환이 늦어질 수 있다.
 *
 * rq_qos 프레임워크(block/blk-rq-qos.h)가 submit_bio 경로 상단에서
 * rq_qos_throttle()을 통해 등록된 모든 QoS 정책의 .throttle 콜백을 순서대로
 * 호출하며, 이 함수는 io.latency 정책의 진입점이다. 먼저 blkiolat->enabled가
 * false(활성화된 cgroup이 하나도 없음)이면 즉시 통과시켜 오버헤드를 없앤다.
 * 이후 bio->bi_blkg에서 시작해 blkg->parent를 따라 root 직전까지(root
 * 자신은 부모가 없으므로 루프 조건에서 자동 제외) 올라가며, 각 노드마다
 * check_scale_change()로 부모의 scale_cookie 변화를 반영한 뒤
 * __blkcg_iolatency_throttle()로 실제 대기/통과 여부를 결정한다. REQ_SWAP
 * 플래그가 설정된 bio는 use_memdelay=true로 넘겨 memory.delay 서브시스템과
 * 연동한다. 순회가 끝나면 회복 타이머가 아직 무장되지 않았다면 1초 뒤로
 * 무장해, 향후 IO가 뜸해져도 scale_cookie가 회복될 기회를 보장한다.
 * 실행 컨텍스트: submit_bio 경로의 태스크 컨텍스트(sleep 가능).
 * 호출자(caller): rq_qos_throttle() → blkcg_iolatency_ops.throttle.
 * 호출 대상(callee): check_scale_change(), __blkcg_iolatency_throttle(), mod_timer().
 *
 * 호출 체인:
 *   submit_bio() → blk_mq_submit_bio() → rq_qos_throttle()
 *     → [blkcg_iolatency_throttle] → check_scale_change() / __blkcg_iolatency_throttle()
 */
static void blkcg_iolatency_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);
	/* [한국어] 이 큐의 blk_iolatency 인스턴스 획득 */
	struct blkcg_gq *blkg = bio->bi_blkg;
	/* [한국어] bio가 속한 (cgroup, 디스크) 조합. 이 포인터에서 시작해 부모로
	 * 거슬러 올라가며 계층 전체를 검사한다 */
	bool issue_as_root = bio_issue_as_root_blkg(bio);
	/* [한국어] REQ_META 등으로 인해 이 bio가 우선순위 역전 방지를 위해 root
	 * cgroup 권한으로 발행되어야 하는지 판별(block/blk-cgroup.h 헬퍼) */

	if (!blkiolat->enabled)
		return;
	/* [한국어] 이 큐에서 io.latency가 활성화된 cgroup이 하나도 없으면(마스터
	 * 스위치 off) 스로틀 로직 전체를 건너뛰어 오버헤드를 없앤다 */

	while (blkg && blkg->parent) {
		/* [한국어] blkg->parent가 있는 동안(= blkg가 root가 아닌 동안) 반복 —
		 * root cgroup 자신은 스로틀 대상이 아니므로 루프 조건에서 자연히 제외 */
		struct iolatency_grp *iolat = blkg_to_lat(blkg);
		/* [한국어] 이 blkg에 iolatency policy data가 아직 없으면(비활성)
		 * 이 노드는 건너뛰고 한 단계 위 부모로 이동해 계속 순회 */
		if (!iolat) { /* [한국어] 정책이 붙지 않은 중간 노드도 계층에 섞여 있을 수 있다 */
			blkg = blkg->parent; /* [한국어] 건너뛰되 순회는 계속 — 위쪽에 목표를 가진 조상이 있을 수 있다 */
			continue;
		}

		/* [한국어] 스로틀 여부를 판단하기 '전에' 먼저 쿠키를 반영한다.
		 * 그래야 방금 형제가 일으킨 변화가 이번 bio부터 곧바로 적용된다 */
		check_scale_change(iolat);
		/* [한국어] 부모로부터 전파된 scale_cookie 변화를 이 노드에 반영.
		 * submit 시점마다(매 bio) 최신 스로틀 목표를 따라가기 위함 */
		__blkcg_iolatency_throttle(rqos, iolat, issue_as_root,
				     (bio->bi_opf & REQ_SWAP) == REQ_SWAP);
		/* [한국어] bio->bi_opf & REQ_SWAP: bio 연산 플래그에서 REQ_SWAP 비트를
		 * 테스트. swap-out IO라면 use_memdelay=true로 넘겨져 memory.delay에
		 * 지연을 추가로 예약할 수 있게 한다 */
		blkg = blkg->parent;
		/* [한국어] 한 단계 위 조상 cgroup으로 이동해 계속 순회 */
	}
	if (!timer_pending(&blkiolat->timer))
		mod_timer(&blkiolat->timer, jiffies + HZ);
	/* [한국어] 회복 타이머가 아직 무장되지 않았다면 1초(HZ) 뒤로 무장.
	 * 스로틀이 걸린 이후 IO가 뜸해지더라도(submit 경로가 호출되지 않아도)
	 * blkiolatency_timer_fn()이 주기적으로 scale_cookie 회복을 시도하게 한다 */
}

/*
 * [한국어]
 * iolatency_record_time - 완료된 bio 한 건의 지연을 계산해 통계에 반영하거나,
 * root 대납 IO의 경우 해당 cgroup에 memory.delay를 부과.
 *
 * @iolat: 지연을 기록할 대상 cgroup의 iolatency_grp.
 * @start: bio->issue_time_ns — 이 bio가 처음 제출된 시각(nsec).
 * @now: blk_time_get_ns() — 완료를 처리 중인 현재 시각(nsec).
 * @issue_as_root: bio_issue_as_root_blkg(bio) — 이 bio가 root cgroup 권한으로
 *                 발행되었는지(원래 소유 cgroup이 아니라 대납된 IO인지) 여부.
 * @return: 없음(void).
 *
 * now가 start 이하이면(시각 역전 — IRQ 지연, 타이머 오차 등으로 발생 가능한
 * 비정상 값) 통계를 왜곡하지 않도록 조용히 무시한다. 정상적인 경우
 * req_time(=now-start)을 계산하는데, 이는 이 bio가 블록 계층에 제출된
 * 순간부터 완료 처리 시점까지의 전체 경과 시간이다. 장치가 실제로 명령을
 * 처리한 시간뿐 아니라 그 앞뒤의 블록 계층 체류 시간까지 포함한다.
 * issue_as_root인 bio는 원래 소유 cgroup의 통계에 포함시키면 실제보다 지연이
 * 낮게 보이는 왜곡(root가 우선권을 가지므로 더 빠르게 처리됨)이 생기므로,
 * 이 bio가 통계에 반영되는 대신 min_lat_nsec에 못 미친 부족분만큼
 * blkcg_add_delay()로 해당 cgroup에 memory.delay를 부과한다(단, max_depth가
 * 이미 UINT_MAX인, 즉 전혀 스로틀되지 않은 cgroup은 이 처리조차 건너뛴다).
 * 그 외의 정상적인 자기 발행 bio는 latency_stat_record_time()으로 per-CPU
 * 통계에 반영한다.
 * 실행 컨텍스트: blkcg_iolatency_done_bio()의 완료 경로(softirq/IRQ 컨텍스트 가능).
 *
 * 호출 체인:
 *   blkcg_iolatency_done_bio() → [iolatency_record_time] → blkcg_add_delay() /
 *     latency_stat_record_time()
 */
static void iolatency_record_time(struct iolatency_grp *iolat, u64 start,
				  u64 now, bool issue_as_root)
{
	u64 req_time;

	/* [한국어] 시각 역전(now <= start)은 비정상 값이므로 통계를 오염시키지
	 * 않도록 아무 것도 기록하지 않고 반환. 부호 없는 뺄셈이라 그대로 두면
	 * 거대한 양수 지연으로 둔갑해 즉시 SLO 위반으로 오판된다 */
	if (now <= start)
		return;

	req_time = now - start;
	/* [한국어] bio->issue_time_ns부터 현재까지의 경과 시간을 계산한다.
	 * 주의: 이 값은 "장치가 커맨드를 처리한 시간"이 아니라 "블록 계층에
	 * 진입한 시점부터 완료까지"의 전체 지연이다. 따라서 큐 대기 시간,
	 * 스케줄러 지연, 스로틀 대기가 모두 포함된다. 즉 장치가 명령을 받은 뒤
	 * 완료를 보고하기까지의 순수 장치 지연보다 훨씬 넓은 범위이며,
	 * "이 cgroup의 IO가 체감상 얼마나 느렸는가"에 더 가깝다.
	 * blk-iolatency의 목표가 "cgroup이 체감하는 지연"을 제어하는 것이므로
	 * 이 넓은 범위가 오히려 올바른 측정 대상이다. */

	/*
	 * We don't want to count issue_as_root bio's in the cgroups latency
	 * statistics as it could skew the numbers downwards.
	 */
	if (unlikely(issue_as_root && iolat->max_depth != UINT_MAX)) {
		u64 sub = iolat->min_lat_nsec;
		/* [한국어] 이 cgroup의 목표 지연을 "채워야 할 부족분" 계산의 기준으로 저장 */
		if (req_time < sub)
			blkcg_add_delay(lat_to_blkg(iolat), now, sub - req_time);
		/* [한국어] root가 대신 처리해 준 덕분에 실제 지연(req_time)이 목표
		 * (sub)보다 짧게 끝났다면, 그 차이(sub - req_time)만큼을 이
		 * cgroup의 memory.delay 부채로 추가한다. 원래 cgroup이 부담했어야
		 * 할 지연을 root가 대신 흡수해 준 만큼 나중에 갚게 하는 셈이다.
		 * max_depth == UINT_MAX(전혀 스로틀되지 않는 cgroup)이면 이 블록
		 * 자체에 진입하지 않아 부채가 쌓이지 않는다 */
		return;
	}
	/* [한국어] root 대납 IO(그리고 이 cgroup이 실제로 스로틀 중인 경우)는
	 * 지연 통계에 포함하지 않고 위에서 delay 부과만 하고 반환했다.
	 * 아래는 "자기 자신이 정상적으로 발행한 IO"만 도달하는 경로다 */

	latency_stat_record_time(iolat, req_time);
	/* [한국어] per-CPU 통계에 이번 IO의 지연을 기록. 비회전식이면 missed/total
	 * 카운터, HDD면 평균/분산 통계에 반영된다 */
}

#define BLKIOLATENCY_MIN_ADJUST_TIME (500 * NSEC_PER_MSEC)
/* [한국어] scale_cookie 재조정 사이에 강제되는 최소 간격(500ms). 너무 자주
 * scale up/down이 반복되면 워크로드가 요동칠 때마다 max_depth가 출렁여
 * 오히려 성능이 불안정해지므로, 이 값으로 디바운스한다. */
#define BLKIOLATENCY_MIN_GOOD_SAMPLES 5
/* [한국어] scale up(회복)을 허용하기 위한 최소 "양호한" 표본 수. IO가 너무
 * 적은 상태에서 우연히 SLO를 만족한 것으로 착각해 무리하게 회복하는 것을
 * 방지한다. */

/*
 * [한국어]
 * iolatency_check_latencies - 통계 윈도우가 만료될 때마다 per-CPU 지연
 * 통계를 집계하고, SLO 위반/만족 여부에 따라 부모의 scale_cookie를 조정.
 *
 * @iolat: 윈도우가 만료된 cgroup의 iolatency_grp.
 * @now: blk_time_get_ns() — 이 평가를 수행하는 현재 시각(nsec). 디바운스
 *       판정(last_scale_event와 비교)과 scale_lat 갱신 시각 기록에 사용.
 * @return: 없음(void).
 *
 * blkcg_iolatency_done_bio()가 window_start + cur_win_nsec 경과를 감지하면
 * 호출된다. 먼저 preempt_disable() 하에 모든 온라인 CPU의 per-CPU 슬롯을
 * 순회하며 stat에 합산하고 각 슬롯을 리셋해, 이번 윈도우 동안 분산 기록된
 * 완료 지연을 하나의 stat으로 모은다. 부모가 없으면(자신이 root 바로 아래가
 * 아니거나 아직 초기화 순서상 부모 pd가 없으면) 더 진행할 수 없으므로 반환한다.
 * HDD 모드에서는 iolat_update_total_lat_avg()로 장기 평균을 갱신한다. 이번
 * 윈도우가 SLO를 만족하고 부모의 scale_cookie도 이미 DEFAULT(제한 없음)라면
 * 더 할 일이 없어 조기 반환한다(락 획득조차 생략해 오버헤드를 줄임). 그 외의
 * 경우 부모의 child_lat.lock을 잡고, 이번 통계를 누적 버퍼(cur_stat)에 더하고
 * 자신의 최신 샘플 수를 부모의 nr_samples 합계에 반영한다. 마지막 조정으로부터
 * BLKIOLATENCY_MIN_ADJUST_TIME(500ms)이 지나지 않았으면 디바운스를 위해
 * 조정 없이 빠져나간다. 그 이후, "직전 누적분(cur_stat)"과 "이번 윈도우(stat)"
 * 양쪽 모두 SLO를 만족하면서 표본 수가 충분하면(BLKIOLATENCY_MIN_GOOD_SAMPLES
 * 이상) 자신이 scale down의 책임자(scale_grp)였던 경우에 한해 scale up을
 * 시작한다. 반대로 SLO를 위반했고 자신이 현재 부모가 기록한 scale_lat보다
 * 엄격하거나(더 낮은 목표) 아직 scale_grp이 없다면 자신을 책임자로 등록하고
 * scale down을 개시한다. 조정이 일어났으면 cur_stat을 리셋해 다음 윈도우를
 * 새로 시작한다.
 * 실행 컨텍스트: blkcg_iolatency_done_bio()와 동일(완료 경로, IRQ/softirq
 * 가능) — child_lat.lock을 spin_lock_irqsave()로 잡아 자기 자신과의
 * 재진입(다른 CPU의 동시 완료 처리)으로부터 보호.
 *
 * 호출 체인:
 *   blkcg_iolatency_done_bio() → [iolatency_check_latencies] →
 *     latency_stat_sum() / scale_cookie_change()
 */
static void iolatency_check_latencies(struct iolatency_grp *iolat, u64 now)
{
	struct blkcg_gq *blkg = lat_to_blkg(iolat);
	struct iolatency_grp *parent; /* [한국어] 쿠키를 실제로 조정할 대상은 자신이 아니라 부모다 */
	struct child_latency_info *lat_info; /* [한국어] 부모의 공유 조율 상태 (스핀락으로 보호) */
	struct latency_stat stat; /* [한국어] 이번 윈도우의 per-CPU 합계를 담을 스택 버퍼 */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장/복원할 인터럽트 상태 */
	int cpu; /* [한국어] 온라인 CPU 순회 인덱스 */

	/* [한국어] 집계용 임시 stat을 0으로 초기화 — 스택 변수라 반드시 명시적으로 비워야 한다 */
	latency_stat_init(iolat, &stat);
	preempt_disable();
	/* [한국어] per-CPU 슬롯을 순회하는 동안 현재 CPU에서 밀려나 다른 CPU로
	 * 이동하는 것을 막아, per_cpu_ptr()로 얻은 포인터들이 순회 내내
	 * 일관되게 "논리적으로 각 CPU 전용"임을 보장 */
	for_each_online_cpu(cpu) { /* [한국어] online만 도는 이유: offline CPU의 슬롯에는
				    * 이 윈도우 동안 새로 쌓인 것이 없다 */
		struct latency_stat *s;
		/* [한국어] cpu번 CPU 전용 슬롯 포인터 획득. 완료 인터럽트가
		 * 도달한 CPU별로 쌓인 값을 하나씩 꺼내는 단계 */
		s = per_cpu_ptr(iolat->stats, cpu);
		latency_stat_sum(iolat, &stat, s);
		/* [한국어] 이 CPU 슬롯의 값을 stat에 누적 */
		latency_stat_init(iolat, s);
		/* [한국어] 누적을 마친 이 CPU 슬롯은 즉시 0으로 리셋해 다음 윈도우를
		 * 준비 — 즉 "읽으면서 동시에 비우는" 방식으로 이중 계산을 방지 */
	}
	preempt_enable();
	/* [한국어] 순회가 끝났으므로 preemption 재활성화 */

	parent = blkg_to_lat(blkg->parent);
	/* [한국어] 부모의 iolatency_grp을 얻을 수 없으면(자신이 root거나 초기화
	 * 순서상 부모 pd가 아직 없음) scale 판단 자체가 불가능하므로 반환.
	 * 주의: 위의 per-CPU 수집은 이미 끝났으므로 통계는 정상적으로 비워진다 */
	if (!parent)
		return;

	lat_info = &parent->child_lat;
	/* [한국어] 이후 scale_cookie 조정에 사용할 부모의 공유 상태 포인터 확보 */

	iolat_update_total_lat_avg(iolat, &stat);
	/* [한국어] HDD 모드일 때만 장기 평균(lat_avg)을 이번 윈도우 값으로 갱신
	 * (비회전식은 함수 내부에서 조기 반환) */

	/* Everything is ok and we don't need to adjust the scale. */
	if (latency_sum_ok(iolat, &stat) &&
	    atomic_read(&lat_info->scale_cookie) == DEFAULT_SCALE_COOKIE)
		return;
	/* [한국어] 이번 윈도우가 SLO를 만족하고, 부모의 scale_cookie도 이미
	 * DEFAULT(스로틀 없음)라면 손댈 것이 없다 — 락 획득 없이 빠르게 반환해
	 * 정상 상태에서의 오버헤드를 최소화 */

	/* Somebody beat us to the punch, just bail. */
	spin_lock_irqsave(&lat_info->lock, flags);
	/* [한국어] 부모의 child_latency_info를 잠근다. irqsave 변형을 쓰는 이유는
	 * 이 함수 자체가 IRQ/softirq 컨텍스트에서 호출될 수 있어 일반 spin_lock은
	 * 데드락(같은 CPU의 인터럽트로 재진입) 위험이 있기 때문 */

	latency_stat_sum(iolat, &iolat->cur_stat, &stat);
	/* [한국어] 이번 윈도우 값을 누적 버퍼(cur_stat)에 더한다 — "직전 + 이번"
	 * 두 윈도우 연속 만족 여부를 판단하기 위한 준비 */
	lat_info->nr_samples -= iolat->nr_samples;
	/* [한국어] 부모의 전체 합계에서 이 cgroup이 "이전에" 기여했던 샘플 수를 뺀다
	 * (스핀락으로 보호되므로 원자 연산 없이도 안전) */
	lat_info->nr_samples += latency_stat_samples(iolat, &iolat->cur_stat);
	/* [한국어] 그 자리에 "지금" 누적된 최신 샘플 수를 다시 더해 부모의 합계를
	 * 최신 상태로 갱신 — 즉 "치환" 효과 */
	iolat->nr_samples = latency_stat_samples(iolat, &iolat->cur_stat);
	/* [한국어] 자신의 최신 샘플 수를 저장해 두어, check_scale_change()의 5%
	 * 기여도 임계값 계산에서 분자로 사용될 수 있게 한다 */

	if ((lat_info->last_scale_event >= now ||
	    now - lat_info->last_scale_event < BLKIOLATENCY_MIN_ADJUST_TIME))
		goto out;
	/* [한국어] 마지막 조정 이후 500ms가 지나지 않았거나 시각이 역전되었다면
	 * (last_scale_event >= now) 디바운스를 위해 이번엔 조정하지 않고 락만
	 * 풀고 반환 */

	if (latency_sum_ok(iolat, &iolat->cur_stat) &&
	    latency_sum_ok(iolat, &stat)) {
		/* [한국어] "직전 누적 + 이번 윈도우" 모두 SLO를 만족하더라도 표본이
		 * 5개 미만이면 신뢰할 수 없으므로 scale up을 보류 — IO가 거의 없는
		 * 구간의 "좋아 보이는" 통계로 스로틀을 풀면, 부하가 돌아오는 순간
		 * 다시 위반이 나서 진동한다 */
		if (latency_stat_samples(iolat, &iolat->cur_stat) <
		    BLKIOLATENCY_MIN_GOOD_SAMPLES)
			goto out; /* [한국어] 락을 잡은 채로 왔으므로 반드시 out 라벨을 거쳐 해제해야 한다 */
		if (lat_info->scale_grp == iolat) { /* [한국어] 스로틀을 시작시킨 당사자만 그것을 풀 수 있다 —
						     * 아무나 풀면 원인 그룹이 아직 목표를 못 지키는데도 압력이 사라진다 */
			lat_info->last_scale_event = now; /* [한국어] 디바운스 시각 갱신: 최소 500ms 간격 유지 */
			scale_cookie_change(iolat->blkiolat, lat_info, true); /* [한국어] 형제들에게 "풀어도 된다"를 전파 */
		}
		/* [한국어] 자신이 이전 scale down의 책임자(scale_grp)였고, 이제
		 * 충분한 표본과 함께 SLO를 만족하므로 회복(scale up)을 시작.
		 * 책임자가 아니면(다른 cgroup 문제였다면) 이 조건에 걸리지 않아
		 * 아무 것도 하지 않는다 */
	} else if (lat_info->scale_lat == 0 ||
		   lat_info->scale_lat >= iolat->min_lat_nsec) {
		lat_info->last_scale_event = now;
		/* [한국어] 책임자를 교체하는 조건: 아직 아무도 없거나(!scale_grp),
		 * 기존 책임자보다 내 목표가 더 엄격한 경우. 가장 엄격한 목표를
		 * 기준으로 삼아야 check_scale_change()의 "나는 원인이 아니다" 판별이
		 * 올바르게 동작한다 */
		if (!lat_info->scale_grp ||
		    lat_info->scale_lat > iolat->min_lat_nsec) {
			/* [한국어] READ_ONCE(check_scale_change)와 짝을 맞춘 WRITE_ONCE —
			 * 그쪽은 락 없이 읽으므로 컴파일러가 쪼개거나 재배치하면 안 된다 */
			WRITE_ONCE(lat_info->scale_lat, iolat->min_lat_nsec);
			/* [한국어] scale_lat을 자신의 (더 엄격한) 목표 지연으로 갱신.
			 * WRITE_ONCE로 check_scale_change()의 READ_ONCE와 짝을 맞춰
			 * 최소한의 가시성/재배치 방지를 보장 */
			lat_info->scale_grp = iolat;
			/* [한국어] 이번 scale down의 책임자로 자신을 등록 */
		}
		scale_cookie_change(iolat->blkiolat, lat_info, false);
		/* [한국어] SLO 위반이 확인되었으므로 부모의 scale_cookie를 감소
		 * 방향으로 조정 — 이후 모든 자식들의 check_scale_change()가 이를
		 * 감지해 max_depth를 줄인다 */
	}
	latency_stat_init(iolat, &iolat->cur_stat);
	/* [한국어] scale 조정이 이뤄졌으므로(up 또는 down 분기 중 하나를 탔으므로)
	 * cur_stat을 리셋해 다음 평가 사이클을 깨끗하게 시작 */
out:
	spin_unlock_irqrestore(&lat_info->lock, flags);
	/* [한국어] 조기 goto(디바운스/표본 부족)든 정상 흐름 끝이든 이 라벨에서
	 * 반드시 락을 해제 */
}

/*
 * [한국어]
 * blkcg_iolatency_done_bio - bio 완료 시 cgroup 계층을 순회하며 inflight를
 * 감소시키고, 지연을 기록하며, 통계 윈도우 만료를 검사하는 rq_qos .done_bio 콜백.
 *
 * @rqos: 이 큐의 rq_qos 인스턴스.
 * @bio: 완료 처리 중인 bio. bio->bi_blkg로 시작 cgroup, bio->issue_time_ns로
 *       제출 시각, bio->bi_status로 실제 제출 여부(BLK_STS_AGAIN)를 판별.
 * @return: 없음(void).
 *
 * bio 완료 시 rq_qos_done_bio()가 등록된 모든 QoS 정책의 .done_bio를
 * 호출하며, 이 함수가 io.latency의 완료 처리 진입점이다. 먼저 이 bio가
 * BIO_QOS_THROTTLED로 표시되어 있는지 확인한다 — throttle() 단계에서
 * inflight를 실제로 증가시킨 bio만 이 플래그가 있으므로, 없으면 대칭되는
 * 감소 처리가 불필요해 조기 반환한다. 이후 blkiolat->enabled가 꺼져 있으면
 * (마스터 스위치 off) 마찬가지로 통계를 건드리지 않는다. now(완료 시각)를
 * 한 번 구한 뒤, bio->bi_blkg에서 시작해 root 직전까지 조상 cgroup을
 * 순회하며 각 노드의 rq_wait.inflight를 하나씩 감소시키고(atomic_dec_return,
 * 음수가 되면 WARN — 제출/완료 카운트 불일치를 조기 발견), min_lat_nsec이
 * 설정되어 있고 bio가 실제로 하위 계층에 제출되었던 경우(BLK_STS_AGAIN이
 * 아님)에 한해 iolatency_record_time()으로 지연을 기록한다. 그 다음
 * window_start를 확인해 현재 통계 윈도우(cur_win_nsec)가 만료되었으면
 * atomic64_try_cmpxchg()로 새 윈도우 시작 시각으로 교체를 시도하고, 성공한
 * (즉 이 완료가 윈도우 만료를 "처음" 감지한) 경우에만
 * iolatency_check_latencies()를 호출해 scale 조정 여부를 판단한다. 마지막으로
 * inflight 감소를 대기 중인 submitter에게 wake_up()으로 알린다.
 * 실행 컨텍스트: bio 완료 경로 — 블록 계층 softirq 또는 완료 인터럽트 문맥에서
 * 호출될 수 있다(장치 완료 인터럽트에서 이어지는 지점). 이 때문에 이 함수가
 * 호출하는 iolatency_check_latencies()의 락 획득도 irqsave 변형을 사용한다.
 * 호출자(caller): rq_qos_done_bio() → blkcg_iolatency_ops.done_bio.
 * 호출 대상(callee): iolatency_record_time(), iolatency_check_latencies().
 *
 * 호출 체인:
 *   bio_endio() → rq_qos_done_bio() → [blkcg_iolatency_done_bio] →
 *     iolatency_record_time() / iolatency_check_latencies()
 */
static void blkcg_iolatency_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw; /* [한국어] 계층 각 노드의 inflight 카운터/대기열 */
	struct iolatency_grp *iolat; /* [한국어] 순회 중인 노드의 정책 데이터 */
	u64 window_start; /* [한국어] 이 노드의 현재 통계 윈도우 시작 시각 */
	u64 now; /* [한국어] 계층 전체에 대해 한 번만 읽는 기준 시각 */
	bool issue_as_root = bio_issue_as_root_blkg(bio); /* [한국어] throttle 때와 같은 판정을 다시 해야
							   * inflight 증감이 대칭을 이룬다 */
	int inflight = 0; /* [한국어] 감소 후 남은 in-flight 수 — 깨울지 판단하는 데 쓴다 */

	blkg = bio->bi_blkg;
	/* [한국어] 이 bio가 속한 (cgroup, 디스크) 조합. 이후 계층을 거슬러 오르는
	 * 순회의 시작점 */
	if (!blkg || !bio_flagged(bio, BIO_QOS_THROTTLED))
		return;
	/* [한국어] blkg가 없거나(이례적) BIO_QOS_THROTTLED 플래그가 없으면(즉
	 * throttle() 단계에서 이 bio에 대해 inflight를 증가시킨 적이 없으면)
	 * 대칭 처리를 할 필요가 없으므로 조기 반환 */

	iolat = blkg_to_lat(bio->bi_blkg);
	if (!iolat)
		return;
	/* [한국어] 시작 cgroup 자체에 iolatency policy data가 없으면(비활성)
	 * 완료 처리를 할 대상이 없으므로 반환 */

	if (!iolat->blkiolat->enabled)
		return;
	/* [한국어] iolatency 마스터 스위치가 꺼져 있으면(이 큐의 어떤 cgroup도
	 * target을 설정하지 않은 상태) 완료 통계를 수집하지 않는다. enable/disable
	 * 전환은 queue freeze 하에서만 일어나므로 inflight 카운트가 도중에
	 * 불일치할 위험은 없다 */

	now = blk_time_get_ns();
	/* [한국어] 이번 완료 처리 전체에 사용할 현재 시각을 한 번만 구해 계층
	 * 순회 동안 일관된 기준시각을 사용 */
	while (blkg && blkg->parent) {
		iolat = blkg_to_lat(blkg);
		/* [한국어] 이 노드에 policy data가 없으면 건너뛰고 부모로 이동.
		 * throttle 경로도 동일하게 건너뛰었으므로 증감이 어긋나지 않는다 */
		if (!iolat) { /* [한국어] 정책이 붙지 않은 노드 */
			blkg = blkg->parent;
			continue;
		}
		/* [한국어] 이 cgroup의 inflight 카운터/대기열 포인터 확보 */
		rqw = &iolat->rq_wait;

		/* [한국어] dec 후 값을 함께 받는 이유: 감소와 "지금 몇 개 남았나"를
		 * 따로 읽으면 그 사이에 다른 CPU가 값을 바꿔, 깨울지 판단하는 근거가
		 * 실제 상태와 어긋난다 */
		inflight = atomic_dec_return(&rqw->inflight);
		/* [한국어] 완료된 IO 한 건만큼 inflight를 원자적으로 감소시키고
		 * 감소 후의 값을 반환받는다(제출 시 iolat_acquire_inflight/
		 * __blkcg_iolatency_throttle에서 증가시킨 것과 짝) */
		WARN_ON_ONCE(inflight < 0);
		/* [한국어] inflight가 음수가 되면 감소가 증가보다 많았다는 뜻 —
		 * 제출/완료 카운트 불일치(이중 완료 또는 누락된 제출)를 나타내는
		 * 버그 신호이므로 커널 경고를 남긴다 */
		/*
		 * If bi_status is BLK_STS_AGAIN, the bio wasn't actually
		 * submitted, so do not account for it.
		 */
		if (iolat->min_lat_nsec && bio->bi_status != BLK_STS_AGAIN) {
			/* [한국어] 이 cgroup에 목표 지연이 설정되어 있고(min_lat_nsec != 0),
			 * bio가 실제로 하위 계층까지 제출되었던 경우(BLK_STS_AGAIN이면
			 * 재시도 대상이라 실제 완료가 아니므로 제외)에만 통계에 반영 */
			iolatency_record_time(iolat, bio->issue_time_ns, now,
					      issue_as_root);
			/* [한국어] bio->issue_time_ns(제출 시각)부터 now(완료 시각)까지의
			 * 지연을 계산해 통계에 반영하거나 root 대납 delay를 부과 */
			window_start = atomic64_read(&iolat->window_start);
			/* [한국어] 현재 통계 윈도우 시작 시각을 원자적으로 로드
			 * (atomic64_read로 32비트 아키텍처에서도 64비트 값을 안전하게 읽음) */
			if (now > window_start &&
			    (now - window_start) >= iolat->cur_win_nsec) {
				/* [한국어] 경과 시간이 윈도우 크기 이상이면 윈도우 만료 —
				 * 통계 평가 시점에 도달 */
				if (atomic64_try_cmpxchg(&iolat->window_start,
							 &window_start, now))
					iolatency_check_latencies(iolat, now);
				/* [한국어] CAS로 window_start를 now로 교체하는 데 성공한
				 * 완료(=이 윈도우 만료를 "최초로" 감지한 완료)만
				 * iolatency_check_latencies()를 실행. 여러 CPU가 동시에
				 * 만료를 감지해도 정확히 한 번만 평가되도록 보장 */
			}
		}
		wake_up(&rqw->wait);
		/* [한국어] inflight가 하나 줄었으므로 이 cgroup의 rq_wait에서 대기
		 * 중인 제출자가 있다면 깨워 재시도 기회를 준다 */
		blkg = blkg->parent;
		/* [한국어] 한 단계 위 조상 cgroup으로 이동해 계속 순회 */
	}
}

/*
 * [한국어]
 * blkcg_iolatency_exit - io.latency QoS 인스턴스를 정리하고 메모리를 해제하는
 * rq_qos .exit 콜백.
 *
 * @rqos: 해제할 blk_iolatency를 포함하는 rq_qos 인스턴스.
 * @return: 없음(void).
 *
 * 디스크/큐가 소멸되거나 rq_qos_del()로 이 정책이 제거될 때 호출된다.
 * timer_shutdown_sync()로 회복 타이머가 더 이상 실행되지 않음을 보장하고
 * (진행 중인 콜백이 있다면 완료까지 대기), flush_work()로 enable_work가
 * 완료될 때까지 기다려 queue freeze 기반 enabled 토글 작업과의 경쟁을
 * 없앤다. 이 두 동기화가 끝나야만 blkiolat 메모리 접근이 더 이상 일어나지
 * 않음을 보장할 수 있다. blkcg_deactivate_policy()로 이 큐의 모든
 * iolatency_grp(각 cgroup의 policy data)을 제거한 뒤, 마지막으로 blkiolat
 * 자체를 해제한다.
 * 실행 컨텍스트: 큐 해제 경로(sleep 가능한 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   rq_qos_del()/디스크 해제 경로 → [blkcg_iolatency_exit] →
 *     timer_shutdown_sync() / flush_work() / blkcg_deactivate_policy()
 */
static void blkcg_iolatency_exit(struct rq_qos *rqos)
{
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);

	timer_shutdown_sync(&blkiolat->timer);
	/* [한국어] 회복 타이머를 동기적으로 완전히 종료 — 이후 blkiolatency_timer_fn()이
	 * 실행되지 않음을 보장해야 blkiolat을 안전하게 해제할 수 있다 */
	flush_work(&blkiolat->enable_work);
	/* [한국어] enable_work가 이미 큐잉/실행 중이었다면 완료까지 대기. queue
	 * freeze 하에서 enabled 플래그를 만지는 작업이 해제 도중 실행되는 것을 방지 */
	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_iolatency);
	/* [한국어] 이 디스크의 모든 cgroup에서 iolatency policy data(iolatency_grp)를
	 * 제거. 각 pd에 대해 iolatency_pd_offline()/iolatency_pd_free()가 호출된다 */
	kfree(blkiolat);
	/* [한국어] blk_iolatency 구조체 자체를 해제 — 이 시점 이후로는 rqos를 통한
	 * 접근이 없어야 한다(rq_qos_del()이 리스트에서 먼저 제거했다고 전제) */
}

static const struct rq_qos_ops blkcg_iolatency_ops = {
	.throttle = blkcg_iolatency_throttle,
	/* [한국어] submit_bio -> blk_mq_submit_bio 경로 상단, rq_qos_throttle()에서
	 * 호출되는 제출 시점 콜백. bio가 request로 바뀌기도 전에
	 * 지연 제어(대기)를 수행하는 지점 */
	.done_bio = blkcg_iolatency_done_bio,
	/* [한국어] bio 완료 시 rq_qos_done_bio()를 통해 호출되는 완료 시점 콜백.
	 * bio 완료 처리 경로와 연결되는 지점 */
	.exit = blkcg_iolatency_exit,
	/* [한국어] 큐/디스크 해제 시 rq_qos_del() 경로에서 호출되어 blk_iolatency를
	 * 정리하는 콜백 */
};

/*
 * [한국어]
 * blkiolatency_timer_fn - 1초(HZ) 주기 타이머 콜백. 스로틀이 걸린 채 방치된
 * cgroup들의 scale_cookie를 점진적으로 회복시킨다.
 *
 * @t: 이 콜백이 연결된 timer_list(blk_iolatency.timer). timer_container_of()로
 *     상위 blk_iolatency를 역참조.
 * @return: 없음(void). 타이머 콜백 관례상 반환값 없음.
 *
 * submit 경로(blkcg_iolatency_throttle)가 매 스로틀마다 mod_timer()로 이
 * 타이머를 1초 뒤로 재무장하므로, IO가 활발한 동안에는 계속 미뤄지다가
 * IO가 뜸해지는 순간(재무장이 멈추는 순간)에야 실제로 발동한다. 발동하면
 * RCU 보호 하에 root_blkg부터 모든 자손 blkcg_gq를 전위(pre-order) 순회하며,
 * 각 cgroup이 "부모" 입장에서 관리하는 child_lat.scale_cookie가
 * DEFAULT_SCALE_COOKIE 미만(=자식들에게 스로틀을 지시 중)이면 회복을 검토한다.
 * scale_grp이 NULL이면(책임 소재가 불명확하면) 즉시 scale up을 시도하고,
 * scale_grp이 있으면 마지막 조정으로부터 5초가 지났는지 확인해 지났다면
 * "그 cgroup이 더 이상 IO를 내지 않아 스스로 회복 신호를 주지 못하는
 * 상황일 수 있다"고 보고 scale_grp을 지운다(다음 호출 또는 완료 경로에서
 * 재평가되도록). blkg_tryget()/blkg_put()으로 순회 중 해제되는 blkg에 대한
 * 접근을 안전하게 만든다.
 * 실행 컨텍스트: 커널 타이머(softirq) 컨텍스트 — sleep 불가. RCU read-side
 * critical section 안에서 실행되며, 각 cgroup의 child_lat.lock을
 * spin_lock_irqsave()로 잠근다(완료 경로와의 경쟁 방지).
 *
 * 호출 체인:
 *   (커널 타이머 인프라, 1초 만료) → [blkiolatency_timer_fn] →
 *     scale_cookie_change()
 */
static void blkiolatency_timer_fn(struct timer_list *t)
{
	/* [한국어] timer_list 임베디드 필드로부터 상위 blk_iolatency 구조체를 역참조 */
	struct blk_iolatency *blkiolat = timer_container_of(blkiolat, t,
							    timer); /* [한국어] container_of 계열 — 타이머 API가 넘겨주는 건 임베디드 필드 주소뿐이다 */
	struct blkcg_gq *blkg; /* [한국어] 트리 순회 커서 */
	struct cgroup_subsys_state *pos_css; /* [한국어] blkg_for_each_descendant_pre가 요구하는 cgroup 순회 상태 */
	/* [한국어] 이번 회복 검토 전체에 사용할 기준 시각. 노드마다 다시 읽으면
	 * 앞쪽 노드와 뒤쪽 노드가 서로 다른 기준으로 판정되어 일관성이 깨진다 */
	u64 now = blk_time_get_ns();

	/* [한국어] blkg_for_each_descendant_pre()가 cgroup 트리를 RCU로 순회하므로,
	 * queue freeze 없이도 blkcg_gq 트리 구조 자체의 존재를 안전하게 참조하기
	 * 위해 RCU read-side critical section 진입. 타이머 컨텍스트라 sleep이
	 * 불가능해 mutex 같은 무거운 동기화는 애초에 선택지가 아니다 */
	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkiolat->rqos.disk->queue->root_blkg) {
		/* [한국어] root_blkg를 루트로 모든 자손 blkcg_gq를 전위 순회 — 이 큐에
		 * 속한 모든 cgroup의 scale_cookie 상태를 회복 기회로 검토 */
		struct iolatency_grp *iolat; /* [한국어] 이 노드의 정책 데이터 */
		struct child_latency_info *lat_info; /* [한국어] 이 노드가 '부모로서' 자식들에게 들려주는 공유 상태 */
		unsigned long flags; /* [한국어] irqsave용 인터럽트 상태 */
		u64 cookie; /* [한국어] 이 노드가 자식들에게 지시 중인 현재 쿠키 값 */

		/*
		 * We could be exiting, don't access the pd unless we have a
		 * ref on the blkg.
		 */
		/* [한국어] blkg 참조 카운트 획득 실패는 이 blkg가 해제 진행 중이라는
		 * 뜻이므로, pd 등 내부 데이터에 접근하지 않고 건너뛴다. RCU만으로는
		 * 트리 구조의 존재만 보장될 뿐 pd의 수명은 보장되지 않는다 */
		if (!blkg_tryget(blkg))
			continue;

		iolat = blkg_to_lat(blkg); /* [한국어] 순회는 blkcg 트리 전체를 돌지만, 정책이 붙은 노드만 처리 대상이다 */
		if (!iolat) /* [한국어] 정책이 붙지 않은 노드 */
			goto next; /* [한국어] tryget으로 올린 참조가 있으므로 continue가 아니라 next로 가서 put해야 한다 */
		/* [한국어] 이 cgroup에 iolatency policy data가 없으면(비활성) 처리할
		 * 것이 없으므로 참조만 반환하고 다음으로 이동 */

		lat_info = &iolat->child_lat;
		/* [한국어] 이 cgroup이 "부모"로서 관리 중인 scale_cookie를 읽는다.
		 * 락 없이 먼저 읽어서, 손댈 필요가 없는 대다수 노드에 대해
		 * 스핀락 획득 자체를 피한다 */
		cookie = atomic_read(&lat_info->scale_cookie);

		if (cookie >= DEFAULT_SCALE_COOKIE) /* [한국어] 이 노드는 자식들을 조이고 있지 않다 =
						     * 이 타이머가 해 줄 회복 작업이 없다 */
			goto next; /* [한국어] 락도 잡기 전이므로 참조만 반환하면 된다 */
		/* [한국어] 이미 DEFAULT 이상이면(자식들에게 스로틀을 지시하고 있지
		 * 않으면) 회복할 것이 없으므로 건너뛴다 */

		spin_lock_irqsave(&lat_info->lock, flags); /* [한국어] irqsave: 이 락은 완료 경로(IRQ 컨텍스트일 수 있음)와
							    * 공유되므로 같은 CPU의 인터럽트로 재진입하면 데드락이다 */
		if (lat_info->last_scale_event >= now) /* [한국어] 시각 역전 방어 — 아래 뺄셈이 부호 없는 연산이라
							* 역전 상태로 계산하면 거대한 양수가 나온다 */
			goto next_lock;
		/* [한국어] last_scale_event가 now 이상이면 시각이 역전된 상황(타이머
		 * 지연/재스케줄로 이례적으로 이 콜백이 너무 이르게 실행된 경우)이므로
		 * 이번 회차는 건너뛴다 */

		/*
		 * We scaled down but don't have a scale_grp, scale up and carry
		 * on.
		 */
		if (lat_info->scale_grp == NULL) { /* [한국어] 조인 상태인데 책임자가 없다 = 아무도 회복을 시작해 줄 수 없다.
						    * 이 타이머가 없으면 그 상태로 영원히 갇힌다 */
			scale_cookie_change(iolat->blkiolat, lat_info, true);
			goto next_lock;
		}
		/* [한국어] scale down 상태이지만 책임자(scale_grp)를 특정할 수 없다면,
		 * 계속 스로틀 상태로 방치하기보다 안전하게 scale up을 시도해
		 * 점진적으로 회복시킨다 */

		/*
		 * It's been 5 seconds since our last scale event, clear the
		 * scale grp in case the group that needed the scale down isn't
		 * doing any IO currently.
		 */
		if (now - lat_info->last_scale_event >=
		    ((u64)NSEC_PER_SEC * 5))
			lat_info->scale_grp = NULL;
		/* [한국어] 마지막 조정으로부터 5초 이상 지났다면 scale_grp을 지운다.
		 * 책임 cgroup이 더 이상 IO를 내지 않아(완료 경로가 호출되지 않아)
		 * 스스로 scale up을 트리거하지 못하는 상황을 다음 평가에서
		 * "책임자 없음"으로 재검토하게 하기 위함 */
next_lock:
		spin_unlock_irqrestore(&lat_info->lock, flags);
next:
		/* [한국어] blkg_tryget()으로 획득한 참조를 반환. 모든 이탈 경로가
		 * 이 라벨로 모이므로 참조 누수가 생기지 않는다 */
		blkg_put(blkg);
	}
	rcu_read_unlock(); /* [한국어] 트리 순회 종료 — 이 시점 이후 blkg 포인터는 더 이상 유효를 보장받지 못한다 */
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
 * [한국어]
 * blkiolatency_enable_work_fn - enable_cnt 변화에 따라 blkiolat->enabled
 * 마스터 스위치를 queue freeze 상태에서 안전하게 토글.
 *
 * @work: iolatency_set_min_lat_nsec()이 schedule_work()로 예약한
 *        blkiolat->enable_work.
 * @return: 없음(void).
 *
 * 원문 커널 주석(위 영어 comment) 요약: iolatency는 매 IO마다 cgroup 계층을
 * 두 번(제출/완료) 순회하며 inflight를 추적해야 하므로 비용이 크다. 이 큐의
 * 어떤 cgroup도 target을 설정하지 않았다면 이 추적 자체를 꺼서 오버헤드를
 * 없애고 싶다. 다만 "켜진 상태에서 진행 중이던 inflight 카운트"가 꺼지는
 * 순간에 누수되지 않도록, enabled 플래그는 반드시 큐를 freeze(모든 새 IO
 * 진입을 막고 기존 IO가 모두 완료되기를 기다림)한 상태에서만 뒤집는다. 이
 * freeze는 잠들 수 있는(sleepable) 컨텍스트가 필요하므로, atomic 컨텍스트일
 * 수 있는 enable_cnt 변경 지점(iolatency_set_min_lat_nsec)에서 직접 하지
 * 않고 이 workqueue 함수로 미룬 것이다.
 * enabled != atomic_read(enable_cnt)인 경우에만 실제로 freeze/toggle을
 * 수행하며, enabled로 전환 시에는 QUEUE_FLAG_BIO_ISSUE_TIME을 켜서 이후
 * bio->issue_time_ns 기록이 시작되게 하고, disabled로 전환 시에는 꺼서
 * 불필요한 타임스탬프 기록 오버헤드를 없앤다.
 * 동시성: 이 blkiolat에 대해 이 함수의 인스턴스는 동시에 하나만 실행됨이
 * workqueue에 의해 보장되고, 가장 최근의 enable_cnt 변경 이후 최소 한 번은
 * 반드시 실행됨이 보장되므로 "최신 enable_cnt만 반영"해도 충분하다(원문
 * 주석 그대로). blkiolat 자체의 생존은 blkcg_iolatency_exit()이
 * flush_work()로 이 함수의 완료를 기다린 후에야 kfree()하므로 보장된다.
 * 실행 컨텍스트: workqueue 컨텍스트(sleep 가능, blk_mq_freeze_queue() 호출 가능).
 *
 * 호출 체인:
 *   iolatency_set_min_lat_nsec() → schedule_work() → (workqueue) →
 *     [blkiolatency_enable_work_fn] → blk_mq_freeze_queue()/unfreeze_queue()
 */
static void blkiolatency_enable_work_fn(struct work_struct *work)
{
	struct blk_iolatency *blkiolat = container_of(work, struct blk_iolatency,
						      enable_work); /* [한국어] work 구조체 주소 → 그것을 품은 blk_iolatency 복원 */
	/* [한국어] work 필드 오프셋으로 상위 blk_iolatency 역참조 */
	bool enabled; /* [한국어] "있어야 할" 상태. 현재 반영된 blkiolat->enabled와 비교해 차이가 있을 때만 움직인다 */

	/*
	 * There can only be one instance of this function running for @blkiolat
	 * and it's guaranteed to be executed at least once after the latest
	 * ->enabled_cnt modification. Acting on the latest ->enable_cnt is
	 * sufficient.
	 *
	 * Also, we know @blkiolat is safe to access as ->enable_work is flushed
	 * in blkcg_iolatency_exit().
	 */
	/* [한국어] enable_cnt(0보다 크면 true로 암묵 변환)를 읽어 "있어야 할" 상태를 결정.
	 * 이 워커는 카운터가 오르내린 '횟수'를 따라가지 않고 마지막 값만 본다 —
	 * 위 원문 주석이 말하듯 최신 값 한 번 반영이면 충분하기 때문이다 */
	enabled = atomic_read(&blkiolat->enable_cnt);
	if (enabled != blkiolat->enabled) { /* [한국어] 상태가 이미 맞으면 freeze를 건너뛴다 —
					     * freeze는 in-flight IO를 전부 기다리는, 이 파일에서 가장 비싼 연산이다 */
		/* [한국어] 목표 상태(enabled)와 현재 반영된 상태(blkiolat->enabled)가
		 * 다를 때만 무거운 freeze 작업을 수행 — 불필요한 freeze 반복 방지 */
		struct request_queue *q = blkiolat->rqos.disk->queue;
		/* [한국어] 플래그 조작 대상이 될 request_queue 포인터 캐싱 */
		unsigned int memflags;

		/* [한국어] 큐를 freeze해 새 IO 진입을 막고 기존 inflight IO가 모두
		 * 완료될 때까지 대기. 이걸 하지 않고 스위치를 뒤집으면, throttle에서
		 * inflight를 올린 bio가 done에서 꺼진 스위치를 만나 감소되지 않고
		 * 카운터가 영구히 새어 나간다. 그래서 freeze가 필수다 */
		memflags = blk_mq_freeze_queue(blkiolat->rqos.disk->queue);
		/* [한국어] memflags는 이후 unfreeze 시 이전 memalloc 상태를 복원하기 위한 값 */
		blkiolat->enabled = enabled;
		/* [한국어] freeze된 상태에서 실제 마스터 스위치를 목표값으로 전환.
		 * 이 순간 진행 중인 inflight IO가 없으므로 카운트 불일치가 생기지 않음 */
		if (enabled) /* [한국어] 켜질 때: 완료 경로에서 지연을 계산하려면 제출 시각이 필요하다 */
			blk_queue_flag_set(QUEUE_FLAG_BIO_ISSUE_TIME, q);
		else /* [한국어] 꺼질 때: 아무도 읽지 않을 시각을 매 bio마다 기록하는 비용을 없앤다 */
			blk_queue_flag_clear(QUEUE_FLAG_BIO_ISSUE_TIME, q);
		/* [한국어] QUEUE_FLAG_BIO_ISSUE_TIME: bio 제출 시각(issue_time_ns)을
		 * 기록할지 여부를 제어하는 큐 플래그. enabled=true일 때만 이 시각을
		 * 기록해야 완료 경로에서 지연을 계산할 수 있으므로 함께 토글 */
		blk_mq_unfreeze_queue(blkiolat->rqos.disk->queue, memflags);
		/* [한국어] freeze 해제 — 이후 새 IO의 제출/완료가 재개된다 */
	}
}

/*
 * [한국어]
 * blk_iolatency_init - 디스크(gendisk)에 io.latency QoS 인스턴스를 최초로 생성/등록.
 *
 * @disk: io.latency를 사용하려는 gendisk(블록 디바이스).
 * @return: 0(성공) 또는 음수 errno(-ENOMEM 등 실패).
 *
 * cgroupfs의 io.latency 파일에 처음 target을 쓸 때(iolatency_set_limit())
 * 이 큐에 아직 rq_qos 인스턴스가 없다면 호출된다. blk_iolatency 구조체를
 * 할당한 뒤, rq_qos_add()로 RQ_QOS_LATENCY 슬롯에 blkcg_iolatency_ops를
 * 등록해 submit/done_bio 콜백 경로를 연결하고, blkcg_activate_policy()로
 * 이 디스크의 기존 모든 cgroup에 대해 iolatency_pd_alloc/init을 트리거해
 * iolatency_grp을 만든다. 마지막으로 회복 타이머(blkiolatency_timer_fn)와
 * enabled 토글 workqueue(blkiolatency_enable_work_fn)를 초기화한다. 실패
 * 시 이미 수행된 단계를 역순으로 되돌리는 goto 기반 에러 처리를 사용한다.
 * 실행 컨텍스트: sysfs write 경로(iolatency_set_limit), 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iolatency_set_limit() → [blk_iolatency_init] → rq_qos_add() /
 *     blkcg_activate_policy() / timer_setup() / INIT_WORK()
 */
static int blk_iolatency_init(struct gendisk *disk)
{
	struct blk_iolatency *blkiolat; /* [한국어] 이 디스크 전체에 하나뿐인 정책 인스턴스 */
	int ret; /* [한국어] 아래 두 등록 단계 각각의 실패를 서로 다른 라벨로 되감기 위해 필요 */

	/* [한국어] 0 초기화 할당 — enabled/enable_cnt가 0이어야 "아직 아무 cgroup도
	 * 목표를 설정하지 않음"이라는 초기 상태가 성립한다 */
	blkiolat = kzalloc_obj(*blkiolat);
	if (!blkiolat) /* [한국어] 메모리 부족 */
		return -ENOMEM; /* [한국어] 아직 아무것도 등록하지 않았으므로 되돌릴 것이 없다 */

	/* [한국어] RQ_QOS_LATENCY 슬롯으로 체인에 끼워 넣는다. 슬롯 id가 정책마다
	 * 고정이라 wbt/iocost와 순서가 섞이지 않고, 같은 정책이 두 번 등록되는
	 * 일도 없다 */
	ret = rq_qos_add(&blkiolat->rqos, disk, RQ_QOS_LATENCY,
			 &blkcg_iolatency_ops); /* [한국어] 이 시점부터 콜백이 실제 IO 경로에서 호출될 수 있다 */
	if (ret) /* [한국어] 같은 슬롯에 이미 등록되어 있거나 디스크가 정리 중인 경우 */
		goto err_free;
	/* [한국어] rq_qos 프레임워크에 이 정책을 RQ_QOS_LATENCY 슬롯으로 등록.
	 * 이후 submit/complete 경로에서 blkcg_iolatency_ops의 콜백이 호출된다.
	 * 실패 시 방금 할당한 blkiolat만 해제하면 되므로 err_free로 이동 */
	ret = blkcg_activate_policy(disk, &blkcg_policy_iolatency);
	if (ret) /* [한국어] 여기서 실패하면 rq_qos는 이미 체인에 걸린 상태다 — 반드시 먼저 떼어내야 한다 */
		goto err_qos_del;
	/* [한국어] blkcg 코어에 이 policy를 활성화 — 이 디스크에 이미 존재하는
	 * 모든 cgroup에 대해 iolatency_pd_alloc()/iolatency_pd_init()이 호출되어
	 * iolatency_grp들이 생성된다. 실패 시 rq_qos 등록도 함께 되돌려야 하므로
	 * err_qos_del로 이동 */

	timer_setup(&blkiolat->timer, blkiolatency_timer_fn, 0);
	/* [한국어] 1초 주기 회복 타이머를 초기화(아직 무장은 하지 않음 — 실제
	 * 무장은 첫 스로틀 발생 시 blkcg_iolatency_throttle()의 mod_timer()) */
	INIT_WORK(&blkiolat->enable_work, blkiolatency_enable_work_fn);
	/* [한국어] enabled 토글용 workqueue 작업을 초기화 — queue freeze 기반
	 * on/off 전환의 실행 단위 준비 */

	return 0;

err_qos_del: /* [한국어] 되감기 순서는 등록 순서의 역순 — rq_qos를 먼저 떼고 메모리를 나중에 푼다 */
	rq_qos_del(&blkiolat->rqos);
	/* [한국어] blkcg_activate_policy() 실패 시 앞서 등록한 rq_qos를 제거해
	 * 콜백이 더 이상 호출되지 않게 정리 */
err_free:
	kfree(blkiolat);
	/* [한국어] 마지막으로 blkiolat 메모리 자체를 해제 */
	return ret;
}

/*
 * [한국어]
 * iolatency_set_min_lat_nsec - cgroup 하나의 목표 지연(min_lat_nsec)을
 * 설정 또는 해제하고, 그에 따라 큐 전체 enable_cnt를 조정.
 *
 * @blkg: target을 설정/해제할 (cgroup, 디스크) 조합.
 * @val: 새 목표 지연(nsec). 0이면 이 cgroup에서 io.latency 제어를 끈다.
 * @return: 없음(void).
 *
 * iolat->min_lat_nsec을 val로 바꾸고, 그에 비례해 통계 윈도우 크기
 * (cur_win_nsec)를 재계산한다 — val의 16배를 기본으로 하되
 * BLKIOLATENCY_MIN_WIN_SIZE(100ms)~BLKIOLATENCY_MAX_WIN_SIZE(1s) 범위로
 * 클램프한다. oldval(변경 전 값)과 val을 비교해 0→양수 전이(처음 활성화)면
 * 큐 전체 enable_cnt를 증가시키고, 그 결과가 1이 되었다면(이 큐에서 최초로
 * 활성화된 cgroup) blkiolatency_enable_work_fn()을 예약해 inflight 추적을
 * 켠다. 반대로 양수→0 전이(마지막 비활성화)면 지연 부채를 정리하고
 * enable_cnt를 감소시켜, 0이 되면(이 큐의 마지막 활성 cgroup이 사라짐)
 * 마찬가지로 work를 예약해 추적을 끈다.
 * 실행 컨텍스트: iolatency_set_limit()(sysfs write, 프로세스 컨텍스트)과
 * iolatency_pd_offline()(cgroup 오프라인 처리 경로)에서 호출.
 *
 * 호출 체인:
 *   iolatency_set_limit()/iolatency_pd_offline() → [iolatency_set_min_lat_nsec]
 *     → schedule_work(enable_work)
 */
static void iolatency_set_min_lat_nsec(struct blkcg_gq *blkg, u64 val)
{
	struct iolatency_grp *iolat = blkg_to_lat(blkg);
	/* [한국어] 대상 cgroup의 iolatency_grp 획득 */
	struct blk_iolatency *blkiolat = iolat->blkiolat;
	/* [한국어] 큐 전체 상태(enable_cnt/enable_work)에 접근하기 위한 포인터 */
	u64 oldval = iolat->min_lat_nsec;
	/* [한국어] 활성화/비활성화 "전이"를 판단하기 위해 변경 전 값을 저장 */

	iolat->min_lat_nsec = val;
	/* [한국어] 목표 지연(SLO) 설정. 0이면 이 cgroup의 io.latency 제어 비활성화 */
	iolat->cur_win_nsec = max_t(u64, val << 4, BLKIOLATENCY_MIN_WIN_SIZE);
	/* [한국어] 목표 지연의 16배 또는 최소 윈도우(100ms) 중 큰 값을 통계 윈도우로
	 * 선택. 목표가 매우 짧아 그 16배도 100ms에 못 미치면 최소값이
	 * 적용되어 충분한 샘플 수를 확보한다 */
	iolat->cur_win_nsec = min_t(u64, iolat->cur_win_nsec,
				    BLKIOLATENCY_MAX_WIN_SIZE);
	/* [한국어] 1초 상한으로 재클램프 — 목표 지연이 매우 커도 회복 반응성을
	 * 위해 윈도우가 1초를 넘지 않게 한다 */

	if (!oldval && val) {
		if (atomic_inc_return(&blkiolat->enable_cnt) == 1) /* [한국어] "0 → 1" 전이를 잡아내는 관용구.
								    * inc 후 read가 아니라 inc_return이어야 두 CPU가 동시에
								    * 켜도 정확히 한 쪽만 1을 보게 된다 */
			schedule_work(&blkiolat->enable_work); /* [한국어] freeze가 필요한 작업이라 여기서 직접 하지 않고
								* workqueue로 미룬다 — 여기는 sysfs write 경로이고
								* 락을 쥔 채 freeze를 기다릴 수 없다 */
		/* [한국어] enable_cnt를 증가시킨 결과가 정확히 1이라면(즉 이 증가가
		 * "0에서 1로"의 전이였다면) 이 큐에서 최초로 활성화된 cgroup이므로
		 * enable_work를 예약해 마스터 스위치를 켠다 */
	}
	/* [한국어] oldval==0 && val!=0: 이 cgroup이 처음으로 target을 설정하는
	 * 경우(비활성 → 활성 전이) */
	if (oldval && !val) {
		blkcg_clear_delay(blkg);
		/* [한국어] target을 해제하므로 이 cgroup에 남아있던 memory.delay
		 * 지연 부채를 모두 지운다 — 더 이상 제어 대상이 아니므로 */
		if (atomic_dec_return(&blkiolat->enable_cnt) == 0)
			schedule_work(&blkiolat->enable_work);
		/* [한국어] enable_cnt 감소 결과가 0이라면(마지막 활성 cgroup이었다면)
		 * enable_work를 예약해 마스터 스위치를 꺼서 불필요한 추적 오버헤드를 없앤다 */
	}
	/* [한국어] oldval!=0 && val==0: 이 cgroup이 target을 해제하는 경우
	 * (활성 → 비활성 전이) */
}

/*
 * [한국어]
 * iolatency_clear_scaling - 부모의 scale 관련 상태(scale_cookie/scale_grp/
 * scale_lat/last_scale_event)를 초기값으로 리셋.
 *
 * @blkg: target이 변경된 cgroup. 이 함수는 blkg 자신이 아니라 blkg->parent의
 *        child_lat을 리셋한다(부모가 자식들을 관리하는 구조이므로).
 * @return: 없음(void).
 *
 * 어떤 cgroup의 목표 지연(min_lat_nsec)이 바뀌면, 그 부모가 들고 있던
 * "이전 목표 기준"의 scale_cookie/scale_grp/scale_lat 판단은 더 이상
 * 유효하지 않을 수 있다(예: 이전에 scale down의 책임자였던 cgroup의 목표가
 * 사라지거나 크게 바뀜). 이 함수는 그런 낡은 판단을 지우고 DEFAULT_SCALE_COOKIE
 * (제한 없음)로 되돌려 새로운 목표 기준으로 재평가가 시작되게 한다. root
 * cgroup에는 부모가 없으므로(blkg->parent가 NULL) 아무 것도 하지 않는다.
 * 실행 컨텍스트: iolatency_set_limit()(target 변경 시), iolatency_pd_offline()
 * (cgroup 제거 시)에서 호출. child_lat.lock으로 iolatency_check_latencies()/
 * blkiolatency_timer_fn()과의 경쟁을 방지.
 *
 * 호출 체인:
 *   iolatency_set_limit()/iolatency_pd_offline() → [iolatency_clear_scaling]
 */
static void iolatency_clear_scaling(struct blkcg_gq *blkg)
{
	if (blkg->parent) {
		/* [한국어] 부모가 있을 때만(즉 blkg가 root가 아닐 때만) 부모의 child_lat을
		 * 리셋할 대상이 존재한다 */
		struct iolatency_grp *iolat = blkg_to_lat(blkg->parent);
		struct child_latency_info *lat_info; /* [한국어] 리셋 대상은 '부모가 자식들에게 지시하던' 상태다 */
		if (!iolat) /* [한국어] 부모에 정책 데이터가 없으면 지울 상태도 없다 */
			return;
		/* [한국어] 부모에 아직 iolatency policy data가 없으면(비활성) 리셋할
		 * 것이 없으므로 반환 */

		lat_info = &iolat->child_lat;
		/* [한국어] 목표가 바뀌면 그 목표를 근거로 내려졌던 판단(scale_grp,
		 * scale_lat, 쿠키)이 전부 무효가 된다. 그래서 부분 수정이 아니라
		 * 통째로 초기 상태로 되돌린다 */
		spin_lock(&lat_info->lock);
		/* [한국어] 이 함수는 프로세스 컨텍스트(sysfs write)에서만 호출되므로
		 * irqsave 없이 일반 spin_lock()으로 충분(완료 경로의 irqsave 잠금과는
		 * 상호 배제만 되면 됨) */
		atomic_set(&lat_info->scale_cookie, DEFAULT_SCALE_COOKIE);
		/* [한국어] scale_cookie를 DEFAULT(제한 없음)로 리셋 — 새 목표 기준의
		 * 재평가를 "제한 없음"에서부터 다시 시작 */
		lat_info->last_scale_event = 0;
		/* [한국어] 마지막 조정 시각을 0으로 리셋해 디바운스 타이머를 재시작 */
		lat_info->scale_grp = NULL;
		/* [한국어] 이전 scale down의 책임 cgroup 기록을 지운다 — 더 이상
		 * 유효하지 않은 판단이므로 */
		lat_info->scale_lat = 0;
		/* [한국어] 기준 지연값도 0으로 리셋해 다음 위반 시 새로 설정되게 함 */
		spin_unlock(&lat_info->lock);
	}
}

/*
 * [한국어]
 * iolatency_set_limit - cgroupfs io.latency 파일에 대한 write(2) 처리
 * (cftype.write 콜백).
 *
 * @of: kernfs open file 컨텍스트(어떤 cgroup의 어떤 파일이 열렸는지 포함).
 * @buf: 사용자가 쓴 문자열. 예: "8:0 target=4000" (디바이스 major:minor와
 *       target=usec 쌍).
 * @nbytes: buf의 바이트 길이.
 * @off: 사용되지 않음(오프셋 쓰기 미지원 인터페이스).
 * @return: 성공 시 nbytes(전체를 소비했음을 알림), 실패 시 음수 errno.
 *
 * 사용자가 "echo '8:0 target=4000' > io.latency" 형태로 특정 블록 디바이스에
 * 대한 이 cgroup의 목표 지연을 설정하는 진입점이다. blkg_conf_open_bdev()로
 * 문자열 앞부분의 devmaj:devmin을 파싱해 대상 bdev를 연다. 이 큐에 아직
 * io.latency rq_qos 인스턴스가 없다면(iolat_rq_qos()가 NULL) rq_qos_mutex를
 * 쥔 채로 blk_iolatency_init()을 호출해 최초 초기화를 원자적으로 수행한다
 * (원문 주석: rq_qos_add() 성공 후 blk_iolatency_init()이 실패하는 경합을
 * 피하기 위함). blkg_conf_prep()으로 대상 blkcg_gq를 확정한 뒴, 남은
 * 문자열(ctx.body)을 공백으로 토큰화하며 "key=value" 쌍을 파싱한다.
 * 현재는 "target" 키만 지원하며, 값이 "max"면 0(제한 해제), 숫자면 usec
 * 단위를 nsec로 변환해 저장한다. 그 외의 키나 파싱 실패는 -EINVAL로 처리.
 * 파싱이 끝나면 iolatency_set_min_lat_nsec()으로 실제 값을 반영하고,
 * 값이 실제로 바뀌었다면 iolatency_clear_scaling()으로 낡은 scale 상태를
 * 리셋한다.
 * 실행 컨텍스트: cgroupfs write 시스템 호출 경로(프로세스 컨텍스트, sleep 가능).
 *
 * 호출 체인:
 *   sys_write() → kernfs_fop_write_iter() → iolatency_files[].write
 *     → [iolatency_set_limit] → blk_iolatency_init() /
 *       iolatency_set_min_lat_nsec() / iolatency_clear_scaling()
 */
static ssize_t iolatency_set_limit(struct kernfs_open_file *of, char *buf,
			     size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	/* [한국어] 이 파일을 연 cgroup의 css로부터 blkcg 획득 */
	struct blkcg_gq *blkg;
	struct blkg_conf_ctx ctx; /* [한국어] "devmaj:devmin key=val" 형식을 단계적으로 소비하는 파서 상태 */
	struct iolatency_grp *iolat;
	char *p, *tok; /* [한국어] strsep용 커서와 현재 토큰 */
	u64 lat_val = 0; /* [한국어] 기본값 0 = "max"(제한 없음)와 같은 의미 */
	u64 oldval; /* [한국어] 변경 전 목표 — 실제로 값이 바뀌었을 때만 scale 상태를 리셋하기 위해 필요 */
	int ret;

	blkg_conf_init(&ctx, buf);
	/* [한국어] 파싱 컨텍스트 초기화 — buf를 이후 blkg_conf_open_bdev/prep이
	 * 단계적으로 소비하게 될 상태를 준비 */

	ret = blkg_conf_open_bdev(&ctx);
	if (ret)
		goto out;
	/* [한국어] buf 앞부분의 "devmaj:devmin" 토큰을 파싱해 대상 block_device를
	 * 열고 ctx.bdev에 저장. 실패(디바이스 없음 등) 시 즉시 out으로 */

	/*
	 * blk_iolatency_init() may fail after rq_qos_add() succeeds which can
	 * confuse iolat_rq_qos() test. Make the test and init atomic.
	 */
	lockdep_assert_held(&ctx.bdev->bd_queue->rq_qos_mutex);
	/* [한국어] blkg_conf_open_bdev()가 이미 rq_qos_mutex를 쥔 채로 반환했음을
	 * lockdep으로 검증(디버그 빌드에서만 실질적 효과) */
	if (!iolat_rq_qos(ctx.bdev->bd_queue))
		ret = blk_iolatency_init(ctx.bdev->bd_disk);
	/* [한국어] 이 큐에 아직 io.latency rq_qos 인스턴스가 없으면(최초 사용)
	 * rq_qos_mutex 보호 하에 원자적으로 초기화 — "확인"과 "초기화" 사이에
	 * 다른 스레드가 끼어들어 중복 초기화되는 경쟁을 방지 */
	if (ret)
		goto out;
	/* [한국어] blk_iolatency_init() 실패(-ENOMEM 등) 시 out으로 이동 */

	ret = blkg_conf_prep(blkcg, &blkcg_policy_iolatency, &ctx);
	if (ret)
		goto out;
	/* [한국어] 나머지 파싱을 계속 진행해 최종 blkg를 확정하고 ctx.blkg/ctx.body에
	 * 저장(ctx.body는 "target=..." 이후 남은 문자열) */

	iolat = blkg_to_lat(ctx.blkg);
	/* [한국어] 확정된 blkg에서 이번에 설정할 iolatency_grp 획득 */
	p = ctx.body;
	/* [한국어] 이제부터 strsep()으로 토큰화할 남은 문자열 포인터 */

	ret = -EINVAL;
	/* [한국어] 아래 파싱 루프에서 성공적으로 끝까지 처리되지 못하면 -EINVAL로
	 * 남도록 기본값 설정 */
	while ((tok = strsep(&p, " "))) {
		/* [한국어] 공백으로 구분된 다음 토큰(예: "target=4000")을 하나씩 추출 */
		char key[16];
		char val[21];	/* 18446744073709551616 */

		/* [한국어] 폭 지정자(%15, %20)가 필수다 — 없으면 사용자 문자열로
		 * 스택 버퍼를 넘길 수 있다. key/val 크기가 각각 16/21인 것과 짝이 맞는다 */
		if (sscanf(tok, "%15[^=]=%20s", key, val) != 2)
			goto out; /* [한국어] ret는 위에서 이미 -EINVAL로 세팅되어 있다 */
		/* [한국어] "key=value" 형식으로 파싱 실패(형식이 다르거나 값이 없음)
		 * 하면 -EINVAL로 즉시 종료 */

		if (!strcmp(key, "target")) {
			u64 v;

			if (!strcmp(val, "max")) /* [한국어] 사용자 인터페이스상의 "제한 없음" 표기 */
				lat_val = 0; /* [한국어] 내부적으로는 목표 0이 곧 비활성이다 */
			/* [한국어] "max"는 "제한 없음"을 의미 — target을 0(비활성)으로 설정 */
			else if (sscanf(val, "%llu", &v) == 1)
				lat_val = v * NSEC_PER_USEC;
			/* [한국어] 사용자가 입력한 값(usec 단위)을 nsec로 환산해 저장 */
			else
				goto out;
			/* [한국어] "max"도 아니고 숫자로도 파싱 안 되면 잘못된 입력 */
		} else {
			goto out;
			/* [한국어] "target" 외의 키는 현재 지원하지 않으므로 -EINVAL */
		}
	}

	/* Walk up the tree to see if our new val is lower than it should be. */
	blkg = ctx.blkg;
	oldval = iolat->min_lat_nsec; /* [한국어] 같은 값을 다시 써도 scale 상태를 날리지 않도록, 반영 전에 보관 */
	/* [한국어] 변경 전/후를 비교하기 위해 현재 값을 저장 */

	iolatency_set_min_lat_nsec(blkg, lat_val);
	/* [한국어] 파싱된 새 목표를 실제로 반영(min_lat_nsec/cur_win_nsec/enable_cnt 갱신) */
	if (oldval != iolat->min_lat_nsec)
		iolatency_clear_scaling(blkg);
	/* [한국어] 값이 실제로 바뀌었다면(전이가 있었다면) 부모의 낡은 scale
	 * 상태를 리셋해 새 목표 기준으로 재평가가 시작되게 한다 */
	ret = 0;
	/* [한국어] 여기까지 도달했으면 파싱과 반영이 모두 성공한 것 */
out:
	blkg_conf_exit(&ctx);
	/* [한국어] blkg_conf_init()/open_bdev()/prep()이 잡았던 참조/락을 정리
	 * (성공/실패 모든 경로에서 공통으로 실행) */
	return ret ?: nbytes;
	/* [한국어] ret이 0(성공)이면 write(2) 관례상 소비한 바이트 수(nbytes)를,
	 * 실패면 음수 errno를 반환 */
}

/*
 * [한국어]
 * iolatency_prfill_limit - io.latency 파일의 각 디바이스 라인을 출력하는
 * blkcg_print_blkgs() 콜백.
 *
 * @sf: 결과를 쓸 seq_file(cgroupfs read(2) 백엔드).
 * @pd: 현재 출력 대상 (cgroup, 디바이스) 조합의 policy data.
 * @off: blkcg_print_blkgs() 호출 시 넘겨진 seq_cft(sf)->private 값(이
 *       콜백에서는 사용하지 않음).
 * @return: 항상 0(blkcg_print_blkgs()의 콜백 관례).
 *
 * 이 pd에 target이 설정되어 있지 않으면(min_lat_nsec == 0) 이 디바이스에
 * 대해서는 아무 줄도 출력하지 않는다(return 0으로 조용히 스킵). 설정되어
 * 있으면 "<devname> target=<usec>" 형식으로 한 줄 출력한다.
 *
 * 호출 체인:
 *   iolatency_print_limit() → blkcg_print_blkgs() → [iolatency_prfill_limit]
 */
static u64 iolatency_prfill_limit(struct seq_file *sf,
				  struct blkg_policy_data *pd, int off)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	/* [한국어] 이 policy data가 속한 iolatency_grp 획득 */
	const char *dname = blkg_dev_name(pd->blkg);
	/* [한국어] "8:0"과 같은 디바이스 이름 문자열 획득(NULL 가능) */

	if (!dname || !iolat->min_lat_nsec)
		return 0;
	/* [한국어] 디바이스 이름을 얻을 수 없거나 이 cgroup에 target이 설정되어
	 * 있지 않으면 이 줄은 출력하지 않는다 */
	seq_printf(sf, "%s target=%llu\n",
		   dname, div_u64(iolat->min_lat_nsec, NSEC_PER_USEC));
	/* [한국어] nsec 단위로 저장된 min_lat_nsec을 usec로 환산해 사용자에게
	 * 원래 입력 단위 그대로 보여준다 */
	return 0;
}

/*
 * [한국어]
 * iolatency_print_limit - io.latency 파일의 read(2)/cat 처리 (cftype.seq_show 콜백).
 *
 * @sf: 결과를 쓸 seq_file.
 * @v: seq_file 반복자 프로토콜에서 쓰이는 값(여기서는 사용하지 않음).
 * @return: 항상 0.
 *
 * blkcg_print_blkgs()에 iolatency_prfill_limit()을 콜백으로 넘겨, 이
 * cgroup 아래의 모든 (자손을 포함한) blkg를 순회하며 target이 설정된
 * 디바이스만 한 줄씩 출력하게 위임한다.
 *
 * 호출 체인:
 *   sys_read()/cat → kernfs_fop_read_iter() → iolatency_files[].seq_show
 *     → [iolatency_print_limit] → blkcg_print_blkgs() → iolatency_prfill_limit()
 */
static int iolatency_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  iolatency_prfill_limit,
			  &blkcg_policy_iolatency, seq_cft(sf)->private, false);
	/* [한국어] 이 cgroup(seq_css(sf))부터 시작해 모든 자손 blkg를 순회하며
	 * iolatency_prfill_limit()을 호출 — false는 "leaf만이 아니라 전체 출력"
	 * 등의 옵션(blkcg_print_blkgs 정의 참고) */
	return 0;
}

/*
 * [한국어]
 * iolatency_ssd_stat - SSD(percentile) 모드 cgroup의 디버그 통계를 seq_file에 출력.
 *
 * @iolat: 출력 대상 iolatency_grp(iolat->ssd == true인 경우에만 호출됨).
 * @s: 결과를 쓸 seq_file(blkcg debug stats 인터페이스, blkcg_debug_stats 활성 시).
 * @return: 없음(void).
 *
 * 모든 온라인 CPU의 per-CPU 통계를 임시로 합산해(원본 슬롯은 리셋하지
 * 않음 — iolatency_check_latencies()와 달리 순수 조회용) missed/total과
 * 현재 max_depth를 사람이 읽을 수 있는 형식으로 출력한다.
 * max_depth가 UINT_MAX(무제한)이면 "depth=max"로, 아니면 실제 숫자로 표시한다.
 * 실행 컨텍스트: iolatency_pd_stat() 호출 경로와 동일(디버그 인터페이스 read 경로).
 *
 * 호출 체인:
 *   iolatency_pd_stat() → [iolatency_ssd_stat] → latency_stat_sum()
 */
static void iolatency_ssd_stat(struct iolatency_grp *iolat, struct seq_file *s)
{
	struct latency_stat stat;
	int cpu;

	latency_stat_init(iolat, &stat);
	/* [한국어] 집계용 임시 stat을 0으로 초기화 */
	preempt_disable();
	/* [한국어] per-CPU 슬롯 순회 동안 CPU 마이그레이션 방지 */
	for_each_online_cpu(cpu) {
		struct latency_stat *s;
		/* [한국어] 바깥 함수 인자 s(seq_file)를 가리는 지역 변수 — 이 CPU의
		 * 통계 슬롯 포인터로만 사용됨(스코프가 분리되어 있어 안전) */
		s = per_cpu_ptr(iolat->stats, cpu);
		/* [한국어] cpu번 CPU의 통계 슬롯 포인터 획득 */
		latency_stat_sum(iolat, &stat, s);
		/* [한국어] 조회 전용이므로 iolatency_check_latencies()와 달리 이 슬롯을
		 * 리셋하지 않는다 — 실제 스로틀 판단에 영향을 주지 않는 순수 조회 */
	}
	preempt_enable();
	/* [한국어] 순회 종료, preemption 재활성화 */

	if (iolat->max_depth == UINT_MAX)
		seq_printf(s, " missed=%llu total=%llu depth=max",
			(unsigned long long)stat.ps.missed,
			(unsigned long long)stat.ps.total);
	/* [한국어] 제한 없음 상태면 depth를 "max" 문자열로 표시 */
	else
		seq_printf(s, " missed=%llu total=%llu depth=%u",
			(unsigned long long)stat.ps.missed,
			(unsigned long long)stat.ps.total,
			iolat->max_depth);
	/* [한국어] 스로틀 중이면 실제 max_depth 숫자를 함께 표시 */
}

/*
 * [한국어]
 * iolatency_pd_stat - blkcg 디버그 통계(io.stat 등) 출력 시 이 policy가
 * 기여하는 부분을 채우는 pd_stat_fn 콜백.
 *
 * @pd: 출력 대상 (cgroup, 디바이스)의 policy data.
 * @s: 결과를 쓸 seq_file.
 * @return: 없음(void).
 *
 * blkcg_debug_stats(커널 부트 옵션/모듈 파라미터로 제어되는 전역 스위치)가
 * 꺼져 있으면 아무 것도 출력하지 않는다. ssd 모드면 iolatency_ssd_stat()에
 * 위임하고, HDD 모드면 장기 평균(lat_avg)을 usec로, 통계 윈도우(cur_win_nsec)를
 * msec로 환산해 depth와 함께 출력한다.
 * 실행 컨텍스트: cgroupfs debug stats 파일 read(2) 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   blkcg_print_stat() 등 blkcg 코어 → blkcg_policy_iolatency.pd_stat_fn
 *     → [iolatency_pd_stat] → iolatency_ssd_stat()
 */
static void iolatency_pd_stat(struct blkg_policy_data *pd, struct seq_file *s)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	/* [한국어] 대상 iolatency_grp 획득 */
	unsigned long long avg_lat;
	unsigned long long cur_win;

	if (!blkcg_debug_stats)
		return;
	/* [한국어] 디버그 통계 전역 스위치가 꺼져 있으면 아무 것도 출력하지 않음
	 * (일반 운영 환경의 오버헤드를 줄이기 위한 게이트) */

	if (iolat->ssd)
		return iolatency_ssd_stat(iolat, s);
		/* [한국어] 비회전식 모드는 missed/total/depth 형식의 percentile 통계로
		 * 위임 출력 */

	avg_lat = div64_u64(iolat->lat_avg, NSEC_PER_USEC);
	/* [한국어] HDD 모드: 장기 평균 지연을 nsec에서 usec로 환산 */
	cur_win = div64_u64(iolat->cur_win_nsec, NSEC_PER_MSEC);
	/* [한국어] 통계 윈도우 크기를 nsec에서 msec로 환산 */
	if (iolat->max_depth == UINT_MAX)
		seq_printf(s, " depth=max avg_lat=%llu win=%llu",
			avg_lat, cur_win);
	/* [한국어] 제한 없음이면 depth를 "max"로 표시 */
	else
		seq_printf(s, " depth=%u avg_lat=%llu win=%llu",
			iolat->max_depth, avg_lat, cur_win);
	/* [한국어] 스로틀 중이면 실제 max_depth 숫자를 표시 */
}

/*
 * [한국어]
 * iolatency_pd_alloc - 새로 생성되는 (cgroup, 디바이스) 조합에 대해
 * iolatency_grp 메모리를 할당하는 pd_alloc_fn 콜백.
 *
 * @disk: 이 policy data가 속할 디스크(NUMA 노드 힌트로 사용).
 * @blkcg: 이 policy data가 속할 cgroup(이 함수 자체에서는 사용하지 않음 —
 *         blkcg 코어 콜백 시그니처 통일을 위해 전달됨).
 * @gfp: 할당 플래그(예: GFP_KERNEL, 호출 컨텍스트에 따라 GFP_NOWAIT 등).
 * @return: 성공 시 새 iolatency_grp의 &iolat->pd, 실패 시 NULL.
 *
 * blkcg_activate_policy()가 새 (cgroup, 디바이스) 조합마다, 혹은 새
 * cgroup이 생성될 때마다 이 콜백을 호출해 policy별 확장 데이터를 마련한다.
 * kzalloc_node()로 디스크의 NUMA 노드에 맞춰 할당해 지역성을 높이고,
 * per-CPU 통계 배열(iolat->stats)을 __alloc_percpu_gfp()로 추가 할당한다.
 * 이 단계에서는 필드를 세부 초기화하지 않으며, 실제 초기값 설정은
 * iolatency_pd_init()이 담당한다(alloc과 init이 분리된 것은 blkcg 코어의
 * 공통 policy 인터페이스 관례).
 * 실행 컨텍스트: cgroup/디바이스 연결 경로(프로세스 컨텍스트, gfp에 따라
 * sleep 가능 여부 결정).
 *
 * 호출 체인:
 *   blkcg_activate_policy()/cgroup 생성 경로 → [iolatency_pd_alloc]
 *     → kzalloc_node() / __alloc_percpu_gfp()
 */
static struct blkg_policy_data *iolatency_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct iolatency_grp *iolat;

	/* [한국어] 디스크의 NUMA 노드에 맞춰 iolatency_grp 전체를 0으로 할당.
	 * node_id를 지정하는 이유는 이 구조체를 만지는 쪽이 대체로 그 디스크의
	 * 인터럽트/완료 처리가 도는 노드이기 때문이다 */
	iolat = kzalloc_node(sizeof(*iolat), gfp, disk->node_id);
	if (!iolat)
		return NULL; /* [한국어] blkcg 코어가 이 NULL을 -ENOMEM으로 해석한다 */
	/* [한국어] 통계는 완료 경로에서 매 bio마다 갱신되므로 공유 카운터로 두면
	 * 캐시 라인 경합이 그대로 IO 지연이 된다. 그래서 per-CPU로 흩어 두고
	 * 윈도우 만료 시점에만 한 번 모은다 */
	iolat->stats = __alloc_percpu_gfp(sizeof(struct latency_stat),
				       __alignof__(struct latency_stat), gfp);
	if (!iolat->stats) { /* [한국어] per-CPU 할당 실패 */
		kfree(iolat); /* [한국어] 앞서 잡은 본체를 먼저 되돌린다 — 여기서 빠뜨리면 누수다 */
		return NULL;
	}
	return &iolat->pd;
	/* [한국어] blkcg 코어는 pd 포인터만 알면 되므로 iolat 내부의 pd 필드
	 * 주소를 반환 — 이후 pd_to_lat()으로 역참조 가능 */
}

/*
 * [한국어]
 * iolatency_pd_init - iolatency_pd_alloc()이 할당한 iolatency_grp의 필드를
 * 실제 초기값으로 채우는 pd_init_fn 콜백.
 *
 * @pd: iolatency_pd_alloc()이 반환했던 policy data(즉 &iolat->pd).
 * @return: 없음(void).
 *
 * blk_queue_rot()으로 이 큐가 회전식 미디어(HDD)인지 확인해 ssd 플래그를
 * 결정한다(non-rotational이면 ssd=true). 모든 possible CPU의
 * per-CPU 통계 슬롯과 cur_stat을 0으로 초기화하고, rq_wait/child_lat.lock을
 * 초기화한다. max_depth는 UINT_MAX(무제한)로 시작해, 실제 제한은 이후 SLO
 * 위반이 감지될 때 scale down을 통해 걸리게 된다. window_start를 현재
 * 시각으로, cur_win_nsec을 기본 100ms로 설정한다(target이 아직 설정 전이므로
 * iolatency_set_min_lat_nsec() 호출 전 임시값). 부모의 policy data가 이미
 * 초기화되어 있다면(리스트 순서상 부모가 먼저 init된 경우) 부모의 현재
 * scale_cookie를 상속해 처음부터 부모와 일관된 상태로 시작하고, 그렇지
 * 않으면(아직 부모가 init되지 않았거나 자신이 root 바로 아래) DEFAULT로
 * 초기화한다. 마지막으로 이 iolatency_grp이 "부모"로서 가질 child_lat의
 * scale_cookie도 DEFAULT로 초기화한다.
 * 실행 컨텍스트: iolatency_pd_alloc()과 동일한 cgroup/디바이스 연결 경로.
 *
 * 호출 체인:
 *   blkcg_activate_policy()/cgroup 생성 경로 → [iolatency_pd_init] →
 *     latency_stat_init() / rq_wait_init()
 */
static void iolatency_pd_init(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	struct blkcg_gq *blkg = lat_to_blkg(iolat);
	struct rq_qos *rqos = iolat_rq_qos(blkg->q);
	/* [한국어] 이 큐에 이미 등록되어 있어야 하는 io.latency rq_qos 인스턴스
	 * 획득(blk_iolatency_init()이 policy 활성화보다 먼저 rq_qos_add()를
	 * 호출했으므로 이 시점엔 존재가 보장됨) */
	struct blk_iolatency *blkiolat = BLKIOLATENCY(rqos);
	/* [한국어] rq_qos로부터 상위 blk_iolatency 역참조 */
	u64 now = blk_time_get_ns();
	/* [한국어] 첫 통계 윈도우의 시작 시각으로 사용할 현재 시각 */
	int cpu;

	/* [한국어] 이 값은 여기서 딱 한 번 정해지고 이후 바뀌지 않는다. latency_stat이
	 * union이라 도중에 뒤집히면 이미 쌓인 통계를 다른 타입으로 해석하게 된다 */
	iolat->ssd = !blk_queue_rot(blkg->q);
	/* [한국어] blk_queue_rot()이 false(회전하지 않는 미디어)이면
	 * ssd=true로 설정해 percentile(missed/total) 방식을 쓰게 한다. HDD면
	 * ssd=false로 평균(mean) 방식을 사용 */

	for_each_possible_cpu(cpu) { /* [한국어] online이 아니라 possible을 도는 것이 핵심 */
		struct latency_stat *stat;
		stat = per_cpu_ptr(iolat->stats, cpu); /* [한국어] 아직 켜지지 않은 CPU의 슬롯도 주소는 유효하다 */
		latency_stat_init(iolat, stat); /* [한국어] ssd 플래그를 이미 정한 뒤라 올바른 union 멤버가 초기화된다 */
	}
	/* [한국어] 아직 온라인이 아닌 CPU(hotplug로 나중에 켜질 수 있는 CPU)까지
	 * 포함해 모든 possible CPU의 슬롯을 미리 0으로 초기화 — 나중에 온라인화될
	 * 때 초기화되지 않은 값을 읽는 것을 방지 */

	latency_stat_init(iolat, &iolat->cur_stat);
	/* [한국어] 누적 버퍼도 0으로 초기화 */
	rq_wait_init(&iolat->rq_wait);
	/* [한국어] inflight 카운터와 대기열을 초기 상태로 설정 */
	spin_lock_init(&iolat->child_lat.lock);
	/* [한국어] 이 노드가 "부모"로서 가질 child_lat 보호 락 초기화 */
	iolat->max_depth = UINT_MAX;
	/* [한국어] 초기에는 깊이 제한 없음(UINT_MAX) — 실제 제한은 SLO 위반이 관측된 뒤에만 걸린다 */
	iolat->blkiolat = blkiolat;
	/* [한국어] 큐 전체 상태에 대한 역참조 저장 */
	iolat->cur_win_nsec = 100 * NSEC_PER_MSEC;
	/* [한국어] target이 아직 설정되지 않은 초기 상태의 임시 윈도우 크기(기본 100ms).
	 * 이후 iolatency_set_min_lat_nsec()이 target에 맞춰 재계산한다 */
	atomic64_set(&iolat->window_start, now);
	/* [한국어] 첫 통계 윈도우 시작 시각 설정 — 이후 완료 경로가 이 값과 now를
	 * 비교해 윈도우 만료를 판정 */

	/*
	 * We init things in list order, so the pd for the parent may not be
	 * init'ed yet for whatever reason.
	 */
	if (blkg->parent && blkg_to_pd(blkg->parent, &blkcg_policy_iolatency)) {
		/* [한국어] 부모가 존재하고, 부모의 iolatency policy data가 이미
		 * 초기화되어 있다면(리스트 순회 순서상 부모가 먼저 init된 경우) */
		struct iolatency_grp *parent = blkg_to_lat(blkg->parent);
		/* [한국어] DEFAULT로 시작하면 안 된다 — 부모가 이미 조이고 있는 중에
		 * 새 자식만 "제한 없음"에서 출발하면, 첫 비교에서 쿠키가 내려간 것으로
		 * 보여 혼자 두 배로 조여지거나 반대로 형제들과 어긋난다 */
		atomic_set(&iolat->scale_cookie,
			   atomic_read(&parent->child_lat.scale_cookie));
		/* [한국어] 부모가 현재 자식들에게 지시 중인 scale_cookie를 그대로
		 * 상속해, 새로 생성된 자식이 처음부터 부모와 어긋나지 않게 한다 */
	} else {
		atomic_set(&iolat->scale_cookie, DEFAULT_SCALE_COOKIE);
		/* [한국어] 부모가 없거나(root 바로 아래) 아직 init되지 않았다면
		 * 안전하게 DEFAULT(제한 없음)로 시작 */
	}

	atomic_set(&iolat->child_lat.scale_cookie, DEFAULT_SCALE_COOKIE);
	/* [한국어] 이 노드가 "부모" 입장에서 자신의 자식들에게 보여줄 scale_cookie도
	 * DEFAULT로 초기화 — 아직 이 노드 아래에 스로틀을 유발한 자식이 없는 상태 */
}

/*
 * [한국어]
 * iolatency_pd_offline - cgroup이 오프라인(제거 진행 중)될 때 이 policy의
 * 설정/scale 상태를 정리하는 pd_offline_fn 콜백.
 *
 * @pd: 오프라인 처리 중인 policy data.
 * @return: 없음(void).
 *
 * cgroup이 rmdir()되면 blkcg 코어가 각 policy의 pd_offline_fn을 호출한다.
 * 이 cgroup의 target을 0으로 설정해(iolatency_set_min_lat_nsec) 활성 상태를
 * 해제하고 enable_cnt를 적절히 감소시키며, 부모의 scale 상태도 함께
 * 리셋한다(iolatency_clear_scaling) — 사라지는 이 cgroup이 scale_grp으로
 * 남아 부모의 판단을 오염시키지 않도록 하기 위함이다.
 * 실행 컨텍스트: cgroup 제거 경로(rmdir, 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cgroup_rmdir() → blkcg 코어 오프라인 처리 → [iolatency_pd_offline] →
 *     iolatency_set_min_lat_nsec() / iolatency_clear_scaling()
 */
static void iolatency_pd_offline(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	struct blkcg_gq *blkg = lat_to_blkg(iolat);

	iolatency_set_min_lat_nsec(blkg, 0);
	/* [한국어] target을 0으로 설정해 비활성화 경로를 타게 하고, enable_cnt를
	 * 감소시켜 필요하면 마스터 스위치도 함께 끄게 한다 */
	iolatency_clear_scaling(blkg);
	/* [한국어] 부모가 이 cgroup을 scale_grp으로 기억하고 있었을 수 있으므로
	 * 부모의 scale 상태를 리셋해 낡은 참조가 남지 않게 한다 */
}

/*
 * [한국어]
 * iolatency_pd_free - iolatency_grp과 그 per-CPU 통계 배열 메모리를
 * 해제하는 pd_free_fn 콜백.
 *
 * @pd: 해제할 policy data.
 * @return: 없음(void).
 *
 * iolatency_pd_offline() 이후, blkcg 코어가 이 (cgroup, 디바이스) 조합을
 * 완전히 제거할 때 호출된다. iolatency_pd_alloc()에서 할당했던 두 메모리
 * (per-CPU stats 배열, iolat 본체)를 역순으로 해제한다.
 * 실행 컨텍스트: cgroup/디바이스 해제 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   blkcg_deactivate_policy()/cgroup 소멸 경로 → [iolatency_pd_free] →
 *     free_percpu() / kfree()
 */
static void iolatency_pd_free(struct blkg_policy_data *pd)
{
	struct iolatency_grp *iolat = pd_to_lat(pd);
	/* [한국어] 해제 순서는 할당의 역순. 여기 도달했다는 것은 offline을 이미
	 * 거쳐 이 pd를 참조하는 IO 경로가 없다는 뜻이다 */
	free_percpu(iolat->stats);
	kfree(iolat);
	/* [한국어] iolatency_grp 본체 해제 */
}

static struct cftype iolatency_files[] = {
	{
		.name = "latency",
		/* [한국어] cgroupfs에 노출되는 파일명("io.latency"로 최종 조합됨).
		 * 사용자가 target=usec 형식으로 이 SLO를 설정/조회하는 인터페이스 */
		.flags = CFTYPE_NOT_ON_ROOT,
		/* [한국어] root cgroup에는 이 파일을 만들지 않음 — root는 스로틀
		 * 대상이 될 수 없으므로(항상 최상위 발행 권한) target 설정이 의미 없다 */
		.seq_show = iolatency_print_limit,
		/* [한국어] read(2)/cat 시 호출되는 콜백 — 현재 설정된 target들을 출력 */
		.write = iolatency_set_limit,
		/* [한국어] write(2) 시 호출되는 콜백 — 새 target을 파싱해 반영 */
	},
	{}
	/* [한국어] cftype 배열의 끝을 나타내는 sentinel(이름이 없는 빈 엔트리) */
};

static struct blkcg_policy blkcg_policy_iolatency = {
	.dfl_cftypes	= iolatency_files,
	/* [한국어] cgroup v2(default hierarchy)에서 사용할 파일 테이블.
	 * io.latency sysfs 인터페이스를 이 배열로 등록 */
	.pd_alloc_fn	= iolatency_pd_alloc,
	/* [한국어] 새 (cgroup, 디바이스) 조합에 대한 policy data 메모리 할당 콜백 */
	.pd_init_fn	= iolatency_pd_init,
	/* [한국어] 할당된 policy data의 필드를 초기값으로 채우는 콜백 */
	.pd_offline_fn	= iolatency_pd_offline,
	/* [한국어] cgroup 제거 시작 시 설정/scale 상태를 정리하는 콜백 */
	.pd_free_fn	= iolatency_pd_free,
	/* [한국어] policy data 메모리를 최종 해제하는 콜백 */
	.pd_stat_fn	= iolatency_pd_stat,
	/* [한국어] blkcg 디버그 통계 출력에 이 policy의 상태를 기여하는 콜백 */
};

/*
 * [한국어]
 * iolatency_init - 커널/모듈 초기화 시 io.latency blkcg policy를 전역 등록.
 *
 * @return: blkcg_policy_register()의 반환값(0 성공, 음수 errno 실패).
 *
 * 이 함수가 성공적으로 실행되어야 비로소 cgroupfs에 io.latency 파일이
 * 나타나고, iolatency_pd_alloc/init 등의 콜백이 blkcg 코어에 의해 호출될
 * 수 있게 된다.
 * 실행 컨텍스트: module_init 매크로에 의해 모듈 로드 시(또는 built-in이면
 * 커널 부팅 초기화 단계에서) 1회 호출.
 *
 * 호출 체인:
 *   module_init() 인프라 → [iolatency_init] → blkcg_policy_register()
 */
static int __init iolatency_init(void)
{
	return blkcg_policy_register(&blkcg_policy_iolatency);
	/* [한국어] blkcg 코어에 이 policy(cftype 테이블과 pd_* 콜백들)를 등록.
	 * 이후 모든 블록 디바이스에서 io.latency 사용이 가능해진다 */
}

/*
 * [한국어]
 * iolatency_exit - 모듈 언로드 시 io.latency blkcg policy를 전역 해제.
 *
 * @return: 없음(void).
 *
 * 실행 컨텍스트: module_exit 매크로에 의해 모듈 제거 시 1회 호출.
 *
 * 호출 체인:
 *   module_exit() 인프라 → [iolatency_exit] → blkcg_policy_unregister()
 */
static void __exit iolatency_exit(void)
{
	blkcg_policy_unregister(&blkcg_policy_iolatency);
	/* [한국어] 등록된 policy를 해제 — 모든 디바이스에서 io.latency 기능이
	 * 비활성화되고 cgroupfs 파일도 사라진다 */
}

module_init(iolatency_init);
/* [한국어] 커널 모듈/서브시스템 초기화 시 iolatency_init()을 호출하도록 등록
 * (블록 계층 초기화 순서에 맞춰 자동 실행되는 매크로) */
module_exit(iolatency_exit);
/* [한국어] 모듈 언로드 시 iolatency_exit()을 호출하도록 등록 */
/* [한국어] === 핵심 요약: io.latency가 실제로 하는 일 ===
 *
 * - 개입 지점은 bio다. blk_mq_submit_bio가 request를 얻기 전에
 *   rq_qos_throttle → blkcg_iolatency_throttle이 불리므로, 여기서 재우는
 *   태스크는 태그도 드라이버 자원도 잡고 있지 않다. 측정하는 지연 역시
 *   장치 지연이 아니라 bio가 블록 계층에 머문 전체 시간이다.
 * - 판정 방식은 미디어 특성에 따라 둘로 갈린다. 비회전식(ssd=true)은
 *   목표 초과 건수 비율(missed/total < 10%)로, 회전식은 윈도우 평균으로
 *   본다. 쓰기가 유난히 빨리 끝나는 장치에서 평균만 보면 실제 압박을
 *   과소평가하기 때문이다(파일 상단 원문 주석 참조).
 * - 집행 수단 1: max_depth. 기본값은 UINT_MAX(무제한)이고, 위반이 관측된
 *   뒤에야 값이 생긴다. 이건 하드웨어 큐 깊이가 아니라 이 cgroup이 동시에
 *   띄울 수 있는 bio 수에 대한 순수 소프트웨어 상한이다.
 * - 전파 방식: 위반한 그룹이 자기를 조이는 게 아니라, 부모의 공유
 *   scale_cookie를 내려 같은 부모 아래 형제들을 조인다. io.latency가
 *   상한이 아니라 보호 장치이기 때문이다. 절반씩 줄이고 1/16씩 늘리는
 *   비대칭 덕에 위반에는 빠르게, 회복에는 느리게 반응한다.
 * - 집행 수단 2: max_depth가 1까지 갔는데도 부족하고, 그 IO가 우선순위
 *   역전을 피하려 root 명의로 발행되는 종류(REQ_META/REQ_SWAP)라면 깊이
 *   조절이 통하지 않는다. 이때는 blkcg_add_delay()로 빚을 쌓아 두었다가
 *   해당 태스크가 유저스페이스로 복귀할 때 실제로 재운다.
 * - 죽어가는 태스크(fatal_signal_pending)와 root 명의 발행은 스로틀을
 *   우회한다 — 전자는 OOM 회복을 늦추지 않기 위해, 후자는 우선순위 역전을
 *   만들지 않기 위해서다.
 */
