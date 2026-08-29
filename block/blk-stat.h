/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] request_queue별 I/O 완료 지연(latency) 통계 콜백 인터페이스 (block/blk-stat.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/blk-stat.c가 구현하는 I/O 완료 지연 시간(latency) 통계
 * 수집 인프라의 공개 API와 핵심 자료구조 struct blk_stat_callback을 선언한다.
 * request_queue 하나에 등록되는 blk_stat_callback은, 수집 윈도우(@timer)가
 * 활성 상태인 동안 request 완료 지연을 @bucket_fn이 지정하는 latency bucket
 * 으로 분류하여 per-cpu 버퍼(@cpu_stat)에 누적하고, 윈도우가 만료되면 전역
 * 배열(@stat)로 병합한 뒤 @timer_fn을 호출해 상위 소비자(blk-wbt,
 * blk-iolatency, kyber-iosched 등)에게 결과를 전달한다. 이 파일 자체는
 * 구조체 정의·인라인 헬퍼·extern 선언만 담고 있으며, per-cpu 누적/병합/
 * 타이머 처리의 실제 구현은 block/blk-stat.c에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층 I/O 파이프라인에서 이 헤더가 정의하는 콜백 메커니즘은 "완료 후
 * 통계 수집" 단계에 위치한다. 전형적 호출 체인은 다음과 같다:
 *   blk_mq_start_request (QUEUE_FLAG_STATS가 켜져 있으면 io_start_time_ns
 *   기록) → [디바이스 드라이버 처리: 예) NVMe라면 SQ 제출 → 컨트롤러 처리 →
 *   CQ 수신] → blk_mq_end_request → blk_stat_add(rq, now) (block/blk-stat.c,
 *   이 헤더가 선언) → request_queue->stats->callbacks를 RCU로 순회하며 각
 *   blk_stat_callback의 bucket_fn으로 bucket 인덱스 결정 → per-cpu
 *   cpu_stat[bucket]에 누적 → blk_stat_activate_nsecs()/msecs()로 설정된
 *   타이머 만료 → blk_stat_timer_fn(block/blk-stat.c 내부 static 함수, 이
 *   헤더에는 선언이 없음) → blk_rq_stat_sum()으로 전역 stat[]에 병합 →
 *   cb->timer_fn(cb) 호출로 상위 소비자에게 통지. 이 헤더의 인라인 함수
 *   (blk_stat_is_active, blk_stat_activate_nsecs/msecs, blk_stat_deactivate)는
 *   상위 소비자가 수집 윈도우를 열고 닫는 데 쓰는 프런트엔드 API이다. 실행
 *   컨텍스트는 request 완료 IRQ/softirq(blk_stat_add 호출측), 타이머
 *   softirq(timer_fn 만료측), 프로세스 컨텍스트(등록/해제 및 인라인 헬퍼
 *   호출측)로 나뉜다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더에 의존하는 모듈:
 *   - block/blk-stat.c: 이 헤더가 선언한 모든 함수의 실제 구현체.
 *   - block/blk-mq.c: request 완료 경로에서 blk_stat_add()를 호출하고,
 *     QUEUE_FLAG_STATS 플래그로 io_start_time_ns 기록 여부를 결정한다.
 *   - block/blk-core.c: request_queue 생성/해제 시 blk_alloc_queue_stats()/
 *     blk_free_queue_stats()를 호출해 struct blk_queue_stats(q->stats)를
 *     관리한다.
 * 이 헤더에 의존하는(포함하는) 소비자 모듈:
 *   - block/blk-wbt.c, block/blk-iolatency.c, block/kyber-iosched.c 등:
 *     blk_stat_alloc_callback()으로 콜백을 만들고 blk_stat_add_callback()으로
 *     등록한 뒤, blk_stat_activate_nsecs()/msecs()로 수집 윈도우를 열며,
 *     timer_fn 콜백에서 병합된 latency 분포를 읽어 쓰로틀링/스케줄링 정책을
 *     조정한다.
 * 데이터 흐름: request 완료 → blk_stat_add() → bucket_fn 분류 → cpu_stat
 * (per-cpu) 누적 → 타이머 만료 → stat(전역) 병합 → timer_fn(상위 소비자
 * 정책 갱신). 공유하는 핵심 자료구조:
 *   - struct blk_stat_callback: 이 헤더에 전체 정의. RCU list 노드, 타이머,
 *     per-cpu/전역 bucket 배열, bucket_fn/timer_fn, 소비자 사설 데이터
 *     (data)를 한 객체에 묶는다.
 *   - struct blk_rq_stat: 이 헤더에서는 완전한 타입 정의 없이 포인터
 *     (struct blk_rq_stat *, __percpu *)로만 등장하는 불완전 타입이다. 실제
 *     필드는 block/blk-stat.c의 사용 패턴(stat->min/max/mean/nr_samples/
 *     batch)으로 확인되며, 단일 bucket의 최소/최대/평균 지연과 샘플 수,
 *     누적 합(batch)을 담는 히스토그램 구조체이다. 이 구조체의 정의부가
 *     현재 이 저장소(sparse checkout, 일부 파일만 체크아웃됨)에는 보이지
 *     않는다 — include/linux/blk_types.h 등 다른 헤더에서 온다.
 *   - struct blk_queue_stats: request_queue->stats. callbacks 리스트, lock,
 *     accounting 카운터를 담는 불투명 구조체로, 이 헤더에는 전방 선언과
 *     포인터를 반환/인자로 받는 함수(blk_alloc_queue_stats 등)만 노출된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct blk_stat_callback: request_queue 하나에 매달리는 통계 콜백 객체.
 *   list(RCU 리스트 노드), timer(수집 윈도우 타이머), cpu_stat(per-cpu
 *   bucket 배열), bucket_fn(request→bucket 분류자), buckets(bucket 개수),
 *   stat(병합된 전역 bucket 배열), timer_fn(윈도우 만료 콜백), data(소비자
 *   사설 데이터), rcu(RCU 지연 해제용 헤드)로 구성된다.
 * - blk_stat_alloc_callback() : timer_fn/bucket_fn/buckets/data로 콜백
 *   객체를 할당(전역 stat 배열 + per-cpu cpu_stat 포함, block/blk-stat.c
 *   구현).
 * - blk_stat_add_callback() / blk_stat_remove_callback() : 콜백을
 *   request_queue의 RCU 리스트에 등록/해제하고 QUEUE_FLAG_STATS를 갱신.
 * - blk_stat_activate_nsecs() / blk_stat_activate_msecs() /
 *   blk_stat_deactivate() / blk_stat_is_active() : 수집 윈도우 타이머를
 *   열고 닫고 상태를 조회하는 인라인 프런트엔드.
 * - blk_stat_add() : (block/blk-stat.c 구현) request 완료 시 등록된 모든
 *   활성 콜백에 지연값을 기록하는 핫패스 진입점.
 * - blk_rq_stat_add()/_sum()/_init() : (block/blk-stat.c 구현) 단일
 *   bucket에 대한 샘플 추가/병합/리셋.
 */
