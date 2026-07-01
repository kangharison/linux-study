/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe SSD 관점 파일 요약:
 *
 * 이 파일은 block cgroup(blkcg)의 낮은 수준(private) 헤더로, request_queue 단위로
 * cgroup별 I/O 자원을 분리/제어하기 위한 구조체와 정책 등록 인터페이스를 정의합니다.
 * NVMe 입장에서는 bio -> request 변환 및 NVMe SQ/CQ 발행 직전에 위치한
 * blk-mq 스케줄링 경로와 연결되며, blk-throttle, BFQ, mq-deadline 같은 정책이
 * NVMe I/O 흐름을 제한하거나 지연시키는 기반 데이터 구조를 제공합니다.
 * 상위 인터페이스는 include/linux/blk-cgroup.h에 있고, 구현/사용은
 * block/blk-cgroup.c, block/blk-mq.c, block/blk-throttle.c, block/bfq-iosched.c 등과
 * 긴장 관계를 이룹니다.
 */
#ifndef _BLK_CGROUP_PRIVATE_H
#define _BLK_CGROUP_PRIVATE_H
/*
 * block cgroup private header
 *
 * Based on ideas and code from CFQ, CFS and BFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2009 Vivek Goyal <vgoyal@redhat.com>
 * 	              Nauman Rafique <nauman@google.com>
 */

#include <linux/blk-cgroup.h> /* blkcg 정책/헬퍼 인라인 함수 포함 -> bio->request 변환 시 cgroup 결정 */
#include <linux/cgroup.h>     /* cgroup_subsys_state, css_for_each_* 등 cgroup 계층 순회용 */
#include <linux/kthread.h>    /* kthread 관련 (blkcg punt bio work 처리에 간접 사용) */
#include <linux/blk-mq.h>     /* blk-mq hctx/tags/request 구조체 -> NVMe multi-queue 매핑에 직접 연결 */
#include <linux/llist.h>      /* lockless list -> percpu iostat flush 경로에서 cache-friendly 연결 */
#include "blk.h"              /* request_queue, QUEUE_FLAG_* 등 queue 상태/깃발 정의 */

/*
 * 이 헤더는 include/linux/blk-cgroup.h 의 낮은 수준 보조 헤더입니다.
 * block/blk-mq.c, block/blk.h, block/elevator.c, block/blk-throttle.c,
 * block/bfq-iosched.c 등과 함께 동작하며, NVMe I/O가 blk_mq_submit_bio()를
 * 거쳐 request_queue -> blk_mq_hw_ctx -> nvme_queue_rq()로 흐를 때 cgroup
 * 소속/지연/합병 정보를 참조합니다.
 */

struct blkcg_gq;        /* cgroup <-> request_queue 연결 노드 (NVMe namespace 단위) */
struct blkg_policy_data; /* blk-throttle/BFQ 등 policy별 private data 헤더 */


/* percpu_counter batch for blkg_[rw]stats, per-cpu drift doesn't matter */
#define BLKG_STAT_CPU_BATCH	(INT_MAX / 2) /* percpu counter 배치 크기: NVMe 완료 횟수/바이트 갱신 시 cache-line 경합 완화 */

#ifdef CONFIG_BLK_CGROUP

/*
 * NVMe 명령 유형별 통계를 구분하기 위한 열거형입니다.
 * READ/WRITE는 일반적인 NVMe Read/Write command에, DISCARD는
 * Dataset Management/Deallocate/Trim 에 대응됩니다.
 */
enum blkg_iostat_type {
	BLKG_IOSTAT_READ,	/* NVMe Read command 통계 */
	BLKG_IOSTAT_WRITE,	/* NVMe Write command 통계 */
	BLKG_IOSTAT_DISCARD,	/* NVMe Dataset Management/Trim 통계 */

	BLKG_IOSTAT_NR,	/* 통계 항목 개수 -> 배열 인덱스로도 사용 */
};

/*
 * NVMe I/O 완료 시 누적할 바이트 수와 완료 횟수(ios)를 저장합니다.
 * nvme_complete_rq() 경로에서 blk-cgroup이 I/O를 회계할 때 사용됩니다.
 */
struct blkg_iostat {
	u64				bytes[BLKG_IOSTAT_NR];	/* 명령 종류별 전송/완료 바이트 -> NVMe SQ entry의 length와 대응 */
	u64				ios[BLKG_IOSTAT_NR];	/* 명령 종류별 완료 횟수 -> CQ entry 처리 횟수 누적 */
};

/*
 * NVMe I/O가 완료될 때 percpu 단위로 먼저 갱신하고, 일정 주기마다
 * blkcg 전체의 iostat로 flush 합니다. 이를 통해 NVMe interrupt/completion
 * 경로에서 request_queue->queue_lock 경합을 줄입니다.
 */
struct blkg_iostat_set {
	struct u64_stats_sync		sync;	/* percpu 통계 갱신과 read 간 동기화 -> NVMe ISR에서 읽을 때 정확한 값 보장 */
	struct blkcg_gq		       *blkg;	/* 이 통계 집합이 속한 blkcg_gq -> NVMe request_queue 연결 */
	struct llist_node		lnode;	/* lhead 연결 리스트 노드 */
	int				lqueued;	/* queued in llist -> percpu 측정값이 아직 flush되지 않았음을 표시 */
	struct blkg_iostat		cur;	/* 현재 누적된 NVMe I/O 통계 */
	struct blkg_iostat		last;	/* 마지막 flush 시점의 스냅샷 -> cgroup 파일 읽기 시 비교/차분용 */
};

