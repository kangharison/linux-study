// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hierarchical Budget Worst-case Fair Weighted Fair Queueing
 * (B-WF2Q+): hierarchical scheduling algorithm by which the BFQ I/O
 * scheduler schedules generic entities. The latter can represent
 * either single bfq queues (associated with processes) or groups of
 * bfq queues (associated with cgroups).
 */

/*
 * NVMe SSD 관점 파일 요약:
 * 이 파일은 BFQ I/O 스케줄러의 핵심인 B-WF2Q+ (Budget Worst-case Fair Weighted Fair Queueing+)
 * 계층적 스케줄링 엔진을 구현한다. 블록 레이어 상위에서 bio/request가 날아오면
 * (blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)),
 * BFQ는 이 파일의 알고리즘으로 어느 bfq_queue(bfqq)의 요청을 NVMe SQ/CQ로 먼저 날릴지
 * 가상 시간(vtime)과 finish 시간을 기준으로 결정한다. 따라서 NVMe 입장에서는 이 파일이
 * doorbell을 울리기 직전, 호스트 측 최종 디스패치 우선순위를 정하는 관문이다.
 *
 * 관련 파일: block/bfq-iosched.h(구조체 정의), block/bfq-iosched.c(상위 스케줄러 루프),
 *           block/blk-mq.c, drivers/nvme/host/pci.c(SQ/CQ 처리).
 */
#include "bfq-iosched.h"


/*
 * 주요 구조체와 NVMe 동작의 연결 (필드는 bfq-iosched.h에 정의됨):
 *
 * struct bfq_entity  : 스케줄링 단위. leaf면 bfq_queue(프로세스/스레드의 I/O 흐름)을,
 *                      낮부 노드면 bfq_group(cgroup)을 나타낸다. NVMe 관점에서는
 *                      하나의 CID/IO 명령 흐름이 어느 우선순위로 doorbell에 도달할지를
 *                      결정하는 단위라 볼 수 있다. start, finish, weight, budget 필드가
 *                      NVMe SQ에 삽입되는 상대적 순서를 좌우한다.
 *   - start/finish   : 가상 시간축에서의 시작/종료 시각. finish가 작을수록 먼저 서비스.
 *   - weight         : cgroup/ioprio에서 환산된 가중치. NVMe multi-queue에서 특정 hw_queue의
 *                      blk_mq_hw_ctx에 매핑된 bfqq의 상대 대역폭을 결정.
 *   - budget         : 한 서비스 슬롯에 쓸 수 있는 최대 섹터 수. NVMe PRP/SGL 명령 하나가
 *                      얼마나 많은 섹터를 이동할지 간접적으로 제한.
 *   - sched_data     : 부모 그룹의 bfq_sched_data를 가리킴. 계층적 스케줄링의 연결고리.
 *
 * struct bfq_service_tree : 한 ioprio_class(RT/BE/IDLE) 내의 활성/유휴 엔티티 RB-tree.
 *                      NVMe에서는 동일 우선순위 클래스 안에서 다음에 doorbell을 칠
 *                      후보를 O(log N)으로 찾는 자료구조. vtime은 가상 시간 시계.
 *
 * struct bfq_sched_data : 계층의 한 노드. in_service_entity(현재 NVMe로 디스패치 중인 단위),
 *                      next_in_service(다음 후보)를 캐싱해 다음 doorbell 지연을 줄임.
 *
 * struct bfq_queue   : leaf 엔티티로 실제 request(bio)를 담는다. dispatched 필드는 NVMe
 *                      드라이버/하드웨어에 아직 완료되지 않은 in-flight CID 개수와 대응.
 *   - actuator_idx   : 다중 액츄에이터 드라이브의 특정 액츄에이터와 연결. NVMe는 물리적
 *                      플래시 채널/CE 단위로 병렬 처리하므로 이 인덱스가 실제 병렬도와
 *                      맞물릴 수 있음 (추정).
 *
 * struct bfq_data    : 장치 단위 상태. request_queue, dispatch 리스트, peak_rate 등을
 *                      가지며 NVMe controller의 한 namespace/queue와 1:1로 대응한다.
 */
/**
 * bfq_gt - compare two timestamps.
 * @a: first ts.
 * @b: second ts.
 *
 * Return @a > @b, dealing with wrapping correctly.
 */

/*
 * bfq_gt - 두 가상 타임스탬프를 래핑을 고려해 비교.
 *
 * NVMe 연결: finish/start 시간 비교는 다음 CID 묶음이 SQ의 어디에 삽입될지
 *            결정하는 데 직접 사용된다. doorbell 전에 어떤 bfqq가 먼저인지
 *            판단하는 가장 빈번한 연산 중 하나.
 */
static int bfq_gt(u64 a, u64 b) /* NVMe: doorbell 순서를 결정하는 finish/start 비교 (추정). */
{
	return (s64)(a - b) > 0; /* wraparound 고려 비교: finish가 작을수록 SQ 앞쪽 삽입. */
}

static struct bfq_entity *bfq_root_active_entity(struct rb_root *tree)
{
	struct rb_node *node = tree->rb_node; /* active tree 루트: 다음 NVMe CID 후보 집합의 머리. */

	return rb_entry(node, struct bfq_entity, rb_node); /* 루트 엔티티가 다음 doorbell 우선순위 1위 후보. */
}

static unsigned int bfq_class_idx(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf bfqq면 NVMe SQ로 직접 매핑되는 단위. */

	return bfqq ? bfqq->ioprio_class - 1 : /* RT/BE/IDLE 클래스를 service_tree 인덱스로 변환. */
		BFQ_DEFAULT_GRP_CLASS - 1; /* cgroup 그룹은 기본 클래스로 NVMe 대역폭 그룹화. */
}

unsigned int bfq_tot_busy_queues(struct bfq_data *bfqd)
{
	return bfqd->busy_queues[0] + bfqd->busy_queues[1] + /* NVMe: 총 active bfqq 수 == CID 생성 가능 흐름 수. */
		bfqd->busy_queues[2]; /* RT + BE + IDLE 큐 깊이 합산. */
}

static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 bool expiration);

static bool bfq_update_parent_budget(struct bfq_entity *next_in_service);

/**
 * bfq_update_next_in_service - update sd->next_in_service
 * @sd: sched_data for which to perform the update.
 * @new_entity: if not NULL, pointer to the entity whose activation,
 *		requeueing or repositioning triggered the invocation of
 *		this function.
 * @expiration: id true, this function is being invoked after the
 *             expiration of the in-service entity
 *
 * This function is called to update sd->next_in_service, which, in
 * its turn, may change as a consequence of the insertion or
 * extraction of an entity into/from one of the active trees of
 * sd. These insertions/extractions occur as a consequence of
 * activations/deactivations of entities, with some activations being
 * 'true' activations, and other activations being requeueings (i.e.,
 * implementing the second, requeueing phase of the mechanism used to
 * reposition an entity in its active tree; see comments on
 * __bfq_activate_entity and __bfq_requeue_entity for details). In
 * both the last two activation sub-cases, new_entity points to the
 * just activated or requeued entity.
 *
 * Returns true if sd->next_in_service changes in such a way that
 * entity->parent may become the next_in_service for its parent
 * entity.
 */

/*
 * bfq_update_next_in_service - sd->next_in_service 갱신.
 *
 * 목적: active tree에 엔티티가 추가/제거/재배치될 때, 다음에 서비스할 후보를 갱신.
 * 호출 경로(추정):
 *   blk_mq_submit_bio -> blk_mq_get_request -> ... -> bfq_add_bfqq_busy ->
 *   bfq_activate_bfqq -> bfq_activate_requeue_entity -> bfq_update_next_in_service
 * NVMe 연결: next_in_service가 갱신되면 bfq_get_next_queue()가 빠르게 다음 bfqq를
 *            골라 nvme_queue_rq()가 doorbell을 칠 수 있다. 불필요한 탐색을 줄여
 *            NVMe SQ 삽입 지연을 감소시킴.
 */
