// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Kirin Phone SoCs
 *
 * Copyright (C) 2017 HiSilicon Electronics Co., Ltd.
 *		https://www.huawei.com
 *
 * Author: Xiaowei Song <songxiaowei@huawei.com>
 */

/*
 * [한국어 설명] HiSilicon Kirin 960/970 스마트폰 SoC 의 DesignWare PCIe 호스트
 * 글루 — 한 파일에서 두 세대를 함께 지원한다 (pcie-kirin.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare(DWC) PCIe 코어를 HiSilicon Kirin 계열 SoC 에 붙이는
 * 글루 드라이버다. 설정공간 열거, iATU, MSI, 버스 스캔은 모두
 * pcie-designware-host.c 가 하고, 이 파일은 그 코어가 알 수 없는 SoC 고유의
 * 것만 맡는다. RC(호스트) 전용이며 엔드포인트 모드는 없다.
 *
 * 이 파일이 맡는 것이 셋이다. 첫째, DBI 접근의 사이드밴드 처리다 — 이
 * 하드웨어는 DBI 창을 그냥 읽고 쓸 수 없고, 접근 전후로 ELBI 레지스터의
 * 인에이블 비트를 켰다 꺼야 한다. 그래서 read_dbi/write_dbi 훅을 걸고,
 * 루트 버스의 설정공간 접근도 DWC 기본 경로 대신 이 파일의 pci_ops 로
 * 갈아 끼운다. 둘째, PHY 관리다. 셋째, 여러 슬롯의 PERST# 와 clkreq GPIO
 * 관리다.
 *
 * 두 세대를 함께 지원하는 방식이 이 파일을 읽는 축이다. 갈림은 DT
 * compatible 에 딸린 kirin_pcie_data.phy_type 하나로만 표현되며, 값은 둘뿐이다.
 *   - PCIE_KIRIN_INTERNAL_PHY (Kirin 960): PHY 를 이 파일이 직접 다룬다.
 *     hi3660_ 으로 시작하는 함수 여덟 개가 그것이며, 클럭 다섯 개와 syscon
 *     레지스터 두 벌(crgctrl/sysctrl)을 손수 조작한다. 파일 중간의 상류
 *     주석이 "DT 스키마를 바꾸지 않고는 PHY 드라이버로 분리할 수 없다" 고
 *     그 사정을 적어 두었다.
 *   - PCIE_KIRIN_EXTERNAL_PHY (Kirin 970): 별도의 PHY 드라이버에 맡긴다.
 *     devm_of_phy_get() 으로 얻어 phy_init()/phy_power_on() 을 부르는 것이
 *     전부다.
 *
 * 그래서 이 파일은 사실상 두 덩어리다. 앞쪽 절반(hi3660_ 계열)은 Kirin 960
 * 전용 PHY 코드이고, 상류 주석 "The non-PHY part starts here" 뒤부터가 두
 * 세대가 함께 쓰는 본체다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → kirin_pcie_probe()
 *     → of_device_get_match_data()  : 960/970 갈림을 phy_type 으로 받는다
 *     → kirin_pcie_get_resource()   : "apb" regmap, PERST# GPIO, clkreq GPIO,
 *                                     DT 자식 노드의 슬롯별 PERST# GPIO
 *     → platform_set_drvdata()      : to_kirin_pcie 매크로가 쓸 연결고리를 심는다
 *     → kirin_pcie_power_on()       : phy_type 에 따라 내장 PHY 절차 또는
 *                                     외부 PHY 드라이버 호출, 그다음 PERST# 조작
 *     → dw_pcie_host_init()
 *          → (콜백) kirin_pcie_host_init() : 루트 버스 pci_ops 를 갈아 끼운다
 *          → dw_pcie_setup_rc() → dw_pcie_start_link()
 *             → (콜백) kirin_pcie_start_link() : LTSSM 비트를 쓴다
 *          → 버스 스캔 → (pci_ops.add_bus) kirin_pcie_add_bus()
 *                        : 슬롯마다 PERST# 를 내보내고 10ms 기다린다
 *
 * 설정공간 접근 경로가 이 드라이버의 특징이다. DWC 기본 경로는
 * dw_pcie_own_conf_map_bus() 가 돌려준 DBI 주소에 pci_generic_config_read/write
 * 가 생 MMIO 접근을 한다. 그런데 이 하드웨어는 사이드밴드 인에이블 없이는
 * DBI 를 읽을 수 없으므로, map_bus 방식으로는 그 켜고 끄기를 끼워 넣을 자리가
 * 없다. 그래서 kirin_pci_ops 는 map_bus 를 두지 않고 .read/.write 를 직접
 * 구현해 dw_pcie_read_dbi()/write_dbi() 를 거치게 하고, 그것이 다시 이 파일의
 * read_dbi/write_dbi 훅으로 내려와 사이드밴드를 켰다 끈다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 인터럽트 핸들러가 하나도 없다 —
 * MSI 와 INTx 는 DWC 코어가 다룬다. usleep_range() 와 GPIO 조작이 있어
 * 잠들 수 있는 경로가 많다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. dw_pcie_host_init() 이 버스를 스캔하고, 스캔 중
 *   kirin_pcie_add_bus() 가 불려 슬롯 PERST# 가 나간다.
 * 아래쪽: pcie-designware.c 와 pcie-designware-host.c. 접점은 두 벌의 콜백
 *   표다 — dw_pcie_ops(read_dbi/write_dbi/link_up/start_link)와
 *   dw_pcie_host_ops(init 하나). 여기에 PCI 코어 쪽 pci_ops 한 벌
 *   (kirin_pci_ops)이 더 있는 것이 이 드라이버의 특징이다.
 * 옆쪽: Kirin 970 경로에서 generic PHY 프레임워크(drivers/phy)를 쓴다. 그
 *   드라이버는 이 트리에 없어 확인 못 함. Kirin 960 경로에서는 syscon
 *   regmap 으로 crgctrl/sysctrl 블록을 만지는데, 그 syscon 드라이버(drivers/mfd)
 *   도 이 트리에 없다.
 *
 * 데이터 흐름:
 *   DT compatible → kirin_pcie_data.phy_type → kirin_pcie.type
 *     → power_on/power_off 의 갈림 하나를 결정한다.
 *   DT "apb" reg → regmap → 링크 상태 읽기, LTSSM 쓰기, 사이드밴드 켜고 끄기.
 *   DT 자식 노드(포트 → 슬롯) → id_reset_gpio[] → add_bus 가 PERST# 로 내보낸다.
 *   설정공간 접근 → kirin_pci_ops → dw_pcie_read_dbi → kirin_pcie_read_dbi
 *     → 사이드밴드 on → dw_pcie_read → 사이드밴드 off.
 *
 * 공유 상태: struct kirin_pcie 하나와, 960 경로에서만 쓰는
 *   struct hi3660_pcie_phy 하나. 둘 다 probe 이후 사실상 불변이며 락이 없다.
 *   다만 사이드밴드 켜고 끄기는 설정공간 접근마다 일어나는 읽기-수정-쓰기인데,
 *   이 파일에 그것을 보호하는 잠금은 없다 — 상위 PCI 코어가 설정공간 접근을
 *   직렬화한다는 전제로 보이나, 그 전제가 이 파일에 적혀 있지는 않다.
 *
 * === 주요 함수/구조체 요약 ===
 * kirin_pcie_probe()            : 진입점. 두 세대 갈림이 여기서 시작된다.
 * kirin_pcie_get_resource()     : "apb" regmap 과 GPIO 들을 얻고 DT 자식을 훑는다.
 * kirin_pcie_parse_port()       : 슬롯마다 PERST# GPIO 를 얻어 이름을 붙인다.
 * kirin_pcie_power_on()         : phy_type 에 따라 내장/외부 PHY 를 켜고 PERST# 를 낸다.
 * kirin_pcie_read_dbi()/write_dbi() : 사이드밴드를 켜고 DBI 에 접근한 뒤 끈다.
 * kirin_pcie_rd_own_conf()/wr_own_conf() : 루트 버스 설정공간을 DBI 로 돌린다.
 * kirin_pcie_add_bus()          : 버스 생성 시 슬롯마다 PERST# 를 내보낸다.
 * hi3660_pcie_phy_power_on()    : Kirin 960 내장 PHY 를 켜는 전체 절차.
 * hi3660_pcie_phy_clk_ctrl()    : 클럭 다섯 개를 순서대로 켜고 끈다.
 * struct kirin_pcie             : 이 드라이버의 상태. 두 세대가 공유한다.
 * struct hi3660_pcie_phy        : Kirin 960 내장 PHY 전용 상태.
 *
 * === 960 과 970 이 나뉘는 지점 ===
 * 갈림이 코드에 나타나는 곳은 정확히 두 군데뿐이다 — kirin_pcie_power_on()
 * 과 kirin_pcie_power_off() 의 `if (type == PCIE_KIRIN_INTERNAL_PHY)`.
 * 그 밖의 모든 것(사이드밴드 DBI, 링크 상태 판정, LTSSM, 슬롯 PERST#,
 * pci_ops 갈아 끼우기)은 두 세대가 똑같이 쓴다.
 *
 * 갈리는 내용:
 *   - 960(내장): 이 파일의 hi3660_ 함수들이 클럭 다섯(phy_ref/aux/apb_phy/
 *     apb_sys/aclk)과 syscon 두 벌을 직접 다루고, PHY 레지스터를 "phy" 창에
 *     ioremap 해 만진다. 클럭 소스는 100MHz 로 직접 설정한다.
 *   - 970(외부): devm_of_phy_get() 으로 얻은 PHY 에 phy_init()/phy_power_on()
 *     을 위임한다. 끌 때는 clkreq GPIO 들을 raw 1 로 올린 뒤 phy_power_off()
 *     와 phy_exit() 를 부른다. 이 clkreq GPIO 처리는 960 경로에는 없다.
 *   - 자원: 960 만 "phy" reg 창과 "hisilicon,hi3660-crgctrl"/"-sctrl" syscon 을
 *     필요로 한다. 970 은 DT 의 phy 노드를 필요로 한다.
 */

/* [한국어] struct clk 와 devm_clk_get()/clk_prepare_enable()/clk_set_rate().
 * Kirin 960 내장 PHY 경로에서 클럭 다섯 개를 직접 다루기 위해 필요하다. */
#include <linux/clk.h>
/* [한국어] __always_inline 등 컴파일러 속성. 이 파일이 직접 쓰는 곳은 없고
 * 상류가 포함해 두었다(전수 확인). */
#include <linux/compiler.h>
/* [한국어] usleep_range(). PHY 대기, PERST# 유지 시간이 모두 이것이다. */
#include <linux/delay.h>
/* [한국어] IS_ERR()/PTR_ERR()/ERR_PTR(). 이 파일의 오류 처리 대부분이 이 관용구다. */
#include <linux/err.h>
/* [한국어] gpiod_ 계열 소비자 API. PERST# 와 clkreq GPIO 를 얻고 조작한다.
 * 슬롯마다 PERST# 가 따로 있는 구성 때문에 이 파일에서 비중이 크다. */
#include <linux/gpio/consumer.h>
/* [한국어] 인터럽트 헤더. 이 파일에는 핸들러가 없다(전수 확인) — 상류가 포함해 두었다. */
#include <linux/interrupt.h>
/* [한국어] syscon_regmap_lookup_by_compatible(). Kirin 960 경로가 crgctrl 과
 * sysctrl 이라는 두 SoC 제어 블록을 compatible 문자열로 찾아 쓴다. */
#include <linux/mfd/syscon.h>
/* [한국어] of_device_get_match_data() 와 for_each_available_child_of_node_scoped().
 * 960/970 갈림과 DT 자식 노드 순회가 여기서 온다. */
#include <linux/of.h>
/* [한국어] of_pci_get_devfn(). DT 자식 노드의 reg 속성에서 슬롯 번호를 뽑는다. */
#include <linux/of_pci.h>
/* [한국어] generic PHY 프레임워크. Kirin 970 경로가 phy_init()/phy_power_on() 을
 * 쓰기 위해 필요하다. 960 경로는 이것을 쓰지 않는다. */
#include <linux/phy/phy.h>
/* [한국어] PCI_SLOT() 같은 BDF 조작 매크로와 PCIBIOS_ 반환 코드. */
#include <linux/pci.h>
/* [한국어] 설정공간 레지스터 오프셋 정의. 이 파일이 직접 쓰는 곳은 없고
 * 상류가 포함해 두었다(전수 확인). */
#include <linux/pci_regs.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] devm_regmap_init_mmio()/regmap_read()/regmap_write().
 * "apb" 창을 생 iomem 이 아니라 regmap 으로 감싸 쓰는 것이 이 드라이버의 선택이다. */
#include <linux/regmap.h>
/* [한국어] struct resource. 자원 조회 API 가 쓰는 타입이다. */
#include <linux/resource.h>
/* [한국어] u32, bool 등 기본 타입. */
#include <linux/types.h>
/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_ops,
 * dw_pcie_host_ops, dw_pcie_host_init(), dw_pcie_read()/write(),
 * dw_pcie_read_dbi()/write_dbi(). */
#include "pcie-designware.h"

/* [한국어] struct dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 매크로.
 * rcar-gen4 등 다른 글루가 쓰는 container_of 방식이 아니라, 플랫폼 디바이스의
 * drvdata 를 거친다 — 이 드라이버는 struct kirin_pcie 안에 dw_pcie 를 값으로
 * 품지 않고 포인터로만 들고 있어서(kirin_pcie.pci) container_of 를 쓸 수 없다.
 *
 * 그 대신 시점 의존성이 생긴다. kirin_pcie_probe() 가 platform_set_drvdata()
 * 를 부른 뒤에야 이 매크로가 의미를 갖는다. 실제로 이 매크로를 쓰는 함수들은
 * 모두 그보다 뒤(power_on 이후, 또는 DWC 코어의 콜백)에서만 불린다. */
#define to_kirin_pcie(x) dev_get_drvdata((x)->dev)

/* [한국어] ELBI(External Local Bus Interface) 레지스터. DBI 창에 접근하기 전에
 * 사이드밴드로 인에이블을 켜야 하는데, 그 스위치가 여기 있다. */
/* PCIe ELBI registers */
/* [한국어] 컨트롤 0 — DBI "쓰기" 모드의 사이드밴드 스위치가 있다. */
#define SOC_PCIECTRL_CTRL0_ADDR		0x000
/* [한국어] 컨트롤 1 — DBI "읽기" 모드의 사이드밴드 스위치. 읽기와 쓰기가
 * 서로 다른 레지스터를 쓰는 것이 이 하드웨어의 특징이다. */
#define SOC_PCIECTRL_CTRL1_ADDR		0x004
/* [한국어] 두 컨트롤 레지스터에서 같은 자리(비트 21)를 쓰는 인에이블 비트.
 * 켜져 있는 동안에만 DBI 접근이 통한다. 그래서 이 파일의 DBI 접근은 늘
 * "켜기 → 접근 → 끄기" 세 단계다. */
#define PCIE_ELBI_SLV_DBI_ENABLE	(0x1 << 21)

/* [한국어] 아래 셋은 "apb" 창(kirin_pcie.apb regmap)의 오프셋이다. */
/* info located in APB */
/* [한국어] LTSSM(링크 훈련 상태기계) 시작 레지스터. kirin_pcie_start_link() 가
 * 여기에 비트를 써서 링크 훈련을 시작시킨다. */
#define PCIE_APP_LTSSM_ENABLE	0x01c
/* [한국어] PHY 상태 레지스터. 링크 업 여부를 여기서 읽는다.
 *
 * [상류 코드 관찰] 이 이름은 이 파일 안에서 두 번 정의된다 — 여기(0x400)와,
 * 아래 Kirin 960 PHY 절의 같은 이름(역시 0x400). 치환 목록이 같아 C 표준상
 * 허용되는 재정의라 컴파일 오류가 나지 않는다. 다만 가리키는 창은 서로 다르다:
 * 이쪽은 "apb" regmap 의 오프셋이고 저쪽은 "phy" 창에 ioremap 한 주소의
 * 오프셋이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
#define PCIE_APB_PHY_STATUS0	0x400
/* [한국어] 링크 업 판정 마스크 0x8020 = 비트 15 와 비트 5. 두 비트가 모두
 * 서 있어야 링크가 선 것으로 본다. 각 비트가 물리 계층과 데이터 링크 계층 중
 * 무엇을 가리키는지는 이 트리에서 확인 못 함. */
#define PCIE_LINKUP_ENABLE	(0x8020)
/* [한국어] LTSSM 시작 비트(비트 11). start_link 가 이 값을 레지스터에 통째로 쓴다. */
#define PCIE_LTSSM_ENABLE_BIT	(0x1 << 11)

/* [한국어] 아래 다섯은 sysctrl(SoC 시스템 제어) 블록의 오프셋이다.
 * Kirin 960 내장 PHY 경로에서만 쓴다. */
/* info located in sysctrl */
/* [한국어] PCIe 전원(CMOS) 제어 오프셋. */
#define SCTRL_PCIE_CMOS_OFFSET	0x60
/* [한국어] 전원을 켤 때 쓰는 값. 끌 때는 0 을 쓴다. */
#define SCTRL_PCIE_CMOS_BIT	0x10
/* [한국어] 아이솔레이션(격리) 해제 오프셋. 전원 도메인이 꺼진 동안 신호를
 * 잘라 두는 장치를 푸는 것이다. */
