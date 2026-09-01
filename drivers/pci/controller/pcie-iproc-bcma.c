// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Broadcom Corporation
 * Copyright (C) 2015 Hauke Mehrtens <hauke@hauke-m.de>
 */

/* [한국어] 커널 공통 정의(-ENOMEM 등 errno, 기본 매크로). */
/*
 * [한국어 설명] iProc PCIe 컨트롤러의 BCMA 버스 결합 계층 (pcie-iproc-bcma.c)
 *
 * === 파일의 역할 ===
 * Broadcom iProc 계열 SoC 의 PCIe 호스트 컨트롤러를, 그 SoC 내부 버스인
 * BCMA(Broadcom AMBA)에 붙여 주는 100줄 미만의 결합(glue) 드라이버다.
 * 컨트롤러를 실제로 다루는 코드 — 레지스터 초기화, 링크 학습 대기, config 공간
 * 접근, 인바운드/아웃바운드 주소 매핑, MSI 설정 — 는 한 줄도 여기에 없고 전부
 * 이웃 파일 pcie-iproc.c 에 모여 있다. 이 파일이 하는 일은 딱 세 가지다.
 * (1) BCMA 코어가 서술하는 것(이미 매핑된 레지스터 창 io_addr, 물리 주소 addr,
 * 슬레이브 주소 addr_s[0])을 공용 구조체 struct iproc_pcie 의 필드로 옮겨 담는 번역,
 * (2) 디바이스 트리가 없는 환경이라 메모리 윈도(addr_s[0] 부터 128MB)와 INTx 사상을
 * 코드로 직접 조립하는 일, (3) 하드웨어의 class 코드가 0x200 으로 잘못 박혀 있어
 * 커널이 루트 포트를 브리지로 인식하지 못하는 문제를 EARLY fixup 으로 교정하는 일.
 * 같은 컨트롤러를 디바이스 트리 기반 플랫폼에 붙이는 짝은 pcie-iproc-platform.c 이며,
 * 두 파일은 공용 코어를 사이에 두고 완전히 대칭이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층을 위에서부터 보면 PCI 코어(drivers/pci/probe.c 등) → 호스트 브리지 추상화
 * (pci_host_bridge) → iProc 공용 컨트롤러 코어(pcie-iproc.c) → 버스 결합 계층(이 파일)
 * → BCMA 버스 코어 → SoC 하드웨어 순이다. 즉 이 파일은 아래에서 두 번째, 하드웨어에
 * 가장 가까운 소프트웨어 층이면서도 정작 하드웨어를 직접 만지지는 않는 독특한 자리에 있다.
 * 진입은 두 방향이다. 정방향은 BCMA 버스가 코어를 열거해 id_table 이 맞으면
 * iproc_bcma_pcie_probe() 를 부르고, 그것이 iproc_pcie_setup()(pcie-iproc.c:1445) 에
 * 제어를 넘겨 PCI 버스 스캔까지 이어진다. 역방향은 열거 도중 PCI 코어가 이 파일이
 * 등록해 둔 두 콜백을 되부르는 것으로, 하나는 EARLY fixup(class 교정)이고 다른 하나는
 * map_irq(INTx → IRQ 사상)다. 실행 컨텍스트는 전부 프로세스 컨텍스트이며,
 * probe/remove 는 링크 학습과 하위 드라이버 probe/remove 를 유발하므로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 아래쪽 의존: include/linux/bcma/bcma.h — struct bcma_device(:267)의 io_addr/addr/
 * addr_s[] 필드, bcma_core_irq()(:487), bcma_get_drvdata/bcma_set_drvdata,
 * module_bcma_driver() 매크로(:324), 매칭 상수 BCMA_MANUF_BCM(0x4BF, :64)과
 * BCMA_CORE_NS_PCIEG2(0x501, :78). 이 파일이 PCI 드라이버가 아니라 BCMA 드라이버로
 * 등록된다는 사실이 이 의존 관계에 드러난다.
 * 옆쪽 의존: pcie-iproc.h 의 struct iproc_pcie 와 enum iproc_pcie_type,
 * 그리고 EXPORT 된 iproc_pcie_setup()/iproc_pcie_remove()(:113~114). 이 둘이
 * 공용 코어와 이 파일 사이의 유일한 함수 경계다.
 * 위쪽 의존: linux/pci.h 의 devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * pci_add_resource(), devm_request_pci_bus_resources(), DECLARE_PCI_FIXUP_EARLY,
 * PCI_VENDOR_ID_BROADCOM(0x14e4), PCI_CLASS_BRIDGE_PCI_NORMAL(0x060400).
 * 데이터 흐름: bdev->io_addr → pcie->base(레지스터 접근용 가상 주소),
 * bdev->addr → pcie->base_addr(주소 계산·로그용 물리 주소),
 * bdev->addr_s[0] → pcie->mem(하위 장치 BAR 를 배정할 128MB 메모리 풀) →
 * bridge->windows → PCI 코어의 자원 배정. 역방향으로는 dev->sysdata 에서 다시
 * struct iproc_pcie 를 꺼내 BCMA 코어까지 거슬러 올라가 IRQ 번호를 얻는다.
 * 공유 상태: bcma_set_drvdata() 로 BCMA 코어에 매달아 두는 struct iproc_pcie 포인터
 * 하나뿐이며, 이 파일에는 전역 변수도 정적 상태도 없다. 인스턴스마다 독립적이라
 * 동기화가 필요한 지점 자체가 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - bcma_pcie2_fixup_class(): 읽기 전용 class 필드가 0x200 으로 잘못 박힌 것을
 *   커널 쪽 사본에서만 0x060400 으로 교정한다. 반드시 EARLY 단계여야 하는 이유는
 *   코어가 class 로 브리지 여부를 판정하기 전에 끼어들어야 하기 때문이다.
 *   디바이스 ID 0x8011 과 0x8012 두 개에 각각 등록된다.
 * - iproc_bcma_pcie_map_irq(): slot 과 pin 을 무시하고 언제나 bcma_core_irq(bdev, 5)
 *   하나를 돌려준다. SoC 내장 브리지의 레거시 인터럽트 라인이 물리적으로 하나뿐이라
 *   모든 INTx 가 같은 IRQ 로 합류하기 때문이며, 따라서 이 IRQ 는 항상 공유 인터럽트다.
 * - iproc_bcma_pcie_probe(): 브리지 할당 → iproc_pcie 필드 채우기 → 128MB 메모리
 *   윈도 조립과 예약 → map_irq 등록 → iproc_pcie_setup() 위임. 모든 할당이 devm_
 *   계열이라 세 개의 오류 경로에 정리 코드가 전혀 없는 것이 특징이다.
 * - iproc_bcma_pcie_remove(): drvdata 를 되찾아 iproc_pcie_remove() 에 넘긴다.
 *   그 함수가 pci_stop_root_bus()/pci_remove_root_bus() 로 버스를 해체하고
 *   MSI 를 끄고 PHY 를 내린다(pcie-iproc.c:1528~1539).
 * - iproc_bcma_pcie_table / iproc_bcma_pcie_driver: 매칭 테이블과 드라이버 서술.
 *   리비전과 클래스를 와일드카드로 두어 NS PCIe Gen2 코어라면 리비전을 가리지 않는다.
 * - 이 파일에는 구조체 정의가 없다. 다루는 struct iproc_pcie 는 pcie-iproc.h 소유이고,
 *   struct bcma_device 와 struct bcma_driver 는 BCMA 서브시스템 소유다.
 */

