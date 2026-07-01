// SPDX-License-Identifier: GPL-2.0
/*
 *  MQ Deadline i/o scheduler - adaptation of the legacy deadline scheduler,
 *  for the blk-mq scheduling framework
 *
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 */
/*
 * NVMe SSD 관점에서 본 block/mq-deadline.c
 *
 * blk-mq용 deadline I/O 스케줄러이다. NVMe SSD로 들어온 bio는
 * blk_mq_submit_bio -> blk_mq_get_request -> 본 스케줄러를 거쳐
 * deadline/FIFO/starvation/ioprio 순으로 정렬되고, blk_mq_dispatch_rq_list
 * -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe SQ에
 * CID를 할당해 명령이 전송된다(추정). NVMe 컨트롤러가 플래시 명령으로
 * 변환하기 직전 마지막 소프트웨어 재배치 계층이며
 * block/elevator.c, block/blk-mq-sched.c, block/blk-mq.c와 연결된다.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"

/*
 * See Documentation/block/deadline-iosched.rst
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
/* NVMe 관점: read_expire는 NVMe read의 최대 대기 시간, write_expire는 NVMe write의 최대 대기 시간(추정). */
/*
 * Time after which to dispatch lower priority requests even if higher
 * priority requests are pending.
 */
static const int prio_aging_expire = 10 * HZ;
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

/* NVMe request의 데이터 방향을 표현한다. READ/WRITE는 blk-mq 상수를 그대로 사용. */
enum dd_data_dir {
	DD_READ		= READ,
	DD_WRITE	= WRITE,
};

enum { DD_DIR_COUNT = 2 };

/* ioprio 기반으로 NVMe SQ에 들어갈 명령의 우선순위군을 나눈다. */
enum dd_prio {
	DD_RT_PRIO	= 0,
	DD_BE_PRIO	= 1,
	DD_IDLE_PRIO	= 2,
	DD_PRIO_MAX	= 2,
};

enum { DD_PRIO_COUNT = 3 };

/*
 * I/O statistics per I/O priority. It is fine if these counters overflow.
 * What matters is that these counters are at least as wide as
 * log2(max_outstanding_requests).
 */
/*
 * NVMe 관점: 우선순위별로 NVMe SQ에 삽입/병합/dispatch/완료된 request
 * 수를 추적한다. 이 값들은 NVMe CQ 완료 시점과 비교해 큐에 남아있는
 * outstanding 명령 수를 파악하는 데 사용된다(추정).
 */
struct io_stats_per_prio {
	/* NVMe SQ 삽입 직전까지 스케줄러에 들어온 request 수 */
	uint32_t inserted;
	/* 인접 NVMe LBA로 병합되어 SQ 점유를 줄인 횟수 */
	uint32_t merged;
	/* blk_mq_hw_ctx를 통해 NVMe 드라이버로 넘어간 횟수 */
	uint32_t dispatched;
	/* NVMe CQ 완료 후 blk-mq가 회수한 횟수 */
	atomic_t completed;
};

/*
 * Deadline scheduler data per I/O priority (enum dd_prio). Requests are
 * present on both sort_list[] and fifo_list[].
 */
/*
 * NVMe 관점: 동일 우선순위의 NVMe request를 LBA 순서(sort_list)와
 * 도착 순서(fifo_list) 두 축으로 관리한다. latest_pos는 NVMe 순차 IO
 * 탐색을 위한 기준점이며, stats는 해당 우선순위의 SQ/CQ 흐름을
 * 추적한다(추정).
 */
struct dd_per_prio {
	/* LBA 기준으로 정렬된 NVMe read/write request 트리 */
	struct rb_root sort_list[DD_DIR_COUNT];
	/* 도착 순서(FIFO)로 관리되는 NVMe request 리스트 */
	struct list_head fifo_list[DD_DIR_COUNT];
	/* Position of the most recently dispatched request. */
	/* 마지막 dispatch LBA, NVMe 순차 IO 스캔 재개점 */
	sector_t latest_pos[DD_DIR_COUNT];
	/* 우선순위별 NVMe IO 통계 */
	struct io_stats_per_prio stats;
};

/*
 * NVMe 관점 deadline_data:
 *  - dispatch: 즉시 NVMe 하드웨어 큐로 날려볼 request 임시 대기열
 *  - per_prio[DD_RT_PRIO/DD_BE_PRIO/DD_IDLE_PRIO]: ioprio에 따른 NVMe
 *    명령 우선순위별 정렬/FIFO 상태
 *  - fifo_expire[]: read/write 각각의 NVMe deadline 만료 시간
 *  - fifo_batch: 한 번에 연속 dispatch할 NVMe request 수 (SQ batching)
 *  - writes_starved: read가 write를 최대 몇 번 선점할 수 있는지 (NVMe
 *    write buffer flush starvation 완화)
 *  - front_merges: 새 bio를 기존 request 앞쪽 LBA에 병합해 NVMe
 *    PRP/SGL 개수를 줄일지 여부
 *  - prio_aging_expire: 낮은 우선순위 request가 NVMe SQ에 들어가기
 *    전 최대 대기 시간
 *  - lock: 멀티 큐 NVMe 환경에서 공유 deadline 상태를 보호하는 단일
 *    스핀락
 */
struct deadline_data {
	/*
	 * run time data
	 */

	/* 즉시 NVMe 하드웨어 큐로 날려볼 request 임시 대기열 */
	struct list_head dispatch;
	/* RT/BE/IDLE 우선순위별 NVMe request 정렬 상태 */
	struct dd_per_prio per_prio[DD_PRIO_COUNT];

