/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Header file for the BFQ I/O scheduler: data structures and
 * prototypes of interface functions among BFQ components.
 */

/*
 * [한국어 설명] BFQ I/O 스케줄러가 공유하는 핵심 자료구조와 내부 인터페이스 정의 (bfq-iosched.h)
 *
 * === 파일의 역할 ===
 * 이 헤더 파일은 BFQ(Budget Fair Queueing) I/O 스케줄러가 사용하는 모든 핵심
 * 자료구조(bfq_entity, bfq_queue, bfq_group, bfq_data, bfq_sched_data,
 * bfq_service_tree, bfq_io_cq 등)와, block/bfq-iosched.c(메인 로직),
 * block/bfq-wf2q.c(B-WF2Q+ 알고리즘), block/bfq-cgroup.c(cgroup 정책) 세
 * 소스 파일이 공유해야 하는 내부 함수 프로토타입을 정의한다. BFQ는 계층적
 * B-WF2Q+(Budget-based Worst-case Fair Weighted Fair Queueing) 알고리즘으로
 * 프로세스별(bfq_queue) 및 cgroup별(bfq_group) I/O 자원을 예산(budget) 단위로
 * 공정하게 분배하는데, 이 파일은 그 알고리즘이 다루는 모든 상태를 한곳에
 * 정의하여 세 소스 파일이 동일한 자료구조 레이아웃과 잠금(bfqd->lock) 규칙을
 * 공유하도록 보장한다. BFQ는 장치 종류에 의존하지 않는 스케줄러이므로, 이
 * 파일의 자료구조도 특정 드라이버(NVMe/SCSI/virtio 등)의 큐 구조가 아니라
 * "요청을 언제 어느 순서로 드라이버에 넘길지"라는 정책만을 표현한다.
 * 다만 실무적으로 BFQ는 회전 디스크와 단일 큐 성격의 장치에서 주로 쓰이며,
 * 병렬성이 높은 NVMe에서는 bfqd->lock이 장치 단위 전역 락이라 확장성 병목이
 * 되고 idling이 처리량을 깎기 때문에 기본 스케줄러로 선택되지 않는 편이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * BFQ는 블록 계층(block layer)의 I/O 스케줄러(elevator) 중 하나로, blk-mq
 * (멀티큐 블록 계층) 프레임워크 위에서 동작한다. 상위 흐름은 다음과 같다:
 * 응용 프로그램의 read/write 시스템 콜 -> 파일시스템 -> submit_bio() ->
 * blk_mq_submit_bio()에서 스케줄러가 붙어 있으면 bfq_insert_requests()로
 * request가 들어오고, 이후 blk_mq_run_hw_queue() -> bfq_dispatch_request()가
 * 이 헤더에 정의된 bfq_queue/bfq_entity 트리(B-WF2Q+ service_tree)에서 다음에
 * 내보낼 request를 고른다. 선택된 request는 blk_mq_dispatch_rq_list()를 거쳐
 * 실제 드라이버(NVMe의 경우 nvme_queue_rq() -> nvme_submit_cmd()의 SQ tail
 * doorbell 기록)로 전달된다. 이 헤더는 세 .c 파일이 컴파일 타임에 include하여
 * 동일한 타입 정의를 공유하는 지점이며, 실행 컨텍스트는 커널 블록 계층
 * 내부로, 대부분 프로세스 컨텍스트(시스템 콜 경로)에서 bfqd->lock(스핀락)을
 * 쥔 채 실행되지만 idle_slice_timer의 hrtimer 콜백처럼 소프트IRQ 컨텍스트에서
 * 진입하는 경로도 있다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 정의하는 자료구조는 blk-cgroup(cgroup 및 blkg/blkcg 정책,
 * blk-cgroup-rwstat.h를 통해 연결), blk-mq(request_queue, struct request),
 * rb-tree(rb_root/rb_node 기반 B-WF2Q+ 서비스 트리), hrtimer(idle_slice_timer),
 * blktrace(bfq_log_bfqq/bfq_log 매크로가 사용하는 blk_add_trace_msg 계열)에
 * 의존한다. 반대로 block/bfq-iosched.c, block/bfq-wf2q.c, block/bfq-cgroup.c는
 * 모두 이 헤더에 의존하며, 이 헤더가 노출하는 함수 프로토타입을 통해서만
 * 서로의 내부 상태(bfq_queue, bfq_entity, bfq_group)에 접근한다. 데이터
 * 흐름 관점에서 보면 bio/request는 bfq_io_cq(태스크당 컨텍스트, sync/async x
 * actuator별 bfq_queue 행렬을 보관) -> bfq_queue(프로세스별 대기열) ->
 * bfq_entity(스케줄링 단위) -> bfq_sched_data의 service_tree(B-WF2Q+ 트리) ->
 * bfq_group(cgroup 계층)까지 위로 올라가며 각 레벨에서 가상 시간(virtual
 * time)과 예산(budget)이 계산된다. 모든 레벨을 관통하는 공유 자료구조는
 * bfq_data(디바이스 전역 상태이자 spinlock의 소유자)이며,
 * bfq_data->rq_in_driver[actuator]는 각 actuator(blk_independent_access_ranges로
 * 노출되는 독립 접근 영역)별로 "이미 스케줄러를 떠나 드라이버에서 처리 중인
 * request 수"를 센다. 이 값은 태그를 아끼기 위한 것이 아니라, injection 한도와
 * hw_tag(장치가 실제로 큐잉을 하는가) 판정의 입력으로 쓰인다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bfq_queue: 하나의 프로세스(또는 협력 프로세스 그룹, 또는 공유
 *   async 큐)가 발생시키는 I/O 요청들의 leaf 큐. LBA 순으로 정렬된 sort_list,
 *   FIFO 만료 리스트, B-WF2Q+ 등록을 위한 entity 필드를 가지며 단일
 *   actuator만을 대상으로 한다.
 * - struct bfq_entity: bfq_queue 또는 bfq_group을 감싸 B-WF2Q+ 트리에
 *   등록하는 스케줄링 단위. start/finish 가상 타임스탬프, weight, budget을
 *   관리하며 F_i = S_i + budget/weight 공식으로 종료 시각을 계산한다.
 * - struct bfq_group: cgroup 계층의 한 (디바이스, cgroup) 노드. 자신의
 *   entity와 sched_data를 가지며 자식 bfq_queue/bfq_group들을 관리한다.
 * - struct bfq_data: 디바이스 전체의 전역 상태. in_service_queue(현재
 *   디스패치 중인 큐), dispatch 리스트, weight-raising 파라미터, 예산
 *   타임아웃, actuator별 in-flight 카운터(rq_in_driver[]) 등을 담는다.
 * - struct bfq_sched_data: RT/BE/IDLE 3개 ioprio_class별 service_tree
 *   배열을 가지는 다단계 스케줄러. bfq_group과 최상위 bfqd 모두 자신의
 *   sched_data를 가져 계층적 B-WF2Q+를 구성한다.
 * - bfq_get_next_queue()/bfq_bfqq_expire(): B-WF2Q+ 트리에서 다음 서비스
 *   대상을 고르고, 현재 서비스 중인 큐를 만료시키는 핵심 진입점으로, 이
 *   헤더가 선언만 제공하고 구현은 bfq-iosched.c/bfq-wf2q.c에 있다.
 */

#ifndef _BFQ_H
#define _BFQ_H

#include <linux/blktrace_api.h> // bfq_log/bfq_log_bfqq 매크로가 쓰는 blk_add_trace_msg() 선언 - BFQ의 내부 결정을 blktrace 스트림에 남기기 위해 필요
#include <linux/hrtimer.h> // idle_slice_timer(다음 요청을 기다리며 장치를 비워 두는 idling 타이머)가 hrtimer이므로 필요

#include "blk-cgroup-rwstat.h" // bfqg_stats가 cgroup별 바이트/IO 수를 담는 blkg_rwstat 타입을 쓰기 때문에 필요

/*
 * BFQ 우선순위 클래스 수: RT/BE/IDLE. ionice(2)의 IOPRIO_CLASS_*와 1:1로
 * 대응하며, bfq_sched_data가 클래스마다 독립된 service_tree를 두는 근거다.
 * 상위 클래스에 활성 entity가 하나라도 있으면 하위 클래스는 서비스되지
 * 않는 엄격한 우선순위 구조이므로(단, IDLE 굶주림 방지 예외 있음),
 * 이 값은 곧 "몇 단계의 엄격한 우선순위 계층을 둘 것인가"를 뜻한다.
 */
#define BFQ_IOPRIO_CLASSES	3 // RT/BE/IDLE 세 개 - 배열 인덱스 0/1/2가 그대로 IOPRIO_CLASS_RT/BE/IDLE에 대응
/*
 * IDLE 클래스가 굶어 죽지 않도록 보장하는 주기. RT/BE가 계속 바쁘면
 * IDLE 클래스는 원리상 영원히 서비스를 못 받으므로, 마지막으로 IDLE을
 * 서비스한 뒤 이 시간이 지나면 강제로 한 번 기회를 준다.
 */
#define BFQ_CL_IDLE_TIMEOUT	(HZ/5) // 200ms - 사람이 체감하는 응답 한계보다 짧으면서도 RT/BE 처리량을 크게 해치지 않는 절충값

/*
 * BFQ entity의 weight(가중치) 허용 범위. 이 범위는 cgroup의 io.weight
 * 파일 및 bfq_group_data->weight의 유효 범위와 정확히 일치해야 하며,
 * bfq_io_set_weight_legacy()/io_weight_write() 같은 sysfs/cgroupfs
 * 입력 검증 코드가 이 상수를 참조한다. 값이 이 범위를 벗어나면 두
 * entity 사이의 비율이 정수 오버/언더플로 없이 계산되지 않는다.
 */
#define BFQ_MIN_WEIGHT			1 // 0을 허용하면 F_i = S_i + budget/weight에서 0으로 나누게 되므로 하한은 반드시 1
#define BFQ_MAX_WEIGHT			1000 // 상한 - weight 합(wsum)과 budget의 곱이 unsigned long 범위 안에 머물도록 제한하는 값
#define BFQ_WEIGHT_CONVERSION_COEFF	10 // ionice 레벨(0~7)을 BFQ weight로 옮길 때 곱하는 배율 - 레벨 하나 차이가 weight 10만큼 벌어지게 해 IOPRIO_BE_NR(8)단계를 [10,80] 구간에 펼친다

/*
 * ioprio가 설정되지 않은 프로세스(대부분의 일반 프로세스)에 적용되는
 * 기본값들. CFQ 스케줄러와 동일한 기본 동작을 유지하기 위해 CFQ가
 * 쓰던 것과 같은 상수를 그대로 채택했다.
 */
#define BFQ_DEFAULT_QUEUE_IOPRIO	4 // IOPRIO_BE_NR(8)단계의 중간값 - ionice를 쓰지 않은 프로세스끼리는 모두 같은 weight를 받아 완전한 공평 분배가 되도록 한 선택

#define BFQ_DEFAULT_GRP_IOPRIO	0 // 그룹 entity의 weight는 cgroup의 io.weight에서 오므로 이 값은 실제로 쓰이지 않는 placeholder
#define BFQ_DEFAULT_GRP_CLASS	IOPRIO_CLASS_BE // 그룹은 클래스 구분 없이 항상 BE 트리에 놓인다 - 클래스 우선순위는 leaf bfq_queue 수준에서만 적용된다

/*
 * bfq_bfqq_name()이 만드는 "bfq<pid><S|A>" 또는 "bfqSHARED-<S|A>"
 * 형태의 문자열을 담을 스택 버퍼 크기. blktrace 메시지 태그로 쓰이므로
 * pid_t의 최대 자릿수(10자) + 접두사/접미사를 여유있게 담을 수 있는
 * 길이로 정해졌다.
 */
#define MAX_BFQQ_NAME_LENGTH 16 // "bfq" + pid 10자리 + 'S'/'A' + NUL을 담기에 충분한 최소 크기 - 스택에 잡는 버퍼이므로 넉넉히 키우지 않는다

/*
 * soft real-time(등시성, 예: 오디오/비디오 재생처럼 일정 주기로 정해진
 * 양만 읽으면 되는 워크로드)로 판정된 큐의 weight를 얼마나 부풀릴지의
 * 배율. 대화형 weight-raising보다 훨씬 큰 값을 쓰는 이유는, 이런 큐는
 * 필요한 대역폭이 작아서 크게 우대해도 다른 큐의 처리량을 거의 빼앗지
 * 않는 반면, 한 번 마감을 놓치면 사용자에게 바로 끊김으로 드러나기
 * 때문이다.
 */
#define BFQ_SOFTRT_WEIGHT_FACTOR	100 // soft-rt 큐의 유효 weight 배율 - 사실상 "다른 큐보다 먼저" 수준으로 끌어올리는 값

/*
 * BFQ가 개별적으로 추적할 수 있는 최대 actuator(독립 접근 영역) 수.
 * 값은 블록 계층의 blk_independent_access_ranges(gendisk->ia_ranges)에서
 * 오며, 실제로 1보다 큰 값을 보고하는 것은 헤드가 물리적으로 둘 이상인
 * 다중 actuator 드라이브다. 대다수 SSD/NVMe는 ia_ranges를 노출하지
 * 않으므로 num_actuators == 1로 동작한다(bfq_actuator_index() 참고).
 * 영역이 분리되어 있으면 서로 다른 영역의 request는 물리적으로 동시에
 * 처리될 수 있으므로, BFQ는 in-flight 카운터와 큐 배열을 영역별로 나눠
 * 한 영역의 혼잡이 다른 영역의 판단을 오염시키지 않게 한다.
 */
#define BFQ_MAX_ACTUATORS 8 // 배열 차원을 정하는 컴파일 타임 상한 - ia_ranges가 이보다 많으면 bfq_init_queue()가 단일 actuator로 폴백한다

struct bfq_entity;

/**
 * struct bfq_service_tree - per ioprio_class service tree.
 *
 * Each service tree represents a B-WF2Q+ scheduler on its own.  Each
 * ioprio_class has its own independent scheduler, and so its own
 * bfq_service_tree.  All the fields are protected by the queue lock
 * of the containing bfqd.
 *
 * NVMe 관점: 각 ioprio_class(RT/BE/IDLE)별로 활성/유휴 entity를 관리하는
 * B-WF2Q+ 스케줄러 트리이다. NVMe에서는 이 트리에서 선택된 bfq_queue의
 * head request가 blk_mq_dispatch_rq_list() -> nvme_queue_rq()를 거쳐
 * SQ로 전달된다. active 트리는 SQ로 아직 디스패치되지 않은 후보
 * 요청들을, idle 트리는 일시 휴식 중인 queue의 entity를 담는다.
 */
struct bfq_service_tree {
	/* tree for active entities (i.e., those backlogged) */
	/* [한국어] backlogged(대기 요청이 있는) entity들의 rb-tree.
	 * 설정자: bfq_activate_entity()/__bfq_requeue_entity() 등 bfq-wf2q.c의
	 *   엔티티 활성화 경로가 rb_root 삽입/삭제를 통해 갱신한다.
	 * 읽는 자: bfq_get_next_queue()가 rb_first()로 최소 start-time
	 *   entity(다음 서비스 후보)를 찾을 때 순회한다.
	 * 값 범위: rb_node로 연결된 bfq_entity 집합, 비어 있으면 RB_ROOT.
	 * 동기화: 이 tree가 속한 bfq_sched_data를 소유한 bfqd->lock으로 보호되며
	 *   락 없이 접근하면 트리 구조가 깨진다(rb-tree는 non-atomic 갱신). */
	struct rb_root active;

	/* tree for idle entities (i.e., not backlogged, with V < F_i)*/
	/* [한국어] 더 이상 backlogged는 아니지만 아직 가상시간 V가 자신의
	 * finish time F_i보다 작아 완전히 제거하지 않고 남겨두는 entity 트리.
	 * 설정자: bfq_forget_idle()/bfq_put_idle_entity()가 만료 조건을 만족하면
	 *   이 트리에서 제거하고, __bfq_deactivate_entity()가 idle로 전이시킬 때
	 *   삽입한다.
	 * 읽는 자: bfq_forget_idle()이 first_idle/last_idle을 기준으로 오래된
	 *   idle entity를 정리할 때 순회한다.
	 * 값 범위: active와 마찬가지로 rb_node 연결 집합, 비어있으면 RB_ROOT.
	 * 동기화: bfqd->lock으로 보호. */
	struct rb_root idle;

	/* idle entity with minimum F_i */
	/* [한국어] idle 트리에서 finish time F_i가 가장 작은(가장 먼저 만료될)
	 * entity에 대한 캐시 포인터. O(1)로 다음에 완전히 정리할 후보를 알기 위해 둔다.
	 * 설정자: idle 트리에 entity를 넣거나 뺄 때 bfq-wf2q.c의 헬퍼가 갱신.
	 * 읽는 자: bfq_forget_idle()이 정리 대상 판단에 사용.
	 * 값 범위: 유효한 bfq_entity 포인터 또는 트리가 비었을 때 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_entity *first_idle;

	/* idle entity with maximum F_i */
	/* [한국어] idle 트리에서 finish time F_i가 가장 큰(가장 나중에 만료될)
	 * entity 캐시 포인터. vtime 갱신 시 새 vtime의 상한을 정하는 데 쓰인다.
	 * 설정자: first_idle과 동일한 삽입/삭제 경로에서 함께 갱신.
	 * 읽는 자: bfq_update_vtime() 계열 함수가 vtime을 너무 크게 전진시키지
	 *   않도록 참조.
	 * 값 범위: 유효한 bfq_entity 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_entity *last_idle;

	/* scheduler virtual time */
	/* [한국어] 이 service_tree(하나의 ioprio_class 레벨)가 진행시키는
	 * B-WF2Q+ 가상 시간. 실제 시간이 아니라 "서비스된 sector/weight" 단위로
	 * 누적되는 논리 시계이며, 각 entity의 start/finish 타임스탬프가 이
	 * vtime을 기준으로 비교되어 다음 서비스 대상이 정해진다.
	 * 설정자: bfq_updated_next_in_service()/bfq_bfqq_served() 경로에서
	 *   서비스가 진행될 때마다 전진.
	 * 읽는 자: bfq_gt()로 start/finish 타임스탬프와 비교하는 모든 B-WF2Q+
	 *   로직(bfq_get_next_queue 등).
	 * 값 범위: 단조 증가하는 64비트 값(오버플로는 실질적으로 발생하지 않음).
	 * 동기화: bfqd->lock. */
	u64 vtime;

	/* scheduler weight sum; active and idle entities contribute to it */
	/* [한국어] 이 service_tree에 속한 active+idle entity들의 weight 합.
	 * 가상 시간 vtime을 전진시킬 때 "전체 대비 이 entity의 몫"을 정규화하는
	 * 분모로 쓰인다.
	 * 설정자: entity가 트리에 들어오거나 나갈 때(bfq_activate_entity,
	 *   __bfq_deactivate_entity 등)마다 weight만큼 가감.
	 * 읽는 자: vtime 전진 계산 시 나눗셈의 분모로 사용.
	 * 값 범위: 0 이상, 이론적 상한은 BFQ_MAX_WEIGHT * (동시 활성 entity 수).
	 * 동기화: bfqd->lock. */
	unsigned long wsum;
};

/**
 * struct bfq_sched_data - multi-class scheduler.
 *
 * bfq_sched_data is the basic scheduler queue.  It supports three
 * ioprio_classes, and can be used either as a toplevel queue or as an
 * intermediate queue in a hierarchical setup.
 *
 * The supported ioprio_classes are the same as in CFQ, in descending
 * priority order, IOPRIO_CLASS_RT, IOPRIO_CLASS_BE, IOPRIO_CLASS_IDLE.
 * Requests from higher priority queues are served before all the
 * requests from lower priority queues; among requests of the same
 * queue requests are served according to B-WF2Q+.
 *
 * The schedule is implemented by the service trees, plus the field
 * @next_in_service, which points to the entity on the active trees
 * that will be served next, if 1) no changes in the schedule occurs
 * before the current in-service entity is expired, 2) the in-service
 * queue becomes idle when it expires, and 3) if the entity pointed by
 * in_service_entity is not a queue, then the in-service child entity
 * of the entity pointed by in_service_entity becomes idle on
 * expiration. This peculiar definition allows for the following
 * optimization, not yet exploited: while a given entity is still in
 * service, we already know which is the best candidate for next
 * service among the other active entities in the same parent
 * entity. We can then quickly compare the timestamps of the
 * in-service entity with those of such best candidate.
 *
 * All fields are protected by the lock of the containing bfqd.
 *
 * NVMe 관점: cgroup 계층의 어느 노드에서나 동일한 3개 클래스를 갖는
 * B-WF2Q+ 스케줄러 인스턴스이다. next_in_service는 현재 서비스 중인
 * entity가 만료됐을 때 곧바로 이어받을 후보를 미리 계산해 두는 캐시로,
 * 만료 시점에 트리를 다시 뒤지지 않아도 되게 해 준다.
 */
struct bfq_sched_data {
	/* entity in service */
	/* [한국어] 이 sched_data 레벨에서 현재 실제로 서비스 중인(디스패치가
	 * 진행 중인) entity. leaf라면 bfq_queue를 감싼 entity, 상위 레벨이라면
	 * 하위 bfq_group을 감싼 entity가 될 수 있다.
	 * 설정자: __bfq_set_in_service_entity()가 새 entity를 선택할 때, 만료
	 *   시 __bfq_bfqd_reset_in_service()가 NULL로 리셋.
	 * 읽는 자: bfq_select_queue()/bfq_dispatch_request()가 현재 무엇이
	 *   드라이버로 나가고 있는지 확인할 때.
	 * 값 범위: 유효 bfq_entity 포인터 또는 아무것도 서비스 중이 아니면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_entity *in_service_entity;

	/* head-of-line entity (see comments above) */
	/* [한국어] in_service_entity가 만료되었을 때 곧바로 이어받을 것으로
	 * 예상되는 activetree 상의 최선 후보. 구조체 상단 주석에 설명된 대로,
	 * 아직 실제로 서비스가 전환되기 전에 후보를 미리 알아두어 타임스탬프
	 * 비교를 빠르게 하기 위한 캐시다.
	 * 설정자: bfq_update_next_in_service()가 active tree 변경 시마다 갱신.
	 * 읽는 자: next_queue_may_preempt() 등 선점 판단 로직, bfq_get_next_queue().
	 * 값 범위: 유효 포인터 또는 활성 entity가 없으면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_entity *next_in_service;

	/* array of service trees, one per ioprio_class */
	/* [한국어] IOPRIO_CLASS_RT/BE/IDLE 3개 클래스마다 독립된 B-WF2Q+
	 * service_tree(위 struct bfq_service_tree 참고)를 두어, 상위 클래스가
	 * 항상 하위 클래스보다 먼저 서비스되도록 강제한다(같은 클래스 내에서만
	 * B-WF2Q+ 공정성이 적용됨).
	 * 설정자: bfq_init_entity()가 entity의 ioprio_class에 따라 어느 인덱스의
	 *   tree를 쓸지 bfq_entity_service_tree()로 결정, 이후 활성화 경로가 삽입.
	 * 읽는 자: bfq_get_next_queue()가 인덱스 0(RT)부터 순서대로 비어있지
	 *   않은 tree를 찾아 그 안에서 다음 서비스 대상을 고른다.
	 * 값 범위: 배열 인덱스는 0(RT)/1(BE)/2(IDLE). 각 원소는 독립적인 rb-tree 쌍.
	 * 동기화: bfqd->lock. */
	struct bfq_service_tree service_tree[BFQ_IOPRIO_CLASSES];

	/* last time CLASS_IDLE was served */
	/* [한국어] IDLE 클래스가 마지막으로 서비스된 jiffies 시각. RT/BE 클래스가
	 * 계속 바쁘면 IDLE 클래스는 원칙적으로 영원히 굶을 수 있는데, 이 값을
	 * BFQ_CL_IDLE_TIMEOUT과 비교해 일정 시간 이상 지나면 강제로 IDLE 클래스에
	 * 서비스 기회를 준다(starvation 방지).
	 * 설정자: IDLE 클래스 entity가 서비스될 때 jiffies로 갱신.
	 * 읽는 자: bfq_get_next_queue()가 클래스 선택 시 IDLE 굶주림 여부 판단.
	 * 값 범위: 부팅 후 경과 jiffies 값, 초기값은 미서비스 상태를 뜻하는 0에 가까움.
	 * 동기화: bfqd->lock. */
	unsigned long bfq_class_idle_last_service;

};

/**
 * struct bfq_weight_counter - counter of the number of all active queues
 *                             with a given weight.
 *
 * NVMe 관점: 동일한 weight를 가진 활성 bfq_queue의 수를 센다. 모든
 * queue의 weight가 동일하면 BFQ는 fairness 보장을 단순화하고, NVMe
 * SQ depth 제한(bfq_limit_depth)을 보수적으로 설정할 필요가 없어진다.
 */
struct bfq_weight_counter {
	unsigned int weight; /* weight of the queues this counter refers to */
	/* [한국어] 이 카운터가 대표하는 weight 값. bfqd->queue_weights_tree에서
	 * 이 값이 rb-tree의 정렬 키가 된다.
	 * 설정자: bfq_weights_tree_add()가 새 weight를 위한 노드를 만들 때 대입.
	 * 읽는 자: bfq_weights_tree_add/remove()가 동일 weight 노드를 찾을 때 비교.
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT] 사이의 값.
	 * 동기화: bfqd->lock. */

	unsigned int num_active; /* nr of active queues with this weight */
	/* [한국어] 동일 weight를 갖고 현재 active(backlogged)한 bfq_queue의 개수.
	 * 모든 큐의 weight가 같으면(즉 tree에 노드가 하나뿐이면) BFQ는 "대칭
	 * 시나리오"로 판단해 depth 제한(bfq_limit_depth) 등 여러 휴리스틱을
	 * 단순화할 수 있다.
	 * 설정자: bfq_weights_tree_add()가 증가, bfq_weights_tree_remove()가 감소.
	 * 읽는 자: bfq_asymmetric_scenario() 계열이 0이 되면 노드를 제거해도 되는지
	 *   판단.
	 * 값 범위: 0 이상. 0이 되면 이 카운터 노드 자체가 트리에서 제거됨.
	 * 동기화: bfqd->lock. */
	/*
	 * Weights tree member (see bfq_data's @queue_weights_tree)
	 */
	struct rb_node weights_node;
	/* [한국어] bfqd->queue_weights_tree(rb_root_cached)에 이 카운터를
	 * 연결하는 rb-tree 노드.
	 * 설정자: bfq_weights_tree_add()가 rb_link_node()/rb_insert_color()로 삽입.
	 * 읽는 자: 트리 순회 시 rb_entry()의 기준 포인터로 사용.
	 * 값 범위: 트리에 속해 있는 동안 유효한 rb_node, 제거 후에는 의미 없음.
	 * 동기화: bfqd->lock. */
};

/**
 * struct bfq_entity - schedulable entity.
 *
 * A bfq_entity is used to represent either a bfq_queue (leaf node in the
 * cgroup hierarchy) or a bfq_group into the upper level scheduler.  Each
 * entity belongs to the sched_data of the parent group in the cgroup
 * hierarchy.  Non-leaf entities have also their own sched_data, stored
 * in @my_sched_data.
 *
 * Each entity stores independently its priority values; this would
 * allow different weights on different devices, but this
 * functionality is not exported to userspace by now.  Priorities and
 * weights are updated lazily, first storing the new values into the
 * new_* fields, then setting the @prio_changed flag.  As soon as
 * there is a transition in the entity state that allows the priority
 * update to take place the effective and the requested priority
 * values are synchronized.
 *
 * Unless cgroups are used, the weight value is calculated from the
 * ioprio to export the same interface as CFQ.  When dealing with
 * "well-behaved" queues (i.e., queues that do not spend too much
 * time to consume their budget and have true sequential behavior, and
 * when there are no external factors breaking anticipation) the
 * relative weights at each level of the cgroups hierarchy should be
 * guaranteed.  All the fields are protected by the queue lock of the
 * containing bfqd.
 *
 * 스케줄링 관점: bfq_queue 또는 bfq_group을 B-WF2Q+ 스케줄러에 등록하는
 * 단위이다. start/finish 타임스탬프는 가상 시간 기반으로 다음에 서비스할
 * 후보 순위를 결정하며, budget/weight 비율이 곧 각 entity가 받는
 * 대역폭 몫(fairness)을 정한다. on_st_or_in_serv 플래그가 true면
 * 해당 entity는 active/idle 트리 또는 현재 서비스 중이므로, NVMe
 * in-flight 요청과 직접 연결될 수 있다.
 */
struct bfq_entity {
	/* service_tree member */
	/* [한국어] 자신이 소속된 bfq_service_tree(active 또는 idle 트리)에
	 * 연결되는 rb-tree 노드. 이 노드가 트리 어디에 위치하느냐가 곧 이
	 * entity가 디스패치될 상대적 순서를 뜻한다.
	 * 설정자: bfq_insert()/bfq_extract() (bfq-wf2q.c)가 rb_link_node/
	 *   rb_erase로 삽입·제거.
	 * 읽는 자: rb_first()/rb_next() 등으로 트리를 순회하는 모든 B-WF2Q+ 로직.
	 * 값 범위: 트리에 속한 동안만 유효, tree 필드가 NULL이면 미사용 상태.
	 * 동기화: bfqd->lock. */
	struct rb_node rb_node;

