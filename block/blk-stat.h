/* SPDX-License-Identifier: GPL-2.0 */
/*
 * block/blk-stat.h
 *
 * NVMe 입장에서 본 block layer 통계 인프라.
 *
 * 이 헤더는 struct request_queue 단위로 I/O 완료 지연 시간(latency)과
 * 크기(size)를 버킷(bucket) 단위로 수집·집계하는 콜백 메커니즘을 정의한다.
 * NVMe SSD에서 한 CID(command identifier)가 doorbell 발행(SQ)부터
 * completion 처리(CQ)까지 걸린 완료 시간을 측정하고, 이를 바탕으로
 * scheduler(bfq, mq-deadline)나 엔드유저 도구가 성능 분석/조정을
 * 수행할 수 있게 한다. 논리적으로 block/blk-mq.h, block/blk-mq.c의
 * request 할당/완료 경로 뒤에 위치하며, drivers/nvme/host/pci.c의
 * nvme_complete_rq -> blk_mq_complete_request 흐름에서 데이터가
 * 최종적으로 기록된다.
 */
#ifndef BLK_STAT_H
#define BLK_STAT_H

#include <linux/kernel.h>	/* NVMe: ktime/u64 등 커널 기본 타입; CQ entry time stamp 계산의 기반 */
#include <linux/blkdev.h>	/* NVMe: struct request_queue, request 정의; nvme_queue <-> request_queue 매핑에 사용 */
#include <linux/ktime.h>	/* NVMe: doorbell(SQ) 시각과 CQ 완료 시각의 차이(latency) 산출에 필수 */
#include <linux/rcupdate.h>	/* NVMe: callback list 순회 시 nvme reset/hotplug와의 RCU 일관성 보장 */
#include <linux/timer.h>	/* NVMe: latency 수집 윈도우 타이머; CQ 인터럽트와 별도 타이밍으로 발행 */

/**
 * struct blk_stat_callback - Block statistics callback.
 *
 * A &struct blk_stat_callback is associated with a &struct request_queue. While
 * @timer is active, that queue's request completion latencies are sorted into
 * buckets by @bucket_fn and added to a per-cpu buffer, @cpu_stat. When the
 * timer fires, @cpu_stat is flushed to @stat and @timer_fn is invoked.
 */
struct blk_stat_callback {
	/**
	 * @list: RCU list of callbacks for a &struct request_queue.
	 *
	 * NVMe: 하나의 nvme_queue(또는 struct request_queue)에 여러
	 * blk_stat_callback이 RCU 기반으로 연결된다. hotplug나 reset
	 * 상황에서도 안전하게 순회/제거할 수 있도록 보호된다.
	 */
	struct list_head list;		/* NVMe: request_queue->stats callbacks RCU list의 한 노드 (추정) */

	/**
	 * @timer: Timer for the next callback invocation.
	 */
	struct timer_list timer;	/* NVMe: latency 샘플링 윈도우 만료 시 timer_fn 발행; CQ ISR과 다른 CPU에서 실행 가능 */

	/**
	 * @cpu_stat: Per-cpu statistics buckets.
	 *
	 * NVMe: completion 인터럽트 핸들러가 동시에 여러 CPU에서
	 * 실행될 수 있으므로 per-cpu 버퍼에 먼저 누적한 뒤 timer가
	 * 발행할 때 @stat으로 병합한다. cacheline contention을
	 * 줄이는 핵심 설계이다.
	 */
	struct blk_rq_stat __percpu *cpu_stat;	/* NVMe: CQ affinity에 따른 per-CPU 완료 통계 버퍼; false sharing 방지 */

	/**
	 * @bucket_fn: Given a request, returns which statistics bucket it
	 * should be accounted under. Return -1 for no bucket for this
	 * request.
	 *
	 * NVMe: 요청의 크기, 방향(read/write), 우선순위 등을 보고
	 * 어느 latency bucket에 기록할지 결정한다. 예를 들어 4KB
	 * read가 어느 구간에 속하는지 판별해 SSD의 특정 I/O 패턴
	 * 분포를 추출할 때 사용된다.
	 */
	int (*bucket_fn)(const struct request *);	/* NVMe: request -> latency bucket 인덱스; CID/tag 정보는 직접 안 씀 (추정) */

