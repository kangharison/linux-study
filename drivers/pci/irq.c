// SPDX-License-Identifier: GPL-2.0
/*
 * PCI IRQ handling code
 *
 * Copyright (c) 2008 James Bottomley <James.Bottomley@HansenPartnership.com>
 * Copyright (C) 2017 Christoph Hellwig.
 */

/*
 * [한국어 설명] 할당된 인터럽트에 핸들러를 걸고, 레거시 INTx 배선을 계산한다 (irq.c)
 *
 * === 파일의 역할 ===
 * 인터럽트를 다루는 일은 두 단계로 나뉜다. "이 장치에 인터럽트 벡터 N개를
 * 확보한다"(msi/ 디렉터리)와 "확보된 벡터에 어떤 함수를 실행하도록 건다"
 * (이 파일)이다. 이 파일은 후자, 그리고 MSI 를 쓸 수 없는 장치를 위한
 * 레거시 INTx 배선 계산을 담당한다.
 *
 * 크게 세 덩어리다.
 *   1) pci_request_irq() / pci_free_irq() - 벡터 인덱스를 Linux IRQ 번호로
 *      바꿔 request_threaded_irq() / free_irq() 를 부르는 얇은 래퍼.
 *      NVMe 가 큐마다 인터럽트 핸들러를 거는 것이 바로 이 함수다.
 *   2) INTx 배선 계산 - pci_swizzle_interrupt_pin(), pci_common_swizzle(),
 *      pci_get_interrupt_pin(), pci_assign_irq(). 브리지를 거칠 때마다
 *      INTx 핀이 어떻게 바뀌는지를 스펙대로 따라간다.
 *   3) INTx 마스킹 - pci_check_and_mask_intx() / pci_check_and_unmask_intx().
 *      공유 인터럽트에서 "이 장치가 보낸 것인가" 를 확인하고 막는다.
 *
 * INTx swizzling 이 왜 필요한지 알아 두면 2번 덩어리가 읽힌다. PCI 시절
 * 인터럽트는 INTA~INTD 네 개의 물리적 선이었고, 여러 장치가 그 선을 공유했다.
 * 슬롯마다 같은 핀을 쓰면 모든 카드의 INTA 가 한 선에 몰려 부하가 편중되므로,
 * PCI-to-PCI 브리지 스펙(9.1절)은 브리지를 하나 지날 때마다 슬롯 번호만큼
 * 핀을 돌리도록 정했다. 그래서 "슬롯 1 의 INTA 는 상위에서 INTB" 가 된다.
 * 이 규칙을 Root Complex 에 닿을 때까지 반복 적용해야 최종 물리 선을 알 수 있고,
 * 그것을 하는 것이 pci_get_interrupt_pin() + pci_common_swizzle() 이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 핸들러 등록 경로:
 *   드라이버 (nvme queue_request_irq 등)
 *     -> [이 파일] pci_request_irq()
 *        -> pci_irq_vector()          [msi/api.c] : 벡터 인덱스 -> Linux IRQ 번호
 *        -> request_threaded_irq()    [kernel/irq/manage.c] : 실제 등록
 *
 * INTx 배선 경로 (MSI 를 못 쓰는 장치):
 *   pci_device_probe() -> pcibios_alloc_irq()
 *     -> 아키텍처 코드 -> [이 파일] pci_assign_irq()
 *        -> pci_get_interrupt_pin() -> pci_common_swizzle()
 *           -> pci_swizzle_interrupt_pin()   (브리지 한 단계마다)
 *        -> bridge->swizzle_irq / map_irq 콜백 (플랫폼이 채운 것)
 *
 * 실행 컨텍스트: pci_request_irq/pci_free_irq 는 프로세스 컨텍스트 전용이다.
 * kvasprintf(GFP_KERNEL)와 free_irq() 가 잠들 수 있기 때문이다. 반면
 * pci_check_and_mask_intx() 계열은 인터럽트 문맥에서 불린다(공유 IRQ 핸들러
 * 안에서 원인을 가려내는 용도).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 모든 PCI 드라이버(pci_request_irq/pci_free_irq), PCI 코어의
 *   pci_device_probe/pci_device_remove(pcibios_alloc_irq/free_irq).
 * 아래쪽: kernel/irq/ 의 request_threaded_irq/free_irq, msi/api.c 의
 *   pci_irq_vector, ../pci.h 의 config 접근 함수.
 * 옆쪽: 아키텍처/플랫폼 코드가 채우는 struct pci_host_bridge 의
 *   swizzle_irq / map_irq 콜백. 이 파일은 배선 규칙의 공통 부분만 알고,
 *   "그 핀이 결국 몇 번 IRQ 인가" 는 플랫폼에게 묻는다.
 * 공유 상태: struct pci_dev 의 irq(최종 Linux IRQ 번호), pin(config space 의
 *   Interrupt Pin), 그리고 IRQ 이름 문자열(kvasprintf 로 할당해 free_irq 가
 *   돌려주는 것을 kfree 한다 - 소유권이 특이하다).
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * 이 파일에서 NVMe 가 부르는 것은 pci_request_irq() 와 pci_free_irq() 둘뿐이다.
 *
 *   queue_request_irq(nvmeq)   [drivers/nvme/host/pci.c]
 *     use_threaded_interrupts 가 참이면:
 *       pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq_check, nvme_irq,
 *                       nvmeq, "nvme%dq%d", nr, nvmeq->qid)
 *       -> 하드 IRQ 문맥에서 nvme_irq_check 가 CQ 의 phase 비트만 보고
 *          "처리할 것이 있는가" 를 판정하고, 있으면 IRQ_WAKE_THREAD 를
 *          돌려 스레드에서 nvme_irq 가 완료를 거둔다.
 *     기본값(거짓)이면:
 *       pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq, NULL, nvmeq, ...)
 *       -> thread_fn 이 NULL 이므로 스레드를 만들지 않고, 하드 IRQ 문맥에서
 *          nvme_irq 가 곧바로 CQ 를 비운다. 지연이 가장 짧은 구성이다.
 *
 *   pci_free_irq(pdev, nvmeq->cq_vector, nvmeq)
 *     nvme_suspend_queue(), 그리고 nvme_setup_io_queues() 가 벡터를 재할당하기
 *     전에 벡터 0 의 핸들러를 미리 떼는 자리에서 불린다.
 *
 * dev_id 로 struct nvme_queue 포인터를 넘긴다는 점이 중요하다. IRQF_SHARED 라
 * 여러 핸들러가 같은 IRQ 를 공유할 수 있고, free_irq() 는 이 쿠키로 어느
 * 핸들러를 뗄지 식별한다. 큐마다 nvme_queue 가 다르므로 자연히 유일해진다.
 *
 * (기존 주석은 이 파일의 기능으로 pci_irq_vector() 를 들었으나, 그 함수는
 *  msi/api.c 에 정의돼 있고 이 파일은 부르기만 한다. 또 nvme_irq_check 와
 *  nvme_irq 가 항상 짝으로 동작하는 것처럼 적었으나, 실제로는
 *  use_threaded_interrupts 모듈 파라미터가 참일 때만 그렇고 기본값에서는
 *  nvme_irq 하나만 하드 IRQ 로 동작한다. 위 내용으로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_request_irq()          : 벡터 인덱스에 핸들러를 건다. IRQ 이름을
 *                              kvasprintf 로 만들어 넘기고, 실패하면 되돌린다.
 *                              항상 IRQF_SHARED 이며, handler 가 NULL 이면
 *                              IRQF_ONESHOT 가 추가된다.
 * pci_free_irq()             : 위의 역동작. free_irq() 가 돌려준 이름 문자열을
 *                              여기서 kfree 한다.
 * pci_swizzle_interrupt_pin(): 브리지 한 단계를 지날 때의 핀 회전.
 *                              ((pin-1 + slot) % 4) + 1 이 전부다.
 * pci_common_swizzle()       : 위를 Root Complex 에 닿을 때까지 반복.
 * pci_get_interrupt_pin()    : 최종 핀 번호와, 그 핀이 꽂힌 최상위 브리지를 함께 반환.
 * pci_assign_irq()           : 위 결과를 플랫폼의 map_irq 콜백에 넘겨 실제
 *                              Linux IRQ 번호를 얻어 dev->irq 에 넣는다.
 * pci_check_and_mask_intx()  : 공유 INTx 에서 이 장치가 인터럽트를 걸었는지
 *                              확인하고, 걸었다면 Command 레지스터의
 *                              INTx Disable 비트로 막는다. 두 동작이
 *                              pci_lock 안에서 원자적으로 일어나야 한다.
 * pci_check_and_unmask_intx(): 위의 역동작.
 * pcibios_alloc_irq()/pcibios_free_irq() : 아키텍처가 덮어쓸 수 있는 __weak 훅.
 *                              드라이버 바인딩 직전/직후에 PCI 코어가 부른다.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/pci.h>

#include "pci.h"

/**
 * pci_request_irq - allocate an interrupt line for a PCI device
 * @dev:	PCI device to operate on
 * @nr:		device-relative interrupt vector index (0-based).
 * @handler:	Function to be called when the IRQ occurs.
 *		Primary handler for threaded interrupts.
 *		If NULL and thread_fn != NULL the default primary handler is
 *		installed.
 * @thread_fn:	Function called from the IRQ handler thread
 *		If NULL, no IRQ thread is created
 * @dev_id:	Cookie passed back to the handler function
 * @fmt:	Printf-like format string naming the handler
 *
 * This call allocates interrupt resources and enables the interrupt line and
 * IRQ handling. From the point this call is made @handler and @thread_fn may
 * be invoked.  All interrupts requested using this function might be shared.
 *
 * @dev_id must not be NULL and must be globally unique.
 */

