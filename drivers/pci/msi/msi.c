// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI)
 *
 * Copyright (C) 2003-2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 * Copyright (C) 2016 Christoph Hellwig.
 */

/*
 * [한국어 설명] MSI/MSI-X 하드웨어를 실제로 조작하는 구현부 (msi.c)
 *
 * === 파일의 역할 ===
 * api.c 가 결정한 것을 실제 하드웨어에 반영하는 곳이다. config space 의 MSI /
 * MSI-X capability 구조를 파싱하고, MSI-X 테이블이 놓인 BAR 영역을 ioremap 하고,
 * 벡터마다 msi_desc 를 만들어 커널 MSI 계층에 등록하고, 마지막으로 Message
 * Control 레지스터의 Enable 비트를 켠다. 해제는 그 역순이다.
 *
 * 이 파일을 읽으려면 MSI 와 MSI-X 가 하드웨어적으로 얼마나 다른지를 알아야 한다.
 *
 *   MSI  - 모든 정보가 config space 안의 capability 구조에 들어 있다.
 *          Message Address(32 또는 64비트) 하나와 Message Data 하나가 전부다.
 *          벡터가 여러 개여도 주소는 하나를 공유하고, 데이터의 하위 몇 비트만
 *          벡터 번호로 바뀐다. 그래서 벡터 수가 2의 거듭제곱이어야 하고,
 *          번호도 연속이어야 하며, 최대 32개다. 벡터별 마스킹은 선택 기능이라
 *          없는 장치도 많다.
 *
 *   MSI-X - capability 구조에는 "테이블이 어느 BAR 의 어느 오프셋에 있는가"
 *          (Table Offset/BIR)만 적혀 있고, 실제 벡터 정보는 그 BAR 안의
 *          MSI-X Table 에 있다. 항목 하나가 16바이트(주소 하위 4 + 상위 4 +
 *          데이터 4 + 벡터 제어 4)이고, 최대 2048개다. 항목마다 주소가 따로라
 *          벡터별로 다른 CPU 를 지정할 수 있고, Vector Control 의 0번 비트로
 *          벡터별 마스킹이 항상 가능하다.
 *
 * 이 차이 때문에 이 파일의 코드가 msi_ 계열과 msix_ 계열 두 갈래로 크게 나뉜다.
 * MSI-X 쪽이 BAR 매핑(msix_map_region)이라는 추가 단계를 갖는 것이 핵심 차이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * api.c (드라이버가 부르는 얼굴)
 *   -> [이 파일] __pci_enable_msi_range() / __pci_enable_msix_range()
 *      -> msi_capability_init() / msix_capability_init()
 *         -> msix_map_region()          : MSI-X 테이블이 있는 BAR 영역을 ioremap
 *         -> msix_setup_msi_descs()     : 벡터마다 msi_desc 를 만들어 등록
 *         -> pci_msi_setup_msi_irqs()   : irqdomain.c 로 넘겨 실제 IRQ 번호 확보
 *            -> 아키텍처 IRQ 컨트롤러가 주소/데이터를 정해 돌려준다
 *         -> msix_update_entries()      : 정해진 주소/데이터를 MSI-X 테이블에 기록
 *         -> pci_msix_clear_and_set_ctrl(): Enable 비트를 켜고 Function Mask 를 푼다
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(뮤텍스와 메모리 할당이 있다).
 * 예외적으로 pci_msi_mask_irq()/pci_msi_unmask_irq() 와 __pci_write_msi_msg()
 * 계열은 인터럽트 처리 도중 IRQ 코어가 부를 수 있어 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: api.c (공개 API), pcidev_msi.c (장치 발견 시 초기 비활성화).
 * 아래쪽: irqdomain.c (irq_domain 에 벡터 등록), ../pci.h 의 config 접근 함수,
 *   그리고 커널 공통 MSI 계층(kernel/irq/msi.c)의 msi_desc 저장소.
 * 공유 상태:
 *   - struct pci_dev 의 msi_cap / msix_cap (capability 오프셋),
 *     msi_enabled / msix_enabled (현재 상태), msi_irq_groups (sysfs).
 *   - struct msi_desc 의 pci.mask_base (MSI-X 테이블의 가상 주소),
 *     pci.msi_mask / pci.msix_ctrl (마스크 레지스터의 소프트웨어 캐시).
 *     캐시를 두는 이유는 MSI-X 테이블 읽기가 느리고, 마스킹이 인터럽트
 *     처리 경로에서 일어나기 때문이다.
 * 데이터 흐름: capability 레지스터 -> msi_desc -> irqdomain -> 아키텍처가 정한
 *   message address/data -> 다시 하드웨어(MSI capability 또는 MSI-X 테이블).
 *   즉 정보가 하드웨어에서 올라갔다가 다시 내려온다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다. 전부 api.c 를 거친다.
 * 주석을 제거한 drivers/nvme/ 검색으로 확인한 실제 호출과 그 아래 경로다.
 *
 *   nvme_pci_enable()
 *     -> pci_alloc_irq_vectors(pdev, 1, 1, flags)      [api.c]
 *        -> [이 파일] __pci_enable_msix_range(...)
 *           -> msix_capability_init()  : 테이블 매핑 + 벡터 1개 등록 + Enable
 *      admin 큐 하나만 돌리기 위한 최소 구성이다.
 *
 *   nvme_setup_io_queues()
 *     -> pci_free_irq_vectors(pdev)                    [api.c]
 *        -> [이 파일] pci_msix_shutdown() -> pci_free_msi_irqs()
 *     -> nvme_setup_irqs() -> pci_alloc_irq_vectors_affinity(...)
 *        -> [이 파일] __pci_enable_msix_range(...) 를 다시
 *      벡터 수를 늘리려면 껐다 켜는 수밖에 없다. MSI-X Enable 상태에서
 *      테이블 크기를 바꿀 방법이 하드웨어에 없기 때문이다.
 *
 *   전원 복귀(D3cold -> D0) 시
 *     pci_restore_state() -> pci_restore_msi_state()   [api.c]
 *       -> [이 파일] __pci_restore_msix_state()
 *      MSI-X 테이블은 장치 안의 메모리라 전원이 끊기면 내용이 날아간다.
 *      그래서 커널이 msi_desc 에 캐시해 둔 주소/데이터를 다시 써 넣는다.
 *      NVMe 드라이버 코드에는 이 호출이 없다 - PCI 코어가 대신 한다.
 *
 * NVMe 가 요청하는 벡터 수: "admin 1개 + (I/O 큐 수 - 폴링 큐 수)".
 * 폴링 큐를 빼는 이유는 그 큐가 인터럽트 대신 blk-mq 의 poll 경로로
 * 완료를 확인하기 때문이다. NVME_QUIRK_SINGLE_VECTOR 가 걸린 Apple
 * 컨트롤러는 모든 큐가 0번 벡터를 공유해야 해서 1개만 요청한다.
 *
 * 핸들러 등록/해제는 이 파일이 아니라 drivers/pci/irq.c 가 담당한다.
 * NVMe 는 queue_request_irq() 에서 pci_request_irq(pdev, nvmeq->cq_vector, ...) 로
 * 벡터에 nvme_irq / nvme_irq_check 핸들러를 걸고, pci_free_irq() 로 뗀다.
 * 즉 "벡터를 몇 개 확보하느냐"(이 파일)와 "그 벡터에 어떤 함수를 거느냐"
 * (irq.c)가 서로 다른 계층이다.
 *
 * (기존 주석에 P2PDMA/CMB/SR-IOV/AER/ATS/ReBAR/DPC 를 나열한 문단이 있었으나
 *  이 파일의 코드와 아무 관계가 없어 삭제했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * msi_capability_init()      : MSI capability 를 읽어 msi_desc 를 만들고 Enable.
 *                              벡터 수를 2의 거듭제곱으로 내림하는 처리가 여기 있다.
 * msix_capability_init()     : MSI-X 테이블을 매핑하고 벡터들을 등록한 뒤 Enable.
 *                              등록 전에 모든 벡터를 마스크해 두는 순서가 중요하다.
 * msix_map_region()          : Table Offset/BIR 을 해석해 테이블이 있는 물리 주소를
 *                              구하고 ioremap 한다. MSI-X 만의 단계다.
 * msix_mask_all()            : 테이블의 모든 항목을 마스크. 켜자마자 엉뚱한
 *                              인터럽트가 오는 것을 막는다.
 * pci_msi_update_mask()      : MSI 의 Mask Bits 레지스터를 갱신(캐시 포함).
 * pci_msix_clear_and_set_ctrl(): Message Control 레지스터의 비트를 read-modify-write.
 * __pci_write_msi_msg()      : 결정된 주소/데이터를 하드웨어에 기록. MSI 면 config
 *                              space 에, MSI-X 면 테이블 항목에 쓴다.
 * __pci_restore_msi_state() / __pci_restore_msix_state() : 전원 복귀 후 복원.
 * pci_intx_for_msi()         : MSI 를 켤 때 INTx 를 끄고, 끌 때 다시 켠다.
 *                              두 방식이 동시에 활성이면 인터럽트가 두 번 온다.
 * pci_msi_enable (전역 bool) : "pci=nomsi" 부팅 인자로 MSI 전체를 끄는 스위치.
 *
 * 기존 커널 영어 주석은 그대로 보존하고, 한국어 설명을 위/옆에 추가한다.
 */
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include "../pci.h"
#include "msi.h"

