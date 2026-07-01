/* SPDX-License-Identifier: GPL-2.0
 *
 * Legacy blkg rwstat helpers enabled by CONFIG_BLK_CGROUP_RWSTAT.
 * Do not use in new code.
 *
 * ============================================================
 * NVMe SSD 관점 파일 요약
 * ------------------------------------------------------------
 * 이 헤더는 blk-cgroup(block cgroup) 계층에서 I/O 통계(rwstat)를
 * 수집/집계하기 위한 레거시 헬퍼들을 정의한다. NVMe 스택에서는
 * 상위 cgroup에 속한 여러 namespace/controller로 발행된 Read/Write/
 * Discard/Sync/Async 요청의 양(바이트/섹터)을 추적하는 용도로 쓰인다.
 *
 * 주요 경로(추정):
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 이후 완료 인터럽트(CQ) 처리 과정에서
 * 통계가 갱신되며, 사용자는 /sys/fs/cgroup/.../io.* 등을 통해
 * blkg_rwstat_read() / blkg_rwstat_recursive_sum() 결과를 확인한다.
 *
 * 연관 파일: block/blk-cgroup.h (blkcg 구조체 및 정책 정의)
 * ============================================================
 */
#ifndef _BLK_CGROUP_RWSTAT_H
#define _BLK_CGROUP_RWSTAT_H

/* 이 파일은 block/blk-cgroup.h에 정의된 blkcg 구조체/정책을 기반으로
 * I/O 통계 헬퍼를 제공한다. */
#include "blk-cgroup.h"

/* blkg_rwstat_type: NVMe 명령 유형별 카운터 인덱스.
 * NVMe Read -> BLKG_RWSTAT_READ,
 * NVMe Write -> BLKG_RWSTAT_WRITE,
 * NVMe Dataset Management/Deallocate -> BLKG_RWSTAT_DISCARD,
 * Sync/Async는 submission 경로에서의 플래그 특성을 반영한다.
 */
enum blkg_rwstat_type {
	BLKG_RWSTAT_READ,		/* NVMe Read(0x02) / Compare 등 Read 계열 opcode에 매핑 (추정) */
	BLKG_RWSTAT_WRITE,		/* NVMe Write(0x01) 및 Write Zeroes(0x08)에 매핑 (추정) */
	BLKG_RWSTAT_SYNC,		/* REQ_SYNC 플래그: NVMe Flush(0x00) 또는 동기적 submission을 반영 (추정) */
	BLKG_RWSTAT_ASYNC,		/* NVMe Read/Write 중 REQ_SYNC 미설정 시 async 경로로 집계 (추정) */
	BLKG_RWSTAT_DISCARD,		/* NVMe Dataset Management(Deallocate, 0x09)에 매핑 (추정) */

	BLKG_RWSTAT_NR,			/* NVMe 명령 클래스별 통계 배열 크기 */
	BLKG_RWSTAT_TOTAL = BLKG_RWSTAT_NR,	/* cgroup io.stat 노출 시 전체 카운터 수 (추정) */
};

/* blkg_rwstat: blkcg 그룹의 I/O 통계를 per-CPU와 보조 카운터로 관리.
 *
 * cpu_cnt[]: 현재 살아 있는 blkg에 대한 per-CPU 카운터 배열.
 *   NVMe 관점에서는 nvme_queue에 연결된 request가 SQ에 기록될 때
 *   (doorbell ringing 이전 또는 이후) read/write/discard/sync/async
 *   별로 요청량이 누적된다. per-CPU 구조는 host CPU 간 doorbell
 *   contention을 줄이기 위한 캐시 최적화로 볼 수 있다.
 *
 * aux_cnt[]: 죽은 자식 cgroup의 통계를 합산해 보관하는 원자 카운터.
 *   NVMe namespace/controller가 hot-removal 되거나 cgroup이 삭제된
 *   뒤에도 상위 cgroup의 누적 통계에 포함시키기 위해 사용된다.
 */
/*
 * blkg_[rw]stat->aux_cnt is excluded for local stats but included for
 * recursive.  Used to carry stats of dead children.
 */