/*
 * [한국어]
 * pci_request_irq - 장치의 N번 인터럽트 벡터에 핸들러를 건다
 *
 * @dev:       대상 PCI 장치
 * @nr:        장치 기준 0-기반 벡터 인덱스. MSI-X 면 MSI-X 테이블의 항목 번호,
 *             MSI 면 할당받은 연속 벡터 중 몇 번째인지, INTx 면 항상 0 이다.
 * @handler:   인터럽트가 오면 하드 IRQ 문맥에서 실행할 함수.
 *             thread_fn 이 있을 때는 "1차 핸들러" 역할을 한다.
 *             NULL 이면서 thread_fn 이 있으면 커널의 기본 1차 핸들러가 대신 쓰인다.
 * @thread_fn: 스레드 문맥에서 실행할 함수. NULL 이면 스레드를 만들지 않는다.
 * @dev_id:    핸들러에 그대로 전달될 쿠키. 공유 IRQ 에서 핸들러를 식별하는
 *             열쇠이기도 하므로 NULL 이면 안 되고 시스템 전체에서 유일해야 한다.
 * @fmt:       IRQ 이름을 만들 printf 형식 문자열. 뒤따르는 가변 인자와 함께
 *             "nvme0q12" 같은 이름이 되어 /proc/interrupts 에 나타난다.
 * @return:    0 = 성공, 음수 errno = 실패.
 *
 * 하는 일 자체는 request_threaded_irq() 를 부르는 것뿐이다. 그럼에도 별도
 * 함수를 두는 이유는 두 가지 귀찮은 일을 대신해 주기 때문이다.
 *
 *   1) 벡터 인덱스 -> Linux IRQ 번호 변환. 드라이버는 "내 3번째 큐" 라는
 *      장치 기준 번호로 생각하지만 커널은 전역 IRQ 번호로 관리한다.
 *      pci_irq_vector() 가 그 사이를 잇는다. 이 변환을 함수 안에 감춰 두면
 *      드라이버가 두 종류의 번호를 헷갈릴 일이 없다.
 *   2) IRQ 이름의 생명주기 관리. request_threaded_irq() 는 이름 문자열을
 *      복사하지 않고 포인터만 들고 있으므로, 드라이버가 스택 버퍼를 넘기면
 *      /proc/interrupts 가 쓰레기를 가리키게 된다. 그래서 여기서 힙에
 *      할당하고, 짝인 pci_free_irq() 가 free_irq() 로부터 그 포인터를
 *      돌려받아 해제한다. 소유권이 커널 IRQ 계층을 한 바퀴 도는 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. kvasprintf(GFP_KERNEL) 과
 *   request_threaded_irq() 가 모두 잠들 수 있다.
 * 호출자: PCI 드라이버. NVMe 는 queue_request_irq() 에서 큐마다 한 번씩 부른다.
 * 피호출자: pci_irq_vector() [msi/api.c], request_threaded_irq() [kernel/irq/manage.c].
 * 에러 경로: 이름 할당 실패면 -ENOMEM, 등록 실패면 이름을 해제하고
 *   request_threaded_irq() 의 errno 를 그대로 올린다.
 *
 * 호출 체인:
 *   nvme_create_queue -> queue_request_irq -> [pci_request_irq]
 *     -> pci_irq_vector -> request_threaded_irq
 */