#include <linux/kernel.h>
/* [한국어] PCI 코어 공개 API — struct pci_dev, pci_host_bridge, devm_pci_alloc_host_bridge(),
 * pci_add_resource(), DECLARE_PCI_FIXUP_EARLY, PCI_VENDOR_ID_BROADCOM,
 * PCI_CLASS_BRIDGE_PCI_NORMAL 이 모두 여기서 온다. */
#include <linux/pci.h>
/* [한국어] 이 파일을 로드 가능한 모듈로 만들기 위한 정의. MODULE_DEVICE_TABLE/MODULE_LICENSE 등. */
#include <linux/module.h>
/* [한국어] SZ_128M 등 크기 상수와 슬랩 할당자 선언. 여기서는 SZ_128M 때문에 필요하다. */
#include <linux/slab.h>
/* [한국어] PHY 서브시스템 헤더. struct iproc_pcie 에 struct phy 포인터 필드가 있어
 * pcie-iproc.h 를 포함하려면 그 타입의 선언이 먼저 있어야 한다.
 * 이 파일 자체는 PHY 를 직접 다루지 않는다(BCMA 계열은 PHY 를 쓰지 않는다). */
#include <linux/phy/phy.h>
/* [한국어] BCMA(Broadcom AMBA) 버스 서브시스템 헤더. struct bcma_device(:267),
 * struct bcma_driver, bcma_core_irq()(:487), bcma_get_drvdata/bcma_set_drvdata,
 * module_bcma_driver() 매크로(:324), BCMA_MANUF_BCM(:64)/BCMA_CORE_NS_PCIEG2(:78)
 * 상수가 모두 여기서 온다. 이 파일이 PCI 드라이버가 아니라 BCMA 드라이버인 이유가
 * 이 헤더에 담겨 있다 — 컨트롤러 자체가 PCI 버스가 아니라 SoC 내부 BCMA 버스에 붙어 있다. */
