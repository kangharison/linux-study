/*
 * block/blk-mq.h: 리눅스 블록 계층의 멀티큐(block-mq) 납부자 인터페이스.
 *
 * NVMe SSD 관점에서 본 위치:
 *   응용/파일시스템 -> VFS -> 페이지캐시 -> bio -> block/blk-mq.c (bio 정리)
 *   -> block/blk-mq-sched.c (스케줄러) -> block/blk-mq-tag.c (tag/CID 할당)
 *   -> drivers/nvme/host/pci.c 의 nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * 본 헤더는 위 경로에서 blk_mq_hw_ctx(hardware queue, NVMe의 SQ/CQ 쌍에 해당),
 * blk_mq_ctx(software queue, CPU별 제출 컨텍스트), tag(CID), dispatch budget
 * 등을 정의하며, NVMe 드라이버가 multi-queue I/O를 처리하는 데 필요한
 * 핵심 자료구조와 납부자 함수를 노출한다.
 */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef INT_BLK_MQ_H
#define INT_BLK_MQ_H

#include <linux/blk-mq.h>	/* NVMe 드라이버가 직접 포함하는 blk-mq API의 납부자 헤더 */
#include "blk-stat.h"		/* NVMe I/O 지연/대기 통계 수집용(추정) */

struct blk_mq_tag_set;		/* NVMe 장치 전체 CID(tag) 풀 정의 전방 선언 */
struct elevator_tags;		/* mq-deadline/kyber 등 스케줄러 tag pool 전방 선언 */

/*
 * struct blk_mq_ctxs: 여러 CPU별 blk_mq_ctx를 묶어 관리한다.
 * NVMe 관점에서는 여러 CPU가 동일한 request_queue를 통해 제출할 때
 * 각 CPU의 software queue(ctx)를 묶어주는 컨테이너 역할을 한다.
 */
struct blk_mq_ctxs {		/* NVMe controller에 속한 모든 CPU별 software queue(ctx) 컨테이너(추정) */
	struct kobject kobj;				/* sysfs 노드 생명주기 관리 */
	struct blk_mq_ctx __percpu	*queue_ctx;	/* per-CPU software queue 배열: CPU 수만큼 NVMe 제출 진입점 존재 */
};

/**
 * struct blk_mq_ctx - State for a software queue facing the submitting CPUs
 *
 * NVMe 연결:
 *   - 각 blk_mq_ctx는 특정 CPU에서 제출되는 I/O를 모으는 software queue다.
 *   - 이후 blk_mq_map_queue()를 거쳐 하나의 blk_mq_hw_ctx로 연결되며,
 *     NVMe 드라이버에서는 해당 blk_mq_hw_ctx가 nvme_queue(SQ/CQ 쌍)에
 *     매핑된다(구현에 따라 1:1 또는 N:1, (추정)).
 *   - rq_lists[HCTX_MAX_TYPES]: READ/POLL/DEFAULT 등 유형별로 분리된
 *     request 리스트. NVMe 성능 최적화를 위해 읽기/폴 분리가 가능하다.
 *   - cpu: 이 ctx가 속한 CPU.
 *   - index_hw[] / hctxs[]: CPU->hardware queue 매핑. NVMe SQ 선택에 직접
 *     영향을 준다.
 *   - queue / ctxs / kobj: sysfs 및 request_queue 역참조용.
 */
struct blk_mq_ctx {		/* CPU별 software queue: bio -> ctx -> hctx -> nvme_queue/SQ(추정) */
	struct {
		spinlock_t		lock;		/* ctx 낮은 rq_lists 보호: 제출자-디스패처 동시 접근 방지 */
		struct list_head	rq_lists[HCTX_MAX_TYPES]; /* READ/POLL/DEFAULT request 큐; NVMe SQ 유형별 분산 */
	} ____cacheline_aligned_in_smp;	/* cacheline 정렬: NVMe 제출자와 softirq 완료 handler 간 false sharing 방지(추정) */

	unsigned int		cpu;			/* 이 ctx가 소속된 CPU; NVMe SQ affinity 분배 기준 */
	unsigned short		index_hw[HCTX_MAX_TYPES]; /* CPU->hctx 인덱스; NVMe SQ 번호 선택 */
	struct blk_mq_hw_ctx 	*hctxs[HCTX_MAX_TYPES];	 /* 실제 NVMe SQ/CQ 쌍을 가리키는 hctx 포인터 */

	struct request_queue	*queue;			/* 역참조: 어떤 NVMe 디스크의 request_queue인지 */
	struct blk_mq_ctxs      *ctxs;			/* 상위 컨테이너; sysfs lifetime 연결 */
	struct kobject		kobj;			/* per-ctx sysfs 진입점 */
} ____cacheline_aligned_in_smp;	/* 전체 struct cacheline 정렬: CPU별 NVMe 제출 경로의 cache locality 확보(추정) */

