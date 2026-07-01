/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Header file for the BFQ I/O scheduler: data structures and
 * prototypes of interface functions among BFQ components.
 */

/*
 * =====================================================================
 * NVMe 관점 파일 개요 (5-10줄)
 * =====================================================================
 * 이 헤더 파일은 BFQ(Budget Fair Queueing) I/O 스케줄러의 핵심 데이터
 * 구조체와 낸부 모듈 간 인터페이스 함수 원형을 정의한다. NVMe SSD에서
 * 응용 프로그램이 낸 bio를 blk_mq_submit_bio()가 받아들이면, BFQ는 이
 * 파일의 bfq_queue/bfq_entity를 통해 요청을 스케줄링하고, 최종적으로
 * blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
 * 경로로 NVMe Submission Queue(SQ)에 삽입될 request를 선별한다. 따라서
 * 이 파일은 NVMe SQ/CQ 자원 할당, actuator 단위 큐 관리, 그리고 latency
 * 보장을 위한 예산(budget) 제어의 상위 계층 정책을 담당한다.
 * =====================================================================
 */

#ifndef _BFQ_H
#define _BFQ_H

#include <linux/blktrace_api.h> // blktrace는 NVMe SQ/CQ 이벤트 추적 및 latency 분석용 인터페이스
#include <linux/hrtimer.h> // hrtimer 기반 idling; NVMe doorbell batching의 타이밍 제어에 사용

#include "blk-cgroup-rwstat.h" // cgroup별 NVMe 대역폭/IO 통계를 위한 rwstat 헤더

/*
 * BFQ 우선순위 클래스 수: RT/BE/IDLE. NVMe에서는 이 클래스가 SQ로
 * 디스패치되는 상대적 우선순위를 결정한다.
 */
#define BFQ_IOPRIO_CLASSES	3 // RT/BE/IDLE 3클스는 NVMe SQ 우선순위 그룹화와 대응됨
/*
 * 큐가 idle 상태로 남아 있을 수 있는 최대 시간. NVMe에서 이 값이 너무
 * 크면 doorbell batching 기회를 잃고, 너무 작으면 sequential locality를
 * 해칠 수 있다.
 */
#define BFQ_CL_IDLE_TIMEOUT	(HZ/5) // 200ms idle timeout; NVMe CQ 완료 후 다음 doorbell 지연 상한

#define BFQ_MIN_WEIGHT			1 // 최소 weight; NVMe SQ 자원 분배 비율 하한
#define BFQ_MAX_WEIGHT			1000 // 최대 weight; NVMe SQ 자원 분배 비율 상한
#define BFQ_WEIGHT_CONVERSION_COEFF	10 // weight 변환 계수; ioprio -> NVMe SQ 우선순위 매핑에 사용

#define BFQ_DEFAULT_QUEUE_IOPRIO	4 // 기본 queue ioprio; NVMe SQ 내 상대 우선순위 결정

#define BFQ_DEFAULT_GRP_IOPRIO	0 // 기본 그룹 ioprio; NVMe SQ cgroup 기본 우선순위
#define BFQ_DEFAULT_GRP_CLASS	IOPRIO_CLASS_BE // 기본 그룹 클래스; NVMe SQ BE 클래스 기본값

#define MAX_BFQQ_NAME_LENGTH 16 // bfq_queue 이름 최대 길이; NVMe trace/로그 식별자

/*
 * Soft real-time 응용은 대화형(interactive)보다 latency에 민감하므로
 * 가중치를 크게 높인다. NVMe CQ 완료 기준으로 민감한 워크로드가
 * SQ 슬롯을 먼저 확보하도록 한다.
 */
#define BFQ_SOFTRT_WEIGHT_FACTOR	100 // soft-RT 가중치 배율; NVMe latency-critical 워크로드의 SQ 우선도 상승

/*
 * 지원하는 최대 actuator 수. NVMe SSD에서 하나의 namespace는 여러
 * 독립 actuator(Independent Access Range)를 가질 수 있으며, 각
 * actuator는 자신의 SQ/CQ 쌍 또는 NVMe queue를 통해 병렬 처리된다.
 */
#define BFQ_MAX_ACTUATORS 8 // NVMe Independent Access Ranges 최대 개수; actuator/SQ 쌍 매핑 (추정)

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
	struct rb_root active; // active entity 트리; 아직 NVMe SQ로 디스패치되지 않은 후보 집합
	/* tree for idle entities (i.e., not backlogged, with V < F_i)*/
	struct rb_root idle; // idle entity 트리; NVMe CQ 완료 후 재doorbell을 대기하는 queue

	/* idle entity with minimum F_i */
	struct bfq_entity *first_idle; // 가장 먼저 idle이 만료될 entity; NVMe SQ 다음 후보 선정 가속
	/* idle entity with maximum F_i */
	struct bfq_entity *last_idle; // 가장 나중에 idle이 만료될 entity; NVMe doorbell batching 기회 판단

	/* scheduler virtual time */
	u64 vtime; // B-WF2Q+ 가상 시간; NVMe SQ doorbell 우선순위 계산의 기준값
	/* scheduler weight sum; active and idle entities contribute to it */
	unsigned long wsum; // 가중치 합; NVMe SQ 자원 분배 비율 정규화에 사용
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
 * bfq_queue가 NVMe CQ 완료로 만료될 때 바로 다음에 SQ로 복귀할
 * candidate를 미리 가리켜, doorbell 작성 지연을 줄이는 데 도움을 준다.
 */
struct bfq_sched_data {
	/* entity in service */
	struct bfq_entity *in_service_entity; // 현재 NVMe SQ로 서비스 중인 entity; in-flight request의 소유자
	/* head-of-line entity (see comments above) */
	struct bfq_entity *next_in_service; // 다음 서비스 예정 entity; NVMe CQ 완료 후 빠른 SQ 전환용 후보
	/* array of service trees, one per ioprio_class */
	struct bfq_service_tree service_tree[BFQ_IOPRIO_CLASSES]; // RT/BE/IDLE별 service tree; NVMe SQ 클래스별 후보 풀
	/* last time CLASS_IDLE was served */
	unsigned long bfq_class_idle_last_service; // IDLE 클래스 마지막 서비스 시각; NVMe SQ 기아(starvation) 방지

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
	unsigned int num_active; /* nr of active queues with this weight */ // 동일 weight를 가진 활성 queue 수; BFQ depth limit 보수성에 영향
	/*
	 * Weights tree member (see bfq_data's @queue_weights_tree)
	 */
	struct rb_node weights_node; // weight 트리 노드; NVMe fairness 및 bfq_limit_depth 정책 지원
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
 * NVMe 관점: bfq_queue 또는 bfq_group을 B-WF2Q+ 스케줄러에 등록하는
 * 단위이다. start/finish 타임스탬프는 가상 시간 기반으로 NVMe SQ에
 * 들어갈 후보 순위를 결정하며, budget/weight 비율이 곧 NVMe doorbell
 * 트리거 빈도(fairness)로 이어진다. on_st_or_in_serv 플래그가 true면
 * 해당 entity는 active/idle 트리 또는 현재 서비스 중이므로, NVMe
 * in-flight 요청과 직접 연결될 수 있다.
 */
struct bfq_entity {
	/* service_tree member */
	struct rb_node rb_node; // B-WF2Q+ rb-tree 노드; NVMe SQ로 디스패치될 순서 결정

	/*
	 * Flag, true if the entity is on a tree (either the active or
	 * the idle one of its service_tree) or is in service.
	 */
	bool on_st_or_in_serv; // active/idle 트리 또는 현재 NVMe SQ 서비스 여부 (in-flight 연결)

	/* B-WF2Q+ start and finish timestamps [sectors/weight] */
	u64 start, finish; // 가상 시작/종료 타임스탬프; NVMe doorbell 우선순위의 근거

	/* tree the entity is enqueued into; %NULL if not on a tree */
	struct rb_root *tree; // 소속 service_tree 포인터; NVMe SQ 후보 그룹 식별

	/*
	 * minimum start time of the (active) subtree rooted at this
	 * entity; used for O(log N) lookups into active trees
	 */
	u64 min_start; // 하위 active subtree의 최소 start; NVMe SQ 후보 O(log N) 탐색 가속

	/* amount of service received during the last service slot */
	int service; // 최근 서비스량(sectors); NVMe throughput 추정 및 fairness 과금 입력

	/* budget, used also to calculate F_i: F_i = S_i + @budget / @weight */
	int budget; // 예산; F_i = S_i + budget/weight 로 NVMe doorbell 빈도 계산