static bool bfq_update_next_in_service(struct bfq_sched_data *sd,
				       struct bfq_entity *new_entity,
				       bool expiration)
{
	struct bfq_entity *next_in_service = sd->next_in_service; /* 캐시된 다음 NVMe doorbell 후보. */
	bool parent_sched_may_change = false; /* cgroup 상위 그룹의 SQ 후보 변경 여부. */
	bool change_without_lookup = false; /* full active tree 탐색(skip) 가능하면 지연 감소. */

	/*
	 * If this update is triggered by the activation, requeueing
	 * or repositioning of an entity that does not coincide with
	 * sd->next_in_service, then a full lookup in the active tree
	 * can be avoided. In fact, it is enough to check whether the
	 * just-modified entity has the same priority as
	 * sd->next_in_service, is eligible and has a lower virtual
	 * finish time than sd->next_in_service. If this compound
	 * condition holds, then the new entity becomes the new
	 * next_in_service. Otherwise no change is needed.
	 */
	if (new_entity && new_entity != sd->next_in_service) { /* 새 CID(bio)가 기존 후보와 다를 때만 비교. */
		/*
		 * Flag used to decide whether to replace
		 * sd->next_in_service with new_entity. Tentatively
		 * set to true, and left as true if
		 * sd->next_in_service is NULL.
		 */
		change_without_lookup = true; /* 우선 탐색 없이 new_entity가 더 좋은지 평가. */

		/*
		 * If there is already a next_in_service candidate
		 * entity, then compare timestamps to decide whether
		 * to replace sd->service_tree with new_entity.
		 */
		if (next_in_service) { /* 기존 후보와 finish 시간 비교로 doorbell 순위 결정. */
			unsigned int new_entity_class_idx = /* 클래스가 다류면 RT>BE>IDLE 우선순위로 SQ 기회 차등. */
				bfq_class_idx(new_entity); /* new_entity의 ioprio_class 인덱스. */
			struct bfq_service_tree *st = /* 해당 클래스의 active tree: NVMe SQ 후보 집합. */
				sd->service_tree + new_entity_class_idx; /* service_tree[RT/BE/IDLE] 선택. */

			/* NVMe: start <= vtime이어야 SQ 진입 후보, finish가 작으면 먼저 SQ로(추정). */
			change_without_lookup =
				(new_entity_class_idx == /* 동일 클래스 내에서만 finish 비교가 의미 있음. */
				 bfq_class_idx(next_in_service) /* next_in_service와 클래스 일치 여부. */
				 && /* 아래 조건들이 모두 참이면 new_entity가 먼저 SQ로 향함. */
				 !bfq_gt(new_entity->start, st->vtime) /* start <= vtime: eligibility 조건, SQ 진입 가능. */
				 && /* finish 비교: 작을수록 doorbell 우선. */
				 bfq_gt(next_in_service->finish, /* next_in_service보다 new_entity finish가 작은지. */
					new_entity->finish)); /* new_entity finish가 더 작으면 SQ 앞쪽 배정. */
		}

		if (change_without_lookup) /* 탐색 없이 새 후보가 다음 doorbell 대상으로 확정. */
			next_in_service = new_entity; /* 다음 nvme_queue_rq()가 이 엔티티를 선택. */
	}

	if (!change_without_lookup) /* lookup needed: O(log N) active tree 탐색으로 NVMe 후보 갱신. */
		next_in_service = bfq_lookup_next_entity(sd, expiration); /* RT->BE->IDLE 순으로 다음 bfqq 탐색. */

	if (next_in_service) { /* 후보가 있으면 부모 budget 전파 및 상위 갱신. */
		bool new_budget_triggers_change = /* 자식 budget 변경이 부모 SQ 순서를 바꿀지. */
			bfq_update_parent_budget(next_in_service); /* cgroup 계층의 NVMe 대역폭 상한 재조정. */

		parent_sched_may_change = !sd->next_in_service || /* 첫 후보 선정이면 상위까지 재계산. */
			new_budget_triggers_change; /* budget 변경으로 인해 상위 doorbell 순서 변경 가능. */
	}

	sd->next_in_service = next_in_service; /* 캐시 갱신: 다음 nvme_queue_rq() 지연 감소. */

	return parent_sched_may_change; /* 상위 sched_data도 next_in_service를 다시 봐야 하면 true. */
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED

/*
 * Returns true if this budget changes may let next_in_service->parent
 * become the next_in_service entity for its parent entity.
 */

/*
 * bfq_update_parent_budget - 자식 엔티티의 budget 변경을 부모에게 전파.
 *
 * cgroup 그룹이 자식 bfqq의 budget만큼 예산을 물려받아, 계층적 공정성을 유지.
 * NVMe 연결: 그룹 단위로 NVMe 대역폭을 제한/배분할 때, 부모 그룹의 budget이
 *            실제로 NVMe에 도달하는 총 섹터 수의 상한이 됨.
 */
static bool bfq_update_parent_budget(struct bfq_entity *next_in_service)
{
	struct bfq_entity *bfqg_entity;
	struct bfq_group *bfqg;
	struct bfq_sched_data *group_sd;
	bool ret = false;

	group_sd = next_in_service->sched_data; /* 부모 그룹의 sched_data: NVMe 대역폭 분배 단위. */

	bfqg = container_of(group_sd, struct bfq_group, sched_data); /* cgroup 그룹 객체 획득. */
	/*
	 * bfq_group's my_entity field is not NULL only if the group
	 * is not the root group. We must not touch the root entity
	 * as it must never become an in-service entity.
	 */
	bfqg_entity = bfqg->my_entity; /* 루트가 아닌 cgroup 엔티티면 NVMe SQ 후보로 간주. */
	if (bfqg_entity) { /* 그룹 엔티티가 존재하면 자식 budget을 상속. */
		if (bfqg_entity->budget > next_in_service->budget) /* 부모 예산이 더 크면 순서 변경 가능. */
			ret = true; /* 상위 service tree 재배치(requeue)가 필요함을 표시. */
		bfqg_entity->budget = next_in_service->budget; /* 그룹 단위 NVMe 명령 섹터 상한 동기화. */
	}

	return ret;
}

/*
 * This function tells whether entity stops being a candidate for next
 * service, according to the restrictive definition of the field
 * next_in_service. In particular, this function is invoked for an
 * entity that is about to be set in service.
 *
 * If entity is a queue, then the entity is no longer a candidate for
 * next service according to the that definition, because entity is
 * about to become the in-service queue. This function then returns
 * true if entity is a queue.
 *
 * In contrast, entity could still be a candidate for next service if
 * it is not a queue, and has more than one active child. In fact,
 * even if one of its children is about to be set in service, other
 * active children may still be the next to serve, for the parent
 * entity, even according to the above definition. As a consequence, a
 * non-queue entity is not a candidate for next-service only if it has
 * only one active child. And only if this condition holds, then this
 * function returns true for a non-queue entity.
 */

/*
 * bfq_no_longer_next_in_service - 엔티티가 다음 서비스 후보에서 빠지는지 판단.
 *
 * queue면 true(곧 in-service가 되므로). 그룹이면 active 자식이 하나뿐일 때 true.
 * NVMe 연결: 선택된 bfqq를 active tree에서 제거해 중복 doorbell 후보를 방지.
 */
static bool bfq_no_longer_next_in_service(struct bfq_entity *entity)
{
	struct bfq_group *bfqg;

	if (bfq_entity_to_bfqq(entity)) /* leaf bfqq면 곧 in-service가 되어 후보 집합에서 제외. */
		return true; /* NVMe: 선택된 bfqq는 active tree에서 빼 중복 doorbell 방지. */

	bfqg = container_of(entity, struct bfq_group, entity); /* cgroup 그룹 엔티티 변환. */

	/*
	 * The field active_entities does not always contain the
	 * actual number of active children entities: it happens to
	 * not account for the in-service entity in case the latter is
	 * removed from its active tree (which may get done after
	 * invoking the function bfq_no_longer_next_in_service in
	 * bfq_get_next_queue). Fortunately, here, i.e., while
	 * bfq_no_longer_next_in_service is not yet completed in
	 * bfq_get_next_queue, bfq_active_extract has not yet been
	 * invoked, and thus active_entities still coincides with the
	 * actual number of active entities.
	 */
	if (bfqg->active_entities == 1) /* 활성 자식이 하나뿐이면 부모도 후보에서 제거. */
		return true; /* NVMe: 부모 그룹도 다음 doorbell 후보에서 빠짐. */

	return false; /* NVMe: 부모에게 여전히 다른 active 자식이 있으면 후보로 남김. */
}

static void bfq_inc_active_entities(struct bfq_entity *entity)
{
	struct bfq_sched_data *sd = entity->sched_data; /* entity의 부모 sched_data. */
	struct bfq_group *bfqg = container_of(sd, struct bfq_group, sched_data);

	if (bfqg != bfqg->bfqd->root_group) /* 루트 그룹은 카운트 제외: 전체 NVMe 큐 집계의 기준. */
		bfqg->active_entities++; /* cgroup 내 active CID 흐름 수 증가. */
}

static void bfq_dec_active_entities(struct bfq_entity *entity)
{
	struct bfq_sched_data *sd = entity->sched_data; /* 부모 sched_data. */
	struct bfq_group *bfqg = container_of(sd, struct bfq_group, sched_data);

	if (bfqg != bfqg->bfqd->root_group)
		bfqg->active_entities--; /* cgroup 내 active CID 흐름 수 감소. */
}

#else /* CONFIG_BFQ_GROUP_IOSCHED */

static bool bfq_update_parent_budget(struct bfq_entity *next_in_service) /* cgroup 비활성 시 stub. */
{
	return false; /* NVMe: 그룹 계층 없으면 부모 budget 전파 불필요. */
}

static bool bfq_no_longer_next_in_service(struct bfq_entity *entity) /* cgroup 비활성 시 stub. */
{
	return true; /* NVMe: 그룹 계층 없으면 모든 엔티티가 queue로 간주되어 후보에서 제거. */
}

static void bfq_inc_active_entities(struct bfq_entity *entity) /* cgroup 비활성 시 stub. */
{
}

static void bfq_dec_active_entities(struct bfq_entity *entity) /* cgroup 비활성 시 stub. */
{
}

#endif /* CONFIG_BFQ_GROUP_IOSCHED */

/*
 * Shift for timestamp calculations.  This actually limits the maximum
 * service allowed in one timestamp delta (small shift values increase it),
 * the maximum total weight that can be used for the queues in the system
 * (big shift values increase it), and the period of virtual time
 * wraparounds.
 */

/*
 * WFQ_SERVICE_SHIFT: 서비스 양을 가상 시간으로 변환할 때 쓰이는 고정 소수점 shift.
 *
 * NVMe 연결: shift 값은 한 번에 doorbell로 날릴 수 있는 최대 예산(budget)의
 *            정밀도와 시스템 전체 weight 합산 범위를 트레이드오프한다. NVMe SSD는
 *            초당 수천~수만 CID를 처리하므로 정밀도가 latency 변동에 영향을 줌.
 */
#define WFQ_SERVICE_SHIFT	22 /* NVMe 고속 CQ완료에 맞춘 vtime 정밀도/weight 범위 트레이드오프. */


/*
 * bfq_entity_to_bfqq - 엔티티가 leaf queue를 표현하는지 확인하고 bfq_queue 반환.
 *
 * NVMe 연결: leaf면 실제 request(bio)를 담는 bfqq이므로 nvme_queue_rq()에서
 *            선택된 엔티티가 NVMe SQ/CQ로 연결되는 최종 단위인지 판별.
 */
struct bfq_queue *bfq_entity_to_bfqq(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = NULL; /* NVMe: leaf queue가 아니면 NULL(그룹 엔티티). */

	if (!entity->my_sched_data) /* my_sched_data가 없으면 leaf: 실제 request(bio) 보유. */
		bfqq = container_of(entity, struct bfq_queue, entity); /* bfqq <=> NVMe SQ/CQ CID 흐름. */

	return bfqq; /* leaf면 bfqq 반환, 그룹이면 NULL. */
}


/**
 * bfq_delta - map service into the virtual time domain.
 * @service: amount of service.
 * @weight: scale factor (weight of an entity or weight sum).
 */

/*
 * bfq_delta - 서비스량(섹터)을 가상 시간 증분으로 변환.
 *
 * NVMe 연결: NVMe 명령 하나의 PRP/SGL 길이(섹터)가 weight로 정규화되어
 *            가상 시간이 흐른다. 가벼운 작업일수록(낮은 weight) 같은 섹터라도
 *            더 많은 vtime을 소비하므로, doorbell 우선순위가 밀린다.
 */
static u64 bfq_delta(unsigned long service, unsigned long weight) /* NVMe: 섹터수/weight -> vtime 증분. */
{
	return div64_ul((u64)service << WFQ_SERVICE_SHIFT, weight); /* weight 클수록 같은 섹터에 소모 vtime 감소 -> doorbell 유리. */
}

/**
 * bfq_calc_finish - assign the finish time to an entity.
 * @entity: the entity to act upon.
 * @service: the service to be charged to the entity.
 */

/*
 * bfq_calc_finish - 엔티티의 finish 시간을 계산: F = S + budget/weight.
 *
 * NVMe 연결: finish 값이 작은 bfqq일수록 먼저 nvme_submit_cmd()를 통해 doorbell.
 *            budget은 한 번에 NVMe에 허용하는 최대 섹터 수를, weight는 cgroup/
 *            ioprio 기반 상대 우선순위를 반영.
 */
static void bfq_calc_finish(struct bfq_entity *entity, unsigned long service)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf면 완료 로그에 bfqq 식별. */

	entity->finish = entity->start + /* finish = start + service/weight: NVMe SQ 삽입 우선순위 값. */
		bfq_delta(service, entity->weight); /* budget만큼의 vtime을 weight로 정규화. */

	if (bfqq) { /* NVMe: 디버그 로그에 bfqq별 doorbell 우선순위 기록. */
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			"calc_finish: serv %lu, w %d",
			service, entity->weight);
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			"calc_finish: start %llu, finish %llu, delta %llu",
			entity->start, entity->finish,
			bfq_delta(service, entity->weight));
	}
}

/**
 * bfq_entity_of - get an entity from a node.
 * @node: the node field of the entity.
 *
 * Convert a node pointer to the relative entity.  This is used only
 * to simplify the logic of some functions and not as the generic
 * conversion mechanism because, e.g., in the tree walking functions,
 * the check for a %NULL value would be redundant.
 */
struct bfq_entity *bfq_entity_of(struct rb_node *node)
{
	struct bfq_entity *entity = NULL; /* rb_node -> bfq_entity 매핑: SQ 후보 객체 복원. */

	if (node) /* NULL guard: active tree 순회 중 안전한 변환. */
		entity = rb_entry(node, struct bfq_entity, rb_node); /* NVMe: tree node에서 doorbell 후보 복원. */

	return entity; /* rb_node에서 변환된 bfq_entity 반환. */
}

/**
 * bfq_extract - remove an entity from a tree.
 * @root: the tree root.
 * @entity: the entity to remove.
 */
static void bfq_extract(struct rb_root *root, struct bfq_entity *entity)
{
	entity->tree = NULL; /* active/idle tree 소속 해제: 더 이상 SQ 후보 집합에 없음. */
	rb_erase(&entity->rb_node, root); /* O(log N) 제거: 다음 doorbell 탐색 집합 갱신. */
}

/**
 * bfq_idle_extract - extract an entity from the idle tree.
 * @st: the service tree of the owning @entity.
 * @entity: the entity being removed.
 */
static void bfq_idle_extract(struct bfq_service_tree *st,
			     struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf면 idle_list에서도 제거. */
	struct rb_node *next;

	if (entity == st->first_idle) { /* NVMe: idle tree의 finish 최소 후보가 빠지면 다음 후보로 갱신. */
		next = rb_next(&entity->rb_node); /* 중위 순회 다음 node가 새 first_idle. */
		st->first_idle = bfq_entity_of(next); /* NVMe: 다음 vtime 점프 시 복귀할 후보 갱신. */
	}

	if (entity == st->last_idle) { /* NVMe: idle tree의 finish 최대 후보가 빠지면 갱신. */
		next = rb_prev(&entity->rb_node); /* 중위 순회 이전 node가 새 last_idle. */
		st->last_idle = bfq_entity_of(next); /* NVMe: vtime 점프 한계점 재설정. */
	}

	bfq_extract(&st->idle, entity); /* NVMe: idle tree에서 제거 -> 다시 active SQ 후보 가능. */

	if (bfqq) /* NVMe: idle_list에서 leaf bfqq 제거. */
		list_del(&bfqq->bfqq_list); /* per-bfqd idle 후보 리스트에서 제거. */
}

/**
 * bfq_insert - generic tree insertion.
 * @root: tree root.
 * @entity: entity to insert.
 *
 * This is used for the idle and the active tree, since they are both
 * ordered by finish time.
 */
