// SPDX-License-Identifier: GPL-2.0
/*
 * MSI[X} related functions which are available unconditionally.
 */
/*
 * NVMe 관점 요약:
 *  - 이 파일은 PCIe 장치 탐색(probe) 시 무조건 실행되는 MSI/MSI-X 초기화
 *    함수들을 담고 있다. NVMe SSD가 PCIe 버스에서 발견되면 커널의
 *    PCI 서브시스템이 pci_device_add() -> pci_init_capabilities() 경로로
 *    이 파일의 pci_msi_init()과 pci_msix_init()을 호출한다.
 *  - NVMe 장치는 고성능 입출력을 위해 대부분 MSI-X(다중 벡터)를 사용하며,
 *    NVMe 호스트 드라이버(drivers/nvme/host/pci.c)는 나중에
 *    pci_alloc_irq_vectors(), pci_request_irq(), pci_irq_vector() 등을
 *    통해 MSI-X 벡터를 할당하고 각 Completion Queue에 IRQ를 연결한다.
 *  - 부팅 초기나 hotplug/rescan, 그리고 시스템 복귀 시 장치가 이미
 *    MSI/MSI-X Enable 비트를 1로 남겨두고 있으면 "screaming interrupt"
 *    (드라이버가 아직 처리하지 않는 가짜/연속 인터럽트)가 발생할 수 있다.
 *    따라서 이 파일에서 Capability를 읽어와 우선 끄는 것이 안전하다.
 *  - ECAM(Enhanced Configuration Access Mechanism)을 통해 PCI Configuration
 *    Space의 MSI/MSI-X Capability를 접근하며, 이는 NVMe BAR가 매핑되기
 *    전에도 동작해야 하는 전제 조건이다.
 *  - 전원 제어/reset 경로에서도 MSI-X가 남아 있으면 D3cold->D0 복귀 후
 *    의도치 않은 인터럽트가 유발될 수 있으므로, 드라이어 바인딩 전
 *    초기화 단계에서 비활성화하는 것은 NVMe 장치의 안정적인 boot/resume에
 *    직접적으로 기여한다.
 */
#include "../pci.h"		/* NVMe: PCI 서브시스템 낸부 헤더, pci_dev, PCI capability 매크로 등 포함 */

/*
 * Disable the MSI[X] hardware to avoid screaming interrupts during boot.
 * This is the power on reset default so usually this should be a noop.
 */
/* NVMe: 부팅/장치 탐색 시 MSI/MSI-X 하드웨어를 끄는 공통 함수들의 시작 부분 */

/* NVMe: NVMe SSD를 포함한 PCI 장치의 MSI Capability 초기화 함수 */
void pci_msi_init(struct pci_dev *dev)
{
	u16 ctrl;			/* NVMe: MSI Message Control 레지스터(16bit)를 담을 지역 변수 */

	/* NVMe: Configuration Space에서 MSI Capability의 오프셋을 찾아 저장 */
	dev->msi_cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
	if (!dev->msi_cap)		/* NVMe: MSI Capability가 없으면(예: 일부 레거시 장치) */
		return;			/* NVMe: 더 이상 할 일이 없으므로 함수 종료 */

	/* NVMe: MSI Capability의 Message Control 레지스터를 ECAM으로 읽어옴 */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	if (ctrl & PCI_MSI_FLAGS_ENABLE) {	/* NVMe: MSI Enable 비트가 켜져 있다면 */
		/* NVMe: Enable 비트만 클리어하여 MSI 인터럽트를 금지함 */
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS,
				      ctrl & ~PCI_MSI_FLAGS_ENABLE);
	}

	/* NVMe: 64bit 주소 지원 플래그가 설정되어 있지 않은 경우 */
	if (!(ctrl & PCI_MSI_FLAGS_64BIT))
		/* NVMe: DMA 주소를 32bit로 마스크(MSI 메시지 주소도 32bit 제한) */
		dev->msi_addr_mask = DMA_BIT_MASK(32);
}

/* NVMe: NVMe SSD에서 필수적으로 사용하는 MSI-X Capability 초기화 함수 */
void pci_msix_init(struct pci_dev *dev)
{
	u16 ctrl;			/* NVMe: MSI-X Message Control 레지스터(16bit)를 담을 지역 변수 */

	/* NVMe: Configuration Space에서 MSI-X Capability의 오프셋을 찾아 저장 */
	dev->msix_cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
	if (!dev->msix_cap)		/* NVMe: MSI-X Capability가 없으면 */
		return;			/* NVMe: NVMe에서도 MSI-X 미지원 시 MSI 폴스백을 위해 종료 */

	/* NVMe: MSI-X Capability의 Message Control 레지스터를 ECAM으로 읽어옴 */
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	if (ctrl & PCI_MSIX_FLAGS_ENABLE) {	/* NVMe: MSI-X Enable 비트가 켜져 있다면 */
		/* NVMe: Enable 비트만 클리어하여 MSI-X 인터럽트를 금지함 */
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      ctrl & ~PCI_MSIX_FLAGS_ENABLE);
	}
}
