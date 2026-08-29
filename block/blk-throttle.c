// SPDX-License-Identifier: GPL-2.0
/*
 * Interface for controlling IO bandwidth on a request queue
 *
 * Copyright (C) 2010 Vivek Goyal <vgoyal@redhat.com>
 */

/*
 * [한국어] blk-throttle: cgroup 기반 IO 대역폭/IOPS 조율 계층 (blk-throttle.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 커널의 cgroup 기반 블록 IO 쓰로틀링(throttling) 계층을
 * 구현한다. 응용 프로그램의 bio가 블록 드라이버(blk-mq)에
 * 도달하기 전, bps(bytes per second)와 iops(IO per second) 제한을 적용해
 * cgroup별 대역폭/초당 명령 수를 소프트웨어적으로 제어한다.
 * 토큰 버킷(Token Bucket) 알고리즘을 100ms 슬라이스 윈도우 단위로 구현하며,
 * 제한을 초과한 bio는 throtl_grp 큐에 보관 후 pending_timer가 disptime에
 * 도달하면 상위 service_queue로 전달한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   submit_bio()
 *     → blk_mq_submit_bio()
 *       → __blk_throtl_bio()          ← 이 파일의 핵심 진입점
 *         [제한 초과 시: throtl 큐에 보관]
 *         → throtl_pending_timer_fn() [disptime 도달 시]
 *           → throtl_select_dispatch() → tg_dispatch_one_bio()
 *             → (최상위 service_queue 도달)
 *               → blk_throtl_dispatch_work_fn() [kthrotld workqueue]
 *                 → submit_bio_noacct_nocheck()
 *                   → blk_mq_submit_bio() → 드라이버 queue_rq()
 * 실행 컨텍스트: 소프트웨어 커널 컨텍스트 (프로세스 컨텍스트 + softirq +
 * kworker). queue_lock(spinlock)으로 동시성 제어.
 * 이 계층은 장치 종류와 무관하다 - bio를 붙잡아 두었다가 시간이 되면 놓아줄
 * 뿐이고, 그 아래가 NVMe인지 SCSI인지 loop인지는 전혀 참조하지 않는다.
 * 다만 고성능 장치일수록 장치 자체의 지연이 짧아, 여기서 붙잡는 100ms 단위
 * 슬라이스와 타이머 해상도(jiffies)가 상대적으로 큰 오차로 드러난다.
 *
 * === 타 모듈과의 연결 ===
 * - blk-cgroup (blk-cgroup.c, blkcg_gq, blkcg_policy): cgroup별 blkg를
 *   할당하고 blkcg_policy_throtl를 등록해 throtl_grp(pd)를 관리.
 * - blk-mq (blk-mq.c): __blk_throtl_bio()가 bio를 제어하며, throttle 통과
 *   후 submit_bio_noacct_nocheck()로 blk-mq에 재진입.
 * - 블록 드라이버 일반: blk-throttle은 bio를 request로 바꾸기 전 단계에서
 *   붙잡으므로, 스로틀에 걸린 bio는 아예 tag(request)를 할당받지 않는다.
 *   드라이버 큐에 부하를 주지 않는 대신, 대기 시간은 순전히 jiffies 타이머
 *   해상도에 의존한다.
 * - blk-throttle.h: throtl_grp, throtl_service_queue, throtl_qnode, 플래그
 *   정의 포함.
 * - cgroupfs io.max / blkio.throttle.*: 사용자 공간에서 bps/iops 상한 설정.
 * 데이터 흐름: bio → __blk_throtl_bio() → throtl_grp 큐 → pending_timer →
 * td->service_queue → kthrotld → blk-mq → 장치 큐.
 *
 * === 주요 함수/구조체 요약 ===
 * __blk_throtl_bio()         : 핵심 진입점. bio가 bps/iops 제한 내이면 통과,
 *                              초과 시 throtl 큐에 보관 후 true 반환.
 * tg_dispatch_time()         : bio가 현재 slice에서 기다려야 할 jiffies 계산.
 *                              bps → iops 순서로 검사.
 * tg_dispatch_one_bio()      : bio를 현재 tg 큐에서 꺼내 부모 service_queue로
 *                              이동. 최상위 도달 시 dispatch_work 트리거.
 * throtl_pending_timer_fn()  : pending_timer 핸들러. disptime 도래한 tg를
 *                              순회해 bio를 상위로 전파.
 * blk_throtl_dispatch_work_fn(): kthrotld에서 실행. 최상위 service_queue의
 *                              bio를 submit_bio_noacct_nocheck()로 blk-mq에 전달.
 * struct throtl_grp (tg)     : per-(cgroup, queue) 상태. bps[2]/iops[2] 제한,
 *                              bytes_disp/io_disp 사용량, slice_start/end 윈도우.
 * struct throtl_data (td)    : per-request_queue 컨트롤러. 최상위 service_queue
 *                              와 dispatch_work 포함.
 */

#include <linux/module.h>      /* [한국어] 커널 모듈 초기화/해제 (module_init) */
#include <linux/slab.h>        /* [한국어] kzalloc_node/kfree: NUMA-aware 메모리 할당 */
#include <linux/blkdev.h>      /* [한국어] request_queue, gendisk: 블록 장치 핵심 구조체 */
#include <linux/bio.h>         /* [한국어] struct bio, bio_list: IO 요청 단위 */
#include <linux/blktrace_api.h>/* [한국어] blk_add_trace_msg: blktrace 디버그 메시지 */
#include "blk.h"               /* [한국어] 블록 레이어 내부 API: submit_bio_noacct_nocheck 등 */
#include "blk-cgroup-rwstat.h" /* [한국어] blkg_rwstat: cgroup별 IO 통계 집계 */
#include "blk-throttle.h"      /* [한국어] throtl_grp/service_queue/qnode/플래그 정의 */

/* Max dispatch from a group in 1 round */
#define THROTL_GRP_QUANTUM 8    /* [한국어] 한 라운드에서 하나의 cgroup이 디스패치할 최대 bio 수; READ 6 + WRITE 2 비율로 분배 */

/* Total max dispatch from all groups in one round */
#define THROTL_QUANTUM 32       /* [한국어] 한 라운드(타이머 1회 실행)에서 모든 cgroup을 합쳐 상위로 올릴 수 있는 bio 수 상한.
                                 * 이 상한이 없으면 disptime이 한꺼번에 도래했을 때 throtl_select_dispatch()가
                                 * queue_lock을 잡은 채 수천 개 bio를 처리해 락 보유 시간이 길어진다.
                                 * 32에서 끊고 나머지는 다음 타이머 라운드로 미뤄 락 지연을 제한한다. */

/* Throttling is performed over a slice and after that slice is renewed */
#define DFL_THROTL_SLICE (HZ / 10) /* [한국어] 기본 슬라이스: 100ms(HZ/10). 이 윈도우 내 bps/iops 평균을 제한 */

/* A workqueue to queue throttle related work */
static struct workqueue_struct *kthrotld_workqueue; /* [한국어] throttle 통과 bio를 blk-mq로 전달하는 전용 워크큐; blk_throtl_dispatch_work_fn()이 여기서 실행됨 */

#define rb_entry_tg(node)	rb_entry((node), struct throtl_grp, rb_node) /* [한국어] rb_node → throtl_grp 변환 매크로; pending_tree 순회 시 사용 */

struct throtl_data
{
	/* service tree for active throtl groups */
	/* [한국어] service_queue 트리의 '뿌리'. cgroup 계층이 그대로 service_queue
	 * 트리로 사상되는데, 내부 노드는 throtl_grp에 내장된 sq이고 이 필드가
	 * 그 트리의 최상위(부모 없는) 노드다. bio는 자기 cgroup의 sq에서 출발해
	 * 부모 방향으로 한 단계씩 올라오며, 여기에 도달했다는 것은 경로상의 모든
	 * cgroup 제한을 통과했다는 뜻이므로 그때부터 실제 발행 대상이 된다. */
	struct throtl_service_queue service_queue;
	/* [한국어] 장치(request_queue) 단위 최상위 service_queue.
	 * 모든 하위 cgroup의 bio가 최종적으로 이 큐에 도달하며,
	 * blk_throtl_dispatch_work_fn()이 여기서 bio를 꺼내 blk-mq로 전달.
	 * 설정자: throtl_service_queue_init()이 blk_throtl_init()에서 초기화.
	 * 읽는 자: throtl_pending_timer_fn(), blk_throtl_dispatch_work_fn().
	 * 동기화: queue_lock(spinlock) 보호. */

	struct request_queue *queue;
	/* [한국어] 이 throttle 상태가 연결된 request_queue.
	 * gendisk 하나(파티션이 아니라 디스크 전체)에 대응하며, 스로틀의
	 * 모든 시간 계산과 락(queue_lock)이 이 큐를 기준으로 이뤄진다.
	 * blk_throtl_init()에서 할당 후 q->td로 역참조됨.
	 * 설정자: blk_throtl_init().
	 * 읽는 자: throtl_pending_timer_fn(), blk_throtl_dispatch_work_fn().
	 * 동기화: 초기화 이후 불변; queue_lock 없이 읽기 가능. */

	/* Total Number of queued bios on READ and WRITE lists */
	unsigned int nr_queued[2];
	/* [한국어] READ[0] / WRITE[1] 방향별로 throtl 큐에 대기 중인 bio 총 수.
	 * __blk_throtl_bio()에서 증가, tg_dispatch_one_bio()에서 감소.
	 * 소프트웨어 Queue Depth 제어 지표; queue_lock 보호.
	 * 설정자: __blk_throtl_bio()(증가), tg_dispatch_one_bio()(감소).
	 * 읽는 자: tg_dispatch_one_bio()가 BUG_ON 검증에 사용.
	 * 동기화: queue_lock(spinlock). */

	/* Work for dispatching throttled bios */
	struct work_struct dispatch_work;
	/* [한국어] throttle 통과 bio를 submit_bio_noacct_nocheck()로 전달하는
	 * kthrotld workqueue work item.
	 * throtl_pending_timer_fn()에서 queue_work()로 예약.
	 * 핸들러: blk_throtl_dispatch_work_fn().
	 * 설정자: INIT_WORK()가 blk_throtl_init()에서 초기화.
	 * 동기화: workqueue 내부 직렬화. */
};

/* [한국어] 전방 선언이 필요한 이유: 이 핸들러의 정의는 파일 한참 아래(디스패치
 * 로직 뒤)에 있지만, 훨씬 위쪽의 throtl_service_queue_init()이 timer_setup()
 * 인자로 이 함수 주소를 넘겨야 한다. 정의를 위로 옮기면 이 함수가 호출하는
 * throtl_select_dispatch() 계열을 모두 앞으로 끌어와야 하므로, 선언만 앞세운다. */
static void throtl_pending_timer_fn(struct timer_list *t);

/*
 * [한국어]
 * tg_to_blkg - throtl_grp에서 blkcg_gq 포인터를 반환한다.
 * @tg: 변환할 throtl_grp
 * @return: 연결된 blkcg_gq 포인터
 *
 * throtl_grp은 blkcg_gq의 policy data(pd)로 내장되므로,
 * pd_to_blkg()로 역참조한다.
 * 호출 체인: 다양한 함수 → [tg_to_blkg] → blkcg_gq 사용
 */
static inline struct blkcg_gq *tg_to_blkg(struct throtl_grp *tg)
{
	return pd_to_blkg(&tg->pd); /* [한국어] throtl_grp -> blkcg_gq 매핑: 이 장치의 cgroup별 queue 상태 접근 */
}

/**
 * sq_to_tg - return the throl_grp the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * Return the throtl_grp @sq belongs to.  If @sq is the top-level one
 * embedded in throtl_data, %NULL is returned.
 */
/*
 * [한국어]
 * sq_to_tg - service queue에서 그것을 품고 있는 throtl_grp를 역산
 *
 * @sq: 대상 service queue
 * @return: 이 sq를 내장한 throtl_grp. 최상위(throtl_data 내장) sq면 NULL.
 *
 * blk-throttle은 cgroup 계층 구조를 그대로 반영한 service queue 트리를 만든다.
 * 트리의 내부 노드는 throtl_grp(cgroup 하나)에 내장된 sq이고, 루트는
 * throtl_data(디스크 하나)에 내장된 sq다. 두 경우를 구분하는 방법이
 * parent_sq의 유무다 — 루트에는 부모가 없다.
 *
 * container_of는 "구조체 멤버의 주소에서 그 구조체 자체의 주소를 빼내는"
 * 커널 관용구로, 순수한 컴파일 타임 오프셋 뺄셈이라 비용이 없다.
 *
 * 실행 컨텍스트: 어디서든(순수 포인터 산술). 락 불필요.
 *
 * 호출 체인:
 *   sq_to_td / throtl_pending_timer_fn 등 → [sq_to_tg]
 */
static struct throtl_grp *sq_to_tg(struct throtl_service_queue *sq)
{
	if (sq && sq->parent_sq) /* [한국어] parent_sq가 있다는 것은 트리의 루트가 아니라는 뜻이고, 루트가 아닌 sq는 반드시 어떤 throtl_grp 안에 내장되어 있다. sq==NULL 방어는 호출자가 이미 루트의 parent_sq(=NULL)를 넘기는 경우가 있기 때문 */
		return container_of(sq, struct throtl_grp, service_queue); /* [한국어] service_queue 멤버 주소에서 컴파일 타임 오프셋만큼 빼서 감싸는 throtl_grp을 얻는다 - 역참조가 아니라 순수 주소 계산이라 비용이 없다 */
	else
		return NULL; /* [한국어] 루트(throtl_data에 내장된 sq)에는 대응하는 cgroup이 없다. 호출자들은 이 NULL을 '최상위에 도달했다'는 신호로 사용한다 */
}

/**
 * sq_to_td - return throtl_data the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * A service_queue can be embedded in either a throtl_grp or throtl_data.
 * Determine the associated throtl_data accordingly and return it.
 */
/*
 * [한국어]
 * sq_to_td - service queue에서 그것이 속한 디스크 단위 throtl_data를 찾는다
 *
 * @sq: 대상 service queue
 * @return: 이 sq가 속한 throtl_data (항상 유효, NULL 아님)
 *
 * sq_to_tg()와 달리 이 함수는 반드시 유효한 값을 돌려준다. 모든 service
 * queue는 어떤 디스크에 속하기 때문이다. 두 경로로 나뉜다:
 *   - 내부 노드(throtl_grp 내장)면 그 그룹이 가리키는 tg->td
 *   - 루트(throtl_data 내장)면 container_of로 직접 역산
 *
 * throtl_data는 디스크 하나의 스로틀 상태 전체(전역 타이머, 큐 등)를 담으므로,
 * 트리 어느 지점에서든 여기 도달할 수 있어야 한다.
 *
 * 실행 컨텍스트: 어디서든(순수 포인터 산술). 락 불필요.
 *
 * 호출 체인:
 *   throtl_schedule_pending_timer 등 → [sq_to_td] → sq_to_tg
 */
static struct throtl_data *sq_to_td(struct throtl_service_queue *sq)
{
	struct throtl_grp *tg = sq_to_tg(sq); /* [한국어] sq가 throtl_grp에 속하는지 확인; 최상위면 NULL */

	if (tg) /* [한국어] cgroup 노드인 경우: throtl_grp이 자신이 속한 디스크의 throtl_data를 이미 캐시하고 있으므로(throtl_pd_init에서 설정) 그대로 쓴다 */
		return tg->td;
	else
		return container_of(sq, struct throtl_data, service_queue); /* [한국어] 루트 노드인 경우: 이 sq 자체가 throtl_data의 멤버이므로 오프셋을 빼서 바깥 구조체를 얻는다 */
}

/*
 * [한국어]
 * tg_bps_limit - cgroup의 READ 또는 WRITE bps 상한을 반환한다.
 * @tg: 조회할 throtl_grp
 * @rw: READ(0) 또는 WRITE(1)
 * @return: bps 제한값(바이트/초). 제한 없으면 U64_MAX.
 *
 * v2(default hierarchy)에서 root cgroup은 자식의 제한을 상속받으므로
 * 자신의 제한을 U64_MAX(무제한)로 반환한다. 그 외 cgroup은 tg->bps[rw]를
 * 반환한다.
 * 호출 체인: tg_within_bps_limit(), tg_dispatch_bps_time() → [tg_bps_limit]
 */
static uint64_t tg_bps_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 여부 확인용 */

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* [한국어] root cgroup은 하위에 제한을 상속시키기 위해 bps 제한을 무제한으로 둠 */
		return U64_MAX; /* [한국어] bps 무제한: 유입 제한 없음 */

	return tg->bps[rw]; /* [한국어] 해당 방향(READ/WRITE)의 bps 상한 반환 */
}

/*
 * [한국어]
 * tg_iops_limit - cgroup의 READ 또는 WRITE iops 상한을 반환한다.
 * @tg: 조회할 throtl_grp
 * @rw: READ(0) 또는 WRITE(1)
 * @return: iops 제한값(회/초). 제한 없으면 UINT_MAX.
 *
 * v2(default hierarchy)에서 root cgroup은 무제한(UINT_MAX) 반환.
 * 호출 체인: tg_within_iops_limit(), tg_dispatch_iops_time() → [tg_iops_limit]
 */
static unsigned int tg_iops_limit(struct throtl_grp *tg, int rw)
{
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 여부 확인용 */

	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && !blkg->parent) /* [한국어] root cgroup iops 무제한: 초당 IO 제한 없음 */
		return UINT_MAX; /* [한국어] iops 무제한; 하위 계층 초당 명령 제한 없음 */

	return tg->iops[rw]; /* [한국어] 해당 방향(READ/WRITE)의 iops 상한 반환 */
}

/**
 * throtl_log - log debug message via blktrace
 * @sq: the service_queue being reported
 * @fmt: printf format string
 * @args: printf args
 *
 * The messages are prefixed with "throtl BLKG_NAME" if @sq belongs to a
 * throtl_grp; otherwise, just "throtl".
 */
/*
 * [한국어]
 * throtl_log - blktrace 스트림에 스로틀 디버그 한 줄을 남기는 매크로
 *
 * 함수가 아니라 매크로인 이유는 두 가지다. 첫째, 가변 인자 포맷을 그대로
 * 하위 blk_add_*_trace_msg()에 넘겨야 한다. 둘째, blktrace가 꺼져 있을 때
 * 인자 평가 자체를 건너뛰어야 한다 - 로그 문자열 인자로는 tg_bps_limit()
 * 같은 함수 호출이 자주 들어가는데, 디스패치 경로는 queue_lock을 잡은 채
 * 도는 핫 경로라 꺼진 로그를 위해 그 계산을 하면 그대로 낭비가 된다.
 *
 * 각 줄의 의도:
 *   __tg / __td : sq 하나만 받아서 "어느 cgroup인지"와 "어느 디스크인지"를
 *                 둘 다 역산한다. 루트 sq면 __tg는 NULL이 된다.
 *   (void)__td  : 값을 쓰지 않고 버리는 이 표현은 "미사용 변수 경고를 막는다"는
 *                 뜻의 관용구다. 아래 세 줄이 모두 __td를 참조하는데도 이것이
 *                 필요하다는 것은, blktrace 관련 매크로가 어떤 커널 설정에서는
 *                 인자를 버리는 형태로 전개되어 __td의 사용처가 전부 사라지는
 *                 경우가 있다는 뜻이다 (그 정의는 include/linux/blktrace_api.h에
 *                 있으며 이 트리에는 포함돼 있지 않아 직접 확인하지 못했다).
 *   likely(!..) : 실운영에서 blktrace는 거의 항상 꺼져 있으므로 분기 예측을
 *                 "로그 안 함" 쪽으로 유도한다.
 *   break       : 매크로 전체가 do { } while (0)이므로 break가 곧 조기 종료다.
 *                 return을 쓰면 호출한 함수가 빠져나가 버리므로 쓸 수 없다.
 *   __tg 유무   : cgroup에 속한 sq면 그 cgroup의 css를 함께 기록해 사용자가
 *                 blktrace 출력에서 어느 cgroup의 스로틀인지 구분할 수 있게
 *                 하고, 루트 sq면 cgroup 정보 없이 디스크 단위로만 남긴다.
 *
 * 실행 컨텍스트: 호출 지점을 그대로 따른다(대부분 queue_lock 보유 + IRQ off).
 */
#define throtl_log(sq, fmt, args...)	do {				\
	struct throtl_grp *__tg = sq_to_tg((sq));			\
	struct throtl_data *__td = sq_to_td((sq));			\
									\
	(void)__td;							\
	if (likely(!blk_trace_note_message_enabled(__td->queue)))	\
		break;							\
	if ((__tg)) {							\
		blk_add_cgroup_trace_msg(__td->queue,			\
			&tg_to_blkg(__tg)->blkcg->css, "throtl " fmt, ##args);\
	} else {							\
		blk_add_trace_msg(__td->queue, "throtl " fmt, ##args);	\
	}								\
} while (0)

/*
 * [한국어]
 * throtl_bio_data_size - bps 계산에 사용할 bio 크기를 반환한다.
 * @bio: 크기를 계산할 bio
 * @return: bps 카운트에 사용할 바이트 수
 *
 * DISCARD 명령은 실제 데이터 전송이 없으므로 512B로 간주한다.
 * 그 외 bio는 bi_iter.bi_size를 사용한다.
 * 호출 체인: tg_within_bps_limit(), throtl_charge_bps_bio() → [throtl_bio_data_size]
 */
static inline unsigned int throtl_bio_data_size(struct bio *bio)
{
	/* assume it's one sector */
	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) /* [한국어] DISCARD는 bi_size가 '지울 영역의 크기'라서 GB 단위가 될 수 있다. 실제로 버스를 타는 데이터는 없는데 그 값을 bps에 그대로 넣으면 한 번의 discard로 cgroup 대역폭 예산이 통째로 소진된다 */
		return 512; /* [한국어] 그래서 최소 단위 1섹터(512B)로 고정 계산한다. bps에서는 무시에 가깝게 만들되, iops 쪽에서는 여전히 1개 명령으로 세어진다 */
	return bio->bi_iter.bi_size; /* [한국어] 일반 bio는 실제 전송 바이트 수를 그대로 사용한다 */
}

/*
 * [한국어]
 * throtl_qnode_init - throtl_qnode를 초기화한다.
 * @qn: 초기화할 qnode
 * @tg: 이 qnode가 속할 throtl_grp
 *
 * qnode는 아직 하위 계층에 진입하지 못하고 throtl 큐에 묶인 bio들을
 * 모아두는 그릇이며, blkg reference를 유지해 bio가 하위 계층으로 디스패치될
 * 때까지 cgroup 객체가 해제되지 않도록 한다.
 * 호출 체인: throtl_pd_alloc() → [throtl_qnode_init]
 */
static void throtl_qnode_init(struct throtl_qnode *qn, struct throtl_grp *tg)
{
	INIT_LIST_HEAD(&qn->node); /* [한국어] qnode를 service_queue의 queued[rw] 리스트에 연결하기 위한 list head 초기화 */
	bio_list_init(&qn->bios_bps); /* [한국어] bps 제한으로 발행이 지연된 bio 리스트 초기화 */
	bio_list_init(&qn->bios_iops); /* [한국어] iops 제한으로 발행이 지연된 bio 리스트 초기화 */
	qn->tg = tg; /* [한국어] bio가 하위 계층으로 디스패치될 때까지 cgroup(blkg) 참조 유지 */
}

/**
 * throtl_qnode_add_bio - add a bio to a throtl_qnode and activate it
 * @bio: bio being added
 * @qn: qnode to add bio to
 * @sq: the service_queue @qn belongs to
 *
 * Add @bio to @qn and put @qn on @sq->queued if it's not already on.
 * @qn->tg's reference count is bumped when @qn is activated.  See the
 * comment on top of throtl_qnode definition for details.
 *
 * [한국어]
 * 한 qnode 안에 큐가 둘(bios_bps, bios_iops)인 이유가 이 함수에 드러난다.
 * bio는 bps 관문을 먼저 통과한 뒤 iops 관문을 만나는데, 두 관문의 대기
 * 대상을 한 리스트에 섞어 두면 "지금 bps에 걸린 bio"와 "bps는 통과했고
 * iops만 남은 bio"를 구분할 수 없다. 분리해 두면 각 관문이 자기 큐의 선두만
 * 보면 되고, 분할되어 되돌아온 bio처럼 이미 bps를 통과한 것들은 bps 줄을
 * 건너뛰고 iops 줄로 바로 들어갈 수 있다.
 *
 * qnode를 sq->queued[]에 처음 올릴 때 blkg 참조를 하나 올린다. 이 참조는
 * throtl_pop_queued()가 qnode를 리스트에서 뺄 때 정확히 하나 내려간다.
 * 대기 중인 bio가 있는 동안 cgroup이 해제되면 그 bio를 꺼낼 자료구조 자체가
 * 사라지기 때문이다.
 */
static void throtl_qnode_add_bio(struct bio *bio, struct throtl_qnode *qn,
				 struct throtl_service_queue *sq)
{
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향; 읽기/쓰기 예산이 방향별로 분리되어 있다 */

	/*
	 * Split bios have already been throttled by bps, so they are
	 * directly queued into the iops path.
	 */
	if (bio_flagged(bio, BIO_TG_BPS_THROTTLED) || /* [한국어] BIO_TG_BPS_THROTTLED: 이 tg에서 bps 예산을 이미 차감했다는 표시(throtl_charge_bps_bio가 설정). 두 번 차감하면 실제보다 대역폭을 적게 준 셈이 된다 */
	    bio_flagged(bio, BIO_BPS_THROTTLED)) { /* [한국어] BIO_BPS_THROTTLED: 트리 최상위까지 bps 검사를 모두 통과한 bio. 두 플래그 중 하나라도 있으면 남은 관문은 iops뿐이므로 bps 큐를 건너뛴다 */
		bio_list_add(&qn->bios_iops, bio); /* [한국어] 발행 전 iops 제한 대기열 */
		sq->nr_queued_iops[rw]++; /* [한국어] iops 제한 대기열 카운트 증가: 초당 IO 한도 초과 여부 추적 */
	} else {
		bio_list_add(&qn->bios_bps, bio); /* [한국어] bps 제한으로 인한 유입 지연 */
		sq->nr_queued_bps[rw]++; /* [한국어] bps 제한 대기열 카운트 증가: 대역폭 한도 초과 여부 추적 */
	}

	if (list_empty(&qn->node)) { /* [한국어] qnode가 처음 활성화될 때만 list에 추가; 발행 전 cgroup 큐에 편입 */
		list_add_tail(&qn->node, &sq->queued[rw]); /* [한국어] bio를 throtl service_queue queued[rw]에 추가; 블록 드라이버 이전 소프트웨어 대기열 */
		blkg_get(tg_to_blkg(qn->tg)); /* [한국어] 디스패치 전까지 blkg 유지 */
	}
}

/**
 * throtl_peek_queued - peek the first bio on a qnode list
 * @queued: the qnode list to peek
 *
 * Always take a bio from the head of the iops queue first. If the queue is
 * empty, we then take it from the bps queue to maintain the overall idea of
 * fetching bios from the head.
 */
/*
 * [한국어]
 * throtl_peek_queued - qnode 리스트의 첫 번째 bio를 peek(꺼내지 않고 확인)한다.
 * @queued: 확인할 qnode list_head (sq->queued[rw])
 * @return: 첫 번째 bio 포인터. 큐가 비면 NULL.
 *
 * iops 큐를 먼저 확인하고, 비어 있으면 bps 큐를 확인한다.
 * tg_dispatch_time()에서 현재 상태 확인용으로 사용.
 * 호출 체인: tg_dispatch_time(), tg_update_disptime() → [throtl_peek_queued]
 */