	/*
	 * Flag, true if the entity is on a tree (either the active or
	 * the idle one of its service_tree) or is in service.
	 */
	/* [한국어] 이 entity가 (active/idle 어느 쪽이든) 트리에 있거나 혹은
	 * 현재 서비스 중임을 나타내는 플래그. false이면 완전히 스케줄러 밖에
	 * 있는(비활성) 상태이다.
	 * 설정자: bfq_activate_requeue_entity()가 true로, __bfq_deactivate_entity()
	 *   가 false로 설정.
	 * 읽는 자: bfq_bfqq_busy() 등 entity의 활성 여부를 묻는 모든 헬퍼.
	 * 값 범위: bool. NVMe in-flight 요청과 직접 연결될 수 있는지 여부의 근거.
	 * 동기화: bfqd->lock. */
	bool on_st_or_in_serv;

	/* B-WF2Q+ start and finish timestamps [sectors/weight] */
	/* [한국어] B-WF2Q+ 가상 시작(start, S_i)/종료(finish, F_i) 타임스탬프.
	 * F_i = S_i + budget/weight 로 계산되며, 트리 안에서 F_i가 작은 순서로
	 * 서비스되는 것이 곧 먼저 디스패치될 우선순위를 뜻한다.
	 * 설정자: bfq_calc_finish()/bfq_activate_entity()가 활성화·서비스 시마다 갱신.
	 * 읽는 자: bfq_gt(entity->finish, ...) 비교로 트리 내 정렬 및 다음 서비스
	 *   대상 선택에 사용.
	 * 값 범위: 소속 service_tree의 vtime과 같은 단위의 단조 증가 값.
	 * 동기화: bfqd->lock. */
	u64 start, finish;

	/* tree the entity is enqueued into; %NULL if not on a tree */
	/* [한국어] 현재 entity가 들어있는 rb_root(active 또는 idle 트리)에 대한
	 * 역참조 포인터. rb_node만으로는 어느 트리에 있는지 알 수 없어 별도로 둔다.
	 * 설정자: 삽입 시 해당 트리의 주소로 설정, 제거 시 NULL로 리셋.
	 * 읽는 자: bfq_entity_service_tree() 등이 소속 확인 시 사용.
	 * 값 범위: 유효한 &service_tree->active/idle 주소 또는 NULL.
	 * 동기화: bfqd->lock. */
	struct rb_root *tree;

	/*
	 * minimum start time of the (active) subtree rooted at this
	 * entity; used for O(log N) lookups into active trees
	 */
	/* [한국어] 이 entity를 루트로 하는 active 서브트리 전체에서 가장 작은
	 * start 값의 캐시. augmented rb-tree 기법으로, 트리 전체를 순회하지
	 * 않고도 O(log N)에 "다음 서비스할 leaf"를 찾기 위한 가속 필드다.
	 * 설정자: bfq_update_min()이 삽입/삭제/회전 시마다 부모 방향으로 재계산.
	 * 읽는 자: bfq_first_active_entity()가 최소 start를 가진 leaf를 찾을 때.
	 * 값 범위: 서브트리 내 min(start) 값.
	 * 동기화: bfqd->lock. */
	u64 min_start;

	/* amount of service received during the last service slot */
	/* [한국어] 가장 최근 서비스 구간(slot) 동안 이 entity가 실제로 소비한
	 * 서비스량(섹터 수 기준). 예산 소진 여부 및 fairness 과금 계산의 입력이다.
	 * 설정자: bfq_bfqq_served()가 request 완료 시마다 누적.
	 * 읽는 자: bfq_bfqq_expire()가 예산 대비 실제 소비량을 비교해 만료 사유를
	 *   판단할 때(BFQQE_BUDGET_EXHAUSTED 등).
	 * 값 범위: 0 이상, budget 근처에서 리셋됨.
	 * 동기화: bfqd->lock. */
	int service;

	/* budget, used also to calculate F_i: F_i = S_i + @budget / @weight */
	/* [한국어] 이 entity(정확히는 대응 bfq_queue)에게 한 서비스 구간 동안
	 * 허용된 예산(섹터 수). 이 큐가 만료되기 전까지 몇 섹터어치를 연속으로
	 * 내보낼 수 있는지를 정하며, F_i = S_i + budget/weight 공식을 통해
	 * 가상 종료 시각에 직접 반영된다. budget이 크면 한 큐가 오래 장치를
	 * 붙들어 처리량에 유리하지만 다른 큐의 대기 시간이 늘어난다.
	 * 설정자: bfq_updated_next_in_service()/__bfq_entity_update_weight_prio()
	 *   등이 peak_rate/과거 소비 패턴을 바탕으로 재계산.
	 * 읽는 자: bfq_calc_finish()가 F_i 계산에 사용, bfq_bfqq_expire()가
	 *   예산 소진 여부 판단에 사용.
	 * 값 범위: 0 초과, bfqd->bfq_max_budget에 의해 상한이 걸림.
	 * 동기화: bfqd->lock. */
	int budget;

	/* Number of requests allocated in the subtree of this entity */
	/* [한국어] 이 entity를 루트로 하는 서브트리(자기 자신 포함, cgroup 계층
	 * 하위 전체) 안에서 현재 할당(디스패치 대기 또는 in-flight)되어 있는
	 * request의 총 개수. cgroup 단위로 진행 중인 request 수를 집계하는 데 쓰인다.
	 * 설정자: request 할당/해제 시 부모 방향으로 +1/-1 전파.
	 * 읽는 자: depth 제한 로직이 그룹 단위 in-flight 개수를 볼 때 참조.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int allocated;

	/* device weight, if non-zero, it overrides the default weight of
	 * bfq_group_data */
	/* [한국어] 특정 디바이스에 대해서만 적용되는 weight override 값.
	 * 0이면 미설정을 뜻하고, bfq_group_data의 기본 weight를 그대로 쓴다.
	 * (같은 cgroup이라도 디바이스마다 다른 weight를 줄 수 있게 하는 여지이나
	 * 현재 커널은 이 기능을 유저스페이스에 완전히 노출하지는 않는다.)
	 * 설정자: cgroup의 per-device weight 설정 경로(io.bfq.weight 등)가 기록.
	 * 읽는 자: bfq_group_set_weight() 등이 유효 weight 계산 시 참조.
	 * 값 범위: 0(미설정) 또는 [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT].
	 * 동기화: bfqd->lock. */
	int dev_weight;
	/* weight of the queue */
	/* [한국어] 이 entity에 현재 적용 중인 유효 weight. B-WF2Q+ 계산
	 * (F_i = S_i + budget/weight)에 직접 쓰이는 값이며, 동일 weight를 가진
	 * bfq_queue들은 bfqd->queue_weights_tree에서 같은 카운터를 공유한다.
	 * 설정자: __bfq_entity_update_weight_prio()가 prio_changed 처리 시,
	 *   또는 weight-raising 전이 시 갱신.
	 * 읽는 자: bfq_calc_finish(), bfq_weights_tree_add/remove().
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT] (또는 WR 배율이 곱해진 값).
	 * 동기화: bfqd->lock. */
	int weight;
	/* next weight if a change is in progress */
	/* [한국어] ioprio/weight 변경이 요청되었지만 아직 반영 시점(entity가
	 * idle->active 등 안전한 상태 전이를 할 때)에 도달하지 않았을 때 잠시
	 * 보관해두는 다음 weight 값. prio_changed 플래그와 함께 사용되는 지연
	 * 적용(lazy update) 패턴이다.
	 * 설정자: ioprio/weight 변경 syscall 경로(bfq_set_next_ioprio_data() 등)가 기록.
	 * 읽는 자: __bfq_entity_update_weight_prio()가 prio_changed 확인 후 weight로 반영.
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT].
	 * 동기화: bfqd->lock. */
	int new_weight;

	/* original weight, used to implement weight boosting */
	/* [한국어] weight-raising(WR, 대화형/soft-RT 워크로드를 위한 일시적
	 * 가중치 상승) 이전의 "진짜" weight. WR 기간이 끝나면 이 값으로 복원되어
	 * 장치 자원 분배 비율이 원래대로 돌아간다.
	 * 설정자: WR이 시작될 때 현재 weight를 이 필드에 저장(bfq_update_bfqq_wr_on_rq_arrival 등).
	 * 읽는 자: WR 종료 처리 로직이 weight 복원 시 참조.
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT].
	 * 동기화: bfqd->lock. */
	int orig_weight;

	/* parent entity, for hierarchical scheduling */
	/* [한국어] cgroup 계층에서 이 entity의 상위 노드에 해당하는 entity.
	 * bfq_queue의 entity라면 자신이 속한 bfq_group의 entity를, bfq_group의
	 * entity라면 상위 bfq_group의 entity를 가리킨다. 장치 자원이 cgroup
	 * 계층을 따라 상속·집계되는 경로가 바로 이 포인터 체인이다.
	 * 설정자: bfq_init_entity()/그룹 이동 함수가 계층 구성 시 설정.
	 * 읽는 자: for_each_entity()/for_each_entity_safe() 매크로가 루트까지
	 *   순회할 때 사용.
	 * 값 범위: 유효 포인터 또는 루트 그룹이면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_entity *parent;

	/*
	 * For non-leaf nodes in the hierarchy, the associated
	 * scheduler queue, %NULL on leaf nodes.
	 */
	struct bfq_sched_data *my_sched_data;
	/* [한국어] 이 entity가 bfq_group을 감싸는 비-leaf 노드일 때, 그 그룹이
	 * 소유한 자신만의 bfq_sched_data(자식 entity들을 위한 스케줄러). leaf인
	 * bfq_queue의 entity에서는 NULL이다.
	 * 설정자: bfq_init_entity()가 그룹 entity 초기화 시 &bfqg->sched_data로 설정.
	 * 읽는 자: bfq_entity_to_bfqq() 등이 leaf/non-leaf 구분에 사용.
	 * 값 범위: non-leaf면 유효 포인터, leaf면 NULL.
	 * 동기화: bfqd->lock. */
	/* the scheduler queue this entity belongs to */
	struct bfq_sched_data *sched_data;
	/* [한국어] 이 entity 자신이 스케줄링되는 대상 sched_data, 즉 parent가
	 * 소유한 my_sched_data와 같은 값(parent->my_sched_data). entity가 어느
	 * B-WF2Q+ 트리 집합에 속하는지를 나타낸다.
	 * 설정자: bfq_init_entity()가 parent로부터 상속.
	 * 읽는 자: bfq_entity_service_tree()가 ioprio_class에 맞는 service_tree를
	 *   고를 때 이 sched_data의 배열을 인덱싱.
	 * 값 범위: 유효한 bfq_sched_data 포인터(루트 그룹 이상에서는 항상 존재).
	 * 동기화: bfqd->lock. */

	/* flag, set to request a weight, ioprio or ioprio_class change  */
	int prio_changed;
	/* [한국어] weight, ioprio, ioprio_class 중 하나라도 변경이 요청되어
	 * 아직 실제로 적용되지 않았음을 나타내는 플래그. 설정되면 다음 안전한
	 * 시점(entity가 idle 상태가 될 때 등)에 __bfq_entity_update_weight_prio()가
	 * new_weight 등을 실제 weight로 반영하고 트리 내 위치를 재조정한다.
	 * 설정자: ioprio/weight 변경 syscall, cgroup weight 변경 경로가 non-zero로 설정.
	 * 읽는 자: bfq_activate_requeue_entity() 등이 확인 후 업데이트 함수 호출.
	 * 값 범위: 0(변경 없음) 또는 non-zero(변경 대기 중).
	 * 동기화: bfqd->lock. */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	/* flag, set if the entity is counted in groups_with_pending_reqs */
	bool in_groups_with_pending_reqs;
	/* [한국어] 이 entity(그룹)가 현재 bfqd->num_groups_with_pending_reqs
	 * 카운트에 반영되어 있는지 나타내는 플래그. 이중 카운트/이중 감소를
	 * 막기 위한 상태 비트다. CONFIG_BFQ_GROUP_IOSCHED가 켜진 빌드에서만
	 * 존재하며, cgroup별 NVMe in-flight 분산 통계에 쓰인다.
	 * 설정자: bfq_add_bfqq_in_groups_with_pending_reqs()가 true로,
	 *   bfq_del_bfqq_in_groups_with_pending_reqs()가 false로 설정.
	 * 읽는 자: 위 두 함수 자신이 재진입/중복 갱신 방지를 위해 확인.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
#endif

	/* last child queue of entity created (for non-leaf entities) */
	struct bfq_queue *last_bfqq_created;
	/* [한국어] 이 entity(그룹)의 자식으로 가장 최근에 생성된 bfq_queue.
	 * 짧은 시간 안에 연속으로 생성된 큐들이 사실 협력(cooperating) 관계일
	 * 가능성을 판단하는 stable-merge 휴리스틱의 입력으로 쓰여, 디스패치 경로를
	 * 하나로 합칠지 예측하는 데 사용된다.
	 * 설정자: 새 bfq_queue가 이 그룹 아래에서 생성될 때 갱신.
	 * 읽는 자: bfq_setup_stable_merge() 계열이 최근 생성 큐와의 시간 간격을 비교.
	 * 값 범위: 유효 포인터 또는 아직 자식이 없으면 NULL.
	 * 동기화: bfqd->lock. */
};

/* [한국어] struct bfq_group의 전방 선언. 실제 정의는 이 헤더 뒤쪽,
 * CONFIG_BFQ_GROUP_IOSCHED 블록 안에 있지만 그보다 앞서 정의되는
 * bfq_entity가 my_sched_data/parent를 통해 그룹을 가리켜야 하므로
 * 여기에서 이름만 먼저 알려 준다. cgroup 지원을 끄고 빌드해도 이
 * 선언은 남지만, 그 경우 정의가 없는 불완전 타입으로만 쓰인다. */
struct bfq_group;

/**
 * struct bfq_ttime - per process thinktime stats.
 *
 * 프로세스가 자신의 요청이 완료된 뒤 다음 요청을 낼 때까지의 think time을
 * 측정한다. ttime_mean이 짧으면 그 프로세스는 완료 직후 곧바로 다음 요청을
 * 낼 가능성이 높다는 뜻이고, 이는 곧 "이 큐를 위해 잠시 장치를 비워 두는
 * idling이 값싸게 성공할 것"이라는 근거가 된다. 반대로 다른 큐의 요청을
 * 끼워 넣는 injection은 그 짧은 간격을 밀어내므로 억제해야 한다.
 */
struct bfq_ttime {
	/* completion time of the last request */
	/* [한국어] 이 프로세스(bfq_queue)의 가장 최근 request가 완료된 시각
	 * (ns, ktime 기준). 다음 request가 도착할 때 이 시각과의 차이가 곧
	 * think time(사용자가 다음 I/O를 내기까지 "생각한" 시간) 샘플이 된다.
	 * 설정자: bfq_completed_request()가 NVMe CQ 완료/블록 계층 완료 콜백에서 갱신.
	 * 읽는 자: bfq_update_io_thinktime()이 새 request 도착 시 차이를 계산할 때.
	 * 값 범위: ktime_get_ns() 단조 시계 값.
	 * 동기화: bfqd->lock. */
	u64 last_end_request;

	/* total process thinktime */
	/* [한국어] 지금까지 관측된 think time 샘플들의 누적 합(ns). ttime_mean을
	 * 구하는 이동평균 계산의 분자 역할을 한다.
	 * 설정자: bfq_update_io_thinktime()이 새 샘플을 지수이동평균 방식으로 반영.
	 * 읽는 자: 평균 재계산 시 자기 자신을 참조.
	 * 값 범위: 0 이상, 실제로는 감쇠(decay)되므로 무한정 커지지 않음.
	 * 동기화: bfqd->lock. */
	u64 ttime_total;
	/* number of thinktime samples */
	/* [한국어] 누적된 think time 샘플의 개수(가중 이동평균에서의 유효
	 * 표본 수). 표본이 적으면 ttime_mean의 신뢰도가 낮다는 뜻이므로, 이
	 * 값을 이용해 판단의 확신도를 조절한다.
	 * 설정자: bfq_update_io_thinktime()이 매 샘플마다 증가(포화 상한 있음).
	 * 읽는 자: has_short_ttime 판정 로직이 충분한 샘플이 쌓였는지 확인할 때.
	 * 값 범위: 0 이상, 내부적으로 지수이동평균 창 크기에 의해 상한.
	 * 동기화: bfqd->lock. */
	unsigned long ttime_samples;
	/* average process thinktime */
	/* [한국어] think time의 지수이동평균(ns). 이 값이 짧다는 것은 이
	 * 프로세스가 자기 요청의 완료 직후 거의 곧바로 다음 요청을 낸다는
	 * 뜻이므로, BFQQF_has_short_ttime 판정을 통해 다른 큐의 request
	 * injection을 억제하고 이 큐의 sequential locality를 지켜준다.
	 * 반대로 think time이 길면 그 사이 장치를 놀리는 것이 손해이므로
	 * injection을 허용하는 쪽으로 판단이 기운다.
	 * 설정자: bfq_update_io_thinktime()이 ttime_total/ttime_samples로 재계산.
	 * 읽는 자: bfq_bfqq_has_short_ttime() 판정 로직, idling 여부 결정 로직.
	 * 값 범위: 0 이상(ns 단위), 짧을수록 "대화형/latency-critical"에 가까움.
	 * 동기화: bfqd->lock. */
	u64 ttime_mean;
};

/**
 * struct bfq_queue - leaf schedulable entity.
 *
 * A bfq_queue is a leaf request queue; it can be associated with an
 * io_context or more, if it is async or shared between cooperating
 * processes. Besides, it contains I/O requests for only one actuator
 * (an io_context is associated with a different bfq_queue for each
 * actuator it generates I/O for). @cgroup holds a reference to the
 * cgroup, to be sure that it does not disappear while a bfqq still
 * references it (mostly to avoid races between request issuing and
 * task migration followed by cgroup destruction).  All the fields are
 * protected by the queue lock of the containing bfqd.
 *
 * NVMe 관점: 실제로 디스패치될 request를 담는 leaf 스케줄링
 * 단위이다. 각 bfq_queue는 단일 actuator만을 대상으로 하므로,
 * actuator_idx는 gendisk->ia_ranges가 노출하는 독립 접근 영역 하나에
 * 대응한다. sort_list는 LBA 순서로 정렬되어 있어, 인접한 요청을 연달아
 * 내보내는 것만으로 순차 접근 최적화가 이뤄진다. next_rq는
 * blk_mq_run_hw_queue -> bfq_dispatch_requests -> __bfq_dispatch_request
 * 경로에서 NVMe
 * 드라이버로 전달될 다음 request 후보이다.
 */
struct bfq_queue {
	/* reference counter */
	/* [한국어] 이 bfq_queue의 참조 카운트. bic, entity, 진행 중인 request,
	 * merge/cooperation 관계 등 여러 곳에서 포인터를 들고 있을 수 있어
	 * refcount 방식으로 생명주기를 관리한다.
	 * 설정자: bfq_get_queue() 계열이 증가, bfq_put_queue()가 감소시키며
	 *   0이 되면 kmem_cache_free()로 실제 해제.
	 * 읽는 자: 해제 여부 판단 코드 전체.
	 * 값 범위: 1 이상(살아있는 동안), 0이 되는 순간 구조체 자체가 사라짐.
	 * 동기화: bfqd->lock 하에서만 증감(단일 CPU 관점의 정수 연산, atomic_t 아님에
	 *   유의 — 반드시 락 보호 하에 갱신). */
	int ref;
	/* counter of references from other queues for delayed stable merge */
	/* [한국어] "지연된 stable merge"를 위해 다른 bfq_queue가 이 큐를 잠재적
	 * merge 대상(stable_merge_bfqq)으로 붙잡고 있는 개수. 일반 ref와 별도로
	 * 관리해 merge 후보 관계가 끊어지는 시점을 구분한다.
	 * 설정자: bfq_setup_stable_merge()가 후보를 등록할 때 증가, merge 성사/
	 *   포기 시 감소.
	 * 읽는 자: bfq_release_process_ref() 등이 큐를 완전히 해제해도 되는지 판단.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int stable_ref;
	/* parent bfq_data */
	/* [한국어] 이 큐가 속한 디바이스 전역 bfq_data로의 역참조. 락(bfqd->lock)의
	 * 소유자를 찾거나, 디바이스 전역 파라미터(bfq_slice_idle 등)를 읽을 때 사용.
	 * 설정자: bfq_get_queue()가 큐 생성 시 1회 설정, 이후 불변.
	 * 읽는 자: 거의 모든 bfq_queue 조작 함수가 bfqd->lock 등을 얻기 위해 참조.
	 * 값 범위: 유효한 bfq_data 포인터(NULL 불가).
	 * 동기화: 생성 후 불변이므로 별도 동기화 불필요. */
	struct bfq_data *bfqd;

	/* current ioprio and ioprio class */
	/* [한국어] 현재 유효하게 적용 중인 I/O 우선순위 값과 클래스
	 * (IOPRIO_CLASS_RT/BE/IDLE). entity.weight 계산과 어느 service_tree에
	 * 들어갈지를 결정하며, 장치로 나갈 우선순위 그룹을 좌우한다.
	 * 설정자: __bfq_entity_update_weight_prio()가 prio_changed 처리 시 반영.
	 * 읽는 자: bfq_entity_service_tree(), bfq_ioprio_to_weight().
	 * 값 범위: ioprio는 0~7(IOPRIO_PRIO_LEVEL 매크로 참고), ioprio_class는
	 *   IOPRIO_CLASS_RT/BE/IDLE 중 하나.
	 * 동기화: bfqd->lock. */
	unsigned short ioprio, ioprio_class;
	/* next ioprio and ioprio class if a change is in progress */
	/* [한국어] ioprio_set() 등으로 변경이 요청되었으나 아직 반영 전인 값들.
	 * entity.prio_changed가 설정되어 있는 동안 여기에 보관되었다가, 안전한
	 * 시점에 ioprio/ioprio_class로 옮겨진다.
	 * 설정자: bfq_set_next_ioprio_data()가 새 값을 기록.
	 * 읽는 자: __bfq_entity_update_weight_prio()가 반영 시점에 읽음.
	 * 값 범위: ioprio/ioprio_class와 동일한 범위.
	 * 동기화: bfqd->lock. */
	unsigned short new_ioprio, new_ioprio_class;

	/* last total-service-time sample, see bfq_update_inject_limit() */
	/* [한국어] 이 큐에 대해 최근에 측정된 "총 서비스 시간"(요청이 큐에
	 * 들어와서 완료되기까지, 다른 큐의 injection까지 포함한 총 소요 시간, ns).
	 * inject_limit을 늘릴지 줄일지 판단하는 bfq_update_inject_limit()의
	 * 핵심 입력값이다.
	 * 설정자: bfq_update_inject_limit()이 wait_dispatch/waited_rq 샘플링 완료 시 갱신.
	 * 읽는 자: 동일 함수가 다음 샘플과 비교해 injection이 유해했는지 판단.
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: bfqd->lock. */
	u64 last_serv_time_ns;
	/* limit for request injection */
	/* [한국어] 이 큐가 서비스 중일 때, 다른 큐의 request를 몇 개까지
	 * "끼워넣기(injection)" 허용할지의 상한. NVMe에서 이 actuator가
	 * underutilized 상태일 때 다른 큐의 request로 SQ 슬롯을 채워 throughput을
	 * 올리되, 너무 많으면 이 큐 자신의 latency가 늘어나므로 상한을 둔다.
	 * 설정자: bfq_update_inject_limit()이 서비스 시간 샘플을 바탕으로 증감.
	 * 읽는 자: bfq_select_queue()가 injection 대상 선택 시 이 한계와 비교.
	 * 값 범위: 0(injection 금지) 이상.
	 * 동기화: bfqd->lock. */
	unsigned int inject_limit;
	/* last time the inject limit has been decreased, in jiffies */
	/* [한국어] inject_limit을 마지막으로 줄인 시각(jiffies). 너무 자주
	 * 갈팡질팡하며 증감하지 않도록, 감소 이후 일정 시간 동안은 재평가를
	 * 늦추는 냉각(cooldown) 타이머로 쓰인다.
	 * 설정자: bfq_update_inject_limit()이 inject_limit을 줄일 때 jiffies로 갱신.
	 * 읽는 자: 동일 함수가 재평가 시점 판단에 사용.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	unsigned long decrease_time_jif;

	/*
	 * Shared bfq_queue if queue is cooperating with one or more
	 * other queues.
	 */
	/* [한국어] 이 큐가 다른 큐와 협력(cooperation) 관계로 판단되어 병합될
	 * 대상 공유 큐. 병합되면 이후 이 큐로 들어올 새 request는 new_bfqq로
	 * 리다이렉트되어, 인접 LBA를 접근하는 여러 프로세스의 요청이 하나의
	 * 장치 경로로 합쳐진다.
	 * 설정자: bfq_setup_merge()가 협력 관계를 확정할 때 설정.
	 * 읽는 자: bic_to_bfqq() 등이 실제 사용할 큐를 결정할 때 따라간다.
	 * 값 범위: 유효 포인터 또는 병합되지 않았으면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_queue *new_bfqq;
	/* request-position tree member (see bfq_group's @rq_pos_tree) */
	/* [한국어] bfq_group->rq_pos_tree(요청 위치 기준 rb-tree)에 이 큐를
	 * 연결하는 노드. 인접한 LBA를 다루는 큐(잠재적 cooperator)를 O(log n)에
	 * 찾게 해 준다. 서로 다른 프로세스가 사실상 하나의 순차 스트림을
	 * 번갈아 내는 경우(interleaved I/O)를 발견해 두 큐를 병합하는 것이
	 * 목적이며, 병합하지 않으면 두 큐가 번갈아 서비스되면서 순차 접근이
	 * 랜덤 접근처럼 보이게 된다.
	 * 설정자: bfq_pos_tree_add_move()가 next_rq 위치 변경 시 재삽입.
	 * 읽는 자: bfq_find_close_cooperator()가 인접 노드 탐색에 사용.
	 * 값 범위: pos_root가 NULL이 아닐 때만 유효.
	 * 동기화: bfqd->lock. */
	struct rb_node pos_node;
	/* request-position tree root (see bfq_group's @rq_pos_tree) */
	/* [한국어] 이 큐가 속한 rq_pos_tree(보통 bfq_group->rq_pos_tree)에 대한
	 * 역참조. 큐가 async라면 NULL일 수 있다(async 큐는 cooperation 대상이
	 * 아니므로 위치 트리에 넣지 않음).
	 * 설정자: bfq_pos_tree_add_move()가 그룹에 따라 설정.
	 * 읽는 자: pos_node 삽입/삭제 시 대상 트리 확인.
	 * 값 범위: 유효 &bfq_group->rq_pos_tree 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	struct rb_root *pos_root;

	/* sorted list of pending requests */
	/* [한국어] 이 큐에 대기 중인 request들을 LBA(디스크 상 위치) 순으로
	 * 정렬해 담는 rb-tree. 위치 순으로 꺼내 보낼 수 있게 해 디스크 헤드
	 * 이동(HDD)과 장치 내부의 순차 접근 이점을 함께 살린다.
	 * 설정자: bfq_insert_request()가 새 request를 elv_rb_add()로 삽입.
	 * 읽는 자: bfq_choose_req()가 next_rq를 고를 때, bfq_remove_request()가
	 *   제거할 때.
	 * 값 범위: request들의 rb_node 집합, 비었으면 RB_ROOT.
	 * 동기화: bfqd->lock. */
	struct rb_root sort_list;
	/* if fifo isn't expired, next request to serve */
	/* [한국어] FIFO 타임아웃이 아직 발생하지 않았다면, sort_list에서 다음에
	 * 서비스할 것으로 선택된 request. 이 포인터가 결국
	 * blk_mq_dispatch_rq_list()를 거쳐 드라이버의 queue_rq()로 전달될
	 * 다음 후보다.
	 * 설정자: bfq_choose_req()가 sort_list 갱신 시마다 재계산.
	 * 읽는 자: __bfq_dispatch_request()가 실제 디스패치 대상으로 꺼낼 때.
	 * 값 범위: 유효 request 포인터 또는 큐가 비었으면 NULL.
	 * 동기화: bfqd->lock. */
	struct request *next_rq;
	/* number of sync and async requests queued */
	int queued[2];
	/* [한국어] [0]=async, [1]=sync 큐에 대기 중인 request 개수(배열 인덱스는
	 * bfq_io_cq.bfqq 행렬의 첫 인덱스와 동일한 관례). 장치 backlog 크기를
	 * 나타내며, 0이 되면 큐가 idle로 전이될 후보가 된다.
	 * 설정자: request 삽입/제거 시 bfq_insert_request()/bfq_remove_request()가 증감.
	 * 읽는 자: bfq_bfqq_busy() 등 backlog 여부 판단 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/* number of pending metadata requests */
	int meta_pending;
	/* [한국어] 대기 중인 request 중 메타데이터 성격(RQF_PM 등 특수 플래그가
	 * 붙은) request의 개수. 저널링/메타데이터 I/O 비율을 추적해 일부 판단
	 * 로직(예: seek 패턴 분류)에서 예외 처리하는 데 참고된다.
	 * 설정자: 메타데이터 request 삽입/제거 시 증감.
	 * 읽는 자: 통계/휴리스틱 판단 코드.
	 * 값 범위: 0 이상, queued[]의 부분집합.
	 * 동기화: bfqd->lock. */
	/* fifo list of requests in sort_list */
	struct list_head fifo;
	/* [한국어] sort_list와 같은 request들을 도착(FIFO) 순서로도 유지하는
	 * 리스트. LBA 순서만으로는 오래 대기한 request가 계속 밀릴 수 있어,
	 * 타임아웃 판정(bfq_fifo_expire) 및 NVMe timeout/abort 유사 상황에서
	 * "가장 오래 기다린 request"를 찾는 데 쓰인다.
	 * 설정자: bfq_insert_request()가 list_add_tail()로 추가, 완료/제거 시 list_del().
	 * 읽는 자: bfq_check_fifo()가 FIFO 헤드의 만료 여부를 검사.
	 * 값 범위: 비었거나 request->queuelist로 연결된 리스트.
	 * 동기화: bfqd->lock. */

