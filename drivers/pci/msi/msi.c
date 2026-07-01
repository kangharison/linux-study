// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI)
 *
 * Copyright (C) 2003-2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 * Copyright (C) 2016 Christoph Hellwig.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/msi/msi.c)은 PCI/PCIe 장치의 MSI(Message Signaled
 * Interrupt) 및 MSI-X(Message Signaled Interrupt eXtended) 기능을 초기화,
 * 활성화, 비활성화, 복원하는 핵심 코드이다.
 *
 * NVMe SSD는 고성능 I/O를 위해 전통적인 INTx(legacy pin) 인터럽트 대신
 * MSI 또는 MSI-X를 사용한다. 특히 MSI-X는 하나의 NVMe 장치에 대해 수십
 * ~ 수백 개의 독립적인 인터럽트 벡터를 제공하므로, NVMe 드라이버가 각
 * Completion Queue(CQ)마다 전용 인터럽트 벡터를 할당하여 CPU affinity를
 * 세밀하게 조정할 수 있게 한다.
 *
 * NVMe 드라이버에서의 대표적인 호출 경로:
 *   nvme_probe()
 *     -> nvme_setup_irqs()
 *        -> pci_alloc_irq_vectors_affinity(PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY)
 *           -> __pci_enable_msix_range()  (MSI-X 우선)
 *              또는 __pci_enable_msi_range() (MSI-X 실패 시 MSI 후보)
 *        -> pci_irq_vector()              (각 벡터의 Linux virq 획득)
 *        -> pci_request_irq()             (nvme_irq 핸들러 등록)
 *     -> nvme_reset_work() / queue_request_irq()
 *        -> pci_irq_vector(), pci_request_irq()
 *   nvme_remove() / nvme_reset_work()
 *     -> pci_free_irq()
 *     -> pci_free_irq_vectors()
 *        -> pci_msix_shutdown() / pci_msi_shutdown()
 *
 * 본 파일에서 다루는 NVMe와 직접 연관된 주요 기능:
 *   - MSI/MSI-X capability 초기화 및 MSI message(Address/Data) 설정
 *   - MSI-X table(BAR 내 위치) 매핑 및 벡터별 mask/unmask
 *   - IRQ affinity mask 처리: NVMe 다중 큐의 CPU 분산에 사용
 *   - MSI/MSI-X 벡터 개수 협상: NVMe admin queue + I/O queue 수만큼 벡터 확보
 *   - 장치 D3cold/suspend 이후 __pci_restore_msi_state()로 MSI context 복원
 *   - P2PDMA/CMB, SR-IOV, AER, PCIe 포트 서비스, ATS, ReBAR, DPC 등은
 *     NVMe 입장에서 모두 PCIe 트랜잭션/주소 공간 기반 기능이며, MSI/MSI-X는
 *     이들과 독립적으로 동작하지만 NVMe 장치가 호스트/피어/가상함수 등에
 *     비동기 이벤트를 알리는 통로이므로 안정성에 결정적이다.
 *
 * 기존 커널 주석은 그대로 보존하고, NVMe 관점의 한국어 설명을 추가한다.
 * ===================================================================
 */
#include <linux/bitfield.h> /* NVMe: 비트 필드 매크로(u32/u16 추출/조합)를 위해 포함. */
#include <linux/err.h>      /* NVMe: 오류 코드(IS_ERR_VALUE 등) 정의 포함. */
#include <linux/export.h>   /* NVMe: 커널 심볼 EXPORT 매크로 포함. */
#include <linux/irq.h>      /* NVMe: Linux IRQ 코어 구조체(irq_data, irq_desc 등) 포함. */
#include <linux/irqdomain.h> /* NVMe: 하드웨어 irq ↔ 가상 irq(virq) 매핑 도메인 포함. */

#include "../pci.h"         /* NVMe: PCI 서브시스템 내부 헤더(구조체, 플래그, capability 오프셋). */
#include "msi.h"            /* NVMe: MSI 서브시스템 전용 내장 헤더(msi_desc, MSI_FLAG_* 등). */

bool pci_msi_enable = true; /* NVMe: 전역 MSI 활성화 스위치. 커널 부팅 시 "pci=nomsi"로 끌 수 있음. */

/**
 * pci_msi_supported - check whether MSI may be enabled on a device
 * @dev: pointer to the pci_dev data structure of MSI device function
 * @nvec: how many MSIs have been requested?
 *
 * Look at global flags, the device itself, and its parent buses
 * to determine if MSI/-X are supported for the device. If MSI/-X is
 * supported return 1, else return 0.
 *
 * NVMe: 이 함수는 NVMe SSD가 MSI/MSI-X를 사용할 수 있는지를 최상위에서
 * 검사한다. 전역 플래그, 장치 자체의 no_msi 플래그, 그리고 NVMe가 연결된
 * Root Port까지의 상위 bus가 MSI를 차단하는지(NO_MSI) 확인한다.
 * 예를 들어 SR-IOV VF가 연결된 bus나 특정 PCIe 스위치 하위 bus에서
 * MSI 라우팅이 불가능하면 NVMe는 INTx로 fallback해야 한다.
 **/
static int pci_msi_supported(struct pci_dev *dev, int nvec)
{
	struct pci_bus *bus; /* NVMe: NVMe 장치가 연결된 bus부터 root까지 거슬러 올라갈 포인터. */

	/* MSI must be globally enabled and supported by the device */
	if (!pci_msi_enable) /* NVMe: 커널 전체에서 MSI가 꺼져 있으면(예: pci=nomsi) 사용 불가. */
		return 0; /* NVMe: MSI 지원 불가를 의미하는 0 반환. */

	if (!dev || dev->no_msi) /* NVMe: 장치가 없거나, 해당 장치에 no_msi quirk가 설정된 경우. */
		return 0; /* NVMe: MSI 사용 불가. */

	/*
	 * You can't ask to have 0 or less MSIs configured.
	 *  a) it's stupid ..
	 *  b) the list manipulation code assumes nvec >= 1.
	 */
	if (nvec < 1) /* NVMe: 요청 벡터 수가 1 미만이면(예: 0개) 의미 없으므로 거부. */
		return 0; /* NVMe: 최소 1개 벡터가 필요. */

	/*
	 * Any bridge which does NOT route MSI transactions from its
	 * secondary bus to its primary bus must set NO_MSI flag on
	 * the secondary pci_bus.
	 *
	 * The NO_MSI flag can either be set directly by:
	 * - arch-specific PCI host bus controller drivers (deprecated)
	 * - quirks for specific PCI bridges
	 *
	 * or indirectly by platform-specific PCI host bridge drivers by
	 * advertising the 'msi_domain' property, which results in the
	 * NO_MSI flag when no MSI domain is found for this bridge
	 * at probe time.
	 */
	for (bus = dev->bus; bus; bus = bus->parent) /* NVMe: NVMe bus부터 root bus까지 모든 상위 bus 순회. */
		if (bus->bus_flags & PCI_BUS_FLAGS_NO_MSI) /* NVMe: 현재 bus에 MSI 트랜잭션 라우팅 금지 플래그가 설정됐는지 확인. */
			return 0; /* NVMe: 중간 bridge가 MSI를 라우팅하지 못하면 NVMe MSI 사용 불가. */

	return 1; /* NVMe: 모든 검사를 통과하면 NVMe 장치가 MSI/MSI-X를 사용할 수 있음. */
}

/*
 * pcim_msi_release:
 *   devm 리소스 해제 시 호출되는 MSI 관리 콜백이다.
 *   NVMe: NVMe 드라이버가 pci_alloc_irq_vectors_affinity() 등을 통해
 *   devm 방식으로 MSI를 획득한 경우, 장치 제거/정리 시 자동으로 호출되어
 *   할당된 IRQ 벡터를 반납한다.
 */
static void pcim_msi_release(void *pcidev)
{
	struct pci_dev *dev = pcidev; /* NVMe: 정리 대상 NVMe PCIe 장치의 pci_dev 포인터. */

	dev->is_msi_managed = false; /* NVMe: MSI가 devm에 의해 관리되지 않음으로 표시. */
	pci_free_irq_vectors(dev);   /* NVMe: NVMe가 사용하던 모든 MSI/MSI-X 벡터 해제. */
}

/*
 * Needs to be separate from pcim_release to prevent an ordering problem
 * vs. msi_device_data_release() in the MSI core code.
 *
 * TODO: Remove the legacy side-effect of pcim_enable_device() that
 * activates automatic IRQ vector management. This design is dangerous
 * and confusing because it switches normally un-managed functions
 * into managed mode. Drivers should explicitly manage their IRQ vectors
 * without this implicit behavior.
 *
 * The current implementation uses both pdev->is_managed and
 * pdev->is_msi_managed flags, which adds unnecessary complexity.
 * This should be simplified in a future kernel version.
 *
 * NVMe: pci_enable_device() 낚시를 통해 자동 관리 모드로 전환되는 것을
 * 방지하기 위해, MSI 전용 release 액션을 별도로 등록한다.
 */
static int pcim_setup_msi_release(struct pci_dev *dev)
{
	int ret; /* NVMe: devm 액션 등록 결과를 저장할 변수. */

	if (!pci_is_managed(dev) || dev->is_msi_managed) /* NVMe: 장치가 managed 모드가 아니거나 이미 MSI release가 등록됐으면 중복 등록 방지. */
		return 0; /* NVMe: 이미 설정 완료이거나 관리 불필요. */

	ret = devm_add_action(&dev->dev, pcim_msi_release, dev); /* NVMe: 장치 해제 시 pcim_msi_release()가 자동 호출되도록 devm 액션 등록. */
	if (ret) /* NVMe: devm 액션 등록 실패 시. */
		return ret; /* NVMe: 실패 오류 코드 반환. */

	dev->is_msi_managed = true; /* NVMe: MSI release 콜백 등록 완료 플래그 설정. */
	return 0; /* NVMe: 정상 등록 완료. */
}

