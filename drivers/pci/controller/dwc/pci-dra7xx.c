// SPDX-License-Identifier: GPL-2.0
/*
 * pcie-dra7xx - PCIe controller driver for TI DRA7xx SoCs
 *
 * Copyright (C) 2013-2014 Texas Instruments Incorporated - https://www.ti.com
 *
 * Authors: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] TI DRA7xx SoC 의 DesignWare PCIe 결합 계층 (pci-dra7xx.c)
 *
 * === 파일의 역할 ===
 * Texas Instruments DRA7xx 계열 SoC 의 PCIe 컨트롤러를 초기화하는 드라이버다.
 * 컨트롤러 IP 가 Synopsys DesignWare(DWC) PCIe 이므로 링크 관리·주소 변환·
 * 버스 스캔 같은 공통 로직은 이웃 파일 pcie-designware 계열이 담당하고,
 * 이 파일은 TI 고유 부분만 얹는다.
 *
 * 한 소스가 RC(루트 컴플렉스)와 EP(엔드포인트) 두 모드를 모두 담는다는 점이
 * pcie-artpec6.c 와 같은 구조인데, 두 파일을 나란히 놓으면 차이가 잘 드러난다.
 *
 *   pcie-artpec6.c                이 파일(pci-dra7xx.c)
 *   ---------------------------   ------------------------------------------
 *   제어 레지스터가 syscon 안      제어 레지스터가 자체 MMIO 창(ti_conf)에
 *   regmap 으로 접근               readl/writel 로 직접 접근
 *   모드를 compatible 로 결정      모드를 compatible 의 match data 로 결정
 *   (4개 항목: 세대 2 × 모드 2)    (5개 항목: 변종별 + 모드별)
 *   INTx 처리 없음                 INTx irq_domain 을 직접 만든다
 *   MSI 를 DWC 코어에 맡김         MSI 상태 레지스터를 직접 훑는 핸들러가 있다
 *   remove 없음, bind 차단         shutdown 있음, PM 콜백 네 개
 *
 * 즉 artpec6 가 "최소한의 결합" 이라면 이 파일은 인터럽트 경로를 상당 부분
 * 직접 떠맡는 쪽이다. 그 이유는 이 SoC 의 래퍼가 MSI 와 INTx 를 자체
 * 레지스터(IRQSTATUS_MSI / IRQENABLE_SET_MSI)로 모아 주기 때문이다.
 *
 * 이 파일이 하는 일을 정리하면 넷이다.
 *   1) DT 에서 모드·PHY·클럭·레지스터 창을 받아 struct dra7xx_pcie 를 채운다.
 *   2) 래퍼 레지스터로 장치 타입(RC/EP/LEG_EP)을 정하고 LTSSM 을 켠다.
 *   3) 인터럽트를 떠맡는다 — 래퍼의 오류 인터럽트, MSI, INTx 세 갈래다.
 *   4) 모드에 따라 dw_pcie_host_init() 또는 dw_pcie_ep_init() 으로 넘긴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층은 PCI 코어 → DWC 공용 코어(pcie-designware-host.c 또는 -ep.c) →
 * 이 파일 → 래퍼 MMIO / PHY → SoC 하드웨어 순이다.
 *
 * RC 모드:
 *   platform_driver -> dra7xx_pcie_probe()
 *     -> dra7xx_add_pcie_port()
 *        -> dw_pcie_host_init()  [DWC 코어]
 *           -> 코어가 dra7xx_pcie_host_init() 을 되불러 인터럽트를 켠다
 *   그 뒤 인터럽트가 오면 dra7xx_pcie_msi_irq_handler() 가 MSI 를,
 *   dra7xx_pcie_irq_handler() 가 래퍼 오류를 처리한다.
 *
 * EP 모드:
 *   probe -> dra7xx_add_pcie_ep()
 *     -> dw_pcie_ep_init()            [DWC 코어]
 *        -> 코어가 dra7xx_pcie_ep_init() 을 되부른다(pcie-designware-ep.c:2734)
 *     -> dw_pcie_ep_init_registers()
 *     -> pci_epc_init_notify()
 *
 * EPC 콜백이 이 파일에 직접 오지 않는다는 점이 중요하다. 이 드라이버는
 * struct pci_epc_ops 가 아니라 struct dw_pcie_ep_ops 를 제공하며, 두 단계를
 * 거쳐 불린다. 아래 라인 번호는 모두 확인한 것이다.
 *
 *   EPF 드라이버
 *     -> EPC 코어(drivers/pci/endpoint/pci-epc-core.c)
 *        write_header:1130 / set_bar:1054 / clear_bar:967 / align_addr:858 /
 *        map_addr:766 / unmap_addr:741 / set_msi:620 / get_msi:578 /
 *        raise_irq:484 / start:434 / stop:410 / get_features:381
 *     -> DWC 코어의 epc_ops(pcie-designware-ep.c) 가 그 콜백들을 구현
 *     -> 그중 셋만 이 파일의 dw_pcie_ep_ops 로 내려온다
 *        raise_irq    : dw_pcie_ep_raise_irq():1765 이 :1774 에서
 *                       ep->ops->raise_irq() -> dra7xx_pcie_raise_irq()
 *        get_features : dw_pcie_ep_get_features():1873 이 :1882 에서
 *                       ep->ops->get_features() -> dra7xx_pcie_get_features()
 *                       (dw_pcie_ep_get_features():1138 경로도 같은 콜백을 쓴다)
 *        init         : dw_pcie_ep_init():2981 이 :2734 에서
 *                       ep->ops->init() -> dra7xx_pcie_ep_init()
 *   나머지 아홉 콜백(write_header, set_bar, clear_bar, align_addr, map_addr,
 *   unmap_addr, set_msi, get_msi, start, stop)은 DWC 코어가 iATU 와 config
 *   레지스터를 직접 다뤄 처리하므로 이 파일까지 내려오지 않는다.
 *
 * 실행 컨텍스트: probe/PM 은 프로세스 컨텍스트다. dra7xx_pcie_irq_handler()
 * 는 threaded 가 아닌 일반 핸들러이고, dra7xx_pcie_msi_irq_handler() 는
 * 체인 핸들러라 둘 다 인터럽트 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_rp / dw_pcie_ep 와
 *   세 콜백 표(dw_pcie_ops / dw_pcie_host_ops / dw_pcie_ep_ops),
 *   dw_pcie_host_init(), dw_pcie_ep_init(), dw_pcie_ep_init_registers(),
 *   dw_pcie_ep_raise_msi_irq(). 그리고 ../../pci.h 의 코어 내부 선언.
 * 아래쪽: PHY 프레임워크(레인마다 phy 하나), 클럭, syscon/regmap
 *   (레인 구성과 정렬되지 않은 접근 우회에 쓴다), DT 파서, irq_domain.
 * 공유 상태: struct dra7xx_pcie 하나. dev_get_drvdata 로 어디서든 되찾을 수
 *   있게 to_dra7xx_pcie() 매크로를 두었다. 포인터 하나뿐이라 전역 가변
 *   상태가 없고 인스턴스마다 독립적이다.
 *
 * === 주요 함수/구조체 요약 ===
 * dra7xx_pcie_probe()        : 진입점. 모드를 정하고 자원을 모아 DWC 코어에 넘긴다.
 * dra7xx_pcie_establish_link(): LTSSM 을 켜 링크 훈련을 시작한다.
 * dra7xx_pcie_irq_handler()  : 래퍼의 오류·PM 인터럽트를 받아 로그로 남긴다.
 * dra7xx_pcie_msi_irq_handler(): MSI 와 INTx 를 모아 받는 체인 핸들러.
 * dra7xx_pcie_handle_msi()   : MSI 상태 레지스터 한 뭉치를 훑어 넘긴다.
 * dra7xx_pcie_init_irq_domain(): INTx 용 irq_domain 을 만든다.
 * dra7xx_pcie_raise_irq()    : EP 모드에서 호스트로 인터럽트를 올린다.
 * dra7xx_pcie_enable_phy()   : 레인마다 PHY 를 켠다.
 * dra7xx_pcie_unaligned_memaccess() : 정렬되지 않은 접근을 허용하는 우회.
 * struct dra7xx_pcie         : 이 드라이버의 장치별 상태 전부.
 * struct dra7xx_pcie_of_data : compatible 에 묶이는 모드와 레인 선택 마스크.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * RC 모드로 동작할 때 이 드라이버가 만든 버스 위에 NVMe SSD 가 열거될 수는
 * 있으나, 그 경로에 NVMe 에 특화된 처리는 없고 모든 PCIe 장치에 똑같이
 * 적용된다. EP 모드는 방향이 반대라 NVMe 호스트 드라이버와 무관하다.
 */

/* [한국어] clk.h — 컨트롤러 클럭 제어. */
#include <linux/clk.h>
/* [한국어] delay.h — mdelay(). EP 모드에서 INTx 를 세웠다 내리는 지연에 쓴다. */
#include <linux/delay.h>
/* [한국어] device.h — struct device 와 dev_get_drvdata(). to_dra7xx_pcie 매크로의 바탕이다. */
#include <linux/device.h>
/* [한국어] err.h — IS_ERR / PTR_ERR. */
#include <linux/err.h>
/* [한국어] interrupt.h — devm_request_irq(), irqreturn_t. 오류 인터럽트 핸들러를 등록한다. */
#include <linux/interrupt.h>
/* [한국어] irq.h — struct irq_data, dummy_irq_chip. INTx 에 개별 마스킹이 없어
 * 더미 chip 을 그대로 쓴다. */
#include <linux/irq.h>
/* [한국어] irqchip/chained_irq.h — chained_irq_enter/exit. MSI/INTx 체인 핸들러의 규약이다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irqdomain.h — INTx 용 irq_domain 생성과 매핑. */
#include <linux/irqdomain.h>
/* [한국어] kernel.h — 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] module.h — 모듈 메타데이터와 module_platform_driver(). */
#include <linux/module.h>
/* [한국어] of.h — DT 순회와 of_device_get_match_data(). 모드 판정의 근거를 여기서 얻는다. */
#include <linux/of.h>
/* [한국어] of_pci.h — DT PCI 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] pci.h — PCI 표준 상수와 타입. */
#include <linux/pci.h>
/* [한국어] phy/phy.h — 레인마다 하나씩 다루는 PHY API. */
#include <linux/phy/phy.h>
/* [한국어] platform_device.h — 플랫폼 드라이버 뼈대와 자원 조회. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime.h — 런타임 PM. probe 가 참조를 잡아 클럭과 전원을 붙인다. */
#include <linux/pm_runtime.h>
/* [한국어] resource.h — struct resource. */
#include <linux/resource.h>
/* [한국어] types.h — 기본 타입. */
#include <linux/types.h>
/* [한국어] mfd/syscon.h — syscon_regmap_lookup_by_phandle_args(). 레인 구성과
 * 정렬되지 않은 접근 우회가 PCIe IP 밖의 syscon 레지스터에 있어 필요하다. */
#include <linux/mfd/syscon.h>
/* [한국어] regmap.h — regmap_update_bits(). 위 syscon 접근의 실제 수단이다. */
#include <linux/regmap.h>
/* [한국어] gpio/consumer.h — GPIO API. 이 파일에서 직접 쓰지는 않는다(전수 grep 확인). */
#include <linux/gpio/consumer.h>

/* [한국어] ../../pci.h — PCI 코어 내부 헤더. 두 단계 위 디렉터리라 경로가 길다. */
#include "../../pci.h"
/* [한국어] pcie-designware.h — DWC 공용 코어의 계약. struct dw_pcie / dw_pcie_rp /
 * dw_pcie_ep 와 세 콜백 표, dw_pcie_host_init(), dw_pcie_ep_init(),
 * dw_pcie_readl_dbi() 등이 여기 있다. 이 파일이 결합 계층인 근거다. */
#include "pcie-designware.h"

/* PCIe controller wrapper DRA7XX configuration registers */

/* [한국어] 래퍼의 주 인터럽트 상태 레지스터. 오류와 전원 관리 사건이 여기 모인다.
 * 이 창은 DWC IP 안이 아니라 TI 가 그 주위에 두른 래퍼의 것이다. */
#define	PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN		0x0024
/* [한국어] 같은 인터럽트의 활성 레지스터. 비트를 세워야 그 사건이 올라온다. */
#define	PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MAIN		0x0028
/* [한국어] 시스템 오류(0번). */
#define	ERR_SYS						BIT(0)
/* [한국어] 치명적 오류(1번). */
#define	ERR_FATAL					BIT(1)
/* [한국어] 치명적이지 않은 오류(2번). */
#define	ERR_NONFATAL					BIT(2)
/* [한국어] 정정 가능한 오류(3번). */
#define	ERR_COR						BIT(3)
/* [한국어] AXI 태그 조회 치명적 오류(4번). AXI 는 SoC 내부 버스다. */
#define	ERR_AXI						BIT(4)
/* [한국어] ECRC(종단 간 CRC) 오류(5번). */
#define	ERR_ECRC					BIT(5)
/* [한국어] PME_Turn_Off 메시지 수신(8번). 6~7번이 비어 있다. */
#define	PME_TURN_OFF					BIT(8)
/* [한국어] PME Turn-Off Ack 수신(9번). */
#define	PME_TO_ACK					BIT(9)
/* [한국어] PM_PME 메시지 수신(10번). */
#define	PM_PME						BIT(10)
/* [한국어] 링크 리셋 요청(11번). */
#define	LINK_REQ_RST					BIT(11)
/* [한국어] 링크업 상태 변화(12번). 이 파일에서 실제 동작이 있는 유일한 비트로,
 * EP 모드에서 DWC 코어에 링크업을 알린다. */