	/* entity representing this queue in the scheduler */
	struct bfq_entity entity;
	/* [한국어] 이 bfq_queue를 B-WF2Q+ 스케줄러에 등록하기 위한 내장
	 * bfq_entity(포인터가 아니라 값으로 내장되어 있음에 유의). cgroup
	 * 계층에서의 위치, weight, 타임스탬프가 모두 여기 들어있다.
	 * 설정자: bfq_init_entity()가 큐 생성 시 초기화.
	 * 읽는 자: 이 큐를 서비스 트리에 넣고 빼는 bfq-wf2q.c의 모든 함수.
	 * 값 범위: struct bfq_entity 참고.
	 * 동기화: bfqd->lock. */

	/* pointer to the weight counter associated with this entity */
	struct bfq_weight_counter *weight_counter;
	/* [한국어] 이 큐의 현재 weight와 같은 weight를 가진 다른 활성 큐들을
	 * 세는 bfq_weight_counter로의 포인터. bfqd->queue_weights_tree에서
	 * 동일 weight 노드를 공유하며, 대칭 시나리오 판정(bfq_limit_depth 등)에
	 * 쓰인다.
	 * 설정자: bfq_weights_tree_add()가 큐 활성화 시 연결.
	 * 읽는 자: bfq_weights_tree_remove()가 큐 비활성화 시 num_active 감소.
	 * 값 범위: 유효 포인터 또는 아직 트리에 없으면 NULL.
	 * 동기화: bfqd->lock. */

	/* maximum budget allowed from the feedback mechanism */
	int max_budget;
	/* [한국어] 피드백(과거 소비 패턴 관찰) 메커니즘이 계산한, 이 큐에
	 * 허용되는 최대 예산. 한 큐가 장치를 독점하지 못하도록 하는 상한이며,
	 * peak_rate 추정치와 bfq_timeout을 바탕으로 계산된다.
	 * 설정자: bfq_updated_next_in_service()/bfq_bfqq_expire() 계열이 재계산.
	 * 읽는 자: entity.budget을 새로 할당할 때 상한으로 참조.
	 * 값 범위: 0 초과, bfqd->bfq_max_budget 이하.
	 * 동기화: bfqd->lock. */
	/* budget expiration (in jiffies) */
	unsigned long budget_timeout;
	/* [한국어] 현재 예산을 다 쓰기 전이라도 강제로 만료시켜야 하는 시각
	 * (jiffies). seeky한 큐가 예산을 오래 붙들고 있어 다른(특히 순차적인)
	 * 큐의 latency를 해치는 것을 막는 안전장치로, NVMe latency 보장의 한
	 * 축이다.
	 * 설정자: __bfq_set_in_service_queue()가 서비스 시작 시 jiffies+bfq_timeout으로 설정.
	 * 읽는 자: bfq_bfqq_budget_timeout()이 시간 초과 여부 검사.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */

	/* number of requests on the dispatch list or inside driver */
	int dispatched;
	/* [한국어] 이미 스케줄러 밖(dispatch 리스트 또는 드라이버/장치)으로
	 * 나갔지만 아직 완료 보고가 오지 않은 request 개수. in-flight 상태를
	 * 정확히 추적해 idling/injection 판단에 사용한다.
	 * 설정자: bfq_dispatch_request()가 증가, bfq_completed_request()가 감소.
	 * 읽는 자: bfq_bfqq_busy() 등 완전히 비었는지 판단하는 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */

	/* status flags */
	unsigned long flags;
	/* [한국어] enum bfqq_state_flags(아래 정의)의 비트 조합. sync 여부,
	 * FIFO 만료 여부, short think-time 여부, large burst 소속 여부 등
	 * 장치/CQ 흐름 제어에 직접 영향을 주는 상태 비트들의 모음이다.
	 * 설정자/읽는 자: BFQ_BFQQ_FNS() 매크로가 생성하는
	 *   bfq_mark_bfqq_*()/bfq_clear_bfqq_*()/bfq_bfqq_*() 함수들.
	 * 값 범위: enum bfqq_state_flags 비트들의 OR 조합.
	 * 동기화: bfqd->lock. */

	/* node for active/idle bfqq list inside parent bfqd */
	struct list_head bfqq_list;
	/* [한국어] bfqd->active_list[actuator]/idle_list 중 하나에 이 큐를
	 * 연결하는 리스트 노드. 서비스 트리와는 별개로, "현재 활성인 큐 전체"를
	 * 순회할 때(예: injection 후보 탐색) 쓰인다.
	 * 설정자: 큐 활성화/비활성화 시 list_add()/list_del()로 이동.
	 * 읽는 자: bfq_select_queue() 등이 active_list를 순회할 때.
	 * 값 범위: 리스트에 연결되어 있거나(LIST_POISON 아님) 미연결.
	 * 동기화: bfqd->lock. */

	/* associated @bfq_ttime struct */
	struct bfq_ttime ttime;
	/* [한국어] 이 큐(프로세스)의 think time 통계(위 struct bfq_ttime 참고).
	 * 설정자: bfq_update_io_thinktime()이 request 도착 시마다 갱신.
	 * 읽는 자: has_short_ttime 판정, idling 여부 결정 로직.
	 * 값 범위: struct bfq_ttime 참고.
	 * 동기화: bfqd->lock. */

	/* when bfqq started to do I/O within the last observation window */
	u64 io_start_time;
	/* [한국어] 현재 관찰 구간(observation window) 내에서 이 큐가 I/O를
	 * 시작한 시각(ns). tot_idle_time과 함께 이 구간 동안의 실질 활동 비율을
	 * 계산해 throughput 추정에 활용된다.
	 * 설정자: 큐가 idle에서 backlogged로 전이될 때 갱신.
	 * 읽는 자: bfq_update_peak_rate() 등 throughput 추정 로직.
	 * 값 범위: ktime 단조 시계 값.
	 * 동기화: bfqd->lock. */
	/* how long bfqq has remained empty during the last observ. window */
	u64 tot_idle_time;
	/* [한국어] 같은 관찰 구간 동안 이 큐가 비어 있었던(request가 없던)
	 * 누적 시간(ns). 이 큐가 실제로 장치를 점유한 비율을
	 * 역산하는 데 쓰인다.
	 * 설정자: 큐가 비어있는 동안 흐른 시간을 누적.
	 * 읽는 자: throughput/idle 비율 추정 로직.
	 * 값 범위: 0 이상(ns 단위), io_start_time 이후 경과 시간 이하.
	 * 동기화: bfqd->lock. */

	/* bit vector: a 1 for each seeky requests in history */
	u32 seek_history;
	/* [한국어] 최근 request들이 seek(비순차 접근)이었는지를 비트마다
	 * 기록하는 시프트 레지스터(1=seeky, 0=sequential). 이 히스토리의 1의
	 * 개수/비율로 큐를 "random" 또는 "sequential" 워크로드로 분류해 예산
	 * 크기와 idling 정책을 조정한다.
	 * 설정자: 새 request 도착 시 bfq_rq_close/bfq_updated_next_in_service 등이
	 *   좌측 시프트 후 최하위 비트에 결과 반영.
	 * 읽는 자: BFQQ_seeky() 계열 판정 매크로/함수.
	 * 값 범위: 32비트 비트마스크.
	 * 동기화: bfqd->lock. */

	/* node for the device's burst list */
	struct hlist_node burst_list_node;
	/* [한국어] bfqd->burst_list(짧은 시간 내에 잇달아 생성된 큐들의 목록)에
	 * 이 큐를 연결하는 해시리스트 노드. 다수의 큐가 한꺼번에 활성화되는
	 * "버스트"를 감지해 과도한 SQ 경쟁을 완화하는 bfq_handle_burst()에서 쓰인다.
	 * 설정자: bfq_handle_burst()가 버스트 조건 만족 시 hlist_add_head().
	 * 읽는 자: 버스트 관련 통계/정리 로직.
	 * 값 범위: 연결되어 있거나 미연결.
	 * 동기화: bfqd->lock. */

	/* position of the last request enqueued */
	sector_t last_request_pos;
	/* [한국어] 이 큐에 마지막으로 삽입된 request의 시작 LBA. 다음 request와의
	 * 거리 차로 순차성(sequentiality)을 판단해 seek_history를 갱신하는 데
	 * 쓰인다. 이 판정 결과가 BFQQ_SEEKY()로 이어져 idling 여부와 budget
	 * 조정 방향을 좌우한다.
	 * 설정자: bfq_insert_request()가 매 삽입마다 갱신.
	 * 읽는 자: bfq_rq_close() 등 인접성 판정 로직.
	 * 값 범위: 0 이상의 섹터 오프셋(sector_t).
	 * 동기화: bfqd->lock. */

	/* Number of consecutive pairs of request completion and
	 * arrival, such that the queue becomes idle after the
	 * completion, but the next request arrives within an idle
	 * time slice; used only if the queue's IO_bound flag has been
	 * cleared.
	 */
	unsigned int requests_within_timer;
	/* [한국어] "완료 후 idle 슬라이스 이내에 다음 request 도착"이 연속으로
	 * 관측된 횟수. IO_bound 플래그가 아직 안 켜진 큐에 대해서만 의미가
	 * 있으며, "완료 직후 곧바로 다음 요청이 온다"는 패턴이 우연이 아니라
	 * 일관된 성향임을 확인하는 근거가 되어 결국 IO_bound로 승격시킬지
	 * 판단한다.
	 * 설정자: bfq_update_io_thinktime() 계열이 조건 만족 시 증가, 아니면 0으로 리셋.
	 * 읽는 자: IO_bound 플래그 승격 조건 검사 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */

	/* pid of the process owning the queue, used for logging purposes */
	pid_t pid;
	/* [한국어] 이 큐를 소유한 프로세스의 PID. 로깅/blktrace 식별용으로만
	 * 쓰이며(bfq_bfqq_name() 참고), 스케줄링 로직 자체에는 영향을 주지 않는다.
	 * 공유 큐(merge된 경우)에서는 -1로 설정되어 "SHARED"로 표시된다.
	 * 설정자: bfq_get_queue()가 큐 생성 시 현재 태스크의 pid로 설정.
	 * 읽는 자: bfq_bfqq_name(), bfq_log_bfqq() 매크로.
	 * 값 범위: 유효 pid 또는 공유 큐를 뜻하는 -1.
	 * 동기화: 생성 후 거의 불변(merge 시 -1로 바뀔 수 있음), bfqd->lock. */

	/*
	 * Pointer to the bfq_io_cq owning the bfq_queue, set to %NULL
	 * if the queue is shared.
	 */
	struct bfq_io_cq *bic;
	/* [한국어] 이 큐를 소유한 태스크별 bfq_io_cq로의 역참조. 큐가 여러
	 * 프로세스 간에 공유(merge)되면 소유자가 하나로 특정되지 않으므로 NULL이
	 * 된다. 장치 경로가 태스크별로 분리되어 있는지(bic != NULL) 아니면
	 * 공유되는지를 구분하는 값이다.
	 * 설정자: bic_set_bfqq()가 최초 연결 시 설정, merge 시 NULL로 리셋.
	 * 읽는 자: bic 정보가 필요한 merge/split 로직.
	 * 값 범위: 유효 포인터 또는 NULL(공유 상태).
	 * 동기화: bfqd->lock. */

	/* current maximum weight-raising time for this queue */
	unsigned long wr_cur_max_time;
	/* [한국어] 현재 weight-raising(WR) 기간이 지속될 수 있는 최대 시간
	 * (jiffies). low_latency 모드에서 대화형/soft-RT로 판정된 큐에
	 * 일시적으로 높은 weight를 주는 기간의 상한으로, 장치 상에서
	 * latency-critical 트래픽에게 자원을 더 주는 QoS 창(window)이다.
	 * 설정자: WR 시작 시 bfq_wr_duration() 계산 결과로 설정.
	 * 읽는 자: WR 종료 시점 판단 로직(last_wr_start_finish와 비교).
	 * 값 범위: 0 이상(jiffies 단위).
	 * 동기화: bfqd->lock. */
	/*
	 * Minimum time instant such that, only if a new request is
	 * enqueued after this time instant in an idle @bfq_queue with
	 * no outstanding requests, then the task associated with the
	 * queue it is deemed as soft real-time (see the comments on
	 * the function bfq_bfqq_softrt_next_start())
	 */
	unsigned long soft_rt_next_start;
	/* [한국어] 이 시각 이후에 idle한 큐로 새 request가 들어와야만 해당
	 * 프로세스를 soft real-time으로 인정할 수 있다는 기준 시각(jiffies).
	 * NVMe 관점에서 대화형(interactive) 워크로드의 latency 민감도를 판단하는
	 * 임계값 역할을 한다.
	 * 설정자: bfq_bfqq_softrt_next_start()가 계산해 갱신.
	 * 읽는 자: 새 request 도착 시 soft-RT 재분류 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/*
	 * Start time of the current weight-raising period if
	 * the @bfq-queue is being weight-raised, otherwise
	 * finish time of the last weight-raising period.
	 */
	unsigned long last_wr_start_finish;
	/* [한국어] 현재 WR 중이면 이번 WR 기간의 시작 시각, WR 중이 아니면
	 * 지난 WR 기간이 끝난 시각(둘 다 jiffies). wr_cur_max_time과 비교해
	 * WR을 계속할지 종료할지 판단하는 기준점이다.
	 * 설정자: WR 시작/종료 시점마다 jiffies로 갱신.
	 * 읽는 자: WR 만료 판정 로직, 재-WR(재활성화) 가능 여부 판단.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/* factor by which the weight of this queue is multiplied */
	unsigned int wr_coeff;
	/* [한국어] WR이 적용될 때 orig_weight에 곱해지는 배율. 1이면 WR이 적용되고
	 * 있지 않다는 뜻이고, bfqd->bfq_wr_coeff까지 커질 수 있어 이 큐가 장치
	 * 자원 분배에서 받는 몫을 크게 늘린다.
	 * 설정자: WR 시작 시 bfqd->bfq_wr_coeff로 설정, 종료 시 1로 복원.
	 * 읽는 자: entity.weight 재계산 시 orig_weight * wr_coeff 형태로 사용.
	 * 값 범위: [1, bfqd->bfq_wr_coeff].
	 * 동기화: bfqd->lock. */
	/*
	 * Time of the last transition of the @bfq_queue from idle to
	 * backlogged.
	 */
	unsigned long last_idle_bklogged;
	/* [한국어] 이 큐가 마지막으로 idle -> backlogged로 전이한 시각(jiffies).
	 * service_from_backlogged를 언제부터 다시 세기 시작했는지의 기준점이다.
	 * 설정자: 큐가 활성화(bfq_add_bfqq_busy 등)될 때 갱신.
	 * 읽는 자: fairness 관련 통계 계산 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/*
	 * Cumulative service received from the @bfq_queue since the
	 * last transition from idle to backlogged.
	 */
	unsigned long service_from_backlogged;
	/* [한국어] last_idle_bklogged 이후 이 큐가 누적으로 받은 서비스량.
	 * fairness 보장이 실제로 지켜지고 있는지 검증하거나 seek 패턴 재평가
	 * 시점을 정하는 데 참고된다.
	 * 설정자: 서비스가 진행될 때마다 누적.
	 * 읽는 자: 관련 fairness/재평가 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/*
	 * Cumulative service received from the @bfq_queue since its
	 * last transition to weight-raised state.
	 */
	unsigned long service_from_wr;
	/* [한국어] 이 큐가 마지막으로 WR 상태로 전이한 이후 누적으로 받은
	 * 서비스량. WR 기간 동안 실제로 얼마나 많은 장치 서비스를 받았는지
	 * 기록해, 과도한 WR 지속을 제한하는 판단에 쓰인다.
	 * 설정자: WR 중 서비스가 진행될 때마다 누적, WR 시작 시 0으로 리셋.
	 * 읽는 자: WR 종료 조건 판단 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */

	/*
	 * Value of wr start time when switching to soft rt
	 */
	unsigned long wr_start_at_switch_to_srt;
	/* [한국어] 일반 WR 상태에서 soft-RT WR 상태로 전환할 때의
	 * last_wr_start_finish 스냅샷(jiffies). soft-RT WR의 지속 시간을
	 * 별도로 계산하기 위한 기준점이다.
	 * 설정자: soft-RT로 전환하는 순간 기록.
	 * 읽는 자: soft-RT WR 지속시간 계산 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	unsigned long split_time; /* time of last split */
	/* [한국어] 이 큐가 협력(cooperating) 관계에서 분리(split)된 마지막
	 * 시각(jiffies). 분리 직후 너무 빨리 다시 merge되는 것을 막거나, 분리된
	 * 큐의 특성(think time 등)을 다시 학습하는 유예 기간의 기준이 된다.
	 * 설정자: bfq_split_bfqq()가 분리 시 갱신.
	 * 읽는 자: merge 재시도 억제 로직.
	 * 값 범위: jiffies 값 또는 분리 이력이 없으면 초기값.
	 * 동기화: bfqd->lock. */
	unsigned long first_IO_time; /* time of first I/O for this queue */
	/* [한국어] 이 큐가 생성된 이후 최초로 I/O를 수행한 시각(jiffies).
	 * creation_time과의 차이로 "생성 후 실제 사용까지 걸린 지연"을 파악해
	 * 초기 분류 휴리스틱(예: burst 판정)에 참고된다.
	 * 설정자: 첫 request 삽입 시 1회 설정.
	 * 읽는 자: 초기 분류/버스트 판정 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	unsigned long creation_time; /* when this queue is created */
	/* [한국어] 이 bfq_queue가 생성된 시각(jiffies). last_bfqq_created와
	 * 비교해 짧은 시간 내에 잇달아 생성된 큐들을 stable-merge 후보로 묶는
	 * 판단 기준으로 쓰인다.
	 * 설정자: bfq_get_queue()가 큐 생성 시 1회 설정.
	 * 읽는 자: bfq_setup_stable_merge() 등 생성 시각 비교 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/*
	 * Pointer to the waker queue for this queue, i.e., to the
	 * queue Q such that this queue happens to get new I/O right
	 * after some I/O request of Q is completed. For details, see
	 * the comments on the choice of the queue for injection in
	 * bfq_select_queue().
	 */
	struct bfq_queue *waker_bfqq;
	/* [한국어] 이 큐가 새 I/O를 받는 시점이 다른 큐 Q의 완료 직후와
	 * 반복적으로 겹친다고 판단될 때, 그 Q를 가리키는 포인터("깨움" 관계).
	 * NVMe CQ 완료 처리 로직이 Q를 완료시킬 때 이 큐로 곧 request가 올 것을
	 * 예측해 injection 여부 결정에 활용한다(bfq_select_queue() 참고).
	 * 설정자: bfq_check_waker()가 충분한 반복 관측(num_waker_detections) 후 확정.
	 * 읽는 자: bfq_select_queue()가 injection 대상 판단 시 참조.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	/* pointer to the curr. tentative waker queue, see bfq_check_waker() */
	struct bfq_queue *tentative_waker_bfqq;
	/* [한국어] waker_bfqq로 확정되기 전, 그 가능성을 테스트 중인 임시
	 * 후보 큐. num_waker_detections 횟수만큼 반복 관측되면 waker_bfqq로
	 * 승격된다.
	 * 설정자: bfq_check_waker()가 새 패턴을 관측할 때 설정/교체.
	 * 읽는 자: 동일 함수가 반복 관측 여부 판단에 사용.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	/* number of times the same tentative waker has been detected */
	unsigned int num_waker_detections;
	/* [한국어] tentative_waker_bfqq와 동일한 패턴이 연속으로 관측된 횟수.
	 * 이 값이 임계치를 넘으면 waker_bfqq로 확정해 injection 예측의 신뢰도로
	 * 삼는다.
	 * 설정자: bfq_check_waker()가 같은 후보가 재관측될 때마다 증가, 다른
	 *   패턴이 보이면 리셋.
	 * 읽는 자: 동일 함수의 승격 조건 검사.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/* time when we started considering this waker */
	u64 waker_detection_started;
	/* [한국어] 현재 tentative_waker_bfqq 후보를 관측하기 시작한 시각(ns).
	 * 너무 오래된 관측을 무한정 신뢰하지 않도록 시간 창을 두는 데 쓰인다.
	 * 설정자: 새 tentative 후보가 설정될 때 갱신.
	 * 읽는 자: bfq_check_waker()가 관측 유효 기간 판단.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */

	/* node for woken_list, see below */
	struct hlist_node woken_list_node;
	/* [한국어] 자신을 waker로 삼고 있는 다른 큐의 woken_list에 이 큐를
	 * 연결하는 노드. waker_bfqq가 가리키는 큐가 사라질 때, 그 woken_list를
	 * 순회하며 이 노드를 통해 자신의 waker_bfqq를 NULL로 리셋할 수 있게 한다.
	 * 설정자: waker_bfqq 관계가 확정될 때 상대방 woken_list에 hlist_add_head().
	 * 읽는 자: waker 큐 소멸 시 순회.
	 * 값 범위: 연결되어 있거나 미연결.
	 * 동기화: bfqd->lock. */
	/*
	 * Head of the list of the woken queues for this queue, i.e.,
	 * of the list of the queues for which this queue is a waker
	 * queue. This list is used to reset the waker_bfqq pointer in
	 * the woken queues when this queue exits.
	 */
	struct hlist_head woken_list;
	/* [한국어] 이 큐를 waker로 삼고 있는(즉 이 큐 완료 직후 새 I/O를 받는)
	 * 다른 큐들의 목록. 이 큐가 소멸(exit)할 때 이 리스트를 순회하며 각
	 * woken 큐의 waker_bfqq를 정리(NULL 처리)한다.
	 * 설정자: 다른 큐가 이 큐를 waker로 확정할 때 hlist에 자신을 추가.
	 * 읽는 자: 이 큐 소멸 시 bfq_put_queue() 계열이 순회하며 정리.
	 * 값 범위: 비어있거나 hlist_node로 연결된 큐 집합.
	 * 동기화: bfqd->lock. */

	/* index of the actuator this queue is associated with */
	unsigned int actuator_idx;
	/* [한국어] 이 큐가 담당하는 actuator(독립 접근 범위, NVMe라면
	 * Independent Access Range 또는 대응하는 queue 쌍으로 추정)의 인덱스.
	 * bfq_io_cq.bfqq[sync/async][actuator_idx] 행렬의 열 인덱스와 대응하며,
	 * 하나의 프로세스라도 actuator마다 별도의 bfq_queue를 갖게 한다.
	 * 설정자: bfq_get_queue()가 큐 생성 시 bio가 겨냥하는 actuator로 설정.
	 * 읽는 자: bfqd->active_list[actuator_idx], bfqd->rq_in_driver[actuator_idx]
	 *   등 actuator별 배열 인덱싱에 사용.
	 * 값 범위: [0, bfqd->num_actuators) (추정).
	 * 동기화: 생성 후 불변. */
};
/**
* struct bfq_data - bfqq data unique and persistent for associated bfq_io_cq
*
* [한국어] 이 구조체는 이름과 달리 bfq_data가 아니라, 하나의 bfq_io_cq(태스크당
* 컨텍스트)에 딸린 "동기(sync) bfq_queue가 다른 큐와 merge/split을 겪을 때"
* 상태를 저장·복원하기 위한 스냅샷 저장소다. 두 프로세스의 큐가 협력 관계로
* 판단되어 하나로 합쳐지면(merge) 원래 큐는 사용을 멈추지만, 나중에 다시
* split될 수 있으므로 merge 시점의 학습된 상태(think time, IO_bound, WR
* 정보 등)를 잃지 않도록 여기에 백업해 둔다.
*/
struct bfq_iocq_bfqq_data {
	/*
	 * Snapshot of the has_short_time flag before merging; taken
	 * to remember its values while the queue is merged, so as to
	 * be able to restore it in case of split.
	 */
	/* [한국어] merge 직전 BFQQF_has_short_ttime 플래그의 스냅샷.
	 * 설정자: bfq_bfqq_save_state()가 merge 직전에 기록.
	 * 읽는 자: bfq_bfqq_resume_state()가 split 시 원래 큐에 복원.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	bool saved_has_short_ttime;
	/*
	 * Same purpose as the previous two fields for the I/O bound
	 * classification of a queue.
	 */
	/* [한국어] merge 직전 BFQQF_IO_bound 플래그의 스냅샷. 장치
	 * timeout/abort 판정에 쓰이던 "IO-bound" 분류를 split 후에도 잃지
	 * 않기 위함.
	 * 설정자/읽는 자: saved_has_short_ttime과 동일한 save/resume 경로.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	bool saved_IO_bound;

	/*
	 * Same purpose as the previous fields for the values of the
	 * field keeping the queue's belonging to a large burst
	 */
	/* [한국어] merge 직전 BFQQF_in_large_burst 플래그의 스냅샷.
	 * 설정자/읽는 자: bfq_bfqq_save_state()/bfq_bfqq_resume_state().
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	bool saved_in_large_burst;
	/*
	 * True if the queue belonged to a burst list before its merge
	 * with another cooperating queue.
	 */
	/* [한국어] merge 이전에 burst_list_node가 bfqd->burst_list에 실제로
	 * 연결되어 있었는지 여부. in_large_burst와 별개로, "버스트 목록에
	 * 있었는가"라는 이력 자체를 보존한다.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	bool was_in_burst_list;

	/*
	 * Save the weight when a merge occurs, to be able
	 * to restore it in case of split. If the weight is not
	 * correctly resumed when the queue is recycled,
	 * then the weight of the recycled queue could differ
	 * from the weight of the original queue.
	 */
	/* [한국어] merge 직전 entity.weight의 스냅샷. split 후 weight가 어긋나면
	 * 장치 자원 분배 비율이 원래 큐와 달라지므로 정확한 복원이 중요하다.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT] 범위(또는 WR 배율 포함 값).
	 * 동기화: bfqd->lock. */
	unsigned int saved_weight;
	/* [한국어] merge 직전 io_start_time 스냅샷. split 후 관찰 구간 계산을
	 * 이어가기 위한 복원값.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	u64 saved_io_start_time;
	/* [한국어] merge 직전 tot_idle_time 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: ns 단위 누적값.
	 * 동기화: bfqd->lock. */
	u64 saved_tot_idle_time;