/* association between a blk cgroup and a request queue */
/*
 * struct blkcg_gq 는 특정 cgroup과 특정 request_queue(q) 사이의 연결점입니다.
 * NVMe SSD에서는 하나의 request_queue가 보통 하나의 nvme namespace를 나타낼 수
 * 있으며, blkcg_gq 는 "이 cgroup이 이 NVMe namespace로 본 I/O"를 추적합니다.
 */
struct blkcg_gq {
	/* Pointer to the associated request_queue */
	struct request_queue		*q;	/* 연결된 NVMe request_queue -> NVMe namespace의 mq submit entry point */
	struct list_head		q_node;	/* request_queue의 blkg 리스트 연결 -> queue 제거/해제 시 순회 */
	struct hlist_node		blkcg_node;	/* blkcg->blkg_list 연결 -> cgroup 제거 시 역참조용 */
	struct blkcg			*blkcg;	/* 상위 cgroup -> 계층적 throttle/IO weight 적용 시 거슬러 올라감 */

	/* all non-root blkcg_gq's are guaranteed to have access to parent */
	struct blkcg_gq			*parent;	/* 계층적 throttling/통계 집계를 위한 부모 blkg -> blk-throttle tree 상속 경로 */

	/* reference count */
	struct percpu_ref		refcnt;	/* blkg 생명 주기 관리 -> request 완료 시 blkg_put으로 감소, 0이면 free work */

	/* is this blkg online? protected by both blkcg and q locks */
	bool				online;	/* NVMe I/O 처리 가능 여부 -> dying queue에서 false로 전환되어 SQ/CQ 새 명령 차단 */

	struct blkg_iostat_set __percpu	*iostat_cpu;	/* NVMe I/O percpu 통계 -> 각 CPU별 CQ ISR에서 lockless 갱신 */
	struct blkg_iostat_set		iostat;	/* NVMe I/O 집계 통계 -> percpu 값을 합산한 공유 스냅샷 */

	struct blkg_policy_data		*pd[BLKCG_MAX_POLS];	/* blk-throttle, BFQ 등 정책별 사설 데이터 -> request 허용/지연/우선순위 판단 */
#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	spinlock_t			async_bio_lock;	/* async_bios 보호 -> cgroup 제약으로 bio punt 시 hctx 외부에서 안전하게 추가 */
	struct bio_list			async_bios;	/* cgroup 제약으로 보류된 bio들 (NVMe 발행 지연) -> throttled bio의 임시 대기열 */
#endif
	union {
		struct work_struct	async_bio_work;	/* 보류 bio를 비동기로 처리하는 work -> kworker에서 bio를 다시 submit_bio로 재진입 */
		struct work_struct	free_work;	/* blkg 해제 work -> refcnt 0이 되면 RCU grace period 후 메모리 해제 */
	};

	atomic_t			use_delay;	/* 현재 지연 적용 중인 카운트 -> 0이 아니면 NVMe SQ doorbell 지연/backpressure */
	atomic64_t			delay_nsec;	/* 추가 지연 시간(nsec) -> request 할당/발행 전 sleep 지속 시간 */
	atomic64_t			delay_start;	/* 지연 시작 시점 -> delta 누적 시 now와의 차이로 decay 계산 */
	u64				last_delay;	/* 마지막으로 적용한 지연량 -> throttle 정책이 다음 지연량을 exponential하게 계산 */
	int				last_use;	/* 마지막 사용 시점 기록용 -> aging/idle 판단에 사용 */

	struct rcu_head			rcu_head;	/* RCU 해제용 -> blkg free_work에서 call_rcu()로 안전한 메모리 반납 */
};

/*
 * struct blkcg 는 하나의 block cgroup을 표현합니다.
 * NVMe 관점에서는 여러 NVMe namespace(request_queue)에 걸친 I/O를 한 cgroup으로
 * 묶어 제한/계층화할 때 사용됩니다.
 */
struct blkcg {
	struct cgroup_subsys_state	css;	/* cgroup subsys 상태 -> cgroupfs에서 이 cgroup의 위치 */
	spinlock_t			lock;	/* blkcg 난 수준 락 -> blkg_list/blkg_tree 변경 시 보호 */
	refcount_t			online_pin;	/* 온라인 상태 고정 -> queue 제거 전 모든 I/O 완료까지 유지 */
	/* If there is block congestion on this cgroup. */
	atomic_t			congestion_count;	/* NVMe queue 혼잡/지연 표시자 (throttling 백프레셔) -> 0이 아니면 request 발행 제한 */

	struct radix_tree_root		blkg_tree;	/* request_queue id -> blkcg_gq 매핑 -> submit_bio 시 빠른 lookup */
	struct blkcg_gq	__rcu		*blkg_hint;	/* 최근 조회한 blkg 캐시 (빠른 NVMe 경로) -> RCU read lock 하에서 접근 */
	struct hlist_head		blkg_list;	/* 이 cgroup의 모든 blkg 목록 -> cgroup 제거/정책 순회 시 사용 */

