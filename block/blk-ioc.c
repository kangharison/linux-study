// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to io context handling
 */
/*
 * [한국어 설명] 프로세스별 I/O 컨텍스트(io_context)와 (io_context, request_queue) 쌍의
 * 연결 구조체인 io_cq(icq)의 할당/조회/참조카운트/해제를 담당하는 파일 (blk-ioc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 계층에서 "이 I/O를 누가, 어떤 우선순위로, 어떤 스케줄러
 * 상태를 가지고 발행했는가"를 추적하기 위한 두 자료구조 io_context와
 * io_cq(icq)의 생명주기를 관리한다. io_context는 태스크(프로세스/스레드)
 * 하나에 대응하는 I/O 서브시스템 상태(ioprio, icq 목록 등)를 담는
 * 참조카운트 객체이고, io_cq는 io_context와 request_queue 한 쌍마다
 * 존재하며 BFQ/mq-deadline 같은 I/O 스케줄러(elevator)가 태스크별
 * 프라이빗 상태(bfq_io_cq 등)를 매달아 두는 지점이다. 이 파일은 이 두
 * 객체의 alloc/get/put/lookup/create/destroy 전 과정과, 그 과정에서
 * 필요한 이중 락(ioc lock + queue lock) 순서 문제, RCU 기반 lookup,
 * 태스크 종료/fork 시의 정리/복제 로직까지 모두 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일의 함수들은 블록 I/O 요청 발행 경로(submit_bio 계열)에서
 * request가 만들어지고 elevator(I/O 스케줄러)에 붙는 시점에 호출된다.
 * 대표적인 호출 체인은 다음과 같다.
 *
 *   (issue 경로, icq 확보)
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_new_requests
 *       -> bfq_get_rq_private / dd_insert_request 등 elevator 콜백
 *       -> ioc_find_get_icq() -> (없으면) ioc_create_icq()
 *
 *   (조회 전용, 빠른 경로)
 *   bfq_bic_lookup -> ioc_lookup_icq()
 *
 *   (태스크 종료 경로)
 *   do_exit (kernel/exit.c) -> exit_io_context()
 *       -> ioc_exit_icqs() -> ioc_exit_icq() (스케줄러별 exit_icq 콜백)
 *       -> put_io_context() -> (refcount==0) -> ioc_delay_free()
 *       -> ioc_release_fn() (workqueue) -> ioc_destroy_icq() 반복 -> kmem_cache_free
 *
 *   (fork/clone 경로)
 *   copy_process (kernel/fork.c) -> copy_io() -> __copy_io()
 *
 *   (큐 소멸 경로)
 *   elevator_exit 등 -> ioc_clear_queue() -> ioc_destroy_icq() 반복
 *
 *   (ioprio 설정 경로)
 *   sys_ioprio_set (block/ioprio.c) -> set_task_ioprio()
 *
 * 실행 컨텍스트는 대부분 "태스크 프로세스 컨텍스트"이며 (issue 경로, exit
 * 경로, ioprio 설정, fork 경로 모두 호출 태스크 자신 또는 대상 태스크의
 * 컨텍스트), ioc_release_fn()만 예외적으로 system_power_efficient_wq
 * workqueue 상의 별도 커널 워커 스레드 컨텍스트에서 비동기 실행된다.
 * 이는 이중 락(ioc->lock -> q->queue_lock 역순 회피) 문제를 워크큐로
 * 미뤄서 데드락을 피하기 위함이다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk.h: ioc_find_get_icq(), ioc_lookup_icq(), ioc_clear_queue()의
 *   선언부. elevator.c가 큐 해제 시 ioc_clear_queue()를 호출하고,
 *   BFQ/mq-deadline 등 elevator 구현이 ioc_find_get_icq()/ioc_lookup_icq()를
 *   호출한다.
 * - block/blk-mq-sched.h: elevator_type->ops.init_icq/exit_icq 콜백을
 *   호출하는 지점(ioc_create_icq, ioc_exit_icq)이 이 스케줄러 인터페이스와
 *   맞닿아 있다.
 * - include/linux/iocontext.h: struct io_context, struct io_cq, ICQ_EXITED/
 *   ICQ_DESTROYED 플래그의 실제 정의가 있는 헤더. 이 파일은 그 정의를
 *   기반으로 lifecycle 연산만 구현하며, 필드 자체를 정의하지는 않는다.
 * - block/ioprio.c: sys_ioprio_set() 시스템 콜이 set_task_ioprio()를
 *   호출해 태스크의 io_context->ioprio 필드를 갱신한다.
 * - block/bfq-iosched.c, block/mq-deadline.c 등 elevator 구현: io_cq를
 *   struct io_cq를 첫 멤버로 포함하는 자신만의 확장 구조체(예: bfq_io_cq)
 *   로 감싸서 elevator_type->icq_size/icq_align에 등록하고, init_icq/
 *   exit_icq 콜백으로 자신의 상태를 초기화/정리한다.
 * - kernel/exit.c(do_exit), kernel/fork.c(copy_process): 태스크 생명주기
 *   이벤트(종료, 복제)에서 이 파일의 exit_io_context()/__copy_io()를 호출해
 *   io_context를 정리하거나 상속한다.
 * - 데이터 흐름: task_struct->io_context (1:1) -> io_context->icq_tree/
 *   icq_list (다대일, 이 태스크가 발행한 모든 큐에 대한 icq) <-> 각
 *   request_queue->icq_list (다대일, 그 큐에 연결된 모든 태스크의 icq).
 *   즉 icq는 ioc와 q 양쪽에서 동시에 참조되는 교차점 객체이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - get_io_context()/put_io_context(): io_context 참조카운트 증감. put이
 *   0에 도달하면 icq가 남아있는지에 따라 즉시 해제하거나 워크큐로 지연.
 * - ioc_create_icq()/ioc_lookup_icq()/ioc_find_get_icq(): icq를 생성/조회/
 *   "조회 후 없으면 생성"하는 3종 세트. radix tree + RCU hint 포인터로
 *   빠른 조회를 지원.
 * - ioc_exit_icq()/ioc_exit_icqs(): elevator의 exit_icq 콜백을 호출해
 *   스케줄러 프라이빗 상태를 정리(아직 icq 자체는 안 지움).
 * - ioc_destroy_icq()/ioc_release_fn()/ioc_delay_free(): icq를 실제로
 *   ioc/q 양쪽 리스트/radix tree에서 unlink하고 RCU로 해제. 두 락의
 *   순서 문제(q lock -> ioc lock) 때문에 trylock/재시도/워크큐 지연 등
 *   여러 전략을 조합.
 * - alloc_io_context()/exit_io_context()/__copy_io()/set_task_ioprio():
 *   task_struct->io_context 필드 자체의 할당/해제/복제/ioprio 갱신.
 * - ioc_clear_queue(): request_queue 소멸 시 그 큐에 연결된 모든 icq를
 *   일괄 정리 (역방향: q 관점에서 순회).
 */
#include <linux/kernel.h> /* [한국어] 커널 기본 매크로/타입(likely/unlikely, container_of, 기본 정수 타입 등)을 위해 포함 */
#include <linux/module.h> /* [한국어] EXPORT_SYMBOL/EXPORT_SYMBOL_GPL 등 모듈 심볼 export 인프라를 위해 포함 - 이 파일의 여러 함수가 다른 모듈(elevator 등)에 공개되므로 필요 */
#include <linux/init.h> /* [한국어] __init 어노테이션과 subsys_initcall() 매크로(파일 하단 blk_ioc_init 등록)를 위해 포함 */
#include <linux/bio.h> /* [한국어] struct bio 관련 정의 - 이 파일이 속한 블록 계층의 최상위 I/O 단위(bio)를 참조하는 상위 계층과의 연결점이므로 포함 */
#include <linux/blkdev.h> /* [한국어] struct request_queue, struct elevator_type 등 블록 디바이스 큐/스케줄러 핵심 타입 선언을 위해 포함 - 이 파일 전체가 이 타입들을 직접 다룸 */
#include <linux/slab.h> /* [한국어] kmem_cache_create/kmem_cache_alloc_node/kmem_cache_free 등 slab 할당자 API를 위해 포함 - io_context/io_cq 할당의 기반 */
#include <linux/security.h> /* [한국어] security_task_setioprio() LSM(Linux Security Module) 훅 선언을 위해 포함 - set_task_ioprio()의 ioprio 변경 권한 검사에 사용 */
#include <linux/sched/task.h> /* [한국어] struct task_struct와 task_lock()/task_unlock() 등 태스크 조작 API를 위해 포함 - io_context는 태스크에 매달린 필드이므로 필수 */

#include "blk.h" /* [한국어] 블록 계층 내부 전용 선언 - ioc_find_get_icq/ioc_lookup_icq/ioc_clear_queue 등 이 파일이 구현하는 함수들의 프로토타입이 이 헤더에 선언되어 있음 */
#include "blk-mq-sched.h" /* [한국어] blk-mq 스케줄러(elevator) 프레임워크 내부 인터페이스 헤더 - elevator_type->ops 콜백(init_icq/exit_icq 등)과 연동하기 위해 포함 */

/*
 * For io context allocations
 */
/*
 * [한국어] io_context 구조체 전용 slab 캐시 포인터.
 * 설정자: blk_ioc_init()에서 kmem_cache_create("blkdev_ioc", ...)로 딱 한 번 초기화.
 * 읽는 자: alloc_io_context()가 kmem_cache_alloc_node()로 새 io_context를 할당할 때,
 *   그리고 put_io_context()/ioc_release_fn()/set_task_ioprio()/ioc_find_get_icq() 등이
 *   io_context를 해제(kmem_cache_free)할 때 참조한다.
 * 값 범위: subsys_initcall(blk_ioc_init) 이후에는 항상 유효한 캐시 포인터 (NULL 아님).
 *   SLAB_PANIC 플래그로 생성되므로 생성 실패 시 커널 자체가 panic한다.
 * 동기화: 파일 스코프 static 전역이지만 초기화 이후에는 읽기 전용으로만 쓰이므로
 *   별도 락이 필요 없다 (slab 캐시 자체의 동시성은 slub/slab 코드가 보장).
 */
static struct kmem_cache *iocontext_cachep;

#ifdef CONFIG_BLK_ICQ /* [한국어] icq(io_cq) 인프라 자체가 이 커널 설정에서 활성화되어 있는지 분기 - 비활성 시 이 블록 전체가 컴파일에서 빠지고 뒤쪽 #else에서 대체 스텁이 정의됨 */
/**
 * get_io_context - increment reference count to io_context
 * @ioc: io_context to get
 *
 * Increment reference count to @ioc.
 */
/*
 * [한국어]
 * get_io_context - io_context 참조카운트를 1 증가시킨다
 *
 * @ioc: 참조카운트를 올릴 대상 io_context. 호출 시점에 이미 최소 1개의
 *       유효한 참조를 호출자가 들고 있어야 한다(그렇지 않으면 ioc가 이미
 *       해제된 상태를 참조하는 use-after-free가 될 수 있음).
 * @return: 없음 (void). 실패 경로가 없다 - refcount 증가는 실패할 수 없는 연산.
 *
 * io_context는 여러 request/icq/태스크(CLONE_IO 공유 시)에서 동시에
 * 참조될 수 있는 객체이므로, 마지막 참조가 사라지는 시점(put_io_context가
 * 0을 관측하는 시점)을 정확히 알기 위해 원자적 참조카운트로 관리한다.
 * 이 함수는 그 카운트를 증가시키기만 하며, BUG_ON으로 "이미 0 이하인
 * refcount를 다시 올리려는" 프로그래밍 오류(이미 해제된 객체를 다시
 * get하려는 use-after-free)를 즉시 커널 패닉으로 잡아낸다.
 * 동작 과정: (1) 현재 refcount가 0 이하인지 확인해 그렇다면 BUG_ON으로
 * 즉시 중단, (2) atomic_long_inc()로 원자적으로 1 증가.
 * 실행 컨텍스트: 호출자의 프로세스 컨텍스트에서 그대로 실행되며, atomic_long_inc
 * 자체는 락-프리 원자 연산이므로 별도의 락 없이 여러 CPU에서 동시에 안전하게
 * 호출 가능하다.
 * 호출자(caller): ioc_find_get_icq()가 기존 io_context를 재사용할 때
 * 참조를 하나 추가로 얻기 위해 호출한다.
 * 피호출자(callee): atomic_long_inc() (인라인 원자 연산).
 * 에러 처리: 실패 경로 없음. 선행 조건 위반 시 BUG_ON으로 커널 패닉.
 *
 * 호출 체인:
 *   ioc_find_get_icq() -> [get_io_context] -> atomic_long_inc()
 */
static void get_io_context(struct io_context *ioc)
{
	BUG_ON(atomic_long_read(&ioc->refcount) <= 0); /* [한국어] refcount가 이미 0 이하라면 ioc가 해제되었거나 초기화되지 않은 상태 - 이런 상태에서 get을 시도하는 것은 심각한 논리 오류이므로 즉시 패닉시켜 조기에 잡아낸다 */
	atomic_long_inc(&ioc->refcount); /* [한국어] refcount를 원자적으로 1 증가 - 다른 CPU의 동시 get/put과 경쟁해도 read-modify-write가 원자적으로 수행되어 카운트 값이 어긋나지 않는다 */
}

/*
 * Exit an icq. Called with ioc locked for blk-mq, and with both ioc
 * and queue locked for legacy.
 */