	/*
	 * Similar to previous fields: save wr information.
	 */
	/* [한국어] merge 직전 wr_coeff 스냅샷. split 후 WR 배율이 원래대로
	 * 복원되어 장치 우선순위가 유지된다.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: [1, bfqd->bfq_wr_coeff].
	 * 동기화: bfqd->lock. */
	unsigned long saved_wr_coeff;
	/* [한국어] merge 직전 last_wr_start_finish 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	unsigned long saved_last_wr_start_finish;
	/* [한국어] merge 직전 service_from_wr 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	unsigned long saved_service_from_wr;
	/* [한국어] merge 직전 wr_start_at_switch_to_srt 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	unsigned long saved_wr_start_at_switch_to_srt;
	/* [한국어] merge 직전 ttime(think time 통계) 전체 스냅샷. 병합된 동안에는
	 * 여러 프로세스의 요청 간격이 섞여 통계가 오염되므로, split 후 원래
	 * 프로세스의 think time 학습을 0부터 다시 시작하지 않도록 보존한다.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: struct bfq_ttime 참고.
	 * 동기화: bfqd->lock. */
	struct bfq_ttime saved_ttime;
	/* [한국어] merge 직전 wr_cur_max_time 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: jiffies 단위, 0 이상.
	 * 동기화: bfqd->lock. */
	unsigned int saved_wr_cur_max_time;
	/* Save also injection state */
	unsigned int saved_inject_limit;
	/* [한국어] merge 직전 inject_limit 스냅샷. 장치 injection 한계를
	 * split 후에도 이어가기 위함.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	unsigned long saved_decrease_time_jif;
	/* [한국어] merge 직전 decrease_time_jif 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	u64 saved_last_serv_time_ns;
	/* [한국어] merge 직전 last_serv_time_ns 스냅샷.
	 * 설정자/읽는 자: save/resume 경로.
	 * 값 범위: ns 단위, 0 이상.
	 * 동기화: bfqd->lock. */
	/* candidate queue for a stable merge (due to close creation time) */
	struct bfq_queue *stable_merge_bfqq;
	/* [한국어] 생성 시각이 가까워 stable merge 후보로 지목된 다른
	 * bfq_queue. new_bfqq(즉시 merge)와 달리, "안정적으로 반복 관측되면
	 * merge하겠다"는 지연된 후보를 가리킨다.
	 * 설정자: bfq_setup_stable_merge()가 후보 지정 시 설정(stable_ref로
	 *   대상 큐의 참조를 함께 늘림).
	 * 읽는 자: 실제 merge 시도 로직이 후보 유효성 재확인 시 참조.
	 * 값 범위: 유효 포인터 또는 후보가 없으면 NULL.
	 * 동기화: bfqd->lock. */
	bool stably_merged;	/* non splittable if true */
	/* [한국어] true면 이 큐가 "안정적으로 merge된" 상태여서 이후 split
	 * 대상이 되지 않음을 뜻한다. 일시적 협력(cooperation)과 달리 되돌릴
	 * 필요가 없다고 판단된 영구적 merge다.
	 * 설정자: stable merge가 성사될 때 true로 설정.
	 * 읽는 자: split 판단 로직이 이 플래그가 서있으면 split을 건너뜀.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
};
/**
 * struct bfq_io_cq - per (request_queue, io_context) structure.
 *
 * NVMe 관점: request_queue는 NVMe 컨트롤러의 hardware queue와 연결된다.
 * bfqq[2][BFQ_MAX_ACTUATORS] 배열은 sync/async 및 actuator별로 나뉘어
 * 있으므로, NVMe Multi-Queue의 blk_mq_hw_ctx 또는 queue 쌍과 대응될 수
 * 있다. 이 구조체는 blk_mq_sched_bio_merge() -> bfq_bio_merge() 시
 * bic 조회를 통해 병합 가능한 bio를 찾는 데 활용된다.
 */
struct bfq_io_cq {
	/* associated io_cq structure */
	/* [한국어] 블록 계층 공통의 per (request_queue, io_context) 구조체.
	 * container_of()로 bfq_io_cq 전체를 얻어야 하므로 반드시 첫 번째
	 * 멤버여야 한다(원본 주석의 "must be the first member" 제약).
	 * 설정자: blk-ioc.c의 ioc_create_icq()가 아이콘텍스트 생성 시 초기화.
	 * 읽는 자: icq_to_bic() 매크로/헬퍼가 icq 포인터로부터 bic를 역산.
	 * 값 범위: 유효한 struct io_cq(NULL 불가).
	 * 동기화: io_context의 락 규칙을 따름(블록 계층 공통 icq 규칙). */
	struct io_cq icq; /* must be the first member */
	/*
	 * Matrix of associated process queues: first row for async
	 * queues, second row sync queues. Each row contains one
	 * column for each actuator. An I/O request generated by the
	 * process is inserted into the queue pointed by bfqq[i][j] if
	 * the request is to be served by the j-th actuator of the
	 * drive, where i==0 or i==1, depending on whether the request
	 * is async or sync. So there is a distinct queue for each
	 * actuator.
	 */
	struct bfq_queue *bfqq[2][BFQ_MAX_ACTUATORS];
	/* [한국어] 이 태스크(io_context)가 만드는 request를 담을 bfq_queue
	 * 행렬. 행(i)=0이면 async, 1이면 sync, 열(j)=actuator 인덱스.
	 * request가 j번째 actuator를 겨냥하면 bfqq[i][j]에 삽입되어, 같은
	 * 프로세스라도 actuator별로 별도의 디스패치 경로를 갖게 한다.
	 * 설정자: bic_set_bfqq()가 큐 생성/merge/split 시 갱신.
	 * 읽는 자: bic_to_bfqq()가 bio 도착 시 어느 큐로 넣을지 조회.
	 * 값 범위: 유효 포인터 또는 아직 생성 전이면 NULL.
	 * 동기화: bfqd->lock(설정) / bfqd->bio_bic 경로에서는 bio-merge용 락 완화
	 *   규칙을 따름. */
	/* per (request_queue, blkcg) ioprio */
	int ioprio;
	/* [한국어] 이 태스크가 마지막으로 관측된 ioprio 값의 캐시. ioprio가
	 * 실제로 바뀌었는지(재조회 없이) 빠르게 비교하기 위해 둔다.
	 * 설정자: bfq_check_ioprio_change()가 태스크의 io_context ioprio와
	 *   비교 후 변경 시 갱신.
	 * 읽는 자: 동일 함수가 다음 호출에서 변경 여부 판단.
	 * 값 범위: ioprio 인코딩 값(IOPRIO_PRIO_VALUE 등).
	 * 동기화: bfqd->lock. */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	uint64_t blkcg_serial_nr; /* the current blkcg serial */
	/* [한국어] 이 태스크가 마지막으로 속했던 blkcg의 serial number 캐시.
	 * cgroup 이전(migration) 여부를 감지하기 위한 비교값이며,
	 * CONFIG_BFQ_GROUP_IOSCHED가 켜진 빌드에서만 존재한다.
	 * 설정자: bfq_bic_update_cgroup()이 태스크의 현재 blkcg serial과
	 *   비교 후 변경 시 갱신.
	 * 읽는 자: 동일 함수가 다음 bio 도착 시 cgroup 이전 여부 판단.
	 * 값 범위: blkcg_gq/css의 serial_nr 값.
	 * 동기화: bfqd->lock. */
#endif
	/*
	 * Persistent data for associated synchronous process queues
	 * (one queue per actuator, see field bfqq above). In
	 * particular, each of these queues may undergo a merge.
	 */
	struct bfq_iocq_bfqq_data bfqq_data[BFQ_MAX_ACTUATORS];
	/* [한국어] actuator별 동기(sync) bfq_queue 하나하나에 대응하는
	 * merge/split 상태 저장소(위 struct bfq_iocq_bfqq_data 참고). async
	 * 큐는 여러 프로세스가 공유하는 것이 정상이라 merge 개념이 없어
	 * 별도 스냅샷이 필요 없다.
	 * 설정자: bfq_bfqq_save_state()/bfq_bfqq_resume_state()가 merge/split
	 *   시점에 갱신.
	 * 읽는 자: split 발생 시 원래 큐 상태를 복원하는 로직.
	 * 값 범위: struct bfq_iocq_bfqq_data 참고, 배열 크기는 BFQ_MAX_ACTUATORS.
	 * 동기화: bfqd->lock. */
	unsigned int requests;	/* Number of requests this process has in flight */
	/* [한국어] 이 프로세스(io_context)가 현재 in-flight로 가진 전체 request
	 * 개수(모든 actuator/sync-async 합산). depth 제한(bfq_limit_depth)
	 * 계산 시 이 태스크가 이미 얼마나 장치 슬롯을 쓰고 있는지 참조된다.
	 * 설정자: request 할당/완료 시 증감.
	 * 읽는 자: bfq_limit_depth()가 새 tag 할당을 제한할지 판단할 때.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
};

/**
 * struct bfq_data - per-device data structure.
 *
 * All the fields are protected by @lock.
 *
 * BFQ가 붙은 request_queue 하나당 하나씩 존재하며, 그 request_queue를
 * 감싼다. dispatch 리스트는 blk_mq_run_hw_queue -> bfq_dispatch_request
 * -> blk_mq_dispatch_rq_list -> 드라이버의 queue_rq()로 이어지는 경로에서
 * 스케줄링을 건너뛰고 곧바로 나갈 request들을 담는다.
 * rq_in_driver[actuator]는 각 actuator별로 "스케줄러를 떠나 드라이버에서
 * 처리 중인" request 수를 센다. 이 값의 용도는 태그 고갈 방지가 아니라
 * (그것은 blk-mq의 태그 할당이 이미 막는다) injection 한도 판단과
 * hw_tag 추정이다. actuator_load_threshold(=4)보다 적게 떠 있는
 * actuator는 한산하다고 보아, 그쪽으로 보낼 다른 큐의 request를 끼워
 * 넣는다.
 */
struct bfq_data {
	/* device request queue */
	/* [한국어] 이 BFQ 인스턴스가 붙어 있는 blk-mq request_queue. NVMe라면
	 * 이 request_queue 아래에 hardware queue(blk_mq_hw_ctx)들이 매달려
	 * 있고, 최종적으로 nvme_queue_rq()가 이 queue를 통해 커맨드를 낸다.
	 * 설정자: bfq_init_queue()가 elevator 초기화 시 1회 설정.
	 * 읽는 자: blk_trace_note_message_enabled((bfqd)->queue) 등 로깅
	 *   매크로, dispatch 경로 전체.
	 * 값 범위: 유효한 request_queue 포인터(NULL 불가).
	 * 동기화: elevator 생명주기 동안 불변. */
	struct request_queue *queue;
	/* dispatch queue */
	/* [한국어] B-WF2Q+ 트리에서 선택되었지만 아직 blk_mq_dispatch_rq_list()로
	 * 실제 드라이버에 넘어가지 않은 request들의 대기열. 장치로 전달되기
	 * 직전 마지막 홀딩 지점이다.
	 * 설정자: bfq_dispatch_request()가 list_add()/list_add_tail()로 추가.
	 * 읽는 자: blk-mq 코어가 .dispatch_request 콜백 반환값을 소비할 때.
	 * 값 범위: 비어있거나 request->queuelist로 연결된 리스트.
	 * 동기화: bfqd->lock. */
	struct list_head dispatch;

	/* root bfq_group for the device */
	/* [한국어] 이 디바이스의 cgroup 계층 최상위 노드. cgroup을 쓰지 않는
	 * task나 아직 특정 cgroup으로 분류되지 않은 I/O가 귀속되는 기본 그룹이며,
	 * 장치 자원 분배 트리의 뿌리다.
	 * 설정자: bfq_create_group_hierarchy()가 큐 초기화 시 생성.
	 * 읽는 자: bfq_bio_bfqg() 등이 cgroup 미지정 bio의 그룹을 찾을 때.
	 * 값 범위: 유효 bfq_group 포인터(NULL 불가, 디바이스 생명주기 내내 존재).
	 * 동기화: bfqd->lock (구조 자체는 불변, 하위 상태는 락 보호). */
	struct bfq_group *root_group;

	/*
	 * rbtree of weight counters of @bfq_queues, sorted by
	 * weight. Used to keep track of whether all @bfq_queues have
	 * the same weight. The tree contains one counter for each
	 * distinct weight associated to some active and not
	 * weight-raised @bfq_queue (see the comments to the functions
	 * bfq_weights_tree_[add|remove] for further details).
	 */
	/* [한국어] 현재 활성(active)이면서 weight-raise되지 않은 bfq_queue들의
	 * weight별 bfq_weight_counter를 모아둔 rb-tree(가장 왼쪽 노드를 캐시하는
	 * rb_root_cached). 이 트리의 노드가 하나뿐이면 "모든 큐의 weight가
	 * 같은 대칭 시나리오"로 판단해 depth 제한 등을 단순화할 수 있다.
	 * 설정자: bfq_weights_tree_add()/bfq_weights_tree_remove().
	 * 읽는 자: bfq_asymmetric_scenario() 계열이 대칭성 판정 시 노드 개수를 확인.
	 * 값 범위: bfq_weight_counter들의 집합, 비어있으면 RB_ROOT_CACHED.
	 * 동기화: bfqd->lock. */
	struct rb_root_cached queue_weights_tree;

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	/*
	 * Number of groups with at least one process that
	 * has at least one request waiting for completion. Note that
	 * this accounts for also requests already dispatched, but not
	 * yet completed. Therefore this number of groups may differ
	 * (be larger) than the number of active groups, as a group is
	 * considered active only if its corresponding entity has
	 * queues with at least one request queued. This
	 * number is used to decide whether a scenario is symmetric.
	 * For a detailed explanation see comments on the computation
	 * of the variable asymmetric_scenario in the function
	 * bfq_better_to_idle().
	 *
	 * However, it is hard to compute this number exactly, for
	 * groups with multiple processes. Consider a group
	 * that is inactive, i.e., that has no process with
	 * pending I/O inside BFQ queues. Then suppose that
	 * num_groups_with_pending_reqs is still accounting for this
	 * group, because the group has processes with some
	 * I/O request still in flight. num_groups_with_pending_reqs
	 * should be decremented when the in-flight request of the
	 * last process is finally completed (assuming that
	 * nothing else has changed for the group in the meantime, in
	 * terms of composition of the group and active/inactive state of child
	 * groups and processes). To accomplish this, an additional
	 * pending-request counter must be added to entities, and must
	 * be updated correctly. To avoid this additional field and operations,
	 * we resort to the following tradeoff between simplicity and
	 * accuracy: for an inactive group that is still counted in
	 * num_groups_with_pending_reqs, we decrement
	 * num_groups_with_pending_reqs when the first
	 * process of the group remains with no request waiting for
	 * completion.
	 *
	 * Even this simpler decrement strategy requires a little
	 * carefulness: to avoid multiple decrements, we flag a group,
	 * more precisely an entity representing a group, as still
	 * counted in num_groups_with_pending_reqs when it becomes
	 * inactive. Then, when the first queue of the
	 * entity remains with no request waiting for completion,
	 * num_groups_with_pending_reqs is decremented, and this flag
	 * is reset. After this flag is reset for the entity,
	 * num_groups_with_pending_reqs won't be decremented any
	 * longer in case a new queue of the entity remains
	 * with no request waiting for completion.
	 */
	/* [한국어] 원본 영어 주석에 설명된 대로 정확한 계산이 어려워 근사치로
	 * 관리되는 "in-flight request를 가진 것으로 간주되는 cgroup 수". NVMe
	 * 관점에서는 여러 cgroup(tenant)이 얼마나 동시에 in-flight I/O를 갖는지를
	 * 보여주는 지표로, 대칭/비대칭 시나리오 판정(bfq_better_to_idle())에 쓰인다.
	 * 설정자: bfq_add_bfqq_in_groups_with_pending_reqs()가 증가,
	 *   bfq_del_bfqq_in_groups_with_pending_reqs()가 (엔티티의
	 *   in_groups_with_pending_reqs 플래그를 보며) 감소.
	 * 읽는 자: 비대칭 시나리오 판정 로직.
	 * 값 범위: 0 이상, CONFIG_BFQ_GROUP_IOSCHED가 켜진 빌드에서만 존재.
	 * 동기화: bfqd->lock. */
	unsigned int num_groups_with_pending_reqs;
#endif

	/*
	 * Per-class (RT, BE, IDLE) number of bfq_queues containing
	 * requests (including the queue in service, even if it is
	 * idling).
	 */
	/* [한국어] ioprio_class(RT=0/BE=1/IDLE=2)별로 request를 가진(현재
	 * idling 중이라도 서비스 중인 큐 포함) bfq_queue 개수. 장치에 후보로
	 * 나설 수 있는 큐가 클래스별로 몇 개인지를 나타내며, 클래스 간 우선순위
	 * 판단과 병렬도 추정에 쓰인다.
	 * 설정자: bfq_add_bfqq_busy()/bfq_del_bfqq_busy()가 큐 활성화/비활성화
	 *   시 해당 클래스 인덱스를 증감.
	 * 읽는 자: bfq_tot_busy_queues() 등 총 busy 큐 수 계산 로직.
	 * 값 범위: 0 이상, 배열 크기 3(RT/BE/IDLE).
	 * 동기화: bfqd->lock. */
	unsigned int busy_queues[3];
	/* number of weight-raised busy @bfq_queues */
	/* [한국어] 현재 weight-raised(WR) 상태이면서 busy한 bfq_queue 개수.
	 * 장치에서 latency-sensitive(대화형/soft-RT)로 취급되어 우선 처리
	 * 중인 큐가 몇 개인지 나타내며, WR 대상이 없을 때(0) idling 정책이
	 * 완화될 수 있다.
	 * 설정자: WR 시작/종료 시 증감.
	 * 읽는 자: bfq_better_to_idle() 등이 WR 큐 존재 여부로 idling 필요성 판단.
	 * 값 범위: [0, busy_queues 합계 이하].
	 * 동기화: bfqd->lock. */
	int wr_busy_queues;
	/* number of queued requests */
	/* [한국어] 디바이스 전체에서 대기 중인(아직 디스패치되지 않은) request
	 * 총 개수. 모든 bfq_queue.queued[]의 합에 해당하며 장치에 아직
	 * 밀어넣지 못한 backlog 크기를 나타낸다.
	 * 설정자: request 삽입/제거 시 증감.
	 * 읽는 자: elevator 코어가 has_work 콜백에서 참조.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int queued;
	/* number of requests dispatched and waiting for completion */
	/* [한국어] 모든 actuator를 합쳐 현재 드라이버(디스크/NVMe 컨트롤러)에
	 * 디스패치되어 완료를 기다리는 request 총 개수. NVMe의 CID/tag 또는
	 * SQ 엔트리 사용량 전체를 반영하는 값이다.
	 * 설정자: 디스패치 시 증가, 완료 시 감소.
	 * 읽는 자: hw_tag 추정, injection 판단 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int tot_rq_in_driver;
	/*
	 * number of requests dispatched and waiting for completion
	 * for each actuator
	 */
	/* [한국어] actuator(인덱스)별로 세분화한 in-flight request 개수.
	 * tot_rq_in_driver의 actuator별 분해값이며, actuator마다 별도의 NVMe
	 * queue 쌍/SQ 깊이 관리가 필요하므로 개별 추적이 필수적이다.
	 * 설정자: 디스패치/완료 시 해당 actuator 인덱스를 증감.
	 * 읽는 자: actuator_load_threshold와 비교해 underutilized 여부 판단,
	 *   injection 후보 선택 로직.
	 * 값 범위: 0 이상, 배열 크기 BFQ_MAX_ACTUATORS.
	 * 동기화: bfqd->lock. */
	int rq_in_driver[BFQ_MAX_ACTUATORS];
	/* true if the device is non rotational and performs queueing */
	/* [한국어] 디바이스가 non-rotational(HDD가 아님, 즉 SSD/NVMe)이면서
	 * 내부적으로 큐잉(NCQ/다중 태그)을 수행하는지 여부. true이면 여러
	 * idling/injection 휴리스틱이 "NVMe형" 동작으로 전환된다(순차성보다
	 * 병렬성이 중요해짐).
	 * 설정자: bfq_init_queue()가 blk_queue_nonrot()/큐잉 지원 여부로 초기화.
	 * 읽는 자: bfq_bfqq_may_idle() 등 idling 여부 판단 로직 전반.
	 * 값 범위: bool.
	 * 동기화: 초기화 후 거의 불변. */
	bool nonrot_with_queueing;

	/*
	 * Maximum number of requests in driver in the last
	 * @hw_tag_samples completed requests.
	 */
	/* [한국어] 최근 hw_tag_samples개의 완료 구간 동안 관측된 tot_rq_in_driver
	 * 최댓값. 드라이버가 실제로 여러 태그를 동시에 처리하는지(장치의 실질
	 * 병렬 처리 능력)를 추정하는 입력이다.
	 * 설정자: 완료 처리 경로가 매 샘플마다 최댓값 갱신.
	 * 읽는 자: hw_tag 판정 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int max_rq_in_driver;
	/* number of samples used to calculate hw_tag */
	/* [한국어] hw_tag 판정을 위해 누적된 완료 샘플 개수. 일정 개수가 쌓이면
	 * max_rq_in_driver를 근거로 hw_tag를 확정하고 카운터를 리셋한다.
	 * 설정자: 완료 처리 경로가 매번 증가, 판정 후 0으로 리셋.
	 * 읽는 자: hw_tag 재평가 시점 판단.
	 * 값 범위: 0 이상, 임계치(예: 50) 도달 시 리셋.
	 * 동기화: bfqd->lock. */
	int hw_tag_samples;
	/* flag set to one if the driver is showing a queueing behavior */
	/* [한국어] 드라이버(NVMe 등)가 실제로 큐잉 동작(동시에 여러 request
	 * in-flight)을 보이는지 나타내는 판정 결과. 1이면 idling을 하지 않고도
	 * 병렬 SQ 사용으로 처리량을 낼 수 있다고 보아 여러 idling 휴리스틱을
	 * 완화한다.
	 * 설정자: hw_tag_samples가 임계치에 도달했을 때 max_rq_in_driver 기준으로 설정.
	 * 읽는 자: bfq_bfqq_may_idle() 등.
	 * 값 범위: -1(미확정)/0/1.
	 * 동기화: bfqd->lock. */
	int hw_tag;
	/* number of budgets assigned */
	/* [한국어] 지금까지 예산이 할당(재계산)된 총 횟수. 초기 몇 번의 할당
	 * 동안은 peak_rate 추정치가 아직 불안정하므로, 이 카운터로 "충분히
	 * calibration되었는지"를 판단해 예산 계산 방식을 달리한다.
	 * 설정자: __bfq_set_in_service_queue() 등이 예산 재계산 시마다 증가.
	 * 읽는 자: 예산 계산 로직이 초기 warm-up 여부 판단.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int budgets_assigned;

	/*
	 * Timer set when idling (waiting) for the next request from
	 * the queue in service.
	 */
	/* [한국어] 현재 서비스 중인 큐에 대해 "다음 request가 곧 올 것"이라
	 * 기대하며 device idling을 수행할 때 쓰는 hrtimer. 만료 전에 request가
	 * 도착하면 타이머를 취소하고 그 큐를 계속 서비스해 순차성을 잇고,
	 * 끝내 오지 않아 만료되면 다른 큐로 서비스를 전환한다. 즉 이 타이머는
	 * "장치를 잠시 놀리는 대신 순차성/공정성을 산다"는 도박의 만기다.
	 * 설정자: bfq_arm_slice_timer()가 hrtimer_start()로 무장, 콜백
	 *   bfq_idle_slice_timer()가 만료 처리.
	 * 읽는 자: bfq_bfqq_expire() 등이 idling 중인지 확인할 때
	 *   hrtimer_active()로 조회.
	 * 값 범위: struct hrtimer(커널 hrtimer 서브시스템 관리).
	 * 동기화: 콜백은 소프트IRQ 컨텍스트에서 실행되므로 내부에서
	 *   bfqd->lock을 다시 획득해야 함(인터럽트 컨텍스트 재진입 고려). */
	struct hrtimer idle_slice_timer;
	/* bfq_queue in service */
	/* [한국어] 현재 실제로 디스패치가 진행 중인 최상위 bfq_queue. 장치로
	 * request가 나가고 있는 실질적인 타깃이며, bfqd 전체에서 단 하나만
	 * 존재할 수 있다.
	 * 설정자: __bfq_set_in_service_queue()가 설정, 만료 시
	 *   __bfq_bfqd_reset_in_service()가 NULL로 리셋.
	 * 읽는 자: 디스패치 경로 전체, injection 판단 로직.
	 * 값 범위: 유효 포인터 또는 아무 것도 서비스 중이 아니면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_queue *in_service_queue;
	/* on-disk position of the last served request */
	sector_t last_position;
	/* [한국어] 디바이스 전체에서 마지막으로 서비스된 request의 LBA. 다음
	 * request와의 거리로 전역적인 순차성/탐색 비용을 추정하며,
	 * bfq_choose_req()의 다음 후보 선택과 peak_rate 표본의 순차성 판정에
	 * 쓰인다.
	 * 설정자: bfq_dispatch_request()가 매 디스패치마다 갱신.
	 * 읽는 자: bfq_rq_close() 등 근접성 판정 로직.
	 * 값 범위: 0 이상의 섹터 오프셋.
	 * 동기화: bfqd->lock. */
	/* position of the last served request for the in-service queue */
	sector_t in_serv_last_pos;
	/* [한국어] last_position과 유사하지만 in_service_queue 하나에 국한된
	 * 마지막 서비스 위치. 현재 서비스 중인 큐 자체의 순차성(다음 request가
	 * 이어질지)을 판단할 때 last_position보다 더 정확한 기준이 된다.
	 * 설정자: in_service_queue에서 request가 디스패치될 때 갱신.
	 * 읽는 자: 현재 서비스 큐의 sequentiality 판정 로직.
	 * 값 범위: 0 이상의 섹터 오프셋.
	 * 동기화: bfqd->lock. */
	/* time of last request completion (ns) */
	u64 last_completion;
	/* [한국어] 디바이스 전체에서 마지막 request가 완료된 시각(ns, NVMe CQ
	 * 완료/인터럽트 처리 시점에 해당). injection 여부 판단 및 latency
	 * 계산의 기준 시각으로 쓰인다.
	 * 설정자: bfq_completed_request()가 매 완료마다 갱신.
	 * 읽는 자: bfq_select_queue()의 injection 판단, waker 탐지 로직.
	 * 값 범위: ktime 단조 시계 값.
	 * 동기화: bfqd->lock. */
	/* bfqq owning the last completed rq */
	struct bfq_queue *last_completed_rq_bfqq;
	/* [한국어] 방금 완료된 request를 소유했던 bfq_queue. 이 큐가 waker인지,
	 * 어떤 큐가 그 직후 새 I/O를 받는지(woken 관계)를 관측하는 데 쓰인다.
	 * 설정자: bfq_completed_request()가 완료 시마다 갱신.
	 * 읽는 자: bfq_check_waker() 등 waker/woken 탐지 로직.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	/* last bfqq created, among those in the root group */
	struct bfq_queue *last_bfqq_created;
	/* [한국어] 루트 그룹(cgroup을 쓰지 않는 경우 사실상 디바이스 전체)에서
	 * 가장 최근에 생성된 bfq_queue. bfq_entity.last_bfqq_created와 같은
	 * 목적(stable-merge 후보 판단)을 최상위 레벨에서 담당한다.
	 * 설정자: 루트 그룹 아래에서 새 큐가 생성될 때 갱신.
	 * 읽는 자: stable-merge 후보 판단 로직.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	/* time of last transition from empty to non-empty (ns) */
	u64 last_empty_occupied_ns;
	/* [한국어] 디바이스 전체가 비어있다가(대기 request 없음) 다시 request를
	 * 받게 된 마지막 전환 시각(ns). 관찰 구간(observation window) 계산의
	 * 시작점으로 쓰여 peak_rate 추정에 반영된다.
	 * 설정자: 큐 활성화 경로가 empty->non-empty 전이 시 갱신.
	 * 읽는 자: peak_rate/throughput 추정 로직.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */

	/*
	 * Flag set to activate the sampling of the total service time
	 * of a just-arrived first I/O request (see
	 * bfq_update_inject_limit()). This will cause the setting of
	 * waited_rq when the request is finally dispatched.
	 */
	bool wait_dispatch;
	/* [한국어] 큐가 비어있다가 막 도착한 첫 request의 "총 서비스 시간"을
	 * 샘플링하기 위해 활성화하는 플래그. true이면 이 request가 실제로
	 * 디스패치될 때 waited_rq에 기록되어, 완료 시 inject_limit 재계산의
	 * 트리거가 된다.
	 * 설정자: 큐가 empty->non-empty로 전이할 때 bfq_update_inject_limit()
	 *   경로가 true로 설정.
	 * 읽는 자: 디스패치 처리 로직이 waited_rq를 설정할지 판단.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	/*
	 *  If set, then bfq_update_inject_limit() is invoked when
	 *  waited_rq is eventually completed.
	 */
	struct request *waited_rq;
	/* [한국어] wait_dispatch로 샘플링이 트리거된 그 request 자체에 대한
	 * 포인터. 이 request가 완료(NVMe CQ 완료)되면 bfq_update_inject_limit()이
	 * 호출되어 last_serv_time_ns/inject_limit이 갱신된다.
	 * 설정자: wait_dispatch가 true인 상태에서 request가 디스패치될 때 설정.
	 * 읽는 자: bfq_completed_request()가 이 request의 완료인지 확인.
	 * 값 범위: 유효 request 포인터 또는 NULL(샘플링 중이 아님).
	 * 동기화: bfqd->lock. */
	/*
	 * True if some request has been injected during the last service hole.
	 */
	bool rqs_injected;
	/* [한국어] 방금 지난 "서비스 공백(service hole)" 동안 다른 큐의
	 * request가 injection되었는지 여부. 총 서비스 시간 샘플이 injection의
	 * 영향을 받았는지 구분해 inject_limit 계산의 정확도를 높인다.
	 * 설정자: injection이 실제로 일어날 때 true로 설정.
	 * 읽는 자: bfq_update_inject_limit()이 샘플 유효성 판단 시 참조.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	/* time of first rq dispatch in current observation interval (ns) */
	u64 first_dispatch;
	/* [한국어] 현재 throughput 관찰 구간에서 첫 번째 request가 디스패치된
	 * 시각(ns). delta_from_first 계산의 시작점이다.
	 * 설정자: 새 관찰 구간이 시작될 때 갱신.
	 * 읽는 자: peak_rate 갱신 로직.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	/* time of last rq dispatch in current observation interval (ns) */
	u64 last_dispatch;
	/* [한국어] 현재 관찰 구간에서 가장 최근에 request가 디스패치된 시각(ns).
	 * 설정자: 매 디스패치마다 갱신.
	 * 읽는 자: peak_rate 갱신 로직이 구간 길이 계산에 사용.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	/* beginning of the last budget */
	ktime_t last_budget_start;
	/* [한국어] 현재(또는 마지막) 예산 서비스 슬라이스가 시작된 시각. 예산
	 * 소비 속도를 측정해 장치 서비스 슬라이스 길이를 검증하는 데 쓰인다.
	 * 설정자: __bfq_set_in_service_queue()가 서비스 시작 시 갱신.
	 * 읽는 자: 예산 소비 시간 계산 로직.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	/* beginning of the last idle slice */
	ktime_t last_idling_start;
	/* [한국어] 마지막으로 device idling을 시작한 시각(ktime). 만료 시점에
	 * 이 값과의 차이로 "실제로 얼마나 오래 장치를 비워 두었는지"를 구해,
	 * budget 소진이 느렸던 원인이 큐 탓인지 idling 탓인지 구분하는 데
	 * 쓰인다(bfq_bfqq_expire()의 slow 판정).
	 * 설정자: bfq_arm_slice_timer()가 idling 시작 시 갱신.
	 * 읽는 자: idling 종료 시 지속시간 계산 로직.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	unsigned long last_idling_start_jiffies;
	/* [한국어] last_idling_start와 같은 사건을 jiffies 단위로도 기록한
	 * 값. hrtimer 콜백처럼 jiffies 비교가 더 저렴한 경로에서 사용된다.
	 * 설정자: last_idling_start와 함께 갱신.
	 * 읽는 자: idling 관련 jiffies 기반 비교 로직.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/* number of samples in current observation interval */
	int peak_rate_samples;
	/* [한국어] 현재 관찰 구간에서 누적된 dispatch 샘플 수. 일정 수 이상
	 * 쌓여야 peak_rate 추정치를 신뢰할 수 있다고 판단한다.
	 * 설정자: 매 디스패치마다 증가, 구간 리셋 시 0으로.
	 * 읽는 자: peak_rate 갱신 로직의 신뢰도 판단.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/* num of samples of seq dispatches in current observation interval */
	u32 sequential_samples;
	/* [한국어] 현재 관찰 구간에서 "순차적(sequential)"으로 판정된 디스패치
	 * 샘플 수. 전체 샘플 대비 이 비율이 3/4에 못 미치면 그 관측 구간은
	 * 랜덤 성분이 커서 장치의 "최고" 처리율을 대표하지 못한다고 보고
	 * peak_rate 갱신에서 배제한다.
	 * 설정자: 매 디스패치마다 순차 여부 판정 후 증가.
	 * 읽는 자: peak_rate 계산 시 순차성 가중치 반영.
	 * 값 범위: 0 이상, peak_rate_samples 이하.
	 * 동기화: bfqd->lock. */
	/* total num of sectors transferred in current observation interval */
	u64 tot_sectors_dispatched;
	/* [한국어] 현재 관찰 구간 동안 디스패치된 섹터 수의 누적 합.
	 * delta_from_first로 나누어 throughput(peak_rate)을 계산하는 분자다.
	 * 설정자: 매 디스패치마다 request 크기(섹터)만큼 누적.
	 * 읽는 자: peak_rate 갱신 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/* max rq size seen during current observation interval (sectors) */
	u32 last_rq_max_size;
	/* [한국어] 현재 관찰 구간에서 관측된 가장 큰 request 크기(섹터).
	 * 마지막 dispatch 이후 아직 완료되지 않은 요청이 있을 때, 관측 구간을
	 * 어디까지 늘려 잡을지를 이 크기로 보정한다. 32번의 dispatch마다
	 * 리셋되어 오래된 이상치가 영구히 남지 않는다.
	 * 설정자: 매 디스패치마다 request 크기와 비교해 최댓값 갱신.
	 * 읽는 자: peak_rate 계산 로직.
	 * 값 범위: 0 이상(섹터 단위).
	 * 동기화: bfqd->lock. */
	/* time elapsed from first dispatch in current observ. interval (us) */
	u64 delta_from_first;
	/* [한국어] first_dispatch로부터 현재까지 경과한 시간(마이크로초).
	 * tot_sectors_dispatched를 이 값으로 나누어 peak_rate를 계산한다.
	 * 설정자: peak_rate 재계산 시점마다 갱신.
	 * 읽는 자: peak_rate 계산식의 분모.
	 * 값 범위: 0 이상(us 단위).
	 * 동기화: bfqd->lock. */
	/*
	 * Current estimate of the device peak rate, measured in
	 * [(sectors/usec) / 2^BFQ_RATE_SHIFT]. The left-shift by
	 * BFQ_RATE_SHIFT is performed to increase precision in
	 * fixed-point calculations.
	 */
	u32 peak_rate;
	/* [한국어] 디바이스의 추정 최대 처리율(고정소수점, 2^BFQ_RATE_SHIFT
	 * 배율 적용). NVMe SSD의 실제 대역폭을 반영해 예산 크기(bfq_max_budget)
	 * 계산 및 idling 정책의 이득/손실 판단에 쓰인다.
	 * 설정자: 관찰 구간이 끝날 때마다 tot_sectors_dispatched/delta_from_first로 재계산.
	 * 읽는 자: 예산 계산 로직, bfq_wr_duration() 등 rate_dur_prod 계산.
	 * 값 범위: 0 초과, 고정소수점 인코딩.
	 * 동기화: bfqd->lock. */
	/* maximum budget allotted to a bfq_queue before rescheduling */
	int bfq_max_budget;
	/* [한국어] 자동 계산(peak_rate 기반) 또는 사용자 설정으로 정해진, 한
	 * bfq_queue가 재스케줄되기 전까지 가질 수 있는 최대 예산. 이 값이 각
	 * 큐의 max_budget 상한이 되어 장치 독점을 막는다.
	 * 설정자: bfq_update_dispatch_stats() 계열이 peak_rate/bfq_user_max_budget
	 *   기반으로 재계산.
	 * 읽는 자: 각 bfq_queue의 max_budget 산정 로직.
	 * 값 범위: 0 초과.
	 * 동기화: bfqd->lock. */