int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
		irq_handler_t thread_fn, void *dev_id, const char *fmt, ...)
{
	va_list ap;			/* [한국어] fmt 뒤의 가변 인자를 훑을 커서 */
	int ret;			/* [한국어] request_threaded_irq 의 결과 */
	char *devname;			/* [한국어] 힙에 만들 IRQ 이름. 소유권이 커널
					 * IRQ 계층으로 넘어갔다가 pci_free_irq 에서 돌아온다 */
	/* [한국어] 항상 공유 가능으로 등록한다. MSI-X 벡터는 사실상 전용이라
	 * 공유가 일어나지 않지만, INTx 로 내려간 경우에는 반드시 공유여야 하고,
	 * 두 경우를 구분해 봐야 얻는 것이 없기 때문이다. IRQF_SHARED 를 붙이면
	 * 커널이 dev_id 로 핸들러를 구분하므로 dev_id 가 유일해야 한다는
	 * 제약이 따라온다. */
	unsigned long irqflags = IRQF_SHARED;

	/* [한국어] 1차 핸들러 없이 스레드만 쓰겠다는 뜻이다. 이때는 커널이
	 * 기본 1차 핸들러를 넣어 주는데, 그 핸들러는 무조건 IRQ_WAKE_THREAD 를
	 * 돌려주므로 인터럽트 원인이 하드웨어에서 지워지지 않는다. 레벨 트리거라면
	 * 스레드가 처리를 끝내기 전에 같은 인터럽트가 무한히 재발한다.
	 * IRQF_ONESHOT 은 "스레드 핸들러가 끝날 때까지 이 IRQ 를 마스크해 두라" 는
	 * 지시라 그 폭주를 막는다. */
	if (!handler)
		irqflags |= IRQF_ONESHOT;

	va_start(ap, fmt);		/* [한국어] 가변 인자 순회 시작 */
	/* [한국어] 이름을 힙에 만든다. kvasprintf 는 길이를 미리 계산해 딱 맞는
	 * 크기로 할당하므로 버퍼 넘침이 없다. NVMe 는 "nvme%dq%d" 를 넘겨
	 * "nvme0q1" 같은 이름을 만들고, 이것이 /proc/interrupts 에 그대로 보인다 —
	 * 어느 큐가 어느 CPU 에서 얼마나 인터럽트를 받는지 확인하는 근거다. */
	devname = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);			/* [한국어] 순회 종료 */
	if (!devname)			/* [한국어] 이름조차 만들지 못할 만큼 메모리가 부족하면 */
		return -ENOMEM;		/* [한국어] 등록을 시도하지 않고 실패 */

	/* [한국어] 실제 등록. pci_irq_vector(dev, nr) 가 장치 기준 인덱스를
	 * 전역 Linux IRQ 번호로 바꾼다 — MSI-X 면 그 벡터에 배정된 virq,
	 * INTx 면 dev->irq 다. thread_fn 이 NULL 이면 스레드는 만들어지지 않고
	 * handler 가 하드 IRQ 문맥에서 전부 처리한다(NVMe 의 기본 구성). */
	ret = request_threaded_irq(pci_irq_vector(dev, nr), handler, thread_fn,
				   irqflags, devname, dev_id);
	/* [한국어] 실패했다면 이름 소유권이 넘어가지 않았으므로 여기서 되돌린다.
	 * 성공했다면 해제하면 안 된다 — 커널 IRQ 계층이 그 포인터를 계속 쓰고,
	 * 나중에 free_irq() 가 돌려줄 때 pci_free_irq() 가 해제한다. */
	if (ret)
		kfree(devname);
	return ret;			/* [한국어] 0 또는 음수 errno */
}
EXPORT_SYMBOL(pci_request_irq);

