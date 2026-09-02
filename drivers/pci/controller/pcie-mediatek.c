// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek PCIe host controller driver.
 *
 * Copyright (c) 2017 MediaTek Inc.
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *	   Honghui Zhang <honghui.zhang@mediatek.com>
 */

/*
 * [한국어 설명] 미디어텍 SoC 의 PCIe 호스트 (pcie-mediatek.c)
 *
 * === 파일의 역할 ===
 * 미디어텍 SoC(MT7622, MT2712 등)의 PCIe 컨트롤러를 다룬다. DesignWare 가
 * 아니라 미디어텍 자체 IP 라서 dwc/ 가 아닌 이 자리에 있고, 링크 관리부터
 * config 접근, 인터럽트까지 전부 이 파일이 직접 구현한다. 그래서 파일이
 * 1300줄 가까이 된다.
 *
 * 이 컨트롤러의 구조에서 눈여겨볼 점은 포트마다 독립적이라는 것이다.
 * 하나의 컨트롤러가 여러 루트 포트를 갖는데, 각 포트가 자기 레지스터
 * 묶음과 자기 PHY, 자기 클럭, 자기 리셋을 갖는다. 그래서 struct mtk_pcie 가
 * struct mtk_pcie_port 의 목록을 들고 있고, 초기화가 포트 단위로 반복된다.
 *
 * 포트 하나가 실패해도 나머지는 살려야 하기 때문에, 초기화 실패 시 그
 * 포트만 목록에서 빼고 계속 진행한다. 하드웨어가 여럿인 드라이버에서
 * 흔히 보는 방어적 구조다.
 *
 * 세대가 둘 있다는 점도 알아야 한다. 구형(MT2701 등)과 신형(MT7622 등)이
 * 레지스터 배치와 링크 관리 방식이 달라서, soc_data 로 콜백을 갈아 끼운다.
 * 같은 파일 안에 두 하드웨어 세대가 공존하는 것이다.
 *
 * MSI 도 자체 구현이다. DesignWare 처럼 컨트롤러 안에 MSI 수신기가 있고,
 * 그것을 IRQ 도메인으로 감싸 하위 장치에 벡터를 나눠 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 mediatek,mt7622-pcie 등이 있으면
 *   -> [이 파일] mtk_pcie_probe()
 *      -> 포트마다: 클럭/PHY/리셋 준비, 레지스터 설정, 링크 대기
 *      -> MSI 도메인 구성
 *      -> pci_host_probe() -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트. config 접근은 잠금 아래.
 * 인터럽트 분배는 하드 IRQ.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: PHY·클럭·리셋 프레임워크, 커널 IRQ 도메인, PCI 코어.
 * 공유 상태: struct mtk_pcie 와 그 아래 struct mtk_pcie_port 목록.
 *
 * === 주요 함수/구조체 요약 ===
 * mtk_pcie_probe()        : 전체 초기화. 포트 목록을 만들고 각각 준비한다.
 * mtk_pcie_parse_port() / mtk_pcie_setup() : 디바이스 트리에서 포트 정보를
 *                           읽고 자원을 확보한다.
 * mtk_pcie_startup_port() / mtk_pcie_startup_port_v2() : 링크를 올린다.
 *                           구형과 신형의 절차가 달라 둘로 나뉘어 있다.
 * mtk_pcie_config_read() / mtk_pcie_config_write() : config 접근.
 * mtk_pcie_setup_irq() / mtk_pcie_intr_handler() : 인터럽트 설정과 분배.
 * mtk_pcie_allocate_msi_domains() 계열 : MSI 도메인 구성.
 * struct mtk_pcie         : 컨트롤러 전체.
 * struct mtk_pcie_port    : 루트 포트 하나. 자기 PHY·클럭·리셋을 갖는다.
 * struct mtk_pcie_soc     : 세대별 차이를 담은 콜백 표.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 호출하지 않는다(이 트리에서 전수 확인).
 * 이 드라이버가 세우는 것은 버스이고, NVMe 는 그 위에서 열거되는 장치 중
 * 하나일 뿐이다. 이 파일에는 NVMe 를 특별히 다루는 코드가 없다.
 *
 * 참고로 이 드라이버는 **링크 속도나 레인 수를 설정하지 않는다** -- DT 의
 * max-link-speed 나 num-lanes 를 읽는 코드가 없고, 그 값들은 하드웨어
 * 합성과 보드 배선이 정한 대로 쓰인다. 이 파일이 링크와 관련해 손대는
 * 것은 FTS 개수와 흐름 제어 크레딧(v1), 그리고 LTSSM/ASPM L1 스위치
 * (v2)뿐이다. 따라서 이 파일을 읽어 얻을 수 있는 성능 관련 정보는
 * 그 세 가지에 한정된다.
 */

/* [한국어] clk_prepare_enable 계열. 포트마다 최대 여섯 클록을 다룬다. */
#include <linux/clk.h>
/* [한국어] msleep. v2 기동의 PERST# 유지 시간에 쓴다. */
#include <linux/delay.h>
/* [한국어] readl_poll_timeout 과 그 atomic 판. 링크 대기와 설정 TLP 완료 대기에 쓴다. */
#include <linux/iopoll.h>
/* [한국어] dummy_irq_chip 을 제공한다. INTx 도메인이 마스크 콜백 없는 이 칩을 쓴다. */
#include <linux/irq.h>
/* [한국어] chained_irq_enter/exit. 포트 인터럽트 하나를 INTx 와 MSI 로 분해하는
 * 연쇄 핸들러를 감싼다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info. MSI 부모 도메인의 자식 파생 콜백으로 그대로 쓴다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain_create_linear 등 도메인 API. */
#include <linux/irqdomain.h>
/* [한국어] 커널 기본 매크로. 이 파일에서 직접 참조하는 심볼은 없다. */
#include <linux/kernel.h>
/* [한국어] syscon_node_to_regmap. 'mediatek,generic-pciecfg' 노드를 regmap 으로 잡는다. */
#include <linux/mfd/syscon.h>
/* [한국어] struct msi_msg 와 MSI 도메인 플래그들. */
#include <linux/msi.h>
/* [한국어] MODULE_* 매크로와 module_platform_driver. */
#include <linux/module.h>
/* [한국어] of_address 계열. 자원 파싱 경로에서 간접적으로 쓰인다. */
#include <linux/of_address.h>
/* [한국어] of_pci_get_devfn, of_get_pci_domain_nr. DT 구조 판별의 핵심이다. */
#include <linux/of_pci.h>
/* [한국어] of_platform 계열. 자식 노드 순회에 쓰인다. */
#include <linux/of_platform.h>
/* [한국어] PCI_SLOT/PCI_FUNC, PCI_NUM_INTX, PCI_VENDOR_ID_MEDIATEK 같은 규약 상수. */
#include <linux/pci.h>
/* [한국어] phy_init/phy_power_on 등 PHY 프레임워크 API. */
#include <linux/phy/phy.h>
/* [한국어] platform_driver 등록과 platform_get_irq 계열. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_enable/get_sync. 전원 도메인을 켠 뒤에야 클록을 켤 수 있다. */
#include <linux/pm_runtime.h>
/* [한국어] regmap_update_bits/regmap_write. 새 바인딩의 LTSSM 스위치와 AN7583 의
 * pbus 창 설정에 쓴다. */
#include <linux/regmap.h>
/* [한국어] reset_control_assert/deassert. 포트별 리셋 라인을 제어한다. */
#include <linux/reset.h>

/* [한국어] PCI 코어 내부 헤더. PCIE_T_PVPERL_MS 같은 규약 타이밍 상수를 가져온다. */
#include "../pci.h"

/* PCIe shared registers */
/* [한국어] v1 컨트롤러 창의 시스템 설정 레지스터. 포트별 PERST# 비트가 여기 있다. */
#define PCIE_SYS_CFG		0x00
/* [한국어] v1 의 인터럽트 활성 레지스터. 포트마다 비트 하나씩. */
#define PCIE_INT_ENABLE		0x0c
/* [한국어] v1 설정 접근의 주소 레지스터. 옛 PCI 의 CF8 에 해당한다. */
#define PCIE_CFG_ADDR		0x20
/* [한국어] v1 설정 접근의 데이터 레지스터. CFC 에 해당한다. */
#define PCIE_CFG_DATA		0x24

/* PCIe per port registers */
/* [한국어] 아래 셋은 v1 의 **포트별** 창에 있는 레지스터다. 위 넷은 컨트롤러 창이었다. */
#define PCIE_BAR0_SETUP		0x10
/* [한국어] 클래스 코드와 리비전을 쓰는 레지스터. */
#define PCIE_CLASS		0x34
/* [한국어] v1 의 링크 상태 레지스터. */
#define PCIE_LINK_STATUS	0x50

/* [한국어] 컨트롤러 창의 INT_ENABLE 에서 포트 x 의 비트. 20번부터 포트당 하나씩이다. */
#define PCIE_PORT_INT_EN(x)	BIT(20 + (x))
/* [한국어] 컨트롤러 창의 SYS_CFG 에서 포트 x 의 PERST# 비트. 1번부터 시작한다. */
#define PCIE_PORT_PERST(x)	BIT(1 + (x))
/* [한국어] 포트 창의 LINK_STATUS 에서 링크 업을 뜻하는 비트. */
#define PCIE_PORT_LINKUP	BIT(0)
/* [한국어] BAR0 이 덮을 최대 범위. 상위 16비트를 모두 세워 인바운드 접근이
 * 시스템 메모리에 닿게 한다. */
#define PCIE_BAR_MAP_MAX	GENMASK(31, 16)

/* [한국어] BAR0 활성 비트. */
#define PCIE_BAR_ENABLE		BIT(0)
/* [한국어] 리비전 ID 로 쓸 값. BIT(0) 이므로 1 이다. */
#define PCIE_REVISION_ID	BIT(0)
/* [한국어] 클래스 코드 0x060400(PCI-to-PCI 브리지)을 8비트 밀어 리비전 자리를 비운다.
 * 둘을 OR 해서 한 번에 쓴다. */
#define PCIE_CLASS_CODE		(0x60400 << 8)
/* [한국어] v1 설정 주소의 레지스터 번호 필드. 하위 6비트(7:2)와 확장 4비트(11:8)를
 * 따로 담는 것이 특징 -- 확장 부분은 24비트 자리로 밀려 간다. */
#define PCIE_CONF_REG(regn)	(((regn) & GENMASK(7, 2)) | \
				((((regn) >> 8) & GENMASK(3, 0)) << 24))
/* [한국어] 함수 번호 필드(비트 8~10). */
#define PCIE_CONF_FUN(fun)	(((fun) << 8) & GENMASK(10, 8))
/* [한국어] 장치 번호 필드(비트 11~15). */
#define PCIE_CONF_DEV(dev)	(((dev) << 11) & GENMASK(15, 11))
/* [한국어] 버스 번호 필드(비트 16~23). */
#define PCIE_CONF_BUS(bus)	(((bus) << 16) & GENMASK(23, 16))
/* [한국어] 네 필드를 합쳐 완전한 설정 주소를 만든다. CFG_ADDR 에 이 값을 쓰고
 * CFG_DATA 를 건드리면 그 대상에 접근된다. */
#define PCIE_CONF_ADDR(regn, fun, dev, bus) \
	(PCIE_CONF_REG(regn) | PCIE_CONF_FUN(fun) | \
	 PCIE_CONF_DEV(dev) | PCIE_CONF_BUS(bus))

/* MediaTek specific configuration registers */
/* [한국어] 확장 설정공간의 FTS 개수 레지스터. **자기 자신의** 설정공간에 있어
 * CFG_ADDR/CFG_DATA 를 거쳐야 닿는다. */
#define PCIE_FTS_NUM		0x70c
/* [한국어] 그 안의 FTS 필드(비트 15:8). */
#define PCIE_FTS_NUM_MASK	GENMASK(15, 8)
/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): C 에서 `<<` 가 `&` 보다 우선하므로
 * 이 매크로는 `(x) & (0xff << 8)` = `(x) & 0xff00` 으로 해석된다.
 * 유일한 호출부가 0x50 을 넘기므로 결과는 **0** 이고, 위 마스크로 필드를
 * 지운 뒤 0 을 OR 하는 셈이 되어 FTS 값이 설정되지 않는다.
 * 의도한 0x5000 을 얻으려면 `((x) & 0xff) << 8` 이어야 한다. */
#define PCIE_FTS_NUM_L0(x)	((x) & 0xff << 8)

/* [한국어] 확장 설정공간의 흐름 제어 크레딧 레지스터. */
#define PCIE_FC_CREDIT		0x73c
/* [한국어] 그 안에서 갱신할 필드들(비트 31, 그리고 28~16). */
#define PCIE_FC_CREDIT_MASK	(GENMASK(31, 31) | GENMASK(28, 16))
/* [한국어] 값을 16비트 밀어 그 자리에 넣는다. 위 마스크와 달리 비트 31 은 다루지 않아,
 * 호출부가 넘기는 0x806c 의 상위 비트가 28~16 범위를 넘으면 잘린다. */
#define PCIE_FC_CREDIT_VAL(x)	((x) << 16)

/* PCIe V2 share registers */
/* [한국어] v2 컨트롤러 창(또는 syscon)의 시스템 설정 레지스터. */
#define PCIE_SYS_CFG_V2		0x0
/* [한국어] 포트 x 의 LTSSM 활성 비트. **포트당 8비트 묶음**이라 x * 8 만큼 떨어진다. */
#define PCIE_CSR_LTSSM_EN(x)	BIT(0 + (x) * 8)
/* [한국어] 같은 묶음 안의 ASPM L1 활성 비트. */
#define PCIE_CSR_ASPM_L1_EN(x)	BIT(1 + (x) * 8)

/* PCIe V2 per-port registers */
/* [한국어] MSI 목적지로 쓰이는 레지스터. 디바이스가 이 주소에 벡터 번호를 쓰면
 * 컨트롤러가 가로챈다. */
#define PCIE_MSI_VECTOR		0x0c0

/* [한국어] v2 포트 창에 비친 자기 설정공간의 벤더 ID 오프셋. 0x100 부터가
 * 설정공간 영역이다. */
#define PCIE_CONF_VEND_ID	0x100
/* [한국어] 같은 영역의 디바이스 ID. */
#define PCIE_CONF_DEVICE_ID	0x102
/* [한국어] 같은 영역의 클래스 ID. */
#define PCIE_CONF_CLASS_ID	0x106

/* [한국어] v2 의 인터럽트 마스크 레지스터. INTx 넷과 MSI 요약 비트를 함께 다룬다. */
#define PCIE_INT_MASK		0x420
/* [한국어] 그 안의 INTx 마스크 비트 넷(16~19). */
#define INTX_MASK		GENMASK(19, 16)
/* [한국어] INTx 비트가 시작하는 자리. 상태 레지스터와 마스크 레지스터에서 같은 위치다. */
#define INTX_SHIFT		16
/* [한국어] v2 의 인터럽트 상태 레지스터. */
#define PCIE_INT_STATUS		0x424
/* [한국어] 그 안의 MSI 요약 비트. 개별 벡터는 아래 IMSI_STATUS 에 있다. */
#define MSI_STATUS		BIT(23)
/* [한국어] 개별 MSI 벡터 32개의 상태. write-1-to-clear 다. */
#define PCIE_IMSI_STATUS	0x42c
/* [한국어] MSI 목적지 주소를 하드웨어에 알리는 레지스터. */
#define PCIE_IMSI_ADDR		0x430
/* [한국어] INT_MASK 에서 MSI 를 막는 비트. 상태 쪽 MSI_STATUS 와 같은 23번이다. */
#define MSI_MASK		BIT(23)
/* [한국어] 포트당 MSI 벡터 수. 비트맵 크기와 도메인 크기를 모두 이 값으로 잡는다. */
#define MTK_MSI_IRQS_NUM	32

/* [한국어] AHB→PCIe 주소 창의 기준 주소 하위 32비트. CPU 가 이 범위를 건드리면
 * PCIe 트랜잭션으로 나간다. */
#define PCIE_AHB_TRANS_BASE0_L	0x438
/* [한국어] 같은 창의 상위 32비트. */
#define PCIE_AHB_TRANS_BASE0_H	0x43c
/* [한국어] 그 창의 크기를 2의 지수로 담는 필드(하위 5비트). */
#define AHB2PCIE_SIZE(x)	((x) & GENMASK(4, 0))
/* [한국어] PCIe→AHB 방향 창의 제어 레지스터. */
#define PCIE_AXI_WINDOW0	0x448
/* [한국어] 그 창을 켜는 비트. */
#define WIN_ENABLE		BIT(7)
/*
 * Define PCIe to AHB window size as 2^33 to support max 8GB address space
 * translate, support least 4GB DRAM size access from EP DMA(physical DRAM
 * start from 0x40000000).
 */
/* [한국어] PCIe→AHB 창의 고정 크기 값. 아래 WIN_ENABLE 과 OR 해서 한 번에 쓴다.
 * 상수인 것은 이 방향 창이 항상 같은 크기로 열리기 때문이다. */
#define PCIE2AHB_SIZE	0x21

/* PCIe V2 configuration transaction header */
/* [한국어] 아래 여섯 레지스터가 v2 의 설정 TLP 조립 창이다. v1 의 주소/데이터
 * 방식과 달리 헤더를 손으로 만들어야 한다. */
#define PCIE_CFG_HEADER0	0x460
/* [한국어] TLP 헤더 두 번째 워드(바이트 인에이블). */
#define PCIE_CFG_HEADER1	0x464
/* [한국어] TLP 헤더 세 번째 워드(목적지 BDF). */
#define PCIE_CFG_HEADER2	0x468
/* [한국어] 쓰기 데이터를 올리는 레지스터. */
#define PCIE_CFG_WDATA		0x470
/* [한국어] 요청 시작과 완료 상태를 함께 담는 레지스터. */
#define PCIE_APP_TLP_REQ	0x488
/* [한국어] 읽기 결과를 꺼내는 레지스터. */
#define PCIE_CFG_RDATA		0x48c
/* [한국어] 요청을 시작하는 비트. 소프트웨어가 세우고 하드웨어가 완료 시 내린다. */
#define APP_CFG_REQ		BIT(0)
/* [한국어] 완료 상태 필드(비트 5~7). 0 이 아니면 오류 완료다. */
#define APP_CPL_STATUS		GENMASK(7, 5)

/* [한국어] 설정 트랜잭션의 TLP 타입 값(Type 0). */
#define CFG_WRRD_TYPE_0		4
/* [한국어] 설정 쓰기의 포맷 값. */
#define CFG_WR_FMT		2
/* [한국어] 설정 읽기의 포맷 값. 0 이라 OR 해도 아무 비트를 세우지 않는다. */
#define CFG_RD_FMT		0

