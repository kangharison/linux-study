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
 * 이 파일에는 크기를 캐시해 두는 함수가 없다. pci_get_rom_size() 를 부를
 * 때마다 이미지 헤더를 다시 따라간다.
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
/* [한국어]
 * pci_enable_rom - 확장 ROM BAR 의 디코딩을 켠다
 *
 * @pdev: 대상 장치.
 * @return: 0 = 성공(또는 켤 필요 없음), -1 = ROM 자원이 없음.
 *
 * 확장 ROM 은 다른 BAR 과 달리 자기 Enable 비트를 따로 갖는다. 부팅 뒤에는
 * 쓸 일이 거의 없어 평소 꺼 두고 필요할 때만 켜기 위해서다.
 *
 * 주소와 Enable 비트를 **한 번에** 쓰는 것이 이 함수의 요점이다. 주소를
 * 먼저 쓰고 나중에 켜면, 그 사이에 잘못된 주소로 디코딩이 켜지는 순간이
 * 생길 수 있다.
 *
 * 주소를 다시 계산하는 이유는 자원이 재배정됐을 수 있기 때문이다. 커널이
 * 아는 CPU 주소를 버스 주소로 되돌려야 레지스터에 쓸 값이 된다.
 *
 * shadow ROM 이면 아무것도 하지 않고 성공을 답한다. 그때 ROM 사본이 이미
 * 시스템 메모리(0xC0000 영역)에 올라와 있어, 장치의 ROM 을 켤 이유가 없다.
 *
 * pci_disable_rom() 과 짝을 이루며, 호출자는 다 읽은 뒤 반드시 꺼야 한다.
 *
 * 실행 컨텍스트: ROM 읽기 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: ROM 자원이 아예 없으면 -1 이다. 다른 관용과 달리 -errno 가
 * 아니라 -1 인 점에 주의할 만하다.
 *
 * 호출 체인:
 *   pci_map_rom() / 드라이버 → [이 함수]
 *     → pcibios_resource_to_bus() → pci_write_config_dword()
 */
