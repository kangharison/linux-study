// SPDX-License-Identifier: GPL-2.0
/*
 * Block stat tracking code
 *
 * Copyright (C) 2016 Jens Axboe
 */
/*
 * [한국어 설명] 블록 계층 I/O 지연 시간 통계 수집 인프라 (blk-stat.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 블록 계층의 I/O 지연 시간(latency) 통계를 수집·관리하는 핵심
 * 인프라를 구현한다. request_queue 단위로 blk_stat_callback을 등록하여 request 완료
 * 시마다 지연값을 per-cpu bucket에 누적하고, 주기적 타이머를 통해 전체 CPU에 흩어진
 * 통계를 하나의 전역 histogram으로 병합한 뒤 상위 제어 모듈(blk-wbt, blk-iolatency,
 * kyber-iosched)에 콜백으로 알려준다. 상위 모듈은 이 통계를 바탕으로 쓰로틀링 정책이나
 * 스케줄링 가중치를 조정한다. 측정 대상은 blk_mq_start_request()에서 기록된
 * rq->io_start_time_ns부터 완료 경로의 blk_stat_add() 호출 시각(now)까지의 차이이며,
 * 이는 블록 계층 소프트웨어 완료 시점 기준의 지연이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층 I/O 흐름에서 이 파일은 '완료 측 계측 계층'에 위치한다.
 * 전형적인 호출 체인은 다음과 같다:
 *   blk_mq_submit_bio → blk_mq_start_request (io_start_time_ns 기록) →
 *   [디바이스 드라이버 처리: NVMe의 경우 SQ 제출 → 컨트롤러 처리 → CQ 수신] →
 *   blk_mq_end_request → __blk_mq_end_request → __blk_mq_end_request_acct →
 *   blk_stat_add (이 파일, per-cpu bucket 누적) →
 *   [타이머 만료] → blk_stat_timer_fn (이 파일, 전역 병합) →
 *   cb->timer_fn (blk-wbt / blk-iolatency / kyber-iosched: 통계 소비·정책 갱신)
 * QUEUE_FLAG_STATS 플래그가 설정된 경우에만 blk_mq_start_request()가 io_start_time_ns를
 * 기록한다. 이 파일의 blk_stat_add_callback() 및 blk_stat_enable_accounting()이 해당
 * 플래그를 켜고 끈다. 실행 컨텍스트는 인터럽트/softirq(완료 경로) 및 타이머 softirq.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈:
 *   - blk-core.c: request_queue 생성 시 blk_alloc_queue_stats()를 호출하여 q->stats를
 *     초기화한다. queue 해제 시 blk_free_queue_stats()로 자원을 반납한다.
 *   - blk-mq.c: request 완료 시 blk_stat_add()를 호출한다. QUEUE_FLAG_STATS 플래그에
 *     따라 io_start_time_ns 기록 여부를 결정하는 주체이기도 하다.
 * 이 파일에 의존하는 모듈:
 *   - blk-wbt.c: blk_stat_alloc_callback()으로 read/write 2개 bucket 콜백을 등록하고,
 *     타이머 만료 시 평균 지연을 분석하여 토큰 버킷 발행 속도를 조정한다.
 *   - blk-iolatency.c: per-cgroup 지연 목표를 달성하기 위해 콜백을 등록하고, 지연
 *     초과 시 throttle_delay를 늘려 진입률을 제한한다.
 *   - kyber-iosched.c: 읽기/쓰기/기타 bucket 지연 분포를 보고 hctx 토큰 발행 가중치를
 *     동적으로 조정한다.
 * 공유하는 핵심 자료구조:
 *   - struct blk_rq_stat: 단일 bucket의 min/max/mean/nr_samples/batch를 담은 히스토그램.
 *   - struct blk_stat_callback: per-cpu stat 배열, 타이머, bucket_fn, timer_fn을 묶은 객체.
 *   - struct blk_queue_stats: request_queue당 callbacks 리스트, lock, accounting 카운터.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_stat_add()           — request 완료 시 per-cpu bucket에 지연값을 누적하는 핫패스
 * blk_stat_timer_fn()      — 타이머 만료 시 per-cpu 통계를 전역 병합 후 상위 콜백 실행
 * blk_stat_alloc_callback()— bucket 배열, per-cpu 통계 공간, 타이머를 포함한 콜백 객체 할당
 * blk_stat_add_callback()  — 콜백을 request_queue에 RCU 등록하고 STATS 플래그 활성화
 * blk_stat_remove_callback()— 콜백을 RCU 제거하고 타이머를 동기적으로 정지
 * blk_rq_stat_sum()        — per-cpu 통계를 전역 통계로 가중 평균 방식으로 병합
 * struct blk_rq_stat       — min/max/mean/nr_samples/batch: 단일 bucket의 지연 집계 구조체
 * struct blk_stat_callback — per-cpu bucket 배열, 타이머, bucket_fn, timer_fn을 묶은 콜백 객체
 * struct blk_queue_stats   — request_queue당 콜백 리스트·lock·accounting을 담은 통계 관리자
 */
#include <linux/kernel.h>   /* [한국어] 기본 자료형·매크로(u64, min, max, div_u64 등) 제공:
                             *         지연 계산 및 bucket 인덱스 범위 처리에 사용 */
#include <linux/rculist.h>  /* [한국어] list_for_each_entry_rcu, list_add_tail_rcu, list_del_rcu 등
                             *         RCU 기반 리스트 API 제공: blk_stat_add()의 완료 핫패스에서
                             *         callbacks 리스트를 lockless로 순회하기 위해 필수 */

#include "blk-stat.h"       /* [한국어] struct blk_rq_stat, struct blk_stat_callback, blk_stat_is_active()
                             *         등 이 파일이 구현하는 모든 통계 자료구조·API의 선언 헤더 */
#include "blk-mq.h"         /* [한국어] blk_mq_start_request(), blk_mq_end_request() 등
                             *         multi-queue 요청 생명주기 선언: blk_stat_add()는 blk-mq.c
                             *         완료 경로(__blk_mq_end_request_acct)에서 호출된다 */
#include "blk.h"            /* [한국어] QUEUE_FLAG_STATS, blk_queue_flag_set/clear() 등 queue 플래그
                             *         관리 API: io_start_time_ns 기록 활성화 여부를 제어하는 데 필요 */

