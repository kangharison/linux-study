// SPDX-License-Identifier: GPL-2.0
/*
 * PCI ROM access routines
 *
 * (C) Copyright 2004 Jon Smirl <jonsmirl@yahoo.com>
 * (C) Copyright 2004 Silicon Graphics, Inc. Jesse Barnes <jbarnes@sgi.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/rom.c)은 PCI 장치의 Expansion ROM(Option ROM) BAR를
 * 제어하고, ROM 이미지를 커널 가상 주소 공간에 매핑하는 기능을 제공한다.
 * NVMe SSD도 PCIe 엔드포인트이므로 PCI_ROM_RESOURCE를 가질 수 있으며,
 * 다음과 같은 NVMe 관련 시나리오에서 본 파일의 함수가 사용될 수 있다.
 *   - NVMe SSD에 내장된 UEFI/BIOS Option ROM이 있는 경우: 부팅 펌웨어가
 *     이 ROM을 통해 NVMe 장치를 초기화하거나 부팅 코드를 로드한다.
 *   - 커널 런타임에서 /sys/bus/pci/devices/<BDF>/rom 파일을 통해 NVMe
 *     장치의 ROM 이미지를 사용자 공간으로 노출할 때 호출된다.
 *   - PCI 핫플러그/재스캔 시 NVMe 장치의 ROM 리소스를 재할당/재활성화
 *     해야 할 때 사용된다.
 *   - ROM BAR를 enable/disable 할 때 일부 장치는 ROM과 MMIO(BAR0/1 등)
 *     간 주소 디코더를 공유하므로, NVMe BAR 접근에 잠재적 영향이 있다.
 * 주요 호출 경로(예시):
 *   sysfs "rom" read -> pci_map_rom() -> pci_enable_rom() -> ioremap()
 *   -> pci_get_rom_size() -> copy_to_user() -> pci_unmap_rom() ->
 *   pci_disable_rom()
 *   또는 부팅 단계에서 vgacon/vgaarb/드라이버 로드 시 ROM을 읽어들이는
 *   경로에서 사용될 수 있다.
 * 본 파일은 drivers/nvme/host/pci.c에서 직접 호출하지는 않지만, NVMe
 * 장치의 PCI 리소스 생명주기와 사용자 공간 ROM 노출에 직접 관여한다.
 * ===================================================================
 */
#include <linux/kernel.h> /* NVMe: 커널 기본 자료형과 매크로 사용 */
#include <linux/export.h> /* NVMe: EXPORT_SYMBOL 관련 매크로 */
#include <linux/pci.h> /* NVMe: PCI 버스 및 pci_dev 구조체 정의 */
#include <linux/slab.h> /* NVMe: 커널 메모리 할당 헬퍼 */