	/* Data direction of latest dispatched request. */
	/* 마지막에 dispatch한 방향(read/write), NVMe SQ batching에 활용 */
	enum dd_data_dir last_dir;
	/* 현재 연속 처리 중인 NVMe request 수 (fifo_batch 한도 내) */
	unsigned int batching;		/* number of sequential requests made */
	/* write가 read에 밀린 횟수, NVMe latency 균형 제어용 */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	/* read/write request별 NVMe deadline 만료 시간(jiffies) */
	int fifo_expire[DD_DIR_COUNT];
	/* 한 번에 연속 dispatch할 NVMe request 수 */
	int fifo_batch;
	/* read가 write를 최대 몇 번까지 선점 가능한지 (NVMe QoS) */
	int writes_starved;
	/* 새 bio를 기존 request 앞쪽 LBA에 병합할지 (NVMe PRP/SGL 최적화) */
	int front_merges;
	/* 낮은 우선순위 request의 최대 대기 시간 (NVMe starvation 방지) */
	int prio_aging_expire;

	/* deadline_data 공유 상태 보호 (NVMe 멀티 큐에서 단일 락) */
	spinlock_t lock;
};

/* NVMe 관점: bio/request의 ioprio class를 mq-deadline 낶 prio로 매핑해
 * NVMe SQ 진입 우선순위를 결정한다. IOPRIO_CLASS_RT는 DD_RT_PRIO로
 * 가장 먼저 dispatch된다(추정). */
/* Maps an I/O priority class to a deadline scheduler priority. */
static const enum dd_prio ioprio_class_to_prio[] = {
	[IOPRIO_CLASS_NONE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_RT]	= DD_RT_PRIO,
	[IOPRIO_CLASS_BE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_IDLE]	= DD_IDLE_PRIO,
};

/*
 * NVMe request의 LBA/방향에 해당하는 rbtree 루트를 반환한다.
 * 호출 경로(추정): dd_insert_request -> deadline_add_rq_rb -> deadline_rb_root
 */
static inline struct rb_root *
deadline_rb_root(struct dd_per_prio *per_prio, struct request *rq)
{
	return &per_prio->sort_list[rq_data_dir(rq)]; /* rq_data_dir(rq)가 READ/WRITE를 반환하며, NVMe read/write SQ 선택과 무관하지만 동일 방향 request끼리 LBA 인접 병합(PRP/SGL 최적화)을 위해 분리한다. */
}

/*
 * Returns the I/O priority class (IOPRIO_CLASS_*) that has been assigned to a
 * request.
 */
static u8 dd_rq_ioclass(struct request *rq)
{
	return IOPRIO_PRIO_CLASS(req_get_ioprio(rq)); /* req_get_ioprio()는 bio->bi_ioprio에서 ioprio를 상속받은 값으로, NVMe SQ 우선순위 분리(dd_prio)의 입력값이다(추정). */
}

/*
 * Return the first request for which blk_rq_pos() >= @pos.
 */
/*
 * NVMe 관점: 주어진 LBA(@pos) 이후의 첫 NVMe request를 rbtree에서
 * 찾는다. 이는 NVMe 순차 IO 스트림을 이어가기 위한 스캔에 사용된다.
 */
static inline struct request *deadline_from_pos(struct dd_per_prio *per_prio,
				enum dd_data_dir data_dir, sector_t pos)
{
	struct rb_node *node = per_prio->sort_list[data_dir].rb_node; /* NVMe LBA 순회를 위한 rbtree 루트; NVMe 명령의 SLBA 기준으로 정렬되어 있어 순차 IO 흐름을 재개할 수 있다(추정). */
	struct request *rq, *res = NULL;

	while (node) {
		rq = rb_entry_rq(node);
		if (blk_rq_pos(rq) >= pos) { /* blk_rq_pos(rq)는 NVMe 명령의 시작 LBA(섹터 단위)와 대응되며, 순차 IO를 판단해 SQ batching에 활용된다(추정). */
			res = rq;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return res;
}

static void
/*
 * NVMe request를 LBA 기준 rbtree에 삽입한다.
 * 호출 경로(추정): dd_insert_request -> deadline_add_rq_rb
 */
deadline_add_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(per_prio, rq);

	elv_rb_add(root, rq); /* elv_rb_add()가 LBA 순서로 rbtree에 삽입; 인접 LBA는 나중에 front/back merge를 통해 NVMe PRP/SGL 항목을 줄이는 후보가 된다(추정). */
}

static inline void
/* NVMe request를 LBA 기준 rbtree에서 제거한다. */
deadline_del_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	elv_rb_del(deadline_rb_root(per_prio, rq), rq);
}

/*
 * remove rq from rbtree and fifo.
 */
/*
 * NVMe 관점: NVMe SQ 진입이 취소되거나 dispatch 직전에 request를
 * rbtree, fifo, merge hash에서 모두 제거한다. q->last_merge도
 * 갱신해 후속 merge 후보가 잘못된 request를 가리키지 않도록 한다.
 */
static void deadline_remove_request(struct request_queue *q,
				    struct dd_per_prio *per_prio,
				    struct request *rq)
{
	list_del_init(&rq->queuelist); /* FIFO 리스트에서 제거; 이 request는 곧 NVMe SQ에 들어가거나 폐기된다. */

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		deadline_del_rq_rb(per_prio, rq);

	elv_rqhash_del(q, rq); /* merge hash에서 제거; 이미 dispatch 예정이면 후속 bio 병합 대상에서 제외한다. */
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void dd_request_merged(struct request_queue *q, struct request *req,
/*
 * NVMe request가 front merge된 후 rbtree상의 위치를 재조정한다.
 * front merge는 요청의 시작 LBA가 앞쪽으로 확장되므로 rbtree
 * 정렬 순서가 달라질 수 있다.
 */
			      enum elv_merge type)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = dd_rq_ioclass(req);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];
	struct dd_per_prio *per_prio = &dd->per_prio[prio];

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(per_prio, req), req);
		deadline_add_rq_rb(per_prio, req); /* front merge 후 시작 LBA가 감소했으므로 rbtree에서 재삽입해 NVMe SQ dispatch 시 정확한 순차 후보를 찾을 수 있게 한다. */
	}
}

/*
 * Callback function that is invoked after @next has been merged into @req.
 */