#define SCTRL_PCIE_ISO_OFFSET	0x44
/* [한국어] 아이솔레이션 해제에 쓰는 값. */
#define SCTRL_PCIE_ISO_BIT	0x30
/* [한국어] HP 클럭 게이트 오프셋. */
#define SCTRL_PCIE_HPCLK_OFFSET	0x190
/* [한국어] 클럭 게이트를 여는 값. */
#define SCTRL_PCIE_HPCLK_BIT	0x184000
/* [한국어] 출력 인에이블(OE) 제어 오프셋.
 *
 * [상류 코드 관찰] 0x14a 는 4의 배수가 아니다. 같은 블록의 다른 오프셋
 * (0x60, 0x44, 0x190)이 모두 4바이트 정렬인 것과 다르다. 이 오프셋을 쓰는
 * regmap 은 syscon 이 만든 것이라 그 stride 설정을 이 트리에서 확인할 수 없어,
 * 접근이 실제로 거부되는지 여부는 확인 못 함. 원본 스냅숏(1f0e418bb6)에서
 * 값이 0x14a 임을 확인했으며 코드는 손대지 않았다. */
#define SCTRL_PCIE_OE_OFFSET	0x14a
/* [한국어] 디바운스 파라미터. OE 레지스터에 OR 로 얹는 값이다. */
#define PCIE_DEBOUNCE_PARAM	0xF0F400
/* [한국어] OE 우회 비트 둘(비트 29:28). hi3660_pcie_phy_oe_enable() 이 이것을
 * 지워 우회를 끄고 정상 출력 경로를 쓰게 한다. */
#define PCIE_OE_BYPASS		(0x3 << 28)

/*
 * Max number of connected PCI slots at an external PCI bridge
 *
 * This is used on HiKey 970, which has a PEX 8606 bridge with 4 connected
 * lanes (lane 0 upstream, and the other three lanes, one connected to an
 * in-board Ethernet adapter and the other two connected to M.2 and mini
 * PCI slots.
 *
 * Each slot has a different clock source and uses a separate PERST# pin.
 */
/* [한국어] 위 상류 주석이 설명한 대로 외부 브리지 아래 슬롯 수의 상한이다.
 * id_reset_gpio[], reset_names[], id_clkreq_gpio[], clkreq_names[] 네 배열의
 * 길이가 모두 이 값이다. */
#define MAX_PCI_SLOTS		3

/* [한국어] 이 드라이버가 지원하는 두 PHY 통합 방식. 이 enum 의 값 하나가
 * Kirin 960 과 970 을 가르는 유일한 표식이다. */
enum pcie_kirin_phy_type {
	/* [한국어] 내장 PHY (Kirin 960). PHY 를 이 파일이 직접 다룬다.
	 * 설정자: kirin_960_data.phy_type 이 이 값을 갖고, probe 가 kirin_pcie.type 에 복사.
	 * 읽는 자: kirin_pcie_power_on() 과 kirin_pcie_power_off() 의 조건문 둘뿐.
	 * 값 범위: 0(열거 첫 항목).
	 * 동기화: probe 후 불변. */
	PCIE_KIRIN_INTERNAL_PHY,
	/* [한국어] 외부 PHY (Kirin 970). 별도의 PHY 드라이버에 맡긴다.
	 * 설정자: kirin_970_data.phy_type.
	 * 읽는 자: 위와 같은 두 조건문의 else 갈래.
	 * 값 범위: 1.
	 * 동기화: probe 후 불변.
	 *
	 * 이 갈래에서만 clkreq GPIO 를 쓰고, 이 갈래에서만 DT 에 phy 노드가 필요하다. */
	PCIE_KIRIN_EXTERNAL_PHY
};

/* [한국어] 이 드라이버의 상태 전부. 두 세대가 함께 쓴다. */
struct kirin_pcie {
	/* [한국어] 이 인스턴스가 내장 PHY 인지 외부 PHY 인지.
	 * 설정자: kirin_pcie_probe() 가 DT match data 의 phy_type 을 복사한다.
	 * 읽는 자: kirin_pcie_power_on(), kirin_pcie_power_off().
	 * 값 범위: 위 enum 의 두 값.
	 * 동기화: probe 후 불변.
	 *
	 * 이 파일에서 960/970 을 가르는 것이 정확히 이 필드 하나다. */
	enum pcie_kirin_phy_type	type;

	/* [한국어] DWC 코어의 컨트롤러 문맥.
	 * 설정자: kirin_pcie_probe() 가 따로 devm_kzalloc 한 것을 여기 건다.
	 * 읽는 자: kirin_pcie_remove()(pci->pp), kirin_pcie_add_bus()(pci->dev),
	 *   그리고 DWC 코어 전체.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 후 불변.
	 *
	 * 값이 아니라 포인터인 점이 중요하다. 그 때문에 container_of 로 되돌아올 수
	 * 없어 to_kirin_pcie 매크로가 drvdata 를 거치는 것이다. */
	struct dw_pcie	*pci;
	/* [한국어] "apb" 레지스터 창을 감싼 regmap.
	 * 설정자: kirin_pcie_get_resource() 가 ioremap 후 devm_regmap_init_mmio() 로 만든다.
	 * 읽는 자: 사이드밴드 켜고 끄기(두 함수), 링크 상태 읽기, LTSSM 쓰기.
	 * 값 범위: 유효한 regmap 포인터.
	 * 동기화: regmap 자체가 내부 락을 갖지만, 이 파일의 읽기-수정-쓰기 쌍은
	 *   그 락으로 보호되지 않는다(각 호출이 따로 잠기므로).
	 *
	 * 생 iomem 대신 regmap 을 쓰는 이유가 이 파일에 적혀 있지는 않다. */
	struct regmap   *apb;
	/* [한국어] 외부 PHY 핸들 (Kirin 970 전용).
	 * 설정자: kirin_pcie_power_on() 의 devm_of_phy_get().
	 * 읽는 자: 같은 함수의 phy_init()/phy_power_on() 과
	 *   kirin_pcie_power_off() 의 phy_power_off()/phy_exit().
	 * 값 범위: 유효한 phy 포인터. 960 경로에서는 0 초기화된 채 NULL 로 남는다.
	 * 동기화: probe 후 불변.
	 *
	 * 960 경로에서 이 필드가 NULL 인 채로 kirin_pcie_power_off() 의 phy_power_off()
	 * 에 닿지는 않는다 — 그 함수가 INTERNAL_PHY 갈래에서 먼저 반환하기 때문이다. */
	struct phy	*phy;
	/* [한국어] 내장 PHY 의 상태(struct hi3660_pcie_phy) 포인터. 옆의 상류 주석이
	 * 밝히듯 PCIE_KIRIN_INTERNAL_PHY 에서만 쓴다.
	 * 설정자: hi3660_pcie_phy_init() 이 devm_kzalloc 한 것을 여기 건다.
	 * 읽는 자: hi3660_pcie_phy_power_on()/power_off() 가 첫 줄에서 꺼내 쓴다.
	 * 값 범위: 유효한 hi3660_pcie_phy 포인터, 또는 970 경로에서는 NULL.
	 * 동기화: probe 후 불변.
	 *
	 * 타입이 void 포인터인 것은 세대마다 다른 PHY 상태를 담기 위한 것으로
	 * 보이지만, 실제로 이 파일에 담기는 타입은 hi3660_pcie_phy 하나뿐이다. */
	void		*phy_priv;	/* only for PCIE_KIRIN_INTERNAL_PHY */

	/* DWC PERST# */
	/* [한국어] 컨트롤러 자신이 내보내는 PERST#(하위 장치 리셋) GPIO. 위 상류 주석의
	 * "DWC PERST#" 가 이것이다.
	 * 설정자: kirin_pcie_get_resource() 의 devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW).
	 * 읽는 자: kirin_pcie_power_on() 이 raw 1 로 올린다.
	 * 값 범위: 유효한 gpio_desc 포인터. optional 이 아닌 get 이라 없으면 프로브가 실패한다.
	 * 동기화: probe 경로에서만 만진다.
	 *
	 * 아래 슬롯별 PERST# 와 구분해야 한다. 이것은 브리지로 나가는 하나이고,
	 * 아래 배열은 브리지 뒤 슬롯마다 하나씩이다. */
	struct gpio_desc *id_dwc_perst_gpio;

	/* Per-slot PERST# */
	/* [한국어] DT 에서 찾아낸 슬롯 개수.
	 * 설정자: kirin_pcie_parse_port() 가 슬롯 하나를 찾을 때마다 증가시킨다.
	 * 읽는 자: kirin_pcie_add_bus() 가 순회 상한으로 쓴다. 0 이면 곧바로 반환한다.
	 * 값 범위: 아래 [상류 코드 관찰] 대로 실제로는 0~2 에 머문다.
	 * 동기화: probe 경로에서만 쓴다.
	 *
	 * [상류 코드 관찰] parse_port 의 상한 검사가 `num_slots + 1 >= MAX_PCI_SLOTS`
	 * 라서, 세 번째 슬롯을 찾은 순간(검사 시점 num_slots == 2) -EINVAL 로 프로브가
	 * 실패한다. 배열 길이는 3 인데 실제로 받아들여지는 슬롯은 둘까지다.
	 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
	int		num_slots;
	/* [한국어] 슬롯별 PERST# GPIO 배열.
	 * 설정자: kirin_pcie_parse_port() 가 DT 손자 노드마다 하나씩 채운다.
	 * 읽는 자: kirin_pcie_add_bus() 가 버스 생성 시 모두 raw 1 로 올린다.
	 * 값 범위: 앞의 num_slots 개만 유효하고 나머지는 NULL 이다.
	 * 동기화: probe 에서 채우고 버스 스캔에서 읽는다. 두 시점이 겹치지 않는다. */
	struct gpio_desc *id_reset_gpio[MAX_PCI_SLOTS];
	/* [한국어] 위 GPIO 들의 소비자 이름 문자열 배열("pcie_perst_<슬롯번호>").
	 * 설정자: kirin_pcie_parse_port() 의 devm_kasprintf().
	 * 읽는 자: gpiod_set_consumer_name() 에 넘겨 debugfs 등에 표시되게 하고,
	 *   kirin_pcie_add_bus() 가 오류 로그에 쓴다.
	 * 값 범위: 유효한 문자열 포인터. devres 가 수명을 관리한다.
	 * 동기화: probe 후 불변.
	 *
	 * 이름의 숫자가 배열 인덱스가 아니라 DT 가 준 슬롯 번호(PCI_SLOT)라는 점이
	 * 아래 clkreq 이름과 다르다. */
	const char	*reset_names[MAX_PCI_SLOTS];

	/* Per-slot clkreq */
	/* [한국어] clkreq GPIO 개수.
	 * 설정자: kirin_pcie_get_gpio_enable() 이 gpiod_count() 결과로 채운다.
	 * 읽는 자: kirin_pcie_power_off() 가 순회 상한으로 쓴다.
	 * 값 범위: 0 ~ MAX_PCI_SLOTS(3). 속성이 없으면 0 으로 남는다.
	 * 동기화: probe 후 불변. */
	int		n_gpio_clkreq;
	/* [한국어] clkreq GPIO 배열.
	 * 설정자: kirin_pcie_get_gpio_enable() 의 devm_gpiod_get_index().
	 * 읽는 자: kirin_pcie_power_off() 가 외부 PHY 갈래에서 모두 raw 1 로 올린다.
	 *   켜는 쪽에서 내리는 곳은 이 파일에 없다 — GPIOD_OUT_LOW 로 얻는 것이
	 *   곧 초기값 0 이기 때문이다.
	 * 값 범위: 앞의 n_gpio_clkreq 개만 유효.
	 * 동기화: probe 와 power_off 에서만 만진다. */
	struct gpio_desc *id_clkreq_gpio[MAX_PCI_SLOTS];
	/* [한국어] 위 clkreq GPIO 들의 소비자 이름 배열("pcie_clkreq_<인덱스>").
	 * 설정자: kirin_pcie_get_gpio_enable() 의 devm_kasprintf().
	 * 읽는 자: gpiod_set_consumer_name() 뿐이다 — 로그에 쓰는 곳이 없어
	 *   PERST# 이름 배열과 쓰임이 다르다.
	 * 값 범위: 유효한 문자열 포인터.
	 * 동기화: probe 후 불변. */
	const char	*clkreq_names[MAX_PCI_SLOTS];
};

/*
 * Kirin 960 PHY. Can't be split into a PHY driver without changing the
 * DT schema.
 */

/* [한국어] 내장 PHY 의 기준 클럭 주파수 100MHz. PCIe 규격이 요구하는 값이며,
 * hi3660_pcie_phy_clk_ctrl() 이 clk_set_rate() 로 직접 못박는다. */
#define REF_CLK_FREQ			100000000

/* [한국어] 아래 넷은 "phy" 창(hi3660_pcie_phy.base)의 오프셋이다. 위쪽 "apb"
 * 창과 이름이 비슷하지만 다른 창이다. */
/* PHY info located in APB */
/* [한국어] PHY 제어 0 — 전원 차단 비트가 있다. */
#define PCIE_APB_PHY_CTRL0	0x0
/* [한국어] PHY 제어 1 — 기준 클럭 패드 선택과 리셋 확인 비트가 있다. */
#define PCIE_APB_PHY_CTRL1	0x4
/* [한국어] PHY 상태 0. 위쪽 "apb" 절에서 같은 이름을 같은 값으로 이미 정의했다 —
 * 그 정의에 달아 둔 [상류 코드 관찰] 참조. 이쪽은 "phy" 창의 오프셋으로 쓰인다. */
#define PCIE_APB_PHY_STATUS0   0x400
/* [한국어] PIPE 클럭 안정 비트(비트 19). 이름과 실제 쓰임이 반대라는 점은
 * hi3660_pcie_phy_start() 의 [상류 코드 관찰] 을 보라. */
#define PIPE_CLK_STABLE		BIT(19)
/* [한국어] 기준 클럭 패드 선택 비트(비트 8). PHY 시작 절차가 이것을 지운다. */
#define PHY_REF_PAD_BIT		BIT(8)
/* [한국어] PHY 전원 차단 비트(비트 22). 지워야 PHY 에 전원이 들어간다. */
#define PHY_PWR_DOWN_BIT	BIT(22)
/* [한국어] PHY 리셋 확인 비트(비트 16). 지워야 리셋이 풀린다. */
#define PHY_RST_ACK_BIT		BIT(16)

/* [한국어] 아래 둘은 crgctrl(클럭·리셋 생성기) 블록의 오프셋이다. syscon 으로 찾는다. */
/* peri_crg ctrl */
/* [한국어] PCIe 리셋 해제 오프셋. */
#define CRGCTRL_PCIE_ASSERT_OFFSET	0x88
/* [한국어] 리셋을 푸는 데 쓰는 값. hi3660_pcie_phy_power_on() 이 통째로 쓴다. */
#define CRGCTRL_PCIE_ASSERT_BIT		0x8c000000

/* [한국어] 아래 열 개는 이 PHY 절차가 지켜야 하는 대기 시간들이다.
 * 모두 usleep_range() 의 최소/최대 쌍으로 짝지어 정의되어 있다. */
/* Time for delay */
/* [한국어] 기준 클럭이 선 뒤 PERST# 를 낼 때까지의 최소 대기(21ms). */
#define REF_2_PERST_MIN		21000
/* [한국어] 같은 대기의 최대치(25ms). usleep_range 가 이 범위 안에서 자유롭게
 * 깨어날 수 있어 타이머 병합에 유리하다. */
#define REF_2_PERST_MAX		25000
/* [한국어] PERST# 를 낸 뒤 설정공간에 접근하기까지의 최소 대기(10ms). */
#define PERST_2_ACCESS_MIN	10000
/* [한국어] 같은 대기의 최대치(12ms). */
#define PERST_2_ACCESS_MAX	12000
/* [한국어] PIPE 클럭이 안정되기를 기다리는 최소 시간(550us). */
#define PIPE_CLK_WAIT_MIN	550
/* [한국어] 같은 대기의 최대치(600us). */
#define PIPE_CLK_WAIT_MAX	600
/* [한국어] PCIe 전원(CMOS)을 켠 뒤의 최소 대기(100us). */
#define TIME_CMOS_MIN		100
/* [한국어] 같은 대기의 최대치(105us). */
#define TIME_CMOS_MAX		105
/* [한국어] PHY 전원 차단을 푼 뒤의 최소 대기(10us). */
#define TIME_PHY_PD_MIN		10
/* [한국어] 같은 대기의 최대치(11us). */
#define TIME_PHY_PD_MAX		11

/* [한국어] Kirin 960 내장 PHY 전용 상태. kirin_pcie.phy_priv 가 이것을 가리킨다.
 * 970 경로에서는 아예 만들어지지 않는다. */
