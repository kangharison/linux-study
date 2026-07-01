// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cgroups support for the BFQ I/O scheduler.
 *
 * NVMe SSD 관점에서 본 block/bfq-cgroup.c
 *
 * 이 파일은 BFQ I/O 스케줄러의 cgroup 지원을 담당한다. blk-cgroup 프레임워크와
 * 연계하여 프로세스 그룹 단위의 I/O 가중치, 통계, 계층 구조를 관리한다.
 * NVMe SSD 입장에서 본다면, 이 파일은 상위 blk-mq 레이어에서 제출된 bio/request를
 * 어떤 cgroup의 bfq_queue로 라우팅할지 결정하며, 최종적으로는
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * 경로로 전달되는 I/O의 우선순위와 자원 분배 정책을 제어한다.
 *
 * 논리적으로 block/bfq-iosched.c, block/bfq-wf2q.c와 연결되며,
 * block/blk-cgroup.c의 blkcg_policy 콜백을 통해 cgroup 생성/삭제/오프라인
 * 이벤트를 수신한다.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/cgroup.h>
#include <linux/ktime.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include <linux/sbitmap.h>
#include <linux/delay.h>

#include "elevator.h"
#include "bfq-iosched.h"

#ifdef CONFIG_BFQ_CGROUP_DEBUG
/*
 * bfq_stat_init - per-cpu 카운터 기반 bfq 통계 자료구조 초기화
 * @stat: 초기화할 bfq_stat (대개 struct bfqg_stats 내부 필드)
 * @gfp: 메모리 할당 플래그
 *
 * NVMe 관점: I/O 완료 시점의 service_time, wait_time 등을 cpu 단위로
 * 누적하여 기록한다. 이 값들은 추후 cgroup 디버그 fs를 통해 노출되며,
 * NVMe SQ/CQ에서 명령이 얼마나 대기/서비스되었는지 분석하는 데 활용된다.
 */
static int bfq_stat_init(struct bfq_stat *stat, gfp_t gfp)
{
	int ret;

	ret = percpu_counter_init(&stat->cpu_cnt, 0, gfp); /* per-cpu 카운터는 NVMe 완료 인터럽트가 각 CPU로 도달할 때 캐시 라인 경쟁 완화 */
	if (ret)
		return ret;

	atomic64_set(&stat->aux_cnt, 0); /* aux_cnt는 NVMe 완료 콜백과 cgroup 해제 경쟁 시 원자적 누적 */
	return 0;
}

static void bfq_stat_exit(struct bfq_stat *stat)
{
	percpu_counter_destroy(&stat->cpu_cnt);
}

/**
 * bfq_stat_add - add a value to a bfq_stat
 * @stat: target bfq_stat
 * @val: value to add
 *
 * Add @val to @stat.  The caller must ensure that IRQ on the same CPU
 * don't re-enter this function for the same counter.
 *
 * NVMe 관점: 동일 CPU에서 NVMe 완료 인터럽트(hardirq/softirq)가 같은
 * 카운터를 재진입하지 않도록 호출자가 보장해야 한다. 그렇지 않으면
 * percpu_counter 값이 왜곡되어 CID 단위 완료 지연 측정이 부정확해진다.
 */
static inline void bfq_stat_add(struct bfq_stat *stat, uint64_t val)
{
	percpu_counter_add_batch(&stat->cpu_cnt, val, BLKG_STAT_CPU_BATCH); /* batch 임계값(BLKG_STAT_CPU_BATCH)은 NVMe ISR/softirq 빈번 갱신 시 캐시 반송비 완화 */
}

/**
 * bfq_stat_read - read the current value of a bfq_stat
 * @stat: bfq_stat to read
 */
static inline uint64_t bfq_stat_read(struct bfq_stat *stat)
{
	return percpu_counter_sum_positive(&stat->cpu_cnt); /* 완료 인터럽트 진행 중인 CPU의 누적값도 포함, NVMe SQ/CQ 지연 분석 */
}

/**
 * bfq_stat_reset - reset a bfq_stat
 * @stat: bfq_stat to reset
 */
static inline void bfq_stat_reset(struct bfq_stat *stat)
{
	percpu_counter_set(&stat->cpu_cnt, 0); /* cgroup 오프라인 시점에 NVMe SQ/CQ 남은 in-flight 기록은 부모로 이관 후 초기화 */
	atomic64_set(&stat->aux_cnt, 0); /* aux_cnt 초기화는 이전 cgroup의 NVMe 완료 이력이 새 통계에 섞이지 않도록 방지 */
}

/**
 * bfq_stat_add_aux - add a bfq_stat into another's aux count
 * @to: the destination bfq_stat
 * @from: the source
 *
 * Add @from's count including the aux one to @to's aux count.
 *
 * NVMe 관점: cgroup이 삭제될 때 자식 그룹의 누적 통계를 부모의 aux 카운터로
 * 이관하여, NVMe 명령 완료 기록이 상위 cgroup 통계에서도 유지되도록 한다.
 */
static inline void bfq_stat_add_aux(struct bfq_stat *to,
				     struct bfq_stat *from)
{
	atomic64_add(bfq_stat_read(from) + atomic64_read(&from->aux_cnt),
		     &to->aux_cnt);
}

/**
 * blkg_prfill_stat - prfill callback for bfq_stat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @off: offset to the bfq_stat in @pd
 *
 * prfill callback for printing a bfq_stat.
 */
static u64 blkg_prfill_stat(struct seq_file *sf, struct blkg_policy_data *pd,
		int off)
{
	return __blkg_prfill_u64(sf, pd, bfq_stat_read((void *)pd + off));
}

/* bfqg stats flags */
enum bfqg_stats_flags {
	BFQG_stats_waiting = 0,
	BFQG_stats_idling,
	BFQG_stats_empty,
};

#define BFQG_FLAG_FNS(name)						\
static void bfqg_stats_mark_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags |= (1 << BFQG_stats_##name);			\
}									\
static void bfqg_stats_clear_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags &= ~(1 << BFQG_stats_##name);			\
}									\
static int bfqg_stats_##name(struct bfqg_stats *stats)		\
{									\
	return (stats->flags & (1 << BFQG_stats_##name)) != 0;		\
}									\

BFQG_FLAG_FNS(waiting)
BFQG_FLAG_FNS(idling)
BFQG_FLAG_FNS(empty)
#undef BFQG_FLAG_FNS

/*
 * bfqg_stats_update_group_wait_time - 그룹 전체가 서비스 받기까지 대기한 시간 기록
 * @stats: 갱신할 struct bfqg_stats
 *
 * 스케줄러 락을 잡은 상태에서 호출한다.
 *
 * NVMe 관점: NVMe SQ에 명령을 채우기 전, 다른 cgroup의 bfq_queue가
 * in_service_queue로 선정되기를 기다리는 시간을 측정한다. 이 시간이 길면
 * 해당 cgroup의 I/O가 doorbell 발행까지 지연됨을 의미한다.
 */
static void bfqg_stats_update_group_wait_time(struct bfqg_stats *stats)
{
	u64 now;

	if (!bfqg_stats_waiting(stats))
		return; /* waiting 플래그가 꺼져 있으면 NVMe 제출 직전 대기 시간 측정 불필요 */

	now = blk_time_get_ns(); /* blk_time_get_ns는 doorbell 발행 시점과 동일한 monotonic 시간축 사용 */
	if (now > stats->start_group_wait_time)
		bfq_stat_add(&stats->group_wait_time, /* group_wait_time 누적은 SQ 가득 참/CID 부족으로 doorbell 지연된 시간 분석 (추정) */
			      now - stats->start_group_wait_time);
	bfqg_stats_clear_waiting(stats);
}

/* This should be called with the scheduler lock held. */
static void bfqg_stats_set_start_group_wait_time(struct bfq_group *bfqg,
						 struct bfq_group *curr_bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (bfqg_stats_waiting(stats)) /* 이미 waiting 상태면 doorbell 지연 측정 중복 방지 */
		return;
	if (bfqg == curr_bfqg) /* 현재 서비스 중인 그룹이면 NVMe SQ에 이미 명령 흐륯으므로 대기 시간 측정 불필요 */
		return;
	stats->start_group_wait_time = blk_time_get_ns(); /* 대기 시작 시점: 이후 doorbell 발행까지의 간격 측정 기준 */
	bfqg_stats_mark_waiting(stats);
}

/* This should be called with the scheduler lock held. */
static void bfqg_stats_end_empty_time(struct bfqg_stats *stats)
{
	u64 now;

	if (!bfqg_stats_empty(stats)) /* empty 플래그가 꺼져 있으면 NVMe 제출 후보 I/O가 존재 */
		return;

	now = blk_time_get_ns();
	if (now > stats->start_empty_time) /* empty_time 측정 시점은 bfq_group 내 NVMe 명령 후보가 없어진 시점 */
		bfq_stat_add(&stats->empty_time,
			      now - stats->start_empty_time); /* empty_time 누적은 디바이스 유휴로 NVMe doorbell 발행 중단된 시간 */
	bfqg_stats_clear_empty(stats);
}

/*
 * bfqg_stats_update_dequeue - bfq_queue가 스케줄러에서 디큐된 횟수 기록
 * @bfqg: 대상 bfq_group
 *
 * NVMe 관점: NVMe SQ/CQ 용량이나 디바이스 처리량 제한으로 인해 bfq_queue가
 * 스케줄링 트리에서 제거(dequeue)되는 횟수를 추적한다. 이는 SQ 가득 참
 * 상태나 CID 고갈과 관련된 병목 분석에 참고된다 (추정).
 */
void bfqg_stats_update_dequeue(struct bfq_group *bfqg)
{
	bfq_stat_add(&bfqg->stats.dequeue, 1); /* dequeue 카운트 증가: NVMe SQ 가득 차서 bfq_queue가 dispatch 트리에서 제거된 횟수 (추정) */
}

void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (blkg_rwstat_total(&stats->queued)) /* queued 통계가 0이면 현재 NVMe 제출 대기 중인 I/O 없음 */
		return;

	/*
	 * group is already marked empty. This can happen if bfqq got new
	 * request in parent group and moved to this group while being added
	 * to service tree. Just ignore the event and move on.
	 */
	if (bfqg_stats_empty(stats)) /* 이미 empty로 표시되면 NVMe SQ/CQ idle 상태가 중복 기록되지 않도록 방지 */
		return;

	stats->start_empty_time = blk_time_get_ns(); /* empty 시작 시점: 이후 새 bio/request 도착까지 NVMe doorbell 중단 시간 측정 */
	bfqg_stats_mark_empty(stats);
}

void bfqg_stats_update_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (bfqg_stats_idling(stats)) { /* idling 플래그가 켜 있으면 NVMe doorbell 발행을 일시 중단한 상태 */
		u64 now = blk_time_get_ns();

		if (now > stats->start_idle_time) /* idle_time은 디바이스 처리 완료 후 추가 NVMe 제출까지의 간격 */
			bfq_stat_add(&stats->idle_time,
				      now - stats->start_idle_time);
		bfqg_stats_clear_idling(stats);
	}
}

void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	stats->start_idle_time = blk_time_get_ns();
	bfqg_stats_mark_idling(stats);
}

