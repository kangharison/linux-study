// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2004 Koninklijke Philips Electronics NV
 *
 * Conversion to platform driver and DT:
 * Copyright 2014 Linaro Ltd.
 *
 * 14/04/2005 Initial version, colin.king@philips.com
 */
#include <linux/kernel.h>		/* PCI/NVMe: 커널 기본 타입/매크로 포함 */
#include <linux/module.h>		/* PCI/NVMe: module_init/module_platform_driver 등 모듈 등록용 */
#include <linux/of_address.h>		/* PCI/NVMe: DT 'reg' 주소 파싱, BAR 매핑 시 참조 */
#include <linux/of_pci.h>		/* PCI/NVMe: OF PCI 헬퍼, PCIe 열거 지원 */
#include <linux/of_platform.h>		/* PCI/NVMe: platform_device from DT */
#include <linux/pci.h>			/* PCI/NVMe: pci_bus, pci_dev, pci_host_bridge, NVMe SSD 열거 */
#include <linux/platform_device.h>	/* PCI/NVMe: platform_driver probe 구조 */

#include "../pci.h"			/* PCI/NVMe: PCI 코어 날것 매크로/함수, 호스트 브리지 내장 */

static void __iomem *versatile_pci_base;	/* PCI/NVMe: Versatile PCI 컨트롤러 레지스터 베이스 (I/O window 설정용) */
static void __iomem *versatile_cfg_base[2];	/* PCI/NVMe: [0]=로컬/자체 PCI cfg, [1]=외부 PCI cfg 버스 접근용 */

#define PCI_IMAP(m)		(versatile_pci_base + ((m) * 4))	/* PCI/NVMe: PCI->AXI inbound window m 레지스터 */
#define PCI_SMAP(m)		(versatile_pci_base + 0x14 + ((m) * 4))	/* PCI/NVMe: AXI->PCI outbound window m 레지스터 */
#define PCI_SELFID		(versatile_pci_base + 0xc)	/* PCI/NVMe: 자신의 PCI slot 번호를 쓰는 레지스터 */

#define VP_PCI_DEVICE_ID		0x030010ee	/* PCI/NVMe: Versatile FPGA PCI 코어의 (Vendor=0x10ee, Dev=0x0300) ID */
#define VP_PCI_CLASS_ID			0x0b400000	/* PCI/NVMe: PCI-PCI bridge class, 이 슬롯이 FPGA 브리지임을 식별 */

static u32 pci_slot_ignore;	/* PCI/NVMe: config read/write 시 무시할 PCI 슬롯 비트마스크 (NVMe 장치가 아닌 FPGA 브리지 숨김) */

static int __init versatile_pci_slot_ignore(char *str)	/* PCI/NVMe: 부팅 옵션 'pci_slot_ignore=' 처리, 특정 slot을 열거 제외 */
{
	int slot;	/* PCI/NVMe: 파싱 중인 PCI 슬롯 번호 */

	while (get_option(&str, &slot)) {	/* PCI/NVMe: 콤마 구분 슬롯 번호를 하나씩 읽음 */
		if ((slot < 0) || (slot > 31))	/* PCI/NVMe: PCI 슬롯 번호는 0~31 유효 */
			pr_err("Illegal slot value: %d\n", slot);	/* PCI/NVMe: 잘못된 slot 입력 에러 로그 */
		else
			pci_slot_ignore |= (1 << slot);	/* PCI/NVMe: 해당 슬롯을 무시 마스크에 설정, 이 슬롯의 NVMe도 노출 안 됨 */
	}
	return 1;	/* PCI/NVMe: __setup 핸들러 성공 반환 */
}
__setup("pci_slot_ignore=", versatile_pci_slot_ignore);	/* PCI/NVMe: 커널 커맨드라인 옵션 등록 */


static void __iomem *versatile_map_bus(struct pci_bus *bus,	/* PCI/NVMe: PCI cfg 공간 버스 주소 변환, nvme pci.c의 pci_read_config_word 등이 간접 사용 */
				       unsigned int devfn, int offset)	/* PCI/NVMe: devfn=장치/함수, offset=cfg 레지스터 오프셋 */
{
	unsigned int busnr = bus->number;	/* PCI/NVMe: 현재 탐색 중인 PCI 버스 번호 */

	if (pci_slot_ignore & (1 << PCI_SLOT(devfn)))	/* PCI/NVMe: 무시 슬롯이면 config 접근 실패 처리 -> 해당 기능 미탐색 */
		return NULL;

	return versatile_cfg_base[1] + ((busnr << 16) | (devfn << 8) | offset);	/* PCI/NVMe: 외부 cfg 버스 공간에 버스/슬롯/함수/오프셋 합성 */
}