static struct bio *throtl_peek_queued(struct list_head *queued)
{
	struct throtl_qnode *qn; /* [한국어] 리스트 선두의 qnode - 이번 round-robin 차례인 자식 cgroup */
	struct bio *bio; /* [한국어] peek 결과. 꺼내지 않으므로 큐 상태는 변하지 않는다 */

	if (list_empty(queued)) /* [한국어] 활성화된 qnode가 하나도 없음 = 이 방향으로 대기 중인 bio가 없다 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* [한국어] 선두만 본다. throtl_pop_queued()가 꺼낼 때마다 qnode를 꼬리로 돌리므로, 선두는 항상 '다음 차례인 cgroup'이다 */
	bio = bio_list_peek(&qn->bios_iops); /* [한국어] iops 큐를 먼저 본다: bio는 bps 관문을 통과한 뒤 iops 큐로 옮겨지므로, iops 큐 쪽이 항상 더 오래된(먼저 도착한) bio다. 여기서 순서를 뒤집으면 FIFO가 깨진다 */
	if (!bio)
		bio = bio_list_peek(&qn->bios_bps); /* [한국어] iops 큐가 비었을 때만 아직 bps 관문에 걸려 있는 bio를 본다 */
	WARN_ON_ONCE(!bio); /* [한국어] qnode는 bio가 하나라도 있을 때만 queued 리스트에 올라가고(throtl_qnode_add_bio), 비면 즉시 제거된다(throtl_pop_queued). 따라서 여기서 NULL이면 두 카운터와 리스트 상태가 어긋난 것 */
	return bio; /* [한국어] 호출자(tg_dispatch_time/tg_update_disptime)는 이 bio 하나만으로 그룹 전체의 다음 디스패치 시각을 계산한다 */
}

/**
 * throtl_pop_queued - pop the first bio form a qnode list
 * @sq: the service_queue to pop a bio from
 * @tg_to_put: optional out argument for throtl_grp to put
 * @rw: read/write
 *
 * Pop the first bio from the qnode list @sq->queued. Note that we firstly
 * focus on the iops list because bios are ultimately dispatched from it.
 * After popping, the first qnode is removed from @sq->queued if empty or moved
 * to the end of @sq->queued so that the popping order is round-robin.
 *
 * When the first qnode is removed, its associated throtl_grp should be put
 * too.  If @tg_to_put is NULL, this function automatically puts it;
 * otherwise, *@tg_to_put is set to the throtl_grp to put and the caller is
 * responsible for putting it.
 *
 * [한국어]
 * 이 함수가 하는 일은 세 가지가 한 덩어리로 묶여 있다.
 *   (1) 선두 qnode에서 bio 하나를 꺼내고 해당 대기 카운터를 줄인다.
 *   (2) 그 qnode가 비었으면 리스트에서 빼고, 안 비었으면 꼬리로 돌린다
 *       - 이것이 형제 cgroup 간 round-robin의 전부다. 돌리지 않으면 선두
 *         cgroup이 큐를 비울 때까지 나머지가 전혀 진행하지 못한다.
 *   (3) qnode를 뺐다면 활성화 때 올려둔 blkg 참조를 되돌려 놓아야 한다.
 *
 * @tg_to_put이 있는 이유가 미묘하다. 참조를 여기서 바로 내리면, 그 참조가
 * 마지막 하나였을 경우 blkg와 그 안의 throtl_grp/service_queue가 즉시
 * 해제된다. 그런데 호출자(tg_dispatch_one_bio)는 방금 꺼낸 bio를 바로 그
 * 부모 service_queue에 집어넣어야 하므로, 참조를 내리는 시점을 bio 이동이
 * 끝난 뒤로 미룰 수 있어야 한다. 그래서 "내려야 할 tg"를 밖으로 넘겨주고
 * 실제 put은 호출자가 한다. 호출자가 NULL을 주면(디스패치 work 경로처럼
 * 이후에 sq를 더 안 쓰는 경우) 여기서 바로 내린다.
 */
static struct bio *throtl_pop_queued(struct throtl_service_queue *sq,
				     struct throtl_grp **tg_to_put, bool rw)
{
	struct list_head *queued = &sq->queued[rw]; /* [한국어] READ와 WRITE는 완전히 독립된 리스트다. 한쪽이 막혀도 다른 쪽은 진행할 수 있어야 하기 때문 */
	struct throtl_qnode *qn; /* [한국어] 이번에 꺼낼 대상 qnode(= 자식 cgroup 하나의 대기 슬롯) */
	struct bio *bio; /* [한국어] 꺼낸 bio. 성공하면 호출자가 부모 sq로 옮기거나 실제 발행한다 */

	if (list_empty(queued)) /* [한국어] 이 방향에 활성 qnode가 없음 = 꺼낼 bio 없음 */
		return NULL;

	qn = list_first_entry(queued, struct throtl_qnode, node); /* [한국어] 선두 = 이번 round-robin 차례 */
	bio = bio_list_pop(&qn->bios_iops); /* [한국어] iops 큐 우선: bps를 이미 통과해 여기로 옮겨진 bio가 더 먼저 도착한 bio다 */
	if (bio) { /* [한국어] iops 큐에서 나왔으므로 iops 대기 카운터를 줄인다 */
		sq->nr_queued_iops[rw]--; /* [한국어] sq_queued()가 두 카운터의 합을 보므로, 어느 큐에서 꺼냈는지에 맞는 쪽을 정확히 줄여야 총 대기 수가 어긋나지 않는다 */
	} else {
		bio = bio_list_pop(&qn->bios_bps); /* [한국어] iops 큐가 비었을 때만 bps 큐에서 꺼낸다 */
		if (bio) /* [한국어] bps 큐도 비어 있을 수 있으므로(그 경우는 아래 WARN에 걸린다) NULL 검사 후에 카운터를 만진다 */
			sq->nr_queued_bps[rw]--; /* [한국어] bps 대기 카운터 감소 */
	}
	WARN_ON_ONCE(!bio); /* [한국어] 리스트에 올라온 qnode는 정의상 bio를 최소 하나 갖고 있다. 여기서 NULL이면 카운터/리스트/큐 중 하나가 어긋난 상태 */

	if (bio_list_empty(&qn->bios_bps) && bio_list_empty(&qn->bios_iops)) { /* [한국어] 두 큐가 모두 비었을 때만 비활성화한다 - 한쪽만 보고 지우면 남은 쪽 bio가 영원히 잊힌다 */
		list_del_init(&qn->node); /* [한국어] init까지 하는 이유: throtl_qnode_add_bio()가 list_empty(&qn->node)로 '활성 여부'를 판정하므로, 단순 del로 두면 다음 활성화 판정이 깨진다 */
		if (tg_to_put)
			*tg_to_put = qn->tg; /* [한국어] 참조 해제를 호출자에게 미룬다. 여기서 내리면 마지막 참조였을 때 이 sq/tg가 그 자리에서 해제되어, 호출자가 이어서 하는 bio 이동이 해제된 메모리를 건드리게 된다 */
		else
			blkg_put(tg_to_blkg(qn->tg)); /* [한국어] 호출자가 이후 tg를 안 쓴다고 밝힌 경우(NULL)에는 여기서 바로 내린다. 이 참조는 qnode 활성화 시 throtl_qnode_add_bio()의 blkg_get()과 1:1로 짝을 이룬다 */
	} else {
		list_move_tail(&qn->node, queued); /* [한국어] 아직 bio가 남았으면 꼬리로 보낸다. 이 한 줄이 형제 cgroup 간 공평성을 만든다 - 남겨두면 선두 cgroup이 큐를 다 비울 때까지 나머지가 굶는다 */
	}

	return bio; /* [한국어] WARN에 걸린 경우 NULL이 나갈 수 있으나, 정상 상태에서는 항상 유효한 bio */
}

/*
 * [한국어]
 * throtl_service_queue_init - throtl_service_queue를 초기화한다.
 * @sq: 초기화할 service_queue (호출자가 0으로 초기화했다고 가정)
 *
 * queued[READ/WRITE] 리스트, pending_tree RB 트리, pending_timer를 초기화.
 * pending_timer 핸들러는 throtl_pending_timer_fn()이며 disptime 도달 시 발동.
 * 호출 체인: throtl_pd_alloc(), blk_throtl_init() → [throtl_service_queue_init]
 */
/* init a service_queue, assumes the caller zeroed it */
static void throtl_service_queue_init(struct throtl_service_queue *sq)
{
	INIT_LIST_HEAD(&sq->queued[READ]); /* [한국어] READ 방향 throtl 대기열 초기화: 발행 전 READ bio 큐 */
	INIT_LIST_HEAD(&sq->queued[WRITE]); /* [한국어] WRITE 방향 throtl 대기열 초기화: 발행 전 WRITE bio 큐 */
	sq->pending_tree = RB_ROOT_CACHED; /* [한국어] 자식 그룹들을 disptime 오름차순으로 세워 둘 트리. _CACHED 형태라 최좌단(=다음에 깨울 그룹)을 O(1)로 얻는다 */
	timer_setup(&sq->pending_timer, throtl_pending_timer_fn, 0); /* [한국어] pending_timer 초기화: 하위 계층으로 bio를 풀어줄 시점을 지연시키는 소프트웨어 타이머 */
}

/*
 * [한국어]
 * throtl_pd_alloc - blkcg policy data를 할당하고 throtl_grp을 초기화한다.
 * @disk: 대상 gendisk
 * @blkcg: 대상 blkcg
 * @gfp: 메모리 할당 플래그
 * @return: 초기화된 blkg_policy_data 포인터, 실패 시 NULL
 *
 * 새 cgroup이 이 장치에 대해 활성화될 때마다 생성되며,
 * bps/iops 상한을 기본 무제한(U64_MAX/UINT_MAX)으로 시작한다.
 * 실패 시 이미 할당된 rwstat을 해제하고 NULL 반환.
 * 호출 체인: blkcg_activate_policy() → pd_alloc_fn → [throtl_pd_alloc]
 */
static struct blkg_policy_data *throtl_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct throtl_grp *tg; /* [한국어] 새로 만들 per-(cgroup, 디스크) 스로틀 상태. pd가 첫 멤버라 &tg->pd가 곧 tg 주소다 */
	int rw; /* [한국어] READ(0)/WRITE(1) 반복자. 방향별 자료구조가 모두 2칸 배열이라 루프로 초기화한다 */

	tg = kzalloc_node(sizeof(*tg), gfp, disk->node_id); /* [한국어] 이 tg는 해당 디스크의 IO 경로에서 매 bio마다 읽히므로, 디스크가 붙은 NUMA 노드에 할당해 원격 노드 접근을 줄인다. kzalloc이라 아래에서 명시적으로 채우지 않는 필드는 전부 0으로 시작한다 */
	if (!tg) /* [한국어] gfp가 GFP_NOWAIT일 수 있는(blkg 생성 경로) 호출이라 실패는 정상적으로 일어날 수 있는 사건이다 */
		return NULL;

	if (blkg_rwstat_init(&tg->stat_bytes, gfp)) /* [한국어] percpu 카운터를 내부에서 할당하므로 실패할 수 있다. 여기서부터는 되돌릴 것이 생긴다 */
		goto err_free_tg; /* [한국어] 아직 stat_bytes는 살아 있지 않으므로 tg만 해제하면 된다 */

	if (blkg_rwstat_init(&tg->stat_ios, gfp)) /* [한국어] 두 번째 percpu 할당 */
		goto err_exit_stat_bytes; /* [한국어] 성공한 stat_bytes를 먼저 되돌리고 그 다음 tg를 해제 - 할당의 역순으로 푸는 표준적인 되감기 순서다 */

	throtl_service_queue_init(&tg->service_queue); /* [한국어] 이 cgroup이 '부모'가 되어 자식들의 bio를 받는 큐 + 그 큐의 pending 타이머를 준비한다. kzalloc 이후여야 타이머/리스트 초기화가 유효하다 */

	for (rw = READ; rw <= WRITE; rw++) { /* [한국어] 방향마다 qnode가 2개씩, 총 4개 필요하다 */
		throtl_qnode_init(&tg->qnode_on_self[rw], tg); /* [한국어] 이 cgroup 자신의 sq에 매달릴 슬롯: 이 cgroup에 직접 도착한 bio가 여기 쌓인다 */
		throtl_qnode_init(&tg->qnode_on_parent[rw], tg); /* [한국어] 부모 sq에 매달릴 슬롯: 부모 큐에서 '이 자식 몫'을 따로 유지해야 형제 간 round-robin이 성립하므로, 자기 큐용과 부모 큐용 슬롯을 분리해 둔다 */
	}

	RB_CLEAR_NODE(&tg->rb_node); /* [한국어] '아직 pending_tree에 없음'을 명시적으로 표시. throtl_rb_erase()가 지운 노드에도 같은 처리를 해, 삽입/삭제가 반복돼도 상태 판정이 일관되게 유지된다 */
	tg->bps[READ] = U64_MAX; /* [한국어] 기본값은 '제한 없음'이다. 0이 무제한이 아니라 U64_MAX가 무제한인 이유는, 0을 무제한으로 두면 대역폭 계산식에서 0으로 나누는 경우와 구분되지 않기 때문 */
	tg->bps[WRITE] = U64_MAX; /* [한국어] 쓰기 방향도 동일하게 무제한으로 시작 */
	tg->iops[READ] = UINT_MAX; /* [한국어] iops는 unsigned int 폭이므로 무제한 표식도 UINT_MAX다 */
	tg->iops[WRITE] = UINT_MAX; /* [한국어] 쓰기 방향 iops 무제한 */

	return &tg->pd; /* [한국어] blk-cgroup 코어에는 pd 주소를 돌려준다. pd가 구조체 첫 멤버라는 규약 덕에 나중에 container_of로 tg를 되찾을 수 있다 */

err_exit_stat_bytes:
	blkg_rwstat_exit(&tg->stat_bytes); /* [한국어] stat_ios 초기화 실패 경로에서만 도달: 앞서 성공한 percpu 카운터를 해제한다 */
err_free_tg:
	kfree(tg); /* [한국어] 두 실패 경로가 공유하는 마지막 단계 */
	return NULL; /* [한국어] NULL을 받은 blkcg_activate_policy()는 -ENOMEM으로 정책 활성화를 중단한다 */
}

/*
 * [한국어]
 * throtl_pd_init - 할당된 policy data를 부모 service_queue에 연결한다.
 * @pd: 초기화할 blkg_policy_data (throtl_grp)
 *
 * cgroup 계층이 하위 계층에 도달하기 전의 IO 우선순위/제한 트리가 된다.
 * v1(non-dfl)에서는 모든 tg가 throtl_data 최상위 service_queue 바로 아래로
 * 평탄화되고, v2(dfl)에서는 실제 계층 구조를 따라 부모 tg의 service_queue를
 * 부모로 설정한다.
 * 호출 체인: blkcg_activate_policy() → pd_init_fn → [throtl_pd_init]
 */
static void throtl_pd_init(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	struct blkcg_gq *blkg = tg_to_blkg(tg); /* [한국어] throtl_grp → blkcg_gq 변환; parent 확인용 */
	struct throtl_data *td = blkg->q->td; /* [한국어] blkcg_gq -> request_queue -> throtl_data 연결; 디스크(request_queue) 단위 throttle */
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg 자신의 service_queue; 부모 연결 대상 */

	/*
	 * If on the default hierarchy, we switch to properly hierarchical
	 * behavior where limits on a given throtl_grp are applied to the
	 * whole subtree rather than just the group itself.  e.g. If 16M
	 * read_bps limit is set on a parent group, summary bps of
	 * parent group and its subtree groups can't exceed 16M for the
	 * device.
	 *
	 * If not on the default hierarchy, the broken flat hierarchy
	 * behavior is retained where all throtl_grps are treated as if
	 * they're all separate root groups right below throtl_data.
	 * Limits of a group don't interact with limits of other groups
	 * regardless of the position of the group in the hierarchy.
	 */
	sq->parent_sq = &td->service_queue; /* [한국어] v1(non-dfl)에서는 모든 throtl_grp이 throtl_data의 최상위 service_queue 아래로 평탄화 */
	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && blkg->parent) /* [한국어] v2(dfl)에서 parent가 있으면 상위 cgroup의 service_queue를 부모로 설정 (계층적 cgroup QoS) */
		sq->parent_sq = &blkg_to_tg(blkg->parent)->service_queue; /* [한국어] 부모 blkg의 throtl_grp service_queue를 이 tg의 부모로 설정 */
	tg->td = td; /* [한국어] throtl_data 역참조; 디스크(request_queue) 단위 dispatch_work/pending_timer 접근용 */
}

/*
 * Set has_rules[] if @tg or any of its parents have limits configured.
 * This doesn't require walking up to the top of the hierarchy as the
 * parent's has_rules[] is guaranteed to be correct.
 *
 * [한국어]
 * tg_update_has_rules - 자신이나 조상에게 bps/iops 제한이 있는지 표시한다.
 * @tg: 갱신할 throtl_grp
 *
 * 제한이 전혀 없는 cgroup은 blk_should_throtl()에서 곧바로 걸러져
 * __blk_throtl_bio()의 락/트리 작업을 통째로 건너뛴다. 즉 이 플래그는
 * 스로틀 판정 결과가 아니라 "판정을 할 필요가 있는가"를 미리 계산해 둔
 * 캐시다.
 *
 * 조상까지 거슬러 올라가지 않아도 되는 이유: 부모의 has_rules[]는 부모가
 * 갱신될 때 이미 그 위 조상들을 반영해 놓았고, 설정이 바뀌면
 * tg_conf_updated()가 서브트리를 pre-order로 순회하며 위에서 아래로
 * 다시 계산한다. 그래서 부모 한 칸만 보면 충분하다.
 * 호출 체인: throtl_pd_online(), tg_conf_updated() → [tg_update_has_rules]
 */
static void tg_update_has_rules(struct throtl_grp *tg)
{
	struct throtl_grp *parent_tg = sq_to_tg(tg->service_queue.parent_sq); /* [한국어] 루트 바로 아래 그룹이면 부모 sq가 throtl_data의 sq라서 NULL이 나온다. 아래 조건에서 항상 NULL 검사를 먼저 하는 이유가 이것 */
	int rw; /* [한국어] 방향별로 규칙이 따로 있으므로(읽기만 제한하는 설정이 흔하다) READ/WRITE를 분리해 계산한다 */

	for (rw = READ; rw <= WRITE; rw++) { /* [한국어] 두 방향을 각각 독립적으로 판정 */
		tg->has_rules_iops[rw] = /* [한국어] "나 또는 내 조상 중 누구라도 iops 제한을 걸었는가" */
			(parent_tg && parent_tg->has_rules_iops[rw]) || /* [한국어] 조상 쪽 결론은 부모의 플래그 한 칸에 이미 접혀 있다 */
			tg_iops_limit(tg, rw) != UINT_MAX; /* [한국어] UINT_MAX가 '무제한' 표식이므로, 그와 다르면 내가 직접 건 제한이 있다는 뜻. tg->iops[]를 직접 보지 않고 tg_iops_limit()을 쓰는 이유는 cgroup v2 루트가 항상 무제한으로 취급돼야 하기 때문 */
		tg->has_rules_bps[rw] = /* [한국어] bps 쪽도 같은 구조 */
			(parent_tg && parent_tg->has_rules_bps[rw]) || /* [한국어] 조상에 bps 제한이 있으면 내 bio도 결국 그 제한을 통과해야 하므로 나도 검사 대상이다 */
			tg_bps_limit(tg, rw) != U64_MAX; /* [한국어] bps의 무제한 표식은 U64_MAX */
	}
}

/*
 * [한국어]
 * throtl_pd_online - cgroup이 online될 때 has_rules[]를 갱신한다.
 * @pd: online된 blkg_policy_data (throtl_grp)
 *
 * 새 cgroup이 조상의 제한을 벗어나지 않도록 has_rules[]를 갱신.
 * 이후 이 cgroup으로 들어오는 bio는 갱신된 규칙으로 rate limit 적용.
 * 호출 체인: blkcg_activate_policy() → pd_online_fn → [throtl_pd_online]
 */
static void throtl_pd_online(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	/*
	 * We don't want new groups to escape the limits of its ancestors.
	 * Update has_rules[] after a new group is brought online.
	 */
	tg_update_has_rules(tg); /* [한국어] cgroup online 시 제한 상태 갱신; 이후 bio부터 유입 제한 결정 */
}

/*
 * [한국어]
 * throtl_pd_free - throtl_grp에 할당된 모든 자원을 해제한다.
 * @pd: 해제할 blkg_policy_data (throtl_grp)
 *
 * pending_timer 동기적 삭제 → rwstat 해제 → kfree 순서로 정리.
 * 호출 체인: blkcg_policy.pd_free_fn → [throtl_pd_free]
 */
static void throtl_pd_free(struct blkg_policy_data *pd)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */

	timer_delete_sync(&tg->service_queue.pending_timer); /* [한국어] _sync여야 한다. 그냥 delete하면 이미 다른 CPU에서 실행 중인 콜백이 아래 kfree 이후에도 tg를 계속 만져 use-after-free가 된다. 콜백이 끝날 때까지 여기서 기다린다 */
	blkg_rwstat_exit(&tg->stat_bytes); /* [한국어] bps 통계 rwstat 해제 */
	blkg_rwstat_exit(&tg->stat_ios); /* [한국어] iops 통계 rwstat 해제 */
	kfree(tg); /* [한국어] throtl_grp 메모리 해제; 블록 장치 throttle 계층에서 제거 */
}

/*
 * [한국어]
 * throtl_rb_first - pending_tree에서 disptime이 가장 이른 throtl_grp을 반환.
 * @parent_sq: 탐색할 상위 service_queue
 * @return: leftmost throtl_grp 포인터. 트리가 비면 NULL.
 *
 * pending_tree는 disptime 기준으로 정렬된 RB 트리이므로 leftmost가
 * 다음 dispatch 대상이다.
 * 호출 체인: throtl_select_dispatch(), update_min_dispatch_time() → [throtl_rb_first]
 */
static struct throtl_grp *
throtl_rb_first(struct throtl_service_queue *parent_sq)
{
	struct rb_node *n; /* [한국어] 트리의 최좌단 노드 = disptime이 가장 이른 그룹 */

	n = rb_first_cached(&parent_sq->pending_tree); /* [한국어] _cached 계열은 최좌단 노드를 별도 필드에 캐시해 두므로 트리를 내려가지 않고 O(1)로 얻는다. 디스패치 루프가 매 회전마다 이 값을 보기 때문에 상수 시간이 중요하다 */
	WARN_ON_ONCE(!n); /* [한국어] 호출자들은 모두 nr_pending > 0을 확인한 뒤에 들어온다. 그런데도 트리가 비었다면 nr_pending과 트리가 어긋난 것이므로 한 번만 경고를 띄운다 */
	if (!n) /* [한국어] 경고를 내되 죽지는 않는다 - 스로틀 상태가 어긋났다고 IO 경로 전체를 멈출 이유는 없다 */
		return NULL;
	return rb_entry_tg(n); /* [한국어] rb_node는 throtl_grp 안에 박혀 있으므로 오프셋을 빼서 그룹을 되찾는다 */
}

/*
 * [한국어]
 * throtl_rb_erase - pending_tree에서 throtl_grp 노드를 제거한다.
 * @n: 제거할 rb_node
 * @parent_sq: 대상 service_queue
 *
 * disptime이 갱신되거나 cgroup이 비면 호출해 트리를 정리한다.
 * 호출 체인: throtl_dequeue_tg(), tg_update_disptime() → [throtl_rb_erase]
 */
static void throtl_rb_erase(struct rb_node *n,
			    struct throtl_service_queue *parent_sq)
{
	rb_erase_cached(n, &parent_sq->pending_tree); /* [한국어] disptime이 도래한 cgroup을 pending_tree에서 제거; dispatch 라운드 완료 */
	RB_CLEAR_NODE(n); /* [한국어] rb_node 초기화; 이미 트리에서 제거된 노드 표시 */
}

/*
 * [한국어]
 * update_min_dispatch_time - parent_sq의 first_pending_disptime을 갱신한다.
 * @parent_sq: 갱신할 service_queue
 *
 * pending_tree의 leftmost tg의 disptime을 first_pending_disptime에 복사.
 * pending_timer 만료 시점을 이 값 기준으로 arm한다.
 * 호출 체인: throtl_schedule_next_dispatch() → [update_min_dispatch_time]
 */
static void update_min_dispatch_time(struct throtl_service_queue *parent_sq)
{
	struct throtl_grp *tg;

	tg = throtl_rb_first(parent_sq); /* [한국어] pending_tree의 leftmost(가장 이른 disptime) tg 획득 */
	if (!tg) /* [한국어] pending_tree가 비면 갱신 불필요 */
		return;

	parent_sq->first_pending_disptime = tg->disptime; /* [한국어] 트리 최좌단의 시각을 부모 쪽에 복사해 둔다. 타이머를 걸 때마다 트리를 다시 뒤지지 않고 이 필드만 보면 되도록 캐시하는 것 */
}

/*
 * [한국어]
 * tg_service_queue_add - throtl_grp을 부모 service_queue의 pending_tree에 삽입.
 * @tg: 삽입할 throtl_grp (tg->disptime이 RB tree 키)
 *
 * disptime 오름차순 RB tree에 삽입. leftmost면 pending_timer 만료 후보.
 * 호출 체인: throtl_enqueue_tg(), tg_update_disptime() → [tg_service_queue_add]
 */
static void tg_service_queue_add(struct throtl_grp *tg)
{
	struct throtl_service_queue *parent_sq = tg->service_queue.parent_sq; /* [한국어] 트리는 '부모'가 소유한다. 형제들끼리 disptime 순으로 줄을 세워야 부모가 다음에 누구를 깨울지 O(1)로 알 수 있기 때문 */
	struct rb_node **node = &parent_sq->pending_tree.rb_root.rb_node; /* [한국어] 이중 포인터를 쓰는 이유: 마지막에 rb_link_node()가 이 위치에 새 노드 주소를 써 넣어야 하므로, 값이 아니라 '어디에 쓸지'를 들고 내려간다 */
	struct rb_node *parent = NULL; /* [한국어] 내려가면서 마지막으로 지난 노드. 트리가 비어 있으면 NULL로 남아 새 노드가 루트가 된다 */
	struct throtl_grp *__tg; /* [한국어] 비교 대상인 현재 노드의 그룹. 매크로 인자와 이름이 겹치지 않도록 밑줄 접두사를 쓴다 */
	unsigned long key = tg->disptime; /* [한국어] 정렬 키는 '이 그룹이 다음 bio를 낼 수 있는 시각'이다. 우선순위가 아니라 시각이므로, 같은 값이 여러 개 있어도 문제되지 않는다 */
	bool leftmost = true; /* [한국어] rb_insert_color_cached()에 넘길 힌트. 한 번이라도 오른쪽으로 꺾으면 최좌단이 아니게 되므로 아래에서 false로 떨어뜨린다 */

	while (*node != NULL) { /* [한국어] 표준 BST 하강. 삽입 위치(빈 자리)를 찾을 때까지 내려간다 */
		parent = *node; /* [한국어] 지금 서 있는 노드를 새 노드의 부모 후보로 기억 */
		__tg = rb_entry_tg(parent); /* [한국어] 비교하려면 rb_node가 아니라 그룹의 disptime이 필요하므로 바깥 구조체를 되찾는다 */

		if (time_before(key, __tg->disptime)) /* [한국어] 단순 '<'가 아니라 time_before()를 쓰는 이유: jiffies는 32비트에서 약 49.7일마다 감싸 돌기 때문에, 부호 있는 뺄셈으로 비교해야 랩어라운드 구간에서도 순서가 뒤집히지 않는다 */
			node = &parent->rb_left; /* [한국어] 더 이른 시각이면 왼쪽으로 */
		else {
			node = &parent->rb_right; /* [한국어] 같거나 늦으면 오른쪽. 동률을 오른쪽으로 보내면 먼저 들어온 노드가 계속 왼쪽에 남아 FIFO에 가까운 성질을 얻는다 */
			leftmost = false; /* [한국어] 오른쪽으로 꺾은 이상 이 노드는 최좌단이 될 수 없다 */
		}
	}

	rb_link_node(&tg->rb_node, parent, node); /* [한국어] 찾아둔 빈 자리에 노드를 매단다(아직 색 규칙은 깨진 상태) */
	rb_insert_color_cached(&tg->rb_node, &parent_sq->pending_tree, /* [한국어] 회전/재색칠로 균형을 복구하면서, leftmost 힌트로 캐시된 최좌단 포인터도 함께 갱신한다 */
			       leftmost); /* [한국어] 이 힌트 덕분에 throtl_rb_first()가 트리를 내려가지 않고 상수 시간에 다음 대상을 얻는다 */
}