/**
 * pci_free_irq - free an interrupt allocated with pci_request_irq
 * @dev:	PCI device to operate on
 * @nr:		device-relative interrupt vector index (0-based).
 * @dev_id:	Device identity to free
 *
 * Remove an interrupt handler. The handler is removed and if the interrupt
 * line is no longer in use by any driver it is disabled.  The caller must
 * ensure the interrupt is disabled on the device before calling this function.
 * The function does not return until any executing interrupts for this IRQ
 * have completed.
 *
 * This function must not be called from interrupt context.
 */

/*
 * [한국어]
 * pci_free_irq - pci_request_irq() 로 건 핸들러를 뗀다
 *
 * @dev:    대상 PCI 장치
 * @nr:     장치 기준 0-기반 벡터 인덱스. 등록할 때와 같은 값이어야 한다.
 * @dev_id: 등록할 때 넘긴 것과 같은 쿠키. IRQF_SHARED 로 등록했으므로
 *          한 IRQ 에 여러 핸들러가 있을 수 있고, 이 값으로 어느 것을
 *          뗄지 식별한다. 다르면 아무것도 떼지 못한다.
 * @return: 없음.
 *
 * 한 줄짜리 함수지만 그 한 줄에 세 가지 일이 겹쳐 있다.
 *   pci_irq_vector(dev, nr) : 장치 기준 인덱스 -> 전역 IRQ 번호
 *   free_irq(...)           : 핸들러 제거. 등록 때 넘겼던 이름 포인터를 반환한다.
 *   kfree(...)              : 그 이름을 해제. pci_request_irq 가 힙에 만든 것이다.
 *
 * free_irq() 가 이름을 돌려준다는 사실이 이 함수의 존재 이유다. 커널 IRQ
 * 계층은 이름을 복사하지 않고 포인터만 들고 있다가, 해제할 때 "이제 이건
 * 네 것" 하고 돌려준다. 그 소유권을 받아 정리하는 곳이 여기다. 드라이버가
 * free_irq() 를 직접 부르면 이 문자열이 영원히 새어 나간다.
 *
 * 순서 제약: 호출 전에 장치 쪽 인터럽트를 먼저 꺼야 한다. free_irq() 는
 * 이미 실행 중인 핸들러가 끝날 때까지 기다려 주지만, 그 사이에 장치가
 * 새 인터럽트를 보내면 핸들러가 사라진 IRQ 에 인터럽트가 남아 결국
 * "nobody cared" 로 그 IRQ 가 통째로 비활성화된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. free_irq() 는 실행 중인 핸들러의
 *   완료를 기다리므로 인터럽트 문맥에서 부르면 교착한다.
 * 호출자: NVMe 는 nvme_suspend_queue() 와, 벡터를 재할당하기 전에
 *   벡터 0 의 핸들러를 미리 떼는 nvme_setup_io_queues() 에서 부른다.
 *
 * 호출 체인:
 *   nvme_dev_disable -> nvme_suspend_queue -> [pci_free_irq]
 *     -> pci_irq_vector -> free_irq -> kfree
 */
