// SPDX-License-Identifier: GPL-2.0
/*
 * Block stat tracking code
 *
 * Copyright (C) 2016 Jens Axboe
 */
/*
 * NVMe SSD 관점 파일 요약:
 *   본 파일은 Linux 블록 계층의 I/O 지연 시간(latency) 통계 수집 인프라를 구현한다.
 *   request_queue 단위로 bucket 단위 통계를 관리하며, blk-wbt, blk-iolatency,
 *   kyber-iosched 같은 상위 모듈이 NVMe 완료 지연을 분석하는 데 사용한다.
 *   NVMe 스택에서의 위치: blk_mq_start_request()가 doorbell 울리기 직전
 *   rq->io_start_time_ns 를 기록하고, 컨트롤러가 CQ 항목을 기록한 후
 *   nvme_process_cq -> nvme_complete_rq -> blk_mq_end_request ->
 *   __blk_mq_end_request -> __blk_mq_end_request_acct -> blk_stat_add
 *   경로로 호출되어 SQ 제출부터 CQ 완료까지의 NVMe 명령 수명 주기 지연을 계산한다.
 *   선행 파일: blk-core.c (q->stats 할당), blk-mq.c (완료 시 호출).
 *   (추정) 본 파일의 통계는 PRP/SGL DMA 완료 시점이 아닌 block layer 의
 *   완료 콜백 시점을 기준으로 측정된다.
 */
#include <linux/kernel.h>	/* NVMe CQ 핸들러의 기본 자료형/매크로 제공 */
#include <linux/rculist.h>	/* NVMe 완료 핫패스의 RCU callback 리스트 순회용 */

#include "blk-stat.h"	/* blk_rq_stat, blk_stat_callback 정의 */
#include "blk-mq.h"	/* -> NVMe multi-queue(hctx/tag) 완료 경로 연결 */
#include "blk.h"	/* QUEUE_FLAG_STATS 등 NVMe queue 상태 플래그 */

/*
 * blk_stat_callback 주요 필드 (NVMe 관점):
 *   @list: request_queue->stats->callbacks RCU 리스트에 연결됨. NVMe 폴리/인터럽트
 *          완료 경로에서 RCU read lock 하에 순회 대상이 됨.
 *   @timer: bucket 통계를 주기적으로 집계(flush)하기 위한 타이머.
 *   @cpu_stat: per-cpu bucket 배열. NVMe 완료는 모든 CPU 코어에서 빈번히
 *              발생하므로 캐시 라인 충돌을 피하기 위해 cpu 단위로 분리.
 *   @bucket_fn: request 를 read/write 등 NVMe 동작 특성에 따라 bucket 인덱스로
 *               분류하는 함수 (예: NVMe read cmd / write cmd 구분).
 *   @buckets: bucket 개수.
 *   @stat: 타이머가 만료될 때 cpu_stat 로부터 병합된 전역 통계.
 *   @timer_fn: 통계 수집 윈도우 만료 시 상위 모듈(wbt/iolatency/kyber)이
 *              등록한 콜백.
 *   @data: 상위 모듈의 private 데이터 (예: struct rq_wb, struct iolatency_grp).
 *   @rcu: RCU grace period 이후 메모리 해제를 위한 헤드.
 *
 * blk_rq_stat 주요 필드 (NVMe 관점):
 *   @min: 측정된 NVMe 명령 지연 중 최소값. 초기값은 -1ULL (최대 u64).
 *   @max: 측정된 NVMe 명령 지연 중 최대값.
 *   @nr_samples: 수집된 NVMe 명령 샘플 수.
 *   @mean: 평균 지연 (가중 평균).
 *   @batch: per-cpu 버퍼에 누적된 지연값 합산. 타이머 만료 시 stat 으로 병합.
 *   (추정) mean 필드는 cpu_stat 병합 시에만 계산되며, per-cpu 추가 단계에서는
 *   batch 에 누적만 한다.
 */

