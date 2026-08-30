/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] 블록 계층 요청 QoS(Quality of Service) 플러그인 프레임워크 헤더 (blk-rq-qos.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/blk-rq-qos.c가 구현하는 rq_qos 플러그인 프레임워크의 "공개 API 표면"이다.
 * struct rq_qos/rq_qos_ops/rq_wait/rq_depth 등 핵심 자료구조 정의와, 그 자료구조를 조작하는
 * inline 래퍼 함수(rq_qos_throttle/track/merge/issue/done/requeue/done_bio/cleanup/
 * queue_depth_changed/exit)를 제공한다. 이 프레임워크 자체는 어떤 정책도 구현하지 않으며,
 * request_queue마다 연결 리스트로 매달린 rq_qos 객체들의 ops vtable을 순서대로 호출해주는
 * "배전반(dispatch board)" 역할만 한다. 실제 정책(쓰기 대역폭 제한, cgroup 지연 목표,
 * 비용 기반 제어)은 이 헤더가 선언한 rq_qos_ops를 채워 넣는 3개의 구현체
 * (block/blk-wbt.c, block/blk-iolatency.c, block/blk-iocost.c)에 들어있다.
 * static inline 래퍼들은 매 bio/request 마다 호출되는 매우 뜨거운 경로(hot path)이므로,
 * QUEUE_FLAG_QOS_ENABLED 비트와 q->rq_qos 포인터를 먼저 검사해 QoS가 전혀 등록되지 않은
 * 평범한 블록 디바이스에서는 함수 호출조차 발생하지 않도록 설계되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * bio가 request로 변환되어 디바이스에 발행되고 완료되는 전체 흐름 중, 이 헤더가 선언하는
 * 함수들이 실제로 끼어드는 지점은 다음과 같다(모두 block/blk-rq-qos.c의 __rq_qos_*() 구현과
 * 1:1 대응):
 *   [제출]   blk_mq_submit_bio()
 *              → rq_qos_throttle()        bio→request 변환 "전" 대역폭/지연 게이트
 *              → blk_mq_get_request()
 *                  → rq_qos_track()       할당된 request에 bio 컨텍스트를 매핑
 *   [병합]   blk_attempt_bio_merge()  → rq_qos_merge()   인접 bio 병합 시 비용 재계산
 *   [발행]   blk_mq_dispatch_rq_list() / blk_mq_try_issue_directly()
 *              → rq_qos_issue()           드라이버 ->queue_rq() 직전 마지막 통보
 *   [완료]   blk_mq_complete_request() → rq_qos_done()   request 단위 정산(passthrough 제외)
 *   [완료]   bio_endio()               → rq_qos_done_bio()  bio 단위 정산(stacked 장치 고려)
 *   [재큐]   blk_mq_requeue_request()  → rq_qos_requeue()  자원 부족 등으로 되돌아갈 때 반납
 *   [오류]   blk_mq_end_request(오류)  → rq_qos_cleanup()  아직 발행되지 못한 bio의 상태 롤백
 *   [설정변경] nr_requests sysfs 변경  → rq_qos_queue_depth_changed()  최대 depth 재계산
 *   [해제]   del_gendisk() 등          → rq_qos_exit()     체인의 모든 정책을 순서대로 해제
 * throttle/track/merge는 bio를 제출하는 프로세스 컨텍스트에서 실행되며 필요하면 잠들 수 있다
 * (rq_qos_wait() 참고). done/done_bio는 대개 인터럽트/softirq(완료 경로)에서 실행되므로
 * 절대 블로킹할 수 없다. issue는 디스패치 워커(kblockd) 또는 제출 태스크에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - include/linux/blkdev.h: struct request_queue(->rq_qos, ->queue_flags, ->rq_qos_mutex),
 *     struct gendisk(->queue). 이 프레임워크가 훅을 매다는 "그릇"이다.
 *   - include/linux/blk-mq.h: struct request의 전체 정의(105번째 줄 부근)와
 *     blk_rq_is_passthrough(). rq_qos_ops의 track/merge/issue/requeue/done 콜백이 모두
 *     struct request *를 매개변수로 받으므로 완전한 타입 정의가 필요하다.
 *   - include/linux/blk_types.h: struct bio, BIO_QOS_THROTTLED/BIO_QOS_MERGED 플래그 비트.
 *     bio_flagged()/bio_set_flag()로 이 비트를 검사·설정한다.
 *   - block/blk-mq-debugfs.h: struct blk_mq_debugfs_attr와 CONFIG_BLK_DEBUG_FS OFF 시의
 *     no-op 스텁. rq_qos_ops.debugfs_attrs 필드의 타입 출처.
 *   - block/blk-rq-qos.c: 이 헤더가 선언만 하는 모든 비-inline 함수
 *     (rq_qos_add/del/exit/wait, rq_wait_inc_below, rq_depth_scale_up/down/calc_max_depth,
 *     __rq_qos_*())의 실제 구현이 위치한다. 이 헤더와 반드시 함께 읽어야 전체 그림이 맞는다.
 * 이 헤더에 의존하는 모듈(정책 구현체 — 실제로 grep하여 확인한 콜백 구현 현황):
 *   - block/blk-wbt.c: RQ_QOS_WBT. wbt_rqos_ops = { .throttle=wbt_wait, .issue=wbt_issue,
 *     .track=wbt_track, .requeue=wbt_requeue, .done=wbt_done, .cleanup=wbt_cleanup,
 *     .queue_depth_changed=wbt_queue_depth_changed, .exit=wbt_exit,
 *     .debugfs_attrs=wbt_debugfs_attrs }. struct rq_wait(그룹당 4개 배열)와 struct rq_depth를
 *     모두 사용하는 유일한 구현체.
 *   - block/blk-iolatency.c: RQ_QOS_LATENCY. blkcg_iolatency_ops = { .throttle=
 *     blkcg_iolatency_throttle, .done_bio=blkcg_iolatency_done_bio, .exit=
 *     blkcg_iolatency_exit }. rq_qos_wait()과 struct rq_wait(iolatency_grp당 1개)는 쓰지만
 *     rq_depth는 쓰지 않고 자체 스케일 쿠키(iolat->scale_cookie)로 depth를 조절한다.
 *   - block/blk-iocost.c: RQ_QOS_COST. ioc_rqos_ops = { .throttle=ioc_rqos_throttle,
 *     .merge=ioc_rqos_merge, .done_bio=ioc_rqos_done_bio, .done=ioc_rqos_done,
 *     .queue_depth_changed=ioc_rqos_queue_depth_changed, .exit=ioc_rqos_exit }. rq_wait도
 *     rq_depth도 쓰지 않고, vtime(가상 시간) 기반 비용 모델로 자체 예산을 관리한다.
 * 데이터 흐름: bio/request 포인터가 각 콜백에 그대로 전달되며, 정책 구현체는 자신의
 * private 구조체(struct rq_wb, struct blk_iolatency, struct ioc)를 rq_qos_ops를 담은
 * struct rq_qos를 통해 container_of 패턴(RQWB(), rqos_to_ioc() 등)으로 역참조한다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct rq_qos       — request_queue에 매달리는 정책 인스턴스 1개. ops/disk/id/next로 구성.
 * struct rq_qos_ops   — 정책이 구현할 콜백 vtable. 정책마다 부분집합만 채운다(위 표 참고).
 * struct rq_wait      — inflight 카운터 + waitqueue. wbt/iolatency의 in-flight 한도 관리.
 * struct rq_depth     — scale_step 기반 동적 depth 계산 상태. 현재 wbt만 사용.
 * rq_qos_throttle()/track()/merge()/issue()/done()/done_bio()/requeue()/cleanup()/
 *   queue_depth_changed() — QUEUE_FLAG_QOS_ENABLED 검사 후 __rq_qos_*()(블록 계층 각 지점에서
 *   호출되는 hot-path 게이트)로 위임하는 static inline 래퍼들.
 * rq_qos_add()/rq_qos_del()/rq_qos_exit() — 정책 등록/해제/전체 해제(모두 block/blk-rq-qos.c).
 * rq_qos_wait()       — budget 부족 시 태스크를 재우는 공용 대기 루틴(wbt/iolatency 사용).
 */
#ifndef RQ_QOS_H
#define RQ_QOS_H	/* [한국어] 인클루드 가드. 바로 위 #ifndef 와 짝을 이뤄, 이 헤더가 한 번역 단위에
				 * 여러 번 포함돼도 내용이 한 번만 펼쳐지게 한다(중복 정의 오류 방지). */

#include <linux/kernel.h>	/* [한국어] bool/NULL 등 커널 전역 기본 타입·매크로 — 이 헤더가 직접 쓰는
				 * 매크로는 없지만 관례적으로 최상단에 포함해 다른 헤더들이 기대하는
				 * 기본 정의(예: container_of 계열)가 갖춰지도록 보장한다 */
#include <linux/blkdev.h>	/* [한국어] struct request_queue(->rq_qos, ->queue_flags,
				 * ->rq_qos_mutex)와 struct gendisk(->queue) 정의 —
				 * 이 프레임워크가 QoS 체인을 매다는 그릇 자체가 여기 있다 */
#include <linux/blk_types.h>	/* [한국어] struct bio와 BIO_QOS_THROTTLED/BIO_QOS_MERGED
				 * 플래그 비트 정의 — rq_qos_throttle()/merge()가 이 비트를 세워
				 * 이후 rq_qos_done_bio()가 어떤 bio를 통보해야 하는지 판단한다 */
#include <linux/atomic.h>	/* [한국어] atomic_t, atomic_read/atomic_set/atomic_try_cmpxchg —
				 * struct rq_wait.inflight 카운터를 락 없이 증감시키기 위해 필요 */
#include <linux/wait.h>		/* [한국어] wait_queue_head_t, init_waitqueue_head(),
				 * wait_queue_entry — struct rq_wait.wait 필드의 타입 출처이자
				 * rq_qos_wait()가 태스크를 재우고 깨우는 데 사용하는 기반 API */
#include <linux/blk-mq.h>	/* [한국어] struct request의 완전한 정의(105번째 줄 부근)와
				 * blk_rq_is_passthrough() — rq_qos_ops의 track/merge/issue/
				 * requeue/done 콜백 시그니처가 struct request *를 요구하므로,
				 * 전방 선언이 아닌 완전한 타입 정의가 이 시점에 있어야 한다 */