enum {				/* block layer tag = NVMe CID(Command Identifier) 범위(추정) */
	BLK_MQ_NO_TAG		= -1U,	/* CID/tag 미할당 상태; NVMe SQ 엔트리 미배정 */
	BLK_MQ_TAG_MIN		= 1,	/* 최소 tag 번호; 0은 보통 예약/미사용 */
	BLK_MQ_TAG_MAX		= BLK_MQ_NO_TAG - 1, /* 최대 CID: NVMe 명령 수 상한과 관련 */
};

#define BLK_MQ_CPU_WORK_BATCH	(8)	/* 한 CPU에서 한 번에 처리할 request 수; NVMe doorbell batch 크기에 영향(추정) */

typedef unsigned int __bitwise blk_insert_t;	/* request 삽입 위치/방식; NVMe abort 후 우선 재제출 등에 사용(추정) */
#define BLK_MQ_INSERT_AT_HEAD		((__force blk_insert_t)0x01) /* requeue/head 삽입: NVMe abort 후 우선 재제출 시 사용 가능(추정) */

/*
 * blk_mq_submit_bio(): 응용/파일시스템이 만든 bio를 block layer에 제출한다.
 * NVMe 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_setup_cmd(SQ 엔트리 구성) -> nvme_submit_cmd(doorbell)
 */
void blk_mq_submit_bio(struct bio *bio);	/* bio -> request 변환 및 NVMe 제출 경로 진입(추정) */

/*
 * blk_mq_poll(): 폴(poll) 방식 완료를 처리한다.
 * NVMe 연결:
 *   - 폴 모드에서는 host가 NVMe CQ를 직접 회수(polling)하여 인터럽트
 *     없이 완료 지연을 줄인다.
 *   - cookie에 포함된 tag(CID)를 통해 어느 CQ를 봐야 하는지 파악한다.
 */
int blk_mq_poll(struct request_queue *q, blk_qc_t cookie, struct io_comp_batch *iob,
		unsigned int flags);	/* poll 완료: NVMe CQ 직접 회수, interrupt coalescing 우회(추정) */
void blk_mq_exit_queue(struct request_queue *q);	/* request_queue 소멸; NVMe controller remove/reset 시 정리(추정) */
struct elevator_tags *blk_mq_update_nr_requests(struct request_queue *q,
						struct elevator_tags *tags,
						unsigned int nr);	/* queue depth(nr_requests) 동적 변경; NVMe SQ depth 재조정(추정) */
void blk_mq_wake_waiters(struct request_queue *q);	/* tag(CID) 풀 여유 발생 시 대기 제출자 깨움 -> doorbell 재시도(추정) */
bool blk_mq_dispatch_rq_list(struct blk_mq_hw_ctx *hctx, struct list_head *,
			     bool);	/* hctx(SQ)로 request 리스트 전달; nvme_queue_rq 호출 직전 관문(추정) */
void blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx, struct list_head *list);	/* 스케줄러 pending -> dispatch list 이동 -> NVMe SQ 배치(추정) */
struct request *blk_mq_dequeue_from_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *start);	/* 특정 ctx에서 다음 NVMe 명령 후보 request 추출(추정) */
void blk_mq_put_rq_ref(struct request *rq);		/* request reference 해제; NVMe 완료 후 메모리 반환(추정) */

/*
 * Internal helpers for allocating/freeing the request map
 *
 * NVMe 관점:
 *   - tag_set은 NVMe 장치 전체의 CID(tag) 풀을 관리한다.
 *   - blk_mq_alloc_map_and_rqs()는 SQ당 사용 가능한 CID 개수(depth)만큼
 *     tag bitmap과 request 배열을 할당한다.
 */
void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,
		     unsigned int hctx_idx);		/* hctx_idx 번 SQ의 request/CID 배열 해제(추정) */
void blk_mq_free_rq_map(struct blk_mq_tag_set *set, struct blk_mq_tags *tags);	/* tag bitmap 해제; NVMe queue 삭제 전(추정) */
struct blk_mq_tags *blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
				unsigned int hctx_idx, unsigned int depth);	/* NVMe SQ당 depth만큼 CID pool+request 할당(추정) */
void blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
			     struct blk_mq_tags *tags,
			     unsigned int hctx_idx);		/* map+rq 통합 해제; NVMe queue teardown(추정) */

/*
 * CPU -> queue mappings
 */
extern int blk_mq_hw_queue_to_node(struct blk_mq_queue_map *qmap, unsigned int);	/* NUMA 노드별 NVMe SQ affinity 결정(추정) */

/*
 * blk_mq_map_queue_type() - map (hctx_type,cpu) to hardware queue
 * @q: request queue
 * @type: the hctx type index
 * @cpu: CPU
 *
 * NVMe 연결:
 *   - CPU와 hctx_type(READ/POLL/DEFAULT)에 따라 어느 blk_mq_hw_ctx(SQ/CQ)
 *     로 볼낼지 결정한다.
 *   - NVMe 드라이버는 nr_hw_queues만큼 nvme_queue를 만들고, 이 매핑을
 *     통해 CPU 코어가 특정 SQ로 분배된다.
 */