/*
 * [한국어]
 * throtl_enqueue_tg - throtl_grp을 pending_tree에 등록한다.
 * @tg: 등록할 throtl_grp
 *
 * THROTL_TG_PENDING 플래그가 없는 경우에만 tg_service_queue_add()를 호출.
 * 이미 등록된 tg를 중복 삽입하지 않도록 보호.
 * 호출 체인: throtl_add_bio_tg() → [throtl_enqueue_tg] → tg_service_queue_add()
 */
static void throtl_enqueue_tg(struct throtl_grp *tg)
{
	if (!(tg->flags & THROTL_TG_PENDING)) { /* [한국어] THROTL_TG_PENDING 비트 테스트; 이미 dispatch 예약된 cgroup인지 확인 */
		tg_service_queue_add(tg); /* [한국어] pending_tree에 disptime 기준으로 삽입 */
		tg->flags |= THROTL_TG_PENDING; /* [한국어] '트리에 들어 있음'을 기록한다. 이 플래그가 곧 중복 삽입 가드이고, throtl_dequeue_tg()가 제거 여부를 판단하는 근거이기도 하다 */
		tg->service_queue.parent_sq->nr_pending++; /* [한국어] 부모 service_queue의 pending cgroup 수 증가; 하위 계층으로 풀어줄 후보 증가 */
	}
}

/*
 * [한국어]
 * throtl_dequeue_tg - throtl_grp을 pending_tree에서 제거한다.
 * @tg: 제거할 throtl_grp
 *
 * THROTL_TG_PENDING 플래그가 있을 때만 동작. tg의 큐가 비거나 flush 시 호출.
 * 호출 체인: throtl_select_dispatch(), tg_flush_bios() → [throtl_dequeue_tg]
 */
static void throtl_dequeue_tg(struct throtl_grp *tg)
{
	if (tg->flags & THROTL_TG_PENDING) { /* [한국어] 트리에 없는 노드를 지우면 rb_erase가 엉뚱한 링크를 건드린다. 이 플래그가 '실제로 트리에 올라가 있다'는 유일한 판정 근거이고, throtl_enqueue_tg()의 중복 삽입 가드와 짝을 이룬다 */
		struct throtl_service_queue *parent_sq =
			tg->service_queue.parent_sq; /* [한국어] 트리와 nr_pending 카운터는 모두 부모 쪽에 있다 */

		throtl_rb_erase(&tg->rb_node, parent_sq); /* [한국어] 트리에서 빼고 노드를 초기화한다. 빼는 순간 캐시된 최좌단도 함께 갱신되어 다음 타이머 대상이 자동으로 바뀐다 */
		--parent_sq->nr_pending; /* [한국어] 부모 pending cgroup 수 감소; 발행 후보 감소 */
		tg->flags &= ~THROTL_TG_PENDING; /* [한국어] 상태 표식도 함께 지운다. 트리 상태와 플래그가 어긋나면 다음 삽입/삭제가 잘못된 판단을 한다 */
	}
}

/*
 * [한국어]
 * throtl_schedule_pending_timer - pending_timer를 지정 만료 시점으로 arm한다.
 * @sq: 타이머를 가진 service_queue
 * @expires: 타이머 만료 jiffies
 *
 * 최대 8 슬라이스(800ms) 제한을 두어 동적 limit 변경 시 과도한 지연 방지.
 * queue_lock 보유 상태에서 호출.
 * 호출 체인: throtl_schedule_next_dispatch(), tg_flush_bios() → [throtl_schedule_pending_timer]
 */
/* Call with queue lock held */
static void throtl_schedule_pending_timer(struct throtl_service_queue *sq,
					  unsigned long expires)
{
	unsigned long max_expire = jiffies + 8 * DFL_THROTL_SLICE; /* [한국어] 상한은 8 슬라이스 = 800ms(HZ/10 기준). 이 상한이 필요한 이유는 아래 원본 주석대로 '제한이 동적으로 바뀌기 때문'이다: 아주 낮은 bps로 계산된 대기 시간이 수십 초일 수 있는데, 그 사이 사용자가 제한을 올려도 타이머가 깨어나지 않으면 새 설정이 반영되지 않는다. 최소 800ms마다 한 번은 깨어나 재계산하게 만든다 */

	/*
	 * Since we are adjusting the throttle limit dynamically, the sleep
	 * time calculated according to previous limit might be invalid. It's
	 * possible the cgroup sleep time is very long and no other cgroups
	 * have IO running so notify the limit changes. Make sure the cgroup
	 * doesn't sleep too long to avoid the missed notification.
	 */
	if (time_after(expires, max_expire)) /* [한국어] 계산된 만료 시각이 상한을 넘으면 */
		expires = max_expire; /* [한국어] 최대 8 슬라이스로 클리핑; 제한 변경 알림을 놓치지 않도록 */
	mod_timer(&sq->pending_timer, expires); /* [한국어] add_timer가 아니라 mod_timer인 이유: 이 타이머는 이미 걸려 있을 수 있고(다른 자식이 먼저 예약), mod_timer는 그 경우 만료 시각만 바꾼다. 즉 sq마다 타이머는 항상 최대 1개만 살아 있다 */
	/* [한국어] 예약 결과를 blktrace에 남긴다. 절대 시각(expires)이 아니라 delay(=expires-jiffies)로
	 * 찍는 이유는, 로그를 읽을 때 궁금한 것이 "언제"가 아니라 "얼마나 미뤘는가"이기 때문이다.
	 * jiffies도 함께 남겨 여러 로그 줄 사이의 시간 간격을 복원할 수 있게 한다. */
	throtl_log(sq, "schedule timer. delay=%lu jiffies=%lu",
		   expires - jiffies, jiffies);
}

/**
 * throtl_schedule_next_dispatch - schedule the next dispatch cycle
 * @sq: the service_queue to schedule dispatch for
 * @force: force scheduling
 *
 * Arm @sq->pending_timer so that the next dispatch cycle starts on the
 * dispatch time of the first pending child.  Returns %true if either timer
 * is armed or there's no pending child left.  %false if the current
 * dispatch window is still open and the caller should continue
 * dispatching.
 *
 * If @force is %true, the dispatch timer is always scheduled and this
 * function is guaranteed to return %true.  This is to be used when the
 * caller can't dispatch itself and needs to invoke pending_timer
 * unconditionally.  Note that forced scheduling is likely to induce short
 * delay before dispatch starts even if @sq->first_pending_disptime is not
 * in the future and thus shouldn't be used in hot paths.
 *
 * [한국어]
 * pending_tree의 첫 그룹 disptime이 미래면 타이머를 걸고, 이미 지났으면
 * 호출자가 그 자리에서 디스패치를 계속하게 한다. 반환값의 의미가 직관과
 * 반대라 헷갈리기 쉬운데,
 * true는 "네가 더 할 일은 없다(타이머를 걸었거나 대기 중인 자식이 없다)"이고
 * false는 "창이 아직 열려 있으니 계속 디스패치하라"는 뜻이다.
 */
static bool throtl_schedule_next_dispatch(struct throtl_service_queue *sq,
					  bool force)
{
	/* any pending children left? */
	if (!sq->nr_pending) /* [한국어] 대기 중인 자식이 없으면 걸 타이머도 없다. true를 돌려주어 호출자의 디스패치 루프를 끝낸다 */
		return true;

	update_min_dispatch_time(sq); /* [한국어] 트리 최좌단의 시각을 first_pending_disptime에 복사해 온다 */

	/* is the next dispatch time in the future? */
	if (force || time_after(sq->first_pending_disptime, jiffies)) { /* [한국어] 아직 시간이 안 됐으면 타이머로 미룬다. force는 '호출자가 지금 직접 디스패치할 수 없는 상황'에서 쓰이며, 시간이 이미 됐더라도 굳이 타이머를 걸어 다른 컨텍스트에서 처리하게 만든다 */
		throtl_schedule_pending_timer(sq, sq->first_pending_disptime); /* [한국어] 하위 계층으로 bio를 풀어줄 시점에 pending_timer arm */
		return true;
	}

	/* tell the caller to continue dispatching */
	return false; /* [한국어] dispatch window가 열려 있음; 하위 계층으로 즉시 추가 bio 전달 가능 */
}

/*
 * [한국어]
 * throtl_start_new_slice_with_credit - 이전 슬라이스 미사용분을 credit으로
 * 이월하며 새 슬라이스를 시작한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @start: credit 시작 기준 jiffies
 *
 * 이전 슬라이스에서 대역폭을 다 쓰지 않은 경우 credit 이월을 통해
 * 유휴 후 burst를 허용. bytes_disp/io_disp는 0으로 초기화.
 * 호출 체인: start_parent_slice_with_credit() → [throtl_start_new_slice_with_credit]
 */
static inline void throtl_start_new_slice_with_credit(struct throtl_grp *tg,
		bool rw, unsigned long start)
{
	tg->bytes_disp[rw] = 0; /* [한국어] 사용량을 0으로 되돌린다. 이 파일의 '슬라이스'는 (slice_start, 사용량) 한 쌍으로 표현되는 토큰 버킷이다 - 남은 토큰을 따로 세지 않고, "경과 시간 × limit(=지금까지 벌어들인 토큰) - 사용량"으로 매번 계산한다 */
	tg->io_disp[rw] = 0; /* [한국어] iops 쪽 사용량도 같이 리셋. bps와 iops는 같은 시간 창(slice_start~slice_end)을 공유하지만 예산은 각자 센다 */

	/*
	 * Previous slice has expired. We must have trimmed it after last
	 * bio dispatch. That means since start of last slice, we never used
	 * that bandwidth. Do try to make use of that bandwidth while giving
	 * credit.
	 */
	if (time_after(start, tg->slice_start[rw])) /* [한국어] 여기서 '크레딧'이란 slice_start를 과거로 남겨두는 것이다. 예산은 (jiffies - slice_start) × limit로 계산되므로, 시작점이 과거일수록 즉시 쓸 수 있는 토큰이 많다. 부모의 slice_start가 자식이 기다리기 시작한 시점(start)보다 더 과거라면 굳이 앞당길 필요가 없으므로, start가 더 나중일 때만 갱신한다 */
		tg->slice_start[rw] = start; /* [한국어] 자식이 대기하기 시작한 시각으로 맞춘다. 자식이 참고 기다린 구간만큼은 부모 입장에서도 '쓰지 않은 대역폭'이므로 그 몫을 인정해 준다 */

	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* [한국어] 끝은 항상 '지금부터 한 슬라이스'다. 시작만 과거로 늘리고 끝은 현재 기준으로 두어, 크레딧은 즉시 쓰이되 무한히 누적되지는 않게 한다 */
	/* [한국어] 크레딧 이월로 시작된 슬라이스임을 별도 문구로 남긴다. start와 end의 간격이
	 * 한 슬라이스보다 크게 찍히는 것이 정상이며, 그 차이가 곧 이월된 크레딧의 크기다. */
	throtl_log(&tg->service_queue,
		   "[%c] new slice with credit start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_start_new_slice - 새로운 rate limit 슬라이스를 시작한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @clear: 사용량(bytes_disp/io_disp)을 0으로 초기화할지 여부
 *
 * slice_start를 현재 jiffies로, slice_end를 jiffies + DFL_THROTL_SLICE로 설정.
 * 호출 체인: tg_update_slice(), tg_conf_updated() → [throtl_start_new_slice]
 */
static inline void throtl_start_new_slice(struct throtl_grp *tg, bool rw,
					  bool clear)
{
	if (clear) { /* [한국어] clear=false로 부르는 곳이 tg_conf_updated()다. 설정 변경 직전에 tg_update_carryover()가 bytes_disp/io_disp에 '새 기준에서의 빚/여유'를 음수까지 포함해 넣어 두는데, 여기서 0으로 밀어 버리면 그 보정이 통째로 사라진다 */
		tg->bytes_disp[rw] = 0; /* [한국어] 평상시(슬라이스 만료 후 재시작)에는 사용량을 지우고 깨끗한 창에서 다시 센다 */
		tg->io_disp[rw] = 0; /* [한국어] iops 사용량도 동일 */
	}
	tg->slice_start[rw] = jiffies; /* [한국어] 크레딧 없는 새 창은 '지금'부터 시작한다. 즉 시작 시점에 쓸 수 있는 토큰이 0이며, 첫 bio는 아래 tg_within_bps_limit()의 "경과 0이면 한 슬라이스로 간주" 규칙 덕분에 통과한다 */
	tg->slice_end[rw] = jiffies + DFL_THROTL_SLICE; /* [한국어] 창 길이는 기본 100ms. 짧게 잡을수록 순간 rate가 정확해지지만 타이머 깨우기가 잦아지고, 길게 잡으면 버스트가 커진다 */

	/* [한국어] 새 슬라이스 경계를 로그로 남겨, 나중에 blktrace에서 "언제 창이 리셋됐고
	 * 그래서 예산이 왜 늘었는지"를 추적할 수 있게 한다. */
	throtl_log(&tg->service_queue,
		   "[%c] new slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_set_slice_end - 슬라이스 종료 시점을 DFL_THROTL_SLICE 단위로 정렬.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @jiffy_end: 목표 종료 jiffies (slice 단위로 올림)
 *
 * 호출 체인: throtl_trim_slice(), throtl_extend_slice(), tg_dispatch_bps_time()
 *           → [throtl_set_slice_end]
 */
static inline void throtl_set_slice_end(struct throtl_grp *tg, bool rw,
					unsigned long jiffy_end)
{
	tg->slice_end[rw] = roundup(jiffy_end, DFL_THROTL_SLICE); /* [한국어] 요청받은 시각을 그대로 쓰지 않고 슬라이스 배수로 '올림'한다. 항상 올림이므로 창이 요구보다 짧아지는 일은 없고(짧아지면 아직 기다려야 할 bio가 만료된 창을 만나 예산이 리셋돼 버린다), 경계를 격자에 맞춰 두면 throtl_trim_slice()의 rounddown 계산과 어긋나지 않는다. jiffy_end는 절대 시각이므로 roundup의 기준점도 부팅 이후 jiffies 0이다 */
}

/*
 * [한국어]
 * throtl_extend_slice - 슬라이스 종료 시점을 jiffy_end까지 연장한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @jiffy_end: 연장할 목표 jiffies
 *
 * 이미 충분히 길면 연장하지 않음. bio 대기 시간이 현재 슬라이스를 넘어서면
 * slice_end를 확장해 다음 슬라이스 시작을 늦춘다.
 * 호출 체인: tg_dispatch_bps_time(), tg_dispatch_iops_time() → [throtl_extend_slice]
 */
static inline void throtl_extend_slice(struct throtl_grp *tg, bool rw,
				       unsigned long jiffy_end)
{
	if (!time_before(tg->slice_end[rw], jiffy_end)) /* [한국어] 이 함수는 '연장'만 한다. 현재 끝이 이미 목표보다 뒤면 아무것도 하지 않는데, 여기서 줄여 버리면 대기 중인 bio가 계산한 대기 시각이 창 밖으로 밀려나 예산이 초기화되고, 결국 설정보다 많은 IO가 나가게 된다 */
		return;

	throtl_set_slice_end(tg, rw, jiffy_end); /* [한국어] 어떤 bio가 jiffy_end까지 기다려야 한다고 계산됐다면, 그 시각까지는 같은 창이 유지돼야 그 계산이 유효하다. 그래서 대기 시간을 산출한 직후 항상 창을 그만큼 늘려 둔다 */
	/* [한국어] 연장은 '어떤 bio가 이만큼 기다려야 한다'는 계산의 부산물이라, 로그를 보면
	 * 어느 시점에 어느 방향이 얼마나 밀렸는지를 슬라이스 경계 변화로 역추적할 수 있다. */
	throtl_log(&tg->service_queue,
		   "[%c] extend slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/*
 * [한국어]
 * throtl_slice_used - 현재 슬라이스가 만료되었는지 판단한다.
 * @tg: 확인할 throtl_grp
 * @rw: READ 또는 WRITE
 * @return: 슬라이스가 만료되었으면 true, 아직 유효하면 false
 *
 * jiffies가 [slice_start, slice_end] 범위 안에 있으면 false.
 * 호출 체인: throtl_trim_slice(), tg_update_slice() → [throtl_slice_used]
 */
/* Determine if previously allocated or extended slice is complete or not */
static bool throtl_slice_used(struct throtl_grp *tg, bool rw)
{
	if (time_in_range(jiffies, tg->slice_start[rw], tg->slice_end[rw])) /* [한국어] 현재 jiffy가 slice 범위 내에 있으면 rate limit 윈도우가 아직 유효 */
		return false;

	return true; /* [한국어] slice 범위 밖이면 만료; 새 slice 시작 필요 */
}

/*
 * [한국어]
 * sq_queued - service_queue의 특정 방향(type)에 대기 중인 bio 총 수를 반환.
 * @sq: 조회할 service_queue
 * @type: READ 또는 WRITE
 * @return: bps 큐 + iops 큐 합산 대기 bio 수
 *
 * 호출 체인: tg_within_limit(), tg_dispatch_time(), throtl_dispatch_tg() → [sq_queued]
 */
static unsigned int sq_queued(struct throtl_service_queue *sq, int type)
{
	return sq->nr_queued_bps[type] + sq->nr_queued_iops[type]; /* [한국어] bps/iops 대기열 합산; 발행 지연 중인 총 bio 수 */
}

/*
 * [한국어]
 * calculate_io_allowed - 경과 시간 동안 허용되는 IO 수를 계산한다.
 * @iops_limit: iops 상한 (회/초)
 * @jiffy_elapsed: 경과 jiffies
 * @return: 허용 IO 수 (UINT_MAX면 사실상 무제한)
 *
 * iops_limit * jiffy_elapsed / HZ. 오버플로 방지를 위해 do_div 사용.
 * 호출 체인: tg_within_iops_limit(), throtl_trim_iops() → [calculate_io_allowed]
 */
static unsigned int calculate_io_allowed(u32 iops_limit,
					 unsigned long jiffy_elapsed)
{
	unsigned int io_allowed; /* [한국어] 최종 결과. iops 상한이 unsigned int라 허용량도 같은 폭으로 맞춘다 */
	u64 tmp; /* [한국어] 중간값은 반드시 64비트여야 한다. iops_limit(최대 ~4e9) × jiffy_elapsed는 32비트를 쉽게 넘긴다 */

	/*
	 * jiffy_elapsed should not be a big value as minimum iops can be
	 * 1 then at max jiffy elapsed should be equivalent of 1 second as we
	 * will allow dispatch after 1 second and after that slice should
	 * have been trimmed.
	 */

	tmp = (u64)iops_limit * jiffy_elapsed; /* [한국어] 먼저 곱하고 나중에 나눈다. 순서를 바꿔 (iops_limit/HZ)×jiffy로 하면 iops_limit이 HZ보다 작을 때 몫이 0이 되어 허용량이 영원히 0이 된다. u64 캐스팅은 32비트 곱셈 오버플로를 막기 위한 것 */
	do_div(tmp, HZ); /* [한국어] jiffies 단위를 초 단위로 환산한다(HZ = 1초당 jiffy 수). do_div을 쓰는 이유는 32비트 아키텍처에서 64비트 나눗셈 연산자가 링크 에러를 내기 때문 - 커널은 라이브러리 헬퍼 대신 이 매크로를 요구한다 */

	if (tmp > UINT_MAX) /* [한국어] 반환 폭이 unsigned int라 그대로 대입하면 잘려서 작은 값이 된다. 잘린 값은 '허용량이 적다'는 뜻이 되어 오히려 IO를 막아 버리므로, 넘치면 최대값으로 포화시킨다 */
		io_allowed = UINT_MAX;
	else
		io_allowed = tmp; /* [한국어] 폭 안에 들어오면 그대로 사용 */

	return io_allowed; /* [한국어] 호출자는 이 값을 io_disp(이미 쓴 개수)와 비교해 통과/대기를 판정한다 */
}

/*
 * [한국어]
 * calculate_bytes_allowed - 경과 시간 동안 허용되는 바이트 수를 계산한다.
 * @bps_limit: bps 상한 (바이트/초)
 * @jiffy_elapsed: 경과 jiffies
 * @return: 허용 바이트 수 (U64_MAX면 사실상 무제한)
 *
 * bps_limit * jiffy_elapsed / HZ. ilog2 합이 62 초과 시 U64_MAX 반환.
 * 호출 체인: tg_within_bps_limit(), throtl_trim_bps(), __tg_update_carryover()
 *           → [calculate_bytes_allowed]
 */
static u64 calculate_bytes_allowed(u64 bps_limit, unsigned long jiffy_elapsed)
{
	/*
	 * Can result be wider than 64 bits?
	 * We check against 62, not 64, due to ilog2 truncation.
	 */
	if (ilog2(bps_limit) + ilog2(jiffy_elapsed) - ilog2(HZ) > 62) /* [한국어] 실제로 곱해 보지 않고 비트 수로 미리 자릿수를 어림한다: log2(a×b/c) ≈ log2(a)+log2(b)-log2(c). ilog2는 내림이라 최대 1비트씩 과소평가될 수 있어, 64가 아니라 여유를 둔 62에서 끊는다 */
		return U64_MAX; /* [한국어] 넘칠 것 같으면 '사실상 무제한'을 뜻하는 U64_MAX를 돌려준다. 호출부는 이 값을 예산이 남아돈다는 뜻으로 받아 bio를 통과시킨다 - 오버플로로 작은 값이 나와 엉뚱하게 IO를 막는 것보다 안전한 쪽으로 실패한다 */
	return mul_u64_u64_div_u64(bps_limit, (u64)jiffy_elapsed, (u64)HZ); /* [한국어] a×b/c를 128비트 중간값으로 계산하는 헬퍼. 그냥 곱했다가 나누면 위에서 걸러지지 않은 구간에서도 정밀도를 잃는다 */
}

/*
 * [한국어]
 * throtl_trim_bps - 경과 시간만큼의 bps 예산을 slice 사용량에서 차감한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @time_elapsed: 차감 기준 경과 jiffies
 * @return: 실제로 차감된 바이트 수
 *
 * bps 제한이 U64_MAX(무제한)이면 0 반환. 경과 시간 허용량과 실제 사용량을
 * 비교해 bytes_disp[rw]를 줄인다.
 * 호출 체인: throtl_trim_slice() → [throtl_trim_bps]
 */
static long long throtl_trim_bps(struct throtl_grp *tg, bool rw,
				 unsigned long time_elapsed)
{
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] 차감량은 "이 창에서 흘러간 시간 × 상한"이므로 상한을 먼저 읽는다 */
	long long bytes_trim; /* [한국어] 부호 있는 타입인 이유: bytes_disp가 carryover 때문에 음수(빚)일 수 있고, calculate_bytes_allowed()의 U64_MAX가 여기 대입되면 음수로 보이게 되는데 아래 <=0 검사가 그 경우까지 함께 걸러 준다 */

	if (bps_limit == U64_MAX) /* [한국어] 상한이 없으면 애초에 bytes_disp를 근거로 무엇을 막고 있지도 않으므로 정리할 것도 없다 */
		return 0; /* [한국어] 0을 돌려주면 호출자는 "bps 쪽은 손댈 게 없었다"로 해석한다 */

	/* Need to consider the case of bytes_allowed overflow. */
	bytes_trim = calculate_bytes_allowed(bps_limit, time_elapsed); /* [한국어] 흘려보낼 시간(time_elapsed)에 해당하는 토큰 양. 이만큼을 사용량에서 빼면, 그 시간을 슬라이스 시작점에서 잘라내는 것과 같아진다 */
	if (bytes_trim <= 0 || tg->bytes_disp[rw] < bytes_trim) { /* [한국어] 두 가지 경우를 한꺼번에 처리한다 - (a) 오버플로로 U64_MAX가 와서 음수로 보이는 경우, (b) 벌어들인 토큰이 쓴 양보다 많은 경우. 어느 쪽이든 사용량을 음수로 만들면 안 되므로 0에서 멈춘다 */
		bytes_trim = tg->bytes_disp[rw]; /* [한국어] 반환값은 '실제로 깎은 양'이어야 한다. 호출자가 이 값으로 slice_start를 얼마나 옮길지 판단하기 때문에, 부풀려 돌려주면 예산이 실제보다 늘어난다 */
		tg->bytes_disp[rw] = 0; /* [한국어] 남은 빚이 없으므로 사용량은 0 */
	} else {
		tg->bytes_disp[rw] -= bytes_trim; /* [한국어] 쓴 양이 더 많으면 그 차이(=아직 갚지 못한 몫)를 남긴다. 이 잔액이 다음 창으로 넘어가 계속 대기 시간을 만든다 */
	}

	return bytes_trim; /* [한국어] 0이 아니면 호출자가 slice_start를 앞으로 밀어도 된다는 신호가 된다 */
}

/*
 * [한국어]
 * throtl_trim_iops - 경과 시간만큼의 iops 예산을 slice 사용량에서 차감한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @time_elapsed: 차감 기준 경과 jiffies
 * @return: 실제로 차감된 IO 수
 *
 * iops 제한이 UINT_MAX이면 0 반환. io_disp[rw]를 경과 시간 허용량만큼 줄인다.
 * 호출 체인: throtl_trim_slice() → [throtl_trim_iops]
 */
static int throtl_trim_iops(struct throtl_grp *tg, bool rw,
			    unsigned long time_elapsed)
{
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] bps 버전과 완전히 같은 구조이며, 단위만 바이트에서 IO 개수로 바뀐다 */
	int io_trim; /* [한국어] 여기서도 부호 있는 타입이어야 UINT_MAX 오버플로 표식이 아래 <=0 검사에 걸린다 */

	if (iops_limit == UINT_MAX) /* [한국어] iops 상한이 없으면 io_disp로 막고 있는 것도 없다 */
		return 0;

	/* Need to consider the case of io_allowed overflow. */
	io_trim = calculate_io_allowed(iops_limit, time_elapsed); /* [한국어] 흘려보낼 시간에 해당하는 IO 토큰 개수 */
	if (io_trim <= 0 || tg->io_disp[rw] < io_trim) { /* [한국어] 오버플로이거나 벌어들인 몫이 쓴 몫보다 많으면 0에서 끊는다 */
		io_trim = tg->io_disp[rw]; /* [한국어] 실제로 깎은 개수만 보고한다 */
		tg->io_disp[rw] = 0; /* [한국어] 빚 없음 */
	} else {
		tg->io_disp[rw] -= io_trim; /* [한국어] 아직 갚지 못한 IO 수를 남긴다 */
	}

	return io_trim; /* [한국어] bps 쪽과 함께 0이면 호출자가 slice_start를 옮기지 않는다 */
}

/*
 * [한국어]
 * throtl_trim_slice - 오래된 slice 사용량을 정리해 평균 rate를 재조정한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * bio 디스패치 직후 또는 직접 통과 후 호출. 2 슬라이스 이상 경과했을 때만
 * 실제 차감. slice_start를 앞당겨 남은 예산을 재계산한다.
 * 호출 체인: __blk_throtl_bio(), tg_dispatch_one_bio() → [throtl_trim_slice]
 */
/* Trim the used slices and adjust slice start accordingly */
static inline void throtl_trim_slice(struct throtl_grp *tg, bool rw)
{
	unsigned long time_elapsed; /* [한국어] 슬라이스 시작점을 앞으로 얼마나 밀지. 그대로 '깎아낼 토큰의 시간 폭'이기도 하다 */
	long long bytes_trim; /* [한국어] 실제로 깎인 바이트 수(로그와 조기 반환 판정에 사용) */
	int io_trim; /* [한국어] 실제로 깎인 IO 수 */

	BUG_ON(time_before(tg->slice_end[rw], tg->slice_start[rw])); /* [한국어] 창의 끝이 시작보다 앞이면 이후 모든 시간 계산이 음수 방향으로 뒤집힌다. 조용히 진행하면 예산이 폭주해 제한이 사실상 사라지므로 여기서 멈춘다 */

	/*
	 * If bps are unlimited (-1), then time slice don't get
	 * renewed. Don't try to trim the slice if slice is used. A new
	 * slice will start when appropriate.
	 */
	if (throtl_slice_used(tg, rw)) /* [한국어] slice가 만료되면 차감하지 않고 새 slice 시작 시점을 기다림; rate 윈도우 재설정 */
		return;

	/*
	 * A bio has been dispatched. Also adjust slice_end. It might happen
	 * that initially cgroup limit was very low resulting in high
	 * slice_end, but later limit was bumped up and bio was dispatched
	 * sooner, then we need to reduce slice_end. A high bogus slice_end
	 * is bad because it does not allow new slice to start.
	 */
	throtl_set_slice_end(tg, rw, jiffies + DFL_THROTL_SLICE); /* [한국어] bio가 디스패치되었으므로 slice 종료 시점을 현재 기준으로 재조정; rate limit 윈도우 보정 */

	time_elapsed = rounddown(jiffies - tg->slice_start[rw], /* [한국어] 슬라이스 격자에 맞춰 '내림'한다. 올림하면 아직 벌지 않은 토큰까지 깎아 준 셈이 되어 설정보다 빠른 rate를 허용하게 된다 */
				 DFL_THROTL_SLICE); /* [한국어] 격자 단위는 슬라이스 길이(100ms). 이 정렬이 있어야 slice_start가 항상 격자 위에 있고, roundup으로 맞춘 slice_end와 어긋나지 않는다 */
	/* Don't trim slice until at least 2 slices are used */
	if (time_elapsed < DFL_THROTL_SLICE * 2) /* [한국어] 최소 2 슬라이스를 모아서 처리하는 이유: 이 함수는 bio 한 개가 나갈 때마다 불리는 핫 경로다. 매번 잘라내면 (a) 계산 비용이 크고 (b) 창이 너무 자주 리셋되어 순간 rate가 튄다. 아래에서 한 칸을 더 빼므로, 실제로 깎이는 시간은 최소 1 슬라이스가 된다 */
		return;

	/*
	 * The bio submission time may be a few jiffies more than the expected
	 * waiting time, due to 'extra_bytes' can't be divided in
	 * tg_within_bps_limit(), and also due to timer wakeup delay. In this
	 * case, adjust slice_start will discard the extra wait time, causing
	 * lower rate than expected. Therefore, other than the above rounddown,
	 * one extra slice is preserved for deviation.
	 */
	time_elapsed -= DFL_THROTL_SLICE; /* [한국어] 한 슬라이스를 일부러 남긴다. 실제 발행 시각은 계산된 대기 시각보다 몇 jiffy 늦기 마련인데(나눗셈에서 버린 나머지 + 타이머 깨우기 지연), 그 늦어진 만큼까지 잘라내 버리면 그 시간에 벌었어야 할 토큰이 사라져 실측 rate가 설정치보다 낮아진다. 위 원본 주석이 말하는 편차 완충 구간이다 */
	bytes_trim = throtl_trim_bps(tg, rw, time_elapsed); /* [한국어] 두 예산을 같은 time_elapsed로 깎아야, 아래에서 slice_start를 한 번 옮기는 것이 양쪽 모두에 대해 정합적이다 */
	io_trim = throtl_trim_iops(tg, rw, time_elapsed); /* [한국어] iops 쪽도 동일한 시간 폭만큼 정리 */
	if (!bytes_trim && !io_trim) /* [한국어] 양쪽 다 깎인 게 없다는 것은 이미 사용량이 0이라는 뜻이다. 이때 slice_start만 앞으로 밀면 아직 쓰지도 않은 구간을 '이미 지나간 것'으로 처리해 버려 앞으로 벌 토큰을 잃는다 */
		return;

	tg->slice_start[rw] += time_elapsed; /* [한국어] 사용량에서 뺀 만큼 시작점도 앞으로 민다. 이 두 조작이 짝을 이뤄야 "경과 시간 × limit - 사용량"이라는 예산 식의 값이 보존된다. 한쪽만 하면 예산이 늘거나 줄어 버린다 */

	/* [한국어] 정리 결과를 남긴다. nr은 몇 슬라이스분을 흘려보냈는지, bytes/io는 실제로 깎인 양,
	 * start/end는 조정 후의 창 경계다. 실측 rate가 설정치와 다를 때 이 줄만 따라가면
	 * 창이 언제 얼마나 밀렸는지 재구성할 수 있다. */
	throtl_log(&tg->service_queue,
		   "[%c] trim slice nr=%lu bytes=%lld io=%d start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', time_elapsed / DFL_THROTL_SLICE,
		   bytes_trim, io_trim, tg->slice_start[rw], tg->slice_end[rw],
		   jiffies);
}

/*
 * [한국어]
 * __tg_update_carryover - 설정 변경 시 이전 제한 하에서 대기한 양을 보정한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 * @bytes: 출력: bps carryover 바이트 수
 * @ios: 출력: iops carryover IO 수
 *
 * cgroup의 bps/iops 설정이 바뀔 때 이미 대기 중인 bio들의 손해/이익을
 * 새 기준에 반영. 큐가 비었으면 bytes_disp/io_disp를 0으로 초기화.
 * 호출 체인: tg_update_carryover() → [__tg_update_carryover]
 */
static void __tg_update_carryover(struct throtl_grp *tg, bool rw,
				  long long *bytes, int *ios)
{
	unsigned long jiffy_elapsed = jiffies - tg->slice_start[rw]; /* [한국어] 지금 창이 열린 뒤 흘러간 시간. 이 시간 동안 '옛 상한으로' 벌었어야 할 양과 실제로 쓴 양의 차이가 곧 이월분이다 */
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] 이 함수는 새 값이 tg->bps[]에 기록되기 '전'에 호출되므로, 여기서 읽히는 것은 아직 옛 상한이다 - 그래서 옛 기준의 정산이 성립한다 */
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] iops도 마찬가지로 옛 상한 */
	long long bytes_allowed; /* [한국어] 옛 상한 기준으로 이 창에서 허용됐던 총 바이트 */
	int io_allowed; /* [한국어] 옛 상한 기준으로 허용됐던 총 IO 개수 */

	/*
	 * If the queue is empty, carryover handling is not needed. In such cases,
	 * tg->[bytes/io]_disp should be reset to 0 to avoid impacting the dispatch
	 * of subsequent bios. The same handling applies when the previous BPS/IOPS
	 * limit was set to max.
	 */
	if (sq_queued(&tg->service_queue, rw) == 0) { /* [한국어] 기다리는 bio가 없으면 '누가 손해를 봤는지' 따질 대상 자체가 없다. 이 경우 옛 기준의 사용량을 그대로 남기면, 새 상한이 훨씬 낮아졌을 때 그 사용량이 빚처럼 남아 다음 bio가 부당하게 오래 기다린다 */
		tg->bytes_disp[rw] = 0; /* [한국어] 깨끗한 상태에서 새 상한을 적용한다 */
		tg->io_disp[rw] = 0; /* [한국어] iops 쪽도 동일 */
		return;
	}

	/*
	 * If config is updated while bios are still throttled, calculate and
	 * accumulate how many bytes/ios are waited across changes. And use the
	 * calculated carryover (@bytes/@ios) to update [bytes/io]_disp, which
	 * will be used to calculate new wait time under new configuration.
	 * And we need to consider the case of bytes/io_allowed overflow.
	 */
	if (bps_limit != U64_MAX) { /* [한국어] 옛 상한이 무제한이었다면 '허용됐던 양'이 사실상 무한이라 차액을 정의할 수 없다. 이때는 *bytes를 0으로 둔 채 넘어가, 아래에서 사용량이 0으로 리셋되게 한다 */
		bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed); /* [한국어] 옛 상한으로 이 창에서 벌었던 총 토큰 */
		if (bytes_allowed > 0) /* [한국어] 오버플로 표식(U64_MAX가 음수로 보이는 경우)을 걸러낸다. 그 경우도 정산을 포기하고 0으로 둔다 */
			*bytes = bytes_allowed - tg->bytes_disp[rw]; /* [한국어] (벌었어야 할 양) - (실제 쓴 양). 양수면 아직 못 쓴 몫이 있다는 뜻이고, 음수면 이미 초과해서 쓴 빚이다 */
	}
	if (iops_limit != UINT_MAX) { /* [한국어] iops 쪽도 같은 구조 */
		io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed); /* [한국어] 옛 상한으로 벌었던 IO 개수 */
		if (io_allowed > 0) /* [한국어] 오버플로 방어 */
			*ios = io_allowed - tg->io_disp[rw]; /* [한국어] 남은 몫(양수) 또는 빚(음수) */
	}

	tg->bytes_disp[rw] = -*bytes; /* [한국어] 부호를 뒤집어 사용량 칸에 다시 넣는 것이 요령이다. 예산 식이 "경과×limit - 사용량"이므로, 사용량에 음수를 넣으면 그만큼 예산이 늘어난 것과 같다. 즉 못 쓴 몫(*bytes>0)은 사용량 음수 = 선불 크레딧이 되고, 빚(*bytes<0)은 사용량 양수 = 새 상한 아래에서 갚아야 할 몫이 된다 */
	tg->io_disp[rw] = -*ios;      /* [한국어] iops 쪽도 같은 방식으로 새 기준에 이월 */
}