/*
 * [한국어] struct blk_queue_stats — request_queue 단위 통계 관리 구조체
 *
 * request_queue 하나당 하나씩 할당되며(q->stats), 등록된 blk_stat_callback 목록을
 * 관리하고 기본 io_start_time_ns 기록의 활성/비활성을 조율한다.
 * blk_alloc_queue_stats()에서 할당·초기화되고, blk_free_queue_stats()에서 해제된다.
 */
struct blk_queue_stats {
	struct list_head callbacks;
	/* [한국어] 이 request_queue에 등록된 blk_stat_callback 객체들의 RCU 연결 리스트 헤드.
	 * 설정자: blk_stat_add_callback()이 list_add_tail_rcu()로 항목을 추가한다.
	 *        blk_stat_remove_callback()이 list_del_rcu()로 항목을 제거한다.
	 * 읽는 자: blk_stat_add()의 완료 핫패스에서 list_for_each_entry_rcu()로 lockless 순회.
	 * 값 범위: 0개 이상의 blk_stat_callback. 비어 있으면 QUEUE_FLAG_STATS 클리어 대상.
	 * 동기화: 추가/제거는 q->stats->lock(spinlock_irqsave)으로 보호.
	 *        순회는 RCU read lock으로 보호(락-프리 읽기 허용). */

	spinlock_t lock;
	/* [한국어] callbacks 리스트 수정 및 accounting 카운터 변경을 보호하는 spinlock.
	 * 설정자: blk_alloc_queue_stats()에서 spin_lock_init()으로 초기화.
	 * 사용처: blk_stat_add_callback(), blk_stat_remove_callback(),
	 *         blk_stat_enable_accounting(), blk_stat_disable_accounting().
	 * 값 범위: 해당 없음 (spinlock은 잠금/해제 상태만 존재).
	 * 동기화: 완료 IRQ 컨텍스트에서도 호출될 수 있으므로 spin_lock_irqsave로 취득. */

	int accounting;
	/* [한국어] 콜백 없이 기본 io_start_time_ns 타임스탬프 기록만 요청하는 사용자의
	 *         레퍼런스 카운터. blk-iocost, bfq 등이 직접 타임스탬프를 읽기 위해 사용.
	 * 설정자: blk_stat_enable_accounting()이 증가(++)시키고,
	 *         blk_stat_disable_accounting()이 감소(--)시킨다.
	 * 읽는 자: blk_stat_remove_callback()과 blk_stat_disable_accounting()이
	 *          0 여부를 확인하여 QUEUE_FLAG_STATS 클리어 타이밍을 결정.
	 * 값 범위: 0 이상의 정수. 0이고 callbacks가 비어 있으면 STATS 플래그 해제 대상.
	 * 동기화: q->stats->lock(spinlock)으로 보호. */
};

/*
 * [한국어]
 * blk_rq_stat_init - 단일 blk_rq_stat bucket을 초기값으로 리셋한다.
 *
 * @stat: 초기화할 bucket 통계 포인터. per-cpu 버퍼 또는 전역 stat 배열의 원소.
 * @return: 없음 (void).
 *
 * min 필드를 -1ULL(u64 최댓값)로 초기화하여 첫 샘플이 반드시 min이 되도록 하고,
 * max/mean/nr_samples/batch를 0으로 리셋하여 새 집계 윈도우를 준비한다.
 * "없으면 어떤 문제가 생기는가": 초기화 없이 누적하면 이전 윈도우의 값이 섞여
 * 평균·최솟값이 왜곡된다.
 * 실행 컨텍스트: 타이머 softirq(blk_stat_timer_fn에서 호출)와 일반 프로세스 컨텍스트
 *               (blk_stat_add_callback 초기화 경로)에서 모두 호출됨.
 *
 * 호출 체인:
 *   blk_stat_add_callback → for_each_possible_cpu → [이 함수]
 *   blk_stat_timer_fn → [이 함수] (per-cpu 버퍼 병합 후 초기화)
 *   blk_stat_alloc_callback → (per-cpu 버퍼는 percpu_alloc으로 0-fill 되지만
 *                               min 필드는 이 함수로 -1ULL로 설정해야 함)
 */
void blk_rq_stat_init(struct blk_rq_stat *stat)
{
	stat->min = -1ULL;  /* [한국어] min 초기값을 u64 최댓값으로 설정: 첫 번째 샘플이 무조건
	                     *         min이 되도록 보장. 0으로 초기화하면 실제 최소 지연이 0이
	                     *         되어 의미 없는 결과가 나온다. */
	stat->max = stat->nr_samples = stat->mean = 0;  /* [한국어] max, 샘플 수, 가중 평균을
	                                                  *         동시에 0으로 초기화: 새 집계
	                                                  *         윈도우 시작 준비 */
	stat->batch = 0;    /* [한국어] per-cpu 누적 합(batch)을 0으로 리셋: blk_rq_stat_sum()이
	                     *         가중 평균을 계산할 때 src->batch를 사용하므로 반드시 초기화 */
}

/* [한국어] src는 per-cpu 통계이므로 mean 필드는 아직 계산되지 않은 상태이다.
 *         mean 계산은 blk_rq_stat_sum()에서 병합 시 수행한다. */
/*
 * [한국어]
 * blk_rq_stat_sum - per-cpu 통계 src를 전역 통계 dst에 가중 평균으로 병합한다.
 *
 * @dst: 병합 대상 전역 통계 bucket. blk_stat_callback->stat[bucket]을 가리킴.
 * @src: 병합 원본 per-cpu 통계 bucket. per_cpu_ptr(cb->cpu_stat, cpu)[bucket].
 * @return: 없음 (void).
 *
 * 여러 CPU에 분산되어 누적된 per-cpu 통계를 하나의 전역 histogram으로 합산한다.
 * mean은 단순 평균이 아닌 가중 평균으로 재계산하여 샘플 수 차이가 있는 CPU 간에도
 * 정확한 전체 평균을 구한다: (src->batch + dst->mean * dst->nr_samples) / (합산 nr_samples).
 * nr_samples 오버플로우를 방지하기 위해 합산이 dst->nr_samples보다 작거나 같으면 조기 반환.
 * 실행 컨텍스트: 타이머 softirq(blk_stat_timer_fn) 단독 호출, 재진입 없음.
 * 에러 경로: nr_samples 오버플로우 감지 시 silent return(통계 미병합) — 극단적 상황에서
 *            정확도보다 안전을 우선시하는 방어 코드.
 *
 * 호출 체인:
 *   blk_stat_timer_fn → for_each_online_cpu → [이 함수]
 */