/*
 * Ordering vs. devres: msi device data has to be installed first so that
 * pcim_msi_release() is invoked before it on device release.
 *
 * NVMe: MSI context(msi_device_data)를 먼저 설치하고, 그 다음 MSI release
 * 콜백을 등록함으로써 장치 제거 시 올바른 해제 순서를 보장한다.
 */
static int pci_setup_msi_context(struct pci_dev *dev)
{
	int ret = msi_setup_device_data(&dev->dev); /* NVMe: PCI 장치에 MSI 디스크립터 저장용 dev_msi_info 구조체 할당 및 초기화. */

	if (ret) /* NVMe: MSI 디바이스 데이터 할당 실패 시. */
		return ret; /* NVMe: 오류 코드 반환. */

	return pcim_setup_msi_release(dev); /* NVMe: MSI 관리용 devm release 콜백 등록. */
}

/*
 * Helper functions for mask/unmask and MSI message handling
 *
 * NVMe: MSI/MSI-X 벡터를 마스크/언마스크하고, MSI message(address/data)를
 * 읽고 쓰는 보조 함수들이다. NVMe의 per-CQ 인터럽트 제어(예: idle CQ
 * 마스크, hotplug 시 언마스크)에 직접 사용된다.
 */

/*
 * pci_msi_update_mask:
 *   MSI capability의 mask 레지스터를 업데이트한다.
 *   NVMe: NVMe 장치의 특정 MSI 벡터(들)를 소프트웨어적으로 마스크/언마스크.
 *   MSI-X는 테이블 entry의 vector control로 제어하지만, 레거시 MSI는
 *   capability 구조체 내 MSI Mask 비트를 사용한다.
 */
void pci_msi_update_mask(struct msi_desc *desc, u32 clear, u32 set)
{
	struct pci_dev *dev = msi_desc_to_pci_dev(desc); /* NVMe: MSI 디스크립터에서 해당 NVMe 장치의 pci_dev 획득. */
	raw_spinlock_t *lock = &dev->msi_lock; /* NVMe: MSI mask 레지스터 동시 접근 보호용 스핀락. */
	unsigned long flags; /* NVMe: 인터럽트 상태 저장용 플래그. */

	if (!desc->pci.msi_attrib.can_mask) /* NVMe: 장치/플랫폼이 MSI 마스킹을 지원하지 않으면. */
		return; /* NVMe: 아무 것도 하지 않고 리턴. */

	raw_spin_lock_irqsave(lock, flags); /* NVMe: MSI mask 레지스터 보호를 위해 인터럽트 끈 상태로 락 획득. */
	desc->pci.msi_mask &= ~clear; /* NVMe: clear 비트에 해당하는 마스크 비트를 0으로 해제. */
	desc->pci.msi_mask |= set;    /* NVMe: set 비트에 해당하는 마스크 비트를 1으로 설정. */
	pci_write_config_dword(dev, desc->pci.mask_pos, desc->pci.msi_mask); /* NVMe: 갱신된 mask 값을 PCI configuration space에 기록. */
	raw_spin_unlock_irqrestore(lock, flags); /* NVMe: 락 해제 및 인터럽트 상태 복원. */
}

/**
 * pci_msi_mask_irq - Generic IRQ chip callback to mask PCI/MSI interrupts
 * @data:	pointer to irqdata associated to that interrupt
 *
 * NVMe: irq_chip의 mask 콜백으로, 특정 Linux virq에 해당하는 NVMe MSI
 * 벡터를 마스크한다. NVMe 드라이버가 disable_irq() 등을 호출할 때
 * 하위 레벨에서 사용될 수 있다.
 */
void pci_msi_mask_irq(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data); /* NVMe: irq_data로부터 MSI 디스크립터 획득. */

	__pci_msi_mask_desc(desc, BIT(data->irq - desc->irq)); /* NVMe: 이 irq가 desc 내 몇 번째 벡터인지 계산 후 해당 비트만 마스크. */
}
EXPORT_SYMBOL_GPL(pci_msi_mask_irq);

/**
 * pci_msi_unmask_irq - Generic IRQ chip callback to unmask PCI/MSI interrupts
 * @data:	pointer to irqdata associated to that interrupt
 *
 * NVMe: irq_chip의 unmask 콜백으로, 마스크된 NVMe MSI 벡터를 다시
 * 활성화하여 인터럽트를 수신할 수 있게 한다.
 */
void pci_msi_unmask_irq(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data); /* NVMe: irq_data에서 MSI 디스크립터 획득. */

	__pci_msi_unmask_desc(desc, BIT(data->irq - desc->irq)); /* NVMe: desc 내 해당 벡터의 마스크 비트만 해제. */
}
EXPORT_SYMBOL_GPL(pci_msi_unmask_irq);

/*
 * __pci_read_msi_msg:
 *   현재 장치에 설정된 MSI/MSI-X message(Address Lo/Hi, Data)를 읽어온다.
 *   NVMe: NVMe 장치가 실제로 어느 주소로, 어떤 data 값으로 MSI를 발생시킬지
 *   확인할 때 사용. MSI-X는 BAR에 매핑된 테이블에서, MSI는 config space에서
 *   읽는다. D0 상태가 아니면 BUG_ON으로 방어한다.
 */
void __pci_read_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	struct pci_dev *dev = msi_desc_to_pci_dev(entry); /* NVMe: MSI 디스크립터에 연결된 NVMe 장치 획득. */

	BUG_ON(dev->current_state != PCI_D0); /* NVMe: D0가 아닌 상태에서는 MSI message 레지스터를 읽으면 안 됨. */

	if (entry->pci.msi_attrib.is_msix) { /* NVMe: MSI-X 벡터인 경우. */
		void __iomem *base = pci_msix_desc_addr(entry); /* NVMe: 해당 MSI-X entry가 매핑된 MMIO 주소 획득. */

		if (WARN_ON_ONCE(entry->pci.msi_attrib.is_virtual)) /* NVMe: 가상 MSI-X entry는 config가 없으므로 경고 후 리턴. */
			return; /* NVMe: 가상 entry는 message를 읽을 수 없음. */

		msg->address_lo = readl(base + PCI_MSIX_ENTRY_LOWER_ADDR); /* NVMe: MSI-X 테이블에서 하위 32bit address 읽기. */
		msg->address_hi = readl(base + PCI_MSIX_ENTRY_UPPER_ADDR); /* NVMe: MSI-X 테이블에서 상위 32bit address 읽기. */
		msg->data = readl(base + PCI_MSIX_ENTRY_DATA);             /* NVMe: MSI-X 테이블에서 interrupt data 읽기. */
	} else { /* NVMe: 레거시 MSI 벡터인 경우. */
		int pos = dev->msi_cap; /* NVMe: PCI config space 내 MSI capability 위치. */
		u16 data; /* NVMe: 16bit MSI data 임시 저장 변수. */

		pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_LO,
				      &msg->address_lo); /* NVMe: config space에서 MSI address 하위 32bit 읽기. */
		if (entry->pci.msi_attrib.is_64) { /* NVMe: 64bit MSI capable 장치인 경우. */
			pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,
					      &msg->address_hi); /* NVMe: 64bit address 상위 32bit 읽기. */
			pci_read_config_word(dev, pos + PCI_MSI_DATA_64, &data); /* NVMe: 64bit capability offset에서 data 읽기. */
		} else { /* NVMe: 32bit MSI only 장치인 경우. */
			msg->address_hi = 0; /* NVMe: 32bit 장치는 상위 address가 없으므로 0으로 고정. */
			pci_read_config_word(dev, pos + PCI_MSI_DATA_32, &data); /* NVMe: 32bit capability offset에서 data 읽기. */
		}
		msg->data = data; /* NVMe: 읽은 16bit data를 msg->data에 저장. */
	}
}

/*
 * pci_write_msg_msi:
 *   레거시 MSI capability에 message(address/data) 및 multiple message
 *   enabled 개수를 기록한다.
 *   NVMe: NVMe가 MSI(비-MSI-X) 모드로 동작할 때, IRQ 도메인이 할당한
 *   목적지 주소(APIC ID 등)와 data를 config space에 기록.
 */
static inline void pci_write_msg_msi(struct pci_dev *dev, struct msi_desc *desc,
				     struct msi_msg *msg)
{
	int pos = dev->msi_cap; /* NVMe: MSI capability 오프셋. */
	u16 msgctl; /* NVMe: MSI Message Control 레지스터 임시 값. */

	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl); /* NVMe: 현재 MSI control 레지스터 읽기. */
	msgctl &= ~PCI_MSI_FLAGS_QSIZE; /* NVMe: 기존 enabled vector 개수(QSIZE) 필드 클리어. */
	msgctl |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, desc->pci.msi_attrib.multiple); /* NVMe: 요청한 2^n 개수만큼 QSIZE 필드 설정. */
	pci_write_config_word(dev, pos + PCI_MSI_FLAGS, msgctl); /* NVMe: 갱신된 control 레지스터 쓰기. */

	pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_LO, msg->address_lo); /* NVMe: MSI 목적지 주소 하위 32bit 기록. */
	if (desc->pci.msi_attrib.is_64) { /* NVMe: 64bit capable NVMe 장치이면. */
		pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,  msg->address_hi); /* NVMe: MSI 목적지 주소 상위 32bit 기록. */
		pci_write_config_word(dev, pos + PCI_MSI_DATA_64, msg->data); /* NVMe: 64bit offset에 interrupt data 기록. */
	} else { /* NVMe: 32bit MSI 장치이면. */
		pci_write_config_word(dev, pos + PCI_MSI_DATA_32, msg->data); /* NVMe: 32bit offset에 interrupt data 기록. */
	}
	/* Ensure that the writes are visible in the device */
	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl); /* NVMe: config write가 장치에 도달했음을 보장하기 위한 read flush. */
}

/*
 * pci_write_msg_msix:
 *   MSI-X 테이블의 특정 entry에 message를 기록한다.
 *   NVMe: NVMe가 MSI-X를 사용할 때 각 CQ에 대응하는 MSI-X entry의
 *   address/data를 설정. 스펙상 unmasked 상태에서 address/data를 변경하면
 *   undefined behavior이므로, 먼저 마스크하고 쓴 뒤 원래 상태로 복원한다.
 */