#include <linux/bcma/bcma.h>
/* [한국어] struct resource 와 IORESOURCE_MEM 플래그 정의. 아래에서 호스트 브리지의
 * 메모리 윈도를 손으로 조립할 때 쓴다. */
#include <linux/ioport.h>

/* [한국어] iProc PCIe 코어 공용 헤더(같은 디렉토리). struct iproc_pcie 정의,
 * enum iproc_pcie_type(IPROC_PCIE_PAXB_BCMA 포함), 그리고 이 파일이 마지막에 부르는
 * iproc_pcie_setup()/iproc_pcie_remove() 선언(:113~114)이 여기에 있다.
 * 실제 컨트롤러 로직은 전부 pcie-iproc.c 에 있고 이 파일은 BCMA 결합만 담당한다. */
#include "pcie-iproc.h"


/* NS: CLASS field is R/O, and set to wrong 0x200 value */
/* [한국어]
 * bcma_pcie2_fixup_class - 루트 포트의 잘못된 class 코드를 브리지 값으로 교정한다
 *
 * @dev: 방금 열거된 PCI 장치. PCI 코어가 fixup 을 실행할 때 넘겨 주며,
 *      여기서는 Northstar SoC 의 PCIe 루트 포트(벤더 0x14E4, 디바이스 0x8011/0x8012)다.
 *
 * 왜 필요한가: 위 영어 주석이 밝히듯 이 하드웨어의 class 필드는 읽기 전용인데
 * 값이 0x200 으로 잘못 박혀 있다. PCI 코어는 hdr_type 과 class 를 보고 그 장치가
 * 브리지인지 판단하고, 브리지가 아니면 하위 버스를 열거하지 않는다. 교정하지 않으면
 * 루트 포트 뒤에 연결된 장치가 통째로 보이지 않게 된다. config 쓰기로는 고칠 수
 * 없으므로, 커널이 들고 있는 struct pci_dev 사본의 class 필드만 바꾼다 —
 * 하드웨어는 그대로 두고 소프트웨어의 인식만 바로잡는 방식이다.
 *
 * 동작 과정: 한 줄이다. dev->class 에 PCI_CLASS_BRIDGE_PCI_NORMAL(0x060400)을 대입한다.
 * 값의 구성은 base class 06(bridge) / sub class 04(PCI-to-PCI) / prog-if 00(normal decode)이다.
 *
 * 실행 컨텍스트: PCI 열거 경로, 프로세스 컨텍스트. DECLARE_PCI_FIXUP_EARLY 로 등록되어
 * pci_setup_device() 직후 — 즉 코어가 class 를 근거로 브리지 여부를 판정하기 "전에" —
 * 실행된다. HEADER 나 FINAL 단계였다면 이미 잘못된 판정이 끝난 뒤라 교정이 무의미하다.
 * 이 단계 선택이 fixup 전체의 핵심이다. 장치마다 한 번만 불리므로 재진입 문제는 없다.
 *
 * 에러 경로: 없다. 반환값도 없고 실패할 여지도 없다.
 *
 * 호출 체인:
 *   pci_scan_single_device() → pci_setup_device()
 *     → pci_fixup_device(pci_fixup_early, dev) → [bcma_pcie2_fixup_class]
 */
