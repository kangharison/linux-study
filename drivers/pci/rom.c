// SPDX-License-Identifier: GPL-2.0
/*
 * PCI ROM access routines
 *
 * (C) Copyright 2004 Jon Smirl <jonsmirl@yahoo.com>
 * (C) Copyright 2004 Silicon Graphics, Inc. Jesse Barnes <jbarnes@sgi.com>
 */

/*
 * [한국어 설명] 장치에 내장된 옵션 ROM 을 읽는 계층 (rom.c)
 *
 * === 파일의 역할 ===
 * 옵션 ROM(Expansion ROM)은 장치가 자기 안에 들고 있는 코드다. 부팅 초기,
 * OS 가 뜨기 전에 펌웨어가 그것을 실행해 장치를 쓸 수 있게 만든다.
 * 그래픽 카드의 VGA BIOS 와 부팅 가능한 스토리지 컨트롤러의 코드가
 * 대표적이며, x86 BIOS 시절부터 이어져 온 구조다.
 *
 * ROM 은 일곱 번째 BAR 처럼 취급된다(PCI_ROM_RESOURCE). 다만 두 가지가 다르다.
 *   - Enable 비트가 따로 있다. 평소에는 꺼 두고, 읽을 때만 잠시 켠다.
 *     켜 두면 그 주소 구간을 장치가 계속 점유하기 때문이다.
 *   - 크기가 실제 내용보다 클 수 있다. ROM 안에는 여러 이미지가 이어져
 *     있고(각각 다른 아키텍처용), 각 이미지 헤더에 "다음이 있는가" 표시가
 *     있다. 그것을 따라가야 실제 길이를 안다.
 *
 * 이 파일은 그 절차를 처리한다. Enable 을 켜고, 매핑하고, 헤더의 서명
 * (0xAA55)을 확인하고, 이미지 사슬을 훑어 크기를 재고, 다 끝나면 Enable 을
 * 다시 끈다.
 *
 * 세 가지 출처를 시도한다는 점도 중요하다.
 *   1) 장치의 ROM BAR (하드웨어에 실제로 있는 것)
 *   2) 펌웨어가 시스템 메모리에 복사해 둔 것(dev->rom, ROM shadow)
 *   3) 플랫폼이 제공하는 것(pci_platform_rom)
 * 2번이 필요한 이유는 일부 장치가 초기화된 뒤에는 ROM 을 읽을 수 없게
 * 되기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 읽기: cat /sys/bus/pci/devices/.../rom
 *         -> pci-sysfs.c 의 pci_read_rom()
 *            -> [이 파일] pci_map_rom() — Enable 을 켜고 매핑, 크기 계산
 *            -> 내용을 사용자에게 복사
 *            -> [이 파일] pci_unmap_rom() — 매핑 해제, Enable 을 다시 끔
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(ioremap 과 config 접근).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sysfs.c 의 rom 속성, 그리고 ROM 을 읽어야 하는 드라이버
 *   (GPU 드라이버가 VBIOS 를 파싱할 때).
 * 아래쪽: setup-res.c 의 자원 관리, access.c 의 config 접근, ioremap.
 * 공유 상태: struct pci_dev 의 rom / romlen(펌웨어가 복사해 둔 것),
 *   rom_attr_enabled(sysfs 에서 읽기를 허용했는지), 그리고
 *   resource[PCI_ROM_RESOURCE].
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * NVMe SSD 도 옵션 ROM 을 가질 수 있고, 실제로 부팅 가능한 엔터프라이즈
 * 드라이브에는 있는 경우가 있다. 그 ROM 이 UEFI 드라이버를 담아, OS 가
 * 뜨기 전에 펌웨어가 그 드라이브에서 부팅할 수 있게 한다.
 *
 * 하지만 커널이 뜬 뒤에는 그 ROM 이 쓰이지 않는다. NVMe 드라이버가 자기
 * 코드로 컨트롤러를 처음부터 초기화하기 때문이다. 그래서 이 파일과
 * NVMe 의 접점은 "sysfs 로 ROM 을 덤프할 수 있다" 는 정도에 그친다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_map_rom()          : ROM 을 읽을 수 있게 준비한다. Enable 을 켜고,
 *                          매핑하고, 이미지 사슬을 훑어 실제 크기를 정한다.
 *                          펌웨어 사본이 있으면 그쪽을 우선한다.
 * pci_unmap_rom()        : 매핑을 풀고 Enable 을 원래대로 되돌린다.
 * pci_enable_rom()       : ROM BAR 의 Enable 비트를 켠다. 자원이 배정돼
 *                          있지 않으면 실패한다.
 * pci_disable_rom()      : 끈다.
 * pci_get_rom_size()     : 이미지 헤더의 서명(0xAA55)과 "다음 이미지" 표시를
 *                          따라가며 전체 크기를 잰다. 이 파일에서 가장
 *                          형식 지식이 필요한 부분이다.
 * pci_rom_size()         : 위 결과를 캐시해 돌려준다.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "pci.h"

/**
 * pci_enable_rom - enable ROM decoding for a PCI device
 * @pdev: PCI device to enable
 *
 * Enable ROM decoding on @dev.  This involves simply turning on the last
 * bit of the PCI ROM BAR.  Note that some cards may share address decoders
 * between the ROM and other resources, so enabling it may disable access
 * to MMIO registers or other card memory.
 */
