/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] 블록 계층 IO 스케줄러(elevator)의 멀티큐(blk-mq) 연산 인터페이스와
 *              핵심 자료구조를 선언하는 헤더 파일 (elevator.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 mq-deadline·BFQ·kyber·"none" 등 모든 blk-mq IO 스케줄러 플러그인이
 * 공통으로 구현/사용해야 하는 인터페이스(struct elevator_mq_ops), 각 request_queue에
 * 부착되는 스케줄러 인스턴스(struct elevator_queue), 스케줄러 종류 자체를 표현하는
 * 서술자(struct elevator_type), sysfs로 노출되는 튜너블(struct elv_fs_entry), 그리고
 * 스케줄러 전용 blk-mq tag/CID 풀(struct elevator_tags)을 선언한다. 실제 정책 로직은
 * 여기 없다 — block/mq-deadline.c, block/bfq-iosched.c, block/kyber-iosched.c가 이
 * 헤더를 include하여 각자의 ops 테이블을 채우고, block/elevator.c가 공통 등록·병합·
 * sysfs 배관(plumbing)을 구현한다. 즉 이 헤더는 "정책과 매커니즘의 분리"에서
 * 매커니즘(스케줄러를 어떻게 꽂고 빼는지)과 정책 vtable의 규격만 정의하는 계약서다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * blk-mq 스택에서 elevator는 blk-mq 코어와 실제 드라이버(NVMe라면 nvme_queue_rq())
 * 사이, "요청 발행 이후 ~ SQ(Submission Queue) 진입 이전" 구간을 담당한다.
 *
 *   [응용] write(2)/read(2) → submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓  → elv_merge()/elv_bio_merge_ok(): 병합 가능한 기존 request 탐색
 *       ↓  → blk_mq_get_request() → ops.limit_depth(): tag/CID 할당 깊이 제한
 *       ↓  → ops.prepare_request(): request가 스케줄러에 편입되기 전 준비
 *       ↓  → ops.insert_requests(): 스케줄러 내부 큐(rb-tree/fifo/B-WF2Q+)에 삽입
 *       ↓
 *   [blk-mq dispatch] ops.has_work() → ops.dispatch_request(): 다음 request 선택
 *       ↓
 *   [드라이버] ->queue_rq() (NVMe: nvme_queue_rq() → SQ doorbell)
 *       ↓
 *   [완료 인터럽트] ops.completed_request() → ops.finish_request()
 *
 * "none"을 선택하면 이 모든 ops가 비어 있고 blk-mq가 bio를 거의 그대로 드라이버에
 * 전달한다(고성능 NVMe SSD의 기본값과 유사). 실행 컨텍스트는 대부분 bio를 제출한
 * 프로세스 컨텍스트(제출/삽입/병합)와 드라이버 인터럽트 컨텍스트 또는 softirq
 * (완료 콜백)로 나뉜다 — 아래 각 ops 필드 주석에 콜백별 컨텍스트를 명시한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 헤더가 참조하는 타입):
 *   - block/blk-mq.h: struct blk_mq_hw_ctx, blk_insert_t 등 blk-mq 코어 타입.
 *   - <linux/hashtable.h>: struct elevator_queue.hash(DECLARE_HASHTABLE)의 기반.
 * 구현(이 헤더의 ops를 채우는 쪽):
 *   - block/mq-deadline.c: struct elevator_type mq_deadline — dd_* 콜백들.
 *   - block/bfq-iosched.c: struct elevator_type iosched_bfq_mq — bfq_* 콜백들.
 *   - block/kyber-iosched.c: struct elevator_type kyber_sched — kyber_* 콜백들.
 * 매커니즘 제공(이 헤더의 선언을 정의):
 *   - block/elevator.c: elv_merge/elv_register/elevator_alloc 등 공통 로직,
 *     elv_rqhash_ 계열/elv_rb_ 계열 헬퍼, elv_iosched_show/store(sysfs 스케줄러 전환).
 *   - block/blk-mq-sched.c: blk_mq_init_sched/exit_sched — elevator_tags(tag 풀)
 *     할당/해제와 elevator_mq_ops 초기화 콜백(init_sched/init_hctx) 호출.
 *   - block/blk-mq.c: prepare_request/insert_requests/depth_updated/finish_request
 *     등 IO 경로 콜백을 실제로 호출하는 지점.
 *   - block/blk-ioc.c: init_icq/exit_icq — per-io_context 스케줄러 상태 관리.
 * 공유 자료구조: struct request_queue.elevator(elevator_queue*),
 *   struct blk_mq_hw_ctx.sched_tags/sched_data, struct io_cq(스케줄러별 서브클래싱
 *   — 예: bfq_io_cq가 io_cq를 embed).
 *
 * === 주요 함수/구조체 요약 ===
 * struct elevator_mq_ops   - 스케줄러가 구현하는 21개 콜백의 vtable (본 파일의 핵심).
 * struct elevator_type     - 스케줄러 "종류"의 정적 서술자; ops·이름·icq 캐시 보유.
 * struct elevator_queue    - request_queue 하나에 부착된 스케줄러 "인스턴스".
 * struct elevator_tags     - 스케줄러 전용 blk-mq tag(shadow CID) 풀.
 * struct elv_fs_entry      - /sys/block/<disk>/queue/iosched/ 튜너블 서술자.
 * elv_merge()/elv_merged_request() - bio-request 병합 판정과 병합 후 상태 갱신.
 * elv_register()/elv_unregister()  - 스케줄러 모듈을 전역 elv_list에 등록/해제.
 * elevator_alloc()         - elevator_queue 인스턴스를 할당하고 tag 자원을 연결.
 * elv_rqhash_*()/elv_rb_*()- 끝 섹터 해시(back-merge)와 LBA 기준 RB-tree(정렬/front-merge).
 */

#ifndef _ELEVATOR_H
#define _ELEVATOR_H	/* [한국어] 인클루드 가드. 바로 위 #ifndef 와 짝을 이뤄, 이 헤더가 한 번역 단위에
				 * 여러 번 포함돼도 내용이 한 번만 펼쳐지게 한다(중복 정의 오류 방지). */

#include <linux/percpu.h>
/* [한국어] per-CPU 변수 매크로(DEFINE_PER_CPU 등)를 제공 - io_cq/icq 캐시 등
 * 스케줄러가 CPU-로컬 통계나 캐시를 둘 때 사용할 수 있는 기반 인프라이다.
 * elevator.h 자체는 per-CPU 변수를 선언하지 않지만, 이 헤더를 포함하는
 * mq-deadline.c/bfq-iosched.c 등이 per-CPU 통계를 쓸 수 있도록 전이(transitively)
 * 포함시킨다. */
#include <linux/hashtable.h>
/* [한국어] DECLARE_HASHTABLE()/hash_add()/hash_for_each_possible_safe() 등
 * 고정 크기 해시테이블 매크로를 제공 - struct elevator_queue.hash 필드(끝 섹터
 * 기반 back-merge 후보 탐색용 해시)를 선언하는 데 필수적이다. */
#include "blk-mq.h"
/* [한국어] struct blk_mq_hw_ctx, struct request_queue, blk_insert_t 등 blk-mq
 * 코어 타입을 가져온다. elevator_mq_ops의 콜백 시그니처 대부분이 이 타입들을
 * 매개변수로 사용하므로 elevator.h는 blk-mq.h 없이는 컴파일될 수 없다. */

/*
 * [한국어] 전방 선언(forward declaration): 아래 세 타입은 이 헤더에서 포인터로만
 * 참조되고 멤버에 직접 접근하지 않으므로, 전체 정의를 include하지 않고 이름만
 * 알려준다. 이렇게 하면 elevator.h를 포함하는 파일들이 불필요한 헤더 의존성
 * (컴파일 시간 증가, 순환 include 위험)을 지지 않아도 된다.
 *   - struct io_cq: block/blk-ioc.h에 정의된 per-io_context 스케줄러 상태.
 *     BFQ의 bfq_io_cq처럼 스케줄러가 이를 embed하여 서브클래싱한다.
 *   - struct elevator_type: 본 파일 뒤쪽에서 실제로 정의된다(자기 참조 방지를
 *     위해 여기서 먼저 이름만 선언).
 *   - struct blk_mq_debugfs_attr: block/blk-mq-debugfs.h에 정의된 debugfs 속성
 *     서술자 - struct elevator_type의 queue_debugfs_attrs/hctx_debugfs_attrs가
 *     이 타입의 배열을 가리킨다. */
struct io_cq;
struct elevator_type;
struct blk_mq_debugfs_attr;

/*
 * Return values from elevator merger
 */
/*
 * [한국어] elv_merge()가 반환하는 병합 판정 결과. bio가 기존 request와 어떤
 * 방식으로 병합 가능한지를 나타내며, 호출자(blk_mq_submit_bio 경로)는 이 값에
 * 따라 blk_try_merge()/blk_attempt_req_merge()의 병합 방향을 결정한다.
 * 병합이 성공하면 별도 request를 새로 만들지 않아도 되므로, 최종적으로
 * 드라이버에 제출되는 명령(예: NVMe SQ 엔트리) 수와 태그/CID 소모가 줄어든다.
 */
enum elv_merge {
	ELEVATOR_NO_MERGE	= 0,
	/* [한국어] 병합 불가/불필요 판정.
	 * 설정자: elv_merge()가 nomerges 큐 플래그, bio_mergeable() 실패, 해시/
	 *   스케줄러 병합 후보를 모두 찾지 못했을 때 반환.
	 * 읽는 자: blk_mq_submit_bio() — 이 값을 받으면 새 struct request를 할당해
	 *   별도 명령으로 드라이버에 제출한다.
	 * 값 범위: enum elv_merge 중 유일하게 "병합 실패"를 뜻하는 값(0).
	 * 동기화: 순수 값 반환이므로 별도 동기화 불필요. */

	ELEVATOR_FRONT_MERGE	= 1,
	/* [한국어] 새 bio가 기존 request의 "앞쪽"에 붙는 병합(선행 섹터에 이어짐).
	 * 설정자: 스케줄러별 request_merge 콜백(예: bfq_request_merge, dd_request_merge)이
	 *   bio의 끝 섹터가 기존 request의 시작 섹터와 정확히 이어질 때 반환.
	 * 읽는 자: elv_merge()의 호출자가 blk_try_merge()에 이 방향을 전달해 request의
	 *   시작 섹터/데이터 길이를 확장한다.
	 * 값 범위: front-merge를 뜻하는 고정값 1.
	 * 동기화: 없음(값 자체는 상태를 갖지 않음; 병합 대상 request의 락은 호출자 책임). */

	ELEVATOR_BACK_MERGE	= 2,
	/* [한국어] 새 bio가 기존 request의 "뒤쪽"에 붙는 병합(가장 흔한 케이스).
	 * 설정자: elv_merge()가 elv_rqhash_find()로 끝 섹터가 일치하는 request를
	 *   찾았을 때, 또는 스케줄러 request_merge 콜백이 반환.
	 * 읽는 자: 호출자가 blk_try_merge()로 병합을 완료하고 elv_merged_request()를
	 *   호출해 스케줄러 상태(해시 재배치 등)를 갱신한다.
	 * 값 범위: back-merge를 뜻하는 고정값 2.
	 * 동기화: 없음. */

	ELEVATOR_DISCARD_MERGE	= 3,
	/* [한국어] discard(TRIM/Deallocate)/write-zeroes 요청 전용 병합.
	 * 설정자: elv_merge()에서 blk_discard_mergable()이 true인 back-merge 후보를
	 *   찾았을 때 반환. 일반 데이터 병합과 달리 discard 범위(구간) 병합이다.
	 * 읽는 자: 호출자가 discard 범위를 확장하는 병합 경로로 분기.
	 * 값 범위: discard 전용 병합을 뜻하는 고정값 3.
	 * 동기화: 없음. */
};

struct blk_mq_alloc_data;
/* [한국어] request/tag 할당 매개변수(block/blk-mq.h에 정의)를 가리키는 전방
 * 선언. elevator_mq_ops.limit_depth의 두 번째 인자 타입으로만 쓰이므로 포인터
 * 선언에는 완전한 정의가 필요 없다. */
struct blk_mq_hw_ctx;
/* [한국어] 하드웨어 디스패치 큐(하나의 hctx가 보통 NVMe의 SQ/CQ 한 쌍에 대응)를
 * 가리키는 전방 선언. elevator_mq_ops의 init_hctx/exit_hctx/insert_requests/
 * dispatch_request/has_work 등 다수 콜백이 이 타입의 포인터를 매개변수로 받는다. */

/*
 * [한국어] elevator_tags: 스케줄러 전용 blk-mq tag(그림자 태그, shadow tag) 집합.
 * blk-mq는 드라이버 제출용 tag(hctx->tags)와 별개로, 스케줄러가 내부적으로
 * request를 추적하기 위한 "그림자" tag 풀을 가질 수 있다. NVMe 맥락에서는 이
 * shadow tag가 CID(Command ID) 발급의 상한을 결정하며, 스케줄러가 관리할 수
 * 있는 in-flight request 수를 제한한다. block/blk-mq-sched.c의
 * blk_mq_alloc_sched_tags()/blk_mq_free_sched_tags()가 이 구조체를 생성/해제한다.
 */
struct elevator_tags {
	/* num. of hardware queues for which tags are allocated */
	unsigned int nr_hw_queues;
	/* [한국어] 이 elevator_tags가 관리하는 하드웨어 큐(hctx) 개수.
	 * 설정자: blk_mq_alloc_sched_tags()가 blk_mq_tag_set.nr_hw_queues 값으로 설정.
	 * 읽는 자: blk_mq_free_sched_tags()가 per-hctx 모드에서 tags[] 해제 루프의
	 *   상한으로 사용(for (i = 0; i < et->nr_hw_queues; i++)).
	 * 값 범위: 1 이상. shared-tags 모드에서도 이 값은 실제 hctx 수를 담고 있지만
	 *   tags[] 배열 자체는 인덱스 0 하나만 유효하게 채워진다(아래 tags[] 참고).
	 * 동기화: nr_hw_queues 변경(blk_mq_update_nr_hw_queues)은 큐 freeze 상태에서만
	 *   일어나므로 별도 락 없이 안전. */

	/* depth used while allocating tags */
	unsigned int nr_requests;
	/* [한국어] 이 tag 풀이 동시에 관리할 수 있는 최대 request(=태그) 개수.
	 * 설정자: blk_mq_alloc_sched_tags()가 호출자가 넘긴 nr_requests(대개
	 *   q->tag_set->queue_depth 또는 사용자가 조정한 nr_requests)로 설정.
	 * 읽는 자: blk_mq_init_sched()가 q->nr_requests = et->nr_requests로 복사해
	 *   request_queue의 depth를 이 값에 맞추고, shared-tags 모드에서는
	 *   blk_mq_tag_update_sched_shared_tags()가 sbitmap 크기 조정에 사용.
	 * 값 범위: 보통 tag_set->queue_depth 이하 — 하드웨어(NVMe controller MQES 등)
	 *   가 지원하는 큐 깊이를 넘을 수 없다.
	 * 동기화: 스케줄러 전환/nr_hw_queues 변경 시에만 재계산되며 그 경로는
	 *   q->elevator_lock + queue freeze로 보호된다. */

