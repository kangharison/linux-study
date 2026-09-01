// SPDX-License-Identifier: GPL-2.0
/*
 *  pci-rcar-gen2: internal PCI bus support
 *
 * Copyright (C) 2013 Renesas Solutions Corp.
 * Copyright (C) 2013 Cogent Embedded, Inc.
 *
 * Author: Valentine Barshak <valentine.barshak@cogentembedded.com>
 */

/* [한국어] udelay() — 리셋 유지 시간. */
/*
 * [한국어 설명] R-Car Gen2 의 내장 AHB-PCI 브리지 드라이버 (pci-rcar-gen2.c)
 *
 * === 파일의 역할 ===
 * Renesas R-Car Gen2 SoC 안에 들어 있는 AHB-PCI 브리지를 다룬다. 파일 상단의
 * 상류 주석이 스스로를 "internal PCI bus support" 라 부르는 그대로,
 * 바깥으로 나가는 PCIe 슬롯이 아니라 SoC 내부에 고정된 USB 호스트
 * (OHCI/EHCI) 두 개를 PCI 장치로 보이게 하는 것이 전부다.
 * 같은 디렉터리의 pcie-rcar.c 계열과 이름이 비슷하지만 완전히 다른 물건이다.
 * 그쪽은 PCIe 이고 이쪽은 전통적인 PCI 이며, 그쪽은 host/EP 두 드라이버가
 * 공용 코드를 나눠 쓰는 반면 이쪽은 파일 하나로 끝난다.
 * 이 하드웨어에서 가장 특이한 것은 **config 접근 방식** 이다. 주소를
 * 계산해 읽는 것이 아니라, 창 제어 레지스터(AHBPCI_WIN1_CTR)를 먼저 고쳐
 * 접근 대상을 고른 뒤 고정된 주소를 읽는다. 그래서 map_bus 콜백인
 * rcar_pci_cfg_base() 가 주소만 돌려주는 순수 함수가 아니라 레지스터를
 * 쓰는 부작용을 갖는다.
 * 브리지가 USB 호스트와 한 몸이라는 사실도 곳곳에 드러난다. 리셋과 클록,
 * 그리고 PCI-AHB 창 크기까지 RCAR_USBCTR_REG 하나에 모여 있고, probe 가
 * reg 항목을 두 개 요구하는데 두 번째가 USB 호스트의 레지스터 구간이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCI 코어 바로 아래, 하드웨어 위에 홀로 서 있다.
 * DesignWare 같은 공용 IP 코어를 쓰지 않으므로 중간 계층이 없다.
 *   builtin_platform_driver → rcar_pci_probe()
 *     → devm_pci_alloc_host_bridge()(브리지와 사설 상태를 한 덩어리로)
 *     → reg[0] 매핑, reg[1](USB 메모리) 확인, IRQ 확보
 *     → rcar_pci_setup() 으로 하드웨어 기동
 *     → pci_host_probe() 로 코어에 넘긴다
 * 그 뒤 동작 중에는 config 접근만 남는다.
 *   pci_read_config_*() → PCI 코어 → pci_generic_config_read [access.c]
 *     → ops->map_bus = rcar_pci_cfg_base() (창을 고치고 주소를 준다)
 * 디버그 빌드에서는 오류 인터럽트 경로가 하나 더 있다 —
 * rcar_pci_err_irq() 가 상태 레지스터를 읽어 오류를 로그로 남기고 지운다.
 * 실행 컨텍스트는 셋이다. probe 와 setup 은 프로세스 컨텍스트이고,
 * map_bus 는 코어가 pci_lock 을 쥔 채 부르므로 잠들 수 없으며,
 * 오류 핸들러는 하드 IRQ 다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어의 config 접근 경로(access.c 의 pci_generic_config_read/write)와
 * pci_host_probe(). 이 드라이버가 고유하게 채우는 것은 map_bus 하나뿐이고
 * 읽기·쓰기는 표준 함수를 그대로 쓴다.
 * 옆쪽: drivers/pci/pci.h 의 devm_pci_alloc_host_bridge() /
 * pci_host_bridge_priv() / pci_host_bridge_from_priv(). 브리지와 사설 상태를
 * 한 덩어리로 할당하는 방식이라, setup 이 사설 포인터에서 브리지를 되찾을
 * 수 있다.
 * 아래쪽: ioread32/iowrite32(readl/writel 이 아닌 옛 스타일), udelay,
 * 런타임 PM, 그리고 devm_request_irq(디버그 빌드에서만).
 * DT 바인딩: compatible 다섯 개(r8a7790/r8a7791/r8a7794/rcar-gen2/rzn1)가
 * 같은 IP 를 공유한다. 선택 속성으로 dma-ranges 를 보아 PCI-AHB 창 1 의
 * 크기를 정하며, 없으면 0x40000000 에서 1GB 를 기본값으로 쓴다.
 * 공유 상태: struct rcar_pci 하나(필드 다섯). 전역 변수는 없고 상수 표
 * 둘(pci_ops, of_device_id)만 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rcar_pci_cfg_base(): map_bus 콜백. 세 가지를 걸러 낸다 — 루트 버스가
 *   아니거나 기능 번호가 0 이 아니면, 슬롯이 2 를 넘으면(내장 USB 둘뿐이다),
 *   그리고 슬롯 0(브리지 자신)의 0x40 이상이면 NULL 이다. 그 뒤 창 제어
 *   레지스터를 고쳐 대상을 고르고 주소를 돌려준다. 주소 계산이 아니라
 *   레지스터 조작이 대상을 정한다는 점이 이 하드웨어의 핵심이다.
 * - rcar_pci_setup(): 하드웨어 기동 전체. 순서가 촘촘하다 — 전원 차단 해제와
 *   리셋 → 4us 대기 → 리셋 해제와 창 크기 설정을 한 번의 쓰기로 →
 *   AHB 모드 → 중재기 → PCI-AHB 창 1(DMA 경로) → AHB-PCI 창 2(USB 레지스터)
 *   → 창 1 을 config 용으로 되돌리고 BAR 둘을 씀 → Command 활성화 →
 *   인터럽트 허용. 통째로 쓰는 곳과 읽기-수정-쓰기 하는 곳이 나뉘어 있는데,
 *   초기 설정이면 전자, 보존할 값이 있으면 후자다.
 * - rcar_pci_err_irq() / rcar_pci_setup_errirq(): CONFIG_PCI_DEBUG 전용.
 *   꺼진 빌드에서는 무동작 스텁이 쓰여 호출부가 #ifdef 없이 유지된다.
 *   등록 실패를 오류로 다루지 않는 것도 부가 기능이기 때문이다.
 * - rcar_pci_probe(): reg 항목 두 개(브리지 레지스터, USB 메모리)를 요구하고,
 *   두 번째에 64KB 정렬 검사를 건다 — 창 2 설정이 하위 16비트를 쓰지 않기
 *   때문이다. pci_add_flags(PCI_REASSIGN_ALL_BUS) 로 펌웨어 설정을 버린다.
 * - struct rcar_pci: 필드 다섯. mem_res 는 값 복사, cfg_res 는 포인터라
 *   다루는 방식이 갈린다.
 * - 레지스터 상수들: 모두 RCAR_AHBPCI_PCICOM_OFFSET(0x800) 기준의 상대
 *   위치로 정의된다. RCAR_USBCTR_REG 하나에 USB 리셋·PCI 클록·PLL 리셋·
 *   창 크기가 모여 있는 것이 이 브리지가 USB 호스트와 한 몸임을 보여 준다.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - probe 가 IRQ 를 얻지 못하면 실패하는데, 그 IRQ 는 CONFIG_PCI_DEBUG
 *   빌드에서만 쓰인다. 디버그가 꺼진 빌드에서도 IRQ 를 요구하는 셈이다.
 * - rcar_pci_setup() 이 pm_runtime_get_sync() 로 올린 참조를 놓는 경로가
 *   이 파일 어디에도 없다. remove 함수가 없고 내장 드라이버라 뗄 수 없으므로
 *   실무적으로 문제가 되지는 않는다.
 * - rcar_pci_cfg_base() 가 창 제어 레지스터를 쓰고 주소를 돌려주는 사이에
 *   잠금이 없다. 코어의 pci_lock 이 config 접근을 직렬화한다는 전제다.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * udelay(4) 가 만족시키는 리셋 유지 시간, 그리고 AHB 버스 모드와 중재기
 * 비트들의 정확한 의미는 Renesas 문서에만 있어 확인할 수 없었다.
 * 이름이 알려 주는 것과 코드가 하는 일만 적었다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. 이 브리지 아래에는 내장 USB 호스트 두 개만 있고 슬롯이
 * 없으므로 NVMe SSD 를 꽂을 수 없다. R-Car Gen2 에서 NVMe 를 쓰려면
 * PCIe 쪽인 pcie-rcar-host.c 를 거친다.
 * 다만 PCI-AHB 창 1 의 크기 설정은 개념적으로 낯익다. 그 창이 장치의 DMA 가
 * 시스템 메모리에 닿는 통로이고, 크기가 곧 장치가 접근할 수 있는 메모리
 * 범위의 상한이라는 점은 NVMe 컨트롤러의 DMA 에도 그대로 적용되는 구조다.
 */