/*
 * [한국어]
 * tg_update_carryover - READ/WRITE 양쪽에 대해 carryover를 갱신한다.
 * @tg: 대상 throtl_grp
 *
 * tg_set_conf() 또는 tg_set_limit()에서 limit 변경 직전에 호출.
 * 호출 체인: tg_set_conf(), tg_set_limit() → [tg_update_carryover] → __tg_update_carryover()
 */
static void tg_update_carryover(struct throtl_grp *tg)
{
	long long bytes[2] = {0}; /* [한국어] READ/WRITE bps carryover 임시 저장 */
	int ios[2] = {0}; /* [한국어] READ/WRITE iops carryover 임시 저장 */

	__tg_update_carryover(tg, READ, &bytes[READ], &ios[READ]); /* [한국어] READ 방향 carryover 계산 및 bytes_disp/io_disp 업데이트 */
	__tg_update_carryover(tg, WRITE, &bytes[WRITE], &ios[WRITE]); /* [한국어] WRITE 방향 carryover 계산 및 bytes_disp/io_disp 업데이트 */

	/* see comments in struct throtl_grp for meaning of carryover. */
	/* [한국어] 네 값은 순서대로 READ 바이트 / WRITE 바이트 / READ IO / WRITE IO 이월분이다.
	 * 부호가 핵심이다 - 양수면 새 상한에서 미리 써도 되는 크레딧, 음수면 갚아야 할 빚.
	 * 설정을 바꾼 직후 IO가 한동안 멈춰 보이거나 반대로 확 몰리는 현상을 이 줄로 설명할 수 있다. */
	throtl_log(&tg->service_queue, "%s: %lld %lld %d %d\n", __func__,
		   bytes[READ], bytes[WRITE], ios[READ], ios[WRITE]); /* [한국어] carryover 결과 blktrace 로그 */
}

/*
 * [한국어]
 * tg_within_iops_limit - bio 하나가 현재 iops slice 안에 들어갈 수 있는지 검사.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @iops_limit: iops 상한
 * @return: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 *
 * io_disp[rw] + 1 <= 허용 IO 수이면 0 반환. 초과 시 다음 슬라이스 경계까지의
 * 시간을 반환. 최소 1 IO 보장 로직 포함.
 * 호출 체인: tg_dispatch_iops_time(), tg_within_limit() → [tg_within_iops_limit]
 */
static unsigned long tg_within_iops_limit(struct throtl_grp *tg, struct bio *bio,
				 u32 iops_limit)
{
	bool rw = bio_data_dir(bio); /* [한국어] 예산은 방향별로 완전히 분리되어 있다 */
	int io_allowed; /* [한국어] 올림한 시간 창에서 허용되는 IO 개수 */
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd; /* [한국어] 각각 실제 경과 시간 / 반환할 대기 시간 / 슬라이스 경계로 올린 경과 시간 */

	jiffy_elapsed = jiffies - tg->slice_start[rw]; /* [한국어] 창이 열린 뒤 흐른 실제 시간 */

	/* Round up to the next throttle slice, wait time must be nonzero */
	jiffy_elapsed_rnd = roundup(jiffy_elapsed + 1, DFL_THROTL_SLICE); /* [한국어] +1을 먼저 더하고 올리는 것이 핵심이다. 경과 시간이 정확히 슬라이스 경계에 놓였을 때 그냥 올리면 값이 그대로라, 아래 jiffy_wait = rnd - elapsed 가 0이 되어 "기다리라고 해놓고 0을 반환"하는 모순이 생긴다. +1 덕분에 항상 다음 경계로 넘어가 대기 시간이 0보다 커진다 */
	io_allowed = calculate_io_allowed(iops_limit, jiffy_elapsed_rnd); /* [한국어] 실제 경과가 아니라 '다음 경계까지'를 기준으로 계산한다 - 즉 이 창에서 앞으로 벌게 될 몫까지 미리 인정해 준다. 이 선지급이 없으면 상한이 낮을 때 첫 bio조차 나가지 못한다 */
	if (io_allowed > 0 && tg->io_disp[rw] + 1 <= io_allowed) /* [한국어] +1은 지금 판정 중인 이 bio 자신이다. io_disp는 carryover 때문에 음수일 수 있어서, 부호 있는 비교로 다뤄야 한다 */
		return 0; /* [한국어] 0 = 기다릴 필요 없음. 호출자는 이 값을 그대로 disptime 계산에 쓴다 */

	/* Calc approx time to dispatch */
	jiffy_wait = jiffy_elapsed_rnd - jiffy_elapsed; /* [한국어] 기본 대기 시간은 '다음 슬라이스 경계까지'다. 경계를 넘어가면 그만큼 토큰이 더 쌓이기 때문 */

	/* make sure at least one io can be dispatched after waiting */
	jiffy_wait = max(jiffy_wait, HZ / iops_limit + 1); /* [한국어] IO 하나당 걸리는 시간이 HZ/iops_limit jiffy다. 상한이 아주 낮으면(예: 1 iops) 슬라이스 경계까지 기다려 봐야 토큰이 1개도 안 차서, 깨어났다가 다시 자기를 반복하며 타이머만 낭비한다. +1은 정수 나눗셈에서 버린 나머지 때문에 아슬아슬하게 모자라는 것을 막는 여유분 */
	return jiffy_wait; /* [한국어] 호출자(tg_dispatch_iops_time)는 이 시간만큼 슬라이스를 연장한 뒤 값을 위로 올린다 */
}

/*
 * [한국어]
 * tg_within_bps_limit - bio 하나가 현재 bps slice 안에 들어갈 수 있는지 검사.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @bps_limit: bps 상한
 * @return: 기다려야 할 jiffies. 0이면 즉시 디스패치 가능.
 *
 * bytes_disp[rw] + bio_size <= 허용 바이트이면 0 반환. 초과 시 대기 시간 계산.
 * 슬라이스 시작 직후에는 DFL_THROTL_SLICE를 기준으로 계산.
 * 호출 체인: tg_dispatch_bps_time(), tg_within_limit() → [tg_within_bps_limit]
 */
static unsigned long tg_within_bps_limit(struct throtl_grp *tg, struct bio *bio,
				u64 bps_limit)
{
	bool rw = bio_data_dir(bio); /* [한국어] 방향별로 독립된 예산 */
	long long bytes_allowed; /* [한국어] 부호 있는 타입: calculate_bytes_allowed()가 오버플로 시 돌려주는 U64_MAX가 여기서 음수로 보이고, 아래에서 그 음수를 '무제한' 신호로 쓴다 */
	u64 extra_bytes; /* [한국어] 예산을 넘긴 바이트 수 */
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd; /* [한국어] 실제 경과 / 반환할 대기 시간 / 올림한 경과 */
	unsigned int bio_size = throtl_bio_data_size(bio); /* [한국어] bps에 반영할 크기. DISCARD는 여기서 512B로 축소돼 대역폭 예산을 통째로 삼키지 않는다 */

	jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw]; /* [한국어] 두 변수를 같은 값으로 출발시킨 뒤, rnd 쪽만 아래에서 보정한다 */

	/* Slice has just started. Consider one slice interval */
	if (!jiffy_elapsed) /* [한국어] 창이 방금 열려 경과가 0이면 허용량도 0이 되어, 크기와 상관없이 모든 bio가 대기하게 된다 */
		jiffy_elapsed_rnd = DFL_THROTL_SLICE; /* [한국어] 그래서 최소 한 슬라이스분(100ms × bps)은 미리 인정해 준다. 이것이 blk-throttle이 허용하는 버스트의 상한이기도 하다 */

	jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, DFL_THROTL_SLICE); /* [한국어] 예산 계산은 항상 슬라이스 격자 위에서 한다. 매 jiffy마다 예산이 조금씩 늘어나면 대기 시간이 계속 흔들리는데, 격자에 맞추면 창 안에서 판정이 안정적이다 */
	bytes_allowed = calculate_bytes_allowed(bps_limit, jiffy_elapsed_rnd); /* [한국어] 이 창에서 쓸 수 있는 총 바이트 */
	/* Need to consider the case of bytes_allowed overflow. */
	if ((bytes_allowed > 0 && tg->bytes_disp[rw] + bio_size <= bytes_allowed) /* [한국어] 이미 쓴 양 + 이번 bio가 예산 안에 들어오면 통과 */
	    || bytes_allowed < 0) /* [한국어] 음수 = u64 U64_MAX가 부호 있는 타입에 담긴 것 = 예산이 64비트를 넘칠 만큼 크다는 뜻이므로 무조건 통과시킨다 */
		return 0;

	/* Calc approx time to dispatch */
	extra_bytes = tg->bytes_disp[rw] + bio_size - bytes_allowed; /* [한국어] 예산을 얼마나 넘겼는가. 이 초과분을 상한으로 나누면 '그만큼 더 벌기까지 걸리는 시간'이 나온다 */
	jiffy_wait = div64_u64(extra_bytes * HZ, bps_limit); /* [한국어] (초과 바이트 / bps)를 jiffy 단위로: ×HZ를 먼저 해야 1초 미만 구간에서 몫이 0으로 죽지 않는다. div64_u64는 32비트 아키텍처에서도 안전한 64비트 나눗셈 헬퍼 */

	if (!jiffy_wait) /* [한국어] 정수 나눗셈에서 1 jiffy 미만이 0으로 떨어진 경우 */
		jiffy_wait = 1; /* [한국어] 0을 돌려주면 호출자는 '통과'로 해석하는데, 여기까지 왔다는 것은 예산을 넘겼다는 뜻이라 모순이다. 최소 1 jiffy는 반드시 기다리게 한다 */

	/*
	 * This wait time is without taking into consideration the rounding
	 * up we did. Add that time also.
	 */
	jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed); /* [한국어] 위 계산은 '올림한 시점'을 기준으로 한 것이므로, 실제 현재 시각에서 그 기준점까지 남은 시간을 더해야 절대 대기 시간이 된다. 이걸 빠뜨리면 아직 벌지 않은 토큰을 이미 있는 것처럼 세어 설정보다 빠른 rate가 나온다 */
	return jiffy_wait; /* [한국어] 호출자는 이 시간만큼 슬라이스를 연장하고, disptime = jiffies + 이 값으로 타이머를 잡는다 */
}

/*
 * [한국어]
 * throtl_charge_bps_bio - bio의 바이트를 bps 사용량(bytes_disp)에 기록한다.
 * @tg: 대상 throtl_grp
 * @bio: 과금할 bio
 *
 * BIO_BPS_THROTTLED 또는 BIO_TG_BPS_THROTTLED가 이미 설정된 분할 bio는
 * 중복 과금하지 않음. BIO_TG_BPS_THROTTLED 플래그를 설정해 iops 경로로만 가도록.
 * 호출 체인: tg_dispatch_time(), tg_within_limit(), tg_dispatch_one_bio()
 *           → [throtl_charge_bps_bio]
 */
static void throtl_charge_bps_bio(struct throtl_grp *tg, struct bio *bio)
{
	unsigned int bio_size = throtl_bio_data_size(bio); /* [한국어] 판정에 썼던 것과 같은 크기 산정식을 써야 한다. 판정과 과금의 기준이 다르면 예산이 서서히 어긋난다 */

	/* Charge the bio to the group */
	if (!bio_flagged(bio, BIO_BPS_THROTTLED) && /* [한국어] 이 bio는 tg_within_limit / tg_dispatch_time / tg_dispatch_one_bio 등 여러 지점에서 과금 시도를 받는다. 플래그가 '이미 냈다'는 유일한 증거이며, 없으면 같은 bio가 두 번 차감돼 실측 대역폭이 설정치의 절반으로 떨어진다 */
	    !bio_flagged(bio, BIO_TG_BPS_THROTTLED)) { /* [한국어] 하나는 트리 전체 통과 표식, 하나는 현재 tg에서의 과금 표식이다. 둘 중 아무거나 있으면 중복이다 */
		bio_set_flag(bio, BIO_TG_BPS_THROTTLED); /* [한국어] 표식을 먼저 남기고 값을 더한다. 이후 이 bio는 bps 큐를 건너뛰고 iops 큐로 직행한다 */
		tg->bytes_disp[bio_data_dir(bio)] += bio_size; /* [한국어] 사용량 누적. 이 값은 throtl_trim_slice()가 시간 경과에 맞춰 다시 깎아 준다 */
	}
}

/*
 * [한국어]
 * throtl_charge_iops_bio - bio 하나를 iops 사용량(io_disp)에 기록한다.
 * @tg: 대상 throtl_grp
 * @bio: 과금할 bio
 *
 * io_disp[rw]를 1 증가시키고 BIO_TG_BPS_THROTTLED 플래그를 클리어.
 * 호출 체인: tg_dispatch_one_bio(), __blk_throtl_bio() → [throtl_charge_iops_bio]
 */
static void throtl_charge_iops_bio(struct throtl_grp *tg, struct bio *bio)
{
	bio_clear_flag(bio, BIO_TG_BPS_THROTTLED); /* [한국어] bps 통과 플래그 클리어; iops 단계에서 초당 IO 수만 계산 */
	tg->io_disp[bio_data_dir(bio)]++; /* [한국어] 크기와 무관하게 bio 하나를 1로 센다 - iops 제한의 단위가 '개수'이기 때문이다. 4KB든 1MB든 여기서는 똑같이 1이다 */
}

/*
 * [한국어]
 * tg_update_slice - bio 디스패치 직전 슬라이스 상태를 갱신한다.
 * @tg: 대상 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * 슬라이스가 만료되고 큐가 비면 새 슬라이스 시작, 그 외엔 연장.
 * 호출 체인: tg_dispatch_bps_time(), tg_dispatch_iops_time() → [tg_update_slice]
 */
static void tg_update_slice(struct throtl_grp *tg, bool rw)
{
	if (throtl_slice_used(tg, rw) && /* [한국어] 창이 만료됐다는 것만으로는 리셋 근거가 못 된다 */
	    sq_queued(&tg->service_queue, rw) == 0) /* [한국어] 대기 중인 bio가 있는데 창을 새로 열면 사용량이 0이 되어, 이미 예산을 초과해 기다리던 bio들이 공짜로 통과한다. 즉 '아무도 안 기다리는 조용한 상태'에서만 리셋이 안전하다 */
		throtl_start_new_slice(tg, rw, true); /* [한국어] 놀고 있던 그룹은 깨끗한 창에서 다시 시작한다 */
	else
		throtl_extend_slice(tg, rw, jiffies + DFL_THROTL_SLICE); /* [한국어] 그 외에는 창을 리셋하지 않고 끝만 늘린다. 사용량과 slice_start를 그대로 두어야 지금까지의 누적 판정이 유지된다 */
}

/*
 * [한국어]
 * tg_dispatch_bps_time - bio가 bps 제한을 만족하는지 검사하고 대기 시간 반환.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @return: 대기 jiffies. 0이면 bps 제한 통과.
 *
 * bps 무제한/THROTL_TG_CANCELING/이미 통과한 bio는 즉시 0 반환.
 * 슬라이스 갱신 후 tg_within_bps_limit()으로 검사.
 * 호출 체인: tg_dispatch_time(), tg_within_limit() → [tg_dispatch_bps_time]
 */
static unsigned long tg_dispatch_bps_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio); /* [한국어] 방향별 상한과 예산 */
	u64 bps_limit = tg_bps_limit(tg, rw); /* [한국어] cgroup v2 루트에서는 항상 U64_MAX가 나와 아래에서 곧바로 통과된다 */
	unsigned long bps_wait; /* [한국어] 이 bio가 bps 때문에 기다려야 할 jiffy */

	/* no need to throttle if this bio's bytes have been accounted */
	if (bps_limit == U64_MAX || tg->flags & THROTL_TG_CANCELING || /* [한국어] CANCELING은 디스크가 사라지는 중이라는 뜻이다. 이때까지 rate를 지키겠다고 bio를 붙잡으면 del_gendisk()가 영원히 끝나지 않으므로, 제한을 무시하고 전부 흘려보낸다 */
	    bio_flagged(bio, BIO_BPS_THROTTLED) || /* [한국어] 이미 트리 전체의 bps 관문을 통과한 bio */
	    bio_flagged(bio, BIO_TG_BPS_THROTTLED)) /* [한국어] 이 tg에서 이미 바이트를 차감한 bio. 다시 검사하면 자기가 낸 사용량 때문에 스스로 막히는 자기 봉쇄가 일어난다 */
		return 0;

	tg_update_slice(tg, rw); /* [한국어] 판정 전에 창을 정리한다. 만료된 창을 그대로 두고 계산하면 아주 오래전 slice_start 때문에 예산이 비현실적으로 커진다 */
	bps_wait = tg_within_bps_limit(tg, bio, bps_limit); /* [한국어] 실제 예산 계산 */
	throtl_extend_slice(tg, rw, jiffies + bps_wait); /* [한국어] 계산한 대기 시각까지는 같은 창이 유지돼야 그 계산이 성립한다. 연장을 빼먹으면 그 사이에 창이 만료되어 예산이 리셋되고, bio가 예정보다 일찍 나가 rate가 초과된다 */

	return bps_wait; /* [한국어] 0이면 bps 관문 통과 */
}

/*
 * [한국어]
 * tg_dispatch_iops_time - bio가 iops 제한을 만족하는지 검사하고 대기 시간 반환.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio
 * @return: 대기 jiffies. 0이면 iops 제한 통과.
 *
 * iops 무제한/THROTL_TG_CANCELING이면 즉시 0 반환.
 * 슬라이스 갱신 후 tg_within_iops_limit()으로 검사.
 * 호출 체인: tg_dispatch_time(), tg_within_limit() → [tg_dispatch_iops_time]
 */