/*
 * [한국어]
 * ioc_exit_icq - 하나의 icq에 대해 elevator(스케줄러)의 exit 콜백을 호출한다
 *
 * @icq: exit 처리할 io_cq. 호출 시점에 ioc->lock을 보유하고 있어야 하며
 *       (legacy 블록 계층 지원 코드 경로에서는 추가로 q->queue_lock까지
 *       보유), icq가 어떤 request_queue(icq->q)에 속하는지는 이미
 *       확정된 상태여야 한다.
 * @return: 없음 (void).
 *
 * icq는 elevator_type->ops.init_icq로 초기화된 스케줄러별 프라이빗
 * 상태(BFQ의 bfq_io_cq 등)를 가질 수 있다. 이 icq가 더 이상 유효하지
 * 않게 되는 시점(태스크 종료, 큐 소멸)에 그 프라이빗 상태를 정리해 줘야
 * 리소스 누수(예: BFQ 엔티티가 스케줄 트리에 남아있는 문제)가 없다.
 * 이 함수는 그 정리(elevator_type->ops.exit_icq)를 딱 한 번만 실행되도록
 * ICQ_EXITED 플래그로 멱등성을 보장한다.
 * 동작 과정: (1) icq가 속한 request_queue의 elevator_type을 얻는다,
 * (2) 이미 ICQ_EXITED가 설정되어 있으면(다른 경로에서 이미 exit 처리됨)
 * 즉시 반환해 중복 실행을 막는다, (3) elevator가 exit_icq 콜백을 등록해
 * 두었으면 호출한다, (4) ICQ_EXITED 플래그를 설정해 이후 재호출 시
 * 다시 실행되지 않도록 표시한다.
 * 실행 컨텍스트: blk-mq 경로에서는 ioc->lock만 보유한 채 호출되고,
 * (레거시 블록 계층에서는) ioc->lock과 q->queue_lock을 모두 보유한 채
 * 호출된다 - 즉 이 함수 내부에서는 별도로 락을 잡지 않고 호출자가
 * 미리 잡아 둔 락에 의존한다.
 * 호출자(caller): ioc_exit_icqs()가 태스크의 모든 icq에 대해 순회하며
 * 호출하고, ioc_destroy_icq()가 icq를 실제로 파괴하기 직전에 한 번
 * 호출해 파괴 전에 스케줄러 상태가 반드시 정리되도록 보장한다.
 * 피호출자(callee): elevator_type->ops.exit_icq (예: bfq_exit_icq).
 * 에러 처리: exit_icq 콜백 자체가 void 반환이므로 실패 개념이 없다.
 *
 * 호출 체인:
 *   ioc_exit_icqs() / ioc_destroy_icq() -> [ioc_exit_icq] -> et->ops.exit_icq()
 */
static void ioc_exit_icq(struct io_cq *icq)
{
	struct elevator_type *et = icq->q->elevator->type; /* [한국어] icq가 연결된 request_queue의 현재 elevator(I/O 스케줄러) 타입을 얻는다 - exit_icq 콜백이 스케줄러마다 다르므로 이 타입 정보가 필요 */

	if (icq->flags & ICQ_EXITED) /* [한국어] 이 icq가 이미 exit 처리된 적이 있는지 확인 - ioc_exit_icqs 순회와 ioc_destroy_icq 양쪽에서 중복 호출될 수 있으므로 멱등성 보장 필요 */
		return; /* [한국어] 이미 정리되었으므로 아무 것도 하지 않고 반환 - et->ops.exit_icq를 두 번 호출하면 스케줄러 프라이빗 상태를 이중 해제할 위험이 있음 */

	if (et->ops.exit_icq) /* [한국어] 현재 elevator가 exit_icq 콜백을 등록했는지 확인 - 모든 스케줄러가 icq 프라이빗 상태를 갖는 것은 아니므로 콜백이 NULL일 수 있음 (예: none 스케줄러) */
		et->ops.exit_icq(icq); /* [한국어] 스케줄러별 정리 콜백 호출 - 예를 들어 BFQ는 bfq_io_cq에 매달린 bfq_queue/엔티티를 유휴 트리에서 제거하거나 병합 대상에서 해제 */

	icq->flags |= ICQ_EXITED; /* [한국어] 이 icq가 exit 처리되었음을 표시 - 이후 재호출(ioc_destroy_icq 등)에서 위 두 줄이 다시 실행되지 않도록 막는 플래그 */
}

/*
 * [한국어]
 * ioc_exit_icqs - io_context에 매달린 모든 icq에 대해 exit 콜백을 일괄 실행한다
 *
 * @ioc: exit 처리할 io_context. 이 태스크(또는 CLONE_IO로 공유하는
 *       태스크 그룹)가 지금까지 발행한 I/O가 거쳐간 모든 request_queue에
 *       대응하는 icq들이 ioc->icq_list에 매달려 있다.
 * @return: 없음 (void).
 *
 * 태스크가 종료(exit)할 때, 그 태스크가 발행했던 I/O들의 스케줄러
 * 프라이빗 상태(BFQ 엔티티 등)를 더 이상 유효하지 않은 상태로 만들어야
 * 한다. 이 함수는 icq 객체 자체를 파괴하지는 않고(그건 icq가 최종
 * unlink될 때 ioc_destroy_icq가 담당), "스케줄러 콜백 실행"만 먼저
 * 끝내 두어 태스크 종료 시점에 스케줄러가 더 이상 이 태스크의 I/O를
 * 스케줄링 대상으로 고려하지 않도록 한다.
 * 동작 과정: (1) ioc->lock을 인터럽트 비활성화와 함께 획득해 icq_list에
 * 대한 동시 수정(다른 CPU의 ioc_create_icq에 의한 hlist_add_head 등)을
 * 막는다, (2) hlist_for_each_entry로 icq_list를 순회하며 각 icq에 대해
 * ioc_exit_icq()를 호출한다, (3) 순회가 끝나면 락을 해제한다.
 * 실행 컨텍스트: exit_io_context()로부터 호출되며, 이는 태스크 자신의
 * do_exit() 경로이므로 프로세스 컨텍스트에서 실행된다. spin_lock_irq를
 * 사용하는 이유는 icq_list가 인터럽트 컨텍스트(블록 I/O 완료 인터럽트
 * 등)에서도 접근될 가능성을 배제하지 않기 위함이다.
 * 호출자(caller): exit_io_context() - active_ref가 0이 될 때(마지막
 * io_context 사용자가 사라질 때)만 호출된다.
 * 피호출자(callee): ioc_exit_icq() (각 icq에 대해 반복 호출).
 * 에러 처리: 실패 개념 없음 (모든 하위 함수가 void).
 *
 * 호출 체인:
 *   exit_io_context() -> [ioc_exit_icqs] -> ioc_exit_icq() (반복)
 */
static void ioc_exit_icqs(struct io_context *ioc)
{
	struct io_cq *icq; /* [한국어] hlist_for_each_entry 순회 중 현재 icq를 가리키는 커서 포인터 */

	spin_lock_irq(&ioc->lock); /* [한국어] icq_list 순회 도중 다른 CPU/인터럽트가 리스트를 변경하지 못하도록 ioc 락을 잡고 로컬 인터럽트도 비활성화 */
	hlist_for_each_entry(icq, &ioc->icq_list, ioc_node) /* [한국어] 이 io_context에 연결된 모든 icq(hlist, ioc_node로 연결)를 순회 */
		ioc_exit_icq(icq); /* [한국어] 각 icq에 대해 elevator의 exit_icq 콜백을 호출해 스케줄러 프라이빗 상태를 정리 */
	spin_unlock_irq(&ioc->lock); /* [한국어] 순회 종료 후 락 해제 및 인터럽트 상태 복원 */
}

/*
 * Release an icq. Called with ioc locked for blk-mq, and with both ioc
 * and queue locked for legacy.
 */
/*
 * [한국어]
 * ioc_destroy_icq - icq를 ioc/q 양쪽 자료구조에서 완전히 unlink하고 RCU로 해제한다
 *
 * @icq: 파괴할 io_cq. 호출 전에 반드시 icq->ioc->lock과 icq->q->queue_lock
 *       두 락을 모두 (올바른 순서로) 보유하고 있어야 한다.
 * @return: 없음 (void).
 *
 * icq는 io_context 쪽(icq_tree, icq_list)과 request_queue 쪽(icq_list)
 * 양쪽에서 동시에 참조되는 교차점 객체이므로, 완전히 없애려면 양쪽
 * 자료구조 모두에서 안전하게 unlink해야 한다. 게다가 이 icq를 RCU 없이
 * lookup 중인 다른 스레드(ioc_lookup_icq의 rcu_read_lock 구간)와의
 * 경쟁을 피하기 위해 실제 메모리 해제는 kfree_rcu로 grace period 이후로
 * 미룬다.
 * 동작 과정:
 *  (1) icq에서 소속 ioc/q/elevator_type을 얻는다.
 *  (2) lockdep_assert_held로 두 락이 실제로 잡혀있는지 개발 시점에 검증한다
 *      (락을 안 잡고 호출하면 이후 리스트 조작이 데이터 레이스가 됨).
 *  (3) 이미 ICQ_DESTROYED가 설정되어 있으면(중복 호출) 즉시 반환한다.
 *  (4) radix_tree_delete/hlist_del_init/list_del_init으로 icq를
 *      ioc->icq_tree, ioc->icq_list, q->icq_list 세 자료구조 모두에서
 *      제거한다.
 *  (5) ioc->icq_hint가 바로 이 icq를 가리키고 있었다면 NULL로 되돌린다
 *      (그렇지 않으면 이미 unlink된 icq를 hint가 계속 가리켜 use-after-free
 *      lookup이 발생할 수 있음). hint 갱신/해제는 모두 queue_lock 하에서만
 *      이뤄지므로 "지금 @icq를 안 가리키면 앞으로도 안 가리킨다"는 불변식이
 *      성립해 안전하게 판단 가능.
 *  (6) ioc_exit_icq()를 호출해 (아직 안 됐다면) 스케줄러 exit 콜백을
 *      마지막으로 한 번 더 보장 실행한다.
 *  (7) RCU 콜백이 실행되는 시점에는 icq->q가 이미 해제되어 있을 수
 *      있으므로, icq_cache(어떤 slab에서 free해야 하는지)를 미리
 *      icq->__rcu_icq_cache에 기록해 둔다. 이 필드는 icq->q_node와
 *      union으로 겹쳐 있어(struct io_cq 정의 참고) q_node가 이미
 *      list_del_init된 이후에나 안전하게 덮어쓸 수 있다.
 *  (8) ICQ_DESTROYED 플래그를 세워 중복 파괴를 막는다.
 *  (9) kfree_rcu(icq, __rcu_head)로 RCU grace period 이후 실제 메모리
 *      해제를 예약한다. __rcu_head 필드 역시 icq->ioc_node와 union으로
 *      겹쳐 있어(이미 hlist_del_init 이후이므로 안전) 별도의 rcu_head를
 *      추가로 할당하지 않고도 RCU-free가 가능하다.
 * 실행 컨텍스트: 프로세스 컨텍스트에서 ioc->lock + q->queue_lock을 모두
 * 보유한 채 실행된다. RCU read-side critical section은 아니지만,
 * kfree_rcu가 이후의 실제 해제를 RCU 도메인에 위임한다.
 * 호출자(caller): ioc_release_fn()(태스크 종료 시 ioc 쪽에서 남은 icq를
 * 정리), ioc_clear_queue()(큐 소멸 시 q 쪽에서 icq를 정리).
 * 피호출자(callee): radix_tree_delete(), hlist_del_init(), list_del_init(),
 * rcu_access_pointer()/rcu_assign_pointer(), ioc_exit_icq(), kfree_rcu().
 * 에러 처리: 실패 경로 없음 (모든 하위 연산이 실패하지 않는 unlink류
 * 연산). ICQ_DESTROYED 플래그가 재진입/중복 호출에 대한 유일한 방어선.
 *
 * 호출 체인:
 *   ioc_release_fn() / ioc_clear_queue() -> [ioc_destroy_icq] ->
 *       ioc_exit_icq() -> et->ops.exit_icq()
 */
static void ioc_destroy_icq(struct io_cq *icq)
{
	struct io_context *ioc = icq->ioc; /* [한국어] 이 icq가 속한 io_context - icq_tree/icq_list에서 unlink할 때 필요 */
	struct request_queue *q = icq->q; /* [한국어] 이 icq가 속한 request_queue - q->icq_list에서 unlink하고 q->id로 radix tree 키를 계산할 때 필요 */
	struct elevator_type *et = q->elevator->type; /* [한국어] icq_cache(icq를 할당했던 slab 캐시)를 얻기 위해 현재 elevator 타입을 조회 - RCU 콜백 시점엔 q가 사라졌을 수 있어 미리 확보 */

	lockdep_assert_held(&ioc->lock); /* [한국어] 개발/디버그 빌드에서 ioc->lock이 실제로 잠겨 있는지 검증 - 안 잡혀 있으면 아래 리스트 조작이 데이터 레이스가 되므로 조기에 버그를 잡기 위한 방어적 assert */
	lockdep_assert_held(&q->queue_lock); /* [한국어] q->queue_lock도 잠겨 있어야 함을 검증 - q->icq_list와 icq_hint는 queue_lock으로 보호되는 자료구조이기 때문 */

	if (icq->flags & ICQ_DESTROYED) /* [한국어] 이미 파괴 처리된 icq인지 확인 - ioc_release_fn과 ioc_clear_queue가 서로 경쟁적으로 같은 icq를 파괴하려 할 수 있으므로 멱등성 필요 */
		return; /* [한국어] 이미 파괴되었으므로 radix_tree_delete 등을 다시 실행하면 이중 삭제 버그가 되어 즉시 반환 */

	radix_tree_delete(&ioc->icq_tree, icq->q->id); /* [한국어] ioc->icq_tree에서 이 icq를 제거 - 키는 request_queue의 고유 id(q->id)이며, 이 트리는 ioc_lookup_icq가 빠른 조회에 사용하는 인덱스 */
	hlist_del_init(&icq->ioc_node); /* [한국어] ioc->icq_list(hlist)에서 이 icq를 제거하고 ioc_node를 초기화 상태로 되돌림 - ioc_exit_icqs의 순회 대상에서도 빠지게 됨 */
	list_del_init(&icq->q_node); /* [한국어] q->icq_list에서 이 icq를 제거하고 q_node를 초기화 상태로 되돌림 - q_node는 __rcu_icq_cache와 union이므로 이 시점 이후에나 __rcu_icq_cache에 안전하게 값을 쓸 수 있음 */

	/*
	 * Both setting lookup hint to and clearing it from @icq are done
	 * under queue_lock.  If it's not pointing to @icq now, it never
	 * will.  Hint assignment itself can race safely.
	 */
	if (rcu_access_pointer(ioc->icq_hint) == icq) /* [한국어] icq_hint(마지막 성공 lookup 결과를 캐싱하는 RCU 포인터)가 지금 파괴 중인 이 icq를 가리키고 있는지 확인 - queue_lock 하에서 검사하므로 이 결과 이후로는 바뀌지 않음(위 원본 주석 설명 참고) */
		rcu_assign_pointer(ioc->icq_hint, NULL); /* [한국어] hint가 이 icq를 가리키고 있었다면 NULL로 교체해 이후 ioc_lookup_icq가 곧 해제될 이 icq를 반환하지 않도록 함 - RCU assign이므로 동시 reader는 이전 값 또는 NULL 중 하나를 일관되게 관측 */

	ioc_exit_icq(icq); /* [한국어] 아직 exit 처리가 안 됐을 경우를 대비해 스케줄러 exit 콜백을 마지막으로 한 번 더 보장 실행 (ICQ_EXITED 플래그로 중복 실행은 자체 방지됨) */

	/*
	 * @icq->q might have gone away by the time RCU callback runs
	 * making it impossible to determine icq_cache.  Record it in @icq.
	 */
	icq->__rcu_icq_cache = et->icq_cache; /* [한국어] RCU 콜백이 실제로 실행되는 시점에는 icq->q(request_queue)가 이미 해제되어 elevator_type을 더 이상 조회할 수 없을 수 있으므로, 지금 시점에 icq_cache(할당 slab)를 icq 자신에게 미리 박아둠 - q_node와 union이라 위에서 list_del_init된 이후라야 안전 */
	icq->flags |= ICQ_DESTROYED; /* [한국어] 이 icq가 파괴 처리되었음을 표시 - 이후 재호출(경쟁하는 ioc_release_fn/ioc_clear_queue 등)에서 위 unlink 로직이 다시 실행되지 않도록 막음 */
	kfree_rcu(icq, __rcu_head); /* [한국어] RCU grace period가 지난 뒤 icq 메모리를 실제로 해제 예약 - ioc_lookup_icq의 rcu_read_lock 구간에서 아직 이 icq를 읽고 있을 수 있는 동시 reader가 안전하게 다 빠져나간 뒤에만 실제 free됨. __rcu_head는 ioc_node와 union이라 이미 hlist_del_init된 이후라야 안전하게 겹쳐 쓸 수 있음 */
}

