// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어] writeback 스로틀링 구현 (block/blk-wbt.c)
 *
 * === 파일의 역할 ===
 * blk-mq 기반 NVMe SSD에서 buffered writeback이 발생할 때 큐 깊이(queue depth)를
 * 동적으로 조절하여 NVMe SQ(Submission Queue)의 doorbell 폭주와 NAND 쓰기 버퍼
 * 포화를 완화한다. CoDel 알고리즘에서 영감을 받은 latency 기반 피드백 루프로
 * rq_depth.max_depth를 1~16 사이에서 2의 거듭제곱 단위로 스텝 조절한다.
 * bio가 request로 변환되기 전(blk_mq_get_request) 단계에서 wbt_wait으로 개입해
 * 동시 in-flight 쓰기 request 수를 제한하고, NVMe CQ 완료 latency를 blk-stat으로
 * 수집해 다음 샘플링 윈도우에서 큐 깊이를 재조정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * rq-qos(request 품질 제어) 계층의 writeback throttling 구현체.
 * 실행 흐름:
 *   VFS write → page cache dirty → blk_mq_submit_bio
 *     → __rq_qos_throttle (blk-rq-qos.c) → wbt_wait (이 파일)
 *     → [대기: rq_wait inflight 카운터 점검] → blk_mq_get_request
 *     → nvme_queue_rq → nvme_submit_cmd(doorbell)
 *   NVMe CQ 완료 → wbt_done (이 파일) → blk_stat 샘플 기록
 *   blk_stat 콜백 → wbt_cb (이 파일) → scale_up/down
 * 실행 컨텍스트: wbt_wait은 submit 경로(process context), wbt_done은 CQ 인터럽트
 * 또는 polling 경로에서 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-rq-qos.c: rq_qos_ops 등록, __rq_qos_throttle 훅 체인
 *   - block/blk-stat.c: blk_stat_callback으로 NVMe CQ latency 수집
 *   - block/blk.h: rq_wait(inflight 카운터+대기큐), rq_depth(동적 QD 상태)
 *   - include/linux/backing-dev.h: blk_issue_stat_local (sync I/O 판별)
 * 이 모듈에 의존하는 모듈:
 *   - block/blk-mq.c: wbt_wait/wbt_issue/wbt_done/wbt_track을 rq-qos 훅으로 호출
 *   - block/genhd.c: wbt_init (디스크 초기화 시 WBT 등록)
 * 핵심 자료구조: struct rq_wb (per-device WBT 상태)
 *
 * === 주요 함수/구조체 요약 ===
 * wbt_wait()           : bio 제출 경로에서 in-flight 초과 시 대기 진입
 * wbt_done()           : request 완료 후 inflight 감소 + 대기자 wake
 * wbt_cb()             : blk-stat 콜백 — NVMe latency 측정 후 scale 결정
 * scale_up/down()      : rq_depth.max_depth를 2배/0.5배로 조절
 * wbt_init()           : per-disk WBT 자료구조 할당·초기화, rq-qos 등록
 * struct rq_wb:
 *   rq_wait[WBT_NUM_RWQ] : BG/SWAP/DISCARD 그룹별 inflight 카운터 + waitq
 *   rq_depth             : 동적 queue depth 상태 (max_depth, scaling_step)
 *   win_nsec/cur_win_nsec: 기본/현재 latency 샘플링 윈도우
 *   min_lat_nsec         : 목표 latency 임계값
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

#include "blk-stat.h"			/* [한국어] blk_stat_callback: NVMe CQ 완료 latency 수집 콜백 */
#include "blk-wbt.h"			/* [한국어] struct rq_wb, wbt_init/exit 프로토타입 */
#include "blk-rq-qos.h"			/* [한국어] rq_qos_ops, rq_wait, rq_depth: bio/request QoS 훅 인프라 */
#include "elevator.h"			/* [한국어] elevator_registered(): IO 스케줄러 여부 확인 */
#include "blk.h"			/* [한국어] 블록 계층 내부 헤더 */

#define CREATE_TRACE_POINTS		/* [한국어] wbt tracepoint 정의 매크로: NVMe SQ doorbell 제어 이벤트 추적 */
#include <trace/events/wbt.h>		/* [한국어] trace_wbt_stat/scale/lat 등 WBT 이벤트 tracepoint */

/*
 * [한국어] WBT가 request를 분류하는 플래그 — NVMe SQ 발행 특성에 따라 구분
 */
enum wbt_flags {
	WBT_TRACKED		= 1,	/* [한국어] NVMe SQ로 발행할 buffered write/discard — inflight 어카운팅 대상 */
	WBT_READ		= 2,	/* [한국어] NVMe read CID: 완료 latency를 샘플로 사용해 scale 결정 */
	WBT_SWAP		= 4,	/* [한국어] kswapd/swap_writeout 경로의 긴급 쓰기: BG와 별도 그룹 */
	WBT_DISCARD		= 8,	/* [한국어] NVMe Deallocate/DSM (Trim/Discard 명령) 요청 */

	WBT_NR_BITS		= 4,	/* [한국어] 위 플래그 비트 수: rq->wbt_flags 필드 크기 결정에 사용 */
};

/*
 * [한국어] WBT가 관리하는 inflight 대기열 그룹 인덱스
 * BG/SWAP/DISCARD를 별도 rq_wait로 관리해 그룹별 NVMe SQ CID 수를 독립 조절
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
	WBT_STATE_OFF_DEFAULT	= 3,	/* [한국어] 기본 비활성 상태: NVMe 장치 특성으로 비활성화 */
	WBT_STATE_OFF_MANUAL	= 4,	/* [한국어] 수동 비활성 상태: sysfs/ioctl로 명시적으로 끔 */
};

/*
 * [한국어]
 * struct rq_wb - NVMe SQ에 대한 writeback 스로틀링 상태를 담는 per-device 자료구조
 *
 * blk-rq-qos 계층의 rq_qos를 내포하며, NVMe SQ에 동시에 발행할 수 있는
 * 쓰기/discard CID 수와 완료 latency 목표를 관리한다.
 * per-device(per-disk)로 하나씩 존재하며 wbt_init에서 할당된다.
 */
struct rq_wb {
	/*
	 * Settings that govern how we throttle
	 */
	unsigned int wb_background;
	/* [한국어] 배경 쓰기 허용 최대 in-flight 요청 수 (CID 슬롯 기준).
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
	/* [한국어] blk-stat NVMe CQ 완료 latency 수집 콜백.
	 * 설정자: wbt_init()에서 blk_stat_alloc_callback(wbt_cb)으로 할당.
	 * 읽는 자: blk_stat_activate/deactivate에서 타이머 등록/해제.
	 * 동기화: blk-stat 내부 락으로 보호. */

	u64 sync_issue;
	/* [한국어] 마지막 sync read CID 발행 시각 (ktime_get_ns() 기준).
	 * 설정자: wbt_issue()에서 REQ_SYNC read 발행 시 기록.
	 * 읽는 자: wbt_done()에서 sync read 완료 latency 측정용.
	 * 동기화: 단일 sync I/O 추적이므로 sync_cookie와 함께 비교해 유효성 확인. */

	void *sync_cookie;
	/* [한국어] 마지막 sync read request 포인터 (역참조 금지, 식별자 용도만).
	 * 설정자: wbt_issue()에서 rq 포인터를 저장.
	 * 읽는 자: wbt_done()에서 rq == sync_cookie 비교로 동일 요청임을 확인.
	 * 동기화: 단일 I/O 컨텍스트에서만 동작하므로 별도 락 불필요. */

	unsigned long last_issue;
	/* [한국어] 마지막 read CID 발행 시각 (jiffies).
	 * 설정자: wbt_issue()에서 read 발행 시 기록.
	 * 읽는 자: close_io()에서 100ms 이내 read 경쟁 여부 판단.
	 * 동기화: 단일 스레드 접근 가정, 별도 락 없음. */

	unsigned long last_comp;
	/* [한국어] 마지막 read CID CQ 완료 시각 (jiffies).
	 * 설정자: wbt_done()에서 read 완료 시 기록.
	 * 읽는 자: close_io()에서 100ms 이내 recent read 완료 여부 판단.
	 * 동기화: 단일 스레드 접근 가정. */

	unsigned long min_lat_nsec;
	/* [한국어] NVMe read CQ 완료 목표 latency 임계값 (ns).
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
	/* [한국어] NVMe SQ에 대응하는 소프트웨어 dynamic queue depth 상태.
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
	return container_of(rqos, struct rq_wb, rqos); /* [한국어] blk-rq-qos 리스트 노드에서 rq_wb 복원: NVMe SQ QoS 상태 접근 */
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
	return rq->wbt_flags & WBT_TRACKED; /* [한국어] WBT_TRACKED: NVMe SQ inflight 어카운팅 대상 쓰기/trim 요청 */
}

/*
 * [한국어]
 * wbt_is_read - request가 NVMe read CID인지 확인
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
	return rq->wbt_flags & WBT_READ; /* [한국어] WBT_READ: NVMe read CID — latency 샘플 수집 대상 */
}

/*
 * [한국어] WBT queue depth 기본값 및 샘플링 정책 상수
 * NVMe SSD SQ 물리 깊이(보통 64~1024)와 달리, WBT는
 * rq_depth.max_depth를 1~RWB_DEF_DEPTH 사이에서 조절한다.
 */