#include "blk-mq-debugfs.h"	/* [한국어] struct blk_mq_debugfs_attr 타입과, CONFIG_BLK_DEBUG_FS가
				 * 꺼졌을 때도 안전하게 호출 가능한 debugfs 등록 함수의 no-op 스텁.
				 * rq_qos_ops.debugfs_attrs 필드가 이 타입을 가리킨다 */

/*
 * [한국어] 이 헤더는 blk-mq(멀티큐 블록 계층) 위에서만 동작한다. rq_qos_add()의 구현
 * (block/blk-rq-qos.c)이 blk_mq_freeze_queue()/blk_mq_unfreeze_queue()로 큐를 동결한
 * 뒤에만 체인을 수정하는 것에서 알 수 있듯, 레거시 단일 큐(request_fn 기반) 드라이버는
 * 대상이 아니다. block/blk-wbt.c의 blk_wbt_init(), block/blk-iolatency.c의
 * blk_iolatency_init(), block/blk-iocost.c의 blk_iocost_init()이 각각 disk->queue를
 * 대상으로 rq_qos_add()를 호출해 이 프레임워크에 자신을 등록하는 것이 실제 흐름이다.
 */

struct blk_mq_debugfs_attr;	/* [한국어] block/blk-mq-debugfs.h가 정의하는 debugfs 파일 속성
				 * 테이블 엔트리 타입의 전방 선언. rq_qos_ops.debugfs_attrs
				 * 필드가 이 타입의 포인터를 가리키기 위해서만 필요하며, 이 헤더는
				 * 그 구조체의 내부 필드를 직접 사용하지 않으므로 완전한 정의
				 * 대신 전방 선언만으로 충분하다(포인터 크기만 알면 됨) */

/*
 * [한국어]
 * enum rq_qos_id - request_queue 하나에 동시에 등록될 수 있는 QoS 정책의 식별자
 *
 * struct rq_qos.id 필드에 저장되며, rq_qos_id()가 q->rq_qos 연결 리스트를 순회하며
 * 원하는 정책을 찾을 때 비교 키로 쓰인다. rq_qos_add()는 동일 id가 이미 등록되어 있으면
 * -EBUSY를 반환해 같은 정책이 한 큐에 중복 등록되는 것을 막는다.
 */
enum rq_qos_id {
	RQ_QOS_WBT,
	/* [한국어] block/blk-wbt.c(Writeback Throttling)가 사용하는 식별자.
	 * 설정자: blk_wbt_init() → rq_qos_add(&rwb->rqos, disk, RQ_QOS_WBT, &wbt_rqos_ops).
	 * 읽는 자: wbt_rq_qos() = rq_qos_id(q, RQ_QOS_WBT)로 큐에서 WBT 인스턴스를 찾을 때
	 *   (blk-sysfs.c의 sysfs 속성, blk-mq-debugfs.c의 debugfs 파일 등에서 사용).
	 * 값의 의미: 백그라운드 쓰기(WRITE)가 몰릴 때 동기(sync) read의 지연이 커지는 것을
	 *   막기 위해 in-flight write 수를 소프트웨어적으로 제한하는 정책.
	 * 동기화: 별도 락 불필요 — enum 상수 값 자체는 불변이며 비교에만 쓰인다. */

	RQ_QOS_LATENCY,
	/* [한국어] block/blk-iolatency.c(io.latency cgroup 컨트롤러)가 사용하는 식별자.
	 * 설정자: blk_iolatency_init() → rq_qos_add(&blkiolat->rqos, disk, RQ_QOS_LATENCY,
	 *   &blkcg_iolatency_ops).
	 * 읽는 자: iolat_rq_qos() = rq_qos_id(q, RQ_QOS_LATENCY)로 큐에서 iolatency 인스턴스를
	 *   찾을 때(blkcg 정책 활성화 확인, io.latency 파일 파서 등에서 사용).
	 * 값의 의미: cgroup별 목표 지연(latency target, io.latency 설정값)을 초과하는 상위
	 *   cgroup의 I/O를 제한해 하위 우선순위 cgroup의 지연을 보장하는 정책.
	 * 동기화: RQ_QOS_WBT와 동일하게 비교 전용 상수. */

	RQ_QOS_COST,
	/* [한국어] block/blk-iocost.c(비용 기반 cgroup I/O 컨트롤러, io.cost)가 사용하는
	 *   식별자.
	 * 설정자: blk_iocost_init() → rq_qos_add(&ioc->rqos, disk, RQ_QOS_COST, &ioc_rqos_ops).
	 * 읽는 자: q_to_ioc() 내부에서 rqos_to_ioc(rq_qos_id(q, RQ_QOS_COST))로 큐에서
	 *   iocost 컨트롤러 구조체(struct ioc)를 역참조할 때 사용.
	 * 값의 의미: I/O 하나하나에 장치 모델 기반 "가상 비용(vtime cost)"을 매겨, cgroup
	 *   계층 구조에 따라 예산(budget)을 배분하는 정책. rq_wait/rq_depth를 쓰지 않는
	 *   유일한 구현체다.
	 * 동기화: RQ_QOS_WBT와 동일하게 비교 전용 상수. */
};

/*
 * [한국어]
 * struct rq_wait - "지금 몇 개가 in-flight인지"와 "한도를 넘으면 누가 기다릴지"를 함께
 *                  관리하는 세마포어 유사 구조체
 *
 * wbt(그룹별로 WBT_NUM_RWQ개 배열)와 iolatency(iolatency_grp당 1개)가 임베드해서 쓴다.
 * inflight 카운터를 원자적으로 증가시키되 상한(limit, 보통 rq_depth->max_depth 또는
 * 정책별 파라미터)을 넘지 않게 하는 rq_wait_inc_below()와 짝을 이루며, 한도 초과로
 * 증가에 실패한 태스크는 rq_qos_wait()를 통해 이 구조체의 waitqueue에서 잠든다.
 * iocost는 이 구조체를 쓰지 않고 vtime 기반의 자체 디베이트(debt) 메커니즘을 쓴다.
 */
struct rq_wait {
	wait_queue_head_t wait;
	/* [한국어] budget(=inflight 여유)이 없어 rq_qos_wait()로 잠든 태스크들의 대기 큐.
	 * 설정자: rq_wait_init()이 init_waitqueue_head()로 최초 1회 초기화.
	 * 읽는 자/쓰는 자: rq_qos_wait()가 prepare_to_wait_exclusive(&rqw->wait, ...)로
	 *   자신을 이 큐에 등록하고, 완료 경로(wbt_rqw_done() 등, ops->done/done_bio 내부)가
	 *   wake_up_nr(&rqw->wait, 1)로 대기자 중 정확히 하나를 깨운다. rq_qos_wake_function()
	 *   이 커스텀 wake 함수로 등록되어, 깨우는 시점에 budget 획득까지 시도한다
	 *   (block/blk-rq-qos.c 참고).
	 * 값 범위: 커널 wait_queue_head_t의 표준 상태 — 비어있거나(대기자 없음) 1개 이상의
	 *   wait_queue_entry가 연결된 상태.
	 * 동기화: 내부 spinlock(wait.lock)으로 보호되며, prepare_to_wait_exclusive/
	 *   finish_wait/wake_up_nr이 모두 이 락을 통해 리스트를 조작하므로 호출자가 별도로
	 *   잠글 필요는 없다. */

	atomic_t inflight;
	/* [한국어] 현재 이 rq_wait를 통해 "허가받고 진행 중인" 요청 수를 세는 원자 카운터.
	 * 설정자/쓰는 자: rq_wait_init()이 0으로 초기화. rq_wait_inc_below()
	 *   (내부적으로 atomic_inc_below()의 CAS 루프)가 상한 미만일 때만 +1. 완료 경로에서
	 *   atomic_dec()으로 -1(예: wbt_rqw_done()의 atomic_dec(&rqw->inflight)).
	 * 읽는 자: rq_qos_wait()의 fast-path 검사와 rq_qos_wake_function()의 재시도 검사가
	 *   atomic_read()/atomic_try_cmpxchg()로 이 값을 확인한다.
	 * 값 범위: 0 이상. 상한(limit)은 이 구조체 자체가 아니라 호출자가 rq_wait_inc_below()
	 *   에 넘기는 값(wbt는 rq_depth->max_depth, iolatency는 iolat->rq_depth류 파라미터)
	 *   으로 정책마다 다르게 결정된다.
	 * 동기화: CAS(atomic_try_cmpxchg) 기반이므로 락이 필요 없다. 여러 CPU가 동시에
	 *   증가를 시도해도 정확히 하나만 상한 도달 시점에 성공한다. */
};

/*
 * [한국어]
 * struct rq_qos - request_queue에 매달리는 QoS 정책 인스턴스 1개를 표현하는 공통 헤더
 *
 * wbt/iolatency/iocost는 각자의 private 구조체(struct rq_wb, struct blk_iolatency,
 * struct ioc) 맨 앞(또는 내부)에 이 구조체를 embed하고, RQWB()/rqos_to_ioc() 같은
 * container_of 기반 헬퍼로 서로를 오간다. request_queue->rq_qos는 이 구조체를 head로
 * 하는 단일 연결 리스트이며, rq_qos_add()가 항상 리스트 head에 삽입하므로 "나중에 등록된
 * 정책일수록 __rq_qos_*() 순회에서 먼저 호출된다."
 */
struct rq_qos {
	const struct rq_qos_ops *ops;
	/* [한국어] 이 정책 인스턴스가 구현하는 콜백 vtable을 가리키는 포인터.
	 * 설정자: rq_qos_add()가 인자로 받은 ops를 그대로 저장(rqos->ops = ops). wbt/
	 *   iolatency/iocost 각각 정적 const 테이블(wbt_rqos_ops/blkcg_iolatency_ops/
	 *   ioc_rqos_ops)의 주소를 넘긴다.
	 * 읽는 자: block/blk-rq-qos.c의 모든 __rq_qos_*() 함수가 rqos->ops->throttle 등을
	 *   NULL 체크 후 호출한다. 정책마다 구현하지 않은 콜백은 NULL이므로 호출을 건너뛴다
	 *   (예: iolatency는 ops->done이 NULL이라 __rq_qos_done()에서 건너뛰어진다).
	 * 값 범위: NULL 불가 — rq_qos_add() 호출자가 항상 유효한 정적 테이블을 넘긴다.
	 * 동기화: const 테이블이므로 런타임 갱신이 없어 별도 동기화가 필요 없다. */