static inline void pci_write_msg_msix(struct msi_desc *desc, struct msi_msg *msg)
{
	void __iomem *base = pci_msix_desc_addr(desc); /* NVMe: 대상 MSI-X entry의 MMIO 베이스 주소. */
	u32 ctrl = desc->pci.msix_ctrl; /* NVMe: 현재 entry의 vector control 캐시 값. */
	bool unmasked = !(ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT); /* NVMe: 현재 entry가 unmasked(인터럽트 활성) 상태인지 판단. */

	if (desc->pci.msi_attrib.is_virtual) /* NVMe: 가상 MSI-X entry는 실제 하드웨어가 없으므로 쓰지 않음. */
		return; /* NVMe: 가상 entry는 조기 리턴. */
	/*
	 * The specification mandates that the entry is masked
	 * when the message is modified:
	 *
	 * "If software changes the Address or Data value of an
	 * entry while the entry is unmasked, the result is
	 * undefined."
	 */
	if (unmasked) /* NVMe: entry가 현재 unmasked이면. */
		pci_msix_write_vector_ctrl(desc, ctrl | PCI_MSIX_ENTRY_CTRL_MASKBIT); /* NVMe: address/data 변경 전에 일시적으로 마스크. */

	writel(msg->address_lo, base + PCI_MSIX_ENTRY_LOWER_ADDR); /* NVMe: MSI-X 테이블 entry에 하위 32bit address 기록. */
	writel(msg->address_hi, base + PCI_MSIX_ENTRY_UPPER_ADDR); /* NVMe: MSI-X 테이블 entry에 상위 32bit address 기록. */
	writel(msg->data, base + PCI_MSIX_ENTRY_DATA);             /* NVMe: MSI-X 테이블 entry에 interrupt data 기록. */

	if (unmasked) /* NVMe: 쓰기 전에 unmasked였다면. */
		pci_msix_write_vector_ctrl(desc, ctrl); /* NVMe: 마스크를 해제하여 다시 인터럽트를 받을 수 있게 복원. */

	/* Ensure that the writes are visible in the device */
	readl(base + PCI_MSIX_ENTRY_DATA); /* NVMe: MMIO write가 장치에 도달했음을 보장하기 위한 read flush. */
}

/*
 * __pci_write_msi_msg:
 *   MSI/MSI-X message를 실제 하드웨어에 쓰고, 필요시 콜백을 호출한다.
 *   NVMe: NVMe 장치가 D0 상태이고 연결되어 있을 때에만 레지스터에 기록.
 *   전원 상태가 D3 등이면 하드웨어를 건드리지 않고 소프트웨어 캐시만 갱신.
 */
void __pci_write_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	struct pci_dev *dev = msi_desc_to_pci_dev(entry); /* NVMe: MSI 디스크립터로부터 NVMe 장치 획득. */

	if (dev->current_state != PCI_D0 || pci_dev_is_disconnected(dev)) { /* NVMe: D0가 아니거나 장치가 disconnect된 경우. */
		/* Don't touch the hardware now */
	} else if (entry->pci.msi_attrib.is_msix) { /* NVMe: MSI-X entry이면. */
		pci_write_msg_msix(entry, msg); /* NVMe: MSI-X 테이블에 message 기록. */
	} else { /* NVMe: 레거시 MSI entry이면. */
		pci_write_msg_msi(dev, entry, msg); /* NVMe: MSI capability에 message 기록. */
	}

	entry->msg = *msg; /* NVMe: 소프트웨어 캐시(entry->msg)도 갱신(suspend/resume 복원 시 사용). */

	if (entry->write_msi_msg) /* NVMe: platform/irq chip이 등록한 후처리 콜백이 있으면. */
		entry->write_msi_msg(entry, entry->write_msi_msg_data); /* NVMe: 콜백 호출(예: IOMMU/irqdomain 동기화). */
}

/*
 * pci_write_msi_msg:
 *   Linux virq 번호로 MSI 디스크립터를 찾아 message를 쓴다.
 *   NVMe: irqdomain 레벨에서 virq에 해당하는 NVMe MSI message를
 *   변경할 때 사용되는 외부 인터페이스.
 */
void pci_write_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	struct msi_desc *entry = irq_get_msi_desc(irq); /* NVMe: virq로부터 MSI 디스크립터 검색. */

	__pci_write_msi_msg(entry, msg); /* NVMe: 실제 message 쓰기 수행. */
}
EXPORT_SYMBOL_GPL(pci_write_msi_msg);


/* PCI/MSI specific functionality */

/*
 * pci_intx_for_msi:
 *   MSI/MSI-X 활성화/비활성화 시 legacy INTx를 끄거나 복원한다.
 *   NVMe: NVMe가 MSI/MSI-X를 사용하면 INTx 핀 인터럽트는 불필요하므로
 *   비활성화하여 중복 인터럽트/성능 저하를 방지. 단, 특정 buggy 칩셋은
 *   MSI 활성화 시에도 INTx를 끄면 안 되는 경우가 있어 dev_flags로 보호.
 */
static void pci_intx_for_msi(struct pci_dev *dev, int enable)
{
	if (!(dev->dev_flags & PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG)) /* NVMe: MSI-INTX disable bug가 없는 장치에 대해서만. */
		pci_intx(dev, enable); /* NVMe: INTx 인터럽트를 enable(1) 또는 disable(0). */
}

/*
 * pci_msi_set_enable:
 *   MSI capability의 MSI Enable 비트를 on/off 한다.
 *   NVMe: NVMe의 MSI 활성화/비활성화를 직접 제어. MSI-X와 달리 단일
 *   enable 비트로 모든 MSI vector를 한 번에 켜고 끈다.
 */
static void pci_msi_set_enable(struct pci_dev *dev, int enable)
{
	u16 control; /* NVMe: MSI Message Control 레지스터 임시 값. */

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control); /* NVMe: MSI control 레지스터 읽기. */
	control &= ~PCI_MSI_FLAGS_ENABLE; /* NVMe: Enable 비트 클리어. */
	if (enable) /* NVMe: enable 인자가 참이면. */
		control |= PCI_MSI_FLAGS_ENABLE; /* NVMe: Enable 비트 설정. */
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control); /* NVMe: 갱신된 control 값을 config space에 기록. */
}

/*
 * msi_setup_msi_desc:
 *   레거시 MSI 디스크립터를 초기화하고 디바이스 리스트에 삽입 준비.
 *   NVMe: NVMe가 MSI(1개 또는 multi-MSI)를 요청할 때, 요청한 벡터 수와
 *   affinity mask를 반영한 msi_desc를 생성한다. NVMe는 일반적으로 MSI-X를
 *   선호하지만, MSI-X를 지원하지 않는 구형 NVMe나 플랫폼 제한 시 MSI로
 *   fallback할 수 있다.
 */
static int msi_setup_msi_desc(struct pci_dev *dev, int nvec,
			      struct irq_affinity_desc *masks)
{
	struct msi_desc desc; /* NVMe: 임시 MSI 디스크립터(초기화 후 리스트에 복사됨). */
	u16 control; /* NVMe: MSI control 레지스터 값. */

	/* MSI Entry Initialization */
	memset(&desc, 0, sizeof(desc)); /* NVMe: 디스크립터를 0으로 초기화. */

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control); /* NVMe: MSI capability의 control 레지스터 읽기. */
	/* Lies, damned lies, and MSIs */
	if (dev->dev_flags & PCI_DEV_FLAGS_HAS_MSI_MASKING) /* NVMe: 장치 quirk로 인해 마스킹 capability가 있는 것으로 간주. */
		control |= PCI_MSI_FLAGS_MASKBIT; /* NVMe: maskbit 지원 플래그 강제 설정. */
	if (pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY)) /* NVMe: IRQ 도메인이 레거시 마스킹을 허용하지 않으면. */
		control &= ~PCI_MSI_FLAGS_MASKBIT; /* NVMe: maskbit 플래그 제거. */

	desc.nvec_used			= nvec; /* NVMe: 이 디스크립터가 사용할 벡터 개수 기록. */
	desc.pci.msi_attrib.is_64	= !!(control & PCI_MSI_FLAGS_64BIT); /* NVMe: 64bit MSI 지원 여부 기록. */
	desc.pci.msi_attrib.can_mask	= !!(control & PCI_MSI_FLAGS_MASKBIT); /* NVMe: 소프트웨어 마스킹 가능 여부 기록. */
	desc.pci.msi_attrib.default_irq	= dev->irq; /* NVMe: MSI 해제 시 복원할 기존 INTx IRQ 번호 저장. */
	desc.pci.msi_attrib.multi_cap	= FIELD_GET(PCI_MSI_FLAGS_QMASK, control); /* NVMe: 하드웨어가 지원하는 최대 multiple message 개수(2^N) 읽기. */
	desc.pci.msi_attrib.multiple	= ilog2(__roundup_pow_of_two(nvec)); /* NVMe: 요청한 nvec을 2의 거듭제곱으로 올림 후 log2값 산출. */
	desc.affinity			= masks; /* NVMe: CPU affinity 마스크 포인터 연결. */

	if (control & PCI_MSI_FLAGS_64BIT) /* NVMe: 64bit MSI capable이면. */
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_64; /* NVMe: 64bit offset의 mask 레지스터 위치. */
	else /* NVMe: 32bit MSI only이면. */
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_32; /* NVMe: 32bit offset의 mask 레지스터 위치. */

	/* Save the initial mask status */
	if (desc.pci.msi_attrib.can_mask) /* NVMe: 마스킹을 지원하면. */
		pci_read_config_dword(dev, desc.pci.mask_pos, &desc.pci.msi_mask); /* NVMe: 현재 mask 레지스터 값을 저장핒두었다가 복원 시 사용. */

	return msi_insert_msi_desc(&dev->dev, &desc); /* NVMe: 초기화된 디스크립터를 장치의 msi_desc 리스트에 삽입. */
}