enum {
	/*
	 * Default setting, we'll scale up (to 75% of QD max) or down (min 1)
	 * from here depending on device stats
	 */
	RWB_DEF_DEPTH	= 16, /* [한국어] 기본 NVMe SQ 소프트웨어 깊이: scaling_step==0일 때 초기값 */

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
 * @return: true이면 WBT 활성 (NVMe SQ 스로틀링 적용)
 *
 * NULL 체크와 enable_state 확인을 합친다. DEFAULT/MANUAL 모두 OFF이면
 * WBT가 완전히 비활성화되어 NVMe SQ에 대한 소프트웨어 제한이 없다.
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
 * 때만 갱신해 중복 기록을 피한다. NVMe read CID의 발행/완료 시각을
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
 * 이미 쓰기를 제한했으므로 NVMe SQ wbt_rqw_done에서 wake 임계값을 낮춰
 * SQ를 더 적극적으로 활용할 수 있게 한다.
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
 * SWAP/DISCARD/BG 세 그룹에 각각 독립적인 NVMe SQ 발행 한도를 적용하기
 * 위해 플래그 기반으로 대응하는 rq_wait를 선택한다.
 *
 * 호출 체인:
 *   __wbt_wait / __wbt_done → [get_rq_wait]
 */
static inline struct rq_wait *get_rq_wait(struct rq_wb *rwb,
					  enum wbt_flags wb_acct)
{
	if (wb_acct & WBT_SWAP) /* [한국어] kswapd 쓰기: SWAP 그룹 — BG와 독립적으로 NVMe SQ 한도 관리 */
		return &rwb->rq_wait[WBT_RWQ_SWAP];
	else if (wb_acct & WBT_DISCARD) /* [한국어] NVMe Deallocate/Trim: DISCARD 그룹 — 배경 한도로만 제한 */
		return &rwb->rq_wait[WBT_RWQ_DISCARD];

	return &rwb->rq_wait[WBT_RWQ_BG]; /* [한국어] 일반 background writeback: BG 그룹 */
}

/*
 * [한국어]
 * rwb_wake_all - 모든 WBT 대기 그룹의 대기 태스크를 깨운다
 *
 * @rwb: WBT 상태
 *
 * scale_up이나 wb_limit 갱신으로 NVMe SQ 허용 깊이가 늘었을 때
 * BG/SWAP/DISCARD 세 그룹 모두에서 sleep 중인 submit 경로를 깨운다.
 * 깨어난 태스크는 wbt_wait의 rq_wait_inc_below에서 다시 한도를 확인한다.
 *
 * 호출 체인:
 *   scale_up / wbt_update_limits → [rwb_wake_all] → wake_up_all
 */
static void rwb_wake_all(struct rq_wb *rwb)
{
	int i; /* [한국어] 그룹 인덱스: WBT_RWQ_BG(0), WBT_RWQ_SWAP(1), WBT_RWQ_DISCARD(2) */

	for (i = 0; i < WBT_NUM_RWQ; i++) { /* [한국어] 세 그룹 순회: 모든 그룹의 NVMe SQ 대기자 깨움 */
		struct rq_wait *rqw = &rwb->rq_wait[i]; /* [한국어] i번째 그룹의 inflight 카운터 + 대기 큐 */

		if (wq_has_sleeper(&rqw->wait)) /* [한국어] 해당 그룹에 sleep 중인 submit 경로 존재 시에만 wake */
			wake_up_all(&rqw->wait); /* [한국어] NVMe SQ 여유 신호: sleep 중인 wbt_wait 경로 일제 깨움 */
	}
}

/*
 * [한국어]
 * wbt_rqw_done - NVMe CQ 완료 후 rq_wait 그룹의 inflight 감소 + 대기 태스크 wake
 *
 * @rwb: WBT 상태
 * @rqw: 완료된 request가 속한 rq_wait 그룹 (BG/SWAP/DISCARD)
 * @wb_acct: 완료된 request의 WBT 플래그
 *
 * atomic_dec_return으로 inflight를 원자적으로 감소시킨다. 감소 후 inflight가
 * 한도 이하로 낮아지면 대기 중인 submit 경로를 깨운다. Wake 임계값은 DISCARD는
 * wb_background, write cache 있고 dirty 대기 없으면 0(즉시 깨우지 않음),
 * 나머지는 wb_normal로 결정한다.
 * NVMe CQ 처리 후 blk_mq_end_request → rq_qos_done → wbt_done 경로에서 호출.
 *
 * 호출 체인:
 *   __wbt_done / wbt_cleanup_cb → [wbt_rqw_done] → wake_up_all
 */
static void wbt_rqw_done(struct rq_wb *rwb, struct rq_wait *rqw,
			 enum wbt_flags wb_acct)
{
	int inflight, limit; /* [한국어] inflight: 감소 후 NVMe SQ in-flight 수, limit: wake 임계값 */

	inflight = atomic_dec_return(&rqw->inflight); /* [한국어] 원자적 inflight 감소: CQ 완료로 CID 슬롯 회수 */

	/*
	 * For discards, our limit is always the background. For writes, if
	 * the device does write back caching, drop further down before we
	 * wake people up.
	 */
	if (wb_acct & WBT_DISCARD)
		/* [한국어] DISCARD: 항상 wb_background 한도 사용 (더 엄격한 제한) */
		limit = rwb->wb_background;
	else if (blk_queue_write_cache(rwb->rqos.disk->queue) &&
		 !wb_recent_wait(rwb))
		/* [한국어] write cache 있고 dirty throttle 이력 없음: 완료해도 바로 깨우지 않음 (NVMe write cache 활용 여지) */
		limit = 0;
	else
		limit = rwb->wb_normal; /* [한국어] 일반 쓰기/swap: wb_normal 한도에서 wake */

	/*
	 * Don't wake anyone up if we are above the normal limit.
	 */
	if (inflight && inflight >= limit) /* [한국어] inflight가 여전히 한도 이상이면 wake 불필요: SQ가 여전히 혼잡 */
		return;

	if (wq_has_sleeper(&rqw->wait)) {
		int diff = limit - inflight; /* [한국어] NVMe SQ에 추가 발행 가능한 CID 여유량 */

		if (!inflight || diff >= rwb->wb_background / 2) /* [한국어] SQ가 비었거나 여유가 wb_background/2 이상이면 일제 wake */
			wake_up_all(&rqw->wait); /* [한국어] sleep 중인 wbt_wait submit 경로 깨움: nvme_submit_cmd(doorbell) 재개 */
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
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] rq_qos → rq_wb 역참조: NVMe SQ QoS 상태 접근 */
	struct rq_wait *rqw; /* [한국어] 완료된 CID가 속한 그룹의 rq_wait */

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
 * NVMe CQ 처리 후 blk_mq_end_request → rq_qos_done → wbt_done 경로에서 호출.
 * WBT_TRACKED이면 inflight 감소(→ __wbt_done), WBT_READ이면 완료 시각 기록.
 * sync_cookie가 이 rq이면 sync_issue/sync_cookie를 초기화한다.
 *
 * 호출 체인:
 *   nvme_process_cq → blk_mq_complete_request → rq_qos_done
 *     → [wbt_done] → __wbt_done → wbt_rqw_done
 */
static void wbt_done(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] rq_qos → rq_wb: NVMe SQ QoS 상태 접근 */

	if (!wbt_is_tracked(rq)) {
		/* [한국어] WBT_TRACKED가 없으면 read CID 완료 경로 */
		if (wbt_is_read(rq)) { /* [한국어] NVMe read CID 완료: latency 샘플 수집, sync 추적 */
			if (rwb->sync_cookie == rq) { /* [한국어] 추적 중인 sync read CID가 완료됨 */
				rwb->sync_issue = 0; /* [한국어] sync read 발행 시각 추적 해제 */
				rwb->sync_cookie = NULL; /* [한국어] sync_cookie 해제: 다음 sync read를 위해 초기화 */
			}

			wb_timestamp(rwb, &rwb->last_comp); /* [한국어] read CID CQ 완료 시각 기록: close_io에서 최근 read 여부 판단 */
		}
	} else {
		WARN_ON_ONCE(rq == rwb->sync_cookie); /* [한국어] tracked write가 sync_cookie이면 버그: write는 sync_cookie로 등록되지 않음 */
		__wbt_done(rqos, wbt_flags(rq)); /* [한국어] tracked CID inflight 감소: CQ 완료에 따른 SQ 슬롯 회수 */
	}
	wbt_clear_state(rq); /* [한국어] request 재사용 전 WBT 플래그 초기화: wbt_flags를 0으로 */
}

/*
 * [한국어]
 * stat_sample_valid - blk-stat 샘플이 scale 결정에 충분한지 확인
 *
 * @stat: blk_rq_stat 배열 (READ/WRITE 인덱스)
 * @return: true이면 샘플 유효 (NVMe SQ 부하 판단 가능)
 *
 * NVMe read CID 1개 이상 + write CID 3개(RWB_MIN_WRITE_SAMPLES) 이상이
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
	return (stat[READ].nr_samples >= 1 && /* [한국어] NVMe read CID 완료 샘플 1개 이상: 저전력 idle과 구분하기 위해 */
		stat[WRITE].nr_samples >= RWB_MIN_WRITE_SAMPLES); /* [한국어] NVMe write CID 샘플 3개 이상: 쓰기 부하가 실제로 있음을 확인 */
}

/*
 * [한국어]
 * rwb_sync_issue_lat - sync read CID가 NVMe SQ에 발행된 후 경과 시간 반환
 *
 * @rwb: WBT 상태
 * @return: sync read CID SQ 체류 시간 (ns), CID가 없으면 0
 *
 * blk-stat 윈도우에 완료 이벤트가 없더라도 sync_issue/sync_cookie를 통해
 * 진행 중인 sync read CID의 발행~현재 경과 시간을 추적한다. 이 시간이
 * 윈도우보다 크면 CQ 완료 없이도 latency 위반으로 판정한다.
 *
 * 호출 체인:
 *   latency_exceeded → [rwb_sync_issue_lat]
 */
