// SPDX-License-Identifier: GPL-2.0

/*
 * NVMe 관점 파일 상단 요약:
 *
 * 이 파일은 blk-mq 기반 NVMe SSD에서 buffered writeback이 발생할 때
 * 큐 깊이(queue depth)를 동적으로 조절하여 NVMe SQ(Submission Queue)의
 * doorbell 폭주와 NAND 쓰기 버퍼 포화를 완화하는 역할을 한다.
 * 사용자 공간 -> VFS -> page cache -> blk_mq_submit_bio ->
 * __rq_qos_throttle -> wbt_wait -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서
 * bio가 request로 변환되기 전 단계에서 개입한다.
 * blk-rq-qos.c, blk-stat.c와 함께 동작하며,
 * NVMe latency 통계를 기반으로 SQ에 들어가는 CID 단위의
 * 동시 발행 개수를 스로틀링한다.
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
#include <linux/kernel.h>		/* 커널 기본 자료형 및 매크로 */
#include <linux/blk_types.h>		/* bio, request 관련 타입 정의 */
#include <linux/slab.h>			/* kzalloc_obj 등 메모리 할당 */
#include <linux/backing-dev.h>		/* bdi, balance_dirty_pages 관련 */
#include <linux/swap.h>			/* swap_writeout 경로 판별 */

#include "blk-stat.h"			/* blk_stat_callback: NVMe 완료 latency 샘플 수집 */
#include "blk-wbt.h"
#include "blk-rq-qos.h"			/* rq_qos_ops, rq_wait: bio/request 단계 QoS 훅 */
#include "elevator.h"
#include "blk.h"

#define CREATE_TRACE_POINTS /* wbt tracepoint 정의: NVMe SQ doorbell 제어 이벤트 추적용 (추정) */
#include <trace/events/wbt.h>

/*
 * WBT가 추적하는 request 유형을 나타낸다.
 * NVMe 관점에서는 이 플래그가 SQ에 들어갈 CID의 동작 특성을
 * 분류하는 데 사용된다.
 */
enum wbt_flags {
	WBT_TRACKED		= 1,	/* NVMe SQ로 발행할 buffered write/discard CID */
	WBT_READ		= 2,	/* NVMe read CID: latency 기준 샘플로 사용 */
	WBT_SWAP		= 4,	/* swap_writeout에서 날아오는 긴급 쓰기 */
	WBT_DISCARD		= 8,	/* NVMe Deallocate/Trim에 해당하는 discard */

	WBT_NR_BITS		= 4,	/* 플래그 비트 수 */
};

/*
 * WBT가 관리하는 대기 큐(wait queue) 종류.
 * NVMe SQ의 경우 배경 쓰기/스왑/trim을 별도의 어카운팅 그룹으로
 * 구분하여 동시 inflight CID 수를 조절한다.
 */
enum {
	WBT_RWQ_BG		= 0,	/* 일반 background writeback */
	WBT_RWQ_SWAP,			/* swapout으로 인한 긴급 쓰기 */
	WBT_RWQ_DISCARD,		/* discard/trim 요청 */
	WBT_NUM_RWQ,
};

/*
 * If current state is WBT_STATE_ON/OFF_DEFAULT, it can be covered to any other
 * state, if current state is WBT_STATE_ON/OFF_MANUAL, it can only be covered
 * to WBT_STATE_OFF/ON_MANUAL.
 */
enum {
	WBT_STATE_ON_DEFAULT	= 1,	/* 기본적으로 wbt 활성화 상태 */
	WBT_STATE_ON_MANUAL	= 2,	/* sysfs 등에서 수동으로 켬 */
	WBT_STATE_OFF_DEFAULT	= 3,	/* 기본적으로 wbt 비활성화 상태 */
	WBT_STATE_OFF_MANUAL	= 4,	/* sysfs 등에서 수동으로 끔 */
};

/*
 * struct rq_wb: NVMe SQ에 대한 writeback 스로틀링 상태를 담는다.
 *
 * 각 필드는 NVMe SQ/CQ 흐름에서 동시에 발행할 수 있는 CID 개수와
 * 완료 latency 목표를 관리하는 데 사용된다.
 */
struct rq_wb {
	/*
	 * Settings that govern how we throttle
	 */
	unsigned int wb_background; /* CID 상한으로 nvme_submit_cmd 호출 빈도를 간접 제한 (추정) */		/* 배경 쓰기 허용 CID 상한 */
	unsigned int wb_normal; /* 일반 쓰기 CID 상한: NVMe SQ doorbell 폭주 방지 (추정) */			/* 일반 쓰기 허용 CID 상한 */

	short enable_state; /* WBT on/off: NVMe SQ 소프트웨어 스로틀링 활성 여부 */			/* WBT_STATE_* */
	/*
	 * Number of consecutive periods where we don't have enough
	 * information to make a firm scale up/down decision.
	 */
	unsigned int unknown_cnt; /* latency 샘플 부족 시 NVMe SQ depth를 중앙 상태로 서서히 복귀 */		/* latency 샘플 부족이 연속된 횟수 */

	u64 win_nsec; /* NVMe CQ latency 샘플링 기본 윈도우 (100ms) */				/* 기본 모니터링 윈도우 (100ms) */
	u64 cur_win_nsec; /* SQ 혼잡 시 NVMe 반응 속도에 맞춰 줄어든 모니터링 윈도우 */			/* 현재 동적으로 조절된 윈도우 */

	struct blk_stat_callback *cb;		/* blk-stat 완료 콜백: CQ 완료 시 latency 수집 */

	u64 sync_issue; /* sync read CID 발행 시각: NVMe SQ 체류 시간 추적 */				/* 마지막 sync read CID 발행 시각 */
	void *sync_cookie; /* sync read request 식별자: CID 재발행 비교용 (비역참조) */			/* 마지막 sync read request 포인터(비역참조) */

	unsigned long last_issue; /* 마지막 read CID doorbell 시각(jiffies) */	/* 마지막 read CID 발행 시각(jiffies) */
	unsigned long last_comp; /* 마지막 read CID CQ 완료 시각(jiffies) */	/* 마지막 read CID 완료 시각(jiffies) */
	unsigned long min_lat_nsec; /* NVMe read CID 목표 완료 latency 임계값 */	/* 목표 latency: NVMe read 완료 기준 임계값 */
	struct rq_qos rqos;		/* rq_qos 연결 리스트 노드 */
	struct rq_wait rq_wait[WBT_NUM_RWQ]; /* NVMe SQ CID 그룹별 inflight 대기 큐 */	/* 배경/스왑/discard inflight 대기 큐 */
	struct rq_depth rq_depth; /* NVMe SQ에 대응하는 소프트웨어 queue depth 상태 */	/* NVMe SQ에 대한 동적 queue depth 상태 */
};

static int wbt_init(struct gendisk *disk, struct rq_wb *rwb);

/* rq_qos에서 rq_wb를 추출: blk-rq-qos.c의 리스트 탐색과 연결됨 */
static inline struct rq_wb *RQWB(struct rq_qos *rqos)
{
	return container_of(rqos, struct rq_wb, rqos); /* blk-rq-qos 리스트에서 rq_wb 획득 -> NVMe SQ QoS 상태 접근 */
}

static inline void wbt_clear_state(struct request *rq)
{
	rq->wbt_flags = 0; /* request가 NVMe SQ로 재발행되기 전 WBT 플래그 초기화 */		/* request 재사용/완료 시 WBT 플래그 초기화 */
}

static inline enum wbt_flags wbt_flags(struct request *rq)
{
	return rq->wbt_flags; /* request가 속한 NVMe SQ CID 그룹 반환 */		/* request가 어떤 WBT 그룹에 속하는지 반환 */
}

static inline bool wbt_is_tracked(struct request *rq)
{
	return rq->wbt_flags & WBT_TRACKED; /* NVMe SQ로 발행할 tracked 쓰기/trim CID 여부 */	/* NVMe SQ로 발행할 쓰기/trim CID 여부 */
}

static inline bool wbt_is_read(struct request *rq)
{
	return rq->wbt_flags & WBT_READ; /* NVMe read CID 여부 */	/* NVMe read CID 여부 */
}

/*
 * WBT queue depth 기본값 및 샘플링 정책.
 * NVMe SSD에서 SQ의 물리적 깊이는 보통 64~1024이지만,
 * WBT는 소프트웨어 추정값인 rq_depth.max_depth를 1~RWB_DEF_DEPTH
 * 사이에서 2의 거듭제곱 단위로 조절한다.
 */
enum {
	/*
	 * Default setting, we'll scale up (to 75% of QD max) or down (min 1)
	 * from here depending on device stats
	 */
	RWB_DEF_DEPTH	= 16,

	/*
	 * 100msec window
	 */
	RWB_WINDOW_NSEC		= 100 * 1000 * 1000ULL,

	/*
	 * Disregard stats, if we don't meet this minimum
	 */
	RWB_MIN_WRITE_SAMPLES	= 3,