	/* shared tag is stored at index 0 */
	struct blk_mq_tags *tags[];
	/* [한국어] 실제 tag 비트맵(struct blk_mq_tags)들을 담는 가변 길이 배열
	 * (flexible array member) - elevator_tags 구조체 뒤에 이어 붙여 kmalloc된다.
	 * 두 가지 모드로 쓰인다:
	 *   1) shared-tags 모드(blk_mq_is_shared_tags(flags)==true): 모든 hctx가
	 *      tags[0] 하나를 공유한다(nr_tags=1로 할당). q->sched_shared_tags와
	 *      모든 hctx->sched_tags가 동일하게 tags[0]을 가리킨다.
	 *   2) per-hctx 모드: hctx마다 독립된 tag 풀을 가지며 tags[i]가 i번째
	 *      hctx(대략 NVMe qid i)에 대응한다(nr_tags=nr_hw_queues로 할당).
	 * 설정자: blk_mq_alloc_sched_tags()가 blk_mq_alloc_map_and_rqs()로 채움.
	 * 읽는 자: blk_mq_init_sched()가 hctx->sched_tags에 연결; dispatch 시
	 *   스케줄러가 이 풀에서 shadow tag를 얻어 request를 추적.
	 * 값 범위: 각 원소는 유효한 blk_mq_tags 포인터 또는(shared 모드에서 사용되지
	 *   않는 인덱스는) 참조되지 않음.
	 * 동기화: 풀 자체의 태그 획득/반환은 blk_mq_tags 내부 sbitmap이 원자적으로
	 *   처리; tags[] 배열 자체의 구성은 큐 freeze 상태에서만 변경. */
};

/*
 * [한국어] elevator_resources: elevator_alloc()에 전달되는 "스케줄러 인스턴스
 * 생성 재료" 묶음. blk_mq_alloc_sched_res()가 미리 tag 풀(et)과 스케줄러
 * 사설 데이터(data)를 준비해 두면, elevator_alloc()이 이를 struct
 * elevator_queue에 그대로 옮겨 담는다. elv_change_ctx.res로도 재사용되어
 * 스케줄러 전환 중 새/이전 자원을 임시로 보관하는 데도 쓰인다.
 */
struct elevator_resources {
	/* holds elevator data */
	void *data;
	/* [한국어] 스케줄러 구현체의 사설(private) 상태 포인터.
	 * 설정자: 각 스케줄러의 alloc_sched_data 콜백(예: kyber_alloc_sched_data)이
	 *   있으면 그 반환값, 없으면 ops.init_sched 내부에서 별도로 할당한 구조체
	 *   (mq-deadline의 struct deadline_data, BFQ의 struct bfq_data)로 채워짐.
	 * 읽는 자: elevator_alloc()이 struct elevator_queue.elevator_data로 복사;
	 *   이후 모든 ops 콜백이 e->elevator_data(또는 q->elevator->elevator_data)로
	 *   접근하는 실체가 바로 이 포인터.
	 * 값 범위: 스케줄러마다 가리키는 실제 타입이 다른 void* — 타입 안전성은
	 *   전적으로 각 스케줄러 구현이 책임진다.
	 * 동기화: 스케줄러별 내부 락(BFQ의 bfqd->lock, mq-deadline의 dd->lock 등)으로
	 *   보호되며 이 구조체 자체는 단순 포인터 컨테이너라 락이 없다. */

	/* holds elevator tags */
	struct elevator_tags *et;
	/* [한국어] 이 스케줄러 인스턴스가 사용할 shadow tag 풀.
	 * 설정자: blk_mq_alloc_sched_res()가 blk_mq_alloc_sched_tags()의 결과로 채움.
	 * 읽는 자: elevator_alloc()이 struct elevator_queue.et로 복사; 이후
	 *   blk_mq_init_sched()가 hctx->sched_tags/q->sched_shared_tags 연결에 사용.
	 * 값 범위: 유효한 elevator_tags 포인터(위 struct elevator_tags 참고).
	 * 동기화: 전환 중에는 q->elevator_lock + queue freeze로 단일 소유자만 접근. */
};

/* Holding context data for changing elevator */
/*
 * [한국어] elv_change_ctx: sysfs("echo bfq > .../scheduler") 또는 nr_hw_queues
 * 변경("elv_update_nr_hw_queues") 같은 "elevator 교체" 트랜잭션 하나의 상태를
 * 스택 변수로 들고 다니는 컨텍스트. elevator_change() → elevator_switch() →
 * elevator_change_done()으로 이어지는 여러 함수가 이 구조체 하나를 계속
 * 참조/갱신하며 전환 과정을 진행한다.
 */
struct elv_change_ctx {
	const char *name;
	/* [한국어] 전환 목표 스케줄러의 이름 문자열("none", "mq-deadline", "bfq" 등).
	 * 설정자: elv_iosched_store()가 sysfs 입력을 strstrip()한 결과, 또는
	 *   elevator_set_default/elevator_set_none이 고정 문자열로 설정.
	 * 읽는 자: elevator_switch()가 strncmp(ctx->name, "none", 4)로 "none"
	 *   여부를 판단하고, elevator_find_get(ctx->name)로 elv_list에서 대응하는
	 *   elevator_type을 조회.
	 * 값 범위: ELV_NAME_MAX(16바이트) 이내의 NUL 종단 문자열.
	 * 동기화: ctx는 호출 스레드의 스택/지역 변수이므로 별도 동기화 불필요 —
	 *   다만 name이 가리키는 버퍼의 수명은 ctx 사용 구간 내내 유지돼야 한다. */
	bool no_uevent;
	/* [한국어] 스케줄러 교체 완료 시 KOBJ_ADD uevent를 udev 등에 통지할지 여부.
	 * 설정자: elevator_set_default()가 true로 설정(부팅 중 초기 기본값 적용이라
	 *   사용자 공간에 알릴 필요가 없음); sysfs를 통한 명시적 변경은 기본값 false.
	 * 읽는 자: elevator_change_done() → elv_register_queue(q, ctx->new,
	 *   !ctx->no_uevent) — true면 uevent 발행을 건너뜀.
	 * 값 범위: true/false.
	 * 동기화: 불필요(단순 플래그 값). */

	/* for unregistering old elevator */
	struct elevator_queue *old;
	/* [한국어] elevator_switch()가 교체 직전 떼어낸 "이전" 스케줄러 인스턴스.
	 * 설정자: elevator_switch()가 q->elevator가 있었다면 ctx->old = q->elevator
	 *   로 저장한 뒤 elevator_exit()으로 내부 자원을 정리(이 시점에 q->elevator는
	 *   NULL이 됨).
	 * 읽는 자: elevator_change_done()이 elv_unregister_queue()로 sysfs 노드를
	 *   지우고 blk_mq_free_sched_res()로 tag/data를 마저 해제한 뒤
	 *   kobject_put()으로 최종 kfree를 유도.
	 * 값 범위: 유효한 elevator_queue 포인터 또는 NULL(원래 elevator가 없었던 경우).
	 * 동기화: q->elevator_lock을 쥔 상태에서만 설정/해제되어 race 없음. */
	/* for registering new elevator */
	struct elevator_queue *new;
	/* [한국어] elevator_switch()가 새로 부착한 스케줄러 인스턴스(성공 시).
	 * 설정자: elevator_switch()가 blk_mq_init_sched() 성공 후
	 *   ctx->new = q->elevator로 저장.
	 * 읽는 자: elevator_change_done()이 elv_register_queue()로 sysfs 노드를
	 *   생성; 실패하면 elv_exit_and_release()로 롤백(rollback)한다.
	 * 값 범위: 유효한 elevator_queue 포인터 또는 NULL("none"으로 전환했거나
	 *   전환 자체가 실패한 경우).
	 * 동기화: old와 동일하게 elevator_lock/큐 freeze로 보호. */
	/* store elevator type */
	struct elevator_type *type;
	/* [한국어] 전환 목표 스케줄러의 elevator_type(정적 서술자) 참조.
	 * 설정자: elv_iosched_store()/elevator_set_default()가
	 *   elevator_find_get()으로 elv_list에서 조회해 참조 카운트를 올린 채 저장.
	 * 읽는 자: elevator_change()가 blk_mq_alloc_sched_res(q, ctx->type, ...)로
	 *   자원을 사전 할당할 때, elv_update_nr_hw_queues()가 elevator_switch() 전
	 *   ctx->type 존재 여부로 재부착 대상인지 판단할 때 사용.
	 * 값 범위: 유효한 elevator_type 포인터 또는 NULL("none" 전환이거나 아직
	 *   조회 전).
	 * 동기화: elevator_tryget()/elevator_put()으로 모듈 참조 카운트를 관리해
	 *   전환 도중 모듈이 언로드되지 않도록 보장. */
	/* store elevator resources */
	struct elevator_resources res;
	/* [한국어] 전환에 사용할 tag 풀(et)과 스케줄러 사설 데이터(data)를 미리
	 * 담아두는 임베디드 구조체(포인터가 아니라 값으로 포함).
	 * 설정자: elevator_change()가 blk_mq_alloc_sched_res()로 채운 뒤
	 *   elevator_switch()에 그대로 전달; blk_mq_init_sched()가 이 값을 읽어
	 *   elevator_alloc()에 넘긴다.
	 * 읽는 자: 전환 실패 시 blk_mq_free_sched_res(&ctx->res, ...)로 미사용
	 *   자원을 되돌리는 경로에서도 참조.
	 * 값 범위: struct elevator_resources 값 자체(위 정의 참고) — "none"으로
	 *   전환할 때는 애초에 할당되지 않아 {0}으로 유지.
	 * 동기화: ctx가 스택 변수이므로 이 필드도 함수 호출 체인을 벗어나지 않음. */
};

/*
 * [한국어] elevator_mq_ops: blk-mq IO 스케줄러가 구현해야 하는 21개 콜백의
 * vtable. block/blk-mq.c, block/blk-mq-sched.c(.h), block/blk-ioc.c,
 * block/elevator.c가 각 시점에 이 콜백들을 호출한다. finish_request와
 * insert_requests/dispatch_request는 elv_register()에서 WARN_ON_ONCE로
 * 존재를 강제하는 "필수" 콜백이고, init_sched 역시 blk_mq_init_sched()가
 * NULL 체크 없이 직접 호출하므로 사실상 필수다. 나머지는 모두
 * `if (ops->xxx) ops->xxx(...)` 형태로 존재 여부를 확인 후 호출되는
 * "선택적" 콜백이며, 실제로 mq-deadline/BFQ/kyber 세 스케줄러가 각기 다른
 * 부분집합만 구현한다(아래 각 필드 주석에 실제 구현 함수명을 명시).
 */
struct elevator_mq_ops {

	int (*init_sched)(struct request_queue *, struct elevator_queue *);
	/* [한국어] 스케줄러 "전역" 상태를 초기화하는 필수(사실상) 콜백.
	 * 호출 시점/자: block/blk-mq-sched.c의 blk_mq_init_sched()가 elevator_alloc()
	 *   과 tag 연결을 마친 뒤 NULL 체크 없이 e->ops.init_sched(q, eq)로 호출.
	 * 실행 컨텍스트: 스케줄러 전환 트랜잭션 안(프로세스 컨텍스트), 큐가 quiesce된
	 *   상태 - 진행 중인 dispatch와 경쟁하지 않는다.
	 * 구현: mq-deadline dd_init_sched(우선순위별 fifo/rb-tree, 통계 초기화),
	 *   BFQ bfq_init_queue(struct bfq_data 전체 및 cgroup 연동 초기화),
	 *   kyber kyber_init_sched(도메인별 sbitmap/타이머 초기화).
	 * 반환/에러: 음수 errno 시 blk_mq_init_sched()가 실패 처리하여 elevator를
	 *   부착하지 않고 롤백(elevator_release 경로로 kobject_put). */
	void (*exit_sched)(struct elevator_queue *);
	/* [한국어] init_sched의 반대 - 스케줄러 전역 상태를 해제.
	 * 호출 시점/자: block/blk-mq-sched.c blk_mq_exit_sched()가
	 *   `if (e->type->ops.exit_sched) e->type->ops.exit_sched(e)`로 호출.
	 * 실행 컨텍스트: elevator_exit() → 큐 freeze 상태, sysfs_lock을 쥔 채 호출되어
	 *   동시 파라미터 변경(elv_attr_store)과 경쟁하지 않는다.
	 * 구현: mq-deadline dd_exit_sched(fifo_list가 비어있는지 확인 후 free),
	 *   BFQ bfq_exit_queue(bfqq/bfqg 트리 전체 해제, cgroup 링크 해제),
	 *   kyber kyber_exit_sched(타이머 정지, domain_tokens 등 회수는
	 *   free_sched_data가 별도 담당).
	 * 에러: 반환값 없음(void) - 실패해도 자원 누수 외에 별도 에러 경로 없음. */
	int (*init_hctx)(struct blk_mq_hw_ctx *, unsigned int);
	/* [한국어] hctx(하드웨어 디스패치 큐, NVMe라면 SQ/CQ 한 쌍) 단위 초기화 - 선택적.
	 * 호출 시점/자: blk_mq_init_sched()가 각 hctx에 대해
	 *   `if (e->ops.init_hctx) e->ops.init_hctx(hctx, i)`로 호출(i = hctx 인덱스).
	 * 실행 컨텍스트: init_sched 이후, 여전히 전환 트랜잭션 내부.
	 * 구현: kyber만 구현(kyber_init_hctx) - hctx마다 도메인별(READ/WRITE/기타)
	 *   sbitmap과 dispatch cursor를 갖기 때문. mq-deadline/BFQ는 전역 상태만
	 *   쓰므로 이 콜백이 없다(NULL).
	 * 에러: 실패 시 blk_mq_init_sched()가 이미 초기화된 이전 hctx들을
	 *   blk_mq_exit_sched()로 정리하고 kobject_put() 후 에러 반환. */
	void (*exit_hctx)(struct blk_mq_hw_ctx *, unsigned int);
	/* [한국어] init_hctx의 반대 - hctx별 스케줄러 컨텍스트 해제 - 선택적.
	 * 호출 시점/자: blk_mq_exit_sched()가
	 *   `if (e->type->ops.exit_hctx && hctx->sched_data) e->type->ops.exit_hctx(hctx, i)`
	 *   로 호출(sched_data가 이미 NULL이면 init_hctx가 실패했던 hctx이므로 skip).
	 * 실행 컨텍스트: elevator_exit() 경로, 큐 freeze 상태.
	 * 구현: kyber만 구현(kyber_exit_hctx).
	 * 에러: void 반환 - 실패 경로 없음. */
	void (*depth_updated)(struct request_queue *);
	/* [한국어] 큐 depth(q->nr_requests) 변경 시 스케줄러의 내부 한도를 재계산 - 선택적.
	 * 호출 시점/자: block/blk-mq.c(약 8682번째 줄 부근)가
	 *   `if (q->elevator && q->elevator->type->ops.depth_updated)
	 *    q->elevator->type->ops.depth_updated(q)`로 호출 - 사용자가 nr_requests
	 *   sysfs 파일을 조정하거나 blk_mq_init_sched()가 초기 depth를 반영할 때.
	 * 실행 컨텍스트: sysfs write 프로세스 컨텍스트, 큐 freeze 상태에서 호출.
	 * 구현: mq-deadline dd_depth_updated(blk_mq_set_min_shallow_depth로
	 *   async_depth 하한 갱신), BFQ bfq_depth_updated(sync/async depth 테이블
	 *   재계산), kyber kyber_depth_updated(async_depth 재설정).
	 * 에러: void - 실패 경로 없음. */
	void *(*alloc_sched_data)(struct request_queue *);
	/* [한국어] 스케줄러 사설 데이터를 elevator_resources.data와 별개 경로로
	 * 미리 할당하는 선택적 콜백(주로 nr_hw_queues 재계산 시 재사용).
	 * 호출 시점/자: block/blk-mq-sched.h의 인라인 헬퍼가
	 *   `if (e && e->ops.alloc_sched_data) sched_data = e->ops.alloc_sched_data(q)`
	 *   형태로 호출 - blk_mq_alloc_sched_res_batch() 등에서 여러 큐/ctx에 대해
	 *   일괄적으로 사설 데이터를 준비할 때 쓰인다.
	 * 실행 컨텍스트: 스케줄러 자원 사전 할당 단계(전환/hw_queue 변경 준비 중).
	 * 구현: kyber만 구현(kyber_alloc_sched_data) - struct kyber_queue_data(kqd)를
	 *   할당. mq-deadline/BFQ는 init_sched 안에서 직접 kzalloc하므로 이 콜백이
	 *   없다.
	 * 반환/에러: NULL이면 호출자가 -ENOMEM으로 처리. */
	void (*free_sched_data)(void *);
	/* [한국어] alloc_sched_data로 만든 데이터를 해제하는 짝 콜백 - 선택적.
	 * 호출 시점/자: blk-mq-sched.h 인라인 헬퍼가
	 *   `if (e && e->ops.free_sched_data) e->ops.free_sched_data(data)`로 호출.
	 * 실행 컨텍스트: 전환 실패 롤백 또는 큐 해제 경로.
	 * 구현: kyber만 구현(kyber_free_sched_data) - kqd, cpu_latency, domain
	 *   token 배열 등을 kfree.
	 * 에러: void - 실패 경로 없음. */