#define	LINK_UP_EVT					BIT(12)
/* [한국어] Bus Master Enable 변화(13번). */
#define	CFG_BME_EVT					BIT(13)
/* [한국어] Memory Space Enable 변화(14번). */
#define	CFG_MSE_EVT					BIT(14)
/* [한국어] 위 열세 비트를 한데 묶은 값. 래퍼 인터럽트를 한 번에 켜고 끌 때 쓴다. */
#define	INTERRUPTS (ERR_SYS | ERR_FATAL | ERR_NONFATAL | ERR_COR | ERR_AXI | \
			/* [한국어] 오류 여섯과 전원 관리 셋, */
			ERR_ECRC | PME_TURN_OFF | PME_TO_ACK | PM_PME | \
			/* [한국어] 링크 둘과 config 변화 둘을 모두 포함한다. */
			LINK_REQ_RST | LINK_UP_EVT | CFG_BME_EVT | CFG_MSE_EVT)

/* [한국어] MSI/INTx 인터럽트 상태 레지스터. 위 주 인터럽트와 별개 창이다. */
#define	PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI		0x0034
/* [한국어] 같은 인터럽트의 활성 레지스터. */
#define	PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MSI		0x0038
/* [한국어] INTA(0번). */
#define	INTA						BIT(0)
/* [한국어] INTB(1번). */
#define	INTB						BIT(1)
/* [한국어] INTC(2번). */
#define	INTC						BIT(2)
/* [한국어] INTD(3번). */
#define	INTD						BIT(3)
/* [한국어] MSI 도착(4번). 이 한 비트가 모든 MSI 벡터를 대표하며, 실제 벡터는
 * DWC IP 안의 상태 레지스터를 따로 훑어야 안다. */
#define	MSI						BIT(4)
/* [한국어] INTA~INTD 네 비트를 묶은 값. 이름에 EP 가 들어 있지만 RC 모드에서
 * 아래 장치의 INTx 를 받는 데도 쓰인다. */
#define	LEG_EP_INTERRUPTS (INTA | INTB | INTC | INTD)

/* [한국어] 장치 타입 선택 레지스터. 이 컨트롤러를 RC 로 쓸지 EP 로 쓸지 정한다. */
#define	PCIECTRL_TI_CONF_DEVICE_TYPE			0x0100
/* [한국어] 엔드포인트(0). */
#define	DEVICE_TYPE_EP					0x0
/* [한국어] 레거시 엔드포인트(1). 이 파일에서 실제로 쓰이지는 않는다(전수 grep 확인). */
#define	DEVICE_TYPE_LEG_EP				0x1
/* [한국어] 루트 컴플렉스(4). probe 가 모드에 따라 이 값이나 DEVICE_TYPE_EP 를 쓴다. */
#define	DEVICE_TYPE_RC					0x4

/* [한국어] 장치 명령 레지스터. LTSSM 스위치가 여기 있다. */
#define	PCIECTRL_DRA7XX_CONF_DEVICE_CMD			0x0104
/* [한국어] LTSSM 활성 비트(0번). 이 한 비트를 세우면 링크 훈련이 시작되고,
 * 내리면 링크가 끊어진다. */
#define	LTSSM_EN					0x1

/* [한국어] PHY/컨트롤러 상태 레지스터. */
#define	PCIECTRL_DRA7XX_CONF_PHY_CS			0x010C
/* [한국어] 링크업 비트(16번). dra7xx_pcie_link_up() 이 이 비트로 판정한다. */
#define	LINK_UP						BIT(16)
/* [한국어] CPU 주소를 버스 주소로 자를 때 쓰는 마스크(하위 28비트).
 * cpu_addr_fixup 콜백이 상위 4비트를 버리는 근거이며, 그 상위 비트의
 * 의미는 이 트리에서 확인 못 함. */
#define	DRA7XX_CPU_TO_BUS_ADDR				0x0FFFFFFF

/* [한국어] EP 모드에서 INTx 를 올리는 레지스터. 쓰기만으로 assert 된다. */
#define	PCIECTRL_TI_CONF_INTX_ASSERT			0x0124
/* [한국어] 같은 INTx 를 내리는 레지스터. 두 레지스터가 따로 있어 읽고-고쳐-쓰기가 필요 없다. */
#define	PCIECTRL_TI_CONF_INTX_DEASSERT			0x0128

/* [한국어] EP 모드에서 MSI 를 보내는 레지스터. */
#define	PCIECTRL_TI_CONF_MSI_XMT			0x012c
/* [한국어] MSI 전송 요청 비트(0번). 벡터 번호와 함께 써 넣으면 하드웨어가 보낸다. */
#define MSI_REQ_GRANT					BIT(0)
/* [한국어] 그 레지스터에서 벡터 번호 필드의 시작 비트(7). */
#define MSI_VECTOR_SHIFT				7

/* [한국어] 1레인/2레인 선택 비트(13번). syscon 쪽 레지스터의 비트이며,
 * 변종별 of_data 의 b1co_mode_sel_mask 로 쓰인다. */
#define PCIE_1LANE_2LANE_SELECTION			BIT(13)
/* [한국어] B1C0 모드 선택 비트(2번). 2레인 구성에서 세운다. */
#define PCIE_B1C0_MODE_SEL				BIT(2)
/* [한국어] B0/B1 TSYNCEN 비트(0번). 두 레인의 동기화를 켜는 것으로 보이나
 * 정확한 의미는 이 트리에서 확인 못 함. */
#define PCIE_B0_B1_TSYNCEN				BIT(0)

/* [한국어] 이 드라이버의 장치별 상태 전부. */
struct dra7xx_pcie {
	/* [한국어] DWC 공용 코어의 장치 구조체. 이 결합 계층과 코어를 잇는 통로다.
	 * 설정자: dra7xx_pcie_probe() 가 할당해 ops 를 걸고 채운다.
	 * 읽는 자: 거의 모든 함수. to_dw_pcie_from_pp/ep 로 거슬러 오는 경로의 종착점이다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다. */
	struct dw_pcie		*pci;
	/* [한국어] TI 래퍼 레지스터 창의 가상 주소(상류 주석대로 DT 의 ti_conf).
	 * 설정자: probe 가 devm_platform_ioremap_resource_byname 으로 매핑한다.
	 * 읽는 자: dra7xx_pcie_readl/writel 을 통해 래퍼를 만지는 모든 코드.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다.
	 * DWC IP 자체의 dbi 창과는 별개라는 점이 중요하다 — 그쪽은 코어가 관리한다. */
	void __iomem		*base;		/* DT ti_conf */
	/* [한국어] PHY 개수(상류 주석대로 DT 의 phy-names 수). 곧 레인 수다.
	 * 설정자: probe 가 DT 에서 센다.
	 * 읽는 자: enable_phy / disable_phy 의 루프 상한.
	 * 값 범위: 1 이상.
	 * 동기화: probe 에서만 설정된다. */
	int			phy_count;	/* DT phy-names count */
	/* [한국어] 레인마다 하나씩인 PHY 핸들 배열.
	 * 설정자: probe 가 devm 으로 배열을 잡고 이름별로 채운다.
	 * 읽는 자: enable_phy / disable_phy.
	 * 값 범위: phy_count 개짜리 배열.
	 * 동기화: probe 에서만 설정된다. */
	struct phy		**phy;
	/* [한국어] INTx 용 irq_domain.
	 * 설정자: dra7xx_pcie_init_irq_domain(). CONFIG_PCI_MSI 가 꺼진 빌드에서만 만든다.
	 * 읽는 자: 체인 핸들러가 INTx 를 넘길 때.
	 * 값 범위: 유효한 포인터 또는 NULL(MSI 빌드).
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다. */
	struct irq_domain	*irq_domain;
	/* [한국어] 컨트롤러 클럭.
	 * 설정자: probe 가 얻어 켠다.
	 * 읽는 자: probe 의 에러 경로.
	 * 값 범위: 유효한 포인터. 선택 사항이라 없을 수도 있다.
	 * 동기화: probe 에서만 설정된다. */
	struct clk              *clk;
	/* [한국어] 이 인스턴스의 동작 모드(RC 또는 EP).
	 * 설정자: probe 가 of_device_get_match_data 로 얻은 of_data 에서 가져온다.
	 * 읽는 자: probe 의 분기, irq_handler 의 링크업 처리, suspend/resume.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다.
	 * 이 한 값이 이 파일의 모든 모드 분기를 가른다. */
	enum dw_pcie_device_mode mode;
};

/* [한국어] compatible 에 묶이는 변종 정보. 매칭 표의 data 가 이 타입을 가리킨다. */
struct dra7xx_pcie_of_data {
	/* [한국어] 이 compatible 이 뜻하는 동작 모드.
	 * 설정자: 정적 초기화(아래 of_data 들).
	 * 읽는 자: probe 가 dra7xx->mode 로 옮긴다.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: const 정적 데이터. */
	enum dw_pcie_device_mode mode;
	/* [한국어] 2레인 구성에서 쓸 레인 선택 비트 마스크.
	 * 설정자: 정적 초기화. 변종마다 비트 위치가 달라 값이 다르다.
	 * 읽는 자: probe 가 dra7xx_pcie_configure_two_lane() 에 넘긴다.
	 * 값 범위: 0(2레인 미지원) 또는 PCIE_1LANE_2LANE_SELECTION 같은 비트.
	 * 동기화: const 정적 데이터. */
	u32 b1co_mode_sel_mask;
};

/* [한국어] DWC 장치 구조체에서 이 결합 계층의 상태를 되찾는 매크로.
 * dev_get_drvdata 이므로 probe 가 platform_set_drvdata 를 부른 뒤에만 동작한다.
 * 계층이 셋(EPC/DWC/글루)이라 이런 되찾기가 자주 필요하다. */
#define to_dra7xx_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * dra7xx_pcie_readl - 래퍼 레지스터를 읽는다
 *
 * @pcie: 이 드라이버의 상태.   @offset: 래퍼 기준 오프셋.
 * @return: 읽은 32비트 값.
 *
 * pcie->base 는 DT 의 ti_conf 창이며, DWC IP 자체의 레지스터가 아니라
 * TI 가 그 주위에 두른 래퍼 레지스터다. 장치 타입 선택, LTSSM 제어,
 * 인터럽트 상태·마스크가 전부 이 창에 있다.
 *
 * artpec6 가 같은 성격의 레지스터를 syscon regmap 으로 접근하는 것과 대비된다 —
 * 이쪽은 자체 MMIO 창이라 readl 로 직접 읽는다.
 *
 * 실행 컨텍스트: 제약 없음. 인터럽트 문맥에서도 불린다.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → readl()
 */
static inline u32 dra7xx_pcie_readl(struct dra7xx_pcie *pcie, u32 offset)
{
	/* [한국어] 래퍼 창의 기준 주소에 오프셋을 더해 읽는다. */
	return readl(pcie->base + offset);
}

/* [한국어]
 * dra7xx_pcie_writel - 래퍼 레지스터에 쓴다
 *
 * @pcie: 이 드라이버의 상태.   @offset: 래퍼 기준 오프셋.   @value: 쓸 값.
 * @return: 없음.
 *
 * 읽기 판의 짝이다. 인자 순서가 (오프셋, 값)으로 writel 과 반대인 점을
 * 유의해야 한다 — 이 파일 안에서는 일관되지만 다른 드라이버와 섞어 읽을 때
 * 헷갈리기 쉽다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → writel()
 */
static inline void dra7xx_pcie_writel(struct dra7xx_pcie *pcie, u32 offset,
				      u32 value)
{
	/* [한국어] 인자 순서가 (오프셋, 값)으로 writel 과 반대다. 이 파일 안에서는
	 * 일관되지만 다른 드라이버와 섞어 읽을 때 헷갈리기 쉽다. */
	writel(value, pcie->base + offset);
}

/* [한국어]
 * dra7xx_pcie_cpu_addr_fixup - CPU 물리 주소를 버스 주소로 자른다
 *
 * @pci:      DWC 코어의 장치 구조체. 쓰지 않는다.
 * @cpu_addr: CPU 쪽 물리 주소.
 * @return: 하위 28비트만 남긴 주소.
 *
 * DWC 코어가 iATU(주소 변환 창)를 설정할 때 부르는 콜백이다. SoC 마다
 * CPU 주소와 컨트롤러가 이해하는 주소가 다를 수 있어 이 훅이 있다.
 *
 * DRA7XX_CPU_TO_BUS_ADDR 이 0x0FFFFFFF 이므로 상위 4비트를 버린다.
 * 그 상위 비트가 SoC 내부의 주소 공간 선택자이고 컨트롤러 쪽에는
 * 의미가 없기 때문으로 보이나, 그 근거는 이 트리에서 확인 못 함.
 *
 * artpec6 의 같은 콜백이 모드에 따라 다른 기준 주소를 빼는 것과 달리,
 * 이쪽은 모드와 무관하게 마스킹 하나로 끝난다.
 *
 * 실행 컨텍스트: 제약 없음. 순수 계산이다.
 *
 * 호출 체인:  DWC 코어의 iATU 설정 → dw_pcie_ops.cpu_addr_fixup → [이 함수]
 */
static u64 dra7xx_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 cpu_addr)
{
	return cpu_addr & DRA7XX_CPU_TO_BUS_ADDR;
}

