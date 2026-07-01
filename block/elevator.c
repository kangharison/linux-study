// SPDX-License-Identifier: GPL-2.0
/*
 *  Block device elevator/IO-scheduler.
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 * 30042000 Jens Axboe <axboe@kernel.dk> :
 *
 * Split the elevator a bit so that it is possible to choose a different
 * one or even write a new "plug in". There are three pieces:
 * - elevator_fn, inserts a new request in the queue list
 * - elevator_merge_fn, decides whether a new buffer can be merged with
 *   an existing request
 * - elevator_dequeue_fn, called when a request is taken off the active list
 *
 * 20082000 Dave Jones <davej@suse.de> :
 * Removed tests for max-bomb-segments, which was breaking elvtune
 *  when run without -bN
 *
 * Jens:
 * - Rework again to work with bio instead of buffer_heads
 * - loose bi_dev comparisons, partition handling is right now
 * - completely modularize elevator setup and teardown
 *
 */
/*
 * NVMe 관점 파일 요약:
 *   이 파일은 블록 계층의 elevator/IO 스케줄러 코어로, 상위 bio/request를
 *   스케줄러(mq-deadline, bfq, none 등)에 연결하고 병합/해시/RB-tree를 관리한다.
 *   NVMe SSD 입장에서 본 elevator는 bio가 nvme_queue_rq()를 통해 SQ 엔트리로
 *   전환되기 전에 요청을 정렬/병합/스케줄링하는 단계이다.
 *   전형적 호출 경로:
 *     blk_mq_submit_bio -> blk_mq_get_request -> elv_merge ->
 *     (optional) blk_attempt_req_merge -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   관련 파일: block/blk-mq.c, block/blk-mq-sched.c, block/blk-core.c
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/blktrace_api.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-pm.h"
#include "blk-wbt.h"
#include "blk-cgroup.h"

/* 등록된 elevator_type 목록을 보호하는 스핀락; NVMe 스케줄러 플러그인들도 이 리스트에 등록됨 */
static DEFINE_SPINLOCK(elv_list_lock);
static LIST_HEAD(elv_list);

/*
 * Merge hash stuff.
 */
#define rq_hash_key(rq)		(blk_rq_pos(rq) + blk_rq_sectors(rq))

/*
 * rq_hash_key: 요청의 끝 섹터를 해시 키로 사용.
 *   NVMe 관점에서는 연속된 LBA를 가진 bio들을 back-merge 후보로 빠르게 찾아
 *   PRP/SGL 체인 길이를 줄이는 데 활용된다.
 */
/*
 * Query io scheduler to see if the current process issuing bio may be
 * merged with rq.
 */
/*
 * elv_iosched_allow_bio_merge - IO 스케줄러가 @bio를 @rq와 병합할 수 있는지 확인
 *   호출 경로: elv_bio_merge_ok -> elv_iosched_allow_bio_merge
 *            -> (스케줄러별 allow_merge)
 *   NVMe 연결: mq-deadline/bfq 등에서 정책상 병합을 허용하면, 이후
 *              blk_try_merge() 또는 blk_attempt_req_merge()로 연결되어
 *              SQ에 들어갈 PRP/SGL 체인 길이를 줄일 수 있다.
 */
static bool elv_iosched_allow_bio_merge(struct request *rq, struct bio *bio)
{
	struct request_queue *q = rq->q;
	struct elevator_queue *e = q->elevator;

/* allow_merge ops: 스케줄러별 NVMe 병합 정책 확인 지점 */
	if (e->type->ops.allow_merge)
		return e->type->ops.allow_merge(q, rq, bio);

	return true;
}

/*
 * can we safely merge with this request?
 */
/*
 * elv_bio_merge_ok - 요청 병합의 기본 안전성 검사
 *   blk_rq_merge_ok()로 장치/방향/기타 제약을 확인하고,
 *   스케줄러 정책 허용 여부를 elv_iosched_allow_bio_merge()로 재확인한다.
 *   NVMe 연결: 병합 성공 시 하나의 struct request에 여러 bio가 묶여
 *              nvme_queue_rq()가 SQ 엔트리 1개로 처리할 수 있어
 *              doorbell 횟수와 CID 소모를 감소시킨다 (추정).
 */
bool elv_bio_merge_ok(struct request *rq, struct bio *bio)
{
/* 장치/파티션/방향 등이 달라 NVMe SQ로 분리 제출되어야 하는지 확인 */
	if (!blk_rq_merge_ok(rq, bio))
		return false;

/* 스케줄러가 허용하면 NVMe 관점에서도 병합이 유리한지 최종 판단 */
	if (!elv_iosched_allow_bio_merge(rq, bio))
		return false;

	return true;
}
EXPORT_SYMBOL(elv_bio_merge_ok);

/**
 * elevator_match - Check whether @e's name or alias matches @name
 * @e: Scheduler to test
 * @name: Elevator name to test
 *
 * Return true if the elevator @e's name or alias matches @name.
 */
static bool elevator_match(const struct elevator_type *e, const char *name)
{
/* NVMe 장치가 sysfs에서 선택한 scheduler 이름과 등록된 스케줄러를 매칭 */
	return !strcmp(e->elevator_name, name) ||
		(e->elevator_alias && !strcmp(e->elevator_alias, name));
}

static struct elevator_type *__elevator_find(const char *name)
{
	struct elevator_type *e;

/* elv_list를 순회하며 NVMe에 적용할 scheduler를 탐색 */
	list_for_each_entry(e, &elv_list, list)
		if (elevator_match(e, name))
			return e;
	return NULL;
}

static struct elevator_type *elevator_find_get(const char *name)
{
	struct elevator_type *e;

/* elv_list 보호: NVMe 스케줄러 등록/제거와 동시에 접근하지 않도록 */
	spin_lock(&elv_list_lock);
	e = __elevator_find(name);
	if (e && (!elevator_tryget(e)))
/* 참조 획득 실패 시(e.g. 모듈 제거 중) NVMe 장치는 해당 스케줄러 사용 불가 */
		e = NULL;
	spin_unlock(&elv_list_lock);
	return e;
}

