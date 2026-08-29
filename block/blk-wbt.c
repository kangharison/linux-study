// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어] writeback 스로틀링(WBT) 구현 (block/blk-wbt.c)
 *
 * === 파일의 역할 ===
 * 이 파일이 푸는 문제는 하나다: 백그라운드 buffered writeback이 장치 큐를
 * 가득 채워, 뒤늦게 도착한 전경(foreground) 읽기가 그 쓰기 backlog 뒤에
 * 줄을 서면서 읽기 지연이 수십~수백 배로 늘어나는 현상이다. 페이지 캐시를
 * 비우는 쪽은 "얼마나 빨리 끝나는지"에 관심이 없으므로 큐를 채우는 데
 * 아무런 제동이 걸리지 않고, 그 대가는 전적으로 읽기 쪽이 치른다.
 * 네트워크의 bufferbloat과 같은 구조의 문제이며, 실제로 이 구현은 CoDel에서
 * 착안했다. 다만 CoDel은 패킷을 버려서 큐를 줄이지만 블록 계층은 I/O를
 * 버릴 수 없으므로, 대신 "동시에 발행할 수 있는 쓰기 개수(depth)"를 줄인다.
 * 감지는 관측으로 한다 — blk-stat으로 윈도우 단위 읽기 완료 지연을 모으고,
 * 그 윈도우의 최소 읽기 지연이 목표(min_lat_nsec)를 넘으면 큐가 쓰기로
 * 막혔다고 보고 depth를 절반으로 줄인다. 지연이 양호하면 다시 늘린다.
 * 즉 이 파일은 장치 종류나 프로토콜에 대한 지식이 전혀 없고, 오직
 * "읽기 지연 관측 → 쓰기 깊이 조절"이라는 폐루프 하나로만 동작한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * rq-qos(요청 큐 QoS) 계층에 rq_qos_ops로 등록되는 정책 중 하나다.
 * 실행 흐름:
 *   VFS write → page cache dirty → writeback 커널 스레드 → submit_bio
 *     → blk_mq_submit_bio → __rq_qos_throttle (blk-rq-qos.c)
 *     → wbt_wait (이 파일) → [한도 초과 시 rq_wait 대기큐에서 sleep]
 *     → blk_mq_get_new_requests → 드라이버 ->queue_rq
 *   장치 완료 인터럽트/폴링 → blk_mq_end_request → __rq_qos_done
 *     → wbt_done (이 파일) → inflight 감소 + 대기자 wake
 *   blk-stat 윈도우 만료 → wb_timer_fn (이 파일) → latency_exceeded
 *     → scale_up / scale_down
 * 실행 컨텍스트: wbt_wait은 제출 경로(프로세스 컨텍스트, sleep 가능),
 * wbt_done/wbt_track/wbt_issue는 완료·발행 경로(인터럽트 또는 softirq 가능),
 * wb_timer_fn은 blk-stat의 타이머 콜백 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-rq-qos.c/.h: rq_qos 체인 등록, throttle/track/issue/done 훅,
 *     rq_wait(inflight 카운터 + 대기큐), rq_depth(동적 depth 상태),
 *     rq_qos_wait()(공정한 대기 루프), rq_depth_scale_up/down()
 *   - block/blk-stat.c: blk_stat_callback으로 윈도우 단위 완료 지연 수집.
 *     버킷은 wbt_data_dir()가 READ/WRITE 2개로 나눈다.
 *   - include/linux/backing-dev.h: bdi — tracepoint 식별과
 *     wb_recent_wait()의 dirty throttle 이력 조회에 사용
 *   - include/linux/swap.h: current_is_kswapd() — swap 쓰기 분류
 * 이 모듈에 의존하는 모듈:
 *   - block/blk-mq.c: rq_qos 훅을 통해 간접 호출
 *   - block/blk-sysfs.c: wbt_lat_usec 속성이 wbt_init/wbt_set_min_lat 호출
 *   - block/genhd.c(add_disk 경로): wbt_enable_default()
 * 데이터 흐름: bio의 opf → bio_to_wbt_flags()가 WBT_* 그룹 플래그로 변환 →
 * wbt_track()이 request->wbt_flags에 기록 → wbt_done()이 그 플래그로 어느
 * rq_wait의 inflight를 되돌릴지 결정. 완료 지연은 blk-stat → wb_timer_fn →
 * rq_depth.max_depth → calc_wb_limits() → wb_normal/wb_background로 흐른다.
 *
 * === 주요 함수/구조체 요약 ===
 * wbt_wait()           : throttle 훅. 쓰기/discard bio가 그룹 한도를 넘으면
 *                        슬롯이 빌 때까지 대기시킨다(읽기는 대기시키지 않음).
 * wbt_done()           : done 훅. 읽기면 완료 시각 기록, 쓰기면 inflight 반환.
 * latency_exceeded()   : 윈도우의 최소 읽기 지연 vs min_lat_nsec 판정.
 * wb_timer_fn()        : blk-stat 윈도우 콜백. 판정 결과로 scale_up/down 선택.
 * scale_up/scale_down(): rq_depth.max_depth를 단계적으로 늘리거나 줄이고,
 *                        줄일 때는 관측 윈도우도 함께 좁혀 반응을 빠르게 한다.
 * wbt_init()           : per-disk rq_wb 초기화 및 rq_qos 체인 등록.
 * struct rq_wb:
 *   rq_wait[WBT_NUM_RWQ] : BG/SWAP/DISCARD 그룹별 inflight 카운터 + 대기큐
 *   rq_depth             : 동적 depth 상태 (max_depth, scale_step)
 *   win_nsec/cur_win_nsec: 기본/현재 관측 윈도우 (scale_step에 따라 축소)
 *   min_lat_nsec         : 목표 읽기 완료 지연 — 이 값을 넘으면 쓰기를 조인다
 */

/*
 * buffered writeback throttling. loosely based on CoDel. We can't drop
 * packets for IO scheduling, so the logic is something like this:
 *
 * - Monitor latencies in a defined window of time.
 * - If the minimum latency in the above window exceeds some target, increment
 *   scaling step and scale down queue depth by a factor of 2x. The monitoring
 *   window is then shrunk to 100 / sqrt(scaling step + 1).
 * - For any window where we don't have solid data on what the latencies
 *   look like, retain status quo.
 * - If latencies look good, decrement scaling step.
 * - If we're only doing writes, allow the scaling step to go negative. This
 *   will temporarily boost write performance, snapping back to a stable
 *   scaling step of 0 if reads show up or the heavy writers finish. Unlike
 *   positive scaling steps where we shrink the monitoring window, a negative
 *   scaling step retains the default step==0 window size.
 *
 * Copyright (C) 2016 Jens Axboe
 *
 */
#include <linux/kernel.h>		/* [한국어] 커널 기본 자료형 및 매크로 (min/max, likely/unlikely 등) */
#include <linux/blk_types.h>		/* [한국어] bio, request 관련 타입 정의 (REQ_OP_*, REQ_SYNC 등) */
#include <linux/slab.h>			/* [한국어] kzalloc_obj 등 메모리 할당 */
#include <linux/backing-dev.h>		/* [한국어] bdi(backing_dev_info), balance_dirty_pages 관련 */
#include <linux/swap.h>			/* [한국어] current_is_kswapd(): swap_writeout 경로 판별용 */

#include "blk-stat.h"			/* [한국어] blk_stat_callback: 요청 완료 지연 수집 콜백 */
#include "blk-wbt.h"			/* [한국어] struct rq_wb, wbt_init/exit 프로토타입 */
#include "blk-rq-qos.h"			/* [한국어] rq_qos_ops, rq_wait, rq_depth: bio/request QoS 훅 인프라 */
#include "elevator.h"			/* [한국어] elevator_registered(): IO 스케줄러 여부 확인 */
#include "blk.h"			/* [한국어] 블록 계층 내부 헤더 */

#define CREATE_TRACE_POINTS		/* [한국어] wbt tracepoint 정의 매크로 — 이 파일이 정의하는 trace_wbt_* 이벤트의
					   실체(구조체/포맷)를 이 번역 단위에서 생성한다 */
#include <trace/events/wbt.h>		/* [한국어] trace_wbt_stat/scale/lat 등 WBT 이벤트 tracepoint */

/*
 * [한국어] WBT가 request를 분류하는 플래그 — 장치 큐 발행 특성에 따라 구분
 */
enum wbt_flags {
	WBT_TRACKED		= 1,	/* [한국어] 장치 큐로 발행할 buffered write/discard — inflight 어카운팅 대상 */
	WBT_READ		= 2,	/* [한국어] 읽기 요청: 완료 latency를 샘플로 사용해 scale 결정 */
	WBT_SWAP		= 4,	/* [한국어] kswapd/swap_writeout 경로의 긴급 쓰기: BG와 별도 그룹 */
	WBT_DISCARD		= 8,	/* [한국어] discard(trim) 요청 */

	WBT_NR_BITS		= 4,	/* [한국어] 위 플래그 비트 수: rq->wbt_flags 필드 크기 결정에 사용 */
};

/*
 * [한국어] WBT가 관리하는 inflight 대기열 그룹 인덱스
 * BG/SWAP/DISCARD를 별도 rq_wait로 관리해 그룹별 장치 큐 요청 수를 독립 조절
 */
enum {
	WBT_RWQ_BG		= 0,	/* [한국어] 일반 background writeback 그룹 */
	WBT_RWQ_SWAP,			/* [한국어] swapout 긴급 쓰기 그룹 */
	WBT_RWQ_DISCARD,		/* [한국어] discard/trim 그룹 */
	WBT_NUM_RWQ,			/* [한국어] 그룹 수 = 3: rq_wait[WBT_NUM_RWQ] 배열 크기 */
};

/*
 * If current state is WBT_STATE_ON/OFF_DEFAULT, it can be covered to any other
 * state, if current state is WBT_STATE_ON/OFF_MANUAL, it can only be covered
 * to WBT_STATE_OFF/ON_MANUAL.
 */
/* [한국어] WBT 활성/비활성 상태 — DEFAULT는 자동 전이 가능, MANUAL은 수동 조작만 허용 */
enum {
	WBT_STATE_ON_DEFAULT	= 1,	/* [한국어] 기본 활성 상태: 디스크 초기화 시 자동으로 켜짐 */
	WBT_STATE_ON_MANUAL	= 2,	/* [한국어] 수동 활성 상태: sysfs/ioctl로 명시적으로 켬 */
	WBT_STATE_OFF_DEFAULT	= 3,	/* [한국어] 기본 비활성 상태: 장치 특성으로 비활성화 */
	WBT_STATE_OFF_MANUAL	= 4,	/* [한국어] 수동 비활성 상태: sysfs/ioctl로 명시적으로 끔 */
};

/*
 * [한국어]
 * struct rq_wb - 장치 큐에 대한 writeback 스로틀링 상태를 담는 per-device 자료구조
 *
 * blk-rq-qos 계층의 rq_qos를 내포하며, 장치 큐에 동시에 발행할 수 있는
 * 쓰기/discard 요청 수와 완료 latency 목표를 관리한다.
 * per-device(per-disk)로 하나씩 존재하며 wbt_init에서 할당된다.
 */
struct rq_wb {
	/*
	 * Settings that govern how we throttle
	 */
	/* [한국어] 아래 두 한도는 max_depth에서 파생되는 '집행용' 값이다.
	 * 피드백 루프가 정하는 것은 max_depth 하나뿐이고, calc_wb_limits()가
	 * 그것을 wb_normal(절반)/wb_background(1/4)로 쪼갠다. 굳이 나누는
	 * 이유는, 사용자가 기다리고 있는 쓰기(fsync 등)와 아무도 기다리지
	 * 않는 배경 writeback을 같은 한도로 다루면 후자가 전자의 몫까지
	 * 큐를 차지해 버리기 때문이다. */
	unsigned int wb_background;
	/* [한국어] 배경 writeback이 동시에 띄울 수 있는 최대 in-flight 요청 수.
	 * 설정자: calc_wb_limits()가 rq_depth.max_depth 기반으로 계산.
	 * 읽는 자: wbt_wait에서 rq_wait[WBT_RWQ_BG].inflight와 비교해 대기 여부 결정.
	 * 값 범위: 1 ~ max_depth, 항상 wb_normal의 절반 이하.
	 * 동기화: rq_wait[BG].lock으로 보호. */

	unsigned int wb_normal;
	/* [한국어] 일반 쓰기(쓰기+swap) 허용 최대 in-flight 요청 수.
	 * 설정자: calc_wb_limits()가 rq_depth.max_depth 기반으로 계산.
	 * 읽는 자: wbt_wait에서 rq_wait[WBT_RWQ_BG/SWAP].inflight 합산과 비교.
	 * 값 범위: 1 ~ max_depth.
	 * 동기화: rq_wait[BG/SWAP].lock으로 보호. */

	short enable_state;
	/* [한국어] WBT 활성/비활성 상태 (WBT_STATE_* 중 하나).
	 * 설정자: wbt_enable_default(), wbt_set_enable() (sysfs/ioctl).
	 * 읽는 자: rwb_enabled()로 참조해 WBT 스킵 여부 판단.
	 * 값 범위: WBT_STATE_ON_DEFAULT(1) ~ WBT_STATE_OFF_MANUAL(4).
	 * 동기화: 단일 스레드에서만 변경되므로 별도 락 불필요 (추정). */

	/*
	 * Number of consecutive periods where we don't have enough
	 * information to make a firm scale up/down decision.
	 */
	unsigned int unknown_cnt;
	/* [한국어] latency 샘플 부족이 연속된 횟수.
	 * 설정자: wbt_cb()에서 샘플 수가 RWB_MIN_WRITE_SAMPLES 미만일 때 증가.
	 * 읽는 자: wbt_cb에서 RWB_UNKNOWN_BUMP 초과 시 scale_step을 0으로 복귀.
	 * 값 범위: 0 ~ (무한): 샘플이 충분하면 0으로 리셋.
	 * 동기화: blk-stat 콜백(단일 컨텍스트)에서만 접근. */

	u64 win_nsec;
	/* [한국어] 기본 latency 샘플링 윈도우 (기본값: 100ms = 100*10^6 ns).
	 * 설정자: wbt_init()과 wbt_set_min_lat()에서 초기화.
	 * 읽는 자: wbt_cb()에서 cur_win_nsec를 결정할 때 기준값으로 사용.
	 * 동기화: 초기화 후 읽기 전용. */

	u64 cur_win_nsec;
	/* [한국어] 현재 동적으로 조절된 샘플링 윈도우.
	 * 설정자: wbt_cb()에서 scale 단계 상승 시 100/sqrt(step+1) 배로 줄임.
	 * 읽는 자: blk_stat_activate_nsecs()에 전달해 다음 콜백 타이밍 결정.
	 * 동기화: blk-stat 콜백(단일 컨텍스트)에서만 수정. */