#include <linux/delay.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/init.h>
/* [한국어] devm_request_irq() 와 IRQF_SHARED, irqreturn_t. */
#include <linux/interrupt.h>
/* [한국어] ioread32()/iowrite32(). readl/writel 대신 io 계열을 쓰는 옛 스타일이다. */
#include <linux/io.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/of_address.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/of_pci.h>
/* [한국어] PCI_SLOT()/PCI_FUNC(), pci_host_probe(), pci_generic_config_read/write(). */
#include <linux/pci.h>
/* [한국어] platform_get_resource(), devm_platform_get_and_ioremap_resource(). */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_enable()/get_sync(). */
#include <linux/pm_runtime.h>
/* [한국어] SZ_1G 등 크기 상수. */
#include <linux/sizes.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/slab.h>

/* [한국어] devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * pci_host_bridge_from_priv(), pci_add_flags(). */
#include "../pci.h"

/* AHB-PCI Bridge PCI communication registers */
/* [한국어] AHB-PCI 브리지의 통신 레지스터 영역이 시작하는 오프셋. 아래 모든
 * 레지스터 상수가 이 값을 더해 정의된다. */
#define RCAR_AHBPCI_PCICOM_OFFSET	0x800

/* [한국어] PCI 에서 AHB 로 향하는 창 1 의 제어 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCIAHB_WIN1_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x00)
/* [한국어] 창 2 의 제어 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCIAHB_WIN2_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x04)
/* [한국어] 프리페치 없음. */
#define RCAR_PCIAHB_PREFETCH0		0x0
/* [한국어] 4바이트 프리페치. */
#define RCAR_PCIAHB_PREFETCH4		0x1
/* [한국어] 8바이트. */
#define RCAR_PCIAHB_PREFETCH8		0x2
/* [한국어] 16바이트. 아래 setup 이 창 1 에 최대값을 쓴다. */
#define RCAR_PCIAHB_PREFETCH16		0x3