static inline struct blk_mq_hw_ctx *blk_mq_map_queue_type(struct request_queue *q,
						  enum hctx_type type,
						  unsigned int cpu)
{							/* (type, cpu) -> NVMe SQ/CQ 쌍 매핑(추정) */
	return queue_hctx((q), (q->tag_set->map[type].mq_map[cpu]));	/* cpu->NVMe SQ 인덱스 변환; mq_map[cpu]가 곧 SQ 번호(추정) */
}

/*
 * blk_mq_get_hctx_type(): bio의 플래그에 따라 hardware queue 유형을 선택한다.
 * NVMe 연결:
 *   - REQ_POLLED: 폴 모드 CQ를 사용하는 hctx를 선택 -> nvme_poll_cq()
 *   - REQ_OP_READ: 읽기 전용 hctx(분리되어 있을 경우)
 *   - 그 외: DEFAULT hctx
 */
static inline enum hctx_type blk_mq_get_hctx_type(blk_opf_t opf)
{
	enum hctx_type type = HCTX_TYPE_DEFAULT;	/* 기본 NVMe SQ/CQ 쌍 선택 시작점 */

	/*
	 * The caller ensure that if REQ_POLLED, poll must be enabled.
	 */
	if (opf & REQ_POLLED)
		type = HCTX_TYPE_POLL;			/* 폴 CQ 사용; 인터럽트 없이 NVMe 완료 회수 */
	else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
		type = HCTX_TYPE_READ;			/* 읽기 전용 SQ 분리 시 NVMe read parallelism 향상(추정) */
	return type;
}

/*
 * blk_mq_map_queue() - map (cmd_flags,type) to hardware queue
 * @opf: operation type (REQ_OP_*) and flags (e.g. REQ_POLLED).
 * @ctx: software queue cpu ctx
 *
 * NVMe 연결:
 *   - submit_bio -> blk_mq_get_ctx(CPU별 software queue) -> blk_mq_map_queue()
 *   -> blk_mq_hw_ctx -> nvme_queue_rq -> nvme_queue(SQ/CQ) 선택
 */
static inline struct blk_mq_hw_ctx *blk_mq_map_queue(blk_opf_t opf,
						     struct blk_mq_ctx *ctx)
{
	return ctx->hctxs[blk_mq_get_hctx_type(opf)];	/* opf -> NVMe SQ/CQ 쌍 최종 선택; hctxs[]는 초기화 시 mq_map 기준 설정(추정) */
}

/*
 * Default to double of smaller one between hw queue_depth and
 * 128, since we don't split into sync/async like the old code
 * did. Additionally, this is a per-hw queue depth.
 *
 * NVMe 연결:
 *   - 반환값은 각 SQ당 Outstanding 명령 수 상한(CID 개수)의 초기값으로
 *     사용된다. NVMe 컨트롤러의 MQES(Minimum Queue Entry Size)와는
 *     별도로 block layer가 관리하는 소프트웨어 한도다.
 */
static inline unsigned int blk_mq_default_nr_requests(
		struct blk_mq_tag_set *set)
{
	return 2 * min_t(unsigned int, set->queue_depth, BLKDEV_DEFAULT_RQ);	/* NVMe SQ depth 대비 sw queue depth 초기화; 2배 버퍼로 burst 수용(추정) */
}

/*
 * sysfs helpers
 */
extern void blk_mq_sysfs_init(struct request_queue *q);		/* /sys/block/<disk>/queue/mq 초기화; NVMe SQ 노드 생성(추정) */
extern void blk_mq_sysfs_deinit(struct request_queue *q);	/* sysfs 트리 제거; NVMe 제거 시(추정) */
int blk_mq_sysfs_register(struct gendisk *disk);			/* gendisk 아래에 NVMe 디스크 sysfs 등록(추정) */
void blk_mq_sysfs_unregister(struct gendisk *disk);			/* 역등록(추정) */
int blk_mq_sysfs_register_hctxs(struct request_queue *q);	/* per-hctx(NVMe SQ) sysfs 등록(추정) */
void blk_mq_sysfs_unregister_hctxs(struct request_queue *q);	/* per-hctx sysfs 해제(추정) */
extern void blk_mq_hctx_kobj_init(struct blk_mq_hw_ctx *hctx);	/* hctx kobject 초기화(추정) */
void blk_mq_free_plug_rqs(struct blk_plug *plug);			/* plug batch에 모인 request 해제; 아직 doorbell 안 된 NVMe request 폐기(추정) */
void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule);	/* plug -> NVMe dispatch: batched I/O 전송(추정) */

void blk_mq_cancel_work_sync(struct request_queue *q);			/* queue workqueue 취소; NVMe reset 시 outstanding dispatch work 정리(추정) */

void blk_mq_release(struct request_queue *q);				/* request_queue 최종 해제; NVMe controller 제거 시(추정) */

/*
 * __blk_mq_get_ctx(): 주어진 cpu에 해당하는 blk_mq_ctx를 반환한다.
 * NVMe 연결:
 *   - 제출 경로에서 현재 CPU의 software queue를 빠르게 찾아,
 *     lockless fast path로 nvme_queue에 도달하기 위한 첫 단계다.
 */