	struct blk_stat_callback *cb;
	/* [한국어] blk-stat 요청 완료 지연 수집 콜백.
	 * 설정자: wbt_init()에서 blk_stat_alloc_callback(wbt_cb)으로 할당.
	 * 읽는 자: blk_stat_activate/deactivate에서 타이머 등록/해제.
	 * 동기화: blk-stat 내부 락으로 보호. */

	u64 sync_issue;
	/* [한국어] 마지막 sync 읽기 요청 발행 시각 (ktime_get_ns() 기준).
	 * 설정자: wbt_issue()에서 REQ_SYNC read 발행 시 기록.
	 * 읽는 자: wbt_done()에서 sync read 완료 latency 측정용.
	 * 동기화: 단일 sync I/O 추적이므로 sync_cookie와 함께 비교해 유효성 확인. */

	void *sync_cookie;
	/* [한국어] 마지막 sync read request 포인터 (역참조 금지, 식별자 용도만).
	 * 설정자: wbt_issue()에서 rq 포인터를 저장.
	 * 읽는 자: wbt_done()에서 rq == sync_cookie 비교로 동일 요청임을 확인.
	 * 동기화: 단일 I/O 컨텍스트에서만 동작하므로 별도 락 불필요. */

	unsigned long last_issue;
	/* [한국어] 마지막 읽기 요청 발행 시각 (jiffies).
	 * 설정자: wbt_issue()에서 read 발행 시 기록.
	 * 읽는 자: close_io()에서 100ms 이내 read 경쟁 여부 판단.
	 * 동기화: 단일 스레드 접근 가정, 별도 락 없음. */

	unsigned long last_comp;
	/* [한국어] 마지막 읽기 요청 완료 시각 (jiffies).
	 * 설정자: wbt_done()에서 read 완료 시 기록.
	 * 읽는 자: close_io()에서 100ms 이내 recent read 완료 여부 판단.
	 * 동기화: 단일 스레드 접근 가정. */

	unsigned long min_lat_nsec;
	/* [한국어] 읽기 요청의 목표 완료 지연 임계값 (ns).
	 * 설정자: wbt_set_min_lat()에서 sysfs/초기화 시 기록.
	 * 읽는 자: wbt_cb()에서 측정된 latency와 비교해 scale up/down 결정.
	 * 값 범위: 0이면 WBT 비활성, 양수이면 ns 단위 임계값. */

	struct rq_qos rqos;
	/* [한국어] rq-qos 연결 리스트 노드 — rq_wb를 blk-rq-qos 체인에 연결.
	 * container_of로 RQWB(rqos) 매크로를 통해 rq_wb를 복원.
	 * 설정자/읽는 자: blk-rq-qos.c의 rq_qos_add/del/get 함수들.
	 * 동기화: blk-rq-qos 내부 lock으로 보호. */

	struct rq_wait rq_wait[WBT_NUM_RWQ];
	/* [한국어] BG/SWAP/DISCARD 그룹별 in-flight 카운터 + 대기 큐.
	 * 설정자: wbt_wait()에서 rq_wait_inc_below()로 inflight 증가.
	 * 읽는 자: wbt_done()에서 감소 후 대기자 wake, wbt_wait에서 한도 비교.
	 * 동기화: 각 rq_wait.lock(spinlock)으로 독립적으로 보호. */

	struct rq_depth rq_depth;
	/* [한국어] 장치 큐에 대응하는 소프트웨어 dynamic queue depth 상태.
	 * 필드: max_depth(현재 허용 QD), default_depth, scaling_step.
	 * 설정자: scale_up/down()에서 rq_depth_scale_up/down()으로 조절.
	 * 읽는 자: calc_wb_limits()에서 wb_background/wb_normal 계산 기준.
	 * 동기화: blk-stat 콜백(단일 컨텍스트)에서만 수정. */
};

/* [한국어] wbt_init 전방 선언: wbt_rqos_exit 등에서 재초기화 시 필요 */
static int wbt_init(struct gendisk *disk, struct rq_wb *rwb);

/*
 * [한국어]
 * RQWB - rq_qos 포인터에서 rq_wb를 추출하는 helper
 *
 * @rqos: blk-rq-qos 체인의 노드 포인터
 * @return: rqos를 멤버로 갖는 rq_wb 포인터
 *
 * container_of로 rq_qos에서 rq_wb를 역산한다.
 * blk-rq-qos 훅(wbt_wait/wbt_done 등)은 rq_qos*를 인자로 받으므로
 * 내부에서 항상 RQWB(rqos)로 rq_wb에 접근한다.
 *
 * 호출 체인:
 *   wbt_wait / wbt_done / wbt_cb → [RQWB] → rq_wb 접근
 */
static inline struct rq_wb *RQWB(struct rq_qos *rqos)
{
	return container_of(rqos, struct rq_wb, rqos); /* [한국어] blk-rq-qos 리스트 노드에서 rq_wb 복원: 장치 큐 QoS 상태 접근 */
}

/*
 * [한국어]
 * wbt_clear_state - request의 WBT 플래그 초기화
 *
 * @rq: 초기화할 request
 *
 * request 완료 또는 재사용 시 wbt_flags를 0으로 초기화한다.
 * wbt_is_tracked/wbt_is_read가 이 필드를 기반으로 동작하므로
 * 이전 요청의 WBT 그룹 정보가 다음 요청으로 잘못 전달되지 않게 한다.
 *
 * 호출 체인:
 *   wbt_rqos_done / blk_mq_rq_ctx_init → [wbt_clear_state]
 */
static inline void wbt_clear_state(struct request *rq)
{
	rq->wbt_flags = 0; /* [한국어] WBT 플래그 초기화: WBT_TRACKED/READ/SWAP/DISCARD 모두 해제 */
}

/*
 * [한국어]
 * wbt_flags - request의 WBT 그룹 플래그 반환
 *
 * @rq: 조회할 request
 * @return: rq->wbt_flags (WBT_TRACKED|READ|SWAP|DISCARD 조합)
 *
 * 호출 체인:
 *   wbt_done / wbt_issue → [wbt_flags]
 */
static inline enum wbt_flags wbt_flags(struct request *rq)
{
	return rq->wbt_flags; /* [한국어] request의 WBT_TRACKED/READ/SWAP/DISCARD 플래그 조합 반환 */
}

/*
 * [한국어]
 * wbt_is_tracked - request가 WBT inflight 어카운팅 대상인지 확인
 *
 * @rq: 조회할 request
 * @return: WBT_TRACKED 비트가 설정되어 있으면 true
 *
 * true이면 이 request는 rq_wait.inflight 카운트에 포함되며,
 * 완료 시 wbt_done에서 inflight를 감소시킨다.
 *
 * 호출 체인:
 *   wbt_done → [wbt_is_tracked]
 */
static inline bool wbt_is_tracked(struct request *rq)
{
	return rq->wbt_flags & WBT_TRACKED; /* [한국어] WBT_TRACKED: 장치 큐 inflight 어카운팅 대상 쓰기/trim 요청 */
}

/*
 * [한국어]
 * wbt_is_read - request가 읽기 요청인지 확인
 *
 * @rq: 조회할 request
 * @return: WBT_READ 비트가 설정되어 있으면 true
 *
 * read 요청은 WBT inflight 제한 대상이 아니지만,
 * 완료 latency를 샘플로 수집해 scale 결정에 사용한다.
 *
 * 호출 체인:
 *   wbt_done / wbt_issue → [wbt_is_read]
 */
static inline bool wbt_is_read(struct request *rq)
{
	return rq->wbt_flags & WBT_READ; /* [한국어] WBT_READ: 읽기 요청 — latency 샘플 수집 대상 */
}

/*
 * [한국어] WBT queue depth 기본값 및 샘플링 정책 상수
 * 장치가 실제로 소화할 수 있는 하드웨어 큐 깊이와는 별개로, WBT는
 * 자기 자신의 소프트웨어 한도인 rq_depth.max_depth를 1~RWB_DEF_DEPTH
 * 사이에서만 움직인다. 목표가 "장치를 포화시키는 것"이 아니라
 * "읽기가 뒤에 줄 서지 않을 만큼만 쓰기를 흘리는 것"이기 때문이다.
 */
enum {
	/*
	 * Default setting, we'll scale up (to 75% of QD max) or down (min 1)
	 * from here depending on device stats
	 */
	RWB_DEF_DEPTH	= 16, /* [한국어] 기본 장치 큐 소프트웨어 깊이: scaling_step==0일 때 초기값 */

	/*
	 * 100msec window
	 */
	RWB_WINDOW_NSEC	= 100 * 1000 * 1000ULL, /* [한국어] 기본 latency 샘플링 윈도우 100ms (ns 단위) */

	/*
	 * Disregard stats, if we don't meet this minimum
	 */
	RWB_MIN_WRITE_SAMPLES = 3, /* [한국어] 통계 유효성 최소 샘플 수: 이보다 적으면 unknown으로 처리 */

	/*
	 * If we have this number of consecutive windows without enough
	 * information to scale up or down, slowly return to center state
	 * (step == 0).
	 */
	RWB_UNKNOWN_BUMP = 5, /* [한국어] 연속 unknown 윈도우 수 임계값: 초과 시 scale_step을 0으로 복귀 */
};

/*
 * [한국어]
 * rwb_enabled - WBT가 현재 활성 상태인지 확인
 *
 * @rwb: 조회할 rq_wb
 * @return: true이면 WBT 활성 (장치 큐 스로틀링 적용)
 *
 * NULL 체크와 enable_state 확인을 합친다. DEFAULT/MANUAL 모두 OFF이면
 * WBT가 완전히 비활성화되어 장치 큐에 대한 소프트웨어 제한이 없다.
 *
 * 호출 체인:
 *   wbt_wait / wbt_done / wb_timestamp → [rwb_enabled]
 */
static inline bool rwb_enabled(struct rq_wb *rwb)
{
	return rwb && rwb->enable_state != WBT_STATE_OFF_DEFAULT && /* [한국어] rwb NULL 체크 + DEFAULT 비활성 제외 */
		      rwb->enable_state != WBT_STATE_OFF_MANUAL; /* [한국어] MANUAL 비활성도 제외: 두 조건 모두 아닐 때 enabled */
}

/*
 * [한국어]
 * wb_timestamp - WBT 활성 상태일 때 현재 jiffies를 변수에 기록
 *
 * @rwb: WBT 상태
 * @var: 기록 대상 변수 (last_issue 또는 last_comp)
 *
 * WBT가 활성화된 경우 현재 jiffies를 var에 저장한다. 동일 jiffy가 아닐
 * 때만 갱신해 중복 기록을 피한다. 읽기 요청의 발행/완료 시각을
 * 기록하여 close_io()에서 100ms 이내 경쟁 IO 여부를 판단하는 데 쓴다.
 *
 * 호출 체인:
 *   wbt_issue(발행) → [wb_timestamp(&rwb->last_issue)]
 *   wbt_done(완료)  → [wb_timestamp(&rwb->last_comp)]
 */
static void wb_timestamp(struct rq_wb *rwb, unsigned long *var)
{
	if (rwb_enabled(rwb)) { /* [한국어] WBT 활성 시에만 시각 기록: 비활성이면 last_issue/last_comp 갱신 불필요 */
		const unsigned long cur = jiffies; /* [한국어] 현재 시각 캡처: monotonic jiffies 기반 */

		if (cur != *var) /* [한국어] 동일 jiffy 중복 기록 방지: 해상도 이하 이벤트를 별개로 구분하지 않음 */
			*var = cur; /* [한국어] last_issue 또는 last_comp 갱신: close_io에서 100ms 이내 활동 판단에 사용 */
	}
}

/*
 * [한국어]
 * wb_recent_wait - balance_dirty_pages에서 최근 1초 이내 rate throttle 이력 확인
 *
 * @rwb: WBT 상태
 * @return: true이면 1초 이내에 상위 dirty throttle 대기가 있었음
 *
 * balance_dirty_pages가 last_bdp_sleep을 기록한다. 1초 이내이면 상위 계층이
 * 이미 쓰기를 제한했으므로 장치 큐 wbt_rqw_done에서 wake 임계값을 낮춰
 * 큐를 더 적극적으로 채워도 된다고 본다.
 *
 * 호출 체인:
 *   get_limit / wbt_rqw_done → [wb_recent_wait]
 */
static bool wb_recent_wait(struct rq_wb *rwb)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* [한국어] 디스크에 연결된 bdi: dirty page rate 제어 정보 보유 */

	return time_before(jiffies, bdi->last_bdp_sleep + HZ); /* [한국어] HZ(1초) 이내 sleep 이력 → dirty throttle이 최근에 있었음 */
}

/*
 * [한국어]
 * get_rq_wait - WBT 플래그에 따라 올바른 rq_wait 그룹 반환
 *
 * @rwb: WBT 상태
 * @wb_acct: 요청의 WBT 플래그 (WBT_SWAP, WBT_DISCARD, 또는 BG)
 * @return: 해당 그룹의 rq_wait 포인터 (inflight 카운터 + 대기큐)
 *
 * SWAP/DISCARD/BG 세 그룹에 각각 독립적인 장치 큐 발행 한도를 적용하기
 * 위해 플래그 기반으로 대응하는 rq_wait를 선택한다.
 *
 * 호출 체인:
 *   __wbt_wait / __wbt_done → [get_rq_wait]
 */
static inline struct rq_wait *get_rq_wait(struct rq_wb *rwb,
					  enum wbt_flags wb_acct)
{
	if (wb_acct & WBT_SWAP) /* [한국어] kswapd 쓰기: SWAP 그룹 — BG와 독립적으로 장치 큐 한도 관리 */
		return &rwb->rq_wait[WBT_RWQ_SWAP]; /* [한국어] swap 쓰기를 BG와 섞으면, 메모리 회수가 배경 writeback의
					     * 한도에 갇혀 시스템 전체가 OOM 쪽으로 밀린다. 그래서 별도 카운터를 준다. */
	else if (wb_acct & WBT_DISCARD) /* [한국어] discard/trim: DISCARD 그룹 — 배경 한도로만 제한 */
		return &rwb->rq_wait[WBT_RWQ_DISCARD]; /* [한국어] discard는 장치에서 매우 오래 걸릴 수 있어
						* 일반 쓰기와 카운터를 공유하면 쓰기 슬롯을 통째로 잠식한다. */

	return &rwb->rq_wait[WBT_RWQ_BG]; /* [한국어] 일반 background writeback: BG 그룹 */
}