void blk_rq_stat_sum(struct blk_rq_stat *dst, struct blk_rq_stat *src)
{
	/* [한국어] nr_samples 오버플로우 감지: 두 bucket의 샘플 수를 합산했을 때 dst->nr_samples
	 *         이하이면 unsigned 정수 오버플로우가 발생한 것이므로 병합을 건너뛴다.
	 *         오버플로우된 샘플 수로 나눗셈을 수행하면 mean이 완전히 왜곡된다. */
	if (dst->nr_samples + src->nr_samples <= dst->nr_samples)
		return;  /* [한국어] 오버플로우 시 병합 스킵: 부정확한 통계보다 누락이 낫다 */

	dst->min = min(dst->min, src->min);  /* [한국어] 두 bucket 중 더 작은 최소 지연 유지:
	                                      *         모든 CPU의 최소 지연 중 전역 최솟값 추출 */
	dst->max = max(dst->max, src->max);  /* [한국어] 두 bucket 중 더 큰 최대 지연 유지:
	                                      *         모든 CPU의 최대 지연 중 전역 최댓값 추출 */

	/*
	 * [한국어] 가중 평균 재계산 공식:
	 *   새 mean = (src의 모든 샘플 합 + dst의 기존 평균 * dst의 샘플 수) /
	 *             (dst의 샘플 수 + src의 샘플 수)
	 * src->batch는 해당 CPU에서 누적된 지연값의 총합이다 (mean이 아님).
	 * dst->mean * dst->nr_samples는 dst가 이미 가진 가중 합이다.
	 * 두 합을 더한 뒤 전체 샘플 수로 나누면 병합 후의 정확한 전체 평균이 된다.
	 * div_u64()는 64비트 분자를 64비트 분모로 나누는 커널 유틸리티 (아키텍처 이식성).
	 */
	dst->mean = div_u64(src->batch + dst->mean * dst->nr_samples,
				dst->nr_samples + src->nr_samples);
	/* [한국어] 가중 평균 계산: (src 총합 + dst 기존 가중합) / (전체 샘플 수).
	 *         dst->mean을 먼저 dst->nr_samples와 곱하여 이전 가중합을 복원하고,
	 *         src->batch(src의 샘플 총합)를 더해 전체 합을 구한 뒤 나눈다. */

	dst->nr_samples += src->nr_samples;  /* [한국어] 전체 샘플 수 누적: 이후 blk_rq_stat_sum
	                                      *         호출에서 이 dst가 또 다른 src와 병합될 때
	                                      *         가중 평균 분모로 사용된다 */
}

/*
 * [한국어]
 * blk_rq_stat_add - 단일 완료 지연값을 per-cpu bucket에 추가한다.
 *
 * @stat: 현재 CPU의 해당 bucket을 가리키는 blk_rq_stat 포인터.
 *        per_cpu_ptr(cb->cpu_stat, cpu)[bucket]으로 획득.
 * @value: 이번 request의 완료 지연(ns). blk_stat_add()에서 now - io_start_time_ns로 계산.
 * @return: 없음 (void).
 *
 * min/max를 갱신하고 batch(샘플 합)에 value를 더하며 nr_samples를 증가시킨다.
 * 이 함수 자체는 동기화 없이 per-cpu 버퍼에 쓰므로, 호출자(blk_stat_add)가
 * get_cpu()로 CPU 선점을 비활성화한 상태에서 호출해야 한다.
 * 실행 컨텍스트: IRQ 또는 softirq(request 완료 경로), CPU 선점 비활성화 상태.
 *
 * 호출 체인:
 *   blk_stat_add → get_cpu → [이 함수] → put_cpu
 */
void blk_rq_stat_add(struct blk_rq_stat *stat, u64 value)
{
	stat->min = min(stat->min, value);  /* [한국어] per-cpu bucket의 최소 지연 갱신:
	                                     *         -1ULL로 초기화된 min보다 value가 작으면
	                                     *         (항상 그럴 것) min이 갱신된다 */
	stat->max = max(stat->max, value);  /* [한국어] per-cpu bucket의 최대 지연 갱신:
	                                     *         이 CPU에서 처리된 요청 중 가장 느린 것 추적 */
	stat->batch += value;               /* [한국어] 샘플 합 누적: blk_rq_stat_sum()이 가중 평균을
	                                     *         계산할 때 분자로 사용한다. mean이 아님에 주의. */
	stat->nr_samples++;                 /* [한국어] 이 CPU에서 완료된 request 수 증가:
	                                     *         blk_rq_stat_sum()의 가중 평균 분모로 사용 */
}

/*
 * [한국어]
 * blk_stat_add - request 완료 시 등록된 모든 콜백의 적절한 bucket에 지연을 기록한다.
 *
 * @rq:  완료된 request. rq->q로 request_queue를, rq->io_start_time_ns로 시작 시각을 얻음.
 * @now: 완료 시점의 nanosecond 타임스탬프(blk_time_get_ns()). 호출자가 전달.
 * @return: 없음 (void).
 *
 * 이 함수는 블록 계층 I/O 통계 수집의 핵심 핫패스이다. request의 io_start_time_ns와
 * 현재 시각 now의 차이를 지연값으로 계산한 뒤, request_queue에 등록된 모든 활성
 * blk_stat_callback을 RCU로 순회하여 각 콜백의 bucket_fn으로 bucket 인덱스를 결정하고
 * 해당 CPU의 per-cpu bucket에 누적한다. per-cpu 버퍼 덕분에 여러 CPU에서 동시에
 * 완료가 발생해도 잠금 없이 안전하게 누적할 수 있다.
 * 실행 컨텍스트: IRQ 또는 softirq(request 완료 경로). 선점이 get_cpu()로 비활성화됨.
 * 에러 경로: bucket_fn이 음수를 반환하면 해당 request는 통계에서 제외(continue).
 *            now < io_start_time_ns이면 value를 0으로 클램프하여 언더플로우 방지.
 *
 * 호출 체인:
 *   blk_mq_end_request → __blk_mq_end_request → __blk_mq_end_request_acct →
 *   [이 함수] → rcu_read_lock → list_for_each_entry_rcu → blk_rq_stat_add
 */