	/*
	 * If we have this number of consecutive windows without enough
	 * information to scale up or down, slowly return to center state
	 * (step == 0).
	 */
	RWB_UNKNOWN_BUMP	= 5,
};

static inline bool rwb_enabled(struct rq_wb *rwb)
{
	return rwb && rwb->enable_state != WBT_STATE_OFF_DEFAULT && /* WBT 비활성 시 NVMe SQ doorbell 제한 해제 */
		      rwb->enable_state != WBT_STATE_OFF_MANUAL;
}

/*
 * wb_timestamp()
 * 목적: WBT가 활성화된 경우 현재 jiffies를 기록한다.
 * 호출 경로: wbt_issue -> wb_timestamp (read 발행 시)
 *            wbt_done  -> wb_timestamp (read 완료 시)
 * NVMe 연결: NVMe read CID의 발행/완료 시각을 기록하여 최근 IO 활동을
 *            판별하고, close_io()에서 100ms 이내 경쟁 IO 존재 여부를
 *            결정하는 데 사용한다.
 */
static void wb_timestamp(struct rq_wb *rwb, unsigned long *var)
{
	if (rwb_enabled(rwb)) { /* WBT가 NVMe SQ 스로틀링 중일 때만 시각 기록 */
		const unsigned long cur = jiffies; /* doorbell을 칠 수 있는 현재 시점(jiffies) 캡처 */

		if (cur != *var) /* 동일 jiffy 중복 기록 방지: SQ 이벤트 카운터 정밀도 보존 (추정) */
			*var = cur; /* last_issue/last_comp 갱신 -> NVMe read CID 활동 추적 */
	}
}

/*
 * wb_recent_wait()
 * 목적: balance_dirty_pages()에서 최근 1초 이내에 task가 rate throttled
 *       된 적이 있는지 확인한다.
 * 호출 경로: get_limit -> wb_recent_wait
 *            wbt_rqw_done -> wb_recent_wait
 * NVMe 연결: 쓰기 폭주로 인해 상위 층(dirty page 관리)에서 이미 대기한
 *            경우, NVMe SQ를 더 적극적으로 활용할 수 있도록 한도를
 *            완화한다(추정).
 */
static bool wb_recent_wait(struct rq_wb *rwb)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* bdi: NVMe 디스크의 dirty page 쓰기 제어 정보 */

	return time_before(jiffies, bdi->last_bdp_sleep + HZ); /* 1초 내 rate-limit 대기 이력 -> SQ 여유 허용 판단 (추정) */
}

/*
 * get_rq_wait()
 * 목적: bio의 WBT 플래그에 해당하는 rq_wait 배열 원소를 반환한다.
 * 호출 경로: __wbt_wait, __wbt_done
 * NVMe 연결: swap/discard/background 쓰기별로 별도의 inflight 카운터를
 *            운영하여 NVMe SQ의 CID 발행 한도를 그룹별로 제어한다.
 */
static inline struct rq_wait *get_rq_wait(struct rq_wb *rwb,
					  enum wbt_flags wb_acct)
{
	if (wb_acct & WBT_SWAP) /* swapout CID: 별도 rq_wait 그룹으로 SQ 한도 분리 */
		return &rwb->rq_wait[WBT_RWQ_SWAP]; /* swap용 inflight 카운터 선택 */
	else if (wb_acct & WBT_DISCARD) /* NVMe Deallocate CID 그룹 분기 */
		return &rwb->rq_wait[WBT_RWQ_DISCARD]; /* discard용 inflight 카운터 선택 */

	return &rwb->rq_wait[WBT_RWQ_BG]; /* 일반 background write CID 그룹 */
}

/*
 * rwb_wake_all()
 * 목적: scale_up 등으로 한도가 늘어났을 때 대기 중인 태스크를 모두 깨운다.
 * 호출 경로: scale_up -> rwb_wake_all
 *            wbt_update_limits -> rwb_wake_all
 * NVMe 연결: NVMe SQ에 여유가 생기면 doorbell을 더 적극적으로 칠 수 있도록
 *            sleep 상태의 submit 경로를 재개한다.
 */
static void rwb_wake_all(struct rq_wb *rwb)
{
	int i;

	for (i = 0; i < WBT_NUM_RWQ; i++) { /* NVMe SQ CID 그룹(bg/swap/discard) 순회하며 대기 태스크 깨움 */
		struct rq_wait *rqw = &rwb->rq_wait[i]; /* i번째 그룹의 inflight 대기 큐 */

		if (wq_has_sleeper(&rqw->wait)) /* 해당 그룹에서 sleep 중인 submit 경로 존재 확인 */
			wake_up_all(&rqw->wait); /* NVMe SQ 여유 발생 -> doorbell을 칠 태스크 일제 깨움 */
	}
}

/*
 * wbt_rqw_done()
 * 목적: request 완료 시 해당 rq_wait 그룹의 inflight를 감소시키고,
 *       한도 이하로 낮아지면 대기 태스크를 깨운다.
 * 호출 경로: __wbt_done -> wbt_rqw_done
 *            wbt_cleanup_cb -> wbt_rqw_done
 * NVMe 연결: NVMe CQ 처리가 끝나고 request가 free되기 전에 호출되어
 *            SQ에 남은 CID 개수를 갱신한다.
 */
static void wbt_rqw_done(struct rq_wb *rwb, struct rq_wait *rqw,
			 enum wbt_flags wb_acct)
{
	int inflight, limit; /* inflight: NVMe SQ에 남은 CID 수, limit: wake 임계값 */

	inflight = atomic_dec_return(&rqw->inflight); /* CQ 완료 후 원자적 CID 회계: doorbell 재발행 가능 신호 (추정) */

	/*
	 * For discards, our limit is always the background. For writes, if
	 * the device does write back caching, drop further down before we
	 * wake people up.
	 */
	if (wb_acct & WBT_DISCARD) /* discard CID는 배경 한도로만 제한 */
		limit = rwb->wb_background; /* discard용 NVMe SQ depth 한도 */
	else if (blk_queue_write_cache(rwb->rqos.disk->queue) && /* NVMe write cache/BBU 존재 시 완료 경로 특수화 (추정) */
		 !wb_recent_wait(rwb)) /* 최근 상위 dirty 대기 없으면 완료 즉시 깨우지 않음 */
		limit = 0; /* write cache가 버퍼링 -> doorbell 제한 완화, wake 억제 (추정) */	/* write cache가 있고 최근 대기 없으면 깨우지 않음(추정) */
	else
		limit = rwb->wb_normal; /* 일반 쓰기 CID 그룹의 SQ 한도 */

	/*
	 * Don't wake anyone up if we are above the normal limit.
	 */
	if (inflight && inflight >= limit) /* NVMe SQ inflight가 한도 이상이면 wake 불필요 */
		return;

	if (wq_has_sleeper(&rqw->wait)) {
		int diff = limit - inflight; /* NVMe SQ에 추가 가능한 CID 여유량 계산 */

		if (!inflight || diff >= rwb->wb_background / 2) /* SQ가 비었거나 여유 충분할 때만 일제 깨움 */
			wake_up_all(&rqw->wait); /* submit 태스크 재개 -> nvme_submit_cmd(doorbell) 가능 (추정) */
	}
}

/*
 * __wbt_done()
 * 목적: WBT_TRACKED 플래그가 설정된 request 완료 시 inflight 감소를 처리한다.
 * 호출 경로: wbt_done -> __wbt_done
 *            wbt_cleanup -> __wbt_done
 * NVMe 연결: NVMe SQ에서 회수된 CID에 대해 blk-rq-qos의 accounting을
 *            정리한다.
 */
static void __wbt_done(struct rq_qos *rqos, enum wbt_flags wb_acct)
{
	struct rq_wb *rwb = RQWB(rqos); /* rq_qos -> rq_wb: NVMe SQ QoS 상태 획득 */
	struct rq_wait *rqw; /* CID 그룹별 inflight 대기 큐 포인터 */

	if (!(wb_acct & WBT_TRACKED)) /* WBT가 추적하지 않는 read 등은 회계 걸러냄 */
		return; /* NVMe SQ inflight 변경 없음 */

	rqw = get_rq_wait(rwb, wb_acct); /* bio/CID 그룹에 맞는 rq_wait 선택 */
	wbt_rqw_done(rwb, rqw, wb_acct); /* CQ 완료에 따른 inflight 감소 및 wake 처리 */
}

/*
 * wbt_done()
 * 목적: request가 완료되거나 병합/해제될 때 WBT 상태를 정리한다.
 * 호출 경로: rq_qos_done -> __rq_qos_done -> wbt_done
 *            (NVMe CQ 처리 후 blk_mq_end_request 경로)
 * NVMe 연결: NVMe ISR -> nvme_process_cq -> blk_mq_complete_request ->
 *            rq_qos_done -> wbt_done 순으로 도달한다.
 */