	struct blkcg_policy_data	*cpd[BLKCG_MAX_POLS];	/* 정책별 per-cgroup 데이터 -> blk-throttle IO weight, BFQ group 등 */

	struct list_head		all_blkcgs_node;	/* 전체 blkcg 연결 리스트 -> blkcg_policy_register/종료 시 전체 순회 */

	/*
	 * List of updated percpu blkg_iostat_set's since the last flush.
	 */
	struct llist_head __percpu	*lhead;	/* NVMe I/O 완료로 갱신된 percpu 통계의 머리 -> lockless llist_add 사용 */

#ifdef CONFIG_BLK_CGROUP_FC_APPID
	char                            fc_app_id[FC_APPID_LEN];	/* FC app id (NVMe-oF 관련, 추정) -> FC-NVMe 타겟/호스트 식별자 */
#endif
#ifdef CONFIG_CGROUP_WRITEBACK
	struct list_head		cgwb_list;	/* cgroup writeback 연결 -> f2fs/ext4 flush thread가 NVMe write를 배치할 때 사용 */
#endif
};

static inline struct blkcg *css_to_blkcg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct blkcg, css) : NULL;
}

/*
 * A blkcg_gq (blkg) is association between a block cgroup (blkcg) and a
 * request_queue (q).  This is used by blkcg policies which need to track
 * information per blkcg - q pair.
 *
 * There can be multiple active blkcg policies and each blkg:policy pair is
 * represented by a blkg_policy_data which is allocated and freed by each
 * policy's pd_alloc/free_fn() methods.  A policy can allocate private data
 * area by allocating larger data structure which embeds blkg_policy_data
 * at the beginning.
 */
/*
 * blkg_policy_data 는 blk-throttle, BFQ, mq-deadline 같은 정책이
 * "cgroup + NVMe request_queue" 단위로 상태를 저장하기 위한 헤더입니다.
 * NVMe I/O가 blk_mq_get_request() -> 정책 판단 -> nvme_queue_rq()로 전달될 때
 * 이 구조체를 통해 cgroup별 한도/우선순위가 반영됩니다.
 */
struct blkg_policy_data {
	/* the blkg and policy id this per-policy data belongs to */
	struct blkcg_gq			*blkg;	/* 소속 blkcg_gq (NVMe request_queue 연결) -> policy가 queue depth/congestion 조회 */
	int				plid;	/* 정책 ID (e.g. blk-throttle) -> q->blkcg_pols bit index와 일치 */
	bool				online;	/* 정책 데이터 활성화 여부 -> false면 NVMe 경로에서 정책 검사 skip */
};

/*
 * Policies that need to keep per-blkcg data which is independent from any
 * request_queue associated to it should implement cpd_alloc/free_fn()
 * methods.  A policy can allocate private data area by allocating larger
 * data structure which embeds blkcg_policy_data at the beginning.
 * cpd_init() is invoked to let each policy handle per-blkcg data.
 */
/*
 * blkcg_policy_data 는 특정 NVMe request_queue와 무관하게 cgroup 전체에
 * 걸쳐 유지되는 정책 데이터입니다. 예를 들어 blk-throttle이 cgroup 전체
 * IO/s 한도를 관리할 때 사용됩니다.
 */
struct blkcg_policy_data {
	/* the blkcg and policy id this per-policy data belongs to */
	struct blkcg			*blkcg;	/* 소속 cgroup -> cgroup 전체 throttle 한도/weight 저장 */
	int				plid;	/* 정책 ID */
};

/*
 * 아래 typedef들은 각 blkcg 정책이 등록해야 하는 콜백 서명을 정의합니다.
 * NVMe 스택에서는 blkcg_policy_register()를 통해 blk-throttle, BFQ 등이
 * 등록되고, 이 콜백들이 blkg 생성/해제 시점에 불립니다.
 */
typedef struct blkcg_policy_data *(blkcg_pol_alloc_cpd_fn)(gfp_t gfp); /* per-cgroup data 할당 -> blkcg 생성 시 호출 */
typedef void (blkcg_pol_init_cpd_fn)(struct blkcg_policy_data *cpd);   /* per-cgroup data 초기화 */
typedef void (blkcg_pol_free_cpd_fn)(struct blkcg_policy_data *cpd);   /* per-cgroup data 해제 */
typedef void (blkcg_pol_bind_cpd_fn)(struct blkcg_policy_data *cpd);   /* cgroup bind 시점 정리 */
typedef struct blkg_policy_data *(blkcg_pol_alloc_pd_fn)(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp); /* per-blkg data 할당 -> NVMe namespace(gendisk)마다 생성 */
typedef void (blkcg_pol_init_pd_fn)(struct blkg_policy_data *pd);      /* per-blkg data 초기화 */
typedef void (blkcg_pol_online_pd_fn)(struct blkg_policy_data *pd);    /* blkg online 전환 -> NVMe I/O 수용 가능 */
typedef void (blkcg_pol_offline_pd_fn)(struct blkg_policy_data *pd);   /* blkg offline 전환 -> NVMe queue drain/명령 차단 */
typedef void (blkcg_pol_free_pd_fn)(struct blkg_policy_data *pd);      /* per-blkg data 해제 */
typedef void (blkcg_pol_reset_pd_stats_fn)(struct blkg_policy_data *pd); /* 통계 리셋 */
typedef void (blkcg_pol_stat_pd_fn)(struct blkg_policy_data *pd,
				struct seq_file *s); /* sysfs/proc 통계 출력 */