static inline struct blk_mq_ctx *__blk_mq_get_ctx(struct request_queue *q,
						  unsigned int cpu)
{
	return per_cpu_ptr(q->queue_ctx, cpu);	/* per-CPU software queue: 제출자가 자신의 CPU NVMe 진입점 획득 */
}

/*
 * This assumes per-cpu software queueing queues. They could be per-node
 * as well, for instance. For now this is hardcoded as-is. Note that we don't
 * care about preemption, since we know the ctx's are persistent. This does
 * mean that we can't rely on ctx always matching the currently running CPU.
 *
 * NVMe 관점:
 *   - 현재 실행 중인 CPU의 software queue를 가져온다. preemption으로 인해
 *     ctx가 실제 CPU와 항상 일치하지는 않지만, 이는 NVMe SQ 분배의
 *     균일성에만 영향을 주고 정합성을 해치지는 않는다(추정).
 */
static inline struct blk_mq_ctx *blk_mq_get_ctx(struct request_queue *q)
{
	return __blk_mq_get_ctx(q, raw_smp_processor_id());	/* 실행 CPU 식별 -> NVMe SQ affinity 선정; preemption 시 약간의 skew 허용(추정) */
}

/*
 * struct blk_mq_alloc_data: request/tag 할당 때 사용하는 매개변수 구조체.
 *
 * NVMe 연결:
 *   - q / ctx / hctx: 어느 NVMe queue(nvme_queue/SQ)에 명령을 낼지 지정.
 *   - cmd_flags: READ/WRITE/POLL 등 -> NVMe opcode(NVME_CMD_READ/WRITE)와
 *     hctx 유형 결정.
 *   - rq_flags: RQF_SCHED_TAGS 등 스케줄러 tag 사용 여부.
 *   - shallow_depth / nr_tags: 할당할 tag(CID) 개수 제한. NVMe SQ가 꽉 찼을
 *     때 태그 부족으로 -EAGAIN/블로킹이 발생할 수 있다.
 *   - cached_rqs: 미리 할당된 request를 재사용하여 doorbell 레지스터 쓰기
 *     빈도를 줄이는(배치 제출) 최적화에 활용될 수 있다(추정).
 */
struct blk_mq_alloc_data {	/* request/tag 할당 매개변수; NVMe CID/SQ 선택(추정) */
	/* input parameter */
	struct request_queue *q;	/* 대상 NVMe 디스크 큐 */
	blk_mq_req_flags_t flags;	/* BLK_MQ_REQ_*: 예약 태그, 비블록, 폴 등 NVMe 명령 속성 */
	unsigned int shallow_depth;	/* 한 번에 할당할 최대 CID 수; NVMe SQ 여유 기반 제한 */
	blk_opf_t cmd_flags;		/* bio op + flags -> NVMe opcode/폴 플래그 */
	req_flags_t rq_flags;		/* request 상태 플래그; RQF_SCHED_TAGS 등 */

	/* allocate multiple requests/tags in one go */
	unsigned int nr_tags;		/* 연속 CID 개수 요청; PRP/SGL 다중 범위에 활용 가능(추정) */
	struct rq_list *cached_rqs;	/* 미리 할당된 request cache; doorbell batch 최적화(추정) */

	/* input & output parameter */
	struct blk_mq_ctx *ctx;		/* 선택된 software queue; in/out */
	struct blk_mq_hw_ctx *hctx;	/* 선택된 NVMe SQ/CQ 쌍; in/out */
};

struct blk_mq_tags *blk_mq_init_tags(unsigned int nr_tags,
		unsigned int reserved_tags, unsigned int flags, int node);	/* CID bitmap 초기화; NVMe SQ당 tag pool 생성(추정) */
void blk_mq_free_tags(struct blk_mq_tag_set *set, struct blk_mq_tags *tags);	/* tag pool 해제(추정) */

/*
 * blk_mq_get_tag(): 하나의 CID(tag)를 할당한다.
 * NVMe 연결:
 *   - NVMe submission command에는 16비트 CID(Command Identifier)가 있다.
 *   - block layer의 tag는 이 CID에 대응되며, 동일 SQ 내에서 고유해야 한다.
 *   - tag 부족 시 제출자는 대기(sbitmap_queue wait queue)하게 된다.
 */
unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data);	/* 단일 CID 할당; NVMe doorbell 전 필수(추정) */
unsigned long blk_mq_get_tags(struct blk_mq_alloc_data *data, int nr_tags,
		unsigned int *offset);	/* 연속 CID 블록 할당; scatter/gather 또는 다중 SQ 엔트리 제출에 사용 가능(추정) */
void blk_mq_put_tag(struct blk_mq_tags *tags, struct blk_mq_ctx *ctx,
		unsigned int tag);	/* NVMe 명령 완료 후 단일 CID 반환(추정) */
void blk_mq_put_tags(struct blk_mq_tags *tags, int *tag_array, int nr_tags);	/* 다수 CID 반환; NVMe CQ 일괄 처리 후(추정) */
void blk_mq_tag_resize_shared_tags(struct blk_mq_tag_set *set,
		unsigned int size);	/* 공유 tag pool 크기 조정; NVMe queue depth 동적 변경(추정) */