	/* Number of requests allocated in the subtree of this entity */
	int allocated; // 하위 subtree에 할당된 request 수; NVMe SQ depth 분배 추적

	/* device weight, if non-zero, it overrides the default weight of
	 * bfq_group_data */
	int dev_weight; // 장치별 weight override; NVMe SQ 자원 분배 우선도 재정의
	/* weight of the queue */
	int weight; // 카운터가 대표하는 weight; 동일 가중치 NVMe queue 그룹 식별
	/* next weight if a change is in progress */
	int new_weight; // 변경 예정 weight; NVMe fairness 재조정 시 적용

	/* original weight, used to implement weight boosting */
	int orig_weight; // 원래 weight; weight raising 복원 시 NVMe SQ 비율 복귀

	/* parent entity, for hierarchical scheduling */
	struct bfq_entity *parent; // cgroup 계층 상위 entity; NVMe SQ 자원 상속 및 집계 경로

	/*
	 * For non-leaf nodes in the hierarchy, the associated
	 * scheduler queue, %NULL on leaf nodes.
	 */
	struct bfq_sched_data *my_sched_data; // 비리프 노드의 스케줄러; NVMe SQ 후보를 그룹별로 재스케줄
	/* the scheduler queue this entity belongs to */
	struct bfq_sched_data *sched_data; // 소속 상위 스케줄러; NVMe SQ dispatch 정책의 컨텍스트

	/* flag, set to request a weight, ioprio or ioprio_class change  */
	int prio_changed; // 우선순위 변경 플래그; 설정 시 NVMe SQ 후보 트리 재정렬

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	/* flag, set if the entity is counted in groups_with_pending_reqs */
	bool in_groups_with_pending_reqs; // pending rq 그룹 카운팅 여부; NVMe in-flight cgroup 분산 추적
#endif

	/* last child queue of entity created (for non-leaf entities) */
	struct bfq_queue *last_bfqq_created; // 마지막으로 생성된 자식 bfq_queue; NVMe SQ stable merge 예측
};

struct bfq_group;

/**
 * struct bfq_ttime - per process thinktime stats.
 *
 * NVMe 관점: 프로세스가 완료 후 다음 요청을 낼 때까지의 think time을
 * 측정한다. ttime_mean이 짧으면 해당 프로세스가 NVMe CQ interrupt/completion
 * 직후 바로 다음 doorbell을 요구할 가능성이 높아, request injection을
 * 억제하여 locality를 유지해야 한다.
 */
struct bfq_ttime {
	/* completion time of the last request */
	u64 last_end_request; // 마지막 request 완료 시각; NVMe CQ interrupt/completion 시점

	/* total process thinktime */
	u64 ttime_total; // 누적 think time; NVMe doorbell 도착 간격 추정 입력
	/* number of thinktime samples */
	unsigned long ttime_samples; // think time 샘플 수; NVMe SQ 예측 정확도
	/* average process thinktime */
	u64 ttime_mean; // 평균 think time; 짧으면 NVMe SQ locality 유지를 위해 injection 억제
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
 * NVMe 관점: 실제 NVMe SQ로 디스패치될 request를 담는 leaf 스케줄링
 * 단위이다. 각 bfq_queue는 단일 actuator만을 대상으로 하므로,
 * actuator_idx가 NVMe Independent Access Range 또는 queue 쌍과 대응될
 * 수 있다. sort_list는 LBA 순서로 정렬되어 있어, NVMe PRP/SGL 준비 및
 * sequential access 최적화에 유리하다. next_rq는 blk_mq_run_hw_queue
 * -> bfq_dispatch_requests -> __bfq_dispatch_request 경로에서 NVMe
 * 드라이버로 전달될 다음 request 후보이다.
 */
struct bfq_queue {
	/* reference counter */
	int ref; // 참조 카운트; NVMe request 생명주기 동안 bfq_queue 유지
	/* counter of references from other queues for delayed stable merge */
	int stable_ref; // delayed stable merge 참조; NVMe SQ 경로 재사용 안정성
	/* parent bfq_data */
	struct bfq_data *bfqd; // 상위 bfq_data; NVMe 장치/SQ 컨텍스트 역참조

	/* current ioprio and ioprio class */
	unsigned short ioprio, ioprio_class; // 현재 ioprio/class; NVMe SQ 우선순위 클래스 결정
	/* next ioprio and ioprio class if a change is in progress */
	unsigned short new_ioprio, new_ioprio_class; // 변경 예정 ioprio/class; NVMe SQ 재분류 보류 값

	/* last total-service-time sample, see bfq_update_inject_limit() */
	u64 last_serv_time_ns; // 마지막 서비스 시간(ns); NVMe SQ request injection 한계 갱신 입력
	/* limit for request injection */
	unsigned int inject_limit; // 허용 injection 개수; NVMe underutilized actuator의 SQ 활용 (추정)
	/* last time the inject limit has been decreased, in jiffies */
	unsigned long decrease_time_jif; // injection 한계 감소 시각; NVMe SQ 깊이 추정 냉각 타이머

	/*
	 * Shared bfq_queue if queue is cooperating with one or more
	 * other queues.
	 */
	struct bfq_queue *new_bfqq; // 공유/병합 대상 queue; NVMe SQ 경로 통합 candidate
	/* request-position tree member (see bfq_group's @rq_pos_tree) */
	struct rb_node pos_node; // LBA 위치 트리 노드; NVMe PRP/SGL sequential 준비를 위한 인접 탐색
	/* request-position tree root (see bfq_group's @rq_pos_tree) */
	struct rb_root *pos_root; // LBA 위치 트리 루트; NVMe SQ 인접 후보(cooperator) 탐색 기준

	/* sorted list of pending requests */
	struct rb_root sort_list; // LBA 정렬된 pending request 트리; NVMe PRP/SGL sequential 배치에 유리
	/* if fifo isn't expired, next request to serve */
	struct request *next_rq;   // 다음 서비스 request; blk_mq_dispatch_rq_list -> nvme_queue_rq 후보
	/* number of sync and async requests queued */
	int queued[2]; // 큐에 대기 중인 IO; NVMe SQ backlog
	/* number of pending metadata requests */
	int meta_pending; // 메타데이터 request 수; NVMe 관리/IO CMD 비율 추적
	/* fifo list of requests in sort_list */
	struct list_head fifo; // FIFO 만료 리스트; NVMe timeout/abort 시 처리 순서

	/* entity representing this queue in the scheduler */
	struct bfq_entity entity; // B-WF2Q+ entity; NVMe SQ cgroup/queue 우선순위

	/* pointer to the weight counter associated with this entity */
	struct bfq_weight_counter *weight_counter; // 가중치 카운터; NVMe fairness 및 bfq_limit_depth 계산

	/* maximum budget allowed from the feedback mechanism */
	int max_budget; // 최대 예산; NVMe SQ에서 한 queue가 독점하지 않도록 제한
	/* budget expiration (in jiffies) */
	unsigned long budget_timeout; // 예산 소비 timeout; NVMe latency 보장을 위한 강제 만료

	/* number of requests on the dispatch list or inside driver */
	int dispatched; // dispatch 리스트 또는 드라이버 내 request 수; NVMe in-flight 추적

	/* status flags */
	unsigned long flags; // BFQQ 상태 플래그; NVMe SQ/CQ 흐름 제어에 직접 영향

	/* node for active/idle bfqq list inside parent bfqd */
	struct list_head bfqq_list; // active/idle bfqq 리스트 노드; NVMe SQ 후보 순회용

	/* associated @bfq_ttime struct */
	struct bfq_ttime ttime; // think time 구조체; NVMe doorbell 타이밍 예측

	/* when bfqq started to do I/O within the last observation window */
	u64 io_start_time; // I/O 관찰 구간 시작 시각; NVMe throughput 측정
	/* how long bfqq has remained empty during the last observ. window */
	u64 tot_idle_time; // 관찰 구간 누적 idle 시간; NVMe SQ idle 비율 추정

	/* bit vector: a 1 for each seeky requests in history */
	u32 seek_history; // seek 패턴 히스토리; NVMe random/sequential 분류 및 예산 조정

	/* node for the device's burst list */
	struct hlist_node burst_list_node; // 버스트 리스트 노드; NVMe SQ activation 폭조 감지

	/* position of the last request enqueued */
	sector_t last_request_pos; // 마지막 enqueue 위치(LBA); NVMe PRP/SGL sequential 연속성 판단