struct hi3660_pcie_phy {
	/* [한국어] 이 PHY 가 속한 장치. 클럭·자원 조회와 오류 로그에 쓴다.
	 * 설정자: hi3660_pcie_phy_init().
	 * 읽는 자: get_clk, get_resource, phy_start.
	 * 값 범위: PCIe 컨트롤러의 struct device 와 같은 것 — PHY 가 별도 디바이스가
	 *   아니라 같은 노드에 얹혀 있음을 뜻한다.
	 * 동기화: 초기화 후 불변. */
	struct device	*dev;
	/* [한국어] "phy" 레지스터 창의 가상 주소.
	 * 설정자: hi3660_pcie_phy_get_resource() 의 ioremap("phy").
	 * 읽는 자: kirin_apb_phy_readl()/writel() 뿐 — 이 창은 그 두 헬퍼로만 접근한다.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: 초기화 후 불변. */
	void __iomem	*base;
	/* [한국어] crgctrl(클럭·리셋 생성기) 블록의 regmap.
	 * 설정자: syscon_regmap_lookup_by_compatible("hisilicon,hi3660-crgctrl").
	 * 읽는 자: hi3660_pcie_phy_power_on() 의 리셋 해제 쓰기 한 곳뿐이다.
	 * 값 범위: 유효한 regmap 포인터.
	 * 동기화: syscon regmap 이 내부 락을 갖는다. 여러 드라이버가 같은 블록을
	 *   공유하므로 그 락이 필요하다. */
	struct regmap	*crgctrl;
	/* [한국어] sysctrl(시스템 제어) 블록의 regmap.
	 * 설정자: syscon_regmap_lookup_by_compatible("hisilicon,hi3660-sctrl").
	 * 읽는 자: 전원, 아이솔레이션, 클럭 게이트, OE 를 만지는 네 곳.
	 * 값 범위: 유효한 regmap 포인터.
	 * 동기화: 위 crgctrl 과 같다. */
	struct regmap	*sysctrl;
	/* [한국어] APB 시스템 클럭("pcie_apb_sys").
	 * 설정자: hi3660_pcie_phy_get_clk() 의 devm_clk_get().
	 * 읽는 자: hi3660_pcie_phy_clk_ctrl() 이 켜고 끈다.
	 * 값 범위: 유효한 clk 포인터. 없으면 프로브가 실패한다(optional 이 아니다).
	 * 동기화: clk 프레임워크가 관리한다. */
	struct clk	*apb_sys_clk;
	/* [한국어] APB PHY 클럭("pcie_apb_phy"). PHY 레지스터 창에 접근하려면 필요하다.
	 * 설정자/읽는 자/값 범위/동기화: 위 apb_sys_clk 와 같다. */
	struct clk	*apb_phy_clk;
	/* [한국어] PHY 기준 클럭("pcie_phy_ref"). 다섯 중 유일하게 주파수를 직접
	 * 설정하는 클럭이다(clk_set_rate 로 100MHz).
	 * 설정자: hi3660_pcie_phy_get_clk().
	 * 읽는 자: hi3660_pcie_phy_clk_ctrl() 이 rate 를 정하고 켠다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: clk 프레임워크가 관리한다. */
	struct clk	*phy_ref_clk;
	/* [한국어] AXI 클럭("pcie_aclk"). 데이터 경로 쪽 버스 클럭이다.
	 * 설정자/읽는 자/값 범위/동기화: 위 apb_sys_clk 와 같다. */
	struct clk	*aclk;
	/* [한국어] 보조 클럭("pcie_aux"). 주 클럭이 꺼진 저전력 상태에서도 최소한의
	 * 상태를 유지하는 데 쓰이는 종류다.
	 * 설정자: hi3660_pcie_phy_get_clk().
	 * 읽는 자: hi3660_pcie_phy_clk_ctrl() 이 마지막으로 켜고 가장 먼저 끈다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: clk 프레임워크가 관리한다. */
	struct clk	*aux_clk;
};

/* Registers in PCIePHY */
/* [한국어]
 * kirin_apb_phy_writel - Kirin 960 내장 PHY 의 레지스터에 쓴다
 *
 * @hi3660_pcie_phy: 내장 PHY 상태. base 가 채워져 있어야 한다.
 * @val: 쓸 값.
 * @reg: "phy" 창 안의 오프셋.
 * @return: 없음.
 *
 * "phy" 창에 접근하는 두 헬퍼 중 쓰기 쪽이다. 이 창은 이 헬퍼 쌍으로만
 * 접근하므로, 창 접근을 한곳에 모아 두는 역할을 한다.
 *
 * 인자 순서가 writel() 과 같은 (값, 주소) 라는 점을 눈여겨볼 것 —
 * 커널 관용구를 따른 것이다.
 *
 * regmap 이 아니라 생 iomem 인 것이 위쪽 "apb" 창과 다르다. 같은 파일 안에서
 * 두 창을 다른 방식으로 다루는 셈인데, 그 이유는 이 파일에 적혀 있지 않다.
 *
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_start() → [이 함수] → writel()
 */
static inline void kirin_apb_phy_writel(struct hi3660_pcie_phy *hi3660_pcie_phy,
					u32 val, u32 reg)
{
	/* [한국어] 창 시작에 오프셋을 더한 주소에 쓴다. 이 한 줄이 전부다. */
	writel(val, hi3660_pcie_phy->base + reg);
}

/* [한국어]
 * kirin_apb_phy_readl - Kirin 960 내장 PHY 의 레지스터를 읽는다
 *
 * @hi3660_pcie_phy: 내장 PHY 상태.
 * @reg: "phy" 창 안의 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 위 kirin_apb_phy_writel() 의 짝이다. 이 파일에서 이 함수의 쓰임은 전부
 * hi3660_pcie_phy_start() 안에 있고, 그중 셋은 읽기-수정-쓰기의 첫 단계,
 * 하나는 상태 확인이다.
 *
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. MMIO 읽기에는 실패라는 개념이 없다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_start() → [이 함수] → readl()
 */
static inline u32 kirin_apb_phy_readl(struct hi3660_pcie_phy *hi3660_pcie_phy,
				      u32 reg)
{
	/* [한국어] 창 시작에 오프셋을 더한 주소에서 읽는다. */
	return readl(hi3660_pcie_phy->base + reg);
}

/* [한국어]
 * hi3660_pcie_phy_get_clk - Kirin 960 내장 PHY 가 쓰는 클럭 다섯 개를 얻는다
 *
 * @phy: 내장 PHY 상태. dev 가 채워져 있어야 한다.
 * @return: 0 성공. 하나라도 없으면 그 오류 코드.
 *
 * 이 함수가 이 드라이버의 960/970 갈림을 가장 잘 보여 준다. 970 경로는
 * PHY 드라이버 하나만 얻으면 끝나지만, 960 경로는 클럭 다섯 개를 이름으로
 * 일일이 얻어야 한다 — PHY 를 이 파일이 직접 다루기 때문이다.
 *
 * 다섯 모두 devm_clk_get() 이라 optional 이 아니다. 즉 DT 에 다섯 이름이
 * 모두 있어야 하며, 하나라도 없으면 프로브가 실패한다.
 *
 * devm 이므로 실패 시 되감을 것이 없다 — 앞서 얻은 클럭 핸들은 devres 가
 * 장치 해제 때 알아서 놓아 준다. 그래서 중간 실패마다 곧바로 반환한다.
 *
 * 실행 컨텍스트: probe. clk 조회는 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 그대로 올린다. -EPROBE_DEFER 가 올라오면 플랫폼 코어가
 * 나중에 다시 프로브한다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_init() → [이 함수] → devm_clk_get()
 */
static int hi3660_pcie_phy_get_clk(struct hi3660_pcie_phy *phy)
{
	/* [한국어] clk 조회 API 가 받는 장치. hi3660_pcie_phy_init() 이 채워 둔 것이다. */
	struct device *dev = phy->dev;

	/* [한국어] PHY 기준 클럭. 다섯 중 유일하게 아래에서 주파수까지 지정하는 것이다. */
	phy->phy_ref_clk = devm_clk_get(dev, "pcie_phy_ref");
	/* [한국어] 조회 실패. */
	if (IS_ERR(phy->phy_ref_clk))
		/* [한국어] devm 이라 되감을 것이 없어 바로 올린다. */
		return PTR_ERR(phy->phy_ref_clk);

	/* [한국어] 보조 클럭. 저전력 상태에서 상태를 유지하는 데 쓰이는 종류다. */
	phy->aux_clk = devm_clk_get(dev, "pcie_aux");
	/* [한국어] 조회 실패. */
	if (IS_ERR(phy->aux_clk))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(phy->aux_clk);

	/* [한국어] APB PHY 클럭. PHY 레지스터 창에 접근하려면 이것이 살아 있어야 한다. */
	phy->apb_phy_clk = devm_clk_get(dev, "pcie_apb_phy");
	/* [한국어] 조회 실패. */
	if (IS_ERR(phy->apb_phy_clk))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(phy->apb_phy_clk);

	/* [한국어] APB 시스템 클럭. */
	phy->apb_sys_clk = devm_clk_get(dev, "pcie_apb_sys");
	/* [한국어] 조회 실패. */
	if (IS_ERR(phy->apb_sys_clk))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(phy->apb_sys_clk);

	/* [한국어] AXI 클럭. 데이터 경로 쪽 버스 클럭이다. */
	phy->aclk = devm_clk_get(dev, "pcie_aclk");
	/* [한국어] 조회 실패. */
	if (IS_ERR(phy->aclk))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(phy->aclk);

	/* [한국어] 다섯 개를 모두 얻었다. 켜는 것은 hi3660_pcie_phy_clk_ctrl() 이 한다. */
	return 0;
}

/* [한국어]
 * hi3660_pcie_phy_get_resource - 내장 PHY 가 쓸 레지스터 창과 syscon 두 벌을 얻는다
 *
 * @phy: 내장 PHY 상태. dev 가 채워져 있어야 한다.
 * @return: 0 성공, 실패 시 그 오류 코드.
 *
 * 얻는 것이 셋이다.
 *   1. "phy" 레지스터 창 — PHY 자체의 제어/상태 레지스터.
 *   2. crgctrl — 클럭·리셋 생성기. 리셋 해제 한 번에만 쓴다.
 *   3. sysctrl — 시스템 제어. 전원, 아이솔레이션, 클럭 게이트, OE 에 쓴다.
 *
 * 뒤 둘은 DT 의 phandle 이 아니라 compatible 문자열로 직접 찾는다. 즉 이
 * 노드가 그 블록을 참조한다고 DT 에 적어 두지 않아도 되지만, 대신 SoC 이름이
 * 코드에 박히게 된다 — 이 파일이 Kirin 960(hi3660) 전용 코드를 품고 있다는
 * 사실이 문자열로 드러나는 지점이다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 그대로 올린다. syscon 이 아직 등록되지 않았으면
 * -EPROBE_DEFER 가 올라올 수 있다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_init() → [이 함수]
 *     → devm_platform_ioremap_resource_byname(),
 *       syscon_regmap_lookup_by_compatible()
 */
static int hi3660_pcie_phy_get_resource(struct hi3660_pcie_phy *phy)
{
	/* [한국어] 자원 조회에 쓸 장치. */
	struct device *dev = phy->dev;
	/* [한국어] 자원 조회 API 가 받는 플랫폼 디바이스. 아래에서 dev 로부터 되찾는다. */
	struct platform_device *pdev;

	/* registers */
	/* [한국어] struct device 에서 그것을 품은 플랫폼 디바이스로 거슬러 올라간다.
	 * 커널에는 같은 일을 하는 to_platform_device() 매크로가 있는데, 여기서는
	 * container_of 를 직접 썼다. */
	pdev = container_of(dev, struct platform_device, dev);

	/* [한국어] "phy" 라는 이름의 reg 항목을 찾아 매핑한다. 이 창이 위
	 * kirin_apb_phy_readl/writel 이 다루는 대상이다. */
	phy->base = devm_platform_ioremap_resource_byname(pdev, "phy");
	/* [한국어] 자원이 없거나 매핑 실패. */
	if (IS_ERR(phy->base))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(phy->base);

	/* [한국어] 클럭·리셋 생성기 블록을 compatible 문자열로 찾는다. 여러 드라이버가
	 * 공유하는 블록이라 syscon 이라는 공용 창구를 거친다. */
	phy->crgctrl = syscon_regmap_lookup_by_compatible("hisilicon,hi3660-crgctrl");
	/* [한국어] 아직 등록되지 않았거나 DT 에 없다. */
	if (IS_ERR(phy->crgctrl))
		/* [한국어] 그대로 올린다. -EPROBE_DEFER 면 나중에 다시 시도된다. */
		return PTR_ERR(phy->crgctrl);

	/* [한국어] 시스템 제어 블록. 전원·아이솔레이션·클럭 게이트·OE 가 여기 있다. */
	phy->sysctrl = syscon_regmap_lookup_by_compatible("hisilicon,hi3660-sctrl");
	/* [한국어] 마찬가지로 실패할 수 있다. */
	if (IS_ERR(phy->sysctrl))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(phy->sysctrl);

	/* [한국어] 셋을 모두 얻었다. 실제 사용은 power_on 부터다. */
	return 0;
}

/* [한국어]
 * hi3660_pcie_phy_start - Kirin 960 내장 PHY 를 깨우고 PIPE 클럭이 서기를 기다린다
 *
 * @phy: 내장 PHY 상태. base 와 dev 가 채워져 있어야 한다.
 * @return: 0 성공, -ETIMEDOUT 이면 PIPE 클럭이 서지 않았다.
 *
 * hi3660_pcie_phy_power_on() 의 마지막 단계다. 전원과 클럭 게이트가 이미
 * 열린 상태에서, PHY 자체의 제어 비트 셋을 순서대로 풀어 준다.
 *
 * 순서가 곧 의존 관계다.
 *   1. 기준 클럭 패드 선택 비트를 지운다.
 *   2. 전원 차단 비트를 지운다. 그다음 10us 를 쉰다 — 전원이 실제로 올라올
 *      시간을 준다.
 *   3. 리셋 확인 비트를 지운다. 여기서 PHY 가 동작을 시작한다.
 *   4. 550us 를 쉬고 PIPE 클럭 상태를 확인한다.
 *
 * PIPE(PHY Interface for PCI Express)는 컨트롤러와 PHY 사이의 표준
 * 인터페이스이고, 그 구간 클럭이 서야 링크 훈련을 시작할 수 있다.
 *
 * [상류 코드 관찰] 마지막 판정이 이름과 반대로 읽힌다. 비트 이름은
 * PIPE_CLK_STABLE("안정") 인데, 그 비트가 서 있으면 "PIPE clk is not stable"
 * 이라는 메시지와 함께 -ETIMEDOUT 을 돌려준다. 즉 코드는 이 비트를
 * "안정" 이 아니라 "불안정" 표시로 다루고 있다. 둘 중 어느 쪽이 하드웨어
 * 사실인지는 HiSilicon 데이터시트를 볼 수 없어 이 트리에서 확인 못 함.
 * 원본 스냅숏(1f0e418bb6)에서 코드가 이대로임을 확인했으며 손대지 않았다.
 *
 * 실행 컨텍스트: probe. usleep_range() 로 잠든다.
 *
 * 에러 경로: dev_err_probe() 로 로그를 남기며 -ETIMEDOUT 을 돌려준다.
 * 호출자는 그때 켜 둔 클럭들을 되돌린다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_power_on() → [이 함수]
 *     → kirin_apb_phy_readl()/writel(), usleep_range(), dev_err_probe()
 */
static int hi3660_pcie_phy_start(struct hi3660_pcie_phy *phy)
{
	/* [한국어] dev_err_probe 에 넘길 장치. */
	struct device *dev = phy->dev;
	/* [한국어] 읽기-수정-쓰기와 상태 확인에 함께 쓰는 임시 값. */
	u32 reg_val;

	/* [한국어] 제어 1 을 읽는다. 다른 비트를 보존해야 한다. */
	reg_val = kirin_apb_phy_readl(phy, PCIE_APB_PHY_CTRL1);
	/* [한국어] 기준 클럭 패드 선택 비트를 지운다. */
	reg_val &= ~PHY_REF_PAD_BIT;
	/* [한국어] 반영한다. */
	kirin_apb_phy_writel(phy, reg_val, PCIE_APB_PHY_CTRL1);

	/* [한국어] 제어 0 을 읽는다. */
	reg_val = kirin_apb_phy_readl(phy, PCIE_APB_PHY_CTRL0);
	/* [한국어] 전원 차단 비트를 지워 PHY 에 전원을 넣는다. */
	reg_val &= ~PHY_PWR_DOWN_BIT;
	/* [한국어] 반영한다. */
	kirin_apb_phy_writel(phy, reg_val, PCIE_APB_PHY_CTRL0);
	/* [한국어] 전원이 실제로 올라올 시간 10~11us 를 준다. 이 대기 없이 다음
	 * 단계로 가면 리셋 해제가 먹지 않을 수 있다. */
	usleep_range(TIME_PHY_PD_MIN, TIME_PHY_PD_MAX);

	/* [한국어] 제어 1 을 다시 읽는다. 위에서 이미 한 번 고쳤으므로 최신 값이 필요하다. */
	reg_val = kirin_apb_phy_readl(phy, PCIE_APB_PHY_CTRL1);
	/* [한국어] 리셋 확인 비트를 지워 PHY 리셋을 푼다. 이 쓰기 이후 PHY 가 스스로
	 * 초기화를 시작한다. */
	reg_val &= ~PHY_RST_ACK_BIT;
	/* [한국어] 반영한다. */
	kirin_apb_phy_writel(phy, reg_val, PCIE_APB_PHY_CTRL1);

	/* [한국어] PIPE 클럭이 설 때까지 550~600us 기다린다. 폴링이 아니라 고정 대기라,
	 * 이보다 오래 걸리는 개체는 아래 판정에서 실패로 처리된다. */
	usleep_range(PIPE_CLK_WAIT_MIN, PIPE_CLK_WAIT_MAX);
	/* [한국어] PHY 상태 레지스터를 읽는다. */
	reg_val = kirin_apb_phy_readl(phy, PCIE_APB_PHY_STATUS0);
	/* [한국어] 위 [상류 코드 관찰] 이 가리키는 판정이다. 이름과 달리 비트가 서 있을 때
	 * 실패로 본다. */
	if (reg_val & PIPE_CLK_STABLE)
		/* [한국어] dev_err_probe 는 로그를 남기고 받은 오류 코드를 그대로 돌려준다 —
		 * -EPROBE_DEFER 일 때 로그를 줄여 주는 것이 이 헬퍼의 존재 이유다. */
		return dev_err_probe(dev, -ETIMEDOUT,
				     "PIPE clk is not stable\n");

	/* [한국어] PIPE 클럭이 섰다. 이제 링크 훈련을 시작할 수 있다. */
	return 0;
}