void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	bfq_stat_add(&stats->avg_queue_size_sum, /* avg_queue_size_sum에 현재 NVMe SQ backlog 길이(queued) 누적 */
		      blkg_rwstat_total(&stats->queued)); /* queued 값은 NVMe SQ backlog 예측의 근거자료 (추정) */
	bfq_stat_add(&stats->avg_queue_size_samples, 1);
	bfqg_stats_update_group_wait_time(stats);
}

/*
 * bfqg_stats_update_io_add - bio/request가 bfq_group에 추가될 때 통계 갱신
 * @bfqg: I/O가 속할 bfq_group
 * @bfqq: I/O를 추가한 bfq_queue
 * @opf: I/O 작업 플래그 (읽기/쓰기 구분)
 *
 * NVMe 관점: blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq로
 * 전달되는 각 I/O가 어떤 cgroup에 속하는지 기록한다. stats.queued는 현재
 * NVMe 제출 대기 중인 명령 수를 cgroup 단위로 추적하며, SQ/CQ 관리 시
 * backlog 예측에 활용될 수 있다 (추정).
 */
void bfqg_stats_update_io_add(struct bfq_group *bfqg, struct bfq_queue *bfqq,
			      blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.queued, opf, 1); /* queued 카운터 증가는 NVMe SQ/CQ backlog 길이 증가를 의미 */
	bfqg_stats_end_empty_time(&bfqg->stats);
	if (!(bfqq == bfqg->bfqd->in_service_queue)) /* in_service_queue가 아니면 doorbell 발행까지 group_wait_time 측정 시작 */
		bfqg_stats_set_start_group_wait_time(bfqg, bfqq_group(bfqq)); /* bfqq_group(bfqq)은 bio -> request -> bfq_queue -> bfq_group 라우팅 경로 */
}

/*
 * bfqg_stats_update_io_remove - 완료 또는 취소로 request가 제거될 때 통계 갱신
 * @bfqg: 대상 bfq_group
 * @opf: I/O 작업 플래그
 *
 * NVMe 관점: NVMe 완료 인터럽트(CQ entry 소비) 후 blk_mq_end_request가
 * 호출되면서 bfq측에서 request가 빠질 때 queued 카운터를 감소시킨다.
 */
void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.queued, opf, -1); /* NVMe 완료 인터럽트 후 request 해제 시 queued 감소, SQ/CQ 가용 슬롯 증가 */
}

void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.merged, opf, 1); /* bfq merge는 NVMe PRP/SGL 구성 전 bio/req 합병을 통해 DMA segment 수 감소 가능 (추정) */
}

/*
 * bfqg_stats_update_completion - I/O 완료 시점의 wait/service 시간 기록
 * @bfqg: 완료된 I/O가 속한 bfq_group
 * @start_time_ns: request가 생성된 시점
 * @io_start_time_ns: request가 실제로 서비스되기 시작한 시점
 * @opf: I/O 작업 플래그
 *
 * NVMe 관점:
 * - wait_time: bio가 bfq_queue에 들어온 후 nvme_submit_cmd(doorbell)이
 *   발행되기까지의 대기 시간. SQ가 가득 차거나 CID가 부족하면 증가한다.
 * - service_time: doorbell 발행 후 NVMe 컨트롤러가 CQ entry를 기록하여
 *   완료 인터럽트가 도달할 때까지의 실제 디바이스 처리 시간.
 */
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf)
{
	struct bfqg_stats *stats = &bfqg->stats;
	u64 now = blk_time_get_ns();

	if (now > io_start_time_ns) /* service_time = doorbell 발행 후 NVMe 컨트롤러가 CQ entry를 기록할 때까지 */
		blkg_rwstat_add(&stats->service_time, opf, /* now - io_start_time_ns는 실제 NVMe 디바이스 처리 시간 */
				now - io_start_time_ns);
	if (io_start_time_ns > start_time_ns)
		blkg_rwstat_add(&stats->wait_time, opf, /* wait_time = bio/bfq_queue 진입 후 nvme_submit_cmd(doorbell) 발행 전까지 */
				io_start_time_ns - start_time_ns); /* io_start_time_ns - start_time_ns는 NVMe SQ/CQ 슬롯 대기/스케줄링 지연 */
}

#else /* CONFIG_BFQ_CGROUP_DEBUG */

void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf) { }
void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf) { }
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf) { }
void bfqg_stats_update_dequeue(struct bfq_group *bfqg) { }
void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg) { }

#endif /* CONFIG_BFQ_CGROUP_DEBUG */

#ifdef CONFIG_BFQ_GROUP_IOSCHED

/*
 * blk-cgroup policy-related handlers
 * The following functions help in converting between blk-cgroup
 * internal structures and BFQ-specific structures.
 *
 * NVMe 관점: blk-cgroup은 상위 cgroup 단위의 I/O 제어를 담당하고, BFQ는
 * 그 내부에서 bfq_group/bfq_queue 단위로 NVMe 제출 순서를 세분화한다.
 * 아래 함수들은 blkcg_policy_bfq 콜백에서 사용하는 구조체 변환을 지원한다.
 */

static struct bfq_group *pd_to_bfqg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct bfq_group, pd) : NULL; /* blk-cgroup 구조체에서 bfq_group 추출, NVMe submission 경로에서 bio->bi_blkg -> bfq_group 탐색에 사용 */
}

struct blkcg_gq *bfqg_to_blkg(struct bfq_group *bfqg)
{
	return pd_to_blkg(&bfqg->pd); /* bfq_group -> blkcg_gq 변환은 NVMe 완료 시 rq->bio->bi_blkg 유효성 확인에 활용 */
}

static struct bfq_group *blkg_to_bfqg(struct blkcg_gq *blkg)
{
	return pd_to_bfqg(blkg_to_pd(blkg, &blkcg_policy_bfq)); /* bio->bi_blkg에서 bfq_group을 얻어 NVMe I/O의 cgroup 정책 결정 */
}

/*
 * bfq_group handlers
 * The following functions help in navigating the bfq_group hierarchy
 * by allowing to find the parent of a bfq_group or the bfq_group
 * associated to a bfq_queue.
 */

/*
 * bfqg_parent - 주어진 bfq_group의 부모 그룹 반환
 * @bfqg: 부모를 찾을 bfq_group
 *
 * NVMe 관점: blk-cgroup 계층 구조는 NVMe SQ/CQ에 직접 대응되지 않는다.
 * 다만, 부모/자식 간의 weight 상속은 NVMe 명령이 실제로 doorbell에 도달하기
 * 전에 어떤 cgroup이 더 많은 대역폭을 받을지 결정한다.
 */
static struct bfq_group *bfqg_parent(struct bfq_group *bfqg)
{
	struct blkcg_gq *pblkg = bfqg_to_blkg(bfqg)->parent; /* blk-cgroup 계층에서 부모 blkg 탐색, WFQ 가중치 상속 경로 */

	return pblkg ? blkg_to_bfqg(pblkg) : NULL; /* 부모가 없으면 root_group으로 폴팩, NVMe SQ 기본 weight 적용 */
}

/*
 * bfqq_group - bfq_queue가 속한 bfq_group 반환
 * @bfqq: 대상 bfq_queue
 *
 * NVMe 관점: bio -> request 변환 후 bio->bi_blkg을 통해 cgroup이 결정되고,
 * 그에 대응하는 bfq_group 안에 bfq_queue가 배치된다. 이 그룹의 weight가
 * NVMe SQ에 삽입될 명령의 상대적 우선순위를 좌우한다.
 */
struct bfq_group *bfqq_group(struct bfq_queue *bfqq)
{
	struct bfq_entity *group_entity = bfqq->entity.parent; /* entity.parent가 NULL이면 root_group으로 폴팩 */

	return group_entity ? container_of(group_entity, struct bfq_group, /* bfq_group 낶아 entity를 통해 cgroup 단위 WFQ 트리 연결 */
					   entity) :
			      bfqq->bfqd->root_group;
}

/*
 * The following two functions handle get and put of a bfq_group by
 * wrapping the related blk-cgroup hooks.
 */

static void bfqg_get(struct bfq_group *bfqg)
{
	refcount_inc(&bfqg->ref); /* bfq_group 레퍼런스 획득: NVMe 완료 콜백에서 참조되는 동안 해제 방지 */
}

static void bfqg_put(struct bfq_group *bfqg)
{
	if (refcount_dec_and_test(&bfqg->ref)) /* 레퍼런스 감소 후 0이면 bfq_group 해제 */
		kfree(bfqg); /* 해제 시점에 NVMe SQ/CQ에 남은 in-flight 명령 참조가 없어야 안전 */
}

/*
 * bfqg_and_blkg_get - bfq_group과 연결된 blkcg_gq 레퍼런스 획득
 * @bfqg: 대상 bfq_group
 *
 * NVMe 관점: bfq_queue가 nvme_queue_rq에 전달되는 동안 해당 bfq_group과
 * blkg이 소멸하지 않도록 보장한다. 그렇지 않으면 NVMe 완료 콜백에서
 * 통계 기록이나 weight 참조 시 use-after-free가 발생할 수 있다.
 */
static void bfqg_and_blkg_get(struct bfq_group *bfqg)
{
	/* see comments in bfq_bic_update_cgroup for why refcounting bfqg */
	bfqg_get(bfqg); /* bfq_group 레퍼런스 획득: NVMe 완료 콜백에서 통계/weight 참조 안전성 확보 */

	blkg_get(bfqg_to_blkg(bfqg)); /* blkg_get은 request_queue와 cgroup 간 링크 보호, rq->bio->bi_blkg 유효성 보장 */
}

void bfqg_and_blkg_put(struct bfq_group *bfqg)
{
	blkg_put(bfqg_to_blkg(bfqg)); /* blkg_put은 NVMe 완료 후 cgroup 구조체 레퍼런스 해제 */

	bfqg_put(bfqg); /* bfq_group 해제는 in-flight NVMe 명령에 대한 참조가 모두 끝난 후에만 발생 */
}

/*
 * bfqg_stats_update_legacy_io - 레거시 범용(request-based) I/O 통계 갱신
 * @q: 대상 request_queue
 * @rq: 완료된 request
 *
 * NVMe 관점: NVMe 완료 인터럽트 후 request->bio->bi_blkg로부터 bfq_group을
 * 찾아 bytes/ios 통계를 갱신한다. 이는 blk_mq_complete_request ->
 * nvme_pci_complete_rq -> blk_mq_end_request 경로에서 호출될 수 있다.
 */
void bfqg_stats_update_legacy_io(struct request_queue *q, struct request *rq)
{
	struct bfq_group *bfqg = blkg_to_bfqg(rq->bio->bi_blkg); /* rq->bio->bi_blkg에서 bfq_group 식별, NVMe 완료 인터럽트 컨텍스트에서 호출 가능 */

	if (!bfqg) /* bfq_group이 없으면 NVMe 완료 통계 기록 불가 */
		return;

	blkg_rwstat_add(&bfqg->stats.bytes, rq->cmd_flags, blk_rq_bytes(rq)); /* bytes 통계는 NVMe namespace별 cgroup 처리량(바이트) */
	blkg_rwstat_add(&bfqg->stats.ios, rq->cmd_flags, 1); /* ios 통계는 NVMe namespace별 cgroup 명령 완료 횟수 */
}

