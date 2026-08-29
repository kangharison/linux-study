/*
 * [한국어 설명] block-mq 서브시스템 내부 전용 공유 헤더 — hctx/ctx/tag 자료구조 (blk-mq.h)
 *
 * === 파일의 역할 ===
 * blk-mq.h 는 블록 계층 멀티큐(Multi-Queue, 이하 MQ) 서브시스템의 "내부 전용"
 * 헤더다. include/linux/blk-mq.h 가 드라이버(NVMe/SCSI 등)에게 공개하는 API를
 * 정의한다면, 이 파일은 block/blk-mq.c, block/blk-mq-tag.c, block/blk-mq-sched.c,
 * block/blk-mq-sysfs.c, 그리고 각 IO 스케줄러(block/bfq-iosched.c,
 * block/mq-deadline.c, block/kyber-iosched.c)가 서로 주고받는 내부 자료구조와
 * 헬퍼 함수를 정의한다. 소프트웨어 큐(struct blk_mq_ctx), 하드웨어 큐
 * (struct blk_mq_hw_ctx, 이하 hctx), tag(드라이버가 요청 슬롯을 식별하는 번호 —
 * NVMe라면 SQ 엔트리에 대응하는 CID(Command Identifier)) 할당 매개변수
 * (struct blk_mq_alloc_data), dispatch 시점의 budget/active-request 카운팅
 * 헬퍼가 모두 이 파일에 모여 있다. 드라이버 자체는 이 헤더를 포함하지 않고,
 * block 레이어 내부 구현 파일들만 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트: 이 헤더의 static inline 함수 대부분은 bio 제출(submit) 경로와
 * hctx dispatch 경로에서 호출되므로 주로 프로세스 컨텍스트에서 실행되며,
 * 일부(active-request 카운트, tag 반환, hctx 상태 검사)는 softirq/인터럽트
 * 컨텍스트의 완료(completion) 경로에서도 호출될 수 있다.
 *
 * [블록 계층 전체 흐름과 이 헤더의 위치]
 *   응용/파일시스템 -> submit_bio() -> blk_mq_submit_bio() (blk-mq.c)
 *     -> blk_mq_get_ctx()/blk_mq_map_queue() (본 헤더)   : CPU -> sw큐 -> hw큐 결정
 *     -> blk_mq_get_driver_tag() (본 헤더)               : tag(NVMe라면 CID) 확보
 *     -> blk_mq_dispatch_rq_list() (blk-mq.c)            : hctx -> 드라이버 queue_rq
 *     -> ops->queue_rq() (예: nvme_queue_rq)             : SQ 엔트리 작성/doorbell
 *     <- 드라이버 완료 통보 -> blk_mq_put_driver_tag() (본 헤더) : tag 반환
 *
 * 이 헤더가 정의하는 blk_mq_ctx/blk_mq_hw_ctx 는 request_queue 생성 시
 * blk_mq_init_allocated_queue()(blk-mq.c)가 초기화하며, IO 스케줄러가 붙으면
 * blk-mq-sched.c 의 insert/dispatch 경로가 hctx->sched_tags 를 통해 이 헤더가
 * 제공하는 tag 헬퍼들을 호출한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(포함/참조):
 *   include/linux/blk-mq.h : blk_mq_hw_ctx, blk_mq_queue_map, blk_mq_tags,
 *                            sbitmap_queue 등 공개 자료구조 정의 — 본 파일은
 *                            그 내부 필드를 직접 참조한다.
 *   blk-stat.h              : IO 지연/크기 통계 자료구조 선언(직접 사용은
 *                            blk-mq.c 쪽에서 이루어진다).
 * 피의존(이 헤더를 포함하는 쪽):
 *   blk-mq.c        : request 할당/삽입/디스패치/완료 전체 경로 — 이 헤더의
 *                     거의 모든 심볼을 사용하는 핵심 소비자.
 *   blk-mq-tag.c    : blk_mq_get_tag()/put_tag(), hctx_may_queue() 등 tag
 *                     배분 정책의 실제 구현.
 *   blk-mq-sched.c  : blk_mq_tags_from_data() 로 스케줄러 tag 와 드라이버
 *                     tag 중 무엇을 쓸지 선택.
 *   blk-mq-sysfs.c  : blk_mq_sysfs_*() 로 hctx/ctx 의 sysfs 노드 관리.
 *   elevator.c, bfq-iosched.c, mq-deadline.c, kyber-iosched.c :
 *                     blk_mq_ctx/blk_mq_hw_ctx 순회 및 tag 예약에 이 헤더의
 *                     인라인 헬퍼를 사용.
 * 핵심 공유 자료구조와 관계:
 *   blk_mq_ctx --(index_hw[]/hctxs[] 로 매핑)--> blk_mq_hw_ctx
 *   blk_mq_hw_ctx --(tags/sched_tags 포인터)--> blk_mq_tags
 *   blk_mq_alloc_data --(q/ctx/hctx 를 in/out 으로 실어 나름)--> 위 세 구조체 전부
 *
 * === 주요 함수/구조체 요약 ===
 *   struct blk_mq_ctx        : CPU별 소프트웨어 큐 — rq_lists/cpu/hctxs 등을 보유
 *   struct blk_mq_alloc_data : tag/request 할당 시 q/ctx/hctx/flags 를 실어 나르는
 *                              in/out 매개변수 묶음
 *   blk_mq_map_queue()       : (cmd_flags, ctx) -> hctx 매핑, dispatch 대상 결정
 *   blk_mq_get_driver_tag()/blk_mq_put_driver_tag() : 드라이버 tag(요청 슬롯) 확보/반환
 *   hctx_may_queue()         : 공유 tag 환경에서 hctx 별 공정 tag 배분 여부 판단
 *   blk_mq_run_dispatch_ops(): RCU/SRCU 로 감싼 dispatch 콜백 실행 매크로
 */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef INT_BLK_MQ_H	/* [한국어] 매크로 정의: 헤더 중복 포함 방지(include guard) 시작 조건 — 이 파일을 여러 번 include 해도 재정의 오류가 나지 않도록 막는다 */
#define INT_BLK_MQ_H	/* [한국어] 매크로 정의: 위 #ifndef 와 짝을 이루는 include guard 토큰 — 최초 include 시 한 번만 정의되어 이후 재포함을 차단 */

#include <linux/blk-mq.h>	/* [한국어] block 레이어가 드라이버(NVMe 등)에 공개하는 API 헤더 — blk_mq_hw_ctx/blk_mq_queue_map/blk_mq_tags 등 본 파일이 확장하는 공개 자료구조가 여기 선언됨 */
#include "blk-stat.h"		/* [한국어] block 레이어 내부 IO 통계(blk_stat_*) 선언 — request 완료 지연/처리량 이동평균 계산에 쓰이며, 본 파일이 정의하는 dispatch 경로와 연동 */

struct blk_mq_tag_set;		/* [한국어] 전방 선언: 드라이버 하나(NVMe 컨트롤러 등)가 소유하는 tag/hctx 전역 설정 묶음 — 정의는 include/linux/blk-mq.h, 본 파일은 포인터로만 참조 */
struct elevator_tags;		/* [한국어] 전방 선언: IO 스케줄러(mq-deadline/kyber/bfq)가 쓰는 스케줄러 전용 tag 풀 — blk_mq_update_nr_requests() 등에서 포인터로만 다룸 */

/*
 * [한국어] struct blk_mq_ctxs — request_queue 하나가 소유하는, 모든 CPU의
 * blk_mq_ctx(소프트웨어 큐)를 한데 묶는 최상위 컨테이너.
 *
 * request_queue 는 CPU 개수만큼의 blk_mq_ctx 를 per-CPU 변수로 할당해 두는데,
 * 이 per-CPU 배열 자체의 sysfs kobject 생명주기와 메모리 해제 시점을 관리할
 * 단일 소유자가 필요하다. blk_mq_ctxs 가 그 역할을 하며, request_queue->queue_ctx
 * 필드가 이 구조체를 가리킨다. NVMe 관점에서는 여러 CPU 코어가 동일한
 * request_queue(하나의 NVMe 네임스페이스/디스크)를 통해 동시에 I/O 를 제출할 때,
 * 각 CPU 전용 제출 큐(ctx)들을 모아 놓은 상위 컨테이너로 볼 수 있다.
 */
struct blk_mq_ctxs {
	struct kobject kobj;
	/* [한국어] /sys/block/<disk>/mq/ 아래에 생성되는 이 컨테이너의 sysfs kobject.
	 * 설정자: blk_mq_sysfs_init()(blk-mq-sysfs.c)이 kobject_init()으로 초기화하고
	 *         request_queue->mq_kobj 아래에 매단다.
	 * 읽는 자: kobject 상속 체계(kobject release 콜백, sysfs show/store)와
	 *         blk_mq_sysfs_deinit()이 참조 카운트 해제 시 사용.
	 * 값 범위: 유효한 kobject (queue_ctx 배열이 살아있는 동안 항상 유효).
	 * 동기화: kobject 자체의 refcount(kref)로 생명주기를 관리하며, 별도의
	 *         외부 락 없이 get/put 호출만으로 참조를 주고받는다. */

	struct blk_mq_ctx __percpu	*queue_ctx;
	/* [한국어] CPU 개수만큼 존재하는 blk_mq_ctx 배열의 per-CPU 포인터.
	 * 설정자: blk_mq_alloc_ctxs()(blk-mq.c)가 alloc_percpu()로 할당하고
	 *         각 per-CPU 슬롯을 blk_mq_init_cpu_queues()에서 초기화한다.
	 * 읽는 자: __blk_mq_get_ctx()/blk_mq_get_ctx()(본 파일)가 per_cpu_ptr()로
	 *         현재 CPU에 대응하는 blk_mq_ctx를 얻어 제출 경로 진입점으로 삼는다.
	 * 값 범위: 유효한 per-CPU 포인터 — request_queue 해제 전까지 NULL이 아니다.
	 * 동기화: per-CPU 데이터이므로 같은 CPU 안에서는 자연히 직렬화되며,
	 *         다른 CPU가 남의 slot을 읽는 것은 초기화 이후에는 읽기 전용이라
	 *         별도 락이 필요 없다(구조 자체를 재할당하는 경로는 freeze로 보호). */
};

/**
 * struct blk_mq_ctx - State for a software queue facing the submitting CPUs
 *
 * [한국어] blk_mq_ctx — 제출 측 CPU 하나를 마주보는 소프트웨어 큐 상태.
 *
 * 이 구조체는 CPU 하나에 대응하는 "제출 진입점"이다. blk_mq_submit_bio()가
 * 만든 request는 먼저 자신을 제출한 CPU의 blk_mq_ctx에 잠깐 머무른 뒤,
 * blk_mq_map_queue()를 거쳐 실제 하드웨어 큐(struct blk_mq_hw_ctx, 이하 hctx)로
 * 넘어간다. NVMe 관점에서는 hctx 하나가 보통 nvme_queue(SQ/CQ 쌍) 하나에
 * 대응하므로, blk_mq_ctx는 "이 CPU가 어느 SQ로 명령을 낼지 결정되기 전, 잠시
 * 모이는 대기 지점"에 해당한다. request_queue 는 possible CPU 개수만큼의
 * blk_mq_ctx를 per-CPU 변수로 갖고, blk_mq_ctxs가 이들을 컨테이너로 묶는다.
 */
struct blk_mq_ctx {
	struct {
		spinlock_t		lock;
		/* [한국어] 아래 rq_lists를 보호하는 스핀락.
		 * 설정자/초기화자: blk_mq_init_cpu_queues()(blk-mq.c)가
		 *   spin_lock_init()으로 초기화.
		 * 잠그는 자: blk_mq_insert_request()가 새 request를 rq_lists에
		 *   넣을 때, blk_mq_flush_busy_ctxs()/blk_mq_dequeue_from_ctx()가
		 *   dispatch를 위해 꺼낼 때 잡는다.
		 * 값 범위: 0(unlocked)/1(locked) 상태를 갖는 일반 spinlock_t.
		 * 동기화: 같은 ctx에 대해 여러 CPU가 동시에 request를 밀어 넣거나
		 *   빼낼 수 있으므로 반드시 필요하다 — 이 lock이 없으면 리스트
		 *   포인터 갱신이 경합해 리스트가 깨질 수 있다. 잠그는 범위는
		 *   최소화되어 있어(리스트 삽입/삭제만) 홀드 시간이 짧다. */
		struct list_head	rq_lists[HCTX_MAX_TYPES];
		/* [한국어] hctx 유형(HCTX_TYPE_DEFAULT/READ/POLL)별로 분리된
		 * 대기 request 연결 리스트 — 아직 어떤 hctx로도 dispatch되지
		 * 않은 request가 잠시 쌓이는 곳이다.
		 * 설정자: blk_mq_insert_request()가 request의 hctx 유형에 맞는
		 *   rq_lists[type]에 list_add_tail()로 추가.
		 * 읽는 자: blk_mq_flush_busy_ctxs()/blk_mq_dequeue_from_ctx()가
		 *   dispatch 시점에 리스트를 비우며 dispatch list로 옮긴다.
		 * 값 범위: HCTX_MAX_TYPES(보통 3: DEFAULT/READ/POLL) 개의 독립된
		 *   리스트 헤드. 각각 비어있거나 여러 request를 가질 수 있다.
		 *   NVMe에서 READ 전용 큐가 분리되어 있으면 읽기 요청은
		 *   rq_lists[HCTX_TYPE_READ]에, 폴링 요청은 [HCTX_TYPE_POLL]에
		 *   쌓여 서로 다른 SQ로 분산 dispatch될 수 있다.
		 * 동기화: 위 lock으로 보호됨. */
	} ____cacheline_aligned_in_smp;
	/* [한국어] 이 익명 구조체(lock + rq_lists)를 별도 cacheline에 정렬한다.
	 * 이유: 제출자가 lock을 잡고 rq_lists를 갱신하는 동안, 같은 blk_mq_ctx의
	 * 다른 필드(cpu, hctxs[] 등 read-mostly 필드)를 다른 CPU가 읽을 때
	 * cacheline을 공유해 false sharing이 발생하는 것을 막는다(추정 — 커널
	 * 커밋 히스토리상 성능 최적화 목적으로 도입된 정렬). */