/* [한국어]
 * hi3660_pcie_phy_oe_enable - sysctrl 의 출력 인에이블(OE) 우회를 끄고 디바운스를 켠다
 *
 * @phy: 내장 PHY 상태. sysctrl 이 채워져 있어야 한다.
 * @return: 없음.
 *
 * hi3660_pcie_phy_power_on() 의 앞부분에서 한 번 불린다. 하는 일은 sysctrl
 * 레지스터 하나의 읽기-수정-쓰기다 — 디바운스 파라미터를 얹고 OE 우회 비트
 * 둘을 지운다.
 *
 * 이 함수가 같은 파일의 다른 sysctrl 접근과 다른 점이 있다. 다른 곳들은
 * regmap_write() 로 값을 통째로 쓰지만, 여기만 읽고 고쳐 되쓴다. 두 방식이
 * 섞여 있는 이유는 이 파일에 적혀 있지 않다.
 *
 * 이 오프셋(0x14a)이 4의 배수가 아니라는 점은 그 정의에 달아 둔
 * [상류 코드 관찰] 을 보라.
 *
 * 실행 컨텍스트: probe. regmap 접근이라 잠들 수 있다.
 *
 * 에러 경로: regmap_read/write 의 반환값을 보지 않는다. 원본 스냅숏에서도
 * 같으며 코드는 손대지 않았다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_power_on() → [이 함수] → regmap_read(), regmap_write()
 */
static void hi3660_pcie_phy_oe_enable(struct hi3660_pcie_phy *phy)
{
	/* [한국어] 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] 현재 값을 읽는다. 반환값을 보지 않으므로 실패하면 val 이
	 * 초기화되지 않은 채 쓰일 수 있다 — 위 함수 주석의 관찰 참조. */
	regmap_read(phy->sysctrl, SCTRL_PCIE_OE_OFFSET, &val);
	/* [한국어] 디바운스 파라미터를 얹는다. 신호가 튀는 것을 걸러 내는 설정이다. */
	val |= PCIE_DEBOUNCE_PARAM;
	/* [한국어] OE 우회 비트 둘을 지워, 우회하지 않고 정상 출력 경로를 쓰게 한다. */
	val &= ~PCIE_OE_BYPASS;
	/* [한국어] 고친 값을 되쓴다. */
	regmap_write(phy->sysctrl, SCTRL_PCIE_OE_OFFSET, val);
}

/* [한국어]
 * hi3660_pcie_phy_clk_ctrl - 내장 PHY 의 클럭 다섯 개를 순서대로 켜거나 끈다
 *
 * @phy: 내장 PHY 상태. 다섯 클럭 핸들이 채워져 있어야 한다.
 * @enable: true 면 켜기, false 면 끄기.
 * @return: 0 성공. 켜는 도중 실패하면 그 오류 코드(이미 켠 것은 되돌린 뒤).
 *
 * 이 함수의 구조가 볼 만하다. 끄기 경로와 켜기 실패의 되감기 경로가 같은
 * 라벨 사다리를 공유한다 — enable 이 false 면 곧바로 close_clk 로 뛰어
 * 사다리 맨 위부터 다섯 개를 모두 끄고, 켜다가 실패하면 실패한 지점에
 * 해당하는 라벨로 뛰어 그 아래(=이미 켠 것)만 끈다.
 *
 * 켜는 순서는 phy_ref → apb_sys → apb_phy → aclk → aux 이고, 끄는 순서는
 * 정확히 그 역순이다. 라벨 이름이 "그 클럭을 켜다 실패했다" 는 뜻이라,
 * 예를 들어 aclk_fail 로 뛰면 aclk 부터가 아니라 그 아래 apb_phy 부터 끈다.
 *
 * 기준 클럭만 clk_set_rate() 로 주파수를 못박는다. PCIe 규격이 요구하는
 * 100MHz 다.
 *
 * 실행 컨텍스트: probe 또는 정리 경로. clk 조작은 잠들 수 있다.
 *
 * 에러 경로: 위 설명대로 사다리로 되감는다. 끄기 경로는 실패할 수 없으므로
 * ret 가 0 인 채로 반환된다.
 *
 * 호출 체인:
 *   hi3660_pcie_phy_power_on() / hi3660_pcie_phy_power_off() → [이 함수]
 *     → clk_set_rate(), clk_prepare_enable(), clk_disable_unprepare()
 */
static int hi3660_pcie_phy_clk_ctrl(struct hi3660_pcie_phy *phy, bool enable)
{
	/* [한국어] 결과. 끄기 경로에서는 0 인 채로 사다리를 지나 반환된다. */
	int ret = 0;

	/* [한국어] 끄라는 요청이면 켜는 절차를 통째로 건너뛴다. */
	if (!enable)
		/* [한국어] 사다리 맨 위로 뛰어 다섯 개를 모두 끈다. */
		goto close_clk;

	/* [한국어] 기준 클럭의 주파수를 100MHz 로 못박는다. 다섯 중 유일하게 rate 를
	 * 지정하는 클럭이며, 링크 양쪽이 같은 기준으로 동작해야 하기 때문이다. */
	ret = clk_set_rate(phy->phy_ref_clk, REF_CLK_FREQ);
	/* [한국어] 주파수 설정 실패. */
	if (ret)
		/* [한국어] 아직 아무것도 켜지 않았으므로 되감을 것이 없다. */
		return ret;

	/* [한국어] 기준 클럭을 켠다. 첫 번째다. */
	ret = clk_prepare_enable(phy->phy_ref_clk);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] 역시 되감을 것이 없다. */
		return ret;

	/* [한국어] APB 시스템 클럭. 두 번째다. */
	ret = clk_prepare_enable(phy->apb_sys_clk);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] 이 라벨로 뛰면 그 아래의 phy_ref 만 꺼진다. */
		goto apb_sys_fail;

	/* [한국어] APB PHY 클럭. 세 번째다. */
	ret = clk_prepare_enable(phy->apb_phy_clk);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] apb_sys 와 phy_ref 를 되돌린다. */
		goto apb_phy_fail;

	/* [한국어] AXI 클럭. 네 번째다. */
	ret = clk_prepare_enable(phy->aclk);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] apb_phy 아래를 되돌린다. */
		goto aclk_fail;

	/* [한국어] 보조 클럭. 마지막이다. */
	ret = clk_prepare_enable(phy->aux_clk);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] aclk 아래를 되돌린다. */
		goto aux_clk_fail;

	/* [한국어] 다섯 개가 모두 켜졌다. */
	return 0;

/* [한국어] 끄기 요청이 뛰어 들어오는 자리. 여기부터는 켜는 순서의 역순이다. */
close_clk:
	/* [한국어] 보조 클럭을 끈다. 가장 나중에 켠 것을 가장 먼저 끈다. */
	clk_disable_unprepare(phy->aux_clk);
/* [한국어] aux 를 켜다 실패했을 때 오는 자리 — 그 아래부터 되돌린다. */
aux_clk_fail:
	/* [한국어] AXI 클럭을 끈다. */
	clk_disable_unprepare(phy->aclk);
/* [한국어] aclk 를 켜다 실패했을 때. */
aclk_fail:
	/* [한국어] APB PHY 클럭을 끈다. */
	clk_disable_unprepare(phy->apb_phy_clk);
/* [한국어] apb_phy 를 켜다 실패했을 때. */
apb_phy_fail:
	/* [한국어] APB 시스템 클럭을 끈다. */
	clk_disable_unprepare(phy->apb_sys_clk);
/* [한국어] apb_sys 를 켜다 실패했을 때. */
apb_sys_fail:
	/* [한국어] 기준 클럭을 끈다. 사다리의 끝이다. */
	clk_disable_unprepare(phy->phy_ref_clk);

	/* [한국어] 켜다 실패했으면 그 오류, 끄기 요청이었으면 0 이 나간다. */
	return ret;
}

/* [한국어]
 * hi3660_pcie_phy_power_on - Kirin 960 내장 PHY 전체를 켜는 절차
 *
 * @pcie: 이 드라이버의 상태. phy_priv 가 이미 채워져 있어야 한다.
 * @return: 0 성공, 실패 시 그 오류 코드.
 *
 * 970 경로의 phy_init()+phy_power_on() 두 줄에 대응하는 960 쪽 전체다.
 * 순서가 이 함수의 내용 전부다.
 *   1. sysctrl 로 PCIe 전원을 넣고 100us 기다린다.
 *   2. OE 우회를 끈다.
 *   3. 클럭 다섯 개를 켠다.
 *   4. 아이솔레이션을 풀고, crgctrl 로 리셋을 풀고, 클럭 게이트를 연다.
 *   5. PHY 자체를 깨우고 PIPE 클럭을 기다린다.
 *
 * 4번의 세 쓰기가 모두 read-modify-write 가 아니라 통째로 쓰기라는 점을
 * 눈여겨볼 것. 이런 SoC 제어 레지스터가 "쓴 비트만 동작하는" 방식이면
 * 문제가 없지만, 그 전제는 이 파일에 적혀 있지 않고 해당 syscon 드라이버가
 * 이 트리에 없어 확인 못 함.
 *
 * 실행 컨텍스트: probe. usleep_range 와 clk 조작으로 잠든다.
 *
 * 에러 경로: 클럭 켜기 실패는 그대로 반환(clk_ctrl 이 스스로 되감았다).
 * PHY 시작 실패는 disable_clks 로 가서 클럭만 되돌린다 — 전원과 아이솔레이션,
 * 리셋은 되돌리지 않는다.
 *
 * 호출 체인:
 *   kirin_pcie_power_on() → [이 함수]
 *     → regmap_write(), hi3660_pcie_phy_oe_enable(),
 *       hi3660_pcie_phy_clk_ctrl(), hi3660_pcie_phy_start()
 */
static int hi3660_pcie_phy_power_on(struct kirin_pcie *pcie)
{
	/* [한국어] void 포인터로 보관된 내장 PHY 상태를 제 타입으로 꺼낸다. */
	struct hi3660_pcie_phy *phy = pcie->phy_priv;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* Power supply for Host */
	/* [한국어] sysctrl 의 전원 레지스터에 켜기 값을 쓴다. 옆의 상류 주석대로
	 * 호스트 쪽 전원 공급이다. */
	regmap_write(phy->sysctrl,
		     SCTRL_PCIE_CMOS_OFFSET, SCTRL_PCIE_CMOS_BIT);
	/* [한국어] 전원이 안정될 시간 100~105us 를 준다. */
	usleep_range(TIME_CMOS_MIN, TIME_CMOS_MAX);

	/* [한국어] 출력 인에이블 우회를 끄고 디바운스를 켠다. */
	hi3660_pcie_phy_oe_enable(phy);

	/* [한국어] 클럭 다섯 개를 순서대로 켠다. */
	ret = hi3660_pcie_phy_clk_ctrl(phy, true);
	/* [한국어] 하나라도 실패했다. */
	if (ret)
		/* [한국어] clk_ctrl 이 이미 켠 것들을 되돌렸으므로 그대로 반환한다. */
		return ret;

	/* ISO disable, PCIeCtrl, PHY assert and clk gate clear */
	/* [한국어] 아이솔레이션을 푼다. 전원 도메인이 꺼진 동안 신호를 잘라 두던
	 * 장치를 여는 것으로, 옆의 상류 주석이 이 세 쓰기를 한 줄로 요약해 두었다. */
	regmap_write(phy->sysctrl,
		     SCTRL_PCIE_ISO_OFFSET, SCTRL_PCIE_ISO_BIT);
	/* [한국어] crgctrl 로 PCIe 리셋을 푼다. 이 파일에서 crgctrl 을 쓰는 유일한 곳이다. */
	regmap_write(phy->crgctrl,
		     CRGCTRL_PCIE_ASSERT_OFFSET, CRGCTRL_PCIE_ASSERT_BIT);
	/* [한국어] 클럭 게이트를 연다. 위 clk_prepare_enable 과 별개의 경로로,
	 * 같은 클럭을 가리키는지 다른 것인지는 이 트리에서 확인 못 함. */
	regmap_write(phy->sysctrl,
		     SCTRL_PCIE_HPCLK_OFFSET, SCTRL_PCIE_HPCLK_BIT);

	/* [한국어] PHY 자체를 깨우고 PIPE 클럭이 서기를 기다린다. */
	ret = hi3660_pcie_phy_start(phy);
	/* [한국어] PIPE 클럭이 서지 않았다. */
	if (ret)
		/* [한국어] 켠 클럭만 되돌리러 간다. */
		goto disable_clks;

	/* [한국어] 여기까지 오면 내장 PHY 가 동작 상태다. */
	return 0;

/* [한국어] PHY 시작 실패만 이 라벨로 온다. */
disable_clks:
	/* [한국어] 클럭 다섯 개를 끈다. 전원·아이솔레이션·리셋은 되돌리지 않는다. */
	hi3660_pcie_phy_clk_ctrl(phy, false);
	/* [한국어] 실패 원인을 그대로 올린다. */
	return ret;
}

/* [한국어]
 * hi3660_pcie_phy_init - 내장 PHY 상태를 잡고 클럭·자원을 확보한다
 *
 * @pdev: 이 컨트롤러의 플랫폼 디바이스.
 * @pcie: 이 드라이버의 상태. 이 함수가 phy_priv 를 채운다.
 * @return: 0 성공, -ENOMEM 또는 하위 조회 실패값.
 *
 * 960 경로에서만 불린다. 이름이 "init" 이지만 하드웨어를 만지지는 않고,
 * 상태 구조체를 잡고 클럭 다섯과 자원 셋을 확보하는 데서 끝난다. 실제로
 * 켜는 것은 곧이어 불리는 hi3660_pcie_phy_power_on() 이다.
 *
 * 970 경로의 devm_of_phy_get() 한 줄에 대응하는 자리다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, 조회 실패는 그 값을 그대로 올린다.
 * 모두 devm 이라 되감을 것이 없다.
 *
 * 호출 체인:
 *   kirin_pcie_power_on() → [이 함수]
 *     → devm_kzalloc(), hi3660_pcie_phy_get_clk(), hi3660_pcie_phy_get_resource()
 */
static int hi3660_pcie_phy_init(struct platform_device *pdev,
				struct kirin_pcie *pcie)
{
	/* [한국어] devm 할당과 하위 조회에 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 새로 잡을 내장 PHY 상태. */
	struct hi3660_pcie_phy *phy;
	/* [한국어] 하위 조회의 결과. */
	int ret;

	/* [한국어] 0 으로 초기화된 PHY 상태를 devres 로 잡는다. */
	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!phy)
		/* [한국어] 되감을 것이 없다. */
		return -ENOMEM;

	/* [한국어] 드라이버 상태에 걸어 둔다. 이 이후 power_on/power_off 가 이것으로
	 * PHY 상태에 닿는다. */
	pcie->phy_priv = phy;
	/* [한국어] 하위 조회 함수들이 쓸 장치를 채운다. PHY 가 별도 디바이스가 아니라
	 * PCIe 컨트롤러와 같은 노드에 얹혀 있음을 뜻한다. */
	phy->dev = dev;

	/* [한국어] 클럭 다섯 개를 얻는다. */
	ret = hi3660_pcie_phy_get_clk(phy);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. */
		return ret;

	/* [한국어] 레지스터 창과 syscon 두 벌을 얻고, 그 결과를 그대로 반환한다. */
	return hi3660_pcie_phy_get_resource(phy);
}