/*
 * msi_verify_entries:
 *   IRQ 도메인이 할당한 MSI message 주소가 NVMe 장치의 msi_addr_mask를
 *   준수하는지 검증한다. 64bit 주소를 지원하지 않는 장치에는 상위 주소가
 *   0인 영역만 허용.
 *   NVMe: DMA/MSI address mask가 64bit 미만인 NVMe 컨트롤러(예: ReBAR이나
 *   ATS 없이 32bit로 동작하는 장치)에서 MSI 목적지 주소가 유효한지 확인.
 */
static int msi_verify_entries(struct pci_dev *dev)
{
	struct msi_desc *entry; /* NVMe: 장치의 MSI 디스크립터 순회용. */
	u64 address; /* NVMe: MSI 목적지 물리 주소(64bit 조합). */

	if (dev->msi_addr_mask == DMA_BIT_MASK(64)) /* NVMe: 장치가 64bit MSI 주소를 모두 지원하면 검증 불필요. */
		return 0; /* NVMe: 검증 통과. */

	msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) { /* NVMe: 모든 MSI 디스크립터를 순회. */
		address = (u64)entry->msg.address_hi << 32 | entry->msg.address_lo; /* NVMe: address_lo/hi를 64bit 주소로 조합. */
		if (address & ~dev->msi_addr_mask) { /* NVMe: 할당된 주소가 장치의 msi_addr_mask 범위를 벗어나면. */
			pci_err(dev, "arch assigned 64-bit MSI address %#llx above device MSI address mask %#llx\n",
				address, dev->msi_addr_mask); /* NVMe: 주소 범위 초과 오류 로그 출력. */
			break; /* NVMe: 첫 번째 위반 항목 발견 시 순회 중단. */
		}
	}
	return !entry ? 0 : -EIO; /* NVMe: 모든 entry를 통과했으면 0, 아니면 -EIO 반환. */
}

/*
 * __msi_capability_init:
 *   레거시 MSI capability를 실제로 초기화하고 IRQ 벡터를 할당한다.
 *   NVMe: NVMe 드라이버가 MSI 모드를 선택했을 때 호출되며, IRQ 도메인에서
 *   nvec 개수만큼의 가상 IRQ를 할당받아 config space에 기록한다.
 */
static int __msi_capability_init(struct pci_dev *dev, int nvec, struct irq_affinity_desc *masks)
{
	int ret = msi_setup_msi_desc(dev, nvec, masks); /* NVMe: MSI 디스크립터 초기화 및 장치 리스트 등록. */
	struct msi_desc *entry, desc; /* NVMe: entry는 실제 디스크립터, desc는 오류 복구용 복사본. */

	if (ret) /* NVMe: 디스크립터 설정 실패 시. */
		return ret; /* NVMe: 오류 반환. */

	/* All MSIs are unmasked by default; mask them all */
	entry = msi_first_desc(&dev->dev, MSI_DESC_ALL); /* NVMe: 방금 삽입된 첫 번째 MSI 디스크립터 획득. */
	pci_msi_mask(entry, msi_multi_mask(entry)); /* NVMe: 설정 도중 spurious 인터럽트 방지를 위해 모든 벡터 마스크. */
	/*
	 * Copy the MSI descriptor for the error path because
	 * pci_msi_setup_msi_irqs() will free it for the hierarchical
	 * interrupt domain case.
	 */
	memcpy(&desc, entry, sizeof(desc)); /* NVMe: 오류 발생 시 복구를 위해 디스크립터를 스택에 복사. */

	/* Configure MSI capability structure */
	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSI); /* NVMe: IRQ 도메인에 nvec 개의 MSI IRQ 할당 요청. */
	if (ret) /* NVMe: IRQ 할당 실패 시. */
		goto err; /* NVMe: 오류 처리 경로로 이동. */

	ret = msi_verify_entries(dev); /* NVMe: 할당된 MSI 주소가 장치 제약을 만족하는지 검증. */
	if (ret) /* NVMe: 주소 검증 실패 시. */
		goto err; /* NVMe: 오류 처리 경로로 이동. */

	/* Set MSI enabled bits	*/
	dev->msi_enabled = 1; /* NVMe: 소프트웨어 상태에서 MSI 활성화 표시. */
	pci_intx_for_msi(dev, 0); /* NVMe: MSI를 쓰므로 legacy INTx 인터럽트 비활성화. */
	pci_msi_set_enable(dev, 1); /* NVMe: PCI config space의 MSI Enable 비트 설정. */

	pcibios_free_irq(dev); /* NVMe: BIOS/플랫폼이 관리하던 기존 INTx IRQ 자원 반납. */
	dev->irq = entry->irq; /* NVMe: dev->irq를 첫 번째 MSI vector의 Linux virq로 갱신. */
	return 0; /* NVMe: MSI 초기화 성공. */
err:
	pci_msi_unmask(&desc, msi_multi_mask(&desc)); /* NVMe: 오류 시 복사본을 이용해 마스크 해제(초기 상태 복원). */
	pci_free_msi_irqs(dev); /* NVMe: 할당했던 MSI IRQ 및 리소스 해제. */
	return ret; /* NVMe: 오류 코드 반환. */
}

/**
 * msi_capability_init - configure device's MSI capability structure
 * @dev: pointer to the pci_dev data structure of MSI device function
 * @nvec: number of interrupts to allocate
 * @affd: description of automatic IRQ affinity assignments (may be %NULL)
 *
 * Setup the MSI capability structure of the device with the requested
 * number of interrupts.  A return value of zero indicates the successful
 * setup of an entry with the new MSI IRQ.  A negative return value indicates
 * an error, and a positive return value indicates the number of interrupts
 * which could have been allocated.
 *
 * NVMe: NVMe 드라이버가 pci_alloc_irq_vectors_affinity()를 통해 MSI를
 * 요청할 때, IRQ 도메인이 multi-MSI를 지원하지 않으면 1을 반환하여
 * 상위 루프가 벡터 개수를 조정하게 한다.
 */
static int msi_capability_init(struct pci_dev *dev, int nvec,
			       struct irq_affinity *affd)
{
	/* Reject multi-MSI early on irq domain enabled architectures */
	if (nvec > 1 && !pci_msi_domain_supports(dev, MSI_FLAG_MULTI_PCI_MSI, ALLOW_LEGACY)) /* NVMe: multi-MSI를 요청했는데 도메인이 지원하지 않으면. */
		return 1; /* NVMe: 1개만 가능함을 상위에 알림(양수 = 가능 개수). */

	/*
	 * Disable MSI during setup in the hardware, but mark it enabled
	 * so that setup code can evaluate it.
	 */
	pci_msi_set_enable(dev, 0); /* NVMe: 설정 중에는 하드웨어의 MSI Enable을 끈다. */

	struct irq_affinity_desc *masks __free(kfree) =
		affd ? irq_create_affinity_masks(nvec, affd) : NULL; /* NVMe: affinity 정보가 있으면 nvec개의 CPU affinity 마스크 생성. */

	guard(msi_descs_lock)(&dev->dev); /* NVMe: msi_desc 리스트 보호 락 획득(scoped). */
	return __msi_capability_init(dev, nvec, masks); /* NVMe: 실제 MSI 초기화 함수 호출. */
}

/*
 * __pci_enable_msi_range:
 *   NVMe 드라이버가 요청한 [minvec, maxvec] 범위 내에서 MSI 벡터를
 *   확보한다. PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY 플래그가 넘어올 때
 *   MSI-X가 먼저 시도되고, 실패 시 MSI 후보로 이 함수가 호출될 수 있다.
 *   NVMe: NVMe의 admin queue + I/O queue 수만큼 벡터를 얻기 위해
 *   점진적으로 개수를 조정하며 할당한다.
 */
int __pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec,
			   struct irq_affinity *affd)
{
	int nvec; /* NVMe: 현재 시도 중인 벡터 개수. */
	int rc;   /* NVMe: 중간 결과 저장. */

	if (!pci_msi_supported(dev, minvec) || dev->current_state != PCI_D0) /* NVMe: MSI 지원/장치 상태 검사. */
		return -EINVAL; /* NVMe: MSI 사용 불가 또는 장치가 D0가 아니면 거부. */

	/* Check whether driver already requested MSI-X IRQs */
	if (dev->msix_enabled) { /* NVMe: 이미 MSI-X가 활성화된 상태면. */
		pci_info(dev, "can't enable MSI (MSI-X already enabled)\n"); /* NVMe: MSI-X와 MSI는 동시에 활성화될 수 없음을 알림. */
		return -EINVAL; /* NVMe: 오류 반환. */
	}

	if (maxvec < minvec) /* NVMe: 범위가 역전됐으면. */
		return -ERANGE; /* NVMe: 잘못된 범위. */

	if (WARN_ON_ONCE(dev->msi_enabled)) /* NVMe: 이미 MSI가 활성화된 상태에서 다시 호출되면 경고. */
		return -EINVAL; /* NVMe: 중복 활성화 방지. */

	/* Test for the availability of MSI support */
	if (!pci_msi_domain_supports(dev, 0, ALLOW_LEGACY)) /* NVMe: IRQ 도메인이 기본 MSI조차 지원하지 않으면. */
		return -ENOTSUPP; /* NVMe: 지원되지 않음. */

	nvec = pci_msi_vec_count(dev); /* NVMe: 장치가 지원하는 최대 MSI 벡터 개수 조회. */
	if (nvec < 0) /* NVMe: 오류 코드 반환 시. */
		return nvec; /* NVMe: 오류 그대로 반환. */
	if (nvec < minvec) /* NVMe: 하드웨어 최대값이 minvec보다 작으면. */
		return -ENOSPC; /* NVMe: 공간 부족. */

	rc = pci_setup_msi_context(dev); /* NVMe: MSI 디바이스 데이터 및 devm release 콜백 준비. */
	if (rc) /* NVMe: MSI context 준비 실패 시. */
		return rc; /* NVMe: 오류 반환. */