void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id)
{
	/* [한국어] 안에서 밖으로 읽는다. 벡터 인덱스를 IRQ 번호로 바꾸고,
	 * 그 IRQ 에서 dev_id 로 식별되는 핸들러를 떼고, free_irq 가 돌려준
	 * 이름 문자열을 해제한다. free_irq 가 NULL 을 돌려줘도(해당 핸들러가
	 * 없던 경우) kfree(NULL) 은 안전하므로 별도 검사가 없다. */
	kfree(free_irq(pci_irq_vector(dev, nr), dev_id));
}
EXPORT_SYMBOL(pci_free_irq);

/**
 * pci_swizzle_interrupt_pin - swizzle INTx for device behind bridge
 * @dev: the PCI device
 * @pin: the INTx pin (1=INTA, 2=INTB, 3=INTC, 4=INTD)
 *
 * Perform INTx swizzling for a device behind one level of bridge.  This is
 * required by section 9.1 of the PCI-to-PCI bridge specification for devices
 * behind bridges on add-in cards.  For devices with ARI enabled, the slot
 * number is always 0 (see the Implementation Note in section 2.2.8.1 of
 * the PCI Express Base Specification, Revision 2.1)
 */

u8 pci_swizzle_interrupt_pin(const struct pci_dev *dev, u8 pin)
{
	int slot;

	if (pci_ari_enabled(dev->bus))
		slot = 0;
	else
		slot = PCI_SLOT(dev->devfn);

	return (((pin - 1) + slot) % 4) + 1;
}
int pci_get_interrupt_pin(struct pci_dev *dev, struct pci_dev **bridge)
{
	u8 pin;

	pin = dev->pin;
	if (!pin)
		return -1;

	while (!pci_is_root_bus(dev->bus)) {
		pin = pci_swizzle_interrupt_pin(dev, pin);
		dev = dev->bus->self;
	}
	*bridge = dev;
	return pin;
}