/* [한국어]
 * hi3660_pcie_phy_power_off - 내장 PHY 를 끈다
 *
 * @pcie: 이 드라이버의 상태.
 * @return: 항상 0.
 *
 * hi3660_pcie_phy_power_on() 의 되감기지만 대칭이 아니다. 전원을 끊고 클럭을
 * 끄는 둘만 하고, 아이솔레이션·리셋·클럭 게이트·OE 는 되돌리지 않는다.
 * 전원이 끊기면 그 도메인의 레지스터 상태가 어차피 사라지므로 되돌릴 이유가
 * 없다는 판단으로 보이나, 그 근거가 이 파일에 적혀 있지는 않다.
 *
 * 반환 타입이 int 인 것은 호출자 kirin_pcie_power_off() 가 이 값을 그대로
 * 자기 반환값으로 쓰기 때문이다.
 *
 * 실행 컨텍스트: probe 실패 되감기 또는 remove. clk 조작으로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   kirin_pcie_power_off() → [이 함수]
 *     → regmap_write(), hi3660_pcie_phy_clk_ctrl()
 */
static int hi3660_pcie_phy_power_off(struct kirin_pcie *pcie)
{
	/* [한국어] void 포인터로 보관된 PHY 상태를 제 타입으로 꺼낸다. */
	struct hi3660_pcie_phy *phy = pcie->phy_priv;

	/* Drop power supply for Host */
	/* [한국어] sysctrl 의 전원 레지스터에 0 을 써서 전원을 끊는다. 켤 때 쓴
	 * SCTRL_PCIE_CMOS_BIT 와 짝을 이룬다. */
	regmap_write(phy->sysctrl, SCTRL_PCIE_CMOS_OFFSET, 0x00);

	/* [한국어] 클럭 다섯 개를 끈다. enable=false 라 함수 안에서 곧바로 사다리로 뛴다. */
	hi3660_pcie_phy_clk_ctrl(phy, false);

	/* [한국어] 실패할 수 있는 동작이 없어 항상 성공이다. */
	return 0;
}

/*
 * The non-PHY part starts here
 */

/* [한국어] 위 상류 주석이 알리듯 여기부터가 두 세대(960/970)가 함께 쓰는 본체다.
 * 앞쪽 절반은 Kirin 960 전용 PHY 코드였고, 이후로는 phy_type 을 보는 두 곳을
 * 빼면 세대 구분이 나타나지 않는다.
 *
 * 아래 표는 "apb" 창을 regmap 으로 감쌀 때 쓸 설정이다. 32비트 레지스터가
 * 4바이트 간격으로 늘어서 있다는, MMIO 로서는 가장 평범한 배치를 선언한다. */
static const struct regmap_config pcie_kirin_regmap_conf = {
	/* [한국어] debugfs 등에 표시될 이 regmap 의 이름. */
	.name = "kirin_pcie_apb",
	/* [한국어] 레지스터 주소를 32비트로 표현한다. */
	.reg_bits = 32,
	/* [한국어] 레지스터 값이 32비트다. */
	.val_bits = 32,
	/* [한국어] 유효한 레지스터가 4바이트 간격으로 있다. 그래서 이 regmap 에
	 * 넘기는 오프셋은 4의 배수여야 한다 — 위쪽 SCTRL_PCIE_OE_OFFSET 의
	 * [상류 코드 관찰] 이 문제 삼는 것이 그 규칙이지만, 그 오프셋은 이 regmap 이
	 * 아니라 syscon 이 만든 다른 regmap 에 쓰인다. */
	.reg_stride = 4,
};

/* [한국어]
 * kirin_pcie_get_gpio_enable - 슬롯별 clkreq GPIO 들을 얻는다
 *
 * @pcie: 이 드라이버의 상태. n_gpio_clkreq 와 id_clkreq_gpio[] 를 채운다.
 * @pdev: 이 컨트롤러의 플랫폼 디바이스.
 * @return: 0 성공(GPIO 가 하나도 없어도 성공), 실패 시 음수.
 *
 * clkreq 는 하위 장치가 "기준 클럭이 필요하다" 고 알리는 신호인데, 이 보드에서는
 * 그것을 GPIO 로 대신 제어한다. 슬롯마다 클럭 소스가 다르다는 것이 파일 위쪽
 * 상류 주석이 설명한 HiKey 970 의 구성이다.
 *
 * 선택 속성이라는 점이 요점이다. gpiod_count() 가 음수를 돌려주면 "이 보드에는
 * 없다" 는 뜻이므로 0(성공)을 반환하고 넘어간다 — 그래서 이 함수가 실패로
 * 취급하는 것은 개수 초과와 조회 실패, 이름 할당 실패뿐이다.
 *
 * 얻은 GPIO 는 이 파일에서 kirin_pcie_power_off() 가 외부 PHY 갈래에서만
 * 쓴다. 즉 사실상 Kirin 970 전용 경로다.
 *
 * 실행 컨텍스트: probe. GPIO 조회는 잠들 수 있다.
 *
 * 에러 경로: dev_err_probe() 로 로그를 남기며 오류를 올린다. devm 이라
 * 되감을 것이 없다.
 *
 * 호출 체인:
 *   kirin_pcie_get_resource() → [이 함수]
 *     → gpiod_count(), devm_gpiod_get_index(), devm_kasprintf(),
 *       gpiod_set_consumer_name()
 */
static int kirin_pcie_get_gpio_enable(struct kirin_pcie *pcie,
				      struct platform_device *pdev)
{
	/* [한국어] GPIO 조회와 로그에 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] ret 는 개수이자 오류 코드, i 는 루프 인덱스. */
	int ret, i;

	/* This is an optional property */
	/* [한국어] "hisilicon,clken" 이름으로 선언된 GPIO 가 몇 개인지 센다.
	 * gpiod 계열 API 는 이 con_id 에 정해진 접미사를 붙여 DT 속성 이름을 만드는데,
	 * 그 규칙을 구현하는 drivers/gpio 는 이 트리에 없어 확인 못 함. */
	ret = gpiod_count(dev, "hisilicon,clken");
	/* [한국어] 음수는 "없다" 는 뜻이다. 옆의 상류 주석대로 선택 속성이므로 오류가 아니다. */
	if (ret < 0)
		/* [한국어] n_gpio_clkreq 는 0 으로 남고, power_off 의 순회가 한 번도 돌지 않는다. */
		return 0;

	/* [한국어] 배열 길이(MAX_PCI_SLOTS)를 넘으면 담을 곳이 없다. 이 검사는 아래
	 * 슬롯 PERST# 쪽 검사와 달리 경계가 정확하다 — 3 개까지 허용한다. */
	if (ret > MAX_PCI_SLOTS)
		/* [한국어] DT 가 잘못 쓰인 경우이므로 -EINVAL 로 프로브를 끊는다. */
		return dev_err_probe(dev, -EINVAL,
				     "Too many GPIO clock requests!\n");

	/* [한국어] 개수를 기록해 둔다. 이후 순회의 상한이 된다. */
	pcie->n_gpio_clkreq = ret;

	/* [한국어] 센 개수만큼 하나씩 얻는다. */
	for (i = 0; i < pcie->n_gpio_clkreq; i++) {
		/* [한국어] i 번째 clkreq GPIO 를 출력·초기값 0(low)으로 얻는다.
		 * 초기값이 0 이라 이 파일에 명시적으로 내리는 코드가 없어도 처음부터 내려가 있다. */
		pcie->id_clkreq_gpio[i] = devm_gpiod_get_index(dev,
							"hisilicon,clken", i,
							GPIOD_OUT_LOW);
		/* [한국어] 조회 실패. DT 표기가 잘못됐거나 GPIO 컨트롤러가 아직 없다. */
		if (IS_ERR(pcie->id_clkreq_gpio[i]))
			/* [한국어] -EPROBE_DEFER 면 로그를 줄여 주는 dev_err_probe 를 쓴다. */
			return dev_err_probe(dev, PTR_ERR(pcie->id_clkreq_gpio[i]),
					     "unable to get a valid clken gpio\n");

		/* [한국어] "pcie_clkreq_<인덱스>" 라는 소비자 이름을 만든다. 아래 슬롯 PERST#
		 * 이름이 DT 가 준 슬롯 번호를 쓰는 것과 달리, 이쪽은 배열 인덱스를 그대로 쓴다. */
		pcie->clkreq_names[i] = devm_kasprintf(dev, GFP_KERNEL,
						       "pcie_clkreq_%d", i);
		/* [한국어] 문자열 할당 실패. */
		if (!pcie->clkreq_names[i])
			/* [한국어] 메모리 부족은 로그 없이 그대로 올린다. */
			return -ENOMEM;

		/* [한국어] GPIO 에 이름을 붙인다. debugfs 의 gpio 목록에서 어느 소비자가
		 * 잡고 있는지 보이게 하려는 것이며, 동작에는 영향이 없다. */
		gpiod_set_consumer_name(pcie->id_clkreq_gpio[i],
					pcie->clkreq_names[i]);
	}

	/* [한국어] 하나도 없었든 다 얻었든 성공이다. */
	return 0;
}

/* [한국어]
 * kirin_pcie_parse_port - DT 를 두 단계 더 내려가 슬롯별 PERST# GPIO 를 모은다
 *
 * @pcie: 이 드라이버의 상태. num_slots, id_reset_gpio[], reset_names[] 를 채운다.
 * @pdev: 이 컨트롤러의 플랫폼 디바이스.
 * @node: 훑을 시작 노드. 호출자가 PCIe 노드의 자식 하나를 넘긴다.
 * @return: 0 성공, 실패 시 음수.
 *
 * 파일 위쪽 상류 주석이 설명한 HiKey 970 구성 — 외부 PEX 8606 브리지 아래
 * 슬롯마다 PERST# 핀이 따로 있는 배치 — 를 DT 에서 읽어 오는 함수다.
 *
 * 중첩이 깊다는 점을 눈여겨볼 것. 호출자가 이미 PCIe 노드의 자식을 순회하며
 * 이 함수를 부르고, 이 함수가 다시 두 단계를 더 내려간다. 결과적으로 PCIe
 * 노드 기준 세 단계 아래 노드에서 "reset" GPIO 를 찾는다.
 *
 * 슬롯 번호를 두 갈래로 다룬다는 점도 특징이다. 배열 인덱스는 발견 순서
 * (num_slots)이고, 이름에 박는 번호는 DT 의 reg 에서 뽑은 PCI 슬롯 번호다.
 * 그래서 인덱스와 이름의 숫자가 일치하지 않을 수 있다.
 *
 * [상류 코드 관찰] 상한 검사가 `pcie->num_slots + 1 >= MAX_PCI_SLOTS` 라,
 * 세 번째 슬롯을 찾은 시점(num_slots == 2)에 프로브가 -EINVAL 로 실패한다.
 * 배열 길이는 3 이고 위쪽 상류 주석은 슬롯이 셋인 보드를 예로 들지만, 실제로
 * 받아들여지는 것은 둘까지다. 또 그 검사가 id_reset_gpio[i] 에 값을 넣은
 * 뒤에 온다 — i 는 검사 전에도 2 를 넘지 않아 배열 범위를 벗어나지는 않는다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. GPIO 조회와 메모리 할당으로 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 dev_err_probe 로 알리며 올린다. 다만 "reset" 속성이
 * 없는 노드(-ENOENT)는 오류가 아니라 건너뛸 대상으로 다룬다 — 브리지 아래
 * 모든 자식이 슬롯인 것은 아니기 때문이다.
 *
 * 호출 체인:
 *   kirin_pcie_get_resource() → [이 함수]
 *     → devm_fwnode_gpiod_get_index(), of_pci_get_devfn(),
 *       devm_kasprintf(), gpiod_set_consumer_name()
 */
static int kirin_pcie_parse_port(struct kirin_pcie *pcie,
				 struct platform_device *pdev,
				 struct device_node *node)
{
	/* [한국어] GPIO 조회와 로그에 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] ret 는 하위 호출 결과, slot 은 DT 가 준 PCI 슬롯 번호,
	 * i 는 배열에 넣을 자리(발견 순서)다. */
	int ret, slot, i;

	/* [한국어] 첫째 단계 — 넘겨받은 노드의 사용 가능한 자식들을 훑는다.
	 * _scoped 판이라 루프를 빠져나갈 때 of_node_put 이 자동으로 불린다. */
	for_each_available_child_of_node_scoped(node, parent) {
		/* [한국어] 둘째 단계 — 그 자식의 자식들을 훑는다. 이 depth 에 있는 노드가
		 * 슬롯 후보다. */
		for_each_available_child_of_node_scoped(parent, child) {
			/* [한국어] 이번에 채울 배열 자리. 발견 순서 그대로다. */
			i = pcie->num_slots;

			/* [한국어] 이 슬롯 노드의 "reset" GPIO 를 출력·초기값 0 으로 얻는다.
			 * fwnode 판을 쓰는 것은 대상이 현재 장치가 아니라 DT 자손 노드이기 때문이다. */
			pcie->id_reset_gpio[i] = devm_fwnode_gpiod_get_index(dev,
							 of_fwnode_handle(child),
							 "reset", 0, GPIOD_OUT_LOW,
							 NULL);
			/* [한국어] 조회 실패. 다만 두 경우를 갈라야 한다. */
			if (IS_ERR(pcie->id_reset_gpio[i])) {
				/* [한국어] 속성이 아예 없다 = 이 노드는 PERST# 를 가진 슬롯이 아니다. */
				if (PTR_ERR(pcie->id_reset_gpio[i]) == -ENOENT)
					/* [한국어] 오류가 아니므로 다음 자식으로 넘어간다. num_slots 도 늘리지 않는다. */
					continue;
				/* [한국어] 그 밖의 실패는 진짜 오류다. */
				return dev_err_probe(dev, PTR_ERR(pcie->id_reset_gpio[i]),
						     "unable to get a valid reset gpio\n");
			}

			/* [한국어] 위 [상류 코드 관찰] 이 가리키는 상한 검사. GPIO 를 이미 배열에
			 * 넣은 뒤에 온다. */
			if (pcie->num_slots + 1 >= MAX_PCI_SLOTS)
				/* [한국어] 담을 수 있는 수를 넘었다는 뜻으로 프로브를 끊는다. */
				return dev_err_probe(dev, -EINVAL,
						     "Too many PCI slots!\n");

			/* [한국어] 이 슬롯을 셈에 넣는다. add_bus 가 이 값만큼 PERST# 를 내보낸다. */
			pcie->num_slots++;

			/* [한국어] DT 의 reg 에서 이 노드의 devfn(장치·기능 번호)을 뽑는다. */
			ret = of_pci_get_devfn(child);
			/* [한국어] reg 가 없거나 형식이 틀리다. */
			if (ret < 0)
				/* [한국어] 이름을 만들 수 없으므로 프로브를 끊는다. */
				return dev_err_probe(dev, ret,
						     "failed to parse devfn\n");

			/* [한국어] devfn 에서 상위 5비트를 뽑아 슬롯 번호로 삼는다. 이 번호가
			 * 배열 인덱스가 아니라 이름에 들어간다. */
			slot = PCI_SLOT(ret);

			/* [한국어] "pcie_perst_<슬롯번호>" 라는 소비자 이름을 만든다. 오류 로그에
			 * 어느 슬롯인지 드러나게 하는 것이 목적이다(add_bus 가 이것을 쓴다). */
			pcie->reset_names[i] = devm_kasprintf(dev, GFP_KERNEL,
							      "pcie_perst_%d",
							      slot);
			/* [한국어] 문자열 할당 실패. */
			if (!pcie->reset_names[i])
				/* [한국어] 메모리 부족을 그대로 올린다. */
				return -ENOMEM;

			/* [한국어] GPIO 에 그 이름을 붙인다. */
			gpiod_set_consumer_name(pcie->id_reset_gpio[i],
						pcie->reset_names[i]);
		}
	}

	/* [한국어] 모든 자손을 훑었다. 슬롯이 하나도 없어도 성공이다 — 슬롯 확장이
	 * 없는 보드가 정상적으로 존재하기 때문이다. */
	return 0;
}

/* [한국어]
 * kirin_pcie_get_resource - "apb" 창과 GPIO 들을 모두 확보한다
 *
 * @kirin_pcie: 이 드라이버의 상태.
 * @pdev: 이 컨트롤러의 플랫폼 디바이스.
 * @return: 0 성공, 실패 시 음수.
 *
 * probe 의 자원 확보 단계 전체다. 얻는 것이 넷이다.
 *   1. "apb" 레지스터 창 → regmap 으로 감싼다.
 *   2. 컨트롤러 자신의 PERST# GPIO("reset").
 *   3. clkreq GPIO 들(선택).
 *   4. DT 자식들을 훑어 슬롯별 PERST# GPIO 들.
 *
 * DBI 창을 여기서 얻지 않는다는 점이 중요하다. 그것은 나중에 DWC 코어의
 * dw_pcie_get_resources() 가 얻는다.
 *
 * [상류 코드 관찰] 반환 타입이 long 이다. PTR_ERR() 이 long 을 돌려주기
 * 때문으로 보이는데, 호출자 kirin_pcie_probe() 는 그것을 int 변수로 받는다.
 * 오류 코드 범위에서는 잘림이 문제가 되지 않지만 타입이 어긋나 있는 것은
 * 사실이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 그대로 올린다. 모두 devm 이라 되감을 것이 없다.
 *
 * 호출 체인:
 *   kirin_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname(), devm_regmap_init_mmio(),
 *       devm_gpiod_get(), kirin_pcie_get_gpio_enable(), kirin_pcie_parse_port()
 */