	/* Number of consecutive pairs of request completion and
	 * arrival, such that the queue becomes idle after the
	 * completion, but the next request arrives within an idle
	 * time slice; used only if the queue's IO_bound flag has been
	 * cleared.
	 */
	unsigned int requests_within_timer; // 타이머 내 후속 도착 횟수; NVMe CQ 완료 후 doorbell 예측 신뢰도

	/* pid of the process owning the queue, used for logging purposes */
	pid_t pid; // 소유 pid; NVMe queue trace/디버그 식별자

	/*
	 * Pointer to the bfq_io_cq owning the bfq_queue, set to %NULL
	 * if the queue is shared.
	 */
	struct bfq_io_cq *bic; // per-task io_cq; NVMe SQ 경로의 task별 분리

	/* current maximum weight-raising time for this queue */
	unsigned long wr_cur_max_time; // 현재 weight raising 최대 시간; NVMe latency QoS 보장 기간
	/*
	 * Minimum time instant such that, only if a new request is
	 * enqueued after this time instant in an idle @bfq_queue with
	 * no outstanding requests, then the task associated with the
	 * queue it is deemed as soft real-time (see the comments on
	 * the function bfq_bfqq_softrt_next_start())
	 */
	unsigned long soft_rt_next_start; // soft RT 판단 기준 시각; NVMe 대화형 latency 임계
	/*
	 * Start time of the current weight-raising period if
	 * the @bfq-queue is being weight-raised, otherwise
	 * finish time of the last weight-raising period.
	 */
	unsigned long last_wr_start_finish; // WR 시작/종료 시각; NVMe SQ 우선순위 변동 이력
	/* factor by which the weight of this queue is multiplied */
	unsigned int wr_coeff; // weight raising 계수; NVMe SQ 할당량 배율
	/*
	 * Time of the last transition of the @bfq_queue from idle to
	 * backlogged.
	 */
	unsigned long last_idle_bklogged; // idle -> backlogged 전환 시각; NVMe SQ 재활성 타이밍
	/*
	 * Cumulative service received from the @bfq_queue since the
	 * last transition from idle to backlogged.
	 */
	unsigned long service_from_backlogged; // backlogged 이후 누적 서비스; NVMe fairness 과금
	/*
	 * Cumulative service received from the @bfq_queue since its
	 * last transition to weight-raised state.
	 */
	unsigned long service_from_wr; // WR 기간 누적 서비스; NVMe SQ 우선 서비스량 기록

	/*
	 * Value of wr start time when switching to soft rt
	 */
	unsigned long wr_start_at_switch_to_srt; // SRT 전환 시 WR 시작 시각; NVMe SQ latency 정책 전환 추적
	unsigned long split_time; /* time of last split */
	unsigned long first_IO_time; /* time of first I/O for this queue */
	unsigned long creation_time; /* when this queue is created */
	/*
	 * Pointer to the waker queue for this queue, i.e., to the
	 * queue Q such that this queue happens to get new I/O right
	 * after some I/O request of Q is completed. For details, see
	 * the comments on the choice of the queue for injection in
	 * bfq_select_queue().
	 */
	struct bfq_queue *waker_bfqq; // waker queue; NVMe CQ 완료 후 이 queue로 request가 도착할 예측
	/* pointer to the curr. tentative waker queue, see bfq_check_waker() */
	struct bfq_queue *tentative_waker_bfqq; // 임시 waker queue; NVMe SQ injection 후보 탐색 중
	/* number of times the same tentative waker has been detected */
	unsigned int num_waker_detections; // 동일 waker 탐지 횟수; NVMe SQ 예측 신뢰도
	/* time when we started considering this waker */
	u64 waker_detection_started; // waker 탐지 시작 시각; NVMe CQ 간격 측정

	/* node for woken_list, see below */
	struct hlist_node woken_list_node; // woken 리스트 노드; NVMe SQ 의존 관계 갱신
	/*
	 * Head of the list of the woken queues for this queue, i.e.,
	 * of the list of the queues for which this queue is a waker
	 * queue. This list is used to reset the waker_bfqq pointer in
	 * the woken queues when this queue exits.
	 */
	struct hlist_head woken_list; // woken queue 리스트; NVMe SQ wake 관계 해제 시 사용

	/* index of the actuator this queue is associated with */
	unsigned int actuator_idx; // 대상 actuator 인덱스; NVMe Independent Access Range/queue 쌍 매핑 (추정)
};
/**
* struct bfq_data - bfqq data unique and persistent for associated bfq_io_cq
*/
struct bfq_iocq_bfqq_data {
	/*
	 * Snapshot of the has_short_time flag before merging; taken
	 * to remember its values while the queue is merged, so as to
	 * be able to restore it in case of split.
	 */
	bool saved_has_short_ttime; // merge 전 short think time 플래그; NVMe SQ locality 복원
	/*
	 * Same purpose as the previous two fields for the I/O bound
	 * classification of a queue.
	 */
	bool saved_IO_bound; // merge 전 IO_bound 플래그; NVMe SQ timeout/abort 특성 보존

	/*
	 * Same purpose as the previous fields for the values of the
	 * field keeping the queue's belonging to a large burst
	 */
	bool saved_in_large_burst; // merge 전 large burst 소속; NVMe SQ activation 이력 보존
	/*
	 * True if the queue belonged to a burst list before its merge
	 * with another cooperating queue.
	 */
	bool was_in_burst_list; // burst list 이력; NVMe SQ 폭주 관리 상태 보존

	/*
	 * Save the weight when a merge occurs, to be able
	 * to restore it in case of split. If the weight is not
	 * correctly resumed when the queue is recycled,
	 * then the weight of the recycled queue could differ
	 * from the weight of the original queue.
	 */
	unsigned int saved_weight; // merge 전 weight; NVMe SQ fairness 복원
	u64 saved_io_start_time; // merge 전 I/O 시작 시각; NVMe throughput 이력 보존
	u64 saved_tot_idle_time; // merge 전 누적 idle; NVMe SQ idle 정책 복원

	/*
	 * Similar to previous fields: save wr information.
	 */
	unsigned long saved_wr_coeff; // merge 전 WR 계수; NVMe SQ 우선순위 복원
	unsigned long saved_last_wr_start_finish; // merge 전 WR 시각; NVMe SQ latency 정책 복원
	unsigned long saved_service_from_wr; // merge 전 WR 서비스량; NVMe fairness 복원
	unsigned long saved_wr_start_at_switch_to_srt; // merge 전 SRT 전환 시각; NVMe SQ 정책 상태 복원
	struct bfq_ttime saved_ttime; // merge 전 think time; NVMe doorbell 예측 복원
	unsigned int saved_wr_cur_max_time; // merge 전 WR 최대 시간; NVMe QoS 기간 복원
	/* Save also injection state */
	unsigned int saved_inject_limit; // merge 전 injection 한계; NVMe SQ depth 복원
	unsigned long saved_decrease_time_jif; // merge 전 injection 감소 시각; NVMe SQ 깊이 추정 복원
	u64 saved_last_serv_time_ns; // merge 전 서비스 시간; NVMe SQ injection 한계 복원
	/* candidate queue for a stable merge (due to close creation time) */
	struct bfq_queue *stable_merge_bfqq; // stable merge 후보; NVMe SQ 경로 재사용 candidate
	bool stably_merged;	/* non splittable if true */
};
/**
 * struct bfq_io_cq - per (request_queue, io_context) structure.
 *
 * NVMe 관점: request_queue는 NVMe 컨트롤러의 hardware queue와 연결된다.
 * bfqq[2][BFQ_MAX_ACTUATORS] 배열은 sync/async 및 actuator별로 나뉘어
 * 있으므로, NVMe Multi-Queue의 blk_mq_hw_ctx 또는 queue 쌍과 대응될 수
 * 있다. 이 구조체는 blk_mq_sched_bio_merge() -> bfq_bio_merge() 시
 * bic 조회를 통해 NVMe SQ에서 merge 가능한 bio를 찾는 데 활용된다.
 */
struct bfq_io_cq {
	/* associated io_cq structure */
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
	struct bfq_queue *bfqq[2][BFQ_MAX_ACTUATORS]; // sync/async x actuator bfq_queue; NVMe Multi-Queue/SQ 쌍과 대응 (추정)
	/* per (request_queue, blkcg) ioprio */
	int ioprio;
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	uint64_t blkcg_serial_nr; /* the current blkcg serial */
#endif
	/*
	 * Persistent data for associated synchronous process queues
	 * (one queue per actuator, see field bfqq above). In
	 * particular, each of these queues may undergo a merge.
	 */
	struct bfq_iocq_bfqq_data bfqq_data[BFQ_MAX_ACTUATORS]; // per actuator queue 상태; NVMe SQ 경로 복원 데이터
	unsigned int requests;	/* Number of requests this process has in flight */
};

/**
 * struct bfq_data - per-device data structure.
 *
 * All the fields are protected by @lock.
 *
 * NVMe 관점: NVMe SSD 하나당 하나의 bfq_data가 존재하며, 상위
 * request_queue(struct request_queue *queue)를 감싼다. dispatch 리스트는
 * blk_mq_run_hw_queue -> bfq_dispatch_requests -> blk_mq_dispatch_rq_list
 * -> nvme_queue_rq로 이어지는 경로에서 NVMe SQ로 전달될 request들을
 * 담는다. rq_in_driver[actuator]는 각 actuator별 NVMe in-flight 요청
 * 수(CID/SQ 엔트리 점유 수)를 추적하여, SQ 슬롯 고갈을 방지한다.
 * actuator_load_threshold는 NCQ의 32 슬롯(또는 NVMe SQ 깊이)을 기준으로
 * underutilized actuator를 판별해 request injection을 결정한다.
 */
struct bfq_data {
	/* device request queue */
	struct request_queue *queue; // NVMe 컨트롤러의 request_queue; blk_mq_hw_ctx 상위 객체
	/* dispatch queue */
	struct list_head dispatch; // dispatch queue; NVMe SQ로 전달 직전 request 대기열