struct blkg_rwstat {
	struct percpu_counter		cpu_cnt[BLKG_RWSTAT_NR];	/* SQ submission CPU별 누적: doorbell cacheline 분리 효과 (추정) */
	atomic64_t			aux_cnt[BLKG_RWSTAT_NR];	/* 제거된 nvme_queue/namespace 통계를 상위 cgroup으로 보존 (추정) */
};

/* blkg_rwstat_sample: blkg_rwstat의 읽기 전용 스냅숏.
 * nvme_queue 통계를 사용자 공간 seq_file로 낼 때 per-CPU 카운터를
 * 일관성 있게 복사해 두는 용도다.
 */
struct blkg_rwstat_sample {
	u64				cnt[BLKG_RWSTAT_NR];		/* Read/Write/Discard/Sync/Async 스냅숏: /sys/fs/cgroup/.../io.* 노출용 (추정) */
};

/* blkg_rwstat_read_counter(): cpu_cnt[idx]와 aux_cnt[idx]를 합산해
 * 현재 시점의 단일 항목 카운터 값을 반환한다.
 *
 * 호출 경로(추정):
 * blkcg_stat_show -> blkg_prfill_rwstat -> __blkg_prfill_rwstat
 * -> blkg_rwstat_read_counter
 *
 * NVMe 연결: /sys/fs/cgroup/.../io.* 파일에서 namespace 단위로
 * 보고되는 Read/Write/Discard 바이트/섹터 값이 이 합산 결과를
 * 사용한다.
 */
static inline u64 blkg_rwstat_read_counter(struct blkg_rwstat *rwstat,
		unsigned int idx)
{
	return atomic64_read(&rwstat->aux_cnt[idx]) +		/* 제거된 자식 cgroup(nvme_queue/namespace)의 누적값을 먼저 읽음 (추정) */
		percpu_counter_sum_positive(&rwstat->cpu_cnt[idx]);	/* 살아있는 per-CPU SQ/CQ 통계를 cross-CPU 합산 (추정) */
}

/* blkg_rwstat_init(): blkg_rwstat의 per-CPU 카운터를 할당/초기화한다.
 * 정책 등록 시 blkcg_policy->pd_init_fn 등에서 호출된다(추정).
 *
 * NVMe 연결: nvme_queue 또는 namespace 단위 blkcg policy 데이터를
 * 생성할 때 호출되어 이후 SQ/CQ 기반 I/O 통계 누적을 위한 자원을
 * 마련한다.
 */
int blkg_rwstat_init(struct blkg_rwstat *rwstat, gfp_t gfp);	/* -> blkcg_policy_data 할당 -> percpu_counter_alloc: NVMe 다중 hctx 통계용 (추정) */

/* blkg_rwstat_exit(): blkg_rwstat_init()에서 할당한 per-CPU 카운터를
 * 해제한다. blkcg policy 데이터 해제 시점에 호출된다(추정).
 */
void blkg_rwstat_exit(struct blkg_rwstat *rwstat);		/* -> blkcg_gq 소멸 시 nvme_queue 통계 자원 회수 (추정) */

/* __blkg_prfill_rwstat(): 이미 채워진 rwstat 샘플을 seq_file에
 * 출력 형식으로 기록한다.
 *
 * 호출 경로(추정):
 * blkcg_stat_show -> policy->stat_show -> __blkg_prfill_rwstat
 *
 * NVMe 연결: 사용자가 /sys/fs/cgroup/.../io.stat 등을 읽을 때
 * namespace/controller별 Read/Write/Discard 합계가 변환된다.
 */
u64 __blkg_prfill_rwstat(struct seq_file *sf, struct blkcg_policy_data *pd,
			 const struct blkg_rwstat_sample *rwstat);	/* io.stat: "rbytes=... wbytes=... dbytes=..." 포맷 변환 (추정) */

/* blkg_prfill_rwstat(): blkg policy 데이터의 지정된 오프셋에서
 * rwstat을 읽어 __blkg_prfill_rwstat()로 포맷팅한다.
 *
 * 호출 경로(추정):
 * blkcg_stat_show -> blkg_prfill_rwstat -> __blkg_prfill_rwstat
 */
