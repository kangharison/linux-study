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
 * [한국어 설명] 블록 계층 IO 스케줄러(elevator) 핵심 구현 (elevator.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 블록 계층에서 IO 스케줄러(elevator)의 공통 인터페이스와
 * 핵심 자료구조 관리를 담당한다. elevator는 응용이 발행한 bio/request를
 * mq-deadline·BFQ·kyber·none 등의 스케줄러 플러그인에 연결하고, 요청 병합
 * (merge)·LBA 기반 해시·RB-tree를 통해 NVMe SQ로 내려가기 전 단계에서 요청을
 * 정렬·병합·스케줄링한다. 스케줄러 등록/해제, sysfs 연동, 동적 교체(switch)
 * 로직도 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IO 경로에서 elevator는 blk-mq와 실제 드라이버(nvme_queue_rq) 사이에 위치한다:
 *
 *   [응용] write(2) → submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓  → elv_merge(): 기존 request와 병합 시도 (back/front/discard)
 *       ↓  → elv_attempt_insert_merge(): 신규 request를 해시에서 찾은 후보와 병합
 *       ↓  → 스케줄러 ops.insert_requests(): mq-deadline/BFQ 큐에 삽입
 *       ↓
 *   [blk-mq dispatch] ops.dispatch_request(): 스케줄러가 최적 request 선택
 *       ↓
 *   [NVMe 드라이버] nvme_queue_rq() → SQ doorbell → CQ 완료
 *
 * elevator가 없을 때("none" 선택 시): blk-mq가 직접 request를 드라이버에 전달.
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(bio 제출 경로); elevator_release는
 * kobject_put() 경로이므로 어떤 컨텍스트에서도 호출될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-mq.c: blk_mq_submit_bio()→elv_merge() 호출, blk_mq_init_sched()
 *   - block/blk-mq-sched.c: blk_mq_init_sched()·blk_mq_exit_sched() — elevator
 *     초기화·해제 시 tag_set과 연결
 *   - block/mq-deadline.c, bfq-iosched.c, kyber-iosched.c: elv_register()로
 *     elv_list에 등록; ops 콜백(insert/dispatch/allow_merge 등) 제공
 *   - block/blk-merge.c: blk_attempt_req_merge()·blk_try_merge() 구현
 *   - block/blk-ioc.c: ioc_clear_queue() — elevator 종료 시 io_context 정리
 * 공유 자료구조:
 *   - struct elevator_queue (elevator.h): type·kobj·hash·flags·elevator_data
 *   - struct elevator_type (elevator.h): ops vtable·elevator_name·icq_cache
 *   - elv_list (전역): 등록된 스케줄러 목록, elv_list_lock으로 보호
 *
 * === 주요 함수/구조체 요약 ===
 * elv_merge()              - bio가 기존 request와 병합 가능한지 결정; 해시·캐시·스케줄러 순으로 탐색
 * elv_attempt_insert_merge() - 신규 request를 해시에서 찾은 후보에 연속 back-merge; SQ 엔트리 수 감소
 * elv_register/unregister()  - 스케줄러 모듈이 전역 elv_list에 등록/해제
 * elevator_change()          - sysfs/내부 요청으로 elevator를 동적 교체; queue freeze→switch→unfreeze
 * elevator_set_default()     - 장치 등록 시 mq-deadline(단일큐) 또는 none(멀티큐) 기본 적용
 * elv_rqhash_{add/del/find}()- 끝 섹터 기반 해시로 back-merge 후보를 O(1) 탐색
 * elv_rb_{add/del/find}()    - LBA 기반 RB-tree로 front-merge·dispatch 순서를 O(log N) 탐색
 * struct elevator_queue      - 디바이스 큐(request_queue)에 붙는 elevator 상태; type·hash·kobj
 * struct elevator_type       - 스케줄러 플러그인 설명자; ops vtable·이름·icq 캐시
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

/*
 * [한국어]
 * elevator_release - elevator_queue의 kobject 참조가 0이 됐을 때 메모리 해제
 *
 * @kobj: 해제할 elevator_queue에 내장된 kobject 포인터
 *
 * kobject_put()이 참조 카운트를 0으로 만들면 kobj_type.release로 등록된
 * 이 함수가 호출된다. elevator_queue는 직접 kfree할 수 없고 반드시 kobject
 * 생명주기를 통해 해제해야 하는데, 이 함수가 그 최종 단계이다.
 * elevator_exit() → kobject_del() → kobject_put() → elevator_release() 순.
 *
 * 호출 체인:
 *   elevator_exit/elv_exit_and_release → kobject_put(&e->kobj) → [elevator_release]
 */
static void elevator_release(struct kobject *kobj)
{
	struct elevator_queue *e;

/* kobj에서 상위 elevator_queue 포인터 역산: kobject는 구조체 내장 멤버 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* 스케줄러 타입(elevator_type) 모듈 참조 해제: 이 시점이 마지막 사용자일 수 있음 */
	elevator_put(e->type);
/* elevator_queue 구조체 자체 해제 */
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
 * [한국어]
 * elv_rb_del - LBA 기준 RB-tree에서 request를 제거
 *
 * @root: 스케줄러 내부의 RB-tree 루트
 * @rq:   제거할 request
 *
 * dispatch된 request 또는 병합으로 사라지는 request를 스케줄러 큐에서 제거.
 * RB_EMPTY_NODE: 이미 제거된 노드를 다시 제거하면 커널 패닉이 일어나므로
 * BUG_ON으로 사전 방어한다. rb_erase 후 RB_CLEAR_NODE로 "트리 밖" 상태 명시.
 *
 * 호출 체인:
 *   스케줄러 ops.dispatch_request / ops.requests_merged → [elv_rb_del]
 */
void elv_rb_del(struct rb_root *root, struct request *rq)
{
/* 이미 RB-tree에서 제거된 노드인지 확인: 중복 제거는 트리 구조 파괴 */
	BUG_ON(RB_EMPTY_NODE(&rq->rb_node));
/* RB-tree에서 노드 제거 및 재균형(rebalance) */
	rb_erase(&rq->rb_node, root);
/* rb_node를 "빈 노드"로 초기화: 이후 ELV_ON_HASH 등의 검사와 일관성 유지 */
	RB_CLEAR_NODE(&rq->rb_node);
}
EXPORT_SYMBOL(elv_rb_del);

/*
 * [한국어]
 * elv_rb_find - 특정 @sector(LBA)로 시작하는 request를 RB-tree에서 탐색
 *
 * @root:   스케줄러 내부의 RB-tree 루트
 * @sector: 찾을 LBA(논리 블록 주소)
 * @return: 정확히 @sector에서 시작하는 request; 없으면 NULL
 *
 * BFQ·mq-deadline이 front-merge 후보(뒤쪽이 @sector인 request) 탐색,
 * 또는 dispatch 순서 결정에 O(log N) 탐색으로 사용한다.
 * 주의: back-merge 후보는 끝 섹터 기준 해시(elv_rqhash_find)로 찾는다.
 *
 * 호출 체인:
 *   스케줄러 ops.request_merge → [elv_rb_find]
 */
struct request *elv_rb_find(struct rb_root *root, sector_t sector)
{
/* RB-tree 루트에서 시작해 이진 탐색 */
	struct rb_node *n = root->rb_node;
	struct request *rq;

	while (n) {
/* 현재 노드의 request 포인터 복원 */
		rq = rb_entry(n, struct request, rb_node);

		if (sector < blk_rq_pos(rq))
/* 찾는 LBA가 더 작음 → 왼쪽 서브트리(더 작은 LBA들) 탐색 */
			n = n->rb_left;
		else if (sector > blk_rq_pos(rq))
/* 찾는 LBA가 더 큼 → 오른쪽 서브트리(더 큰 LBA들) 탐색 */
			n = n->rb_right;
		else
/* 정확히 일치: 이 request의 시작 LBA가 @sector */
			return rq;
	}

/* 트리에 없음: 해당 LBA에서 시작하는 기존 request 없음 */
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
/*
 * [한국어]
 * elv_merge_requests - request 두 개가 병합됐음을 스케줄러에 알리고 자료구조 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   병합 결과로 남을 request (next를 흡수한 쪽)
 * @next: 병합되어 사라질 request (rq에 흡수됨)
 *
 * blk_attempt_req_merge()가 두 request를 하나로 합친 직후에 호출된다.
 * 스케줄러별 requests_merged 콜백으로 내부 큐 상태(예: BFQ의 두 rq 간
 * 연결)를 갱신하고, rq의 끝 섹터가 바뀌었으므로 해시를 재배치하여
 * 이후 back-merge 탐색이 올바른 위치에서 일어나도록 한다.
 *
 * 호출 체인:
 *   blk_attempt_req_merge → [elv_merge_requests]
 */
void elv_merge_requests(struct request_queue *q, struct request *rq,
			     struct request *next)
{
/* elevator_queue: 현재 활성 스케줄러 ops 접근 */
	struct elevator_queue *e = q->elevator;

/* requests_merged: BFQ·mq-deadline 등이 두 rq 간 내부 연결을 정리하는 콜백 */
	if (e->type->ops.requests_merged)
		e->type->ops.requests_merged(q, rq, next);

/* rq가 next를 흡수했으므로 끝 섹터가 바뀜 → 해시 키 재계산 후 재배치 */
	elv_rqhash_reposition(q, rq);
/* last_merge 캐시: 다음 one-hit 캐시 시도에서 이 rq가 first candidate */
	q->last_merge = rq;
}

/*
 * [한국어]
 * elv_latter_request - 스케줄러 dispatch 순서에서 @rq 다음에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 다음 request; 없으면 NULL
 *
 * 스케줄러 내부 자료구조(BFQ의 B-WF2Q 큐, mq-deadline의 fifo/RB-tree 등)에서
 * @rq 바로 뒤에 위치한 요청을 찾는다. front-merge 시 merge 체인을 따라갈
 * 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_latter_request] → 스케줄러 ops.next_request
 */
struct request *elv_latter_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 next_request 콜백: BFQ/mq-deadline 내부 dispatch 순서에서 다음 후보 */
	if (e->type->ops.next_request)
		return e->type->ops.next_request(q, rq);

	return NULL;
}

/*
 * [한국어]
 * elv_former_request - 스케줄러 dispatch 순서에서 @rq 이전에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 이전 request; 없으면 NULL
 *
 * @rq 앞의 요청을 찾아 front-merge 후보를 탐색하거나 dispatch 순서를
 * 역방향으로 추적할 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_former_request] → 스케줄러 ops.former_request
 */
struct request *elv_former_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 former_request 콜백: dispatch 순서에서 @rq 앞의 request 반환 */
	if (e->type->ops.former_request)
		return e->type->ops.former_request(q, rq);

	return NULL;
}

#define to_elv(atr) container_of_const((atr), struct elv_fs_entry, attr)

/*
 * [한국어]
 * elv_attr_show - /sys/block/<disk>/queue/iosched/<attr> 읽기 핸들러
 *
 * @kobj: elevator_queue에 내장된 kobject (iosched sysfs 노드)
 * @attr: 읽을 sysfs attribute (elv_fs_entry로 캐스팅)
 * @page: 출력 버퍼 (PAGE_SIZE 크기)
 * @return: 쓴 바이트 수 또는 에러 코드
 *
 * sysfs kobject ops.show로 등록되어 사용자가 scheduler 파라미터를 읽을 때
 * 호출된다. sysfs_lock을 잡아 elevator 구조체와의 race를 막고,
 * ELEVATOR_FLAG_DYING이면(장치 제거 중) -ENODEV를 반환해 dangling 접근을 차단.
 *
 * 호출 체인:
 *   sysfs read → elv_sysfs_ops.show → [elv_attr_show] → entry->show(e, page)
 */
static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
/* attr을 elv_fs_entry로 캐스팅: elevator 스케줄러별 sysfs 파일 설명자 */
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
/* -ENODEV: DYING 상태이거나 show 콜백 없는 경우의 기본 에러 */
	ssize_t error = -ENODEV;

/* show 콜백 없으면 읽기 불가 */
	if (!entry->show)
		return -EIO;

/* kobj로부터 상위 elevator_queue 복원 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: elevator 구조체와 elevator_exit() 간 동시 접근 보호 */
	mutex_lock(&e->sysfs_lock);
/* ELEVATOR_FLAG_DYING: 장치 제거/elevator 교체 중 → 파라미터 접근 차단 */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		error = entry->show(e, page);
	mutex_unlock(&e->sysfs_lock);
	return error;
}