#ifndef BLK_STAT_H
#define BLK_STAT_H

#include <linux/kernel.h>	/* [한국어] u64 등 기본 타입과 min()/max()/div_u64() 커널 유틸리티
				 * 제공 — latency 계산과 bucket 인덱스 처리에 사용(block/blk-stat.c) */
#include <linux/blkdev.h>	/* [한국어] struct request_queue, struct request 정의 제공 —
				 * blk_stat_add()가 rq->q, rq->io_start_time_ns에 접근하는 데 필요 */
#include <linux/ktime.h>	/* [한국어] ktime 관련 타입/헬퍼 제공 — completion 시각(now)과
				 * io_start_time_ns의 차이(지연)를 계산하는 기반 */
#include <linux/rcupdate.h>	/* [한국어] RCU(Read-Copy-Update) 리스트 순회/보호 API 제공 —
				 * blk_stat_add() 완료 핫패스가 callbacks 리스트를 락 없이 순회하게 함 */
#include <linux/timer.h>	/* [한국어] struct timer_list, mod_timer(), timer_delete_sync() 등
				 * 커널 타이머 API 제공 — 수집 윈도우(latency window)의 개폐를 구현하는 기반 */

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
	 */
	struct list_head list;
	/* [한국어] 하나의 request_queue(q->stats->callbacks)에 매달리는 RCU
	 * 연결 리스트의 한 노드.
	 * 설정자: blk_stat_add_callback()이 list_add_tail_rcu()로 이 콜백을
	 *        request_queue의 리스트 끝에 추가한다.
	 * 읽는 자: blk_stat_add()의 완료 핫패스가 list_for_each_entry_rcu()로
	 *         이 리스트를 락 없이(lockless) 순회하며 각 콜백의 bucket_fn을
	 *         호출한다. blk_stat_remove_callback()이 list_del_rcu()로 제거.
	 * 값 범위: 유효한 리스트 노드(자기 자신을 가리키는 초기 상태 포함).
	 *         request_queue에 등록되기 전까지는 아직 어떤 리스트에도 연결
	 *         되지 않은 상태(blk_stat_alloc_callback()은 list_head를
	 *         명시적으로 초기화하지 않으므로, 사용 전 반드시
	 *         blk_stat_add_callback()으로 등록해야 안전).
	 * 동기화: 추가/제거는 q->stats->lock(spinlock_irqsave)으로 보호되고,
	 *         순회는 RCU read-side critical section(rcu_read_lock)으로
	 *         보호된다 — 완료 IRQ 경로에서 락 경합 없이 읽을 수 있는 핵심
	 *         설계. */

	/**
	 * @timer: Timer for the next callback invocation.
	 */
	struct timer_list timer;
	/* [한국어] 이 콜백의 latency 수집 윈도우(집계 주기)를 구현하는 커널
	 * 타이머.
	 * 설정자: blk_stat_alloc_callback()이 timer_setup()으로 핸들러를
	 *        blk_stat_timer_fn(block/blk-stat.c 내부 static 함수)으로
	 *        연결한다. blk_stat_activate_nsecs()/blk_stat_activate_msecs()
	 *        (이 헤더의 인라인 함수)가 mod_timer()로 다음 만료 시각을 설정.
	 * 읽는 자: blk_stat_is_active()가 timer_pending()으로 타이머가 아직
	 *         살아 있는지(윈도우가 열려 있는지) 확인한다. 타이머 만료 시
	 *         커널 타이머 softirq가 blk_stat_timer_fn을 호출한다.
	 * 값 범위: pending(수집 중) 또는 expired/미설정(비활성) 상태.
	 * 동기화: blk_stat_deactivate()/blk_stat_remove_callback()이
	 *         timer_delete_sync()로 동기적으로 정지시켜, 반환 시점에
	 *         다른 CPU에서 handler가 실행 중이지 않음을 보장한다. */

	/**
	 * @cpu_stat: Per-cpu statistics buckets.
	 */
	struct blk_rq_stat __percpu *cpu_stat;
	/* [한국어] CPU별로 독립된 latency bucket 배열(buckets개 원소)을 담는
	 * per-cpu 포인터. 완료 인터럽트가 여러 CPU에서 동시에 발생해도 서로
	 * 다른 캐시라인에 쓰도록 하여 락 없이 안전하게 누적하기 위한 설계이다.
	 * 설정자: blk_stat_alloc_callback()이 __alloc_percpu()로 할당한다.
	 *        blk_stat_add_callback()과 blk_stat_timer_fn()이 각 bucket을
	 *        blk_rq_stat_init()으로 리셋한다.
	 * 읽는 자: blk_stat_add()가 per_cpu_ptr(cpu_stat, cpu)[bucket]으로
	 *         현재 CPU의 bucket을 얻어 blk_rq_stat_add()로 값을 누적한다.
	 *         blk_stat_timer_fn()이 for_each_online_cpu()로 모든 CPU의
	 *         값을 순회하며 blk_rq_stat_sum()으로 @stat에 병합한다.
	 * 값 범위: NULL이 아닌 유효한 percpu 포인터(할당 실패 시 콜백 자체를
	 *         할당하지 않음). 각 bucket 값은 blk_rq_stat_init()으로 리셋된
	 *         상태를 시작점으로 한다.
	 * 동기화: 완료 경로는 get_cpu()로 선점을 비활성화한 뒤 자신의 per-cpu
	 *         슬롯에만 쓰므로 별도 락이 필요 없다(false sharing 방지가
	 *         이 필드가 per-cpu로 분리된 이유). free_percpu()로
	 *         blk_stat_free_callback_rcu()에서 해제된다. */

	/**
	 * @bucket_fn: Given a request, returns which statistics bucket it
	 * should be accounted under. Return -1 for no bucket for this
	 * request.
	 */
	int (*bucket_fn)(const struct request *);
	/* [한국어] request 하나를 받아 어느 latency bucket(예: read=0,
	 * write=1처럼 콜백마다 정의하는 분류 기준)에 기록할지 결정하는 함수
	 * 포인터. 음수를 반환하면 이 request는 통계에서 완전히 제외된다.
	 * 설정자: blk_stat_alloc_callback() 호출 시 인자로 전달되어 저장.
	 *        (예: blk-wbt.c는 read/write 2-bucket 분류 함수를 전달)
	 * 읽는 자: blk_stat_add()가 각 콜백마다 cb->bucket_fn(rq)를 호출해
	 *         bucket 인덱스를 얻고, 그 인덱스로 cpu_stat[]/stat[]을 색인.
	 * 값 범위: [0, buckets) 구간의 정수, 또는 제외를 의미하는 음수(-1 등).
	 * 동기화: 순수 함수 호출이며 별도 동기화 상태를 갖지 않는다. 완료
	 *         IRQ/softirq 컨텍스트에서 호출되므로 슬립 불가. */

	/**
	 * @buckets: Number of statistics buckets.
	 */
	unsigned int buckets;
	/* [한국어] 이 콜백이 관리하는 latency bucket 개수. @stat 전역 배열과
	 * @cpu_stat의 각 per-cpu 슬롯 모두 이 개수만큼의 blk_rq_stat 원소를
	 * 갖는다.
	 * 설정자: blk_stat_alloc_callback() 호출 시 인자로 전달되어 저장되고,
	 *        같은 값으로 @stat/@cpu_stat의 배열 크기를 결정한다.
	 * 읽는 자: blk_stat_add(), blk_stat_add_callback(), blk_stat_timer_fn()
	 *         모두 이 값을 루프 상한으로 사용해 bucket 배열을 순회한다.
	 * 값 범위: 1 이상의 양의 정수(0이면 통계 배열이 비어 의미가 없음).
	 * 동기화: 콜백 생성 시 한 번 설정된 뒤 콜백의 생애주기 동안 불변
	 *         (immutable) — 별도 동기화 불필요. */

	/**
	 * @stat: Array of statistics buckets.
	 */
	struct blk_rq_stat *stat;
	/* [한국어] 모든 CPU의 @cpu_stat이 병합된 전역 latency histogram 배열
	 * (buckets개 원소). 타이머 만료 시점의 "최종 집계 결과"이며, 상위
	 * 소비자(timer_fn)가 실제로 참조하는 값이다.
	 * 설정자: blk_stat_alloc_callback()이 kmalloc_objs()로 할당한다.
	 *        blk_stat_timer_fn()이 매 윈도우마다 blk_rq_stat_init()으로
	 *        리셋한 뒤 blk_rq_stat_sum()으로 모든 CPU의 값을 병합해 채운다.
	 * 읽는 자: cb->timer_fn(cb)이 실행될 때 상위 소비자가 cb->stat[bucket]
	 *         을 읽어 평균/최소/최대 latency 등을 근거로 정책을 조정한다.
	 * 값 범위: NULL이 아닌 buckets개 원소의 배열(할당 실패 시 콜백 자체가
	 *         생성되지 않음).
	 * 동기화: 타이머 softirq(blk_stat_timer_fn)만이 이 배열을 쓰고, 같은
	 *         호출 스택에서 timer_fn이 읽으므로 별도 락이 필요 없다.
	 *         kfree()로 blk_stat_free_callback_rcu()에서 해제된다. */

	/**
	 * @timer_fn: Callback function.
	 */
	void (*timer_fn)(struct blk_stat_callback *);
	/* [한국어] 수집 윈도우(타이머)가 만료되고 전역 @stat 병합이 끝난
	 * 직후 호출되는 상위 소비자 콜백. wbt_timer_fn(blk-wbt.c),
	 * kyber_stat_timer_fn(kyber-iosched.c) 등이 여기에 연결된다.
	 * 설정자: blk_stat_alloc_callback() 호출 시 인자로 전달되어 저장.
	 * 읽는 자: blk_stat_timer_fn()이 for_each_online_cpu 병합을 마친 뒤
	 *         가장 마지막에 cb->timer_fn(cb)로 호출한다.
	 * 값 범위: NULL이 아닌 유효한 함수 포인터(호출자가 항상 지정해야 함).
	 * 동기화: 타이머 softirq 컨텍스트에서 호출되므로 콜백 구현체는 슬립할
	 *         수 없다. 이 콜백 내부에서 @data로 상위 모듈의 상태를 읽고
	 *         갱신할 때 필요한 락은 상위 모듈이 별도로 관리한다. */

	/**
	 * @data: Private pointer for the user.
	 */
	void *data;
	/* [한국어] 상위 소비자(스케줄러/cost model 등)가 자신의 사설 상태를
	 * 담아두는 opaque(불투명) 포인터. blk-stat.c/blk-stat.h는 이 값의
	 * 내부 구조를 알지 못하며 그대로 보관/전달만 한다.
	 * 설정자: blk_stat_alloc_callback() 호출 시 인자로 전달되어 저장.
	 * 읽는 자: cb->timer_fn(cb) 내부에서 상위 소비자가 cb->data를 자신의
	 *         타입으로 캐스팅해 상태를 읽고 갱신한다(예: bfq/wbt/kyber가
	 *         자신의 큐/디바이스 상태를 여기 저장 — 이 헤더/구현
	 *         파일만으로는 각 소비자의 구체적 캐스팅 방식까지는 확인 불가).
	 * 값 범위: 임의의 포인터 또는 NULL(소비자가 사설 상태가 필요 없는 경우).
	 * 동기화: blk-stat.c는 이 필드에 대해 어떠한 동기화도 제공하지 않는다.
	 *         가리키는 객체의 동시성 보호는 전적으로 상위 소비자 책임이다. */

	/**
	 * @rcu: rcu list head
	 */
	struct rcu_head rcu;
	/* [한국어] 이 콜백 객체 자체를 RCU grace period 이후 안전하게 해제하기
	 * 위한 rcu_head. blk_stat_add()가 rcu_read_lock 구간에서 아직 이
	 * 콜백을 참조하고 있을 수 있으므로, list_del_rcu() 직후 바로 kfree하면
	 * UAF(Use-After-Free)가 발생할 수 있어 이 필드를 매개로 지연 해제한다.
	 * 설정자: 별도 초기화 함수 없이 call_rcu(&cb->rcu, ...) 호출 시점에
	 *        커널 RCU 서브시스템이 내부적으로 사용한다.
	 * 읽는 자: blk_stat_free_callback_rcu()가 container_of(head, struct
	 *         blk_stat_callback, rcu)로 이 필드의 오프셋을 이용해 콜백
	 *         객체 시작 주소를 역산한다.
	 * 값 범위: call_rcu() 호출 전에는 사용되지 않는 예약 공간.
	 * 동기화: RCU 서브시스템이 자체적으로 grace period를 관리하며, 이
	 *         필드에 대한 별도의 락은 존재하지 않는다. */
};