static u64 rwb_sync_issue_lat(struct rq_wb *rwb)
{
	u64 issue = READ_ONCE(rwb->sync_issue); /* [한국어] sync read CID 발행 시각 읽기: READ_ONCE로 컴파일러 재정렬 방지 */

	if (!issue || !rwb->sync_cookie) /* [한국어] sync read CID가 없거나 쿠키가 없으면 체류 시간 없음 */
		return 0; /* [한국어] 체류 시간 0: latency_exceeded에서 위반 없음으로 처리 */

	return blk_time_get_ns() - issue; /* [한국어] 현재 시각 - doorbell 시각 = CID의 NVMe SQ 체류 시간 */
}

/*
 * [한국어]
 * wbt_inflight - 세 WBT 그룹의 현재 in-flight 총합 반환
 *
 * @rwb: WBT 상태
 * @return: BG + SWAP + DISCARD 그룹의 inflight 합계
 *
 * NVMe SQ에 발행되어 아직 CQ 완료를 받지 못한 CID 총수를 소프트웨어에서
 * 추정한다. atomic_read로 각 그룹을 읽으므로 CQ 완료와 race 가능하나
 * 근사값으로 충분하다.
 *
 * 호출 체인:
 *   latency_exceeded / wb_timer_fn → [wbt_inflight]
 */
static inline unsigned int wbt_inflight(struct rq_wb *rwb)
{
	unsigned int i, ret = 0; /* [한국어] 세 그룹의 inflight 합산용 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹 순회 */
		ret += atomic_read(&rwb->rq_wait[i].inflight); /* [한국어] 각 그룹의 atomic inflight 읽기: 합산으로 NVMe SQ 추정 CID 수 계산 */

	return ret; /* [한국어] 전체 NVMe SQ 추정 in-flight CID 수 */
}

/* [한국어] latency_exceeded() 반환값 — NVMe SQ 부하 상태 분류 */
enum {
	LAT_OK = 1,		/* [한국어] NVMe SQ latency가 목표 이하: scale_up 가능 */
	LAT_UNKNOWN,		/* [한국어] 샘플 부족으로 판단 불가: scale 유지 */
	LAT_UNKNOWN_WRITES,	/* [한국어] read 샘플 없이 쓰기만 진행 중: 음수 step으로 boost 가능 */
	LAT_EXCEEDED,		/* [한국어] NVMe SQ latency가 목표 초과: scale_down 필요 */
};

/*
 * [한국어]
 * latency_exceeded - blk-stat 샘플과 sync read 발행 시각으로 NVMe SQ 부하 판정
 *
 * @rwb: WBT 상태
 * @stat: blk-stat이 수집한 READ/WRITE 완료 latency 샘플 배열
 * @return: LAT_OK / LAT_UNKNOWN / LAT_UNKNOWN_WRITES / LAT_EXCEEDED
 *
 * NVMe CQ 완료 데이터(stat) + sync read 발행 이후 경과 시간(rwb_sync_issue_lat)
 * 두 가지 신호로 SQ 포화 여부를 판단한다. 포화 시 LAT_EXCEEDED를 반환하여
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
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* [한국어] NVMe 디스크 BDI 참조: trace 이벤트 식별용 */
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] NVMe SQ 소프트웨어 depth 상태 참조 */
	u64 thislat; /* [한국어] sync read CID의 SQ 체류/완료 지연 (ns) */
	/*
	 * If our stored sync issue exceeds the window size, or it
	 * exceeds our min target AND we haven't logged any entries,
	 * flag the latency as exceeded. wbt works off completion latencies,
	 * but for a flooded device, a single sync IO can take a long time
	 * to complete after being issued. If this time exceeds our
	 * monitoring window AND we didn't see any other completions in that
	 * window, then count that sync IO as a violation of the latency.
	 */
	thislat = rwb_sync_issue_lat(rwb); /* [한국어] sync read CID가 NVMe SQ/CQ에서 머문 시간 측정 */
	if (thislat > rwb->cur_win_nsec || /* [한국어] 모니터링 윈도우 전체를 초과: SQ 포화로 CID 장기 대기 (추정) */
	    (thislat > rwb->min_lat_nsec && !stat[READ].nr_samples)) { /* [한국어] read 샘플 없이 목표 초과: doorbell 폭주로 CQ 응답 없음 의심 */
		trace_wbt_lat(bdi, thislat); /* [한국어] tracepoint: NVMe SQ latency 위반 기록 */
		return LAT_EXCEEDED; /* [한국어] SQ 포화 판정 -> scale_down 유도 */
	}

	/*
	 * No read/write mix, if stat isn't valid
	 */
	if (!stat_sample_valid(stat)) { /* [한국어] NVMe CQ 완료 샘플 불충분: read 1개 + write 3개 미만 */
		/*
		 * If we had writes in this stat window and the window is
		 * current, we're only doing writes. If a task recently
		 * waited or still has writes in flights, consider us doing
		 * just writes as well.
		 */
		if (stat[WRITE].nr_samples || wb_recent_wait(rwb) || /* [한국어] write CQ 완료 존재 또는 태스크가 최근 대기/인플라이트 */
		    wbt_inflight(rwb)) /* [한국어] NVMe SQ에 미완료 CID가 있어 write-only 부하로 판단 */
			return LAT_UNKNOWN_WRITES; /* [한국어] write-only: 음수 step에서 boost, 양수 step에서는 scale_up 억제 */
		return LAT_UNKNOWN; /* [한국어] 샘플 없음: 판단 불가, scale 유지 */
	}

	/*
	 * If the 'min' latency exceeds our target, step down.
	 */
	if (stat[READ].min > rwb->min_lat_nsec) { /* [한국어] read CQ 완료의 최소 latency가 목표 초과: SQ에 read 경쟁 발생 */
		trace_wbt_lat(bdi, stat[READ].min); /* [한국어] tracepoint: read CQ latency 위반 기록 */
		trace_wbt_stat(bdi, stat); /* [한국어] tracepoint: NVMe SQ 전체 통계 기록 */
		return LAT_EXCEEDED; /* [한국어] latency 목표 초과 -> scale_down 유도 */
	}

	if (rqd->scale_step) /* [한국어] scale_step이 0이 아니면 depth 조정 중 */
		trace_wbt_stat(bdi, stat); /* [한국어] tracepoint: scale 조정 중 SQ 통계 기록 */

	return LAT_OK; /* [한국어] latency 양호 -> scale_up 가능 신호 */
}

/*
 * [한국어]
 * rwb_trace_step - WBT scale 조정 단계를 tracepoint로 기록
 *
 * @rwb: WBT 상태
 * @msg: 조정 방향 문자열 ("scale up" / "scale down")
 *
 * scale_up/scale_down이 NVMe SQ depth를 변경할 때마다 현재 scale_step,
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
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* [한국어] NVMe 디스크 BDI 참조: tracepoint 식별용 */
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] NVMe SQ 소프트웨어 depth 상태 */

	trace_wbt_step(bdi, msg, rqd->scale_step, rwb->cur_win_nsec, /* [한국어] tracepoint: scale 방향, 현재 step, 윈도우(ns) 기록 */
			rwb->wb_background, rwb->wb_normal, rqd->max_depth); /* [한국어] BG/Normal CID 한도 및 max_depth 기록 */
}

/*
 * [한국어]
 * calc_wb_limits - rq_depth.max_depth에서 wb_normal/wb_background 한도 계산
 *
 * @rwb: WBT 상태
 *
 * NVMe SQ의 동시 발행 CID 상한(max_depth)이 변경될 때마다 호출된다.
 * 전체 depth를 일반 쓰기(wb_normal)와 배경 쓰기(wb_background)로
 * 나눠 read latency 보호를 위한 write CID 발행 한도를 설정한다.
 *
 * 실행 컨텍스트: blk_stat_callback / wbt_init 컨텍스트
 *
 * 호출 체인:
 *   scale_up / scale_down / wbt_update_limits / wbt_init → [calc_wb_limits]
 */
static void calc_wb_limits(struct rq_wb *rwb)
{
	if (rwb->min_lat_nsec == 0) { /* [한국어] WBT 목표 latency 0: SQ 스로틀링 비활성 */
		rwb->wb_normal = rwb->wb_background = 0; /* [한국어] 한도 0 → get_limit()가 max_depth를 그대로 반환하도록 */
	} else if (rwb->rq_depth.max_depth <= 2) { /* [한국어] NVMe SQ depth가 매우 작을 때 보수적 분배 */
		rwb->wb_normal = rwb->rq_depth.max_depth; /* [한국어] 일반 쓰기에 전체 SQ depth 사용 */
		rwb->wb_background = 1; /* [한국어] 배경 쓰기 최소 1 CID 보장: depth가 매우 작으면 background도 1 */
	} else {
		rwb->wb_normal = (rwb->rq_depth.max_depth + 1) / 2; /* [한국어] 일반 쓰기: SQ depth의 절반 (반올림) */
		rwb->wb_background = (rwb->rq_depth.max_depth + 3) / 4; /* [한국어] 배경 쓰기: SQ depth의 1/4 (반올림), read latency 우선 보호 */
	}
}

/*
 * [한국어]
 * scale_up - NVMe SQ 소프트웨어 depth를 한 단계 증가
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
	if (!rq_depth_scale_up(&rwb->rq_depth)) /* [한국어] NVMe SQ max_depth 증가 실패 (이미 최대): 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background 한도 재계산: depth 증가 반영 */
	rwb->unknown_cnt = 0; /* [한국어] LAT_UNKNOWN 연속 카운터 초기화: scale_up으로 상태 리셋 */
	rwb_wake_all(rwb); /* [한국어] 대기 중인 submit 태스크 깨우기: 새로운 depth 한도로 CID 발행 재개 */
	rwb_trace_step(rwb, tracepoint_string("scale up")); /* [한국어] tracepoint: NVMe SQ depth 증가 기록 */
}