/*
 * struct blk_queue_stats (NVMe 관점):
 *   @callbacks: 이 request_queue 에 등록된 blk_stat_callback RCU 연결 리스트.
 *               NVMe 완료 핫패스에서 RCU read lock 으로 lockless 순회됨.
 *   @lock: callbacks 리스트와 accounting 카운터 보호용 스핀락.
 *   @accounting: 별도 callback 없이 기본 통계만 활성화할 때의 레퍼런스 카운트.
 *                 blk_stat_enable_accounting() 가 증가, disable_accounting() 이
 *                 감소시키며 0 이 되고 callbacks 가 비어 있으면
 *                 QUEUE_FLAG_STATS 를 클리어한다.
 */
struct blk_queue_stats {
	struct list_head callbacks;	/* NVMe latency callback RCU 리스트 헤드 */
	spinlock_t lock;		/* callback 등록/제거 시 NVMe queue 동기화 */
	int accounting;			/* NVMe 기본 timestamp 기록 활성화 카운트 */
};

/*
 * blk_rq_stat_init:
 *   목적: 단일 bucket 의 통계 값을 초기화한다.
 *   호출 경로: blk_stat_alloc_callback -> (per-cpu/init) -> blk_rq_stat_init
 *             blk_stat_timer_fn (타이머 만료 시 cpu_stat 초기화)
 *   NVMe 연결: NVMe 명령 지연 샘플을 누적하기 전 min/max/batch 등을 초기화.
 */
void blk_rq_stat_init(struct blk_rq_stat *stat)
{
	stat->min = -1ULL;	/* NVMe 최소 지연: 초기값을 최대 u64 로 설정하여 첫 샘플이 반드시 min 이 되게 함 */
	stat->max = stat->nr_samples = stat->mean = 0;	/* max, 샘플 수, 평균 모두 0 으로 리셋 */
	stat->batch = 0;	/* per-cpu 누적 합을 0 으로 초기화: 다음 NVMe 완료부터 새 윈도우 집계 */
}

/* src 는 per-cpu 통계이며, mean 은 아직 초기화되지 않은 상태 */
/*
 * blk_rq_stat_sum:
 *   목적: per-cpu 통계 src 를 전역 통계 dst 로 병합한다.
 *   호출 경로: blk_stat_timer_fn -> blk_rq_stat_sum
 *   NVMe 연결: 여러 CPU 코어에서 동시에 수집된 NVMe 완료 지연을 하나의
 *             전역 histogram 으로 집계. mean 은 가중 평균으로 재계산.
 */
void blk_rq_stat_sum(struct blk_rq_stat *dst, struct blk_rq_stat *src)
{
	/* 오버플로우 방지: src 샘플이 0이면 합도 변하지 않으므로 리턴 */
	if (dst->nr_samples + src->nr_samples <= dst->nr_samples)
		return;	/* (추정) nr_samples 오버플로우 시 NVMe 지연 평균이 왜곡되므로 병합 스킵 */

	dst->min = min(dst->min, src->min); /* NVMe 최소 지연 갱신 */
	dst->max = max(dst->max, src->max); /* NVMe 최대 지연 갱신 */

	/*
	 * 가중 평균 재계산: (src 총합 + dst 기존 평균 * dst 샘플 수) /
	 *                 (전체 샘플 수). div_u64 로 64비트 나눗셈 수행.
	 */
	dst->mean = div_u64(src->batch + dst->mean * dst->nr_samples,
				dst->nr_samples + src->nr_samples);

	dst->nr_samples += src->nr_samples; /* NVMe 명령 샘플 수 누적 */
}

/*
 * blk_rq_stat_add:
 *   목적: 하나의 NVMe 명령 완료 지연값을 per-cpu bucket 에 추가한다.
 *   호출 경로: blk_stat_add -> blk_rq_stat_add
 *   NVMe 연결: NVMe CQ 완료 시점의 now 와 rq->io_start_time_ns 차이를 value 로
 *             받아 min/max/batch/nr_samples 를 갱신.
 */
