// SPDX-License-Identifier: GPL-2.0
/*
 * PCI IRQ handling code
 *
 * Copyright (c) 2008 James Bottomley <James.Bottomley@HansenPartnership.com>
 * Copyright (C) 2017 Christoph Hellwig.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/irq.c)은 PCI 장치의 전통적인 INTx IRQ 및 범용
 * 인터럽트 요청/해제 API를 제공한다. NVMe SSD는 PCIe 엔드포인트로서
 * MSI/MSI-X를 선호하지만, 초기화 단계나 특정 quirk 상황에서 INTx를
 * 사용할 수도 있으며, 이 파일의 함수들은 NVMe 드라이버가 인터럽트
 * 핸들러를 등록/해제할 때 직접 호출된다.
 *
 * NVMe 드라이버에서의 주요 호출 경로:
 *   nvme_probe -> nvme_pci_enable -> pci_alloc_irq_vectors(1, 1,
 *   PCI_IRQ_ALL_TYPES) -> MSI/MSI-X 또는 INTx 벡터 할당
 *
 *   nvme_setup_io_queues -> nvme_calc_irq_sets ->
 *   pci_alloc_irq_vectors_affinity(1, irq_queues, PCI_IRQ_ALL_TYPES |
 *   PCI_IRQ_AFFINITY, &affd) -> NVMe admin + I/O 큐별 MSI-X 벡터 할당
 *
 *   queue_request_irq -> pci_request_irq(pdev, nvmeq->cq_vector,
 *   nvme_irq_check, nvme_irq, nvmeq, "nvme%dq%d", ...) ->
 *   request_threaded_irq()를 통해 커널 IRQ 레이어에 NVMe 큐 인터럽트
 *   등록. Completion Queue(CQ)에 대한 MSI-X 인터럽트가 발생하면
 *   nvme_irq_check가 먼저 실행된 뒤 nvme_irq 스레드에서 CQ 처리.
 *
 *   nvme_reset_work/nvme_remove -> pci_free_irq(pdev, nvmeq->cq_vector,
 *   nvmeq) -> 등록된 NVMe 인터럽트 핸들러 해제
 *
 *   nvme_pci_disable -> pci_free_irq_vectors(pdev) -> MSI/MSI-X/INTx
 *   벡터 전체 해제
 *
 * 특히 NVMe 관련성이 높은 부분:
 *   - pci_request_irq()/pci_free_irq(): NVMe 큐별 MSI-X 인터럽트를
 *     커널 IRQ 번호로 매핑하고 핸들러를 등록/해제한다.
 *   - pci_irq_vector(): NVMe 큐 인덱스(0=admin, 1...N=I/O)를 커널
 *     전역 IRQ 번호로 변환. NVMe 드라이버의 nvme_poll_irqdisable() 등에서
 *     disable_irq()/enable_irq() 직전에 사용된다.
 *   - pci_assign_irq()/pci_swizzle_interrupt_pin(): PCIe Root Port나
 *     bridge 뒤에 연결된 NVMe 장치의 INTx 라인을 Root Complex까지
 *     swizzling하고 플랫폼 IRQ 번호로 할당. MSI/MSI-X를 쓰지 못하는
 *     레거시 환경이나 quirk 장치에서 중요하다.
 *   - pci_check_and_mask_intx()/pci_check_and_unmask_intx(): NVMe가
 *     INTx를 사용할 때 인터럽트 마스킹/언마스킹과 pending 상태 확인을
 *     원자적으로 수행. MSI-X가 없는 일부 임베디드 NVMe 컨트롤러에서
 *     사용될 수 있다.
 *
 * 참고: MSI/MSI-X 벡터 할당 자체는 drivers/pci/msi.c 등에 있으며,
 * 본 파일은 할당된 벡터를 커널 IRQ 레이어에 연결하고 레거시 INTx를
 * 제어하는 역할을 담당한다.
 * ===================================================================
 */

#include <linux/device.h>	/* NVMe: PCI device 구조체 및 device model 관련 헤더 포함. */
#include <linux/kernel.h>	/* NVMe: 커널 기본 매크로와 함수를 사용하기 위한 헤더. */
#include <linux/errno.h>	/* NVMe: 오류 코드(ENOMEM 등) 정의 헤더. */
#include <linux/export.h>	/* NVMe: pci_request_irq 등의 심볼을 외부 모듈에 낼 때 사용. */
#include <linux/interrupt.h>	/* NVMe: request_threaded_irq(), irq_handler_t 등 IRQ API 헤더. */
#include <linux/pci.h>		/* NVMe: PCI 핵심 데이터 구조와 함수 선언 헤더. */

