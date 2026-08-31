// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2004 Koninklijke Philips Electronics NV
 *
 * Conversion to platform driver and DT:
 * Copyright 2014 Linaro Ltd.
 *
 * 14/04/2005 Initial version, colin.king@philips.com
 */
/*
 * [한국어 설명] ARM Versatile 개발 보드의 PCI 호스트 (pci-versatile.c)
 *
 * === 파일의 역할 ===
 * ARM 이 만든 Versatile/RealView 계열 개발 보드의 PCI 호스트 브리지를
 * 다룬다. 2004년 필립스에서 시작된 오래된 코드이고, 지금은 그 보드를
 * 에뮬레이션하는 QEMU 에서 주로 보게 된다.
 *
 * 이 파일이 흥미로운 이유는 PCIe 이전의 PCI 가 어떻게 생겼는지 보여 주기
 * 때문이다. PCIe 는 점대점 링크지만 원래 PCI 는 여러 장치가 하나의 버스를
 * 공유했고, config 접근도 방식이 달랐다.
 *
 * 여기서 config 접근은 창(window)을 통해 이뤄진다. 특정 물리 주소 범위가
 * config space 로 연결되어 있고, 버스와 장치 번호를 주소 비트에 실어
 * 접근한다. 지금의 ECAM 과 발상은 비슷하지만 비트 배치가 다르다.
 *
 * 자원 창도 하드웨어에 직접 설정한다. 요즘 SoC 는 iATU 같은 유연한 변환
 * 장치를 쓰지만, 이 보드는 고정된 몇 개의 창에 시작 주소만 넣는 식이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 arm,versatile-pci 가 있으면
 *   -> [이 파일] versatile_pci_probe()
 *      -> 레지스터 창 매핑, 자원 창 설정
 *      -> pci_host_probe() -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트. config 접근 함수는 PCI 코어의
 * 잠금 아래에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: PCI 코어(probe.c 의 열거, access.c 의 config 접근 경로).
 * 공유 상태: 매핑한 레지스터 베이스 주소들.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관계가 없다(전수 확인 — 함수 호출 0건). 이 보드에
 * NVMe 를 꽂을 일도 사실상 없다.
 *
 * 그래도 배울 점은 있다. 지금 NVMe 가 쓰는 PCIe 의 config 접근, BAR,
 * 자원 창 같은 개념이 전부 이 시절 PCI 에서 온 것이고, 이 파일은 그것을
 * 가장 벌거벗은 형태로 보여 준다. NVMe 드라이버가 pci_resource_start()
 * 로 얻는 주소가 어떻게 정해지는지 궁금할 때 참고가 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * versatile_pci_probe()   : 레지스터를 매핑하고 자원 창을 설정한 뒤 열거한다.
 *                           이 파일에서 실질적인 일을 하는 유일한 함수다.
 * versatile_map_bus()     : config 접근의 핵심. 버스·devfn·오프셋을 받아
 *                           실제로 읽고 쓸 가상 주소를 돌려준다. 나머지
 *                           읽기·쓰기는 커널의 범용 구현(pci_generic_config_*)
 *                           에 맡기므로, 이 보드에 고유한 것은 주소 계산뿐이다.
 * versatile_pci_slot_ignore : 무시할 슬롯을 지정하는 모듈 파라미터.
 *                           이 보드에는 실제로 아무것도 없는 슬롯 위치가 있어
 *                           거기까지 탐색하면 버스 오류가 나기 때문이다.
 * pci_versatile_ops       : map_bus 와 범용 읽기·쓰기를 묶은 표.
 * versatile_cfg_base[]    : config 창 두 개의 베이스 주소. 타입 0(바로 아래
 *                           장치)과 타입 1(브리지 너머)을 따로 매핑한다.
 * versatile_pci_of_match  : 지원 compatible.
 * versatile_pci_driver    : 플랫폼 드라이버 정의.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include "../pci.h"

static void __iomem *versatile_pci_base;
static void __iomem *versatile_cfg_base[2];

#define PCI_IMAP(m)		(versatile_pci_base + ((m) * 4))
#define PCI_SMAP(m)		(versatile_pci_base + 0x14 + ((m) * 4))
#define PCI_SELFID		(versatile_pci_base + 0xc)

#define VP_PCI_DEVICE_ID		0x030010ee
#define VP_PCI_CLASS_ID			0x0b400000

static u32 pci_slot_ignore;

static int __init versatile_pci_slot_ignore(char *str)
{
	int slot;

	while (get_option(&str, &slot)) {
		if ((slot < 0) || (slot > 31))
			pr_err("Illegal slot value: %d\n", slot);
		else
			pci_slot_ignore |= (1 << slot);
	}
	return 1;
}
__setup("pci_slot_ignore=", versatile_pci_slot_ignore);


static void __iomem *versatile_map_bus(struct pci_bus *bus,
				       unsigned int devfn, int offset)
{
	unsigned int busnr = bus->number;

	if (pci_slot_ignore & (1 << PCI_SLOT(devfn)))
		return NULL;

	return versatile_cfg_base[1] + ((busnr << 16) | (devfn << 8) | offset);
}

static struct pci_ops pci_versatile_ops = {
	.map_bus = versatile_map_bus,
	.read	= pci_generic_config_read32,
	.write	= pci_generic_config_write,
};

static int versatile_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct resource_entry *entry;
	int i, myslot = -1, mem = 1;
	u32 val;
	void __iomem *local_pci_cfg_base;
	struct pci_host_bridge *bridge;

	bridge = devm_pci_alloc_host_bridge(dev, 0);
	if (!bridge)
		return -ENOMEM;

	versatile_pci_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(versatile_pci_base))
		return PTR_ERR(versatile_pci_base);

	versatile_cfg_base[0] = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(versatile_cfg_base[0]))
		return PTR_ERR(versatile_cfg_base[0]);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	versatile_cfg_base[1] = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(versatile_cfg_base[1]))
		return PTR_ERR(versatile_cfg_base[1]);

	resource_list_for_each_entry(entry, &bridge->windows) {
		if (resource_type(entry->res) == IORESOURCE_MEM) {
			writel(entry->res->start >> 28, PCI_IMAP(mem));
			writel(__pa(PAGE_OFFSET) >> 28, PCI_SMAP(mem));
			mem++;
		}
	}

	/*
	 * We need to discover the PCI core first to configure itself
	 * before the main PCI probing is performed
	 */
	for (i = 0; i < 32; i++) {
		if ((readl(versatile_cfg_base[0] + (i << 11) + PCI_VENDOR_ID) == VP_PCI_DEVICE_ID) &&
		    (readl(versatile_cfg_base[0] + (i << 11) + PCI_CLASS_REVISION) == VP_PCI_CLASS_ID)) {
			myslot = i;
			break;
		}
	}
	if (myslot == -1) {
		dev_err(dev, "Cannot find PCI core!\n");
		return -EIO;
	}
	/*
	 * Do not to map Versatile FPGA PCI device into memory space
	 */
	pci_slot_ignore |= (1 << myslot);

	dev_info(dev, "PCI core found (slot %d)\n", myslot);

	writel(myslot, PCI_SELFID);
	local_pci_cfg_base = versatile_cfg_base[1] + (myslot << 11);

	val = readl(local_pci_cfg_base + PCI_COMMAND);
	val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE;
	writel(val, local_pci_cfg_base + PCI_COMMAND);

	/*
	 * Configure the PCI inbound memory windows to be 1:1 mapped to SDRAM
	 */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_0);
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_1);
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_2);

	/*
	 * For many years the kernel and QEMU were symbiotically buggy
	 * in that they both assumed the same broken IRQ mapping.
	 * QEMU therefore attempts to auto-detect old broken kernels
	 * so that they still work on newer QEMU as they did on old
	 * QEMU. Since we now use the correct (ie matching-hardware)
	 * IRQ mapping we write a definitely different value to a
	 * PCI_INTERRUPT_LINE register to tell QEMU that we expect
	 * real hardware behaviour and it need not be backwards
	 * compatible for us. This write is harmless on real hardware.
	 */
	writel(0, versatile_cfg_base[0] + PCI_INTERRUPT_LINE);

	pci_add_flags(PCI_REASSIGN_ALL_BUS);

	bridge->ops = &pci_versatile_ops;

	return pci_host_probe(bridge);
}

static const struct of_device_id versatile_pci_of_match[] = {
	{ .compatible = "arm,versatile-pci", },
	{ },
};
MODULE_DEVICE_TABLE(of, versatile_pci_of_match);

static struct platform_driver versatile_pci_driver = {
	.driver = {
		.name = "versatile-pci",
		.of_match_table = versatile_pci_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = versatile_pci_probe,
};
module_platform_driver(versatile_pci_driver);

MODULE_DESCRIPTION("Versatile PCI driver");