	unsigned int		cpu;
	/* [한국어] 이 blk_mq_ctx가 대응하는 CPU 번호.
	 * 설정자: blk_mq_init_cpu_queues()가 per-CPU 순회 중 자신의 cpu 번호로
	 *   1회 초기화(이후 불변).
	 * 읽는 자: blk_mq_hctx_mark_pending() 등이 index_hw[]를 인덱싱할 때,
	 *   디버그/트레이스 코드가 어느 CPU의 ctx인지 식별할 때 사용.
	 * 값 범위: 0 ~ nr_cpu_ids-1.
	 * 동기화: 초기화 이후 읽기 전용(불변)이라 락이 필요 없다. */
	unsigned short		index_hw[HCTX_MAX_TYPES];
	/* [한국어] hctx 유형별로, 이 ctx가 해당 hctx 안에서 차지하는 소프트웨어
	 * 큐 슬롯 인덱스. hctx->ctxs[] 배열에서 이 ctx가 몇 번째인지를 가리킨다.
	 * 설정자: blk_mq_map_swqueue()(blk-mq.c)가 CPU->hctx 매핑을 계산하며
	 *   ctx가 hctx에 추가되는 순서대로 채운다.
	 * 읽는 자: blk_mq_hctx_mark_pending()/blk_mq_hctx_clear_pending()이
	 *   hctx->ctx_map의 어느 비트를 세팅/해제할지 결정할 때 인덱스로 사용.
	 * 값 범위: 0 ~ hctx->nr_ctx-1 (유형별로 별도 값). 매핑되지 않은 유형은
	 *   사용되지 않는다.
	 * 동기화: 큐 토폴로지가 재계산(blk_mq_update_nr_hw_queues 등)되기
	 *   전까지 불변이며, 재계산은 freeze 상태에서 이루어지므로 별도 락
	 *   없이 안전하다. */
	struct blk_mq_hw_ctx 	*hctxs[HCTX_MAX_TYPES];
	/* [한국어] hctx 유형별로 이 CPU가 최종적으로 매핑되는 hctx 포인터 —
	 * blk_mq_map_queue()가 반환하는 실제 값이 여기서 나온다.
	 * 설정자: blk_mq_map_swqueue()가 tag_set->map[type].mq_map[cpu]를
	 *   근거로 채운다.
	 * 읽는 자: blk_mq_map_queue()가 opf로 결정된 유형의 hctxs[type]을
	 *   그대로 반환 — 이것이 곧 이 CPU에서 나가는 I/O가 도달할 하드웨어
	 *   큐(NVMe라면 SQ/CQ 쌍)다.
	 * 값 범위: 유효한 blk_mq_hw_ctx 포인터, 또는 해당 유형이 이 CPU에
	 *   매핑되지 않았다면 사용되지 않는 값(호출자는 blk_mq_get_hctx_type()
	 *   으로 실제 사용 가능한 유형만 조회).
	 * 동기화: 토폴로지 재계산 전까지 불변, index_hw[]와 동일한 보호. */

	struct request_queue	*queue;
	/* [한국어] 이 ctx가 속한 request_queue로의 역참조 포인터.
	 * 설정자: blk_mq_alloc_ctxs()/blk_mq_init_cpu_queues()가 초기화 시 대입.
	 * 읽는 자: sysfs 노드 구성, ctx가 속한 디스크/큐를 찾아야 하는 여러
	 *   디버그·통계 경로.
	 * 값 범위: 유효한 request_queue 포인터(NULL 불가) — 이 ctx가 살아있는
	 *   동안 항상 같은 큐를 가리킨다.
	 * 동기화: 불변 포인터라 락 불필요. */
	struct blk_mq_ctxs      *ctxs;
	/* [한국어] 이 ctx가 속한 상위 컨테이너(struct blk_mq_ctxs)로의 포인터 —
	 * sysfs kobject 생명주기를 공유하기 위해 사용된다.
	 * 설정자: blk_mq_alloc_ctxs()가 컨테이너를 만든 뒤 각 per-CPU ctx에
	 *   대입.
	 * 읽는 자: blk_mq_sysfs_deinit() 등이 kobject 참조 카운트를 낮출 때
	 *   상위 컨테이너를 거쳐 접근.
	 * 값 범위: 유효한 blk_mq_ctxs 포인터.
	 * 동기화: 불변 포인터. */
	struct kobject		kobj;
	/* [한국어] 이 ctx 하나에 대응하는 sysfs kobject — 필요 시
	 * /sys/block/<disk>/mq/<hctx>/cpu<N>/ 형태로 노출된다.
	 * 설정자: blk_mq_sysfs_register_hctxs() 경로에서 kobject_init().
	 * 읽는 자: sysfs show/store 콜백, kobject release 시 참조.
	 * 값 범위: 유효한 kobject.
	 * 동기화: kobject 자체의 kref로 관리. */
} ____cacheline_aligned_in_smp;
/* [한국어] struct blk_mq_ctx 전체를 cacheline 경계에 정렬한다.
 * 이유: per-CPU 배열의 인접한 두 CPU용 blk_mq_ctx가 같은 cacheline에 걸치면,
 * 서로 다른 CPU가 각자의 ctx(특히 위의 lock+rq_lists)를 동시에 갱신할 때
 * cacheline invalidation이 반복되는 false sharing 성능 저하가 생긴다. 구조체
 * 전체를 cacheline 크기에 맞춰 정렬해 이를 방지한다(추정 — 일반적인 per-CPU
 * 자료구조 설계 관례). */

/*
 * [한국어] block layer tag 값의 특수/경계 상수 모음.
 *
 * 여기서 말하는 "tag"는 block-mq가 request 하나를 식별하는 정수 번호로,
 * NVMe 드라이버가 SQ(Submission Queue) 엔트리를 채울 때 넣는 CID(Command
 * Identifier)와 사실상 같은 개념으로 대응된다(드라이버가 rq->tag를 그대로
 * cmd->common.command_id에 복사).
 */
enum {
	BLK_MQ_NO_TAG		= -1U,
	/* [한국어] "아직 tag가 할당되지 않음"을 뜻하는 특수 값. unsigned에서
	 * -1U는 all-bits-set(가장 큰 unsigned 값)이므로 실제 유효 tag 범위와
	 * 절대 겹치지 않는다.
	 * 설정자: request 생성 시 rq->tag/rq->internal_tag 초기값, tag 반환
	 *   시(__blk_mq_put_driver_tag()) 다시 이 값으로 리셋.
	 * 읽는 자: blk_mq_get_driver_tag()/blk_mq_put_driver_tag() 등이
	 *   "이 request가 지금 유효한 tag(=NVMe SQ 슬롯)를 갖고 있는가"를
	 *   판단하는 기준으로 비교.
	 * 값 범위: 상수 하나뿐이며 다른 매크로와 산술 관계로 얽혀 있다
	 *   (BLK_MQ_TAG_MAX 정의 참고). */

	BLK_MQ_TAG_MIN		= 1,
	/* [한국어] 유효한 tag의 최소값. 0이 아니라 1부터 시작하는 이유는,
	 * 여러 드라이버/스케줄러 구현에서 0을 "특수/예약" 의미로 쓰는 관례를
	 * 피하고, sbitmap 등에서 사용하는 예약 tag 슬롯과 충돌을 줄이기
	 * 위함으로 보인다(추정).
	 * 설정자: 이 열거형 정의 자체 — 코드 다른 곳에서 대입되지 않는다.
	 * 읽는 자: tag 할당 범위 검증/문서화 목적으로 참조(직접 사용처는
	 *   드물고, 주로 BLK_MQ_TAG_MAX 계산과 대비되는 하한을 명시하는 역할).
	 * 값 범위: 상수 1. */

	BLK_MQ_TAG_MAX		= BLK_MQ_NO_TAG - 1,
	/* [한국어] 유효한 tag의 최대값 = (unsigned)(-1) - 1, 즉 UINT_MAX - 1.
	 * BLK_MQ_NO_TAG(all-bits-set)를 예약값으로 빼놓기 위해 그 바로 아래
	 * 값을 상한으로 잡는다.
	 * 설정자: 이 열거형 정의 자체.
	 * 읽는 자: tag_set/sbitmap 초기화 시 요청 가능한 최대 depth의 이론적
	 *   상한을 검증하는 경계값으로 참조될 수 있다. 실제 NVMe SQ depth는
	 *   컨트롤러의 MQES(Maximum Queue Entries Supported)에 의해 이보다
	 *   훨씬 작은 값으로 제한된다.
	 * 값 범위: 상수 (UINT_MAX - 1). */
};

/*
 * [한국어] 매크로 정의: BLK_MQ_CPU_WORK_BATCH — 워크큐 콜백 한 번에 처리할
 * per-CPU 완료 request 배치 크기.
 *
 * 이 값이 필요한 이유: 완료된 request를 원래 CPU로 되돌려 처리(IPI/워크큐)할 때,
 * 한 번의 콜백 실행에서 무한정 많은 request를 처리하면 다른 워크가 굶주릴 수
 * 있다. 8개 단위로 배치를 끊어 처리량과 지연시간의 균형을 맞춘다(값 자체는
 * 커널 개발자들이 실험적으로 정한 경험적 상수).
 */
#define BLK_MQ_CPU_WORK_BATCH	(8)

/*
 * [한국어] typedef 정의: blk_insert_t — request를 hctx의 어느 위치/방식으로
 * 삽입할지 나타내는 비트마스크 타입.
 *
 * __bitwise 애트리뷰트: sparse 정적 분석 도구가 이 타입과 일반 unsigned int를
 * 암묵적으로 섞어 쓰면 경고를 내도록 강제한다 — blk_insert_t 값은 반드시
 * BLK_MQ_INSERT_AT_HEAD 같은 지정된 상수를 통해서만 만들어져야 함을 타입
 * 시스템 수준에서 문서화한 것이다.
 */
typedef unsigned int __bitwise blk_insert_t;
/*
 * [한국어] 매크로 정의: BLK_MQ_INSERT_AT_HEAD — request를 hctx 소프트웨어
 * 큐의 뒤(tail)가 아니라 앞(head)에 삽입하라는 플래그.
 *
 * 이 값이 필요한 이유: 일반적으로 request는 도착 순서대로 tail에 쌓여
 * FIFO 성격을 유지하지만, requeue(재제출)되는 request는 이미 한 번 처리
 * 시도가 있었으므로 다른 신규 request보다 먼저 재시도되어야 하는 경우가
 * 많다(예: 드라이버가 일시적 자원 부족으로 반려한 request, 또는 NVMe
 * 컨트롤러 reset 이후 재시도되는 abort된 명령). 이런 경우 head 삽입으로
 * 우선순위를 준다.
 * __force 캐스트: __bitwise 타입 검사를 의도적으로 우회해 상수 리터럴
 * 0x01을 blk_insert_t로 변환한다는 것을 sparse에 명시적으로 알린다.
 */
#define BLK_MQ_INSERT_AT_HEAD		((__force blk_insert_t)0x01)

/*
 * [한국어]
 * blk_mq_submit_bio - bio를 block-mq 경로로 제출하는 최상위 진입점
 *
 * @bio: 상위 파일시스템/VFS가 만든 Block I/O 단위. 하나 이상의 request로
 *       변환되거나 기존 request에 병합된다.
 * @return: 없음(void) — 완료는 나중에 bio->bi_end_io 콜백으로 비동기 통보.
 *
 * gendisk->fops->submit_bio 로 등록되어, 블록 디바이스에 대한 모든 I/O가
 * 이 함수를 통해 block-mq 로 들어온다. 내부에서 기존 request와의 병합을
 * 시도하고, 실패하면 새 request를 할당해 plug 리스트에 쌓거나 즉시
 * hctx로 dispatch한다. 정의와 상세 라인 주석은 blk-mq.c에 있다(본 파일은
 * 선언만 제공).
 * 실행 컨텍스트: 프로세스 컨텍스트(파일시스템 I/O 경로, 보통 preempt 가능).
 *
 * 호출 체인:
 *   submit_bio()(block/blk-core.c) -> [blk_mq_submit_bio] ->
 *     blk_mq_get_new_requests()/병합 시도 -> blk_add_rq_to_plug() 또는
 *     blk_mq_try_issue_directly() -> blk_mq_dispatch_rq_list() -> ops->queue_rq
 */
void blk_mq_submit_bio(struct bio *bio);

/*
 * [한국어]
 * blk_mq_poll - 폴링(polling) 모드로 완료된 I/O를 회수한다
 *
 * @q:     대상 request_queue.
 * @cookie: submit_bio()가 반환한 blk_qc_t — 어느 hctx/cookie에서 폴링할지
 *          식별하는 값(내부에 hctx 인덱스 등이 인코딩됨).
 * @iob:   완료 배치를 모아서 상위(io_uring 등)에 한 번에 통보하기 위한
 *         io_comp_batch 버퍼.
 * @flags: BLK_POLL_* 플래그(예: 한 번만 볼지, busy-loop 허용 여부).
 * @return: 이번 호출에서 회수(완료 처리)한 request 개수. 0이면 아직 완료된
 *          것이 없었다는 뜻.
 *
 * 인터럽트 기반 완료 대신, 호출한 태스크가 직접 하드웨어 완료 큐를
 * 반복적으로 확인(polling)하여 인터럽트 지연/coalescing을 우회하는
 * 저지연 경로다. NVMe 폴링 큐를 쓰는 경우 CQ(Completion Queue)를 직접
 * 읽어 들이는 nvme_poll_cq() 계열까지 내려간다.
 * 실행 컨텍스트: 프로세스 컨텍스트(io_uring IORING_ENTER_GETEVENTS,
 * preadv2(RWF_HIPRI) 등에서 호출), 필요 시 busy-loop.
 *
 * 호출 체인:
 *   io_uring/blk_poll() 등 -> [blk_mq_poll] -> blk_hctx_poll() ->
 *     ops->poll(드라이버 콜백, 예: nvme_poll)
 */
int blk_mq_poll(struct request_queue *q, blk_qc_t cookie, struct io_comp_batch *iob,
		unsigned int flags);
/*
 * [한국어]
 * blk_mq_exit_queue - request_queue 해제 시 block-mq 전용 자원을 정리한다
 *
 * @q: 해제 중인 request_queue.
 * @return: 없음(void).
 *
 * blk_cleanup_queue() 등 큐 소멸 경로 후반부에서 호출되어, hctx 배열/태그
 * 세트 참조/스케줄러 등 block-mq가 소유한 자원을 마지막으로 정리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐가 이미 freeze/quiesce된 이후.
 *
 * 호출 체인:
 *   blk_put_queue()/disk_release() 계열 -> [blk_mq_exit_queue] ->
 *     blk_mq_exit_sched()/blk_mq_sysfs_deinit() 등
 */
void blk_mq_exit_queue(struct request_queue *q);
/*
 * [한국어]
 * blk_mq_update_nr_requests - 사용자가 sysfs로 nr_requests(큐 깊이)를 바꿀 때
 * 스케줄러 tag 풀을 재할당한다
 *
 * @q:    대상 request_queue.
 * @tags: 현재 사용 중인 struct elevator_tags(스케줄러 tag 풀). 재사용
 *        가능하면 그대로, 아니면 새로 할당해 반환.
 * @nr:   사용자가 요청한 새로운 큐 깊이(nr_requests).
 * @return: 갱신된(또는 새로 할당된) struct elevator_tags 포인터. 실패 시
 *          ERR_PTR 인코딩된 에러를 반환할 수 있다.
 *
 * /sys/block/<disk>/queue/nr_requests 에 값을 쓰면 이 함수가 호출되어
 * mq-deadline/kyber/bfq 같은 스케줄러가 쓰는 tag 풀 크기를 조정한다.
 * NVMe SQ 자체의 하드웨어 깊이(MQES)와는 별개로, block layer가 관리하는
 * 소프트웨어 큐 깊이를 바꾸는 것이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write, queue freeze 보호 하에 수행).
 *
 * 호출 체인:
 *   queue_requests_store()(blk-sysfs.c) -> [blk_mq_update_nr_requests] ->
 *     blk_mq_tag_update_sched_shared_tags()/blk_mq_alloc_map_and_rqs() 등
 */
struct elevator_tags *blk_mq_update_nr_requests(struct request_queue *q,
						struct elevator_tags *tags,
						unsigned int nr);