static void bcma_pcie2_fixup_class(struct pci_dev *dev)
{
	/* [한국어] 루트 포트의 class 코드를 PCI-to-PCI 브리지(0x060400)로 강제 교정한다.
	 * PCI_CLASS_BRIDGE_PCI_NORMAL 은 base class 06(bridge), sub class 04(PCI-to-PCI),
	 * programming interface 00(normal decode)을 합친 값이다.
	 * 이 대입이 없으면 PCI 코어가 이 장치를 브리지로 인식하지 못해 하위 버스를
	 * 열거하지 않는다 — 즉 링크 뒤의 카드가 통째로 보이지 않게 된다.
	 * 위 영어 주석대로 하드웨어의 class 필드가 읽기 전용이라 config 쓰기로는
	 * 고칠 수 없고, 커널 쪽 struct pci_dev 사본만 바꾸는 방식을 택했다. */
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
}
/* [한국어] 디바이스 ID 0x8011 에 대해 위 fixup 을 EARLY 단계로 등록한다.
 * EARLY 는 pci_setup_device() 직후, 즉 코어가 hdr_type/class 를 보고 브리지 여부를
 * 판단하기 전에 실행되는 단계다. 그보다 늦은 HEADER/FINAL 단계였다면 이미 잘못된
 * class 로 열거가 끝난 뒤라 교정이 소용없다 — 단계 선택이 이 fixup 의 핵심이다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x8011, bcma_pcie2_fixup_class);
/* [한국어] 같은 fixup 을 디바이스 ID 0x8012 에도 등록한다. 같은 결함을 가진 리비전이
 * 둘이라는 뜻이며, 두 줄로 나눈 이유는 매크로가 ID 하나씩만 받기 때문이다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x8012, bcma_pcie2_fixup_class);

/* [한국어]
 * iproc_bcma_pcie_map_irq - 하위 장치의 레거시 INTx 를 이 SoC 의 IRQ 번호로 사상한다
 *
 * @dev: IRQ 를 배정받을 하위 PCI 장치. const 인 이유는 이 콜백이 장치 상태를
 *      바꾸지 않고 조회만 하기 때문이다.
 * @slot: 장치의 슬롯(디바이스) 번호. 사용하지 않는다.
 * @pin: INTA~INTD 중 어느 핀을 쓰는지(1~4). 역시 사용하지 않는다.
 * @return: 커널 IRQ 번호. 슬롯과 핀에 관계없이 언제나 같은 값이다.
 *
 * 왜 필요한가: PCI 코어는 하위 장치의 pdev->irq 를 정할 때 호스트 브리지가 등록해 둔
 * map_irq 콜백에 의존한다. 디바이스 트리를 쓰는 플랫폼이라면 인터럽트 맵을 파싱하지만,
 * BCMA 는 SoC 내부 버스라 그런 서술이 없고 대신 BCMA 코어가 자기 인터럽트 배열을 갖는다.
 * 이 함수가 그 배열에서 PCIe 몫을 꺼내 주는 다리 역할을 한다.
 *
 * 동작 과정:
 *   1) dev->sysdata 에서 호스트 컨트롤러 private(struct iproc_pcie)을 꺼낸다.
 *      이 포인터는 iproc_pcie_setup() 이 브리지를 등록할 때 심어 둔 것이다.
 *   2) pcie->dev(= BCMA 코어의 struct device)를 container_of 로 감싸
 *      struct bcma_device 로 되돌린다. PCI 세계에서 BCMA 세계로 건너가는 지점이다.
 *   3) bcma_core_irq(bdev, 5) 로 그 코어의 인터럽트 배열 중 5번을 얻어 그대로 돌려준다.
 *
 * slot 과 pin 을 무시하는 것이 이 구현의 특징이다. SoC 내장 PCIe 브리지라
 * 레거시 인터럽트 라인이 물리적으로 하나뿐이어서, 링크 뒤의 어떤 장치가 어떤 핀을
 * 어서트하든 결국 같은 IRQ 로 합류한다. 그래서 이 IRQ 는 항상 공유 인터럽트로
 * 다뤄져야 하며, 핸들러는 자기 장치가 올린 것인지 상태 레지스터로 직접 확인해야 한다.
 * 숫자 5 는 계산 결과가 아니라 이 코어의 인터럽트 배열에서 PCIe 가 차지하는 자리를
 * 가리키는 하드웨어 상수다.
 *
 * 실행 컨텍스트: PCI 열거 경로, 프로세스 컨텍스트. pci_assign_irq() 가 장치마다 한 번씩 부른다.
 *
 * 에러 경로: 없다. bcma_core_irq() 가 유효한 IRQ 를 못 찾으면 0 을 돌려줄 수 있고,
 * 그 경우 PCI 코어는 그 장치에 레거시 인터럽트가 없는 것으로 취급한다.
 *
 * 호출 체인:
 *   pci_bus_add_devices() → pcibios_add_device / pci_assign_irq()
 *     → bridge->map_irq == [iproc_bcma_pcie_map_irq] → bcma_core_irq() (BCMA 버스 코드)
 */