/*
 * [한국어]
 * scale_down - NVMe SQ 소프트웨어 depth를 한 단계 감소
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
	if (!rq_depth_scale_down(&rwb->rq_depth, hard_throttle)) /* [한국어] NVMe SQ max_depth 감소 실패 (최소 1): 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background 한도 재계산: depth 감소 반영 */
	rwb->unknown_cnt = 0; /* [한국어] LAT_UNKNOWN 연속 카운터 초기화: scale_down으로 상태 리셋 */
	rwb_trace_step(rwb, tracepoint_string("scale down")); /* [한국어] tracepoint: NVMe SQ depth 감소 기록 */
}

/*
 * [한국어]
 * rwb_arm_timer - blk-stat 콜백 타이머를 다음 샘플링 윈도우로 재설정
 *
 * @rwb: WBT 상태
 *
 * scale_step이 양수(SQ 혼잡)이면 기본 100ms 윈도우를 역제곱근 축소하여
 * NVMe SQ 포화 상태를 더 빠르게 감지한다. step이 0 이하이면 기본 윈도우를
 * 유지한다. blk_stat_activate_nsecs 호출로 CQ 완료 latency 수집을 재시작.
 *
 * 실행 컨텍스트: blk_stat_callback 소프트웨어 IRQ (hrtimer) 또는 submit 경로
 *
 * 호출 체인:
 *   wbt_wait / wb_timer_fn → [rwb_arm_timer]
 *                             → blk_stat_activate_nsecs
 */
static void rwb_arm_timer(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] NVMe SQ 소프트웨어 depth 상태 참조 */

	if (rqd->scale_step > 0) { /* [한국어] SQ 혼잡 상태: 짧은 윈도우로 빠른 latency 반응 */
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
		rwb->cur_win_nsec = rwb->win_nsec; /* [한국어] SQ 여유 시 기본 100ms 샘플링 윈도우 유지 */
	}

	blk_stat_activate_nsecs(rwb->cb, rwb->cur_win_nsec); /* [한국어] blk-stat 타이머 재설정: cur_win_nsec 후 wb_timer_fn 재호출 */
}

/*
 * [한국어]
 * wb_timer_fn - blk-stat 콜백: NVMe CQ latency 샘플 분석 후 SQ depth 조정
 *
 * @cb: blk_stat_callback (cb->data = rq_wb, cb->stat = READ/WRITE 샘플)
 *
 * blk-stat이 cur_win_nsec 윈도우 동안 수집한 NVMe CQ 완료 latency 샘플을
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
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] NVMe SQ 소프트웨어 depth 상태 */
	unsigned int inflight = wbt_inflight(rwb); /* [한국어] 현재 NVMe SQ 추정 in-flight CID 합계 */
	int status;

	if (!rwb->rqos.disk) /* [한국어] NVMe 컨트롤러 제거 중: disk 포인터 없으면 종료 */
		return;

	status = latency_exceeded(rwb, cb->stat); /* [한국어] NVMe CQ latency 샘플로 SQ 부하 상태 판정 */

	trace_wbt_timer(rwb->rqos.disk->bdi, status, rqd->scale_step, inflight); /* [한국어] tracepoint: SQ 상태/scale_step/inflight 기록 */

	/*
	 * If we exceeded the latency target, step down. If we did not,
	 * step one level up. If we don't know enough to say either exceeded
	 * or ok, then don't do anything.
	 */
	switch (status) {
	case LAT_EXCEEDED:
		scale_down(rwb, true); /* [한국어] NVMe SQ latency 초과: 동시 발행 CID 수 강제 감소 */
		break;
	case LAT_OK:
		scale_up(rwb); /* [한국어] NVMe SQ latency 양호: doorbell 발행 한도 1단계 증가 */
		break;
	case LAT_UNKNOWN_WRITES:
		/*
		 * We don't have a valid read/write sample, but we do have
		 * writes going on. Allow step to go negative, to increase
		 * write performance.
		 */
		scale_up(rwb); /* [한국어] write-only 부하: 음수 step까지 허용해 NVMe SQ 쓰기 throughput 극대화 */
		break;
	case LAT_UNKNOWN:
		if (++rwb->unknown_cnt < RWB_UNKNOWN_BUMP) /* [한국어] RWB_UNKNOWN_BUMP 미만이면 아직 대기 */
			break;
		/*
		 * We get here when previously scaled reduced depth, and we
		 * currently don't have a valid read/write sample. For that
		 * case, slowly return to center state (step == 0).
		 */
		if (rqd->scale_step > 0) /* [한국어] 이전에 SQ depth 축소 상태: 점진적 복원 */
			scale_up(rwb); /* [한국어] depth 1단계 확대 -> doorbell 허용량 증가 */
		else if (rqd->scale_step < 0) /* [한국어] 이전에 write-only boost 상태: 점진적 수렴 */
			scale_down(rwb, false); /* [한국어] 부드럽게 NVMe SQ depth 중앙으로 복귀 (hard_throttle=false) */
		break;
	default:
		break;
	}

	/*
	 * Re-arm timer, if we have IO in flight
	 */
	if (rqd->scale_step || inflight) /* [한국어] SQ 조정 중이거나 in-flight CID 있으면 다음 윈도우 예약 */
		rwb_arm_timer(rwb); /* [한국어] 다음 NVMe CQ latency 샘플링 타이머 재설정 */
}

/*
 * [한국어]
 * wbt_update_limits - queue depth 변경 시 WBT scale 상태를 초기화하고 한도 재계산
 *
 * @rwb: WBT 상태
 *
 * NVMe 드라이버가 queue depth를 변경하거나 min_lat_nsec가 바뀔 때 호출된다.
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
	struct rq_depth *rqd = &rwb->rq_depth; /* [한국어] NVMe SQ 소프트웨어 depth 상태 참조 */

	rqd->scale_step = 0; /* [한국어] NVMe SQ depth 중앙 상태로 리셋: 이전 scale_up/down 기록 초기화 */
	rqd->scaled_max = false; /* [한국어] hardware max depth 미도달 플래그 초기화 */

	rq_depth_calc_max_depth(rqd); /* [한국어] blk-mq queue depth 기반 NVMe SQ max_depth 재계산 */
	calc_wb_limits(rwb); /* [한국어] wb_normal/wb_background CID 한도 재계산 */

	rwb_wake_all(rwb); /* [한국어] 한도 변경으로 대기 중인 doorbell submit 태스크 깨우기 */
}

/*
 * [한국어]
 * wbt_disabled - 현재 WBT가 비활성 상태인지 확인
 *
 * @q: request_queue (NVMe 디스크 큐)
 * @return: true이면 WBT 꺼짐 (SQ doorbell 무제한)
 *
 * WBT rq_qos가 없거나 enable_state가 OFF인 경우 true를 반환한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio 또는 외부 → [wbt_disabled]
 */
bool wbt_disabled(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] request_queue에서 WBT rq_qos 객체 검색 */

	return !rqos || !rwb_enabled(RQWB(rqos)); /* [한국어] WBT 없거나 꺼져 있으면 doorbell 무제한 */
}

/*
 * [한국어]
 * wbt_get_min_lat - WBT의 현재 목표 read latency 반환
 *
 * @q: request_queue
 * @return: min_lat_nsec (ns), WBT 없으면 0
 *
 * sysfs나 드라이버에서 현재 설정된 NVMe read CID 목표 완료 latency를 조회할 때
 * 사용한다.
 *
 * 호출 체인:
 *   sysfs read → [wbt_get_min_lat]
 */
u64 wbt_get_min_lat(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] WBT QoS 핸들 획득 */
	if (!rqos) /* [한국어] NVMe 디스크에 WBT가 설치되지 않은 경우 0 반환 */
		return 0;
	return RQWB(rqos)->min_lat_nsec; /* [한국어] NVMe read CID 목표 완료 latency (ns) 반환 */
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

	RQWB(rqos)->min_lat_nsec = val; /* [한국어] NVMe read CID 목표 완료 latency 갱신 */
	if (val) /* [한국어] latency 값이 있으면 수동 WBT 활성화 */
		RQWB(rqos)->enable_state = WBT_STATE_ON_MANUAL; /* [한국어] 수동 WBT 켬: SQ 소프트웨어 스로틀링 활성 */
	else
		RQWB(rqos)->enable_state = WBT_STATE_OFF_MANUAL; /* [한국어] 수동 WBT 끔: doorbell 제한 해제 */

	wbt_update_limits(RQWB(rqos)); /* [한국어] 새 목표 latency에 맞춰 CID 발행 한도 재설정 */
}


/*
 * [한국어]
 * close_io - 최근 100ms 이내에 NVMe read CID 발행/완료 이력 확인
 *
 * @rwb: WBT 상태
 * @return: true이면 최근 read 활동 있음 (write CID 한도를 더 낮게 적용)
 *
 * last_issue (sync read 발행 시각) 또는 last_comp (sync read CQ 완료 시각)
 * 기준으로 100ms 이내이면 read 경쟁이 활발하다고 보고 write CID 한도를
 * wb_background로 낮춘다.
 *
 * 호출 체인:
 *   get_limit → [close_io]
 */
static bool close_io(struct rq_wb *rwb)
{
	const unsigned long now = jiffies; /* [한국어] 현재 jiffies 시각 */

	return time_before(now, rwb->last_issue + HZ / 10) || /* [한국어] 100ms 이내 sync read CID doorbell 발행 이력 */
		time_before(now, rwb->last_comp + HZ / 10); /* [한국어] 100ms 이내 sync read CID CQ 완료 이력 */
}

/* [한국어] REQ_HIPRIO: NVMe SQ에서 우선 처리할 sync/meta/prio/swap bio 플래그 조합 */
#define REQ_HIPRIO	(REQ_SYNC | REQ_META | REQ_PRIO | REQ_SWAP)