	struct gendisk *disk;
	/* [한국어] 이 QoS 정책이 적용되는 블록 디바이스의 gendisk.
	 * 설정자: rq_qos_add()가 인자로 받은 disk를 저장(rqos->disk = disk).
	 * 읽는 자: rq_qos_del()이 rqos->disk->queue로 이 정책이 속한 request_queue를
	 *   역참조해 체인에서 분리한다(rqos 자체는 request_queue를 직접 들고 있지 않음).
	 * 값 범위: NULL 불가 — 등록 시점에 유효한 gendisk가 항상 존재해야 한다.
	 * 동기화: 등록 이후 값이 바뀌지 않는 불변 필드(디바이스 재부팅 없이는 gendisk가
	 *   교체되지 않음). */

	enum rq_qos_id id;
	/* [한국어] 이 인스턴스가 wbt/iolatency/iocost 중 어느 정책인지 나타내는 식별자.
	 * 설정자: rq_qos_add()가 인자로 받은 id를 저장.
	 * 읽는 자: rq_qos_id()가 q->rq_qos 리스트를 순회하며 원하는 id와 일치하는 rqos를
	 *   찾을 때 비교(wbt_rq_qos()/iolat_rq_qos()가 각각 RQ_QOS_WBT/RQ_QOS_LATENCY로 호출).
	 *   rq_qos_add()도 등록 전 동일 id 중복 여부를 이 필드로 검사한다.
	 * 값 범위: enum rq_qos_id의 세 값 중 하나(RQ_QOS_WBT/RQ_QOS_LATENCY/RQ_QOS_COST).
	 * 동기화: 등록 이후 불변. */

	struct rq_qos *next;
	/* [한국어] 같은 request_queue에 등록된 다음 QoS 정책을 가리키는 단일 연결 리스트
	 *   포인터. 리스트의 head는 request_queue->rq_qos이다.
	 * 설정자: rq_qos_add()가 기존 head를 next로 저장한 뒤(rqos->next = q->rq_qos) 자신을
	 *   새 head로 세운다(head 삽입 방식). rq_qos_del()이 포인터-투-포인터 순회로 이
	 *   필드를 갱신해 체인에서 자신을 분리한다.
	 * 읽는 자: block/blk-rq-qos.c의 모든 __rq_qos_*() 함수가
	 *   `do { ... ; rqos = rqos->next; } while (rqos);` 패턴으로 체인 전체를 순회한다.
	 * 값 범위: 마지막 원소는 NULL. 그 외에는 유효한 rq_qos 포인터.
	 * 동기화: q->rq_qos_mutex(request_queue 내부 뮤텍스) 보유 상태 + 큐 동결(freeze)
	 *   상태에서만 이 필드가 수정된다(rq_qos_add()/rq_qos_del() 내부의
	 *   lockdep_assert_held(&q->rq_qos_mutex) 참고). 순회 자체는 in-flight I/O 경로에서
	 *   락 없이 이루어지므로, "수정은 큐를 멈추고, 순회는 락 없이"라는 비대칭 규칙이다. */
#ifdef CONFIG_BLK_DEBUG_FS
	struct dentry *debugfs_dir;
	/* [한국어] 이 정책의 상태를 /sys/kernel/debug/block/<disk>/rqos/<name>/ 아래에
	 *   노출하는 debugfs 디렉터리. CONFIG_BLK_DEBUG_FS가 켜진 빌드에서만 존재한다.
	 * 설정자: block/blk-mq-debugfs.c의 blk_mq_debugfs_register_rqos() 계열 함수가
	 *   ops->debugfs_attrs가 NULL이 아닌 정책에 한해 debugfs_create_dir()로 생성.
	 *   실제로 이 필드를 채우는 것은 wbt_rqos_ops뿐이다(iolatency/iocost는
	 *   debugfs_attrs를 채우지 않으므로 이 디렉터리도 생성되지 않는다).
	 * 읽는 자: 언등록 시 blk_mq_debugfs_unregister_rqos()가 debugfs_remove_recursive()로
	 *   재귀 삭제.
	 * 값 범위: NULL(디렉터리 미생성) 또는 유효한 dentry 포인터.
	 * 동기화: q->debugfs_mutex 계열 락 하에서 등록/해제되며, 그 밖의 경로에서는
	 *   읽기 전용으로 취급된다. */
#endif
};

/*
 * [한국어]
 * struct rq_qos_ops - QoS 정책이 구현하는 콜백 vtable
 *
 * block/blk-rq-qos.c의 __rq_qos_*() 함수들이 request_queue->rq_qos 체인을 순회하며 각
 * 콜백을 호출한다. 콜백은 전부 선택 사항(optional)이다 — 각 __rq_qos_*() 호출부가
 * `if (rqos->ops->xxx) rqos->ops->xxx(...)`로 NULL 체크를 하므로, 정책은 자신에게 의미
 * 있는 콜백만 채우면 된다. 아래 각 필드 주석은 block/blk-wbt.c, block/blk-iolatency.c,
 * block/blk-iocost.c의 실제 rq_qos_ops 초기화 테이블을 grep해서 확인한 결과를 반영한다
 * (구현하지 않는 정책은 명시적으로 "미구현"이라고 적었다).
 */
struct rq_qos_ops {
	void (*throttle)(struct rq_qos *, struct bio *);
	/* [한국어] bio가 request로 변환되기 "전", blk_mq_submit_bio() → rq_qos_throttle()
	 *   → __rq_qos_throttle()에서 체인의 모든 정책에 순서대로 호출되는 진입 관문.
	 * 구현 현황: 3개 정책 모두 구현.
	 *   - wbt: wbt_wait() — bio_to_wbt_flags()로 그룹(sync read/기타)을 정해 해당
	 *     rq_wait의 inflight를 rq_wait_inc_below()로 획득 시도, 실패 시 rq_qos_wait().
	 *   - iolatency: blkcg_iolatency_throttle() → __blkcg_iolatency_throttle() —
	 *     cgroup 계층을 따라 지연 목표 초과 여부를 확인하고 rq_qos_wait()로 대기.
	 *   - iocost: ioc_rqos_throttle() — calc_vtime_cost()로 비용을 계산해 iocg의 vtime
	 *     예산에서 차감(불충분하면 iocg_incur_debt()로 부채 계상, rq_wait는 쓰지 않음).
	 * 호출 시점의 실행 컨텍스트: bio 제출 프로세스 컨텍스트(블로킹 가능). */

	void (*track)(struct rq_qos *, struct request *, struct bio *);
	/* [한국어] blk_mq_get_request()가 request를 할당한 직후, bio를 그 request에
	 *   연결(track)할 때 rq_qos_track() → __rq_qos_track()에서 호출.
	 * 구현 현황: block/blk-wbt.c만 구현(wbt_track). rq->wbt_flags에
	 *   bio_to_wbt_flags(rwb, bio) 결과를 OR로 누적해, 이후 wbt_issue()/wbt_done()이
	 *   이 request가 어떤 그룹(sync read 등)에 속하는지 재계산 없이 알 수 있게 한다.
	 *   iolatency/iocost는 이 콜백을 채우지 않으므로(NULL) __rq_qos_track()에서
	 *   건너뛰어진다 — 두 정책은 bio->bi_blkg(cgroup 연결)만으로 충분히 컨텍스트를
	 *   파악하기 때문에 별도의 request 단위 추적이 필요 없다. */

	void (*merge)(struct rq_qos *, struct request *, struct bio *);
	/* [한국어] blk_attempt_bio_merge() 계열이 bio를 기존 request에 병합(merge)하기로
	 *   결정했을 때 rq_qos_merge() → __rq_qos_merge()에서 호출. 이 시점에는 이미
	 *   병합이 확정된 뒤이므로, 콜백은 "병합 사실을 통보받아 자신의 회계를 갱신"하는
	 *   역할만 한다.
	 * 구현 현황: block/blk-iocost.c만 구현(ioc_rqos_merge). 병합되는 bio 몫의 추가
	 *   비용만 calc_vtime_cost(bio, iocg, true)로 별도 계산해 vtime에서 추가 차감한다
	 *   (이미 병합 대상 request가 낸 비용에 "증분"만 더하는 방식).
	 *   wbt/iolatency는 이 콜백을 채우지 않는다 — wbt는 그룹 플래그가 이미 track에서
	 *   request 단위로 OR 누적되어 병합과 무관하게 정확하고, iolatency는 애초에
	 *   병합 여부와 무관하게 cgroup 단위로만 지연을 측정하기 때문이다(추정). */

	void (*issue)(struct rq_qos *, struct request *);
	/* [한국어] request가 실제로 드라이버 ->queue_rq()로 발행되기 직전,
	 *   blk_mq_dispatch_rq_list()/blk_mq_try_issue_directly() → rq_qos_issue() →
	 *   __rq_qos_issue()에서 호출.
	 * 구현 현황: block/blk-wbt.c만 구현(wbt_issue). sync read이고 아직 추적 중인
	 *   sync_issue가 없을 때만 rq->io_start_time_ns를 rwb->sync_issue에 기록하고
	 *   rq 포인터를 rwb->sync_cookie에 저장(완료 시 "이 포인터와 같은가"라는 식별용
	 *   힌트로만 쓰고 절대 역참조하지 않음 — request가 완료 후 재사용될 수 있으므로).
	 *   목적은 blk-stat 완료 콜백을 기다리지 않고도 오래 걸리는 sync read를 조기에
	 *   감지하기 위함이다. iolatency/iocost는 발행 시점에 할 일이 없어 미구현. */

	void (*requeue)(struct rq_qos *, struct request *);
	/* [한국어] 드라이버가 BLK_STS_RESOURCE 등을 반환해 request가 다시 큐로 돌아갈 때
	 *   blk_mq_requeue_request() → rq_qos_requeue() → __rq_qos_requeue()에서 호출.
	 * 구현 현황: block/blk-wbt.c만 구현(wbt_requeue). 재큐되는 request가 마침
	 *   rwb->sync_cookie와 같으면(=wbt_issue에서 추적 중이던 sync read가 되돌아온
	 *   경우) sync_issue를 0으로, sync_cookie를 NULL로 리셋해 잘못된 체류 시간이
	 *   latency_exceeded()에서 계산되는 것을 막는다. iolatency/iocost는 재큐 시
	 *   되돌릴 상태가 없어 미구현(비용/inflight는 done/done_bio에서만 정산). */