/* [한국어] 헤더 DW0 의 길이 필드(하위 10비트). 설정 접근은 항상 1 워드다. */
#define CFG_DW0_LENGTH(length)	((length) & GENMASK(9, 0))
/* [한국어] DW0 의 타입 필드(비트 24~28). */
#define CFG_DW0_TYPE(type)	(((type) << 24) & GENMASK(28, 24))
/* [한국어] DW0 의 포맷 필드(비트 29~31). */
#define CFG_DW0_FMT(fmt)	(((fmt) << 29) & GENMASK(31, 29))
/* [한국어] 헤더 DW2 의 레지스터 번호 필드(비트 2~11). */
#define CFG_DW2_REGN(regn)	((regn) & GENMASK(11, 2))
/* [한국어] DW2 의 함수 번호 필드. */
#define CFG_DW2_FUN(fun)	(((fun) << 16) & GENMASK(18, 16))
/* [한국어] DW2 의 장치 번호 필드. */
#define CFG_DW2_DEV(dev)	(((dev) << 19) & GENMASK(23, 19))
/* [한국어] DW2 의 버스 번호 필드. */
#define CFG_DW2_BUS(bus)	(((bus) << 24) & GENMASK(31, 24))
/* [한국어] DW0 을 조립한다. 길이는 항상 1 로 고정이다. */
#define CFG_HEADER_DW0(type, fmt) \
	(CFG_DW0_LENGTH(1) | CFG_DW0_TYPE(type) | CFG_DW0_FMT(fmt))
/* [한국어] DW1 은 **바이트 인에이블**이다. GENMASK(size-1, 0) 을 (where & 3) 만큼
 * 밀어, 4바이트 워드 안에서 어느 바이트를 읽고 쓸지 표시한다. */
#define CFG_HEADER_DW1(where, size) \
	(GENMASK(((size) - 1), 0) << ((where) & 0x3))
/* [한국어] DW2 를 조립한다. 목적지 BDF 와 레지스터 번호가 여기 들어간다. */
#define CFG_HEADER_DW2(regn, fun, dev, bus) \
	(CFG_DW2_REGN(regn) | CFG_DW2_FUN(fun) | \
	CFG_DW2_DEV(dev) | CFG_DW2_BUS(bus))

/* [한국어] v2 포트 창의 리셋 제어 레지스터. */
#define PCIE_RST_CTRL		0x510
/* [한국어] PHY 리셋 해제 비트. */
#define PCIE_PHY_RSTB		BIT(0)
/* [한국어] PIPE 인터페이스 리셋 해제 비트. */
#define PCIE_PIPE_SRSTB		BIT(1)
/* [한국어] MAC 리셋 해제 비트. */
#define PCIE_MAC_SRSTB		BIT(2)
/* [한국어] 코어 리셋 해제 비트. */
#define PCIE_CRSTB		BIT(3)
/* [한국어] PERST# 해제 비트. 위 넷과 함께 한 번에 세운다. */
#define PCIE_PERSTB		BIT(8)
/* [한국어] 링크가 끊겼을 때 자동 리셋을 거는 비트들. 리셋 절차 중 이것만 세워 두는
 * 단계가 있다. */
#define PCIE_LINKDOWN_RST_EN	GENMASK(15, 13)
/* [한국어] v2 의 링크 상태 레지스터. v1(0x50)과 오프셋이 완전히 다르다. */
#define PCIE_LINK_STATUS_V2	0x804
/* [한국어] 그 안의 링크 업 비트. */
#define PCIE_PORT_LINKUP_V2	BIT(10)

struct mtk_pcie_port;
/* [한국어] 이 아래는 SoC 판본 차이를 흡수하기 위한 자료구조들이다. */

/**
 * enum mtk_pcie_quirks - MTK PCIe quirks
 * @MTK_PCIE_FIX_CLASS_ID: host's class ID needed to be fixed
 * @MTK_PCIE_FIX_DEVICE_ID: host's device ID needed to be fixed
 * @MTK_PCIE_NO_MSI: Bridge has no MSI support, and relies on an external block
 * @MTK_PCIE_SKIP_RSTB: Skip calling RSTB bits on PCIe probe
 */
enum mtk_pcie_quirks {
	MTK_PCIE_FIX_CLASS_ID = BIT(0),
	/* [한국어] 디바이스 ID 가 잘못 나오는 판본을 위한 표시. FIX_CLASS_ID 와 달리 값이
	 * SoC 표의 device_id 필드에서 온다. */
	MTK_PCIE_FIX_DEVICE_ID = BIT(1),
	MTK_PCIE_NO_MSI = BIT(2),
	MTK_PCIE_SKIP_RSTB = BIT(3),
};

/**
 * struct mtk_pcie_soc - differentiate between host generations
 * @device_id: device ID which this host need to be fixed
 * @ops: pointer to configuration access functions
 * @startup: pointer to controller setting functions
 * @setup_irq: pointer to initialize IRQ functions
 * @quirks: PCIe device quirks.
 */
struct mtk_pcie_soc {
	/* [한국어] 이 SoC 의 루트 포트가 광고하는 device ID. 위 영어 주석대로
	 * '고쳐 주어야 하는' 값이다.
	 * 설정자: 파일 끝의 정적 mtk_pcie_soc_* 인스턴스들.
	 * 읽는 자: mtk_pcie_fixup_class 계열 quirk 가 이 ID 와 대조해 클래스 코드를
	 *   브리지로 바로잡는다. 일부 MediaTek 루트 포트가 자신을 브리지가 아닌
	 *   다른 클래스로 신고해 열거가 어긋나기 때문이다.
	 * 값 범위: 16비트 PCI device ID.
	 * 동기화: const 정적 데이터. */
	unsigned int device_id;
	/* [한국어] 이 SoC 가 쓸 설정공간 접근 방식.
	 * 설정자: 파일 끝의 정적 mtk_pcie_soc_* 인스턴스들.
	 * 읽는 자: mtk_pcie_probe 가 host->ops 에 건다.
	 * 값 범위: mtk_pcie_ops(v1, map_bus 방식) 또는 mtk_pcie_ops_v2(read/write
	 * 직접 구현). 두 방식은 하드웨어 자체가 달라 서로 바꿔 쓸 수 없다.
	 * 동기화: const 정적 데이터. */
	struct pci_ops *ops;
	/* [한국어] 포트를 기동하는 SoC 별 콜백.
	 * 설정자: 정적 SoC 표.
	 * 읽는 자: mtk_pcie_enable_port 가 마지막에 부른다.
	 * 값 범위: startup_port(v1) / startup_port_v2 / startup_port_an7583.
	 * **NULL 검사 없이 호출되므로 모든 표가 반드시 채워야 한다.**
	 * 동기화: const 정적 데이터. */
	int (*startup)(struct mtk_pcie_port *port);
	/* [한국어] IRQ 도메인과 핸들러를 세우는 SoC 별 콜백.
	 * 설정자: 정적 SoC 표.
	 * 읽는 자: mtk_pcie_parse_port 가 **NULL 검사 후** 부른다.
	 * 값 범위: mtk_pcie_setup_irq 또는 NULL. v1 은 NULL 이라 INTx/MSI 도메인을
	 * 아예 만들지 않는다.
	 * 동기화: const 정적 데이터. */
	int (*setup_irq)(struct mtk_pcie_port *port, struct device_node *node);
	/* [한국어] 이 SoC 가 필요로 하는 우회들의 비트 조합.
	 * 설정자: 정적 SoC 표.
	 * 읽는 자: startup_port_v2 의 세 분기와 mtk_pcie_probe 의 msi_domain 설정.
	 * 값 범위: enum mtk_pcie_quirks 의 OR 조합. 0 이면 우회가 필요 없다.
	 * 동기화: const 정적 데이터. */
	enum mtk_pcie_quirks quirks;
/* [한국어] 이 구조체가 SoC 판본 차이를 한곳에 모으는 덕에, 나머지 코드는 판본을
 * 거의 의식하지 않는다. */
};

/**
 * struct mtk_pcie_port - PCIe port information
 * @base: IO mapped register base
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @reset: pointer to port reset control
 * @sys_ck: pointer to transaction/data link layer clock
 * @ahb_ck: pointer to AHB slave interface operating clock for CSR access
 *          and RC initiated MMIO access
 * @axi_ck: pointer to application layer MMIO channel operating clock
 * @aux_ck: pointer to pe2_mac_bridge and pe2_mac_core operating clock
 *          when pcie_mac_ck/pcie_pipe_ck is turned off
 * @obff_ck: pointer to OBFF functional block operating clock
 * @pipe_ck: pointer to LTSSM and PHY/MAC layer operating clock
 * @phy: pointer to PHY control block
 * @slot: port slot
 * @irq: GIC irq
 * @irq_domain: legacy INTx IRQ domain
 * @inner_domain: inner IRQ domain
 * @lock: protect the msi_irq_in_use bitmap
 * @msi_irq_in_use: bit map for assigned MSI IRQ
 */
struct mtk_pcie_port {
	/* [한국어] 이 포트 전용 레지스터 창의 가상 주소.
	 * 설정자: mtk_pcie_parse_port() 가 DT 의 포트 노드에서 매핑한다.
	 * 읽는 자: 이 포트의 링크 기동, LTSSM 제어, INTx/MSI 레지스터 접근 전부.
	 * 값 범위: 유효한 iomem 포인터. 컨트롤러 전체가 공유하는 mtk_pcie.base 와는
	 *   별개로, 포트마다 독립된 창이다.
	 * 동기화: 창 자체는 프로브 이후 불변이고, 레지스터 접근의 경합은 포트별
	 *   인터럽트 처리 경로에서 조정된다. */
	void __iomem *base;
	/* [한국어] ports 목록에 매달리기 위한 연결 고리.
	 * 설정자: mtk_pcie_parse_port 가 list_add_tail 로 붙인다.
	 * 읽는 자: 모든 순회 함수. mtk_pcie_port_free 가 list_del 로 뗀다.
	 * 값 범위: 항상 유효한 노드. 뗀 뒤에는 곧바로 메모리가 해제된다.
	 * 동기화: 없음. 목록 조작이 프로브·제거 경로에서만 일어난다는 전제다. */
	struct list_head list;
	/* [한국어] 부모 컨트롤러로 돌아가는 포인터.
	 * 설정자: mtk_pcie_parse_port.
	 * 읽는 자: 거의 모든 포트 함수가 dev 나 soc 에 닿기 위해 쓴다.
	 * 값 범위: NULL 이 아니다.
	 * 동기화: 프로브에서 한 번 쓰고 이후 읽기만 한다. */
	struct mtk_pcie *pcie;
	/* [한국어] 이 포트의 리셋 라인.
	 * 설정자: parse_port 가 optional exclusive 로 얻는다.
	 * 읽는 자: mtk_pcie_enable_port 의 assert/deassert.
	 * 값 범위: NULL 이거나 유효한 핸들. 다만 parse_port 가 -EPROBE_DEFER 만
	 * 걸러 내므로 **다른 오류 포인터가 그대로 남을 수 있다**.
	 * 동기화: 프로브·재개 경로 전용. */
	struct reset_control *reset;
	/* [한국어] 필수 시스템 클록.
	 * 설정자: parse_port 가 devm_clk_get 으로 얻는다(실패하면 프로브 중단).
	 * 읽는 자: enable_port 가 가장 먼저 켜고, put_resources 가 마지막에 끈다.
	 * 값 범위: 유효한 clk 핸들.
	 * 동기화: 프로브·서스펜드·재개 경로 전용. */
	struct clk *sys_ck;
	/* [한국어] AHB 버스 클록(선택).
	 * 설정자: parse_port 가 devm_clk_get_optional 로 얻는다.
	 * 읽는 자: enable_port/put_resources/suspend/resume.
	 * 값 범위: **NULL 가능** -- 이 클록이 없는 SoC 판본이 있다.
	 * clk_prepare_enable(NULL) 은 아무 일도 하지 않으므로 그대로 동작한다.
	 * 동기화: 위와 같다. */
	struct clk *ahb_ck;
	/* [한국어] AXI 버스 클록(선택). 위와 같은 규칙. */
	struct clk *axi_ck;
	/* [한국어] 보조 클록(선택). 위와 같은 규칙. */
	struct clk *aux_ck;
	/* [한국어] OBFF(Optimized Buffer Flush/Fill) 클록(선택). 위와 같은 규칙. */
	struct clk *obff_ck;
	/* [한국어] PIPE 인터페이스 클록(선택). PHY 와 MAC 사이의 인터페이스용이다.
	 * **가장 나중에 켜고 가장 먼저 끄는** 클록이라, 다른 클록에 의존한다. */
	struct clk *pipe_ck;
	/* [한국어] 이 포트의 PHY(선택).
	 * 설정자: parse_port 가 devm_phy_optional_get 으로 얻는다.
	 * 읽는 자: enable_port 의 init/power_on, put_resources 의 반대 동작.
	 * 값 범위: NULL 가능. PHY 프레임워크 함수들이 NULL 을 무시한다.
	 * 동기화: 프로브·서스펜드·재개 경로 전용. */
	struct phy *phy;
	/* [한국어] 이 포트의 슬롯 번호.
	 * 설정자: parse_port. DT 의 devfn 또는 도메인 번호에서 온다.
	 * 읽는 자: 자원 이름 조립, LTSSM 비트 위치 계산, find_port 의 대조.
	 * 값 범위: 0 이상. 레지스터 비트 위치에 쓰이므로 하드웨어의 포트 수를
	 * 넘으면 엉뚱한 비트를 건드리게 되지만, 그 검사는 없다.
	 * 동기화: 프로브에서 한 번 쓰고 이후 읽기만 한다. */
	u32 slot;
	/* [한국어] 이 포트의 부모 인터럽트 번호.
	 * 설정자: mtk_pcie_setup_irq.
	 * 읽는 자: 연쇄 핸들러 등록과 teardown.
	 * 값 범위: 양수. 음수는 setup_irq 가 걸러 낸다.
	 * 동기화: 프로브·제거 경로 전용. */
	int irq;
	/* [한국어] INTx 도메인.
	 * 설정자: mtk_pcie_init_irq_domain.
	 * 읽는 자: mtk_pcie_intr_handler 가 generic_handle_domain_irq 에 넘긴다.
	 * 값 범위: 유효한 도메인. 생성 실패 시 init_irq_domain 이 오류를 올리므로
	 * **NULL 로 남은 채 핸들러가 걸리는 일은 없다**(rockchip 과 대조된다).
	 * 동기화: 생성은 프로세스 문맥, 사용은 인터럽트 문맥. */
	struct irq_domain *irq_domain;
	/* [한국어] MSI 부모 도메인.
	 * 설정자: mtk_pcie_allocate_msi_domains.
	 * 읽는 자: intr_handler 의 MSI 분해 경로.
	 * 값 범위: CONFIG_PCI_MSI 가 꺼지면 만들어지지 않는다.
	 * 동기화: 위와 같다. */
	struct irq_domain *inner_domain;
	/* [한국어] msi_irq_in_use 비트맵을 지키는 잠금.
	 * 설정자: mtk_pcie_allocate_msi_domains 의 mutex_init.
	 * 읽는 자: irq_domain_alloc/free.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: **뮤텍스**인 것이 요점 -- 벡터 할당/해제는 프로세스 문맥에서만
	 * 일어나므로 스핀락이 필요 없다. */
	struct mutex lock;
	/* [한국어] 32개 MSI 벡터의 사용 여부 비트맵.
	 * 설정자: irq_domain_alloc 이 __set_bit, free 가 __clear_bit.
	 * 읽는 자: alloc 의 find_first_zero_bit, free 의 test_bit.
	 * 값 범위: MTK_MSI_IRQS_NUM(32)비트.
	 * 동기화: 위 lock 뮤텍스가 지킨다. __set_bit/__clear_bit 의 비원자 판을
	 * 쓰는 것도 잠금이 있기 때문이다. */
	DECLARE_BITMAP(msi_irq_in_use, MTK_MSI_IRQS_NUM);
/* [한국어] 포트마다 이 구조체가 하나씩 있고, 컨트롤러는 이들을 목록으로 엮는다. */
};

/**
 * struct mtk_pcie - PCIe host information
 * @dev: pointer to PCIe device
 * @base: IO mapped register base
 * @cfg: IO mapped register map for PCIe config
 * @free_ck: free-run reference clock
 * @ports: pointer to PCIe port information
 * @soc: pointer to SoC-dependent operations
 */
struct mtk_pcie {
	/* [한국어] 이 컨트롤러의 platform device. 로그와 devm_* 할당의 기준점이 된다.
	 * 설정자: mtk_pcie_probe() 가 &pdev->dev 를 저장한다.
	 * 읽는 자: 이 파일 전반의 dev_err/dev_info 와 모든 devm_ 계열 호출.
	 * 값 범위: 항상 유효한 포인터.
	 * 동기화: probe 이후 불변. */
	struct device *dev;
	/* [한국어] 'subsys' MMIO 창의 가상 주소.
	 * 설정자: mtk_pcie_subsys_powerup 이 있으면 매핑한다.
	 * 읽는 자: v1 은 모든 설정 접근에, v2 는 LTSSM 스위치에만 쓴다.
	 * 값 범위: **NULL 가능** -- 새 바인딩은 이 창 대신 아래 cfg regmap 을 쓴다.
	 * startup_port_v2 가 그 경우를 if/else 로 갈라 처리한다.
	 * 동기화: 없음. */
	void __iomem *base;
	/* [한국어] 'mediatek,generic-pciecfg' syscon 의 regmap.
	 * 설정자: subsys_powerup 이 트리 전체에서 그 노드를 찾아 잡는다.
	 * 읽는 자: startup_port_v2 의 LTSSM 스위치(base 가 없을 때).
	 * 값 범위: NULL 가능. base 와 cfg 둘 다 없으면 LTSSM 을 켜지 않는다.
	 * 동기화: regmap 자체가 내부 잠금을 갖는다. */
	struct regmap *cfg;
	/* [한국어] 모든 포트가 공유하는 기준 클록.
	 * 설정자: subsys_powerup. **없으면 NULL 로 만들고 계속 진행한다.**
	 * 읽는 자: subsys_powerup/powerdown, suspend/resume.
	 * 값 범위: NULL 가능.
	 * 동기화: 프로브·서스펜드·재개 경로 전용. */
	struct clk *free_ck;
/* [한국어] 아래 ports 목록이 이 드라이버의 중심 자료구조다. */

	struct list_head ports;
	/* [한국어] compatible 에 매인 SoC 별 설정표.
	 * 설정자: mtk_pcie_probe 의 of_device_get_match_data.
	 * 읽는 자: parse_port(setup_irq), enable_port(startup), probe(ops, quirks),
	 * startup_port_v2(quirks, device_id).
	 * 값 범위: NULL 검사가 없다 -- of_match_table 로 매칭됐다면 반드시 있다는 전제다.
	 * 동기화: const 정적 데이터. */
	const struct mtk_pcie_soc *soc;
};