static const struct kobj_type elv_ktype;

/*
 * struct elevator_queue 주요 필드 (NVMe 관점)
 *   type          : 등록된 IO 스케줄러(mq-deadline/bfq/none)의 ops 테이블.
 *                   NVMe request_queue는 이 ops를 통해 삽입/디스패치/병합 정책을 따름.
 *   kobj/sysfs_lock: /sys/block/<disk>/queue/iosched sysfs 노출용.
 *                   NVMe 드라이버/관리자는 이 경로로 스케줄러 파라미터를 변경.
 *   hash          : 요청 끝 섹터 기반 해시, 연속 LBA bio의 back-merge 탐색에 사용.
 *   et            : blk_mq_sched가 할당한 스케줄러 전용 tag/자원 정보.
 *   elevator_data : 스케줄러 사설 데이터(e.g. mq-deadline의 fifo/deadline 라인).
 *   flags         : ELEVATOR_FLAG_REGISTERED/DYING 등.
 *                   NVMe 장치 제거 시 DYING 플래그로 스케줄러 종료를 제어.
 *
 * elevator_alloc - request_queue에 elevator_queue를 할당/초기화
 *   호출 경로: blk_mq_init_sched -> elevator_alloc
 *   NVMe 연결: NVMe namespace의 request_queue 생성 시 tag_set과 함께
 *              스케줄러 자원이 할당되며, 이후 nvme_queue_rq()가 이 elevator를
 *              경유해 요청을 꺼낸다.
 */
struct elevator_queue *elevator_alloc(struct request_queue *q,
		struct elevator_type *e, struct elevator_resources *res)
{
	struct elevator_queue *eq;

/* NVMe request_queue node에 맞춰 elevator 상태 메모리 할당 */
	eq = kzalloc_node(sizeof(*eq), GFP_KERNEL, q->node);
	if (unlikely(!eq))
		return NULL;

	__elevator_get(e);
/* type: 사용할 스케줄러(mq-deadline/bfq/none)의 ops 등록 */
	eq->type = e;
	kobject_init(&eq->kobj, &elv_ktype);
/* kobj: /sys/block/<disk>/queue/iosched 노드 초기화 */
	mutex_init(&eq->sysfs_lock);
/* sysfs_lock: iosched tunable 동시 접근 보호 */
	hash_init(eq->hash);
/* hash: back-merge를 위한 끝 섹터 해시 초기화 */
	eq->et = res->et;
/* et: blk_mq_sched가 준비한 tag/스케줄러 자원 참조 */
	eq->elevator_data = res->data;
/* elevator_data: 스케줄러 사설 상태(e.g. mq-deadline 큐) 저장 */

	return eq;
}

static void elevator_release(struct kobject *kobj)
{
	struct elevator_queue *e;

	e = container_of(kobj, struct elevator_queue, kobj);
	elevator_put(e->type);
	kfree(e);
}

/*
 * elevator_exit - request_queue에서 elevator를 분리하고 정리
 *   호출 경로: elevator_switch -> elevator_exit -> blk_mq_exit_sched
 *   NVMe 연결: NVMe 컨트롤러 리셋/제거 시 queue freeze 후 호출되며,
 *              더 이상 nvme_queue_rq()가 이 elevator의 dispatch를 요구하지 않도록
 *              스케줄러 상태를 해제한다.
 */
static void elevator_exit(struct request_queue *q)
{
	struct elevator_queue *e = q->elevator;

	lockdep_assert_held(&q->elevator_lock);

	ioc_clear_queue(q);

	mutex_lock(&e->sysfs_lock);
/* blk_mq_exit_sched: NVMe tag-set과 연결된 스케줄러 자원 해제 */
	blk_mq_exit_sched(q, e);
	mutex_unlock(&e->sysfs_lock);
}

/*
 * __elv_rqhash_del - 요청을 섹터 기반 해시에서 제거
 *   RQF_HASHED 플래그를 클리어하여 NVMe 디스패치 경로에서 중복 제거됨을 표시.
 */
static inline void __elv_rqhash_del(struct request *rq)
{
/* 해시에서 제거: 이미 NVMe SQ에 나간 요청은 병합 후보에서 제외 */
	hash_del(&rq->hash);
/* RQF_HASHED 클리어 */
	rq->rq_flags &= ~RQF_HASHED;
}

/*
 * elv_rqhash_del - 해시에 있을 경우에만 __elv_rqhash_del() 호출
 *   NVMe SQ에 제출된 요청은 더 이상 병합 대상이 아니므로 해시에서 제거하는 경로가
 *   있음 (추정).
 */
void elv_rqhash_del(struct request_queue *q, struct request *rq)
{
	if (ELV_ON_HASH(rq))
		__elv_rqhash_del(rq);
}
EXPORT_SYMBOL_GPL(elv_rqhash_del);

/*
 * elv_rqhash_add - 요청을 끝 섹터 키로 해시에 삽입
 *   호출 경로: elv_attempt_insert_merge, elv_merged_request 등
 *   NVMe 연결: 연속된 LBA를 가진 새로운 bio가 들어왔을 때 back-merge 후보를
 *              빠르게 찾아 CID를 아끼고 PRP/SGL 개수를 줄일 수 있다.
 */
void elv_rqhash_add(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* BUG_ON: 이미 해시에 있는 request를 중복 삽입하면 안 됨 */
	BUG_ON(ELV_ON_HASH(rq));
/* 끝 섹터(rq_hash_key)를 키로 해시 추가 */
	hash_add(e->hash, &rq->hash, rq_hash_key(rq));
/* RQF_HASHED 설정: 병합 후보 탐색 가능 표시 */
	rq->rq_flags |= RQF_HASHED;
}
EXPORT_SYMBOL_GPL(elv_rqhash_add);

/*
 * elv_rqhash_reposition - 병합 등으로 요청 크기가 달라졌을 때 해시 재배치
 *   병합 후 rq_hash_key가 변경되면 다시 해시에 넣어 NVMe merge 후보 탐색이
 *   정확히 동작하도록 한다.
 */