	/*
	 * List of all the bfq_queues active for a specific actuator
	 * on the device. Keeping active queues separate on a
	 * per-actuator basis helps implementing per-actuator
	 * injection more efficiently.
	 */
	struct list_head active_list[BFQ_MAX_ACTUATORS];
	/* [한국어] actuator별로 현재 active(backlogged)한 bfq_queue들을 모은
	 * 리스트. B-WF2Q+ 서비스 트리와는 별개의 순회용 인덱스로, 특정
	 * actuator가 underutilized일 때 injection 후보를 빠르게 찾기 위해
	 * actuator 단위로 분리해 둔다.
	 * 설정자: 큐가 활성화/비활성화될 때 bfqq_list를 통해 연결/해제.
	 * 읽는 자: injection 후보 탐색 로직이 해당 actuator의 리스트만 순회.
	 * 값 범위: 배열 크기 BFQ_MAX_ACTUATORS, 각 원소는 리스트 헤드.
	 * 동기화: bfqd->lock. */
	/* list of all the bfq_queues idle on the device */
	struct list_head idle_list;
	/* [한국어] 더 이상 backlogged는 아니지만 아직 완전히 제거되지 않은
	 * bfq_queue들의 리스트. NVMe CQ 완료 이후 다시 활성화(재-backlogged)될
	 * 가능성이 있는 큐들을 추적한다.
	 * 설정자: 큐가 idle로 전이될 때 연결, 재활성화/소멸 시 해제.
	 * 읽는 자: 유휴 큐 정리/통계 로직.
	 * 값 범위: 리스트 헤드.
	 * 동기화: bfqd->lock. */

	/*
	 * Timeout for async/sync requests; when it fires, requests
	 * are served in fifo order.
	 */
	u64 bfq_fifo_expire[2];
	/* [한국어] [0]=async, [1]=sync request가 FIFO 순서로 강제 서비스되기까지
	 * 허용되는 타임아웃(ns). LBA 정렬(sort_list)만 따르면 오래된 request가
	 * 계속 밀릴 수 있어, NVMe timeout/abort와 유사하게 "이 시간이 지나면
	 * 무조건 서비스"하는 상한을 둔다.
	 * 설정자: sysfs 튜너블(bfq_fifo_expire_sync/async)이 사용자 설정으로 갱신.
	 * 읽는 자: bfq_check_fifo()가 FIFO 헤드의 만료 여부 검사에 사용.
	 * 값 범위: 0 초과(ns 단위).
	 * 동기화: bfqd->lock (설정 변경은 sysfs write 경로에서 큐 상태와 함께 갱신). */
	/* weight of backward seeks wrt forward ones */
	unsigned int bfq_back_penalty;
	/* [한국어] 헤드 이동 방향 기준으로 "뒤로" 가는 seek이 "앞으로" 가는
	 * seek보다 몇 배 더 비싼 것으로 취급할지의 배율. NVMe는 물리적 헤드가
	 * 없어 이 개념이 상대적으로 약하지만, LBA 재정렬 시 여전히 후방 접근을
	 * 불리하게 평가하는 데 쓰인다.
	 * 설정자: sysfs 튜너블(back_seek_penalty)이 갱신.
	 * 읽는 자: bfq_choose_req()가 두 request 중 어느 쪽이 가까운지 비교할 때.
	 * 값 범위: 1 이상(배율).
	 * 동기화: bfqd->lock. */
	/* maximum allowed backward seek */
	unsigned int bfq_back_max;
	/* [한국어] "허용 가능한" 후방 seek의 최대 거리(섹터). 이 거리를 넘어서는
	 * 후방 request는 순차 후보로 고려하지 않는다.
	 * 설정자: sysfs 튜너블(back_seek_max)이 갱신.
	 * 읽는 자: bfq_choose_req()의 근접성 판정.
	 * 값 범위: 0 이상(섹터 단위).
	 * 동기화: bfqd->lock. */
	/* maximum idling time */
	u32 bfq_slice_idle;
	/* [한국어] device idling을 유지할 최대 시간. idle_slice_timer의
	 * 무장(arm) 길이로 쓰이며, "순차성/공정성을 얻기 위해 장치를 얼마나
	 * 오래 놀릴 것인가"라는 트레이드오프를 직접 조절하는 튜너블이다.
	 * 0으로 두면 idling이 완전히 꺼진다 - 내부 병렬성이 높아 한 큐를
	 * 기다리는 것이 곧 처리량 손실인 장치에서 그렇게 설정한다.
	 * 설정자: sysfs 튜너블(slice_idle)이 갱신.
	 * 읽는 자: bfq_arm_slice_timer()가 hrtimer 만료 시간 설정에 사용.
	 * 값 범위: 0(idling 비활성) 이상(ns 단위).
	 * 동기화: bfqd->lock. */
	/* user-configured max budget value (0 for auto-tuning) */
	int bfq_user_max_budget;
	/* [한국어] 사용자가 sysfs로 직접 지정한 최대 예산 값. 0이면 자동
	 * 조정(peak_rate 기반)을 뜻하며, 그 외에는 이 값이 bfq_max_budget
	 * 계산의 상한으로 강제된다(장치 depth의 수동 상한 설정에 해당).
	 * 설정자: sysfs 튜너블(max_budget) write 경로가 갱신.
	 * 읽는 자: bfq_max_budget 재계산 로직.
	 * 값 범위: 0(자동) 또는 양수.
	 * 동기화: bfqd->lock. */
	/*
	 * Timeout for bfq_queues to consume their budget; used to
	 * prevent seeky queues from imposing long latencies to
	 * sequential or quasi-sequential ones (this also implies that
	 * seeky queues cannot receive guarantees in the service
	 * domain; after a timeout they are charged for the time they
	 * have been in service, to preserve fairness among them, but
	 * without service-domain guarantees).
	 */
	unsigned int bfq_timeout;
	/* [한국어] 한 큐가 예산을 다 쓰지 못했더라도 강제로 만료되는
	 * 시간(jiffies). seeky 큐가 예산을 오래 붙들어 순차적인 큐의 장치
	 * latency를 해치지 못하도록 하는 시간 상한이며, bfq_queue.budget_timeout
	 * 계산의 기준값이다.
	 * 설정자: sysfs 튜너블(timeout_sync)이 갱신, extern bfq_timeout이 기본값 제공.
	 * 읽는 자: __bfq_set_in_service_queue()가 budget_timeout 설정 시 사용.
	 * 값 범위: 0 초과(jiffies 단위).
	 * 동기화: bfqd->lock. */

	/*
	 * Force device idling whenever needed to provide accurate
	 * service guarantees, without caring about throughput
	 * issues. CAVEAT: this may even increase latencies, in case
	 * of useless idling for processes that did stop doing I/O.
	 */
	bool strict_guarantees;
	/* [한국어] true이면 throughput 손실을 감수하고서라도 필요할 때마다
	 * 강제로 idling을 수행해 서비스 보장(fairness/latency)을 정확히
	 * 지킨다. 원본 주석의 경고처럼, I/O를 멈춘 프로세스에 대해 불필요한
	 * idling으로 오히려 다른 큐의 latency가 늘 수 있다.
	 * 설정자: sysfs 튜너블(strict_guarantees)이 갱신.
	 * 읽는 자: bfq_bfqq_may_idle() 등 idling 여부 판단 로직.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	/*
	 * Last time at which a queue entered the current burst of
	 * queues being activated shortly after each other; for more
	 * details about this and the following parameters related to
	 * a burst of activations, see the comments on the function
	 * bfq_handle_burst.
	 */
	unsigned long last_ins_in_burst;
	/* [한국어] 현재 진행 중인 "활성화 버스트"에 큐가 마지막으로 편입된
	 * 시각(jiffies). 다음 큐 생성이 bfq_burst_interval 이내에 일어나면 같은
	 * 버스트로 간주해 장치 activation 폭주를 감지한다.
	 * 설정자: bfq_handle_burst()가 새 큐가 버스트에 편입될 때마다 갱신.
	 * 읽는 자: 동일 함수가 다음 큐와의 시간 간격 비교에 사용.
	 * 값 범위: jiffies 값.
	 * 동기화: bfqd->lock. */
	/*
	 * Reference time interval used to decide whether a queue has
	 * been activated shortly after @last_ins_in_burst.
	 */
	unsigned long bfq_burst_interval;
	/* [한국어] last_ins_in_burst로부터 이 시간 이내에 새 큐가 활성화되면
	 * "같은 버스트"로 판정하는 기준 간격(jiffies).
	 * 설정자: bfq_init_queue()가 상수로 초기화(사용자 튜너블 아님).
	 * 읽는 자: bfq_handle_burst()의 시간 간격 비교.
	 * 값 범위: 0 초과(jiffies 단위).
	 * 동기화: 초기화 후 불변. */
	/* number of queues in the current burst of queue activations */
	int burst_size;
	/* [한국어] 현재 진행 중인 버스트에 속한 큐의 개수. 이 값이
	 * bfq_large_burst_thresh를 넘으면 large_burst로 승격되어 장치
	 * 경쟁 완화를 위한 추가 정책(예: WR 억제)이 적용된다.
	 * 설정자: bfq_handle_burst()가 버스트에 큐가 추가될 때마다 증가.
	 * 읽는 자: large_burst 승격 조건 검사.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	/* common parent entity for the queues in the burst */
	struct bfq_entity *burst_parent_entity;
	/* [한국어] 현재 버스트에 속한 큐들이 공통으로 속하는 상위 entity(대개
	 * 같은 cgroup의 entity). 버스트가 특정 cgroup 안에서만 발생했는지
	 * 판단해 장치 cgroup 자원 공유 정책에 반영한다.
	 * 설정자: 버스트 시작 시 첫 큐의 부모 entity로 설정.
	 * 읽는 자: 새 큐가 버스트에 편입될 자격이 있는지(같은 부모인지) 검사.
	 * 값 범위: 유효 포인터 또는 버스트가 없으면 NULL.
	 * 동기화: bfqd->lock. */
	/* Maximum burst size above which the current queue-activation
	 * burst is deemed as 'large'.
	 */
	unsigned long bfq_large_burst_thresh;
	/* [한국어] burst_size가 이 값을 넘으면 large_burst로 판정하는 임계치.
	 * 설정자: bfq_init_queue()가 상수로 초기화.
	 * 읽는 자: bfq_handle_burst()의 large_burst 승격 판정.
	 * 값 범위: 0 초과.
	 * 동기화: 초기화 후 불변. */
	/* true if a large queue-activation burst is in progress */
	bool large_burst;
	/* [한국어] 현재 대형 버스트가 진행 중임을 나타내는 플래그. true이면
	 * 새로 생성되는 큐들에 대해 WR을 억제하는 등, 장치 스케줄러가
	 * 폭주 상황에 대응하는 보수적 정책을 적용한다.
	 * 설정자: burst_size가 임계치를 넘을 때 true로, 버스트가 해소되면 false로.
	 * 읽는 자: WR 시작 여부 판단 로직.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	/*
	 * Head of the burst list (as for the above fields, more
	 * details in the comments on the function bfq_handle_burst).
	 */
	struct hlist_head burst_list;
	/* [한국어] 현재 버스트에 속한 bfq_queue들을 연결하는 해시리스트 헤드
	 * (각 큐의 burst_list_node로 연결). bfq_queue 생명주기 관리를
	 * 위해 버스트 전체를 순회할 때 쓰인다.
	 * 설정자: bfq_handle_burst()가 큐를 hlist_add_head()로 추가.
	 * 읽는 자: 버스트 정리/통계 로직.
	 * 값 범위: 비어있거나 hlist_node로 연결된 큐 집합.
	 * 동기화: bfqd->lock. */
	/* if set to true, low-latency heuristics are enabled */
	bool low_latency;
	/* [한국어] true이면 대화형/soft-RT 프로세스를 위한 low-latency 휴리스틱
	 * (WR 등)이 활성화된다. NVMe 관점에서 대화형 워크로드의 SQ 우선순위를
	 * 높이는 기능 전체의 on/off 스위치다.
	 * 설정자: sysfs 튜너블(low_latency)이 갱신.
	 * 읽는 자: WR 시작 여부를 판단하는 모든 코드 경로의 최상위 게이트.
	 * 값 범위: bool.
	 * 동기화: bfqd->lock. */
	/*
	 * Maximum factor by which the weight of a weight-raised queue
	 * is multiplied.
	 */
	unsigned int bfq_wr_coeff;
	/* [한국어] WR이 적용될 때 곱해지는 배율의 상한. bfq_queue.wr_coeff가
	 * 이 값까지 오를 수 있으며, 장치에서 latency-sensitive 큐가 받을 수
	 * 있는 최대 우선도를 제한한다.
	 * 설정자: sysfs 튜너블(low_latency 관련) 또는 자동 계산 로직이 설정.
	 * 읽는 자: WR 시작 시 wr_coeff에 대입되는 상한값.
	 * 값 범위: 1 이상.
	 * 동기화: bfqd->lock. */
	/* Maximum weight-raising duration for soft real-time processes */
	unsigned int bfq_wr_rt_max_time;
	/* [한국어] soft real-time으로 판정된 프로세스에 대한 WR 최대 지속
	 * 시간(jiffies). soft-RT QoS를 보장하는 시간 창의
	 * 상한이다.
	 * 설정자: sysfs 튜너블 또는 자동 계산(rate_dur_prod 기반)이 설정.
	 * 읽는 자: wr_cur_max_time 계산 로직.
	 * 값 범위: 0 초과(jiffies 단위).
	 * 동기화: bfqd->lock. */
	/*
	 * Minimum idle period after which weight-raising may be
	 * reactivated for a queue (in jiffies).
	 */
	unsigned int bfq_wr_min_idle_time;
	/* [한국어] 큐가 이만큼 idle 상태로 있었어야만 WR을 재활성화할 수 있는
	 * 최소 유휴 시간(jiffies). 너무 자주 WR을 재시작해 fairness를 해치는
	 * 것을 막는다.
	 * 설정자: bfq_init_queue()가 상수로 초기화.
	 * 읽는 자: WR 재활성화 조건 판단 로직.
	 * 값 범위: 0 초과(jiffies 단위).
	 * 동기화: 초기화 후 불변. */
	/*
	 * Minimum period between request arrivals after which
	 * weight-raising may be reactivated for an already busy async
	 * queue (in jiffies).
	 */
	unsigned long bfq_wr_min_inter_arr_async;
	/* [한국어] 이미 busy한 async 큐에 대해 WR을 재활성화하려면 request
	 * 도착 간격이 이만큼은 벌어져 있어야 한다는 최소 시간(jiffies). 장치
	 * async 경로에서의 WR 남용을 막는 정책이다.
	 * 설정자: bfq_init_queue()가 상수로 초기화.
	 * 읽는 자: async 큐의 WR 재활성화 조건 판단.
	 * 값 범위: 0 초과(jiffies 단위).
	 * 동기화: 초기화 후 불변. */
	/* Max service-rate for a soft real-time queue, in sectors/sec */
	unsigned int bfq_wr_max_softrt_rate;
	/* [한국어] soft-RT로 인정되는 큐가 가질 수 있는 최대 서비스율
	 * (섹터/초). 이보다 빠른 처리율을 요구하는 큐는 진짜 soft-RT가 아니라고
	 * 판단해 장치 대역폭 한계를 강제한다.
	 * 설정자: bfq_init_queue()가 상수로 초기화.
	 * 읽는 자: soft-RT 판정 로직(bfq_bfqq_softrt_next_start() 등).
	 * 값 범위: 0 초과(섹터/초 단위).
	 * 동기화: 초기화 후 불변. */
	/*
	 * Cached value of the product ref_rate*ref_wr_duration, used
	 * for computing the maximum duration of weight raising
	 * automatically.
	 */
	u64 rate_dur_prod;
	/* [한국어] 기준 처리율(ref_rate)과 기준 WR 지속시간(ref_wr_duration)의
	 * 곱을 캐시해둔 값. 실제 peak_rate가 기준과 다를 때 "처리율에 반비례한"
	 * WR 지속시간을 자동으로 계산하는 데 재사용된다(WR duration 자동 계산).
	 * 설정자: bfq_init_queue()가 초기 상수로 설정.
	 * 읽는 자: wr_cur_max_time 자동 계산 로직이 peak_rate로 나눔.
	 * 값 범위: 0 초과.
	 * 동기화: 초기화 후 불변. */
	/* fallback dummy bfqq for extreme OOM conditions */
	struct bfq_queue oom_bfqq;
	/* [한국어] 메모리 부족(OOM)으로 정상적인 bfq_queue를 새로 할당할 수
	 * 없을 때 사용하는 비상용 더미 큐. 이 큐를 쓰면 정상적인 fairness는
	 * 보장되지 않지만 최소한 request가 드라이버로 나갈 경로 자체는
	 * 유지된다(가용성 우선).
	 * 설정자: bfq_init_queue()가 elevator 초기화 시 1회 초기화.
	 * 읽는 자: bfq_get_queue()가 kmem_cache 할당 실패 시 폴백으로 반환.
	 * 값 범위: 항상 유효(bfqd와 함께 살아있음), ref는 인위적으로 높게 유지.
	 * 동기화: bfqd->lock. */
	spinlock_t lock;
	/* [한국어] 이 bfq_data(및 그 아래 모든 bfq_group/bfq_queue/bfq_entity)를
	 * 보호하는 단일 스핀락. BFQ의 거의 모든 상태 변경은 이 락을 쥔 채
	 * 이루어지며, 완료 콜백(인터럽트/소프트IRQ 컨텍스트)과 제출(프로세스
	 * 컨텍스트) 경로가 이 락으로 상호 배제된다. 장치 단위의 단일 전역
	 * 락이라는 점이 BFQ의 확장성 한계이기도 하다 - 하드웨어 큐가 여럿인
	 * 고 IOPS 장치에서는 이 락이 직렬화 병목이 되어, 그런 장치에서는
	 * none이나 mq-deadline이 기본으로 쓰인다.
	 * 설정자/읽는 자: bfq-iosched.c/bfq-wf2q.c/bfq-cgroup.c 전체.
	 * 값 범위: spinlock_t.
	 * 동기화: 이 필드 자체가 동기화 primitive이며, hrtimer 콜백처럼
	 *   인터럽트 컨텍스트에서 획득할 수도 있어 spin_lock_irq류를 사용. */

	/*
	 * bic associated with the task issuing current bio for
	 * merging. This and the next field are used as a support to
	 * be able to perform the bic lookup, needed by bio-merge
	 * functions, before the scheduler lock is taken, and thus
	 * avoid taking the request-queue lock while the scheduler
	 * lock is being held.
	 */
	struct bfq_io_cq *bio_bic;
	/* [한국어] 현재 처리 중인 bio를 낸 태스크의 bfq_io_cq 캐시. bio-merge
	 * 판단(bfq_bio_merge())이 스케줄러 락을 잡기 전에 미리 bic를 조회할 수
	 * 있게 해, request-queue 락을 쥔 채로 스케줄러 락까지 중첩해서 잡는
	 * 상황(락 순서 역전 위험)을 피한다.
	 * 설정자: bfq_bio_merge()가 병합 판단 시작 시 설정.
	 * 읽는 자: 동일 함수 및 bio_bfqq 조회 경로.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock 없이도 접근 가능하도록 설계된 캐시(원본 주석 참고),
	 *   다만 실제 갱신은 bfqd->lock 보호 하에 이루어짐. */
	/* bfqq associated with the task issuing current bio for merging */
	struct bfq_queue *bio_bfqq;
	/* [한국어] bio_bic로부터 조회된, 현재 bio 병합 후보가 되는
	 * bfq_queue 캐시. 새 request가 기존 큐로 병합될 수
	 * 있는지 판단하는 bio-merge 경로의 중간 결과다.
	 * 설정자: bfq_bio_merge()가 bic_to_bfqq()로 조회 후 설정.
	 * 읽는 자: 동일 함수가 실제 병합 시도 시 사용.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bio_bic과 동일한 완화된 접근 규칙을 따름. */
	/*
	 * Depth limits used in bfq_limit_depth (see comments on the
	 * function)
	 *
	 * async_depths[2][2]는 blk-mq의 태그(sbitmap) 할당 단계에서 한
	 * 프로세스가 잡을 수 있는 태그 수를 제한한다. 장치의 큐 슬롯이
	 * 아니라 request_queue의 nr_requests(태그 풀)를 나누는 것이며,
	 * 목적은 오버플로 방지가 아니라 "쓰기 폭주가 태그를 전부 선점해
	 * 읽기 큐가 request조차 만들지 못하는" 상황을 막는 것이다.
	 * 스케줄러 큐에 들어오기 전 단계라 BFQ의 가중치로는 되돌릴 수
	 * 없기 때문에 이 시점에서 미리 막아야 한다.
	 */
	unsigned int async_depths[2][2];
	/* [한국어] blk_mq_get_request()가 태그(tag)를 할당하기 전에 참조하는
	 * depth(동시 허용 in-flight 개수) 제한 표. 첫 인덱스는 sync/async,
	 * 두 번째 인덱스는 "이 시나리오가 대칭적인지 알려졌는지" 여부로 추정되며,
	 * bfq_limit_depth()가 이 표에서 실제 상한을 뽑아 blk_mq_alloc_request()
	 * 계열에 돌려준다. 특정 종류의 request가 태그 풀을 독점하는 것을 막는
	 * 1차 방어선이다.
	 * 설정자: bfq_depth_updated()가 큐 깊이(nr_requests)/대칭성 변화 시 재계산.
	 * 읽는 자: bfq_limit_depth()가 새 tag 할당 요청마다 조회.
	 * 값 범위: 0 이상, 값이 0이면 사실상 그 조합의 할당을 거의 금지.
	 * 동기화: bfqd->lock. */

	/*
	 * Number of independent actuators. This is equal to 1 in
	 * case of single-actuator drives.
	 *
	 * NVMe 관점: NVMe 컨트롤러가 Independent Access Ranges를
	 * 보고하면 이 값이 1보다 커진다. 각 actuator는 별도의 NVMe queue
	 * 쌍 또는 LBA 범위를 담당할 수 있다(추정).
	 */
	unsigned int num_actuators;
	/* [한국어] 이 디바이스가 가진 독립 actuator(멀티 액추에이터 HDD 또는
	 * NVMe Independent Access Ranges로 추정) 개수. 대부분의 단일
	 * actuator 드라이브에서는 1이며, 이 값이 배열 필드(rq_in_driver[],
	 * active_list[], sector[] 등)의 유효 인덱스 범위를 결정한다.
	 * 설정자: bfq_init_queue()가 disk->ia_ranges 정보로 초기화(고정, 런타임
	 *   불변으로 추정).
	 * 읽는 자: actuator 배열을 순회하는 거의 모든 코드가 상한으로 참조.
	 * 값 범위: [1, BFQ_MAX_ACTUATORS].
	 * 동기화: 초기화 후 불변. */
	/*
	 * Disk independent access ranges for each actuator
	 * in this device.
	 */
	sector_t sector[BFQ_MAX_ACTUATORS];
	/* [한국어] actuator별로 담당하는 LBA 범위의 시작 섹터. bio의 목적지
	 * LBA가 어느 actuator에 속하는지(즉 어느 bfq_queue 열 인덱스로 라우팅할지)
	 * 판단하는 기준값이다.
	 * 설정자: bfq_init_queue()가 disk의 independent_access_ranges 정보로 초기화.
	 * 읽는 자: bfq_actuator_index() 계열이 bio 위치와 비교해 actuator 결정.
	 * 값 범위: 0 이상(섹터 오프셋), 배열 크기 BFQ_MAX_ACTUATORS.
	 * 동기화: 초기화 후 불변. */
	sector_t nr_sectors[BFQ_MAX_ACTUATORS];
	/* [한국어] 각 actuator가 담당하는 LBA 범위의 길이(섹터 수).
	 * sector[i] ~ sector[i]+nr_sectors[i] 구간이 i번째 actuator의 영역이다.
	 * 설정자: sector[]와 함께 초기화.
	 * 읽는 자: actuator 판별 로직이 범위 포함 여부 검사.
	 * 값 범위: 0 이상(섹터 단위).
	 * 동기화: 초기화 후 불변. */
	struct blk_independent_access_range ia_ranges[BFQ_MAX_ACTUATORS];
	/* [한국어] 블록 계층 공통의 Independent Access Range 서술자 배열
	 * 원본. sector[]/nr_sectors[]는 이 정보로부터 뽑아낸 캐시이며, 이
	 * 필드 자체는 디바이스가 보고한 원본 구조체를 그대로 보관한다(NVMe
	 * queue 쌍 매핑 정보로 추정).
	 * 설정자: bfq_init_queue()가 디스크 토폴로지 조회 결과로 초기화.
	 * 읽는 자: 그룹/큐 초기화 시 actuator 개수·경계 확인.
	 * 값 범위: struct blk_independent_access_range 정의 참고.
	 * 동기화: 초기화 후 불변. */