static void wbt_done(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */

	if (!wbt_is_tracked(rq)) { /* NVMe SQ로 발행된 tracked CID가 아닌 경우 */
		if (wbt_is_read(rq)) { /* NVMe read CID 완료 경로 */
			if (rwb->sync_cookie == rq) { /* sync read CID 완료 시 추적 쿠키 일치 */
				rwb->sync_issue = 0; /* sync read CID 발행 시각 추적 해제 */
				rwb->sync_cookie = NULL; /* sync cookie 해제: CQ 처리 완료 (비역참조) */
			}

			wb_timestamp(rwb, &rwb->last_comp); /* read CID CQ 완료 시각 기록 */
		}
	} else {
		WARN_ON_ONCE(rq == rwb->sync_cookie); /* tracked request가 sync_cookie인 경우 버그 감지 */
		__wbt_done(rqos, wbt_flags(rq)); /* tracked CID inflight 감소: SQ 회계 정리 */
	}
	wbt_clear_state(rq); /* request 재사용 전 WBT 플래그 초기화 */
}

static inline bool stat_sample_valid(struct blk_rq_stat *stat)
{
	/*
	 * We need at least one read sample, and a minimum of
	 * RWB_MIN_WRITE_SAMPLES. We require some write samples to know
	 * that it's writes impacting us, and not just some sole read on
	 * a device that is in a lower power state.
	 */
	return (stat[READ].nr_samples >= 1 && /* read 샘플 1개 이상: NVMe read CID 완료 데이터 존재 */
		stat[WRITE].nr_samples >= RWB_MIN_WRITE_SAMPLES); /* write 샘플 3개 이상: NVMe SQ 쓰기 부하 판단 기준 */
}

/*
 * rwb_sync_issue_lat()
 * 목적: sync read request가 발행된 후 현재까지 경과한 시간을 반환한다.
 * 호출 경로: latency_exceeded -> rwb_sync_issue_lat
 * NVMe 연결: 특정 sync read CID가 NVMe SQ에 오래 머물러 있는 경우를
 *            감지하여 completion 없이도 latency 위반이라고 판단한다.
 */
static u64 rwb_sync_issue_lat(struct rq_wb *rwb)
{
	u64 issue = READ_ONCE(rwb->sync_issue); /* sync read CID 발행 시각 획득: READ_ONCE로 메모리 순서 보장 */

	if (!issue || !rwb->sync_cookie) /* sync read CID가 NVMe SQ에 발행되지 않았거나 쿠키 없음 */
		return 0; /* 체류 시간 0 -> latency 위반이 아님 */

	return blk_time_get_ns() - issue; /* 현재 시각 - doorbell 시각 = CID SQ 체류 시간 (추정) */
}

/*
 * wbt_inflight()
 * 목적: 세 rq_wait 그룹의 현재 inflight 합계를 반환한다.
 * 호출 경로: latency_exceeded, wb_timer_fn
 * NVMe 연결: NVMe SQ에 현재 발행되어 완료되지 않은 CID 총수를 소프트웨어
 *            측정값으로 추정한다.
 */
static inline unsigned int wbt_inflight(struct rq_wb *rwb)
{
	unsigned int i, ret = 0; /* 세 CID 그룹의 inflight 합산용 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* bg/swap/discard 그룹의 NVMe SQ inflight 순회 */
		ret += atomic_read(&rwb->rq_wait[i].inflight); /* 원자적으로 각 그룹 CID 개수 읽기: CQ 완료와 race 방지 */

	return ret; /* NVMe SQ 추정 동시 발행 CID 총수 반환 */
}

/* latency_exceeded() 반환 상태: NVMe SQ의 부하 상태를 나타낸다 */
enum {
	LAT_OK = 1,		/* NVMe SQ latency가 목표 내에 있음 */
	LAT_UNKNOWN,		/* 샘플 부족으로 판단 불가 */
	LAT_UNKNOWN_WRITES,	/* read 샘플 없이 쓰기만 진행 중 */
	LAT_EXCEEDED,		/* NVMe SQ latency가 목표를 초과함 */
};

/*
 * latency_exceeded()
 * 목적: blk-stat이 수집한 완료 latency 샘플과 sync read 발행 시각을
 *       바탕으로 NVMe SQ의 부하가 목표 latency를 초과했는지 판정한다.
 * 호출 경로: wb_timer_fn -> latency_exceeded
 * NVMe 연결: NVMe CQ에서 수집된 read/write 완료 지연을 분석하여
 *            SQ 포화(추정) 여부를 판단하고 queue depth를 조절할지
 *            결정한다.
 */
static int latency_exceeded(struct rq_wb *rwb, struct blk_rq_stat *stat)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi; /* NVMe 디스크 dirty 제어 객체 */
	struct rq_depth *rqd = &rwb->rq_depth; /* NVMe SQ에 대응하는 소프트웨어 depth 상태 */
	u64 thislat; /* sync read CID의 SQ 체류/완료 지연 */
	/*
	 * If our stored sync issue exceeds the window size, or it
	 * exceeds our min target AND we haven't logged any entries,
	 * flag the latency as exceeded. wbt works off completion latencies,
	 * but for a flooded device, a single sync IO can take a long time
	 * to complete after being issued. If this time exceeds our
	 * monitoring window AND we didn't see any other completions in that
	 * window, then count that sync IO as a violation of the latency.
	 */
	thislat = rwb_sync_issue_lat(rwb); /* sync read CID가 NVMe SQ/CQ에서 머문 시간 */
	if (thislat > rwb->cur_win_nsec || /* 모니터링 윈도우보다 오래 체류: SQ 포화 신호 (추정) */
	    (thislat > rwb->min_lat_nsec && !stat[READ].nr_samples)) { /* read 샘플 없이 sync read만 지연 -> doorbell 폭주 의심 (추정) */
		trace_wbt_lat(bdi, thislat); /* wbt trace: NVMe SQ latency 위반 기록 */
		return LAT_EXCEEDED; /* NVMe SQ 부하 초과 -> scale_down 유도 */
	}

	/*
	 * No read/write mix, if stat isn't valid
	 */
	if (!stat_sample_valid(stat)) { /* 충분한 read/write CQ 샘플이 없으면 판단 보류 */
		/*
		 * If we had writes in this stat window and the window is
		 * current, we're only doing writes. If a task recently
		 * waited or still has writes in flights, consider us doing
		 * just writes as well.
		 */
		if (stat[WRITE].nr_samples || wb_recent_wait(rwb) || /* write 샘플 존재 또는 상위 대기/인플라이트 -> 쓰기만 진행 중 (추정) */
		    wbt_inflight(rwb)) /* NVMe SQ에 미완료 CID 존재 */
			return LAT_UNKNOWN_WRITES; /* write-only 부하: 음수 step으로 성능 boost 가능 */
		return LAT_UNKNOWN; /* 판단 불가: NVMe SQ 상태 유지 */
	}

	/*
	 * If the 'min' latency exceeds our target, step down.
	 */
	if (stat[READ].min > rwb->min_lat_nsec) { /* NVMe read CID 최소 완료 latency가 목표 초과 */
		trace_wbt_lat(bdi, stat[READ].min); /* trace: read CQ latency 위반 */
		trace_wbt_stat(bdi, stat); /* trace: NVMe SQ 통계 기록 */
		return LAT_EXCEEDED; /* NVMe SQ latency 목표 초과 -> depth 감소 */
	}

	if (rqd->scale_step) /* scale_step이 0이 아니면 조정 중인 SQ depth 상태 */
		trace_wbt_stat(bdi, stat); /* trace: scale_step 중인 SQ 통계 기록 */

	return LAT_OK; /* NVMe SQ latency 양호 -> depth 증가 가능 */
}

static void rwb_trace_step(struct rq_wb *rwb, const char *msg)
{
	struct backing_dev_info *bdi = rwb->rqos.disk->bdi;
	struct rq_depth *rqd = &rwb->rq_depth;

	trace_wbt_step(bdi, msg, rqd->scale_step, rwb->cur_win_nsec, /* trace: NVMe SQ depth 조정 단계 기록 */
			rwb->wb_background, rwb->wb_normal, rqd->max_depth);
}

/*
 * calc_wb_limits()
 * 목적: rq_depth.max_depth로부터 wb_normal과 wb_background 한도를 계산한다.
 * 호출 경로: scale_up, scale_down, wbt_update_limits, wbt_init
 * NVMe 연결: NVMe SQ의 동시 발행 CID 상한(max_depth)이 바뀔 때마다
 *            일반/배경 쓰기용 한도를 재계산한다.
 */
static void calc_wb_limits(struct rq_wb *rwb)
{
	if (rwb->min_lat_nsec == 0) { /* WBT 목표 latency 0 -> SQ 스로틀링 비활성 */
		rwb->wb_normal = rwb->wb_background = 0; /* CID 발행 한도 0: doorbell 제한 없음 */
	} else if (rwb->rq_depth.max_depth <= 2) { /* NVMe SQ depth가 매우 작을 때 보수적 분배 */
		rwb->wb_normal = rwb->rq_depth.max_depth; /* 일반 쓰기에 전체 SQ depth 사용 */
		rwb->wb_background = 1; /* 배경 쓰기 최소 1 CID 보장 */
	} else {
		rwb->wb_normal = (rwb->rq_depth.max_depth + 1) / 2; /* 일반 쓰기: NVMe SQ depth의 절반 */
		rwb->wb_background = (rwb->rq_depth.max_depth + 3) / 4; /* 배경 쓰기: SQ depth의 1/4, read latency 우선 (추정) */
	}
}