/*
 * [한국어]
 * get_limit - bio 우선순위에 따른 NVMe SQ in-flight CID 상한 반환
 *
 * @rwb: WBT 상태
 * @opf: bio의 opf 플래그 (REQ_SYNC, REQ_BACKGROUND, REQ_SWAP 등)
 * @return: 이 bio가 사용할 수 있는 NVMe SQ 동시 발행 CID 상한
 *
 * bio 우선순위에 따라 max_depth / wb_normal / wb_background 중 하나를
 * 반환한다. sync/swap/HIPRIO bio는 NVMe SQ를 최대한 활용하고, 배경 쓰기는
 * wb_background로 read latency 보호를 위해 발행량을 줄인다.
 *
 * 호출 체인:
 *   wbt_inflight_cb → [get_limit]
 *                      → close_io
 */
static inline unsigned int get_limit(struct rq_wb *rwb, blk_opf_t opf)
{
	unsigned int limit; /* [한국어] 이 bio의 NVMe SQ 동시 발행 CID 상한 */

	if ((opf & REQ_OP_MASK) == REQ_OP_DISCARD) /* [한국어] discard/Deallocate CID는 배경 한도로 제한 */
		return rwb->wb_background; /* [한국어] NVMe Deallocate 동시 발행 개수 제한: read와 경쟁 최소화 */

	/*
	 * At this point we know it's a buffered write. If this is
	 * swap trying to free memory, or REQ_SYNC is set, then
	 * it's WB_SYNC_ALL writeback, and we'll use the max limit for
	 * that. If the write is marked as a background write, then use
	 * the idle limit, or go to normal if we haven't had competing
	 * IO for a bit.
	 */
	if ((opf & REQ_HIPRIO) || wb_recent_wait(rwb)) /* [한국어] sync/swap 고우선순위 또는 상위 태스크 대기 이력 */
		limit = rwb->rq_depth.max_depth; /* [한국어] NVMe SQ 최대 depth까지 허용: read latency 경쟁 낮음 판단 */
	else if ((opf & REQ_BACKGROUND) || close_io(rwb)) { /* [한국어] 배경 쓰기 또는 최근 100ms 내 read 활동 */
		/*
		 * If less than 100ms since we completed unrelated IO,
		 * limit us to half the depth for background writeback.
		 */
		limit = rwb->wb_background; /* [한국어] read latency 보호를 위해 배경 한도로 CID 발행 제한 */
	} else
		limit = rwb->wb_normal; /* [한국어] 일반 쓰기: 중간 한도 (wb_normal ≈ max_depth / 2) */

	return limit; /* [한국어] bio→request 변환 전 허용 NVMe SQ CID 발행 상한 */
}

/*
 * [한국어]
 * struct wbt_wait_data - rq_qos_wait 콜백에 전달되는 bio 컨텍스트
 *
 * bio가 NVMe SQ 진입 전 rq_wait에서 대기할 때 wbt_inflight_cb /
 * wbt_cleanup_cb에 전달된다. get_limit 호출에 필요한 opf와 WBT 회계에
 * 필요한 wb_acct를 묶어 단일 private_data로 전달한다.
 */
struct wbt_wait_data {
	struct rq_wb *rwb;
	/* [한국어] NVMe SQ WBT 상태 포인터.
	 * 설정자: __wbt_wait()에서 초기화.
	 * 읽는 자: wbt_inflight_cb, wbt_cleanup_cb.
	 * 동기화: 단일 bio submit 스레드에서만 사용 — 별도 락 불필요. */

	enum wbt_flags wb_acct;
	/* [한국어] 이 bio의 NVMe SQ CID 그룹 분류 (WBT_TRACKED | WBT_SWAP | WBT_DISCARD 등).
	 * 설정자: __wbt_wait()에서 bio_to_wbt_flags() 결과를 전달.
	 * 읽는 자: wbt_cleanup_cb → wbt_rqw_done으로 회계 롤백.
	 * 값 범위: wbt_flags 열거형 비트 조합. */

	blk_opf_t opf;
	/* [한국어] bio의 opf 플래그 (REQ_SYNC / REQ_BACKGROUND / REQ_SWAP 등).
	 * 설정자: __wbt_wait()에서 bio->bi_opf를 전달.
	 * 읽는 자: wbt_inflight_cb → get_limit으로 NVMe SQ 동시 발행 상한 결정.
	 * 값 범위: blk_opf_t 비트 조합. */
};

/*
 * [한국어]
 * wbt_inflight_cb - rq_qos_wait 대기 재개 시 NVMe SQ CID 슬롯 원자 획득 시도
 *
 * @rqw: 이 bio의 CID 그룹 rq_wait (bg/swap/discard 중 하나)
 * @private_data: struct wbt_wait_data 포인터
 * @return: true이면 inflight 증가 성공 (NVMe SQ 진입 허가),
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
	return rq_wait_inc_below(rqw, get_limit(data->rwb, data->opf)); /* [한국어] 현재 CID 한도 미만이면 atomic inflight 증가 -> NVMe SQ 진입 허가 */
}

/*
 * [한국어]
 * wbt_cleanup_cb - rq_qos_wait 대기 취소 시 NVMe SQ CID 회계 롤백
 *
 * @rqw: CID 그룹 rq_wait
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
	wbt_rqw_done(data->rwb, rqw, data->wb_acct); /* [한국어] NVMe SQ 진입 실패 시 CID 회계 롤백 (inflight 감소 + 대기 큐 깨우기) */
}

/*
 * [한국어]
 * __wbt_wait - NVMe SQ in-flight 한도 초과 시 bio를 대기시킴
 *
 * @rwb: WBT 상태
 * @wb_acct: 이 bio의 CID 그룹 (WBT_TRACKED / WBT_SWAP / WBT_DISCARD)
 * @opf: bio의 opf 플래그 (get_limit에서 NVMe SQ 동시 발행 상한 결정용)
 *
 * blk_mq_submit_bio → blk_mq_get_request 이전에 실행되어 NVMe SQ
 * doorbell을 치기 전에 소프트웨어적으로 CID 발행을 제어한다.
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
	struct rq_wait *rqw = get_rq_wait(rwb, wb_acct); /* [한국어] CID 그룹별 rq_wait 선택 (bg/swap/discard) */
	struct wbt_wait_data data = {
		.rwb = rwb, /* [한국어] NVMe SQ WBT 상태 전달 */
		.wb_acct = wb_acct, /* [한국어] CID 그룹 전달: 회계 및 한도 계산에 사용 */
		.opf = opf, /* [한국어] bio 플래그 전달: get_limit에서 NVMe SQ 우선순위 결정 */
	};

	rq_qos_wait(rqw, &data, wbt_inflight_cb, wbt_cleanup_cb); /* [한국어] CID 슬롯 확보까지 대기: NVMe SQ doorbell 직전 소프트웨어 게이트 */
}

/*
 * [한국어]
 * wbt_should_throttle - bio가 WBT 스로틀링 대상인지 판별
 *
 * @bio: 검사할 bio
 * @return: true이면 WBT 추적/스로틀링 대상 (buffered write 또는 discard)
 *
 * buffered write와 discard/trim만 NVMe SQ 동시 발행 제한을 적용한다.
 * O_DIRECT sync write (REQ_SYNC|REQ_IDLE 조합)는 제외 — 직접 I/O는
 * 페이지 캐시를 거치지 않아 doorbell 폭주 위험이 낮다.
 *
 * 호출 체인:
 *   bio_to_wbt_flags → [wbt_should_throttle]
 */
static inline bool wbt_should_throttle(struct bio *bio)
{
	switch (bio_op(bio)) { /* [한국어] bio opcode로 NVMe SQ 스로틀링 대상 판별 */
	case REQ_OP_WRITE: /* [한국어] 쓰기 CID 후보: O_DIRECT 여부 추가 확인 필요 */
		/*
		 * Don't throttle WRITE_ODIRECT
		 */
		if ((bio->bi_opf & (REQ_SYNC | REQ_IDLE)) == /* [한국어] O_DIRECT sync write: REQ_SYNC|REQ_IDLE 동시 설정 */
		    (REQ_SYNC | REQ_IDLE)) /* [한국어] O_DIRECT 경로는 doorbell 폭주 우려 낮음 → 제외 */
			return false; /* [한국어] NVMe SQ 스로틀링 대상 아님 */
		fallthrough; /* [한국어] buffered write는 아래 discard와 같이 true 반환 */
	case REQ_OP_DISCARD: /* [한국어] discard/TRIM CID: write와 같이 NVMe SQ 한도 적용 */
		return true; /* [한국어] NVMe SQ 동시 발행 제한 필요 */
	default:
		return false; /* [한국어] read 등은 별도 추적/제한 없음 */
	}
}

/*
 * [한국어]
 * bio_to_wbt_flags - bio 특성에 따른 WBT CID 그룹 플래그 생성
 *
 * @rwb: WBT 상태
 * @bio: 분류할 bio
 * @return: wbt_flags 비트 조합 (WBT_READ / WBT_TRACKED / WBT_SWAP / WBT_DISCARD)
 *
 * bio가 NVMe SQ의 어떤 CID 그룹에 속할지 결정한다. read이면 WBT_READ (latency
 * 샘플링), buffered write/discard이면 WBT_TRACKED를 설정하고 swap/discard 여부에
 * 따라 추가 비트를 설정한다.
 *
 * 호출 체인:
 *   wbt_wait / wbt_track / wbt_cleanup → [bio_to_wbt_flags]
 *                                         → wbt_should_throttle
 */
static enum wbt_flags bio_to_wbt_flags(struct rq_wb *rwb, struct bio *bio)
{
	enum wbt_flags flags = 0; /* [한국어] CID 그룹 플래그 초기화 */

	if (!rwb_enabled(rwb)) /* [한국어] WBT 꺼져 있으면 추적 플래그 없음 (doorbell 무제한) */
		return 0;