	/* root bfq_group for the device */
	struct bfq_group *root_group; // 루트 cgroup 그룹; NVMe SQ 최상위 자원 분배

	/*
	 * rbtree of weight counters of @bfq_queues, sorted by
	 * weight. Used to keep track of whether all @bfq_queues have
	 * the same weight. The tree contains one counter for each
	 * distinct weight associated to some active and not
	 * weight-raised @bfq_queue (see the comments to the functions
	 * bfq_weights_tree_[add|remove] for further details).
	 */
	struct rb_root_cached queue_weights_tree; // weight 카운터 rbtree; NVMe fairness 및 bfq_limit_depth 정책

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
	unsigned int num_groups_with_pending_reqs; // pending rq를 가진 cgroup 수; NVMe in-flight cgroup 분산 지표
#endif

	/*
	 * Per-class (RT, BE, IDLE) number of bfq_queues containing
	 * requests (including the queue in service, even if it is
	 * idling).
	 */
	unsigned int busy_queues[3]; // 클스별 busy queue 수; NVMe SQ 병렬도 및 활성 후보 수
	/* number of weight-raised busy @bfq_queues */
	int wr_busy_queues; // weight-raised busy queue 수; NVMe latency-sensitive SQ 수
	/* number of queued requests */
	int queued; // 큐에 대기 중인 IO; NVMe SQ backlog
	/* number of requests dispatched and waiting for completion */
	int tot_rq_in_driver; // 총 in-flight request 수; NVMe CID/tag/SQ 엔트리 사용량
	/*
	 * number of requests dispatched and waiting for completion
	 * for each actuator
	 */
	int rq_in_driver[BFQ_MAX_ACTUATORS]; // actuator별 in-flight 수; NVMe per-queue 쌍 SQ depth 관리
	/* true if the device is non rotational and performs queueing */
	bool nonrot_with_queueing; // non-rotational + queueing; NVMe SSD임을 나타내는 플래그

	/*
	 * Maximum number of requests in driver in the last
	 * @hw_tag_samples completed requests.
	 */
	int max_rq_in_driver; // 최근 최대 in-flight; NVMe SQ 실제 깊이 추정 입력
	/* number of samples used to calculate hw_tag */
	int hw_tag_samples; // hw_tag 샘플 수; NVMe SQ 병렬 처리(tagging) 판단 신뢰도
	/* flag set to one if the driver is showing a queueing behavior */
	int hw_tag; // 드라이버 queueing 행동 플래그; NVMe SQ 효율 지표
	/* number of budgets assigned */
	int budgets_assigned; // 예산 할당 횟수; NVMe throughput calibration 빈도

	/*
	 * Timer set when idling (waiting) for the next request from
	 * the queue in service.
	 */
	struct hrtimer idle_slice_timer; // idling hrtimer; NVMe doorbell batching/지연 타이밍 제어
	/* bfq_queue in service */
	struct bfq_queue *in_service_queue; // 현재 서비스 중인 bfq_queue; NVMe SQ dispatch의 실제 타깃
	/* on-disk position of the last served request */
	sector_t last_position; // 마지막 서비스 LBA; NVMe sequentiality 및 PRP/SGL 연속성
	/* position of the last served request for the in-service queue */
	sector_t in_serv_last_pos; // in-service queue의 마지막 LBA; NVMe PRP/SGL 연속 준비
	/* time of last request completion (ns) */
	u64 last_completion; // 마지막 NVMe CQ 완료 시각; injection/latency 계산 기준
	/* bfqq owning the last completed rq */
	struct bfq_queue *last_completed_rq_bfqq; // 마지막 완료 rq의 bfq_queue; NVMe CQ 기반 injection 판단
	/* last bfqq created, among those in the root group */
	struct bfq_queue *last_bfqq_created; // 마지막으로 생성된 자식 bfq_queue; NVMe SQ stable merge 예측
	/* time of last transition from empty to non-empty (ns) */
	u64 last_empty_occupied_ns; // empty -> non-empty 전환 시각; NVMe SQ 활성화 타이밍

	/*
	 * Flag set to activate the sampling of the total service time
	 * of a just-arrived first I/O request (see
	 * bfq_update_inject_limit()). This will cause the setting of
	 * waited_rq when the request is finally dispatched.
	 */
	bool wait_dispatch; // 첫 dispatch 샘플링 플래그; NVMe SQ injection 한계 측정 트리거
	/*
	 *  If set, then bfq_update_inject_limit() is invoked when
	 *  waited_rq is eventually completed.
	 */
	struct request *waited_rq; // 샘플링 중인 request; NVMe CQ 완료 후 inject_limit 갱신
	/*
	 * True if some request has been injected during the last service hole.
	 */
	bool rqs_injected; // 마지막 service hole 동안 injection 여부; NVMe SQ 활용도
	/* time of first rq dispatch in current observation interval (ns) */
	u64 first_dispatch; // 관찰 구간 첫 dispatch 시각; NVMe throughput 측정
	/* time of last rq dispatch in current observation interval (ns) */
	u64 last_dispatch; // 관찰 구간 마지막 dispatch 시각; NVMe throughput 측정
	/* beginning of the last budget */
	ktime_t last_budget_start; // 마지막 예산 시작 시각; NVMe SQ 서비스 슬라이스 측정
	/* beginning of the last idle slice */
	ktime_t last_idling_start; // 마지막 idle 시작(ktime); NVMe doorbell batching 간격
	unsigned long last_idling_start_jiffies; // 마지막 idle 시작(jiffies); NVMe CQ 완료 기반 타이머
	/* number of samples in current observation interval */
	int peak_rate_samples; // peak rate 샘플 수; NVMe sequential throughput 추정 신뢰도
	/* num of samples of seq dispatches in current observation interval */
	u32 sequential_samples; // sequential 샘플 수; NVMe PRP/SGL 최적화 판단
	/* total num of sectors transferred in current observation interval */
	u64 tot_sectors_dispatched; // dispatch sector 누적; NVMe throughput 추정
	/* max rq size seen during current observation interval (sectors) */
	u32 last_rq_max_size; // 현재 관찰 구간 최대 rq 크기; NVMe PRP/SGL segment 한계 추정
	/* time elapsed from first dispatch in current observ. interval (us) */
	u64 delta_from_first; // 관찰 구간 경과(us); NVMe throughput 계산
	/*
	 * Current estimate of the device peak rate, measured in
	 * [(sectors/usec) / 2^BFQ_RATE_SHIFT]. The left-shift by
	 * BFQ_RATE_SHIFT is performed to increase precision in
	 * fixed-point calculations.
	 */
	u32 peak_rate; // 추정 피크 throughput; NVMe SQ 예산 크기 및 idle 정책 조정
	/* maximum budget allotted to a bfq_queue before rescheduling */
	int bfq_max_budget; // 최대 예산; NVMe SQ에서 한 queue가 독점하지 않도록 제한

	/*
	 * List of all the bfq_queues active for a specific actuator
	 * on the device. Keeping active queues separate on a
	 * per-actuator basis helps implementing per-actuator
	 * injection more efficiently.
	 */
	struct list_head active_list[BFQ_MAX_ACTUATORS]; // per-actuator active queue 리스트; NVMe SQ injection 순회용
	/* list of all the bfq_queues idle on the device */
	struct list_head idle_list; // idle queue 리스트; NVMe CQ 후 재활성화 후보