/*
 * [한국어]
 * blk_alloc_queue_stats - request_queue의 통계 관리 구조체(struct
 * blk_queue_stats)를 할당한다.
 *
 * @return: 성공 시 초기화된 struct blk_queue_stats 포인터, 실패 시 NULL.
 *
 * request_queue 생성 경로(block/blk-core.c)에서 q->stats를 채우기 위해
 * 호출된다. 반환된 구조체는 이 blk_stat_callback들의 RCU 리스트(callbacks),
 * 리스트 보호 spinlock, 기본 accounting 카운터를 담는다(구체적 필드는
 * block/blk-stat.c 구현 참고). 이 헤더에서는 struct blk_queue_stats가
 * 전방 선언(불투명 타입)으로만 노출되어 호출자는 필드에 직접 접근할 수 없고
 * 반드시 blk_stat_* API를 통해서만 다뤄야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(request_queue 할당 경로). 슬립 가능
 * (block/blk-stat.c의 구현이 GFP_KERNEL 할당을 사용).
 * 에러 경로: NULL 반환 시 호출자(request_queue 할당 경로)가 전체 큐 생성을
 * 실패로 처리해야 한다.
 *
 * 호출 체인:
 *   blk_alloc_queue (block/blk-core.c) → [이 함수] → q->stats에 저장
 */