bool pci_msi_enable = true;

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
	struct pci_bus *bus;

	/* MSI must be globally enabled and supported by the device */
	if (!pci_msi_enable)
		return 0;

	if (!dev || dev->no_msi)
		return 0;

	/*
	 * You can't ask to have 0 or less MSIs configured.
	 *  a) it's stupid ..
	 *  b) the list manipulation code assumes nvec >= 1.
	 */
	if (nvec < 1)
		return 0;

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
	 * advertising the 'msi_domain' property, which results in
	 * the NO_MSI flag when no MSI domain is found for this bridge
	 * at probe time.
	 */
	for (bus = dev->bus; bus; bus = bus->parent)
		if (bus->bus_flags & PCI_BUS_FLAGS_NO_MSI)
			return 0;

	return 1;
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
	struct pci_dev *dev = pcidev;

	dev->is_msi_managed = false;
	pci_free_irq_vectors(dev);
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
	int ret;

	if (!pci_is_managed(dev) || dev->is_msi_managed)
		return 0;

	ret = devm_add_action(&dev->dev, pcim_msi_release, dev);
	if (ret)
		return ret;

	dev->is_msi_managed = true;
	return 0;
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
	int ret = msi_setup_device_data(&dev->dev);

	if (ret)
		return ret;

	return pcim_setup_msi_release(dev);
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
	struct pci_dev *dev = msi_desc_to_pci_dev(desc);
	raw_spinlock_t *lock = &dev->msi_lock;
	unsigned long flags;

	if (!desc->pci.msi_attrib.can_mask)
		return;

	raw_spin_lock_irqsave(lock, flags);
	desc->pci.msi_mask &= ~clear;
	desc->pci.msi_mask |= set;
	pci_write_config_dword(dev, desc->pci.mask_pos, desc->pci.msi_mask);
	raw_spin_unlock_irqrestore(lock, flags);
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
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	__pci_msi_mask_desc(desc, BIT(data->irq - desc->irq));
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
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	__pci_msi_unmask_desc(desc, BIT(data->irq - desc->irq));
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
	struct pci_dev *dev = msi_desc_to_pci_dev(entry);

	BUG_ON(dev->current_state != PCI_D0);

	if (entry->pci.msi_attrib.is_msix) {
		void __iomem *base = pci_msix_desc_addr(entry);

		if (WARN_ON_ONCE(entry->pci.msi_attrib.is_virtual))
			return;

		msg->address_lo = readl(base + PCI_MSIX_ENTRY_LOWER_ADDR);
		msg->address_hi = readl(base + PCI_MSIX_ENTRY_UPPER_ADDR);
		msg->data = readl(base + PCI_MSIX_ENTRY_DATA);
	} else {
		int pos = dev->msi_cap;
		u16 data;

		pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_LO,
				      &msg->address_lo);
		if (entry->pci.msi_attrib.is_64) {
			pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,
					      &msg->address_hi);
			pci_read_config_word(dev, pos + PCI_MSI_DATA_64, &data);
		} else {
			msg->address_hi = 0;
			pci_read_config_word(dev, pos + PCI_MSI_DATA_32, &data);
		}
		msg->data = data;
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
	int pos = dev->msi_cap;
	u16 msgctl;

	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
	msgctl &= ~PCI_MSI_FLAGS_QSIZE;
	msgctl |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, desc->pci.msi_attrib.multiple);
	pci_write_config_word(dev, pos + PCI_MSI_FLAGS, msgctl);

	pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_LO, msg->address_lo);
	if (desc->pci.msi_attrib.is_64) {
		pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,  msg->address_hi);
		pci_write_config_word(dev, pos + PCI_MSI_DATA_64, msg->data);
	} else {
		pci_write_config_word(dev, pos + PCI_MSI_DATA_32, msg->data);
	}
	/* Ensure that the writes are visible in the device */
	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
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
	void __iomem *base = pci_msix_desc_addr(desc);
	u32 ctrl = desc->pci.msix_ctrl;
	bool unmasked = !(ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);

	if (desc->pci.msi_attrib.is_virtual)
		return;
	/*
	 * The specification mandates that the entry is masked
	 * when the message is modified:
	 *
	 * "If software changes the Address or Data value of an
	 * entry while the entry is unmasked, the result is
	 * undefined."
	 */
	if (unmasked)
		pci_msix_write_vector_ctrl(desc, ctrl | PCI_MSIX_ENTRY_CTRL_MASKBIT);

	writel(msg->address_lo, base + PCI_MSIX_ENTRY_LOWER_ADDR);
	writel(msg->address_hi, base + PCI_MSIX_ENTRY_UPPER_ADDR);
	writel(msg->data, base + PCI_MSIX_ENTRY_DATA);

	if (unmasked)
		pci_msix_write_vector_ctrl(desc, ctrl);

	/* Ensure that the writes are visible in the device */
	readl(base + PCI_MSIX_ENTRY_DATA);
}