/*
 * NVMe 관점: @next가 @req로 병합되면 NVMe SQ상에서 하나의 CID로
 * 처리되므로 @next는 제거한다. FIFO상에서 더 급한 deadline을
 * 물려받아 NVMe latency 약속을 유지한다.
 */
static void dd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = dd_rq_ioclass(next);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];

	lockdep_assert_held(&dd->lock);

	dd->per_prio[prio].stats.merged++; /* 병합 횟수 증가; NVMe SQ에 필요한 CID/tag 개수가 실제보다 적음을 기록한다. */

	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, &dd->per_prio[prio], next);
}

/*
 * move an entry to dispatch queue
 */
/*
 * NVMe 관점: 선택된 request를 rbtree/fifo에서 제거하여 곧
 * blk_mq_hw_ctx -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로
 * 전달될 준비를 한다(추정).
 */
static void
deadline_move_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      struct request *rq)
{
	/*
	 * take it off the sort and fifo list
	 */
	deadline_remove_request(rq->q, per_prio, rq);
}

/* Number of requests queued for a given priority level. */
/* NVMe 관점: 우선순위별로 NVMe SQ에 들어가기 전 대기 중인 request 수를 추정한다. */
static u32 dd_queued(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats;

	lockdep_assert_held(&dd->lock);

	return stats->inserted - atomic_read(&stats->completed); /* inserted - completed가 음수가 되면 NVMe CQ 완료가 SQ 삽입보다 많았음을 의미하며, 통계 오버플로우 가능성을 시사한다(추정). */
}

/*
 * deadline_check_fifo returns true if and only if there are expired requests
 * in the FIFO list. Requires !list_empty(&dd->fifo_list[data_dir]).
 */
/*
 * NVMe 관점: FIFO 리스트의 선두 request가 deadline(read_expire 또는
 * write_expire)을 넘었는지 확인한다. 만료되면 NVMe SQ에 넣기 전
 * latency 약속을 지키기 위해 우선 dispatch해야 한다.
 */
static inline bool deadline_check_fifo(struct dd_per_prio *per_prio,
				       enum dd_data_dir data_dir)
{
	struct request *rq = rq_entry_fifo(per_prio->fifo_list[data_dir].next);

	return time_is_before_eq_jiffies((unsigned long)rq->fifo_time); /* time_is_before_eq_jiffies()가 true면 해당 NVMe request가 read_expire/write_expire을 초과해 SQ로 즉시 날려야 한다. */
}

/*
 * For the specified data direction, return the next request to
 * dispatch using arrival ordered lists.
 */
/* NVMe 관점: 도착 순서(FIFO) 기준으로 다음 dispatch할 NVMe request를 반환한다. */
static struct request *
deadline_fifo_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	if (list_empty(&per_prio->fifo_list[data_dir]))
		return NULL;

	return rq_entry_fifo(per_prio->fifo_list[data_dir].next);
}

/*
 * For the specified data direction, return the next request to
 * dispatch using sector position sorted lists.
 */
/* NVMe 관점: LBA 순서 기준으로 다음 NVMe request를 반환해 순차 IO를 유도한다. */
static struct request *
deadline_next_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	return deadline_from_pos(per_prio, data_dir,
				 per_prio->latest_pos[data_dir]);
}

/*
 * Returns true if and only if @rq started after @latest_start where
 * @latest_start is in jiffies.
 */
/* NVMe 관점: request가 @latest_start 시점 이후에 도착했는지 확인해 aging dispatch에 활용한다. */
static bool started_after(struct deadline_data *dd, struct request *rq,
			  unsigned long latest_start)
{
	unsigned long start_time = (unsigned long)rq->fifo_time;

	start_time -= dd->fifo_expire[rq_data_dir(rq)]; /* fifo_time에서 만료 시간을 빼면 실제 NVMe SQ 삽입(도착) 시점이 된다. */

	return time_after(start_time, latest_start);
}

static struct request *dd_start_request(struct deadline_data *dd,
/*
 * NVMe request가 실제로 dispatch되기 직전 상태를 기록한다.
 * - latest_pos: NVMe 순차 IO 기준점 갱신
 * - stats.dispatched: NVMe 드라이버로 넘어간 횟수 증가
 * - RQF_STARTED: blk-mq에서 request 전송 시작 플래그 설정
 */
					enum dd_data_dir data_dir,
					struct request *rq)
{
	u8 ioprio_class = dd_rq_ioclass(rq);
	enum dd_prio prio = ioprio_class_to_prio[ioprio_class];

	dd->per_prio[prio].latest_pos[data_dir] = blk_rq_pos(rq); /* NVMe SQ batching을 위한 다음 순차 스캔 기준점으로 현재 request의 시작 LBA를 저장한다(추정). */
	dd->per_prio[prio].stats.dispatched++; /* NVMe 드라이버가 소유 중인 request 수를 증가; dd_owned_by_driver에서 outstanding NVMe 명령 수 추정에 사용된다(추정). */
	rq->rq_flags |= RQF_STARTED; /* RQF_STARTED는 blk-mq가 request를 전송 중임을 표시; NVMe doorbell 직전에 설정되므로 timeout/abort 판단의 시작점이 된다(추정). */
	return rq;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc and with a start time <= @latest_start.
 */
/*
 * NVMe 관점: NVMe SQ에 들어갈 다음 request를 선택하는 핵심 함수.
 * 1) 동일 방향의 순차 batch가 fifo_batch 미만이면 계속 이어간다.
 * 2) 그렇지 않으면 read/write 방향과 starvation 정책을 따져 선택.
 * 3) deadline이 만료되면 FIFO 순서를, 아니면 LBA 순서를 우선.
 * 4) @latest_start보다 늦게 도착한 request는 dispatch하지 않음.
 *
 * 호출 경로(추정):
 * dd_dispatch_request -> __dd_dispatch_request ->
 *   (반환된 rq) -> blk_mq_dispatch_rq_list -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell)
 */
static struct request *__dd_dispatch_request(struct deadline_data *dd,
					     struct dd_per_prio *per_prio,
					     unsigned long latest_start)
{
	struct request *rq, *next_rq;
	enum dd_data_dir data_dir;

	lockdep_assert_held(&dd->lock);

	/*
	 * batches are currently reads XOR writes
	 */
	rq = deadline_next_request(dd, per_prio, dd->last_dir); /* NVMe 관점: 아직 현재 batch가 fifo_batch 미만이면 같은 방향의 순차 NVMe IO를 이어간다. */
	if (rq && dd->batching < dd->fifo_batch) {
		/* we have a next request and are still entitled to batch */
		data_dir = rq_data_dir(rq); /* 동일 NVMe SQ 방향(READ/WRITE) 내에서 fifo_batch 한도로 연속 doorbell batching을 유지한다(추정). */
		goto dispatch_request;
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	/* NVMe 관점: batch가 끝났으면 read/write 방향과 starvation 정책으로 NVMe 명령 방향을 선택한다. */
	if (!list_empty(&per_prio->fifo_list[DD_READ])) {
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_READ]));