void elv_rqhash_reposition(struct request_queue *q, struct request *rq)
{
	__elv_rqhash_del(rq);
	elv_rqhash_add(q, rq);
}

/*
 * elv_rqhash_find - 끝 섹터가 @offset인 merge 후보 request 탐색
 *   호출 경로: elv_merge -> elv_rqhash_find
 *   NVMe 연결: 연속 LBA bio가 들어오면 이 함수로 후보를 찾아
 *              blk_try_merge()로 병합; 실패 시 request_merge ops로 폴백(fallback).
 */
struct request *elv_rqhash_find(struct request_queue *q, sector_t offset)
{
	struct elevator_queue *e = q->elevator;
	struct hlist_node *next;
	struct request *rq;

	hash_for_each_possible_safe(e->hash, rq, next, hash, offset) {
		BUG_ON(!ELV_ON_HASH(rq));

/* rq_mergeable: 이미 NVMe SQ에 제출된 요청은 병합 불가 */
		if (unlikely(!rq_mergeable(rq))) {
			__elv_rqhash_del(rq);
			continue;
		}

/* back-merge 가능한 후보 발견; 이후 blk_try_merge로 연결 */
		if (rq_hash_key(rq) == offset)
			return rq;
	}

	return NULL;
}

/*
 * RB-tree support functions for inserting/lookup/removal of requests
 * in a sorted RB tree.
 */
/*
 * elv_rb_add - 요청을 LBA 기준 RB-tree에 삽입
 *   호출 경로: mq-deadline/bfq 등의 스케줄러 낶부
 *   NVMe 연결: NVMe SSD에서는 디스패치 순서가 SQ/CID 순서에 의해 결정되지만,
 *              elevator 단계에서 LBA 순서로 정렬하면 rotational latency 없는
 *              NVMe에서도 연속된 LBA 액세스를 모아 성능을 높일 수 있다.
 */
void elv_rb_add(struct rb_root *root, struct request *rq)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct request *__rq;

	while (*p) {
		parent = *p;
		__rq = rb_entry(parent, struct request, rb_node);

		if (blk_rq_pos(rq) < blk_rq_pos(__rq))
/* LBA가 작으면 왼쪽: NVMe에 제출될 때도 순차적 LBA가 인접하도록 유도 */
			p = &(*p)->rb_left;
		else if (blk_rq_pos(rq) >= blk_rq_pos(__rq))
/* LBA가 같거나 크면 오른쪽: 다음 병합/디스패치 후보 탐색에 활용 */
			p = &(*p)->rb_right;
	}

	rb_link_node(&rq->rb_node, parent, p);
	rb_insert_color(&rq->rb_node, root);
}
EXPORT_SYMBOL(elv_rb_add);

/*
 * elv_rb_del - RB-tree에서 요청 제거
 *   NVMe 디스패치가 완료되어 제거될 때 호출 (추정).
 */
void elv_rb_del(struct rb_root *root, struct request *rq)
{
	BUG_ON(RB_EMPTY_NODE(&rq->rb_node));
	rb_erase(&rq->rb_node, root);
	RB_CLEAR_NODE(&rq->rb_node);
}
EXPORT_SYMBOL(elv_rb_del);

/*
 * elv_rb_find - @sector에 해당하는 요청을 RB-tree에서 탐색
 *   NVMe 스케줄러 낶부에서 LBA 기반 후보를 O(log N)로 찾는 데 사용.
 */
struct request *elv_rb_find(struct rb_root *root, sector_t sector)
{
	struct rb_node *n = root->rb_node;
	struct request *rq;

	while (n) {
		rq = rb_entry(n, struct request, rb_node);

		if (sector < blk_rq_pos(rq))
			n = n->rb_left;
		else if (sector > blk_rq_pos(rq))
			n = n->rb_right;
		else
			return rq;
	}

	return NULL;
}
EXPORT_SYMBOL(elv_rb_find);

/*
 * elv_merge - 상위 bio가 기존 request와 병합할 수 있는지 결정
 *   호출 경로:
 *     blk_mq_submit_bio -> blk_mq_get_request -> elv_merge
 *   NVMe 연결:
 *     - blk_queue_nomerges 시 ELEVATOR_NO_MERGE를 반환하여 bio를 그대로
 *       별도 request로 전달; nvme_queue_rq()에서 개별 SQ 엔트리가 됨.
 *     - q->last_merge 캐시 히트 시 blk_try_merge()로 빠른 병합.
 *     - 해시/스케줄러 병합 실패 시 ELEVATOR_NO_MERGE 반환.
 *   반환: ELEVATOR_BACK_MERGE / ELEVATOR_FRONT_MERGE / ELEVATOR_DISCARD_MERGE
 */
enum elv_merge elv_merge(struct request_queue *q, struct request **req,
		struct bio *bio)
{
	struct elevator_queue *e = q->elevator;
	struct request *__rq;

	/*
	 * Levels of merges:
	 * 	nomerges:  No merges at all attempted
	 * 	noxmerges: Only simple one-hit cache try
	 * 	merges:	   All merge tries attempted
	 */
/* nomerges 또는 bio 병합 불가: NVMe에도 bio별 request로 전달 */
	if (blk_queue_nomerges(q) || !bio_mergeable(bio))
		return ELEVATOR_NO_MERGE;

	/*
	 * First try one-hit cache.
	 */
	if (q->last_merge && elv_bio_merge_ok(q->last_merge, bio)) {
/* one-hit 캐시: 직전 병합 요청을 재활용해 doorbell 지연 감소 (추정) */
		enum elv_merge ret = blk_try_merge(q->last_merge, bio);

		if (ret != ELEVATOR_NO_MERGE) {
			*req = q->last_merge;
			return ret;
		}
	}

