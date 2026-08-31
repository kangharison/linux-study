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
/* [한국어] 이 아래는 컨트롤러 제어 레지스터의 위치를 계산하는 매크로들이다. */

#define PCI_IMAP(m)		(versatile_pci_base + ((m) * 4))
#define PCI_SMAP(m)		(versatile_pci_base + 0x14 + ((m) * 4))
#define PCI_SELFID		(versatile_pci_base + 0xc)

#define VP_PCI_DEVICE_ID		0x030010ee
#define VP_PCI_CLASS_ID			0x0b400000

static u32 pci_slot_ignore;

/* [한국어]
 * versatile_pci_slot_ignore - 커널 명령줄의 pci_slot_ignore= 를 파싱한다
 *
 * @str: '=' 뒤의 문자열. 쉼표로 구분된 슬롯 번호 목록이다.
 * @return: 항상 1. __setup 규약에서 1 은 '이 인자를 처리했다' 는 뜻이라,
 *          커널이 그것을 init 프로세스의 환경으로 넘기지 않는다.
 *
 * 지정된 슬롯을 열거에서 제외한다. 이 보드는 PCI 코어 자신이 슬롯 하나를
 * 차지하고 있어, 그것을 일반 장치로 열거하면 안 된다 -- 그 자동 제외는
 * probe 가 처리하고(아래 참조), 이 명령줄 인자는 그 밖에 사람이 손으로
 * 빼고 싶은 슬롯을 위한 것이다.
 *
 * get_option 이 한 번에 하나씩 숫자를 떼어 내며 str 을 전진시키므로,
 * while 루프가 목록 끝까지 돈다. 0~31 범위 밖은 오류로 찍고 무시한다 --
 * PCI 규약의 장치 번호가 5비트이기 때문이다.
 *
 * 결과는 전역 비트맵 pci_slot_ignore 에 누적된다. __setup 함수라 부팅 초기
 * (드라이버 프로브보다 훨씬 이전)에 실행된다.
 *
 * 실행 컨텍스트: 부팅 초기의 명령줄 파싱 단계.
 *
 * 호출 체인:
 *   커널 명령줄 파서 → __setup("pci_slot_ignore=") → [이 함수] → get_option
 */
static int __init versatile_pci_slot_ignore(char *str)
{
	int slot;

	while (get_option(&str, &slot)) {
		/* [한국어] PCI 규약의 장치 번호는 5비트(0~31)다. 그 밖의 값은 비트맵에 넣을 자리가 없다. */
		if ((slot < 0) || (slot > 31))
			/* [한국어] 잘못된 인자를 조용히 무시하면 사용자가 왜 슬롯이 제외되지 않는지 알 수 없다. */
			pr_err("Illegal slot value: %d\n", slot);
		/* [한국어] 범위 안이면 비트맵에 반영한다. */
		else
			pci_slot_ignore |= (1 << slot);
	/* [한국어] get_option 이 str 을 전진시키므로 목록 끝까지 돈다. */
	}
	return 1;
/* [한국어] __setup 규약에서 1 은 '이 인자를 처리했다' 는 뜻이다. 0 을 돌려주면 커널이
 * 이 문자열을 init 프로세스의 환경 변수로 넘긴다. */
}
__setup("pci_slot_ignore=", versatile_pci_slot_ignore);


/* [한국어]
 * versatile_map_bus - BDF 를 설정공간 주소로 바꾸고 제외 슬롯을 거른다
 *
 * @bus: 접근할 버스.
 * @devfn: 장치/함수 번호.
 * @offset: 설정공간 안의 바이트 오프셋.
 * @return: 접근할 가상 주소, 또는 제외된 슬롯이면 NULL.
 *
 * 이 컨트롤러는 설정공간 전체를 하나의 창에 평평하게 펼쳐 두므로, 주소
 * 계산이 시프트 세 번으로 끝난다: 버스는 16비트, devfn 은 8비트 왼쪽으로
 * 밀고 오프셋을 더한다. ECAM(버스 20비트, devfn 12비트)과 자릿수가 다른
 * 옛 방식이다.
 *
 * 그 앞의 pci_slot_ignore 검사가 이 함수의 또 다른 역할이다. 해당 비트가
 * 서 있으면 NULL 을 돌려주고, PCI 코어는 그것을 장치 없음(0xffffffff)으로
 * 처리한다. probe 가 PCI 코어 자신의 슬롯을 이 비트맵에 넣어 두므로,
 * 열거가 컨트롤러 자신을 장치로 잡는 일이 없다.
 *
 * versatile_cfg_base[1] 을 쓰는 점에 유의 -- [0] 은 probe 가 코어를 찾을 때만
 * 쓰는 별도의 창이다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_generic_config_read32/write → ops->map_bus → [이 함수]
 */