/*
 * [한국어]
 * blk_mq_wake_waiters - 모든 hctx에서 tag 부족으로 대기 중인 제출자를 깨운다
 *
 * @q: 대상 request_queue.
 * @return: 없음(void).
 *
 * tag(NVMe 관점의 CID/SQ 슬롯) 풀에 여유가 생겼거나, 큐 상태가 바뀌어
 * (예: unquiesce, unfreeze) 더 이상 대기할 필요가 없어졌을 때 호출되어,
 * sbitmap_queue의 wait queue에서 잠들어 있던 제출 스레드들을 깨운다.
 * 실행 컨텍스트: 프로세스 컨텍스트(큐 unfreeze/unquiesce, 태그셋 리사이즈 등).
 *
 * 호출 체인:
 *   blk_mq_unfreeze_queue()/blk_mq_unquiesce_queue() 등 -> [blk_mq_wake_waiters]
 *     -> hctx 순회 -> blk_mq_tag_wakeup_all()
 */
void blk_mq_wake_waiters(struct request_queue *q);
/*
 * [한국어]
 * blk_mq_dispatch_rq_list - request 리스트를 이 hctx를 통해 드라이버로 전달한다
 *
 * @hctx: dispatch를 수행할 하드웨어 큐(NVMe라면 SQ/CQ 쌍 하나에 대응).
 * @list: dispatch할 request들의 연결 리스트(입력 후 남은 것은 다시
 *        hctx->dispatch에 되돌려질 수 있음 — in/out 성격).
 * @bool 인자(이름 없음, 세 번째 매개변수): got_budget — 호출자가 이미
 *       dispatch budget을 확보해 놓았는지 여부.
 * @return: true면 리스트를 모두(또는 드라이버가 받아들일 수 있는 만큼)
 *          처리했다는 뜻, false면 중간에 STOP되어 재시도가 필요함을 의미.
 *
 * 이 함수가 block-mq의 실질적인 "제출 관문"이다 — budget 확보, tag 확보,
 * ops->queue_rq() 호출까지 이 안에서 이루어진다. NVMe라면 이 안에서
 * nvme_queue_rq()가 호출되어 SQ 엔트리를 채우고 doorbell을 울린다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 softirq(run_hw_queue 경로에 따라 다름).
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue()/blk_mq_try_issue_directly() 등 ->
 *     [blk_mq_dispatch_rq_list] -> blk_mq_get_dispatch_budget() ->
 *     blk_mq_get_driver_tag() -> ops->queue_rq()
 */
bool blk_mq_dispatch_rq_list(struct blk_mq_hw_ctx *hctx, struct list_head *,
			     bool);
/*
 * [한국어]
 * blk_mq_flush_busy_ctxs - 이 hctx에 매핑된 ctx들의 pending request를 리스트로 모은다
 *
 * @hctx: 대상 하드웨어 큐 — hctx->ctx_map으로 어느 ctx에 pending이 있는지 안다.
 * @list: 결과를 담을 출력 리스트(dispatch 직전 임시 리스트).
 * @return: 없음(void) — 결과는 @list에 누적됨.
 *
 * hctx_may_queue()/hctx->ctx_map을 훑어 pending 표시가 된 CPU들의
 * blk_mq_ctx->rq_lists[hctx->type]을 전부 @list로 옮겨 담는다. 이후
 * blk_mq_dispatch_rq_list()가 이 리스트를 드라이버로 넘긴다.
 * 실행 컨텍스트: run_hw_queue 경로, 프로세스 컨텍스트/softirq 모두 가능.
 *
 * 호출 체인:
 *   blk_mq_sched_dispatch_requests()/blk_mq_run_hw_queue() ->
 *     [blk_mq_flush_busy_ctxs] -> blk_mq_hctx_clear_pending() (ctx별)
 */
void blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx, struct list_head *list);
/*
 * [한국어]
 * blk_mq_dequeue_from_ctx - 특정 ctx 하나에서 request 한 개를 꺼낸다
 *
 * @hctx:  대상 하드웨어 큐.
 * @start: 탐색을 시작할 blk_mq_ctx — 라운드로빈 공정성을 위해 마지막으로
 *         시작한 지점부터 훑는다(NULL이면 hctx->dispatch_from 사용).
 * @return: 꺼낸 request 포인터, 없으면 NULL.
 *
 * 스케줄러가 없는(none) 큐에서 dispatch_from 필드를 이용해 여러 ctx를
 * 라운드로빈으로 순회하며 request를 하나씩 꺼내는 헬퍼. 특정 CPU의 ctx만
 * 계속 우선시되어 다른 CPU가 굶주리는 것을 방지한다.
 * 실행 컨텍스트: dispatch 경로(프로세스/softirq).
 *
 * 호출 체인:
 *   blk_mq_do_dispatch_ctx()(blk-mq-sched.c) -> [blk_mq_dequeue_from_ctx]
 */
struct request *blk_mq_dequeue_from_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *start);
/*
 * [한국어]
 * blk_mq_put_rq_ref - request의 참조 카운트를 감소시키고 0이면 해제한다
 *
 * @rq: 참조를 반환할 request.
 * @return: 없음(void).
 *
 * timeout 처리 등에서 request를 iterate하며 임시로 참조를 잡아 두었다가
 * (refcount_inc) 다 쓰고 나서 이 함수로 반환한다. 참조가 0이 되는 순간
 * 실제 완료 처리(end_request)가 트리거될 수 있다.
 * 실행 컨텍스트: 주로 timeout 워크(kblockd) 컨텍스트, 드물게 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_check_expired()/blk_mq_timeout_work() -> [blk_mq_put_rq_ref]
 *     -> __blk_mq_end_request() (refcount가 0이 되는 경우)
 */
void blk_mq_put_rq_ref(struct request *rq);

/*
 * Internal helpers for allocating/freeing the request map
 *
 * [한국어] request map(=tag bitmap + request 배열 + DMA 페이지)을 할당/해제하는
 * 내부 헬퍼 4종. tag_set 은 드라이버 하나(예: NVMe 컨트롤러 하나)가 소유하는
 * 전역 설정이고, 이 헬퍼들은 hctx(=SQ) 하나 단위로 그 안에 필요한 tag
 * bitmap과 struct request 배열을 만들고 없앤다.
 */
/*
 * [한국어]
 * blk_mq_free_rqs - 특정 hctx의 request 배열(및 정적 request)을 해제한다
 *
 * @set:      드라이버 tag_set(공유 tag 여부, ops 등 전역 설정 보유).
 * @tags:     해제 대상 blk_mq_tags(내부에 rqs[]/static_rqs[] 보유).
 * @hctx_idx: 몇 번째 하드웨어 큐(SQ)에 대한 tag/rq 인지 식별.
 * @return: 없음(void).
 *
 * tags->static_rqs[] 를 순회하며 set->ops->exit_request(있다면)를 호출해
 * 드라이버가 할당한 per-request 사설 데이터(예: NVMe PRP 리스트 DMA 메모리)를
 * 먼저 정리한 뒤, request 배열 자체의 메모리(페이지 단위)를 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(큐 삭제, tag_set 리사이즈 경로).
 *
 * 호출 체인:
 *   blk_mq_free_map_and_rqs()/tag_set 해제 경로 -> [blk_mq_free_rqs] ->
 *     set->ops->exit_request (드라이버 콜백)
 */
void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,
		     unsigned int hctx_idx);
/*
 * [한국어]
 * blk_mq_free_rq_map - blk_mq_tags 구조체 자체(비트맵 포함)를 해제한다
 *
 * @set:  드라이버 tag_set.
 * @tags: 해제할 blk_mq_tags(bitmap_tags/breserved_tags sbitmap 포함).
 * @return: 없음(void).
 *
 * blk_mq_free_rqs()가 request 배열을 비운 뒤, 이 함수가 tags 구조체와 그
 * 안의 sbitmap_queue 들을 최종적으로 kfree 한다. NVMe 관점에서는 SQ 하나가
 * 사라지기 직전에 그 SQ의 CID 비트맵을 완전히 제거하는 단계다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_free_map_and_rqs() -> [blk_mq_free_rq_map]
 */
void blk_mq_free_rq_map(struct blk_mq_tag_set *set, struct blk_mq_tags *tags);
/*
 * [한국어]
 * blk_mq_alloc_map_and_rqs - hctx 하나에 필요한 tag map + request 배열을 할당한다
 *
 * @set:      드라이버 tag_set(노드 친화성, cmd_size 등 참조).
 * @hctx_idx: 대상 하드웨어 큐 인덱스.
 * @depth:    이 hctx가 가질 tag 개수(=대응하는 NVMe SQ의 큐 깊이).
 * @return: 새로 할당된 struct blk_mq_tags 포인터, 실패 시 NULL.
 *
 * blk_mq_init_tags()로 depth 크기의 sbitmap_queue(tag 비트맵)를 만들고,
 * 이어서 depth 개의 struct request를 (필요하면 set->ops->init_request로
 * 드라이버 사설 데이터까지) 초기화해 tags->static_rqs[]/rqs[]에 채운다.
 * NVMe 드라이버라면 여기서 할당된 request 각각의 사설 영역에 PRP 리스트용
 * DMA 버퍼가 마련된다(드라이버 init_request 구현에 따라 다름, 추정).
 * 실행 컨텍스트: 프로세스 컨텍스트(큐 생성, nr_requests 변경, hw queue 개수 변경).
 *
 * 호출 체인:
 *   blk_mq_alloc_tag_set()/blk_mq_update_nr_hw_queues() 등 ->
 *     [blk_mq_alloc_map_and_rqs] -> blk_mq_init_tags() -> blk_mq_alloc_rqs()
 */
struct blk_mq_tags *blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
				unsigned int hctx_idx, unsigned int depth);
/*
 * [한국어]
 * blk_mq_free_map_and_rqs - 위 alloc의 역작업 — request+tag map을 통째로 해제
 *
 * @set:      드라이버 tag_set.
 * @tags:     해제할 blk_mq_tags.
 * @hctx_idx: 대상 하드웨어 큐 인덱스(공유 tag 여부 판단에 사용).
 * @return: 없음(void).
 *
 * 내부적으로 blk_mq_free_rqs() -> blk_mq_free_rq_map() 순서로 호출해 request
 * 배열과 tag 비트맵을 모두 정리하는 편의 래퍼. 공유 tag(BLK_MQ_F_TAG_HCTX_SHARED)
 * 인 경우 실제 해제는 마지막 참조가 없어졌을 때만 이뤄지도록 내부에서 조율한다
 * (구현은 blk-mq.c, 추정).
 * 실행 컨텍스트: 프로세스 컨텍스트(큐/태그셋 teardown 경로).
 *
 * 호출 체인:
 *   blk_mq_exit_hctx()/blk_mq_free_tag_set() -> [blk_mq_free_map_and_rqs] ->
 *     blk_mq_free_rqs() -> blk_mq_free_rq_map()
 */
void blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
			     struct blk_mq_tags *tags,
			     unsigned int hctx_idx);

/*
 * CPU -> queue mappings
 *
 * [한국어] CPU 번호로부터 그 CPU가 속한 NUMA 노드를 구해, hctx를 그 노드에
 * 친화적으로 배치하기 위한 헬퍼.
 */
/*
 * [한국어]
 * blk_mq_hw_queue_to_node - qmap 안에서 특정 CPU가 매핑된 hctx의 NUMA 노드를 조회
 *
 * @qmap: CPU->hctx_idx 매핑 테이블(struct blk_mq_queue_map).
 * @cpu (두 번째 매개변수, 이름 없음): 조회할 CPU 번호.
 * @return: 그 CPU가 매핑된 하드웨어 큐가 속한 NUMA 노드 번호. 매핑이 없으면
 *          NUMA_NO_NODE 등 기본값을 반환할 수 있다(구현은 blk-mq.c).
 *
 * hctx를 위한 메모리(request 배열, DMA 디스크립터 등)를 kzalloc_node()로
 * 할당할 때 어느 노드에서 할당해야 그 hctx를 다루는 CPU들과 가까운지
 * 결정하는 데 쓰인다. NVMe에서는 SQ/CQ 메모리를 담당 CPU와 같은 NUMA 노드에
 * 두면 캐시/메모리 대역폭 이득이 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트(큐 초기화 시점).
 *
 * 호출 체인:
 *   blk_mq_alloc_and_init_hctx()(blk-mq.c) -> [blk_mq_hw_queue_to_node]
 */
extern int blk_mq_hw_queue_to_node(struct blk_mq_queue_map *qmap, unsigned int);

/*
 * [한국어]
 * blk_mq_map_queue_type - map (hctx_type,cpu) to hardware queue
 *
 * @q:    request_queue — q->tag_set->map[]에서 매핑 테이블을 찾는다.
 * @type: hctx 유형(HCTX_TYPE_DEFAULT/READ/POLL).
 * @cpu:  조회할 CPU 번호.
 * @return: (type, cpu) 조합이 가리키는 실제 struct blk_mq_hw_ctx 포인터.
 *
 * q->tag_set->map[type] 은 struct blk_mq_queue_map 으로, mq_map[cpu]에
 * "이 CPU가 이 유형에서는 몇 번째 하드웨어 큐를 쓰는가"가 인덱스로 들어
 * 있다. queue_hctx() 매크로(include/linux/blk-mq.h)가 q->queue_hw_ctx[]
 * 배열에서 그 인덱스의 hctx를 꺼내 온다. NVMe 관점에서는 CPU와
 * hctx_type(READ/POLL/DEFAULT) 조합에 따라 어느 nvme_queue(SQ/CQ 쌍)로
 * 보낼지 결정하는 것과 같다 — NVMe 드라이버는 nr_hw_queues 개의
 * nvme_queue를 만들고, 이 매핑 테이블을 통해 CPU 코어들이 그 큐들에
 * 분배된다.
 * 실행 컨텍스트: 어디서든 호출 가능한 순수 조회 함수(부수효과 없음).
 *
 * 호출 체인:
 *   blk_mq_map_queue()/디버그·sysfs 코드 -> [blk_mq_map_queue_type] ->
 *     queue_hctx()
 */
static inline struct blk_mq_hw_ctx *blk_mq_map_queue_type(struct request_queue *q,
						  enum hctx_type type,
						  unsigned int cpu)
{
	return queue_hctx((q), (q->tag_set->map[type].mq_map[cpu]));
}

/*
 * [한국어]
 * blk_mq_get_hctx_type - bio/request의 연산 플래그로부터 사용할 hctx 유형을 결정
 *
 * @opf: REQ_OP_* 연산 코드와 REQ_* 플래그가 합쳐진 blk_opf_t 값(예:
 *       REQ_OP_READ | REQ_POLLED).
 * @return: HCTX_TYPE_POLL / HCTX_TYPE_READ / HCTX_TYPE_DEFAULT 중 하나.
 *
 * 우선순위는 POLL > READ > DEFAULT 순이다: 폴링 요청(REQ_POLLED)이면
 * 무조건 폴링 전용 큐를 쓰고(NVMe라면 인터럽트 없이 nvme_poll_cq()로
 * 직접 회수), 그렇지 않고 순수 읽기(REQ_OP_READ)이면 읽기 전용 큐가
 * 있는 경우 그쪽으로, 나머지(쓰기 등)는 기본(DEFAULT) 큐로 보낸다.
 * 드라이버가 READ/POLL 전용 큐를 만들지 않았다면 tag_set->map[]에서
 * 해당 유형이 DEFAULT와 같은 큐를 가리키도록 설정되어 있어 결과적으로
 * 문제가 없다.
 * 실행 컨텍스트: 제출 경로 어디서든 호출 가능(순수 계산).
 *
 * 호출 체인:
 *   blk_mq_map_queue() -> [blk_mq_get_hctx_type]
 */