/*
 * scale_up()
 * 목적: latency가 양호할 때 queue depth를 한 단계 증가시킨다.
 * 호출 경로: wb_timer_fn(LAT_OK 또는 LAT_UNKNOWN_WRITES) -> scale_up
 *            wb_timer_fn(LAT_UNKNOWN에서 step > 0) -> scale_up
 * NVMe 연결: NVMe SQ에 여유가 생겼다고 판단되면 동시 발행 CID 개수를
 *            늘려 doorbell throughput을 향상시킨다.
 */
static void scale_up(struct rq_wb *rwb)
{
	if (!rq_depth_scale_up(&rwb->rq_depth)) /* NVMe SQ max_depth 증가 실패 시(이미 최대) 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* CID 한도 재계산 -> doorbell 폭주 가능성 증가 */
	rwb->unknown_cnt = 0; /* latency 샘플 카운터 초기화 */
	rwb_wake_all(rwb); /* 대기 중인 submit 경로 깨워 doorbell 발행 재개 */
	rwb_trace_step(rwb, tracepoint_string("scale up")); /* trace: NVMe SQ depth 확대 기록 */
}

/*
 * scale_down()
 * 목적: latency가 초과되었을 때 queue depth를 한 단계 감소시킨다.
 * 호출 경로: wb_timer_fn(LAT_EXCEEDED) -> scale_down
 *            wb_timer_fn(LAT_UNKNOWN에서 step < 0) -> scale_down
 * NVMe 연결: NVMe SQ가 포화되어 latency가 증가하면 동시 발행 CID 개수를
 *            줄여 NAND 쓰기 버퍼나 SQ 포화를 완화한다.
 */
static void scale_down(struct rq_wb *rwb, bool hard_throttle)
{
	if (!rq_depth_scale_down(&rwb->rq_depth, hard_throttle)) /* NVMe SQ max_depth 감소 실패 시(최소 1) 조기 리턴 */
		return;
	calc_wb_limits(rwb); /* CID 한도 재계산 -> doorbell 빈도 감소 */
	rwb->unknown_cnt = 0; /* latency 샘플 카운터 초기화 */
	rwb_trace_step(rwb, tracepoint_string("scale down")); /* trace: NVMe SQ depth 축소 기록 */
}

/*
 * rwb_arm_timer()
 * 목적: 다음 latency 샘플링 타이머를 재설정한다.
 * 호출 경로: wbt_wait -> rwb_arm_timer
 *            wb_timer_fn -> rwb_arm_timer
 * NVMe 연결: scale_step이 양수이면 NVMe SQ가 이미 혼잡하므로 모니터링
 *            윈도우를 짧게 하여 더 빠르게 반응한다.
 */
static void rwb_arm_timer(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth;

	if (rqd->scale_step > 0) { /* SQ가 혼잡(scale_step>0) -> 더 짧은 윈도우로 빠른 반응 */
		/*
		 * We should speed this up, using some variant of a fast
		 * integer inverse square root calculation. Since we only do
		 * this for every window expiration, it's not a huge deal,
		 * though.
		 */
		rwb->cur_win_nsec = div_u64(rwb->win_nsec << 4, /* 기본 윈도우에 역제곱 승수 적용 */
					int_sqrt((rqd->scale_step + 1) << 8)); /* scale_step이 클수록 NVMe SQ 상태 모니터링 주기 축소 */
	} else {
		/*
		 * For step < 0, we don't want to increase/decrease the
		 * window size.
		 */
		rwb->cur_win_nsec = rwb->win_nsec; /* SQ 여유 시 기본 100ms 샘플링 윈도우 유지 */
	}

	blk_stat_activate_nsecs(rwb->cb, rwb->cur_win_nsec); /* blk-stat 콜백 재설정 -> NVMe CQ latency 샘플 타이머 (추정) */
}

/*
 * wb_timer_fn()
 * 목적: blk-stat 콜백으로 주기적으로 호출되어 latency 상태에 따라
 *       queue depth를 scale up/down한다.
 * 호출 경로: blk_stat_callback -> wb_timer_fn
 *            (blk-stat.c가 NVMe CQ 완료 이벤트를 집계한 후)
 * NVMe 연결: NVMe SQ의 평균/최소 완료 latency를 보고 scale_step을
 *            조정함으로써 향후 nvme_submit_cmd(doorbell)에 의해 발행될
 *            CID 개수를 간접적으로 제어한다.
 */
static void wb_timer_fn(struct blk_stat_callback *cb)
{
	struct rq_wb *rwb = cb->data; /* blk-stat 콜백에서 rq_wb 상태 복원 */
	struct rq_depth *rqd = &rwb->rq_depth; /* NVMe SQ depth 상태 */
	unsigned int inflight = wbt_inflight(rwb); /* 현재 NVMe SQ 추정 CID 개수 */
	int status;

	if (!rwb->rqos.disk) /* 디스크 해제 중이면 CQ 통계 처리 중단 */
		return; /* NVMe 컨트롤러 제거 시 wbt timer 종료 (추정) */

	status = latency_exceeded(rwb, cb->stat); /* NVMe CQ latency 샘플로 SQ 부하 판정 */

	trace_wbt_timer(rwb->rqos.disk->bdi, status, rqd->scale_step, inflight); /* trace: SQ 상태/인플라이트 기록 */

	/*
	 * If we exceeded the latency target, step down. If we did not,
	 * step one level up. If we don't know enough to say either exceeded
	 * or ok, then don't do anything.
	 */
	switch (status) {
	case LAT_EXCEEDED:
		scale_down(rwb, true); /* SQ latency 초과 -> 동시 발행 CID 수 감소 */
		break;
	case LAT_OK:
		scale_up(rwb); /* SQ latency 양호 -> doorbell 발행 한도 증가 */
		break;
	case LAT_UNKNOWN_WRITES:
		/*
		 * We don't have a valid read/write sample, but we do have
		 * writes going on. Allow step to go negative, to increase
		 * write performance.
		 */
		scale_up(rwb); /* write-only 부하 시 음수 step으로 SQ 활용 극대화 */
		break;
	case LAT_UNKNOWN:
		if (++rwb->unknown_cnt < RWB_UNKNOWN_BUMP) /* RWB_UNKNOWN_BUMP 회 연속 판단 불가 시 중앙 복귀 */
			break;
		/*
		 * We get here when previously scaled reduced depth, and we
		 * currently don't have a valid read/write sample. For that
		 * case, slowly return to center state (step == 0).
		 */
		if (rqd->scale_step > 0) /* 이전에 SQ depth가 축소된 상태에서 점진적 복원 */
			scale_up(rwb); /* depth 1단계 확대 -> doorbell 허용량 증가 */
		else if (rqd->scale_step < 0) /* 이전에 write-only boost 상태면 점진적 축소 */
			scale_down(rwb, false); /* 부드럽게 NVMe SQ depth 원위치 (hard_throttle=false) */
		break;
	default:
		break;
	}

	/*
	 * Re-arm timer, if we have IO in flight
	 */
	if (rqd->scale_step || inflight) /* SQ 조정 중이거나 inflight CID 있으면 타이머 재가동 */
		rwb_arm_timer(rwb); /* 다음 NVMe CQ latency 샘플링 예약 */
}

/*
 * wbt_update_limits()
 * 목적: queue depth가 변경되었을 때 scale 상태를 초기화하고 한도를
 *       재계산한다.
 * 호출 경로: wbt_init, wbt_set_min_lat, wbt_queue_depth_changed
 * NVMe 연결: NVMe 드라이버의 queue depth 갱신에 따라 rq_depth.max_depth를
 *            재설정하고 새로운 CID 발행 한도를 적용한다.
 */
static void wbt_update_limits(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth;

	rqd->scale_step = 0; /* NVMe SQ depth 중앙 상태로 리셋 */
	rqd->scaled_max = false; /* hardware max depth 미도달 플래그 초기화 */

	rq_depth_calc_max_depth(rqd); /* blk-mq queue depth 기반 NVMe SQ max_depth 계산 */
	calc_wb_limits(rwb); /* wb_normal/background CID 한도 재계산 */

	rwb_wake_all(rwb); /* 한도 변경으로 대기 중인 doorbell 경로 깨움 */
}

bool wbt_disabled(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* request_queue에서 WBT rq_qos 객체 검색 */

	return !rqos || !rwb_enabled(RQWB(rqos)); /* WBT가 꺼져 있으면 NVMe SQ doorbell 무제한 */
}

u64 wbt_get_min_lat(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* WBT QoS 핸들 획득 */
	if (!rqos)
		return 0; /* NVMe 디스크에 WBT가 설치되지 않은 경우 */
	return RQWB(rqos)->min_lat_nsec; /* 목표 latency 없음: SQ 제한 없음 */
} /* NVMe read CID 목표 완료 latency 반환 */