	/*
	 * Timeout for async/sync requests; when it fires, requests
	 * are served in fifo order.
	 */
	u64 bfq_fifo_expire[2]; // async/sync FIFO 만료 시간; NVMe timeout/abort 기준
	/* weight of backward seeks wrt forward ones */
	unsigned int bfq_back_penalty; // backward seek 패널티; NVMe random access 비용 반영
	/* maximum allowed backward seek */
	unsigned int bfq_back_max; // 최대 backward seek; NVMe PRP/SGL 비순차 페널티
	/* maximum idling time */
	u32 bfq_slice_idle; // 최대 idling 시간; NVMe doorbell batching vs latency 트레이드오프
	/* user-configured max budget value (0 for auto-tuning) */
	int bfq_user_max_budget; // 사용자 설정 최대 예산; NVMe SQ 수동 depth 제한
	/*
	 * Timeout for bfq_queues to consume their budget; used to
	 * prevent seeky queues from imposing long latencies to
	 * sequential or quasi-sequential ones (this also implies that
	 * seeky queues cannot receive guarantees in the service
	 * domain; after a timeout they are charged for the time they
	 * have been in service, to preserve fairness among them, but
	 * without service-domain guarantees).
	 */
	unsigned int bfq_timeout; // bfq_queue 예산 소비 timeout; NVMe SQ latency 보장

	/*
	 * Force device idling whenever needed to provide accurate
	 * service guarantees, without caring about throughput
	 * issues. CAVEAT: this may even increase latencies, in case
	 * of useless idling for processes that did stop doing I/O.
	 */
	bool strict_guarantees; // 강제 idling; NVMe latency 정확성을 우선, throughput 희생 가능
	/*
	 * Last time at which a queue entered the current burst of
	 * queues being activated shortly after each other; for more
	 * details about this and the following parameters related to
	 * a burst of activations, see the comments on the function
	 * bfq_handle_burst.
	 */
	unsigned long last_ins_in_burst; // 버스트 마지막 삽입 시각; NVMe SQ activation 폭조 감지
	/*
	 * Reference time interval used to decide whether a queue has
	 * been activated shortly after @last_ins_in_burst.
	 */
	unsigned long bfq_burst_interval; // 버스트 간격; NVMe SQ 폭주 판단 기준
	/* number of queues in the current burst of queue activations */
	int burst_size; // 현재 버스트 크기; NVMe SQ 폭주 심각도
	/* common parent entity for the queues in the burst */
	struct bfq_entity *burst_parent_entity; // 버스트 공통 부모 entity; NVMe SQ cgroup 자원 공유 기준
	/* Maximum burst size above which the current queue-activation
	 * burst is deemed as 'large'.
	 */
	unsigned long bfq_large_burst_thresh; // 대형 버스트 임계; NVMe SQ anti-burst 정책 기준
	/* true if a large queue-activation burst is in progress */
	bool large_burst; // 대형 버스트 진행 여부; NVMe SQ scheduler 폭주 대응 상태
	/*
	 * Head of the burst list (as for the above fields, more
	 * details in the comments on the function bfq_handle_burst).
	 */
	struct hlist_head burst_list; // 버스트 리스트 헤드; NVMe SQ queue lifetime 및 폭주 관리
	/* if set to true, low-latency heuristics are enabled */
	bool low_latency; // low-latency 휴리스틱 활성화; NVMe 대화형 latency 최적화
	/*
	 * Maximum factor by which the weight of a weight-raised queue
	 * is multiplied.
	 */
	unsigned int bfq_wr_coeff; // WR 최대 계수; NVMe latency-sensitive SQ 우선도 상한
	/* Maximum weight-raising duration for soft real-time processes */
	unsigned int bfq_wr_rt_max_time; // WR RT 최대 시간; NVMe soft-RT QoS 기간
	/*
	 * Minimum idle period after which weight-raising may be
	 * reactivated for a queue (in jiffies).
	 */
	unsigned int bfq_wr_min_idle_time; // WR 최소 idle 시간; NVMe SQ WR 재활성 기준
	/*
	 * Minimum period between request arrivals after which
	 * weight-raising may be reactivated for an already busy async
	 * queue (in jiffies).
	 */
	unsigned long bfq_wr_min_inter_arr_async; // async 최소 도착 간격; NVMe SQ async latency 정책
	/* Max service-rate for a soft real-time queue, in sectors/sec */
	unsigned int bfq_wr_max_softrt_rate; // soft RT 최대 서비스율; NVMe SQ 대역폭 한계
	/*
	 * Cached value of the product ref_rate*ref_wr_duration, used
	 * for computing the maximum duration of weight raising
	 * automatically.
	 */
	u64 rate_dur_prod; // ref_rate * ref_wr_duration; NVMe WR duration 자동 계산 캐시
	/* fallback dummy bfqq for extreme OOM conditions */
	struct bfq_queue oom_bfqq; // OOM fallback bfq_queue; NVMe SQ 메모리 부족 시 emergency 경로
	spinlock_t lock; // bfq_data 스핀락; NVMe completion 순서 및 doorbell 가시성 보호

	/*
	 * bic associated with the task issuing current bio for
	 * merging. This and the next field are used as a support to
	 * be able to perform the bic lookup, needed by bio-merge
	 * functions, before the scheduler lock is taken, and thus
	 * avoid taking the request-queue lock while the scheduler
	 * lock is being held.
	 */
	struct bfq_io_cq *bio_bic; // bio merge용 bic 캐시; NVMe SQ merge 선별 최적화
	/* bfqq associated with the task issuing current bio for merging */
	struct bfq_queue *bio_bfqq; // bio merge용 bfqq 캐시; NVMe SQ merge 후보 예비
	/*
	 * Depth limits used in bfq_limit_depth (see comments on the
	 * function)
	 *
	 * NVMe 관점: async_depths[2][2]는 blk_mq_get_request() 호출 시
	 * NVMe SQ의 가용 슬롯 수를 제한하여, SQ overflow 또는 doorbell
	 * stall을 방지한다. 구체적 한계값은 bfq_limit_depth()에서
	 * 계산되며, 이는 blk_mq_run_hw_queue -> ... -> nvme_queue_rq
	 * 사이에서 적용된다.
	 */
	unsigned int async_depths[2][2]; // async depth limits[sync/async][unknown/known]; blk_mq_get_request -> tag 할당 제한 (NVMe SQ overflow 방지)