		if (deadline_fifo_request(dd, per_prio, DD_WRITE) &&
		    (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = DD_READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (!list_empty(&per_prio->fifo_list[DD_WRITE])) { /* NVMe 관점: read가 없거나 write가 starvation 한계에 도달하면 NVMe write를 처리한다. */
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_WRITE]));

		dd->starved = 0; /* starvation 카운터 리셋; NVMe write buffer flush 기회를 보장한다. */

		data_dir = DD_WRITE;

		goto dispatch_find_request;
	}

	return NULL;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	/* NVMe 관점: deadline 만료나 연속 LBA 소진 시 FIFO 순서로 NVMe SQ에 넣을 후보를 찾는다. */
	next_rq = deadline_next_request(dd, per_prio, data_dir);
	if (deadline_check_fifo(per_prio, data_dir) || !next_rq) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = deadline_fifo_request(dd, per_prio, data_dir); /* NVMe 관점: deadline이 만료되었거나 역방향/끝지점이면 FIFO 기준으로 NVMe request를 dispatch한다. */
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = next_rq; /* NVMe 관점: 같은 방향의 순차 IO가 있으면 LBA 순서로 NVMe request를 이어간다. */
	}

	if (!rq)
		return NULL;

	dd->last_dir = data_dir; /* 선택된 방향을 기록; 다음 dispatch 선택 시 batching/방향 판단에 사용된다. */
	dd->batching = 0; /* 새로운 batch 시작; NVMe SQ doorbell batch 카운터를 0으로 초기화한다. */

dispatch_request:
	if (started_after(dd, rq, latest_start))
		return NULL;

	/*
	 * rq is the selected appropriate request.
	 */
	/* NVMe 관점: 선택된 request가 NVMe SQ에 들어갈 최종 후보이다. */
	dd->batching++;
	deadline_move_request(dd, per_prio, rq);
	return dd_start_request(dd, data_dir, rq);
}

/*
 * Check whether there are any requests with priority other than DD_RT_PRIO
 * that were inserted more than prio_aging_expire jiffies ago.
 */
/*
 * NVMe 관점: RT_PRIO가 지속적으로 점유하면 BE/IDLE prio의 NVMe
 * request가 영구히 굶는 것을 방지하기 위해, aging된 낮은 우선순위
 * request라도 일정 시간이 지나면 강제 dispatch한다.
 */
static struct request *dd_dispatch_prio_aged_requests(struct deadline_data *dd,
						      unsigned long now)
{
	struct request *rq;
	enum dd_prio prio;
	int prio_cnt;

	lockdep_assert_held(&dd->lock);

	/* 활성 우선순위가 2개 이상일 때만 aging dispatch를 검사; RT만 있으면 BE/IDLE이 굶을 이유가 없으므로 NVMe SQ 우선순위 분리가 의미 있게 작동한다(추정). */
	prio_cnt = !!dd_queued(dd, DD_RT_PRIO) + !!dd_queued(dd, DD_BE_PRIO) +
		   !!dd_queued(dd, DD_IDLE_PRIO);
	if (prio_cnt < 2)
		return NULL;

	for (prio = DD_BE_PRIO; prio <= DD_PRIO_MAX; prio++) {
	/* now - prio_aging_expire보다 늦게 도착한 request는 제외해 오래된 BE/IDLE request만 NVMe SQ로 긴급 dispatch한다. */
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio],
					   now - dd->prio_aging_expire);
		if (rq)
			return rq;
	}

	return NULL;
}

/*
 * Called from blk_mq_run_hw_queue() -> __blk_mq_sched_dispatch_requests().
 *
 * One confusing aspect here is that we get called for a specific
 * hardware queue, but we may return a request that is for a
 * different hardware queue. This is because mq-deadline has shared
 * state for all hardware queues, in terms of sorting, FIFOs, etc.
 */
/*
 * NVMe 관점: blk-mq 하드웨어 큐(blk_mq_hw_ctx)별로 호출되지만
 * mq-deadline은 request_queue 전체에 대해 단일 deadline_data를
 * 공유한다. 따라서 여러 NVMe 큐(SQ)가 있어도 스케줄링 정책은
 * 큐 단위로 통일되어 적용된다(추정). 먼저 예비 dispatch 리스트를
 * 처리하고, aging 우선순위를 확인한 뒤, RT -> BE -> IDLE 순으로
 * request를 선택해 반환한다. 반환된 request는 blk-mq를 통해
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)로 전달된다(추정).
 */