/* @stats = 0 */
static void bfqg_stats_reset(struct bfqg_stats *stats)
{
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* queued stats shouldn't be cleared */
	blkg_rwstat_reset(&stats->merged); /* merged ~ empty_time 초기화: cgroup 정책 재설정 시 NVMe 이력 초기화 */
	blkg_rwstat_reset(&stats->service_time);
	blkg_rwstat_reset(&stats->wait_time);
	bfq_stat_reset(&stats->time);
	bfq_stat_reset(&stats->avg_queue_size_sum);
	bfq_stat_reset(&stats->avg_queue_size_samples);
	bfq_stat_reset(&stats->dequeue);
	bfq_stat_reset(&stats->group_wait_time);
	bfq_stat_reset(&stats->idle_time);
	bfq_stat_reset(&stats->empty_time);
#endif
}

/* @to += @from */
static void bfqg_stats_add_aux(struct bfqg_stats *to, struct bfqg_stats *from)
{
	if (!to || !from)
		return;

#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* queued stats shouldn't be cleared */
	blkg_rwstat_add_aux(&to->merged, &from->merged); /* merged ~ empty_time 이관: 자식 cgroup 삭제 시 NVMe 처리 기록을 부모로 승계 */
	blkg_rwstat_add_aux(&to->service_time, &from->service_time);
	blkg_rwstat_add_aux(&to->wait_time, &from->wait_time);
	bfq_stat_add_aux(&to->time, &from->time);
	bfq_stat_add_aux(&to->avg_queue_size_sum, &from->avg_queue_size_sum);
	bfq_stat_add_aux(&to->avg_queue_size_samples,
			  &from->avg_queue_size_samples);
	bfq_stat_add_aux(&to->dequeue, &from->dequeue);
	bfq_stat_add_aux(&to->group_wait_time, &from->group_wait_time);
	bfq_stat_add_aux(&to->idle_time, &from->idle_time);
	bfq_stat_add_aux(&to->empty_time, &from->empty_time);
#endif
}

/*
 * Transfer @bfqg's stats to its parent's aux counts so that the ancestors'
 * recursive stats can still account for the amount used by this bfqg after
 * it's gone.
 *
 * NVMe 관점: cgroup이 삭제되더라도 그동안 NVMe SQ/CQ를 통해 처리된
 * 바이트/IOS/서비스 시간이 부모 그룹의 재귀 통계에 포함되도록 한다.
 */
static void bfqg_stats_xfer_dead(struct bfq_group *bfqg)
{
	struct bfq_group *parent;

	if (!bfqg) /* root_group */ /* root_group은 부모가 없어 NVMe 완료 통계 이관 불필요 */
		return;

	parent = bfqg_parent(bfqg); /* 부모 bfq_group 탐색: WFQ 트리에서 상위 그룹 결정 */

	lockdep_assert_held(&bfqg_to_blkg(bfqg)->q->queue_lock); /* queue_lock/disk_lock 보유 중: NVMe 완료 콜백과 cgroup 구조 변경 동기화 */

	if (unlikely(!parent)) /* 부모가 없으면 root_group으로 처리, NVMe I/O 최상위 그룹에 통계 유지 */
		return;

	bfqg_stats_add_aux(&parent->stats, &bfqg->stats); /* aux 카운터로 자식 그룹의 NVMe 처리 기록을 부모에게 승계 */
	bfqg_stats_reset(&bfqg->stats); /* 이관 후 자식 통계 리셋: 다음 cgroup 재사용 시 이전 NVMe 이력 누적 방지 */
}

/*
 * bfq_init_entity - bfq_entity를 지정된 bfq_group에 초기화/연결
 * @entity: 초기화할 bfq_entity (bfq_queue 또는 bfq_group의 entity)
 * @bfqg: 소속시킬 bfq_group
 *
 * NVMe 관점: bio -> request 생성 후 bfq_queue의 entity가 어떤 bfq_group의
 * sched_data에 매달리는지 결정한다. entity->weight는 WFQ 가상 시간 계산에
 * 사용되어 NVMe SQ에 제출될 명령의 상대적 서비스 비율을 조정한다.
 */
void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	entity->weight = entity->new_weight; /* entity weight 초기화: WFQ 가상 시간 계산 입력값, NVMe doorbell 발행 빈도 결정 */
	entity->orig_weight = entity->new_weight; /* orig_weight 백업: 우선순위 변경 후 NVMe SQ 서비스 비율 복원 기준 */
	if (bfqq) {
		bfqq->ioprio = bfqq->new_ioprio; /* ioprio는 io.sched.priority에 매핑되어 NVMe SQ 선택 우선순위에 영향 */
		bfqq->ioprio_class = bfqq->new_ioprio_class; /* ioprio_class는 RT/BE/IDLE 클래스로 NVMe I/O 긴급도 분류 */
		/*
		 * Make sure that bfqg and its associated blkg do not
		 * disappear before entity.
		 */
		bfqg_and_blkg_get(bfqg); /* blkg_get은 NVMe 완료까지 bfq_group/blkg 객체 유지 */
	}
	entity->parent = bfqg->my_entity; /* NULL for root group */ /* parent가 NULL이면 root_group 직속, 최상위 WFQ 참여 */
	entity->sched_data = &bfqg->sched_data; /* sched_data 연결로 부모의 service_tree에 NVMe I/O 엔티티 등록 */
}

static void bfqg_stats_exit(struct bfqg_stats *stats)
{
	blkg_rwstat_exit(&stats->bytes);
	blkg_rwstat_exit(&stats->ios); /* bytes ~ empty_time 자료구조 해제: bfq_group이 NVMe 완료 후에만 호출되어야 안전 */
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	blkg_rwstat_exit(&stats->merged);
	blkg_rwstat_exit(&stats->service_time);
	blkg_rwstat_exit(&stats->wait_time);
	blkg_rwstat_exit(&stats->queued);
	bfq_stat_exit(&stats->time);
	bfq_stat_exit(&stats->avg_queue_size_sum);
	bfq_stat_exit(&stats->avg_queue_size_samples);
	bfq_stat_exit(&stats->dequeue);
	bfq_stat_exit(&stats->group_wait_time);
	bfq_stat_exit(&stats->idle_time);
	bfq_stat_exit(&stats->empty_time);
#endif
}

static int bfqg_stats_init(struct bfqg_stats *stats, gfp_t gfp)
{
	if (blkg_rwstat_init(&stats->bytes, gfp) || /* bytes/ios 통계 초기화: NVMe namespace별 cgroup 처리량/명령 수 추적 시작 */
	    blkg_rwstat_init(&stats->ios, gfp))
		goto error;

#ifdef CONFIG_BFQ_CGROUP_DEBUG
	if (blkg_rwstat_init(&stats->merged, gfp) || /* merged/queued/service_time/wait_time 등 NVMe 관련 지연/합병 통계 초기화 */
	    blkg_rwstat_init(&stats->service_time, gfp) ||
	    blkg_rwstat_init(&stats->wait_time, gfp) ||
	    blkg_rwstat_init(&stats->queued, gfp) ||
	    bfq_stat_init(&stats->time, gfp) ||
	    bfq_stat_init(&stats->avg_queue_size_sum, gfp) ||
	    bfq_stat_init(&stats->avg_queue_size_samples, gfp) ||
	    bfq_stat_init(&stats->dequeue, gfp) ||
	    bfq_stat_init(&stats->group_wait_time, gfp) ||
	    bfq_stat_init(&stats->idle_time, gfp) ||
	    bfq_stat_init(&stats->empty_time, gfp))
		goto error; /* 통계 초기화 실패 시 NVMe I/O 통계 추적 불가, I/O 제출은 계속 가능 */
#endif

	return 0;

error:
	bfqg_stats_exit(stats); /* 부분 초기화 실패 시 이미 할당된 per-cpu 통계 자료구조 정리 */
	return -ENOMEM;
}

static struct bfq_group_data *cpd_to_bfqgd(struct blkcg_policy_data *cpd)
{
	return cpd ? container_of(cpd, struct bfq_group_data, pd) : NULL; /* blkcg_policy_data에서 bfq_group_data 추출 */
}

static struct bfq_group_data *blkcg_to_bfqgd(struct blkcg *blkcg)
{
	return cpd_to_bfqgd(blkcg_to_cpd(blkcg, &blkcg_policy_bfq)); /* blkcg에서 bfq_group_data 획득, cgroup별 NVMe weight 정책 접근 */
}

/*
 * bfq_cpd_alloc - blkcg_policy_data 할당 및 기본 weight 설정
 * @gfp: 메모리 할당 플래그
 *
 * NVMe 관점: 새 cgroup 생성 시 해당 cgroup의 기본 I/O weight를 결정한다.
 * 이 weight는 추후 NVMe 명령이 bfq_queue를 통해 스케줄링될 때 서비스 비율의
 * 기준점이 된다.
 */
static struct blkcg_policy_data *bfq_cpd_alloc(gfp_t gfp)
{
	struct bfq_group_data *bgd;

	bgd = kzalloc_obj(*bgd, gfp); /* kzalloc_obj로 cgroup 데이터 할당, 실패 시 해당 cgroup의 NVMe I/O 정책 적용 불가 */
	if (!bgd) /* 메모리 부족 시 NVMe SQ/CQ의 cgroup 단위 우선순위 설정 불가 */
		return NULL;

	bgd->weight = CGROUP_WEIGHT_DFL; /* default weight는 NVMe SQ에서 root group과 동일 서비스 비율 기준 */
	return &bgd->pd;
}

static void bfq_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(cpd_to_bfqgd(cpd)); /* cgroup 해제 시 bfq_group_data 메모리 반환, 이후 NVMe I/O는 root_group 정책 적용 */
}

/*
 * bfq_pd_alloc - gendisk에 연결될 bfq_group 할당
 * @disk: 대상 디스크
 * @blkcg: 대상 blkcg
 * @gfp: 메모리 할당 플래그
 *
 * NVMe 관점: NVMe namespace에 해당하는 gendisk마다 blkcg별 bfq_group을
 * 생성한다. bfq_group은 NVMe SQ/CQ에 직접 대응하지는 않지만, 디스크별로
 * 독립적인 weight/통계 트리를 형성하여 NVMe I/O 분배 정책을 세분화한다.
 */
static struct blkg_policy_data *bfq_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct bfq_group *bfqg;

	bfqg = kzalloc_node(sizeof(*bfqg), gfp, disk->node_id); /* kzalloc_node는 NUMA 노드 로컬 메모리로 NVMe DMA 관련 할당과 같은 노드 선호 (추정) */
	if (!bfqg) /* 메모리 할당 실패 시 bfq_group 생성 실패 */
		return NULL;

	if (bfqg_stats_init(&bfqg->stats, gfp)) { /* stats init 실패 시 NVMe I/O 통계 추적 불가 */
		kfree(bfqg);
		return NULL; /* 해당 blkcg의 NVMe I/O는 root_group 정책으로 폴팩 */
	}

	/* see comments in bfq_bic_update_cgroup for why refcounting */
	refcount_set(&bfqg->ref, 1); /* refcount 1은 생성자 참조, NVMe 완료 콜백과의 레이스 방지 기준 */
	return &bfqg->pd;
}