static struct pci_ops pci_versatile_ops = {	/* PCI/NVMe: 이 host controller의 PCI cfg 접근 ops, NVMe 열거/초기화에 사용 */
	.map_bus = versatile_map_bus,	/* PCI/NVMe: bus/devfn/offset -> io memory 매핑 */
	.read	= pci_generic_config_read32,	/* PCI/NVMe: 32bit cfg read; NVMe BAR, CAP, MSI-X cap 읽을 때 사용 */
	.write	= pci_generic_config_write,	/* PCI/NVMe: cfg write; NVMe MSI-X enable, Bus Master 등 설정 시 사용 */
};

static int versatile_pci_probe(struct platform_device *pdev)	/* PCI/NVMe: host controller 드라이버 probe; 성공 시 PCI 버스 생성 -> NVMe SSD 탐색 가능 */
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: platform device의 generic device 포인터 */
	struct resource *res;		/* PCI/NVMe: DT 'reg'로부터 얻은 물리 메모리 리소스 */
	struct resource_entry *entry;	/* PCI/NVMe: bridge->windows에서 MEM window 순회용 */
	int i, myslot = -1, mem = 1;	/* PCI/NVMe: i=루프, myslot=FPGA 브리지 슬롯, mem=inbound window 인덱스 */
	u32 val;			/* PCI/NVMe: PCI_COMMAND 등 레지스터 읽기/수정/쓰기용 */
	void __iomem *local_pci_cfg_base;	/* PCI/NVMe: FPGA PCI 코어 자신의 cfg 공간 포인터 */
	struct pci_host_bridge *bridge;	/* PCI/NVMe: PCI 코어가 관리하는 host bridge 구조체, NVMe bus 열거의 출발점 */

	bridge = devm_pci_alloc_host_bridge(dev, 0);	/* PCI/NVMe: host bridge 할당, 루트 버스 번호 0번 사용 */
	if (!bridge)	/* PCI/NVMe: 할당 실패 시 -ENOMEM */
		return -ENOMEM;

	versatile_pci_base = devm_platform_ioremap_resource(pdev, 0);	/* PCI/NVMe: index 0: Versatile PCI 컨트롤러 레지스터 ioremap */
	if (IS_ERR(versatile_pci_base))	/* PCI/NVMe: ioremap 실패 시 에러 반환 */
		return PTR_ERR(versatile_pci_base);

	versatile_cfg_base[0] = devm_platform_ioremap_resource(pdev, 1);	/* PCI/NVMe: index 1: 로컬/자체 PCI cfg 공간 ioremap */
	if (IS_ERR(versatile_cfg_base[0]))	/* PCI/NVMe: ioremap 실패 시 에러 반환 */
		return PTR_ERR(versatile_cfg_base[0]);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);	/* PCI/NVMe: index 2: 외부 PCI cfg 버스 접근 리소스 가져옴 */
	versatile_cfg_base[1] = devm_pci_remap_cfg_resource(dev, res);	/* PCI/NVMe: PCI cfg 공간으로 재매핑 (특수 속성/보호 적용) */
	if (IS_ERR(versatile_cfg_base[1]))	/* PCI/NVMe: 매핑 실패 시 에러 반환 */
		return PTR_ERR(versatile_cfg_base[1]);

	resource_list_for_each_entry(entry, &bridge->windows) {	/* PCI/NVMe: bridge에 등록된 PCI 메모리 window 순회, NVMe BAR 매핑/DMA 메모리 영역 설정 */
		if (resource_type(entry->res) == IORESOURCE_MEM) {	/* PCI/NVMe: MEM 타입 window만 처리 (NVMe BAR는 MMIO) */
			writel(entry->res->start >> 28, PCI_IMAP(mem));	/* PCI/NVMe: PCI inbound window 시작 주소 상위 4bit 설정 */
			writel(__pa(PAGE_OFFSET) >> 28, PCI_SMAP(mem));	/* PCI/NVMe: SDRAM 시작 주소 상위 4bit -> PCI 버스에서 본 시스템 메모리 1:1 매핑, NVMe DMA용 */
			mem++;	/* PCI/NVMe: 다음 inbound window */
		}
	}

	/*
	 * We need to discover the PCI core first to configure itself
	 * before the main PCI probing is performed
	 */
	for (i = 0; i < 32; i++) {	/* PCI/NVMe: 0~31 슬롯을 스캔하여 Versatile FPGA PCI 브리지 찾기 */
		if ((readl(versatile_cfg_base[0] + (i << 11) + PCI_VENDOR_ID) == VP_PCI_DEVICE_ID) &&	/* PCI/NVMe: slot i의 Vendor/Device ID 확인 */
		    (readl(versatile_cfg_base[0] + (i << 11) + PCI_CLASS_REVISION) == VP_PCI_CLASS_ID)) {	/* PCI/NVMe: class/revision까지 일치하면 FPGA PCI 코어 */
			myslot = i;	/* PCI/NVMe: FPGA 브리지 슬롯 번호 기록 */
			break;	/* PCI/NVMe: 찾으면 스캔 종료 */
		}
	}
	if (myslot == -1) {	/* PCI/NVMe: FPGA PCI 코어를 찾지 못하면 */
		dev_err(dev, "Cannot find PCI core!\n");	/* PCI/NVMe: 치명적 에러 로그 */
		return -EIO;	/* PCI/NVMe: PCIe/NVMe 열거 불가, probe 실패 */
	}
	/*
	 * Do not to map Versatile FPGA PCI device into memory space
	 */
	pci_slot_ignore |= (1 << myslot);	/* PCI/NVMe: FPGA 브리지는 일반 PCI 장치로 메모리 공간 할당하지 않도록 무시 */

	dev_info(dev, "PCI core found (slot %d)\n", myslot);	/* PCI/NVMe: FPGA 브리지 슬롯 발견 로그 */

	writel(myslot, PCI_SELFID);	/* PCI/NVMe: 컨트롤러에 자신의 slot 번호 기록 */
	local_pci_cfg_base = versatile_cfg_base[1] + (myslot << 11);	/* PCI/NVMe: 외부 cfg 버스에서 자신의 cfg 공간 주소 계산 */

	val = readl(local_pci_cfg_base + PCI_COMMAND);	/* PCI/NVMe: 현재 PCI_COMMAND 읽기 (NVMe가 요구하는 Bus Master/MEM enable 전 상태) */
	val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE;	/* PCI/NVMe: MEM enable + Bus Master + 메모리 write-invalidate 설정, NVMe DMA 및 MMIO 필수 */
	writel(val, local_pci_cfg_base + PCI_COMMAND);	/* PCI/NVMe: PCI_COMMAND 갱신, 이후 NVMe SSD가 DMA/MEM access 가능해짐 */

	/*
	 * Configure the PCI inbound memory windows to be 1:1 mapped to SDRAM
	 */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_0);	/* PCI/NVMe: BAR0에 SDRAM 물리주소 쓰기, inbound 1:1 window 0 */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_1);	/* PCI/NVMe: BAR1에 SDRAM 물리주소 쓰기, inbound 1:1 window 1 */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_2);	/* PCI/NVMe: BAR2에 SDRAM 물리주소 쓰기, inbound 1:1 window 2 */

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
	writel(0, versatile_cfg_base[0] + PCI_INTERRUPT_LINE);	/* PCI/NVMe: QEMU 하위호환 감지 묘화; NVMe legacy INT 할당 경로에도 영향 */

	pci_add_flags(PCI_REASSIGN_ALL_BUS);	/* PCI/NVMe: PCI 코어에 모든 버스 번호 재할당 요청, NVMe 장치가 있는 하위 버스 올바르게 번호 매김 */

	bridge->ops = &pci_versatile_ops;	/* PCI/NVMe: host bridge에 cfg read/write ops 등록; 이후 NVMe SSD cfg 접근 시 사용 */

	return pci_host_probe(bridge);	/* PCI/NVMe: PCI 버스 열거 시작 -> NVMe SSD의 pci_dev 생성 후 nvme_pci_probe() 바인딩 가능 */
}

