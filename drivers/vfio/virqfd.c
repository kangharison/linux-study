// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO generic eventfd code for IRQFD support.
 * Derived from drivers/vfio/pci/vfio_pci_intrs.c
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

/*
 * [한국어 설명] eventfd 기반 가상 IRQ 통로 (drivers/vfio/virqfd.c)
 *
 * === 파일의 역할 ===
 * VFIO 가 사용자 공간(QEMU/KVM 등)에서 전달한 eventfd 를 "가상 IRQ
 * 입력선" 으로 받아들여, eventfd 가 신호를 받을 때마다 등록된 핸들러를
 * 자동 실행하는 일반 프레임워크. PCI 의 INTx mask/unmask, mlx5 의
 * migration 인지 도움 등 "VFIO 사용자 공간이 트리거하는 비동기 사건"
 * 전반이 이 인프라 위에서 동작한다. KVM 의 irqfd 에서 영감을 받아
 * VFIO 측에 일반화한 코드이며, 사용자가 한 번 fd 를 등록해 두면 매번
 * ioctl 을 거치지 않고도 커널이 sleep 상태에서 wakeup 만 받아 즉시
 * 동작할 수 있다(저지연·콘텍스트 스위치 절감).
 *
 * 핵심 설계 두 가지:
 *  (1) eventfd 의 wait_queue 에 직접 등록한 콜백(virqfd_wakeup) 으로
 *      신호 수신을 IRQ atomic 컨텍스트에서 즉시 받는다.
 *  (2) 실제 작업은 schedule_work 로 process context 의 workqueue 에
 *      위임하여, 호출자(드라이버 vendor 콜백) 가 sleep 가능한 환경에서
 *      안전하게 실행되도록 분리한다(handler=fast/atomic, thread=slow/sleepable).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간 → ioctl(VFIO_DEVICE_SET_IRQS, fd_array)
 *   → 버스 드라이버 (vfio-pci-core/intrs.c, mlx5/main.c 등)
 *      → vfio_virqfd_enable(opaque, handler, thread, data, &slot, fd)
 *         (이 파일이 *slot 에 virqfd 객체 보관)
 *   ▲                                              ▼
 *   └────── 사용자가 eventfd write(2) ◄── 게스트 가상 디바이스(KVM) ──┘
 *
 * 사용자 공간이 (또는 KVM 이 자체 irqfd 짝 짓기로) eventfd 에 신호를
 * 보내면 → 커널 wait_queue → virqfd_wakeup → handler(atomic) →
 * thread(workqueue) 의 두 단계 핸들러가 차례로 실행된다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/vfio.h
 *      : struct virqfd 정의(필드 7개) 와 본 파일이 export 하는 3 함수
 *        (vfio_virqfd_enable/disable/flush_thread) prototype.
 *  - drivers/vfio/vfio_main.c
 *      : 모듈 init/exit 단계에서 vfio_virqfd_init/exit 를 호출 —
 *        cleanup workqueue 를 한 번만 만들도록 책임짐.
 *  - drivers/vfio/pci/vfio_pci_intrs.c
 *      : 가장 중요한 소비자. INTx mask 자동화, MSI ack 등 PCI IRQ
 *        라이프사이클을 본 모듈로 자동화한다.
 *  - drivers/vfio/pci/mlx5/main.c, virtio/migrate.c, hisi_acc/...
 *      : vendor 마이그레이션 드라이버가 게스트 알림 채널로 사용.
 *  - fs/eventfd.c
 *      : eventfd_ctx_fileget/_do_read/_remove_wait_queue/_put 등 모든
 *        eventfd 라이프사이클 API.
 *  - kernel/workqueue.c
 *      : create_singlethread_workqueue / queue_work / flush_workqueue.
 *        cleanup 은 단일 스레드 wq 로 직렬화하여 race 단순화.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_irqfd_cleanup_wq: 모든 virqfd 해체/inject 를 직렬화하는 단일
 *    스레드 workqueue. 전역 1개.
 *  - virqfd_lock: pvirqfd 포인터 set/clear 직렬화하는 spinlock.
 *  - vfio_virqfd_enable(): 사용자 fd 를 받아 virqfd 객체 생성·바인딩.
 *  - vfio_virqfd_disable(): 비동기 해체 트리거 + workqueue flush.
 *  - vfio_virqfd_flush_thread(): inject 워커가 끝날 때까지 대기.
 *  - virqfd_wakeup(): eventfd wait_queue 에 등록되는 fast-path 콜백.
 *  - virqfd_shutdown()/virqfd_inject()/virqfd_flush_inject(): worker 진입점.
 *
 * struct virqfd (vfio.h 정의) 필드 의미:
 *   opaque   : 호출자 컨텍스트(보통 디바이스 객체).
 *   handler  : atomic 컨텍스트 fast 콜백. NULL 가능. 0 반환 시 thread 생략.
 *   thread   : process 컨텍스트 slow 콜백. NULL 가능.
 *   data     : 둘 다 받는 보조 인자(보통 IRQ index).
 *   pvirqfd  : 호출자가 보유한 "이 슬롯" 포인터의 주소(이중 등록 방지 + 외부 disable 가능).
 *   eventfd  : 사용자 fd 의 커널측 컨텍스트.
 *   wait/pt  : eventfd 의 waitqueue 에 등록되는 entry 와 poll_table.
 *   shutdown/inject/flush_inject: 3종 worker.
 */