	if (bio_op(bio) == REQ_OP_READ) { /* [한국어] NVMe read CID 분류: latency 샘플링 대상 */
		flags = WBT_READ; /* [한국어] read CID: sync_issue/sync_cookie 추적 대상 */
	} else if (wbt_should_throttle(bio)) { /* [한국어] buffered write / discard CID */
		if (bio->bi_opf & REQ_SWAP) /* [한국어] swapout으로 생성된 긴급 쓰기: swap 그룹으로 분리 */
			flags |= WBT_SWAP; /* [한국어] swap 그룹 rq_wait 사용: OOM 방지 우선순위 유지 */
		if (bio_op(bio) == REQ_OP_DISCARD) /* [한국어] discard/TRIM CID */
			flags |= WBT_DISCARD; /* [한국어] discard 그룹 rq_wait 사용: wb_background 한도 적용 */
		flags |= WBT_TRACKED; /* [한국어] NVMe SQ inflight 추적 대상: __wbt_wait로 대기 */
	}
	return flags; /* [한국어] bio→request 변환 시 request->rq_flags에 복사될 WBT CID 그룹 */
}

/*
 * [한국어]
 * wbt_cleanup - bio가 request로 변환되지 못한 경우 NVMe SQ CID 회계 정리
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
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] NVMe SQ WBT 상태 복원 */
	enum wbt_flags flags = bio_to_wbt_flags(rwb, bio); /* [한국어] 실패한 bio의 CID 그룹 재판별 */
	__wbt_done(rqos, flags); /* [한국어] NVMe SQ 진입 전 실패 시 inflight 감소 + 대기 큐 깨우기 */
}

/*
 * [한국어]
 * wbt_wait - bio가 request로 변환되기 전 WBT 스로틀링 수행
 *
 * @rqos: WBT rq_qos
 * @bio: 제출 중인 bio
 *
 * blk_mq_submit_bio → __rq_qos_throttle 훅으로 호출된다. NVMe SQ의
 * doorbell을 치기 직전 마지막 소프트웨어 CID 발행 제어 지점이다.
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
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] NVMe SQ WBT 상태 복원 */
	enum wbt_flags flags; /* [한국어] 이 bio의 NVMe SQ CID 그룹 */

	flags = bio_to_wbt_flags(rwb, bio); /* [한국어] bio → NVMe SQ CID 그룹 분류 */
	if (!(flags & WBT_TRACKED)) { /* [한국어] tracked write/discard가 아니면 스로틀링 없이 통과 */
		if (flags & WBT_READ) /* [한국어] read CID면 발행 시각 갱신: close_io 판단에 사용 */
			wb_timestamp(rwb, &rwb->last_issue); /* [한국어] last_issue = 현재 시각: 100ms 내 read 활동 기록 */
		return; /* [한국어] writeback throttling 불필요: NVMe SQ 직행 */
	}

	__wbt_wait(rwb, flags, bio->bi_opf); /* [한국어] CID 한도 도달 시 슬롯 확보까지 대기: NVMe SQ doorbell 지연 */

	if (!blk_stat_is_active(rwb->cb)) /* [한국어] blk-stat latency 타이머가 비활성이면 */
		rwb_arm_timer(rwb); /* [한국어] NVMe CQ 완료 latency 모니터링 타이머 시작 */
}

/*
 * [한국어]
 * wbt_track - bio가 request와 결합될 때 WBT CID 그룹 플래그를 request에 기록
 *
 * @rqos: WBT rq_qos
 * @rq: 생성된 request
 * @bio: 결합된 bio
 *
 * blk_mq_submit_bio → rq_qos_track 훅으로 호출된다. request->wbt_flags에
 * bio의 CID 그룹(WBT_TRACKED / WBT_SWAP / WBT_DISCARD)을 OR 적산하여
 * wbt_issue / wbt_done에서 회계를 연결한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → rq_qos_track → [wbt_track]
 *                                        → bio_to_wbt_flags
 */
static void wbt_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] NVMe SQ WBT 상태 복원 */
	rq->wbt_flags |= bio_to_wbt_flags(rwb, bio); /* [한국어] request에 CID 그룹 OR 적산: 다중 bio 병합 시 플래그 누적 */
}

/*
 * [한국어]
 * wbt_issue - request를 NVMe SQ에 발행할 때 sync read 발행 시각 기록
 *
 * @rqos: WBT rq_qos
 * @rq: 발행 중인 request (NVMe SQ로 doorbell 치기 직전)
 *
 * blk_mq_issue_request → rq_qos_issue 훅으로 호출된다. sync read CID가
 * NVMe SQ에서 오래 머무는 경우 blk-stat 완료 이벤트 없이도 latency 초과를
 * 조기에 감지하기 위해 sync_issue에 발행 시각을 기록한다.
 *
 * 실행 컨텍스트: blk_mq_issue_request 경로 (태스크 또는 kblockd)
 *
 * 호출 체인:
 *   blk_mq_issue_request → rq_qos_issue → [wbt_issue]
 */
static void wbt_issue(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] NVMe SQ WBT 상태 복원 */

	if (!rwb_enabled(rwb)) /* [한국어] WBT off면 sync read 발행 시각 추적 불필요 */
		return;

	/*
	 * Track sync issue, in case it takes a long time to complete. Allows us
	 * to react quicker, if a sync IO takes a long time to complete. Note
	 * that this is just a hint. The request can go away when it completes,
	 * so it's important we never dereference it. We only use the address to
	 * compare with, which is why we store the sync_issue time locally.
	 */
	if (wbt_is_read(rq) && !rwb->sync_issue) { /* [한국어] read CID이고 아직 추적 중인 sync read 없을 때 */
		rwb->sync_cookie = rq; /* [한국어] sync read 식별용 쿠키: 완료 시 주소 비교용 (역참조 금지) */
		rwb->sync_issue = rq->io_start_time_ns; /* [한국어] NVMe SQ doorbell 시각: io_start_time_ns 저장 */
	}
}

/*
 * [한국어]
 * wbt_requeue - request가 NVMe SQ에서 취소되어 재큐될 때 sync_cookie 정리
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
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] NVMe SQ WBT 상태 복원 */
	if (!rwb_enabled(rwb)) /* [한국어] WBT off면 재큐 추적 불필요 */
		return;
	if (rq == rwb->sync_cookie) { /* [한국어] 재큐된 request가 추적 중인 sync read CID면 */
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
 * blk_stat_alloc_callback에 함수 포인터로 등록된다. NVMe CQ 완료 이벤트가
 * blk-stat 콜백에 도달할 때 호출되어 read/write 완료 latency를 별도 버킷에
 * 분리하여 저장하도록 방향을 지정한다.
 *
 * 호출 체인:
 *   blk_stat_callback → [wbt_data_dir] (콜백 등록 시 함수 포인터)
 */
static int wbt_data_dir(const struct request *rq)
{
	const enum req_op op = req_op(rq); /* [한국어] request opcode → NVMe command 계열 판별 */

	if (op == REQ_OP_READ) /* [한국어] NVMe Read Command: READ 버킷 */
		return READ;
	else if (op_is_write(op)) /* [한국어] NVMe Write/Flush Command 계열: WRITE 버킷 */
		return WRITE;

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
	struct rq_wb *rwb = kzalloc_obj(*rwb); /* [한국어] NVMe SQ WBT 상태 객체 영청 할당 */

	if (!rwb) /* [한국어] 메모리 부족: WBT 설치 실패, doorbell 제한 없음 */
		return NULL;

	rwb->cb = blk_stat_alloc_callback(wb_timer_fn, wbt_data_dir, 2, rwb); /* [한국어] blk-stat 콜백 등록: READ/WRITE 2채널 NVMe CQ latency 샘플링 */
	if (!rwb->cb) { /* [한국어] blk-stat 콜백 할당 실패 */
		kfree(rwb); /* [한국어] rq_wb 해제 */
		return NULL; /* [한국어] WBT 계층 미설치 */
	}

	return rwb; /* [한국어] NVMe SQ 스로틀링 상태 객체 반환 */
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
	blk_stat_free_callback(rwb->cb); /* [한국어] NVMe CQ latency 샘플링 콜백 해제 */
	kfree(rwb); /* [한국어] NVMe SQ WBT 상태 메모리 반환 */
}

/*
 * [한국어]
 * __wbt_enable_default - NVMe 디스크에 WBT를 기본 활성화할 조건 판단
 *
 * @disk: 검사할 gendisk (NVMe 디스크)
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
	struct request_queue *q = disk->queue; /* [한국어] NVMe 디스크의 request_queue */
	struct rq_qos *rqos; /* [한국어] 기존 WBT rq_qos 핸들 */
	bool enable = IS_ENABLED(CONFIG_BLK_WBT_MQ); /* [한국어] blk-mq WBT 컴파일 옵션: NVMe multi-queue 지원 빌드이면 true */

	mutex_lock(&disk->rqos_state_mutex); /* [한국어] rq_qos 상태 보호: NVMe 디스크 등록/해제와의 race 방지 */

	if (blk_queue_disable_wbt(q)) /* [한국어] NVMe 큐에서 WBT 비활성화 플래그 설정 시 */
		enable = false; /* [한국어] WBT 금지: doorbell 무제한 유지 */

	/* Throttling already enabled? */
	rqos = wbt_rq_qos(q); /* [한국어] 이미 설치된 WBT rq_qos 검색 */
	if (rqos) { /* [한국어] WBT 이미 존재: enable_state만 갱신 */
		if (enable && RQWB(rqos)->enable_state == WBT_STATE_OFF_DEFAULT) /* [한국어] 금지 해제되고 기본 off 상태면 */
			RQWB(rqos)->enable_state = WBT_STATE_ON_DEFAULT; /* [한국어] NVMe SQ 스로틀링 기본 활성화 */
		mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */
		return false; /* [한국어] 추가 설치 불필요 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */

	/* Queue not registered? Maybe shutting down... */
	if (!blk_queue_registered(q)) /* [한국어] NVMe 디스크 등록 전이거나 해제 중이면 설치 불가 */
		return false;

	if (queue_is_mq(q) && enable) /* [한국어] blk-mq 큐이고 WBT 활성화 조건 충족: 신규 설치 진행 */
		return true;
	return false; /* [한국어] non-mq 또는 WBT off: 설치 안 함 */
}

/*
 * [한국어]
 * wbt_enable_default - NVMe 디스크에 WBT 기본 활성화 검사 (외부 호출용 래퍼)
 *
 * @disk: gendisk
 *
 * __wbt_enable_default를 호출하여 WBT 활성화 조건만 확인한다.
 * NVMe 드라이버나 다른 블록 드라이버가 EXPORT_SYMBOL_GPL을 통해 호출.
 *
 * 호출 체인:
 *   NVMe 드라이버 / blk 코어 → [wbt_enable_default]
 *                               → __wbt_enable_default
 */
void wbt_enable_default(struct gendisk *disk)
{
	__wbt_enable_default(disk); /* [한국어] WBT 기본 활성화 조건 검사 (반환값 무시) */
}
EXPORT_SYMBOL_GPL(wbt_enable_default); /* [한국어] NVMe 드라이버 등에서 WBT 기본 활성화 심볼 노출 */

/*
 * [한국어]
 * wbt_init_enable_default - WBT 설치 조건 충족 시 rq_wb 할당·초기화·debugfs 등록
 *
 * @disk: gendisk (NVMe 디스크)
 *
 * add_disk 경로에서 NVMe 디스크가 등록될 때 호출된다. __wbt_enable_default로
 * 조건을 확인하고, 충족되면 wbt_alloc → wbt_init으로 rq_wb를 할당하고
 * request_queue의 rq_qos 리스트에 등록한다.
 *
 * 실행 컨텍스트: add_disk 경로 (NVMe 컨트롤러 초기화 완료 후)
 *
 * 호출 체인:
 *   add_disk → [wbt_init_enable_default]
 *               → __wbt_enable_default
 *               → wbt_alloc
 *               → wbt_init
 */
void wbt_init_enable_default(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] NVMe 디스크 request_queue */
	struct rq_wb *rwb;
	unsigned int memflags; /* [한국어] debugfs lock 반환값 저장 */

	if (!__wbt_enable_default(disk)) /* [한국어] WBT 설치 조건 미충족: 설치 생략 */
		return;

	rwb = wbt_alloc(); /* [한국어] NVMe SQ WBT 상태 객체 할당 */
	if (!rwb) /* [한국어] 메모리 부족: WBT 설치 실패 */
		return;

	if (wbt_init(disk, rwb)) { /* [한국어] rq_qos 리스트 등록 및 rq_depth / win_nsec 초기화 */
		pr_warn("%s: failed to enable wbt\n", disk->disk_name); /* [한국어] NVMe 디스크 WBT 활성화 실패 경고 */
		wbt_free(rwb); /* [한국어] 할당된 rq_wb 해제 */
		return; /* WBT 미설치, doorbell 제한 없음 */
	}

	memflags = blk_debugfs_lock(q); /* [한국어] debugfs 등록 동기화 */
	blk_mq_debugfs_register_rq_qos(q); /* [한국어] NVMe SQ WBT 상태 debugfs 노드 등록 */
	blk_debugfs_unlock(q, memflags); /* [한국어] debugfs lock 해제 */
}