static unsigned long tg_dispatch_iops_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio); /* [한국어] 방향별 상한과 예산 */
	u32 iops_limit = tg_iops_limit(tg, rw); /* [한국어] 무제한 표식은 UINT_MAX */
	unsigned long iops_wait; /* [한국어] iops 때문에 기다려야 할 jiffy */

	if (iops_limit == UINT_MAX || tg->flags & THROTL_TG_CANCELING) /* [한국어] bps 쪽과 달리 '이미 과금됨' 플래그 검사가 없다. iops 과금(throtl_charge_iops_bio)은 bio가 실제로 상위로 올라가는 순간 딱 한 번 일어나고, 판정 시점에는 아직 세지 않았기 때문에 중복 걱정이 없다 */
		return 0;

	tg_update_slice(tg, rw); /* [한국어] bps와 같은 창을 공유하므로 여기서도 창을 먼저 정리한다 */
	iops_wait = tg_within_iops_limit(tg, bio, iops_limit); /* [한국어] 개수 기준 예산 계산 */
	throtl_extend_slice(tg, rw, jiffies + iops_wait); /* [한국어] 계산된 대기 시각까지 창 유지 */

	return iops_wait; /* [한국어] 0이면 iops 관문 통과 */
}

/*
 * [한국어]
 * tg_dispatch_time - bio가 현재 tg에서 디스패치 가능할 때까지의 대기 시간 계산.
 * @tg: 대상 throtl_grp
 * @bio: 검사할 bio (큐의 첫 번째 bio여야 함)
 * @return: 대기 jiffies. 0이면 즉시 디스패치 가능.
 *
 * bps를 먼저 검사하고 통과 시 bps 과금 후 iops를 검사. 큐에 bio가 있으면
 * 큐의 첫 bio와 동일한 bio인지 BUG_ON으로 검증.
 * 호출 체인: throtl_dispatch_tg(), tg_update_disptime() → [tg_dispatch_time]
 *   → tg_dispatch_bps_time() → tg_dispatch_iops_time()
 *
 * 관문의 순서가 bps → iops인 데는 이유가 있다. bps를 통과한 bio는 곧바로
 * iops 큐로 옮겨지므로(qnode의 bios_bps/bios_iops 분리) bps 판정은 bio당
 * 정확히 한 번만 일어나야 하고, 그래서 여기서 통과 즉시 과금까지 마친 뒤
 * iops 판정으로 넘긴다. 순서를 뒤집으면 iops에 걸려 대기하는 동안 bps를
 * 반복 판정하게 되어, 같은 bio가 여러 번 예산을 갉아먹는다.
 *
 * 반환값은 '이 그룹에서 이 bio가 나갈 수 있게 되기까지의 jiffy'이며,
 * 호출자는 이를 disptime(= jiffies + wait)으로 바꿔 pending_tree 정렬 키로
 * 쓴다.
 */
static unsigned long tg_dispatch_time(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio); /* [한국어] 아래 BUG_ON에서 같은 방향 큐의 선두와 비교하기 위해 필요 */
	unsigned long wait; /* [한국어] bps 단계에서 나온 대기 시간. 0이면 iops 단계로 넘어간다 */

	/*
 	 * Currently whole state machine of group depends on first bio
	 * queued in the group bio list. So one should not be calling
	 * this function with a different bio if there are other bios
	 * queued.
	 */
	BUG_ON(sq_queued(&tg->service_queue, rw) && /* [한국어] 그룹의 상태 기계 전체가 '선두 bio 하나'를 기준으로 돌아간다: disptime도 선두 bio로 계산하고, 슬라이스 연장도 선두 bio 기준으로 한다 */
	       bio != throtl_peek_queued(&tg->service_queue.queued[rw])); /* [한국어] 대기열이 있는데 선두가 아닌 bio로 이 함수를 부르면, 그 bio 기준으로 계산된 disptime이 실제로 먼저 나갈 bio와 어긋나 그룹이 영영 안 깨어나거나 반대로 계속 헛깨어난다. 조용히 틀리는 것보다 즉시 멈추는 편이 낫다 */

	wait = tg_dispatch_bps_time(tg, bio); /* [한국어] 1단계: 대역폭 관문 */
	if (wait != 0) /* [한국어] bps에서 이미 막혔으면 iops는 볼 필요가 없다. 어차피 더 늦은 쪽이 아니라 '먼저 막힌 쪽'의 시간만큼 기다린 뒤 다시 판정하기 때문 */
		return wait;

	/*
	 * Charge bps here because @bio will be directly placed into the
	 * iops queue afterward.
	 */
	throtl_charge_bps_bio(tg, bio); /* [한국어] bps 통과 후 사용량 기록; 이제 iops 제한만 남음 */

	return tg_dispatch_iops_time(tg, bio); /* [한국어] 2단계: 개수 관문. 여기 반환값이 곧 이 함수의 결과가 된다 */
}

/**
 * throtl_add_bio_tg - add a bio to the specified throtl_grp
 * @bio: bio to add
 * @qn: qnode to use
 * @tg: the target throtl_grp
 *
 * Add @bio to @tg's service_queue using @qn.  If @qn is not specified,
 * tg->qnode_on_self[] is used.
 *
 * [한국어]
 * 제한을 넘긴 bio가 실제로 '붙잡히는' 지점이다. 여기 들어온 bio는 시간이
 * 지나 disptime이 도래할 때까지 아래로 내려가지 않는다.
 *
 * @qn이 나뉘는 이유: 이 그룹에 직접 도착한 bio는 tg->qnode_on_self[]에,
 * 자식에서 올라온 bio는 그 자식의 tg->qnode_on_parent[]에 담긴다. 같은
 * 부모 큐에 여러 자식이 각자의 슬롯으로 매달려 있어야, throtl_pop_queued()가
 * 슬롯을 돌려가며 형제 간 공평성을 만들 수 있다. 슬롯을 하나로 합치면
 * 먼저 들어온 자식이 큐를 독점한다.
 *
 * 두 개의 WAS_EMPTY 플래그는 "이 그룹의 disptime이 지금 값 그대로면 안
 * 된다"는 1회성 신호다. 큐가 비어 있는 동안 disptime은 갱신되지 않은 채
 * 과거에 머물러 있으므로, 첫 bio가 들어온 순간 재계산과 타이머 재예약을
 * 강제해야 그 bio가 제때 깨어난다.
 */
static void throtl_add_bio_tg(struct bio *bio, struct throtl_qnode *qn,
			      struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	bool rw = bio_data_dir(bio); /* bio의 READ/WRITE 방향; 장치 큐 방향 */

	if (!qn) /* 호출자가 특정 qnode를 지정하지 않으면 자신의 qnode 사용 */
		qn = &tg->qnode_on_self[rw]; /* 자신의 service_queue qnode; 발행 전 bio 대기 위치 */

	/*
	 * If @tg doesn't currently have any bios queued in the same
	 * direction, queueing @bio can change when @tg should be
	 * dispatched.  Mark that @tg was empty.  This is automatically
	 * cleared on the next tg_update_disptime().
	 */
	if (sq_queued(sq, rw) == 0) /* [한국어] 같은 방향의 큐가 비어 있었다면 첫 bio 도착; dispatch 시점 재계산 필요 */
		tg->flags |= THROTL_TG_WAS_EMPTY; /* [한국어] 하위 계층으로 갈 bio가 생겼음을 표시 */

	throtl_qnode_add_bio(bio, qn, sq); /* [한국어] bio를 throtl 큐에 추가; 하위 계층으로의 유입을 일시 지연 */

	/*
	 * Since we have split the queues, when the iops queue is
	 * previously empty and a new @bio is added into the first @qn,
	 * we also need to update the @tg->disptime.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED) && /* [한국어] 분할 bio가 bps 큐에서 iops 큐로 넘어올 때 첫 bio라면 dispatch 시점 갱신 */
	    bio == throtl_peek_queued(&sq->queued[rw]))
		tg->flags |= THROTL_TG_IOPS_WAS_EMPTY; /* [한국어] iops 큐도 갱신 필요 */

	throtl_enqueue_tg(tg); /* [한국어] disptime 기준 pending_tree에 등록 */
}

/*
 * [한국어]
 * tg_update_disptime - 그룹의 다음 디스패치 시각을 계산하고 pending_tree를
 * 재정렬한다.
 * @tg: 대상 throtl_grp
 *
 * READ/WRITE 큐의 첫 bio에 대해 tg_dispatch_time()을 호출.
 * 반환된 대기 시간을 절대 시각으로 바꿔 disptime에 넣고, 트리에서 빼었다가
 * 다시 넣어 정렬을 유지한다. disptime이 가장 작은 그룹이 leftmost가 되어
 * 다음 타이머 만료 시점을 결정한다.
 * 호출 체인: __blk_throtl_bio(), throtl_dispatch_tg(), tg_flush_bios()
 *           → [tg_update_disptime] → tg_dispatch_time()
 */
static void tg_update_disptime(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] 대기 중인 bio를 들여다볼 이 그룹의 큐 */
	unsigned long read_wait = -1, write_wait = -1, min_wait, disptime; /* [한국어] 초기값 -1은 unsigned에서 ULONG_MAX가 된다. 즉 '해당 방향에는 대기 bio가 없음'을 아래 min()에서 자동으로 지는 값으로 표현한 것 */
	struct bio *bio; /* [한국어] 각 방향의 선두 bio(꺼내지 않고 들여다보기만 한다) */

	bio = throtl_peek_queued(&sq->queued[READ]); /* [한국어] 그룹 상태는 선두 bio 하나로 대표된다 */
	if (bio)
		read_wait = tg_dispatch_time(tg, bio); /* [한국어] READ 선두가 나갈 수 있게 되기까지의 시간 */

	bio = throtl_peek_queued(&sq->queued[WRITE]); /* [한국어] WRITE 방향도 독립적으로 본다 */
	if (bio)
		write_wait = tg_dispatch_time(tg, bio); /* [한국어] WRITE 선두의 대기 시간 */

	min_wait = min(read_wait, write_wait); /* [한국어] 둘 중 '먼저 준비되는 쪽'에 맞춰 깨어난다. 늦은 쪽에 맞추면 준비된 방향이 그동안 놀게 되고, 한쪽만 보면 다른 방향이 굶는다. 양쪽 다 비어 있으면 ULONG_MAX가 남아 disptime이 아주 먼 미래가 되지만, 그 경우 이 그룹은 곧 pending_tree에서 빠진다 */
	disptime = jiffies + min_wait; /* [한국어] 상대 시간을 절대 시각으로 바꾼다. pending_tree의 정렬 키가 절대 시각이어야 형제끼리 비교가 가능하다 */

	/* Update dispatch time */
	throtl_rb_erase(&tg->rb_node, tg->service_queue.parent_sq); /* [한국어] RB 트리는 키가 바뀌면 위치도 바뀌어야 한다. 제자리에서 disptime만 고치면 정렬이 깨져 leftmost가 실제 최소가 아니게 된다. 그래서 빼고 → 고치고 → 다시 넣는 3단계를 지킨다 */
	tg->disptime = disptime; /* [한국어] 트리 밖에 있는 동안에만 키를 바꾼다 */
	tg_service_queue_add(tg); /* [한국어] 새 키로 다시 삽입. 이때 leftmost 캐시도 함께 갱신된다 */

	/* see throtl_add_bio_tg() */
	tg->flags &= ~THROTL_TG_WAS_EMPTY; /* [한국어] 재계산을 요구하던 신호를 여기서 소비한다. 지우지 않으면 이후 디스패치 경로가 매번 불필요하게 강제 재예약을 걸어 타이머가 계속 흔들린다 */
	tg->flags &= ~THROTL_TG_IOPS_WAS_EMPTY; /* [한국어] iops 큐용 신호도 같이 소비 */
}

/*
 * [한국어]
 * start_parent_slice_with_credit - 부모 tg의 슬라이스를 credit 포함 재시작한다.
 * @child_tg: 자식 throtl_grp (슬라이스 시작 시점 참고용)
 * @parent_tg: 부모 throtl_grp
 * @rw: READ 또는 WRITE
 *
 * 부모 슬라이스가 만료됐을 때만 credit 이월 재시작. 자식의 slice_start를
 * 부모의 새 시작 시점으로 사용.
 * 호출 체인: tg_dispatch_one_bio() → [start_parent_slice_with_credit]
 */
static void start_parent_slice_with_credit(struct throtl_grp *child_tg,
					struct throtl_grp *parent_tg, bool rw)
{
	if (throtl_slice_used(parent_tg, rw)) { /* [한국어] 부모 창이 살아 있으면 손대지 않는다. 살아 있는 창을 다시 열면 부모의 누적 사용량이 지워져 계층 제한이 헐거워진다 */
		throtl_start_new_slice_with_credit(parent_tg, rw, /* [한국어] 부모 창이 만료됐다는 것은 그동안 부모 쪽으로 아무 IO도 올라오지 않았다는 뜻이다. 그런데 그 시간 동안 자식은 자기 제한에 걸려 기다리고 있었다 - 그 대기 구간은 부모 입장에서도 '쓰지 않은 대역폭'이므로 크레딧으로 돌려준다 */
				child_tg->slice_start[rw]); /* [한국어] 기준점은 자식이 기다리기 시작한 시각이다. 이 값을 부모의 slice_start로 삼으면 그 구간만큼의 토큰이 부모에게 즉시 생긴다 */
	}

}

/*
 * [한국어]
 * tg_dispatch_one_bio - bio 하나를 이 그룹에서 부모 service_queue로 올린다
 * @tg: bio를 꺼낼 그룹
 * @rw: READ 또는 WRITE
 *
 * "디스패치"라고 하지만 실제로 장치에 내보내는 것이 아니라 계층을 한 칸
 * 올리는 것뿐이다. cgroup 트리의 깊이만큼 이 함수가 반복되어야 bio가 루트에
 * 닿고, 루트(td->service_queue)에 닿은 bio만 나중에
 * blk_throtl_dispatch_work_fn()이 실제로 발행한다. 계층 제한이 곱해지는
 * (자식 제한 ∩ 조상 제한) 구조가 이 반복에서 나온다.
 *
 * 실행 컨텍스트: queue_lock 보유 + IRQ off (타이머 콜백 또는 제출 경로).
 *
 * 호출 체인:
 *   throtl_dispatch_tg() → [tg_dispatch_one_bio]
 *     → throtl_pop_queued(), throtl_charge_iops_bio(),
 *       throtl_add_bio_tg() 또는 throtl_qnode_add_bio(), throtl_trim_slice()
 */
static void tg_dispatch_one_bio(struct throtl_grp *tg, bool rw)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] bio를 꺼낼 곳 */
	struct throtl_service_queue *parent_sq = sq->parent_sq; /* [한국어] bio를 넣을 곳. 루트 그룹이면 여기가 throtl_data의 sq다 */
	struct throtl_grp *parent_tg = sq_to_tg(parent_sq); /* [한국어] NULL이면 부모가 루트라는 뜻이고, 그때는 아래에서 '발행 준비 완료' 처리로 갈라진다 */
	struct throtl_grp *tg_to_put = NULL; /* [한국어] pop이 참조 해제를 여기로 미뤄 둘 수 있게 하는 자리. 아래 원본 주석이 설명하듯, 참조를 pop 시점에 내리면 이어지는 bio 이동 중에 sq가 해제될 수 있다 */
	struct bio *bio; /* [한국어] 이번에 한 칸 올릴 bio */

	/*
	 * @bio is being transferred from @tg to @parent_sq.  Popping a bio
	 * from @tg may put its reference and @parent_sq might end up
	 * getting released prematurely.  Remember the tg to put and put it
	 * after @bio is transferred to @parent_sq.
	 */
	bio = throtl_pop_queued(sq, &tg_to_put, rw); /* [한국어] 참조 해제를 미루기 위해 두 번째 인자를 넘긴다 */

	throtl_charge_iops_bio(tg, bio); /* [한국어] iops는 '나갈 때' 센다. 판정 시점이 아니라 실제로 한 칸 올라가는 이 순간에 세야 bio 하나가 정확히 한 번만 계수된다. 이 함수는 BIO_TG_BPS_THROTTLED도 함께 지워, 위 계층에서 bps 판정을 다시 받을 수 있게 한다 */

	/*
	 * If our parent is another tg, we just need to transfer @bio to
	 * the parent using throtl_add_bio_tg().  If our parent is
	 * @td->service_queue, @bio is ready to be issued.  Put it on its
	 * bio_lists[] and decrease total number queued.  The caller is
	 * responsible for issuing these bios.
	 */
	if (parent_tg) { /* [한국어] 위에 아직 cgroup이 더 있는 경우: bio는 그 그룹의 제한을 다시 받아야 한다 */
		throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg); /* [한국어] 내 몫의 슬롯(qnode_on_parent)으로 부모 큐에 매단다. 부모 입장에서 이 슬롯이 '자식 tg 하나'를 대표하며, 부모는 슬롯들을 돌려가며 꺼내 형제 간 공평성을 유지한다 */
		start_parent_slice_with_credit(tg, parent_tg, rw); /* [한국어] 자식이 기다리는 동안 부모 창이 놀았다면 그 몫을 부모에게 크레딧으로 넘긴다 */
	} else { /* [한국어] 부모가 루트(디스크 단위 큐)인 경우: 더 검사할 계층이 없다 */
		bio_set_flag(bio, BIO_BPS_THROTTLED); /* [한국어] '모든 계층의 bps 관문을 통과했다'는 최종 표식. 이 bio가 나중에 분할(split)되어 다시 들어와도 이 플래그 덕분에 bps를 두 번 내지 않는다 */
		throtl_qnode_add_bio(bio, &tg->qnode_on_parent[rw],
				     parent_sq); /* [한국어] 루트 큐에 넣는다. 여기 있는 bio는 이미 발행 자격을 얻었고, blk_throtl_dispatch_work_fn()이 꺼내 갈 때까지만 머문다 */
		BUG_ON(tg->td->nr_queued[rw] <= 0); /* [한국어] nr_queued는 __blk_throtl_bio()가 붙잡을 때 올리고 여기서 내린다. 내리기 전에 이미 0이면 증감 짝이 어긋난 것이고, 그대로 두면 언더플로로 거대한 수가 되어 상태 판정이 전부 무너진다 */
		tg->td->nr_queued[rw]--; /* [한국어] 붙잡혀 있던 bio 수에서 이 bio를 뺀다 */
	}

	throtl_trim_slice(tg, rw); /* [한국어] bio가 실제로 나갔으니 이 시점에 창을 정리한다. 정리를 미루면 slice_end만 계속 늘어나 새 창이 열리지 못하고, 나중에 상한이 낮아졌을 때 그동안의 사용량이 통째로 새 기준에 적용돼 대기 시간이 폭증한다 */

	if (tg_to_put) /* [한국어] pop이 참조 해제를 미뤄 둔 경우 */
		blkg_put(tg_to_blkg(tg_to_put)); /* [한국어] bio 이동이 모두 끝난 지금이 안전한 시점이다. 이 put으로 blkg가 해제되더라도 더 이상 그 메모리를 건드리지 않는다 */
}

/*
 * [한국어]
 * throtl_dispatch_tg - 한 throtl_grp에서 제한을 통과한 bio들을 부모로 이동.
 * @tg: 대상 throtl_grp
 * @return: 이번 라운드에서 총 디스패치한 bio 수
 *
 * READ 75%(max 6개), WRITE 25%(max 2개) 비율로 한 라운드 최대
 * THROTL_GRP_QUANTUM(8)개 bio를 tg_dispatch_one_bio()로 상위로 전달.
 * tg_dispatch_time()이 0인 bio만 디스패치.
 * 호출 체인: throtl_select_dispatch() → [throtl_dispatch_tg] → tg_dispatch_one_bio()
 */
static int throtl_dispatch_tg(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; READ/WRITE 대기열 접근 */
	unsigned int nr_reads = 0, nr_writes = 0; /* [한국어] 이번 호출에서 방향별로 올린 개수 */
	unsigned int max_nr_reads = THROTL_GRP_QUANTUM * 3 / 4; /* [한국어] 8 × 3/4 = 6. 읽기에 더 큰 몫을 주는 이유는 읽기가 대개 동기(요청자가 결과를 기다림)이고 쓰기는 페이지 캐시를 거쳐 비동기인 경우가 많아, 읽기 지연이 사용자 체감에 직접 나타나기 때문 */
	unsigned int max_nr_writes = THROTL_GRP_QUANTUM - max_nr_reads; /* [한국어] 나머지 2개. 읽기를 우선하되 쓰기 몫을 0이 아니게 남겨, 읽기가 끊이지 않는 워크로드에서도 쓰기가 완전히 굶지 않게 한다 */
	struct bio *bio; /* [한국어] 판정에 쓸 선두 bio(꺼내지 않고 들여다보기만) */

	/* Try to dispatch 75% READS and 25% WRITES */

	while ((bio = throtl_peek_queued(&sq->queued[READ])) && /* [한국어] 큐가 빌 때까지가 아니라 '예산이 남아 있는 동안'만 돈다 */
	       tg_dispatch_time(tg, bio) == 0) { /* [한국어] 매번 다시 판정하는 이유: 방금 올린 bio가 예산을 갉아먹었으므로 다음 bio는 통과하지 못할 수 있다. 0이 아닌 순간 이 방향은 더 못 나가므로 루프가 끝난다 */

		tg_dispatch_one_bio(tg, READ); /* [한국어] peek한 그 bio가 여기서 실제로 꺼내진다. 판정과 꺼내기가 같은 bio를 가리킨다는 전제 위에서 동작한다 */
		nr_reads++; /* [한국어] 이번 라운드 몫 소비 */

		if (nr_reads >= max_nr_reads) /* [한국어] 예산이 아직 남아 있어도 6개에서 끊는다. 여기서 끊지 않으면 큐가 긴 그룹 하나가 queue_lock을 잡은 채 오래 돌아, 형제 그룹과 다른 CPU의 제출 경로가 모두 지연된다 */
			break;
	}

	while ((bio = throtl_peek_queued(&sq->queued[WRITE])) && /* [한국어] 쓰기 방향도 같은 방식으로 처리한다 */
	       tg_dispatch_time(tg, bio) == 0) { /* [한국어] 쓰기 예산은 읽기와 완전히 분리돼 있어, 읽기가 막혀 있어도 쓰기는 나갈 수 있다 */

		tg_dispatch_one_bio(tg, WRITE); /* [한국어] 쓰기 bio 한 칸 전진 */
		nr_writes++; /* [한국어] 쓰기 몫 소비 */

		if (nr_writes >= max_nr_writes) /* [한국어] 2개에서 끊는다 */
			break;
	}

	return nr_reads + nr_writes; /* [한국어] 상위 루프(throtl_select_dispatch)가 이 값을 누적해 라운드 전체 상한(THROTL_QUANTUM)을 판정한다 */
}

/*
 * [한국어]
 * throtl_select_dispatch - 시간이 된 자식 그룹들을 골라 bio를 위로 올린다
 * @parent_sq: 자식들의 pending_tree를 소유한 service_queue
 * @return: 이번 라운드에서 위로 올린 bio 총수 (0이면 아무것도 못 올렸다)
 *
 * 스케줄링의 뼈대다. pending_tree는 disptime 오름차순이므로 최좌단부터
 * 보면 되고, 최좌단의 disptime이 아직 미래면 그 뒤는 볼 필요가 없어 즉시
 * 루프를 끝낸다 - 정렬 덕분에 "가장 이른 것도 아직 아니다"가 곧 "전부 아직
 * 아니다"이기 때문이다.
 *
 * 한 그룹을 처리할 때마다 큐가 남았으면 disptime을 다시 계산해 트리에
 * 재삽입하므로, 같은 그룹이 연속으로 선택되어 형제를 굶기지 않는다.
 * 전체 라운드는 THROTL_QUANTUM에서 끊어 queue_lock 보유 시간을 제한한다.
 *
 * 실행 컨텍스트: queue_lock 보유 + IRQ off (pending_timer 콜백에서 호출).
 */
static int throtl_select_dispatch(struct throtl_service_queue *parent_sq)
{
	unsigned int nr_disp = 0; /* [한국어] 라운드 전체 상한(THROTL_QUANTUM) 판정용 누계 */

	while (1) { /* [한국어] 아래 네 가지 조건 중 하나에 걸릴 때까지 반복한다: 후보 없음 / 트리 빔 / 아직 시간 안 됨 / 라운드 상한 도달 */
		struct throtl_grp *tg; /* [한국어] 이번에 처리할 자식 그룹 */
		struct throtl_service_queue *sq; /* [한국어] 그 그룹의 큐(처리 후 잔여 여부 확인용) */

		if (!parent_sq->nr_pending) /* [한국어] 대기 중인 자식이 하나도 없다 */
			break;

		tg = throtl_rb_first(parent_sq); /* [한국어] 최좌단 = disptime이 가장 이른 그룹 */
		if (!tg) /* [한국어] nr_pending과 트리가 어긋난 비정상 상태(throtl_rb_first가 이미 경고를 냈다). 무한 루프를 피하려면 여기서 빠져야 한다 */
			break;

		if (time_before(jiffies, tg->disptime)) /* [한국어] 가장 이른 그룹조차 아직 시간이 안 됐다면 나머지는 볼 것도 없다. 트리가 정렬돼 있어 가능한 조기 종료다 */
			break; /* [한국어] 남은 그룹들은 pending_timer가 다시 깨워 준다 */

		nr_disp += throtl_dispatch_tg(tg); /* [한국어] 이 그룹에서 예산이 허락하는 만큼(최대 8개) 위로 올린다 */

		sq = &tg->service_queue; /* [한국어] 처리 후 이 그룹에 아직 bio가 남았는지 본다 */
		if (sq_queued(sq, READ) || sq_queued(sq, WRITE)) /* [한국어] 남았다면 예산이 떨어졌거나 quantum이 소진된 것이다 */
			tg_update_disptime(tg); /* [한국어] 새 disptime으로 트리에 재삽입한다. 이 재삽입이 없으면 같은 그룹이 계속 최좌단에 남아 형제가 영영 차례를 못 받는다 */
		else
			throtl_dequeue_tg(tg); /* [한국어] 다 비웠으면 트리에서 뺀다. 빈 그룹을 남겨 두면 타이머가 아무 할 일 없이 계속 깨어난다 */

		if (nr_disp >= THROTL_QUANTUM) /* [한국어] 한 라운드 총 bio 수가 THROTL_QUANTUM에 도달하면 중단; 한 라운드 배치 크기 제한 */
			break;
	}

	return nr_disp; /* [한국어] 이번 라운드에서 하위 계층으로 풀어준 총 bio 수 반환 */
}

/**
 * throtl_pending_timer_fn - timer function for service_queue->pending_timer
 * @t: the pending_timer member of the throtl_service_queue being serviced
 *
 * This timer is armed when a child throtl_grp with active bio's become
 * pending and queued on the service_queue's pending_tree and expires when
 * the first child throtl_grp should be dispatched.  This function
 * dispatches bio's from the children throtl_grps to the parent
 * service_queue.
 *
 * If the parent's parent is another throtl_grp, dispatching is propagated
 * by either arming its pending_timer or repeating dispatch directly.  If
 * the top-level service_tree is reached, throtl_data->dispatch_work is
 * kicked so that the ready bio's are issued.
 *
 * [한국어]
 * 붙잡아 둔 bio를 다시 흐르게 하는 유일한 시동 지점이다. 이 타이머가 없으면
 * 제한에 걸린 bio는 다음 bio가 제출될 때까지 아무도 깨워 주지 않는다.
 *
 * 이 함수는 sq 하나가 아니라 계층을 타고 올라가며 반복될 수 있다. 자식에서
 * 부모로 bio를 올렸는데 마침 부모의 디스패치 창도 열려 있다면, 부모의
 * 타이머를 새로 걸어 다시 깨어나기를 기다리는 대신 그 자리에서 sq를 부모로
 * 바꿔 again 라벨로 되돌아간다. 타이머 한 번에 여러 계층을 통과시켜 지연을
 * 줄이려는 것이다.
 *
 * 실행 컨텍스트: 타이머 softirq. 여기서 queue_lock을 직접 잡으므로
 * spin_lock_irq를 쓴다. 루프 중간에 락을 놓았다 다시 잡는 구간이 있어,
 * 락을 놓은 사이 다른 CPU가 큐 상태를 바꿀 수 있다는 전제로 매 회전마다
 * 상태를 다시 읽는다.
 */