/* [한국어]
 * dra7xx_pcie_link_up - 링크가 올라왔는지 본다
 *
 * @pci: DWC 코어의 장치 구조체.
 * @return: 링크가 살아 있으면 true.
 *
 * DWC 코어가 링크 훈련 완료를 기다릴 때 반복해서 부르는 콜백이다.
 * DWC IP 자체의 링크 상태가 아니라 TI 래퍼의 PHY_CS 레지스터에서
 * LINK_UP 비트(16번)를 본다.
 *
 * to_dra7xx_pcie() 로 글루 상태를 되찾는데, 그 매크로가
 * dev_get_drvdata 이므로 probe 가 drvdata 를 걸어 둔 뒤에만 동작한다.
 *
 * 실행 컨텍스트: 제약 없음. DWC 코어가 폴링 루프에서 부른다.
 *
 * 호출 체인:  DWC 코어의 링크 대기 → dw_pcie_ops.link_up → [이 함수]
 *               → dra7xx_pcie_readl()
 */
static bool dra7xx_pcie_link_up(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_PHY_CS);
/* [한국어] 하위 28비트만 남긴다(윗줄). 상위 4비트는 SoC 내부 주소 공간 선택자로
 * 보이며 컨트롤러 쪽에는 의미가 없으나, 그 근거는 이 트리에서 확인 못 함. */

	return reg & LINK_UP;
}

/* [한국어]
 * dra7xx_pcie_stop_link - LTSSM 을 꺼 링크를 내린다
 *
 * @pci: DWC 코어의 장치 구조체.
 * @return: 없음.
 *
 * DEVICE_CMD 레지스터의 LTSSM_EN 비트를 내린다. 그러면 링크 훈련 상태
 * 기계가 멈추고 링크가 끊어진다.
 *
 * establish_link 의 역이며, DWC 코어가 EP 모드의 stop 이나 오류 복구
 * 경로에서 부른다.
 *
 * 읽고-고쳐-쓰기라 같은 레지스터의 다른 비트가 보존된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.stop_link → [이 함수]
 *               → dra7xx_pcie_readl/writel()
 */
static void dra7xx_pcie_stop_link(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg;
/* [한국어] 래퍼의 PHY_CS 에서 LINK_UP 비트를 본다(윗줄). DWC IP 자체의 링크 상태가
 * 아니라 TI 래퍼가 별도로 알려 주는 값이다. */

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	/* [한국어] LTSSM 활성 비트를 내린다. */
	reg &= ~LTSSM_EN;
	/* [한국어] 고친 값을 되쓴다. 이 순간 링크 훈련이 멈추고 링크가 끊어진다. */
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);
}

/* [한국어]
 * dra7xx_pcie_establish_link - LTSSM 을 켜 링크 훈련을 시작한다
 *
 * @pci: DWC 코어의 장치 구조체.
 * @return: 항상 0.
 *
 * DEVICE_CMD 레지스터의 LTSSM_EN 을 세운다. 이 한 비트가 링크 훈련을
 * 시작시키는 스위치다.
 *
 * 먼저 이미 링크가 올라와 있으면 로그만 남기고 그대로 켠다 — 되돌리거나
 * 건너뛰지 않는다.
 *
 * 링크가 설 때까지 기다리지 않는 점이 요점이다. 기다리는 일은 DWC 코어의
 * dw_pcie_wait_for_link() 가 맡고, 이 콜백은 시작만 지시한다. 그래서
 * 반환값이 항상 0 이고 실패할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.start_link → [이 함수]
 *               → dra7xx_pcie_readl/writel()
 */
static int dra7xx_pcie_establish_link(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	struct device *dev = pci->dev;
	/* [한국어] 읽고-고쳐-쓸 DEVICE_CMD 값. */
	u32 reg;

	if (dw_pcie_link_up(pci)) {
		/* [한국어] 이미 링크가 올라와 있으면 알린다. */
		dev_err(dev, "link is already up\n");
		/* [한국어] 다만 0 을 돌려주고 그대로 아래에서 LTSSM 을 켠다 — 되돌리거나
		 * 건너뛰지 않는다. */
		return 0;
	}

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	/* [한국어] LTSSM 활성 비트를 세운다. */
	reg |= LTSSM_EN;
	/* [한국어] 고친 값을 쓴다. 이 한 줄이 링크 훈련을 시작시킨다. 링크가 설 때까지
	 * 기다리는 일은 DWC 코어의 dw_pcie_wait_for_link() 가 맡는다. */
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);
/* [한국어] 그래서 이 함수는 항상 0 을 돌려주며 실패할 수 없다. */

	return 0;
}

/* [한국어]
 * dra7xx_pcie_enable_msi_interrupts - 래퍼의 MSI/INTx 인터럽트를 켠다
 *
 * @dra7xx: 이 드라이버의 상태.   @return: 없음.
 *
 * 두 가지를 켠다.
 *   LEG_EP_INTERRUPTS - INTA~INTD 네 비트. 이름에 EP 가 들어 있지만
 *                       RC 모드에서 아래 장치의 INTx 를 받는 데도 쓰인다.
 *   MSI               - MSI 도착 비트.
 *
 * 먼저 상태 레지스터에 같은 비트를 써서 지운다. 이 하드웨어의 상태 비트는
 * 1 을 쓰면 지워지는 방식이라, 켜기 전에 부트로더가 남긴 대기 인터럽트를
 * 치우는 것이다. 그러지 않으면 켜자마자 쏟아진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  dra7xx_pcie_enable_interrupts() → [이 함수] → dra7xx_pcie_writel()
 */
static void dra7xx_pcie_enable_msi_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI,
			   LEG_EP_INTERRUPTS | MSI);

	dra7xx_pcie_writel(dra7xx,
			   PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MSI,
			   MSI | LEG_EP_INTERRUPTS);
}

/* [한국어]
 * dra7xx_pcie_enable_wrapper_interrupts - 래퍼의 오류·PM 인터럽트를 켠다
 *
 * @dra7xx: 이 드라이버의 상태.   @return: 없음.
 *
 * INTERRUPTS 매크로가 묶은 열세 비트를 한 번에 켠다 — 오류 여섯 종류
 * (SYS/FATAL/NONFATAL/COR/AXI/ECRC), 전원 관리 셋(PME_TURN_OFF/PME_TO_ACK/
 * PM_PME), 링크 관련 셋(LINK_REQ_RST/LINK_UP_EVT), config 변경 둘
 * (CFG_BME_EVT/CFG_MSE_EVT)이다.
 *
 * MSI 판과 마찬가지로 켜기 전에 상태를 지운다.
 *
 * 이 인터럽트들의 대부분은 dra7xx_pcie_irq_handler() 가 로그로만 남긴다.
 * 실제 동작이 있는 것은 LINK_UP_EVT 하나로, EP 모드에서 DWC 코어에
 * 링크업을 알린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  dra7xx_pcie_enable_interrupts() → [이 함수] → dra7xx_pcie_writel()
 */
static void dra7xx_pcie_enable_wrapper_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN,
			   INTERRUPTS);
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MAIN,
			   /* [한국어] 열세 비트를 한 번에 켠다(윗줄에서 상태를 지웠다). 지우기가 먼저인 이유는
			    * 부트로더가 남긴 대기 인터럽트가 켜자마자 쏟아지는 것을 막기 위해서다. */
			   INTERRUPTS);
}

/* [한국어]
 * dra7xx_pcie_enable_interrupts - 래퍼 인터럽트를 모두 켠다
 *
 * @dra7xx: 이 드라이버의 상태.   @return: 없음.
 *
 * 위 두 함수를 차례로 부른다. MSI 쪽은 CONFIG_PCI_MSI 가 켜진 빌드에서만
 * 부르는데, 그렇지 않으면 MSI 를 받아도 넘길 곳이 없기 때문이다.
 *
 * RC 모드에서는 host_init 콜백이, EP 모드에서는 probe 가 이 함수를 부른다 —
 * 두 경로가 같은 초기화를 공유하는 지점이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  dra7xx_pcie_host_init() / dra7xx_add_pcie_ep() → [이 함수]
 */
static void dra7xx_pcie_enable_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_enable_wrapper_interrupts(dra7xx);
	dra7xx_pcie_enable_msi_interrupts(dra7xx);
}

/* [한국어]
 * dra7xx_pcie_host_init - DWC 코어가 RC 초기화 중 되부르는 훅
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 * @return: 항상 0.
 *
 * dw_pcie_host_init() 이 공통 초기화를 하다가 SoC 고유 부분을 맡기려고
 * 부르는 콜백이다. 여기서는 래퍼 인터럽트를 켜는 일만 한다.
 *
 * 컨테이너에서 거슬러 올라가는 두 단계가 특징이다 — pp 에서
 * to_dw_pcie_from_pp() 로 DWC 장치를, 거기서 to_dra7xx_pcie() 로
 * 글루 상태를 얻는다. 계층이 셋이라 이런 왕복이 생긴다.
 *
 * 링크 훈련은 여기서 시작하지 않는다. DWC 코어가 이 콜백이 돌아온 뒤
 * dw_pcie_ops.start_link(= dra7xx_pcie_establish_link)를 따로 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_add_pcie_port() → dw_pcie_host_init() [DWC 코어]
 *               → dw_pcie_host_ops.init → [이 함수]
 *               → dra7xx_pcie_enable_interrupts()
 */
static int dra7xx_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
/* [한국어] CONFIG_PCI_MSI 가 켜진 빌드에서만 MSI 쪽을 켠다(윗줄).
 * 꺼져 있으면 MSI 를 받아도 넘길 곳이 없다. */

	dra7xx_pcie_enable_interrupts(dra7xx);

	return 0;
}

/* [한국어]
 * dra7xx_pcie_intx_map - INTx 가상 IRQ 하나를 설정한다
 *
 * @domain: INTx irq_domain.   @irq: 배정된 virq.   @hwirq: INTx 번호(0~3).
 * @return: 항상 0.
 *
 * irq_domain_ops 의 map 콜백이다. dummy_irq_chip 을 걸고 흐름 제어를
 * handle_simple_irq 로 지정한다.
 *
 * dummy_irq_chip 을 쓰는 것이 요점이다. 이 래퍼에는 INTx 를 개별로
 * 마스크하는 수단이 없어 mask/unmask 콜백에 넣을 것이 없기 때문이다.
 * mvebu 가 언마스크 레지스터의 비트를 개별로 다루는 것과 대비된다.
 *
 * handle_simple_irq 인 이유도 같다. 마스킹이 없으니 level/edge 흐름 제어의
 * 마스크 조작이 무의미하고, 핸들러만 부르면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(매핑 생성).
 *
 * 호출 체인:  irq_domain 코어 → [이 함수] → irq_set_chip_and_handler()
 */
static int dra7xx_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);
/* [한국어] INTx 에 개별 마스킹 수단이 없어 dummy_irq_chip 을 그대로 쓴다(윗줄).
 * handle_simple_irq 인 이유도 같다 — 마스킹이 없으니 level/edge 흐름 제어의
 * 마스크 조작이 무의미하고 핸들러만 부르면 된다. */

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 매핑 콜백. */
	.map = dra7xx_pcie_intx_map,
	/* [한국어] DT 의 INTx 지정을 hwirq 로 해석하는 PCI 전용 헬퍼. */
	.xlate = pci_irqd_intx_xlate,
};

/* [한국어]
 * dra7xx_pcie_handle_msi - MSI 상태 레지스터 한 뭉치를 훑어 넘긴다
 *
 * @pp:    DWC 코어의 루트 포트.
 * @index: 컨트롤러 블록 번호(뭉치 하나가 32개 벡터를 담는다).
 * @return: 처리한 것이 있으면 1, 없으면 0.
 *
 * DWC IP 는 MSI 상태를 32비트씩 여러 블록으로 나눠 둔다. 이 함수가
 * 블록 하나를 맡아 선 비트마다 IRQ 도메인으로 넘긴다.
 *
 * 레지스터를 DWC 코어의 dbi 접근(dw_pcie_readl_dbi)으로 읽는 점이
 * 이 파일의 다른 레지스터 접근과 다르다 — 래퍼가 아니라 DWC IP 안의
 * 레지스터이기 때문이다.
 *
 * 반환값이 "처리했는가" 인 이유는 호출자가 그것으로 루프를 계속할지
 * 정하기 때문이다. 아래 handle_msi_irq 의 설계와 맞물린다.
 *
 * 비트를 지우지 않는 점을 짚어 둔다. 지우는 일은 DWC 코어의 MSI
 * irq_chip 이 ack 콜백에서 한다.
 *
 * 실행 컨텍스트: 인터럽트. 잠들지 않는다.
 *
 * 호출 체인:  dra7xx_pcie_handle_msi_irq() → [이 함수]
 *               → generic_handle_domain_irq()
 */
static int dra7xx_pcie_handle_msi(struct dw_pcie_rp *pp, int index)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	unsigned long val;
	/* [한국어] 찾은 비트 위치. */
	int pos;

	val = dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_STATUS +
				   /* [한국어] 블록마다 오프셋을 MSI_REG_CTRL_BLOCK_SIZE 만큼 밀어(윗줄) 상태를 읽는다.
				    * dbi 접근인 이유는 이 레지스터가 래퍼가 아니라 DWC IP 안에 있어서다. */
				   (index * MSI_REG_CTRL_BLOCK_SIZE));
	if (!val)
		/* [한국어] 이 블록에 선 비트가 없으면 처리할 것이 없다. */
		return 0;

	pos = find_first_bit(&val, MAX_MSI_IRQS_PER_CTRL);
	/* [한국어] 선 비트를 처음부터 끝까지 훑는다(윗줄에서 첫 비트를 찾았다). */
	while (pos != MAX_MSI_IRQS_PER_CTRL) {
		/* [한국어] 블록 번호와 비트 위치를 합쳐 전역 hwirq 를 만들어 넘긴다. */
		generic_handle_domain_irq(pp->irq_domain,
					  (index * MAX_MSI_IRQS_PER_CTRL) + pos);
		pos++;
		/* [한국어] 다음 선 비트를 찾는다. 비트를 지우지 않는데, 지우는 일은 DWC 코어의
		 * MSI irq_chip 이 ack 콜백에서 하기 때문이다. */
		pos = find_next_bit(&val, MAX_MSI_IRQS_PER_CTRL, pos);
	/* [한국어] 비트 루프 끝. */
	}

	return 1;
}