	void (*done)(struct rq_qos *, struct request *);
	/* [한국어] request 완료 시 blk_mq_complete_request() → rq_qos_done()(passthrough
	 *   request는 rq_qos_done()이 blk_rq_is_passthrough()로 걸러 이 콜백까지 오지
	 *   않음) → __rq_qos_done()에서 호출.
	 * 구현 현황: wbt와 iocost가 구현, iolatency는 미구현.
	 *   - wbt: wbt_done() — wbt_rqw_done()으로 해당 그룹의 rq_wait->inflight를
	 *     atomic_dec()하고 wake_up_nr(&rqw->wait, 1)로 대기자 1명을 깨움.
	 *   - iocost: ioc_rqos_done() — nr_met/nr_missed(목표 지연 충족/미충족 카운터)와
	 *     rq_wait_ns(대기 시간 통계)를 갱신해 autop(자동 파라미터 조정)의 입력으로 삼음.
	 *   - iolatency는 done_bio에서 모든 정산을 마치므로 request 단위 done이 불필요. */

	void (*done_bio)(struct rq_qos *, struct bio *);
	/* [한국어] bio_endio() → rq_qos_done_bio() → __rq_qos_done_bio()에서 호출되는
	 *   bio 단위 완료 통보. 하나의 request에 여러 bio가 병합되어 있으면 bio 개수만큼
	 *   개별 호출된다(request 단위 done()과는 호출 횟수가 다를 수 있음).
	 * 구현 현황: iolatency와 iocost가 구현, wbt는 미구현.
	 *   - iolatency: blkcg_iolatency_done_bio() — bio 완료 시각과 목표 지연을 비교해
	 *     iolat->rq_depth류 스케일 쿠키를 갱신하고, 필요하면 blkcg_schedule_throttle()
	 *     로 다음 스로틀을 예약.
	 *   - iocost: ioc_rqos_done_bio() — done_vtime을 누적해 실제로 소모된 가상 시간을
	 *     iocg에 반영.
	 *   - wbt는 request 단위 done()에서 이미 그룹 회계를 마치므로 bio 단위 통보가
	 *     필요 없다. */

	void (*cleanup)(struct rq_qos *, struct bio *);
	/* [한국어] bio가 request로 전혀 진입하지 못하고 오류로 중간 정리될 때
	 *   rq_qos_cleanup() → __rq_qos_cleanup()에서 호출. throttle의 역방향 연산 —
	 *   throttle에서 예약한 budget/비용이 있다면 여기서 되돌린다.
	 * 구현 현황: block/blk-wbt.c만 구현(wbt_cleanup). throttle 단계에서 미리
	 *   증가시킨 inflight 카운터를 원상복구한다. iolatency/iocost는 미구현 —
	 *   두 정책은 throttle 단계에서 실패 시 이미 rq_qos_wait()의 cleanup_cb 경로로
	 *   자체 롤백이 끝나므로 별도의 cleanup 콜백이 필요 없다(추정). */

	void (*queue_depth_changed)(struct rq_qos *);
	/* [한국어] 하드웨어 큐 depth(예: nr_requests sysfs 변경, nr_hw_queues 재구성)가
	 *   바뀔 때 rq_qos_queue_depth_changed() → __rq_qos_queue_depth_changed()에서 호출.
	 * 구현 현황: wbt와 iocost가 구현, iolatency는 미구현.
	 *   - wbt: wbt_queue_depth_changed() — rqd->queue_depth를 새 값으로 갱신하고
	 *     rq_depth_calc_max_depth()를 재호출해 max_depth를 재계산.
	 *   - iocost: ioc_rqos_queue_depth_changed() — 새 depth에 맞춰 autop(자동 프로파일
	 *     선택) 로직을 다시 태운다.
	 *   - iolatency는 rq_depth를 아예 쓰지 않으므로 이 콜백도 필요 없다. */

	void (*exit)(struct rq_qos *);
	/* [한국어] 디바이스 제거(del_gendisk()) 또는 정책의 명시적 해제(echo 0 > wbt_lat_usec
	 *   등) 경로에서, rq_qos_del()로 체인에서 이미 분리된 뒤 rq_qos_exit()(또는 정책의
	 *   자체 해제 함수)이 마지막으로 호출하는 정리 콜백.
	 * 구현 현황: 3개 정책 모두 구현(wbt_exit/blkcg_iolatency_exit/ioc_rqos_exit).
	 *   공통적으로 자신의 private 구조체(struct rq_wb/blk_iolatency/ioc)에 할당된
	 *   타이머 삭제, blkg 정책 데이터 해제, kfree() 등을 수행한다. 이 콜백이 호출되는
	 *   시점에는 이미 체인에서 분리되어 있으므로 이후 __rq_qos_*() 순회에는 더 이상
	 *   나타나지 않는다. */

	const struct blk_mq_debugfs_attr *debugfs_attrs;
	/* [한국어] 이 정책의 상태를 debugfs에 노출할 파일 속성 테이블(이름/권한/show 콜백
	 *   배열). block/blk-mq-debugfs.c가 CONFIG_BLK_DEBUG_FS 빌드에서 struct
	 *   rq_qos.debugfs_dir 아래에 이 배열을 순회하며 파일을 생성한다.
	 * 구현 현황: block/blk-wbt.c만 채운다(wbt_debugfs_attrs, "curr_win_nsec",
	 *   "enabled", "id", "min_lat_nsec", "unknown_cnt", "wc" 등의 파일을 노출).
	 *   iolatency/iocost는 이 필드를 NULL로 두어(구조체 정적 초기화 시 미지정 필드는
	 *   0/NULL) debugfs에 자신의 서브디렉터리를 만들지 않는다.
	 * 동기화: 정적 const 테이블이라 런타임 변경이 없다. */
};

/*
 * [한국어]
 * struct rq_depth - scale_step 하나로 max_depth(허용 가능한 최대 in-flight 수)를
 *                   지수적으로 늘리거나 줄이는 상태 기계
 *
 * 현재 커널 트리에서 이 구조체를 실제로 쓰는 것은 block/blk-wbt.c(struct rq_wb.rq_depth)
 * 뿐이다. block/blk-iolatency.c는 자체 스케일 쿠키(iolat->cur_win_nsec, scale_cookie
 * 등)로 유사한 역할을 하고, block/blk-iocost.c는 vtime 기반 비용 모델을 쓰므로 depth
 * 개념 자체가 없다. 동작 산식은 block/blk-rq-qos.c의 rq_depth_calc_max_depth()에 그대로
 * 구현되어 있다:
 *   scale_step == 0 : 기본 상태, max_depth = min(default_depth, queue_depth)
 *   scale_step  > 0 : max_depth = 1 + ((depth-1) >> scale_step)  (지수적 축소)
 *   scale_step  < 0 : max_depth = 1 + ((depth-1) << -scale_step), 단 queue_depth*3/4 상한
 *                     (지수적 확장)
 */
struct rq_depth {
	unsigned int max_depth;
	/* [한국어] 현재 허용되는 최대 in-flight(동시 진행) 요청 수. rq_wait_inc_below()의
	 *   limit 인자로 그대로 전달되어 실제 게이트 역할을 한다.
	 * 설정자: rq_depth_calc_max_depth()가 scale_step/queue_depth/default_depth로부터
	 *   산출해 대입. QD=1(queue_depth==1)인 특수 장치는 scale_step<=0일 때 강제로
	 *   2로 설정해(소프트웨어 파이프라이닝 효과), scale_step>0일 때는 1로 고정한다.
	 * 읽는 자: 정책의 acquire_inflight 콜백(예: wbt_inflight_cb())이
	 *   rq_wait_inc_below(rqw, rqd->max_depth)로 이 값을 상한으로 사용.
	 * 값 범위: 1 이상. 상한은 queue_depth(하드웨어 큐 깊이)를 넘지 않도록 설계됨.
	 * 동기화: 별도 락 없이 단일 갱신 경로(정책의 타이머 콜백)에서만 수정되고, 읽는
	 *   쪽은 값이 일시적으로 갱신 중이어도 치명적이지 않은(soft limit) 용도라 그대로
	 *   원자적이지 않은 읽기를 허용한다. */

	int scale_step;
	/* [한국어] 지연 피드백에 따른 스케일링 "단계". 0이 기본, 양수는 축소(지연 증가에
	 *   대한 반응), 음수는 확장(여유 있을 때 처리량 개선).
	 * 설정자: rq_depth_scale_up()이 1 감소(더 확장 쪽으로), rq_depth_scale_down()이
	 *   1 증가(더 축소 쪽으로) 또는 hard_throttle=true일 때 0으로 즉시 리셋.
	 * 읽는 자: rq_depth_calc_max_depth()가 부호와 크기를 보고 시프트 방향과 폭을 결정.
	 * 값 범위: 이론상 임의의 정수지만, min(31, scale_step)으로 시프트 오버플로를
	 *   막으므로 실질적으로 -31~+31 범위 밖은 추가 효과가 없다.
	 * 동기화: max_depth와 동일하게 단일 갱신 경로 가정. */

	bool scaled_max;
	/* [한국어] 직전 라운드에서 이미 "확장 가능한 최댓값"에 도달했는지 나타내는 플래그.
	 *   true면 rq_depth_scale_up()이 더 이상 확장을 시도하지 않고 즉시 false를 반환해
	 *   oscillation(진동)을 방지한다.
	 * 설정자: rq_depth_scale_up()이 rq_depth_calc_max_depth()의 반환값을 그대로 저장.
	 *   rq_depth_scale_down()은 축소할 때마다 무조건 false로 리셋(다음 라운드에 다시
	 *   확장을 시도할 수 있게 허용).
	 * 읽는 자: rq_depth_scale_up() 진입 시 이 플래그부터 확인.
	 * 값 범위: true/false.
	 * 동기화: max_depth와 동일. */