/*
 * bfq_pd_init - 할당된 bfq_group의 스케줄링 관련 필드 초기화
 * @pd: 초기화할 blkg_policy_data
 *
 * NVMe 관점: bfq_group의 entity에 weight를 부여하고, my_sched_data,
 * rq_pos_tree 등을 설정한다. rq_pos_tree는 NVMe 명령의 LBA 순서에 따라
 * request를 정렬하여 merge/sort를 돕는다.
 */
static void bfq_pd_init(struct blkg_policy_data *pd)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	struct bfq_group *bfqg = blkg_to_bfqg(blkg);
	struct bfq_data *bfqd = blkg->q->elevator->elevator_data;
	struct bfq_entity *entity = &bfqg->entity;
	struct bfq_group_data *d = blkcg_to_bfqgd(blkg->blkcg);

	entity->orig_weight = entity->weight = entity->new_weight = d->weight; /* weight 복사는 WFQ 가중치 초기화, NVMe SQ 서비스 비율 결정 */
	entity->my_sched_data = &bfqg->sched_data; /* my_sched_data는 자식 entity들이 NVMe 제출 우선순위를 상속받을 서비스 트리 */
	entity->last_bfqq_created = NULL;

	bfqg->my_entity = entity; /*
				   * the root_group's will be set to NULL
				   * in bfq_init_queue()
				   */
	bfqg->bfqd = bfqd;
	bfqg->active_entities = 0; /* active_entities는 NVMe SQ에 제출할 후보 bfq_queue/bfq_group 엔티티 수 */
	bfqg->num_queues_with_pending_reqs = 0; /* num_queues_with_pending_reqs는 NVMe SQ에 제출할 후보 bfq_queue 수 */
	bfqg->rq_pos_tree = RB_ROOT; /* rq_pos_tree는 LBA 순서 정렬로 NVMe 명령 순차 처리/merge 기회 제공 */
}

static void bfq_pd_free(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd); /* bfq_group에서 stats 자료구조 정리, NVMe 완료 콜백 참조 종료 후 호출 */

	bfqg_stats_exit(&bfqg->stats);
	bfqg_put(bfqg); /* 레퍼런스 0이면 bfq_group 해제, in-flight NVMe 명령 참조 주의 */
}

static void bfq_pd_reset_stats(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);

	bfqg_stats_reset(&bfqg->stats); /* 통계 리셋은 cgroup 정책 재설정 시 NVMe 이력 초기화 */
}

/*
 * bfq_group_set_parent - bfq_group을 부모 그룹 아래로 연결
 * @bfqg: 연결할 자식 bfq_group
 * @parent: 부모 bfq_group
 *
 * NVMe 관점: cgroup 계층 구조에 따라 자식 그룹의 entity가 부부 그룹의
 * sched_data/service_tree에 매달린다. 이 계층적 WFQ는 NVMe SQ에 도달하기
 * 전에 어느 cgroup의 bfq_queue가 먼저 선택될지를 결정한다.
 */
static void bfq_group_set_parent(struct bfq_group *bfqg,
					struct bfq_group *parent)
{
	struct bfq_entity *entity;

	entity = &bfqg->entity;
	entity->parent = parent->my_entity; /* 부모 entity로 연결되어 NVMe I/O가 상위 WFQ 트리를 통해 스케줄링됨 */
	entity->sched_data = &parent->sched_data; /* 부모 sched_data 등록: NVMe doorbell 발행 우선순위 계층 구성 */
}

/*
 * bfq_link_bfqg - blk-cgroup 계층에 맞춰 BFQ 내부 그룹 체인을 연결
 * @bfqd: bfq_data (디스크별 BFQ 데이터)
 * @bfqg: 연결할 bfq_group
 *
 * NVMe 관점: cgroup이 동적으로 생성/변경될 때마다 bfq_group 간 부모-자식
 * 관계를 재구성한다. 이를 통해 NVMe I/O의 cgroup 단위 가중치가 실제
 * 계층 구조를 반영하게 된다.
 */
static void bfq_link_bfqg(struct bfq_data *bfqd, struct bfq_group *bfqg)
{
	struct bfq_group *parent;
	struct bfq_entity *entity;

	/*
	 * Update chain of bfq_groups as we might be handling a leaf group
	 * which, along with some of its relatives, has not been hooked yet
	 * to the private hierarchy of BFQ.
	 */
	entity = &bfqg->entity;
	for_each_entity(entity) { /* for_each_entity는 entity 상향 순회, cgroup 계층 누락 시 복구 */
		struct bfq_group *curr_bfqg = container_of(entity, /* 현재 entity가 속한 bfq_group 획득 */
						struct bfq_group, entity);
		if (curr_bfqg != bfqd->root_group) { /* root_group이 아닌 그룹에 대해 부모를 찾아 연결 */
			parent = bfqg_parent(curr_bfqg); /* 부모 bfq_group 탐색 */
			if (!parent)
				parent = bfqd->root_group;
			bfq_group_set_parent(curr_bfqg, parent); /* 부모가 없으면 root_group에 직접 연결 */
		} /* NVMe I/O의 cgroup 단위 우선순위 트리 복원 */
	}
}

/*
 * bfq_bio_bfqg - bio에 연결된 blkcg로부터 적절한 bfq_group을 찾음
 * @bfqd: bfq_data
 * @bio: 대상 bio
 *
 * NVMe 관점: blk_mq_submit_bio에서 bio가 제출될 때 bio->bi_blkg를 통해
 * 소속 cgroup을 식별하고, 그에 대응하는 bfq_group을 반환한다. 반환된
 * bfq_group의 sched_data 안에서 bfq_queue가 선택되며, 이후
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)로 이어진다.
 */
struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio)
{
	struct blkcg_gq *blkg = bio->bi_blkg; /* bio->bi_blkg는 blk_mq_submit_bio 시점에 설정된 cgroup 링크 */
	struct bfq_group *bfqg;

	while (blkg) {
		if (!blkg->online) { /* online 아닌 blkg는 NVMe 제출 시 유효한 cgroup 정책이 아님 */
			blkg = blkg->parent; /* 부모 blkg로 폴팩하여 살아있는 cgroup 정책 적용 */
			continue;
		}
		bfqg = blkg_to_bfqg(blkg);
		if (bfqg->pd.online) { /* pd.online인 bfq_group만 NVMe I/O 라우팅 대상 */
			bio_associate_blkg_from_css(bio, &blkg->blkcg->css); /* bio_associate_blkg_from_css는 이후 merge/제출 과정에서 안정적인 cgroup 참조 보장 */
			return bfqg;
		}
		blkg = blkg->parent; /* 다음 상위 blkg 탐색, NVMe SQ/CQ 정책 적용 가능 그룹 찾기 */
	}
	bio_associate_blkg_from_css(bio, /* 유효한 blkg가 없으면 root_group의 css로 bio 재연결 */
				&bfqg_to_blkg(bfqd->root_group)->blkcg->css);
	return bfqd->root_group; /* root_group으로 폴팩, NVMe I/O는 기본 weight 정책 따름 */
}

/**
 * bfq_bfqq_move - migrate @bfqq to @bfqg.
 * @bfqd: queue descriptor.
 * @bfqq: the queue to move.
 * @bfqg: the group to move to.
 *
 * Move @bfqq to @bfqg, deactivating it from its old group and reactivating
 * it on the new one.  Avoid putting the entity on the old group idle tree.
 *
 * Must be called under the scheduler lock, to make sure that the blkg
 * owning @bfqg does not disappear (see comments in
 * bfq_bic_update_cgroup on guaranteeing the consistency of blkg
 * objects).
 *
 * NVMe 관점:
 * - cgroup이 변경되면 해당 bfq_queue를 새 bfq_group으로 이동시킨다.
 * - 이동 과정에서 bfq_queue를 일시적으로 비활성화(deactivate)하고 새 그룹의
 *   service_tree에 다시 삽입(activate)한다.
 * - 이때 NVMe SQ/CQ에는 아직 명령이 내부에 있을 수 있으므로, in_flight request
 *   완료 콜백에서 stale한 bfq_group 참조가 없도록 레퍼런스 관리가 중요하다.
 * - bfq_schedule_dispatch를 통해 NVMe 제출(dispatch)를 재개할 수 있다.
 */
void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		   struct bfq_group *bfqg)
{
	struct bfq_entity *entity = &bfqq->entity; /* bfqq entity: WFQ 트리에서 이동할 위치 */
	struct bfq_group *old_parent = bfqq_group(bfqq); /* old_parent는 현재 NVMe doorbell 시퀀스가 속한 WFQ 그룹 */
	bool has_pending_reqs = false;

	/*
	 * No point to move bfqq to the same group, which can happen when
	 * root group is offlined
	 */
	if (old_parent == bfqg) /* 동일 그룹 이동은 불필요, NVMe doorbell 패턴 변화 없음 */
		return;

	/*
	 * oom_bfqq is not allowed to move, oom_bfqq will hold ref to root_group
	 * until elevator exit.
	 */
	if (bfqq == &bfqd->oom_bfqq) /* oom_bfqq는 메모리 부족 시 NVMe I/O 제출 보장용 예비 큐 */
		return; /* oom_bfqq는 root_group에 고정되어 cgroup 정책 실패에도 NVMe doorbell 발행 가능 */
	/*
	 * Get extra reference to prevent bfqq from being freed in
	 * next possible expire or deactivate.
	 */
	bfqq->ref++; /* ref++는 이동 중 bfqq가 expire/deactivate로 해제되지 않도록 보호 */

	if (entity->in_groups_with_pending_reqs) {
		has_pending_reqs = true; /* pending reqs 그룹에서 제거하여 dispatch 후보에서 잠시 배제, NVMe SQ 혼잡 방지 */
		bfq_del_bfqq_in_groups_with_pending_reqs(bfqq); /* NVMe multi-queue 상에서 다른 bfq_queue의 doorbell 기회 확보 */
	}

	/* If bfqq is empty, then bfq_bfqq_expire also invokes
	 * bfq_del_bfqq_busy, thereby removing bfqq and its entity
	 * from data structures related to current group. Otherwise we
	 * need to remove bfqq explicitly with bfq_deactivate_bfqq, as
	 * we do below.
	 */
	if (bfqq == bfqd->in_service_queue) /* in_service_queue면 현재 NVMe SQ에 제출 중인 큐의 서비스를 만료 */
		bfq_bfqq_expire(bfqd, bfqd->in_service_queue, /* bfq_bfqq_expire는 현재 NVMe doorbell 시퀀스 중단 및 새 큐 선정 */
				false, BFQQE_PREEMPTED); /* preempted 플래그는 NVMe SQ/CQ 리소스를 다른 cgroup에 양보 */

	if (bfq_bfqq_busy(bfqq)) /* busy 상태면 서비스 트리에서 비활성화, NVMe 제출 후보 제외 */
		bfq_deactivate_bfqq(bfqd, bfqq, false, false); /* deactivate는 WFQ 가상 시간 갱신과 NVMe dispatch 트리 재구성 */
	else if (entity->on_st_or_in_serv) /* idle tree에 있으면 NVMe doorbell 발행이 일시 중단된 상태 */
		bfq_put_idle_entity(bfq_entity_service_tree(entity), entity); /* idle tree에서 제거하여 NVMe SQ 제출 후보로 복귀 준비 */
	bfqg_and_blkg_put(old_parent); /* old_parent 레퍼런스 해제, in-flight NVMe 명령 완료 후에도 안전해야 함 */

	bfq_reassign_last_bfqq(bfqq, NULL);
	entity->parent = bfqg->my_entity; /* 새 부모 entity 연결: NVMe I/O가 새 cgroup의 WFQ 트리에 참여 */
	entity->sched_data = &bfqg->sched_data; /* 새 sched_data 등록으로 NVMe doorbell 우선순위 재계산 */
	/* pin down bfqg and its associated blkg  */
	bfqg_and_blkg_get(bfqg); /* 새 bfqg/blkg 레퍼런스 획득, NVMe 완료까지 객체 유지 */

	if (has_pending_reqs) /* pending reqs 복원: NVMe SQ에 제출할 후보로 다시 등록 */
		bfq_add_bfqq_in_groups_with_pending_reqs(bfqq); /* dispatch 후보 복귀는 bfq_schedule_dispatch -> nvme_queue_rq 연결 가능 */

	if (bfq_bfqq_busy(bfqq)) {
		if (unlikely(!bfqd->nonrot_with_queueing)) /* nonrot_with_queueing 체크는 NVMe 비회전 장치의 위치 트리 사용 여부 */
			bfq_pos_tree_add_move(bfqd, bfqq);
		bfq_activate_bfqq(bfqd, bfqq); /* 새 그룹에서 activate: NVMe SQ 제출 우선순위 트리에 재진입 */
	} /* 위치 트리 추가는 NVMe 명령 LBA 순서 최적화를 위한 선택적 단계 */

	if (!bfqd->in_service_queue && !bfqd->tot_rq_in_driver) /* in_service_queue가 없고 driver에 request가 없으면 NVMe 제출(dispatch) 재개 필요 */
		bfq_schedule_dispatch(bfqd); /* bfq_schedule_dispatch는 blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로 연결 */
	/* release extra ref taken above, bfqq may happen to be freed now */
	bfq_put_queue(bfqq); /* 추가 ref 해제, 이때 bfqq가 완전히 해제될 수 있음 */
}