/* [한국어]
 * mtk_pcie_subsys_powerdown - 서브시스템 공용 클록과 런타임 PM 을 되돌린다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 없음.
 *
 * 포트별 자원이 아니라 **컨트롤러 전체가 공유하는 것**만 정리한다:
 * free_ck(모든 포트가 쓰는 기준 클록)와 런타임 PM 참조다.
 *
 * pm_runtime_put_sync 와 disable 을 짝으로 부르는 것은 powerup 이
 * enable 과 get_sync 를 짝으로 불렀기 때문이다. 순서도 정확히 역순이다.
 *
 * 실행 컨텍스트: 프로브 실패 되감기와 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_setup(포트 없음) / mtk_pcie_put_resources → [이 함수]
 *     → clk_disable_unprepare → pm_runtime_put_sync → pm_runtime_disable
 */
static void mtk_pcie_subsys_powerdown(struct mtk_pcie *pcie)
{
	struct device *dev = pcie->dev;

	clk_disable_unprepare(pcie->free_ck);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

/* [한국어]
 * mtk_pcie_port_free - 포트 하나를 목록에서 빼고 매핑·메모리를 해제한다
 *
 * @port: 해제할 포트.
 * @return: 없음.
 *
 * devm 으로 잡은 것을 **명시적으로** 되돌리는 점이 특이하다. devm 은 보통
 * 드라이버가 내려갈 때 자동 해제되는데, 여기서는 링크가 서지 않은 포트를
 * 프로브 도중에 버려야 하므로 수동으로 푼다 -- 그러지 않으면 쓰지 않는
 * 포트의 매핑이 드라이버 수명 내내 남는다.
 *
 * list_del 을 iounmap 뒤, kfree 앞에 두는 순서가 중요하다. 목록에서 먼저
 * 빼야 이후 순회가 해제된 메모리를 건드리지 않는다. 호출자들이
 * list_for_each_entry_safe 를 쓰는 것도 같은 이유다.
 *
 * 실행 컨텍스트: 프로브 및 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_enable_port(실패 경로) / mtk_pcie_put_resources → [이 함수]
 *     → devm_iounmap → list_del → devm_kfree
 */
static void mtk_pcie_port_free(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
/* [한국어] 아래 두 줄이 devm 자원을 수동으로 되돌린다. */

	devm_iounmap(dev, port->base);
	/* [한국어] **목록에서 먼저 뺀다.** 아래 kfree 뒤에 하면 해제된 메모리의 연결 고리를
	 * 건드리게 된다. */
	list_del(&port->list);
	devm_kfree(dev, port);
}

/* [한국어]
 * mtk_pcie_put_resources - 모든 포트의 PHY·클록을 끄고 서브시스템까지 되돌린다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 없음.
 *
 * 포트마다 mtk_pcie_enable_port 가 켠 것을 정확히 역순으로 되돌린다:
 * PHY 전원 → PHY 초기화 → pipe/obff/axi/aux/ahb/sys 클록 순이다. 클록
 * 해제 순서가 켠 순서의 역순인 것은, 뒤에 켠 클록이 앞의 클록에 의존할
 * 수 있기 때문이다.
 *
 * list_for_each_entry_safe 를 쓰는 이유: 루프 안의 mtk_pcie_port_free 가
 * 목록에서 항목을 빼고 메모리를 해제하므로, 다음 항목 포인터를 미리
 * 확보해 두지 않으면 해제된 메모리를 따라가게 된다.
 *
 * 마지막에 서브시스템 공용 자원까지 되돌려, 이 함수 하나로 정리가 끝난다.
 *
 * 실행 컨텍스트: 프로브 실패 되감기와 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_probe(실패) / mtk_pcie_remove → [이 함수]
 *     → phy_power_off/phy_exit → clk_disable_unprepare(6개)
 *       → mtk_pcie_port_free → mtk_pcie_subsys_powerdown
 */
static void mtk_pcie_put_resources(struct mtk_pcie *pcie)
{
	struct mtk_pcie_port *port, *tmp;

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] PHY 전원을 먼저 내리고 초기화를 되돌리는 순서. enable_port 의 정확한 역순이다. */
		phy_power_off(port->phy);
		phy_exit(port->phy);
		clk_disable_unprepare(port->pipe_ck);
		clk_disable_unprepare(port->obff_ck);
		clk_disable_unprepare(port->axi_ck);
		clk_disable_unprepare(port->aux_ck);
		clk_disable_unprepare(port->ahb_ck);
		clk_disable_unprepare(port->sys_ck);
		mtk_pcie_port_free(port);
	}

	mtk_pcie_subsys_powerdown(pcie);
}

/* [한국어]
 * mtk_pcie_check_cfg_cpld - 설정 TLP 의 완료(Completion)를 기다리고 상태를 확인한다
 *
 * @port: 접근 중인 포트.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * v2 하드웨어는 설정공간을 주소 매핑으로 노출하지 않는다. 대신 소프트웨어가
 * TLP 헤더를 직접 조립해 보내고 완료를 기다려야 한다 -- 이 함수가 그
 * 기다리는 쪽이다.
 *
 * 두 단계를 확인한다:
 *  1. APP_CFG_REQ 비트가 **내려갈 때까지** 기다린다. 이 비트는 요청을 낼 때
 *     세우고 하드웨어가 완료를 받으면 스스로 내린다. 100ms 안에 안 내려가면
 *     응답이 오지 않은 것이다.
 *  2. APP_CPL_STATUS 필드가 0 이 아니면 완료가 왔지만 **오류 상태**다
 *     (Unsupported Request, Completer Abort 등).
 * 두 경우 모두 PCIBIOS_SET_FAILED 로 묶어 돌려준다 -- 호출자 입장에서는
 * 어느 쪽이든 값을 믿을 수 없다는 점이 같다.
 *
 * readl_poll_timeout_**atomic** 을 쓰는 점이 중요하다. 설정 접근은 PCI 코어의
 * 스핀락 아래에서 일어날 수 있어 잠들면 안 되므로, 바쁜 대기 판이어야 한다.
 *
 * 실행 컨텍스트: 설정 접근 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   mtk_pcie_hw_rd_cfg / mtk_pcie_hw_wr_cfg → [이 함수]
 *     → readl_poll_timeout_atomic
 */
static int mtk_pcie_check_cfg_cpld(struct mtk_pcie_port *port)
{
	u32 val;
	int err;
/* [한국어] 아래 대기가 이 함수의 본체다. */

	err = readl_poll_timeout_atomic(port->base + PCIE_APP_TLP_REQ, val,
					/* [한국어] APP_CFG_REQ 가 **내려갈 때까지** 기다린다. 요청을 낼 때 세운 비트를
					 * 하드웨어가 완료를 받으면 스스로 내린다. 10us 간격으로 최대 100ms. */
					!(val & APP_CFG_REQ), 10,
					100 * USEC_PER_MSEC);
	if (err)
		/* [한국어] 응답이 오지 않았다. 목적지가 없거나 링크가 끊긴 경우다. */
		return PCIBIOS_SET_FAILED;

	if (readl(port->base + PCIE_APP_TLP_REQ) & APP_CPL_STATUS)
		/* [한국어] 완료는 왔지만 오류 상태다(Unsupported Request, Completer Abort 등).
		 * 호출자 입장에서는 값을 믿을 수 없다는 점이 위와 같아 같은 코드로 묶는다. */
		return PCIBIOS_SET_FAILED;

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * mtk_pcie_hw_rd_cfg - 설정 읽기 TLP 를 손으로 조립해 보내고 결과를 꺼낸다
 *
 * @port: 접근할 포트.
 * @bus: 목적지 버스 번호.
 * @devfn: 목적지 장치/함수 번호.
 * @where: 설정공간 오프셋.
 * @size: 1, 2, 4 바이트.
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * TLP 헤더 세 워드를 직접 만들어 레지스터에 쓴다:
 *  - DW0: 길이 1, 타입 CFG_WRRD_TYPE_0, 포맷 CFG_RD_FMT(읽기).
 *  - DW1: **바이트 인에이블**. CFG_HEADER_DW1(where, size) 가
 *    GENMASK(size-1, 0) 를 (where & 3) 만큼 밀어, 4바이트 워드 안에서 어느
 *    바이트를 읽을지 표시한다.
 *  - DW2: 목적지 BDF 와 레지스터 번호.
 * 그 다음 APP_CFG_REQ 를 세워 전송을 시작하고 완료를 기다린다.
 *
 * 읽기 결과는 항상 워드 단위로 돌아오므로, size 가 1 이나 2 이면
 * (where & 3) 만큼 오른쪽으로 밀어 원하는 바이트를 꺼내고 마스크한다.
 * size 가 4 이면 그대로 쓴다 -- 그래서 그 경우의 분기가 없다.
 *
 * 실행 컨텍스트: 설정 접근 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   mtk_pcie_config_read → [이 함수] → mtk_pcie_check_cfg_cpld
 */
static int mtk_pcie_hw_rd_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn,
			      int where, int size, u32 *val)
{
	u32 tmp;

	/* Write PCIe configuration transaction header for Cfgrd */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_RD_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	/* [한국어] DW2 에 목적지 BDF 와 레지스터 번호를 담는다. PCI_FUNC/PCI_SLOT 이
	 * devfn 을 함수와 장치로 쪼갠다. */
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus),
	       /* [한국어] 헤더 세 워드가 모두 준비됐다. */
	       port->base + PCIE_CFG_HEADER2);

	/* Trigger h/w to transmit Cfgrd TLP */
	tmp = readl(port->base + PCIE_APP_TLP_REQ);
	tmp |= APP_CFG_REQ;
	/* [한국어] APP_CFG_REQ 를 세워 전송을 시작한다. 읽고-고쳐-쓰기라 다른 비트는 보존된다. */
	writel(tmp, port->base + PCIE_APP_TLP_REQ);
/* [한국어] 이제 완료를 기다린다. */

	/* Check completion status */
	if (mtk_pcie_check_cfg_cpld(port))
		return PCIBIOS_SET_FAILED;

	/* Read cpld payload of Cfgrd */
	*val = readl(port->base + PCIE_CFG_RDATA);

	if (size == 1)
		*val = (*val >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (8 * (where & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * mtk_pcie_hw_wr_cfg - 설정 쓰기 TLP 를 손으로 조립해 보낸다
 *
 * @port: 접근할 포트.
 * @bus: 목적지 버스 번호.
 * @devfn: 목적지 장치/함수 번호.
 * @where: 설정공간 오프셋.
 * @size: 1, 2, 4 바이트.
 * @val: 쓸 값.
 * @return: mtk_pcie_check_cfg_cpld 의 결과.
 *
 * 읽기 쪽과 헤더 조립이 같고 포맷만 CFG_WR_FMT 로 바뀐다. 다른 점은
 * 데이터를 함께 보내야 한다는 것이다.
 *
 * `val = val << 8 * (where & 3)` 이 그 준비다. 설정 데이터 레지스터는
 * 4바이트 워드 단위이므로, 워드 안의 어느 자리에 쓸지에 맞춰 값을 밀어
 * 올린다. 어느 바이트가 실제로 반영될지는 DW1 의 바이트 인에이블이 정하므로,
 * 나머지 자리에 무엇이 들어가든 무시된다.
 *
 * 읽기와 달리 완료 결과를 그대로 반환한다 -- 꺼낼 값이 없기 때문이다.
 *
 * 실행 컨텍스트: 설정 접근 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   mtk_pcie_config_write → [이 함수] → mtk_pcie_check_cfg_cpld
 */
static int mtk_pcie_hw_wr_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn,
			      int where, int size, u32 val)
{
	/* Write PCIe configuration transaction header for Cfgwr */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_WR_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	/* [한국어] 쓰기 쪽도 DW2 구성은 같다. */
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus),
	       /* [한국어] 헤더 준비 완료. 아래에서 데이터를 올린다. */
	       port->base + PCIE_CFG_HEADER2);

	/* Write Cfgwr data */
	val = val << 8 * (where & 3);
	writel(val, port->base + PCIE_CFG_WDATA);
/* [한국어] 아래는 MSI 관련 콜백들이다. 포트마다 32개 벡터를 자체 비트맵으로 관리한다. */

	/* Trigger h/w to transmit Cfgwr TLP */
	val = readl(port->base + PCIE_APP_TLP_REQ);
	val |= APP_CFG_REQ;
	/* [한국어] 쓰기 요청을 시작한다. 읽기 쪽과 달리 val 변수를 재사용하는데, 위에서
	 * 이미 데이터 레지스터에 쓴 뒤라 문제가 없다. */
	writel(val, port->base + PCIE_APP_TLP_REQ);
/* [한국어] 완료 결과를 그대로 반환한다 -- 꺼낼 값이 없다. */

	/* Check completion status */
	return mtk_pcie_check_cfg_cpld(port);
}

/* [한국어]
 * mtk_pcie_find_port - 어느 루트 포트를 거쳐야 하는지 버스 트리를 거슬러 찾는다
 *
 * @bus: 접근하려는 버스.
 * @devfn: 장치/함수 번호.
 * @return: 담당 포트, 또는 없으면 NULL.
 *
 * 이 컨트롤러는 루트 포트가 여러 개이고 **각각 별도의 레지스터 창**을
 * 갖는다. 그래서 설정 접근 전에 '이 목적지가 어느 포트 아래에 있는가' 를
 * 먼저 정해야 한다.
 *
 * while 루프가 그 일을 한다. bus->number 가 0 이 아니면 루트 버스가 아니므로
 * bus->self(그 버스를 만든 브리지 장치)를 타고 한 단계 위로 올라간다.
 * 루트 버스에 닿으면 devfn 이 곧 루트 포트의 슬롯 번호가 되고, 그것으로
 * ports 목록에서 짝을 찾는다.
 *
 * 즉 하위 버스의 어떤 장치를 겨누든, 결국 그 경로의 **첫 루트 포트**를
 * 찾아내는 것이 이 함수의 목적이다.
 *
 * 실행 컨텍스트: 설정 접근 경로. 목록을 순회하지만 잠금이 없다 --
 * ports 목록은 프로브 때 완성된 뒤 열거 중에는 변하지 않는다는 전제다.
 *
 * 호출 체인:
 *   mtk_pcie_config_read / mtk_pcie_config_write → [이 함수]
 */
static struct mtk_pcie_port *mtk_pcie_find_port(struct pci_bus *bus,
						unsigned int devfn)
{
	struct mtk_pcie *pcie = bus->sysdata;
	struct mtk_pcie_port *port;
	/* [한국어] 버스 트리를 거슬러 올라가며 쓸 임시 포인터. */
	struct pci_dev *dev = NULL;
/* [한국어] 아래 루프가 루트 버스에 닿을 때까지 한 단계씩 올라간다. */

	/*
	 * Walk the bus hierarchy to get the devfn value
	 * of the port in the root bus.
	 */
	while (bus && bus->number) {
		dev = bus->self;
		/* [한국어] 브리지 장치가 속한 버스로 올라간다. */
		bus = dev->bus;
		/* [한국어] 그 브리지의 devfn 이 다음 대조 대상이 된다. 루프가 끝나면 이 값이
		 * 루트 포트의 슬롯 번호를 담는다. */
		devfn = dev->devfn;
	/* [한국어] 이제 devfn 이 루트 버스 기준이다. */
	}

	list_for_each_entry(port, &pcie->ports, list)
		/* [한국어] 슬롯 번호로 담당 포트를 찾는다. */
		if (port->slot == PCI_SLOT(devfn))
			/* [한국어] 찾았다. */
			return port;
/* [한국어] 목록에 없으면 그 슬롯에 포트가 없다는 뜻이다. */

	return NULL;
}

/* [한국어]
 * mtk_pcie_config_read - v2 하드웨어의 설정공간 읽기 진입점
 *
 * @bus, @devfn, @where, @size: 표준 설정 읽기 인자.
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL, PCIBIOS_DEVICE_NOT_FOUND, 또는 PCIBIOS_SET_FAILED.
 *
 * map_bus 콜백을 쓰지 않고 read/write 를 직접 구현하는 점이 v1(mtk_pcie_ops)과
 * 다르다. v2 는 설정공간이 주소로 매핑되어 있지 않아, 주소를 돌려줄 방법이
 * 없기 때문이다.
 *
 * 담당 포트를 못 찾으면 PCIBIOS_DEVICE_NOT_FOUND 를 돌려준다. PCI 코어는
 * 그것을 '그 자리에 장치 없음' 으로 처리하므로, 존재하지 않는 슬롯을
 * 훑는 열거가 자연히 걸러진다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근 경로.
 *
 * 호출 체인:
 *   pci_read_config_* → ops->read → [이 함수] → mtk_pcie_find_port
 *     → mtk_pcie_hw_rd_cfg
 */
static int mtk_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *val)
{
	struct mtk_pcie_port *port;
	u32 bn = bus->number;
/* [한국어] 담당 포트를 먼저 찾는다. */

	port = mtk_pcie_find_port(bus, devfn);
	/* [한국어] 그 슬롯에 포트가 없다. */
	if (!port)
		/* [한국어] PCI 코어가 이 값을 '장치 없음' 으로 처리하므로, 없는 슬롯을 훑는
		 * 열거가 자연히 걸러진다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return mtk_pcie_hw_rd_cfg(port, bn, devfn, where, size, val);
}

/* [한국어]
 * mtk_pcie_config_write - v2 하드웨어의 설정공간 쓰기 진입점
 *
 * @bus, @devfn, @where, @size, @val: 표준 설정 쓰기 인자.
 * @return: PCIBIOS_SUCCESSFUL, PCIBIOS_DEVICE_NOT_FOUND, 또는 PCIBIOS_SET_FAILED.
 *
 * 읽기 쪽과 대칭이다. 포트를 찾고 TLP 조립 함수에 넘긴다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근 경로.
 *
 * 호출 체인:
 *   pci_write_config_* → ops->write → [이 함수] → mtk_pcie_find_port
 *     → mtk_pcie_hw_wr_cfg
 */
static int mtk_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	struct mtk_pcie_port *port;
	u32 bn = bus->number;
/* [한국어] 쓰기 쪽도 같은 절차다. */

	port = mtk_pcie_find_port(bus, devfn);
	/* [한국어] 포트를 못 찾았다. */
	if (!port)
		/* [한국어] 읽기와 같은 코드를 돌려준다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return mtk_pcie_hw_wr_cfg(port, bn, devfn, where, size, val);
/* [한국어] v2 는 map_bus 를 쓰지 않으므로 이 두 함수가 전부다. */
}

static struct pci_ops mtk_pcie_ops_v2 = {
	/* [한국어] 읽기 콜백. map_bus 가 없는 것이 v1 과의 결정적 차이다. */
	.read  = mtk_pcie_config_read,
	/* [한국어] 쓰기 콜백. */
	.write = mtk_pcie_config_write,
};