/*
 * Slow path for ioc release in put_io_context().  Performs double-lock
 * dancing to unlink all icq's and then frees ioc.
 */
/*
 * [한국어]
 * ioc_release_fn - workqueue 컨텍스트에서 ioc에 남은 모든 icq를 정리하고 ioc 자체를 해제한다
 *
 * @work: container_of로 struct io_context::release_work를 역산하기 위한
 *        work_struct 포인터. queue_work(system_power_efficient_wq, ...)로
 *        예약된 작업이 워크큐 스레드에서 실행될 때 전달된다.
 * @return: 없음 (void). workqueue 콜백 시그니처를 따름.
 *
 * put_io_context()에서 refcount가 0이 되었지만 icq_list가 비어있지 않은
 * 경우(ioc_delay_free가 true를 반환한 경우), icq를 정리하려면 반드시
 * q->queue_lock -> ioc->lock 순서로 락을 잡아야 하는데, put_io_context()
 * 호출 시점에는 어떤 락을 이미 들고 있는지 호출자마다 달라 그 자리에서
 * 바로 q->queue_lock을 먼저 잡을 수 없는 경우가 있다(락 순서 역전으로
 * 데드락 위험). 이 함수는 그 처리를 별도의 워크큐 컨텍스트로 미뤄서,
 * 호출자의 락 상태와 무관하게 항상 올바른 순서로 락을 다시 획득할 수
 * 있게 한다.
 * 동작 과정:
 *  (1) work_struct 포인터로부터 container_of를 이용해 io_context 전체
 *      구조체 포인터를 복원한다.
 *  (2) ioc->lock을 잡고 icq_list가 빌 때까지 반복한다.
 *  (3) 리스트의 첫 icq를 하나 꺼내 그 icq가 속한 큐(q)를 얻는다.
 *  (4) q->queue_lock을 spin_trylock으로 시도한다:
 *      - 성공하면(락 순서 역전 없이 바로 잡힘) 그 자리에서
 *        ioc_destroy_icq()를 호출하고 곧바로 q lock을 푼다.
 *      - 실패하면(다른 CPU가 이미 큐 락을 갖고 있고, 그 CPU가 반대로
 *        ioc lock을 기다리고 있을 수도 있는 상황) rcu_read_lock()으로
 *        q와 icq가 이 구간 동안 메모리에서 사라지지 않음을 보장한 뒤,
 *        ioc->lock을 일단 놓고 q->queue_lock -> ioc->lock 순서로 다시
 *        잡아 락 순서를 정상화한다. 그 다음 ioc_destroy_icq()를 호출하고
 *        q lock과 RCU read-side critical section을 모두 정리한다.
 *  (5) 루프가 끝나(icq_list가 완전히 비워지면) ioc->lock을 해제한다.
 *  (6) io_context 자체를 kmem_cache_free()로 iocontext_cachep에 반환한다.
 * 실행 컨텍스트: system_power_efficient_wq에 바인딩된 커널 워커
 * 스레드 컨텍스트 - put_io_context()를 호출한 원래 태스크의 컨텍스트가
 * 아니라 완전히 별도의 프로세스 컨텍스트에서 비동기로 실행된다. 이
 * 덕분에 원래 호출자가 어떤 락을 들고 있었는지와 무관하게 이 함수
 * 내부에서 자유롭게 두 락을 원하는 순서로 재획득할 수 있다.
 * 호출자(caller): 워크큐 코어(process_one_work)가 ioc_delay_free()에서
 * queue_work()로 예약해 둔 이 콜백을 실행한다 (직접 함수 호출이 아니라
 * 워크큐 인프라를 통한 간접 호출).
 * 피호출자(callee): ioc_destroy_icq() (반복), kmem_cache_free().
 * 에러 처리: 실패 경로 없음. spin_trylock 실패는 에러가 아니라 락 순서
 * 재정렬이 필요하다는 신호로 취급되어 else 분기로 처리된다.
 *
 * 호출 체인:
 *   ioc_delay_free() -> queue_work() -> (워크큐 워커) -> [ioc_release_fn]
 *       -> ioc_destroy_icq() (반복) -> kmem_cache_free()
 */
static void ioc_release_fn(struct work_struct *work)
{
	struct io_context *ioc = container_of(work, struct io_context,
					      release_work); /* [한국어] work_struct 임베디드 필드(release_work)의 주소로부터 이를 감싸는 struct io_context 전체의 시작 주소를 역산 - INIT_WORK(&ioc->release_work, ioc_release_fn)로 등록될 때의 관례를 역이용 */
	spin_lock_irq(&ioc->lock); /* [한국어] icq_list를 순회/제거하는 동안 다른 CPU의 동시 접근을 막기 위해 ioc 락 획득 (인터럽트도 비활성화) */

	while (!hlist_empty(&ioc->icq_list)) { /* [한국어] ioc에 남은 icq가 하나라도 있는 동안 반복 - 루프가 끝나면 icq_list는 완전히 빈 상태가 됨 */
		struct io_cq *icq = hlist_entry(ioc->icq_list.first,
						struct io_cq, ioc_node); /* [한국어] hlist의 첫 번째 노드로부터 hlist_entry(container_of)로 실제 io_cq 구조체 포인터를 복원 - 매 반복마다 리스트의 head를 하나씩 꺼내는 방식 */
		struct request_queue *q = icq->q; /* [한국어] 이 icq가 속한 request_queue - 이 큐의 queue_lock을 잡아야 ioc_destroy_icq를 안전하게 호출할 수 있음 */

		if (spin_trylock(&q->queue_lock)) { /* [한국어] 이미 ioc->lock을 쥔 상태에서 q->queue_lock을 논블로킹으로 시도 - 성공하면 락 순서(q -> ioc) 위반 없이 바로 진행 가능 */
			ioc_destroy_icq(icq); /* [한국어] 두 락을 모두 쥔 상태에서 icq를 ioc/q 양쪽에서 unlink하고 RCU 해제 예약 */
			spin_unlock(&q->queue_lock); /* [한국어] q 락만 먼저 해제 - ioc->lock은 바깥 while 루프를 위해 계속 보유 */
		} else { /* [한국어] trylock 실패 - 다른 CPU가 q->queue_lock을 쥔 채 ioc->lock을 기다리고 있을 수 있는 잠재적 락 순서 역전 상황이므로 안전하게 재획득 순서를 바꿔야 함 */
			/* Make sure q and icq cannot be freed. */
			rcu_read_lock(); /* [한국어] 아래에서 ioc->lock을 잠깐 놓는 구간 동안 q(request_queue)와 icq가 RCU 보호 하에 있어 다른 스레드가 이들을 실제로 free하지 못하도록 보장 */

			/* Re-acquire the locks in the correct order. */
			spin_unlock(&ioc->lock); /* [한국어] 올바른 순서(q -> ioc)로 다시 잡기 위해 일단 ioc 락을 놓음 - RCU 보호 덕분에 icq/q 자체는 안전 */
			spin_lock(&q->queue_lock); /* [한국어] 정상 순서의 첫 단계로 q->queue_lock을 블로킹 방식으로 획득 */
			spin_lock(&ioc->lock); /* [한국어] 이어서 ioc->lock을 획득 - 이제 q -> ioc 순서로 두 락을 모두 보유한 상태가 되어 데드락 위험 없이 진행 가능 */

			ioc_destroy_icq(icq); /* [한국어] 두 락을 정상 순서로 재획득한 상태에서 icq를 unlink/해제 예약 - ioc 락을 놓았다 다시 잡는 사이 icq가 이미 다른 경로에서 파괴됐을 수 있으나 ICQ_DESTROYED 플래그 검사로 안전하게 무시됨 */

			spin_unlock(&q->queue_lock); /* [한국어] q 락 해제 */
			rcu_read_unlock(); /* [한국어] RCU read-side critical section 종료 - unlink가 이미 끝났으므로 더 이상 보호가 필요 없음 */
		}
	}

	spin_unlock_irq(&ioc->lock); /* [한국어] 루프가 끝나 icq_list가 완전히 비었으므로 ioc 락을 최종 해제 (인터럽트 상태도 복원) */

	kmem_cache_free(iocontext_cachep, ioc); /* [한국어] icq가 모두 정리된 io_context 자체를 slab 캐시로 반환 - 이 시점 이후 ioc 포인터는 더 이상 유효하지 않음 */
}

/*
 * Releasing icqs requires reverse order double locking and we may already be
 * holding a queue_lock.  Do it asynchronously from a workqueue.
 */
/*
 * [한국어]
 * ioc_delay_free - icq가 남아있으면 해제를 워크큐로 지연시키고, 없으면 즉시 해제 가능함을 알린다
 *
 * @ioc: refcount가 방금 0에 도달한 io_context (put_io_context에서 전달).
 * @return: true면 "아직 못 지웠다"는 뜻으로, icq 정리를 위해 이미
 *          ioc_release_fn()을 워크큐에 예약했으니 io_context 자체의
 *          kmem_cache_free는 워크큐 콜백이 대신 수행할 것임을 의미한다.
 *          false면 icq_list가 이미 비어있어(정리할 것이 없어) 호출자가
 *          바로 이 자리에서 io_context를 해제해도 안전함을 의미한다.
 *
 * icq를 안전하게 unlink하려면 q->queue_lock -> ioc->lock 순서로 락을
 * 잡아야 하는데, put_io_context()를 호출하는 시점에는 호출자가 이미
 * 어떤 큐의 queue_lock을 쥐고 있을 수도 있어(예: request 완료 경로),
 * 그 자리에서 바로 q->queue_lock을 (이미 다른 큐거나 순서가 안 맞게)
 * 잡으려 하면 락 순서 역전/데드락 위험이 생긴다. 그래서 icq가 하나라도
 * 남아있는 경우에는 정리를 통째로 워크큐(별도 컨텍스트)로 미룬다.
 * 동작 과정: (1) ioc->lock을 IRQ-safe하게 획득, (2) icq_list가
 * 비어있지 않으면 release_work를 system_power_efficient_wq에 큐잉하고
 * true를 반환(락 해제 후), (3) 비어있으면 false를 반환(락 해제 후) -
 * 이 경우 호출자가 직접 kmem_cache_free를 수행해야 함.
 * 실행 컨텍스트: put_io_context()의 호출자 컨텍스트에서 그대로 실행되며
 * (별도 스레드 전환 없음), 임의의 인터럽트 컨텍스트에서 호출될 가능성을
 * 배제하지 않기 위해 spin_lock_irqsave를 사용한다.
 * 호출자(caller): put_io_context() - refcount가 0이 된 직후.
 * 피호출자(callee): queue_work() (icq_list가 비어있지 않을 때만).
 * 에러 처리: queue_work가 이미 큐잉된 work를 중복 큐잉하지 않도록
 * 보장하는 것은 워크큐 코어의 책임이며, 이 함수 자체는 실패할 수 없다
 * (bool 반환은 에러가 아니라 "지연되었는가"를 나타내는 상태 값).
 *
 * 호출 체인:
 *   put_io_context() -> [ioc_delay_free] -> queue_work() -> (워크큐) -> ioc_release_fn()
 */
static bool ioc_delay_free(struct io_context *ioc)
{
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장한 이전 인터럽트 활성화 상태 - unlock 시 복원하기 위한 지역 변수 */

	spin_lock_irqsave(&ioc->lock, flags); /* [한국어] icq_list 검사 도중 다른 CPU/인터럽트의 동시 수정을 막기 위해 ioc 락 획득 - 인터럽트 컨텍스트에서 호출될 가능성이 있어 irqsave 변형 사용 */
	if (!hlist_empty(&ioc->icq_list)) { /* [한국어] 아직 이 io_context에 연결된 icq가 하나라도 남아있는지 확인 - 남아있으면 즉시 해제 불가, 워크큐로 미뤄야 함 */
		queue_work(system_power_efficient_wq, &ioc->release_work); /* [한국어] ioc_release_fn을 별도 워커 컨텍스트에서 실행하도록 예약 - system_power_efficient_wq는 절전을 고려한 공용 워크큐로, 락 순서 문제를 원래 호출자 컨텍스트에서 분리하기 위해 사용 */
		spin_unlock_irqrestore(&ioc->lock, flags); /* [한국어] 워크큐 예약이 끝났으므로 락 해제 및 인터럽트 상태 복원 */
		return true; /* [한국어] 해제가 지연되었음을 호출자(put_io_context)에게 알림 - 호출자는 kmem_cache_free를 직접 하지 않아야 함 */
	}
	spin_unlock_irqrestore(&ioc->lock, flags); /* [한국어] icq_list가 비어있는 경우의 락 해제 - 이 경로에서는 워크큐를 예약하지 않았음 */
	return false; /* [한국어] 정리할 icq가 없으므로 호출자가 바로 io_context를 해제해도 안전함을 알림 */
}