static inline enum hctx_type blk_mq_get_hctx_type(blk_opf_t opf)
{
	/* [한국어] 기본값은 DEFAULT 큐 — POLL/READ 조건에 해당하지 않는
	 * 모든 연산(WRITE, DISCARD, FLUSH 등)은 이 유형으로 간다. */
	enum hctx_type type = HCTX_TYPE_DEFAULT;

	/*
	 * The caller ensure that if REQ_POLLED, poll must be enabled.
	 */
	/* [한국어] REQ_POLLED 비트가 서 있으면 폴링 전용 hctx를 선택한다.
	 * 위 원본 주석대로, 호출자가 이미 "이 큐가 폴링을 지원한다"는 것을
	 * blk_mq_can_poll()로 확인했다는 전제 하에 호출된다. */
	if (opf & REQ_POLLED)
		type = HCTX_TYPE_POLL;
	/* [한국어] REQ_OP_MASK로 상위 플래그 비트를 걷어내고 순수 연산 코드만
	 * 비교 — REQ_OP_READ(읽기)이면 읽기 전용 큐(있다면)로 분리한다.
	 * NVMe에서 읽기/쓰기를 별도 SQ로 나누면 읽기 지연시간을 쓰기 폭주로
	 * 부터 격리하는 효과를 볼 수 있다(구성에 따라 다름). */
	else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
		type = HCTX_TYPE_READ;
	/* [한국어] 최종 결정된 유형을 반환 — 이 값이 그대로 ctx->hctxs[]와
	 * ctx->index_hw[]의 인덱스로 쓰인다. */
	return type;
}

/*
 * [한국어]
 * blk_mq_map_queue - map (cmd_flags,type) to hardware queue
 *
 * @opf: 연산 타입(REQ_OP_*)과 플래그(예: REQ_POLLED)가 합쳐진 값.
 * @ctx: 현재 CPU의 소프트웨어 큐(blk_mq_get_ctx()로 얻은 값).
 * @return: 이 CPU에서 이 연산을 낼 때 실제로 사용할 struct blk_mq_hw_ctx.
 *
 * blk_mq_get_hctx_type(opf)로 유형을 정하고, ctx->hctxs[type]에서 미리
 * 계산되어 있던 hctx 포인터를 그대로 꺼낸다(런타임에 매핑 테이블을 다시
 * 훑지 않고 O(1)로 조회). 이것이 제출 경로에서 "이 CPU, 이 연산이 어느
 * NVMe SQ/CQ 쌍으로 갈지"를 최종 결정하는 지점이다.
 * 실행 컨텍스트: bio 제출 경로(프로세스 컨텍스트), lock-free 조회.
 *
 * 호출 체인:
 *   submit_bio -> blk_mq_get_ctx(CPU별 software queue) -> [blk_mq_map_queue]
 *     -> blk_mq_hw_ctx 확정 -> (이후 dispatch에서) ops->queue_rq
 */
static inline struct blk_mq_hw_ctx *blk_mq_map_queue(blk_opf_t opf,
						     struct blk_mq_ctx *ctx)
{
	return ctx->hctxs[blk_mq_get_hctx_type(opf)];
}

/*
 * Default to double of smaller one between hw queue_depth and
 * 128, since we don't split into sync/async like the old code
 * did. Additionally, this is a per-hw queue depth.
 */
/*
 * [한국어]
 * blk_mq_default_nr_requests - 드라이버가 명시하지 않은 기본 큐 깊이(nr_requests) 계산
 *
 * @set: 드라이버 tag_set — set->queue_depth(드라이버가 보고한 하드웨어 큐 깊이,
 *       NVMe라면 컨트롤러 MQES 기반 값)를 참조.
 * @return: 스케줄러/소프트웨어 큐가 기본으로 쓸 request 개수.
 *
 * 위 원본 주석(영문)이 설명하듯, 예전에는 sync/async를 나눠 depth를 반씩
 * 쓰던 관례가 있었지만 지금은 그렇게 하지 않는 대신, 하드웨어가 보고한
 * queue_depth와 BLKDEV_DEFAULT_RQ(고정 상한, 보통 128) 중 작은 값의 두 배를
 * 기본으로 쓴다. 두 배로 주는 이유는 소프트웨어 큐에 약간의 버퍼를 두어
 * 순간적인 요청 폭주(burst)를 흡수하기 위함이다 — 실제 하드웨어로 나가는
 * 개수는 여전히 set->queue_depth(및 tag 공정 분배 정책)로 제한된다.
 * 실행 컨텍스트: 큐 초기화 시점(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue()(blk-mq.c) -> [blk_mq_default_nr_requests]
 */
static inline unsigned int blk_mq_default_nr_requests(
		struct blk_mq_tag_set *set)
{
	return 2 * min_t(unsigned int, set->queue_depth, BLKDEV_DEFAULT_RQ);
}

/*
 * sysfs helpers
 *
 * [한국어] /sys/block/<disk>/mq/ 아래의 sysfs 트리를 만들고 없애는 헬퍼 모음.
 * 실제 attribute 정의와 show/store 콜백은 blk-mq-sysfs.c에 있으며, 본 파일은
 * blk-mq.c/blk-mq-sysfs.c가 서로 호출하기 위한 선언만 제공한다.
 */
/*
 * [한국어] blk_mq_sysfs_init - request_queue의 mq_kobj를 초기화(등록은 아직 안 함).
 * @q: 대상 request_queue. @return: 없음(void).
 * queue 생성 초기 단계에서 kobject 골격만 세팅해, 이후 disk가 add_disk()될
 * 때 실제 sysfs 디렉터리가 노출되도록 준비한다.
 * 호출 체인: blk_mq_init_allocated_queue() -> [blk_mq_sysfs_init]
 */
extern void blk_mq_sysfs_init(struct request_queue *q);
/*
 * [한국어] blk_mq_sysfs_deinit - 위 init의 역작업, kobject 참조를 정리.
 * @q: 대상 request_queue. @return: 없음(void).
 * 호출 체인: blk_mq_exit_queue()/큐 해제 경로 -> [blk_mq_sysfs_deinit]
 */
extern void blk_mq_sysfs_deinit(struct request_queue *q);
/*
 * [한국어] blk_mq_sysfs_register - gendisk가 add_disk() 될 때 mq sysfs 트리를 실제로 노출.
 * @disk: 대상 gendisk(디스크). @return: 0 성공, 음수 errno 실패.
 * 이 시점 이후 /sys/block/<disk>/mq/... 경로가 사용자 공간에 보인다.
 * 호출 체인: add_disk() -> [blk_mq_sysfs_register] -> blk_mq_sysfs_register_hctxs()
 */
int blk_mq_sysfs_register(struct gendisk *disk);
/*
 * [한국어] blk_mq_sysfs_unregister - 위 register의 역작업, del_gendisk() 경로에서 호출.
 * @disk: 대상 gendisk. @return: 없음(void).
 * 호출 체인: del_gendisk() -> [blk_mq_sysfs_unregister]
 */
void blk_mq_sysfs_unregister(struct gendisk *disk);
/*
 * [한국어] blk_mq_sysfs_register_hctxs - 각 hctx(SQ)별 sysfs 서브디렉터리를 등록.
 * @q: 대상 request_queue. @return: 0 성공, 음수 errno 실패.
 * hw 큐 개수가 변경(nr_hw_queues 변경)될 때도 재호출되어 새 hctx의 sysfs
 * 노드를 추가한다.
 * 호출 체인: blk_mq_sysfs_register()/blk_mq_update_nr_hw_queues() ->
 *   [blk_mq_sysfs_register_hctxs] -> blk_mq_hctx_kobj_init()
 */
int blk_mq_sysfs_register_hctxs(struct request_queue *q);
/*
 * [한국어] blk_mq_sysfs_unregister_hctxs - 위 register_hctxs의 역작업.
 * @q: 대상 request_queue. @return: 없음(void).
 * 호출 체인: blk_mq_sysfs_unregister()/hw 큐 개수 축소 경로 ->
 *   [blk_mq_sysfs_unregister_hctxs]
 */
void blk_mq_sysfs_unregister_hctxs(struct request_queue *q);
/*
 * [한국어] blk_mq_hctx_kobj_init - hctx 하나의 kobject를 초기화(ktype 연결).
 * @hctx: 대상 hardware context. @return: 없음(void).
 * 호출 체인: blk_mq_sysfs_register_hctxs() -> [blk_mq_hctx_kobj_init]
 */
extern void blk_mq_hctx_kobj_init(struct blk_mq_hw_ctx *hctx);
/*
 * [한국어] blk_mq_free_plug_rqs - blk_plug에 배치(batch)로 쌓여 있던, 아직 드라이버로
 * 넘어가지 않은 request들을 모두 해제.
 * @plug: 태스크의 plug 구조체(struct blk_plug). @return: 없음(void).
 * 태스크가 비정상 종료하거나 plug를 강제로 비워야 할 때, doorbell을 아직
 * 울리지 않은 request들을 그냥 버리는 경로다.
 * 호출 체인: blk_finish_plug()/에러 경로 -> [blk_mq_free_plug_rqs]
 */
void blk_mq_free_plug_rqs(struct blk_plug *plug);
/*
 * [한국어] blk_mq_flush_plug_list - plug에 쌓인 request들을 실제로 dispatch(다수를 한
 * 번에 다운스트림 hctx로 전달)한다.
 * @plug: 태스크의 plug 구조체. @from_schedule: 스케줄러 컨텍스트 전환(예:
 *        태스크가 sleep 하기 직전) 때문에 호출되었는지 여부 — 워크큐로
 *        미루어 비동기 처리할지 결정하는 힌트로 쓰인다.
 * @return: 없음(void).
 * blk_add_rq_to_plug()로 모아 둔 request들을 한꺼번에 내보내 doorbell
 * 쓰기 횟수를 줄이는 배치 최적화의 마지막 단계다.
 * 호출 체인: blk_finish_plug()/io_schedule 경로 -> [blk_mq_flush_plug_list]
 *   -> blk_mq_dispatch_rq_list() 등
 */
void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule);

/*
 * [한국어] blk_mq_cancel_work_sync - 큐에 예약된 run_work(지연 dispatch 워크)를
 * 취소하고 완료를 동기적으로 기다린다.
 * @q: 대상 request_queue. @return: 없음(void).
 * 큐를 freeze/삭제하기 전에, 아직 실행되지 않은 hctx->run_work 워크아이템이
 * 나중에 이미 해제된 자료구조를 건드리지 않도록 확실히 취소해 둔다.
 * 호출 체인: blk_mq_exit_queue()/blk_cleanup_queue() -> [blk_mq_cancel_work_sync]
 */
void blk_mq_cancel_work_sync(struct request_queue *q);

/*
 * [한국어] blk_mq_release - request_queue 소멸 시 block-mq hctx 배열 등 마지막 메모리를 해제.
 * @q: 해제 중인 request_queue. @return: 없음(void).
 * request_queue의 참조 카운트(refcount)가 0이 되어 실제로 kfree 되기 직전에
 * 호출되는 release 콜백 경로의 일부.
 * 호출 체인: blk_release_queue() -> [blk_mq_release]
 */
void blk_mq_release(struct request_queue *q);

/*
 * [한국어]
 * __blk_mq_get_ctx - 주어진 CPU 번호에 해당하는 blk_mq_ctx를 반환
 *
 * @q:   request_queue — q->queue_ctx per-CPU 배열을 갖고 있다.
 * @cpu: 조회할 CPU 번호(반드시 possible CPU 범위 내).
 * @return: 그 CPU에 대응하는 struct blk_mq_ctx 포인터(항상 유효, NULL 아님).
 *
 * per_cpu_ptr()은 per-CPU 오프셋 계산만 수행하는 극히 저비용 연산이라, 이
 * 함수는 사실상 포인터 산술 한 번으로 끝난다. 락이 전혀 없는 이유는
 * blk_mq_ctx 배열 자체가 큐 생성 시 한 번 할당된 뒤 불변이기 때문이다 —
 * "찾는" 행위에는 동기화가 필요 없고, 그 안의 rq_lists를 조작할 때만
 * ctx->lock이 필요하다.
 * 실행 컨텍스트: 어떤 컨텍스트에서도 안전(인터럽트/softirq/프로세스 모두 가능).
 *
 * 호출 체인:
 *   blk_mq_get_ctx() -> [__blk_mq_get_ctx] -> per_cpu_ptr()
 *   (다른 CPU의 ctx를 직접 지정해 조회하고 싶은 드문 경로에서도 사용)
 */
static inline struct blk_mq_ctx *__blk_mq_get_ctx(struct request_queue *q,
						  unsigned int cpu)
{
	return per_cpu_ptr(q->queue_ctx, cpu);
}

/*
 * This assumes per-cpu software queueing queues. They could be per-node
 * as well, for instance. For now this is hardcoded as-is. Note that we don't
 * care about preemption, since we know the ctx's are persistent. This does
 * mean that we can't rely on ctx always matching the currently running CPU.
 */
/*
 * [한국어]
 * blk_mq_get_ctx - 현재 실행 중인 CPU의 blk_mq_ctx를 반환
 *
 * @q: request_queue.
 * @return: raw_smp_processor_id()가 가리키는 현재 CPU의 struct blk_mq_ctx.
 *
 * 제출 경로 진입 시 "지금 이 CPU를 위한 소프트웨어 큐가 어디인가"를 찾는
 * 첫 걸음이다. 위 원본 주석대로, 이 값은 per-CPU 큐 구조를 전제하며,
 * raw_smp_processor_id() 호출 이후 태스크가 다른 CPU로 선점(preempt)되어
 * 옮겨가더라도 이미 얻은 ctx 포인터 자체는 여전히 유효하다(ctx는 영속
 * 객체이므로) — 다만 그 시점부터는 "실제 실행 중인 CPU"와 "ctx가
 * 대표하는 CPU"가 미세하게 어긋날 수 있다. 이는 correctness 문제가
 * 아니라 CPU affinity/분배의 미세한 skew 정도로 그친다.
 * 실행 컨텍스트: 프로세스 컨텍스트(제출 경로), preempt 가능 상태에서 호출.
 *
 * 호출 체인:
 *   blk_mq_submit_bio()/blk_mq_alloc_request() 등 -> [blk_mq_get_ctx]
 *     -> __blk_mq_get_ctx() -> blk_mq_map_queue()
 */
static inline struct blk_mq_ctx *blk_mq_get_ctx(struct request_queue *q)
{
	return __blk_mq_get_ctx(q, raw_smp_processor_id());
}

/*
 * [한국어] struct blk_mq_alloc_data — request/tag 할당 호출 스택을 오가는
 * in/out 매개변수 묶음.
 *
 * blk_mq_get_tag()/blk_mq_alloc_request() 등 tag 할당 계열 함수들은 인자를
 * 하나하나 넘기는 대신, 이 구조체 하나(대개 스택에 값으로 생성 후 포인터
 * 전달)로 필요한 입력을 모아 넘기고, 동시에 "어떤 ctx/hctx가 선택됐는지"
 * 라는 결과도 같은 구조체에 되돌려 받는다(in/out 매개변수 패턴). NVMe
 * 관점에서 이 구조체는 "어느 SQ에, 어떤 종류(opcode 계열)의 명령을, 몇
 * 개까지 낼 것인가"를 기술하는 요청서에 해당한다.
 */