static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	const unsigned long now = jiffies;
	struct request *rq;
	enum dd_prio prio;

	spin_lock(&dd->lock); /* NVMe 멀티 큐 환경에서 공유 deadline_data를 보호; hctx마다 별도 nvme_queue_rq()가 실행되므로 경합 가능성이 있다(추정). */

	if (!list_empty(&dd->dispatch)) { /* NVMe 관점: 이미 곧바로 날려볼로 예약된 request가 있으면 먼저 처리 */
		rq = list_first_entry(&dd->dispatch, struct request, queuelist);
		list_del_init(&rq->queuelist);
		dd_start_request(dd, rq_data_dir(rq), rq);
		goto unlock;
	}

	rq = dd_dispatch_prio_aged_requests(dd, now); /* NVMe 관점: 낮은 우선순위 request가 너무 오래 대기했는지 확인 */
	if (rq)
		goto unlock;

	/*
	 * Next, dispatch requests in priority order. Ignore lower priority
	 * requests if any higher priority requests are pending.
	 */
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio], now);
		if (rq || dd_queued(dd, prio))
			break;
	}

unlock:
	spin_unlock(&dd->lock);

	return rq;
}

static void dd_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
/*
 * NVMe 관점: 동기식 read가 아닌 쓰기/비동기 IO에 대해 async_depth를
 * shallow_depth로 제한한다. 이는 NVMe SSD의 비동기 SQ 항목 수를
 * 적절히 조절해 write buffer를 보호하기 위함이다(추정).
 */
{
	if (!blk_mq_is_sync_read(opf))
		data->shallow_depth = data->q->async_depth; /* q->async_depth를 shallow_depth로 설정; NVMe SQ의 in-flight write/비동기 명령 수를 제한해 SQ full이나 buffer overflow를 완화한다(추정). */
}

/* Called by blk_mq_init_sched() and blk_mq_update_nr_requests(). */
/* NVMe 관점: q->async_depth가 바뀌면 NVMe SQ의 최소 shallow depth도 재설정한다. */
static void dd_depth_updated(struct request_queue *q)
{
	blk_mq_set_min_shallow_depth(q, q->async_depth); /* blk_mq_set_min_shallow_depth()는 NVMe tag set(sbitmap) 할당 시 사용 가능한 최대 NVMe CID/tag 수의 하한을 갱신한다(추정). */
}

static void dd_exit_sched(struct elevator_queue *e)
/*
 * 스케줄러 종료 시 각 우선순위의 fifo_list가 비어 있어야 하며,
 * 삽입/dispatch/완료 통계가 일치해야 한다. 불일치하면 NVMe SQ/CQ
 * 사이에 누락된 request가 존재할 수 있음을 의미한다(추정).
 */
{
	struct deadline_data *dd = e->elevator_data;
	enum dd_prio prio;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];
		const struct io_stats_per_prio *stats = &per_prio->stats;
		uint32_t queued;

		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_READ]));
		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_WRITE]));

		spin_lock(&dd->lock);
		queued = dd_queued(dd, prio);
		spin_unlock(&dd->lock);

		WARN_ONCE(queued != 0,
			  "statistics for priority %d: i %u m %u d %u c %u\n",
			  prio, stats->inserted, stats->merged,
			  stats->dispatched, atomic_read(&stats->completed));
	}

	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data).
 */
/*
 * NVMe 관점: request_queue에 대해 deadline_data를 할당하고,
 * RT/BE/IDLE 우선순위별 rbtree/fifo를 초기화한다. QUEUE_FLAG_SQ_SCHED
 * 플래그를 설정해 단일 스케줄러 인스턴스가 큐 전체를 관리하도록
 * 하며, async_depth를 q->nr_requests로 설정해 NVMe SQ에 발행 가능한
 * 비동기 request 수를 제한한다(추정).
 */
static int dd_init_sched(struct request_queue *q, struct elevator_queue *eq)
{
	struct deadline_data *dd;
	enum dd_prio prio;

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd)
		return -ENOMEM;

	eq->elevator_data = dd;

	INIT_LIST_HEAD(&dd->dispatch);
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];

		INIT_LIST_HEAD(&per_prio->fifo_list[DD_READ]);
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_WRITE]);
		per_prio->sort_list[DD_READ] = RB_ROOT;
		per_prio->sort_list[DD_WRITE] = RB_ROOT;
	}
	dd->fifo_expire[DD_READ] = read_expire;
	dd->fifo_expire[DD_WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->last_dir = DD_WRITE;
	dd->fifo_batch = fifo_batch;
	dd->prio_aging_expire = prio_aging_expire;
	spin_lock_init(&dd->lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q); /* QUEUE_FLAG_SQ_SCHED가 설정되면 NVMe 멀티 SQ에도 단일 deadline 상태를 공유하며, 스케줄러가 큐 전체의 dispatch 순서를 조율한다. */

	q->elevator = eq;
	q->async_depth = q->nr_requests;
	dd_depth_updated(q); /* q->async_depth를 q->nr_requests로 설정; NVMe SQ depth보다 작거나 같은 값으로 비동기 request 할당 한도를 제한한다(추정). */
	return 0;
}

/*
 * Try to merge @bio into an existing request. If @bio has been merged into
 * an existing request, store the pointer to that request into *@rq.
 */
/*
 * NVMe 관점: 새 bio를 기존 NVMe request의 앞쪽 LBA에 front merge할
 * 수 있는지 탐색한다. 성공하면 PRP/SGL 항목 수를 줄이고 NVMe SQ
 * 항목 수를 절약해 throughput을 높인다(추정).
 */
static int dd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = IOPRIO_PRIO_CLASS(bio->bi_ioprio);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];
	struct dd_per_prio *per_prio = &dd->per_prio[prio];
	sector_t sector = bio_end_sector(bio); /* bio_end_sector(bio)는 bio가 끝나는 섹터; front merge를 위해 bio 끝 섹터와 정확히 일치하는 기존 request의 시작 LBA를 탐색한다. */
	struct request *__rq;

	if (!dd->front_merges)
		return ELEVATOR_NO_MERGE;

	__rq = elv_rb_find(&per_prio->sort_list[bio_data_dir(bio)], sector); /* rbtree에서 sector 위치의 request를 검색; 발견되면 NVMe PRP/SGL 병합 후보가 될 수 있다(추정). */
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));

		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			if (blk_discard_mergable(__rq))
				return ELEVATOR_DISCARD_MERGE;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