u64 blkg_prfill_rwstat(struct seq_file *sf, struct blkcg_policy_data *pd,
		       int off);		/* blkg + policy offset -> blkg_rwstat_read_counter -> NVMe namespace I/O 집계 (추정) */

/* blkg_rwstat_recursive_sum(): blkcg 하위 트리를 재귀적으로 순회하며
 * per-CPU 카운터와 aux 카운터를 모두 합산한다.
 *
 * 호출 경로(추정):
 * blkcg_stat_show -> blkg_rwstat_recursive_sum
 *
 * NVMe 연결: 상위 cgroup 아래 여러 nvme_controller / namespace의
 * I/O를 통합해 전체 그룹 단위 Read/Write/Discard 사용량을 보고할
 * 때 사용한다.
 */
void blkg_rwstat_recursive_sum(struct blkcg_gq *blkg, struct blkcg_policy *pol,
		int off, struct blkg_rwstat_sample *sum);	/* 다중 controller/namespace 하위 트리의 SQ/CQ 통계를 누적 (추정) */


/* blkg_rwstat_add(): @opf에 따라 @val를 NVMe 명령 유형별 카운터에
 * 추가한다. Sync/Async도 별도로 누적한다.
 *
 * 호출 경로(추정):
 * blk_mq_submit_bio -> blk_mq_get_request ->
 * (blkcg_bio_issue_check / blk_account_io_start 등) ->
 * blkg_rwstat_add
 *
 * NVMe 연결:
 * - op_is_discard(): NVMe Dataset Management(Deallocate) 명령
 * - op_is_write():  NVMe Write 명령
 * - 그 외:          NVMe Read 명령
 * - op_is_sync():   SYNC 플래그가 설정된 submission (poll/flush 관련
 *                   동작 특성 반영, 추정)
 */
/**
 * blkg_rwstat_add - add a value to a blkg_rwstat
 * @rwstat: target blkg_rwstat
 * @opf: REQ_OP and flags
 * @val: value to add
 *
 * Add @val to @rwstat.  The counters are chosen according to @rw.  The
 * caller is responsible for synchronizing calls to this function.
 */
static inline void blkg_rwstat_add(struct blkg_rwstat *rwstat,
				   blk_opf_t opf, uint64_t val)
{
	struct percpu_counter *cnt;							/* 선택된 NVMe 명령 유형별 per-CPU 카운터 포인터 (추정) */

	/* NVMe Deallocate(Discard) 명령이면 DISCARD 카운터를 선택 */
	if (op_is_discard(opf))								/* REQ_OP_DISCARD: -> nvme_setup_dsm() -> Dataset Management(0x09) (추정) */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_DISCARD];		/* discard/deallocate 통계 누적 대상 설정 (추정) */
	/* NVMe Write 명령이면 WRITE 카운터를 선택 */
	else if (op_is_write(opf))							/* REQ_OP_WRITE / REQ_OP_WRITE_ZEROES: -> nvme_setup_rw() (추정) */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_WRITE];			/* NVMe Write(0x01) 또는 Write Zeroes(0x08) 통계 누적 대상 (추정) */
	/* 그 외는 NVMe Read로 간주하여 READ 카운터를 선택 (추정) */
	else
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_READ];			/* NVMe Read(0x02), Compare(0x05) 등 Read 계열 통계 누적 대상 (추정) */

	/* 해당 방향 카운터에 요청량(섹터/바이트)을 누적 */
	percpu_counter_add_batch(cnt, val, BLKG_STAT_CPU_BATCH);	/* per-CPU 배치 누적: doorbell 전후 cacheline bouncing 감소 (추정) */

	/* SYNC/ASYNC 플래그에 따라 추가로 Sync 또는 Async 카운터 누적 */
	if (op_is_sync(opf))									/* REQ_SYNC: NVMe Flush(0x00) 또는 sync submission 경로 반영 (추정) */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_SYNC];			/* sync I/O 카운터 선택 (추정) */
	else
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_ASYNC];			/* async I/O: poll / interrupt CQ 완료와 연계된 통계 (추정) */

	percpu_counter_add_batch(cnt, val, BLKG_STAT_CPU_BATCH);	/* Sync/Async per-CPU 배치 누적: CQ 인터럽트 핸들러와 동일 cacheline 최소화 (추정) */
}