struct blk_mq_alloc_data {
	/* input parameter */
	struct request_queue *q;
	/* [한국어] 할당 대상 request_queue — 이 큐의 tag_set/hctx 배열에서
	 * 실제 tag를 뽑는다.
	 * 설정자: 호출자(blk_mq_alloc_request() 등)가 함수 진입 시 대입.
	 * 읽는 자: blk_mq_get_tag()/blk_mq_tags_from_data() 등 이 구조체를
	 *   받는 모든 헬퍼.
	 * 값 범위: 유효한 request_queue 포인터(NULL 불가).
	 * 동기화: 호출자의 스택 프레임에 존재하는 값이라 별도 동기화 불필요
	 *   (단일 호출 스레드만 접근). */
	blk_mq_req_flags_t flags;
	/* [한국어] BLK_MQ_REQ_NOWAIT(비블로킹 요청)/BLK_MQ_REQ_RESERVED(예약
	 * tag 사용) 등 할당 동작 자체를 제어하는 플래그.
	 * 설정자: 호출자가 요청 성격(즉시 실패 허용 여부 등)에 따라 설정.
	 * 읽는 자: blk_mq_get_tag()가 tag 부족 시 대기할지, 즉시 실패
	 *   (BLK_MQ_NO_TAG 반환)할지 분기하는 데 사용.
	 * 값 범위: BLK_MQ_REQ_* 비트 OR 조합, 0이면 기본(블로킹 가능) 동작.
	 * 동기화: 스택 값, 락 불필요. */
	unsigned int shallow_depth;
	/* [한국어] sbitmap_get_shallow()에 전달되어, 이 한 번의 할당이 tag
	 * 풀 전체 depth 중 앞쪽 일부 범위로만 제한되도록 강제하는 값(0이면
	 * 제한 없음). 스케줄러(kyber 등)가 우선순위별로 tag 사용량 상한을
	 * 두고 싶을 때 이용한다.
	 * 설정자: 스케줄러의 dispatch 준비 코드가 정책에 따라 설정.
	 * 읽는 자: blk_mq_get_tag() -> __sbitmap_queue_get_shallow().
	 * 값 범위: 0(제한 없음) ~ tag 풀 depth.
	 * 동기화: 스택 값, 락 불필요. */
	blk_opf_t cmd_flags;
	/* [한국어] 이 request가 수행할 연산(REQ_OP_READ/WRITE 등)과 부가
	 * 플래그(REQ_POLLED 등) — blk_mq_get_hctx_type()이 이 값으로 hctx
	 * 유형을 결정하고, 드라이버가 이를 NVMe opcode(nvme_cmd_read(0x02)/WRITE
	 * 등)로 변환한다.
	 * 설정자: 호출자가 bio->bi_opf 등에서 그대로 복사.
	 * 읽는 자: blk_mq_map_queue()/blk_mq_get_hctx_type() (hctx 결정),
	 *   드라이버 queue_rq()(opcode 결정).
	 * 값 범위: REQ_OP_* 값 + REQ_* 플래그의 OR 조합.
	 * 동기화: 스택 값, 락 불필요. */
	req_flags_t rq_flags;
	/* [한국어] 이 할당이 완료된 뒤 만들어질 request에 세팅될 RQF_* 상태
	 * 플래그 일부 — 특히 RQF_SCHED_TAGS가 서 있으면 드라이버 tag 대신
	 * 스케줄러 tag(hctx->sched_tags)를 사용하라는 의미다.
	 * 설정자: 스케줄러 연동 코드(blk-mq-sched.c)가 스케줄러 사용 여부에
	 *   따라 설정.
	 * 읽는 자: blk_mq_tags_from_data()가 이 플래그를 보고 sched_tags와
	 *   tags 중 어느 tag 풀에서 뽑을지 분기.
	 * 값 범위: RQF_* 비트 OR 조합.
	 * 동기화: 스택 값, 락 불필요. */

	/* allocate multiple requests/tags in one go */
	unsigned int nr_tags;
	/* [한국어] 한 번에 연속으로 확보할 tag 개수 — blk_mq_get_tags()가
	 * 여러 tag를 배치로 뽑아 cached_rqs에 채울 때 목표 개수로 쓰인다.
	 * 설정자: 배치 할당 호출자(예: io_uring 다중 제출 경로)가 설정.
	 * 읽는 자: blk_mq_get_tags() 내부 루프 종료 조건.
	 * 값 범위: 1 이상 — 1이면 사실상 단일 blk_mq_get_tag()와 동일한 효과.
	 * 동기화: 스택 값, 락 불필요. */
	struct rq_list *cached_rqs;
	/* [한국어] 이미 할당된 뒤 아직 쓰이지 않고 캐시되어 있는 request들의
	 * 리스트 — 호출자가 미리 확보해 둔 request를 재사용해 매번 새로
	 * 할당하는 오버헤드(및 doorbell/tag 획득 왕복)를 줄이는 배치 최적화용.
	 * 설정자: 호출자가 이전에 blk_mq_get_tags() 등으로 채워 둔 캐시를 전달.
	 * 읽는 자: blk_mq_get_tag()가 캐시에 남은 request가 있으면 그것부터
	 *   재사용하고, 없을 때만 새로 tag를 뽑는다(구현은 blk-mq.c).
	 * 값 범위: 유효한 rq_list 포인터 또는 NULL(캐시 미사용).
	 * 동기화: 호출자 단일 스레드 소유 리스트, 락 불필요. */

	/* input & output parameter */
	struct blk_mq_ctx *ctx;
	/* [한국어] 입력으로는 "이 CPU 소프트웨어 큐를 기준으로 매핑하라"는
	 * 힌트, 출력으로는 "실제로 선택된 소프트웨어 큐"가 담긴다.
	 * 설정자(입력): 호출자가 blk_mq_get_ctx()로 얻은 값을 미리 채우거나
	 *   NULL로 두어 함수 내부에서 결정하게 할 수 있다.
	 * 설정자(출력): blk_mq_get_tag() 등이 최종 확정된 ctx로 갱신.
	 * 읽는 자: 할당 완료 후 호출자가 새 request의 rq->mq_ctx 초기화 등에
	 *   사용.
	 * 값 범위: 유효한 blk_mq_ctx 포인터.
	 * 동기화: 스택 값(호출자 단일 스레드), 락 불필요. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] ctx와 마찬가지로 in/out — 입력은 힌트, 출력은 "이 요청이
	 * 실제로 나갈 하드웨어 큐(NVMe SQ/CQ 쌍)"를 확정한 결과다.
	 * 설정자(입력/출력): blk_mq_map_queue()/blk_mq_get_tag() 내부에서
	 *   확정되어 대입.
	 * 읽는 자: blk_mq_tags_from_data()가 hctx->tags 또는
	 *   hctx->sched_tags 선택에 사용, 이후 dispatch 경로 전체가 이
	 *   hctx를 기준으로 진행.
	 * 값 범위: 유효한 blk_mq_hw_ctx 포인터.
	 * 동기화: 스택 값, 락 불필요. */
};

/*
 * [한국어]
 * blk_mq_init_tags - 새 tag 풀(비트맵 2개 + 카운터)을 할당하고 초기화
 *
 * @nr_tags:       일반(비예약) tag 개수 상한.
 * @reserved_tags: 이 중 예약 영역으로 떼어 둘 개수(예: NVMe admin/flush
 *                 우선 처리용, 값 범위 하위 [0, reserved_tags) 가 예약됨).
 * @flags:         BLK_MQ_F_* 플래그(예: 라운드로빈 비트 할당 여부).
 * @node:          NUMA 노드 힌트 — 이 tag 풀(및 내부 sbitmap 배열)을 어느
 *                 노드 메모리에 둘지.
 * @return: 새로 할당된 struct blk_mq_tags 포인터, 실패 시 NULL.
 *
 * 내부적으로 bitmap_tags/breserved_tags 두 개의 sbitmap_queue를 각각의
 * 크기(reserved 영역과 비-reserved 영역)로 초기화한다. NVMe SQ 하나가
 * 만들어질 때 그 SQ에서 쓸 CID 비트맵을 만드는 것과 대응된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(큐/태그셋 초기화).
 *
 * 호출 체인:
 *   blk_mq_alloc_map_and_rqs() -> [blk_mq_init_tags] -> sbitmap_queue_init_node()
 */
struct blk_mq_tags *blk_mq_init_tags(unsigned int nr_tags,
		unsigned int reserved_tags, unsigned int flags, int node);
/*
 * [한국어]
 * blk_mq_free_tags - blk_mq_init_tags()로 만든 tag 풀을 해제
 *
 * @set:  드라이버 tag_set(해제 정책 참조용).
 * @tags: 해제할 struct blk_mq_tags.
 * @return: 없음(void).
 *
 * 내부 sbitmap_queue 두 개를 sbitmap_queue_free()로 해제하고 tags 구조체
 * 자체도 kfree 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_free_rq_map() -> [blk_mq_free_tags]
 */
void blk_mq_free_tags(struct blk_mq_tag_set *set, struct blk_mq_tags *tags);

/*
 * [한국어]
 * blk_mq_get_tag - blk_mq_alloc_data에 기술된 조건으로 tag 하나를 할당
 *
 * @data: q/ctx/hctx/flags/shallow_depth 등을 담은 할당 매개변수(in/out).
 * @return: 성공 시 할당된 tag 번호(NVMe라면 곧 CID), 실패(그리고
 *          BLK_MQ_REQ_NOWAIT 등으로 대기 불가) 시 BLK_MQ_NO_TAG.
 *
 * blk_mq_tags_from_data()로 드라이버 tag/스케줄러 tag 중 어느 풀에서 뽑을지
 * 정하고, hctx_may_queue()로 공유 tag 환경에서 이 hctx가 tag를 더 받아도
 * 되는지 확인한 뒤 sbitmap_queue에서 실제 비트를 찾아 세운다. 즉시 받을
 * tag가 없으면(그리고 blocking이 허용되면) sbq_wait 큐에서 잠들어 다른
 * 제출자가 tag를 반환할 때 깨어난다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — BLK_MQ_REQ_NOWAIT 미설정 시 sleep 가능.
 *
 * 호출 체인:
 *   blk_mq_get_new_requests()/blk_mq_alloc_request() -> [blk_mq_get_tag]
 *     -> blk_mq_tags_from_data() -> hctx_may_queue() ->
 *     __sbitmap_queue_get()/get_shallow()
 */
unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data);
/*
 * [한국어]
 * blk_mq_get_tags - 위와 같지만 tag를 nr_tags개 연속/배치로 확보
 *
 * @data:    할당 매개변수(cached_rqs 등에 결과를 채울 수 있음).
 * @nr_tags: 확보를 시도할 tag 개수.
 * @offset:  (출력) 실제로 확보된 tag들이 비트맵에서 시작하는 오프셋.
 * @return: 실제로 확보한 tag들을 나타내는 비트마스크(unsigned long) —
 *          비트 위치 + @offset을 더하면 실제 tag 번호가 된다.
 *
 * 여러 tag를 한 번의 sbitmap 연산으로 묶어 확보해, 다중 request를 배치로
 * 준비할 때(예: io_uring 다중 제출) 매번 개별 락/원자 연산을 반복하는
 * 오버헤드를 줄인다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_alloc_requests() 등 배치 할당 경로 -> [blk_mq_get_tags]
 */
unsigned long blk_mq_get_tags(struct blk_mq_alloc_data *data, int nr_tags,
		unsigned int *offset);
/*
 * [한국어]
 * blk_mq_put_tag - tag 하나를 반환(free)
 *
 * @tags: 반환 대상 tag 풀.
 * @ctx:  이 tag를 반환하는 소프트웨어 큐(wait queue 라운드로빈 힌트로 사용).
 * @tag:  반환할 tag 번호.
 * @return: 없음(void).
 *
 * sbitmap_queue_clear()로 해당 비트를 지우고, 그 tag를 기다리던 제출자가
 * 있으면(bt_wait_ptr() 로 고른 wait state를 통해) 깨운다. NVMe 관점에서는
 * 명령 완료(CQ 엔트리 수신) 후 그 CID를 재사용 가능 상태로 되돌리는
 * 마지막 단계다.
 * 실행 컨텍스트: 완료 경로(softirq) 또는 프로세스 컨텍스트 모두 가능.
 *
 * 호출 체인:
 *   __blk_mq_put_driver_tag()(본 파일) -> [blk_mq_put_tag] ->
 *     sbitmap_queue_clear()
 */
void blk_mq_put_tag(struct blk_mq_tags *tags, struct blk_mq_ctx *ctx,
		unsigned int tag);
/*
 * [한국어]
 * blk_mq_put_tags - 여러 tag를 한 번에 반환
 *
 * @tags:     반환 대상 tag 풀.
 * @tag_array: 반환할 tag 번호들의 배열.
 * @nr_tags:  배열의 원소 개수.
 * @return: 없음(void).
 *
 * blk_mq_put_tag()을 배치로 반복하는 대신, CQ 완료를 일괄 처리(io_comp_batch)
 * 할 때 여러 tag를 한 번에 반환해 오버헤드를 줄인다.
 * 실행 컨텍스트: 완료 경로(폴 배치 완료 등).
 *
 * 호출 체인:
 *   blk_mq_end_request_batch() 등 -> [blk_mq_put_tags]
 */
void blk_mq_put_tags(struct blk_mq_tags *tags, int *tag_array, int nr_tags);
/*
 * [한국어]
 * blk_mq_tag_resize_shared_tags - 공유 tag 풀(tag_set 전체 공유) 크기 변경
 *
 * @set:  드라이버 tag_set.
 * @size: 새 tag 풀 크기(요청 개수 상한).
 * @return: 없음(void).
 *
 * BLK_MQ_F_TAG_HCTX_SHARED 인 드라이버에서 nr_requests 변경 시, 여러 hctx가
 * 공유하는 tag_set->shared_tags 하나만 리사이즈하면 모든 hctx에 반영된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write, freeze 보호 하에 수행).
 *
 * 호출 체인:
 *   blk_mq_update_nr_requests() -> [blk_mq_tag_resize_shared_tags]
 */
void blk_mq_tag_resize_shared_tags(struct blk_mq_tag_set *set,
		unsigned int size);
/*
 * [한국어]
 * blk_mq_tag_update_sched_shared_tags - 스케줄러가 쓰는 공유 tag 수 갱신
 *
 * @q:  대상 request_queue(스케줄러가 붙어 있는 상태).
 * @nr: 새로운 스케줄러 tag 개수.
 * @return: 없음(void).
 *
 * mq-deadline/kyber/bfq 등 스케줄러가 자체적으로 유지하는 elevator_tags의
 * 크기를 nr_requests 변경에 맞춰 다시 계산한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_update_nr_requests() -> [blk_mq_tag_update_sched_shared_tags]
 */
void blk_mq_tag_update_sched_shared_tags(struct request_queue *q,
					 unsigned int nr);

/*
 * [한국어]
 * blk_mq_tag_wakeup_all - tag 풀에 딸린 모든 대기자를 강제로 깨운다
 *
 * @tags:    대상 tag 풀.
 * @bool 인자(이름 없음, 두 번째 매개변수): 예약(reserved) tag 대기열까지
 *          함께 깨울지 여부.
 * @return: 없음(void).
 *
 * 개별 tag 하나가 반환될 때 깨우는 blk_mq_put_tag()과 달리, 큐 unfreeze나
 * quiesce 해제처럼 "상황이 통째로 바뀌어 모두 다시 시도해봐야 하는" 시점에
 * wait queue 전체를 깨운다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_wake_waiters() -> [blk_mq_tag_wakeup_all] (hctx마다 반복)
 */