/**
 * ioc_clear_queue - break any ioc association with the specified queue
 * @q: request_queue being cleared
 *
 * Walk @q->icq_list and exit all io_cq's.
 */
/*
 * [한국어]
 * ioc_clear_queue - request_queue 관점에서, 이 큐에 연결된 모든 icq를 일괄 정리한다
 *
 * @q: 소멸 중인 request_queue. 더 이상 어떤 io_context와도 연결되어
 *     있으면 안 되는 상태로 만들어야 한다.
 * @return: 없음 (void).
 *
 * request_queue(디스크)가 소멸할 때, 그 큐를 참조하고 있던 임의의
 * 태스크들의 io_context에는 여전히 이 큐에 대한 icq가 남아있을 수
 * 있다. 이 함수는 ioc_release_fn()과 반대 방향(태스크가 아니라 큐를
 * 기준으로)으로 순회하며 같은 목적(icq unlink + 해제 예약)을 달성한다.
 * 동작 과정: (1) q->queue_lock을 잡는다 (ioc_release_fn과 달리 이
 * 함수는 항상 q lock을 먼저 잡는 입장이라 락 순서 문제가 상대적으로
 * 단순하다), (2) q->icq_list가 빌 때까지 반복하며 매번 리스트의 첫
 * icq를 얻는다, (3) 그 icq가 속한 ioc->lock을 추가로 잡는다 (q lock을
 * 이미 쥔 상태에서 ioc lock을 잡으므로 q -> ioc 순서가 자연히 지켜짐),
 * (4) ioc_destroy_icq()로 실제 unlink + RCU 해제 예약을 수행한다,
 * (5) ioc 락을 해제한다. 루프가 끝나면 q 락도 해제한다.
 * 실행 컨텍스트: 큐가 실제로 해제되는 경로(디스크 제거, elevator 전환
 * 등)의 프로세스 컨텍스트에서 호출된다. spin_lock_irq를 사용해 인터럽트
 * 컨텍스트에서의 q->icq_list 접근 가능성도 배제한다.
 * 호출자(caller): elevator.c의 큐/elevator 해제 경로 (elevator_exit
 * 계열에서 이전 elevator와 연결된 icq들을 정리할 때).
 * 피호출자(callee): ioc_destroy_icq().
 * 에러 처리: 실패 경로 없음.
 *
 * 호출 체인:
 *   elevator_exit() 등 큐 해제 경로 -> [ioc_clear_queue] -> ioc_destroy_icq() (반복)
 */
void ioc_clear_queue(struct request_queue *q)
{
	spin_lock_irq(&q->queue_lock); /* [한국어] q->icq_list를 순회/제거하는 동안 다른 CPU의 동시 접근(icq 추가 등)을 막기 위해 큐 락 획득, 인터럽트도 비활성화 */
	while (!list_empty(&q->icq_list)) { /* [한국어] 이 큐에 연결된 icq가 하나라도 남아있는 동안 반복 */
		struct io_cq *icq =
			list_first_entry(&q->icq_list, struct io_cq, q_node); /* [한국어] q->icq_list(list_head)의 첫 노드로부터 container_of로 io_cq 구조체 포인터 복원 - 매 반복마다 head를 하나씩 꺼내는 방식 */

		/*
		 * Other context won't hold ioc lock to wait for queue_lock, see
		 * details in ioc_release_fn().
		 */
		spin_lock(&icq->ioc->lock); /* [한국어] q lock을 이미 쥔 상태에서 ioc lock을 추가로 획득 - q -> ioc 순서이므로 ioc_release_fn의 trylock/재획득 로직과 달리 단순 블로킹 lock으로 충분 (다른 경로가 반대 순서로 잡지 않는다는 것이 원본 주석의 근거) */
		ioc_destroy_icq(icq); /* [한국어] 두 락을 모두 쥔 상태에서 icq를 ioc/q 양쪽 자료구조에서 unlink하고 RCU 해제 예약 */
		spin_unlock(&icq->ioc->lock); /* [한국어] ioc 락 해제 - q 락은 바깥 while 루프를 위해 계속 보유 */
	}
	spin_unlock_irq(&q->queue_lock); /* [한국어] 루프 종료 후(q->icq_list가 완전히 비었음) 큐 락 최종 해제 및 인터럽트 상태 복원 */
}
#else /* CONFIG_BLK_ICQ */ /* [한국어] icq 인프라(CONFIG_BLK_ICQ)가 비활성화된 커널 설정 - I/O 스케줄러가 태스크별 프라이빗 상태를 쓰지 않는 구성이므로 아래는 위 블록 함수들의 아무것도 하지 않는 대체 스텁 */
/*
 * [한국어]
 * ioc_exit_icqs - CONFIG_BLK_ICQ 비활성 시 사용되는 no-op 스텁
 *
 * @ioc: 사용되지 않음 (icq 인프라 자체가 없으므로 정리할 icq가 없음).
 * @return: 없음 (void).
 *
 * icq 인프라가 빌드에서 완전히 빠진 설정에서는 io_context에 icq_list
 * 필드 자체가 없다(#ifdef CONFIG_BLK_ICQ로 그 필드들도 컴파일에서
 * 제외됨, alloc_io_context 참고). 따라서 exit_io_context()가 호출하는
 * 이 함수는 아무 일도 하지 않아도 되며, static inline으로 정의해
 * 호출부 오버헤드조차 없앤다.
 * 실행 컨텍스트: exit_io_context()와 동일 (프로세스 컨텍스트).
 * 호출자(caller): exit_io_context().
 * 피호출자(callee): 없음.
 *
 * 호출 체인:
 *   exit_io_context() -> [ioc_exit_icqs] (아무 동작 없음)
 */
static inline void ioc_exit_icqs(struct io_context *ioc)
{
}
/*
 * [한국어]
 * ioc_delay_free - CONFIG_BLK_ICQ 비활성 시 사용되는 스텁 (항상 즉시 해제 가능)
 *
 * @ioc: 사용되지 않음.
 * @return: 항상 false - icq 자체가 없으므로 정리를 지연할 이유가 없고,
 *          호출자(put_io_context)는 항상 그 자리에서 바로 io_context를
 *          해제해도 안전하다.
 *
 * CONFIG_BLK_ICQ가 꺼진 설정에서는 icq_list/lock 등의 필드가 io_context
 * 구조체에서 아예 빠지므로(#ifdef), 워크큐로 미룰 대상(icq)도 없고
 * 이중 락 순서 문제도 발생하지 않는다. 따라서 항상 false를 반환해
 * put_io_context()가 바로 kmem_cache_free를 수행하도록 한다.
 * 실행 컨텍스트: put_io_context()와 동일.
 * 호출자(caller): put_io_context().
 * 피호출자(callee): 없음.
 *
 * 호출 체인:
 *   put_io_context() -> [ioc_delay_free] (항상 false) -> kmem_cache_free()
 */
static inline bool ioc_delay_free(struct io_context *ioc)
{
	return false; /* [한국어] icq 인프라가 없는 빌드에서는 지연 해제가 필요 없으므로 항상 false를 반환해 호출자가 즉시 해제하도록 함 */
}
#endif /* CONFIG_BLK_ICQ */ /* [한국어] CONFIG_BLK_ICQ 조건부 컴파일 블록(icq 인프라 활성 버전 vs 스텁 버전) 종료 */

/**
 * put_io_context - put a reference of io_context
 * @ioc: io_context to put
 *
 * Decrement reference count of @ioc and release it if the count reaches
 * zero.
 */
/*
 * [한국어]
 * put_io_context - io_context 참조를 하나 반환하고, 마지막 참조면 해제까지 처리한다
 *
 * @ioc: 참조를 반환할 io_context. 호출자가 이 시점까지 유효한 참조를
 *       하나 들고 있었어야 한다 (그렇지 않으면 이미 0인 refcount를
 *       또 감소시키는 이중 반환 버그).
 * @return: 없음 (void). 이 함수 호출 이후 호출자는 더 이상 @ioc를
 *          역참조해서는 안 된다 (이 함수가 마지막 참조였다면 이미
 *          해제되었거나 해제가 예약된 상태이기 때문).
 *
 * io_context는 request->elv.icq를 통해 request 하나하나, 그리고
 * CLONE_IO로 io_context를 공유하는 여러 태스크로부터 동시에 참조될
 * 수 있는 공유 객체다. 이 함수는 그 참조 중 하나를 반환하며, 반환한
 * 참조가 마지막 참조였다면(refcount가 0에 도달) 실제 메모리 해제까지
 * 담당한다.
 * 동작 과정: (1) refcount가 이미 0 이하가 아닌지 방어적으로 확인
 * (BUG_ON), (2) atomic_long_dec_and_test()로 원자적으로 감소시키면서
 * 그 결과가 0인지 검사, (3) 0이 되었다면 ioc_delay_free()를 호출해
 * "아직 정리할 icq가 남아있어 워크큐로 미뤄야 하는지" 판단, (4) 미룰
 * 필요가 없다면(ioc_delay_free가 false 반환) 그 자리에서 바로
 * kmem_cache_free로 io_context 메모리를 해제.
 * 실행 컨텍스트: 임의의 프로세스 컨텍스트(요청 완료 경로, 태스크 종료
 * 경로 등)에서 호출될 수 있으며, 이 함수 자체는 스핀락을 직접 잡지
 * 않지만 ioc_delay_free() 내부에서 ioc->lock을 잡는다.
 * 호출자(caller): exit_io_context()(태스크 종료 시), BFQ 등 elevator의
 * request 완료 경로(rq->elv.icq->ioc에 대해 호출), ioc_find_get_icq()의
 * 에러 경로(icq 생성 실패 시 되돌리기 위해).
 * 피호출자(callee): ioc_delay_free(), kmem_cache_free().
 * 에러 처리: 별도 에러 반환 없음. 선행 조건(양의 refcount) 위반은
 * BUG_ON으로 즉시 패닉.
 *
 * 호출 체인:
 *   exit_io_context() / bfq_finish_request() / ioc_find_get_icq() 실패 경로
 *       -> [put_io_context] -> ioc_delay_free() -> (필요 시) kmem_cache_free()
 */
void put_io_context(struct io_context *ioc)
{
	BUG_ON(atomic_long_read(&ioc->refcount) <= 0); /* [한국어] refcount가 이미 0 이하라면 이중 반환(use-after-free 유발)이므로 즉시 패닉시켜 조기에 잡아냄 */
	if (atomic_long_dec_and_test(&ioc->refcount) && !ioc_delay_free(ioc)) /* [한국어] refcount를 원자적으로 1 감소시키고 결과가 정확히 0인지 검사(dec_and_test) - 0이 됐고, 동시에 ioc_delay_free가 "지연 불필요(icq 없음)"를 뜻하는 false를 반환했을 때만 아래에서 직접 해제 */
		kmem_cache_free(iocontext_cachep, ioc); /* [한국어] 마지막 참조였고 남은 icq도 없으므로 io_context 메모리를 즉시 slab 캐시로 반환 */
}
EXPORT_SYMBOL_GPL(put_io_context); /* [한국어] GPL 라이선스 모듈에서만 사용 가능하도록 심볼을 export - BFQ/mq-deadline 등 elevator 모듈 및 여러 블록 계층 코드가 이 함수를 호출하기 위해 필요 */

/* Called by the exiting task */
/*
 * [한국어]
 * exit_io_context - 종료 중인 태스크로부터 io_context를 분리하고 필요 시 정리한다
 *
 * @task: 종료 처리 중인 태스크(보통 current). 호출 전에 task->io_context가
 *        NULL이 아님이 보장되어야 한다 - 실제로 유일한 호출자인
 *        kernel/exit.c의 do_exit()는 `if (tsk->io_context)` 검사를 거친
 *        뒤에만 이 함수를 호출한다.
 * @return: 없음 (void).
 *
 * io_context는 CLONE_IO로 여러 태스크가 공유할 수 있으므로, 참조카운트
 * (refcount)와 별개로 "이 io_context를 활성 상태로 사용 중인 태스크가
 * 몇 개인가"를 나타내는 active_ref 카운터를 둔다. 태스크가 종료할 때는
 * task_struct->io_context 링크를 끊고 active_ref를 감소시키며, 그 값이
 * 0이 되어 더 이상 어떤 태스크도 이 io_context를 활성적으로 쓰고 있지
 * 않을 때만 icq exit 처리와 refcount 반환(put)을 수행한다.
 * 동작 과정: (1) task_lock으로 task_struct 갱신을 보호하며 io_context
 * 포인터를 꺼내고 task->io_context를 NULL로 만들어(이후 이 태스크가
 * 이 io_context를 다시 참조하지 못하도록) 링크를 끊는다, (2) task_unlock
 * 으로 락 해제, (3) active_ref를 원자적으로 감소시키고 그 결과가 0인지
 * 검사(dec_and_test), (4) 0이면(이 태스크가 마지막 사용자였다면)
 * ioc_exit_icqs()로 스케줄러 exit 콜백들을 모두 실행한 뒤
 * put_io_context()로 refcount 참조를 반환(필요 시 실제 메모리 해제까지
 * 이어짐).
 * 실행 컨텍스트: 태스크 자신의 종료 경로(do_exit)에서, 그 태스크 자신의
 * 프로세스 컨텍스트로 실행된다 (다른 태스크를 위해 호출되지 않음 -
 * "Called by the exiting task"라는 원본 주석이 이를 명시).
 * 호출자(caller): kernel/exit.c의 do_exit() (task->io_context가 NULL이
 * 아닐 때만).
 * 피호출자(callee): ioc_exit_icqs(), put_io_context() (active_ref가
 * 0이 되었을 때만).
 * 에러 처리: 실패 경로 없음.
 *
 * 호출 체인:
 *   do_exit() -> [exit_io_context] -> ioc_exit_icqs() -> put_io_context()
 */