	if (blk_queue_noxmerges(q))
/* noxmerges: 단순 캐시만 시도; NVMe random 워크로드에서 오버헤드 감소 */
		return ELEVATOR_NO_MERGE;

	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	__rq = elv_rqhash_find(q, bio->bi_iter.bi_sector);
/* LBA 연속 후보 탐색: 해시에서 back-merge 대상을 찾음 */
	if (__rq && elv_bio_merge_ok(__rq, bio)) {
		*req = __rq;

		if (blk_discard_mergable(__rq))
/* discard 병합: NVMe Deallocate/Trim SQ 엔트리 수 감소 */
			return ELEVATOR_DISCARD_MERGE;
		return ELEVATOR_BACK_MERGE;
	}

	if (e->type->ops.request_merge)
/* 스케줄러별 request_merge: 해시 실패 후 폴백(fallback) (e.g. bfq front-merge) */
		return e->type->ops.request_merge(q, req, bio);

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to do an insertion back merge. Only check for the case where
 * we can append 'rq' to an existing request, so we can throw 'rq' away
 * afterwards.
 *
 * Returns true if we merged, false otherwise. 'free' will contain all
 * requests that need to be freed.
 */
/*
 * elv_attempt_insert_merge - 새 request를 기존 request 뒤에 붙여 제거
 *   호출 경로: blk_mq_get_request -> ... -> elv_attempt_insert_merge
 *   NVMe 연결: bio를 신규 request에 할당한 직후, 기존 request에 back-merge해
 *              free 목록에 넣고, 최종적으로 nvme_queue_rq()가 처리할
 *              request 수를 줄인다. 연속 병합이 여러 번 일어날 수 있음.
 */
bool elv_attempt_insert_merge(struct request_queue *q, struct request *rq,
			      struct list_head *free)
{
	struct request *__rq;
	bool ret;

	if (blk_queue_nomerges(q))
/* nomerges 설정 시 즉시 실패: NVMe도 별도 SQ 엔트리 유지 */
		return false;

	/*
	 * First try one-hit cache.
	 */
	if (q->last_merge && blk_attempt_req_merge(q, q->last_merge, rq)) {
/* last_merge 캐시 우선 시도 */
		list_add(&rq->queuelist, free);
		return true;
	}

	if (blk_queue_noxmerges(q))
/* noxmerges 시 해시 탐색 생략 */
		return false;

	ret = false;
	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	while (1) {
		__rq = elv_rqhash_find(q, blk_rq_pos(rq));
/* 해시에서 찾은 후보와 병합 시도; 성공 시 rq는 free 목록으로 */
		if (!__rq || !blk_attempt_req_merge(q, __rq, rq))
			break;

		list_add(&rq->queuelist, free);
/* 병합된 request를 다시 후보로 해 추가 병합 시도: NVMe SQ 엔트리 최소화 */
		/* The merged request could be merged with others, try again */
		ret = true;
/* rq를 병합된 __rq로 교체, 다음 루프에서 더 뒤의 연속 LBA 탐색 */
		rq = __rq;
	}

	return ret;
}

/*
 * elv_merged_request - 병합 완료 후 스케줄러에 알리고 해시/캐시 갱신
 *   호출 경로: blk_attempt_req_merge 완료 후
 *   NVMe 연결: 병합된 request가 nvme_queue_rq()에 의해 SQ 엔트리로 변환될 때
 *              더 큰 단위로 전송되어 host memory buffer(PRP/SGL) 효율 상승.
 */
void elv_merged_request(struct request_queue *q, struct request *rq,
		enum elv_merge type)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.request_merged)
		e->type->ops.request_merged(q, rq, type);

	if (type == ELEVATOR_BACK_MERGE)
/* back-merge 후 해시 키가 바뀌므로 재배치 */
		elv_rqhash_reposition(q, rq);

/* last_merge 캐시 갱신 */
	q->last_merge = rq;
}

/*
 * elv_merge_requests - 두 request가 병합됨을 스케줄러에 통지
 *   호출 경로: blk_attempt_req_merge -> elv_merge_requests
 *   NVMe 연결: merge된 최종 request가 nvme_queue_rq()에서 단일 CID와
 *              하나의 doorbell로 제출될 수 있음 (추정).
 */
void elv_merge_requests(struct request_queue *q, struct request *rq,
			     struct request *next)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.requests_merged)
		e->type->ops.requests_merged(q, rq, next);

	elv_rqhash_reposition(q, rq);
	q->last_merge = rq;
}

struct request *elv_latter_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 다음 request 콜백: NVMe dispatch 순서에서 뒤에 위치한 후보 */
	if (e->type->ops.next_request)
		return e->type->ops.next_request(q, rq);

	return NULL;
}

struct request *elv_former_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 이전 request 콜백: NVMe dispatch 순서에서 앞에 위치한 후보 */
	if (e->type->ops.former_request)
		return e->type->ops.former_request(q, rq);

	return NULL;
}

#define to_elv(atr) container_of_const((atr), struct elv_fs_entry, attr)

static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
	ssize_t error = -ENODEV;

	if (!entry->show)
		return -EIO;

	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: NVMe iosched tunable 읽기 동안 elevator 구조체 보호 */
	mutex_lock(&e->sysfs_lock);
/* ELEVATOR_FLAG_DYING이 설정되면 NVMe 장치 제거 중이므로 tunable 접근 차단 */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		error = entry->show(e, page);
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
	ssize_t error = -ENODEV;

	if (!entry->store)
		return -EIO;

	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: NVMe scheduler 파라미터 쓰기 중 race 방지 */
	mutex_lock(&e->sysfs_lock);
/* dying 상태에서는 NVMe 큐가 정리 중이므로 파라미터 변경 불가 */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		error = entry->store(e, page, length);
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static const struct sysfs_ops elv_sysfs_ops = {
	.show	= elv_attr_show,
	.store	= elv_attr_store,
};

static const struct kobj_type elv_ktype = {
	.sysfs_ops	= &elv_sysfs_ops,
	.release	= elevator_release,
};