#include "pci.h"		/* NVMe: PCI 서브시스템 내부 전용 선언과 lock 등 포함. */

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
 * pci_request_irq:
 *   NVMe 큐에 대해 MSI-X/MSI/INTx 인터럽트 핸들러를 커널 IRQ 레이어에
 *   등록한다. NVMe 드라이버의 queue_request_irq()에서 호출되며,
 *   pdev는 NVMe 컨트롤러의 pci_dev, nr은 해당 큐에 할당된 벡터 인덱스,
 *   handler는 nvme_irq_check(하드IRQ), thread_fn은 nvme_irq(스레드IRQ),
 *   dev_id는 struct nvme_queue 포인터가 전달된다.
 */
int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
		irq_handler_t thread_fn, void *dev_id, const char *fmt, ...)
{
	va_list ap;			/* NVMe: 가변 인자 목록을 처리하기 위한 구조체. */
	int ret;			/* NVMe: request_threaded_irq()의 반환값 저장. */
	char *devname;			/* NVMe: /proc/interrupts 등에 표시될 IRQ 이름 버퍼. */
	unsigned long irqflags = IRQF_SHARED;	/* NVMe: 기본적으로 공유 인터럽트 플래그 설정. */

	if (!handler)			/* NVMe: 핸들러가 NULL이면 스레드 IRQ만 사용하는 경우. */
		irqflags |= IRQF_ONESHOT;	/* NVMe: ONESHOT 설정으로 스레드 핸들러 완료 전 인터럽트 재발생 방지. */

	va_start(ap, fmt);		/* NVMe: 가변 인자 목록 초기화. */
	devname = kvasprintf(GFP_KERNEL, fmt, ap);	/* NVMe: "nvme0q12" 같은 큐별 IRQ 이름을 커널 메모리에 할당. */
	va_end(ap);			/* NVMe: 가변 인자 처리 종료. */
	if (!devname)			/* NVMe: 이름 할당 실패 시. */
		return -ENOMEM;		/* NVMe: 메모리 부족 오류 반환. */

	ret = request_threaded_irq(pci_irq_vector(dev, nr), handler, thread_fn,
				   irqflags, devname, dev_id);
	/* NVMe: NVMe 큐 인덱스(nr)를 커널 IRQ 번호로 변환 후 스레드 IRQ 등록.
	 *      MSI-X 모드에서는 nr이 MSI-X table entry 번호이며, INTx일 때는
	 *      dev->irq가 사용된다. */
	if (ret)			/* NVMe: IRQ 등록 실패 시. */
		kfree(devname);		/* NVMe: 할당한 IRQ 이름 버퍼만 해제(IRQ 등록은 실패). */
	return ret;			/* NVMe: 성공(0) 또는 오류 코드 반환. */
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
 * pci_free_irq:
 *   pci_request_irq()로 등록한 NVMe 큐의 인터럽트 핸들러를 해제한다.
 *   NVMe 드라이버는 nvme_reset_work, nvme_remove, 큐 해제 시 호출하며,
 *   장치 자체의 인터럽트가 비활성화된 상태여야 안전하다.
 */
void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id)
{
	kfree(free_irq(pci_irq_vector(dev, nr), dev_id));
	/* NVMe: 큐 인덱스(nr)를 커널 IRQ 번호로 변환하여 free_irq() 호출.
	 *      free_irq()는 등록 시 사용했던 devname을 반환하며, 여기서
	 *      메모리를 해제한다. */
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

/*
 * pci_swizzle_interrupt_pin:
 *   한 단계 PCI-to-PCI bridge 뒤에 있는 NVMe 장치의 INTx 핀을 swizzling한다.
 *   NVMe SSD가 add-in card나 bridge 뒤에 장착된 경우, 하위 장치의 INTA~INTD가
 *   상위 bridge의 다른 핀으로 매핑되므로 이 함수로 올바른 핀을 계산한다.
 */
u8 pci_swizzle_interrupt_pin(const struct pci_dev *dev, u8 pin)
{
	int slot;		/* NVMe: 현재 PCI 장치의 물리 슬롯 번호. */

	if (pci_ari_enabled(dev->bus))	/* NVMe: ARI(Alternative Routing-ID Interpretation)가 켜져 있으면. */
		slot = 0;		/* NVMe: ARI에서는 slot 번호가 항상 0으로 간주된다. */
	else
		slot = PCI_SLOT(dev->devfn);	/* NVMe: devfn에서 상위 5비트(slot)를 추출. */

	return (((pin - 1) + slot) % 4) + 1;
	/* NVMe: 핀 번호와 slot 번호를 더해 4로 나눈 나머지로 bridge 뒤의
	 *      INTx 핀을 결정. 예: slot 1의 INTA는 상위 bridge의 INTB로 연결. */
}

/*
 * pci_get_interrupt_pin:
 *   NVMe 장치의 INTx 핀이 Root Complex에 도달할 때까지 bridge를 따라
 *   swizzling하고, 최종적으로 도달한 루트 bridge 측 장치를 *bridge에
 *   저장한다. 플랫폼 map_irq()에 넘길 pin과 bridge를 얻는 데 사용된다.
 */
int pci_get_interrupt_pin(struct pci_dev *dev, struct pci_dev **bridge)
{
	u8 pin;		/* NVMe: 현재 단계의 INTx 핀 번호. */

	pin = dev->pin;		/* NVMe: NVMe 장치의 PCI config 공간 INT_PIN 값을 읽어온다. */
	if (!pin)		/* NVMe: INT_PIN이 0이면 이 장치는 INTx를 사용하지 않는다. */
		return -1;	/* NVMe: INTx 미사용을 의미하는 -1 반환. */

	while (!pci_is_root_bus(dev->bus)) {	/* NVMe: 현재 bus가 root bus가 아닐 때까지 bridge를 따라 올라간다. */
		pin = pci_swizzle_interrupt_pin(dev, pin);	/* NVMe: bridge를 지날 때마다 INTx 핀을 swizzling. */
		dev = dev->bus->self;	/* NVMe: 상위 bridge의 pci_dev로 이동. */
	}
	*bridge = dev;		/* NVMe: 최종 root bus에 연결된 bridge 장치를 반환. */
	return pin;		/* NVMe: Root Complex에 도달한 INTx 핀 번호 반환. */
}

/**
 * pci_common_swizzle - swizzle INTx all the way to root bridge
 * @dev: the PCI device
 * @pinp: pointer to the INTx pin value (1=INTA, 2=INTB, 3=INTD, 4=INTD)
 *
 * Perform INTx swizzling for a device.  This traverses through all PCI-to-PCI
 * bridges all the way up to a PCI root bus.
 */

/*
 * pci_common_swizzle:
 *   NVMe 장치의 INTx 핀을 Root Complex까지 누적 swizzling한다.
 *   *pinp에 최종 핀을 기록하고, root bus에 연결된 장치의 slot 번호를
 *   반환한다. 레거시 플랫폼 INTx 라우팅 테이블에서 사용된다.
 */
u8 pci_common_swizzle(struct pci_dev *dev, u8 *pinp)
{
	u8 pin = *pinp;		/* NVMe: 호출자가 전달한 INTx 핀 값을 복사. */

	while (!pci_is_root_bus(dev->bus)) {	/* NVMe: root bus에 도달할 때까지 반복. */
		pin = pci_swizzle_interrupt_pin(dev, pin);	/* NVMe: bridge 단계별 핀 swizzling. */
		dev = dev->bus->self;	/* NVMe: 상위 bridge로 이동. */
	}
	*pinp = pin;		/* NVMe: 최종 swizzled 핀 번호를 호출자가 제공한 변수에 기록. */
	return PCI_SLOT(dev->devfn);	/* NVMe: root bus 장치의 slot 번호 반환. */
}
EXPORT_SYMBOL_GPL(pci_common_swizzle);

/*
 * pci_assign_irq:
 *   NVMe 장치의 INTx 라인을 host bridge가 제공하는 map_irq() 콜백을 통해
 *   플랫폼 IRQ 번호로 할당한다. NVMe가 MSI/MSI-X를 쓰지 못하는 경우
 *   dev->irq에 플랫폼 IRQ 번호가 설정되며, PCI config space의
 *   INTERRUPT_LINE 레지스터에도 기록된다.
 */
void pci_assign_irq(struct pci_dev *dev)
{
	u8 pin;			/* NVMe: INTERRUPT_PIN 레지스터 값. */
	u8 slot = -1;		/* NVMe: root 측 slot 번호(기본값 -1). */
	int irq = 0;		/* NVMe: 할당받은 플랫폼 IRQ 번호(기본 0). */
	struct pci_host_bridge *hbrg = pci_find_host_bridge(dev->bus);
	/* NVMe: NVMe 장치가 연결된 bus의 host bridge 획득. */

	if (!(hbrg->map_irq)) {	/* NVMe: 아키텍처/플랫폼이 runtime IRQ 매핑 함수를 제공하지 않으면. */
		pci_dbg(dev, "runtime IRQ mapping not provided by arch\n");
		return;		/* NVMe: IRQ 할당 없이 리턴. */
	}

	/*
	 * If this device is not on the primary bus, we need to figure out
	 * which interrupt pin it will come in on. We know which slot it
	 * will come in on because that slot is where the bridge is. Each
	 * time the interrupt line passes through a PCI-PCI bridge we must
	 * apply the swizzle function.
	 */
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	/* NVMe: NVMe 장치의 PCI config space에서 INT_PIN 바이트 읽기. */
	/* Cope with illegal. */
	if (pin > 4)		/* NVMe: 비정상적인 pin 값(5 이상)이면. */
		pin = 1;	/* NVMe: INTA로 강제 보정. */

	if (pin) {		/* NVMe: INT_PIN이 0이 아니면(1~4) INTx 라우팅 수행. */
		/* Follow the chain of bridges, swizzling as we go. */
		if (hbrg->swizzle_irq)	/* NVMe: host bridge에 swizzle 콜백이 있으면. */
			slot = (*(hbrg->swizzle_irq))(dev, &pin);
			/* NVMe: 플랫폼별 swizzle 함수를 호출해 root 측 slot/pin 계산. */

		/*
		 * If a swizzling function is not used, map_irq() must
		 * ignore slot.
		 */
		irq = (*(hbrg->map_irq))(dev, slot, pin);
		/* NVMe: 플랫폼 map_irq()를 호출해 NVMe 장치의 INTx에 대한
		 *      시스템 IRQ 번호를 할당받는다. */
		if (irq == -1)	/* NVMe: map_irq()가 -1을 반환하면 할당 실패. */
			irq = 0;	/* NVMe: IRQ 0으로 설정(미할당 표시). */
	}
	dev->irq = irq;		/* NVMe: pci_dev의 irq 필드에 최종 IRQ 번호 저장. */

	pci_dbg(dev, "assign IRQ: got %d\n", dev->irq);
	/* NVMe: 디버그 메시지로 할당된 IRQ 번호 출력. */

	/*
	 * Always tell the device, so the driver knows what is the real IRQ
	 * to use; the device does not use it.
	 */
	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);
	/* NVMe: PCI config space INTERRUPT_LINE 레지스터에 할당된 IRQ 번호
	 *      기록. NVMe 컨트롤러는 보통 MSI/MSI-X를 쓰지만, 레거시 호환을
	 *      위해 이 값을 기록한다. */
}