/*
 * [한국어]
 * rwb_wake_all - 모든 WBT 대기 그룹의 대기 태스크를 깨운다
 *
 * @rwb: WBT 상태
 *
 * scale_up이나 wb_limit 갱신으로 장치 큐 허용 깊이가 늘었을 때
 * BG/SWAP/DISCARD 세 그룹 모두에서 sleep 중인 submit 경로를 깨운다.
 * 깨어난 태스크는 wbt_wait의 rq_wait_inc_below에서 다시 한도를 확인한다.
 *
 * 호출 체인:
 *   scale_up / wbt_update_limits → [rwb_wake_all] → wake_up_all
 */
static void rwb_wake_all(struct rq_wb *rwb)
{
	int i; /* [한국어] 그룹 인덱스: WBT_RWQ_BG(0), WBT_RWQ_SWAP(1), WBT_RWQ_DISCARD(2) */

	for (i = 0; i < WBT_NUM_RWQ; i++) { /* [한국어] 세 그룹 순회: 모든 그룹의 장치 큐 대기자 깨움 */
		struct rq_wait *rqw = &rwb->rq_wait[i]; /* [한국어] i번째 그룹의 inflight 카운터 + 대기 큐 */

		if (wq_has_sleeper(&rqw->wait)) /* [한국어] 해당 그룹에 sleep 중인 submit 경로 존재 시에만 wake */
			wake_up_all(&rqw->wait); /* [한국어] 장치 큐 여유 신호: sleep 중인 wbt_wait 경로 일제 깨움 */
	}
}

/*
 * [한국어]
 * wbt_rqw_done - 요청 완료 후 rq_wait 그룹의 inflight 감소 + 대기 태스크 wake
 *
 * @rwb: WBT 상태
 * @rqw: 완료된 request가 속한 rq_wait 그룹 (BG/SWAP/DISCARD)
 * @wb_acct: 완료된 request의 WBT 플래그
 *
 * atomic_dec_return으로 inflight를 원자적으로 감소시킨다. 감소 후 inflight가
 * 한도 이하로 낮아지면 대기 중인 submit 경로를 깨운다. Wake 임계값은 DISCARD는
 * wb_background, write cache 있고 dirty 대기 없으면 0(즉시 깨우지 않음),
 * 나머지는 wb_normal로 결정한다.
 * 완료 처리 후 blk_mq_end_request → rq_qos_done → wbt_done 경로에서 호출.
 *
 * 호출 체인:
 *   __wbt_done / wbt_cleanup_cb → [wbt_rqw_done] → wake_up_all
 */
static void wbt_rqw_done(struct rq_wb *rwb, struct rq_wait *rqw,
			 enum wbt_flags wb_acct)
{
	int inflight, limit; /* [한국어] inflight: 감소 후 장치 큐 in-flight 수, limit: wake 임계값 */

	inflight = atomic_dec_return(&rqw->inflight); /* [한국어] 원자적 inflight 감소: 완료했으므로 in-flight 슬롯 반환 */

	/*
	 * For discards, our limit is always the background. For writes, if
	 * the device does write back caching, drop further down before we
	 * wake people up.
	 */
	if (wb_acct & WBT_DISCARD) /* [한국어] discard는 지연 예측이 어려우므로 깨우는 기준도 가장 보수적인 배경 한도로 고정 */
		/* [한국어] DISCARD: 항상 wb_background 한도 사용 (더 엄격한 제한) */
		limit = rwb->wb_background;
	else if (blk_queue_write_cache(rwb->rqos.disk->queue) && /* [한국어] write-back 캐시가 있으면 '완료'는 매체 기록이 아니라
								  * 캐시 적재만 뜻한다 — 완료를 곧이곧대로 믿고 바로 다음 쓰기를
								  * 밀어 넣으면 캐시가 터질 때 지연이 한꺼번에 튄다. */
		 !wb_recent_wait(rwb)) /* [한국어] 단, 상위 balance_dirty_pages가 최근에 이미 태스크를 재웠다면
					* dirty 생성 자체가 눌린 상태이므로 여기서 추가로 조일 필요는 없다. */
		/* [한국어] write cache 있고 dirty throttle 이력 없음: 완료해도 바로 깨우지 않음 (장치 write cache가 흡수해 줄 여지가 있음) */
		limit = 0;
	else /* [한국어] 위 두 특수 상황이 아니면 평상시 한도로 깨운다 */
		limit = rwb->wb_normal; /* [한국어] 일반 쓰기/swap: wb_normal 한도에서 wake */

	/*
	 * Don't wake anyone up if we are above the normal limit.
	 */
	if (inflight && inflight >= limit) /* [한국어] inflight가 여전히 한도 이상이면 wake 불필요: 아직 여유가 없어 깨워도 다시 잘 것 */
		return;

	if (wq_has_sleeper(&rqw->wait)) { /* [한국어] 대기자가 없으면 wake_up_all의 barrier/스핀락 비용조차 아깝다 —
					   * 완료는 매우 빈번한 경로라 이 선검사가 실제로 유의미하다. */
		int diff = limit - inflight; /* [한국어] 장치 큐에 추가 발행 가능한 요청 여유량 */

		if (!inflight || diff >= rwb->wb_background / 2) /* [한국어] 큐가 비었거나 여유가 wb_background/2 이상이면 일제 wake */
			wake_up_all(&rqw->wait); /* [한국어] sleep 중인 wbt_wait submit 경로 깨움: 요청 제출 재개 */
	}
}

/*
 * [한국어]
 * __wbt_done - WBT_TRACKED request 완료 시 inflight accounting 정리
 *
 * @rqos: blk-rq-qos 노드 (RQWB()로 rq_wb 역참조)
 * @wb_acct: 완료된 request의 WBT 플래그
 *
 * WBT_TRACKED 플래그가 없으면 (read 등) 즉시 반환한다.
 * get_rq_wait으로 그룹을 찾아 wbt_rqw_done에서 inflight를 감소시킨다.
 *
 * 호출 체인:
 *   wbt_done / wbt_cleanup → [__wbt_done] → wbt_rqw_done
 */
static void __wbt_done(struct rq_qos *rqos, enum wbt_flags wb_acct)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] rq_qos → rq_wb 역참조: 장치 큐 QoS 상태 접근 */
	struct rq_wait *rqw; /* [한국어] 완료된 요청이 속한 그룹의 rq_wait */

	if (!(wb_acct & WBT_TRACKED)) /* [한국어] WBT 추적 대상이 아니면 (read, NOMERGE 등) 회계 skip */
		return;

	rqw = get_rq_wait(rwb, wb_acct); /* [한국어] BG/SWAP/DISCARD 중 해당 그룹의 rq_wait 선택 */
	wbt_rqw_done(rwb, rqw, wb_acct); /* [한국어] inflight 감소 + 한도 이하이면 대기 태스크 wake */
}

/*
 * [한국어]
 * wbt_done - request 완료 시 WBT 상태 정리 (rq_qos_ops.done 훅)
 *
 * @rqos: blk-rq-qos 노드
 * @rq: 완료된 request
 *
 * 완료 처리 후 blk_mq_end_request → rq_qos_done → wbt_done 경로에서 호출.
 * WBT_TRACKED이면 inflight 감소(→ __wbt_done), WBT_READ이면 완료 시각 기록.
 * sync_cookie가 이 rq이면 sync_issue/sync_cookie를 초기화한다.
 *
 * 호출 체인:
 *   장치 완료 처리 → blk_mq_complete_request → rq_qos_done
 *     → [wbt_done] → __wbt_done → wbt_rqw_done
 */
static void wbt_done(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] rq_qos → rq_wb: 장치 큐 QoS 상태 접근 */

	if (!wbt_is_tracked(rq)) { /* [한국어] 스로틀 대상이 아니었던 요청 — inflight를 올린 적이 없으니 내릴 것도 없다.
				    * 대신 읽기라면 '언제 마지막으로 읽기가 끝났는가'라는 관측 신호만 갱신한다. */
		/* [한국어] WBT_TRACKED가 없으면 읽기 요청 완료 경로 */
		if (wbt_is_read(rq)) { /* [한국어] 읽기 요청 완료: latency 샘플 수집, sync 추적 */
			if (rwb->sync_cookie == rq) { /* [한국어] 추적 중인 sync 읽기 요청이 완료됨 */
				rwb->sync_issue = 0; /* [한국어] sync read 발행 시각 추적 해제 */
				rwb->sync_cookie = NULL; /* [한국어] sync_cookie 해제: 다음 sync read를 위해 초기화 */
			}

			wb_timestamp(rwb, &rwb->last_comp); /* [한국어] 읽기 요청 완료 시각 기록: close_io에서 최근 read 여부 판단 */
		}
	} else {
		WARN_ON_ONCE(rq == rwb->sync_cookie); /* [한국어] tracked write가 sync_cookie이면 버그: write는 sync_cookie로 등록되지 않음 */
		__wbt_done(rqos, wbt_flags(rq)); /* [한국어] tracked 요청 inflight 감소: 완료에 따른 in-flight 슬롯 반환 */
	}
	wbt_clear_state(rq); /* [한국어] request 재사용 전 WBT 플래그 초기화: wbt_flags를 0으로 */
}

/*
 * [한국어]
 * stat_sample_valid - blk-stat 샘플이 scale 결정에 충분한지 확인
 *
 * @stat: blk_rq_stat 배열 (READ/WRITE 인덱스)
 * @return: true이면 샘플 유효 (장치 큐 부하 판단 가능)
 *
 * 읽기 요청 1개 이상 + 쓰기 요청 3개(RWB_MIN_WRITE_SAMPLES) 이상이
 * 필요하다. read 없이 write만 있으면 저전력 idle이 아닌 쓰기 부하임을
 * 확인할 수 없다.
 *
 * 호출 체인:
 *   latency_exceeded → [stat_sample_valid]
 */
static inline bool stat_sample_valid(struct blk_rq_stat *stat)
{
	/*
	 * We need at least one read sample, and a minimum of
	 * RWB_MIN_WRITE_SAMPLES. We require some write samples to know
	 * that it's writes impacting us, and not just some sole read on
	 * a device that is in a lower power state.
	 */
	return (stat[READ].nr_samples >= 1 && /* [한국어] 읽기 요청 완료 샘플 1개 이상: 저전력 idle과 구분하기 위해 */
		stat[WRITE].nr_samples >= RWB_MIN_WRITE_SAMPLES); /* [한국어] 쓰기 요청 샘플 3개 이상: 쓰기 부하가 실제로 있음을 확인 */
}

/*
 * [한국어]
 * rwb_sync_issue_lat - sync 읽기 요청이 장치 큐에 발행된 후 경과 시간 반환
 *
 * @rwb: WBT 상태
 * @return: sync 읽기 요청 큐 체류 시간 (ns), 요청이 없으면 0
 *
 * blk-stat 윈도우에 완료 이벤트가 없더라도 sync_issue/sync_cookie를 통해
 * 진행 중인 sync 읽기 요청의 발행~현재 경과 시간을 추적한다. 이 시간이
 * 윈도우보다 크면 완료 보고 없이도 latency 위반으로 판정한다.
 *
 * 호출 체인:
 *   latency_exceeded → [rwb_sync_issue_lat]
 */
static u64 rwb_sync_issue_lat(struct rq_wb *rwb)
{
	u64 issue = READ_ONCE(rwb->sync_issue); /* [한국어] sync 읽기 요청 발행 시각 읽기: READ_ONCE로 컴파일러 재정렬 방지 */

	if (!issue || !rwb->sync_cookie) /* [한국어] sync 읽기 요청이 없거나 쿠키가 없으면 체류 시간 없음 */
		return 0; /* [한국어] 체류 시간 0: latency_exceeded에서 위반 없음으로 처리 */

	return blk_time_get_ns() - issue; /* [한국어] 현재 시각 - 발행 시각 = 요청의 장치 큐 체류 시간 */
}

/*
 * [한국어]
 * wbt_inflight - 세 WBT 그룹의 현재 in-flight 총합 반환
 *
 * @rwb: WBT 상태
 * @return: BG + SWAP + DISCARD 그룹의 inflight 합계
 *
 * 장치 큐에 발행되어 아직 완료 보고를 받지 못한 요청 총수를 소프트웨어에서
 * 추정한다. atomic_read로 각 그룹을 읽으므로 완료 경로와 race 가능하나
 * 근사값으로 충분하다.
 *
 * 호출 체인:
 *   latency_exceeded / wb_timer_fn → [wbt_inflight]
 */
static inline unsigned int wbt_inflight(struct rq_wb *rwb)
{
	unsigned int i, ret = 0; /* [한국어] 세 그룹의 inflight 합산용 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹 순회 */
		ret += atomic_read(&rwb->rq_wait[i].inflight); /* [한국어] 각 그룹의 atomic inflight 읽기: 합산으로 장치 큐 추정 요청 수 계산 */

	return ret; /* [한국어] 전체 장치 큐 추정 in-flight 요청 수 */
}

/* [한국어] latency_exceeded() 반환값 — 장치 큐 부하 상태 분류 */
enum {
	LAT_OK = 1,		/* [한국어] 장치 큐 latency가 목표 이하: scale_up 가능 */
	LAT_UNKNOWN,		/* [한국어] 샘플 부족으로 판단 불가: scale 유지 */
	LAT_UNKNOWN_WRITES,	/* [한국어] read 샘플 없이 쓰기만 진행 중: 음수 step으로 boost 가능 */
	LAT_EXCEEDED,		/* [한국어] 장치 큐 latency가 목표 초과: scale_down 필요 */
};

/*
 * [한국어]
 * latency_exceeded - blk-stat 샘플과 sync read 발행 시각으로 장치 큐 부하 판정
 *
 * @rwb: WBT 상태
 * @stat: blk-stat이 수집한 READ/WRITE 완료 latency 샘플 배열
 * @return: LAT_OK / LAT_UNKNOWN / LAT_UNKNOWN_WRITES / LAT_EXCEEDED
 *
 * 요청 완료 데이터(stat) + sync read 발행 이후 경과 시간(rwb_sync_issue_lat)
 * 두 가지 신호로 큐 포화 여부를 판단한다. 포화 시 LAT_EXCEEDED를 반환하여
 * wb_timer_fn이 scale_down을 호출하도록 유도한다.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer)
 *
 * 호출 체인:
 *   wb_timer_fn → [latency_exceeded]
 *                  → rwb_sync_issue_lat
 *                  → stat_sample_valid
 *                  → wb_recent_wait / wbt_inflight
 */