/* [한국어]
 * mtk_compose_msi_msg - 디바이스가 써야 할 MSI 주소/데이터를 조립한다
 *
 * @data: 이 벡터의 irq_data. chip_data 에 포트가, hwirq 에 벡터 번호가 있다.
 * @msg: 채워 넣을 MSI 메시지.
 * @return: 없음.
 *
 * MSI 목적지는 이 포트의 PCIE_MSI_VECTOR 레지스터다. 디바이스가 그 주소에
 * hwirq 값을 쓰면 컨트롤러가 해당 벡터의 상태 비트를 세운다.
 *
 * address_hi 를 0 으로 두는 것은 이 하드웨어의 MSI 목적지가 32비트 주소
 * 공간 안에 있다는 뜻이다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 주소를 얻는 데
 * virt_to_phys(port->base + PCIE_MSI_VECTOR) 를 쓴다. port->base 는
 * devm_platform_ioremap_resource_byname 이 돌려준 __iomem 포인터인데,
 * virt_to_phys() 는 일반적으로 직접 매핑(lowmem) 주소에 대해 정의된 변환이라
 * ioremap 결과에 적용하는 것은 관례에서 벗어난다. 자원의 물리 주소를 따로
 * 보관해 쓰는 편이 통상적인 방식이다. 같은 산술이
 * mtk_pcie_enable_msi() 에도 있어 두 곳이 일관되게 동작한다.
 *
 * 실행 컨텍스트: 디바이스가 MSI 벡터를 할당받는 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_msi 코어 → chip->irq_compose_msi_msg → [이 함수]
 */
static void mtk_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(data);
	phys_addr_t addr;
/* [한국어] 아래에서 MSI 목적지 주소를 계산한다. */

	/* MT2712/MT7622 only support 32-bit MSI addresses */
	addr = virt_to_phys(port->base + PCIE_MSI_VECTOR);
	msg->address_hi = 0;
	/* [한국어] 하위 32비트만 쓴다. 위에서 address_hi 를 0 으로 두었으므로 이 하드웨어의
	 * MSI 목적지는 32비트 공간 안에 있다는 뜻이다. */
	msg->address_lo = lower_32_bits(addr);
/* [한국어] 데이터로는 벡터 번호를 그대로 쓴다. */

	msg->data = data->hwirq;
/* [한국어] 디버그 로그. hwirq 를 int 로 캐스팅하는 것은 irq_hw_number_t 의 폭이
 * 아키텍처마다 달라 포맷과 어긋나는 것을 막기 위해서다. */

	dev_dbg(port->pcie->dev, "msi#%d address_hi %#x address_lo %#x\n",
		/* [한국어] 로그 인자들. */
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

/* [한국어]
 * mtk_msi_ack_irq - 처리한 MSI 벡터의 상태 비트를 지운다
 *
 * @data: 처리 중인 벡터의 irq_data.
 * @return: 없음.
 *
 * PCIE_IMSI_STATUS 는 write-1-to-clear 라, 지우려는 비트 하나만 세운 값을
 * 그대로 써 넣는다. 읽고 고쳐 쓸 필요가 없으므로 잠금도 필요 없다 --
 * 다른 비트에 0 을 쓰는 것은 그 비트에 아무 영향이 없다.
 *
 * 이 소거가 있어야 mtk_pcie_intr_handler 의 while 루프가 끝난다. 그 루프는
 * IMSI_STATUS 가 0 이 될 때까지 도는데, 여기서 비트를 지우지 않으면
 * 무한히 같은 벡터를 다시 올린다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   handle_edge_irq → chip->irq_ack → [이 함수] → writel
 */
static void mtk_msi_ack_irq(struct irq_data *data)
{
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(data);
	u32 hwirq = data->hwirq;
/* [한국어] write-1-to-clear 라 잠금 없이 안전하다. */

	writel(1 << hwirq, port->base + PCIE_IMSI_STATUS);
/* [한국어] 이 소거가 intr_handler 의 while 루프를 끝내는 조건이다. */
}

static struct irq_chip mtk_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 에 나타날 이름. */
	.name			= "MTK MSI",
	/* [한국어] 메시지 조립 콜백. 마스크/언마스크 콜백이 없는 점에 유의 -- 이 하드웨어는
	 * 개별 MSI 벡터를 막을 수 없다. */
	.irq_compose_msi_msg	= mtk_compose_msi_msg,
	.irq_ack		= mtk_msi_ack_irq,
};

/* [한국어]
 * mtk_pcie_irq_domain_alloc - MSI 비트맵에서 벡터 하나를 잡아 virq 에 연결한다
 *
 * @domain: 이 포트의 MSI 부모 도메인. host_data 에 포트가 들어 있다.
 * @virq: 커널이 배정한 가상 IRQ 번호.
 * @nr_irqs: 요청 개수. 이 하드웨어는 1 만 지원한다.
 * @args: 쓰지 않는다.
 * @return: 0 성공, -ENOSPC 면 32개 벡터가 모두 쓰이는 중.
 *
 * WARN_ON(nr_irqs != 1) 이 이 구현의 제약을 드러낸다. multi-MSI 는 연속되고
 * 정렬된 벡터 묶음을 요구하는데, 여기서는 find_first_zero_bit 로 **아무
 * 빈자리 하나**만 잡으므로 연속성을 보장할 수 없다. 그래서 도메인 플래그에도
 * MSI_FLAG_MULTI_PCI_MSI 가 없다(MTK_MSI_FLAGS_SUPPORTED 참조).
 *
 * 비트맵을 **뮤텍스**로 지키는 점이 DesignWare(raw 스핀락)와 다르다. 이
 * 경로는 벡터 할당/해제뿐이라 프로세스 문맥에서만 불리기 때문이다.
 * irq_domain_set_info 는 잠금 밖에서 부른다 -- 그 함수가 잠들 수 있다.
 *
 * 흐름 핸들러로 handle_edge_irq 를 거는 이유: MSI 는 메시지 한 번이 곧 한
 * 번의 사건이고 되풀이 신호가 없다.
 *
 * 실행 컨텍스트: 벡터 할당을 요청하는 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors → msi 코어 → domain_ops->alloc → [이 함수]
 */
static int mtk_pcie_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				     unsigned int nr_irqs, void *args)
{
	struct mtk_pcie_port *port = domain->host_data;
	unsigned long bit;
/* [한국어] 아래 비트맵 조작을 잠금으로 감싼다. */

	WARN_ON(nr_irqs != 1);
	/* [한국어] 뮤텍스를 잡는다. 프로세스 문맥 전용이라 스핀락이 필요 없다. */
	mutex_lock(&port->lock);

	bit = find_first_zero_bit(port->msi_irq_in_use, MTK_MSI_IRQS_NUM);
	/* [한국어] 32개가 모두 쓰이는 중이다. */
	if (bit >= MTK_MSI_IRQS_NUM) {
		/* [한국어] 반환 전에 반드시 잠금을 푼다. */
		mutex_unlock(&port->lock);
		return -ENOSPC;
	}

	__set_bit(bit, port->msi_irq_in_use);
/* [한국어] 잠금을 푼 뒤 도메인 정보를 설정한다 -- irq_domain_set_info 가 잠들 수
 * 있으므로 잠금 안에서 부르면 안 된다. */

	mutex_unlock(&port->lock);

	irq_domain_set_info(domain, virq, bit, &mtk_msi_bottom_irq_chip,
			    /* [한국어] host_data(포트)를 chip_data 로 심고, handle_edge_irq 를 흐름 핸들러로 건다.
			     * MSI 는 메시지 한 번이 곧 한 번의 사건이라 엣지가 맞다. */
			    domain->host_data, handle_edge_irq,
			    NULL, NULL);

	return 0;
}

/* [한국어]
 * mtk_pcie_irq_domain_free - 할당했던 MSI 벡터를 비트맵에 되돌린다
 *
 * @domain: 이 포트의 MSI 도메인.
 * @virq: 반납할 가상 IRQ 번호.
 * @nr_irqs: 반납 개수.
 * @return: 없음.
 *
 * 포트를 domain->host_data 가 아니라 **irq_data 의 chip_data** 에서 꺼내는
 * 점이 alloc 과 다르다. 두 경로 모두 같은 포트에 닿지만, 여기서는 어차피
 * d->hwirq 가 필요해 irq_data 를 먼저 얻기 때문이다.
 *
 * 이미 비어 있는 비트를 해제하려 하면 오류를 찍고 **비트맵은 건드리지
 * 않는다.** 이중 해제를 조용히 넘기면 나중에 같은 벡터가 두 번 할당되므로,
 * 로그를 남기고 상태를 그대로 두는 편이 안전하다.
 *
 * 마지막의 irq_domain_free_irqs_parent 는 부모(상위 도메인) 쪽 자원까지
 * 반납한다. 이 도메인이 msi_create_parent_irq_domain 으로 만들어졌기 때문에
 * 필요한 호출이다.
 *
 * 실행 컨텍스트: pci_free_irq_vectors 등의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_free_irq_vectors → msi 코어 → domain_ops->free → [이 함수]
 */
static void mtk_pcie_irq_domain_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(d);
/* [한국어] 아래 비트맵 조작을 잠금으로 감싼다. */

	mutex_lock(&port->lock);

	if (!test_bit(d->hwirq, port->msi_irq_in_use))
		/* [한국어] 이미 비어 있는 비트를 해제하려 한다 -- 이중 해제다. */
		dev_err(port->pcie->dev, "trying to free unused MSI#%lu\n",
			/* [한국어] 비트맵을 건드리지 않고 로그만 남긴다. 조용히 넘기면 나중에 같은 벡터가
			 * 두 번 할당된다. */
			d->hwirq);
	else
		__clear_bit(d->hwirq, port->msi_irq_in_use);
/* [한국어] 정상 경로에서만 비트를 지웠다. */

	mutex_unlock(&port->lock);

	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
/* [한국어] 부모 도메인 쪽 자원까지 반납한다. 이 도메인이 msi_create_parent_irq_domain
 * 으로 만들어졌기 때문에 필요하다. */
}

static const struct irq_domain_ops msi_domain_ops = {
	/* [한국어] 할당 콜백. */
	.alloc	= mtk_pcie_irq_domain_alloc,
	/* [한국어] 해제 콜백. 이 도메인은 이 둘만 제공한다. */
	.free	= mtk_pcie_irq_domain_free,
};

/* [한국어] 자식 MSI 도메인이 반드시 갖춰야 할 플래그. **NO_AFFINITY** 가 들어 있는
 * 것은 이 하드웨어가 MSI 를 특정 CPU 로 보낼 수 없기 때문이다. */
#define MTK_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				MSI_FLAG_USE_DEF_CHIP_OPS	| \
				MSI_FLAG_NO_AFFINITY)

/* [한국어] 지원 기능. MSI_FLAG_MULTI_PCI_MSI 가 **없는** 점이 요점 --
 * irq_domain_alloc 이 연속 벡터를 보장하지 못하므로 multi-MSI 를 허용하지
 * 않는다. */
#define MTK_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| \
				 MSI_FLAG_PCI_MSIX)

static const struct msi_parent_ops mtk_msi_parent_ops = {
	/* [한국어] 자식 도메인이 반드시 갖춰야 할 플래그. NO_AFFINITY 가 들어 있는 것은
	 * 이 하드웨어가 MSI 를 특정 CPU 로 보낼 수 없기 때문이다. */
	.required_flags		= MTK_MSI_FLAGS_REQUIRED,
	/* [한국어] 지원 기능. **MSI_FLAG_MULTI_PCI_MSI 가 없다** -- alloc 이 연속 벡터를
	 * 보장하지 못하므로 multi-MSI 를 허용하지 않는다. */
	.supported_flags	= MTK_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "MTK-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * mtk_pcie_allocate_msi_domains - 이 포트의 MSI 부모 도메인을 만든다
 *
 * @port: 대상 포트. 만들어진 도메인이 port->inner_domain 에 저장된다.
 * @return: 0 성공, -ENOMEM 생성 실패.
 *
 * **포트마다 별도의 도메인**을 만드는 점이 이 드라이버의 특징이다. 각 포트가
 * 자기 MSI 레지스터 묶음(PCIE_IMSI_*)과 32개 벡터 비트맵을 따로 갖기
 * 때문이다. 그래서 도메인 크기도 MTK_MSI_IRQS_NUM(32)으로 고정이다.
 *
 * fwnode 로 포트 노드가 아니라 **컨트롤러 장치의 노드**를 쓰는 점에 유의.
 * 포트가 여러 개면 같은 fwnode 로 도메인이 여러 개 만들어진다.
 *
 * mutex_init 이 선언보다 앞에 오는 배치는 C99 의 혼합 선언을 쓴 것으로,
 * 잠금을 도메인 생성보다 먼저 준비해 두려는 의도로 보인다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_init_irq_domain → [이 함수] → msi_create_parent_irq_domain
 */
static int mtk_pcie_allocate_msi_domains(struct mtk_pcie_port *port)
{
	mutex_init(&port->lock);

	struct irq_domain_info info = {
		/* [한국어] 포트 노드가 아니라 **컨트롤러 장치의 노드**를 쓴다. 포트가 여러 개면
		 * 같은 fwnode 로 도메인이 여러 개 만들어진다. */
		.fwnode		= dev_fwnode(port->pcie->dev),
		/* [한국어] 위에서 정의한 alloc/free 쌍. */
		.ops		= &msi_domain_ops,
		.host_data	= port,
		.size		= MTK_MSI_IRQS_NUM,
	};

	port->inner_domain = msi_create_parent_irq_domain(&info, &mtk_msi_parent_ops);
	/* [한국어] 도메인 생성 실패. */
	if (!port->inner_domain) {
		/* [한국어] MSI 없이는 대부분의 장치가 성능을 내지 못하므로 명확히 알린다. */
		dev_err(port->pcie->dev, "failed to create IRQ domain\n");
		/* [한국어] 호출자(init_irq_domain)가 INTx 도메인까지 되돌린다. */
		return -ENOMEM;
	}

	return 0;
}

/* [한국어]
 * mtk_pcie_enable_msi - MSI 목적지 주소를 하드웨어에 알리고 MSI 인터럽트를 연다
 *
 * @port: 대상 포트.
 * @return: 없음.
 *
 * 두 가지를 한다:
 *  1. PCIE_IMSI_ADDR 에 MSI 목적지의 하위 32비트를 쓴다. 디바이스가 그
 *     주소로 쓰기를 보내면 컨트롤러가 가로채 벡터 상태 비트를 세운다.
 *     주소 산술이 mtk_compose_msi_msg 와 같아야 두 쪽이 맞아떨어진다.
 *  2. PCIE_INT_MASK 에서 MSI_MASK 비트를 지워 MSI 인터럽트를 허용한다.
 *     읽고-고쳐-쓰기라 다른 마스크 비트는 보존된다.
 *
 * startup_port_v2 의 마지막 부분에서, 링크가 선 뒤에 불린다. 링크가 서기
 * 전에 열면 학습 중의 잡음이 인터럽트로 올라올 수 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_startup_port_v2 → [이 함수] → virt_to_phys → writel
 */
static void mtk_pcie_enable_msi(struct mtk_pcie_port *port)
{
	u32 val;
	phys_addr_t msg_addr;
/* [한국어] 아래 두 단계로 MSI 를 연다. */

	msg_addr = virt_to_phys(port->base + PCIE_MSI_VECTOR);
	/* [한국어] 목적지 주소의 하위 32비트. 이 산술이 mtk_compose_msi_msg 와 같아야
	 * 디바이스가 쓰는 주소와 하드웨어가 가로채는 주소가 맞아떨어진다. */
	val = lower_32_bits(msg_addr);
	/* [한국어] 하드웨어에 목적지를 알린다. */
	writel(val, port->base + PCIE_IMSI_ADDR);
/* [한국어] 이제 마스크를 푼다. */

	val = readl(port->base + PCIE_INT_MASK);
	/* [한국어] MSI 비트만 지운다. 읽고-고쳐-쓰기라 INTx 마스크 등 다른 비트는 보존된다. */
	val &= ~MSI_MASK;
	/* [한국어] 갱신된 마스크를 반영한다. */
	writel(val, port->base + PCIE_INT_MASK);
}

/* [한국어]
 * mtk_pcie_irq_teardown - 모든 포트의 연쇄 핸들러와 IRQ 도메인을 걷어낸다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 없음.
 *
 * 순서가 중요하다. 먼저 연쇄 핸들러를 떼어(NULL, NULL) 새 인터럽트가 도메인에
 * 닿지 못하게 한 뒤에 도메인을 없앤다. 반대로 하면 남아 있던 인터럽트가
 * 이미 해제된 도메인을 참조한다.
 *
 * 두 도메인을 각각 NULL 검사하는 이유: INTx 도메인은 항상 만들어지지만
 * MSI 도메인은 CONFIG_PCI_MSI 가 켜졌을 때만 만들어지고, 만들다 실패했을
 * 수도 있다.
 *
 * 마지막의 irq_dispose_mapping 은 부모 IRQ 의 매핑을 푼다.
 *
 * list_for_each_entry_safe 를 쓰지만 이 루프는 목록을 수정하지 않는다 --
 * 다른 정리 함수들과 형태를 맞춘 것으로 보인다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_remove → [이 함수] → irq_set_chained_handler_and_data
 *     → irq_domain_remove → irq_dispose_mapping
 */
static void mtk_pcie_irq_teardown(struct mtk_pcie *pcie)
{
	struct mtk_pcie_port *port, *tmp;

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] **먼저** 핸들러를 떼어 새 인터럽트가 도메인에 닿지 못하게 한다. */
		irq_set_chained_handler_and_data(port->irq, NULL, NULL);
/* [한국어] 이제 도메인을 없애도 안전하다. */

		if (port->irq_domain)
			/* [한국어] INTx 도메인은 항상 만들어지지만, 프로브가 중간에 실패했으면 없을 수 있다. */
			irq_domain_remove(port->irq_domain);

		if (IS_ENABLED(CONFIG_PCI_MSI)) {
			/* [한국어] MSI 도메인은 CONFIG_PCI_MSI 가 켜졌을 때만 만들어진다. */
			if (port->inner_domain)
				/* [한국어] 만들다 실패했을 수도 있어 NULL 검사가 필요하다. */
				irq_domain_remove(port->inner_domain);
		}

		irq_dispose_mapping(port->irq);
	}
}

/* [한국어]
 * mtk_pcie_intx_map - INTx 도메인의 hwirq 하나를 커널 virq 에 붙인다
 *
 * @domain: INTx 선형 도메인.
 * @irq: 커널이 배정한 가상 IRQ 번호.
 * @hwirq: 0~3 (INTA~INTD).
 * @return: 항상 0.
 *
 * **dummy_irq_chip 을 쓰는 점**이 이 드라이버의 특징이다. 마스크/언마스크
 * 콜백이 없는 빈 칩이라, 개별 INTx 를 소프트웨어로 막을 수 없다 -- 이
 * 하드웨어의 INT_MASK 레지스터가 INTx 넷을 한 덩어리(INTX_MASK)로만
 * 다루기 때문이다.
 *
 * 흐름 핸들러도 handle_level_irq 가 아니라 handle_simple_irq 다. 레벨 판을
 * 쓰려면 마스크 콜백이 있어야 하는데 없기 때문이고, 대신 상태 소거를
 * 핸들러(mtk_pcie_intr_handler)가 직접 한다.
 *
 * 실행 컨텍스트: 장치가 INTx 를 요청할 때의 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_mapping → domain_ops->map → [이 함수]
 */