/*
 * [한국어]
 * wbt_default_latency_nsec - 회전식/비회전식 디스크의 기본 WBT 목표 latency 반환
 *
 * @q: request_queue
 * @return: 비회전식(NVMe SSD) 2,000,000 ns (2ms), 회전식(HDD) 75,000,000 ns (75ms)
 *
 * NVMe SSD는 기본 2ms로 설정하여 SQ 포화 상태에서 빠르게 scale_down이
 * 발동되도록 한다. HDD는 회전 지연이 크므로 75ms를 사용한다.
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
	if (blk_queue_rot(q)) /* [한국어] 회전식 디스크(HDD): NVMe는 항상 false */
		return 75000000ULL; /* [한국어] HDD 기본 목표 latency 75ms */
	return 2000000ULL; /* [한국어] NVMe SSD 기본 목표 latency 2ms: SQ 포화 빠른 감지 */
}

/*
 * [한국어]
 * wbt_queue_depth_changed - NVMe SQ/CQ depth 변경 시 WBT 한도 재계산
 *
 * @rqos: WBT rq_qos
 *
 * blk_mq_update_nr_hw_queues 등으로 NVMe 컨트롤러의 SQ depth가 변경되면
 * rq_depth.queue_depth를 새 값으로 동기화하고 wbt_update_limits로
 * wb_normal / wb_background를 재계산한다.
 *
 * 호출 체인:
 *   rq_qos_queue_depth_changed → [wbt_queue_depth_changed]
 *                                  → wbt_update_limits
 */
static void wbt_queue_depth_changed(struct rq_qos *rqos)
{
	RQWB(rqos)->rq_depth.queue_depth = blk_queue_depth(rqos->disk->queue); /* [한국어] NVMe 하드웨어 queue depth 변경 시 rq_depth 동기화 */
	wbt_update_limits(RQWB(rqos)); /* [한국어] 변경된 SQ depth에 맞춰 CID 발행 한도 재계산 */
}

/*
 * [한국어]
 * wbt_exit - WBT rq_qos 계층 제거 및 메모리 해제
 *
 * @rqos: WBT rq_qos
 *
 * rq_qos_exit 훅으로 NVMe 디스크가 제거되거나 WBT가 비활성화될 때 호출된다.
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
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] 제거할 NVMe SQ WBT 상태 복원 */

	blk_stat_remove_callback(rqos->disk->queue, rwb->cb); /* [한국어] NVMe CQ latency 샘플링 콜백 제거 */
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
	struct rq_wb *rwb;
	if (!rqos) /* [한국어] WBT 미설치: 변경 없음 */
		return;
	mutex_lock(&disk->rqos_state_mutex); /* [한국어] rq_qos 상태 보호: 동시 enable/disable race 방지 */
	rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */
	if (rwb->enable_state == WBT_STATE_ON_DEFAULT) { /* [한국어] 기본 on 상태일 때만 off로 전환: 수동 설정 보존 */
		blk_stat_deactivate(rwb->cb); /* [한국어] NVMe CQ latency 샘플링 중단 */
		rwb->enable_state = WBT_STATE_OFF_DEFAULT; /* [한국어] WBT 기본 off: doorbell 제한 해제 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */
}
EXPORT_SYMBOL_GPL(wbt_disable_default); /* [한국어] WBT 비활성화 심볼 노출: NVMe 드라이버 등에서 사용 */

#ifdef CONFIG_BLK_DEBUG_FS
/* [한국어] debugfs 항목들: NVMe SQ 스로틀링 상태를 /sys/kernel/debug/block/<dev>/wbt에 노출 */

/*
 * [한국어]
 * wbt_curr_win_nsec_show - debugfs: 현재 blk-stat 샘플링 윈도우 크기 출력
 */
static int wbt_curr_win_nsec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] debugfs data: WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%llu\n", rwb->cur_win_nsec); /* [한국어] 현재 NVMe CQ latency 모니터링 윈도우(ns) 출력 */
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
 * wbt_id_show - debugfs: rq_qos ID 출력 (NVMe 디스크 내 QoS 식별자)
 */
static int wbt_id_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */

	seq_printf(m, "%u\n", rqos->id); /* [한국어] rq_qos ID: NVMe 디스크 내 WBT QoS 식별자 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_inflight_show - debugfs: 각 CID 그룹의 현재 in-flight 수 출력
 */
static int wbt_inflight_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */
	int i;

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹 순회 */
		seq_printf(m, "%d: inflight %d\n", i, /* [한국어] 그룹별 NVMe SQ in-flight CID 개수 출력 */
			   atomic_read(&rwb->rq_wait[i].inflight));
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

	seq_printf(m, "%lu\n", rwb->min_lat_nsec); /* [한국어] NVMe read CID 목표 완료 latency(ns) 출력 */
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
 * wbt_normal_show - debugfs: 일반 쓰기 CID 발행 상한(wb_normal) 출력
 */
static int wbt_normal_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%u\n", rwb->wb_normal); /* [한국어] 일반 쓰기 NVMe SQ CID 발행 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * wbt_background_show - debugfs: 배경 쓰기 CID 발행 상한(wb_background) 출력
 */
static int wbt_background_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data; /* [한국어] WBT rq_qos */
	struct rq_wb *rwb = RQWB(rqos); /* [한국어] WBT 상태 복원 */

	seq_printf(m, "%u\n", rwb->wb_background); /* [한국어] 배경 쓰기 NVMe SQ CID 발행 상한 출력 */
	return 0;
}

/* [한국어] wbt_debugfs_attrs: WBT debugfs 항목 테이블 */
static const struct blk_mq_debugfs_attr wbt_debugfs_attrs[] = {
	{"curr_win_nsec", 0400, wbt_curr_win_nsec_show}, /* [한국어] 현재 blk-stat 윈도우 크기(ns) */
	{"enabled", 0400, wbt_enabled_show},             /* [한국어] WBT 활성화 상태 */
	{"id", 0400, wbt_id_show},                       /* [한국어] rq_qos ID */
	{"inflight", 0400, wbt_inflight_show},           /* [한국어] 그룹별 in-flight CID 수 */
	{"min_lat_nsec", 0400, wbt_min_lat_nsec_show},   /* [한국어] 목표 read latency(ns) */
	{"unknown_cnt", 0400, wbt_unknown_cnt_show},     /* [한국어] 연속 LAT_UNKNOWN 횟수 */
	{"wb_normal", 0400, wbt_normal_show},            /* [한국어] 일반 쓰기 CID 상한 */
	{"wb_background", 0400, wbt_background_show},   /* [한국어] 배경 쓰기 CID 상한 */
	{},
};
#endif

