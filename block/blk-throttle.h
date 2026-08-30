/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_THROTTLE_H
#define BLK_THROTTLE_H	/* [한국어] 인클루드 가드. 바로 위 #ifndef 와 짝을 이뤄, 이 헤더가 한 번역 단위에
				 * 여러 번 포함돼도 내용이 한 번만 펼쳐지게 한다(중복 정의 오류 방지). */

/*
 * [한국어] blk-throttle 헤더: cgroup 기반 IO 대역폭/IOPS 조율 자료구조 정의 (blk-throttle.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 blk-throttle.c가 구현하는 cgroup 기반 블록 IO 쓰로틀링(throttling)
 * 계층의 핵심 자료구조(struct throtl_qnode, struct throtl_service_queue,
 * struct throtl_grp)와 상태 플래그(enum tg_state_flags), 그리고 블록 레이어
 * 제출 경로에서 인라인으로 즉시 판단해야 하는 헬퍼 함수들
 * (pd_to_tg(), blkg_to_tg(), blk_throtl_activated(), blk_should_throtl(),
 * blk_throtl_bio())을 정의한다. bio가 request로 바뀌어 드라이버에 넘어가기
 * 전 단계에서, cgroup별 bps(bytes per second)/iops(IO per second) 상한을
 * 검사·집계하기 위한 공용 타입 정의 지점이다. 이 계층은 하위 장치가 무엇인지
 * 전혀 참조하지 않는 일반 블록 계층 코드다.
 * CONFIG_BLK_DEV_THROTTLING이 꺼진 커널에서는 동일한 이름의 no-op 인라인
 * stub을 제공해 호출부(blk-mq, block/blk-core.c 등)가 #ifdef 분기 없이
 * 동일한 API를 쓸 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (CONFIG_BLK_DEV_THROTTLING=y):
 *   submit_bio()
 *     → blk_mq_submit_bio()
 *       → blk_throtl_bio()            (본 헤더의 static inline)
 *         → blk_should_throtl()       (본 헤더의 static inline) : 통과 여부 사전 판단
 *         → __blk_throtl_bio()        (blk-throttle.c 구현, 본 헤더는 선언만 보유)
 *           [제한 초과 시 throtl_grp 큐에 보관 후 pending_timer 대기]
 *           → (blk-throttle.c의 디스패치 경로를 거쳐) submit_bio_noacct_nocheck()
 *             → blk_mq_submit_bio() 재진입 → request 할당 → 드라이버 queue_rq()
 * 실행 컨텍스트: 이 헤더의 인라인 함수들은 submit_bio()를 호출하는 임의의
 * 프로세스/커널 스레드 컨텍스트에서 실행된다 (인터럽트 컨텍스트 아님).
 * 별도의 락을 걸지 않고 bio->bi_blkg, bio->bi_opf 등 bio 자체의 불변 필드만
 * 읽으므로 재진입/동시 호출에 안전하다 (tg 내부 상태 변경은 __blk_throtl_bio()
 * 쪽에서 queue_lock으로 보호).
 *
 * === 타 모듈과의 연결 ===
 * - blk-cgroup (blkcg_gq, blkcg_policy, blkg_policy_data): struct throtl_grp
 *   는 blkg_policy_data(pd)를 첫 멤버로 내장해 blkcg 정책 프레임워크에
 *   등록된다. blkg_to_tg()/pd_to_tg()가 이 변환을 담당.
 * - blk-cgroup-rwstat.h (struct blkg_rwstat): struct throtl_grp의
 *   stat_bytes/stat_ios 필드가 이 타입을 사용해 cgroup별 READ/WRITE
 *   바이트·IO 수를 집계한다. 이 헤더가 반드시 먼저 include되어야 하는 이유.
 * - blk-throttle.c: 이 헤더가 선언한 __blk_throtl_bio(), blk_throtl_exit(),
 *   blk_throtl_cancel_bios(), extern blkcg_policy_throtl의 실제 구현을 담고
 *   있으며, throtl_data 등 이 헤더에 없는 내부 전용 자료구조도 함께 정의한다.
 * - blk-mq / block core (blk-mq.c, blk-core.c): blk_throtl_bio()를
 *   submit_bio() 경로 초입에서 호출해, bio가 request로 변환되기 전에 이
 *   헤더의 판단 로직을 통과시킨다.
 * - cgroupfs (io.max, blkio.throttle.*): 사용자 공간에서 설정한 bps/iops
 *   상한이 blk-throttle.c의 tg_set_conf() 등을 통해 여기 정의된
 *   throtl_grp.bps[]/iops[]에 기록된다 (이 헤더는 그 저장소 타입만 정의).
 * 데이터 흐름: bio(bi_blkg, bi_opf, bi_iter.bi_size) → blkg_to_tg()로 tg 획득
 * → blk_should_throtl()에서 tg->has_rules_*[]/stat_* 갱신 → true/false 판단
 * → blk_throtl_bio()가 필요 시 __blk_throtl_bio()(blk-throttle.c)에 위임.
 *
 * === 주요 함수/구조체 요약 ===
 * struct throtl_qnode          : service_queue에 큐잉되는 bio를 출처(cgroup)
 *                                별로 분리 보관해 round-robin 공정성을 보장.
 * struct throtl_service_queue  : 계층적 cgroup 트리의 각 레벨(자기 자신 또는
 *                                td)이 갖는 대기열 + pending_tree + 타이머.
 * struct throtl_grp (tg)       : cgroup 1개 × request_queue 1개에 대응하는
 *                                rate-limit 상태 전체(bps/iops 상한,
 *                                slice 윈도우, 사용량, 통계).
 * enum tg_state_flags          : tg->flags에 저장되는 비트 플래그
 *                                (PENDING/WAS_EMPTY/IOPS_WAS_EMPTY/CANCELING).
 * pd_to_tg()/blkg_to_tg()      : blkcg 정책 프레임워크 타입 ↔ throtl_grp
 *                                상호 변환 인라인 함수.
 * blk_throtl_activated()       : request_queue에 throtl 정책이 실제로
 *                                활성화되어 있는지 확인.
 * blk_should_throtl()          : bio가 throttle 검사를 받아야 하는지
 *                                (has_rules_iops/bps + 통계 집계) 사전 판단.
 * blk_throtl_bio()             : blk_should_throtl() 결과에 따라
 *                                __blk_throtl_bio() 호출 여부를 결정하는
 *                                submit_bio() 경로의 실질적 진입점.
 */

#include "blk-cgroup-rwstat.h"
/* [한국어] struct blkg_rwstat 타입 정의를 가져온다. 아래 struct throtl_grp의
 * stat_bytes/stat_ios 필드가 이 타입으로 cgroup별 READ/WRITE 바이트 수와
 * IO 개수를 누적한다. blk_should_throtl()(본 헤더 하단)이 bio 처리 시마다
 * blkg_rwstat_add()로 갱신한다. */

/*
 * To implement hierarchical throttling, throtl_grps form a tree and bios
 * are dispatched upwards level by level until they reach the top and get
 * issued.  When dispatching bios from the children and local group at each
 * level, if the bios are dispatched into a single bio_list, there's a risk
 * of a local or child group which can queue many bios at once filling up
 * the list starving others.
 *
 * To avoid such starvation, dispatched bios are queued separately
 * according to where they came from.  When they are again dispatched to
 * the parent, they're popped in round-robin order so that no single source
 * hogs the dispatch window.
 *
 * throtl_qnode is used to keep the queued bios separated by their sources.
 * Bios are queued to throtl_qnode which in turn is queued to
 * throtl_service_queue and then dispatched in round-robin order.
 *
 * It's also used to track the reference counts on blkg's.  A qnode always
 * belongs to a throtl_grp and gets queued on itself or the parent, so
 * incrementing the reference of the associated throtl_grp when a qnode is
 * queued and decrementing when dequeued is enough to keep the whole blkg
 * tree pinned while bios are in flight.
 */

/*
 * [한국어] struct throtl_qnode 개요:
 *  - node: service_queue->queued[]에 연결되는 고리. 이 리스트에 올라 있는
 *    동안만 "활성 출처"로 취급되며, 꺼낼 때마다 꼬리로 돌려져 형제 cgroup
 *    간 round-robin이 성립한다.
 *  - bios_bps: 아직 bps 관문을 통과하지 못해 대기 중인 bio들.
 *  - bios_iops: bps는 통과했고 iops 관문만 남은 bio들. 두 관문의 대기열을
 *    나눠 두어야 각 관문이 자기 줄의 선두만 보고 판정할 수 있고, 이미 bps를
 *    통과한 bio(분할되어 되돌아온 것 등)가 bps 줄에 다시 서지 않는다.
 *  - tg: 이 qnode가 속한 throtl_grp(cgroup)을 가리킨다. qnode가 활성화되어
 *    있는 동안 이 tg의 blkg 참조를 하나 붙들고 있다.
 * 필드별 상세(설정자/읽는 자/값 범위/동기화)는 각 필드 아래 개별 주석 참고.
 */
struct throtl_qnode {
	struct list_head	node;		/* service_queue->queued[] */
	/* [한국어] 이 qnode를 상위 struct throtl_service_queue의 queued[rw]
	 * 리스트에 연결하는 노드. qnode 자체가 "하나의 cgroup 출처"를 의미하며,
	 * 이 node가 리스트에 매달려 있어야 round-robin 순회 대상이 된다.
	 * 설정자: throtl_qnode_add_bio()가 qnode를 처음 활성화할 때
	 *   list_add_tail(&qn->node, &sq->queued[rw])로 연결.
	 * 읽는 자: throtl_peek_queued()/throtl_pop_queued()가 list_first_entry()로
	 *   순회 순서상 맨 앞 qnode를 찾고, 소진되지 않으면 list_move_tail()로
	 *   맨 뒤로 옮겨 다음 라운드의 기회를 다른 qnode에게 넘긴다.
	 * 값 범위: 리스트에 미연결 시 list_empty(&node) == true (초기화 직후
	 *   또는 bios_bps/bios_iops가 모두 비어 throtl_pop_queued()가 제거한 뒤).
	 * 동기화: 소속 request_queue의 queue_lock(spinlock) 보유 상태에서만
	 *   조작된다 (throtl_grp/tg 단위가 아니라 전역 queue 단위 락). */