	bool (*allow_merge)(struct request_queue *, struct request *, struct bio *);
	/* [한국어] 이미 정렬/병합 후보로 찾아낸 (request, bio) 쌍을 스케줄러
	 * "정책" 관점에서 병합해도 되는지 최종 승인하는 선택적 콜백.
	 * 호출 시점/자: block/elevator.c의 elv_iosched_allow_bio_merge()가
	 *   `if (e->type->ops.allow_merge) return e->type->ops.allow_merge(q, rq, bio)`
	 *   로 호출 - elv_bio_merge_ok()가 blk_rq_merge_ok()(하드웨어/방향 제약)
	 *   통과 후 이 콜백으로 정책 제약을 재확인.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트, request_queue 락 없이 호출될 수
	 *   있어 구현체가 자체 락(BFQ의 bfqd->lock 등)으로 내부 상태를 보호해야 함.
	 * 구현: BFQ만 구현(bfq_allow_bio_merge) - bio를 제출한 프로세스의 bfq_queue와
	 *   request 소유 bfq_queue가 cgroup/우선순위 관점에서 병합 가능한지 판정.
	 *   mq-deadline/kyber는 이 콜백이 없어 항상 허용(true)으로 취급.
	 * 반환: true = 병합 허용, false = 병합 거부(별도 request 유지). */
	bool (*bio_merge)(struct request_queue *, struct bio *, unsigned int);
	/* [한국어] bio가 request로 변환되기 "전" 단계에서 스케줄러 내부 자료구조를
	 * 이용해 병합 후보를 직접 탐색/시도하는 선택적 콜백.
	 * 호출 시점/자: block/blk-mq-sched.c __blk_mq_sched_bio_merge()가
	 *   `if (e && e->type->ops.bio_merge) ret = e->type->ops.bio_merge(q, bio, nr_segs)`
	 *   로 호출 - elevator가 있을 때는 이 콜백이 blk_bio_list_merge()(elevator
	 *   없을 때 기본 경로)를 대체한다.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트, hctx ctx 락 보유 상태로 호출.
	 * 구현: mq-deadline dd_bio_merge(우선순위별 spinlock을 쥐고 rb-tree/fifo에서
	 *   병합 시도), BFQ bfq_bio_merge(현재 프로세스의 bfq_queue 내부 트리에서
	 *   탐색), kyber kyber_bio_merge(현재 ctx의 rq_list에서 병합 시도).
	 * 반환: true면 병합 성공 - 호출자는 새 request를 만들지 않아도 됨. */
	int (*request_merge)(struct request_queue *q, struct request **, struct bio *);
	/* [한국어] elv_merge()가 끝 섹터 해시(back-merge)로 후보를 못 찾았을 때
	 * 마지막으로 위임하는 스케줄러별 병합 판정(주로 front-merge 탐색) - 선택적.
	 * 호출 시점/자: block/elevator.c elv_merge()가
	 *   `if (e->type->ops.request_merge) return e->type->ops.request_merge(q, req, bio)`
	 *   로 호출 - 해시 실패 시 최후 폴백(fallback) 경로.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
	 * 구현: mq-deadline dd_request_merge(bio 시작 섹터로 elv_rb_find() 호출해
	 *   front-merge 후보 탐색), BFQ bfq_request_merge(동일하게 rb-tree 탐색).
	 *   kyber는 request_merge가 없다(bio_merge만으로 충분하다고 판단).
	 * 반환: enum elv_merge 값(ELEVATOR_FRONT_MERGE 등) - *req에 병합 대상 저장. */
	void (*request_merged)(struct request_queue *, struct request *, enum elv_merge);
	/* [한국어] bio 하나가 기존 request에 병합된 "직후" 스케줄러 내부 상태
	 * (정렬 위치, 통계 등)를 갱신하는 선택적 후처리 콜백.
	 * 호출 시점/자: block/elevator.c elv_merged_request()가
	 *   `if (e->type->ops.request_merged) e->type->ops.request_merged(q, rq, type)`
	 *   로 호출 - blk_try_merge()로 실제 병합을 마친 직후.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
	 * 구현: mq-deadline dd_request_merged(front-merge였다면 rb-tree에서 위치
	 *   재조정), BFQ bfq_request_merged(bfq_queue 내부 rb-tree 위치 재조정).
	 *   kyber는 구현하지 않음.
	 * 에러: void - 실패 경로 없음. */
	void (*requests_merged)(struct request_queue *, struct request *, struct request *);
	/* [한국어] 두 개의 "완전한" request가 하나로 합쳐졌을 때(elv_attempt_insert_merge
	 * 등으로 서로 다른 두 request가 결합) 스케줄러 내부 연결을 갱신하는 선택적 콜백.
	 * 호출 시점/자: block/elevator.c elv_merge_requests()가
	 *   `if (e->type->ops.requests_merged) e->type->ops.requests_merged(q, rq, next)`
	 *   로 호출 - rq가 next를 흡수한 직후(next는 곧 free됨).
	 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
	 * 구현: mq-deadline dd_merged_requests(next의 fifo_time을 rq로 승계),
	 *   BFQ bfq_requests_merged(두 bfq_queue 간 병합/협력 관계 갱신).
	 *   kyber는 구현하지 않음. */
	void (*limit_depth)(blk_opf_t, struct blk_mq_alloc_data *);
	/* [한국어] tag(=CID) 할당 "전" 단계에서 요청 종류별 shallow depth(동시
	 * 확보 가능한 태그 수 상한)를 제한하는 선택적 콜백. 비동기/쓰기 트래픽이
	 * 동기 읽기용 태그를 고갈시키지 않도록 하는 QoS 성격의 관문이다.
	 * 호출 시점/자: block/blk-mq.c blk_mq_limit_depth()가
	 *   `if (ops->limit_depth) ops->limit_depth(data->cmd_flags, data)`로 호출 -
	 *   blk_mq_get_new_requests() → 태그 할당 직전 지점.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트, sbitmap 태그 획득 이전이므로
	 *   블로킹 없이 빠르게 값만 계산해야 한다.
	 * 구현: mq-deadline dd_limit_depth/kyber kyber_limit_depth(동기 읽기가
	 *   아니면 q->async_depth로 shallow_depth 제한), BFQ bfq_limit_depth
	 *   (weight-raising 큐 존재 여부 × sync 여부 조합의 사전 계산 테이블에서
	 *   상한을 조회, 동기 읽기는 무제한).
	 * 데이터: data->shallow_depth(출력)에 상한을 써넣는다. */
	void (*prepare_request)(struct request *);
	/* [한국어] request가 스케줄러 내부 자료구조에 삽입되기 "직전" 초기화를
	 * 수행하는 선택적 콜백 - 스케줄러별 request-embedded 상태(fifo_time,
	 * bfq_queue 연결 등)를 준비한다.
	 * 호출 시점/자: block/blk-mq.c가
	 *   `if (e->type->ops.prepare_request) e->type->ops.prepare_request(rq)`로 호출
	 *   - blk_mq_get_request()가 request를 초기화한 직후.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
	 * 구현: mq-deadline dd_prepare_request(RQF_ELVPRIV 설정 정도의 최소 작업),
	 *   BFQ bfq_prepare_request(bfq_queue 연결은 아직 안 하고 표시만),
	 *   kyber kyber_prepare_request(domain token을 -1로 초기화해 "아직 미획득"
	 *   상태 표시). */
	void (*finish_request)(struct request *);
	/* [한국어] request가 "완전히" 끝날 때(완료 처리 후 free 직전) 스케줄러
	 * 상태를 정리하는 필수(elv_register가 WARN_ON_ONCE로 강제) 콜백.
	 * 호출 시점/자: block/blk-mq.c가
	 *   `q->elevator->type->ops.finish_request(rq)`를 NULL 체크 없이 호출 -
	 *   request 완료/폐기 경로(__blk_mq_free_request 계열).
	 * 실행 컨텍스트: 완료 인터럽트에서 이어지는 softirq 또는 프로세스 컨텍스트
	 *   (완료 처리 지점에 따라 다름) - reentrant 가능성을 고려해 구현.
	 * 구현: mq-deadline dd_finish_request(우선순위 통계 갱신),
	 *   BFQ bfq_finish_request(bfq_queue 참조 해제, entity 상태 정리),
	 *   kyber kyber_finish_request(domain token을 domain_tokens에 반환 -
	 *   requeue_request와 동일 함수를 공유). */
	void (*insert_requests)(struct blk_mq_hw_ctx *hctx, struct list_head *list,
			blk_insert_t flags);
	/* [한국어] 완성된 request 목록을 hctx(디스패치 큐)에 삽입하는 필수
	 * (elv_register가 WARN_ON_ONCE로 강제) 콜백 - 스케줄러 내부 큐(rb-tree/
	 * fifo/B-WF2Q+ 등)에 request들을 편입시킨다.
	 * 호출 시점/자: block/blk-mq.c가
	 *   `q->elevator->type->ops.insert_requests(hctx, &list, flags)`를 호출 -
	 *   blk_mq_flush_plug_list()(plug 해제) 또는 blk_mq_insert_request() 등
	 *   request가 최종적으로 디스패치 큐에 들어갈 때.
	 * @flags: blk_insert_t(BLK_MQ_INSERT_AT_HEAD 등) - requeue/timeout 후
	 *   우선순위 있게 앞쪽에 넣어야 하는지 등을 지시.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트 또는 requeue 워커.
	 * 구현: mq-deadline dd_insert_requests, BFQ bfq_insert_requests,
	 *   kyber kyber_insert_requests - 각자의 정렬/우선순위 자료구조에 삽입. */
	struct request *(*dispatch_request)(struct blk_mq_hw_ctx *);
	/* [한국어] 이 hctx에서 드라이버로 내보낼 "다음 request 하나"를 선택하는
	 * 필수(elv_register가 WARN_ON_ONCE로 강제) 콜백 - 스케줄링 알고리즘의
	 * 핵심 결정 지점.
	 * 호출 시점/자: block/blk-mq-sched.c __blk_mq_do_dispatch_sched()가
	 *   `rq = e->type->ops.dispatch_request(hctx)`로 반복 호출 - 드라이버
	 *   ->queue_rq()에 넘길 request를 하나씩 뽑아낸다.
	 * 실행 컨텍스트: kblockd 워크큐(softirq류) 또는 직접 디스패치 경로 -
	 *   드라이버 제출 직전이므로 지연시간에 민감하다.
	 * 구현: mq-deadline dd_dispatch_request(read/write 배치 및 FIFO expire
	 *   시각 기준 선택), BFQ bfq_dispatch_request(B-WF2Q+ 알고리즘으로 다음
	 *   서비스할 bfq_queue와 request 선택), kyber kyber_dispatch_request
	 *   (도메인별 토큰이 있는 request 우선 선택).
	 * 반환: 선택된 request 또는 더 이상 보낼 것이 없으면 NULL. */
	bool (*has_work)(struct blk_mq_hw_ctx *);
	/* [한국어] 이 hctx에 아직 디스패치할 작업이 남아있는지 빠르게 확인하는
	 * 선택적 콜백 - true를 반환하지 않으면 blk-mq가 굳이 dispatch_request를
	 * 호출하지 않고 idle 상태로 남을 수 있다(불필요한 폴링 방지).
	 * 호출 시점/자: block/blk-mq-sched.c __blk_mq_do_dispatch_sched()가
	 *   `if (e->type->ops.has_work && !e->type->ops.has_work(hctx)) break`
	 *   형태로 디스패치 루프의 종료 조건에 사용.
	 * 실행 컨텍스트: dispatch_request와 동일한 컨텍스트.
	 * 구현: mq-deadline dd_has_work, BFQ bfq_has_work, kyber kyber_has_work -
	 *   각자 내부 큐(rb-tree/fifo/도메인별 rq_list)가 비어있는지 확인.
	 * 반환: true = 디스패치할 작업 있음, false = 없음(idle 진입 가능). */
	void (*completed_request)(struct request *, u64);
	/* [한국어] request가 드라이버로부터 완료 통지를 받았을 때(하지만 아직
	 * finish_request 전) 지연시간 등 통계를 기록하는 선택적 콜백.
	 * 호출 시점/자: block/blk-mq-sched.h 인라인 헬퍼가
	 *   `if (e->type->ops.completed_request) e->type->ops.completed_request(rq, now)`
	 *   로 호출 - @now는 완료 타임스탬프(ns).
	 * 실행 컨텍스트: 완료 인터럽트 처리 경로(드라이버 IRQ 핸들러 또는 폴링
	 *   완료 루프) - 짧고 락 경합이 적어야 한다.
	 * 구현: kyber만 구현(kyber_completed_request) - 도메인별 레이턴시를
	 *   기록해 read_lat_nsec/write_lat_nsec 튜너블과 비교, 토큰 수 자동 조절에
	 *   반영. mq-deadline/BFQ는 구현하지 않음. */
	void (*requeue_request)(struct request *);
	/* [한국어] 디스패치됐던 request가 드라이버 사정(BLK_STS_RESOURCE 등)으로
	 * 다시 스케줄러 큐에 되돌아올 때 호출되는 선택적 콜백 - 태그/토큰 등
	 * "미리 확보했던" 자원을 되돌리는 역할을 한다.
	 * 호출 시점/자: block/blk-mq-sched.h 인라인 헬퍼가
	 *   `if (e->type->ops.requeue_request) e->type->ops.requeue_request(rq)`로 호출
	 *   - blk_mq_requeue_request() 경로.
	 * 실행 컨텍스트: 드라이버 콜백에서 이어지는 컨텍스트 또는 requeue 워커.
	 * 구현: BFQ bfq_finish_requeue_request(finish_request와 유사하게 bfq_queue
	 *   정리), kyber는 kyber_finish_request를 requeue_request에도 그대로
	 *   등록해 동일 로직(domain token 반환)을 재사용. mq-deadline은 구현하지
	 *   않음(별도 requeue 특수 처리 불필요). */
	struct request *(*former_request)(struct request_queue *, struct request *);
	/* [한국어] 스케줄러의 정렬 순서 상에서 @rq "바로 앞"에 위치한 request를
	 * 찾는 선택적 콜백 - front-merge 후보 탐색이나 병합 체인 역추적에 쓰인다.
	 * 호출 시점/자: block/elevator.c elv_former_request()가
	 *   `if (e->type->ops.former_request) return e->type->ops.former_request(q, rq)`
	 *   로 호출.
	 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
	 * 구현: mq-deadline과 BFQ 둘 다 공용 헬퍼 elv_rb_former_request를 그대로
	 *   등록(자체 rb-tree가 LBA 순 정렬이므로 rb_prev만으로 충분). kyber는
	 *   구현하지 않음(도메인 토큰 기반이라 LBA 정렬 개념이 없음). */
	struct request *(*next_request)(struct request_queue *, struct request *);
	/* [한국어] former_request의 반대 - @rq "바로 다음"에 위치한 request를
	 * 찾는 선택적 콜백 - back-merge 후보 탐색이나 순차 접근 예측에 쓰인다.
	 * 호출 시점/자: block/elevator.c elv_latter_request()가
	 *   `if (e->type->ops.next_request) return e->type->ops.next_request(q, rq)`
	 *   로 호출.
	 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
	 * 구현: mq-deadline과 BFQ 둘 다 공용 헬퍼 elv_rb_latter_request를 등록.
	 *   kyber는 구현하지 않음. */
	void (*init_icq)(struct io_cq *);
	/* [한국어] 새 io_cq(프로세스별 io_context가 이 request_queue에 대해 갖는
	 * 컨텍스트)가 생성될 때 스케줄러별 초기화를 수행하는 선택적 콜백.
	 * 호출 시점/자: block/blk-ioc.c가 icq 생성 경로에서
	 *   `if (et->ops.init_icq) et->ops.init_icq(icq)`로 호출 -
	 *   icq_cache(kmem_cache)로 할당된 직후.
	 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트(해당 프로세스가 이 큐에 처음
	 *   IO를 낼 때 lazily 생성).
	 * 구현: 현재 트리의 세 스케줄러(mq-deadline/BFQ/kyber) 모두 init_icq를
	 *   구현하지 않는다(icq_size가 설정된 BFQ조차 icq 자체는 icq_cache로
	 *   생성만 하고 별도 init 콜백 없이 bfq_io_cq 임베디드 필드의 기본값에
	 *   의존) - 필드는 확장성을 위해 남아있는 훅으로 이해하면 된다. */
	void (*exit_icq)(struct io_cq *);
	/* [한국어] icq가 해제될 때(태스크 종료, io_context 해제 등) 스케줄러별
	 * 정리를 수행하는 선택적 콜백 - init_icq의 반대.
	 * 호출 시점/자: block/blk-ioc.c ioc_exit_icq()가
	 *   `if (et->ops.exit_icq) et->ops.exit_icq(icq)`로 호출.
	 * 실행 컨텍스트: 태스크 종료 경로(do_exit → exit_io_context) 또는
	 *   io_context 명시적 해제 - 임의의 프로세스 컨텍스트일 수 있음.
	 * 구현: BFQ만 구현(bfq_exit_icq → 내부적으로 _bfq_exit_icq/
	 *   bfq_exit_icq_bfqq 호출) - 이 프로세스가 소유했던 sync/async
	 *   bfq_queue들의 참조를 해제하고 필요하면 idle 상태로 만든다.
	 *   mq-deadline/kyber는 프로세스별 icq 서브클래싱을 쓰지 않아 구현이
	 *   없다. */
};