/*
 * struct blkcg_policy 는 하나의 cgroup I/O 정책(예: blk-throttle, BFQ)을
 * 커널에 등록할 때 사용하는 구조체입니다. NVMe SSD에서도 이 정책들이
 * request 발행 전/후에 개입하여 대역폭/IOPS를 제어합니다.
 */
struct blkcg_policy {
	int				plid;	/* 정책 고유 ID -> q->blkcg_pols bit 위치 */
	/* cgroup files for the policy */
	struct cftype			*dfl_cftypes;	/* cgroup v2 파일 -> io.max, io.weight 등 */
	struct cftype			*legacy_cftypes;	/* cgroup v1 파일 -> blkio.throttle.* */

	/* operations */
	blkcg_pol_alloc_cpd_fn		*cpd_alloc_fn;	/* per-cgroup 데이터 할당 */
	blkcg_pol_free_cpd_fn		*cpd_free_fn;	/* per-cgroup 데이터 해제 */

	blkcg_pol_alloc_pd_fn		*pd_alloc_fn;	/* per-blkg 데이터 할당 -> gendisk(NVMe namespace)마다 */
	blkcg_pol_init_pd_fn		*pd_init_fn;	/* per-blkg 데이터 초기화 */
	blkcg_pol_online_pd_fn		*pd_online_fn;	/* blkg 온라인 처리 -> request_queue 활성화 후 */
	blkcg_pol_offline_pd_fn		*pd_offline_fn;	/* blkg 오프라인 처리 -> QUEUE_FLAG_DYING 시 명령 완료 대기 */
	blkcg_pol_free_pd_fn		*pd_free_fn;	/* per-blkg 데이터 해제 */
	blkcg_pol_reset_pd_stats_fn	*pd_reset_stats_fn;	/* 통계 리셋 */
	blkcg_pol_stat_pd_fn		*pd_stat_fn;	/* sysfs/proc 통계 출력 */
};

extern struct blkcg blkcg_root;       /* root cgroup -> NVMe namespace에서 정책 미적용/기본값 */
extern bool blkcg_debug_stats;        /* 디버그 통계 출력 여부 -> NVMe I/O 통계 디버깅 시 사용 */

/*
 * request_queue 초기화 시 blk-cgroup 관련 자료구조를 세팅합니다.
 * NVMe namespace가 생성될 때 nvme_alloc_ns -> blk_mq_alloc_disk ->
 * blkcg_init_disk 경로에서 간접/직접 호출됩니다 (추정).
 */
void blkg_init_queue(struct request_queue *q);

/*
 * gendisk 생성 시 blk-cgroup 정책 데이터를 초기화합니다.
 * NVMe 드라이버가 nvme_alloc_ns()에서 disk를 등록할 때 연결됩니다 (추정).
 */
int blkcg_init_disk(struct gendisk *disk);

/*
 * gendisk 제거 시 blk-cgroup 리소스를 정리합니다.
 * NVMe namespace 해제 시 역순으로 호출됩니다 (추정).
 */
void blkcg_exit_disk(struct gendisk *disk);

/* Blkio controller policy registration */
/*
 * blk-throttle, BFQ 같은 정책을 커널에 등록/해제합니다.
 * NVMe 입장에서는 이 정책들이 request를 NVMe SQ에 밀어넣기 전에
 * 허용/지연/합병 여부를 결정하게 됩니다.
 */
int blkcg_policy_register(struct blkcg_policy *pol);
void blkcg_policy_unregister(struct blkcg_policy *pol);
int blkcg_activate_policy(struct gendisk *disk, const struct blkcg_policy *pol);  /* NVMe namespace별로 policy 활성화 -> q->blkcg_pols bit set */
void blkcg_deactivate_policy(struct gendisk *disk,
			     const struct blkcg_policy *pol);  /* NVMe namespace별로 policy 비활성화 -> SQ/CQ drain 후 cleanup */

const char *blkg_dev_name(struct blkcg_gq *blkg);
void blkcg_print_blkgs(struct seq_file *sf, struct blkcg *blkcg,
		       u64 (*prfill)(struct seq_file *,
				     struct blkg_policy_data *, int),
		       const struct blkcg_policy *pol, int data,
		       bool show_total);
u64 __blkg_prfill_u64(struct seq_file *sf, struct blkg_policy_data *pd, u64 v);

struct blkg_conf_ctx {
	char				*input;
	char				*body;
	struct block_device		*bdev;
	struct blkcg_gq			*blkg;
};

void blkg_conf_init(struct blkg_conf_ctx *ctx, char *input);
int blkg_conf_open_bdev(struct blkg_conf_ctx *ctx);
unsigned long blkg_conf_open_bdev_frozen(struct blkg_conf_ctx *ctx);
int blkg_conf_prep(struct blkcg *blkcg, const struct blkcg_policy *pol,
		   struct blkg_conf_ctx *ctx);
void blkg_conf_exit(struct blkg_conf_ctx *ctx);
void blkg_conf_exit_frozen(struct blkg_conf_ctx *ctx, unsigned long memflags);