static int elv_register_queue(struct request_queue *q,
			      struct elevator_queue *e,
			      bool uevent)
{
	int error;

/* /sys/block/<disk>/queue/iosched kobject 생성: NVMe 관리자 접근 지점 */
	error = kobject_add(&e->kobj, &q->disk->queue_kobj, "iosched");
	if (!error) {
		const struct elv_fs_entry *attr = e->type->elevator_attrs;
		if (attr) {
/* scheduler별 sysfs 파일 생성(e.g. read_expire, fifo_batch 등) */
			while (attr->attr.name) {
				if (sysfs_create_file(&e->kobj, &attr->attr))
					break;
				attr++;
			}
		}
		if (uevent)
			kobject_uevent(&e->kobj, KOBJ_ADD);

		/*
		 * Sched is initialized, it is ready to export it via
		 * debugfs
		 */
/* blk-mq sched debugfs 등록: NVMe dispatch 지연/depth 분석용 */
		blk_mq_sched_reg_debugfs(q);
/* ELEVATOR_FLAG_REGISTERED 설정: NVMe I/O 경로에서 이 elevator를 사용 가능 */
		set_bit(ELEVATOR_FLAG_REGISTERED, &e->flags);
	}
	return error;
}

static void elv_unregister_queue(struct request_queue *q,
				 struct elevator_queue *e)
{
	if (e && test_and_clear_bit(ELEVATOR_FLAG_REGISTERED, &e->flags)) {
/* kobject 제거: NVMe 장치 제거 시 /sys/block/<disk>/queue/iosched 제거 */
		kobject_uevent(&e->kobj, KOBJ_REMOVE);
		kobject_del(&e->kobj);

		/* unexport via debugfs before exiting sched */
		blk_mq_sched_unreg_debugfs(q);
	}
}

/*
 * struct elevator_type 주요 필드 (NVMe 관점)
 *   elevator_name/alias : "mq-deadline", "bfq", "none" 등 사용자가 sysfs에서 선택하는 이름.
 *   ops                 : insert_requests, dispatch_request, allow_merge 등 콜백.
 *                         NVMe request_queue는 이 콜백을 통해 bio를 SQ에 제출하기 전
 *                         정렬/병합/디스패치를 수행한다.
 *   icq_size/icq_align  : io_cq(ICQ) 캐시 크기/정렬; cgroup 기반 스케줄링 상태.
 *   elevator_attrs      : /sys/block/<disk>/queue/iosched 아래의 tunable.
 *   icq_cache           : per-cgroup IO context 캐시.
 *   list                : elv_list 연결 리스트 엔트리.
 *
 * elv_register - 새 IO 스케줄러를 전역 elv_list에 등록
 *   호출 경로: 스케줄러 모듈 initcall -> elv_register
 *   NVMe 연결: mq-deadline, bfq, kyber 등이 등록되며,
 *              NVMe 장치 초기화 시 elevator_set_default()에서 이 리스트를
 *              조회해 적합한 스케줄러를 선택한다.
 */
int elv_register(struct elevator_type *e)
{
	/* finish request is mandatory */
/* finish_request는 NVMe 완료 인터럽트 경로에서도 호출되므로 필수 */
	if (WARN_ON_ONCE(!e->ops.finish_request))
		return -EINVAL;
	/* insert_requests and dispatch_request are mandatory */
	if (WARN_ON_ONCE(!e->ops.insert_requests || !e->ops.dispatch_request))
/* insert_requests/dispatch_request: blk-mq와 NVMe 간 요청 흐름의 핵심 콜백 */
		return -EINVAL;

	/* create icq_cache if requested */
	if (e->icq_size) {
/* icq 캐시: cgroup별 NVMe I/O 스케줄링 상태 저장소 */
		if (WARN_ON(e->icq_size < sizeof(struct io_cq)) ||
		    WARN_ON(e->icq_align < __alignof__(struct io_cq)))
			return -EINVAL;

		snprintf(e->icq_cache_name, sizeof(e->icq_cache_name),
			 "%s_io_cq", e->elevator_name);
		e->icq_cache = kmem_cache_create(e->icq_cache_name, e->icq_size,
						 e->icq_align, 0, NULL);
		if (!e->icq_cache)
			return -ENOMEM;
	}

	/* register, don't allow duplicate names */
/* 중복 등록 방지: 동일 이름의 NVMe 스케줄러가 두 개 존재하면 안 됨 */
	spin_lock(&elv_list_lock);
	if (__elevator_find(e->elevator_name)) {
		spin_unlock(&elv_list_lock);
		kmem_cache_destroy(e->icq_cache);
		return -EBUSY;
	}
/* elv_list에 추가되어 NVMe 장치에서 선택 가능해짐 */
	list_add_tail(&e->list, &elv_list);
	spin_unlock(&elv_list_lock);

	printk(KERN_INFO "io scheduler %s registered\n", e->elevator_name);

	return 0;
}
EXPORT_SYMBOL_GPL(elv_register);

/*
 * elv_unregister - IO 스케줄러 모듈 제거
 *   호출 경로: 모듈 exit -> elv_unregister
 *   NVMe 연결: 스케줄러 모듈이 제거되면 NVMe 장치는 더 이상 해당 elevator를
 *              사용할 수 없으므로, 등록 해제 전 rcu_barrier()로 진행 중인
 *              nvme_queue_rq() 경로의 참조가 완료되도록 보장.
 */
void elv_unregister(struct elevator_type *e)
{
	/* unregister */
/* elv_list에서 제거: 이후 NVMe 장치는 해당 스케줄러를 찾을 수 없음 */
	spin_lock(&elv_list_lock);
	list_del_init(&e->list);
	spin_unlock(&elv_list_lock);

	/*
	 * Destroy icq_cache if it exists.  icq's are RCU managed.  Make
	 * sure all RCU operations are complete before proceeding.
	 */
	if (e->icq_cache) {
/* rcu_barrier: NVMe 완료 경로의 RCU readers 종료 대기 */
		rcu_barrier();
		kmem_cache_destroy(e->icq_cache);
		e->icq_cache = NULL;
	}
}
EXPORT_SYMBOL_GPL(elv_unregister);

/*
 * Switch to new_e io scheduler.
 *
 * If switching fails, we are most likely running out of memory and not able
 * to restore the old io scheduler, so leaving the io scheduler being none.
 */