static int latency_exceeded(struct rq_wb *rwb, struct blk_rq_stat *stat)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* [한국어] 디스크 BDI 참조: trace 이벤트 식별용 */
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] 장치 큐 소프트웨어 depth 상태 참조 */
	u64 thislat; /* [한국어] sync 읽기 요청의 큐 체류/완료 지연 (ns) */
	/*
	 * If our stored sync issue exceeds the window size, or it
	 * exceeds our min target AND we haven't logged any entries,
	 * flag the latency as exceeded. wbt works off completion latencies,
	 * but for a flooded device, a single sync IO can take a long time
	 * to complete after being issued. If this time exceeds our
	 * monitoring window AND we didn't see any other completions in that
	 * window, then count that sync IO as a violation of the latency.
	 */
	thislat = rwb_sync_issue_lat(rwb); /* [한국어] sync 읽기 요청이 장치 큐에서 머문 시간 측정 */
	if (thislat > rwb->cur_win_nsec || /* [한국어] 모니터링 윈도우 전체를 초과: 완료가 윈도우보다 오래 걸림 = 큐가 막힌 것으로 판정 */
	    (thislat > rwb->min_lat_nsec && !stat[READ].nr_samples)) { /* [한국어] read 샘플 없이 목표 초과: 큐가 막혀 완료 보고가 돌아오지 않는 상태로 의심 */
		trace_wbt_lat(bdi, thislat); /* [한국어] tracepoint: 장치 큐 latency 위반 기록 */
		return LAT_EXCEEDED; /* [한국어] 큐 포화 판정 -> scale_down 유도 */
	}

	/*
	 * No read/write mix, if stat isn't valid
	 */
	if (!stat_sample_valid(stat)) { /* [한국어] 요청 완료 샘플 불충분: read 1개 + write 3개 미만 */
		/*
		 * If we had writes in this stat window and the window is
		 * current, we're only doing writes. If a task recently
		 * waited or still has writes in flights, consider us doing
		 * just writes as well.
		 */
		if (stat[WRITE].nr_samples || wb_recent_wait(rwb) || /* [한국어] 쓰기 완료 샘플 존재 또는 태스크가 최근 대기/인플라이트 */
		    wbt_inflight(rwb)) /* [한국어] 장치 큐에 미완료 요청이 있어 write-only 부하로 판단 */
			return LAT_UNKNOWN_WRITES; /* [한국어] write-only: 음수 step에서 boost, 양수 step에서는 scale_up 억제 */
		return LAT_UNKNOWN; /* [한국어] 샘플 없음: 판단 불가, scale 유지 */
	}

	/*
	 * If the 'min' latency exceeds our target, step down.
	 */
	if (stat[READ].min > rwb->min_lat_nsec) { /* [한국어] 읽기 완료 지연의 최솟값가 목표 초과: 쓰기 backlog가 읽기를 밀어내는 중 */
		trace_wbt_lat(bdi, stat[READ].min); /* [한국어] tracepoint: 읽기 지연 목표 위반 기록 */
		trace_wbt_stat(bdi, stat); /* [한국어] tracepoint: 장치 큐 전체 통계 기록 */
		return LAT_EXCEEDED; /* [한국어] latency 목표 초과 -> scale_down 유도 */
	}

	if (rqd->scale_step) /* [한국어] scale_step이 0이 아니면 depth 조정 중 */
		trace_wbt_stat(bdi, stat); /* [한국어] tracepoint: scale 조정 중 지연 통계 기록 */

	return LAT_OK; /* [한국어] latency 양호 -> scale_up 가능 신호 */
}

/*
 * [한국어]
 * rwb_trace_step - WBT scale 조정 단계를 tracepoint로 기록
 *
 * @rwb: WBT 상태
 * @msg: 조정 방향 문자열 ("scale up" / "scale down")
 *
 * scale_up/scale_down이 장치 큐 깊이를 변경할 때마다 현재 scale_step,
 * 윈도우 크기, BG/Normal 한도, max_depth를 tracepoint에 남겨 디버깅을
 * 돕는다.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer)
 *
 * 호출 체인:
 *   scale_up / scale_down / wb_timer_fn → [rwb_trace_step]
 */
static void rwb_trace_step(struct rq_wb *rwb, const char *msg)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* [한국어] 디스크 BDI 참조: tracepoint 식별용 */
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] 장치 큐 소프트웨어 depth 상태 */

	trace_wbt_step(bdi, msg, rqd->scale_step, rwb->cur_win_nsec, /* [한국어] tracepoint: scale 방향, 현재 step, 윈도우(ns) 기록 */
			rwb->wb_background, rwb->wb_normal, rqd->max_depth); /* [한국어] BG/Normal 요청 한도 및 max_depth 기록 */
}

/*
 * [한국어]
 * calc_wb_limits - rq_depth.max_depth에서 wb_normal/wb_background 한도 계산
 *
 * @rwb: WBT 상태
 *
 * 장치 큐의 동시 발행 요청 상한(max_depth)이 변경될 때마다 호출된다.
 * 전체 depth를 일반 쓰기(wb_normal)와 배경 쓰기(wb_background)로
 * 나눠 read latency 보호를 위한 쓰기 요청 발행 한도를 설정한다.
 *
 * 실행 컨텍스트: blk_stat_callback / wbt_init 컨텍스트
 *
 * 호출 체인:
 *   scale_up / scale_down / wbt_update_limits / wbt_init → [calc_wb_limits]
 */
static void calc_wb_limits(struct rq_wb *rwb)
{
	if (rwb->min_lat_nsec == 0) { /* [한국어] WBT 목표 latency 0: 스로틀링 비활성 */
		rwb->wb_normal = rwb->wb_background = 0; /* [한국어] 한도 0 → get_limit()가 max_depth를 그대로 반환하도록 */
	} else if (rwb->rq_depth.max_depth <= 2) { /* [한국어] 장치 큐 깊이가 매우 작을 때 보수적 분배 */
		rwb->wb_normal = rwb->rq_depth.max_depth; /* [한국어] 일반 쓰기에 전체 장치 큐 깊이 사용 */
		rwb->wb_background = 1; /* [한국어] 배경 쓰기 최소 1 요청 보장: depth가 매우 작으면 background도 1 */
	} else {
		rwb->wb_normal = (rwb->rq_depth.max_depth + 1) / 2; /* [한국어] 일반 쓰기: 장치 큐 깊이의 절반 (반올림) */
		rwb->wb_background = (rwb->rq_depth.max_depth + 3) / 4; /* [한국어] 배경 쓰기: 장치 큐 깊이의 1/4 (반올림), read latency 우선 보호 */
	}
}

/*
 * [한국어]
 * scale_up - 장치 큐 소프트웨어 depth를 한 단계 증가
 *
 * @rwb: WBT 상태
 *
 * latency가 양호(LAT_OK, LAT_UNKNOWN_WRITES)하거나 UNKNOWN 상태에서
 * 이미 step이 양수일 때 호출된다. rq_depth_scale_up으로 max_depth를
 * 올리고 wb_normal/wb_background를 재계산한 뒤 대기 태스크를 깨운다.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer)
 *
 * 호출 체인:
 *   wb_timer_fn → [scale_up]
 *                  → rq_depth_scale_up
 *                  → calc_wb_limits
 *                  → rwb_wake_all
 *                  → rwb_trace_step
 */
static void scale_up(struct rq_wb *rwb)
{
	if (!rq_depth_scale_up(&rwb->rq_depth)) /* [한국어] 장치 큐 max_depth 증가 실패 (이미 최대): 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background 한도 재계산: depth 증가 반영 */
	rwb->unknown_cnt = 0; /* [한국어] LAT_UNKNOWN 연속 카운터 초기화: scale_up으로 상태 리셋 */
	rwb_wake_all(rwb); /* [한국어] 대기 중인 submit 태스크 깨우기: 새로운 depth 한도로 요청 발행 재개 */
	rwb_trace_step(rwb, tracepoint_string("scale up")); /* [한국어] tracepoint: 장치 큐 깊이 증가 기록 */
}

/*
 * [한국어]
 * scale_down - 장치 큐 소프트웨어 depth를 한 단계 감소
 *
 * @rwb: WBT 상태
 * @hard_throttle: true이면 CoDel 최소값까지 강하게 축소 (LAT_EXCEEDED),
 *                 false이면 한 단계씩 점진적 축소
 *
 * latency가 목표를 초과(LAT_EXCEEDED)하거나 UNKNOWN 상태에서 step이
 * 이미 음수일 때 호출된다. rq_depth_scale_down으로 max_depth를 내리고
 * wb_normal/wb_background를 재계산한다. scale_up과 달리 깨우기는 없다.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer)
 *
 * 호출 체인:
 *   wb_timer_fn → [scale_down]
 *                  → rq_depth_scale_down
 *                  → calc_wb_limits
 *                  → rwb_trace_step
 */
static void scale_down(struct rq_wb *rwb, bool hard_throttle)
{
	if (!rq_depth_scale_down(&rwb->rq_depth, hard_throttle)) /* [한국어] 장치 큐 max_depth 감소 실패 (최소 1): 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background 한도 재계산: depth 감소 반영 */
	rwb->unknown_cnt = 0; /* [한국어] LAT_UNKNOWN 연속 카운터 초기화: scale_down으로 상태 리셋 */
	rwb_trace_step(rwb, tracepoint_string("scale down")); /* [한국어] tracepoint: 장치 큐 깊이 감소 기록 */
}

/*
 * [한국어]
 * rwb_arm_timer - blk-stat 콜백 타이머를 다음 샘플링 윈도우로 재설정
 *
 * @rwb: WBT 상태
 *
 * scale_step이 양수(큐 혼잡)이면 기본 100ms 윈도우를 역제곱근 축소하여
 * 장치 큐 포화 상태를 더 빠르게 감지한다. step이 0 이하이면 기본 윈도우를
 * 유지한다. blk_stat_activate_nsecs 호출로 완료 지연 수집을 재시작.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer) 또는 submit 경로
 *
 * 호출 체인:
 *   wbt_wait / wb_timer_fn → [rwb_arm_timer]
 *                             → blk_stat_activate_nsecs
 */
static void rwb_arm_timer(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] 장치 큐 소프트웨어 depth 상태 참조 */

	if (rqd->scale_step > 0) { /* [한국어] 큐 혼잡 상태: 짧은 윈도우로 빠른 latency 반응 */
		/*
		 * We should speed this up, using some variant of a fast
		 * integer inverse square root calculation. Since we only do
		 * this for every window expiration, it's not a huge deal,
		 * though.
		 */
		rwb->cur_win_nsec = div_u64(rwb->win_nsec << 4, /* [한국어] 기본 윈도우를 16배 확대 후 역제곱근으로 나눠 윈도우 축소 */
					int_sqrt((rqd->scale_step + 1) << 8)); /* [한국어] scale_step이 클수록 모니터링 주기가 더 짧아짐 */
	} else {
		/*
		 * For step < 0, we don't want to increase/decrease the
		 * window size.
		 */
		rwb->cur_win_nsec = rwb->win_nsec; /* [한국어] 지연이 양호할 때 기본 100ms 샘플링 윈도우 유지 */
	}

	blk_stat_activate_nsecs(rwb->cb, rwb->cur_win_nsec); /* [한국어] blk-stat 타이머 재설정: cur_win_nsec 후 wb_timer_fn 재호출 */
}

/*
 * [한국어]
 * wb_timer_fn - blk-stat 콜백: 완료 지연 샘플 분석 후 장치 큐 깊이 조정
 *
 * @cb: blk_stat_callback (cb->data = rq_wb, cb->stat = READ/WRITE 샘플)
 *
 * blk-stat이 cur_win_nsec 윈도우 동안 수집한 요청 완료 지연 샘플을
 * latency_exceeded()로 분석하여 scale_up/scale_down을 호출한다. LAT_UNKNOWN
 * 상태가 RWB_UNKNOWN_BUMP 회 연속되면 scale_step을 0 방향으로 복원한다.
 *
 * 실행 컨텍스트: 소프트웨어 IRQ (blk_stat 내부 hrtimer 만료)
 *
 * 호출 체인:
 *   blk_stat_callback → [wb_timer_fn]
 *                        → latency_exceeded
 *                        → scale_up / scale_down
 *                        → rwb_arm_timer
 */
static void wb_timer_fn(struct blk_stat_callback *cb)
{
	struct rq_wb *rwb = cb->data; /* [한국어] blk-stat 콜백 data 필드에서 rq_wb 복원 */
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] 장치 큐 소프트웨어 depth 상태 */
	unsigned int inflight = wbt_inflight(rwb); /* [한국어] 현재 장치 큐 추정 in-flight 요청 합계 */
	int status; /* [한국어] latency_exceeded()의 4값 판정 결과 — 아래 switch가 이 값 하나로 방향을 정한다 */

	if (!rwb->rqos.disk) /* [한국어] 장치 제거 중: disk 포인터 없으면 종료 */
		return;

	status = latency_exceeded(rwb, cb->stat); /* [한국어] 완료 지연 샘플로 큐 부하 상태 판정 */

	trace_wbt_timer(rwb->rqos.disk->bdi, status, rqd->scale_step, inflight); /* [한국어] tracepoint: 판정 결과/scale_step/inflight 기록 */

	/*
	 * If we exceeded the latency target, step down. If we did not,
	 * step one level up. If we don't know enough to say either exceeded
	 * or ok, then don't do anything.
	 */
	switch (status) { /* [한국어] 판정 → 행동 매핑. '모르겠다'가 두 종류인 이유는, 읽기 샘플이 없는 상황이
			   * (a) 그냥 한가한 것과 (b) 쓰기만 몰아치는 것으로 나뉘고 대응이 반대이기 때문이다. */
	case LAT_EXCEEDED: /* [한국어] 읽기가 실제로 밀렸다 — 즉시, 강하게 줄인다(hard_throttle=true) */
		scale_down(rwb, true); /* [한국어] 장치 큐 latency 초과: 동시 발행 요청 수 강제 감소 */
		break;
	case LAT_OK: /* [한국어] 목표를 지키고 있다 — 한 단계만 푼다. 한 번에 되돌리면 다시 막히는 진동이 생긴다 */
		scale_up(rwb); /* [한국어] 장치 큐 latency 양호: 발행 한도 1단계 증가 */
		break;
	case LAT_UNKNOWN_WRITES: /* [한국어] 읽기가 아예 없음 = 보호할 대상이 없음. 이때만 step을 음수로 내려
				  * 기본 depth 이상으로 쓰기를 밀어준다. 읽기가 등장하면 곧바로 0으로 되돌아온다. */
		/*
		 * We don't have a valid read/write sample, but we do have
		 * writes going on. Allow step to go negative, to increase
		 * write performance.
		 */
		scale_up(rwb); /* [한국어] write-only 부하: 음수 step까지 허용해 장치 큐 쓰기 throughput 극대화 */
		break;
	case LAT_UNKNOWN: /* [한국어] 판단 근거 자체가 없음. 곧장 움직이지 않고 RWB_UNKNOWN_BUMP회를 센다 —
			   * 근거 없이 조정하면 한가한 구간의 잡음으로 depth가 표류한다. */
		if (++rwb->unknown_cnt < RWB_UNKNOWN_BUMP) /* [한국어] RWB_UNKNOWN_BUMP 미만이면 아직 대기 */
			break;
		/*
		 * We get here when previously scaled reduced depth, and we
		 * currently don't have a valid read/write sample. For that
		 * case, slowly return to center state (step == 0).
		 */
		if (rqd->scale_step > 0) /* [한국어] 이전에 장치 큐 깊이 축소 상태: 점진적 복원 */
			scale_up(rwb); /* [한국어] depth 1단계 확대 -> 발행 허용량 증가 */
		else if (rqd->scale_step < 0) /* [한국어] 이전에 write-only boost 상태: 점진적 수렴 */
			scale_down(rwb, false); /* [한국어] 부드럽게 장치 큐 깊이 중앙으로 복귀 (hard_throttle=false) */
		break;
	default:
		break;
	}

	/*
	 * Re-arm timer, if we have IO in flight
	 */
	if (rqd->scale_step || inflight) /* [한국어] 깊이 조정 중이거나 in-flight 요청이 있으면 다음 윈도우 예약 */
		rwb_arm_timer(rwb); /* [한국어] 다음 완료 지연 샘플링 타이머 재설정 */
}