/**
 * bio_issue_as_root_blkg - see if this bio needs to be issued as root blkg
 * @bio: the target &bio
 *
 * Return: true if this bio needs to be submitted with the root blkg context.
 *
 * In order to avoid priority inversions we sometimes need to issue a bio as if
 * it were attached to the root blkg, and then backcharge to the actual owning
 * blkg.  The idea is we do bio_blkcg_css() to look up the actual context for
 * the bio and attach the appropriate blkg to the bio.  Then we call this helper
 * and if it is true run with the root blkg for that queue and then do any
 * backcharging to the originating cgroup once the io is complete.
 */
/*
 * bio_issue_as_root_blkg()
 * 목적: 메타데이터/스왑 I/O가 NVMe SQ 발행 시 root cgroup으로 처리되어
 *      우선순위 역전을 피하도록 합니다. 이후 실제 소유 cgroup에 비용을
 *      재청구(backcharge)합니다.
 * 호출 경로: submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *          blk_cgroup_mergeable / bio_issue_as_root_blkg ->
 *          nvme_queue_rq -> nvme_submit_cmd(doorbell) (추정)
 */
static inline bool bio_issue_as_root_blkg(struct bio *bio)
{
	return (bio->bi_opf & (REQ_META | REQ_SWAP)) != 0; /* 메타데이터/스왑 bio면 true -> root blkg CID/tag 우선 배정 (추정) */
}

/**
 * blkg_lookup - lookup blkg for the specified blkcg - q pair
 * @blkcg: blkcg of interest
 * @q: request_queue of interest
 *
 * Lookup blkg for the @blkcg - @q pair.
 *
 * Must be called in a RCU critical section.
 */
/*
 * blkg_lookup()
 * 목적: 주어진 blkcg와 request_queue(q) 쌍에 해당하는 blkcg_gq를 빠르게 찾습니다.
 *      NVMe I/O 경로에서 bio가 속한 cgroup을 결정할 때 사용됩니다.
 * 호출 경로: submit_bio -> blk_mq_submit_bio -> blkg_lookup ->
 *          blk_throtl_bio / bfq -> nvme_queue_rq (추정)
 */
static inline struct blkcg_gq *blkg_lookup(struct blkcg *blkcg,
					   struct request_queue *q)
{
	struct blkcg_gq *blkg;

	if (blkcg == &blkcg_root)
		return q->root_blkg; /* root cgroup은 미리 할당된 root_blkg 즉시 반환 -> hot path 최적화 */

	/* hint 캐시를 먼저 확인: lockdep 검증 하에 RCU read로 접근 */
	blkg = rcu_dereference_check(blkcg->blkg_hint,
				lockdep_is_held(&q->queue_lock));
	if (blkg && blkg->q == q)
		return blkg; /* 캐시 히트 -> NVMe SQ/CQ 선택/정책 조회 지연 최소화 */

	/* hint 실패 시 radix tree에서 request_queue id로 정확히 검색 */
	blkg = radix_tree_lookup(&blkcg->blkg_tree, q->id);
	if (blkg && blkg->q != q)
		blkg = NULL; /* queue id 재사용/불일치 검증 -> 잘못된 blkg 연결 방지 (추정) */
	return blkg; /* NULL이면 root_blkg fallback 후 root cgroup 정책 적용 */
}

/**
 * blkg_to_pd - get policy private data
 * @blkg: blkg of interest
 * @pol: policy of interest
 *
 * Return pointer to private data associated with the @blkg-@pol pair.
 */
/*
 * blkg_to_pd()
 * 목적: blkg에서 특정 정책의 private data를 얻습니다.
 *      NVMe I/O 처리 중 blk-throttle 한도나 BFQ 우선순위를 조회할 때 사용.
 */
static inline struct blkg_policy_data *blkg_to_pd(struct blkcg_gq *blkg,
						  struct blkcg_policy *pol)
{
	return blkg ? blkg->pd[pol->plid] : NULL; /* blkg NULL이면 정책 skip -> NVMe queue_rq가 throttle 없이 진행 */
}

/*
 * blkcg_to_cpd(): cgroup 전용 정책 데이터를 조회합니다.
 * NVMe 관련 정책이 cgroup 전체 IOPS 제한을 관리할 때 사용 (추정).
 */
static inline struct blkcg_policy_data *blkcg_to_cpd(struct blkcg *blkcg,
						     struct blkcg_policy *pol)
{
	return blkcg ? blkcg->cpd[pol->plid] : NULL; /* NULL이면 root/비활성 정책으로 간주 */
}

/**
 * pd_to_blkg - get blkg associated with policy private data
 * @pd: policy private data of interest
 *
 * @pd is policy private data.  Determine the blkg it's associated with.
 */
/*
 * pd_to_blkg()
 * 목적: 정책 private data로부터 다시 blkg을 역참조합니다.
 *      NVMe SQ 발행 전 정책이 request의 소속 blkg을 찾을 때 유용합니다.
 */
static inline struct blkcg_gq *pd_to_blkg(struct blkg_policy_data *pd)
{
	return pd ? pd->blkg : NULL; /* policy data에서 request_queue/blkg 역참조 */
}