/*
 * pci_check_and_set_intx_mask:
 *   NVMe 장치의 PCI COMMAND/STATUS 레지스터를 원자적으로 읽고, INTx
 *   Disable 비트를 설정/해제한다. mask=true이면 pending 인터럽트가 있을
 *   때만 마스크를 설정하고, mask=false이면 pending이 없을 때만 언마스크.
 *   MSI/MSI-X를 지원하지 않는 NVMe 컨트롤러의 INTx 핸들링에서 사용될 수
 *   있다.
 */
static bool pci_check_and_set_intx_mask(struct pci_dev *dev, bool mask)
{
	struct pci_bus *bus = dev->bus;	/* NVMe: NVMe 장치가 연결된 PCI bus. */
	bool mask_updated = true;	/* NVMe: 마스크 레지스터가 실제로 변경되었는지 표시. */
	u32 cmd_status_dword;	/* NVMe: COMMAND(하위 16비트) + STATUS(상위 16비트)를 한 번에 읽은 값. */
	u16 origcmd, newcmd;	/* NVMe: COMMAND 레지스터 원본/새 값. */
	unsigned long flags;	/* NVMe: raw spinlock irqsave용 플래그 저장. */
	bool irq_pending;	/* NVMe: STATUS의 INTERRUPT pending 비트 상태. */

	/*
	 * We do a single dword read to retrieve both command and status.
	 * Document assumptions that make this possible.
	 */
	BUILD_BUG_ON(PCI_COMMAND % 4);
	/* NVMe: 컴파일 타임에 PCI_COMMAND가 4바이트 정렬되어 있음을 검사. */
	BUILD_BUG_ON(PCI_COMMAND + 2 != PCI_STATUS);
	/* NVMe: PCI_STATUS가 PCI_COMMAND로부터 정확히 2바이트 뒤에 위치함을
	 *      검사하여 한 dword로 둘 다 읽을 수 있음을 보장. */

	raw_spin_lock_irqsave(&pci_lock, flags);
	/* NVMe: PCI config 접근 시 동기화를 위해 전역 pci_lock 획득,
	 *      인터럽트는 저장핒두고 비활성화. */

	bus->ops->read(bus, dev->devfn, PCI_COMMAND, 4, &cmd_status_dword);
	/* NVMe: NVMe 장치의 COMMAND+STATUS 레지스터를 4바이트로 읽음. */

	irq_pending = (cmd_status_dword >> 16) & PCI_STATUS_INTERRUPT;
	/* NVMe: 상위 16비트(STATUS)에서 INTERRUPT pending 비트를 추출.
	 *      NVMe가 INTx로 인터럽트를 발생시켰는지 확인. */

	/*
	 * Check interrupt status register to see whether our device
	 * triggered the interrupt (when masking) or the next IRQ is
	 * already pending (when unmasking).
	 */
	if (mask != irq_pending) {	/* NVMe: 마스크 요청과 pending 상태가 불일치하면. */
		mask_updated = false;	/* NVMe: 레지스터 변경 없음을 표시. */
		goto done;		/* NVMe: lock 해제 후 반환. */
	}

	origcmd = cmd_status_dword;	/* NVMe: COMMAND 레지스터 원본 값(하위 16비트). */
	newcmd = origcmd & ~PCI_COMMAND_INTX_DISABLE;	/* NVMe: INTx Disable 비트를 일단 0으로 만든다. */
	if (mask)			/* NVMe: 마스크 요청이면. */
		newcmd |= PCI_COMMAND_INTX_DISABLE;	/* NVMe: INTx Disable 비트를 1로 설정. */
	if (newcmd != origcmd)		/* NVMe: COMMAND 레지스터 값이 실제로 바뀌어야 할 때만. */
		bus->ops->write(bus, dev->devfn, PCI_COMMAND, 2, newcmd);
		/* NVMe: 2바이트로 COMMAND 레지스터를 써서 INTx 마스크 상태 변경. */

done:
	raw_spin_unlock_irqrestore(&pci_lock, flags);
	/* NVMe: pci_lock 해제 및 인터럽트 상태 복원. */

	return mask_updated;	/* NVMe: 마스크가 실제로 갱신되었으면 true, 아니면 false. */
}