static void bfq_insert(struct rb_root *root, struct bfq_entity *entity) /* NVMe: finish 순으로 SQ 후보 tree에 삽입. */
{
	struct bfq_entity *entry; /* NVMe: 현재 비교 중인 doorbell 후보. */
	struct rb_node **node = &root->rb_node; /* 삽입 위치 탐색 포인터. */
	struct rb_node *parent = NULL; /* RB tree 부모: NVMe 후보 집합의 경로. */

	while (*node) { /* O(log N) finish 비교 탐색 루프. */
		parent = *node; /* NVMe: 현재 subtree의 루트 후보. */
		entry = rb_entry(parent, struct bfq_entity, rb_node); /* 현재 node의 SQ 후보. */

		if (bfq_gt(entry->finish, entity->finish)) /* 삽입할 entity finish가 더 작으면. */
			node = &parent->rb_left; /* NVMe: 더 빠른 doorbell 후보는 왼쪽 subtree. */
		else /* 삽입할 entity finish가 같거나 크면. */
			node = &parent->rb_right; /* NVMe: 늦은 doorbell 후보는 오른쪽 subtree. */
	}

	rb_link_node(&entity->rb_node, parent, node); /* NVMe: leaf 위치에 새 doorbell 후보 연결. */
	rb_insert_color(&entity->rb_node, root); /* RB tree balance 복구. */

	entity->tree = root; /* NVMe: active/idle tree 소속 기록. */
}

/**
 * bfq_update_min - update the min_start field of a entity.
 * @entity: the entity to update.
 * @node: one of its children.
 *
 * This function is called when @entity may store an invalid value for
 * min_start due to updates to the active tree.  The function  assumes
 * that the subtree rooted at @node (which may be its left or its right
 * child) has a valid min_start value.
 */
static void bfq_update_min(struct bfq_entity *entity, struct rb_node *node)
{
	struct bfq_entity *child;

	if (node) { /* 자식 min_start가 현재 entity의 min_start보다 작으면 갱신. */
		child = rb_entry(node, struct bfq_entity, rb_node); /* subtree의 최소 start 후보. */
		if (bfq_gt(entity->min_start, child->min_start)) /* NVMe: eligible 범위를 subtree로 전파. */
			entity->min_start = child->min_start; /* 부모가 자식의 가장 빠른 doorbell 시점을 알게 됨. */
	}
}

/**
 * bfq_update_active_node - recalculate min_start.
 * @node: the node to update.
 *
 * @node may have changed position or one of its children may have moved,
 * this function updates its min_start value.  The left and right subtrees
 * are assumed to hold a correct min_start value.
 */
static void bfq_update_active_node(struct rb_node *node)
{
	struct bfq_entity *entity = rb_entry(node, struct bfq_entity, rb_node);

	entity->min_start = entity->start; /* 자신의 start로 초기화 후 자식과 병합. */
	bfq_update_min(entity, node->rb_right); /* 오른쪽 subtree의 min_start 반영. */
	bfq_update_min(entity, node->rb_left); /* 왼쪽 subtree의 min_start 반영. */
}

/**
 * bfq_update_active_tree - update min_start for the whole active tree.
 * @node: the starting node.
 *
 * @node must be the deepest modified node after an update.  This function
 * updates its min_start using the values held by its children, assuming
 * that they did not change, and then updates all the nodes that may have
 * changed in the path to the root.  The only nodes that may have changed
 * are the ones in the path or their siblings.
 */
static void bfq_update_active_tree(struct rb_node *node)
{
	struct rb_node *parent;

up: /* 수정된 node에서 루트까지 min_start 전파 루프. */
	bfq_update_active_node(node); /* 현재 node의 min_start 재계산. */

	parent = rb_parent(node); /* 부모로 이동하며 NVMe 후보 집합의 메타데이터 갱신. */
	if (!parent) /* 루트에 도달하면 active tree 메타데이터 갱신 완료. */
		return; /* NVMe: 다음 doorbell 탐색을 위한 tree 상태 일관성 확보. */

	if (node == parent->rb_left && parent->rb_right) /* 형제 subtree도 min_start 갱신 필요. */
		bfq_update_active_node(parent->rb_right); /* 오른쪽 형제의 eligible 범위 재평가. */
	else if (parent->rb_left) /* 왼쪽 형제가 있으면 갱신. */
		bfq_update_active_node(parent->rb_left); /* 왼쪽 형제의 eligible 범위 재평가. */

	node = parent; /* 루트 방향으로 한 단계 이동. */
	goto up; /* 루트까지 min_start 전파 반복. */
}

/**
 * bfq_active_insert - insert an entity in the active tree of its
 *                     group/device.
 * @st: the service tree of the entity.
 * @entity: the entity being inserted.
 *
 * The active tree is ordered by finish time, but an extra key is kept
 * per each node, containing the minimum value for the start times of
 * its children (and the node itself), so it's possible to search for
 * the eligible node with the lowest finish time in logarithmic time.
 */

/*
 * bfq_active_insert - 엔티티를 active tree에 삽입.
 *
 * 호출 경로: bfq_update_fin_time_enqueue -> bfq_active_insert
 *           -> (이후) bfq_get_next_queue -> nvme_queue_rq
 * NVMe 연결: active tree에 들어간 bfqq는 NVMe SQ에 진입할 후보가 된다.
 *            active_list[actuator_idx]에도 등록되어 다중 액츄에이터 NVMe SSD의
 *            병렬 인젝션 후보를 빠르게 순회할 수 있게 한다.
 */
static void bfq_active_insert(struct bfq_service_tree *st,
			      struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf면 active_list[actuator_idx]에도 등록. */
	struct rb_node *node = &entity->rb_node; /* 방금 삽입된 node부터 min_start 전파 시작점. */

	bfq_insert(&st->active, entity); /* finish 순으로 NVMe SQ 후보 tree에 삽입. */

	if (node->rb_left) /* 왼쪽 자식 있으면 deepest modified node 갱신. */
		node = node->rb_left; /* min_start 전파의 시작점을 왼쪽으로 이동. */
	else if (node->rb_right) /* 왼쪽 없으면 오른쪽 자식으로. */
		node = node->rb_right; /* min_start 전파의 시작점을 오른쪽으로 이동. */

	bfq_update_active_tree(node); /* 삽입으로 변경된 subtree의 min_start를 루트까지 전파. */

	if (bfqq) /* leaf bfqq면 actuator별 active_list에 추가. */
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->active_list[bfqq->actuator_idx]); /* NVMe multi-actuator: per-actuator doorbell 후보 리스트. */

	bfq_inc_active_entities(entity); /* cgroup active 카운터 증가. */
}

/**
 * bfq_ioprio_to_weight - calc a weight from an ioprio.
 * @ioprio: the ioprio value to convert.
 */
unsigned short bfq_ioprio_to_weight(int ioprio)
{
	return (IOPRIO_NR_LEVELS - ioprio) * BFQ_WEIGHT_CONVERSION_COEFF; /* ioprio 낮을수록(우선) weight 커짐 -> NVMe SQ 앞쪽. */
}

/**
 * bfq_weight_to_ioprio - calc an ioprio from a weight.
 * @weight: the weight value to convert.
 *
 * To preserve as much as possible the old only-ioprio user interface,
 * 0 is used as an escape ioprio value for weights (numerically) equal or
 * larger than IOPRIO_NR_LEVELS * BFQ_WEIGHT_CONVERSION_COEFF.
 */
static unsigned short bfq_weight_to_ioprio(int weight)
{
	return max_t(int, 0, /* weight를 사용자 ioprio로 역변환. */
		     IOPRIO_NR_LEVELS - weight / BFQ_WEIGHT_CONVERSION_COEFF); /* NVMe QoS 표시용. */
}

static void bfq_get_entity(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	if (bfqq) { /* leaf bfqq 참조 카운트 증가: in-flight CID 동안 객체 유지. */
		bfqq->ref++; /* NVMe: doorbell ~ CQ 완료 사이 bfqq 해제 방지. */
		bfq_log_bfqq(bfqq->bfqd, bfqq, "get_entity: %p %d",
			     bfqq, bfqq->ref);
	}
}

/**
 * bfq_find_deepest - find the deepest node that an extraction can modify.
 * @node: the node being removed.
 *
 * Do the first step of an extraction in an rb tree, looking for the
 * node that will replace @node, and returning the deepest node that
 * the following modifications to the tree can touch.  If @node is the
 * last node in the tree return %NULL.
 */
static struct rb_node *bfq_find_deepest(struct rb_node *node)
{
	struct rb_node *deepest;

	if (!node->rb_right && !node->rb_left) /* leaf node 제거: 부모가 deepest. */
		deepest = rb_parent(node); /* NVMe: active tree 제거 후 min_start 갱신 시작점. */
	else if (!node->rb_right) /* 왼쪽 자식만 있음. */
		deepest = node->rb_left; /* 왼쪽 subtree가 deepest modified. */
	else if (!node->rb_left) /* 오른쪽 자식만 있음. */
		deepest = node->rb_right; /* 오른쪽 subtree가 deepest modified. */
	else { /* 양쪽 자식 모두 있음: successor 찾기. */
		deepest = rb_next(node); /* 중위 순회 다음 node가 replacement candidate. */
		if (deepest->rb_right) /* successor의 오른쪽 자식이 deepest일 수 있음. */
			deepest = deepest->rb_right; /* NVMe: tree 구조 변경 최심부 갱신. */
		else if (rb_parent(deepest) != node) /* successor가 직접 자식이 아니면 부모가 deepest. */
			deepest = rb_parent(deepest); /* NVMe: 제거 후 min_start 갱신 시작점. */
	}

	return deepest;
}

/**
 * bfq_active_extract - remove an entity from the active tree.
 * @st: the service_tree containing the tree.
 * @entity: the entity being removed.
 */

/*
 * bfq_active_extract - 엔티티를 active tree에서 제거.
 *
 * NVMe 연결: 선택된 bfqq를 active tree에서 빼면 다음 doorbell 때 중복 선택을
 *            막는다. active_list에서도 제거하여 in-flight CID가 없는 후보 집합을
 *            정확히 유지.
 */
static void bfq_active_extract(struct bfq_service_tree *st,
			       struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct rb_node *node;

	node = bfq_find_deepest(&entity->rb_node); /* 제거로 변경되는 최심부 node 탐색. */
	bfq_extract(&st->active, entity); /* NVMe: 선택된 bfqq를 SQ 후보 집합에서 제거. */

	if (node) /* tree 구조가 변경되었으면 min_start 루트까지 갱신. */
		bfq_update_active_tree(node); /* NVMe: 다음 doorbell 탐색을 위한 tree 메타데이터 복구. */
	if (bfqq) /* active_list에서도 제거. */
		list_del(&bfqq->bfqq_list);

	bfq_dec_active_entities(entity); /* cgroup active 카운터 감소. */
}

/**
 * bfq_idle_insert - insert an entity into the idle tree.
 * @st: the service tree containing the tree.
 * @entity: the entity to insert.
 */

/*
 * bfq_idle_insert - 엔티티를 idle tree로 이동.
 *
 * NVMe 연결: 일시적으로 요청이 끊긴 bfqq는 idle로 빠지며, 새 CID가 도착하면
 *            다시 active로 복귀. NVMe의 낮은 큐 깊이(queue depth) 환경에서는
 *            idle 전환/복귀가 빈번해 SQ 입장률을 좌우함.
 */
static void bfq_idle_insert(struct bfq_service_tree *st,
			    struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct bfq_entity *first_idle = st->first_idle; /* idle tree의 finish 최소/최대 후보. */
	struct bfq_entity *last_idle = st->last_idle; /* vtime 점프 기준점. */

	if (!first_idle || bfq_gt(first_idle->finish, entity->finish)) /* 새 엔티티가 idle 최우선이면 갱신. */
		st->first_idle = entity; /* 다음 vtime 점프 시 먼저 active로 복귀할 후보. */
	if (!last_idle || bfq_gt(entity->finish, last_idle->finish)) /* 새 엔티티가 idle 최후위면 갱신. */
		st->last_idle = entity; /* vtime 점프 한계점 갱신. */

	bfq_insert(&st->idle, entity); /* idle tree 삽입: 잠시 후 도착할 CID를 기다림. */

	if (bfqq)
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->idle_list); /* per-bfqd idle list: 빠른 순회용. */
}

/**
 * bfq_forget_entity - do not consider entity any longer for scheduling
 * @st: the service tree.
 * @entity: the entity being removed.
 * @is_in_service: true if entity is currently the in-service entity.
 *
 * Forget everything about @entity. In addition, if entity represents
 * a queue, and the latter is not in service, then release the service
 * reference to the queue (the one taken through bfq_get_entity). In
 * fact, in this case, there is really no more service reference to
 * the queue, as the latter is also outside any service tree. If,
 * instead, the queue is in service, then __bfq_bfqd_reset_in_service
 * will take care of putting the reference when the queue finally
 * stops being served.
 */

/*
 * bfq_forget_entity - 엔티티를 스케줄러에서 완전히 제거.
 *
 * NVMe 연결: 더 이상 NVMe doorbell 후보가 아님. wsum에서 weight를 빼서
 *            가상 시간 흐름 비율을 조정. bfqq가 아물도 참조하지 않으면 메모리 해제.
 */