/*
 * cpd_to_blkcg(): per-cgroup 정책 데이터로부터 blkcg를 역참조합니다.
 */
static inline struct blkcg *cpd_to_blkcg(struct blkcg_policy_data *cpd)
{
	return cpd ? cpd->blkcg : NULL;
}

/**
 * blkg_get - get a blkg reference
 * @blkg: blkg to get
 *
 * The caller should be holding an existing reference.
 */
/*
 * blkg_get(): blkg 참조 카운트를 증가시킵니다.
 * NVMe request가 blkg을 계속 참조하면서 완료될 때까지 생존을 보장합니다.
 */
static inline void blkg_get(struct blkcg_gq *blkg)
{
	percpu_ref_get(&blkg->refcnt); /* percpu_ref -> hot path에서 CPU local fastpath로 참조 증가 */
}

/**
 * blkg_tryget - try and get a blkg reference
 * @blkg: blkg to get
 *
 * This is for use when doing an RCU lookup of the blkg.  We may be in the midst
 * of freeing this blkg, so we can only use it if the refcnt is not zero.
 */
/*
 * blkg_tryget(): RCU lookup 경로에서 blkg 해제 중일 수 있으므로 원자적으로
 * refcnt가 0이 아닐 때만 참조를 획득합니다. NVMe I/O 완료 interrupt에서
 * blkg 사용 전 안전성을 위해 사용될 수 있습니다.
 */
static inline bool blkg_tryget(struct blkcg_gq *blkg)
{
	return blkg && percpu_ref_tryget(&blkg->refcnt); /* blkg NULL이거나 해제 중이면 false -> NVMe completion이 backcharge skip */
}

/**
 * blkg_put - put a blkg reference
 * @blkg: blkg to put
 */
/*
 * blkg_put(): blkg 참조를 해제합니다. 마지막 참조 시 free_work를 통해
 * 구조체가 해제될 수 있습니다.
 */
static inline void blkg_put(struct blkcg_gq *blkg)
{
	percpu_ref_put(&blkg->refcnt); /* refcnt 0이면 percpu_ref_release -> schedule free_work -> RCU grace 후 메모리 반납 */
}

/**
 * blkg_for_each_descendant_pre - pre-order walk of a blkg's descendants
 * @d_blkg: loop cursor pointing to the current descendant
 * @pos_css: used for iteration
 * @p_blkg: target blkg to walk descendants of
 *
 * Walk @c_blkg through the descendants of @p_blkg.  Must be used with RCU
 * read locked.  If called under either blkcg or queue lock, the iteration
 * is guaranteed to include all and only online blkgs.  The caller may
 * update @pos_css by calling css_rightmost_descendant() to skip subtree.
 * @p_blkg is included in the iteration and the first node to be visited.
 */
#define blkg_for_each_descendant_pre(d_blkg, pos_css, p_blkg)		\
	css_for_each_descendant_pre((pos_css), &(p_blkg)->blkcg->css)	\
		if (((d_blkg) = blkg_lookup(css_to_blkcg(pos_css),	\
					    (p_blkg)->q))) /* pre-order 순회: 부모 먼저 -> blk-throttle이 하위 cgroup에 IO 할당량 상속/전파 */

/**
 * blkg_for_each_descendant_post - post-order walk of a blkg's descendants
 * @d_blkg: loop cursor pointing to the current descendant
 * @pos_css: used for iteration
 * @p_blkg: target blkg to walk descendants of
 *
 * Similar to blkg_for_each_descendant_pre() but performs post-order
 * traversal instead.  Synchronization rules are the same.  @p_blkg is
 * included in the iteration and the last node to be visited.
 */
#define blkg_for_each_descendant_post(d_blkg, pos_css, p_blkg)		\
	css_for_each_descendant_post((pos_css), &(p_blkg)->blkcg->css)	\
		if (((d_blkg) = blkg_lookup(css_to_blkcg(pos_css),	\
					    (p_blkg)->q))) /* post-order 순회: 자식 먼저 -> 통계 하위에서 상위로 합산 후 NVMe namespace 집계 */

/*
 * blkcg_use_delay()
 * 목적: 이 blkg에 대한 지연(delay) 사용 카운트를 증가시키고, 첫 지연자라면
 *      cgroup의 congestion_count를 올려 NVMe queue에 백프레셔를 표시합니다.
 * 호출 경로: blk_cgroup_bio_start -> blkcg_add_delay ->
 *          blkcg_use_delay -> blk_mq_get_request 지연 경로 (추정)
 */
static inline void blkcg_use_delay(struct blkcg_gq *blkg)
{
	if (WARN_ON_ONCE(atomic_read(&blkg->use_delay) < 0))
		return; /* use_delay가 음수면 set_delay 모드와 혼용된 버그 -> 경고 후 무시 */
	/* 첫 번째 지연자가 congestion_count를 증가시킴 */
	if (atomic_add_return(1, &blkg->use_delay) == 1)
		atomic_inc(&blkg->blkcg->congestion_count); /* congestion_count != 0 -> NVMe submitter sleep/지연 결정 */
}

/*
 * blkcg_unuse_delay()
 * 목적: blkg 지연 사용 카운트를 하나 감소시키고, 마지막 지연자라면
 *      congestion_count를 낮춰 NVMe queue 발행 제한을 해제합니다.
 *      다른 CPU와의 경쟁을 방지하기 위해 atomic_try_cmpxchg 루프 사용.
 */