void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool);
/*
 * [한국어]
 * blk_mq_queue_tag_busy_iter - 큐 전체의 사용 중(busy) tag를 순회하며 콜백 호출
 *
 * @q:   대상 request_queue.
 * @fn:  각 busy request마다 호출할 콜백(busy_tag_iter_fn), false 반환 시 중단.
 * @priv: 콜백에 그대로 전달되는 사용자 데이터.
 * @return: 없음(void).
 *
 * 큐에 속한 모든 hctx의 tag 풀(공유/비공유 모두)을 훑어, 현재 outstanding
 * (드라이버에 넘어가 완료를 기다리는) 상태인 request들에 대해 fn을 호출한다.
 * timeout 검사, 통계 집계(blk_mq_in_driver_rw), 강제 abort 등에 쓰인다.
 * 실행 컨텍스트: 프로세스 컨텍스트(동기 순회, 콜백 안에서 sleep 하면 안 됨이
 * 일반적).
 *
 * 호출 체인:
 *   blk_mq_in_driver_rw()/blk_mq_timeout_work() 등 ->
 *     [blk_mq_queue_tag_busy_iter] -> blk_mq_all_tag_iter() (hctx별)
 */
void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_tag_iter_fn *fn,
		void *priv);
/*
 * [한국어]
 * blk_mq_all_tag_iter - 특정 tag 풀 하나(bitmap_tags + breserved_tags)를 순회
 *
 * @tags: 순회할 tag 풀.
 * @fn:   각 busy tag마다 호출할 콜백.
 * @priv: 콜백 사용자 데이터.
 * @return: 없음(void).
 *
 * blk_mq_queue_tag_busy_iter()가 hctx 단위로 내려가서 실제로 호출하는
 * 하위 순회 함수 — sbitmap_for_each_set() 계열로 세팅된 비트(=사용 중
 * tag)를 찾아 그 tag에 대응하는 request로 fn을 호출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blk_mq_queue_tag_busy_iter() -> [blk_mq_all_tag_iter] ->
 *     블록마다 fn(rq, priv) 호출
 */
void blk_mq_all_tag_iter(struct blk_mq_tags *tags, busy_tag_iter_fn *fn,
		void *priv);

/*
 * [한국어]
 * bt_wait_ptr - sbitmap_queue에서 이번에 사용할 wait state(대기열 슬롯)를 선택
 *
 * @bt:   tag 비트맵을 감싸는 sbitmap_queue.
 * @hctx: 이 tag 요청이 속한 하드웨어 큐(없으면 NULL).
 * @return: 사용할 struct sbq_wait_state 포인터.
 *
 * sbitmap_queue는 wake-up thundering herd를 줄이기 위해 내부에 여러 개의
 * wait state(ws[] 배열)를 두고 라운드로빈으로 분산한다. hctx가 주어지면
 * hctx->wait_index(hctx별 회전 카운터)를 이용해 분산하고, hctx가 없으면
 * (공유 컨텍스트 등) 첫 번째 공용 wait state(ws[0])를 그대로 쓴다.
 * 실행 컨텍스트: tag 대기가 필요한 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sbitmap 내부(__sbitmap_queue_get 실패 시)/blk_mq_put_tag() ->
 *     [bt_wait_ptr] -> sbq_wait_ptr()
 */
static inline struct sbq_wait_state *bt_wait_ptr(struct sbitmap_queue *bt,
						 struct blk_mq_hw_ctx *hctx)
{
	if (!hctx)
		return &bt->ws[0];
	return sbq_wait_ptr(bt, &hctx->wait_index);
}

/*
 * [한국어]
 * __blk_mq_tag_busy - (내부 구현) 공유 tag 환경에서 이 hctx를 활성 상태로 등록
 *
 * @hctx(이름 없는 매개변수): 활성화할 하드웨어 큐.
 * @return: 없음(void).
 *
 * hctx->tags->active_queues 카운터를 증가시키고 필요하면
 * BLK_MQ_S_TAG_ACTIVE 상태 비트를 세팅한다(실제 구현은 blk-mq-tag.c).
 * blk_mq_tag_busy() 래퍼를 통해서만 호출되도록 의도된 내부 함수다.
 * 실행 컨텍스트: 제출 경로, 첫 request가 이 hctx로 갈 때.
 *
 * 호출 체인:
 *   blk_mq_tag_busy() -> [__blk_mq_tag_busy]
 */
void __blk_mq_tag_busy(struct blk_mq_hw_ctx *);
/*
 * [한국어]
 * __blk_mq_tag_idle - (내부 구현) 공유 tag 환경에서 이 hctx를 비활성으로 등록 해제
 *
 * @hctx(이름 없는 매개변수): 비활성화할 하드웨어 큐.
 * @return: 없음(void).
 *
 * active_queues 카운터를 감소시키고 필요하면 BLK_MQ_S_TAG_ACTIVE 비트를
 * 해제한다 — 더 이상 이 hctx가 공유 tag 경쟁에 참여하지 않음을 표시해,
 * hctx_may_queue()의 공정 분배 계산에서 제외되게 한다.
 * 실행 컨텍스트: 큐가 quiesce/idle 상태로 전이될 때.
 *
 * 호출 체인:
 *   blk_mq_tag_idle() -> [__blk_mq_tag_idle]
 */
void __blk_mq_tag_idle(struct blk_mq_hw_ctx *);

/*
 * [한국어]
 * blk_mq_tag_busy - 공유 tag 모드일 때만 이 hctx를 활성으로 표시
 *
 * @hctx: 대상 하드웨어 큐.
 * @return: 없음(void).
 *
 * BLK_MQ_F_TAG_QUEUE_SHARED 플래그(여러 hctx/네임스페이스가 하나의 tag
 * 풀을 공유)가 없다면 이 hctx는 애초에 경쟁 상대가 없으므로 아무 일도
 * 하지 않는다 — 불필요한 원자 연산을 피하는 fast-path 가드다.
 * 실행 컨텍스트: 제출 경로.
 *
 * 호출 체인:
 *   blk_mq_get_driver_tag() 계열 -> [blk_mq_tag_busy] -> __blk_mq_tag_busy()
 */
static inline void blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_tag_busy(hctx);
}

/*
 * [한국어]
 * blk_mq_tag_idle - 공유 tag 모드일 때만 이 hctx를 비활성으로 표시
 *
 * @hctx: 대상 하드웨어 큐.
 * @return: 없음(void).
 *
 * blk_mq_tag_busy()의 반대 동작 — 공유 tag가 아니면 아무 일도 하지 않는다.
 * 실행 컨텍스트: 큐 quiesce/idle 경로.
 *
 * 호출 체인:
 *   blk_mq_quiesce_queue() 등 -> [blk_mq_tag_idle] -> __blk_mq_tag_idle()
 */
static inline void blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_tag_idle(hctx);
}

/*
 * [한국어]
 * blk_mq_tag_is_reserved - 주어진 tag가 예약(reserved) 영역에 속하는지 검사
 *
 * @tags: 대상 tag 풀.
 * @tag:  검사할 tag 번호.
 * @return: true면 예약 tag(0 ~ nr_reserved_tags-1), false면 일반 tag.
 *
 * blk_mq_init_tags()에서 tags->nr_reserved_tags 개수만큼 앞쪽 구간을
 * 예약 영역으로 분리해 둔 것을 그대로 비교한다. 예약 tag는 보통 flush
 * request 등 특별 취급이 필요한 request에 쓰인다.
 * 실행 컨텍스트: 어디서든 호출 가능한 순수 비교 함수.
 *
 * 호출 체인:
 *   blk_mq_tag_to_rq() 등 tag->request 변환 경로에서 예약 여부 판단에 사용
 */
static inline bool blk_mq_tag_is_reserved(struct blk_mq_tags *tags,
					  unsigned int tag)
{
	return tag < tags->nr_reserved_tags;
}

/*
 * [한국어]
 * blk_mq_is_shared_tags - tag 풀 전체(hctx 단위가 아니라 tag_set 단위)가
 * 공유되는지 검사
 *
 * @flags: hctx->flags 또는 tag_set->flags(BLK_MQ_F_* 비트 조합).
 * @return: true면 BLK_MQ_F_TAG_HCTX_SHARED — 여러 hctx가 물리적으로 같은
 *          하나의 blk_mq_tags(및 그 sbitmap)를 공유한다는 뜻.
 *
 * BLK_MQ_F_TAG_QUEUE_SHARED(공유 tag "환경"인지, 즉 active_queues 카운팅이
 * 필요한지)와는 구분되는 개념이다 — HCTX_SHARED는 "물리적으로 같은 tags
 * 객체를 쓰는가"를 가리고, 이 값에 따라 __blk_mq_add_active_requests() 등이
 * hctx->nr_active 대신 hctx->queue->nr_active_requests_shared_tags 전역
 * 카운터를 쓸지 결정한다.
 * 실행 컨텍스트: 어디서든 호출 가능한 순수 비트 검사.
 *
 * 호출 체인:
 *   __blk_mq_add_active_requests()/hctx_may_queue() 등 다수 -> [blk_mq_is_shared_tags]
 */
static inline bool blk_mq_is_shared_tags(unsigned int flags)
{
	return flags & BLK_MQ_F_TAG_HCTX_SHARED;
}

/*
 * [한국어]
 * blk_mq_tags_from_data - 할당 매개변수(data)로부터 실제로 사용할 tag 풀을 선택
 *
 * @data: hctx/rq_flags를 담은 blk_mq_alloc_data.
 * @return: data->hctx->sched_tags 또는 data->hctx->tags 중 하나.
 *
 * RQF_SCHED_TAGS 플래그(data->rq_flags에 설정)는 "이 할당은 스케줄러가
 * 관리하는 중간 tag 풀(hctx->sched_tags)에서 이루어져야 한다"는 뜻이다 —
 * mq-deadline/kyber/bfq처럼 스케줄러가 자체 우선순위로 dispatch 시점까지
 * request를 들고 있다가, 실제 드라이버에 넘길 때 별도로 드라이버 tag를
 * 받는 2단계 tag 체계에서 첫 번째 단계에 해당한다. 플래그가 없으면
 * 스케줄러 없이(none) hctx->tags(드라이버 tag 풀, NVMe라면 SQ CID 풀)를
 * 바로 사용한다.
 * 실행 컨텍스트: tag 할당 경로 어디서든.
 *
 * 호출 체인:
 *   blk_mq_get_tag()/blk_mq_get_tags() -> [blk_mq_tags_from_data]
 */
static inline struct blk_mq_tags *blk_mq_tags_from_data(struct blk_mq_alloc_data *data)
{
	if (data->rq_flags & RQF_SCHED_TAGS)
		return data->hctx->sched_tags;
	return data->hctx->tags;
}

/*
 * [한국어]
 * blk_mq_hctx_stopped - 이 hctx(SQ/CQ)가 dispatch 중지 상태인지 검사
 *
 * @hctx: 검사할 하드웨어 큐.
 * @return: true면 새 dispatch를 하면 안 되는 중지 상태, false면 정상 진행 가능.
 *
 * BLK_MQ_S_STOPPED 비트는 blk_mq_stop_hw_queue()가 세팅하고
 * blk_mq_start_stopped_hw_queue()가 해제한다. NVMe 컨트롤러 reset, PCI
 * 오류, 큐 timeout 복구 등의 상황에서 일시적으로 이 hctx로의 제출을
 * 막아야 할 때 쓰인다.
 * 실행 컨텍스트: dispatch 경로(프로세스/softirq), 매 dispatch 시도마다 호출.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue()/blk_mq_sched_dispatch_requests() ->
 *     [blk_mq_hctx_stopped] -> (true면 dispatch 생략)
 */
static inline bool blk_mq_hctx_stopped(struct blk_mq_hw_ctx *hctx)
{
	/* Fast path: hardware queue is not stopped most of the time. */
	/* [한국어] 대부분의 경우 hctx는 정지 상태가 아니므로, likely()로
	 * 분기 예측기에 힌트를 주어 이 흔한 경로를 빠르게 처리한다. */
	if (likely(!test_bit(BLK_MQ_S_STOPPED, &hctx->state)))
		return false;

	/*
	 * This barrier is used to order adding of dispatch list before and
	 * the test of BLK_MQ_S_STOPPED below. Pairs with the memory barrier
	 * in blk_mq_start_stopped_hw_queue() so that dispatch code could
	 * either see BLK_MQ_S_STOPPED is cleared or dispatch list is not
	 * empty to avoid missing dispatching requests.
	 */
	/* [한국어] 위 원본 주석대로, blk_mq_start_stopped_hw_queue()가
	 * STOPPED 비트를 지우는 것과 dispatch list에 새 request를 추가하는
	 * 두 스레드 간의 순서를 강제한다. 이 배리어가 없으면 "STOPPED가
	 * 지워지는 것도 못 보고, dispatch list에 항목이 추가된 것도 못
	 * 보는" 최악의 재정렬이 일어나 request가 영원히 dispatch되지 못하고
	 * 누락될 수 있다. */
	smp_mb();

	/* [한국어] 배리어 이후 다시 한 번 STOPPED 비트를 확인 — 이번 결과가
	 * 최종 반환값이 된다(fast path에서 놓쳤을 수 있는 최신 상태 반영). */
	return test_bit(BLK_MQ_S_STOPPED, &hctx->state);
}

/*
 * [한국어]
 * blk_mq_hw_queue_mapped - 이 hctx가 실제로 쓸 수 있게 매핑되어 있는지 검사
 *
 * @hctx: 검사할 하드웨어 큐.
 * @return: true면 CPU 매핑도 있고 tag 풀도 준비된 유효한 hctx.
 *
 * hctx는 정의상 존재하더라도(배열에 들어 있더라도) 실제로 어떤 CPU도
 * 매핑되지 않았거나(hot-unplug로 모든 대응 CPU가 빠진 경우) tag 풀이 아직
 * 할당되지 않았을 수 있다. 이런 hctx는 dispatch 대상에서 제외해야 한다.
 * 실행 컨텍스트: hctx 순회 경로 어디서든.
 *
 * 호출 체인:
 *   blk_mq_run_hw_queues()/큐 초기화 검증 경로 -> [blk_mq_hw_queue_mapped]
 */
static inline bool blk_mq_hw_queue_mapped(struct blk_mq_hw_ctx *hctx)
{
	return hctx->nr_ctx && hctx->tags;
}

/*
 * [한국어]
 * blk_mq_in_driver_rw - (선언, 정의는 이 파일 밖) 특정 블록 디바이스의
 * READ/WRITE in-flight 개수를 조회 — 상세 설명은 이 함수가 사용하는
 * blk_mq_queue_tag_busy_iter()/blk_mq_check_in_driver() 주석을 blk-mq.c에서
 * 참고(그쪽 파일에 라인 단위 주석 완비).
 *
 * @part:     대상 블록 디바이스(파티션 또는 전체 디스크).
 * @inflight: [출력] inflight[0]=READ 개수, inflight[1]=WRITE 개수.
 * @return: 없음(void).
 *
 * 호출 체인:
 *   blk_inflight_rw()(blk-sysfs.c, iostat 등) -> [blk_mq_in_driver_rw]
 *     -> blk_mq_queue_tag_busy_iter()
 */
void blk_mq_in_driver_rw(struct block_device *part, unsigned int inflight[2]);