	unsigned int queue_depth;
	/* [한국어] 하드웨어(또는 blk-mq 태그 셋) 큐가 실제로 지원하는 최대 깊이. 정책의
	 *   max_depth 계산에서 "절대 넘을 수 없는 물리적 상한" 역할을 한다.
	 * 설정자: 정책 초기화 시 q->nr_requests 등으로 최초 설정, 이후
	 *   __rq_qos_queue_depth_changed() → ops->queue_depth_changed
	 *   (wbt_queue_depth_changed())가 새 하드웨어 depth로 갱신.
	 * 읽는 자: rq_depth_calc_max_depth()가 default_depth와의 min()을 취하거나
	 *   (scale_step<0일 때) queue_depth*3/4를 확장 상한으로 사용.
	 * 값 범위: 1 이상. blk-mq 태그 셋 크기(보통 하드웨어 SQ 깊이)에서 비롯됨.
	 * 동기화: max_depth와 동일. */

	unsigned int default_depth;
	/* [한국어] scale_step==0(기본 상태)일 때 목표로 삼는 depth. 정책마다 초기화 시점에
	 *   결정하는 "정책 취향" 값(예: wbt는 대체로 queue_depth의 일부 비율로 설정).
	 * 설정자: 정책 초기화 코드(wbt_init() 등)가 1회 설정. 이후에는 변경되지 않는다
	 *   (queue_depth_changed에서도 default_depth 자체는 그대로 두고 queue_depth만 갱신).
	 * 읽는 자: rq_depth_calc_max_depth()가 min(default_depth, queue_depth)로 기본
	 *   depth를 산출할 때 사용.
	 * 값 범위: 1 이상, queue_depth 이하가 되도록 정책이 초기화 시 조정.
	 * 동기화: 사실상 불변(초기화 이후 읽기 전용). */
};

/*
 * [한국어]
 * rq_qos_id() - request_queue에 등록된 QoS 정책 체인에서 특정 id의 인스턴스를 검색
 *
 * @q:  검색 대상 request_queue. q->rq_qos가 연결 리스트의 head.
 * @id: 찾고자 하는 정책 식별자(RQ_QOS_WBT/RQ_QOS_LATENCY/RQ_QOS_COST).
 * @return: id가 일치하는 rq_qos 포인터, 등록되어 있지 않으면 NULL.
 *
 * q->rq_qos 체인을 head부터 선형 순회하며 rqos->id를 비교한다. 체인 길이가 최대 3
 * (wbt/iolatency/iocost)이므로 O(n) 순회의 비용은 무시할 만하다. rq_qos_add()가 등록
 * 전 중복 검사에도 이 함수를 사용한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서 호출해도 안전 — 리스트 자체는 락 없이 순회 가능하도록
 * 설계되어 있다(수정은 큐 동결 상태에서만 일어나므로 in-flight 순회와 경합하지 않음).
 * 재진입: 순수 읽기 전용 순회이므로 재진입 안전.
 *
 * 호출 체인:
 *   wbt_rq_qos() / iolat_rq_qos() → [이 함수]
 *   rq_qos_add() (중복 등록 검사) → [이 함수]
 *   q_to_ioc() (block/blk-iocost.c) → [이 함수] → rqos_to_ioc()
 */
static inline struct rq_qos *rq_qos_id(struct request_queue *q,
				       enum rq_qos_id id)
{
	struct rq_qos *rqos;	/* [한국어] 순회 커서 — q->rq_qos에서 시작해 next를 따라간다 */
	for (rqos = q->rq_qos; rqos; rqos = rqos->next) { /* [한국어] head부터 NULL(리스트
							     * 끝)까지 순회 */
		if (rqos->id == id)	/* [한국어] 이 노드의 정책 식별자가 찾는 id와 일치하는지 비교 */
			break;		/* [한국어] 일치하면 즉시 순회 중단 — rqos가 결과 */
	}
	return rqos;	/* [한국어] 일치 노드(찾음) 또는 NULL(끝까지 못 찾음, rqos가 NULL로 수렴) */
}

/*
 * [한국어]
 * wbt_rq_qos() - request_queue에서 WBT(Writeback Throttling) 인스턴스를 검색
 *
 * @q: 검색 대상 request_queue.
 * @return: 등록된 WBT rq_qos 포인터, 없으면 NULL.
 *
 * rq_qos_id(q, RQ_QOS_WBT)의 얇은 래퍼. block/blk-wbt.c 전역에서 "이 큐에 WBT가 설치돼
 * 있는가"를 확인하거나 이미 설치된 인스턴스를 얻을 때 사용한다.
 * 실행 컨텍스트: rq_qos_id()와 동일(제약 없음).
 *
 * 호출 체인:
 *   wbt_enable_default() / wbt_set_min_lat() / block/blk-sysfs.c(wbt_lat_show 등)
 *     → [이 함수] → rq_qos_id()
 */
static inline struct rq_qos *wbt_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_WBT);	/* [한국어] WBT 전용 식별자로 체인 검색 위임 */
}

/*
 * [한국어]
 * iolat_rq_qos() - request_queue에서 io.latency(cgroup 지연 제어) 인스턴스를 검색
 *
 * @q: 검색 대상 request_queue.
 * @return: 등록된 iolatency rq_qos 포인터, 없으면 NULL.
 *
 * rq_qos_id(q, RQ_QOS_LATENCY)의 얇은 래퍼. block/blk-iolatency.c가 io.latency cgroup
 * 파일 파서나 blkg 정책 활성화 검사에서 "이 큐에 iolatency가 등록돼 있는가"를 확인할 때
 * 사용한다.
 * 실행 컨텍스트: rq_qos_id()와 동일(제약 없음).
 *
 * 호출 체인:
 *   iolatency_set_limit() / iolatency_pd_init() → [이 함수] → rq_qos_id()
 */
static inline struct rq_qos *iolat_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_LATENCY);	/* [한국어] iolatency 전용 식별자로 체인 검색 위임 */
}

/*
 * [한국어]
 * rq_wait_init() - struct rq_wait를 "아무도 없고, 아무도 안 기다리는" 초기 상태로 세팅
 *
 * @rq_wait: 초기화할 rq_wait 포인터. wbt는 배열(rq_wait[WBT_NUM_RWQ]) 각 원소마다,
 *           iolatency는 iolatency_grp당 1개씩 이 함수로 초기화한다.
 * @return: 없음(void).
 *
 * inflight 카운터를 0으로, waitqueue를 빈 상태로 만든다. 정책 초기화(wbt_init(),
 * iolatency_pd_init()) 시점에 한 번만 호출되며, 이후에는 rq_wait_inc_below()/
 * atomic_dec()/rq_qos_wait()가 이 구조체를 조작한다.
 * 실행 컨텍스트: 정책 초기화 경로(프로세스 컨텍스트). 아직 어떤 I/O도 이 rq_wait를
 * 참조하지 않는 시점에 호출되므로 동시성 문제가 없다.
 *
 * 호출 체인:
 *   wbt_init() (block/blk-wbt.c) → [이 함수]  (WBT_NUM_RWQ개 각각에 대해)
 *   iolatency_pd_init() (block/blk-iolatency.c) → [이 함수]
 */
static inline void rq_wait_init(struct rq_wait *rq_wait)
{
	atomic_set(&rq_wait->inflight, 0);	/* [한국어] in-flight 카운터를 0으로: 아직
						 * 아무 요청도 진행 중이 아닌 초기 상태 */
	init_waitqueue_head(&rq_wait->wait);	/* [한국어] waitqueue 헤드를 빈 리스트로
						 * 초기화: 이후 prepare_to_wait_exclusive()가
						 * 안전하게 등록할 수 있도록 준비 */
}

/*
 * [한국어]
 * rq_qos_add() - 새 QoS 정책 인스턴스를 request_queue의 rq_qos 체인에 등록
 *
 * @rqos: 등록할 인스턴스(호출자가 자신의 private 구조체 안에 이미 embed해 둔 것).
 * @disk: 정책을 적용할 블록 디바이스의 gendisk.
 * @id:   RQ_QOS_WBT/RQ_QOS_LATENCY/RQ_QOS_COST 중 하나.
 * @ops:  이 정책이 구현하는 콜백 vtable(정적 const 테이블).
 * @return: 성공 0, 동일 id가 이미 등록돼 있으면 -EBUSY.
 *
 * 실제 구현은 block/blk-rq-qos.c에 있다(이 헤더에는 선언만 존재). q->rq_qos_mutex를
 * 호출자가 미리 잡고 있어야 하며(lockdep_assert_held), 내부에서 blk_mq_freeze_queue()로
 * in-flight I/O가 전혀 없는 상태를 만든 뒤 체인 head에 rqos를 삽입하고
 * QUEUE_FLAG_QOS_ENABLED를 세운다.
 * 실행 컨텍스트: 정책 초기화 경로(프로세스 컨텍스트). 큐 동결로 인해 블로킹 가능.
 * 에러 경로: -EBUSY를 반환하더라도 큐 동결은 반드시 해제되어 정상 I/O가 막히지 않는다.
 *
 * 호출 체인:
 *   blk_wbt_init() / blk_iolatency_init() / blk_iocost_init()
 *     → mutex_lock(&q->rq_qos_mutex) → [이 함수] → mutex_unlock()
 */
int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops);
/*
 * [한국어]
 * rq_qos_del() - QoS 정책 인스턴스를 request_queue의 rq_qos 체인에서 분리
 *
 * @rqos: 제거할 인스턴스.
 * @return: 없음(void).
 *
 * rqos->disk->queue로 request_queue를 역참조한 뒤, q->rq_qos_mutex 보유 + 큐 동결
 * 상태에서 포인터-투-포인터 순회로 체인에서 분리한다. 체인이 완전히 비면
 * QUEUE_FLAG_QOS_ENABLED를 클리어한다. 주의: 이 함수는 ops->exit()를 호출하지
 * 않는다 — exit 호출은 호출자(정책의 해제 함수)의 책임이다.
 * 실행 컨텍스트: 정책 해제 경로(프로세스 컨텍스트). 큐 동결로 인해 블로킹 가능.
 *
 * 호출 체인:
 *   wbt_exit() 계열 상위 정리 함수 → mutex_lock(&q->rq_qos_mutex) → [이 함수]
 *     → (이후 별도로) ops->exit(rqos) → mutex_unlock()
 */