/**
 * pci_common_swizzle - swizzle INTx all the way to root bridge
 * @dev: the PCI device
 * @pinp: pointer to the INTx pin value (1=INTA, 2=INTB, 3=INTD, 4=INTD)
 *
 * Perform INTx swizzling for a device.  This traverses through all PCI-to-PCI
 * bridges all the way up to a PCI root bus.
 */

u8 pci_common_swizzle(struct pci_dev *dev, u8 *pinp)
{
	u8 pin = *pinp;

	while (!pci_is_root_bus(dev->bus)) {
		pin = pci_swizzle_interrupt_pin(dev, pin);
		dev = dev->bus->self;
	}
	*pinp = pin;
	return PCI_SLOT(dev->devfn);
}
EXPORT_SYMBOL_GPL(pci_common_swizzle);

void pci_assign_irq(struct pci_dev *dev)
{
	u8 pin;
	u8 slot = -1;
	int irq = 0;
	struct pci_host_bridge *hbrg = pci_find_host_bridge(dev->bus);

	if (!(hbrg->map_irq)) {
		pci_dbg(dev, "runtime IRQ mapping not provided by arch\n");
		return;
	}

	/*
	 * If this device is not on the primary bus, we need to figure out
	 * which interrupt pin it will come in on. We know which slot it
	 * will come in on because that slot is where the bridge is. Each
	 * time the interrupt line passes through a PCI-PCI bridge we must
	 * apply the swizzle function.
	 */
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	/* Cope with illegal. */
	if (pin > 4)
		pin = 1;

	if (pin) {
		/* Follow the chain of bridges, swizzling as we go. */
		if (hbrg->swizzle_irq)
			slot = (*(hbrg->swizzle_irq))(dev, &pin);

		/*
		 * If a swizzling function is not used, map_irq() must
		 * ignore slot.
		 */
		irq = (*(hbrg->map_irq))(dev, slot, pin);
		if (irq == -1)
			irq = 0;
	}
	dev->irq = irq;
	pci_dbg(dev, "assign IRQ: got %d\n", dev->irq);

	/*
	 * Always tell the device, so the driver knows what is the real IRQ
	 * to use; the device does not use it.
	 */
	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);
}
static bool pci_check_and_set_intx_mask(struct pci_dev *dev, bool mask)
{
	struct pci_bus *bus = dev->bus;
	bool mask_updated = true;
	u32 cmd_status_dword;
	u16 origcmd, newcmd;
	unsigned long flags;
	bool irq_pending;

	/*
	 * We do a single dword read to retrieve both command and status.
	 * Document assumptions that make this possible.
	 */
	BUILD_BUG_ON(PCI_COMMAND % 4);
	BUILD_BUG_ON(PCI_COMMAND + 2 != PCI_STATUS);

	raw_spin_lock_irqsave(&pci_lock, flags);

	bus->ops->read(bus, dev->devfn, PCI_COMMAND, 4, &cmd_status_dword);

	irq_pending = (cmd_status_dword >> 16) & PCI_STATUS_INTERRUPT;

	/*
	 * Check interrupt status register to see whether our device
	 * triggered the interrupt (when masking) or the next IRQ is
	 * already pending (when unmasking).
	 */
	if (mask != irq_pending) {
		mask_updated = false;
		goto done;
	}

	origcmd = cmd_status_dword;
	newcmd = origcmd & ~PCI_COMMAND_INTX_DISABLE;
	if (mask)
		newcmd |= PCI_COMMAND_INTX_DISABLE;
	if (newcmd != origcmd)
		bus->ops->write(bus, dev->devfn, PCI_COMMAND, 2, newcmd);

done:
	raw_spin_unlock_irqrestore(&pci_lock, flags);

	return mask_updated;
}

