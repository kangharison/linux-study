// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hierarchical Budget Worst-case Fair Weighted Fair Queueing
 * (B-WF2Q+): hierarchical scheduling algorithm by which the BFQ I/O
 * scheduler schedules generic entities. The latter can represent
 * either single bfq queues (associated with processes) or groups of
 * bfq queues (associated with cgroups).
 */

/*
 * [한국어 설명] B-WF2Q+ 계층적 스케줄링 알고리즘 구현 - BFQ의 수학적 핵심 (bfq-wf2q.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 BFQ(Budget Fair Queueing) I/O 스케줄러의 수학적 핵심인
 * B-WF2Q+(Hierarchical Budget Worst-case Fair Weighted Fair Queueing+)
 * 알고리즘을 구현한다. B-WF2Q+는 각 스케줄링 단위(entity)에 가상 시작
 * 시각(start, S_i)과 가상 종료 시각(finish, F_i = S_i + budget/weight)을
 * 부여하고, 이 타임스탬프를 기준으로 active tree(rb-tree)에서 "지금 서비스
 * 가능하며(eligible: S_i <= vtime) F_i가 가장 작은" entity를 골라 다음
 * 서비스 대상으로 삼는 알고리즘이다. bfq_entity는 이중 역할을 하는
 * 추상화로, leaf(my_sched_data == NULL)이면 프로세스 하나의 I/O 흐름인
 * bfq_queue를, non-leaf이면 cgroup 하나를 나타내는 bfq_group을 감싸며,
 * 이 파일의 모든 함수는 leaf/non-leaf를 구분하지 않고 동일한 알고리즘으로
 * 처리하므로 계층적(H-WF2Q+) 공정성을 자연스럽게 얻는다. "budget"(예산,
 * 섹터 수)을 서비스량의 단위로 쓰는 WF2Q+의 변형이 곧 B-WF2Q+이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층의 elevator(I/O 스케줄러) 중 하나인 BFQ 내부에서, 이 파일은
 * "누구를 다음에 서비스할지 결정하는 트리 연산 엔진" 계층에 해당한다.
 * 상위 호출 체인: 파일시스템 -> submit_bio() -> blk_mq_submit_bio() ->
 * bfq_insert_request()가 bfq_add_bfqq_busy()를 통해 이 파일의
 * bfq_activate_bfqq()/bfq_activate_requeue_entity()를 호출해 entity를
 * active tree에 편입시키고, blk_mq_run_hw_queue() -> bfq_dispatch_request()가
 * 이 파일의 bfq_get_next_queue()를 호출해 다음에 디스패치할 bfq_queue를
 * 얻는다. 완료 경로에서는 blk_mq_complete_request() -> bfq_completed_request()가
 * 이 파일의 bfq_bfqq_served()를 호출해 실제로 소비된 서비스량만큼 가상
 * 시간(vtime)을 전진시킨다. 실행 컨텍스트는 대부분 bfqd->lock(스핀락)을
 * 쥔 프로세스 컨텍스트(시스템 콜 경로에서의 insert/dispatch)이며, 이
 * 파일 자체는 별도의 인터럽트/소프트IRQ 진입점을 갖지 않고 항상
 * bfq-iosched.c 쪽의 콜백에서 호출되는 형태로만 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/bfq-iosched.h가 정의하는 bfq_entity/bfq_queue/bfq_group/
 * bfq_sched_data/bfq_service_tree 자료구조와 for_each_entity()/
 * for_each_entity_safe() 매크로에 전적으로 의존하며, 자체적으로는 새
 * 자료구조를 정의하지 않는다. block/bfq-iosched.c는 이 파일이 제공하는
 * bfq_activate_bfqq(), bfq_deactivate_bfqq(), bfq_add_bfqq_busy(),
 * bfq_del_bfqq_busy(), bfq_get_next_queue(), bfq_bfqq_served(),
 * bfq_bfqq_charge_time() 등을 호출해 실제 request 삽입/디스패치/완료
 * 이벤트를 B-WF2Q+ 트리 갱신으로 연결한다. block/bfq-cgroup.c는 bfq_group의
 * entity가 이 파일의 활성화/비활성화 경로를 그대로 타도록 만들어 cgroup
 * 계층을 형성한다. 데이터 흐름: bio -> struct request -> bfq_queue(leaf
 * entity) -> (활성화) active tree 삽입 -> bfq_get_next_queue()가 트리에서
 * 선택 -> 디스패치 -> 완료 시 bfq_bfqq_served()가 vtime 전진 -> 트리
 * 재정렬, 의 순환이 반복된다. 공유하는 핵심 자료구조는 struct bfq_entity
 * (스케줄링 단위, start/finish/weight/budget), struct bfq_service_tree
 * (ioprio_class별 active/idle rb-tree와 vtime), struct bfq_sched_data
 * (in_service_entity/next_in_service 캐시를 가진 스케줄러 노드)이다.
 *
 * === 주요 함수/구조체 요약 ===
 * bfq_gt()                        - wraparound를 고려한 u64 타임스탬프
 *                                   비교. 모든 타임스탬프 비교의 기반.
 * bfq_calc_finish()               - F_i = S_i + budget/weight 계산,
 *                                   B-WF2Q+ 타임스탬프 갱신의 핵심.
 * bfq_active_insert()/extract()   - active tree(augmented rb-tree) 삽입/
 *                                   삭제와 min_start 캐시 갱신.
 * __bfq_activate_entity()/
 * __bfq_requeue_entity()          - entity를 활성화하거나 재배치(requeue)
 *                                   하여 새 타임스탬프로 트리에 재삽입.
 * bfq_lookup_next_entity()/
 * bfq_get_next_queue()            - RT>BE>IDLE 우선순위와 vtime 기준
 *                                   eligibility를 따져 다음 서비스 대상
 *                                   leaf bfq_queue를 계층적으로 선택.
 * bfq_bfqq_served()                - 완료된 서비스량만큼 vtime을 전진시켜
 *                                   다음 라운드의 공정성을 갱신.
 * struct bfq_entity (정의는 bfq-iosched.h) - 이 파일이 조작하는 대상.
 *   start/finish/weight/budget/min_start/tree 필드가 모두 이 파일의
 *   함수들에 의해 직접 갱신된다.
 */
#include "bfq-iosched.h"
/* [한국어] BFQ 세 소스 파일(bfq-iosched.c/bfq-wf2q.c/bfq-cgroup.c)이 공유하는
 * 핵심 자료구조(bfq_entity, bfq_queue, bfq_group, bfq_sched_data,
 * bfq_service_tree 등)와 매크로(for_each_entity 등), 다른 소스 파일에 정의된
 * 함수 프로토타입(bfq_put_queue, bfq_weights_tree_add/remove, bfq_log_bfqq 등)을
 * 가져온다. 이 파일은 새 자료구조를 정의하지 않고 헤더가 정의한 구조체 위에서
 * B-WF2Q+ 알고리즘만 구현하므로, 사실상 이 include가 유일한 의존성 진입점이다. */

/**
 * bfq_gt - compare two timestamps.
 * @a: first ts.
 * @b: second ts.
 *
 * Return @a > @b, dealing with wrapping correctly.
 */
/*
 * [한국어]
 * bfq_gt - 두 개의 가상 타임스탬프(u64)를 래핑(wraparound)까지 고려해
 *   "a가 b보다 크다"를 판정한다.
 *
 * @a: 비교할 첫 번째 타임스탬프(주로 entity->start/finish 또는 st->vtime).
 * @b: 비교할 두 번째 타임스탬프.
 * @return: a가 b보다 "이후" 시점이면 true, 아니면 false.
 *
 * B-WF2Q+의 모든 타임스탬프는 u64 값이 서비스량 누적에 따라 계속
 * 증가하다가 이론상 wraparound(오버플로 후 0으로 되돌아옴)할 수 있다.
 * 단순히 a > b로 비교하면 wraparound 직후 아주 작은 값이 실제로는
 * "미래"인데도 "과거"로 오판될 수 있다. 이를 막기 위해 두 값을 뺀 결과를
 * s64(부호 있는 64비트)로 재해석해 부호를 보는 트릭을 쓴다: a - b가
 * s64로 양수이면 a가 b보다 "이후"라고 판정하며, 이는 두 값의 실제 차이가
 * s64 표현 범위(약 2^63) 안에 있는 한 wraparound와 무관하게 항상 올바르다.
 * B-WF2Q+ 알고리즘 전체에서 eligibility 판정(start <= vtime)과 트리 정렬
 * (finish 비교)에 반복적으로 쓰이는 가장 기초적인 헬퍼다. 별도 락 없이도
 * 안전한 순수 함수(pure function)이며, 호출자가 bfqd->lock을 쥔 상태에서
 * 호출하는 것이 일반적이지만 그 자체로는 어떤 공유 상태도 읽거나 쓰지 않는다.
 *
 * 호출 체인:
 *   bfq_update_next_in_service/__bfq_activate_entity/bfq_insert 등
 *   B-WF2Q+ 전반 -> [bfq_gt] -> (하위 호출 없음, 순수 산술 비교)
 */
static int bfq_gt(u64 a, u64 b)
{
	return (s64)(a - b) > 0; /* a-b를 부호있는 64비트로 재해석: 양수면 a가 b보다 "이후" 시점(wraparound에 안전) */
}

static struct bfq_entity *bfq_root_active_entity(struct rb_root *tree)
{
	/*
	 * [한국어]
	 * bfq_root_active_entity - active tree의 루트 노드에 대응하는
	 *   bfq_entity를 반환한다(rb-tree 루트를 entity로 캐스팅하는 헬퍼).
	 *
	 * @tree: 조회할 active tree(struct rb_root, 보통 &st->active).
	 * @return: 트리 루트에 위치한 bfq_entity 포인터. 이 함수는 tree가
	 *   비어있지 않다는 것을 호출자가 이미 보장했다고 가정하며(빈 트리에서
	 *   호출하면 rb_entry가 NULL 포인터에 대해 container_of 연산을 수행해
	 *   미정의 동작이 됨), 실제로 유일한 호출자인 bfq_calc_vtime_jump()가
	 *   호출 전에 RB_EMPTY_ROOT() 검사를 이미 마친 뒤에만 호출한다.
	 *
	 * rb-tree 자체는 "루트가 최소 finish를 가진 노드"라는 보장을 하지
	 * 않지만(rb-tree는 삽입 키인 finish로 균형을 잡을 뿐), 이 함수가
	 * 실제로 쓰이는 목적은 min_start 캐시를 읽기 위함이다: 트리의 루트는
	 * 전체 서브트리를 대표하므로, 루트의 min_start 필드가 곧 트리
	 * 전체에서 가장 이른 start를 가진 entity의 값과 같다(augmented
	 * rb-tree 불변식). bfqd->lock 하에서 호출된다.
	 *
	 * 호출 체인:
	 *   bfq_calc_vtime_jump() -> [bfq_root_active_entity] -> (하위 호출 없음)
	 */
	struct rb_node *node = tree->rb_node; /* rb_root의 최상위 rb_node(루트) 포인터를 꺼냄 */

	return rb_entry(node, struct bfq_entity, rb_node); /* container_of 매크로로 rb_node를 감싸는 bfq_entity를 역산해 반환 */
}

static unsigned int bfq_class_idx(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_class_idx - entity가 어느 ioprio_class(RT/BE/IDLE)에 속하는지
	 *   판단해 sched_data->service_tree[] 배열의 인덱스로 변환한다.
	 *
	 * @entity: 인덱스를 구할 대상 entity(leaf bfq_queue 또는 non-leaf
	 *   bfq_group을 감싼 entity 모두 가능).
	 * @return: 0(RT) / 1(BE) / 2(IDLE) 중 하나의 배열 인덱스.
	 *
	 * entity가 leaf(bfq_entity_to_bfqq()가 NULL이 아닌 값을 반환)라면
	 * 그 bfq_queue의 ioprio_class(IOPRIO_CLASS_RT=1/BE=2/IDLE=3)에서 1을
	 * 빼 0 기반 배열 인덱스로 만든다. entity가 non-leaf(cgroup을 감싼
	 * bfq_group의 entity)라면 개별 request의 ioprio_class 개념이 없으므로
	 * BFQ_DEFAULT_GRP_CLASS(기본값 IOPRIO_CLASS_BE)를 사용해 그룹 전체를
	 * Best-Effort 클래스 트리에 배치한다. 이 인덱스는 이후
	 * bfq_entity_service_tree()가 sched_data->service_tree[idx]를 골라
	 * entity가 어느 rb-tree 쌍(active/idle)에서 스케줄링될지 결정하는
	 * 데 직접 쓰인다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_entity_service_tree()/bfq_update_next_in_service() 등
	 *   -> [bfq_class_idx] -> bfq_entity_to_bfqq()
	 */
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf면 대응 bfq_queue를, non-leaf(그룹)면 NULL을 얻음 */

	return bfqq ? bfqq->ioprio_class - 1 : /* leaf: ioprio_class(1~3)를 0-based 인덱스(0~2)로 변환 */
		BFQ_DEFAULT_GRP_CLASS - 1; /* non-leaf(그룹 entity): 기본 그룹 클래스(BE)의 인덱스를 사용 */
}

unsigned int bfq_tot_busy_queues(struct bfq_data *bfqd)
{
	/*
	 * [한국어]
	 * bfq_tot_busy_queues - 세 ioprio_class(RT/BE/IDLE)를 합친 전체
	 *   busy(=대기 request를 가진) bfq_queue의 총 개수를 반환한다.
	 *
	 * @bfqd: 디바이스 전역 스케줄러 상태.
	 * @return: bfqd->busy_queues[0]+[1]+[2]의 합.
	 *
	 * bfqd->busy_queues[]는 bfq_add_bfqq_busy()/bfq_del_bfqq_busy()가
	 * 큐의 ioprio_class별로 증감시키는 카운터 배열이다. 이 함수는 그
	 * 세 값을 단순 합산해 "지금 스케줄러에 서비스를 기다리는 큐가
	 * 하나라도 있는가"를 판단하는 데 쓰이며, 0이면 bfq_get_next_queue()가
	 * 곧바로 NULL을 반환하도록 하는 빠른 종료 조건이 된다. 순수 읽기
	 * 연산이라 자체적으로 락을 잡지 않지만, 호출자는 통상 bfqd->lock을
	 * 쥔 상태에서 호출한다.
	 *
	 * 호출 체인:
	 *   bfq_get_next_queue()/elevator의 has_work 판단 로직 등
	 *   -> [bfq_tot_busy_queues] -> (하위 호출 없음, 배열 합산)
	 */
	return bfqd->busy_queues[0] + bfqd->busy_queues[1] + /* RT(0)와 BE(1) 클래스의 busy 큐 수를 더함 */
		bfqd->busy_queues[2]; /* IDLE(2) 클래스의 busy 큐 수까지 더해 전체 합을 완성 */
}

static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 bool expiration);
/* [한국어] bfq_lookup_next_entity()의 전방 선언 - 이 함수는 아래
 * bfq_update_next_in_service()에서 먼저 호출되지만 실제 정의는 파일
 * 뒤쪽(라인 순서상 나중)에 있으므로, 컴파일러가 시그니처를 미리 알 수
 * 있도록 원형만 앞서 선언해 둔다. */

static bool bfq_update_parent_budget(struct bfq_entity *next_in_service);
/* [한국어] bfq_update_parent_budget()의 전방 선언 - CONFIG_BFQ_GROUP_IOSCHED
 * 활성/비활성 여부에 따라 실제 정의가 아래 #ifdef 블록에서 갈라지는데,
 * 정의보다 먼저 호출부(bfq_update_next_in_service)가 나오므로 필요하다. */

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
 * [한국어]
 * bfq_update_next_in_service - sd->next_in_service(다음에 서비스할
 *   것으로 캐시된 entity)를 갱신한다.
 *
 * @sd: 갱신 대상 sched_data(계층의 한 노드: 루트 그룹이거나 중간 cgroup).
 * @new_entity: 활성화/재큐잉/재배치를 막 겪은 entity. 이 값이 NULL이
 *   아니면 "이 entity 하나만 확인해도 될지"를 먼저 값싸게 검사해 O(log N)
 *   전체 탐색을 피할 수 있는지 시도한다.
 * @expiration: true면 in-service entity의 만료(expire) 처리 경로에서
 *   호출된 것으로, __bfq_lookup_next_entity()에 in-service가 없는 것처럼
 *   알려야 한다(만료 중인 entity는 사실상 이미 서비스 중이 아니므로).
 * @return: true이면 sd->next_in_service가 바뀌어 상위(entity->parent)의
 *   next_in_service도 재계산해야 할 수 있음을 뜻한다. false이면 이
 *   레벨에서 전파를 멈춰도 된다.
 *
 * next_in_service는 "in_service_entity가 만료되면 다음에 서비스될 것"을
 * 미리 계산해 캐시해 두는 최적화 필드다(정의는 bfq-iosched.h의
 * struct bfq_sched_data 참고). 이 함수는 두 가지 경로로 next_in_service를
 * 계산한다: (1) new_entity가 주어지고 그것이 기존 next_in_service와 같은
 * ioprio_class이며 eligible(start <= vtime)하고 finish가 기존보다 작으면,
 * 트리 전체를 다시 훑지 않고 new_entity로 즉시 교체(change_without_lookup);
 * (2) 그 조건이 성립하지 않으면 bfq_lookup_next_entity()를 호출해 트리를
 * O(log N)으로 다시 탐색한다. 새 next_in_service가 정해지면
 * bfq_update_parent_budget()을 호출해 그 entity가 속한 (non-root) 그룹
 * entity의 budget을 자식 budget으로 동기화하고, 이 budget 변경이 상위의
 * 스케줄링 순서를 바꿀 수 있는지도 함께 반환받는다. bfqd->lock 하에서
 * 호출되는 계층 스케줄러의 갱신 훅으로, entity의 활성화/비활성화/재큐잉이
 * 일어날 때마다 for_each_entity() 루프 안에서 반복 호출되어 leaf에서
 * 루트까지 전파된다.
 *
 * 호출 체인:
 *   bfq_activate_requeue_entity()/bfq_deactivate_entity()/
 *   bfq_get_next_queue() -> [bfq_update_next_in_service] ->
 *   bfq_class_idx()/bfq_lookup_next_entity()/bfq_update_parent_budget()
 */