void blk_rq_stat_add(struct blk_rq_stat *stat, u64 value)
{
	stat->min = min(stat->min, value); /* NVMe 최소 완료 지연 */
	stat->max = max(stat->max, value); /* NVMe 최대 완료 지연 */
	stat->batch += value;              /* per-cpu 누적 합 (mean 계산용) */
	stat->nr_samples++;                /* NVMe 명령 하나 완료 카운트 */
}

/*
 * blk_stat_add:
 *   목적: request 완료 시점에 NVMe 명령의 총 지연 시간을 계산하고,
 *         등록된 통계 callback 의 bucket 에 per-cpu 로 기록한다.
 *   호출 경로:
 *     nvme_process_cq -> nvme_complete_rq -> blk_mq_end_request ->
 *     __blk_mq_end_request -> __blk_mq_end_request_acct -> blk_stat_add
 *   NVMe 연결:
 *     - rq->io_start_time_ns 는 blk_mq_start_request() 에서
 *       nvme_queue_rq -> nvme_submit_cmd(doorbell) 직전에 기록됨.
 *     - now 는 CQ 항목 처리 시점의 blk_time_get_ns() 값.
 *     - value = now - io_start_time_ns 는 SQ doorbell 시점부터
 *       CQ 완료 콜백 시점까지의 NVMe 명령 수명 지연을 의미.
 */
void blk_stat_add(struct request *rq, u64 now)
{
	struct request_queue *q = rq->q;	/* NVMe request 가 속한 request_queue -> nvme_ns->queue */
	struct blk_stat_callback *cb;		/* NVMe latency callback iterator */
	struct blk_rq_stat *stat;		/* 최종 누적할 per-cpu bucket 포인터 */
	int bucket, cpu;			/* bucket: read/write 구분, cpu: 완료 처리 CPU */
	u64 value;				/* NVMe SQ doorbell ~ CQ completion 지연(ns) */

	/*
	 * NVMe 명령 지연 계산: doorbell 직전 기록된 io_start_time_ns 와
	 * CQ 처리 시점 now 의 차이. (추정) now 가 역전될 경우를 대비해 0 으로 클램프.
	 */
	value = (now >= rq->io_start_time_ns) ? now - rq->io_start_time_ns : 0;

	rcu_read_lock();	/* NVMe 완료 핫패스: callback 리스트를 lockless 로 순회하기 위한 RCU read 크리티컬 섹션 시작 */
	cpu = get_cpu();	/* 현재 완료 처리 CPU 를 선점 고정(preempt disable) 하여 per-cpu 버퍼 안전 접근 */
	/*
	 * request_queue 에 등록된 모든 blk_stat_callback 을 RCU 로 순회.
	 * NVMe 완료 핫패스이므로 lockless read 가 중요.
	 */
	list_for_each_entry_rcu(cb, &q->stats->callbacks, list) {	/* NVMe CQ 핸들러가 등록된 모든 latency callback 을 RCU 기반으로 순회 */
		if (!blk_stat_is_active(cb))
			continue;	/* 활성화되지 않은 callback 은 건재: NVMe 완료 통계 누적 스킵 */

		bucket = cb->bucket_fn(rq); /* NVMe read/write 등 bucket 분류 */
		if (bucket < 0)
			continue;	/* bucket_fn 이 -1 반환 시 해당 NVMe rq 는 통계에서 제외(예: flush/cmd) */

		stat = &per_cpu_ptr(cb->cpu_stat, cpu)[bucket];	/* 완료 CPU 의 per-cpu bucket 선택: 캐시 라인 분산으로 lockless 누적 가능 */
		blk_rq_stat_add(stat, value); /* per-cpu bucket 에 지연 기록 */
	}
	put_cpu();		/* preempt enable: per-cpu 통계 접근 종료 */
	rcu_read_unlock();	/* RCU read 크리티컬 섹션 종료 -> nvme_complete_rq 가 메모리 해제 전 grace period 보장 */
}