#include <linux/vfio.h>		/* [한국어] 외부 export 매크로(EXPORT_SYMBOL_GPL) 와 VFIO uAPI 의존 — 본 파일은 IRQ 코드이지만 헤더 일관성을 위해 포함. */
#include <linux/eventfd.h>		/* [한국어] eventfd_ctx, eventfd_ctx_fileget/do_read/put/remove_wait_queue: 모든 eventfd 조작 API. */
#include <linux/file.h>		/* [한국어] CLASS(fd, …)/fd_empty/fd_file: fd 자동 lifetime 관리(__cleanup__) — 5.x 이후 추가된 RAII 매크로. */
#include <linux/module.h>		/* [한국어] EXPORT_SYMBOL_GPL: 본 모듈이 외부 vfio-pci-core 등에 노출되도록. */
#include <linux/slab.h>		/* [한국어] kzalloc_obj/kfree: virqfd 객체 동적 할당/해제. */
#include "vfio.h"			/* [한국어] struct virqfd 정의 + prototype 일관성 검증. */

/* [한국어] 모든 virqfd 의 shutdown/flush_inject 워커가 큐잉되는 단일-스레드 workqueue.
 * 설정자: vfio_virqfd_init() — 모듈 init 1회.
 * 읽는 자: virqfd_deactivate, vfio_virqfd_disable/flush_thread.
 * 단일 스레드 강제 이유: 동일 virqfd 의 shutdown 과 inject 가 race 하지 않도록
 *   한 spec 안에서 직렬화. 또한 flush_workqueue 한 번이 모든 outstanding cleanup 을
 *   대기하는 단순한 동기화 모델을 가능케 함. */
static struct workqueue_struct *vfio_irqfd_cleanup_wq;

/* [한국어] *pvirqfd 슬롯의 set/clear 를 직렬화하는 전역 spinlock.
 * 보호 대상: 호출자 측 슬롯(*pvirqfd) 와 그 안에서의 NULL/non-NULL 전이.
 * 사용 컨텍스트: enable() 의 슬롯 점유, wakeup() 의 EPOLLHUP 자동 해제,
 *   disable() 의 명시적 해제 — 세 경로가 동시에 같은 슬롯을 건드릴 수 있어
 *   전역 락 1개로 단순화(slot 별 락은 해제 race 가 더 복잡해진다). */
static DEFINE_SPINLOCK(virqfd_lock);

/*
 * [한국어]
 * vfio_virqfd_init - 모듈 init 시 cleanup workqueue 생성
 *
 * @return: 0 성공, -ENOMEM 실패.
 *
 * vfio_main.c 의 vfio_init() 에서 호출되며, 실패 시 모듈 적재가 실패하므로
 * 본 워크큐의 존재성은 모듈 lifetime 동안 보장된다(이후 NULL 검사 불필요).
 *
 * 호출 체인: module_init(vfio_init) → [본 함수].
 */