void exit_io_context(struct task_struct *task)
{
	struct io_context *ioc; /* [한국어] task로부터 분리해 낼 io_context를 임시로 담아둘 지역 변수 */

	task_lock(task); /* [한국어] task_struct->io_context 필드를 다른 스레드(예: set_task_ioprio, __copy_io)와 동시에 읽고 쓰지 않도록 태스크 락 획득 */
	ioc = task->io_context; /* [한국어] 현재 이 태스크가 가리키고 있는 io_context 포인터를 로컬 변수로 복사 - 호출자(do_exit)가 이미 NULL이 아님을 확인했으므로 유효한 포인터 */
	task->io_context = NULL; /* [한국어] task_struct에서 io_context 링크를 끊음 - 태스크가 곧 사라지므로 더 이상 이 태스크를 통해 io_context를 참조할 수 없게 함 */
	task_unlock(task); /* [한국어] task_struct 보호 락 해제 - 아래의 active_ref 조작은 io_context 자체의 원자 연산이라 task_lock이 더 이상 필요 없음 */

	if (atomic_dec_and_test(&ioc->active_ref)) { /* [한국어] "이 io_context를 활성적으로 쓰는 태스크 수"를 원자적으로 1 감소시키고 0이 됐는지 검사 - CLONE_IO로 여러 태스크가 공유 중이면 아직 0이 아닐 수 있어 그 경우 아래 정리를 건너뜀 */
		ioc_exit_icqs(ioc); /* [한국어] 마지막 사용자였으므로 이 io_context에 연결된 모든 icq에 대해 스케줄러 exit 콜백을 실행해 프라이빗 상태를 정리 */
		put_io_context(ioc); /* [한국어] active_ref와 별개로 관리되는 refcount 참조를 하나 반환 - 이 태스크가 원래 들고 있던 refcount 몫을 놓아주며, 이게 마지막 refcount였다면 실제 해제까지 이어짐 */
	}
}

/*
 * [한국어]
 * alloc_io_context - 새 io_context를 할당하고 초기 상태로 세팅한다
 *
 * @gfp_flags: 할당 시 사용할 GFP 플래그(예: GFP_ATOMIC - 인터럽트/락
 *             보유 컨텍스트, GFP_KERNEL - 블로킹 가능한 일반 컨텍스트).
 *             호출 컨텍스트에 따라 호출자가 적절히 선택해 전달한다.
 * @node: NUMA 선호 노드 (NUMA_NO_NODE면 특정 노드 선호 없음, 또는
 *        q->node처럼 관련 디바이스가 붙은 노드를 지정해 지역성을 높임).
 * @return: 성공 시 refcount=1, active_ref=1로 초기화된 새 io_context
 *          포인터. 실패(slab 할당 실패) 시 NULL.
 *
 * io_context는 프로세스 최초 I/O 발행 시점 또는 ioprio 설정 시점에
 * 지연 할당(lazy allocation)되는 객체다. 이 함수는 그 할당과 최소
 * 초기 상태 구성을 한 곳에 모아 여러 호출자(icq 최초 생성, ioprio 설정,
 * fork 시 복제)가 일관된 초기 상태를 얻도록 한다.
 * 동작 과정: (1) __GFP_ZERO를 추가해 슬랩에서 0으로 초기화된 메모리를
 * 할당(모든 포인터/카운터가 안전한 0 상태로 시작), (2) 할당 실패 시
 * NULL 반환, (3) refcount를 1로, active_ref를 1로 설정(방금 만든 이
 * io_context를 사용할 첫 번째 활성 사용자가 자기 자신이라는 의미),
 * (4) CONFIG_BLK_ICQ가 활성화된 빌드에서만 icq 관련 필드(lock, radix
 * tree, hlist head, release work)를 초기화 - icq 인프라가 없는 빌드는
 * 이 필드들 자체가 구조체에 없으므로 초기화도 스킵, (5) ioprio를
 * IOPRIO_DEFAULT(스케줄러 기본 우선순위)로 설정, (6) 완성된 io_context
 * 포인터 반환.
 * 실행 컨텍스트: 호출자의 컨텍스트를 그대로 이어받는다 - GFP_ATOMIC로
 * 호출되면 스핀락을 쥔 채로도 안전하고, GFP_KERNEL로 호출되면 블로킹
 * 가능한 프로세스 컨텍스트여야 한다.
 * 호출자(caller): set_task_ioprio()(ioprio 설정 시 io_context가 아직
 * 없으면 새로 만듦), __copy_io()(fork 시 부모의 ioprio를 물려받는 새
 * io_context 생성), ioc_find_get_icq()(태스크에 io_context가 전혀 없는
 * 상태에서 icq를 요청받았을 때 최초 생성).
 * 피호출자(callee): kmem_cache_alloc_node(), atomic_long_set(),
 * atomic_set(), spin_lock_init(), INIT_RADIX_TREE(), INIT_HLIST_HEAD(),
 * INIT_WORK() (CONFIG_BLK_ICQ 활성 시).
 * 에러 처리: slab 할당 실패 시 NULL을 반환하고 호출자가 이를 검사해
 * 자신의 에러 경로(-ENOMEM 반환 등)로 전파.
 *
 * 호출 체인:
 *   set_task_ioprio() / __copy_io() / ioc_find_get_icq() -> [alloc_io_context]
 *       -> kmem_cache_alloc_node()
 */
static struct io_context *alloc_io_context(gfp_t gfp_flags, int node)
{
	struct io_context *ioc; /* [한국어] 새로 할당할 io_context를 가리킬 포인터 - 아직 할당 전이므로 미정 상태 */

	ioc = kmem_cache_alloc_node(iocontext_cachep, gfp_flags | __GFP_ZERO,
				    node); /* [한국어] iocontext_cachep slab에서 io_context 하나를 할당 - __GFP_ZERO를 추가로 OR해 할당된 메모리를 0으로 초기화(포인터/카운터가 안전한 초기값을 갖도록), node로 NUMA 지역성 힌트 전달 */
	if (unlikely(!ioc)) /* [한국어] 메모리 부족 등으로 slab 할당이 실패했는지 확인 - unlikely()는 이 경로가 드물다는 컴파일러 힌트 */
		return NULL; /* [한국어] 할당 실패를 호출자에게 알림 - 호출자가 각자의 에러 처리(예: -ENOMEM 반환)를 수행 */

	atomic_long_set(&ioc->refcount, 1); /* [한국어] 참조카운트를 1로 초기화 - 지금 이 함수가 반환하는 포인터 자체가 그 첫 번째 참조 */
	atomic_set(&ioc->active_ref, 1); /* [한국어] 활성 사용자 카운트를 1로 초기화 - 방금 이 io_context를 만든 태스크 자신이 첫 번째(그리고 지금은 유일한) 활성 사용자 */
#ifdef CONFIG_BLK_ICQ /* [한국어] icq 인프라가 활성화된 빌드에서만 icq 관련 필드들을 초기화 - 비활성 빌드에서는 이 필드들이 struct io_context에 아예 존재하지 않음 */
	spin_lock_init(&ioc->lock); /* [한국어] icq_tree/icq_list/icq_hint 등을 보호할 io_context 전용 스핀락 초기화 */
	INIT_RADIX_TREE(&ioc->icq_tree, GFP_ATOMIC); /* [한국어] request_queue->id를 키로 icq를 빠르게 찾기 위한 radix tree 초기화 - 트리 내부 노드 할당은 GFP_ATOMIC으로 수행되도록 지정(락 보유 중에도 삽입 가능하게) */
	INIT_HLIST_HEAD(&ioc->icq_list); /* [한국어] 이 io_context에 연결된 모든 icq를 매다는 hlist head 초기화 (ioc_exit_icqs/ioc_release_fn이 순회하는 리스트) */
	INIT_WORK(&ioc->release_work, ioc_release_fn); /* [한국어] 지연 해제 시 실행할 워크큐 콜백을 release_work에 미리 바인딩 - ioc_delay_free가 이후 queue_work()로 이 work를 큐잉 */
#endif /* [한국어] CONFIG_BLK_ICQ 조건부 블록 종료 */
	ioc->ioprio = IOPRIO_DEFAULT; /* [한국어] I/O 우선순위를 커널 기본값으로 초기화 - set_task_ioprio가 나중에 명시적으로 값을 바꾸기 전까지 이 기본값이 사용됨 */

	return ioc; /* [한국어] 완전히 초기화된 io_context 포인터를 호출자에게 반환 */
}

/*
 * [한국어]
 * set_task_ioprio - 대상 태스크의 I/O 우선순위(ioprio)를 검증 후 설정한다
 *
 * @task: ioprio를 설정할 대상 태스크 (자기 자신이거나 ioprio_set()
 *        시스템 콜의 대상이 된 다른 프로세스/스레드일 수 있음).
 * @ioprio: IOPRIO_PRIO_VALUE() 등으로 인코딩된 새 I/O 우선순위 값
 *          (클래스 + 데이터로 구성, block/ioprio.c 참고).
 * @return: 0 성공. -EPERM (권한 없음 - 다른 사용자의 우선순위를 상승/
 *          변경할 CAP_SYS_NICE도 없고 uid도 일치하지 않음). 음수
 *          (security_task_setioprio LSM 훅이 거부한 에러 코드).
 *          -ENOMEM (io_context가 아직 없었는데 새로 할당하다 실패).
 *
 * ioprio_set() 시스템 콜(block/ioprio.c)이 임의의 프로세스/프로세스
 * 그룹/사용자에 속한 태스크들의 I/O 우선순위를 바꿀 수 있으므로, 대상이
 * 호출자 자신이 아닐 수 있다. 이 함수는 (1) 권한 검사, (2) LSM 보안
 * 훅 검사, (3) 대상 태스크에 io_context가 없으면 새로 할당, (4) 실제
 * ioprio 필드 갱신까지 전 과정을 처리한다.
 * 동작 과정:
 *  (1) 현재 태스크의 credential과 대상 태스크의 credential(RCU로 안전하게
 *      읽음)을 비교해, 대상의 uid가 호출자의 effective/real uid와 다르고
 *      CAP_SYS_NICE 권한도 없다면 -EPERM으로 거부한다.
 *  (2) security_task_setioprio() LSM 훅을 호출해 추가 보안 정책(SELinux
 *      등)의 승인을 받는다. 실패하면 그 에러 코드를 그대로 전파.
 *  (3) task_lock으로 task_struct를 보호하며 io_context가 있는지 확인.
 *      없다면(unlikely) 락을 놓고 GFP_ATOMIC으로 새 io_context를 할당한
 *      뒤(블로킹 없이 수행되어야 하므로 GFP_ATOMIC), 다시 락을 잡고 그
 *      사이 대상 태스크가 이미 종료 중(PF_EXITING)이거나 다른 경쟁자가
 *      먼저 io_context를 설치했는지 재확인한다 - 두 경우 모두 방금 할당한
 *      ioc는 버리고(kmem_cache_free) 기존 것을 쓴다(TOCTOU 경쟁 처리).
 *  (4) 최종적으로 유효한 task->io_context->ioprio 필드에 새 값을 쓴다.
 * 실행 컨텍스트: ioprio_set() 시스템 콜 경로의 프로세스 컨텍스트. 대상
 * 태스크가 자기 자신이 아닐 수 있어 RCU와 task_lock으로 안전하게
 * 접근한다.
 * 호출자(caller): block/ioprio.c의 sys_ioprio_set() - 단일 프로세스,
 * 프로세스 그룹(PGRP), 또는 사용자(USER) 소유의 모든 태스크에 대해
 * 반복 호출될 수 있다.
 * 피호출자(callee): current_cred(), __task_cred(), uid_eq(), capable(),
 * security_task_setioprio(), alloc_io_context(), kmem_cache_free().
 * 에러 처리: 각 단계의 실패는 즉시 해당 에러 코드로 반환되며, 부분적으로
 * 진행된 상태(예: io_context는 할당했지만 아직 연결 안 함)는 반드시
 * 정리(kmem_cache_free)된 뒤 반환된다.
 *
 * 호출 체인:
 *   sys_ioprio_set() (block/ioprio.c) -> [set_task_ioprio] -> alloc_io_context()
 */