void blk_stat_add(struct request *rq, u64 now)
{
	struct request_queue *q = rq->q;   /* [한국어] 이 request가 속한 request_queue 참조:
	                                    *         q->stats->callbacks 리스트 순회의 진입점 */
	struct blk_stat_callback *cb;      /* [한국어] RCU 리스트 순회 이터레이터:
	                                    *         등록된 콜백(wbt, iolatency, kyber 등)을 하나씩 가리킴 */
	struct blk_rq_stat *stat;          /* [한국어] 현재 CPU·bucket의 per-cpu 통계 포인터:
	                                    *         blk_rq_stat_add()에 전달되어 누적 대상이 됨 */
	int bucket, cpu;                   /* [한국어] bucket: bucket_fn()의 반환값(read/write/기타 분류),
	                                    *         cpu: 완료를 처리 중인 현재 CPU 번호 */
	u64 value;                         /* [한국어] 계산된 request 완료 지연값(ns):
	                                    *         now - rq->io_start_time_ns */

	/* [한국어] 지연값 계산: 시작 시각이 완료 시각보다 클 경우(시계 역전, 매우 드문 경우)
	 *         언더플로우를 막기 위해 0으로 클램프한다. 정상적인 경우 now > io_start_time_ns. */
	value = (now >= rq->io_start_time_ns) ? now - rq->io_start_time_ns : 0;

	rcu_read_lock();   /* [한국어] RCU read 크리티컬 섹션 시작: callbacks 리스트를 lockless로
	                    *         순회하기 위한 진입. 이 섹션 내에서는 list_del_rcu()로 제거된
	                    *         항목도 grace period까지는 유효하게 보인다. */
	cpu = get_cpu();   /* [한국어] 현재 CPU 번호 획득 + 선점 비활성화: 이후 per_cpu_ptr()로
	                    *         접근하는 per-cpu 버퍼가 다른 CPU로 이전되지 않도록 보장.
	                    *         put_cpu()를 호출하기 전까지 CPU 이주 불가. */
	/* [한국어] request_queue에 등록된 모든 blk_stat_callback을 RCU 보호 하에 순회.
	 *         락-프리 읽기이므로 완료 핫패스에서 성능 영향 최소화. */
	list_for_each_entry_rcu(cb, &q->stats->callbacks, list) {
		/* [한국어] 콜백 활성 상태 확인: blk_stat_is_active()는 타이머가 실행 중인지 확인.
		 *         활성화되지 않은 콜백은 아직 통계 수집을 시작하지 않았으므로 건너뜀. */
		if (!blk_stat_is_active(cb))
			continue;  /* [한국어] 비활성 콜백 건너뜀: 타이머가 설정되기 전 상태 */

		/* [한국어] bucket_fn으로 이 request의 유형을 분류: 일반적으로 read=0, write=1.
		 *         콜백마다 다른 분류 기준을 가질 수 있으며, 음수를 반환하면 제외 대상. */
		bucket = cb->bucket_fn(rq);  /* [한국어] request 유형 → bucket 인덱스 변환:
		                              *         예) BLK_STAT_READ=0, BLK_STAT_WRITE=1 */
		if (bucket < 0)
			continue;  /* [한국어] bucket_fn이 -1을 반환하면 이 request는 통계 제외:
			            *         예) 통계 불필요한 flush/discard 명령 등 */

		/* [한국어] 현재 CPU의 해당 bucket에 대한 per-cpu 통계 포인터 획득:
		 *         per_cpu_ptr()로 이 CPU 전용 배열의 [bucket] 원소를 가리킴.
		 *         다른 CPU와 공유하지 않으므로 잠금 없이 안전하게 쓸 수 있다. */
		stat = &per_cpu_ptr(cb->cpu_stat, cpu)[bucket];
		blk_rq_stat_add(stat, value);  /* [한국어] per-cpu bucket에 지연값 누적:
		                                *         min/max 갱신, batch 증가, nr_samples 증가 */
	}
	put_cpu();          /* [한국어] 선점 재활성화: per-cpu 버퍼 접근 완료, CPU 이주 허용 재개 */
	rcu_read_unlock();  /* [한국어] RCU read 크리티컬 섹션 종료: 이제 grace period가 진행될 수
	                     *         있으며, 제거된 콜백의 메모리 해제가 예약될 수 있다. */
}

/*
 * [한국어]
 * blk_stat_timer_fn - 주기적 통계 집계 타이머 핸들러.
 *
 * @t: 만료된 타이머 포인터. timer_container_of()로 blk_stat_callback을 복원.
 * @return: 없음 (void).
 *
 * 이 함수는 blk_stat_callback에 등록된 타이머가 만료될 때 호출되는 핸들러이다.
 * 먼저 전역 stat 배열을 초기화하고, 모든 온라인 CPU의 per-cpu bucket을 순회하며
 * 전역 stat으로 병합한 뒤 각 per-cpu 버퍼를 초기화한다. 집계가 끝나면
 * 등록된 timer_fn(wbt의 wbt_timer_fn, kyber의 kyber_stat_timer_fn 등)을 호출하여
 * 상위 모듈이 지연 통계를 소비하고 정책을 갱신하게 한다.
 * 실행 컨텍스트: 타이머 softirq. for_each_online_cpu는 CPU 핫플러그와 race가 있을 수
 *               있지만, 커널 타이머 설계상 허용되는 수준이다.
 * 에러 경로: 없음 (통계 집계 실패 시 별도 에러 경로 없이 다음 타이머 주기에 재시도).
 *
 * 호출 체인:
 *   mod_timer → [타이머 softirq] → [이 함수] → blk_rq_stat_sum
 *                                              → blk_rq_stat_init
 *                                              → cb->timer_fn (wbt/iolatency/kyber)
 */