/*
 * __pci_write_msi_msg:
 *   MSI/MSI-X message를 실제 하드웨어에 쓰고, 필요시 콜백을 호출한다.
 *   NVMe: NVMe 장치가 D0 상태이고 연결되어 있을 때에만 레지스터에 기록.
 *   전원 상태가 D3 등이면 하드웨어를 건드리지 않고 소프트웨어 캐시만 갱신.
 */
void __pci_write_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	struct pci_dev *dev = msi_desc_to_pci_dev(entry);

	if (dev->current_state != PCI_D0 || pci_dev_is_disconnected(dev)) {
		/* Don't touch the hardware now */
	} else if (entry->pci.msi_attrib.is_msix) {
		pci_write_msg_msix(entry, msg);
	} else {
		pci_write_msg_msi(dev, entry, msg);
	}

	entry->msg = *msg;

	if (entry->write_msi_msg)
		entry->write_msi_msg(entry, entry->write_msi_msg_data);
}

/*
 * pci_write_msi_msg:
 *   Linux virq 번호로 MSI 디스크립터를 찾아 message를 쓴다.
 *   NVMe: irqdomain 레벨에서 virq에 해당하는 NVMe MSI message를
 *   변경할 때 사용되는 외부 인터페이스.
 */
void pci_write_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	struct msi_desc *entry = irq_get_msi_desc(irq);

	__pci_write_msi_msg(entry, msg);
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
	if (!(dev->dev_flags & PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG))
		pci_intx(dev, enable);
}