struct blk_queue_stats *blk_alloc_queue_stats(void);
/*
 * [한국어]
 * blk_free_queue_stats - blk_alloc_queue_stats()로 할당된 통계 관리
 * 구조체를 해제한다.
 *
 * @stats: 해제할 struct blk_queue_stats 포인터. NULL이면 아무 동작도 하지
 *         않는다(block/blk-stat.c 구현이 NULL 검사를 포함).
 * @return: 없음 (void).
 *
 * request_queue 해제 경로(block/blk-core.c)에서 호출되며, 해제 시점에
 * callbacks 리스트가 비어 있지 않으면 버그(콜백 미제거)로 간주해 경고를
 * 남긴다(구체 로직은 block/blk-stat.c 참고).
 * 실행 컨텍스트: 프로세스 컨텍스트(request_queue 해제 경로). 모든 I/O가
 * 완료된 뒤 호출되는 것을 전제로 한다.
 * 에러 경로: 없음(해제 함수이므로 실패 개념이 없다).
 *
 * 호출 체인:
 *   blk_cleanup_queue / blk_put_queue (block/blk-core.c) → [이 함수]
 */
void blk_free_queue_stats(struct blk_queue_stats *);

/*
 * [한국어]
 * blk_stat_add - request 완료 시 등록된 모든 활성 콜백의 bucket에 지연을
 * 기록하는 완료 핫패스 진입점 (구현: block/blk-stat.c).
 *
 * @rq: 완료된 request. rq->q로 request_queue를, rq->io_start_time_ns로
 *      시작 시각을 얻는다(io_start_time_ns는 QUEUE_FLAG_STATS가 켜진 상태에서
 *      blk_mq_start_request()가 기록).
 * @now: 완료 시점의 나노초 타임스탬프. 호출자(block/blk-mq.c의 완료 경로)가
 *       측정해서 전달한다.
 * @return: 없음 (void).
 *
 * now - rq->io_start_time_ns로 이 request의 완료 지연을 계산한 뒤,
 * request_queue에 RCU로 연결된 모든 blk_stat_callback을 순회하며 활성
 * (blk_stat_is_active) 콜백에 한해 bucket_fn으로 bucket을 결정하고 현재
 * CPU의 per-cpu 버퍼(cpu_stat[bucket])에 값을 누적한다. 상세 구현(RCU
 * 순회, get_cpu()를 통한 CPU 고정 등)은 block/blk-stat.c를 참고.
 * 실행 컨텍스트: request 완료 IRQ 또는 softirq. 슬립 불가.
 * 호출자(caller): block/blk-mq.c의 request 완료 경로
 * (block/blk-mq.c의 __blk_mq_end_request_acct — 완료 시각을 인자로 받아
 * 없어 정확한 호출 지점은 확인하지 못함).
 * 호출 대상(callee): 등록된 각 콜백의 bucket_fn, 그리고 block/blk-stat.c의
 * blk_rq_stat_add().
 * 에러 경로: bucket_fn이 음수를 반환하면 해당 콜백은 건너뛴다(통계 제외).
 *
 * 호출 체인:
 *   blk_mq_end_request → ... → [이 함수] → cb->bucket_fn → blk_rq_stat_add
 */
void blk_stat_add(struct request *rq, u64 now);