static void blk_stat_timer_fn(struct timer_list *t)
{
	/* [한국어] 타이머 구조체 포인터에서 포함하는 blk_stat_callback 객체를 역산:
	 *         timer_container_of()는 container_of()의 타이머 특화 버전. */
	struct blk_stat_callback *cb = timer_container_of(cb, t, timer);
	unsigned int bucket;  /* [한국어] bucket 인덱스 루프 변수: 0부터 cb->buckets-1까지 */
	int cpu;              /* [한국어] 온라인 CPU 번호 루프 변수: for_each_online_cpu에서 사용 */

	/* [한국어] 전역 stat 배열 초기화: 새 집계 윈도우를 위해 모든 bucket을 리셋.
	 *         이 루프 이후 blk_rq_stat_sum()이 per-cpu 값을 하나씩 여기에 병합한다. */
	for (bucket = 0; bucket < cb->buckets; bucket++)  /* [한국어] read, write 등 각 bucket 순회 */
		blk_rq_stat_init(&cb->stat[bucket]);  /* [한국어] 전역 bucket 초기화(min=-1ULL, 나머지 0) */

	/* [한국어] 모든 온라인 CPU의 per-cpu 통계를 전역 stat으로 병합:
	 *         각 CPU마다 완료 IRQ를 처리하면서 독립적으로 누적한 지연값을
	 *         하나의 전역 histogram으로 합산한다. 병합 후 per-cpu 버퍼를 초기화하여
	 *         다음 집계 윈도우를 위해 비워 둔다. */
	for_each_online_cpu(cpu) {  /* [한국어] CPU 핫플러그로 갑자기 오프라인이 된 CPU는 건너뜀:
	                             *         그 CPU의 잔여 통계는 다음 윈도우에서 처리될 수 있음 */
		struct blk_rq_stat *cpu_stat;

		/* [한국어] 이 CPU의 per-cpu bucket 배열 포인터 획득: cb->cpu_stat는 percpu 포인터. */
		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);
		for (bucket = 0; bucket < cb->buckets; bucket++) {  /* [한국어] 각 bucket(read/write/기타) 처리 */
			/* [한국어] per-cpu bucket을 전역 stat에 가중 평균으로 병합:
			 *         cpu_stat[bucket].batch를 분자로, nr_samples를 가중치로 사용 */
			blk_rq_stat_sum(&cb->stat[bucket], &cpu_stat[bucket]);
			/* [한국어] 병합 완료된 per-cpu 버퍼를 초기화: 다음 집계 윈도우의
			 *         blk_stat_add() 호출을 위해 깨끗한 상태로 만든다. */
			blk_rq_stat_init(&cpu_stat[bucket]);
		}
	}

	/* [한국어] 상위 제어 모듈의 콜백 호출: cb->timer_fn은 wbt_timer_fn, iolatency_timer_fn,
	 *         kyber_stat_timer_fn 등으로, 병합된 전역 stat을 읽어 정책 파라미터를 조정함. */
	cb->timer_fn(cb);
}

/*
 * [한국어]
 * blk_stat_alloc_callback - blk_stat_callback 객체를 할당하고 초기화한다.
 *
 * @timer_fn: 타이머 만료 시 호출될 상위 모듈 콜백. wbt_timer_fn, kyber_stat_timer_fn 등.
 * @bucket_fn: request → bucket 인덱스 변환 함수. 보통 read/write를 0/1로 분류.
 * @buckets: 생성할 bucket 수. read/write 구분이면 2, 더 세분화하면 그 이상.
 * @data: 상위 모듈의 private 데이터 포인터. timer_fn 내에서 cb->data로 접근.
 * @return: 성공 시 초기화된 blk_stat_callback 포인터, 할당 실패 시 NULL.
 *
 * 세 단계로 메모리를 할당한다:
 *   1. blk_stat_callback 구조체 자체(kmalloc_obj)
 *   2. buckets개의 blk_rq_stat 전역 배열(kmalloc_objs)
 *   3. percpu 포인터(buckets * sizeof(blk_rq_stat)): 모든 CPU에 per-cpu 공간 확보
 * 어느 단계에서든 실패하면 이미 할당된 메모리를 롤백하고 NULL을 반환한다.
 * 호출자는 반환받은 객체를 blk_stat_add_callback()으로 큐에 등록해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(wbt/iolatency/kyber 초기화 중). 잠금 없음.
 * 에러 경로: 3단계 중 어느 단계의 할당이 실패해도 이미 할당된 자원을 kfree/free_percpu로
 *            해제하고 NULL 반환. 호출자는 NULL을 받으면 초기화를 중단해야 한다.
 *
 * 호출 체인:
 *   blk_wbt_init / blk_iolatency_init / kyber_init → [이 함수] → blk_stat_add_callback
 */
struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data)
{
	struct blk_stat_callback *cb;  /* [한국어] 할당할 콜백 객체 포인터 */

	/* [한국어] 콜백 구조체 자체 할당: kmalloc_obj()는 sizeof(*cb) 크기를 GFP_KERNEL로 할당 */
	cb = kmalloc_obj(*cb);
	if (!cb)
		return NULL;  /* [한국어] 콜백 구조체 할당 실패: 상위 모듈 초기화 중단 신호 */

	/* [한국어] 전역 bucket 배열 할당: buckets개의 blk_rq_stat 원소를 갖는 배열.
	 *         타이머 만료 시 per-cpu 통계가 병합되는 전역 histogram. */
	cb->stat = kmalloc_objs(struct blk_rq_stat, buckets);
	if (!cb->stat) {
		kfree(cb);   /* [한국어] 구조체만 할당된 상태에서 실패: 구조체 롤백 후 NULL 반환 */
		return NULL; /* [한국어] 전역 stat 배열 할당 실패 */
	}
	/* [한국어] per-cpu 통계 공간 할당: 각 CPU마다 buckets개의 blk_rq_stat를 확보.
	 *         __alloc_percpu()는 정렬까지 지정 가능한 저수준 percpu 할당 함수. */
	cb->cpu_stat = __alloc_percpu(buckets * sizeof(struct blk_rq_stat),
				      __alignof__(struct blk_rq_stat));
	/* [한국어] __alignof__(struct blk_rq_stat): blk_rq_stat의 자연 정렬 경계.
	 *         캐시 라인 정렬로 false sharing을 줄이기 위한 정렬 지정. */
	if (!cb->cpu_stat) {
		kfree(cb->stat);  /* [한국어] 전역 배열 롤백 */
		kfree(cb);        /* [한국어] 구조체 롤백 */
		return NULL;      /* [한국어] per-cpu 공간 할당 실패: lockless 통계 수집 불가 */
	}

	cb->timer_fn = timer_fn;    /* [한국어] 상위 모듈의 타이머 만료 핸들러 등록:
	                              *         blk_stat_timer_fn이 집계 후 이 함수를 호출 */
	cb->bucket_fn = bucket_fn;  /* [한국어] request → bucket 분류 함수 연결:
	                              *         blk_stat_add()에서 bucket 인덱스를 얻는 데 사용 */
	cb->data = data;            /* [한국어] 상위 모듈 private 포인터 저장: timer_fn이 접근 */
	cb->buckets = buckets;      /* [한국어] bucket 수 저장: 루프 상한으로 사용 */
	/* [한국어] 타이머 초기화: blk_stat_timer_fn을 핸들러로, 플래그 0(기본 타이머)으로 설정.
	 *         이후 blk_stat_activate_callback()에서 mod_timer()로 첫 만료 시각을 설정한다. */
	timer_setup(&cb->timer, blk_stat_timer_fn, 0);

	return cb;  /* [한국어] 완전히 초기화된 콜백 객체 반환: 호출자가 blk_stat_add_callback()으로 등록 */
}