/* [한국어] AHB 에서 PCI 로 향하는 창 1 의 제어 레지스터. config 접근 때마다
 * 이 레지스터를 바꿔 대상을 고르는 것이 이 하드웨어의 방식이다.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_AHBPCI_WIN1_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x10)
/* [한국어] 창 2 의 제어 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_AHBPCI_WIN2_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x14)
/* [한국어] 이 창을 메모리 접근용으로 쓴다는 표시. */
#define RCAR_AHBPCI_WIN_CTR_MEM		(3 << 1)
/* [한국어] config 접근용으로 쓴다는 표시. 같은 창을 두 용도로 갈아 끼운다. */
#define RCAR_AHBPCI_WIN_CTR_CFG		(5 << 1)
/* [한국어] 브리지 자신(호스트)의 config 를 가리킨다. */
#define RCAR_AHBPCI_WIN1_HOST		(1 << 30)
/* [한국어] 하위 장치의 config 를 가리킨다. 30번과 31번 비트가 대상을 가른다. */
#define RCAR_AHBPCI_WIN1_DEVICE		(1 << 31)

/* [한국어] 인터럽트 허용 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCI_INT_ENABLE_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x20)
/* [한국어] 인터럽트 상태 레지스터. 허용과 상태가 같은 비트 배치를 쓴다.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCI_INT_STATUS_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x24)
/* [한국어] 타깃 어보트를 냈다. */
#define RCAR_PCI_INT_SIGTABORT		(1 << 0)
/* [한국어] 재시도 어보트를 냈다. */
#define RCAR_PCI_INT_SIGRETABORT	(1 << 1)
/* [한국어] 원격 어보트를 받았다. */
#define RCAR_PCI_INT_REMABORT		(1 << 2)
/* [한국어] 패리티 오류. */
#define RCAR_PCI_INT_PERR		(1 << 3)
/* [한국어] 시스템 오류를 냈다. */
#define RCAR_PCI_INT_SIGSERR		(1 << 4)
/* [한국어] 응답 오류. */
#define RCAR_PCI_INT_RESERR		(1 << 5)
/* [한국어] 창 1 오류. */
#define RCAR_PCI_INT_WIN1ERR		(1 << 12)
/* [한국어] 창 2 오류. */
#define RCAR_PCI_INT_WIN2ERR		(1 << 13)
/* [한국어] INTA. */
#define RCAR_PCI_INT_A			(1 << 16)
/* [한국어] INTB. 내장 USB 호스트 둘이 각각 하나씩 쓴다. */
#define RCAR_PCI_INT_B			(1 << 17)
/* [한국어] 전원 관리 이벤트. */
#define RCAR_PCI_INT_PME		(1 << 19)
/* [한국어] 위 오류 비트들을 한데 묶은 마스크. 디버그용 오류 핸들러가 이 마스크로
 * 판정하고, 같은 마스크로 지운다. */
#define RCAR_PCI_INT_ALLERRORS (RCAR_PCI_INT_SIGTABORT		| \
				RCAR_PCI_INT_SIGRETABORT	| \
				RCAR_PCI_INT_REMABORT		| \
				RCAR_PCI_INT_PERR		| \
				RCAR_PCI_INT_SIGSERR		| \
				RCAR_PCI_INT_RESERR		| \
				RCAR_PCI_INT_WIN1ERR		| \
				RCAR_PCI_INT_WIN2ERR)

/* [한국어] AHB 버스 제어 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_AHB_BUS_CTR_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x30)
/* [한국어] 마스터 모드의 HTRANS 허용. */
#define RCAR_AHB_BUS_MMODE_HTRANS	(1 << 0)
/* [한국어] 바이트 버스트 허용. */
#define RCAR_AHB_BUS_MMODE_BYTE_BURST	(1 << 1)
/* [한국어] 쓰기 증가 허용. */
#define RCAR_AHB_BUS_MMODE_WR_INCR	(1 << 2)
/* [한국어] 버스 요청 허용. */
#define RCAR_AHB_BUS_MMODE_HBUS_REQ	(1 << 7)
/* [한국어] 슬레이브 모드의 READY 제어. */
#define RCAR_AHB_BUS_SMODE_READYCTR	(1 << 17)
/* [한국어] 위 다섯을 한데 묶은 값. setup 이 이 레지스터에 통째로 쓴다 —
 * 읽기-수정-쓰기가 아닌 이유는 초기 설정이라 보존할 값이 없기 때문이다. */
#define RCAR_AHB_BUS_MODE		(RCAR_AHB_BUS_MMODE_HTRANS |	\
					RCAR_AHB_BUS_MMODE_BYTE_BURST |	\
					RCAR_AHB_BUS_MMODE_WR_INCR |	\
					RCAR_AHB_BUS_MMODE_HBUS_REQ |	\
					RCAR_AHB_BUS_SMODE_READYCTR)