void rq_qos_del(struct rq_qos *rqos);

/*
 * [한국어] rq_qos_wait()에 전달되는 두 콜백 타입.
 *
 * acquire_inflight_cb_t: "budget이 있으면 원자적으로 획득하고 true, 없으면 false"를
 *   구현해야 한다. 대표 구현은 rq_wait_inc_below() 자체(또는 그 위에 조건을 얹은
 *   wbt_inflight_cb()/iolat_acquire_inflight()). rq_qos_wake_function()이 waiter를
 *   깨울 때도 동일한 콜백을 재사용해 "깨우면서 동시에 획득"을 시도한다.
 * cleanup_cb_t: rq_qos_wait() 내부에서 "첫 waiter의 재시도"와 "wake 함수의 획득"이
 *   동시에 성공해 budget을 이중으로 획득해버린 race를 감지했을 때, 초과분을 반납하는
 *   콜백. 대표 구현은 wbt_cleanup_cb()/iolat_cleanup_cb() — 공통적으로 atomic_dec()으로
 *   inflight를 되돌린다.
 * 사용 정책: block/blk-wbt.c(wbt_inflight_cb/wbt_cleanup_cb)와
 *   block/blk-iolatency.c(iolat_acquire_inflight/iolat_cleanup_cb)만 이 콜백 쌍을
 *   rq_qos_wait()에 넘긴다. block/blk-iocost.c는 rq_wait/rq_qos_wait() 메커니즘을
 *   전혀 쓰지 않으므로(vtime 기반 자체 디베이트) 이 타입의 구현체가 없다.
 */
typedef bool (acquire_inflight_cb_t)(struct rq_wait *rqw, void *private_data);
/* [한국어] 반환값이 void인 이유: 이 콜백은 '실패할 수 있는 반납'이 아니라 이미 확정된
 * 초과 획득분을 되돌리는 것뿐이라, 호출자가 확인할 결과가 없다. */
typedef void (cleanup_cb_t)(struct rq_wait *rqw, void *private_data);

/*
 * [한국어]
 * rq_qos_wait() - budget이 없을 때 태스크를 재우고, budget이 생기면 정확히 한 waiter만
 *                 골라 깨우는 공용 대기 루틴
 *
 * @rqw: budget 상태(inflight, waitqueue)를 담은 rq_wait.
 * @private_data: acquire_inflight_cb/cleanup_cb에 그대로 전달되는 정책별 컨텍스트
 *                (wbt는 그룹 정보를 담은 스택 구조체, iolatency는 iolatency_grp 포인터).
 * @acquire_inflight_cb: budget 획득 시도 콜백.
 * @cleanup_cb: 이중 획득 race 발생 시 초과분을 반납하는 콜백.
 * @return: 없음(void). 반환 시점에는 반드시 budget을 획득한 상태다.
 *
 * 상세 동작(구현은 block/blk-rq-qos.c에 있으며, 이 헤더에는 프로토타입만 선언):
 *   1) waitqueue가 비어 있고 즉시 획득 가능하면 잠들지 않고 바로 반환(fast path).
 *   2) 그렇지 않으면 커스텀 wake 함수(rq_qos_wake_function)로 exclusive waiter 등록.
 *   3) 자신이 waitqueue의 첫 waiter라면, 등록 직후 한 번 더 획득을 재시도해
 *      "아무도 안 깨워주는 deadlock"을 방지(forward progress 보장).
 *   4) io_schedule()로 TASK_UNINTERRUPTIBLE 대기, wake 함수가 획득 성공 시 깨움.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트에서만 호출 가능(io_schedule()로 블로킹).
 * done/done_bio 같은 인터럽트·softirq 컨텍스트에서는 절대 호출해서는 안 된다.
 *
 * 호출 체인:
 *   wbt_wait() (block/blk-wbt.c) → [이 함수]
 *   __blkcg_iolatency_throttle() (block/blk-iolatency.c) → [이 함수]
 */
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb);

/*
 * [한국어]
 * rq_wait_inc_below() - inflight 카운터가 limit 미만일 때만 원자적으로 +1
 *
 * @rq_wait: 대상 rq_wait.
 * @limit:   허용 상한(exclusive) — 보통 wbt는 struct rq_depth.max_depth를 그대로 넘긴다.
 * @return:  증가 성공(budget 획득) true, 상한 도달로 실패 false.
 *
 * 실제 CAS 루프는 block/blk-rq-qos.c의 static 함수 atomic_inc_below()에 구현되어 있고,
 * 이 함수는 그 공개 래퍼다. acquire_inflight_cb_t 타입에 맞는 시그니처를 가지므로
 * wbt_inflight_cb() 등에서 그대로(또는 조건을 덧붙여) 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출) 또는 wake 함수(softirq) 양쪽에서 안전 —
 * 락 없는 원자 연산만 사용하기 때문.
 *
 * 호출 체인:
 *   wbt_inflight_cb() → [이 함수]
 *   rq_qos_wake_function() → cb(=acquire_inflight_cb) → [이 함수]
 */
bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit);

/*
 * [한국어]
 * rq_depth_scale_up() - scale_step을 감소시켜 max_depth를 한 단계 확장
 *
 * @rqd: 대상 rq_depth(wbt 전용).
 * @return: 확장 수행 시 true, 이미 확장 한도(scaled_max)에 도달해 있으면 false.
 *
 * 지연이 목표 범위 안이거나 처리량 개선이 필요할 때 wbt의 스케일 타이머에서 호출된다.
 * 실제 산식은 rq_depth_calc_max_depth()(block/blk-rq-qos.c)에 구현.
 * 실행 컨텍스트: 타이머 콜백(softirq) 또는 이에 준하는 주기적 재평가 경로.
 *
 * 호출 체인:
 *   wbt_timer_fn() (block/blk-wbt.c) → [이 함수] → rq_depth_calc_max_depth()
 */
bool rq_depth_scale_up(struct rq_depth *rqd);
/*
 * [한국어]
 * rq_depth_scale_down() - scale_step을 증가시켜 max_depth를 한 단계(또는 즉시) 축소
 *
 * @rqd: 대상 rq_depth(wbt 전용).
 * @hard_throttle: true면 scale_step을 0으로 즉시 리셋(심각한 지연 위반에 대한 즉각 대응),
 *                 false면 1씩 점진적으로 증가.
 * @return: 축소 수행 시 true, 이미 max_depth==1(최솟값)이면 false.
 *
 * 지연 목표 초과나 타임아웃 발생 시 wbt의 스케일 타이머에서 호출된다.
 * 실행 컨텍스트: rq_depth_scale_up()과 동일.
 *
 * 호출 체인:
 *   wbt_timer_fn() (block/blk-wbt.c) → [이 함수] → rq_depth_calc_max_depth()
 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle);
/*
 * [한국어]
 * rq_depth_calc_max_depth() - 현재 scale_step으로부터 max_depth를 산출해 대입
 *
 * @rqd: 대상 rq_depth(wbt 전용).
 * @return: true면 이미 확장 상한에 도달(더 이상 scale_up 불필요/불가), false면 여지 있음.
 *
 * rq_depth_scale_up()/rq_depth_scale_down() 양쪽에서 마지막 단계로 호출되는 실제 계산
 * 함수. QD=1 특수 케이스와, scale_step 부호에 따른 지수적 시프트 계산(구조체 rq_depth
 * 상단 주석 참고)을 수행한다.
 * 실행 컨텍스트: 호출자와 동일(타이머 콜백).
 *
 * 호출 체인:
 *   rq_depth_scale_up() → [이 함수]
 *   rq_depth_scale_down() → [이 함수]
 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd);

/*
 * [한국어]
 * __rq_qos_*() 계열 - rq_qos_*() static inline 래퍼가 QUEUE_FLAG_QOS_ENABLED 확인 후
 *                     위임하는 실제 체인 순회 함수들
 *
 * 아래 9개 함수는 모두 block/blk-rq-qos.c에 구현되어 있고, 공통 패턴은
 * `do { if (rqos->ops->콜백) rqos->ops->콜백(...); rqos = rqos->next; } while (rqos);`
 * 이다 — 즉 q->rq_qos 체인의 처음(가장 최근에 등록된 정책)부터 끝까지, 콜백을 구현한
 * 정책에 한해 순서대로 호출한다. 이 헤더 상단의 struct rq_qos_ops 필드별 주석에 각
 * 콜백을 실제로 구현하는 정책(wbt/iolatency/iocost) 현황이 정리되어 있으니 함께 참고.
 */
void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio);
/*
 * [한국어] __rq_qos_cleanup - bio 오류/중간 정리 시 체인의 ops->cleanup을 순회 호출.
 * @rqos: 체인 head(request_queue->rq_qos). @bio: 정리 대상 bio. @return: 없음.
 * 구현 정책: wbt(wbt_cleanup)만 해당 콜백을 가짐.
 * 호출 체인: rq_qos_cleanup() (inline, 이 헤더) → [이 함수] → ops->cleanup
 */
void __rq_qos_done(struct rq_qos *rqos, struct request *rq);
/*
 * [한국어] __rq_qos_done - request 완료 시 체인의 ops->done을 순회 호출.
 * @rqos: 체인 head. @rq: 완료된 request(passthrough 제외는 호출부에서 이미 필터링됨).
 * @return: 없음.
 * 구현 정책: wbt(wbt_done), iocost(ioc_rqos_done). iolatency는 done을 구현하지 않음.
 * 호출 체인: rq_qos_done() (inline) → [이 함수] → ops->done
 */
void __rq_qos_issue(struct rq_qos *rqos, struct request *rq);
/*
 * [한국어] __rq_qos_issue - 드라이버 발행 직전 체인의 ops->issue를 순회 호출.
 * @rqos: 체인 head. @rq: 발행될 request. @return: 없음.
 * 구현 정책: wbt(wbt_issue)만 해당 콜백을 가짐.
 * 호출 체인: rq_qos_issue() (inline) → [이 함수] → ops->issue
 */
void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq);
/*
 * [한국어] __rq_qos_requeue - request 재큐 시 체인의 ops->requeue를 순회 호출.
 * @rqos: 체인 head. @rq: 재큐될 request. @return: 없음.
 * 구현 정책: wbt(wbt_requeue)만 해당 콜백을 가짐.
 * 호출 체인: rq_qos_requeue() (inline) → [이 함수] → ops->requeue
 */