int __init vfio_virqfd_init(void)
{
	vfio_irqfd_cleanup_wq =							/* [한국어] 단일 스레드 — shutdown/flush 직렬화 보장 + race 단순화. */
		create_singlethread_workqueue("vfio-irqfd-cleanup");		/* [한국어] /proc 또는 ps 에서 식별 가능한 이름 부여. */
	if (!vfio_irqfd_cleanup_wq)						/* [한국어] 워크큐 생성 실패는 거의 OOM 케이스 — 메시지 없이 ENOMEM 만 반환. */
		return -ENOMEM;

	return 0;
}

/*
 * [한국어]
 * vfio_virqfd_exit - 모듈 unload 시 cleanup workqueue 파괴
 *
 * 호출 시점에는 모든 virqfd 가 disable 되어 있어야 한다(vfio_main 의
 * unload 순서 책임). destroy_workqueue 는 outstanding work 를 모두
 * flush 한 후 안전하게 메모리 해제.
 */
void vfio_virqfd_exit(void)
{
	destroy_workqueue(vfio_irqfd_cleanup_wq);				/* [한국어] flush + free 일체화. wq 가 NULL 이면 noop 이지만 init 성공 가정. */
}

/*
 * [한국어]
 * virqfd_deactivate - shutdown 워커를 cleanup wq 에 큐잉
 *
 * @virqfd: 해체 대상.
 *
 * 호출자는 반드시 *pvirqfd 슬롯에서 virqfd 포인터를 NULL 로 갱신한 뒤
 * (== 더 이상 새 wakeup 이 못 보도록) 본 함수를 부른다. 본 함수는 단지
 * shutdown 워커를 큐에 넣을 뿐이며, 실제 해제는 worker 컨텍스트에서
 * sleep 가능한 상태로 안전하게 진행된다.
 *
 * 호출 컨텍스트: virqfd_lock 보유 상태 + 가능하면 IRQ-disabled.
 */
static void virqfd_deactivate(struct virqfd *virqfd)
{
	queue_work(vfio_irqfd_cleanup_wq, &virqfd->shutdown);			/* [한국어] 동일 work 가 두 번 큐잉돼도 workqueue 가 자체적으로 1회만 실행 보장(중복 enqueue 무해). */
}

/*
 * [한국어]
 * virqfd_wakeup - eventfd wait_queue 에 등록되는 wakeup 콜백
 *
 * @wait: virqfd->wait 의 entry. container_of 로 virqfd 회수.
 * @mode: wakeup 모드(사용 안 함).
 * @sync: 동기 wakeup 여부(사용 안 함).
 * @key:  __poll_t 비트마스크 — EPOLLIN(신호 도착), EPOLLHUP(eventfd close).
 * @return: 항상 0(wait queue API 관례 — 0=계속 wakeup 계열).
 *
 * 이 콜백은 atomic 컨텍스트에서 호출된다(eventfd 의 ctx->wqh.lock 보유).
 * 따라서 sleep/메모리 alloc/락 시도 금지. 실제 사용자 콜백은 두 단계로
 * 분리:
 *  - handler(opaque, data): atomic 안전한 빠른 검사. NULL 또는 비-0 반환 시
 *     아래 thread 단계 trigger. 0 반환은 "이 사건은 무시" 의미.
 *  - thread(opaque, data): schedule_work 로 process context 워커에서 실행.
 *
 * EPOLLHUP 처리:
 *  사용자가 eventfd 를 close 하면 wait_queue 가 EPOLLHUP 으로 wakeup 한다.
 *  이때는 우리 슬롯(*pvirqfd)이 아직 우리 객체를 가리키고 있을 때만 자동
 *  해제(deactivate) — 외부에서 disable 호출이 먼저 일어났다면 슬롯이 이미
 *  바뀌어 있어 idempotent. KVM irqfd 와 동일한 lock-acquired-here 보장.
 */