/*
 * [한국어]
 * wbt_rqos_ops - blk-rq-qos.c가 bio/request 생명주기 단계마다 호출하는 콜백 테이블
 *
 * NVMe SQ로 들어가는 bio/request가 submit/issue/done/requeue 단계를 거칠 때
 * WBT가 개입할 훅 함수들을 정의한다. blk-rq-qos.c가 이 테이블을 통해
 * 각 콜백을 호출한다.
 */
static const struct rq_qos_ops wbt_rqos_ops = {
	.throttle = wbt_wait,                           /* [한국어] bio→request 변환 전: NVMe SQ CID 한도 대기 */
	.issue = wbt_issue,                             /* [한국어] doorbell 직전: sync read CID 발행 시각 기록 */
	.track = wbt_track,                             /* [한국어] request에 CID 그룹 기록: issue/done 회계 연결 */
	.requeue = wbt_requeue,                         /* [한국어] NVMe SQ 회수 후 재발행: sync_cookie 정리 */
	.done = wbt_done,                               /* [한국어] CQ 완료 후: inflight 감소 및 WBT 상태 정리 */
	.cleanup = wbt_cleanup,                         /* [한국어] bio NVMe SQ 진입 실패 시 회계 롤백 */
	.queue_depth_changed = wbt_queue_depth_changed, /* [한국어] NVMe SQ/CQ depth 변경 시 CID 한도 재계산 */
	.exit = wbt_exit,                               /* [한국어] NVMe 디스크 제거 시 WBT rq_qos 해제 */
#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = wbt_debugfs_attrs,             /* [한국어] NVMe SQ 스로틀링 상태 debugfs 노출 */
#endif
};

/*
 * [한국어]
 * wbt_init - rq_wb 초기화 및 request_queue의 rq_qos 리스트에 WBT 등록
 *
 * @disk: NVMe gendisk
 * @rwb: 이미 할당된 rq_wb (wbt_alloc으로 생성됨)
 * @return: 0 성공, 음수 오류
 *
 * NVMe 디스크에 WBT 계층을 설치한다. rq_wait 3개를 초기화하고, 기본값
 * (100ms 윈도우, 16 depth, 2ms latency)을 설정한 뒤 rq_qos_add로
 * blk-rq-qos 리스트에 등록하고 blk_stat_add_callback으로 NVMe CQ 완료
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
	struct request_queue *q = disk->queue; /* [한국어] NVMe 디스크의 request_queue */
	int ret; /* [한국어] rq_qos_add 등록 결과 */
	int i; /* [한국어] CID 그룹 순회 인덱스 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* [한국어] BG/SWAP/DISCARD 그룹별 rq_wait 초기화 */
		rq_wait_init(&rwb->rq_wait[i]); /* [한국어] atomic inflight = 0, waitqueue 초기화 */

	rwb->last_comp = rwb->last_issue = jiffies; /* [한국어] read CID 활동 기준 시각 초기화: close_io 판단 기준 */
	rwb->win_nsec = RWB_WINDOW_NSEC; /* [한국어] NVMe CQ latency 샘플링 기본 100ms 윈도우 */
	rwb->enable_state = WBT_STATE_ON_DEFAULT; /* [한국어] WBT 기본 활성 상태: NVMe SQ 스로틀링 시작 */
	rwb->rq_depth.default_depth = RWB_DEF_DEPTH; /* [한국어] 초기 NVMe SQ 소프트웨어 depth 16 */
	rwb->min_lat_nsec = wbt_default_latency_nsec(q); /* [한국어] NVMe SSD면 2ms, HDD면 75ms 목표 latency */
	rwb->rq_depth.queue_depth = blk_queue_depth(q); /* [한국어] NVMe 하드웨어 queue depth 초기 동기화 */
	wbt_update_limits(rwb); /* [한국어] CID 발행 한도 초기화 (wb_normal / wb_background) */

	/*
	 * Assign rwb and add the stats callback.
	 */
	mutex_lock(&q->rq_qos_mutex); /* [한국어] rq_qos 리스트 보호: 동시 등록 race 방지 */
	ret = rq_qos_add(&rwb->rqos, disk, RQ_QOS_WBT, &wbt_rqos_ops); /* [한국어] blk-rq-qos에 WBT 등록: bio submit 경로에 훅 연결 */
	mutex_unlock(&q->rq_qos_mutex); /* [한국어] 리스트 보호 해제 */
	if (ret) /* [한국어] 등록 실패: 이미 WBT가 있거나 오류 */
		return ret;

	blk_stat_add_callback(q, rwb->cb); /* [한국어] NVMe CQ 완료 latency 샘플링 콜백 활성화 */
	return 0; /* [한국어] WBT 설치 성공 */
}

/*
 * [한국어]
 * wbt_set_lat - 사용자 지정 latency 값으로 WBT 목표 재설정
 *
 * @disk: gendisk (NVMe 디스크)
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
	struct request_queue *q = disk->queue; /* [한국어] NVMe 디스크 request_queue */
	struct rq_qos *rqos = wbt_rq_qos(q); /* [한국어] 기존 WBT rq_qos 핸들 */
	struct rq_wb *rwb = NULL; /* [한국어] 신규 rq_wb (WBT 미설치 시 할당) */
	unsigned int memflags; /* [한국어] blk_mq_freeze / debugfs lock 반환값 저장 */
	int ret = 0; /* [한국어] 결과 */

	if (!rqos) { /* [한국어] WBT 미설치: 신규 할당 필요 */
		rwb = wbt_alloc(); /* [한국어] NVMe SQ WBT 상태 신규 할당 */
		if (!rwb) /* [한국어] 메모리 부족 */
			return -ENOMEM;
	}

	/*
	 * Ensure that the queue is idled, in case the latency update
	 * ends up either enabling or disabling wbt completely. We can't
	 * have IO inflight if that happens.
	 */
	memflags = blk_mq_freeze_queue(q); /* [한국어] 큐 동결: WBT on/off 전환 중 in-flight IO가 없어야 함 */
	if (!rqos) { /* [한국어] WBT 미설치: wbt_init으로 초기화 */
		ret = wbt_init(disk, rwb); /* [한국어] NVMe SQ rq_qos 등록 및 blk-stat 콜백 활성화 */
		if (ret) { /* [한국어] 등록 실패 */
			wbt_free(rwb); /* [한국어] 신규 할당한 rq_wb 해제 */
			goto out; /* [한국어] unfreeze 후 종료 */
		}
	}

	if (val == -1) /* [한국어] -1: 기본 latency로 재설정 (NVMe SSD 2ms / HDD 75ms) */
		val = wbt_default_latency_nsec(q);
	else if (val >= 0) /* [한국어] 사용자 지정 값: μs 단위를 ns로 변환 */
		val *= 1000ULL; /* [한국어] μs → ns: blk-stat latency 샘플과 단위 통일 */

	if (wbt_get_min_lat(q) == val) /* [한국어] NVMe SQ 목표 latency 변화 없음: 불필요한 quiesce 회피 */
		goto out;

	blk_mq_quiesce_queue(q); /* [한국어] request 처리 일시 정지: latency 변경 중 NVMe SQ 안전 상태 확보 */

	mutex_lock(&disk->rqos_state_mutex); /* [한국어] WBT 상태 보호: enable_state race 방지 */
	wbt_set_min_lat(q, val); /* [한국어] 새 목표 latency 설정 및 WBT 활성 상태 업데이트 */
	mutex_unlock(&disk->rqos_state_mutex); /* [한국어] 상태 보호 해제 */

	blk_mq_unquiesce_queue(q); /* [한국어] NVMe SQ 처리 재개 */
out:
	blk_mq_unfreeze_queue(q, memflags); /* [한국어] 큐 동결 해제: NVMe SQ doorbell 발행 재개 */

	memflags = blk_debugfs_lock(q); /* [한국어] debugfs 등록 보호 */
	blk_mq_debugfs_register_rq_qos(q); /* [한국어] 변경된 NVMe SQ WBT 상태 debugfs 갱신 */
	blk_debugfs_unlock(q, memflags); /* [한국어] debugfs lock 해제 */

	return ret; /* [한국어] WBT latency 설정 결과 */
}

/* NVMe 관점 핵심 요약 */
/*
 * - wbt_wait는 blk_mq_submit_bio -> __rq_qos_throttle 경로에서 bio를
 *   request로 변환하기 전에 호출되며, NVMe SQ의 동시 발행 개수를
 *   wb_normal/wb_background 한도로 제한한다.
 * - wbt_issue/wbt_done은 각각 request 발행 시점과 NVMe completion
 *   시점(CQ 처리 이후)에 inflight 카운터를 조정하여 queue depth를
 *   동적으로 추적한다.
 * - wb_timer_fn은 blk-stat이 수집한 완료 지연(latency) 샘플을 보고
 *   NVMe NAND 쓰기 버퍼 포화 여부에 따라 scale_up/scale_down으로
 *   rq_depth.max_depth를 조절한다.
 * - NVMe SQ는 하나의 doorbell로 여러 CID를 발행할 수 있으므로,
 *   wbt는 SQ entry 개수(추정)보다는 blk-mq request queue depth를
 *   기준으로 하드웨어 부하를 추정한다.
 * - blk-rq-qos.c, blk-stat.c와 함께 동작하며, NVMe 드라이버의
 *   nvme_queue_rq 이전 단계에서 마지막 소프트웨어 스로틀링 계층이다.
 */