	struct bio_list		bios_bps;	/* queued bios for bps limit */
	/* [한국어] bps(대역폭) 제한에 걸려 아직 하위 계층으로 전진하지 못한
	 * bio들의 연결 리스트. 이 리스트에 있다는 것은 아직
	 * throtl_charge_bps_bio()로 bps 사용량이 차감되지 않았다는 뜻이다.
	 * 설정자: throtl_qnode_add_bio()가 BIO_TG_BPS_THROTTLED/BIO_BPS_THROTTLED
	 *   플래그가 없는 bio를 bio_list_add()로 추가.
	 * 읽는 자: throtl_peek_queued()/throtl_pop_queued()가 bios_iops가 빈
	 *   경우에만 이 리스트를 확인 (iops 큐가 우선).
	 * 값 범위: bio_list이므로 비어 있으면(bio_list_empty()) NULL 헤드;
	 *   여러 bio가 FIFO로 연결될 수 있음.
	 * 동기화: queue_lock 보유 상태에서만 bio_list_add()/bio_list_pop() 호출. */

	struct bio_list		bios_iops;	/* queued bios for iops limit */
	/* [한국어] iops(초당 명령 수) 제한만 남은 bio들의 연결 리스트. bps 제한을
	 * 이미 통과했거나(BIO_TG_BPS_THROTTLED/BIO_BPS_THROTTLED 플래그) 원래
	 * bps 규칙이 없는 bio가 여기로 직접 들어온다.
	 * 설정자: throtl_qnode_add_bio()가 위 플래그가 설정된 bio를 이 리스트에
	 *   추가.
	 * 읽는 자: throtl_peek_queued()/throtl_pop_queued()가 항상 이 리스트를
	 *   먼저 확인해 하위 계층으로 내보낼 다음 bio를 고른다 (iops 큐 우선 원칙).
	 * 값 범위: bios_bps와 동일하게 FIFO bio_list.
	 * 동기화: queue_lock 보유 상태에서만 조작. */

	struct throtl_grp	*tg;		/* tg this qnode belongs to */
	/* [한국어] 이 qnode가 속한 throtl_grp(cgroup)에 대한 역참조 포인터.
	 * qnode 자체는 어느 cgroup의 bio를 대신 들고 있는지 알아야 하며,
	 * 이 포인터를 통해 qnode가 활성화되는 동안 blkg 참조 카운트를 유지한다.
	 * 설정자: throtl_qnode_init()에서 초기화 시 1회 대입 (이후 불변).
	 * 읽는 자: throtl_qnode_add_bio()가 qnode를 처음 리스트에 매달 때
	 *   blkg_get(tg_to_blkg(qn->tg))로 참조를 올리고, throtl_pop_queued()가
	 *   qnode를 리스트에서 제거할 때 blkg_put()으로 참조를 내린다.
	 * 값 범위: 유효한 throtl_grp 포인터 (NULL 불가; qnode는 항상 tg에
	 *   내장되어 있음 - qnode_on_self[]/qnode_on_parent[] 배열).
	 * 동기화: 초기화 후 값 자체는 불변이지만, 이를 통해 조작하는 blkg
	 *   참조 카운트는 queue_lock 하에서 원자적으로 증감. */
};

/*
 * [한국어] struct throtl_service_queue 개요:
 *  - parent_sq: 계층적 cgroup 트리에서 부모 service_queue를 가리킨다. bio는
 *    말단 cgroup에서 루트로 한 칸씩 올라가며 각 계층의 제한을 다시 받고,
 *    루트(parent_sq == NULL)에 닿아야 비로소 발행 대상이 된다.
 *  - queued[2]: READ/WRITE별 throtl_qnode 연결 리스트. 꺼낼 때마다 qnode를
 *    꼬리로 돌려 형제 cgroup 간 round-robin을 만들고, 한 그룹이 큐를
 *    독점해 나머지가 굶는 상황을 막는다.
 *  - nr_queued_bps[2]/nr_queued_iops[2]: 각 관문에서 대기 중인 bio 개수.
 *    두 값의 합(sq_queued())이 "이 방향에 대기가 있는가"의 판단 근거다.
 *  - pending_tree: 활성화된 throtl_grp을 disptime 기준으로 정렬한 RB 트리.
 *    다음에 디스패치할 후보 그룹을 시간 순서로 선택할 때 사용된다.
 *  - first_pending_disptime/pending_timer: 가장 먼저 throttle이 풀릴 그룹의
 *    시점을 관리하며, 타이머가 만료되면 blk_throtl_dispatch_work_fn()을
 *    통해 bio를 다시 blk_mq_submit_bio() 경로로 복귀시킨다.
 * 필드별 상세(설정자/읽는 자/값 범위/동기화)는 각 필드 아래 개별 주석 참고.
 */
struct throtl_service_queue {
	struct throtl_service_queue *parent_sq;	/* the parent service_queue */
	/* [한국어] 계층적 cgroup 트리에서 한 단계 위(부모)의 service_queue.
	 * bio는 leaf(자식) tg의 service_queue에서 시작해 이 포인터를 따라
	 * 한 레벨씩 위로 디스패치되며, 최상위(=throtl_data.service_queue,
	 * parent_sq == NULL)에 도달해야 비로소 실제 발행 경로로 넘어간다.
	 * 설정자: throtl_pd_init()에서 v1(cgroup1)이면 항상 td->service_queue로,
	 *   v2(cgroup2, default hierarchy)면 부모 blkg의 tg->service_queue로
	 *   1회 설정 (cgroup 계층 이동 없이는 이후 불변).
	 * 읽는 자: tg_service_queue_add()/throtl_schedule_next_dispatch() 등
	 *   대부분의 디스패치 로직이 "부모에게 넘긴다"의 대상으로 참조.
	 * 값 범위: 최상위 throtl_data.service_queue는 자체 parent_sq가 NULL;
	 *   sq_to_tg()/sq_to_td()가 이 NULL 여부로 최상위 여부를 판별한다.
	 * 동기화: request_queue의 queue_lock(spinlock) 보호 하에 읽고 쓴다. */

	/*
	 * Bios queued directly to this service_queue or dispatched from
	 * children throtl_grp's.
	 */
	struct list_head	queued[2];	/* throtl_qnode [READ/WRITE] */
	/* [한국어] READ(=queued[0])/WRITE(=queued[1]) 방향별로, 이 service_queue에
	 * 매달린 throtl_qnode들의 연결 리스트 헤드. 여러 cgroup(local 또는 자식)
	 * 에서 온 qnode가 이 리스트에 나란히 매달리고, 리스트 순서(round-robin)
	 * 대로 하나씩 꺼내져 하위 계층으로 전진한다.
	 * 설정자: throtl_qnode_add_bio()가 qnode를 처음 활성화할 때
	 *   list_add_tail()로 추가.
	 * 읽는 자: throtl_peek_queued()/throtl_pop_queued()가 list_first_entry()로
	 *   맨 앞 qnode를 조회/제거한다.
	 * 값 범위: 리스트가 비면(list_empty()) 이 방향으로 대기 중인 bio가 전혀
	 *   없다는 뜻이며 sq_queued()가 0을 반환.
	 * 동기화: queue_lock 보호. */

	unsigned int		nr_queued_bps[2];	/* number of queued bps bios */
	/* [한국어] READ/WRITE 방향별로 이 service_queue 아래(모든 qnode 합산)에서
	 * bps 제한 때문에 대기 중인 bio 총 개수. throtl_qnode.bios_bps 리스트들의
	 * 합계와 정확히 일치해야 한다 (throtl_qnode_add_bio()/throtl_pop_queued()가
	 * bio_list 조작과 항상 함께 증감시킴).
	 * 설정자: throtl_qnode_add_bio()(+1), throtl_pop_queued()(-1).
	 * 읽는 자: sq_queued()가 nr_queued_bps[rw] + nr_queued_iops[rw]로
	 *   "이 방향에 대기 중인 bio가 있는지"를 판단할 때 사용.
	 * 값 범위: 0 이상. 아직 bps 관문을 통과하지 못한 bio 수이며, 값이
	 *   클수록 그 방향의 대역폭 예산이 부족하다는 뜻이다.
	 * 동기화: queue_lock 보호. */

	unsigned int		nr_queued_iops[2];	/* number of queued iops bios */
	/* [한국어] READ/WRITE 방향별로 iops 제한 때문에 대기 중인 bio 총 개수.
	 * throtl_qnode.bios_iops 리스트들의 합계와 일치.
	 * 설정자: throtl_qnode_add_bio()(+1), throtl_pop_queued()(-1).
	 * 읽는 자: sq_queued(), throtl_dispatch_tg()의 종료 조건 판단 등.
	 * 값 범위: 0 이상. bps는 통과했지만 iops 관문에서 대기 중인 bio 수.
	 * 동기화: queue_lock 보호. */

	/*
	 * RB tree of active children throtl_grp's, which are sorted by
	 * their ->disptime.
	 */
	struct rb_root_cached	pending_tree;	/* RB tree of active tgs */
	/* [한국어] 이 service_queue의 직계 자식 throtl_grp 중, 대기 중인 bio가
	 * 있어 향후 disptime에 디스패치되어야 하는 tg들을 disptime 오름차순으로
	 * 정렬한 RB 트리 (rb_root_cached라 leftmost를 O(1)에 조회 가능).
	 * "다음에 bio를 내보낼 후보 cgroup을 시간순으로 골라내는" 스케줄링
	 * 자료구조이며, 정렬돼 있기 때문에 최좌단이 아직 때가 아니면 나머지도
	 * 볼 필요 없이 순회를 끝낼 수 있다.
	 * 설정자: tg_service_queue_add()가 tg->disptime을 key로 삽입,
	 *   throtl_rb_erase()가 제거.
	 * 읽는 자: throtl_rb_first()가 leftmost(가장 이른 disptime) tg를 조회해
	 *   throtl_select_dispatch()/update_min_dispatch_time()에 전달.
	 * 값 범위: 비어 있으면(RB_EMPTY_ROOT 상당) nr_pending == 0과 일치.
	 * 동기화: queue_lock 보호. */