/*
 * pci_enable_rom:
 *   지정된 PCI 장치(예: NVMe SSD)의 ROM BAR 디코딩을 활성화한다.
 *   ROM 이미지를 읽기 전에 반드시 호출해야 하며, 일부 장치는 ROM과
 *   MMIO(BAR0/1) 간 주소 디코더를 공유하므로 NVMe 레지스터 접근에
 *   영향을 줄 수 있음에 주의해야 한다.
 */
int pci_enable_rom(struct pci_dev *pdev)
{
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	struct pci_bus_region region;
	u32 rom_addr;

	if (!res->flags)
		return -1;

	/* Nothing to enable if we're using a shadow copy in RAM */
	if (res->flags & IORESOURCE_ROM_SHADOW)
		return 0;

	/*
	 * Ideally pci_update_resource() would update the ROM BAR address,
	 * and we would only set the enable bit here.  But apparently some
	 * devices have buggy ROM BARs that read as zero when disabled.
	 */
	pcibios_resource_to_bus(pdev->bus, &region, res);
	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
	rom_addr &= ~PCI_ROM_ADDRESS_MASK;
	rom_addr |= region.start | PCI_ROM_ADDRESS_ENABLE;
	pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_enable_rom);

/**
 * pci_disable_rom - disable ROM decoding for a PCI device
 * @pdev: PCI device to disable
 *
 * Disable ROM decoding on a PCI device by turning off the last bit in the
 * ROM BAR.
 */
/*
 * pci_disable_rom:
 *   NVMe 장치 등의 ROM BAR 디코딩을 비활성화한다.
 *   ROM 이미지를 다 읽은 후 또는 ROM 매핑 해제 시 호출되어, ROM이
 *   NVMe BAR 접근이나 다른 리소스와 충돌하지 않도록 한다.
 */
void pci_disable_rom(struct pci_dev *pdev)
{
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	u32 rom_addr;

	if (res->flags & IORESOURCE_ROM_SHADOW)
		return;

	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
	rom_addr &= ~PCI_ROM_ADDRESS_ENABLE;
	pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr);
}
EXPORT_SYMBOL_GPL(pci_disable_rom);

/**
 * pci_get_rom_size - obtain the actual size of the ROM image
 * @pdev: target PCI device
 * @rom: kernel virtual pointer to image of ROM
 * @size: size of PCI window
 *  return: size of actual ROM image
 *
 * Determine the actual length of the ROM image.
 * The PCI window size could be much larger than the
 * actual image size.
 */
/*
 * pci_get_rom_size:
 *   NVMe 장치의 ROM 이미지 중 실제로 유효한 부분의 크기를 바이트 단위로
 *   계산한다. PCI ROM BAR가 할당받은 윈도우 크기는 실제 이미지보다 클
 *   수 있으므로, 헤더(0xAA55), PCIR 시그네처, 이미지 길이 필드를 검사해
 *   진짜 크기를 찾는다. /sys/.../rom 읽기 시 copy_to_user에 직접 영향.
 */