static int iproc_bcma_pcie_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	/* [한국어] PCI 코어가 pci_dev 마다 보관해 둔 호스트 컨트롤러 private 포인터를 꺼낸다.
	 * sysdata 는 iproc_pcie_setup() 이 브리지를 등록할 때 심어 둔 값이다. */
	struct iproc_pcie *pcie = dev->sysdata;
	/* [한국어] struct iproc_pcie 의 dev 필드는 BCMA 코어의 struct device 를 가리키므로,
	 * container_of 로 그것을 감싸고 있는 struct bcma_device 로 되돌린다.
	 * PCI 세계에서 BCMA 세계로 건너가는 유일한 지점이다. */
	struct bcma_device *bdev = container_of(pcie->dev, struct bcma_device, dev);

	/* [한국어] 이 컨트롤러의 BCMA 코어에 배정된 인터럽트 번호 중 5번을 돌려준다.
	 * 슬롯이나 핀(pin) 값을 전혀 보지 않는다는 점이 중요하다 — 링크 뒤에 무엇이
	 * 꽂혀 있든, INTA~INTD 중 무엇을 쓰든 모두 같은 IRQ 하나로 합류한다.
	 * SoC 내장 PCIe 브리지라 레거시 인터럽트 라인이 물리적으로 하나뿐이기 때문이며,
	 * 숫자 5 는 이 코어의 인터럽트 배열에서 PCIe 가 차지하는 자리를 뜻하는 하드웨어 상수다. */
	return bcma_core_irq(bdev, 5);
}

/* [한국어]
 * iproc_bcma_pcie_probe - BCMA 버스에 나타난 iProc PCIe 코어를 초기화한다
 *
 * @bdev: BCMA 버스가 열거한 PCIe Gen2 코어. 이 함수가 쓰는 필드는 넷이다 —
 *      bdev->dev(devm 수명 기준이자 로그 대상), bdev->io_addr(BCMA 가 이미
 *      ioremap 해 둔 레지스터 가상 주소), bdev->addr(같은 창의 물리 주소),
 *      bdev->addr_s[0](이 코어에 배정된 첫 슬레이브 주소 = PCIe 메모리 창의 시작).
 * @return: 0 = 성공. -ENOMEM = 브리지 할당 실패 또는 레지스터 창 부재.
 *      그 밖의 음수 = 자원 예약 실패 또는 iproc_pcie_setup() 이 돌려준 오류.
 *
 * 왜 필요한가: iProc PCIe 컨트롤러는 SoC 에 따라 붙는 버스가 다르다. 디바이스 트리
 * 기반 플랫폼에서는 pcie-iproc-platform.c 가, Broadcom Northstar 처럼 BCMA 버스에
 * 붙는 경우에는 이 파일이 결합을 담당한다. 컨트롤러 로직 자체는 pcie-iproc.c 하나에
 * 모여 있고, 이 함수는 "BCMA 코어 서술 -> struct iproc_pcie 채우기"라는 번역만 한다.
 * 그래서 코드가 30줄 남짓이고, 레지스터를 직접 두드리는 줄이 하나도 없다.
 *
 * 동작 과정:
 *   1) devm_pci_alloc_host_bridge() 로 브리지와 private 영역을 한 번에 할당한다.
 *      이후 모든 할당이 devm_ 계열이라 오류 경로에 정리 코드가 전혀 없다.
 *   2) pci_host_bridge_priv() 로 struct iproc_pcie 를 꺼내 dev / type / base /
 *      base_addr 를 채운다. type 을 IPROC_PCIE_PAXB_BCMA 로 지정하면 pcie-iproc.c 의
 *      iproc_pcie_rev_init()(:1386)이 그에 맞는 레지스터 오프셋 테이블을 고른다.
 *   3) 레지스터 창(io_addr)이 없으면 즉시 중단한다.
 *   4) addr_s[0] 부터 128MB 를 메모리 윈도로 손수 조립해 브리지 윈도 목록에 넣고,
 *      devm_request_pci_bus_resources() 로 커널 자원 트리에 예약한다.
 *      I/O 포트 창은 만들지 않는다 — 이 브리지가 지원하지 않기 때문이다.
 *   5) map_irq 콜백을 걸고, bcma_set_drvdata() 로 remove 가 되찾을 수 있게 해 둔 뒤,
 *      iproc_pcie_setup() 에 제어를 넘긴다. 레지스터 초기화·링크 학습·버스 스캔은
 *      전부 거기서 일어나므로, 이 함수의 성공 여부는 결국 그 반환값이다.
 *
 * 실행 컨텍스트: BCMA 버스의 드라이버 바인딩 경로, 프로세스 컨텍스트. 코어마다 한 번
 * 불리며 잠들 수 있다(iproc_pcie_setup() 이 링크 학습을 기다린다).
 *
 * 에러 경로: 세 지점 모두 정리 없이 곧장 return 한다. 할당이 전부 devm_ 이라
 * 드라이버 코어가 되감아 주기 때문이며, 이것이 devm API 를 쓰는 가장 큰 실익이다.
 *
 * 호출 체인:
 *   BCMA 버스 코어가 매칭 → bcma_driver.probe == [iproc_bcma_pcie_probe]
 *     → devm_pci_alloc_host_bridge() / pci_add_resource()
 *     → devm_request_pci_bus_resources()
 *     → iproc_pcie_setup() (pcie-iproc.c:1445) → 레지스터 초기화 → PCI 버스 스캔
 */