/* [한국어] USB 제어 레지스터. 이 브리지가 USB 호스트와 한 몸이라 리셋과 클록,
 * 그리고 PCI-AHB 창 크기까지 이 하나에 모여 있다.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_USBCTR_REG			(RCAR_AHBPCI_PCICOM_OFFSET + 0x34)
/* [한국어] USB 호스트 리셋. */
#define RCAR_USBCTR_USBH_RST		(1 << 0)
/* [한국어] PCI 클록 마스크. */
#define RCAR_USBCTR_PCICLK_MASK		(1 << 1)
/* [한국어] PLL 리셋. */
#define RCAR_USBCTR_PLL_RST		(1 << 2)
/* [한국어] 직접 전원 차단 상태. setup 이 이것을 지워 깨운다. */
#define RCAR_USBCTR_DIRPD		(1 << 8)
/* [한국어] PCI-AHB 창 2 활성화. */
#define RCAR_USBCTR_PCIAHB_WIN2_EN	(1 << 9)
/* [한국어] 창 1 크기 256MB. */
#define RCAR_USBCTR_PCIAHB_WIN1_256M	(0 << 10)
/* [한국어] 512MB. */
#define RCAR_USBCTR_PCIAHB_WIN1_512M	(1 << 10)
/* [한국어] 1GB. */
#define RCAR_USBCTR_PCIAHB_WIN1_1G	(2 << 10)
/* [한국어] 2GB. */
#define RCAR_USBCTR_PCIAHB_WIN1_2G	(3 << 10)
/* [한국어] 위 네 값을 덮는 마스크. setup 이 크기를 다시 정하기 전에 이 자리를 지운다. */
#define RCAR_USBCTR_PCIAHB_WIN1_MASK	(3 << 10)

/* [한국어] PCI 중재기 제어 레지스터.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCI_ARBITER_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x40)
/* [한국어] 요청선 0 허용. */
#define RCAR_PCI_ARBITER_PCIREQ0	(1 << 0)
/* [한국어] 요청선 1 허용. */
#define RCAR_PCI_ARBITER_PCIREQ1	(1 << 1)
/* [한국어] 우회 모드. */
#define RCAR_PCI_ARBITER_PCIBP_MODE	(1 << 12)

/* [한국어] 리비전 레지스터. setup 이 부팅 로그에 찍는다.
 * 레지스터 오프셋. 모두 PCICOM 영역(0x800) 기준의 상대 위치다. */
#define RCAR_PCI_UNIT_REV_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x48)