	/*
	 * Number of independent actuators. This is equal to 1 in
	 * case of single-actuator drives.
	 *
	 * NVMe 관점: NVMe 컨트롤러가 Independent Access Ranges를
	 * 보고하면 이 값이 1보다 커진다. 각 actuator는 별도의 NVMe queue
	 * 쌍 또는 LBA 범위를 담당할 수 있다(추정).
	 */
	unsigned int num_actuators; // 독립 actuator 수; NVMe Independent Access Ranges/queue 쌍 수 (추정)
	/*
	 * Disk independent access ranges for each actuator
	 * in this device.
	 */
	sector_t sector[BFQ_MAX_ACTUATORS]; // actuator 시작 LBA; NVMe SQ LBA 라우팅
	sector_t nr_sectors[BFQ_MAX_ACTUATORS]; // actuator 범위 크기; NVMe SQ 범위 기반 라우팅
	struct blk_independent_access_range ia_ranges[BFQ_MAX_ACTUATORS]; // Independent Access Range; NVMe queue 쌍 매핑 정보 (추정)

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
	 * NVMe 관점: actuator_load_threshold는 NVMe SQ의 실제 깊이와
	 * 다를 수 있으며, NCQ 32 슬롯을 경험적으로 가정한 값이다. NVMe
	 * SQ 깊이가 64 또는 그 이상인 현대 SSD에서는 이 threshold가
	 * underutilization 판단을 다소 보수적으로 만들 수 있다(추정).
	 */
	unsigned int actuator_load_threshold; // underutilized actuator 임계; NVMe SQ depth 기반 request injection (추정)
};

/*
 * bfq_queue 상태 플래그. NVMe 관점에서 각 플래그는 SQ/CQ 흐름 제어에
 * 직접 영향을 준다.
 */
enum bfqq_state_flags {
	BFQQF_just_created = 0,	/* queue just allocated */ // queue 방금 할당; NVMe SQ 초기화 상태
	BFQQF_busy,		/* has requests or is in service */ // request 보유 또는 서비스 중; NVMe SQ 활성 후보
	BFQQF_wait_request,	/* waiting for a request */ // request 도착 대기; NVMe SQ doorbell 지연 상태
	BFQQF_non_blocking_wait_rq, /*
				     * waiting for a request
				     * without idling the device
				     */	// idling 없이 request 대기; NVMe SQ busy polling 유사 경로
	BFQQF_fifo_expire,	/* FIFO checked in this slice */ // FIFO 만료 검사 수행; NVMe timeout/abort 준비
	BFQQF_has_short_ttime,	/* queue has a short think time */ // 짧은 think time; NVMe CQ 후속 doorbell이 밀접함
	BFQQF_sync,		/* synchronous queue */ // sync queue; NVMe SQ sync I/O 경로
	BFQQF_IO_bound,		/*
				 * bfqq has timed-out at least once
				 * having consumed at most 2/10 of
				 * its budget
				 */	// budget 내 timeout 이력; NVMe SQ timeout/abort 패턴
	BFQQF_in_large_burst,	/*
				 * bfqq activated in a large burst,
				 * see comments to bfq_handle_burst.
				 */	// large burst 활성화; NVMe SQ activation 폭주
	BFQQF_softrt_update,	/*
				 * may need softrt-next-start
				 * update
				 */	// soft RT 갱신 필요; NVMe latency 정책 갱신
	BFQQF_coop,		/* bfqq is shared */ // shared queue; NVMe SQ 경로 병합 상태
	BFQQF_split_coop,	/* shared bfqq will be split */ // split 예정; NVMe SQ 경로 분리 예고
};
#define BFQ_BFQQ_FNS(name)						\
void bfq_mark_bfqq_##name(struct bfq_queue *bfqq);			\
void bfq_clear_bfqq_##name(struct bfq_queue *bfqq);			\
int bfq_bfqq_##name(const struct bfq_queue *bfqq);
BFQ_BFQQ_FNS(just_created); // BFQQF_just_created get/set; NVMe SQ 초기화 상태 전환
BFQ_BFQQ_FNS(busy); // BFQQF_busy get/set; NVMe SQ 활성/비활성화
BFQ_BFQQ_FNS(wait_request); // BFQQF_wait_request get/set; NVMe SQ doorbell 지연 상태
BFQ_BFQQ_FNS(non_blocking_wait_rq); // BFQQF_non_blocking_wait_rq get/set; NVMe SQ busy-polling 유사 경로
BFQ_BFQQ_FNS(fifo_expire); // BFQQF_fifo_expire get/set; NVMe timeout/abort 준비 플래그
BFQ_BFQQ_FNS(has_short_ttime); // BFQQF_has_short_ttime get/set; NVMe CQ 후속 doorbell 밀접성
BFQ_BFQQ_FNS(sync); // BFQQF_sync get/set; NVMe SQ sync/async 경로 구분
BFQ_BFQQ_FNS(IO_bound); // BFQQF_IO_bound get/set; NVMe SQ timeout/abort 이력 플래그
BFQ_BFQQ_FNS(in_large_burst); // BFQQF_in_large_burst get/set; NVMe SQ activation 폭주 플래그
BFQ_BFQQ_FNS(coop); // BFQQF_coop get/set; NVMe SQ 경로 병합 상태
BFQ_BFQQ_FNS(split_coop); // BFQQF_split_coop get/set; NVMe SQ 경로 분리 예고
BFQ_BFQQ_FNS(softrt_update); // BFQQF_softrt_update get/set; NVMe latency 정책 갱신 필요
#undef BFQ_BFQQ_FNS

/* Expiration reasons. */
/*
 * bfq_queue가 서비스 중 만료되는 이유. NVMe CQ 완료 시나리오와
 * 연결하여 볼 수 있다.
 */
enum bfqq_expiration {
	BFQQE_TOO_IDLE = 0,		/*
					 * queue has been idling for
					 * too long
					 */	// 과도한 idle로 만료; NVMe SQ doorbell이 너무 늦게 트리거됨
	BFQQE_BUDGET_TIMEOUT,	/* budget took too long to be used */ // 예산 소비 시간 초과; NVMe SQ latency 보장을 위한 강제 교체
	BFQQE_BUDGET_EXHAUSTED,	/* budget consumed */ // 예산 소진; NVMe SQ fairness를 위한 정상 교체
	BFQQE_NO_MORE_REQUESTS,	/* the queue has no more requests */ // 대기 요청 없음; NVMe SQ에서 해당 queue 제거
	BFQQE_PREEMPTED		/* preemption in progress */
};
struct bfq_stat {
	struct percpu_counter		cpu_cnt; // percpu request 카운터; NVMe SQ throughput 측정
	atomic64_t			aux_cnt; // 보조 원자 카운터; NVMe completion 통계
};

struct bfqg_stats {
	/* basic stats */
	struct blkg_rwstat		bytes; // cgroup별 바이트; NVMe 대역폭 할당 추적
	struct blkg_rwstat		ios; // cgroup별 IO 수; NVMe SQ IOPS
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* number of ios merged */
	struct blkg_rwstat		merged; // merge 횟수; NVMe SQ command 수 감소 효과
	/* total time spent on device in ns, may not be accurate w/ queueing */
	struct blkg_rwstat		service_time; // 디바이스 서비스 시간; NVMe CQ latency
	/* total time spent waiting in scheduler queue in ns */
	struct blkg_rwstat		wait_time; // 스케줄러 대기 시간; NVMe SQ 대기 latency
	/* number of IOs queued up */
	struct blkg_rwstat		queued; // 큐에 대기 중인 IO; NVMe SQ backlog
	/* total disk time and nr sectors dispatched by this group */
	struct bfq_stat		time; // 총 디스크 시간; NVMe SQ 서비스 시간 누적
	/* sum of number of ios queued across all samples */
	struct bfq_stat		avg_queue_size_sum; // 평균 큐 크기 합; NVMe SQ 평균 깊이
	/* count of samples taken for average */
	struct bfq_stat		avg_queue_size_samples; // 평균 큐 크기 샘플; NVMe SQ 깊이 통계
	/* how many times this group has been removed from service tree */
	struct bfq_stat		dequeue; // 서비스 트리 제거 횟수; NVMe SQ 스케줄링 이벤트
	/* total time spent waiting for it to be assigned a timeslice. */
	struct bfq_stat		group_wait_time; // 그룹 대기 시간; NVMe SQ cgroup latency
	/* time spent idling for this blkcg_gq */
	struct bfq_stat		idle_time; // idle 시간; NVMe doorbell batching 지연
	/* total time with empty current active q with other requests queued */
	struct bfq_stat		empty_time; // empty 시간; NVMe SQ 활용도
	/* fields after this shouldn't be cleared on stat reset */
	u64				start_group_wait_time; // 그룹 대기 시작; NVMe SQ latency 측정
	u64				start_idle_time; // idle 시작; NVMe SQ idle 측정
	u64				start_empty_time; // empty 시작; NVMe SQ idle 측정
	uint16_t			flags;
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
	struct blkcg_policy_data pd; // blkcg/blkg policy data; NVMe SQ cgroup 식별

	unsigned int weight; // 카운터가 대표하는 weight; 동일 가중치 NVMe queue 그룹 식별
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
 * 상위 그룹의 B-WF2Q+ 트리에 삽입되어 NVMe SQ 자원을 cgroup 계층에
 * 따라 분배받는다. rq_pos_tree는 인접한 LBA를 가진 bfq_queue를 찾아
 * NVMe SQ에서 sequential하게 배치할 수 있도록 merge/cooperation
 * candidate를 제공한다.
 */
struct bfq_group {
	/* must be the first member */
	struct blkg_policy_data pd; // blkcg/blkg policy data; NVMe SQ cgroup 식별
	/* reference counter (see comments in bfq_bic_update_cgroup) */
	refcount_t ref; // 참조 카운트; NVMe request 생명주기 동안 bfq_queue 유지
	struct bfq_entity entity; // B-WF2Q+ entity; NVMe SQ cgroup/queue 우선순위
	struct bfq_sched_data sched_data; // 소속 상위 스케줄러; NVMe SQ dispatch 정책의 컨텍스트

	struct bfq_data *bfqd; // 상위 bfq_data; NVMe 장치/SQ 컨텍스트 역참조
	struct bfq_queue *async_bfqq[2][IOPRIO_NR_LEVELS][BFQ_MAX_ACTUATORS]; // async bfq_queue 매트릭스; NVMe SQ async 경로
	struct bfq_queue *async_idle_bfqq[BFQ_MAX_ACTUATORS]; // idle async queue; NVMe SQ idle async 경로