/*
 * blk_stat_timer_fn:
 *   목적: 타이머 만료 시 각 bucket 의 전역 stat 을 초기화하고,
 *         모든 online CPU 의 per-cpu 통계를 병합한 뒤 사용자 콜백을 호출한다.
 *   호출 경로: mod_timer -> (만료) -> blk_stat_timer_fn -> cb->timer_fn
 *   NVMe 연결: NVMe 완료가 여러 CPU 에 흩어져 기록된 지연을 주기적으로
 *             집계하여 wbt/iolatency/kyber 가 평균/백분위 지연을 판단.
 */
static void blk_stat_timer_fn(struct timer_list *t)
{
	struct blk_stat_callback *cb = timer_container_of(cb, t, timer);	/* 타이머 구조체에서 NVMe latency callback 객체 복원 */
	unsigned int bucket;	/* bucket 인덱스: read/write/기타 NVMe 명령 유형 */
	int cpu;		/* online CPU iterator: MSI-X 로 분산된 NVMe CQ 처리 코어 */

	/* 전역 stat 버킷 초기화: 새 윈도우 집계 준비 */
	for (bucket = 0; bucket < cb->buckets; bucket++)	/* NVMe read/write 등 각 명령 유형별 전역 통계 리셋 */
		blk_rq_stat_init(&cb->stat[bucket]);

	/*
	 * 각 online CPU 의 per-cpu bucket 을 전역 stat 으로 병합 후
	 * per-cpu 버퍼를 초기화. (추정) NVMe MSI-X 가 모든 CPU 에 분산될 때
	 * 이 루프가 캐시 라인 스탬핑을 유발할 수 있으나, 타이머 주기는
	 * 완료 핫패스보다 훨씬 드물게 발생.
	 */
	for_each_online_cpu(cpu) {	/* NVMe CQ 인터럽트가 도달 가능한 모든 online CPU 순회 */
		struct blk_rq_stat *cpu_stat;

		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);	/* 해당 CPU 의 per-cpu bucket 배열 획득 */
		for (bucket = 0; bucket < cb->buckets; bucket++) {	/* 모든 NVMe 명령 유형에 대해 병합 수행 */
			blk_rq_stat_sum(&cb->stat[bucket], &cpu_stat[bucket]);	/* CPU 별 NVMe 지연을 전역 histogram 으로 합산 */
			blk_rq_stat_init(&cpu_stat[bucket]);	/* 병합 후 per-cpu 버퍼 클리어: 다음 윈도우 누적 준비 */
		}
	}

	cb->timer_fn(cb); /* 상위 NVMe latency 제어 모듈 콜백 실행 */
}

/*
 * blk_stat_alloc_callback:
 *   목적: bucket 개수만큼의 전역 stat 과 per-cpu stat 배열을 할당하고
 *         타이머를 초기화한다.
 *   호출 경로: blk-wbt / blk-iolatency / kyber-iosched 의 init 시
 *   NVMe 연결: NVMe queue 의 request_queue 에 대해 latency callback 을
 *             생성. bucket_fn 은 보통 read/write 를 구분하여 NVMe 명령
 *             유형별 지연을 측정.
 */
struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data)
{
	struct blk_stat_callback *cb;

	cb = kmalloc_obj(*cb);
	if (!cb)
		return NULL;	/* NVMe latency callback 할당 실패: 상위 모듈(wbt/kyber) 초기화 중단 */

	cb->stat = kmalloc_objs(struct blk_rq_stat, buckets);
	if (!cb->stat) {
		kfree(cb);
		return NULL;	/* 전역 histogram 할당 실패 시 callback 객체 롤백 */
	}
	/*
	 * per-cpu bucket 할당: NVMe 완료 핫패스에서 lockless 누적을 위해
	 * 각 CPU 마다 buckets 개의 struct blk_rq_stat 공간을 할당.
	 */
	cb->cpu_stat = __alloc_percpu(buckets * sizeof(struct blk_rq_stat),
				      __alignof__(struct blk_rq_stat));
	if (!cb->cpu_stat) {
		kfree(cb->stat);
		kfree(cb);
		return NULL;	/* per-cpu 분산 통계 공간 할당 실패: NVMe lockless 수집 불가능 */
	}

	cb->timer_fn = timer_fn;	/* wbt/iolatency/kyber 의 NVMe latency 윈도우 만료 핸들러 등록 */
	cb->bucket_fn = bucket_fn;	/* NVMe read/write 분류 함수 연결 */
	cb->data = data;		/* 상위 모듈 private: 예) struct rq_wb */
	cb->buckets = buckets;		/* NVMe 명령 유형별 bucket 개수 설정(read/write 이면 2) */
	timer_setup(&cb->timer, blk_stat_timer_fn, 0);	/* 주기적 집계 타이머 초기화: IRQ 컨텍스트에서 NVMe 통계 flush */

	return cb;
}

/*
 * blk_stat_add_callback:
 *   목적: 할당된 callback 을 request_queue 의 stats 리스트에 등록하고
 *         통계 수집 플래그를 활성화한다.
 *   호출 경로: blk-wbt / blk-iolatency / kyber-iosched init ->
 *             blk_stat_alloc_callback -> blk_stat_add_callback
 *   NVMe 연결: 해당 NVMe request_queue 에 대해 latency 통계 수집 시작.
 *             이후 nvme_complete_rq 경로에서 blk_stat_add() 가 호출됨.
 */
void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb)
{
	unsigned int bucket;	/* bucket 인덱스 초기화 루프용 */
	unsigned long flags;	/* q->stats->lock save/restore용 */
	int cpu;		/* possible CPU iterator: 등록 직전 잔여 통계 클리어 */

	/*
	 * 등록 전 모든 per-cpu bucket 을 초기화. (추정) 이전 윈도우의
	 * 잔여 샘플이 새 콜백의 첫 집계에 섞이지 않도록 방지.
	 */
	for_each_possible_cpu(cpu) {	/* 등록 시점에 존재할 수 있는 모든 CPU 의 per-cpu 버퍼 순회 */
		struct blk_rq_stat *cpu_stat;

		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);
		for (bucket = 0; bucket < cb->buckets; bucket++)	/* read/write 등 모든 bucket 리셋 */
			blk_rq_stat_init(&cpu_stat[bucket]);
	}

	spin_lock_irqsave(&q->stats->lock, flags);	/* callback 리스트와 QUEUE_FLAG_STATS 동시 보호: NVMe queue 상태 변경 동기화 */
	list_add_tail_rcu(&cb->list, &q->stats->callbacks);	/* RCU 리스트에 추가: NVMe 완료 핫패스가 곧바로 lockless 순회 가능 */
	blk_queue_flag_set(QUEUE_FLAG_STATS, q); /* NVMe 지연 측정 활성화 */
	spin_unlock_irqrestore(&q->stats->lock, flags);
}

/*
 * blk_stat_remove_callback:
 *   목적: callback 을 request_queue 의 stats 리스트에서 제거하고,
 *         더 이상 사용하지 않을 때 타이머를 안전하게 정지한다.
 *   호출 경로: scheduler/wbt exit -> blk_stat_remove_callback
 *   NVMe 연결: 해당 NVMe queue 에 대한 latency 통계 수집 중단.
 *             RCU grace period 후 blk_stat_free_callback 에서 메모리 해제.
 */