static long kirin_pcie_get_resource(struct kirin_pcie *kirin_pcie,
				    struct platform_device *pdev)
{
	/* [한국어] 자원 조회와 로그에 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. 아래에서 자식들을 훑는 출발점이다. */
	struct device_node *node = dev->of_node;
	/* [한국어] regmap 으로 감싸기 전의 생 매핑 주소. 이 지역 변수 밖으로 나가지 않는다. */
	void __iomem *apb_base;
	/* [한국어] 하위 호출들의 결과. */
	int ret;

	/* [한국어] "apb" 라는 이름의 reg 항목을 찾아 매핑한다. LTSSM, 링크 상태,
	 * 사이드밴드 스위치가 모두 이 창에 있다. */
	apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");
	/* [한국어] 자원이 없거나 매핑 실패. */
	if (IS_ERR(apb_base))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(apb_base);

	/* [한국어] 매핑한 창을 regmap 으로 감싼다. 이후 이 파일의 모든 apb 접근이
	 * regmap_read/write 를 거친다 — 생 iomem 을 직접 쓰는 곳은 없다. */
	kirin_pcie->apb = devm_regmap_init_mmio(dev, apb_base,
						&pcie_kirin_regmap_conf);
	/* [한국어] regmap 생성 실패. */
	if (IS_ERR(kirin_pcie->apb))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(kirin_pcie->apb);

	/* pcie internal PERST# gpio */
	/* [한국어] 컨트롤러 자신의 PERST# GPIO 를 출력·초기값 0 으로 얻는다.
	 * optional 이 아니라, DT 에 없으면 프로브가 실패한다. 옆의 상류 주석이
	 * 이것을 "pcie internal PERST#" 라고 부른 것은 아래 슬롯별 PERST# 와
	 * 구분하기 위해서다. */
	kirin_pcie->id_dwc_perst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	/* [한국어] 조회 실패. */
	if (IS_ERR(kirin_pcie->id_dwc_perst_gpio))
		/* [한국어] -EPROBE_DEFER 를 조용히 처리해 주는 dev_err_probe 를 쓴다. */
		return dev_err_probe(dev, PTR_ERR(kirin_pcie->id_dwc_perst_gpio),
				     "unable to get a valid gpio pin\n");
	/* [한국어] 이 GPIO 에 "pcie_perst_bridge" 라는 이름을 붙인다. 슬롯별 이름이
	 * "pcie_perst_<번호>" 인 것과 구분되도록 지은 이름이다. */
	gpiod_set_consumer_name(kirin_pcie->id_dwc_perst_gpio, "pcie_perst_bridge");

	/* [한국어] clkreq GPIO 들을 얻는다. 없으면 그냥 0 을 돌려주므로 실패가 아니다. */
	ret = kirin_pcie_get_gpio_enable(kirin_pcie, pdev);
	/* [한국어] 개수 초과나 조회 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. */
		return ret;

	/* Parse OF children */
	/* [한국어] 이 노드의 사용 가능한 자식을 하나씩 parse_port 에 넘긴다.
	 * parse_port 가 다시 두 단계를 더 내려가므로, 실제 슬롯 노드는 여기서
	 * 세 단계 아래에 있다. */
	for_each_available_child_of_node_scoped(node, child) {
		/* [한국어] 이 자식 아래에서 슬롯별 PERST# 를 모은다. */
		ret = kirin_pcie_parse_port(kirin_pcie, pdev, child);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 그대로 올린다. _scoped 매크로가 노드 참조를 알아서 놓아 준다. */
			return ret;
	}

	/* [한국어] 네 가지 자원을 모두 확보했다. */
	return 0;
}

/* [한국어]
 * kirin_pcie_sideband_dbi_w_mode - DBI "쓰기" 사이드밴드를 켜거나 끈다
 *
 * @kirin_pcie: 이 드라이버의 상태.
 * @on: true 면 켜기, false 면 끄기.
 * @return: 없음.
 *
 * 이 하드웨어에서 DBI 창은 그냥 접근할 수 있는 것이 아니라, ELBI 레지스터의
 * 인에이블 비트가 켜져 있는 동안에만 통한다. 이 함수가 그 스위치의 쓰기 쪽이다.
 *
 * 그래서 이 드라이버는 DWC 코어의 write_dbi 훅을 걸어, 모든 DBI 쓰기를
 * "켜기 → 쓰기 → 끄기" 로 감싼다. 접근이 끝나면 곧바로 끄는 이유는 이 파일에
 * 적혀 있지 않다.
 *
 * 읽기 쪽은 다른 레지스터(CTRL1)를 쓰는 별도 함수다 — 같은 비트 자리를
 * 쓰지만 레지스터가 갈라져 있다.
 *
 * 실행 컨텍스트: DBI 쓰기가 일어나는 모든 곳. regmap 접근이라 잠들 수 있다.
 *
 * 에러 경로: regmap 반환값을 보지 않는다. 원본 스냅숏에서도 같다.
 *
 * 호출 체인:
 *   kirin_pcie_write_dbi() → [이 함수] → regmap_read(), regmap_write()
 */
static void kirin_pcie_sideband_dbi_w_mode(struct kirin_pcie *kirin_pcie,
					   bool on)
{
	/* [한국어] 읽기-수정-쓰기용 임시 값. 다른 비트를 보존해야 한다. */
	u32 val;

	/* [한국어] 컨트롤 0 의 현재 값을 읽는다. */
	regmap_read(kirin_pcie->apb, SOC_PCIECTRL_CTRL0_ADDR, &val);
	/* [한국어] 켜라는 요청. */
	if (on)
		/* [한국어] 인에이블 비트(비트 21)를 세운다. */
		val = val | PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄라는 요청. */
	else
		/* [한국어] 같은 비트를 지운다. */
		val = val & ~PCIE_ELBI_SLV_DBI_ENABLE;

	/* [한국어] 고친 값을 되쓴다. 이 쓰기가 끝나야 DBI 접근이 유효해진다. */
	regmap_write(kirin_pcie->apb, SOC_PCIECTRL_CTRL0_ADDR, val);
}

/* [한국어]
 * kirin_pcie_sideband_dbi_r_mode - DBI "읽기" 사이드밴드를 켜거나 끈다
 *
 * @kirin_pcie: 이 드라이버의 상태.
 * @on: true 면 켜기, false 면 끄기.
 * @return: 없음.
 *
 * 바로 위 쓰기 판과 짜임이 똑같고 레지스터만 다르다 — 이쪽은
 * SOC_PCIECTRL_CTRL1_ADDR 이다. 인에이블 비트 자리는 같다.
 *
 * 읽기와 쓰기의 스위치를 따로 둔 것이 이 하드웨어의 설계이며, 그 때문에
 * 이 드라이버는 read_dbi 와 write_dbi 훅을 둘 다 걸어야 한다. 둘 중 하나만
 * 걸면 다른 방향의 접근이 사이드밴드 없이 나가게 된다.
 *
 * 실행 컨텍스트: DBI 읽기가 일어나는 모든 곳. regmap 접근이라 잠들 수 있다.
 *
 * 에러 경로: regmap 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   kirin_pcie_read_dbi() → [이 함수] → regmap_read(), regmap_write()
 */
static void kirin_pcie_sideband_dbi_r_mode(struct kirin_pcie *kirin_pcie,
					   bool on)
{
	/* [한국어] 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] 컨트롤 1 의 현재 값을 읽는다. 쓰기 판과 다른 유일한 지점이다. */
	regmap_read(kirin_pcie->apb, SOC_PCIECTRL_CTRL1_ADDR, &val);
	/* [한국어] 켜라는 요청. */
	if (on)
		/* [한국어] 인에이블 비트를 세운다. */
		val = val | PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄라는 요청. */
	else
		/* [한국어] 같은 비트를 지운다. */
		val = val & ~PCIE_ELBI_SLV_DBI_ENABLE;

	/* [한국어] 되쓴다. */
	regmap_write(kirin_pcie->apb, SOC_PCIECTRL_CTRL1_ADDR, val);
}

/* [한국어]
 * kirin_pcie_rd_own_conf - 루트 포트 자신의 설정공간을 읽는다 (pci_ops.read)
 *
 * @bus: 접근 대상 버스. 이 ops 가 걸린 것은 루트 버스뿐이다.
 * @devfn: 장치·기능 번호.
 * @where: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @val: 읽은 값을 넣을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * DWC 코어의 기본 경로와 다른 점이 이 함수의 존재 이유다. 기본 경로는
 * dw_pcie_own_conf_map_bus() 가 DBI 주소를 돌려주면 pci_generic_config_read()
 * 가 그 주소에 생 MMIO 접근을 한다. 그 구조에는 사이드밴드를 켜고 끄는
 * 단계를 끼워 넣을 자리가 없다.
 *
 * 그래서 이 드라이버는 map_bus 대신 .read/.write 를 직접 구현하고,
 * dw_pcie_read_dbi() 를 거치게 한다. 그것이 다시 이 파일의 read_dbi 훅으로
 * 내려와 사이드밴드를 켜고 끈다. 즉 이 한 줄이 사이드밴드 처리로 이어지는
 * 통로다.
 *
 * 슬롯 검사는 DWC 기본 경로와 같은 이유다 — 루트 버스에는 이 루트 포트
 * 하나만 있으므로, 막지 않으면 열거가 같은 DBI 영역을 여러 장치로 착각한다.
 *
 * 실행 컨텍스트: 설정공간 열거와 그 뒤의 모든 config 접근. 하위 regmap 때문에
 * 잠들 수 있다.
 *
 * 에러 경로: 슬롯이 0 이 아니면 "장치 없음" 을 돌려준다. 읽기 자체의 실패는
 * dw_pcie_read_dbi() 가 로그로만 남기므로 여기까지 올라오지 않는다.
 *
 * 호출 체인:
 *   PCI 코어의 설정공간 접근 → bus->ops->read → [이 함수]
 *     → dw_pcie_read_dbi() → pci->ops->read_dbi → kirin_pcie_read_dbi()
 */
static int kirin_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	/* [한국어] bus->sysdata 는 DWC 코어가 심어 둔 루트 포트 문맥이다. 거기서
	 * 컨트롤러 문맥으로 올라간다.
	 * 다만 그 전제는 DWC 가 ECAM 이 아닌 경로를 골랐을 때만 성립한다 —
	 * ECAM 경로에서는 sysdata 가 pci_config_window 로 바뀐다
	 * (pcie-designware-host.c 에서 확인). 이 파일은 native_ecam 을 켜지도,
	 * ecam_enabled 를 확인하지도 않는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	/* [한국어] 루트 버스에는 장치 0 하나뿐이다. */
	if (PCI_SLOT(devfn))
		/* [한국어] 그 밖의 슬롯은 없는 것으로 답해 유령 장치가 열거되지 않게 한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 루트 포트 설정공간은 IP 내부의 DBI 창에 그대로 노출되어 있다.
	 * 이 호출이 사이드밴드 처리를 포함한 경로로 이어진다. */
	*val = dw_pcie_read_dbi(pci, where, size);
	/* [한국어] PCI 코어에 성공을 알린다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * kirin_pcie_wr_own_conf - 루트 포트 자신의 설정공간에 쓴다 (pci_ops.write)
 *
 * @bus: 접근 대상 버스(루트 버스).
 * @devfn: 장치·기능 번호.
 * @where: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 바로 위 읽기 판의 짝이며 구조가 같다. 다른 점은 dw_pcie_write_dbi() 를
 * 부른다는 것뿐이고, 그것이 write_dbi 훅으로 내려가 "쓰기" 쪽 사이드밴드
 * (CTRL0)를 켰다 끈다.
 *
 * 실행 컨텍스트: 설정공간 쓰기가 일어나는 모든 곳. 잠들 수 있다.
 *
 * 에러 경로: 슬롯 검사만 있다.
 *
 * 호출 체인:
 *   PCI 코어의 설정공간 접근 → bus->ops->write → [이 함수]
 *     → dw_pcie_write_dbi() → pci->ops->write_dbi → kirin_pcie_write_dbi()
 */
static int kirin_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	/* [한국어] 읽기 판과 같은 두 단계로 컨트롤러 문맥을 되찾는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	/* [한국어] 루트 버스에는 장치 0 하나뿐이다. */
	if (PCI_SLOT(devfn))
		/* [한국어] 그 밖은 없는 것으로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] DBI 창에 쓴다. 사이드밴드 처리는 하위 훅이 맡는다. */
	dw_pcie_write_dbi(pci, where, size, val);
	/* [한국어] 성공을 알린다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * kirin_pcie_add_bus - 버스가 만들어질 때 슬롯마다 PERST# 를 내보낸다 (pci_ops.add_bus)
 *
 * @bus: 방금 만들어진 버스.
 * @return: 항상 0. 실패해도 스캔을 막지 않는다.
 *
 * DWC 기본 pci_ops 에는 없는 훅이다. 이 드라이버가 따로 단 이유는, 파일 위쪽
 * 상류 주석이 설명한 대로 외부 브리지 아래 슬롯마다 PERST# 핀이 따로 있어
 * 컨트롤러의 PERST# 하나로는 부족하기 때문이다.
 *
 * 시점이 요점이다. PCI 코어가 버스를 만든 직후 이 훅을 부르므로(probe.c 에서
 * 확인), 그 버스 위의 장치를 열거하기 전에 리셋 신호를 내보내고 안정화 시간을
 * 확보할 수 있다. probe 단계에서 미리 하면 버스가 아직 없어 시점이 어긋난다.
 *
 * [상류 코드 관찰] GPIO 조작이 실패해도 로그만 남기고 계속 진행하며, 함수는
 * 언제나 0 을 돌려준다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지
 * 않았다. 반환값을 0 이 아닌 값으로 하면 PCI 코어가 그 버스 스캔을 멈추므로,
 * 일부 슬롯이 실패해도 나머지는 살리려는 선택으로 읽힌다.
 *
 * 실행 컨텍스트: 버스 스캔. usleep_range 로 잠든다.
 *
 * 에러 경로: 위 관찰대로 로그만 남긴다.
 *
 * 호출 체인:
 *   pci_scan_child_bus() 계열 → bus->ops->add_bus → [이 함수]
 *     → gpiod_direction_output_raw(), usleep_range()
 */
static int kirin_pcie_add_bus(struct pci_bus *bus)
{
	/* [한국어] 버스의 sysdata 에서 컨트롤러 문맥으로 올라간다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);
	/* [한국어] 거기서 drvdata 를 거쳐 이 드라이버의 상태로 온다. */
	struct kirin_pcie *kirin_pcie = to_kirin_pcie(pci);
	/* [한국어] i 는 슬롯 인덱스, ret 는 GPIO 조작 결과. */
	int i, ret;

	/* [한국어] 슬롯이 하나도 없는 보드다 — 외부 브리지가 없는 구성. */
	if (!kirin_pcie->num_slots)
		/* [한국어] 할 일이 없으므로 곧바로 성공을 알린다. */
		return 0;

	/* Send PERST# to each slot */
	/* [한국어] DT 에서 찾은 슬롯 수만큼 순회한다. */
	for (i = 0; i < kirin_pcie->num_slots; i++) {
		/* [한국어] PERST# 를 raw 1 로 올린다. _raw 판은 DT 의 active-low 표기를 무시하고
		 * 핀의 물리 레벨을 그대로 지정한다 — 그래서 DT 극성 표기와 무관하게 동작한다. */
		ret = gpiod_direction_output_raw(kirin_pcie->id_reset_gpio[i], 1);
		/* [한국어] 실패. */
		if (ret) {
			/* [한국어] 어느 슬롯인지 이름으로 남긴다. reset_names[] 를 로그에 쓰는
			 * 유일한 곳이다. 위 [상류 코드 관찰] 대로 계속 진행한다. */
			dev_err(pci->dev, "PERST# %s error: %d\n",
				kirin_pcie->reset_names[i], ret);
		}
	}
	/* [한국어] 모든 슬롯의 PERST# 를 낸 뒤 10~12ms 기다린다. 이 대기가 지나야
	 * 아래 장치들의 설정공간 접근이 유효하다. 상수 이름(PERST_2_ACCESS)이 그
	 * 의미를 그대로 담고 있다. */
	usleep_range(PERST_2_ACCESS_MIN, PERST_2_ACCESS_MAX);

	/* [한국어] 위 관찰대로 언제나 성공을 알린다. */
	return 0;
}

/* [한국어] 루트 버스의 설정공간 접근 방식을 정하는 표. kirin_pcie_host_init()
 * 이 DWC 기본 표를 이것으로 갈아 끼운다.
 *
 * DWC 기본 표(pcie-designware-host.c 의 정적 dw_pcie_ops)와 짜임이 다르다는
 * 점이 핵심이다. 기본 표는 map_bus 하나로 DBI 주소를 돌려주고 읽기·쓰기를
 * 공용 헬퍼에 맡기지만, 이 표는 map_bus 없이 read/write 를 직접 구현한다 —
 * 사이드밴드를 켜고 끄는 단계를 끼워 넣어야 하기 때문이다.
 * 여기에 add_bus 가 하나 더 있다.
 *
 * 하위 버스는 이 표가 아니라 DWC 가 걸어 둔 child_ops 가 맡는다. 이 표를
 * 갈아 끼워도 그쪽은 건드리지 않으므로, 하위 장치의 설정공간 접근은
 * 여전히 DWC 의 iATU 경로로 나간다. */