/* [한국어]
 * dra7xx_pcie_handle_msi_irq - 모든 MSI 블록이 빌 때까지 반복해 훑는다
 *
 * @pp: DWC 코어의 루트 포트.
 * @return: 없음.
 *
 * 상류 주석이 이 함수의 설계 이유를 밝힌다. 나가기 전에 모든 MSI 상태
 * 비트가 0 이어야 하며, 그러지 않으면 래퍼가 새 MSI 를 등록하지 않는다.
 * 그래서 한 번 훑는 것으로 끝내지 않고, 처리한 것이 있으면 다시 훑는다.
 *
 * 동시에 그 반복이 무한해질 위험을 상류가 함께 다룬다. MSI 가 폭주하면
 * 인터럽트 문맥에서 시스템이 멈추므로, 1000회 상한을 두고 넘으면 경고를
 * 남기고 빠져나온다.
 *
 * count 를 1000 과 비교하는 위치가 루프 조건과 그 뒤 두 곳인데, 루프를
 * 빠져나온 이유가 "다 처리해서" 인지 "상한에 걸려서" 인지 구분해 경고를
 * 찍기 위해서다.
 *
 * num_ctrls 는 전체 벡터 수를 블록 크기로 나눈 값이라, 이 컨트롤러가
 * 실제로 쓰는 블록만 훑는다.
 *
 * 실행 컨텍스트: 인터럽트. ratelimited 경고를 쓰는 이유도 그래서다.
 *
 * 호출 체인:  dra7xx_pcie_msi_irq_handler() → [이 함수] → dra7xx_pcie_handle_msi()
 */
static void dra7xx_pcie_handle_msi_irq(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	int ret, i, count, num_ctrls;
/* [한국어] 전체 벡터 수를 블록 크기로 나눠(윗줄) 실제로 쓰는 블록 수를 구한다. */

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;
/* [한국어] 아래 루프가 상류 주석이 설명하는 "모두 0 이 될 때까지" 반복이다. */

	/**
	 * Need to make sure all MSI status bits read 0 before exiting.
	 * Else, new MSI IRQs are not registered by the wrapper. Have an
	 * upperbound for the loop and exit the IRQ in case of IRQ flood
	 * to avoid locking up system in interrupt context.
	 */
	count = 0;
	do {
		ret = 0;

		for (i = 0; i < num_ctrls; i++)
			/* [한국어] 블록마다 처리하고 결과를 OR 로 모은다. 하나라도 처리했으면 ret 이 1 이 된다. */
			ret |= dra7xx_pcie_handle_msi(pp, i);
		/* [한국어] 반복 횟수를 센다. */
		count++;
	/* [한국어] 처리한 것이 있으면 다시 돈다. 상한 1000 은 상류 주석대로 MSI 폭주 시
	 * 인터럽트 문맥에서 시스템이 멈추는 것을 막는 안전장치다. */
	} while (ret && count <= 1000);

	if (count > 1000)
		/* [한국어] 상한에 걸려 나왔으면 경고한다(윗줄이 그 판정). ratelimited 인 이유는
		 * 인터럽트 문맥이라 같은 메시지가 쏟아질 수 있어서다. */
		dev_warn_ratelimited(pci->dev,
				     "Too many MSI IRQs to handle\n");
}

/* [한국어]
 * dra7xx_pcie_msi_irq_handler - MSI 와 INTx 를 모아 받는 체인 핸들러
 *
 * @desc: 상위 IRQ 의 descriptor.
 * @return: 없음.
 *
 * 래퍼가 MSI 와 INTx 를 한 인터럽트 선에 모아 주므로, 이 핸들러가
 * 상태 레지스터를 읽어 갈래를 나눈다.
 *
 *   INTA~INTD - 해당 INTx 를 이 파일의 irq_domain 으로 넘긴다.
 *   MSI       - 위 handle_msi_irq() 로 넘겨 DWC 코어의 MSI 도메인이 처리한다.
 *
 * chained_irq_enter/exit 로 감싸는 것이 체인 핸들러의 규약이다.
 *
 * INTx 와 MSI 의 처리 방식이 다른 점을 눈여겨볼 만하다. INTx 는 상태 비트를
 * 직접 지우지만 MSI 는 지우지 않는다 — MSI 쪽은 DWC 코어의 irq_chip 이
 * ack 에서 지우기 때문이다.
 *
 * 기본 갈래(default)가 없어 정의되지 않은 비트는 조용히 무시된다.
 *
 * 실행 컨텍스트: 인터럽트.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq() /
 *               dra7xx_pcie_handle_msi_irq()
 */
static void dra7xx_pcie_msi_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dra7xx_pcie *dra7xx;
	/* [한국어] DWC 코어의 루트 포트. */
	struct dw_pcie_rp *pp;
	/* [한국어] DWC 장치 구조체. */
	struct dw_pcie *pci;
	/* [한국어] 상태 레지스터 값. find_first_bit 계열에 넘기려고 unsigned long 이다. */
	unsigned long reg;
	/* [한국어] 비트 반복자. */
	u32 bit;

	chained_irq_enter(chip, desc);
/* [한국어] 체인 핸들러 진입. 이 사이에 상위 컨트롤러가 이 선을 마스크해 재진입이 없다. */

	pp = irq_desc_get_handler_data(desc);
	/* [한국어] 루트 포트에서 DWC 장치를, */
	pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 거기서 글루 상태를 되찾는다. 계층이 셋이라 두 단계가 필요하다. */
	dra7xx = to_dra7xx_pcie(pci);

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI);
	/* [한국어] 읽은 상태를 그대로 되써서 지운다(윗줄에서 읽었다). 1 을 쓰면 지워지는
	 * 방식이라 지금 선 비트만 정확히 지워진다. */
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI, reg);
/* [한국어] 아래에서 갈래를 나눈다. */

	switch (reg) {
	/* [한국어] MSI 도착이면 */
	case MSI:
		/* [한국어] DWC IP 의 상태 레지스터를 훑어 실제 벡터를 찾아 넘긴다. */
		dra7xx_pcie_handle_msi_irq(pp);
		break;
	case INTA:
	/* [한국어] INTB 이하 네 INTx 는 이 파일의 도메인으로 넘긴다. */
	case INTB:
	case INTC:
	case INTD:
		for_each_set_bit(bit, &reg, PCI_NUM_INTX)
			generic_handle_domain_irq(dra7xx->irq_domain, bit);
		/* [한국어] INTx 처리 끝. 기본 갈래가 없어 정의되지 않은 비트는 조용히 무시된다. */
		break;
	}

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * dra7xx_pcie_irq_handler - 래퍼의 오류·전원 관리 인터럽트를 처리한다
 *
 * @irq: 울린 IRQ 번호. 쓰지 않는다.
 * @arg: 등록 때 넘긴 dra7xx_pcie.
 * @return: 항상 IRQ_HANDLED.
 *
 * INTERRUPTS 매크로가 켜 둔 열세 비트를 하나씩 확인한다. 대부분은
 * dev_dbg 로 로그만 남긴다 — 디버깅용 관측 지점인 셈이다.
 *
 * 실제 동작이 있는 것은 LINK_UP_EVT 하나다. EP 모드일 때
 * dw_pcie_ep_linkup() 을 불러 DWC 코어에 링크가 올라왔음을 알리고,
 * 그러면 코어가 EPF 드라이버에 그 사실을 전파한다. RC 모드에서는
 * 로그만 남기는데, 링크 상태를 DWC 코어가 따로 폴링하기 때문이다.
 *
 * 마지막에 읽은 상태를 그대로 되써서 지운다. 1 을 쓰면 지워지는 방식이라
 * 지금 선 비트만 정확히 지워진다 — 그 사이 새로 선 비트를 실수로 지우지
 * 않는 관용구다.
 *
 * 항상 IRQ_HANDLED 를 돌려주므로 이 선을 공유할 수 없다. 실제로 probe 가
 * devm_request_irq 로 배타적으로 등록한다.
 *
 * 실행 컨텍스트: 인터럽트(threaded 아님).
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → dw_pcie_ep_linkup() → dra7xx_pcie_writel()
 */
static irqreturn_t dra7xx_pcie_irq_handler(int irq, void *arg)
{
	struct dra7xx_pcie *dra7xx = arg;
	struct dw_pcie *pci = dra7xx->pci;
	/* [한국어] 로그용 device. */
	struct device *dev = pci->dev;
	/* [한국어] EP 모드에서 링크업을 알릴 대상. */
	struct dw_pcie_ep *ep = &pci->ep;
	/* [한국어] 래퍼 상태 레지스터 값. */
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN);
/* [한국어] 주 인터럽트 상태를 읽는다(윗줄). */

	if (reg & ERR_SYS)
		/* [한국어] 이하 대부분은 로그만 남긴다 — 디버깅용 관측 지점인 셈이다. */
		dev_dbg(dev, "System Error\n");

	if (reg & ERR_FATAL)
		/* [한국어] 치명적 오류. */
		dev_dbg(dev, "Fatal Error\n");

	if (reg & ERR_NONFATAL)
		/* [한국어] 치명적이지 않은 오류. */
		dev_dbg(dev, "Non Fatal Error\n");

	if (reg & ERR_COR)
		/* [한국어] 정정 가능한 오류. */
		dev_dbg(dev, "Correctable Error\n");

	if (reg & ERR_AXI)
		/* [한국어] AXI 태그 조회 오류. AXI 는 SoC 내부 버스다. */
		dev_dbg(dev, "AXI tag lookup fatal Error\n");

	if (reg & ERR_ECRC)
		/* [한국어] ECRC(종단 간 CRC) 오류. */
		dev_dbg(dev, "ECRC Error\n");

	if (reg & PME_TURN_OFF)
		/* [한국어] PME_Turn_Off 메시지 수신. */
		dev_dbg(dev,
			"Power Management Event Turn-Off message received\n");

	if (reg & PME_TO_ACK)
		/* [한국어] 그 Ack 수신. */
		dev_dbg(dev,
			"Power Management Turn-Off Ack message received\n");

	if (reg & PM_PME)
		/* [한국어] PM_PME 메시지 수신. */
		dev_dbg(dev, "PM Power Management Event message received\n");

	if (reg & LINK_REQ_RST)
		/* [한국어] 링크 리셋 요청. */
		dev_dbg(dev, "Link Request Reset\n");

	if (reg & LINK_UP_EVT) {
		/* [한국어] 링크업 변화는 유일하게 실제 동작이 있다. EP 모드일 때만 */
		if (dra7xx->mode == DW_PCIE_EP_TYPE)
			/* [한국어] DWC 코어에 링크업을 알린다. 그러면 코어가 EPF 드라이버에 전파한다.
			 * RC 모드에서는 로그만 남기는데, 링크 상태를 DWC 코어가 따로 폴링하기 때문이다. */
			dw_pcie_ep_linkup(ep);
		dev_dbg(dev, "Link-up state change\n");
	/* [한국어] 링크업 처리 끝. */
	}

	if (reg & CFG_BME_EVT)
		/* [한국어] Bus Master Enable 변화. */
		dev_dbg(dev, "CFG 'Bus Master Enable' change\n");

	if (reg & CFG_MSE_EVT)
		/* [한국어] Memory Space Enable 변화. */
		dev_dbg(dev, "CFG 'Memory Space Enable' change\n");

	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN, reg);
/* [한국어] 읽은 상태를 그대로 되써서 지운다(윗줄). 그 사이 새로 선 비트를
 * 실수로 지우지 않는 관용구다. */

	return IRQ_HANDLED;
}

/* [한국어]
 * dra7xx_pcie_init_irq_domain - INTx 용 irq_domain 을 만든다
 *
 * @pp: DWC 코어의 루트 포트.
 * @return: 0 성공, -ENODEV 는 DT 에 자식 노드가 없는 경우,
 *          -EINVAL 은 도메인 생성 실패.
 *
 * DT 자식 노드 하나를 도메인의 식별자로 삼는다. 그 노드가 없으면 이
 * 보드는 INTx 를 쓰지 않는 구성이다.
 *
 * 크기가 PCI_NUM_INTX(4)인 선형 도메인이다. INTA~INTD 뿐이라 충분하다.
 *
 * of_node_put 을 호출 순서에 맞춰 부르는 점을 눈여겨볼 만하다.
 * of_get_next_child 가 참조를 걸어 주므로 도메인을 만든 뒤 반납해야 한다 —
 * 도메인이 fwnode 로 자기 참조를 따로 잡기 때문에 여기서 놓아도 안전하다.
 *
 * artpec6 에는 이 함수에 해당하는 것이 없다. 그쪽은 INTx 를 DWC 코어에
 * 맡기고 이쪽은 직접 떠맡는 것이 두 드라이버의 차이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_add_pcie_port() → [이 함수] → irq_domain_create_linear()
 */
static int dra7xx_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	/* [한국어] 글루 상태를 되찾는다. */
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	/* [한국어] 컨트롤러의 DT 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 그 첫 자식 노드를 도메인 식별자로 삼는다. of_get_next_child 가 참조를
	 * 걸어 주므로 아래에서 반납해야 한다. */
	struct device_node *pcie_intc_node =  of_get_next_child(node, NULL);

	if (!pcie_intc_node) {
		/* [한국어] 자식 노드가 없으면 이 보드는 INTx 를 쓰지 않는 구성이다. */
		dev_err(dev, "No PCIe Intc node found\n");
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	irq_set_chained_handler_and_data(pp->irq, dra7xx_pcie_msi_irq_handler,
					 /* [한국어] 루트 포트를 host_data 로 넘겨(윗줄) 매핑 콜백이 문맥을 되찾게 한다. */
					 pp);
	dra7xx->irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
						      /* [한국어] 크기가 PCI_NUM_INTX(4)인 선형 도메인. INTA~INTD 뿐이라 충분하다. */
						      PCI_NUM_INTX, &intx_domain_ops, pp);
	of_node_put(pcie_intc_node);
	if (!dra7xx->irq_domain) {
		/* [한국어] 도메인 생성에 실패했으면 남기고 */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	return 0;
}