	unsigned int		nr_pending;	/* # queued in the tree */
	/* [한국어] pending_tree에 현재 삽입되어 있는 throtl_grp 노드 수.
	 * throtl_schedule_next_dispatch()가 "더 이상 처리할 pending 자식이
	 * 없다"를 빠르게 판단하는 카운터 (RB 트리 순회 없이 0 검사만으로 충분).
	 * 설정자: throtl_enqueue_tg()(+1), throtl_dequeue_tg()(-1).
	 * 읽는 자: throtl_schedule_next_dispatch()가 0이면 즉시 true(더 예약할
	 *   것 없음) 반환.
	 * 값 범위: 0 이상, pending_tree의 실제 노드 수와 항상 일치해야 함.
	 * 동기화: queue_lock 보호. */

	unsigned long		first_pending_disptime;	/* disptime of the first tg */
	/* [한국어] pending_tree에서 disptime이 가장 이른(leftmost) tg의 disptime
	 * 값을 캐시해 둔 것. pending_timer를 이 값으로 arm하여, 실제로 그
	 * jiffies에 도달했을 때만 타이머 콜백(throtl_pending_timer_fn())이
	 * 깨어나 디스패치를 재개하게 한다.
	 * 설정자: update_min_dispatch_time()이 throtl_rb_first() 결과로 갱신.
	 * 읽는 자: throtl_schedule_next_dispatch()가 jiffies와 비교해 아직
	 *   미래인지 확인 후 throtl_schedule_pending_timer()에 전달.
	 * 값 범위: jiffies 단위 절대 시각. pending_tree가 비어 있을 때는 갱신되지
	 *   않으므로 stale할 수 있으나, 이 경우 nr_pending==0으로 먼저 걸러진다.
	 * 동기화: queue_lock 보호. */

	struct timer_list	pending_timer;	/* fires on first_pending_disptime */
	/* [한국어] first_pending_disptime 시각에 만료되도록 예약되는 커널
	 * 타이머. 콜백은 throtl_pending_timer_fn()이며, 이 함수가 disptime이
	 * 도래한 tg들을 순회해 bio를 상위 service_queue로 전달하고 필요하면
	 * blk_throtl_dispatch_work_fn() workqueue를 깨워 실제 발행까지 이어진다.
	 * 붙잡아 둔 bio를 다시 흐르게 하는 유일한 시동 장치이며, 이것이 없으면
	 * 제한에 걸린 bio는 다음 bio가 제출될 때까지 아무도 깨워 주지 않는다.
	 * sq마다 타이머는 최대 하나만 살아 있다(mod_timer로 시각만 갱신).
	 * 설정자: throtl_service_queue_init()에서 timer_setup()으로 핸들러 등록,
	 *   throtl_schedule_pending_timer()에서 mod_timer()로 만료 시각 갱신.
	 * 읽는 자: 커널 타이머 서브시스템이 만료 시 콜백을 softirq 컨텍스트에서
	 *   호출.
	 * 값 범위: 활성/비활성 여부는 timer_pending()으로 확인 가능.
	 * 동기화: 타이머 자체는 커널 타이머 휠이 관리하며, 콜백 내부에서는
	 *   queue_lock을 직접 획득해 tg/sq 상태를 조작한다. throtl_pd_free()가
	 *   timer_delete_sync()로 동기적으로 정지시켜 use-after-free를 방지. */
};

/* [한국어] throtl_grp.flags에 담기는 상태 비트들.
 * 네 비트 모두 원자적 비트 연산이 아니라 평범한 OR/AND-NOT 대입으로 다뤄지며,
 * 그 안전성은 전적으로 queue_lock에 의존한다. 성격이 둘로 나뉘는데,
 * PENDING/CANCELING은 지속 상태이고 WAS_EMPTY/IOPS_WAS_EMPTY는 세우고 곧
 * 소비해 지우는 1회성 신호다. */
enum tg_state_flags {
	THROTL_TG_PENDING		= 1 << 0,	/* on parent's pending tree */
	/* [한국어] 이 tg가 현재 부모 service_queue의 pending_tree(RB 트리)에
	 * 삽입되어 있어, disptime이 되면 디스패치될 예정임을 나타낸다.
	 * 트리에 없는 노드를 rb_erase하면 엉뚱한 링크를 건드리게 되므로,
	 * 삽입/삭제 양쪽의 가드 역할을 겸한다.
	 * 설정: throtl_enqueue_tg()가 pending_tree에 처음 삽입할 때 OR로 설정.
	 * 해제: throtl_dequeue_tg()가 pending_tree에서 제거할 때 AND-NOT으로
	 *   클리어 (tg의 두 방향 큐가 모두 비었거나 flush될 때 호출됨).
	 * 값 범위: 비트 0. 설정된 동안에만 tg->rb_node가 유효한 트리 멤버.
	 * 동기화: queue_lock 보호 하에 tg->flags 전체를 읽고 쓴다 (개별 비트에
	 *   대한 원자적 연산이 아니라 일반 OR/AND-NOT 대입). */

	THROTL_TG_WAS_EMPTY		= 1 << 1,	/* bio_lists[] became non-empty */
	/* [한국어] 이 tg의 READ 또는 WRITE 큐(sq_queued() 기준)가 비어 있다가
	 * 방금 첫 bio가 추가되었음을 표시하는 일회성 플래그. disptime이 아직
	 * 과거 값(비어 있던 시절의 값)일 수 있으므로, 다음 tg_update_disptime()
	 * 호출에서 반드시 재계산이 필요함을 알리는 용도다.
	 * 설정: throtl_add_bio_tg()가 sq_queued(sq, rw) == 0인 상태에서 bio를
	 *   추가할 때 설정.
	 * 해제: tg_update_disptime()이 disptime을 재계산한 직후 항상 클리어
	 *   (설정 → 소비 → 해제의 1회성 신호 패턴).
	 * 값 범위: 비트 1. 두 방향(READ/WRITE) 공용 플래그이므로 어느 한쪽만
	 *   비었다가 채워져도 설정된다.
	 * 동기화: queue_lock 보호. */

	/*
	 * The sq's iops queue is empty, and a bio is about to be enqueued
	 * to the first qnode's bios_iops list.
	 */
	THROTL_TG_IOPS_WAS_EMPTY	= 1 << 2,
	/* [한국어] bps 큐에서 iops 큐로 막 넘어오는 분할(split) bio가, 해당
	 * 방향의 iops 큐에서 사실상 첫 bio가 되는 경우에 설정되는 플래그.
	 * bps/iops 큐가 분리되어 있어(struct throtl_qnode 참고) WAS_EMPTY만으로는
	 * "iops 큐 관점에서 새로 비었다가 채워짐"을 놓칠 수 있어 별도로 둔다.
	 * 설정: throtl_add_bio_tg()에서 bio가 BIO_BPS_THROTTLED 플래그(=bps는
	 *   이미 통과)를 가지고 있고 동시에 해당 큐의 peek 결과 첫 bio일 때 설정.
	 * 해제: tg_update_disptime()이 WAS_EMPTY와 함께 항상 같이 클리어.
	 * 값 범위: 비트 2.
	 * 동기화: queue_lock 보호. */

	THROTL_TG_CANCELING		= 1 << 3,	/* starts to cancel bio */
	/* [한국어] blk_throtl_cancel_bios()(장치 제거/컨트롤러 teardown 등으로
	 * 더 이상 정상적인 rate-limit 스케줄링이 의미 없어졌을 때) 경로에서
	 * 설정되어, 이후 rate-limit 판단 함수들이 "즉시 통과"로 단축 응답하게
	 * 만드는 플래그. tg_within_bps_limit()/tg_within_iops_limit()이 이
	 * 플래그를 보면 실제 bps/iops 계산을 생략하고 대기 시간 0을 반환한다.
	 * 설정: tg_flush_bios()가 취소 경로 진입 시 1회 설정 (이미 설정되어
	 *   있으면 중복 flush를 막기 위해 조기 반환).
	 * 해제: 명시적으로 해제하는 코드 경로 없음 - tg 자체가 throtl_pd_free()
	 *   로 소멸될 때 함께 사라진다 (일단 취소가 시작되면 그 tg는 다시 정상
	 *   상태로 되돌아가지 않음).
	 * 값 범위: 비트 3.
	 * 동기화: queue_lock 보호. */
};

/*
 * [한국어] struct throtl_grp 개요 - (cgroup, 디스크) 한 쌍의 스로틀 상태:
 *  - pd: blk-cgroup 정책 프레임워크의 공용 헤더. 반드시 첫 멤버여야
 *    container_of 기반 변환(pd_to_tg/blkg_to_tg)이 성립한다.
 *  - rb_node: 부모 service_queue의 pending_tree에 매달릴 때 쓰는 노드.
 *  - td: 이 그룹이 속한 throtl_data. gendisk 하나(= request_queue 하나)에
 *    대응하며, 장치 종류와는 무관하다.
 *  - service_queue: 이 cgroup이 '부모'로서 자식들의 bio를 받아 두는 큐이자,
 *    이 cgroup에 직접 도착한 bio가 대기하는 곳.
 *  - qnode_on_self[2]/qnode_on_parent[2]: 자기 큐용/부모 큐용 대기 슬롯.
 *    슬롯을 분리해야 부모가 자식별로 돌아가며 꺼내는 round-robin이 된다.
 *  - disptime: 이 그룹이 다음 bio를 낼 수 있게 되는 절대 시각(jiffies).
 *    pending_tree의 정렬 키이며, 최좌단 그룹의 값이 타이머 만료 시각이 된다.
 *  - flags: THROTL_TG_PENDING 등 상태 비트(위 enum 참고).
 *  - has_rules_bps[2]/has_rules_iops[2]: READ/WRITE 방향에 bps/IOPS 규칙이
 *    있는지 표시. 있을 때만 blk_should_throtl()에서 제한을 검사한다.
 *  - bps[2]/iops[2]: 설정된 bps/IOPS 상한값. 하위 계층으로의 바이트/IO
 *    유입률을 이 값 이하로 억제한다.
 *  - bytes_disp[2]/io_disp[2]: 현재 슬라이스에서 이미 디스패치된
 *    바이트/IO 수. 설정이 변경되면 carryover를 음수로 반영하여 새로운
 *    bps/iops 기준에서 대기 시간을 재계산한다.
 *  - last_check_time: 현재 코드에서 아무 곳에서도 읽거나 쓰지 않는 필드
 *    (해당 필드 주석 참고).
 *  - slice_start[2]/slice_end[2]: 현재 제한 윈도우의 시작/종료 시점.
 *    예산은 저장되지 않고 "(jiffies - slice_start) × 상한 - 사용량"으로
 *    그때그때 계산된다.
 *  - stat_bytes/stat_ios: blk-cgroup-rwstat 기반 통계. cgroupfs로 노출되는
 *    조회용 값이며 rate 판정에는 쓰이지 않는다.
 * 필드별 상세(설정자/읽는 자/값 범위/동기화)는 각 필드 아래 개별 주석 참고.
 */