static int iproc_bcma_pcie_probe(struct bcma_device *bdev)
{
	/* [한국어] BCMA 코어의 struct device. devm_ 계열 할당의 수명 기준이자, 로그 출력 대상이다. */
	struct device *dev = &bdev->dev;
	/* [한국어] 이 컨트롤러 인스턴스의 상태 전체를 담을 iProc 공용 구조체 포인터. */
	struct iproc_pcie *pcie;
	/* [한국어] PCI 코어에 등록할 호스트 브리지 객체. iproc_pcie 는 이 객체 뒤에 붙어 함께 할당된다. */
	struct pci_host_bridge *bridge;
	/* [한국어] devm_request_pci_bus_resources() 와 iproc_pcie_setup() 의 반환값을 받을 변수. */
	int ret;

	/* [한국어] 호스트 브리지와 그 뒤에 이어붙일 private 영역(sizeof(*pcie))을 한 번에 할당한다.
	 * devm_ 접두사이므로 dev 가 사라질 때 자동 해제되어, 오류 경로에서 따로 free 할
	 * 필요가 없다 — 아래 return 들이 정리 코드 없이 곧장 빠져나가는 이유다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		/* [한국어] 메모리 부족은 유일하게 이 함수가 스스로 만드는 오류 중 하나다. */
		return -ENOMEM;

	/* [한국어] 위에서 함께 할당된 private 영역의 시작 주소를 얻는다. 별도 kmalloc 없이
	 * 브리지와 수명을 공유하는 것이 이 관용구의 목적이다. */
	pcie = pci_host_bridge_priv(bridge);

	/* [한국어] 이후 pcie-iproc.c 의 공용 코드가 로그와 devm 할당에 쓸 device 포인터를 심는다. */
	pcie->dev = dev;

	/* [한국어] 컨트롤러 변종을 BCMA 결합형(PAXB_BCMA)으로 지정한다. pcie-iproc.c 의
	 * iproc_pcie_rev_init()(:1386)이 이 값을 보고 레지스터 오프셋 테이블
	 * iproc_pcie_reg_paxb_bcma 를 고른다. 다른 변종과 달리 has_apb_err_disable 이나
	 * ob/ib 매핑 설정을 켜지 않는다 — BCMA 판은 그 기능이 없다는 뜻이다. */
	pcie->type = IPROC_PCIE_PAXB_BCMA;
	/* [한국어] BCMA 코어가 이미 매핑해 둔 MMIO 가상 주소를 그대로 가져다 쓴다.
	 * 다른 iProc 변종이 devm_ioremap_resource() 로 직접 매핑하는 것과 대조적으로,
	 * 여기서는 BCMA 버스 코드가 코어 등록 시 이미 ioremap 을 끝내 놓았다. */
	pcie->base = bdev->io_addr;
	/* [한국어] BCMA 코어에 MMIO 창이 없으면 레지스터에 접근할 방법이 없으므로 진행할 수 없다. */
	if (!pcie->base) {
		/* [한국어] 어떤 이유로 실패했는지 관리자에게 알린다. */
		dev_err(dev, "no controller registers\n");
		/* [한국어] [상류 코드 관찰, 수정하지 않음] 실패 원인은 "레지스터 창이 없다"이므로
		 * 의미상 -ENODEV 가 더 맞지만 상류는 -ENOMEM 을 돌려준다. 바로 위 로그가
		 * "no controller registers" 인 것과 어긋난다. 그대로 둔다. */
		return -ENOMEM;
	}

	/* [한국어] 같은 레지스터 창의 물리 주소. 가상 주소(base)와 별개로 필요한 이유는,
	 * pcie-iproc.c 가 아웃바운드 매핑 창을 계산하거나 오류 로그에 물리 주소를
	 * 찍을 때 CPU 가상 주소가 아니라 버스/물리 주소를 써야 하기 때문이다. */
	pcie->base_addr = bdev->addr;

	/* [한국어] 호스트 브리지가 하위 PCI 장치에 나눠 줄 메모리 윈도의 시작 물리 주소.
	 * BCMA 코어의 슬레이브 주소 배열 addr_s[0] 을 그대로 쓴다 — SoC 가 이 코어에
	 * 할당해 둔 첫 번째 주소 영역이 곧 PCIe 메모리 공간이라는 하드웨어 약속이다. */
	pcie->mem.start = bdev->addr_s[0];
	/* [한국어] 윈도의 끝 주소. 크기를 128MB 로 못박고 -1 을 해 포함 구간(inclusive)으로 만든다.
	 * struct resource 의 end 는 마지막 유효 바이트를 가리키는 규약이므로 -1 이 필수다.
	 * 128MB 는 디바이스 트리나 레지스터에서 읽어 온 값이 아니라 이 SoC 계열의
	 * 고정 크기를 코드에 박아 둔 것이다. */
	pcie->mem.end = bdev->addr_s[0] + SZ_128M - 1;
	/* [한국어] /proc/iomem 등에 표시될 이름. 진단 목적 외의 기능은 없다. */
	pcie->mem.name = "PCIe MEM space";
	/* [한국어] 이 자원이 메모리 공간임을 표시한다. I/O 포트 창은 아예 만들지 않는데,
	 * 이 SoC 의 PCIe 브리지가 레거시 I/O 공간을 지원하지 않기 때문이다. */
	pcie->mem.flags = IORESOURCE_MEM;
	/* [한국어] 조립한 자원을 브리지의 윈도 목록에 등록한다. 이 목록이 나중에
	 * PCI 코어가 하위 장치의 BAR 를 배정할 때 사용할 주소 풀이 된다. */
	pci_add_resource(&bridge->windows, &pcie->mem);
	/* [한국어] 위에서 목록에 넣은 모든 자원을 커널 자원 트리에 정식으로 예약한다.
	 * 다른 드라이버가 같은 물리 주소 범위를 이미 점유하고 있으면 여기서 실패한다.
	 * devm_ 계열이라 해제도 자동이다. */
	ret = devm_request_pci_bus_resources(dev, &bridge->windows);
	/* [한국어] 자원 충돌은 치명적이다 — 주소가 겹친 채로 진행하면 엉뚱한 장치를 건드리게 된다. */
	if (ret)
		/* [한국어] 자원 코드가 준 오류를 그대로 위로 전달한다. devm 덕분에 정리할 것이 없다. */
		return ret;

	/* [한국어] 레거시 INTx 를 IRQ 번호로 바꾸는 콜백을 등록한다. PCI 코어가 각 장치의
	 * pdev->irq 를 정할 때 이 함수를 부른다. */
	pcie->map_irq = iproc_bcma_pcie_map_irq;

	/* [한국어] BCMA 코어에 이 컨트롤러 객체를 매달아 둔다. remove 콜백이 인자로 받는 것은
	 * bcma_device 뿐이므로, 이렇게 심어 두지 않으면 나중에 iproc_pcie 를 되찾을 수 없다. */
	bcma_set_drvdata(bdev, pcie);

	/* [한국어] 공용 코어에 제어권을 넘긴다. 여기서 레지스터 초기화, 링크 학습 대기,
	 * PCI 버스 스캔과 장치 등록이 모두 일어난다. 반환값을 그대로 돌려주므로
	 * 이 함수의 성공/실패는 결국 공용 코어의 판단이다. */
	return iproc_pcie_setup(pcie, &bridge->windows);
}