static bool bfq_update_next_in_service(struct bfq_sched_data *sd,
				       struct bfq_entity *new_entity,
				       bool expiration)
{
	struct bfq_entity *next_in_service = sd->next_in_service; /* 현재 캐시된 "다음 서비스 후보"를 지역 변수로 복사 */
	bool parent_sched_may_change = false; /* 상위 레벨도 재계산이 필요한지 여부, 기본값은 "불필요" */
	bool change_without_lookup = false; /* 트리 전체 탐색을 생략하고 new_entity로 바로 교체할 수 있는지 여부 */

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
	if (new_entity && new_entity != sd->next_in_service) { /* 방금 바뀐 entity가 있고, 그것이 이미 캐시된 후보와 다를 때만 값싼 검사를 시도 */
		/*
		 * Flag used to decide whether to replace
		 * sd->next_in_service with new_entity. Tentatively
		 * set to true, and left as true if
		 * sd->next_in_service is NULL.
		 */
		change_without_lookup = true; /* 일단 "탐색 없이 교체 가능"으로 가정(기존 후보가 없으면 이 값 그대로 유지됨) */

		/*
		 * If there is already a next_in_service candidate
		 * entity, then compare timestamps to decide whether
		 * to replace sd->service_tree with new_entity.
		 */
		if (next_in_service) { /* 기존에 캐시된 후보가 있으면 실제로 비교해서 우위를 판정해야 함 */
			unsigned int new_entity_class_idx = /* new_entity가 속한 ioprio_class의 배열 인덱스 */
				bfq_class_idx(new_entity); /* RT/BE/IDLE 중 어디인지 계산 */
			struct bfq_service_tree *st = /* 해당 클래스의 service_tree(주로 vtime을 참조하기 위함) */
				sd->service_tree + new_entity_class_idx; /* service_tree 배열에서 그 클래스 슬롯 포인터를 얻음 */

			change_without_lookup = /* 아래 세 조건이 모두 참이어야만 값싼 교체가 정당함 */
				(new_entity_class_idx == /* 조건1: 두 entity가 같은 우선순위 클래스에 속해야 finish 비교가 의미 있음 */
				 bfq_class_idx(next_in_service) /* 기존 next_in_service의 클래스 인덱스와 비교 */
				 &&
				 !bfq_gt(new_entity->start, st->vtime) /* 조건2: new_entity가 eligible(start <= 현재 vtime)해야 함 */
				 &&
				 bfq_gt(next_in_service->finish, /* 조건3: 기존 후보의 finish가 new_entity의 finish보다 커야(즉 new_entity가 더 빨리 끝나야) 교체 */
					new_entity->finish));
		}

		if (change_without_lookup) /* 위 조건들을 통과했거나(또는 애초에 기존 후보가 없었으면) */
			next_in_service = new_entity; /* O(log N) 탐색 없이 new_entity를 새 다음-서비스 후보로 확정 */
	}

	if (!change_without_lookup) /* lookup needed */ /* 값싼 교체가 불가능했던 경우: 트리 전체를 다시 탐색해야 함 */
		next_in_service = bfq_lookup_next_entity(sd, expiration); /* RT->BE->IDLE 순으로 서비스 트리를 훑어 최적 entity를 다시 계산 */

	if (next_in_service) { /* 이 sched_data 레벨에 여전히(또는 새로) 서비스할 후보가 있으면 */
		bool new_budget_triggers_change = /* 그 후보의 budget이 부모 그룹 entity의 budget과 달라 상위 순서에 영향을 줄 수 있는지 */
			bfq_update_parent_budget(next_in_service); /* 부모(non-root) 그룹 entity의 budget을 자식 budget으로 동기화하며 변경 여부를 반환받음 */

		parent_sched_may_change = !sd->next_in_service || /* 이전에 후보가 아예 없었다가 처음 생겼다면 상위도 재확인해야 함 */
			new_budget_triggers_change; /* 또는 budget 변경 자체가 상위 스케줄링 순서를 바꿀 수 있으면 상위 재확인 필요 */
	}

	sd->next_in_service = next_in_service; /* 계산된 결과를 실제로 캐시에 반영: 다음 만료 시 이 값을 즉시 in_service_entity로 승격 가능 */

	return parent_sched_may_change; /* 호출자(for_each_entity 루프)가 이 값이 false면 상위 전파를 멈춰도 됨을 알려줌 */
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
/* [한국어] CONFIG_BFQ_GROUP_IOSCHED(cgroup 기반 계층적 I/O 제어) 빌드
 * 옵션이 켜졌을 때만 컴파일되는 블록. cgroup 계층이 실제로 존재하므로
 * bfq_group의 active_entities 카운트, 부모 budget 전파 등 계층 전용
 * 로직을 여기서 구현한다. 꺼진 빌드에서는 파일 뒤쪽 #else 블록의 1단계
 * 전용 스텁이 대신 쓰인다. */

/*
 * Returns true if this budget changes may let next_in_service->parent
 * become the next_in_service entity for its parent entity.
 */
/*
 * [한국어]
 * bfq_update_parent_budget - next_in_service로 선택된 entity의 budget을
 *   그 entity가 속한 (non-root) bfq_group entity의 budget에 동기화한다.
 *
 * @next_in_service: 방금 새로 선택된 다음 서비스 후보 entity(leaf
 *   bfq_queue이거나 non-leaf bfq_group일 수 있음).
 * @return: 그룹 entity의 budget이 실제로 커졌으면(즉 상위 스케줄링 순서에
 *   영향을 줄 수 있으면) true, 변경이 없거나 root 그룹이면 false.
 *
 * B-WF2Q+ 계층 구조에서 non-leaf entity(그룹)의 budget은 "그 그룹 안에서
 * 다음에 서비스될 자식의 budget"을 그대로 물려받는 방식으로 정해진다
 * (그룹 자신은 실제 request를 갖지 않으므로 자체 budget 개념이 없고,
 * 대신 자식 중 가장 유력한 후보의 budget을 대표값으로 사용). 이 함수는
 * next_in_service->sched_data로부터 container_of()를 이용해 그 sched_data를
 * 소유한 bfq_group을 역산하고, 그 그룹의 my_entity(그룹 자신을 상위에서
 * 표현하는 entity, 루트 그룹이면 NULL)에 next_in_service->budget을 대입한다.
 * 대입 전 값보다 커졌다면 상위(그 그룹의 부모) 스케줄링에서 이 그룹의
 * finish 시각 계산이 달라질 수 있으므로 true를 반환해 호출자
 * (bfq_update_next_in_service)가 상위로 전파하도록 신호를 보낸다. 루트
 * 그룹은 my_entity가 NULL이라 이 갱신 자체가 적용되지 않는다(루트는 다시
 * 상위로 전파할 부모가 없기 때문). bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_next_in_service() -> [bfq_update_parent_budget] ->
 *   (container_of만 사용, 하위 함수 호출 없음)
 */
static bool bfq_update_parent_budget(struct bfq_entity *next_in_service)
{
	struct bfq_entity *bfqg_entity; /* next_in_service가 속한 그룹을 상위에서 표현하는 entity(없으면 NULL) */
	struct bfq_group *bfqg; /* next_in_service->sched_data를 소유한 bfq_group */
	struct bfq_sched_data *group_sd; /* next_in_service가 스케줄링되는 sched_data(=그 그룹의 sched_data) */
	bool ret = false; /* 기본값: budget 변경으로 인한 상위 재계산 불필요 */

	group_sd = next_in_service->sched_data; /* next_in_service를 담고 있는 계층 노드의 sched_data를 얻음 */

	bfqg = container_of(group_sd, struct bfq_group, sched_data); /* sched_data 필드 오프셋을 역산해 이를 소유한 bfq_group 객체를 복원 */
	/*
	 * bfq_group's my_entity field is not NULL only if the group
	 * is not the root group. We must not touch the root entity
	 * as it must never become an in-service entity.
	 */
	bfqg_entity = bfqg->my_entity; /* 루트 그룹이면 NULL, 그 외에는 상위에서 이 그룹을 나타내는 entity */
	if (bfqg_entity) { /* 루트가 아닌 그룹일 때만 budget 전파(루트 entity는 서비스 대상이 될 수 없어 갱신 불필요) */
		if (bfqg_entity->budget > next_in_service->budget) /* 갱신 전 그룹 budget이 새 값보다 이미 컸다면 */
			ret = true; /* budget이 실질적으로 줄어드는 변경이므로 상위 finish 계산에 영향 -> 재전파 필요 신호 */
		bfqg_entity->budget = next_in_service->budget; /* 그룹 entity의 budget을 자식(next_in_service)의 budget으로 동기화 */
	}

	return ret; /* 상위 sched_data의 next_in_service 재계산이 필요한지를 호출자에게 알림 */
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
 * [한국어]
 * bfq_no_longer_next_in_service - entity가 방금 in-service로 선택되면서
 *   더 이상 "다음 서비스 후보(next_in_service)" 자격을 유지할 수 없는지
 *   판정한다.
 *
 * @entity: 방금 in-service로 승격되기 직전인 entity(leaf 또는 non-leaf).
 * @return: true이면 entity를 active tree에서 실제로 추출(extract)해야
 *   함(더 이상 후보 자격이 없으므로). false이면 active tree에 그대로
 *   남겨둬도 됨(다른 자식이 여전히 유효한 next_in_service 후보일 수
 *   있으므로).
 *
 * next_in_service의 정의(bfq-iosched.h의 struct bfq_sched_data 참고)는
 * 다소 제한적이다: "in_service_entity가 만료되면서 그 하위(자식) 서비스도
 * 함께 끝난다고 가정했을 때 다음에 서비스될 entity"를 가리킨다. entity가
 * leaf(bfq_entity_to_bfqq()가 non-NULL)라면, 이 entity 자체가 지금
 * in-service가 되는 것이므로 정의상 더는 "다음" 후보일 수 없어 true를
 * 반환한다. entity가 non-leaf(그룹)라면 얘기가 다르다: 그 그룹 안에
 * active한 자식이 둘 이상이면, 이번에 하나가 in-service로 뽑혀도 나머지
 * 자식들이 여전히 그 그룹 레벨에서 유효한 next_in_service 후보이므로
 * false를 반환해 그룹 entity를 active tree에 남겨둔다. 반대로 active
 * 자식이 정확히 하나(bfqg->active_entities == 1)뿐이면, 그 하나가 곧
 * in-service가 되어 그룹 전체가 후보에서 빠져야 하므로 true를 반환한다.
 * 이 함수가 호출되는 시점(bfq_get_next_queue() 내부, bfq_active_extract가
 * 아직 실행되기 전)에는 active_entities가 실제 active 자식 수와 정확히
 * 일치함이 함수 상단 원본 주석에 보장되어 있다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_get_next_queue() -> [bfq_no_longer_next_in_service] ->
 *   bfq_entity_to_bfqq()
 */
static bool bfq_no_longer_next_in_service(struct bfq_entity *entity)
{
	struct bfq_group *bfqg; /* entity가 non-leaf일 때 이를 감싸는 bfq_group */

	if (bfq_entity_to_bfqq(entity)) /* entity가 leaf(bfq_queue를 감쌈)라면 */
		return true; /* leaf는 곧바로 in-service가 되므로 무조건 "더 이상 후보 아님" */

	bfqg = container_of(entity, struct bfq_group, entity); /* non-leaf이므로 entity를 포함하는 bfq_group을 역산 */

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
	if (bfqg->active_entities == 1) /* 이 그룹에 active한 자식이 단 하나뿐이면(=지금 뽑히는 그 자식뿐) */
		return true; /* 그 하나가 in-service가 되면 그룹 전체가 후보를 잃으므로 그룹도 추출 대상 */

	return false; /* active 자식이 둘 이상 남아 있으므로 그룹은 여전히 유효한 next_in_service 후보 */
}

static void bfq_inc_active_entities(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_inc_active_entities - entity가 새로 active tree에 들어갈 때,
	 *   그 부모 bfq_group의 active_entities 카운터를 1 증가시킨다.
	 *
	 * @entity: 방금 active tree에 삽입된 entity.
	 * @return: 없음(void).
	 *
	 * active_entities는 bfq_no_longer_next_in_service()가 "그룹에 active
	 * 자식이 몇 개 남았는지"를 O(1)로 판단하기 위해 유지하는 캐시
	 * 카운터다. entity->sched_data는 entity가 스케줄링되는 계층 노드를
	 * 가리키며, 이를 소유한 bfq_group을 container_of로 얻은 뒤, 그
	 * 그룹이 root_group이 아닌 경우에만 카운트를 올린다(루트 그룹은
	 * active_entities를 사용하는 next_in_service 최적화 대상이 아니므로
	 * 카운트할 필요가 없다). bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_active_insert() -> [bfq_inc_active_entities] -> (하위 호출 없음)
	 */
	struct bfq_sched_data *sd = entity->sched_data; /* entity가 스케줄링되는 sched_data(=부모 그룹의 sched_data) */
	struct bfq_group *bfqg = container_of(sd, struct bfq_group, sched_data); /* 그 sched_data를 소유한 bfq_group 역산 */

	if (bfqg != bfqg->bfqd->root_group) /* 루트 그룹이 아닐 때만(루트는 active_entities 최적화 대상이 아님) */
		bfqg->active_entities++; /* 이 그룹에 새로 active 자식이 하나 늘었음을 반영 */
}

static void bfq_dec_active_entities(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_dec_active_entities - entity가 active tree에서 빠질 때, 그
	 *   부모 bfq_group의 active_entities 카운터를 1 감소시킨다
	 *   (bfq_inc_active_entities()의 역연산).
	 *
	 * @entity: 방금 active tree에서 제거된 entity.
	 * @return: 없음(void).
	 *
	 * bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_active_extract() -> [bfq_dec_active_entities] -> (하위 호출 없음)
	 */
	struct bfq_sched_data *sd = entity->sched_data; /* entity의 부모 sched_data */
	struct bfq_group *bfqg = container_of(sd, struct bfq_group, sched_data); /* 이를 소유한 bfq_group 역산 */

	if (bfqg != bfqg->bfqd->root_group) /* 루트 그룹이 아닐 때만 카운트 대상 */
		bfqg->active_entities--; /* active 자식이 하나 줄었음을 반영 */
}

#else /* CONFIG_BFQ_GROUP_IOSCHED */
/* [한국어] cgroup 계층 지원이 꺼진 빌드용 대체 구현. 계층이 루트 한
 * 단계뿐이므로 "그룹 budget 전파"나 "active 자식 수 세기" 같은 계층
 * 전용 개념 자체가 무의미해져, 아래 네 함수는 항상 안전한 고정값만
 * 반환/수행하는 1줄짜리 스텁으로 대체된다. 이렇게 하면 호출부
 * (bfq_update_next_in_service, bfq_active_insert/extract 등)의 코드를
 * #ifdef 없이 그대로 재사용할 수 있다. */

static bool bfq_update_parent_budget(struct bfq_entity *next_in_service)
{
	/*
	 * [한국어]
	 * bfq_update_parent_budget (cgroup 미지원 빌드용 스텁) - 계층이
	 *   없으므로 부모 budget 전파 자체가 존재하지 않아 항상 false.
	 *
	 * @next_in_service: 사용되지 않음(계층이 없어 부모가 없음).
	 * @return: 항상 false(상위 재계산이 필요한 경우가 있을 수 없음).
	 *
	 * 호출 체인:
	 *   bfq_update_next_in_service() -> [bfq_update_parent_budget(stub)]
	 */
	return false; /* 계층 자체가 없으므로 "부모 budget 변경"이라는 개념이 성립하지 않음 */
}

static bool bfq_no_longer_next_in_service(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_no_longer_next_in_service (cgroup 미지원 빌드용 스텁) - 계층이
	 *   1단계(루트)뿐이므로 모든 entity가 사실상 leaf처럼 취급되어
	 *   항상 true.
	 *
	 * @entity: 사용되지 않음.
	 * @return: 항상 true(entity는 항상 즉시 active tree에서 추출되어야 함).
	 *
	 * 호출 체인:
	 *   bfq_get_next_queue() -> [bfq_no_longer_next_in_service(stub)]
	 */
	return true; /* 그룹 계층이 없어 "그룹에 다른 active 자식이 남는" 경우 자체가 없으므로 항상 추출 대상 */
}

static void bfq_inc_active_entities(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_inc_active_entities (cgroup 미지원 빌드용 스텁) - 카운트할
	 *   bfq_group 자체가 없으므로 아무 동작도 하지 않는다.
	 *
	 * @entity: 사용되지 않음.
	 * @return: 없음(void).
	 */
} /* [한국어] 빈 함수 본문: cgroup 계층이 없어 active_entities 카운터 자체가 무의미함 */

static void bfq_dec_active_entities(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_dec_active_entities (cgroup 미지원 빌드용 스텁) - 위와 대칭적인
	 *   이유로 아무 동작도 하지 않는다.
	 *
	 * @entity: 사용되지 않음.
	 * @return: 없음(void).
	 */
} /* [한국어] 빈 함수 본문: 감소시킬 active_entities 카운터가 없음 */

#endif /* CONFIG_BFQ_GROUP_IOSCHED */

/*
 * Shift for timestamp calculations.  This actually limits the maximum
 * service allowed in one timestamp delta (small shift values increase it),
 * the maximum total weight that can be used for the queues in the system
 * (big shift values increase it), and the period of virtual time
 * wraparounds.
 */
/* [한국어] WFQ_SERVICE_SHIFT - 서비스량(섹터 수)을 가상 시간 단위로
 * 변환할 때 쓰는 고정소수점 좌측 시프트 폭. bfq_delta()에서
 * (service << WFQ_SERVICE_SHIFT) / weight 형태로 쓰여, 정수 나눗셈에서
 * 잃는 소수부 정밀도를 시프트만큼의 추가 비트로 보존한다. 값이 작으면
 * 한 번의 타임스탬프 갱신(delta)으로 표현 가능한 최대 서비스량이
 * 커지는 대신 정밀도가 떨어지고, 값이 크면 반대로 시스템 전체가 감당할
 * 수 있는 총 weight 합의 상한이 줄어들고 vtime의 wraparound 주기가
 * 짧아진다. 22라는 값은 이 세 가지(정밀도/최대 weight 합/wraparound
 * 주기) 사이의 실측 기반 절충값이다. */
#define WFQ_SERVICE_SHIFT	22

/*
 * [한국어]
 * bfq_entity_to_bfqq - entity가 leaf(즉 bfq_group이 아니라 실제
 *   bfq_queue를 감싼 것)인지 판별하고, 맞다면 그 bfq_queue를 반환한다.
 *
 * @entity: 판별할 entity.
 * @return: entity가 leaf이면 container_of()로 복원한 bfq_queue 포인터,
 *   non-leaf(그룹을 감싼 entity)이면 NULL.
 *
 * bfq_entity는 leaf/non-leaf 여부를 별도의 enum이나 플래그가 아니라
 * my_sched_data 필드의 NULL 여부로 구분한다: my_sched_data는 "이 entity
 * 자신이 non-leaf일 때 그 하위 자식들을 위해 소유하는 sched_data"를
 * 가리키므로, leaf(bfq_queue)는 하위 자식이 없어 이 필드가 항상 NULL이고
 * non-leaf(bfq_group)는 항상 유효한 포인터를 갖는다. 이 함수는 B-WF2Q+
 * 트리 순회 도중 "이 entity가 실제 request를 가진 leaf인지"를 판별해야
 * 하는 거의 모든 곳(활성화, 비활성화, 삽입, 추출, 서비스 완료 처리 등)에서
 * 반복적으로 호출되는 가장 기초적인 타입 판별/변환 헬퍼다. 락 없이도
 * 안전한 순수 변환이지만, entity 구조 자체가 bfqd->lock으로 보호되므로
 * 실제로는 그 락 하에서 호출된다.
 *
 * 호출 체인:
 *   B-WF2Q+ 전반의 거의 모든 함수 -> [bfq_entity_to_bfqq] ->
 *   (하위 호출 없음, container_of만 수행)
 */
struct bfq_queue *bfq_entity_to_bfqq(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = NULL; /* 기본값: non-leaf(그룹)로 가정하고 NULL로 초기화 */

	if (!entity->my_sched_data) /* my_sched_data가 없다는 것은 이 entity에게 하위 자식이 없다는 뜻 = leaf */
		bfqq = container_of(entity, struct bfq_queue, entity); /* entity 필드의 오프셋을 역산해 이를 포함하는 bfq_queue 복원 */

	return bfqq; /* leaf면 유효한 bfq_queue 포인터, non-leaf(그룹)면 NULL */
}


/**
 * bfq_delta - map service into the virtual time domain.
 * @service: amount of service.
 * @weight: scale factor (weight of an entity or weight sum).
 */
/*
 * [한국어]
 * bfq_delta - 실제 서비스량(섹터 수 등)을 weight로 정규화해 가상 시간
 *   증분값으로 변환한다.
 *
 * @service: 소비된(또는 소비될) 서비스량(섹터 수 단위의 정수).
 * @weight: 정규화에 쓰일 가중치(개별 entity->weight 또는 service_tree
 *   전체의 wsum일 수 있음).
 * @return: (service << WFQ_SERVICE_SHIFT) / weight로 계산된 가상 시간
 *   증분값(u64).
 *
 * B-WF2Q+의 핵심 아이디어는 "실제 시간"이 아니라 "weight로 나눈
 * 서비스량"을 가상 시간(virtual time)으로 취급하는 것이다: 같은 양의
 * 서비스를 받아도 weight가 큰(높은 우선순위) entity는 가상 시간이 덜
 * 흐른 것으로 계산되어, 결과적으로 다음 차례에 더 유리한(작은) finish
 * 시각을 갖게 된다. WFQ_SERVICE_SHIFT만큼 미리 좌측 시프트한 뒤
 * div64_ul()로 나누는 것은, service*2^22를 weight로 정수 나눗셈해도
 * 실질적으로 소수부 정밀도를 22비트만큼 보존하기 위한 고정소수점
 * 기법이다. 이 함수는 bfq_calc_finish()(F_i 계산)와 bfq_bfqq_served()
 * (vtime 전진) 양쪽에서 모두 쓰이는 B-WF2Q+ 수식의 공통 분모다.
 *
 * 호출 체인:
 *   bfq_calc_finish()/bfq_bfqq_served() -> [bfq_delta] ->
 *   div64_ul() (커널 64비트 나눗셈 헬퍼)
 */
static u64 bfq_delta(unsigned long service, unsigned long weight)
{
	return div64_ul((u64)service << WFQ_SERVICE_SHIFT, weight); /* service를 22비트 좌측 시프트(정밀도 보존)한 뒤 weight로 나눠 가상 시간 증분을 얻음 */
}

/**
 * bfq_calc_finish - assign the finish time to an entity.
 * @entity: the entity to act upon.
 * @service: the service to be charged to the entity.
 */
/*
 * [한국어]
 * bfq_calc_finish - entity의 finish(F_i) 타임스탬프를 "F_i = S_i +
 *   delta(service, weight)" 공식으로 계산해 갱신한다.
 *
 * @entity: 갱신 대상 entity(entity->start는 이미 유효한 값으로 설정돼
 *   있어야 함).
 * @service: 이번에 이 entity에게 과금(charge)할 서비스량(주로
 *   entity->budget 또는 entity->service).
 * @return: 없음(void). entity->finish 필드를 직접 갱신한다.
 *
 * WF2Q+ 계열 알고리즘의 정의 공식 F_i = S_i + service/weight를 그대로
 * 구현한다: 시작 시각(S_i)에 "이번 서비스 구간에서 받을 것으로 가정하는
 * 서비스량을 weight로 정규화한 값"을 더해 종료 시각(F_i)을 얻는다.
 * weight가 클수록(높은 우선순위) 같은 service라도 delta가 작아져 F_i가
 * 더 빨리(작게) 정해지고, 결과적으로 active tree에서 더 앞쪽(먼저
 * 서비스될 위치)에 놓이게 된다. 이 함수는 entity가 활성화될 때
 * (entity->budget으로), requeue될 때(entity->service, 즉 실제로 받은
 * 서비스량으로), 비활성화될 때 등 타임스탬프를 다시 계산해야 하는 모든
 * 지점에서 호출된다. leaf entity(bfq_queue)인 경우 bfq_log_bfqq()로
 * 디버그 트레이스(blktrace)에 계산 과정을 남겨, 필드에서 타이밍 이슈를
 * 추적할 수 있게 한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_fin_time_enqueue()/__bfq_requeue_entity()/
 *   __bfq_deactivate_entity() -> [bfq_calc_finish] -> bfq_delta()
 */
static void bfq_calc_finish(struct bfq_entity *entity, unsigned long service)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 디버그 로그에 남길 bfq_queue를 얻음(non-leaf면 NULL) */

	entity->finish = entity->start + /* F_i = S_i + ... : 시작 가상 시각에 서비스 환산량을 더해 종료 시각을 정함 */
		bfq_delta(service, entity->weight); /* service를 entity 자신의 weight로 정규화한 가상 시간 증분 */

	if (bfqq) { /* leaf(실제 프로세스 큐)일 때만 상세 디버그 트레이스 기록 */
		bfq_log_bfqq(bfqq->bfqd, bfqq, /* blktrace 기반 디버그 로그: 이번에 과금된 서비스량과 weight 기록 */
			"calc_finish: serv %lu, w %d",
			service, entity->weight);
		bfq_log_bfqq(bfqq->bfqd, bfqq, /* 계산 전후의 start/finish 및 delta 값을 함께 기록해 사후 분석 가능하게 함 */
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
/*
 * [한국어]
 * bfq_entity_of - rb_node 포인터를 그 노드를 포함하는 bfq_entity로
 *   변환하되, node가 NULL인 경우까지 안전하게 처리한다.
 *
 * @node: 변환할 rb_node(주로 rb_next()/rb_prev()의 반환값처럼 NULL일
 *   수 있는 값).
 * @return: node가 NULL이 아니면 container_of()로 얻은 bfq_entity 포인터,
 *   node가 NULL이면 NULL.
 *
 * rb_entry()/container_of() 자체는 NULL 포인터에 대해 안전하지 않은
 * (오프셋을 뺀 잘못된 주소를 만들어내는) 매크로이므로, 이 함수는 호출
 * 전에 NULL 검사를 대신 수행해 트리 순회 코드가 매번 중복된 NULL 검사를
 * 쓰지 않도록 감싸주는 얇은 래퍼다. 주로 rb_next()/rb_prev()가 트리의
 * 끝에서 NULL을 반환할 수 있는 idle 트리 갱신 경로(bfq_idle_extract)에서
 * 쓰인다.
 *
 * 호출 체인:
 *   bfq_idle_extract() -> [bfq_entity_of] -> (하위 호출 없음)
 */
struct bfq_entity *bfq_entity_of(struct rb_node *node)
{
	struct bfq_entity *entity = NULL; /* 기본값: node가 없으면(NULL) 결과도 NULL */

	if (node) /* 유효한 노드가 주어졌을 때만 변환 시도 */
		entity = rb_entry(node, struct bfq_entity, rb_node); /* rb_node 필드 오프셋을 역산해 이를 포함하는 bfq_entity 복원 */

	return entity; /* 변환된 entity 또는 NULL */
}

/**
 * bfq_extract - remove an entity from a tree.
 * @root: the tree root.
 * @entity: the entity to remove.
 */
/*
 * [한국어]
 * bfq_extract - entity를 rb-tree(root)에서 제거하는 가장 저수준의
 *   공통 헬퍼.
 *
 * @root: entity가 속해 있던 rb_root(active 또는 idle 트리의 루트).
 * @entity: 제거할 entity.
 * @return: 없음(void).
 *
 * entity->tree를 NULL로 리셋해 "지금 어떤 트리에도 속해 있지 않음"을
 * 표시한 뒤, 커널 rb-tree 코어 함수 rb_erase()로 실제 트리 구조에서
 * 노드를 뽑아낸다. 이 함수 자체는 min_start 캐시나 first_idle/last_idle
 * 같은 상위 메타데이터를 갱신하지 않으므로, active tree에서 제거할
 * 때는 bfq_active_extract()가, idle tree에서 제거할 때는
 * bfq_idle_extract()가 이 함수를 호출한 뒤 각자 필요한 후처리를 이어서
 * 수행한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_idle_extract()/bfq_active_extract() -> [bfq_extract] ->
 *   rb_erase() (커널 rb-tree 코어)
 */
static void bfq_extract(struct rb_root *root, struct bfq_entity *entity)
{
	entity->tree = NULL; /* "어떤 트리에도 속하지 않음" 상태로 리셋: 이후 entity->tree == NULL 검사가 이 사실을 반영 */
	rb_erase(&entity->rb_node, root); /* 커널 rb-tree 코어 함수로 실제 노드를 트리에서 제거하고 균형을 재조정 */
}

/**
 * bfq_idle_extract - extract an entity from the idle tree.
 * @st: the service tree of the owning @entity.
 * @entity: the entity being removed.
 */
/*
 * [한국어]
 * bfq_idle_extract - idle tree(더 이상 backlogged는 아니지만 아직
 *   완전히 잊혀지지 않은 entity들의 트리)에서 entity를 제거한다.
 *
 * @st: entity가 속한 bfq_service_tree(idle/first_idle/last_idle 필드를
 *   함께 갖고 있음).
 * @entity: idle tree에서 제거할 entity.
 * @return: 없음(void).
 *
 * idle tree는 first_idle(finish가 가장 작은 entity)과 last_idle(finish가
 * 가장 큰 entity)이라는 두 개의 캐시 포인터를 추가로 유지한다
 * (bfq_forget_idle()이 vtime을 언제 얼마나 전진시킬지 판단하는 데 씀).
 * 이 함수는 제거하려는 entity가 마침 first_idle이나 last_idle과 같다면
 * rb_next()/rb_prev()로 "그 다음으로 작은/큰" entity를 찾아 캐시를
 * 미리 갱신한 뒤(bfq_entity_of()로 NULL 안전하게 변환), 실제 트리
 * 제거는 bfq_extract()에 위임한다. entity가 leaf(bfq_queue)라면
 * 장치 전역 idle_list(struct list_head)에서도 함께 제거해, idle
 * 상태의 큐만 빠르게 순회할 수 있는 보조 리스트의 일관성도 유지한다.
 * bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_put_idle_entity()/__bfq_activate_entity() ->
 *   [bfq_idle_extract] -> bfq_entity_of()/bfq_extract()
 */
static void bfq_idle_extract(struct bfq_service_tree *st,
			     struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 idle_list에서도 제거해야 하므로 미리 얻어둠 */
	struct rb_node *next; /* first_idle/last_idle 갱신 시 임시로 쓸 인접 노드 포인터 */

	if (entity == st->first_idle) { /* 제거 대상이 마침 "가장 빨리 만료될" 캐시 포인터라면 */
		next = rb_next(&entity->rb_node); /* 중위 순회 상 다음(더 큰 finish) 노드를 찾음 */
		st->first_idle = bfq_entity_of(next); /* 새로운 first_idle로 갱신(트리에 더 없으면 NULL) */
	}

	if (entity == st->last_idle) { /* 제거 대상이 마침 "가장 늦게 만료될" 캐시 포인터라면 */
		next = rb_prev(&entity->rb_node); /* 중위 순회 상 이전(더 작은 finish) 노드를 찾음 */
		st->last_idle = bfq_entity_of(next); /* 새로운 last_idle로 갱신(트리에 더 없으면 NULL) */
	}

	bfq_extract(&st->idle, entity); /* 실제 rb-tree 구조에서 entity를 제거(entity->tree도 NULL로 리셋됨) */

	if (bfqq) /* leaf였다면 */
		list_del(&bfqq->bfqq_list); /* 장치 전역 idle_list에서도 함께 제거해 리스트 일관성 유지 */
}

/**
 * bfq_insert - generic tree insertion.
 * @root: tree root.
 * @entity: entity to insert.
 *
 * This is used for the idle and the active tree, since they are both
 * ordered by finish time.
 */
/*
 * [한국어]
 * bfq_insert - entity를 finish 시각(F_i) 순으로 정렬된 rb-tree에
 *   삽입하는 범용 저수준 헬퍼(active tree와 idle tree 모두에서 재사용).
 *
 * @root: 삽입할 rb_root(&st->active 또는 &st->idle).
 * @entity: 삽입할 entity(entity->finish가 이미 유효한 값으로 계산돼
 *   있어야 함).
 * @return: 없음(void).
 *
 * 표준적인 이진 탐색 트리 삽입 절차를 그대로 따른다: 루트에서 시작해
 * 각 노드의 finish와 비교하며 삽입할 위치(리프가 될 자리)를 찾는다.
 * 기존 노드의 finish가 삽입할 entity의 finish보다 크면(bfq_gt) 왼쪽
 * 서브트리로, 그렇지 않으면(같거나 작으면) 오른쪽 서브트리로 내려가
	 * 결과적으로 "finish가 작은 entity일수록 트리의 왼쪽(먼저 방문되는
 * 쪽)에 위치"하는 불변식을 만든다. 탐색이 끝나 리프 위치를 찾으면
 * rb_link_node()로 새 노드를 그 자리에 연결하고 rb_insert_color()로
 * 적흑트리(red-black tree) 균형을 재조정한다. 마지막으로 entity->tree에
 * root를 기록해 "지금 이 트리에 속해 있음"을 표시한다. 이 함수는 삽입
 * "그 자체"만 담당하며, active tree 전용의 min_start 캐시 갱신
 * (bfq_update_active_tree)이나 active_list/idle_list 리스트 등록은
 * 호출자(bfq_active_insert/bfq_idle_insert)가 이어서 처리한다. bfqd->lock
 * 하에서 호출.
 *
 * 호출 체인:
 *   bfq_active_insert()/bfq_idle_insert() -> [bfq_insert] ->
 *   bfq_gt()/rb_link_node()/rb_insert_color() (커널 rb-tree 코어)
 */
static void bfq_insert(struct rb_root *root, struct bfq_entity *entity)
{
	struct bfq_entity *entry; /* 탐색 도중 현재 비교 대상이 되는 기존 트리 노드의 entity */
	struct rb_node **node = &root->rb_node; /* 삽입 위치를 찾아 내려가는 이중 포인터(부모의 좌/우 자식 슬롯을 가리킴) */
	struct rb_node *parent = NULL; /* 탐색이 끝났을 때의 부모 노드(리프의 부모가 됨) */

	while (*node) { /* *node가 NULL이 될 때까지(=빈 슬롯을 찾을 때까지) 트리를 따라 내려감 */
		parent = *node; /* 현재 위치를 부모 후보로 기록 */
		entry = rb_entry(parent, struct bfq_entity, rb_node); /* 현재 노드가 감싸는 entity를 얻어 finish 비교에 사용 */

		if (bfq_gt(entry->finish, entity->finish)) /* 기존 노드의 finish가 삽입할 entity보다 크면(더 늦게 끝남) */
			node = &parent->rb_left; /* finish가 작은(더 급한) entity는 왼쪽 서브트리로 */
		else /* 기존 노드의 finish가 같거나 작으면 */
			node = &parent->rb_right; /* 오른쪽 서브트리로 내려가 정렬 순서 유지 */
	}

	rb_link_node(&entity->rb_node, parent, node); /* 찾은 빈 슬롯에 새 rb_node를 실제로 연결(아직 색상/균형 미조정 상태) */
	rb_insert_color(&entity->rb_node, root); /* 적흑트리 삽입 규칙에 따라 색상을 정하고 필요하면 회전으로 균형 복구 */

	entity->tree = root; /* entity가 이제 이 트리(root)에 속해 있음을 기록 */
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
/*
 * [한국어]
 * bfq_update_min - entity의 min_start 캐시 값을, 주어진 자식 서브트리
 *   (node)의 min_start와 비교해 필요하면 끌어내린다.
 *
 * @entity: min_start를 갱신할 대상(부모) entity.
 * @node: entity의 왼쪽 또는 오른쪽 자식 rb_node(이미 그 서브트리
 *   내에서는 min_start가 올바르다고 가정).
 * @return: 없음(void). entity->min_start를 직접 갱신한다.
 *
 * min_start는 "이 entity를 루트로 하는 active 서브트리 전체에서 가장
 * 작은 start(S_i) 값"을 캐시하는 augmented rb-tree 필드다(정의는
 * bfq-iosched.h 참고). 이 캐시 덕분에 bfq_first_active_entity()가
 * 트리 전체를 선형 탐색하지 않고도 O(log N)에 "eligible한(start <=
 * vtime) entity가 있는 서브트리"를 판별할 수 있다. 이 함수는 자식
 * node가 존재할 때, 그 자식의 min_start가 현재 entity의 min_start보다
 * 작다면(bfq_gt로 판정) entity->min_start를 그 자식의 값으로 낮춘다 -
 * 즉 "부모의 min_start는 자신의 start와 양쪽 자식의 min_start 중
 * 최솟값"이라는 불변식을 한 방향(한 자식)에 대해서만 부분적으로
 * 적용하는 헬퍼다. bfq_update_active_node()가 왼쪽/오른쪽 자식
 * 양쪽에 대해 이 함수를 두 번 호출함으로써 완전한 최솟값 병합을
 * 완성한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_active_node() -> [bfq_update_min] -> bfq_gt()
 */
static void bfq_update_min(struct bfq_entity *entity, struct rb_node *node)
{
	struct bfq_entity *child; /* node가 가리키는 자식 서브트리의 루트 entity */

	if (node) { /* 해당 방향(좌/우)에 실제로 자식이 존재할 때만 병합 시도 */
		child = rb_entry(node, struct bfq_entity, rb_node); /* rb_node로부터 자식 entity를 복원 */
		if (bfq_gt(entity->min_start, child->min_start)) /* 부모의 현재 min_start가 자식의 min_start보다 크면(즉 더 늦은 시각이면) */
			entity->min_start = child->min_start; /* 더 이른(작은) 자식의 min_start로 부모 값을 끌어내림 */
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
/*
 * [한국어]
 * bfq_update_active_node - 특정 rb_node 하나의 min_start를 "자신의
 *   start"와 "좌/우 자식의 min_start" 세 값 중 최솟값으로 완전히
 *   재계산한다.
 *
 * @node: min_start를 재계산할 노드(위치가 바뀌었거나 자식 중 하나가
 *   변경된 노드).
 * @return: 없음(void).
 *
 * 먼저 entity->min_start를 entity 자신의 start(S_i)로 리셋해 "적어도
 * 자기 자신의 start보다 작을 수는 없다"는 기준선을 세운 뒤,
 * bfq_update_min()을 오른쪽 자식과 왼쪽 자식에 대해 각각 한 번씩 호출해
 * 더 작은 값이 있으면 끌어내린다. 이 함수가 정확히 동작하려면 호출
 * 시점에 좌우 서브트리의 min_start가 "이미 올바르다"는 전제가 필요하며
 * (함수 자신의 서브트리까지 재귀적으로 다시 계산하지는 않음), 이 전제는
 * 호출자인 bfq_update_active_tree()가 트리의 가장 깊은 변경 지점부터
 * 시작해 루트 방향으로 순서대로 호출함으로써 보장한다. bfqd->lock
 * 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_active_tree() -> [bfq_update_active_node] ->
 *   bfq_update_min()
 */
static void bfq_update_active_node(struct rb_node *node)
{
	struct bfq_entity *entity = rb_entry(node, struct bfq_entity, rb_node); /* node가 감싸는 entity를 얻음 */

	entity->min_start = entity->start; /* 기준선: 최소한 자기 자신의 start보다 작을 수는 없으므로 우선 이 값으로 초기화 */
	bfq_update_min(entity, node->rb_right); /* 오른쪽 서브트리에 더 작은 min_start가 있으면 반영 */
	bfq_update_min(entity, node->rb_left); /* 왼쪽 서브트리에 더 작은 min_start가 있으면 반영(양쪽 다 검사해 진짜 최솟값 완성) */
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
/*
 * [한국어]
 * bfq_update_active_tree - 트리 수정(삽입/삭제) 이후, 가장 깊이
 *   변경된 노드에서부터 루트까지 min_start 캐시를 순서대로 다시
 *   계산해 트리 전체의 augmented 불변식을 복구한다.
 *
 * @node: 이번 수정으로 인해 min_start가 무효화됐을 수 있는 "가장
 *   깊은" 노드(삽입이면 새로 연결된 리프의 자식 방향, 삭제면
 *   bfq_find_deepest()가 계산한 노드).
 * @return: 없음(void).
 *
 * rb-tree 삽입/삭제는 회전(rotation)을 동반할 수 있어 여러 노드의
 * 부모-자식 관계가 바뀔 수 있지만, 그 변경의 영향은 항상 "실제로 이동한
 * 노드들이 이루는 경로"와 "그 경로상 노드들의 형제(sibling)"로 국한된다.
 * 이 함수는 가장 깊은 변경 지점(node)에서 시작해 bfq_update_active_node()로
 * 그 노드의 min_start를 다시 계산한 뒤, 부모로 한 단계씩 올라가면서
 * (goto up 루프) 그 부모의 "다른 쪽" 자식(형제)도 함께 재계산하고,
 * 부모 자신도 재계산한다 - 이렇게 자식이 바뀐 노드는 반드시 그 자신도
 * min_start가 바뀔 수 있으므로 연쇄적으로 루트까지 전파해야 하기
 * 때문이다. 루트(parent가 NULL)에 도달하면 종료한다. 이 알고리즘
 * 덕분에 트리 전체를 O(N)으로 재계산하지 않고 변경 경로만 O(log N)으로
 * 갱신할 수 있다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_active_insert()/bfq_active_extract() -> [bfq_update_active_tree]
 *   -> bfq_update_active_node() (반복 호출)
 */
static void bfq_update_active_tree(struct rb_node *node)
{
	struct rb_node *parent; /* 현재 node의 부모(루프를 타고 루트까지 올라가는 데 사용) */

up: /* [한국어] goto 레이블: 아래에서 부모로 이동한 뒤 다시 이 지점으로 돌아와 반복(트리 깊이만큼 순회) */
	bfq_update_active_node(node); /* 현재 node의 min_start를 자신의 start와 좌우 자식 값으로 재계산 */

	parent = rb_parent(node); /* 커널 rb-tree 코어 헬퍼로 현재 노드의 부모를 얻음 */
	if (!parent) /* 부모가 없다는 것은 node가 이미 루트라는 뜻 */
		return; /* 루트까지 전파를 완료했으므로 갱신 종료 */

	if (node == parent->rb_left && parent->rb_right) /* node가 부모의 왼쪽 자식이고, 오른쪽 형제도 존재하면 */
		bfq_update_active_node(parent->rb_right); /* 오른쪽 형제도 (node 변경의 영향권 안에 있을 수 있으므로) 재계산 */
	else if (parent->rb_left) /* (node가 오른쪽 자식이었거나 오른쪽 형제가 없는 경우) 왼쪽 자식이 있으면 */
		bfq_update_active_node(parent->rb_left); /* 왼쪽 형제를 재계산 */

	node = parent; /* 한 단계 위(부모)로 이동 */
	goto up; /* 루트에 도달할 때까지 위 과정을 반복 */
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
 * [한국어]
 * bfq_active_insert - entity를 active tree(backlogged entity들의 rb-tree)에
 *   삽입하고, min_start 캐시와 보조 리스트(active_list)/그룹 카운터까지
 *   함께 갱신한다.
 *
 * @st: entity가 삽입될 bfq_service_tree(&st->active가 실제 rb_root).
 * @entity: 삽입할 entity(entity->finish가 이미 계산돼 있어야 함).
 * @return: 없음(void).
 *
 * bfq_insert()로 실제 rb-tree 삽입을 수행한 뒤, 방금 삽입된 노드의
 * 자식(왼쪽 우선, 없으면 오른쪽)을 "가장 깊이 변경된 노드"로 삼아
 * bfq_update_active_tree()를 호출해 min_start 캐시를 루트까지 전파한다
 * (새로 삽입된 리프 자신은 자식이 없어 min_start가 이미 entity->start와
 * 같으므로, 그 자식이 있다면 그 방향에서부터 다시 계산을 시작해도
 * 결과가 같다는 성질을 이용한 최적화). entity가 leaf(bfq_queue)라면
 * 장치 전역 active_list[bfqq->actuator_idx]에도 등록해, 특정 actuator
 * (다중 액추에이터 드라이브에서 독립적으로 움직이는 헤드 그룹) 소속
 * 큐만 빠르게 순회할 수 있는 보조 리스트를 유지한다. 마지막으로
 * bfq_inc_active_entities()를 호출해 부모 그룹의 active 자식 카운트를
 * 갱신한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_fin_time_enqueue() -> [bfq_active_insert] ->
 *   bfq_insert()/bfq_update_active_tree()/bfq_inc_active_entities()
 */
static void bfq_active_insert(struct bfq_service_tree *st,
			      struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 active_list에도 등록해야 하므로 미리 얻어둠 */
	struct rb_node *node = &entity->rb_node; /* 방금 삽입된 entity의 rb_node - min_start 전파 시작점 계산에 사용 */

	bfq_insert(&st->active, entity); /* finish 순서 불변식을 지키며 실제 rb-tree에 entity를 삽입 */

	if (node->rb_left) /* 삽입된 노드에 왼쪽 자식이 생겼다면(회전 등으로) */
		node = node->rb_left; /* 그 자식을 min_start 재계산의 시작점으로 삼음 */
	else if (node->rb_right) /* 왼쪽 자식은 없지만 오른쪽 자식이 있다면 */
		node = node->rb_right; /* 오른쪽 자식을 시작점으로 삼음 */

	bfq_update_active_tree(node); /* 시작점부터 루트까지 min_start 캐시를 순서대로 재계산 */

	if (bfqq) /* leaf(실제 프로세스 큐)였다면 */
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->active_list[bfqq->actuator_idx]); /* 해당 actuator 전용 active_list에도 등록 */

	bfq_inc_active_entities(entity); /* 부모 bfq_group의 active_entities 카운터를 1 증가 */
}

/**
 * bfq_ioprio_to_weight - calc a weight from an ioprio.
 * @ioprio: the ioprio value to convert.
 */
/*
 * [한국어]
 * bfq_ioprio_to_weight - CFQ 호환 ioprio 레벨 값을 BFQ의 weight 수치로
 *   변환한다.
 *
 * @ioprio: IOPRIO_PRIO_LEVEL()로 뽑아낸 우선순위 레벨(0~7, 숫자가
 *   작을수록 더 높은 우선순위).
 * @return: (IOPRIO_NR_LEVELS - ioprio) * BFQ_WEIGHT_CONVERSION_COEFF로
 *   계산된 weight 값(ioprio가 작을수록, 즉 우선순위가 높을수록 weight가
 *   커짐).
 *
 * 레거시 ionice(1)/ioprio_set() 인터페이스는 "숫자가 작을수록 우선"이라는
 * 반비례 스케일을 쓰지만, B-WF2Q+ 알고리즘은 "weight가 클수록 유리"라는
 * 정비례 스케일을 쓰므로 두 체계를 잇는 선형 변환이 필요하다. 이 함수는
 * 그 변환을 수행해, 사용자가 ionice로 지정한 우선순위를 F_i 계산에
 * 쓰이는 weight로 바꿔준다. 순수 산술 변환으로 락이 필요 없다.
 *
 * 호출 체인:
 *   __bfq_entity_update_weight_prio()의 역방향 대응 함수(호출부는
 *   ioprio 설정 경로, bfq-iosched.c) -> [bfq_ioprio_to_weight] ->
 *   (하위 호출 없음, 산술 계산만)
 */
unsigned short bfq_ioprio_to_weight(int ioprio)
{
	return (IOPRIO_NR_LEVELS - ioprio) * BFQ_WEIGHT_CONVERSION_COEFF; /* 레벨이 낮을수록(우선순위 높음) 결과 weight가 커지도록 반전 후 배율 곱함 */
}

/**
 * bfq_weight_to_ioprio - calc an ioprio from a weight.
 * @weight: the weight value to convert.
 *
 * To preserve as much as possible the old only-ioprio user interface,
 * 0 is used as an escape ioprio value for weights (numerically) equal or
 * larger than IOPRIO_NR_LEVELS * BFQ_WEIGHT_CONVERSION_COEFF.
 */
/*
 * [한국어]
 * bfq_weight_to_ioprio - bfq_ioprio_to_weight()의 역변환: weight 값을
 *   다시 근사적인 ioprio 레벨로 되돌린다.
 *
 * @weight: 되돌릴 weight 값(cgroup weight 설정 등으로 ioprio 범위를
 *   벗어나게 커졌을 수도 있음).
 * @return: max(0, IOPRIO_NR_LEVELS - weight / BFQ_WEIGHT_CONVERSION_COEFF).
 *   weight가 커서 이 값이 음수가 될 상황이면 0("설정 안 됨"을 뜻하는
 *   escape 값)으로 클리핑된다.
 *
 * cgroup을 통해 weight를 직접 설정한 큐는 ioprio 체계로 정확히 표현되지
 * 않을 수 있으므로, 사용자에게 "대략 어느 ioprio에 해당하는지"를
 * 보여주기 위한 근사 역변환이다(예: sysfs/ioctl로 현재 ioprio를
 * 조회하는 레거시 인터페이스 호환). weight가 IOPRIO_NR_LEVELS *
 * BFQ_WEIGHT_CONVERSION_COEFF 이상으로 크면 결과가 0 이하가 되는데,
 * 이 경우 0을 "ioprio로 표현 불가"라는 특수값으로 그대로 반환한다.
 * 순수 산술 변환으로 락이 필요 없다.
 *
 * 호출 체인:
 *   __bfq_entity_update_weight_prio() -> [bfq_weight_to_ioprio] ->
 *   (하위 호출 없음, 산술 계산만)
 */
static unsigned short bfq_weight_to_ioprio(int weight)
{
	return max_t(int, 0, /* 계산 결과가 음수가 되지 않도록 0으로 하한을 둠(over-weight 큐를 위한 escape 값) */
		     IOPRIO_NR_LEVELS - weight / BFQ_WEIGHT_CONVERSION_COEFF); /* weight를 배율로 나눠 레벨 스케일로 되돌린 뒤 반전 */
}

static void bfq_get_entity(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_get_entity - entity가 leaf(bfq_queue)라면 그 큐에 대한
	 *   "서비스 참조 카운트(service reference)"를 하나 얻는다
	 *   (bfqq->ref를 증가).
	 *
	 * @entity: 참조를 얻을 entity(leaf가 아니면 아무 동작도 하지 않음).
	 * @return: 없음(void).
	 *
	 * entity가 active tree에 삽입되어 스케줄링 대상이 되는 순간, 그
	 * bfq_queue는 "스케줄러가 아직 이 큐를 참조하고 있다"는 사실을
	 * ref 카운트로 보장받아야 한다 - 그렇지 않으면 이 큐를 참조하던
	 * 마지막 프로세스가 사라져도 스케줄러가 계속 붙잡고 있는 큐가
	 * 조기에 해제(free)되어 use-after-free가 발생할 수 있다. 이 참조는
	 * 나중에 entity가 서비스 트리를 완전히 떠날 때(bfq_forget_entity())
	 * bfq_put_queue()로 반납된다. leaf일 때만 bfq_log_bfqq()로 참조
	 * 카운트 변화를 디버그 트레이스에 남긴다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   __bfq_activate_entity() -> [bfq_get_entity] -> bfq_log_bfqq()
	 */
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 대응하는 bfq_queue, 아니면 NULL */

	if (bfqq) { /* leaf(실제 프로세스 큐)일 때만 참조 카운트 개념이 적용됨 */
		bfqq->ref++; /* "스케줄러가 붙잡고 있음"을 나타내는 참조 카운트 증가 - 조기 해제 방지 */
		bfq_log_bfqq(bfqq->bfqd, bfqq, "get_entity: %p %d", /* 디버그 트레이스: 어느 큐의 ref가 몇으로 바뀌었는지 기록 */
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
/*
 * [한국어]
 * bfq_find_deepest - 곧 제거될 node를 대신할 successor를 찾고, 그 제거로
 *   인해 min_start가 무효화될 수 있는 "가장 깊은" 노드를 미리 계산한다.
 *
 * @node: 제거될 예정인 rb_node.
 * @return: 이후 rb_erase()가 수행할 트리 재조정에서 영향을 받을 수
 *   있는 가장 깊은 노드. node가 트리의 유일한 노드였다면 NULL(더
 *   갱신할 것이 없음).
 *
 * rb_erase()는 제거할 노드가 자식을 몇 개 가졌는지에 따라 내부적으로
 * 노드를 재배치한다: 자식이 없으면 부모가 직접 영향받고, 자식이
 * 하나뿐이면 그 자식이, 자식이 둘이면 중위 순회상 successor(오른쪽
 * 서브트리의 최솟값, 여기서는 rb_next())가 실제로 이동해 삭제될
 * 노드의 자리를 대신한다. 이 함수는 rb_erase()를 호출하기 "전에" 이
 * 논리를 미리 재현해, 실제 rb-tree 삭제가 일어나기 전 시점의 트리
 * 구조를 보고 "삭제 후 min_start 재계산을 시작해야 할 가장 깊은 지점"을
 * 미리 판별해 둔다(rb_erase 이후에는 포인터 관계가 이미 바뀌어 있어
 * 판별이 불가능하거나 더 복잡해지기 때문). 네 가지 경우를 순서대로
 * 처리한다: (1) 자식이 전혀 없으면 부모가 deepest, (2) 오른쪽 자식만
 * 없으면 왼쪽 자식이 deepest, (3) 왼쪽 자식만 없으면 오른쪽 자식이
 * deepest, (4) 양쪽 다 있으면 successor(rb_next)를 찾고, 그 successor의
 * 오른쪽 자식이 있으면 그것이, 없고 successor가 node의 직접 자식이
 * 아니면 successor의 부모가 deepest가 된다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_active_extract() -> [bfq_find_deepest] -> rb_next()/rb_parent()
 *   (커널 rb-tree 코어)
 */
static struct rb_node *bfq_find_deepest(struct rb_node *node)
{
	struct rb_node *deepest; /* 계산 결과로 반환할, 삭제 후 min_start 재계산을 시작할 노드 */

	if (!node->rb_right && !node->rb_left) /* 자식이 전혀 없는 리프 노드라면 */
		deepest = rb_parent(node); /* 삭제 시 실질적으로 바뀌는 것은 부모의 자식 포인터뿐이므로 부모가 deepest */
	else if (!node->rb_right) /* 오른쪽 자식만 없다면(왼쪽 자식만 있음) */
		deepest = node->rb_left; /* 왼쪽 자식이 node 자리를 대신하므로 그 자식이 deepest */
	else if (!node->rb_left) /* 왼쪽 자식만 없다면(오른쪽 자식만 있음) */
		deepest = node->rb_right; /* 오른쪽 자식이 node 자리를 대신하므로 그 자식이 deepest */
	else { /* 양쪽 자식이 모두 있는 경우: rb_erase 내부적으로 successor로 대체됨 */
		deepest = rb_next(node); /* 중위 순회상 다음 노드(오른쪽 서브트리의 최솟값)가 successor */
		if (deepest->rb_right) /* successor 자신이 오른쪽 자식을 갖고 있다면(successor는 왼쪽 자식이 없다는 성질 이용) */
			deepest = deepest->rb_right; /* successor가 빠져나간 자리를 그 오른쪽 자식이 대신하므로 그것이 deepest */
		else if (rb_parent(deepest) != node) /* successor에 자식이 없고, successor가 node의 직접 자식도 아니라면 */
			deepest = rb_parent(deepest); /* successor가 빠진 자리(그 부모)가 실질적인 변경 지점 */
	}

	return deepest; /* 이후 bfq_update_active_tree()가 이 지점부터 루트까지 min_start를 재계산 */
}

/**
 * bfq_active_extract - remove an entity from the active tree.
 * @st: the service_tree containing the tree.
 * @entity: the entity being removed.
 */
/*
 * [한국어]
 * bfq_active_extract - entity를 active tree에서 제거하고, min_start
 *   캐시/active_list/그룹 카운터를 함께 갱신한다(bfq_active_insert()의
 *   역연산).
 *
 * @st: entity가 속해 있던 bfq_service_tree.
 * @entity: 제거할 entity.
 * @return: 없음(void).
 *
 * 먼저 bfq_find_deepest()로 "제거 후 min_start 재계산을 시작할 지점"을
 * rb_erase() 호출 전에 미리 계산해 둔 뒤, bfq_extract()로 실제 rb-tree
 * 제거를 수행한다(순서가 중요: deepest 계산은 반드시 트리가 아직 변경
 * 전 상태일 때 이뤄져야 한다). 계산된 지점이 있으면(트리가 완전히
 * 비지 않았다면) bfq_update_active_tree()로 min_start를 루트까지 다시
 * 전파한다. entity가 leaf(bfq_queue)라면 active_list에서도 제거하고,
 * 마지막으로 bfq_dec_active_entities()로 부모 그룹의 active 자식
 * 카운트를 감소시킨다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_requeue_entity()/__bfq_deactivate_entity()/bfq_get_next_queue()
 *   -> [bfq_active_extract] -> bfq_find_deepest()/bfq_extract()/
 *   bfq_update_active_tree()/bfq_dec_active_entities()
 */
static void bfq_active_extract(struct bfq_service_tree *st,
			       struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 active_list에서도 제거해야 하므로 미리 얻어둠 */
	struct rb_node *node; /* bfq_find_deepest()가 계산할 min_start 재전파 시작점 */

	node = bfq_find_deepest(&entity->rb_node); /* 실제 삭제 전에, 삭제로 영향받을 가장 깊은 노드를 미리 계산 */
	bfq_extract(&st->active, entity); /* 실제 rb-tree에서 entity를 제거(entity->tree도 NULL로 리셋) */

	if (node) /* 트리에 다른 노드가 남아 min_start 재계산이 의미 있다면 */
		bfq_update_active_tree(node); /* 계산해 둔 지점부터 루트까지 min_start 캐시를 다시 전파 */
	if (bfqq) /* leaf였다면 */
		list_del(&bfqq->bfqq_list); /* actuator별 active_list에서도 제거 */

	bfq_dec_active_entities(entity); /* 부모 그룹의 active_entities 카운터를 1 감소 */
}

/**
 * bfq_idle_insert - insert an entity into the idle tree.
 * @st: the service tree containing the tree.
 * @entity: the entity to insert.
 */
/*
 * [한국어]
 * bfq_idle_insert - entity를 idle tree(finish는 지났지만 vtime이 아직
 *   그 finish를 넘어서지 않아 완전히 잊지 않은 entity들의 트리)에
 *   삽입한다.
 *
 * @st: entity가 삽입될 bfq_service_tree.
 * @entity: 삽입할 entity(entity->finish가 이미 확정돼 있어야 함).
 * @return: 없음(void).
 *
 * 삽입 전에 이 entity의 finish가 기존 first_idle/last_idle보다
 * 작거나/크면 그 캐시 포인터를 새 entity로 갱신한다(트리가 비어
 * 있었다면 무조건 첫 entity가 양쪽 다 차지). 실제 rb-tree 삽입은
 * bfq_insert()에 위임한다(active tree와 마찬가지로 finish 순 정렬이지만,
 * idle tree는 min_start 증강이 필요 없으므로 bfq_update_active_tree()
 * 호출이 없다는 점이 bfq_active_insert()와의 차이). entity가
 * leaf(bfq_queue)라면 장치 전역 idle_list에도 등록한다. bfqd->lock
 * 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_deactivate_entity() -> [bfq_idle_insert] -> bfq_insert()
 */
static void bfq_idle_insert(struct bfq_service_tree *st,
			    struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 idle_list 등록 대상이므로 미리 얻어둠 */
	struct bfq_entity *first_idle = st->first_idle; /* 갱신 전 기존 "가장 빨리 만료될" 캐시 */
	struct bfq_entity *last_idle = st->last_idle; /* 갱신 전 기존 "가장 늦게 만료될" 캐시 */

	if (!first_idle || bfq_gt(first_idle->finish, entity->finish)) /* 트리가 비어 있었거나, 새 entity의 finish가 더 작으면 */
		st->first_idle = entity; /* 새 entity가 "가장 빨리 만료될" 후보로 갱신 */
	if (!last_idle || bfq_gt(entity->finish, last_idle->finish)) /* 트리가 비어 있었거나, 새 entity의 finish가 더 크면 */
		st->last_idle = entity; /* 새 entity가 "가장 늦게 만료될" 후보로 갱신 */

	bfq_insert(&st->idle, entity); /* finish 순 정렬을 유지하며 idle rb-tree에 실제 삽입 */

	if (bfqq) /* leaf였다면 */
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->idle_list); /* 장치 전역 idle_list에도 등록 */
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
 * [한국어]
 * bfq_forget_entity - entity를 스케줄러가 더 이상 고려하지 않도록
 *   완전히 "잊는다"(어떤 트리에도 속하지 않고, 서비스 참조도 반납).
 *
 * @st: entity가 속해 있던(혹은 있던 것으로 간주되는) bfq_service_tree.
 * @entity: 잊을 entity.
 * @is_in_service: true이면 entity가 지금 in-service 상태임(즉 아직
 *   __bfq_bfqd_reset_in_service()가 처리하지 않은 서비스 참조가 남아있음).
 * @return: 없음(void).
 *
 * entity->on_st_or_in_serv를 false로 낮춰 "트리에도, 서비스 중에도
 * 있지 않음"을 표시하고, service_tree의 wsum(총 weight 합)에서 이
 * entity의 weight를 빼 vtime 전진 계산의 분모를 정확히 유지한다.
 * entity가 leaf(bfq_queue)이고 is_in_service가 false라면(이미 서비스
 * 중이 아니라면) bfq_get_entity()가 잡았던 서비스 참조를 지금
 * bfq_put_queue()로 반납한다 - is_in_service가 true인 경우에는 아직
 * 서비스가 끝나지 않았으므로, 나중에 __bfq_bfqd_reset_in_service()가
 * 서비스가 실제로 끝나는 시점에 참조를 반납하도록 미룬다(원본 주석
 * 참고). bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_put_idle_entity()/__bfq_deactivate_entity() ->
 *   [bfq_forget_entity] -> bfq_put_queue()
 */
static void bfq_forget_entity(struct bfq_service_tree *st,
			      struct bfq_entity *entity,
			      bool is_in_service)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 서비스 참조 반납 대상이 되는 bfq_queue */

	entity->on_st_or_in_serv = false; /* "트리 위에도, 서비스 중도 아님" 상태로 확정 표시 */
	st->wsum -= entity->weight; /* 이 entity의 weight를 총합에서 빼 vtime 정규화 분모를 정확히 유지 */
	if (bfqq && !is_in_service) /* leaf이고 지금 서비스 중이 아니라면(서비스 참조를 지금 반납해도 안전) */
		bfq_put_queue(bfqq); /* bfq_get_entity()가 잡았던 서비스 참조 카운트를 반납, ref가 0이 되면 bfqq 해제 */
}

/**
 * bfq_put_idle_entity - release the idle tree ref of an entity.
 * @st: service tree for the entity.
 * @entity: the entity being released.
 */
/*
 * [한국어]
 * bfq_put_idle_entity - idle tree에 있던 entity를 트리에서 뽑아내고
 *   완전히 잊는다(idle 상태의 entity를 최종 정리하는 진입점).
 *
 * @st: entity가 속한 bfq_service_tree.
 * @entity: 정리할 entity.
 * @return: 없음(void).
 *
 * bfq_idle_extract()로 idle rb-tree 및 idle_list에서 제거한 뒤,
 * bfq_forget_entity()를 호출해 완전히 스케줄러의 고려 대상에서
 * 빼낸다. is_in_service 인자는 "entity가 마침 그 sched_data의
 * in_service_entity와 같은가"를 즉석에서 계산해 넘긴다 - 이 함수가
 * 호출되는 시점에 entity가 실제로 서비스 중일 가능성은 이론상 낮지만
 * (idle tree에 있다는 것 자체가 보통 비활성 상태를 뜻함), 방어적으로
 * 정확한 값을 계산해 전달한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_forget_idle() -> [bfq_put_idle_entity] -> bfq_idle_extract()/
 *   bfq_forget_entity()
 */
void bfq_put_idle_entity(struct bfq_service_tree *st, struct bfq_entity *entity)
{
	bfq_idle_extract(st, entity); /* idle rb-tree 및 idle_list에서 제거 */
	bfq_forget_entity(st, entity, /* 스케줄러 고려 대상에서 완전히 제거하고 필요하면 서비스 참조도 반납 */
			  entity == entity->sched_data->in_service_entity); /* 혹시라도 지금 서비스 중인지 즉석 판정해 전달 */
}

/**
 * bfq_forget_idle - update the idle tree if necessary.
 * @st: the service tree to act upon.
 *
 * To preserve the global O(log N) complexity we only remove one entry here;
 * as the idle tree will not grow indefinitely this can be done safely.
 */
/*
 * [한국어]
 * bfq_forget_idle - active tree가 비어 있고 idle tree의 entity들도
 *   이미 만료됐다면, vtime을 앞으로 점프시키고 가장 오래된 idle
 *   entity 하나를 정리한다.
 *
 * @st: 정리할 bfq_service_tree.
 * @return: 없음(void).
 *
 * active tree가 완전히 비어 있는데(RB_EMPTY_ROOT) idle tree에 entity가
 * 남아있고, 그 중 가장 늦게 만료되는 last_idle의 finish조차 이미
 * vtime을 넘지 않았다면(즉 모든 idle entity가 "논리적으로는 이미
 * 지나간" 상태라면), vtime을 last_idle->finish까지 통째로 앞당긴다 -
 * 더 이상 서비스할 active entity가 없는데 vtime만 계속 정지해 있으면
 * 나중에 새 entity가 활성화될 때 부당하게 오래된 vtime을 기준으로
 * eligibility가 계산되는 것을 막기 위함이다. 그 다음, first_idle(가장
 * 빨리 만료되는 entity)의 finish가 이미 새 vtime을 넘지 않는다면
 * bfq_put_idle_entity()로 그 entity 하나만 idle tree에서 완전히
 * 제거한다 - 원본 주석에 설명된 대로 "한 번에 하나씩만" 정리해도
 * idle tree가 무한정 커지지 않으므로 전체 알고리즘의 O(log N) 복잡도를
 * 해치지 않으면서 점진적으로 청소하는 전략이다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_bfqq_served()/__bfq_lookup_next_entity()/bfq_update_vtime() ->
 *   [bfq_forget_idle] -> bfq_put_idle_entity()
 */
static void bfq_forget_idle(struct bfq_service_tree *st)
{
	struct bfq_entity *first_idle = st->first_idle; /* idle tree에서 가장 빨리 만료되는 entity 캐시 */
	struct bfq_entity *last_idle = st->last_idle; /* idle tree에서 가장 늦게 만료되는 entity 캐시 */

	if (RB_EMPTY_ROOT(&st->active) && last_idle && /* active tree가 완전히 비어 있고 idle tree에 entity가 있으며 */
	    !bfq_gt(last_idle->finish, st->vtime)) { /* 가장 늦은 idle entity조차 이미 vtime을 넘지 못했다면(모두 지나간 상태) */
		/*
		 * Forget the whole idle tree, increasing the vtime past
		 * the last finish time of idle entities.
		 */
		st->vtime = last_idle->finish; /* vtime을 마지막 idle entity의 finish까지 앞당겨 정지된 시계를 재가동 */
	}

	if (first_idle && !bfq_gt(first_idle->finish, st->vtime)) /* 가장 빠른 idle entity의 finish가 이미 (갱신된) vtime을 넘지 못했다면 */
		bfq_put_idle_entity(st, first_idle); /* 그 entity 하나만 idle tree에서 완전히 제거(점진적 청소) */
}

struct bfq_service_tree *bfq_entity_service_tree(struct bfq_entity *entity)
{
	/*
	 * [한국어]
	 * bfq_entity_service_tree - entity->sched_data->service_tree[] 배열에서
	 *   entity의 ioprio_class에 맞는 슬롯을 찾아 반환한다.
	 *
	 * @entity: 조회할 entity.
	 * @return: entity가 속해야 할 &sched_data->service_tree[idx] 포인터.
	 *
	 * bfq_class_idx()로 RT/BE/IDLE 중 어느 배열 인덱스인지 계산한 뒤,
	 * entity->sched_data(entity가 스케줄링되는 계층 노드)의
	 * service_tree 배열에서 그 인덱스의 슬롯을 골라 반환하는 단순
	 * 인덱싱 헬퍼다. entity가 활성화/비활성화/재큐잉될 때마다 "지금
	 * 어느 rb-tree 쌍(active/idle)을 조작해야 하는지"를 결정하는
	 * 진입점 역할을 한다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_activate_requeue_entity()/__bfq_deactivate_entity()/
	 *   __bfq_requeue_entity() 등 -> [bfq_entity_service_tree] ->
	 *   bfq_class_idx()
	 */
	struct bfq_sched_data *sched_data = entity->sched_data; /* entity가 스케줄링되는 계층 노드(3개 클래스별 트리를 보유) */
	unsigned int idx = bfq_class_idx(entity); /* entity의 ioprio_class에 해당하는 배열 인덱스(0=RT/1=BE/2=IDLE) */

	return sched_data->service_tree + idx; /* 배열 포인터 산술로 해당 클래스의 bfq_service_tree 슬롯 주소를 계산해 반환 */
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
 * [한국어]
 * __bfq_entity_update_weight_prio - entity->prio_changed로 표시된
 *   weight/ioprio/ioprio_class 변경 요청을 실제로 반영하고, 클래스가
 *   바뀌었다면 새 service_tree로 옮긴다.
 *
 * @old_st: entity가 현재(호출 시점) 속해 있던 bfq_service_tree.
 * @entity: 갱신 대상 entity.
 * @update_class_too: true이면 ioprio_class 변경까지 함께 반영한다.
 *   entity가 이미 어떤 트리 위에 있을 수 있는 시점에는 반드시 false로
 *   호출해야 한다 - 클래스가 바뀌면 소속 service_tree 배열 인덱스
 *   자체가 달라져, entity가 여전히 "옛 클래스의 트리"에 물리적으로
 *   남아있는 채로 "새 클래스의 트리"를 기준으로 연산하면 트리 정합성이
 *   깨지기 때문이다(원본 주석 참고).
 * @return: entity가 최종적으로 속하게 된 새 bfq_service_tree(클래스가
 *   바뀌지 않았으면 old_st와 동일, 바뀌었으면 새 클래스의 트리).
 *
 * "지연 적용(lazy update)" 패턴의 실제 실행 지점이다: ioprio/weight
 * 변경 요청은 즉시 반영되지 않고 entity->new_weight/prio_changed에
 * 잠시 보관되었다가(bfq-iosched.c의 설정 경로), entity가 안전한
 * 시점(주로 idle에서 active로 전이하는 순간)에 이 함수가 호출되어야
 * 실제 weight/ioprio_class가 바뀐다. 처리 순서는 다음과 같다:
 * (1) smp_rmb()로 bfq_group_set_weight()의 smp_wmb()와 짝을 맞춰,
 *     다른 CPU가 먼저 기록한 new_weight 등의 값이 이 CPU에서도
 *     올바른 순서로 보이도록 메모리 배리어를 세운다.
 * (2) old_st->wsum에서 기존 weight를 먼저 빼둔다(나중에 새 weight를
 *     다시 더할 것이므로 잠시 total에서 이 entity 몫을 제외).
 * (3) new_weight가 orig_weight와 다르면(실제 변경 요청이 있으면) 범위를
 *     [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT]로 클리핑하고(벗어나면 pr_crit()로
 *     커널 로그에 경고), orig_weight를 갱신하며 leaf라면 표시용
 *     ioprio도 역산해 갱신한다.
 * (4) update_class_too이고 leaf라면 ioprio_class도 실제로 갱신하고,
 *     클래스 변경이 완전히 끝났다면 prio_changed 플래그를 0으로
 *     클리어한다.
 * (5) bfq_entity_service_tree()로 (클래스가 바뀌었을 수 있는) 새
 *     service_tree를 다시 조회한다.
 * (6) weight-raising 배율(wr_coeff)까지 곱한 최종 new_weight를 계산하고,
 *     실제 weight 값이 바뀌었다면 bfq_weights_tree_remove/add()로
 *     장치 전역 weight 카운터 트리(서로 다른 weight를 가진 큐가
 *     섞여 있는지를 추적하는 자료구조)를 갱신한다.
 * (7) 새 service_tree의 wsum에 새 weight를 더하고, 클래스가 실제로
 *     바뀌었다면(new_st != old_st) entity->start를 새 트리의 현재
 *     vtime으로 리셋한다(새 클래스 경쟁에 "지금부터" 참가하는 것으로
 *     취급).
 * 원본 주석대로, 이 함수가 너무 이른 시점에 weight를 바꿔 약간의
 * 불공정성을 유발할 수 있음이 알려진 한계로 남아 있다(정확히 하려면
 * entity->finish <= old_st->vtime인 시점까지 미뤄야 함). bfqd->lock
 * 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_fin_time_enqueue() -> [__bfq_entity_update_weight_prio]
 *   -> bfq_entity_service_tree()/bfq_weight_to_ioprio()/
 *   bfq_weights_tree_remove()/bfq_weights_tree_add()
 */
struct bfq_service_tree *
__bfq_entity_update_weight_prio(struct bfq_service_tree *old_st,
				struct bfq_entity *entity,
				bool update_class_too)
{
	struct bfq_service_tree *new_st = old_st; /* 기본값: 클래스가 바뀌지 않으면 기존 트리를 그대로 반환 */

	if (entity->prio_changed) { /* weight/ioprio/ioprio_class 중 하나라도 변경이 대기 중이면 */
		struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 ioprio 필드까지 갱신할 대상 bfq_queue */
		unsigned int prev_weight, new_weight; /* weight 변경 전/후 값을 비교해 카운터 트리 갱신 여부를 판단하기 위한 임시 변수 */

		/* Matches the smp_wmb() in bfq_group_set_weight. */
		smp_rmb(); /* [한국어] bfq_group_set_weight()가 new_weight 등을 기록한 뒤 실행한 smp_wmb()와 짝을 이뤄, 그 값들이 이 CPU에서도 올바른 순서로 보이도록 보장 */
		old_st->wsum -= entity->weight; /* 기존 weight를 옛 트리의 총합에서 먼저 제외(변경 처리 도중 이중 계산 방지) */

		if (entity->new_weight != entity->orig_weight) { /* 실제로 weight 값 자체가 바뀌도록 요청된 경우 */
			if (entity->new_weight < BFQ_MIN_WEIGHT || /* 요청된 값이 허용 최소치보다 작거나 */
			    entity->new_weight > BFQ_MAX_WEIGHT) { /* 허용 최대치보다 크면(비정상 설정) */
				pr_crit("update_weight_prio: new_weight %d\n", /* 커널 로그에 심각도 crit으로 잘못된 값 경고 */
					entity->new_weight);
				if (entity->new_weight < BFQ_MIN_WEIGHT) /* 하한을 벗어났다면 */
					entity->new_weight = BFQ_MIN_WEIGHT; /* 최소 허용치로 강제 클리핑 */
				else /* 상한을 벗어났다면 */
					entity->new_weight = BFQ_MAX_WEIGHT; /* 최대 허용치로 강제 클리핑 */
			}
			entity->orig_weight = entity->new_weight; /* 검증된 새 값을 "원본(WR 적용 전) weight"로 확정 */
			if (bfqq) /* leaf라면 */
				bfqq->ioprio = /* 사용자에게 보여줄 ioprio 필드도 새 weight 기준으로 역산해 갱신 */
				  bfq_weight_to_ioprio(entity->orig_weight);
		}

		if (bfqq && update_class_too) /* leaf이고 호출자가 클래스 변경까지 허용한(entity가 트리 밖에 있는) 시점이면 */
			bfqq->ioprio_class = bfqq->new_ioprio_class; /* 대기 중이던 새 ioprio_class를 실제로 반영 */

		/*
		 * Reset prio_changed only if the ioprio_class change
		 * is not pending any longer.
		 */
		if (!bfqq || bfqq->ioprio_class == bfqq->new_ioprio_class) /* non-leaf이거나, leaf인데 클래스 변경이 이미 완료됐다면 */
			entity->prio_changed = 0; /* 더 이상 대기 중인 변경이 없으므로 플래그 클리어 */

		/*
		 * NOTE: here we may be changing the weight too early,
		 * this will cause unfairness.  The correct approach
		 * would have required additional complexity to defer
		 * weight changes to the proper time instants (i.e.,
		 * when entity->finish <= old_st->vtime).
		 */
		new_st = bfq_entity_service_tree(entity); /* (클래스가 바뀌었을 수 있으니) entity의 새 ioprio_class 기준으로 service_tree를 다시 조회 */

		prev_weight = entity->weight; /* 실제 적용 중이던(WR 배율까지 반영된) 이전 weight 값을 저장 */
		new_weight = entity->orig_weight * /* 검증된 원본 weight에 */
			     (bfqq ? bfqq->wr_coeff : 1); /* leaf면 weight-raising 배율(wr_coeff)을 곱하고, non-leaf(그룹)면 배율 1을 적용 */
		/*
		 * If the weight of the entity changes, and the entity is a
		 * queue, remove the entity from its old weight counter (if
		 * there is a counter associated with the entity).
		 */
		if (prev_weight != new_weight && bfqq) /* 실제 weight 수치가 바뀌었고 leaf라면 */
			bfq_weights_tree_remove(bfqq); /* 장치 전역 weight 카운터 트리에서 옛 weight에 대한 등록을 제거 */
		entity->weight = new_weight; /* 최종 확정된 weight를 entity에 반영: 이후 F_i 계산에 바로 쓰임 */
		/*
		 * Add the entity, if it is not a weight-raised queue,
		 * to the counter associated with its new weight.
		 */
		if (prev_weight != new_weight && bfqq && bfqq->wr_coeff == 1) /* weight가 바뀌었고 leaf이며 WR 중이 아니라면(순수 weight 변경) */
			bfq_weights_tree_add(bfqq); /* 새 weight로 카운터 트리에 다시 등록 */

		new_st->wsum += entity->weight; /* 새(또는 그대로인) 트리의 총 weight 합에 최종 weight를 더해 반영 */

		if (new_st != old_st) /* 클래스가 실제로 바뀌어 트리 자체가 달라졌다면 */
			entity->start = new_st->vtime; /* 새 트리의 현재 vtime을 시작점으로 삼아 "지금부터" 그 클래스에서 경쟁 시작 */
	}

	return new_st; /* entity가 최종적으로 속한 service_tree(클래스 불변이면 old_st와 동일) */
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
 * [한국어]
 * bfq_bfqq_served - bfqq가 실제로 소비한 서비스량(served)을 leaf부터
 *   루트까지 모든 상위 entity의 service 필드에 반영하고, 각 레벨의
 *   vtime을 전진시킨다.
 *
 * @bfqq: 서비스를 받은 bfq_queue.
 * @served: 이번에 실제로 소비된 서비스량(섹터 수 등).
 * @return: 없음(void).
 *
 * request가 디스패치되거나 완료될 때 호출되어, B-WF2Q+의 "실제로 얼마나
 * 썼는가"라는 회계(accounting)를 갱신하는 함수다. 먼저 이 큐가 아직
 * backlogged 상태에서 서비스를 받은 적이 없다면(service_from_backlogged
 * == 0) first_IO_time을 지금 시각(jiffies)으로 기록해 "이 backlogged
 * 구간에서 처음 서비스된 시각"을 추적한다(interactive 큐 판별 등에
 * 활용). weight-raised 상태(wr_coeff > 1)라면 service_from_wr에도
 * 누적해 WR 기간 동안 얼마나 서비스를 받았는지 별도로 추적한다(WR
 * 유지 시간 판단에 사용). 그 뒤 for_each_entity() 루프로 leaf에서
 * 루트까지 모든 상위 entity를 순회하며: 각 레벨의 service_tree를
 * 얻고, entity->service에 served를 누적하며(이번 서비스 슬롯에서
 * 실제로 쓴 양), st->vtime에 bfq_delta(served, st->wsum)만큼 전진시킨다
 * (그 레벨 전체 weight 합으로 정규화된 만큼 가상 시간이 흐름), 마지막으로
 * bfq_forget_idle()을 호출해 vtime 전진으로 인해 만료된 idle entity가
 * 있으면 정리한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_dispatch_request()/bfq_completed_request()(bfq-iosched.c) ->
 *   [bfq_bfqq_served] -> bfq_entity_service_tree()/bfq_delta()/
 *   bfq_forget_idle()
 */
void bfq_bfqq_served(struct bfq_queue *bfqq, int served)
{
	struct bfq_entity *entity = &bfqq->entity; /* 서비스를 받은 큐를 감싸는 leaf entity(루프의 시작점) */
	struct bfq_service_tree *st; /* 순회 중인 레벨의 bfq_service_tree(루프 안에서 매번 갱신) */

	if (!bfqq->service_from_backlogged) /* 이번 backlogged 구간에서 아직 한 번도 서비스받은 적이 없다면 */
		bfqq->first_IO_time = jiffies; /* "처음 서비스된 시각"을 지금으로 기록(interactive 판별 등에 사용) */

	if (bfqq->wr_coeff > 1) /* weight-raising이 적용 중인(latency 우선) 큐라면 */
		bfqq->service_from_wr += served; /* WR 기간 중 누적 서비스량을 추적(WR 종료 조건 판단에 쓰임) */

	bfqq->service_from_backlogged += served; /* backlogged로 전이한 이후 누적 서비스량 갱신(통계/휴리스틱용) */
	for_each_entity(entity) { /* leaf(bfqq->entity)에서 시작해 parent 포인터를 따라 루트(root_group)까지 순회 */
		st = bfq_entity_service_tree(entity); /* 이 레벨(entity)이 속한 ioprio_class의 service_tree 획득 */

		entity->service += served; /* 이번 서비스 슬롯에서 이 레벨이 실제로 소비한 서비스량 누적 */

		st->vtime += bfq_delta(served, st->wsum); /* 이 레벨 전체 weight 합(wsum)으로 정규화한 만큼 가상 시간을 전진 */
		bfq_forget_idle(st); /* vtime 전진으로 인해 이제 정리 가능해진 idle entity가 있으면 정리 */
	}
	bfq_log_bfqq(bfqq->bfqd, bfqq, "bfqq_served %d secs", served); /* 디버그 트레이스: 이번에 얼마나 서비스됐는지 기록 */
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
 * [한국어]
 * bfq_bfqq_charge_time - 예산(budget)을 느리게 소모하는 큐에게, 실제
 *   전송량 대신 "시간(time_ms) 기준으로 환산한 가상 서비스량"을
 *   과금한다.
 *
 * @bfqd: 디바이스 전역 상태(peak_rate 추정에 쓰이는 bfq_max_budget 등을
 *   제공).
 * @bfqq: 과금 대상 큐.
 * @time_ms: 이 큐가 서비스 중이었던(그러나 느리게 진행된) 시간(밀리초).
 * @return: 없음(void).
 *
 * B-WF2Q+ 엔진은 본래 "서비스량(섹터 수)" 도메인에서 동작하는데, 예산을
 * 다 쓰는 데 시간이 오래 걸리는(느린) 큐를 서비스량 기준으로만
 * 공정하게 대하면 처리량(throughput)이 손해를 볼 수 있다(원본 주석
 * 참고). 이를 보완하기 위해, 그런 큐에게는 "만약 최고 성능(peak_rate)
 * 으로 그 시간 동안 처리했다면 냈을 법한 가상의 서비스량"을 실제
 * 서비스량 대신 과금해, 서비스가 아닌 시간 기준의 공정성을 흉내낸다.
 * time_ms를 bfq_timeout(예산 소진 강제 타임아웃) 이내로 자른
 * bounded_time_ms를 구하고, bfqd->bfq_max_budget에 비례시켜 "그 시간
 * 동안 최고 속도로 처리했다면 냈을 서비스량"(serv_to_charge_for_time)을
 * 계산한 뒤, 실제로 이미 기록된 entity->service보다 작지 않도록
 * (max) 보정해 tot_serv_to_charge를 확정한다. 이 값이 현재 budget보다
 * 크면 budget 자체도 늘려(불일치 방지) 이후 F_i 계산이 이 큰 값을
 * 기준으로 하도록 맞춘다. 마지막으로 bfq_bfqq_served()를 호출해,
 * "지금까지 이미 반영된 service"를 뺀 나머지(추가로 과금해야 할 양)만큼
 * 실제로 서비스된 것처럼 vtime을 전진시킨다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_bfqq_expire()(BFQQE_TOO_IDLE 등, bfq-iosched.c) ->
 *   [bfq_bfqq_charge_time] -> bfq_bfqq_served()
 */
void bfq_bfqq_charge_time(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  unsigned long time_ms)
{
	struct bfq_entity *entity = &bfqq->entity; /* 과금 대상 큐를 감싸는 leaf entity */
	unsigned long timeout_ms = jiffies_to_msecs(bfq_timeout); /* 예산 소진 강제 타임아웃(bfq_timeout, jiffies)을 ms 단위로 변환 */
	unsigned long bounded_time_ms = min(time_ms, timeout_ms); /* 과금 시간이 타임아웃을 넘지 않도록 상한을 둠(과도한 과금 방지) */
	int serv_to_charge_for_time = /* "그 시간 동안 최고 속도(peak)로 처리했다면 냈을" 가상 서비스량 */
		(bfqd->bfq_max_budget * bounded_time_ms) / timeout_ms; /* 최대 budget을 (경과시간/타임아웃) 비율로 스케일링 */
	int tot_serv_to_charge = max(serv_to_charge_for_time, entity->service); /* 이미 실제로 기록된 service보다 작게 과금하지 않도록 보정 */

	/* Increase budget to avoid inconsistencies */
	if (tot_serv_to_charge > entity->budget) /* 과금할 양이 현재 budget보다 크면(F_i 계산과 불일치 우려) */
		entity->budget = tot_serv_to_charge; /* budget 자체를 과금량만큼 늘려 이후 계산의 일관성을 확보 */

	bfq_bfqq_served(bfqq, /* 실제 서비스된 것처럼 처리해 vtime을 전진시키는 공통 경로 재사용 */
			max_t(int, 0, tot_serv_to_charge - entity->service)); /* 이미 반영된 service를 뺀 "추가로 과금할" 순증분만 전달(음수 방지) */
}

/*
 * [한국어]
 * bfq_update_fin_time_enqueue - weight/priority 지연 적용을 마무리하고
 *   F_i를 계산한 뒤, 필요하면 backshift 보정을 적용해 active tree에
 *   entity를 삽입한다.
 *
 * @entity: 삽입할 entity(호출 시점에 어떤 서비스 트리에도 속해 있지
 *   않아야 함 - 원본 주석 참고).
 * @st: entity가 삽입될 bfq_service_tree.
 * @backshifted: true이면 entity->start/finish가 non_blocking_wait_rq
 *   경로에서 이미 vtime보다 과거로 설정된("backshift"된) 상태임을
 *   의미하며, 이 경우 과도한 이득을 완화하는 보정이 필요할 수 있다.
 * @return: 없음(void).
 *
 * 이 함수는 두 활성화 경로(__bfq_activate_entity()의 진짜 활성화,
 * __bfq_requeue_entity()의 재큐잉/재배치) 양쪽에서 공통으로 쓰이는
 * "타임스탬프 확정 + 트리 삽입" 마무리 단계다. 먼저
 * __bfq_entity_update_weight_prio()를 호출해 대기 중이던 weight/ioprio
 * 변경을 실제로 반영하고(entity가 지금 트리 밖에 있으므로 이 시점이
 * 안전함), 이어서 bfq_calc_finish()로 F_i = S_i + budget/weight를
 * 계산한다. 그 다음 원본 주석에 상세히 설명된 backshift 보정을
 * 적용한다: 만약 backshifted가 true이고(non_blocking_wait_rq로 인해
 * entity->start/finish가 이미 vtime보다 과거로 밀려 있고) 그 finish가
 * 여전히 현재 vtime보다 과거라면(bfq_gt(vtime, finish)), 그 차이
 * (delta = vtime - finish)만큼 start와 finish를 함께 밀어올려 finish를
 * 현재 vtime과 같게 만든다 - 그렇지 않으면 오래 idle이었다가 복귀한
 * 큐가 지나치게 유리한(작은) finish를 계속 유지해 다른 큐들을
 * 장시간 독점(monopolize)할 수 있기 때문이다. 다만 이 큐가
 * weight-raised(wr_coeff > 1) 상태라면 delta를 wr_coeff로 나눠 덜
 * 밀어올림으로써, latency에 민감한 WR 큐가 backshift 보정으로 인해
 * 비WR 큐보다 오히려 불리해지는 것을 방지한다. 마지막으로
 * bfq_active_insert()로 최종 확정된 타임스탬프를 가진 entity를 active
 * tree에 실제로 삽입한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_activate_entity()/__bfq_requeue_entity() ->
 *   [bfq_update_fin_time_enqueue] -> __bfq_entity_update_weight_prio()/
 *   bfq_calc_finish()/bfq_gt()/bfq_active_insert()
 */
static void bfq_update_fin_time_enqueue(struct bfq_entity *entity,
					struct bfq_service_tree *st,
					bool backshifted)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity); /* leaf라면 backshift 보정 시 wr_coeff를 참조하기 위해 얻어둠 */

	/*
	 * When this function is invoked, entity is not in any service
	 * tree, then it is safe to invoke next function with the last
	 * parameter set (see the comments on the function).
	 */
	st = __bfq_entity_update_weight_prio(st, entity, true); /* 대기 중인 weight/ioprio_class 변경을 반영하고, 클래스가 바뀌었으면 새 service_tree를 돌려받음 */
	bfq_calc_finish(entity, entity->budget); /* budget을 서비스량으로 삼아 F_i = S_i + budget/weight를 계산 */

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
	if (backshifted && bfq_gt(st->vtime, entity->finish)) { /* backshift된 상태이면서 그 finish가 여전히 현재 vtime보다 과거라면(과도한 이득 우려) */
		unsigned long delta = st->vtime - entity->finish; /* 현재 vtime과 finish의 차이만큼을 보정량으로 계산 */

		if (bfqq) /* leaf(latency-sensitive 여부를 판단할 수 있는 실제 큐)라면 */
			delta /= bfqq->wr_coeff; /* weight-raised 큐는 보정을 wr_coeff분의 1로 줄여 latency 이점을 덜 훼손 */

		entity->start += delta; /* start를 delta만큼 뒤(미래)로 밀어 */
		entity->finish += delta; /* finish도 함께 밀어 현재 vtime과 같아지도록 맞춤(과도한 독점 방지) */
	}

	bfq_active_insert(st, entity); /* 최종 확정된 F_i를 기준으로 entity를 active tree에 삽입 */
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
 * [한국어]
 * __bfq_activate_entity - 완전히 비활성 상태였던(또는 idle tree에
 *   있던) entity를 "진짜 활성화"한다: 새 타임스탬프를 계산해 active
 *   tree에 삽입한다.
 *
 * @entity: 활성화할 entity(비활성 상태이거나 idle tree에 있어야 함 -
 *   이미 active하거나 서비스 중인 경우는 __bfq_requeue_entity()가
 *   대신 처리).
 * @non_blocking_wait_rq: true이면 이 entity(주로 leaf bfq_queue)가
 *   방금 새 request를 기다리던 중이었음을 의미하며, seeky/latency
 *   민감 큐를 위한 backshift 최적화 여부를 결정한다.
 * @return: 없음(void).
 *
 * "진짜(true) 활성화"란 entity가 완전히 idle/비활성 상태였다가 그
 * 하위(자식)에 새 request가 도착해 처음으로 active해지는 경우를
 * 말한다(재큐잉/재배치와 구분됨 - 원본 주석 참고). 먼저 entity가
 * 속한 service_tree를 얻고, non_blocking_wait_rq이면서 현재 vtime이
 * 이미 entity의 (과거) finish를 넘어섰다면 backshifted 플래그를
 * 세우고 min_vstart를 그 finish로 고정한다 - 이는 request를 기다리며
 * 잠시 쉬었던(비-seeky) 성격의 큐가 복귀할 때 지나치게 불리한(현재
 * vtime 기준의 큰) start를 받지 않도록, 옛 finish 근처의 유리한
 * 위치에서 다시 시작하게 해주는 latency 최적화다. 그렇지 않다면
 * min_vstart는 그냥 현재 vtime이 된다. 그 다음 entity가 idle tree에
 * 있었다면(finish가 아직 vtime을 넘지 않아 완전히 잊히지 않은 채
 * 남아있던 경우) bfq_idle_extract()로 꺼낸 뒤, start를 "min_vstart와
 * 옛 finish 중 더 큰 쪽"으로 설정한다(공정하게 더 늦은 시점부터
 * 재시작). idle tree에 없었다면(완전히 새로 활성화되는 경우) start를
 * 그냥 min_vstart로 설정하고, service_tree의 wsum에 weight를 더하며,
 * bfq_get_entity()로 서비스 참조를 얻고 on_st_or_in_serv를 true로
 * 표시한다. 마지막으로 bfq_update_fin_time_enqueue()를 호출해 F_i를
 * 계산하고 실제로 active tree에 삽입한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_activate_requeue_entity() -> [__bfq_activate_entity] ->
 *   bfq_entity_service_tree()/bfq_gt()/bfq_idle_extract()/
 *   bfq_get_entity()/bfq_update_fin_time_enqueue()
 */
static void __bfq_activate_entity(struct bfq_entity *entity,
				  bool non_blocking_wait_rq)
{
	struct bfq_service_tree *st = bfq_entity_service_tree(entity); /* entity의 ioprio_class에 해당하는 service_tree */
	bool backshifted = false; /* 아래에서 비-seeky 최적화가 적용됐는지 여부, bfq_update_fin_time_enqueue()로 전달됨 */
	unsigned long long min_vstart; /* 새 start의 하한으로 쓸 가상 시각(vtime 또는 옛 finish) */

	/* See comments on bfq_fqq_update_budg_for_activation */
	if (non_blocking_wait_rq && bfq_gt(st->vtime, entity->finish)) { /* request 대기 중이었고 vtime이 이미 옛 finish를 지났다면(latency 최적화 대상) */
		backshifted = true; /* 이후 bfq_update_fin_time_enqueue()가 과도한 이득을 보정하도록 표시 */
		min_vstart = entity->finish; /* 옛 finish 근처의 유리한 위치를 새 start의 하한으로 사용 */
	} else /* 그 외의 경우(대기 중이 아니었거나 아직 vtime이 옛 finish를 넘지 않음) */
		min_vstart = st->vtime; /* 현재 vtime을 새 start의 하한으로 사용(일반적인 활성화) */

	if (entity->tree == &st->idle) { /* entity가 idle tree에 남아 있던 경우(finish가 아직 vtime을 넘지 않아 완전히 잊히지 않았던 상태) */
		/*
		 * Must be on the idle tree, bfq_idle_extract() will
		 * check for that.
		 */
		bfq_idle_extract(st, entity); /* idle tree/idle_list에서 꺼내옴 */
		entity->start = bfq_gt(min_vstart, entity->finish) ? /* min_vstart가 옛 finish보다 크면(더 늦은 시점이면) */
			min_vstart : entity->finish; /* min_vstart를, 아니면(옛 finish가 더 크거나 같으면) 옛 finish를 새 start로 채택 - 더 늦은(공정한) 쪽 선택 */
	} else { /* entity가 idle tree에도 없던 경우(완전히 새로 활성화되는 경우) */
		/*
		 * The finish time of the entity may be invalid, and
		 * it is in the past for sure, otherwise the queue
		 * would have been on the idle tree.
		 */
		entity->start = min_vstart; /* 새 start를 하한값(vtime 또는 finish)으로 그대로 설정 */
		st->wsum += entity->weight; /* 이 service_tree의 총 weight 합에 이 entity의 weight를 더함 */
		/*
		 * entity is about to be inserted into a service tree,
		 * and then set in service: get a reference to make
		 * sure entity does not disappear until it is no
		 * longer in service or scheduled for service.
		 */
		bfq_get_entity(entity); /* leaf라면 서비스 참조 카운트를 얻어 조기 해제 방지 */

		entity->on_st_or_in_serv = true; /* "이제 트리 위에 있거나 곧 서비스될 것"임을 표시 */
	}

	bfq_update_fin_time_enqueue(entity, st, backshifted); /* F_i를 확정 계산하고(필요시 backshift 보정 후) active tree에 실제 삽입 */
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
 * [한국어]
 * __bfq_requeue_entity - 서비스가 끝났거나(만료) 하위 next_in_service가
 *   바뀌어 재배치가 필요한 entity의 타임스탬프를 갱신하고 active
 *   tree에 다시(또는 새 위치에) 삽입한다.
 *
 * @entity: 재큐잉/재배치할 entity.
 * @return: 없음(void).
 *
 * 두 가지 경우를 구분해 처리한다(원본 주석 참고). (1) entity가 지금
 * 그 sched_data의 in_service_entity와 같다면 - 즉 방금까지 서비스
 * 중이었다면(leaf의 만료이거나, 그룹 entity의 budget이 자식 변화로
 * 바뀌어 재계산이 필요한 경우) - 먼저 bfq_calc_finish(entity,
 * entity->service)로 "실제로 받은 서비스량"을 기준으로 finish를
 * 계산하고, entity->start를 그 finish로 이동시킨다(서비스받은 만큼
 * 시작점을 앞으로 전진). 이 entity가 활성화될 때 자식이 둘 이상이라
 * active tree에서 추출되지 않은 채로 서비스됐을 수 있으므로(그룹
 * entity의 경우), entity->tree가 아직 설정돼 있다면(즉 여전히 트리
 * 위에 있다면) 재배치를 위해 bfq_active_extract()로 일단 뽑아낸다.
 * (2) entity가 in-service가 아니라 이미 active 상태였다면 - 하위의
 * next_in_service가 바뀌어 이 entity(그룹)의 budget이 바뀐 경우이므로
 * -  finish 재계산이 필요해 위치가 바뀔 수 있다는 점은 (1)과 같으며,
 * 여기서는 서비스받은 적이 없으므로 바로 bfq_active_extract()로 뽑아낸다.
 * 두 경우 모두 마지막으로 bfq_update_fin_time_enqueue(entity, st,
 * false)를 호출해(backshifted는 항상 false - 재큐잉은 backshift 최적화
 * 대상이 아님) 새 F_i를 계산하고 active tree에 다시 삽입한다.
 * bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_activate_requeue_entity()/bfq_deactivate_entity() ->
 *   [__bfq_requeue_entity] -> bfq_entity_service_tree()/
 *   bfq_calc_finish()/bfq_active_extract()/bfq_update_fin_time_enqueue()
 */
static void __bfq_requeue_entity(struct bfq_entity *entity)
{
	struct bfq_sched_data *sd = entity->sched_data; /* entity가 스케줄링되는 계층 노드(in_service_entity 비교에 사용) */
	struct bfq_service_tree *st = bfq_entity_service_tree(entity); /* entity의 ioprio_class에 해당하는 service_tree */

	if (entity == sd->in_service_entity) { /* 방금까지 이 entity가 서비스 중이었다면(leaf 만료 또는 그룹 budget 변경) */
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
		bfq_calc_finish(entity, entity->service); /* 실제로 받은 서비스량(entity->service)을 기준으로 임시 finish를 계산 */
		entity->start = entity->finish; /* 받은 서비스만큼 start를 전진시켜, "서비스받은 뒤의 시점"부터 다시 경쟁하도록 함 */
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
		if (entity->tree) /* 자식이 둘 이상이라 active tree에서 추출되지 않은 채 서비스됐던 경우(여전히 트리 위에 있음) */
			bfq_active_extract(st, entity); /* 위치 재조정을 위해 일단 트리에서 뽑아냄 */
	} else { /* The entity is already active, and not in service */ /* 서비스 중은 아니었지만 이미 active 상태(하위 next_in_service 변경으로 인한 재배치) */
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
		bfq_active_extract(st, entity); /* budget 변경으로 위치가 바뀔 수 있으므로 일단 트리에서 뽑아냄 */
	}

	bfq_update_fin_time_enqueue(entity, st, false); /* 새 F_i를 계산하고(backshift 없이) active tree에 다시 삽입 */
}

static void __bfq_activate_requeue_entity(struct bfq_entity *entity,
					  bool non_blocking_wait_rq)
{
	/*
	 * [한국어]
	 * __bfq_activate_requeue_entity - entity의 현재 상태를 보고
	 *   "진짜 활성화"와 "재큐잉/재배치" 중 어느 경로로 처리할지
	 *   분기하는 라우터 함수.
	 *
	 * @entity: 대상 entity.
	 * @non_blocking_wait_rq: 진짜 활성화 경로로 갈 경우
	 *   __bfq_activate_entity()에 그대로 전달될 latency 최적화 힌트.
	 * @return: 없음(void).
	 *
	 * entity가 지금 서비스 중이거나(sched_data->in_service_entity ==
	 * entity) 이미 active tree 위에 있다면(entity->tree == &st->active)
	 * __bfq_requeue_entity()로 재큐잉/재배치를 수행한다. 그렇지 않다면
	 * (서비스 중도 아니고 active tree에도 없다면 = 완전히 idle이었다가
	 * 지금 처음 활성화되는 것) __bfq_activate_entity()로 진짜 활성화를
	 * 수행한다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_activate_requeue_entity() -> [__bfq_activate_requeue_entity]
	 *   -> __bfq_requeue_entity() 또는 __bfq_activate_entity()
	 */
	struct bfq_service_tree *st = bfq_entity_service_tree(entity); /* entity의 ioprio_class에 해당하는 service_tree(active tree 소속 판정에 사용) */

	if (entity->sched_data->in_service_entity == entity || /* entity가 지금 이 레벨에서 서비스 중이거나 */
	    entity->tree == &st->active) /* 이미 active tree 위에 있다면(둘 중 하나만 참이어도) */
		 /*
		  * in service or already queued on the active tree,
		  * requeue or reposition
		  */
		__bfq_requeue_entity(entity); /* 재큐잉/재배치 경로: 타임스탬프만 갱신하고 트리 내 위치를 조정 */
	else /* 서비스 중도 아니고 active tree에도 없는 경우(완전히 새로 활성화되는 경우) */
		/*
		 * Not in service and not queued on its active tree:
		 * the activity is idle and this is a true activation.
		 */
		__bfq_activate_entity(entity, non_blocking_wait_rq); /* 진짜 활성화 경로: idle tree에서 꺼내거나 완전히 새로 활성화 */
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
 * [한국어]
 * bfq_activate_requeue_entity - leaf entity 하나를 활성화/재큐잉하고,
 *   그로 인해 함께 갱신이 필요해지는 모든 조상(부모 그룹) entity까지
 *   for_each_entity()로 루트까지 연쇄 처리한다.
 *
 * @entity: 활성화/재큐잉을 시작할 leaf entity.
 * @non_blocking_wait_rq: __bfq_activate_entity()에 전달될 latency
 *   최적화 힌트.
 * @requeue: true이면 이 호출이 재큐잉(bfqq가 만료되는 경로)임을 의미해,
 *   budget/next_in_service 변경 여부와 무관하게 모든 조상을 강제로
 *   재큐잉해야 함(만료로 인해 모든 조상이 서비스를 멈추기 때문).
 * @expiration: true이면 in-service 큐의 만료 경로에서 호출된 것으로,
 *   bfq_update_next_in_service()에 그대로 전달되어 in_service_entity
 *   해석 방식에 영향을 준다.
 * @return: 없음(void).
 *
 * for_each_entity(entity) 루프로 leaf에서 시작해 parent 포인터를 따라
 * 루트(root_group)까지 올라가며, 매 레벨마다
 * __bfq_activate_requeue_entity()로 그 레벨의 활성화/재큐잉을 수행한다.
 * 그 뒤 bfq_update_next_in_service()를 호출해 이 레벨의
 * next_in_service를 갱신하고, 그 결과(상위에 영향을 줄 수 있는지)가
 * false이고 requeue도 아니라면 더 이상 상위로 전파할 필요가 없다고
 * 판단해 루프를 중단한다(break) - budget이나 next_in_service가 바뀌지
 * 않았다면 상위 그룹의 스케줄링에도 영향이 없기 때문이다. requeue가
 * true인 경우에는 이 최적화를 적용하지 않고 끝까지(루트까지) 강제로
 * 순회하는데, 이는 만료 경로에서는 "이 레벨에서 next_in_service가
 * 안 바뀌었다"는 사실만으로 상위 조상들의 재큐잉이 불필요하다고
 * 단정할 수 없기 때문이다(원본 주석의 requeue 정의 참고). bfqd->lock
 * 하에서 호출.
 *
 * 호출 체인:
 *   bfq_activate_bfqq()/bfq_requeue_bfqq() ->
 *   [bfq_activate_requeue_entity] -> __bfq_activate_requeue_entity()/
 *   bfq_update_next_in_service()
 */
static void bfq_activate_requeue_entity(struct bfq_entity *entity,
					bool non_blocking_wait_rq,
					bool requeue, bool expiration)
{
	for_each_entity(entity) { /* leaf에서 시작해 parent를 따라 루트(root_group)까지 상향 순회 */
		__bfq_activate_requeue_entity(entity, non_blocking_wait_rq); /* 현재 레벨의 entity를 활성화 또는 재큐잉/재배치 */
		if (!bfq_update_next_in_service(entity->sched_data, entity, /* 이 레벨의 next_in_service를 갱신하고 */
						expiration) && !requeue) /* 상위에 영향이 없고(false) 재큐잉 경로도 아니라면 */
			break; /* 더 이상 전파할 필요가 없으므로 상위 순회를 조기 종료 */
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
 * [한국어]
 * __bfq_deactivate_entity - entity 한 단계를 active tree에서 제거하고
 *   (필요하고 허용되면) idle tree로 옮기거나, 완전히 잊는다.
 *
 * @entity: 비활성화할 entity.
 * @ins_into_idle_tree: true이면 조건이 맞을 때 idle tree로 옮기는 것을
 *   허용, false이면 무조건 완전히 잊는다(idle 보관 금지).
 * @return: true이면 이 entity가 실제로 비활성화 처리됐음(호출자가 상위
 *   전파를 계속해야 함), false이면 entity가 이미 비활성 상태여서
 *   아무 것도 하지 않았음(no-op).
 *
 * entity->on_st_or_in_serv가 false라면 - 즉 애초에 한 번도 활성화된
 * 적이 없거나 이미 비활성 상태라면 - 아무 것도 하지 않고 즉시 false를
 * 반환한다(원본 주석의 no-op 케이스). 그렇지 않다면: 먼저
 * bfq_entity_service_tree()로 소속 service_tree를 얻고, 이 entity가
 * 지금 이 레벨의 in_service_entity인지(is_in_service) 확인한다.
 * bfq_calc_finish(entity, entity->service)로 지금까지 실제로 받은
 * 서비스량을 기준으로 finish를 최종 확정한다. is_in_service라면
 * sd->in_service_entity를 NULL로 리셋하고(더 이상 이 레벨에서 서비스
 * 중인 것이 없음), 그렇지 않다면 entity->service를 0으로 리셋한다
 * (in-service가 아닌 entity의 service 카운터는 다음 만료 시 아무도
 * 정리해주지 않으므로 지금 직접 정리 - 원본 주석 참고). 그 다음
 * entity->tree가 active면 bfq_active_extract()로, (in-service가 아니면서)
 * idle이면 bfq_idle_extract()로 제거한다(in-service였던 entity는 보통
 * 이미 어느 트리에도 없다 - 함수 상단 NOTE 참고). 마지막으로,
 * ins_into_idle_tree가 false이거나 finish가 이미 vtime을 넘지 않았다면
 * (더 기다릴 가치가 없다면) bfq_forget_entity()로 완전히 잊고, 그렇지
 * 않다면(finish가 아직 vtime보다 미래라면) bfq_idle_insert()로 idle
 * tree에 보관해 나중에 빠르게 복귀할 수 있게 한다. bfqd->lock 하에서
 * 호출.
 *
 * 호출 체인:
 *   bfq_deactivate_entity() -> [__bfq_deactivate_entity] ->
 *   bfq_entity_service_tree()/bfq_calc_finish()/bfq_active_extract()/
 *   bfq_idle_extract()/bfq_gt()/bfq_forget_entity()/bfq_idle_insert()
 */
bool __bfq_deactivate_entity(struct bfq_entity *entity, bool ins_into_idle_tree)
{
	struct bfq_sched_data *sd = entity->sched_data; /* entity가 스케줄링되는 계층 노드 */
	struct bfq_service_tree *st; /* entity가 속한(속했던) bfq_service_tree */
	bool is_in_service; /* entity가 지금 이 레벨에서 서비스 중이었는지 여부 */

	if (!entity->on_st_or_in_serv) /* 트리 위에도 없고 서비스 중도 아니라면(이미 비활성 또는 활성화된 적 없음) */
					/*
					 * entity never activated, or
					 * already inactive
					 */
		return false; /* 처리할 것이 없는 no-op이므로 즉시 false 반환 */

	/*
	 * If we get here, then entity is active, which implies that
	 * bfq_group_set_parent has already been invoked for the group
	 * represented by entity. Therefore, the field
	 * entity->sched_data has been set, and we can safely use it.
	 */
	st = bfq_entity_service_tree(entity); /* entity의 ioprio_class에 해당하는 service_tree 획득 */
	is_in_service = entity == sd->in_service_entity; /* 이 entity가 지금 이 레벨의 in_service_entity인지 판정 */

	bfq_calc_finish(entity, entity->service); /* 실제로 받은 서비스량(entity->service)을 기준으로 finish를 최종 확정 */

	if (is_in_service) /* 지금 서비스 중이었다면 */
		sd->in_service_entity = NULL; /* 이 레벨에서 더 이상 서비스 중인 entity가 없음을 표시 */
	else /* 서비스 중이 아니었다면(active 또는 idle 상태) */
		/*
		 * Non in-service entity: nobody will take care of
		 * resetting its service counter on expiration. Do it
		 * now.
		 */
		entity->service = 0; /* in-service가 아닌 entity의 service는 만료 경로에서 아무도 리셋해주지 않으므로 지금 직접 리셋 */

	if (entity->tree == &st->active) /* active tree 위에 있었다면 */
		bfq_active_extract(st, entity); /* active tree에서 제거(min_start 등도 함께 갱신) */
	else if (!is_in_service && entity->tree == &st->idle) /* 서비스 중이 아니면서 idle tree 위에 있었다면 */
		bfq_idle_extract(st, entity); /* idle tree에서 제거 */

	if (!ins_into_idle_tree || !bfq_gt(entity->finish, st->vtime)) /* idle 보관이 금지됐거나, finish가 이미 vtime을 넘지 않았다면(더 기다릴 가치 없음) */
		bfq_forget_entity(st, entity, is_in_service); /* 완전히 잊고 필요하면 서비스 참조까지 반납 */
	else /* idle 보관이 허용되고 finish가 아직 vtime보다 미래라면(다시 돌아올 가능성에 대비) */
		bfq_idle_insert(st, entity); /* idle tree에 보관해 두어 나중에 빠르게 재활성화 가능하게 함 */

	return true; /* 실제로 비활성화 처리를 수행했으므로 호출자가 상위 전파를 계속해야 함 */
}

/**
 * bfq_deactivate_entity - deactivate an entity representing a bfq_queue.
 * @entity: the entity to deactivate.
 * @ins_into_idle_tree: true if the entity can be put into the idle tree
 * @expiration: true if this function is being invoked in the expiration path
 *             of the in-service queue
 */
/*
 * [한국어]
 * bfq_deactivate_entity - leaf entity 하나를 비활성화하고, 필요하면
 *   그로 인해 함께 비활성화되거나 재배치가 필요해지는 조상들까지
 *   상위로 전파 처리하는 최상위 진입점.
 *
 * @entity: 비활성화를 시작할 leaf entity.
 * @ins_into_idle_tree: __bfq_deactivate_entity()에 전달될, idle tree
 *   보관 허용 여부.
 * @expiration: bfq_update_next_in_service()에 그대로 전달될, 만료
 *   경로 여부.
 * @return: 없음(void).
 *
 * 크게 두 단계로 구성된다. 1단계(for_each_entity_safe 루프): leaf에서
 * 시작해 parent를 향해 올라가며(entity가 free될 수도 있으므로 parent를
 * 미리 계산해 두는 안전한 순회 매크로 사용) 각 레벨에서
 * __bfq_deactivate_entity()를 호출한다. 그 결과가 false이면(이미
 * 비활성 상태였던 no-op이면) 더 손댈 것이 없으므로 즉시 return한다.
 * 방금 비활성화한 entity가 그 부모의 next_in_service였다면
 * bfq_update_next_in_service(sd, NULL, expiration)으로 새 후보를
 * 다시 계산한다. 그 결과 부모의 next_in_service나 in_service_entity
 * 중 하나라도 여전히 유효하다면(부모가 아직 backlogged) 더 이상 위로
 * 전파할 필요가 없으므로 break한다. 그렇지 않다면(부모도 완전히
 * backlogged가 아니게 됐다면) ins_into_idle_tree를 true로 강제
 * 설정한 뒤 계속 상위로 올라간다(부모 entity들도 서비스 보장을 위해
 * idle tree에 보관되도록 허용 - 원본 주석 참고). 2단계(break로 멈춘
 * 지점부터 다시 for_each_entity 루프): 1단계가 중간에 멈췄다면
 * (parent가 여전히 active) 그 지점부터 루트까지, 이미 active인
 * entity라도 __bfq_requeue_entity()로 강제 재큐잉/재배치하고
 * bfq_update_next_in_service()로 next_in_service를 다시 계산한다 -
 * 하위에서 entity 하나가 비활성화되면서 형제(sibling)들의 상대적
 * 순서가 바뀌었을 수 있기 때문이다. 이 결과가 변화 없고(false) 만료
 * 경로도 아니라면(!expiration) 더 전파할 필요가 없어 break한다.
 * bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_deactivate_bfqq() -> [bfq_deactivate_entity] ->
 *   __bfq_deactivate_entity()/bfq_update_next_in_service()/
 *   __bfq_requeue_entity()
 */
static void bfq_deactivate_entity(struct bfq_entity *entity,
				  bool ins_into_idle_tree,
				  bool expiration)
{
	struct bfq_sched_data *sd; /* 순회 중인 레벨의 sched_data */
	struct bfq_entity *parent = NULL; /* for_each_entity_safe가 미리 계산해 두는, entity 해제에도 안전한 다음 순회 대상 */

	for_each_entity_safe(entity, parent) { /* leaf에서 루트로, entity가 이 루프 도중 free돼도 안전하게 순회 */
		sd = entity->sched_data; /* 현재 레벨의 sched_data 획득 */

		if (!__bfq_deactivate_entity(entity, ins_into_idle_tree)) { /* 이 레벨의 비활성화가 실질적으로 아무 것도 하지 않은(no-op) 경우 */
			/*
			 * entity is not in any tree any more, so
			 * this deactivation is a no-op, and there is
			 * nothing to change for upper-level entities
			 * (in case of expiration, this can never
			 * happen).
			 */
			return; /* 상위 레벨에 영향이 전혀 없으므로 즉시 함수 종료 */
		}

		if (sd->next_in_service == entity) /* 방금 비활성화한 entity가 이 레벨의 next_in_service 캐시였다면 */
			/*
			 * entity was the next_in_service entity,
			 * then, since entity has just been
			 * deactivated, a new one must be found.
			 */
			bfq_update_next_in_service(sd, NULL, expiration); /* 캐시가 무효화됐으므로 트리를 다시 탐색해 새 후보를 계산 */

		if (sd->next_in_service || sd->in_service_entity) { /* 부모 레벨에 여전히 유효한 후보나 서비스 중인 entity가 있다면(부모가 여전히 backlogged) */
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
			break; /* 부모는 여전히 active하므로 더 이상 "비활성화" 전파는 불필요(단, 재배치는 아래 2단계에서 처리) */
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
		ins_into_idle_tree = true; /* 부모(그룹)도 완전히 backlogged가 아니게 됐으므로, 계속 올라가며 idle 보관을 허용 */
	}

	/*
	 * If the deactivation loop is fully executed, then there are
	 * no more entities to touch and next loop is not executed at
	 * all. Otherwise, requeue remaining entities if they are
	 * about to stop receiving service, or reposition them if this
	 * is not the case.
	 */
	entity = parent; /* 위 루프가 break한 지점(여전히 active한 부모)부터 재배치 루프를 시작 */
	for_each_entity(entity) { /* break 지점에서 루트까지, 남은 조상들의 위치/next_in_service를 재계산 */
		/*
		 * Invoke __bfq_requeue_entity on entity, even if
		 * already active, to requeue/reposition it in the
		 * active tree (because sd->next_in_service has
		 * changed)
		 */
		__bfq_requeue_entity(entity); /* 하위 변화로 인해 이 entity의 finish가 바뀌었을 수 있으므로 강제로 재배치 */

		sd = entity->sched_data; /* 현재 레벨의 sched_data 획득 */
		if (!bfq_update_next_in_service(sd, entity, expiration) && /* 이 레벨의 next_in_service를 재계산했는데 상위 영향이 없고 */
		    !expiration) /* 만료 경로도 아니라면 */
			/*
			 * next_in_service unchanged or not causing
			 * any change in entity->parent->sd, and no
			 * requeueing needed for expiration: stop
			 * here.
			 */
			break; /* 더 이상 상위로 전파할 필요가 없으므로 조기 종료 */
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
 * [한국어]
 * bfq_calc_vtime_jump - active tree에 적어도 하나의 eligible한(start
 *   <= vtime) entity가 존재하도록 하는 데 필요한 vtime 점프 목표값을
 *   계산한다.
 *
 * @st: 계산 대상 bfq_service_tree(active tree가 비어있지 않다고 가정).
 * @return: active tree의 루트 entity가 가진 min_start가 현재 vtime보다
 *   미래라면 그 min_start 값(vtime을 여기까지 점프시켜야 함), 그렇지
 *   않다면(이미 eligible한 entity가 있다면) 현재 vtime 그대로.
 *
 * active tree가 비어있지 않은데도 모든 entity의 start가 아직 현재
 * vtime보다 미래일 수 있다(예: 방금 활성화된 entity들이 backshift
 * 등으로 미래 시점에 배치된 경우). 이런 상황에서 vtime을 그대로 두면
 * eligible한 entity가 하나도 없어 서비스할 대상을 고를 수 없으므로,
 * vtime을 "가장 이른 start를 가진 entity가 eligible해지는 시점"까지
 * 강제로 앞당겨야 한다. bfq_root_active_entity()로 트리 루트의
 * min_start(augmented rb-tree 캐시, 서브트리 전체의 최소 start)를
 * O(1)에 얻어, 그 값이 현재 vtime보다 미래(bfq_gt)라면 그 값을 반환해
 * "여기까지 점프해야 함"을 알린다. 이미 vtime이 그 min_start를
 * 넘었다면(즉 이미 eligible한 entity가 존재한다면) 점프가 필요 없으므로
 * 현재 vtime을 그대로 반환한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   __bfq_lookup_next_entity() -> [bfq_calc_vtime_jump] ->
 *   bfq_root_active_entity()/bfq_gt()
 */
static u64 bfq_calc_vtime_jump(struct bfq_service_tree *st)
{
	struct bfq_entity *root_entity = bfq_root_active_entity(&st->active); /* active tree 루트 entity(그 min_start가 트리 전체 최솟값) */

	if (bfq_gt(root_entity->min_start, st->vtime)) /* 트리 전체에서 가장 이른 start조차 아직 현재 vtime보다 미래라면 */
		return root_entity->min_start; /* 그 지점까지 vtime을 점프시켜야 비로소 eligible한 entity가 생김 */

	return st->vtime; /* 이미 eligible한 entity가 존재하므로(현재 vtime이 충분히 전진해 있으므로) 점프 불필요 */
}

static void bfq_update_vtime(struct bfq_service_tree *st, u64 new_value)
{
	/*
	 * [한국어]
	 * bfq_update_vtime - service_tree의 vtime을 new_value로 전진시키되,
	 *   실제로 전진하는 경우에만 부수효과(idle tree 정리)를 수행한다.
	 *
	 * @st: 갱신 대상 bfq_service_tree.
	 * @new_value: bfq_calc_vtime_jump() 등이 계산한 목표 vtime.
	 * @return: 없음(void).
	 *
	 * vtime은 서비스가 진행되며 계속 증가하기만 하는 논리 시계이므로,
	 * new_value가 현재 vtime보다 실제로 클 때만(역행 금지) 갱신을
	 * 수행한다. 갱신한 경우에는 vtime이 앞으로 튀었으므로 이제 만료
	 * 조건(finish <= vtime)을 만족하게 된 idle entity가 있을 수
	 * 있어, bfq_forget_idle()을 호출해 정리한다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   __bfq_lookup_next_entity() -> [bfq_update_vtime] ->
	 *   bfq_forget_idle()
	 */
	if (new_value > st->vtime) { /* 목표값이 현재 vtime보다 실제로 미래일 때만(vtime은 역행하지 않음) */
		st->vtime = new_value; /* vtime을 목표값까지 전진 */
		bfq_forget_idle(st); /* 전진으로 인해 이제 만료 조건을 만족한 idle entity가 있으면 정리 */
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
 * [한국어]
 * bfq_first_active_entity - active tree에서 "eligible(start <= vtime)
 *   하면서 finish가 가장 작은" entity를 O(log N)에 탐색한다.
 *
 * @st: 탐색 대상 bfq_service_tree.
 * @vtime: eligibility 판정 기준이 되는 가상 시각(보통 bfq_calc_vtime_jump()
 *   등이 계산한 값).
 * @return: 조건을 만족하는 entity, active tree에 eligible한 entity가
 *   전혀 없으면 NULL(이론상 vtime이 bfq_calc_vtime_jump()로 이미
 *   보정됐다면 발생하지 않아야 함).
 *
 * active tree는 finish 순으로 정렬돼 있어 "그냥 가장 왼쪽(가장 작은
 * finish)"을 고르면 빠르지만, 그 entity가 아직 eligible하지 않을(start
 * > vtime, 아직 시작할 수 없는) 수 있다는 문제가 있다. min_start
 * augmented 캐시 덕분에 "이 서브트리에 eligible한 entity가 하나라도
 * 있는가"를 O(1)에 판별할 수 있으므로, 이 함수는 루트에서 시작해
 * "왼쪽 서브트리에 eligible한 entity가 있는 한(min_start <= vtime)
 * 계속 왼쪽으로 내려가고, 왼쪽에 더 없으면 그 시점까지 찾은
 * best(first)를 채택하거나, 아직 못 찾았다면 오른쪽으로 내려가
 * 계속 탐색"하는 방식으로 동작한다: 각 노드에서 entry->start가
 * vtime을 넘지 않으면(eligible) 일단 first 후보로 저장한다(트리 성질상
 * 왼쪽으로 갈수록 finish가 작으므로, 나중에 덮어써지는 후보가 항상 더
 * 낫다). 왼쪽 자식이 있고 그 min_start가 vtime을 넘지 않는다면(왼쪽
 * 서브트리에 더 나은 eligible entity가 있을 수 있음) goto left로
 * 계속 왼쪽 탐색을 이어간다. 왼쪽에 더 볼 것이 없어졌는데 first를
 * 이미 찾았다면 그것이 최종 답이므로 탐색을 종료(break)한다. 아직
 * 못 찾았다면 오른쪽으로 내려가 계속 찾는다(오른쪽 서브트리에는
 * 지금까지 본 것보다 finish가 큰 entity들만 있지만, eligible한 것을
 * 아직 못 찾았으므로 어쩔 수 없이 살펴봐야 함). bfqd->lock 하에서
 * 호출.
 *
 * 호출 체인:
 *   __bfq_lookup_next_entity() -> [bfq_first_active_entity] ->
 *   bfq_gt()
 */
static struct bfq_entity *bfq_first_active_entity(struct bfq_service_tree *st,
						  u64 vtime)
{
	struct bfq_entity *entry, *first = NULL; /* entry: 현재 훑는 노드의 entity, first: 지금까지 찾은 최선의 eligible 후보 */
	struct rb_node *node = st->active.rb_node; /* active tree의 루트에서 탐색 시작 */

	while (node) { /* 트리를 따라 내려가며 탐색(왼쪽 우선, 필요시 오른쪽) */
		entry = rb_entry(node, struct bfq_entity, rb_node); /* 현재 노드가 감싸는 entity를 얻음 */
left: /* [한국어] goto 레이블: 왼쪽 서브트리에 더 나은 eligible 후보가 있을 때 재진입하는 지점 */
		if (!bfq_gt(entry->start, vtime)) /* entry->start가 vtime을 넘지 않으면(eligible: 지금 서비스 가능) */
			first = entry; /* 현재까지의 최선 후보로 저장(더 왼쪽에 더 나은 것이 있으면 나중에 덮어씀) */

		if (node->rb_left) { /* 왼쪽 자식이 존재하면 그 서브트리도 확인 */
			entry = rb_entry(node->rb_left, /* 왼쪽 자식 노드의 entity를 얻음(min_start 확인용) */
					 struct bfq_entity, rb_node);
			if (!bfq_gt(entry->min_start, vtime)) { /* 왼쪽 서브트리 전체의 최소 start조차 vtime을 넘지 않는다면(그 안에 eligible한 것이 있음이 보장) */
				node = node->rb_left; /* 왼쪽으로 내려가 */
				goto left; /* 그 서브트리에서 더 나은(더 작은 finish) eligible 후보를 계속 탐색 */
			}
		}
		if (first) /* 왼쪽에 더 볼 것이 없고, 이미 eligible한 후보를 찾았다면 */
			break; /* 트리 성질상 이보다 더 나은(작은 finish) 후보는 없으므로 탐색 종료 */
		node = node->rb_right; /* 아직 못 찾았다면(왼쪽에 eligible한 것이 없었다면) 오른쪽 서브트리에서 계속 탐색 */
	}

	return first; /* 찾은 최선의 eligible entity(없으면 NULL) */
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
 * [한국어]
 * __bfq_lookup_next_entity - 한 service_tree(하나의 ioprio_class)
 *   안에서 "다음에 서비스될" entity를 찾고, 필요하면 vtime을 함께
 *   전진시킨다.
 *
 * @st: 탐색 대상 bfq_service_tree.
 * @in_service: 이 st가 속한 sched_data 레벨에 지금 서비스 중인
 *   entity가 있는지 여부(만료 경로에서는 호출자가 "없는 것처럼"
 *   조작해 전달할 수 있음 - bfq_lookup_next_entity() 참고).
 * @return: 다음 서비스 대상이 될 entity, active tree가 비어있으면 NULL.
 *
 * active tree가 완전히 비어있으면(RB_EMPTY_ROOT) 후보가 없으므로 즉시
 * NULL을 반환한다. 그렇지 않다면 bfq_calc_vtime_jump()로 "적어도
 * 하나의 entity가 eligible해지는 데 필요한 vtime"을 계산해 둔다.
 * 여기서 핵심 분기: in_service가 false라면(이 레벨에 지금 서비스
 * 중인 것이 실제로 없다면) bfq_update_vtime()으로 vtime을 그 값까지
 * 실제로 전진시킨다 - 서비스 중인 것이 없으므로 지금 당장 vtime을
 * 밀어도 아무 문제가 없기 때문이다. 반대로 in_service가 true라면
 * (이미 서비스 중인 entity가 있다면) vtime을 건드리지 않는다 - 이미
 * eligible한 entity(바로 그 in-service entity, 비록 서비스를 위해
 * active tree에서는 빠져 있을 수 있지만)가 존재하므로 vtime을 앞당길
 * 필요가 없고, 함부로 전진시키면 아직 서비스받지 못한 다른 entity들의
 * eligibility 판정을 왜곡할 수 있기 때문이다(원본 주석 참고). 마지막으로
 * bfq_first_active_entity()로 (전진했을 수도 있는) vtime을 기준으로
 * eligible하며 finish가 가장 작은 entity를 실제로 찾아 반환한다.
 * bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_lookup_next_entity() -> [__bfq_lookup_next_entity] ->
 *   bfq_calc_vtime_jump()/bfq_update_vtime()/bfq_first_active_entity()
 */
static struct bfq_entity *
__bfq_lookup_next_entity(struct bfq_service_tree *st, bool in_service)
{
	struct bfq_entity *entity; /* 최종적으로 선택될 entity */
	u64 new_vtime; /* eligible한 entity를 보장하는 데 필요한 vtime 목표값 */

	if (RB_EMPTY_ROOT(&st->active)) /* active tree에 entity가 하나도 없다면 */
		return NULL; /* 서비스할 후보 자체가 없으므로 즉시 반환 */

	/*
	 * Get the value of the system virtual time for which at
	 * least one entity is eligible.
	 */
	new_vtime = bfq_calc_vtime_jump(st); /* 적어도 하나의 entity가 eligible해지는 데 필요한 vtime 목표값 계산 */

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
	if (!in_service) /* 이 레벨에 지금 서비스 중인 entity가 없다면(vtime을 전진시켜도 안전) */
		bfq_update_vtime(st, new_vtime); /* vtime을 목표값까지 실제로 전진(필요시 idle 정리도 함께 수행) */

	entity = bfq_first_active_entity(st, new_vtime); /* (전진했을 수도 있는) vtime 기준으로 eligible하며 finish 최소인 entity 탐색 */

	return entity; /* 이 service_tree에서 선택된 다음 서비스 후보 */
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
 * [한국어]
 * bfq_lookup_next_entity - sched_data 한 레벨 안에서, RT/BE/IDLE
 *   3개 ioprio_class의 우선순위를 지키며 다음에 서비스할 entity를
 *   선택한다(IDLE 클래스 굶주림 방지 로직 포함).
 *
 * @sd: 탐색 대상 sched_data(3개 class별 service_tree 배열을 가짐).
 * @expiration: true이면 in-service 큐의 만료 처리 중임을 의미하며,
 *   sd->in_service_entity가 아직 필드상으로는 NULL이 아니더라도
 *   __bfq_lookup_next_entity()에는 "지금 서비스 중인 것이 없다"고
 *   알려야 한다(원본 주석 참고 - in_service_entity는 만료 경로의
 *   마지막 단계에서야 실제로 리셋되기 때문).
 * @return: 선택된 entity, 모든 클래스가 비어있으면 NULL.
 *
 * 먼저 IDLE 클래스 굶주림 방지 로직을 확인한다: RT/BE가 계속 바쁘면
 * IDLE 클래스는 원칙적으로 영원히 서비스받지 못할 수 있으므로,
 * sd->bfq_class_idle_last_service(IDLE이 마지막으로 서비스된 시각)
 * 이후 BFQ_CL_IDLE_TIMEOUT(200ms)이 지났다면, IDLE 클래스에 active한
 * entity가 있는 한(!RB_EMPTY_ROOT) class_idx를 강제로 IDLE 클래스
 * 인덱스로 설정해 최우선 검사 대상으로 만들고, bfq_class_idle_last_service를
 * 지금 시각(jiffies)으로 갱신한다(원본 주석: 이번에 서비스되지 않더라도
 * "검토는 됐다"는 의미로 갱신). 그 다음 class_idx(보통 0=RT부터,
 * 또는 위에서 강제로 IDLE로 설정됐다면 그 인덱스부터)에서
 * BFQ_IOPRIO_CLASSES까지 순서대로 각 service_tree에 대해
 * __bfq_lookup_next_entity()를 호출한다 - 이때 in_service 인자로
 * "sd->in_service_entity가 존재하면서 동시에 만료 경로가 아닌 경우"만
 * true를 전달해, 만료 중일 때는 vtime 전진이 허용되도록 만든다. 어느
 * 클래스에서든 entity를 찾으면(더 높은 우선순위 클래스부터 검사하므로)
 * 그 즉시 반복을 멈추고 반환한다 - 즉 RT에 후보가 있으면 BE/IDLE은
 * 아예 보지 않는 완전한 우선순위 순서다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_update_next_in_service() -> [bfq_lookup_next_entity] ->
 *   __bfq_lookup_next_entity()
 */
static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 bool expiration)
{
	struct bfq_service_tree *st = sd->service_tree; /* service_tree 배열의 시작(인덱스 0 = RT) */
	struct bfq_service_tree *idle_class_st = st + (BFQ_IOPRIO_CLASSES - 1); /* 배열의 마지막 슬롯 = IDLE 클래스 트리 */
	struct bfq_entity *entity = NULL; /* 최종 선택된 entity(기본값 없음) */
	int class_idx = 0; /* 탐색을 시작할 클래스 인덱스(기본 0=RT, 굶주림 방지 시 IDLE로 강제될 수 있음) */

	/*
	 * Choose from idle class, if needed to guarantee a minimum
	 * bandwidth to this class (and if there is some active entity
	 * in idle class). This should also mitigate
	 * priority-inversion problems in case a low priority task is
	 * holding file system resources.
	 */
	if (time_is_before_jiffies(sd->bfq_class_idle_last_service + /* IDLE이 마지막으로 서비스된 시각에 타임아웃을 더한 값이 이미 지났다면 */
				   BFQ_CL_IDLE_TIMEOUT)) { /* (즉 IDLE이 일정 시간 이상 방치됐다면) */
		if (!RB_EMPTY_ROOT(&idle_class_st->active)) /* 그리고 IDLE 클래스에 실제로 active한 entity가 있다면 */
			class_idx = BFQ_IOPRIO_CLASSES - 1; /* IDLE 클래스를 최우선 검사 대상으로 강제 지정(굶주림 방지) */
		/* About to be served if backlogged, or not yet backlogged */
		sd->bfq_class_idle_last_service = jiffies; /* 이번에 검토(또는 서비스)한 시각으로 타임스탬프 갱신 */
	}

	/*
	 * Find the next entity to serve for the highest-priority
	 * class, unless the idle class needs to be served.
	 */
	for (; class_idx < BFQ_IOPRIO_CLASSES; class_idx++) { /* class_idx부터 IDLE까지 우선순위 순으로 순회 */
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
		entity = __bfq_lookup_next_entity(st + class_idx, /* 현재 클래스의 service_tree에서 탐색 */
						  sd->in_service_entity && /* 실제로 in_service_entity 필드가 유효하고 */
						  !expiration); /* 만료 처리 중이 아닐 때만 "서비스 중인 것이 있다"고 전달 */

		if (entity) /* 이 클래스에서 후보를 찾았다면(더 높은 우선순위 클래스가 먼저 검사됨) */
			break; /* 더 낮은 우선순위 클래스는 검사하지 않고 즉시 종료 */
	}

	return entity; /* 우선순위 규칙에 따라 선택된 entity(모든 클래스가 비었으면 NULL) */
}

bool next_queue_may_preempt(struct bfq_data *bfqd)
{
	/*
	 * [한국어]
	 * next_queue_may_preempt - 루트 그룹 레벨에서 next_in_service가
	 *   현재 in_service_entity와 다른지(=선점 여지가 있는지) 확인한다.
	 *
	 * @bfqd: 디바이스 전역 상태.
	 * @return: true이면 지금 서비스 중인 것과 다른, 더 유리한 후보가
	 *   대기 중임을 의미(선점을 고려해야 함). false이면 지금 서비스
	 *   중인 것 자체가 이미 최선의 후보.
	 *
	 * 루트 그룹의 sched_data는 장치 전체에서 단 하나이며, 그
	 * next_in_service가 in_service_entity와 다르다는 것은 "지금
	 * 서비스 중인 것보다 finish가 더 작은(더 급한) entity가 이미
	 * 대기 중"이라는 뜻이다. 이는 새로 활성화된 높은 우선순위 큐가
	 * 있을 때 즉시 선점(현재 서비스를 중단하고 그 큐로 전환)할지
	 * 판단하는 BFQ 상위 로직(bfq_bfqq_expire() 등)의 게이트 조건으로
	 * 쓰인다. 단순 포인터 비교이므로 락 없이도 안전하지만, 필드 자체가
	 * bfqd->lock으로 보호되므로 통상 그 락 하에서 호출된다.
	 *
	 * 호출 체인:
	 *   bfq_bfqq_expire()/새 큐 활성화 경로(bfq-iosched.c) ->
	 *   [next_queue_may_preempt] -> (하위 호출 없음, 포인터 비교만)
	 */
	struct bfq_sched_data *sd = &bfqd->root_group->sched_data; /* 계층의 최상위(루트 그룹) sched_data */

	return sd->next_in_service != sd->in_service_entity; /* 캐시된 다음 후보와 현재 서비스 중인 entity가 다르면 선점 여지가 있음 */
}

/*
 * Get next queue for service.
 */
/*
 * [한국어]
 * bfq_get_next_queue - 계층의 루트에서 leaf까지 next_in_service를
 *   따라 내려가며, 그 경로상의 모든 entity를 in-service로 승격시키고
 *   최종 leaf bfq_queue를 반환한다. B-WF2Q+ 스케줄링의 핵심 진입점.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: 다음에 서비스할 bfq_queue, 활성 큐가 전혀 없으면 NULL.
 *
 * 먼저 bfq_tot_busy_queues()가 0이면(활성 큐가 전혀 없으면) 즉시
 * NULL을 반환한다. 그렇지 않으면 루트 그룹의 sched_data부터 시작해
 * "entity->my_sched_data가 NULL이 될 때까지"(즉 leaf에 도달할
 * 때까지) 아래 방향으로 내려가는 루프를 돈다: 각 레벨에서
 * sd->next_in_service(이전에 계산돼 캐시된 최선 후보)를 그대로
 * sd->in_service_entity로 승격시킨다(원본 주석의 WARNING: 이 값은
 * "지금 이 순간 다시 계산한 값"이 아니라 "마지막으로 트리가 바뀌었을
 * 때 계산해 캐시해 둔 값"이라 아주 드물게 최신 상태와 다를 수 있지만,
 * 그 오차는 다음 만료 시 다시 bfq_lookup_next_entity()가 호출되며
 * 자연히 교정된다). 그 다음 bfq_no_longer_next_in_service()로 이
 * entity가 (지금 in-service가 됨으로써) 더 이상 next_in_service
 * 후보 자격이 없는지 확인하고, 맞다면 bfq_active_extract()로 active
 * tree에서 실제로 뽑아낸다(트리에 남아 있으면 나중에 next_in_service
 * 재계산 때 자기 자신이 다시 뽑힐 수 있으므로). 그리고 entity->my_sched_data로
 * 한 단계 더 내려간다(leaf라면 my_sched_data가 NULL이라 루프 종료).
 * 루프가 끝나면 entity는 최종 선택된 leaf이므로 bfq_entity_to_bfqq()로
 * 그 bfq_queue를 얻는다. 마지막으로 for_each_entity() 루프로 그 leaf에서
 * 루트까지 다시 올라가며 bfq_update_next_in_service(sd, NULL, false)를
 * 호출해, 방금 leaf가 in-service로 확정됨에 따라 각 레벨의
 * next_in_service를 최신 상태로 재계산한다(상위에 영향이 없으면
 * break로 조기 종료). bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_dispatch_request()(bfq-iosched.c) -> [bfq_get_next_queue] ->
 *   bfq_tot_busy_queues()/bfq_no_longer_next_in_service()/
 *   bfq_active_extract()/bfq_entity_to_bfqq()/bfq_update_next_in_service()
 */
struct bfq_queue *bfq_get_next_queue(struct bfq_data *bfqd)
{
	struct bfq_entity *entity = NULL; /* 루프를 따라 내려가며 갱신될, 현재 레벨에서 선택된 entity */
	struct bfq_sched_data *sd; /* 현재 순회 중인 계층 레벨의 sched_data */
	struct bfq_queue *bfqq; /* 최종적으로 leaf에서 얻어질 bfq_queue */

	if (bfq_tot_busy_queues(bfqd) == 0) /* 장치 전체에 활성 큐가 하나도 없다면 */
		return NULL; /* 디스패치할 대상이 없으므로 즉시 반환 */

	/*
	 * Traverse the path from the root to the leaf entity to
	 * serve. Set in service all the entities visited along the
	 * way.
	 */
	sd = &bfqd->root_group->sched_data; /* 계층의 최상위(루트 그룹) sched_data에서 하강 시작 */
	for (; sd ; sd = entity->my_sched_data) { /* entity가 leaf가 되어 my_sched_data가 NULL이 될 때까지 한 단계씩 하강 */
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
		entity = sd->next_in_service; /* 캐시된 "다음 서비스 후보"를 이번에 실제로 서비스할 entity로 확정 */
		sd->in_service_entity = entity; /* 이 레벨에서 지금부터 이 entity가 서비스 중임을 기록 */

		/*
		 * If entity is no longer a candidate for next
		 * service, then it must be extracted from its active
		 * tree, so as to make sure that it won't be
		 * considered when computing next_in_service. See the
		 * comments on the function
		 * bfq_no_longer_next_in_service() for details.
		 */
		if (bfq_no_longer_next_in_service(entity)) /* leaf이거나(그룹인데) active 자식이 하나뿐이라 후보 자격을 잃었다면 */
			bfq_active_extract(bfq_entity_service_tree(entity), /* active tree에서 실제로 뽑아내 */
					   entity); /* 이후 next_in_service 재계산에서 중복 선택되지 않도록 함 */

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

	bfqq = bfq_entity_to_bfqq(entity); /* 루프 종료 시점의 entity는 leaf이므로, 그 bfq_queue를 얻음 */

	/*
	 * We can finally update all next-to-serve entities along the
	 * path from the leaf entity just set in service to the root.
	 */
	for_each_entity(entity) { /* 방금 확정된 leaf에서 루트까지 다시 올라가며 */
		struct bfq_sched_data *sd = entity->sched_data; /* 현재 레벨의 sched_data(바깥 sd와 별개의 지역 변수) */

		if (!bfq_update_next_in_service(sd, NULL, false)) /* 이 레벨의 next_in_service를 최신 상태로 재계산 */
			break; /* 상위에 영향이 없다면 더 올라갈 필요 없이 조기 종료 */
	}

	return bfqq; /* 다음에 디스패치할 bfq_queue(선택된 leaf) */
}

/* returns true if the in-service queue gets freed */
/*
 * [한국어]
 * __bfq_bfqd_reset_in_service - 현재 서비스 중이던 큐(bfqd->in_service_queue)를
 *   완전히 리셋하고, 계층 전체의 in_service_entity 표시를 지운다.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: true이면 이 호출로 인해 in-service였던 bfq_queue가 실제로
 *   메모리 해제됐음을 의미(호출자가 그 포인터를 더 이상 참조하면
 *   안 됨을 알리는 신호). false이면 아직 다른 참조가 남아 있어 큐가
 *   살아있음.
 *
 * 서비스 중이던 큐가 만료(expire)될 때 스케줄러 상태를 "지금 아무도
 * 서비스 중이 아님"으로 되돌리는 마무리 단계다. bfqd->in_service_queue와
 * 그 leaf entity(in_serv_entity)를 얻은 뒤, bfq_clear_bfqq_wait_request()로
 * "request 대기" 플래그를 지우고, hrtimer_try_to_cancel()로 idle
 * 슬라이스 타이머(장치를 잠시 더 붙잡아 두는 idling 메커니즘)를
 * 취소하며, bfqd->in_service_queue를 NULL로 리셋한다. 그 뒤
 * for_each_entity() 루프로 leaf에서 루트까지 모든 레벨의
 * sched_data->in_service_entity를 NULL로 리셋한다(이 시점에는 이미
 * 각 레벨이 적절히 비활성화/재큐잉됐다고 전제 - 원본 주석 참고).
 * 마지막으로 in_serv_entity->on_st_or_in_serv가 false라면(트리
 * 위에도 없고, 방금 서비스도 끝났다면 = 스케줄러가 완전히 손을 뗀
 * 상태) 그 서비스 참조(bfq_get_entity()가 잡았던 것)를
 * bfq_put_queue()로 반납한다. 이때 반납 직전의 ref 값이 1이었다면
 * (이 서비스 참조가 마지막 참조였다면) bfq_put_queue() 호출로 인해
 * bfqq가 실제로 free됐다는 뜻이므로 true를 반환해 호출자가 더 이상
 * in_serv_bfqq를 건드리지 않도록 경고한다. bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_bfqq_expire()(bfq-iosched.c) -> [__bfq_bfqd_reset_in_service] ->
 *   bfq_put_queue()
 */
bool __bfq_bfqd_reset_in_service(struct bfq_data *bfqd)
{
	struct bfq_queue *in_serv_bfqq = bfqd->in_service_queue; /* 지금까지 서비스 중이던 bfq_queue */
	struct bfq_entity *in_serv_entity = &in_serv_bfqq->entity; /* 그 큐를 감싸는 leaf entity */
	struct bfq_entity *entity = in_serv_entity; /* 아래 for_each_entity 루프의 시작점(leaf) */

	bfq_clear_bfqq_wait_request(in_serv_bfqq); /* "새 request를 기다리는 중" 플래그를 지움 - 서비스가 끝났으므로 더 이상 대기 상태가 아님 */
	hrtimer_try_to_cancel(&bfqd->idle_slice_timer); /* 장치를 잠시 더 붙잡아 두려던 idle 타이머가 있다면 취소 */
	bfqd->in_service_queue = NULL; /* 장치 전역 상태에서도 "지금 서비스 중인 큐 없음"으로 리셋 */

	/*
	 * When this function is called, all in-service entities have
	 * been properly deactivated or requeued, so we can safely
	 * execute the final step: reset in_service_entity along the
	 * path from entity to the root.
	 */
	for_each_entity(entity) /* leaf에서 루트까지 모든 계층 레벨을 순회하며 */
		entity->sched_data->in_service_entity = NULL; /* 각 레벨의 "서비스 중" 표시를 모두 지움 */

	/*
	 * in_serv_entity is no longer in service, so, if it is in no
	 * service tree either, then release the service reference to
	 * the queue it represents (taken with bfq_get_entity).
	 */
	if (!in_serv_entity->on_st_or_in_serv) { /* 트리 위에도 없고 서비스 중 표시도 이제 없다면(스케줄러가 완전히 손을 뗌) */
		/*
		 * If no process is referencing in_serv_bfqq any
		 * longer, then the service reference may be the only
		 * reference to the queue. If this is the case, then
		 * bfqq gets freed here.
		 */
		int ref = in_serv_bfqq->ref; /* 반납 직전의 참조 카운트를 미리 저장(반납 후에는 bfqq가 해제됐을 수 있어 다시 읽을 수 없음) */
		bfq_put_queue(in_serv_bfqq); /* 서비스 참조를 반납, ref가 0이 되면 이 호출 안에서 bfqq가 실제로 해제됨 */
		if (ref == 1) /* 반납 전 ref가 1이었다면(이 서비스 참조가 유일한 참조였다면) */
			return true; /* 방금 반납으로 bfqq가 해제됐음을 호출자에게 알림 */
	}

	return false; /* bfqq가 아직 다른 참조로 살아있거나, 애초에 트리/서비스에서 완전히 벗어나지 않았음 */
}

void bfq_deactivate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			 bool ins_into_idle_tree, bool expiration)
{
	/*
	 * [한국어]
	 * bfq_deactivate_bfqq - bfq_queue를 감싸는 leaf entity에 대해
	 *   bfq_deactivate_entity()를 호출하는 얇은 래퍼(bfq_queue 레벨의
	 *   공개 API).
	 *
	 * @bfqd: 디바이스 전역 상태(현재 구현에서는 직접 사용되지 않고
	 *   시그니처 일관성을 위해 유지됨).
	 * @bfqq: 비활성화할 큐.
	 * @ins_into_idle_tree: idle tree 보관 허용 여부.
	 * @expiration: 만료 경로 여부.
	 * @return: 없음(void).
	 *
	 * bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_bfqq_expire()/bfq_del_bfqq_busy()(bfq-iosched.c 등) ->
	 *   [bfq_deactivate_bfqq] -> bfq_deactivate_entity()
	 */
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 leaf entity */

	bfq_deactivate_entity(entity, ins_into_idle_tree, expiration); /* entity 레벨의 실제 비활성화(및 상위 전파)를 위임 */
}


void bfq_activate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/*
	 * [한국어]
	 * bfq_activate_bfqq - bfq_queue를 감싸는 leaf entity에 대해
	 *   bfq_activate_requeue_entity()를 호출해 active tree에 삽입하는
	 *   bfq_queue 레벨의 공개 API.
	 *
	 * @bfqd: 디바이스 전역 상태(직접 사용되지 않고 시그니처 일관성
	 *   유지용).
	 * @bfqq: 활성화할 큐.
	 * @return: 없음(void).
	 *
	 * bfq_bfqq_non_blocking_wait_rq()로 이 큐가 "request를 기다리며
	 * 막 활성화되는" seeky-latency 최적화 대상인지 확인해
	 * non_blocking_wait_rq 인자로 전달하고, requeue/expiration은
	 * 항상 false로 호출한다(진짜 새 활성화이지 재큐잉이 아니므로).
	 * 활성화 후에는 bfq_clear_bfqq_non_blocking_wait_rq()로 그
	 * 플래그를 클리어해 다음 활성화 때 다시 명시적으로 설정되도록 한다.
	 * bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_add_bfqq_busy()/bfq_requeue_bfqq() -> [bfq_activate_bfqq]
	 *   -> bfq_activate_requeue_entity()
	 */
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 leaf entity */

	bfq_activate_requeue_entity(entity, bfq_bfqq_non_blocking_wait_rq(bfqq), /* seeky-latency 최적화 힌트를 그대로 전달 */
				    false, false); /* 진짜 활성화이므로 requeue=false, expiration=false */
	bfq_clear_bfqq_non_blocking_wait_rq(bfqq); /* 힌트 플래그를 소비했으므로 클리어(다음번엔 다시 명시적으로 설정돼야 함) */
}

void bfq_requeue_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		      bool expiration)
{
	/*
	 * [한국어]
	 * bfq_requeue_bfqq - 이미 알고 있던 bfq_queue를 다시 서비스 트리에
	 *   넣는(재큐잉하는) bfq_queue 레벨의 공개 API.
	 *
	 * @bfqd: 디바이스 전역 상태(직접 사용되지 않고 시그니처 일관성
	 *   유지용).
	 * @bfqq: 재큐잉할 큐.
	 * @expiration: 방금 만료 처리를 거친 뒤의 재큐잉인지 여부.
	 * @return: 없음(void).
	 *
	 * non_blocking_wait_rq는 항상 false로(재큐잉은 새 request를
	 * 기다리던 seeky 큐의 첫 활성화가 아니므로 해당 최적화가 무의미),
	 * requeue는 "이 큐가 지금 in-service인지"를 그대로 전달한다 -
	 * in-service인 큐가 재큐잉되는 것은 만료로 인한 것이므로 그
	 * 상위 조상들도 강제로 재큐잉돼야 함을 뜻하기 때문(원본 주석의
	 * requeue 정의 참고). bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   bfq_bfqq_expire()(bfq-iosched.c) -> [bfq_requeue_bfqq] ->
	 *   bfq_activate_requeue_entity()
	 */
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 leaf entity */

	bfq_activate_requeue_entity(entity, false, /* 재큐잉이므로 non_blocking_wait_rq 최적화는 적용하지 않음 */
				    bfqq == bfqd->in_service_queue, expiration); /* requeue 플래그는 "지금 이 큐가 in-service인가"로 결정 */
}

void bfq_add_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq)
{
	/*
	 * [한국어]
	 * bfq_add_bfqq_in_groups_with_pending_reqs - bfqq가 pending
	 *   request를 처음 갖게 될 때, 그 소속 그룹을
	 *   bfqd->num_groups_with_pending_reqs 집계에 반영한다.
	 *
	 * @bfqq: 대상 큐.
	 * @return: 없음(void).
	 *
	 * entity->in_groups_with_pending_reqs 플래그로 "이미 집계에
	 * 반영됐는지"를 추적해 중복 카운트를 막는다: 아직 반영되지
	 * 않았다면 플래그를 true로 세우고, bfqq_group(bfqq)->
	 * num_queues_with_pending_reqs를 증가시키되 그 증가 "전" 값이
	 * 0이었다면(이 그룹에 pending 큐가 이번이 처음이라면)
	 * bfqd->num_groups_with_pending_reqs까지 함께 증가시킨다 -
	 * 즉 그룹 단위 카운트는 "그 그룹에 pending 큐가 하나라도
	 * 있는가"만 반영하는 근사(0/1) 카운트다. CONFIG_BFQ_GROUP_IOSCHED가
	 * 꺼진 빌드에서는 그룹 개념이 없어 함수 본문 전체가 비활성화된다.
	 * bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   request 삽입 경로(bfq-iosched.c) ->
	 *   [bfq_add_bfqq_in_groups_with_pending_reqs] -> bfqq_group()
	 */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 leaf entity(in_groups_with_pending_reqs 플래그 보유) */

	if (!entity->in_groups_with_pending_reqs) { /* 아직 이 그룹의 pending 집계에 반영되지 않았다면 */
		entity->in_groups_with_pending_reqs = true; /* 중복 반영 방지를 위해 반영됨으로 표시 */
		if (!(bfqq_group(bfqq)->num_queues_with_pending_reqs++)) /* 그룹 내 pending 큐 카운트를 증가시키되, 증가 전 값이 0이었다면(이 그룹의 첫 pending 큐라면) */
			bfqq->bfqd->num_groups_with_pending_reqs++; /* 장치 전역 "pending 큐를 가진 그룹 수"도 함께 증가 */
	}
#endif
}

void bfq_del_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq)
{
	/*
	 * [한국어]
	 * bfq_del_bfqq_in_groups_with_pending_reqs - bfqq의 pending
	 *   request가 모두 사라졌을 때 그룹의 pending 집계에서 제외한다
	 *   (bfq_add_bfqq_in_groups_with_pending_reqs()의 역연산).
	 *
	 * @bfqq: 대상 큐.
	 * @return: 없음(void).
	 *
	 * entity->in_groups_with_pending_reqs가 true(이미 집계에 반영돼
	 * 있음)일 때만 플래그를 false로 내리고, 그룹의
	 * num_queues_with_pending_reqs를 감소시키되 그 감소 "후" 값이
	 * 0이 됐다면(이 그룹에 pending 큐가 이제 하나도 없다면)
	 * bfqd->num_groups_with_pending_reqs도 함께 감소시킨다.
	 * CONFIG_BFQ_GROUP_IOSCHED가 꺼진 빌드에서는 본문 전체가
	 * 비활성화된다. bfqd->lock 하에서 호출.
	 *
	 * 호출 체인:
	 *   request 완료/제거 경로(bfq-iosched.c) ->
	 *   [bfq_del_bfqq_in_groups_with_pending_reqs] -> bfqq_group()
	 */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 leaf entity */

	if (entity->in_groups_with_pending_reqs) { /* 이 큐가 그룹의 pending 집계에 반영돼 있었다면 */
		entity->in_groups_with_pending_reqs = false; /* 집계에서 빠졌음을 표시 */
		if (!(--bfqq_group(bfqq)->num_queues_with_pending_reqs)) /* 그룹 내 pending 큐 카운트를 먼저 감소시킨 뒤, 그 결과가 0이 됐다면 */
			bfqq->bfqd->num_groups_with_pending_reqs--; /* 장치 전역 "pending 큐를 가진 그룹 수"도 함께 감소 */
	}
#endif
}

/*
 * Called when the bfqq no longer has requests pending, remove it from
 * the service tree. As a special case, it can be invoked during an
 * expiration.
 */
/*
 * [한국어]
 * bfq_del_bfqq_busy - 더 이상 대기 request가 없는 bfqq를 "busy"
 *   관련 전역 카운터/트리에서 모두 제거한다.
 *
 * @bfqq: 대상 큐.
 * @expiration: 만료로 인한 제거인지 여부(통계 구분용).
 * @return: 없음(void).
 *
 * bfq_clear_bfqq_busy()로 busy 플래그를 지우고,
 * bfqd->busy_queues[ioprio_class-1]를 감소시켜 클래스별 busy 큐 수를
 * 갱신한다. weight-raised 상태(wr_coeff > 1)였다면
 * bfqd->wr_busy_queues도 함께 감소시킨다. bfqg_stats_update_dequeue()로
 * cgroup 통계(dequeue 이벤트)를 갱신한 뒤,
 * bfq_deactivate_bfqq(bfqd, bfqq, true, expiration)를 호출해 실제로
 * entity를 active/idle tree에서 내리는 B-WF2Q+ 레벨 처리를 수행한다
 * (ins_into_idle_tree를 true로 넘겨 finish가 아직 남아있으면 idle로
 * 보관되도록 허용). 마지막으로 bfqq->dispatched(드라이버에 아직
 * 전달돼 완료를 기다리는 request 수)가 0이라면(이 큐에 관련된 어떤
 * I/O도 진행 중이지 않다면) bfq_del_bfqq_in_groups_with_pending_reqs()로
 * pending 그룹 집계에서 빼고, bfq_weights_tree_remove()로 장치 전역
 * weight 카운터 트리에서도 제거한다 - 원본 주석대로 이 마지막 호출은
 * bfqq를 실제로 해제시킬 수 있으므로 반드시 맨 마지막에 수행해야
 * 한다(그 이후로는 bfqq를 더 이상 참조하면 안 됨). bfqd->lock 하에서
 * 호출.
 *
 * 호출 체인:
 *   bfq_deactivate_bfqq()(만료/제거 경로, bfq-iosched.c) ->
 *   [bfq_del_bfqq_busy] -> bfqg_stats_update_dequeue()/
 *   bfq_deactivate_bfqq()/bfq_del_bfqq_in_groups_with_pending_reqs()/
 *   bfq_weights_tree_remove()
 */
void bfq_del_bfqq_busy(struct bfq_queue *bfqq, bool expiration)
{
	struct bfq_data *bfqd = bfqq->bfqd; /* 이 큐가 속한 디바이스 전역 상태 */

	bfq_log_bfqq(bfqd, bfqq, "del from busy"); /* 디버그 트레이스: busy 집합에서 제거됨을 기록 */

	bfq_clear_bfqq_busy(bfqq); /* "busy(대기 request 있음)" 플래그를 지움 */

	bfqd->busy_queues[bfqq->ioprio_class - 1]--; /* 이 큐의 ioprio_class에 해당하는 busy 큐 카운터를 감소 */

	if (bfqq->wr_coeff > 1) /* weight-raising이 적용 중이던 큐였다면 */
		bfqd->wr_busy_queues--; /* WR 상태인 busy 큐 카운터도 함께 감소 */

	bfqg_stats_update_dequeue(bfqq_group(bfqq)); /* 소속 cgroup의 dequeue 통계(blkio 통계)를 갱신 */

	bfq_deactivate_bfqq(bfqd, bfqq, true, expiration); /* entity 레벨에서 실제로 active/idle tree 처리를 수행(idle 보관 허용) */

	if (!bfqq->dispatched) { /* 드라이버에 아직 완료되지 않은 request가 하나도 없다면(이 큐 관련 I/O가 전혀 진행 중이지 않음) */
		bfq_del_bfqq_in_groups_with_pending_reqs(bfqq); /* pending 그룹 집계에서 제외 */
		/*
		 * Next function is invoked last, because it causes bfqq to be
		 * freed. DO NOT use bfqq after the next function invocation.
		 */
		bfq_weights_tree_remove(bfqq); /* 장치 전역 weight 카운터 트리에서 제거(이 호출로 bfqq가 해제될 수 있으므로 반드시 마지막) */
	}
}

/*
 * Called when an inactive queue receives a new request.
 */
/*
 * [한국어]
 * bfq_add_bfqq_busy - 비활성 상태였던 bfqq가 새 request를 받아
 *   busy(활성) 상태로 전이할 때, 관련 전역 카운터/트리에 등록한다
 *   (bfq_del_bfqq_busy()의 역연산).
 *
 * @bfqq: 대상 큐.
 * @return: 없음(void).
 *
 * bfq_activate_bfqq()로 실제 B-WF2Q+ 활성화(active tree 삽입)를
 * 수행한 뒤, bfq_mark_bfqq_busy()로 busy 플래그를 세우고
 * bfqd->busy_queues[ioprio_class-1]를 증가시킨다. bfqq->dispatched가
 * 0이라면(드라이버에 진행 중인 request가 없던, 완전히 새로 시작하는
 * 흐름이라면) bfq_add_bfqq_in_groups_with_pending_reqs()로 pending
 * 그룹 집계에 등록하고, weight-raised 상태가 아니라면(wr_coeff == 1)
 * bfq_weights_tree_add()로 weight 카운터 트리에도 등록한다(WR 큐는
 * 일시적으로 다른 weight를 쓰므로 이 카운터 집계에서 의도적으로
 * 제외됨 - __bfq_entity_update_weight_prio() 참고). weight-raised
 * 상태(wr_coeff > 1)라면 bfqd->wr_busy_queues를 증가시킨다. 마지막으로
 * "waker(이 큐를 깨운 것으로 추정되는 다른 큐)의 woken list" 관리:
 * 이 큐가 이미 어떤 waker의 woken_list에 등록돼 있는데(hlist_unhashed로
 * 확인) 그 리스트의 맨 앞(head)이 아니라면, 일단 제거한 뒤 다시 head로
 * 옮겨 "가장 최근에 깨어난 woken 큐"가 항상 리스트 맨 앞에 오도록
 * 유지한다(waker/woken 관계는 동기적으로 서로를 깨우는 프로세스 쌍을
 * 추적해 injection/idling 휴리스틱에 활용하는 BFQ 특유 메커니즘).
 * bfqd->lock 하에서 호출.
 *
 * 호출 체인:
 *   bfq_add_request()(bfq-iosched.c) -> [bfq_add_bfqq_busy] ->
 *   bfq_activate_bfqq()/bfq_add_bfqq_in_groups_with_pending_reqs()/
 *   bfq_weights_tree_add()
 */
void bfq_add_bfqq_busy(struct bfq_queue *bfqq)
{
	struct bfq_data *bfqd = bfqq->bfqd; /* 이 큐가 속한 디바이스 전역 상태 */

	bfq_log_bfqq(bfqd, bfqq, "add to busy"); /* 디버그 트레이스: busy 집합에 추가됨을 기록 */

	bfq_activate_bfqq(bfqd, bfqq); /* entity 레벨에서 실제 active tree 삽입(B-WF2Q+ 활성화)을 수행 */

	bfq_mark_bfqq_busy(bfqq); /* "busy(대기 request 있음)" 플래그를 세움 */
	bfqd->busy_queues[bfqq->ioprio_class - 1]++; /* 이 큐의 ioprio_class에 해당하는 busy 큐 카운터를 증가 */

	if (!bfqq->dispatched) { /* 드라이버에 진행 중인 request가 없던(완전히 새로 시작하는) 흐름이라면 */
		bfq_add_bfqq_in_groups_with_pending_reqs(bfqq); /* pending 그룹 집계에 등록 */
		if (bfqq->wr_coeff == 1) /* weight-raising이 적용 중이지 않다면(순수 weight 상태) */
			bfq_weights_tree_add(bfqq); /* 장치 전역 weight 카운터 트리에 등록(WR 큐는 의도적으로 제외) */
	}

	if (bfqq->wr_coeff > 1) /* weight-raising이 적용 중이라면 */
		bfqd->wr_busy_queues++; /* WR 상태인 busy 큐 카운터를 증가 */

	/* Move bfqq to the head of the woken list of its waker */
	if (!hlist_unhashed(&bfqq->woken_list_node) && /* 이 큐가 어떤 waker의 woken_list에 이미 등록돼 있고 */
	    &bfqq->woken_list_node != bfqq->waker_bfqq->woken_list.first) { /* 그 리스트의 맨 앞(head)이 아직 아니라면 */
		hlist_del_init(&bfqq->woken_list_node); /* 기존 위치에서 제거(리스트에서 빼되 노드 자체는 재사용 가능하게 초기화) */
		hlist_add_head(&bfqq->woken_list_node, /* 방금 활성화된(=가장 최근에 깨어난) 이 큐를 */
			       &bfqq->waker_bfqq->woken_list); /* waker의 woken_list 맨 앞으로 다시 삽입 */
	}
}