int set_task_ioprio(struct task_struct *task, int ioprio)
{
	int err; /* [한국어] security_task_setioprio() LSM 훅의 반환값을 담을 지역 변수 */
	const struct cred *cred = current_cred(), *tcred; /* [한국어] cred: 호출자(현재 태스크)의 credential, tcred: 아래에서 대상 task의 credential을 담을 변수 - 두 credential을 비교해 권한을 검사할 것 */

	rcu_read_lock(); /* [한국어] __task_cred()로 다른 태스크의 credential을 읽는 동안, 그 태스크가 setuid 등으로 credential을 교체(RCU로 관리됨)하더라도 안전하게 이전 유효한 credential을 참조하기 위해 RCU 보호 구간 진입 */
	tcred = __task_cred(task); /* [한국어] 대상 task의 현재 credential을 RCU 보호 하에 읽음 - task->cred를 직접 읽지 않고 이 헬퍼를 쓰는 이유는 RCU 규칙을 올바르게 지키기 위함 */
	if (!uid_eq(tcred->uid, cred->euid) &&
	    !uid_eq(tcred->uid, cred->uid) && !capable(CAP_SYS_NICE)) { /* [한국어] 대상 태스크의 uid가 호출자의 effective uid와도, real uid와도 다르고, 게다가 호출자가 CAP_SYS_NICE(우선순위 조작 권한)도 없다면 - 즉 "내 프로세스가 아니고 관리자 권한도 없는" 경우 */
		rcu_read_unlock(); /* [한국어] 거부하고 반환하기 전에 RCU 보호 구간부터 정리 */
		return -EPERM; /* [한국어] 권한 없음 에러를 호출자(ioprio_set 시스템 콜)에 전파 - 사용자 공간에는 EPERM으로 노출됨 */
	}
	rcu_read_unlock(); /* [한국어] credential 비교가 끝났으므로 RCU 보호 구간 종료 - 더 이상 tcred를 참조하지 않음 */

	err = security_task_setioprio(task, ioprio); /* [한국어] LSM(Linux Security Module, 예: SELinux/AppArmor) 훅 호출 - 위의 DAC(uid/capability) 검사와 별개로 MAC 정책상 이 태스크의 ioprio를 바꿀 수 있는지 추가 검사 */
	if (err) /* [한국어] 보안 정책이 거부했는지 확인 */
		return err; /* [한국어] LSM이 반환한 에러 코드를 그대로 호출자에 전파 */

	task_lock(task); /* [한국어] 대상 task->io_context 필드를 다른 스레드(예: 그 태스크 자신의 exit_io_context, __copy_io)와 동시에 접근하지 않도록 태스크 락 획득 */
	if (unlikely(!task->io_context)) { /* [한국어] 대상 태스크가 지금까지 한 번도 io_context를 할당받은 적이 없는지 확인 - unlikely()는 대부분의 태스크가 이미 io_context를 갖고 있을 것이라는 힌트 */
		struct io_context *ioc; /* [한국어] 아래에서 새로 할당할 io_context를 담을 지역 변수 */

		task_unlock(task); /* [한국어] 메모리 할당(alloc_io_context)은 블로킹할 수 있으므로, 락을 쥔 채로 호출하면 안 되어 미리 해제 */

		ioc = alloc_io_context(GFP_ATOMIC, NUMA_NO_NODE); /* [한국어] 새 io_context 할당 시도 - GFP_ATOMIC을 쓰는 이유는 이 함수가 잠재적으로 스핀락을 쥔 호출 경로에서도 불릴 가능성을 배제하지 않기 위함, NUMA_NO_NODE는 특정 노드 선호 없음 */
		if (!ioc) /* [한국어] 할당 실패 확인 */
			return -ENOMEM; /* [한국어] 메모리 부족을 호출자에 전파 - 이 시점에는 아직 어떤 락도 쥐고 있지 않으므로 그냥 반환해도 안전 */

		task_lock(task); /* [한국어] 할당이 끝났으므로 다시 태스크 락을 잡고 최신 상태를 재확인(TOCTOU: 락을 놓은 사이 다른 스레드가 상태를 바꿨을 수 있음) */
		if (task->flags & PF_EXITING) { /* [한국어] 락을 놓았던 사이 대상 태스크가 이미 종료 절차(do_exit)에 들어갔는지 확인 - 종료 중인 태스크에 새 io_context를 연결하면 exit_io_context와 경쟁해 누수/UAF 위험 */
			kmem_cache_free(iocontext_cachep, ioc); /* [한국어] 이미 종료 중이므로 방금 할당한 io_context는 쓸모없어짐 - 즉시 반환해 누수 방지 */
			goto out; /* [한국어] ioprio 필드를 갱신하지 않고(종료 중인 태스크이므로 의미 없음) 바로 함수 마무리 단계로 이동 */
		}
		if (task->io_context) /* [한국어] 락을 놓았던 사이 다른 스레드(예: 동시에 들어온 또 다른 set_task_ioprio 호출, 또는 __copy_io)가 먼저 io_context를 설치했는지 확인 - 경쟁 상황 처리 */
			kmem_cache_free(iocontext_cachep, ioc); /* [한국어] 이미 다른 io_context가 설치되어 있으므로 방금 할당한 것은 중복 - 버림 */
		else
			task->io_context = ioc; /* [한국어] 경쟁자가 없었으므로 방금 할당한 io_context를 대상 태스크에 정식으로 연결 */
	}
	task->io_context->ioprio = ioprio; /* [한국어] (기존에 있었거나 방금 새로 연결된) io_context의 ioprio 필드를 요청받은 새 값으로 갱신 - 이후 이 태스크가 발행하는 I/O의 우선순위 결정에 사용됨 */
out: /* [한국어] PF_EXITING 조기 종료 경로와 정상 경로가 모두 모이는 레이블 - goto out으로 여기로 뛰어와 아래의 공통 락 해제/반환 코드를 공유 */
	task_unlock(task); /* [한국어] task_struct 보호 락 해제 */
	return 0; /* [한국어] 성공 반환 (PF_EXITING으로 조기 종료된 경우도 "권한/보안 검사는 통과했으나 이미 종료 중이라 적용할 곳이 없다"는 의미로 0을 반환 - 에러로 취급하지 않음) */
}
EXPORT_SYMBOL_GPL(set_task_ioprio); /* [한국어] GPL 모듈에서만 사용 가능하도록 export - block/ioprio.c의 시스템 콜 구현 및 일부 인트리 모듈이 이 함수를 호출 */

/*
 * [한국어]
 * __copy_io - fork/clone 시 부모 태스크의 io_context를 공유하거나 우선순위만 복제한다
 *
 * @clone_flags: clone(2)/fork()에 전달된 클론 플래그 (u64) - CLONE_IO
 *               비트가 설정되어 있으면 "I/O 컨텍스트를 부모와 공유하라"는
 *               사용자 요청(스레드가 부모와 같은 I/O 우선순위/컨텍스트를
 *               쓰고 싶을 때, 예: 스레드풀에서 상속받은 ioprio를 유지).
 * @tsk: 새로 생성 중인 자식 task_struct. 아직 스케줄러에 등록되기 전
 *       단계로, tsk->io_context는 이 함수 호출 전에는 미정 상태.
 * @return: 0 성공. -ENOMEM (CLONE_IO가 없고 부모의 ioprio가 유효한 값
 *          이어서 새 io_context를 만들어야 하는데 할당 실패한 경우).
 *
 * copy_io() 인라인 래퍼(include/linux/iocontext.h)가 이 함수를 호출하기
 * 전에 이미 current->io_context가 NULL이면 그냥 0을 반환해 두었으므로,
 * 이 함수에 들어올 때는 current->io_context가 항상 유효한 포인터임이
 * 보장된다.
 * 동작 과정: (1) 현재(부모) 태스크의 io_context를 얻는다, (2) CLONE_IO가
 * 설정되어 있으면 - 부모와 자식이 "같은" io_context를 물리적으로
 * 공유해야 하므로 active_ref만 증가시키고(공유 사용자가 하나 늘어남을
 * 표시) 자식의 io_context 포인터를 부모 것과 동일하게 설정한다(같은
 * 객체를 가리킴, 새로 할당하지 않음), (3) CLONE_IO가 없지만 부모의
 * ioprio가 유효한(디폴트가 아닌) 값이면 - 공유는 하지 않되 그 우선순위
 * 값만 물려주기 위해 자식용으로 완전히 새로운 io_context를 할당하고
 * ioprio 필드만 복사한다, (4) CLONE_IO도 없고 ioprio도 기본값이면 -
 * 아무 것도 하지 않아 자식은 필요할 때 지연 할당(lazy alloc)으로
 * 자기만의 io_context를 나중에 갖게 된다.
 * 실행 컨텍스트: fork()/clone() 시스템 콜 처리 중인 프로세스 컨텍스트
 * (kernel/fork.c의 copy_process() 내부). 자식 태스크는 아직 실행을
 * 시작하지 않았으므로 tsk 필드에 대한 동시 접근 경쟁은 없다.
 * 호출자(caller): copy_io() 인라인 함수 (include/linux/iocontext.h) -
 * kernel/fork.c의 copy_process()가 자식 task_struct 초기화 단계에서
 * 호출한다.
 * 피호출자(callee): atomic_inc(), alloc_io_context(), ioprio_valid().
 * 에러 처리: 새 io_context 할당 실패 시 -ENOMEM을 반환하며, 이는
 * copy_process()가 자식 생성 전체를 실패로 되돌리는 신호로 쓰인다
 * (fork 자체가 실패로 처리됨).
 *
 * 호출 체인:
 *   copy_process() (kernel/fork.c) -> copy_io() (인라인) -> [__copy_io]
 *       -> alloc_io_context()
 */
int __copy_io(u64 clone_flags, struct task_struct *tsk)
{
	struct io_context *ioc = current->io_context; /* [한국어] fork를 호출한 부모(현재 태스크)의 io_context - copy_io() 래퍼가 이미 NULL이 아님을 확인했으므로 안전하게 역참조 가능 */

	/*
	 * Share io context with parent, if CLONE_IO is set
	 */
	if (clone_flags & CLONE_IO) { /* [한국어] 사용자가 clone(2)에 CLONE_IO 플래그를 지정했는지 확인 - 지정 시 부모와 자식이 "같은" io_context 객체를 물리적으로 공유(예: io_context에 딸린 icq/스케줄러 상태까지 그대로 공유하고 싶을 때) */
		atomic_inc(&ioc->active_ref); /* [한국어] 공유하는 io_context의 활성 사용자 수를 1 증가 - 이제 부모와 자식 둘 다 이 io_context의 "활성 사용자"이므로, 나중에 각자 exit_io_context에서 감소시킬 때 마지막 사용자만 실제 정리를 트리거하게 됨 */
		tsk->io_context = ioc; /* [한국어] 자식의 io_context 포인터를 부모 것과 동일한 객체로 설정 - 새로 할당하지 않고 같은 메모리를 가리키므로 이후 ioprio 등의 변경이 양쪽에 모두 영향을 줌 */
	} else if (ioprio_valid(ioc->ioprio)) { /* [한국어] CLONE_IO는 없지만, 부모의 ioprio가 "설정된 적 있는 유효한 값"인지 확인(디폴트가 아니라 명시적으로 설정된 우선순위) - 이 경우 공유는 원치 않아도 우선순위 값만은 물려주고 싶다는 의미 */
		tsk->io_context = alloc_io_context(GFP_KERNEL, NUMA_NO_NODE); /* [한국어] 자식 전용의 완전히 새로운 io_context를 할당 - fork 경로는 블로킹 가능한 프로세스 컨텍스트이므로 GFP_KERNEL 사용 가능, NUMA_NO_NODE는 특정 노드 선호 없음 */
		if (!tsk->io_context) /* [한국어] 새 io_context 할당이 실패했는지 확인 */
			return -ENOMEM; /* [한국어] 메모리 부족을 호출자(copy_io/copy_process)에 전파 - fork 자체가 실패로 처리되도록 함 */
		tsk->io_context->ioprio = ioc->ioprio; /* [한국어] 새로 만든 자식 전용 io_context에 부모의 ioprio 값만 복사 - 객체는 별개이므로 이후 부모/자식이 각자 독립적으로 ioprio를 바꿀 수 있음 */
	}

	return 0; /* [한국어] 성공 반환 - CLONE_IO도 없고 ioprio도 기본값이었던 경우(else 분기 자체를 안 탐)에도 이 줄까지 흘러와 0을 반환하며, 이때 자식은 io_context가 여전히 NULL인 상태로 남아 나중에 필요 시 지연 할당됨 */
}

#ifdef CONFIG_BLK_ICQ /* [한국어] icq 인프라가 활성화된 빌드에서만 컴파일되는 두 번째 블록 - I/O 발행 경로에서 실제로 icq를 조회/생성하는 함수들 */
/**
 * ioc_lookup_icq - lookup io_cq from ioc in io issue path
 * @q: the associated request_queue
 *
 * Look up io_cq associated with @ioc - @q pair from @ioc.  Must be called
 * from io issue path, either return NULL if current issue io to @q for the
 * first time, or return a valid icq.
 */
/*
 * [한국어]
 * ioc_lookup_icq - 현재 태스크의 io_context에서 특정 큐에 대응하는 icq를 락-프리로 조회한다
 *
 * @q: 조회 대상 request_queue.
 * @return: current->io_context와 @q 쌍에 대응하는 유효한 io_cq 포인터,
 *          또는 아직 이 태스크가 @q에 I/O를 발행한 적이 없다면 NULL.
 *
 * I/O 발행 경로(submit_bio 계열)에서 매 I/O마다 icq를 찾아야 하는데,
 * 이 경로는 매우 빈번하게 실행되는 핫패스이므로 스핀락 없이 RCU와
 * 캐시 힌트만으로 조회를 끝내는 것이 중요하다. icq는 두 가지 방법으로
 * 인덱싱된다: (1) 가장 최근에 성공한 조회 결과를 캐싱하는 icq_hint
 * 포인터(RCU 보호), (2) request_queue->id를 키로 하는 정식 radix tree
 * (역시 RCU 보호 하에 조회 가능). hint가 맞아떨어지면 트리 탐색 없이
 * O(1)로 끝나고, 아니면 트리를 정식으로 탐색한 뒤 결과를 다시 hint에
 * 캐싱해 다음 조회를 더 빠르게 만든다.
 * 동작 과정: (1) rcu_read_lock()으로 RCU 읽기 보호 구간 진입 - 이
 * 구간 동안 icq가 실제로 free(kfree_rcu)되지 않음을 보장, (2) icq_hint를
 * RCU-safe하게 역참조하고 그 hint가 가리키는 icq가 정말 @q에 속하는지
 * 확인 - 맞으면 바로 out으로 점프, (3) hint가 없거나 다른 큐를 가리키면
 * radix_tree_lookup으로 @q->id를 키로 정식 탐색, (4) 찾은 icq가 실제로
 * @q에 속하면 그 결과를 icq_hint에 다시 캐싱(다음 조회를 빠르게 하기
 * 위함, 캐싱 자체는 경쟁해도 안전), 아니면 NULL로 정리, (5)
 * rcu_read_unlock() 후 결과 반환.
 * 실행 컨텍스트: I/O 발행 경로의 프로세스 컨텍스트 - "io issue path
 * ensures both request_queue and current task are valid"라는 원본
 * 주석대로, 호출자가 이미 @q와 current 태스크의 생존을 보장한 상태에서
 * 호출되어야 한다. rcu_read_lock 구간 자체는 짧은 락-프리 구간.
 * 호출자(caller): ioc_find_get_icq()(기존 icq가 있는지 먼저 확인),
 * BFQ의 bfq_bic_lookup() 같은 elevator의 조회 전용 헬퍼.
 * 피호출자(callee): rcu_dereference(), radix_tree_lookup(),
 * rcu_assign_pointer().
 * 에러 처리: "찾지 못함"은 에러가 아니라 정상적인 NULL 반환으로 표현되며
 * (해당 큐에 대한 첫 I/O를 뜻함), 호출자가 필요 시 ioc_create_icq()로
 * 새로 만든다.
 *
 * 호출 체인:
 *   ioc_find_get_icq() / bfq_bic_lookup() -> [ioc_lookup_icq]
 *       -> radix_tree_lookup() / rcu_dereference(icq_hint)
 */