struct throtl_grp {
	/* must be the first member */
	/* [한국어] 아래 pd는 반드시 첫 멤버여야 한다 - blk-cgroup 코어는 이
	 * 구조체를 pd 주소로만 다루고, pd_to_tg()는 오프셋 0을 전제로 원래
	 * 구조체를 되찾기 때문이다. 순서를 바꾸면 조용히 잘못된 주소를 얻는다. */
	struct blkg_policy_data pd;
	/* [한국어] blk-cgroup 정책 프레임워크의 공용 policy-data 헤더.
	 * struct throtl_grp을 blkcg_policy_throtl에 대한 per-blkg private
	 * data로 등록하기 위한 임베딩 필드이며, "must be the first member"라는
	 * 원본 주석대로 반드시 구조체의 첫 필드여야 pd_to_tg()의
	 * container_of() 변환이 올바르게 동작한다.
	 * 설정자: throtl_pd_alloc()이 blkcg_policy_throtl.pd_alloc_fn으로
	 *   호출되어 tg 전체(pd 포함)를 kzalloc_node()로 할당.
	 * 읽는 자: pd_to_tg()가 container_of(pd, struct throtl_grp, pd)로
	 *   역변환; blkg_to_pd()/blkg_to_tg()가 blkcg_gq → pd → tg 경로로 사용.
	 * 값 범위: blkg_policy_data 자체의 내부 필드(blkg, plid 등)는
	 *   blk-cgroup.c가 관리하며 이 파일에서는 직접 건드리지 않는다.
	 * 동기화: blkg 생명주기와 함께 관리되며, blkg 참조 카운트로 보호. */

	/* active throtl group service_queue member */
	struct rb_node rb_node;
	/* [한국어] 부모 service_queue의 pending_tree(RB 트리)에 이 tg를
	 * 연결하는 노드. disptime을 key로 정렬되어, "다음에 하위 계층으로 bio를
	 * 내보낼 후보"를 시간 순으로 찾는 데 쓰인다.
	 * 설정자: tg_service_queue_add()가 rb_link_node()+rb_insert_color_cached()
	 *   로 삽입.
	 * 읽는 자: throtl_rb_first()가 leftmost 노드를 rb_entry_tg()로 tg로
	 *   변환해 조회.
	 * 값 범위: 트리에 없을 때는 RB_CLEAR_NODE()로 초기화된 상태
	 *   (throtl_pd_alloc()의 최초 상태, 또는 throtl_rb_erase() 직후).
	 *   THROTL_TG_PENDING 플래그와 항상 함께 관리된다.
	 * 동기화: queue_lock 보호. */

	/* throtl_data this group belongs to */
	struct throtl_data *td;
	/* [한국어] 이 tg가 속한 request_queue 전체의 throtl_data(제어 최상위
	 * 컨텍스트)를 가리키는 역참조 포인터. 하나의 gendisk
	 * request_queue에 대응하는 throttle 컨트롤러 인스턴스.
	 * 설정자: throtl_pd_init()이 blkg->q->td로부터 1회 설정 (이후 불변;
	 *   cgroup을 다른 디스크로 옮기는 기능은 없음).
	 * 읽는 자: sq_to_td()가 최상위가 아닌 sq에서 거슬러 올라갈 때, 또는
	 *   throtl_log() 매크로가 blktrace 메시지의 대상 큐를 찾을 때 사용.
	 * 값 범위: 유효한 throtl_data 포인터 (NULL 불가 - pd_init 이후).
	 * 동기화: 불변 포인터이므로 별도 동기화 불필요; 가리키는 대상(td)의
	 *   필드 접근은 queue_lock으로 보호. */

	/* this group's service queue */
	struct throtl_service_queue service_queue;
	/* [한국어] 이 tg 자신이 갖는 service_queue. 자기 자신에게 직접 도착한
	 * bio(qnode_on_self 경유)와, 만약 v2(default hierarchy) 계층 구조에서
	 * 자식 cgroup이 있다면 그 자식들이 디스패치한 bio가 함께 모이는
	 * 대기 지점이다. 이 안의 pending_tree/pending_timer가 자식 cgroup들
	 * 사이의 스케줄링을 담당한다.
	 * 설정자: throtl_service_queue_init()이 throtl_pd_alloc() 시점에
	 *   초기화 (queued[]/pending_tree/pending_timer 등 내부 필드 setup).
	 * 읽는 자: 이 tg가 다른 tg의 parent_sq로 참조되거나
	 *   (blkg_to_tg(blkg->parent)->service_queue), 이 tg 자신의 disptime을
	 *   계산할 때 sq->queued[]를 조회.
	 * 값 범위: struct throtl_service_queue 정의 참고.
	 * 동기화: queue_lock 보호. */

	/*
	 * qnode_on_self is used when bios are directly queued to this
	 * throtl_grp so that local bios compete fairly with bios
	 * dispatched from children.  qnode_on_parent is used when bios are
	 * dispatched from this throtl_grp into its parent and will compete
	 * with the sibling qnode_on_parents and the parent's
	 * qnode_on_self.
	 */
	struct throtl_qnode qnode_on_self[2];
	/* [한국어] READ(=[0])/WRITE(=[1])별로, "이 cgroup에 직접 도착한 bio"를
	 * 담는 qnode. 이 tg 자신의 service_queue(위 service_queue 필드)에
	 * 매달려, 자식 cgroup들이 올려보낸 bio와 동일한 자격으로 round-robin에
	 * 참여한다 (로컬 bio가 자식 트래픽에 묻히지 않도록 하는 공정성 장치).
	 * 설정자: throtl_pd_alloc()에서 throtl_qnode_init()으로 초기화;
	 *   throtl_add_bio_tg()가 caller가 별도 qnode를 지정하지 않으면
	 *   기본값으로 사용 (&tg->qnode_on_self[rw]).
	 * 읽는 자: throtl_qnode_add_bio()가 bio를 여기 추가하고
	 *   sq->queued[rw]에 등록.
	 * 값 범위: 배열 인덱스는 READ=0, WRITE=1 (bio_data_dir() 반환값과 동일
	 *   convention).
	 * 동기화: queue_lock 보호. */

	struct throtl_qnode qnode_on_parent[2];
	/* [한국어] READ/WRITE별로, "이 tg가 부모 service_queue로 올려보낸 bio"를
	 * 담는 qnode. 부모 입장에서 보면 이 qnode는 형제 tg들의
	 * qnode_on_parent[] 및 부모 자신의 qnode_on_self[]와 동일한 자격으로
	 * 부모의 pending 대기열에서 round-robin 경쟁을 한다.
	 * 설정자: throtl_pd_alloc()에서 throtl_qnode_init()으로 초기화;
	 *   throtl_dispatch_tg()/tg_dispatch_one_bio() 계열이 이 tg의 bio를
	 *   부모에게 넘길 때 &tg->qnode_on_parent[rw]를 사용
	 *   (throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg)).
	 * 읽는 자: 부모 tg(또는 최상위 td)의 service_queue.queued[rw]를 통해
	 *   부모 쪽 throtl_peek_queued()/throtl_pop_queued()가 조회.
	 * 값 범위: 배열 인덱스는 READ=0, WRITE=1.
	 * 동기화: queue_lock 보호. */

	/*
	 * Dispatch time in jiffies. This is the estimated time when group
	 * will unthrottle and is ready to dispatch more bio. It is used as
	 * key to sort active groups in service tree.
	 */
	unsigned long disptime;
	/* [한국어] 이 tg가 다음 bio를 하위 계층으로 내보낼 수 있게 되는
	 * 예상 시각(jiffies 절대값). tg_dispatch_time()이 계산한
	 * "READ/WRITE 중 더 짧은 대기 시간"을 현재 jiffies에 더해 산출하며,
	 * 부모 service_queue.pending_tree의 정렬 key로 쓰인다.
	 * 설정자: tg_update_disptime()이 유일한 설정 지점.
	 * 읽는 자: tg_service_queue_add()가 RB 트리 삽입 위치 비교에,
	 *   update_min_dispatch_time()이 부모의 first_pending_disptime 갱신에
	 *   사용.
	 * 값 범위: jiffies 절대 시각. 현재보다 과거면 "이미 디스패치 가능"을
	 *   의미 (throtl_select_dispatch()가 time_after(jiffies, disptime)로
	 *   판단).
	 * 동기화: queue_lock 보호. */

	unsigned int flags;
	/* [한국어] enum tg_state_flags(본 헤더 상단)에 정의된 비트 플래그들의
	 * 조합. THROTL_TG_PENDING/WAS_EMPTY/IOPS_WAS_EMPTY/CANCELING 각각의
	 * 의미는 해당 enum 정의 옆 주석 참고.
	 * 설정자/해제자: throtl_enqueue_tg()/throtl_dequeue_tg()(PENDING),
	 *   throtl_add_bio_tg()/tg_update_disptime()(WAS_EMPTY,
	 *   IOPS_WAS_EMPTY), tg_flush_bios()(CANCELING).
	 * 읽는 자: 위 각 함수 및 tg_within_bps_limit()/tg_within_iops_limit()
	 *   (CANCELING 검사).
	 * 값 범위: enum tg_state_flags 비트의 OR 조합.
	 * 동기화: queue_lock 보호 하에 OR/AND-NOT으로 조작 (원자적
	 *   비트연산이 아닌 일반 정수 연산). */