static int mtk_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);
/* [한국어] dummy_irq_chip 과 handle_simple_irq 를 쓰므로, 상태 소거는 핸들러가 직접 한다. */

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 이 도메인은 map 만 제공한다. 선형 도메인이라 hwirq↔virq 대응은 커널이 관리한다. */
	.map = mtk_pcie_intx_map,
};

/* [한국어]
 * mtk_pcie_init_irq_domain - INTx 도메인과 (가능하면) MSI 도메인을 만든다
 *
 * @port: 대상 포트.
 * @node: 이 포트의 DT 노드.
 * @return: 0 성공, -ENODEV 는 자식 인터럽트 컨트롤러 노드 없음 또는 도메인
 *          생성 실패, 그 외는 MSI 도메인 생성 실패값.
 *
 * DT 관례상 PCIe 포트의 INTx 는 **자식 노드**로 기술된다. 그래야 하위 장치
 * 노드가 interrupt-parent 로 그것을 가리킬 수 있다. of_get_next_child 로
 * 첫 자식을 가져오는데, 이름을 보지 않고 첫 번째를 쓰는 점에 유의 --
 * 이 바인딩에서는 자식이 인터럽트 컨트롤러 하나뿐이라는 전제다.
 *
 * of_node_put 을 도메인 생성 **직후** 부르는 순서가 의도적이다. 도메인이
 * fwnode 를 따로 붙들므로 여기서 놓아도 안전하고, 실패 경로에서도 참조가
 * 새지 않는다.
 *
 * MSI 도메인 생성이 실패하면 방금 만든 INTx 도메인을 되돌린다 -- 두 도메인이
 * 짝으로 존재해야 teardown 이 일관되게 동작한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_setup_irq → [이 함수] → irq_domain_create_linear
 *     → mtk_pcie_allocate_msi_domains
 */
static int mtk_pcie_init_irq_domain(struct mtk_pcie_port *port,
				    struct device_node *node)
{
	struct device *dev = port->pcie->dev;
	struct device_node *pcie_intc_node;
	/* [한국어] MSI 도메인 생성 결과를 담을 변수. */
	int ret;
/* [한국어] 아래에서 자식 인터럽트 컨트롤러 노드를 찾는다. */

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL);
	if (!pcie_intc_node) {
		/* [한국어] DT 에 자식 노드가 없다 -- 하위 장치가 INTx 를 요청할 대상이 없다. */
		dev_err(dev, "no PCIe Intc node found\n");
		/* [한국어] DT 를 고쳐야 하는 문제이므로 프로브를 중단한다. */
		return -ENODEV;
	}

	port->irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						    /* [한국어] 도메인 연산과 host_data(포트)를 넘긴다. host_data 는 intx_map 이 각 IRQ 의
						     * chip_data 로 다시 심는다. */
						    &intx_domain_ops, port);
	of_node_put(pcie_intc_node);
	if (!port->irq_domain) {
		/* [한국어] 도메인 생성 실패. */
		dev_err(dev, "failed to get INTx IRQ domain\n");
		/* [한국어] INTx 없이는 이 포트를 쓸 수 없다. */
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* [한국어] MSI 가 빌드에 있으면 MSI 도메인도 만든다. */
		ret = mtk_pcie_allocate_msi_domains(port);
		/* [한국어] MSI 도메인 생성 실패. */
		if (ret) {
			/* [한국어] 방금 만든 INTx 도메인을 되돌린다 -- 두 도메인이 짝으로 존재해야
			 * teardown 이 일관되게 동작한다. */
			irq_domain_remove(port->irq_domain);
			return ret;
		}
	}

	return 0;
}

/* [한국어]
 * mtk_pcie_intr_handler - 포트의 인터럽트 하나를 INTx 와 MSI 로 분해한다
 *
 * @desc: 부모 IRQ 의 irq_desc. 핸들러 데이터에 포트가 매달려 있다.
 * @return: 없음.
 *
 * 이 컨트롤러는 포트당 인터럽트 선 하나에 INTx 넷과 MSI 32개를 모두 실어
 * 보내고, 무엇이 울렸는지는 PCIE_INT_STATUS 의 비트로 알려 준다.
 *
 * INTx 부분:
 *   상태 비트가 INTX_SHIFT(16)부터 시작하므로 for_each_set_bit_**from** 으로
 *   그 자리부터 훑는다. 비트를 **먼저 지우고** 도메인에 올리는 순서가 요점 --
 *   dummy_irq_chip 을 쓰는 탓에 ack 콜백이 없어, 여기서 지우지 않으면
 *   같은 인터럽트가 계속 다시 들어온다.
 *   도메인에 올릴 때는 bit - INTX_SHIFT 로 0~3 범위로 되돌린다.
 *
 * MSI 부분:
 *   먼저 INT_STATUS 의 MSI 요약 비트를 지운 뒤, IMSI_STATUS 가 0 이 될
 *   때까지 **while 루프**로 반복해 읽는다. 개별 벡터 비트는 여기서 지우지
 *   않고 mtk_msi_ack_irq 가 지우므로, 그 소거가 반영되어 IMSI_STATUS 가
 *   0 이 되어야 루프가 끝난다. 요약 비트를 루프 **전에** 지우는 것도
 *   의도적이다 -- 루프를 도는 동안 새 MSI 가 오면 요약 비트가 다시 서서
 *   다음 인터럽트로 처리된다.
 *
 * 변수 bit 가 두 부분에서 재사용되는 점에 유의. INTx 루프가 끝나면 bit 는
 * 16 이상이 되어 있지만, MSI 쪽 for_each_set_bit 은 0 부터 다시 시작하므로
 * 영향이 없다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   GIC → 부모 irq 핸들러 → [이 함수] → generic_handle_domain_irq
 */
static void mtk_pcie_intr_handler(struct irq_desc *desc)
{
	struct mtk_pcie_port *port = irq_desc_get_handler_data(desc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	/* [한국어] 읽어 온 인터럽트 상태 비트들. for_each_set_bit 계열이 unsigned long
	 * 포인터를 받으므로 이 타입이어야 한다. */
	unsigned long status;
	/* [한국어] **INTX_SHIFT(16)에서 시작한다.** INTx 상태 비트가 16번부터 놓여 있어,
	 * 아래 for_each_set_bit_from 이 그 자리부터 훑게 하기 위해서다. */
	u32 bit = INTX_SHIFT;
/* [한국어] 부모 컨트롤러의 진입 처리를 먼저 한다. */

	chained_irq_enter(irqchip, desc);
/* [한국어] 인터럽트 상태를 한 번만 읽어 INTx 와 MSI 판정에 함께 쓴다. */

	status = readl(port->base + PCIE_INT_STATUS);
	/* [한국어] INTx 넷 중 하나라도 걸렸는지 먼저 본다. */
	if (status & INTX_MASK) {
		/* [한국어] 상한이 PCI_NUM_INTX + INTX_SHIFT 인 것은 비트 16~19 만 훑는다는 뜻이다. */
		for_each_set_bit_from(bit, &status, PCI_NUM_INTX + INTX_SHIFT) {
			/* Clear the INTx */
			writel(1 << bit, port->base + PCIE_INT_STATUS);
			generic_handle_domain_irq(port->irq_domain,
						  bit - INTX_SHIFT);
		}
	}

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* [한국어] MSI 요약 비트. 중괄호 앞 공백이 없는 것은 상류 그대로다. */
		if (status & MSI_STATUS){
			/* [한국어] 개별 벡터 상태를 담을 변수. 루프 안에서 선언해 범위를 좁혔다. */
			unsigned long imsi_status;
/* [한국어] 아래에서 요약 비트를 먼저 지운다. */

			/*
			 * The interrupt status can be cleared even if the
			 * MSI status remains pending. As such, given the
			 * edge-triggered interrupt type, its status should
			 * be cleared before being dispatched to the
			 * handler of the underlying device.
			 */
			writel(MSI_STATUS, port->base + PCIE_INT_STATUS);
			while ((imsi_status = readl(port->base + PCIE_IMSI_STATUS))) {
				/* [한국어] 걸린 벡터마다 도메인에 올린다. 개별 비트는 여기서 지우지 않고
				 * mtk_msi_ack_irq 가 지운다 -- 그 소거가 반영되어야 while 루프가 끝난다. */
				for_each_set_bit(bit, &imsi_status, MTK_MSI_IRQS_NUM)
					/* [한국어] MSI 도메인에 올리면 handle_edge_irq → ack → 디바이스 핸들러 순으로 진행된다. */
					generic_handle_domain_irq(port->inner_domain, bit);
			/* [한국어] IMSI_STATUS 가 0 이 될 때까지 다시 읽는다. */
			}
		}
	}

	chained_irq_exit(irqchip, desc);
}

/* [한국어]
 * mtk_pcie_setup_irq - 도메인을 만들고 포트의 인터럽트에 연쇄 핸들러를 건다
 *
 * @port: 대상 포트.
 * @node: 이 포트의 DT 노드.
 * @return: 0 성공, 음수는 도메인 생성 또는 IRQ 조회 실패값.
 *
 * IRQ 를 얻는 방법이 두 갈래인 점이 특징이다:
 *  - DT 에 "interrupt-names" 속성이 있으면 "pcie_irq" 라는 이름으로 찾는다.
 *  - 없으면 **포트 슬롯 번호를 인덱스로** 써서 찾는다. 하나의 컨트롤러
 *    노드가 여러 포트의 인터럽트를 순서대로 나열하던 옛 바인딩을 위한
 *    폴백이다.
 *
 * 이름 속성을 dev->of_node(컨트롤러 노드)에서 보는데, 인자로 받은 node(포트
 * 노드)가 아니라는 점에 유의. 두 바인딩 모두 인터럽트를 컨트롤러 노드에
 * 두기 때문이다.
 *
 * 도메인을 먼저 만들고 핸들러를 나중에 거는 순서가 필수다 -- 반대로 하면
 * 도메인이 준비되기 전에 인터럽트가 들어올 수 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_parse_port → soc->setup_irq → [이 함수]
 *     → mtk_pcie_init_irq_domain → irq_set_chained_handler_and_data
 */
static int mtk_pcie_setup_irq(struct mtk_pcie_port *port,
			      struct device_node *node)
{
	struct mtk_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] platform_get_irq 계열을 부르기 위해 필요하다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 도메인 생성 결과. */
	int err;
/* [한국어] 도메인을 먼저 만든다. */

	err = mtk_pcie_init_irq_domain(port, node);
	/* [한국어] 도메인 생성 실패. */
	if (err) {
		/* [한국어] 어느 단계에서 막혔는지 알 수 있게 남긴다. */
		dev_err(dev, "failed to init PCIe IRQ domain\n");
		/* [한국어] 실패값을 그대로 올린다. */
		return err;
	}

	if (of_property_present(dev->of_node, "interrupt-names"))
		/* [한국어] 새 바인딩 -- 이름으로 찾는다. */
		port->irq = platform_get_irq_byname(pdev, "pcie_irq");
	/* [한국어] 옛 바인딩 -- 슬롯 번호를 인덱스로 쓴다. */
	else
		port->irq = platform_get_irq(pdev, port->slot);
/* [한국어] 하나의 컨트롤러 노드가 여러 포트의 인터럽트를 순서대로 나열하던 방식이다. */

	if (port->irq < 0)
		/* [한국어] -EPROBE_DEFER 포함해 그대로 올린다. */
		return port->irq;
/* [한국어] 이제 도메인이 준비됐으므로 핸들러를 걸어도 안전하다. */

	irq_set_chained_handler_and_data(port->irq,
					 mtk_pcie_intr_handler, port);

	return 0;
}

/* [한국어]
 * mtk_pcie_startup_port_v2 - v2 하드웨어의 포트를 리셋부터 주소 창까지 세운다
 *
 * @port: 시작할 포트.
 * @return: 0 성공, -EINVAL 은 MEM 윈도 없음, -ETIMEDOUT 은 링크 실패.
 *
 * MT2712/MT7622/MT7629/AN7583 계열이 쓰는 기동 경로다. 순서와 근거:
 *
 *  1. 브리지의 첫 MEM 윈도를 찾는다. 마지막 단계의 AHB→PCIe 주소 창을
 *     여기서 얻으므로, 없으면 시작할 이유가 없다.
 *  2. LTSSM 과 ASPM L1 을 켠다. 이 SoC 는 그 스위치가 **두 곳 중 하나**에
 *     있다 -- 옛 바인딩은 "subsys" MMIO(pcie->base), 새 바인딩은
 *     syscon regmap(pcie->cfg)이다. 둘 다 없으면 아무것도 하지 않는다.
 *     비트 위치가 슬롯 번호에 따라 8칸씩 떨어진다(PCIE_CSR_LTSSM_EN(x)).
 *  3. SKIP_RSTB 쿼크가 없으면 리셋 절차를 밟는다:
 *       - RST_CTRL 에 0 을 써 모든 리셋을 assert.
 *       - LINKDOWN_RST_EN 만 세운다.
 *       - PCIE_T_PVPERL_MS 만큼 기다린다(전원 유효 후 PERST# 해제까지의
 *         규약 시간).
 *       - PHY/PERST/PIPE/MAC/CRSTB 를 한꺼번에 deassert.
 *     AN7583 은 이 쿼크로 절차를 건너뛴다 -- 부트로더가 이미 했거나
 *     하드웨어가 다르게 처리한다는 뜻이다.
 *  4. FIX_CLASS_ID 쿼크면 벤더 ID 와 클래스 코드를 직접 써 넣는다. 일부
 *     판본이 잘못된 값을 내보내 커널이 브리지로 인식하지 못하기 때문이다.
 *  5. FIX_DEVICE_ID 쿼크면 디바이스 ID 도 SoC 표의 값으로 덮어쓴다.
 *  6. 링크가 설 때까지 최대 100ms 기다린다. 상류 주석대로 Gen1/2 학습에는
 *     그 정도면 충분하다.
 *  7. INTx 마스크를 풀고, MSI 가 빌드에 있으면 MSI 도 연다. **링크가 선
 *     뒤에** 여는 순서가 요점이다.
 *  8. AHB→PCIe 창을 연다. 크기 필드에 fls(resource_size(mem)) 를 넣는데,
 *     이는 크기의 최상위 비트 위치, 즉 2의 지수를 뜻한다.
 *  9. PCIe→AHB 창을 고정 크기(PCIE2AHB_SIZE)로 열어 인바운드 DMA 를 허용한다.
 *
 * 실행 컨텍스트: 프로브·재개 경로의 프로세스 문맥. msleep 이 있어 잠들 수
 * 있어야 한다.
 *
 * 호출 체인:
 *   mtk_pcie_enable_port → soc->startup → [이 함수]
 *     → readl_poll_timeout → mtk_pcie_enable_msi
 */
static int mtk_pcie_startup_port_v2(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	/* [한국어] AHB→PCIe 창을 잡는 데 쓸 MEM 윈도. NULL 로 시작해 아래에서 채운다. */
	struct resource *mem = NULL;
	/* [한국어] 윈도 목록에서 찾은 항목. */
	struct resource_entry *entry;
	/* [한국어] 이 SoC 의 쿼크와 device_id 를 읽기 위한 설정표. */
	const struct mtk_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 읽고-고쳐-쓰기용 임시값. */
	u32 val;
	/* [한국어] 링크 대기 결과. */
	int err;
/* [한국어] 아래에서 MEM 윈도부터 확인한다. */

	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM);
	/* [한국어] 항목을 찾았으면 자원 포인터를 꺼낸다. */
	if (entry)
		/* [한국어] entry 와 mem 을 나눠 두는 것은 아래 NULL 검사를 한 번에 하기 위해서다. */
		mem = entry->res;
	/* [한국어] MEM 윈도가 없으면 마지막의 AHB→PCIe 창을 잡을 수 없다. */
	if (!mem)
		/* [한국어] DT 의 ranges 가 잘못됐다는 뜻이다. */
		return -EINVAL;

	/* MT7622 platforms need to enable LTSSM and ASPM from PCIe subsys */
	if (pcie->base) {
		val = readl(pcie->base + PCIE_SYS_CFG_V2);
		/* [한국어] 이 포트의 LTSSM 을 켠다. 비트 위치가 슬롯 번호에 따라 8칸씩 떨어진다. */
		val |= PCIE_CSR_LTSSM_EN(port->slot) |
		       /* [한국어] ASPM L1 도 함께 켠다. 두 비트가 같은 8비트 묶음 안에 있다. */
		       PCIE_CSR_ASPM_L1_EN(port->slot);
		writel(val, pcie->base + PCIE_SYS_CFG_V2);
	/* [한국어] 'subsys' 창이 없는 새 바인딩 -- syscon regmap 으로 같은 레지스터에 닿는다. */
	} else if (pcie->cfg) {
		/* [한국어] regmap 경로는 읽지 않고 바꿀 비트만 지정한다. */
		val = PCIE_CSR_LTSSM_EN(port->slot) |
		      /* [한국어] regmap_update_bits 가 마스크와 값을 함께 받으므로 읽기가 필요 없다. */
		      PCIE_CSR_ASPM_L1_EN(port->slot);
		regmap_update_bits(pcie->cfg, PCIE_SYS_CFG_V2, val, val);
	/* [한국어] 둘 다 없으면 LTSSM 을 켜지 않는다 -- 부트로더가 이미 켰다는 전제다. */
	}

	if (!(soc->quirks & MTK_PCIE_SKIP_RSTB)) {
		/* Assert all reset signals */
		writel(0, port->base + PCIE_RST_CTRL);

		/*
		 * Enable PCIe link down reset, if link status changed from
		 * link up to link down, this will reset MAC control registers
		 * and configuration space.
		 */
		writel(PCIE_LINKDOWN_RST_EN, port->base + PCIE_RST_CTRL);

		msleep(PCIE_T_PVPERL_MS);

		/* De-assert PHY, PE, PIPE, MAC and configuration reset	*/
		val = readl(port->base + PCIE_RST_CTRL);
		val |= PCIE_PHY_RSTB | PCIE_PERSTB | PCIE_PIPE_SRSTB |
		       /* [한국어] PHY/PERST/PIPE/MAC/CRSTB 를 **한꺼번에** deassert 한다. 하나씩 풀면
		        * 중간 상태에서 하드웨어가 오동작할 수 있다. */
		       PCIE_MAC_SRSTB | PCIE_CRSTB;
		/* [한국어] 리셋 해제를 반영한다. */
		writel(val, port->base + PCIE_RST_CTRL);
	/* [한국어] SKIP_RSTB 쿼크가 있으면 이 블록 전체를 건너뛴다. */
	}

	/* Set up vendor ID and class code */
	if (soc->quirks & MTK_PCIE_FIX_CLASS_ID) {
		val = PCI_VENDOR_ID_MEDIATEK;
		/* [한국어] 벤더 ID 를 직접 써 넣는다. 일부 판본이 잘못된 값을 내보내기 때문이다. */
		writew(val, port->base + PCIE_CONF_VEND_ID);
/* [한국어] 클래스 코드도 마찬가지다. */

		val = PCI_CLASS_BRIDGE_PCI;
		/* [한국어] PCI_CLASS_BRIDGE_PCI 를 써야 커널이 이 함수를 브리지로 인식해 하위
		 * 버스를 열거한다. */
		writew(val, port->base + PCIE_CONF_CLASS_ID);
	/* [한국어] FIX_CLASS_ID 쿼크가 없는 판본은 하드웨어 값을 그대로 쓴다. */
	}

	if (soc->quirks & MTK_PCIE_FIX_DEVICE_ID)
		/* [한국어] 디바이스 ID 는 SoC 표의 값으로 덮어쓴다 -- 판본마다 다른 값이라
		 * 상수로 박을 수 없다. */
		writew(soc->device_id, port->base + PCIE_CONF_DEVICE_ID);