void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb)
{
	unsigned long flags;

	spin_lock_irqsave(&q->stats->lock, flags);	/* NVMe queue stats 상태 보호 */
	list_del_rcu(&cb->list);	/* RCU 리스트에서 제거: 이후 nvme_complete_rq 의 list_for_each_entry_rcu 에서 보이지 않음 */
	/* callback 과 기본 accounting 모두 없으면 통계 플래그 해제 */
	if (list_empty(&q->stats->callbacks) && !q->stats->accounting)
		blk_queue_flag_clear(QUEUE_FLAG_STATS, q);	/* NVMe 지연 측정 비활성화: blk_mq_start_request 의 io_start_time_ns 기록 중단 */
	spin_unlock_irqrestore(&q->stats->lock, flags);

	timer_delete_sync(&cb->timer); /* 타이머 핸들러가 완료될 때까지 대기 */
}

/*
 * blk_stat_free_callback_rcu:
 *   목적: RCU grace period 이후 per-cpu stat, stat 배열, callback 구조체를
 *         해제한다.
 *   NVMe 연결: NVMe queue 가 제거되거나 scheduler 가 교첉될 때 관련
 *             latency 메모리를 정리.
 */
static void blk_stat_free_callback_rcu(struct rcu_head *head)
{
	struct blk_stat_callback *cb;

	cb = container_of(head, struct blk_stat_callback, rcu);	/* RCU 헤드로부터 NVMe latency callback 복원 */
	free_percpu(cb->cpu_stat);	/* per-cpu NVMe 통계 메모리 해제: 모든 CPU 의 bucket 배열 반납 */
	kfree(cb->stat);		/* 전역 histogram 메모리 해제 */
	kfree(cb);			/* callback 객체 해제: NVMe queue 완전 분리 */
}

/*
 * blk_stat_free_callback:
 *   목적: callback 의 메모리를 RCU 를 통해 안전하게 해제한다.
 *   호출 경로: blk_stat_remove_callback 이후 -> blk_stat_free_callback
 */
void blk_stat_free_callback(struct blk_stat_callback *cb)
{
	if (cb)
		call_rcu(&cb->rcu, blk_stat_free_callback_rcu);	/* 모든 nvme_complete_rq RCU read 크리티컬 섹션 종료 후 메모리 해제 예약 */
}

/*
 * blk_stat_disable_accounting:
 *   목적: callback 없이 기본 통계 수집을 사용하는 사용자의 레퍼런스를 감소.
 *   호출 경로: blk-iocost / bfq 등의 비활성화 경로
 *   NVMe 연결: NVMe queue 에 대한 기본 io_start_time_ns 기록을 중단할 수
 *             있음. accounting 이 0 이고 callbacks 가 비어 있으면
 *             QUEUE_FLAG_STATS 를 클리어하여 blk_mq_start_request 의
 *             타임스탬프 기록을 멈춤.
 */
void blk_stat_disable_accounting(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(&q->stats->lock, flags);	/* accounting/플래그 원자적 감소를 위한 NVMe queue lock */
	if (!--q->stats->accounting && list_empty(&q->stats->callbacks))
		blk_queue_flag_clear(QUEUE_FLAG_STATS, q);	/* 마지막 사용자 종료: NVMe SQ doorbell 직전 timestamp 기록 중단 */
	spin_unlock_irqrestore(&q->stats->lock, flags);
}
EXPORT_SYMBOL_GPL(blk_stat_disable_accounting);

/*
 * blk_stat_enable_accounting:
 *   목적: callback 없이 기본 통계 수집을 사용하는 사용자의 레퍼런스를 증가.
 *   호출 경로: blk-iocost / bfq 등의 활성화 경로
 *   NVMe 연결: NVMe queue 에 대해 rq->io_start_time_ns 및 RQF_STATS 기록
 *             활성화. 이는 blk_mq_start_request 에서 doorbell 직전 타임스탬프를
 *             남기게 한다.
 */