/*
 * NVMe 관점: blk_mq_sched_try_merge()를 통해 bio와 일치하는 기존
 * NVMe request를 찾아 병합한다. 실패 시 새 request를 할당해야 하므로
 * 불필요한 NVMe CID 낭비를 막는 첫 관문이다(추정).
 */
static bool dd_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&dd->lock);
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free); /* blk_mq_sched_try_merge() 낶에서 front/back merge, discard merge를 시도; 성공 시 bio는 기존 request에 병합되어 NVMe SQ 항목 하나로 처리된다(추정). */
	spin_unlock(&dd->lock);

	if (free)
		blk_mq_free_request(free); /* 병합 과정에서 사용된 임시 request가 있으면 해제; CID/tag 할당 회수를 의미한다(추정). */

	return ret;
}

/*
 * add rq to rbtree and fifo
 */
/*
 * NVMe 관점: blk-mq로부터 전달받은 request를 우선순위/방향에 따라
 * LBA 기준 rbtree와 도착 순서 FIFO에 삽입한다. BLK_MQ_INSERT_AT_HEAD
 * 플래그가 있으면 예비 dispatch 리스트(dd->dispatch)에 즉시 넣어
 * 우선 처리되도록 한다. front merge 가능하면 hash에 추가해 후속
 * bio 병합을 가속화한다. fifo_time은 deadline을 계산하는 기준.
 *
 * 호출 경로(추정):
 * blk_mq_submit_bio -> blk_mq_get_request -> dd_insert_requests ->
 * dd_insert_request
 */
static void dd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			      blk_insert_t flags, struct list_head *free)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_data_dir data_dir = rq_data_dir(rq);
	u16 ioprio = req_get_ioprio(rq);
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	struct dd_per_prio *per_prio;
	enum dd_prio prio;

	lockdep_assert_held(&dd->lock);

	prio = ioprio_class_to_prio[ioprio_class]; /* ioprio_class -> dd_prio 매핑; NVMe SQ 우선순위 그룹 결정 */
	per_prio = &dd->per_prio[prio];
	if (!rq->elv.priv[0])
		per_prio->stats.inserted++; /* rq->elv.priv[0]이 NULL이면 처음 삽입되는 request이므로 통계 증가; 병합된 request는 이미 기존 request에 포함되어 NVMe SQ 항목으로 집계되므로 중복 삽입을 피한다(추정). */
	rq->elv.priv[0] = per_prio;

	if (blk_mq_sched_try_insert_merge(q, rq, free))
		return; /* blk_mq_sched_try_insert_merge()가 동일 방향/인접 LBA의 request와 병합; 성공하면 NVMe PRP/SGL이 확장되고 새 rq는 free 리스트로 넘어가 blk_mq_free_request()에서 CID/tag를 회수한다(추정). */

	trace_block_rq_insert(rq);

	if (flags & BLK_MQ_INSERT_AT_HEAD) {
		list_add(&rq->queuelist, &dd->dispatch); /* NVMe 관점: 재시작/우선 처리가 필요한 request를 일반 FIFO 대신 즉시 dispatch 리스트로 */
		rq->fifo_time = jiffies; /* 즉시 dispatch될 것이므로 deadline 만료 시점을 현재 jiffies로 설정해 dd_dispatch_request에서 먼저 선택되도록 유도한다. */
	} else {
		deadline_add_rq_rb(per_prio, rq);

		if (rq_mergeable(rq)) {
			elv_rqhash_add(q, rq); /* merge hash에 추가; 후속 bio가 동일/인접 LBA일 때 O(1)로 front/back merge 후보를 찾아 NVMe SQ 항목을 절약한다(추정). */
			if (!q->last_merge)
				q->last_merge = rq;
		}

		/*
		 * set expire time and add to fifo list
		 */
		rq->fifo_time = jiffies + dd->fifo_expire[data_dir]; /* NVMe read/write deadline 만료 시점을 기록; NVMe latency 약속을 지키기 위한 FIFO 우선 dispatch 판단 기준. */
		list_add_tail(&rq->queuelist, &per_prio->fifo_list[data_dir]);
	}
}

/*
 * Called from blk_mq_insert_request() or blk_mq_dispatch_list().
 */
/*
 * NVMe 관점: blk-mq가 request 리스트를 한 번에 넘기면 이 함수에서
 * lock을 잡고 각 request를 deadline 구조에 삽입한다. 이렇게 모아서
 * 삽입해야 멀티 큐 NVMe 환경에서 락 경합을 최소화할 수 있다(추정).
 */
static void dd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list,
			       blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	LIST_HEAD(free);

	spin_lock(&dd->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		dd_insert_request(hctx, rq, flags, &free);
	}
	spin_unlock(&dd->lock);

	blk_mq_free_requests(&free); /* 병합/삽입 과정에서 해제된 request들을 blk-mq에 반납; 이들의 NVMe CID/tag는 sbitmap으로 회수되어 재사용 가능해진다(추정). */
}

/* Callback from inside blk_mq_rq_ctx_init(). */
/* NVMe 관점: request가 스케줄러에 들어가기 전 elv.priv[0]를 초기화한다. */
static void dd_prepare_request(struct request *rq)
{
	rq->elv.priv[0] = NULL; /* NULL로 초기화; dd_insert_request에서 첫 삽입 여부와 dd_finish_request에서 bypass 여부를 판단하는 데 사용된다. */
}

/*
 * Callback from inside blk_mq_free_request().
 */
/*
 * NVMe 관점: NVMe CQ 완료 인터럽트가 처리된 후 blk-mq가 request를
 * 해제할 때 호출된다. rq->elv.priv[0]이 NULL이면 스케줄러를
 * 우회한 request(bypass insert)이므로 통계에서 제외한다.
 */