static const struct dw_pcie_host_ops dra7xx_pcie_host_ops = {
	/* [한국어] DWC 코어가 RC 초기화 중 되부를 훅. */
	.init = dra7xx_pcie_host_init,
};

/* [한국어]
 * dra7xx_pcie_ep_init - DWC 코어가 EP 초기화 중 되부르는 훅
 *
 * @ep: DWC 코어의 엔드포인트 구조체.
 * @return: 없음.
 *
 * dw_pcie_ep_init() 이 EP 공통 초기화를 하다가 SoC 고유 부분을 맡기려고
 * 부르는 콜백이다(pcie-designware-ep.c:2734 에서 ep->ops->init()).
 *
 * 하는 일은 한 가지다 — dra7xx_pcie_enable_wrapper_interrupts() 를 불러
 * TI 래퍼 층의 인터럽트를 켠다. DWC IP 바깥에 TI 가 덧붙인 래퍼가 자체
 * 인터럽트 마스크를 갖고 있어, EP 모드에서도 그것을 열어 주어야
 * 링크 상태 변화나 오류를 받을 수 있다.
 *
 * RC 경로는 같은 함수를 dra7xx_pcie_enable_interrupts() 를 통해 부른다.
 * 즉 EP 는 래퍼 인터럽트만 켜고, RC 는 거기에 MSI 인터럽트까지 함께 켠다 —
 * EP 모드에서는 MSI 를 받는 쪽이 아니라 보내는 쪽이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_add_pcie_ep() → dw_pcie_ep_init() [pcie-designware-ep.c:2981]
 *               → :2734 ep->ops->init → [이 함수]
 */
static void dra7xx_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);

	/* [한국어] TI 래퍼 층의 인터럽트 마스크를 연다. DWC IP 자체의 인터럽트와
	 * 별개로 래퍼가 자기 마스크를 갖고 있어, 이것을 열지 않으면 EP 모드에서
	 * 링크 상태 변화나 오류 인터럽트가 올라오지 않는다.
	 * RC 경로는 dra7xx_pcie_enable_interrupts() 를 통해 같은 함수를 부르며,
	 * 거기서는 MSI 인터럽트까지 함께 켠다. */
	dra7xx_pcie_enable_wrapper_interrupts(dra7xx);
}

/* [한국어]
 * dra7xx_pcie_raise_intx_irq - EP 모드에서 INTx 를 한 번 올린다
 *
 * @dra7xx: 이 드라이버의 상태.   @return: 없음.
 *
 * 래퍼의 INTX_ASSERT 레지스터에 쓰고, 잠시 뒤 INTX_DEASSERT 에 써서
 * 내린다. INTx 는 레벨 방식이라 이 상승-하강이 한 번의 인터럽트가 된다.
 *
 * 두 레지스터가 따로 있어 읽고-고쳐-쓰기가 필요 없다 — 쓰기만으로
 * assert 와 deassert 가 각각 일어난다.
 *
 * 사이의 지연이 mdelay 라 바쁘게 기다린다. EP 인터럽트 발생은 드물고
 * 짧아 스케줄러를 부를 이유가 없다는 판단으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(EPF 드라이버의 요청). mdelay 로
 * 바쁘게 기다리므로 그동안 CPU 를 점유한다.
 *
 * 호출 체인:  dra7xx_pcie_raise_irq() → [이 함수] → dra7xx_pcie_writel()
 */
static void dra7xx_pcie_raise_intx_irq(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_INTX_ASSERT, 0x1);
	mdelay(1);
	dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_INTX_DEASSERT, 0x1);
}

/* [한국어]
 * dra7xx_pcie_raise_msi_irq - EP 모드에서 MSI 를 한 번 보낸다
 *
 * @dra7xx:        이 드라이버의 상태.
 * @interrupt_num: 보낼 벡터 번호.
 * @return: 없음.
 *
 * MSI_XMT 레지스터에 벡터 번호와 요청 비트를 함께 써 넣으면 하드웨어가
 * 호스트로 MSI 를 보낸다.
 *
 * 번호를 MSI_VECTOR_SHIFT(7)만큼 밀어 얹고 MSI_REQ_GRANT(0번 비트)를
 * 세우는 구성이다. 한 번의 쓰기로 "이 벡터를 지금 보내라" 가 전달된다.
 *
 * INTx 판과 달리 지연이 없다. MSI 는 메모리 쓰기 하나로 끝나 유지 시간이
 * 필요 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  dra7xx_pcie_raise_irq() → [이 함수] → dra7xx_pcie_writel()
 */
static void dra7xx_pcie_raise_msi_irq(struct dra7xx_pcie *dra7xx,
				      u8 interrupt_num)
{
	u32 reg;

	reg = (interrupt_num - 1) << MSI_VECTOR_SHIFT;
	/* [한국어] 벡터 번호를 자리로 민 값에(윗줄) 전송 요청 비트를 얹는다. */
	reg |= MSI_REQ_GRANT;
	/* [한국어] 한 번의 쓰기로 "이 벡터를 지금 보내라" 가 전달된다. INTx 판과 달리
	 * 지연이 없는데, MSI 는 메모리 쓰기 하나로 끝나 유지 시간이 필요 없어서다. */
	dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_MSI_XMT, reg);
}

/* [한국어]
 * dra7xx_pcie_raise_irq - EP 모드에서 인터럽트 종류에 맞는 방식으로 올린다
 *
 * @ep:            DWC 코어의 엔드포인트 구조체.
 * @func_no:       기능 번호.
 * @type:          PCI_IRQ_INTX 또는 PCI_IRQ_MSI.
 * @interrupt_num: MSI 벡터 번호.
 * @return: 0 성공, -EINVAL 은 지원하지 않는 종류.
 *
 * EPF 드라이버가 호스트를 깨워 달라고 할 때 두 단계를 거쳐 여기까지 온다.
 * EPC 코어의 pci_epc_raise_irq() 가 :484 에서 DWC 코어의
 * dw_pcie_ep_raise_irq() 를 부르고, 그것이 :1774 에서 ep->ops->raise_irq 로
 * 이 함수를 부른다.
 *
 *   PCI_IRQ_INTX - 이 파일의 래퍼 레지스터로 직접 올린다.
 *   PCI_IRQ_MSI  - 두 가지를 한다. 먼저 DWC 코어의
 *                  dw_pcie_ep_raise_msi_irq() 로 표준 MSI 절차를 밟고,
 *                  그 결과가 실패면 래퍼의 MSI_XMT 로 다시 시도한다.
 *                  두 경로를 두는 이유는 이 트리에서 확인 못 함.
 *
 * MSI-X 갈래가 없다. 아래 get_features 가 msix_capable 을 알리지 않으므로
 * EPC 코어가 MSI-X 를 요청하지 않는 것이 전제다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. INTx 경로는 mdelay 로 바쁘게 기다린다.
 *
 * 호출 체인:  EPF → pci_epc_raise_irq() [pci-epc-core.c:484]
 *               → dw_pcie_ep_raise_irq() [pcie-designware-ep.c:1765, :1774]
 *               → [이 함수] → dra7xx_pcie_raise_intx_irq() / _raise_msi_irq()
 */
static int dra7xx_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				 unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
/* [한국어] 인터럽트 종류로 갈린다. */

	switch (type) {
	/* [한국어] 레거시 핀 인터럽트면 */
	case PCI_IRQ_INTX:
		/* [한국어] 래퍼 레지스터로 직접 올린다. */
		dra7xx_pcie_raise_intx_irq(dra7xx);
		break;
	case PCI_IRQ_MSI:
		/* [한국어] MSI 면 DWC 코어의 표준 절차를 먼저 밟고, 실패하면 래퍼로 다시 시도한다
		 * (윗줄이 그 첫 시도). 두 경로를 두는 이유는 이 트리에서 확인 못 함. */
		dra7xx_pcie_raise_msi_irq(dra7xx, interrupt_num);
		break;
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features dra7xx_pcie_epc_features = {
	/* [한국어] DWC 공통 기능 묶음. 헤더가 정의한 매크로다. */
	DWC_EPC_COMMON_FEATURES,
	.linkup_notifier = true,
	.msi_capable = true,
};

/* [한국어]
 * dra7xx_pcie_get_features - 이 EP 하드웨어의 능력 표를 돌려준다
 *
 * @ep: DWC 코어의 엔드포인트 구조체. 쓰지 않는다.
 * @return: 고정된 dra7xx_pcie_epc_features 의 주소.
 *
 * EPF 드라이버가 "이 컨트롤러로 무엇을 할 수 있는가" 를 물으면 두 단계를
 * 거쳐 여기까지 온다. EPC 코어의 pci_epc_get_features() 가 :381 에서
 * DWC 코어의 dw_pcie_ep_get_features() 를 부르고, 그것이 :1882 에서
 * ep->ops->get_features 로 이 함수를 부른다. DWC 코어 안의
 * dw_pcie_ep_set_bar() / dw_pcie_ep_disable_bars() 경로(:1142)도 같은
 * 콜백을 쓴다.
 *
 * 인자를 무시하고 항상 같은 구조체를 돌려주는데, 이 하드웨어의 제약이
 * 기능마다 다르지 않기 때문이다.
 *
 * 알리는 내용은 바로 위 구조체 정의에 있다 — DWC 공통 기능,
 * 링크업 알림 지원, MSI 지원이다. msix_capable 이 없어 EPC 코어가
 * MSI-X 를 요청하지 않으며, 그래서 dra7xx_pcie_raise_irq() 에도
 * MSI-X 갈래가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  EPF → pci_epc_get_features() [pci-epc-core.c:381]
 *               → dw_pcie_ep_get_features() [pcie-designware-ep.c:1873, :1882]
 *               → [이 함수]
 */
static const struct pci_epc_features*
dra7xx_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &dra7xx_pcie_epc_features;
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {
	/* [한국어] EP 초기화 중 되불릴 훅. */
	.init = dra7xx_pcie_ep_init,
	/* [한국어] 인터럽트 발생 훅. 이 표에 없는 콜백은 DWC 코어가 직접 처리한다. */
	.raise_irq = dra7xx_pcie_raise_irq,
	.get_features = dra7xx_pcie_get_features,
};

/* [한국어]
 * dra7xx_add_pcie_ep - EP 모드로 DWC 코어를 띄운다
 *
 * @dra7xx: 이 드라이버의 상태.   @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno.
 *
 * EP 모드 probe 의 뒷부분을 맡는다.
 *   1) ep_ops 를 걸어 DWC 코어가 이 파일의 세 콜백(init/raise_irq/
 *      get_features)을 되부를 수 있게 한다.
 *   2) "ep_dbics" 와 "ep_dbics2" 자원을 매핑해 DWC IP 의 config 창을 잡는다.
 *      dbi 는 DesignWare 의 설정 인터페이스 이름이다.
 *   3) "addr_space" 자원에서 아웃바운드 창으로 쓸 물리 주소 구간을 받는다.
 *   4) dw_pcie_ep_init() 으로 DWC 코어의 EP 초기화를 시작한다. 그 안에서
 *      이 파일의 dra7xx_pcie_ep_init() 이 되불린다.
 *   5) dw_pcie_ep_init_registers() 로 EP 레지스터를 세우고,
 *      실패하면 dw_pcie_ep_deinit() 으로 되돌린다.
 *   6) pci_epc_init_notify() 로 EPF 드라이버에 준비 완료를 알린다.
 *      이 시점부터 EPF 가 BAR 를 세우고 인터럽트를 올릴 수 있다.
 *
 * RC 판(dra7xx_add_pcie_port)과 달리 인터럽트 도메인을 만들지 않는다 —
 * EP 는 인터럽트를 받는 쪽이 아니라 보내는 쪽이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_pcie_probe() → [이 함수] → dw_pcie_ep_init()
 *               → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 */
static int dra7xx_add_pcie_ep(struct dra7xx_pcie *dra7xx,
			      struct platform_device *pdev)
{
	int ret;
	struct dw_pcie_ep *ep;
	/* [한국어] 로그와 자원 조회의 기준. */
	struct device *dev = &pdev->dev;
	/* [한국어] DWC 장치 구조체. */
	struct dw_pcie *pci = dra7xx->pci;
/* [한국어] 아래에서 EP 콜백 표를 건다. */

	ep = &pci->ep;
	/* [한국어] 이 한 줄이 DWC 코어가 이 파일의 세 콜백을 되부를 수 있게 한다. */
	ep->ops = &pcie_ep_ops;
/* [한국어] 아래에서 DWC IP 의 config 창을 매핑한다. */

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "ep_dbics");
	/* [한국어] dbi 창 매핑에 실패했으면 */
	if (IS_ERR(pci->dbi_base))
		/* [한국어] 그 오류를 전한다. dbi 는 DesignWare 의 설정 인터페이스 이름이다. */
		return PTR_ERR(pci->dbi_base);
/* [한국어] 아래에서 둘째 dbi 창도 매핑한다. */

	pci->dbi_base2 =
		/* [한국어] ep_dbics2 는 EP 모드에서 쓰는 보조 config 창이다. */
		devm_platform_ioremap_resource_byname(pdev, "ep_dbics2");
	if (IS_ERR(pci->dbi_base2))
		/* [한국어] 매핑에 실패하면 그 오류를 전한다. */
		return PTR_ERR(pci->dbi_base2);

	ret = dw_pcie_ep_init(ep);
	/* [한국어] DWC 코어의 EP 초기화에 실패했으면(윗줄) */
	if (ret) {
		/* [한국어] 남기고 */
		dev_err(dev, "failed to initialize endpoint\n");
		/* [한국어] errno 를 전한다. 이 호출 안에서 이 파일의 dra7xx_pcie_ep_init() 이 되불린다. */
		return ret;
	}

	ret = dw_pcie_ep_init_registers(ep);
	/* [한국어] EP 레지스터 세우기에 실패했으면(윗줄) */
	if (ret) {
		/* [한국어] 남기고 */
		dev_err(dev, "Failed to initialize DWC endpoint registers\n");
		/* [한국어] 코어 초기화를 되돌린다. 이 되돌리기가 있어야 자원이 새지 않는다. */
		dw_pcie_ep_deinit(ep);
		return ret;
	}

	pci_epc_init_notify(ep->epc);

	return 0;
}