void blk_mq_tag_update_sched_shared_tags(struct request_queue *q,
					 unsigned int nr);	/* 스케줄러 공유 tag 수 갱신; NVMe QoS 스케줄러(추정) */

void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool);	/* CID 대기열 전체 깨움; NVMe SQ 여유 발생 시 제출자 재시도(추정) */
void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_tag_iter_fn *fn,
		void *priv);	/* queue 전체 outstanding NVMe 명령 순회; timeout/abort 검색(추정) */
void blk_mq_all_tag_iter(struct blk_mq_tags *tags, busy_tag_iter_fn *fn,
		void *priv);	/* 특정 tag pool 전체 순회; NVMe CID별 abort/reset 처리(추정) */

/*
 * bt_wait_ptr(): sbitmap_queue의 wait state를 반환한다.
 * NVMe 연결:
 *   - tag(CID)가 부족해 대기 중인 제출자를 깨울 때 사용.
 *   - hctx가 NULL이면 공용 wait state를, 아니면 hctx별 wait_index에 따라
 *     분산된 wait state를 사용해 nvme_queue 간 lock contention을 줄인다.
 */
static inline struct sbq_wait_state *bt_wait_ptr(struct sbitmap_queue *bt,
						 struct blk_mq_hw_ctx *hctx)
{
	if (!hctx)
		return &bt->ws[0];		/* 공용 wait state: NVMe 장치 전체 CID 부족 시(추정) */
	return sbq_wait_ptr(bt, &hctx->wait_index);	/* hctx별 wait_index 분산: SQ 간 wake contention 완화(추정) */
}

void __blk_mq_tag_busy(struct blk_mq_hw_ctx *);	/* hctx 활성화 세부 처리(추정) */
void __blk_mq_tag_idle(struct blk_mq_hw_ctx *);	/* hctx 비활성화 세부 처리(추정) */

/*
 * blk_mq_tag_busy(): 이 hctx(SQ/CQ)가 tag를 활발히 사용 중임을 표시한다.
 * NVMe 연결:
 *   - shared tag 환경에서 특정 nvme_queue가 활성화되었음을 기록하여,
 *     hctx_may_queue()에서 태그 공정 분배에 참조된다.
 */
static inline void blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_tag_busy(hctx);	/* shared tag 모드에서만 활성화 표시; NVMe SQ간 공정 분배 플래그 설정 */
}

static inline void blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_tag_idle(hctx);	/* shared tag 모드에서 활성화 해제; 해당 NVMe SQ가 더 이상 경쟁하지 않음 */
}

/*
 * blk_mq_tag_is_reserved(): 예약 태그 범위인지 검사한다.
 * NVMe 연결:
 *   - 예약 태그는 NVMe admin command나 flush 등 우선 처리가 필요한
 *     명령에 사용될 수 있다(추정).
 */
static inline bool blk_mq_tag_is_reserved(struct blk_mq_tags *tags,
					  unsigned int tag)
{
	return tag < tags->nr_reserved_tags;	/* 0..nr_reserved_tags-1는 NVMe admin/flush 등 우선 명령용(추정) */
}

static inline bool blk_mq_is_shared_tags(unsigned int flags)
{
	return flags & BLK_MQ_F_TAG_HCTX_SHARED;	/* 태그 풀 공유 여부: 여러 NVMe SQ가 하나의 CID pool 공유 */
}

/*
 * blk_mq_tags_from_data(): 할당 데이터로부터 사용할 tag set을 선택한다.
 * NVMe 연결:
 *   - 스케줄러 태그(sched_tags)를 사용하면 NVMe 드라이버 tag 대신
 *     스케줄러가 중간에서 CID 후보를 관리한다(예: kyber, mq-deadline).
 *   - 일반적으로는 hctx->tags가 NVMe SQ의 CID 풀에 해당한다.
 */
static inline struct blk_mq_tags *blk_mq_tags_from_data(struct blk_mq_alloc_data *data)
{
	if (data->rq_flags & RQF_SCHED_TAGS)
		return data->hctx->sched_tags;	/* 스케줄러 중간 tag pool 사용; NVMe CID는 dispatch 직전에 확보(추정) */
	return data->hctx->tags;		/* NVMe 드라이버 직접 CID pool 사용 */
}

/*
 * blk_mq_hctx_stopped(): hctx(SQ/CQ)가 중지되었는지 검사한다.
 * NVMe 연결:
 *   - NVMe 컨트롤러 reset, suspend, queue timeout 등으로 SQ 제출이
 *     중단되면 이 함수가 true를 반환해 새 명령 dispatch를 막는다.
 *   - likely() fast path는 정상 동작 중인 NVMe SQ가 대부분이라는 가정.
 */