struct rcar_pci {
	/* [한국어] 진단 메시지와 devm 할당의 기준이 될 device.
	 * 설정자: probe.  읽는 자: 이 파일의 거의 모든 함수.
	 * 값 범위: 유효한 device 포인터.  동기화: probe 후 불변. */
	struct device *dev;
	/* [한국어] 매핑된 브리지 레지스터 창.
	 * 설정자: probe 의 devm_platform_get_and_ioremap_resource().
	 * 읽는 자: config 접근과 setup 의 모든 레지스터 조작.
	 * 값 범위: 유효한 __iomem 포인터.  동기화: probe 후 불변. */
	void __iomem *reg;
	/* [한국어] 내장 USB 호스트(OHCI/EHCI)의 레지스터가 놓일 메모리 자원. **복사본** 이라
	 *   아래 cfg_res 가 포인터인 것과 다르다.
	 * 설정자: probe 가 platform_get_resource() 결과를 값으로 복사한다.
	 * 읽는 자: setup 의 AHB-PCI 창 2 설정.
	 * 값 범위: 하위 16비트가 0 이어야 한다 — probe 가 그것을 확인한다.
	 * 동기화: probe 후 불변. */
	struct resource mem_res;
	/* [한국어] 브리지 레지스터 창의 자원. 이쪽은 포인터로 들고 있다.
	 * 설정자: probe.  읽는 자: setup 이 통신 영역의 물리 주소를 계산할 때.
	 * 값 범위: 유효한 resource 포인터.  동기화: probe 후 불변. */
	struct resource *cfg_res;
	/* [한국어] 오류 인터럽트 번호.
	 * 설정자: probe 의 platform_get_irq().
	 * 읽는 자: rcar_pci_setup_errirq() — CONFIG_PCI_DEBUG 일 때만 쓰인다.
	 * 값 범위: 유효한 IRQ 번호. 음수면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	int irq;
};

/* PCI configuration space operations */
static void __iomem *rcar_pci_cfg_base(struct pci_bus *bus, unsigned int devfn,
				       int where)
{
	/* [한국어] 코어가 sysdata 에 넣어 둔 이 드라이버의 상태. */
	struct rcar_pci *priv = bus->sysdata;
	/* [한국어] 슬롯 번호와 창 제어 값. */
	int slot, val;

	/* [한국어] 루트 버스가 아니거나 기능 번호가 0 이 아니면 대상이 아니다. 이 브리지
	 * 아래에는 다중 기능 장치가 없다. */
	if (!pci_is_root_bus(bus) || PCI_FUNC(devfn))
		return NULL;

	/* Only one EHCI/OHCI device built-in */
	/* [한국어] 옆의 영어 주석대로 내장 EHCI/OHCI 두 개뿐이므로, */
	slot = PCI_SLOT(devfn);
	/* [한국어] 슬롯 2 를 넘으면 장치가 없다. */
	if (slot > 2)
		return NULL;

	/* bridge logic only has registers to 0x40 */
	/* [한국어] 옆의 영어 주석대로 브리지 자신(슬롯 0)의 레지스터는 0x40 까지뿐이다. */
	if (slot == 0x0 && where >= 0x40)
		return NULL;

	/* [한국어] 슬롯 0 이 아니면 하위 장치를, 슬롯 0 이면 브리지 자신을 가리키게 한다. */
	val = slot ? RCAR_AHBPCI_WIN1_DEVICE | RCAR_AHBPCI_WIN_CTR_CFG :
		     RCAR_AHBPCI_WIN1_HOST | RCAR_AHBPCI_WIN_CTR_CFG;

	/* [한국어] 창 제어 레지스터를 바꿔 접근 대상을 고른다. 이것이 이 하드웨어의 config
	 * 접근 방식으로, 주소가 아니라 **창의 설정** 이 대상을 정한다.
	 * [상류 코드 관찰] 이 쓰기와 아래 실제 접근 사이에 잠금이 없다. 코어의
	 * pci_lock 이 config 접근을 직렬화한다는 전제 위에 있다. */
	iowrite32(val, priv->reg + RCAR_AHBPCI_WIN1_CTR_REG);
	/* [한국어] 주소를 계산해 돌려준다. 슬롯을 1 비트 밀어 0x100 을 곱하는데,
	 * 슬롯 1 과 2 가 각각 0 과 0x100 오프셋이 되는 셈이다. */
	return priv->reg + (slot >> 1) * 0x100 + where;
}

/* [한국어] 디버그 빌드에서만 오류 인터럽트를 단다. 옆의 영어 주석이 그 조건을 밝힌다. */
#ifdef CONFIG_PCI_DEBUG
/* if debug enabled, then attach an error handler irq to the bridge */

static irqreturn_t rcar_pci_err_irq(int irq, void *pw)
{
	/* [한국어] 등록 시 넘겨 둔 상태. */
	struct rcar_pci *priv = pw;
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = priv->dev;
	/* [한국어] 인터럽트 상태를 읽는다. */
	u32 status = ioread32(priv->reg + RCAR_PCI_INT_STATUS_REG);

	/* [한국어] 오류 비트 중 하나라도 서 있으면, */
	if (status & RCAR_PCI_INT_ALLERRORS) {
		/* [한국어] 어떤 상태였는지 남기고, */
		dev_err(dev, "error irq: status %08x\n", status);

		/* clear the error(s) */
		/* [한국어] 옆의 영어 주석대로 그 오류들을 지운다. 읽은 값을 마스크와 AND 해서
		 * 쓰므로, 오류가 아닌 비트(INTA/INTB/PME)는 건드리지 않는다. */
		iowrite32(status & RCAR_PCI_INT_ALLERRORS,
			  priv->reg + RCAR_PCI_INT_STATUS_REG);
		/* [한국어] 우리 인터럽트였다고 답한다. */
		return IRQ_HANDLED;
	}

	/* [한국어] 오류가 아니면 남의 인터럽트다. 공유 인터럽트이므로 IRQ_NONE 을 돌려주어야
	 * 커널이 다른 핸들러를 시도한다. */
	return IRQ_NONE;
}

/* [한국어]
 * rcar_pci_setup_errirq - 디버그 빌드에서만 오류 인터럽트 핸들러를 단다
 *
 * @priv: 이 드라이버의 상태.
 *
 * CONFIG_PCI_DEBUG 가 켜진 빌드에서만 실체가 있고, 꺼져 있으면 바로 아래의
 * 무동작 스텁이 쓰인다. 호출부인 rcar_pci_setup() 을 #ifdef 로 어지럽히지
 * 않으려는 흔한 배치다.
 *
 * IRQ 등록이 실패해도 오류로 반환하지 않고 로그만 남긴 채 물러난다.
 * 디버그용 부가 기능이라 그것 없이도 브리지는 정상 동작하기 때문이다.
 * 그래서 반환값 자체가 없다.
 *
 * 허용 레지스터를 읽기-수정-쓰기로 다루는 것도 이유가 있다. 호출자인
 * rcar_pci_setup() 이 바로 앞에서 INTA/INTB/PME 를 켜 두었으므로,
 * 통째로 쓰면 그것이 지워진다.
 *
 * 실행 컨텍스트: probe 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 로그만 남긴다.
 *
 * 호출 체인:
 *   rcar_pci_setup() → [이 함수] → devm_request_irq() → ioread32/iowrite32()
 */
static void rcar_pci_setup_errirq(struct rcar_pci *priv)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = priv->dev;
	/* [한국어] 결과. */
	int ret;
	/* [한국어] 인터럽트 허용 레지스터 값. */
	u32 val;

	/* [한국어] 공유 인터럽트로 오류 핸들러를 건다. */
	ret = devm_request_irq(dev, priv->irq, rcar_pci_err_irq,
			       IRQF_SHARED, "error irq", priv);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고 물러난다. 오류로 반환하지 않는 것이 중요한데, 디버그용 부가
		 * 기능이라 그것 없이도 브리지는 정상 동작하기 때문이다. */
		dev_err(dev, "cannot claim IRQ for error handling\n");
		return;
	}

	/* [한국어] 허용 레지스터를 읽어, */
	val = ioread32(priv->reg + RCAR_PCI_INT_ENABLE_REG);
	/* [한국어] 모든 오류 비트를 더하고, */
	val |= RCAR_PCI_INT_ALLERRORS;
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 아래 setup 이 켜 둔 INTA/INTB/PME 를 보존한다. */
	iowrite32(val, priv->reg + RCAR_PCI_INT_ENABLE_REG);
}
#else
/* [한국어] 디버그가 꺼진 빌드의 무동작 스텁. 호출부를 #ifdef 로 감싸지 않아도 되게 한다. */
static inline void rcar_pci_setup_errirq(struct rcar_pci *priv) { }
#endif

