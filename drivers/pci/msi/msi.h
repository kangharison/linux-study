/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/pci.h>		/* PCI/NVMe: struct pci_dev, PCI_MSIX_*, PCIe NVMe BAR/MSI capability 접근 */
#include <linux/msi.h>		/* PCI/NVMe: struct msi_desc, MSI/MSIX 인터럽트 기술자 */

/* PCI/NVMe: Message Control 레지스터의 Table Size 필드(0-based)를 실제 entry 개수로 변환 */
#define msix_table_size(flags)	((flags & PCI_MSIX_FLAGS_QSIZE) + 1)

/* PCI/NVMe: NVMe SSD에 nvec 개의 MSI/MSIX 벡터를 할당; nvme_reset_work() 후 pci_enable_msix_range() 경로로 호출됨 */
int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
/* PCI/NVMe: NVMe 장치 해제 시 pci_disable_msix()에서 호출하여 MSI/MSIX IRQ를 모두 반납 */
void pci_msi_teardown_msi_irqs(struct pci_dev *dev);

/* Mask/unmask helpers */
/* PCI/NVMe: MSI/MSIX 벡터 마스크 레지스터를 원자적으로 갱신; CQ 완료 인터럽트 억제/허용 시 사용 */
void pci_msi_update_mask(struct msi_desc *desc, u32 clear, u32 set);

static inline void pci_msi_mask(struct msi_desc *desc, u32 mask)
{
	/* PCI/NVMe: desc->msi_mask 기준으로 mask 비트만큼 인터럽트를 마스크; NVMe CQ ISR 일시 정지 */
	pci_msi_update_mask(desc, 0, mask);
}

static inline void pci_msi_unmask(struct msi_desc *desc, u32 mask)
{
	/* PCI/NVMe: mask 비트만큼 인터럽트 마스크를 해제; NVMe CQ 완료 알림 다시 활성화 */
	pci_msi_update_mask(desc, mask, 0);
}

static inline void __iomem *pci_msix_desc_addr(struct msi_desc *desc)
{
	/* PCI/NVMe: NVMe SSD의 MSI-X Table 내 특정 entry의 메모리 주소를 산출; BAR0 기반 mapping 영역 참조 */
	return desc->pci.mask_base + desc->msi_index * PCI_MSIX_ENTRY_SIZE;
}

/*
 * This internal function does not flush PCI writes to the device.  All
 * users must ensure that they read from the device before either assuming
 * that the device state is up to date, or returning out of this file.
 * It does not affect the msi_desc::msix_ctrl cache either. Use with care!
 */