static struct pci_ops kirin_pci_ops = {
	/* [한국어] 루트 포트 설정공간 읽기. DBI 로 돌린다. */
	.read = kirin_pcie_rd_own_conf,
	/* [한국어] 루트 포트 설정공간 쓰기. */
	.write = kirin_pcie_wr_own_conf,
	/* [한국어] 버스 생성 시 슬롯 PERST# 를 내보낸다. DWC 기본 표에는 없는 항목이다. */
	.add_bus = kirin_pcie_add_bus,
};

/* [한국어]
 * kirin_pcie_read_dbi - 사이드밴드를 켜고 DBI 를 읽은 뒤 끈다 (dw_pcie_ops.read_dbi)
 *
 * @pci: DWC 코어의 컨트롤러 문맥.
 * @base: 읽을 창의 시작 주소. 코어가 pci->dbi_base 를 넘긴다.
 * @reg: 그 창 안의 오프셋.
 * @size: 1/2/4 바이트.
 * @return: 읽은 값.
 *
 * 이 드라이버가 DWC 코어에 read_dbi 훅을 거는 유일한 이유가 이 세 줄이다.
 * 훅이 없으면 코어는 dw_pcie_read() 로 생 MMIO 접근을 하는데, 이 하드웨어에서는
 * 사이드밴드가 꺼진 상태의 DBI 접근이 통하지 않는다.
 *
 * 접근 직후 다시 끄는 것이 특징이다. 켠 채로 두면 무엇이 문제인지는 이 파일에
 * 적혀 있지 않다.
 *
 * [상류 코드 관찰] dw_pcie_read() 의 반환값을 버린다 — 정렬이나 크기가
 * 잘못되면 그 함수가 오류를 돌려주지만 여기서는 확인하지 않고, 그 경우 ret 가
 * 초기화되지 않은 채 반환될 수 있다. 원본 스냅숏(1f0e418bb6)에서 확인했으며
 * 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 모든 DBI 읽기. regmap 때문에 잠들 수 있다.
 * [상류 코드 관찰] 사이드밴드 켜고 끄기를 감싸는 잠금이 없다. 두 스레드가
 * 동시에 DBI 를 읽으면 한쪽이 끄는 사이 다른 쪽이 접근할 수 있는 구조이나,
 * 실제로 그런 동시 접근이 일어나는지는 이 트리만으로 단정할 수 없다.
 *
 * 에러 경로: 위 관찰대로 없다.
 *
 * 호출 체인:
 *   dw_pcie_read_dbi() → pci->ops->read_dbi → [이 함수]
 *     → kirin_pcie_sideband_dbi_r_mode(), dw_pcie_read()
 */
static u32 kirin_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
			       u32 reg, size_t size)
{
	/* [한국어] drvdata 를 거쳐 이 드라이버의 상태를 되찾는다. apb regmap 이 필요해서다. */
	struct kirin_pcie *kirin_pcie = to_kirin_pcie(pci);
	/* [한국어] 읽은 값을 담을 곳. 아래 dw_pcie_read 가 채운다. */
	u32 ret;

	/* [한국어] 읽기 사이드밴드를 켠다. 이 순간부터 DBI 읽기가 통한다. */
	kirin_pcie_sideband_dbi_r_mode(kirin_pcie, true);
	/* [한국어] 실제 읽기. base 는 코어가 넘긴 dbi_base 이고 reg 를 더해 주소를 만든다. */
	dw_pcie_read(base + reg, size, &ret);
	/* [한국어] 곧바로 끈다. 켠 상태를 남기지 않는 것이 이 드라이버의 규칙이다. */
	kirin_pcie_sideband_dbi_r_mode(kirin_pcie, false);

	/* [한국어] 읽은 값을 코어에 돌려준다. */
	return ret;
}

/* [한국어]
 * kirin_pcie_write_dbi - 사이드밴드를 켜고 DBI 에 쓴 뒤 끈다 (dw_pcie_ops.write_dbi)
 *
 * @pci: DWC 코어의 컨트롤러 문맥.
 * @base: 쓸 창의 시작 주소(코어가 pci->dbi_base 를 넘긴다).
 * @reg: 그 창 안의 오프셋.
 * @size: 1/2/4 바이트.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 위 읽기 판의 짝이다. 다른 점은 CTRL0 쪽 스위치(쓰기 사이드밴드)를 쓴다는
 * 것과, 반환값이 없다는 것뿐이다.
 *
 * 읽기와 쓰기의 스위치가 다른 레지스터라는 하드웨어 사정 때문에 훅도 둘로
 * 나뉜다 — 하나만 걸면 다른 방향이 사이드밴드 없이 나가게 된다.
 *
 * 실행 컨텍스트: 모든 DBI 쓰기. 잠들 수 있다. 잠금이 없는 것은 읽기 판과 같다.
 *
 * 에러 경로: dw_pcie_write() 의 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_write_dbi() → pci->ops->write_dbi → [이 함수]
 *     → kirin_pcie_sideband_dbi_w_mode(), dw_pcie_write()
 */
static void kirin_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size, u32 val)
{
	/* [한국어] apb regmap 에 닿기 위해 이 드라이버의 상태를 되찾는다. */
	struct kirin_pcie *kirin_pcie = to_kirin_pcie(pci);

	/* [한국어] 쓰기 사이드밴드를 켠다. 읽기 쪽과 다른 레지스터(CTRL0)다. */
	kirin_pcie_sideband_dbi_w_mode(kirin_pcie, true);
	/* [한국어] 실제 쓰기. */
	dw_pcie_write(base + reg, size, val);
	/* [한국어] 곧바로 끈다. */
	kirin_pcie_sideband_dbi_w_mode(kirin_pcie, false);
}

/* [한국어]
 * kirin_pcie_link_up - 링크가 서 있는지 DWC 코어에 답한다 (dw_pcie_ops.link_up)
 *
 * @pci: DWC 코어의 컨트롤러 문맥.
 * @return: true 면 링크가 섰다.
 *
 * 코어가 링크를 기다릴 때 반복해 부르는 콜백이다. apb 창의 PHY 상태
 * 레지스터를 읽어 정해진 두 비트가 모두 서 있는지 본다.
 *
 * 부분 일치가 아니라 동등 비교라는 점이 요점이다. 마스크 결과를 그대로
 * 반환하면 한쪽 비트만 선 중간 상태도 링크 업으로 오판하게 된다.
 *
 * 각 비트가 무엇을 뜻하는지는 이 트리에서 확인 못 함. 다른 DWC 글루들이
 * 물리 계층(SMLH)과 데이터 링크 계층(RDLH) 두 비트를 함께 보는 것과 같은
 * 짜임으로 보이나, 이 파일에는 그 이름이 없다.
 *
 * 실행 컨텍스트: 초기화 경로. regmap 때문에 잠들 수 있다.
 *
 * 에러 경로: regmap_read 의 반환값을 보지 않는다. 실패하면 val 이 초기화되지
 * 않은 채 판정에 쓰인다.
 *
 * 호출 체인:
 *   dw_pcie_link_up() / dw_pcie_wait_for_link() → pci->ops->link_up → [이 함수]
 *     → regmap_read()
 */
static bool kirin_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] apb regmap 에 닿기 위해 상태를 되찾는다. */
	struct kirin_pcie *kirin_pcie = to_kirin_pcie(pci);
	/* [한국어] 읽은 상태 값. */
	u32 val;

	/* [한국어] apb 창의 PHY 상태 레지스터를 읽는다. 위쪽 "phy" 창의 같은 이름과
	 * 헷갈리지 말 것 — 그 [상류 코드 관찰] 참조. */
	regmap_read(kirin_pcie->apb, PCIE_APB_PHY_STATUS0, &val);
	/* [한국어] 두 비트가 모두 서 있을 때만 참이다. */
	return (val & PCIE_LINKUP_ENABLE) == PCIE_LINKUP_ENABLE;
}

/* [한국어]
 * kirin_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작한다 (dw_pcie_ops.start_link)
 *
 * @pci: DWC 코어의 컨트롤러 문맥.
 * @return: 항상 0.
 *
 * dw_pcie_host_init() 이 dw_pcie_setup_rc() 를 마치고 링크가 아직 서 있지
 * 않을 때 부른다.
 *
 * 하는 일이 한 줄뿐인 것이 이 드라이버의 특징이다. 속도 변경을 손으로
 * 반복시키는 글루도 있지만, 여기서는 LTSSM 비트만 켜고 나머지는 하드웨어와
 * DWC 코어에 맡긴다. 짝이 되는 stop_link 훅도 두지 않았다 — 즉 이 드라이버는
 * 링크를 내리는 경로를 제공하지 않는다.
 *
 * 읽기-수정-쓰기가 아니라 값을 통째로 쓴다. 그래서 이 레지스터의 다른 비트는
 * 모두 0 이 되는데, 이 레지스터에 다른 비트가 있는지는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 초기화 경로. regmap 때문에 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환 타입이 int 인 것은 콜백 원형을 맞추기 위해서다.
 *
 * 호출 체인:
 *   dw_pcie_start_link() → pci->ops->start_link → [이 함수] → regmap_write()
 */
static int kirin_pcie_start_link(struct dw_pcie *pci)
{
	/* [한국어] apb regmap 에 닿기 위해 상태를 되찾는다. */
	struct kirin_pcie *kirin_pcie = to_kirin_pcie(pci);

	/* assert LTSSM enable */
	/* [한국어] LTSSM 시작 비트만 담긴 값을 통째로 쓴다. 옆의 상류 주석대로
	 * 이 쓰기가 링크 훈련의 시작이다. */
	regmap_write(kirin_pcie->apb, PCIE_APP_LTSSM_ENABLE,
		     PCIE_LTSSM_ENABLE_BIT);

	/* [한국어] 실패할 수 있는 동작이 없으므로 항상 성공이다. */
	return 0;
}

/* [한국어]
 * kirin_pcie_host_init - 루트 버스의 설정공간 접근 방식을 갈아 끼운다
 *
 * @pp: DWC 코어의 루트 포트 문맥.
 * @return: 항상 0.
 *
 * dw_pcie_host_ops.init 콜백이며, 이 드라이버가 거는 유일한 호스트 훅이다.
 * 다른 글루들이 이 자리에서 클럭·리셋·PHY 를 만지는 것과 달리, 여기서는
 * 그 일을 probe 가 dw_pcie_host_init() 을 부르기 전에 이미 끝내 두었다.
 * 그래서 남은 일이 한 줄뿐이다.
 *
 * 이 한 줄이 중요한 이유는 시점 때문이다. DWC 코어는 이 콜백을 부르기 전에
 * 이미 pp->bridge->ops 를 자기 기본 표로 채워 두었다
 * (pcie-designware-host.c 에서 확인). 그것을 되돌려 놓을 자리가 이 콜백뿐이라,
 * 여기서 kirin_pci_ops 로 덮어쓴다. probe 에서 미리 써 두면 코어가 나중에
 * 덮어써 버린다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pp->ops->init → [이 함수]
 */
static int kirin_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 루트 버스의 설정공간 접근 표를 이 파일의 것으로 바꾼다.
	 * child_ops 는 건드리지 않으므로 하위 버스는 여전히 DWC 경로를 쓴다. */
	pp->bridge->ops = &kirin_pci_ops;

	/* [한국어] 실패할 수 있는 동작이 없다. */
	return 0;
}

/* [한국어] DWC 코어가 부르는 코어 콜백 표. 넷이 걸려 있는데, 그중 둘
 * (read_dbi/write_dbi)이 이 드라이버가 존재하는 가장 큰 이유다 — 사이드밴드
 * 없이는 DBI 접근이 통하지 않기 때문이다.
 *
 * stop_link 가 없다는 점을 눈여겨볼 것. 즉 이 드라이버는 링크를 내리는 경로를
 * 제공하지 않으며, 정리 때는 PHY 전원을 끊는 것으로 대신한다. */
static const struct dw_pcie_ops kirin_dw_pcie_ops = {
	/* [한국어] DBI 읽기를 사이드밴드로 감싼다. */
	.read_dbi = kirin_pcie_read_dbi,
	/* [한국어] DBI 쓰기를 사이드밴드로 감싼다. 읽기와 다른 레지스터를 쓴다. */
	.write_dbi = kirin_pcie_write_dbi,
	/* [한국어] 링크 상태 질의. */
	.link_up = kirin_pcie_link_up,
	/* [한국어] 링크 훈련 시작. */
	.start_link = kirin_pcie_start_link,
};

/* [한국어] 호스트 코어가 부르는 훅 표. 하나뿐이며, 그 하나도 하드웨어를
 * 만지지 않고 pci_ops 를 갈아 끼우기만 한다 — 이 드라이버는 하드웨어 준비를
 * dw_pcie_host_init() 호출 전에 probe 에서 이미 끝내 두기 때문이다. */
static const struct dw_pcie_host_ops kirin_pcie_host_ops = {
	/* [한국어] 루트 버스 pci_ops 교체. deinit 짝은 두지 않았고, 정리는
	 * kirin_pcie_remove() 가 직접 한다. */
	.init = kirin_pcie_host_init,
};

/* [한국어]
 * kirin_pcie_power_off - PHY 를 끈다. 두 세대가 갈리는 두 곳 중 하나
 *
 * @kirin_pcie: 이 드라이버의 상태.
 * @return: 항상 0. 내장 PHY 갈래에서도 그 함수가 0 만 돌려준다.
 *
 * kirin_pcie_power_on() 의 되감기이며, 이 파일에서 phy_type 을 보는 두 곳
 * 중 하나다.
 *   - 내장 PHY(960): hi3660_pcie_phy_power_off() 하나로 끝난다.
 *   - 외부 PHY(970): clkreq GPIO 들을 모두 raw 1 로 올린 뒤 PHY 드라이버에
 *     phy_power_off()/phy_exit() 를 위임한다.
 *
 * clkreq 처리가 970 갈래에만 있다는 점이 두 세대의 차이를 드러낸다. 960 은
 * 클럭을 이 파일이 직접 다루므로 GPIO 로 요청할 일이 없다.
 *
 * power_on 과 대칭이 아니라는 점도 유의할 것 — power_on 이 마지막에 낸
 * 컨트롤러 PERST#(id_dwc_perst_gpio)를 여기서 되돌리지 않는다.
 *
 * 실행 컨텍스트: probe 실패 되감기 또는 remove. 잠들 수 있다.
 *
 * 에러 경로: GPIO 와 PHY 호출의 반환값을 모두 무시한다. 정리 경로라 실패해도
 * 할 수 있는 일이 없기 때문이다.
 *
 * 호출 체인:
 *   kirin_pcie_power_on()(실패 시) / kirin_pcie_remove() → [이 함수]
 *     → hi3660_pcie_phy_power_off() 또는
 *       gpiod_direction_output_raw(), phy_power_off(), phy_exit()
 */
static int kirin_pcie_power_off(struct kirin_pcie *kirin_pcie)
{
	/* [한국어] clkreq GPIO 순회용 인덱스. */
	int i;

	/* [한국어] 세대 갈림. 내장 PHY 면 이 파일의 hi3660_ 절차가 맡는다. */
	if (kirin_pcie->type == PCIE_KIRIN_INTERNAL_PHY)
		/* [한국어] 전원을 끊고 클럭 다섯 개를 끈다. 아래 외부 PHY 코드는 지나지 않는다. */
		return hi3660_pcie_phy_power_off(kirin_pcie);

	/* [한국어] 여기부터 외부 PHY(970) 갈래다. clkreq 를 먼저 정리한다. */
	for (i = 0; i < kirin_pcie->n_gpio_clkreq; i++)
		/* [한국어] clkreq 를 raw 1 로 올린다. GPIOD_OUT_LOW 로 얻어 0 이었던 것을
		 * 반대로 돌리는 것이며, _raw 라 DT 극성 표기와 무관하게 물리 레벨을 지정한다. */
		gpiod_direction_output_raw(kirin_pcie->id_clkreq_gpio[i], 1);

	/* [한국어] PHY 드라이버에 전원 끄기를 맡긴다. */
	phy_power_off(kirin_pcie->phy);
	/* [한국어] 이어서 PHY 를 해제한다. init 의 짝이다. */
	phy_exit(kirin_pcie->phy);

	/* [한국어] 두 갈래 모두 성공으로 답한다. */
	return 0;
}