	struct bfq_entity *my_entity; // 자신의 entity 포인터; NVMe SQ cgroup 트리 순회 보조
	int active_entities; // 활성 entity 수; NVMe SQ cgroup 병렬도
	int num_queues_with_pending_reqs; // pending rq queue 수; NVMe SQ cgroup in-flight

	struct rb_root rq_pos_tree; // LBA 위치 트리; NVMe SQ sequential merge/cooperation

	struct bfqg_stats stats; // 통계; NVMe SQ cgroup 모니터링
};

#else
struct bfq_group {
	struct bfq_entity entity; // B-WF2Q+ entity; NVMe SQ cgroup/queue 우선순위
	struct bfq_sched_data sched_data; // 소속 상위 스케줄러; NVMe SQ dispatch 정책의 컨텍스트
	struct bfq_queue *async_bfqq[2][IOPRIO_NR_LEVELS][BFQ_MAX_ACTUATORS]; // async bfq_queue 매트릭스; NVMe SQ async 경로
	struct bfq_queue *async_idle_bfqq[BFQ_MAX_ACTUATORS]; // idle async queue; NVMe SQ idle async 경로
	struct rb_root rq_pos_tree; // LBA 위치 트리; NVMe SQ sequential merge/cooperation
};
#endif

/* --------------- main algorithm interface ----------------- */
#define BFQ_SERVICE_TREE_INIT	((struct bfq_service_tree)		\
				{ RB_ROOT, RB_ROOT, NULL, NULL, 0, 0 }) // service tree 초기값; NVMe SQ 후보 트리의 초기 상태
extern const int bfq_timeout; // bfq_queue 예산 소비 timeout; NVMe SQ latency 보장
/*
 * bfq_io_cq -> bfq_queue 변환 및 설정. NVMe 관점에서는 bio가
 * 도착했을 때 어느 actuator/SQ 경로로 볼 것인지 결정하는 첫 단계이다.
 * 호출 경로: blk_mq_submit_bio -> blk_mq_get_request ->
 *          bfq_init_rq -> bic_to_bfqq (추정)
 */
struct bfq_queue *bic_to_bfqq(struct bfq_io_cq *bic, bool is_sync,
				unsigned int actuator_idx); // bic -> bfq_queue 변환; NVMe actuator/SQ 경로 선택의 첫 단계
void bic_set_bfqq(struct bfq_io_cq *bic, struct bfq_queue *bfqq, bool is_sync,
				unsigned int actuator_idx); // bfq_queue 설정; NVMe SQ 경로를 task에 연결
/* bfq_io_cq로부터 bfq_data 획득. NVMe 장치 전역 lock의 소유자 확인용 */
struct bfq_data *bic_to_bfqd(struct bfq_io_cq *bic); // bfq_data 획득; NVMe 장치 전역 lock의 컨텍스트 확인
/*
 * request 위치 트리에 bfq_queue 추가/이동. NVMe에서 LBA 인접 queue
 * 탐색(bfq_find_close_cooperator)을 위해 사용된다.
 */
void bfq_pos_tree_add_move(struct bfq_data *bfqd, struct bfq_queue *bfqq); // request 위치 트리 갱신; NVMe sequential SQ 준비
/*
 * 가중치 트리 추가/제거. NVMe fairness 및 bfq_limit_depth의 depth
 * 제한 정책에 영향을 준다.
 */
void bfq_weights_tree_add(struct bfq_queue *bfqq); // weight 카운터 추가; NVMe fairness 및 depth limit 정책
void bfq_weights_tree_remove(struct bfq_queue *bfqq); // weight 카운터 제거; NVMe depth limit 갱신
/*
 * 현재 서비스 중인 bfq_queue를 만료시킨다. NVMe CQ 완료 후나 budget
 * 소진 시 호출되며, 이후 bfq_get_next_queue()가 다음 SQ 후보를
 * 선정한다. 호출 경로: nvme_irq -> nvme_complete_rq ->
 *          blk_mq_complete_request -> bfq_completed_request ->
 *          bfq_bfqq_expire (추정)
 */
void bfq_bfqq_expire(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		     bool compensate, enum bfqq_expiration reason); // 현재 서비스 queue 만료; NVMe CQ 완료/budget 소진 시 호출
void bfq_put_queue(struct bfq_queue *bfqq); // bfq_queue 참조 해제; NVMe SQ 경로 제거
void bfq_put_cooperator(struct bfq_queue *bfqq); // cooperator 참조 해제; NVMe SQ 병합 해제
void bfq_end_wr_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg); // async WR 종료; NVMe SQ async 우선순위 복귀
void bfq_release_process_ref(struct bfq_data *bfqd, struct bfq_queue *bfqq); // 프로세스 참조 해제; NVMe SQ task detach
/*
 * 디스패치 작업을 예약. NVMe SQ로 request를 복귀시킬 타이밍을
 * 조정한다. 호출 경로: blk_mq_run_hw_queue -> ... -> bfq_schedule_dispatch
 */
void bfq_schedule_dispatch(struct bfq_data *bfqd); // dispatch 예약; NVMe SQ doorbell 타이밍 조정
void bfq_put_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg); // async queue 해제; NVMe SQ async 자원 회수
/* ------------ end of main algorithm interface -------------- */

/* ---------------- cgroups-support interface ---------------- */
void bfqg_stats_update_legacy_io(struct request_queue *q, struct request *rq); // legacy IO 통계 갱신; NVMe SQ cgroup IO 기록
void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf); // IO 제거 통계; NVMe SQ abort/requeue 반영
void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf); // merge 통계; NVMe SQ command 수 감소 기록
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf); // 완료 통계; NVMe CQ latency 기록
void bfqg_stats_update_dequeue(struct bfq_group *bfqg); // dequeue 통계; NVMe SQ 스케줄링 이벤트
void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg); // idle 시작 기록; NVMe SQ idle 측정
void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		   struct bfq_group *bfqg); // bfq_queue cgroup 이동; NVMe SQ cgroup 재분류

#ifdef CONFIG_BFQ_CGROUP_DEBUG
void bfqg_stats_update_io_add(struct bfq_group *bfqg, struct bfq_queue *bfqq,
			      blk_opf_t opf); // IO 추가 통계(debug); NVMe SQ backlog 기록
void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg); // empty 시작 기록(debug); NVMe SQ idle 측정
void bfqg_stats_update_idle_time(struct bfq_group *bfqg); // idle 시간 갱신(debug); NVMe SQ idle 통계
void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg); // 평균 큐 크기 갱신(debug); NVMe SQ 깊이 통계
#endif
void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg); // entity 초기화; NVMe SQ 우선순위 트리 등록
void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio); // blkcg 갱신; NVMe SQ cgroup migration
void bfq_end_wr_async(struct bfq_data *bfqd); // async WR 종료; NVMe SQ async latency 정책
struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio); // bio에서 bfq_group 추출; NVMe SQ cgroup 선택
struct blkcg_gq *bfqg_to_blkg(struct bfq_group *bfqg); // bfq_group -> blkcg_gq; NVMe SQ cgroup 추적
struct bfq_group *bfqq_group(struct bfq_queue *bfqq); // bfq_queue의 그룹; NVMe SQ cgroup 분류
struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node); // 그룹 계층 생성; NVMe SQ cgroup 트리 구축
void bfqg_and_blkg_put(struct bfq_group *bfqg); // 그룹 참조 해제; NVMe SQ cgroup 자원 회수
#ifdef CONFIG_BFQ_GROUP_IOSCHED
extern struct cftype bfq_blkcg_legacy_files[]; // cgroup legacy control files; NVMe SQ cgroup 정책 노출
extern struct cftype bfq_blkg_files[]; // cgroup blkg control files; NVMe SQ cgroup-장치 정책 노출
extern struct blkcg_policy blkcg_policy_bfq; // blkcg policy 등록; NVMe SQ cgroup 프레임워크 연결
#endif

/* ------------- end of cgroups-support interface ------------- */
/* - interface of the internal hierarchical B-WF2Q+ scheduler - */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
/* both next loops stop at one of the child entities of the root group */
#define for_each_entity(entity)	\
	for (; entity ; entity = entity->parent) // cgroup 계층 상향 순회; NVMe SQ 상위 그룹 자원 집계
/*
 * For each iteration, compute parent in advance, so as to be safe if
 * entity is deallocated during the iteration. Such a deallocation may
 * happen as a consequence of a bfq_put_queue that frees the bfq_queue
 * containing entity.
 */