	if (!pci_setup_msi_device_domain(dev, nvec)) /* NVMe: 장치별 MSI irqdomain 설정. */
		return -ENODEV; /* NVMe: 도메인 설정 실패. */

	if (nvec > maxvec) /* NVMe: 하드웨어 최대값이 요청 max를 초과하면. */
		nvec = maxvec; /* NVMe: maxvec으로 제한. */

	for (;;) { /* NVMe: 원하는 개수만큼 얻을 때까지 재시도 루프. */
		if (affd) { /* NVMe: affinity 정보가 주어진 경우. */
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd); /* NVMe: affinity 정책에 따라 실제 할당 가능한 벡터 수 재계산. */
			if (nvec < minvec) /* NVMe: 재계산 후 minvec 미만이면. */
				return -ENOSPC; /* NVMe: CPU affinity 제약으로 할당 불가. */
		}

		rc = msi_capability_init(dev, nvec, affd); /* NVMe: nvec 개의 MSI 벡터 초기화 시도. */
		if (rc == 0) /* NVMe: 성공하면. */
			return nvec; /* NVMe: 확보한 벡터 수 반환. */

		if (rc < 0) /* NVMe: 치명적 오류면. */
			return rc; /* NVMe: 오류 반환. */
		if (rc < minvec) /* NVMe: 가능한 개수가 minvec보다 작으면. */
			return -ENOSPC; /* NVMe: 최소 요구 불만족. */

		nvec = rc; /* NVMe: 상위가 제안한 더 작은 개수로 다시 시도. */
	}
}

/**
 * pci_msi_vec_count - Return the number of MSI vectors a device can send
 * @dev: device to report about
 *
 * This function returns the number of MSI vectors a device requested via
 * Multiple Message Capable register. It returns a negative errno if the
 * device is not capable sending MSI interrupts. Otherwise, the call succeeds
 * and returns a power of two, up to a maximum of 2^5 (32), according to the
 * MSI specification.
 *
 * NVMe: NVMe 컨트롤러가 MSI 모드에서 지원하는 최대 벡터 수를 반환.
 * Multiple Message Capable 필드를 읽어 2^N 형태로 계산한다.
 **/
int pci_msi_vec_count(struct pci_dev *dev)
{
	int ret; /* NVMe: 반환할 벡터 개수. */
	u16 msgctl; /* NVMe: MSI control 레지스터 값. */

	if (!dev->msi_cap) /* NVMe: 장치에 MSI capability가 없으면. */
		return -EINVAL; /* NVMe: MSI 지원 불가. */

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &msgctl); /* NVMe: MSI control 레지스터 읽기. */
	ret = 1 << FIELD_GET(PCI_MSI_FLAGS_QMASK, msgctl); /* NVMe: QMASK 값을 기반으로 2^N 벡터 개수 산출. */

	return ret; /* NVMe: 최대 MSI 벡터 수 반환. */
}
EXPORT_SYMBOL(pci_msi_vec_count);

/*
 * Architecture override returns true when the PCI MSI message should be
 * written by the generic restore function.
 *
 * NVMe: 아키텍처별로 MSI message 복원 방식이 다를 수 있다. 기본적으로
 * true를 반환하여 generic 복원 루틴이 config space/MSI-X table에 message를
 * 다시 쓰도록 한다.
 */
bool __weak arch_restore_msi_irqs(struct pci_dev *dev)
{
	return true; /* NVMe: 기본적으로 generic restore가 message를 기록해야 함. */
}

/*
 * __pci_restore_msi_state:
 *   NVMe 장치가 suspend에서 resume되거나 D3에서 D0로 돌아온 후, 저장된
 *   MSI context를 복원한다. MSI Enable, message address/data, mask,
 *   multiple message 설정을 모두 복원.
 *   NVMe: NVMe SSD 런타임 전원 관리나 AER(Advanced Error Reporting) 복구
 *   후 MSI가 끊기지 않도록 하는 데 필수적이다.
 */
void __pci_restore_msi_state(struct pci_dev *dev)
{
	struct msi_desc *entry; /* NVMe: 복원할 MSI 디스크립터. */
	u16 control; /* NVMe: MSI control 레지스터 임시 값. */

	if (!dev->msi_enabled) /* NVMe: MSI가 활성화된 상태가 아니면. */
		return; /* NVMe: 복원할 것이 없음. */

	entry = irq_get_msi_desc(dev->irq); /* NVMe: dev->irq에 해당하는 MSI 디스크립터 획득. */

	pci_intx_for_msi(dev, 0); /* NVMe: INTx를 끄고 MSI를 사용하도록 설정. */
	pci_msi_set_enable(dev, 0); /* NVMe: 복원 중에는 일시적으로 MSI Enable을 끔. */
	if (arch_restore_msi_irqs(dev)) /* NVMe: 아키텍처가 generic message 복원을 허용하면. */
		__pci_write_msi_msg(entry, &entry->msg); /* NVMe: 저장된 MSI message를 다시 기록. */

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control); /* NVMe: 현재 MSI control 레지스터 읽기. */
	pci_msi_update_mask(entry, 0, 0); /* NVMe: mask 레지스터를 원래 값으로 복원(clear/set 모두 0이므로 현재 캐시 값 유지). */
	control &= ~PCI_MSI_FLAGS_QSIZE; /* NVMe: QSIZE 필드 클리어. */
	control |= PCI_MSI_FLAGS_ENABLE |
		   FIELD_PREP(PCI_MSI_FLAGS_QSIZE, entry->pci.msi_attrib.multiple); /* NVMe: MSI Enable과 multiple message 개수 설정. */
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control); /* NVMe: 갱신된 control 레지스터 기록. */
}

/*
 * pci_msi_shutdown:
 *   레거시 MSI를 완전히 종료하고 INTx로 복원한다.
 *   NVMe: NVMe 드라이버가 pci_free_irq_vectors()를 호출하거나 장치 제거
 *   시 MSI 리소스를 반납. 이후 dev->irq는 legacy INTx IRQ로 돌아간다.
 */
void pci_msi_shutdown(struct pci_dev *dev)
{
	struct msi_desc *desc; /* NVMe: 첫 번째 MSI 디스크립터. */

	if (!pci_msi_enable || !dev || !dev->msi_enabled) /* NVMe: 전역 MSI off, 장치 없음, MSI 미활성화 시. */
		return; /* NVMe: 할 것 없음. */

	pci_msi_set_enable(dev, 0); /* NVMe: 하드웨어의 MSI Enable 비트 클리어. */
	pci_intx_for_msi(dev, 1); /* NVMe: legacy INTx 인터럽트를 다시 활성화. */
	dev->msi_enabled = 0; /* NVMe: 소프트웨어 상태에서 MSI 비활성화. */

	/* Return the device with MSI unmasked as initial states */
	desc = msi_first_desc(&dev->dev, MSI_DESC_ALL); /* NVMe: 첫 번째 MSI 디스크립터 획득. */
	if (!WARN_ON_ONCE(!desc)) /* NVMe: 디스크립터가 없으면 경고하지만 정리는 계속. */
		pci_msi_unmask(desc, msi_multi_mask(desc)); /* NVMe: 모든 MSI 벡터의 마스크를 해제하여 초기 상태로 복원. */

	/* Restore dev->irq to its default pin-assertion IRQ */
	dev->irq = desc->pci.msi_attrib.default_irq; /* NVMe: dev->irq를 MSI 이전의 INTx IRQ 번호로 복원. */
	pcibios_alloc_irq(dev); /* NVMe: BIOS/플랫폼에 INTx IRQ 재할당 요청. */
}

/* PCI/MSI-X specific functionality */

/*
 * pci_msix_clear_and_set_ctrl:
 *   MSI-X capability의 Message Control 레지스터에서 clear/set할 비트를
 *   한 번에 처리한다.
 *   NVMe: NVMe 장치의 MSI-X Enable, Function Mask, Table Size 관련
 *   플래그를 원자적으로 갱신할 때 사용.
 */
static void pci_msix_clear_and_set_ctrl(struct pci_dev *dev, u16 clear, u16 set)
{
	u16 ctrl; /* NVMe: MSI-X control 레지스터 임시 값. */

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl); /* NVMe: MSI-X capability의 control 레지스터 읽기. */
	ctrl &= ~clear; /* NVMe: clear할 비트를 0으로 만듦. */
	ctrl |= set;    /* NVMe: set할 비트를 1로 설정. */
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl); /* NVMe: 갱신된 control 값을 config space에 기록. */
}

/*
 * msix_map_region:
 *   MSI-X 테이블이 위치한 BAR 영역을 물리 주소로 찾아 ioremap한다.
 *   NVMe: NVMe 장치의 BAR(보통 BAR0) 내에 있는 MSI-X table을 커널
 *   가상 주소로 매핑. NVMe가 MSI-X를 사용하면 커널은 이 테이블을 통해
 *   각 CQ 벡터의 address/data/mask를 제어한다.
 */
static void __iomem *msix_map_region(struct pci_dev *dev,
				     unsigned int nr_entries)
{
	resource_size_t phys_addr; /* NVMe: MSI-X 테이블의 물리 주소. */
	u32 table_offset; /* NVMe: BAR 내 MSI-X 테이블 오프셋(raw). */
	unsigned long flags; /* NVMe: 해당 BAR의 리소스 플래그. */
	u8 bir; /* NVMe: BAR Indicator Register(BAR 번호). */

	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE,
			      &table_offset); /* NVMe: MSI-X capability에서 테이블 offset/BIR 정보 읽기. */
	bir = (u8)(table_offset & PCI_MSIX_TABLE_BIR); /* NVMe: 하위 3bit에서 BAR 번호 추출. */
	flags = pci_resource_flags(dev, bir); /* NVMe: 해당 BAR의 IORESOURCE_MEM/IO 등 플래그 획득. */
	if (!flags || (flags & IORESOURCE_UNSET)) /* NVMe: BAR가 설정되지 않았거나 unset이면. */
		return NULL; /* NVMe: 매핑 불가. */

	table_offset &= PCI_MSIX_TABLE_OFFSET; /* NVMe: BIR을 제외한 순수 offset 부분만 남김. */
	phys_addr = pci_resource_start(dev, bir) + table_offset; /* NVMe: BAR 물리 시작 주소 + 테이블 offset으로 최종 물리 주소 산출. */

	return ioremap(phys_addr, nr_entries * PCI_MSIX_ENTRY_SIZE); /* NVMe: 테이블 물리 주소를 커널 가상 주소로 매핑 후 반환. */
}