static void __iomem *versatile_map_bus(struct pci_bus *bus,
				       unsigned int devfn, int offset)
{
	unsigned int busnr = bus->number;

	if (pci_slot_ignore & (1 << PCI_SLOT(devfn)))
		/* [한국어] 제외된 슬롯. NULL 을 돌려주면 PCI 코어가 장치 없음(0xffffffff)으로 처리한다. */
		return NULL;

	return versatile_cfg_base[1] + ((busnr << 16) | (devfn << 8) | offset);
/* [한국어] 버스 16비트, devfn 8비트로 미는 옛 방식이다. ECAM(버스 20, devfn 12)과
 * 자릿수가 다르다. */
}

static struct pci_ops pci_versatile_ops = {
	/* [한국어] 제외 필터와 주소 계산을 이 콜백 하나가 맡는다. */
	.map_bus = versatile_map_bus,
	/* [한국어] read 만 32비트 전용 판(_read32)을 쓰는 점에 유의 -- 이 컨트롤러가 설정공간
	 * 읽기를 32비트 단위로만 받기 때문이다. 쓰기는 일반 판을 쓴다. */
	.read	= pci_generic_config_read32,
	.write	= pci_generic_config_write,
};

/* [한국어]
 * versatile_pci_probe - ARM Versatile 보드의 PCI 컨트롤러를 세운다
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, -ENOMEM/-EIO 또는 매핑 실패값.
 *
 * 이 보드의 특이한 점은 **PCI 컨트롤러 자신이 PCI 버스 위의 한 슬롯에
 * 꽂혀 있다** 는 것이다. 그래서 probe 가 자기 자신을 찾아내는 단계를 거친다.
 *
 * 단계:
 *  1. 브리지 객체 할당. DT 의 ranges 가 이때 bridge->windows 로 파싱된다.
 *  2. 세 개의 MMIO 창을 매핑한다 -- 인덱스 0 은 컨트롤러 제어 레지스터
 *     (PCI_IMAP/PCI_SMAP/PCI_SELFID 의 기준), 1 과 2 는 서로 다른 설정공간
 *     창이다. 2번만 devm_pci_remap_cfg_resource 를 쓰는데, 그 창이 실제
 *     설정 접근에 쓰이기 때문이다.
 *  3. MEM 윈도마다 IMAP/SMAP 쌍을 쓴다. 두 레지스터가 주소를 28비트
 *     오른쪽으로 민 값을 받는다 = 256MiB 단위로 창을 잡는다는 뜻이다.
 *     IMAP 은 PCI 쪽에서 들어올 주소를, SMAP 은 그것을 옮길 시스템 메모리
 *     주소(__pa(PAGE_OFFSET), 곧 RAM 의 시작)를 정한다.
 *  4. **PCI 코어 자기 자신 찾기**: 슬롯 0~31 을 훑으며 벤더/디바이스 ID 가
 *     VP_PCI_DEVICE_ID 이고 클래스가 VP_PCI_CLASS_ID 인 자리를 찾는다.
 *     (i << 11) 은 슬롯당 2KiB 간격이다. 못 찾으면 -EIO 로 실패한다 --
 *     컨트롤러가 응답하지 않는다는 뜻이므로 진행할 수 없다.
 *  5. 찾은 슬롯을 pci_slot_ignore 에 넣어 열거에서 제외하고, PCI_SELFID
 *     레지스터에 그 번호를 알려 준다.
 *  6. 자기 설정공간에서 COMMAND 에 MEMORY|MASTER|INVALIDATE 를 켜고,
 *     BAR0~2 를 모두 RAM 시작 주소로 맞춘다 -- 인바운드 DMA 가 시스템
 *     메모리에 닿게 하는 설정이다.
 *  7. INTERRUPT_LINE 을 0 으로 지운다.
 *  8. pci_add_flags(PCI_REASSIGN_ALL_BUS) -- 펌웨어가 배정한 버스 번호를
 *     믿지 않고 커널이 전부 다시 매기게 한다.
 *  9. ops 를 걸고 pci_host_probe 로 열거를 시작한다.
 *
 * 전역 변수 세 개(versatile_pci_base, versatile_cfg_base[], pci_slot_ignore)를
 * 쓰므로 **인스턴스가 하나뿐이라는 전제**가 깔려 있다. 이 보드에 컨트롤러가
 * 하나뿐이라 성립한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → devm_pci_alloc_host_bridge
 *     → devm_platform_ioremap_resource → pci_host_probe
 */