#include "pci.h" /* NVMe: PCI 서브시스템 내부 헤더, ROM 관련 상수 포함 */

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
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스(PCI_ROM_RESOURCE, 일반적으로 리소스 6번) 포인터 획득 */
	struct pci_bus_region region; /* NVMe: CPU 물리 주소를 PCI bus 주소로 변환한 결과를 담을 임시 구조체 */
	u32 rom_addr; /* NVMe: ROM BAR에 쓸 32비트 구성 레지스터 값 */

	if (!res->flags) /* NVMe: ROM 리소스가 전혀 할당/설정되지 않았으면(플래그가 0이면) */
		return -1; /* NVMe: ROM이 없거나 설정되지 않았음을 -1로 반환 */

	/* Nothing to enable if we're using a shadow copy in RAM */
	if (res->flags & IORESOURCE_ROM_SHADOW) /* NVMe: 시스템 RAM에 섀도우된 ROM 복사본을 사용하는 경우(예: VGA 호환 ROM) */
		return 0; /* NVMe: 실제 ROM BAR를 켤 필요가 없으므로 성공(0) 반환 */

	/*
	 * Ideally pci_update_resource() would update the ROM BAR address,
	 * and we would only set the enable bit here.  But apparently some
	 * devices have buggy ROM BARs that read as zero when disabled.
	 */
	pcibios_resource_to_bus(pdev->bus, &region, res); /* NVMe: NVMe ROM 리소스의 CPU 물리 주소를 PCI bus 주소 영역으로 변환(Root Complex window 기준) */
	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr); /* NVMe: NVMe 장치의 ROM BAR 구성 레지스터(0x30) 현재 값 읽기 */
	rom_addr &= ~PCI_ROM_ADDRESS_MASK; /* NVMe: ROM 주소 비트를 제거하고 하위 비트(enable, 지원 여부 등)만 남김 */
	rom_addr |= region.start | PCI_ROM_ADDRESS_ENABLE; /* NVMe: bus 시작 주소와 ROM enable 비트를 OR하여 새 ROM BAR 값 구성 */
	pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr); /* NVMe: 구성된 주소/enable 값을 NVMe 장치의 ROM BAR에 기록하여 ROM 디코딩 활성화 */
	return 0; /* NVMe: ROM BAR enable 성공 */
}
EXPORT_SYMBOL_GPL(pci_enable_rom); /* NVMe: GPL 모듈에서 pci_enable_rom 심볼을 사용할 수 있도록 나이출 */

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
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스 포인터 획득 */
	u32 rom_addr; /* NVMe: ROM BAR 구성 레지스터 값을 읽어올 변수 */

	if (res->flags & IORESOURCE_ROM_SHADOW) /* NVMe: RAM 섀도우 ROM을 사용 중이면 실제 ROM BAR는 쓸 필요 없음 */
		return; /* NVMe: ROM BAR 조작 없이 즉시 반환 */

	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr); /* NVMe: NVMe 장치의 ROM BAR 현재 값 읽기 */
	rom_addr &= ~PCI_ROM_ADDRESS_ENABLE; /* NVMe: ROM enable 비트만 클리어하여 ROM 주소 디코딩을 끔 */
	pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr); /* NVMe: enable 비트가 꺼진 값을 ROM BAR에 기록 */
}
EXPORT_SYMBOL_GPL(pci_disable_rom); /* NVMe: GPL 모듈에서 pci_disable_rom 심볼 노출 */

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
	void __iomem *image; /* NVMe: 현재 검사 중인 ROM 이미지 시작 주소 포인터 */
	int last_image; /* NVMe: 현재 이미지가 ROM 체인의 마지막 이미지인지 표시(0x80 비트) */
	unsigned int length; /* NVMe: 현재 ROM 이미지의 길이(512바이트 단위) */

	image = rom; /* NVMe: 첫 번째 ROM 이미지는 매핑 시작 위치부터 시작 */
	do { /* NVMe: ROM에 여러 이미지가 연쇄되어 있을 수 있으므로 반복 검사 */
		void __iomem *pds; /* NVMe: PCI Data Structure(PCIR)의 커널 가상 주소 */
		/* Standard PCI ROMs start out with these bytes 55 AA */
		if (readw(image) != 0xAA55) { /* NVMe: ROM 이미지 시작 2바이트가 0x55 0xAA(리틀엔디언 읽기 시 0xAA55)인지 확인 */
			pci_info(pdev, "Invalid PCI ROM header signature: expecting 0xaa55, got %#06x\n",
				 readw(image)); /* NVMe: NVMe 장치의 ROM 헤더 시그네처가 잘못되었음을 커널 로그에 출력 */
			break; /* NVMe: 유효한 ROM 이미지가 아니므로 크기 계산 루프 종료 */
		}
		/* get the PCI data structure and check its "PCIR" signature */
		pds = image + readw(image + 24); /* NVMe: ROM 헤더 오프셋 24에 기록된 PCIR 오프셋을 읽어 PCIR 주소 계산 */
		if (readl(pds) != 0x52494350) { /* NVMe: PCIR 시작 4바이트가 "PCIR" 아스키(0x52494350)인지 검증 */
			pci_info(pdev, "Invalid PCI ROM data signature: expecting 0x52494350, got %#010x\n",
				 readl(pds)); /* NVMe: PCIR 시그네처 불일치 시 NVMe 장치 로그 출력 */
			break; /* NVMe: 잘못된 ROM 데이터 구조이므로 루프 종료 */
		}
		last_image = readb(pds + 21) & 0x80; /* NVMe: PCIR 오프셋 21의 최상위 비트로 마지막 이미지 여부 확인 */
		length = readw(pds + 16); /* NVMe: PCIR 오프셋 16에서 이미지 길이(512바이트 블록 수) 읽기 */
		image += length * 512; /* NVMe: 다음 ROM 이미지 위치로 포인터 이동 */
		/* Avoid iterating through memory outside the resource window */
		if (image >= rom + size) /* NVMe: 다음 이미지가 ROM 매핑 윈도우를 벗어나면 */
			break; /* NVMe: 더 이상 안전하게 읽을 수 없으므로 중단 */
		if (!last_image) { /* NVMe: 현재 이미지가 마지막이 아니면 다음 이미지가 유효한지 확인 */
			if (readw(image) != 0xAA55) { /* NVMe: 다음 이미지의 헤더 시그네처가 올바른지 검사 */
				pci_info(pdev, "No more image in the PCI ROM\n"); /* NVMe: 연쇄된 다음 ROM 이미지가 없음을 로그 기록 */
				break; /* NVMe: 더 이상 이미지가 없으므로 루프 종료 */
			}
		}
	} while (length && !last_image); /* NVMe: 길이가 0이 아니고 마지막 이미지가 아닐 때까지 다음 이미지로 진행 */

	/* never return a size larger than the PCI resource window */
	/* there are known ROMs that get the size wrong */
	return min((size_t)(image - rom), size); /* NVMe: 계산된 이미지 누적 크기와 PCI 윈도우 크기 중 작은 값을 반환(잘못된 ROM 길이 대비) */
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
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스 포인터 획득 */
	loff_t start; /* NVMe: ROM 리소스의 bus/물리 시작 주소를 담을 변수 */
	void __iomem *rom; /* NVMe: ioremap 결과 커널 가상 주소 포인터 */

	/* assign the ROM an address if it doesn't have one */
	if (res->parent == NULL && pci_assign_resource(pdev, PCI_ROM_RESOURCE)) /* NVMe: ROM BAR에 아직 부모/주소가 없으면 PCI 코어에 자원 할당 요청 */
		return NULL; /* NVMe: ROM 주소 할당 실패 시 매핑 불가(NULL) 반환 */

	start = pci_resource_start(pdev, PCI_ROM_RESOURCE); /* NVMe: 할당된 ROM BAR의 시작 주소(PCI bus 주소) 획득 */
	*size = pci_resource_len(pdev, PCI_ROM_RESOURCE); /* NVMe: ROM BAR의 길이(윈도우 크기)를 호출자가 제공한 size 변수에 기록 */
	if (*size == 0) /* NVMe: ROM 윈도우 크기가 0이면 매핑할 것이 없음 */
		return NULL; /* NVMe: 매핑할 ROM 공간이 없으므로 NULL 반환 */

	/* Enable ROM space decodes */
	if (pci_enable_rom(pdev)) /* NVMe: ROM BAR 디코딩 활성화; 실패하면 뒤로 감 */
		return NULL; /* NVMe: ROM enable 실패 시 NULL 반환 */

	rom = ioremap(start, *size); /* NVMe: ROM 물리 주소를 커널 가상 주소 공간에 매핑(NVMe 장치의 ROM 직접 접근 가능) */
	if (!rom) /* NVMe: ioremap 실패 시(메모리 부족 등) */
		goto err_ioremap; /* NVMe: 오류 처리 레이블로 이동하여 ROM disable 복원 */

	/*
	 * Try to find the true size of the ROM since sometimes the PCI window
	 * size is much larger than the actual size of the ROM.
	 * True size is important if the ROM is going to be copied.
	 */
	*size = pci_get_rom_size(pdev, rom, *size); /* NVMe: ROM 윈도우 크기에서 실제 ROM 이미지 크기를 파싱하여 갱신 */
	if (!*size) /* NVMe: 유효한 ROM 이미지 크기가 0이면(잘못된 ROM) */
		goto invalid_rom; /* NVMe: invalid_rom 레이블로 이동해 매핑 해제 및 정리 */

	return rom; /* NVMe: 커널 가상 주소로 매핑된 NVMe ROM 이미지 포인터 반환 */