	/*
	 * If the number of I/O requests queued in the device for a
	 * given actuator is below next threshold, then the actuator
	 * is deemed as underutilized. If this condition is found to
	 * hold for some actuator upon a dispatch, but (i) the
	 * in-service queue does not contain I/O for that actuator,
	 * while (ii) some other queue does contain I/O for that
	 * actuator, then the head I/O request of the latter queue is
	 * returned (injected), instead of the head request of the
	 * currently in-service queue.
	 *
	 * We set the threshold, empirically, to the minimum possible
	 * value for which an actuator is fully utilized, or close to
	 * be fully utilized. By doing so, injected I/O 'steals' as
	 * few drive-queue slots as possibile to the in-service
	 * queue. This reduces as much as possible the probability
	 * that the service of I/O from the in-service bfq_queue gets
	 * delayed because of slot exhaustion, i.e., because all the
	 * slots of the drive queue are filled with I/O injected from
	 * other queues (NCQ provides for 32 slots).
	 *
	 * 주의: 이 임계치는 장치 큐 깊이 자체가 아니다. bfq_init_queue()가
	 * 4로 고정 초기화하는 작은 상수이며(런타임 튜너블도 아니다), 뜻은
	 * "이 actuator에 떠 있는 요청이 4개 미만이면 아직 한산하다"이다.
	 * 위 영어 주석의 32는 NCQ가 제공하는 전체 슬롯 수를 배경으로 든 것일
	 * 뿐, 임계치의 근거가 아니다. 값이 작을수록 주입이 in-service 큐의
	 * 슬롯을 덜 빼앗지만, 그만큼 유휴 actuator를 놀리게 된다.
	 */
	unsigned int actuator_load_threshold;
	/* [한국어] rq_in_driver[actuator]가 이 값 미만이면 해당 actuator를
	 * "underutilized"로 판정하는 임계치. 이 조건에서 in-service 큐가 그
	 * actuator에 request가 없고 다른 큐가 있다면, 그 다른 큐의 head
	 * request를 injection해 놀고 있는 actuator를 활용하되, in-service
	 * 큐가 쓰던 장치 큐 슬롯은 최소한만 빼앗는다.
	 * 설정자: bfq_init_queue()가 4로 초기화.
	 * 읽는 자: bfq_find_bfqq_for_underused_actuator()의 판정 로직.
	 * 값 범위: 현재 코드에서는 상수 4.
	 * 동기화: 초기화 이후 이 값을 쓰는 코드가 없어 사실상 불변이다. */
};

/*
 * bfq_queue 상태 플래그. 아래 각 값은 bfq_queue.flags의 비트 인덱스이며,
 * BFQ_BFQQ_FNS 매크로가 생성하는 mark/clear/query 함수 3종의 대상이 된다.
 * flags는 bfqd->lock으로 보호되므로 접근자들은 원자 비트 연산이 아니라
 * 비원자 __set_bit/__clear_bit을 쓴다.
 */
enum bfqq_state_flags {
	BFQQF_just_created = 0,	/* queue just allocated */
	/* [한국어] 이 큐가 방금 kmem_cache_alloc()으로 할당되어 아직 한 번도
	 * 서비스되지 않은 초기 상태임을 표시. 최초 진입 시 과도하게 보수적인
	 * 판단(예: seek 히스토리 없음)을 적용하지 않기 위한 유예 플래그다.
	 * stable merge와 burst 판정도 이 유예 구간에서만 의미가 있다. */
	BFQQF_busy,		/* has requests or is in service */
	/* [한국어] 큐에 request가 있거나 현재 서비스 중임을 표시. 서비스
	 * 트리에 속해 있는지와 밀접하게 연관되며, bfqd->busy_queues[class]
	 * 카운트에 반영되는 기준 플래그다. */
	BFQQF_wait_request,	/* waiting for a request */
	/* [한국어] idling 중이며 다음 request 도착을 기다리고 있음을 표시.
	 * idle_slice_timer가 무장된 상태와 대응된다. request가 도착하면 즉시
	 * 이 플래그를 내리고 타이머를 취소해야 idling이 헛돌지 않는다. */
	BFQQF_non_blocking_wait_rq, /*
				     * waiting for a request
				     * without idling the device
				     */
	/* [한국어] request를 기다리되 device idling(장치를 비워 두는 대기)은
	 * 하지 않는 상태. 큐가 비었지만 아직 만료시키지는 않은 구간을 뜻하며,
	 * 이후 request가 "제때" 도착했는지(arrived_in_time) 판정해 budget을
	 * 재계산할 때 이 플래그가 근거가 된다. */
	BFQQF_fifo_expire,	/* FIFO checked in this slice */
	/* [한국어] 현재 서비스 슬라이스 동안 이미 FIFO 만료 검사를 한 번
	 * 수행했음을 표시 - 같은 슬라이스에서 FIFO 만료로 두 번 강제
	 * 디스패치하지 않도록 막는다. */
	BFQQF_has_short_ttime,	/* queue has a short think time */
	/* [한국어] bfq_ttime.ttime_mean이 짧다고 판정된 상태. 완료 직후 곧바로
	 * 다음 request가 올 것으로 보이므로 idling은 값싸게 성공하고, 반대로
	 * 다른 큐의 injection은 그 짧은 간격을 밀어내므로 억제한다. */
	BFQQF_sync,		/* synchronous queue */
	/* [한국어] O_DIRECT/동기 쓰기 등 동기(sync) I/O를 내는 큐임을 표시.
	 * bfq_io_cq.bfqq[1][*] 행에 대응하며, async 큐와 다른 idling/우선순위
	 * 정책이 적용된다. */
	BFQQF_IO_bound,		/*
				 * bfqq has timed-out at least once
				 * having consumed at most 2/10 of
				 * its budget
				 */
	/* [한국어] 예산의 20% 이하만 쓴 채로 타임아웃된 이력이 있어 "진짜
	 * I/O-bound(지속적으로 I/O를 내는)" 큐로 분류됨을 표시. 예산을 거의
	 * 못 쓰고 시간만 보냈다는 것은 요청을 띄엄띄엄 낸다는 뜻이므로,
	 * 이런 큐를 위해 장치를 비워 두는 것은 대개 손해다. */
	BFQQF_in_large_burst,	/*
				 * bfqq activated in a large burst,
				 * see comments to bfq_handle_burst.
				 */
	/* [한국어] 이 큐가 bfqd->large_burst 기간 중에 생성되었음을 표시.
	 * 짧은 시간에 수많은 큐가 한꺼번에 생기는 상황(부팅, 대규모 빌드)에서
	 * 그 큐들을 전부 대화형으로 오인해 weight-raising하면 장치가 마비되므로,
	 * 이 표시가 붙은 큐는 weight-raising 대상에서 제외된다. */
	BFQQF_softrt_update,	/*
				 * may need softrt-next-start
				 * update
				 */
	/* [한국어] soft_rt_next_start 값을 재계산해야 할 수도 있음을 표시하는
	 * 지연 갱신 플래그. 만료 시점에 아직 in-flight request가 남아 있으면
	 * 등시성 여부를 판정할 수 없어, 모든 완료 후로 판정을 미룬다. */
	BFQQF_coop,		/* bfqq is shared */
	/* [한국어] 이 큐가 다른 프로세스와 협력(cooperation) 관계로 공유되고
	 * 있음을 표시 - 이 큐의 request는 여러 프로세스에서 온 것일 수 있다. */
	BFQQF_split_coop,	/* shared bfqq will be split */
	/* [한국어] 공유 중인 큐가 곧 분리(split)될 예정임을 표시하는 플래그.
	 * 협력 관계가 깨졌다고 판단되면 각 프로세스가 자기 큐로 돌아가야 하며,
	 * 그때 bic에 저장해 둔 saved_* 스냅샷으로 원래 상태를 복원한다. */
};
/*
 * [한국어]
 * BFQ_BFQQ_FNS(name) - enum bfqq_state_flags의 각 비트에 대해 3개의
 *   접근자 함수(mark/clear/query)를 선언하는 코드 생성 매크로.
 *
 * @name: enum bfqq_state_flags의 BFQQF_##name 값에서 접두사를 뗀 이름
 *        (예: name=busy -> BFQQF_busy 비트를 다룸).
 * @return: (매크로 자체는 반환값 없음) 생성되는 3개 함수의 반환값은 아래 참고.
 *
 * 이 매크로가 name마다 생성하는 함수는 다음과 같다:
 *   void bfq_mark_bfqq_##name(struct bfq_queue *bfqq)
 *     - bfqq->flags에 BFQQF_##name 비트를 set. @bfqq: 대상 큐(호출자가
 *       bfqd->lock을 쥔 상태여야 함). @return: 없음.
 *   void bfq_clear_bfqq_##name(struct bfq_queue *bfqq)
 *     - bfqq->flags에서 BFQQF_##name 비트를 clear. @bfqq/@return 동일.
 *   int bfq_bfqq_##name(const struct bfq_queue *bfqq)
 *     - bfqq->flags의 BFQQF_##name 비트가 set인지 조회(읽기 전용, const).
 *       @bfqq: 조회 대상. @return: 0(clear) 또는 non-zero(set).
 * 매크로 형태로 만드는 이유는 12개 플래그마다 동일한 3-함수 패턴을 손으로
 * 반복 작성하지 않기 위함이며, 실제 함수 본문은 bfq-iosched.c에 있다.
 * 실행 컨텍스트: 모두 bfqd->lock을 쥔 프로세스/소프트IRQ 컨텍스트에서
 * 호출되어야 하며, 이 매크로 자체는 재진입 문제가 없는 순수 코드 생성기다.
 * 호출 체인: 각종 bfq_*() 스케줄링 함수 -> bfq_mark_bfqq_xxx() /
 *   bfq_clear_bfqq_xxx() / bfq_bfqq_xxx() -> bfqq->flags 비트 연산.
 */
#define BFQ_BFQQ_FNS(name)						\
void bfq_mark_bfqq_##name(struct bfq_queue *bfqq);			\
void bfq_clear_bfqq_##name(struct bfq_queue *bfqq);			\
int bfq_bfqq_##name(const struct bfq_queue *bfqq);
BFQ_BFQQ_FNS(just_created); // 갓 생성돼 아직 한 번도 서비스받지 않은 큐 - burst/stable-merge 판정이 이 시점에만 의미가 있어 별도 플래그가 필요하다
BFQ_BFQQ_FNS(busy); // 대기 request가 있거나 서비스 중이라 busy_queues[] 카운트에 포함되는 상태
BFQ_BFQQ_FNS(wait_request); // idling 타이머를 걸어 두고 다음 request 도착을 기다리는 중 - 도착하면 타이머를 즉시 취소해야 하므로 상태 추적이 필요
BFQ_BFQQ_FNS(non_blocking_wait_rq); // 장치를 비워 두지 않은 채(=idling 없이) 다음 request를 기다리는 상태 - budget 재계산 시 "제때 도착했는가" 판정에 쓰인다
BFQ_BFQQ_FNS(fifo_expire); // 이번 서비스 슬라이스에서 FIFO 만료 검사를 이미 한 번 했음 - 같은 슬라이스에서 중복 검사를 막는다
BFQ_BFQQ_FNS(has_short_ttime); // think time이 짧다고 판정된 큐 - idling이 값싸게 성공하고 injection은 해로운 쪽
BFQ_BFQQ_FNS(sync); // 동기 I/O 큐(프로세스가 완료를 기다림) - async 큐와 달리 지연시간을 지켜 줄 가치가 있다
BFQ_BFQQ_FNS(IO_bound); // 오랫동안 backlog를 유지하는 I/O 바운드 큐 - idling으로 지켜 줄 만한 대상인지 판단하는 근거
BFQ_BFQQ_FNS(in_large_burst); // 대량 큐 생성 폭주(예: 부팅, 대규모 빌드)의 일원 - weight-raising 대상에서 제외해 처리량을 지킨다
BFQ_BFQQ_FNS(coop); // 다른 프로세스와 병합되어 공유되고 있는 큐
BFQ_BFQQ_FNS(split_coop); // 공유 큐를 곧 분리(split)해야 함 - 협력 관계가 깨졌다고 판단된 상태
BFQ_BFQQ_FNS(softrt_update); // soft real-time 판정을 다시 계산해야 함 - 만료 시점에 in-flight가 남아 있어 판정을 미뤄 둔 경우
#undef BFQ_BFQQ_FNS

/* Expiration reasons. */
/*
 * bfq_queue가 서비스 중 만료(expire)되는 이유. bfq_bfqq_expire()는 이
 * 사유에 따라 다음 budget을 늘릴지 줄일지, 그리고 소비하지 못한 예산을
 * 어떻게 정산할지를 다르게 결정하므로, 여기 나열된 값은 단순한 로그
 * 태그가 아니라 budget 피드백 루프의 입력이다.
 */
enum bfqq_expiration {
	BFQQE_TOO_IDLE = 0,		/*
					 * queue has been idling for
					 * too long
					 */
	/* [한국어] idle_slice_timer가 만료되도록 너무 오래 기다렸는데도 다음
	 * request가 오지 않아 만료된 경우. idling에 건 기대가 빗나가 장치를
	 * 놀린 시간이 순손실이 된 상황이며, 이 사유일 때만 budget을 줄인다. */
	BFQQE_BUDGET_TIMEOUT,	/* budget took too long to be used */
	/* [한국어] budget_timeout(jiffies)이 지나도록 예산을 다 쓰지 못해
	 * 강제로 만료된 경우. 예산이 남았다는 것은 이 큐가 느렸다는 뜻이지만,
	 * 원인이 큐의 seek 패턴인지 장치 자체의 느림(예: 디스크 바깥/안쪽
	 * 트랙 속도 차)인지 알 수 없으므로 budget을 2배로 늘려 한 번 더
	 * 기회를 준다. */
	BFQQE_BUDGET_EXHAUSTED,	/* budget consumed */
	/* [한국어] 할당된 예산을 시간 안에 전부 소비해 만료된 경우. seeky하지도
	 * think time이 길지도 않다는 것이 실측으로 증명된 셈이라, budget을 4배로
	 * 크게 늘려 처리량을 끌어올린다. */
	BFQQE_NO_MORE_REQUESTS,	/* the queue has no more requests */
	/* [한국어] 큐에 더 이상 대기 중인 request가 없어(자연스럽게 비어)
	 * 만료된 경우. 이때는 budget을 "실제로 쓴 만큼"으로 맞춰야 한다 -
	 * 쓰지도 않을 큰 예산을 계속 들고 있으면 B-WF2Q+의 finish time이
	 * 실제 소비와 어긋나 이 큐가 반복해서 부당한 이득을 본다. */
	BFQQE_PREEMPTED		/* preemption in progress */
	/* [한국어] 더 높은 우선순위(RT 등)나 더 높은 weight의 큐가 활성화되어
	 * 현재 서비스 중인 큐가 선점(preemption)당한 경우. 만료 원인이 이 큐의
	 * 행동이 아니므로, 이 사유일 때는 budget을 조정하지 않는다. */
};
/* [한국어] CPU별로 분산된 카운터(percpu_counter)로, 잦은 갱신에 따른
 * cache-line contention을 줄이면서 근사적으로 빠르게 값을 누적한다.
 * blkio 통계(장치 처리율 측정 등)처럼 정확도보다 갱신 성능이
 * 중요한 카운터에 쓰인다.
 * 설정자: blkg_stat_add() 계열이 percpu_counter_add()로 갱신.
 * 읽는 자: percpu_counter_sum()으로 전체 합을 읽는 통계 조회 경로(cgroupfs read).
 * 값 범위: 64비트 부호있는 값(내부적으로 배치 지역 카운터 + 전역 합).
 * 동기화: percpu_counter 자체의 내부 락/원자 연산으로 보호(추가 락 불필요). */
struct bfq_stat {
	struct percpu_counter		cpu_cnt;
	/* [한국어] cgroup 계층 이동(recursive 통계 재계산) 등에서 percpu_counter로
	 * 표현하기 어려운 보정값을 더할 때 쓰는 보조 원자 카운터.
	 * 설정자: blkg_stat_add() 등이 재부모화(reparenting) 시 atomic64_add().
	 * 읽는 자: 전체 값 조회 시 cpu_cnt 합과 함께 더해짐.
	 * 값 범위: 64비트 부호있는 값.
	 * 동기화: atomic64_t 자체의 원자 연산. */
	atomic64_t			aux_cnt;
};

/* basic stats */
/* [한국어] 이 bfq_group을 거쳐간 read/write(+discard 등)별 바이트 수
 * 누적 통계(blkg_rwstat은 방향별 percpu 카운터 묶음). cgroupfs의
 * io.stat 등에서 조회되는 NVMe 대역폭 사용량 추적 데이터다.
 * 설정자: bfqg_stats_update_legacy_io() 등이 request 완료 시 갱신.
 * 읽는 자: cgroup 통계 조회 인터페이스(blkcg_print_blkgs 등).
 * 값 범위: blkg_rwstat 내부의 방향별 카운터, 0 이상.
 * 동기화: blkg_rwstat 자체의 percpu/원자 연산으로 보호. */
struct bfqg_stats {
	struct blkg_rwstat		bytes;
	/* [한국어] 방향별 완료된 IO 개수 누적 통계(IOPS 산출의 원천 데이터).
	 * 설정자: bytes와 동일한 갱신 경로.
	 * 읽는 자: cgroup 통계 조회 인터페이스.
	 * 값 범위: 0 이상.
	 * 동기화: blkg_rwstat 내부 보호. */
	struct blkg_rwstat		ios;
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* number of ios merged */
	/* [한국어] bio-merge로 다른 request와 합쳐진 IO 개수(디버그 전용
	 * 통계). merge가 많을수록 실제 장치에 나가는 커맨드 수가 줄어드는
	 * 효과를 보여준다.
	 * 설정자: bfqg_stats_update_io_merged().
	 * 읽는 자: CONFIG_BFQ_CGROUP_DEBUG 빌드의 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상.
	 * 동기화: blkg_rwstat 내부 보호. */
	struct blkg_rwstat		merged;
	/* total time spent on device in ns, may not be accurate w/ queueing */
	/* [한국어] 디바이스 상에서 실제로 서비스되는 데 걸린 총 시간(ns).
	 * 원본 주석처럼 큐잉(NVMe처럼 여러 요청이 동시에 in-flight인 경우)
	 * 환경에서는 "서비스 시간"의 경계가 모호해 정확하지 않을 수 있다.
	 * 설정자: bfqg_stats_update_completion().
	 * 읽는 자: 디버그 cgroupfs 파일(NVMe CQ latency 근사치).
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: blkg_rwstat 내부 보호. */
	struct blkg_rwstat		service_time;
	/* total time spent waiting in scheduler queue in ns */
	/* [한국어] BFQ 스케줄러 큐 안에서 대기한 총 시간(ns, 장치에
	 * 나가기 전 스케줄러 대기 latency).
	 * 설정자: bfqg_stats_update_completion().
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: blkg_rwstat 내부 보호. */
	struct blkg_rwstat		wait_time;
	/* number of IOs queued up */
	/* [한국어] 이 그룹에 큐잉되어 있던 IO 개수의 누적/스냅샷 통계(장치
	 * backlog 크기의 cgroup별 분해).
	 * 설정자: bfqg_stats_update_io_add()(디버그 빌드에서만 정의).
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상.
	 * 동기화: blkg_rwstat 내부 보호. */
	struct blkg_rwstat		queued;
	/* total disk time and nr sectors dispatched by this group */
	/* [한국어] 이 그룹이 디스크를 사용한 총 시간 및 디스패치한 섹터 수
	 * (bfq_stat = percpu_counter + aux_cnt 조합). 장치 서비스 시간의
	 * cgroup별 누적치다.
	 * 설정자: 완료/디스패치 경로의 통계 갱신 함수.
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상.
	 * 동기화: struct bfq_stat 내부 보호(percpu/atomic). */
	struct bfq_stat		time;
	/* sum of number of ios queued across all samples */
	/* [한국어] 표본을 채취할 때마다의 큐 크기를 합산한 값. 아래
	 * avg_queue_size_samples로 나누면 평균 큐 크기(장치 평균 깊이)가
	 * 된다.
	 * 설정자: bfqg_stats_update_avg_queue_size().
	 * 읽는 자: 평균 계산 시 분자로 사용.
	 * 값 범위: 0 이상.
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		avg_queue_size_sum;
	/* count of samples taken for average */
	/* [한국어] avg_queue_size_sum을 누적하며 채취한 표본 개수(평균 계산의
	 * 분모).
	 * 설정자: bfqg_stats_update_avg_queue_size().
	 * 읽는 자: 평균 계산 시 분모로 사용.
	 * 값 범위: 0 이상.
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		avg_queue_size_samples;
	/* how many times this group has been removed from service tree */
	/* [한국어] 이 그룹의 entity가 B-WF2Q+ 서비스 트리에서 제거된
	 * 횟수(장치 스케줄링 이벤트 빈도 지표).
	 * 설정자: bfqg_stats_update_dequeue().
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상.
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		dequeue;
	/* total time spent waiting for it to be assigned a timeslice. */
	/* [한국어] 이 그룹이 타임슬라이스(서비스 기회)를 배정받기까지 기다린
	 * 총 시간(장치 cgroup 레벨 latency).
	 * 설정자: bfqg_stats_set_start_idle_time()/완료 경로가 start_group_wait_time과
	 *   짝을 이뤄 누적.
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		group_wait_time;
	/* time spent idling for this blkcg_gq */
	/* [한국어] 이 blkcg_gq(그룹)를 위해 device idling한 누적 시간.
	 * "이 그룹의 몫을 지켜 주느라 장치를 놀린 시간"이 얼마인지를
	 * cgroup 단위로 분해해 보여 준다.
	 * 설정자: bfqg_stats_update_idle_time()(디버그 빌드).
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		idle_time;
	/* total time with empty current active q with other requests queued */
	/* [한국어] 현재 active 큐는 비어있지만 다른 request가 대기 중이던
	 * 총 시간(장치 활용도 저하 구간의 지표).
	 * 설정자: bfqg_stats_set_start_empty_time() 관련 경로(디버그 빌드).
	 * 읽는 자: 디버그 cgroupfs 파일.
	 * 값 범위: 0 이상(ns 단위).
	 * 동기화: struct bfq_stat 내부 보호. */
	struct bfq_stat		empty_time;
	/* fields after this shouldn't be cleared on stat reset */
	u64				start_group_wait_time;
	/* [한국어] group_wait_time 누적을 위한 시작 시각 마커. 통계 리셋 시
	 * 지워지면 안 되는(원본 주석 참고) "진행 중인 측정의 시작점"이다.
	 * 설정자: 그룹이 대기를 시작할 때 기록.
	 * 읽는 자: 대기 종료 시 group_wait_time += now - start_group_wait_time.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock (통계 리셋 로직과 순서 주의). */
	u64				start_idle_time;
	/* [한국어] idle_time 누적을 위한 시작 시각 마커.
	 * 설정자: idling 시작 시 기록.
	 * 읽는 자: idling 종료 시 idle_time 누적 계산.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	u64				start_empty_time;
	/* [한국어] empty_time 누적을 위한 시작 시각 마커.
	 * 설정자: empty 상태 시작 시 기록.
	 * 읽는 자: empty 상태 종료 시 empty_time 누적 계산.
	 * 값 범위: ktime 값.
	 * 동기화: bfqd->lock. */
	uint16_t			flags;
	/* [한국어] start_group_wait_time/start_idle_time/start_empty_time
	 * 각각이 "현재 측정 진행 중"인지를 나타내는 비트 플래그 모음(중복
	 * 시작 방지용).
	 * 설정자: 각 bfqg_stats_set_start_*_time()이 set, 대응 종료 처리가 clear.
	 * 읽는 자: 각 시작 함수가 이미 진행 중인지 확인.
	 * 값 범위: 비트 플래그 조합.
	 * 동기화: bfqd->lock. */
#endif /* CONFIG_BFQ_CGROUP_DEBUG */
};

#ifdef CONFIG_BFQ_GROUP_IOSCHED

/*
 * CONFIG_BFQ_GROUP_IOSCHED: cgroup별 I/O 격리를 활성화한다. NVMe
 * 관점에서는 서로 다른 cgroup의 SQ 사용량을 계층적 B-WF2Q+로 제어하여
 * tenant 간 NVMe 대역폭을 공정하게 분배할 수 있다.
 */

/*
 * struct bfq_group_data - per-blkcg storage for the blkio subsystem.
 *
 * @ps: @blkcg_policy_storage that this structure inherits
 * @weight: weight of the bfq_group
 */
struct bfq_group_data {
	/* must be the first member */
	/* [한국어] blkcg_policy_storage를 상속하는 공통 헤더. blkcg_to_bfqgd()
	 * 등이 container_of()로 이 구조체를 얻으려면 반드시 첫 번째 멤버여야
	 * 한다. blkcg(하나의 cgroup)에 대해 BFQ 정책이 저장하는 per-cgroup
	 * 설정값의 앵커다.
	 * 설정자: blkcg_policy_bfq.cpd_alloc_fn(bfq_cpd_alloc)이 cgroup 생성 시 초기화.
	 * 읽는 자: blkcg_to_bfqgd()가 blkcg로부터 이 구조체를 역산할 때.
	 * 값 범위: 유효한 struct blkcg_policy_data.
	 * 동기화: cgroup 코어의 락 규칙을 따름(cgroup_mutex/rcu). */
	struct blkcg_policy_data pd;

	/* [한국어] 이 cgroup에 설정된 io.weight(또는 legacy blkio.weight) 값.
	 * 이 cgroup 아래에서 새로 생성되는 bfq_group의 entity.weight 기본값이
	 * 되어, 장치 자원 분배 비율을 cgroup 단위로 결정한다.
	 * 설정자: cgroupfs write 핸들러(bfq_io_set_weight_legacy 등)가 사용자
	 *   입력으로 갱신.
	 * 읽는 자: 새 bfq_group 생성 시 entity.weight 초기값으로 복사.
	 * 값 범위: [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT].
	 * 동기화: cgroup 코어 락 + bfqd->lock(그룹 생성 시). */
	unsigned int weight;
};
/**
 * struct bfq_group - per (device, cgroup) data structure.
 * @entity: schedulable entity to insert into the parent group sched_data.
 * @sched_data: own sched_data, to contain child entities (they may be
 *              both bfq_queues and bfq_groups).
 * @bfqd: the bfq_data for the device this group acts upon.
 * @async_bfqq: array of async queues for all the tasks belonging to
 *              the group, one queue per ioprio value per ioprio_class,
 *              except for the idle class that has only one queue.
 * @async_idle_bfqq: async queue for the idle class (ioprio is ignored).
 * @my_entity: pointer to @entity, %NULL for the toplevel group; used
 *             to avoid too many special cases during group creation/
 *             migration.
 * @stats: stats for this bfqg.
 * @active_entities: number of active entities belonging to the group;
 *                   unused for the root group. Used to know whether there
 *                   are groups with more than one active @bfq_entity
 *                   (see the comments to the function
 *                   bfq_better_to_idle()).
 * @rq_pos_tree: rbtree sorted by next_request position, used when
 *               determining if two or more queues have interleaving
 *               requests (see bfq_find_close_cooperator()).
 *
 * Each (device, cgroup) pair has its own bfq_group, i.e., for each cgroup
 * there is a set of bfq_groups, each one collecting the lower-level
 * entities belonging to the group that are acting on the same device.
 *
 * Locking works as follows:
 *    o @bfqd is protected by the queue lock, RCU is used to access it
 *      from the readers.
 *    o All the other fields are protected by the @bfqd queue lock.
 *
 * NVMe 관점: (NVMe 장치, cgroup) 쌍별 스케줄링 그룹이다. entity는
 * 상위 그룹의 B-WF2Q+ 트리에 삽입되어 장치 자원을 cgroup 계층에
 * 따라 분배받는다. rq_pos_tree는 인접한 LBA를 가진 bfq_queue를 찾아
 * 디스패치 시 순차적으로 배치할 수 있도록 merge/cooperation
 * candidate를 제공한다.
 */
struct bfq_group {
	/* must be the first member */
	/* [한국어] blkg_policy_storage를 상속하는 공통 헤더(bfq_group_data.pd와
	 * 짝을 이루나, 이쪽은 blkg=blkcg_gq, 즉 (디바이스,cgroup) 쌍 단위).
	 * container_of()로 이 구조체를 얻으려면 첫 번째 멤버여야 한다.
	 * 설정자: blkcg_policy_bfq.pd_alloc_fn이 그룹 생성 시 초기화.
	 * 읽는 자: bfqg_to_blkg()/blkg_to_bfqg()가 상호 변환 시 사용.
	 * 값 범위: 유효한 struct blkg_policy_data.
	 * 동기화: bfqd->lock + cgroup 코어 규칙. */
	struct blkg_policy_data pd;
	/* reference counter (see comments in bfq_bic_update_cgroup) */
	/* [한국어] 이 bfq_group의 참조 카운트. bic가 cgroup 이전 도중에도
	 * 이전 그룹을 잠시 붙잡고 있을 수 있어(bfq_bic_update_cgroup() 참고)
	 * refcount_t로 관리한다.
	 * 설정자: bfq_group 참조를 얻는 모든 경로가 증가, bfqg_and_blkg_put()이 감소.
	 * 읽는 자: 0이 되면 실제 해제(bfq_pd_free 등).
	 * 값 범위: 1 이상(살아있는 동안).
	 * 동기화: refcount_t 자체의 원자 연산 + bfqd->lock. */
	refcount_t ref;
	/* [한국어] 이 그룹을 상위 그룹의 B-WF2Q+ 트리에 스케줄링 단위로
	 * 등록하기 위한 entity. entity.my_sched_data가 아래 sched_data를
	 * 가리켜, "그룹도 entity로서 스케줄링되고, 동시에 자신만의 스케줄러를
	 * 갖는다"는 계층 구조를 완성한다.
	 * 설정자: bfq_init_entity()가 그룹 생성 시 초기화.
	 * 읽는 자: 상위 레벨 B-WF2Q+ 트리 순회 로직.
	 * 값 범위: struct bfq_entity 참고.
	 * 동기화: bfqd->lock. */
	struct bfq_entity entity;
	/* [한국어] 이 그룹 아래에 속한 자식 entity(하위 bfq_queue 또는 하위
	 * bfq_group)들을 스케줄링하는 자신만의 B-WF2Q+ 스케줄러. 장치
	 * 자원이 이 그룹에 배분된 몫 안에서, 다시 자식들에게 어떻게 나뉘는지를
	 * 결정한다.
	 * 설정자: bfq_init_entity()가 그룹 생성 시 초기화.
	 * 읽는 자: 이 그룹의 자식을 다루는 모든 bfq-wf2q.c 함수.
	 * 값 범위: struct bfq_sched_data 참고.
	 * 동기화: bfqd->lock. */
	struct bfq_sched_data sched_data;