/*
 * blk_mq_put_dispatch_budget() / blk_mq_get_dispatch_budget():
 * dispatch 시점에 드라이버 수준 예산을 관리한다.
 *
 * [한국어] "budget"은 tag(CID)와는 별개로, 드라이버가 자체적으로 두는 동시
 * inflight 명령 수 상한이다. 대부분의 드라이버(NVMe 포함)는 budget 콜백을
 * 등록하지 않고 tag 개수만으로 충분히 제한하지만, 일부 드라이버(예: SCSI
 * 일부 LLD)는 호스트 어댑터 자원 제한 때문에 별도 budget이 필요하다.
 * set_rq_budget_token/get_rq_budget_token은 이렇게 확보한 budget "토큰"을
 * request 자체에 실어 날라, 나중에(완료/timeout 시) 정확히 짝을 맞춰
 * 반환할 수 있게 한다.
 */
/*
 * [한국어]
 * blk_mq_put_dispatch_budget - 확보했던 dispatch budget을 드라이버에 반환
 *
 * @q:            대상 request_queue.
 * @budget_token: blk_mq_get_dispatch_budget()이 반환했던 토큰.
 * @return: 없음(void).
 *
 * 호출 체인:
 *   dispatch 실패/request 완료 경로 -> [blk_mq_put_dispatch_budget] ->
 *     q->mq_ops->put_budget (드라이버 콜백)
 */
static inline void blk_mq_put_dispatch_budget(struct request_queue *q,
					      int budget_token)
{
	/* [한국어] 드라이버가 put_budget 콜백을 등록했을 때만 호출 — budget을
	 * 아예 쓰지 않는 드라이버(NVMe 대부분)는 이 블록이 no-op이다. */
	if (q->mq_ops->put_budget)
		q->mq_ops->put_budget(q, budget_token);
}

/*
 * [한국어]
 * blk_mq_get_dispatch_budget - dispatch 전에 드라이버 수준 budget을 확보 시도
 *
 * @q: 대상 request_queue.
 * @return: 드라이버가 반환한 budget 토큰(0 이상, 의미는 드라이버 정의),
 *          budget 미사용 드라이버라면 0(제한 없음을 뜻하는 관용값).
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list() -> [blk_mq_get_dispatch_budget] ->
 *     q->mq_ops->get_budget (드라이버 콜백)
 */
static inline int blk_mq_get_dispatch_budget(struct request_queue *q)
{
	/* [한국어] get_budget 콜백이 있으면 그 결과(토큰 또는 실패 코드)를
	 * 그대로 돌려준다 — 실패 시 dispatch가 뒤로 미뤄진다. */
	if (q->mq_ops->get_budget)
		return q->mq_ops->get_budget(q);
	/* [한국어] budget 콜백이 없는 드라이버는 애초에 budget 개념이 없으므로
	 * 항상 성공(0)으로 간주 — 그 드라이버는 tag 개수만으로 제한된다. */
	return 0;
}

/*
 * [한국어]
 * blk_mq_set_rq_budget_token - request에 budget 토큰을 저장
 *
 * @rq:    대상 request.
 * @token: 저장할 토큰 값(음수면 무효).
 * @return: 없음(void).
 *
 * 호출 체인:
 *   blk_mq_get_budget_and_tag() 등 -> [blk_mq_set_rq_budget_token] ->
 *     rq->q->mq_ops->set_rq_budget_token (드라이버 콜백)
 */
static inline void blk_mq_set_rq_budget_token(struct request *rq, int token)
{
	/* [한국어] 토큰이 음수면 애초에 budget을 획득하지 못했다는 뜻이므로
	 * 저장할 것이 없어 그냥 반환한다. */
	if (token < 0)
		return;

	/* [한국어] 드라이버가 set_rq_budget_token 콜백을 제공하면, 나중에
	 * 완료/timeout 시 이 request로부터 토큰을 되찾아(get_rq_budget_token)
	 * 정확히 반환(put_dispatch_budget)할 수 있도록 지금 기록해 둔다. */
	if (rq->q->mq_ops->set_rq_budget_token)
		rq->q->mq_ops->set_rq_budget_token(rq, token);
}

/*
 * [한국어]
 * blk_mq_get_rq_budget_token - request에 저장된 budget 토큰을 조회
 *
 * @rq: 대상 request.
 * @return: 저장되어 있던 토큰 값, 콜백 미지원 드라이버면 -1(토큰 없음).
 *
 * 호출 체인:
 *   request 완료/timeout 처리 경로 -> [blk_mq_get_rq_budget_token] ->
 *     blk_mq_put_dispatch_budget()로 반환
 */
static inline int blk_mq_get_rq_budget_token(struct request *rq)
{
	/* [한국어] 드라이버가 get_rq_budget_token 콜백을 제공하면 그 결과를
	 * 그대로 반환. */
	if (rq->q->mq_ops->get_rq_budget_token)
		return rq->q->mq_ops->get_rq_budget_token(rq);
	/* [한국어] budget 미사용 드라이버는 -1을 반환해 "돌려줄 토큰 없음"을
	 * 호출자에게 알린다. */
	return -1;
}

/*
 * active_requests 계열 함수: 공유 태그 환경에서 현재 hctx/SQ의 outstanding
 * 명령 수를 추적한다.
 *
 * [한국어] 이 함수군은 두 계층으로 나뉜다:
 *   1) __blk_mq_*_active_requests() (밑줄 두 개, 무조건 카운트 갱신) —
 *      실제 원자 연산을 수행하는 내부 구현.
 *   2) blk_mq_*_active_requests() (밑줄 없음, BLK_MQ_F_TAG_QUEUE_SHARED일
 *      때만 갱신) — 공유 tag 환경이 아니면 카운팅 자체가 무의미하므로
 *      원자 연산 비용을 아끼는 얇은 가드.
 * "shared_tags"(BLK_MQ_F_TAG_HCTX_SHARED)인 경우 여러 hctx가 물리적으로
 * 같은 카운터(hctx->queue->nr_active_requests_shared_tags)를 공유하고,
 * 그렇지 않은 일반 공유(BLK_MQ_F_TAG_QUEUE_SHARED만 있는 경우)는 hctx마다
 * 자신만의 hctx->nr_active를 쓴다. hctx_may_queue()가 이 카운터를 읽어
 * SQ별 공정한 tag 분배 여부를 판단한다.
 */
/*
 * [한국어]
 * __blk_mq_add_active_requests - (내부) active-request 카운터에 val을 가산
 *
 * @hctx: 대상 하드웨어 큐.
 * @val:  더할 값(보통 1, 배치 처리 시 여러 개).
 * @return: 없음(void).
 *
 * 호출 체인:
 *   __blk_mq_inc_active_requests()/blk_mq_add_active_requests() ->
 *     [__blk_mq_add_active_requests]
 */
static inline void __blk_mq_add_active_requests(struct blk_mq_hw_ctx *hctx,
						int val)
{
	/* [한국어] tag 풀 자체가 tag_set 전체에서 물리적으로 공유되면
	 * (HCTX_SHARED), 개별 hctx 카운터가 아니라 큐 전역 카운터에 더한다
	 * — 그래야 여러 hctx가 같은 tag 풀을 보고 있다는 사실이 카운트에도
	 * 반영된다. */
	if (blk_mq_is_shared_tags(hctx->flags))
		atomic_add(val, &hctx->queue->nr_active_requests_shared_tags);
	else
		/* [한국어] 일반적인 경우: 이 hctx 전용 카운터에 더한다. */
		atomic_add(val, &hctx->nr_active);
}

/*
 * [한국어]
 * __blk_mq_inc_active_requests - (내부) active-request 카운터를 1 증가
 * @hctx: 대상 하드웨어 큐. @return: 없음(void).
 * 호출 체인: blk_mq_inc_active_requests() -> [__blk_mq_inc_active_requests]
 */
static inline void __blk_mq_inc_active_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] 증가값 1로 고정한 __blk_mq_add_active_requests() 특수화 —
	 * request 하나가 새로 dispatch될 때(tag를 막 확보했을 때) 호출된다. */
	__blk_mq_add_active_requests(hctx, 1);
}

/*
 * [한국어]
 * __blk_mq_sub_active_requests - (내부) active-request 카운터에서 val을 차감
 * @hctx: 대상 하드웨어 큐. @val: 뺄 값. @return: 없음(void).
 * 호출 체인: __blk_mq_dec_active_requests()/blk_mq_sub_active_requests() ->
 *   [__blk_mq_sub_active_requests]
 */
static inline void __blk_mq_sub_active_requests(struct blk_mq_hw_ctx *hctx,
		int val)
{
	/* [한국어] add와 대칭 — 공유 tag 풀이면 전역 카운터에서, 아니면
	 * hctx 전용 카운터에서 뺀다. */
	if (blk_mq_is_shared_tags(hctx->flags))
		atomic_sub(val, &hctx->queue->nr_active_requests_shared_tags);
	else
		atomic_sub(val, &hctx->nr_active);
}

/*
 * [한국어]
 * __blk_mq_dec_active_requests - (내부) active-request 카운터를 1 감소
 * @hctx: 대상 하드웨어 큐. @return: 없음(void).
 * 호출 체인: __blk_mq_put_driver_tag() -> [__blk_mq_dec_active_requests]
 */
static inline void __blk_mq_dec_active_requests(struct blk_mq_hw_ctx *hctx)
{
	/* [한국어] request 하나가 완료되거나 abort되어 tag를 반환할 때
	 * 호출되어 outstanding 카운트를 1 줄인다. */
	__blk_mq_sub_active_requests(hctx, 1);
}

/*
 * [한국어]
 * blk_mq_add_active_requests - BLK_MQ_F_TAG_QUEUE_SHARED일 때만 카운트 가산
 * @hctx: 대상 하드웨어 큐. @val: 더할 값. @return: 없음(void).
 * 호출 체인: (배치 dispatch 성공 경로) -> [blk_mq_add_active_requests]
 */
static inline void blk_mq_add_active_requests(struct blk_mq_hw_ctx *hctx,
					      int val)
{
	/* [한국어] 공유 tag 환경이 아니면 hctx_may_queue()의 공정 분배 계산
	 * 자체가 동작하지 않으므로(항상 true 반환) 카운트를 갱신할 필요가
	 * 없다 — 불필요한 원자 연산을 생략하는 fast-path. */
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_add_active_requests(hctx, val);
}

/*
 * [한국어]
 * blk_mq_inc_active_requests - BLK_MQ_F_TAG_QUEUE_SHARED일 때만 카운트 +1
 * @hctx: 대상 하드웨어 큐. @return: 없음(void).
 * 호출 체인: __blk_mq_alloc_driver_tag()(blk-mq.c, tag 확보 성공 시) ->
 *   [blk_mq_inc_active_requests]
 */
static inline void blk_mq_inc_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_inc_active_requests(hctx);
}

/*
 * [한국어]
 * blk_mq_sub_active_requests - BLK_MQ_F_TAG_QUEUE_SHARED일 때만 카운트 차감
 * @hctx: 대상 하드웨어 큐. @val: 뺄 값. @return: 없음(void).
 * 호출 체인: (배치 완료 처리 경로) -> [blk_mq_sub_active_requests]
 */
static inline void blk_mq_sub_active_requests(struct blk_mq_hw_ctx *hctx,
					      int val)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_sub_active_requests(hctx, val);
}

/*
 * [한국어]
 * blk_mq_dec_active_requests - BLK_MQ_F_TAG_QUEUE_SHARED일 때만 카운트 -1
 * @hctx: 대상 하드웨어 큐. @return: 없음(void).
 * 호출 체인: __blk_mq_put_driver_tag()(본 파일) -> [blk_mq_dec_active_requests]
 */
static inline void blk_mq_dec_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		__blk_mq_dec_active_requests(hctx);
}

/*
 * [한국어]
 * __blk_mq_active_requests - 이 hctx의 현재 active(outstanding) 명령 수를 조회
 *
 * @hctx: 대상 하드웨어 큐.
 * @return: 공유 tag 풀이면 tag_set 전역 카운터, 아니면 hctx 전용 카운터 값.
 *
 * hctx_may_queue()가 이 값을 quota(SQ당 허용된 tag 상한)와 비교해 새
 * request를 더 받을지 판단하는 데 사용하는 읽기 전용 조회 함수다.
 * 실행 컨텍스트: 어디서든 호출 가능(atomic_read 만 수행).
 *
 * 호출 체인:
 *   hctx_may_queue() -> [__blk_mq_active_requests]
 */
static inline int __blk_mq_active_requests(struct blk_mq_hw_ctx *hctx)
{
	if (blk_mq_is_shared_tags(hctx->flags))
		return atomic_read(&hctx->queue->nr_active_requests_shared_tags);
	return atomic_read(&hctx->nr_active);
}

/*
 * [한국어]
 * __blk_mq_put_driver_tag - (내부) request에서 driver tag를 회수해 반환
 *
 * @hctx: 이 request가 물려 있던 하드웨어 큐.
 * @rq:   tag를 반환할 request.
 * @return: 없음(void).
 *
 * outstanding 카운트 감소 -> 실제 tag 비트 반환 -> request의 tag 필드를
 * "미보유" 상태로 리셋, 이 세 단계를 정확한 순서로 수행한다. 순서가
 * 중요한 이유: 카운트를 먼저 줄여야 hctx_may_queue()가 즉시 새로운
 * 제출자에게 문을 열어줄 수 있고, tag를 실제로 반환해야 그 제출자가
 * sbitmap에서 그 tag를 다시 뽑을 수 있다.
 * 실행 컨텍스트: 완료 경로(softirq) 또는 요청 실패/abort 경로.
 *
 * 호출 체인:
 *   blk_mq_put_driver_tag() -> [__blk_mq_put_driver_tag] ->
 *     blk_mq_dec_active_requests() -> blk_mq_put_tag()
 */
static inline void __blk_mq_put_driver_tag(struct blk_mq_hw_ctx *hctx,
					   struct request *rq)
{
	/* [한국어] outstanding(active) 카운트를 먼저 줄인다 — hctx_may_queue()가
	 * 다음 순간부터 이 hctx에 여유가 생겼다고 판단할 수 있게 한다. */
	blk_mq_dec_active_requests(hctx);
	/* [한국어] 실제 tag 비트를 sbitmap_queue에 반환하고, 대기 중이던
	 * 제출자가 있으면 깨운다(bt_wait_ptr()이 고른 wait state를 통해). */
	blk_mq_put_tag(hctx->tags, rq->mq_ctx, rq->tag);
	/* [한국어] request가 더 이상 유효한 하드웨어 tag(NVMe라면 SQ 슬롯)를
	 * 가리키지 않음을 표시 — 중복 반환/오용 방지의 핵심 불변조건. */
	rq->tag = BLK_MQ_NO_TAG;
}

/*
 * [한국어]
 * blk_mq_put_driver_tag - request의 driver tag를 안전하게(중복 방지) 반환
 *
 * @rq: 대상 request.
 * @return: 없음(void).
 *
 * 이미 tag가 반환되었거나(rq->tag == NO_TAG) 애초에 내부(스케줄러) tag조차
 * 할당되지 않은 request(rq->internal_tag == NO_TAG, 스케줄러 tag 확보 전
 * 단계에서 실패한 경우)에 대해서는 아무 것도 하지 않는 가드를 거친 뒤,
 * 실제 반환은 __blk_mq_put_driver_tag()에 위임한다.
 * 실행 컨텍스트: 완료/에러 처리 경로.
 *
 * 호출 체인:
 *   blk_mq_end_request()/dispatch 실패 처리 등 -> [blk_mq_put_driver_tag]
 *     -> __blk_mq_put_driver_tag()
 */