#define ELV_NAME_MAX	(16)
/* [한국어] 스케줄러 이름 문자열의 최대 길이(NUL 포함). elv_change_ctx.name
 * 버퍼 크기(elv_iosched_store의 지역 배열)와 struct elevator_type.icq_cache_name
 * 배열 크기(ELV_NAME_MAX + 6, "_io_cq" 접미사 포함) 계산에 쓰인다. "mq-deadline"
 * (11자), "bfq"(3자), "kyber"(5자), "none"(4자) 모두 여유 있게 들어간다. */

/* sysfs를 통해 스케줄러 속성을 노출하기 위한 구조 */
/*
 * [한국어] elv_fs_entry: /sys/block/<disk>/queue/iosched/<attr> 하나에 대응하는
 * sysfs 파일 서술자. struct elevator_type.elevator_attrs가 이 구조체의 배열을
 * 가리키며, block/elevator.c의 elv_register_queue()가 배열을 순회하며
 * sysfs_create_file()로 실제 파일을 만든다. 예: mq-deadline의 read_expire,
 * write_expire, fifo_batch; BFQ의 slice_idle, low_latency 등.
 */
struct elv_fs_entry {
	struct attribute attr;
	/* [한국어] sysfs 커널 오브젝트 모델(kobject)이 요구하는 기본 attribute
	 * (이름 문자열 + 파일 모드) - 이 필드가 있어야 sysfs_create_file()로
	 * 실제 파일 노드를 만들 수 있다.
	 * 설정자: 각 스케줄러 소스의 정적 배열 초기화 매크로(__ATTR 등)로
	 *   컴파일 타임에 채워짐(예: mq-deadline.c의 DD_ATTR 계열 매크로).
	 * 읽는 자: elv_register_queue()가 attr.name이 NULL일 때까지 배열을
	 *   순회하며 sysfs_create_file(&e->kobj, &attr->attr) 호출.
	 * 값 범위: name은 "read_expire" 등 파일명 문자열, mode는 0644/0444 등.
	 * 동기화: 정적 초기화라 런타임 동기화 불필요. */
	ssize_t (*show)(struct elevator_queue *, char *);
	/* [한국어] 이 sysfs 파일을 read(2)했을 때 값을 문자열로 써주는 콜백.
	 * 설정자: 스케줄러가 attr 배열 정의 시 함수 포인터로 지정.
	 * 읽는 자: block/elevator.c elv_attr_show()가 sysfs_lock을 쥔 채
	 *   `error = entry->show(e, page)`로 호출(ELEVATOR_FLAG_DYING이면 호출
	 *   자체를 건너뛰고 -ENODEV).
	 * 값 범위: 반환값은 page에 쓴 바이트 수(성공) 또는 음수 errno.
	 * 동기화: 호출자가 e->sysfs_lock을 쥐고 호출하므로 콜백 내부에서 elevator_data
	 *   를 읽을 때 추가 락이 필요 없는 경우가 많다(스케줄러 자체 락이 필요하면
	 *   콜백 내부에서 별도로 획득). */
	ssize_t (*store)(struct elevator_queue *, const char *, size_t);
	/* [한국어] 이 sysfs 파일에 write(2)했을 때 사용자 입력을 파싱해 적용하는 콜백.
	 * 설정자: show와 동일하게 attr 배열 정의 시 지정.
	 * 읽는 자: block/elevator.c elv_attr_store()가 sysfs_lock을 쥔 채
	 *   `error = entry->store(e, page, length)`로 호출(DYING이면 건너뜀).
	 * 값 범위: 반환값은 소비한 바이트 수(성공, 보통 length 그대로) 또는 음수 errno.
	 * 동기화: show와 동일 - sysfs_lock으로 elevator_exit()과의 race는 막히지만,
	 *   내부 자료구조(rb-tree 등)를 만지는 값이면 스케줄러 자체 락이 추가로
	 *   필요할 수 있다. */
};

/*
 * identifies an elevator type, such as AS or deadline
 */
/*
 * [한국어] elevator_type: 스케줄러 "종류"(mq-deadline/bfq/kyber/none) 자체를
 * 나타내는 정적 서술자 - 모듈 로드 시 1개만 만들어져 elv_list에 등록되고,
 * 여러 request_queue가 이 하나의 elevator_type을 공유해서 참조한다(참조
 * 카운트는 elevator_owner 모듈 refcount로 관리). request_queue별 "인스턴스"
 * 상태는 별도의 struct elevator_queue가 담당한다 - elevator_type은 "클래스",
 * elevator_queue는 "인스턴스"에 대응한다고 생각하면 된다.
 */
struct elevator_type
{
	/* managed by elevator core */
	struct kmem_cache *icq_cache;
	/* [한국어] 이 스케줄러의 io_cq 서브클래스(예: bfq_io_cq) 전용 slab 캐시.
	 * 설정자: elv_register()가 icq_size > 0이면 kmem_cache_create()로 생성.
	 * 읽는 자: block/blk-ioc.c의 icq 할당 경로가 이 캐시로 kmem_cache_alloc()
	 *   호출; elv_unregister()가 rcu_barrier() 이후 kmem_cache_destroy().
	 * 값 범위: icq_size가 0이면(=이 스케줄러가 icq 서브클래싱을 쓰지 않으면,
	 *   예: mq-deadline/kyber) NULL로 유지.
	 * 동기화: 생성/파괴는 모듈 로드/언로드 시 한 번씩만 일어나 elv_list_lock
	 *   보호 구간 밖에서도 안전(등록 실패/중복 시에만 즉시 destroy). */

	/* fields provided by elevator implementation */
	struct elevator_mq_ops ops;
	/* [한국어] 이 스케줄러가 구현하는 콜백 vtable 본체(위 struct elevator_mq_ops
	 * 참고) - 이 파일에서 가장 핵심적인 필드.
	 * 설정자: 각 스케줄러 소스 파일의 static 전역 elevator_type 초기화 리터럴
	 *   (mq-deadline.c의 mq_deadline, bfq-iosched.c의 iosched_bfq_mq,
	 *   kyber-iosched.c의 kyber_sched)에서 .ops = { ... } 형태로 채움.
	 * 읽는 자: block/elevator.c, blk-mq.c, blk-mq-sched.c(.h), blk-ioc.c
	 *   전역의 e->type->ops.xxx 또는 e->ops.xxx 형태 호출.
	 * 값 범위: 함수 포인터들의 조합 - NULL인 필드는 "이 스케줄러는 해당
	 *   기능을 지원하지 않음"을 뜻한다(선택적 콜백들).
	 * 동기화: 등록 후에는 불변(immutable) - 런타임에 갱신되지 않으므로 락 불필요. */