static inline bool blk_mq_hctx_stopped(struct blk_mq_hw_ctx *hctx)
{
	/* Fast path: hardware queue is not stopped most of the time. */
	if (likely(!test_bit(BLK_MQ_S_STOPPED, &hctx->state)))
		return false;		/* 정상 NVMe SQ: doorbell 제출 계속 허용(추정) */

	/*
	 * This barrier is used to order adding of dispatch list before and
	 * the test of BLK_MQ_S_STOPPED below. Pairs with the memory barrier
	 * in blk_mq_start_stopped_hw_queue() so that dispatch code could
	 * either see BLK_MQ_S_STOPPED is cleared or dispatch list is not
	 * empty to avoid missing dispatching requests.
	 */
	smp_mb();			/* NVMe SQ stop/start 사이의 명령/완료 재정렬 방지; doorbell 전 상태 일관성 확보(추정) */

	return test_bit(BLK_MQ_S_STOPPED, &hctx->state);	/* NVMe controller reset/timeout/suspend로 SQ 제출 차단(추정) */
}

/*
 * blk_mq_hw_queue_mapped(): 이 hctx가 유효하게 매핑되었는지 검사한다.
 * NVMe 연결:
 *   - nr_ctx > 0: 이 SQ로 연결된 software queue(ctx)가 존재.
 *   - hctx->tags: CID 풀이 할당되어 있음.
 *   - 둘 중 하나라도 없으면 해당 nvme_queue로 I/O를 볼낼 수 없다.
 */
static inline bool blk_mq_hw_queue_mapped(struct blk_mq_hw_ctx *hctx)
{
	return hctx->nr_ctx && hctx->tags;	/* NVMe SQ에 ctx 연결 && CID pool 존재; 둘 다 필요 */
}

void blk_mq_in_driver_rw(struct block_device *part, unsigned int inflight[2]);	/* in-flight read/write 카운트; NVMe 성능 모니터링(추정) */

/*
 * blk_mq_put_dispatch_budget() / blk_mq_get_dispatch_budget():
 * dispatch 시점에 드라이버 수준 예산을 관리한다.
 * NVMe 연결:
 *   - 일부 NVMe 드라이버는 동시 inflight 명령 수를 제한하기 위해
 *     budget을 둔다. 예산 획득 실패 시 dispatch를 미룬다.
 *   - set_rq_budget_token/get_rq_budget_token은 request에 예산 정보를
 *     저장해 nvme_queue_rq 이후에도 추적할 수 있게 한다.
 */
static inline void blk_mq_put_dispatch_budget(struct request_queue *q,
					      int budget_token)
{
	if (q->mq_ops->put_budget)
		q->mq_ops->put_budget(q, budget_token);		/* NVMe 드라이버가 inflight 예산 회수; queue depth 제어(추정) */
}

static inline int blk_mq_get_dispatch_budget(struct request_queue *q)
{
	if (q->mq_ops->get_budget)
		return q->mq_ops->get_budget(q);			/* NVMe 드라이버에게 inflight 슬롯 요청; 실패 시 dispatch 보류(추정) */
	return 0;							/* budget 미사용 드라이버: NVMe 자체 queue depth로 제한(추정) */
}

static inline void blk_mq_set_rq_budget_token(struct request *rq, int token)
{
	if (token < 0)
		return;							/* 유효하지 않은 budget token 무시 */

	if (rq->q->mq_ops->set_rq_budget_token)
		rq->q->mq_ops->set_rq_budget_token(rq, token);	/* request에 NVMe 예산 token 기록; abort/timeout 시 회수용(추정) */
}

static inline int blk_mq_get_rq_budget_token(struct request *rq)
{
	if (rq->q->mq_ops->get_rq_budget_token)
		return rq->q->mq_ops->get_rq_budget_token(rq);	/* request에 저장된 NVMe 예산 token 조회; 완료 시 회수(추정) */
	return -1;							/* token 미사용 */
}

/*
 * active_requests 계열 함수: 공유 태그 환경에서 현재 hctx/SQ의 outstanding
 * 명령 수를 추적한다.
 * NVMe 연결:
 *   - nr_active는 해당 SQ에 아직 완료되지 않은 NVMe 명령 수와 대응된다.
 *   - shared tags 시 shared_tags 전체 카운트를 사용해 SQ 간 공정성을
 *     조정한다.
 */
static inline void __blk_mq_add_active_requests(struct blk_mq_hw_ctx *hctx,
						int val)
{
	if (blk_mq_is_shared_tags(hctx->flags))
		atomic_add(val, &hctx->queue->nr_active_requests_shared_tags);	/* 공유 CID pool outstanding += val; NVMe 전체 inflight 추적(추정) */
	else
		atomic_add(val, &hctx->nr_active);		/* per-SQ outstanding += val; NVMe SQ별 inflight 추적(추정) */
}

static inline void __blk_mq_inc_active_requests(struct blk_mq_hw_ctx *hctx)
{
	__blk_mq_add_active_requests(hctx, 1);		/* NVMe 명령 1개 dispatch; outstanding 증가(추정) */
}

static inline void __blk_mq_sub_active_requests(struct blk_mq_hw_ctx *hctx,
		int val)
{
	if (blk_mq_is_shared_tags(hctx->flags))
		atomic_sub(val, &hctx->queue->nr_active_requests_shared_tags);	/* 공유 pool 완료/abort로 outstanding -= val(추정) */
	else
		atomic_sub(val, &hctx->nr_active);		/* per-SQ 완료/abort로 outstanding -= val(추정) */
}