	/* are there any throtl rules between this group and td? */
	bool has_rules_bps[2];
	/* [한국어] READ(=[0])/WRITE(=[1])별로, 이 tg 자신 또는 조상(부모) 중
	 * 누군가에게 bps 제한이 설정되어 있는지를 나타내는 캐시된 불리언.
	 * false면 blk_should_throtl()(본 헤더 하단)이 bps 검사 자체를
	 * 생략하고 곧바로 스로틀 검사를 통과시킨다 (제한이 전혀 없는 cgroup의
	 * 오버헤드를 없애기 위한 최적화).
	 * 설정자: tg_update_has_rules()가 tg_bps_limit(tg, rw) != U64_MAX
	 *   또는 부모의 has_rules_bps[rw]를 OR 조건으로 계산.
	 * 읽는 자: blk_should_throtl()(이 헤더), tg_within_bps_limit() 등.
	 * 값 범위: bool[2]; true면 반드시 bps 검사를 거쳐야 함.
	 * 동기화: queue_lock 보호 하에 tg_conf_updated()/throtl_pd_online()
	 *   경로에서만 갱신되고, 읽기는 그 외 언제든 가능 (구성 변경이 드문
	 *   경로이므로 매 bio마다 락 없이 읽는 것을 허용). */

	bool has_rules_iops[2];
	/* [한국어] READ/WRITE별로 iops 제한 유무를 나타내는 캐시. 의미와
	 * 갱신/사용 방식은 has_rules_bps[2]와 동일하되 iops 기준
	 * (tg_iops_limit(tg, rw) != UINT_MAX)으로 계산된다는 점만 다르다.
	 * blk_should_throtl()은 iops 규칙을 "항상 우선 검사"하므로, 이 필드가
	 * true면 bps 규칙 유무와 무관하게 즉시 throttle 대상으로 판정된다.
	 * 설정자/읽는 자/동기화: has_rules_bps[2] 설명과 동일. */

	/* bytes per second rate limits */
	uint64_t bps[2];
	/* [한국어] READ(=[0])/WRITE(=[1])별로 설정된 초당 바이트 수 상한.
	 * 이 방향으로 하위 계층에 내려보내는 데이터의 초당 총량을
	 * 이 값 이하로 억제하겠다는 사용자 설정값이다.
	 * 설정자: tg_set_conf()가 cgroupfs(io.max, blkio.throttle.*)에 쓰인
	 *   값을 sscanf()로 파싱해 이 필드에 직접 대입 (of_cft(of)->private
	 *   오프셋을 통한 포인터 산술로 bps[READ] 또는 bps[WRITE] 중 하나를
	 *   선택).
	 * 읽는 자: tg_bps_limit()이 v2 root cgroup 특수 케이스(U64_MAX 강제)를
	 *   제외하고 이 값을 그대로 반환; tg_dispatch_bps_time()이 실제
	 *   대기 시간 계산에 사용.
	 * 값 범위: 0은 사용자가 U64_MAX(무제한)로 변환해 저장하므로 실제로는
	 *   1 이상이거나 U64_MAX. throtl_pd_alloc()의 초기값은 U64_MAX(무제한).
	 * 동기화: queue_lock 보호 하에서만 변경 (tg_set_conf()가 kernfs write
	 *   경로에서 직접 queue_lock을 잡고 기록). */

	/* IOPS limits */
	unsigned int iops[2];
	/* [한국어] READ/WRITE별로 설정된 초당 IO 개수 상한. bio 크기와 무관하게
	 * 개수만 세므로, 4KB 랜덤 IO와 1MB 순차 IO가 여기서는 똑같이 1로 계산된다.
	 * bps 상한과는 독립적으로 적용되며, 둘 다 걸려 있으면 먼저 막히는 쪽이
	 * 실제 대기 시간을 결정한다.
	 * 설정자/읽는 자/동기화: bps[2]와 동일한 경로(tg_set_conf(),
	 *   tg_iops_limit(), tg_dispatch_iops_time())를 iops 버전으로 사용.
	 * 값 범위: 0은 UINT_MAX(무제한)로 변환되어 저장; 초기값은
	 *   throtl_pd_alloc()에서 UINT_MAX. */

	/*
	 * Number of bytes/bio's dispatched in current slice.
	 * When new configuration is submitted while some bios are still throttled,
	 * first calculate the carryover: the amount of bytes/IOs already waited
	 * under the previous configuration. Then, [bytes/io]_disp are represented
	 * as the negative of the carryover, and they will be used to calculate the
	 * wait time under the new configuration.
	 */
	int64_t bytes_disp[2];
	/* [한국어] READ/WRITE별로 "현재 slice_start[rw]~slice_end[rw] 윈도우
	 * 안에서 이미 하위 계층으로 디스패치된 바이트 수" 누적값. bps 판정
	 * (tg_dispatch_bps_time())이 "bytes_disp[rw] + 이번 bio 크기가
	 * bps_limit * 경과시간/HZ를 넘는가"를 검사하는 기준이 된다.
	 * carryover 매커니즘: 설정이 도중에 바뀌면(__tg_update_carryover())
	 *   "이전 설정 기준으로 이미 손해 본/이득 본 바이트 수"를 계산해, 그
	 *   값의 음수를 bytes_disp[rw]에 넣어둔다. 그러면 새 설정으로 대기
	 *   시간을 계산할 때 이 이력이 자연스럽게 반영된다 (원본 주석 참고).
	 * 설정자: throtl_charge_bps_bio()(+bio 크기), throtl_trim_slice()/
	 *   throtl_start_new_slice_with_credit()/throtl_start_new_slice()(0으로
	 *   리셋 또는 경과분 차감), tg_update_carryover()/tg_set_conf()(carryover
	 *   음수 대입).
	 * 읽는 자: tg_dispatch_bps_time()이 bps 초과 여부 판단에 사용.
	 * 값 범위: int64_t라 carryover 대입 시 일시적으로 음수가 될 수 있음
	 *   (설계상 "빚"을 표현하기 위해 부호 있는 타입 사용).
	 * 동기화: queue_lock 보호. */

	int io_disp[2];
	/* [한국어] bytes_disp[2]의 IOPS 버전 - 현재 slice 윈도우 안에서 이미
	 * 디스패치된 bio(명령) 개수. tg_dispatch_iops_time()의 판정 기준.
	 * 설정자/읽는 자/동기화: bytes_disp[2]와 동일한 이벤트
	 *   (throtl_charge_iops_bio(), throtl_trim_slice() 등)에서 함께
	 *   갱신되며 의미도 동일한 carryover 규칙을 따른다.
	 * 값 범위: int이므로 이 필드도 carryover 대입 시 음수가 될 수 있음. */

	unsigned long last_check_time;
	/* [한국어] 이 저장소의 block/ 전체를 검색해도 이 필드를 읽거나 쓰는 코드가
	 * 없다 - 선언은 여기 하나뿐이고 참조가 전혀 없다(확인 결과). 따라서 현재
	 * 코드에서는 아무 역할도 하지 않으며, 값도 kzalloc 이후 0으로 남는다.
	 * 이름과 타입(jiffies 시각)으로 보아 "마지막으로 rate limit을 점검한 시각"을
	 * 담으려던 것으로 읽히지만, 어느 시점에 왜 남게 되었는지는 이 트리의
	 * 소스만으로는 확인할 수 없다(커밋 이력 확인 필요).
	 * 동기화: (미사용) 사용된다면 queue_lock 보호가 필요할 위치. */

	/* When did we start a new slice */
	unsigned long slice_start[2];
	/* [한국어] READ/WRITE별로 "현재 rate-limit 슬라이스가 시작된 시각"
	 * (jiffies). bps/iops 사용량(bytes_disp/io_disp)은 이 시각부터 누적된
	 * 값이며, 슬라이스가 리셋되면(throtl_trim_slice() 등) 이 값도 함께
	 * 전진한다. "지금 이 순간의 평균 처리율을 계산하는
	 * 분모가 되는 시간 윈도우의 시작점".
	 * 설정자: throtl_start_new_slice()/throtl_start_new_slice_with_credit()
	 *   (새 슬라이스 시작), throtl_trim_slice()(경과된 배수만큼 전진).
	 * 읽는 자: throtl_slice_used()(현재 jiffies가 [slice_start, slice_end]
	 *   범위 안인지 판정), tg_dispatch_bps_time()/tg_dispatch_iops_time()
	 *   (경과 시간 = jiffies - slice_start 계산).
	 * 값 범위: jiffies 절대 시각, 항상 slice_end[rw] 이전이어야 함
	 *   (BUG_ON(time_before(slice_end, slice_start))로 불변조건 검증).
	 * 동기화: queue_lock 보호. */

	unsigned long slice_end[2];
	/* [한국어] READ/WRITE별로 "현재 슬라이스가 끝나는 시각"(jiffies).
	 * DFL_THROTL_SLICE(100ms) 단위로 정렬되어(throtl_set_slice_end()가
	 * roundup()) 슬라이스 경계를 맞춘다. 이 시각이 지나면 슬라이스가
	 * 만료되어 throtl_trim_slice()가 사용량을 재조정한다.
	 * 설정자: throtl_start_new_slice() 계열이 jiffies + DFL_THROTL_SLICE로
	 *   설정, throtl_set_slice_end()/throtl_extend_slice()가 필요 시 연장.
	 * 읽는 자: throtl_slice_used(), throtl_trim_slice()의 만료 판정.
	 * 값 범위: jiffies 절대 시각, slice_start[rw]보다 항상 미래(또는 같음).
	 * 동기화: queue_lock 보호. */

	struct blkg_rwstat stat_bytes;
	/* [한국어] 이 cgroup의 READ/WRITE 방향별 누적 전송 바이트 수 통계
	 * (blk-cgroup-rwstat.h의 blkg_rwstat, per-CPU 카운터 기반). rate-limit
	 * 판정에는 직접 쓰이지 않고, blkio.throttle.io_service_bytes 등
	 * 사용자 공간 조회용 통계 값이다.
	 * 설정자: blk_should_throtl()(본 헤더)이 cgroup v1(non-default
	 *   hierarchy)에서만 BIO_CGROUP_ACCT 플래그로 중복 집계를 막으며
	 *   blkg_rwstat_add()로 bio 크기를 누적.
	 * 읽는 자: cgroupfs의 통계 파일 read 핸들러(blk-throttle.c 안의 show
	 *   함수들, 이 헤더 밖)가 blkg_rwstat 조회 API로 읽어감.
	 * 값 범위: per-CPU 누적값이므로 논리적으로는 단조 증가.
	 * 동기화: blkg_rwstat 내부적으로 percpu_counter 사용, 별도 락 불필요
	 *   (percpu 카운터 자체의 원자성에 의존). */