	size_t icq_size;	/* see iocontext.h */
	/* [한국어] icq_cache로 할당할 io_cq 서브클래스의 실제 크기(바이트).
	 * 설정자: 스케줄러가 sizeof(struct bfq_io_cq)처럼 자신의 io_cq 서브클래스
	 *   크기로 초기화(mq-deadline/kyber는 io_cq 서브클래싱을 안 해서 0).
	 * 읽는 자: elv_register()가 `WARN_ON(e->icq_size < sizeof(struct io_cq))`로
	 *   최소 크기를 검증한 뒤 kmem_cache_create()의 object size로 사용.
	 * 값 범위: 0(icq 캐시 없음) 또는 sizeof(struct io_cq) 이상.
	 * 동기화: 정적 초기화 값 - 불변. */
	size_t icq_align;	/* ditto */
	/* [한국어] icq_size와 짝을 이루는 정렬(alignment) 요구사항.
	 * 설정자: __alignof__(struct bfq_io_cq)처럼 실제 타입의 정렬 요구사항으로 초기화.
	 * 읽는 자: elv_register()가 `WARN_ON(e->icq_align < __alignof__(struct io_cq))`
	 *   로 검증 후 kmem_cache_create()의 align 인자로 사용.
	 * 값 범위: icq_size와 마찬가지로 0(미사용) 또는 유효한 정렬값(보통 8/16).
	 * 동기화: 정적 초기화 값 - 불변. */
	const struct elv_fs_entry *elevator_attrs;
	/* [한국어] 이 스케줄러의 sysfs 튜너블 배열(마지막 원소는 attr.name==NULL로
	 * 종료 표시) - /sys/block/<disk>/queue/iosched/ 하위 파일들의 원본.
	 * 설정자: 각 스케줄러가 정적 배열(예: bfq_attrs[], deadline_attrs[],
	 *   kyber_sched_attrs[])의 주소로 초기화.
	 * 읽는 자: elv_register_queue()가 NULL이 아니면 배열을 순회하며
	 *   sysfs_create_file() 반복 호출.
	 * 값 범위: 유효한 배열 포인터 또는 NULL(튜너블 없음 - 실제로는 세
	 *   스케줄러 모두 최소 하나 이상의 attr을 갖는다).
	 * 동기화: 정적 배열이라 불변. */
	const char *elevator_name;
	/* [한국어] 사용자가 sysfs("echo <name> > .../scheduler")로 선택할 때
	 * 쓰는 정식 이름 문자열.
	 * 설정자: "mq-deadline", "bfq", "kyber" 등 컴파일 타임 상수.
	 * 읽는 자: elevator_match()가 strcmp()로 사용자가 입력한 이름과 비교;
	 *   elv_iosched_show()가 등록된 스케줄러 목록을 나열할 때 사용;
	 *   elv_register()가 icq_cache_name 생성("%s_io_cq")에도 사용.
	 * 값 범위: ELV_NAME_MAX(16) 미만의 고유 문자열 - elv_register()가 중복
	 *   이름이면 -EBUSY로 거부.
	 * 동기화: 정적 상수 - 불변. */
	const char *elevator_alias;
	/* [한국어] elevator_name의 별칭(옛 이름 등) - 하위 호환용.
	 * 설정자: mq-deadline이 "deadline"(레거시 single-queue deadline과 동일 이름)
	 *   으로 설정; BFQ/kyber는 별칭이 없어 NULL.
	 * 읽는 자: elevator_match()가 elevator_name과 함께 alias도 strcmp()로 비교해
	 *   사용자가 옛 이름으로 요청해도 동일 스케줄러를 찾도록 함.
	 * 값 범위: 유효한 문자열 또는 NULL(별칭 없음).
	 * 동기화: 정적 상수 - 불변. */
	struct module *elevator_owner;
	/* [한국어] 이 elevator_type을 제공하는 커널 모듈 - 참조 카운트로 사용 중
	 * 모듈이 언로드되지 않도록 보호한다.
	 * 설정자: THIS_MODULE 매크로로 컴파일 타임에 채워짐.
	 * 읽는 자: elevator_tryget()/__elevator_get()/elevator_put()이 각각
	 *   try_module_get()/__module_get()/module_put()으로 참조 카운트 증감.
	 * 값 범위: 유효한 module 포인터(내장 빌트인이면 특수 처리되어 no-op에 가까움).
	 * 동기화: 모듈 서브시스템 내부의 원자적 refcount로 보호. */
#ifdef CONFIG_BLK_DEBUG_FS
	const struct blk_mq_debugfs_attr *queue_debugfs_attrs;
	/* [한국어] CONFIG_BLK_DEBUG_FS(디버그용 blk-mq debugfs 지원)가 켜진
	 * 빌드에서만 존재 - request_queue 단위(hctx 아님) debugfs 속성 배열.
	 * 설정자: 각 스케줄러의 정적 배열(예: kyber_queue_debugfs_attrs[]).
	 * 읽는 자: block/blk-mq-sched.c blk_mq_sched_reg_debugfs() →
	 *   blk_mq_debugfs_register_sched(q)가 이 배열로 /sys/kernel/debug/block/
	 *   <disk>/sched/ 파일들을 생성.
	 * 값 범위: 유효한 배열 포인터 또는 NULL(디버그 속성 없음, 예: mq-deadline은
	 *   hctx 단위 attrs만 제공).
	 * 동기화: 정적 배열 - 불변. CONFIG_BLK_DEBUG_FS가 꺼지면 필드 자체가
	 *   컴파일에서 제외된다(조건부 컴파일). */
	const struct blk_mq_debugfs_attr *hctx_debugfs_attrs;
	/* [한국어] hctx(디스패치 큐, NVMe SQ 단위) 별 debugfs 속성 배열 - 위
	 * queue_debugfs_attrs와 대응하는 hctx 스코프 버전.
	 * 설정자: 각 스케줄러의 정적 배열(예: kyber_hctx_debugfs_attrs[],
	 *   deadline_queue_debugfs_attrs[]는 mq-deadline이 hctx 단위로 등록하는 예).
	 * 읽는 자: blk_mq_sched_reg_debugfs()가 각 hctx에 대해
	 *   blk_mq_debugfs_register_sched_hctx(q, hctx) 호출 시 사용.
	 * 값 범위: 유효한 배열 포인터 또는 NULL.
	 * 동기화: 정적 배열 - 불변; 등록/해제 자체는 blk_debugfs_lock()으로 직렬화
	 *   (elevator.c/blk-mq-sched.c 쪽 책임이지 이 필드 자체의 동기화는 아님). */
#endif

	/* managed by elevator core */
	char icq_cache_name[ELV_NAME_MAX + 6];	/* elvname + "_io_cq" */
	/* [한국어] icq_cache의 kmem_cache 이름 문자열 - /proc/slabinfo 등에서
	 * 식별할 수 있도록 "<elevator_name>_io_cq" 형태로 조립된다.
	 * 설정자: elv_register()가 snprintf(e->icq_cache_name, ...,
	 *   "%s_io_cq", e->elevator_name)로 런타임에 채움(정적 초기화 아님).
	 * 읽는 자: kmem_cache_create()의 name 인자로 전달.
	 * 값 범위: 최대 ELV_NAME_MAX+6-1 글자(NUL 제외) - "mq-deadline_io_cq"
	 *   (11+7=18자)도 여유 있게 들어간다(16+6=22바이트 버퍼).
	 * 동기화: elv_register() 단일 호출 내에서만 쓰여지므로 별도 동기화 불필요. */
	struct list_head list;
	/* [한국어] 전역 elv_list(block/elevator.c의 static LIST_HEAD)에 이
	 * elevator_type을 매다는 연결 리스트 노드.
	 * 설정자: elv_register()가 list_add_tail(&e->list, &elv_list)로 연결.
	 * 읽는 자: __elevator_find()가 list_for_each_entry()로 순회하며 이름 매칭;
	 *   elv_iosched_show()가 등록된 스케줄러 목록 출력 시 순회.
	 * 값 범위: 등록 중에는 elv_list에 연결된 유효한 노드, elv_unregister()가
	 *   list_del_init()으로 분리하면 자기 자신을 가리키는 빈 리스트가 됨.
	 * 동기화: elv_list_lock(spinlock)으로 전체 리스트에 대한 추가/삭제/순회를
	 *   보호 - NVMe 등 여러 드라이버가 동시에 모듈을 로드/언로드해도 안전. */
};

/*
 * [한국어]
 * elevator_tryget - 스케줄러 소유 모듈의 참조 카운트를 조건부로 증가
 *
 * @e: 참조를 얻으려는 elevator_type
 * @return: 성공 시 true(참조 획득), 모듈이 이미 제거 중이면 false
 *
 * try_module_get()은 모듈이 이미 언로드 진행 중(refcount가 0으로 가는 중)이면
 * 실패를 반환하는 "안전한" 참조 획득 방식이다. 사용자가 sysfs로 스케줄러를
 * 선택하는 시점과 모듈이 rmmod되는 시점이 겹칠 수 있으므로, 무조건 증가시키는
 * __elevator_get()과 달리 이 함수는 그 race를 방어한다.
 * 실행 컨텍스트: bio 제출과 무관한 관리 경로(스케줄러 조회/전환) - 임의의
 * 프로세스 컨텍스트에서 호출 가능, 블로킹하지 않는다.
 * caller: block/elevator.c의 elevator_find_get()이 __elevator_find()로 찾은
 *   elevator_type에 대해 이 함수로 참조를 확보한 뒤 반환(실패하면 NULL 반환).
 * callee: try_module_get() - 커널 모듈 서브시스템의 원자적 refcount 증가.
 * 에러 처리: false 반환 시 elevator_find_get()이 e = NULL로 처리해 호출자가
 *   해당 스케줄러를 사용할 수 없는 것으로 취급.
 *
 * 호출 체인:
 *   elevator_find_get() → [elevator_tryget]
 */
static inline bool elevator_tryget(struct elevator_type *e)
{
	return try_module_get(e->elevator_owner);
	/* [한국어] e->elevator_owner(THIS_MODULE)의 refcount를 원자적으로 +1 시도.
	 * 모듈이 이미 MODULE_STATE_GOING(언로드 중)이면 실패(false)를 반환해
	 * 호출자가 이 스케줄러를 더 이상 쓰지 않도록 막는다. */
}

/*
 * [한국어]
 * __elevator_get - 스케줄러 소유 모듈의 참조 카운트를 무조건 증가
 *
 * @e: 참조를 얻을 elevator_type
 * @return: 없음(void)
 *
 * elevator_tryget()과 달리 실패 가능성을 따지지 않는 무조건적 증가다.
 * 이미 다른 경로(예: elevator_find_get()의 elevator_tryget 성공)로 참조가
 * 확보된 상태에서, elevator_alloc()이 elevator_queue를 새 소유자로 등록할 때
 * "추가로" 참조를 하나 더 얹는 용도로 쓰인다 - 이 시점엔 모듈이 이미
 * 안전하게 잡혀 있음이 보장되므로 실패 검사가 불필요하다.
 * 실행 컨텍스트: elevator_alloc() 호출 경로(스케줄러 전환 트랜잭션 내부).
 * caller: block/elevator.c의 elevator_alloc()이 eq->type = e로 소유권을
 *   넘기기 직전에 호출.
 * callee: __module_get() - 무조건적 원자적 refcount 증가(실패 없음).
 * 에러 처리: 없음 - 실패할 수 없는 연산.
 *
 * 호출 체인:
 *   elevator_alloc() → [__elevator_get]
 */
static inline void __elevator_get(struct elevator_type *e)
{
	__module_get(e->elevator_owner);
	/* [한국어] refcount를 무조건 +1 - elevator_tryget()과 달리 실패 검사 없음.
	 * 이미 안전하게 참조가 확보된 상황(전환 트랜잭션 도중)에서만 호출되므로
	 * 여기서 모듈이 사라질 가능성은 없다고 간주한다. */
}

/*
 * [한국어]
 * elevator_put - 스케줄러 소유 모듈의 참조 카운트를 감소
 *
 * @e: 참조를 반환할 elevator_type
 * @return: 없음(void)
 *
 * elevator_tryget()/__elevator_get()으로 얻은 참조를 반환한다. 참조가 0에
 * 도달하면 모듈 서브시스템이 언로드를 허용하게 된다 - 즉 이 함수 호출 전까지는
 * 해당 스케줄러를 제공하는 모듈이 rmmod되지 않음을 보장한다.
 * 실행 컨텍스트: 임의의 프로세스 컨텍스트 - 전환 실패/성공 양쪽 정리 경로에서
 *   호출된다.
 * caller: elevator_switch()(new_e 사용 후), elevator_set_default()/
 *   elevator_release()(elevator_queue kfree 직전 e->type 참조 반환) 등
 *   elevator_type 참조를 놓아야 하는 모든 지점.
 * callee: module_put() - 원자적 refcount 감소, 0 도달 시 모듈 언로드 가능 상태 전이.
 * 에러 처리: 없음 - 실패할 수 없는 연산(참조 카운트 불일치는 커널 경고로
 *   드러날 수 있으나 이 함수 자체가 에러를 반환하지 않는다).
 *
 * 호출 체인:
 *   elevator_switch() / elevator_release() / elevator_set_default() → [elevator_put]
 */
static inline void elevator_put(struct elevator_type *e)
{
	module_put(e->elevator_owner);
	/* [한국어] refcount -1. 0에 도달하면 모듈이 MODULE_STATE_GOING으로 전이
	 * 가능해져 이후 rmmod가 실제로 진행될 수 있다. */
}

#define ELV_HASH_BITS 6
/* [한국어] struct elevator_queue.hash(DECLARE_HASHTABLE)의 버킷 비트 수 -
 * 2^6 = 64개 버킷을 갖는 해시테이블을 만든다. 끝 섹터(rq_hash_key)를 키로
 * back-merge 후보를 찾는 elv_rqhash_find()의 평균 탐색 비용을 O(1)에 가깝게
 * 유지하기 위한 크기 선택이다 - request_queue당 in-flight request 수가
 * 보통 수십~수백 개이므로 64버킷이면 체이닝 길이가 짧게 유지된다. */

void elv_rqhash_del(struct request_queue *q, struct request *rq);
/*
 * [한국어]
 * elv_rqhash_del - request를 끝 섹터 기반 해시(back-merge 후보 테이블)에서 제거
 *
 * @q:  이 request가 속한 request_queue(해시는 q->elevator->hash에 있음)
 * @rq: 해시에서 제거할 request
 * @return: 없음(void)
 *
 * block/elevator.c의 실제 구현은 `if (ELV_ON_HASH(rq)) __elv_rqhash_del(rq)`
 * 형태로, RQF_HASHED 플래그가 서 있을 때만 hash_del()과 플래그 클리어를
 * 수행한다(멱등적 - 이미 빠져 있으면 아무 일도 하지 않음). request가
 * dispatch되어 더 이상 병합 대상이 아니게 되거나, 병합되어 사라질 때 호출된다.
 * 실행 컨텍스트: bio 제출/dispatch 프로세스 컨텍스트.
 * caller: 각 스케줄러의 dispatch_request 구현(예: dd_dispatch_request 계열이
 *   request를 뽑아낼 때) - 더 이상 병합 후보가 아님을 표시.
 * callee: __elv_rqhash_del() → hash_del()(linux/hashtable.h).
 * 에러 처리: 없음 - 해시에 없는 request를 넘겨도 안전(ELV_ON_HASH 검사로 방어).
 *
 * 호출 체인:
 *   스케줄러 ops.dispatch_request 구현 → [elv_rqhash_del]
 */
void elv_rqhash_add(struct request_queue *q, struct request *rq);
/*
 * [한국어]
 * elv_rqhash_add - request를 끝 섹터 키로 back-merge 후보 해시에 삽입
 *
 * @q:  이 request가 속한 request_queue
 * @rq: 해시에 추가할 request(아직 해시에 없어야 함)
 * @return: 없음(void)
 *
 * 실제 구현은 `BUG_ON(ELV_ON_HASH(rq)); hash_add(e->hash, &rq->hash,
 * rq_hash_key(rq)); rq->rq_flags |= RQF_HASHED;` - 끝 섹터(blk_rq_pos +
 * blk_rq_sectors)를 키로 hash_add()하고 RQF_HASHED를 세운다. 이미 해시에 있는
 * request를 다시 추가하면 BUG_ON으로 커널 패닉 - 호출자가 상태를 정확히
 * 추적해야 함을 뜻하는 강한 불변조건이다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트(request 삽입/병합 경로).
 * caller: elv_attempt_insert_merge(), elv_merged_request() - 새 request가
 *   생기거나 병합으로 끝 섹터가 바뀐 request를 해시에 (재)등록할 때.
 * callee: hash_add()(linux/hashtable.h).
 * 에러 처리: 없음(BUG_ON은 프로그래밍 오류를 잡기 위한 사전 방어 - 정상
 *   경로에서는 발생하지 않아야 함).
 *
 * 호출 체인:
 *   elv_attempt_insert_merge() / elv_merged_request() → [elv_rqhash_add]
 */