/*
 * pci_msi_set_enable:
 *   MSI capability의 MSI Enable 비트를 on/off 한다.
 *   NVMe: NVMe의 MSI 활성화/비활성화를 직접 제어. MSI-X와 달리 단일
 *   enable 비트로 모든 MSI vector를 한 번에 켜고 끈다.
 */
static void pci_msi_set_enable(struct pci_dev *dev, int enable)
{
	u16 control;

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	control &= ~PCI_MSI_FLAGS_ENABLE;
	if (enable)
		control |= PCI_MSI_FLAGS_ENABLE;
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control);
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
	struct msi_desc desc;
	u16 control;

	/* MSI Entry Initialization */
	memset(&desc, 0, sizeof(desc));

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	/* Lies, damned lies, and MSIs */
	if (dev->dev_flags & PCI_DEV_FLAGS_HAS_MSI_MASKING)
		control |= PCI_MSI_FLAGS_MASKBIT;
	if (pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY))
		control &= ~PCI_MSI_FLAGS_MASKBIT;

	desc.nvec_used			= nvec;
	desc.pci.msi_attrib.is_64	= !!(control & PCI_MSI_FLAGS_64BIT);
	desc.pci.msi_attrib.can_mask	= !!(control & PCI_MSI_FLAGS_MASKBIT);
	desc.pci.msi_attrib.default_irq	= dev->irq;
	desc.pci.msi_attrib.multi_cap	= FIELD_GET(PCI_MSI_FLAGS_QMASK, control);
	desc.pci.msi_attrib.multiple	= ilog2(__roundup_pow_of_two(nvec));
	desc.affinity			= masks;

	if (control & PCI_MSI_FLAGS_64BIT)
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_64;
	else
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_32;

	/* Save the initial mask status */
	if (desc.pci.msi_attrib.can_mask)
		pci_read_config_dword(dev, desc.pci.mask_pos, &desc.pci.msi_mask);

	return msi_insert_msi_desc(&dev->dev, &desc);
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
	struct msi_desc *entry;
	u64 address;

	if (dev->msi_addr_mask == DMA_BIT_MASK(64))
		return 0;

	msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) {
		address = (u64)entry->msg.address_hi << 32 | entry->msg.address_lo;
		if (address & ~dev->msi_addr_mask) {
			pci_err(dev, "arch assigned 64-bit MSI address %#llx above device MSI address mask %#llx\n",
				address, dev->msi_addr_mask);
			break;
		}
	}
	return !entry ? 0 : -EIO;
}