/*
 * [한국어]
 * blk_stat_add_callback - 콜백을 request_queue의 stats 리스트에 RCU 등록하고
 *                         QUEUE_FLAG_STATS를 활성화한다.
 *
 * @q:  콜백을 등록할 request_queue.
 * @cb: blk_stat_alloc_callback()으로 할당된 콜백 객체.
 * @return: 없음 (void).
 *
 * 등록 전 모든 possible CPU의 per-cpu bucket을 초기화하여 이전 윈도우의 잔여
 * 통계가 첫 집계에 섞이지 않도록 한다. 이후 spinlock_irqsave로 보호된 상태에서
 * RCU 리스트에 콜백을 추가하고 QUEUE_FLAG_STATS를 설정하여 blk_mq_start_request()가
 * io_start_time_ns를 기록하기 시작하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(wbt/iolatency 초기화 중). spin_lock_irqsave 사용.
 * 에러 경로: 없음 (실패 불가. 호출자가 유효한 cb를 전달한다고 가정).
 *
 * 호출 체인:
 *   blk_wbt_init / blk_iolatency_init / kyber_init →
 *   blk_stat_alloc_callback → [이 함수] → (완료 시) blk_stat_add 경로 활성화
 */
void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb)
{
	unsigned int bucket;    /* [한국어] bucket 인덱스 루프 변수 */
	unsigned long flags;    /* [한국어] spin_lock_irqsave/irqrestore를 위한 이전 IRQ 플래그 저장 */
	int cpu;                /* [한국어] possible CPU 번호 루프 변수 */

	/* [한국어] 모든 possible CPU(오프라인 포함)의 per-cpu 버퍼를 사전 초기화:
	 *         등록 직전 잔여 통계가 첫 타이머 만료 시 전역 stat에 섞이는 것을 방지.
	 *         for_each_possible_cpu는 현재 오프라인 CPU도 포함(future 핫플러그 대비). */
	for_each_possible_cpu(cpu) {
		struct blk_rq_stat *cpu_stat;

		/* [한국어] 이 CPU의 per-cpu 버퍼 획득 */
		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);
		for (bucket = 0; bucket < cb->buckets; bucket++)  /* [한국어] 모든 bucket 초기화 */
			blk_rq_stat_init(&cpu_stat[bucket]);  /* [한국어] min=-1ULL, 나머지 0으로 리셋 */
	}

	/* [한국어] 콜백 리스트 수정과 STATS 플래그 설정을 원자적으로 수행.
	 *         IRQ를 비활성화(irqsave)하는 이유: 완료 IRQ 핸들러도 STATS 플래그를 참조하므로
	 *         IRQ 컨텍스트와의 race를 방지해야 한다. */
	spin_lock_irqsave(&q->stats->lock, flags);
	/* [한국어] RCU 리스트 끝에 콜백 추가: list_add_tail_rcu()는 메모리 배리어를 삽입하여
	 *         이후 blk_stat_add()의 list_for_each_entry_rcu()가 이 항목을 볼 수 있게 보장 */
	list_add_tail_rcu(&cb->list, &q->stats->callbacks);
	/* [한국어] QUEUE_FLAG_STATS 설정: 이 플래그가 설정되면 blk_mq_start_request()에서
	 *         io_start_time_ns를 기록하기 시작. blk_stat_add()가 의미 있는 값을 계산하려면
	 *         이 플래그가 반드시 설정되어 있어야 한다. */
	blk_queue_flag_set(QUEUE_FLAG_STATS, q);
	spin_unlock_irqrestore(&q->stats->lock, flags);  /* [한국어] 락 해제 및 IRQ 복원 */
}

/*
 * [한국어]
 * blk_stat_remove_callback - 콜백을 request_queue의 stats 리스트에서 RCU 제거하고,
 *                            필요시 QUEUE_FLAG_STATS를 클리어하며 타이머를 정지한다.
 *
 * @q:  콜백이 등록된 request_queue.
 * @cb: 제거할 콜백 객체.
 * @return: 없음 (void).
 *
 * RCU 리스트에서 콜백을 제거한 후, callbacks 리스트가 비어 있고 기본 accounting 사용자도
 * 없으면(accounting == 0) QUEUE_FLAG_STATS를 클리어하여 io_start_time_ns 기록을 중단시킨다.
 * 이후 timer_delete_sync()로 타이머가 완전히 만료·완료될 때까지 동기적으로 대기한 뒤 반환.
 * RCU grace period 보장: 리스트 제거 직후 완료 핫패스가 아직 이 콜백을 참조 중일 수 있으므로,
 * 실제 메모리 해제는 blk_stat_free_callback()의 call_rcu()로 유예한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(scheduler/wbt exit 경로). 블로킹 가능.
 *
 * 호출 체인:
 *   blk_wbt_exit / blk_iolatency_exit / kyber_exit → [이 함수] → blk_stat_free_callback
 */