static inline int blkcg_unuse_delay(struct blkcg_gq *blkg)
{
	int old = atomic_read(&blkg->use_delay); /* 현재 delay 사용자 수 스냅샷 -> 다른 CPU와 경쟁 가능 */

	if (WARN_ON_ONCE(old < 0))
		return 0; /* set_delay 모드와 혼용 감지 */
	if (old == 0)
		return 0; /* 이미 지연 없음 -> NVMe doorbell 정상 발행 */

	/*
	 * We do this song and dance because we can race with somebody else
	 * adding or removing delay.  If we just did an atomic_dec we'd end up
	 * negative and we'd already be in trouble.  We need to subtract 1 and
	 * then check to see if we were the last delay so we can drop the
	 * congestion count on the cgroup.
	 */
	/* atomic_dec 음수 방지를 위해 CAS 루프로 1 감소 */
	while (old && !atomic_try_cmpxchg(&blkg->use_delay, &old, old - 1))
		; /* 실패 시 old 갱신 후 재시도 -> smp_mb__after_atomic 내장 (추정) */

	if (old == 0)
		return 0; /* race로 이미 0이면 중단 */
	/* 마지막 지연자가 congestion_count 감소 -> NVMe I/O 발행 재개 신호 */
	if (old == 1)
		atomic_dec(&blkg->blkcg->congestion_count); /* congestion_count 0이면 NVMe submitter 깨어나 doorbell 재개 */
	return 1;
}

/**
 * blkcg_set_delay - Enable allocator delay mechanism with the specified delay amount
 * @blkg: target blkg
 * @delay: delay duration in nsecs
 *
 * When enabled with this function, the delay is not decayed and must be
 * explicitly cleared with blkcg_clear_delay(). Must not be mixed with
 * blkcg_[un]use_delay() and blkcg_add_delay() usages.
 */
/*
 * blkcg_set_delay()
 * 목적: blkg에 고정된 지연 시간을 설정합니다. blkcg_use_delay()와 달리
 *      decay되지 않으며, blkcg_clear_delay()로 명시적으로 해제해야 합니다.
 *      NVMe SQ doorbell 전 요청 대기에 사용될 수 있습니다 (추정).
 */
static inline void blkcg_set_delay(struct blkcg_gq *blkg, u64 delay)
{
	int old = atomic_read(&blkg->use_delay); /* 현재 delay 상태 읽기 -> 0일 때만 고정 지연 진입 */

	/* We only want 1 person setting the congestion count for this blkg. */
	/* 한 명만 congestion_count를 설정하도록 CAS */
	if (!old && atomic_try_cmpxchg(&blkg->use_delay, &old, -1))
		atomic_inc(&blkg->blkcg->congestion_count); /* use_delay=-1 표식 -> 고정 지연 모드, NVMe submit block (추정) */

	atomic64_set(&blkg->delay_nsec, delay); /* 요청 할당자가 sleep할 nsec 기록 -> nvme_queue_rq 발행 전 지연 시간 (추정) */
}

/**
 * blkcg_clear_delay - Disable allocator delay mechanism
 * @blkg: target blkg
 *
 * Disable use_delay mechanism. See blkcg_set_delay().
 */
/*
 * blkcg_clear_delay()
 * 목적: blkcg_set_delay()로 설정된 고정 지연을 해제하고 congestion_count를
 *      감소시켜 NVMe I/O 발행 제한을 풉니다.
 */
static inline void blkcg_clear_delay(struct blkcg_gq *blkg)
{
	int old = atomic_read(&blkg->use_delay); /* 현재 -1(고정지연) 또는 0 확인 */

	/* We only want 1 person clearing the congestion count for this blkg. */
	/* 한 명만 congestion_count를 해제하도록 CAS */
	if (old && atomic_try_cmpxchg(&blkg->use_delay, &old, 0))
		atomic_dec(&blkg->blkcg->congestion_count); /* congestion_count 0이면 NVMe queue 깨어나 SQ doorbell 재개 */
}

/**
 * blk_cgroup_mergeable - Determine whether to allow or disallow merges
 * @rq: request to merge into
 * @bio: bio to merge
 *
 * @bio and @rq should belong to the same cgroup and their issue_as_root should
 * match. The latter is necessary as we don't want to throttle e.g. a metadata
 * update because it happens to be next to a regular IO.
 */
/*
 * blk_cgroup_mergeable()
 * 목적: 두 I/O를 NVMe SQ에 합칠(merge) 수 있는지 cgroup 소속과
 *      root-blkg 발행 여부를 검사합니다. 메타데이터 I/O가 일반 I/O와 합쳐져
 *      잘못 throttle되는 것을 방지합니다.
 * 호출 경로: blk_mq_bio_list_merge -> blk_cgroup_mergeable ->
 *          blk_mq_try_merge -> nvme_queue_rq (추정)
 */
static inline bool blk_cgroup_mergeable(struct request *rq, struct bio *bio)
{
	return rq->bio->bi_blkg == bio->bi_blkg && /* 동일 cgroup/정책 컨텍스트 -> CID/tag 회계 일관성 유지 */
		bio_issue_as_root_blkg(rq->bio) == bio_issue_as_root_blkg(bio); /* root 발행 일치 -> 메타데이터 우선순위 역전 방지 */
}