static void bfq_forget_entity(struct bfq_service_tree *st,
			      struct bfq_entity *entity,
			      bool is_in_service)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	entity->on_st_or_in_serv = false; /* NVMe: 더 이상 SQ 후보 또는 서비스 중 아님. */
	st->wsum -= entity->weight; /* 총 weight 감소: vtime 흐름 비율 조정. */
	if (bfqq && !is_in_service) /* in-service가 아니면 service ref 해제. */
		bfq_put_queue(bfqq); /* NVMe: 더 이상 CID 후보가 아닌 bfqq 메모리 반환 가능. */
}

/**
 * bfq_put_idle_entity - release the idle tree ref of an entity.
 * @st: service tree for the entity.
 * @entity: the entity being released.
 */
void bfq_put_idle_entity(struct bfq_service_tree *st, struct bfq_entity *entity)
{
	bfq_idle_extract(st, entity); /* idle tree에서 제거. */
	bfq_forget_entity(st, entity, /* in-service 였으면 ref는 상위 리셋에서 처리. */
			  entity == entity->sched_data->in_service_entity); /* NVMe: 현재 서비스 중이었는지 확인. */
}

/**
 * bfq_forget_idle - update the idle tree if necessary.
 * @st: the service tree to act upon.
 *
 * To preserve the global O(log N) complexity we only remove one entry here;
 * as the idle tree will not grow indefinitely this can be done safely.
 */

/*
 * bfq_forget_idle - idle tree 정리 및 vtime 전진.
 *
 * NVMe 연결: active가 비어 있고 idle 엔티티들의 finish가 vtime보다 작거나 같으면
 *            vtime을 점프시켜 다음 CID 도착 시 지나친 공정성을 회복. NVMe SSD의
 *            낮은 seek penalty 때문에 이런 가상 시간 보정이 latency에 직접 영향.
 */
static void bfq_forget_idle(struct bfq_service_tree *st)
{
	struct bfq_entity *first_idle = st->first_idle; /* idle tree 경계 후보. */
	struct bfq_entity *last_idle = st->last_idle; /* vtime 점프 한계. */

	if (RB_EMPTY_ROOT(&st->active) && last_idle && /* active 비어 있고 idle finish가 vtime 이하이면. */
	    /* NVMe: 후보가 없으면 vtime을 idle finish까지 점프(추정). */
	    !bfq_gt(last_idle->finish, st->vtime)) {
		/*
		 * Forget the whole idle tree, increasing the vtime past
		 * the last finish time of idle entities.
		 */
		st->vtime = last_idle->finish; /* NVMe: vtime을 idle finish까지 점프해 공정성 회복. */
	}

	if (first_idle && !bfq_gt(first_idle->finish, st->vtime)) /* vtime이 first_idle finish를 지나면. */
		bfq_put_idle_entity(st, first_idle); /* idle tree 정리: 메모리/후보 집합 최소화. */
}

struct bfq_service_tree *bfq_entity_service_tree(struct bfq_entity *entity)
{
	struct bfq_sched_data *sched_data = entity->sched_data; /* entity가 속한 계층의 sched_data. */
	unsigned int idx = bfq_class_idx(entity); /* RT/BE/IDLE 클래스 인덱스. */

	return sched_data->service_tree + idx; /* NVMe: 우선순위 클래스별 SQ 후보 tree 반환. */
}

/*
 * Update weight and priority of entity. If update_class_too is true,
 * then update the ioprio_class of entity too.
 *
 * The reason why the update of ioprio_class is controlled through the
 * last parameter is as follows. Changing the ioprio class of an
 * entity implies changing the destination service trees for that
 * entity. If such a change occurred when the entity is already on one
 * of the service trees for its previous class, then the state of the
 * entity would become more complex: none of the new possible service
 * trees for the entity, according to bfq_entity_service_tree(), would
 * match any of the possible service trees on which the entity
 * is. Complex operations involving these trees, such as entity
 * activations and deactivations, should take into account this
 * additional complexity.  To avoid this issue, this function is
 * invoked with update_class_too unset in the points in the code where
 * entity may happen to be on some tree.
 */

/*
 * __bfq_entity_update_weight_prio - 엔티티의 weight/priority 변경을 반영.
 *
 * 호출 경로: blk_ioc_set_ioprio 같은 상위 인터페이스 -> bfq_set_next_ioprio ->
 *           __bfq_entity_update_weight_prio (추정)
 * NVMe 연결: ioprio/weight가 바뀌면 해당 bfqq가 NVMe SQ에서 차지하는 상대적
 *            대역폭이 변하고, service tree가 바뀔 수 있다. weight 카운터 트리를
 *            갱신해 asymmetric scenario(다른 weight 혼재)를 탐지하여 NVMe의 병렬
 *            인젝션 결정에 반영.
 */
struct bfq_service_tree *
__bfq_entity_update_weight_prio(struct bfq_service_tree *old_st,
				struct bfq_entity *entity,
				bool update_class_too)
{
	struct bfq_service_tree *new_st = old_st; /* 클래스 변경 없으면 동일 service tree. */

	if (entity->prio_changed) { /* ioprio/weight가 사용자에 의해 변경됨. */
		struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf bfqq의 ioprio 갱신 대상. */
		unsigned int prev_weight, new_weight; /* NVMe: SQ에서 차지할 상대 대역폭 전/후. */

		/* Matches the smp_wmb() in bfq_group_set_weight: doorbell 가시성/순서 보장을 위한 배리어 짝. */
		smp_rmb(); /* NVMe: weight 변경이 다른 CPU의 doorbell/CQ 경로에 보이도록 메모리 순서 보장. */
		old_st->wsum -= entity->weight; /* 기존 weight를 총합에서 제거. */

		if (entity->new_weight != entity->orig_weight) { /* weight 값이 실제로 변경되었을 때. */
			if (entity->new_weight < BFQ_MIN_WEIGHT || /* 허용 범위를 벗어난 weight 클리핑. */
			    entity->new_weight > BFQ_MAX_WEIGHT) { /* NVMe: 비정상 weight는 최소/최대로 강제. */
				pr_crit("update_weight_prio: new_weight %d\n", /* 커널 경고: 잘못된 NVMe QoS 설정. */
					entity->new_weight); /* 잘못된 weight 값 기록. */
				if (entity->new_weight < BFQ_MIN_WEIGHT) /* 하한 클리핑. */
					entity->new_weight = BFQ_MIN_WEIGHT; /* NVMe: 최소 대역폭 보장. */
				else /* 상한 클리핑. */
					entity->new_weight = BFQ_MAX_WEIGHT; /* NVMe: 최대 대역폭 상한. */
			}
			entity->orig_weight = entity->new_weight; /* 원본 weight 갱신. */
			if (bfqq) /* leaf면 ioprio도 역산. */
				bfqq->ioprio = /* 사용자 인터페이스용 ioprio 갱신. */
				  bfq_weight_to_ioprio(entity->orig_weight); /* NVMe QoS 표시 역변환. */
		}

		if (bfqq && update_class_too) /* tree에 없을 때만 클래스 변경 가능. */
			bfqq->ioprio_class = bfqq->new_ioprio_class; /* NVMe: RT/BE/IDLE 클래스 재배정. */

		/*
		 * Reset prio_changed only if the ioprio_class change
		 * is not pending any longer.
		 */
		if (!bfqq || bfqq->ioprio_class == bfqq->new_ioprio_class) /* 클래스 변경 완료 시 플래그 클리어. */
			entity->prio_changed = 0; /* NVMe: 다음 doorbell 때 새 weight 적용 완료. */

		/*
		 * NOTE: here we may be changing the weight too early,
		 * this will cause unfairness.  The correct approach
		 * would have required additional complexity to defer
		 * weight changes to the proper time instants (i.e.,
		 * when entity->finish <= old_st->vtime).
		 */
		new_st = bfq_entity_service_tree(entity); /* 클래스 변경 시 service_tree 이동. */

		prev_weight = entity->weight; /* 이전 NVMe 대역폭 가중치. */
		new_weight = entity->orig_weight * /* wr_coeff(임시 latency 우선) 포함 최종 weight. */
			     (bfqq ? bfqq->wr_coeff : 1); /* NVMe: wr_coeff>1이면 SQ에서 latency 우선. */
		/*
		 * If the weight of the entity changes, and the entity is a
		 * queue, remove the entity from its old weight counter (if
		 * there is a counter associated with the entity).
		 */
		if (prev_weight != new_weight && bfqq) /* weight가 변했으면 weight counter tree 갱신. */
			bfq_weights_tree_remove(bfqq); /* NVMe: asymmetric weight 집계에서 제거. */
		entity->weight = new_weight; /* 최종 weight 확정: doorbell 순서/대역폭에 직접 반영. */
		/*
		 * Add the entity, if it is not a weight-raised queue,
		 * to the counter associated with its new weight.
		 */
		if (prev_weight != new_weight && bfqq && bfqq->wr_coeff == 1) /* weight-raised 아닐 때만 카운터 추가. */
			bfq_weights_tree_add(bfqq); /* NVMe: 새 weight로 asymmetric scenario 집계. */

		new_st->wsum += entity->weight; /* 새 service tree 총 weight 갱신. */

		if (new_st != old_st) /* 클래스 변경으로 service_tree가 바뀌면. */
			entity->start = new_st->vtime; /* NVMe: 새 클래스 tree의 현재 vtime에서 SQ 경쟁 시작. */
	}

	return new_st;
}

/**
 * bfq_bfqq_served - update the scheduler status after selection for
 *                   service.
 * @bfqq: the queue being served.
 * @served: bytes to transfer.
 *
 * NOTE: this can be optimized, as the timestamps of upper level entities
 * are synchronized every time a new bfqq is selected for service.  By now,
 * we keep it to better check consistency.
 */

/*
 * bfq_bfqq_served - bfqq가 실제로 서비스된 후 상태 갱신.
 *
 * 호출 경로: blk_mq_complete_request에서 완료된 섹터 수를 받아 호출 (추정):
 *           nvme_irq -> nvme_complete_rq -> blk_mq_complete_request ->
 *           bfq_completed_request -> bfq_bfqq_served
 * NVMe 연결: 완료된 섹터 수(served)만큼 vtime을 전진. NVMe CQ에서 CID 완료
 *            시점과 연동되어 다음 doorbell의 순서가 재계산된다.
 */
void bfq_bfqq_served(struct bfq_queue *bfqq, int served)
{
	struct bfq_entity *entity = &bfqq->entity; /* 완료된 CID에 해당하는 bfqq 엔티티. */
	struct bfq_service_tree *st; /* NVMe: 완료 시 vtime을 전진시킬 service tree. */

	if (!bfqq->service_from_backlogged) /* 첫 서비스 시점 기록. */
		bfqq->first_IO_time = jiffies; /* NVMe: 이 bfqq의 첫 CID 완료 시점. */

	if (bfqq->wr_coeff > 1) /* weight-raised 상태면 wr 서비스량 누적. */
		bfqq->service_from_wr += served; /* NVMe: latency boost 한도 추적. */

	bfqq->service_from_backlogged += served; /* busy 상태에서 완료된 총 섹터. */
	/* NVMe: 완료된 CID만큼 루트까지 vtime을 전진. */
	for_each_entity(entity) { /* NVMe: 루트까지 완료된 섹터만큼 vtime 전진. */
		st = bfq_entity_service_tree(entity); /* 각 계층의 service tree 선택. */

		entity->service += served; /* 현재 service slot에서 완료된 섹터 누적. */

		st->vtime += bfq_delta(served, st->wsum); /* NVMe: CQ 완료만큼 가상 시간 전진 -> 다음 doorbell 순서 재조정. */
		bfq_forget_idle(st); /* vtime이 idle finish를 지나면 idle tree 정리. */
	}
	bfq_log_bfqq(bfqq->bfqd, bfqq, "bfqq_served %d secs", served);
}

/**
 * bfq_bfqq_charge_time - charge an amount of service equivalent to the length
 *			  of the time interval during which bfqq has been in
 *			  service.
 * @bfqd: the device
 * @bfqq: the queue that needs a service update.
 * @time_ms: the amount of time during which the queue has received service
 *
 * If a queue does not consume its budget fast enough, then providing
 * the queue with service fairness may impair throughput, more or less
 * severely. For this reason, queues that consume their budget slowly
 * are provided with time fairness instead of service fairness. This
 * goal is achieved through the BFQ scheduling engine, even if such an
 * engine works in the service, and not in the time domain. The trick
 * is charging these queues with an inflated amount of service, equal
 * to the amount of service that they would have received during their
 * service slot if they had been fast, i.e., if their requests had
 * been dispatched at a rate equal to the estimated peak rate.
 *
 * It is worth noting that time fairness can cause important
 * distortions in terms of bandwidth distribution, on devices with
 * internal queueing. The reason is that I/O requests dispatched
 * during the service slot of a queue may be served after that service
 * slot is finished, and may have a total processing time loosely
 * correlated with the duration of the service slot. This is
 * especially true for short service slots.
 */