/*
 * bfq_sync_bfqq_move - 동기(synchronous) bfq_queue의 cgroup 이동 처리
 * @bfqd: bfq_data
 * @sync_bfqq: 이동할 동기 bfq_queue
 * @bic: 해당 bfq_io_cq
 * @bfqg: 목표 bfq_group
 * @act_idx: actuator 인덱스
 *
 * NVMe 관점: 동기 큐는 보통 한 프로세스가 독점하므로 단순 이동이 가능하지만,
 * cooperative merge가 발생한 경우(여러 프로세스가 동일 bfq_queue 공유)에는
 * merge chain이 모두 동일 cgroup에 속하는지 확인해야 한다. 그렇지 않으면
 * 서로 다른 cgroup의 I/O가 하나의 NVMe doorbell 시퀀스로 합쳐져 우선순위
 * 정책이 묵살당할 수 있다.
 */
static void bfq_sync_bfqq_move(struct bfq_data *bfqd,
			       struct bfq_queue *sync_bfqq,
			       struct bfq_io_cq *bic,
			       struct bfq_group *bfqg,
			       unsigned int act_idx)
{
	struct bfq_queue *bfqq;

	if (!sync_bfqq->new_bfqq && !bfq_bfqq_coop(sync_bfqq)) { /* 단일 사용자면 단순 이동, NVMe doorbell 시퀀스에 영향 최소화 */
		/* We are the only user of this bfqq, just move it */
		if (sync_bfqq->entity.sched_data != &bfqg->sched_data) /* sched_data가 이미 목표 그룹이면 NVMe WFQ 트리 변경 불필요 */
			bfq_bfqq_move(bfqd, sync_bfqq, bfqg); /* bfq_bfqq_move는 새 bfq_group의 service_tree에 NVMe I/O 엔티티 재등록 */
		return;
	}

	/*
	 * The queue was merged to a different queue. Check
	 * that the merge chain still belongs to the same
	 * cgroup.
	 */
	for (bfqq = sync_bfqq; bfqq; bfqq = bfqq->new_bfqq) /* merge chain이 모두 동일 cgroup인지 확인, 서로 다른 cgroup이면 분리 */
		if (bfqq->entity.sched_data != &bfqg->sched_data) /* entity.sched_data 불일치는 NVMe SQ에 제출될 명령의 cgroup 소속이 다름을 의미 */
			break;
	if (bfqq) {
		/*
		 * Some queue changed cgroup so the merge is not valid
		 * anymore. We cannot easily just cancel the merge (by
		 * clearing new_bfqq) as there may be other processes
		 * using this queue and holding refs to all queues
		 * below sync_bfqq->new_bfqq. Similarly if the merge
		 * already happened, we need to detach from bfqq now
		 * so that we cannot merge bio to a request from the
		 * old cgroup.
		 */
		bfq_put_cooperator(sync_bfqq); /* cooperative merge 해제: 서로 다른 cgroup의 NVMe I/O가 하나의 doorbell 시퀀스로 합쳐지지 않도록 방지 */
		bic_set_bfqq(bic, NULL, true, act_idx); /* bic에서 sync_bfqq 분리로 새로운 NVMe 명령은 old cgroup과 분리 */
		bfq_release_process_ref(bfqd, sync_bfqq); /* bfqq 레퍼런스 해제는 in-flight NVMe 명령 완료 후에도 안전해야 함 */
	} /* 새 bfqq는 이후 bio 제출 시 새 cgroup의 bfq_group에 할당 */
}

/**
 * __bfq_bic_change_cgroup - move @bic to @bfqg.
 * @bfqd: the queue descriptor.
 * @bic: the bic to move.
 * @bfqg: the group to move to.
 *
 * Move bic to blkcg, assuming that bfqd->lock is held; which makes
 * sure that the reference to cgroup is valid across the call (see
 * comments in bfq_bic_update_cgroup on this issue)
 *
 * NVMe 관점: 멀티 액추에이터(Multi-actuator) NVMe 디바이스를 고려하여
 * 각 actuator 인덱스(act_idx)별로 async/sync bfq_queue를 모두 이동시킨다.
 * 이를 통해 actuator별로 독립된 NVMe SQ/CQ에 매핑되는 요청들이 올바른
 * cgroup 정책을 따른다.
 */
static void __bfq_bic_change_cgroup(struct bfq_data *bfqd,
				    struct bfq_io_cq *bic,
				    struct bfq_group *bfqg)
{
	unsigned int act_idx;

	for (act_idx = 0; act_idx < bfqd->num_actuators; act_idx++) { /* num_actuators 루프는 멀티 액추에이터 NVMe에서 actuator별 SQ/CQ에 대응 (추정) */
		struct bfq_queue *async_bfqq = bic_to_bfqq(bic, false, act_idx); /* async_bfqq는 프로세스별 비동기 큐, NVMe writeback/flush 명령 후보 */
		struct bfq_queue *sync_bfqq = bic_to_bfqq(bic, true, act_idx); /* sync_bfqq는 프로세스별 동기 큐, NVMe read/write 명령 후보 */

		if (async_bfqq && /* async 큐는 cgroup 변경 시 해제 후 재할당, NVMe SQ 우선순위 트리 단순화 */
		    async_bfqq->entity.sched_data != &bfqg->sched_data) {
			bic_set_bfqq(bic, NULL, false, act_idx); /* bic_set_bfqq NULL은 기존 async 큐와의 연결 해제 */
			bfq_release_process_ref(bfqd, async_bfqq); /* 레퍼런스 해제는 기존 async 큐의 NVMe 완료 후에도 안전해야 함 */
		}

		if (sync_bfqq) /* sync 큐 이동은 동일 actuator의 NVMe SQ/CQ 정책 연속성 유지 */
			bfq_sync_bfqq_move(bfqd, sync_bfqq, bic, bfqg, act_idx); /* bfq_sync_bfqq_move는 cooperative merge 여부를 고려한 안전한 cgroup 이동 */
	}
}

/*
 * bfq_bic_update_cgroup - bio 제출 시점에 cgroup 변경 여부를 확인하고 갱신
 * @bic: 해당 bfq_io_cq
 * @bio: 제출된 bio
 *
 * NVMe 관점: bio가 제출될 때마다 bfq_bio_bfqg를 호출하여 bio->bi_blkg에
 * 해당하는 bfq_group을 찾는다. cgroup이 바뀌었다면 bfq_link_bfqg로 계층을
 * 연결하고 __bfq_bic_change_cgroup으로 bfq_queue를 이동시킨 후,
 * bfq_dispatch_rq -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로
 * 올바른 weight가 적용되도록 한다.
 */
void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio)
{
	struct bfq_data *bfqd = bic_to_bfqd(bic);
	struct bfq_group *bfqg = bfq_bio_bfqg(bfqd, bio); /* bfq_bio_bfqg는 bio의 cgroup -> bfq_group 매핑, NVMe 제출 경로의 첫 단계 */
	uint64_t serial_nr; /* serial_nr은 cgroup 변경 여부를 locklessly 확인하는 토큰 */

	serial_nr = bfqg_to_blkg(bfqg)->blkcg->css.serial_nr; /* bfqg_to_blkg(bfqg)->blkcg->css.serial_nr은 현재 bio가 속한 cgroup 식별자 */

	/*
	 * Check whether blkcg has changed.  The condition may trigger
	 * spuriously on a newly created cic but there's no harm.
	 */
	if (unlikely(!bfqd) || likely(bic->blkcg_serial_nr == serial_nr)) /* serial_nr 일치하면 cgroup 변경 없음, NVMe doorbell 지연 최소화 */
		return;

	/*
	 * New cgroup for this process. Make sure it is linked to bfq internal
	 * cgroup hierarchy.
	 */
	bfq_link_bfqg(bfqd, bfqg); /* bfq_link_bfqg는 cgroup 계층 누락 시 NVMe WFQ 트리 복구 */
	__bfq_bic_change_cgroup(bfqd, bic, bfqg);
	bic->blkcg_serial_nr = serial_nr; /* serial_nr 갱신으로 다음 bio 제출 시 재확인 비용 절감 */
}

/**
 * bfq_flush_idle_tree - deactivate any entity on the idle tree of @st.
 * @st: the service tree being flushed.
 */
static void bfq_flush_idle_tree(struct bfq_service_tree *st)
{
	struct bfq_entity *entity = st->first_idle; /* idle tree에 남은 entity를 비활성화, NVMe SQ 제출 후보 정리 */

	for (; entity ; entity = st->first_idle) /* st->first_idle 순회는 WFQ idle 상태의 entity를 정리 */
		__bfq_deactivate_entity(entity, false); /* __bfq_deactivate_entity는 가상 시간 갱신과 함께 NVMe dispatch 트리에서 제거 */
}

