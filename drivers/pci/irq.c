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
	/* [한국어] 슬롯 번호를 담을 곳. 회전량이 된다. */
	int slot;

	/* [한국어] ARI(Alternative Routing-ID)가 켜져 있으면, */
	if (pci_ari_enabled(dev->bus))
		/* [한국어] 위 영어 주석대로 슬롯 번호를 언제나 0 으로 본다. ARI 는 devfn 의
		 * 상위 5비트를 슬롯이 아니라 함수 번호의 일부로 재해석하므로,
		 * PCI_SLOT() 이 뽑아낸 값이 슬롯이 아니게 되기 때문이다.
		 * 근거는 PCIe Base 2.1 의 2.2.8.1 구현 노트다. */
		slot = 0;
	else
		/* [한국어] 보통은 devfn 의 상위 5비트가 슬롯 번호다. */
		slot = PCI_SLOT(dev->devfn);

	/* [한국어] 실제 회전. pin 은 1~4(INTA~INTD)인데 나머지 연산은 0 기준이라
	 * 1 을 빼고 계산한 뒤 다시 1 을 더한다. 슬롯 번호만큼 돌리는 이 규칙이
	 * PCI-to-PCI 브리지 규격 9.1 절이 정한 것으로, 카드 위의 여러 장치가
	 * 같은 INTx 선에 몰리지 않게 흩뜨리는 효과가 있다. */
	return (((pin - 1) + slot) % 4) + 1;
}
/* [한국어]
 * pci_get_interrupt_pin - 루트 버스까지 회전시킨 최종 INTx 핀과 그 브리지를 얻는다
 *
 * @dev: 출발 장치.
 * @bridge: 루트 버스 바로 아래의 최상단 브리지를 여기에 담아 준다.
 * @return: 회전이 끝난 핀 번호(1~4), 또는 -1 = 이 장치는 INTx 를 쓰지 않음.
 *
 * 장치가 자기 config 에 적어 둔 핀 번호는 그 장치 바로 옆에서만 유효하다.
 * 브리지를 하나 지날 때마다 규격이 정한 대로 핀이 회전하므로, 루트에서
 * 실제로 어느 선에 실리는지 알려면 경로 전체를 따라가며 회전시켜야 한다.
 *
 * dev 를 지역에서 덮어쓰며 올라가는 것이 이 함수의 관용구다. 인자로 받은
 * 포인터를 그대로 커서로 쓰며, 루프가 끝나면 그 자리에 최상단 브리지가
 * 남아 있어 그것을 그대로 *bridge 에 담는다.
 *
 * 핀이 없을 때 0 이 아니라 -1 을 반환하는 것은, 0 이 유효한 핀 번호가 아니면서
 * 동시에 "핀 없음" 을 뜻해 반환값으로 쓰면 호출자가 헷갈리기 때문이다.
 *
 * 아래 pci_common_swizzle() 과 루프가 완전히 같고 돌려주는 것만 다르다 —
 * 그쪽은 최상단 브리지의 슬롯 번호를, 이쪽은 브리지 장치 자체를 준다.
 *
 * 실행 컨텍스트: IRQ 배정 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: -1 뿐이다.
 *
 * 호출 체인:
 *   아키텍처의 IRQ 매핑 코드 → [이 함수] → pci_swizzle_interrupt_pin()(브리지 수만큼)
 */