static inline void blk_mq_put_driver_tag(struct request *rq)
{
	/* [한국어] 두 조건 중 하나라도 해당하면 반환할 유효한 driver tag가
	 * 없다는 뜻 — 중복 반환(double free 유사 버그)을 막기 위한 가드. */
	if (rq->tag == BLK_MQ_NO_TAG || rq->internal_tag == BLK_MQ_NO_TAG)
		return;

	__blk_mq_put_driver_tag(rq->mq_hctx, rq);
}

/*
 * [한국어]
 * __blk_mq_alloc_driver_tag - (구현은 blk-mq.c) request에 실제로 driver tag를
 * 새로 할당 시도
 *
 * @rq: tag가 필요한 request.
 * @return: true면 rq->tag에 새 tag가 채워짐, false면 tag 풀 소진으로 실패.
 *
 * hctx_may_queue()의 공정 분배 검사를 통과해야 하며, 성공 시
 * blk_mq_inc_active_requests()로 outstanding 카운트를 올린다(상세 라인
 * 주석은 blk-mq.c).
 * 실행 컨텍스트: dispatch 직전(프로세스/softirq).
 *
 * 호출 체인:
 *   blk_mq_get_driver_tag() -> [__blk_mq_alloc_driver_tag]
 */
bool __blk_mq_alloc_driver_tag(struct request *rq);

/*
 * [한국어]
 * blk_mq_get_driver_tag - request에 driver tag가 없으면 새로 확보
 *
 * @rq: 대상 request.
 * @return: true면 지금 이 request는 유효한 driver tag를 갖고 있음(원래
 *          있었거나 방금 확보), false면 tag를 못 구해 아직 dispatch할 수
 *          없음.
 *
 * dispatch 직전에 호출되어, NVMe 관점에서는 SQ 엔트리에 써 넣을 CID를
 * 마지막으로 확정하는 지점이다. 이미 tag가 있으면(재시도 경로 등) 굳이
 * 다시 할당하지 않고 그대로 통과시킨다.
 * 실행 컨텍스트: dispatch 경로(프로세스/softirq).
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list() -> [blk_mq_get_driver_tag] ->
 *     __blk_mq_alloc_driver_tag() (필요한 경우만)
 */
static inline bool blk_mq_get_driver_tag(struct request *rq)
{
	/* [한국어] 아직 tag가 없는 경우에만(&&의 왼쪽이 참일 때만) 새로
	 * 할당을 시도한다 — 할당 시도 자체가 실패하면 전체 조건이 참이 되어
	 * false를 반환(단락 평가로 이미 tag가 있으면 우변은 평가되지 않음). */
	if (rq->tag == BLK_MQ_NO_TAG && !__blk_mq_alloc_driver_tag(rq))
		return false;

	return true;
}

/*
 * [한국어]
 * blk_mq_clear_mq_map - CPU->hctx 매핑 테이블을 전부 0(기본 큐)으로 리셋
 *
 * @qmap: 초기화할 struct blk_mq_queue_map.
 * @return: 없음(void).
 *
 * 매핑 재계산(blk_mq_update_nr_hw_queues 등) 시작 전에 이전 매핑을 지우는
 * 용도. 0으로 리셋하는 이유는 possible이지만 online이 아닌 CPU, 혹은
 * 아직 실제 매핑이 계산되지 않은 CPU가 최소한 유효한 큐 인덱스(0번)를
 * 가리키도록 보장해 잘못된(범위 밖) 인덱스 참조를 막기 위함이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(큐 토폴로지 재계산, freeze 상태).
 *
 * 호출 체인:
 *   blk_mq_map_queues()/set_map() 계열(blk-mq-cpumap.c) 진입부 ->
 *     [blk_mq_clear_mq_map] -> (이후 실제 매핑 계산으로 덮어씀)
 */
static inline void blk_mq_clear_mq_map(struct blk_mq_queue_map *qmap)
{
	int cpu;

	/* [한국어] 커널이 인식하는 모든 "possible" CPU(핫플러그로 아직
	 * 온라인이 아닌 CPU까지 포함)를 순회 — 나중에 그 CPU가 온라인될 때
	 * 매핑이 이미 유효한 기본값을 갖고 있도록 하기 위함. */
	for_each_possible_cpu(cpu)
		/* [한국어] 해당 CPU의 매핑을 큐 인덱스 0(기본/첫 번째 하드웨어
		 * 큐)으로 리셋 — 아직 실제 토폴로지 계산 전 임시 안전값. */
		qmap->mq_map[cpu] = 0;
}

/* Free all requests on the list */
/*
 * [한국어]
 * blk_mq_free_requests - 리스트에 걸린 모든 request를 해제
 *
 * @list: 해제할 request들의 연결 리스트 헤드.
 * @return: 없음(void).
 *
 * 아직 드라이버로 dispatch되지 않은(즉 tag/SQ 슬롯을 확보하지 못했거나,
 * 확보했더라도 실제 doorbell 전에 취소된) request들을 한꺼번에 정리할 때
 * 쓰는 범용 헬퍼 — 에러 처리, 큐 teardown 등에서 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(에러/teardown 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() 에러 경로/큐 teardown -> [blk_mq_free_requests]
 *     -> blk_mq_free_request() (request마다 반복)
 */
static inline void blk_mq_free_requests(struct list_head *list)
{
	/* [한국어] 리스트가 빌 때까지 반복 — list_empty()는 head->next ==
	 * head 인지만 확인하는 저비용 검사. */
	while (!list_empty(list)) {
		/* [한국어] 리스트의 첫 엔트리를 struct request로 역참조
		 * (container_of 패턴) — list->next가 가리키는 rq->queuelist
		 * 로부터 바깥의 struct request 포인터를 복원. */
		struct request *rq = list_entry_rq(list->next);

		/* [한국어] 리스트에서 이 request를 제거하고, queuelist 필드
		 * 자체도 빈 리스트로 재초기화(list_del_init) — 이후
		 * blk_mq_free_request()가 이 필드를 다시 건드려도 안전하게
		 * 한다. */
		list_del_init(&rq->queuelist);
		/* [한국어] request 자체(및 스케줄러/드라이버 사설 데이터)를
		 * 완전히 해제. 이 시점에서 tag는 아직 안 잡혔거나 이미
		 * 반환된 상태라고 가정한다(그렇지 않으면 tag 누수). */
		blk_mq_free_request(rq);
	}
}

/*
 * For shared tag users, we track the number of currently active users
 * and attempt to provide a fair share of the tag depth for each of them.
 */
/*
 * [한국어]
 * hctx_may_queue - 이 hctx(SQ)가 지금 새로운 tag를 받아도 되는지(공정 분배) 검사
 *
 * @hctx: 검사 대상 하드웨어 큐(NULL 가능 — 그러면 항상 true).
 * @bt:   대상 tag 풀의 sbitmap_queue.
 * @return: true면 tag 할당을 계속 진행해도 됨, false면 이 hctx의 quota를
 *          이미 소진했으니 다른 tag 대신 대기해야 함.
 *
 * shared tag(BLK_MQ_F_TAG_QUEUE_SHARED) 환경, 즉 여러 하드웨어 큐(예:
 * NVMe의 여러 네임스페이스 또는 여러 hctx)가 같은 tag 풀을 나눠 쓸 때,
 * 활성 사용자 수(active_queues)를 기준으로 각 hctx가 tag 풀 전체를
 * 독차지하지 못하도록 공정한 상한(quota)을 계산한다.
 * 실행 컨텍스트: tag 할당 경로(프로세스 컨텍스트, blk_mq_get_tag() 내부).
 *
 * 호출 체인:
 *   blk_mq_get_tag() -> [hctx_may_queue] -> __blk_mq_active_requests()
 */
static inline bool hctx_may_queue(struct blk_mq_hw_ctx *hctx,
				  struct sbitmap_queue *bt)
{
	unsigned int depth, users;

	/* [한국어] hctx가 없거나(호출자가 특정 hctx 문맥 없이 호출) 공유
	 * tag 모드가 아니면, 이 hctx는 자기 tag 풀을 독점하므로 공정 분배
	 * 계산 자체가 필요 없다 — 무조건 허용. */
	if (!hctx || !(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED))
		return true;

	/*
	 * Don't try dividing an ant
	 */
	/* [한국어] tag 풀 depth가 1이면 나눌 것도 없다 — 유일한 tag를 어차피
	 * 순서대로 하나씩만 쓸 수 있으므로 공정 분배 계산이 무의미하다.
	 * (원본 주석의 은유: "개미 한 마리를 나눠 먹으려 하지 마라") */
	if (bt->sb.depth == 1)
		return true;

	if (blk_mq_is_shared_tags(hctx->flags)) {
		struct request_queue *q = hctx->queue;

		/* [한국어] tag_set 전체가 공유되는 경우: 그 request_queue가
		 * "hctx 활성" 상태로 아직 표시되지 않았다면(다른 큐들이 아직
		 * 활발히 경쟁하지 않는 상태) 제한 없이 허용한다. */
		if (!test_bit(QUEUE_FLAG_HCTX_ACTIVE, &q->queue_flags))
			return true;
	} else {
		/* [한국어] hctx 단위 공유인 경우: 이 hctx 자체가 아직
		 * "tag 활성" 상태로 표시되지 않았다면(blk_mq_tag_busy()가
		 * 아직 호출 안 됨, 즉 아직 이 hctx로 나간 request가 없음)
		 * 마찬가지로 제한 없이 허용한다. */
		if (!test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
			return true;
	}

	/* [한국어] READ_ONCE: 다른 CPU가 동시에 active_queues를 갱신할 수
	 * 있으므로, 컴파일러가 이 읽기를 최적화(캐싱/재정렬)하지 않도록
	 * 강제해 항상 최신값에 가까운 스냅샷을 얻는다. */
	users = READ_ONCE(hctx->tags->active_queues);
	/* [한국어] 활성 사용자가 0이면(막 활성화되어 아직 카운트가 반영되기
	 * 전 등의 과도기) 나눌 대상이 없으므로 허용. */
	if (!users)
		return true;

	/*
	 * Allow at least some tags
	 */
	/* [한국어] 공정 분배 공식: 전체 depth를 users로 올림 나눗셈한 값과
	 * 4 중 큰 쪽을 quota로 삼는다. 올림 나눗셈((depth + users - 1) /
	 * users)을 쓰는 이유는 나머지를 버리지 않고 각 사용자가 최소한
	 * ceil(depth/users)만큼은 받을 수 있게 하기 위함이며, 최소 4를
	 * 보장하는 이유는 사용자가 아주 많을 때 quota가 0이나 1처럼 너무
	 * 작아져 성능이 붕괴하는 것을 막기 위함이다. */
	depth = max((bt->sb.depth + users - 1) / users, 4U);
	/* [한국어] 이 hctx가 현재 갖고 있는 outstanding 명령 수가 quota
	 * 미만이면 새 tag 할당을 허용, quota에 도달했으면 거부(다른
	 * hctx에게 양보). */
	return __blk_mq_active_requests(hctx) < depth;
}

/*
 * [한국어] __blk_mq_run_dispatch_ops() / blk_mq_run_dispatch_ops() - dispatch
 * 임계구역을 RCU 또는 SRCU 읽기 락으로 감싸는 매크로 쌍.
 *
 * dispatch_ops로 전달된 코드 블록(주로 ops->queue_rq() 호출을 포함하는
 * dispatch 루틴)이 실행되는 동안, q->tag_set이나 hctx 자료구조가 다른
 * 스레드에 의해 해제(teardown)되지 않도록 보호한다.
 *   - BLK_MQ_F_BLOCKING 플래그가 있는 tag_set(드라이버의 queue_rq 콜백이
 *     스스로 sleep 할 수 있는 경우, 예: 일부 소프트웨어/네트워크 기반
 *     드라이버)은 SRCU(Sleepable RCU)를 사용한다 — 일반 RCU 임계구역
 *     안에서는 sleep이 금지되지만 SRCU는 이를 허용한다.
 *   - 그렇지 않은 일반 드라이버(NVMe 등, queue_rq가 atomic 컨텍스트에서도
 *     동작 가능)는 더 가벼운 rcu_read_lock()/rcu_read_unlock()으로 충분하다.
 *   - check_sleep 인자는 might_sleep_if()에 전달되어, SRCU 경로인데
 *     실제로 sleep이 금지된 컨텍스트에서 잘못 호출되면 CONFIG_DEBUG_ATOMIC_SLEEP
 *     빌드에서 경고를 낸다.
 *   - dispatch_ops는 함수가 아니라 매크로 인자로 전개되는 코드 블록이므로,
 *     그 안에서 break/continue/return 등을 함부로 쓰면 매크로가 만든
 *     do-while(0)/if-else 구조를 벗어나는 예기치 않은 제어 흐름이 될 수
 *     있어 주의가 필요하다(호출부에서 이를 준수).
 * 매크로 연속줄(백슬래시) 주의: 이 매크로는 여러 줄을 백슬래시로 이어
 * 하나의 문장으로 전개되므로, 각 줄 끝의 '\'는 반드시 물리적 줄의 마지막
 * 문자여야 한다 — 이 헤더에 주석을 추가할 때도 코드 라인 자체는 건드리지
 * 않고 지금 이 설명 블록처럼 매크로 정의 "앞"에서만 서술한다.
 *
 * 호출 체인 (blk-mq.c/blk-mq-sched.c 등의 사용처):
 *   blk_mq_run_hw_queue()/blk_mq_dispatch_rq_list() 등
 *     -> blk_mq_run_dispatch_ops(q, ...) -> __blk_mq_run_dispatch_ops(q, true, ...)
 *     -> (SRCU 또는 RCU 임계구역 안에서) dispatch_ops 전개 -> ops->queue_rq (드라이버 콜백)
 */
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
 * [한국어]
 * blk_mq_can_poll - 이 request_queue가 폴링(polling) I/O 모드를 지원하는지 검사
 *
 * @q: 대상 request_queue.
 * @return: true면 폴링 완료 경로(blk_mq_poll())를 쓸 수 있음.
 *
 * 두 조건을 모두 만족해야 한다:
 *   1) q->limits.features에 BLK_FEAT_POLL 비트가 서 있어야 한다 — 드라이버
 *      스스로 "나는 폴링 완료를 지원한다"고 선언한 것(NVMe라면 컨트롤러/
 *      드라이버가 인터럽트 없는 완료 회수를 구현했다는 뜻).
 *   2) tag_set->map[HCTX_TYPE_POLL].nr_queues가 0보다 커야 한다 — 실제로
 *      폴링 전용 하드웨어 큐(NVMe라면 폴링용 SQ/CQ 쌍)가 하나 이상
 *      초기화되어 있어야 한다는 뜻.
 * 이 둘 중 하나라도 없으면 blk_mq_get_hctx_type()이 REQ_POLLED를 봐도
 * 사실상 DEFAULT 큐로만 갈 수 있으므로, 상위(io_uring 등)에서 폴링 경로
 * 진입 자체를 이 함수로 미리 걸러낸다.
 * 실행 컨텍스트: 어디서든 호출 가능한 순수 조회 함수.
 *
 * 호출 체인:
 *   io_uring IORING_SETUP_IOPOLL 초기화/submit_bio 등 -> [blk_mq_can_poll]
 *     -> (true 시) blk_mq_poll() 경로 사용 허용
 */
static inline bool blk_mq_can_poll(struct request_queue *q)
{
	return (q->limits.features & BLK_FEAT_POLL) &&
		q->tag_set->map[HCTX_TYPE_POLL].nr_queues;
}

#endif