static void throtl_pending_timer_fn(struct timer_list *t)
{
	struct throtl_service_queue *sq = timer_container_of(sq, t, /* [한국어] 타이머 콜백은 struct timer_list 주소만 받는다. 그 필드가 박혀 있는 바깥 구조체를 오프셋 뺄셈으로 되찾는 매크로이며, 첫 인자로 결과 변수 자신을 넘겨 타입을 추론한다 */
							     pending_timer); /* [한국어] 되찾을 기준이 되는 멤버 이름 */
	struct throtl_grp *tg = sq_to_tg(sq); /* [한국어] 이 타이머가 cgroup 노드의 것인지 루트의 것인지 가른다. 루트면 NULL */
	struct throtl_data *td = sq_to_td(sq); /* [한국어] 루트 경로에서만 실제로 쓰인다(아래 q 결정과 dispatch_work 예약) */
	struct throtl_service_queue *parent_sq; /* [한국어] again 라벨에서 매번 다시 읽는다 - sq가 부모로 바뀌면 이 값도 따라 바뀌어야 하기 때문 */
	struct request_queue *q; /* [한국어] 이 함수 전체에서 잡을 락의 소유자 */
	bool dispatched; /* [한국어] 이번 회차에 실제로 뭔가 올렸는지. 아무것도 못 올렸으면 상위 전파도 workqueue 기동도 할 필요가 없다 */
	int ret; /* [한국어] throtl_select_dispatch()가 올린 bio 수 */

	/* throtl_data may be gone, so figure out request queue by blkg */
	if (tg) /* [한국어] 원본 주석이 경고하듯 td는 이미 해제됐을 수 있다(blk_throtl_exit이 kfree). 반면 tg는 blkg 참조로 살아 있음이 보장되므로, 가능하면 blkg 쪽에서 queue를 얻는다 */
		q = tg->pd.blkg->q; /* [한국어] blkg를 통해 안전하게 request_queue를 얻는다 */
	else
		q = td->queue; /* [한국어] 루트 타이머는 td와 생명주기가 같아(blk_throtl_exit이 timer_delete_sync를 먼저 한다) td를 봐도 안전하다 */

	spin_lock_irq(&q->queue_lock); /* [한국어] 스로틀 상태 전체가 이 락 하나로 보호된다. 경쟁 상대는 다른 CPU의 __blk_throtl_bio()(bio를 큐에 넣으며 트리와 카운터를 건드림), cgroup 설정 변경 경로, 디스크 해제 경로다. _irq 계열인 이유는 이 함수 자신이 softirq에서 돌기 때문 */

	if (!q->root_blkg) /* [한국어] 디스크가 해제되는 중이면 blkg 계층이 이미 헐린 상태다. 그 위에서 트리를 순회하면 해제된 노드를 밟는다 */
		goto out_unlock; /* [한국어] 남은 bio는 blk_throtl_cancel_bios()가 따로 흘려보낸다 */

again:
	parent_sq = sq->parent_sq; /* [한국어] 부모 service_queue; cgroup QoS 계층에서 한 단계 위로 전파 */
	dispatched = false; /* [한국어] 이번 타이머 실행에서 bio를 실제로 풀었는지 추적 */

	while (true) { /* [한국어] 창이 계속 열려 있는 한(= 지금 당장 나갈 수 있는 그룹이 아직 남아 있는 한) 반복한다 */
		unsigned int __maybe_unused bio_cnt_r = sq_queued(sq, READ); /* [한국어] 로그 전용 값. __maybe_unused는 blktrace가 컴파일에서 빠지면 throtl_log가 통째로 사라져 이 변수가 미사용이 되기 때문에 붙인다 */
		unsigned int __maybe_unused bio_cnt_w = sq_queued(sq, WRITE); /* [한국어] 같은 이유 */

		/* [한국어] 이번 회전을 시작할 때의 대기량 스냅숏. 로그를 시간순으로 읽으면
		 * 큐가 줄어드는지(정상) 계속 늘어나는지(제한이 유입을 못 따라감)를 볼 수 있다. */
		throtl_log(sq, "dispatch nr_queued=%u read=%u write=%u",
			   bio_cnt_r + bio_cnt_w, bio_cnt_r, bio_cnt_w);

		ret = throtl_select_dispatch(sq); /* [한국어] 시간이 된 자식들에서 bio를 위로 올린다 */
		if (ret) { /* [한국어] 하나라도 올렸으면 */
			throtl_log(sq, "bios disp=%u", ret); /* [한국어] 이번 회전에 올린 개수를 남긴다 */
			dispatched = true; /* [한국어] 아래 상위 전파/워크큐 기동의 전제 조건 */
		}

		if (throtl_schedule_next_dispatch(sq, false)) /* [한국어] true면 '더 할 일이 없거나 타이머를 걸어 두었다'는 뜻이다. 즉 이 자리에서 계속 돌 이유가 없다 */
			break;

		/* this dispatch windows is still open, relax and repeat */
		spin_unlock_irq(&q->queue_lock); /* [한국어] 창이 아직 열려 있으니 곧바로 한 번 더 돈다. 다만 락을 계속 쥔 채 반복하면 그동안 제출 경로가 전부 멈추므로, 회전 사이에 일부러 락을 놓아 다른 CPU에 기회를 준다 */
		cpu_relax(); /* [한국어] 락을 놓자마자 다시 잡으면 같은 CPU가 그대로 재획득해 양보 효과가 사라진다. 이 힌트로 파이프라인을 잠깐 늦춰 상대가 락을 가져갈 틈을 만든다 */
		spin_lock_irq(&q->queue_lock); /* [한국어] 다시 잡는다. 놓은 사이에 트리와 큐가 바뀌었을 수 있으므로, 위에서 캐시해 둔 값을 쓰지 않고 루프 처음부터 상태를 다시 읽는 구조로 되어 있다 */
	}

	if (!dispatched) /* [한국어] 이번 타이머에서 bio를 하나도 풀지 못하면 정리 종료 */
		goto out_unlock;

	if (parent_sq) { /* [한국어] 아직 루트가 아니다 = 방금 올린 bio는 부모 큐에 쌓였을 뿐 발행되지 않았다 */
		/* @parent_sq is another throl_grp, propagate dispatch */
		if (tg->flags & THROTL_TG_WAS_EMPTY || /* [한국어] 이 두 플래그가 서 있다는 것은 '부모 큐에서 이 그룹의 슬롯이 비어 있다가 방금 채워졌다'는 뜻이다. 그 경우 부모가 들고 있는 이 그룹의 disptime은 큐가 비었던 시절의 낡은 값이라 반드시 다시 계산해야 한다 */
		    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) { /* [한국어] iops 큐 쪽에서만 새로 채워진 경우를 잡는 별도 신호 */
			tg_update_disptime(tg); /* [한국어] 부모 트리에서 이 그룹의 위치를 새 disptime으로 다시 잡는다 */
			if (!throtl_schedule_next_dispatch(parent_sq, false)) { /* [한국어] 부모의 dispatch window가 열려 있으면 즉시 상위로 전파; 유입 연쇄 */
				/* window is already open, repeat dispatching */
				sq = parent_sq; /* [한국어] 부모 service_queue를 현재 기준으로 삼고 다시 dispatch */
				tg = sq_to_tg(sq); /* [한국어] 부모 service_queue의 throtl_grp 획득; cgroup QoS 계층 전파 */
				goto again; /* [한국어] 부모 계층에서 다시 하위 계층으로 bio 풀기 시도 */
			}
		}
	} else {
		/* reached the top-level, queue issuing */
		queue_work(kthrotld_workqueue, &td->dispatch_work); /* [한국어] 루트 큐의 bio를 실제로 발행할 workqueue 예약 */
	}
out_unlock:
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제 */
}

/**
 * blk_throtl_dispatch_work_fn - work function for throtl_data->dispatch_work
 * @work: work item being executed
 *
 * This function is queued for execution when bios reach the bio_lists[]
 * of throtl_data->service_queue.  Those bios are ready and issued by this
 * function.
 *
 * [한국어]
 * 왜 타이머 콜백에서 바로 발행하지 않고 workqueue로 넘기는가.
 * submit_bio_noacct_nocheck()는 블록 스택 전체를 다시 타는 무거운 경로이며
 * 스택 프레임도 깊고 슬립할 수 있는 지점을 포함한다. 타이머 콜백은 softirq
 * 이고 이 파일은 그 안에서 queue_lock을 쥐고 있으므로 거기서 발행할 수 없다.
 * 그래서 "락을 잡고 목록만 뽑아 두고, 락을 놓은 뒤 프로세스 컨텍스트(kworker)
 * 에서 발행"으로 나눈다.
 *
 * 실행 컨텍스트: kthrotld workqueue의 kworker(프로세스 컨텍스트).
 * WQ_MEM_RECLAIM으로 만들어져 메모리 부족 시에도 진행이 보장된다 - 이 경로가
 * 막히면 스왑/라이트백이 영영 진행되지 못하기 때문이다.
 */
static void blk_throtl_dispatch_work_fn(struct work_struct *work)
{
	struct throtl_data *td = container_of(work, struct throtl_data, /* [한국어] work_struct는 throtl_data 안에 박혀 있으므로, 콜백이 받은 work 주소에서 디스크 단위 상태를 되찾는다 */
					      dispatch_work);
	struct throtl_service_queue *td_sq = &td->service_queue; /* [한국어] 루트 큐. 여기 있는 bio는 이미 모든 계층의 제한을 통과한 상태다 */
	struct request_queue *q = td->queue; /* [한국어] 락 소유자 */
	struct bio_list bio_list_on_stack; /* [한국어] 스택에 두는 이유가 핵심이다 - 락 안에서 뽑아낸 bio를 이 지역 리스트로 옮겨 놓으면, 락을 놓은 뒤 공유 자료구조를 전혀 건드리지 않고 발행만 할 수 있다 */
	struct bio *bio; /* [한국어] 반복자 */
	struct blk_plug plug; /* [한국어] 여러 bio를 한 번에 내보낼 때 하위 계층이 요청을 모아 두었다가 한꺼번에 처리하도록 하는 장치 */
	int rw; /* [한국어] 두 방향 큐를 모두 비우기 위한 반복자 */

	bio_list_init(&bio_list_on_stack); /* [한국어] 스택 변수라 반드시 명시적으로 초기화해야 한다 */

	spin_lock_irq(&q->queue_lock); /* [한국어] 큐에서 꺼내는 동안에만 락을 잡는다. 발행까지 락 안에서 하면 그동안 모든 제출 경로가 멈춘다 */
	for (rw = READ; rw <= WRITE; rw++) /* [한국어] 양방향 모두 비운다 */
		while ((bio = throtl_pop_queued(td_sq, NULL, rw))) /* [한국어] NULL을 넘기는 이유: 이 함수는 꺼낸 뒤 sq를 더 쓰지 않으므로 참조 해제를 미룰 필요가 없다 */
			bio_list_add(&bio_list_on_stack, bio); /* [한국어] 지역 리스트로 옮겨 담기만 한다 */
	spin_unlock_irq(&q->queue_lock); /* [한국어] 여기서부터는 공유 상태를 건드리지 않으므로 락이 필요 없다 */

	if (!bio_list_empty(&bio_list_on_stack)) { /* [한국어] 빈 목록에 plug를 걸었다 푸는 것은 순수한 낭비라 미리 걸러낸다 */
		blk_start_plug(&plug); /* [한국어] 여기 모인 bio들은 서로 다른 시점에 붙잡혔다가 한꺼번에 풀려나는 묶음이다. plug로 감싸면 하위 계층이 이들을 모아 병합/일괄 제출할 기회를 얻는다 */
		while ((bio = bio_list_pop(&bio_list_on_stack))) /* [한국어] 넣은 순서대로 꺼내 발행 순서를 보존한다 */
			submit_bio_noacct_nocheck(bio, false); /* [한국어] 제출 경로로 되돌려 보낸다. _nocheck 계열이라 이미 마친 검사를 반복하지 않고, bio에 남은 BIO_BPS_THROTTLED 덕분에 스로틀 계층으로 다시 잡히지도 않는다 */
		blk_finish_plug(&plug); /* [한국어] 모아 둔 요청을 실제로 흘려보낸다 */
	}
}

/*
 * [한국어]
 * tg_prfill_conf_u64 - cgroup sysfs에 u64 설정값을 출력한다.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: throtl_grp 내 u64 필드 오프셋
 * @return: 0 (U64_MAX면 출력 생략)
 *
 * 호출 체인: tg_print_conf_u64() → [tg_prfill_conf_u64]
 */
static u64 tg_prfill_conf_u64(struct seq_file *sf, struct blkg_policy_data *pd,
			      int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	u64 v = *(u64 *)((void *)tg + off); /* [한국어] 오프셋으로 u64 필드 직접 접근; bps 상한값 읽기 */

	if (v == U64_MAX) /* [한국어] 무제한이면 출력 생략 */
		return 0;
	return __blkg_prfill_u64(sf, pd, v); /* [한국어] seq_file에 "devname value\n" 형식 출력 */
}

/*
 * [한국어]
 * tg_prfill_conf_uint - cgroup sysfs에 unsigned int 설정값을 출력한다.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: throtl_grp 내 uint 필드 오프셋
 * @return: 0 (UINT_MAX면 출력 생략)
 *
 * 호출 체인: tg_print_conf_uint() → [tg_prfill_conf_uint]
 */
static u64 tg_prfill_conf_uint(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	unsigned int v = *(unsigned int *)((void *)tg + off); /* [한국어] 오프셋으로 uint 필드 직접 접근; iops 상한값 읽기 */

	if (v == UINT_MAX) /* [한국어] 무제한이면 출력 생략 */
		return 0;
	return __blkg_prfill_u64(sf, pd, v); /* [한국어] seq_file에 값 출력 */
}

/*
 * [한국어]
 * tg_print_conf_u64 - 전체 cgroup 계층의 u64 설정값을 seq_file에 출력한다.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * blkcg_print_blkgs()로 모든 blkg를 순회해 tg_prfill_conf_u64()를 호출.
 * 호출 체인: cftype.seq_show → [tg_print_conf_u64]
 */
static int tg_print_conf_u64(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_u64,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false); /* [한국어] 모든 blkg를 순회해 bps 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_print_conf_uint - 전체 cgroup 계층의 uint 설정값을 seq_file에 출력한다.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_conf_uint]
 */
static int tg_print_conf_uint(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_uint,
			  &blkcg_policy_throtl, seq_cft(sf)->private, false); /* [한국어] 모든 blkg를 순회해 iops 상한 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_conf_updated - cgroup의 bps/iops 설정 변경 후 하위 트리 전체 상태를 갱신한다.
 * @tg: 설정이 바뀐 throtl_grp
 * @global: true면 루트 blkg부터 전체 서브트리 순회; false면 tg 서브트리만
 * @return: 없음 (void)
 *
 * tg_set_conf() 또는 tg_set_limit()이 새 bps/iops 한도를 tg에 기록한 뒤 호출된다.
 * 이 함수는 (1) 변경된 tg 서브트리의 has_rules[] 플래그를 갱신해 blk-throttle
 * 우회 여부를 재결정하고, (2) READ/WRITE slice를 재시작해 갑작스러운 rate 하향이
 * 이미 진행 중인 하위 계층 IO에 소급 적용되지 않도록 한다.
 * (3) pending 상태면 disptime을 재계산하고 부모 pending_timer를 재예약한다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트); queue->queue_lock 보유 상태.
 *
 * 호출 체인:
 *   tg_set_conf() / tg_set_limit() → [tg_conf_updated] → tg_update_has_rules(),
 *   throtl_start_new_slice(), tg_update_disptime(), throtl_schedule_next_dispatch()
 */
static void tg_conf_updated(struct throtl_grp *tg, bool global)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] 마지막에 부모 타이머를 다시 걸 때 parent_sq를 얻기 위해 필요 */
	struct cgroup_subsys_state *pos_css; /* [한국어] 서브트리 순회 커서 */
	struct blkcg_gq *blkg; /* [한국어] 순회 중 현재 노드 */

	/* [한국어] 새로 적용된 네 값을 남긴다. 이 시점에는 이미 tg->bps[]/iops[]에 새 값이
	 * 기록된 뒤라, 여기 찍히는 것이 '변경 후' 값이다. 설정 변경 시각을 IO 로그와
	 * 나란히 놓고 보면 rate가 언제부터 달라졌는지 정확히 맞출 수 있다. */
	throtl_log(&tg->service_queue,
		   "limit change rbps=%llu wbps=%llu riops=%u wiops=%u",
		   tg_bps_limit(tg, READ), tg_bps_limit(tg, WRITE),
		   tg_iops_limit(tg, READ), tg_iops_limit(tg, WRITE));

	rcu_read_lock(); /* [한국어] blkg 계층 순회는 RCU로 보호된다. 순회 중 다른 CPU가 cgroup을 지울 수 있는데, RCU 유예 기간 덕분에 이 구간에서는 노드가 해제되지 않는다 */
	/*
	 * Update has_rules[] flags for the updated tg's subtree.  A tg is
	 * considered to have rules if either the tg itself or any of its
	 * ancestors has rules.  This identifies groups without any
	 * restrictions in the whole hierarchy and allows them to bypass
	 * blk-throttle.
	 */
	blkg_for_each_descendant_pre(blkg, pos_css, /* [한국어] 반드시 pre-order(부모 먼저)여야 한다. tg_update_has_rules()가 '부모의 has_rules'를 읽어 자식 값을 정하므로, 부모가 먼저 갱신돼 있지 않으면 자식이 낡은 값을 상속한다 */
			global ? tg->td->queue->root_blkg : tg_to_blkg(tg)) { /* [한국어] global이면 디스크 전체 트리를, 아니면 바뀐 그룹의 서브트리만 다시 계산한다. 제한 변경의 영향은 아래로만 퍼지므로 보통 서브트리로 충분하다 */
		struct throtl_grp *this_tg = blkg_to_tg(blkg); /* [한국어] 현재 노드의 스로틀 상태 */

		tg_update_has_rules(this_tg); /* [한국어] 이 노드의 우회 가능 여부(has_rules) 재계산 */
		/* ignore root/second level */
		if (!cgroup_subsys_on_dfl(io_cgrp_subsys) || !blkg->parent || /* [한국어] continue가 루프 본문의 마지막 문장이라, 이 분기는 현재 코드에서 실행 흐름을 전혀 바꾸지 않는다(조건이 참이든 거짓이든 다음 반복으로 넘어간다). 조건이 무엇을 걸러내려던 것인지는 원본 주석("ignore root/second level")에만 남아 있고, 걸러낸 뒤 하려던 처리는 이 코드에 없다 */
		    !blkg->parent->parent) /* [한국어] 루트와 그 바로 아래(2단계)를 가려내던 조건 */
			continue;
	}
	rcu_read_unlock(); /* [한국어] 순회 종료 */

	/*
	 * We're already holding queue_lock and know @tg is valid.  Let's
	 * apply the new config directly.
	 *
	 * Restart the slices for both READ and WRITES. It might happen
	 * that a group's limit are dropped suddenly and we don't want to
	 * account recently dispatched IO with new low rate.
	 */
	throtl_start_new_slice(tg, READ, false); /* [한국어] READ slice 재시작; 새 rate limit 윈도우 적용 */
	throtl_start_new_slice(tg, WRITE, false); /* [한국어] WRITE slice 재시작; 새 rate limit 윈도우 적용 */

	if (tg->flags & THROTL_TG_PENDING) { /* [한국어] 이 그룹이 이미 대기 트리에 올라가 있다면, 트리 안의 disptime은 옛 상한으로 계산된 값이라 새 설정과 맞지 않는다 */
		tg_update_disptime(tg); /* [한국어] 새 제한 하에서 다음 발행 시점 재계산 */
		throtl_schedule_next_dispatch(sq->parent_sq, true); /* [한국어] force=true인 이유: 상한을 크게 올렸다면 새 disptime이 이미 과거일 수 있는데, 이 설정 경로는 프로세스 컨텍스트라 여기서 직접 디스패치할 수 없다. 그래서 무조건 타이머를 걸어 타이머 컨텍스트가 처리하도록 넘긴다 */
	}
}

/*
 * [한국어]
 * blk_throtl_init - gendisk의 request_queue에 blk-throttle 계층을 활성화한다.
 * @disk: 대상 gendisk; 대상 블록 장치
 * @return: 0(성공) 또는 음수 에러 코드; 실패 시 throtl_data 해제 후 반환
 *
 * cgroup sysfs에서 처음으로 bps/iops 한도가 기록될 때 (tg_set_conf/tg_set_limit)
 * 아직 초기화되지 않은 블록 장치에 대해 호출된다.
 * throtl_data를 NUMA-aware하게 할당하고, dispatch_work와 최상위 service_queue를
 * 초기화한 뒤 blkcg_policy_throtl를 장치에 등록한다.
 * 등록 이후 해당 queue로 submit되는 모든 bio는 __blk_throtl_bio()에서 rate limit 검사를 받는다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트); freeze/quiesce로 request_queue 동결.
 *
 * 호출 체인:
 *   tg_set_conf() / tg_set_limit() → [blk_throtl_init] → blkcg_activate_policy()
 *   (→ pd_alloc_fn → throtl_pd_alloc() / pd_init_fn → throtl_pd_init())
 */
static int blk_throtl_init(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] throtl_data를 매달 대상 */
	struct throtl_data *td; /* [한국어] 이 디스크의 스로틀 상태(루트 큐 + 발행 work) */
	unsigned int memflags; /* [한국어] blk_mq_freeze_queue()가 돌려주는 값. 동결 중에는 이 경로가 스스로 IO를 유발하는 메모리 할당을 하지 않도록 현재 태스크의 할당 컨텍스트를 잠시 바꾸는데, 그 원상복구 정보를 담는다 */
	int ret; /* [한국어] 정책 활성화 결과 */

	td = kzalloc_node(sizeof(*td), GFP_KERNEL, q->node); /* [한국어] 디스크가 붙은 노드에 할당한다. 여기는 설정 경로라 GFP_KERNEL로 슬립하며 기다려도 된다 */
	if (!td) /* [한국어] 이 시점에는 q->td도 아직 건드리지 않았고 정책도 켜지 않았다 */
		return -ENOMEM; /* [한국어] 되돌릴 것이 없으므로 바로 반환한다. 사용자에게는 설정 write가 -ENOMEM으로 실패한 것으로 보인다 */

	INIT_WORK(&td->dispatch_work, blk_throtl_dispatch_work_fn); /* [한국어] 아래에서 정책을 켜는 순간부터 bio가 붙잡힐 수 있으므로, 그 bio를 다시 흘려보낼 work를 미리 준비해 둔다 */
	throtl_service_queue_init(&td->service_queue); /* [한국어] 루트 큐와 그 타이머도 정책 활성화 이전에 사용 가능한 상태여야 한다 */

	memflags = blk_mq_freeze_queue(disk->queue); /* [한국어] 진행 중인 IO가 모두 끝날 때까지 기다리고 새 진입을 막는다. 이 동결이 없으면 어떤 bio는 q->td가 NULL인 상태로 통과하고 어떤 bio는 새 정책을 만나는, 경계가 불분명한 구간이 생긴다 */
	blk_mq_quiesce_queue(disk->queue); /* [한국어] 동결에 더해 이미 큐에 들어온 요청의 디스패치까지 멈춘다. 동결(신규 진입 차단)과 정지(디스패치 차단)는 서로 다른 것을 막으므로 둘 다 필요하다 */

	q->td = td; /* request_queue에 throtl_data 연결; 이후 bio는 발행 전 rate limit 검사 */
	td->queue = q; /* [한국어] 반대 방향 링크. q->td와 td->queue가 서로를 가리켜, 트리 어느 지점에서든 락 소유자인 request_queue에 도달할 수 있다 */

	/* activate policy, blk_throtl_activated() will return true */
	ret = blkcg_activate_policy(disk, &blkcg_policy_throtl); /* blkcg_policy_throtl 활성화; 이 장치에 cgroup 기반 throttle 정책 등록 */
	if (ret) { /* 정책 등록 실패 시 이 장치에 대한 throttle 상태 rollback */
		q->td = NULL; /* throtl_data 연결 해제; 스로틀 계층 비활성화 */
		kfree(td); /* throtl_data 메모리 해제; 블록 장치 throttle 상태 제거 */
	}

	blk_mq_unquiesce_queue(disk->queue); /* [한국어] 디스패치 재개. 성공/실패 어느 쪽이든 반드시 풀어야 하므로 분기 밖에 둔다 */
	blk_mq_unfreeze_queue(disk->queue, memflags); /* [한국어] 동결 해제. quiesce → unfreeze 순서로 푸는 것은 freeze → quiesce로 잠근 순서의 역순이다. memflags로 할당 컨텍스트도 원래대로 되돌린다 */

	return ret;
}


/*
 * [한국어]
 * tg_set_conf - cgroup sysfs write를 통해 bps 또는 iops 한도를 갱신한다.
 * @of: kernfs_open_file; cgroup 경로와 cftype의 private 오프셋 포함
 * @buf: 사용자 공간에서 write된 문자열 (예: "8:0 104857600")
 * @nbytes: 입력 바이트 수
 * @off: kernfs 파일 오프셋 (미사용)
 * @is_u64: true면 bps(u64), false면 iops(unsigned int)
 * @return: nbytes(성공) 또는 음수 에러 코드
 *
 * 사용자가 "echo 10485760 > /sys/fs/cgroup/.../io.throttle.read_bps_device" 형식으로
 * 장치별 rate limit을 설정하면 kernfs를 통해 이 함수가 호출된다.
 * bdev 획득 → throttle 초기화(미활성 시) → blkg 준비 → 값 파싱 → tg 필드 기록
 * → tg_conf_updated() 순서로 동작한다.
 * 0 입력은 무제한(U64_MAX)으로 변환된다.
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트).
 *
 * 호출 체인:
 *   tg_set_conf_u64() / tg_set_conf_uint() → [tg_set_conf] → blk_throtl_init(),
 *   blkg_conf_prep(), tg_conf_updated()
 */