/*
 * [한국어]
 * wbt_update_limits - queue depth 변경 시 WBT scale 상태를 초기화하고 한도 재계산
 *
 * @rwb: WBT 상태
 *
 * 블록 드라이버가 queue depth를 변경하거나 min_lat_nsec가 바뀔 때 호출된다.
 * scale_step을 0으로 리셋하고 rq_depth_calc_max_depth로 max_depth를 재계산한
 * 뒤 calc_wb_limits로 wb_normal/wb_background를 갱신한다.
 *
 * 실행 컨텍스트: wbt_init (초기화) 또는 sysfs/드라이버 콜백
 *
 * 호출 체인:
 *   wbt_init / wbt_set_min_lat / wbt_rqos_queue_depth_changed → [wbt_update_limits]
 *                                                                 → rq_depth_calc_max_depth
 *                                                                 → calc_wb_limits
 *                                                                 → rwb_wake_all
 */
static void wbt_update_limits(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] 장치 큐 소프트웨어 depth 상태 참조 */

	rqd->scale_step = 0; /* [한국어] 장치 큐 깊이 중앙 상태로 리셋: 이전 scale_up/down 기록 초기화 */
	rqd->scaled_max = false; /* [한국어] hardware max depth 미도달 플래그 초기화 */

	rq_depth_calc_max_depth(rqd); /* [한국어] blk-mq queue depth 기반 장치 큐 max_depth 재계산 */
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background 요청 한도 재계산 */

	rwb_wake_all(rwb); /* [한국어] 한도 변경으로 대기 중인 제출 대기 태스크 깨우기 */
}

/*
 * [한국어]
 * wbt_disabled - 현재 WBT가 비활성 상태인지 확인
 *
 * @q: request_queue (디스크 큐)
 * @return: true이면 WBT 꺼짐 (제출 제한 없음)
 *
 * WBT rq_qos가 없거나 enable_state가 OFF인 경우 true를 반환한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio 또는 외부 → [wbt_disabled]
 */
bool wbt_disabled(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] request_queue에서 WBT rq_qos 객체 검색 */

	return !rqos || !rwb_enabled(RQWB(rqos)); /* [한국어] WBT 없거나 꺼져 있으면 제출 제한 없음 */
}

/*
 * [한국어]
 * wbt_get_min_lat - WBT의 현재 목표 read latency 반환
 *
 * @q: request_queue
 * @return: min_lat_nsec (ns), WBT 없으면 0
 *
 * sysfs나 드라이버에서 현재 설정된 읽기 요청 목표 완료 latency를 조회할 때
 * 사용한다.
 *
 * 호출 체인:
 *   sysfs read → [wbt_get_min_lat]
 */
u64 wbt_get_min_lat(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] WBT QoS 핸들 획득 */
	if (!rqos) /* [한국어] 디스크에 WBT가 설치되지 않은 경우 0 반환 */
		return 0;
	return RQWB(rqos)->min_lat_nsec; /* [한국어] 읽기 요청 목표 완료 latency (ns) 반환 */
}

/*
 * [한국어]
 * wbt_set_min_lat - WBT의 목표 read latency를 변경하고 상태 갱신
 *
 * @q: request_queue
 * @val: 새 목표 latency (ns), 0이면 WBT 비활성화
 *
 * sysfs 또는 드라이버에서 목표 latency를 변경할 때 호출된다. val이 0이면
 * WBT_STATE_OFF_MANUAL로 스로틀링을 완전히 해제하고, 0이 아니면
 * WBT_STATE_ON_MANUAL로 활성화한다.
 *
 * 호출 체인:
 *   sysfs write / wbt_adjust_qd → [wbt_set_min_lat]
 *                                   → wbt_update_limits
 */
static void wbt_set_min_lat(struct request_queue *q, u64 val)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] WBT QoS 핸들 획득 */
	if (!rqos) /* [한국어] WBT 미설치 시 무시 */
		return;

	RQWB(rqos)->min_lat_nsec = val; /* [한국어] 읽기 요청 목표 완료 latency 갱신 */
	if (val) /* [한국어] latency 값이 있으면 수동 WBT 활성화 */
		RQWB(rqos)->enable_state = WBT_STATE_ON_MANUAL; /* [한국어] 수동 WBT 켬: 소프트웨어 스로틀링 활성 */
	else /* [한국어] val==0은 '목표 지연 없음' = 사용자가 명시적으로 끈 것으로 해석한다 */
		RQWB(rqos)->enable_state = WBT_STATE_OFF_MANUAL; /* [한국어] 수동 WBT 끔: 제출 제한 해제 */

	wbt_update_limits(RQWB(rqos)); /* [한국어] 새 목표 latency에 맞춰 요청 발행 한도 재설정 */
}


/*
 * [한국어]
 * close_io - 최근 100ms 이내에 읽기 요청 발행/완료 이력 확인
 *
 * @rwb: WBT 상태
 * @return: true이면 최근 read 활동 있음 (쓰기 요청 한도를 더 낮게 적용)
 *
 * last_issue (sync read 발행 시각) 또는 last_comp (sync read 완료 시각)
 * 기준으로 100ms 이내이면 read 경쟁이 활발하다고 보고 쓰기 요청 한도를
 * wb_background로 낮춘다.
 *
 * 호출 체인:
 *   get_limit → [close_io]
 */
static bool close_io(struct rq_wb *rwb)
{
	const unsigned long now = jiffies; /* [한국어] 현재 jiffies 시각 */

	return time_before(now, rwb->last_issue + HZ / 10) || /* [한국어] 100ms 이내에 sync 읽기를 발행한 이력 */
		time_before(now, rwb->last_comp + HZ / 10); /* [한국어] 100ms 이내 sync 읽기 완료 이력 */
}

/* [한국어] REQ_HIPRIO: 장치 큐에서 우선 처리할 sync/meta/prio/swap bio 플래그 조합 */
#define REQ_HIPRIO	(REQ_SYNC | REQ_META | REQ_PRIO | REQ_SWAP)

/*
 * [한국어]
 * get_limit - bio 우선순위에 따른 장치 큐 in-flight 요청 상한 반환
 *
 * @rwb: WBT 상태
 * @opf: bio의 opf 플래그 (REQ_SYNC, REQ_BACKGROUND, REQ_SWAP 등)
 * @return: 이 bio가 사용할 수 있는 장치 큐 동시 발행 요청 상한
 *
 * bio 우선순위에 따라 max_depth / wb_normal / wb_background 중 하나를
 * 반환한다. sync/swap/HIPRIO bio는 장치 큐를 최대한 활용하고, 배경 쓰기는
 * wb_background로 read latency 보호를 위해 발행량을 줄인다.
 *
 * 호출 체인:
 *   wbt_inflight_cb → [get_limit]
 *                      → close_io
 */
static inline unsigned int get_limit(struct rq_wb *rwb, blk_opf_t opf)
{
	unsigned int limit; /* [한국어] 이 bio의 장치 큐 동시 발행 요청 상한 */

	if ((opf & REQ_OP_MASK) == REQ_OP_DISCARD) /* [한국어] discard 요청은 배경 한도로 제한 */
		return rwb->wb_background; /* [한국어] discard 동시 발행 개수 제한: read와 경쟁 최소화 */

	/*
	 * At this point we know it's a buffered write. If this is
	 * swap trying to free memory, or REQ_SYNC is set, then
	 * it's WB_SYNC_ALL writeback, and we'll use the max limit for
	 * that. If the write is marked as a background write, then use
	 * the idle limit, or go to normal if we haven't had competing
	 * IO for a bit.
	 */
	if ((opf & REQ_HIPRIO) || wb_recent_wait(rwb)) /* [한국어] sync/swap 고우선순위 또는 상위 태스크 대기 이력 */
		limit = rwb->rq_depth.max_depth; /* [한국어] 장치 큐 최대 depth까지 허용: read latency 경쟁 낮음 판단 */
	else if ((opf & REQ_BACKGROUND) || close_io(rwb)) { /* [한국어] 배경 쓰기 또는 최근 100ms 내 read 활동 */
		/*
		 * If less than 100ms since we completed unrelated IO,
		 * limit us to half the depth for background writeback.
		 */
		limit = rwb->wb_background; /* [한국어] read latency 보호를 위해 배경 한도로 요청 발행 제한 */
	} else /* [한국어] 고우선순위도 아니고 배경도 아닌 평범한 쓰기 — 가운데 한도를 준다 */
		limit = rwb->wb_normal; /* [한국어] 일반 쓰기: 중간 한도 (wb_normal ≈ max_depth / 2) */

	return limit; /* [한국어] bio→request 변환 전 허용 장치 큐 요청 발행 상한 */
}

/*
 * [한국어]
 * struct wbt_wait_data - rq_qos_wait 콜백에 전달되는 bio 컨텍스트
 *
 * bio가 장치 큐 진입 전 rq_wait에서 대기할 때 wbt_inflight_cb /
 * wbt_cleanup_cb에 전달된다. get_limit 호출에 필요한 opf와 WBT 회계에
 * 필요한 wb_acct를 묶어 단일 private_data로 전달한다.
 */
struct wbt_wait_data {
	struct rq_wb *rwb;
	/* [한국어] 장치 큐 WBT 상태 포인터.
	 * 설정자: __wbt_wait()에서 초기화.
	 * 읽는 자: wbt_inflight_cb, wbt_cleanup_cb.
	 * 동기화: 단일 bio submit 스레드에서만 사용 — 별도 락 불필요. */

	enum wbt_flags wb_acct;
	/* [한국어] 이 bio의 장치 큐 요청 그룹 분류 (WBT_TRACKED | WBT_SWAP | WBT_DISCARD 등).
	 * 설정자: __wbt_wait()에서 bio_to_wbt_flags() 결과를 전달.
	 * 읽는 자: wbt_cleanup_cb → wbt_rqw_done으로 회계 롤백.
	 * 값 범위: wbt_flags 열거형 비트 조합. */

	blk_opf_t opf;
	/* [한국어] bio의 opf 플래그 (REQ_SYNC / REQ_BACKGROUND / REQ_SWAP 등).
	 * 설정자: __wbt_wait()에서 bio->bi_opf를 전달.
	 * 읽는 자: wbt_inflight_cb → get_limit으로 장치 큐 동시 발행 상한 결정.
	 * 값 범위: blk_opf_t 비트 조합. */
};

/*
 * [한국어]
 * wbt_inflight_cb - rq_qos_wait 대기 재개 시 장치 큐 in-flight 슬롯 원자 획득 시도
 *
 * @rqw: 이 bio의 요청 그룹 rq_wait (bg/swap/discard 중 하나)
 * @private_data: struct wbt_wait_data 포인터
 * @return: true이면 inflight 증가 성공 (장치 큐 진입 허가),
 *          false이면 한도 초과 (rq_qos_wait가 다시 대기)
 *
 * rq_qos_wait가 태스크를 깨울 때마다 호출되며, rq_wait_inc_below로
 * inflight를 원자적으로 증가시킨다. limit는 get_limit으로 동적 계산.
 *
 * 호출 체인:
 *   rq_qos_wait → [wbt_inflight_cb]
 *                  → rq_wait_inc_below
 *                  → get_limit
 */
static bool wbt_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct wbt_wait_data *data = private_data; /* [한국어] rq_qos_wait가 전달한 bio 컨텍스트 */
	return rq_wait_inc_below(rqw, get_limit(data->rwb, data->opf)); /* [한국어] 현재 요청 한도 미만이면 atomic inflight 증가 -> 장치 큐 진입 허가 */
}

/*
 * [한국어]
 * wbt_cleanup_cb - rq_qos_wait 대기 취소 시 장치 큐 in-flight 회계 롤백
 *
 * @rqw: 요청 그룹 rq_wait
 * @private_data: struct wbt_wait_data 포인터
 *
 * rq_qos_wait에서 sleep이 취소(시그널, 타임아웃 등)되거나 실패하면 호출된다.
 * wbt_inflight_cb가 inflight를 이미 증가시킨 경우 wbt_rqw_done으로 롤백.
 *
 * 호출 체인:
 *   rq_qos_wait → [wbt_cleanup_cb]
 *                  → wbt_rqw_done
 */
static void wbt_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	struct wbt_wait_data *data = private_data; /* [한국어] 취소된 bio 컨텍스트 */
	wbt_rqw_done(data->rwb, rqw, data->wb_acct); /* [한국어] 장치 큐 진입 실패 시 in-flight 회계 롤백 (inflight 감소 + 대기 큐 깨우기) */
}

/*
 * [한국어]
 * __wbt_wait - 장치 큐 in-flight 한도 초과 시 bio를 대기시킴
 *
 * @rwb: WBT 상태
 * @wb_acct: 이 bio의 요청 그룹 (WBT_TRACKED / WBT_SWAP / WBT_DISCARD)
 * @opf: bio의 opf 플래그 (get_limit에서 장치 큐 동시 발행 상한 결정용)
 *
 * blk_mq_submit_bio → blk_mq_get_request 이전에 실행되어 장치 큐
 * 장치에 발행하기 전에 소프트웨어적으로 요청 발행을 제어한다.
 * rq_qos_wait가 wbt_inflight_cb를 반복 호출하며 슬롯이 생길 때까지 대기.
 *
 * 실행 컨텍스트: bio submit 경로 (태스크 컨텍스트), 슬립 가능
 *
 * 호출 체인:
 *   wbt_wait → [__wbt_wait]
 *               → get_rq_wait
 *               → rq_qos_wait (wbt_inflight_cb / wbt_cleanup_cb)
 */