struct io_cq *ioc_lookup_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* [한국어] 현재 태스크(호출자 자신)의 io_context - 이 함수는 항상 current 기준으로만 조회하며 임의의 다른 태스크에 대해서는 조회하지 않음 */
	struct io_cq *icq; /* [한국어] 조회 결과를 담을 지역 변수 - 최종적으로 유효한 icq 또는 NULL이 됨 */

	/*
	 * icq's are indexed from @ioc using radix tree and hint pointer,
	 * both of which are protected with RCU, io issue path ensures that
	 * both request_queue and current task are valid, the found icq
	 * is guaranteed to be valid until the io is done.
	 */
	rcu_read_lock(); /* [한국어] RCU 읽기 보호 구간 시작 - 이 구간이 끝날 때까지는 다른 스레드가 ioc_destroy_icq()에서 kfree_rcu()로 예약한 icq라도 실제 메모리가 회수되지 않음을 보장 */
	icq = rcu_dereference(ioc->icq_hint); /* [한국어] 마지막으로 성공한 조회를 캐싱해 둔 hint 포인터를 RCU-safe하게 읽음 - 일반 포인터 대입이 아니라 rcu_dereference를 쓰는 이유는 컴파일러/CPU 재정렬로부터 안전한 순서를 보장받기 위함 */
	if (icq && icq->q == q) /* [한국어] hint가 존재하고, 그 hint가 정확히 지금 찾는 @q를 가리키는지 확인 - 다른 큐에 대한 hint라면 이번 조회에는 쓸모없음 */
		goto out; /* [한국어] hint 적중 - radix tree 탐색 없이 바로 반환 경로로 이동 (핫패스 최적화) */

	icq = radix_tree_lookup(&ioc->icq_tree, q->id); /* [한국어] hint가 없거나 빗나갔으므로 정식 radix tree에서 request_queue의 고유 id를 키로 탐색 */
	if (icq && icq->q == q) /* [한국어] 트리에서 뭔가 찾았고, 그것이 정말 @q에 속하는지 재확인(방어적 검증 - id 재사용 등의 극단적 상황 대비) */
		rcu_assign_pointer(ioc->icq_hint, icq);	/* allowed to race */ /* [한국어] 이번 조회 결과를 hint로 캐싱해 다음 조회를 빠르게 함 - 원본 주석대로 여러 CPU가 동시에 이 대입을 해도 경쟁이 허용됨(RCU 포인터 대입 자체가 원자적이고, 최악의 경우 캐시가 살짝 오래된 값이 되는 정도라 안전) */
	else
		icq = NULL; /* [한국어] 트리에도 없거나 큐가 일치하지 않으면 "아직 이 큐에 대한 icq가 없다"는 뜻으로 명시적으로 NULL 처리 */
out: /* [한국어] hint 적중(위쪽 goto out) 또는 트리 탐색 완료 양쪽 경로가 모두 모이는 레이블 */
	rcu_read_unlock(); /* [한국어] RCU 보호 구간 종료 - 이 시점 이후로는 icq가 다른 스레드에 의해 해제될 수 있으므로, 반환된 icq를 계속 안전하게 쓰려면 호출자가 별도의 참조나 RCU 구간을 유지해야 함 */
	return icq; /* [한국어] 찾은 icq(유효) 또는 NULL(해당 큐에 대한 첫 I/O)을 호출자에게 반환 */
}
EXPORT_SYMBOL(ioc_lookup_icq); /* [한국어] 일반 EXPORT_SYMBOL(비-GPL 모듈에서도 사용 가능)로 export - 오래전부터 있던 심볼이라 GPL 제한 없이 넓게 공개된 상태로 유지 */

/**
 * ioc_create_icq - create and link io_cq
 * @q: request_queue of interest
 *
 * Make sure io_cq linking @ioc and @q exists.  If icq doesn't exist, they
 * will be created using @gfp_mask.
 *
 * The caller is responsible for ensuring @ioc won't go away and @q is
 * alive and will stay alive until this function returns.
 */
/*
 * [한국어]
 * ioc_create_icq - 현재 태스크의 io_context와 지정된 큐를 잇는 새 icq를 할당하고 양쪽에 연결한다
 *
 * @q: icq를 새로 만들 대상 request_queue. 호출자가 이 함수 반환 전까지
 *     @q가 살아있음을 보장해야 한다 (원본 주석의 책임 분담 명시).
 * @return: 성공적으로 새로 만들었거나(또는 그 사이 다른 스레드가 이미
 *          만들어 둔 것을 찾았다면) 유효한 io_cq 포인터. 메모리 할당
 *          실패 시 NULL. (참고: 커널 독스트링의 "@gfp_mask"는 과거
 *          버전의 흔적으로, 현재 시그니처에는 그런 파라미터가 없고
 *          내부적으로 GFP_ATOMIC을 고정 사용한다 - 원본 코드/주석이므로
 *          그대로 유지.)
 *
 * ioc_lookup_icq()로 찾지 못한 경우(이 태스크가 이 큐에 처음 I/O를
 * 보내는 경우), 실제로 elevator_type이 정의한 크기(icq_size,
 * bfq_io_cq처럼 struct io_cq를 감싸는 확장 구조체)만큼 메모리를 할당하고,
 * ioc 쪽 자료구조(icq_tree, icq_list)와 q 쪽 자료구조(q->icq_list) 양쪽에
 * 모두 연결해야 icq가 "완성"된다. 두 자료구조에 동시에 연결하려면 두
 * 락(q->queue_lock, ioc->lock)을 모두 잡아야 하고, 그 사이 다른 스레드가
 * 먼저 같은 (ioc,q) 쌍의 icq를 만들었을 수도 있는 경쟁 상황까지 처리한다.
 * 동작 과정:
 *  (1) 현재 elevator_type의 icq_cache(스케줄러가 등록해 둔, 자신의
 *      확장 구조체 크기에 맞는 전용 slab)에서 icq 메모리를 GFP_ATOMIC
 *      + __GFP_ZERO로 할당(제로 초기화).
 *  (2) 할당 실패 시 NULL 반환.
 *  (3) radix_tree_maybe_preload()로 이후의 radix_tree_insert가 락을
 *      쥔 상태에서도 블로킹 없이 성공할 수 있도록 트리 내부 노드용
 *      메모리를 미리 확보(preload). 실패하면 방금 할당한 icq를 되돌리고
 *      NULL 반환.
 *  (4) icq의 기본 필드(ioc, q)를 채우고 q_node/ioc_node 연결 리스트
 *      헤더를 초기화 - 아직 어느 리스트에도 실제로 매달리지 않은 상태.
 *  (5) q->queue_lock과 ioc->lock을 정해진 순서(q 먼저, ioc 나중)로
 *      잡는다.
 *  (6) radix_tree_insert로 ioc->icq_tree에 삽입을 시도한다:
 *      - 성공(likely, 대부분의 경우 경쟁자가 없음)하면 hlist/list에도
 *        추가하고, elevator가 init_icq 콜백을 등록해 두었다면 호출해
 *        스케줄러 프라이빗 상태를 초기화한다.
 *      - 실패(이미 같은 키로 다른 스레드가 먼저 삽입 완료 - 경쟁에서
 *        짐)하면 방금 할당한 icq는 버리고(kmem_cache_free), 대신
 *        ioc_lookup_icq()로 그 "승자" icq를 찾아 반환한다. 그것마저
 *        못 찾으면(이론상 있어서는 안 되는 상황) 에러 로그를 남긴다.
 *  (7) 두 락을 해제하고 preload를 종료한 뒤, 최종 icq(새로 만든 것이든
 *      경쟁에서 진 뒤 찾은 것이든)를 반환.
 * 실행 컨텍스트: I/O 발행 경로의 프로세스 컨텍스트. GFP_ATOMIC을 쓰는
 * 이유는 이 함수가 (경우에 따라) 블로킹이 허용되지 않는 경로에서도
 * 호출될 수 있기 때문.
 * 호출자(caller): ioc_find_get_icq() - ioc_lookup_icq()로 기존 icq를
 * 찾지 못했을 때만 호출.
 * 피호출자(callee): kmem_cache_alloc_node(), radix_tree_maybe_preload(),
 * radix_tree_insert(), hlist_add_head(), list_add(),
 * et->ops.init_icq(), kmem_cache_free(), ioc_lookup_icq(),
 * radix_tree_preload_end().
 * 에러 처리: 메모리/트리 preload 실패는 NULL 반환으로 전파. 삽입 경쟁
 * 실패는 에러가 아니라 "이미 다른 스레드가 만든 것을 재사용"하는
 * 정상적인 경쟁 처리 경로이며, 오직 승자를 찾지 못하는 이론상 불가능한
 * 상황만 printk 경고로 남긴다(원본 주석의 "cfq:"는 이 코드가 과거
 * CFQ 스케줄러 시절부터 이어져 온 레거시 문자열).
 *
 * 호출 체인:
 *   ioc_find_get_icq() -> [ioc_create_icq] -> radix_tree_insert() 성공 시
 *       et->ops.init_icq() / 실패 시 ioc_lookup_icq()
 */
static struct io_cq *ioc_create_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* [한국어] 현재 태스크의 io_context - 이 함수도 ioc_lookup_icq처럼 항상 current 기준으로 동작 */
	struct elevator_type *et = q->elevator->type; /* [한국어] 이 큐에 현재 붙어있는 elevator(I/O 스케줄러) 타입 - icq_cache(전용 slab)와 init_icq 콜백 정보를 여기서 얻음 */
	struct io_cq *icq; /* [한국어] 새로 할당하거나 최종적으로 반환할 icq를 가리킬 포인터 */

	/* allocate stuff */
	icq = kmem_cache_alloc_node(et->icq_cache, GFP_ATOMIC | __GFP_ZERO,
				    q->node); /* [한국어] elevator가 등록해 둔 전용 slab(icq_cache)에서 icq(또는 그 확장 구조체) 메모리를 할당 - 스케줄러마다 icq_size/icq_align이 달라 별도 slab을 씀, __GFP_ZERO로 제로 초기화, q->node로 NUMA 지역성 확보 */
	if (!icq) /* [한국어] 할당 실패 확인 */
		return NULL; /* [한국어] 메모리 부족을 호출자(ioc_find_get_icq)에 전파 */

	if (radix_tree_maybe_preload(GFP_ATOMIC) < 0) { /* [한국어] 아래에서 락을 쥔 채 실행할 radix_tree_insert가 내부적으로 새 트리 노드를 할당해야 할 수도 있는데, 락 보유 중 블로킹 할당은 불가하므로 미리 필요한 메모리를 이 스레드의 percpu 예비 풀에 채워둠(preload). 그 예비 확보 자체가 실패했는지 확인 */
		kmem_cache_free(et->icq_cache, icq); /* [한국어] preload 실패로 삽입을 진행할 수 없으므로 방금 할당한 icq를 되돌려 누수 방지 */
		return NULL; /* [한국어] 실패를 호출자에 전파 */
	}

	icq->ioc = ioc; /* [한국어] icq가 어느 io_context에 속하는지 기록 - 이후 ioc_destroy_icq/ioc_release_fn 등이 icq->ioc로 역참조할 때 사용 */
	icq->q = q; /* [한국어] icq가 어느 request_queue에 속하는지 기록 - ioc_lookup_icq의 hint 검증(icq->q == q)과 ioc_destroy_icq의 unlink 로직에서 사용 */
	INIT_LIST_HEAD(&icq->q_node); /* [한국어] q->icq_list에 연결될 리스트 노드를 자기 자신을 가리키는 빈 상태로 초기화 - 아직 실제 리스트에는 매달리지 않음 */
	INIT_HLIST_NODE(&icq->ioc_node); /* [한국어] ioc->icq_list(hlist)에 연결될 노드를 초기화 - 마찬가지로 아직 매달리지 않은 상태 */

	/* lock both q and ioc and try to link @icq */
	spin_lock_irq(&q->queue_lock); /* [한국어] q->icq_list와 icq_hint를 안전하게 갱신하기 위해 큐 락을 먼저 획득 (인터럽트도 비활성화) - q -> ioc 순서를 지키는 것이 이 파일 전체의 락 순서 규약 */
	spin_lock(&ioc->lock); /* [한국어] 이어서 ioc 락을 획득 - 이미 q 락으로 인터럽트가 비활성화된 상태이므로 여기서는 _irq 변형이 필요 없음(중첩 스핀락) */

	if (likely(!radix_tree_insert(&ioc->icq_tree, q->id, icq))) { /* [한국어] q->id를 키로 ioc->icq_tree에 이 icq를 삽입 시도 - 반환값 0(성공)이면 !0은 true가 되어 이 분기로 진입, likely()는 보통 경쟁자가 없어 성공한다는 힌트. 같은 키가 이미 있으면(다른 스레드가 먼저 만듦) 음수 에러를 반환해 이 분기를 타지 않음 */
		hlist_add_head(&icq->ioc_node, &ioc->icq_list); /* [한국어] radix tree 삽입에 성공했으므로 ioc->icq_list(hlist)에도 추가 - 이제 ioc_exit_icqs/ioc_release_fn의 순회 대상이 됨 */
		list_add(&icq->q_node, &q->icq_list); /* [한국어] q->icq_list에도 추가 - 이제 ioc_clear_queue의 순회 대상이 됨. 이 시점부터 icq는 ioc와 q 양쪽에서 모두 "공식적으로 존재"하는 상태 */
		if (et->ops.init_icq) /* [한국어] 현재 elevator가 init_icq 콜백을 등록했는지 확인 - 스케줄러 프라이빗 상태(BFQ의 bfq_io_cq 확장 필드 등) 초기화가 필요한 경우에만 존재 */
			et->ops.init_icq(icq); /* [한국어] 스케줄러별 초기화 콜백 호출 - 예: BFQ가 bfq_io_cq의 엔티티/큐 포인터들을 세팅 */
	} else { /* [한국어] radix_tree_insert가 실패(음수 반환) - 이 ioc/q 쌍에 대한 icq를 다른 스레드가 이 함수와 동시에 먼저 만들어 이미 트리에 넣어버린 경쟁 상황 */
		kmem_cache_free(et->icq_cache, icq); /* [한국어] 경쟁에서 졌으므로 방금 할당한 icq는 필요 없음 - 즉시 반환해 누수 방지 (아직 어떤 리스트에도 안 매달렸으므로 unlink 없이 바로 free 가능) */
		icq = ioc_lookup_icq(q); /* [한국어] 경쟁에서 이긴 다른 스레드가 만든 icq를 대신 찾아옴 - 이미 두 락을 쥔 상태이므로 이 조회는 사실상 radix_tree_lookup 직접 호출과 동등하게 안전 */
		if (!icq) /* [한국어] 방금 삽입 실패했다면 반드시 누군가 성공적으로 삽입했어야 하는데도 찾지 못하는, 이론상 있어서는 안 되는 상황인지 확인 */
			printk(KERN_ERR "cfq: icq link failed!\n"); /* [한국어] 불변식이 깨진 비정상 상황을 커널 로그에 남김 - "cfq:"는 이 코드가 과거 CFQ 스케줄러 시절부터 이어져 온 레거시 문자열(기능은 현재 모든 elevator 공통 경로) */
	}

	spin_unlock(&ioc->lock); /* [한국어] ioc 락 해제 (나중에 잡은 락을 먼저 해제 - LIFO 순서) */
	spin_unlock_irq(&q->queue_lock); /* [한국어] q 락 해제 및 인터럽트 상태 복원 */
	radix_tree_preload_end(); /* [한국어] radix_tree_maybe_preload로 확보해 둔 percpu 예비 메모리 풀 사용을 종료 - 더 이상 이 스레드의 preload 상태를 유지할 필요 없음 */
	return icq; /* [한국어] 새로 만든 icq 또는 경쟁에서 진 뒤 찾아온 기존 icq를 호출자에게 반환 (둘 다 실패라면 NULL) */
}