	struct blkg_rwstat stat_ios;
	/* [한국어] stat_bytes의 IOPS 버전 - READ/WRITE 방향별 누적 IO(명령)
	 * 개수 통계. blk_should_throtl()이 cgroup v1에서 매 bio마다 무조건(중복
	 * 방지 플래그 없이) blkg_rwstat_add(..., 1)로 1씩 누적.
	 * 읽는 자/값 범위/동기화: stat_bytes와 동일. */
};

/* [한국어] 아래 extern은 정의가 아니라 선언이다. 실체(콜백 테이블 전체)는
 * blk-throttle.c에 있고, 이 헤더의 인라인 함수들(blkg_to_tg 등)이
 * blkg_to_pd()/blkcg_policy_enabled()에 "어느 정책인지"를 넘기기 위한
 * 식별자로만 필요하기 때문에 여기서는 이름만 노출한다. */
extern struct blkcg_policy blkcg_policy_throtl;
/* [한국어] blk-throttle 정책을 blkcg(cgroup 블록 IO 컨트롤러) 프레임워크에
 * 등록하기 위한 blkcg_policy 서술자. 실제 정의와 pd_alloc_fn/pd_init_fn/
 * pd_online_fn/pd_free_fn 등 콜백 연결은 blk-throttle.c에서 이루어지고
 * (module_init(throtl_init) → blkcg_policy_register()), 이 헤더에서는
 * pd_to_tg()/blkg_to_tg()/blk_throtl_activated()가 blkg_to_pd()/
 * blkcg_policy_enabled() 호출 시 "어떤 정책인지"를 식별하는 키로 사용하기
 * 위해 extern 선언만 노출한다. */

/*
 * [한국어]
 * pd_to_tg - blkg_policy_data 포인터를 감싸는 throtl_grp으로 역변환한다.
 *
 * @pd: blkcg_policy_throtl에 대해 할당된 blkg_policy_data 포인터. 반드시
 *      struct throtl_grp의 pd 필드(구조체의 첫 멤버)를 가리켜야 하며, 다른
 *      정책(blkcg_policy)의 blkg_policy_data를 넘기면 컴파일은 되지만 잘못된
 *      메모리를 가리키게 되므로 호출자가 정책 종류를 보장해야 한다.
 * @return: pd가 NULL이 아니면 container_of()로 계산한 throtl_grp 포인터,
 *          pd가 NULL이면 NULL 그대로 반환.
 *
 * struct throtl_grp은 pd를 첫 멤버로 내장하므로(위 struct throtl_grp의 pd
 * 필드 주석 참고), pd의 주소에서 구조체 오프셋(0)만큼 역산하면 바로
 * throtl_grp의 시작 주소가 나온다. blkcg 정책 프레임워크는 각 정책을
 * blkg_policy_data라는 공통 타입으로 다루므로, 이 함수는 그 공통 타입에서
 * blk-throttle 전용 타입으로 좁혀 들어가는 통로 역할을 한다.
 * 실행 컨텍스트: 인라인 함수이며 락을 요구하지 않는다 (포인터 산술만 수행).
 * 호출하는 쪽(blkg_to_tg())이 자체적으로 동기화 요건을 책임진다.
 * 호출자: blkg_to_tg()가 blkg_to_pd()의 반환값을 바로 이 함수에 넘긴다.
 * 호출 대상: container_of() (컴파일 타임 매크로, 실제 함수 호출 아님).
 * 에러 경로: pd가 NULL이면(정책이 아직 활성화되지 않은 blkg 등) NULL을
 * 그대로 반환해 호출자가 NULL 체크로 처리하게 한다 (여기서 assert하지 않음).
 *
 * 호출 체인:
 *   blkg_to_tg() → [pd_to_tg] → container_of()
 */
static inline struct throtl_grp *pd_to_tg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct throtl_grp, pd) : NULL;
	/* [한국어] 삼항 연산자: pd가 NULL이면 NULL 전파, 아니면 pd가 가리키는
	 * blkg_policy_data가 embedded된 throtl_grp의 시작 주소를 container_of()
	 * 매크로(오프셋 뺄셈)로 계산해 반환. */
}

/*
 * [한국어]
 * blkg_to_tg - blkcg_gq(하나의 cgroup × 하나의 request_queue 조합)로부터
 *              그 cgroup의 throtl_grp을 조회한다.
 *
 * @blkg: 조회 대상 blkcg_gq. 보통 bio->bi_blkg에서 얻어지며, bio가 속한
 *        cgroup과 목적지 request_queue 쌍을 나타낸다.
 * @return: blkg에 연결된 throtl_grp 포인터. blkcg_policy_throtl이 이
 *          request_queue에서 아직 활성화되지 않았다면 blkg_to_pd()가 NULL을
 *          반환하여 결과적으로 이 함수도 NULL을 반환할 수 있다.
 *
 * blk-cgroup 프레임워크는 여러 정책(blk-throttle, blk-iolatency, io.cost 등)
 * 이 같은 blkcg_gq에 각자의 private data를 동시에 붙일 수 있게 설계되어
 * 있다. 이 함수는 그중 blk-throttle 정책(blkcg_policy_throtl, 바로 위의
 * extern 선언)에 해당하는 조각만 뽑아 throtl_grp 타입으로 반환한다.
 * 동작: blkg_to_pd(blkg, &blkcg_policy_throtl)로 해당 정책의
 * blkg_policy_data를 얻고, pd_to_tg()로 최종 타입 변환.
 * 실행 컨텍스트: 인라인, 락 불필요 (blkg 자체의 생명주기는 호출자/blkg
 * refcount가 보장해야 함 - 예: bio->bi_blkg는 bio가 유효한 동안 유효).
 * 호출자: blk_should_throtl()(이 헤더 하단)이 bio->bi_blkg로부터 tg를 얻을 때
 * 사용하는 것이 대표적 사용처.
 * 호출 대상: blkg_to_pd(), pd_to_tg().
 * 에러 경로: 반환값이 NULL일 수 있으므로(정책 미활성) 호출자는 반드시 NULL
 * 체크 후 사용해야 한다 - 다만 blk_should_throtl()의 호출 시점은
 * blk_throtl_activated()로 미리 정책 활성화를 확인한 뒤이므로 실질적으로는
 * NULL이 아님이 보장된다.
 *
 * 호출 체인:
 *   blk_should_throtl() → [blkg_to_tg] → blkg_to_pd(), pd_to_tg()
 */
static inline struct throtl_grp *blkg_to_tg(struct blkcg_gq *blkg)
{
	return pd_to_tg(blkg_to_pd(blkg, &blkcg_policy_throtl));
	/* [한국어] blkg_to_pd(): blkg에서 blkcg_policy_throtl에 해당하는
	 * blkg_policy_data 슬롯을 찾음 (정책 미활성 시 NULL) → pd_to_tg():
	 * 그 pd를 감싸는 throtl_grp으로 역변환. */
}

/*
 * Internal throttling interface
 */
/*
 * [한국어] CONFIG_BLK_DEV_THROTTLING 컴파일 옵션 분기:
 * 이 커널 설정 옵션이 꺼져 있으면 blk-throttle.c 전체가 빌드에서 빠지므로
 * (Kconfig에서 BLK_DEV_THROTTLING 미선택), throtl_data/__blk_throtl_bio() 등
 * 실제 구현이 존재하지 않는다. 그런데도 blk-mq/블록 코어는 blk_throtl_bio()/
 * blk_throtl_exit()/blk_throtl_cancel_bios()를 조건 없이 호출하고 싶어하므로,
 * 아래 #ifndef 분기에서 즉시 반환하는(또는 아무 것도 하지 않는) 동일
 * 시그니처의 인라인 함수를 대신 제공해 호출부의 #ifdef 오염을 막는다.
 * #else 분기는 실제 기능이 켜졌을 때의 진짜 선언/구현이다. */
#ifndef CONFIG_BLK_DEV_THROTTLING
/*
 * [한국어]
 * blk_throtl_exit - (CONFIG_BLK_DEV_THROTTLING=n 빌드) throttle 자원 해제
 * no-op 버전.
 *
 * @disk: 해제 대상 gendisk . 이 빌드에서는 사용하지 않음.
 * @return: 없음 (void)
 *
 * blk-throttle 기능 자체가 커널 설정에서 빠진 빌드에서는 q->td가 존재할 수
 * 없으므로 정리할 자원이 전혀 없다. del_gendisk() 등 호출부가 #ifdef 분기
 * 없이 항상 blk_throtl_exit()를 호출할 수 있도록 동일한 이름의 빈 함수를
 * 제공하는 스텁이다 (CONFIG_BLK_DEV_THROTTLING=y일 때의 진짜 구현은 아래
 * #else 블록의 extern 선언 및 blk-throttle.c 참고).
 * 실행 컨텍스트: 호출자와 동일 (별도 요구사항 없음, 아무 것도 하지 않음).
 * 호출자: del_gendisk() 등 gendisk 해제 경로.
 * 호출 대상: 없음.
 * 에러 경로: 없음 (항상 성공적으로 즉시 반환).
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_exit] (no-op)
 */
static inline void blk_throtl_exit(struct gendisk *disk) { }