/*
 * bfq_bfqq_charge_time - 예산을 느리게 소모하는 큐에 시간 기반 공정성 적용.
 *
 * NVMe 연결: NVMe SSD는 seek penalty가 작아 낮은 throughput 큐가 장치를
 *            오래 붙잡으면 다른 큐의 latency가 크게 저하될 수 있다. peak_rate를
 *            기준으로 시간을 섹터로 환산해 charge하면, 느린 큐도 가상 시간상으로
 *            공정하게 퇴출되어 다음 doorbell 기회를 다른 큐에 넘긴다.
 */
void bfq_bfqq_charge_time(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  unsigned long time_ms)
{
	struct bfq_entity *entity = &bfqq->entity; /* 느린 bfqq 엔티티. */
	unsigned long timeout_ms = jiffies_to_msecs(bfq_timeout); /* NVMe timeout을 ms로 변환. */
	unsigned long bounded_time_ms = min(time_ms, timeout_ms); /* timeout 이상 charge 방지. */
	int serv_to_charge_for_time = /* 시간을 peak_rate 기준 섹터로 환산. */
		(bfqd->bfq_max_budget * bounded_time_ms) / timeout_ms; /* NVMe: 느린 큐도 시간만큼 vtime 소모. */
	int tot_serv_to_charge = max(serv_to_charge_for_time, entity->service); /* 이미 실제 서비스보다 작게 charge 안 함. */

	/* Increase budget to avoid inconsistencies: NVMe SQ에서 예산 초과 불일치 방지. */
	if (tot_serv_to_charge > entity->budget) /* charge 양이 예산 초과 시. */
		entity->budget = tot_serv_to_charge; /* NVMe: 현재 service slot의 섹터 상한 확장. */

	bfq_bfqq_served(bfqq, /* 완료 처리처럼 vtime 전진. */
			max_t(int, 0, tot_serv_to_charge - entity->service)); /* NVMe: 시간 공정성만큼 doorbell 기회 양도. */
}

static void bfq_update_fin_time_enqueue(struct bfq_entity *entity,
					struct bfq_service_tree *st,
					bool backshifted)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	/*
	 * When this function is invoked, entity is not in any service
	 * tree, then it is safe to invoke next function with the last
	 * parameter set (see the comments on the function).
	 */
	st = __bfq_entity_update_weight_prio(st, entity, true); /* weight/class 갱신 후 service_tree 재확정. */
	bfq_calc_finish(entity, entity->budget); /* NVMe: budget 기준 finish 계산 -> SQ 삽입 우선순위. */

	/*
	 * If some queues enjoy backshifting for a while, then their
	 * (virtual) finish timestamps may happen to become lower and
	 * lower than the system virtual time.	In particular, if
	 * these queues often happen to be idle for short time
	 * periods, and during such time periods other queues with
	 * higher timestamps happen to be busy, then the backshifted
	 * timestamps of the former queues can become much lower than
	 * the system virtual time. In fact, to serve the queues with
	 * higher timestamps while the ones with lower timestamps are
	 * idle, the system virtual time may be pushed-up to much
	 * higher values than the finish timestamps of the idle
	 * queues. As a consequence, the finish timestamps of all new
	 * or newly activated queues may end up being much larger than
	 * those of lucky queues with backshifted timestamps. The
	 * latter queues may then monopolize the device for a lot of
	 * time. This would simply break service guarantees.
	 *
	 * To reduce this problem, push up a little bit the
	 * backshifted timestamps of the queue associated with this
	 * entity (only a queue can happen to have the backshifted
	 * flag set): just enough to let the finish timestamp of the
	 * queue be equal to the current value of the system virtual
	 * time. This may introduce a little unfairness among queues
	 * with backshifted timestamps, but it does not break
	 * worst-case fairness guarantees.
	 *
	 * As a special case, if bfqq is weight-raised, push up
	 * timestamps much less, to keep very low the probability that
	 * this push up causes the backshifted finish timestamps of
	 * weight-raised queues to become higher than the backshifted
	 * finish timestamps of non weight-raised queues.
	 */
	/* NVMe: 과거 finish를 끌어올려 starvation 방지(추정). */
	if (backshifted && bfq_gt(st->vtime, entity->finish)) { /* NVMe: finish가 vtime보다 과거면 starvation 방지. */
		unsigned long delta = st->vtime - entity->finish; /* 뒤처진 만큼의 vtime 차이. */

		if (bfqq) /* weight-raised 큐는 덜 밀어 latency 우선 유지. */
			delta /= bfqq->wr_coeff; /* NVMe: latency-sensitive 큐의 doorbell 기회 보호. */

		entity->start += delta; /* start/finish를 vtime 방향으로 이동. */
		entity->finish += delta; /* NVMe: SQ 후보 집합 내에서 과도한 우선권 완화. */
	}

	bfq_active_insert(st, entity); /* NVMe: 갱신된 finish로 active tree/SQ 후보 집합에 삽입. */
}

/**
 * __bfq_activate_entity - handle activation of entity.
 * @entity: the entity being activated.
 * @non_blocking_wait_rq: true if entity was waiting for a request
 *
 * Called for a 'true' activation, i.e., if entity is not active and
 * one of its children receives a new request.
 *
 * Basically, this function updates the timestamps of entity and
 * inserts entity into its active tree, after possibly extracting it
 * from its idle tree.
 */

/*
 * __bfq_activate_entity - 비활성 엔티티를 active tree로 진정한 활성화.
 *
 * 호출 경로: bio 도착 -> bfq_add_bfqq_busy -> bfq_activate_bfqq ->
 *           bfq_activate_requeue_entity -> __bfq_activate_entity
 * NVMe 연결: 프로세스가 새 bio를 날리면(즉, NVMe CID 후보가 생기면) 해당 bfqq를
 *            active tree에 넣어 doorbell 우선순위 경쟁에 참여시킨다. non_blocking
 *            모드에서는 finish를 vtime으로 밀어 seeky 큐의 latency를 낮춘다.
 */
static void __bfq_activate_entity(struct bfq_entity *entity,
				  bool non_blocking_wait_rq)
{
	struct bfq_service_tree *st = bfq_entity_service_tree(entity); /* entity의 클래스별 service tree. */
	bool backshifted = false; /* non_blocking_wait_rq 시 finish backshift 여부. */
	unsigned long long min_vstart; /* NVMe: 다음 doorbell 경쟁 시작 vtime 하한. */

	/* See comments on bfq_fqq_update_budg_for_activation */
	/* NVMe: vtime이 finish를 지나쳤으면 start를 뒤로 당겨 latency 개선(추정). */
	if (non_blocking_wait_rq && bfq_gt(st->vtime, entity->finish)) { /* NVMe: vtime이 finish를 지나면 seeky 큐 latency 개선을 위해 backshift. */
		backshifted = true; /* NVMe: start/finish를 vtime 근처로 당김(추정). */
		min_vstart = entity->finish; /* finish를 최소 시작점으로 유지. */
	} else /* 일반적인 경우 현재 vtime에서 시작. */
		min_vstart = st->vtime; /* NVMe: 다음 CID 도착 시 vtime 기준 경쟁 시작. */

	if (entity->tree == &st->idle) { /* idle tree에 있으면 추출 후 active로. */
		/*
		 * Must be on the idle tree, bfq_idle_extract() will
		 * check for that.
		 */
		bfq_idle_extract(st, entity); /* NVMe: idle -> active 복귀: 새 CID가 SQ 후보가 됨. */
		entity->start = bfq_gt(min_vstart, entity->finish) ? /* finish보다 min_vstart가 크면 그쪽 사용. */
			min_vstart : entity->finish; /* NVMe: 공정한 다음 doorbell 시작점. */
	} else /* 완전히 비활성 상태였으면 새로 활성화. */
		/*
		 * The finish time of the entity may be invalid, and
		 * it is in the past for sure, otherwise the queue
		 * would have been on the idle tree.
		 */
		entity->start = min_vstart; /* NVMe: 새 CID 흐름의 doorbell 경쟁 시작점. */
		st->wsum += entity->weight; /* service tree 총 weight에 추가. */
		/*
		 * entity is about to be inserted into a service tree,
		 * and then set in service: get a reference to make
		 * sure entity does not disappear until it is no
		 * longer in service or scheduled for service.
		 */
		bfq_get_entity(entity); /* NVMe: in-service 또는 scheduled 동안 bfqq 메모리 유지. */

		entity->on_st_or_in_serv = true; /* NVMe: 이제 SQ 후보 집합에 포함됨. */
	}

	bfq_update_fin_time_enqueue(entity, st, backshifted); /* finish 계산 후 active tree 삽입. */
}

/**
 * __bfq_requeue_entity - handle requeueing or repositioning of an entity.
 * @entity: the entity being requeued or repositioned.
 *
 * Requeueing is needed if this entity stops being served, which
 * happens if a leaf descendant entity has expired. On the other hand,
 * repositioning is needed if the next_inservice_entity for the child
 * entity has changed. See the comments inside the function for
 * details.
 *
 * Basically, this function: 1) removes entity from its active tree if
 * present there, 2) updates the timestamps of entity and 3) inserts
 * entity back into its active tree (in the new, right position for
 * the new values of the timestamps).
 */

/*
 * __bfq_requeue_entity - in-service거나 active인 엔티티를 재배치(requeue).
 *
 * NVMe 연결: 현재 NVMe에 디스패치 중이던 bfqq가 만료(expire)되면, 받은 서비스만큼
 *            start/finish를 갱신하고 active tree에 다시 넣어 다음 doorbell 때
 *            공정한 순서로 경쟁하게 한다.
 */
static void __bfq_requeue_entity(struct bfq_entity *entity)
{
	struct bfq_sched_data *sd = entity->sched_data;
	struct bfq_service_tree *st = bfq_entity_service_tree(entity); /* 현재 service tree. */

	if (entity == sd->in_service_entity) { /* NVMe: 현재 NVMe에 디스패치 중이던 엔티티 만료. */
		/*
		 * We are requeueing the current in-service entity,
		 * which may have to be done for one of the following
		 * reasons:
		 * - entity represents the in-service queue, and the
		 *   in-service queue is being requeued after an
		 *   expiration;
		 * - entity represents a group, and its budget has
		 *   changed because one of its child entities has
		 *   just been either activated or requeued for some
		 *   reason; the timestamps of the entity need then to
		 *   be updated, and the entity needs to be enqueued
		 *   or repositioned accordingly.
		 *
		 * In particular, before requeueing, the start time of
		 * the entity must be moved forward to account for the
		 * service that the entity has received while in
		 * service. This is done by the next instructions. The
		 * finish time will then be updated according to this
		 * new value of the start time, and to the budget of
		 * the entity.
		 */
		bfq_calc_finish(entity, entity->service); /* 받은 서비스만큼 finish 갱신. */
		entity->start = entity->finish; /* NVMe: 다음 doorbell 경쟁 시작점을 finish로 이동. */
		/*
		 * In addition, if the entity had more than one child
		 * when set in service, then it was not extracted from
		 * the active tree. This implies that the position of
		 * the entity in the active tree may need to be
		 * changed now, because we have just updated the start
		 * time of the entity, and we will update its finish
		 * time in a moment (the requeueing is then, more
		 * precisely, a repositioning in this case). To
		 * implement this repositioning, we: 1) dequeue the
		 * entity here, 2) update the finish time and requeue
		 * the entity according to the new timestamps below.
		 */
		if (entity->tree) /* active tree에 남아 있었으면(다중 자식 그룹) 제거. */
			bfq_active_extract(st, entity); /* NVMe: 만료된 엔티티를 SQ 후보 집합에서 제거. */
	} else { /* The entity is already active, and not in service: next_in_service 변경으로 재배치. */
		/*
		 * In this case, this function gets called only if the
		 * next_in_service entity below this entity has
		 * changed, and this change has caused the budget of
		 * this entity to change, which, finally implies that
		 * the finish time of this entity must be
		 * updated. Such an update may cause the scheduling,
		 * i.e., the position in the active tree, of this
		 * entity to change. We handle this change by: 1)
		 * dequeueing the entity here, 2) updating the finish
		 * time and requeueing the entity according to the new
		 * timestamps below. This is the same approach as the
		 * non-extracted-entity sub-case above.
		 */
		bfq_active_extract(st, entity); /* NVMe: 다음 doorbell 후보 변동으로 위치 재조정. */
	}

	bfq_update_fin_time_enqueue(entity, st, false); /* 갱신된 finish로 active tree 재삽입. */
}