/* PCI host controller setup */
static void rcar_pci_setup(struct rcar_pci *priv)
{
	/* [한국어] 사설 영역에서 그것을 품은 호스트 브리지를 되찾는다. devm_pci_alloc_host_bridge()
	 * 가 둘을 한 덩어리로 할당하기 때문에 가능한 변환이다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(priv);
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = priv->dev;
	/* [한국어] 레지스터 창. */
	void __iomem *reg = priv->reg;
	/* [한국어] DMA 범위 항목. */
	struct resource_entry *entry;
	/* [한국어] 창 크기, CPU 쪽 주소, PCI 쪽 주소. */
	unsigned long window_size;
	unsigned long window_addr;
	unsigned long window_pci;
	/* [한국어] 레지스터 값. */
	u32 val;

	/* [한국어] DT 의 dma-ranges 에서 첫 메모리 항목을 찾는다. */
	entry = resource_list_first_type(&bridge->dma_ranges, IORESOURCE_MEM);
	/* [한국어] 없으면, */
	if (!entry) {
		/* [한국어] 기본값을 쓴다. 0x40000000 은 이 SoC 의 DRAM 시작 주소다. */
		window_addr = 0x40000000;
		window_pci = 0x40000000;
		/* [한국어] 기본 창 크기 1GB. */
		window_size = SZ_1G;
	} else {
		/* [한국어] 있으면 CPU 쪽 시작 주소와, */
		window_addr = entry->res->start;
		/* [한국어] 오프셋을 뺀 PCI 쪽 주소, */
		window_pci = entry->res->start - entry->offset;
		/* [한국어] 그리고 크기를 가져온다. */
		window_size = resource_size(entry->res);
	}

	/* [한국어] 런타임 PM 을 켜고, */
	pm_runtime_enable(dev);
	/* [한국어] 참조를 올려 하드웨어를 깨운다. 아래 레지스터 접근이 가능해지는 시점이다. */
	pm_runtime_get_sync(dev);

	/* [한국어] 리비전을 읽어, */
	val = ioread32(reg + RCAR_PCI_UNIT_REV_REG);
	/* [한국어] 부팅 로그에 남긴다. */
	dev_info(dev, "PCI: revision %x\n", val);

	/* Disable Direct Power Down State and assert reset */
	/* [한국어] 옆의 영어 주석대로 직접 전원 차단 상태를 풀고, */
	val = ioread32(reg + RCAR_USBCTR_REG) & ~RCAR_USBCTR_DIRPD;
	/* [한국어] USB 호스트와 PLL 을 리셋한다. */
	val |= RCAR_USBCTR_USBH_RST | RCAR_USBCTR_PLL_RST;
	/* [한국어] 쓴다. */
	iowrite32(val, reg + RCAR_USBCTR_REG);
	/* [한국어] 리셋 유지 시간 4마이크로초. 하드웨어 요구 사항으로, 이 트리에 근거
	 * 문서가 없다. */
	udelay(4);

	/* De-assert reset and reset PCIAHB window1 size */
	/* [한국어] 옆의 영어 주석대로 리셋을 풀면서 창 1 크기 필드도 함께 지운다.
	 * 아래에서 다시 채우기 위해서다. */
	val &= ~(RCAR_USBCTR_PCIAHB_WIN1_MASK | RCAR_USBCTR_PCICLK_MASK |
		 RCAR_USBCTR_USBH_RST | RCAR_USBCTR_PLL_RST);

	/* Setup PCIAHB window1 size */
	/* [한국어] DT 가 알려 준 창 크기를 레지스터 인코딩으로 바꾼다. */
	switch (window_size) {
	/* [한국어] 2GB 면, */
	case SZ_2G:
		/* [한국어] 그 인코딩을 더한다. */
		val |= RCAR_USBCTR_PCIAHB_WIN1_2G;
		break;
	/* [한국어] 1GB. */
	case SZ_1G:
		val |= RCAR_USBCTR_PCIAHB_WIN1_1G;
		break;
	/* [한국어] 512MB. */
	case SZ_512M:
		val |= RCAR_USBCTR_PCIAHB_WIN1_512M;
		break;
	/* [한국어] 그 밖의 크기는 지원하지 않는다. */
	default:
		/* [한국어] 경고를 남기고, */
		pr_warn("unknown window size %ld - defaulting to 256M\n",
			window_size);
		/* [한국어] 256MB 로 낮춘 뒤, */
		window_size = SZ_256M;
		/* [한국어] 아래 case 로 흘러내린다. fallthrough 를 명시해 컴파일러 경고를 막는다. */
		fallthrough;
	/* [한국어] 256MB. */
	case SZ_256M:
		val |= RCAR_USBCTR_PCIAHB_WIN1_256M;
		break;
	}
	/* [한국어] 완성된 값을 쓴다. 리셋 해제와 창 크기 설정이 이 한 번의 쓰기로 함께
	 * 이루어진다. */
	iowrite32(val, reg + RCAR_USBCTR_REG);

	/* Configure AHB master and slave modes */
	/* [한국어] AHB 마스터·슬레이브 모드를 통째로 쓴다. 초기 설정이라 보존할 값이 없다. */
	iowrite32(RCAR_AHB_BUS_MODE, reg + RCAR_AHB_BUS_CTR_REG);

	/* Configure PCI arbiter */
	/* [한국어] 중재기 설정은 읽기-수정-쓰기다. 다른 비트를 보존해야 하기 때문이다. */
	val = ioread32(reg + RCAR_PCI_ARBITER_CTR_REG);
	/* [한국어] 두 요청선과 우회 모드를 켠다. */
	val |= RCAR_PCI_ARBITER_PCIREQ0 | RCAR_PCI_ARBITER_PCIREQ1 |
	       RCAR_PCI_ARBITER_PCIBP_MODE;
	/* [한국어] 쓴다. */
	iowrite32(val, reg + RCAR_PCI_ARBITER_CTR_REG);

	/* PCI-AHB mapping */
	/* [한국어] PCI 에서 AHB 로 향하는 창 1 을 설정한다. DRAM 주소에 최대 프리페치를
	 * 더해 쓰며, 장치가 DMA 로 시스템 메모리에 닿는 경로가 된다. */
	iowrite32(window_addr | RCAR_PCIAHB_PREFETCH16,
		  reg + RCAR_PCIAHB_WIN1_CTR_REG);

	/* AHB-PCI mapping: OHCI/EHCI registers */
	/* [한국어] AHB 에서 PCI 로 향하는 창 2 를 내장 USB 호스트의 레지스터 구간으로 정한다. */
	val = priv->mem_res.start | RCAR_AHBPCI_WIN_CTR_MEM;
	/* [한국어] 쓴다. */
	iowrite32(val, reg + RCAR_AHBPCI_WIN2_CTR_REG);

	/* Enable AHB-PCI bridge PCI configuration access */
	/* [한국어] 창 1 을 브리지 자신의 config 접근용으로 되돌린다. 아래 두 번의 BAR 쓰기가
	 * 이 설정을 전제로 한다. */
	iowrite32(RCAR_AHBPCI_WIN1_HOST | RCAR_AHBPCI_WIN_CTR_CFG,
		  reg + RCAR_AHBPCI_WIN1_CTR_REG);
	/* Set PCI-AHB Window1 address */
	/* [한국어] 브리지의 BAR1 에 PCI 쪽 창 주소를 쓴다. 프리페치 가능으로 표시한다. */
	iowrite32(window_pci | PCI_BASE_ADDRESS_MEM_PREFETCH,
		  reg + PCI_BASE_ADDRESS_1);
	/* Set AHB-PCI bridge PCI communication area address */
	/* [한국어] 통신 영역의 물리 주소를 계산한다. */
	val = priv->cfg_res->start + RCAR_AHBPCI_PCICOM_OFFSET;
	/* [한국어] BAR0 에 쓴다. 브리지 자신이 PCI 공간에서 그 주소로 보이게 하는 것이다. */
	iowrite32(val, reg + PCI_BASE_ADDRESS_0);

	/* [한국어] Command 를 읽어, */
	val = ioread32(reg + PCI_COMMAND);
	/* [한국어] SERR·패리티 검사·메모리 디코딩·버스 마스터를 켜고, */
	val |= PCI_COMMAND_SERR | PCI_COMMAND_PARITY |
	       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	/* [한국어] 쓴다. 버스 마스터가 있어야 장치가 DMA 를 낼 수 있다. */
	iowrite32(val, reg + PCI_COMMAND);

	/* Enable PCI interrupts */
	/* [한국어] INTA·INTB·PME 를 허용한다. 오류 비트는 여기서 켜지 않는데,
	 * 디버그 빌드에서만 rcar_pci_setup_errirq() 가 따로 켜기 때문이다. */
	iowrite32(RCAR_PCI_INT_A | RCAR_PCI_INT_B | RCAR_PCI_INT_PME,
		  reg + RCAR_PCI_INT_ENABLE_REG);

	/* [한국어] 디버그 빌드면 오류 핸들러를 단다. 아니면 무동작 스텁이다. */
	rcar_pci_setup_errirq(priv);
}