/* [한국어] 이제 링크를 기다린다. */

	/* 100ms timeout value should be enough for Gen1/2 training */
	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS_V2, val,
				 !!(val & PCIE_PORT_LINKUP_V2), 20,
				 100 * USEC_PER_MSEC);
	if (err)
		/* [한국어] 100ms 안에 링크가 서지 않았다. 슬롯이 비었거나 장치가 응답하지 않는다.
		 * 호출자(enable_port)가 이 실패를 받아 포트를 통째로 버린다. */
		return -ETIMEDOUT;

	/* Set INTx mask */
	val = readl(port->base + PCIE_INT_MASK);
	val &= ~INTX_MASK;
	/* [한국어] INTx 마스크를 푼다. 링크가 선 뒤에 여는 순서가 요점이다. */
	writel(val, port->base + PCIE_INT_MASK);
/* [한국어] MSI 도 마찬가지로 링크 뒤에 연다. */

	if (IS_ENABLED(CONFIG_PCI_MSI))
		/* [한국어] CONFIG_PCI_MSI 가 꺼져 있으면 MSI 레지스터를 건드리지 않는다. */
		mtk_pcie_enable_msi(port);

	/* Set AHB to PCIe translation windows */
	val = lower_32_bits(mem->start) |
	      AHB2PCIE_SIZE(fls(resource_size(mem)));
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_L);
/* [한국어] 아래 두 줄이 AHB→PCIe 주소 창을 연다. */

	val = upper_32_bits(mem->start);
	/* [한국어] 상위 32비트를 별도 레지스터에 쓴다. 64비트 물리 주소를 쓰는 SoC 를 위한 것이다. */
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_H);
/* [한국어] 이제 반대 방향 창을 연다. */

	/* Set PCIe to AXI translation memory space.*/
	val = PCIE2AHB_SIZE | WIN_ENABLE;
	writel(val, port->base + PCIE_AXI_WINDOW0);
/* [한국어] PCIe→AHB 창은 크기가 고정(PCIE2AHB_SIZE)이다. 인바운드 DMA 가
 * 시스템 메모리에 닿게 하는 설정이다. */

	return 0;
}

/* [한국어]
 * mtk_pcie_map_bus - v1 하드웨어의 설정공간 주소를 만든다
 *
 * @bus: 접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: 설정공간 오프셋.
 * @return: 데이터 레지스터의 주소.
 *
 * v1 은 v2 와 접근 방식이 완전히 다르다. TLP 를 손으로 조립하는 대신
 * **주소/데이터 레지스터 쌍**을 쓴다 -- 옛 PCI 의 CF8/CFC 방식과 같은 구조다:
 *  1. CFG_ADDR 에 BDF + 레지스터 번호를 쓴다.
 *  2. CFG_DATA 를 읽거나 쓰면 그 대상에 접근된다.
 *
 * 반환 주소에 (where & 3) 을 더하는 것은, 데이터 레지스터가 4바이트 워드라
 * 1/2 바이트 접근 시 워드 안의 올바른 바이트를 가리키게 하기 위해서다.
 * 그 덕에 read/write 는 공용 헬퍼(pci_generic_config_read/write)로 충분하다.
 *
 * **주소를 쓰는 것과 데이터에 접근하는 것이 원자적이지 않은데 잠금이 없다.**
 * PCI 코어가 설정 접근을 자체 잠금 아래에서 수행한다는 전제다.
 *
 * 포트를 구별하지 않는 점도 v2 와 다르다 -- v1 은 컨트롤러 창(pcie->base)
 * 하나로 모든 접근을 처리한다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근 경로.
 *
 * 호출 체인:
 *   pci_generic_config_read/write → ops->map_bus → [이 함수]
 */
static void __iomem *mtk_pcie_map_bus(struct pci_bus *bus,
				      unsigned int devfn, int where)
{
	struct mtk_pcie *pcie = bus->sysdata;

	writel(PCIE_CONF_ADDR(where, PCI_FUNC(devfn), PCI_SLOT(devfn),
			      /* [한국어] CFG_ADDR 에 BDF + 레지스터 번호를 쓴다. 다음 CFG_DATA 접근이 이 대상에 간다. */
			      bus->number), pcie->base + PCIE_CFG_ADDR);

	return pcie->base + PCIE_CFG_DATA + (where & 3);
/* [한국어] 주소 쓰기와 데이터 접근이 원자적이지 않지만, PCI 코어가 설정 접근을
 * 자체 잠금 아래에서 수행한다는 전제로 잠금이 없다. */
}

static struct pci_ops mtk_pcie_ops = {
	/* [한국어] v1 은 map_bus 를 제공하므로 read/write 는 공용 헬퍼로 충분하다. */
	.map_bus = mtk_pcie_map_bus,
	/* [한국어] v2 가 read/write 를 직접 구현하는 것과 대조된다. */
	.read  = pci_generic_config_read,
	.write = pci_generic_config_write,
};

/* [한국어]
 * mtk_pcie_startup_port - v1 하드웨어의 포트를 기동하고 링크 파라미터를 조정한다
 *
 * @port: 시작할 포트.
 * @return: 0 성공, -ETIMEDOUT 은 링크 실패.
 *
 * MT2701/MT7623 이 쓰는 옛 경로다. v2 와 달리 포트별 창이 아니라 컨트롤러
 * 창(pcie->base)의 슬롯별 비트로 제어한다.
 *
 * 단계:
 *  1. SYS_CFG 의 PORT_PERST(slot) 를 세웠다 지워 PERST# 를 한 번 토글한다.
 *     v2 처럼 유지 시간을 두지 않는 점이 다르다.
 *  2. 포트 창의 LINK_STATUS 에서 링크 업을 최대 100ms 기다린다.
 *  3. INT_ENABLE 에서 이 포트의 인터럽트를 연다.
 *  4. BAR0 을 최대 크기(PCIE_BAR_MAP_MAX)로 열어 둔다. 인바운드 접근이
 *     시스템 메모리에 닿게 하는 설정이다.
 *  5. 클래스 코드와 리비전을 직접 써 넣는다 -- v2 의 FIX_CLASS_ID 쿼크와
 *     같은 목적이지만, v1 은 조건 없이 항상 한다.
 *  6. 흐름 제어 크레딧(FC_CREDIT)과 FTS 개수를 조정한다. 두 값 모두
 *     **자기 자신의 설정공간을 CFG_ADDR/CFG_DATA 로 거쳐** 읽고 쓴다 --
 *     이 레지스터들이 확장 설정공간에 있어 포트 창으로는 닿지 않기 때문이다.
 *     주소를 두 번 쓰는 것(읽기 전, 쓰기 전)은 데이터 접근이 주소를
 *     소비하는 하드웨어이기 때문으로 보인다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): PCIE_FTS_NUM_L0(x) 가
 * `((x) & 0xff << 8)` 로 정의되어 있다. C 에서 `<<` 가 `&` 보다 우선하므로
 * 이는 `(x) & 0xff00` 으로 해석되고, 호출부가 넘기는 0x50 에 대해
 * **0x50 & 0xff00 == 0** 이 된다. 바로 앞 줄이 PCIE_FTS_NUM_MASK(비트 15:8)
 * 를 지우므로, 결과적으로 FTS 필드는 0 으로 설정된다. 의도한 값(0x5000)이
 * 들어가려면 `((x) & 0xff) << 8` 이어야 한다.
 *
 * 실행 컨텍스트: 프로브·재개 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_enable_port → soc->startup → [이 함수] → readl_poll_timeout
 */
static int mtk_pcie_startup_port(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	u32 func = PCI_FUNC(port->slot);
	/* [한국어] 슬롯을 3비트 왼쪽으로 밀었다가 PCI_SLOT 으로 다시 꺼낸다. 결과적으로
	 * port->slot 과 같은 값이 되는 왕복이지만, devfn 형태를 거친다는 의도를
	 * 드러내는 표기다. */
	u32 slot = PCI_SLOT(port->slot << 3);
	/* [한국어] 읽고-고쳐-쓰기용 임시값. */
	u32 val;
	/* [한국어] 링크 대기 결과. */
	int err;
/* [한국어] 아래에서 PERST# 를 토글한다. */

	/* assert port PERST_N */
	val = readl(pcie->base + PCIE_SYS_CFG);
	val |= PCIE_PORT_PERST(port->slot);
	/* [한국어] PERST# 를 assert 한다. v2 와 달리 유지 시간을 두지 않는다. */
	writel(val, pcie->base + PCIE_SYS_CFG);
/* [한국어] 곧바로 해제로 넘어간다. */

	/* de-assert port PERST_N */
	val = readl(pcie->base + PCIE_SYS_CFG);
	val &= ~PCIE_PORT_PERST(port->slot);
	/* [한국어] PERST# 를 deassert 한다. */
	writel(val, pcie->base + PCIE_SYS_CFG);
/* [한국어] 이제 링크를 기다린다. */

	/* 100ms timeout value should be enough for Gen1/2 training */
	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS, val,
				 !!(val & PCIE_PORT_LINKUP), 20,
				 100 * USEC_PER_MSEC);
	if (err)
		/* [한국어] 100ms 안에 링크가 서지 않았다. */
		return -ETIMEDOUT;

	/* enable interrupt */
	val = readl(pcie->base + PCIE_INT_ENABLE);
	val |= PCIE_PORT_INT_EN(port->slot);
	/* [한국어] 이 포트의 인터럽트를 컨트롤러 수준에서 연다. v1 은 포트별 도메인이 없어
	 * 이 비트 하나로 제어한다. */
	writel(val, pcie->base + PCIE_INT_ENABLE);
/* [한국어] 아래에서 BAR 와 클래스 코드를 설정한다. */

	/* map to all DDR region. We need to set it before cfg operation. */
	writel(PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE,
	       port->base + PCIE_BAR0_SETUP);

	/* configure class code and revision ID */
	writel(PCIE_CLASS_CODE | PCIE_REVISION_ID, port->base + PCIE_CLASS);

	/* configure FC credit */
	writel(PCIE_CONF_ADDR(PCIE_FC_CREDIT, func, slot, 0),
	       pcie->base + PCIE_CFG_ADDR);
	val = readl(pcie->base + PCIE_CFG_DATA);
	/* [한국어] 흐름 제어 크레딧 필드를 비운다. */
	val &= ~PCIE_FC_CREDIT_MASK;
	/* [한국어] 0x806c 를 넣는다. 이 값의 근거는 상류에 없고 이 트리에서도 확인하지
	 * 못했다 -- 하드웨어 문서의 권장값으로 보인다. */
	val |= PCIE_FC_CREDIT_VAL(0x806c);
	/* [한국어] **주소를 다시 쓴다.** 데이터 접근이 주소를 소비하는 하드웨어라,
	 * 읽기와 쓰기 각각에 주소 설정이 필요하다. */
	writel(PCIE_CONF_ADDR(PCIE_FC_CREDIT, func, slot, 0),
	       /* [한국어] 주소 레지스터에 같은 대상을 다시 지정한다. */
	       pcie->base + PCIE_CFG_ADDR);
	writel(val, pcie->base + PCIE_CFG_DATA);
/* [한국어] 이제 FTS 개수를 조정한다. */

	/* configure RC FTS number to 250 when it leaves L0s */
	writel(PCIE_CONF_ADDR(PCIE_FTS_NUM, func, slot, 0),
	       pcie->base + PCIE_CFG_ADDR);
	val = readl(pcie->base + PCIE_CFG_DATA);
	/* [한국어] FTS 필드(비트 15:8)를 비운다. */
	val &= ~PCIE_FTS_NUM_MASK;
	/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): PCIE_FTS_NUM_L0(x) 가
	 * `((x) & 0xff << 8)` 로 정의되어 있는데, C 에서 `<<` 가 `&` 보다 우선하므로
	 * `(x) & 0xff00` 으로 해석된다. 0x50 & 0xff00 == 0 이므로 이 OR 는 아무
	 * 비트도 세우지 않고, 바로 위에서 필드를 지웠으니 결과는 FTS = 0 이다.
	 * 의도한 0x5000 이 들어가려면 `((x) & 0xff) << 8` 이어야 한다. */
	val |= PCIE_FTS_NUM_L0(0x50);
	/* [한국어] 쓰기 전에 주소를 다시 지정한다. */
	writel(PCIE_CONF_ADDR(PCIE_FTS_NUM, func, slot, 0),
	       /* [한국어] 위 크레딧 설정과 같은 패턴이다. */
	       pcie->base + PCIE_CFG_ADDR);
	writel(val, pcie->base + PCIE_CFG_DATA);
/* [한국어] 여기까지가 v1 의 기동 절차다. */

	return 0;
}

/* [한국어]
 * mtk_pcie_startup_port_an7583 - AN7583 전용 버스 창을 먼저 잡고 v2 기동으로 넘긴다
 *
 * @port: 시작할 포트.
 * @return: 0 성공, 음수는 syscon 조회/윈도 부재 실패값 또는 v2 기동의 결과.
 *
 * Airoha AN7583 은 PCIe MEM 윈도를 **컨트롤러 바깥의 pbus 레지스터**에도
 * 알려 줘야 트랜잭션이 통과한다. 그 설정만 추가하고 나머지는 v2 경로를
 * 그대로 쓴다.
 *
 * syscon_regmap_lookup_by_phandle_args 가 "mediatek,pbus-csr" phandle 과
 * 함께 인자 두 개를 받아 온다. args[0] 은 기준 주소 레지스터의 오프셋,
 * args[1] 은 크기 마스크 레지스터의 오프셋이다 -- 어느 레지스터를 쓸지를
 * DT 가 정한다.
 *
 * 쓰는 값:
 *  - 기준 주소: entry->res->start - entry->offset. offset 을 빼는 것은
 *    **PCI 버스 쪽 주소**가 필요하기 때문이다(CPU 주소가 아니다).
 *  - 크기: GENMASK(31, __fls(size)). 크기의 최상위 비트부터 31까지를 1 로
 *    만든 마스크로, '이 비트 위쪽이 일치하면 이 창' 이라는 주소 비교기
 *    형태다. __fls 는 최상위 1 비트의 위치를 준다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_enable_port → soc->startup → [이 함수]
 *     → syscon_regmap_lookup_by_phandle_args → regmap_write
 *       → mtk_pcie_startup_port_v2
 */
static int mtk_pcie_startup_port_an7583(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] MEM 윈도를 찾기 위한 브리지 포인터. */
	struct pci_host_bridge *host;
	/* [한국어] 찾은 윈도 항목. */
	struct resource_entry *entry;
	/* [한국어] AN7583 전용 pbus 레지스터의 regmap. */
	struct regmap *pbus_regmap;
	/* [한국어] pbus 에 알릴 기준 주소. */
	resource_size_t addr;
	/* [한국어] args 는 DT 가 지정한 두 레지스터 오프셋, size 는 창 크기. */
	u32 args[2], size;
/* [한국어] 아래에서 syscon 을 찾는다. */

	/*
	 * Configure PBus base address and base address mask to allow
	 * the hw to detect if a given address is accessible on PCIe
	 * controller.
	 */
	pbus_regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
							   "mediatek,pbus-csr",
							   ARRAY_SIZE(args),
							   args);
	if (IS_ERR(pbus_regmap))
		/* [한국어] pbus regmap 을 못 찾았다. 이 SoC 는 그것 없이 트랜잭션이 통과하지 않는다. */
		return PTR_ERR(pbus_regmap);

	host = pci_host_bridge_from_priv(pcie);
	/* [한국어] 이제 알릴 주소 범위를 찾는다. */
	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM);
	/* [한국어] MEM 윈도가 없다. */
	if (!entry)
		/* [한국어] 알릴 범위가 없으므로 진행할 수 없다. */
		return -ENODEV;

	addr = entry->res->start - entry->offset;
	/* [한국어] 기준 주소를 args[0] 이 지정한 레지스터에 쓴다. 어느 레지스터인지를
	 * DT 가 정한다. */
	regmap_write(pbus_regmap, args[0], lower_32_bits(addr));
	/* [한국어] 창 크기의 하위 32비트. */
	size = lower_32_bits(resource_size(entry->res));
	/* [한국어] GENMASK(31, __fls(size)) 는 크기의 최상위 비트부터 31까지를 1 로 만든
	 * 마스크다. '이 비트 위쪽이 일치하면 이 창' 이라는 주소 비교기 형태다. */
	regmap_write(pbus_regmap, args[1], GENMASK(31, __fls(size)));
/* [한국어] pbus 설정이 끝났으므로 나머지는 공통 v2 경로에 맡긴다. */

	return mtk_pcie_startup_port_v2(port);
}

/* [한국어]
 * mtk_pcie_enable_port - 포트의 클록·리셋·PHY 를 켜고 기동한다 (실패하면 포트를 버린다)
 *
 * @port: 대상 포트.
 * @return: 없음. **실패를 알리지 않는다** -- 그것이 이 함수의 설계다.
 *
 * 여섯 클록을 순서대로 켜고, 리셋을 토글하고, PHY 를 올린 뒤 SoC 별
 * startup 콜백을 부른다. 성공하면 그대로 반환한다.
 *
 * 실패했을 때의 처리가 이 함수의 핵심이다. 반환값이 void 인 이유가 여기
 * 있다 -- 포트 하나가 실패해도 **다른 포트는 계속 쓸 수 있어야 하므로**,
 * 실패한 포트만 조용히 정리하고 목록에서 빼 버린다. 그래서
 * mtk_pcie_setup 의 순회는 실패를 보지 않고 끝까지 돈다.
 *
 * 되감기가 goto 라벨 사슬로 되어 있어, 어느 단계에서 실패하든 그때까지 켠
 * 것만 정확히 역순으로 끈다. 마지막 mtk_pcie_port_free 가 포트를 목록에서
 * 빼고 메모리까지 반납한다.
 *
 * 링크가 서지 않은 경우(`!pcie->soc->startup(port)` 가 거짓)도 같은 경로로
 * 흘러간다 -- "Port%d link down" 로그를 남기고 포트를 버린다. 즉 슬롯이
 * 비어 있는 포트는 열거 대상에서 아예 사라진다.
 *
 * 실행 컨텍스트: 프로브 및 재개 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_setup / mtk_pcie_resume_noirq → [이 함수]
 *     → clk_prepare_enable(6회) → reset_control_assert/deassert
 *       → phy_init/phy_power_on → soc->startup
 */