/*
 * __msi_capability_init:
 *   레거시 MSI capability를 실제로 초기화하고 IRQ 벡터를 할당한다.
 *   NVMe: NVMe 드라이버가 MSI 모드를 선택했을 때 호출되며, IRQ 도메인에서
 *   nvec 개수만큼의 가상 IRQ를 할당받아 config space에 기록한다.
 */
static int __msi_capability_init(struct pci_dev *dev, int nvec, struct irq_affinity_desc *masks)
{
	int ret = msi_setup_msi_desc(dev, nvec, masks);
	struct msi_desc *entry, desc;

	if (ret)
		return ret;

	/* All MSIs are unmasked by default; mask them all */
	entry = msi_first_desc(&dev->dev, MSI_DESC_ALL);
	pci_msi_mask(entry, msi_multi_mask(entry));
	/*
	 * Copy the MSI descriptor for the error path because
	 * pci_msi_setup_msi_irqs() will free it for the hierarchical
	 * interrupt domain case.
	 */
	memcpy(&desc, entry, sizeof(desc));

	/* Configure MSI capability structure */
	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSI);
	if (ret)
		goto err;

	ret = msi_verify_entries(dev);
	if (ret)
		goto err;

	/* Set MSI enabled bits	*/
	dev->msi_enabled = 1;
	pci_intx_for_msi(dev, 0);
	pci_msi_set_enable(dev, 1);

	pcibios_free_irq(dev);
	dev->irq = entry->irq;
	return 0;
err:
	pci_msi_unmask(&desc, msi_multi_mask(&desc));
	pci_free_msi_irqs(dev);
	return ret;
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
	if (nvec > 1 && !pci_msi_domain_supports(dev, MSI_FLAG_MULTI_PCI_MSI, ALLOW_LEGACY))
		return 1;

	/*
	 * Disable MSI during setup in the hardware, but mark it enabled
	 * so that setup code can evaluate it.
	 */
	pci_msi_set_enable(dev, 0);

	struct irq_affinity_desc *masks __free(kfree) =
		affd ? irq_create_affinity_masks(nvec, affd) : NULL;

	guard(msi_descs_lock)(&dev->dev);
	return __msi_capability_init(dev, nvec, masks);
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
	int nvec;
	int rc;

	if (!pci_msi_supported(dev, minvec) || dev->current_state != PCI_D0)
		return -EINVAL;

	/* Check whether driver already requested MSI-X IRQs */
	if (dev->msix_enabled) {
		pci_info(dev, "can't enable MSI (MSI-X already enabled)\n");
		return -EINVAL;
	}

	if (maxvec < minvec)
		return -ERANGE;

	if (WARN_ON_ONCE(dev->msi_enabled))
		return -EINVAL;

	/* Test for the availability of MSI support */
	if (!pci_msi_domain_supports(dev, 0, ALLOW_LEGACY))
		return -ENOTSUPP;

	nvec = pci_msi_vec_count(dev);
	if (nvec < 0)
		return nvec;
	if (nvec < minvec)
		return -ENOSPC;

	rc = pci_setup_msi_context(dev);
	if (rc)
		return rc;

	if (!pci_setup_msi_device_domain(dev, nvec))
		return -ENODEV;

	if (nvec > maxvec)
		nvec = maxvec;

	for (;;) {
		if (affd) {
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd);
			if (nvec < minvec)
				return -ENOSPC;
		}

		rc = msi_capability_init(dev, nvec, affd);
		if (rc == 0)
			return nvec;

		if (rc < 0)
			return rc;
		if (rc < minvec)
			return -ENOSPC;

		nvec = rc;
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
	int ret;
	u16 msgctl;

	if (!dev->msi_cap)
		return -EINVAL;

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &msgctl);
	ret = 1 << FIELD_GET(PCI_MSI_FLAGS_QMASK, msgctl);

	return ret;
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
	return true;
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
	struct msi_desc *entry;
	u16 control;

	if (!dev->msi_enabled)
		return;

	entry = irq_get_msi_desc(dev->irq);

	pci_intx_for_msi(dev, 0);
	pci_msi_set_enable(dev, 0);
	if (arch_restore_msi_irqs(dev))
		__pci_write_msi_msg(entry, &entry->msg);

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	pci_msi_update_mask(entry, 0, 0);
	control &= ~PCI_MSI_FLAGS_QSIZE;
	control |= PCI_MSI_FLAGS_ENABLE |
		   FIELD_PREP(PCI_MSI_FLAGS_QSIZE, entry->pci.msi_attrib.multiple);
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control);
}