static int virqfd_wakeup(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	struct virqfd *virqfd = container_of(wait, struct virqfd, wait);	/* [한국어] wait entry 는 virqfd 안에 내장 — entry 주소 → 부모 virqfd 주소 역산. */
	__poll_t flags = key_to_poll(key);					/* [한국어] poll_to_key/key_to_poll 매크로 쌍. wait_queue API 가 비트마스크를 void* 로 전달. */

	if (flags & EPOLLIN) {							/* [한국어] eventfd write(2) 또는 KVM 측 trigger 로 카운터가 증가했음을 의미. */
		u64 cnt;							/* [한국어] eventfd 의 64bit 카운터. read 로 0 으로 리셋해야 다음 신호를 받음. */
		eventfd_ctx_do_read(virqfd->eventfd, &cnt);			/* [한국어] _do_read 는 lock 보유 가정 변형 — wait_queue 콜백 컨텍스트에서 안전. */

		/* An event has been signaled, call function */
		if ((!virqfd->handler ||					/* [한국어] handler 미등록 = 항상 thread 실행. handler 등록 시 그 결과로 분기. */
		     virqfd->handler(virqfd->opaque, virqfd->data)) &&		/* [한국어] handler 가 0 반환 = 사건 무시 → thread 실행 안 함. 비-0 반환 = 후속 처리 필요. */
		    virqfd->thread)						/* [한국어] thread 가 등록돼 있을 때만 워커 큐잉 — handler-only 모드도 가능. */
			schedule_work(&virqfd->inject);			/* [한국어] inject 워커는 시스템 글로벌 wq(events) 사용 — cleanup_wq 와 분리해 inject 와 shutdown 이 서로 막지 않게. */
	}

	if (flags & EPOLLHUP) {						/* [한국어] eventfd close 통지 — 사용자가 fd 를 닫으면 자동 정리해야 메모리/콜백 누수 방지. */
		unsigned long flags;						/* [한국어] spin_lock_irqsave 보관용 (외곽 변수 'flags' 와 같은 이름이라 shadow — 의도적 패턴, 표준화). */
		spin_lock_irqsave(&virqfd_lock, flags);			/* [한국어] *pvirqfd 슬롯 검사·갱신 직렬화. wait_queue lock 과 nested 가능 — irqsave 로 deadlock 회피. */

		/*
		 * The eventfd is closing, if the virqfd has not yet been
		 * queued for release, as determined by testing whether the
		 * virqfd pointer to it is still valid, queue it now.  As
		 * with kvm irqfds, we know we won't race against the virqfd
		 * going away because we hold the lock to get here.
		 */
		if (*(virqfd->pvirqfd) == virqfd) {				/* [한국어] 슬롯에 아직 우리가 들어 있을 때만 — 외부 disable 이 먼저면 다른 값 또는 NULL. */
			*(virqfd->pvirqfd) = NULL;				/* [한국어] 슬롯을 NULL 로 — 후속 enable 이 이 슬롯을 재사용 가능하게. */
			virqfd_deactivate(virqfd);				/* [한국어] shutdown 워커 큐잉 — 실제 free 는 worker 에서. */
		}

		spin_unlock_irqrestore(&virqfd_lock, flags);
	}

	return 0;								/* [한국어] wait_queue API 0 = remove 하지 않음(deactivate 측이 명시적으로 remove 처리). */
}

/*
 * [한국어]
 * virqfd_ptable_queue_proc - vfs_poll 단계에서 wait queue 등록
 *
 * @file: eventfd 의 file. 사용 안 함.
 * @wqh:  eventfd 가 노출하는 wait_queue_head_t — 이쪽에 우리 entry 추가.
 * @pt:   poll_table 포인터(virqfd->pt) — container_of 로 virqfd 회수.
 *
 * vfs_poll(file, &virqfd->pt) 호출 시 file_operations->poll 이
 * poll_wait(file, wqh, pt) 를 부르고, 그 결과로 본 콜백이 한 번 실행된다.
 * 즉 "poll() 를 흉내내며 등록만 수행" 하는 패턴.
 */
static void virqfd_ptable_queue_proc(struct file *file,
				     wait_queue_head_t *wqh, poll_table *pt)
{
	struct virqfd *virqfd = container_of(pt, struct virqfd, pt);		/* [한국어] poll_table 도 내장 필드 — entry 주소로 부모 회수. */
	add_wait_queue(wqh, &virqfd->wait);					/* [한국어] eventfd 의 waitqueue 에 우리 wakeup 등록. 이후 모든 신호가 virqfd_wakeup 로 흐름. */
}