static inline void __blk_mq_dec_active_requests(struct blk_mq_hw_ctx *hctx)
{
	__blk_mq_sub_active_requests(hctx, 1);		/* NVMe 명령 1개 완료/abort; outstanding 감소(추정) */
}

static inline void blk_mq_add_active_requests(struct blk_mq_hw_ctx *hctx,
					      int val)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_add_active_requests(hctx, val);	/* shared tag 모드에서만 카운트 갱신(추정) */
}

static inline void blk_mq_inc_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_inc_active_requests(hctx);		/* shared tag 모드: NVMe 명령 제출 시 inflight 증가(추정) */
}

static inline void blk_mq_sub_active_requests(struct blk_mq_hw_ctx *hctx,
					      int val)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_sub_active_requests(hctx, val);	/* shared tag 모드: NVMe 완료/abort 시 inflight 감소(추정) */
}

static inline void blk_mq_dec_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_dec_active_requests(hctx);		/* shared tag 모드: NVMe 완료/abort 1건(추정) */
}

static inline int __blk_mq_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (blk_mq_is_shared_tags(hctx->flags))
		return atomic_read(&hctx->queue->nr_active_requests_shared_tags);	/* 공유 pool 기준 NVMe 전체 inflight(추정) */
	return atomic_read(&hctx->nr_active);		/* per-SQ NVMe inflight(추정) */
}

/*
 * __blk_mq_put_driver_tag(): request에 할당된 driver tag(CID)를 반환한다.
 * NVMe 연결:
 *   - 명령 완료 후 CID를 회수하여 재사용 가능하게 만든다.
 *   - rq->tag = BLK_MQ_NO_TAG로 초기화해 이 request가 더 이상 유효한
 *     SQ 엔트리를 가리키지 않음을 표시한다.
 */
static inline void __blk_mq_put_driver_tag(struct blk_mq_hw_ctx *hctx,
					   struct request *rq)
{
	blk_mq_dec_active_requests(hctx);	/* NVMe SQ inflight 감소; 완료/abort 직후(추정) */
	blk_mq_put_tag(hctx->tags, rq->mq_ctx, rq->tag);	/* CID 반환; 동일 SQ 내에서 재사용 가능(추정) */
	rq->tag = BLK_MQ_NO_TAG;		/* request가 유효한 NVMe SQ slot을 더 이상 가리키지 않음 */
}

static inline void blk_mq_put_driver_tag(struct request *rq)
{
	if (rq->tag == BLK_MQ_NO_TAG || rq->internal_tag == BLK_MQ_NO_TAG)
		return;					/* 이미 반환되었거나 낮은 태그 미할당; 중복 put 방지(추정) */

	__blk_mq_put_driver_tag(rq->mq_hctx, rq);	/* NVMe 명령 완료 -> CID 회수 경로(추정) */
}

bool __blk_mq_alloc_driver_tag(struct request *rq);	/* 실제 CID 할당 납부자; 실패 시 NVMe doorbell 불가(추정) */

/*
 * blk_mq_get_driver_tag(): request에 driver tag(CID)를 확보한다.
 * NVMe 연결:
 *   - dispatch 직전에 호출되어 실제 SQ 엔트리에 기록할 CID를 확보.
 *   - 실패하면 아직 SQ에 doorbell을 울릴 수 없으므로 dispatch를 보류.
 */
static inline bool blk_mq_get_driver_tag(struct request *rq)
{
	if (rq->tag == BLK_MQ_NO_TAG && !__blk_mq_alloc_driver_tag(rq))
		return false;				/* CID 부족: NVMe SQ 꽉 참 -> doorbell 보류(추정) */

	return true;					/* CID 확보: nvme_setup_cmd -> nvme_submit_cmd 가능(추정) */
}

static inline void blk_mq_clear_mq_map(struct blk_mq_queue_map *qmap)
{
	int cpu;

	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;			/* 모든 CPU를 NVMe SQ 0으로 fallback 매핑; 초기화/오류 복구 시(추정) */
}

/* Free all requests on the list */
static inline void blk_mq_free_requests(struct list_head *list)
{
	while (!list_empty(list)) {
		struct request *rq = list_entry_rq(list->next);

		list_del_init(&rq->queuelist);		/* dispatch/requeue list에서 제거 */
		blk_mq_free_request(rq);		/* request 해제; 아직 제출되지 않은 NVMe 명령 폐기(추정) */
	}
}

/*
 * For shared tag users, we track the number of currently active users
 * and attempt to provide a fair share of the tag depth for each of them.
 */

/*
 * hctx_may_queue(): 이 hctx(SQ)가 새로운 tag(CID)를 받을 수 있는지 검사한다.
 * NVMe 연결:
 *   - shared tag 모드에서 여러 nvme_queue가 하나의 tag pool을 공유할 때,
 *     활성 queue 수(active_queues)로 태그를 공정 분배한다.
 *   - 분배 공식: depth = max((tag_pool_depth + users - 1) / users, 4)
 *     -> 각 SQ가 최소 4개, 평균적으로 depth/users 개의 CID를 사용할 수 있음.
 *   - __blk_mq_active_requests(hctx) < depth 이면 새 명령 허용.
 */