/*
 * pci_msi_shutdown:
 *   레거시 MSI를 완전히 종료하고 INTx로 복원한다.
 *   NVMe: NVMe 드라이버가 pci_free_irq_vectors()를 호출하거나 장치 제거
 *   시 MSI 리소스를 반납. 이후 dev->irq는 legacy INTx IRQ로 돌아간다.
 */
void pci_msi_shutdown(struct pci_dev *dev)
{
	struct msi_desc *desc;

	if (!pci_msi_enable || !dev || !dev->msi_enabled)
		return;

	pci_msi_set_enable(dev, 0);
	pci_intx_for_msi(dev, 1);
	dev->msi_enabled = 0;

	/* Return the device with MSI unmasked as initial states */
	desc = msi_first_desc(&dev->dev, MSI_DESC_ALL);
	if (!WARN_ON_ONCE(!desc))
		pci_msi_unmask(desc, msi_multi_mask(desc));

	/* Restore dev->irq to its default pin-assertion IRQ */
	dev->irq = desc->pci.msi_attrib.default_irq;
	pcibios_alloc_irq(dev);
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
	u16 ctrl;

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	ctrl &= ~clear;
	ctrl |= set;
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl);
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
	resource_size_t phys_addr;
	u32 table_offset;
	unsigned long flags;
	u8 bir;

	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE,
			      &table_offset);
	bir = (u8)(table_offset & PCI_MSIX_TABLE_BIR);
	flags = pci_resource_flags(dev, bir);
	if (!flags || (flags & IORESOURCE_UNSET))
		return NULL;

	table_offset &= PCI_MSIX_TABLE_OFFSET;
	phys_addr = pci_resource_start(dev, bir) + table_offset;

	return ioremap(phys_addr, nr_entries * PCI_MSIX_ENTRY_SIZE);
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
	desc->nvec_used			= 1;
	desc->pci.msi_attrib.is_msix	= 1;
	desc->pci.msi_attrib.is_64	= 1;
	/* [한국어] MSI/MSI-X 를 해제했을 때 되돌아갈 레거시 INTx IRQ 번호를 보관해 둔다. */
	desc->pci.msi_attrib.default_irq = dev->irq;
	desc->pci.mask_base		= dev->msix_base;


	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY) &&
	    !desc->pci.msi_attrib.is_virtual) {
		void __iomem *addr = pci_msix_desc_addr(desc);

		desc->pci.msi_attrib.can_mask = 1;
		/* Workaround for SUN NIU insanity, which requires write before read */
		if (dev->dev_flags & PCI_DEV_FLAGS_MSIX_TOUCH_ENTRY_DATA_FIRST)
			writel(0, addr + PCI_MSIX_ENTRY_DATA);
		desc->pci.msix_ctrl = readl(addr + PCI_MSIX_ENTRY_VECTOR_CTRL);
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
	int ret = 0, i, vec_count = pci_msix_vec_count(dev);
	struct irq_affinity_desc *curmsk;
	struct msi_desc desc;

	memset(&desc, 0, sizeof(desc));

	for (i = 0, curmsk = masks; i < nvec; i++, curmsk++) {
		desc.msi_index = entries ? entries[i].entry : i;
		desc.affinity = masks ? curmsk : NULL;
		desc.pci.msi_attrib.is_virtual = desc.msi_index >= vec_count;

		msix_prepare_msi_desc(dev, &desc);

		ret = msi_insert_msi_desc(&dev->dev, &desc);
		if (ret)
			break;
	}
	return ret;
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
	struct msi_desc *desc;

	if (entries) {
		msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL) {
			entries->vector = desc->irq;
			entries++;
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
	u32 ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT;
	int i;

	for (i = 0; i < tsize; i++, base += PCI_MSIX_ENTRY_SIZE)
		writel(ctrl, base + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

DEFINE_FREE(free_msi_irqs, struct pci_dev *, if (_T) pci_free_msi_irqs(_T));

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
	struct pci_dev *dev __free(free_msi_irqs) = __dev;

	int ret = msix_setup_msi_descs(dev, entries, nvec, masks);
	if (ret)
		return ret;

	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSIX);
	if (ret)
		return ret;

	/* Check if all MSI entries honor device restrictions */
	ret = msi_verify_entries(dev);
	if (ret)
		return ret;

	msix_update_entries(dev, entries);
	retain_and_null_ptr(dev);
	return 0;
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
		affd ? irq_create_affinity_masks(nvec, affd) : NULL;

	guard(msi_descs_lock)(&dev->dev);
	return __msix_setup_interrupts(dev, entries, nvec, masks);
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
	int ret, tsize;
	u16 control;

	/*
	 * Some devices require MSI-X to be enabled before the MSI-X
	 * registers can be accessed.  Mask all the vectors to prevent
	 * interrupts coming in before they're fully set up.
	 */
	pci_msix_clear_and_set_ctrl(dev, 0, PCI_MSIX_FLAGS_MASKALL |
				    PCI_MSIX_FLAGS_ENABLE);

	/* Mark it enabled so setup functions can query it */
	dev->msix_enabled = 1;

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
	/* Request & Map MSI-X table region */
	tsize = msix_table_size(control);
	dev->msix_base = msix_map_region(dev, tsize);
	if (!dev->msix_base) {
		ret = -ENOMEM;
		goto out_disable;
	}

	ret = msix_setup_interrupts(dev, entries, nvec, affd);
	if (ret)
		goto out_unmap;

	/* Disable INTX */
	pci_intx_for_msi(dev, 0);

	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY)) {
		/*
		 * Ensure that all table entries are masked to prevent
		 * stale entries from firing in a crash kernel.
		 *
		 * Done late to deal with a broken Marvell NVME device
		 * which takes the MSI-X mask bits into account even
		 * when MSI-X is disabled, which prevents MSI delivery.
		 */
		msix_mask_all(dev->msix_base, tsize);
	}
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);

	pcibios_free_irq(dev);
	return 0;