	/**
	 * @buckets: Number of statistics buckets.
	 */
	unsigned int buckets;		/* NVMe: bucket 개수 = latency 구간 분해능; 4KB/128KB 등 구간별 NVMe 패턴 분석 폭 결정 */

	/**
	 * @stat: Array of statistics buckets.
	 *
	 * NVMe: timer_fn 발행 시 @cpu_stat가 병합되어 사용자에게
	 * 노출되는 최종 집계 배열이다. nvmecli나 sysfs 기반 모니터링
	 * 도구가 이 값을 근거로 평균/최대 latency 등을 산출한다.
	 */
	struct blk_rq_stat *stat;	/* NVMe: timer_fn 직전 cpu_stat 병합 대상; sysfs/scheduler에 노출되는 집계 배열 */

	/**
	 * @timer_fn: Callback function.
	 *
	 * NVMe: 수집 윈도우가 만료되면 이 콜백이 실행되어 scheduler
	 * (예: bfq)가 SSD의 최근 latency 분포를 확인하고, 다음
	 * nvme_queue_rq -> nvme_submit_cmd(doorbell)로 발행할 요청의
	 * 우선순위나 깊이(depth)를 조정할 수 있다.
	 */
	void (*timer_fn)(struct blk_stat_callback *);	/* NVMe: 윈도우 만료 콜백; bfq가 이를 통해 SQ depth/우선순위 조정 (추정) */

	/**
	 * @data: Private pointer for the user.
	 *
	 * NVMe: scheduler나 드라이버 전용 상태를 담는 opaque 포인터.
	 * 예를 들어 bfq는 자신의 queue 상태를 여기에 저장한다.
	 */
	void *data;			/* NVMe: bfq queue state 등 scheduler 전용 컨텍스트; NVMe 드라이버는 직접 사용 안 함 */

	/**
	 * @rcu: rcu list head
	 */
	struct rcu_head rcu;		/* NVMe: callback list 안전한 지연 해제(kfree_rcu); reset/모듈 제거 시 race 회피 */
};

/*
 * NVMe: struct request_queue 단위 통계 구조체를 할당한다.
 * 이 구조체는 blk-mq가 nvme_queue에 대한 request_queue를
 * 초기화할 때 함께 생성된다 (추정).
 */
struct blk_queue_stats *blk_alloc_queue_stats(void);	/* NVMe: nvme_alloc_queue() -> blk_mq_init_queue() 납부에서 호출 가능 (추정) */
void blk_free_queue_stats(struct blk_queue_stats *);	/* NVMe: request_queue 해제 시 nvme_queue cleanup 경로에서 호출 */

/*
 * NVMe: request 완료 시점에 현재 시간을 기록한다.
 * 호출 경로: nvme_irq -> nvme_process_cq -> nvme_complete_rq ->
 *           blk_mq_complete_request -> blk_stat_add(now).
 * 이 값은 이후 bucket_fn이 사용할 완료 latency 계산의 기반이 된다.
 */
void blk_stat_add(struct request *rq, u64 now);		/* NVMe: CQ entry timestamp(now)와 rq->start_time_ns 차이로 CID별 latency 산출 */

/* record time/size info in request but not add a callback */
/*
 * NVMe: 요청에 time/size 계산을 활성화하되, 별도의 통계 콜백은
 * 등록하지 않는다. 모든 I/O의 기본 accounting이 필요할 때
 * 사용된다. 예: 기본 latency 측정은 켜되 scheduler가 periodic
 * timer_fn은 필요 없는 경우 (추정).
 */
void blk_stat_enable_accounting(struct request_queue *q);	/* NVMe: q->stats 계수 활성화; 모든 NVMe I/O의 time/size 기록 시작 */
void blk_stat_disable_accounting(struct request_queue *q);	/* NVMe: q->stats 계수 비활성화; suspend/reset 시 통계 오버헤드 차단 */

/**
 * blk_stat_alloc_callback() - Allocate a block statistics callback.
 * @timer_fn: Timer callback function.
 * @bucket_fn: Bucket callback function.
 * @buckets: Number of statistics buckets.
 * @data: Value for the @data field of the &struct blk_stat_callback.
 *
 * See &struct blk_stat_callback for details on the callback functions.
 *
 * Return: &struct blk_stat_callback on success or NULL on ENOMEM.
 *
 * NVMe: scheduler나 드라이버가 NVMe I/O latency 분포를 주기적으로
 * 받아볼 수 있는 통계 콜백 객체를 생성한다. 이후
 * blk_stat_add_callback()을 통해 특정 request_queue(즉, nvme_queue에
 * 대응)에 연결한다. 실패 시 NVMe 장치 초기화는 ENOMEM으로
 * 중단될 수 있다.
 */
struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data);	/* NVMe: per-nvme_queue latency 분석 객체 할당; ENOMEM 시 nvme probe 실패 가능 */

/**
 * blk_stat_add_callback() - Add a block statistics callback to be run on a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * Note that a single &struct blk_stat_callback can only be added to a single
 * &struct request_queue.
 *
 * NVMe: 생성된 blk_stat_callback을 nvme_queue의 request_queue에
 * 장착한다. 이후 해당 queue로 발행된 모든 I/O의 완료 통계가
 * 이 콜백의 cpu_stat에 누적된다. 호출 예:
 * bfq-iosched.c -> bfq_add_request -> blk_stat_add_callback.
 */
void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb);	/* NVMe: request_queue <-> nvme_queue 매핑에 callback 연결; 이후 모든 CID 완료 기록 */

/**
 * blk_stat_remove_callback() - Remove a block statistics callback from a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * When this returns, the callback is not running on any CPUs and will not be
 * called again unless readded.
 *
 * NVMe: nvme_queue 해제 전이나 scheduler 전환 시 콜백을 안전하게
 * 제거한다. timer_delete_sync()를 통해 inflight timer_fn이
 * 완료될 때까지 대기하므로 race 없이 제거할 수 있다.
 */
void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb);	/* NVMe: queue 제거/scheduler 전환 시 RCU list에서 안전 분리; inflight timer_fn 동기화 */

/**
 * blk_stat_free_callback() - Free a block statistics callback.
 * @cb: The callback.
 *
 * @cb may be NULL, in which case this does nothing. If it is not NULL, @cb must
 * not be associated with a request queue. I.e., if it was previously added with
 * blk_stat_add_callback(), it must also have been removed since then with
 * blk_stat_remove_callback().
 *
 * NVMe: blk_stat_remove_callback() 이후 per-cpu 메모리와 stat
 * 배열, callback 구조체 자체를 해제한다. NVMe reset이나 모듈
 * 제거 시 cleanup 경로에서 호출된다.
 */
void blk_stat_free_callback(struct blk_stat_callback *cb);	/* NVMe: per-cpu stat 배열과 callback 객체 메모리 반납; reset/모듈 exit 경로 */

/**
 * blk_stat_is_active() - Check if a block statistics callback is currently
 * gathering statistics.
 * @cb: The callback.
 *
 * Returns: %true iff the callback is active.
 *
 * NVMe: NVMe 장치가 활성 수집 윈도우를 가지고 있는지 확인한다.
 * 활성화된 경우에만 completion 경로에서 cpu_stat에 기록하므로,
 * 이 값은 불필요한 통계 오버헤드를 피하는 스위치로 작동한다.
 */
static inline bool blk_stat_is_active(struct blk_stat_callback *cb)
{
	/* NVMe: timer pending = 수집 윈도우 활성; CQ ISR에서 빠른 체크 후 cpu_stat 기록 여부 결정 */
	return timer_pending(&cb->timer);
}

/**
 * blk_stat_activate_nsecs() - Gather block statistics during a time window in
 * nanoseconds.
 * @cb: The callback.
 * @nsecs: Number of nanoseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 *
 * NVMe: nsecs 단위의 짧은 윈도우 동안 NVMe I/O latency를 수집한다.
 * mod_timer()로 timer를 설정하면 timer_fn이 지정 시간 후에
 * 발행되어, 해당 기간의 latency 분포를 분석할 수 있다.
 */
static inline void blk_stat_activate_nsecs(struct blk_stat_callback *cb,
					   u64 nsecs)
{
	/* HZ 기반 jiffies로 변환: 고해상도 타이머를 사용하지만
	 * 실제 정밀도는 HZ와 rounding에 좌우됨 (추정). */
	mod_timer(&cb->timer, jiffies + nsecs_to_jiffies(nsecs));	/* NVMe: nsecs 단위 샘플링 윈도우 시작; timer_fn이 만료 후 SQ/CQ 전략 재조정 */
}