/**
 * bfq_reparent_leaf_entity - move leaf entity to the root_group.
 * @bfqd: the device data structure with the root group.
 * @entity: the entity to move, if entity is a leaf; or the parent entity
 *	    of an active leaf entity to move, if entity is not a leaf.
 * @ioprio_class: I/O priority class to reparent.
 */
static void bfq_reparent_leaf_entity(struct bfq_data *bfqd,
				     struct bfq_entity *entity,
				     int ioprio_class)
{
	struct bfq_queue *bfqq;
	struct bfq_entity *child_entity = entity;

	while (child_entity->my_sched_data) { /* leaf not reached yet */ /* leaf entity에 도달할 때까지 하위 service_tree 순회 */
		struct bfq_sched_data *child_sd = child_entity->my_sched_data;
		struct bfq_service_tree *child_st = child_sd->service_tree + /* ioprio_class별 service_tree 선택: RT/BE/IDLE에 따른 NVMe SQ 우선순위 */
			ioprio_class;
		struct rb_root *child_active = &child_st->active;

		child_entity = bfq_entity_of(rb_first(child_active)); /* active 트리의 첫 번째 entity는 가장 먼저 NVMe doorbell을 받을 후보 */

		if (!child_entity) /* active entity가 없으면 in_service_entity를 leaf로 간주 */
			child_entity = child_sd->in_service_entity;
	}

	bfqq = bfq_entity_to_bfqq(child_entity);
	bfq_bfqq_move(bfqd, bfqq, bfqd->root_group); /* leaf bfq_queue를 root_group으로 이동, NVMe 완료 콜백에서 참조할 그룹 보존 */
}

/**
 * bfq_reparent_active_queues - move to the root group all active queues.
 * @bfqd: the device data structure with the root group.
 * @bfqg: the group to move from.
 * @st: the service tree to start the search from.
 * @ioprio_class: I/O priority class to reparent.
 */
static void bfq_reparent_active_queues(struct bfq_data *bfqd,
				       struct bfq_group *bfqg,
				       struct bfq_service_tree *st,
				       int ioprio_class)
{
	struct rb_root *active = &st->active; /* active 트리의 모든 leaf를 root로 이동 */
	struct bfq_entity *entity;

	while ((entity = bfq_entity_of(rb_first(active)))) /* active 트리 첫 entity를 반복적으로 root_group으로 reparent */
		bfq_reparent_leaf_entity(bfqd, entity, ioprio_class); /* NVMe SQ에 남은 명령의 완료 통계가 root_group에서 계속 누적되도록 함 */

	if (bfqg->sched_data.in_service_entity) /* in_service_entity는 active 트리에서 분리된 현재 서비스 중인 NVMe 명령 소유 */
		bfq_reparent_leaf_entity(bfqd, /* in_service_entity도 root_group으로 이동하여 NVMe 완료 시 통계 안정성 확보 */
					 bfqg->sched_data.in_service_entity,
					 ioprio_class); /* ioprio_class별로 NVMe SQ/CQ 우선순위 그룹 정리 */
}

/**
 * bfq_pd_offline - deactivate the entity associated with @pd,
 *		    and reparent its children entities.
 * @pd: descriptor of the policy going offline.
 *
 * blkio already grabs the queue_lock for us, so no need to use
 * RCU-based magic
 *
 * NVMe 관점: cgroup이 오프라인되면 해당 bfq_group의 활성 큐들을 root_group으로
 * 옮기고 idle tree를 비운다. 이때 NVMe SQ/CQ에 내부에 있는 in-flight 명령은
 * 완료 콜백에서 통계가 누락될 수 있으므로, 오프라인 이후의 완료 통계는
 * 일부 손실될 수 있다고 명시되어 있다 (see bfqg_stats_xfer_dead).
 */
static void bfq_pd_offline(struct blkg_policy_data *pd)
{
	struct bfq_service_tree *st;
	struct bfq_group *bfqg = pd_to_bfqg(pd);
	struct bfq_data *bfqd = bfqg->bfqd; /* 오프라인 대상 bfq_group */
	struct bfq_entity *entity = bfqg->my_entity; /* 오프라인할 group entity */
	unsigned long flags;
	int i;

	spin_lock_irqsave(&bfqd->lock, flags); /* queue_lock/disk_lock: NVMe 완료 인터럽트와 cgroup 구조 변경 동기화 */

	if (!entity) /* root group */ /* root group은 NVMe I/O의 최상위 그룹으로 오프라인 대상 아님 */
		goto put_async_queues; /* root group 오프라인 시 NVMe SQ/CQ에 명령 제출 불가능해짐 */

	/*
	 * Empty all service_trees belonging to this group before
	 * deactivating the group itself.
	 */
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++) { /* BFQ_IOPRIO_CLASSES 루프는 RT/BE/IDLE 클래스별 NVMe SQ 우선순위 트리 정리 */
		st = bfqg->sched_data.service_tree + i; /* ioprio_class별 service_tree 선택 */

		/*
		 * It may happen that some queues are still active
		 * (busy) upon group destruction (if the corresponding
		 * processes have been forced to terminate). We move
		 * all the leaf entities corresponding to these queues
		 * to the root_group.
		 * Also, it may happen that the group has an entity
		 * in service, which is disconnected from the active
		 * tree: it must be moved, too.
		 * There is no need to put the sync queues, as the
		 * scheduler has taken no reference.
		 */
		bfq_reparent_active_queues(bfqd, bfqg, st, i); /* active 큐를 root_group으로 이동, NVMe 완료 통계 누락 방지 */

		/*
		 * The idle tree may still contain bfq_queues
		 * belonging to exited task because they never
		 * migrated to a different cgroup from the one being
		 * destroyed now. In addition, even
		 * bfq_reparent_active_queues() may happen to add some
		 * entities to the idle tree. It happens if, in some
		 * of the calls to bfq_bfqq_move() performed by
		 * bfq_reparent_active_queues(), the queue to move is
		 * empty and gets expired.
		 */
		bfq_flush_idle_tree(st); /* idle tree flush: NVMe SQ에 제출되지 않을 후보 큐 정리 */
	}

	__bfq_deactivate_entity(entity, false); /* group entity 비활성화: NVMe WFQ 트리에서 제거 */

put_async_queues:
	bfq_put_async_queues(bfqd, bfqg); /* async queues 해제: NVMe writeback/flush 관련 큐 자원 정리 */

	spin_unlock_irqrestore(&bfqd->lock, flags);
	/*
	 * @blkg is going offline and will be ignored by
	 * blkg_[rw]stat_recursive_sum().  Transfer stats to the parent so
	 * that they don't get lost.  If IOs complete after this point, the
	 * stats for them will be lost.  Oh well...
	 */
	bfqg_stats_xfer_dead(bfqg); /* statsXferDead는 in-flight NVMe 명령의 완료 통계가 일부 손실될 수 있음을 감수하고 부모로 이관 */
}

/*
 * bfq_end_wr_async - writeback 종료 시 모든 비동기 큐의 WR 상태 해제
 * @bfqd: bfq_data
 *
 * NVMe 관점: 비동기 writeback 큐들은 NVMe SQ에 flush/fua 없이 모아서 제출되는
 * 경향이 있다. cgroup별로 writeback 종료 시점을 처리하여, 각 bfq_group의
 * 비동기 큐가 더 이상 WR(writeback-related) 상태를 갖지 않도록 한다.
 */
void bfq_end_wr_async(struct bfq_data *bfqd)
{
	struct blkcg_gq *blkg;

	list_for_each_entry(blkg, &bfqd->queue->blkg_list, q_node) { /* blkg_list 순회는 해당 NVMe namespace(request_queue)에 연결된 모든 cgroup */
		struct bfq_group *bfqg = blkg_to_bfqg(blkg); /* 각 bfq_group의 async 큐 WR 상태 해제 */

		bfq_end_wr_async_queues(bfqd, bfqg); /* async 큐 WR 해제는 writeback 종료 후 NVMe flush/fua 패턴 변경 */
	}
	bfq_end_wr_async_queues(bfqd, bfqd->root_group); /* root_group의 async 큐도 동일하게 처리 */
}

static int bfq_io_show_weight_legacy(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	unsigned int val = 0;

	if (bfqgd) /* weight 값 읽기: NVMe SQ 서비스 비율의 cgroup 기본값 */
		val = bfqgd->weight;

	seq_printf(sf, "%u\n", val);

	return 0;
}

static u64 bfqg_prfill_weight_device(struct seq_file *sf,
				     struct blkg_policy_data *pd, int off)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);

	if (!bfqg->entity.dev_weight) /* per-device weight가 0이면 default weight 사용, NVMe namespace별 정책 */
		return 0;
	return __blkg_prfill_u64(sf, pd, bfqg->entity.dev_weight); /* per-device weight 출력은 NVMe namespace마다 다른 cgroup 대역폭을 보여줌 */
}

static int bfq_io_show_weight(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);

	seq_printf(sf, "default %u\n", bfqgd->weight); /* default weight 출력: 모든 NVMe namespace의 기본 cgroup 정책 */
	blkcg_print_blkgs(sf, blkcg, bfqg_prfill_weight_device, /* blkcg_print_blkgs는 모든 blkg(NVMe namespace)의 weight를 순회 출력 */
			  &blkcg_policy_bfq, 0, false);
	return 0;
}

/*
 * bfq_group_set_weight - bfq_group의 I/O 가중치 설정
 * @bfqg: 대상 bfq_group
 * @weight: 상속받을 기본 weight
 * @dev_weight: per-device weight (0이면 @weight 사용)
 *
 * NVMe 관점: 이 가중치는 WFQ 가상 시간 계산에 직접 반영되어, NVMe SQ에
 * 제출될 명령의 상대적 서비스 비율을 조정한다. weight가 클수록 동일
 * 경쟁 조건에서 더 자주 doorbell을 발행할 기회를 얻는다.
 */
static void bfq_group_set_weight(struct bfq_group *bfqg, u64 weight, u64 dev_weight)
{
	weight = dev_weight ?: weight; /* dev_weight 우선 적용: 동일 blkcg라도 NVMe namespace마다 다른 weight 가능 */

	bfqg->entity.dev_weight = dev_weight; /* dev_weight 저장: per-device NVMe 정책 상태 유지 */
	/*
	 * Setting the prio_changed flag of the entity
	 * to 1 with new_weight == weight would re-set
	 * the value of the weight to its ioprio mapping.
	 * Set the flag only if necessary.
	 */
	if ((unsigned short)weight != bfqg->entity.new_weight) { /* new_weight 갱신은 WFQ 가상 시간 재계산, NVMe doorbell 발행 빈도 변화 */
		bfqg->entity.new_weight = (unsigned short)weight; /* (unsigned short) 캐스팅은 WFQ weight 자료형 범위 내 제한 */
		/*
		 * Make sure that the above new value has been
		 * stored in bfqg->entity.new_weight before
		 * setting the prio_changed flag. In fact,
		 * this flag may be read asynchronously (in
		 * critical sections protected by a different
		 * lock than that held here), and finding this
		 * flag set may cause the execution of the code
		 * for updating parameters whose value may
		 * depend also on bfqg->entity.new_weight (in
		 * __bfq_entity_update_weight_prio).
		 * This barrier makes sure that the new value
		 * of bfqg->entity.new_weight is correctly
		 * seen in that code.
		 */
		smp_wmb(); /* smp_wmb: new_weight 저장이 prio_changed 플래그 읽기보다 먼저 NVMe dispatch 경로에 보이도록 보장 */
		bfqg->entity.prio_changed = 1; /* prio_changed 플래그는 __bfq_entity_update_weight_prio에서 WFQ 파라미터 재계산 트리거 */
	}
}