void elv_rqhash_reposition(struct request_queue *q, struct request *rq);
/*
 * [한국어]
 * elv_rqhash_reposition - 병합 등으로 끝 섹터가 바뀐 request를 해시에서 재배치
 *
 * @q:  이 request가 속한 request_queue
 * @rq: 재배치할 request(끝 섹터가 이미 변경된 이후 상태)
 * @return: 없음(void)
 *
 * 실제 구현은 단순히 `__elv_rqhash_del(rq); elv_rqhash_add(q, rq);` -
 * 옛 키로 제거하고 새 키(현재 rq_hash_key(rq) 값)로 다시 삽입한다. request가
 * 병합으로 커지면 끝 섹터가 바뀌므로, 이후 elv_rqhash_find()가 정확한 위치에서
 * 후보를 찾도록 하려면 반드시 이 재배치가 필요하다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: elv_merged_request()(back-merge 후), elv_merge_requests()(두
 *   request 병합 후) - 둘 다 rq의 끝 섹터가 변경된 직후 호출.
 * callee: __elv_rqhash_del()(내부 정적 함수), elv_rqhash_add().
 * 에러 처리: 없음 - __elv_rqhash_del이 ELV_ON_HASH 검사 없이 바로 hash_del
 *   하므로 호출 전 rq가 실제로 해시에 있어야 한다(호출자가 보장).
 *
 * 호출 체인:
 *   elv_merged_request() / elv_merge_requests() → [elv_rqhash_reposition]
 */
struct request *elv_rqhash_find(struct request_queue *q, sector_t offset);
/*
 * [한국어]
 * elv_rqhash_find - 끝 섹터가 정확히 @offset인 back-merge 후보 request 탐색
 *
 * @q:      탐색할 request_queue
 * @offset: 찾으려는 끝 섹터(보통 새 bio의 시작 섹터 bio->bi_iter.bi_sector)
 * @return: 조건에 맞는 request 포인터, 없으면 NULL
 *
 * hash_for_each_possible_safe()로 @offset 버킷의 후보들을 순회하며, 이미 dispatch
 * 되어 병합 불가능한(!rq_mergeable()) request는 발견 즉시 __elv_rqhash_del()로
 * 해시에서 청소(lazy cleanup)하고 계속 탐색한다. rq_hash_key(rq) == offset인
 * 첫 후보를 반환한다 - 즉 O(1)에 가까운 back-merge 후보 탐색이다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
 * caller: block/elevator.c의 elv_merge()(새 bio의 병합 후보 탐색),
 *   elv_attempt_insert_merge()(새 request를 기존 request 뒤에 이어붙일 후보 탐색).
 * callee: hash_for_each_possible_safe(), rq_mergeable(), __elv_rqhash_del().
 * 에러 처리: 후보가 전혀 없거나 모두 병합 불가능하면 NULL 반환 - 호출자는
 *   이를 "해시 기반 병합 실패"로 처리하고 스케줄러별 request_merge로 폴백.
 *
 * 호출 체인:
 *   elv_merge() / elv_attempt_insert_merge() → [elv_rqhash_find]
 */

/*
 * each queue has an elevator_queue associated with it
 */
/*
 * [한국어] elevator_queue: 하나의 request_queue에 부착된 스케줄러 "인스턴스".
 * elevator_type이 "어떤 스케줄러인지"(클래스, 여러 큐가 공유)를 나타낸다면,
 * elevator_queue는 "이 특정 큐에서 그 스케줄러가 어떤 상태를 갖고 있는지"
 * (인스턴스, 큐마다 독립)를 나타낸다. request_queue.elevator 필드가 이
 * 구조체를 가리키며, elevator_alloc()이 생성하고 elevator_release()
 * (kobject 참조 0 도달 시)가 해제한다.
 */
struct elevator_queue
{
	struct elevator_type *type;
	/* [한국어] 이 인스턴스가 어떤 스케줄러 "종류"에 속하는지 가리키는 역참조.
	 * 설정자: elevator_alloc()이 eq->type = e(전달받은 elevator_type)로 설정,
	 *   동시에 __elevator_get(e)로 모듈 참조도 함께 증가.
	 * 읽는 자: 거의 모든 elv_* 함수가 e->type->ops.xxx로 콜백을 호출하는 진입점
	 *   - 예: elv_merge()의 e->type->ops.request_merge.
	 * 값 범위: 유효한 elevator_type 포인터(NULL 불가 - elevator_queue가
	 *   존재하려면 반드시 type이 있어야 함).
	 * 동기화: 인스턴스 생성 시 한 번 설정된 후 불변(elevator_queue 수명 동안
	 *   고정) - 별도 락 불필요. */
	struct elevator_tags *et;
	/* [한국어] 이 인스턴스가 사용하는 shadow tag(CID) 풀 - struct
	 * elevator_resources.et에서 복사됨.
	 * 설정자: elevator_alloc()이 eq->et = res->et로 설정.
	 * 읽는 자: elevator_change_done()이 old elevator 해제 시
	 *   `res.et = ctx->old->et`로 복사해 blk_mq_free_sched_res()에 넘김;
	 *   blk_mq_init_sched()가 hctx->sched_tags/q->sched_shared_tags 연결에 사용.
	 * 값 범위: 유효한 elevator_tags 포인터.
	 * 동기화: 생성 시 설정 후 불변 - 전환 트랜잭션(elevator_lock+freeze) 동안만
	 *   교체됨. */
	void *elevator_data;
	/* [한국어] 스케줄러 구현체의 사설(private) 상태 포인터 - 실제 타입은
	 * 스케줄러마다 다르다(mq-deadline: struct deadline_data, BFQ: struct
	 * bfq_data, kyber: struct kyber_queue_data).
	 * 설정자: elevator_alloc()이 eq->elevator_data = res->data로 설정.
	 * 읽는 자: 모든 ops 콜백 내부에서 q->elevator->elevator_data(또는 콜백에
	 *   전달된 elevator_queue*의 elevator_data)를 자신의 사설 타입으로
	 *   캐스팅해 접근 - 예: bfqd = e->elevator_data.
	 * 값 범위: 스케줄러마다 다른 타입을 가리키는 void* - 타입 안전성은
	 *   각 스케줄러 구현이 책임짐.
	 * 동기화: 가리키는 구조체 내부의 필드별 동기화는 각 스케줄러 자체 락
	 *   (bfqd->lock, dd->lock 등)이 담당 - elevator_queue 계층에서는 관리하지
	 *   않음. */
	struct kobject kobj;
	/* [한국어] sysfs 노출용 kobject - /sys/block/<disk>/queue/iosched
	 * 디렉토리 자체에 대응한다.
	 * 설정자: elevator_alloc()이 kobject_init(&eq->kobj, &elv_ktype)로 초기화.
	 * 읽는 자: elv_register_queue()가 kobject_add()로 실제 sysfs 노드 생성;
	 *   elv_attr_show/store가 container_of(kobj, struct elevator_queue, kobj)로
	 *   역참조; kobject_put() 참조가 0이 되면 elevator_release()가 kfree(e).
	 * 값 범위: kobject 서브시스템이 관리하는 참조 카운트를 내장 - 직접 필드
	 *   접근보다는 kobject_get/put API를 통해 조작.
	 * 동기화: kobject 자체의 refcount는 원자적(atomic) - 구조체 나머지
	 *   필드와의 일관성은 sysfs_lock이 별도로 보장. */
	struct mutex sysfs_lock;
	/* [한국어] sysfs 속성 접근(show/store)과 elevator_exit() 간의 동시 접근을
	 * 막는 뮤텍스.
	 * 설정자: elevator_alloc()이 mutex_init(&eq->sysfs_lock)로 초기화.
	 * 읽는 자: elv_attr_show()/elv_attr_store()가 entry->show/store 호출 전후로
	 *   획득/해제; elevator_exit()도 blk_mq_exit_sched() 호출 구간에서 획득 -
	 *   즉 "파라미터 읽는 중에 스케줄러가 통째로 사라지는" race를 막는다.
	 * 값 범위: mutex 상태(잠김/풀림) - 재진입 불가.
	 * 동기화: 이 뮤텍스 자체가 elevator_queue 파괴와 sysfs I/O 간의 유일한
	 *   동기화 장치이므로, 두 경로 모두 반드시 이 락을 통해서만 elevator_data에
	 *   접근하도록 관례화되어 있다. */
	unsigned long flags;
	/* [한국어] ELEVATOR_FLAG_REGISTERED/ELEVATOR_FLAG_DYING 두 비트를 담는
	 * 상태 플래그.
	 * 설정자: elv_register_queue()가 REGISTERED를 set_bit(); elevator 종료
	 *   경로(변경 전 old elevator 처리 등)가 DYING을 set_bit()하는 지점은
	 *   블록 계층 상위(디스크 제거/전환) 로직에 있다.
	 * 읽는 자: elv_attr_show/store가 DYING이면 -ENODEV로 접근 차단;
	 *   elv_unregister_queue()가 test_and_clear_bit(REGISTERED)로 중복 해제 방지.
	 * 값 범위: 두 비트의 조합 - 초기값은 REGISTERED도 DYING도 서지 않은 0.
	 * 동기화: 각 비트가 test_bit/set_bit/test_and_clear_bit 등 원자적 비트
	 *   연산으로만 조작되어 별도 락 없이도 안전. */
	DECLARE_HASHTABLE(hash, ELV_HASH_BITS);
	/* [한국어] 이 큐의 back-merge 후보를 찾기 위한 끝 섹터 기반 해시테이블
	 * (2^ELV_HASH_BITS = 64버킷) - elv_rqhash_add/del/find/reposition이
	 * 이 필드를 직접 조작한다.
	 * 설정자: elevator_alloc()이 hash_init(eq->hash)로 모든 버킷을 비움.
	 * 읽는 자/쓰는 자: elv_rqhash_find()가 hash_for_each_possible_safe()로 탐색,
	 *   elv_rqhash_add/del이 hash_add()/hash_del()로 갱신.
	 * 값 범위: hlist_head 배열 - 각 버킷은 RQF_HASHED가 선 request들의 연결 리스트.
	 * 동기화: 이 해시는 큐 단위로 하나만 존재하며, bio 제출/병합 경로가 request_queue
	 *   락(또는 per-ctx 락) 보호 하에서만 조작한다고 가정 - elevator_queue
	 *   자체에는 이 해시 전용 락이 없다(호출자 책임). */
};

#define ELEVATOR_FLAG_REGISTERED	0
/* [한국어] elevator_queue.flags의 비트 0 - sysfs에 iosched 디렉토리가 등록되어
 * 있고 사용 가능한 상태임을 뜻한다.
 * 설정: elv_register_queue()가 kobject_add() 성공 후 set_bit()로 세움.
 * 해제: elv_unregister_queue()가 test_and_clear_bit()로 원자적으로 클리어하며,
 *   그 결과가 true였을 때만(즉 등록되어 있었을 때만) uevent/kobject_del을 수행 -
 *   중복 해제를 막는 가드 역할도 겸한다. */
#define ELEVATOR_FLAG_DYING		1
/* [한국어] elevator_queue.flags의 비트 1 - 이 인스턴스가 종료/전환 절차를
 * 밟고 있어 더 이상 sysfs를 통한 파라미터 접근을 허용하면 안 되는 상태.
 * 설정/해제: elv_attr_show()/elv_attr_store()가 test_bit()으로만 확인하고
 *   -ENODEV를 반환하는 소비자 쪽 - 실제 set 지점은 디스크/큐 제거 및 전환
 *   경로(상위 블록 계층)에 있다.
 * 효과: DYING이 서 있으면 show/store 콜백 자체를 호출하지 않아, 스케줄러가
 *   해제되는 도중 elevator_data에 접근해 use-after-free가 나는 것을 방지한다. */

/*
 * block elevator interface
 */
extern enum elv_merge elv_merge(struct request_queue *, struct request **,
		struct bio *);
/*
 * [한국어]
 * elv_merge - bio가 기존 request와 병합 가능한지 판정
 *
 * @q:   대상 request_queue
 * @req: [출력] 병합 대상으로 판정된 request를 담을 포인터의 포인터
 * @bio: 새로 도착한 bio
 * @return: enum elv_merge - ELEVATOR_NO_MERGE(병합 불가) 또는
 *   ELEVATOR_FRONT_MERGE/BACK_MERGE/DISCARD_MERGE(병합 가능, 방향/종류 명시)
 *
 * blk-mq가 bio를 새 request로 승격시키기 전에 항상 먼저 호출하는 진입점.
 * (1) nomerges 플래그나 !bio_mergeable(bio)면 즉시 실패, (2) q->last_merge
 * one-hit 캐시로 빠른 재시도, (3) elv_rqhash_find()로 back-merge 후보 탐색
 * (discard면 ELEVATOR_DISCARD_MERGE), (4) 그래도 없으면 스케줄러별
 * ops.request_merge로 폴백(주로 front-merge) 순으로 단계적으로 시도한다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
 * caller: blk_mq_submit_bio() → blk_mq_get_request() 이전 단계.
 * callee: elv_bio_merge_ok(), elv_rqhash_find(), e->type->ops.request_merge.
 * 에러 처리: 병합 실패는 에러가 아니라 정상 경로(ELEVATOR_NO_MERGE) - 호출자가
 *   새 request를 할당해 계속 진행.
 *
 * 호출 체인:
 *   blk_mq_submit_bio() → [elv_merge] → e->type->ops.request_merge
 */
extern void elv_merge_requests(struct request_queue *, struct request *,
			       struct request *);
/*
 * [한국어]
 * elv_merge_requests - 두 개의 완전한 request가 하나로 병합됐음을 스케줄러에 통지
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   병합 결과로 남는 request(next를 흡수)
 * @next: 병합되어 사라질 request(곧 free됨)
 * @return: 없음(void)
 *
 * blk_attempt_req_merge()가 두 request의 bio 체인/섹터 범위를 하나로 합친
 * "직후" 호출되어, 스케줄러 내부의 두 request 간 연결 상태(BFQ의 bfq_queue
 * 트리 등)를 정리한다. 이어서 rq의 끝 섹터가 바뀌었으므로 elv_rqhash_reposition()
 * 으로 해시 위치를 갱신하고, q->last_merge를 rq로 갱신해 다음 one-hit 캐시
 * 히트 확률을 높인다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: blk_attempt_req_merge().
 * callee: e->type->ops.requests_merged, elv_rqhash_reposition().
 * 에러 처리: 없음(void) - 병합 자체의 성패는 호출자(blk_attempt_req_merge)가
 *   이미 결정한 뒤이므로 이 함수는 순수 후처리.
 *
 * 호출 체인:
 *   blk_attempt_req_merge() → [elv_merge_requests]
 */