/*
 * [한국어]
 * virqfd_shutdown - cleanup workqueue 에서 virqfd 객체 최종 해제
 *
 * @work: virqfd->shutdown.
 *
 * 동작 순서:
 *  1) eventfd waitqueue 에서 우리 entry 제거 — 이후 wakeup 차단.
 *  2) 진행 중인 inject 워커 완료 대기(flush_work) — 콜백이 끝나기 전 free 금지.
 *  3) eventfd context refcount drop — 우리가 fileget 로 잡았던 ref 1 회수.
 *  4) virqfd 자체 free.
 *
 * 단일 스레드 wq 에서 실행되므로 다른 shutdown/flush_inject 와 직렬.
 * inject 는 시스템 wq 에 있어 별 wq — 따라서 flush_work 가 필요(전체 wq 가 아닌
 * 해당 work 단위).
 */
static void virqfd_shutdown(struct work_struct *work)
{
	struct virqfd *virqfd = container_of(work, struct virqfd, shutdown);	/* [한국어] work 주소 → 부모 virqfd 회수. */
	u64 cnt;								/* [한국어] remove_wait_queue 가 마지막 카운터를 회수하지만 본 함수에선 사용 안 함. */

	eventfd_ctx_remove_wait_queue(virqfd->eventfd, &virqfd->wait, &cnt);	/* [한국어] waitqueue 에서 우리를 unlink + 진행중 wakeup 직렬화 보장. 이후 새 콜백 진입 불가. */
	flush_work(&virqfd->inject);						/* [한국어] inject 워커가 systemd wq 에서 실행 중일 수 있으므로 완료 대기. 데이터 race 방지 핵심. */
	eventfd_ctx_put(virqfd->eventfd);					/* [한국어] enable 의 eventfd_ctx_fileget 짝. 마지막 ref 라면 ctx free. */

	kfree(virqfd);								/* [한국어] 더 이상 어떤 콜백도 우리를 못 부름이 보장된 후의 안전한 free. */
}

/*
 * [한국어]
 * virqfd_inject - thread 콜백을 process 컨텍스트에서 실행하는 워커
 *
 * @work: virqfd->inject.
 *
 * virqfd_wakeup 은 atomic 컨텍스트라 sleep 가능한 작업을 못 한다.
 * 사용자가 등록한 thread() 가 (예: KVM irqfd 까지 깨우거나, 인터럽트
 * mask/unmask 같은) sleep 가능 콜이라면 이 워커가 그 컨텍스트를 제공한다.
 * 시스템 글로벌 events wq 에서 실행 — cleanup_wq 와 분리해 race 단순화.
 */
static void virqfd_inject(struct work_struct *work)
{
	struct virqfd *virqfd = container_of(work, struct virqfd, inject);	/* [한국어] work → virqfd 역산. */
	if (virqfd->thread)							/* [한국어] enable 시점 thread NULL 인 모드에서도 schedule_work 가 호출될 수 있지만 wakeup 이 막아주므로 사실상 항상 non-NULL. 안전 가드. */
		virqfd->thread(virqfd->opaque, virqfd->data);			/* [한국어] 사용자 콜백 — 예: vfio_pci INTx 자동 unmask, mlx5 마이그레이션 progress. */
}

/*
 * [한국어]
 * virqfd_flush_inject - flush_thread API 의 워커 진입점
 *
 * @work: virqfd->flush_inject.
 *
 * vfio_virqfd_flush_thread() 는 "현재 진행 중인 inject 가 끝나면 돌아오는"
 * 동기화 API 다. 본 워커는 cleanup_wq 에서 실행되며, 단지 inject 워커가
 * 끝날 때까지 flush_work 로 대기한다. cleanup_wq 자체가 단일 스레드라
 * 호출자(flush_thread→flush_workqueue) 가 본 워커 종료를 기다리는 것은
 * "직전까지 큐잉된 모든 inject 완료" 와 동치가 된다.
 */
static void virqfd_flush_inject(struct work_struct *work)
{
	struct virqfd *virqfd = container_of(work, struct virqfd, flush_inject);

	flush_work(&virqfd->inject);						/* [한국어] 본 워커는 cleanup_wq 안에서 실행 — 부모 flush_workqueue 가 끝나길 기다리면 inject 도 정리됨. */
}