static void dd_finish_request(struct request *rq)
{
	struct dd_per_prio *per_prio = rq->elv.priv[0];

	/*
	 * The block layer core may call dd_finish_request() without having
	 * called dd_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (per_prio)
		atomic_inc(&per_prio->stats.completed); /* per_prio가 NULL이면 스케줄러를 거치지 않고 NVMe SQ로 직접 갔거나 abort/requeue로 인한 bypass이므로 completed 통계에서 제외한다. */
} /* atomic_inc()로 멀티 큐 NVMe CQ 완료 경로에서도 통계의 데이터 레이스 없이 완료 수를 증가; outstanding 명령 추적에 사용된다(추정). */

/* NVMe 관점: 지정한 우선순위에 아직 NVMe SQ에 들어가지 못한 request가 있는지 확인한다. */
static bool dd_has_work_for_prio(struct dd_per_prio *per_prio)
{
	return !list_empty_careful(&per_prio->fifo_list[DD_READ]) || /* list_empty_careful()은 lock 없이 읽기 위해 사용; NVMe SQ에 넣을 후보가 있는지 폴리하는 경량 검사이다(추정). */
		!list_empty_careful(&per_prio->fifo_list[DD_WRITE]);
}

/*
 * NVMe 관점: blk_mq_run_hw_queue()가 이 함수를 폴리하여 NVMe SQ에
 * 새로 넣을 request가 있는지 확인한다. dispatch 리스트나
 * RT/BE/IDLE prio 중 하나라도 작업이 있으면 true를 반환한다.
 */
static bool dd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	enum dd_prio prio;

	if (!list_empty_careful(&dd->dispatch))
		return true;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++)
		if (dd_has_work_for_prio(&dd->per_prio[prio]))
			return true;

	return false;
}

/*
 * sysfs parts below
 */
#define SHOW_INT(__FUNC, __VAR)						\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
									\
	return sysfs_emit(page, "%d\n", __VAR);				\
}
#define SHOW_JIFFIES(__FUNC, __VAR) SHOW_INT(__FUNC, jiffies_to_msecs(__VAR))
SHOW_JIFFIES(deadline_read_expire_show, dd->fifo_expire[DD_READ]);
SHOW_JIFFIES(deadline_write_expire_show, dd->fifo_expire[DD_WRITE]);
SHOW_JIFFIES(deadline_prio_aging_expire_show, dd->prio_aging_expire);
SHOW_INT(deadline_writes_starved_show, dd->writes_starved);
SHOW_INT(deadline_front_merges_show, dd->front_merges);
SHOW_INT(deadline_fifo_batch_show, dd->fifo_batch);
#undef SHOW_INT
#undef SHOW_JIFFIES

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data, __ret;						\
									\
	__ret = kstrtoint(page, 0, &__data);				\
	if (__ret < 0)							\
		return __ret;						\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = __CONV(__data);					\
	return count;							\
}
#define STORE_INT(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, )
#define STORE_JIFFIES(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, msecs_to_jiffies)
STORE_JIFFIES(deadline_read_expire_store, &dd->fifo_expire[DD_READ], 0, INT_MAX);
STORE_JIFFIES(deadline_write_expire_store, &dd->fifo_expire[DD_WRITE], 0, INT_MAX);
STORE_JIFFIES(deadline_prio_aging_expire_store, &dd->prio_aging_expire, 0, INT_MAX);
STORE_INT(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX);
STORE_INT(deadline_front_merges_store, &dd->front_merges, 0, 1);
STORE_INT(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX);
#undef STORE_FUNCTION
#undef STORE_INT
#undef STORE_JIFFIES

#define DD_ATTR(name) \
	__ATTR(name, 0644, deadline_##name##_show, deadline_##name##_store)

static const struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	DD_ATTR(prio_aging_expire),
	__ATTR_NULL
};

#ifdef CONFIG_BLK_DEBUG_FS
#define DEADLINE_DEBUGFS_DDIR_ATTRS(prio, data_dir, name)		\
static void *deadline_##name##_fifo_start(struct seq_file *m,		\
					  loff_t *pos)			\
	__acquires(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	spin_lock(&dd->lock);						\
	return seq_list_start(&per_prio->fifo_list[data_dir], *pos);	\
}									\
									\
static void *deadline_##name##_fifo_next(struct seq_file *m, void *v,	\
					 loff_t *pos)			\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	return seq_list_next(v, &per_prio->fifo_list[data_dir], pos);	\
}									\
									\
static void deadline_##name##_fifo_stop(struct seq_file *m, void *v)	\
	__releases(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_unlock(&dd->lock);						\
}									\
									\
static const struct seq_operations deadline_##name##_fifo_seq_ops = {	\
	.start	= deadline_##name##_fifo_start,				\
	.next	= deadline_##name##_fifo_next,				\
	.stop	= deadline_##name##_fifo_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int deadline_##name##_next_rq_show(void *data,			\
					  struct seq_file *m)		\
{									\
	struct request_queue *q = data;					\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
	struct request *rq;						\
									\
	rq = deadline_from_pos(per_prio, data_dir,			\
			       per_prio->latest_pos[data_dir]);		\
	if (rq)								\
		__blk_mq_debugfs_rq_show(m, rq);			\
	return 0;							\
}

DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_READ, read0);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_WRITE, write0);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_READ, read1);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_WRITE, write1);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_READ, read2);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_WRITE, write2);
#undef DEADLINE_DEBUGFS_DDIR_ATTRS

static int deadline_batching_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->batching);
	return 0;
}

static int deadline_starved_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->starved);
	return 0;
}

static int dd_queued_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;
	u32 rt, be, idle;

	spin_lock(&dd->lock);
	rt = dd_queued(dd, DD_RT_PRIO);
	be = dd_queued(dd, DD_BE_PRIO);
	idle = dd_queued(dd, DD_IDLE_PRIO);
	spin_unlock(&dd->lock);

	seq_printf(m, "%u %u %u\n", rt, be, idle);

	return 0;
}