static void __bfq_activate_requeue_entity(struct bfq_entity *entity,
					  bool non_blocking_wait_rq)
{
	struct bfq_service_tree *st = bfq_entity_service_tree(entity);

	if (entity->sched_data->in_service_entity == entity || /* 현재 NVMe 서비스 중이거나. */
	    entity->tree == &st->active) /* active tree에 있으면 requeue/reposition. */
		 /*
		  * in service or already queued on the active tree,
		  * requeue or reposition
		  */
		__bfq_requeue_entity(entity); /* NVMe: doorbell 후보 집합 내 재배치. */
	else /* in-service 아니면 service 카운터 초기화. */
		/*
		 * Not in service and not queued on its active tree:
		 * the activity is idle and this is a true activation.
		 */
		__bfq_activate_entity(entity, non_blocking_wait_rq); /* NVMe: 새 CID 흐름을 SQ 후보 집합에 추가. */
}


/**
 * bfq_activate_requeue_entity - activate or requeue an entity representing a
 *				 bfq_queue, and activate, requeue or reposition
 *				 all ancestors for which such an update becomes
 *				 necessary.
 * @entity: the entity to activate.
 * @non_blocking_wait_rq: true if this entity was waiting for a request
 * @requeue: true if this is a requeue, which implies that bfqq is
 *	     being expired; thus ALL its ancestors stop being served and must
 *	     therefore be requeued
 * @expiration: true if this function is being invoked in the expiration path
 *             of the in-service queue
 */

/*
 * bfq_activate_requeue_entity - 엔티티와 모든 조상을 activate/requeue.
 *
 * NVMe 연결: cgroup 깊이만큼 재귀적으로 조상을 갱신. 어느 cgroup의 bfqq라도
 *            NVMe CID를 생성하면, 루트까지 경로의 모든 그룹이 active tree에서
 *            갱신되어 doorbell 순서가 올바르게 반영된다.
 */
static void bfq_activate_requeue_entity(struct bfq_entity *entity,
					bool non_blocking_wait_rq,
					bool requeue, bool expiration)
{
	/* NVMe: cgroup 루트까지 조상의 SQ 후보 상태를 갱신. */
	for_each_entity(entity) { /* NVMe: cgroup 루트까지 조상의 SQ 후보 상태 연쇄 갱신. */
		__bfq_activate_requeue_entity(entity, non_blocking_wait_rq); /* 현재 계층 active/requeue 처리. */
		if (!bfq_update_next_in_service(entity->sched_data, entity, /* next_in_service 갱신. */
						expiration) && !requeue) /* NVMe: 상위로 전파할 변경이 없고 requeue 아니면 중단. */
			break; /* 더 이상 상위 SQ 후보 갱신 불필요. */
	}
}

/**
 * __bfq_deactivate_entity - update sched_data and service trees for
 * entity, so as to represent entity as inactive
 * @entity: the entity being deactivated.
 * @ins_into_idle_tree: if false, the entity will not be put into the
 *			idle tree.
 *
 * If necessary and allowed, puts entity into the idle tree. NOTE:
 * entity may be on no tree if in service.
 */

/*
 * __bfq_deactivate_entity - 엔티티를 비활성화하고 필요하면 idle tree로.
 *
 * NVMe 연결: bfqq의 pending request가 0이 되면(즉, NVMe SQ에 넣을 후보가
 *            없어지면) active tree에서 제거. finish > vtime이면 idle tree에
 *            남겨두어 잠시 후 도착할 CID에 대한 공정성을 보존.
 */
bool __bfq_deactivate_entity(struct bfq_entity *entity, bool ins_into_idle_tree)
{
	struct bfq_sched_data *sd = entity->sched_data;
	struct bfq_service_tree *st; /* NVMe: 제거할 SQ 후보 tree. */
	bool is_in_service; /* 현재 NVMe에 디스패치 중이었는지. */

	if (!entity->on_st_or_in_serv) /* NVMe: 이미 후보 집합에 없으면 아무것도 안 함. */
					* entity never activated, or
					* already inactive
					*/
		return false; /* deactivation 불필요. */

	/*
	 * If we get here, then entity is active, which implies that
	 * bfq_group_set_parent has already been invoked for the group
	 * represented by entity. Therefore, the field
	 * entity->sched_data has been set, and we can safely use it.
	 */
	st = bfq_entity_service_tree(entity); /* 클래스별 service tree 획득. */
	is_in_service = entity == sd->in_service_entity; /* NVMe: 현재 서비스 중이면 ref 처리 방식 다름. */

	bfq_calc_finish(entity, entity->service); /* 지금까지 받은 서비스로 finish 확정. */

	if (is_in_service) /* NVMe: 현재 NVMe 서비스 중이던 bfqq 만료. */
		sd->in_service_entity = NULL; /* 현재 서비스 표시 해제. */
	else /* finish > vtime이면 idle tree에 보관. */
		/*
		 * Non in-service entity: nobody will take care of
		 * resetting its service counter on expiration. Do it
		 * now.
		 */
		entity->service = 0; /* NVMe: 다음 doorbell 서비스 slot 누적값 초기화. */

	if (entity->tree == &st->active) /* active tree에 있으면 제거. */
		bfq_active_extract(st, entity); /* NVMe: SQ 후보 집합에서 제거. */
	else if (!is_in_service && entity->tree == &st->idle) /* idle tree에 있으면 제거. */
		bfq_idle_extract(st, entity); /* NVMe: idle 후보 집합에서 제거. */

	if (!ins_into_idle_tree || !bfq_gt(entity->finish, st->vtime)) /* idle 승격 불가 또는 finish <= vtime이면. */
		bfq_forget_entity(st, entity, is_in_service); /* NVMe: 완전히 후보 집합에서 제거, ref 해제. */
	else
		bfq_idle_insert(st, entity); /* NVMe: 잠시 후 CID 도착 시 빠른 SQ 복귀를 위해 idle 보관. */

	return true;
}

/**
 * bfq_deactivate_entity - deactivate an entity representing a bfq_queue.
 * @entity: the entity to deactivate.
 * @ins_into_idle_tree: true if the entity can be put into the idle tree
 * @expiration: true if this function is being invoked in the expiration path
 *             of the in-service queue
 */

/*
 * bfq_deactivate_entity - bfqq를 대표하는 엔티티와 상위 그룹들을 비활성화.
 *
 * NVMe 연결: 하위 bfqq가 텅 비면 상위 cgroup도 inactive가 되면서 NVMe 대역폭
 *            배분 트리에서 제거. idle tree로 승격시켜 향후 CID 도착 시 빠르게
 *            복귀할 수 있게 한다.
 */
static void bfq_deactivate_entity(struct bfq_entity *entity,
				  bool ins_into_idle_tree,
				  bool expiration)
{
	struct bfq_sched_data *sd; /* 각 계층의 sched_data. */
	struct bfq_entity *parent = NULL; /* NVMe: 상위 그룹 deactivation 반복용. */

	for_each_entity_safe(entity, parent) { /* NVMe: leaf에서 cgroup 루트까지 상향 비활성화. */
		sd = entity->sched_data; /* 현재 계층 sched_data. */

		if (!__bfq_deactivate_entity(entity, ins_into_idle_tree)) { /* 이미 비활성이면 상향 중단. */
			/*
			 * entity is not in any tree any more, so
			 * this deactivation is a no-op, and there is
			 * nothing to change for upper-level entities
			 * (in case of expiration, this can never
			 * happen).
			 */
			return; /* NVMe: 더 이상 상위 SQ 후보 제거 불필요. */
		}

		if (sd->next_in_service == entity) /* 제거된 entity가 다음 후보였으면. */
			/*
			 * entity was the next_in_service entity,
			 * then, since entity has just been
			 * deactivated, a new one must be found.
			 */
			bfq_update_next_in_service(sd, NULL, expiration); /* NVMe: 새 doorbell 후보 탐색. */

		if (sd->next_in_service || sd->in_service_entity) /* NVMe: 여전히 활성 후보/서비스 중이면 상향 중단. */
			/*
			 * The parent entity is still active, because
			 * either next_in_service or in_service_entity
			 * is not NULL. So, no further upwards
			 * deactivation must be performed.  Yet,
			 * next_in_service has changed.	Then the
			 * schedule does need to be updated upwards.
			 *
			 * NOTE If in_service_entity is not NULL, then
			 * next_in_service may happen to be NULL,
			 * although the parent entity is evidently
			 * active. This happens if 1) the entity
			 * pointed by in_service_entity is the only
			 * active entity in the parent entity, and 2)
			 * according to the definition of
			 * next_in_service, the in_service_entity
			 * cannot be considered as
			 * next_in_service. See the comments on the
			 * definition of next_in_service for details.
			 */
			break; /* 상위 cgroup은 SQ 후보 집합에 남음. */
		}

		/*
		 * If we get here, then the parent is no more
		 * backlogged and we need to propagate the
		 * deactivation upwards. Thus let the loop go on.
		 */

		/*
		 * Also let parent be queued into the idle tree on
		 * deactivation, to preserve service guarantees, and
		 * assuming that who invoked this function does not
		 * need parent entities too to be removed completely.
		 */
		ins_into_idle_tree = true; /* NVMe: 상위 그룹도 idle 보관 허용. */
	}

	/*
	 * If the deactivation loop is fully executed, then there are
	 * no more entities to touch and next loop is not executed at
	 * all. Otherwise, requeue remaining entities if they are
	 * about to stop receiving service, or reposition them if this
	 * is not the case.
	 */
	entity = parent; /* 상향 중단 지점부터 다음 후보 갱신 시작. */
	for_each_entity(entity) { /* NVMe: 중단 지점에서 루트까지 next_in_service 재계산. */
		/*
		 * Invoke __bfq_requeue_entity on entity, even if
		 * already active, to requeue/reposition it in the
		 * active tree (because sd->next_in_service has
		 * changed)
		 */
		__bfq_requeue_entity(entity); /* active tree에서 재배치. */

		sd = entity->sched_data; /* 현재 계층 sched_data. */
		if (!bfq_update_next_in_service(sd, entity, expiration) && /* next_in_service 갱신. */
		    !expiration) /* NVMe: 만료 경로 아니면 상위 전파 중단 가능. */
			/*
			 * next_in_service unchanged or not causing
			 * any change in entity->parent->sd, and no
			 * requeueing needed for expiration: stop
			 * here.
			 */
			break; /* 더 이상 상위 SQ 후보 갱신 불필요. */
	}
}

/**
 * bfq_calc_vtime_jump - compute the value to which the vtime should jump,
 *                       if needed, to have at least one entity eligible.
 * @st: the service tree to act upon.
 *
 * Assumes that st is not empty.
 */

/*
 * bfq_calc_vtime_jump - 적어도 하나의 엔티티가 eligible해지도록 vtime 점프값 계산.
 *
 * NVMe 연결: active tree의 모든 후보가 아직 미래(start > vtime)라면, vtime을
 *            min_start로 당겨서 다음 doorbell에 적합한 bfqq를 찾는다. 이는 NVMe
 *            SSD에서 짧은 idle gap 후에도 공정한 디스패치를 보장.
 */
static u64 bfq_calc_vtime_jump(struct bfq_service_tree *st)
{
	struct bfq_entity *root_entity = bfq_root_active_entity(&st->active); /* active tree 루트: min_start 후보. */

	if (bfq_gt(root_entity->min_start, st->vtime)) /* 모든 후보가 미래에 있으면. */
		return root_entity->min_start; /* NVMe: vtime을 가장 빠른 doorbell 가능 시점으로 점프. */

	return st->vtime; /* 이미 eligible 후보가 있으면 vtime 유지. */
}

static void bfq_update_vtime(struct bfq_service_tree *st, u64 new_value)
{
	if (new_value > st->vtime) { /* vtime 전진이 필요하면. */
		st->vtime = new_value; /* NVMe: 가상 시간을 전진시켜 다음 doorbell 후보 eligible화. */
		bfq_forget_idle(st); /* vtime이 지난 idle tree 정리. */
	}
}

/**
 * bfq_first_active_entity - find the eligible entity with
 *                           the smallest finish time
 * @st: the service tree to select from.
 * @vtime: the system virtual to use as a reference for eligibility
 *
 * This function searches the first schedulable entity, starting from the
 * root of the tree and going on the left every time on this side there is
 * a subtree with at least one eligible (start <= vtime) entity. The path on
 * the right is followed only if a) the left subtree contains no eligible
 * entities and b) no eligible entity has been found yet.
 */

/*
 * bfq_first_active_entity - vtime 기준 eligible하면서 finish가 가장 작은 엔티티 탐색.
 *
 * NVMe 연결: active tree를 왼쪽 우선으로 낮추면서, start <= vtime인 후보를 찾는다.
 *            찾은 엔티티가 곧 nvme_queue_rq()를 통해 doorbell을 칠 다음 bfqq가 됨.
 */