/* record time/size info in request but not add a callback */
/*
 * [한국어]
 * blk_stat_enable_accounting - 콜백 등록 없이 request의 time/size
 * accounting(io_start_time_ns 기록)만 활성화한다 (구현: block/blk-stat.c).
 *
 * @q: accounting을 활성화할 request_queue.
 * @return: 없음 (void).
 *
 * blk-iocost, bfq처럼 별도의 blk_stat_callback 없이 rq->io_start_time_ns를
 * 직접 읽고 싶은 소비자를 위한 API이다. q->stats의 참조 카운터(accounting)를
 * 증가시키고, 이 카운터가 0에서 1로 바뀌는 첫 활성화이면서 콜백 리스트도
 * 비어 있는 경우에 한해 QUEUE_FLAG_STATS를 설정한다(이미 콜백이 등록돼
 * 있으면 플래그가 이미 켜져 있으므로 중복 설정하지 않음, 구체 로직은
 * block/blk-stat.c 참고). 이 플래그가 켜져야 blk_mq_start_request()가
 * io_start_time_ns를 기록하기 시작한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O 스케줄러/cost model 초기화 경로).
 * 호출자: blk_iocost_init (block/blk-iocost.c:5574),
 * bfq_init_queue (block/bfq-iosched.c:13798).
 * 호출 대상: blk_queue_flag_set() 등 큐 플래그 조작 API(block/blk-stat.c 내부).
 * 에러 경로: 없음(카운터 증가 실패 개념이 없다).
 *
 * 호출 체인:
 *   blk_iocost_init / bfq_init_queue → [이 함수]
 */
void blk_stat_enable_accounting(struct request_queue *q);
/*
 * [한국어]
 * blk_stat_disable_accounting - blk_stat_enable_accounting()으로 늘어난
 * accounting 참조를 되돌린다 (구현: block/blk-stat.c).
 *
 * @q: accounting을 비활성화할 request_queue.
 * @return: 없음 (void).
 *
 * q->stats의 accounting 카운터를 감소시키고, 그 결과가 0이면서 콜백
 * 리스트도 비어 있으면 QUEUE_FLAG_STATS를 클리어하여 이후
 * blk_mq_start_request()가 io_start_time_ns 기록을 중단하게 한다(불필요한
 * 타임스탬프 오버헤드 제거).
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O 스케줄러/cost model 해제 경로).
 * 호출자: blk_iocost_exit (block/blk-iocost.c:5578),
 * bfq_exit_queue (block/bfq-iosched.c:13325).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   blk_iocost_exit / bfq_exit_queue → [이 함수]
 */
void blk_stat_disable_accounting(struct request_queue *q);

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
 */
/*
 * [한국어]
 * blk_stat_alloc_callback - blk_stat_callback 객체를 할당하고 초기화한다
 * (구현: block/blk-stat.c).
 *
 * @timer_fn: 수집 윈도우 만료 시 호출될 상위 소비자 콜백. cb->timer_fn에 저장.
 * @bucket_fn: request → bucket 인덱스 변환 함수. cb->bucket_fn에 저장.
 * @buckets: 생성할 bucket 개수. cb->buckets에 저장되고 stat/cpu_stat 배열
 *           크기를 결정한다.
 * @data: 상위 소비자의 사설 포인터. cb->data에 그대로 저장된다.
 * @return: 성공 시 초기화된 struct blk_stat_callback 포인터, ENOMEM(메모리
 *          부족)이면 NULL.
 *
 * struct blk_stat_callback 자체, buckets개의 전역 blk_rq_stat 배열(cb->stat),
 * buckets개 크기의 per-cpu 통계 공간(cb->cpu_stat) 세 단계로 메모리를
 * 할당한다(각 단계 실패 시 이전 단계까지 롤백 후 NULL 반환, 구체 로직은
 * block/blk-stat.c 참고). 타이머는 blk_stat_timer_fn을 핸들러로 연결해
 * timer_setup()으로 준비만 해두고, 아직 만료 시각은 설정하지 않는다(호출자가
 * blk_stat_activate_nsecs()/msecs()로 실제 윈도우를 열어야 함).
 * 실행 컨텍스트: 프로세스 컨텍스트(상위 소비자 초기화 경로). 슬립 가능.
 * 호출자: blk-wbt.c/blk-iolatency.c/kyber-iosched.c 등의 초기화 함수.
 * 호출 대상: kmalloc 계열 할당 함수, __alloc_percpu(), timer_setup()
 * (block/blk-stat.c 내부).
 * 에러 경로: 세 단계 중 어느 하나라도 할당 실패 시 이미 확보한 자원을
 * 롤백하고 NULL 반환 — 호출자는 NULL이면 자신의 초기화를 중단해야 한다.
 *
 * 호출 체인:
 *   blk_wbt_init (block/blk-wbt.c) 등 rq-qos 정책 초기화 →
 *   [이 함수] → blk_stat_add_callback
 */
struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data);

/**
 * blk_stat_add_callback() - Add a block statistics callback to be run on a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * Note that a single &struct blk_stat_callback can only be added to a single
 * &struct request_queue.
 */
/*
 * [한국어]
 * blk_stat_add_callback - 콜백을 request_queue의 stats 리스트에 RCU
 * 등록하고 QUEUE_FLAG_STATS를 활성화한다 (구현: block/blk-stat.c).
 *
 * @q: 콜백을 등록할 request_queue.
 * @cb: blk_stat_alloc_callback()으로 할당된 콜백 객체.
 * @return: 없음 (void).
 *
 * 등록 전 모든 possible CPU(오프라인 포함)의 per-cpu bucket을 초기화하여
 * 이전 잔여 통계가 첫 집계에 섞이지 않게 한 뒤, q->stats->lock으로 보호된
 * 상태에서 cb->list를 RCU 리스트 끝에 추가하고 QUEUE_FLAG_STATS를 설정한다.
 * 이 플래그가 켜져야 blk_mq_start_request()가 io_start_time_ns를 기록하기
 * 시작하며, 그래야 blk_stat_add()가 의미 있는 지연값을 계산할 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트(상위 소비자 초기화 경로).
 * 호출자: blk_stat_alloc_callback() 직후, 상위 소비자 초기화 코드.
 * 호출 대상: spin_lock_irqsave, list_add_tail_rcu, blk_queue_flag_set
 * (block/blk-stat.c 내부).
 * 에러 경로: 없음(호출자가 유효한 cb를 전달한다고 가정).
 *
 * 호출 체인:
 *   blk_stat_alloc_callback → [이 함수] → (이후) blk_stat_add()가 이 콜백을
 *   순회 대상에 포함
 */
void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb);

/**
 * blk_stat_remove_callback() - Remove a block statistics callback from a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * When this returns, the callback is not running on any CPUs and will not be
 * called again unless readded.
 */