/*
 * blkcg_policy_enabled()
 * 목적: 주어진 request_queue에서 특정 blkcg 정책이 활성화되어 있는지 확인.
 *      NVMe queue에 blk-throttle 등이 걸려 있는지 판단할 때 사용.
 */
static inline bool blkcg_policy_enabled(struct request_queue *q,
				const struct blkcg_policy *pol)
{
	return pol && test_bit(pol->plid, q->blkcg_pols); /* bit test -> 활성화된 정책이 있으면 NVMe 경로에서 throttle/우선순위 검사 수행 */
}

/*
 * blk_cgroup_bio_start()
 * 목적: bio 처리 시작 시 cgroup 지연/통계 계산을 수행합니다.
 *      NVMe I/O가 실제 request로 변환되기 전 throttle 판단 지점에서
 *      호출됩니다 (추정).
 * 호출 경로: submit_bio -> blk_cgroup_bio_start -> blkcg_add_delay ->
 *          blkcg_use_delay -> blk_mq_get_request 지연 (추정)
 */
void blk_cgroup_bio_start(struct bio *bio);

/*
 * blkcg_add_delay()
 * 목적: 현재 시각(now)에 delta 만큼의 지연을 누적합니다.
 *      blk-throttle 등에서 NVMe IOPS/대역폭 한도 초과 시 요청 발행을
 *      늦추는 데 사용됩니다.
 */
void blkcg_add_delay(struct blkcg_gq *blkg, u64 now, u64 delta); /* delta 누적 -> delta가 delay_nsec 한도 초과 시 use_delay 증가/발행 지연 */
#else	/* CONFIG_BLK_CGROUP */

struct blkg_policy_data {
};

struct blkcg_policy_data {
};

struct blkcg_policy {
};

struct blkcg {
};

static inline struct blkcg_gq *blkg_lookup(struct blkcg *blkcg, void *key) { return NULL; } /* cgroup 미지원 -> NVMe 경로에서 항상 root */
static inline void blkg_init_queue(struct request_queue *q) { }
static inline int blkcg_init_disk(struct gendisk *disk) { return 0; }
static inline void blkcg_exit_disk(struct gendisk *disk) { }
static inline int blkcg_policy_register(struct blkcg_policy *pol) { return 0; }
static inline void blkcg_policy_unregister(struct blkcg_policy *pol) { }
static inline int blkcg_activate_policy(struct gendisk *disk,
					const struct blkcg_policy *pol) { return 0; }
static inline void blkcg_deactivate_policy(struct gendisk *disk,
					   const struct blkcg_policy *pol) { }

static inline struct blkg_policy_data *blkg_to_pd(struct blkcg_gq *blkg,
						  struct blkcg_policy *pol) { return NULL; }
static inline struct blkcg_gq *pd_to_blkg(struct blkg_policy_data *pd) { return NULL; }
static inline void blkg_get(struct blkcg_gq *blkg) { }
static inline void blkg_put(struct blkcg_gq *blkg) { }
static inline void blk_cgroup_bio_start(struct bio *bio) { }
static inline bool blk_cgroup_mergeable(struct request *rq, struct bio *bio) { return true; } /* merge 항상 허용 -> NVMe SQ entry 수 최소화 */

#define blk_queue_for_each_rl(rl, q)	\
	for ((rl) = &(q)->root_rl; (rl); (rl) = NULL) /* request limit 순회: cgroup 미지원 시 root_rl만 존재 -> NVMe queue depth 제한 단순화 */

#endif	/* CONFIG_BLK_CGROUP */

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 blkcg <-> request_queue 연결 구조(blkcg_gq)와 정책 콜백을 정의하며,
 *   NVMe namespace로 향하는 I/O가 blk-mq를 거쳐 nvme_queue_rq()로 전달되기 전
 *   cgroup 소속/지연/합병 정보를 결정하는 기반이 됩니다.
 *
 * - blkcg_gq 는 per-cpu iostat, 지연 메커니즘(use_delay/delay_nsec),
 *   그리고 blk-throttle/BFQ 같은 정책 데이터(pd[])를 담아 NVMe I/O를
 *   계층적으로 제어합니다.
 *
 * - blkcg_use_delay()/blkcg_unuse_delay()/blkcg_set_delay()/blkcg_clear_delay()는
 *   atomic reference/counter를 이용해 NVMe queue 수준의 congestion/backpressure를
 *   표현하며, IOPS/대역폭 초과 시 request 발행을 늦추는 데 사용됩니다 (추정).
 *
 * - blkg_lookup()은 hint + radix tree를 통해 NVMe I/O 경로에서 빠른
 *   cgroup lookup을 제공하고, blkg_to_pd()/pd_to_blkg()는 정책별 상태를
 *   request와 연결합니다.
 *
 * - 상위 인터페이스인 include/linux/blk-cgroup.h, 구현체인 block/blk-cgroup.c,
 *   그리고 block/blk-mq.c, block/blk-throttle.c, block/bfq-iosched.c 등과
 *   함께 NVMe SSD의 end-to-end I/O 제어 스택을 구성합니다.
 */

#endif /* _BLK_CGROUP_PRIVATE_H */