/*
 * wbt_set_min_lat()
 * 목적: sysfs 등에서 latency 목표값을 변경할 때 사용된다.
 * 호출 경로: wbt_set_lat -> wbt_set_min_lat
 * NVMe 연결: 목표 latency를 조정하면 NVMe SQ의 허용 부하 수준이
 *            재설정된다.
 */
static void wbt_set_min_lat(struct request_queue *q, u64 val)
{
	struct rq_qos *rqos = wbt_rq_qos(q); /* WBT QoS 핸들 */
	if (!rqos) /* WBT 미설치 시 무시 */
		return;

	RQWB(rqos)->min_lat_nsec = val; /* NVMe SQ 목표 latency 갱신 */
	if (val) /* latency 값이 0이 아니면 WBT 켬 */
		RQWB(rqos)->enable_state = WBT_STATE_ON_MANUAL; /* NVMe SQ 소프트웨어 스로틀링 활성화 */
	else
		RQWB(rqos)->enable_state = WBT_STATE_OFF_MANUAL; /* WBT off -> doorbell 제한 해제 */

	wbt_update_limits(RQWB(rqos)); /* 새 목표에 맞춰 CID 한도 재설정 */
}


/*
 * close_io()
 * 목적: 최근 100ms 이내에 read 발행 또는 완료가 있었는지 확인한다.
 * 호출 경로: get_limit -> close_io
 * NVMe 연결: NVMe read CID가 최근에 활발히 완료되었다면 쓰기를
 *            더 보수적으로 제한하여 read latency를 보호한다.
 */
static bool close_io(struct rq_wb *rwb)
{
	const unsigned long now = jiffies; /* 현재 시점(jiffies) */

	return time_before(now, rwb->last_issue + HZ / 10) || /* 100ms 내 read CID doorbell 발행 이력 */
		time_before(now, rwb->last_comp + HZ / 10); /* 100ms 내 read CID CQ 완료 이력 */
}

#define REQ_HIPRIO	(REQ_SYNC | REQ_META | REQ_PRIO | REQ_SWAP) /* NVMe SQ에서 우선 처리할 sync/meta/prio/swap bio 플래그 집합 */

/*
 * get_limit()
 * 목적: bio의 opf 플래그에 따라 현재 허용할 inflight CID 상한을 반환한다.
 * 호출 경로: wbt_inflight_cb -> get_limit
 * NVMe 연결: NVMe SQ에 동시에 발행할 수 있는 CID 개수를 bio 우선순위에
 *            따라 동적으로 결정한다.
 */
static inline unsigned int get_limit(struct rq_wb *rwb, blk_opf_t opf)
{
	unsigned int limit; /* 현재 bio가 사용할 NVMe SQ CID 상한 */

	if ((opf & REQ_OP_MASK) == REQ_OP_DISCARD) /* discard/Deallocate CID는 배경 한도로 제한 */
		return rwb->wb_background; /* NVMe Deallocate 동시 발행 개수 제한 */

	/*
	 * At this point we know it's a buffered write. If this is
	 * swap trying to free memory, or REQ_SYNC is set, then
	 * it's WB_SYNC_ALL writeback, and we'll use the max limit for
	 * that. If the write is marked as a background write, then use
	 * the idle limit, or go to normal if we haven't had competing
	 * IO for a bit.
	 */
	if ((opf & REQ_HIPRIO) || wb_recent_wait(rwb)) /* sync/swap 또는 상위 대기 이력 -> SQ 최대 활용 */
		limit = rwb->rq_depth.max_depth; /* NVMe SQ 최대 depth까지 CID 발행 허용 */	/* sync/swap: SQ 최대 활용 */
	else if ((opf & REQ_BACKGROUND) || close_io(rwb)) { /* 배경 쓰기 또는 최근 read 경쟁 있음 */
		/*
		 * If less than 100ms since we completed unrelated IO,
		 * limit us to half the depth for background writeback.
		 */
		limit = rwb->wb_background; /* read latency 보호를 위해 보수적 NVMe SQ 사용 */		/* 배경 쓰기: 보수적 한도 */
	} else
		limit = rwb->wb_normal; /* 일반 쓰기: 중간 NVMe SQ CID 한도 */			/* 일반 쓰기: 중간 한도 */

	return limit; /* bio -> request 변환 전 허용 CID 개수 반환 */
}

/*
 * struct wbt_wait_data: rq_qos_wait에 전달되는 콜백 데이터.
 * NVMe 연결: bio가 NVMe SQ로 들어가기 위해 rq_wait에서 대기할 때
 *            한도(limit)와 회계(wb_acct) 정보를 콜백에 전달한다.
 */
struct wbt_wait_data {
	struct rq_wb *rwb; /* NVMe SQ QoS 상태 포인터 */
	enum wbt_flags wb_acct; /* CID 그룹(read/write/swap/discard) 분류 */
	blk_opf_t opf; /* bio opf: REQ_SYNC/PRIO/background -> SQ CID 우선순위 (추정) */
};

/*
 * wbt_inflight_cb()
 * 목적: rq_qos_wait가 깨어날 때마다 inflight가 limit 미만이면
 *       카운터를 증가시키고 true를 반환한다.
 * 호출 경로: rq_qos_wait -> wbt_inflight_cb
 * NVMe 연결: NVMe SQ에 새 CID를 추가핏 수 있는지 원자적으로 검사한다.
 */
static bool wbt_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct wbt_wait_data *data = private_data; /* rq_qos_wait가 전달한 bio 정보 */
	return rq_wait_inc_below(rqw, get_limit(data->rwb, data->opf)); /* limit 미만이면 inflight CID 원자 증가 -> doorbell 진입 허용 */
}

/*
 * wbt_cleanup_cb()
 * 목적: rq_qos_wait에서 sleep이 취소되거나 실패한 경우 inflight를
 *       감소시킨다.
 * 호출 경로: rq_qos_wait -> wbt_cleanup_cb
 * NVMe 연결: NVMe SQ 진입이 중단된 경우 CID 회계를 롤백한다.
 */
static void wbt_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	struct wbt_wait_data *data = private_data; /* sleep 취소된 bio 정보 */
	wbt_rqw_done(data->rwb, rqw, data->wb_acct); /* NVMe SQ 진입 실패 시 CID 회계 롤백 */
}

/*
 * __wbt_wait()
 * 목적: inflight가 limit에 도달하면 bio를 대기시킨다.
 * 호출 경로: wbt_wait -> __wbt_wait
 * NVMe 연결: blk_mq_submit_bio -> blk_mq_get_request 이전에 실행되어
 *            NVMe SQ의 doorbell을 치기 전에 소프트웨어적으로 CID 발행을
 *            제어한다.
 */
static void __wbt_wait(struct rq_wb *rwb, enum wbt_flags wb_acct,
		       blk_opf_t opf)
{
	struct rq_wait *rqw = get_rq_wait(rwb, wb_acct); /* bio/CID 그룹별 대기 큐 선택 */
	struct wbt_wait_data data = {
		.rwb = rwb, /* NVMe SQ QoS 상태 전달 */
		.wb_acct = wb_acct, /* CID 그룹 전달 */
		.opf = opf, /* bio 플래그 전달 -> get_limit에서 SQ 우선순위 결정 */
	};

	rq_qos_wait(rqw, &data, wbt_inflight_cb, wbt_cleanup_cb); /* bio->request 변환 전 NVMe SQ inflight 한도 대기 (추정) */
}

/*
 * wbt_should_throttle()
 * 목적: 주어진 bio가 WBT 스로틀링 대상인지 판단한다.
 * 호출 경로: bio_to_wbt_flags -> wbt_should_throttle
 * NVMe 연결: NVMe SQ로 변환될 buffered write와 discard만 추적하고,
 *            O_DIRECT sync write는 스로틀링하지 않는다.
 */
static inline bool wbt_should_throttle(struct bio *bio)
{
	switch (bio_op(bio)) { /* bio opcode에 따라 NVMe SQ 스로틀링 대상 판별 */
	case REQ_OP_WRITE: /* buffered write CID */
		/*
		 * Don't throttle WRITE_ODIRECT
		 */
		if ((bio->bi_opf & (REQ_SYNC | REQ_IDLE)) == /* O_DIRECT sync write (REQ_SYNC|REQ_IDLE) 제외 */
		    (REQ_SYNC | REQ_IDLE)) /* O_DIRECT 경로는 doorbell 폭주 우려 낮음 */
			return false; /* NVMe SQ 스로틀링 대상 아님 */
		fallthrough; /* buffered write는 아래 return true로 */
	case REQ_OP_DISCARD: /* discard/trim CID */
		return true; /* NVMe SQ 동시 발행 제한 필요 */
	default:
		return false; /* read 등은 별도 추적/제한 없음 */
	}
}

/*
 * bio_to_wbt_flags()
 * 목적: bio의 특성에 맞는 WBT 플래그를 생성한다.
 * 호출 경로: wbt_wait, wbt_track, wbt_cleanup
 * NVMe 연결: bio가 NVMe SQ의 어떤 CID 그룹(read/write/swap/discard)에
 *            속할지 분류한다.
 */