void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio);
/*
 * [한국어] __rq_qos_throttle - bio→request 변환 전 체인의 ops->throttle을 순회 호출.
 * @rqos: 체인 head. @bio: throttle 대상 bio. @return: 없음.
 * 구현 정책: wbt/iolatency/iocost 모두 구현(진입 관문이므로 3개 정책 전부).
 * 호출 체인: rq_qos_throttle() (inline) → [이 함수] → ops->throttle
 *           → (budget 부족 시) rq_qos_wait()
 */
void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio);
/*
 * [한국어] __rq_qos_track - bio-request 매핑 시 체인의 ops->track을 순회 호출.
 * @rqos: 체인 head. @rq: 매핑 대상 request. @bio: 매핑되는 bio. @return: 없음.
 * 구현 정책: wbt(wbt_track)만 해당 콜백을 가짐.
 * 호출 체인: rq_qos_track() (inline) → [이 함수] → ops->track
 */
void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio);
/*
 * [한국어] __rq_qos_merge - bio 병합 시 체인의 ops->merge를 순회 호출.
 * @rqos: 체인 head. @rq: 병합 대상 기존 request. @bio: 병합되는 bio. @return: 없음.
 * 구현 정책: iocost(ioc_rqos_merge)만 해당 콜백을 가짐.
 * 호출 체인: rq_qos_merge() (inline) → [이 함수] → ops->merge
 */
void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio);
/*
 * [한국어] __rq_qos_done_bio - bio 완료 시 체인의 ops->done_bio를 순회 호출.
 * @rqos: 체인 head. @bio: 완료된 bio. @return: 없음.
 * 구현 정책: iolatency(blkcg_iolatency_done_bio), iocost(ioc_rqos_done_bio).
 *           wbt는 done_bio를 구현하지 않음(request 단위 done에서 이미 정산).
 * 호출 체인: rq_qos_done_bio() (inline) → [이 함수] → ops->done_bio
 */
void __rq_qos_queue_depth_changed(struct rq_qos *rqos);
/*
 * [한국어] __rq_qos_queue_depth_changed - 큐 depth 변경 시 체인의
 *          ops->queue_depth_changed를 순회 호출.
 * @rqos: 체인 head. @return: 없음.
 * 구현 정책: wbt(wbt_queue_depth_changed), iocost(ioc_rqos_queue_depth_changed).
 *           iolatency는 이 콜백을 구현하지 않음(rq_depth 미사용).
 * 호출 체인: rq_qos_queue_depth_changed() (inline) → [이 함수] → ops->queue_depth_changed
 */

/*
 * [한국어]
 * rq_qos_cleanup() - QUEUE_FLAG_QOS_ENABLED와 q->rq_qos 존재 여부를 확인한 뒤
 *                    __rq_qos_cleanup()으로 위임하는 hot-path 게이트
 *
 * @q:   대상 request_queue.
 * @bio: 정리 대상 bio.
 * @return: 없음(void).
 *
 * QoS가 이 큐에 전혀 등록되어 있지 않은(가장 흔한) 경우, 플래그 검사 한 번으로
 * 함수 호출·체인 순회를 완전히 생략한다 — bio/request 생명주기 전체에서 반복 호출되는
 * 이 파일의 모든 rq_qos_*() 래퍼가 공유하는 최적화 패턴이다.
 * 실행 컨텍스트: bio 오류/취소 경로(프로세스 또는 softirq — 호출부에 따라 다름).
 *
 * 호출 체인:
 *   blk-mq 오류/취소 경로 → [이 함수] → __rq_qos_cleanup() → ops->cleanup(체인 전체)
 */
static inline void rq_qos_cleanup(struct request_queue *q, struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		/* [한국어] QoS 활성 플래그와 체인이 비어있지 않음을 모두 확인 —
		 * 플래그만으로는 rq_qos_del()이 체인을 비운 직후의 과도기를 놓칠 수
		 * 있어 q->rq_qos NULL 검사를 함께 한다 */
		__rq_qos_cleanup(q->rq_qos, bio);	/* [한국어] 체인 전체의 ops->cleanup
							 * 콜백을 순서대로 호출해 이 bio에
							 * 대해 예약된 상태를 롤백 */
}

/*
 * [한국어]
 * rq_qos_done() - request 완료 시 QoS 체인에 정산을 통보하는 hot-path 게이트
 *
 * @q:  대상 request_queue.
 * @rq: 완료된 request.
 * @return: 없음(void).
 *
 * QUEUE_FLAG_QOS_ENABLED/q->rq_qos 검사에 더해, blk_rq_is_passthrough(rq)인 request는
 * 아예 __rq_qos_done()을 호출하지 않는다 — NVMe admin 명령이나 SCSI passthrough처럼
 * 일반 read/write와 다른 성격의 명령은 대역폭/지연 QoS 통계 대상이 아니기 때문이다.
 * 실행 컨텍스트: 완료 경로(대개 인터럽트/softirq) — ops->done 구현체는 절대 블로킹해서는
 * 안 된다.
 *
 * 호출 체인:
 *   blk_mq_complete_request() 계열 → [이 함수] → __rq_qos_done()
 *     → ops->done(체인 중 wbt/iocost만 구현)
 */
static inline void rq_qos_done(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) &&
	    q->rq_qos && !blk_rq_is_passthrough(rq)) /* [한국어] QoS 활성 + 체인 존재 + 이
						       * request가 passthrough(진단/관리
						       * 명령)가 아님을 모두 확인 */
		__rq_qos_done(q->rq_qos, rq);	/* [한국어] 체인의 ops->done 콜백을 순서대로
						 * 호출해 inflight 감소·통계 갱신을 통보 */
}

/*
 * [한국어]
 * rq_qos_issue() - 드라이버 발행 직전 QoS 체인에 통보하는 hot-path 게이트
 *
 * @q:  대상 request_queue.
 * @rq: 발행 직전 request.
 * @return: 없음(void).
 *
 * 실행 컨텍스트: 디스패치 경로(kblockd 워커 또는 제출 태스크의 직접 디스패치 경로).
 * ops->issue 구현체는 빠르게 끝나야 한다(디스패치 지연에 직접 영향).
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list() / blk_mq_try_issue_directly() → [이 함수]
 *     → __rq_qos_issue() → ops->issue(체인 중 wbt만 구현)
 */
static inline void rq_qos_issue(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 */
		__rq_qos_issue(q->rq_qos, rq);	/* [한국어] 체인의 ops->issue 콜백을 순서대로
						 * 호출해 발행 시점을 통보 */
}

/*
 * [한국어]
 * rq_qos_requeue() - request 재큐 시 QoS 체인에 통보하는 hot-path 게이트
 *
 * @q:  대상 request_queue.
 * @rq: 재큐될 request.
 * @return: 없음(void).
 *
 * 드라이버가 자원 부족(BLK_STS_RESOURCE) 등으로 발행을 포기하고 request를 다시
 * 디스패치 큐로 돌릴 때 호출된다. throttle/issue 단계에서 이미 예약된 상태가 있다면
 * 여기서 되돌려 forward progress를 보장해야 한다.
 * 실행 컨텍스트: 재큐 경로(프로세스 또는 softirq — 호출부에 따라 다름).
 *
 * 호출 체인:
 *   blk_mq_requeue_request() → [이 함수] → __rq_qos_requeue()
 *     → ops->requeue(체인 중 wbt만 구현)
 */
static inline void rq_qos_requeue(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 */
		__rq_qos_requeue(q->rq_qos, rq);	/* [한국어] 체인의 ops->requeue
							 * 콜백을 순서대로 호출해 재큐
							 * 사실을 통보 */
}

/*
 * [한국어]
 * rq_qos_done_bio() - bio 완료 시 QoS 체인에 통보하되, stacked 블록 장치의 상/하위 큐
 *                     불일치까지 고려하는 hot-path 게이트
 *
 * @bio: 완료된 bio. bio->bi_bdev로부터 실제 하위 request_queue를 다시 얻는다.
 * @return: 없음(void).
 *
 * 다른 rq_qos_*() 래퍼와 달리 이 함수는 request_queue를 인자로 받지 않는다 — bio가
 * 소속된 request_queue가 throttle/merge 시점과 완료 시점에 다를 수 있기 때문이다
 * (예: dm-multipath/NVMe multipath에서 상위 장치는 QoS가 꺼져 있어도 하위 장치는 켜져
 * 있을 수 있음). 그래서 매번 bio->bi_bdev로부터 bdev_get_queue()를 다시 호출해 "진짜"
 * 큐를 얻는다.
 * 먼저 이 bio가 애초에 QoS 경로(throttle/merge)를 거쳤는지를 BIO_QOS_THROTTLED/
 * BIO_QOS_MERGED 플래그로 확인해, 거치지 않은 bio는 bdev 조회조차 생략한다(빠른 조기
 * 반환). BIO_QOS_THROTTLED는 block/blk-throttle.c의 BIO_TG_BPS_THROTTLED와 동일한 비트
 * 값을 공유하므로(enum bio_flags에서 별칭), bps 스로틀만 거친 bio도 이 검사를 통과할 수
 * 있다는 점에 유의해야 한다 — 다만 그 경우에도 아래 두 번째 test_bit 검사가 실제
 * rq_qos 존재 여부를 다시 확인하므로 안전하다.
 * 실행 컨텍스트: bio 완료 경로(대개 인터럽트/softirq).
 *
 * 호출 체인:
 *   bio_endio() → [이 함수] → __rq_qos_done_bio()
 *     → ops->done_bio(체인 중 iolatency/iocost만 구현)
 */
