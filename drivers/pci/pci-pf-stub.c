// SPDX-License-Identifier: GPL-2.0
/* pci-pf-stub - simple stub driver for PCI SR-IOV PF device
 *
 * This driver is meant to act as a "whitelist" for devices that provide
 * SR-IOV functionality while at the same time not actually needing a
 * driver of their own.
 */
/* PCI/NVMe: NVMe SSD(특히 클라우드 가속 카드)가 SR-IOV PF로 동작하면서
 *           별도 PF 드라이버가 없을 때, 이 스텁 드라이버가 PF를 점유해
 *           sysfs의 sriov_numvfs 쓰기를 통해 VF를 활성화할 수 있게 한다.
 *           NVMe 호스트 드라이버(drivers/nvme/host/pci.c)가 PF에 바인딩되지
 *           않은 경우에도 게스트/컨테이너가 VF를 통해 NVMe 디바이스에
 *           직접 접근할 수 있도록 하는 다리 역할을 한다.
 */

#include <linux/module.h>	/* NVMe: 모듈 매크로(module_pci_driver, MODULE_LICENSE 등) 정의 */
#include <linux/pci.h>	/* PCI/NVMe: pci_dev, pci_device_id, pci_driver, pci_sriov_configure_simple 등
 *                         NVMe PCIe 호스트 드라이버와 동일한 PCI 코어 헤더 */

/*
 * pci_pf_stub_whitelist - White list of devices to bind pci-pf-stub onto
 *
 * This table provides the list of IDs this driver is supposed to bind
 * onto.  You could think of this as a list of "quirked" devices where we
 * are adding support for SR-IOV here since there are no other drivers
 * that they would be running under.
 */
/* PCI/NVMe: pci_device_id 테이블은 NVMe 호스트 드라이버의 nvme_id_table과
 *           동일한 방식으로 PCI 버스 열거 시 PCI 코어가 매칭 여부를 판단한다.
 *           여기서는 nvme 드라이버가 처리하지 않는 PF들만 등록하여,
 *           NVMe 기능은 없지만 SR-IOV PF 역할만 수행하는 디바이스를
 *           전용 드라이버 없이 관리한다.
 */
static const struct pci_device_id pci_pf_stub_whitelist[] = {
	{ PCI_VDEVICE(AMAZON, 0x0053) },
	/* NVMe: Amazon(0x1d0f) 0x0053 장치; NVMe 디바이스가 아닌 SR-IOV PF로
	 *       노출되어 별도 기능 드라이버 없이 VF 생성만 필요한 케이스.
	 *       NVMe 호스트 드라이버는 이 vendor/device ID에 매칭되지 않으므로
	 *       pci-pf-stub이 PF를 점유해 VF 리소스를 활성화한다.
	 */
	/* required last entry */
	{ 0 }
	/* PCI/NVMe: NVMe 호스트의 id_table과 마찬가지로 {0} 종료 표시;
	 *           pci_match_id() 등 PCI 열수 루프에서 탐색 종료 조건으로 사용됨 */
};
MODULE_DEVICE_TABLE(pci, pci_pf_stub_whitelist);
/* PCI/NVMe: 모듈 로드 시 이 ID 테이블이 modinfo에 노출되어 udev/depmod가
 *           NVMe/SR-IOV PF 디바이스와 이 드라이버를 매칭할 수 있게 한다.
 *           NVMe 호스트 드라이버의 MODULE_DEVICE_TABLE(pci, nvme_id_table)
 *           과 동일한 PCI ID 매칭 메커니즘을 사용한다. */

static int pci_pf_stub_probe(struct pci_dev *dev,
			     const struct pci_device_id *id)
/* PCI/NVMe: pci_driver.probe 콜백; NVMe 호스트 드라이버의 nvme_probe()와
 *           동일한 시그니처이며, PCI 코어가 버스 열거/바인딩 단계에서 호출한다.
 *           이 스텁은 막대한 레지스터 맵, DMA, MSI-X 설정 없이 PF 점유만 수행.
 */
{
	pci_info(dev, "claimed by pci-pf-stub\n");
	/* NVMe: NVMe 호스트 드라이버에서 pci_info/pci_dbg로 상태를 기록하듯,
	 *       PF 점유 사실을 커널 로그에 남긴다. 이 디바이스는 DMA, BAR,
	 *       인터럽트 등을 사용하지 않으므로 추가 초기화가 없다. */
	return 0;
	/* NVMe: probe 성공; PCI 코어는 이 PF를 pci-pf-stub에 바인딩 처리하고
	 *       이후 sysfs의 sriov_numvfs 파일을 통해 VF 활성화 요청을 받을 수
	 *       있게 한다. NVMe 호스트 드라이버의 nvme_probe()가 0을 반환하면
	 *       NVMe 장치에 대한 DMA/MSI-X/BAR 설정이 이어지는 것과 대조된다. */
}

static struct pci_driver pf_stub_driver = {
	.name			= "pci-pf-stub",
	/* NVMe: sysfs /proc/devices 등에 노출되는 드라이버 이름;
	 *       NVMe 호스트 드라이버의 .name = "nvme"와 유사하게 PCI 버스에서
	 *       드라이버 식별자로 사용된다. */
	.id_table		= pci_pf_stub_whitelist,
	/* NVMe: pci_device_id 테이블; PCI 코어 열수 시 매칭 기준.
	 *       NVMe 호스트 드라이버의 nvme_id_table과 동일한 역할. */
	.probe			= pci_pf_stub_probe,
	/* NVMe: PF 바인딩 시 호출되는 콜백; NVMe 호스트의 nvme_probe는 BAR0
	 *       레지스터 접근, DMA 일관성 설정, MSI-X/MSI/legacy IRQ 준비를
	 *       수행하지만, 이 스텁은 단순 점유만 수행한다. */
	.sriov_configure	= pci_sriov_configure_simple,
	/* PCI/NVMe: SR-IOV VF 활성화/비활성화 핵심 콜백. pci_sriov_configure_simple()
	 *           는 PCI 코어가 제공하는 일반적 구현으로, SR-IOV 캐퍼빌리티
	 *           레지스터(NumVFs, VF Enable)를 조작하여 VF를 생성/제거한다.
	 *           NVMe PCIe SSD에서 VF는 게스트 VM의 nvme 드라이버에
	 *           PCI passthrough(IOMMU/VT-d)되거나 호스트의 vfio-pci 등에서
	 *           사용될 수 있다. */
};
module_pci_driver(pf_stub_driver);
/* PCI/NVMe: module_init/module_exit을 대신하는 PCI 드라이버 등록 매크로.
 *           pci_register_driver()를 통해 PCI 버스에 등록되며, NVMe 호스트
 *           드라이버의 module_pci_driver(nvme_driver)와 동일한 메커니즘으로
 *           버스 열수/바인딩/핫플러그(PCI hotplug) 이벤트를 처리한다. */

MODULE_DESCRIPTION("SR-IOV PF stub driver with no functionality");
/* NVMe: 별도 기능 없이 SR-IOV PF 점유만을 위한 드라이버임을 설명.
 *       NVMe 장치의 PF를 위한 것은 아니지만, NVMe/SR-IOV 환경에서
 *       VF 리소스 제공을 가능하게 하는 보조 드라이버임을 명시. */
MODULE_LICENSE("GPL");
/* NVMe: GPL v2 라이선스; NVMe 호스트 드라이버와 동일한 커널 모듈 라이선스
 *       정책을 따를 수 있게 한다. */