/* [한국어]
 * iproc_bcma_pcie_remove - BCMA 코어가 사라질 때 PCIe 컨트롤러를 정리한다
 *
 * @bdev: 제거되는 BCMA 코어. probe 에서 bcma_set_drvdata() 로 심어 둔
 *      struct iproc_pcie 를 되찾는 열쇠로만 쓰인다.
 *
 * 왜 필요한가: probe 가 등록한 PCI 호스트 브리지와 그 아래 열거된 장치들은
 * devm 이 자동으로 되돌려 주지 않는다 — PCI 버스에서 장치를 떼는 것은 메모리 해제가
 * 아니라 순서가 있는 해체 절차이기 때문이다. 그 절차를 공용 코어에 위임하는 것이
 * 이 함수의 전부다.
 *
 * 동작 과정:
 *   1) bcma_get_drvdata() 로 probe 가 심어 둔 컨트롤러 객체를 되찾는다.
 *   2) iproc_pcie_remove() 에 넘겨 PCI 장치 제거와 브리지 등록 해제를 맡긴다.
 * 이 파일에서 직접 해제하는 자원은 하나도 없다. 브리지 할당과 자원 예약이 모두
 * devm_ 계열이라 드라이버 코어가 이 함수 종료 후 알아서 되돌린다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로, 프로세스 컨텍스트. 하위 장치들의 remove()
 * 콜백이 연쇄적으로 불리므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값이 void 인 것은 제거가 실패할 수 없는 단방향 작업이기 때문이다.
 *
 * 호출 체인:
 *   BCMA 버스 코어의 언바인드 → bcma_driver.remove == [iproc_bcma_pcie_remove]
 *     → iproc_pcie_remove() (pcie-iproc.c:1528) → pci_stop_root_bus / pci_remove_root_bus
 */