static inline void pci_msix_write_vector_ctrl(struct msi_desc *desc, u32 ctrl)
{
	void __iomem *desc_addr = pci_msix_desc_addr(desc);

	/* PCI/NVMe: MaskBit 지원 시에만 Vector Control 레지스터에 쓰기; NVMe MSI-X entry별 마스크 갱신 */
	if (desc->pci.msi_attrib.can_mask)
		writel(ctrl, desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

static inline void pci_msix_mask(struct msi_desc *desc)
{
	/* PCI/NVMe: MSI-X Vector Control의 MaskBit을 1로 설정; 해당 NVMe CQ 인터럽트 차단 */
	desc->pci.msix_ctrl |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
	/* PCI/NVMe: MaskBit을 SSD MSI-X Table에 기록; 후속 readl()로 flush하지 않음(별도 flush 필요) */
	pci_msix_write_vector_ctrl(desc, desc->pci.msix_ctrl);
	/* Flush write to device */
	/* PCI/NVMe: Device에 대한 read로 PCIe write posting를 flush; NVMe 컨트롤러가 mask 상태를 인식하도록 보장 */
	readl(desc->pci.mask_base);
}

static inline void pci_msix_unmask(struct msi_desc *desc)
{
	/* PCI/NVMe: MSI-X Vector Control의 MaskBit을 0으로 클리어; NVMe CQ 인터럽트 다시 허용 */
	desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	/* PCI/NVMe: 클리어한 MaskBit을 SSD MSI-X Table에 기록; 인터럽트 언마스크 완료 */
	pci_msix_write_vector_ctrl(desc, desc->pci.msix_ctrl);
}

static inline void __pci_msi_mask_desc(struct msi_desc *desc, u32 mask)
{
	/* PCI/NVMe: NVMe 컨트롤러가 MSI-X를 사용하는지, 레거시 MSI를 사용하는지 분기 처리 */
	if (desc->pci.msi_attrib.is_msix)
		pci_msix_mask(desc);
	else
		pci_msi_mask(desc, mask);
}

static inline void __pci_msi_unmask_desc(struct msi_desc *desc, u32 mask)
{
	/* PCI/NVMe: MSI-X냐 MSI냐에 따라 NVMe 인터럽트 언마스크 경로를 선택 */
	if (desc->pci.msi_attrib.is_msix)
		pci_msix_unmask(desc);
	else
		pci_msi_unmask(desc, mask);
}

/*
 * PCI 2.3 does not specify mask bits for each MSI interrupt.  Attempting to
 * mask all MSI interrupts by clearing the MSI enable bit does not work
 * reliably as devices without an INTx disable bit will then generate a
 * level IRQ which will never be cleared.
 */
static inline __attribute_const__ u32 msi_multi_mask(struct msi_desc *desc)
{
	/* Don't shift by >= width of type */
	/* PCI/NVMe: MSI Multiple Message Capable이 5(32벡터) 이상이면 전체 32bit 마스크 반환; NVMe 다중 CQ 큐 대응 */
	if (desc->pci.msi_attrib.multi_cap >= 5)
		return 0xffffffff;
	/* PCI/NVMe: multi_cap에 따른 유효 MSI 벡터 개수만큼 하위 비트를 1로 채워 마스크 생성 */
	return (1 << (1 << desc->pci.msi_attrib.multi_cap)) - 1;
}

/* PCI/NVMe: NVMe 장치에 대한 MSI-X descriptor 초기화; BAR mapping/pba offset/table entry 정보 구성 */
void msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc);

/* Subsystem variables */
/* PCI/NVMe: 전역 MSI 사용 가능 여부; NVMe PCIe 호스트도 pci_msi_enable이 false면 레거시 INTx로 폼백 */
extern bool pci_msi_enable;

/* MSI internal functions invoked from the public APIs */
/* PCI/NVMe: pci_disable_msi()의 낮은 수준 처리; NVMe reset/제거 시 MSI 벡터 비활성화 */
void pci_msi_shutdown(struct pci_dev *dev);
/* PCI/NVMe: pci_disable_msix()의 낮은 수준 처리; NVMe reset/제거 시 MSI-X 벡터 비활성화 */
void pci_msix_shutdown(struct pci_dev *dev);
/* PCI/NVMe: NVMe 장치의 모든 msi_desc 및 할당된 IRQ 번호를 해제; nvme_remove() 경로에서 활용 */
void pci_free_msi_irqs(struct pci_dev *dev);
/* PCI/NVMe: NVMe 호스트가 요청한 [minvec, maxvec] 범위의 MSI 벡터를 실제로 enable; nvme_setup_irqs() 연결 */
int __pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec, struct irq_affinity *affd);
/* PCI/NVMe: NVMe가 선호하는 entries[] 기반 MSI-X 벡터 범위를 enable; cq_count만큼의 큐 대응 */
int __pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec,
			    int maxvec,  struct irq_affinity *affd, int flags);
/* PCI/NVMe: NVMe 컨트롤러 resume/재초기화 시 MSI 레지스터와 msi_desc 상태를 복원 */
void __pci_restore_msi_state(struct pci_dev *dev);
/* PCI/NVMe: NVMe 컨트롤러 resume/재초기화 시 MSI-X 레지스터와 descriptor 상태를 복원 */
void __pci_restore_msix_state(struct pci_dev *dev);

/* irq_domain related functionality */

enum support_mode {
	ALLOW_LEGACY,	/* PCI/NVMe: IRQ domain가 레거시 호환을 허용할 때 사용; INTx 폼백 가능 */
	DENY_LEGACY,	/* PCI/NVMe: 레거시 INTx 호환을 거부; NVMe는 MSI/MSIX 강제 사용 */
};

/* PCI/NVMe: NVMe SSD가 요구하는 feature(Multiple/MSIX)를 현재 irq_domain가 지원하는지 확인 */
bool pci_msi_domain_supports(struct pci_dev *dev, unsigned int feature_mask, enum support_mode mode);
/* PCI/NVMe: MSI IRQ domain을 NVMe pci_dev에 연결; 벡터 할당 전 device domain 설정 */
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize);
/* PCI/NVMe: MSI-X IRQ domain을 NVMe pci_dev에 연결; NVMe 다중 큐 인터럽트를 위한 domain 설정 */
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize);

/* Legacy (!IRQDOMAIN) fallbacks */

#ifdef CONFIG_PCI_MSI_ARCH_FALLBACKS
/* PCI/NVMe: IRQ domain 미지원 아키텍처에서 NVMe용 MSI/MSIX IRQ를 직접 setup */
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
/* PCI/NVMe: IRQ domain 미지원 아키텍처에서 NVMe 장치의 MSI/MSIX IRQ를 해제 */
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev);
#else
static inline int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* PCI/NVMe: LEGACY fallback 설정 없이 호출되면 경고; NVMe IRQ 할당 실패 처리 */
	WARN_ON_ONCE(1);
	return -ENODEV;
}

static inline void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	/* PCI/NVMe: LEGACY fallback 설정 없이 teardown 호출 시 경고; NVMe 종료 경로 예외 상황 */
	WARN_ON_ONCE(1);
}
#endif