/*
 * [한국어]
 * blk_stat_remove_callback - 콜백을 request_queue의 stats 리스트에서 RCU
 * 제거하고, 필요 시 QUEUE_FLAG_STATS를 클리어하며 타이머를 정지한다
 * (구현: block/blk-stat.c).
 *
 * @q: 콜백이 등록된 request_queue.
 * @cb: 제거할 콜백 객체.
 * @return: 없음 (void).
 *
 * q->stats->lock으로 보호된 상태에서 cb->list를 list_del_rcu()로 제거한 뒤,
 * callbacks 리스트가 비어 있고 accounting 사용자도 없으면 QUEUE_FLAG_STATS를
 * 클리어한다. 이후 timer_delete_sync()로 blk_stat_timer_fn이 다른 CPU에서
 * 실행 중이더라도 완전히 끝날 때까지 동기적으로 대기한 뒤 반환한다 — 함수가
 * 반환된 시점에는 어떤 CPU에서도 이 콜백의 timer_fn이 실행 중이지 않음을
 * 보장한다. 단, 이 함수는 메모리를 해제하지 않으므로(완료 핫패스가 RCU
 * grace period 동안 아직 리스트 순회 중일 수 있음) 실제 kfree는 이어지는
 * blk_stat_free_callback()의 call_rcu()로 유예된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(상위 소비자 해제 경로). timer_delete_sync
 * 때문에 블로킹 가능.
 * 호출자: blk-wbt/blk-iolatency/kyber 등의 exit 경로.
 * 호출 대상: spin_lock_irqsave, list_del_rcu, blk_queue_flag_clear,
 * timer_delete_sync (block/blk-stat.c 내부).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   blk_wbt_exit 등 rq-qos 정책 해제 →
 *   [이 함수] → blk_stat_free_callback
 */
void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb);

/**
 * blk_stat_free_callback() - Free a block statistics callback.
 * @cb: The callback.
 *
 * @cb may be NULL, in which case this does nothing. If it is not NULL, @cb must
 * not be associated with a request queue. I.e., if it was previously added with
 * blk_stat_add_callback(), it must also have been removed since then with
 * blk_stat_remove_callback().
 */
/*
 * [한국어]
 * blk_stat_free_callback - blk_stat_remove_callback() 이후 콜백 메모리를
 * RCU grace period 경과 후 안전하게 해제한다 (구현: block/blk-stat.c).
 *
 * @cb: 해제할 콜백 객체. NULL이면 아무 동작도 하지 않는다. NULL이 아니라면
 *      이미 blk_stat_remove_callback()으로 request_queue에서 분리되어
 *      있어야 한다(등록된 상태에서 바로 호출 금지).
 * @return: 없음 (void).
 *
 * call_rcu(&cb->rcu, ...)로 메모리 해제를 예약한다. 현재 진행 중인 모든 RCU
 * read-side critical section(blk_stat_add의 rcu_read_lock 구간)이 끝난
 * 뒤에야 실제 kfree/free_percpu가 수행되므로, 아직 순회 중일 수 있는 완료
 * 핫패스가 이미 해제된 메모리를 참조하는 UAF(Use-After-Free)를 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. call_rcu() 자체는 블로킹하지 않고 즉시
 * 반환한다(실제 해제는 나중에 RCU 콜백 softirq에서 수행).
 * 호출자: blk_stat_remove_callback() 직후, 상위 소비자 exit 경로.
 * 호출 대상: call_rcu() (block/blk-stat.c 내부).
 * 에러 경로: 없음(cb가 NULL이면 조용히 무시).
 *
 * 호출 체인:
 *   blk_stat_remove_callback → [이 함수] → call_rcu →
 *   [RCU grace period 경과 후] → (내부) blk_stat_free_callback_rcu
 */
void blk_stat_free_callback(struct blk_stat_callback *cb);

/**
 * blk_stat_is_active() - Check if a block statistics callback is currently
 * gathering statistics.
 * @cb: The callback.
 *
 * Returns: %true iff the callback is active.
 */
/*
 * [한국어]
 * blk_stat_is_active - 콜백이 현재 수집 윈도우 안에 있는지(활성 상태인지)
 * 확인한다.
 *
 * @cb: 확인할 blk_stat_callback.
 * @return: 타이머가 아직 만료 전(pending)이면 true, 아니면 false.
 *
 * timer_pending()은 커널 타이머 서브시스템이 제공하는 헬퍼로, cb->timer가
 * 아직 만료되지 않고 대기 중인지를 빠르게 확인한다. blk_stat_add()가 이
 * 함수로 각 콜백의 활성 여부를 먼저 확인한 뒤에만 bucket_fn 호출과 per-cpu
 * 누적을 수행하므로, 비활성(윈도우가 닫힌) 콜백에 대한 불필요한 오버헤드를
 * 피하는 스위치 역할을 한다.
 * 실행 컨텍스트: 어디서든 호출 가능(잠금/슬립 없음). 주로 request 완료
 * IRQ/softirq 경로에서 호출된다.
 * 호출자: block/blk-stat.c의 blk_stat_add().
 * 호출 대상: timer_pending() (커널 타이머 서브시스템 API).
 *
 * 호출 체인:
 *   blk_stat_add → [이 함수] → (true면) cb->bucket_fn 호출로 이어짐
 */
static inline bool blk_stat_is_active(struct blk_stat_callback *cb)
{
	/* [한국어] cb->timer가 아직 만료되지 않고 대기(pending) 중인지 확인:
	 * pending이면 수집 윈도우가 열려 있다는 뜻이므로 true를 반환해
	 * blk_stat_add()가 이 콜백의 bucket_fn/per-cpu 누적을 수행하게 한다. */
	return timer_pending(&cb->timer);
}

/**
 * blk_stat_activate_nsecs() - Gather block statistics during a time window in
 * nanoseconds.
 * @cb: The callback.
 * @nsecs: Number of nanoseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 */