int pci_get_interrupt_pin(struct pci_dev *dev, struct pci_dev **bridge)
{
	/* [한국어] 따라 올라가며 회전시킬 핀 번호. */
	u8 pin;

	/* [한국어] 장치가 실제로 쓰는 핀. 0 이면 INTx 를 쓰지 않는다는 뜻이다. */
	pin = dev->pin;
	/* [한국어] 핀이 없으면, */
	if (!pin)
		/* [한국어] -1 로 실패를 알린다. 0 이 아니라 -1 인 것은 0 이 유효한 핀 번호가
		 * 아니면서도 "핀 없음" 을 뜻하는 값이라 반환값으로 쓰면 헷갈리기 때문이다. */
		return -1;

	/* [한국어] 루트 버스에 닿을 때까지 브리지를 하나씩 거슬러 올라간다. */
	while (!pci_is_root_bus(dev->bus)) {
		/* [한국어] 브리지 하나를 지날 때마다 핀을 회전시킨다. */
		pin = pci_swizzle_interrupt_pin(dev, pin);
		/* [한국어] 부모 브리지로 올라간다. bus->self 가 그 버스를 만든 브리지 장치다. */
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
	/* [한국어] 들어온 핀 값을 지역 변수로 받는다. */
	u8 pin = *pinp;

	/* [한국어] 루트 버스까지 올라가며, */
	while (!pci_is_root_bus(dev->bus)) {
		/* [한국어] 핀을 회전시키고, */
		pin = pci_swizzle_interrupt_pin(dev, pin);
		/* [한국어] 부모로 올라간다. pci_get_interrupt_pin() 과 루프가 같지만 돌려주는 것이
		 * 다르다 — 그쪽은 최상단 브리지 장치를, 이쪽은 그 장치의 슬롯 번호를 준다. */
		dev = dev->bus->self;
	}
	*pinp = pin;
	return PCI_SLOT(dev->devfn);
}
EXPORT_SYMBOL_GPL(pci_common_swizzle);

/* [한국어]
 * pci_assign_irq - 이 장치의 INTx 를 실제 IRQ 번호로 매핑해 배정한다
 *
 * @dev: IRQ 를 배정할 장치.
 *
 * 열거 중 장치마다 한 번 불려, config 의 핀 번호를 커널의 IRQ 번호로 바꾼다.
 *
 * 세 단계다. 핀을 읽고, 브리지들을 거슬러 올라가며 회전시키고, 호스트 브리지가
 * 제공한 map_irq 콜백으로 최종 IRQ 를 얻는다.
 *
 * 호스트 브리지의 두 콜백이 이 함수의 확장점이다. map_irq 가 없으면 그
 * 아키텍처가 런타임 매핑을 지원하지 않는다는 뜻이라 조용히 물러난다.
 * swizzle_irq 는 선택 사항인데, 함수 안의 영어 주석대로 그것이 없으면
 * map_irq 는 slot 인자를 무시해야 한다. slot 초기값이 -1(u8 에서 0xff)인 것이
 * "쓰지 말라" 는 표시가 된다.
 *
 * 읽어 온 핀 값을 검사하는 대목이 방어적이다. config 공간의 값이 5 이상이면
 * 규격 위반이므로 INTA(1)로 되돌린다.
 *
 * irq 초기값이 0 인 것도 의도적이다. 핀이 0 이면 아래 블록을 통째로 건너뛰고
 * 그 0 이 그대로 dev->irq 에 들어가, 자연스럽게 "IRQ 없음" 이 된다.
 *
 * 마지막 config 쓰기는 하드웨어를 위한 것이 아니다. 함수 끝의 영어 주석대로
 * 장치 자신은 PCI_INTERRUPT_LINE 을 쓰지 않으며, 드라이버와 사용자 공간이
 * 읽어 볼 수 있도록 기록해 두는 것뿐이다.
 *
 * 실행 컨텍스트: 장치 열거 또는 핫플러그 추가 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없고, 실패는 dev->irq 가 0 으로 남는 것으로 표현된다.
 *
 * 호출 체인:
 *   pci_device_add() 계열 → [이 함수]
 *     → pci_find_host_bridge() → pci_read_config_byte(PCI_INTERRUPT_PIN)
 *     → hbrg->swizzle_irq() → hbrg->map_irq()
 *     → pci_write_config_byte(PCI_INTERRUPT_LINE)
 */
void pci_assign_irq(struct pci_dev *dev)
{
	/* [한국어] config 에서 읽어 올 핀 번호. */
	u8 pin;
	/* [한국어] 스위즐 함수가 알려 줄 슬롯 번호. -1 을 u8 에 넣어 0xff 가 되며,
	 * 스위즐 함수가 없을 때 그대로 남는다. */
	u8 slot = -1;
	/* [한국어] 배정할 IRQ. 실패 시 0 이 남도록 초기값이 0 이다. */
	int irq = 0;
	/* [한국어] 이 장치가 매달린 호스트 브리지. map_irq 와 swizzle_irq 콜백이 거기 있다. */
	struct pci_host_bridge *hbrg = pci_find_host_bridge(dev->bus);

	/* [한국어] 아키텍처가 런타임 IRQ 매핑 함수를 제공하지 않으면, */
	if (!(hbrg->map_irq)) {
		/* [한국어] 그 사실만 남기고, */
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
		/* [한국어] 위 영어 주석대로 잘못된 값(5 이상)이면 INTA 로 되돌린다. config 공간의
		 * 값을 그대로 믿을 수 없으므로 방어하는 것이다. */
		pin = 1;

	if (pin) {
		/* Follow the chain of bridges, swizzling as we go. */
		if (hbrg->swizzle_irq)
			/* [한국어] 호스트 브리지가 스위즐 함수를 제공하면 그것으로 최상단까지 회전시키고
			 * 슬롯 번호를 얻는다. pin 을 포인터로 넘겨 회전 결과가 그 자리에 반영된다. */
			slot = (*(hbrg->swizzle_irq))(dev, &pin);

		/*
		 * If a swizzling function is not used, map_irq() must
		 * ignore slot.
		 */
		irq = (*(hbrg->map_irq))(dev, slot, pin);
		/* [한국어] 매핑 실패는 -1 로 온다. */
		if (irq == -1)
			/* [한국어] IRQ 없음(0)으로 바꾼다. 호출자와 드라이버가 0 을 "IRQ 없음" 으로
			 * 약속하고 있기 때문이다. */
			irq = 0;
	}
	/* [한국어] 결과를 장치에 기록한다. 핀이 0 이었으면 여기까지 오면서 irq 가 초기값
	 * 0 인 채라, 자연스럽게 "IRQ 없음" 이 된다. */
	dev->irq = irq;
	/* [한국어] 배정 결과를 디버그 로그로 남긴다. */
	pci_dbg(dev, "assign IRQ: got %d\n", dev->irq);

	/*
	 * Always tell the device, so the driver knows what is the real IRQ
	 * to use; the device does not use it.
	 */
	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);
}
/* [한국어]
 * pci_check_and_set_intx_mask - 인터럽트 상태를 확인하고 조건이 맞을 때만 INTx 마스크를 바꾼다
 *
 * @dev: 대상 장치.
 * @mask: true = 마스크하려 함, false = 마스크를 풀려 함.
 * @return: true = 실제로 바꿨다, false = 조건이 맞지 않아 바꾸지 않았다.
 *
 * 공유 INTx 환경에서 "내 장치가 이 인터럽트를 올렸는가" 를 알아내는 함수다.
 * 아래 두 공개 함수가 인자만 바꿔 이것을 부른다.
 *
 * 핵심은 확인과 변경이 하나의 원자적 동작이어야 한다는 점이다. 상태를 읽고
 * "내 것이구나" 판단한 뒤 마스크하려는 사이에 다른 CPU 가 끼어들면 판정이
 * 무의미해진다. 그래서 pci_lock 을 인터럽트까지 끈 채 잡고, 그 안에서
 * 읽기와 쓰기를 모두 끝낸다. 이 함수가 인터럽트 핸들러에서 불리므로
 * 잠들 수 있는 락은 쓸 수 없다.
 *
 * 락 안에서 pci_read_config_dword() 대신 bus->ops->read 를 직접 부르는 것도
 * 그 때문이다. 공개 접근자는 같은 pci_lock 을 다시 잡으려 해 교착한다.
 *
 * Command 와 Status 를 dword 한 번에 읽는 최적화가 두 BUILD_BUG_ON 위에
 * 서 있다 — PCI_COMMAND 가 4의 배수이고 PCI_STATUS 가 그 바로 뒤 2바이트라는
 * 것. 규격상 바뀔 리 없지만 가정을 코드로 남겨 두었다.
 *
 * 판정 조건 `mask != irq_pending` 이 두 방향을 한 줄로 처리한다.
 * 마스크하려면 인터럽트가 떠 있어야 하고(내 장치가 올린 것이므로),
 * 마스크를 풀려면 떠 있지 않아야 한다(다음 인터럽트가 이미 대기 중이면
 * 풀면 안 되므로). 두 경우 모두 mask 와 irq_pending 이 같아야 진행한다.
 *
 * 실행 컨텍스트: 공유 IRQ 핸들러. 인터럽트 문맥이며 잠들 수 없다.
 *
 * 에러 경로: 없다. "바꾸지 않았다" 를 false 로 알릴 뿐이다.
 *
 * 호출 체인:
 *   pci_check_and_mask_intx() / pci_check_and_unmask_intx() → [이 함수]
 *     → raw_spin_lock_irqsave(pci_lock) → bus->ops->read/write
 */
static bool pci_check_and_set_intx_mask(struct pci_dev *dev, bool mask)
{
	/* [한국어] config 접근에 쓸 버스. */
	struct pci_bus *bus = dev->bus;
	/* [한국어] 마스크를 실제로 갱신했는지. 기본값 true 이고 아래 조기 반환 경로에서만
	 * false 가 된다. */
	bool mask_updated = true;
	/* [한국어] Command 와 Status 를 한 번에 담을 dword. */
	u32 cmd_status_dword;
	/* [한국어] 원래 Command 값과 새 값. */
	u16 origcmd, newcmd;
	/* [한국어] 인터럽트 저장용 플래그. */
	unsigned long flags;
	/* [한국어] 이 장치가 인터럽트를 올린 상태인지. */
	bool irq_pending;

	/*
	 * We do a single dword read to retrieve both command and status.
	 * Document assumptions that make this possible.
	 */
	BUILD_BUG_ON(PCI_COMMAND % 4);
	/* [한국어] 위 영어 주석대로 Command 와 Status 를 dword 한 번으로 읽기 위한 전제를
	 * 컴파일 시점에 못박는다. 하나는 PCI_COMMAND 가 4의 배수라는 것, 다른 하나는
	 * PCI_STATUS 가 그 바로 뒤 2바이트에 있다는 것이다. 규격이 바뀔 리는 없지만,
	 * 이 최적화가 어떤 가정 위에 서 있는지를 코드로 남겨 둔 것이다. */
	BUILD_BUG_ON(PCI_COMMAND + 2 != PCI_STATUS);

	/* [한국어] pci_lock 을 인터럽트를 끈 채 잡는다. 이 함수가 인터럽트 핸들러에서
	 * 불리므로 잠들 수 있는 락은 쓸 수 없고, 읽기와 쓰기 사이에 다른 config
	 * 접근이 끼어들면 판정이 어긋나기 때문에 한 임계 구역으로 묶는다. */
	raw_spin_lock_irqsave(&pci_lock, flags);

	/* [한국어] bus->ops->read 를 직접 부른다. pci_read_config_dword() 를 쓰지 않는 이유는
	 * 그 함수가 같은 pci_lock 을 다시 잡으려 해 교착하기 때문이다. */
	bus->ops->read(bus, dev->devfn, PCI_COMMAND, 4, &cmd_status_dword);

	/* [한국어] dword 의 상위 16비트가 Status 이므로 밀어 내린 뒤 인터럽트 비트를 본다. */
	irq_pending = (cmd_status_dword >> 16) & PCI_STATUS_INTERRUPT;

	/*
	 * Check interrupt status register to see whether our device
	 * triggered the interrupt (when masking) or the next IRQ is
	 * already pending (when unmasking).
	 */
	if (mask != irq_pending) {
		/* [한국어] 마스킹하려는데 인터럽트가 안 떠 있거나, 언마스킹하려는데 아직 떠 있으면
		 * 지금은 손댈 때가 아니다. 갱신하지 않았음을 표시하고, */
		mask_updated = false;
		goto done;
	}

	/* [한국어] 하위 16비트가 Command 다. u16 대입이 상위를 잘라 낸다. */
	origcmd = cmd_status_dword;
	/* [한국어] INTx 비활성화 비트를 먼저 지운 값을 만든다. */
	newcmd = origcmd & ~PCI_COMMAND_INTX_DISABLE;
	/* [한국어] 마스크하려는 경우에만, */
	if (mask)
		/* [한국어] 그 비트를 세운다. 지웠다가 조건부로 세우는 방식이라 mask 인자 하나로
		 * 양방향을 다룰 수 있다. */
		newcmd |= PCI_COMMAND_INTX_DISABLE;
	/* [한국어] 실제로 바뀔 때만, */
	if (newcmd != origcmd)
		/* [한국어] 쓴다. Command 는 2바이트이므로 폭을 2 로 준다. 값이 같으면 쓰지 않는 것은
		 * 불필요한 config 쓰기를 줄이려는 것이다. */
		bus->ops->write(bus, dev->devfn, PCI_COMMAND, 2, newcmd);

done:
	raw_spin_unlock_irqrestore(&pci_lock, flags);

	/* [한국어] 갱신했는지를 돌려준다. 공유 IRQ 핸들러가 이 값으로 "내 장치가 올린
	 * 인터럽트인가" 를 판단한다. */
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
	/* [한국어] mask 인자를 true 로 넘긴다. 이름이 길어진 대신 호출부에서 true/false 의
	 * 의미를 헷갈릴 일이 없다. */
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
	/* [한국어] false 로 넘긴다. 위와 완전히 대칭이다. */
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

/* [한국어] 기본 구현은 아무것도 하지 않는다. __weak 이므로 아키텍처가 같은 이름의
 * 함수를 정의하면 그쪽이 링크된다. ISA IRQ 를 PCI 에 배정하지 않도록
 * 표시해 두는 것이 원래 목적이고, 그런 사정이 있는 아키텍처만 채운다. */
void __weak pcibios_penalize_isa_irq(int irq, int active) {}

/* [한국어]
 * pcibios_alloc_irq - 장치에 IRQ 자원을 배정하는 아키텍처 훅 (기본 구현)
 *
 * @dev: 대상 장치.
 * @return: 0 = 성공. 기본 구현은 언제나 성공이다.
 *
 * __weak 이므로 아키텍처가 같은 이름의 함수를 정의하면 그쪽이 링크되고,
 * 정의하지 않으면 이 무동작 판이 쓰인다. ACPI 기반 시스템에서 _PRT 를 보고
 * IRQ 를 배정하는 일이 이 훅에서 이루어진다.
 *
 * 기본 구현이 실패하지 않는 것이 중요하다. 훅을 제공하지 않는 아키텍처에서는
 * "할 일이 없었다" 가 곧 성공이어야, 드라이버 바인딩이 진행되기 때문이다.
 *
 * 실행 컨텍스트: 드라이버 바인딩 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 기본 구현에는 없다. 아키텍처 판은 실패할 수 있고, 그때는
 * 드라이버가 붙지 않는다.
 *
 * 호출 체인:
 *   pci_device_probe() → [이 함수]
 */
int __weak pcibios_alloc_irq(struct pci_dev *dev)
{
	return 0;
}

/* [한국어] 짝이 되는 해제 함수. 마찬가지로 기본은 무동작이다. */
/* [한국어]
 * pcibios_free_irq - 배정했던 IRQ 자원을 놓는 아키텍처 훅 (기본 구현)
 *
 * @dev: 대상 장치.
 *
 * pcibios_alloc_irq() 의 짝이다. 역시 __weak 이며 기본 구현은 아무것도 하지
 * 않는다.
 *
 * 반환값이 없다는 점이 alloc 쪽과 다르다. 해제는 실패할 수 없어야 하고,
 * 실패해도 드라이버 언바인드를 되돌릴 방법이 없기 때문이다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_device_remove() → [이 함수]
 */
void __weak pcibios_free_irq(struct pci_dev *dev)
{
}