	/* [한국어] 이 그룹이 속한 디바이스의 전역 bfq_data. 원본 주석대로
	 * RCU를 통해 읽기 측에서 접근하고, 갱신은 큐 락으로 보호된다.
	 * 설정자: bfq_create_group_hierarchy()가 그룹 생성 시 설정.
	 * 읽는 자: 이 그룹과 관련된 거의 모든 함수가 bfqd->lock 등을 얻기 위해 참조.
	 * 값 범위: 유효 포인터(그룹이 유효한 동안 NULL 불가).
	 * 동기화: bfqd 필드 자체는 queue lock(생성/파괴)+RCU(읽기)로 보호. */
	struct bfq_data *bfqd;
	/* [한국어] 이 그룹에 속한 모든 태스크가 공유하는 async bfq_queue
	 * 행렬. 첫 인덱스는 sync/async(여기서는 항상 async만 채워짐), 두
	 * 번째는 ioprio 레벨(IDLE 클래스는 레벨 무시하고 하나만 사용), 세
	 * 번째는 actuator. async I/O(주로 writeback)는 프로세스별로 큐를
	 * 두지 않고 그룹 단위로 공유해 장치에 나가는 커맨드 수를 줄인다.
	 * 설정자: bfq_get_queue()가 async 큐를 찾다가 없으면 생성해 연결.
	 * 읽는 자: bic_to_bfqq()가 async 경로일 때 그룹의 이 행렬을 조회.
	 * 값 범위: 유효 포인터 또는 아직 생성 전이면 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_queue *async_bfqq[2][IOPRIO_NR_LEVELS][BFQ_MAX_ACTUATORS];
	/* [한국어] IOPRIO_CLASS_IDLE 클래스의 async I/O를 위한 전용 큐(actuator별).
	 * IDLE 클래스는 ioprio 레벨을 구분하지 않으므로 async_bfqq와 별도로
	 * actuator 인덱스만으로 관리한다.
	 * 설정자: bfq_get_queue()가 IDLE async 경로일 때 생성/연결.
	 * 읽는 자: bic_to_bfqq()의 IDLE async 경로.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: bfqd->lock. */
	struct bfq_queue *async_idle_bfqq[BFQ_MAX_ACTUATORS];

	/* [한국어] 위 entity 필드 자신을 가리키는 포인터(자기 참조). 최상위
	 * root_group에서는 NULL이 되어, "내가 최상위인지"를 판별하고 그룹
	 * 생성/이전 시 여러 특수 케이스를 피하기 위한 편의 필드다(원본 주석
	 * 참고).
	 * 설정자: bfq_init_entity()가 그룹 초기화 시 &entity 또는 NULL로 설정.
	 * 읽는 자: for_each_entity() 등이 entity->parent를 계속 따라갈지 판단.
	 * 값 범위: &this->entity 또는 NULL(root_group).
	 * 동기화: 생성 후 불변. */
	struct bfq_entity *my_entity;
	/* [한국어] 이 그룹에 속한 entity 중 현재 active(backlogged)한 것의
	 * 개수(root_group에서는 미사용). 그룹 안에 active entity가 2개 이상
	 * 있는지를 판단해 bfq_better_to_idle()의 비대칭 시나리오 판정에
	 * 쓰인다.
	 * 설정자: 자식 entity가 활성화/비활성화될 때 증감.
	 * 읽는 자: bfq_better_to_idle() 계열 판정 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int active_entities;
	/* [한국어] 이 그룹 안에서 pending request(완료 대기 중인 request,
	 * 디스패치되었지만 미완료인 것 포함)를 가진 bfq_queue의 개수. NVMe
	 * in-flight 요청이 있는 큐 수를 그룹 단위로 집계해
	 * bfqd->num_groups_with_pending_reqs 계산의 입력이 된다.
	 * 설정자: bfq_add_bfqq_in_groups_with_pending_reqs()/bfq_del_...()가 증감.
	 * 읽는 자: 그룹이 pending 상태에서 벗어났는지 판단하는 로직.
	 * 값 범위: 0 이상.
	 * 동기화: bfqd->lock. */
	int num_queues_with_pending_reqs;

	/* [한국어] 이 그룹에 속한 bfq_queue들을 next_rq의 LBA 위치 순으로
	 * 정렬한 rb-tree. 서로 다른 프로세스라도 인접한 LBA를 접근하면(즉
	 * interleaving하면) 협력 관계로 판단해 merge하는
	 * bfq_find_close_cooperator()의 탐색 대상이며, 장치에서 여러 작은
	 * 요청을 하나의 순차 스트림으로 합치는 효과를 낸다.
	 * 설정자: bfq_pos_tree_add_move()가 각 큐의 pos_node를 삽입/이동.
	 * 읽는 자: bfq_find_close_cooperator().
	 * 값 범위: bfq_queue.pos_node로 연결된 집합, 비어있으면 RB_ROOT.
	 * 동기화: bfqd->lock. */
	struct rb_root rq_pos_tree;

	/* [한국어] 이 그룹의 cgroup 통계 모음(위 struct bfqg_stats 참고).
	 * cgroupfs를 통해 사용자에게 노출되는 장치 cgroup 모니터링 데이터의
	 * 원천이다.
	 * 설정자: bfqg_stats_update_*() 계열 함수 전체.
	 * 읽는 자: cgroup 통계 조회 인터페이스.
	 * 값 범위: struct bfqg_stats 참고.
	 * 동기화: bfqd->lock (일부 필드는 blkg_rwstat/percpu_counter 자체 보호). */
	struct bfqg_stats stats;
};

#else
/* [한국어] CONFIG_BFQ_GROUP_IOSCHED가 꺼진 빌드에서 쓰이는 축소판
 * bfq_group. cgroup 지원이 없어도 "그룹" 개념 자체는 필요하므로(모든
 * 큐가 이 유일한 root_group에 속함), entity/sched_data 등 핵심
 * 필드만 남긴다. 필드 의미는 위 CONFIG_BFQ_GROUP_IOSCHED 분기의
 * 동명 필드와 동일하다.
 * 설정자/읽는 자/동기화: 위 bfq_group.entity 설명과 동일. */
struct bfq_group {
	struct bfq_entity entity;
	/* [한국어] 위 CONFIG_BFQ_GROUP_IOSCHED 분기의 sched_data와 동일한
	 * 역할(자식 entity들을 위한 B-WF2Q+ 스케줄러). cgroup이 없으므로
	 * 이 그룹 하나가 디바이스 전체의 유일한 스케줄링 레벨이 된다. */
	struct bfq_sched_data sched_data;
	/* [한국어] 위 분기의 async_bfqq와 동일한 역할(공유 async 큐 행렬). */
	struct bfq_queue *async_bfqq[2][IOPRIO_NR_LEVELS][BFQ_MAX_ACTUATORS];
	/* [한국어] 위 분기의 async_idle_bfqq와 동일한 역할(IDLE 클래스 async 큐). */
	struct bfq_queue *async_idle_bfqq[BFQ_MAX_ACTUATORS];
	/* [한국어] 위 분기의 rq_pos_tree와 동일한 역할(LBA 위치 기반 cooperator 탐색). */
	struct rb_root rq_pos_tree;
};
#endif

/* --------------- main algorithm interface ----------------- */
/*
 * [한국어] bfq_service_tree를 "비어 있는 초기 상태"로 만드는 초기화
 * 리터럴. active/idle을 빈 rb_root로, first_idle/last_idle을 NULL로,
 * vtime/wsum을 0으로 설정한다. 구조체 대입 형태(compound literal)라서
 * 배열 초기화 등에서 `= BFQ_SERVICE_TREE_INIT`처럼 값으로 바로 쓸 수 있다.
 * 장치 후보 트리가 아무 큐도 갖지 않은 최초 상태를 표현한다.
 */
#define BFQ_SERVICE_TREE_INIT	((struct bfq_service_tree)		\
				{ RB_ROOT, RB_ROOT, NULL, NULL, 0, 0 })
/*
 * [한국어] bfq_timeout - bfq_queue가 예산을 다 쓰지 못했을 때 강제
 * 만료되기까지의 기본 타임아웃(jiffies) 상수. bfq-iosched.c에 정의되고
 * 여기서는 extern 선언만 제공해 bfq-cgroup.c 등 다른 파일도 동일한
 * 기본값을 참조할 수 있게 한다. 장치 latency 보장의 기본 시간
 * 파라미터다. */
extern const int bfq_timeout;
/*
 * [한국어]
 * bic_to_bfqq() - 태스크의 bfq_io_cq에서 특정 actuator/sync-async 조합에
 *   대응하는 bfq_queue를 조회.
 *
 * @bic: 조회 대상 태스크의 bfq_io_cq(bic_to_bfqd() 등으로 이미 확보된 컨텍스트).
 * @is_sync: true면 동기(sync) 큐, false면 비동기(async) 큐를 조회.
 * @actuator_idx: 조회할 actuator 인덱스(bic->bfqq[][] 행렬의 열).
 * @return: 유효한 bfq_queue 포인터 또는 아직 생성되지 않았으면 NULL.
 *
 * bio가 도착했을 때 어느 actuator/SQ 경로로 볼 것인지 결정하는 첫
 * 단계이다. bfqd->lock을 쥔 프로세스 컨텍스트에서 호출되어야 하며,
 * 재진입에 안전하지 않다(bic->bfqq[][] 갱신과 경쟁하면 안 됨).
 * 호출 체인: blk_mq_submit_bio -> blk_mq_sched_bio_merge/bfq_init_rq ->
 *   bic_to_bfqq() -> (필요 시) bfq_get_queue()가 새 큐 생성 (추정).
 */
struct bfq_queue *bic_to_bfqq(struct bfq_io_cq *bic, bool is_sync,
				unsigned int actuator_idx);
/*
 * [한국어]
 * bic_set_bfqq() - bic->bfqq[][] 행렬에 실제로 사용할 bfq_queue를 연결.
 *
 * @bic: 갱신 대상 bfq_io_cq.
 * @bfqq: 연결할 bfq_queue(merge/split 시 new_bfqq 또는 원래 큐).
 * @is_sync: sync/async 행 선택.
 * @actuator_idx: actuator 열 선택.
 * @return: 없음(void).
 *
 * 큐 생성, merge, split이 일어날 때마다 태스크가 다음에 쓸 큐를 이
 * 함수로 갱신한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_get_queue()/bfq_merge_bfqqs()/bfq_split_bfqq() -> [이 함수].
 */
void bic_set_bfqq(struct bfq_io_cq *bic, struct bfq_queue *bfqq, bool is_sync,
				unsigned int actuator_idx);
/*
 * [한국어]
 * bic_to_bfqd() - bfq_io_cq로부터 그 디바이스의 bfq_data를 얻는다.
 *
 * @bic: 조회 대상 bfq_io_cq.
 * @return: 유효한 bfq_data 포인터(해당 icq가 아직 이 디바이스에 연결되어
 *   있는 경우) 또는 연결이 끊어졌으면 구현에 따라 NULL/오류 처리.
 *
 * icq->q(request_queue)에서 elevator private 데이터를 꺼내는 헬퍼로,
 * 이후 bfqd->lock을 잡기 위한 첫 단계로 쓰인다. bio-merge 등 스케줄러
 * 락을 아직 잡기 전 컨텍스트에서도 호출될 수 있다.
 * 호출 체인: bfq_bio_merge()/bfq_limit_depth() 등 -> bic_to_bfqd().
 */
struct bfq_data *bic_to_bfqd(struct bfq_io_cq *bic);
/*
 * [한국어]
 * bfq_pos_tree_add_move() - bfq_queue를 소속 그룹의 rq_pos_tree(LBA
 *   위치 기준 rb-tree)에 삽입하거나 위치가 바뀌었으면 재삽입.
 *
 * @bfqd: 디바이스 전역 상태(락/파라미터 접근용).
 * @bfqq: next_rq 위치가 갱신된 큐.
 * @return: 없음(void).
 *
 * next_rq가 바뀔 때마다 호출되어 트리 내 정렬을 최신 상태로 유지한다.
 * 이 트리는 이후 bfq_find_close_cooperator()가 인접 LBA를 가진 큐를
 * 찾아 병합 후보를 판단하는 데 사용된다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_insert_request()/bfq_remove_request() -> [이 함수].
 */
void bfq_pos_tree_add_move(struct bfq_data *bfqd, struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_weights_tree_add() - bfqd->queue_weights_tree에 이 큐의 weight에
 *   해당하는 카운터를 추가(없으면 새로 생성, 있으면 num_active 증가).
 *
 * @bfqq: 활성화되는 bfq_queue.
 * @return: 없음(void).
 *
 * 큐가 backlogged 상태로 전이할 때 호출되어, 대칭 시나리오 판정
 * (bfq_limit_depth 등)의 입력이 되는 weight 트리를 갱신한다.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_activate_bfqq() -> [이 함수].
 */
void bfq_weights_tree_add(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_weights_tree_remove() - 큐가 비활성화될 때 해당 weight 카운터의
 *   num_active를 감소시키고, 0이 되면 트리에서 노드 자체를 제거.
 *
 * @bfqq: 비활성화되는 bfq_queue.
 * @return: 없음(void).
 *
 * bfq_weights_tree_add()의 역연산. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_deactivate_bfqq() -> [이 함수].
 */
void bfq_weights_tree_remove(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_bfqq_expire() - 현재 서비스 중인 bfq_queue를 만료시켜 서비스
 *   트리에서 내리거나 idle 트리로 옮긴다.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 만료 대상 큐(보통 bfqd->in_service_queue).
 * @compensate: true면 idling 등으로 소비하지 못한 시간을 보상해 준다.
 * @reason: enum bfqq_expiration 중 하나(왜 만료되는지, 로깅/통계에도 사용).
 * @return: 없음(void).
 *
 * NVMe CQ 완료 처리 또는 budget_timeout 만료 시 호출되며, 이후
 * bfq_get_next_queue()가 다음 SQ 후보를 고르게 하는 트리거다.
 * bfqd->lock 하에서 호출(일부 경로는 hrtimer 콜백에서 진입).
 * 호출 체인: nvme_irq -> nvme_complete_rq -> blk_mq_complete_request ->
 *   bfq_completed_request -> bfq_bfqq_expire() (추정) -> bfq_get_next_queue().
 */
void bfq_bfqq_expire(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		     bool compensate, enum bfqq_expiration reason);
/*
 * [한국어]
 * bfq_put_queue() - bfq_queue의 참조 카운트를 감소시키고 0이 되면 해제.
 *
 * @bfqq: 참조를 반환할 큐.
 * @return: 없음(void).
 *
 * 이 큐를 가리키던 마지막 포인터가 정리될 때 호출된다(bic 해제, merge
 * 등). ref가 0이 되면 entity를 트리에서 제거하고 메모리를 반환하는
 * 정리 경로로 이어진다. bfqd->lock 하에서 호출.
 * 호출 체인: bic 해제/merge/split 경로 -> [이 함수].
 */
void bfq_put_queue(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_put_cooperator() - 협력(cooperation) 관계로 연결된 큐(new_bfqq
 *   체인)에 대한 참조를 반환.
 *
 * @bfqq: 대상 큐(new_bfqq 체인을 따라가며 각각의 ref를 감소).
 * @return: 없음(void).
 *
 * merge 해제/큐 소멸 시 협력 체인 전체의 참조를 정리해 장치 병합
 * 상태를 되돌린다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_put_queue() 계열 -> [이 함수].
 */
void bfq_put_cooperator(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_end_wr_async_queues() - 한 그룹에 속한 모든 async 큐(async_bfqq[][],
 *   async_idle_bfqq[])의 weight-raising을 종료시킨다.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * 그룹 단위로 async I/O의 WR을 일괄 종료해 async 경로의
 * 우선순위를 정상으로 복귀시킨다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_end_wr_async() -> [이 함수] (그룹마다 반복).
 */
void bfq_end_wr_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg);
/*
 * [한국어]
 * bfq_release_process_ref() - 태스크(io_context)가 이 큐와 맺은 연결을
 *   해제(bic->bfqq 포인터 정리 등을 포함해 프로세스 쪽 참조를 반환).
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 대상 큐.
 * @return: 없음(void).
 *
 * 태스크 종료, cgroup 이전 등으로 더 이상 이 큐를 쓰지 않게 될 때
 * 호출되어 장치 경로에서 태스크를 분리(detach)한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_exit_icq()/bfq_bic_update_cgroup() -> [이 함수].
 */
void bfq_release_process_ref(struct bfq_data *bfqd, struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_schedule_dispatch() - blk-mq 코어에 "디스패치할 것이 있을 수
 *   있으니 다시 실행해 달라"고 요청.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: 없음(void).
 *
 * BFQ가 지금 당장은 디스패치를 안 하기로 했지만(idling 등) 나중에
 * 다시 시도해야 할 때, blk_mq_run_hw_queue() 재실행을 예약해 장치로
 * request를 복귀시킬 타이밍을 조정한다. bfqd->lock 하에서 호출 가능.
 * 호출 체인: 여러 상태 전이 함수 -> [이 함수] -> blk_mq_run_hw_queue (추정).
 */
void bfq_schedule_dispatch(struct bfq_data *bfqd);
/*
 * [한국어]
 * bfq_put_async_queues() - 한 그룹에 속한 모든 async 큐의 참조를 반환.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqg: 대상 그룹(소멸 중이거나 재구성 중).
 * @return: 없음(void).
 *
 * 그룹이 소멸하거나 cgroup 계층이 재구성될 때, 그 그룹이 소유하던
 * 공유 async 큐들의 장치 자원을 회수한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_pd_offline()/그룹 소멸 경로 -> [이 함수].
 */
void bfq_put_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg);
/* ------------ end of main algorithm interface -------------- */

/* ---------------- cgroups-support interface ---------------- */
/*
 * [한국어]
 * bfqg_stats_update_legacy_io() - legacy(v1) blkio cgroup 인터페이스를
 *   위한 IO 통계를 request 완료 시 갱신.
 *
 * @q: 이 request가 속한 request_queue.
 * @rq: 완료(또는 처리)된 request.
 * @return: 없음(void).
 *
 * cgroup v1 blkio 컨트롤러 호환을 위해 bytes/ios 등을 갱신한다.
 * bfqd->lock 하에서 완료 처리 경로 중 호출.
 * 호출 체인: bfq_finish_requeue_request()/완료 경로 -> [이 함수].
 */
void bfqg_stats_update_legacy_io(struct request_queue *q, struct request *rq);
/*
 * [한국어]
 * bfqg_stats_update_io_remove() - request가 스케줄러에서 제거(merge되어
 *   사라지거나 취소)될 때 통계를 갱신.
 *
 * @bfqg: 해당 request가 속했던 그룹.
 * @opf: request의 op flags(방향/타입 판별용).
 * @return: 없음(void).
 *
 * NVMe abort/requeue와 유사하게, 실제로 디바이스까지 가지 않고 제거된
 * request를 통계에 반영한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_remove_request() -> [이 함수].
 */
void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf);
/*
 * [한국어]
 * bfqg_stats_update_io_merged() - 다른 request와 bio-merge가 일어났을
 *   때 merge 카운터를 갱신.
 *
 * @bfqg: 대상 그룹.
 * @opf: op flags.
 * @return: 없음(void).
 *
 * merge가 많을수록 실제 장치에 나가는 커맨드 수가 줄어드는 효과를
 * 통계로 남긴다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bio_merge()/elevator merge 콜백 -> [이 함수].
 */
void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf);
/*
 * [한국어]
 * bfqg_stats_update_completion() - request 완료 시 서비스 시간/대기
 *   시간 통계를 갱신.
 *
 * @bfqg: 대상 그룹.
 * @start_time_ns: request가 디스패치(서비스 시작)된 시각.
 * @io_start_time_ns: request가 스케줄러 큐에 들어온 시각.
 * @opf: op flags(방향별 blkg_rwstat 인덱싱용).
 * @return: 없음(void).
 *
 * NVMe CQ 완료 처리 시 호출되어 service_time/wait_time(디버그 빌드)
 * 등을 갱신한다. bfqd->lock 하에서 호출(완료 콜백 컨텍스트 포함).
 * 호출 체인: bfq_completed_request() -> [이 함수].
 */
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf);
/*
 * [한국어]
 * bfqg_stats_update_dequeue() - 그룹의 entity가 서비스 트리에서 제거될
 *   때 dequeue 카운터를 증가.
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * 장치 스케줄링 이벤트(그룹이 트리에서 빠지는 빈도)를 통계로
 * 남긴다. bfqd->lock 하에서 호출.
 * 호출 체인: __bfq_deactivate_entity()(그룹 entity 대상) -> [이 함수].
 */
void bfqg_stats_update_dequeue(struct bfq_group *bfqg);
/*
 * [한국어]
 * bfqg_stats_set_start_idle_time() - 그룹에 대한 idle_time 측정을
 *   시작(시작 시각 마커 기록).
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * idling이 시작될 때 호출되어 bfqg_stats.start_idle_time을 세팅한다.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_arm_slice_timer() -> [이 함수].
 */
void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg);
/*
 * [한국어]
 * bfq_bfqq_move() - bfq_queue를 다른 cgroup(bfq_group)으로 재배치.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 이동할 큐.
 * @bfqg: 목적지 그룹.
 * @return: 없음(void).
 *
 * 태스크가 cgroup을 이전(migration)했을 때, 진행 중인 큐를 새 그룹의
 * B-WF2Q+ 트리로 옮겨 장치 자원 분배를 새 cgroup 기준으로
 * 재분류한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bic_update_cgroup() -> [이 함수].
 */
void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		   struct bfq_group *bfqg);

#ifdef CONFIG_BFQ_CGROUP_DEBUG
/*
 * [한국어]
 * bfqg_stats_update_io_add() - (디버그 빌드) request가 큐잉될 때 queued
 *   통계를 갱신.
 *
 * @bfqg: 대상 그룹.
 * @bfqq: request가 들어간 큐(로깅용).
 * @opf: op flags.
 * @return: 없음(void).
 *
 * 장치 backlog 크기를 cgroup 단위로 기록하는 디버그 전용 경로.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_insert_request() -> [이 함수] (CONFIG_BFQ_CGROUP_DEBUG일 때만).
 */
void bfqg_stats_update_io_add(struct bfq_group *bfqg, struct bfq_queue *bfqq,
			      blk_opf_t opf);
/*
 * [한국어]
 * bfqg_stats_set_start_empty_time() - (디버그 빌드) 그룹이 "active 큐는
 *   비었지만 다른 request가 대기 중"인 empty 상태 측정을 시작.
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * 장치 활용도 저하 구간을 통계로 남기기 위한 시작 마커 기록.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_del_bfqq_busy() 등 -> [이 함수].
 */
void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg);
/*
 * [한국어]
 * bfqg_stats_update_idle_time() - (디버그 빌드) idling이 끝날 때
 *   idle_time 누적을 마무리.
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * start_idle_time 이후 경과 시간을 idle_time에 더한다. bfqd->lock
 * 하에서 호출.
 * 호출 체인: idling 종료 처리 경로 -> [이 함수].
 */
void bfqg_stats_update_idle_time(struct bfq_group *bfqg);
/*
 * [한국어]
 * bfqg_stats_update_avg_queue_size() - (디버그 빌드) 평균 큐 크기 통계
 *   표본을 하나 추가.
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * 장치 평균 깊이 통계(avg_queue_size_sum/samples)를 갱신한다.
 * bfqd->lock 하에서 호출.
 * 호출 체인: 디스패치/완료 경로의 주기적 샘플링 지점 -> [이 함수].
 */
void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg);
#endif
/*
 * [한국어]
 * bfq_init_entity() - bfq_entity를 초기값(weight, ioprio, 부모 관계
 *   등)으로 초기화.
 *
 * @entity: 초기화할 entity(bfq_queue.entity 또는 bfq_group.entity).
 * @bfqg: 이 entity가 속할 그룹(부모 결정에 사용).
 * @return: 없음(void).
 *
 * 새 bfq_queue/bfq_group이 생성될 때 호출되어, 이 entity를 장치
 * 우선순위 트리(B-WF2Q+)에 참여시킬 준비를 한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_get_queue()/bfq_create_group_hierarchy() -> [이 함수].
 */
void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg);
/*
 * [한국어]
 * bfq_bic_update_cgroup() - bio가 속한 blkcg가 bic가 마지막으로 본
 *   blkcg와 다르면(cgroup 이전) 관련 큐들을 새 그룹으로 옮긴다.
 *
 * @bic: 태스크의 bfq_io_cq.
 * @bio: 방금 도착한 bio(어느 blkcg에서 왔는지 판단하는 근거).
 * @return: 없음(void).
 *
 * cgroup 이전을 감지해 bic->blkcg_serial_nr을 갱신하고 bfq_bfqq_move()를
 * 호출, 장치 cgroup 분류를 최신 상태로 유지한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_init_rq()/bio 처리 경로 -> [이 함수] -> bfq_bfqq_move().
 */
void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio);
/*
 * [한국어]
 * bfq_end_wr_async() - 디바이스 전체(모든 그룹)의 async 큐에 대해
 *   weight-raising을 종료.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: 없음(void).
 *
 * low_latency가 꺼지거나 전역 정책이 바뀔 때, async 경로의
 * WR 우선순위를 일괄 정상화한다. bfqd->lock 하에서 호출.
 * 호출 체인: sysfs write 핸들러(low_latency 변경 등) -> [이 함수] ->
 *   bfq_end_wr_async_queues() (그룹마다).
 */
void bfq_end_wr_async(struct bfq_data *bfqd);
/*
 * [한국어]
 * bfq_bio_bfqg() - bio가 속한 blkcg에 대응하는 이 디바이스의 bfq_group을
 *   찾는다(없으면 생성 경로로 이어질 수 있음).
 *
 * @bfqd: 디바이스 전역 상태.
 * @bio: 그룹을 알아낼 bio.
 * @return: 유효한 bfq_group 포인터(cgroup 미지정이면 root_group).
 *
 * bio-merge/삽입 경로에서 이 bio가 어느 cgroup 자원으로 과금될지
 * 결정하는 데 쓰인다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_init_rq()/bfq_bio_merge() -> [이 함수].
 */
struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio);
/*
 * [한국어]
 * bfqg_to_blkg() - bfq_group에서 그 상위의 공통 blkcg_gq(cgroup+디바이스
 *   공통 표현)를 얻는다.
 *
 * @bfqg: 변환할 그룹.
 * @return: 대응하는 struct blkcg_gq 포인터.
 *
 * bfq_group.pd(blkg_policy_data)로부터 container_of 스타일로 blkg를
 * 역산하는 헬퍼. blktrace cgroup 태깅(bfq_log_bfqq 매크로) 등에 쓰인다.
 * 호출 체인: bfq_log_bfqq() 매크로 -> [이 함수].
 */
struct blkcg_gq *bfqg_to_blkg(struct bfq_group *bfqg);
/*
 * [한국어]
 * bfqq_group() - bfq_queue가 현재 속한 bfq_group을 반환.
 *
 * @bfqq: 조회 대상 큐.
 * @return: entity.parent 체인을 따라간 소속 그룹.
 *
 * 큐의 entity로부터 상위로 올라가 그 큐가 어느 cgroup에 과금되는지
 * 판단한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_log_bfqq() 매크로, bfq_bfqq_move() 등 -> [이 함수].
 */
struct bfq_group *bfqq_group(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_create_group_hierarchy() - 지정된 NUMA 노드에 이 디바이스의
 *   cgroup 계층(필요한 조상 그룹들 포함)을 생성.
 *
 * @bfqd: 디바이스 전역 상태.
 * @node: 메모리 할당에 쓸 NUMA 노드 힌트.
 * @return: 생성된(또는 이미 존재하던) bfq_group 포인터, 실패 시 NULL.
 *
 * 디바이스 초기화 시 root_group을 만들거나, 새 cgroup이 이 디바이스에
 * 처음 I/O를 낼 때 그 cgroup에 대응하는 bfq_group 트리를 구축해 장치
 * cgroup 자원 분배 트리를 완성한다. bfqd->lock 하에서 호출(할당은
 * GFP_NOWAIT/GFP_KERNEL 등 컨텍스트에 맞게 조정될 수 있음).
 * 호출 체인: bfq_init_queue()/bfq_pd_alloc() -> [이 함수].
 */
struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node);
/*
 * [한국어]
 * bfqg_and_blkg_put() - bfq_group과 그 상위 blkg에 대한 참조를 함께 반환.
 *
 * @bfqg: 대상 그룹.
 * @return: 없음(void).
 *
 * bfq_group.ref와 대응 blkg의 참조를 짝 지어 해제해 장치 cgroup
 * 자원(메모리)을 회수한다. bfqd->lock 하에서 호출.
 * 호출 체인: 그룹을 더 이상 쓰지 않게 되는 여러 경로(큐 소멸, 그룹 이전 등)
 *   -> [이 함수].
 */
void bfqg_and_blkg_put(struct bfq_group *bfqg);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
/* [한국어] cgroup v1(legacy) blkio 컨트롤러에 노출할 control file 배열
 * (blkio.bfq.weight 등). blkcg_policy_bfq 등록 시 함께 등록되어 장치
 * cgroup 정책을 사용자 공간에 노출한다. blkcg_policy_register()가 참조. */
extern struct cftype bfq_blkcg_legacy_files[];
/* [한국어] cgroup v2 io 컨트롤러에 노출할 control file 배열
 * (io.bfq.weight 등). blkcg_policy_bfq 등록 시 함께 등록되어 NVMe
 * SQ-장치 단위 정책을 노출한다. blkcg_policy_register()가 참조. */
extern struct cftype bfq_blkg_files[];
/* [한국어] blkcg 프레임워크에 BFQ를 하나의 I/O 컨트롤러 정책으로 등록하기
 * 위한 struct blkcg_policy 인스턴스. cpd_alloc_fn/pd_alloc_fn 등 콜백을
 * 통해 bfq_group_data/bfq_group 생성이 blkcg 코어와 연결된다(장치
 * cgroup 프레임워크 연결점). bfq_init()이 blkcg_policy_register()로 등록. */