static struct bfq_entity *bfq_first_active_entity(struct bfq_service_tree *st,
						  u64 vtime)
{
	struct bfq_entity *entry, *first = NULL; /* NVMe: 다음 doorbell 대상 탐색. */
	struct rb_node *node = st->active.rb_node; /* active tree 루트부터 탐색. */

	while (node) { /* O(log N) eligible + 최소 finish 탐색 루프. */
		entry = rb_entry(node, struct bfq_entity, rb_node); /* 현재 node의 SQ 후보. */
left: /* 왼쪽 subtree 먼저 탐색: finish가 작은 후보 우선. */
		if (!bfq_gt(entry->start, vtime)) /* start <= vtime: eligible -> doorbell 가능. */
			first = entry; /* NVMe: 일단 후보 저장, 더 왼쪽에 있을 수 있어 계속 탐색. */

		if (node->rb_left) { /* 왼쪽 자식의 min_start가 vtime 이하면 더 좋은 후보 존재. */
			entry = rb_entry(node->rb_left, /* 왼쪽 subtree 루트. */
					 struct bfq_entity, rb_node); /* NVMe: 왼쪽에서 더 낮은 finish 후보 탐색. */
			if (!bfq_gt(entry->min_start, vtime)) /* 왼쪽 subtree에 eligible 엔티티가 있음. */
				node = node->rb_left; /* 왼쪽으로 이동. */
				goto left; /* NVMe: 왼쪽 subtree에서 최소 finish doorbell 후보 계속 탐색. */
			}
		}
		if (first) /* eligible 후보를 찾았으면 탐색 종료. */
			break; /* NVMe: 다음 nvme_queue_rq()가 사용할 bfqq 확정. */
		node = node->rb_right; /* 왼쪽에 eligible 없으면 오른쪽 탐색. */
	}

	return first; /* NVMe: 다음 doorbell 우선순위 1위 bfqq(또는 그룹). */
}

/**
 * __bfq_lookup_next_entity - return the first eligible entity in @st.
 * @st: the service tree.
 * @in_service: whether or not there is an in-service entity for the sched_data
 *	this active tree belongs to.
 *
 * If there is no in-service entity for the sched_data st belongs to,
 * then return the entity that will be set in service if:
 * 1) the parent entity this st belongs to is set in service;
 * 2) no entity belonging to such parent entity undergoes a state change
 * that would influence the timestamps of the entity (e.g., becomes idle,
 * becomes backlogged, changes its budget, ...).
 *
 * In this first case, update the virtual time in @st too (see the
 * comments on this update inside the function).
 *
 * In contrast, if there is an in-service entity, then return the
 * entity that would be set in service if not only the above
 * conditions, but also the next one held true: the currently
 * in-service entity, on expiration,
 * 1) gets a finish time equal to the current one, or
 * 2) is not eligible any more, or
 * 3) is idle.
 */

/*
 * __bfq_lookup_next_entity - 한 service tree에서 다음 서비스 대상 반환.
 *
 * NVMe 연결: in-service가 없을 때만 vtime을 점프시켜 후보를 찾는다. in-service가
 *            있으면(=NVMe가 이미 어떤 bfqq의 CID들을 처리 중이면) 현재 서비스를
 *            방해하지 않고 다음 후볼만 미리 계산.
 */
static struct bfq_entity *
__bfq_lookup_next_entity(struct bfq_service_tree *st, bool in_service)
{
	struct bfq_entity *entity; /* 선택된 NVMe doorbell 후보. */
	u64 new_vtime; /* eligible 후보를 만들 vtime 점프 값. */

	if (RB_EMPTY_ROOT(&st->active)) /* active tree 비어 있으면 후보 없음. */
		return NULL; /* NVMe: 해당 클래스에 SQ로 전달할 CID 후보 없음. */

	/*
	 * Get the value of the system virtual time for which at
	 * least one entity is eligible.
	 */
	new_vtime = bfq_calc_vtime_jump(st); /* 후보가 없으면 vtime 점프값 계산. */

	/*
	 * If there is no in-service entity for the sched_data this
	 * active tree belongs to, then push the system virtual time
	 * up to the value that guarantees that at least one entity is
	 * eligible. If, instead, there is an in-service entity, then
	 * do not make any such update, because there is already an
	 * eligible entity, namely the in-service one (even if the
	 * entity is not on st, because it was extracted when set in
	 * service).
	 */
	if (!in_service) /* NVMe: 현재 NVMe에 서비스 중인 bfqq가 없을 때만 vtime 전진. */
		bfq_update_vtime(st, new_vtime); /* vtime 점프로 eligible 후보 확보. */

	entity = bfq_first_active_entity(st, new_vtime); /* NVMe: 새 vtime 기준 최우선 doorbell 후보. */

	return entity; /* 최종 선택된 NVMe SQ 후보 엔티티. */
}

/**
 * bfq_lookup_next_entity - return the first eligible entity in @sd.
 * @sd: the sched_data.
 * @expiration: true if we are on the expiration path of the in-service queue
 *
 * This function is invoked when there has been a change in the trees
 * for sd, and we need to know what is the new next entity to serve
 * after this change.
 */

/*
 * bfq_lookup_next_entity - 우선순위 클래스별로 다음 서비스 엔티티를 선택.
 *
 * NVMe 연결: RT > BE > IDLE 순으로 service tree를 검사. IDLE 클래스는 주기적으로
 *            강제 서비스되어 starvation을 방지. 선택된 bfqq가 NVMe SQ에 들어갈
 *            다음 CID 묶음을 결정.
 */
static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 bool expiration)
{
	struct bfq_service_tree *st = sd->service_tree; /* RT service tree 시작. */
	struct bfq_service_tree *idle_class_st = st + (BFQ_IOPRIO_CLASSES - 1); /* IDLE 클래스 tree. */
	struct bfq_entity *entity = NULL; /* NVMe: 최종 선택된 doorbell 후보. */
	int class_idx = 0; /* RT 클래스부터 시작. */

	/*
	 * Choose from idle class, if needed to guarantee a minimum
	 * bandwidth to this class (and if there is some active entity
	 * in idle class). This should also mitigate
	 * priority-inversion problems in case a low priority task is
	 * holding file system resources.
	 */
	if (time_is_before_jiffies(sd->bfq_class_idle_last_service + /* IDLE 클래스 강제 서비스 주기 경과 시. */
				   /* NVMe: IDLE 클래스도 주기적으로 SQ 기회를 줘 starvation 방지. */
				   BFQ_CL_IDLE_TIMEOUT)) { /* NVMe: IDLE 큐도 주기적으로 SQ 기회 부여. */
		if (!RB_EMPTY_ROOT(&idle_class_st->active)) /* IDLE 클래스에 후보가 있으면. */
			class_idx = BFQ_IOPRIO_CLASSES - 1; /* NVMe: IDLE 클래스를 다음 doorbell 대상으로. */
		/* About to be served if backlogged, or not yet backlogged */
		sd->bfq_class_idle_last_service = jiffies; /* IDLE 서비스 시점 갱신. */
	}

	/*
	 * Find the next entity to serve for the highest-priority
	 * class, unless the idle class needs to be served.
	 */
	for (; class_idx < BFQ_IOPRIO_CLASSES; class_idx++) { /* RT -> BE -> IDLE 순 SQ 후보 탐색 루프. */
		/*
		 * If expiration is true, then bfq_lookup_next_entity
		 * is being invoked as a part of the expiration path
		 * of the in-service queue. In this case, even if
		 * sd->in_service_entity is not NULL,
		 * sd->in_service_entity at this point is actually not
		 * in service any more, and, if needed, has already
		 * been properly queued or requeued into the right
		 * tree. The reason why sd->in_service_entity is still
		 * not NULL here, even if expiration is true, is that
		 * sd->in_service_entity is reset as a last step in the
		 * expiration path. So, if expiration is true, tell
		 * __bfq_lookup_next_entity that there is no
		 * sd->in_service_entity.
		 */
		entity = __bfq_lookup_next_entity(st + class_idx, /* 현재 클래스에서 후보 탐색. */
						  sd->in_service_entity && /* NVMe: 현재 서비스 중이면 vtime 전진 억제. */
						  !expiration); /* 만료 경로에서는 in_service_entity가 이미 해제 예정. */

		if (entity) /* 후보 찾으면 더 낮은 클래스 탐색 중단. */
			break; /* NVMe: 우선순위가 높은 클래스의 bfqq를 doorbell 대상으로 선택. */
	}

	return entity; /* NVMe: RT/BE/IDLE 우선순위를 거쳐 선정된 최종 doorbell 후보. */
}


/*
 * next_queue_may_preempt - 선점 가능한 다음 후보가 있는지 확인.
 *
 * NVMe 연결: next_in_service != in_service_entity면, 현재 NVMe에서 처리 중인
 *            bfqq를 만료시키고 더 우선순위가 높은 bfqq로 doorbell을 전환할 수
 *            있음을 의미. NVMe의 낮은 큐 깊이에서는 선점이 latency 향상에 유효.
 */
bool next_queue_may_preempt(struct bfq_data *bfqd)
{
	struct bfq_sched_data *sd = &bfqd->root_group->sched_data; /* 루트 그룹 sched_data. */

	return sd->next_in_service != sd->in_service_entity; /* NVMe: 더 우선순위 높은 bfqq가 있으면 선점 가능. */
}

/*
 * Get next queue for service.
 */

/*
 * bfq_get_next_queue - 다음에 서비스할 bfq_queue를 선택.
 *
 * 목적: 루트부터 leaf까지 next_in_service를 따라 낮추며, 경로의 모든 엔티티를
 *       in_service로 설정하고 최종 leaf bfqq를 반환.
 * 호출 경로: blk_mq_dispatch_rq_list -> __bfq_dispatch_request ->
 *           bfq_get_next_queue -> (이후) nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * NVMe 연결: 반환된 bfqq의 next_rq가 NVMe SQ에 들어갈 명령 후보. 다중 actuator
 *            NVMe SSD에서는 actuator_idx에 따라 active_list를 따로 관리해 병렬
 *            doorbell을 최적화.
 */
struct bfq_queue *bfq_get_next_queue(struct bfq_data *bfqd)
{
	struct bfq_entity *entity = NULL; /* NVMe: 다음 doorbell로 선택될 엔티티. */
	struct bfq_sched_data *sd; /* 현재 탐색 중인 계층. */
	struct bfq_queue *bfqq; /* 최종 leaf: nvme_queue_rq()가 사용할 큐. */

	if (bfq_tot_busy_queues(bfqd) == 0) /* NVMe: 활성 CID 흐름이 없으면 디스패치 불필요. */
		return NULL; /* blk_mq_dispatch_rq_list에 낼 bfqq 없음. */

	/*
	 * Traverse the path from the root to the leaf entity to
	 * serve. Set in service all the entities visited along the
	 * way.
	 */
	sd = &bfqd->root_group->sched_data; /* 루트 그룹부터 leaf까지 경로 탐색 시작. */
	for (; sd ; sd = entity->my_sched_data) { /* NVMe: 루트 -> leaf 낮추며 doorbell 대상 경로 선택. */
		/*
		 * WARNING. We are about to set the in-service entity
		 * to sd->next_in_service, i.e., to the (cached) value
		 * returned by bfq_lookup_next_entity(sd) the last
		 * time it was invoked, i.e., the last time when the
		 * service order in sd changed as a consequence of the
		 * activation or deactivation of an entity. In this
		 * respect, if we execute bfq_lookup_next_entity(sd)
		 * in this very moment, it may, although with low
		 * probability, yield a different entity than that
		 * pointed to by sd->next_in_service. This rare event
		 * happens in case there was no CLASS_IDLE entity to
		 * serve for sd when bfq_lookup_next_entity(sd) was
		 * invoked for the last time, while there is now one
		 * such entity.
		 *
		 * If the above event happens, then the scheduling of
		 * such entity in CLASS_IDLE is postponed until the
		 * service of the sd->next_in_service entity
		 * finishes. In fact, when the latter is expired,
		 * bfq_lookup_next_entity(sd) gets called again,
		 * exactly to update sd->next_in_service.
		 */

		/* Make next_in_service entity become in_service_entity */
		entity = sd->next_in_service; /* 캐시된 다음 후보를 in-service로 승격. */
		/* NVMe: 이 엔티티가 곧 nvme_submit_cmd(doorbell) 대상이 됨(추정). */
		sd->in_service_entity = entity; /* NVMe: 이 경로가 현재 NVMe 서비스 중임을 표시. */

		/*
		 * If entity is no longer a candidate for next
		 * service, then it must be extracted from its active
		 * tree, so as to make sure that it won't be
		 * considered when computing next_in_service. See the
		 * comments on the function
		 * bfq_no_longer_next_in_service() for details.
		 */
		if (bfq_no_longer_next_in_service(entity)) /* leaf거나 active 자식이 하나면 active tree에서 제거. */
			bfq_active_extract(bfq_entity_service_tree(entity), /* NVMe: 선택된 bfqq를 SQ 후보 집합에서 제거. */
					   entity); /* active tree에서 제거 완료. */

		/*
		 * Even if entity is not to be extracted according to
		 * the above check, a descendant entity may get
		 * extracted in one of the next iterations of this
		 * loop. Such an event could cause a change in
		 * next_in_service for the level of the descendant
		 * entity, and thus possibly back to this level.
		 *
		 * However, we cannot perform the resulting needed
		 * update of next_in_service for this level before the
		 * end of the whole loop, because, to know which is
		 * the correct next-to-serve candidate entity for each
		 * level, we need first to find the leaf entity to set
		 * in service. In fact, only after we know which is
		 * the next-to-serve leaf entity, we can discover
		 * whether the parent entity of the leaf entity
		 * becomes the next-to-serve, and so on.
		 */
	}