/* [한국어]
 * dra7xx_add_pcie_port - RC 모드로 DWC 코어를 띄운다
 *
 * @dra7xx: 이 드라이버의 상태.   @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno.
 *
 * RC 모드 probe 의 뒷부분이다.
 *   1) "intr" IRQ 를 받아 dra7xx_pcie_msi_irq_handler() 를 체인 핸들러로
 *      걸 준비를 한다.
 *   2) CONFIG_PCI_MSI 가 꺼진 빌드에서만 INTx 도메인을 만든다. 켜진
 *      빌드에서는 DWC 코어의 MSI 도메인이 그 역할을 겸한다.
 *   3) host_ops 를 걸고 dw_pcie_host_init() 으로 넘긴다. 그 안에서
 *      dra7xx_pcie_host_init() 이 되불려 인터럽트를 켜고, 이어서
 *      dw_pcie_ops.start_link(= establish_link)가 링크를 시작한다.
 *
 * 체인 핸들러를 거는 순서가 요점이다. 도메인을 먼저 만들고 핸들러를
 * 걸어야, 인터럽트가 오자마자 넘길 곳이 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_pcie_probe() → [이 함수] → dra7xx_pcie_init_irq_domain()
 *               → dw_pcie_host_init() [DWC 코어]
 */
static int dra7xx_add_pcie_port(struct dra7xx_pcie *dra7xx,
				struct platform_device *pdev)
{
	int ret;
	struct dw_pcie *pci = dra7xx->pci;
	/* [한국어] DWC 코어의 루트 포트. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 로그용 device. */
	struct device *dev = pci->dev;

	pp->irq = platform_get_irq(pdev, 1);
	/* [한국어] "intr" IRQ 를 얻지 못했으면(윗줄) */
	if (pp->irq < 0)
		/* [한국어] 그 errno 를 전한다. */
		return pp->irq;

	/* MSI IRQ is muxed */
	pp->msi_irq[0] = -ENODEV;

	ret = dra7xx_pcie_init_irq_domain(pp);
	/* [한국어] 체인 핸들러 등록이나 도메인 생성에 실패했으면(윗줄) */
	if (ret < 0)
		/* [한국어] errno 를 전한다. */
		return ret;

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "rc_dbics");
	/* [한국어] RC 용 dbi 창 매핑에 실패했으면 */
	if (IS_ERR(pci->dbi_base))
		/* [한국어] 그 오류를 전한다. */
		return PTR_ERR(pci->dbi_base);
/* [한국어] 아래에서 host_ops 를 걸고 DWC 코어에 넘긴다. */

	pp->ops = &dra7xx_pcie_host_ops;
/* [한국어] 이 한 줄이 코어가 dra7xx_pcie_host_init() 을 되부를 수 있게 한다. */

	ret = dw_pcie_host_init(pp);
	/* [한국어] 호스트 초기화에 실패했으면 */
	if (ret) {
		/* [한국어] 남기고 */
		dev_err(dev, "failed to initialize host\n");
		/* [한국어] errno 를 전한다. 이 호출 안에서 인터럽트가 켜지고 링크 훈련이 시작된다. */
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] CPU 주소를 버스 주소로 바꾸는 훅. */
	.cpu_addr_fixup = dra7xx_pcie_cpu_addr_fixup,
	/* [한국어] 링크 훈련 시작 훅. 아래 stop_link 와 link_up 까지 셋이 DWC 코어와의 계약이다. */
	.start_link = dra7xx_pcie_establish_link,
	.stop_link = dra7xx_pcie_stop_link,
	.link_up = dra7xx_pcie_link_up,
};

/* [한국어]
 * dra7xx_pcie_disable_phy - 레인마다 PHY 를 끈다
 *
 * @dra7xx: 이 드라이버의 상태.   @return: 없음.
 *
 * phy_count 만큼 역순으로 훑으며 전원을 내리고 종료한다.
 *
 * 역순인 것이 의도적이다. enable 이 0번부터 켰으므로 끄는 것은 마지막
 * 레인부터 하는 것이 대칭이며, 레인 사이에 순서 의존이 있어도 안전하다.
 *
 * phy_power_off 와 phy_exit 를 짝지어 부르는데, enable 판이
 * phy_init + phy_power_on 순으로 켜는 것의 정확한 역이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  dra7xx_pcie_enable_phy() 의 에러 경로 / probe 에러 경로 /
 *             shutdown → [이 함수] → phy_power_off() → phy_exit()
 */
static void dra7xx_pcie_disable_phy(struct dra7xx_pcie *dra7xx)
{
	int phy_count = dra7xx->phy_count;

	while (phy_count--) {
		/* [한국어] 전원을 내리고 종료한다(윗줄이 인덱스 감소). 역순인 것이 의도적이며,
		 * enable 이 0번부터 켠 것의 대칭이다. */
		phy_power_off(dra7xx->phy[phy_count]);
		phy_exit(dra7xx->phy[phy_count]);
	}
}

/* [한국어]
 * dra7xx_pcie_enable_phy - 레인마다 PHY 를 켠다
 *
 * @dra7xx: 이 드라이버의 상태.
 * @return: 0 성공, phy_init/phy_power_on 의 음수 errno.
 *
 * 레인 하나에 PHY 하나가 대응하므로 phy_count 만큼 반복한다.
 * 각 레인에 phy_init 으로 초기화하고 phy_power_on 으로 전원을 넣는다.
 *
 * 에러 처리가 촘촘하다. init 이 실패하면 그 자리에서 빠지고,
 * power_on 이 실패하면 방금 성공한 init 을 phy_exit 로 되돌린 뒤 빠진다.
 * 어느 경우든 err_phy 라벨로 가서 이미 켠 앞쪽 레인들을 정리한다.
 *
 * i 를 하나 줄여 놓고 정리 루프로 들어가는 것이 요점이다 — 실패한
 * 레인은 이미 자기 자리에서 되돌렸으므로 그 앞까지만 정리하면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 초기화에서 잠들 수 있다.
 *
 * 호출 체인:  dra7xx_pcie_probe() → [이 함수] → phy_init() → phy_power_on()
 */