out_unmap:
	iounmap(dev->msix_base);
out_disable:
	dev->msix_enabled = 0;
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE, 0);

	return ret;
}

/*
 * pci_msix_validate_entries:
 *   caller가 제공한 MSI-X entry 번호 배열의 유효성을 검사한다.
 *   NVMe: NVMe 드라이버가 특정 entry 번호를 지정하여 MSI-X를 요청할 때
 *   중복이나 허용되지 않는 gap이 있는지 확인.
 */
static bool pci_msix_validate_entries(struct pci_dev *dev, struct msix_entry *entries, int nvec)
{
	bool nogap;
	int i, j;

	if (!entries)
		return true;

	nogap = pci_msi_domain_supports(dev, MSI_FLAG_MSIX_CONTIGUOUS, DENY_LEGACY);

	for (i = 0; i < nvec; i++) {
		/* Check for duplicate entries */
		for (j = i + 1; j < nvec; j++) {
			if (entries[i].entry == entries[j].entry)
				return false;
		}
		/* Check for unsupported gaps */
		if (nogap && entries[i].entry != i)
			return false;
	}
	return true;
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
	int hwsize, rc, nvec = maxvec;

	if (maxvec < minvec)
		return -ERANGE;

	if (dev->msi_enabled) {
		pci_info(dev, "can't enable MSI-X (MSI already enabled)\n");
		return -EINVAL;
	}

	if (WARN_ON_ONCE(dev->msix_enabled))
		return -EINVAL;

	/* Check MSI-X early on irq domain enabled architectures */
	if (!pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX, ALLOW_LEGACY))
		return -ENOTSUPP;

	if (!pci_msi_supported(dev, nvec) || dev->current_state != PCI_D0)
		return -EINVAL;

	hwsize = pci_msix_vec_count(dev);
	if (hwsize < 0)
		return hwsize;

	if (!pci_msix_validate_entries(dev, entries, nvec))
		return -EINVAL;

	if (hwsize < nvec) {
		/* Keep the IRQ virtual hackery working */
		if (flags & PCI_IRQ_VIRTUAL)
			hwsize = nvec;
		else
			nvec = hwsize;
	}

	if (nvec < minvec)
		return -ENOSPC;

	rc = pci_setup_msi_context(dev);
	if (rc)
		return rc;

	if (!pci_setup_msix_device_domain(dev, hwsize))
		return -ENODEV;

	for (;;) {
		if (affd) {
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd);
			if (nvec < minvec)
				return -ENOSPC;
		}

		rc = msix_capability_init(dev, entries, nvec, affd);
		if (rc == 0)
			return nvec;

		if (rc < 0)
			return rc;
		if (rc < minvec)
			return -ENOSPC;

		nvec = rc;
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
	struct msi_desc *entry;
	bool write_msg;

	if (!dev->msix_enabled)
		return;

	/* route the table */
	pci_intx_for_msi(dev, 0);
	pci_msix_clear_and_set_ctrl(dev, 0,
				PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);

	write_msg = arch_restore_msi_irqs(dev);

	scoped_guard (msi_descs_lock, &dev->dev) {
		msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) {
			if (write_msg)
				__pci_write_msi_msg(entry, &entry->msg);
			pci_msix_write_vector_ctrl(entry, entry->pci.msix_ctrl);
		}
	}

	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);
}