/*
 * [한국어]
 * vfio_virqfd_enable - 사용자 fd 를 virqfd 로 변환하여 슬롯에 설치
 *
 * @opaque:  콜백에 전달할 첫 인자(보통 디바이스 객체).
 * @handler: atomic 컨텍스트 빠른 콜백. NULL 가능. 0 반환 = thread 생략.
 * @thread:  process 컨텍스트 느린 콜백. NULL 가능.
 * @data:    콜백 두 번째 인자(보통 IRQ index 또는 보조 토큰).
 * @pvirqfd: 호출자 측 "슬롯" — 이 함수가 virqfd 객체 포인터를 여기에 보관.
 *           외부 disable 이 NULL 검사로 idempotent 하게 동작하기 위함.
 * @fd:      사용자 공간 eventfd file descriptor.
 *
 * @return:  0 성공. -ENOMEM, -EBADF, -EBUSY, eventfd_ctx_fileget 실패 errno.
 *
 * 호출 컨텍스트: 일반적으로 ioctl 핸들러(슬립 가능). 호출자는 본 함수를
 *   호출하는 동안 자신의 device-level 락을 보유한 상태일 수 있으나,
 *   본 함수는 GFP_KERNEL_ACCOUNT alloc 과 vfs_poll 호출이 있으므로
 *   atomic 컨텍스트에서 호출하면 안 됨.
 *
 * 동작:
 *  1. virqfd 객체 alloc + 사용자 콜백/슬롯 정보 저장.
 *  2. 3종 worker INIT_WORK.
 *  3. CLASS(fd, irqfd)(fd) — RAII 매크로. 함수 return 시 fdput 자동.
 *     fd 가 유효하지 않으면 EBADF.
 *  4. eventfd_ctx_fileget — file 이 정말 eventfd 인지 검증 + ref 획득.
 *  5. virqfd_lock 안에서 *pvirqfd 슬롯 점유 — 이미 점유돼 있으면 EBUSY.
 *  6. wait/poll_table init + vfs_poll 로 eventfd waitqueue 에 등록.
 *  7. 등록 직전에 이미 카운터가 set 이었으면 즉시 inject(이벤트 누락 방지).
 *
 * 에러 경로: alloc 후 어디서 실패하든 eventfd put + virqfd free 까지
 *   롤백. 슬롯 점유 직전이므로 다른 스레드에 영향 없음.
 *
 * 호출 체인:
 *   ioctl → vfio_pci_set_intx_mask_eventfd → [vfio_virqfd_enable]
 *     → CLASS(fd) → eventfd_ctx_fileget → vfs_poll → virqfd_ptable_queue_proc
 */