extern void elv_merged_request(struct request_queue *, struct request *,
		enum elv_merge);
/*
 * [한국어]
 * elv_merged_request - bio 하나가 기존 request에 병합된 후 상태 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   병합 결과로 확장된 request
 * @type: elv_merge()가 반환했던 병합 종류(ELEVATOR_BACK_MERGE 등)
 * @return: 없음(void)
 *
 * elv_merge()가 병합 가능을 판정하고 실제 blk_try_merge()로 병합을 마친 직후
 * 호출된다. 스케줄러별 ops.request_merged로 내부 정렬 위치를 갱신하고,
 * back-merge였다면(끝 섹터가 바뀌었으므로) elv_rqhash_reposition()으로 해시를
 * 재배치한 뒤, q->last_merge를 rq로 갱신한다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
 * caller: blk_mq_submit_bio() 병합 성공 경로(elv_merge()가 ELEVATOR_NO_MERGE가
 *   아닌 값을 반환한 직후).
 * callee: e->type->ops.request_merged, elv_rqhash_reposition().
 * 에러 처리: 없음(void).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() → [elv_merged_request]
 */
extern bool elv_attempt_insert_merge(struct request_queue *, struct request *,
				     struct list_head *);
/*
 * [한국어]
 * elv_attempt_insert_merge - 새 request를 기존 request 뒤에 이어붙여 제거 가능하게 함
 *
 * @q:    대상 request_queue
 * @rq:   새로 삽입하려는 request(병합 성공 시 free 목록으로 옮겨짐)
 * @free: 병합으로 불필요해진 request들을 모으는 리스트(호출자가 이후 일괄 해제)
 * @return: true = 하나 이상 병합됨(rq는 free에 들어있음), false = 병합 없음
 *
 * q->last_merge 캐시를 먼저 시도한 뒤, blk_queue_noxmerges가 아니면
 * elv_rqhash_find()로 반복적으로 back-merge를 시도한다(한 번 병합된 rq가
 * 또 다른 request와 연쇄적으로 병합될 수 있으므로 while(1) 루프) - 이렇게
 * 여러 개의 작은 request를 하나로 뭉치면 최종적으로 드라이버에 제출되는
 * 명령 수가 줄어든다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트(request 삽입 경로).
 * caller: request 삽입 경로(예: plug list 처리) - 신규 request를 큐에
 *   넣기 직전 마지막으로 병합을 시도하는 지점.
 * callee: blk_attempt_req_merge(), elv_rqhash_find().
 * 에러 처리: 병합 실패는 에러가 아니라 false 반환 - 호출자가 rq를 그대로
 *   정상 삽입 경로로 진행.
 *
 * 호출 체인:
 *   request 삽입 경로 → [elv_attempt_insert_merge] → blk_attempt_req_merge()
 */
extern struct request *elv_former_request(struct request_queue *, struct request *);
/*
 * [한국어]
 * elv_former_request - 스케줄러 정렬 순서상 @rq 바로 "앞"의 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 판단한 이전 request, 없으면 NULL
 *
 * 실제 동작은 스케줄러가 등록한 ops.former_request로 완전히 위임된다(이
 * 함수 자체는 NULL 체크 후 호출하는 얇은 래퍼) - mq-deadline/BFQ는 공용
 * elv_rb_former_request(LBA 기준 rb_prev)를 등록해 사용한다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: blk_try_req_merge() 등 front-merge 체인을 역방향으로 추적하는 경로.
 * callee: e->type->ops.former_request.
 * 에러 처리: ops.former_request가 없으면 NULL 반환(에러 아님 - 해당 스케줄러가
 *   이 기능을 지원하지 않음을 뜻함).
 *
 * 호출 체인:
 *   blk_try_req_merge() → [elv_former_request] → 스케줄러 ops.former_request
 */
extern struct request *elv_latter_request(struct request_queue *, struct request *);
/*
 * [한국어]
 * elv_latter_request - 스케줄러 정렬 순서상 @rq 바로 "뒤"의 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 판단한 다음 request, 없으면 NULL
 *
 * elv_former_request의 반대 방향 - ops.next_request로 위임한다. mq-deadline/
 * BFQ는 공용 elv_rb_latter_request(LBA 기준 rb_next)를 등록해 사용한다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: blk_try_req_merge() 등 병합 체인을 순방향으로 추적하는 경로.
 * callee: e->type->ops.next_request.
 * 에러 처리: ops.next_request가 없으면 NULL 반환.
 *
 * 호출 체인:
 *   blk_try_req_merge() → [elv_latter_request] → 스케줄러 ops.next_request
 */

/*
 * io scheduler registration
 */
extern int elv_register(struct elevator_type *);
/*
 * [한국어]
 * elv_register - 새 IO 스케줄러 종류를 전역 elv_list에 등록
 *
 * @e: 등록할 elevator_type(모듈의 static 전역 변수)
 * @return: 0 = 성공, -EINVAL(필수 콜백 누락/icq 크기 부적합),
 *   -ENOMEM(icq_cache 생성 실패), -EBUSY(동일 이름 이미 등록됨)
 *
 * 먼저 WARN_ON_ONCE로 finish_request, insert_requests, dispatch_request가
 * 모두 구현되어 있는지 확인한다(이 셋은 사실상 필수). icq_size > 0이면
 * icq_cache_name을 "<name>_io_cq"로 조립하고 kmem_cache_create()로 전용
 * 캐시를 만든다. 마지막으로 elv_list_lock 하에서 중복 이름을 확인한 뒤
 * elv_list에 list_add_tail()한다. 이후 사용자가 sysfs로 이 이름을 지정하면
 * elevator_find_get()이 이 등록 정보를 찾아낸다.
 * 실행 컨텍스트: 모듈 __init 함수(예: mq_deadline_init) - 부팅 또는
 *   insmod/modprobe 시 단일 스레드 컨텍스트, 동시성 문제 없음(다만 elv_list_lock
 *   자체는 항상 정확히 잡는다).
 * caller: 각 스케줄러 모듈의 __init 함수(deadline_init, kyber_init, bfq_init).
 * callee: kmem_cache_create(), 내부 __elevator_find() 중복 검사.
 * 에러 처리: 실패 시 이미 생성한 icq_cache가 있으면 kmem_cache_destroy()로
 *   되돌리고 음수 errno 반환 - 모듈 __init이 이를 그대로 반환해 모듈 로드
 *   자체를 실패시킨다.
 *
 * 호출 체인:
 *   <스케줄러>_init() → [elv_register]
 */
extern void elv_unregister(struct elevator_type *);
/*
 * [한국어]
 * elv_unregister - IO 스케줄러 종류를 전역 목록에서 해제
 *
 * @e: 해제할 elevator_type
 * @return: 없음(void)
 *
 * elv_list_lock 하에서 list_del_init()으로 elv_list에서 제거해 이후
 * elevator_find_get()이 더는 이 스케줄러를 찾지 못하게 한다. icq_cache가
 * 있었다면 rcu_barrier()로 진행 중인 RCU 콜백(icq 해제 등)이 모두 끝나길
 * 기다린 뒤 kmem_cache_destroy()로 캐시를 파괴한다 - icq는 RCU로 관리되므로
 * 이 대기 없이 바로 destroy하면 use-after-free 위험이 있다.
 * 실행 컨텍스트: 모듈 __exit 함수(예: deadline_exit) - rmmod 시 단일 스레드
 *   컨텍스트지만 rcu_barrier()로 다른 CPU의 RCU 콜백 완료를 기다리므로 블로킹.
 * caller: 각 스케줄러 모듈의 __exit 함수.
 * callee: rcu_barrier(), kmem_cache_destroy().
 * 에러 처리: 없음(void) - 반드시 성공한다고 가정.
 *
 * 호출 체인:
 *   <스케줄러>_exit() → [elv_unregister]
 */

/*
 * io scheduler sysfs switching
 */
ssize_t elv_iosched_show(struct gendisk *disk, char *page);
/*
 * [한국어]
 * elv_iosched_show - /sys/block/<disk>/queue/scheduler 읽기 처리
 *
 * @disk: 대상 gendisk(디스크 디바이스)
 * @page: 결과 문자열을 쓸 출력 버퍼(PAGE_SIZE 크기)
 * @return: page에 쓴 바이트 수
 *
 * q->elevator_lock을 쥐고 현재 선택된 스케줄러("none" 포함)를 확인한 뒤,
 * elv_list_lock 하에서 등록된 모든 스케줄러 이름을 공백으로 구분해 나열하며
 * 현재 선택된 것만 대괄호로 감싼다(예: "[none] mq-deadline bfq kyber"). 이
 * sysfs 파일은 블록 디바이스의 /sys/block/<disk>/queue/scheduler에 해당하며,
 * elv_fs_entry 기반의 iosched/ 하위 파일들과는 별개의(더 상위) 인터페이스다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * caller: block/blk-sysfs.c의 queue_attr(scheduler show 콜백).
 * callee: 없음(락과 리스트 순회, sprintf만 사용).
 * 에러 처리: 없음 - 항상 성공(버퍼 오버플로는 PAGE_SIZE 여유로 방지됨을 전제).
 *
 * 호출 체인:
 *   sysfs read → [elv_iosched_show]
 */
ssize_t elv_iosched_store(struct gendisk *disk, const char *page, size_t count);
/*
 * [한국어]
 * elv_iosched_store - /sys/block/<disk>/queue/scheduler 쓰기 처리(스케줄러 전환)
 *
 * @disk:  대상 gendisk
 * @page:  사용자 입력 버퍼("none", "mq-deadline", "bfq" 등)
 * @count: 입력 길이
 * @return: 성공 시 소비한 바이트 수(count), 실패 시 음수 errno
 *   (-ENOENT: 큐 미등록/전환 금지, -EBUSY: update_nr_hwq_lock 획득 실패)
 *
 * 큐가 등록되어 있는지 확인한 뒤, 입력 문자열을 정제(strstrip)하고
 * elv_iosched_load_module()로 아직 로드되지 않은 스케줄러 모듈을
 * request_module()로 자동 로드한다. 이어서 set->update_nr_hwq_lock을
 * down_read_trylock()으로 획득(실패 시 -EBUSY - kernfs active reference와의
 * 순환 락 의존성을 피하기 위한 trylock)한 뒤 elevator_change()를 호출해
 * 실제 전환을 수행한다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * caller: block/blk-sysfs.c의 queue_attr(scheduler store 콜백).
 * callee: elv_iosched_load_module(), elevator_find_get(), elevator_change().
 * 에러 처리: update_nr_hwq_lock을 못 얻으면 즉시 -EBUSY; elevator_change()
 *   실패 시 그 음수 errno를 그대로 반환. 성공 시에도 ctx.type 참조는 반드시
 *   elevator_put()으로 반환(out: 레이블에서 처리).
 *
 * 호출 체인:
 *   sysfs write → [elv_iosched_store] → elevator_change()
 */

extern bool elv_bio_merge_ok(struct request *, struct bio *);
/*
 * [한국어]
 * elv_bio_merge_ok - request와 bio의 병합이 안전한지 1차 확인
 *
 * @rq:  병합 대상 request
 * @bio: 새로 도착한 bio
 * @return: true = 병합해도 안전, false = 병합 불가
 *
 * 두 단계로 확인한다: (1) blk_rq_merge_ok()로 장치/방향/최대 세그먼트 수 등
 * 하드웨어·큐 제약을 검사, (2) elv_iosched_allow_bio_merge()로 스케줄러
 * 정책(ops.allow_merge)까지 재확인. 두 단계 모두 통과해야 실제
 * blk_try_merge()/blk_attempt_req_merge()로 이어질 수 있다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
 * caller: elv_merge()의 one-hit 캐시 검사, elv_rqhash_find() 후보 필터링 등
 *   병합 후보를 최종 확정하기 전 안전성 검사가 필요한 모든 지점.
 * callee: blk_rq_merge_ok(), elv_iosched_allow_bio_merge() → (선택적)
 *   e->type->ops.allow_merge.
 * 에러 처리: false 반환은 에러가 아니라 "이 후보와는 병합 불가" 정상 판정 -
 *   호출자가 다음 후보를 계속 찾거나 새 request를 만든다.
 *
 * 호출 체인:
 *   elv_merge() 등 병합 경로 → [elv_bio_merge_ok] → (선택) ops.allow_merge
 */
struct elevator_queue *elevator_alloc(struct request_queue *,
		struct elevator_type *, struct elevator_resources *);
/*
 * [한국어]
 * elevator_alloc - request_queue에 부착할 elevator_queue 인스턴스를 할당/초기화
 *
 * @q:   이 스케줄러 인스턴스가 부착될 request_queue
 * @e:   사용할 elevator_type(mq-deadline/bfq/kyber)
 * @res: 미리 준비된 elevator_resources(tag 풀 et + 사설 데이터 data)
 * @return: 초기화된 elevator_queue 포인터, 메모리 부족 시 NULL
 *
 * kzalloc_node(q->node)로 elevator_queue를 큐와 동일한 NUMA 노드에 할당한 뒤,
 * __elevator_get(e)로 모듈 참조를 늘리고 eq->type = e, kobject_init(),
 * mutex_init(&eq->sysfs_lock), hash_init(eq->hash), eq->et = res->et,
 * eq->elevator_data = res->data 순으로 필드를 채운다. 이 시점에는 아직
 * ops.init_sched()가 호출되지 않았고 sysfs에도 등록되지 않은 "생성 직후"
 * 상태다.
 * 실행 컨텍스트: 스케줄러 전환 트랜잭션(프로세스 컨텍스트), 큐 freeze 상태.
 * caller: block/blk-mq-sched.c의 blk_mq_init_sched() - elevator_switch()가
 *   새 스케줄러를 붙일 때 첫 단계로 호출.
 * callee: kzalloc_node(), __elevator_get(), kobject_init(), mutex_init(),
 *   hash_init().
 * 에러 처리: kzalloc_node() 실패 시 NULL 반환 - blk_mq_init_sched()가
 *   -ENOMEM으로 전파.
 *
 * 호출 체인:
 *   blk_mq_init_sched() → [elevator_alloc]
 */

/*
 * Helper functions.
 */