static struct pci_ops rcar_pci_ops = {
	/* [한국어] 주소 계산이 곧 창 설정인 이 하드웨어의 특성상 map_bus 가 부작용을 갖는다. */
	.map_bus = rcar_pci_cfg_base,
	/* [한국어] 읽기는 표준 함수를 그대로 쓴다. */
	.read	= pci_generic_config_read,
	/* [한국어] 쓰기도 마찬가지. */
	.write	= pci_generic_config_write,
};

/* [한국어]
 * rcar_pci_probe - 자원을 확인하고 브리지를 기동한 뒤 코어에 넘긴다
 *
 * @pdev: 매치된 플랫폼 장치.
 * @return: 0 = 성공, -ENOMEM, -ENODEV, -EINVAL, 또는 IRQ/매핑 오류.
 *
 * reg 항목을 두 개 쓴다는 점이 이 드라이버의 특징이다. 첫 번째는 브리지
 * 자신의 레지스터 창이고, 두 번째는 내장 USB 호스트(OHCI/EHCI)의 메모리
 * 구간이다. 이 브리지가 USB 호스트와 한 몸인 하드웨어라 그렇다.
 *
 * 두 번째 자원에 64KB 정렬 검사가 붙어 있다. AHB-PCI 창 2 설정이 하위 16비트를
 * 쓰지 않으므로, 정렬되지 않은 주소를 받으면 조용히 어긋난 구간을 열게 된다.
 *
 * 자원을 다루는 방식이 둘로 갈린다. mem_res 는 값으로 복사하고 cfg_res 는
 * 포인터로 들고 있는데, 전자는 start 만 쓰고 후자도 start 만 쓰는 것을 보면
 * 일관성보다는 코드가 자란 흔적으로 보인다.
 *
 * pci_add_flags(PCI_REASSIGN_ALL_BUS) 는 펌웨어가 설정해 둔 버스 번호를 믿지
 * 않고 커널이 처음부터 다시 배정하겠다는 선언이다.
 *
 * [상류 코드 관찰] IRQ 가 없으면 probe 를 실패시키는데, 그 IRQ 는
 * CONFIG_PCI_DEBUG 빌드에서만 쓰인다. 디버그가 꺼진 빌드에서도 IRQ 를
 * 요구하는 셈이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 할당이 devm_ 이라 되돌릴 것이 없다. rcar_pci_setup() 이
 * 런타임 PM 참조를 올리지만 이 함수에는 그것을 놓는 경로가 없다 —
 * remove 함수가 없고 내장 드라이버라 뗄 수 없기 때문이다.
 *
 * 호출 체인:
 *   builtin_platform_driver → 플랫폼 버스 매치 → [이 함수]
 *     → devm_pci_alloc_host_bridge() → devm_platform_get_and_ioremap_resource()
 *     → platform_get_resource() → platform_get_irq()
 *     → rcar_pci_setup() → pci_host_probe()
 */