/**
 * msix_prepare_msi_desc - Prepare a half initialized MSI descriptor for operation
 * @dev:	The PCI device for which the descriptor is prepared
 * @desc:	The MSI descriptor for preparation
 *
 * This is separate from msix_setup_msi_descs() below to handle dynamic
 * allocations for MSI-X after initial enablement.
 *
 * Ideally the whole MSI-X setup would work that way, but there is no way to
 * support this for the legacy arch_setup_msi_irqs() mechanism and for the
 * fake irq domains like the x86 XEN one. Sigh...
 *
 * The descriptor is zeroed and only @desc::msi_index and @desc::affinity
 * are set. When called from msix_setup_msi_descs() then the is_virtual
 * attribute is initialized as well.
 *
 * Fill in the rest.
 *
 * NVMe: NVMe 장치가 동적으로 MSI-X 벡터를 추가하거나(예: 큐 핫플러그),
 * 초기화 시점에 개별 MSI-X entry를 준비할 때 사용. is_msix, is_64,
 * mask_base(MSI-X table MMIO 주소), can_mask 등을 채운다.
 */
void msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc)
{
	desc->nvec_used			= 1; /* NVMe: MSI-X는 entry당 1개 벡터 사용. */
	desc->pci.msi_attrib.is_msix	= 1; /* NVMe: MSI-X 디스크립터임을 표시. */
	desc->pci.msi_attrib.is_64	= 1; /* NVMe: MSI-X는 항상 64bit address. */
	desc->pci.msi_attrib.default_irq= dev->irq; /* NVMe: MSI-X 해제 시 복원할 INTx IRQ 저장. */
	desc->pci.mask_base		= dev->msix_base; /* NVMe: MSI-X table이 매핑된 커널 가상 주소 연결. */


	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY) &&
	    !desc->pci.msi_attrib.is_virtual) { /* NVMe: 레거시 마스킹을 지원하고 가상 entry가 아니면 mask 정보를 읽어옴. */
		void __iomem *addr = pci_msix_desc_addr(desc); /* NVMe: 현재 MSI-X entry의 MMIO 주소. */

		desc->pci.msi_attrib.can_mask = 1; /* NVMe: 소프트웨어 마스킹 가능으로 표시. */
		/* Workaround for SUN NIU insanity, which requires write before read */
		if (dev->dev_flags & PCI_DEV_FLAGS_MSIX_TOUCH_ENTRY_DATA_FIRST) /* NVMe: 특정 buggy 장치는 read 전에 data에 0을 써야 함. */
			writel(0, addr + PCI_MSIX_ENTRY_DATA); /* NVMe: workaround로 data 레지스터에 0 기록. */
		desc->pci.msix_ctrl = readl(addr + PCI_MSIX_ENTRY_VECTOR_CTRL); /* NVMe: 현재 entry의 vector control(마스크 상태) 읽기. */
	}
}

/*
 * msix_setup_msi_descs:
 *   요청한 nvec 개수만큼 MSI-X 디스크립터를 생성하고 장치 리스트에 추가.
 *   NVMe: NVMe 드라이버가 요청한 큐 개수만큼 MSI-X entry를 만들 때 호출.
 *   entries 배열이 주어지면 entry 번호를 매핑하고, masks는 CPU affinity.
 */
static int msix_setup_msi_descs(struct pci_dev *dev, struct msix_entry *entries,
				int nvec, struct irq_affinity_desc *masks)
{
	int ret = 0, i, vec_count = pci_msix_vec_count(dev); /* NVMe: 반환값, 반복 인덱스, 하드웨어 최대 MSI-X entry 수. */
	struct irq_affinity_desc *curmsk; /* NVMe: 현재 entry에 할당할 affinity mask 포인터. */
	struct msi_desc desc; /* NVMe: 반복적으로 초기화하여 삽입할 임시 디스크립터. */

	memset(&desc, 0, sizeof(desc)); /* NVMe: 임시 디스크립터 0으로 초기화. */

	for (i = 0, curmsk = masks; i < nvec; i++, curmsk++) { /* NVMe: 요청한 nvec 개수만큼 entry 생성. */
		desc.msi_index = entries ? entries[i].entry : i; /* NVMe: caller가 지정한 entry 번호가 있으면 사용, 없으면 순차 할당. */
		desc.affinity = masks ? curmsk : NULL; /* NVMe: affinity mask가 있으면 연결. */
		desc.pci.msi_attrib.is_virtual = desc.msi_index >= vec_count; /* NVMe: 하드웨어 table 크기를 초과하면 가상 entry로 표시. */

		msix_prepare_msi_desc(dev, &desc); /* NVMe: MSI-X entry 디스크립터 필드 채우기. */

		ret = msi_insert_msi_desc(&dev->dev, &desc); /* NVMe: 준비된 디스크립터를 장치 리스트에 삽입. */
		if (ret) /* NVMe: 삽입 실패 시. */
			break; /* NVMe: 루프 중단. */
	}
	return ret; /* NVMe: 성공(0) 또는 오류 코드 반환. */
}

/*
 * msix_update_entries:
 *   MSI-X 초기화 후 caller가 전달한 msix_entry 배열에 할당된 Linux virq를
 *   기록한다.
 *   NVMe: NVMe 드라이버가 pci_enable_msix_range() 등을 호출할 때 전달한
 *   entries[].vector에 실제 할당된 virq를 채워줌.
 */
static void msix_update_entries(struct pci_dev *dev, struct msix_entry *entries)
{
	struct msi_desc *desc; /* NVMe: 순회할 MSI-X 디스크립터. */

	if (entries) { /* NVMe: caller가 entry 배열을 제공했을 때만. */
		msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL) { /* NVMe: 모든 MSI-X 디스크립터 순회. */
			entries->vector = desc->irq; /* NVMe: 할당된 Linux virq를 caller 배열에 기록. */
			entries++; /* NVMe: 다음 entry로 이동. */
		}
	}
}

/*
 * msix_mask_all:
 *   MSI-X 테이블의 모든 entry를 마스크한다.
 *   NVMe: NVMe MSI-X 초기화나 kdump crash kernel 진입 시 stale entry가
 *   인터럽트를 발생시키지 않도록 전체 entry를 일괄 마스크.
 */
static void msix_mask_all(void __iomem *base, int tsize)
{
	u32 ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT; /* NVMe: 마스크 비트만 1로 설정한 값. */
	int i; /* NVMe: 테이블 entry 인덱스. */

	for (i = 0; i < tsize; i++, base += PCI_MSIX_ENTRY_SIZE) /* NVMe: 테이블 크기만큼 모든 entry 순회. */
		writel(ctrl, base + PCI_MSIX_ENTRY_VECTOR_CTRL); /* NVMe: 각 entry의 vector control에 mask 비트 쓰기. */
}

DEFINE_FREE(free_msi_irqs, struct pci_dev *, if (_T) pci_free_msi_irqs(_T)); /* NVMe: __free 스코프드 해제 매크로 정의. */

/*
 * __msix_setup_interrupts:
 *   MSI-X 디스크립터 생성, IRQ 할당, entry 검증, caller 배열 갱신을 한
 *   번에 수행한다.
 *   NVMe: NVMe가 pci_alloc_irq_vectors_affinity(... PCI_IRQ_MSIX ...)로
 *   요청한 벡터들을 실제로 확보하는 낮은 수준 함수.
 */
static int __msix_setup_interrupts(struct pci_dev *__dev, struct msix_entry *entries,
				   int nvec, struct irq_affinity_desc *masks)
{
	struct pci_dev *dev __free(free_msi_irqs) = __dev; /* NVMe: 오류 시 자동으로 pci_free_msi_irqs() 호출되도록 스코프드 변수. */

	int ret = msix_setup_msi_descs(dev, entries, nvec, masks); /* NVMe: nvec개의 MSI-X 디스크립터 생성. */
	if (ret) /* NVMe: 디스크립터 생성 실패 시. */
		return ret; /* NVMe: 오류 반환(이때 free_msi_irqs 자동 호출). */

	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSIX); /* NVMe: IRQ 도메인에 nvec개의 MSI-X IRQ 할당 요청. */
	if (ret) /* NVMe: IRQ 할당 실패 시. */
		return ret; /* NVMe: 오류 반환. */

	/* Check if all MSI entries honor device restrictions */
	ret = msi_verify_entries(dev); /* NVMe: 할당된 message 주소가 장치 제약을 만족하는지 검증. */
	if (ret) /* NVMe: 검증 실패 시. */
		return ret; /* NVMe: 오류 반환. */

	msix_update_entries(dev, entries); /* NVMe: caller의 msix_entry 배열에 virq 기록. */
	retain_and_null_ptr(dev); /* NVMe: 자동 해제 방지(성공이므로 dev 포인터 보존). */
	return 0; /* NVMe: MSI-X 설정 성공. */
}

/*
 * msix_setup_interrupts:
 *   MSI-X 인터럽트 설정 루틴을 락 안에서 호출한다.
 *   NVMe: msi_descs_lock을 획득하여 NVMe 장치의 MSI-X 리스트를 보호한
 *   상태에서 실제 설정 수행.
 */