static ssize_t tg_set_conf(struct kernfs_open_file *of,
			   char *buf, size_t nbytes, loff_t off, bool is_u64)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of)); /* [한국어] kernfs cgroup css → blkcg; 호출한 cgroup 식별 */
	struct blkg_conf_ctx ctx; /* [한국어] blkg_conf_prep/exit에 전달할 컨텍스트 (bdev, blkg 포함) */
	struct throtl_grp *tg; /* [한국어] 설정 대상 cgroup의 throtl_grp */
	int ret; /* [한국어] 에러 코드; 0이면 성공 */
	u64 v; /* [한국어] 파싱된 rate limit 값 (bps 또는 iops) */

	blkg_conf_init(&ctx, buf); /* [한국어] blkg_conf_ctx 초기화; 버퍼와 bdev 정보 설정 준비 */

	ret = blkg_conf_open_bdev(&ctx); /* [한국어] 입력 앞부분의 "major:minor"를 해석해 대상 블록 장치를 연다. 성공하면 ctx가 bdev 참조를 쥐게 되므로, 이후 모든 실패는 반드시 out_finish의 blkg_conf_exit()를 거쳐야 그 참조가 풀린다 */
	if (ret)
		goto out_finish; /* [한국어] 여기서 실패하면 ctx는 아직 아무것도 잡지 않았지만, 정리 함수가 그 경우도 안전하게 처리하므로 같은 출구를 쓴다 */

	if (!blk_throtl_activated(ctx.bdev->bd_queue)) { /* [한국어] 스로틀 계층은 실제로 제한을 거는 사람이 나타날 때 비로소 만들어진다 - 쓰지도 않을 디스크마다 throtl_data와 그룹을 달아 두는 비용을 피하기 위한 지연 초기화 */
		ret = blk_throtl_init(ctx.bdev->bd_disk); /* [한국어] 이 디스크에 대해 처음으로 제한을 거는 순간 계층을 만든다 */
		if (ret)
			goto out_finish; /* [한국어] 계층 생성 실패. blk_throtl_init() 내부에서 자기 몫은 이미 되돌려 놓았고, 여기서는 bdev 참조만 풀면 된다 */
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, &ctx); /* [한국어] (이 cgroup, 이 디스크) 조합의 blkg를 찾거나 새로 만든다. 이것이 있어야 대응하는 throtl_grp도 존재한다 */
	if (ret)
		goto out_finish;

	ret = -EINVAL; /* [한국어] 아래 파싱 단계의 기본 실패 코드를 미리 깔아 둔다. 이렇게 하면 각 실패 지점마다 코드를 대입할 필요 없이 goto만 하면 된다 */
	if (sscanf(ctx.body, "%llu", &v) != 1) /* [한국어] v1 인터페이스는 값 하나만 받는다(장치 지정은 이미 앞에서 소비됨) */
		goto out_finish; /* [한국어] 숫자가 아니면 -EINVAL */
	if (!v) /* [한국어] 사용자가 0을 쓰면 '제한 해제'의 뜻이다 */
		v = U64_MAX; /* [한국어] 내부 표현으로 바꾼다. 0을 그대로 저장하면 대역폭 0 = 완전 차단이 되어 사용자 의도와 정반대가 된다 */

	tg = blkg_to_tg(ctx.blkg); /* 대상 cgroup의 throtl_grp 획득; 장치별 rate limit 객체 */
	tg_update_carryover(tg); /* 설정 변경 전 누적 대기량 carryover; 발행 지연 손실/이익 보정 */

	if (is_u64) /* [한국어] bps(u64) 설정 경로 */
		*(u64 *)((void *)tg + of_cft(of)->private) = v; /* [한국어] u64 필드(bps)에 새 limit 기록; 대역폭 제한 갱신 */
	else /* [한국어] iops(uint) 설정 경로 */
		*(unsigned int *)((void *)tg + of_cft(of)->private) = v; /* [한국어] uint 필드(iops)에 새 limit 기록; 초당 IO 제한 갱신 */

	tg_conf_updated(tg, false); /* [한국어] 설정 변경 후 limit 적용 및 slice 재시작; 유입 rate 갱신 */
	ret = 0; /* [한국어] 성공 */
out_finish:
	blkg_conf_exit(&ctx); /* [한국어] blkg_conf_ctx 정리; bdev 참조 해제 */
	return ret ?: nbytes; /* [한국어] 성공이면 nbytes, 실패면 에러 코드 반환 */
}

/*
 * [한국어]
 * tg_set_conf_u64 - cftype write 핸들러; tg_set_conf()를 u64(bps) 모드로 호출.
 * @of: kernfs open file
 * @buf: 입력 문자열
 * @nbytes, @off: kernfs 표준 파라미터
 * @return: tg_set_conf() 반환값 그대로
 *
 * 호출 체인: cftype.write → [tg_set_conf_u64] → tg_set_conf()
 */
static ssize_t tg_set_conf_u64(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, true); /* [한국어] bps(u64) 경로로 tg_set_conf 위임 */
}

/*
 * [한국어]
 * tg_set_conf_uint - cftype write 핸들러; tg_set_conf()를 uint(iops) 모드로 호출.
 * @of, @buf, @nbytes, @off: kernfs 표준 파라미터
 * @return: tg_set_conf() 반환값 그대로
 *
 * 호출 체인: cftype.write → [tg_set_conf_uint] → tg_set_conf()
 */
static ssize_t tg_set_conf_uint(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	return tg_set_conf(of, buf, nbytes, off, false); /* [한국어] iops(uint) 경로로 tg_set_conf 위임 */
}

/*
 * [한국어]
 * tg_print_rwstat - cgroup 계층의 rwstat(read/write 통계)를 seq_file에 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_rwstat]
 */
static int tg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), /* [한국어] 이 cgroup이 접해 본 모든 디스크를 한 줄씩 출력한다 */
			  blkg_prfill_rwstat, &blkcg_policy_throtl, /* [한국어] 줄 하나를 찍는 일은 공용 헬퍼에 맡기고, 어느 정책의 데이터인지만 지정한다 */
			  seq_cft(sf)->private, true); /* [한국어] private에는 throtl_grp 안에서의 필드 오프셋이 들어 있어(cftype 정의 참고) stat_bytes와 stat_ios가 같은 함수를 공유할 수 있다. 마지막 true는 rwstat 형식(읽기/쓰기 분리 출력)을 뜻한다 */
	return 0;
}

/*
 * [한국어]
 * tg_prfill_rwstat_recursive - 한 cgroup의 rwstat 누적값을 seq_file에 출력.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: rwstat 필드 오프셋
 * @return: 출력 바이트 수
 *
 * 호출 체인: tg_print_rwstat_recursive() → [tg_prfill_rwstat_recursive]
 */
static u64 tg_prfill_rwstat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample sum; /* [한국어] 서브트리 합계를 담을 스택 버퍼. percpu 카운터를 그때그때 접어 넣기 때문에 저장 공간이 따로 필요하다 */

	blkg_rwstat_recursive_sum(pd_to_blkg(pd), &blkcg_policy_throtl, off, /* [한국어] 자신과 모든 자손의 값을 더한다. 자식 cgroup에서 난 IO도 부모 계정에 잡혀야 계층적 회계가 맞기 때문 */
				  &sum);
	return __blkg_prfill_rwstat(sf, pd, &sum); /* [한국어] 합산 결과를 한 줄로 출력 */
}

/*
 * [한국어]
 * tg_print_rwstat_recursive - 전체 cgroup 계층의 누적 rwstat를 seq_file에 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_rwstat_recursive]
 */
static int tg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), /* [한국어] 비재귀 버전과 구조는 같고 */
			  tg_prfill_rwstat_recursive, &blkcg_policy_throtl, /* [한국어] 줄을 찍는 콜백만 '자손 합산' 버전으로 바꾼 것이 차이다 */
			  seq_cft(sf)->private, true); /* [한국어] 오프셋과 출력 형식은 동일 */
	return 0;
}

/* [한국어] throtl_legacy_files - v1 cgroup 인터페이스(blkcg.legacy_cftypes)에서 노출하는 파일 목록.
 * 각 파일은 cgroup/blkio.throttle.<방향>_<단위>_device 형식으로 노출되어
 * 장치별 bps/iops 한도를 개별적으로 설정한다.
 * v2 io.max와 달리 4가지 한도를 별도 파일로 관리한다. */
static struct cftype throtl_legacy_files[] = {
	{
		.name = "throttle.read_bps_device", /* [한국어] READ bps 한도 파일; 읽기 대역폭(바이트/초) 제한 */
		.private = offsetof(struct throtl_grp, bps[READ]), /* [한국어] throtl_grp.bps[READ] 오프셋 */
		.seq_show = tg_print_conf_u64, /* [한국어] cat 시 현재 READ bps 한도 출력 */
		.write = tg_set_conf_u64, /* [한국어] echo 시 새 READ bps 한도 설정 */
	},
	{
		.name = "throttle.write_bps_device", /* [한국어] WRITE bps 한도 파일; 쓰기 대역폭(바이트/초) 제한 */
		.private = offsetof(struct throtl_grp, bps[WRITE]), /* [한국어] private에 '필드 위치'를 넣는 것이 이 표의 요령이다 - 핸들러는 필드마다 따로 만들지 않고, 이 오프셋으로 어느 값을 읽고 쓸지 결정한다 */
		.seq_show = tg_print_conf_u64, /* [한국어] read: 위 오프셋의 u64를 출력(무제한이면 아무것도 안 찍음) */
		.write = tg_set_conf_u64, /* [한국어] write: 값을 파싱해 위 오프셋에 기록하고 설정 변경 후처리까지 수행 */
	},
	{
		.name = "throttle.read_iops_device", /* [한국어] READ iops 한도 파일; 읽기 초당 IO 수 제한 */
		.private = offsetof(struct throtl_grp, iops[READ]), /* [한국어] iops는 unsigned int 배열이라 아래 핸들러도 uint 버전을 쓴다 */
		.seq_show = tg_print_conf_uint, /* [한국어] read: uint 폭으로 읽어 출력 */
		.write = tg_set_conf_uint, /* [한국어] write: uint 폭으로 기록. bps용 u64 핸들러를 쓰면 인접 필드까지 덮어쓰게 된다 */
	},
	{
		.name = "throttle.write_iops_device", /* [한국어] WRITE iops 한도 파일; 쓰기 초당 IO 수 제한 */
		.private = offsetof(struct throtl_grp, iops[WRITE]), /* [한국어] 쓰기 방향 iops 필드 위치 */
		.seq_show = tg_print_conf_uint, /* [한국어] read 핸들러 */
		.write = tg_set_conf_uint, /* [한국어] write 핸들러 */
	},
	{
		.name = "throttle.io_service_bytes", /* [한국어] 여기부터는 설정이 아니라 통계 파일이라 .write가 없다(읽기 전용) */
		.private = offsetof(struct throtl_grp, stat_bytes), /* [한국어] 어느 카운터를 찍을지 지정 */
		.seq_show = tg_print_rwstat, /* [한국어] 이 cgroup 자신의 값만 출력 */
	},
	{
		.name = "throttle.io_service_bytes_recursive", /* [한국어] 위 파일과 같은 카운터를 보되 */
		.private = offsetof(struct throtl_grp, stat_bytes), /* [한국어] 오프셋도 동일하고 */
		.seq_show = tg_print_rwstat_recursive, /* [한국어] 자손까지 합산해 출력한다는 점만 다르다 */
	},
	{
		.name = "throttle.io_serviced", /* [한국어] 바이트가 아니라 IO 개수 통계 */
		.private = offsetof(struct throtl_grp, stat_ios), /* [한국어] 개수 카운터를 가리킨다 */
		.seq_show = tg_print_rwstat, /* [한국어] 자기 값만 출력 */
	},
	{
		.name = "throttle.io_serviced_recursive", /* [한국어] 개수 통계의 자손 합산 버전 */
		.private = offsetof(struct throtl_grp, stat_ios), /* [한국어] 같은 카운터 */
		.seq_show = tg_print_rwstat_recursive, /* [한국어] 합산 출력 */
	},
	{ }	/* terminate */ /* [한국어] cftype 배열은 개수를 따로 넘기지 않고 .name이 NULL인 빈 항목으로 끝을 표시한다. 이 줄을 빠뜨리면 등록 루프가 배열 밖을 읽는다 */
};

/*
 * [한국어]
 * tg_prfill_limit - v2 cgroup io.max 형식으로 한 blkg의 4가지 limit을 출력.
 * @sf: seq_file
 * @pd: blkg_policy_data (throtl_grp)
 * @off: 사용 안 함 (cftype private 오프셋, 여기서는 불필요)
 * @return: 0 (아무것도 출력 안 함 포함)
 *
 * "major:minor rbps=N wbps=N riops=N wiops=N\n" 형식 또는 "max" 문자열 출력.
 * 4가지 모두 무제한이면 출력 생략.
 * 호출 체인: tg_print_limit() → blkcg_print_blkgs() → [tg_prfill_limit]
 */
static u64 tg_prfill_limit(struct seq_file *sf, struct blkg_policy_data *pd,
			 int off)
{
	struct throtl_grp *tg = pd_to_tg(pd); /* [한국어] blkg_policy_data → throtl_grp 변환 */
	const char *dname = blkg_dev_name(pd->blkg); /* [한국어] 장치 이름 (예: "8:0") 획득 */
	u64 bps_dft; /* [한국어] bps 무제한 기본값 (U64_MAX) */
	unsigned int iops_dft; /* [한국어] iops 무제한 기본값 (UINT_MAX) */

	if (!dname) /* [한국어] 장치 이름 없으면 출력 생략 (장치 미연결) */
		return 0;

	bps_dft = U64_MAX; /* [한국어] bps 무제한 기본값 설정 */
	iops_dft = UINT_MAX; /* [한국어] iops 무제한 기본값 설정 */

	if (tg->bps[READ] == bps_dft && /* [한국어] READ bps가 무제한이고 */
	    tg->bps[WRITE] == bps_dft && /* [한국어] WRITE bps가 무제한이고 */
	    tg->iops[READ] == iops_dft && /* [한국어] READ iops가 무제한이고 */
	    tg->iops[WRITE] == iops_dft) /* [한국어] WRITE iops도 무제한이면 */
		return 0; /* [한국어] 모두 무제한이면 출력 생략; 기본값과 동일 */

	seq_printf(sf, "%s", dname); /* [한국어] "major:minor" 장치 식별자 출력 */
	if (tg->bps[READ] == U64_MAX) /* [한국어] 내부 무제한 표식(U64_MAX)을 그대로 숫자로 찍으면 사용자에게는 18446744073709551615라는 의미 없는 값이 보인다 */
		seq_printf(sf, " rbps=max"); /* [한국어] 그래서 "max"라는 문자열로 바꿔 찍고, 쓰기 쪽(tg_set_limit)도 같은 문자열을 무제한으로 받아들여 왕복이 성립한다 */
	else /* [한국어] 실제로 제한이 걸려 있으면 숫자 그대로 */
		seq_printf(sf, " rbps=%llu", tg->bps[READ]); /* [한국어] 네 항목 모두 이 "무제한이면 max, 아니면 값" 형태를 반복한다 */

	if (tg->bps[WRITE] == U64_MAX) /* [한국어] WRITE bps 무제한이면 "max" 출력 */
		seq_printf(sf, " wbps=max");
	else /* [한국어] 설정된 WRITE bps 값 출력 */
		seq_printf(sf, " wbps=%llu", tg->bps[WRITE]);

	if (tg->iops[READ] == UINT_MAX) /* [한국어] READ iops 무제한이면 "max" 출력 */
		seq_printf(sf, " riops=max");
	else /* [한국어] 설정된 READ iops 값 출력 */
		seq_printf(sf, " riops=%u", tg->iops[READ]);

	if (tg->iops[WRITE] == UINT_MAX) /* [한국어] WRITE iops 무제한이면 "max" 출력 */
		seq_printf(sf, " wiops=max");
	else /* [한국어] 설정된 WRITE iops 값 출력 */
		seq_printf(sf, " wiops=%u", tg->iops[WRITE]);

	seq_printf(sf, "\n"); /* [한국어] 행 종료 문자 출력 */
	return 0;
}

/*
 * [한국어]
 * tg_print_limit - v2 cgroup io.max의 seq_show 핸들러; 전체 계층 limit 출력.
 * @sf: seq_file
 * @v: 사용 안 함
 * @return: 0
 *
 * 호출 체인: cftype.seq_show → [tg_print_limit] → blkcg_print_blkgs() → tg_prfill_limit()
 */
static int tg_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_limit, /* [한국어] 이 cgroup이 접해 본 디스크마다 한 줄씩, tg_prfill_limit이 io.max 형식으로 찍는다 */
			  &blkcg_policy_throtl, seq_cft(sf)->private, false); /* [한국어] 마지막 false는 rwstat 형식이 아니라는 뜻이다(통계 파일과 달리 읽기/쓰기를 별도 줄로 나누지 않고 한 줄에 네 값을 모두 담는다) */
	return 0;
}

/*
 * [한국어]
 * tg_set_limit - v2 cgroup io.max write 핸들러; rbps/wbps/riops/wiops를 한꺼번에 설정.
 * @of: kernfs_open_file; io.max cftype
 * @buf: 입력 문자열 (예: "8:0 rbps=10485760 wbps=max riops=100 wiops=max")
 * @nbytes: 입력 바이트 수
 * @off: 사용 안 함
 * @return: nbytes(성공) 또는 음수 에러 코드
 *
 * v2 통합 인터페이스로 이 장치의 cgroup별 읽기/쓰기 bps/iops를 원자적으로 갱신.
 * 기존 4가지 값을 백업한 뒤 파싱한 새 값을 적용하고 tg_conf_updated()로 slice 재시작.
 * "max" 키워드는 U64_MAX로 해석(무제한).
 * 실행 컨텍스트: kernfs write() 경로 (프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cftype.write → [tg_set_limit] → blk_throtl_init(), blkg_conf_prep(),
 *   tg_update_carryover(), tg_conf_updated()
 */
static ssize_t tg_set_limit(struct kernfs_open_file *of,
			  char *buf, size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of)); /* [한국어] kernfs cgroup css → blkcg; 호출한 cgroup 식별 */
	struct blkg_conf_ctx ctx; /* [한국어] blkg_conf 작업에 필요한 bdev/blkg 컨텍스트 */
	struct throtl_grp *tg; /* [한국어] 설정 대상 cgroup의 throtl_grp */
	u64 v[4]; /* [한국어] [0]=rbps [1]=wbps [2]=riops [3]=wiops; 기존값 백업 후 새 값 적용 */
	int ret; /* [한국어] 에러 코드 */

	blkg_conf_init(&ctx, buf); /* [한국어] blkg_conf_ctx 초기화; 입력 버퍼와 bdev 준비 */

	ret = blkg_conf_open_bdev(&ctx); /* [한국어] 입력 앞머리의 "major:minor"로 대상 장치를 연다. 이후 모든 실패는 out_finish를 거쳐 이 참조를 풀어야 한다 */
	if (ret)
		goto out_finish;

	if (!blk_throtl_activated(ctx.bdev->bd_queue)) { /* [한국어] v1 경로와 마찬가지로, 실제로 제한을 거는 이 순간에야 계층을 만든다 */
		ret = blk_throtl_init(ctx.bdev->bd_disk); /* [한국어] 이 디스크에 스로틀 계층을 붙인다 */
		if (ret)
			goto out_finish;
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, &ctx); /* [한국어] (cgroup, 디스크) 쌍의 blkg를 확보 */
	if (ret)
		goto out_finish;

	tg = blkg_to_tg(ctx.blkg); /* [한국어] 그 blkg에 딸린 스로틀 상태 */
	tg_update_carryover(tg); /* [한국어] 반드시 새 값을 쓰기 '전에' 호출해야 한다. 이 함수는 옛 상한을 읽어 정산하므로, 값을 먼저 바꾸면 새 상한으로 과거를 정산하는 잘못된 계산이 된다 */

	v[0] = tg->bps[READ]; /* [한국어] 현재 값으로 초기화해 두는 것이 핵심이다. io.max는 네 항목 중 일부만 적어 보낼 수 있는데, 언급되지 않은 항목은 기존 값을 그대로 유지해야 하기 때문 */
	v[1] = tg->bps[WRITE]; /* [한국어] 아래 파싱 루프는 언급된 항목만 덮어쓴다 */
	v[2] = tg->iops[READ]; /* [한국어] 또한 파싱 도중 오류로 빠져나가면 tg에는 아직 아무것도 쓰지 않은 상태라, 부분 적용 없이 원래 설정이 온전히 남는다 */
	v[3] = tg->iops[WRITE]; /* [한국어] 즉 이 배열이 '전부 성공하거나 전부 무효'를 보장하는 임시 버퍼 역할을 한다 */

	while (true) { /* [한국어] 공백으로 구분된 "키=값" 토큰을 하나씩 소비한다. 네 항목을 모두 적을 필요는 없고, 적힌 것만 덮어쓰는 부분 갱신 방식이다 */
		char tok[27];	/* wiops=18446744073709551616 */ /* [한국어] 크기 27은 가장 긴 입력을 담기 위한 값이다: 키 "wiops="(6) + u64 최대 20자리 + 널 종료(1) = 27. 아래 sscanf의 %26s와 짝을 이뤄 널 자리를 남긴다 */
		char *p; /* [한국어] strsep이 '=' 뒤쪽(값 부분)을 가리키도록 옮겨 놓을 포인터 */
		u64 val = U64_MAX; /* [한국어] 파싱된 limit 값; "max"이면 U64_MAX */
		int len; /* [한국어] sscanf가 소비한 바이트 수 */

		if (sscanf(ctx.body, "%26s%n", tok, &len) != 1) /* [한국어] 다음 토큰 파싱; cgroup QoS 설정 항목 하나 */
			break; /* [한국어] 더 이상 토큰이 없으면 파싱 종료 */
		if (tok[0] == '\0') /* [한국어] 빈 토큰이면 종료 */
			break;
		ctx.body += len; /* [한국어] 파싱 위치 진행; 다음 cgroup QoS 토큰으로 */

		ret = -EINVAL; /* [한국어] 기본 에러: 형식 오류 */
		p = tok; /* [한국어] 토큰 문자열을 strsep으로 분리하기 위한 임시 포인터 */
		strsep(&p, "="); /* [한국어] 첫 '='을 널 문자로 바꾸고 p를 그 다음 칸으로 옮긴다. 결과적으로 tok에는 키만 남고 p가 값을 가리킨다. 반환값을 버리는 이유는 그 값이 곧 tok(키)이라 이미 갖고 있기 때문 */
		if (!p || (sscanf(p, "%llu", &val) != 1 && strcmp(p, "max"))) /* [한국어] '='이 없으면 p가 NULL이다. 숫자로도 안 읽히고 "max"도 아니면 거부한다. val은 선언 시 U64_MAX로 초기화돼 있어, "max"인 경우에는 sscanf가 실패해도 그대로 무제한 값이 남는 구조 */
			goto out_finish;

		ret = -ERANGE; /* [한국어] 형식은 맞지만 값이 허용 범위를 벗어난 경우를 구분해 준다 */
		if (!val) /* [한국어] v1과 달리 v2에서는 0을 무제한으로 재해석하지 않는다. 0을 그대로 받으면 완전 차단이 되고, 무제한을 뜻하려면 "max"라는 명시적 표기가 따로 있기 때문에 0은 사용자 실수로 보고 거부한다 */
			goto out_finish;

		ret = -EINVAL; /* [한국어] 에러: 알 수 없는 키 이름 */
		if (!strcmp(tok, "rbps")) /* [한국어] strsep이 tok의 '='을 널로 바꿔 놓았으므로, 이제 tok은 키 이름만 담고 있다 */
			v[0] = val; /* [한국어] bps는 u64 폭 그대로 저장한다 */
		else if (!strcmp(tok, "wbps"))
			v[1] = val;
		else if (!strcmp(tok, "riops")) /* [한국어] iops는 저장 필드가 unsigned int라 */
			v[2] = min_t(u64, val, UINT_MAX); /* [한국어] UINT_MAX로 잘라 넣는다. 자르지 않고 대입하면 상위 비트가 날아가 사용자가 적은 큰 값이 엉뚱한 작은 제한으로 둔갑한다 */
		else if (!strcmp(tok, "wiops"))
			v[3] = min_t(u64, val, UINT_MAX); /* [한국어] 쓰기 방향도 동일하게 포화시킨다 */
		else /* [한국어] 알 수 없는 키는 조용히 무시하지 않고 거부한다. 오타를 넘겨 버리면 사용자는 제한이 걸린 줄 알지만 실제로는 안 걸린 상태가 된다 */
			goto out_finish;
	}

	tg->bps[READ] = v[0]; /* [한국어] READ bps 최종 적용; 하위 계층 READ 대역폭 제한 */
	tg->bps[WRITE] = v[1]; /* [한국어] WRITE bps 최종 적용; 하위 계층 WRITE 대역폭 제한 */
	tg->iops[READ] = v[2]; /* [한국어] READ iops 최종 적용; 하위 계층 READ 초당 명령 제한 */
	tg->iops[WRITE] = v[3]; /* [한국어] WRITE iops 최종 적용; 하위 계층 WRITE 초당 명령 제한 */

	tg_conf_updated(tg, false); /* [한국어] 4가지 limit 변경 후 slice 재시작; 유입 rate 즉시 재조정 */
	ret = 0; /* [한국어] 성공 */
out_finish:
	blkg_conf_exit(&ctx); /* [한국어] blkg_conf_ctx 정리; bdev 참조 해제 */
	return ret ?: nbytes; /* [한국어] 성공이면 nbytes, 실패면 에러 코드 반환 */
}

/* [한국어] throtl_files - v2 cgroup io.max 파일 정의 (blkcg.dfl_cftypes).
 * "io.max" 파일 하나로 rbps/wbps/riops/wiops를 동시에 설정할 수 있는 통합 인터페이스.
 * CFTYPE_NOT_ON_ROOT: root cgroup에는 노출하지 않음 (root는 제한 없음이 기본). */
static struct cftype throtl_files[] = {
	{
		.name = "max", /* [한국어] v2 io.max 파일; "rbps=N wbps=N riops=N wiops=N" 형식 */
		.flags = CFTYPE_NOT_ON_ROOT, /* [한국어] root cgroup에는 노출 안 함; root는 무제한이 기본 */
		.seq_show = tg_print_limit, /* [한국어] cat 시 현재 4가지 limit 출력 */
		.write = tg_set_limit, /* [한국어] echo 시 4가지 limit 파싱 및 적용 */
	},
	{ }	/* terminate */ /* [한국어] .name이 NULL인 빈 항목으로 배열의 끝을 표시한다 */
};

/*
 * [한국어]
 * throtl_shutdown_wq - blk-throttle dispatch_work를 동기적으로 취소한다.
 * @q: 대상 request_queue
 * @return: 없음 (void)
 *
 * blk_throtl_exit() 또는 blk_throtl_cancel_bios() 경로에서 호출되어
 * kthrotld workqueue의 dispatch_work가 완료될 때까지 대기한 뒤 취소한다.
 * 이 호출 이후에는 더 이상 throttle에서 하위 계층으로 bio가 흘러가지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (cancel_work_sync가 슬립 가능).
 *
 * 호출 체인:
 *   blk_throtl_exit() → [throtl_shutdown_wq] → cancel_work_sync()
 */
static void throtl_shutdown_wq(struct request_queue *q)
{
	struct throtl_data *td = q->td; /* [한국어] request_queue에 연결된 throtl_data; dispatch_work 소유 */

	cancel_work_sync(&td->dispatch_work); /* [한국어] kthrotld workqueue 정리; 하위 계층으로 bio를 보내는 work item 취소 */
}

/*
 * [한국어]
 * tg_flush_bios - cgroup offline 또는 장치 해제 시 throttle 큐의 bio를 강제 디스패치한다.
 * @tg: flush 대상 throtl_grp
 * @return: 없음 (void)
 *
 * THROTL_TG_CANCELING 플래그를 설정해 새 rate limit 검사를 건너뛰고,
 * pending_tree에 등록된 경우 disptime을 즉시로 만들어 하위 계층으로 빠르게 흘려보낸다.
 * del_gendisk() 이후 inflight IO가 남지 않도록 보장하는 안전장치.
 * THROTL_TG_PENDING가 없으면 early return하여 pending_tree 이중 삽입을 방지한다.
 * 실행 컨텍스트: spin_lock_irq 보유 상태 (IRQ disable).
 *
 * 호출 체인:
 *   throtl_pd_offline() / blk_throtl_cancel_bios() → [tg_flush_bios]
 *   → tg_update_disptime(), throtl_schedule_pending_timer()
 */