static int dra7xx_pcie_enable_phy(struct dra7xx_pcie *dra7xx)
{
	int phy_count = dra7xx->phy_count;
	int ret;
	/* [한국어] 레인 반복자. */
	int i;

	for (i = 0; i < phy_count; i++) {
		/* [한국어] 레인마다 PHY 모드를 PCIe 로 지정한다. */
		ret = phy_set_mode(dra7xx->phy[i], PHY_MODE_PCIE);
		/* [한국어] 실패하면 */
		if (ret < 0)
			/* [한국어] 정리 경로로 간다. */
			goto err_phy;

		ret = phy_init(dra7xx->phy[i]);
		/* [한국어] 초기화에 실패해도(윗줄) */
		if (ret < 0)
			/* [한국어] 같은 정리 경로로. */
			goto err_phy;

		ret = phy_power_on(dra7xx->phy[i]);
		/* [한국어] 전원 켜기에 실패했으면(윗줄) */
		if (ret < 0) {
			/* [한국어] 방금 성공한 초기화를 되돌린 뒤 정리 경로로 간다. 실패한 레인은
			 * 자기 자리에서 되돌렸으므로 정리 루프는 그 앞까지만 맡는다. */
			phy_exit(dra7xx->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(dra7xx->phy[i]);
		phy_exit(dra7xx->phy[i]);
	}

	return ret;
}

static const struct dra7xx_pcie_of_data dra7xx_pcie_rc_of_data = {
	/* [한국어] DRA7xx 기본 RC 구성. 2레인 마스크가 없어 1레인 전용이다. */
	.mode = DW_PCIE_RC_TYPE,
/* [한국어] of_data 끝. */
};

static const struct dra7xx_pcie_of_data dra7xx_pcie_ep_of_data = {
	/* [한국어] 같은 SoC 의 EP 구성. 모드만 다르고 나머지는 같다. */
	.mode = DW_PCIE_EP_TYPE,
/* [한국어] of_data 끝. */
};

static const struct dra7xx_pcie_of_data dra746_pcie_rc_of_data = {
	/* [한국어] 이 변종은 레인 선택 비트가 2번이다. */
	.b1co_mode_sel_mask = BIT(2),
	/* [한국어] RC 모드. */
	.mode = DW_PCIE_RC_TYPE,
};

static const struct dra7xx_pcie_of_data dra726_pcie_rc_of_data = {
	/* [한국어] 이 변종은 마스크가 두 비트(3:2)다. 변종마다 비트 위치와 폭이 달라
	 * of_data 가 그 값을 들고 다닌다. */
	.b1co_mode_sel_mask = GENMASK(3, 2),
	/* [한국어] RC 모드. */
	.mode = DW_PCIE_RC_TYPE,
};

static const struct dra7xx_pcie_of_data dra746_pcie_ep_of_data = {
	/* [한국어] 또 다른 변종의 레인 선택 비트. */
	.b1co_mode_sel_mask = BIT(2),
	.mode = DW_PCIE_EP_TYPE,
};

static const struct dra7xx_pcie_of_data dra726_pcie_ep_of_data = {
	/* [한국어] 두 비트 마스크를 쓰는 변종의 EP 판. */
	.b1co_mode_sel_mask = GENMASK(3, 2),
	/* [한국어] EP 모드. 위 RC 판들과 짝을 이뤄, 변종마다 RC/EP 두 항목이 있다. */
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id of_dra7xx_pcie_match[] = {
	/* [한국어] compatible 문자열과 위 of_data 를 잇는 표. 이 표의 data 가 probe 에서
	 * 모드와 레인 마스크의 유일한 근거가 된다. */
	{
		.compatible = "ti,dra7-pcie",
		/* [한국어] 기본 RC. */
		.data = &dra7xx_pcie_rc_of_data,
	},
	{
		.compatible = "ti,dra7-pcie-ep",
		/* [한국어] 기본 EP. */
		.data = &dra7xx_pcie_ep_of_data,
	},
	{
		.compatible = "ti,dra746-pcie-rc",
		/* [한국어] DRA746 의 RC. 이 변종부터 2레인 구성을 지원한다. */
		.data = &dra746_pcie_rc_of_data,
	},
	{
		.compatible = "ti,dra726-pcie-rc",
		/* [한국어] DRA726 의 RC. 마스크 폭이 달라 별도 항목이다. */
		.data = &dra726_pcie_rc_of_data,
	},
	{
		.compatible = "ti,dra746-pcie-ep",
		/* [한국어] DRA746 의 EP. */
		.data = &dra746_pcie_ep_of_data,
	},
	{
		.compatible = "ti,dra726-pcie-ep",
		/* [한국어] DRA726 의 EP. 변종 3종 × 모드 2종으로 여섯 항목이 된다. */
		.data = &dra726_pcie_ep_of_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_dra7xx_pcie_match);

/*
 * dra7xx_pcie_unaligned_memaccess: workaround for AM572x/AM571x Errata i870
 * @dra7xx: the dra7xx device where the workaround should be applied
 *
 * Access to the PCIe slave port that are not 32-bit aligned will result
 * in incorrect mapping to TLP Address and Byte enable fields. Therefore,
 * byte and half-word accesses are not possible to byte offset 0x1, 0x2, or
 * 0x3.
 *
 * To avoid this issue set PCIE_SS1_AXI2OCP_LEGACY_MODE_ENABLE to 1.
 */
/* [한국어]
 * dra7xx_pcie_unaligned_memaccess - 정렬되지 않은 메모리 접근을 허용한다
 *
 * @dev: 이 드라이버의 device.
 * @return: 0 성공, -EINVAL 은 DT 에 해당 phandle 이 없는 경우,
 *          그 밖에는 regmap 갱신의 errno.
 *
 * DT 의 ti,syscon-unaligned-access phandle 로 syscon regmap 과 인자 둘
 * (레지스터 오프셋과 비트 마스크)을 받아, 그 비트를 세운다.
 *
 * 이 SoC 의 기본 설정에서는 PCIe 를 통한 정렬되지 않은 접근이 막혀
 * 있는 것으로 보이며, 그것을 푸는 우회다. 다만 왜 기본이 막혀 있고
 * 어떤 증상이 나는지는 이 트리의 코드와 주석만으로 확인하지 못했다.
 *
 * phandle 이 없을 때 dev_dbg 로만 남기고 -EINVAL 을 돌려주는 점이
 * 눈에 띈다. 호출자인 probe 가 그 실패를 치명적으로 다루는지가
 * 동작을 가르는데, 보드에 따라 이 속성이 없는 것이 정상일 수 있어
 * 로그 수준을 낮춘 것으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_pcie_probe() → [이 함수]
 *               → syscon_regmap_lookup_by_phandle_args() → regmap_update_bits()
 */
static int dra7xx_pcie_unaligned_memaccess(struct device *dev)
{
	int ret;
	struct device_node *np = dev->of_node;
	/* [한국어] syscon 레지스터 오프셋과 비트 마스크를 받을 배열. */
	unsigned int args[2];
	/* [한국어] 그 레지스터를 담은 regmap. */
	struct regmap *regmap;

	regmap = syscon_regmap_lookup_by_phandle_args(np, "ti,syscon-unaligned-access",
						      /* [한국어] DT 의 ti,syscon-unaligned-access 에서 regmap 과 인자 둘을 함께 얻는다(윗줄). */
						      2, args);
	if (IS_ERR(regmap)) {
		/* [한국어] 속성이 없으면 dev_dbg 로만 남긴다 — 보드에 따라 없는 것이 정상일 수 있어
		 * 로그 수준을 낮춘 것으로 보인다. */
		dev_dbg(dev, "can't get ti,syscon-unaligned-access\n");
		/* [한국어] 인자 오류로 답한다. */
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, args[0], args[1], args[1]);
	/* [한국어] 비트 갱신에 실패했으면(윗줄이 그 호출) */
	if (ret)
		/* [한국어] 오류를 남긴다. */
		dev_err(dev, "failed to enable unaligned access\n");
/* [한국어] 성패를 그대로 전한다. */

	return ret;
}

/* [한국어]
 * dra7xx_pcie_configure_two_lane - 컨트롤러를 2레인 모드로 구성한다
 *
 * @dev:               이 드라이버의 device.
 * @b1co_mode_sel_mask: 변종마다 다른 레인 선택 비트 마스크.
 * @return: 0 성공, -EINVAL 은 DT 에 phandle 이 없는 경우.
 *
 * 이 SoC 는 PCIe 레인을 1레인 둘로 쓸지 2레인 하나로 쓸지 고를 수 있고,
 * 그 선택이 PCIe IP 밖의 syscon 레지스터에 있다. DT 의
 * ti,syscon-lane-sel phandle 로 그 regmap 과 오프셋을 받아 설정한다.
 *
 * 마스크와 값을 나눠 계산하는 것이 요점이다. 마스크에는 변종별 선택
 * 비트와 TSYNCEN 을 함께 넣고, 값에는 B1C0_MODE_SEL 과 TSYNCEN 을 넣는다.
 * 그러면 변종별 비트는 0 으로 지워지고 나머지 둘은 1 로 세워진다 —
 * 한 번의 update_bits 로 서로 다른 방향의 두 설정을 함께 적용한다.
 *
 * b1co_mode_sel_mask 가 compatible 마다 다른 이유는 변종별로 그 비트
 * 위치가 달라서다. of_data 표가 그 값을 들고 있다.
 *
 * regmap_update_bits 의 반환값을 확인하지 않고 0 을 돌려준다.
 * 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  dra7xx_pcie_probe() → [이 함수] → regmap_update_bits()
 */
static int dra7xx_pcie_configure_two_lane(struct device *dev,
					  u32 b1co_mode_sel_mask)
{
	struct device_node *np = dev->of_node;
	struct regmap *pcie_syscon;
	/* [한국어] 레인 선택 레지스터의 오프셋을 받을 변수. */
	unsigned int pcie_reg;
	/* [한국어] 갱신할 비트 마스크. */
	u32 mask;
	/* [한국어] 그 자리에 쓸 값. */
	u32 val;

	pcie_syscon = syscon_regmap_lookup_by_phandle_args(np, "ti,syscon-lane-sel",
							   /* [한국어] DT 의 ti,syscon-lane-sel 에서 regmap 과 오프셋을 얻는다(윗줄). */
							   1, &pcie_reg);
	if (IS_ERR(pcie_syscon)) {
		/* [한국어] 없으면 2레인 구성을 할 수 없다. */
		dev_err(dev, "unable to get ti,syscon-lane-sel\n");
		/* [한국어] 인자 오류로 답한다. */
		return -EINVAL;
	}

	mask = b1co_mode_sel_mask | PCIE_B0_B1_TSYNCEN;
	/* [한국어] 값에는 B1C0_MODE_SEL 과 TSYNCEN 을 넣는다(윗줄이 마스크).
	 * 마스크에는 변종별 비트와 TSYNCEN 이 들어가므로, 한 번의 갱신으로
	 * 변종별 비트는 0 으로 지워지고 나머지 둘은 1 로 세워진다. */
	val = PCIE_B1C0_MODE_SEL | PCIE_B0_B1_TSYNCEN;
	/* [한국어] 서로 다른 방향의 두 설정을 한 번에 적용한다. 반환값을 확인하지 않고
	 * 아래에서 0 을 돌려준다 — 코드는 고치지 않고 관찰만 적어 둔다. */
	regmap_update_bits(pcie_syscon, pcie_reg, mask, val);
/* [한국어] 아래에서 성공을 알린다. */

	return 0;
}

/* [한국어]
 * dra7xx_pcie_probe - 진입점. 모드를 정하고 자원을 모아 DWC 코어에 넘긴다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno.
 *
 * 이 파일의 전체 흐름이 담긴 함수다.
 *
 *   1) compatible 의 match data 에서 모드(RC/EP)와 레인 선택 마스크를 얻는다.
 *      이 한 값이 이후 모든 분기를 가른다.
 *   2) 래퍼 레지스터 창("ti_conf")과 IRQ 두 개를 받는다.
 *   3) PHY 를 레인 수만큼 받아 켠다.
 *   4) 런타임 PM 을 켜고 참조를 잡아 클럭과 전원을 붙인다.
 *   5) 2레인 구성이 필요하면 syscon 으로 설정한다.
 *   6) 모드에 따라 갈린다.
 *      RC  - 장치 타입을 DEVICE_TYPE_RC 로 쓰고 dra7xx_add_pcie_port().
 *      EP  - 정렬 접근 우회를 적용하고, 장치 타입을 DEVICE_TYPE_EP 로 쓰고
 *            dra7xx_add_pcie_ep(). 이 우회가 EP 에서만 필요한 이유는
 *            이 트리에서 확인 못 함.
 *   7) 오류 인터럽트 핸들러를 걸고 래퍼 인터럽트를 켠다.
 *
 * 장치 타입 레지스터를 쓰는 시점이 중요하다. LTSSM 을 켜기 전에 정해야
 * 하며, 그래서 DWC 코어에 넘기기 직전에 쓴다.
 *
 * 에러 경로가 여러 단계로 나뉜 것은 어느 지점에서 실패했느냐에 따라
 * 되돌릴 것이 다르기 때문이다 — PHY, 런타임 PM 참조, PM 활성화 순으로 쌓인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩). PHY 초기화와 링크
 * 대기에서 잠든다.
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → dra7xx_pcie_enable_phy()
 *               → dra7xx_add_pcie_port() 또는 dra7xx_add_pcie_ep()
 */
static int dra7xx_pcie_probe(struct platform_device *pdev)
{
	u32 reg;
	int ret;
	/* [한국어] 오류 인터럽트 번호. */
	int irq;
	/* [한국어] PHY 반복자. */
	int i;
	/* [한국어] DT 가 말하는 PHY(=레인) 수. */
	int phy_count;
	/* [한국어] PHY 핸들 배열. */
	struct phy **phy;
	/* [한국어] 각 PHY 와 맺을 device_link 배열. 전원 순서 의존을 커널에 알리는 장치다. */
	struct device_link **link;
	/* [한국어] 래퍼 레지스터 창. */
	void __iomem *base;
	/* [한국어] DWC 장치 구조체. */
	struct dw_pcie *pci;
	/* [한국어] 이 드라이버의 상태. */
	struct dra7xx_pcie *dra7xx;
	/* [한국어] 로그와 devm 할당의 기준. */
	struct device *dev = &pdev->dev;
	/* [한국어] 컨트롤러의 DT 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] "pcie-phyN" 이름을 조립할 버퍼. */
	char name[10];
	/* [한국어] 리셋 GPIO. */
	struct gpio_desc *reset;
	/* [한국어] compatible 에 묶인 변종 정보. */
	const struct dra7xx_pcie_of_data *data;
	/* [한국어] 이 인스턴스의 동작 모드. */
	enum dw_pcie_device_mode mode;
	/* [한국어] 2레인 선택 비트 마스크. */
	u32 b1co_mode_sel_mask;

	data = of_device_get_match_data(dev);
	/* [한국어] match data 를 얻지 못했으면(윗줄) 이 compatible 이 표에 없다는 뜻이라 */
	if (!data)
		/* [한국어] 인자 오류로 답한다. */
		return -EINVAL;

	mode = (enum dw_pcie_device_mode)data->mode;
	/* [한국어] 모드와 레인 마스크를 꺼낸다(윗줄이 모드). 이 두 값이 이후 모든 분기를 가른다. */
	b1co_mode_sel_mask = data->b1co_mode_sel_mask;
/* [한국어] 아래에서 상태 구조체를 할당한다. */

	dra7xx = devm_kzalloc(dev, sizeof(*dra7xx), GFP_KERNEL);
	/* [한국어] 할당 실패면 */
	if (!dra7xx)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] DWC 장치 구조체 할당 실패면 */
	if (!pci)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	pci->dev = dev;
	/* [한국어] DWC 코어가 되부를 세 콜백(cpu_addr_fixup/start_link/link_up)을 건다. */
	pci->ops = &dw_pcie_ops;
/* [한국어] 아래에서 자원을 모은다. */

	irq = platform_get_irq(pdev, 0);
	/* [한국어] 오류 인터럽트를 얻지 못했으면 */
	if (irq < 0)
		/* [한국어] 그 errno 를 전한다. */
		return irq;

	base = devm_platform_ioremap_resource_byname(pdev, "ti_conf");
	/* [한국어] 래퍼 창 매핑에 실패했으면 */
	if (IS_ERR(base))
		/* [한국어] 그 오류를 전한다. */
		return PTR_ERR(base);

	phy_count = of_property_count_strings(np, "phy-names");
	/* [한국어] DT 의 phy-names 를 세지 못했으면(윗줄) */
	if (phy_count < 0) {
		/* [한국어] 남기고 */
		dev_err(dev, "unable to find the strings\n");
		/* [한국어] 그 errno 를 전한다. */
		return phy_count;
	/* [한국어] 조건 블록 끝. */
	}

	phy = devm_kcalloc(dev, phy_count, sizeof(*phy), GFP_KERNEL);
	/* [한국어] PHY 배열 할당 실패면 */
	if (!phy)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	link = devm_kcalloc(dev, phy_count, sizeof(*link), GFP_KERNEL);
	/* [한국어] device_link 배열 할당 실패면 */
	if (!link)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	dra7xx->clk = devm_clk_get_optional(dev, NULL);
	/* [한국어] 클럭을 얻지 못했으면(윗줄) */
	if (IS_ERR(dra7xx->clk))
		/* [한국어] probe 지연(-EPROBE_DEFER)까지 적절히 다루는 헬퍼로 오류를 전한다. */
		return dev_err_probe(dev, PTR_ERR(dra7xx->clk),
				     /* [한국어] 클럭 공급자가 아직 준비되지 않았을 수 있어 이 헬퍼가 맞다. */
				     "clock request failed");

	ret = clk_prepare_enable(dra7xx->clk);
	/* [한국어] 클럭 켜기에 실패했으면(윗줄) */
	if (ret)
		/* [한국어] errno 를 전한다. */
		return ret;

	for (i = 0; i < phy_count; i++) {
		/* [한국어] 레인마다 "pcie-phyN" 이름을 만들어 */
		snprintf(name, sizeof(name), "pcie-phy%d", i);
		/* [한국어] PHY 를 얻는다. */
		phy[i] = devm_phy_get(dev, name);
		/* [한국어] 실패하면 */
		if (IS_ERR(phy[i]))
			/* [한국어] 그 오류를 전한다. */
			return PTR_ERR(phy[i]);
/* [한국어] 아래에서 device_link 를 맺는다. */

		link[i] = device_link_add(dev, &phy[i]->dev, DL_FLAG_STATELESS);
		/* [한국어] 링크 생성에 실패하면(윗줄) */
		if (!link[i]) {
			/* [한국어] 인자 오류로 기록하고 정리 경로로 간다. 이 링크가 있어야 PHY 가
			 * 이 장치보다 먼저 꺼지지 않는다. */
			ret = -EINVAL;
			goto err_link;
		}
	}

	dra7xx->base = base;
	/* [한국어] 모은 것들을 상태 구조체에 옮긴다 — PHY 배열, */
	dra7xx->phy = phy;
	/* [한국어] DWC 장치, */
	dra7xx->pci = pci;
	/* [한국어] 레인 수. */
	dra7xx->phy_count = phy_count;

	if (phy_count == 2) {
		/* [한국어] 2레인 마스크가 있는 변종이면(윗줄) 그 구성을 시도한다. */
		ret = dra7xx_pcie_configure_two_lane(dev, b1co_mode_sel_mask);
		/* [한국어] 실패하면 */
		if (ret < 0)
			/* [한국어] 1레인으로 물러난다. 상류 주석대로 fallback 이며, 오류로 만들지 않는다 —
			 * 1레인으로도 동작하기 때문이다. */
			dra7xx->phy_count = 1; /* Fallback to x1 lane mode */
	/* [한국어] 2레인 처리 끝. */
	}

	ret = dra7xx_pcie_enable_phy(dra7xx);
	/* [한국어] PHY 켜기에 실패했으면(윗줄) */
	if (ret) {
		/* [한국어] 남기고 */
		dev_err(dev, "failed to enable phy\n");
		/* [한국어] errno 를 전한다. */
		return ret;
	}

	platform_set_drvdata(pdev, dra7xx);
/* [한국어] 아래에서 런타임 PM 을 켜고 참조를 잡는다. */

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 참조 획득에 실패했으면 */
	if (ret < 0) {
		/* [한국어] 남기고 */
		dev_err(dev, "pm_runtime_get_sync failed\n");
		/* [한국어] PM 을 되돌리는 경로로 간다. */
		goto err_get_sync;
	}

	reset = devm_gpiod_get_optional(dev, NULL, GPIOD_OUT_HIGH);
	/* [한국어] 리셋 GPIO 요청에 실패했으면(윗줄) */
	if (IS_ERR(reset)) {
		/* [한국어] 오류를 꺼내 */
		ret = PTR_ERR(reset);
		/* [한국어] 남기고 */
		dev_err(&pdev->dev, "gpio request failed, ret %d\n", ret);
		/* [한국어] 정리 경로로 간다. */
		goto err_gpio;
	}

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	/* [한국어] LTSSM 을 꺼 둔다. 장치 타입을 정하기 전에 링크가 돌면 안 되기 때문이다. */
	reg &= ~LTSSM_EN;
	/* [한국어] 고친 값을 쓴다. */
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);

	switch (mode) {
	/* [한국어] 모드에 따라 갈린다. RC 면 */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 그 빌드 지원이 꺼져 있으면 진행할 수 없다. */
		if (!IS_ENABLED(CONFIG_PCI_DRA7XX_HOST)) {
			ret = -ENODEV;
			goto err_gpio;
		}

		dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_DEVICE_TYPE,
				   /* [한국어] 장치 타입을 RC 로 쓴다(윗줄). LTSSM 을 켜기 전에 정해야 한다. */
				   DEVICE_TYPE_RC);

		ret = dra7xx_pcie_unaligned_memaccess(dev);
		/* [한국어] 정렬 접근 우회 적용에 실패했으면(윗줄) */
		if (ret)
			/* [한국어] 경고만 남기고 계속한다 — Errata i870 우회라 없어도 동작은 한다. */
			dev_err(dev, "WA for Errata i870 not applied\n");
/* [한국어] 아래에서 RC 경로를 시작한다. */

		ret = dra7xx_add_pcie_port(dra7xx, pdev);
		/* [한국어] RC 초기화에 실패했으면 */
		if (ret < 0)
			/* [한국어] 정리 경로로 간다. */
			goto err_gpio;
		break;
	case DW_PCIE_EP_TYPE:
		/* [한국어] EP 면 그 빌드 지원을 확인한다. */
		if (!IS_ENABLED(CONFIG_PCI_DRA7XX_EP)) {
			ret = -ENODEV;
			goto err_gpio;
		}

		dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_DEVICE_TYPE,
				   /* [한국어] 장치 타입을 EP 로 쓴다(윗줄). */
				   DEVICE_TYPE_EP);

		ret = dra7xx_pcie_unaligned_memaccess(dev);
		/* [한국어] 정렬 접근 우회가 실패하면(윗줄) EP 에서는 치명적으로 다뤄 */
		if (ret)
			/* [한국어] 정리 경로로 간다. RC 가 경고만 남기는 것과 다른데, 그 이유는
			 * 이 트리에서 확인 못 함. */
			goto err_gpio;

		ret = dra7xx_add_pcie_ep(dra7xx, pdev);
		/* [한국어] EP 초기화에 실패했으면 */
		if (ret < 0)
			/* [한국어] 정리 경로로. */
			goto err_gpio;
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
	}
	dra7xx->mode = mode;
/* [한국어] 아래에서 오류 인터럽트 핸들러를 건다. */

	ret = devm_request_threaded_irq(dev, irq, NULL, dra7xx_pcie_irq_handler,
					/* [한국어] IRQF_SHARED 는 이 선을 다른 장치와 공유할 수 있다는 뜻이고,
					 * IRQF_ONESHOT 은 threaded 핸들러가 끝날 때까지 마스크를 유지한다는 뜻이다.
					 * 다만 이 핸들러는 threaded 가 아니어서 ONESHOT 의 효과가 제한적이다 —
					 * 그 의도는 이 트리에서 확인 못 함. */
					IRQF_SHARED | IRQF_ONESHOT,
					"dra7xx-pcie-main", dra7xx);
	if (ret) {
		/* [한국어] 등록에 실패했으면 남기고 */
		dev_err(dev, "failed to request irq\n");
		/* [한국어] DWC 초기화까지 되돌리는 경로로 간다. */
		goto err_deinit;
	}

	return 0;

err_deinit:
	if (dra7xx->mode == DW_PCIE_RC_TYPE)
		dw_pcie_host_deinit(&dra7xx->pci->pp);
	else
		dw_pcie_ep_deinit(&dra7xx->pci->ep);

err_gpio:
err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	dra7xx_pcie_disable_phy(dra7xx);

err_link:
	while (--i >= 0)
		device_link_del(link[i]);

	return ret;
}