/**
 * pci_check_and_mask_intx - mask INTx on pending interrupt
 * @dev: the PCI device to operate on
 *
 * Check if the device dev has its INTx line asserted, mask it and return
 * true in that case. False is returned if no interrupt was pending.
 */

bool pci_check_and_mask_intx(struct pci_dev *dev)
{
	return pci_check_and_set_intx_mask(dev, true);
}
EXPORT_SYMBOL_GPL(pci_check_and_mask_intx);

/**
 * pci_check_and_unmask_intx - unmask INTx if no interrupt is pending
 * @dev: the PCI device to operate on
 *
 * Check if the device dev has its INTx line asserted, unmask it if not and
 * return true. False is returned and the mask remains active if there was
 * still an interrupt pending.
 */

bool pci_check_and_unmask_intx(struct pci_dev *dev)
{
	return pci_check_and_set_intx_mask(dev, false);
}
EXPORT_SYMBOL_GPL(pci_check_and_unmask_intx);

/**
 * pcibios_penalize_isa_irq - penalize an ISA IRQ
 * @irq: ISA IRQ to penalize
 * @active: IRQ active or not
 *
 * Permits the platform to provide architecture-specific functionality when
 * penalizing ISA IRQs. This is the default implementation. Architecture
 * implementations can override this.
 */

void __weak pcibios_penalize_isa_irq(int irq, int active) {}

int __weak pcibios_alloc_irq(struct pci_dev *dev)
{
	return 0;
}

void __weak pcibios_free_irq(struct pci_dev *dev)
{
}