extern struct blkcg_policy blkcg_policy_bfq;
#endif

/* ------------- end of cgroups-support interface ------------- */
/* - interface of the internal hierarchical B-WF2Q+ scheduler - */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
/* both next loops stop at one of the child entities of the root group */
/*
 * [한국어]
 * for_each_entity(entity) - entity로부터 시작해 entity->parent를 계속
 *   따라가며 cgroup 계층 최상위(루트 그룹의 자식, parent==NULL)까지
 *   반복하는 순회 매크로.
 *
 * @entity: 루프 변수로 쓰이는 struct bfq_entity * 좌변값(루프가 끝나면
 *   NULL이 됨).
 *
 * budget/weight 갱신처럼 "이 entity부터 루트까지 모든 조상에 같은 처리를
 * 적용"해야 하는 코드에서 쓰인다. 장치 자원이 cgroup 계층을 따라
 * 상위로 집계(propagate)되는 연산(예: allocated 카운터 갱신)에 이 매크로가
 * 반복적으로 등장한다. bfqd->lock을 쥔 상태에서 사용해야 안전하다
 * (entity->parent 체인이 락 없이 바뀌지 않는다는 전제).
 */
#define for_each_entity(entity)	\
	for (; entity ; entity = entity->parent)
/*
 * For each iteration, compute parent in advance, so as to be safe if
 * entity is deallocated during the iteration. Such a deallocation may
 * happen as a consequence of a bfq_put_queue that frees the bfq_queue
 * containing entity.
 */
/*
 * [한국어]
 * for_each_entity_safe(entity, parent) - for_each_entity()와 같은
 *   상향 순회이지만, 매 반복에서 parent를 미리 계산해 두어 루프 몸체
 *   안에서 entity 자신이 해제(free)되어도 안전한 버전.
 *
 * @entity: 시작 entity(루프 도중 free될 수 있음을 전제).
 * @parent: 다음 반복에 쓸 부모를 미리 담아 둘 보조 변수.
 *
 * bfq_put_queue()가 entity를 포함한 bfq_queue를 실제로 해제할 수 있는
 * 콜 경로(예: 참조 카운트 정리 도중 상위로 전파)에서, entity->parent를
 * 매번 나중에 읽으면 이미 해제된 메모리를 참조(use-after-free)할 위험이
 * 있어 이 안전한 변형을 쓴다. bfqd->lock 하에서 사용.
 */
#define for_each_entity_safe(entity, parent) \
	for (; entity && ({ parent = entity->parent; 1; }); entity = parent)

#else /* CONFIG_BFQ_GROUP_IOSCHED */
/*
 * Next two macros are fake loops when cgroups support is not
 * enabled. I fact, in such a case, there is only one level to go up
 * (to reach the root group).
 */
/*
 * [한국어] CONFIG_BFQ_GROUP_IOSCHED가 꺼진 빌드에서는 계층이 1단계(루트
 * 그룹)뿐이므로, for_each_entity()는 한 번만 실행되고 곧바로 NULL로
 * 끝나는 "가짜 루프"가 된다. 코드 상에서 cgroup 유무에 관계없이 동일한
 * for_each_entity() 호출부를 재사용할 수 있게 해주는 이식성 장치다. */
#define for_each_entity(entity)	\
	for (; entity ; entity = NULL)
/*
 * [한국어] 위와 같은 이유로 for_each_entity_safe()도 1회만 도는 가짜
 * 루프가 된다. cgroup 미지원 빌드에서는 entity 해제 도중 상위로 전파될
 * "상위 레벨"이 애초에 없으므로 안전성 문제 자체가 없다. */
#define for_each_entity_safe(entity, parent) \
	for (parent = NULL; entity ; entity = parent)
#endif /* CONFIG_BFQ_GROUP_IOSCHED */

/*
 * B-WF2Q+ 스케줄러 낸부 함수들. NVMe 관점에서 이 함수들은 SQ로
 * 디스패치될 request의 우선순위와 타이밍을 결정한다.
 *
 * bfq_get_next_queue: 다음에 서비스할 bfq_queue 선택. 선택된 queue의
	 *     head request가 이후 blk_mq_dispatch_rq_list를 통해 장치로
 *     전달된다.
 * bfq_dispatch_requests -> __bfq_dispatch_request -> bfq_get_next_queue
 *     (추정)
 */
/*
 * [한국어]
 * bfq_entity_to_bfqq() - leaf entity(bfq_group이 아니라 bfq_queue를
 *   감싼 entity)로부터 그 bfq_queue를 역산.
 *
 * @entity: leaf entity(entity->my_sched_data가 NULL이어야 함).
 * @return: container_of() 스타일로 얻은 bfq_queue 포인터.
 *
 * B-WF2Q+ 트리는 bfq_entity 단위로만 다루므로, 실제 request가 있는
 * bfq_queue로 "내려가야" 장치 dispatch(next_rq 접근 등)를 할 수 있다.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_get_next_queue() 등 -> [이 함수] -> __bfq_dispatch_request().
 */
struct bfq_queue *bfq_entity_to_bfqq(struct bfq_entity *entity);
/*
 * [한국어]
 * bfq_tot_busy_queues() - 모든 ioprio_class를 합친 busy bfq_queue의
 *   총 개수.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: bfqd->busy_queues[0]+[1]+[2]의 합.
 *
 * 장치 병렬도(동시에 후보가 될 수 있는 큐 수)를 나타내며, 0이면
 * "할 일이 없다"는 elevator has_work 판단에 직결된다.
 * 호출 체인: elevator 코어의 has_work 콜백/여러 판단 로직 -> [이 함수].
 */
unsigned int bfq_tot_busy_queues(struct bfq_data *bfqd);
/*
 * [한국어]
 * bfq_entity_service_tree() - entity의 ioprio_class에 맞는
 *   bfq_service_tree를 entity->sched_data->service_tree[]에서 찾아 반환.
 *
 * @entity: 조회 대상 entity.
 * @return: 해당 클래스의 &service_tree[class] 포인터.
 *
 * entity가 활성화/비활성화될 때마다 "어느 트리에 넣을지"를 결정하는
 * 데 쓰인다(RT/BE/IDLE 중 하나, 장치 클래스 선택).
 * 호출 체인: bfq_activate_requeue_entity()/__bfq_deactivate_entity() 등 -> [이 함수].
 */
struct bfq_service_tree *bfq_entity_service_tree(struct bfq_entity *entity);
/*
 * [한국어]
 * bfq_entity_of() - rb_node로부터 그 노드를 담고 있는 bfq_entity를 역산.
 *
 * @node: rb_node(bfq_entity.rb_node로 연결된 노드).
 * @return: container_of() 스타일로 얻은 bfq_entity 포인터, node가 NULL이면
 *   구현에 따라 NULL 처리.
 *
 * rb_first()/rb_next() 등이 반환하는 raw rb_node를 실제 entity로
 * 되돌리는 공통 헬퍼로, B-WF2Q+ 트리 순회 코드 전반에서 쓰인다.
 * 호출 체인: bfq_get_next_queue()/여러 트리 탐색 함수 -> [이 함수].
 */
struct bfq_entity *bfq_entity_of(struct rb_node *node);
/*
 * [한국어]
 * bfq_ioprio_to_weight() - CFQ 호환 ioprio 값을 BFQ weight로 변환.
 *
 * @ioprio: IOPRIO_PRIO_LEVEL()로 뽑아낸 우선순위 레벨(0~7, 낮을수록
 *   높은 우선순위).
 * @return: BFQ_WEIGHT_CONVERSION_COEFF를 이용해 계산된 weight 값.
 *
 * 사용자가 ioprio(cgroup이 아닌 legacy 인터페이스)로 우선순위를
 * 지정했을 때, 이를 B-WF2Q+가 이해하는 weight 수치로 바꿔 장치
 * 우선순위 계산에 반영한다.
 * 호출 체인: __bfq_entity_update_weight_prio() -> [이 함수].
 */
unsigned short bfq_ioprio_to_weight(int ioprio);
/*
 * [한국어]
 * bfq_put_idle_entity() - idle 트리에서 entity를 제거.
 *
 * @st: entity가 속한 bfq_service_tree.
 * @entity: 제거할 entity.
 * @return: 없음(void).
 *
 * entity가 완전히 정리(free 또는 재활성화 준비)될 때, idle 트리에
 * 남아있던 흔적을 지워 first_idle/last_idle 캐시도 함께 갱신한다.
 * 장치 후보에서 최종 제외되는 처리다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_forget_idle()/bfq_put_queue() 계열 -> [이 함수].
 */
void bfq_put_idle_entity(struct bfq_service_tree *st,
			 struct bfq_entity *entity);
/*
 * [한국어]
 * __bfq_entity_update_weight_prio() - prio_changed로 표시된 entity의
 *   weight/ioprio/ioprio_class 변경을 실제로 반영하고, 필요하면 새
 *   service_tree로 옮긴다.
 *
 * @old_st: entity가 현재 속해 있던 service_tree.
 * @entity: 갱신 대상 entity.
 * @update_class_too: true이면 ioprio_class 변경까지 함께 처리(클래스가
 *   바뀌면 다른 배열 인덱스의 service_tree로 이동해야 함).
 * @return: entity가 최종적으로 속하게 된 새 bfq_service_tree(old_st와
 *   같을 수도, 클래스가 바뀌었으면 다를 수도 있음).
 *
 * "지연 적용(lazy update)" 패턴의 실행 지점으로, entity가 idle 등 안전한
 * 상태로 전이할 때만 호출되어야 트리 정합성이 깨지지 않는다. 장치
 * 후보 트리 재정렬의 핵심 함수다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_activate_requeue_entity() -> [이 함수].
 */
struct bfq_service_tree *
__bfq_entity_update_weight_prio(struct bfq_service_tree *old_st,
				struct bfq_entity *entity,
				bool update_class_too);
/*
 * [한국어]
 * bfq_bfqq_served() - 큐가 실제로 소비한 서비스량을 entity.service 등에
 *   반영하고 가상 시간을 전진.
 *
 * @bfqq: 서비스를 받은 큐.
 * @served: 이번에 소비된 서비스량(섹터 수 등).
 * @return: 없음(void).
 *
 * request가 디스패치/완료될 때마다 호출되어 B-WF2Q+ fairness 과금
 * (누가 얼마나 장치를 썼는지)을 갱신하고 vtime을 전진시킨다.
 * bfqd->lock 하에서 호출.
 * 호출 체인: bfq_dispatch_request()/bfq_completed_request() -> [이 함수].
 */
void bfq_bfqq_served(struct bfq_queue *bfqq, int served);
/*
 * [한국어]
 * bfq_bfqq_charge_time() - 실제 소비량 대신 "시간(ms)" 기준으로 큐에
 *   서비스를 과금(주로 idling 등으로 실제 request 없이 시간만 흐른 경우).
 *
 * @bfqd: 디바이스 전역 상태(peak_rate 등 시간->서비스량 환산에 참조).
 * @bfqq: 과금 대상 큐.
 * @time_ms: 과금할 시간(밀리초).
 * @return: 없음(void).
 *
 * device idling으로 실제 request 처리 없이 시간이 흘렀을 때도 그
 * 시간만큼 fairness 계산에 반영해야 장치 latency 보장이 무너지지
 * 않는다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bfqq_expire()(BFQQE_TOO_IDLE 등) -> [이 함수].
 */
void bfq_bfqq_charge_time(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  unsigned long time_ms);
/*
 * [한국어]
 * __bfq_deactivate_entity() - entity를 active 트리에서 제거하고, 필요시
 *   idle 트리로 옮기며 부모 방향으로 min_start 등을 재계산.
 *
 * @entity: 비활성화할 entity.
 * @ins_into_idle_tree: true이면 idle 트리로 옮김(F_i가 아직 vtime보다
 *   크다는 뜻), false이면 완전히 제거.
 * @return: true이면 부모 entity의 재조정이 더 필요함(호출자가 상위로
 *   전파해야 함)을 의미, false이면 이 레벨에서 종료.
 *
 * 장치 후보에서 이 entity(및 그 하위 bfq_queue/bfq_group)를 제외하는
 * 핵심 저수준 함수다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_deactivate_bfqq() -> [이 함수] (for_each_entity로 상위 전파).
 */
bool __bfq_deactivate_entity(struct bfq_entity *entity,
			     bool ins_into_idle_tree);
/*
 * [한국어]
 * next_queue_may_preempt() - next_in_service가 현재 in_service_entity를
 *   선점할 자격이 있는지 판단.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: true이면 선점 가능(즉시 서비스 전환을 고려해야 함).
 *
 * 더 높은 우선순위 큐가 활성화되었을 때 장치 우선순위 전환(선점)을
 * 판단하는 게이트 함수다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bfqq_expire()/새 큐 활성화 경로 -> [이 함수].
 */
bool next_queue_may_preempt(struct bfq_data *bfqd);
/*
 * [한국어]
 * bfq_get_next_queue() - B-WF2Q+ 트리에서 다음에 서비스할 bfq_queue를
 *   선택(가장 높은 우선순위 클래스부터, 그 안에서 최소 F_i를 가진
 *   entity를 재귀적으로 leaf까지 하강).
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: 다음 서비스 대상 bfq_queue, 활성 큐가 전혀 없으면 NULL.
 *
 * 선택된 큐의 head request가 이후 blk_mq_dispatch_rq_list()를 통해
 * 장치로 전달되는, BFQ 스케줄링의 핵심 진입점이다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_dispatch_request() -> __bfq_dispatch_request() ->
 *   [이 함수] (추정).
 */
struct bfq_queue *bfq_get_next_queue(struct bfq_data *bfqd);
/*
 * [한국어]
 * __bfq_bfqd_reset_in_service() - 현재 in_service_queue/in_service_entity를
 *   NULL로 리셋하고 관련 상태(idle 타이머 등)를 정리.
 *
 * @bfqd: 디바이스 전역 상태.
 * @return: true이면 리셋 과정에서 추가 정리가 필요했음을 의미(구현에
 *   따라 호출자가 후속 처리를 하도록 신호).
 *
 * 서비스 중이던 큐가 만료될 때 "지금 아무도 서비스 중이 아님" 상태로
 * 되돌리는 장치 완료 후 정리 단계다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bfqq_expire() -> [이 함수].
 */
bool __bfq_bfqd_reset_in_service(struct bfq_data *bfqd);
/*
 * [한국어]
 * bfq_deactivate_bfqq() - bfq_queue를 서비스 트리에서 비활성화(필요하면
 *   idle 트리로 이동)하고, 그 entity에 대해 __bfq_deactivate_entity()를
 *   호출해 상위로 전파.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 비활성화할 큐.
 * @ins_into_idle_tree: idle 트리로 옮길지 여부.
 * @expiration: 이 비활성화가 정상 만료로 인한 것인지(통계/로깅 구분용).
 * @return: 없음(void).
 *
 * 장치 후보에서 큐를 빼는 상위 레벨 API(entity 레벨의
 * __bfq_deactivate_entity를 래핑). bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bfqq_expire()/bfq_del_bfqq_busy() -> [이 함수] ->
 *   __bfq_deactivate_entity() (for_each_entity).
 */
void bfq_deactivate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			 bool ins_into_idle_tree, bool expiration);
/*
 * [한국어]
 * bfq_activate_bfqq() - bfq_queue를 active 트리에 삽입(새 타임스탬프
 *   계산 포함)해 서비스 후보로 만든다.
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 활성화할 큐.
 * @return: 없음(void).
 *
 * request가 새로 도착해 큐가 backlogged로 전이할 때 호출되어, 장치
 * 후보 풀에 이 큐를 추가한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_add_bfqq_busy()/bfq_requeue_bfqq() -> [이 함수].
 */
void bfq_activate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_requeue_bfqq() - 이미 알고 있던 큐를 다시 서비스 트리에 넣는다
 *   (완전히 새로운 활성화가 아니라 "재-큐잉").
 *
 * @bfqd: 디바이스 전역 상태.
 * @bfqq: 재큐잉할 큐.
 * @expiration: 방금 만료 처리를 거친 뒤의 재큐잉인지 여부(타임스탬프
 *   계산 방식에 영향).
 * @return: 없음(void).
 *
 * 예산을 다 쓰고도 여전히 request가 남아있는 큐를 트리에 다시 넣어
 * 장치 후보로 유지한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_bfqq_expire()의 재큐잉 분기 -> [이 함수].
 */
void bfq_requeue_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		      bool expiration);
/*
 * [한국어]
 * bfq_del_bfqq_busy() - 큐를 bfqd->busy_queues[]/active_list[] 등
 *   "busy" 관련 전역 인덱스에서 제거.
 *
 * @bfqq: 대상 큐.
 * @expiration: 만료로 인한 제거인지 여부(통계 구분용).
 * @return: 없음(void).
 *
 * 큐가 완전히 idle이 될 때(더 이상 대기 request가 없을 때) 호출되어
 * 장치 병렬도 카운트를 낮춘다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_deactivate_bfqq() 계열 -> [이 함수].
 */
void bfq_del_bfqq_busy(struct bfq_queue *bfqq, bool expiration);
/*
 * [한국어]
 * bfq_add_bfqq_busy() - 큐를 bfqd->busy_queues[]/active_list[] 등에 추가.
 *
 * @bfqq: 대상 큐.
 * @return: 없음(void).
 *
 * bfq_del_bfqq_busy()의 역연산으로, 큐가 backlogged로 전이할 때 장치
 * 병렬도 카운트를 높인다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_activate_bfqq() 계열 -> [이 함수].
 */
void bfq_add_bfqq_busy(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_add_bfqq_in_groups_with_pending_reqs() - 큐가 처음으로 pending
 *   request를 갖게 될 때, 그 그룹을
 *   bfqd->num_groups_with_pending_reqs 집계에 반영.
 *
 * @bfqq: 대상 큐.
 * @return: 없음(void).
 *
 * entity.in_groups_with_pending_reqs 플래그로 중복 카운트를 막으며,
 * NVMe in-flight cgroup 분산 통계를 갱신한다. bfqd->lock 하에서 호출.
 * 호출 체인: request 삽입 경로 -> [이 함수].
 */
void bfq_add_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_del_bfqq_in_groups_with_pending_reqs() - 큐의 pending request가
 *   모두 사라졌을 때 그룹의 pending 집계에서 제외.
 *
 * @bfqq: 대상 큐.
 * @return: 없음(void).
 *
 * bfq_add_bfqq_in_groups_with_pending_reqs()의 역연산(원본 주석에 설명된
 * 근사 감소 전략을 따름). bfqd->lock 하에서 호출.
 * 호출 체인: request 완료/제거 경로 -> [이 함수].
 */
void bfq_del_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_reassign_last_bfqq() - "마지막으로 생성된 큐" 캐시(entity/bfqd의
 *   last_bfqq_created)를 cur_bfqq에서 new_bfqq로 재할당.
 *
 * @cur_bfqq: 기존에 캐시되어 있던 큐(merge/split으로 대체될 대상).
 * @new_bfqq: 새로 캐시할 큐.
 * @return: 없음(void).
 *
 * merge/split 이후에도 stable-merge 후보 판단(생성 시각 비교)이 올바른
 * 큐를 가리키도록 병합 예측 상태를 복원한다. bfqd->lock 하에서 호출.
 * 호출 체인: bfq_merge_bfqqs()/bfq_split_bfqq() -> [이 함수].
 */
void bfq_reassign_last_bfqq(struct bfq_queue *cur_bfqq,
			    struct bfq_queue *new_bfqq);

/* --------------- end of interface of B-WF2Q+ ---------------- */

/* Logging facilities. */
/*
 * [한국어]
 * bfq_bfqq_name() - 로깅/blktrace 메시지에 쓸 사람이 읽을 수 있는 큐
 *   이름 문자열을 만든다.
 *
 * @bfqq: 이름을 만들 대상 bfq_queue.
 * @str: 결과 문자열을 쓸 버퍼(호출자가 제공, 최소 MAX_BFQQ_NAME_LENGTH
 *   바이트 이상이어야 함).
 * @len: str 버퍼의 크기(snprintf의 잘림 방지용 상한).
 * @return: 없음(void, 결과는 str에 기록).
 *
 * 이 큐가 sync인지 async인지, 그리고 특정 프로세스 전용인지(PID 보유)
 * 아니면 여러 프로세스가 공유하는 merge된 큐인지("SHARED")를 한 눈에
 * 알아볼 수 있는 "bfq<pid><S|A>" 또는 "bfqSHARED-<S|A>" 형태 문자열을
 * 만든다. NVMe blktrace 메시지에서 어느 큐가 어떤 이벤트를 냈는지
 * 식별하는 태그로 쓰인다. 별도의 락 없이도 안전한 순수 포맷팅 함수이며
 * (bfqq->pid/sync 플래그를 읽기만 함), 재진입 문제가 없다.
 * 호출자: bfq_log_bfqq() 매크로(양쪽 분기 모두).
 * 피호출자: bfq_bfqq_sync()(플래그 조회), snprintf().
 * 에러 처리: 별도 실패 경로 없음(버퍼 크기 내에서 잘릴 수 있으나
 *   로깅 목적이므로 치명적이지 않음).
 * 호출 체인: bfq_log_bfqq() 매크로 -> [이 함수] -> blk_add_trace_msg() 계열.
 */
static inline void bfq_bfqq_name(struct bfq_queue *bfqq, char *str, int len)
{
	char type = bfq_bfqq_sync(bfqq) ? 'S' : 'A'; // sync/async를 한 글자로 구분 - 트레이스에서 같은 프로세스의 두 큐를 눈으로 갈라 보기 위한 접미사
	if (bfqq->pid != -1) // pid가 -1이 아니면 이 큐는 특정 프로세스 전용(공유되지 않음) - merge되어 소유자가 사라진 공유 큐와 구분하는 조건
		snprintf(str, len, "bfq%d%c", bfqq->pid, type); // "bfq<pid><S|A>" 형태 - blktrace 로그에서 어느 프로세스의 큐인지 바로 식별할 수 있게 한다
	/* [한국어] pid가 -1이면 병합으로 여러 프로세스가 공유하게 된 큐라는 뜻이다.
	 * 이때 특정 pid를 찍으면 로그를 읽는 사람이 "그 프로세스만의 I/O"로
	 * 오해하므로, 소유자가 없음을 이름 자체로 드러낸다. */
	else
		snprintf(str, len, "bfqSHARED-%c", type);
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
/* [한국어] 위 B-WF2Q+ 인터페이스 섹션에서 이미 선언된 것과 동일한
 * bfqq_group()의 재선언(헤더 내 다른 섹션에서도 이 함수가 필요해
 * 중복 선언됨, C에서는 동일 시그니처의 재선언이 허용됨). bfq_log_bfqq()
 * 매크로가 cgroup 트레이스 태깅을 위해 이 함수로 큐의 그룹을 찾는다. */
struct bfq_group *bfqq_group(struct bfq_queue *bfqq);
/*
 * [한국어]
 * bfq_log_bfqq(bfqd, bfqq, fmt, args...) - 특정 bfq_queue와 관련된
 *   이벤트를 blktrace에 cgroup 태그와 함께 기록하는 로깅 매크로
 *   (CONFIG_BFQ_GROUP_IOSCHED 활성 버전).
 *
 * @bfqd: 로그를 남길 디바이스(bfqd->queue가 blktrace 대상).
 * @bfqq: 이벤트의 주체가 되는 bfq_queue(이름 태그 생성에 사용).
 * @fmt, args...: printf 스타일 포맷 문자열과 인자.
 *
 * blk_trace_note_message_enabled()로 blktrace가 켜져 있는지 먼저 확인해
 * (likely 분기로 꺼져 있는 일반적인 경우의 오버헤드를 최소화) 꺼져
 * 있으면 즉시 빠져나간다. 켜져 있으면 bfq_bfqq_name()으로 큐 이름을
 * 만들고, blk_add_cgroup_trace_msg()로 이 큐가 속한 cgroup(bfqq_group()
 * ->bfqg_to_blkg()->blkcg->css)까지 태깅해 메시지를 남긴다. 장치/CQ
 * 이벤트를 프로세스+cgroup 단위로 추적하기 위한 디버깅 도구다.
 * 실행 컨텍스트: 스케줄러 락(bfqd->lock)을 쥔 프로세스/소프트IRQ 컨텍스트
 * 어디서든 호출 가능(문자열 포맷팅만 하므로 블로킹하지 않음).
 * 호출 체인: bfq-iosched.c/bfq-wf2q.c/bfq-cgroup.c 전역의 디버깅 로그
 *   지점 -> [이 매크로] -> blk_add_cgroup_trace_msg() -> blktrace 링버퍼.
 */
#define bfq_log_bfqq(bfqd, bfqq, fmt, args...)	do {			\
	char pid_str[MAX_BFQQ_NAME_LENGTH];				\
	if (likely(!blk_trace_note_message_enabled((bfqd)->queue)))	\
		break;							\
	bfq_bfqq_name((bfqq), pid_str, MAX_BFQQ_NAME_LENGTH);		\
	blk_add_cgroup_trace_msg((bfqd)->queue,				\
			&bfqg_to_blkg(bfqq_group(bfqq))->blkcg->css,	\
			"%s " fmt, pid_str, ##args);			\
} while (0)
#else /* CONFIG_BFQ_GROUP_IOSCHED */
/*
 * [한국어]
 * bfq_log_bfqq(bfqd, bfqq, fmt, args...) - 위와 같은 목적의 로깅
 *   매크로이나, CONFIG_BFQ_GROUP_IOSCHED가 꺼진 빌드용 버전. cgroup
 *   개념이 없으므로 cgroup 태그 없이 blk_add_trace_msg()만 사용한다.
 * 나머지 파라미터/동작/컨텍스트는 위 CONFIG_BFQ_GROUP_IOSCHED 버전과 동일.
 */
#define bfq_log_bfqq(bfqd, bfqq, fmt, args...) do {	\
	char pid_str[MAX_BFQQ_NAME_LENGTH];				\
	if (likely(!blk_trace_note_message_enabled((bfqd)->queue)))	\
		break;							\
	bfq_bfqq_name((bfqq), pid_str, MAX_BFQQ_NAME_LENGTH);		\
	blk_add_trace_msg((bfqd)->queue, "%s " fmt, pid_str, ##args);	\
} while (0)

#endif /* CONFIG_BFQ_GROUP_IOSCHED */
/*
 * [한국어]
 * bfq_log(bfqd, fmt, args...) - 특정 큐가 아니라 디바이스(bfqd) 전체
 *   차원의 이벤트를 blktrace에 기록하는 로깅 매크로.
 *
 * @bfqd: 로그를 남길 디바이스.
 * @fmt, args...: printf 스타일 포맷 문자열과 인자.
 *
 * bfq_log_bfqq()와 달리 큐 이름 태그나 cgroup 태그 없이 "bfq " 접두사만
 * 붙여 blk_add_trace_msg()를 직접 호출한다. 예산 계산, peak_rate 갱신 등
 * 큐 단위가 아닌 전역 장치 이벤트를 기록할 때 쓰인다.
 * 호출 체인: bfq-iosched.c의 전역 상태 갱신 지점 -> [이 매크로] ->
 *   blk_add_trace_msg() -> blktrace 링버퍼.
 */
#define bfq_log(bfqd, fmt, args...) \
	blk_add_trace_msg((bfqd)->queue, "bfq " fmt, ##args)

/*
 * =====================================================================
 * 이 헤더가 정의하는 정책 모델 요약
 * =====================================================================
 * - bfq_queue는 하나의 프로세스(또는 병합된 협력 프로세스 집합, 또는
 *   그룹 공유 async 큐)의 대기열이며, 하나의 actuator(독립 접근 영역)만
 *   담당한다. sort_list의 LBA 정렬은 위치가 가까운 요청을 연달아
 *   내보내기 위한 것이다.
 * - bfq_data는 아직 드라이버로 넘기지 않은 request 풀(dispatch 리스트)과
 *   actuator별 in-flight 개수(rq_in_driver[])를 관리한다. 후자는 태그
 *   고갈 방지가 아니라 injection 한도와 hw_tag 판정의 입력이다.
 * - B-WF2Q+ 스케줄러는 bfq_entity의 가상 시간(start/finish)으로 다음에
 *   서비스할 큐를 정한다. F_i = S_i + budget/weight이므로, weight가 큰
 *   entity일수록 같은 budget에 대해 finish time이 작아 더 자주 뽑힌다.
 * - weight raising은 대화형/soft real-time으로 보이는 큐의 weight를
 *   한시적으로 올려 응답성을 확보하는 장치이고, idling은 그 큐가 다음
 *   요청을 낼 때까지 장치를 비워 두어 몫과 순차성을 지키는 장치다.
 *   둘 다 처리량을 담보로 지연시간을 사는 거래이므로, 내부 병렬성이
 *   큰 장치에서는 손해가 되어 여러 휴리스틱(hw_tag,
 *   nonrot_with_queueing, inject_limit)이 이를 되돌린다.
 * - BFQ는 장치 종류에 중립적인 스케줄러이며, 위 자료구조 어디에도
 *   특정 드라이버의 큐/커맨드 구조에 대한 의존은 없다. 다만 bfqd->lock이
 *   장치 단위 전역 락이라는 점 때문에, 하드웨어 큐가 여럿인 고 IOPS
 *   장치에서는 확장성 한계가 드러난다.
 * - 이 파일은 block/bfq-iosched.c(메인 로직), block/bfq-wf2q.c(B-WF2Q+),
 *   block/bfq-cgroup.c(cgroup 정책)와 함께 컴파일되며, 위로는
 *   block/blk-mq-sched.c / block/blk-mq.c의 디스패치 경로와 맞닿는다.
 * =====================================================================
 */

#endif /* _BFQ_H */