static const struct of_device_id versatile_pci_of_match[] = {	/* PCI/NVMe: DT compatible 매칭 테이블 */
	{ .compatible = "arm,versatile-pci", },	/* PCI/NVMe: Versatile PCI host controller compatible string */
	{ },	/* PCI/NVMe: 테이블 종료 표시 */
};
MODULE_DEVICE_TABLE(of, versatile_pci_of_match);	/* PCI/NVMe: 모듈 로딩 시 OF 매칭 정보 노출 */

static struct platform_driver versatile_pci_driver = {	/* PCI/NVMe: Versatile PCI host controller platform driver */
	.driver = {
		.name = "versatile-pci",	/* PCI/NVMe: platform driver 이름 */
		.of_match_table = versatile_pci_of_match,	/* PCI/NVMe: DT compatible 매칭 */
		.suppress_bind_attrs = true,	/* PCI/NVMe: sysfs bind/unbind 속성 비활성화, 런타임 분리 방지 */
	},
	.probe = versatile_pci_probe,	/* PCI/NVMe: probe 콜백; NVMe PCIe host bridge 초기화 수행 */
};
module_platform_driver(versatile_pci_driver);	/* PCI/NVMe: module init/exit 등록, 로드 시 platform probe 호출 -> NVMe bus 생성 */

MODULE_DESCRIPTION("Versatile PCI driver");