static enum wbt_flags bio_to_wbt_flags(struct rq_wb *rwb, struct bio *bio)
{
	enum wbt_flags flags = 0; /* CID 그룹 플래그 초기화 */

	if (!rwb_enabled(rwb)) /* WBT 꺼져 있으면 NVMe SQ 제한 없음 */
		return 0; /* 추적 플래그 없음: doorbell 무제한 */

	if (bio_op(bio) == REQ_OP_READ) { /* NVMe read CID 분류 */
		flags = WBT_READ; /* read CID: latency 샘플링 대상 */
	} else if (wbt_should_throttle(bio)) { /* buffered write/discard CID */
		if (bio->bi_opf & REQ_SWAP) /* swapout으로 생성된 긴급 쓰기 CID */
			flags |= WBT_SWAP; /* swap 그룹으로 NVMe SQ 한도 분리 */
		if (bio_op(bio) == REQ_OP_DISCARD) /* discard CID */
			flags |= WBT_DISCARD; /* discard 그룹으로 SQ 한도 분리 */
		flags |= WBT_TRACKED; /* NVMe SQ inflight 추적 대상 표시 */
	}
	return flags; /* bio -> request에 복사될 WBT/CID 그룹 정보 */
}

/*
 * wbt_cleanup()
 * 목적: bio가 request로 변환되지 못하고 중단될 때 inflight를 감소시킨다.
 * 호출 경로: rq_qos_cleanup -> __rq_qos_cleanup -> wbt_cleanup
 * NVMe 연결: bio가 NVMe SQ 진입 전에 실패하거나 병합되어 request가
 *            생성되지 않은 경우 CID 회계를 정리한다.
 */
static void wbt_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */
	enum wbt_flags flags = bio_to_wbt_flags(rwb, bio); /* 실패한 bio의 CID 그룹 재판별 */
	__wbt_done(rqos, flags); /* bio가 NVMe SQ에 진입하지 못하면 inflight 롤백 */
}

/*
 * wbt_wait()
 * 목적: bio가 request로 변환되기 전에 WBT 스로틀링을 수행한다.
 * 호출 경로: blk_mq_submit_bio -> __rq_qos_throttle -> wbt_wait
 *            (blk-rq-qos.c의 throttle 훅을 통해)
 * NVMe 연결: 이 함수가 NVMe SQ의 doorbell을 치기 직전 마지막
 *            소프트웨어 큐 제어 지점이다. 필요 시 태스크를 대기시켜
 *            nvme_submit_cmd() 호출 빈도를 제한한다.
 */
/* May sleep, if we have exceeded the writeback limits. */
static void wbt_wait(struct rq_qos *rqos, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */
	enum wbt_flags flags; /* bio의 CID 그룹 */

	flags = bio_to_wbt_flags(rwb, bio); /* bio -> NVMe SQ CID 그룹 매핑 */
	if (!(flags & WBT_TRACKED)) { /* tracked write/discard가 아니면 제한 없음 */
		if (flags & WBT_READ) /* read CID 활동 기록 */
			wb_timestamp(rwb, &rwb->last_issue); /* read CID doorbell 발행 시각 기록 */
		return; /* writeback throttling 통과 */
	}

	__wbt_wait(rwb, flags, bio->bi_opf); /* CID 한도 도달 시 bio 대기 -> doorbell 지연 (추정) */

	if (!blk_stat_is_active(rwb->cb)) /* latency 샘플링 타이머 미가동 시 */
		rwb_arm_timer(rwb); /* NVMe CQ 완료 latency 모니터링 시작 */
}

/*
 * wbt_track()
 * 목적: bio가 request와 연결될 때 request->wbt_flags를 설정한다.
 * 호출 경로: blk_mq_submit_bio -> rq_qos_track -> wbt_track
 * NVMe 연결: request가 NVMe SQ의 CID로 발행되기 전에 WBT 추적 정보를
 *            request에 기록한다.
 */
static void wbt_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */
	rq->wbt_flags |= bio_to_wbt_flags(rwb, bio); /* request에 CID 그룹 기록 -> issue/done 회계 연결, 이후 merge는 PRP/SGL segment에 영향 (추정) */
}

/*
 * wbt_issue()
 * 목적: request가 실제로 하드웨어로 발행될 때 sync read 발행 시각을
 *       기록한다.
 * 호출 경로: blk_mq_issue_request -> rq_qos_issue -> wbt_issue
 *            -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * NVMe 연결: NVMe SQ로 doorbell을 치는 시점 직전에 호출되며, sync read
 *            CID가 얼마나 오래 발행되어 있는지 추적하여 latency 초과를
 *            더 빨리 감지한다.
 */
static void wbt_issue(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */

	if (!rwb_enabled(rwb)) /* WBT off면 sync read 추적 생략 */
		return; /* doorbell 직전 추가 동작 없음 */

	/*
	 * Track sync issue, in case it takes a long time to complete. Allows us
	 * to react quicker, if a sync IO takes a long time to complete. Note
	 * that this is just a hint. The request can go away when it completes,
	 * so it's important we never dereference it. We only use the address to
	 * compare with, which is why we store the sync_issue time locally.
	 */
	if (wbt_is_read(rq) && !rwb->sync_issue) { /* NVMe read CID이며 아직 sync 추적 중이 아닐 때 */
		rwb->sync_cookie = rq; /* sync read CID 식별용 쿠키 저장 (주소만 비교) */
		rwb->sync_issue = rq->io_start_time_ns; /* NVMe SQ doorbell 시각 기록: io_start_time_ns 사용 (추정) */
	}
}

/*
 * wbt_requeue()
 * 목적: request가 다시 큐로 들어갈 때 sync_cookie를 정리한다.
 * 호출 경로: blk_mq_requeue_request -> rq_qos_requeue -> wbt_requeue
 * NVMe 연결: NVMe SQ에서 회수된 CID가 재발행 대기열로 돌아가면
 *            발행 시각 추적을 초기화한다.
 */
static void wbt_requeue(struct rq_qos *rqos, struct request *rq)
{
	struct rq_wb *rwb = RQWB(rqos); /* NVMe SQ QoS 상태 */
	if (!rwb_enabled(rwb)) /* WBT off면 재발행 추적 불필요 */
		return; /* NVMe SQ 회계 유지 */
	if (rq == rwb->sync_cookie) { /* 재발행된 request가 sync read CID면 */
		rwb->sync_issue = 0; /* 이전 발행 시각 무효화 */
		rwb->sync_cookie = NULL; /* sync 쿠키 해제 -> 재발행 후 새로 측정 */
	}
}

/*
 * wbt_data_dir()
 * 목적: request의 데이터 방향을 blk-stat 콜백에 맞게 변환한다.
 * 호출 경로: wbt_alloc -> blk_stat_alloc_callback(wbt_data_dir, ...)
 * NVMe 연결: NVMe read/write CID의 완료 latency를 blk-stat의 READ/WRITE
 *            버킷에 분리하여 저장한다.
 */
static int wbt_data_dir(const struct request *rq)
{
	const enum req_op op = req_op(rq); /* request opcode -> NVMe command opcode 계열 */

	if (op == REQ_OP_READ) /* NVMe read command */
		return READ; /* read latency 버킷 */
	else if (op_is_write(op)) /* NVMe write/flush command 계열 */
		return WRITE; /* write latency 버킷 */

	/* don't account */
	return -1; /* discard/flush 등은 WBT latency 샘플에서 제외 */
}

/*
 * wbt_alloc()
 * 목적: rq_wb 구조체와 blk-stat 콜백을 할당한다.
 * 호출 경로: wbt_init_enable_default -> wbt_alloc
 *            wbt_set_lat -> wbt_alloc
 * NVMe 연결: NVMe queue depth 스로틀링을 위한 상태 객체를 생성하고,
 *            완료 latency 샘플링 콜백을 등록한다.
 */
static struct rq_wb *wbt_alloc(void)
{
	struct rq_wb *rwb = kzalloc_obj(*rwb); /* NVMe SQ QoS 상태 객체 할당 */

	if (!rwb) /* 메모리 부족 -> WBT 설치 실패 -> doorbell 제한 없음 */
		return NULL; /* 초기화 중단 */

	rwb->cb = blk_stat_alloc_callback(wb_timer_fn, wbt_data_dir, 2, rwb); /* NVMe CQ latency 샘플링 콜백 등록: READ/WRITE 2개 방향 */
	if (!rwb->cb) { /* blk-stat callback 할당 실패 */
		kfree(rwb); /* rq_wb 해제 */
		return NULL; /* WBT 계층 미설치 */
	}

	return rwb; /* NVMe SQ 스로틀링 객체 반환 */
}

static void wbt_free(struct rq_wb *rwb)
{
	blk_stat_free_callback(rwb->cb); /* NVMe CQ latency 샘플링 콜백 해제 */
	kfree(rwb); /* NVMe SQ QoS 상태 메모리 반환 */
}