/* Number of requests owned by the block driver for a given priority. */
static u32 dd_owned_by_driver(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats;

	lockdep_assert_held(&dd->lock);

	/* NVMe 관점: dispatched + merged는 NVMe 드라이버에 전달된 명령 수(merge로 인해 SQ 항목보다 많을 수 있음)이고, completed를 빼면 현재 NVMe 컨트롤러가 처리 중인 outstanding 명령 수의 추정치가 된다(추정). */
	return stats->dispatched + stats->merged -
		atomic_read(&stats->completed);
}

static int dd_owned_by_driver_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;
	u32 rt, be, idle;

	spin_lock(&dd->lock);
	rt = dd_owned_by_driver(dd, DD_RT_PRIO);
	be = dd_owned_by_driver(dd, DD_BE_PRIO);
	idle = dd_owned_by_driver(dd, DD_IDLE_PRIO);
	spin_unlock(&dd->lock);

	seq_printf(m, "%u %u %u\n", rt, be, idle);

	return 0;
}

static void *deadline_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&dd->lock)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_lock(&dd->lock);
	return seq_list_start(&dd->dispatch, *pos);
}

static void *deadline_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	return seq_list_next(v, &dd->dispatch, pos);
}

static void deadline_dispatch_stop(struct seq_file *m, void *v)
	__releases(&dd->lock)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_unlock(&dd->lock);
}

static const struct seq_operations deadline_dispatch_seq_ops = {
	.start	= deadline_dispatch_start,
	.next	= deadline_dispatch_next,
	.stop	= deadline_dispatch_stop,
	.show	= blk_mq_debugfs_rq_show,
};

#define DEADLINE_QUEUE_DDIR_ATTRS(name)					\
	{#name "_fifo_list", 0400,					\
			.seq_ops = &deadline_##name##_fifo_seq_ops}
#define DEADLINE_NEXT_RQ_ATTR(name)					\
	{#name "_next_rq", 0400, deadline_##name##_next_rq_show}
static const struct blk_mq_debugfs_attr deadline_queue_debugfs_attrs[] = {
	DEADLINE_QUEUE_DDIR_ATTRS(read0),
	DEADLINE_QUEUE_DDIR_ATTRS(write0),
	DEADLINE_QUEUE_DDIR_ATTRS(read1),
	DEADLINE_QUEUE_DDIR_ATTRS(write1),
	DEADLINE_QUEUE_DDIR_ATTRS(read2),
	DEADLINE_QUEUE_DDIR_ATTRS(write2),
	DEADLINE_NEXT_RQ_ATTR(read0),
	DEADLINE_NEXT_RQ_ATTR(write0),
	DEADLINE_NEXT_RQ_ATTR(read1),
	DEADLINE_NEXT_RQ_ATTR(write1),
	DEADLINE_NEXT_RQ_ATTR(read2),
	DEADLINE_NEXT_RQ_ATTR(write2),
	{"batching", 0400, deadline_batching_show},
	{"starved", 0400, deadline_starved_show},
	{"dispatch", 0400, .seq_ops = &deadline_dispatch_seq_ops},
	{"owned_by_driver", 0400, dd_owned_by_driver_show},
	{"queued", 0400, dd_queued_show},
	{},
};
#undef DEADLINE_QUEUE_DDIR_ATTRS
#endif

/*
 * NVMe 관점: mq-deadline 스케줄러를 blk-mq에 등록하는 구조체.
 * 각 ops는 NVMe request의 삽입(dispatch 전), dispatch(SQ 진행),
 * 완료(CQ 처리 후), merge, 초기화/종료 등을 담당한다(추정).
 */
static struct elevator_type mq_deadline = {
	.ops = {
		.depth_updated		= dd_depth_updated,
		.limit_depth		= dd_limit_depth,
		.insert_requests	= dd_insert_requests,
		.dispatch_request	= dd_dispatch_request,
		.prepare_request	= dd_prepare_request,
		.finish_request		= dd_finish_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge		= dd_bio_merge,
		.request_merge		= dd_request_merge,
		.requests_merged	= dd_merged_requests,
		.request_merged		= dd_request_merged,
		.has_work		= dd_has_work,
		.init_sched		= dd_init_sched,
		.exit_sched		= dd_exit_sched,
	},

#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = deadline_queue_debugfs_attrs,
#endif
	.elevator_attrs = deadline_attrs,
	.elevator_name = "mq-deadline",
	.elevator_alias = "deadline",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-deadline-iosched");

static int __init deadline_init(void)
{
	return elv_register(&mq_deadline);
}

static void __exit deadline_exit(void)
{
	elv_unregister(&mq_deadline);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("Jens Axboe, Damien Le Moal and Bart Van Assche");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MQ deadline IO scheduler");
/* NVMe 관점 핵심 요약 */
/*
 * - mq-deadline은 NVMe SSD로 향하는 request의 마지막 소프트웨어
 *   재배치 계층으로, deadline/FIFO/우선순위를 기준으로 전송 순서를
 *   결정한다.
 * - read_expire/write_expire는 NVMe read/write 각각의 최대 대기
 *   시간을 제어해 latency spike를 방지한다.
 * - per_prio[DD_RT_PRIO/DD_BE_PRIO/DD_IDLE_PRIO]는 ioprio를 통해
 *   NVMe SQ에 들어갈 명령의 우선순위를 분리한다.
 * - blk_mq_hw_ctx 단위로 호출되지만 deadline_data는 request_queue
 *   전체에서 공유되므로, NVMe 멀티 큐 환경에서도 단일 스케줄링
 *   정책이 적용된다(추정).
 * - dispatch된 request는 blk_mq_dispatch_rq_list -> q->mq_ops->queue_rq
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)을 거쳐 NVMe SQ의
 *   CID로 변환되며, 완료 시 NVMe CQ 인터럽트를 통해
 *   dd_finish_request()가 호출된다(추정).
 */