/*
 * [한국어]
 * blk_throtl_bio - (CONFIG_BLK_DEV_THROTTLING=n 빌드) bio 제한 없음 no-op
 * 버전.
 *
 * @bio: submit_bio() 경로로 들어온 bio. 이 빌드에서는 사용하지 않음.
 * @return: 항상 false ("이 bio는 throttle되지 않았다" = 곧바로 통과시켜도
 *          된다는 뜻).
 *
 * throttle 기능이 빌드에서 빠졌으므로 모든 bio를 무조건 통과시킨다. 반환값
 * false는 blk_mq_submit_bio() 등 호출자에게 "이 함수가 bio 처리를 이미
 * 끝내지 않았으니 정상적으로 blk-mq 경로를 계속 진행하라"는 신호로 쓰인다
 * (CONFIG_BLK_DEV_THROTTLING=y 버전과 반환값 규약을 동일하게 맞춘 것).
 * 실행 컨텍스트: 호출자와 동일.
 * 호출자: submit_bio() → blk_mq_submit_bio() 경로의 호출부.
 * 호출 대상: 없음.
 * 에러 경로: 없음 (실패 개념이 없는 함수).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() → [blk_throtl_bio] (no-op, false 반환)
 *     → 곧바로 request 할당과 드라이버 제출로 진행
 */
static inline bool blk_throtl_bio(struct bio *bio) { return false; }

/*
 * [한국어]
 * blk_throtl_cancel_bios - (CONFIG_BLK_DEV_THROTTLING=n 빌드) 대기 bio 취소
 * no-op 버전.
 *
 * @disk: 취소 대상 gendisk. 이 빌드에서는 사용하지 않음.
 * @return: 없음 (void)
 *
 * 진짜 구현(CONFIG_BLK_DEV_THROTTLING=y)에서는 장치 제거/컨트롤러 리셋 시
 * throtl 큐에 대기 중인 bio들을 강제로 흘려보내는 역할을 하지만, 이 빌드는
 * 애초에 bio를 큐잉하지 않으므로(blk_throtl_bio()가 항상 false) 취소할
 * 대상 자체가 없다.
 * 실행 컨텍스트: 호출자와 동일.
 * 호출자: 장치 제거/quiesce 경로 (예: del_gendisk() 전 정리 단계).
 * 호출 대상: 없음.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (장치 제거 경로) → [blk_throtl_cancel_bios] (no-op)
 */
static inline void blk_throtl_cancel_bios(struct gendisk *disk) { }
#else /* CONFIG_BLK_DEV_THROTTLING */
/*
 * [한국어]
 * blk_throtl_exit - gendisk에 연결된 blk-throttle 상태(throtl_data)를
 * 해제한다 (선언; 실제 구현은 blk-throttle.c).
 *
 * @disk: 해제 대상 gendisk (디스크 전체; 파티션 단위가 아니다).
 * @return: 없음 (void)
 *
 * del_gendisk()가 디스크를 시스템에서 제거할 때 호출되어, 이 disk의
 * request_queue에 붙어 있던 throtl_data(q->td), 그 안의 pending_timer,
 * dispatch_work를 정리한다. blkg_destroy_all()이 먼저 정책을 비활성화하고
 * 남은 자원만 정리하는 역할이라, q->td가 애초에 없으면(정책을 한 번도 켠
 * 적 없는 장치) 곧바로 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (del_gendisk() 호출 경로); 내부에서
 * timer_delete_sync()/cancel_work_sync() 계열을 쓰므로 슬립 가능.
 * 호출자: del_gendisk() (블록 장치 제거 공통 경로).
 * 호출 대상: timer_delete_sync(), throtl_shutdown_wq(), kfree() (모두
 *   blk-throttle.c 내부).
 * 에러 경로: 실패를 반환하는 함수가 아님 (void); q->td가 NULL이면 아무
 *   일도 하지 않고 반환.
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_exit] (blk-throttle.c 구현)
 *     → timer_delete_sync(), throtl_shutdown_wq(), kfree()
 */
void blk_throtl_exit(struct gendisk *disk);

/*
 * [한국어]
 * __blk_throtl_bio - bio가 실제로 throtl 계층을 통과할 수 있는지 판단하고,
 * 필요하면 큐잉·디스패치 타이머까지 예약한다 (선언; 실제 구현은
 * blk-throttle.c).
 *
 * @bio: 검사 대상 bio. blk_should_throtl()이 이미 "검사가 필요하다"고
 *       판단한 뒤에만 이 함수로 넘어온다.
 * @return: true면 이 bio는 (전부 또는 일부 경로가) throtl 큐에 보관되어
 *          호출자가 더 이상 진행시키면 안 됨을 의미; false면 모든 tg의
 *          제한을 즉시 통과해 blk-mq 경로를 계속 진행해도 됨을 의미.
 *
 * bio가 속한 cgroup의 리프(leaf) throtl_grp에서 시작해 조상 방향으로
 * 한 단계씩 bps/iops 제한을 검사하며, 어느 레벨에서든 대기가 필요하면
 * 해당 tg의 service_queue에 bio를 넣고 pending_timer/disptime을 갱신한다.
 * 모든 레벨을 통과하면 bio에 BIO_BPS_THROTTLED를 표시해 다시 검사받지
 * 않게 한 뒤 그대로 진행시킨다.
 * 실행 컨텍스트: submit_bio() 경로의 프로세스/커널 스레드 컨텍스트;
 * 내부적으로 request_queue의 queue_lock(spinlock)을 잡고 tg 상태를 조작.
 * 호출자: blk_throtl_bio()(이 헤더의 static inline, blk_should_throtl()이
 *   true를 반환했을 때만 호출).
 * 호출 대상(blk-throttle.c 내부): throtl_add_bio_tg(), tg_update_disptime(),
 *   throtl_schedule_next_dispatch() 등.
 * 에러 경로: 이 함수 자체는 실패를 반환하지 않음(bool은 "지연 여부"만
 *   의미); 메모리 할당 실패 등은 이 경로에는 존재하지 않는다 (bio 큐잉은
 *   이미 할당된 tg/qnode 자료구조만 사용).
 *
 * 호출 체인:
 *   blk_throtl_bio() → [__blk_throtl_bio] (blk-throttle.c 구현)
 *     → throtl_add_bio_tg(), tg_update_disptime(),
 *       throtl_schedule_next_dispatch()
 *   [지연된 bio는 이후 blk_throtl_dispatch_work_fn()을 거쳐
 *    submit_bio_noacct_nocheck() → blk_mq_submit_bio()로 재진입]
 */
bool __blk_throtl_bio(struct bio *bio);

/*
 * [한국어]
 * blk_throtl_cancel_bios - 이 gendisk에 대해 throtl 큐에 대기 중인 모든
 * bio를 강제로 흘려보낸다 (취소 경로; 선언, 실제 구현은 blk-throttle.c).
 *
 * @disk: 대상 gendisk. 컨트롤러 리셋, 갑작스러운 장치 분리, 디스크
 *        teardown 등으로 더 이상 정상적인 rate-limit 스케줄링을 기다릴 수
 *        없는 상황에서 호출된다.
 * @return: 없음 (void)
 *
 * 정상적으로는 disptime까지 기다려 bps/iops 규칙을 지키며 bio를 내보내지만,
 * 장치가 곧 사라지는 상황에서는 그 대기가 오히려 hang의 원인이 된다. 이
 * 함수는 각 throtl_grp에 THROTL_TG_CANCELING 플래그를 설정해 이후의 rate
 * 계산을 건너뛰게 하고, 큐에 쌓여 있던 bio를 즉시 상위/최종 경로로 흘려
 * 보낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (장치 제거/에러 처리 경로);
 * queue_lock을 잡고 진행.
 * 호출자: del_gendisk() 및 그것을 부르는 드라이버의 장치 제거 경로
 *   (남은 IO를 정리하는 단계, 이 헤더 밖).
 * 호출 대상(blk-throttle.c 내부): tg_flush_bios() 등.
 * 에러 경로: 없음 (실패 없이 항상 큐를 비움).
 *
 * 호출 체인:
 *   (장치 제거/리셋 경로) → [blk_throtl_cancel_bios] (blk-throttle.c 구현)
 *     → tg_flush_bios() → throtl_pop_queued() 등으로 강제 배출
 */
void blk_throtl_cancel_bios(struct gendisk *disk);

/*
 * [한국어]
 * blk_throtl_activated - 이 request_queue에서 blk-throttle 정책이 실제로
 * 활성화되어 있는지 확인한다.
 *
 * @q: 확인 대상 request_queue.
 * @return: true면 이 큐의 bio들이 blk_should_throtl()/__blk_throtl_bio()의
 *          검사 대상이 됨; false면 throttle 모듈이 아예 붙어 있지 않거나
 *          정책이 꺼져 있어 무조건 통과.
 *
 * blk-throttle은 cgroupfs에 처음 bps/iops 값이 쓰일 때(tg_set_conf() →
 * blk_throtl_init())에야 비로소 q->td가 할당되고 blkcg_policy_throtl이
 * 활성화된다. 그 전까지는 이 큐에 대한 throttle 검사 자체가 무의미하므로,
 * 이 함수는 "검사를 아예 생략해도 되는가"를 빠르게 판정하는 fast-path
 * 게이트 역할을 한다.
 * 동작: q->td가 NULL이 아니어야 하고(모듈/자료구조가 준비됨), 동시에
 * blkcg_policy_enabled()가 이 큐에서 blkcg_policy_throtl이 실제로 켜져
 * 있다고 확인해야 true.
 * 실행 컨텍스트: 인라인, submit_bio() 경로의 어떤 컨텍스트에서도 호출
 * 가능; q->td는 초기화 이후 불변 포인터라 락 없이 읽어도 안전.
 * 호출자: blk_should_throtl()(이 헤더 하단)이 매 bio 제출 시 가장 먼저
 *   확인.
 * 호출 대상: blkcg_policy_enabled() (blk-cgroup.c).
 * 에러 경로: 없음 (단순 조건 판정).
 *
 * 호출 체인:
 *   blk_should_throtl() → [blk_throtl_activated] → blkcg_policy_enabled()
 */
static inline bool blk_throtl_activated(struct request_queue *q)
{
	/*
	 * q->td guarantees that the blk-throttle module is already loaded,
	 * and the plid of blk-throttle is assigned.
	 * blkcg_policy_enabled() guarantees that the policy is activated
	 * in the request_queue.
	 */
	/* [한국어] q->td != NULL: 이 디스크에 대해 blk_throtl_init()이 한 번이라도 실행됐다는 뜻. */
	return q->td != NULL && blkcg_policy_enabled(q, &blkcg_policy_throtl);
	/* [한국어] 두 조건을 모두 봐야 하는 이유: td 할당과 정책 활성화는 blk_throtl_init() 안에서 순차로 일어나고, 정책 등록이 실패하면 td는 다시 NULL로 되돌려진다. 둘 중 하나만 보면 그 중간 상태를 잘못 판정한다. */
}