/*
 * [한국어]
 * blk_stat_activate_nsecs - nsecs(나노초) 단위의 수집 윈도우를 연다.
 *
 * @cb: 윈도우를 열 blk_stat_callback.
 * @nsecs: 지금부터 몇 나노초 뒤에 윈도우를 닫을지(타이머를 만료시킬지).
 * @return: 없음 (void).
 *
 * mod_timer()로 cb->timer의 만료 시각을 "지금(jiffies) + nsecs를 jiffies로
 * 환산한 값"으로 재설정한다. 이 호출 이후 blk_stat_is_active(cb)는 만료
 * 전까지 true를 반환하며, blk_stat_add()가 이 콜백에 지연값을 누적하기
 * 시작한다. 타이머가 만료되면 block/blk-stat.c의 blk_stat_timer_fn이
 * 실행되어 전역 병합과 cb->timer_fn 호출을 수행한다.
 * 실행 컨텍스트: 임의 컨텍스트에서 호출 가능(mod_timer는 인터럽트 컨텍스트
 * 에서도 안전). 주로 상위 소비자가 주기적으로 다음 윈도우를 예약할 때 호출.
 * 호출자: 상위 소비자(blk-wbt/kyber 등)가 다음 수집 주기를 시작할 때.
 * 호출 대상: nsecs_to_jiffies(), mod_timer() (커널 타이머 API).
 *
 * 호출 체인:
 *   상위 소비자(timer_fn 내부 등) → [이 함수] → mod_timer →
 *   [nsecs 경과 후] → blk_stat_timer_fn
 */
static inline void blk_stat_activate_nsecs(struct blk_stat_callback *cb,
					   u64 nsecs)
{
	/* [한국어] nsecs(나노초)를 jiffies(커널 저해상도 시간 단위)로 환산해
	 * 현재 jiffies에 더한 값을 타이머 만료 시각으로 설정: mod_timer()는
	 * 이미 등록된 타이머의 만료 시각을 원자적으로 갱신한다. HZ(초당
	 * jiffies 수)와 반올림에 의해 실제 정밀도는 nsecs보다 낮을 수 있다
	 * (정확한 반올림 규칙은 nsecs_to_jiffies() 구현을 따른다). */
	mod_timer(&cb->timer, jiffies + nsecs_to_jiffies(nsecs));
}

/**
 * blk_stat_deactivate() - Disable the statistics timer.
 * @cb: The callback.
 */
/*
 * [한국어]
 * blk_stat_deactivate - 수집 윈도우 타이머를 동기적으로 비활성화한다.
 *
 * @cb: 비활성화할 blk_stat_callback.
 * @return: 없음 (void).
 *
 * timer_delete_sync()는 timer_delete()와 달리, 다른 CPU에서 이미 실행
 * 중인 타이머 핸들러(blk_stat_timer_fn)가 있다면 그 실행이 끝날 때까지
 * 기다린 뒤에 반환한다. 이 함수가 반환되면 이후 blk_stat_is_active(cb)는
 * false를 반환하고, 어떤 CPU에서도 이 콜백의 timer_fn이 실행 중이지 않음이
 * 보장된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 권장(동기 대기이므로 블로킹 가능한
 * 컨텍스트에서 호출해야 함). 인터럽트 컨텍스트에서 호출 시 데드락 위험
 * (자기 자신이 실행 중인 타이머의 완료를 기다리는 데드락이 되므로).
 * 호출자: 상위 소비자가 수집을 명시적으로 중단하거나, 콜백을 제거하기
 * 직전에 호출.
 * 호출 대상: timer_delete_sync() (커널 타이머 API).
 *
 * 호출 체인:
 *   상위 소비자(exit 경로 등) → [이 함수] → timer_delete_sync
 */
static inline void blk_stat_deactivate(struct blk_stat_callback *cb)
{
	/* [한국어] 타이머를 동기적으로 정지: 다른 CPU에서 blk_stat_timer_fn이
	 * 실행 중이면 완료될 때까지 대기 — 반환 후에는 이 콜백의 timer_fn이
	 * 절대 다시 실행되지 않음(재활성화 전까지)을 보장해 RCU/통계 일관성을
	 * 확보한다. */
	timer_delete_sync(&cb->timer);
}

/**
 * blk_stat_activate_msecs() - Gather block statistics during a time window in
 * milliseconds.
 * @cb: The callback.
 * @msecs: Number of milliseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 */
/*
 * [한국어]
 * blk_stat_activate_msecs - msecs(밀리초) 단위의 수집 윈도우를 연다.
 *
 * @cb: 윈도우를 열 blk_stat_callback.
 * @msecs: 지금부터 몇 밀리초 뒤에 윈도우를 닫을지(타이머를 만료시킬지).
 * @return: 없음 (void).
 *
 * blk_stat_activate_nsecs()와 동일한 mod_timer() 메커니즘을 msecs 단위로
 * 제공하는 헬퍼이다. nsecs 버전보다 상대적으로 긴 주기(수십~수백 ms)로
 * 반복 수집할 때 사용하기 적합하며, 상위 소비자가 짧은 스파이크보다는
 * 평균적인 latency 추세를 보고 정책(예: 토큰 버킷 발행 속도, 큐 깊이)을
 * 조정하고 싶을 때 선택한다.
 * 실행 컨텍스트: 임의 컨텍스트에서 호출 가능(mod_timer는 인터럽트 컨텍스트
 * 에서도 안전).
 * 호출자: 상위 소비자가 다음 수집 주기를 시작할 때(흔히 timer_fn 콜백
 * 내부에서 자기 자신의 다음 윈도우를 재예약하는 형태로 호출).
 * 호출 대상: msecs_to_jiffies(), mod_timer() (커널 타이머 API).
 *
 * 호출 체인:
 *   상위 소비자(timer_fn 내부 등) → [이 함수] → mod_timer →
 *   [msecs 경과 후] → blk_stat_timer_fn
 */
static inline void blk_stat_activate_msecs(struct blk_stat_callback *cb,
					   unsigned int msecs)
{
	/* [한국어] msecs(밀리초)를 jiffies로 환산해 현재 jiffies에 더한 값을
	 * 타이머 만료 시각으로 설정: blk_stat_activate_nsecs()와 동일한
	 * mod_timer() 패턴이나 단위 변환 함수만 msecs_to_jiffies()로 다르다. */
	mod_timer(&cb->timer, jiffies + msecs_to_jiffies(msecs));
}