extern struct request *elv_rb_former_request(struct request_queue *, struct request *);
/*
 * [한국어]
 * elv_rb_former_request - LBA 기준 RB-tree에서 @rq 바로 앞의 request 반환
 *
 * @q:  request_queue(미사용 매개변수 - ops.former_request 시그니처 일관성 유지용)
 * @rq: 기준 request(자신의 rb_node가 이미 어떤 rb_root에 연결되어 있어야 함)
 * @return: LBA가 바로 작은 request, 없으면 NULL(가장 작은 LBA인 경우)
 *
 * rb_prev(&rq->rb_node)로 O(log N) 탐색 후 rb_entry_rq()로 request 포인터로
 * 변환한다. mq-deadline과 BFQ가 이 함수를 ops.former_request로 직접 등록해
 * 재사용한다(공용 헬퍼) - front-merge 후보 탐색이나 dispatch 순서 역추적에
 * 쓰인다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: elv_former_request()(ops.former_request로 등록된 경우 그 자체가
 *   호출 대상).
 * callee: rb_prev(), rb_entry_rq().
 * 에러 처리: 이전 노드가 없으면 NULL(정상 - 가장 작은 LBA에는 이전이 없음).
 *
 * 호출 체인:
 *   elv_former_request() → 스케줄러 ops.former_request → [elv_rb_former_request]
 */
extern struct request *elv_rb_latter_request(struct request_queue *, struct request *);
/*
 * [한국어]
 * elv_rb_latter_request - LBA 기준 RB-tree에서 @rq 바로 뒤의 request 반환
 *
 * @q:  request_queue(미사용 매개변수)
 * @rq: 기준 request
 * @return: LBA가 바로 큰 request, 없으면 NULL(가장 큰 LBA인 경우)
 *
 * rb_next(&rq->rb_node)로 O(log N) 탐색 후 rb_entry_rq()로 변환한다.
 * mq-deadline과 BFQ가 ops.next_request로 직접 등록해 재사용한다 - back-merge
 * 후보 탐색이나 순차 dispatch 방향 추적에 쓰인다.
 * 실행 컨텍스트: bio 제출/병합 프로세스 컨텍스트.
 * caller: elv_latter_request()(ops.next_request로 등록된 경우).
 * callee: rb_next(), rb_entry_rq().
 * 에러 처리: 다음 노드가 없으면 NULL(정상 - 가장 큰 LBA에는 다음이 없음).
 *
 * 호출 체인:
 *   elv_latter_request() → 스케줄러 ops.next_request → [elv_rb_latter_request]
 */

/*
 * rb support functions.
 */
extern void elv_rb_add(struct rb_root *, struct request *);
/*
 * [한국어]
 * elv_rb_add - request를 시작 LBA(섹터) 기준으로 RB-tree에 삽입
 *
 * @root: 스케줄러 내부의 RB-tree 루트(예: BFQ/mq-deadline의 정렬 트리)
 * @rq:   삽입할 request
 * @return: 없음(void)
 *
 * 표준 이진 탐색 트리 삽입 - blk_rq_pos(rq)를 기준으로 더 작으면 왼쪽,
 * 크거나 같으면 오른쪽으로 내려가며 삽입 위치를 찾은 뒤 rb_link_node() +
 * rb_insert_color()로 삽입 및 재균형(rebalance)한다. 삽입 후에는 항상 LBA
 * 오름차순으로 in-order 순회가 가능하다.
 * 실행 컨텍스트: bio 제출/삽입 프로세스 컨텍스트.
 * caller: mq-deadline dd_insert_requests, BFQ bfq_insert_requests 등
 *   스케줄러의 insert_requests 콜백 내부.
 * callee: rb_entry(), rb_link_node(), rb_insert_color()(linux/rbtree.h).
 * 에러 처리: 없음(void) - 삽입 실패 개념이 없는 자료구조 연산.
 *
 * 호출 체인:
 *   스케줄러 ops.insert_requests 구현 → [elv_rb_add]
 */
extern void elv_rb_del(struct rb_root *, struct request *);
/*
 * [한국어]
 * elv_rb_del - RB-tree에서 request를 제거
 *
 * @root: 스케줄러 내부의 RB-tree 루트
 * @rq:   제거할 request(현재 트리에 연결되어 있어야 함)
 * @return: 없음(void)
 *
 * BUG_ON(RB_EMPTY_NODE(&rq->rb_node))으로 이미 빠진 노드를 다시 제거하는
 * 프로그래밍 오류를 사전 차단한 뒤, rb_erase()로 제거·재균형하고
 * RB_CLEAR_NODE()로 rq->rb_node를 "트리 밖" 상태로 명시한다 - 이후
 * RB_EMPTY_NODE() 검사가 정확히 동작하도록 하는 필수 후처리다.
 * 실행 컨텍스트: bio 제출/dispatch 프로세스 컨텍스트.
 * caller: 스케줄러의 dispatch_request(디스패치되어 트리를 떠날 때) 또는
 *   requests_merged(병합되어 사라질 때) 콜백 내부.
 * callee: rb_erase(), RB_CLEAR_NODE()(linux/rbtree.h).
 * 에러 처리: 이미 빠진 노드를 넘기면 BUG_ON으로 커널 패닉 - 호출자가 상태를
 *   정확히 추적해야 하는 강한 사전조건.
 *
 * 호출 체인:
 *   스케줄러 ops.dispatch_request / ops.requests_merged → [elv_rb_del]
 */
extern struct request *elv_rb_find(struct rb_root *, sector_t);
/*
 * [한국어]
 * elv_rb_find - 특정 LBA(섹터)에서 "정확히" 시작하는 request를 RB-tree에서 탐색
 *
 * @root:   스케줄러 내부의 RB-tree 루트
 * @sector: 찾으려는 시작 LBA
 * @return: 정확히 @sector에서 시작하는 request, 없으면 NULL
 *
 * 표준 이진 탐색 - 찾는 섹터가 현재 노드보다 작으면 왼쪽, 크면 오른쪽으로
 * 내려가며 O(log N)에 탐색한다. back-merge 후보는 끝 섹터 기준 해시
 * (elv_rqhash_find)로 찾지만, front-merge 후보(뒤쪽이 @sector인 request)는
 * 이 함수로 찾는다는 점이 elv_rqhash_find와의 역할 분담이다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트.
 * caller: mq-deadline dd_request_merge, BFQ bfq_request_merge(둘 다
 *   ops.request_merge 구현) - bio의 시작 섹터로 front-merge 후보 탐색.
 * callee: rb_entry(), blk_rq_pos().
 * 에러 처리: 일치하는 노드가 없으면 NULL(정상 - front-merge 불가 판정으로 이어짐).
 *
 * 호출 체인:
 *   스케줄러 ops.request_merge → [elv_rb_find]
 */

/*
 * Insertion selection
 */
/*
 * [한국어] 아래 ELEVATOR_INSERT_* 매크로들은 과거 single-queue(레거시,
 * blk-mq 이전) elevator 시절 elv_insert()의 삽입 방식 선택자로 쓰이던
 * 값이다. 현재 트리의 blk-mq 전용 elevator_mq_ops.insert_requests는 이
 * 매크로들이 아니라 block/blk-mq.h에 정의된 `typedef unsigned int __bitwise
 * blk_insert_t`와 BLK_MQ_INSERT_AT_HEAD 같은 별도 플래그를 사용한다(실제로
 * block 디렉토리의 어떤 .c 파일에서도 ELEVATOR_INSERT_* 매크로를 참조하는 코드는 없다 -
 * grep으로 확인). 즉 이 6개 매크로는 blk-mq 코드 경로에서는 사실상 죽은
 * (dead) 헤더 상수이며, 헤더 정리(cleanup)에서 아직 제거되지 않은 레거시
 * 흔적으로 이해해야 한다 - 새 코드를 작성한다면 blk_insert_t 쪽을 참고해야
 * 한다.
 */
#define ELEVATOR_INSERT_FRONT	1
/* [한국어] (레거시) 디스패치 큐의 맨 앞에 삽입 - 우선 처리 의도. */
#define ELEVATOR_INSERT_BACK	2
/* [한국어] (레거시) 디스패치 큐의 맨 뒤에 삽입 - 일반적인 순서 유지 삽입. */
#define ELEVATOR_INSERT_SORT	3
/* [한국어] (레거시) 정렬 기준(LBA/시간)에 따라 적절한 위치에 삽입. */
#define ELEVATOR_INSERT_REQUEUE	4
/* [한국어] (레거시) requeue(재시도) 경로 전용 삽입 방식. */
#define ELEVATOR_INSERT_FLUSH	5
/* [한국어] (레거시) flush(캐시 플러시) 요청 전용 삽입 방식. */
#define ELEVATOR_INSERT_SORT_MERGE	6
/* [한국어] (레거시) 정렬 삽입과 동시에 병합까지 시도하는 조합 방식. */

#define rb_entry_rq(node)	rb_entry((node), struct request, rb_node)
/*
 * [한국어]
 * rb_entry_rq - rb_node 포인터로부터 그것을 포함하는 struct request 포인터를 계산
 *
 * @node: struct request.rb_node를 가리키는 rb_node 포인터
 * @return: container_of 방식으로 역산된 struct request 포인터(rb_entry의 alias)
 *
 * rb_entry()(container_of의 rb_node 특화 버전)를 request 타입에 맞게 감싼
 * 편의 매크로다. RB-tree 순회 중에는 rb_node만 얻을 수 있으므로, 실제
 * request 필드(섹터, bio 등)에 접근하려면 이 매크로로 반드시 되짚어 와야
 * 한다.
 * 실행 컨텍스트: 매크로이므로 별도 컨텍스트 제약 없음 - 호출 지점의 컨텍스트를
 *   그대로 따름.
 * caller: elv_rb_former_request/elv_rb_latter_request/elv_rb_find, 그리고
 *   mq-deadline.c/bfq-iosched.c의 자체 rb-tree 순회 코드(예:
 *   mq-deadline.c:210, bfq-iosched.c:1952/1955/1959).
 * callee: rb_entry()(container_of 매크로의 특수화).
 * 에러 처리: 없음(매크로 - node가 NULL이면 안 된다는 사전조건은 호출자 책임).
 */

#define rq_entry_fifo(ptr)	list_entry((ptr), struct request, queuelist)
/*
 * [한국어]
 * rq_entry_fifo - list_head 포인터로부터 그것을 포함하는 struct request 포인터를 계산
 *
 * @ptr: struct request.queuelist를 가리키는 list_head 포인터
 * @return: container_of 방식으로 역산된 struct request 포인터
 *
 * FIFO(도착 순서) 큐는 struct request.queuelist(list_head)로 연결되므로,
 * 이 매크로로 리스트 노드를 request로 되짚는다. mq-deadline이 read/write
 * 별 fifo_list의 .next를 이 매크로로 변환해 "가장 오래 기다린 request"를
 * 얻는다(mq-deadline.c:365, 382). BFQ도 bfq_check_fifo()에서 유사하게
 * bfqq->fifo.next를 이 매크로로 변환한다(bfq-iosched.c:1904).
 * 실행 컨텍스트: 매크로 - 호출 지점의 컨텍스트를 따름.
 * caller: dd_dispatch_request/dd_next_request 계열, bfq_check_fifo().
 * callee: list_entry()(container_of 매크로의 특수화).
 * 에러 처리: 없음(매크로).
 */
#define rq_fifo_clear(rq)	list_del_init(&(rq)->queuelist)
/*
 * [한국어]
 * rq_fifo_clear - request를 FIFO 큐(queuelist)에서 제거
 *
 * @rq: 제거할 request
 * @return: 없음(void 매크로)
 *
 * list_del_init()으로 rq->queuelist를 리스트에서 떼어내고 자기 자신을
 * 가리키는 빈 리스트 상태로 초기화한다(재사용/재삽입 안전). request가
 * dispatch되어 FIFO 대기열을 떠날 때 반드시 호출해야 이후 has_work()/
 * dispatch_request() 판단이 정확해진다.
 * 실행 컨텍스트: 매크로 - 호출 지점의 컨텍스트를 따름.
 * caller: mq-deadline/kyber의 dispatch_request 구현이 request를 뽑아낼 때.
 * callee: list_del_init()(linux/list.h).
 * 에러 처리: 없음(매크로) - 이미 빈 리스트에 호출해도 list_del_init은
 *   멱등적으로 안전하게 동작.
 */

void blk_mq_sched_reg_debugfs(struct request_queue *q);
/*
 * [한국어]
 * blk_mq_sched_reg_debugfs - scheduler debugfs 항목 등록
 *
 * @q: 대상 request_queue
 * @return: 없음(void)
 *
 * /sys/kernel/debug/block/<disk>/sched/ 하위에 큐 전체 debugfs
 * (blk_mq_debugfs_register_sched)와 각 hctx별 debugfs
 * (blk_mq_debugfs_register_sched_hctx, queue_for_each_hw_ctx로 순회)를
 * 등록한다. blk_debugfs_lock()으로 등록 절차 전체를 직렬화해 동시
 * 등록/해제 race를 막는다.
 * 실행 컨텍스트: 스케줄러 전환 성공 후 sysfs 등록 단계(프로세스 컨텍스트).
 * caller: block/elevator.c의 elv_register_queue() - 스케줄러가 초기화되어
 *   "이제 debugfs로 노출해도 안전한" 시점.
 * callee: blk_debugfs_lock/unlock(), blk_mq_debugfs_register_sched(),
 *   blk_mq_debugfs_register_sched_hctx().
 * 에러 처리: 없음(void) - debugfs는 진단용 부가 기능이라 실패해도 IO 경로에
 *   영향 없음(디버그 빌드 전용 기능이기도 함).
 *
 * 호출 체인:
 *   elv_register_queue() → [blk_mq_sched_reg_debugfs]
 */
void blk_mq_sched_unreg_debugfs(struct request_queue *q);
/*
 * [한국어]
 * blk_mq_sched_unreg_debugfs - scheduler debugfs 항목 제거
 *
 * @q: 대상 request_queue
 * @return: 없음(void)
 *
 * blk_mq_sched_reg_debugfs의 역순 - hctx별 debugfs를 먼저 해제
 * (blk_mq_debugfs_unregister_sched_hctx)한 뒤 큐 수준 debugfs를 해제
 * (blk_mq_debugfs_unregister_sched)한다. nomemsave 변형의 락(
 * blk_debugfs_lock_nomemsave)을 쓰는 이유는 이 경로가 스케줄러 해제
 * 중(메모리 회수 압박이 있을 수 있는 상황)에도 안전하게 호출돼야 하기
 * 때문이다(GFP 할당을 하지 않는 락 변형).
 * 실행 컨텍스트: 스케줄러 해제 경로(elv_unregister_queue()), 프로세스 컨텍스트.
 * caller: block/elevator.c의 elv_unregister_queue() - sysfs kobject를
 *   지우기 직전.
 * callee: blk_debugfs_lock_nomemsave/unlock_nomemrestore(),
 *   blk_mq_debugfs_unregister_sched_hctx(), blk_mq_debugfs_unregister_sched().
 * 에러 처리: 없음(void).
 *
 * 호출 체인:
 *   elv_unregister_queue() → [blk_mq_sched_unreg_debugfs]
 */

#endif /* _ELEVATOR_H */