/**
 * blk_stat_deactivate() - Disable the statistics timer.
 * @cb: The callback.
 *
 * NVMe: NVMe queue reset, suspend, 또는 통계 수집 중단 시 타이머를
 * 안전하게 제거한다. timer_delete_sync()는 다른 CPU에서 실행 중인
 * timer_fn이 끝날 때까지 기다리므로 RCU/통계 일관성을 보장한다.
 */
static inline void blk_stat_deactivate(struct blk_stat_callback *cb)
{
	timer_delete_sync(&cb->timer);	/* NVMe: 타이머 동기 중지; CQ ISR과 timer_fn 간 race 및 RCU 일관성 보장 */
}

/**
 * blk_stat_activate_msecs() - Gather block statistics during a time window in
 * milliseconds.
 * @cb: The callback.
 * @msecs: Number of milliseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 *
 * NVMe: msecs 단위의 상대적으로 긴 윈도우 동안 SSD latency를
 * 모니터링할 때 사용된다. scheduler가 주기적으로 평균 latency를
 * 확인하고 SQ/CQ 발행 전략을 조정하는 데 활용된다.
 */
static inline void blk_stat_activate_msecs(struct blk_stat_callback *cb,
					   unsigned int msecs)
{
	mod_timer(&cb->timer, jiffies + msecs_to_jiffies(msecs));	/* NVMe: msecs 단위 장기 윈도우; scheduler가 평균 latency 보고 SQ depth 조정 */
}

/*
 * NVMe: 단일 struct blk_rq_stat에 새로 측정된 latency 샘플을
 * 추가한다. blk_stat_add() 낙부에서 요청이 속한 bucket별로
 * 호출되며, min/max/mean/nr_samples 등을 갱신한다.
 */
void blk_rq_stat_add(struct blk_rq_stat *, u64);	/* NVMe: CID 완료 latency 샘플을 bucket에 누적; min/max/mean 갱신 */

/*
 * NVMe: src per-cpu bucket의 통계를 dst로 병합한다.
 * timer_fn 발행 직전에 blk_stat_add_callback()이 등록된
 * callback의 cpu_stat -> stat 병합 과정에서 사용된다.
 */
void blk_rq_stat_sum(struct blk_rq_stat *, struct blk_rq_stat *);	/* NVMe: per-CPU cpu_stat[] -> 공유 stat[] 병합; timer_fn 발행 직전 수행 */

/*
 * NVMe: 통계 bucket을 초기화한다. 새 윈도우 시작 전에
 * min/max/mean/nr_samples 값을 리셋하여 이전 윈도우의
 * 데이터가 다음 분석에 영향을 주지 않도록 한다.
 */
void blk_rq_stat_init(struct blk_rq_stat *);		/* NVMe: 새 latency 윈도우 시작 전 bucket 리셋; 이전 NVMe I/O 패턴 데이터 무효화 */

#endif

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 struct request_queue(nvme_queue에 매핑됨) 단위로
 *   I/O 완료 latency를 per-cpu 버킷에 수집하고, 주기적으로
 *   timer_fn을 통해 scheduler/드라이버에 분석 결과를 전달한다.
 * - 핵심 호출 경로:
 *   nvme_submit_cmd(doorbell) -> ... -> nvme_irq ->
 *   nvme_process_cq -> nvme_complete_rq -> blk_mq_complete_request ->
 *   blk_stat_add(now) -> bucket_fn -> cpu_stat 누적 ->
 *   timer 발행 -> blk_rq_stat_sum() -> timer_fn.
 * - bfq 등 scheduler는 blk_stat_alloc_callback() /
 *   blk_stat_add_callback()으로 콜백을 등록하고, 수집된 latency
 *   분포를 바탕으로 이후 nvme_queue_rq -> nvme_submit_cmd로
 *   발행할 I/O의 우선순위/깊이를 조정한다.
 * - blk_stat_enable_accounting()은 콜백 없이도 request에
 *   time/size 정보를 기록할 수 있게 해, 모든 NVMe I/O에 대한
 *   기본 accounting을 보장한다.
 * - 이 파일은 block/blk-mq.c의 request 할당/완료 흐름 뒤에
 *   위치하며, drivers/nvme/host/pci.c의 CQ 처리 경로와 직접
 *   연결되어 NVMe SSD 성능 데이터의 최종 집계 지점 역할을 한다.
 */
