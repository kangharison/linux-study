// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI).
 *
 * Legacy architecture specific setup and teardown mechanism.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/msi/legacy.c)은 IRQ domain이 없는 레거시 아키텍처에서
 * PCI 장치의 MSI/MSI-X 인터럽트를 설정하고 해제하는 폴백(fallback) 경로를
 * 제공한다. NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c) 입장에서
 * MSI-X는 IO 큐당 독립 인터럽트 벡터를 할당해 CQ(Completion Queue) 완료를
 * 효율적으로 처리하는 핵심 메커니즘이다. 일반적인 NVMe MSI-X 설정 흐름:
 *   nvme_probe -> pci_enable_msix_range -> __pci_enable_msix_range ->
 *   pci_msi_setup_msi_irqs -> pci_msi_legacy_setup_msi_irqs (폴백) ->
 *   arch_setup_msi_irqs -> arch_setup_msi_irq
 * NVMe 드라이버는 reset_work에서 admin queue와 IO queue에 필요한 벡터 수를
 * 계산하고, 이 파일을 통해 실제 IRQ 번호와 MSI-X descriptor를 할당받는다.
 * 또한 nvme_reset_work, nvme_remove, nvme_reset_ctrl 등에서 드라이버 언바운드
 * 시 pci_disable_msix / pci_free_irq_vectors를 호출하면 이 파일의 teardown
 * 경로가 따라와 sysfs 정리 및 IRQ 해제를 수행한다.
 * ===================================================================
 */
#include "msi.h" /* PCI/NVMe: PCI MSI 공식 헤더; msi_desc, pci_msi_enable 등 정의 */

/* Arch hooks */
/* PCI/NVMe: 아키텍처별 MSI IRQ 1개 할당 콜백; NVMe MSI-X 벡터 1개당 1회 호출 */
int __weak arch_setup_msi_irq(struct pci_dev *dev, struct msi_desc *desc)
{
	return -EINVAL; /* NVMe: 할당 실패 시 NVMe queue 생성 단계에서 -EINVAL 반환 */
}

/* PCI/NVMe: 아키텍처별 MSI IRQ 1개 해제; NVMe remove/reset 시 벡터 반납 */
void __weak arch_teardown_msi_irq(unsigned int irq)
{
}

/* PCI/NVMe: 아키텍처별 MSI/MSI-X 다중 벡터 설정; NVMe IO queue 수만큼 nvec 전달 */
int __weak arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct msi_desc *desc; /* PCI/NVMe: NVMe 장치에 연결된 MSI descriptor */
	int ret;               /* NVMe: 벡터 할당 결과; 실패 시 queue 생성 중단 */

	/*
	 * If an architecture wants to support multiple MSI, it needs to
	 * override arch_setup_msi_irqs()
	 */
	/* PCI/NVMe: 레거시 MSI는 다중 벡터 미지원; NVMe는 MSI-X를 주로 사용 */
	if (type == PCI_CAP_ID_MSI && nvec > 1)
		return 1; /* NVMe: MSI 다중 벡터 불가 시 MSI-X 경로로 fallback 유도 */

	/* PCI/NVMe: 아직 IRQ에 연결되지 않은 descriptor 순회; NVMe queue당 descriptor 1개 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED) {
		ret = arch_setup_msi_irq(dev, desc); /* NVMe: 각 MSI-X 벡터 IRQ 번호 할당 */
		if (ret) /* PCI/NVMe: 할당 실패; NVMe IO queue 수 줄이거나 -ENOSPC 리턴 */
			return ret < 0 ? ret : -ENOSPC; /* NVMe: 벡터 공간 부족 시 상위로 전파 */
	}

	return 0; /* NVMe: 모든 MSI/MSI-X 벡터 할당 성공; queue 생성 계속 */
}

/* PCI/NVMe: NVMe 장치의 모든 MSI/MSI-X IRQ 해제; remove/reset 때 호출 */
void __weak arch_teardown_msi_irqs(struct pci_dev *dev)
{
	struct msi_desc *desc; /* NVMe: 해제할 MSI descriptor */
	int i;                 /* NVMe: per-vector 해제 루프 인덱스 */

	/* PCI/NVMe: 이미 IRQ에 연결된 descriptor 순회; NVMe queue별 벡터 그룹 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		/* PCI/NVMe: descriptor가 사용 중인 벡터 개수만큼 반복 */
		for (i = 0; i < desc->nvec_used; i++)
			arch_teardown_msi_irq(desc->irq + i); /* NVMe: admin/IO queue 벡터 해제 */
	}
}

/* PCI/NVMe: MSI-X 설정 결과 검사; NVMe queue 수 조정에 필요한 가용 벡터 수 산출 */
static int pci_msi_setup_check_result(struct pci_dev *dev, int type, int ret)
{
	struct msi_desc *desc; /* NVMe: 검사 대상 descriptor */
	int avail = 0;         /* NVMe: 성공적으로 할당된 벡터 수 */

	/* PCI/NVMe: MSI 실패이거나 MS-X 아닌 경우 그대로 반환; NVMe MSI-X 경로만 후속 처리 */
	if (type != PCI_CAP_ID_MSIX || ret >= 0)
		return ret; /* NVMe: 성공 또는 MSI 실패 시 상위로 즉시 전달 */

	/* Scan the MSI descriptors for successfully allocated ones. */
	/* PCI/NVMe: MSI-X 부분 할당 시 NVMe가 요청한 nvec 중 실제 사용 가능한 수 집계 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED)
		avail++; /* NVMe: 성공한 descriptor 1개당 가용 벡터 1개 증가 */

	/* NVMe: 부분 할당이면 avail>0을 반환해 NVMe가 queue 수를 줄일 수 있게 함 */
	return avail ? avail : ret;
}

/* PCI/NVMe: NVMe 장치의 MSI/MSI-X IRQ 할당 + sysfs 등록 레거시 진입점 */
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* NVMe: nvec은 admin queue 1개 + IO queue 개수만큼의 MSI-X 벡터 요청량 */
	int ret = arch_setup_msi_irqs(dev, nvec, type);

	/* NVMe: MSI-X 부분 할당이면 avail 반환, 완전 실패면 음수; queue 수 재계산에 사용 */
	ret = pci_msi_setup_check_result(dev, type, ret);
	if (!ret) /* PCI/NVMe: 전체 성공 시에만 sysfs 노드 생성 */
		ret = msi_device_populate_sysfs(&dev->dev); /* NVMe: /sys/devices/.../msi_irqs 노출 */
	return ret; /* NVMe: 0이면 MSI-X 준비 완료; 양수면 줄일 queue 수; 음수면 실패 */
}

/* PCI/NVMe: NVMe 장치 제거/재설정 시 MSI/MSI-X 해제 레거시 진입점 */
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	/* NVMe: /sys/devices/.../msi_irqs sysfs 항목 제거; 드라이버 언바운드 전 정리 */
	msi_device_destroy_sysfs(&dev->dev);
	/* NVMe: 실제 IRQ 번호 반납; NVMe queue interrupt handler 등록 해제 후 호출 */
	arch_teardown_msi_irqs(dev);
}