/*
 * bfq_io_set_weight_legacy - 레거시 cgroup 인터페이스를 통한 weight 설정
 * @css: cgroup_subsys_state
 * @cftype: cgroup 파일 타입
 * @val: 설정할 weight 값
 *
 * NVMe 관점: 사용자가 /sys/fs/cgroup/.../bfq.weight를 쓰면 이 함수가 호출되어
 * 해당 blkcg에 속한 모든 gendisk의 bfq_group weight가 갱신된다. 이 값은
 * 이후 NVMe I/O 스케줄링 우선순위에 즉시 반영된다.
 */
static int bfq_io_set_weight_legacy(struct cgroup_subsys_state *css,
				    struct cftype *cftype,
				    u64 val)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	struct blkcg_gq *blkg;
	int ret = -ERANGE;

	if (val < BFQ_MIN_WEIGHT || val > BFQ_MAX_WEIGHT) /* weight 범위 검사, 극단적 weight는 NVMe SQ 서비스 비율 왜곡 방지 */
		return ret;

	ret = 0;
	spin_lock_irq(&blkcg->lock); /* blkcg->lock은 동일 cgroup의 여러 NVMe namespace에 대한 weight 변경 동기화 */
	bfqgd->weight = (unsigned short)val;
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) { /* blkg_list 순회로 모든 gendisk(NVMe namespace)의 bfq_group weight 갱신 */
		struct bfq_group *bfqg = blkg_to_bfqg(blkg);

		if (bfqg)
			bfq_group_set_weight(bfqg, val, 0); /* bfq_group_set_weight는 WFQ 가상 시간과 NVMe doorbell 우선순위 갱신 */
	}
	spin_unlock_irq(&blkcg->lock);

	return ret;
}

/*
 * bfq_io_set_device_weight - per-device weight 설정
 * @of: kernfs_open_file
 * @buf: 입력 버퍼
 * @nbytes: 입력 크기
 * @off: 오프셋
 *
 * NVMe 관점: 동일 blkcg라도 NVMe 디바이스마다 다른 weight를 부여할 수 있다.
 * 이를 통해 하나의 cgroup이 여러 NVMe namespace에 대해 차등화된 대역폭을
 * 가질 수 있다.
 */
static ssize_t bfq_io_set_device_weight(struct kernfs_open_file *of,
					char *buf, size_t nbytes,
					loff_t off)
{
	int ret;
	struct blkg_conf_ctx ctx;
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct bfq_group *bfqg;
	u64 v;

	blkg_conf_init(&ctx, buf);

	ret = blkg_conf_prep(blkcg, &blkcg_policy_bfq, &ctx); /* blkg_conf_prep는 대상 NVMe namespace(gendisk)와 blkg 식별 */
	if (ret) /* 대상 blkg 준비 실패 시 NVMe weight 설정 중단 */
		goto out;

	if (sscanf(ctx.body, "%llu", &v) == 1) {
		/* require "default" on dfl */
		ret = -ERANGE;
		if (!v) /* v==0은 default weight 복귀, NVMe namespace별 정책 제거 */
			goto out;
	} else if (!strcmp(strim(ctx.body), "default")) {
		v = 0; /* "default" 입력 시 per-device weight 초기화, NVMe namespace 기본 정책 사용 */
	} else {
		ret = -EINVAL;
		goto out;
	}

	bfqg = blkg_to_bfqg(ctx.blkg); /* ctx.blkg에서 bfq_group 획득 */

	ret = -ERANGE;
	if (!v || (v >= BFQ_MIN_WEIGHT && v <= BFQ_MAX_WEIGHT)) { /* weight 범위 검사 후 NVMe SQ 서비스 비율 갱신 */
		bfq_group_set_weight(bfqg, bfqg->entity.weight, v); /* bfqg->entity.weight는 현재 WFQ 가중치, v는 per-device 새 weight */
		ret = 0;
	}
out:
	blkg_conf_exit(&ctx);
	return ret ?: nbytes;
}

static ssize_t bfq_io_set_weight(struct kernfs_open_file *of,
				 char *buf, size_t nbytes,
				 loff_t off)
{
	char *endp;
	int ret;
	u64 v;

	buf = strim(buf);

	/* "WEIGHT" or "default WEIGHT" sets the default weight */
	v = simple_strtoull(buf, &endp, 0); /* "WEIGHT" 또는 "default WEIGHT" 형식 파싱 */
	if (*endp == '\0' || sscanf(buf, "default %llu", &v) == 1) { /* simple_strtoull로 숫자 weight 해석, 실패 시 per-device 경로로 폴팩 */
		ret = bfq_io_set_weight_legacy(of_css(of), NULL, v); /* 숫자 형식이면 레거시 경로로 NVMe cgroup weight 설정 */
		return ret ?: nbytes;
	}

	return bfq_io_set_device_weight(of, buf, nbytes, off);
}

static int bfqg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_rwstat, /* blkcg_print_blkgs는 NVMe namespace별 cgroup 통계를 순회 출력 */
			  &blkcg_policy_bfq, seq_cft(sf)->private, true); /* seq_cft->private는 stats 구조체 내 오프셋, NVMe 처리량/지연 지표 식별 */
	return 0;
}

static u64 bfqg_prfill_rwstat_recursive(struct seq_file *sf,
					struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample sum;

	blkg_rwstat_recursive_sum(pd_to_blkg(pd), &blkcg_policy_bfq, off, &sum); /* blkg_rwstat_recursive_sum은 하위 cgroup 포함 NVMe 처리량 합산 */
	return __blkg_prfill_rwstat(sf, pd, &sum); /* 재귀 합산 결과를 사용자 공간에 노출, NVMe namespace 전체 cgroup 분석 */
}

static int bfqg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_rwstat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, true);
	return 0;
}

#ifdef CONFIG_BFQ_CGROUP_DEBUG
static int bfqg_print_stat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_stat, /* CONFIG_BFQ_CGROUP_DEBUG가 켜진 경우에만 NVMe 세부 지연 통계 출력 */
			  &blkcg_policy_bfq, seq_cft(sf)->private, false); /* seq_cft->private로 stats 구조체 내 특정 NVMe 지연 지표 선택 */
	return 0;
}

static u64 bfqg_prfill_stat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	struct blkcg_gq *pos_blkg;
	struct cgroup_subsys_state *pos_css;
	u64 sum = 0;

	lockdep_assert_held(&blkg->q->queue_lock); /* queue_lock 보유 중에 rcu_read_lock, NVMe 완료 콜백과의 일관성 확보 */

	rcu_read_lock(); /* rcu_read_lock 내에서 descendant 순회, NVMe 완료 시 blkg 안전 참조 */
	blkg_for_each_descendant_pre(pos_blkg, pos_css, blkg) { /* blkg_for_each_descendant_pre는 cgroup 계층 하향 순회 */
		struct bfq_stat *stat;

		if (!pos_blkg->online) /* online 아닌 blkg는 NVMe I/O가 더 이상 발생하지 않으므로 skip */
			continue;

		stat = (void *)blkg_to_pd(pos_blkg, &blkcg_policy_bfq) + off; /* policy_data에서 대상 bfq_stat 포인터 계산 */
		sum += bfq_stat_read(stat) + atomic64_read(&stat->aux_cnt); /* percpu_counter 값과 aux_cnt를 합산, NVMe 완료 이력 누적 */
	}
	rcu_read_unlock();

	return __blkg_prfill_u64(sf, pd, sum); /* 합산된 NVMe 통계를 seq_file에 기록 */
}

static int bfqg_print_stat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_stat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, false);
	return 0;
}

static u64 bfqg_prfill_sectors(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct bfq_group *bfqg = blkg_to_bfqg(pd->blkg); /* bfq_group의 bytes 통계에서 NVMe 처리 바이트 합산 */
	u64 sum = blkg_rwstat_total(&bfqg->stats.bytes); /* blkg_rwstat_total은 읽기/쓰기 합계, NVMe SQ read/write 명령 모두 포함 */

	return __blkg_prfill_u64(sf, pd, sum >> 9); /* 섹터 수로 변환(>>9)하여 NVMe 512B 섹터 단위 처리량 노출 */
}

static int bfqg_print_stat_sectors(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors, &blkcg_policy_bfq, 0, false);
	return 0;
}

static u64 bfqg_prfill_sectors_recursive(struct seq_file *sf,
					 struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample tmp;

	blkg_rwstat_recursive_sum(pd->blkg, &blkcg_policy_bfq, /* bytes 필드 재귀 합산: NVMe namespace 내 모든 cgroup 처리량 */
			offsetof(struct bfq_group, stats.bytes), &tmp); /* offsetof로 stats.bytes 위치 지정 */

	return __blkg_prfill_u64(sf, pd, /* 읽기/쓰기 합계를 섹터 수로 변환 */
		(tmp.cnt[BLKG_RWSTAT_READ] + tmp.cnt[BLKG_RWSTAT_WRITE]) >> 9); /* NVMe namespace별 cgroup 계층 전체 처리량 출력 */
}

static int bfqg_print_stat_sectors_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors_recursive, &blkcg_policy_bfq, 0,
			  false);
	return 0;
}

static u64 bfqg_prfill_avg_queue_size(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd); /* bfq_group 획득 */
	u64 samples = bfq_stat_read(&bfqg->stats.avg_queue_size_samples); /* avg_queue_size_samples는 NVMe backlog 평균 분모 */
	u64 v = 0;

	if (samples) {
		v = bfq_stat_read(&bfqg->stats.avg_queue_size_sum); /* avg_queue_size_sum에서 NVMe 제출 대기열 길이 누적값 읽기 */
		v = div64_u64(v, samples); /* div64_u64로 64비트 나눗셈, NVMe backlog 평균 산출 */
	}
	__blkg_prfill_u64(sf, pd, v); /* 평균 queue_size 출력, NVMe SQ 혼잡도 추정 지표 */
	return 0;
}

/* print avg_queue_size */
static int bfqg_print_avg_queue_size(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), /* blkcg_print_blkgs로 모든 blkg의 평균 queue_size 출력 */
			  bfqg_prfill_avg_queue_size, &blkcg_policy_bfq,
			  0, false);
	return 0;
}
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

/*
 * bfq_create_group_hierarchy - blk-cgroup 정책을 활성화하고 root bfq_group 반환
 * @bfqd: bfq_data
 * @node: 메모리 노드
 *
 * NVMe 관점: elevator 초기화 시 디스크에 대해 blkcg_policy_bfq를 활성화한다.
 * 이로써 이후 bio 제출 시 bio->bi_blkg에서 bfq_group을 찾을 수 있고,
 * NVMe I/O가 cgroup 정책을 따르게 된다.
 */
struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	int ret;

	ret = blkcg_activate_policy(bfqd->queue->disk, &blkcg_policy_bfq); /* blkcg_activate_policy는 elevator 초기화 시 NVMe namespace에 blkcg 정책 연결 */
	if (ret) /* 정책 활성화 실패 시 bfq_group 계층 생성 불가, NVMe I/O는 root_group 정책 적용 */
		return NULL;

	return blkg_to_bfqg(bfqd->queue->root_blkg); /* root_blkg의 bfq_group 반환, NVMe I/O의 최상위 cgroup 단위 준비 */
}

/*
 * struct blkcg_policy blkcg_policy_bfq - BFQ의 blk-cgroup 정책 등록 구조체
 *
 * NVMe 관점: blk-cgroup은 NVMe SQ/CQ와 직접 매핑되지 않는다. 이 정책은
 * cgroup 생성/초기화/오프라인/해제 시점에 bfq_group을 관리하여,
 * NVMe 명령이 제출되기 전에 올바른 cgroup 단위 스케줄링 단위를
 * 준비하도록 한다. cpd_alloc_fn/pd_alloc_fn은 cgroup 생성 시,
 * pd_offline_fn/pd_free_fn은 cgroup 삭제 시 호출된다.
 */
struct blkcg_policy blkcg_policy_bfq = { /* blkcg_policy_bfq 등록: NVMe I/O의 cgroup 단위 스케줄링 단위 준비 */
	.dfl_cftypes		= bfq_blkg_files, /* dfl_cftypes: cgroup v2 인터페이스를 통한 NVMe weight 제어 */
	.legacy_cftypes		= bfq_blkcg_legacy_files, /* legacy_cftypes: cgroup v1 인터페이스를 통한 NVMe weight/통계 제어 */

	.cpd_alloc_fn		= bfq_cpd_alloc, /* cgroup 생성 시 bfq_group_data 할당, NVMe weight 기본값 설정 */
	.cpd_free_fn		= bfq_cpd_free, /* cgroup 해제 시 bfq_group_data 정리 */

	.pd_alloc_fn		= bfq_pd_alloc, /* gendisk(NVMe namespace)에 bfq_group 할당 */
	.pd_init_fn		= bfq_pd_init, /* bfq_group 스케줄링 필드 초기화, NVMe WFQ 트리 참여 준비 */
	.pd_offline_fn		= bfq_pd_offline, /* cgroup 오프라인 시 bfq_group 비활성화 및 NVMe 완료 통계 이관 */
	.pd_free_fn		= bfq_pd_free, /* bfq_group 메모리 해제, in-flight NVMe 명령 참조 종료 후 호출 */
	.pd_reset_stats_fn	= bfq_pd_reset_stats, /* 통계 리셋 콜백, NVMe 이력 초기화 */
};

struct cftype bfq_blkcg_legacy_files[] = {
	{
		.name = "bfq.weight", /* cgroup v1 bfq.weight: NVMe SQ 서비스 비율 기본값 설정 */
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight_legacy,
		.write_u64 = bfq_io_set_weight_legacy, /* 레거시 단일 weight 쓰기: NVMe doorbell 우선순위 변경 */
	},
	{
		.name = "bfq.weight_device", /* cgroup v1 bfq.weight_device: NVMe namespace별 weight 설정 */
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		.write = bfq_io_set_weight, /* per-device weight 쓰기: 동일 blkcg의 NVMe namespace마다 차등 대역폭 */
	},

	/* statistics, covers only the tasks in the bfqg */
	{
		.name = "bfq.io_service_bytes", /* io_service_bytes: NVMe namespace별 cgroup 바이트 처리량 */
		.private = offsetof(struct bfq_group, stats.bytes),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_serviced", /* io_serviced: NVMe namespace별 cgroup 명령 완료 횟수 */
		.private = offsetof(struct bfq_group, stats.ios),
		.seq_show = bfqg_print_rwstat,
	},
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	{
		.name = "bfq.time", /* bfq.time: NVMe I/O 총 시간 (debug) */
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.sectors", /* bfq.sectors: NVMe 처리 섹터 수 (debug) */
		.seq_show = bfqg_print_stat_sectors,
	},
	{
		.name = "bfq.io_service_time", /* bfq.io_service_time: NVMe 디바이스 처리 시간 (debug) */
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_wait_time", /* bfq.io_wait_time: NVMe SQ/CQ 슬롯 대기 시간 (debug) */
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_merged", /* bfq.io_merged: NVMe DMA segment 합병 횟수 (debug) */
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_queued", /* bfq.io_queued: NVMe 제출 대기열 길이 (debug) */
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat,
	},
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

	/* the same statistics which cover the bfqg and its descendants */
	{
		.name = "bfq.io_service_bytes_recursive", /* io_service_bytes_recursive: NVMe namespace 내 하위 cgroup 포함 바이트 처리량 */
		.private = offsetof(struct bfq_group, stats.bytes),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_serviced_recursive", /* io_serviced_recursive: NVMe namespace 내 하위 cgroup 포함 명령 수 */
		.private = offsetof(struct bfq_group, stats.ios),
		.seq_show = bfqg_print_rwstat_recursive,
	},
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	{
		.name = "bfq.time_recursive", /* bfq.time_recursive: NVMe I/O 총 시간 재귀 (debug) */
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat_recursive,
	},
	{
		.name = "bfq.sectors_recursive", /* bfq.sectors_recursive: NVMe 처리 섹터 수 재귀 (debug) */
		.seq_show = bfqg_print_stat_sectors_recursive,
	},
	{
		.name = "bfq.io_service_time_recursive", /* bfq.io_service_time_recursive: NVMe 디바이스 처리 시간 재귀 (debug) */
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{ /* bfq.io_wait_time_recursive: NVMe SQ/CQ 대기 시간 재귀 (debug) */
		.name = "bfq.io_wait_time_recursive",
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{ /* bfq.io_merged_recursive: NVMe DMA segment 합병 횟수 재귀 (debug) */
		.name = "bfq.io_merged_recursive",
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{ /* bfq.io_queued_recursive: NVMe 제출 대기열 길이 재귀 (debug) */
		.name = "bfq.io_queued_recursive",
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.avg_queue_size", /* bfq.avg_queue_size: NVMe SQ backlog 평균 (debug) */
		.seq_show = bfqg_print_avg_queue_size,
	},
	{
		.name = "bfq.group_wait_time",
		.private = offsetof(struct bfq_group, stats.group_wait_time), /* bfq.group_wait_time: NVMe doorbell 지연 시간 (debug) */
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.idle_time", /* bfq.idle_time: NVMe doorbell 중단 시간 (debug) */
		.private = offsetof(struct bfq_group, stats.idle_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.empty_time", /* bfq.empty_time: NVMe SQ/CQ idle 시간 (debug) */
		.private = offsetof(struct bfq_group, stats.empty_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.dequeue", /* bfq.dequeue: NVMe SQ 가득 참으로 인한 dequeue 횟수 (debug) */
		.private = offsetof(struct bfq_group, stats.dequeue),
		.seq_show = bfqg_print_stat,
	},
#endif	/* CONFIG_BFQ_CGROUP_DEBUG */
	{ }	/* terminate */
};

struct cftype bfq_blkg_files[] = {
	{
		.name = "bfq.weight", /* cgroup v2 bfq.weight: NVMe SQ 서비스 비율 기본값 */
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		.write = bfq_io_set_weight, /* v2 weight 쓰기: NVMe doorbell 우선순위 변경 */
	},
	{} /* terminate */
};

#else	/* CONFIG_BFQ_GROUP_IOSCHED */

void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq, /* CONFIG_BFQ_GROUP_IOSCHED 미정의 시 bfq_bfqq_move는 no-op, NVMe I/O는 root_group에서만 스케줄링 */
		   struct bfq_group *bfqg) {}

void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	entity->weight = entity->new_weight; /* weight 초기화: WFQ에 반영되어 NVMe doorbell 빈도 결정 */
	entity->orig_weight = entity->new_weight;
	if (bfqq) {
		bfqq->ioprio = bfqq->new_ioprio;
		bfqq->ioprio_class = bfqq->new_ioprio_class;
	}
	entity->sched_data = &bfqg->sched_data; /* root_group의 sched_data에 연결, NVMe I/O 기본 정책 적용 */
}

void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio) {}

void bfq_end_wr_async(struct bfq_data *bfqd)
{
	bfq_end_wr_async_queues(bfqd, bfqd->root_group);
}

struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio)
{ /* cgroup 지원 없을 때 모든 bio를 root_group으로 라우팅, NVMe SQ 기본 weight 적용 */
	return bfqd->root_group;
}

struct bfq_group *bfqq_group(struct bfq_queue *bfqq) /* bfqq가 속한 그룹은 항상 root_group, NVMe I/O에 계층적 WFQ 미적용 */
{
	return bfqq->bfqd->root_group;
}

void bfqg_and_blkg_put(struct bfq_group *bfqg) {} /* cgroup 지원 없을 때 bfqg/blkg put은 no-op, NVMe 완료 콜백에서 추가 레퍼런스 관리 불필요 */

struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	struct bfq_group *bfqg;
	int i;

	bfqg = kmalloc_node(sizeof(*bfqg), GFP_KERNEL | __GFP_ZERO, node); /* kmalloc_node로 root_group 할당, NVMe NUMA 노드 선호 (추정) */
	if (!bfqg)
		return NULL;

	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++) /* BFQ_IOPRIO_CLASSES 루프: RT/BE/IDLE 서비스 트리 초기화 */
		bfqg->sched_data.service_tree[i] = BFQ_SERVICE_TREE_INIT; /* service_tree 초기화: NVMe SQ 우선순위 트리 준비 */

	return bfqg; /* 생성된 root_group 반환 */
}
#endif	/* CONFIG_BFQ_GROUP_IOSCHED */

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk-cgroup과 BFQ를 연결하는 접착 코드로, bio가 NVMe SQ/CQ로
 *   전달되기 전에 어떤 cgroup의 bfq_queue로 라우팅될지를 결정한다.
 * - bfq_group->entity.weight는 WFQ 기반 가상 시간 계산에 사용되어,
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 경로에서 NVMe 명령의 상대적 우선순위를 제어한다.
 * - cgroup 생성/오프라인/삭제 시 bfq_group의 생명주기와 active/idle tree를
 *   관리하며, in-flight NVMe 명령에 대한 안전한 통계/레퍼런스 처리를 보장한다.
 * - CONFIG_BFQ_CGROUP_DEBUG가 켜진 경우 wait_time/service_time/queued 등을
 *   cgroup 단위로 추적하여, NVMe SQ 가득 참이나 CID 부족에 따른 지연을
 *   분석할 수 있다 (추정).
 * - 논리적으로 block/bfq-iosched.c, block/bfq-wf2q.c와 긴밀하게 연결되며,
 *   block/blk-cgroup.c의 정책 콜백을 통해 cgroup 이벤트를 수신한다.
 */