/**
 * pci_check_and_mask_intx - mask INTx on pending interrupt
 * @dev: the PCI device to operate on
 *
 * Check if the device dev has its INTx line asserted, mask it and return
 * true in that case. False is returned if no interrupt was pending.
 */

/*
 * pci_check_and_mask_intx:
 *   NVMe 장치가 INTx 라인을 asserted했으면 INTx를 마스크하고 true 반환.
 *   pending 인터럽트가 없으면 false 반환. MSI/MSI-X 미지원 NVMe에서
 *   인터럽트 처리 후 재진입 방지에 사용될 수 있다.
 */
bool pci_check_and_mask_intx(struct pci_dev *dev)
{
	return pci_check_and_set_intx_mask(dev, true);
	/* NVMe: mask=true로 INTx pending 확인 및 마스크 설정. */
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

/*
 * pci_check_and_unmask_intx:
 *   NVMe 장치의 INTx pending이 없으면 마스크를 해제(true 반환).
 *   pending이 여전히 있으면 마스크 유지(false 반환). 인터럽트 핸들러
 *   종료 후 다음 인터럽트를 받기 위해 언마스크할 때 사용된다.
 */
bool pci_check_and_unmask_intx(struct pci_dev *dev)
{
	return pci_check_and_set_intx_mask(dev, false);
	/* NVMe: mask=false로 pending 확인 및 언마스크. */
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

/*
 * pcibios_penalize_isa_irq:
 *   ISA IRQ 우선순위를 낮추는 아키텍처별 훅의 기본 구현. NVMe PCIe
 *   장치는 ISA IRQ와 직접 관련 없지만, 일부 레거시 플랫폼에서 INTx
 *   라우팅 시 ISA 충돌을 회피하기 위해 호출될 수 있다.
 */
void __weak pcibios_penalize_isa_irq(int irq, int active) {}

/*
 * pcibios_alloc_irq:
 *   아키텍처별 추가 IRQ 할당 훅의 기본 구현. NVMe 장치의 INTx 할당
 *   과정에서 플랫폼별 추가 작업이 필요할 때 재정의될 수 있다.
 */
int __weak pcibios_alloc_irq(struct pci_dev *dev)
{
	return 0;	/* NVMe: 기본적으로 성공(0) 반환. */
}

/*
 * pcibios_free_irq:
 *   pcibios_alloc_irq()에서 할당한 아키텍처별 IRQ 리소스를 해제하는
 *   기본 구현. NVMe 장치 제거 시 플랫폼 정리 작업에 사용될 수 있다.
 */
void __weak pcibios_free_irq(struct pci_dev *dev)
{
}
