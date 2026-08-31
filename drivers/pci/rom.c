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
int pci_enable_rom(struct pci_dev *pdev) /* NVMe: NVMe SSD 등 PCI 장치의 ROM BAR 디코딩을 활성화하는 함수 진입점 */
{ /* NVMe: pci_enable_rom 함수 본문 시작: ROM BAR enable 및 주소 복원 */
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
} /* NVMe: pci_enable_rom 함수 본문 종료 */
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
void pci_disable_rom(struct pci_dev *pdev) /* NVMe: NVMe SSD 등의 ROM BAR 디코딩을 비활성화하는 함수 진입점 */
{ /* NVMe: pci_disable_rom 함수 본문 시작: ROM BAR enable 비트 클리어 */
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스 포인터 획득 */
	u32 rom_addr; /* NVMe: ROM BAR 구성 레지스터 값을 읽어올 변수 */

	if (res->flags & IORESOURCE_ROM_SHADOW) /* NVMe: RAM 섀도우 ROM을 사용 중이면 실제 ROM BAR는 쓸 필요 없음 */
		return; /* NVMe: ROM BAR 조작 없이 즉시 반환 */

	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr); /* NVMe: NVMe 장치의 ROM BAR 현재 값 읽기 */
	rom_addr &= ~PCI_ROM_ADDRESS_ENABLE; /* NVMe: ROM enable 비트만 클리어하여 ROM 주소 디코딩을 끔 */
	pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr); /* NVMe: enable 비트가 꺼진 값을 ROM BAR에 기록 */
} /* NVMe: pci_disable_rom 함수 본문 종료 */
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
static size_t pci_get_rom_size(struct pci_dev *pdev, void __iomem *rom, /* NVMe: NVMe 장치의 ROM 이미지 중 실제 유효 크기를 계산하는 정적 함수 선언 */
			       size_t size) /* NVMe: ROM 매핑 윈도우 전체 크기(바이트) */
{ /* NVMe: pci_get_rom_size 함수 본문 시작: ROM 헤더/PCIR 파싱 */
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
		} /* NVMe: PCIR 시그네처 검증 if 블록 종료 */
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
			} /* NVMe: 다음 이미지 헤더 검증 if 블록 종료 */
		} /* NVMe: 마지막 이미지가 아닐 때만 실행되는 if 블록 종료 */
	} while (length && !last_image); /* NVMe: 길이가 0이 아니고 마지막 이미지가 아닐 때까지 다음 이미지로 진행 */

	/* never return a size larger than the PCI resource window */
	/* there are known ROMs that get the size wrong */
	return min((size_t)(image - rom), size); /* NVMe: 계산된 이미지 누적 크기와 PCI 윈도우 크기 중 작은 값을 반환(잘못된 ROM 길이 대비) */
} /* NVMe: pci_get_rom_size 함수 본문 종료: 실제 ROM 이미지 크기 반환 */

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
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) /* NVMe: NVMe 장치의 ROM을 커널 가상 주소 공간에 매핑하는 함수 진입점 */
{ /* NVMe: pci_map_rom 함수 본문 시작: 주소 할당, ROM enable, ioremap 수행 */
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

invalid_rom: /* NVMe: ROM 이미지가 유효하지 않을 때 iounmap 후 정리하는 오류 처리 레이블 */
	iounmap(rom); /* NVMe: 잘못된 ROM이므로 앞서 매핑한 가상 주소 해제 */
err_ioremap: /* NVMe: ioremap 실패 시 ROM BAR enable 상태를 복원하는 오류 처리 레이블 */
	/* restore enable if ioremap fails */
	if (!(res->flags & IORESOURCE_ROM_ENABLE)) /* NVMe: 매핑 전에 ROM이 이미 활성화된 상태가 아니었다면 */
		pci_disable_rom(pdev); /* NVMe: ioremap 실패로 인해 켰던 ROM BAR를 다시 끔(상태 복원) */
	return NULL; /* NVMe: ROM 매핑 최종 실패를 NULL로 알림 */
} /* NVMe: pci_map_rom 함수 본문 종료 */
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
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) /* NVMe: pci_map_rom()으로 매핑한 NVMe 장치 ROM을 해제하는 함수 진입점 */
{ /* NVMe: pci_unmap_rom 함수 본문 시작: iounmap 및 ROM BAR 상태 복원 */
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE]; /* NVMe: NVMe 장치의 ROM 리소스 포인터 획득 */

	iounmap(rom); /* NVMe: 커널 가상 주소 매핑을 해제하여 해당 주소 공간 반납 */

	/* Disable again before continuing */
	if (!(res->flags & IORESOURCE_ROM_ENABLE)) /* NVMe: 매핑 전 ROM이 enable 상태가 아니었다면(임시로 켰던 경우) */
		pci_disable_rom(pdev); /* NVMe: ROM BAR 디코딩을 다시 비활성화하여 NVMe 장치 상태 복원 */
} /* NVMe: pci_unmap_rom 함수 본문 종료 */
EXPORT_SYMBOL(pci_unmap_rom); /* NVMe: pci_unmap_rom 심볼을 모듈에 노출 */