int vfio_virqfd_enable(void *opaque,
		       int (*handler)(void *, void *),
		       void (*thread)(void *, void *),
		       void *data, struct virqfd **pvirqfd, int fd)
{
	struct eventfd_ctx *ctx;						/* [한국어] eventfd 의 커널측 컨텍스트 핸들. ref-counted. */
	struct virqfd *virqfd;							/* [한국어] 본 함수가 생성·반환할 객체. 슬롯에 저장 후 호출자가 ref 보유. */
	int ret = 0;								/* [한국어] 에러 경로에서 errno 보관. */
	__poll_t events;							/* [한국어] vfs_poll 결과 — 등록 시점에 이미 신호가 있었는지 확인용. */

	virqfd = kzalloc_obj(*virqfd, GFP_KERNEL_ACCOUNT);			/* [한국어] kmalloc + memset 0 + sizeof(*virqfd) 합친 헬퍼. ACCOUNT = memcg 카운트 — 컨테이너 격리. */
	if (!virqfd)
		return -ENOMEM;

	virqfd->pvirqfd = pvirqfd;						/* [한국어] 호출자 슬롯 주소 — wakeup/disable 양쪽이 슬롯 갱신에 사용. */
	virqfd->opaque = opaque;						/* [한국어] 콜백 첫 인자(보통 vdev). */
	virqfd->handler = handler;						/* [한국어] atomic 콜백. NULL 허용 — wakeup 에서 NULL 분기. */
	virqfd->thread = thread;						/* [한국어] process 콜백. NULL 허용 — inject 큐잉 자체가 안 됨. */
	virqfd->data = data;							/* [한국어] 콜백 두 번째 인자(IRQ index 등). */

	INIT_WORK(&virqfd->shutdown, virqfd_shutdown);			/* [한국어] 해체 워커 — cleanup_wq 에 큐잉 예정. */
	INIT_WORK(&virqfd->inject, virqfd_inject);				/* [한국어] thread 호출 워커 — 시스템 events wq 사용. */
	INIT_WORK(&virqfd->flush_inject, virqfd_flush_inject);		/* [한국어] flush_thread 동기화 보조 워커 — cleanup_wq 사용. */

	CLASS(fd, irqfd)(fd);							/* [한국어] __cleanup__ 기반 RAII — irqfd 변수 scope 종료 시 자동 fdput. 누락 방지 패턴(5.x 이후). */
	if (fd_empty(irqfd)) {							/* [한국어] fd_empty = 비활성/닫힌 fd. EBADF 명확화. */
		ret = -EBADF;
		goto err_fd;
	}

	ctx = eventfd_ctx_fileget(fd_file(irqfd));				/* [한국어] file → eventfd_ctx 변환 + ref +1. eventfd 가 아니면 EINVAL. */
	if (IS_ERR(ctx)) {							/* [한국어] -EINVAL/-EBADF 등 PTR_ERR 인코딩. */
		ret = PTR_ERR(ctx);
		goto err_fd;
	}

	virqfd->eventfd = ctx;							/* [한국어] 컨텍스트 보관 — shutdown 워커가 짝 ref 해제. */

	/*
	 * virqfds can be released by closing the eventfd or directly
	 * through ioctl.  These are both done through a workqueue, so
	 * we update the pointer to the virqfd under lock to avoid
	 * pushing multiple jobs to release the same virqfd.
	 */
	spin_lock_irq(&virqfd_lock);						/* [한국어] 슬롯 set 직렬화 — 동시에 다른 enable 또는 disable 진입 차단. */

	if (*pvirqfd) {							/* [한국어] 이미 누군가 슬롯을 점유 중 = 사용자 측 잘못된 호출 — EBUSY 로 단호히 거절. */
		spin_unlock_irq(&virqfd_lock);
		ret = -EBUSY;
		goto err_busy;
	}
	*pvirqfd = virqfd;							/* [한국어] 슬롯에 우리 객체 게시 — 이후부터 외부 disable/wakeup 가시. */

	spin_unlock_irq(&virqfd_lock);

	/*
	 * Install our own custom wake-up handling so we are notified via
	 * a callback whenever someone signals the underlying eventfd.
	 */
	init_waitqueue_func_entry(&virqfd->wait, virqfd_wakeup);		/* [한국어] wakeup 콜백을 우리 함수로 — eventfd 의 wait_queue 에 추가될 entry 의 동작 정의. */
	init_poll_funcptr(&virqfd->pt, virqfd_ptable_queue_proc);		/* [한국어] poll_wait 에서 호출될 proc 등록. */

	events = vfs_poll(fd_file(irqfd), &virqfd->pt);			/* [한국어] eventfd->poll(file, pt) 호출 — 내부에서 poll_wait 가 우리 ptable_queue_proc 호출, 결과로 우리 entry 가 wait_queue 에 add. 동시에 현재 신호 상태(events) 회신. */

	/*
	 * Check if there was an event already pending on the eventfd
	 * before we registered and trigger it as if we didn't miss it.
	 */
	if (events & EPOLLIN) {						/* [한국어] poll 시점에 이미 카운터 > 0 였다면 — 등록 전 wakeup 누락이므로 명시적으로 한 번 트리거. */
		if ((!handler || handler(opaque, data)) && thread)		/* [한국어] wakeup 과 동일한 두-단계 분기 적용. handler 0 반환 시 thread 생략. */
			schedule_work(&virqfd->inject);
	}
	return 0;
err_busy:
	eventfd_ctx_put(ctx);							/* [한국어] fileget 짝 — 슬롯 점유 실패는 우리 ref 도 풀어야 함. */
err_fd:
	kfree(virqfd);								/* [한국어] 슬롯 점유 전이라 외부 가시성 없음 — 안전한 즉시 free. */

	return ret;
}
EXPORT_SYMBOL_GPL(vfio_virqfd_enable);						/* [한국어] vfio-pci-core 등 외부 모듈 export. GPL 한정 — VFIO 전체가 GPL only. */