static int msix_setup_interrupts(struct pci_dev *dev, struct msix_entry *entries,
				 int nvec, struct irq_affinity *affd)
{
	struct irq_affinity_desc *masks __free(kfree) =
		affd ? irq_create_affinity_masks(nvec, affd) : NULL; /* NVMe: affinity 정책에 따라 nvec개의 CPU 마스크 생성. */

	guard(msi_descs_lock)(&dev->dev); /* NVMe: msi_desc 리스트 보호 락 획득. */
	return __msix_setup_interrupts(dev, entries, nvec, masks); /* NVMe: 실제 MSI-X 인터럽트 설정. */
}

/**
 * msix_capability_init - configure device's MSI-X capability
 * @dev: pointer to the pci_dev data structure of MSI-X device function
 * @entries: pointer to an array of struct msix_entry entries
 * @nvec: number of @entries
 * @affd: Optional pointer to enable automatic affinity assignment
 *
 * Setup the MSI-X capability structure of device function with a
 * single MSI-X IRQ. A return of zero indicates the successful setup of
 * requested MSI-X entries with allocated IRQs or non-zero for otherwise.
 *
 * NVMe: NVMe 드라이버가 MSI-X를 사용할 때 호출되는 핵심 함수. MSI-X
 * enable, 테이블 매핑, 벡터 할당, affinity 마스크 적용, INTX 비활성화를
 * 수행. NVMe의 per-queue 인터럽트 분산 성능은 이 함수의 결과에 직결됨.
 **/
static int msix_capability_init(struct pci_dev *dev, struct msix_entry *entries,
				int nvec, struct irq_affinity *affd)
{
	int ret, tsize; /* NVMe: 반환값과 MSI-X 테이블 크기. */
	u16 control; /* NVMe: MSI-X control 레지스터 값. */

	/*
	 * Some devices require MSI-X to be enabled before the MSI-X
	 * registers can be accessed.  Mask all the vectors to prevent
	 * interrupts coming in before they're fully set up.
	 */
	pci_msix_clear_and_set_ctrl(dev, 0, PCI_MSIX_FLAGS_MASKALL |
				    PCI_MSIX_FLAGS_ENABLE); /* NVMe: MSI-X enable과 전체 mask(Function Mask)를 동시에 설정. */

	/* Mark it enabled so setup functions can query it */
	dev->msix_enabled = 1; /* NVMe: 소프트웨어 상태에서 MSI-X 활성화 표시. */

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control); /* NVMe: MSI-X control에서 table size 읽기. */
	/* Request & Map MSI-X table region */
	tsize = msix_table_size(control); /* NVMe: control의 table size 필드로 entry 개수 산출. */
	dev->msix_base = msix_map_region(dev, tsize); /* NVMe: MSI-X table이 있는 BAR 영역을 ioremap. */
	if (!dev->msix_base) { /* NVMe: 매핑 실패 시. */
		ret = -ENOMEM; /* NVMe: 메모리 부족 오류. */
		goto out_disable; /* NVMe: MSI-X 비활성화 경로로 이동. */
	}

	ret = msix_setup_interrupts(dev, entries, nvec, affd); /* NVMe: 디스크립터 생성, IRQ 할당, entry 갱신 수행. */
	if (ret) /* NVMe: 설정 실패 시. */
		goto out_unmap; /* NVMe: 매핑 해제 및 MSI-X 비활성화 경로로 이동. */

	/* Disable INTX */
	pci_intx_for_msi(dev, 0); /* NVMe: MSI-X를 사용하므로 INTx 인터럽트 비활성화. */

	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY)) {
		/*
		 * Ensure that all table entries are masked to prevent
		 * stale entries from firing in a crash kernel.
		 *
		 * Done late to deal with a broken Marvell NVME device
		 * which takes the MSI-X mask bits into account even
		 * when MSI-X is disabled, which prevents MSI delivery.
		 */
		msix_mask_all(dev->msix_base, tsize); /* NVMe: 레거시 마스킹 지원 시 모든 entry를 마스크하여 초기 상태 유지. */
	}
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0); /* NVMe: Function Mask(Maskall)는 해제하여 개별 entry 마스크만 적용. */

	pcibios_free_irq(dev); /* NVMe: INTx IRQ 자원 반납. */
	return 0; /* NVMe: MSI-X 초기화 성공. */

out_unmap:
	iounmap(dev->msix_base); /* NVMe: 매핑한 MSI-X table 영역 해제. */
out_disable:
	dev->msix_enabled = 0; /* NVMe: 소프트웨어 상태에서 MSI-X 비활성화. */
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE, 0); /* NVMe: Function Mask와 Enable 비트 모두 클리어. */

	return ret; /* NVMe: 오류 코드 반환. */
}

/*
 * pci_msix_validate_entries:
 *   caller가 제공한 MSI-X entry 번호 배열의 유효성을 검사한다.
 *   NVMe: NVMe 드라이버가 특정 entry 번호를 지정하여 MSI-X를 요청할 때
 *   중복이나 허용되지 않는 gap이 있는지 확인.
 */
static bool pci_msix_validate_entries(struct pci_dev *dev, struct msix_entry *entries, int nvec)
{
	bool nogap; /* NVMe: 연속적인 entry 번호만 허용하는지 여부. */
	int i, j; /* NVMe: 중첩 루프 인덱스. */

	if (!entries) /* NVMe: entry 배열이 NULL이면(번호 지정 없이 순차 할당) 유효성 검사 불필요. */
		return true; /* NVMe: 유효함. */

	nogap = pci_msi_domain_supports(dev, MSI_FLAG_MSIX_CONTIGUOUS, DENY_LEGACY); /* NVMe: IRQ 도메인이 연속 entry만 지원하는지 확인. */

	for (i = 0; i < nvec; i++) { /* NVMe: 모든 요청 entry를 순회. */
		/* Check for duplicate entries */
		for (j = i + 1; j < nvec; j++) { /* NVMe: 동일한 entry 번호가 두 번 지정됐는지 검사. */
			if (entries[i].entry == entries[j].entry) /* NVMe: 중복 발견. */
				return false; /* NVMe: 유효하지 않음. */
		}
		/* Check for unsupported gaps */
		if (nogap && entries[i].entry != i) /* NVMe: 연속 할당이 강제되는데 번호가 i가 아니면. */
			return false; /* NVMe: gap 불허. */
	}
	return true; /* NVMe: 모든 검사 통과. */
}

/*
 * __pci_enable_msix_range:
 *   NVMe 드라이버가 요청한 [minvec, maxvec] 범위에서 MSI-X 벡터를
 *   확보한다. PCI_IRQ_MSIX 플래그가 포함된 pci_alloc_irq_vectors_affinity()
 *   호출 시 가장 먼저 시도되는 경로.
 *   NVMe: NVMe는 이 함수를 통해 admin queue + I/O queue 수만큼의
 *   독립적인 MSI-X vector를 얻는다. 벡터 수가 큐 수보다 적으면
 *   상위에서 큐당 벡터 공유 또는 폴 큐 사용 등으로 fallback.
 */
int __pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec,
			    int maxvec, struct irq_affinity *affd, int flags)
{
	int hwsize, rc, nvec = maxvec; /* NVMe: 하드웨어 최대 entry 수, 반환값, 현재 시도 벡터 수. */

	if (maxvec < minvec) /* NVMe: 범위 역전. */
		return -ERANGE; /* NVMe: 잘못된 범위. */

	if (dev->msi_enabled) { /* NVMe: 이미 레거시 MSI가 활성화된 상태면. */
		pci_info(dev, "can't enable MSI-X (MSI already enabled)\n"); /* NVMe: MSI와 MSI-X는 동시에 활성화될 수 없음을 알림. */
		return -EINVAL; /* NVMe: 오류 반환. */
	}

	if (WARN_ON_ONCE(dev->msix_enabled)) /* NVMe: 이미 MSI-X가 활성화됐는데 다시 호출되면 경고. */
		return -EINVAL; /* NVMe: 중복 활성화 방지. */

	/* Check MSI-X early on irq domain enabled architectures */
	if (!pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX, ALLOW_LEGACY)) /* NVMe: IRQ 도메인이 MSI-X를 지원하지 않으면. */
		return -ENOTSUPP; /* NVMe: MSI-X 지원 불가. */

	if (!pci_msi_supported(dev, nvec) || dev->current_state != PCI_D0) /* NVMe: MSI/MSI-X 지원 및 장치 상태 검사. */
		return -EINVAL; /* NVMe: 사용 불가. */

	hwsize = pci_msix_vec_count(dev); /* NVMe: 하드웨어가 지원하는 최대 MSI-X entry 수 조회. */
	if (hwsize < 0) /* NVMe: 오류 코드 반환 시. */
		return hwsize; /* NVMe: 오류 그대로 반환. */

	if (!pci_msix_validate_entries(dev, entries, nvec)) /* NVMe: caller가 지정한 entry 번호 유효성 검사. */
		return -EINVAL; /* NVMe: 유효하지 않은 entry 번호. */

	if (hwsize < nvec) { /* NVMe: 하드웨어 최대 entry 수가 요청보다 적으면. */
		/* Keep the IRQ virtual hackery working */
		if (flags & PCI_IRQ_VIRTUAL) /* NVMe: PCI_IRQ_VIRTUAL 플래그가 있으면 가상 entry를 허용하여 요청 개수만큼 확보. */
			hwsize = nvec; /* NVMe: 가상 entry 포함하여 크기 조정. */
		else
			nvec = hwsize; /* NVMe: 아니면 요청 개수를 하드웨어 최대로 제한. */
	}

	if (nvec < minvec) /* NVMe: 조정 후에도 minvec 미만이면. */
		return -ENOSPC; /* NVMe: 최소 요구 불만족. */

	rc = pci_setup_msi_context(dev); /* NVMe: MSI 디바이스 데이터 및 devm release 준비. */
	if (rc) /* NVMe: 준비 실패 시. */
		return rc; /* NVMe: 오류 반환. */

	if (!pci_setup_msix_device_domain(dev, hwsize)) /* NVMe: 장치별 MSI-X irqdomain 설정. */
		return -ENODEV; /* NVMe: 도메인 설정 실패. */

	for (;;) { /* NVMe: 원하는 개수만큼 얻을 때까지 재시도 루프. */
		if (affd) { /* NVMe: affinity 정보가 주어진 경우. */
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd); /* NVMe: affinity 정책에 따라 실제 할당 가능 벡터 수 재계산. */
			if (nvec < minvec) /* NVMe: 재계산 후 minvec 미만이면. */
				return -ENOSPC; /* NVMe: CPU affinity 제약으로 할당 불가. */
		}

		rc = msix_capability_init(dev, entries, nvec, affd); /* NVMe: nvec 개의 MSI-X 벡터 초기화 시도. */
		if (rc == 0) /* NVMe: 성공하면. */
			return nvec; /* NVMe: 확보한 벡터 수 반환. */

		if (rc < 0) /* NVMe: 치명적 오류면. */
			return rc; /* NVMe: 오류 반환. */
		if (rc < minvec) /* NVMe: 가능한 개수가 minvec보다 작으면. */
			return -ENOSPC; /* NVMe: 최소 요구 불만족. */

		nvec = rc; /* NVMe: 상위가 제안한 더 작은 개수로 다시 시도. */
	}
}