/*
 * __wbt_enable_default()
 * 목적: 디스크가 blk-mq이고 CONFIG_BLK_WBT_MQ가 설정되어 있으며,
 *       WBT가 이미 활성화되어 있지 않다면 WBT를 기본 활성화할지
 *       결정한다.
 * 호출 경로: wbt_enable_default -> __wbt_enable_default
 *            wbt_init_enable_default -> __wbt_enable_default
 * NVMe 연결: NVMe 디스크가 등록될 때 blk-mq 큐에 대해 WBT QoS를
 *            기본적으로 활성화할 수 있는지 판단한다.
 */
static bool __wbt_enable_default(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* NVMe 디스크의 request_queue */
	struct rq_qos *rqos; /* 기존 WBT QoS 핸들 */
	bool enable = IS_ENABLED(CONFIG_BLK_WBT_MQ); /* blk-mq용 WBT 컴파일 옵션: NVMe multi-queue 사용 시 true */

	mutex_lock(&disk->rqos_state_mutex); /* rq_qos 상태 동기화: NVMe 디스크 등록/해제 race 방지 */

	if (blk_queue_disable_wbt(q)) /* NVMe 큐에서 WBT 금지 플래그 확인 (추정) */
		enable = false; /* WBT 미활성 -> doorbell 무제한 유지 */

	/* Throttling already enabled? */
	rqos = wbt_rq_qos(q); /* 이미 설치된 WBT QoS 검색 */
	if (rqos) { /* WBT가 이미 존재하면 */
		if (enable && RQWB(rqos)->enable_state == WBT_STATE_OFF_DEFAULT) /* 금지 해제되었고 기본 off 상태면 */
			RQWB(rqos)->enable_state = WBT_STATE_ON_DEFAULT; /* NVMe SQ 스로틀링 기본 활성화 */
		mutex_unlock(&disk->rqos_state_mutex); /* 상태 보호 해제 */
		return false; /* 추가 초기화 불필요 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* 상태 보호 해제 */

	/* Queue not registered? Maybe shutting down... */
	if (!blk_queue_registered(q)) /* NVMe 디스크 등록 전/해제 중이면 설치 불가 */
		return false; /* WBT 설치 중단 */

	if (queue_is_mq(q) && enable) /* blk-mq 큐이고 WBT가 활성화되어야 하면 */
		return true; /* NVMe SQ QoS 설치 진행 */
	return false; /* non-mq 또는 WBT off: 설치 안 함 */
}

void wbt_enable_default(struct gendisk *disk) /* NVMe 디스크에 WBT 기본 활성화 검사 (wrapper) */
{
	__wbt_enable_default(disk);
}
EXPORT_SYMBOL_GPL(wbt_enable_default); /* NVMe 드라이버 등에서 WBT 기본 활성화 심볼 노출 */

/*
 * wbt_init_enable_default()
 * 목적: WBT를 기본적으로 활성화할 조건이 충족되면 rq_wb를 할당하고
 *       초기화하며 debugfs를 등록한다.
 * 호출 경로: add_disk -> wbt_init_enable_default (디스크 등록 시)
 * NVMe 연결: NVMe 컨트롤러 초기화가 끝나고 gendisk가 추가될 때
 *            NVMe SQ 스로틀링 계층을 설치한다.
 */
void wbt_init_enable_default(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* NVMe 디스크 큐 */
	struct rq_wb *rwb;
	unsigned int memflags; /* debugfs lock 반환값 저장 */

	if (!__wbt_enable_default(disk)) /* WBT 설치 조건 미충족 */
		return; /* NVMe SQ 스로틀링 미설치 */

	rwb = wbt_alloc(); /* NVMe SQ QoS 상태 할당 */
	if (!rwb) /* 메모리 부족 */
		return; /* WBT 설치 실패 */

	if (wbt_init(disk, rwb)) { /* rq_qos 리스트 등록 및 초기화 */
		pr_warn("%s: failed to enable wbt\n", disk->disk_name); /* NVMe 디스크에서 WBT 활성화 실패 경고 */
		wbt_free(rwb); /* 할당된 객체 해제 */
		return; /* WBT 미설치, doorbell 제한 없음 */
	}

	memflags = blk_debugfs_lock(q); /* debugfs 등록 동기화 */
	blk_mq_debugfs_register_rq_qos(q); /* NVMe SQ QoS 상태 debugfs 노드 등록 */
	blk_debugfs_unlock(q, memflags); /* debugfs lock 해제 */
}

/*
 * wbt_default_latency_nsec()
 * 목적: 회전식/비회전식 저장 장치에 대한 기본 latency 목표를 반환한다.
 * 호출 경로: wbt_init -> wbt_default_latency_nsec
 *            wbt_set_lat -> wbt_default_latency_nsec
 * NVMe 연결: NVMe SSD(비회전식)의 경우 기본 2ms로 설정되어, SQ 포화
 *            상태에서 빠른 반응을 유도한다.
 */
static u64 wbt_default_latency_nsec(struct request_queue *q)
{
	/*
	 * We default to 2msec for non-rotational storage, and 75msec
	 * for rotational storage.
	 */
	if (blk_queue_rot(q)) /* 회전식/비회전식 디스크 구분: NVMe는 false */
		return 75000000ULL; /* HDD 기본 75ms (NVMe 아님) */
	return 2000000ULL; /* NVMe SSD 기본 2ms: SQ 포화 빠른 감지 */
}

/*
 * wbt_queue_depth_changed()
 * 목적: 큐의 하드웨어 queue depth가 변경되면 WBT 한도를 갱신한다.
 * 호출 경로: blk_mq_update_nr_hw_queues 등 -> rq_qos_queue_depth_changed
 *            -> wbt_queue_depth_changed
 * NVMe 연결: NVMe 컨트롤러가 SQ/CQ 개수를 변경하면 동일한 디스크의
 *            rq_depth.queue_depth를 새 값으로 맞춘다.
 */
static void wbt_queue_depth_changed(struct rq_qos *rqos)
{
	RQWB(rqos)->rq_depth.queue_depth = blk_queue_depth(rqos->disk->queue); /* NVMe 하드웨어 queue depth 변경 시 rq_depth 동기화 */
	wbt_update_limits(RQWB(rqos)); /* 변경된 SQ depth에 맞춰 CID 한도 재계산 */
}

/*
 * wbt_exit()
 * 목적: WBT QoS 계층을 제거하고 메모리를 해제한다.
 * 호출 경로: rq_qos_exit -> wbt_exit
 * NVMe 연결: NVMe 디스크가 제거되거나 모듈이 해제될 때 SQ 스로틀링
 *            상태를 정리한다.
 */
static void wbt_exit(struct rq_qos *rqos)
{
	struct rq_wb *rwb = RQWB(rqos); /* 제거할 NVMe SQ QoS 상태 */

	blk_stat_remove_callback(rqos->disk->queue, rwb->cb); /* NVMe CQ latency 콜백 제거 */
	wbt_free(rwb); /* QoS 상태 메모리 해제 */
}

/*
 * wbt_disable_default()
 * 목적: 기본적으로 활성화된 WBT를 비활성화한다.
 * 호출 경로: sysfs 또는 디스크 해제 경로
 * NVMe 연결: NVMe SQ의 소프트웨어 스로틀링을 끄고, 이후에는
 *            nvme_submit_cmd(doorbell)가 WBT에 의해 제한받지 않는다.
 */
void wbt_disable_default(struct gendisk *disk)
{
	struct rq_qos *rqos = wbt_rq_qos(disk->queue); /* WBT QoS 핸들 검색 */
	struct rq_wb *rwb; /* NVMe SQ QoS 상태 */
	if (!rqos) /* WBT 미설치 */
		return; /* 변경 없음 */
	mutex_lock(&disk->rqos_state_mutex); /* 상태 보호 */
	rwb = RQWB(rqos);
	if (rwb->enable_state == WBT_STATE_ON_DEFAULT) { /* 기본 on 상태일 때만 off로 전환 */
		blk_stat_deactivate(rwb->cb); /* NVMe CQ latency 샘플링 중단 */
		rwb->enable_state = WBT_STATE_OFF_DEFAULT; /* WBT off: doorbell 제한 해제 */
	}
	mutex_unlock(&disk->rqos_state_mutex); /* 상태 보호 해제 */
}
EXPORT_SYMBOL_GPL(wbt_disable_default); /* WBT 비활성화 심볼 노출 */

#ifdef CONFIG_BLK_DEBUG_FS
/*
 * debugfs 항목들: NVMe SQ 스로틀링 상태를 사용자 공간에 노출한다.
 * inflight, cur_win_nsec, min_lat_nsec 등은 NVMe SQ의 현재 부하와
 * 목표 latency를 모니터링하는 데 사용된다.
 */
static int wbt_curr_win_nsec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%llu\n", rwb->cur_win_nsec); /* 현재 NVMe SQ 모니터링 윈도우 노출 */
	return 0;
}