static void __wbt_wait(struct rq_wb *rwb, enum wbt_flags wb_acct,
		       blk_opf_t opf)
{
	struct rq_wait *rqw = get_rq_wait(rwb, wb_acct); /* [한국어] 요청 그룹별 rq_wait 선택 (bg/swap/discard) */
	struct wbt_wait_data data = { /* [한국어] rq_qos_wait()의 콜백은 인자를 하나만 받으므로, 재시도마다
				       * 다시 계산해야 하는 값들을 이 스택 구조체에 묶어 넘긴다. */
		.rwb = rwb, /* [한국어] 장치 큐 WBT 상태 전달 */
		.wb_acct = wb_acct, /* [한국어] 요청 그룹 전달: 회계 및 한도 계산에 사용 */
		.opf = opf, /* [한국어] bio 플래그 전달: get_limit에서 장치 큐 우선순위 결정 */
	};

	rq_qos_wait(rqw, &data, wbt_inflight_cb, wbt_cleanup_cb); /* [한국어] in-flight 슬롯이 빌 때까지 대기 — request 할당보다 앞이라 태그/드라이버 자원을 붙들지 않은 채 잔다 */
}

/*
 * [한국어]
 * wbt_should_throttle - bio가 WBT 스로틀링 대상인지 판별
 *
 * @bio: 검사할 bio
 * @return: true이면 WBT 추적/스로틀링 대상 (buffered write 또는 discard)
 *
 * buffered write와 discard/trim만 장치 큐 동시 발행 제한을 적용한다.
 * O_DIRECT sync write (REQ_SYNC|REQ_IDLE 조합)는 제외 — 직접 I/O는
 * 페이지 캐시를 거치지 않아 제출 폭주 위험이 낮다.
 *
 * 호출 체인:
 *   bio_to_wbt_flags → [wbt_should_throttle]
 */
static inline bool wbt_should_throttle(struct bio *bio)
{
	switch (bio_op(bio)) { /* [한국어] bio opcode로 장치 큐 스로틀링 대상 판별 */
	case REQ_OP_WRITE: /* [한국어] 쓰기 요청 후보: O_DIRECT 여부 추가 확인 필요 */
		/*
		 * Don't throttle WRITE_ODIRECT
		 */
		if ((bio->bi_opf & (REQ_SYNC | REQ_IDLE)) == /* [한국어] O_DIRECT sync write: REQ_SYNC|REQ_IDLE 동시 설정 */
		    (REQ_SYNC | REQ_IDLE)) /* [한국어] O_DIRECT 경로는 제출 폭주 우려 낮음 → 제외 */
			return false; /* [한국어] 장치 큐 스로틀링 대상 아님 */
		fallthrough; /* [한국어] buffered write는 아래 discard와 같이 true 반환 */
	case REQ_OP_DISCARD: /* [한국어] discard/TRIM 요청: write와 같이 장치 큐 한도 적용 */
		return true; /* [한국어] 장치 큐 동시 발행 제한 필요 */
	default:
		return false; /* [한국어] read 등은 별도 추적/제한 없음 */
	}
}

/*
 * [한국어]
 * bio_to_wbt_flags - bio 특성에 따른 WBT 요청 그룹 플래그 생성
 *
 * @rwb: WBT 상태
 * @bio: 분류할 bio
 * @return: wbt_flags 비트 조합 (WBT_READ / WBT_TRACKED / WBT_SWAP / WBT_DISCARD)
 *
 * bio가 장치 큐의 어떤 요청 그룹에 속할지 결정한다. read이면 WBT_READ (latency
 * 샘플링), buffered write/discard이면 WBT_TRACKED를 설정하고 swap/discard 여부에
 * 따라 추가 비트를 설정한다.
 *
 * 호출 체인:
 *   wbt_wait / wbt_track / wbt_cleanup → [bio_to_wbt_flags]
 *                                         → wbt_should_throttle
 */
static enum wbt_flags bio_to_wbt_flags(struct rq_wb *rwb, struct bio *bio)
{
	enum wbt_flags flags = 0; /* [한국어] 요청 그룹 플래그 초기화 */

	if (!rwb_enabled(rwb)) /* [한국어] WBT 꺼져 있으면 추적 플래그 없음 (제출 제한 없음) */
		return 0;

	if (bio_op(bio) == REQ_OP_READ) { /* [한국어] 읽기 요청 분류: latency 샘플링 대상 */
		flags = WBT_READ; /* [한국어] 읽기 요청: sync_issue/sync_cookie 추적 대상 */
	} else if (wbt_should_throttle(bio)) { /* [한국어] buffered write / discard 요청 */
		if (bio->bi_opf & REQ_SWAP) /* [한국어] swapout으로 생성된 긴급 쓰기: swap 그룹으로 분리 */
			flags |= WBT_SWAP; /* [한국어] swap 그룹 rq_wait 사용: OOM 방지 우선순위 유지 */
		if (bio_op(bio) == REQ_OP_DISCARD) /* [한국어] discard/TRIM 요청 */
			flags |= WBT_DISCARD; /* [한국어] discard 그룹 rq_wait 사용: wb_background 한도 적용 */
		flags |= WBT_TRACKED; /* [한국어] 장치 큐 inflight 추적 대상: __wbt_wait로 대기 */
	}
	return flags; /* [한국어] bio→request 변환 시 request->rq_flags에 복사될 WBT 요청 그룹 */
}

/*
 * [한국어]
 * wbt_cleanup - bio가 request로 변환되지 못한 경우 장치 큐 in-flight 회계 정리
 *
 * @rqos: WBT rq_qos
 * @bio: 실패/중단된 bio
 *
 * bio가 병합되거나 오류로 request가 생성되지 않은 경우 wbt_wait에서
 * 증가시킨 inflight를 __wbt_done으로 감소시키고 대기 태스크를 깨운다.
 *
 * 실행 컨텍스트: blk_mq_submit_bio 실패 경로 (태스크 컨텍스트)
 *
 * 호출 체인:
 *   rq_qos_cleanup → [wbt_cleanup]
 *                     → bio_to_wbt_flags
 *                     → __wbt_done
 */
static void wbt_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 장치 큐 WBT 상태 복원 */
	enum wbt_flags flags = bio_to_wbt_flags(rwb, bio); /* [한국어] 실패한 bio의 요청 그룹 재판별 */
	__wbt_done(rqos, flags); /* [한국어] 장치 큐 진입 전 실패 시 inflight 감소 + 대기 큐 깨우기 */
}

/*
 * [한국어]
 * wbt_wait - bio가 request로 변환되기 전 WBT 스로틀링 수행
 *
 * @rqos: WBT rq_qos
 * @bio: 제출 중인 bio
 *
 * blk_mq_submit_bio → __rq_qos_throttle 훅으로 호출된다. 장치 큐의
 * 장치에 발행하기 직전 마지막 소프트웨어 요청 발행 제어 지점이다.
 * WBT_TRACKED bio이면 __wbt_wait로 슬롯을 대기하고, read이면
 * last_issue를 갱신하여 close_io 판단에 활용한다.
 *
 * 실행 컨텍스트: bio submit 경로 (태스크 컨텍스트), 슬립 가능
 *
 * 호출 체인:
 *   blk_mq_submit_bio → __rq_qos_throttle → [wbt_wait]
 *                                             → bio_to_wbt_flags
 *                                             → __wbt_wait → rq_qos_wait
 *                                             → rwb_arm_timer
 */
/* May sleep, if we have exceeded the writeback limits. */
static void wbt_wait(struct rq_qos *rqos, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 장치 큐 WBT 상태 복원 */
	enum wbt_flags flags; /* [한국어] 이 bio의 장치 큐 요청 그룹 */

	flags = bio_to_wbt_flags(rwb, bio); /* [한국어] bio → 장치 큐 요청 그룹 분류 */
	if (!(flags & WBT_TRACKED)) { /* [한국어] tracked write/discard가 아니면 스로틀링 없이 통과 */
		if (flags & WBT_READ) /* [한국어] 읽기 요청면 발행 시각 갱신: close_io 판단에 사용 */
			wb_timestamp(rwb, &rwb->last_issue); /* [한국어] last_issue = 현재 시각: 100ms 내 read 활동 기록 */
		return; /* [한국어] writeback throttling 불필요: 장치 큐 직행 */
	}

	__wbt_wait(rwb, flags, bio->bi_opf); /* [한국어] 요청 한도 도달 시 슬롯 확보까지 대기: 장치 큐 발행 지연 */

	if (!blk_stat_is_active(rwb->cb)) /* [한국어] blk-stat latency 타이머가 비활성이면 */
		rwb_arm_timer(rwb); /* [한국어] 요청 완료 지연 모니터링 타이머 시작 */
}

/*
 * [한국어]
 * wbt_track - bio가 request와 결합될 때 WBT 요청 그룹 플래그를 request에 기록
 *
 * @rqos: WBT rq_qos
 * @rq: 생성된 request
 * @bio: 결합된 bio
 *
 * blk_mq_submit_bio → rq_qos_track 훅으로 호출된다. request->wbt_flags에
 * bio의 요청 그룹(WBT_TRACKED / WBT_SWAP / WBT_DISCARD)을 OR 적산하여
 * wbt_issue / wbt_done에서 회계를 연결한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → rq_qos_track → [wbt_track]
 *                                        → bio_to_wbt_flags
 */
static void wbt_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 장치 큐 WBT 상태 복원 */
	rq->wbt_flags |= bio_to_wbt_flags(rwb, bio); /* [한국어] request에 요청 그룹 OR 적산: 다중 bio 병합 시 플래그 누적 */
}

/*
 * [한국어]
 * wbt_issue - request를 장치 큐에 발행할 때 sync read 발행 시각 기록
 *
 * @rqos: WBT rq_qos
 * @rq: 드라이버로 발행되기 직전의 request
 *
 * blk_mq_issue_request → rq_qos_issue 훅으로 호출된다. sync 읽기 요청이
 * 장치 큐에서 오래 머무는 경우 blk-stat 완료 이벤트 없이도 latency 초과를
 * 조기에 감지하기 위해 sync_issue에 발행 시각을 기록한다.
 *
 * 실행 컨텍스트: blk_mq_issue_request 경로 (태스크 또는 kblockd)
 *
 * 호출 체인:
 *   blk_mq_issue_request → rq_qos_issue → [wbt_issue]
 */
static void wbt_issue(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 장치 큐 WBT 상태 복원 */

	if (!rwb_enabled(rwb)) /* [한국어] WBT off면 sync read 발행 시각 추적 불필요 */
		return;

	/*
	 * Track sync issue, in case it takes a long time to complete. Allows us
	 * to react quicker, if a sync IO takes a long time to complete. Note
	 * that this is just a hint. The request can go away when it completes,
	 * so it's important we never dereference it. We only use the address to
	 * compare with, which is why we store the sync_issue time locally.
	 */
	if (wbt_is_read(rq) && !rwb->sync_issue) { /* [한국어] 읽기 요청이고 아직 추적 중인 sync read 없을 때 */
		rwb->sync_cookie = rq; /* [한국어] sync read 식별용 쿠키: 완료 시 주소 비교용 (역참조 금지) */
		rwb->sync_issue = rq->io_start_time_ns; /* [한국어] 장치 큐 발행 시각: io_start_time_ns 저장 */
	}
}

/*
 * [한국어]
 * wbt_requeue - request가 장치 큐에서 취소되어 재큐될 때 sync_cookie 정리
 *
 * @rqos: WBT rq_qos
 * @rq: 재큐된 request
 *
 * blk_mq_requeue_request → rq_qos_requeue 훅으로 호출된다. 재큐된 request가
 * sync_cookie와 일치하면 sync_issue를 0으로 초기화하여 잘못된 latency
 * 측정을 방지한다.
 *
 * 호출 체인:
 *   blk_mq_requeue_request → rq_qos_requeue → [wbt_requeue]
 */
static void wbt_requeue(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 장치 큐 WBT 상태 복원 */
	if (!rwb_enabled(rwb)) /* [한국어] WBT off면 재큐 추적 불필요 */
		return;
	if (rq == rwb->sync_cookie) { /* [한국어] 재큐된 request가 추적 중인 sync 읽기 요청면 */
		rwb->sync_issue = 0; /* [한국어] 이전 발행 시각 무효화: 재발행 후 새로 측정 */
		rwb->sync_cookie = NULL; /* [한국어] sync_cookie 해제: latency_exceeded에서 잘못된 체류 시간 계산 방지 */
	}
}

/*
 * [한국어]
 * wbt_data_dir - request의 방향을 blk-stat READ/WRITE 버킷 인덱스로 변환
 *
 * @rq: 완료된 request
 * @return: READ(0), WRITE(1), 또는 -1 (discard 등 제외)
 *
 * blk_stat_alloc_callback에 함수 포인터로 등록된다. 요청 완료 이벤트가
 * blk-stat 콜백에 도달할 때 호출되어 read/write 완료 latency를 별도 버킷에
 * 분리하여 저장하도록 방향을 지정한다.
 *
 * 호출 체인:
 *   blk_stat_callback → [wbt_data_dir] (콜백 등록 시 함수 포인터)
 */
static int wbt_data_dir(const struct request *rq)
{
	const enum req_op op = req_op(rq); /* [한국어] request opcode로 읽기/쓰기 버킷 판별 */

	if (op == REQ_OP_READ) /* [한국어] 읽기 요청: READ 버킷 */
		return READ; /* [한국어] 이 버킷이 곧 판정의 근거 — latency_exceeded()가 보는 stat[READ] */
	else if (op_is_write(op)) /* [한국어] 쓰기/플러시 계열: WRITE 버킷 */
		return WRITE; /* [한국어] 쓰기 샘플은 '쓰기 부하가 실제로 있는가'의 판별에만 쓰이고 목표 비교 대상은 아니다 */

	/* don't account */
	return -1; /* [한국어] Discard/Flush 등: WBT latency 샘플 제외 */
}

/*
 * [한국어]
 * wbt_alloc - rq_wb 상태 객체와 blk-stat 콜백 할당
 *
 * @return: 초기화된 rq_wb, 실패 시 NULL
 *
 * wbt_init_enable_default 또는 wbt_set_lat에서 호출된다. kzalloc으로 rq_wb를
 * 할당하고 blk_stat_alloc_callback으로 READ/WRITE 2채널 latency 샘플링 콜백을
 * 등록한다. 실패 시 rq_wb를 해제하고 NULL을 반환한다.
 *
 * 호출 체인:
 *   wbt_init_enable_default → [wbt_alloc]
 *                              → kzalloc_obj
 *                              → blk_stat_alloc_callback(wb_timer_fn, wbt_data_dir)
 */