/*
 * __pci_restore_msix_state:
 *   MSI-X 상태를 resume/복구 시 복원한다.
 *   NVMe: NVMe 장치가 D3cold, 런타임 절전, AER 복구 등에서 돌아온 후
 *   MSI-X Enable, Function Mask, 각 entry의 address/data/mask를
 *   다시 설정하여 인터럽트 경로를 복구.
 */
void __pci_restore_msix_state(struct pci_dev *dev)
{
	struct msi_desc *entry; /* NVMe: 순회할 MSI-X 디스크립터. */
	bool write_msg; /* NVMe: message를 다시 써야 하는지 여부. */

	if (!dev->msix_enabled) /* NVMe: MSI-X가 활성화된 상태가 아니면. */
		return; /* NVMe: 복원할 것이 없음. */

	/* route the table */
	pci_intx_for_msi(dev, 0); /* NVMe: INTx를 끄고 MSI-X를 사용하도록 설정. */
	pci_msix_clear_and_set_ctrl(dev, 0,
				PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL); /* NVMe: MSI-X Enable과 Function Mask 설정. */

	write_msg = arch_restore_msi_irqs(dev); /* NVMe: 아키텍처가 message 복원을 허용하는지 확인. */

	scoped_guard (msi_descs_lock, &dev->dev) { /* NVMe: msi_desc 리스트 보호 락 획득. */
		msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) { /* NVMe: 모든 MSI-X entry 순회. */
			if (write_msg) /* NVMe: message 복원이 필요하면. */
				__pci_write_msi_msg(entry, &entry->msg); /* NVMe: 저장된 address/data를 MSI-X 테이블에 기록. */
			pci_msix_write_vector_ctrl(entry, entry->pci.msix_ctrl); /* NVMe: 각 entry의 mask/unmask 상태 복원. */
		}
	}

	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0); /* NVMe: Function Mask 해제. */
}

/*
 * pci_msix_shutdown:
 *   MSI-X를 완전히 종료하고 INTx로 복원한다.
 *   NVMe: NVMe 드라이버가 pci_free_irq_vectors()를 호출하거나 장치 제거
 *   시 모든 MSI-X entry를 마스크하고, MSI-X enable을 끄고, INTx를 복원.
 */
void pci_msix_shutdown(struct pci_dev *dev)
{
	struct msi_desc *desc; /* NVMe: 순회용 MSI-X 디스크립터. */

	if (!pci_msi_enable || !dev || !dev->msix_enabled) /* NVMe: 전역 MSI off, 장치 없음, MSI-X 미활성화 시. */
		return; /* NVMe: 할 것 없음. */

	if (pci_dev_is_disconnected(dev)) { /* NVMe: 장치가 PCIe 링크 단절 상태면. */
		dev->msix_enabled = 0; /* NVMe: 소프트웨어 상태만 비활성화. */
		return; /* NVMe: 하드웨어 레지스터는 건드리지 않음. */
	}

	/* Return the device with MSI-X masked as initial states */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL) /* NVMe: 모든 MSI-X entry를 순회. */
		pci_msix_mask(desc); /* NVMe: 각 entry를 마스크하여 초기 상태로 복원. */

	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_ENABLE, 0); /* NVMe: MSI-X Enable 비트 클리어. */
	pci_intx_for_msi(dev, 1); /* NVMe: legacy INTx 인터럽트 다시 활성화. */
	dev->msix_enabled = 0; /* NVMe: 소프트웨어 상태에서 MSI-X 비활성화. */
	pcibios_alloc_irq(dev); /* NVMe: INTx IRQ 재할당. */
}

/* Common interfaces */

/*
 * pci_free_msi_irqs:
 *   MSI/MSI-X IRQ를 해제하고 MSI-X table 매핑을 해제한다.
 *   NVMe: NVMe 드라이버가 pci_free_irq_vectors() 호출 시 낮은 수준에서
 *   실행. MSI-X table ioremap 해제도 함께 수행.
 */
void pci_free_msi_irqs(struct pci_dev *dev)
{
	pci_msi_teardown_msi_irqs(dev); /* NVMe: IRQ 도메인에 할당된 MSI/MSI-X IRQ 해제. */

	if (dev->msix_base) { /* NVMe: MSI-X table이 매핑되어 있으면. */
		iounmap(dev->msix_base); /* NVMe: 매핑한 MMIO 영역 해제. */
		dev->msix_base = NULL; /* NVMe: 포인터를 NULL로 설정하여 이중 해제 방지. */
	}
}

#ifdef CONFIG_PCIE_TPH
/**
 * pci_msix_write_tph_tag - Update the TPH tag for a given MSI-X vector
 * @pdev:	The PCIe device to update
 * @index:	The MSI-X index to update
 * @tag:	The tag to write
 *
 * Returns: 0 on success, error code on failure
 *
 * NVMe: TLP Processing Hints(TPH)는 PCIe 엔드포인트가 메모리 트래픽에
 * 대해 캐시/노드 힌트를 제공하는 기능. NVMe 장치가 TPH를 지원하면
 * 특정 MSI-X vector에 대해 TPH ST Steering Tag를 설정할 수 있다.
 * 이는 NVMe CMB(Controller Memory Buffer)나 P2PDMA 환경에서 메모리
 * 접근 지역성을 최적화하는 데 활용될 수 있다.
 */
int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	struct msi_desc *msi_desc; /* NVMe: 대상 MSI-X 디스크립터. */
	struct irq_desc *irq_desc; /* NVMe: Linux virq에 해당하는 irq_desc. */
	unsigned int virq; /* NVMe: 대상 MSI-X vector의 Linux virq. */

	if (!pdev->msix_enabled) /* NVMe: MSI-X가 활성화되지 않은 NVMe 장치이면. */
		return -ENXIO; /* NVMe: MSI-X 미활성화 오류. */

	virq = msi_get_virq(&pdev->dev, index); /* NVMe: MSI-X index로 Linux virq 조회. */
	if (!virq) /* NVMe: 유효한 virq가 없으면. */
		return -ENXIO; /* NVMe: 존재하지 않는 vector 오류. */

	guard(msi_descs_lock)(&pdev->dev); /* NVMe: msi_desc 리스트 보호 락 획득. */

	/*
	 * This is a horrible hack, but short of implementing a PCI
	 * specific interrupt chip callback and a huge pile of
	 * infrastructure, this is the minor nuisance. It provides the
	 * protection against concurrent operations on this entry and keeps
	 * the control word cache in sync.
	 */
	irq_desc = irq_to_desc(virq); /* NVMe: virq로 irq_desc 구조체 획득. */
	if (!irq_desc) /* NVMe: irq_desc가 없으면. */
		return -ENXIO; /* NVMe: 유효하지 않은 irq 오류. */

	guard(raw_spinlock_irq)(&irq_desc->lock); /* NVMe: 해당 irq의 lock 획득하여 동시 수정 방지. */
	msi_desc = irq_data_get_msi_desc(&irq_desc->irq_data); /* NVMe: irq_data에서 MSI 디스크립터 획득. */
	if (!msi_desc || msi_desc->pci.msi_attrib.is_virtual) /* NVMe: MSI desc 없거나 가상 entry이면. */
		return -ENXIO; /* NVMe: 처리 불가. */

	msi_desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_ST; /* NVMe: 기존 Steering Tag 필드 클리어. */
	msi_desc->pci.msix_ctrl |= FIELD_PREP(PCI_MSIX_ENTRY_CTRL_ST, tag); /* NVMe: 새로운 TPH tag 설정. */
	pci_msix_write_vector_ctrl(msi_desc, msi_desc->pci.msix_ctrl); /* NVMe: 갱신된 control 값을 MSI-X 테이블에 기록. */
	/* Flush the write */
	readl(pci_msix_desc_addr(msi_desc)); /* NVMe: MMIO write가 장치에 도달했음을 보장하는 read flush. */
	return 0; /* NVMe: TPH tag 갱신 성공. */
}
#endif

/* Misc. infrastructure */

/*
 * msi_desc_to_pci_dev:
 *   msi_desc에 연결된 struct device를 struct pci_dev로 변환.
 *   NVMe: MSI-X entry에서 직접 해당 NVMe PCIe 장치를 찾아낼 때 사용.
 */
struct pci_dev *msi_desc_to_pci_dev(struct msi_desc *desc)
{
	return to_pci_dev(desc->dev); /* NVMe: msi_desc->dev를 pci_dev로 캐스팅하여 반환. */
}
EXPORT_SYMBOL(msi_desc_to_pci_dev);

/*
 * pci_no_msi:
 *   전역적으로 MSI를 비활성화한다.
 *   NVMe: 커널 매개변수 "pci=nomsi"가 지정되면 이 함수가 호출되어
 *   NVMe를 포함한 모든 PCI 장치가 INTx만 사용하도록 강제.
 */
void pci_no_msi(void)
{
	pci_msi_enable = false; /* NVMe: 전역 MSI 활성화 플래그를 false로 설정. */
}