static void iproc_bcma_pcie_remove(struct bcma_device *bdev)
{
	/* [한국어] probe 에서 bcma_set_drvdata() 로 심어 둔 컨트롤러 객체를 되찾는다. */
	struct iproc_pcie *pcie = bcma_get_drvdata(bdev);

	/* [한국어] 공용 코어의 제거 경로 — PCI 버스에서 장치를 떼고 브리지 등록을 해제한다.
	 * 이 파일에서 따로 해제할 것은 없다. probe 의 할당이 전부 devm_ 계열이라
	 * 드라이버 코어가 알아서 되돌려 주기 때문이다. */
	iproc_pcie_remove(pcie);
}

/* [한국어] 이 드라이버가 다룰 BCMA 코어를 알려 주는 매칭 테이블. */
static const struct bcma_device_id iproc_bcma_pcie_table[] = {
	/* [한국어] 제조사 = 브로드컴(BCMA_MANUF_BCM, 0x4BF), 코어 ID = NS PCIe Gen2
	 * (BCMA_CORE_NS_PCIEG2, 0x501), 리비전과 클래스는 BCMA_ANY_REV(0xFF)/
	 * BCMA_ANY_CLASS(0xFF) 로 와일드카드다. 즉 리비전을 가리지 않고 모두 받는다. */
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_NS_PCIEG2, BCMA_ANY_REV, BCMA_ANY_CLASS),
	/* [한국어] 테이블의 끝을 알리는 빈 항목. BCMA 버스 코드가 이 0 항목을 만나면 순회를 멈춘다. */
	{},
};
/* [한국어] 빌드 시 이 테이블을 모듈 메타데이터로 뽑아 내, 해당 코어가 나타나면
 * udev/커널이 이 모듈을 자동으로 적재할 수 있게 한다. */
MODULE_DEVICE_TABLE(bcma, iproc_bcma_pcie_table);

static struct bcma_driver iproc_bcma_pcie_driver = {
	/* [한국어] 드라이버 이름. KBUILD_MODNAME 을 써서 파일명과 자동으로 일치시킨다. */
	.name		= KBUILD_MODNAME,
	/* [한국어] 위에서 정의한 매칭 테이블 연결. */
	.id_table	= iproc_bcma_pcie_table,
	/* [한국어] 코어가 나타났을 때 불릴 진입점. */
	.probe		= iproc_bcma_pcie_probe,
	/* [한국어] 코어가 사라질 때 불릴 정리 함수. */
	.remove		= iproc_bcma_pcie_remove,
};
/* [한국어] module_init()/module_exit() 보일러플레이트를 대신하는 매크로.
 * bcma_driver_register()/bcma_driver_unregister() 호출로 전개된다
 * (include/linux/bcma/bcma.h:324). PCI 드라이버가 아니라 BCMA 드라이버로
 * 등록된다는 사실이 이 한 줄에 드러난다. */
module_bcma_driver(iproc_bcma_pcie_driver);

/* [한국어] modinfo 에 표시될 작성자. */
MODULE_AUTHOR("Hauke Mehrtens");
/* [한국어] modinfo 에 표시될 모듈 설명. */
MODULE_DESCRIPTION("Broadcom iProc PCIe BCMA driver");
/* [한국어] 라이선스 선언. GPL 계열이어야 pcie-iproc.c 가 EXPORT_SYMBOL 로 내보낸
 * iproc_pcie_setup()/iproc_pcie_remove() 를 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