/*
 * [한국어]
 * elv_attr_store - /sys/block/<disk>/queue/iosched/<attr> 쓰기 핸들러
 *
 * @kobj:   elevator_queue에 내장된 kobject
 * @attr:   쓸 sysfs attribute
 * @page:   사용자 입력 버퍼
 * @length: 입력 길이
 * @return: 소비한 바이트 수 또는 에러 코드
 *
 * 사용자가 scheduler 파라미터(예: mq-deadline의 read_expire, write_expire)를
 * 변경할 때 호출된다. sysfs_lock으로 elevator_exit()와의 race를 막고,
 * DYING 상태이면 변경을 거부한다.
 *
 * 호출 체인:
 *   sysfs write → elv_sysfs_ops.store → [elv_attr_store] → entry->store(e, page, length)
 */
static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
/* attr을 elv_fs_entry로 캐스팅 */
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
/* -ENODEV: DYING 상태 기본값 */
	ssize_t error = -ENODEV;

/* store 콜백 없으면 쓰기 불가 */
	if (!entry->store)
		return -EIO;

/* kobj에서 elevator_queue 복원 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: elevator_exit()와의 race 방지 — 파라미터 변경 중 elevator 해제 금지 */
	mutex_lock(&e->sysfs_lock);
/* DYING 상태(장치 제거 중)에서는 파라미터 쓰기 차단 */
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