invalid_rom:
	iounmap(rom); /* NVMe: 잘못된 ROM이므로 앞서 매핑한 가상 주소 해제 */
err_ioremap:
	/* restore enable if ioremap fails */
	if (!(res->flags & IORESOURCE_ROM_ENABLE)) /* NVMe: 매핑 전에 ROM이 이미 활성화된 상태가 아니었다면 */
		pci_disable_rom(pdev); /* NVMe: ioremap 실패로 인해 켰던 ROM BAR를 다시 끔(상태 복원) */
	return NULL; /* NVMe: ROM 매핑 최종 실패를 NULL로 알림 */
}
EXPORT_SYMBOL(pci_map_rom); /* NVMe: pci_map_rom 심볼을 모듈에 노출 */

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
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스 포인터 획득 */

	iounmap(rom); /* NVMe: 커널 가상 주소 매핑을 해제하여 해당 주소 공간 반납 */

	/* Disable again before continuing */
	if (!(res->flags & IORESOURCE_ROM_ENABLE)) /* NVMe: 매핑 전 ROM이 enable 상태가 아니었다면(임시로 켰던 경우) */
		pci_disable_rom(pdev); /* NVMe: ROM BAR 디코딩을 다시 비활성화하여 NVMe 장치 상태 복원 */
}
EXPORT_SYMBOL(pci_unmap_rom); /* NVMe: pci_unmap_rom 심볼을 모듈에 노출 */