/*
 * [한국어]
 * blk_should_throtl - 이 bio가 blk-throttle 검사(및 있다면 대기)를 받아야
 * 하는지 사전 판단하고, 필요한 통계도 함께 누적한다.
 *
 * @bio: 검사 대상 bio. bio->bi_bdev, bio->bi_blkg, bio->bi_opf,
 *       bio->bi_iter.bi_size가 모두 유효해야 한다 (submit_bio() 시점이므로
 *       보장됨).
 * @return: true면 __blk_throtl_bio()로 넘어가 실제 rate-limit 판정을 받아야
 *          함; false면 이 bio에 적용될 bps/iops 규칙이 없거나(제한 자체가
 *          없음) 이미 bps 검사를 통과한 상태이므로 곧바로 진행해도 됨.
 *
 * __blk_throtl_bio()를 호출하는 것 자체에도(락, RB 트리 접근 등) 비용이
 * 있으므로, 이 함수는 그 비용을 지불할 필요가 있는 bio만 걸러내는 저비용
 * 사전 필터다. 동작 순서: ① blk_throtl_activated()로 이 큐 자체에 throttle이
 * 켜져 있는지 확인 (꺼져 있으면 즉시 false) ② blkg_to_tg()로 tg 획득
 * ③ cgroup v1(non-default hierarchy)에서만 stat_bytes/stat_ios에 이 bio의
 * 크기/개수를 누적(BIO_CGROUP_ACCT 플래그로 bio당 1회만 바이트 집계) ④ iops
 * 규칙이 있으면 무조건 true(iops는 항상 재검사) ⑤ bps 규칙이 있고 아직
 * BIO_BPS_THROTTLED가 안 찍혀 있으면 true ⑥ 그 외 false.
 * 실행 컨텍스트: submit_bio() 경로, 인터럽트 컨텍스트 아님; blkg_rwstat_add()
 * 는 percpu 카운터라 락 없이 호출 가능.
 * 호출자: blk_throtl_bio()(바로 아래 함수)가 최초 필터로 호출.
 * 호출 대상: blk_throtl_activated(), blkg_to_tg(), blkg_rwstat_add().
 * 에러 경로: 없음 (판정 실패라는 개념이 없는 순수 조건 함수).
 *
 * 호출 체인:
 *   blk_throtl_bio() → [blk_should_throtl] → blk_throtl_activated(),
 *     blkg_to_tg(), blkg_rwstat_add()
 */
static inline bool blk_should_throtl(struct bio *bio)
{
	struct throtl_grp *tg;		/* [한국어] 이 bio가 속한 cgroup의 스로틀 상태. 아래에서 규칙 유무와 통계 누적에 쓰인다. */
	int rw = bio_data_dir(bio);	/* [한국어] 읽기/쓰기 규칙이 완전히 분리돼 있어 방향을 먼저 확정한다. */

	if (!blk_throtl_activated(bio->bi_bdev->bd_queue)) /* [한국어] 가장 싼 검사를 맨 앞에 둔다 */
		/* [한국어] 이 디스크에 스로틀 계층 자체가 없다(아무도 제한을 건 적이 없다).
		 * 가장 흔한 경우이자 가장 싼 검사라 맨 앞에 둔다 - tg를 얻기도 전에 빠져나간다. */
		return false;

	tg = blkg_to_tg(bio->bi_blkg);	/* [한국어] bio에 붙어 있는 blkg에서 이 정책의 데이터를 꺼낸다. */
	/* v1 cgroup 계층에서만 bio별 통계를 직접 누적한다 (v2는 blkg 기반). */
	if (!cgroup_subsys_on_dfl(io_cgrp_subsys)) {
		/* 동일 bio의 바이트 통계가 중복 집계되지 않도록 플래그를 검사한다. */
		if (!bio_flagged(bio, BIO_CGROUP_ACCT)) {
			bio_set_flag(bio, BIO_CGROUP_ACCT);	/* [한국어] 이 함수는 같은 bio에 대해 여러 번 불릴 수 있다(스로틀에 걸렸다 풀려 재제출되는 경우 등). 플래그가 없으면 그때마다 바이트가 중복 누적된다. */
			blkg_rwstat_add(&tg->stat_bytes, bio->bi_opf, /* [한국어] bi_opf에 방향과 sync 여부가 들어 있어, rwstat이 그 값만 보고 어느 칸에 더할지 정한다. */
					bio->bi_iter.bi_size); /* [한국어] 현재 남은 전송 크기. 분할 전 원본 크기가 아니라 '이 bio가 실제로 나르는 양'이다. */
			/* [한국어] percpu 카운터라 락 없이 더해도 안전하며, 읽을 때 합산한다. */
		}
		/* [한국어] 개수 통계는 중복 방지 플래그 없이 매번 1씩 더한다 - 바이트와 달리
		 * 재제출된 bio도 '한 번 더 제출된 IO'로 세는 것이 v1의 기존 동작이다. */
		blkg_rwstat_add(&tg->stat_ios, bio->bi_opf, 1);
	}

	/* iops limit is always counted */
	/* IOPS 규칙이 있으면 반드시 throtl 판단을 수행한다. */
	if (tg->has_rules_iops[rw])
		/* [한국어] iops는 '조건 없이' 다시 검사한다. bps와 달리 통과 사실을 bio에
		 * 기록해 두는 플래그가 없고, 개수 회계는 bio가 실제로 나갈 때 한 번만
		 * 이뤄지므로 여기서 걸러 버리면 셀 기회를 잃는다. */
		return true;

	/* bps 규칙이 있고 아직 bps 제한 큐를 거치지 않은 bio만 다시 검사한다. */
	if (tg->has_rules_bps[rw] && !bio_flagged(bio, BIO_BPS_THROTTLED))
		/* [한국어] bps는 이미 통과한 bio를 다시 보내면 자기가 낸 사용량 때문에
		 * 스스로 막히는 자기 봉쇄가 생긴다. 그래서 통과 표식이 없는 bio만 통과시킨다. */
		return true;

	return false;
	/* [한국어] iops/bps 규칙이 모두 없거나 bps는 이미 통과했으면, 무거운
	 * __blk_throtl_bio()를 부를 이유가 없다 - 그대로 제출 경로를 계속 간다. */
}

/*
 * [한국어]
 * blk_throtl_bio - submit_bio() 경로에서 blk-throttle을 적용하는 실질적
 * 진입점. 필요할 때만 __blk_throtl_bio()에 위임한다.
 *
 * @bio: 검사 대상 bio.
 * @return: true면 bio가 (전체 또는 일부 경로에서) throtl 큐에 보관되어
 *          호출자가 더 진행시키지 않아야 함; false면 즉시 계속 진행해도
 *          되는 bio (제한이 없거나, 있어도 당장 한도 내).
 *
 * blk_should_throtl()의 저비용 사전 필터를 먼저 통과시켜, 규칙이 아예 없는
 * bio는 __blk_throtl_bio()의 상대적으로 무거운 로직(락 획득, tg 트리 갱신)
 * 을 완전히 건너뛰게 한다. 이 함수 자체는 이 헤더에서 공개되는 대표 API로,
 * blk-mq가 submit_bio() 경로 초입에서 호출한다.
 * 실행 컨텍스트: submit_bio() 호출자와 동일한 프로세스/커널 스레드
 * 컨텍스트.
 * 호출자: submit_bio() → blk_mq_submit_bio().
 * 호출 대상: blk_should_throtl()(이 헤더), __blk_throtl_bio()
 *   (blk-throttle.c, CONFIG_BLK_DEV_THROTTLING=y일 때만 정의).
 * 에러 경로: 없음; 두 경우 모두 bool로 "지연 여부"만 반환.
 *
 * 호출 체인:
 *   submit_bio() → blk_mq_submit_bio() → [blk_throtl_bio]
 *     → blk_should_throtl() (false면 즉시 반환)
 *     → __blk_throtl_bio() (true인 경우에만 호출)
 */
static inline bool blk_throtl_bio(struct bio *bio)
{
	/*
	 * block throttling takes effect if the policy is activated
	 * in the bio's request_queue.
	 */
	/* [한국어] 사전 필터를 통과하지 못했으면 스로틀 판정 없이 그대로 진행시킨다. */
	if (!blk_should_throtl(bio))
		return false;
		/* [한국어] 검사 결과 제한 대상이 아니므로 __blk_throtl_bio() 호출
		 * 없이 곧바로 blk-mq 경로로 진행. */

	/* throtl 계층에서 bio를 큐잉하거나 즉시 디스패치한다. */
	return __blk_throtl_bio(bio);
}
#endif /* CONFIG_BLK_DEV_THROTTLING */

/*
 * [한국어] 헤더 요약
 *
 * - blk-throttle은 submit_bio() → blk_mq_submit_bio() 초입에서, bio가 아직
 *   request로 변환되기 전에 개입해 cgroup별 bps/IOPS 상한을 적용하는 일반
 *   블록 계층 관문이다. 하위 장치 종류에 의존하는 코드는 없다.
 * - struct throtl_grp/service_queue/qnode는 cgroup 트리를 그대로 반영한
 *   대기 구조이며, qnode를 출처별로 분리해 형제 그룹 간 round-robin을 만든다.
 * - blk_should_throtl()은 __blk_throtl_bio()의 락/트리 비용을 지불할 가치가
 *   있는 bio만 걸러내는 저비용 사전 필터다. iops 규칙은 항상 재검사하고,
 *   bps 규칙은 BIO_BPS_THROTTLED가 없을 때만 재검사한다.
 * - 제한을 통과한(또는 대기 후 풀려난) bio는 blk_throtl_dispatch_work_fn()에서
 *   submit_bio_noacct_nocheck()로 제출 경로에 다시 들어간다.
 * - 이 헤더는 blk-cgroup-rwstat.h의 blkg_rwstat를 사용하며, 실제 큐잉/디스패치
 *   로직은 blk-throttle.c에 있다.
 */

#endif