static inline void rq_qos_done_bio(struct bio *bio)
{
	struct request_queue *q;	/* [한국어] bio가 실제로 속한(throttle 시점과 다를 수
					 * 있는) request_queue를 담을 지역 변수 */

	if (!bio->bi_bdev || (!bio_flagged(bio, BIO_QOS_THROTTLED) && /* [한국어] bdev 조회(포인터 역참조 2회)를 하기
								       * 전에 플래그부터 본다 — bio_endio()는 극도로
								       * 빈번한 경로라 캐시 미스 하나가 비싸다 */
			     !bio_flagged(bio, BIO_QOS_MERGED))) /* [한국어] throttle을 거쳤거나 merge 통보를 받은 bio만
								  * 정산할 상태를 갖는다. 둘 다 아니면 정산할 것이 없다 */
		/* [한국어] bdev가 아예 없거나(디바이스에 매핑되지 않은 bio), 이 bio가
		 * throttle도 merge도 거치지 않았다면 QoS와 전혀 무관 — 아래의 큐 조회
		 * 자체가 불필요하므로 조기 반환 */
		return;

	q = bdev_get_queue(bio->bi_bdev);	/* [한국어] stacked 장치를 고려해 완료 시점의
						 * "실제" 하위 request_queue를 재조회 */

	/*
	 * A BIO may carry BIO_QOS_* flags even if the associated request_queue
	 * does not have rq_qos enabled. This can happen with stacked block
	 * devices — for example, NVMe multipath, where it's possible that the
	 * bottom device has QoS enabled but the top device does not. Therefore,
	 * always verify that q->rq_qos is present and QoS is enabled before
	 * calling __rq_qos_done_bio().
	 */
	/* [한국어] 위 영문 원본 주석 요지: bio에 QoS 플래그가 세워져 있어도, 재조회한
	 * q가 실제로 QoS를 활성화하고 있는지 다시 검사해야 안전하다(위 조기 반환 검사와
	 * 별개의 재확인) */
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		__rq_qos_done_bio(q->rq_qos, bio);	/* [한국어] 하위 큐 체인의
							 * ops->done_bio 콜백을 순서대로
							 * 호출해 bio 단위 정산을 통보 */
}

/*
 * [한국어]
 * rq_qos_throttle() - bio→request 변환 전 QoS 체인의 진입 관문
 *
 * @q:   대상 request_queue.
 * @bio: throttle 대상 bio.
 * @return: 없음(void). 반환 시점에는 체인의 모든 정책이 이 bio의 진행을 허가한 상태.
 *
 * BIO_QOS_THROTTLED 플래그를 세우는 것도 이 함수의 몫이다 — 이 플래그가 있어야
 * rq_qos_done_bio()가 완료 시점에 이 bio를 하위 큐의 QoS에 통보해야 하는지 판단할 수
 * 있다. 실제 대역폭/지연 판단과 필요 시 잠드는 것은 __rq_qos_throttle()이 호출하는
 * ops->throttle 각각(wbt_wait/blkcg_iolatency_throttle/ioc_rqos_throttle)의 몫이다.
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트. 블로킹 가능(ops->throttle이 rq_qos_wait()로
 * 잠들 수 있음).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() → [이 함수] → __rq_qos_throttle()
 *     → ops->throttle(체인 전체, wbt/iolatency/iocost 모두 구현)
 */
static inline void rq_qos_throttle(struct request_queue *q, struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos) {
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 — 둘 다 충족해야
		 * 아래 블록에 진입 */
		bio_set_flag(bio, BIO_QOS_THROTTLED);	/* [한국어] 이 bio가 throttle 경로를
							 * 거쳤음을 기록 — 완료 시
							 * rq_qos_done_bio()가 하위 큐에
							 * 통보할지 판단하는 근거가 된다 */
		__rq_qos_throttle(q->rq_qos, bio);	/* [한국어] 체인의 ops->throttle
							 * 콜백을 순서대로 호출해 대역폭/
							 * 지연/비용 한도를 검사하고 필요하면
							 * 대기시킨다 */
	}
}

/*
 * [한국어]
 * rq_qos_track() - bio-request 매핑 시 QoS 체인에 통보하는 hot-path 게이트
 *
 * @q:   대상 request_queue.
 * @rq:  이 bio가 매핑되는 request.
 * @bio: 매핑되는 bio.
 * @return: 없음(void).
 *
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트(request 할당 직후).
 *
 * 호출 체인:
 *   blk_mq_get_request() → [이 함수] → __rq_qos_track()
 *     → ops->track(체인 중 wbt만 구현)
 */
static inline void rq_qos_track(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 */
		__rq_qos_track(q->rq_qos, rq, bio);	/* [한국어] 체인의 ops->track
							 * 콜백을 순서대로 호출해 bio를
							 * request에 매핑된 컨텍스트로
							 * 등록 */
}

/*
 * [한국어]
 * rq_qos_merge() - bio 병합 시 QoS 체인에 통보하는 hot-path 게이트
 *
 * @q:   대상 request_queue.
 * @rq:  bio가 병합되는 기존 request.
 * @bio: 병합되는 bio.
 * @return: 없음(void).
 *
 * BIO_QOS_MERGED 플래그를 세우는 것도 이 함수의 몫이다 — throttle과 마찬가지로,
 * 이 플래그가 있어야 완료 시점에 rq_qos_done_bio()가 이 bio를 통보해야 하는지 판단할
 * 수 있다(병합된 bio는 자신만의 별도 request로 진행하지 않으므로, 병합 시점에 플래그를
 * 세워두지 않으면 완료 통보 경로를 놓치게 된다).
 * 실행 컨텍스트: bio 제출 프로세스 컨텍스트(병합 결정 직후).
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() → [이 함수] → __rq_qos_merge()
 *     → ops->merge(체인 중 iocost만 구현)
 */
static inline void rq_qos_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos) {
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 — 둘 다 충족해야
		 * 아래 블록에 진입 */
		bio_set_flag(bio, BIO_QOS_MERGED);	/* [한국어] 이 bio가 merge 경로를
							 * 거쳤음을 기록 — 완료 시
							 * rq_qos_done_bio()의 통보 대상
							 * 판단 근거 */
		__rq_qos_merge(q->rq_qos, rq, bio);	/* [한국어] 체인의 ops->merge
							 * 콜백을 순서대로 호출해 병합된
							 * bio 몫의 비용/통계를 재계산 */
	}
}

/*
 * [한국어]
 * rq_qos_queue_depth_changed() - 하드웨어 큐 depth 변경 시 QoS 체인에 재조정을
 *                                요청하는 게이트
 *
 * @q: 대상 request_queue.
 * @return: 없음(void).
 *
 * nr_requests sysfs 변경이나 nr_hw_queues 재구성처럼 큐의 물리적 깊이가 바뀌는
 * 드문 경로에서만 호출되므로, 다른 rq_qos_*() 래퍼처럼 매 I/O마다 실행되는 hot path는
 * 아니다.
 * 실행 컨텍스트: 큐 설정 변경 경로(프로세스 컨텍스트, sysfs write 등).
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues() / nr_requests sysfs 핸들러 → [이 함수]
 *     → __rq_qos_queue_depth_changed() → ops->queue_depth_changed(체인 중 wbt/iocost)
 */
static inline void rq_qos_queue_depth_changed(struct request_queue *q)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)
		/* [한국어] QoS 활성 플래그와 체인 존재 여부 확인 */
		__rq_qos_queue_depth_changed(q->rq_qos);	/* [한국어] 체인의
								 * ops->queue_depth_changed
								 * 콜백을 순서대로 호출해
								 * max_depth/queue_depth 재계산을
								 * 촉발 */
}

/*
 * [한국어]
 * rq_qos_exit() - request_queue에 등록된 모든 QoS 정책을 순서대로 완전히 해제
 *
 * @q: 대상 request_queue(디바이스 제거 경로에서 전달).
 * @return: 없음(void).
 *
 * 실제 구현(block/blk-rq-qos.c)은 q->rq_qos_mutex를 잡고, 체인이 빌 때까지 head를 하나씩
 * 떼어내며 각 정책의 ops->exit()를 호출한 뒤, 마지막에 QUEUE_FLAG_QOS_ENABLED를
 * 클리어한다. rq_qos_del()과 달리 이 함수는 exit 콜백 호출까지 스스로 책임진다는 점이
 * 다르다(디바이스 전체가 사라지는 경로이므로 개별 정책이 아니라 "전부"를 정리).
 * 실행 컨텍스트: 디바이스 제거 경로(프로세스 컨텍스트). mutex_lock으로 블로킹 가능.
 *
 * 호출 체인:
 *   del_gendisk() / blk_cleanup_queue() → [이 함수]
 *     → mutex_lock(&q->rq_qos_mutex) → ops->exit() (체인의 각 정책, wbt/iolatency/iocost
 *       모두 구현) → mutex_unlock()
 */
void rq_qos_exit(struct request_queue *);

#endif

/*
 * [한국어] 핵심 요약 (이 헤더 + block/blk-rq-qos.c + 3개 정책 구현체를 모두 읽은 뒤의 정리)
 *
 * - 이 헤더는 정책을 구현하지 않는다. struct rq_qos_ops vtable과, 그 vtable을 체인
 *   전체에 대해 순서대로 호출해주는 rq_qos_*()/__rq_qos_*() 래퍼만 제공한다.
 * - 콜백 구현 매트릭스(실제 grep 확인 결과):
 *     throttle             : wbt, iolatency, iocost (전부)
 *     track                : wbt만
 *     merge                : iocost만
 *     issue                : wbt만
 *     requeue              : wbt만
 *     done                 : wbt, iocost (iolatency는 없음)
 *     done_bio             : iolatency, iocost (wbt는 없음)
 *     cleanup              : wbt만
 *     queue_depth_changed  : wbt, iocost (iolatency는 없음)
 *     exit                 : wbt, iolatency, iocost (전부)
 *     debugfs_attrs        : wbt만
 * - struct rq_wait/rq_qos_wait()는 wbt와 iolatency가 공유하는 "잠들었다 깨는" 인프라이고,
 *   struct rq_depth는 wbt 전용이다. iocost는 이 두 메커니즘을 전혀 쓰지 않고 vtime
 *   기반 비용/부채 모델로 완전히 독립적인 스로틀링을 구현한다.
 * - BIO_QOS_THROTTLED/BIO_QOS_MERGED 플래그는 stacked 블록 장치(예: NVMe multipath)에서
 *   throttle/merge 시점의 큐와 완료 시점의 큐가 다를 수 있다는 사실을 보완하기 위한
 *   장치다 — rq_qos_done_bio()가 매번 bio->bi_bdev로 큐를 재조회하는 이유가 여기 있다.
 * - passthrough request(blk_rq_is_passthrough())는 rq_qos_done()에서 필터링되어 일반
 *   I/O QoS 통계/제한 대상에서 제외된다.
 */