void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb)
{
	unsigned long flags;  /* [한국어] spin_lock_irqsave/irqrestore용 이전 IRQ 플래그 */

	/* [한국어] IRQ 비활성화: 완료 IRQ가 STATS 플래그를 확인하는 동안 클리어되는 race 방지 */
	spin_lock_irqsave(&q->stats->lock, flags);
	/* [한국어] RCU 리스트에서 콜백 제거: 이후 blk_stat_add()의 list_for_each_entry_rcu()에서
	 *         이 항목이 더 이상 보이지 않는다(grace period 이후). 현재 순회 중인 경우는
	 *         RCU 보호로 안전하게 완료된다. */
	list_del_rcu(&cb->list);
	/* [한국어] STATS 플래그 클리어 조건: callbacks 리스트가 비어 있고(리스트 제거 후 확인)
	 *         AND 기본 accounting 사용자가 0명인 경우. 하나라도 남아 있으면 플래그 유지. */
	if (list_empty(&q->stats->callbacks) && !q->stats->accounting)
		/* [한국어] STATS 플래그 클리어: blk_mq_start_request()의 io_start_time_ns 기록 중단.
		 *         이로써 완료 경로의 blk_stat_add()가 호출되더라도 지연 계산이 0이 되어
		 *         통계 수집이 무의미해진다(활성 콜백도 없으므로 순회 자체가 빈 루프). */
		blk_queue_flag_clear(QUEUE_FLAG_STATS, q);
	spin_unlock_irqrestore(&q->stats->lock, flags);

	/* [한국어] 타이머 동기적 삭제: 이 함수가 반환되기 전에 blk_stat_timer_fn이 완전히
	 *         종료됨을 보장. 타이머가 실행 중이면 완료될 때까지 블로킹 대기.
	 *         이 후 blk_stat_free_callback()으로 메모리를 RCU 방식으로 해제해야 한다. */
	timer_delete_sync(&cb->timer);
}

/*
 * [한국어]
 * blk_stat_free_callback_rcu - RCU grace period 이후 콜백 메모리를 해제하는 RCU 콜백.
 *
 * @head: call_rcu()에 전달된 rcu_head 포인터. container_of로 blk_stat_callback 복원.
 * @return: 없음 (void).
 *
 * blk_stat_free_callback()이 call_rcu()로 이 함수를 예약하면, 모든 현재 RCU read-side
 * 크리티컬 섹션(blk_stat_add의 rcu_read_lock/rcu_read_unlock 사이)이 완료된 후
 * RCU 콜백 softirq에서 이 함수가 호출된다. 세 가지 자원(per-cpu 배열, 전역 stat, 구조체)을
 * 순서대로 해제한다.
 * 실행 컨텍스트: RCU 콜백 softirq. GFP_ATOMIC 컨텍스트에서 실행되므로 블로킹 불가.
 *
 * 호출 체인:
 *   blk_stat_free_callback → call_rcu → [RCU grace period] → [이 함수]
 */
static void blk_stat_free_callback_rcu(struct rcu_head *head)
{
	struct blk_stat_callback *cb;  /* [한국어] 해제할 콜백 객체 포인터 */

	/* [한국어] rcu_head 포인터에서 포함하는 blk_stat_callback 객체를 역산:
	 *         cb->rcu 필드의 오프셋을 이용한 container_of 패턴 */
	cb = container_of(head, struct blk_stat_callback, rcu);
	free_percpu(cb->cpu_stat);  /* [한국어] per-cpu 버퍼 해제: 모든 CPU의 bucket 배열 반납.
	                              *         blk_stat_add()에서 per_cpu_ptr()로 접근하던 공간 */
	kfree(cb->stat);            /* [한국어] 전역 histogram 배열 해제: buckets개의 blk_rq_stat 반납 */
	kfree(cb);                  /* [한국어] 콜백 구조체 자체 해제: blk_stat_alloc_callback()에서
	                              *         kmalloc_obj()로 할당한 메모리 반납 */
}

/*
 * [한국어]
 * blk_stat_free_callback - 콜백 메모리를 RCU grace period 이후 안전하게 해제한다.
 *
 * @cb: 해제할 콜백 객체. NULL이면 아무것도 하지 않음.
 * @return: 없음 (void).
 *
 * blk_stat_remove_callback()으로 RCU 리스트에서 제거한 후 이 함수를 호출하면,
 * 현재 blk_stat_add()의 rcu_read_lock 섹션이 모두 완료된 뒤 blk_stat_free_callback_rcu()를
 * 통해 메모리가 해제된다. 직접 kfree를 하면 아직 순회 중인 완료 핫패스가 해제된 메모리를
 * 참조하는 UAF(Use-After-Free) 위험이 있으므로 RCU 유예가 필수이다.
 * 실행 컨텍스트: 프로세스 컨텍스트. call_rcu()는 블로킹하지 않고 즉시 반환.
 *
 * 호출 체인:
 *   blk_wbt_exit / blk_iolatency_exit → blk_stat_remove_callback →
 *   [이 함수] → call_rcu → [나중에] → blk_stat_free_callback_rcu
 */
void blk_stat_free_callback(struct blk_stat_callback *cb)
{
	if (cb)  /* [한국어] NULL 안전 검사: NULL이 전달되면 아무것도 하지 않음 */
		/* [한국어] RCU 유예 메모리 해제 예약: 현재 진행 중인 모든 RCU read-side 섹션이
		 *         완료된 다음 softirq에서 blk_stat_free_callback_rcu()가 호출된다. */
		call_rcu(&cb->rcu, blk_stat_free_callback_rcu);
}

/*
 * [한국어]
 * blk_stat_disable_accounting - 기본 io_start_time_ns 기록의 사용자를 한 명 감소시키고,
 *                               더 이상 사용자가 없으면 QUEUE_FLAG_STATS를 클리어한다.
 *
 * @q: accounting을 비활성화할 request_queue.
 * @return: 없음 (void).
 *
 * blk-iocost, bfq 등이 콜백 없이 직접 rq->io_start_time_ns를 읽기 위해
 * blk_stat_enable_accounting()을 호출했다가, 비활성화할 때 이 함수를 호출한다.
 * accounting 카운터를 감소시키고, 0이 되면서 callbacks도 비어 있으면 STATS 플래그를
 * 클리어하여 io_start_time_ns 기록을 중단시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O scheduler/cost model 비활성화 경로).
 *
 * 호출 체인:
 *   blk_iocost_exit / bfq_exit → [이 함수]
 */