/*
 * elevator_switch - request_queue의 IO 스케줄러를 @ctx->name으로 교체
 *   호출 경로: elevator_change -> elevator_switch
 *           -> blk_mq_quiesce_queue / blk_mq_init_sched
 *   NVMe 연결: NVMe 컨트롤러의 queue_depth/hw_queue 변화 또는 사용자 sysfs
 *              변경 시 queue를 freeze/quiesce하고, 기존 elevator를 내린 뒤
 *              새 elevator를 연결. 이 동안 nvme_queue_rq()는 중단된다.
 */
static int elevator_switch(struct request_queue *q, struct elv_change_ctx *ctx)
{
	struct elevator_type *new_e = NULL;
	int ret = 0;

	WARN_ON_ONCE(q->mq_freeze_depth == 0);
	lockdep_assert_held(&q->elevator_lock);

	if (strncmp(ctx->name, "none", 4)) {
		new_e = elevator_find_get(ctx->name);
		if (!new_e)
			return -EINVAL;
	}

	blk_mq_quiesce_queue(q);
/* queue quiesce: NVMe I/O 진행 중인 request들이 안전히 멈춤 */

	if (q->elevator) {
		ctx->old = q->elevator;
/* 기존 elevator_exit: NVMe 요청이 old scheduler를 참조하지 않도록 정리 */
		elevator_exit(q);
	}

	if (new_e) {
		ret = blk_mq_init_sched(q, new_e, &ctx->res);
/* blk_mq_init_sched: 새 elevator와 NVMe tag_set을 연결 */
		if (ret)
			goto out_unfreeze;
		ctx->new = q->elevator;
	} else {
		blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);
/* "none" 선택 시 QUEUE_FLAG_SQ_SCHED 해제: software scheduling 우회 */
		q->elevator = NULL;
		q->nr_requests = q->tag_set->queue_depth;
/* none으로 전환 시 queue_depth를 tag_set depth로 복원: NVMe SQ 깊이와 동기화 */
		q->async_depth = q->tag_set->queue_depth;
	}
/* blk_add_trace_msg: elevator 변경 이벤트 트레이싱 */
	blk_add_trace_msg(q, "elv switch: %s", ctx->name);

out_unfreeze:
	blk_mq_unquiesce_queue(q);
/* queue unquiesce: NVMe I/O 경로 재개 */

	if (ret) {
		pr_warn("elv: switch to \"%s\" failed, falling back to \"none\"\n",
			new_e->elevator_name);
	}

	if (new_e)
		elevator_put(new_e);
	return ret;
}