/*
 * [한국어]
 * vfio_virqfd_disable - 슬롯의 virqfd 를 비동기 해제하고 동기 대기
 *
 * @pvirqfd: 호출자 슬롯 — enable 에 넘긴 동일 주소.
 *
 * 동작:
 *  1) virqfd_lock 안에서 슬롯 NULL 화 + deactivate(shutdown 워커 큐잉).
 *  2) flush_workqueue 로 cleanup_wq 에 쌓인 모든 shutdown 완료까지 대기.
 *
 * 슬롯이 이미 NULL(EPOLLHUP 자동 해제 등) 이어도 idempotent — 워크큐
 * flush 만으로도 outstanding 정리가 보장된다("Even if we don't queue the
 * job, flush the wq to be sure it's been released").
 *
 * 호출 컨텍스트: process. flush_workqueue 가 sleep 가능.
 */
void vfio_virqfd_disable(struct virqfd **pvirqfd)
{
	unsigned long flags;							/* [한국어] irqsave 보관용. */

	spin_lock_irqsave(&virqfd_lock, flags);				/* [한국어] wakeup(EPOLLHUP) 와의 race 방지. */

	if (*pvirqfd) {							/* [한국어] 이미 자동 해제된 경우 noop. */
		virqfd_deactivate(*pvirqfd);					/* [한국어] shutdown 워커 enqueue. */
		*pvirqfd = NULL;						/* [한국어] 슬롯 비우기 — 후속 enable 가능. */
	}

	spin_unlock_irqrestore(&virqfd_lock, flags);

	/*
	 * Block until we know all outstanding shutdown jobs have completed.
	 * Even if we don't queue the job, flush the wq to be sure it's
	 * been released.
	 */
	flush_workqueue(vfio_irqfd_cleanup_wq);				/* [한국어] 단일 스레드 wq 라 "지금까지 큐잉된 모든 작업 종료" 의미. shutdown 직후 호출자가 device unregister 진행해도 안전. */
}
EXPORT_SYMBOL_GPL(vfio_virqfd_disable);

/*
 * [한국어]
 * vfio_virqfd_flush_thread - 진행 중인 inject 워커 종료까지 대기(슬롯 유지)
 *
 * @pvirqfd: 슬롯.
 *
 * disable 와 달리 슬롯/객체를 살려둔 채 "지금까지 큐잉된 thread 콜백
 * 모두 끝났음" 을 보장하는 동기화 함수. 사용자 공간이 ioctl 로 IRQ 라인을
 * 일시적으로 잠재울 때, 이미 큐잉된 inject 가 race 하지 않도록 사용한다.
 *
 * 동작:
 *  1) 슬롯의 thread 가 set 이면 flush_inject 워커를 cleanup_wq 에 큐잉.
 *  2) flush_workqueue 로 그 워커 종료 대기 — 워커가 내부적으로 inject
 *     워커를 flush_work 하므로 inject 종료까지 동기화됨.
 *
 * thread 가 NULL(handler-only 모드) 이면 큐잉할 필요가 없어 noop.
 */
void vfio_virqfd_flush_thread(struct virqfd **pvirqfd)
{
	unsigned long flags;							/* [한국어] 슬롯 검사 보호. */

	spin_lock_irqsave(&virqfd_lock, flags);
	if (*pvirqfd && (*pvirqfd)->thread)					/* [한국어] thread 미사용 시 inject 워커 자체가 안 돌므로 큐잉 불필요. */
		queue_work(vfio_irqfd_cleanup_wq, &(*pvirqfd)->flush_inject);	/* [한국어] flush_inject 가 cleanup_wq 안에서 실행되며 inject 를 flush_work — 단일 스레드 wq 직렬화로 부모 flush_workqueue 가 inject 종료까지 기다리게 됨. */
	spin_unlock_irqrestore(&virqfd_lock, flags);

	flush_workqueue(vfio_irqfd_cleanup_wq);				/* [한국어] 큐잉 안 했어도 선행 outstanding 작업 정리 측면에서 호출(원본 disable 주석과 동일 합리화). */
}
EXPORT_SYMBOL_GPL(vfio_virqfd_flush_thread);