/* [한국어]
 * kirin_pcie_power_on - PHY 를 켜고 PERST# 를 낸다. 두 세대가 갈리는 두 곳 중 하나
 *
 * @pdev: 이 컨트롤러의 플랫폼 디바이스.
 * @kirin_pcie: 이 드라이버의 상태.
 * @return: 0 성공, 실패 시 음수.
 *
 * probe 가 dw_pcie_host_init() 을 부르기 전에 하드웨어를 준비하는 단계 전체다.
 * 다른 글루들이 host_init 콜백 안에서 하는 일을 이 드라이버는 여기서 미리 한다.
 *
 * 앞부분이 세대별로 갈리고 뒷부분은 공통이다.
 *   - 내장 PHY(960): hi3660_pcie_phy_init() 으로 상태·클럭·자원을 확보하고
 *     hi3660_pcie_phy_power_on() 으로 켠다.
 *   - 외부 PHY(970): devm_of_phy_get() 으로 얻어 phy_init()/phy_power_on().
 *   - 공통: 21ms 기다린 뒤 컨트롤러 PERST# 를 내고 다시 10ms 기다린다.
 *
 * 두 대기의 뜻이 상수 이름에 담겨 있다. REF_2_PERST 는 기준 클럭이 선 뒤
 * PERST# 를 낼 때까지, PERST_2_ACCESS 는 PERST# 를 낸 뒤 설정공간에 접근할
 * 때까지 지켜야 하는 시간이다.
 *
 * 되감기 경로가 비대칭이라는 점을 눈여겨볼 것. 내장 PHY 갈래의 두 실패는
 * 곧바로 반환하지만(power_on 이 스스로 되감았으므로), 외부 PHY 갈래와 그
 * 이후의 실패는 err 라벨로 가서 kirin_pcie_power_off() 를 부른다.
 *
 * 실행 컨텍스트: probe. usleep_range 와 PHY 조작으로 잠든다.
 *
 * 에러 경로: 위 설명대로 두 갈래다.
 *
 * 호출 체인:
 *   kirin_pcie_probe() → [이 함수]
 *     → hi3660_pcie_phy_init(), hi3660_pcie_phy_power_on()
 *       또는 devm_of_phy_get(), phy_init(), phy_power_on()
 *     → gpiod_direction_output_raw(), usleep_range()
 */
static int kirin_pcie_power_on(struct platform_device *pdev,
			       struct kirin_pcie *kirin_pcie)
{
	/* [한국어] PHY 조회에 쓸 장치. 외부 PHY 갈래에서만 실제로 쓰인다. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 세대 갈림. 이 파일에서 phy_type 을 보는 두 곳 중 나머지 하나다. */
	if (kirin_pcie->type == PCIE_KIRIN_INTERNAL_PHY) {
		/* [한국어] 내장 PHY 상태를 잡고 클럭 다섯과 자원 셋을 확보한다. */
		ret = hi3660_pcie_phy_init(pdev, kirin_pcie);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 아직 아무것도 켜지 않았으므로 err 로 가지 않고 곧바로 반환한다. */
			return ret;

		/* [한국어] 실제로 전원·클럭·PHY 를 켠다. */
		ret = hi3660_pcie_phy_power_on(kirin_pcie);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 이 함수가 이미 스스로 되감았으므로 역시 곧바로 반환한다. */
			return ret;
	/* [한국어] 여기부터 외부 PHY(970) 갈래다. */
	} else {
		/* [한국어] DT 의 phy 노드에서 PHY 핸들을 얻는다. 그 PHY 드라이버는
		 * drivers/phy 에 있어 이 트리에서 확인 못 함. */
		kirin_pcie->phy = devm_of_phy_get(dev, dev->of_node, NULL);
		/* [한국어] 조회 실패. PHY 드라이버가 아직 없으면 -EPROBE_DEFER 가 온다. */
		if (IS_ERR(kirin_pcie->phy))
			/* [한국어] 코드를 꺼내 올린다. 아직 켠 것이 없어 err 로 가지 않는다. */
			return PTR_ERR(kirin_pcie->phy);

		/* [한국어] PHY 를 초기화한다. */
		ret = phy_init(kirin_pcie->phy);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 여기서부터는 err 라벨로 가서 정리한다. */
			goto err;

		/* [한국어] PHY 에 전원을 넣는다. */
		ret = phy_power_on(kirin_pcie->phy);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] phy_init 을 되돌려야 하므로 err 로 간다. */
			goto err;
	}

	/* perst assert Endpoint */
	/* [한국어] 기준 클럭이 선 뒤 PERST# 를 낼 때까지 21~25ms 를 기다린다.
	 * 옆의 상류 주석이 이 대기의 목적을 한 줄로 적어 두었다. */
	usleep_range(REF_2_PERST_MIN, REF_2_PERST_MAX);

	/* [한국어] 컨트롤러의 PERST# 를 raw 1 로 올린다. 슬롯별 PERST# 는 나중에
	 * 버스 스캔 중 add_bus 가 따로 내보낸다. */
	ret = gpiod_direction_output_raw(kirin_pcie->id_dwc_perst_gpio, 1);
	/* [한국어] GPIO 조작 실패. */
	if (ret)
		/* [한국어] PHY 를 되돌려야 하므로 err 로 간다. */
		goto err;

	/* [한국어] PERST# 를 낸 뒤 설정공간 접근까지 10~12ms 를 기다린다.
	 * add_bus 가 슬롯 PERST# 뒤에 쓰는 대기와 같은 상수다. */
	usleep_range(PERST_2_ACCESS_MIN, PERST_2_ACCESS_MAX);

	/* [한국어] 하드웨어 준비가 끝났다. 이제 probe 가 dw_pcie_host_init() 을 부른다. */
	return 0;
/* [한국어] 외부 PHY 갈래와 PERST# 실패가 모이는 곳. */
err:
	/* [한국어] 켠 것들을 되돌린다. 내장 PHY 갈래의 실패는 여기 오지 않는다. */
	kirin_pcie_power_off(kirin_pcie);

	/* [한국어] 실패 원인을 그대로 올린다. */
	return ret;
}

/* [한국어]
 * kirin_pcie_remove - 이 컨트롤러를 뗀다 (플랫폼 드라이버 remove 콜백)
 *
 * @pdev: 떼어 낼 디바이스.
 * @return: 없음.
 *
 * probe 의 역순이다. 버스를 내리고 PHY 전원을 끈다.
 *
 * 여기서 kirin_pcie_power_off() 를 직접 부르는 것이 이 드라이버의 구조를
 * 드러낸다. dw_pcie_host_ops 에 deinit 훅을 걸어 두지 않았으므로, DWC 코어가
 * SoC 정리를 대신해 주지 않는다. 준비를 host_init 밖에서 했으니 정리도 밖에서
 * 하는 셈이다.
 *
 * regmap, GPIO, 클럭 핸들, 두 상태 구조체는 모두 devm 이라 이 함수가 끝난 뒤
 * 자동으로 풀린다.
 *
 * [상류 코드 관찰] 이 드라이버는 .suppress_bind_attrs = true 로 등록되어
 * sysfs 를 통한 언바인드가 막혀 있다. 즉 이 함수가 실제로 불리는 경로는
 * 모듈 언로드뿐이다.
 *
 * 실행 컨텍스트: 드라이버 언바인드. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → dw_pcie_host_deinit(), kirin_pcie_power_off()
 */
static void kirin_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 심어 둔 상태 구조체를 되찾는다. */
	struct kirin_pcie *kirin_pcie = platform_get_drvdata(pdev);

	/* [한국어] 버스를 내리고 DWC 호스트 자원을 반납한다. host_ops 에 deinit 이
	 * 없으므로 이 호출로 SoC 쪽이 정리되지는 않는다. */
	dw_pcie_host_deinit(&kirin_pcie->pci->pp);

	/* [한국어] 그래서 PHY 전원 끄기를 여기서 직접 부른다. */
	kirin_pcie_power_off(kirin_pcie);
}

/* [한국어] DT compatible 마다 달라지는 것. 필드가 하나뿐이며, 그 하나가
 * 960 과 970 을 가르는 전부다. 다른 SoC 글루들의 drvdata 가 콜백 여럿을
 * 담는 것과 대비된다 — 이 드라이버에서 세대 차이가 그만큼 좁다는 뜻이다. */
struct kirin_pcie_data {
	/* [한국어] 이 compatible 이 내장 PHY 를 쓰는지 외부 PHY 를 쓰는지.
	 * 설정자: 아래 두 정적 표가 각각 한 값씩 못박는다.
	 * 읽는 자: kirin_pcie_probe() 가 kirin_pcie.type 으로 복사한다. 그 뒤로는
	 *   이 필드를 직접 읽는 곳이 없다.
	 * 값 범위: enum pcie_kirin_phy_type 의 두 값.
	 * 동기화: 정적 상수 표라 불변. */
	enum pcie_kirin_phy_type	phy_type;
};

/* [한국어] Kirin 960 용 표. 내장 PHY 를 쓴다 — 즉 이 파일 앞쪽 절반의
 * hi3660_ 함수들이 동작하는 경로다. */
static const struct kirin_pcie_data kirin_960_data = {
	/* [한국어] 내장 PHY. */
	.phy_type = PCIE_KIRIN_INTERNAL_PHY,
};

/* [한국어] Kirin 970 용 표. 외부 PHY 드라이버에 맡긴다. */
static const struct kirin_pcie_data kirin_970_data = {
	/* [한국어] 외부 PHY. clkreq GPIO 처리도 이 갈래에서만 일어난다. */
	.phy_type = PCIE_KIRIN_EXTERNAL_PHY,
};

/* [한국어] DT compatible 과 위 두 표를 잇는 매칭 목록. 세대별 compatible 이
 * 따로 있고 각각 다른 표를 가리킨다. */
static const struct of_device_id kirin_pcie_match[] = {
	/* [한국어] Kirin 960. 내장 PHY 표를 가리킨다. */
	{ .compatible = "hisilicon,kirin960-pcie", .data = &kirin_960_data },
	/* [한국어] Kirin 970. 외부 PHY 표를 가리킨다. */
	{ .compatible = "hisilicon,kirin970-pcie", .data = &kirin_970_data },
	/* [한국어] 목록의 끝을 알리는 빈 항목. */
	{},
};

/* [한국어]
 * kirin_pcie_probe - 이 컨트롤러를 붙인다 (플랫폼 드라이버 진입점)
 *
 * @pdev: 플랫폼 코어가 DT 노드와 매칭해 넘겨준 디바이스.
 * @return: 0 성공, 음수 실패.
 *
 * 순서가 이 함수의 내용 전부이며, 각 단계가 다음 단계의 전제가 된다.
 *   1. match data 를 읽어 960/970 을 가른다.
 *   2. 상태 구조체 둘(kirin_pcie 와 dw_pcie)을 잡는다.
 *   3. 콜백 표 둘을 걸고 세대 정보를 복사한다.
 *   4. 자원(apb regmap, GPIO 들)을 확보한다.
 *   5. drvdata 를 심는다 — to_kirin_pcie 매크로가 이 시점부터 유효해진다.
 *   6. PHY 를 켜고 PERST# 를 낸다.
 *   7. DWC 호스트 코어에 넘긴다.
 *
 * 5번의 위치가 중요하다. to_kirin_pcie 매크로가 drvdata 를 거치므로, 그
 * 매크로를 쓰는 함수(read_dbi/write_dbi/link_up/start_link/add_bus)는 모두
 * 이 줄보다 뒤에서만 불려야 한다. 실제로 그 함수들은 모두 7번 안에서
 * 처음 불린다.
 *
 * dw_pcie 를 kirin_pcie 안에 값으로 두지 않고 따로 할당해 포인터로 거는
 * 구조가 to_kirin_pcie 가 container_of 를 못 쓰는 이유이기도 하다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * 에러 경로: 각 단계의 실패를 그대로 올린다. 되감기가 필요한 것은 6번뿐인데,
 * kirin_pcie_power_on() 이 스스로 되감으므로 여기에는 라벨이 없다.
 * [상류 코드 관찰] 다만 7번(dw_pcie_host_init)이 실패해도 6번을 되돌리지
 * 않는다 — 그 결과를 그대로 반환할 뿐이다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → of_device_get_match_data(), devm_kzalloc(),
 *       kirin_pcie_get_resource(), platform_set_drvdata(),
 *       kirin_pcie_power_on(), dw_pcie_host_init()
 */
static int kirin_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 할당·조회·로그에 두루 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] compatible 에 딸린 정적 표. 세대 정보를 담고 있다. */
	const struct kirin_pcie_data *data;
	/* [한국어] 이번에 만들 드라이버 상태. */
	struct kirin_pcie *kirin_pcie;
	/* [한국어] DWC 코어의 컨트롤러 문맥. 따로 할당한다. */
	struct dw_pcie *pci;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 960/970 갈림의 출발점. 여기서 얻은 phy_type 이 이후 두 곳의
	 * 조건문을 결정한다. */
	data = of_device_get_match_data(dev);
	/* [한국어] 매칭 데이터가 없다 — of_match_table 과 DT 가 어긋난 경우다. */
	if (!data)
		/* [한국어] 세대를 알 수 없으면 아무것도 할 수 없으므로 -EINVAL 로 끊는다. */
		return dev_err_probe(dev, -EINVAL, "OF data missing\n");

	/* [한국어] 드라이버 상태를 0 초기화로 잡는다. 0 초기화 덕에 num_slots 와
	 * n_gpio_clkreq 가 0 에서 시작하고, 쓰지 않는 GPIO 배열 자리가 NULL 로 남는다. */
	kirin_pcie = devm_kzalloc(dev, sizeof(struct kirin_pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!kirin_pcie)
		/* [한국어] 되감을 것이 없다. */
		return -ENOMEM;

	/* [한국어] DWC 컨트롤러 문맥을 따로 잡는다. 값으로 품지 않고 포인터로 거는
	 * 이 선택 때문에 to_kirin_pcie 가 drvdata 를 거치게 된다. */
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!pci)
		/* [한국어] 앞서 잡은 kirin_pcie 는 devres 가 놓아 준다. */
		return -ENOMEM;

	/* [한국어] DWC 코어가 로그와 자원 조회에 쓸 장치를 알려 준다. */
	pci->dev = dev;
	/* [한국어] 코어 콜백 표(read_dbi/write_dbi/link_up/start_link)를 건다. */
	pci->ops = &kirin_dw_pcie_ops;
	/* [한국어] 호스트 훅 표(init 하나)를 건다. */
	pci->pp.ops = &kirin_pcie_host_ops;
	/* [한국어] 두 구조체를 잇는다. */
	kirin_pcie->pci = pci;
	/* [한국어] 세대 정보를 복사해 둔다. 이후 power_on/power_off 가 이것을 본다. */
	kirin_pcie->type = data->phy_type;

	/* [한국어] apb regmap 과 GPIO 들을 확보한다. 반환 타입이 long 인 것을
	 * int 로 받는 지점이다 — 그 함수의 [상류 코드 관찰] 참조. */
	ret = kirin_pcie_get_resource(kirin_pcie, pdev);
	/* [한국어] 자원 확보 실패. */
	if (ret)
		/* [한국어] devm 이라 되감을 것이 없다. */
		return ret;

	/* [한국어] drvdata 를 심는다. 이 줄 이후로 to_kirin_pcie 매크로가 유효해지므로,
	 * 그것을 쓰는 모든 콜백이 이 시점보다 뒤에 불려야 한다. */
	platform_set_drvdata(pdev, kirin_pcie);

	/* [한국어] PHY 를 켜고 PERST# 를 낸다. 세대에 따라 안에서 갈린다. */
	ret = kirin_pcie_power_on(pdev, kirin_pcie);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] power_on 이 스스로 되감았으므로 그대로 반환한다. */
		return ret;

	/* [한국어] DWC 호스트 코어에 넘긴다. 자원 확보, 설정공간 접근 방식 결정,
	 * MSI 준비, host_ops->init 콜백(pci_ops 교체), setup_rc, start_link,
	 * 버스 스캔(그 안에서 add_bus)이 모두 이 한 호출 안에서 일어난다.
	 * 이 호출이 실패해도 위 power_on 을 되돌리지 않는다. */
	return dw_pcie_host_init(&pci->pp);
}

/* [한국어] 플랫폼 드라이버 등록 정보. probe/remove 와 매칭 목록을 묶는다. */
static struct platform_driver kirin_pcie_driver = {
	/* [한국어] 매칭된 디바이스마다 불릴 진입점. */
	.probe			= kirin_pcie_probe,
	/* [한국어] 언바인드 시 불릴 정리 함수. */
	.remove			= kirin_pcie_remove,
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver			= {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name			= "kirin-pcie",
		/* [한국어] 위의 DT 매칭 목록. */
		.of_match_table		= kirin_pcie_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. 즉 사용자가 손으로
		 * 언바인드할 수 없고, remove 가 불리는 경로는 모듈 언로드뿐이다.
		 * PCIe 호스트 브리지를 임의로 떼면 그 아래 장치들이 사라지므로 막아 둔 것이다. */
		.suppress_bind_attrs	= true,
	},
};
/* [한국어] 모듈 초기화/종료 함수를 자동 생성해 위 드라이버를 등록한다. */
module_platform_driver(kirin_pcie_driver);

/* [한국어] 모듈 자동 로딩용 메타데이터. modpost 가 이 목록을 읽어
 * modules.alias 에 compatible 문자열을 넣어 준다.
 *
 * [상류 코드 관찰] 이 선언이 module_platform_driver() 뒤에 온다. 대개는
 * 매칭 목록 정의 바로 아래에 두는데, 순서가 동작에 영향을 주지는 않는다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
MODULE_DEVICE_TABLE(of, kirin_pcie_match);
/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("PCIe host controller driver for Kirin Phone SoCs");
/* [한국어] 원저자 표기. 파일 상단 상류 헤더의 Author 와 같은 사람이다. */
MODULE_AUTHOR("Xiaowei Song <songxiaowei@huawei.com>");
/* [한국어] 라이선스 선언. 파일 맨 위 SPDX 의 GPL-2.0 과 짝을 이룬다.
 * 이 선언이 없으면 커널이 모듈을 오염(tainted)으로 표시한다. */
MODULE_LICENSE("GPL v2");