static void elv_exit_and_release(struct elv_change_ctx *ctx,
		struct request_queue *q)
{
	struct elevator_queue *e;
	unsigned memflags;

/* queue freeze: NVMe submit/dispatch 경로 일시 중단, memflags 보관 */
	memflags = blk_mq_freeze_queue(q);
	mutex_lock(&q->elevator_lock);
	e = q->elevator;
/* elevator_exit: NVMe tag-set과 연결된 scheduler 자원 해제 시작 */
	elevator_exit(q);
	mutex_unlock(&q->elevator_lock);
	blk_mq_unfreeze_queue(q, memflags);
	if (e) {
/* 스케줄러 자원 반환 후 kobject 참조 해제로 최종 해제 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, q->tag_set);
		kobject_put(&e->kobj);
	}
}

/*
 * elevator_change_done - 스케줄러 전환 후 sysfs/debugfs 등록 및 자원 정리
 *   호출 경로: elevator_change -> elevator_change_done
 *   NVMe 연결: 전환이 끝난 후 /sys/block/<disk>/queue/iosched가 다시
 *              노출되며, NVMe I/O 경로가 새 elevator의 dispatch를 사용.
 */
static int elevator_change_done(struct request_queue *q,
				struct elv_change_ctx *ctx)
{
	int ret = 0;

	if (ctx->old) {
		struct elevator_resources res = {
			.et = ctx->old->et,
			.data = ctx->old->elevator_data
		};

		elv_unregister_queue(q, ctx->old);
		blk_mq_free_sched_res(&res, ctx->old->type, q->tag_set);
		kobject_put(&ctx->old->kobj);
	}
	if (ctx->new) {
		ret = elv_register_queue(q, ctx->new, !ctx->no_uevent);
		if (ret)
			elv_exit_and_release(ctx, q);
	}
	return ret;
}

/*
 * Switch this queue to the given IO scheduler.
 */
/*
 * elevator_change - 사용자/sysfs 요청에 따라 elevator를 변경
 *   호출 경로: elevator_set_default / elv_iosched_store -> elevator_change
 *   NVMe 연결:
 *     - queue freeze -> blk_mq_cancel_work_sync -> elevator_switch ->
 *       blk_mq_init_sched -> blk_mq_unquiesce_queue.
 *     - 이 과정에서 NVMe SQ/CID는 그대로 두고, 상위 request_queue의
 *       스케줄링 레이어만 교체한다.
 */
static int elevator_change(struct request_queue *q, struct elv_change_ctx *ctx)
{
	unsigned int memflags;
	struct blk_mq_tag_set *set = q->tag_set;
	int ret = 0;

	lockdep_assert_held(&set->update_nr_hwq_lock);

	if (strncmp(ctx->name, "none", 4)) {
		ret = blk_mq_alloc_sched_res(q, ctx->type, &ctx->res,
/* 스케줄러 자원 사전 할당: NVMe tag_set의 hw_queue 수만큼 */
				set->nr_hw_queues);
		if (ret)
			return ret;
	}

	memflags = blk_mq_freeze_queue(q);
/* queue freeze: NVMe request 발행을 일시 중단 */
	/*
	 * May be called before adding disk, when there isn't any FS I/O,
	 * so freezing queue plus canceling dispatch work is enough to
	 * drain any dispatch activities originated from passthrough
	 * requests, then no need to quiesce queue which may add long boot
	 * latency, especially when lots of disks are involved.
	 *
	 * Disk isn't added yet, so verifying queue lock only manually.
	 */
	blk_mq_cancel_work_sync(q);
/* dispatch work 취소: 진행 중인 blk-mq dispatch가 NVMe SQ에 쓰지 않도록 */
	mutex_lock(&q->elevator_lock);
	if (!(q->elevator && elevator_match(q->elevator->type, ctx->name)))
/* 이미 동일 스케줄러면 전환 생략 */
		ret = elevator_switch(q, ctx);
	mutex_unlock(&q->elevator_lock);
	blk_mq_unfreeze_queue(q, memflags);
	if (!ret)
		ret = elevator_change_done(q, ctx);
/* 전환 후 등록/정리 수행 */

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	if (!ctx->new)
/* 전환 실패 시 미사용 스케줄러 자원 해제 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);

	return ret;
}

/*
 * The I/O scheduler depends on the number of hardware queues, this forces a
 * reattachment when nr_hw_queues changes.
 */
/*
 * elv_update_nr_hw_queues - hw_queue 수(nr_hw_queues) 변경 시 elevator 재부착
 *   호출 경로: blk_mq_update_nr_hw_queues -> elv_update_nr_hw_queues
 *   NVMe 연결:
 *     - NVMe 멀티큐 컨트롤러가 nr_io_queues를 변경하면 각 blk_mq_hw_ctx와
 *       tag_set이 갱신되므로, elevator도 동일한 hw_queue 수에 맞춰
 *       다시 초기화해야 함.
 *     - elevator_switch -> elevator_change_done 순으로 처리.
 */
void elv_update_nr_hw_queues(struct request_queue *q,
		struct elv_change_ctx *ctx)
{
	struct blk_mq_tag_set *set = q->tag_set;
	int ret = -ENODEV;

	WARN_ON_ONCE(q->mq_freeze_depth == 0);
/* queue freeze 상태여야 함: NVMe hw_queue 변경 중 I/O 정지 보장 */

	if (ctx->type && !blk_queue_dying(q) && blk_queue_registered(q)) {
		mutex_lock(&q->elevator_lock);
		/* force to reattach elevator after nr_hw_queue is updated */
/* dying/registered가 아니면 elevator 재부착 */
		ret = elevator_switch(q, ctx);
		mutex_unlock(&q->elevator_lock);
	}
/* unfreeze: NVMe I/O 재개 */
	blk_mq_unfreeze_queue_nomemrestore(q);
	if (!ret)
		WARN_ON_ONCE(elevator_change_done(q, ctx));

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	if (!ctx->new)
/* 재부착 실패 시 자원 해제 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);
}

/*
 * Use the default elevator settings. If the chosen elevator initialization
 * fails, fall back to the "none" elevator (no elevator).
 */
/*
 * elevator_set_default - 장치 등록 시 기본 IO 스케줄러를 연결
 *   호출 경로: add_disk -> blk_register_queue -> elevator_set_default
 *   NVMe 연결:
 *     - NVMe namespace가 디스크로 등록될 때 호출.
 *     - 단일 hw_queue 또는 shared tags인 경우 mq-deadline을 시도하고,
 *       실패하거나 멀티큐이면 "none"을 사용한다.
 *     - NVMe SSD는 보통 "none"이 기본이지만, 커널 구성/태그셋 플래그에 따라
 *       mq-deadline이 선택되기도 한다 (추정).
 */
void elevator_set_default(struct request_queue *q)
{
	struct elv_change_ctx ctx = {
		.name = "mq-deadline",
/* 기본 스케줄러: mq-deadline */
		.no_uevent = true,
	};
	int err;

	/* now we allow to switch elevator */
/* elevator 전환 허용: NVMe 장치도 sysfs로 변경 가능 */
	blk_queue_flag_clear(QUEUE_FLAG_NO_ELV_SWITCH, q);

	if (q->tag_set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT)
/* BLK_MQ_F_NO_SCHED_BY_DEFAULT: NVMe 장치에서 "none" 강제 플래그 */
		return;

	/*
	 * For single queue devices, default to using mq-deadline. If we
	 * have multiple queues or mq-deadline is not available, default
	 * to "none".
	 */
	ctx.type = elevator_find_get(ctx.name);
/* elv_list에서 mq-deadline 조회; 없으면 none 유지 */
	if (!ctx.type)
		return;

	if ((q->nr_hw_queues == 1 ||
/* 단일 큐/shared tags일 때만 mq-deadline 시도; 멀티큐 NVMe는 none 선호 */
			blk_mq_is_shared_tags(q->tag_set->flags))) {
		err = elevator_change(q, &ctx);
/* elevator_change 실패 시 none으로 폴백(fallback) */
		if (err < 0)
			pr_warn("\"%s\" elevator initialization, failed %d, falling back to \"none\"\n",
					ctx.name, err);
	}
	elevator_put(ctx.type);
/* 참조 해제: 이미 request_queue에 연결되었거나 실패함 */
}

/*
 * elevator_set_none - 스케줄러를 "none"으로 변경
 *   호출 경로: 사용자 sysfs "none" 쓰기 -> elv_iosched_store -> elevator_set_none
 *   NVMe 연결: NVMe 고성능 장치에서 software scheduling 오버헤드를 제거하고
 *              bio/request를 직접 blk-mq로 통과시켜 nvme_queue_rq()로 빠르게
 *              전달한다.
 */
void elevator_set_none(struct request_queue *q)
{
	struct elv_change_ctx ctx = {
		.name	= "none",
	};
	int err;

	err = elevator_change(q, &ctx);
	if (err < 0)
		pr_warn("%s: set none elevator failed %d\n", __func__, err);
}

static void elv_iosched_load_module(const char *elevator_name)
{
	struct elevator_type *found;

/* elv_list_lock 짧게 획득하여 요청한 scheduler가 이미 등록되었는지 확인 */
	spin_lock(&elv_list_lock);
	found = __elevator_find(elevator_name);
	spin_unlock(&elv_list_lock);

	if (!found)
/* 미등록 스케줄러 모듈 자동 로드: e.g. "bfq-iosched" */
		request_module("%s-iosched", elevator_name);
}

/*
 * elv_iosched_store - /sys/block/<disk>/queue/scheduler 쓰기 처리
 *   호출 경로: sysfs write -> elv_iosched_store
 *   NVMe 연결:
 *     - 사용자가 "none", "mq-deadline", "bfq" 등을 선택하면,
 *       elevator_change()를 통해 NVMe request_queue의 스케줄러가 변경됨.
 *     - 모듈 자동 로드 후 update_nr_hwq_lock을 획득하여 race 방지.
 */
ssize_t elv_iosched_store(struct gendisk *disk, const char *buf,
			  size_t count)
{
	char elevator_name[ELV_NAME_MAX];
	struct elv_change_ctx ctx = {};
	int ret;
	struct request_queue *q = disk->queue;
	struct blk_mq_tag_set *set = q->tag_set;

	/* Make sure queue is not in the middle of being removed */
	if (!blk_queue_registered(q))
/* 장치가 아직 등록되지 않았으면 NVMe 스케줄러 변경 불가 */
		return -ENOENT;

	/*
	 * If the attribute needs to load a module, do it before freezing the
	 * queue to ensure that the module file can be read when the request
	 * queue is the one for the device storing the module file.
	 */
	strscpy(elevator_name, buf, sizeof(elevator_name));
/* 사용자 입력을 "none"/"mq-deadline"/"bfq" 등 정제 */
	ctx.name = strstrip(elevator_name);

	elv_iosched_load_module(ctx.name);
/* 해당 이름의 스케줄러를 elv_list에서 조회/참조 획득 */
	ctx.type = elevator_find_get(ctx.name);

	/*
	 * Use trylock to avoid circular lock dependency with kernfs active
	 * reference during concurrent disk deletion:
	 *   update_nr_hwq_lock -> kn->active (via del_gendisk -> kobject_del)
	 *   kn->active -> update_nr_hwq_lock (via this sysfs write path)
	 */
	if (!down_read_trylock(&set->update_nr_hwq_lock)) {
/* update_nr_hwq_lock: NVMe hw_queue 수 변경과 동시 수정 방지 */
		ret = -EBUSY;
		goto out;
	}
	if (!blk_queue_no_elv_switch(q)) {
/* elevator_change: 실제로 NVMe request_queue의 스케줄러 교체 */
		ret = elevator_change(q, &ctx);
		if (!ret)
			ret = count;
	} else {
		ret = -ENOENT;
	}
	up_read(&set->update_nr_hwq_lock);
/* lock 해제 후 참조 해제 */

out:
	if (ctx.type)
/* 스케줄러 타입 참조 해제 */
		elevator_put(ctx.type);
	return ret;
}

/*
 * elv_iosched_show - /sys/block/<disk>/queue/scheduler 읽기 처리
 *   호출 경로: sysfs read -> elv_iosched_show
 *   NVMe 연결: 현재 NVMe 장치에 적용된 스케줄러를 대괄호로 표시하며,
 *              등록된 스케줄러 목록을 반환한다.
 */
ssize_t elv_iosched_show(struct gendisk *disk, char *name)
{
	struct request_queue *q = disk->queue;
	struct elevator_type *cur = NULL, *e;
	int len = 0;

	mutex_lock(&q->elevator_lock);
	if (!q->elevator) {
		len += sprintf(name+len, "[none] ");
	} else {
		len += sprintf(name+len, "none ");
		cur = q->elevator->type;
	}

	spin_lock(&elv_list_lock);
/* elv_list를 순회하며 NVMe에 사용 가능한 scheduler 목록 출력 */
	list_for_each_entry(e, &elv_list, list) {
		if (e == cur)
			len += sprintf(name+len, "[%s] ", e->elevator_name);
		else
			len += sprintf(name+len, "%s ", e->elevator_name);
	}
	spin_unlock(&elv_list_lock);

	len += sprintf(name+len, "\n");
	mutex_unlock(&q->elevator_lock);

	return len;
}

struct request *elv_rb_former_request(struct request_queue *q,
				      struct request *rq)
{
	struct rb_node *rbprev = rb_prev(&rq->rb_node);

/* RB-tree에서 이전 LBA 요청 탐색: NVMe dispatch에서 순차적 prefetach 후보 (추정) */
	if (rbprev)
		return rb_entry_rq(rbprev);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_former_request);

struct request *elv_rb_latter_request(struct request_queue *q,
				      struct request *rq)
{
	struct rb_node *rbnext = rb_next(&rq->rb_node);

/* RB-tree에서 다음 LBA 요청 탐색: NVMe dispatch에서 뒤이어 제출할 연속 LBA 후보 (추정) */
	if (rbnext)
		return rb_entry_rq(rbnext);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_latter_request);

/*
 * elevator_setup - "elevator=" 커널 파라미터 처리(더 이상 효과 없음)
 *   NVMe 연결: 현재 NVMe 장치는 sysfs/udev에서 per-device scheduler를
 *              설정하므로, 이 파라미터는 무시됨.
 */
static int __init elevator_setup(char *str)
{
	pr_warn("Kernel parameter elevator= does not have any effect anymore.\n"
		"Please use sysfs to set IO scheduler for individual devices.\n");
	return 1;
}

__setup("elevator=", elevator_setup);

/* NVMe 관점 핵심 요약 */
/*
 *   - elevator.c는 bio/request가 nvme_queue_rq()를 통해 SQ/CID로 변환되기 전의
 *     마지막 software scheduling 단계이다.
 *   - elv_merge() 계열 함수는 연속 LBA bio를 병합하여 PRP/SGL 체인 길이와
 *     doorbell 횟수를 줄이는 데 기여한다.
 *   - struct elevator_queue는 스케줄러 상태(tag, hash, elevator_data)를
 *     관리하며, NVMe 멀티큐 환경에서도 blk_mq_hw_ctx 단위로 동작한다.
 *   - elevator_change/switch/update_nr_hw_queues는 queue freeze를 이용해
 *     NVMe I/O를 일시 중단하고 스케줄러를 안전하게 교체한다.
 *   - "none" 스케줄러를 선택하면 software scheduling 오버헤드가 사라지고,
 *     bio는 곧바로 blk-mq -> nvme_queue_rq() 경로로 흐른다.
 */