/*
 * pci_msix_shutdown:
 *   MSI-X를 완전히 종료하고 INTx로 복원한다.
 *   NVMe: NVMe 드라이버가 pci_free_irq_vectors()를 호출하거나 장치 제거
 *   시 모든 MSI-X entry를 마스크하고, MSI-X enable을 끄고, INTx를 복원.
 */
void pci_msix_shutdown(struct pci_dev *dev)
{
	struct msi_desc *desc;

	if (!pci_msi_enable || !dev || !dev->msix_enabled)
		return;

	if (pci_dev_is_disconnected(dev)) {
		dev->msix_enabled = 0;
		return;
	}

	/* Return the device with MSI-X masked as initial states */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL)
		pci_msix_mask(desc);

	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_ENABLE, 0);
	pci_intx_for_msi(dev, 1);
	dev->msix_enabled = 0;
	pcibios_alloc_irq(dev);
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
	pci_msi_teardown_msi_irqs(dev);

	if (dev->msix_base) {
		iounmap(dev->msix_base);
		dev->msix_base = NULL;
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
	struct msi_desc *msi_desc;
	struct irq_desc *irq_desc;
	unsigned int virq;

	if (!pdev->msix_enabled)
		return -ENXIO;

	virq = msi_get_virq(&pdev->dev, index);
	if (!virq)
		return -ENXIO;

	guard(msi_descs_lock)(&pdev->dev);

	/*
	 * This is a horrible hack, but short of implementing a PCI
	 * specific interrupt chip callback and a huge pile of
	 * infrastructure, this is the minor nuisance. It provides the
	 * protection against concurrent operations on this entry and keeps
	 * the control word cache in sync.
	 */
	irq_desc = irq_to_desc(virq);
	if (!irq_desc)
		return -ENXIO;

	guard(raw_spinlock_irq)(&irq_desc->lock);
	msi_desc = irq_data_get_msi_desc(&irq_desc->irq_data);
	if (!msi_desc || msi_desc->pci.msi_attrib.is_virtual)
		return -ENXIO;

	msi_desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_ST;
	msi_desc->pci.msix_ctrl |= FIELD_PREP(PCI_MSIX_ENTRY_CTRL_ST, tag);
	pci_msix_write_vector_ctrl(msi_desc, msi_desc->pci.msix_ctrl);
	/* Flush the write */
	readl(pci_msix_desc_addr(msi_desc));
	return 0;
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
	return to_pci_dev(desc->dev);
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
	pci_msi_enable = false;
}