#define for_each_entity_safe(entity, parent) \
	for (; entity && ({ parent = entity->parent; 1; }); entity = parent)      // 안전한 상향 순회; NVMe SQ entity 해제 시에도 안전(RCU/lock 고려)

#else /* CONFIG_BFQ_GROUP_IOSCHED */
/*
 * Next two macros are fake loops when cgroups support is not
 * enabled. I fact, in such a case, there is only one level to go up
 * (to reach the root group).
 */
#define for_each_entity(entity)	\
	for (; entity ; entity = NULL) // cgroup 계층 상향 순회; NVMe SQ 상위 그룹 자원 집계
#define for_each_entity_safe(entity, parent) \
	for (parent = NULL; entity ; entity = parent) // 안전한 상향 순회; NVMe SQ entity 해제 시에도 안전(RCU/lock 고려)
#endif /* CONFIG_BFQ_GROUP_IOSCHED */

/*
 * B-WF2Q+ 스케줄러 낸부 함수들. NVMe 관점에서 이 함수들은 SQ로
 * 디스패치될 request의 우선순위와 타이밍을 결정한다.
 *
 * bfq_get_next_queue: 다음에 서비스할 bfq_queue 선택. 선택된 queue의
	 *     head request가 이후 blk_mq_dispatch_rq_list를 통해 NVMe SQ로
 *     전달된다.
 * bfq_dispatch_requests -> __bfq_dispatch_request -> bfq_get_next_queue
 *     (추정)
 */
struct bfq_queue *bfq_entity_to_bfqq(struct bfq_entity *entity); // entity -> bfq_queue 변환; NVMe SQ leaf dispatch 경로
unsigned int bfq_tot_busy_queues(struct bfq_data *bfqd); // 총 busy queue 수; NVMe SQ 병렬도
struct bfq_service_tree *bfq_entity_service_tree(struct bfq_entity *entity); // entity의 service tree; NVMe SQ 클래스(RT/BE/IDLE) 선택
struct bfq_entity *bfq_entity_of(struct rb_node *node); // rb_node -> entity; NVMe SQ 트리 순회
unsigned short bfq_ioprio_to_weight(int ioprio); // ioprio -> weight 변환; NVMe SQ 우선순위 수치화
void bfq_put_idle_entity(struct bfq_service_tree *st,
			 struct bfq_entity *entity); // idle entity 제거; NVMe SQ idle 후보 갱신
struct bfq_service_tree *
__bfq_entity_update_weight_prio(struct bfq_service_tree *old_st,
				struct bfq_entity *entity,
				bool update_class_too); // weight/prio 갱신; NVMe SQ 후보 트리 재정렬
void bfq_bfqq_served(struct bfq_queue *bfqq, int served); // 서비스량 기록; NVMe fairness 과금
void bfq_bfqq_charge_time(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  unsigned long time_ms); // 시간 과금; NVMe SQ latency 보장
bool __bfq_deactivate_entity(struct bfq_entity *entity,
			     bool ins_into_idle_tree); // entity 비활성화; NVMe SQ 후보 제거
bool next_queue_may_preempt(struct bfq_data *bfqd); // 선점 가능 여부; NVMe SQ 우선순위 전환 판단
struct bfq_queue *bfq_get_next_queue(struct bfq_data *bfqd); // 다음 queue 선택; NVMe SQ doorbell 후보 선정
bool __bfq_bfqd_reset_in_service(struct bfq_data *bfqd); // in-service 상태 리셋; NVMe SQ 완료 후 정리
void bfq_deactivate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			 bool ins_into_idle_tree, bool expiration); // bfq_queue 비활성화; NVMe SQ 후보 제거
void bfq_activate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq); // bfq_queue 활성화; NVMe SQ 후보 추가
void bfq_requeue_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		      bool expiration); // bfq_queue 재enqueue; NVMe SQ requeue
void bfq_del_bfqq_busy(struct bfq_queue *bfqq, bool expiration); // busy 리스트 제거; NVMe SQ 병렬도 감소
void bfq_add_bfqq_busy(struct bfq_queue *bfqq); // busy 리스트 추가; NVMe SQ 병렬도 증가
void bfq_add_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq); // pending rq 그룹 추가; NVMe in-flight cgroup 추적
void bfq_del_bfqq_in_groups_with_pending_reqs(struct bfq_queue *bfqq); // pending rq 그룹 제거; NVMe in-flight cgroup 추적
void bfq_reassign_last_bfqq(struct bfq_queue *cur_bfqq,
			    struct bfq_queue *new_bfqq); // 마지막 bfqq 재할당; NVMe SQ merge/split 후 복원

/* --------------- end of interface of B-WF2Q+ ---------------- */

/* Logging facilities. */
static inline void bfq_bfqq_name(struct bfq_queue *bfqq, char *str, int len)
{
	char type = bfq_bfqq_sync(bfqq) ? 'S' : 'A'; // sync(S)/async(A) 타입; NVMe SQ 경로 식별
	if (bfqq->pid != -1) // 개별 queue 로깅; NVMe SQ PID 기반 trace 식별
		snprintf(str, len, "bfq%d%c", bfqq->pid, type); // PID 기반 이름; NVMe blktrace 메시지
	else
		snprintf(str, len, "bfqSHARED-%c", type); // 공유 queue 이름; NVMe blktrace 메시지
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
struct bfq_group *bfqq_group(struct bfq_queue *bfqq);     // bfq_queue의 그룹; NVMe SQ cgroup 분류
#define bfq_log_bfqq(bfqd, bfqq, fmt, args...)	do {			\
	char pid_str[MAX_BFQQ_NAME_LENGTH];				\
	if (likely(!blk_trace_note_message_enabled((bfqd)->queue)))	\
		break;							\
	bfq_bfqq_name((bfqq), pid_str, MAX_BFQQ_NAME_LENGTH);		\
	blk_add_cgroup_trace_msg((bfqd)->queue,				\
			&bfqg_to_blkg(bfqq_group(bfqq))->blkcg->css,	\
			"%s " fmt, pid_str, ##args);			\
} while (0) // NVMe SQ 이벤트를 blktrace에 기록하는 로깅 매크로
#else /* CONFIG_BFQ_GROUP_IOSCHED */
#define bfq_log_bfqq(bfqd, bfqq, fmt, args...) do {	\
	char pid_str[MAX_BFQQ_NAME_LENGTH];				\
	if (likely(!blk_trace_note_message_enabled((bfqd)->queue)))	\
		break;							\
	bfq_bfqq_name((bfqq), pid_str, MAX_BFQQ_NAME_LENGTH);		\
	blk_add_trace_msg((bfqd)->queue, "%s " fmt, pid_str, ##args);	\
} while (0) // NVMe SQ 이벤트를 blktrace에 기록하는 로깅 매크로

#endif /* CONFIG_BFQ_GROUP_IOSCHED */
#define bfq_log(bfqd, fmt, args...) \
	blk_add_trace_msg((bfqd)->queue, "bfq " fmt, ##args) // 일반 BFQ 로깅 매크로; NVMe SQ 이벤트를 blktrace에 기록

/*
 * =====================================================================
 * NVMe 관점 핵심 요약
 * =====================================================================
 * - bfq_queue는 하나의 NVMe actuator(또는 독립 접근 범위)에 대응하며,
 *   sort_list의 LBA 정렬은 NVMe PRP/SGL 준비와 sequential throughput에
 *   유리하다.
 * - bfq_data는 NVMe SQ로 전달되기 전의 request 풀(dispatch)과 각
 *   actuator별 in-flight 개수(rq_in_driver[])를 관리하여 SQ 슬롯 고갈을
 *   방지한다.
 * - B-WF2Q+ 스케줄러는 bfq_entity의 가상 시간(start/finish)으로 NVMe
 *   doorbell이 트리거될 queue의 우선순위를 결정하며, 이는
 *   blk_mq_dispatch_rq_list -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   로 이어진다.
 * - request injection(inject_limit, actuator_load_threshold)은 NVMe SQ
 *   깊이가 충분할 때 underutilized actuator의 throughput을 끌어올리지만,
 *   과도하면 in-service queue의 latency를 저해할 수 있다(추정).
 * - 이 파일은 block/bfq-iosched.c, block/bfq-wf2q.c 등과 논리적으로
 *   연결되며, 특히 block/blk-mq.c의 blk_mq_run_hw_queue 및
 *   drivers/nvme/host/pci.c의 nvme_queue_rq/nvme_submit_cmd와 상하위
 *   호출 관계를 가진다.
 * =====================================================================
 */

#endif /* _BFQ_H */