/* blkg_rwstat_read(): blkg_rwstat의 현재값을 @result에 스냅숏으로
 * 복사한다. percpu_counter_sum_positive()를 호출하므로 호출자는
 * 일부 동기화를 고려해야 한다.
 *
 * 호출 경로(추정):
 * blkg_rwstat_total() 또는 cgroup stat show 루틴 -> blkg_rwstat_read
 *
 * NVMe 연결: namespace별로 수집된 Read/Write/Discard/Sync/Async
 * 카운터를 사용자 공간으로 낼 때 가장 먼저 사용된다.
 */
/**
 * blkg_rwstat_read - read the current values of a blkg_rwstat
 * @rwstat: blkg_rwstat to read
 * @result: where to put the current values
 *
 * Read the current snapshot of @rwstat and return it in the @result counts.
 */
static inline void blkg_rwstat_read(struct blkg_rwstat *rwstat,
		struct blkg_rwstat_sample *result)
{
	int i;												/* NVMe 명령 클래스 인덱스 반복자 (추정) */

	/* BLKG_RWSTAT_READ/WRITE/SYNC/ASYNC/DISCARD 전체 복사 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* READ/WRITE/SYNC/ASYNC/DISCARD 순회: namespace별 5개 통계 클래스 (추정) */
		result->cnt[i] =								/* 스냅숏 배열에 복사: io.stat 노출 전 일관성 확보 (추정) */
			percpu_counter_sum_positive(&rwstat->cpu_cnt[i]);	/* 모든 SQ submission CPU + CQ 완료 CPU 카운터 합산 (추정) */
}

/* blkg_rwstat_total(): READ + WRITE 카운터 합계를 반환한다.
 * Discard는 "총합"에 포함되지 않으며, Read/Write 중심의 정책에서
 * 사용된다(추정).
 *
 * 호출 경로(추정):
 * blk-throttle / cfq 등의 limit 계산 루틴 -> blkg_rwstat_total
 *
 * NVMe 연결: NVMe namespace에 대한 Read/Write 처리량 기반 스로틀링
 * 판단 시 참조값으로 쓰인다.
 */
/**
 * blkg_rwstat_total - read the total count of a blkg_rwstat
 * @rwstat: blkg_rwstat to read
 *
 * Return the total count of @rwstat regardless of the IO direction.  This
 * function can be called without synchronization and takes care of u64
 * atomicity.
 */
static inline uint64_t blkg_rwstat_total(struct blkg_rwstat *rwstat)
{
	struct blkg_rwstat_sample tmp = { };					/* namespace 단위 스냅숏 버퍼: stack 기반 (추정) */

	/* 스냅숏을 먼저 떠서 Read/Write 합계만 반환 (Discard 제외) */
	blkg_rwstat_read(rwstat, &tmp);							/* READ/WRITE/SYNC/ASYNC/DISCARD 5종 스냅숏 생성 (추정) */
	return tmp.cnt[BLKG_RWSTAT_READ] + tmp.cnt[BLKG_RWSTAT_WRITE];	/* Read+Write 합계: throttle/cfq limit 산출 시 NVMe I/O bandwidth 추정용 (추정) */
}

/* blkg_rwstat_reset(): blkg_rwstat의 모든 per-CPU 카운터와 aux
 * 카운터를 0으로 초기화한다. cgroup 이동/재설정 경로에서 호출된다
 * (추정).
 *
 * NVMe 연결: namespace/controller를 다른 cgroup으로 옮기거나
 * 통계를 리셋할 때 기존 누적값을 제거한다.
 */
/**
 * blkg_rwstat_reset - reset a blkg_rwstat
 * @rwstat: blkg_rwstat to reset
 */