static int versatile_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	/* [한국어] 브리지 윈도를 순회할 항목 포인터. */
	struct resource_entry *entry;
	/* [한국어] i 는 슬롯 탐색용, myslot 은 찾은 슬롯 번호(-1 은 못 찾음), mem 은 IMAP/SMAP
	 * 창 번호다. **mem 이 1 부터 시작하는 점에 유의** -- 0번 창은 이 코드가
	 * 쓰지 않는다. */
	int i, myslot = -1, mem = 1;
	/* [한국어] 설정공간 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;
	/* [한국어] 찾은 슬롯의 설정공간 시작 주소. */
	void __iomem *local_pci_cfg_base;
	/* [한국어] PCI 코어가 요구하는 브리지 객체. */
	struct pci_host_bridge *bridge;

	bridge = devm_pci_alloc_host_bridge(dev, 0);
	/* [한국어] 브리지 할당 실패. */
	if (!bridge)
		/* [한국어] 아직 아무것도 잡지 않았으므로 바로 반환한다. */
		return -ENOMEM;

	versatile_pci_base = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 컨트롤러 제어 레지스터 창(인덱스 0) 매핑 실패. */
	if (IS_ERR(versatile_pci_base))
		/* [한국어] 이 창 없이는 IMAP/SMAP/SELFID 에 닿을 수 없다. */
		return PTR_ERR(versatile_pci_base);

	versatile_cfg_base[0] = devm_platform_ioremap_resource(pdev, 1);
	/* [한국어] 설정공간 창 [0] 매핑 실패. 이 창은 아래 슬롯 탐색에만 쓰인다. */
	if (IS_ERR(versatile_cfg_base[0]))
		/* [한국어] 오류 포인터에서 코드를 꺼내 올린다. */
		return PTR_ERR(versatile_cfg_base[0]);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	/* [한국어] 설정공간 창 [1] 은 실제 설정 접근에 쓰이므로 **remap_cfg_resource** 로
	 * 매핑한다 -- 설정공간에 맞는 메모리 속성을 붙여 준다. 위 두 창이 쓰는
	 * 일반 ioremap 과 다른 점이다. */
	versatile_cfg_base[1] = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(versatile_cfg_base[1]))
		/* [한국어] 오류 코드를 올린다. */
		return PTR_ERR(versatile_cfg_base[1]);

	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] MEM 윈도만 창을 잡는다. IO 나 BUS 항목은 이 하드웨어가 다루지 않는다. */
		if (resource_type(entry->res) == IORESOURCE_MEM) {
			/* [한국어] IMAP 은 PCI 쪽에서 들어올 주소 범위를 정한다. 28비트 오른쪽으로 미는 것은
			 * 이 레지스터가 **256MiB 단위**로 창을 잡기 때문이다. */
			writel(entry->res->start >> 28, PCI_IMAP(mem));
			/* [한국어] SMAP 은 그것을 옮길 시스템 메모리 주소를 정한다. __pa(PAGE_OFFSET) 은
			 * RAM 의 물리 시작 주소다 -- 즉 인바운드 DMA 를 RAM 으로 보낸다. */
			writel(__pa(PAGE_OFFSET) >> 28, PCI_SMAP(mem));
			/* [한국어] 다음 창 번호로. 창이 모자라면 어떻게 되는지 검사하지 않는다(상류 그대로). */
			mem++;
		/* [한국어] MEM 이 아닌 항목은 건너뛴다. */
		}
	}

	/*
	 * We need to discover the PCI core first to configure itself
	 * before the main PCI probing is performed
	 */
	for (i = 0; i < 32; i++) {
		if ((readl(versatile_cfg_base[0] + (i << 11) + PCI_VENDOR_ID) == VP_PCI_DEVICE_ID) &&
		    /* [한국어] 클래스/리비전까지 함께 봐야 확실하다. 벤더/디바이스 ID 만으로는 같은 칩이
		     * 다른 자리에 있을 때 구별할 수 없다. */
		    (readl(versatile_cfg_base[0] + (i << 11) + PCI_CLASS_REVISION) == VP_PCI_CLASS_ID)) {
			myslot = i;
			/* [한국어] 찾았으므로 더 볼 필요가 없다. */
			break;
		}
	}
	if (myslot == -1) {
		/* [한국어] 컨트롤러가 자기 자신을 찾지 못했다 -- 하드웨어가 응답하지 않는다는 뜻이다. */
		dev_err(dev, "Cannot find PCI core!\n");
		/* [한국어] 진행할 수 없으므로 -EIO 로 실패한다. */
		return -EIO;
	}
	/*
	 * Do not to map Versatile FPGA PCI device into memory space
	 */
	pci_slot_ignore |= (1 << myslot);

	dev_info(dev, "PCI core found (slot %d)\n", myslot);