static void mtk_pcie_enable_port(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] 각 클록 활성화의 결과. */
	int err;
/* [한국어] 아래 여섯 클록을 순서대로 켠다. 순서가 의존 관계를 따르므로 바꿀 수 없다. */

	err = clk_prepare_enable(port->sys_ck);
	/* [한국어] sys 클록 활성 실패. */
	if (err) {
		/* [한국어] 어느 포트의 어느 클록인지 남긴다. */
		dev_err(dev, "failed to enable sys_ck%d clock\n", port->slot);
		/* [한국어] 아직 켠 것이 없으므로 마지막 라벨로 간다. */
		goto err_sys_clk;
	}

	err = clk_prepare_enable(port->ahb_ck);
	/* [한국어] ahb 클록 실패. */
	if (err) {
		/* [한국어] 같은 형식의 로그. */
		dev_err(dev, "failed to enable ahb_ck%d\n", port->slot);
		/* [한국어] sys 클록만 되돌리면 된다. */
		goto err_ahb_clk;
	}

	err = clk_prepare_enable(port->aux_ck);
	/* [한국어] aux 클록 실패. */
	if (err) {
		/* [한국어] 같은 형식의 로그. */
		dev_err(dev, "failed to enable aux_ck%d\n", port->slot);
		/* [한국어] ahb 부터 역순으로 되돌린다. */
		goto err_aux_clk;
	}

	err = clk_prepare_enable(port->axi_ck);
	/* [한국어] axi 클록 실패. */
	if (err) {
		/* [한국어] 같은 형식의 로그. */
		dev_err(dev, "failed to enable axi_ck%d\n", port->slot);
		/* [한국어] aux 부터 역순으로. */
		goto err_axi_clk;
	}

	err = clk_prepare_enable(port->obff_ck);
	/* [한국어] obff 클록 실패. */
	if (err) {
		/* [한국어] 같은 형식의 로그. */
		dev_err(dev, "failed to enable obff_ck%d\n", port->slot);
		/* [한국어] axi 부터 역순으로. */
		goto err_obff_clk;
	}

	err = clk_prepare_enable(port->pipe_ck);
	/* [한국어] pipe 클록 실패. 가장 마지막에 켜는 클록이다. */
	if (err) {
		/* [한국어] 같은 형식의 로그. */
		dev_err(dev, "failed to enable pipe_ck%d\n", port->slot);
		/* [한국어] obff 부터 역순으로. */
		goto err_pipe_clk;
	}

	reset_control_assert(port->reset);
	reset_control_deassert(port->reset);

	err = phy_init(port->phy);
	/* [한국어] PHY 초기화 실패. */
	if (err) {
		/* [한국어] 이 시점에는 클록 여섯 개가 모두 켜져 있다. */
		dev_err(dev, "failed to initialize port%d phy\n", port->slot);
		/* [한국어] pipe 부터 전부 되돌린다. */
		goto err_phy_init;
	}

	err = phy_power_on(port->phy);
	/* [한국어] PHY 전원 인가 실패. */
	if (err) {
		/* [한국어] init 은 성공했으므로 그것부터 되돌려야 한다. */
		dev_err(dev, "failed to power on port%d phy\n", port->slot);
		/* [한국어] phy_exit 를 거쳐 클록까지 내려간다. */
		goto err_phy_on;
	}

	if (!pcie->soc->startup(port))
		/* [한국어] 기동 성공 -- 여기서만 정상 반환한다. 아래는 전부 되감기 경로다. */
		return;

	dev_info(dev, "Port%d link down\n", port->slot);
/* [한국어] 링크가 서지 않은 경우도 이 아래로 흘러가 포트가 버려진다. */

	phy_power_off(port->phy);
err_phy_on:
	phy_exit(port->phy);
err_phy_init:
	clk_disable_unprepare(port->pipe_ck);
err_pipe_clk:
	clk_disable_unprepare(port->obff_ck);
err_obff_clk:
	clk_disable_unprepare(port->axi_ck);
err_axi_clk:
	clk_disable_unprepare(port->aux_ck);
err_aux_clk:
	clk_disable_unprepare(port->ahb_ck);
err_ahb_clk:
	clk_disable_unprepare(port->sys_ck);
err_sys_clk:
	mtk_pcie_port_free(port);
}

/* [한국어]
 * mtk_pcie_parse_port - DT 에서 포트 하나의 자원을 모아 목록에 넣는다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @node: 이 포트에 해당하는 DT 노드.
 * @slot: 포트의 슬롯 번호. 자원 이름을 만드는 데 쓰인다.
 * @return: 0 성공, 음수는 각 조회의 실패값.
 *
 * 자원 이름이 전부 슬롯 번호로 만들어지는 점이 이 함수의 형태를 결정한다:
 * "port0"/"sys_ck0"/"ahb_ck0"/"pcie-rst0"/"pcie-phy0" 처럼 접미사를 붙인다.
 * 그래서 snprintf 로 이름을 만들고 조회하는 패턴이 아홉 번 반복된다.
 *
 * 필수와 선택이 나뉜다:
 *  - 필수: 포트 MMIO 창("portN"), sys_ck. 없으면 실패한다.
 *  - 선택: ahb/axi/aux/obff/pipe 클록, 리셋, PHY. 없으면 NULL 이 들어오고
 *    이후 clk_prepare_enable(NULL) 등이 아무 일도 하지 않는다. SoC 판본마다
 *    필요한 클록이 달라 이렇게 열어 둔 것이다.
 *
 * 리셋 조회의 오류 처리가 다른 것들과 다르다. **-EPROBE_DEFER 일 때만**
 * 반환하고 나머지 오류는 무시한다 -- optional 판이라 '없음' 은 NULL 이 아니라
 * 오류 포인터로 올 수 있는데, 그 경우에도 진행하겠다는 뜻이다.
 * 다만 그러면 port->reset 에 오류 포인터가 남아 이후
 * reset_control_assert(port->reset) 에 그대로 넘어간다.
 *
 * setup_irq 는 SoC 표에 있을 때만 부른다 -- v1 은 이 콜백이 없어 INTx/MSI
 * 도메인을 만들지 않는다.
 *
 * 마지막에 포트를 ports 목록 **끝에** 붙인다. 순서가 DT 순서와 같아진다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_setup → [이 함수] → devm_platform_ioremap_resource_byname
 *     → devm_clk_get / devm_clk_get_optional → soc->setup_irq
 */
static int mtk_pcie_parse_port(struct mtk_pcie *pcie,
			       struct device_node *node,
			       int slot)
{
	struct mtk_pcie_port *port;
	struct device *dev = pcie->dev;
	/* [한국어] 자원 조회에 필요한 플랫폼 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] "sys_ck0" 같은 이름을 조립할 버퍼. 20바이트면 접미사 두 자리까지 넉넉하다. */
	char name[20];
	/* [한국어] 각 조회의 결과. */
	int err;
/* [한국어] 아래에서 포트 구조체를 할당한다. */

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!port)
		/* [한국어] devm 이라 이후 자동 해제된다. */
		return -ENOMEM;

	snprintf(name, sizeof(name), "port%d", slot);
	/* [한국어] 포트별 MMIO 창. 이름이 "port0", "port1" 처럼 슬롯 번호로 만들어진다. */
	port->base = devm_platform_ioremap_resource_byname(pdev, name);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(port->base)) {
		/* [한국어] 어느 포트인지 남긴다. */
		dev_err(dev, "failed to map port%d base\n", slot);
		/* [한국어] 이 창 없이는 포트를 다룰 수 없으므로 필수다. */
		return PTR_ERR(port->base);
	}

	snprintf(name, sizeof(name), "sys_ck%d", slot);
	/* [한국어] 필수 시스템 클록. */
	port->sys_ck = devm_clk_get(dev, name);
	/* [한국어] 조회 실패. */
	if (IS_ERR(port->sys_ck)) {
		/* [한국어] 어느 포트의 클록인지 남긴다. */
		dev_err(dev, "failed to get sys_ck%d clock\n", slot);
		/* [한국어] 이것만 필수이고 아래 다섯 클록은 선택이다. */
		return PTR_ERR(port->sys_ck);
	/* [한국어] 이제 선택 클록들을 차례로 얻는다. */
	}

	/* sys_ck might be divided into the following parts in some chips */
	snprintf(name, sizeof(name), "ahb_ck%d", slot);
	port->ahb_ck = devm_clk_get_optional(dev, name);
	/* [한국어] _optional 판이라 '없음' 은 NULL 이고 오류가 아니다. 여기 걸리는 것은
	 * DT 표기 오류뿐이다. */
	if (IS_ERR(port->ahb_ck))
		/* [한국어] 로그 없이 바로 반환한다 -- 아래 네 클록도 같은 형태다. */
		return PTR_ERR(port->ahb_ck);
/* [한국어] 다음 클록으로. */

	snprintf(name, sizeof(name), "axi_ck%d", slot);
	/* [한국어] AXI 버스 클록. */
	port->axi_ck = devm_clk_get_optional(dev, name);
	/* [한국어] 표기 오류만 걸린다. */
	if (IS_ERR(port->axi_ck))
		/* [한국어] 실패값을 그대로 올린다. */
		return PTR_ERR(port->axi_ck);
/* [한국어] 다음 클록으로. */

	snprintf(name, sizeof(name), "aux_ck%d", slot);
	/* [한국어] 보조 클록. */
	port->aux_ck = devm_clk_get_optional(dev, name);
	/* [한국어] 표기 오류만 걸린다. */
	if (IS_ERR(port->aux_ck))
		/* [한국어] 실패값을 그대로 올린다. */
		return PTR_ERR(port->aux_ck);
/* [한국어] 다음 클록으로. */

	snprintf(name, sizeof(name), "obff_ck%d", slot);
	/* [한국어] OBFF 클록. */
	port->obff_ck = devm_clk_get_optional(dev, name);
	/* [한국어] 표기 오류만 걸린다. */
	if (IS_ERR(port->obff_ck))
		/* [한국어] 실패값을 그대로 올린다. */
		return PTR_ERR(port->obff_ck);
/* [한국어] 다음 클록으로. */

	snprintf(name, sizeof(name), "pipe_ck%d", slot);
	/* [한국어] PIPE 인터페이스 클록. */
	port->pipe_ck = devm_clk_get_optional(dev, name);
	/* [한국어] 표기 오류만 걸린다. */
	if (IS_ERR(port->pipe_ck))
		/* [한국어] 실패값을 그대로 올린다. */
		return PTR_ERR(port->pipe_ck);
/* [한국어] 이제 리셋과 PHY 를 얻는다. */

	snprintf(name, sizeof(name), "pcie-rst%d", slot);
	/* [한국어] optional exclusive 로 얻는다. */
	port->reset = devm_reset_control_get_optional_exclusive(dev, name);
	/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): **-EPROBE_DEFER 일 때만** 반환하고
	 * 다른 오류는 무시한다. 그러면 port->reset 에 오류 포인터가 남은 채
	 * reset_control_assert(port->reset) 에 그대로 넘어간다. */
	if (PTR_ERR(port->reset) == -EPROBE_DEFER)
		/* [한국어] defer 는 재시도해야 하므로 올린다. */
		return PTR_ERR(port->reset);
/* [한국어] 다음은 PHY 다. */

	/* some platforms may use default PHY setting */
	snprintf(name, sizeof(name), "pcie-phy%d", slot);
	port->phy = devm_phy_optional_get(dev, name);
	/* [한국어] PHY 조회 실패. */
	if (IS_ERR(port->phy))
		/* [한국어] optional 이라 '없음' 은 NULL 이고, 여기 걸리는 것은 표기 오류다. */
		return PTR_ERR(port->phy);
/* [한국어] 모든 자원이 확보됐다. */

	port->slot = slot;
	/* [한국어] 부모 컨트롤러로 돌아가는 포인터를 채운다. 아래 setup_irq 가 이것을 쓴다. */
	port->pcie = pcie;
/* [한국어] IRQ 설정은 SoC 표에 콜백이 있을 때만 한다. */

	if (pcie->soc->setup_irq) {
		/* [한국어] v1 은 이 콜백이 NULL 이라 INTx/MSI 도메인을 만들지 않는다. */
		err = pcie->soc->setup_irq(port, node);
		/* [한국어] IRQ 설정 실패. */
		if (err)
			/* [한국어] 포트를 목록에 넣기 전이라 되감을 것이 없다. */
			return err;
	}

	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);
/* [한국어] 목록 **끝에** 붙여 DT 순서를 유지한다. */

	return 0;
}

/* [한국어]
 * mtk_pcie_subsys_powerup - 컨트롤러 전체가 공유하는 창·regmap·클록을 준비한다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 0 성공, 음수는 매핑/조회/클록 실패값.
 *
 * 포트별이 아니라 **서브시스템 공용** 자원을 다룬다. 세 가지가 모두 선택인
 * 점이 특징이다 -- 바인딩 세대에 따라 있는 것이 다르기 때문이다.
 *
 *  1. "subsys" MMIO 창: 있으면 매핑해 pcie->base 에 둔다. v1 은 이 창으로
 *     모든 설정 접근을 하고, v2 는 LTSSM 스위치에만 쓴다. **없으면
 *     pcie->base 가 NULL 로 남고**, startup_port_v2 가 그 경우 regmap 쪽을
 *     쓴다.
 *  2. "mediatek,generic-pciecfg" syscon: 있으면 regmap 으로 잡아 pcie->cfg
 *     에 둔다. 새 바인딩이 LTSSM 스위치를 여기 두는 방식이다.
 *     of_find_compatible_node 로 **트리 전체에서** 찾는 점에 유의 -- 이
 *     컨트롤러의 자식이 아니라 별개의 syscon 노드다. 참조 카운트를
 *     of_node_put 으로 바로 놓는다.
 *  3. "free_ck" 클록: 없으면 **NULL 로 만들고 계속 진행한다.**
 *     -EPROBE_DEFER 만 실패로 올린다. 이후 clk_prepare_enable(NULL) 은
 *     아무 일도 하지 않으므로 그대로 동작한다.
 *
 * 런타임 PM 을 enable + get_sync 로 켜는 것이 클록 활성보다 앞에 온다 --
 * 전원 도메인이 살아 있어야 클록을 켤 수 있기 때문이다. 실패 경로도 그
 * 역순으로 되돌린다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_setup → [이 함수] → devm_ioremap_resource
 *     → syscon_node_to_regmap → pm_runtime_enable → clk_prepare_enable
 */
static int mtk_pcie_subsys_powerup(struct mtk_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 'subsys' 자원 서술자. */
	struct resource *regs;
	/* [한국어] syscon 노드를 담을 포인터. */
	struct device_node *cfg_node;
	/* [한국어] 클록 활성화 결과. */
	int err;
/* [한국어] 아래 세 자원이 모두 선택이다 -- 바인딩 세대마다 있는 것이 다르다. */

	/* get shared registers, which are optional */
	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "subsys");
	if (regs) {
		/* [한국어] 있으면 매핑한다. 없으면 pcie->base 가 NULL 로 남고 아래 cfg 를 쓴다. */
		pcie->base = devm_ioremap_resource(dev, regs);
		/* [한국어] 매핑 실패. */
		if (IS_ERR(pcie->base))
			/* [한국어] 자원이 선언됐는데 실패한 것이므로 오류다. */
			return PTR_ERR(pcie->base);
	/* [한국어] 다음 자원으로. */
	}

	cfg_node = of_find_compatible_node(NULL, NULL,
					   /* [한국어] **트리 전체에서** 찾는다 -- 이 컨트롤러의 자식이 아니라 별개의 syscon 노드다. */
					   "mediatek,generic-pciecfg");
	if (cfg_node) {
		/* [한국어] regmap 으로 변환한다. */
		pcie->cfg = syscon_node_to_regmap(cfg_node);
		/* [한국어] 노드 참조를 곧바로 놓는다. regmap 이 따로 붙들기 때문에 안전하다. */
		of_node_put(cfg_node);
		if (IS_ERR(pcie->cfg))
			/* [한국어] 노드는 있는데 regmap 변환에 실패한 것이므로 오류다. */
			return PTR_ERR(pcie->cfg);
	/* [한국어] 다음 자원으로. */
	}

	pcie->free_ck = devm_clk_get(dev, "free_ck");
	/* [한국어] 기준 클록 조회 실패. */
	if (IS_ERR(pcie->free_ck)) {
		/* [한국어] defer 만 재시도 대상이다. */
		if (PTR_ERR(pcie->free_ck) == -EPROBE_DEFER)
			/* [한국어] 클록 공급자가 아직 올라오지 않았다. */
			return -EPROBE_DEFER;
/* [한국어] 그 외의 오류는 무시한다. */

		pcie->free_ck = NULL;
	/* [한국어] NULL 로 만들고 계속 진행한다. 이후 clk_prepare_enable(NULL) 은 아무
	 * 일도 하지 않으므로 그대로 동작한다. */
	}

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* enable top level clock */
	err = clk_prepare_enable(pcie->free_ck);
	if (err) {
		/* [한국어] 기준 클록 활성 실패. */
		dev_err(dev, "failed to enable free_ck\n");
		/* [한국어] 런타임 PM 을 되돌리는 라벨로 간다. */
		goto err_free_ck;
	}

	return 0;

err_free_ck:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);

	return err;
}

/* [한국어]
 * mtk_pcie_setup - DT 를 읽어 포트를 만들고 전부 기동한다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 0 성공, 음수는 포트 파싱 또는 서브시스템 준비 실패값.
 *
 * DT 구조가 두 가지라 앞머리에서 갈린다:
 *  - of_get_pci_domain_nr 이 실패(음수)하면 **자식 노드마다 포트가 하나씩**
 *    있는 구조다. 각 자식의 devfn 을 읽어 슬롯 번호를 얻고 파싱한다.
 *  - 성공하면 컨트롤러 노드 **하나가 포트 하나**인 구조이고, 도메인 번호가
 *    곧 슬롯 번호다. 이 경우 포트가 하나뿐이다.
 *
 * 포트를 모두 파싱한 **뒤에** 서브시스템을 준비하는 순서가 요점이다.
 * 파싱이 -EPROBE_DEFER 로 실패할 수 있는데, 그 전에 런타임 PM 을 켜 두면
 * 재시도 때마다 참조가 새기 때문이다.
 *
 * 기동 순회에 list_for_each_entry_safe 를 쓰는 이유가 중요하다.
 * mtk_pcie_enable_port 는 실패한 포트를 **목록에서 빼고 해제**하므로,
 * 다음 항목 포인터를 미리 확보해 두지 않으면 해제된 메모리를 따라간다.
 *
 * 마지막의 list_empty 검사가 그 결과를 받는다 -- 모든 포트가 실패해 목록이
 * 비었으면 서브시스템 자원을 되돌린다. 그래도 **0(성공)을 반환한다** --
 * 포트가 하나도 없어도 프로브 자체는 성공으로 끝나고, mtk_pcie_probe 가
 * 그 상태로 pci_host_probe 를 진행한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_probe → [이 함수] → mtk_pcie_parse_port
 *     → mtk_pcie_subsys_powerup → mtk_pcie_enable_port
 */