static inline void blkg_rwstat_reset(struct blkg_rwstat *rwstat)
{
	int i;												/* NVMe 통계 클래스 반복자 (추정) */

	for (i = 0; i < BLKG_RWSTAT_NR; i++) {					/* READ/WRITE/SYNC/ASYNC/DISCARD 5개 클래스 전체 초기화 (추정) */
		percpu_counter_set(&rwstat->cpu_cnt[i], 0);			/* per-CPU SQ/CQ 누적값 0으로 설정: cgroup 이동 시 이전 namespace 값 제거 (추정) */
		atomic64_set(&rwstat->aux_cnt[i], 0);				/* aux 제거 통계 0으로 설정: 상위 cgroup 누적값 재조정 (추정) */
	}
}

/* blkg_rwstat_add_aux(): @from의 per-CPU 카운터와 aux 카운터를
 * 모두 합산해 @to의 aux 카운터에 원자적으로 더한다.
 *
 * 호출 경로(추정):
 * blkcg_gq가 삭제되거나 이동될 때 blkg_rwstat_add_aux 호출
 *
 * NVMe 연결: 제거된 nvme_queue 또는 namespace의 기록을 상위/이동
 * 대상 cgroup의 aux_cnt로 보존하여 누적 통계가 사라지지 않게 한다.
 */
/**
 * blkg_rwstat_add_aux - add a blkg_rwstat into another's aux count
 * @to: the destination blkg_rwstat
 * @from: the source
 *
 * Add @from's count including the aux one to @to's aux count.
 */
static inline void blkg_rwstat_add_aux(struct blkg_rwstat *to,
				       struct blkg_rwstat *from)
{
	u64 sum[BLKG_RWSTAT_NR];								/* from per-CPU 합산 임시 버퍼: 5개 NVMe 클래스 (추정) */
	int i;												/* 통계 클래스 반복자 (추정) */

	/* 먼저 from의 per-CPU 카운터를 각 항목별로 합산 (추정) */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* from 하위 cgroup의 READ/WRITE/SYNC/ASYNC/DISCARD 순회 (추정) */
		sum[i] = percpu_counter_sum_positive(&from->cpu_cnt[i]);	/* 삭제되는 nvme_queue/namespace의 per-CPU SQ/CQ 통계 합산 (추정) */

	/* per-CPU 합계와 from의 aux 값을 to의 aux에 원자 추가 (추정) */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* 대상 cgroup의 aux_cnt에 병합: 하위 트리 nvme 통계 보존 (추정) */
		atomic64_add(sum[i] + atomic64_read(&from->aux_cnt[i]),
			     &to->aux_cnt[i]);						/* atomic64_add: CQ 완료 순서와 독립적으로 상위 cgroup 누적값 갱신 (추정) */
}

/* NVMe 관점 핵심 요약
 *
 * - blkg_rwstat은 NVMe Read/Write/Discard/Sync/Async 요청량을
 *   cgroup 단위로 추적하며, per-CPU 카운터는 doorbell 경합을
 *   줄이는 캐시 구조다.
 * - aux_cnt는 제거된 nvme_queue/namespace의 통계를 상위 cgroup에
 *   보존해 누적값이 유실되지 않게 한다.
 * - blkg_rwstat_add는 bio/req opf 플래그를 NVMe 명령 유형으로
 *   매핑: discard -> Deallocate, write -> Write, 나머지 -> Read.
 * - blkg_rwstat_recursive_sum은 다중 namespace/controller를 거느린
 *   cgroup의 전체 I/O를 집계해 /sys/fs/cgroup/.../io.* 노출에
 *   사용된다(추정).
 * - 이 파일은 block/blk-cgroup.h의 blkcg_policy/blkcg_gq 위에서
 *   동작하는 통계 계층으로, NVMe 드라이버와 직접 연결되지는
 *   않으나 SQ/CQ 기반 I/O 제어 흐름의 계량 지표를 제공한다.
 */
#endif	/* _BLK_CGROUP_RWSTAT_H */