int pci_enable_rom(struct pci_dev *pdev)
{
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	/* [한국어] pcibios_resource_to_bus() 가 채워 줄 버스 주소 표현. CPU 물리 주소와
	 * PCI 버스 주소가 다른 플랫폼이 있어, BAR 에 쓸 값은 반드시 버스 주소여야 한다. */
	struct pci_bus_region region;
	/* [한국어] ROM BAR 의 현재 값을 담을 변수. */
	u32 rom_addr;

	/* [한국어] flags 가 0 이면 이 장치에 ROM 자원 자체가 없다는 뜻이다. */
	if (!res->flags)
		/* [한국어] [상류 코드 관찰] errno 가 아니라 -1 을 돌려준다. 호출자 pci_map_rom() 은
		 * 0 이 아니면 실패로만 보므로 값 자체는 중요하지 않다. */
		return -1;

	/* Nothing to enable if we're using a shadow copy in RAM */
	if (res->flags & IORESOURCE_ROM_SHADOW)
		return 0;

	/*
	 * Ideally pci_update_resource() would update the ROM BAR address,
	 * and we would only set the enable bit here.  But apparently some
	 * devices have buggy ROM BARs that read as zero when disabled.
	 */
	/* [한국어] CPU 물리 주소인 res->start 를 PCI 버스 주소로 변환한다. BAR 에는 장치가
	 * 이해하는 버스 주소를 써야 하며, 두 주소가 같지 않은 플랫폼이 실제로 있다. */
	pcibios_resource_to_bus(pdev->bus, &region, res);
	/* [한국어] 현재 ROM BAR 값을 읽는다. 위 영어 주석이 밝히듯 이상적으로는
	 * pci_update_resource() 가 주소를 채우고 여기서는 활성화 비트만 세우면 되지만,
	 * 비활성 상태에서 BAR 를 0 으로 읽히게 하는 결함 장치들이 있어 매번 다시 쓴다. */
	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
	/* [한국어] 주소 필드를 지운다. PCI_ROM_ADDRESS_MASK 는 주소 비트들의 마스크이고,
	 * AND ~ 로 그 자리를 비워 아래에서 새 주소를 넣을 자리를 만든다.
	 * 마스크 밖의 비트(예약 비트 등)는 보존한다. */
	rom_addr &= ~PCI_ROM_ADDRESS_MASK;
	/* [한국어] 새 버스 주소와 활성화 비트(bit 0)를 한 번에 넣는다. */
	rom_addr |= region.start | PCI_ROM_ADDRESS_ENABLE;
	/* [한국어] 완성한 값을 되쓴다. 이 쓰기가 실제로 ROM 디코딩을 켠다 — 위 영어 주석의
	 * 경고대로, 카드에 따라 이 순간 MMIO 레지스터 접근이 막힐 수 있다. */
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
/* [한국어]
 * pci_disable_rom - 확장 ROM BAR 의 디코딩을 끈다
 *
 * @pdev: 대상 장치.
 *
 * pci_enable_rom() 의 짝이다. Enable 비트만 지우고 주소는 남겨 둔다 —
 * 다음에 켤 때 다시 쓰므로 지울 이유가 없다.
 *
 * 읽기-수정-쓰기인 것이 그래서다. 통째로 0 을 쓰면 주소까지 지워진다.
 *
 * 꺼야 하는 이유는 주소 공간 때문이다. ROM 이 차지하는 범위는 보통 크고
 * 평소에는 아무도 읽지 않아, 켜 둔 채로 두면 다른 장치가 쓸 수 있었을
 * 주소 공간을 낭비한다.
 *
 * shadow ROM 이면 켠 적이 없으므로 끄지도 않는다. enable 쪽과 같은 조건이다.
 *
 * 실행 컨텍스트: ROM 읽기 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_unmap_rom() / pci_map_rom() 의 오류 경로 → [이 함수]
 *     → pci_write_config_dword()
 */
void pci_disable_rom(struct pci_dev *pdev)
{
	/* [한국어] 이 장치의 ROM 자원. */
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	/* [한국어] ROM BAR 의 현재 값을 담을 변수. */
	u32 rom_addr;

	/* [한국어] RAM 에 있는 그림자 복사본을 쓰는 중이면 실제 ROM 디코딩이 애초에 켜져 있지 않다. */
	if (res->flags & IORESOURCE_ROM_SHADOW)
		return;

	/* [한국어] 현재 BAR 값을 읽는다. */
	pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
	/* [한국어] 활성화 비트만 지운다. 주소 필드는 그대로 두어, 다음에 다시 켤 때
	 * 같은 주소를 재사용할 수 있게 한다. */
	rom_addr &= ~PCI_ROM_ADDRESS_ENABLE;
	/* [한국어] 되쓴다. 이 시점부터 ROM 영역이 더 이상 디코딩되지 않으므로,
	 * 그 주소로 접근하면 다른 자원(또는 아무것도 아닌 것)에 닿는다. */
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
/* [한국어]
 * pci_get_rom_size - ROM 안의 이미지들을 훑어 실제 크기를 알아낸다
 *
 * @pdev: 대상 장치.
 * @rom: 매핑된 ROM 의 시작 주소.
 * @size: 자원 창의 크기(상한).
 * @return: 실제 ROM 내용의 크기.
 *
 * BAR 이 알려 주는 크기는 **자원 창** 의 크기일 뿐 내용의 크기가 아니다.
 * 2MB 창에 512KB 만 들어 있는 것이 흔하다. 그 차이를 알아내는 것이 이
 * 함수의 일이다.
 *
 * ROM 안에는 이미지가 여러 개 이어 붙어 있을 수 있다. 같은 카드를 x86 BIOS
 * 와 UEFI, 또는 여러 아키텍처에서 쓰려고 각각의 이미지를 담기 때문이다.
 * 그래서 하나씩 따라가며 마지막 표시가 나올 때까지 세는 구조가 된다.
 *
 * 각 이미지의 형식이 두 겹의 서명으로 확인된다. 앞의 0xAA55 는 PCI ROM 의
 * 표준 시작 바이트이고(옆의 상류 주석), 그 안에서 가리키는 PCI Data Structure
 * 의 "PCIR" 서명이 두 번째다. 둘 중 하나라도 어긋나면 거기서 멈춘다 —
 * 쓰레기 데이터를 이미지 길이로 읽어 엉뚱한 곳으로 뛰지 않기 위해서다.
 *
 * 길이 단위가 512바이트인 것은 규격이 정한 것이다.
 *
 * 마지막 min() 이 안전장치다. 옆의 상류 주석이 그 이유를 밝히는데,
 * 크기를 틀리게 적어 둔 ROM 이 실제로 존재하기 때문이다. 창 밖을 읽으면
 * 매핑되지 않은 주소에 접근하게 된다. 루프 안에도 같은 취지의 검사가 있어
 * 두 겹으로 막는다.
 *
 * 실행 컨텍스트: ROM 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 서명이 어긋나면 그 자리까지의 크기를 돌려주며 로그를 남긴다.
 * 첫 이미지부터 어긋나면 0 이 되고, 호출자가 그것을 실패로 해석한다.
 *
 * 호출 체인:
 *   pci_map_rom() → [이 함수] → readw() / readl() / readb()
 */
static size_t pci_get_rom_size(struct pci_dev *pdev, void __iomem *rom,
			       size_t size)
{
	/* [한국어] 현재 검사 중인 이미지의 시작 주소. rom 에서 시작해 이미지 하나씩 앞으로 나아간다. */
	void __iomem *image;
	/* [한국어] 이 이미지가 마지막인지 나타내는 플래그(0x80 비트). */
	int last_image;
	/* [한국어] 이 이미지의 길이(512바이트 블록 단위). */
	unsigned int length;

	/* [한국어] 첫 이미지부터 시작한다. */
	image = rom;
	do {
		void __iomem *pds;
		/* Standard PCI ROMs start out with these bytes 55 AA */
		/* [한국어] 표준 PCI ROM 은 0x55 0xAA 로 시작한다(리틀엔디안 워드로 0xAA55).
		 * 위 영어 주석이 그 사실을 밝힌다. */
		if (readw(image) != 0xAA55) {
			/* [한국어] 서명이 다르면 유효한 ROM 이 아니다. 지금까지 나아간 만큼만 유효 크기로 친다. */
			pci_info(pdev, "Invalid PCI ROM header signature: expecting 0xaa55, got %#06x\n",
				 readw(image));
			break;
		}
		/* get the PCI data structure and check its "PCIR" signature */
		/* [한국어] PCIR 데이터 구조의 위치는 헤더 오프셋 24(0x18)에 담긴 상대 오프셋으로 주어진다.
		 * PCI 펌웨어 규격이 정한 배치이며, 이 구조체 안에 이미지 길이와 마지막 여부가 있다. */
		pds = image + readw(image + 24);
		/* [한국어] "PCIR" 네 글자의 리틀엔디안 32비트 표현이 0x52494350 이다.
		 * 'P'=0x50, 'C'=0x43, 'I'=0x49, 'R'=0x52 이므로 역순으로 읽으면 그 값이 된다. */
		if (readl(pds) != 0x52494350) {
			/* [한국어] 서명이 다르면 PCIR 구조체가 없는 것이므로 더 진행할 수 없다. */
			pci_info(pdev, "Invalid PCI ROM data signature: expecting 0x52494350, got %#010x\n",
				 readl(pds));
			break;
		}
		/* [한국어] PCIR 오프셋 21(0x15)의 최상위 비트가 "마지막 이미지" 표시다.
		 * 하나의 ROM 에 여러 이미지(예: 레거시 x86 BIOS 와 UEFI 드라이버)가 이어져
		 * 있을 수 있어, 그 끝을 알아야 실제 크기를 알 수 있다. */
		last_image = readb(pds + 21) & 0x80;
		/* [한국어] PCIR 오프셋 16(0x10)에 이 이미지의 길이가 512바이트 블록 단위로 들어 있다. */
		length = readw(pds + 16);
		/* [한국어] 다음 이미지의 시작으로 건너뛴다. 512 를 곱해 실제 바이트 수로 바꾼다. */
		image += length * 512;
		/* Avoid iterating through memory outside the resource window */
		/* [한국어] 위 영어 주석대로, 계산된 다음 이미지 위치가 매핑한 창을 벗어나면 멈춘다.
		 * ROM 이 잘못된 길이를 보고하는 경우가 실제로 있어, 그대로 따라가면
		 * 매핑 밖 메모리를 읽게 된다. */
		if (image >= rom + size)
			break;
		/* [한국어] 마지막 이미지가 아니라면 다음 이미지가 있어야 한다. */
		if (!last_image) {
			/* [한국어] 그런데 그 자리에 서명이 없다면 ROM 이 자기 구조를 잘못 보고한 것이다. */
			if (readw(image) != 0xAA55) {
				/* [한국어] 더 이상 이미지가 없다고 알리고 멈춘다. */
				pci_info(pdev, "No more image in the PCI ROM\n");
				break;
			}
		}
	/* [한국어] 길이가 0 이면(진행하지 않음) 무한 루프가 되므로 함께 검사한다.
	 * 마지막 이미지를 처리했으면 정상 종료다. */
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
/* [한국어]
 * pci_map_rom - 확장 ROM 을 매핑하고 실제 크기를 알려 준다
 *
 * @pdev: 대상 장치.
 * @size: 결과 크기를 담을 자리.
 * @return: 매핑된 주소, 실패하면 NULL.
 *
 * 비디오 카드의 VBIOS 를 읽는 것이 대표적인 쓰임이다.
 *
 * 네 단계가 순서대로 있고, 각 단계가 실패하면 앞 단계를 되돌린다.
 * 1. 자원이 배정되어 있지 않으면 배정한다. ROM 은 평소 꺼 두는 자원이라
 *    부팅 시점에 주소가 없을 수 있다.
 * 2. 디코딩을 켠다.
 * 3. ioremap 으로 매핑한다.
 * 4. 실제 크기를 재고, 그 결과로 *size 를 좁힌다.
 *
 * 되돌리기에서 눈여겨볼 것이 하나 있다 — 원래 켜져 있었다면 끄지 않는다.
 * IORESOURCE_ROM_ENABLE 이 그 사실을 기억하고 있어, 우리가 켠 것만 우리가
 * 끈다.
 *
 * 크기가 0 이면 실패로 다룬다. 유효한 이미지가 하나도 없다는 뜻이라
 * 매핑을 유지할 이유가 없다.
 *
 * 호출자는 반드시 pci_unmap_rom() 으로 짝을 맞춰야 한다.
 *
 * 실행 컨텍스트: 드라이버 probe 등. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 각 단계의 실패가 모두 NULL 로 합쳐지며, goto 로 앞 단계를
 * 차례로 되돌린다.
 *
 * 호출 체인:
 *   드라이버 / sysfs 의 rom 읽기 → [이 함수]
 *     → pci_assign_resource() → pci_enable_rom() → ioremap()
 *     → pci_get_rom_size()
 */
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	/* [한국어] 이 장치의 ROM 자원. */
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
	/* [한국어] 매핑할 물리 주소. loff_t 인 것은 파일 오프셋 타입을 재사용한 관례다. */
	loff_t start;
	/* [한국어] 매핑 결과 가상 주소. */
	void __iomem *rom;

	/* assign the ROM an address if it doesn't have one */
	/* [한국어] 자원이 아직 부모 자원 트리에 붙어 있지 않으면(주소가 배정되지 않았으면)
	 * 지금 배정한다. 위 영어 주석이 그 의도를 밝힌다.
	 * ROM BAR 는 부팅 시 자원 배정에서 흔히 건너뛰어지므로 이 처리가 필요하다. */
	if (res->parent == NULL && pci_assign_resource(pdev, PCI_ROM_RESOURCE))
		return NULL;

	/* [한국어] 배정된 물리 시작 주소. */
	start = pci_resource_start(pdev, PCI_ROM_RESOURCE);
	/* [한국어] 창 크기. 출력 인자로 호출자에게도 돌려준다. */
	*size = pci_resource_len(pdev, PCI_ROM_RESOURCE);
	/* [한국어] 크기가 0 이면 매핑할 것이 없다. */
	if (*size == 0)
		return NULL;

	/* Enable ROM space decodes */
	/* [한국어] ROM 디코딩을 켠다. 이 호출 전에는 그 주소를 읽어도 ROM 내용이 나오지 않는다. */
	if (pci_enable_rom(pdev))
		return NULL;

	/* [한국어] ROM 창을 커널 주소 공간에 매핑한다. 캐시 불가 속성으로 매핑되므로
	 * readw/readl 로 접근해야 한다. */
	rom = ioremap(start, *size);
	/* [한국어] 매핑 실패 검사. */
	if (!rom)
		/* [한국어] ROM 을 다시 꺼야 하므로 정리 구간으로 간다. */
		goto err_ioremap;

	/*
	 * Try to find the true size of the ROM since sometimes the PCI window
	 * size is much larger than the actual size of the ROM.
	 * True size is important if the ROM is going to be copied.
	 */
	/* [한국어] 위 영어 주석대로, PCI 창 크기가 실제 ROM 이미지보다 훨씬 클 수 있다.
	 * 이미지 체인을 따라가 진짜 크기를 구해 출력 인자를 갱신한다.
	 * 복사할 때 이 크기가 정확해야 쓸데없는 바이트를 함께 가져가지 않는다. */
	*size = pci_get_rom_size(pdev, rom, *size);
	/* [한국어] 유효한 이미지가 하나도 없었다면, */
	if (!*size)
		/* [한국어] 매핑을 풀고 ROM 도 다시 끈다. */
		goto invalid_rom;

	return rom;

invalid_rom:
	/* [한국어] 매핑 해제. 아래로 이어져 ROM 비활성화까지 수행한다. */
	iounmap(rom);
err_ioremap:
	/* restore enable if ioremap fails */
	/* [한국어] 위 영어 주석대로, 원래 켜져 있던 ROM 이면 그대로 두어야 한다.
	 * IORESOURCE_ROM_ENABLE 이 그 "원래 켜져 있었음"을 뜻하므로,
	 * 그 플래그가 없을 때만 우리가 켠 것으로 보고 되돌린다. */
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
/* [한국어]
 * pci_unmap_rom - pci_map_rom() 이 만든 매핑을 되돌린다
 *
 * @pdev: 대상 장치.
 * @rom: 매핑된 주소.
 *
 * 매핑을 풀고, 우리가 켠 디코딩이라면 끈다.
 *
 * 여기서도 IORESOURCE_ROM_ENABLE 을 본다. pci_map_rom() 의 오류 경로와
 * 같은 판단이며, 원래 켜져 있던 것을 우리가 끄면 그것을 켜 둔 쪽이
 * 곤란해진다.
 *
 * 실행 컨텍스트: 드라이버 정리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 / sysfs → [이 함수] → iounmap() → pci_disable_rom()
 */
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
	/* [한국어] 이 장치의 ROM 자원. 아래 플래그 검사에만 쓰인다. */
	struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];

	/* [한국어] 먼저 매핑을 푼다. */
	iounmap(rom);

	/* Disable again before continuing */
	/* [한국어] map 쪽과 같은 규칙이다 — 원래 켜져 있던 ROM 은 건드리지 않고,
	 * 우리가 켠 경우에만 다시 끈다. */
	if (!(res->flags & IORESOURCE_ROM_ENABLE))
		pci_disable_rom(pdev);
}
EXPORT_SYMBOL(pci_unmap_rom);