static struct rq_wb *wbt_alloc(void)
{
	struct rq_wb *rwb = kzalloc_obj(*rwb); /* [한국어] 0으로 초기화 할당 — scale_step/unknown_cnt/sync_issue가
						* 0이어야 '중앙 상태에서 시작'이라는 초기 조건이 성립한다 */

	if (!rwb) /* [한국어] 메모리 부족: WBT 설치 실패, 제출 제한 없음 */
		return NULL; /* [한국어] WBT는 있으면 좋은 최적화일 뿐이라, 실패해도 I/O 경로는 그대로 동작한다 */

	rwb->cb = blk_stat_alloc_callback(wb_timer_fn, wbt_data_dir, 2, rwb); /* [한국어] blk-stat 콜백 등록: READ/WRITE 2채널 완료 지연 샘플링 */
	if (!rwb->cb) { /* [한국어] blk-stat 콜백 할당 실패 */
		kfree(rwb); /* [한국어] rq_wb 해제 */
		return NULL; /* [한국어] WBT 계층 미설치 */
	}

	return rwb; /* [한국어] 장치 큐 스로틀링 상태 객체 반환 */
}

/*
 * [한국어]
 * wbt_free - rq_wb 및 blk-stat 콜백 해제
 *
 * @rwb: 해제할 rq_wb
 *
 * wbt_rqos_exit에서 호출된다. blk_stat_free_callback으로 latency 샘플링
 * 콜백을 해제하고 kfree로 rq_wb 메모리를 반환한다.
 *
 * 호출 체인:
 *   wbt_rqos_exit → [wbt_free]
 */
static void wbt_free(struct rq_wb *rwb)
{
	blk_stat_free_callback(rwb->cb); /* [한국어] 완료 지연 샘플링 콜백 해제 */
	kfree(rwb); /* [한국어] 장치 큐 WBT 상태 메모리 반환 */
}

/*
 * [한국어]
 * __wbt_enable_default - 디스크에 WBT를 기본 활성화할 조건 판단
 *
 * @disk: 검사할 gendisk (디스크)
 * @return: true이면 WBT 신규 설치 필요, false이면 불필요
 *
 * CONFIG_BLK_WBT_MQ 설정, blk_queue_disable_wbt 플래그, 기존 WBT 존재 여부,
 * 디스크 등록 상태, blk-mq 여부를 순서대로 확인한다. 이미 WBT가 존재하면
 * enable_state만 갱신하고 false를 반환 (재설치 불필요).
 *
 * 실행 컨텍스트: add_disk 경로 또는 외부 호출 (디스크 등록 시)
 *
 * 호출 체인:
 *   wbt_enable_default / wbt_init_enable_default → [__wbt_enable_default]
 */
static bool __wbt_enable_default(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] 디스크의 request_queue */
	struct rq_qos *rqos; /* [한국어] 기존 WBT rq_qos 핸들 */
	bool enable = IS_ENABLED(CONFIG_BLK_WBT_MQ); /* [한국어] blk-mq WBT 컴파일 옵션: blk-mq 지원 빌드이면 true */

	mutex_lock(&disk->rqos_state_mutex); /* [한국어] rq_qos 상태 보호: 디스크 등록/해제와의 race 방지 */

	if (blk_queue_disable_wbt(q)) /* [한국어] 큐에서 WBT 비활성화 플래그 설정 시 */
		enable = false; /* [한국어] WBT 금지: 제출 제한 없음 유지 */

	/* Throttling already enabled? */
	rqos = wbt_rq_qos(q); /* [한국어] 이미 설치된 WBT rq_qos 검색 */
	if (rqos) { /* [한국어] WBT 이미 존재: enable_state만 갱신 */
		if (enable && RQWB(rqos)->enable_state == WBT_STATE_OFF_DEFAULT) /* [한국어] 금지 해제되고 기본 off 상태면 */
			RQWB(rqos)->enable_state = WBT_STATE_ON_DEFAULT; /* [한국어] 장치 큐 스로틀링 기본 활성화 */
		mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */
		return false; /* [한국어] 추가 설치 불필요 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */

	/* Queue not registered? Maybe shutting down... */
	if (!blk_queue_registered(q)) /* [한국어] 디스크 등록 전이거나 해제 중이면 설치 불가 */
		return false;

	if (queue_is_mq(q) && enable) /* [한국어] blk-mq 큐이고 WBT 활성화 조건 충족: 신규 설치 진행 */
		return true;
	return false; /* [한국어] non-mq 또는 WBT off: 설치 안 함 */
}

/*
 * [한국어]
 * wbt_enable_default - 디스크에 WBT 기본 활성화 검사 (외부 호출용 래퍼)
 *
 * @disk: gendisk
 *
 * __wbt_enable_default를 호출하여 WBT 활성화 조건만 확인한다.
 * 블록 드라이버나 다른 블록 드라이버가 EXPORT_SYMBOL_GPL을 통해 호출.
 *
 * 호출 체인:
 *   블록 드라이버 / blk 코어 → [wbt_enable_default]
 *                               → __wbt_enable_default
 */
void wbt_enable_default(struct gendisk *disk)
{
	__wbt_enable_default(disk); /* [한국어] WBT 기본 활성화 조건 검사 (반환값 무시) */
}
EXPORT_SYMBOL_GPL(wbt_enable_default); /* [한국어] 블록 드라이버 등에서 WBT 기본 활성화 심볼 노출 */

/*
 * [한국어]
 * wbt_init_enable_default - WBT 설치 조건 충족 시 rq_wb 할당·초기화·debugfs 등록
 *
 * @disk: gendisk (디스크)
 *
 * add_disk 경로에서 디스크가 등록될 때 호출된다. __wbt_enable_default로
 * 조건을 확인하고, 충족되면 wbt_alloc → wbt_init으로 rq_wb를 할당하고
 * request_queue의 rq_qos 리스트에 등록한다.
 *
 * 실행 컨텍스트: add_disk 경로 (장치 초기화 완료 후)
 *
 * 호출 체인:
 *   add_disk → [wbt_init_enable_default]
 *               → __wbt_enable_default
 *               → wbt_alloc
 *               → wbt_init
 */
void wbt_init_enable_default(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] 디스크 request_queue */
	struct rq_wb *rwb; /* [한국어] 새로 할당할 WBT 상태 — 등록에 실패하면 여기서 직접 해제해야 한다 */
	unsigned int memflags; /* [한국어] debugfs lock 반환값 저장 */

	if (!__wbt_enable_default(disk)) /* [한국어] WBT 설치 조건 미충족: 설치 생략 */
		return;

	rwb = wbt_alloc(); /* [한국어] 장치 큐 WBT 상태 객체 할당 */
	if (!rwb) /* [한국어] 메모리 부족: WBT 설치 실패 */
		return;

	if (wbt_init(disk, rwb)) { /* [한국어] rq_qos 리스트 등록 및 rq_depth / win_nsec 초기화 */
		pr_warn("%s: failed to enable wbt\n", disk->disk_name); /* [한국어] 디스크 WBT 활성화 실패 경고 */
		wbt_free(rwb); /* [한국어] 할당된 rq_wb 해제 */
		return; /* WBT 미설치, 제출 제한 없음 */
	}

	memflags = blk_debugfs_lock(q); /* [한국어] debugfs 등록 동기화 */
	blk_mq_debugfs_register_rq_qos(q); /* [한국어] 장치 큐 WBT 상태 debugfs 노드 등록 */
	blk_debugfs_unlock(q, memflags); /* [한국어] debugfs lock 해제 */
}

/*
 * [한국어]
 * wbt_default_latency_nsec - 회전식/비회전식 디스크의 기본 WBT 목표 latency 반환
 *
 * @q: request_queue
 * @return: 비회전식 장치(SSD 등) 2,000,000 ns (2ms), 회전식(HDD) 75,000,000 ns (75ms)
 *
 * BLK_FEAT_ROTATIONAL 여부가 이 파일에서 유일하게 장치 특성을 보는 지점이다.
 * 비회전식은 2ms를 기본 목표로 삼아, 큐가 쓰기로 막히면 빠르게 scale_down이
 * 발동되게 한다. 회전식은 탐색 지연만으로도 수 ms가 걸리므로 2ms를 목표로
 * 두면 항상 위반 상태가 되어 버려, 75ms라는 훨씬 느슨한 값을 쓴다.
 *
 * 다만 이 값은 SATA SSD 시절에 정해진 것이라, 지연이 수십~수백 마이크로초인
 * 최신 NVMe에서는 2ms 목표가 사실상 도달하지 않아 WBT가 거의 개입하지 않게
 * 된다. 그런 장치에서 WBT를 실제로 쓰려면 sysfs의 wbt_lat_usec으로 목표를
 * 직접 낮춰야 한다.
 *
 * 호출 체인:
 *   wbt_init / wbt_set_lat → [wbt_default_latency_nsec]
 */
static u64 wbt_default_latency_nsec(struct request_queue *q)
{
	/*
	 * We default to 2msec for non-rotational storage, and 75msec
	 * for rotational storage.
	 */
	if (blk_queue_rot(q)) /* [한국어] 회전식 디스크(HDD): 비회전식 장치는 false */
		return 75000000ULL; /* [한국어] HDD 기본 목표 latency 75ms */
	return 2000000ULL; /* [한국어] 비회전식 장치(SSD 등) 기본 목표 latency 2ms: 큐 포화를 빠르게 감지 */
}

/*
 * [한국어]
 * wbt_queue_depth_changed - 하드웨어 큐 깊이 변경 시 WBT 한도 재계산
 *
 * @rqos: WBT rq_qos
 *
 * blk_mq_update_nr_hw_queues 등으로 장치의 큐 깊이가 변경되면
 * rq_depth.queue_depth를 새 값으로 동기화하고 wbt_update_limits로
 * wb_normal / wb_background를 재계산한다.
 *
 * 호출 체인:
 *   rq_qos_queue_depth_changed → [wbt_queue_depth_changed]
 *                                  → wbt_update_limits
 */
static void wbt_queue_depth_changed(struct rq_qos *rqos)
{
	RQWB(rqos)->rq_depth.queue_depth = blk_queue_depth(rqos->disk->queue); /* [한국어] 하드웨어 queue depth 변경 시 rq_depth 동기화 */
	wbt_update_limits(RQWB(rqos)); /* [한국어] 변경된 큐 깊이에 맞춰 요청 발행 한도 재계산 */
}

/*
 * [한국어]
 * wbt_exit - WBT rq_qos 계층 제거 및 메모리 해제
 *
 * @rqos: WBT rq_qos
 *
 * rq_qos_exit 훅으로 디스크가 제거되거나 WBT가 비활성화될 때 호출된다.
 * blk_stat_remove_callback으로 latency 샘플링을 중단하고 wbt_free로 메모리를
 * 반환한다.
 *
 * 호출 체인:
 *   rq_qos_exit → [wbt_exit]
 *                  → blk_stat_remove_callback
 *                  → wbt_free
 */
static void wbt_exit(struct rq_qos *rqos)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 제거할 장치 큐 WBT 상태 복원 */

	blk_stat_remove_callback(rqos->disk->queue, rwb->cb); /* [한국어] 완료 지연 샘플링 콜백 제거 */
	wbt_free(rwb); /* [한국어] rq_wb 및 blk-stat 콜백 메모리 해제 */
}

/*
 * [한국어]
 * wbt_disable_default - 기본 활성화된 WBT를 비활성화
 *
 * @disk: gendisk
 *
 * WBT_STATE_ON_DEFAULT 상태에서만 blk_stat_deactivate로 latency 샘플링을
 * 중단하고 WBT_STATE_OFF_DEFAULT로 전환한다. 수동 설정(ON/OFF_MANUAL)은
 * 건드리지 않는다.
 *
 * 호출 체인:
 *   sysfs write / 디스크 해제 경로 → [wbt_disable_default]
 */
void wbt_disable_default(struct gendisk *disk)
{
	struct rq_qos *rqos = wbt_rq_qos(disk->queue); /* [한국어] WBT rq_qos 핸들 검색 */
	struct rq_wb *rwb; /* [한국어] rqos NULL 검사를 통과한 뒤에야 채운다 */
	if (!rqos) /* [한국어] WBT 미설치: 변경 없음 */
		return;
	mutex_lock(&disk->rqos_state_mutex); /* [한국어] rq_qos 상태 보호: 동시 enable/disable race 방지 */
	rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */
	if (rwb->enable_state == WBT_STATE_ON_DEFAULT) { /* [한국어] 기본 on 상태일 때만 off로 전환: 수동 설정 보존 */
		blk_stat_deactivate(rwb->cb); /* [한국어] 완료 지연 샘플링 중단 */
		rwb->enable_state = WBT_STATE_OFF_DEFAULT; /* [한국어] WBT 기본 off: 제출 제한 해제 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */
}
EXPORT_SYMBOL_GPL(wbt_disable_default); /* [한국어] WBT 비활성화 심볼 노출: 블록 드라이버 등에서 사용 */

#ifdef CONFIG_BLK_DEBUG_FS
/* [한국어] debugfs 항목들: 장치 큐 스로틀링 상태를 /sys/kernel/debug/block/<dev>/wbt에 노출 */

/*
 * [한국어]
 * wbt_curr_win_nsec_show - debugfs: 현재 blk-stat 샘플링 윈도우 크기 출력
 */
static int wbt_curr_win_nsec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] debugfs data: WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%llu\n", rwb->cur_win_nsec); /* [한국어] 현재 완료 지연 모니터링 윈도우(ns) 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_enabled_show - debugfs: WBT 활성화 상태 출력 (WBT_STATE_* 값)
 */
static int wbt_enabled_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%d\n", rwb->enable_state); /* [한국어] WBT on/off 상태 (WBT_STATE_ON/OFF_DEFAULT/MANUAL) 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_id_show - debugfs: rq_qos ID 출력 (디스크 내 QoS 식별자)
 */
static int wbt_id_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */

	seq_printf(m, "%u\n", rqos->id); /* [한국어] rq_qos ID: 디스크 내 WBT QoS 식별자 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_inflight_show - debugfs: 각 요청 그룹의 현재 in-flight 수 출력
 */
