// SPDX-License-Identifier: GPL-2.0
/* pci-pf-stub - simple stub driver for PCI SR-IOV PF device
 *
 * This driver is meant to act as a "whitelist" for devices that provide
 * SR-IOV functionality while at the same time not actually needing a
 * driver of their own.
 */

/*
 * [한국어 설명] SR-IOV PF 만 담당하는 최소 드라이버 (pci-pf-stub.c)
 *
 * === 파일의 역할 ===
 * pci-stub.c 와 이름이 비슷하지만 목적이 다르다. 이쪽은 SR-IOV 의
 * PF(Physical Function)에 붙어 VF 를 만들고 없애는 일만 한다.
 *
 * 배경을 알아야 한다. SR-IOV 로 VF 를 만들려면 PF 에 드라이버가 바인딩돼
 * 있어야 한다. sysfs 의 sriov_numvfs 를 쓰려면 그 드라이버가
 * .sriov_configure 콜백을 제공해야 하기 때문이다.
 *
 * 그런데 PF 자체는 쓸 일이 없고 VF 만 게스트에 넘기고 싶은 경우가 있다.
 * 그때 PF 에 진짜 드라이버를 붙이면 불필요한 초기화가 일어나고, 아무
 * 드라이버도 안 붙이면 VF 를 만들 수 없다. 이 파일이 그 사이를 메운다 —
 * PF 를 붙잡되 .sriov_configure = pci_sriov_configure_simple 만 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 바인딩: PF 에 이 드라이버가 붙는다(id_table 또는 dynid).
 * VF 생성: echo 4 > /sys/bus/pci/devices/<PF>/sriov_numvfs
 *            -> pci-sysfs.c -> drv->sriov_configure
 *               -> [이 파일이 지정한] pci_sriov_configure_simple [iov.c]
 *                  -> pci_enable_sriov() -> VF 들이 나타난다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: sysfs 의 sriov_numvfs.
 * 아래쪽: iov.c 의 pci_sriov_configure_simple.
 * 공유 상태: 없다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일을 쓰지 않지만, 흥미롭게도 같은 콜백을 쓴다.
 *
 *   static struct pci_driver nvme_driver = {
 *       ...
 *       .sriov_configure = pci_sriov_configure_simple,
 *   };
 *
 * 즉 NVMe 는 자기 드라이버 안에 이 파일과 똑같은 역할을 직접 넣어 두었다.
 * NVMe SR-IOV 를 지원하는 컨트롤러에서 sriov_numvfs 로 VF 를 만들면
 * 그 VF 들이 각각 독립된 NVMe 컨트롤러로 나타나고, 각자에게 nvme
 * 드라이버가 다시 바인딩된다.
 *
 * === 주요 함수/구조체 요약 ===
 * pf_stub_probe()   : PF 에 바인딩됐음을 로그로 남기고 0 을 돌려준다.
 * pf_stub_driver    : .sriov_configure = pci_sriov_configure_simple 을
 *                     지정한 것이 이 드라이버의 존재 이유다.
 */

#include <linux/module.h>
#include <linux/pci.h>

/*
 * pci_pf_stub_whitelist - White list of devices to bind pci-pf-stub onto
 *
 * This table provides the list of IDs this driver is supposed to bind
 * onto.  You could think of this as a list of "quirked" devices where we
 * are adding support for SR-IOV here since there are no other drivers
 * that they would be running under.
 */
static const struct pci_device_id pci_pf_stub_whitelist[] = {
	{ PCI_VDEVICE(AMAZON, 0x0053) },
	/* required last entry */
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, pci_pf_stub_whitelist);

static int pci_pf_stub_probe(struct pci_dev *dev,
			     const struct pci_device_id *id)
{
	pci_info(dev, "claimed by pci-pf-stub\n");
	return 0;
}

static struct pci_driver pf_stub_driver = {
	.name			= "pci-pf-stub",
	.id_table		= pci_pf_stub_whitelist,
	.probe			= pci_pf_stub_probe,
	.sriov_configure	= pci_sriov_configure_simple,
};
module_pci_driver(pf_stub_driver);

MODULE_DESCRIPTION("SR-IOV PF stub driver with no functionality");
MODULE_LICENSE("GPL");