/* [한국어]
 * dra7xx_pcie_suspend - 절전 전에 버스 마스터를 끈다
 *
 * @dev: 이 드라이버의 device.
 * @return: 항상 0.
 *
 * RC 모드에서만 동작한다. EP 모드면 곧바로 0 을 돌려준다.
 *
 * Command 레지스터의 BusMaster 비트를 내려, 절전 중에 이 컨트롤러가
 * DMA 트랜잭션을 내지 않게 한다. 아래 장치들이 이미 멈춘 상태에서
 * 컨트롤러만 살아 있으면 안 되기 때문이다.
 *
 * DWC 코어의 dbi 접근으로 읽고 쓰는데, 그 레지스터가 래퍼가 아니라
 * DWC IP 안의 config 공간에 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 절전).
 *
 * 호출 체인:  PM 코어 → [이 함수] → dw_pcie_readl_dbi() / dw_pcie_writel_dbi()
 */
static int dra7xx_pcie_suspend(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	struct dw_pcie *pci = dra7xx->pci;
	/* [한국어] 읽고-고쳐-쓸 Command 값. */
	u32 val;

	if (dra7xx->mode != DW_PCIE_RC_TYPE)
		/* [한국어] RC 모드가 아니면 할 일이 없다(윗줄이 그 판정). */
		return 0;

	/* clear MSE */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);
	val &= ~PCI_COMMAND_MEMORY;
	/* [한국어] BusMaster 를 내린 값을 되쓴다(윗줄에서 지웠다). 절전 중에 이 컨트롤러가
	 * DMA 를 내지 않게 하는 것이다. dbi 접근인 이유는 이 레지스터가 래퍼가
	 * 아니라 DWC IP 안의 config 공간에 있어서다. */
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);
/* [한국어] 아래에서 성공을 알린다. */

	return 0;
}

/* [한국어]
 * dra7xx_pcie_resume - 재개 후 버스 마스터를 되살린다
 *
 * @dev: 이 드라이버의 device.
 * @return: 항상 0.
 *
 * suspend 의 짝이다. RC 모드에서만 Command 레지스터의 BusMaster 를
 * 다시 세운다.
 *
 * 다른 상태는 복원하지 않는다. 아래 noirq 판이 PHY 와 래퍼 설정을
 * 맡고, 링크 자체는 DWC 코어가 다시 세우기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 재개).
 *
 * 호출 체인:  PM 코어 → [이 함수] → dw_pcie_readl_dbi() / dw_pcie_writel_dbi()
 */
static int dra7xx_pcie_resume(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	struct dw_pcie *pci = dra7xx->pci;
	/* [한국어] 읽고-고쳐-쓸 Command 값. */
	u32 val;

	if (dra7xx->mode != DW_PCIE_RC_TYPE)
		/* [한국어] RC 모드가 아니면 할 일이 없다. */
		return 0;

	/* set MSE */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);
	val |= PCI_COMMAND_MEMORY;
	/* [한국어] BusMaster 를 되살린 값을 쓴다(윗줄에서 세웠다). */
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);
/* [한국어] 아래에서 성공을 알린다. */

	return 0;
}

/* [한국어]
 * dra7xx_pcie_suspend_noirq - 인터럽트가 꺼진 단계에서 PHY 를 내린다
 *
 * @dev: 이 드라이버의 device.
 * @return: 항상 0.
 *
 * noirq 단계는 인터럽트 처리가 이미 꺼진 늦은 절전 시점이다. PHY 를
 * 여기서 내리는 이유는, 인터럽트가 살아 있는 동안 PHY 를 끄면 링크가
 * 끊기며 발생한 인터럽트를 처리하려다 문제가 되기 때문으로 보인다.
 *
 * 모드와 무관하게 항상 PHY 를 내린다 — 위 suspend/resume 이 RC 전용인
 * 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트가 비활성인 단계다.
 *
 * 호출 체인:  PM 코어(.suspend_noirq) → [이 함수] → dra7xx_pcie_disable_phy()
 */
static int dra7xx_pcie_suspend_noirq(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);

	dra7xx_pcie_disable_phy(dra7xx);

	return 0;
}

/* [한국어]
 * dra7xx_pcie_resume_noirq - 인터럽트를 켜기 전에 PHY 를 되살린다
 *
 * @dev: 이 드라이버의 device.
 * @return: 0 성공, dra7xx_pcie_enable_phy() 의 errno.
 *
 * suspend_noirq 의 짝이다. 인터럽트가 열리기 전에 PHY 를 켜 두어야
 * 링크가 올라오며 오는 인터럽트를 받을 준비가 된다.
 *
 * suspend 쪽과 달리 실패를 그대로 전한다. PHY 를 켜지 못하면 링크가
 * 서지 않아 재개가 의미 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 초기화에서 잠들 수 있다.
 *
 * 호출 체인:  PM 코어(.resume_noirq) → [이 함수] → dra7xx_pcie_enable_phy()
 */
static int dra7xx_pcie_resume_noirq(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	int ret;
/* [한국어] 인터럽트가 열리기 전에 PHY 를 켠다(윗줄). */

	ret = dra7xx_pcie_enable_phy(dra7xx);
	/* [한국어] 실패했으면 */
	if (ret) {
		/* [한국어] 남기고 */
		dev_err(dev, "failed to enable phy\n");
		/* [한국어] errno 를 전한다. suspend 쪽과 달리 실패를 그대로 전하는데,
		 * PHY 가 없으면 링크가 서지 않아 재개가 의미 없기 때문이다. */
		return ret;
	}

	return 0;
}

/* [한국어]
 * dra7xx_pcie_shutdown - 시스템 종료 시 링크와 PHY 를 내린다
 *
 * @pdev: 종료 중인 플랫폼 장치.   @return: 없음.
 *
 * 순서가 의미를 갖는다.
 *   1) dra7xx_pcie_stop_link() 로 LTSSM 을 꺼 링크를 내린다.
 *   2) 래퍼 인터럽트를 전부 마스크하고 상태를 지운다. 종료 중에
 *      처리되지 않는 인터럽트가 남으면 곤란하다.
 *   3) 런타임 PM 참조를 놓고 PM 을 끈다.
 *   4) PHY 를 내린다.
 *
 * remove 콜백이 없고 shutdown 만 있는 점이 artpec6 와 다르다.
 * artpec6 는 suppress_bind_attrs 로 언바인딩 자체를 막았지만,
 * 이쪽은 종료 경로만 갖춘 형태다.
 *
 * pm_runtime_put_sync 의 반환값을 확인하고 음수면 로그를 남기는데,
 * 그 시점에 할 수 있는 일이 없어 계속 진행한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 종료).
 *
 * 호출 체인:  커널 종료 경로 → [이 함수] → dra7xx_pcie_stop_link()
 *               → dra7xx_pcie_disable_phy()
 */
static void dra7xx_pcie_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	/* [한국어] 런타임 PM 참조 반납 결과. */
	int ret;
/* [한국어] 아래에서 링크를 내리고 인터럽트를 막는다. */

	dra7xx_pcie_stop_link(dra7xx->pci);

	ret = pm_runtime_put_sync(dev);
	/* [한국어] 참조 반납에 실패했으면(윗줄) */
	if (ret < 0)
		/* [한국어] 로그만 남긴다. 종료 중이라 할 수 있는 일이 없어 계속 진행한다. */
		dev_dbg(dev, "pm_runtime_put_sync failed\n");
/* [한국어] 아래에서 PHY 를 내린다. */

	pm_runtime_disable(dev);
	dra7xx_pcie_disable_phy(dra7xx);

	clk_disable_unprepare(dra7xx->clk);
}

static const struct dev_pm_ops dra7xx_pcie_pm_ops = {
	/* [한국어] 시스템 절전/재개. BusMaster 만 다룬다. */
	SYSTEM_SLEEP_PM_OPS(dra7xx_pcie_suspend, dra7xx_pcie_resume)
	/* [한국어] 인터럽트가 꺼진 단계의 절전/재개. PHY 를 다룬다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(dra7xx_pcie_suspend_noirq,
				  dra7xx_pcie_resume_noirq)
};

static struct platform_driver dra7xx_pcie_driver = {
	/* [한국어] 바인딩 시 호출. remove 콜백이 없고 shutdown 만 있는 것이
	 * artpec6(bind 자체를 차단)와 다른 점이다. */
	.probe = dra7xx_pcie_probe,
	/* [한국어] 드라이버 공통 부분. */
	.driver = {
		.name	= "dra7-pcie",
		/* [한국어] 위 compatible 표를 건다. */
		.of_match_table = of_dra7xx_pcie_match,
		.suppress_bind_attrs = true,
		.pm	= &dra7xx_pcie_pm_ops,
	},
	.shutdown = dra7xx_pcie_shutdown,
};
module_platform_driver(dra7xx_pcie_driver);

MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_DESCRIPTION("PCIe controller driver for TI DRA7xx SoCs");
MODULE_LICENSE("GPL v2");