static int rcar_pci_probe(struct platform_device *pdev)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 브리지 레지스터 자원과 USB 메모리 자원. */
	struct resource *cfg_res, *mem_res;
	/* [한국어] 드라이버 상태. */
	struct rcar_pci *priv;
	/* [한국어] 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 매핑된 레지스터 창. */
	void __iomem *reg;

	/* [한국어] 호스트 브리지와 사설 영역을 한 덩어리로 할당한다. 따로 kzalloc 하지
	 * 않는 방식이라 위 pci_host_bridge_from_priv() 가 성립한다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*priv));
	/* [한국어] 실패하면, */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 그 덩어리 안의 사설 영역을 가리킨다. */
	priv = pci_host_bridge_priv(bridge);
	/* [한국어] 코어가 config 콜백에 넘겨 줄 자리에 그것을 매단다. */
	bridge->sysdata = priv;

	/* [한국어] 첫 reg 항목을 매핑한다. 자원 포인터도 함께 받는다. */
	reg = devm_platform_get_and_ioremap_resource(pdev, 0, &cfg_res);
	/* [한국어] 실패하면, */
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	/* [한국어] 두 번째 reg 항목이 내장 USB 호스트의 메모리 구간이다. */
	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	/* [한국어] 없거나 시작이 0 이면, */
	if (!mem_res || !mem_res->start)
		return -ENODEV;

	/* [한국어] 64KB 정렬이 아니면 창 설정에 쓸 수 없다. */
	if (mem_res->start & 0xFFFF)
		/* [한국어] 잘못된 인자로 거절한다. */
		return -EINVAL;

	/* [한국어] 자원을 **값으로** 복사한다. 아래 cfg_res 가 포인터인 것과 달리, 이쪽은
	 * start 만 쓰므로 복사가 간단하다. */
	priv->mem_res = *mem_res;
	/* [한국어] 이쪽은 포인터를 그대로 들고 있는다. */
	priv->cfg_res = cfg_res;

	/* [한국어] 오류 인터럽트를 얻는다. */
	priv->irq = platform_get_irq(pdev, 0);
	/* [한국어] 매핑된 창을, */
	priv->reg = reg;
	/* [한국어] device 를 기록한다. */
	priv->dev = dev;

	/* [한국어] IRQ 가 없으면, */
	if (priv->irq < 0) {
		/* [한국어] 기록하고, */
		dev_err(dev, "no valid irq found\n");
		/* [한국어] 그 오류를 올려보낸다. 디버그 빌드가 아니면 이 IRQ 를 쓰지 않는데도
		 * probe 를 실패시키는 것이 눈에 띈다. */
		return priv->irq;
	}

	/* [한국어] config 접근 콜백 표를 연결한다. */
	bridge->ops = &rcar_pci_ops;

	/* [한국어] 모든 버스 번호를 다시 배정하게 한다. 펌웨어가 설정해 둔 값을 믿지 않고
	 * 커널이 처음부터 다시 정한다는 뜻이다. */
	pci_add_flags(PCI_REASSIGN_ALL_BUS);

	/* [한국어] 하드웨어를 기동한다. 버스 열거 전에 창과 중재기가 준비되어야 한다. */
	rcar_pci_setup(priv);

	/* [한국어] 여기서부터는 PCI 코어가 맡는다 — 버스 생성, 열거, 드라이버 바인딩. */
	return pci_host_probe(bridge);
}

static const struct of_device_id rcar_pci_of_match[] = {
	/* [한국어] R-Car H2. */
	{ .compatible = "renesas,pci-r8a7790", },
	/* [한국어] R-Car M2-W. */
	{ .compatible = "renesas,pci-r8a7791", },
	/* [한국어] R-Car E2. */
	{ .compatible = "renesas,pci-r8a7794", },
	/* [한국어] 세대 공통. */
	{ .compatible = "renesas,pci-rcar-gen2", },
	/* [한국어] RZ/N1. 같은 IP 를 쓰는 SoC 들이 한 드라이버를 공유한다. */
	{ .compatible = "renesas,pci-rzn1", },
	/* [한국어] 배열 끝. */
	{ },
};

static struct platform_driver rcar_pci_driver = {
	.driver = {
		/* [한국어] sysfs 와 로그에 보일 이름. */
		.name = "pci-rcar-gen2",
		/* [한국어] sysfs 로 bind/unbind 를 막는다. */
		.suppress_bind_attrs = true,
		/* [한국어] 위 compatible 표. */
		.of_match_table = rcar_pci_of_match,
	},
	/* [한국어] 위 probe. */
	.probe = rcar_pci_probe,
};
/* [한국어] 모듈이 아니라 내장 드라이버로 등록한다. remove 가 없어 뗄 수 없다. */
builtin_platform_driver(rcar_pci_driver);