static int wbt_enabled_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%d\n", rwb->enable_state);
	return 0; /* WBT on/off: NVMe SQ 소프트웨어 스로틀링 상태 */
}

static int wbt_id_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%u\n", rqos->id); /* rq_qos ID: NVMe 디스크 내 QoS 식별자 */
	return 0;
}

static int wbt_inflight_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);
	int i;

	for (i = 0; i < WBT_NUM_RWQ; i++) /* NVMe SQ CID 그룹 순회 */
		seq_printf(m, "%d: inflight %d\n", i, /* 그룹별 NVMe SQ inflight CID 개수 노출 */
			   atomic_read(&rwb->rq_wait[i].inflight));
	return 0;
}

static int wbt_min_lat_nsec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%lu\n", rwb->min_lat_nsec);
	return 0; /* NVMe read CID 목표 완료 latency 노출 */
}

static int wbt_unknown_cnt_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%u\n", rwb->unknown_cnt); /* 연속 latency 샘플 부족 횟수 */
	return 0;
}

static int wbt_normal_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%u\n", rwb->wb_normal); /* 일반 쓰기 CID 상한 */
	return 0;
}

static int wbt_background_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_wb *rwb = RQWB(rqos);

	seq_printf(m, "%u\n", rwb->wb_background);
	return 0; /* 배경 쓰기 CID 상한 */
}

static const struct blk_mq_debugfs_attr wbt_debugfs_attrs[] = {
	{"curr_win_nsec", 0400, wbt_curr_win_nsec_show},
	{"enabled", 0400, wbt_enabled_show},
	{"id", 0400, wbt_id_show},
	{"inflight", 0400, wbt_inflight_show},
	{"min_lat_nsec", 0400, wbt_min_lat_nsec_show},
	{"unknown_cnt", 0400, wbt_unknown_cnt_show},
	{"wb_normal", 0400, wbt_normal_show},
	{"wb_background", 0400, wbt_background_show},
	{},
};
#endif

/*
 * wbt_rqos_ops: blk-rq-qos.c가 bio/request의 각 생명주기 단계에서
 * 호출하는 콜백 테이블.
 * NVMe 연결: NVMe SQ로 들어가는 bio/request가 submit/issue/done/complete
 * 단계를 거칠 때 WBT가 개입할 지점을 정의한다.
 */
static const struct rq_qos_ops wbt_rqos_ops = {
	.throttle = wbt_wait, /* bio -> request 변환 전: NVMe SQ CID 한도 대기 (추정) */
	.issue = wbt_issue, /* doorbell 직전: sync read CID 발행 시각 기록 */
	.track = wbt_track, /* request에 CID 그룹 기록 -> issue/done 연결 */
	.requeue = wbt_requeue, /* NVMe SQ 회수 후 재발행: sync cookie 정리 */
	.done = wbt_done, /* CQ 완료 후: inflight 감소 및 WBT 상태 정리 */
	.cleanup = wbt_cleanup, /* bio NVMe SQ 진입 실패 시 회계 롤백 */
	.queue_depth_changed = wbt_queue_depth_changed, /* NVMe SQ/CQ 개수 변경 시 CID 한도 재계산 */
	.exit = wbt_exit, /* NVMe 디스크 제거 시 QoS 해제 */
#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = wbt_debugfs_attrs, /* NVMe SQ 스로틀링 상태 debugfs 노출 */
#endif
};

/*
 * wbt_init()
 * 목적: rq_wb 구조체를 초기화하고 request_queue의 rq_qos 리스트에 등록한다.
 * 호출 경로: wbt_init_enable_default -> wbt_init
 *            wbt_set_lat -> wbt_init
 * NVMe 연결: NVMe 디스크에 WBT 계층을 설치하여, 이후
 *            blk_mq_submit_bio -> wbt_wait 경로가 활성화된다.
 */
static int wbt_init(struct gendisk *disk, struct rq_wb *rwb)
{
	struct request_queue *q = disk->queue; /* NVMe 디스크의 request_queue */
	int ret; /* 등록 결과 */
	int i; /* CID 그룹 순회 인덱스 */

	for (i = 0; i < WBT_NUM_RWQ; i++) /* bg/swap/discard 그룹별 wait queue 초기화 */
		rq_wait_init(&rwb->rq_wait[i]); /* inflight atomic 및 wait queue 설정 */

	rwb->last_comp = rwb->last_issue = jiffies; /* read CID 활동 기준 시각 초기화 */
	rwb->win_nsec = RWB_WINDOW_NSEC; /* NVMe CQ latency 샘플링 기본 100ms 윈도우 */
	rwb->enable_state = WBT_STATE_ON_DEFAULT; /* NVMe SQ 스로틀링 기본 활성 */
	rwb->rq_depth.default_depth = RWB_DEF_DEPTH; /* 초기 SQ 소프트웨어 depth 16 */
	rwb->min_lat_nsec = wbt_default_latency_nsec(q); /* NVMe SSD면 2ms 목표 latency */
	rwb->rq_depth.queue_depth = blk_queue_depth(q); /* NVMe 하드웨어 queue depth 연동 */
	wbt_update_limits(rwb); /* CID 한도 초기화 */

	/*
	 * Assign rwb and add the stats callback.
	 */
	mutex_lock(&q->rq_qos_mutex); /* request_queue의 rq_qos 리스트 보호 */
	ret = rq_qos_add(&rwb->rqos, disk, RQ_QOS_WBT, &wbt_rqos_ops); /* blk-rq-qos에 WBT 등록 -> bio 경로 연결 */
	mutex_unlock(&q->rq_qos_mutex); /* 리스트 보호 해제 */
	if (ret) /* 등록 실패 */
		return ret; /* WBT 설치 실패 */

	blk_stat_add_callback(q, rwb->cb); /* NVMe CQ 완료 latency 콜백 활성화 */
	return 0; /* WBT 설치 성공 */
}

/*
 * wbt_set_lat()
 * 목적: 사용자가 지정한 latency 값으로 WBT 목표를 설정하거나,
 *       -1이면 기본값으로 재설정한다.
 * 호출 경로: sysfs write_latency_store 등
 * NVMe 연결: NVMe SQ의 목표 완료 latency를 런타임에 변경하여
 *            향후 doorbell 발행 빈도에 대한 기준을 재설정한다.
 */
int wbt_set_lat(struct gendisk *disk, s64 val)
{
	struct request_queue *q = disk->queue; /* NVMe 디스크 큐 */
	struct rq_qos *rqos = wbt_rq_qos(q); /* 기존 WBT QoS 핸들 */
	struct rq_wb *rwb = NULL; /* 새 NVMe SQ QoS 상태 */
	unsigned int memflags; /* freeze/debugfs lock 상태 저장 */
	int ret = 0; /* 결과 */

	if (!rqos) { /* WBT가 아직 설치되지 않은 NVMe 디스크 */
		rwb = wbt_alloc(); /* NVMe SQ QoS 상태 신규 할당 */
		if (!rwb) /* 메모리 부족 */
			return -ENOMEM; /* WBT 설정 실패 */
	}

	/*
	 * Ensure that the queue is idled, in case the latency update
	 * ends up either enabling or disabling wbt completely. We can't
	 * have IO inflight if that happens.
	 */
	memflags = blk_mq_freeze_queue(q); /* IO 중단: WBT on/off 전 NVMe SQ/CQ 안전 상태 확보 */
	if (!rqos) { /* WBT 미설치 시 초기화 */
		ret = wbt_init(disk, rwb); /* NVMe SQ QoS 등록 */
		if (ret) { /* 등록 실패 */
			wbt_free(rwb); /* 객체 해제 */
			goto out; /* unfreeze 후 종료 */
		}
	}

	if (val == -1) /* 기본값으로 재설정 */
		val = wbt_default_latency_nsec(q); /* NVMe SSD면 2ms */
	else if (val >= 0) /* 사용자 지정 값(us) */
		val *= 1000ULL; /* us -> ns 변환: NVMe CQ latency 단위 */

	if (wbt_get_min_lat(q) == val) /* NVMe SQ 목표 latency 변화 없음 */
		goto out; /* 불필요한 quiesce 회피 */

	blk_mq_quiesce_queue(q); /* request 처리 일시 정지: NVMe SQ doorbell 발행 중단 (추정) */

	mutex_lock(&disk->rqos_state_mutex); /* WBT 상태 보호 */
	wbt_set_min_lat(q, val); /* 새 NVMe SQ 목표 latency 및 CID 한도 적용 */
	mutex_unlock(&disk->rqos_state_mutex); /* 상태 보호 해제 */

	blk_mq_unquiesce_queue(q); /* NVMe SQ doorbell 발행 재개 */
out:
	blk_mq_unfreeze_queue(q, memflags); /* 큐 동결 해제 */

	memflags = blk_debugfs_lock(q); /* debugfs 등록 보호 */
	blk_mq_debugfs_register_rq_qos(q); /* 변경된 NVMe SQ QoS 상태 debugfs 갱신 */
	blk_debugfs_unlock(q, memflags); /* debugfs lock 해제 */

	return ret; /* WBT latency 설정 결과 */
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