void blk_stat_disable_accounting(struct request_queue *q)
{
	unsigned long flags;  /* [한국어] IRQ 플래그 저장 */

	/* [한국어] accounting 감소와 STATS 플래그 클리어를 원자적으로 수행:
	 *         IRQ 비활성화는 완료 IRQ와의 race를 방지 */
	spin_lock_irqsave(&q->stats->lock, flags);
	/* [한국어] accounting을 감소시킨 뒤 0인지 확인(--q->stats->accounting의 결과가 0):
	 *         AND callbacks 리스트도 비어 있으면 더 이상 타임스탬프 기록이 불필요. */
	if (!--q->stats->accounting && list_empty(&q->stats->callbacks))
		/* [한국어] STATS 플래그 클리어: blk_mq_start_request()에서 io_start_time_ns를
		 *         더 이상 기록하지 않게 함. rq_flags의 RQF_STATS도 함께 관리됨. */
		blk_queue_flag_clear(QUEUE_FLAG_STATS, q);
	spin_unlock_irqrestore(&q->stats->lock, flags);
}
EXPORT_SYMBOL_GPL(blk_stat_disable_accounting);

/*
 * [한국어]
 * blk_stat_enable_accounting - 기본 io_start_time_ns 기록의 사용자를 한 명 추가하고,
 *                              첫 번째 사용자이면 QUEUE_FLAG_STATS를 설정한다.
 *
 * @q: accounting을 활성화할 request_queue.
 * @return: 없음 (void).
 *
 * blk-iocost, bfq 등이 콜백 없이 직접 rq->io_start_time_ns를 읽고 싶을 때 이 함수를
 * 호출하여 io_start_time_ns 기록을 활성화한다. accounting 카운터를 증가시키고, 처음으로
 * 활성화되는 경우(이전 값이 0) 이면서 callbacks도 비어 있으면 STATS 플래그를 설정한다.
 * callbacks가 있으면 이미 STATS 플래그가 설정되어 있으므로 중복 설정해도 무방하나,
 * 불필요한 중복 호출을 피하기 위해 callbacks가 비어 있을 때만 플래그를 설정한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O scheduler/cost model 활성화 경로).
 *
 * 호출 체인:
 *   blk_iocost_init / bfq_init → [이 함수]
 */
void blk_stat_enable_accounting(struct request_queue *q)
{
	unsigned long flags;  /* [한국어] IRQ 플래그 저장 */

	/* [한국어] accounting 증가와 STATS 플래그 설정을 원자적으로 수행 */
	spin_lock_irqsave(&q->stats->lock, flags);
	/* [한국어] accounting을 증가시키기 전 값이 0이었고(q->stats->accounting++의 이전 값이 0)
	 *         AND callbacks가 비어 있으면 STATS 플래그를 새로 설정:
	 *         callbacks가 있으면 이미 STATS 플래그가 설정되어 있음. */
	if (!q->stats->accounting++ && list_empty(&q->stats->callbacks))
		/* [한국어] STATS 플래그 설정: blk_mq_start_request()에서 io_start_time_ns를
		 *         기록하기 시작. 이 플래그가 없으면 io_start_time_ns=0으로 남아
		 *         blk_stat_add()의 지연 계산이 무의미해진다. */
		blk_queue_flag_set(QUEUE_FLAG_STATS, q);
	spin_unlock_irqrestore(&q->stats->lock, flags);
}
EXPORT_SYMBOL_GPL(blk_stat_enable_accounting);

/*
 * [한국어]
 * blk_alloc_queue_stats - request_queue의 통계 관리 구조체를 할당하고 초기화한다.
 *
 * @return: 성공 시 초기화된 blk_queue_stats 포인터, 실패 시 NULL.
 *
 * request_queue 생성 시(blk-core.c) q->stats에 할당하여 통계 인프라를 준비한다.
 * callbacks 리스트를 초기화하고, spinlock을 초기화하며, accounting을 0으로 설정한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(request_queue 할당 경로). 슬립 가능.
 * 에러 경로: kmalloc_obj 실패 시 NULL 반환. 호출자는 NULL이면 queue 생성을 실패 처리.
 *
 * 호출 체인:
 *   blk_alloc_queue (blk-core.c) → [이 함수] → q->stats 설정
 */
struct blk_queue_stats *blk_alloc_queue_stats(void)
{
	struct blk_queue_stats *stats;  /* [한국어] 할당할 통계 관리 구조체 포인터 */

	/* [한국어] 통계 관리 구조체 할당: sizeof(*stats) 크기의 메모리. GFP_KERNEL이므로 슬립 가능 */
	stats = kmalloc_obj(*stats);
	if (!stats)
		return NULL;  /* [한국어] 할당 실패: request_queue 생성 실패 신호. q->stats는 NULL 상태 */

	INIT_LIST_HEAD(&stats->callbacks);  /* [한국어] callbacks RCU 리스트 헤드 초기화:
	                                     *         list_empty()가 true가 되는 상태 */
	spin_lock_init(&stats->lock);       /* [한국어] spinlock 초기화: 이후 spin_lock_irqsave로 사용 */
	stats->accounting = 0;             /* [한국어] accounting 카운터를 0으로 초기화:
	                                     *         아직 기본 타임스탬프 기록 사용자 없음 */

	return stats;  /* [한국어] 완전히 초기화된 통계 관리자 반환: blk-core.c가 q->stats에 저장 */
}

/*
 * [한국어]
 * blk_free_queue_stats - request_queue의 통계 관리 구조체를 해제한다.
 *
 * @stats: 해제할 blk_queue_stats 포인터. NULL이면 바로 반환.
 * @return: 없음 (void).
 *
 * request_queue 해제 시(blk-core.c) 호출된다. 해제 시점에 callbacks 리스트가 비어 있지
 * 않으면 WARN_ON으로 경고를 발생시킨다 — 이는 상위 모듈이 콜백을 제거하지 않고 queue를
 * 해제하려는 버그를 나타낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트(queue 해제 경로). 모든 I/O가 완료된 후 호출 보장.
 *
 * 호출 체인:
 *   blk_cleanup_queue / blk_put_queue (blk-core.c) → [이 함수]
 */
void blk_free_queue_stats(struct blk_queue_stats *stats)
{
	if (!stats)   /* [한국어] NULL 안전 검사: 할당 실패로 NULL인 경우 안전하게 반환 */
		return;

	/* [한국어] callbacks 리스트가 비어 있지 않으면 버그 경고:
	 *         queue 해제 전 모든 콜백이 blk_stat_remove_callback()으로 제거되어야 함.
	 *         WARN_ON은 커널 로그에 스택 트레이스를 출력하고 계속 실행 */
	WARN_ON(!list_empty(&stats->callbacks));

	kfree(stats);  /* [한국어] 통계 관리 구조체 메모리 반납: blk_alloc_queue_stats()에서 할당 */
}