/*
 * [한국어]
 * ioc_find_get_icq - 현재 태스크의 icq를 찾거나 없으면 만들고, 그 io_context에 대한 참조까지 얻어서 반환한다
 *
 * @q: icq를 확보할 대상 request_queue.
 * @return: current->io_context와 @q 쌍에 대응하는 유효한 io_cq 포인터
 *          (반환된 이 icq의 ioc에는 참조카운트가 하나 추가로 걸려 있음 -
 *          "find_get"이라는 이름 그대로 lookup-or-create + get 의미론).
 *          io_context/icq 할당 실패 시 NULL.
 *
 * 이 함수는 I/O 발행 경로에서 elevator가 request에 icq를 붙일 때 쓰는
 * 최상위 진입점으로, 다음 세 가지를 한 번에 처리한다: (1) 현재 태스크에
 * io_context 자체가 아직 없다면 만들어서 연결, (2) 그 io_context에
 * 참조카운트를 하나 추가로 걸어(get) 이 함수가 반환한 icq를 request가
 * 들고 있는 동안 io_context가 사라지지 않도록 보장, (3) @q에 대한
 * icq가 이미 있으면 재사용하고 없으면 새로 생성.
 * 동작 과정:
 *  (1) 현재 태스크에 io_context가 아예 없는지 확인(unlikely - 대부분
 *      태스크는 이미 갖고 있음).
 *      - 없다면: GFP_ATOMIC으로 새 io_context를 만들고(q->node로 큐의
 *        NUMA 지역성 활용), 실패하면 NULL 반환. 성공하면 task_lock으로
 *        보호하며 그 사이 다른 스레드(예: 동시에 들어온 __copy_io나
 *        다른 icq 확보 경로)가 이미 io_context를 설치했는지 재확인
 *        (TOCTOU) - 이미 있다면 방금 만든 것은 버리고 기존 것을 쓰고,
 *        없다면 방금 만든 것을 정식으로 연결. 마지막으로 get_io_context
 *        로 참조를 하나 얻는다(방금 만들었든 경쟁에서 진 뒤 얻은 기존
 *        것이든 동일하게 처리).
 *      - 이미 있다면: get_io_context로 참조를 하나 얻고,
 *        ioc_lookup_icq()로 @q에 대한 기존 icq가 있는지 조회.
 *  (2) 위 두 경로 어느 쪽이든, icq를 아직 못 찾았다면(icq == NULL)
 *      ioc_create_icq()로 새로 만든다. 그마저 실패하면 방금 (1)에서
 *      얻은 io_context 참조를 put_io_context()로 되돌리고(참조 누수
 *      방지) NULL을 반환한다.
 *  (3) 최종적으로 유효한 icq를 반환. 이 icq를 통해 간접적으로
 *      icq->ioc가 참조카운트 하나를 보유한 상태가 유지된다.
 * 실행 컨텍스트: I/O 발행 경로(request 생성)의 프로세스 컨텍스트.
 * task_lock(current)은 자기 자신의 task_struct만 잠그므로 데드락 걱정
 * 없이 사용 가능.
 * 호출자(caller): BFQ 등 elevator의 request-private 데이터 준비 경로
 * (예: bfq_get_rq_private가 rq->elv.icq = ioc_find_get_icq(rq->q)로 호출).
 * 피호출자(callee): alloc_io_context(), get_io_context(), ioc_lookup_icq(),
 * ioc_create_icq(), put_io_context(), kmem_cache_free().
 * 에러 처리: 어느 단계에서든 할당 실패 시 그때까지 확보한 자원(io_context
 * 참조 등)을 되돌린 뒤 NULL 반환 - 호출자는 icq 없이 request를 처리하는
 * 대체 경로(스케줄러 프라이빗 상태 없이 진행)를 타게 된다.
 *
 * 호출 체인:
 *   bfq_get_rq_private() 등 elevator 콜백 -> [ioc_find_get_icq]
 *       -> alloc_io_context() / get_io_context() -> ioc_lookup_icq()
 *       -> (없으면) ioc_create_icq()
 */
struct io_cq *ioc_find_get_icq(struct request_queue *q)
{
	struct io_context *ioc = current->io_context; /* [한국어] 현재 태스크의 io_context - NULL일 수 있음(아직 한 번도 할당된 적 없는 경우) */
	struct io_cq *icq = NULL; /* [한국어] 최종적으로 찾거나 만들 icq - 아직 못 찾았음을 나타내는 초기값 NULL */

	if (unlikely(!ioc)) { /* [한국어] 현재 태스크가 io_context를 전혀 갖고 있지 않은 드문 경우(unlikely) - 이 큐 이전에도 어떤 큐에도 I/O를 발행한 적이 없거나 ioprio도 설정한 적 없는 태스크 */
		ioc = alloc_io_context(GFP_ATOMIC, q->node); /* [한국어] 새 io_context를 할당 - GFP_ATOMIC은 이 경로가 블로킹 불가능한 컨텍스트에서도 호출될 수 있음을 가정, q->node로 이 큐가 속한 NUMA 노드에 맞춰 지역성 확보 */
		if (!ioc) /* [한국어] 할당 실패 확인 */
			return NULL; /* [한국어] 메모리 부족을 호출자에 전파 - 이 시점엔 아직 아무 것도 연결/참조하지 않았으므로 그냥 반환해도 안전 */

		task_lock(current); /* [한국어] current->io_context 필드를 다른 스레드와 동시에 건드리지 않도록 자기 자신의 태스크 락 획득 */
		if (current->io_context) { /* [한국어] 락을 잡기 전(alloc_io_context 호출 도중) 다른 스레드가 이미 current->io_context를 설치했는지 재확인(TOCTOU 경쟁 처리) - current라도 인터럽트/다른 CPU에서 동시에 이 필드를 만질 여지가 있음 */
			kmem_cache_free(iocontext_cachep, ioc); /* [한국어] 경쟁에서 졌으므로 방금 할당한 io_context는 버림 - 아직 어디에도 연결 안 했으므로 바로 free해도 안전 */
			ioc = current->io_context; /* [한국어] 대신 이미 설치되어 있던 기존 io_context를 사용하도록 로컬 변수 갱신 */
		} else {
			current->io_context = ioc; /* [한국어] 경쟁자가 없었으므로 방금 할당한 io_context를 이 태스크에 정식으로 연결 */
		}

		get_io_context(ioc); /* [한국어] (방금 만들었든 경쟁에서 진 뒤 얻은 기존 것이든) 이 io_context에 대한 참조를 하나 추가로 획득 - 이 함수가 반환하는 icq를 통해 이 참조가 유지되며, 최종적으로 request 완료 시 put_io_context로 반환됨 */
		task_unlock(current); /* [한국어] 태스크 락 해제 */
	} else { /* [한국어] 이미 io_context가 있던 일반적인 경우 */
		get_io_context(ioc); /* [한국어] 기존 io_context에 참조를 하나 추가 획득 - 이 함수가 반환할 icq가 이 참조를 통해 io_context 생존을 보장 */
		icq = ioc_lookup_icq(q); /* [한국어] 이미 이 (ioc, q) 쌍에 대한 icq가 있는지 빠르게 조회 - 있으면 새로 만들 필요 없음 */
	}

	if (!icq) { /* [한국어] 위 두 경로 어느 쪽으로도 icq를 아직 확보하지 못한 경우(새 io_context를 막 만들었거나, 기존 io_context는 있지만 이 큐에는 처음 I/O를 보내는 경우) */
		icq = ioc_create_icq(q); /* [한국어] icq를 새로 만들어 ioc/q 양쪽에 연결 */
		if (!icq) { /* [한국어] 새 icq 생성마저 실패했는지 확인(메모리 부족 등) */
			put_io_context(ioc); /* [한국어] 위에서 get_io_context로 얻어 둔 참조를 되돌림 - icq 생성 실패로 이 함수 전체가 실패하므로 참조 누수를 막기 위해 반드시 되돌려야 함 */
			return NULL; /* [한국어] 실패를 호출자에 전파 */
		}
	}
	return icq; /* [한국어] 최종적으로 유효한 icq를 반환 - 이 icq->ioc는 위에서 획득한 참조카운트 하나를 보유한 상태 */
}
EXPORT_SYMBOL_GPL(ioc_find_get_icq); /* [한국어] GPL 모듈에서만 사용 가능하도록 export - BFQ 등 인트리 elevator 모듈이 이 함수를 호출 */
#endif /* CONFIG_BLK_ICQ */ /* [한국어] icq 인프라 활성 버전의 두 번째 CONFIG_BLK_ICQ 블록(ioc_lookup_icq/ioc_create_icq/ioc_find_get_icq) 종료 */

/*
 * [한국어]
 * blk_ioc_init - io_context 전용 slab 캐시를 부팅 시 한 번 생성한다
 *
 * @param: 없음 (void).
 * @return: 항상 0 (커널 initcall 관례상 성공은 0, 실패해도 이 함수
 *          자체는 SLAB_PANIC 때문에 반환할 일이 없음 - 아래 참고).
 *
 * io_context 구조체를 매번 kmalloc으로 할당하는 대신 전용 slab 캐시를
 * 두면, 같은 크기의 객체를 반복 할당/해제할 때 캐시 지역성과 할당
 * 속도가 좋아진다. 이 함수는 커널 부팅 과정에서 블록 계층이 쓰일 수
 * 있게 되기 전에 딱 한 번 그 캐시를 만들어 둔다.
 * 동작 과정: kmem_cache_create()로 "blkdev_ioc"라는 이름의 slab 캐시를
 * struct io_context 크기만큼, 정렬 요구사항 0(기본 정렬 사용), 그리고
 * SLAB_PANIC 플래그로 생성한다. SLAB_PANIC은 "이 캐시 생성에 실패하면
 * 커널 전체를 panic시켜라"는 의미로, io_context 캐시 없이는 블록 계층
 * 자체가 동작할 수 없으므로 조용히 넘어가는 대신 즉시 부팅을 중단시켜
 * 문제를 조기에 드러내기 위함이다. 마지막 인자 NULL은 생성자(constructor)
 * 콜백이 없음을 의미 - alloc_io_context()가 매 할당 시 __GFP_ZERO로
 * 직접 초기화하므로 별도 생성자가 필요 없다.
 * 실행 컨텍스트: subsys_initcall 단계의 부팅 시퀀스 - 인터럽트가 켜져
 * 있고 블로킹도 가능한 이른 부팅 프로세스 컨텍스트지만, 아직 사용자
 * 공간 프로세스는 실행되지 않은 시점.
 * 호출자(caller): 커널 initcall 프레임워크가 subsys_initcall 레벨에서
 * 자동으로 호출 (직접적인 함수 호출이 아니라 링커 섹션(.initcall*.init)에
 * 등록된 함수 포인터를 부팅 시퀀서가 순회 호출).
 * 피호출자(callee): kmem_cache_create().
 * 에러 처리: SLAB_PANIC 플래그로 인해 실패 시 이 함수가 반환되지 않고
 * 커널이 panic - 즉 이 함수가 정상 반환했다면 iocontext_cachep은 항상
 * 유효하다고 이후 코드 전체가 가정할 수 있다.
 *
 * 호출 체인:
 *   (커널 initcall 프레임워크, subsys_initcall 레벨) -> [blk_ioc_init]
 *       -> kmem_cache_create()
 */
static int __init blk_ioc_init(void)
{
	iocontext_cachep = kmem_cache_create("blkdev_ioc",
			sizeof(struct io_context), 0, SLAB_PANIC, NULL); /* [한국어] "blkdev_ioc"라는 이름으로 struct io_context 크기의 slab 캐시 생성 - 정렬 인자 0은 기본 정렬 사용, SLAB_PANIC은 생성 실패 시 즉시 커널 패닉(블록 계층 필수 인프라이므로 조용한 실패 대신 조기 발견을 택함), 마지막 NULL은 객체 생성자 콜백 없음(alloc_io_context가 직접 초기화하므로 불필요) */
	return 0; /* [한국어] initcall 관례상 성공(0)을 반환 - SLAB_PANIC 덕분에 이 줄에 도달했다는 것 자체가 캐시 생성 성공을 의미 */
}
subsys_initcall(blk_ioc_init); /* [한국어] blk_ioc_init을 subsys_initcall 우선순위로 커널 initcall 테이블에 등록 - core_initcall보다는 늦고 device_initcall보다는 이르게 실행되어, 블록 서브시스템의 다른 초기화 코드가 io_context를 필요로 하기 전에 캐시가 준비되도록 순서를 맞춤 */