void blk_stat_enable_accounting(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(&q->stats->lock, flags);
	if (!q->stats->accounting++ && list_empty(&q->stats->callbacks))
		blk_queue_flag_set(QUEUE_FLAG_STATS, q);	/* 첫 사용자 활성화: NVMe 명령 수명 지연 측정 시작 */
	spin_unlock_irqrestore(&q->stats->lock, flags);
}
EXPORT_SYMBOL_GPL(blk_stat_enable_accounting);

/*
 * blk_alloc_queue_stats:
 *   목적: request_queue 의 통계 관리 구조체(blk_queue_stats)를 할당 및 초기화.
 *   호출 경로: blk-core.c -> blk_alloc_queue_stats (q->stats 할당)
 *   NVMe 연결: NVMe queue 생성 시 blk-core.c 에 의해 호출되어, 이후
 *             NVMe latency callback 등록을 위한 기반을 마련.
 */
struct blk_queue_stats *blk_alloc_queue_stats(void)
{
	struct blk_queue_stats *stats;

	stats = kmalloc_obj(*stats);
	if (!stats)
		return NULL;	/* NVMe queue 별 stats 관리자 할당 실패: q->stats NULL 상태로 queue 생성 실패 */

	INIT_LIST_HEAD(&stats->callbacks);	/* NVMe latency callback RCU 리스트 초기화 */
	spin_lock_init(&stats->lock);		/* callback 등록/제거 및 accounting 보호용 spinlock 초기화 */
	stats->accounting = 0;			/* 기본 io_start_time_ns 사용자 없음: QUEUE_FLAG_STATS 는 아직 off */

	return stats;
}

/*
 * blk_free_queue_stats:
 *   목적: request_queue 의 통계 관리 구조체를 해제한다.
 *   호출 경로: queue 해제 시 blk-core.c -> blk_free_queue_stats
 *   NVMe 연결: NVMe queue 소멸 시 관련 stats 리소스 정리. callbacks 가
 *             남아 있으면 WARN_ON 으로 경고.
 */
void blk_free_queue_stats(struct blk_queue_stats *stats)
{
	if (!stats)
		return;

	WARN_ON(!list_empty(&stats->callbacks));	/* NVMe queue 소멸 전 미제거 latency callback 존재 시 버그 경고 */

	kfree(stats);	/* NVMe request_queue 와 연결된 stats 관리자 메모리 반납 */
}

/* NVMe 관점 핵심 요약 */
/*
 * - 본 파일은 NVMe 명령이 SQ 에 제출된 시점(io_start_time_ns)부터 CQ 완료
 *   콜백 시점(now)까지의 지연을 측정하는 통계 인프라를 제공한다.
 *   핵심 호출 체인:
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) ... (NVMe 처리) ... nvme_process_cq ->
 *   nvme_complete_rq -> blk_mq_end_request -> __blk_mq_end_request ->
 *   __blk_mq_end_request_acct -> blk_stat_add
 * - per-cpu bucket (cpu_stat) 설계는 NVMe 완료가 모든 CPU 에서 빈번히
 *   발생하는 환경에서 lockless 통계 누적을 가능하게 한다.
 * - blk_queue_stats->accounting 과 QUEUE_FLAG_STATS 는 NVMe queue 의
 *   기본 latency 기록을 켜고 끄는 스위치 역할을 한다.
 * - 수집된 통계는 blk-wbt, blk-iolatency, kyber-iosched 등에서 NVMe SSD 의
 *   응답 지연을 모니터링하고 쓰로틀링/스케줄링 결정에 활용된다.
 * - (추정) 본 파일은 DMA 완료, PRP/SGL 해제 등 하드웨어 낮은 단계의
 *   시점이 아니라 block layer 의 software 완료 콜백 시점을 기준으로
 *   측정하므로, 실제 플래시 프로그램 시간과는 미세한 차이가 있을 수 있다.
 */