/* [한국어] 찾은 슬롯 번호를 SELFID 레지스터에 알려 준다. 하드웨어가 자기 BDF 를
 * 알아야 요청자 ID 를 올바로 채운다. */

	writel(myslot, PCI_SELFID);
	/* [한국어] 슬롯당 2KiB(1 << 11) 간격으로 자기 설정공간의 시작을 계산한다.
	 * **여기서는 [1] 창을 쓴다** -- 위 탐색은 [0] 창으로 했다. */
	local_pci_cfg_base = versatile_cfg_base[1] + (myslot << 11);

	val = readl(local_pci_cfg_base + PCI_COMMAND);
	/* [한국어] MEMORY 로 메모리 디코딩을, MASTER 로 스스로 트랜잭션을 일으킬 권한을,
	 * INVALIDATE 로 캐시 라인 무효화 명령 사용을 연다. */
	val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE;
	/* [한국어] 갱신된 COMMAND 를 되쓴다. */
	writel(val, local_pci_cfg_base + PCI_COMMAND);

	/*
	 * Configure the PCI inbound memory windows to be 1:1 mapped to SDRAM
	 */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_0);
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_1);
	/* [한국어] BAR0~2 를 모두 RAM 시작 주소로 맞춘다. 셋 다 같은 값인 것은 이 하드웨어가
	 * 인바운드 창을 여러 BAR 에 걸쳐 표현하기 때문이다. */
	writel(__pa(PAGE_OFFSET), local_pci_cfg_base + PCI_BASE_ADDRESS_2);
/* [한국어] 이어서 인터럽트 라인을 정리한다. */

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
/* [한국어] 여기서 열거가 일어나고 하위 장치 드라이버가 붙는다. 이 호출이 돌아오면
 * 버스가 완전히 살아 있다. */

	return pci_host_probe(bridge);
/* [한국어] 이 드라이버에는 remove 콜백이 없다 -- 보드에 붙박이라 내려갈 일이 없다. */
}

static const struct of_device_id versatile_pci_of_match[] = {
	/* [한국어] ARM Versatile 보드의 PCI 컨트롤러. 이 드라이버가 지원하는 유일한 compatible. */
	{ .compatible = "arm,versatile-pci", },
	/* [한국어] 표의 끝. */
	{ },
};
MODULE_DEVICE_TABLE(of, versatile_pci_of_match);

static struct platform_driver versatile_pci_driver = {
	/* [한국어] 플랫폼 드라이버 등록 정보. */
	.driver = {
		/* [한국어] sysfs 에 나타날 이름. */
		.name = "versatile-pci",
		/* [한국어] 위 표를 걸어, DT 노드가 맞으면 probe 가 불린다. */
		.of_match_table = versatile_pci_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = versatile_pci_probe,
};
module_platform_driver(versatile_pci_driver);

MODULE_DESCRIPTION("Versatile PCI driver");