/*
 * [한국어]
 * blk_rq_stat_add - 단일 완료 지연값을 blk_rq_stat bucket에 추가한다
 * (구현: block/blk-stat.c).
 *
 * @stat (첫 번째 인자, 이름 없음): 값을 누적할 blk_rq_stat bucket 포인터.
 *       보통 blk_stat_add()가 per_cpu_ptr(cb->cpu_stat, cpu)[bucket]으로
 *       얻은 현재 CPU 전용 bucket을 전달한다.
 * @value (두 번째 인자, u64): 이번 request의 완료 지연(나노초).
 *        blk_stat_add()가 now - rq->io_start_time_ns로 계산해 전달한다.
 * @return: 없음 (void).
 *
 * bucket의 min/max를 갱신하고, batch(샘플 값들의 합)에 value를 더하며
 * nr_samples를 1 증가시킨다(구체 계산은 block/blk-stat.c 참고). 이 함수
 * 자체는 동기화를 하지 않으므로, 호출자가 get_cpu() 등으로 CPU 선점을
 * 비활성화한 상태에서 자신만의 per-cpu 슬롯에 대해서만 호출해야 한다.
 * 실행 컨텍스트: request 완료 IRQ/softirq, CPU 선점 비활성화 상태에서 호출.
 * 호출자: block/blk-stat.c의 blk_stat_add().
 * 호출 대상: 없음(min/max 비교와 산술 연산만 수행).
 *
 * 호출 체인:
 *   blk_stat_add → get_cpu → [이 함수] → put_cpu
 */
void blk_rq_stat_add(struct blk_rq_stat *, u64);

/*
 * [한국어]
 * blk_rq_stat_sum - per-cpu 통계 src를 전역 통계 dst에 가중 평균으로
 * 병합한다 (구현: block/blk-stat.c).
 *
 * @dst (첫 번째 인자): 병합 대상 전역 bucket. blk_stat_callback->stat[bucket]
 *      을 가리킨다.
 * @src (두 번째 인자): 병합 원본 per-cpu bucket.
 *      per_cpu_ptr(cb->cpu_stat, cpu)[bucket]을 가리킨다.
 * @return: 없음 (void).
 *
 * 여러 CPU에 흩어져 누적된 per-cpu 통계를 하나의 전역 histogram으로
 * 합산한다. min/max는 단순 비교로, mean은 batch(샘플 합)와 nr_samples를
 * 이용한 가중 평균으로 재계산한다(정확한 수식은 block/blk-stat.c 참고).
 * nr_samples 오버플로우가 감지되면 병합을 건너뛴다(정확도보다 안전 우선).
 * 실행 컨텍스트: 타이머 softirq(blk_stat_timer_fn)에서만 호출되는 것으로
 * 보이며 재진입은 없다.
 * 호출자: block/blk-stat.c의 blk_stat_timer_fn()이 for_each_online_cpu
 * 루프 안에서 각 CPU의 bucket마다 호출.
 * 호출 대상: 없음(산술 연산과 나눗셈만 수행).
 * 에러 경로: nr_samples 합산이 오버플로우하면(unsigned wraparound) 조기
 * 반환하여 병합을 생략한다.
 *
 * 호출 체인:
 *   blk_stat_timer_fn → for_each_online_cpu → [이 함수]
 */
void blk_rq_stat_sum(struct blk_rq_stat *, struct blk_rq_stat *);

/*
 * [한국어]
 * blk_rq_stat_init - 단일 blk_rq_stat bucket을 초기값으로 리셋한다
 * (구현: block/blk-stat.c).
 *
 * @stat (인자, 이름 없음): 초기화할 bucket 포인터. per-cpu 버퍼의 한 원소
 *       또는 전역 stat 배열의 한 원소일 수 있다.
 * @return: 없음 (void).
 *
 * min을 최댓값(예: -1ULL)으로, max/mean/nr_samples/batch를 0으로 리셋하여
 * 새 수집 윈도우를 준비한다. 초기화 없이 재사용하면 이전 윈도우의 값이
 * 섞여 min/mean이 왜곡되므로, 콜백 등록 시(blk_stat_add_callback)와 매
 * 타이머 만료 시(blk_stat_timer_fn) 반드시 호출된다.
 * 실행 컨텍스트: 타이머 softirq(blk_stat_timer_fn)와 프로세스 컨텍스트
 * (blk_stat_add_callback의 등록 경로) 양쪽에서 호출됨.
 * 호출자: block/blk-stat.c의 blk_stat_add_callback(), blk_stat_timer_fn().
 * 호출 대상: 없음(필드 대입만 수행).
 *
 * 호출 체인:
 *   blk_stat_add_callback → for_each_possible_cpu → [이 함수]
 *   blk_stat_timer_fn → [이 함수] (전역 리셋과 per-cpu 리셋 양쪽에 사용)
 */
void blk_rq_stat_init(struct blk_rq_stat *);

#endif

/*
 * [한국어] 파일 전체 핵심 요약 (block/blk-stat.h)
 *
 * - 이 헤더는 request_queue 단위로 등록되는 blk_stat_callback을 정의하고,
 *   완료 지연을 per-cpu bucket에 수집했다가 주기적으로 전역 histogram으로
 *   병합해 timer_fn을 통해 상위 소비자(blk-wbt/blk-iolatency/kyber 등)에게
 *   전달하는 공개 API를 선언한다. 실제 병합/누적 로직은 block/blk-stat.c에.
 * - 핵심 호출 경로:
 *   blk_mq_start_request(io_start_time_ns 기록) → [디바이스 처리] →
 *   blk_mq_end_request → blk_stat_add(now) → bucket_fn → cpu_stat 누적 →
 *   [타이머 만료] → blk_stat_timer_fn(내부) → blk_rq_stat_sum() → timer_fn.
 * - 상위 소비자는 blk_stat_alloc_callback() / blk_stat_add_callback()으로
 *   콜백을 등록하고, blk_stat_activate_nsecs()/msecs()로 수집 윈도우를 열며,
 *   수집된 latency 분포를 바탕으로 쓰로틀링/스케줄링 정책을 조정한다.
 * - blk_stat_enable_accounting()은 콜백 없이도 request에
 *   time/size 정보를 기록할 수 있게 해, 모든 I/O에 대한
 *   기본 accounting을 보장한다(콜백을 등록하지 않고 io_start_time_ns만
 *   기록하고 싶은 blk-iocost/bfq 등을 위한 경로).
 * - 이 파일은 block/blk-mq.c의 request 완료 흐름(blk_mq_end_request) 뒤에
 *   위치하며, block/blk-wbt.c, block/blk-iolatency.c,
 *   block/kyber-iosched.c의 정책 결정 로직과 직접 연결되어 블록 계층
 *   latency 데이터의 수집·병합·전달을 담당하는 공용 인프라 역할을 한다.
 */