/*
 * [한국어]
 * elv_unregister_queue - request_queue의 elevator sysfs/debugfs 노드 제거
 *
 * @q: elevator가 연결된 request_queue
 * @e: 제거할 elevator_queue; NULL이면 no-op
 *
 * ELEVATOR_FLAG_REGISTERED를 원자적으로 클리어하고(test_and_clear_bit), sysfs의
 * iosched kobject를 제거한다. 이 시점부터 /sys/block/<disk>/queue/iosched는
 * 접근 불가하다. elevator_exit() 또는 elevator_change_done() 안에서 호출된다.
 *
 * 호출 체인:
 *   elevator_change_done / elv_exit_and_release → [elv_unregister_queue]
 */
static void elv_unregister_queue(struct request_queue *q,
				 struct elevator_queue *e)
{
/* REGISTERED 비트를 원자적으로 클리어; 이미 클리어됐으면(중복 호출) no-op */
	if (e && test_and_clear_bit(ELEVATOR_FLAG_REGISTERED, &e->flags)) {
/* KOBJ_REMOVE uevent: udev 등이 /sys/block/.../queue/iosched 제거를 인지 */
		kobject_uevent(&e->kobj, KOBJ_REMOVE);
/* sysfs kobject 제거: /sys/block/<disk>/queue/iosched 디렉토리 삭제 */
		kobject_del(&e->kobj);

		/* unexport via debugfs before exiting sched */
/* debugfs 노출 해제: blk-mq sched 디버그 정보 제거 */
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

/*
 * [한국어]
 * elv_exit_and_release - elevator를 완전히 내리고 자원을 해제하는 rollback 경로
 *
 * @ctx: elevator 전환 컨텍스트 (전환 중 할당한 res/type 정보)
 * @q:   elevator를 제거할 request_queue
 *
 * elevator_change_done()에서 새 elevator의 sysfs 등록이 실패하면 이 함수로
 * rollback한다. queue를 freeze해 진행 중인 I/O를 멈추고, elevator_exit()으로
 * 스케줄러 상태를 해제한 뒤, sched_res와 kobject를 최종 반환한다.
 * 이 경로를 거치면 request_queue는 elevator가 없는("none") 상태가 된다.
 *
 * 호출 체인:
 *   elevator_change_done → elv_register_queue 실패 → [elv_exit_and_release]
 */
static void elv_exit_and_release(struct elv_change_ctx *ctx,
		struct request_queue *q)
{
	struct elevator_queue *e;
/* memflags: 메모리 압박 상황을 freeze 이전/이후에 복원하기 위해 저장 */
	unsigned memflags;

/* queue freeze: I/O submit/dispatch를 모두 중단 — elevator 해제 중 race 방지 */
	memflags = blk_mq_freeze_queue(q);
	mutex_lock(&q->elevator_lock);
/* 현재 elevator 포인터 보관 (elevator_exit 후 q->elevator는 NULL이 됨) */
	e = q->elevator;
/* elevator_exit: ioc 정리 + blk_mq_exit_sched(tag_set 연결 해제) */
	elevator_exit(q);
	mutex_unlock(&q->elevator_lock);
/* unfreeze: I/O 경로 재개 (elevator 없는 "none" 상태로) */
	blk_mq_unfreeze_queue(q, memflags);
	if (e) {
/* 전환 중 사전 할당한 sched_res(tag/hw_ctx 자원) 반환 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, q->tag_set);
/* kobject_put: 참조 카운트가 0이 되면 elevator_release()가 kfree 수행 */
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
/* ctx->old: elevator_switch()가 교체한 이전 elevator_queue */
		struct elevator_resources res = {
/* old elevator의 sched_res를 스택에 복사: 이후 old->elevator_data가 NULL이 돼도 접근 가능 */
			.et = ctx->old->et,
			.data = ctx->old->elevator_data
		};

/* sysfs iosched kobject 제거: 이전 스케줄러 파라미터 파일 비공개 */
		elv_unregister_queue(q, ctx->old);
/* 이전 스케줄러의 hw_queue별 자원(tag/wq 등) 해제 */
		blk_mq_free_sched_res(&res, ctx->old->type, q->tag_set);
/* kobject_put: 참조가 0이 되면 elevator_release()가 elevator_queue를 kfree */
		kobject_put(&ctx->old->kobj);
	}
	if (ctx->new) {
/* 새 elevator sysfs 등록: /sys/block/<disk>/queue/iosched 재생성 */
		ret = elv_register_queue(q, ctx->new, !ctx->no_uevent);
		if (ret)
/* 등록 실패 시 rollback: 새 elevator를 내리고 request_queue를 "none" 상태로 */
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

/*
 * [한국어]
 * elv_iosched_load_module - 아직 로드되지 않은 스케줄러 모듈을 자동 로드
 *
 * @elevator_name: 로드할 스케줄러 이름 (예: "bfq", "mq-deadline")
 *
 * 사용자가 sysfs로 특정 스케줄러를 요청했을 때, 해당 스케줄러가 아직
 * elv_list에 없으면 "<name>-iosched" 모듈을 request_module()로 자동 로드한다.
 * 이렇게 하면 "bfq-iosched.ko"를 명시적으로 insmod하지 않아도 sysfs 쓰기만으로
 * BFQ를 활성화할 수 있다. 모듈 로드 이후 elv_list에 등록되므로
 * 이후 elevator_find_get()이 성공한다.
 *
 * 호출 체인:
 *   elv_iosched_store → [elv_iosched_load_module] → request_module()
 */
static void elv_iosched_load_module(const char *elevator_name)
{
	struct elevator_type *found;

/* elv_list_lock 짧게 획득: 모듈 로드 여부만 확인하므로 빠르게 해제 */
	spin_lock(&elv_list_lock);
/* 이미 등록된 스케줄러라면 모듈 로드 불필요 */
	found = __elevator_find(elevator_name);
	spin_unlock(&elv_list_lock);

	if (!found)
/* 미등록 스케줄러: "<name>-iosched" 패턴으로 커널 모듈 자동 로드 */
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

/* elevator_lock: q->elevator 포인터를 안정적으로 읽기 위해 획득 */
	mutex_lock(&q->elevator_lock);
	if (!q->elevator) {
/* elevator 없음("none"): 현재 선택을 대괄호로 표시 */
		len += sprintf(name+len, "[none] ");
	} else {
/* elevator 있음: "none"은 대괄호 없이, 현재 스케줄러는 뒤에서 대괄호 */
		len += sprintf(name+len, "none ");
/* cur: 현재 활성 스케줄러 타입, 이후 목록 출력 시 대괄호 판단에 사용 */
		cur = q->elevator->type;
	}

/* elv_list_lock: 등록된 스케줄러 목록을 안정적으로 순회하기 위해 획득 */
	spin_lock(&elv_list_lock);
/* elv_list 순회: 등록된 모든 스케줄러를 공백으로 구분해 출력 */
	list_for_each_entry(e, &elv_list, list) {
		if (e == cur)
/* 현재 활성 스케줄러: 대괄호로 강조 (예: [mq-deadline]) */
			len += sprintf(name+len, "[%s] ", e->elevator_name);
		else
/* 비활성 스케줄러: 이름만 출력 */
			len += sprintf(name+len, "%s ", e->elevator_name);
	}
	spin_unlock(&elv_list_lock);

/* 줄바꿈으로 출력 종료 */
	len += sprintf(name+len, "\n");
	mutex_unlock(&q->elevator_lock);

	return len;
}

/*
 * [한국어]
 * elv_rb_former_request - LBA 기준 RB-tree에서 @rq 바로 앞의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 작은 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 front-merge 후보를 찾거나 dispatch 순서를
 * 역방향으로 추적할 때 사용한다. rb_prev()는 O(log N)이다.
 * elv_former_request()의 실제 구현체이며 스케줄러가 ops.former_request로
 * 이 함수를 등록한다.
 *
 * 호출 체인:
 *   elv_former_request → 스케줄러 ops.former_request → [elv_rb_former_request]
 */
struct request *elv_rb_former_request(struct request_queue *q,
				      struct request *rq)
{
/* rb_prev: RB-tree에서 LBA 기준 직전 노드 — front-merge의 빈번한 탐색 경로 */
	struct rb_node *rbprev = rb_prev(&rq->rb_node);

/* 이전 노드가 있으면 해당 request 반환; 없으면 NULL (첫 번째 LBA가 가장 작음) */
	if (rbprev)
		return rb_entry_rq(rbprev);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_former_request);

/*
 * [한국어]
 * elv_rb_latter_request - LBA 기준 RB-tree에서 @rq 바로 뒤의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 큰 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 back-merge 후보 및 dispatch 방향 탐색에 사용.
 * elv_latter_request()의 실제 구현체이며 ops.next_request로 등록된다.
 *
 * 호출 체인:
 *   elv_latter_request → 스케줄러 ops.next_request → [elv_rb_latter_request]
 */
struct request *elv_rb_latter_request(struct request_queue *q,
				      struct request *rq)
{
/* rb_next: RB-tree에서 LBA 기준 직후 노드 — back-merge·dispatch 연속성 탐색 */
	struct rb_node *rbnext = rb_next(&rq->rb_node);

/* 다음 노드가 있으면 해당 request 반환; 없으면 NULL (마지막 LBA) */
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