static size_t pci_get_rom_size(struct pci_dev *pdev, void __iomem *rom,
			       size_t size)
{
	void __iomem *image;
	int last_image;
	unsigned int length;

	image = rom;
	do {
		void __iomem *pds;
		/* Standard PCI ROMs start out with these bytes 55 AA */
		if (readw(image) != 0xAA55) {
			pci_info(pdev, "Invalid PCI ROM header signature: expecting 0xaa55, got %#06x\n",
				 readw(image));
			break;
		}
		/* get the PCI data structure and check its "PCIR" signature */
		pds = image + readw(image + 24);
		if (readl(pds) != 0x52494350) {
			pci_info(pdev, "Invalid PCI ROM data signature: expecting 0x52494350, got %#010x\n",
				 readl(pds));
			break;
		}
		last_image = readb(pds + 21) & 0x80;
		length = readw(pds + 16);
		image += length * 512;
		/* Avoid iterating through memory outside the resource window */
		if (image >= rom + size)
			break;
		if (!last_image) {
			if (readw(image) != 0xAA55) {
				pci_info(pdev, "No more image in the PCI ROM\n");
				break;
			}
		}
	} while (length && !last_image);

	/* never return a size larger than the PCI resource window */
	/* there are known ROMs that get the size wrong */
	return min((size_t)(image - rom), size);
}

/**
 * pci_map_rom - map a PCI ROM to kernel space
 * @pdev: pointer to pci device struct
 * @size: pointer to receive size of pci window over ROM
 *
 * Return: kernel virtual pointer to image of ROM
 *
 * Map a PCI ROM into kernel space. If ROM is boot video ROM,
 * the shadow BIOS copy will be returned instead of the
 * actual ROM.
 */
/*
 * pci_map_rom:
 *   NVMe 장치의 ROM을 커널 가상 주소 공간에 매핑한다.
 *   /sys/bus/pci/devices/<NVMe BDF>/rom 읽기, 드라이버의 ROM 복사,
 *   또는 초기화 시 Option ROM을 읽을 때 사용된다.
 *   필요 시 ROM BAR에 주소를 할당하고, ROM 디코딩을 활성화한 뒤
 *   ioremap()으로 커널에 매핑한다.
 */
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	loff_t start;
	void __iomem *rom;

	/* assign the ROM an address if it doesn't have one */
	if (res->parent == NULL && pci_assign_resource(pdev, PCI_ROM_RESOURCE))
		return NULL;

	start = pci_resource_start(pdev, PCI_ROM_RESOURCE);
	*size = pci_resource_len(pdev, PCI_ROM_RESOURCE);
	if (*size == 0)
		return NULL;

	/* Enable ROM space decodes */
	if (pci_enable_rom(pdev))
		return NULL;

	rom = ioremap(start, *size);
	if (!rom)
		goto err_ioremap;

	/*
	 * Try to find the true size of the ROM since sometimes the PCI window
	 * size is much larger than the actual size of the ROM.
	 * True size is important if the ROM is going to be copied.
	 */
	*size = pci_get_rom_size(pdev, rom, *size);
	if (!*size)
		goto invalid_rom;

	return rom;

invalid_rom:
	iounmap(rom);
err_ioremap:
	/* restore enable if ioremap fails */
	if (!(res->flags & IORESOURCE_ROM_ENABLE))
		pci_disable_rom(pdev);
	return NULL;
}
EXPORT_SYMBOL(pci_map_rom);

/**
 * pci_unmap_rom - unmap the ROM from kernel space
 * @pdev: pointer to pci device struct
 * @rom: virtual address of the previous mapping
 *
 * Remove a mapping of a previously mapped ROM
 */
/*
 * pci_unmap_rom:
 *   pci_map_rom()으로 매핑한 NVMe 장치의 ROM을 커널 가상 주소 공간에서
 *   해제한다. /sys/.../rom 사용 종료, 드라이버 종료, 핫플러그 제거
 *   등에서 호출되며, ROM BAR enable 상태를 원래대로 복원한다.
 */
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];

	iounmap(rom);

	/* Disable again before continuing */
	if (!(res->flags & IORESOURCE_ROM_ENABLE))
		pci_disable_rom(pdev);
}
EXPORT_SYMBOL(pci_unmap_rom);