static int mtk_pcie_setup(struct mtk_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *node = dev->of_node;
	/* [한국어] 목록 순회용 포인터 쌍. enable_port 가 항목을 제거하므로 tmp 가 필요하다. */
	struct mtk_pcie_port *port, *tmp;
	/* [한국어] err 는 각 단계의 결과, slot 은 포트 슬롯 번호. */
	int err, slot;
/* [한국어] 아래에서 DT 구조를 판별한다. */

	slot = of_get_pci_domain_nr(dev->of_node);
	/* [한국어] 도메인 번호가 없다 = 자식 노드마다 포트가 하나씩 있는 구조다. */
	if (slot < 0) {
		/* [한국어] _scoped 판을 쓰면 루프를 빠져나갈 때 노드 참조가 자동으로 놓인다. */
		for_each_available_child_of_node_scoped(node, child) {
			/* [한국어] 자식의 devfn 을 읽는다. */
			err = of_pci_get_devfn(child);
			/* [한국어] DT 표기 오류. */
			if (err < 0)
				/* [한국어] dev_err_probe 로 defer 시 조용히 물러난다. */
				return dev_err_probe(dev, err, "failed to get devfn\n");

			slot = PCI_SLOT(err);
/* [한국어] devfn 에서 슬롯 번호를 꺼낸다. */

			err = mtk_pcie_parse_port(pcie, child, slot);
			/* [한국어] 포트 파싱 실패. */
			if (err)
				/* [한국어] 이미 만든 포트는 devm 이 정리한다. */
				return err;
		}
	} else {
		err = mtk_pcie_parse_port(pcie, node, slot);
		/* [한국어] 컨트롤러 노드 하나가 포트 하나인 구조 -- 포트가 하나뿐이다. */
		if (err)
			/* [한국어] 파싱 실패. */
			return err;
	}

	err = mtk_pcie_subsys_powerup(pcie);
	/* [한국어] **포트를 모두 파싱한 뒤에** 서브시스템을 준비한다. 파싱이 defer 로
	 * 실패할 수 있는데, 그 전에 런타임 PM 을 켜면 재시도마다 참조가 샌다. */
	if (err)
		/* [한국어] 서브시스템 준비 실패. */
		return err;

	/* enable each port, and then check link status */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list)
		mtk_pcie_enable_port(port);

	/* power down PCIe subsys if slots are all empty (link down) */
	if (list_empty(&pcie->ports))
		mtk_pcie_subsys_powerdown(pcie);

	return 0;
}

/* [한국어]
 * mtk_pcie_probe - MediaTek PCIe 드라이버 진입점
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수는 실패값.
 *
 * devm_pci_alloc_host_bridge 에 sizeof(*pcie) 를 넘겨 **브리지 뒤에 드라이버
 * 인스턴스를 함께 할당**하는 점이 특징이다. 그래서 pci_host_bridge_priv 로
 * 인스턴스를 얻고, 반대로 pci_host_bridge_from_priv 로 브리지를 되찾는다 --
 * 두 객체의 수명이 하나로 묶인다.
 *
 * 단계:
 *  1. 브리지 + 인스턴스 할당.
 *  2. dev / soc(compatible 별 설정표) / drvdata / ports 목록 초기화.
 *  3. mtk_pcie_setup 이 포트를 만들고 기동한다.
 *  4. 브리지에 ops 와 sysdata 를 건다. **ops 가 SoC 표에서 온다** --
 *     v1 은 map_bus 방식, v2 는 read/write 직접 구현이다.
 *  5. host->msi_domain 을 설정한다. 값이 `!!(quirks & MTK_PCIE_NO_MSI)` 인
 *     것이 언뜻 뒤집힌 듯 보이지만 그렇지 않다 -- PCI 코어는 이 플래그가
 *     참인데 실제 MSI 도메인을 찾지 못하면 PCI_BUS_FLAGS_NO_MSI 를 세운다
 *     (drivers/pci/probe.c:2637). v1 은 setup_irq 가 없어 도메인을 만들지
 *     않으므로, 이 플래그를 세우는 것이 곧 "이 버스에서 MSI 를 끄라" 는
 *     지시가 된다. v2 는 거짓이고 자기 도메인을 만든다.
 *  6. pci_host_probe 로 열거.
 *
 * 실패 시 put_resources 라벨은 **포트 목록이 비어 있지 않을 때만** 정리한다.
 * 비어 있으면 mtk_pcie_setup 이 이미 서브시스템을 되돌린 뒤이기 때문이다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → devm_pci_alloc_host_bridge → mtk_pcie_setup
 *     → pci_host_probe
 */
static int mtk_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_pcie *pcie;
	/* [한국어] 브리지와 인스턴스를 함께 할당받을 포인터. */
	struct pci_host_bridge *host;
	/* [한국어] 각 단계의 결과. */
	int err;
/* [한국어] 아래에서 브리지 뒤에 인스턴스를 붙여 할당한다. */

	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 할당 실패. */
	if (!host)
		/* [한국어] devm 이라 이후 자동 해제된다. */
		return -ENOMEM;

	pcie = pci_host_bridge_priv(host);
/* [한국어] 브리지 뒤에 붙은 인스턴스의 주소를 꺼낸다. 반대 변환은
 * pci_host_bridge_from_priv 다. */

	pcie->dev = dev;
	/* [한국어] compatible 에 매인 SoC 설정표. NULL 검사가 없는 것은 of_match_table 로
	 * 매칭됐다면 반드시 있다는 전제다. */
	pcie->soc = of_device_get_match_data(dev);
	/* [한국어] 제거·전원관리 콜백이 dev_get_drvdata 로 인스턴스를 되찾는다. */
	platform_set_drvdata(pdev, pcie);
	/* [한국어] 포트 목록을 초기화한다. parse_port 가 여기에 붙인다. */
	INIT_LIST_HEAD(&pcie->ports);

	err = mtk_pcie_setup(pcie);
	/* [한국어] 포트 파싱과 기동 실패. */
	if (err)
		/* [한국어] setup 이 자기 실패를 스스로 정리했으므로 라벨을 타지 않는다. */
		return err;

	host->ops = pcie->soc->ops;
	/* [한국어] 설정 접근 함수들이 bus->sysdata 로 이것을 되찾는다. */
	host->sysdata = pcie;
	/* [한국어] 값이 `!!(quirks & MTK_PCIE_NO_MSI)` 인 것이 언뜻 뒤집힌 듯 보이지만
	 * 그렇지 않다. PCI 코어는 이 플래그가 참인데 실제 MSI 도메인을 찾지 못하면
	 * PCI_BUS_FLAGS_NO_MSI 를 세운다(drivers/pci/probe.c:2637). v1 은 setup_irq
	 * 가 없어 도메인을 만들지 않으므로, 이 플래그가 곧 'MSI 를 끄라' 는 지시가
	 * 된다. v2 는 거짓이고 자기 도메인을 만든다. */
	host->msi_domain = !!(pcie->soc->quirks & MTK_PCIE_NO_MSI);

	err = pci_host_probe(host);
	/* [한국어] 여기서 열거가 일어난다. */
	if (err)
		/* [한국어] 열거 실패 -- 이제 되감을 것이 있다. */
		goto put_resources;

	return 0;

put_resources:
	if (!list_empty(&pcie->ports))
		mtk_pcie_put_resources(pcie);

	return err;
}


/* [한국어]
 * mtk_pcie_free_resources - 브리지의 자원 윈도 목록을 해제한다
 *
 * @pcie: 이 컨트롤러 인스턴스.
 * @return: 없음.
 *
 * pci_host_bridge_from_priv 로 브리지를 되찾는다 -- probe 가 둘을 함께
 * 할당했기 때문에 가능한 변환이다.
 *
 * pci_free_resource_list 는 DT 의 ranges 를 파싱해 만들어진 윈도 목록을
 * 비운다. 브리지 자체는 devm 이라 자동 해제되지만, 이 목록은 그렇지 않다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   mtk_pcie_remove → [이 함수] → pci_free_resource_list
 */
static void mtk_pcie_free_resources(struct mtk_pcie *pcie)
{
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	struct list_head *windows = &host->windows;
/* [한국어] 이 아래는 제거·전원관리 경로다. */

	pci_free_resource_list(windows);
}

/* [한국어]
 * mtk_pcie_remove - 버스를 걷어내고 인터럽트·클록·PHY 를 모두 되돌린다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음.
 *
 * 순서가 전부다:
 *  1. pci_stop_root_bus / pci_remove_root_bus -- 하위 장치 드라이버를 떼고
 *     버스 객체를 없앤다. **가장 먼저** 해야 이후 하드웨어를 꺼도 살아
 *     있는 드라이버가 접근하지 않는다.
 *  2. 자원 윈도 목록 해제.
 *  3. IRQ 도메인과 연쇄 핸들러 정리.
 *  4. 포트별 PHY/클록과 서브시스템 자원 해제.
 *
 * 다만 이 드라이버는 suppress_bind_attrs = true 라 sysfs 로 언바인드할 수
 * 없다. 그래서 이 경로는 모듈 제거 시에만 도달한다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → pci_stop_root_bus → pci_remove_root_bus
 *     → mtk_pcie_free_resources → mtk_pcie_irq_teardown
 *       → mtk_pcie_put_resources
 */
static void mtk_pcie_remove(struct platform_device *pdev)
{
	struct mtk_pcie *pcie = platform_get_drvdata(pdev);
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
/* [한국어] 버스를 먼저 걷어낸 뒤 하드웨어를 끄는 순서가 이 함수의 전부다. */

	pci_stop_root_bus(host->bus);
	pci_remove_root_bus(host->bus);
	mtk_pcie_free_resources(pcie);

	mtk_pcie_irq_teardown(pcie);

	mtk_pcie_put_resources(pcie);
}

/* [한국어]
 * mtk_pcie_suspend_noirq - 모든 포트의 클록과 PHY 를 끈다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * 포트가 하나도 없으면 끌 것이 없으므로 곧바로 0 을 돌려준다.
 *
 * 포트마다 클록 여섯 개와 PHY 를 끄는데, **enable_port 가 켠 역순**이다.
 * 그 뒤 공용 free_ck 까지 끈다.
 *
 * 링크를 L2 로 내리거나 PERST# 를 거는 절차가 없는 점에 유의 -- 이
 * 드라이버는 재개 시 mtk_pcie_enable_port 로 포트를 처음부터 다시 세우므로,
 * 서스펜드 쪽에서는 전력만 끊는다.
 *
 * 반환값이 항상 0 이라 서스펜드가 이 단계에서 실패하는 일은 없다.
 *
 * 실행 컨텍스트: 시스템 서스펜드의 noirq 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq → [이 함수]
 *     → clk_disable_unprepare → phy_power_off → phy_exit
 */
static int mtk_pcie_suspend_noirq(struct device *dev)
{
	struct mtk_pcie *pcie = dev_get_drvdata(dev);
	struct mtk_pcie_port *port;
/* [한국어] 포트가 하나도 없으면 끌 것이 없다. */

	if (list_empty(&pcie->ports))
		/* [한국어] 클록을 켜지 않았으므로 그대로 성공으로 끝낸다. */
		return 0;

	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 켠 역순으로 끈다 -- pipe 가 가장 나중에 켠 클록이다. */
		clk_disable_unprepare(port->pipe_ck);
		clk_disable_unprepare(port->obff_ck);
		clk_disable_unprepare(port->axi_ck);
		clk_disable_unprepare(port->aux_ck);
		clk_disable_unprepare(port->ahb_ck);
		clk_disable_unprepare(port->sys_ck);
		phy_power_off(port->phy);
		phy_exit(port->phy);
	}

	clk_disable_unprepare(pcie->free_ck);

	return 0;
}

/* [한국어]
 * mtk_pcie_resume_noirq - 공용 클록을 켜고 모든 포트를 다시 기동한다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * 서스펜드에서 클록과 PHY 를 껐으므로, 재개는 포트를 처음부터 다시 세운다 --
 * 그래서 프로브와 같은 mtk_pcie_enable_port 를 그대로 쓴다.
 *
 * 앞뒤로 list_empty 를 **두 번** 검사하는 것이 요점이다:
 *  - 앞: 서스펜드 전에 이미 포트가 없었으면 할 일이 없다.
 *  - 뒤: enable_port 가 실패한 포트를 목록에서 빼므로, 재개 도중 모든
 *    포트가 사라졌을 수 있다. 그러면 방금 켠 free_ck 를 다시 끈다.
 * 두 번째 검사가 없으면 쓸 포트가 하나도 없는데 공용 클록만 켜진 채 남는다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): clk_prepare_enable(pcie->free_ck)
 * 의 반환값을 검사하지 않는다. suspend 쪽과 마찬가지로 이 콜백은 항상
 * 0 을 돌려주므로, 재개 실패가 상위에 보고되지 않는다.
 *
 * 실행 컨텍스트: 시스템 재개의 noirq 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq → [이 함수]
 *     → clk_prepare_enable → mtk_pcie_enable_port
 */
static int mtk_pcie_resume_noirq(struct device *dev)
{
	struct mtk_pcie *pcie = dev_get_drvdata(dev);
	struct mtk_pcie_port *port, *tmp;
/* [한국어] 서스펜드 전에 이미 포트가 없었으면 할 일이 없다. */

	if (list_empty(&pcie->ports))
		/* [한국어] 이 경우 free_ck 도 켜지 않은 상태다. */
		return 0;

	clk_prepare_enable(pcie->free_ck);

	list_for_each_entry_safe(port, tmp, &pcie->ports, list)
		/* [한국어] 프로브와 같은 함수로 포트를 처음부터 다시 세운다. 실패한 포트는
		 * 이 함수가 목록에서 빼 버린다. */
		mtk_pcie_enable_port(port);

	/* In case of EP was removed while system suspend. */
	if (list_empty(&pcie->ports))
		clk_disable_unprepare(pcie->free_ck);

	return 0;
}

static const struct dev_pm_ops mtk_pcie_pm_ops = {
	/* [한국어] noirq 단계에만 콜백을 등록한다. 클록과 PHY 를 다루므로 인터럽트가
	 * 꺼진 뒤여야 한다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_pcie_suspend_noirq,
				  mtk_pcie_resume_noirq)
};

static const struct mtk_pcie_soc mtk_pcie_soc_v1 = {
	/* [한국어] v1 은 map_bus 방식의 ops 를 쓴다. */
	.ops = &mtk_pcie_ops,
	/* [한국어] v1 전용 기동 함수. setup_irq 가 없어 INTx/MSI 도메인을 만들지 않는다. */
	.startup = mtk_pcie_startup_port,
	.quirks = MTK_PCIE_NO_MSI,
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt2712 = {
	/* [한국어] MT2712 는 v2 ops. */
	.ops = &mtk_pcie_ops_v2,
	/* [한국어] 공통 v2 기동 경로. 쿼크가 없어 클래스/디바이스 ID 를 손대지 않는다. */
	.startup = mtk_pcie_startup_port_v2,
	.setup_irq = mtk_pcie_setup_irq,
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt7622 = {
	/* [한국어] MT7622 도 v2 ops. */
	.ops = &mtk_pcie_ops_v2,
	/* [한국어] 같은 기동 경로에 FIX_CLASS_ID 쿼크만 더한다. */
	.startup = mtk_pcie_startup_port_v2,
	.setup_irq = mtk_pcie_setup_irq,
	.quirks = MTK_PCIE_FIX_CLASS_ID,
};

static const struct mtk_pcie_soc mtk_pcie_soc_an7583 = {
	/* [한국어] AN7583 도 v2 ops. */
	.ops = &mtk_pcie_ops_v2,
	/* [한국어] pbus 창을 먼저 잡는 전용 기동 함수를 쓴다. */
	.startup = mtk_pcie_startup_port_an7583,
	.setup_irq = mtk_pcie_setup_irq,
	.quirks = MTK_PCIE_FIX_CLASS_ID | MTK_PCIE_SKIP_RSTB,
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt7629 = {
	/* [한국어] MT7629 만 device_id 를 채운다 -- FIX_DEVICE_ID 쿼크가 이 값을 쓴다. */
	.device_id = PCI_DEVICE_ID_MEDIATEK_7629,
	/* [한국어] 나머지는 다른 v2 SoC 와 같다. */
	.ops = &mtk_pcie_ops_v2,
	.startup = mtk_pcie_startup_port_v2,
	.setup_irq = mtk_pcie_setup_irq,
	.quirks = MTK_PCIE_FIX_CLASS_ID | MTK_PCIE_FIX_DEVICE_ID,
};

static const struct of_device_id mtk_pcie_ids[] = {
	/* [한국어] Airoha AN7583. MediaTek 계열이 아니지만 같은 IP 를 쓴다. */
	{ .compatible = "airoha,an7583-pcie", .data = &mtk_pcie_soc_an7583 },
	/* [한국어] MT2701 과 아래 MT7623 이 v1 하드웨어다. */
	{ .compatible = "mediatek,mt2701-pcie", .data = &mtk_pcie_soc_v1 },
	{ .compatible = "mediatek,mt7623-pcie", .data = &mtk_pcie_soc_v1 },
	{ .compatible = "mediatek,mt2712-pcie", .data = &mtk_pcie_soc_mt2712 },
	{ .compatible = "mediatek,mt7622-pcie", .data = &mtk_pcie_soc_mt7622 },
	{ .compatible = "mediatek,mt7629-pcie", .data = &mtk_pcie_soc_mt7629 },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_pcie_ids);

static struct platform_driver mtk_pcie_driver = {
	/* [한국어] 프로브 진입점. */
	.probe = mtk_pcie_probe,
	/* [한국어] 제거 진입점. 다만 suppress_bind_attrs 로 sysfs 언바인드는 막혀 있다. */
	.remove = mtk_pcie_remove,
	.driver = {
		.name = "mtk-pcie",
		/* [한국어] 위 표를 걸어 매칭되면 probe 가 불린다. */
		.of_match_table = mtk_pcie_ids,
		.suppress_bind_attrs = true,
		.pm = &mtk_pcie_pm_ops,
	},
};
module_platform_driver(mtk_pcie_driver);
MODULE_DESCRIPTION("MediaTek PCIe host controller driver");
MODULE_LICENSE("GPL v2");