static int wbt_inflight_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */
	int i; /* [한국어] rq_wait 그룹 인덱스 (0=BG, 1=SWAP, 2=DISCARD — enum 순서와 동일) */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹 순회 */
		seq_printf(m, "%d: inflight %d\n", i, /* [한국어] 그룹별 장치 큐 in-flight 요청 개수 출력 */
			   atomic_read(&rwb->rq_wait[i].inflight)); /* [한국어] 락 없이 원자적으로만 읽는다 — 진단용 스냅샷이라
								     * 다른 CPU의 제출/완료와 어긋난 값이 나와도 무방하다 */
	return 0;
}

/*
 * [한국어]
 * wbt_min_lat_nsec_show - debugfs: 현재 WBT 목표 read latency 출력
 */
static int wbt_min_lat_nsec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%lu\n", rwb->min_lat_nsec); /* [한국어] 읽기 요청 목표 완료 latency(ns) 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_unknown_cnt_show - debugfs: 연속 LAT_UNKNOWN 발생 횟수 출력
 */
static int wbt_unknown_cnt_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%u\n", rwb->unknown_cnt); /* [한국어] 연속 latency 샘플 부족(LAT_UNKNOWN) 횟수 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_normal_show - debugfs: 일반 쓰기 요청 발행 상한(wb_normal) 출력
 */
static int wbt_normal_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%u\n", rwb->wb_normal); /* [한국어] 일반 쓰기 장치 큐 요청 발행 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_background_show - debugfs: 배경 쓰기 요청 발행 상한(wb_background) 출력
 */
static int wbt_background_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%u\n", rwb->wb_background); /* [한국어] 배경 쓰기 장치 큐 요청 발행 상한 출력 */
	return 0;
}

/* [한국어] wbt_debugfs_attrs: WBT debugfs 항목 테이블 */
static const struct blk_mq_debugfs_attr wbt_debugfs_attrs[] = {
	{"curr_win_nsec", 0400, wbt_curr_win_nsec_show}, /* [한국어] 현재 blk-stat 윈도우 크기(ns) */
	{"enabled", 0400, wbt_enabled_show},             /* [한국어] WBT 활성화 상태 */
	{"id", 0400, wbt_id_show},                       /* [한국어] rq_qos ID */
	{"inflight", 0400, wbt_inflight_show},           /* [한국어] 그룹별 in-flight 요청 수 */
	{"min_lat_nsec", 0400, wbt_min_lat_nsec_show},   /* [한국어] 목표 read latency(ns) */
	{"unknown_cnt", 0400, wbt_unknown_cnt_show},     /* [한국어] 연속 LAT_UNKNOWN 횟수 */
	{"wb_normal", 0400, wbt_normal_show},            /* [한국어] 일반 쓰기 요청 상한 */
	{"wb_background", 0400, wbt_background_show},   /* [한국어] 배경 쓰기 요청 상한 */
	{}, /* [한국어] 빈 항목 = 배열 끝 표시. blk_mq_debugfs가 개수를 따로 받지 않고 name==NULL로 순회를 멈춘다 */
};
#endif

/*
 * [한국어]
 * wbt_rqos_ops - blk-rq-qos.c가 bio/request 생명주기 단계마다 호출하는 콜백 테이블
 *
 * 장치 큐로 들어가는 bio/request가 submit/issue/done/requeue 단계를 거칠 때
 * WBT가 개입할 훅 함수들을 정의한다. blk-rq-qos.c가 이 테이블을 통해
 * 각 콜백을 호출한다.
 */
static const struct rq_qos_ops wbt_rqos_ops = {
	.throttle = wbt_wait,                           /* [한국어] bio→request 변환 전: 장치 큐 요청 한도 대기 */
	.issue = wbt_issue,                             /* [한국어] 장치 발행 직전: sync 읽기 요청 발행 시각 기록 */
	.track = wbt_track,                             /* [한국어] request에 요청 그룹 기록: issue/done 회계 연결 */
	.requeue = wbt_requeue,                         /* [한국어] 장치 큐 회수 후 재발행: sync_cookie 정리 */
	.done = wbt_done,                               /* [한국어] 요청 완료 후: inflight 감소 및 WBT 상태 정리 */
	.cleanup = wbt_cleanup,                         /* [한국어] bio 장치 큐 진입 실패 시 회계 롤백 */
	.queue_depth_changed = wbt_queue_depth_changed, /* [한국어] 하드웨어 큐 깊이 변경 시 요청 한도 재계산 */
	.exit = wbt_exit,                               /* [한국어] 디스크 제거 시 WBT rq_qos 해제 */
#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = wbt_debugfs_attrs,             /* [한국어] 장치 큐 스로틀링 상태 debugfs 노출 */
#endif
};

/*
 * [한국어]
 * wbt_init - rq_wb 초기화 및 request_queue의 rq_qos 리스트에 WBT 등록
 *
 * @disk: 대상 gendisk
 * @rwb: 이미 할당된 rq_wb (wbt_alloc으로 생성됨)
 * @return: 0 성공, 음수 오류
 *
 * 디스크에 WBT 계층을 설치한다. rq_wait 3개를 초기화하고, 기본값
 * (100ms 윈도우, 16 depth, 2ms latency)을 설정한 뒤 rq_qos_add로
 * blk-rq-qos 리스트에 등록하고 blk_stat_add_callback으로 요청 완료
 * latency 샘플링을 시작한다.
 *
 * 실행 컨텍스트: add_disk 경로 또는 sysfs write (wbt_init_enable_default)
 *
 * 호출 체인:
 *   wbt_init_enable_default / wbt_set_lat → [wbt_init]
 *                                            → rq_qos_add
 *                                            → blk_stat_add_callback
 */
static int wbt_init(struct gendisk *disk, struct rq_wb *rwb)
{
	struct request_queue *q = disk->queue; /* [한국어] 디스크의 request_queue */
	int ret; /* [한국어] rq_qos_add 등록 결과 */
	int i; /* [한국어] 요청 그룹 순회 인덱스 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹별 rq_wait 초기화 */
		rq_wait_init(&rwb->rq_wait[i]); /* [한국어] atomic inflight = 0, waitqueue 초기화 */

	rwb->last_comp = rwb->last_issue = jiffies; /* [한국어] 읽기 요청 활동 기준 시각 초기화: close_io 판단 기준 */
	rwb->win_nsec = RWB_WINDOW_NSEC; /* [한국어] 완료 지연 샘플링 기본 100ms 윈도우 */
	rwb->enable_state = WBT_STATE_ON_DEFAULT; /* [한국어] WBT 기본 활성 상태: 장치 큐 스로틀링 시작 */
	rwb->rq_depth.default_depth = RWB_DEF_DEPTH; /* [한국어] 초기 장치 큐 소프트웨어 depth 16 */
	rwb->min_lat_nsec = wbt_default_latency_nsec(q); /* [한국어] 비회전식 장치(SSD 등)면 2ms, HDD면 75ms 목표 latency */
	rwb->rq_depth.queue_depth = blk_queue_depth(q); /* [한국어] 하드웨어 queue depth 초기 동기화 */
	wbt_update_limits(rwb); /* [한국어] 요청 발행 한도 초기화 (wb_normal / wb_background) */

	/*
	 * Assign rwb and add the stats callback.
	 */
	mutex_lock(&q->rq_qos_mutex); /* [한국어] rq_qos 리스트 보호: 동시 등록 race 방지 */
	ret = rq_qos_add(&rwb->rqos, disk, RQ_QOS_WBT, &wbt_rqos_ops); /* [한국어] blk-rq-qos에 WBT 등록: bio submit 경로에 훅 연결 */
	mutex_unlock(&q->rq_qos_mutex); /* [한국어] 리스트 보호 해제 */
	if (ret) /* [한국어] 등록 실패: 이미 WBT가 있거나 오류 */
		return ret;

	blk_stat_add_callback(q, rwb->cb); /* [한국어] 요청 완료 지연 샘플링 콜백 활성화 */
	return 0; /* [한국어] WBT 설치 성공 */
}

/*
 * [한국어]
 * wbt_set_lat - 사용자 지정 latency 값으로 WBT 목표 재설정
 *
 * @disk: gendisk (디스크)
 * @val: 새 목표 latency (ns), -1이면 기본값으로 재설정
 * @return: 0 성공, 음수 오류
 *
 * sysfs write_latency_store 등에서 호출된다. WBT가 없으면 wbt_alloc으로
 * 신규 할당하고, val == -1이면 wbt_default_latency_nsec 기본값을 사용한다.
 * blk_mq_freeze_queue로 큐를 멈춘 뒤 wbt_init 또는 wbt_set_min_lat로
 * 새 목표를 적용한다.
 *
 * 호출 체인:
 *   sysfs write → [wbt_set_lat]
 *                  → wbt_alloc (필요 시)
 *                  → blk_mq_freeze_queue
 *                  → wbt_init / wbt_set_min_lat
 *                  → blk_mq_unfreeze_queue
 */
int wbt_set_lat(struct gendisk *disk, s64 val)
{
	struct request_queue *q = disk->queue; /* [한국어] 디스크 request_queue */
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] 기존 WBT rq_qos 핸들 */
	struct rq_wb *rwb = NULL; /* [한국어] 신규 rq_wb (WBT 미설치 시 할당) */
	unsigned int memflags; /* [한국어] blk_mq_freeze / debugfs lock 반환값 저장 */
	int ret = 0; /* [한국어] 결과 */

	if (!rqos) { /* [한국어] WBT 미설치: 신규 할당 필요 */
		rwb = wbt_alloc(); /* [한국어] 장치 큐 WBT 상태 신규 할당 */
		if (!rwb) /* [한국어] 메모리 부족 */
			return -ENOMEM; /* [한국어] 큐를 얼리기 전에 실패시킨다 — freeze 이후에 실패하면 unfreeze 정리 경로가 늘어난다 */
	}

	/*
	 * Ensure that the queue is idled, in case the latency update
	 * ends up either enabling or disabling wbt completely. We can't
	 * have IO inflight if that happens.
	 */
	memflags = blk_mq_freeze_queue(q); /* [한국어] 큐 동결: WBT on/off 전환 중 in-flight IO가 없어야 함 */
	if (!rqos) { /* [한국어] WBT 미설치: wbt_init으로 초기화 */
		ret = wbt_init(disk, rwb); /* [한국어] 장치 큐 rq_qos 등록 및 blk-stat 콜백 활성화 */
		if (ret) { /* [한국어] 등록 실패 */
			wbt_free(rwb); /* [한국어] 신규 할당한 rq_wb 해제 */
			goto out; /* [한국어] unfreeze 후 종료 */
		}
	}

	if (val == -1) /* [한국어] -1: 기본 latency로 재설정 (비회전식 장치(SSD 등) 2ms / HDD 75ms) */
		val = wbt_default_latency_nsec(q); /* [한국어] 사용자가 조정한 값을 버리고 장치 회전 특성 기반 기본값으로 되돌린다 */
	else if (val >= 0) /* [한국어] 사용자 지정 값: μs 단위를 ns로 변환 */
		val *= 1000ULL; /* [한국어] μs → ns: blk-stat latency 샘플과 단위 통일 */

	if (wbt_get_min_lat(q) == val) /* [한국어] 장치 큐 목표 latency 변화 없음: 불필요한 quiesce 회피 */
		goto out; /* [한국어] quiesce는 in-flight I/O가 모두 끝날 때까지 기다리므로 값이 같을 때 치르면 순손해다 */

	blk_mq_quiesce_queue(q); /* [한국어] request 처리 일시 정지: latency 변경 중 장치 큐 안전 상태 확보 */

	mutex_lock(&disk->rqos_state_mutex); /* [한국어] WBT 상태 보호: enable_state race 방지 */
	wbt_set_min_lat(q, val); /* [한국어] 새 목표 latency 설정 및 WBT 활성 상태 업데이트 */
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */

	blk_mq_unquiesce_queue(q); /* [한국어] 장치 큐 처리 재개 */
out:
	blk_mq_unfreeze_queue(q, memflags); /* [한국어] 큐 동결 해제: 요청 발행 재개 */

	memflags = blk_debugfs_lock(q); /* [한국어] debugfs 등록 보호 */
	blk_mq_debugfs_register_rq_qos(q); /* [한국어] 변경된 장치 큐 WBT 상태 debugfs 갱신 */
	blk_debugfs_unlock(q, memflags); /* [한국어] debugfs lock 해제 */

	return ret; /* [한국어] WBT latency 설정 결과 */
}

/* [한국어] === 이 파일의 폐루프 요약 === */
/*
 * - 관측: wbt_issue()가 sync 읽기의 발행 시각을 남기고, wbt_done()이
 *   완료 시각을 남긴다. 그와 별개로 blk-stat이 윈도우 단위로 READ/WRITE
 *   완료 지연을 모은다. wbt는 평균이 아니라 윈도우의 '최소' 읽기 지연을
 *   본다 — 한 건이라도 빠르게 끝난 읽기가 있으면 큐는 아직 막히지 않은
 *   것이고, 최솟값마저 목표를 넘으면 예외 없이 막힌 것이기 때문이다.
 * - 판정: latency_exceeded()가 LAT_OK / LAT_UNKNOWN /
 *   LAT_UNKNOWN_WRITES / LAT_EXCEEDED 중 하나를 고른다. 읽기 샘플이
 *   아예 없더라도, 발행해 둔 sync 읽기가 윈도우보다 오래 안 돌아오면
 *   그 자체를 위반으로 친다 — 장치가 완전히 막히면 샘플조차 생기지
 *   않으므로, 샘플 부재를 '조용함'으로 오해하면 안 되기 때문이다.
 * - 대응: wb_timer_fn()이 판정에 따라 scale_up/scale_down을 부른다.
 *   줄일 때는 rq_depth.max_depth만 줄이는 게 아니라 관측 윈도우도 함께
 *   좁혀(rwb_arm_timer) 다음 판정을 더 빨리 내린다.
 * - 집행: calc_wb_limits()가 max_depth에서 wb_normal(절반)과
 *   wb_background(1/4)를 뽑고, wbt_wait() → get_limit()이 bio의 성격에
 *   따라 둘 중 하나를 한도로 골라 rq_wait에서 대기시킨다. 읽기는 어떤
 *   경우에도 이 대기에 걸리지 않는다 — 보호 대상이 읽기이기 때문이다.
 * - 이 파일은 장치 종류나 전송 프로토콜에 대한 지식이 전혀 없다. 유일한
 *   장치 의존 판단은 wbt_default_latency_nsec()의 회전식/비회전식 구분
 *   (75ms vs 2ms)뿐이다. 지연이 매우 낮은 최신 SSD에서는 2ms 기본 목표가
 *   느슨해 WBT가 사실상 개입하지 않을 수 있고, 그래서 드라이버가
 *   blk_queue_disable_wbt()로 아예 끄거나 sysfs의 wbt_lat_usec로 목표를
 *   직접 낮출 수 있게 되어 있다.
 */