static inline bool hctx_may_queue(struct blk_mq_hw_ctx *hctx,
				  struct sbitmap_queue *bt)
{
	unsigned int depth, users;

	if (!hctx || !(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED))
		return true;				/* 공유 tag 모드 아니면 제한 없음; dedicated NVMe SQ depth 사용(추정) */

	/*
	 * Don't try dividing an ant
	 */
	if (bt->sb.depth == 1)
		return true;				/* tag pool depth 1이면 분배 의미 없음; 단일 CID 순차 사용(추정) */

	if (blk_mq_is_shared_tags(hctx->flags)) {
		struct request_queue *q = hctx->queue;

		if (!test_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags))
			return true;			/* QUEUE_FLAG_HCTX_ACTIVE 미설정: 공유 tag 활성화 전/비활성 상태(추정) */
	} else {
		if (!test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return true;			/* 이 hctx가 shared tag 경쟁에 참여하지 않음(추정) */
	}

	users = READ_ONCE(hctx->tags->active_queues);	/* 현재 CID pool을 사용 중인 NVMe SQ 수(추정) */
	if (!users)
		return true;				/* 경쟁자 없음: 전체 CID pool 사용 가능(추정) */

	/*
	 * Allow at least some tags
	 */
	depth = max((bt->sb.depth + users - 1) / users, 4U);	/* SQ당 CID quota 계산; 최소 4개 보장(추정) */
	return __blk_mq_active_requests(hctx) < depth;	/* 이 NVMe SQ의 outstanding이 quota 미만이면 새 명령 허용(추정) */
}

/* run the code block in @dispatch_ops with rcu/srcu read lock held */
#define __blk_mq_run_dispatch_ops(q, check_sleep, dispatch_ops)	\
		do {								\
			if ((q)->tag_set->flags & BLK_MQ_F_BLOCKING) {		\
				struct blk_mq_tag_set *__tag_set = (q)->tag_set;	\
				int srcu_idx;						\
											\
				might_sleep_if(check_sleep);				\
				srcu_idx = srcu_read_lock(__tag_set->srcu);		\
				(dispatch_ops);						\
				srcu_read_unlock(__tag_set->srcu, srcu_idx);		\
			} else {							\
				rcu_read_lock();					\
				(dispatch_ops);						\
				rcu_read_unlock();					\
			}								\
		} while (0)								/* rcu/srcu로 NVMe tag_set/hctx 접근 보호; dispatch 임계영역(추정) */ \

#define blk_mq_run_dispatch_ops(q, dispatch_ops)		\
		__blk_mq_run_dispatch_ops(q, true, dispatch_ops)	/* NVMe dispatch 경로 진입 시 tag_set 생명주기 보호(추정) */

/*
 * blk_mq_can_poll(): 이 queue가 폴(poll) 모드를 지원하는지 검사한다.
 * NVMe 연결:
 *   - BLK_FEAT_POLL: 컨트롤러/드라이버가 폴 완료를 지원함.
 *   - HCTX_TYPE_POLL 큐가 존재: NVMe 폴용 SQ/CQ 쌍이 초기화되어 있음.
 *   - 둘 다 만족해야 submit_bio -> blk_mq_poll 경로로 진입 가능.
 */
static inline bool blk_mq_can_poll(struct request_queue *q)
{
	return (q->limits.features & BLK_FEAT_POLL) &&		/* NVMe controller가 폴 모드 지원(추정) */
		q->tag_set->map[HCTX_TYPE_POLL].nr_queues;	/* 폴 전용 NVMe SQ/CQ 쌍이 할당되어 있음(추정) */
}

#endif

/*
 * NVMe 관점 핵심 요약
 *
 * - 본 파일은 block layer multi-queue의 핵심 헤더로, bio 제출부터
 *   hardware queue(SQ/CQ) 선택, tag(CID) 할당, dispatch budget,
 *   폴 완료 여부를 결정하는 자료구조/함수를 담고 있다.
 * - 전형적 NVMe I/O 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_map_queue ->
 *   blk_mq_get_driver_tag(CID) -> nvme_queue_rq -> nvme_setup_cmd ->
 *   nvme_submit_cmd(doorbell) -> NVMe 컨트롤러가 SQ 소비.
 * - blk_mq_hw_ctx는 NVMe 드라이버의 nvme_queue(Submission Queue/
 *   Completion Queue 쌍)에 대응되며, tag bitmap은 SQ 내에서 고유해야
 *   하는 CID(Command Identifier) 풀에 대응된다.
 * - hctx_may_queue(), blk_mq_tag_busy/idle, active_requests 등은
 *   여러 SQ가 tag pool을 공유할 때 공정성과 동시 inflight 명령 수를
 *   제어해 NVMe 성능 예측 가능성을 높인다.
 * - blk_mq_can_poll()과 HCTX_TYPE_POLL은 NVMe 폧링 I/O 경로의
 *   진입 조건이며, 이를 통해 인터럽트 없는 저지연 완료가 가능하다.
 */