	bfqq = bfq_entity_to_bfqq(entity); /* NVMe: leaf bfqq로 변환 -> nvme_queue_rq() 대상. */

	/*
	 * We can finally update all next-to-serve entities along the
	 * path from the leaf entity just set in service to the root.
	 */
	/* NVMe: leaf에서 루트까지 다음 doorbell 후보를 재계산. */
	for_each_entity(entity) { /* NVMe: leaf에서 루트까지 다음 doorbell 후보를 미리 계산. */
		struct bfq_sched_data *sd = entity->sched_data; /* 현재 계층 sched_data. */

		if (!bfq_update_next_in_service(sd, NULL, false)) /* next_in_service 갱신. */
			break; /* 상위로 전파할 변경 없으면 중단. */
	}

	return bfqq; /* NVMe: 다음에 doorbell을 칠 bfqq 반환 (blk_mq_dispatch_rq_list -> nvme_queue_rq). */
}

/* returns true if the in-service queue gets freed */

/*
 * __bfq_bfqd_reset_in_service - in-service 상태를 완전히 해제.
 *
 * NVMe 연결: bfqd->in_service_queue를 NULL로 만들고 타이머를 취소. NVMe CQ에서
 *            모든 CID가 완료되어 더 이상 현재 bfqq를 서비스할 필요가 없을 때
 *            호출. ref가 1이면 bfqq를 해제.
 */
bool __bfq_bfqd_reset_in_service(struct bfq_data *bfqd)
{
	struct bfq_queue *in_serv_bfqq = bfqd->in_service_queue; /* NVMe: 현재 서비스 중인 bfqq. */
	struct bfq_entity *in_serv_entity = &in_serv_bfqq->entity; /* in-service 엔티티. */
	struct bfq_entity *entity = in_serv_entity; /* 루트까지 리셋할 경로 시작점. */

	bfq_clear_bfqq_wait_request(in_serv_bfqq); /* NVMe: 추가 bio 대기 상태 클리어. */
	hrtimer_try_to_cancel(&bfqd->idle_slice_timer); /* NVMe: idle timeout 타이머 취소 -> abort/requeue 타이밍 정리. */
	bfqd->in_service_queue = NULL; /* NVMe: 현재 NVMe 서비스 중 bfqq 없음. */

	/*
	 * When this function is called, all in-service entities have
	 * been properly deactivated or requeued, so we can safely
	 * execute the final step: reset in_service_entity along the
	 * path from entity to the root.
	 */
	/* NVMe: 루트까지 현재 NVMe 서비스 중 엔티티 표시를 해제. */
	for_each_entity(entity) /* NVMe: 루트까지 모든 계층의 in_service_entity 해제. */
		entity->sched_data->in_service_entity = NULL; /* 현재 NVMe 서비스 중 표시 해제. */

	/*
	 * in_serv_entity is no longer in service, so, if it is in no
	 * service tree either, then release the service reference to
	 * the queue it represents (taken with bfq_get_entity).
	 */
	if (!in_serv_entity->on_st_or_in_serv) { /* NVMe: 후보 집합에도 없으면 service ref 해제. */
		/*
		 * If no process is referencing in_serv_bfqq any
		 * longer, then the service reference may be the only
		 * reference to the queue. If this is the case, then
		 * bfqq gets freed here.
		 */
		int ref = in_serv_bfqq->ref; /* 해제 전 ref 값 저장. */
		bfq_put_queue(in_serv_bfqq); /* NVMe: bfqq 메모리 반환 시도. */
		if (ref == 1) /* service ref가 유일했다면 bfqq가 해제됨. */
			return true; /* NVMe: in-service queue가 메모리에서 해제됨. */
	}

	return false;
}

void bfq_deactivate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			 bool ins_into_idle_tree, bool expiration)
{
	struct bfq_entity *entity = &bfqq->entity; /* bfqq의 스케줄링 엔티티. */

	bfq_deactivate_entity(entity, ins_into_idle_tree, expiration); /* NVMe: bfqq를 SQ 후보 집합에서 제거. */
}


/*
 * bfq_activate_bfqq - 비활성 bfqq를 active로 만들고 스케줄러에 참여.
 *
 * 호출 경로: bio 도착 -> bfq_insert_request -> bfq_add_bfqq_busy ->
 *           bfq_activate_bfqq -> bfq_activate_requeue_entity
 * NVMe 연결: 이제 이 bfqq가 NVMe doorbell 후보 집합에 들어간다.
 */
void bfq_activate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;

	bfq_activate_requeue_entity(entity, bfq_bfqq_non_blocking_wait_rq(bfqq), /* non-blocking wait 여부. */
				    false, false); /* NVMe: 새 CID(bio)로 bfqq를 active SQ 후보로 진입. */
	bfq_clear_bfqq_non_blocking_wait_rq(bfqq); /* non-blocking wait 플래그 클리어. */
}

void bfq_requeue_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		      bool expiration)
{
	struct bfq_entity *entity = &bfqq->entity;

	bfq_activate_requeue_entity(entity, false, /* requeue 시에는 non-blocking wait 없음. */
				    bfqq == bfqd->in_service_queue, expiration); /* NVMe: 만료 후 다음 doorbell 경쟁에 재참여. */
}

void bfq_add_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq)
{
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_entity *entity = &bfqq->entity;

	if (!entity->in_groups_with_pending_reqs) { /* pending req 그룹 집계에 미등록이면. */
		entity->in_groups_with_pending_reqs = true; /* NVMe: CID 후보를 가진 cgroup으로 집계. */
		if (!(bfqq_group(bfqq)->num_queues_with_pending_reqs++)) /* 그룹 내 첫 pending 큐면. */
			bfqq->bfqd->num_groups_with_pending_reqs++; /* NVMe: pending CID를 가진 cgroup 그룹 수 증가. */
	}
#endif
}

void bfq_del_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq)
{
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_entity *entity = &bfqq->entity;

	if (entity->in_groups_with_pending_reqs) { /* 등록되어 있으면 제거. */
		entity->in_groups_with_pending_reqs = false; /* NVMe: 더 이상 pending CID 없는 그룹으로 집계 제거. */
		if (!(--bfqq_group(bfqq)->num_queues_with_pending_reqs)) /* 그룹 내 pending 큐가 0이면. */
			bfqq->bfqd->num_groups_with_pending_reqs--; /* NVMe: pending CID cgroup 그룹 수 감소. */
	}
#endif
}

/*
 * Called when the bfqq no longer has requests pending, remove it from
 * the service tree. As a special case, it can be invoked during an
 * expiration.
 */

/*
 * bfq_del_bfqq_busy - bfqq를 busy 집합에서 제거.
 *
 * NVMe 연결: queued가 0이 되면(더 이상 NVMe SQ에 넣을 CID 후보가 없으면)
 *            active tree와 busy_queues 카운터에서 제거. dispatched가 0이면
 *            weight 카운터 트리에서도 제거.
 */
void bfq_del_bfqq_busy(struct bfq_queue *bfqq, bool expiration)
{
	struct bfq_data *bfqd = bfqq->bfqd; /* 장치 단위 상태. */

	bfq_log_bfqq(bfqd, bfqq, "del from busy");

	bfq_clear_bfqq_busy(bfqq); /* busy 플래그 클리어. */

	bfqd->busy_queues[bfqq->ioprio_class - 1]--; /* NVMe: 클래스별 활성 CID 흐름 수 감소. */

	if (bfqq->wr_coeff > 1) /* weight-raised 큐면. */
		bfqd->wr_busy_queues--; /* NVMe: latency boost 중인 큐 수 감소. */

	bfqg_stats_update_dequeue(bfqq_group(bfqq)); /* cgroup dequeue 통계 갱신. */

	bfq_deactivate_bfqq(bfqd, bfqq, true, expiration); /* NVMe: active -> idle 또는 완전 제거. */

	if (!bfqq->dispatched) { /* NVMe: in-flight CID가 0이면(하드웨어 완료됨) 집계에서 제거. */
		bfq_del_bfqq_in_groups_with_pending_reqs(bfqq);
		/*
		 * Next function is invoked last, because it causes bfqq to be
		 * freed. DO NOT use bfqq after the next function invocation.
		 */
		bfq_weights_tree_remove(bfqq); /* weight counter tree에서 제거. */
	}
}

/*
 * Called when an inactive queue receives a new request.
 */

/*
 * bfq_add_bfqq_busy - 새 request를 받은 비활성 bfqq를 busy 집합에 추가.
 *
 * NVMe 연결: bio가 bfqq에 들어오면(=NVMe CID 후보가 생기면) busy_queues에
 *            추가하고 weight 카운터 트리에 등록. wr_coeff>1이면 wr_busy_queues
 *            증가로 latency 우선 큐임을 표시.
 */
void bfq_add_bfqq_busy(struct bfq_queue *bfqq)
{
	struct bfq_data *bfqd = bfqq->bfqd; /* NVMe controller/namespace 단위 상태. */

	bfq_log_bfqq(bfqd, bfqq, "add to busy");

	bfq_activate_bfqq(bfqd, bfqq); /* NVMe: 새 CID(bio)로 active SQ 후보 집합에 진입. */

	bfq_mark_bfqq_busy(bfqq); /* busy 플래그 설정. */
	bfqd->busy_queues[bfqq->ioprio_class - 1]++; /* NVMe: 클래스별 활성 CID 흐름 수 증가. */

	if (!bfqq->dispatched) { /* NVMe: 현재 in-flight CID가 없으면(새 흐름) 그룹/weight 집계에 추가. */
		bfq_add_bfqq_in_groups_with_pending_reqs(bfqq); /* pending cgroup 집계 추가. */
		if (bfqq->wr_coeff == 1) /* weight-raised 아닐 때만 weight counter 추가. */
			bfq_weights_tree_add(bfqq); /* NVMe: asymmetric weight 집계에 등록. */
	}

	if (bfqq->wr_coeff > 1) /* NVMe: latency-sensitive 흐름이면 wr_busy_queues 증가. */
		bfqd->wr_busy_queues++; /* latency boost 큐 수 증가. */

	/* Move bfqq to the head of the woken list of its waker */
	if (!hlist_unhashed(&bfqq->woken_list_node) && /* waker 관계가 설정되어 있으면. */
	    &bfqq->woken_list_node != bfqq->waker_bfqq->woken_list.first) { /* 이미 head가 아니면. */
		hlist_del_init(&bfqq->woken_list_node); /* 기존 위치에서 제거. */
		hlist_add_head(&bfqq->woken_list_node, /* waker의 woken list head로 이동. */
			       &bfqq->waker_bfqq->woken_list); /* NVMe: sync 워크로드 의존성으로 doorbell batching 최적화(추정). */
	}
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 경로에서, doorbell 직전 호스트 측 최종
 *   디스패치 순서를 B-WF2Q+로 결정한다.
 *
 * - bfq_entity/bfq_queue는 NVMe SQ/CQ로 연결될 CID(command identifier) 흐름의
 *   우선순위 단위이며, start/finish/weight/budget이 doorbell 순서를 좌우한다.
 *
 * - active tree와 next_in_service 캐싱을 통해 O(log N)으로 다음 bfqq를 찾아
 *   NVMe SSD의 낮은 큐 깊이/높은 병렬도 환경에서도 지연을 최소화한다.
 *
 * - 완료 경로(nvme_irq -> nvme_complete_rq -> blk_mq_complete_request)에서
 *   bfq_bfqq_served()가 vtime을 전진시켜, 다음 doorbell의 공정성을 유지한다.
 *
 * - 다중 actuator NVMe SSD를 고려해 active_list[actuator_idx]와 rq_in_driver[]를
 *   분리 관리하며, actuator 단위 병렬 인젝션을 지원한다 (필드 연결은 추정).
 */