static void tg_flush_bios(struct throtl_grp *tg)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] tg의 service_queue; pending_timer 재예약 대상 */

	if (tg->flags & THROTL_TG_CANCELING) /* [한국어] 이미 취소 중이면 중복 flush 방지; 블록 장치 제거 중 */
		return;
	/*
	 * Set the flag to make sure throtl_pending_timer_fn() won't
	 * stop until all throttled bios are dispatched.
	 */
	tg->flags |= THROTL_TG_CANCELING; /* [한국어] 이 순간부터 tg_dispatch_bps_time()/tg_dispatch_iops_time()이 계산 없이 0을 돌려주어, 남은 bio가 제한에 걸리지 않고 전부 빠져나간다. 디스크가 사라지는 중이라 rate를 지키는 것보다 큐를 비우는 것이 우선이다 */

	/*
	 * Do not dispatch cgroup without THROTL_TG_PENDING or cgroup
	 * will be inserted to service queue without THROTL_TG_PENDING
	 * set in tg_update_disptime below. Then IO dispatched from
	 * child in tg_dispatch_one_bio will trigger double insertion
	 * and corrupt the tree.
	 */
	if (!(tg->flags & THROTL_TG_PENDING)) /* [한국어] pending_tree에 없으면 disptime 갱신/타이머 예약 불필요 */
		return;

	/*
	 * Update disptime after setting the above flag to make sure
	 * throtl_select_dispatch() won't exit without dispatching.
	 */
	tg_update_disptime(tg); /* [한국어] disptime을 즉시로 만들어 하위 계층으로 남은 bio를 강제 디스패치 */

	throtl_schedule_pending_timer(sq, jiffies + 1); /* [한국어] 1 jiffy 후 pending_timer 만료; 하위 계층으로 남은 bio를 빠르게 흘림 */
}

/*
 * [한국어]
 * throtl_pd_offline - blkcg_policy의 pd_offline_fn 콜백; cgroup offline 시 호출.
 * @pd: offline되는 cgroup의 blkg_policy_data
 * @return: 없음 (void)
 *
 * cgroup이 offline될 때 blk-cgroup 코어가 이 함수를 호출한다.
 * tg_flush_bios()로 해당 cgroup의 throttle 큐에 남은 bio를 강제 디스패치한다.
 * 실행 컨텍스트: IRQ disable, queue_lock 보유 상태.
 *
 * 호출 체인:
 *   blkcg offline → blkg_destroy() → pd_offline_fn → [throtl_pd_offline] → tg_flush_bios()
 */
static void throtl_pd_offline(struct blkg_policy_data *pd)
{
	tg_flush_bios(pd_to_tg(pd)); /* [한국어] offline되는 cgroup의 throtl 큐 강제 flush; 하위 계층으로 남은 bio 방출 */
}

/* [한국어] blkcg_policy_throtl - blk-cgroup에 등록되는 blk-throttle 정책 기술자.
 * dfl_cftypes/legacy_cftypes: v2(io.max)/v1(throttle.*) sysfs 파일 집합.
 * pd_alloc_fn/pd_init_fn: blkg 생성 시 throtl_grp 할당 및 초기화.
 * pd_online_fn: blkg가 online될 때 throtl_grp 연결 완료 처리.
 * pd_offline_fn: cgroup offline 시 throttle 큐 flush.
 * pd_free_fn: blkg 해제 시 throtl_grp 메모리 정리. */
struct blkcg_policy blkcg_policy_throtl = {
	.dfl_cftypes		= throtl_files, /* [한국어] v2 cgroup io.max 파일; rbps/wbps/riops/wiops 통합 설정 */
	.legacy_cftypes		= throtl_legacy_files, /* [한국어] v1 cgroup throttle.{read,write}_{bps,iops}_device 파일 */

	.pd_alloc_fn		= throtl_pd_alloc, /* [한국어] blkg 생성 시 throtl_grp NUMA-aware 할당 */
	.pd_init_fn		= throtl_pd_init, /* [한국어] blkg 초기화 시 throtl_grp 연결 및 slice 기본값 설정 */
	.pd_online_fn		= throtl_pd_online, /* [한국어] blkg online 시 상위 service_queue 연결 완료 */
	.pd_offline_fn		= throtl_pd_offline, /* [한국어] cgroup offline 시 throttle 큐 강제 flush */
	.pd_free_fn		= throtl_pd_free, /* [한국어] blkg 해제 시 throtl_grp 메모리 반환 */
};

/*
 * [한국어]
 * blk_throtl_cancel_bios - 디스크 해제 시 모든 하위 cgroup의 throttle 큐 bio를 강제 flush.
 * @disk: 해제 중인 gendisk; 블록 장치
 * @return: 없음 (void)
 *
 * del_gendisk() 경로에서 호출되어 블록 장치가 제거되기 전에 throtl 큐에 남아 있는
 * bio들을 전부 하위 계층 방향으로 흘려보내거나 상위로 전파한다.
 * blkg_for_each_descendant_post로 모든 하위 cgroup을 post-order로 순회하며
 * tg_flush_bios()를 호출한다. del_gendisk 이후 inflight IO가 없도록 보장.
 * 실행 컨텍스트: 프로세스 컨텍스트; spin_lock_irq로 IRQ disable.
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_cancel_bios] → tg_flush_bios()
 */
void blk_throtl_cancel_bios(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] 블록 장치의 request_queue */
	struct cgroup_subsys_state *pos_css; /* [한국어] blkg 순회를 위한 css 커서 */
	struct blkcg_gq *blkg; /* [한국어] 순회 중 현재 blkcg_gq */

	if (!blk_throtl_activated(q)) /* [한국어] throttle이 비활성화면 flush 할 것 없음; 스로틀 계층 없음 */
		return;

	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; 장치 큐와 throtl 구조 동시 보호 */
	/*
	 * queue_lock is held, rcu lock is not needed here technically.
	 * However, rcu lock is still held to emphasize that following
	 * path need RCU protection and to prevent warning from lockdep.
	 */
	rcu_read_lock(); /* [한국어] 원본 주석대로 queue_lock을 이미 쥐고 있어 기술적으로는 없어도 되지만, 아래 blkg 순회가 RCU 보호를 전제로 작성된 API라 lockdep 경고를 피하고 의도를 드러내기 위해 명시적으로 잡는다 */
	blkg_for_each_descendant_post(blkg, pos_css, q->root_blkg) { /* [한국어] 하위 cgroup 순회; 블록 장치에 연결된 모든 cgroup의 throttle 큐 처리 */
		/*
		 * disk_release will call pd_offline_fn to cancel bios.
		 * However, disk_release can't be called if someone get
		 * the refcount of device and issued bios which are
		 * inflight after del_gendisk.
		 * Cancel bios here to ensure no bios are inflight after
		 * del_gendisk.
		 */
		tg_flush_bios(blkg_to_tg(blkg)); /* [한국어] 각 cgroup의 throttle 큐 flush; 하위 계층으로 남은 bio 강제 전달 또는 폐기 준비 */
	}
	rcu_read_unlock(); /* [한국어] 순회 종료 */
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제; 장치 큐 처리 재개 */
}

/*
 * [한국어]
 * tg_within_limit - bio가 현재 throtl_grp의 bps/iops 제한 안에 있는지 판단한다.
 * @tg: 검사 대상 throtl_grp
 * @bio: 검사할 bio
 * @rw: READ(0) 또는 WRITE(1)
 * @return: true면 발행 가능, false면 throtl 큐에 대기 필요
 *
 * bps/iops 두 단계 제한을 FIFO 순서로 검사한다.
 * BIO_BPS_THROTTLED가 설정된 분할 bio는 bps 단계를 건너뛰고 iops만 검사.
 * bps 큐가 비어 있고 bio가 bps 제한 내이면, bps 사용량을 선차감하고
 * iops 큐로 직접 보낸다.
 * 이미 대기 중인 bio가 있으면 FIFO 순서를 지키기 위해 항상 false 반환.
 * 실행 컨텍스트: spin_lock_irq(queue_lock) 보유 상태.
 *
 * 호출 체인:
 *   __blk_throtl_bio() → [tg_within_limit] → tg_dispatch_bps_time(),
 *   tg_dispatch_iops_time(), throtl_charge_bps_bio()
 */
static bool tg_within_limit(struct throtl_grp *tg, struct bio *bio, bool rw)
{
	struct throtl_service_queue *sq = &tg->service_queue; /* [한국어] 현재 cgroup의 service_queue; 발행 전 bio 대기 상태 */

	/*
	 * For a split bio, we need to specifically distinguish whether the
	 * iops queue is empty.
	 */
	if (bio_flagged(bio, BIO_BPS_THROTTLED)) /* [한국어] 이 bio는 이전에 모든 계층의 bps를 통과한 뒤 분할되어 되돌아온 것이다. 남은 관문은 iops뿐이므로 */
		return sq->nr_queued_iops[rw] == 0 && /* [한국어] '전체 대기 수'가 아니라 iops 큐만 본다. bps 큐에 대기가 있어도 이 bio는 그 줄에 서지 않으므로, 전체를 보면 끼어들기가 아닌데도 부당하게 막힌다 */
				tg_dispatch_iops_time(tg, bio) == 0; /* [한국어] 앞에 아무도 없고 예산도 남았을 때만 즉시 통과 */

	/*
	 * Throtl is FIFO - if bios are already queued, should queue.
	 * If the bps queue is empty and @bio is within the bps limit, charge
	 * bps here for direct placement into the iops queue.
	 */
	if (sq_queued(&tg->service_queue, rw)) { /* [한국어] 동일 방향에 이미 대기 bio가 있으면 FIFO 순서로 발행 지연 */
		if (sq->nr_queued_bps[rw] == 0 && /* [한국어] 어차피 큐에 들어갈 bio지만, bps 관문을 지금 통과할 수 있다면 미리 통과시켜 둔다 */
		    tg_dispatch_bps_time(tg, bio) == 0) /* [한국어] 그러면 이 bio는 bps 큐를 건너뛰고 바로 iops 큐로 들어가, 나중에 두 관문을 순차로 거치며 두 번 기다리는 일을 피한다 */
			throtl_charge_bps_bio(tg, bio); /* [한국어] bps 사용량을 선차감; 이후 iops 큐에서 초당 IO 제한만 검사 */

		return false; /* [한국어] FIFO: 이미 대기 중인 bio가 있으면 현재 bio도 발행 지연 */
	}

	return tg_dispatch_time(tg, bio) == 0; /* [한국어] 앞에 대기 중인 bio가 없는 일반적인 경우: 두 관문을 순서대로 통과하면 true(즉시 진행), 하나라도 걸리면 false(큐에 넣어라) */
}

/*
 * [한국어]
 * __blk_throtl_bio - bio가 blk-throttle 계층을 통과할 수 있는지 검사하고, 초과 시 큐잉한다.
 * @bio: 검사 대상 bio
 * @return: true면 throttled(발행 지연), false면 즉시 진입 가능
 *
 * submit_bio() → blk_mq_submit_bio() → blk_throtl_bio() 경로로 호출되어
 * cgroup 계층을 bottom-up으로 순회하며 각 throtl_grp의 bps/iops 제한을 검사한다.
 * 모든 제한을 통과하면 bio에 BIO_BPS_THROTTLED를 설정하고 false(비throttle)를 반환,
 * 호출자는 bio를 그대로 하위 계층으로 흘려보낸다.
 * 제한 초과 시 throtl 큐에 넣고 disptime 이후 pending_timer가 재디스패치한다.
 * root 권한 IO(bio_issue_as_root_blkg)는 rate 초과라도 즉시 통과(부채 추적).
 * 실행 컨텍스트: softirq 또는 프로세스 컨텍스트; spin_lock_irq + RCU read lock.
 *
 * 호출 체인:
 *   submit_bio() → blk_mq_submit_bio() → blk_throtl_bio() → [__blk_throtl_bio]
 *   → tg_within_limit(), throtl_add_bio_tg(), tg_update_disptime(),
 *   throtl_schedule_next_dispatch()
 */
bool __blk_throtl_bio(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev); /* [한국어] bio가 속한 블록 장치의 request_queue 획득 */
	struct blkcg_gq *blkg = bio->bi_blkg; /* [한국어] bio의 blkcg_gq; 장치별 cgroup queue 상태 */
	struct throtl_qnode *qn = NULL; /* [한국어] 부모로 전달할 qnode 포인터; 발행 전 cgroup 계층 이동용 */
	struct throtl_grp *tg = blkg_to_tg(blkg); /* [한국어] bio가 속한 cgroup의 throtl_grp; rate limit 상태 */
	struct throtl_service_queue *sq; /* [한국어] 현재 검사 중인 service_queue; 발행 전 관문 */
	bool rw = bio_data_dir(bio); /* [한국어] bio의 READ/WRITE 방향; 장치 큐 방향 */
	bool throttled = false; /* [한국어] bio가 throttle되어 발행이 지연되었는지 결과 */
	struct throtl_data *td = tg->td; /* [한국어] throtl_data; 디스크(request_queue) 단위 dispatch_work/pending_timer */

	rcu_read_lock(); /* [한국어] RCU read lock; bio->bi_blkg 및 cgroup hierarchy가 해제되지 않도록 보호 */
	spin_lock_irq(&q->queue_lock); /* [한국어] request_queue_lock 획득; 장치 큐 구조와 throtl 상태 동시 보호 */
	sq = &tg->service_queue; /* [한국어] 현재 cgroup의 service_queue; 발행 전 대기열 */

	while (true) { /* [한국어] cgroup hierarchy를 bottom-up으로 순회; 모든 rate limit 통과 필요 */
		if (tg_within_limit(tg, bio, rw)) { /* [한국어] 현재 cgroup의 bps/iops 제한 안에 있으면 발행 가능 */
			/* within limits, let's charge and dispatch directly */
			throtl_charge_iops_bio(tg, bio); /* [한국어] 큐를 거치지 않고 바로 통과하는 경로라, 여기서 개수를 세지 않으면 이 bio는 iops 회계에서 통째로 빠진다 */

			/*
			 * We need to trim slice even when bios are not being
			 * queued otherwise it might happen that a bio is not
			 * queued for a long time and slice keeps on extending
			 * and trim is not called for a long time. Now if limits
			 * are reduced suddenly we take into account all the IO
			 * dispatched so far at new low rate and * newly queued
			 * IO gets a really long dispatch time.
			 *
			 * So keep on trimming slice even if bio is not queued.
			 */
			throtl_trim_slice(tg, rw); /* [한국어] slice 정리; rate limit 시간 윈도우 보정 */
		} else if (bio_issue_as_root_blkg(bio)) { /* [한국어] 우선순위 역전을 막기 위한 예외다. 메타데이터 갱신이나 저널 쓰기처럼 다른 cgroup의 진행까지 막고 있는 IO를 여기서 붙잡으면, 제한 대상이 아닌 태스크들까지 그 락을 기다리며 멈춘다. 그래서 예산을 넘겨도 통과시키되 사용량에는 기록해 이후에 갚게 한다 */
			/*
			 * IOs which may cause priority inversions are
			 * dispatched directly, even if they're over limit.
			 *
			 * Charge and dispatch directly, and our throttle
			 * control algorithm is adaptive, and extra IO bytes
			 * will be throttled for paying the debt
			 */
			throtl_charge_bps_bio(tg, bio); /* [한국어] root IO에도 bps 사용량 기록; 대역폭 제한 추적 */
			throtl_charge_iops_bio(tg, bio); /* [한국어] root IO에도 iops 사용량 기록; 초당 IO 추적 (나중에 상환) */
		} else {
			/* if above limits, break to queue */
			break; /* [한국어] 제한 초과: 발행 중단하고 throtl 큐에 bio 적재 */
		}

		/*
		 * @bio passed through this layer without being throttled.
		 * Climb up the ladder.  If we're already at the top, it
		 * can be executed directly.
		 */
		qn = &tg->qnode_on_parent[rw]; /* [한국어] 부모 service_queue의 qnode 선택; rate limit 관문을 한 단계 올라감 */
		sq = sq->parent_sq; /* [한국어] parent_sq로 이동; 발행 전 상위 cgroup 제한 검사 */
		tg = sq_to_tg(sq); /* [한국어] 상위 service_queue의 throtl_grp 획득; 최상위면 NULL */
		if (!tg) { /* [한국어] 최상위 service_queue에 도달하면 모든 rate limit 통과 */
			bio_set_flag(bio, BIO_BPS_THROTTLED); /* [한국어] 모든 throtl_grp 통과, 하위 계층으로 진입 가능 */
			goto out_unlock; /* [한국어] BIO_BPS_THROTTLED 설정; blk-mq -> 블록 드라이버로 직접 전달 가능 */
		}
	}

	/* out-of-limit, queue to @tg */
	/* [한국어] bio가 붙잡히는 순간의 상태를 통째로 남긴다. 이 한 줄에 판정에 쓰인 모든 값이
	 * 들어 있어(사용량 bdisp/iodisp, 이번 bio 크기 sz, 적용된 상한 bps/iops, 양방향 대기 수),
	 * "왜 이 bio가 지금 막혔는가"를 다른 정보 없이 이 줄만으로 재구성할 수 있다.
	 * 붙잡힌 뒤에 찍는 이유는, 위 루프를 빠져나온 시점의 tg가 실제로 막은 계층이기 때문이다. */
	throtl_log(sq, "[%c] bio. bdisp=%llu sz=%u bps=%llu iodisp=%u iops=%u queued=%d/%d",
		   rw == READ ? 'R' : 'W',
		   tg->bytes_disp[rw], bio->bi_iter.bi_size,
		   tg_bps_limit(tg, rw),
		   tg->io_disp[rw], tg_iops_limit(tg, rw),
		   sq_queued(sq, READ), sq_queued(sq, WRITE));

	td->nr_queued[rw]++; /* [한국어] 발행이 지연된 bio 수 증가 */
	throtl_add_bio_tg(bio, qn, tg); /* [한국어] bio를 throtl 큐에 추가; 하위 계층 유입을 지연시키는 소프트웨어 관문 */
	throttled = true; /* [한국어] bio가 throttled 됨; 하위 계층으로 즉시 진입하지 않음 */

	/*
	 * Update @tg's dispatch time and force schedule dispatch if @tg
	 * was empty before @bio, or the iops queue is empty and @bio will
	 * add to.  The forced scheduling isn't likely to cause undue
	 * delay as @bio is likely to be dispatched directly if its @tg's
	 * disptime is not in the future.
	 */
	if (tg->flags & THROTL_TG_WAS_EMPTY || /* [한국어] 이 bio가 빈 큐의 첫 손님인 경우에만 강제 재예약을 한다. 큐가 이미 차 있었다면 그 그룹의 disptime과 타이머는 앞선 bio 때문에 이미 올바르게 걸려 있어, 다시 걸어 봐야 같은 시각으로 덮어쓰는 낭비다 */
	    tg->flags & THROTL_TG_IOPS_WAS_EMPTY) { /* [한국어] bps 큐는 차 있었지만 iops 큐만 비어 있던 경우를 따로 잡는 신호 */
		tg_update_disptime(tg); /* [한국어] 새 bio에 대해 다음 발행 시각(disptime) 재계산 */
		throtl_schedule_next_dispatch(tg->service_queue.parent_sq, true); /* [한국어] force=true. 이 경로는 bio 제출 컨텍스트라 여기서 디스패치 루프를 돌 수 없으므로, disptime이 이미 지났더라도 타이머를 걸어 타이머 컨텍스트에 넘긴다. 원본 주석이 지적하듯 그 지연은 보통 아주 짧다 */
	}

out_unlock:
	spin_unlock_irq(&q->queue_lock); /* [한국어] request_queue_lock 해제; 블록 드라이버가 CQ/ISR 처리 진행 가능 */

	rcu_read_unlock(); /* [한국어] RCU read unlock; bio/cgroup 구조 참조 종료 */
	return throttled; /* [한국어] throttled 여부 반환; true면 발행 지연, false면 즉시 진입 */
}

/*
 * [한국어]
 * blk_throtl_exit - gendisk에 연결된 blk-throttle 상태를 해제하고 자원을 반환한다.
 * @disk: 해제 중인 gendisk; 블록 장치
 * @return: 없음 (void)
 *
 * del_gendisk() 흐름에서 호출되어 블록 장치가 제거될 때 throtl_data,
 * pending_timer, dispatch_work를 정리한다.
 * blkg_destroy_all()이 먼저 정책을 비활성화하므로, 여기서는 throtl_data 존재 여부만
 * 확인하고 pending_timer 동기적 삭제 → dispatch_work 취소 → throtl_data 해제 순으로 처리.
 * 실행 컨텍스트: 프로세스 컨텍스트; timer_delete_sync/cancel_work_sync 슬립 가능.
 *
 * 호출 체인:
 *   del_gendisk() → [blk_throtl_exit] → timer_delete_sync(), throtl_shutdown_wq(), kfree()
 */
void blk_throtl_exit(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] 블록 장치의 request_queue; throtl_data 연결점 */

	/*
	 * blkg_destroy_all() already deactivate throtl policy, just check and
	 * free throtl data.
	 */
	if (!q->td) /* [한국어] throtl_data가 없으면 정리할 것 없음; 블록 장치에 throttle 계층 없음 */
		return;

	timer_delete_sync(&q->td->service_queue.pending_timer); /* [한국어] 반드시 아래 kfree보다 먼저, 그리고 동기적으로 멈춰야 한다. 실행 중인 콜백이 남아 있으면 해제된 td를 계속 참조한다 */
	throtl_shutdown_wq(q); /* [한국어] kthrotld dispatch_work 취소; 하위 계층으로의 bio 전달 중단 */
	kfree(q->td); /* [한국어] throtl_data 메모리 해제; 블록 장치 throttle 상태 제거 */
}

/*
 * [한국어]
 * throtl_init - blk-throttle 모듈 초기화; kthrotld workqueue 생성 및 정책 등록.
 * @return: blkcg_policy_register() 반환값; 0이면 성공
 *
 * 커널 부팅 시 module_init()을 통해 한 번 호출된다.
 * kthrotld(WQ_MEM_RECLAIM) workqueue를 생성하고 blkcg_policy_throtl를 등록한다.
 * kthrotld_workqueue에서 blk_throtl_dispatch_work_fn()이 실행되어 throttle된 bio를
 * 하위 계층 방향으로 내보낸다.
 * workqueue 생성 실패 시 panic() - 부팅 초기의 이 실패는 복구 수단이 없다.
 * 실행 컨텍스트: 커널 초기화 (프로세스 컨텍스트, 단일 CPU).
 *
 * 호출 체인:
 *   module_init → [throtl_init] → alloc_workqueue(), blkcg_policy_register()
 */
static int __init throtl_init(void)
{
	kthrotld_workqueue = alloc_workqueue("kthrotld", WQ_MEM_RECLAIM, 0); /* [한국어] kthrotld workqueue 생성; 하위 계층으로 bio를 전달하는 데몬 */
	if (!kthrotld_workqueue) /* [한국어] WQ_MEM_RECLAIM으로 만드는 이유: 라이트백/스왑 경로의 bio가 여기서 붙잡힐 수 있는데, 그 bio를 풀어 주는 일꾼이 메모리 부족으로 진행하지 못하면 메모리 회수가 이 워크큐를 기다리고 워크큐는 메모리를 기다리는 교착이 된다. 이 플래그가 전용 구조 태스크를 예약해 그 고리를 끊는다 */
		panic("Failed to create kthrotld\n"); /* [한국어] 부팅 초기의 워크큐 생성 실패는 사실상 메모리가 전혀 없다는 뜻이라 복구할 방법이 없다. 게다가 이 워크큐가 없으면 나중에 붙잡힌 bio를 풀어 줄 주체가 사라져, 스로틀을 켠 순간 IO가 영구히 멈춘다 - 조용히 진행하는 것보다 즉시 멈추는 편이 안전하다 */

	return blkcg_policy_register(&blkcg_policy_throtl); /* [한국어] cgroup 파일(io.max, blkio.throttle.*)이 여기서 노출된다. 다만 등록만으로 어떤 디스크에도 스로틀 계층이 생기지는 않는다 - 실제 throtl_data 생성은 사용자가 처음 제한을 걸 때 blk_throtl_init()에서 일어난다 */
}

module_init(throtl_init);

/* [한국어] 파일 전체 요약 - 이 계층을 한 문단으로 다시 정리한다
 *
 * - 위치: submit_bio() → blk_mq_submit_bio() → blk_throtl_bio() 지점에서,
 *   bio가 request로 바뀌기 전에 개입한다. 따라서 스로틀에 걸린 bio는 tag도
 *   할당받지 않고 드라이버 큐에도 전혀 부담을 주지 않는다. 이 파일은 특정
 *   장치 종류에 대해 아무것도 알지 않으며, NVMe든 SCSI든 loop든 동일하게
 *   동작한다.
 *
 * - 제한 방식: 토큰 버킷이지만 남은 토큰을 세지 않는다. (slice_start,
 *   bytes_disp/io_disp) 한 쌍만 두고, 필요할 때마다
 *   "경과 시간 × 상한 - 사용량"으로 예산을 계산한다. 시간이 흐르면 예산이
 *   저절로 늘어나는 구조라 별도의 토큰 보충 타이머가 필요 없다.
 *   throtl_trim_slice()는 사용량과 slice_start를 같은 폭만큼 함께 줄여
 *   이 식의 값을 보존하면서 숫자만 작게 유지한다.
 *
 * - 붙잡을지 말지: tg_within_limit()이 판정한다. 이미 같은 방향에 대기 중인
 *   bio가 있으면 예산과 무관하게 큐에 넣는다(FIFO 유지). 대기가 없으면
 *   bps → iops 순으로 검사해 둘 다 통과할 때만 즉시 통과시킨다.
 *   bio_issue_as_root_blkg()로 표시된 IO는 우선순위 역전을 막기 위해 예산을
 *   넘겨도 통과시키고, 대신 사용량에 기록해 나중에 갚게 한다.
 *
 * - 계층: service_queue 트리가 cgroup 트리를 그대로 반영한다. bio는 자기
 *   그룹의 큐에서 출발해 부모 방향으로 한 칸씩 올라가며 각 계층의 제한을
 *   다시 받고, 루트(throtl_data의 sq)에 닿아야 비로소 발행 대상이 된다.
 *   자식은 부모 큐에 자기 전용 qnode를 걸어 두어, 부모가 슬롯을 돌려가며
 *   꺼내는 것으로 형제 간 공평성이 만들어진다.
 *
 * - 다시 흐르게 하는 경로: 각 sq의 pending_timer가 최좌단 자식의 disptime에
 *   맞춰 깨어나 throtl_select_dispatch()를 돌리고, 루트까지 올라온 bio는
 *   kthrotld workqueue의 blk_throtl_dispatch_work_fn()이
 *   submit_bio_noacct_nocheck()로 다시 흘려보낸다. 타이머 컨텍스트(softirq)와
 *   발행 컨텍스트(kworker)를 나눈 이유는 발행 경로가 무겁고 슬립할 수 있기
 *   때문이다.
 *
 * - 정확도의 한계: 사용량은 '발행 시점'에 기록되며 장치가 실제로 완료한
 *   시점과는 무관하다. 즉 이 계층이 제어하는 것은 하위 계층으로 내보내는
 *   유입률이지 장치의 실측 완료 처리율이 아니다. 또한 시간 해상도가 jiffies
 *   (기본 슬라이스 100ms)이므로, 장치가 빠를수록 이 입자도와 타이머 깨우기
 *   지연이 상대적으로 큰 오차로 드러난다.
 *
 * - 이웃: blk-cgroup(정책 등록과 blkg 생명주기), blk-cgroup-rwstat(통계),
 *   그리고 blk-iolatency 같은 다른 blkcg 정책과 같은 큐 위에서 공존한다.
 */
