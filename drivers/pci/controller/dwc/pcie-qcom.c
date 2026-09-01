// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm PCIe root complex driver
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * Copyright 2015 Linaro Limited.
 *
 * Author: Stanimir Varbanov <svarbanov@mm-sol.com>
 */

/*
 * [한국어 설명] Qualcomm PCIe 루트 컴플렉스 드라이버 (pcie-qcom.c)
 *
 * === 파일의 역할 ===
 * 퀄컴 SoC 에 들어 있는 DesignWare(DWC) 기반 PCIe 컨트롤러를 루트 컴플렉스로
 * 모는 플랫폼 드라이버다. 링크 계층 자체는 DWC 공통 코어
 * (pcie-designware.c / pcie-designware-host.c)가 다루므로, 이 파일이 맡는
 * 것은 그 코어를 감싸는 "글루" 다 — 클록·리셋·레귤레이터·PHY 를 켜고,
 * 퀄컴 고유의 PARF 레지스터 블록을 프로그래밍하고, PERST# 를 조작한다.
 *
 * 이 파일의 성격을 규정하는 것은 하나다: **한 파일이 십여 년치 하드웨어
 * 세대를 담는다.** apq8064(2013년경)부터 x1e80100 까지가 같은 드라이버를
 * 쓰며, 그 차이를 struct qcom_pcie_ops 함수 포인터 표로 흡수한다.
 * 표가 IP 리비전 이름을 그대로 달고 있다 —
 *   ops_2_1_0(Synopsys 4.01a) / ops_1_0_0(4.11a) / ops_2_3_2(4.21a) /
 *   ops_2_4_0(4.20a) / ops_2_3_3(4.30a) / ops_2_7_0(4.30a) / ops_1_9_0 /
 *   ops_1_21_0(5.60a) / ops_2_9_0(5.00a).
 * 각 표는 콜백 일곱 자리를 채운다(get_resources / init / post_init /
 * host_post_init / deinit / ltssm_enable / config_sid). 여기서 갈리는 것이
 * 무엇인지가 이 파일을 읽는 열쇠다.
 *   - get_resources / init / deinit : 세대마다 필요한 클록·리셋·레귤레이터의
 *     이름과 개수, 그리고 켜는 순서가 다르다. 그래서 세대마다 전용
 *     struct qcom_pcie_resources_X_Y_Z 가 있고, 그것들을 union 하나로 겹쳐
 *     둔다 — 한 인스턴스는 한 세대만 쓰므로 겹쳐도 안전하다.
 *   - post_init : PARF 레지스터 프로그래밍의 본체. 세대가 올라가면서
 *     레지스터 자리가 옮겨 가고(_V2 접미사), 다뤄야 할 항목이 늘어난다.
 *   - ltssm_enable : 링크 학습을 켜는 자리가 ELBI(구세대)에서
 *     PARF_LTSSM(신세대)으로 옮겨 간 것이 이 콜백이 둘뿐인 이유다.
 *   - config_sid : IOMMU 의 BDF→SID 변환표를 채우는 일. 1.9.0 에만 있다.
 *   - host_post_init : 열거가 끝난 뒤 하류 장치의 ASPM 을 켜는 일.
 *     1.9.0 과 1.21.0 에만 있다.
 * 표 위에 다시 struct qcom_pcie_cfg 한 겹이 있는데, 같은 ops 를 쓰면서
 * 불리언 플래그만 다른 SoC 들을 구분하기 위한 것이다
 * (override_no_snoop / no_l0s / firmware_managed).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층은 PCI 코어 -> DWC 호스트 코어 -> 이 파일 -> PARF/DBI/ELBI MMIO 와
 * 클록·리셋·PHY·인터커넥트 -> SoC 하드웨어 순이다.
 *
 *   platform_driver -> qcom_pcie_probe()
 *     -> of_device_get_match_data()      SoC 표(qcom_pcie_cfg)를 고른다
 *     -> pm_runtime_get_sync()           전원 도메인을 깨운다
 *     -> [firmware_managed 이면 여기서 갈라진다 — 아래 참고]
 *     -> devm_platform_ioremap_resource_byname("parf")
 *     -> devm_pm_opp_of_add_table() 또는 qcom_pcie_icc_init()
 *     -> cfg->ops->get_resources()       세대별 자원 확보
 *     -> qcom_pcie_parse_ports() 또는 qcom_pcie_parse_legacy_binding()
 *     -> dw_pcie_host_init()             여기서 DWC 코어로 넘어간다
 *
 * DWC 코어가 되부르는 자리가 셋이며, 그것이 struct dw_pcie_host_ops 다.
 *   pp->ops->init      -> qcom_pcie_host_init()
 *                         (drivers/pci/controller/dwc/pcie-designware-host.c:1513)
 *   pp->ops->post_init -> qcom_pcie_host_post_init()   (같은 파일 :1630)
 *   pp->ops->deinit    -> qcom_pcie_host_deinit()      (같은 파일 :1649)
 * 그리고 struct dw_pcie_ops 로 링크 상태를 둘 더 제공한다 —
 * qcom_pcie_link_up() 과 qcom_pcie_start_link().
 *
 * qcom_pcie_host_init() 이 이 파일의 기동 절차 본체다. PERST# 를 걸고 ->
 * 세대별 init -> PHY -> pwrctrl 장치 -> 세대별 post_init -> 능력 손질 ->
 * PERST# 해제 -> config_sid 순으로 간다.
 *
 * firmware_managed 갈래는 완전히 다른 길이다. sa8255p 처럼 펌웨어가 이미
 * 컨트롤러를 세워 둔 플랫폼에서는 이 드라이버가 하드웨어를 건드리지 않고,
 * ECAM(pci-host-common)으로 config 공간만 열어 PCI 코어에 넘긴다. 그래서
 * 그 갈래의 cfg 에는 ops 가 아예 없다(cfg_fw_managed).
 *
 * 실행 컨텍스트: probe/PM 은 프로세스 컨텍스트다. config 접근은 DWC 코어가
 * 담당하며 이 파일에는 없다. 이 파일에 인터럽트 핸들러는 없다 —
 * MSI 와 INTx 도 모두 DWC 코어가 다룬다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/controller/dwc/pcie-designware-host.c 의 dw_pcie_host_init()
 *   과 세 콜백 자리, drivers/pci/probe.c 의 pci_host_probe()(firmware_managed
 *   갈래), 그리고 ../../pci.h 의 코어 내부 선언.
 * 옆쪽: **pcie-qcom-common.c 를 EP 드라이버(pcie-qcom-ep.c)와 공유한다.**
 *   공유하는 것은 정확히 두 함수다 — qcom_pcie_common_set_equalization()
 *   과 qcom_pcie_common_set_16gt_lane_margining(). 둘 다 링크를 세우기
 *   직전에 불리며(이 파일은 qcom_pcie_start_link(), EP 는
 *   qcom_pcie_perst_deassert()), 링크 방향과 무관한 물리 계층 설정이라
 *   RC/EP 양쪽이 그대로 나눠 쓸 수 있다. 반대로 PARF 레지스터 프로그래밍은
 *   같은 이름의 레지스터라도 값과 순서가 달라 각 파일이 따로 갖는다 —
 *   실제로 두 파일이 PARF_SYS_CTRL, PARF_LTSSM, PARF_DEVICE_TYPE 같은
 *   오프셋을 각자 #define 으로 중복 정의한다.
 * 아래쪽: 클록, 리셋 컨트롤러, 레귤레이터, PHY(phy_set_mode_ext 로 RC 모드
 *   지정), 인터커넥트(icc, 대역폭 요구), 런타임 PM 과 OPP(전압 코너),
 *   GPIO(PERST#), 그리고 pci-pwrctrl(하류 슬롯 전원 장치).
 * 공유 상태: struct qcom_pcie 가 컨트롤러 하나를 담고, 그 안의 union res 가
 *   세대별 자원을 겹쳐 담는다. 전역 가변 상태는 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * qcom_pcie_probe()            : 진입점. SoC 표 선택부터 DWC 코어 진입까지.
 * qcom_pcie_host_init()        : DWC 코어가 되부르는 기동 절차 본체.
 * qcom_pcie_start_link()       : 이퀄라이제이션·마진 설정 뒤 LTSSM 을 켠다.
 * qcom_pcie_post_init_2_7_0()  : 신세대 PARF 프로그래밍의 대표 예.
 * qcom_pcie_config_sid_1_9_0() : iommu-map 을 CRC8 해시표로 하드웨어에 적는다.
 * qcom_pcie_icc_opp_update()   : 링크 속도·폭에 맞춰 대역폭/전압을 다시 요구한다.
 * qcom_pcie_parse_ports()      : DT 자식 포트 노드에서 PHY 와 PERST# 를 모은다.
 * qcom_pcie_ecam_host_init()   : firmware_managed 갈래의 ECAM 초기화.
 * struct qcom_pcie             : 컨트롤러 하나의 모든 상태.
 * struct qcom_pcie_ops         : 세대별 콜백 일곱 자리.
 * struct qcom_pcie_cfg         : 같은 ops 를 쓰는 SoC 들을 플래그로 가른다.
 * union qcom_pcie_resources    : 세대별 자원 구조체를 겹쳐 담는 union.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 다만 이 파일에는 상류가 직접 NVMe 를 언급하는 자리가 하나 있다 —
 * qcom_pcie_suspend_noirq() 의 주석이, 서스펜드 때 링크를 L2/L3 로 내리면
 * VDD 가 끊겨 장치가 파워다운되고 "NVMe 같은 스토리지 장치의 수명에 영향을
 * 준다" 고 밝힌다. 그래서 이 드라이버는 활성 장치가 붙어 있는 컨트롤러의
 * 자원을 서스펜드에서 끄지 않는다. NVMe SSD 를 붙인 퀄컴 보드에서 이
 * 판단이 곧 절전과 장치 수명 사이의 선택이 된다.
 */

#include <linux/clk.h> /* [한국어] clk_bulk_ 계열 — 세대마다 개수가 다른 클록을 통째로 켜고 끈다 */
#include <linux/crc8.h> /* [한국어] crc8_populate_msb/crc8 — qcom_pcie_config_sid_1_9_0() 의 BDF 해시 계산 */
#include <linux/debugfs.h> /* [한국어] debugfs_create_dir 와 devm_seqfile — 링크 전이 카운터 노출 */
#include <linux/delay.h> /* [한국어] usleep_range/msleep — 세대별 리셋·클록 안정화 대기 */
#include <linux/gpio/consumer.h> /* [한국어] gpiod_ 계열 — PERST# 를 GPIO 출력으로 걸고 푼다 */
#include <linux/interconnect.h> /* [한국어] icc_set_bw/icc_enable/icc_disable — 인터커넥트 대역폭 요구 */
#include <linux/interrupt.h> /* [한국어] 인터럽트 관련 기본 정의. 이 파일에 핸들러는 없고 DWC 코어가 다룬다 */
#include <linux/io.h> /* [한국어] readl/writel — PARF·DBI·ELBI 레지스터 접근 */
#include <linux/iopoll.h> /* [한국어] 폴링 도우미. 이 파일이 직접 쓰지는 않지만 포함되어 있다 */
#include <linux/kernel.h> /* [한국어] ARRAY_SIZE 등 공통 매크로 */
#include <linux/limits.h> /* [한국어] ULONG_MAX — probe 가 최고 OPP 를 찾을 때 상한으로 쓴다 */
#include <linux/init.h> /* [한국어] 초기화 섹션 매크로. 모듈 등록 경로가 기대하는 정의 */
#include <linux/of.h> /* [한국어] of_device_is_compatible/of_get_property — 구세대의 SoC 직접 판별과 iommu-map 조회 */
#include <linux/of_pci.h> /* [한국어] DT 의 PCI 관련 도우미 */
#include <linux/pci.h> /* [한국어] PCI 코어 API 와 PCI_EXP_ 계열 스펙 상수 */
#include <linux/pci-ecam.h> /* [한국어] pci_ecam_ops 와 pci_ecam_map_bus — firmware_managed 갈래 전용 */
#include <linux/pci-pwrctrl.h> /* [한국어] pci_pwrctrl_ 계열 — 하류 슬롯 전원을 별도 장치로 세운다 */
#include <linux/pm_opp.h> /* [한국어] dev_pm_opp_ 계열 — 링크 속도에 맞춘 전압 코너 선택 */
#include <linux/pm_runtime.h> /* [한국어] pm_runtime_ 계열 — 전원 도메인을 깨우고 재운다 */
#include <linux/platform_device.h> /* [한국어] 플랫폼 디바이스와 자원 조회 */
#include <linux/phy/pcie.h> /* [한국어] PHY_MODE_PCIE_RC — PHY 를 루트 컴플렉스 모드로 지정할 때 쓴다 */
#include <linux/phy/phy.h> /* [한국어] phy_init/phy_power_on/phy_set_mode_ext */
#include <linux/regulator/consumer.h> /* [한국어] regulator_bulk_ 계열 — 구세대가 직접 다루는 PHY·슬롯 전원 */
#include <linux/reset.h> /* [한국어] reset_control_ 계열 — 세대마다 개수가 다른 리셋 조작 */
#include <linux/slab.h> /* [한국어] kzalloc/kfree — config_sid 의 iommu-map 임시 버퍼 */
#include <linux/types.h> /* [한국어] u32/u16/bool 등 기본 타입 */
#include <linux/units.h> /* [한국어] KILO — OPP 주파수를 kHz 로 환산할 때 쓴다 */

#include "../../pci.h" /* [한국어] PCI 서브시스템 내부 헤더. pcie_get_link_speed() 등 코어 비공개 도우미 */
#include "../pci-host-common.h" /* [한국어] pci_host_common_ecam_create() — firmware_managed 갈래가 ECAM 창을 만든다 */
#include "pcie-designware.h" /* [한국어] DWC 코어의 struct dw_pcie / dw_pcie_rp 와 dw_pcie_host_ops 정의 */
#include "pcie-qcom-common.h" /* [한국어] RC 와 EP 가 공유하는 두 함수의 선언(이퀄라이제이션, Gen4 레인 마진) */

/* PARF registers */
#define PARF_SYS_CTRL				0x00 /* [한국어] 시스템 제어. 웨이크업·클록 게이팅·AUX 전원 감지 비트가 모여 있다 */
#define PARF_PM_CTRL				0x20 /* [한국어] 전력 관리 제어. L1 진입 금지 비트가 있다 */
#define PARF_PCS_DEEMPH				0x34 /* [한국어] PHY 송신 디엠퍼시스. 2.1.0 세대의 ipq8064 계열만 쓴다 */
#define PARF_PCS_SWING				0x38 /* [한국어] PHY 송신 스윙. 위와 같은 자리에서만 쓴다 */
#define PARF_PHY_CTRL				0x40 /* [한국어] PHY 제어. 파워다운 비트와 TX 종단 오프셋이 있다 */
#define PARF_PHY_REFCLK				0x4c /* [한국어] PHY 기준 클록 제어. 외부 클록 사용 여부를 정한다 */
#define PARF_CONFIG_BITS			0x50 /* [한국어] PHY 설정 비트 묶음. RX 이퀄라이저 값이 여기 들어간다 */
#define PARF_DBI_BASE_ADDR			0x168 /* [한국어] 구세대의 DBI 물리 주소 등록 자리. CPU 도메인이라 물리 주소를 넣는다 */
#define PARF_SLV_ADDR_SPACE_SIZE		0x16c /* [한국어] 구세대의 슬레이브 주소 공간 크기 */
#define PARF_MHI_CLOCK_RESET_CTRL		0x174 /* [한국어] MHI 클록/리셋 제어. BYPASS 와 클록 활성 비트가 있다 */
#define PARF_AXI_MSTR_WR_ADDR_HALT		0x178 /* [한국어] 구세대의 AXI 마스터 쓰기 주소 정지 제어 */
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2		0x1a8 /* [한국어] 같은 기능의 신세대 자리(_V2). 2.3.2 이후가 쓴다 */
#define PARF_Q2A_FLUSH				0x1ac /* [한국어] Q2A 플러시 제어. 초기화에서 0 으로 둔다 */
#define PARF_LTSSM				0x1b0 /* [한국어] 신세대의 링크 학습 활성 레지스터. 구세대는 ELBI 를 쓴다 */
#define PARF_SID_OFFSET				0x234 /* [한국어] [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_BDF_TRANSLATE_CFG			0x24c /* [한국어] [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_DBI_BASE_ADDR_V2			0x350 /* [한국어] 신세대의 DBI 물리 주소 하위 워드 */
#define PARF_DBI_BASE_ADDR_V2_HI		0x354 /* [한국어] 같은 주소의 상위 워드. 64비트 주소를 다루므로 둘로 나뉜다 */
#define PARF_SLV_ADDR_SPACE_SIZE_V2		0x358 /* [한국어] 신세대의 슬레이브 주소 공간 크기 하위 워드. 0 을 쓴다 */
#define PARF_SLV_ADDR_SPACE_SIZE_V2_HI		0x35c /* [한국어] 그 상위 워드. 실제 크기가 여기 들어간다 */
#define PARF_NO_SNOOP_OVERRIDE			0x3d4 /* [한국어] TLP 의 NO_SNOOP 속성을 무시하게 하는 레지스터. cfg->override_no_snoop 인 SoC 만 쓴다 */
#define PARF_ATU_BASE_ADDR			0x634 /* [한국어] 신세대의 iATU 물리 주소 하위 워드. DWC 5.x 에서 iATU 가 DBI 와 분리되며 생겼다 */
#define PARF_ATU_BASE_ADDR_HI			0x638 /* [한국어] 그 상위 워드 */
#define PARF_DEVICE_TYPE			0x1000 /* [한국어] 컨트롤러를 RC 로 둘지 EP 로 둘지 정하는 레지스터. EP 판(pcie-qcom-ep.c)이 같은 자리에 EP 값을 쓴다 */
#define PARF_BDF_TO_SID_TABLE_N			0x2000 /* [한국어] BDF→SID 변환표의 시작. 256칸짜리 해시표다 */
#define PARF_BDF_TO_SID_CFG			0x2c00 /* [한국어] 그 변환의 우회(bypass) 설정. EP 판은 반대로 우회를 켠다 */

/* ELBI registers */
#define ELBI_SYS_CTRL				0x04 /* [한국어] 구세대의 링크 학습 활성 비트가 있는 ELBI 레지스터 */

/* DBI registers */
#define AXI_MSTR_RESP_COMP_CTRL0		0x818 /* [한국어] AXI 마스터 응답 완료 제어 0. 최대 TLP 크기를 정한다 */
#define AXI_MSTR_RESP_COMP_CTRL1		0x81c /* [한국어] 같은 제어 1. 브리지 사이드밴드 초기화 비트가 있다 */

/* MHI registers */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L2		0xc04 /* [한국어] L2 전이 횟수 카운터. debugfs 가 읽는다 */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L1		0xc0c /* [한국어] L1 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L0S		0xc10 /* [한국어] L0s 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1	0xc84 /* [한국어] L1.1 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2	0xc88 /* [한국어] L1.2 전이 횟수 카운터 */

/* PARF_SYS_CTRL register fields */
#define MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN	BIT(29) /* [한국어] P2 상태에서 MAC 이 PHY 를 파워다운시키는 MUX. 초기화에서 끈다 */
#define MST_WAKEUP_EN				BIT(13) /* [한국어] 마스터 쪽 웨이크업 활성 */
#define SLV_WAKEUP_EN				BIT(12) /* [한국어] 슬레이브 쪽 웨이크업 활성 */
#define MSTR_ACLK_CGC_DIS			BIT(10) /* [한국어] 마스터 AXI 클록 게이팅 금지 */
#define SLV_ACLK_CGC_DIS			BIT(9) /* [한국어] 슬레이브 AXI 클록 게이팅 금지 */
#define CORE_CLK_CGC_DIS			BIT(6) /* [한국어] 코어 클록 게이팅 금지 */
#define AUX_PWR_DET				BIT(4) /* [한국어] AUX 전원이 있다고 보고 */
#define L23_CLK_RMV_DIS				BIT(2) /* [한국어] L2/L3 상태에서 클록 제거 금지 */
#define L1_CLK_RMV_DIS				BIT(1) /* [한국어] L1 상태에서 클록 제거 금지. 위 여덟 비트를 2.3.3/2.9.0 이 한 번에 써 넣는다 */

/* PARF_PM_CTRL register fields */
#define REQ_NOT_ENTR_L1				BIT(5) /* [한국어] L1 진입 금지 비트. 2.7.0 이 이것을 지워 L1 과 L1SS 를 허용한다 */

/* PARF_PCS_DEEMPH register fields */
#define PCS_DEEMPH_TX_DEEMPH_GEN1(x)		FIELD_PREP(GENMASK(21, 16), x) /* [한국어] Gen1 송신 디엠퍼시스 값 필드 */
#define PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(x)	FIELD_PREP(GENMASK(13, 8), x) /* [한국어] Gen2 의 3.5dB 디엠퍼시스 값 필드 */
#define PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(x)	FIELD_PREP(GENMASK(5, 0), x) /* [한국어] Gen2 의 6dB 디엠퍼시스 값 필드. 세 값 모두 ipq8064 계열 전용이다 */

/* PARF_PCS_SWING register fields */
#define PCS_SWING_TX_SWING_FULL(x)		FIELD_PREP(GENMASK(14, 8), x) /* [한국어] 송신 스윙 최대값 필드 */
#define PCS_SWING_TX_SWING_LOW(x)		FIELD_PREP(GENMASK(6, 0), x) /* [한국어] 송신 스윙 최소값 필드 */

/* PARF_PHY_CTRL register fields */
#define PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK	GENMASK(20, 16) /* [한국어] TX 종단 오프셋 필드의 마스크 */
#define PHY_CTRL_PHY_TX0_TERM_OFFSET(x)		FIELD_PREP(PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK, x) /* [한국어] 그 필드에 값을 넣는 도우미. ipq8064 가 7 을 넣는다 */
#define PHY_TEST_PWR_DOWN			BIT(0) /* [한국어] PHY 파워다운 비트. 초기화에서 지우고 deinit 이 다시 세운다 */

/* PARF_PHY_REFCLK register fields */
#define PHY_REFCLK_SSP_EN			BIT(16) /* [한국어] 대역 확산 클록(SSC) 활성 비트 */
#define PHY_REFCLK_USE_PAD			BIT(12) /* [한국어] 외부 패드의 기준 클록을 쓰는 비트. 상류 주석대로 ipq806x 계열에만 필요하다 */

/* PARF_CONFIG_BITS register fields */
#define PHY_RX0_EQ(x)				FIELD_PREP(GENMASK(26, 24), x) /* [한국어] RX 이퀄라이저 값 필드 */

/* PARF_SLV_ADDR_SPACE_SIZE register value */
#define SLV_ADDR_SPACE_SZ			0x80000000 /* [한국어] 슬레이브 주소 공간 크기로 쓰는 값(2GB) */

/* PARF_MHI_CLOCK_RESET_CTRL register fields */
#define AHB_CLK_EN				BIT(0) /* [한국어] AHB 클록 활성. 2.9.0 만 쓴다 */
#define MSTR_AXI_CLK_EN				BIT(1) /* [한국어] 마스터 AXI 클록 활성. 2.9.0 만 쓴다 */
#define BYPASS					BIT(4) /* [한국어] MHI 클록/리셋 제어를 우회하는 비트. 여러 세대가 쓴다 */

/* PARF_AXI_MSTR_WR_ADDR_HALT register fields */
#define EN					BIT(31) /* [한국어] AXI 마스터 쓰기 주소 정지 활성 비트 */

/* PARF_LTSSM register fields */
#define LTSSM_EN				BIT(8) /* [한국어] 링크 학습 활성 비트. EP 판은 같은 비트를 상수 없이 BIT(8) 로 직접 쓴다 */

/* PARF_NO_SNOOP_OVERRIDE register fields */
#define WR_NO_SNOOP_OVERRIDE_EN			BIT(1) /* [한국어] 쓰기 TLP 의 NO_SNOOP 무시 */
#define RD_NO_SNOOP_OVERRIDE_EN			BIT(3) /* [한국어] 읽기 TLP 의 NO_SNOOP 무시. 둘을 함께 켜 캐시 스누핑을 강제한다 */

/* PARF_DEVICE_TYPE register fields */
#define DEVICE_TYPE_RC				0x4 /* [한국어] PARF_DEVICE_TYPE 에 넣을 RC 값 */

/* PARF_BDF_TO_SID_CFG fields */
#define BDF_TO_SID_BYPASS			BIT(0) /* [한국어] BDF→SID 우회 비트. config_sid 가 이것을 지워 변환을 켠다 */

/* ELBI_SYS_CTRL register fields */
#define ELBI_SYS_CTRL_LT_ENABLE			BIT(0) /* [한국어] ELBI 의 링크 학습 활성 비트 */

/* AXI_MSTR_RESP_COMP_CTRL0 register fields */
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K	0x4 /* [한국어] 최대 TLP 크기를 2K 로 두는 값. 2.1.0 이 상류 주석대로 기본 4K 대신 이것을 쓴다 */
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_4K	0x5 /* [한국어] 같은 필드의 4K 값. [관찰] 정의만 있고 쓰이는 곳이 없다 */

/* AXI_MSTR_RESP_COMP_CTRL1 register fields */
#define CFG_BRIDGE_SB_INIT			BIT(0) /* [한국어] 브리지 사이드밴드 초기화 비트 */

/* PCI_EXP_SLTCAP register fields */
#define PCIE_CAP_SLOT_POWER_LIMIT_VAL		FIELD_PREP(PCI_EXP_SLTCAP_SPLV, 250) /* [한국어] 슬롯 전력 한도 값(250). 스케일과 함께 25W 를 뜻하는 것으로 보이나 이 트리에서 확인 못 함 */
#define PCIE_CAP_SLOT_POWER_LIMIT_SCALE		FIELD_PREP(PCI_EXP_SLTCAP_SPLS, 1) /* [한국어] 그 스케일 필드 */
/* [한국어] RC 의 슬롯 능력(Slot Capabilities)에 통째로 써 넣는 비트 묶음.
 * qcom_pcie_post_init_2_3_3() 과 qcom_pcie_post_init_2_9_0() 이 이 값을
 * PCI_EXP_SLTCAP 에 그대로 쓴다 — 읽고-고쳐-쓰기가 아니라 덮어쓰기다.
 *
 * 켜는 것은 핫플러그에 필요한 기능들이다 — Attention Button, Power
 * Controller, MRL 센서, Attention/Power Indicator, Hot-Plug Surprise,
 * Electromechanical Interlock, 그리고 슬롯 전력 한도 값과 스케일.
 * 즉 "이 슬롯은 핫플러그가 가능하다" 고 광고하는 것이며, 특정 장치 종류를
 * 겨냥한 설정은 아니다.
 *
 * [관찰] 이 묶음에는 PCI_EXP_SLTCAP_NCCS 가 들어 있지 않다. 다른 세대들이
 * qcom_pcie_set_slot_nccs() 로 그 비트를 따로 세우는 것과 결과가 달라지는데,
 * 그 차이가 의도된 것인지는 이 트리에서 확인 못 함.
 *
 * 주의: 아래 각 줄 끝의 백슬래시는 매크로 연속을 뜻하며 **줄의 마지막 문자여야 한다.**
 * 뒤에 주석이나 공백이 오면 연속이 끊겨 매크로가 첫 줄에서 끝나 버린다.
 * 그래서 이 블록 안의 각 줄에는 끝 주석을 달 수 없다. */
#define PCIE_CAP_SLOT_VAL			(PCI_EXP_SLTCAP_ABP | \
						 PCI_EXP_SLTCAP_PCP | \
						 PCI_EXP_SLTCAP_MRLSP | \
						 PCI_EXP_SLTCAP_AIP | \
						 PCI_EXP_SLTCAP_PIP | \
						 PCI_EXP_SLTCAP_HPS | \
						 PCI_EXP_SLTCAP_EIP | \
						 PCIE_CAP_SLOT_POWER_LIMIT_VAL | \
						 PCIE_CAP_SLOT_POWER_LIMIT_SCALE)

#define PERST_DELAY_US				1000 /* [한국어] PERST# 조작 뒤 신호가 안정될 때까지 기다리는 시간(us) */

#define QCOM_PCIE_CRC8_POLYNOMIAL		(BIT(2) | BIT(1) | BIT(0)) /* [한국어] BDF 해시에 쓰는 CRC8 다항식. 하위 세 비트가 선 값이다 */

/* [한국어] 링크 속도 코드를 인터커넥트 대역폭 값으로 바꾸는 매크로.
 * 속도 코드 → PCIe 속도 → Mbps → icc 단위로 세 번 변환한다.
 * 실제 대역폭은 여기에 레인 수를 곱해 구한다(qcom_pcie_icc_opp_update).
 * EP 판(pcie-qcom-ep.c)에도 같은 이름·같은 정의가 따로 있다 — 두 파일이
 * PARF 정의와 마찬가지로 이 매크로도 공유하지 않는다. */
#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
		Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed)))

/* [한국어] 1.0.0 세대(apq8084)가 쓰는 자원 묶음.
 * 세대마다 필요한 자원의 종류와 개수가 달라, 이런 구조체가 일곱 개 있고
 * 아래 union qcom_pcie_resources 가 그것들을 겹쳐 담는다. 한 인스턴스는
 * 한 세대만 쓰므로 겹쳐도 안전하다. */
struct qcom_pcie_resources_1_0_0 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] "core" 리셋 하나.
		 * 설정자: qcom_pcie_get_resources_1_0_0() 의 devm_reset_control_get_exclusive().
		 * 읽는 자: init 이 풀고 deinit 이 건다. 이 세대는 이 파일에서 유일하게
		 *          deinit 이 리셋을 다시 거는 대칭을 지킨다.
		 * 값 범위: 이름이 정해진 단일 리셋이라 DT 에 없으면 probe 가 실패한다.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control *core;
		/* [한국어] "vdda" 레귤레이터 하나.
		 * 설정자: get_resources 의 devm_regulator_get().
		 * 읽는 자: init 이 켜고 deinit 이 끈다.
		 * 값 범위: 필수라 없으면 probe 가 실패한다.
		 * 동기화: probe/PM 경로 전용. */
	struct regulator *vdda;
};

#define QCOM_PCIE_2_1_0_MAX_RESETS		6 /* [한국어] 이 세대가 다루는 리셋의 최대 개수(pci/axi/ahb/por/phy/ext) */
#define QCOM_PCIE_2_1_0_MAX_SUPPLY		3 /* [한국어] 이 세대가 다루는 레귤레이터 개수(vdda/vdda_phy/vdda_refclk) */
/* [한국어] 2.1.0 세대(apq8064/ipq8064)가 쓰는 자원 묶음.
 * 리셋과 레귤레이터를 모두 배열로 다루는 것이 이 세대의 특징이다.
 * PHY 전원까지 드라이버가 직접 켜는 구세대 방식이다. */
struct qcom_pcie_resources_2_1_0 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] 이름을 손으로 꽂아 두는 리셋 배열.
		 * 설정자: get_resources 가 [0]~[5] 에 pci/axi/ahb/por/phy/ext 를 넣는다.
		 * 읽는 자: reset_control_bulk_assert / _deassert 의 대상.
		 * 값 범위: 배열 크기는 6 이지만 실제로 쓰는 개수는 아래 num_resets 다.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control_bulk_data resets[QCOM_PCIE_2_1_0_MAX_RESETS];
		/* [한국어] 위 배열에서 실제로 쓰는 개수.
		 * 설정자: get_resources 가 apq8064 이면 5, 그 밖에는 6 으로 둔다 —
		 *          상류 주석대로 "ext" 가 APQ8016 에서 선택이기 때문이다.
		 * 읽는 자: bulk 조작 함수들의 개수 인자.
		 * 값 범위: 5 또는 6.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	int num_resets;
		/* [한국어] 레귤레이터 셋(vdda/vdda_phy/vdda_refclk).
		 * 설정자: get_resources 가 이름을 꽂고 bulk 로 조회한다.
		 * 읽는 자: init 이 켜고 deinit 이 끈다.
		 * 값 범위: 개수가 고정이라 ARRAY_SIZE 로 다룬다.
		 * 동기화: probe/PM 경로 전용. */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_1_0_MAX_SUPPLY];
};

#define QCOM_PCIE_2_3_2_MAX_SUPPLY		2 /* [한국어] 이 세대가 다루는 레귤레이터 개수(vdda/vddpe-3v3) */
/* [한국어] 2.3.2 세대(msm8996)가 쓰는 자원 묶음.
 * **리셋을 하나도 잡지 않는 유일한 세대**라, 필드가 클록과 레귤레이터뿐이다. */
struct qcom_pcie_resources_2_3_2 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] 레귤레이터 둘(vdda/vddpe-3v3).
		 * 설정자: get_resources 가 이름을 꽂고 bulk 로 조회한다.
		 * 읽는 자: init 이 켜고 deinit 이 끈다.
		 * 값 범위: vddpe-3v3 은 슬롯 쪽 3.3V 로 보이며, 2.7.0 계열도 같은 짝을 쓴다.
		 * 동기화: probe/PM 경로 전용. */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_3_2_MAX_SUPPLY];
};

#define QCOM_PCIE_2_3_3_MAX_RESETS		7 /* [한국어] 이 세대가 다루는 리셋 개수(axi_m/axi_s/pipe/axi_m_sticky/sticky/ahb/sleep) */
/* [한국어] 2.3.3 세대(ipq8074)가 쓰는 자원 묶음.
 * 리셋 개수를 가르는 분기가 없어(이 세대를 쓰는 SoC 가 하나뿐이다)
 * num_resets 필드도 없고, 코드가 ARRAY_SIZE 를 직접 쓴다. */
struct qcom_pcie_resources_2_3_3 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] 리셋 일곱. 이름 필드가 rst 인 것이 다른 세대(resets)와 다르다.
		 * 설정자: get_resources 가 [0]~[6] 에 이름을 꽂고 bulk 로 조회한다.
		 * 읽는 자: init 이 걸었다 푼다. deinit 은 다시 걸지 않는다.
		 * 값 범위: 전부 필수라 하나라도 없으면 probe 가 실패한다.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control_bulk_data rst[QCOM_PCIE_2_3_3_MAX_RESETS];
};

#define QCOM_PCIE_2_4_0_MAX_RESETS		12 /* [한국어] 이 세대가 다루는 리셋의 최대 개수. 이 파일에서 가장 많다 */
/* [한국어] 2.4.0 세대(ipq4019/qcs404)가 쓰는 자원 묶음.
 * 리셋을 최대 열둘까지 다루며, 2.1.0 과 같은 "이름 배열 + 개수" 방식으로
 * SoC 차이를 흡수한다. 레귤레이터는 다루지 않는다. */
struct qcom_pcie_resources_2_4_0 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] 리셋 이름 열둘을 꽂아 두는 배열.
		 * 설정자: get_resources 가 axi_m 부터 phy_ahb 까지 순서대로 넣는다.
		 *          앞의 여섯이 공통, 뒤의 여섯이 ipq4019 전용이라 개수 하나로
		 *          가를 수 있게 정렬되어 있다.
		 * 읽는 자: bulk 조작 함수들.
		 * 값 범위: 배열 크기 12, 실제 사용은 아래 num_resets.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control_bulk_data resets[QCOM_PCIE_2_4_0_MAX_RESETS];
		/* [한국어] 실제로 쓰는 리셋 개수.
		 * 설정자: get_resources 가 ipq4019 이면 12, 그 밖에는 6 으로 둔다.
		 * 읽는 자: bulk 조작 함수들의 개수 인자.
		 * 값 범위: 6 또는 12.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	int num_resets;
};

#define QCOM_PCIE_2_7_0_MAX_SUPPLIES		2 /* [한국어] 이 세대가 다루는 레귤레이터 개수(vdda/vddpe-3v3) */
/* [한국어] 2.7.0 계열(sdm845 이후 최신 SoC 대부분)이 쓰는 자원 묶음.
 * ops_2_7_0 / ops_1_9_0 / ops_1_21_0 이 모두 이 구조체를 쓴다.
 * 리셋을 이름 배열이 아니라 **핸들 하나로 묶어** 다루는 것이 구세대와의
 * 가장 큰 차이다 — 리셋 구성이 SoC 마다 달라도 드라이버가 알 필요가 없다. */
struct qcom_pcie_resources_2_7_0 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] 레귤레이터 둘(vdda/vddpe-3v3). 2.3.2 와 같은 짝이다.
		 * 설정자: qcom_pcie_get_resources_2_7_0().
		 * 읽는 자: init 이 켜고 deinit 이 끈다.
		 * 값 범위: 개수 고정.
		 * 동기화: probe/PM 경로 전용. */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_7_0_MAX_SUPPLIES];
		/* [한국어] DT 가 준 리셋 전부를 하나로 묶은 핸들.
		 * 설정자: devm_reset_control_array_get_exclusive() — 이름을 따지지 않는다.
		 * 읽는 자: init 이 걸었다 푼다. deinit 은 다시 걸지 않는다.
		 * 값 범위: 배열 판이 아니라 단일 핸들이라 개수 필드가 필요 없다.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control *rst;
};

/* [한국어] 2.9.0 세대(ipq5018/ipq6018/ipq9574)가 쓰는 자원 묶음.
 * 이 파일에서 가장 단순하다 — 2.7.0 과 같은 리셋 핸들 방식을 쓰면서
 * 레귤레이터가 없다. 네트워킹 SoC 라 슬롯 전원을 다루지 않기 때문이다. */
struct qcom_pcie_resources_2_9_0 {
		/* [한국어] 이 세대가 쓰는 클록 배열.
		 * 설정자: 세대별 get_resources 가 devm_clk_bulk_get_all() 로 DT 가 준
		 *          것을 통째로 받는다 — 이름을 따지지 않는 것이 이 파일 모든
		 *          세대의 공통 방식이다.
		 * 읽는 자: 세대별 init/post_init 이 켜고 deinit 이 끈다.
		 * 값 범위: devm 으로 잡히므로 따로 놓지 않는다.
		 * 동기화: probe/PM 경로에서만 다루며, 그 둘은 겹치지 않는다. */
	struct clk_bulk_data *clks;
		/* [한국어] 위 배열의 원소 수.
		 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 조회 실패다.
		 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 개수 인자.
		 * 값 범위: 음수이면 get_resources 가 그 자리에서 오류로 돌아간다.
		 * 동기화: 위와 같다. */
	int num_clks;
		/* [한국어] DT 가 준 리셋 전부를 묶은 핸들. 2.7.0 과 같은 방식이다.
		 * 설정자: devm_reset_control_array_get_exclusive().
		 * 읽는 자: init 이 걸었다 푼다. deinit 은 다시 걸지 않는다.
		 * 값 범위: 단일 핸들.
		 * 동기화: probe/PM 경로 전용. */
	struct reset_control *rst;
};

/* [한국어] 세대별 자원 구조체 일곱을 겹쳐 담는 union.
 * 컨트롤러 하나는 한 세대만 쓰므로 겹쳐도 안전하며, struct qcom_pcie 의
 * 크기를 가장 큰 세대(2.4.0)에 맞추는 효과가 있다.
 * 어느 멤버를 쓸지는 cfg->ops 가 어느 표를 가리키느냐로 정해지고,
 * 세대별 함수들이 각자 자기 멤버만 꺼내 쓴다. */
union qcom_pcie_resources {
	struct qcom_pcie_resources_1_0_0 v1_0_0; /* [한국어] 1.0.0 세대용 */
	struct qcom_pcie_resources_2_1_0 v2_1_0; /* [한국어] 2.1.0 세대용 */
	struct qcom_pcie_resources_2_3_2 v2_3_2; /* [한국어] 2.3.2 세대용 */
	struct qcom_pcie_resources_2_3_3 v2_3_3; /* [한국어] 2.3.3 세대용 */
	struct qcom_pcie_resources_2_4_0 v2_4_0; /* [한국어] 2.4.0 세대용. 이 파일에서 가장 큰 멤버라 union 의 크기를 결정한다 */
	struct qcom_pcie_resources_2_7_0 v2_7_0; /* [한국어] 2.7.0 계열용(1.9.0, 1.21.0 포함) */
	struct qcom_pcie_resources_2_9_0 v2_9_0; /* [한국어] 2.9.0 세대용 */
};

/* [한국어] struct qcom_pcie 의 전방 선언.
 * 아래 struct qcom_pcie_ops 의 콜백들이 qcom_pcie 를 인자로 받는데,
 * qcom_pcie 자신은 cfg 를 통해 그 표를 가리킨다. 서로를 참조하는 순환이라
 * 한쪽을 먼저 이름만 알려 두어야 한다. */
struct qcom_pcie;

/* [한국어] **이 파일의 뼈대**. IP 리비전마다 하나씩 있는 콜백 표다.
 * 아홉 개의 정적 인스턴스(ops_2_1_0 / ops_1_0_0 / ops_2_3_2 / ops_2_4_0 /
 * ops_2_3_3 / ops_2_7_0 / ops_1_9_0 / ops_1_21_0 / ops_2_9_0)가 파일 뒤쪽에
 * 있고, 각 표 위의 상류 주석이 Qcom IP 리비전과 대응하는 Synopsys IP
 * 리비전을 함께 적어 둔다.
 *
 * 콜백이 불리는 순서는 이렇다.
 *   probe        → get_resources
 *   host_init    → init → (PHY) → post_init → (PERST 해제) → config_sid
 *   열거 완료 후 → host_post_init
 *   host_deinit  → deinit
 *
 * 전부 const 정적 인스턴스라 런타임에 바뀌지 않는다. */
struct qcom_pcie_ops {
		/* [한국어] 세대별 클록·리셋·레귤레이터를 확보한다.
		 * 설정자: 아홉 표 모두 채운다(일부는 같은 함수를 공유).
		 * 읽는 자: qcom_pcie_probe().
		 * 값 범위: NULL 불가 — probe 가 확인 없이 부른다. 다만 firmware_managed
		 *          갈래는 ops 자체가 없어 여기까지 오지 않는다.
		 * 동기화: 읽기 전용 포인터. */
	int (*get_resources)(struct qcom_pcie *pcie);
		/* [한국어] 확보해 둔 자원을 켠다(신세대는 PARF 프로그래밍까지).
		 * 설정자: 아홉 표 모두 채운다.
		 * 읽는 자: qcom_pcie_host_init().
		 * 값 범위: NULL 불가.
		 * 동기화: 읽기 전용 포인터. */
	int (*init)(struct qcom_pcie *pcie);
		/* [한국어] PHY 를 켠 뒤의 PARF 레지스터 프로그래밍.
		 * 설정자: 2.4.0 을 뺀 여덟 표가 채운다 — 2.4.0 은 2.3.2 의 것을 빌려 쓴다.
		 * 읽는 자: qcom_pcie_host_init() 이 NULL 확인 후 부른다.
		 * 값 범위: NULL 이면 그 단계를 건너뛴다.
		 * 동기화: 읽기 전용 포인터. */
	int (*post_init)(struct qcom_pcie *pcie);
		/* [한국어] 버스 열거가 끝난 뒤의 후처리.
		 * 설정자: ops_1_9_0 과 ops_1_21_0 만 채우며, 둘 다
		 *          qcom_pcie_host_post_init_2_7_0(하류 ASPM 활성)을 넣는다.
		 * 읽는 자: qcom_pcie_host_post_init() 이 NULL 확인 후 부른다.
		 * 값 범위: 나머지 일곱 표에서는 NULL 이다.
		 * 동기화: 읽기 전용 포인터. */
	void (*host_post_init)(struct qcom_pcie *pcie);
		/* [한국어] init 이 켠 것을 되돌린다.
		 * 설정자: 아홉 표 모두 채운다.
		 * 읽는 자: qcom_pcie_host_deinit() 과 host_init 의 실패 경로.
		 * 값 범위: NULL 불가.
		 * 동기화: 읽기 전용 포인터. */
	void (*deinit)(struct qcom_pcie *pcie);
		/* [한국어] 링크 학습(LTSSM)을 켠다.
		 * 설정자: 아홉 표 모두 채우되 구현은 둘뿐이다 — 구세대 둘이 ELBI 판,
		 *          나머지 일곱이 PARF_LTSSM 판을 쓴다. 레지스터 자리가 옮겨 간
		 *          것이 이 콜백이 존재하는 유일한 이유다.
		 * 읽는 자: qcom_pcie_start_link() 이 NULL 확인 후 부른다.
		 * 값 범위: 이 파일의 표는 모두 채우지만 확인은 남아 있다.
		 * 동기화: 읽기 전용 포인터. */
	void (*ltssm_enable)(struct qcom_pcie *pcie);
		/* [한국어] IOMMU 의 BDF→SID 변환표를 채운다.
		 * 설정자: ops_1_9_0 만 채운다(qcom_pcie_config_sid_1_9_0).
		 * 읽는 자: qcom_pcie_host_init() 이 PERST# 를 푼 뒤 NULL 확인 후 부른다.
		 * 값 범위: 나머지 여덟 표에서는 NULL.
		 * 동기화: 읽기 전용 포인터. */
	int (*config_sid)(struct qcom_pcie *pcie);
};

 /**
  * struct qcom_pcie_cfg - Per SoC config struct
  * @ops: qcom PCIe ops structure
  * @override_no_snoop: Override NO_SNOOP attribute in TLP to enable cache
  * snooping
  * @firmware_managed: Set if the Root Complex is firmware managed
  */
struct qcom_pcie_cfg {
	const struct qcom_pcie_ops *ops; /* [한국어] 이 SoC 가 쓸 세대별 콜백 표(상류 주석의 @ops). firmware_managed 갈래에서만 NULL 이며, probe 가 그 조합을 유효성 검사한다 */
	bool override_no_snoop; /* [한국어] TLP 의 NO_SNOOP 을 무시해 캐시 스누핑을 강제할지(상류 주석 @override_no_snoop). cfg_1_34_0 만 켠다. 읽는 자는 qcom_pcie_post_init_2_7_0() */
	bool firmware_managed; /* [한국어] 루트 컴플렉스를 펌웨어가 관리하는지(상류 주석 @firmware_managed). cfg_fw_managed 만 켜며, 그 표에는 ops 가 없다. probe 가 이 값으로 완전히 다른 갈래로 간다 */
	bool no_l0s; /* [한국어] L0s 를 광고하지 않을지. cfg_2_3_2 와 cfg_sc8280xp 가 켠다. 읽는 자는 qcom_pcie_clear_aspm_l0s(). [관찰] 위 상류 kernel-doc 에는 이 필드의 설명이 빠져 있다 */
};

/* [한국어] PERST# GPIO 하나를 목록에 매달기 위한 껍데기.
 * 포트 하나가 여러 슬롯의 PERST# 를 가질 수 있어, 배열이 아니라 목록으로
 * 다룬다. qcom_pcie_parse_perst() 가 DT 를 깊이 우선으로 훑어 채운다. */
struct qcom_pcie_perst {
		/* [한국어] 소속 포트의 perst 목록에 연결되는 고리.
		 * 설정자: qcom_pcie_parse_perst() 또는 qcom_pcie_parse_legacy_binding().
		 * 읽는 자: __qcom_pcie_perst_assert() 의 순회, 그리고 실패 경로의 삭제.
		 * 값 범위: 매달기 전에 INIT_LIST_HEAD 로 초기화된다.
		 * 동기화: probe 에서 만들고 이후 구조가 바뀌지 않는다. */
	struct list_head list;
		/* [한국어] 실제 GPIO 핸들.
		 * 설정자: devm_fwnode_gpiod_get(..., GPIOD_OUT_HIGH) — 출력이며, High 가
		 *          곧 PERST# 인가다(active-low). 잡는 순간부터 리셋이 걸린다.
		 *          EP 판(pcie-qcom-ep.c)은 같은 신호를 GPIOD_IN 으로 받는다.
		 * 읽는 자: __qcom_pcie_perst_assert() 의 gpiod_set_value_cansleep().
		 * 값 범위: 레거시 바인딩에서는 NULL 일 수 있고, GPIO API 가 무해하게 처리한다.
		 * 동기화: devm 수명. */
	struct gpio_desc *desc;
};

/* [한국어] DT 의 포트 노드 하나에 대응하는 구조체.
 * 컨트롤러가 여러 포트를 가질 수 있어 이것도 목록으로 다룬다.
 * 레거시 바인딩에서는 이 목록에 항목 하나만 들어간다. */
struct qcom_pcie_port {
		/* [한국어] pcie->ports 목록에 연결되는 고리.
		 * 설정자: qcom_pcie_parse_port() 또는 qcom_pcie_parse_legacy_binding().
		 * 읽는 자: PHY 조작과 PERST# 조작의 순회, 그리고 실패 경로의 삭제.
		 * 값 범위: 매달기 전에 초기화된다.
		 * 동기화: probe 에서 만들고 이후 구조가 바뀌지 않는다. */
	struct list_head list;
		/* [한국어] 이 포트의 PHY.
		 * 설정자: qcom_pcie_parse_port() 의 devm_of_phy_get(), 또는 레거시
		 *          바인딩의 devm_phy_optional_get().
		 * 읽는 자: qcom_pcie_phy_power_on/off() 의 순회.
		 * 값 범위: 레거시 경로에서는 NULL 일 수 있다(optional).
		 * 동기화: devm 수명. */
	struct phy *phy;
		/* [한국어] 이 포트에 딸린 PERST# 들의 목록 머리.
		 * 설정자: 파싱 함수가 INIT_LIST_HEAD 로 초기화한 뒤 채운다.
		 * 읽는 자: __qcom_pcie_perst_assert() 의 안쪽 루프.
		 * 값 범위: 비어 있을 수 있다 — DT 가 reset-gpios 를 주지 않은 포트다.
		 * 동기화: probe 에서 만들고 이후 구조가 바뀌지 않는다. */
	struct list_head perst;
};

/* [한국어] 컨트롤러 하나의 모든 상태.
 * devm 으로 잡아 platform_set_drvdata 로 걸어 두며, to_qcom_pcie 매크로가
 * dw_pcie->dev 의 drvdata 로 되찾는다. 전역 가변 상태는 없다. */
struct qcom_pcie {
		/* [한국어] DWC 코어의 컨트롤러 구조체.
		 * 설정자: qcom_pcie_probe() 가 따로 잡아 연결한다. brcmstb 나 rzg3s 가
		 *          호스트 브리지의 private 영역을 쓰는 것과 달리, 여기서는 DWC
		 *          코어가 그 자리를 쓰므로 별도 할당이다.
		 * 읽는 자: 거의 모든 함수. dev, dbi_base, pp 를 여기서 얻는다.
		 * 값 범위: NULL 불가.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	struct dw_pcie *pci;
	void __iomem *parf; /* [한국어] 퀄컴 고유 제어 레지스터 창(상류 주석대로 DT 의 "parf"). 이 파일의 거의 모든 writel 이 여기를 향한다. 설정자는 probe 의 devm_platform_ioremap_resource_byname(), NULL 불가 */
		/* [한국어] MHI 레지스터 창. 링크 전이 카운터가 여기 있다.
		 * 설정자: qcom_pcie_probe() 가 DT 의 "mhi" 자원이 있을 때만 매핑한다.
		 * 읽는 자: qcom_pcie_link_transition_count().
		 * 값 범위: NULL 이면 probe 가 debugfs 자체를 만들지 않는다 — 읽을
		 *          카운터가 없기 때문이다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	void __iomem *mhi;
		/* [한국어] 세대별 자원 묶음. union 이라 한 세대 것만 유효하다.
		 * 설정자/읽는 자: cfg->ops 가 가리키는 세대의 함수들만 자기 멤버를 쓴다.
		 * 값 범위: 어느 멤버가 유효한지는 cfg 가 정한다 — 잘못된 멤버를 읽으면
		 *          쓰레기 값이 나오므로, 세대별 함수가 자기 것만 꺼내는 규약이
		 *          이 union 의 안전성을 지탱한다.
		 * 동기화: probe/PM 경로 전용. */
	union qcom_pcie_resources res;
		/* [한국어] PCIe 장치 ↔ DRAM 인터커넥트 경로.
		 * 설정자: qcom_pcie_icc_init() — OPP 표가 없는 플랫폼에서만 잡는다.
		 * 읽는 자: qcom_pcie_icc_opp_update() 와 suspend/resume.
		 * 값 범위: OPP 를 쓰는 플랫폼에서는 NULL 이고, 그 사실이 곧 두 전력
		 *          관리 방식을 가르는 분기 조건이 된다.
		 * 동기화: probe/PM 경로 전용. */
	struct icc_path *icc_mem;
		/* [한국어] CPU ↔ 컨트롤러 인터커넥트 경로.
		 * 설정자: qcom_pcie_icc_init().
		 * 읽는 자: suspend 가 조건부로 끄고 resume 이 켠다.
		 * 값 범위: 상류 주석대로 레지스터·config 접근용이라 최소 대역폭(1KBps)만
		 *          요구한다.
		 * 동기화: probe/PM 경로 전용. */
	struct icc_path *icc_cpu;
		/* [한국어] 이 SoC 의 설정 표.
		 * 설정자: qcom_pcie_probe() 의 of_device_get_match_data().
		 * 읽는 자: 세대 분기가 있는 거의 모든 함수.
		 * 값 범위: NULL 이면 probe 가 -ENODATA 로 돌아가므로 이후에는 늘 유효.
		 * 동기화: 읽기 전용 상수를 가리킨다. */
	const struct qcom_pcie_cfg *cfg;
		/* [한국어] 이 컨트롤러의 debugfs 디렉터리.
		 * 설정자: qcom_pcie_init_debugfs() 가 DT 노드 경로를 이름으로 만든다.
		 * 읽는 자: 없음 — 이 파일 어디에서도 이 디렉터리를 지우지 않는다.
		 *          상류 코드 그대로다.
		 * 값 범위: mhi 창이 없으면 만들어지지 않아 NULL 로 남는다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	struct dentry *debugfs;
		/* [한국어] 이 컨트롤러의 포트 목록 머리.
		 * 설정자: probe 가 INIT_LIST_HEAD 로 초기화하고 파싱 함수가 채운다.
		 * 읽는 자: PHY·PERST# 조작의 순회.
		 * 값 범위: 새 바인딩이면 포트 수만큼, 레거시면 하나.
		 * 동기화: probe 에서 만들고 이후 구조가 바뀌지 않는다. */
	struct list_head ports;
		/* [한국어] suspend 가 실제로 컨트롤러를 껐는지.
		 * 설정자: qcom_pcie_suspend_noirq() 가 링크가 서 있지 않을 때만 참으로 둔다.
		 * 읽는 자: qcom_pcie_resume_noirq() 가 이 값으로 되살릴지 정한다.
		 * 값 범위: 활성 장치가 붙어 있으면 끄지 않으므로 거짓으로 남는다 —
		 *          그 판단의 근거가 suspend 의 상류 주석(NVMe 수명 언급)이다.
		 * 동기화: suspend/resume 이 번갈아 실행되므로 경쟁이 없다. */
	bool suspended;
		/* [한국어] OPP 기반 전력 관리를 쓰는지.
		 * 설정자: qcom_pcie_probe() 가 DT 에 OPP 표가 있을 때 참으로 둔다.
		 * 읽는 자: qcom_pcie_icc_opp_update() 와 suspend 가 icc 갈래와 OPP 갈래를
		 *          가르는 데 쓴다.
		 * 값 범위: 거짓이면 icc_mem/icc_cpu 가 대신 유효하다 — 둘은 배타적이다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	bool use_pm_opp;
};

/* [한국어] dw_pcie 포인터에서 이 파일의 컨트롤러 상태를 되찾는 매크로.
 * DWC 코어의 콜백들이 struct dw_pcie 만 넘겨 주므로, 그 dev 의 drvdata 로
 * 거슬러 올라간다. probe 의 platform_set_drvdata(pdev, pcie) 와 짝을 이룬다. */
#define to_qcom_pcie(x)		dev_get_drvdata((x)->dev)

/* [한국어]
 * __qcom_pcie_perst_assert - 이 컨트롤러에 딸린 모든 PERST# 를 한꺼번에 조작한다
 *
 * @pcie:   컨트롤러 상태.
 * @assert: true 면 PERST# 를 걸고(장치 리셋), false 면 푼다.
 *
 * 이 드라이버는 PERST# 를 하나가 아니라 목록으로 다룬다. 포트마다
 * struct qcom_pcie_port 가 있고 그 안에 다시 PERST# 목록이 있어, 이중 루프로
 * 전부 훑는다. DT 의 포트 노드 아래 여러 슬롯이 각자 reset-gpios 를 가질 수
 * 있기 때문이며, qcom_pcie_parse_perst() 가 그 트리를 깊이 우선으로 훑어
 * 목록을 만든다.
 *
 * GPIO 조작에 cansleep 판을 쓴다. PERST# 가 I2C 확장기 같은 느린 버스 뒤에
 * 있을 수 있어, 잠들 수 있는 문맥에서만 부를 수 있다.
 *
 * 마지막 지연이 이 함수의 일부라는 점이 중요하다. 걸든 풀든 신호가 안정될
 * 시간을 주며, 그래서 호출자 둘 다 별도 지연을 두지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:  qcom_pcie_perst_assert() / qcom_pcie_perst_deassert()
 *               → [이 함수] → gpiod_set_value_cansleep()
 */
static void __qcom_pcie_perst_assert(struct qcom_pcie *pcie, bool assert)
{
	struct qcom_pcie_perst *perst; /* [한국어] 안쪽 루프의 순회 항목 */
	struct qcom_pcie_port *port; /* [한국어] 바깥 루프의 순회 항목. 포트마다 PERST# 목록이 따로 있다 */
	int val = assert ? 1 : 0; /* [한국어] GPIO 에 넣을 값. PERST# 는 active-low 라 1 이 곧 리셋 인가다 */

	list_for_each_entry(port, &pcie->ports, list) { /* [한국어] 이 컨트롤러의 모든 포트를 훑는다 */
		list_for_each_entry(perst, &port->perst, list) /* [한국어] 포트마다 딸린 PERST# 를 모두 훑는다 */
			gpiod_set_value_cansleep(perst->desc, val); /* [한국어] 느린 버스 뒤의 GPIO 일 수 있어 cansleep 판을 쓴다 */
	}

	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500); /* [한국어] 걸든 풀든 신호가 안정될 시간을 준다. 이 지연이 함수의 일부라 호출자는 따로 기다리지 않는다 */
}

/* [한국어]
 * qcom_pcie_perst_assert - PERST# 를 걸어 하류 장치를 리셋에 넣는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 인자를 고정한 얇은 래퍼다. 호출자가 true/false 를 잘못 넘길 여지를 없애고,
 * 호출부에서 의도가 이름으로 드러나게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() / qcom_pcie_host_deinit() → [이 함수]
 */
static void qcom_pcie_perst_assert(struct qcom_pcie *pcie)
{
	__qcom_pcie_perst_assert(pcie, true);
}

/* [한국어]
 * qcom_pcie_perst_deassert - 규정 시간을 지킨 뒤 PERST# 를 푼다
 *
 * @pcie: 컨트롤러 상태.
 *
 * assert 판과 달리 앞에 지연이 하나 더 붙는다. 상류 주석이 그 이유를 밝힌다 —
 * PERST# 는 최소 100ms 동안 걸려 있어야 한다. PCIE_T_PVPERL_MS 가 그 값이며,
 * 전원이 안정된 뒤 리셋을 풀기까지의 스펙 규정 시간이다.
 *
 * 이 지연이 여기 있는 덕분에 호출자(qcom_pcie_host_init)는 assert 와 deassert
 * 사이에 무엇을 하든 시간 규정을 따로 지킬 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → [이 함수] → __qcom_pcie_perst_assert()
 */
static void qcom_pcie_perst_deassert(struct qcom_pcie *pcie)
{
	/* Ensure that PERST# has been asserted for at least 100 ms */
	msleep(PCIE_T_PVPERL_MS);
	__qcom_pcie_perst_assert(pcie, false);
}

/* [한국어]
 * qcom_pcie_start_link - 링크 학습을 시작한다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 * @return: 늘 0.
 *
 * struct dw_pcie_ops 의 start_link 로 등록되어 DWC 코어가 부른다.
 *
 * 세 단계인데, 앞의 둘이 **EP 드라이버와 공유하는 부분**이다.
 *   1) qcom_pcie_common_set_equalization() — Gen3 이상에서 필요한 송신
 *      이퀄라이제이션 preset 을 적는다.
 *   2) 최대 속도가 16GT/s(Gen4)이면
 *      qcom_pcie_common_set_16gt_lane_margining() 으로 레인 마진 기능을 켠다.
 *   3) 세대별 ltssm_enable 콜백으로 링크 학습을 켠다.
 *
 * 1)과 2)는 pcie-qcom-common.c 에 있고, pcie-qcom-ep.c 의
 * qcom_pcie_perst_deassert() 도 같은 조건으로 같은 순서로 부른다. 물리 계층
 * 설정이라 링크 방향과 무관하기 때문이다. 반면 3)은 레지스터 자리가 세대마다
 * 달라 이 파일의 콜백으로 남았다.
 *
 * ltssm_enable 이 NULL 인 표는 이 파일에 없지만, 그래도 확인 후 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 초기화 중).
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.start_link → [이 함수]
 *               → qcom_pcie_common_set_equalization() → cfg->ops->ltssm_enable()
 */
static int qcom_pcie_start_link(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* [한국어] DWC 코어가 넘긴 구조체에서 이 파일의 상태를 되찾는다 */

	qcom_pcie_common_set_equalization(pci); /* [한국어] EP 파일과 공유하는 함수. Gen3 이상에서 필요한 송신 이퀄라이제이션 preset 을 적는다 */

	if (pcie_get_link_speed(pci->max_link_speed) == PCIE_SPEED_16_0GT) /* [한국어] 최대 속도가 Gen4(16GT/s)이면 */
		qcom_pcie_common_set_16gt_lane_margining(pci); /* [한국어] 같은 공유 파일의 레인 마진 설정도 켠다. EP 판이 같은 조건으로 같은 순서로 부른다 */

	/* Enable Link Training state machine */
	if (pcie->cfg->ops->ltssm_enable) /* [한국어] 세대별 구현이 있으면 */
		pcie->cfg->ops->ltssm_enable(pcie); /* [한국어] 구세대는 ELBI, 신세대는 PARF_LTSSM 에 활성 비트를 쓴다 */

	return 0; /* [한국어] DWC 코어는 반환값으로 링크 성공을 판정하지 않고 별도로 링크업을 기다린다 */
}

/* [한국어]
 * qcom_pcie_clear_aspm_l0s - L0s 를 광고하지 않게 능력 비트를 지운다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 *
 * cfg->no_l0s 가 참인 SoC 에서만 동작한다. 지금 그 플래그를 켠 표는
 * cfg_2_3_2 와 cfg_sc8280xp 둘이다.
 *
 * RC 자신의 config 공간에서 링크 능력 레지스터의 L0s 비트를 지운다. 그러면
 * OS 가 L0s 를 켜려 하지 않는다. 능력 필드는 보통 읽기 전용이라
 * dw_pcie_dbi_ro_wr_en/dis 로 쓰기 창을 열었다 닫는 것이 요점이다.
 *
 * 플래그가 꺼진 SoC 에서는 곧바로 돌아가므로, 호출자가 SoC 를 가릴 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 초기화 중).
 *
 * 호출 체인:  qcom_pcie_host_init() → [이 함수]
 */
static void qcom_pcie_clear_aspm_l0s(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* [한국어] DWC 구조체에서 이 파일의 상태를 되찾는다 */
	u16 offset; /* [한국어] PCIe capability 의 config 공간 오프셋 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	if (!pcie->cfg->no_l0s) /* [한국어] 이 SoC 가 L0s 를 광고해도 되는 구성이면 */
		return; /* [한국어] 아무 일도 하지 않는다 — 호출자가 SoC 를 가리지 않아도 되는 이유다 */

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 가 config 공간 어디에 있는지 찾는다 */

	dw_pcie_dbi_ro_wr_en(pci); /* [한국어] 능력 필드는 보통 읽기 전용이라 쓰기 창을 연다 */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 링크 능력 레지스터를 읽는다 */
	val &= ~PCI_EXP_LNKCAP_ASPM_L0S; /* [한국어] L0s 지원 비트를 지운다 */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 되쓴다 — 이제 OS 가 L0s 를 켜려 하지 않는다 */

	dw_pcie_dbi_ro_wr_dis(pci); /* [한국어] 쓰기 창을 닫는다. 열어 둔 채로 두면 뜻하지 않은 쓰기가 능력을 바꿀 수 있다 */
}

/* [한국어]
 * qcom_pcie_set_slot_nccs - 핫플러그 명령 완료 통지를 쓰지 않는다고 표시한다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 *
 * 상류 주석이 이유를 그대로 밝힌다 — 퀄컴 루트 포트는 핫플러그 명령에 대한
 * 완료 통지(command completed)를 만들어 내지 못한다. 그 사실을 슬롯 능력의
 * NCCS(No Command Completed Support) 비트로 알려 두지 않으면, 핫플러그
 * 드라이버가 오지 않을 통지를 기다리며 매번 타임아웃을 낸다.
 *
 * 능력 필드를 고치는 것이라 여기서도 쓰기 창을 열었다 닫는다.
 *
 * 세대별 post_init 여럿이 이 함수를 부른다(2_1_0 / 1_0_0 / 2_3_2 / 2_7_0).
 * 반면 2_3_3 과 2_9_0 은 부르지 않고 PCIE_CAP_SLOT_VAL 을 통째로 써 넣는데,
 * 그 상수에는 NCCS 가 들어 있지 않다. 그 차이가 의도된 것인지는 이 트리에서
 * 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(post_init 중).
 *
 * 호출 체인:  세대별 post_init → [이 함수]
 */
static void qcom_pcie_set_slot_nccs(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 오프셋을 선언과 동시에 구한다 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	dw_pcie_dbi_ro_wr_en(pci); /* [한국어] 슬롯 능력도 읽기 전용이라 쓰기 창을 연다 */

	/*
	 * Qcom PCIe Root Ports do not support generating command completion
	 * notifications for the Hot-Plug commands. So set the NCCS field to
	 * avoid waiting for the completions.
	 */
	val = readl(pci->dbi_base + offset + PCI_EXP_SLTCAP); /* [한국어] 슬롯 능력 레지스터를 읽는다 */
	val |= PCI_EXP_SLTCAP_NCCS; /* [한국어] 상류 주석대로 완료 통지를 만들지 못하므로 그 사실을 NCCS 비트로 알린다 */
	writel(val, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* [한국어] 되쓴다. 이것이 없으면 핫플러그 드라이버가 오지 않을 통지를 기다리며 타임아웃을 낸다 */

	dw_pcie_dbi_ro_wr_dis(pci); /* [한국어] 쓰기 창을 닫는다 */
}

/* [한국어]
 * qcom_pcie_configure_dbi_base - PARF 에 DBI 물리 주소를 알려 준다(구세대 판)
 *
 * @pcie: 컨트롤러 상태.
 *
 * 상류 주석이 핵심을 짚는다 — PARF_DBI_BASE_ADDR 레지스터는 CPU 도메인에
 * 있어서 **CPU 물리 주소**를 넣어야 한다. 그래서 ioremap 된 가상 주소
 * (pci->dbi_base)가 아니라 pci->dbi_phys_addr 를 쓴다.
 *
 * 주소를 알려 주는 이유는 PARF 블록이 DBI 영역으로 가는 접근을 가로채
 * 처리해야 하기 때문으로 보이며, 그 구체적 동작은 이 트리에서 확인 못 함.
 *
 * 주소 창 크기도 함께 못박는다.
 *
 * dbi_phys_addr 가 0 이면 아무 일도 하지 않는다. DT 가 dbi 자원을 주지 않은
 * 구성을 위한 것이다.
 *
 * _V2 판(qcom_pcie_configure_dbi_atu_base)과의 차이는 레지스터 자리와
 * 다루는 항목 수다. 그 차이가 세대 구분의 대표적인 예다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(post_init 중).
 *
 * 호출 체인:  qcom_pcie_post_init_1_0_0() / qcom_pcie_post_init_2_3_2()
 *               → [이 함수]
 */
static void qcom_pcie_configure_dbi_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 물리 주소를 담고 있는 DWC 구조체 */

	if (pci->dbi_phys_addr) { /* [한국어] DT 가 dbi 자원을 주지 않았으면 알려 줄 주소가 없다 */
		/*
		 * PARF_DBI_BASE_ADDR register is in CPU domain and require to
		 * be programmed with CPU physical address.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR); /* [한국어] 상류 주석대로 CPU 도메인 레지스터라 가상 주소가 아닌 물리 주소를 넣는다 */
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
							PARF_SLV_ADDR_SPACE_SIZE); /* [한국어] 슬레이브 주소 공간 크기(2GB)도 함께 못박는다 */
	}
}

/* [한국어]
 * qcom_pcie_configure_dbi_atu_base - PARF 에 DBI 와 iATU 물리 주소를 알려 준다(신세대 판)
 *
 * @pcie: 컨트롤러 상태.
 *
 * 구세대 판이 하는 일에 두 가지가 더해진다.
 *
 *   1) DBI 주소를 상위/하위 두 워드로 나눠 적는다. 신세대 SoC 는 64비트
 *      주소를 다루기 때문이다.
 *   2) iATU(주소 변환 유닛) 레지스터 블록의 물리 주소도 함께 적는다.
 *      DWC 5.x 계열에서 iATU 가 DBI 와 분리된 창으로 옮겨 갔기 때문이며,
 *      atu_phys_addr 가 0 이면(즉 분리되지 않은 구성이면) 그 부분을 건너뛴다.
 *
 * 상류 주석이 다시 한 번 "CPU 도메인이라 CPU 물리 주소를 넣어야 한다" 고
 * 못박는다.
 *
 * 창 크기를 적는 방식도 갈린다. 하위 워드에 0 을, 상위 워드에 크기를 넣는데
 * 그 배치의 근거는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(post_init 중).
 *
 * 호출 체인:  qcom_pcie_post_init_2_3_3() / qcom_pcie_init_2_7_0() /
 *               qcom_pcie_post_init_2_9_0() → [이 함수]
 */
static void qcom_pcie_configure_dbi_atu_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 와 iATU 물리 주소를 담고 있는 DWC 구조체 */

	if (pci->dbi_phys_addr) { /* [한국어] DT 가 dbi 자원을 주지 않았으면 알려 줄 주소가 없다 */
		/*
		 * PARF_DBI_BASE_ADDR_V2 and PARF_ATU_BASE_ADDR registers are
		 * in CPU domain and require to be programmed with CPU
		 * physical addresses.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR_V2); /* [한국어] DBI 물리 주소의 하위 워드 */
		writel(upper_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR_V2_HI); /* [한국어] 그 상위 워드. 신세대는 64비트 주소를 다뤄 둘로 나뉜다 */

		if (pci->atu_phys_addr) { /* [한국어] iATU 가 DBI 와 분리된 창으로 옮겨 간 구성에서만 */
			writel(lower_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR); /* [한국어] iATU 물리 주소의 하위 워드 */
			writel(upper_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR_HI); /* [한국어] 그 상위 워드 */
		}

		writel(0x0, pcie->parf + PARF_SLV_ADDR_SPACE_SIZE_V2); /* [한국어] 슬레이브 주소 공간 크기의 하위 워드에 0 을 쓴다. 그 배치의 근거는 이 트리에서 확인 못 함 */
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
						PARF_SLV_ADDR_SPACE_SIZE_V2_HI); /* [한국어] 실제 크기는 상위 워드에 넣는다 */
	}
}

/* [한국어]
 * qcom_pcie_2_1_0_ltssm_enable - ELBI 레지스터로 링크 학습을 켠다(구세대 판)
 *
 * @pcie: 컨트롤러 상태.
 *
 * 이 파일에 ltssm_enable 구현이 둘뿐인 이유가 여기 있다. 구세대 IP 는 링크
 * 학습 활성 비트가 ELBI 블록에 있고, 2.3.2 이후 세대는 PARF_LTSSM 으로
 * 옮겨 갔다. 그 한 가지 차이가 콜백 하나를 만든다.
 *
 * ELBI 창이 없으면 오류만 찍고 돌아간다. DT 가 elbi 자원을 주지 않은
 * 구성인데, 그 경우 링크가 서지 않으므로 위쪽에서 링크업 대기가 실패한다.
 *
 * 이름은 2_1_0 이지만 ops_1_0_0 도 이 함수를 쓴다 — 두 세대가 같은 ELBI
 * 배치를 갖기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_start_link() → cfg->ops->ltssm_enable → [이 함수]
 */
static void qcom_pcie_2_1_0_ltssm_enable(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] ELBI 창 주소를 담고 있는 DWC 구조체 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	if (!pci->elbi_base) { /* [한국어] DT 가 elbi 자원을 주지 않았으면 */
		dev_err(pci->dev, "ELBI is not present\n"); /* [한국어] 링크 학습을 켤 방법이 없다 */
		return; /* [한국어] 반환형이 void 라 오류를 올릴 수 없고, 위쪽의 링크업 대기가 대신 실패한다 */
	}
	/* enable link training */
	val = readl(pci->elbi_base + ELBI_SYS_CTRL); /* [한국어] 현재 값을 읽는다 */
	val |= ELBI_SYS_CTRL_LT_ENABLE; /* [한국어] 링크 학습 활성 비트를 세운다 */
	writel(val, pci->elbi_base + ELBI_SYS_CTRL); /* [한국어] 되쓴다. 신세대는 같은 일을 PARF_LTSSM 에서 한다 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_1_0 - 2.1.0 세대(apq8064/ipq8064)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 자원 조회 실패는 그 오류.
 *
 * 이 파일에서 가장 오래된 세대다. 필요한 것이 셋이다.
 *
 *   - 레귤레이터 셋(vdda / vdda_phy / vdda_refclk). PHY 전원까지 드라이버가
 *     직접 다루는 것이 구세대의 특징이다.
 *   - 클록 전부. 이름을 따지지 않고 devm_clk_bulk_get_all 로 통째로 가져온다.
 *     이 파일의 모든 세대가 같은 방식을 쓴다 — 세대마다 클록 이름과 개수가
 *     다르므로, DT 가 준 것을 그대로 받는 편이 표를 두는 것보다 낫다.
 *   - 리셋 여섯(pci/axi/ahb/por/phy/ext). 이름을 배열에 꽂아 bulk 로 잡는다.
 *
 * 마지막 한 줄이 이 세대의 유일한 SoC 분기다. 상류 주석대로 apq8064 에서는
 * "ext" 리셋이 없어 개수를 5 로 줄인다. compatible 문자열을 직접 비교하는
 * 드문 자리이며, cfg 표를 늘리지 않고 처리한 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* [한국어] union 에서 이 세대의 자원 묶음만 꺼내 쓴다 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회와 로그의 기준 */
	bool is_apq = of_device_is_compatible(dev->of_node, "qcom,pcie-apq8064"); /* [한국어] 이 세대에서 유일한 SoC 직접 판별. 아래 리셋 개수를 가르는 데만 쓴다 */
	int ret; /* [한국어] 각 단계의 결과 */

	res->supplies[0].supply = "vdda"; /* [한국어] PHY 코어 전원 */
	res->supplies[1].supply = "vdda_phy"; /* [한국어] PHY 아날로그 전원 */
	res->supplies[2].supply = "vdda_refclk"; /* [한국어] 기준 클록 전원. 구세대가 PHY 전원까지 직접 다루는 것을 보여 준다 */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* [한국어] 꽂아 둔 이름으로 셋을 한꺼번에 조회한다 */
	if (ret) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 이름을 따지지 않고 DT 가 준 클록을 통째로 받는다 — 이 파일 모든 세대의 공통 방식이다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 오류로 그대로 올린다 */
	}

	res->resets[0].id = "pci"; /* [한국어] PCI 코어 리셋 */
	res->resets[1].id = "axi"; /* [한국어] AXI 리셋 */
	res->resets[2].id = "ahb"; /* [한국어] AHB 리셋 */
	res->resets[3].id = "por"; /* [한국어] POR(전원 인가) 리셋 */
	res->resets[4].id = "phy"; /* [한국어] PHY 리셋 */
	res->resets[5].id = "ext"; /* [한국어] 확장 리셋. 이 여섯 번째만 SoC 에 따라 없을 수 있다 */

	/* ext is optional on APQ8016 */
	res->num_resets = is_apq ? 5 : 6; /* [한국어] 상류 주석대로 APQ8016 에서는 "ext" 가 선택이라 개수를 하나 줄인다 */
	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets); /* [한국어] 그 개수만큼만 조회한다 — 배열은 6칸이지만 5개만 쓴다 */
	if (ret < 0) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	return 0; /* [한국어] 이 세대의 자원 확보 완료 */
}

/* [한국어]
 * qcom_pcie_deinit_2_1_0 - 2.1.0 세대의 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 잡은 역순이다 — 클록을 끄고, 리셋을 걸고, PHY 를 파워다운으로 두고,
 * 레귤레이터를 끈다.
 *
 * 가운데의 writel 이 눈에 띈다. PARF_PHY_CTRL 에 1 을 쓰는데, 그 레지스터의
 * 0번 비트가 PHY_TEST_PWR_DOWN 이므로 PHY 를 파워다운 상태로 되돌리는 것이다.
 * post_init 에서 그 비트를 지워 PHY 를 켰던 것의 역이며, 상수 이름 대신 1 을
 * 쓴 것은 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 정지/실패 경로).
 *
 * 호출 체인:  qcom_pcie_host_deinit() / qcom_pcie_host_init()(실패 경로)
 *               → cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* [한국어] 이 세대의 자원 묶음 */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 먼저 끈다 */
	reset_control_bulk_assert(res->num_resets, res->resets); /* [한국어] 리셋을 건다 */

	writel(1, pcie->parf + PARF_PHY_CTRL); /* [한국어] PARF_PHY_CTRL 의 0번 비트가 PHY_TEST_PWR_DOWN 이라, 1 을 쓰면 PHY 가 파워다운으로 돌아간다. 상수 대신 1 을 쓴 것은 상류 코드 그대로다 */

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 마지막으로 전원을 끈다 — 켠 역순이다 */
}

/* [한국어]
 * qcom_pcie_init_2_1_0 - 2.1.0 세대의 리셋과 전원을 세운다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 상류 주석이 첫 단계의 이유를 밝힌다 — u-boot 가 컨트롤러를 정의되지 않은
 * 상태로 남겨 둘 수 있어, 먼저 리셋을 걸어 상태를 확실히 한다.
 *
 * 순서가 리셋 → 전원 → 리셋 해제다. 전원이 들어온 뒤에 리셋을 풀어야
 * 하드웨어가 안정된 전압에서 시작한다.
 *
 * 실패 되돌리기가 마지막 단계에만 있다. 레귤레이터를 켠 뒤 리셋 해제가
 * 실패하면 그것을 다시 끄고 돌아간다. 첫 단계 실패는 아직 아무것도 켜지
 * 않았으므로 되돌릴 것이 없다.
 *
 * 세대 사이의 대비가 여기서 드러난다 — 2.1.0 은 init 에서 리셋과 전원만
 * 다루고 PARF 프로그래밍을 post_init 으로 미루는 반면, 2.7.0 은 그 둘을
 * init 하나에 합쳐 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	/* reset the PCIe interface as uboot can leave it undefined state */
	ret = reset_control_bulk_assert(res->num_resets, res->resets); /* [한국어] 상류 주석대로 u-boot 가 남긴 정의되지 않은 상태를 지우려 먼저 리셋을 건다 */
	if (ret < 0) { /* [한국어] 리셋조차 걸지 못했으면 */
		dev_err(dev, "cannot assert resets\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않아 되돌릴 것이 없다 */
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 켠다. 리셋이 걸린 상태에서 켜야 안정된 전압에서 시작한다 */
	if (ret < 0) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot enable regulators\n");
		return ret; /* [한국어] 그대로 돌아간다 */
	}

	ret = reset_control_bulk_deassert(res->num_resets, res->resets); /* [한국어] 이제 리셋을 푼다 */
	if (ret < 0) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "cannot deassert resets\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 방금 켠 전원을 되돌리고 */
		return ret; /* [한국어] 오류를 올린다 */
	}

	return 0; /* [한국어] 이 세대의 기동 준비 완료. PARF 프로그래밍은 post_init 이 맡는다 */
}

/* [한국어]
 * qcom_pcie_post_init_2_1_0 - 2.1.0 세대의 PHY·PARF 를 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 클록 활성 실패는 그 오류.
 *
 * 이 세대의 PARF 프로그래밍 본체다. 구세대라 다뤄야 할 아날로그 항목이
 * 많다는 점이 신세대와의 큰 차이다.
 *
 *   1) PHY 파워다운을 해제한다.
 *   2) 클록을 켠다. init 이 아니라 여기서 켜는 것이 이 세대의 배치다.
 *   3) ipq8064 계열이면 PHY 의 디엠퍼시스와 스윙, RX 이퀄라이저 값을 적는다.
 *      각 숫자(24/34/120/4 등)의 근거는 퀄컴 문서에 있을 것으로 보이나 이
 *      트리에서 확인 못 함.
 *   4) ipq8064 이면 TX 종단 오프셋을 7 로 맞춘다.
 *   5) 외부 기준 클록을 켠다. 상류 주석대로 USE_PAD 는 ipq806x 계열에만
 *      필요해, apq8064 가 아닌 경우 그 비트를 지운다. 조건이 부정형인 것에
 *      주의 — apq8064 만 비트를 남긴다.
 *   6) 클록이 잡힐 때까지 기다린다.
 *   7) 상류 주석대로 최대 TLP 크기를 기본 4K 대신 2K 로 낮춘다.
 *   8) 핫플러그 완료 통지 없음을 표시한다.
 *
 * 3)~5)의 compatible 비교가 이 파일에서 SoC 를 직접 이름으로 가르는 몇 안 되는
 * 자리다. 같은 ops 표를 쓰면서도 보드별 아날로그 값이 달라 표로 흡수하기
 * 어려웠던 것으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 창과 device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	struct device_node *node = dev->of_node; /* [한국어] 아래 SoC 직접 판별에 쓸 DT 노드 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */
	int ret; /* [한국어] 클록 활성 결과 */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 읽는다 */
	val &= ~PHY_TEST_PWR_DOWN; /* [한국어] 파워다운 비트를 지운다 */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 — 이제 PHY 가 켜진다 */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 이 세대는 클록을 init 이 아니라 여기서 켠다 */
	if (ret) /* [한국어] 못 켰으면 */
		return ret; /* [한국어] 그대로 올린다 */

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064") ||
	    of_device_is_compatible(node, "qcom,pcie-ipq8064-v2")) { /* [한국어] ipq8064 계열 두 SoC 에서만 아날로그 값을 손본다 */
		writel(PCS_DEEMPH_TX_DEEMPH_GEN1(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(34),
		       pcie->parf + PARF_PCS_DEEMPH); /* [한국어] Gen1/Gen2 디엠퍼시스 세 필드를 한 번에 적는다. 24/24/34 의 근거는 퀄컴 문서에 있을 것으로 보이나 이 트리에서 확인 못 함 */
		writel(PCS_SWING_TX_SWING_FULL(120) |
			       PCS_SWING_TX_SWING_LOW(120),
		       pcie->parf + PARF_PCS_SWING); /* [한국어] 송신 스윙 최대/최소를 120 으로 맞춘다 */
		writel(PHY_RX0_EQ(4), pcie->parf + PARF_CONFIG_BITS); /* [한국어] RX 이퀄라이저를 4 로 둔다 */
	}

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064")) { /* [한국어] ipq8064 하나만 종단 오프셋을 손본다 */
		/* set TX termination offset */
		val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 다시 읽는다 */
		val &= ~PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK; /* [한국어] 종단 오프셋 필드를 비우고 */
		val |= PHY_CTRL_PHY_TX0_TERM_OFFSET(7); /* [한국어] 상류 주석대로 7 로 맞춘다 */
		writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 */
	}

	/* enable external reference clock */
	val = readl(pcie->parf + PARF_PHY_REFCLK); /* [한국어] 기준 클록 제어 레지스터를 읽는다 */
	/* USE_PAD is required only for ipq806x */
	if (!of_device_is_compatible(node, "qcom,pcie-apq8064")) /* [한국어] 상류 주석대로 USE_PAD 는 ipq806x 계열에만 필요하다. 조건이 부정형이라 apq8064 만 그 비트를 남긴다 */
		val &= ~PHY_REFCLK_USE_PAD; /* [한국어] 다른 SoC 에서는 패드 클록을 쓰지 않는다 */
	val |= PHY_REFCLK_SSP_EN; /* [한국어] 대역 확산 클록을 켠다 */
	writel(val, pcie->parf + PARF_PHY_REFCLK); /* [한국어] 되쓴다 */

	/* wait for clock acquisition */
	usleep_range(1000, 1500); /* [한국어] 상류 주석대로 클록이 잡힐 때까지 기다린다 */

	/* Set the Max TLP size to 2K, instead of using default of 4K */
	writel(CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL0); /* [한국어] 상류 주석대로 최대 TLP 크기를 기본 4K 대신 2K 로 낮춘다 */
	writel(CFG_BRIDGE_SB_INIT,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL1); /* [한국어] 브리지 사이드밴드 초기화 비트를 세운다 */

	qcom_pcie_set_slot_nccs(pcie->pci); /* [한국어] 핫플러그 완료 통지를 만들지 못한다는 사실을 표시한다 */

	return 0; /* [한국어] 이 세대의 PARF 프로그래밍 완료 */
}

/* [한국어]
 * qcom_pcie_get_resources_1_0_0 - 1.0.0 세대(apq8084)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 2.1.0 보다 단순하다 — 레귤레이터 하나(vdda), 클록 전부, 리셋 하나(core).
 *
 * 리셋이 하나뿐인 것이 이 세대의 특징이다. 2.1.0 이 여섯, 2.4.0 이 최대
 * 열둘을 다루는 것과 대비된다. 같은 벤더의 IP 라도 리셋을 얼마나 잘게
 * 나눠 노출하느냐가 세대마다 달랐음을 보여 준다.
 *
 * 마지막이 PTR_ERR_OR_ZERO 인 것은 리셋 조회만 남았기 때문이다 — 성공이면
 * 0, 실패면 그 오류가 그대로 반환값이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */

	res->vdda = devm_regulator_get(dev, "vdda"); /* [한국어] 이 세대가 다루는 유일한 레귤레이터 */
	if (IS_ERR(res->vdda)) /* [한국어] 없거나 조회에 실패했으면 */
		return PTR_ERR(res->vdda); /* [한국어] 그 오류를 그대로 올린다 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 오류로 올린다 */
	}

	res->core = devm_reset_control_get_exclusive(dev, "core"); /* [한국어] 이 세대가 다루는 유일한 리셋 */
	return PTR_ERR_OR_ZERO(res->core); /* [한국어] 남은 것이 이 조회뿐이라, 성공이면 0 실패면 그 오류가 그대로 반환값이 된다 */
}

/* [한국어]
 * qcom_pcie_deinit_1_0_0 - 1.0.0 세대의 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * init 의 정확한 역순이다 — 리셋을 걸고, 클록을 끄고, 레귤레이터를 끈다.
 *
 * 2.1.0 판과 달리 PHY 를 따로 파워다운하지 않는다. 이 세대의 post_init 이
 * PHY_TEST_PWR_DOWN 을 건드리지 않기 때문이며, 그만큼 대칭이 깔끔하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* [한국어] 이 세대의 자원 묶음 */

	reset_control_assert(res->core); /* [한국어] 리셋을 먼저 건다 — init 의 정확한 역순이다 */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다 */
	regulator_disable(res->vdda); /* [한국어] 전원을 끈다. 이 세대는 PHY 파워다운을 따로 하지 않아 대칭이 깔끔하다 */
}

/* [한국어]
 * qcom_pcie_init_1_0_0 - 1.0.0 세대의 리셋·클록·전원을 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 순서가 2.1.0 과 반대다 — 여기서는 리셋을 **먼저 풀고** 클록과 전원을 켠다.
 * 2.1.0 은 리셋을 걸어 두고 전원을 켠 뒤에 풀었다. 같은 벤더 IP 의 기동
 * 순서가 세대마다 다르다는 것이 이 파일의 ops 표가 존재하는 이유를 그대로
 * 보여 준다.
 *
 * 되돌리기가 라벨 둘로 대칭을 이룬다 — 전원 실패는 클록부터, 클록 실패는
 * 리셋부터 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = reset_control_deassert(res->core); /* [한국어] 2.1.0 과 반대로 리셋을 먼저 푼다 — 같은 벤더 IP 라도 기동 순서가 세대마다 다르다 */
	if (ret) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "cannot deassert core reset\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록을 켠다 */
	if (ret) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_assert_reset; /* [한국어] 리셋을 되돌린다 */
	}

	ret = regulator_enable(res->vdda); /* [한국어] 전원을 켠다 */
	if (ret) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot enable vdda regulator\n");
		goto err_disable_clks; /* [한국어] 클록부터 되돌린다 */
	}

	return 0; /* [한국어] 이 세대의 기동 준비 완료 */

err_disable_clks: /* [한국어] 전원 실패가 여기로 온다 */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다 */
err_assert_reset: /* [한국어] 클록 실패가 여기로 온다 */
	reset_control_assert(res->core); /* [한국어] 리셋을 다시 건다 — 잡은 역순이다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_post_init_1_0_0 - 1.0.0 세대의 PARF 를 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * 이 파일에서 가장 짧은 post_init 이다. 세 가지뿐이다.
 *
 *   1) 구세대 판으로 DBI 물리 주소를 알려 준다.
 *   2) MSI 를 쓰는 커널이면 AXI 마스터 쓰기 주소 정지 기능을 켠다.
 *      MSI 가 특정 주소로의 쓰기로 구현되므로, 그 쓰기를 컨트롤러가 가로채
 *      처리하도록 하는 설정으로 보이나 구체적 동작은 이 트리에서 확인 못 함.
 *   3) 핫플러그 완료 통지 없음을 표시한다.
 *
 * 2)가 빌드 설정으로 갈리는 드문 자리다. 다른 세대는 MSI 여부와 무관하게
 * 같은 비트를 켜는데, 그 차이의 근거는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_1_0_0(struct qcom_pcie *pcie)
{
	qcom_pcie_configure_dbi_base(pcie); /* [한국어] 구세대 판으로 DBI 물리 주소를 알려 준다 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* [한국어] 커널에 MSI 지원이 들어 있을 때만. 다른 세대는 이 조건 없이 같은 비트를 켜는데, 그 차이의 근거는 이 트리에서 확인 못 함 */
		u32 val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* [한국어] AXI 마스터 쓰기 주소 정지 레지스터를 읽는다 */

		val |= EN; /* [한국어] 활성 비트를 세운다 */
		writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* [한국어] 되쓴다 */
	}

	qcom_pcie_set_slot_nccs(pcie->pci); /* [한국어] 핫플러그 완료 통지를 만들지 못한다는 사실을 표시한다 */

	return 0; /* [한국어] 이 파일에서 가장 짧은 PARF 프로그래밍이 끝났다 */
}

/* [한국어]
 * qcom_pcie_2_3_2_ltssm_enable - PARF_LTSSM 레지스터로 링크 학습을 켠다(신세대 판)
 *
 * @pcie: 컨트롤러 상태.
 *
 * 2.3.2 이후의 모든 세대가 이 함수를 쓴다 — ops_2_3_2 / ops_2_4_0 /
 * ops_2_3_3 / ops_2_7_0 / ops_1_9_0 / ops_1_21_0 / ops_2_9_0 이 모두 같은
 * 포인터를 넣는다. 즉 이 파일의 아홉 표 중 일곱이 여기로 모인다.
 *
 * 구세대 판과 다른 것은 레지스터 자리뿐이다. ELBI 가 아니라 PARF 블록 안의
 * 전용 레지스터에 활성 비트가 있고, 그래서 ELBI 창 유무를 확인할 필요도 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_start_link() → cfg->ops->ltssm_enable → [이 함수]
 */
static void qcom_pcie_2_3_2_ltssm_enable(struct qcom_pcie *pcie)
{
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	/* enable link training */
	val = readl(pcie->parf + PARF_LTSSM); /* [한국어] 신세대의 링크 학습 레지스터를 읽는다 */
	val |= LTSSM_EN; /* [한국어] 활성 비트를 세운다 */
	writel(val, pcie->parf + PARF_LTSSM); /* [한국어] 되쓴다. 구세대가 ELBI 에서 하던 일이 여기로 옮겨 온 것이 이 함수의 존재 이유다 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_3_2 - 2.3.2 세대(msm8996)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 레귤레이터 둘(vdda / vddpe-3v3)과 클록 전부만 잡는다. **리셋을 하나도
 * 잡지 않는 유일한 세대**다.
 *
 * vddpe-3v3 이 새로 등장하는 것이 눈에 띈다. 슬롯 쪽 3.3V 전원으로 보이며,
 * 2.7.0 세대도 같은 짝을 쓴다.
 *
 * 리셋이 없으므로 이 세대의 init 도 전원과 클록만 다루고, deinit 도 그
 * 둘만 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */
	int ret; /* [한국어] 조회 결과 */

	res->supplies[0].supply = "vdda"; /* [한국어] PHY 코어 전원 */
	res->supplies[1].supply = "vddpe-3v3"; /* [한국어] 슬롯 쪽 3.3V 전원. 2.7.0 계열도 같은 짝을 쓴다 */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* [한국어] 둘을 한꺼번에 조회한다 */
	if (ret) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	return 0; /* [한국어] 리셋을 하나도 잡지 않는 유일한 세대의 자원 확보가 끝났다 */
}

/* [한국어]
 * qcom_pcie_deinit_2_3_2 - 2.3.2 세대의 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 이 세대는 리셋을 잡지 않으므로 클록과 레귤레이터만 되돌린다. 이 파일에서
 * 가장 짧은 deinit 이며, 그 짧음 자체가 세대별 자원 구성의 차이를 보여 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* [한국어] 이 세대의 자원 묶음 */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다 */
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 끈다. 리셋이 없어 두 단계뿐이다 */
}

/* [한국어]
 * qcom_pcie_init_2_3_2 - 2.3.2 세대의 전원과 클록을 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 리셋이 없어 전원 → 클록 두 단계뿐이다. 클록이 실패하면 켠 전원을 되돌린다.
 *
 * goto 라벨 없이 그 자리에서 되돌리는데, 되돌릴 것이 하나뿐이라 그 편이
 * 읽기 쉽기 때문이다. 되돌릴 것이 둘 이상인 세대(1.0.0, 2.7.0)는 라벨을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 먼저 켠다 */
	if (ret < 0) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot enable regulators\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록을 켠다 */
	if (ret) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot prepare/enable clocks\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 방금 켠 전원을 되돌린다. 되돌릴 것이 하나뿐이라 라벨 없이 그 자리에서 처리한다 */
		return ret; /* [한국어] 오류를 올린다 */
	}

	return 0; /* [한국어] 이 세대의 기동 준비 완료 */
}

/* [한국어]
 * qcom_pcie_post_init_2_3_2 - 2.3.2/2.4.0 세대의 PARF 를 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * ops_2_3_2 와 ops_2_4_0 이 함께 쓰는 post_init 이다. 두 세대가 자원 구성은
 * 전혀 다른데(2.4.0 은 리셋을 최대 열둘 다룬다) PARF 프로그래밍은 같다는
 * 뜻이며, 이 파일의 콜백 분리가 자원과 레지스터를 별개 축으로 나눈 이유를
 * 보여 준다.
 *
 * 다섯 단계다.
 *   1) PHY 파워다운 해제.
 *   2) 구세대 판으로 DBI 물리 주소를 알려 준다.
 *   3) 상류 주석대로 MAC 의 PHY 파워다운 MUX 를 끈다. P2 상태에서 PHY 가
 *      꺼지지 않게 하는 설정으로 보이며, 구체적 동작은 이 트리에서 확인 못 함.
 *   4) MHI 클록/리셋 제어를 BYPASS 로 둔다.
 *   5) AXI 마스터 쓰기 주소 정지를 켠다. 1.0.0 판이 MSI 여부로 갈랐던 것과
 *      달리 여기서는 무조건 켠다.
 *   6) 핫플러그 완료 통지 없음을 표시한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_2_3_2(struct qcom_pcie *pcie)
{
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값. 이 함수는 res 를 쓰지 않아 자원 묶음을 꺼내지 않는다 */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 읽는다 */
	val &= ~PHY_TEST_PWR_DOWN; /* [한국어] 파워다운 비트를 지운다 */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 */

	qcom_pcie_configure_dbi_base(pcie); /* [한국어] 구세대 판으로 DBI 물리 주소를 알려 준다 */

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL); /* [한국어] 시스템 제어 레지스터를 읽는다 */
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN; /* [한국어] 상류 주석대로 MAC 이 P2 에서 PHY 를 파워다운시키는 MUX 를 끈다 */
	writel(val, pcie->parf + PARF_SYS_CTRL); /* [한국어] 되쓴다 */

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] MHI 클록/리셋 제어를 읽는다 */
	val |= BYPASS; /* [한국어] 우회 비트를 세운다 */
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] 되쓴다 */

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2);
	val |= EN; /* [한국어] 신세대 자리의 AXI 마스터 쓰기 주소 정지 활성 비트를 세운다 */
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* [한국어] 되쓴다. 1.0.0 판이 MSI 여부로 갈랐던 것과 달리 여기서는 무조건 켠다 */

	qcom_pcie_set_slot_nccs(pcie->pci); /* [한국어] 핫플러그 완료 통지를 만들지 못한다는 사실을 표시한다 */

	return 0; /* [한국어] 2.3.2 와 2.4.0 이 함께 쓰는 PARF 프로그래밍이 끝났다 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_4_0 - 2.4.0 세대(ipq4019/qcs404)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 리셋을 **최대 열둘** 다룬다. 이 파일에서 가장 많으며, 이 세대의 IP 가
 * 리셋 선을 가장 잘게 나눠 노출했음을 보여 준다.
 *
 * 이름 열둘을 배열에 꽂아 두고, 마지막에 개수로 잘라 쓴다 — ipq4019 는
 * 열둘 전부, qcs404 는 앞의 여섯만 쓴다. 배열 순서가 곧 "공통이 앞, 확장이
 * 뒤" 로 정렬되어 있어 개수 하나로 가를 수 있다. 2.1.0 이 apq8064 에서
 * 개수를 6→5 로 줄이던 것과 같은 기법이다.
 *
 * 레귤레이터는 잡지 않는다. 이 세대는 전원 관리를 드라이버가 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */
	bool is_ipq = of_device_is_compatible(dev->of_node, "qcom,pcie-ipq4019"); /* [한국어] 아래 리셋 개수를 가르는 SoC 직접 판별 */
	int ret; /* [한국어] 조회 결과 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	res->resets[0].id = "axi_m"; /* [한국어] AXI 마스터 리셋 */
	res->resets[1].id = "axi_s"; /* [한국어] AXI 슬레이브 리셋 */
	res->resets[2].id = "axi_m_sticky"; /* [한국어] AXI 마스터 sticky 리셋 */
	res->resets[3].id = "pipe_sticky"; /* [한국어] PIPE sticky 리셋 */
	res->resets[4].id = "pwr"; /* [한국어] 전원 리셋 */
	res->resets[5].id = "ahb"; /* [한국어] AHB 리셋. 여기까지 여섯이 두 SoC 공통이다 */
	res->resets[6].id = "pipe"; /* [한국어] PIPE 리셋 */
	res->resets[7].id = "axi_m_vmid"; /* [한국어] AXI 마스터 VMID 리셋 */
	res->resets[8].id = "axi_s_xpu"; /* [한국어] AXI 슬레이브 XPU 리셋 */
	res->resets[9].id = "parf"; /* [한국어] PARF 블록 리셋 */
	res->resets[10].id = "phy"; /* [한국어] PHY 리셋 */
	res->resets[11].id = "phy_ahb"; /* [한국어] PHY AHB 리셋. 여기까지 여섯이 ipq4019 전용이다 */

	res->num_resets = is_ipq ? 12 : 6; /* [한국어] 공통이 앞, 확장이 뒤로 정렬되어 있어 개수 하나로 가를 수 있다 */

	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets); /* [한국어] 그 개수만큼만 조회한다 */
	if (ret < 0) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	return 0; /* [한국어] 이 파일에서 가장 많은 리셋을 다루는 세대의 자원 확보가 끝났다 */
}

/* [한국어]
 * qcom_pcie_deinit_2_4_0 - 2.4.0 세대의 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 리셋을 걸고 클록을 끈다. 레귤레이터가 없으므로 두 단계뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* [한국어] 이 세대의 자원 묶음 */

	reset_control_bulk_assert(res->num_resets, res->resets); /* [한국어] 리셋을 건다 */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다. 레귤레이터가 없어 두 단계뿐이다 */
}

/* [한국어]
 * qcom_pcie_init_2_4_0 - 2.4.0 세대의 리셋을 순환시키고 클록을 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 리셋을 걸고 → 기다리고 → 풀고 → 기다린 뒤 클록을 켠다. 지연이 두 번
 * 들어가는 것이 이 세대의 특징이며, 10~12ms 로 이 파일에서 가장 길다.
 * 리셋 선이 열둘이나 되어 전파에 시간이 더 걸리기 때문으로 보이나, 그
 * 근거는 이 트리에서 확인 못 함.
 *
 * 클록 활성이 실패하면 리셋을 다시 걸어 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = reset_control_bulk_assert(res->num_resets, res->resets); /* [한국어] 리셋을 건다 */
	if (ret < 0) { /* [한국어] 못 걸었으면 */
		dev_err(dev, "cannot assert resets\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	usleep_range(10000, 12000); /* [한국어] 리셋이 전파될 시간을 준다. 이 파일에서 가장 긴 10~12ms 인데, 리셋 선이 열둘이나 되어 그런 것으로 보이나 근거는 이 트리에서 확인 못 함 */

	ret = reset_control_bulk_deassert(res->num_resets, res->resets); /* [한국어] 리셋을 푼다 */
	if (ret < 0) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "cannot deassert resets\n");
		return ret; /* [한국어] 그대로 올린다 */
	}

	usleep_range(10000, 12000); /* [한국어] 해제가 반영될 시간을 다시 준다 */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록을 켠다 */
	if (ret) { /* [한국어] 못 켰으면 */
		reset_control_bulk_assert(res->num_resets, res->resets); /* [한국어] 리셋을 다시 걸어 되돌리고 */
		return ret; /* [한국어] 오류를 올린다 */
	}

	return 0; /* [한국어] 이 세대의 기동 준비 완료 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_3_3 - 2.3.3 세대(ipq8074)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 클록 전부와 리셋 일곱(axi_m/axi_s/pipe/axi_m_sticky/sticky/ahb/sleep)을 잡는다.
 *
 * 2.4.0 과 달리 개수를 가르는 분기가 없다 — 이 세대를 쓰는 SoC 가 ipq8074
 * 하나뿐이라 조건이 필요 없었다. 그래서 이후 코드가 ARRAY_SIZE 를 직접 쓴다.
 *
 * 레귤레이터는 잡지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */
	int ret; /* [한국어] 조회 결과 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	res->rst[0].id = "axi_m"; /* [한국어] AXI 마스터 리셋 */
	res->rst[1].id = "axi_s"; /* [한국어] AXI 슬레이브 리셋 */
	res->rst[2].id = "pipe"; /* [한국어] PIPE 리셋 */
	res->rst[3].id = "axi_m_sticky"; /* [한국어] AXI 마스터 sticky 리셋 */
	res->rst[4].id = "sticky"; /* [한국어] sticky 리셋 */
	res->rst[5].id = "ahb"; /* [한국어] AHB 리셋 */
	res->rst[6].id = "sleep"; /* [한국어] sleep 리셋. 이 세대를 쓰는 SoC 가 하나뿐이라 개수를 가르는 분기가 없다 */

	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(res->rst), res->rst); /* [한국어] 그래서 조회도 ARRAY_SIZE 를 직접 쓴다 */
	if (ret < 0) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	return 0; /* [한국어] 이 세대의 자원 확보 완료 */
}

/* [한국어]
 * qcom_pcie_deinit_2_3_3 - 2.3.3 세대의 클록을 끈다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 클록만 끈다. **잡아 둔 리셋을 다시 걸지 않는 것**이 다른 세대와 다르며,
 * init 이 리셋을 풀어 놓은 상태 그대로 남는다. 상류 코드 그대로이며,
 * 그 의도는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* [한국어] 이 세대의 자원 묶음 */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록만 끈다. init 이 풀어 둔 리셋을 다시 걸지 않는 것이 상류 코드 그대로다 */
}

/* [한국어]
 * qcom_pcie_init_2_3_3 - 2.3.3 세대의 리셋을 순환시키고 클록을 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 2.4.0 판과 같은 모양이되 지연이 2~2.5ms 로 더 짧다.
 *
 * 두 번째 지연 위의 상류 주석이 솔직하다 — 리셋이 끝났는지 볼 방법이 없어
 * 그냥 일정 시간을 기다린다. 폴링할 상태 비트가 없는 하드웨어에서 흔한
 * 처리다.
 *
 * 되돌리기 라벨 위의 상류 주석도 짚어 둘 만하다 — 되돌리기 결과를 확인하지
 * 않는데, 어차피 원래 실패 원인을 반환할 것이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst); /* [한국어] 리셋을 건다 */
	if (ret < 0) { /* [한국어] 못 걸었으면 */
		dev_err(dev, "cannot assert resets\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	usleep_range(2000, 2500); /* [한국어] 리셋이 전파될 시간을 준다 */

	ret = reset_control_bulk_deassert(ARRAY_SIZE(res->rst), res->rst); /* [한국어] 리셋을 푼다 */
	if (ret < 0) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "cannot deassert resets\n");
		return ret; /* [한국어] 그대로 올린다 */
	}

	/*
	 * Don't have a way to see if the reset has completed.
	 * Wait for some time.
	 */
	usleep_range(2000, 2500); /* [한국어] 상류 주석대로 리셋 완료를 볼 방법이 없어 일정 시간을 기다린다 */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록을 켠다 */
	if (ret) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot prepare/enable clocks\n"); /* [한국어] 알리고 */
		goto err_assert_resets; /* [한국어] 리셋을 되돌린다 */
	}

	return 0; /* [한국어] 이 세대의 기동 준비 완료 */

err_assert_resets: /* [한국어] 클록 실패가 여기로 온다 */
	/*
	 * Not checking for failure, will anyway return
	 * the original failure in 'ret'.
	 */
	reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst); /* [한국어] 상류 주석대로 결과를 확인하지 않는다 — 어차피 원래 실패 원인을 반환한다 */

	return ret; /* [한국어] 그 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_post_init_2_3_3 - 2.3.3 세대의 PARF·DBI 를 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * 앞선 세대들과 갈라지는 지점이 여럿이라, 이 파일의 "신세대 post_init" 이
 * 어떤 모습인지 보여 주는 첫 사례다.
 *
 *   1) PHY 파워다운 해제 — 여기까지는 앞 세대와 같다.
 *   2) **신세대 판**으로 DBI/iATU 물리 주소를 알려 준다.
 *   3) PARF_SYS_CTRL 에 여덟 비트를 한 번에 써 넣는다. 읽고-고쳐-쓰기가
 *      아니라 통째로 덮어쓰는 것에 주의 — 그 레지스터의 나머지 비트를
 *      0 으로 만든다는 뜻이다. 켜는 것은 마스터/슬레이브 웨이크업,
 *      클록 게이팅 금지 셋, AUX 전원 감지, 그리고 L2/L1 상태에서의 클록
 *      제거 금지다.
 *   4) Q2A 플러시를 0 으로 둔다.
 *   5) config 공간에 PCI_COMMAND_MASTER 를 써 RC 를 버스 마스터로 만든다.
 *   6) 쓰기 창을 열고 세 가지를 고친다 — 슬롯 능력 전체(PCIE_CAP_SLOT_VAL),
 *      ASPM 지원 필드 제거, 완료 타임아웃 비활성.
 *      ASPM 을 지우는 것은 이 세대에서 ASPM 을 쓰지 않겠다는 뜻이고,
 *      완료 타임아웃 비활성은 응답이 늦는 장치에서 오류가 나지 않게 한다.
 *
 * 여기서 슬롯 능력을 통째로 쓰기 때문에, 다른 세대가 부르는
 * qcom_pcie_set_slot_nccs() 를 부르지 않는다. 다만 PCIE_CAP_SLOT_VAL 에는
 * NCCS 가 들어 있지 않아 결과가 같지 않다 — 그 차이가 의도된 것인지는
 * 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_2_3_3(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 창을 얻기 위한 DWC 구조체 */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 config 공간 오프셋 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 읽는다 */
	val &= ~PHY_TEST_PWR_DOWN; /* [한국어] 파워다운 비트를 지운다 */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 */

	qcom_pcie_configure_dbi_atu_base(pcie); /* [한국어] **신세대 판**으로 DBI 와 iATU 물리 주소를 알려 준다 */

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS
		| SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL); /* [한국어] 여덟 비트를 통째로 덮어쓴다 — 읽고-고쳐-쓰기가 아니라 나머지 비트를 0 으로 만든다 */
	writel(0, pcie->parf + PARF_Q2A_FLUSH); /* [한국어] Q2A 플러시를 끈다 */

	writel(PCI_COMMAND_MASTER, pci->dbi_base + PCI_COMMAND); /* [한국어] RC 를 버스 마스터로 만든다 */

	dw_pcie_dbi_ro_wr_en(pci); /* [한국어] 아래 세 가지가 능력 필드라 쓰기 창을 연다 */

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* [한국어] 슬롯 능력을 통째로 써 넣는다. 그래서 이 세대는 qcom_pcie_set_slot_nccs() 를 부르지 않는다 */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 링크 능력 레지스터를 읽는다 */
	val &= ~PCI_EXP_LNKCAP_ASPMS; /* [한국어] ASPM 지원 필드를 지워 이 세대에서 ASPM 을 쓰지 않겠다고 알린다 */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 되쓴다 */

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
		PCI_EXP_DEVCTL2); /* [한국어] 완료 타임아웃을 비활성한다 — 응답이 늦는 장치에서 오류가 나지 않게 한다 */

	dw_pcie_dbi_ro_wr_dis(pci); /* [한국어] 쓰기 창을 닫는다 */

	return 0; /* [한국어] 이 세대의 PARF·DBI 프로그래밍이 끝났다 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_7_0 - 2.7.0 계열의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 이 파일에서 가장 널리 쓰이는 get_resources 다. ops_2_7_0 뿐 아니라
 * ops_1_9_0 과 ops_1_21_0 도 같은 포인터를 넣는다 — 즉 sdm845 부터
 * sc8280xp·x1e80100 까지 최신 계열 전부가 이 함수를 쓴다.
 *
 * 셋을 잡는다.
 *   - 리셋: devm_reset_control_array_get_exclusive 로 **이름을 따지지 않고
 *     DT 가 준 것을 배열째** 가져와 핸들 하나로 묶는다. 구세대가 이름을
 *     하나씩 배열에 꽂던 것(2.1.0 은 여섯, 2.4.0 은 열둘)과 대비되는데,
 *     리셋 구성이 SoC 마다 달라도 드라이버가 알 필요가 없어졌다는 뜻이다.
 *     이 파일에서 세대가 올라가며 코드가 단순해지는 대표적 자리다.
 *   - 레귤레이터 둘: vdda / vddpe-3v3. 2.3.2 와 같은 짝이다.
 *   - 클록 전부.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */
	int ret; /* [한국어] 조회 결과 */

	res->rst = devm_reset_control_array_get_exclusive(dev); /* [한국어] 이름을 따지지 않고 DT 가 준 리셋 전부를 핸들 하나로 묶는다 — 구세대가 이름 배열을 쓰던 것과 갈리는 자리다 */
	if (IS_ERR(res->rst)) /* [한국어] 조회에 실패했으면 */
		return PTR_ERR(res->rst); /* [한국어] 그 오류를 올린다 */

	res->supplies[0].supply = "vdda"; /* [한국어] PHY 코어 전원 */
	res->supplies[1].supply = "vddpe-3v3"; /* [한국어] 슬롯 쪽 3.3V 전원 */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* [한국어] 둘을 한꺼번에 조회한다 */
	if (ret) /* [한국어] 하나라도 없으면 */
		return ret; /* [한국어] 그대로 올린다 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	return 0; /* [한국어] 최신 계열 대부분이 공유하는 자원 확보가 끝났다 */
}

/* [한국어]
 * qcom_pcie_init_2_7_0 - 2.7.0 계열의 자원을 켜고 PARF 까지 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일에서 가장 긴 init 이며, **구세대와의 구조적 차이**가 여기서 드러난다.
 * 구세대는 init 이 전원·클록·리셋만 다루고 PARF 프로그래밍을 post_init 에
 * 미뤘는데, 이 세대는 그 둘을 한 함수에 합쳤다. 그래서 이 세대의 post_init
 * (qcom_pcie_post_init_2_7_0)은 거의 비어 있다.
 *
 * 앞부분(자원):
 *   1) 레귤레이터를 켠다.
 *   2) 클록을 켠다.
 *   3) 리셋을 걸고 → 1~1.5ms 기다리고 → 푼다.
 *   4) 상류 주석대로 SM8450 에서 필요한 리셋 완료 대기를 한 번 더 한다.
 *
 * 뒷부분(PARF):
 *   5) PARF_DEVICE_TYPE 에 RC 값을 써서 컨트롤러를 루트 컴플렉스로 둔다.
 *      같은 IP 를 EP 로도 쓸 수 있기 때문이며, pcie-qcom-ep.c 의
 *      qcom_pcie_perst_deassert() 가 같은 레지스터에 EP 값을 쓴다.
 *   6) PHY 파워다운 해제.
 *   7) 신세대 판으로 DBI/iATU 물리 주소를 알려 준다.
 *   8) MAC 의 PHY 파워다운 MUX 를 끈다.
 *   9) MHI 클록/리셋 제어를 BYPASS 로 둔다.
 *  10) PARF_PM_CTRL 의 REQ_NOT_ENTR_L1 을 지워 L1 진입을 허용하고,
 *      pci->l1ss_support 를 참으로 세워 DWC 코어에 L1 서브스테이트를 쓸 수
 *      있음을 알린다. 구세대에는 없던 절전 기능이다.
 *  11) AXI 마스터 쓰기 주소 정지를 켠다.
 *
 * 되돌리기가 라벨 둘로 대칭을 이룬다. 다만 리셋 조작이 실패했을 때 리셋을
 * 다시 걸지는 않는데, 이미 걸었다 푸는 도중이라 되돌릴 상태가 분명하지
 * 않기 때문으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 *               → qcom_pcie_configure_dbi_atu_base()
 */
static int qcom_pcie_init_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 와 l1ss_support 를 다루기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 켠다 */
	if (ret < 0) { /* [한국어] 못 켰으면 */
		dev_err(dev, "cannot enable regulators\n");
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록을 켠다 */
	if (ret < 0) /* [한국어] 못 켰으면 */
		goto err_disable_regulators; /* [한국어] 전원을 되돌린다 */

	ret = reset_control_assert(res->rst); /* [한국어] 리셋을 건다 */
	if (ret) { /* [한국어] 못 걸었으면 */
		dev_err(dev, "reset assert failed (%d)\n", ret);
		goto err_disable_clocks; /* [한국어] 클록부터 되돌린다 */
	}

	usleep_range(1000, 1500); /* [한국어] 리셋이 전파될 시간을 준다 */

	ret = reset_control_deassert(res->rst); /* [한국어] 리셋을 푼다 */
	if (ret) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		goto err_disable_clocks; /* [한국어] 클록부터 되돌린다 */
	}

	/* Wait for reset to complete, required on SM8450 */
	usleep_range(1000, 1500); /* [한국어] 상류 주석대로 SM8450 에서 필요한 리셋 완료 대기다 */

	/* configure PCIe to RC mode */
	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE); /* [한국어] 같은 IP 가 EP 로도 쓰이므로 RC 로 동작하라고 못박는다. EP 판은 같은 레지스터에 EP 값을 쓴다 */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 읽는다 */
	val &= ~PHY_TEST_PWR_DOWN; /* [한국어] 파워다운 비트를 지운다 */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 */

	qcom_pcie_configure_dbi_atu_base(pcie); /* [한국어] 신세대 판으로 DBI 와 iATU 물리 주소를 알려 준다 */

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL); /* [한국어] 시스템 제어 레지스터를 읽는다 */
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN; /* [한국어] 상류 주석대로 MAC 의 PHY 파워다운 MUX 를 끈다 */
	writel(val, pcie->parf + PARF_SYS_CTRL); /* [한국어] 되쓴다 */

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] MHI 클록/리셋 제어를 읽는다 */
	val |= BYPASS; /* [한국어] 우회 비트를 세운다 */
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] 되쓴다 */

	/* Enable L1 and L1SS */
	val = readl(pcie->parf + PARF_PM_CTRL); /* [한국어] 전력 관리 제어 레지스터를 읽는다 */
	val &= ~REQ_NOT_ENTR_L1; /* [한국어] L1 진입 금지 비트를 지운다 — 구세대에는 없던 절전 기능이다 */
	writel(val, pcie->parf + PARF_PM_CTRL); /* [한국어] 되쓴다 */

	pci->l1ss_support = true; /* [한국어] DWC 코어에 L1 서브스테이트를 쓸 수 있다고 알린다 */

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* [한국어] AXI 마스터 쓰기 주소 정지 레지스터를 읽는다 */
	val |= EN; /* [한국어] 활성 비트를 세운다 */
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* [한국어] 되쓴다 */

	return 0; /* [한국어] 이 세대는 여기까지가 자원과 PARF 를 겸한다 — 그래서 post_init 이 거의 비어 있다 */
err_disable_clocks: /* [한국어] 리셋 실패가 여기로 온다 */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다 */
err_disable_regulators: /* [한국어] 클록 실패가 여기로 온다 */
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 끈다 — 켠 역순이다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_post_init_2_7_0 - 2.7.0 계열의 남은 두 가지를 처리한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * init 이 PARF 프로그래밍을 대부분 끝내 두었으므로 여기 남은 것은 둘뿐이다.
 *
 *   1) cfg->override_no_snoop 인 SoC 에서만 TLP 의 NO_SNOOP 속성을 무시하게
 *      한다. 구조체 주석이 밝히듯 캐시 스누핑을 켜기 위한 것이다. 장치가
 *      "스누핑하지 말라" 고 표시해도 컨트롤러가 무시하고 스누핑하므로,
 *      캐시 일관성이 필요한 플랫폼(sa8775p, sa8540p 계열)에서 켠다.
 *   2) 핫플러그 완료 통지 없음을 표시한다.
 *
 * 이 함수가 짧다는 사실 자체가 이 세대의 배치를 말해 준다 — 구세대에서
 * post_init 이 하던 일이 init 으로 옮겨 갔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_2_7_0(struct qcom_pcie *pcie)
{
	const struct qcom_pcie_cfg *pcie_cfg = pcie->cfg; /* [한국어] 플래그를 보기 위한 SoC 설정 표 */

	if (pcie_cfg->override_no_snoop) /* [한국어] 캐시 스누핑을 강제해야 하는 SoC 에서만 */
		writel(WR_NO_SNOOP_OVERRIDE_EN | RD_NO_SNOOP_OVERRIDE_EN,
				pcie->parf + PARF_NO_SNOOP_OVERRIDE); /* [한국어] 읽기와 쓰기 TLP 의 NO_SNOOP 을 함께 무시하게 한다 */

	qcom_pcie_set_slot_nccs(pcie->pci); /* [한국어] 핫플러그 완료 통지를 만들지 못한다는 사실을 표시한다 */

	return 0; /* [한국어] init 이 대부분을 끝내 두어 이 세대의 post_init 은 여기서 끝난다 */
}

/* [한국어]
 * qcom_pcie_enable_aspm - 하류 장치 하나의 ASPM 을 켜는 순회 콜백
 *
 * @pdev:     순회 중 만난 PCI 장치.
 * @userdata: 쓰지 않는다.
 * @return: 늘 0 — pci_walk_bus 가 순회를 계속한다.
 *
 * pci_walk_bus() 에 넘기는 콜백이다.
 *
 * 상류 주석이 순서의 이유를 밝힌다 — PCI PM 서브스테이트를 켜기 전에 장치가
 * D0 에 있어야 한다. 그래서 전원 상태를 먼저 D0 로 옮기고 링크 상태를 켠다.
 *
 * 두 함수 모두 _locked 판인 것이 중요하다. pci_walk_bus 가 버스 세마포어를
 * 잡은 채로 콜백을 부르므로, 안에서 다시 잡으려 하는 일반 판을 쓰면
 * 교착에 빠진다.
 *
 * PCIE_LINK_STATE_ALL 로 L0s·L1·L1SS 를 한꺼번에 켠다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(열거 직후). 버스 세마포어를 잡은 상태다.
 *
 * 호출 체인:  qcom_pcie_host_post_init_2_7_0() → pci_walk_bus() → [이 함수]
 */
static int qcom_pcie_enable_aspm(struct pci_dev *pdev, void *userdata)
{
	/*
	 * Downstream devices need to be in D0 state before enabling PCI PM
	 * substates.
	 */
	pci_set_power_state_locked(pdev, PCI_D0); /* [한국어] 상류 주석대로 PM 서브스테이트를 켜기 전에 장치가 D0 에 있어야 한다 */
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL); /* [한국어] L0s·L1·L1SS 를 한꺼번에 켠다. 두 함수 모두 _locked 판인 것은 pci_walk_bus 가 이미 버스 세마포어를 잡고 있어 일반 판을 쓰면 교착에 빠지기 때문이다 */

	return 0; /* [한국어] 0 을 돌려주어 순회를 끝까지 계속한다 */
}

/* [한국어]
 * qcom_pcie_host_post_init_2_7_0 - 열거가 끝난 뒤 하류 장치의 ASPM 을 켠다
 *
 * @pcie: 컨트롤러 상태.
 *
 * host_post_init 콜백을 채우는 유일한 구현이며, ops_1_9_0 과 ops_1_21_0 만
 * 이것을 넣는다. ops_2_7_0 자신은 넣지 않는다 — 이름이 2_7_0 이라 헷갈리기
 * 쉬운 자리다.
 *
 * 이 콜백이 다른 콜백들과 다른 점은 **불리는 시점**이다. init/post_init 은
 * 링크를 세우기 전에 불리지만, 이것은 DWC 코어가 버스 열거를 마친 뒤에
 * 부른다(pcie-designware-host.c:1630). 그래야 켤 대상인 하류 장치들이
 * 이미 존재한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(열거 직후).
 *
 * 호출 체인:  DWC 코어 → qcom_pcie_host_post_init() → cfg->ops->host_post_init
 *               → [이 함수] → pci_walk_bus()
 */
static void qcom_pcie_host_post_init_2_7_0(struct qcom_pcie *pcie)
{
	struct dw_pcie_rp *pp = &pcie->pci->pp; /* [한국어] 순회할 버스를 얻기 위해 DWC 의 루트 포트 구조체를 꺼낸다 */

	pci_walk_bus(pp->bridge->bus, qcom_pcie_enable_aspm, NULL); /* [한국어] RC 아래 모든 장치를 훑으며 ASPM 을 켠다. 열거가 끝난 뒤라 대상이 이미 존재한다 */
}

/* [한국어]
 * qcom_pcie_deinit_2_7_0 - 2.7.0 계열의 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 클록과 레귤레이터만 되돌린다. **잡아 둔 리셋을 다시 걸지 않는다** —
 * 2.3.3 판과 같은 비대칭이며, 상류 코드 그대로다. 그 의도는 이 트리에서
 * 확인 못 함.
 *
 * init 이 PARF 를 잔뜩 프로그래밍했지만 여기서 되돌리지 않는 것도 같은
 * 맥락이다. 다시 켤 때 init 이 처음부터 다시 쓰기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_deinit() / qcom_pcie_host_init()(실패 경로)
 *               → cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* [한국어] 이 세대의 자원 묶음 */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록을 끈다 */

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* [한국어] 전원을 끈다. 잡아 둔 리셋은 다시 걸지 않으며, 상류 코드 그대로다 */
}

/* [한국어]
 * qcom_pcie_config_sid_1_9_0 - DT 의 iommu-map 을 하드웨어 해시표로 옮겨 적는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공(매핑이 없으면 아무 일도 하지 않고 0), -ENOMEM 은 할당 실패.
 *
 * 이 파일에서 유일한 config_sid 구현이며 ops_1_9_0 만 쓴다. 하는 일이 다른
 * 콜백들과 결이 달라, 이 함수만 따로 읽어도 될 만하다.
 *
 * **문제**: SMMU(IOMMU)는 요청자를 SID(Stream ID)로 구분한다. PCIe 쪽 요청자
 * 식별자는 BDF(버스/장치/함수)다. 그래서 컨트롤러가 BDF 를 SID 로 바꿔 줘야
 * 하고, 그 대응표를 소프트웨어가 채워야 한다.
 *
 * **하드웨어의 방식**: 대응표가 256칸짜리 해시표다. BDF 를 CRC8 로 해싱해
 * 칸을 정하고, 그 칸에 [BDF | SID오프셋 | NEXT] 를 한 워드로 적는다.
 * 충돌이 나면 NEXT 필드로 다음 칸을 가리키는 연쇄(chaining)를 만든다.
 *
 * 절차:
 *   1) DT 에 iommu-map 이 없으면 할 일이 없다.
 *   2) BDF→SID 변환의 우회(bypass) 모드를 꺼서 변환을 켠다. 기본값이
 *      우회라는 점이 상류 주석에 적혀 있다.
 *   3) iommu-map 을 통째로 읽어 온다. 네 워드가 한 항목이라 구조체 배열로
 *      캐스팅해 다룬다.
 *   4) CRC8 표를 만들고, 하드웨어 표 전체를 0 으로 지운다.
 *   5) 첫 항목의 SID 를 기준으로 삼는다. 하드웨어에는 그 기준으로부터의
 *      오프셋만 8비트로 넣기 때문이다.
 *   6) 항목마다 해시를 구해 빈 칸을 찾는다. 찬 칸이면 그 칸의 NEXT 에
 *      다음 해시를 적어 연쇄를 잇고 다음 칸으로 간다.
 *
 * BDF 를 빅엔디언으로 바꿔 해싱하는 것에 주의. 하드웨어가 그 바이트 순서로
 * 해시를 계산하기 때문으로 보이며, 근거 문서는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc 이 잠들 수 있다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->config_sid → [이 함수]
 */
static int qcom_pcie_config_sid_1_9_0(struct qcom_pcie *pcie)
{
	/* iommu map structure */
	struct {
		u32 bdf; /* [한국어] 요청자 식별자(버스/장치/함수) */
		u32 phandle; /* [한국어] 대응하는 IOMMU 노드의 phandle. 이 함수는 쓰지 않는다 */
		u32 smmu_sid; /* [한국어] 그 BDF 에 대응하는 SMMU Stream ID */
		u32 smmu_sid_len; /* [한국어] 연속으로 매핑할 SID 개수. 이 함수는 쓰지 않는다 */
	} *map; /* [한국어] iommu-map 한 항목의 모양(BDF, IOMMU phandle, SID, SID 길이). DT 배열을 이 구조체로 캐스팅해 다룬다 */
	void __iomem *bdf_to_sid_base = pcie->parf + PARF_BDF_TO_SID_TABLE_N; /* [한국어] BDF→SID 해시표의 시작 주소 */
	struct device *dev = pcie->pci->dev; /* [한국어] DT 조회와 로그의 기준 */
	u8 qcom_pcie_crc8_table[CRC8_TABLE_SIZE]; /* [한국어] CRC8 계산에 쓸 표. 스택에 잡아 이 함수 안에서만 쓴다 */
	int i, nr_map, size = 0; /* [한국어] 루프 인덱스, 항목 수, DT 속성의 바이트 크기 */
	u32 smmu_sid_base; /* [한국어] 첫 항목의 SID. 하드웨어에는 이 기준으로부터의 오프셋만 넣는다 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	of_get_property(dev->of_node, "iommu-map", &size); /* [한국어] 속성의 크기만 먼저 알아본다 */
	if (!size) /* [한국어] iommu-map 이 없으면 */
		return 0; /* [한국어] 채울 표가 없으므로 성공으로 돌아간다 */

	/* Enable BDF to SID translation by disabling bypass mode (default) */
	val = readl(pcie->parf + PARF_BDF_TO_SID_CFG); /* [한국어] 우회 설정 레지스터를 읽는다 */
	val &= ~BDF_TO_SID_BYPASS; /* [한국어] 상류 주석대로 기본값인 우회를 꺼서 변환을 켠다. EP 판은 반대로 우회를 켠다 */
	writel(val, pcie->parf + PARF_BDF_TO_SID_CFG); /* [한국어] 되쓴다 */

	map = kzalloc(size, GFP_KERNEL); /* [한국어] 속성 전체를 담을 버퍼를 잡는다 */
	if (!map) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 변환은 이미 켰지만 되돌리지 않는다 — 상류 코드 그대로다 */

	of_property_read_u32_array(dev->of_node, "iommu-map", (u32 *)map,
				   size / sizeof(u32)); /* [한국어] 속성을 워드 배열로 통째로 읽어 온다 */

	nr_map = size / (sizeof(*map)); /* [한국어] 네 워드가 한 항목이므로 항목 수를 구한다 */

	crc8_populate_msb(qcom_pcie_crc8_table, QCOM_PCIE_CRC8_POLYNOMIAL); /* [한국어] 다항식으로 CRC8 조회표를 만든다. MSB 판인 것은 하드웨어가 그 방식으로 해시를 계산하기 때문으로 보이나 근거는 이 트리에서 확인 못 함 */

	/* Registers need to be zero out first */
	memset_io(bdf_to_sid_base, 0, CRC8_TABLE_SIZE * sizeof(u32)); /* [한국어] 상류 주석대로 하드웨어 표 256칸을 먼저 0 으로 지운다 */

	/* Extract the SMMU SID base from the first entry of iommu-map */
	smmu_sid_base = map[0].smmu_sid; /* [한국어] 상류 주석대로 첫 항목의 SID 를 기준으로 삼는다 */

	/* Look for an available entry to hold the mapping */
	for (i = 0; i < nr_map; i++) { /* [한국어] 항목마다 해시 칸을 찾아 적는다 */
		__be16 bdf_be = cpu_to_be16(map[i].bdf); /* [한국어] BDF 를 빅엔디언으로 바꾼다. 하드웨어가 그 바이트 순서로 해시를 계산하기 때문으로 보이며 근거는 이 트리에서 확인 못 함 */
		u32 val; /* [한국어] 해당 칸의 현재 값 */
		u8 hash; /* [한국어] 계산한 해시(칸 번호) */

		hash = crc8(qcom_pcie_crc8_table, (u8 *)&bdf_be, sizeof(bdf_be), 0); /* [한국어] BDF 두 바이트로 CRC8 해시를 구한다 */

		val = readl(bdf_to_sid_base + hash * sizeof(u32)); /* [한국어] 그 칸을 읽어 비었는지 본다 */

		/* If the register is already populated, look for next available entry */
		while (val) { /* [한국어] 상류 주석대로 이미 차 있으면 다음 빈 칸을 찾는다 — 해시 충돌의 연쇄 처리다 */
			u8 current_hash = hash++; /* [한국어] 지금 칸 번호를 기억하고 다음 칸으로 넘어간다 */
			u8 next_mask = 0xff; /* [한국어] NEXT 필드는 하위 8비트다 */

			/* If NEXT field is NULL then update it with next hash */
			if (!(val & next_mask)) { /* [한국어] 상류 주석대로 그 칸의 NEXT 가 비어 있으면 */
				val |= (u32)hash; /* [한국어] 다음 칸 번호를 NEXT 에 넣어 연쇄를 잇고 */
				writel(val, bdf_to_sid_base + current_hash * sizeof(u32)); /* [한국어] 그 칸에 되쓴다 */
			}

			val = readl(bdf_to_sid_base + hash * sizeof(u32)); /* [한국어] 다음 칸을 읽어 비었는지 다시 본다 */
		}

		/* BDF [31:16] | SID [15:8] | NEXT [7:0] */
		val = map[i].bdf << 16 | (map[i].smmu_sid - smmu_sid_base) << 8 | 0; /* [한국어] 상류 주석의 배치대로 BDF 를 상위 16비트, SID 오프셋을 [15:8], NEXT 를 0 으로 조립한다 */
		writel(val, bdf_to_sid_base + hash * sizeof(u32)); /* [한국어] 찾은 빈 칸에 적는다 */
	}

	kfree(map); /* [한국어] 임시 버퍼를 놓는다 */

	return 0; /* [한국어] 변환표 채우기 완료 */
}

/* [한국어]
 * qcom_pcie_get_resources_2_9_0 - 2.9.0 세대(ipq5018/ipq6018/ipq9574)의 자원을 잡는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 클록 전부와, 이름을 따지지 않는 리셋 배열 하나만 잡는다. 2.7.0 과 같은
 * 방식이되 레귤레이터가 없다 — IPQ 계열은 네트워킹 SoC 라 슬롯 전원을
 * 드라이버가 다루지 않는다.
 *
 * 이 파일에서 가장 단순한 get_resources 이며, 그 단순함이 세대가 올라가며
 * 리셋 관리가 이름 배열에서 배열째 조회로 옮겨 간 결과를 보여 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → cfg->ops->get_resources → [이 함수]
 */
static int qcom_pcie_get_resources_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* [한국어] 이 세대의 자원 묶음 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 자원 조회의 기준 */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* [한국어] 클록을 통째로 받는다 */
	if (res->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return res->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	res->rst = devm_reset_control_array_get_exclusive(dev); /* [한국어] 리셋도 이름을 따지지 않고 배열째 묶는다 — 2.7.0 과 같은 방식이다 */
	if (IS_ERR(res->rst)) /* [한국어] 조회에 실패했으면 */
		return PTR_ERR(res->rst); /* [한국어] 그 오류를 올린다 */

	return 0; /* [한국어] 이 파일에서 가장 단순한 자원 확보가 끝났다 */
}

/* [한국어]
 * qcom_pcie_deinit_2_9_0 - 2.9.0 세대의 클록을 끈다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 클록만 끈다. 2.3.3, 2.7.0 과 마찬가지로 리셋을 다시 걸지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->ops->deinit → [이 함수]
 */
static void qcom_pcie_deinit_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* [한국어] 이 세대의 자원 묶음 */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* [한국어] 클록만 끈다. 리셋을 다시 걸지 않는 것은 2.3.3, 2.7.0 과 같다 */
}

/* [한국어]
 * qcom_pcie_init_2_9_0 - 2.9.0 세대의 리셋을 순환시키고 클록을 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 리셋을 걸고 → 기다리고 → 풀고 → 기다린 뒤 클록을 켠다. 2.3.3 판과 같은
 * 2~2.5ms 지연이다.
 *
 * 가운데 상류 주석이 그 지연의 출처를 밝힌다 — 다운스트림 Codeaurora 커널에서
 * 쓰던 값을 그대로 가져왔다. 스펙에서 유도한 값이 아니라 벤더 커널의 경험값
 * 이라는 뜻이다.
 *
 * 마지막에 클록 활성 결과를 그대로 반환한다. 되돌리기가 없는데, 클록이
 * 실패해도 리셋은 풀린 채로 남는다 — 이 세대의 deinit 이 리셋을 걸지 않는
 * 것과 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->init → [이 함수]
 */
static int qcom_pcie_init_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* [한국어] 이 세대의 자원 묶음 */
	struct device *dev = pcie->pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = reset_control_assert(res->rst); /* [한국어] 리셋을 건다 */
	if (ret) { /* [한국어] 못 걸었으면 */
		dev_err(dev, "reset assert failed (%d)\n", ret);
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */
	}

	/*
	 * Delay periods before and after reset deassert are working values
	 * from downstream Codeaurora kernel
	 */
	usleep_range(2000, 2500); /* [한국어] 상류 주석대로 이 지연은 다운스트림 Codeaurora 커널에서 쓰던 경험값이다 */

	ret = reset_control_deassert(res->rst); /* [한국어] 리셋을 푼다 */
	if (ret) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		return ret; /* [한국어] 그대로 올린다 */
	}

	usleep_range(2000, 2500); /* [한국어] 해제가 반영될 시간을 다시 준다 */

	return clk_bulk_prepare_enable(res->num_clks, res->clks); /* [한국어] 클록 활성 결과를 그대로 반환한다. 되돌리기가 없어 실패해도 리셋은 풀린 채로 남으며, 이 세대의 deinit 이 리셋을 걸지 않는 것과 짝이 맞는다 */
}

/* [한국어]
 * qcom_pcie_post_init_2_9_0 - 2.9.0 세대의 PARF·DBI 를 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * 2.3.3 판과 뼈대가 같으면서 셋이 더 붙는다. 이 세대는 init 이 자원만 다루고
 * PARF 를 전부 여기서 처리하는 구세대식 배치를 유지한다.
 *
 *   1) PHY 파워다운 해제.
 *   2) 신세대 판으로 DBI/iATU 물리 주소를 알려 준다.
 *   3) PARF_DEVICE_TYPE 에 RC 값을 쓴다 — 2.7.0 이 init 에서 하던 일이다.
 *   4) MHI 클록/리셋 제어에 BYPASS 와 클록 활성 둘을 함께 쓴다.
 *   5) **DBI 의 GEN3_RELATED_OFF 에 두 비트를 쓴다.** 이 파일에서 유일하게
 *      DWC 코어가 정의한 Gen3 전용 레지스터를 직접 건드리는 자리다.
 *      RXEQ_RGRDLESS_RXTS 는 RX 트레이닝 시퀀스와 무관하게 이퀄라이제이션을
 *      수행하게 하고, GEN3_ZRXDC_NONCOMPL 은 Gen3 의 ZRX-DC 규격 비준수를
 *      허용하는 것으로 보인다. 두 비트의 정확한 의미는 Synopsys 문서에 있을
 *      것으로 보이나 이 트리에서 확인 못 함.
 *   6) PARF_SYS_CTRL 에 여덟 비트를 통째로 쓴다(2.3.3 과 같은 조합).
 *   7) Q2A 플러시를 0 으로.
 *   8) 쓰기 창을 열고 슬롯 능력·ASPM 제거·완료 타임아웃 비활성을 적는다
 *      (2.3.3 과 같다).
 *   9) **BDF→SID 표 256칸을 0 으로 지운다.** 이 세대는 config_sid 콜백을
 *      쓰지 않으므로 표를 채우지 않는데, 대신 여기서 비워 두어 부트로더가
 *      남긴 값이 남지 않게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init() → cfg->ops->post_init → [이 함수]
 */
static int qcom_pcie_post_init_2_9_0(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 창을 얻기 위한 DWC 구조체 */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 config 공간 오프셋 */
	u32 val; /* [한국어] 읽고-고쳐-쓸 임시 값 */
	int i; /* [한국어] BDF→SID 표를 지우는 루프의 인덱스 */

	val = readl(pcie->parf + PARF_PHY_CTRL); /* [한국어] PHY 제어 레지스터를 읽는다 */
	val &= ~PHY_TEST_PWR_DOWN; /* [한국어] 파워다운 비트를 지운다 */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* [한국어] 되쓴다 */

	qcom_pcie_configure_dbi_atu_base(pcie); /* [한국어] 신세대 판으로 DBI 와 iATU 물리 주소를 알려 준다 */

	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE); /* [한국어] 컨트롤러를 RC 로 둔다. 2.7.0 이 init 에서 하던 일이 이 세대에서는 여기 있다 */
	writel(BYPASS | MSTR_AXI_CLK_EN | AHB_CLK_EN,
		pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] MHI 우회와 함께 마스터 AXI·AHB 클록도 켠다 — 이 두 비트를 쓰는 유일한 세대다 */
	writel(GEN3_RELATED_OFF_RXEQ_RGRDLESS_RXTS |
		GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL,
		pci->dbi_base + GEN3_RELATED_OFF); /* [한국어] DWC 의 Gen3 전용 레지스터에 두 비트를 쓴다. RX 트레이닝 시퀀스와 무관한 이퀄라이제이션과 ZRX-DC 비준수 허용으로 보이나, 정확한 의미는 Synopsys 문서에 있을 것이며 이 트리에서 확인 못 함 */

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS |
		SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL); /* [한국어] 시스템 제어의 여덟 비트를 통째로 덮어쓴다(2.3.3 과 같은 조합) */

	writel(0, pcie->parf + PARF_Q2A_FLUSH); /* [한국어] Q2A 플러시를 끈다 */

	dw_pcie_dbi_ro_wr_en(pci); /* [한국어] 아래 세 가지가 능력 필드라 쓰기 창을 연다 */

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* [한국어] 슬롯 능력을 통째로 써 넣는다 */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 링크 능력 레지스터를 읽는다 */
	val &= ~PCI_EXP_LNKCAP_ASPMS; /* [한국어] ASPM 지원 필드를 지운다 */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* [한국어] 되쓴다 */

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
			PCI_EXP_DEVCTL2); /* [한국어] 완료 타임아웃을 비활성한다 */

	dw_pcie_dbi_ro_wr_dis(pci); /* [한국어] 쓰기 창을 닫는다 */

	for (i = 0; i < 256; i++) /* [한국어] BDF→SID 표 256칸을 */
		writel(0, pcie->parf + PARF_BDF_TO_SID_TABLE_N + (4 * i)); /* [한국어] 모두 0 으로 지운다. 이 세대는 config_sid 콜백이 없어 표를 채우지 않으므로, 부트로더가 남긴 값이 남지 않게 비워 두는 것이다 */

	return 0; /* [한국어] 이 세대의 PARF·DBI 프로그래밍이 끝났다 */
}

/* [한국어]
 * qcom_pcie_link_up - 데이터 링크가 활성인지 본다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 * @return: 데이터 링크 계층이 활성이면 참.
 *
 * struct dw_pcie_ops 의 link_up 으로 등록되어 DWC 코어가 링크 대기와 config
 * 접근 판정에 쓴다.
 *
 * 퀄컴 IP 전용 상태 레지스터가 아니라 **표준 config 공간의 링크 상태
 * 레지스터**를 읽는다. capability 오프셋을 매번 찾아 거기에 LNKSTA 를 더하는
 * 방식이라, IP 세대를 가리지 않고 하나의 구현으로 충분하다. 그래서 이
 * 함수에는 세대 분기가 없다.
 *
 * 같은 컨트롤러의 EP 판(pcie-qcom-ep.c 의 qcom_pcie_dw_link_up)은 대조적으로
 * ELBI 의 벤더 전용 상태 비트를 읽는다 — EP 는 표준 config 공간이 호스트
 * 쪽에서 채워지는 것이라, 자기 링크 상태를 그 방식으로 알기 어렵기 때문으로
 * 보인다.
 *
 * 실행 컨텍스트: 제약 없음. 레지스터 읽기 둘이다.
 *
 * 호출 체인:  DWC 코어(dw_pcie_link_up 등) → dw_pcie_ops.link_up → [이 함수]
 */
static bool qcom_pcie_link_up(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 config 공간 오프셋 */
	u16 val = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* [한국어] 표준 링크 상태 레지스터를 읽는다 — IP 세대를 가리지 않아 이 함수에는 분기가 없다 */

	return val & PCI_EXP_LNKSTA_DLLLA; /* [한국어] 데이터 링크 활성 비트를 그대로 돌려준다. EP 판은 대신 ELBI 의 벤더 전용 비트를 읽는다 */
}

/* [한국어]
 * qcom_pcie_phy_power_off - 이 컨트롤러의 모든 포트 PHY 를 끈다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 포트가 여럿일 수 있어 목록을 훑는다. DT 의 자식 포트 노드마다 PHY 가
 * 하나씩 붙고, qcom_pcie_parse_port() 가 그 목록을 만든다.
 *
 * 반환값을 확인하지 않는다. 정리 경로에서만 불리므로 실패해도 할 수 있는
 * 일이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_host_init()(실패 경로) / qcom_pcie_host_deinit() /
 *               qcom_pcie_phy_power_on()(실패 되돌리기) → [이 함수]
 */
static void qcom_pcie_phy_power_off(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port; /* [한국어] 포트 목록 순회 항목 */

	list_for_each_entry(port, &pcie->ports, list) /* [한국어] 컨트롤러의 모든 포트를 훑으며 */
		phy_power_off(port->phy); /* [한국어] PHY 를 끈다. 정리 경로 전용이라 반환값을 확인하지 않는다 */
}

/* [한국어]
 * qcom_pcie_phy_power_on - 모든 포트 PHY 를 RC 모드로 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 실패하면 그 오류.
 *
 * 포트마다 두 단계다 — 먼저 PHY 를 **RC 모드**로 지정하고, 그다음 켠다.
 *
 * phy_set_mode_ext 의 PHY_MODE_PCIE_RC 가 이 파일과 EP 파일을 가르는 한
 * 줄이다. 같은 퀄컴 PCIe PHY 가 RC 로도 EP 로도 쓰이며,
 * pcie-qcom-ep.c 의 qcom_pcie_enable_resources() 는 같은 자리에
 * PHY_MODE_PCIE_EP 를 넘긴다.
 *
 * 중간에 실패하면 이미 켠 것들을 모두 끄고 돌아간다. 다만 mode 설정이
 * 실패한 경우에는 되돌리지 않는데, 그 시점에는 이 포트의 PHY 를 아직 켜지
 * 않았지만 앞선 포트들은 켜져 있다 — 상류 코드 그대로이며, 호출자
 * (qcom_pcie_host_init)가 err_deinit 경로에서 다시 정리하지도 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 조작이 잠들 수 있다.
 *
 * 호출 체인:  qcom_pcie_host_init() → [이 함수] → phy_set_mode_ext() / phy_power_on()
 */
static int qcom_pcie_phy_power_on(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port; /* [한국어] 포트 목록 순회 항목 */
	int ret; /* [한국어] 각 단계의 결과 */

	list_for_each_entry(port, &pcie->ports, list) { /* [한국어] 컨트롤러의 모든 포트를 훑는다 */
		ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_RC); /* [한국어] PHY 를 **RC 모드**로 지정한다. EP 판은 같은 자리에 PHY_MODE_PCIE_EP 를 넘긴다 */
		if (ret) /* [한국어] 모드 지정이 실패했으면 */
			return ret; /* [한국어] 앞서 켠 포트들을 정리하지 않고 돌아간다 — 상류 코드 그대로다 */

		ret = phy_power_on(port->phy); /* [한국어] PHY 를 켠다 */
		if (ret) { /* [한국어] 못 켰으면 */
			qcom_pcie_phy_power_off(pcie); /* [한국어] 이미 켠 것들을 모두 끄고 */
			return ret; /* [한국어] 오류를 올린다 */
		}
	}

	return 0; /* [한국어] 모든 포트의 PHY 가 켜졌다 */
}

/* [한국어]
 * qcom_pcie_host_init - DWC 코어가 되부르는 기동 절차 본체
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * struct dw_pcie_host_ops 의 init 으로 등록되어 dw_pcie_host_init() 안에서
 * 불린다(drivers/pci/controller/dwc/pcie-designware-host.c:1513). 이 파일의
 * 세대별 콜백들이 실제로 엮이는 자리다.
 *
 *   1) PERST# 를 건다. 하류 장치를 리셋에 넣어 둔 상태에서 컨트롤러를 세운다.
 *   2) cfg->ops->init — 세대별 클록·리셋·전원(과 신세대는 PARF 까지).
 *   3) PHY 를 RC 모드로 켠다.
 *   4) pci_pwrctrl 장치를 만들고 켠다. DT 에 슬롯 전원 제어 노드가 있으면
 *      그것을 별도 장치로 세우는 구조로, 이 단계가 -EPROBE_DEFER 를 돌려줄
 *      수 있다는 점이 아래 되돌리기에서 특별 취급되는 이유다.
 *   5) cfg->ops->post_init — 세대별 PARF 프로그래밍(있으면).
 *   6) 능력 손질 셋 — no_l0s 인 SoC 는 L0s 광고를 지우고, MSI-X 능력과
 *      DPC 확장 능력을 config 공간에서 아예 제거한다. 이 컨트롤러가 그
 *      둘을 제대로 지원하지 못하기 때문으로 보이며, 근거 문서는 이 트리에서
 *      확인 못 함.
 *   7) PERST# 를 푼다. 이 안에서 규정 100ms 가 지켜진다. 이제 하류 장치가
 *      깨어난다.
 *   8) cfg->ops->config_sid — BDF→SID 표(있으면).
 *
 * 되돌리기가 라벨 다섯의 폭포 구조다. err_pwrctrl_destroy 의 조건이 눈에
 * 띄는데, -EPROBE_DEFER 이면 pwrctrl 장치를 부수지 않는다 — 다음 재시도에서
 * 다시 쓰기 위해서다.
 *
 * 이 함수가 probe 뿐 아니라 **resume 에서도 불린다**는 점이 중요하다.
 * qcom_pcie_resume_noirq() 가 suspend 때 내려 둔 컨트롤러를 이 함수로 되살린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:  dw_pcie_host_init() → pp->ops->init → [이 함수]
 *               → cfg->ops->init() → qcom_pcie_phy_power_on()
 *               → cfg->ops->post_init() → qcom_pcie_perst_deassert()
 */
static int qcom_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* [한국어] DWC 의 루트 포트 구조체에서 컨트롤러 구조체를 얻는다 */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* [한국어] 거기서 다시 이 파일의 상태를 되찾는다 */
	int ret; /* [한국어] 각 단계의 결과 */

	qcom_pcie_perst_assert(pcie); /* [한국어] 하류 장치를 리셋에 넣어 둔 상태에서 컨트롤러를 세운다 */

	ret = pcie->cfg->ops->init(pcie); /* [한국어] 세대별 클록·리셋·전원(신세대는 PARF 까지) */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */

	ret = qcom_pcie_phy_power_on(pcie); /* [한국어] PHY 를 RC 모드로 켠다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_deinit; /* [한국어] 세대별 자원을 되돌린다 */

	ret = pci_pwrctrl_create_devices(pci->dev); /* [한국어] DT 에 슬롯 전원 제어 노드가 있으면 별도 장치로 만든다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_disable_phy; /* [한국어] PHY 부터 되돌린다 */

	ret = pci_pwrctrl_power_on_devices(pci->dev); /* [한국어] 그 장치들의 전원을 켠다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_pwrctrl_destroy; /* [한국어] 만든 장치들을 되돌린다 */

	if (pcie->cfg->ops->post_init) { /* [한국어] 세대별 PARF 프로그래밍이 있으면 */
		ret = pcie->cfg->ops->post_init(pcie); /* [한국어] 그것을 부른다 */
		if (ret) /* [한국어] 실패했으면 */
			goto err_pwrctrl_power_off; /* [한국어] 켠 슬롯 전원부터 되돌린다 */
	}

	qcom_pcie_clear_aspm_l0s(pcie->pci); /* [한국어] no_l0s 인 SoC 에서 L0s 광고를 지운다 */
	dw_pcie_remove_capability(pcie->pci, PCI_CAP_ID_MSIX); /* [한국어] MSI-X 능력을 config 공간에서 아예 제거한다 */
	dw_pcie_remove_ext_capability(pcie->pci, PCI_EXT_CAP_ID_DPC); /* [한국어] DPC 확장 능력도 제거한다. 이 컨트롤러가 둘을 제대로 지원하지 못하기 때문으로 보이나 근거 문서는 이 트리에서 확인 못 함 */

	qcom_pcie_perst_deassert(pcie); /* [한국어] 규정 100ms 를 지킨 뒤 PERST# 를 푼다 — 이제 하류 장치가 깨어난다 */

	if (pcie->cfg->ops->config_sid) { /* [한국어] BDF→SID 표를 채우는 세대이면 */
		ret = pcie->cfg->ops->config_sid(pcie); /* [한국어] 그것을 부른다. 장치가 깨어난 뒤여야 하는 것으로 보인다 */
		if (ret) /* [한국어] 실패했으면 */
			goto err_assert_reset; /* [한국어] PERST# 부터 다시 건다 */
	}

	return 0; /* [한국어] 컨트롤러가 완전히 준비되었다 */

err_assert_reset: /* [한국어] config_sid 실패가 여기로 온다 */
	qcom_pcie_perst_assert(pcie); /* [한국어] 하류 장치를 다시 리셋에 넣는다 */
err_pwrctrl_power_off: /* [한국어] post_init 실패가 여기로 온다 */
	pci_pwrctrl_power_off_devices(pci->dev); /* [한국어] 슬롯 전원을 끈다 */
err_pwrctrl_destroy: /* [한국어] pwrctrl 전원 켜기 실패가 여기로 온다 */
	if (ret != -EPROBE_DEFER) /* [한국어] -EPROBE_DEFER 이면 장치를 부수지 않는다 — 다음 재시도에서 다시 쓰기 위해서다 */
		pci_pwrctrl_destroy_devices(pci->dev); /* [한국어] 그 밖의 실패면 만든 장치를 없앤다 */
err_disable_phy: /* [한국어] pwrctrl 생성 실패가 여기로 온다 */
	qcom_pcie_phy_power_off(pcie); /* [한국어] PHY 를 끈다 */
err_deinit: /* [한국어] PHY 실패가 여기로 온다 */
	pcie->cfg->ops->deinit(pcie); /* [한국어] 세대별 자원을 되돌린다 — 잡은 역순이다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_host_deinit - 컨트롤러를 끈다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 *
 * host_init 의 역순이다 — PERST# 를 걸고, 슬롯 전원을 끄고, PHY 를 끄고,
 * 세대별 deinit 을 부른다.
 *
 * 상류 주석이 pwrctrl 장치를 부수지 않는 이유를 밝힌다 — 지금은 이 함수가
 * 시스템 서스펜드 때만 불리기 때문이다. 다시 깨어날 때 그 장치가 그대로
 * 있어야 한다.
 *
 * qcom_pcie_suspend_noirq() 가 이 함수를 직접 부르는 자리도 있어, DWC 코어의
 * 콜백이면서 동시에 이 파일 안에서 쓰이는 헬퍼이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  DWC 코어 → pp->ops->deinit → [이 함수],
 *               그리고 qcom_pcie_suspend_noirq() → [이 함수]
 */
static void qcom_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* [한국어] DWC 의 루트 포트 구조체에서 컨트롤러 구조체를 얻는다 */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* [한국어] 거기서 다시 이 파일의 상태를 되찾는다 */

	qcom_pcie_perst_assert(pcie); /* [한국어] 하류 장치를 리셋에 넣는다 — host_init 의 역순 첫 단계다 */

	/*
	 * No need to destroy pwrctrl devices as this function only gets called
	 * during system suspend as of now.
	 */
	pci_pwrctrl_power_off_devices(pci->dev); /* [한국어] 상류 주석대로 지금은 이 함수가 시스템 서스펜드에서만 불려, 장치를 부수지 않고 전원만 끈다 */
	qcom_pcie_phy_power_off(pcie); /* [한국어] PHY 를 끈다 */
	pcie->cfg->ops->deinit(pcie); /* [한국어] 세대별 자원을 되돌린다 */
}

/* [한국어]
 * qcom_pcie_host_post_init - 열거가 끝난 뒤 세대별 후처리를 부른다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 *
 * struct dw_pcie_host_ops 의 post_init 으로 등록되어, DWC 코어가 버스 열거를
 * 마친 뒤 부른다(pcie-designware-host.c:1630).
 *
 * 하는 일은 세대별 콜백으로의 위임뿐이며, 그 콜백을 채운 표는
 * ops_1_9_0 과 ops_1_21_0 둘뿐이다. 나머지 세대에서는 아무 일도 하지 않는다.
 *
 * 이름이 비슷한 셋을 구분해 둘 만하다.
 *   qcom_pcie_host_post_init()       — 이 함수. DWC 콜백.
 *   cfg->ops->host_post_init         — 세대별 콜백 자리.
 *   qcom_pcie_host_post_init_2_7_0() — 그 자리를 채우는 유일한 구현.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(열거 직후).
 *
 * 호출 체인:  DWC 코어 → pp->ops->post_init → [이 함수]
 *               → cfg->ops->host_post_init()
 */
static void qcom_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* [한국어] DWC 의 루트 포트 구조체에서 컨트롤러 구조체를 얻는다 */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* [한국어] 거기서 다시 이 파일의 상태를 되찾는다 */

	if (pcie->cfg->ops->host_post_init) /* [한국어] 세대별 후처리가 있으면(1.9.0 과 1.21.0 뿐) */
		pcie->cfg->ops->host_post_init(pcie); /* [한국어] 그것을 부른다. 나머지 세대에서는 아무 일도 하지 않는다 */
}

/* [한국어] DWC 호스트 코어가 되부르는 세 콜백.
 * probe 가 pp->ops 에 꽂으며, 각각
 * pcie-designware-host.c 의 :1513(init), :1630(post_init), :1649(deinit)
 * 에서 불린다. 이 셋이 이 파일과 DWC 코어의 접점 전부다. */
static const struct dw_pcie_host_ops qcom_pcie_dw_ops = {
	.init		= qcom_pcie_host_init, /* [한국어] 클록·PHY·PARF·PERST# 를 세우는 기동 절차 본체 */
	.deinit		= qcom_pcie_host_deinit, /* [한국어] 그 역 */
	.post_init	= qcom_pcie_host_post_init, /* [한국어] 버스 열거가 끝난 뒤의 후처리 */
};

/* Qcom IP rev.: 2.1.0	Synopsys IP rev.: 4.01a */
/* [한국어] 가장 오래된 세대. 리셋 여섯과 레귤레이터 셋을 이름으로 다루고,
 * PARF 프로그래밍에서 PHY 의 아날로그 값(디엠퍼시스·스윙·종단)까지 직접
 * 손본다. 링크 학습을 ELBI 에서 켜는 두 세대 중 하나다. */
static const struct qcom_pcie_ops ops_2_1_0 = {
	.get_resources = qcom_pcie_get_resources_2_1_0, /* [한국어] 리셋 여섯·레귤레이터 셋·클록 */
	.init = qcom_pcie_init_2_1_0, /* [한국어] 리셋 걸기 → 전원 → 리셋 해제 */
	.post_init = qcom_pcie_post_init_2_1_0, /* [한국어] PHY 아날로그 값과 TLP 크기까지 손보는 긴 PARF 프로그래밍 */
	.deinit = qcom_pcie_deinit_2_1_0, /* [한국어] 클록·리셋·PHY 파워다운·전원을 되돌린다 */
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable, /* [한국어] ELBI 판 */
};

/* Qcom IP rev.: 1.0.0	Synopsys IP rev.: 4.11a */
/* [한국어] 리셋 하나·레귤레이터 하나로 가장 단순한 자원 구성을 갖는 세대.
 * PARF 프로그래밍도 이 파일에서 가장 짧다. 다만 링크 학습은 2.1.0 과
 * 같은 ELBI 판을 쓴다 — 두 세대가 같은 ELBI 배치를 갖기 때문이다. */
static const struct qcom_pcie_ops ops_1_0_0 = {
	.get_resources = qcom_pcie_get_resources_1_0_0, /* [한국어] 리셋 하나·레귤레이터 하나·클록 */
	.init = qcom_pcie_init_1_0_0, /* [한국어] 리셋 해제 → 클록 → 전원(2.1.0 과 순서가 반대다) */
	.post_init = qcom_pcie_post_init_1_0_0, /* [한국어] DBI 주소와 MSI 관련 비트만 다루는 짧은 PARF 프로그래밍 */
	.deinit = qcom_pcie_deinit_1_0_0, /* [한국어] 이 파일에서 유일하게 deinit 이 리셋을 다시 거는 대칭을 지킨다 */
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable, /* [한국어] ELBI 판. 2.1.0 의 구현을 그대로 빌려 쓴다 */
};

/* Qcom IP rev.: 2.3.2	Synopsys IP rev.: 4.21a */
/* [한국어] 리셋을 하나도 다루지 않는 유일한 세대.
 * 링크 학습을 PARF_LTSSM 에서 켜는 첫 세대이며, 이후 모든 세대가 그 구현을
 * 그대로 쓴다. */
static const struct qcom_pcie_ops ops_2_3_2 = {
	.get_resources = qcom_pcie_get_resources_2_3_2, /* [한국어] 레귤레이터 둘·클록. 리셋 없음 */
	.init = qcom_pcie_init_2_3_2, /* [한국어] 전원 → 클록 두 단계뿐 */
	.post_init = qcom_pcie_post_init_2_3_2, /* [한국어] PHY 파워다운 해제와 PARF 네 항목 */
	.deinit = qcom_pcie_deinit_2_3_2, /* [한국어] 클록·전원만 되돌린다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판. 여기서 처음 등장해 이후 일곱 표가 공유한다 */
};

/* Qcom IP rev.: 2.4.0	Synopsys IP rev.: 4.20a */
/* [한국어] 리셋을 최대 열둘까지 다루는 세대.
 * 자원 구성은 2.3.2 와 전혀 다르지만 PARF 프로그래밍은 같아, post_init 만
 * 2.3.2 의 것을 빌려 쓴다 — 이 파일의 콜백 분리가 자원과 레지스터를
 * 별개 축으로 나눈 이유를 보여 주는 자리다. */
static const struct qcom_pcie_ops ops_2_4_0 = {
	.get_resources = qcom_pcie_get_resources_2_4_0, /* [한국어] 리셋 최대 열둘·클록. 레귤레이터 없음 */
	.init = qcom_pcie_init_2_4_0, /* [한국어] 리셋 순환(10~12ms 지연 두 번) → 클록 */
	.post_init = qcom_pcie_post_init_2_3_2, /* [한국어] **2.3.2 의 것을 그대로 빌려 쓴다** */
	.deinit = qcom_pcie_deinit_2_4_0, /* [한국어] 리셋 걸기 → 클록 끄기 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판 */
};

/* [한국어] 신세대 판 DBI/iATU 주소 등록을 처음 쓰는 세대.
 * PARF_SYS_CTRL 여덟 비트를 통째로 덮어쓰고, 슬롯 능력·ASPM·완료 타임아웃까지
 * config 공간을 직접 손보기 시작한다. */
/* Qcom IP rev.: 2.3.3	Synopsys IP rev.: 4.30a */
static const struct qcom_pcie_ops ops_2_3_3 = {
	.get_resources = qcom_pcie_get_resources_2_3_3, /* [한국어] 리셋 일곱·클록. 개수 분기가 없다 */
	.init = qcom_pcie_init_2_3_3, /* [한국어] 리셋 순환(2~2.5ms 지연 두 번) → 클록 */
	.post_init = qcom_pcie_post_init_2_3_3, /* [한국어] 신세대 주소 등록과 config 공간 손질까지 하는 긴 PARF 프로그래밍 */
	.deinit = qcom_pcie_deinit_2_3_3, /* [한국어] 클록만 끈다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판 */
};

/* Qcom IP rev.: 2.7.0	Synopsys IP rev.: 4.30a */
/* [한국어] 자원 확보와 PARF 프로그래밍을 **init 하나에 합친** 첫 세대.
 * 그래서 이 표의 post_init 은 거의 비어 있다. 리셋을 이름 배열이 아니라
 * 핸들 하나로 묶어 다루기 시작한 것도 이 세대부터다. */
static const struct qcom_pcie_ops ops_2_7_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* [한국어] 리셋 핸들 하나·레귤레이터 둘·클록 */
	.init = qcom_pcie_init_2_7_0, /* [한국어] 자원과 PARF 를 함께 처리하는 이 파일에서 가장 긴 init */
	.post_init = qcom_pcie_post_init_2_7_0, /* [한국어] NO_SNOOP 무시와 NCCS 표시만 남은 짧은 post_init */
	.deinit = qcom_pcie_deinit_2_7_0, /* [한국어] 클록·전원만 되돌린다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판 */
};

/* Qcom IP rev.: 1.9.0 */
/* [한국어] 2.7.0 의 함수를 그대로 쓰면서 두 자리를 더 채운 표.
 * 상류 주석이 이 표에만 Synopsys 리비전을 적지 않은 것이 눈에 띈다.
 * 더해진 둘이 이 세대의 특징이다 — 열거 후 하류 ASPM 활성과,
 * 이 파일에서 유일한 BDF→SID 변환표 채우기다. */
static const struct qcom_pcie_ops ops_1_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.init = qcom_pcie_init_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.post_init = qcom_pcie_post_init_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.host_post_init = qcom_pcie_host_post_init_2_7_0, /* [한국어] **이 표부터 추가**. 열거가 끝난 뒤 하류 장치의 ASPM 을 켠다 */
	.deinit = qcom_pcie_deinit_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판 */
	.config_sid = qcom_pcie_config_sid_1_9_0, /* [한국어] **이 표에만 있다**. iommu-map 을 CRC8 해시표로 하드웨어에 적는다 */
};

/* Qcom IP rev.: 1.21.0  Synopsys IP rev.: 5.60a */
/* [한국어] 1.9.0 에서 config_sid 하나만 빠진 표.
 * sc8280xp·sa8540p·x1e80100 이 이것을 쓰며, 그 SoC 들은 IOMMU 변환을
 * 드라이버가 채우지 않는다는 뜻이다. */
static const struct qcom_pcie_ops ops_1_21_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.init = qcom_pcie_init_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.post_init = qcom_pcie_post_init_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.host_post_init = qcom_pcie_host_post_init_2_7_0, /* [한국어] 하류 ASPM 활성. 1.9.0 과 같다 */
	.deinit = qcom_pcie_deinit_2_7_0, /* [한국어] 2.7.0 의 것을 빌려 쓴다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판. config_sid 가 없는 것이 1.9.0 과의 유일한 차이다 */
};

/* Qcom IP rev.: 2.9.0  Synopsys IP rev.: 5.00a */
/* [한국어] IPQ 네트워킹 SoC 계열의 표.
 * 리셋 핸들 방식은 2.7.0 을 따르되 레귤레이터가 없고, PARF 프로그래밍은
 * 2.3.3 처럼 post_init 에 몰려 있다. 즉 신·구세대 방식이 섞여 있다. */
static const struct qcom_pcie_ops ops_2_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_9_0, /* [한국어] 리셋 핸들 하나·클록. 레귤레이터 없음 */
	.init = qcom_pcie_init_2_9_0, /* [한국어] 리셋 순환(2~2.5ms) → 클록 */
	.post_init = qcom_pcie_post_init_2_9_0, /* [한국어] Gen3 전용 레지스터와 BDF→SID 표 지우기까지 하는 긴 PARF 프로그래밍 */
	.deinit = qcom_pcie_deinit_2_9_0, /* [한국어] 클록만 끈다 */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* [한국어] PARF_LTSSM 판 */
};

/* [한국어] 여기부터가 **두 번째 층**이다.
 * 같은 ops 표를 쓰면서 불리언 플래그만 다른 SoC 들을 가르기 위해,
 * struct qcom_pcie_cfg 인스턴스를 따로 둔다. DT 매칭 표가 가리키는 것은
 * ops 가 아니라 이 cfg 들이다. */
static const struct qcom_pcie_cfg cfg_1_0_0 = {
	.ops = &ops_1_0_0, /* [한국어] 플래그 없이 ops_1_0_0 만 쓴다(apq8084) */
};

static const struct qcom_pcie_cfg cfg_1_9_0 = {
	.ops = &ops_1_9_0, /* [한국어] 플래그 없이 ops_1_9_0 을 쓴다(sc7280/sc8180x/sdx55/sm8150~sm8550) */
};

static const struct qcom_pcie_cfg cfg_1_34_0 = {
	.ops = &ops_1_9_0, /* [한국어] 같은 ops_1_9_0 을 쓰되 */
	.override_no_snoop = true, /* [한국어] 캐시 스누핑을 강제한다. sa8775p 전용이며, ops 는 그대로 두고 플래그만 다르다 — cfg 층이 존재하는 이유를 보여 주는 자리다 */
};

static const struct qcom_pcie_cfg cfg_2_1_0 = {
	.ops = &ops_2_1_0, /* [한국어] 플래그 없이 ops_2_1_0 을 쓴다(apq8064/ipq8064/ipq8064-v2) */
};

static const struct qcom_pcie_cfg cfg_2_3_2 = {
	.ops = &ops_2_3_2, /* [한국어] ops_2_3_2 를 쓰되 */
	.no_l0s = true, /* [한국어] L0s 를 광고하지 않는다(msm8996) */
};

static const struct qcom_pcie_cfg cfg_2_3_3 = {
	.ops = &ops_2_3_3, /* [한국어] 플래그 없이 ops_2_3_3 을 쓴다(ipq8074) */
};

static const struct qcom_pcie_cfg cfg_2_4_0 = {
	.ops = &ops_2_4_0, /* [한국어] 플래그 없이 ops_2_4_0 을 쓴다(ipq4019/qcs404) */
};

static const struct qcom_pcie_cfg cfg_2_7_0 = {
	.ops = &ops_2_7_0, /* [한국어] 플래그 없이 ops_2_7_0 을 쓴다(sdm845) */
};

static const struct qcom_pcie_cfg cfg_2_9_0 = {
	.ops = &ops_2_9_0, /* [한국어] 플래그 없이 ops_2_9_0 을 쓴다(ipq5018/ipq6018/ipq8074-gen3/ipq9574) */
};

static const struct qcom_pcie_cfg cfg_sc8280xp = {
	.ops = &ops_1_21_0, /* [한국어] ops_1_21_0 을 쓰되 */
	.no_l0s = true, /* [한국어] L0s 를 광고하지 않는다(sc8280xp/sa8540p/x1e80100) */
};

/* [한국어] **ops 가 없는 유일한 표**.
 * 펌웨어가 컨트롤러를 이미 세워 둔 플랫폼(sa8255p)용이라, probe 가 이
 * 플래그를 보고 ECAM 갈래로 완전히 빠진다. 그래서 세대별 콜백이 필요 없고,
 * probe 의 유효성 검사도 "firmware_managed 가 아닌데 ops 가 없으면 오류" 라는
 * 형태로 되어 있다. */
static const struct qcom_pcie_cfg cfg_fw_managed = {
	.firmware_managed = true, /* [한국어] 이 한 줄이 probe 를 다른 길로 보낸다 */
};

/* [한국어] DWC 코어가 링크를 다룰 때 되부르는 두 콜백.
 * probe 가 pci->ops 에 꽂는다. EP 판(pcie-qcom-ep.c)은 같은 구조체에 넷을
 * 채운다 — stop_link 와 write_dbi2 가 더 있는데, 둘 다 EP 에만 필요한
 * 동작이다. */
static const struct dw_pcie_ops dw_pcie_ops = {
	.link_up = qcom_pcie_link_up, /* [한국어] 표준 링크 상태 레지스터로 링크업을 판정한다 */
	.start_link = qcom_pcie_start_link, /* [한국어] 이퀄라이제이션·마진 설정 뒤 LTSSM 을 켠다 */
};

/* [한국어]
 * qcom_pcie_icc_init - 인터커넥트 두 경로를 잡고 초기 대역폭을 요구한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 인터커넥트 오류.
 *
 * 퀄컴 SoC 는 버스 대역폭을 소프트웨어가 명시적으로 요구해야 한다. 경로가
 * 둘이다.
 *   - pcie-mem : PCIe 장치가 DRAM 에 접근하는 경로. 실제 데이터가 흐른다.
 *   - cpu-pcie : CPU 가 컨트롤러 레지스터와 장치 config/BAR 에 접근하는 경로.
 *
 * 상류 주석이 각각의 초기값 근거를 밝힌다.
 *   - pcie-mem 은 일부 플랫폼이 인터커넥트 클록을 켜기 전에 대역폭 제약을
 *     먼저 설정하도록 요구하므로, 1레인 Gen1 에 해당하는 값을 미리 건다.
 *     실제 값은 링크가 선 뒤 qcom_pcie_icc_opp_update() 가 갱신한다.
 *   - cpu-pcie 는 하드웨어 팀 권고대로 경로를 살려 두기 위한 최소값 1KBps 만
 *     건다. 데이터가 아니라 레지스터 접근용이라 대역폭이 필요 없기 때문이다.
 *
 * 두 번째가 실패하면 첫 번째 요구를 0 으로 되돌린다.
 *
 * 이 함수는 OPP 표가 없는 플랫폼에서만 불린다. OPP 가 있으면 대역폭과 전압을
 * OPP 프레임워크가 함께 다루므로 probe 가 이 함수를 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → [이 함수] → icc_set_bw()
 */
static int qcom_pcie_icc_init(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	int ret; /* [한국어] 각 단계의 결과 */

	pcie->icc_mem = devm_of_icc_get(pci->dev, "pcie-mem"); /* [한국어] PCIe 장치 ↔ DRAM 경로를 잡는다 */
	if (IS_ERR(pcie->icc_mem)) /* [한국어] DT 가 그 경로를 주지 않았으면 */
		return PTR_ERR(pcie->icc_mem); /* [한국어] 그 오류를 올린다 — OPP 를 쓰지 않는 플랫폼에서는 필수다 */

	pcie->icc_cpu = devm_of_icc_get(pci->dev, "cpu-pcie"); /* [한국어] CPU ↔ 컨트롤러 경로를 잡는다 */
	if (IS_ERR(pcie->icc_cpu)) /* [한국어] 없으면 */
		return PTR_ERR(pcie->icc_cpu); /* [한국어] 그 오류를 올린다 */
	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(pcie->icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1)); /* [한국어] 상류 주석대로 1레인 Gen1 에 해당하는 초기 대역폭을 미리 건다 */
	if (ret) { /* [한국어] 요구가 실패했으면 */
		dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
			ret); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 올린다 */
	}

	/*
	 * Since the CPU-PCIe path is only used for activities like register
	 * access of the host controller and endpoint Config/BAR space access,
	 * HW team has recommended to use a minimal bandwidth of 1KBps just to
	 * keep the path active.
	 */
	ret = icc_set_bw(pcie->icc_cpu, 0, kBps_to_icc(1)); /* [한국어] 상류 주석대로 경로를 살려 두기 위한 최소값 1KBps 만 건다 */
	if (ret) { /* [한국어] 요구가 실패했으면 */
		dev_err(pci->dev, "Failed to set bandwidth for CPU-PCIe interconnect path: %d\n",
			ret); /* [한국어] 알리고 */
		icc_set_bw(pcie->icc_mem, 0, 0); /* [한국어] 앞서 건 pcie-mem 요구를 0 으로 되돌린다 */
		return ret;
	}

	return 0;
}

/* [한국어]
 * qcom_pcie_icc_opp_update - 협상된 링크 속도·폭에 맞춰 대역폭과 전압을 다시 요구한다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 링크가 선 뒤(probe 끝과 resume 끝)에 불려, 미리 걸어 둔 보수적인 값을
 * 실제 링크에 맞게 조정한다.
 *
 * 먼저 표준 링크 상태 레지스터에서 속도와 폭을 읽는다. 링크가 서지 않았으면
 * 아무 일도 하지 않는다 — 잘못된 값으로 제약을 걸지 않기 위해서다.
 *
 * 그다음 두 갈래로 갈린다. 이것이 이 파일의 전력 관리가 두 세대로 나뉘어
 * 있음을 보여 준다.
 *
 *   [icc 갈래] pcie-mem 경로에 폭 x 속도당 대역폭을 요구한다. 단순한
 *   대역폭 조절이다.
 *
 *   [OPP 갈래] OPP(Operating Performance Point) 표에서 이 링크에 맞는
 *   항목을 찾아 적용한다. 대역폭뿐 아니라 **전압 코너**까지 함께 정해지는
 *   것이 icc 갈래와의 차이다. 찾는 방법이 두 단계인데 —
 *     먼저 opp-level(속도 자체를 레벨로 쓰는 표)에서 정확히 맞는 것을 찾고,
 *     없으면 주파수만으로 찾는다. 있으면 그 OPP 를 놓아 준 뒤, 주파수·레벨·
 *     대역폭을 키로 다시 정확히 찾는다. 두 번 찾는 이유는 첫 조회가 존재
 *     여부 확인이고 두 번째가 실제 선택이기 때문으로 보인다.
 *   주파수는 속도(Mbps)에 폭을 곱해 구한다.
 *
 * 찾은 OPP 는 쓰고 나서 반드시 놓아 준다(dev_pm_opp_put) — 참조 카운트가
 * 있는 객체이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_probe() / qcom_pcie_resume_noirq() → [이 함수]
 */
static void qcom_pcie_icc_opp_update(struct qcom_pcie *pcie)
{
	u32 offset, status, width, speed; /* [한국어] capability 오프셋, 링크 상태 워드, 협상된 폭과 속도 */
	struct dw_pcie *pci = pcie->pci; /* [한국어] DBI 창과 device 를 얻기 위한 DWC 구조체 */
	struct dev_pm_opp_key key = {}; /* [한국어] OPP 를 주파수·레벨·대역폭 셋으로 찾을 때 쓰는 키. 0 으로 초기화한다 */
	unsigned long freq_kbps; /* [한국어] kHz 단위로 환산한 주파수 */
	struct dev_pm_opp *opp; /* [한국어] 찾은 OPP 핸들 */
	int ret, freq_mbps; /* [한국어] 각 단계의 결과와 Mbps 단위 속도 */

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 config 공간 오프셋 */
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* [한국어] 표준 링크 상태 레지스터를 읽는다 */

	/* Only update constraints if link is up. */
	if (!(status & PCI_EXP_LNKSTA_DLLLA)) /* [한국어] 상류 주석대로 링크가 서 있을 때만 제약을 갱신한다 */
		return; /* [한국어] 잘못된 값으로 대역폭을 걸지 않기 위해서다 */

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status); /* [한국어] 협상된 링크 속도 */
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status); /* [한국어] 협상된 레인 수 */

	if (pcie->icc_mem) { /* [한국어] 인터커넥트를 직접 다루는 플랫폼이면 */
		ret = icc_set_bw(pcie->icc_mem, 0,
				 width * QCOM_PCIE_LINK_SPEED_TO_BW(speed)); /* [한국어] 폭 x 속도당 대역폭을 요구한다 */
		if (ret) { /* [한국어] 요구가 실패했으면 */
			dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret); /* [한국어] 알리기만 한다 — 링크는 이미 서 있어 치명적이지 않다 */
		}
	} else if (pcie->use_pm_opp) { /* [한국어] OPP 를 쓰는 플랫폼이면. 두 갈래는 배타적이다 */
		freq_mbps = pcie_dev_speed_mbps(pcie_get_link_speed(speed)); /* [한국어] 속도 코드를 Mbps 로 바꾼다 */
		if (freq_mbps < 0) /* [한국어] 알 수 없는 속도이면 */
			return; /* [한국어] 맞출 OPP 를 찾을 수 없다 */

		freq_kbps = freq_mbps * KILO; /* [한국어] kHz 단위로 환산한다 */
		opp = dev_pm_opp_find_level_exact(pci->dev, speed); /* [한국어] 먼저 opp-level 로 정확히 맞는 항목이 있는지 본다 */
		if (IS_ERR(opp)) { /* [한국어] 없으면 */
			 /* opp-level is not defined use only frequency */
			opp = dev_pm_opp_find_freq_exact(pci->dev, freq_kbps * width, /* [한국어] 상류 주석대로 주파수만으로 찾는다. 주파수는 레인 수를 곱해 구한다 */
							 true);
		} else { /* [한국어] 있으면 */
			/* put opp-level OPP */
			dev_pm_opp_put(opp); /* [한국어] 상류 주석대로 조회로 든 참조를 먼저 놓는다 — 이 조회는 존재 확인용이었다 */

			key.freq = freq_kbps * width; /* [한국어] 주파수 키 */
			key.level = speed; /* [한국어] 레벨 키(속도 코드) */
			key.bw = 0; /* [한국어] 대역폭 키는 쓰지 않는다 */
			opp = dev_pm_opp_find_key_exact(pci->dev, &key, true); /* [한국어] 셋을 키로 정확히 맞는 OPP 를 다시 찾는다 */
		}
		if (!IS_ERR(opp)) { /* [한국어] 어느 방법으로든 찾았으면 */
			ret = dev_pm_opp_set_opp(pci->dev, opp); /* [한국어] 그 OPP 를 적용한다 — 대역폭뿐 아니라 전압 코너까지 함께 정해진다 */
			if (ret) /* [한국어] 적용이 실패했으면 */
				dev_err(pci->dev, "Failed to set OPP for freq (%lu): %d\n",
					freq_kbps * width, ret); /* [한국어] 알리기만 한다 */
			dev_pm_opp_put(opp); /* [한국어] 참조 카운트가 있는 객체라 반드시 놓는다 */
		}
	}
}

/* [한국어]
 * qcom_pcie_link_transition_count - 링크 절전 상태 전이 횟수를 debugfs 로 보여 준다
 *
 * @s:    seq_file.
 * @data: 쓰지 않는다.
 * @return: 늘 0.
 *
 * MHI 레지스터 블록에 하드웨어가 세어 둔 카운터 다섯을 그대로 찍는다 —
 * L0s, L1, L1.1, L1.2, L2 로의 전이 횟수다.
 *
 * 절전이 실제로 동작하는지 확인하는 진단 수단이다. ASPM 을 켰는데 카운터가
 * 늘지 않으면 링크가 절전 상태로 들어가지 못하고 있다는 뜻이다.
 *
 * pcie-qcom-ep.c 에 같은 이름·같은 구조의 함수가 있다. 다른 점은 읽는
 * 기준 주소로, RC 는 pcie->mhi 를, EP 는 pcie_ep->mmio 를 쓴다. 두 파일이
 * 공유 파일로 묶지 않고 각자 갖는 대표적인 예다.
 *
 * s->private 가 device 이고 거기서 drvdata 로 컨트롤러 상태를 되찾는다 —
 * debugfs_create_devm_seqfile 의 규약이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 파일을 읽을 때).
 *
 * 호출 체인:  debugfs → [이 함수] → readl_relaxed()
 */
static int qcom_pcie_link_transition_count(struct seq_file *s, void *data)
{
	struct qcom_pcie *pcie = (struct qcom_pcie *)dev_get_drvdata(s->private); /* [한국어] debugfs_create_devm_seqfile 의 규약대로 s->private 가 device 이고 거기서 drvdata 로 상태를 되찾는다 */

	seq_printf(s, "L0s transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L0S)); /* [한국어] L0s 로 들어간 횟수 */

	seq_printf(s, "L1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L1)); /* [한국어] L1 로 들어간 횟수 */

	seq_printf(s, "L1.1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1)); /* [한국어] L1.1 로 들어간 횟수 */

	seq_printf(s, "L1.2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2)); /* [한국어] L1.2 로 들어간 횟수 */

	seq_printf(s, "L2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L2)); /* [한국어] L2 로 들어간 횟수. ASPM 을 켰는데 이 값들이 늘지 않으면 절전이 동작하지 않는 것이다 */

	return 0; /* [한국어] seq_file 출력 완료 */
}

/* [한국어]
 * qcom_pcie_init_debugfs - 이 컨트롤러의 debugfs 디렉터리를 만든다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 디렉터리 이름을 DT 노드 경로로 짓는다. %pOFP 가 노드의 전체 경로를 찍는
 * 포맷이라, 컨트롤러가 여럿인 SoC 에서도 이름이 겹치지 않는다.
 *
 * 이름 할당이 실패하면 그냥 돌아간다. debugfs 는 진단용이라 없어도 동작에
 * 지장이 없기 때문이다 — 반환형이 void 인 것도 그 뜻이다.
 *
 * probe 는 pcie->mhi 가 있을 때만 이 함수를 부른다. 카운터가 MHI 블록에
 * 있어 그 창이 없으면 읽을 것이 없기 때문이다.
 *
 * devm 판 seqfile 을 쓰므로 파일 자체는 device 수명에 묶이지만, 디렉터리는
 * debugfs_create_dir 로 만들어 이 파일 어디에서도 지우지 않는다. 상류 코드
 * 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → [이 함수]
 */
static void qcom_pcie_init_debugfs(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 디렉터리 이름과 devm 할당의 기준 */
	char *name; /* [한국어] 만들 디렉터리 이름 */

	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node); /* [한국어] DT 노드 경로를 이름으로 쓴다 — 컨트롤러가 여럿인 SoC 에서 겹치지 않는다 */
	if (!name) /* [한국어] 이름을 못 만들었으면 */
		return; /* [한국어] debugfs 는 진단용이라 없어도 동작에 지장이 없다 — 반환형이 void 인 이유다 */

	pcie->debugfs = debugfs_create_dir(name, NULL); /* [한국어] 그 이름으로 디렉터리를 만든다 */
	debugfs_create_devm_seqfile(dev, "link_transition_count", pcie->debugfs,
				    qcom_pcie_link_transition_count); /* [한국어] 그 안에 카운터 파일을 얹는다. devm 판이라 device 수명에 묶인다 */
}

/* [한국어]
 * qcom_pci_free_msi - firmware_managed 갈래에서 MSI 자원을 놓는 devm 액션
 *
 * @ptr: devm_add_action_or_reset 에 넘긴 dw_pcie_rp 포인터.
 *
 * devm 정리 콜백이라 시그니처가 void 하나로 고정되어 있고, 그래서 안에서
 * 캐스팅해 쓴다.
 *
 * use_imsi_rx 를 확인하는 것이 요점이다. 그 플래그는
 * qcom_pcie_ecam_host_init() 이 MSI 초기화를 마친 뒤에야 세우므로,
 * 초기화 도중 실패한 경우에는 놓을 것이 없다.
 *
 * 이 함수는 firmware_managed 갈래에만 있다. 일반 갈래에서는 DWC 호스트
 * 코어가 MSI 수명을 관리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(device 해제 시).
 *
 * 호출 체인:  devm 정리 → [이 함수] → dw_pcie_free_msi()
 */
static void qcom_pci_free_msi(void *ptr)
{
	struct dw_pcie_rp *pp = (struct dw_pcie_rp *)ptr; /* [한국어] devm 정리 콜백이라 시그니처가 void 하나로 고정이어서 캐스팅해 쓴다 */

	if (pp && pp->use_imsi_rx) /* [한국어] MSI 초기화를 끝까지 마쳤을 때만 — 그 플래그를 초기화 뒤에 세우는 이유다 */
		dw_pcie_free_msi(pp); /* [한국어] DWC 의 MSI 자원을 놓는다 */
}

/* [한국어]
 * qcom_pcie_ecam_host_init - 펌웨어가 세워 둔 컨트롤러에 MSI 만 붙인다
 *
 * @cfg: pci-host-common 이 만든 ECAM config 창.
 * @return: 0 성공, -ENOMEM 은 할당 실패, 그 밖에는 MSI 초기화 오류.
 *
 * firmware_managed 갈래의 유일한 초기화 함수다. 이 파일의 나머지 코드와
 * 성격이 완전히 다르다 — 클록도, 리셋도, PHY 도, PARF 도 건드리지 않는다.
 * 펌웨어가 이미 다 해 두었기 때문이다.
 *
 * 그런데도 struct dw_pcie 를 하나 만들어 쓴다. DWC 의 MSI 구현
 * (dw_pcie_msi_host_init / dw_pcie_msi_init)을 쓰기 위해서이며, 그 함수들이
 * dw_pcie_rp 를 요구하기 때문이다. dbi_base 로 ECAM 창을 그대로 넘기는데,
 * config 공간과 DBI 가 같은 창으로 노출된 구성이라는 뜻이다.
 *
 * use_imsi_rx 를 MSI 초기화 **뒤에** 세우는 순서가 중요하다. 정리 콜백이
 * 그 플래그로 "놓을 것이 있는지" 를 판단하므로, 초기화 전에 세우면 실패
 * 경로에서 초기화되지 않은 것을 놓으려 한다.
 *
 * 마지막의 devm_add_action_or_reset 은 등록에 실패하면 곧바로 그 액션을
 * 실행해 준다 — 그래서 이 한 줄로 성공/실패 양쪽의 정리가 끝난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  pci_host_common_ecam_create() → pci_ecam_ops.init → [이 함수]
 */
static int qcom_pcie_ecam_host_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent; /* [한국어] ECAM 창을 만든 쪽이 넘겨 준 부모 device */
	struct dw_pcie_rp *pp; /* [한국어] DWC 의 루트 포트 구조체 */
	struct dw_pcie *pci; /* [한국어] 그것을 담을 컨트롤러 구조체 */
	int ret; /* [한국어] 각 단계의 결과 */

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL); /* [한국어] 하드웨어를 건드리지 않는 갈래인데도 DWC 구조체를 만든다 — 아래 MSI 구현이 그것을 요구하기 때문이다 */
	if (!pci) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	pci->dev = dev; /* [한국어] 로그와 devm 의 기준 */
	pp = &pci->pp; /* [한국어] 그 안의 루트 포트 구조체를 꺼내 둔다 */
	pci->dbi_base = cfg->win; /* [한국어] ECAM 창을 그대로 DBI 로 쓴다 — config 공간과 DBI 가 같은 창으로 노출된 구성이라는 뜻이다 */
	pp->num_vectors = MSI_DEF_NUM_VECTORS; /* [한국어] MSI 벡터 수를 기본값으로 둔다 */

	ret = dw_pcie_msi_host_init(pp); /* [한국어] DWC 의 MSI 도메인을 세운다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아직 정리 액션을 걸지 않아 되돌릴 것이 없다 */

	pp->use_imsi_rx = true; /* [한국어] 초기화가 끝난 **뒤에** 세운다 — 정리 콜백이 이 값으로 놓을 것이 있는지 판단하기 때문이다 */
	dw_pcie_msi_init(pp); /* [한국어] MSI 하드웨어를 초기화한다 */

	return devm_add_action_or_reset(dev, qcom_pci_free_msi, pp); /* [한국어] 정리 액션을 건다. 등록이 실패하면 그 액션을 곧바로 실행해 주므로 이 한 줄로 성공·실패 양쪽 정리가 끝난다 */
}

/* [한국어] firmware_managed 갈래 전용 ECAM 동작 표.
 * pci_host_common_ecam_create() 에 넘기며, config 접근은 코어의 범용 ECAM
 * 구현을 그대로 쓰고 init 만 이 파일이 제공한다. 이 파일의 나머지 코드가
 * 쓰는 DWC 경로와는 완전히 분리된 길이다. */
static const struct pci_ecam_ops pci_qcom_ecam_ops = {
	.init		= qcom_pcie_ecam_host_init, /* [한국어] MSI 만 붙이는 초기화 */
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus, /* [한국어] ECAM 규격대로 주소를 계산하는 범용 구현 */
		.read		= pci_generic_config_read, /* [한국어] 범용 읽기 */
		.write		= pci_generic_config_write, /* [한국어] 범용 쓰기 */
	}
};

/* Parse PERST# from all nodes in depth first manner starting from @np */
/* [한국어]
 * qcom_pcie_parse_perst - DT 를 깊이 우선으로 훑어 PERST# GPIO 를 모두 모은다
 *
 * @pcie: 컨트롤러 상태.
 * @port: 이 PERST# 들이 속할 포트.
 * @np:   훑기 시작할 노드.
 * @return: 0 성공, -ENOMEM 은 할당 실패, 그 밖에는 GPIO 조회 오류.
 *
 * 상류 주석이 밝히듯 @np 부터 깊이 우선으로 내려가며 reset-gpios 를 찾는다.
 * 포트 노드 자신이 가질 수도 있고, 그 아래 슬롯 노드들이 각자 가질 수도
 * 있기 때문이다. 재귀로 구현되어 있다.
 *
 * reset-gpios 속성이 없는 노드는 자식만 훑고 지나간다 — goto 로 아래 루프에
 * 바로 들어가는 것이 그 뜻이다.
 *
 * 찾은 것은 struct qcom_pcie_perst 로 감싸 포트의 목록에 매단다. 그 목록을
 * __qcom_pcie_perst_assert() 가 한꺼번에 훑는다.
 *
 * -EBUSY 특별 취급이 눈에 띈다. 상류 주석이 그 배경을 FIXME 로 남겨 두었다 —
 * GPIOLIB 이 지금은 배타적 접근만 지원하는데, 여러 슬롯이 PERST# 를 공유하는
 * 구성에는 비배타적 접근이 필요하다. 그것이 지원되면 여기에 구현하라는
 * 메모이며, 지금은 그 상황을 알아보기 쉬운 오류 메시지로 알리는 데 그친다.
 *
 * GPIOD_OUT_HIGH 로 잡는 것에 주의 — PERST# 는 active-low 라 "출력 High"가
 * 곧 리셋 인가 상태다. 즉 잡는 순간부터 하류 장치가 리셋에 들어간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_parse_port() → [이 함수] → (재귀) → [이 함수]
 */
static int qcom_pcie_parse_perst(struct qcom_pcie *pcie,
				 struct qcom_pcie_port *port,
				 struct device_node *np)
{
	struct device *dev = pcie->pci->dev; /* [한국어] GPIO 조회와 로그의 기준 */
	struct qcom_pcie_perst *perst; /* [한국어] 새로 만들 PERST# 항목 */
	struct gpio_desc *reset; /* [한국어] 조회한 GPIO 핸들 */
	int ret; /* [한국어] 재귀 호출의 결과 */

	if (!of_find_property(np, "reset-gpios", NULL)) /* [한국어] 이 노드에 reset-gpios 가 없으면 */
		goto parse_child_node; /* [한국어] 자기 것은 건너뛰고 자식만 훑는다 */

	reset = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np), "reset", /* [한국어] "reset" 이름으로 GPIO 를 얻는다 */
				      GPIOD_OUT_HIGH, "PERST#"); /* [한국어] 출력 High 로 잡는다 — PERST# 는 active-low 라 잡는 순간부터 리셋이 걸린다. EP 판은 같은 신호를 GPIOD_IN 으로 받는다 */
	if (IS_ERR(reset)) { /* [한국어] 조회에 실패했으면 */
		/*
		 * FIXME: GPIOLIB currently supports exclusive GPIO access only.
		 * Non exclusive access is broken. But shared PERST# requires
		 * non-exclusive access. So once GPIOLIB properly supports it,
		 * implement it here.
		 */
		if (PTR_ERR(reset) == -EBUSY) /* [한국어] 상류 FIXME 주석대로 GPIOLIB 이 배타적 접근만 지원해, 공유 PERST# 구성이면 여기로 온다 */
			dev_err(dev, "Shared PERST# is not supported\n"); /* [한국어] 그 상황을 알아보기 쉬운 메시지로 알린다 */

		return PTR_ERR(reset); /* [한국어] 그 오류를 그대로 올린다 */
	}

	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL); /* [한국어] 목록에 매달 껍데기를 잡는다 */
	if (!perst) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	INIT_LIST_HEAD(&perst->list); /* [한국어] 매달기 전에 고리를 초기화한다 */
	perst->desc = reset; /* [한국어] GPIO 핸들을 담는다 */
	list_add_tail(&perst->list, &port->perst); /* [한국어] 포트의 PERST# 목록 끝에 매단다 */

parse_child_node: /* [한국어] reset-gpios 가 없는 노드도 자식은 훑어야 한다 */
	for_each_available_child_of_node_scoped(np, child) { /* [한국어] available 상태인 자식만 훑는다. _scoped 판이라 참조가 자동으로 놓인다 */
		ret = qcom_pcie_parse_perst(pcie, port, child); /* [한국어] 같은 함수를 자식에 대해 다시 부른다 — 깊이 우선 순회다 */
		if (ret) /* [한국어] 어느 깊이에서든 실패하면 */
			return ret; /* [한국어] 그대로 올린다 */
	}

	return 0; /* [한국어] 이 노드와 그 아래 PERST# 를 모두 모았다 */
}

/* [한국어]
 * qcom_pcie_parse_port - DT 포트 노드 하나에서 PHY 와 PERST# 를 모은다
 *
 * @pcie: 컨트롤러 상태.
 * @node: 포트 노드.
 * @return: 0 성공, -ENOMEM 은 할당 실패, 그 밖에는 PHY/GPIO 오류.
 *
 * 포트 하나가 PHY 하나와 PERST# 여럿을 갖는 구조다.
 *
 * 순서를 짚어 둘 만하다 — PHY 를 먼저 얻고 구조체를 잡은 뒤 phy_init 을
 * 부르고, 그다음 PERST# 를 훑는다. phy_init 이 실패하면 아직 목록에 넣지
 * 않았으므로 되돌릴 것이 없다.
 *
 * 다만 PERST# 훑기가 실패하면 이미 초기화한 PHY 를 정리하지 않고 돌아간다.
 * 그 정리는 호출자인 qcom_pcie_parse_ports() 의 err_port_del 이 맡는데,
 * 그 시점에는 이 포트가 아직 목록에 없어 정리 대상에서 빠진다 — 상류 코드
 * 그대로이며 여기서는 고치지 않는다.
 *
 * 성공하면 마지막에 포트를 컨트롤러의 목록에 매단다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_parse_ports() → [이 함수] → qcom_pcie_parse_perst()
 */
static int qcom_pcie_parse_port(struct qcom_pcie *pcie, struct device_node *node)
{
	struct device *dev = pcie->pci->dev; /* [한국어] PHY 조회와 devm 할당의 기준 */
	struct qcom_pcie_port *port; /* [한국어] 새로 만들 포트 */
	struct phy *phy; /* [한국어] 이 포트의 PHY */
	int ret; /* [한국어] 각 단계의 결과 */

	phy = devm_of_phy_get(dev, node, NULL); /* [한국어] 포트 노드에서 PHY 를 얻는다. 이름 없이(NULL) 첫 번째를 쓴다 */
	if (IS_ERR(phy)) /* [한국어] 없으면 */
		return PTR_ERR(phy); /* [한국어] 그 오류를 올린다 */

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL); /* [한국어] 포트 껍데기를 잡는다 */
	if (!port) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	ret = phy_init(phy); /* [한국어] PHY 를 초기화한다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아직 목록에 넣지 않아 되돌릴 것이 없다 */

	INIT_LIST_HEAD(&port->perst); /* [한국어] PERST# 목록을 비워 둔 채로 초기화한다 */

	ret = qcom_pcie_parse_perst(pcie, port, node); /* [한국어] 이 노드 아래의 PERST# 를 모두 모은다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 이미 초기화한 PHY 를 정리하지 않고 돌아간다 — 이 포트는 아직 목록에 없어 호출자의 정리 대상에서도 빠진다. 상류 코드 그대로다 */

	port->phy = phy; /* [한국어] 모두 성공했으므로 PHY 를 담는다 */
	INIT_LIST_HEAD(&port->list); /* [한국어] 매달기 전에 고리를 초기화한다 */
	list_add_tail(&port->list, &pcie->ports); /* [한국어] 컨트롤러의 포트 목록 끝에 매단다 */

	return 0; /* [한국어] 포트 하나를 다 모았다 */
}

/* [한국어]
 * qcom_pcie_parse_ports - DT 의 모든 포트 노드를 훑는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, -ENODEV 는 포트 노드가 하나도 없는 경우, 그 밖에는 하위 오류.
 *
 * 반환값 규약이 이 함수의 핵심이다. ret 를 -ENODEV 로 초기화해 두고, 포트를
 * 하나라도 처리하면 그 결과(0)로 덮인다. 그래서 **포트 노드가 없으면
 * -ENODEV 가 그대로 반환**되고, 호출자인 probe 가 그것을 "새 바인딩이 아니다"
 * 라는 신호로 받아 레거시 바인딩 파싱으로 넘어간다. 오류가 아니라 분기
 * 신호로 쓰이는 값이다.
 *
 * device_type = "pci" 인 자식만 포트로 본다. DT 노드 아래에는 그 밖의
 * 노드(전원 제어 등)도 있을 수 있기 때문이다.
 *
 * 되돌리기가 이중 루프다 — 포트마다 PERST# 목록을 비우고, PHY 를 정리하고,
 * 포트를 목록에서 뺀다. probe 의 err_phy_exit 와 같은 모양이며, 두 곳이
 * 같은 코드를 갖는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe() → [이 함수] → qcom_pcie_parse_port()
 */
static int qcom_pcie_parse_ports(struct qcom_pcie *pcie)
{
	struct qcom_pcie_perst *perst, *tmp_perst; /* [한국어] 되돌리기의 PERST# 순회 항목(안전 판이라 삭제 중에도 안전하다) */
	struct qcom_pcie_port *port, *tmp_port; /* [한국어] 되돌리기의 포트 순회 항목 */
	struct device *dev = pcie->pci->dev; /* [한국어] DT 노드와 로그의 기준 */
	int ret = -ENODEV; /* [한국어] **포트 노드가 하나도 없을 때 그대로 반환되는 값**. 오류가 아니라 "새 바인딩이 아니다" 라는 분기 신호로 쓰인다 */

	for_each_available_child_of_node_scoped(dev->of_node, of_port) { /* [한국어] 컨트롤러 노드의 자식을 훑는다 */
		if (!of_node_is_type(of_port, "pci")) /* [한국어] device_type 이 "pci" 가 아닌 노드는 */
			continue; /* [한국어] 포트가 아니므로 건너뛴다(전원 제어 노드 등) */
		ret = qcom_pcie_parse_port(pcie, of_port); /* [한국어] 포트 하나를 파싱한다. 성공하면 ret 가 0 으로 덮여 -ENODEV 가 사라진다 */
		if (ret) /* [한국어] 실패했으면 */
			goto err_port_del; /* [한국어] 지금까지 만든 포트를 모두 되돌린다 */
	}

	return ret; /* [한국어] 포트를 하나라도 처리했으면 0, 하나도 없었으면 -ENODEV 다 */

err_port_del: /* [한국어] 파싱 실패가 여기로 온다 */
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) { /* [한국어] 만들어 둔 포트를 하나씩 */
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list)
			list_del(&perst->list); /* [한국어] 그 PERST# 목록을 먼저 비우고 */
		phy_exit(port->phy); /* [한국어] PHY 초기화를 되돌리고 */
		list_del(&port->list); /* [한국어] 포트를 목록에서 뺀다 */
	}

	return ret; /* [한국어] 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_parse_legacy_binding - 옛 DT 바인딩에서 PHY 와 PERST# 를 얻는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, -ENOMEM 은 할당 실패, 그 밖에는 PHY/GPIO 오류.
 *
 * DT 하위 호환을 위한 갈래다. 새 바인딩은 포트를 자식 노드로 두지만, 옛
 * 바인딩은 호스트 브리지 노드 자신이 "pciephy" PHY 와 "perst" GPIO 를
 * 직접 갖는다.
 *
 * 그래도 내부 자료구조는 같게 만든다 — 포트 하나와 PERST# 하나를 만들어
 * 같은 목록에 매단다. 그래서 이후의 모든 코드
 * (__qcom_pcie_perst_assert, qcom_pcie_phy_power_on 등)가 두 바인딩을
 * 구분할 필요가 없다. 두 파싱 함수의 존재 이유가 바로 이 통일이다.
 *
 * 둘 다 optional 판으로 잡는다는 점이 새 바인딩과 다르다 — 옛 보드에는
 * PHY 나 PERST# 가 없는 구성도 있어, 없으면 NULL 로 두고 넘어간다.
 * gpiod_set_value_cansleep 은 NULL 을 무해하게 처리한다.
 *
 * INIT_LIST_HEAD 호출 순서가 조금 어지럽다 — port->perst 를 perst->desc
 * 대입 뒤에 초기화하는데, 그 사이에 그 목록을 쓰지 않으므로 결과는 같다.
 * 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_probe()(새 바인딩이 -ENODEV 일 때) → [이 함수]
 */
static int qcom_pcie_parse_legacy_binding(struct qcom_pcie *pcie)
{
	struct device *dev = pcie->pci->dev;
	struct qcom_pcie_perst *perst; /* [한국어] 만들 PERST# 항목 */
	struct qcom_pcie_port *port; /* [한국어] 만들 포트 */
	struct gpio_desc *reset; /* [한국어] 조회한 GPIO 핸들 */
	struct phy *phy; /* [한국어] 조회한 PHY */
	int ret; /* [한국어] 각 단계의 결과 */

	phy = devm_phy_optional_get(dev, "pciephy"); /* [한국어] 옛 바인딩은 호스트 브리지 노드가 "pciephy" 를 직접 갖는다. optional 이라 없어도 된다 */
	if (IS_ERR(phy)) /* [한국어] 실제 오류이면 */
		return PTR_ERR(phy); /* [한국어] 그대로 올린다 */

	reset = devm_gpiod_get_optional(dev, "perst", GPIOD_OUT_HIGH); /* [한국어] 같은 노드의 "perst" GPIO. 새 바인딩과 달리 이름이 짧다 */
	if (IS_ERR(reset)) /* [한국어] 실제 오류이면 */
		return PTR_ERR(reset); /* [한국어] 그대로 올린다 */

	ret = phy_init(phy); /* [한국어] PHY 를 초기화한다. NULL 이면 무해하게 통과한다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 그대로 올린다 */

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL); /* [한국어] 포트 껍데기를 잡는다 — 새 바인딩과 같은 자료구조로 만들기 위해서다 */
	if (!port) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL); /* [한국어] PERST# 껍데기도 잡는다 */
	if (!perst) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다. 앞서 잡은 port 는 devm 이라 따로 놓지 않는다 */

	port->phy = phy; /* [한국어] PHY 를 담고 */
	INIT_LIST_HEAD(&port->list); /* [한국어] 고리를 초기화한 뒤 */
	list_add_tail(&port->list, &pcie->ports); /* [한국어] 컨트롤러의 포트 목록에 매단다 */

	perst->desc = reset; /* [한국어] GPIO 핸들을 담는다 */
	INIT_LIST_HEAD(&port->perst); /* [한국어] 포트의 PERST# 목록을 초기화한다. 위 대입보다 뒤에 오지만 그사이 그 목록을 쓰지 않아 결과는 같다 */
	INIT_LIST_HEAD(&perst->list); /* [한국어] 항목의 고리도 초기화하고 */
	list_add_tail(&perst->list, &port->perst); /* [한국어] 그 목록에 매단다 — 이제 새 바인딩과 같은 모양이 되어 이후 코드가 둘을 구분할 필요가 없다 */

	return 0; /* [한국어] 옛 바인딩 파싱 완료 */
}

/* [한국어]
 * qcom_pcie_probe - 플랫폼 디바이스를 받아 PCIe 호스트를 세운다
 *
 * @pdev: DT 가 만든 플랫폼 디바이스.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일의 입구이며, **두 갈래로 완전히 나뉜다**.
 *
 *   [firmware_managed 갈래]
 *   sa8255p 처럼 펌웨어가 컨트롤러를 이미 세워 둔 플랫폼이다. 이 드라이버는
 *   하드웨어를 건드리지 않고, pci_host_common_ecam_create() 로 ECAM 창만
 *   열어 PCI 코어에 넘긴 뒤 그 자리에서 돌아간다. 아래의 자원 확보·PARF
 *   프로그래밍은 하나도 실행되지 않는다. 그래서 cfg_fw_managed 에는 ops 가
 *   아예 없고, 위쪽의 유효성 검사도 "firmware_managed 가 아닌데 ops 가
 *   없으면 오류" 라는 형태로 되어 있다.
 *
 *   [일반 갈래]
 *   1) qcom_pcie / dw_pcie 를 잡고 서로 연결한다. dw_pcie_ops 를 꽂는 것이
 *      DWC 코어와의 접점이다.
 *   2) PARF 창을 매핑한다. MHI 창은 선택이며, 있으면 debugfs 카운터를
 *      읽는 데 쓴다.
 *   3) **전력 관리 방식을 고른다.** OPP 표가 DT 에 있으면 그것을 쓰고,
 *      없으면(-ENODEV) 인터커넥트를 직접 다룬다. 상류 주석대로 OPP 를 쓸
 *      때는 링크가 서기 전에 표에서 가장 높은 OPP 를 걸어 최대 전압 코너를
 *      확보한다 — 그래야 링크가 최대 속도로 설 수 있다. 그 값은 probe
 *      끝의 qcom_pcie_icc_opp_update() 가 실제 링크에 맞게 낮춘다.
 *   4) cfg->ops->get_resources 로 세대별 자원을 확보한다.
 *   5) 포트를 파싱한다. 새 바인딩을 먼저 시도하고, -ENODEV 이면 상류
 *      주석대로 DT 하위 호환을 위해 레거시 바인딩으로 되돌아간다.
 *   6) dw_pcie_host_init() 으로 DWC 코어에 넘긴다. 그 안에서
 *      qcom_pcie_host_init() 이 되불린다.
 *   7) 링크가 선 뒤 대역폭/전압을 실제 링크에 맞춘다.
 *
 * pm_runtime 을 가장 먼저 켜는 것에 주의. 아래 모든 레지스터 접근이 전원
 * 도메인이 깨어 있어야 가능하기 때문이며, 그래서 되돌리기의 마지막도
 * pm_runtime 이다.
 *
 * 되돌리기가 라벨 둘이다. err_phy_exit 의 이중 루프가
 * qcom_pcie_parse_ports() 의 것과 같은 모양이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → cfg->ops->get_resources()
 *               → qcom_pcie_parse_ports() → dw_pcie_host_init()
 *                 → pp->ops->init → qcom_pcie_host_init()
 */
static int qcom_pcie_probe(struct platform_device *pdev)
{
	struct qcom_pcie_perst *perst, *tmp_perst; /* [한국어] 되돌리기의 PERST# 순회 항목 */
	struct qcom_pcie_port *port, *tmp_port; /* [한국어] 되돌리기의 포트 순회 항목 */
	const struct qcom_pcie_cfg *pcie_cfg; /* [한국어] 이 SoC 의 설정 표 */
	unsigned long max_freq = ULONG_MAX; /* [한국어] OPP 를 찾을 때 상한으로 쓸 값 */
	struct device *dev = &pdev->dev; /* [한국어] 로그와 devm 의 기준 */
	struct dev_pm_opp *opp; /* [한국어] 찾은 OPP 핸들 */
	struct qcom_pcie *pcie; /* [한국어] 이 파일의 컨트롤러 상태 */
	struct dw_pcie_rp *pp; /* [한국어] DWC 의 루트 포트 구조체 */
	struct resource *res; /* [한국어] MHI 자원 조회 결과 */
	struct dw_pcie *pci; /* [한국어] DWC 의 컨트롤러 구조체 */
	int ret; /* [한국어] 각 단계의 결과 */

	pcie_cfg = of_device_get_match_data(dev); /* [한국어] DT compatible 에 대응하는 설정 표를 꺼낸다 */
	if (!pcie_cfg) { /* [한국어] 매칭 표에 데이터가 없으면 */
		dev_err(dev, "No platform data\n"); /* [한국어] 알리고 */
		return -ENODATA; /* [한국어] 그대로 돌아간다 */
	}

	if (!pcie_cfg->firmware_managed && !pcie_cfg->ops) { /* [한국어] 펌웨어 관리가 아닌데 ops 도 없으면 */
		dev_err(dev, "No platform ops\n"); /* [한국어] 세대별 동작을 할 수 없다 */
		return -ENODATA; /* [한국어] 그대로 돌아간다. 이 검사가 cfg_fw_managed 만 ops 를 비워 두는 것을 허용한다 */
	}

	pm_runtime_enable(dev); /* [한국어] 아래 모든 레지스터 접근이 전원 도메인이 깨어 있어야 가능하다 */
	ret = pm_runtime_get_sync(dev); /* [한국어] 참조를 잡아 실제로 깨운다 */
	if (ret < 0) /* [한국어] 못 깨웠으면 */
		goto err_pm_runtime_put; /* [한국어] 참조를 놓고 런타임 PM 을 끈다 */

	if (pcie_cfg->firmware_managed) { /* [한국어] **여기서 길이 완전히 갈린다** — 펌웨어가 이미 세워 둔 컨트롤러다 */
		struct pci_host_bridge *bridge; /* [한국어] 그 갈래가 쓸 호스트 브리지 */
		struct pci_config_window *cfg; /* [한국어] ECAM config 창 */

		bridge = devm_pci_alloc_host_bridge(dev, 0); /* [한국어] 브리지를 잡는다. private 영역이 필요 없어 크기가 0 이다 */
		if (!bridge) { /* [한국어] 메모리가 없으면 */
			ret = -ENOMEM; /* [한국어] 그대로 정리로 간다 */
			goto err_pm_runtime_put; /* [한국어] 런타임 PM 만 되돌리면 된다 */
		}

		/* Parse and map our ECAM configuration space area */
		cfg = pci_host_common_ecam_create(dev, bridge,
				&pci_qcom_ecam_ops); /* [한국어] 상류 주석대로 ECAM config 공간을 파싱해 매핑한다. 이 한 줄이 하드웨어 초기화를 대신한다 */
		if (IS_ERR(cfg)) { /* [한국어] 실패했으면 */
			ret = PTR_ERR(cfg); /* [한국어] 그 오류를 꺼내 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
		}

		bridge->sysdata = cfg; /* [한국어] ECAM 창을 브리지의 private 데이터로 둔다 */
		bridge->ops = (struct pci_ops *)&pci_qcom_ecam_ops.pci_ops; /* [한국어] config 접근을 범용 ECAM 구현으로 넘긴다 */
		bridge->msi_domain = true; /* [한국어] MSI 도메인을 쓴다고 표시한다 — 실제 MSI 는 위 ops 의 init 이 세운다 */

		ret = pci_host_probe(bridge); /* [한국어] PCI 코어에 열거를 넘긴다 */
		if (ret) /* [한국어] 실패했으면 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */

		return 0; /* [한국어] **여기서 probe 가 끝난다** — 아래의 자원 확보와 PARF 프로그래밍은 실행되지 않는다 */
	}

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL); /* [한국어] 이 파일의 컨트롤러 상태를 잡는다 */
	if (!pcie) { /* [한국어] 메모리가 없으면 */
		ret = -ENOMEM; /* [한국어] 그대로 */
		goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL); /* [한국어] DWC 의 컨트롤러 구조체도 따로 잡는다. 호스트 브리지의 private 영역은 DWC 코어가 쓰므로 여기서 별도 할당이다 */
	if (!pci) { /* [한국어] 메모리가 없으면 */
		ret = -ENOMEM; /* [한국어] 그대로 */
		goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	INIT_LIST_HEAD(&pcie->ports); /* [한국어] 포트 목록을 비운 채로 초기화한다 */

	pci->dev = dev; /* [한국어] 로그와 devm 의 기준 */
	pci->ops = &dw_pcie_ops; /* [한국어] 링크 상태·시작 콜백 표를 꽂는다 — DWC 코어와의 접점 하나다 */
	pp = &pci->pp; /* [한국어] 루트 포트 구조체를 꺼내 둔다 */

	pcie->pci = pci; /* [한국어] 두 구조체를 잇는다 */

	pcie->cfg = pcie_cfg; /* [한국어] SoC 설정 표를 꽂는다. 이 한 줄이 아래 모든 세대 분기의 출발점이다 */

	pcie->parf = devm_platform_ioremap_resource_byname(pdev, "parf"); /* [한국어] 퀄컴 고유 제어 레지스터 창을 매핑한다 */
	if (IS_ERR(pcie->parf)) { /* [한국어] 실패했으면 */
		ret = PTR_ERR(pcie->parf); /* [한국어] 그 오류를 꺼내 */
		goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	/* MHI region is optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mhi"); /* [한국어] 상류 주석대로 MHI 창은 선택이다 */
	if (res) { /* [한국어] DT 가 주었으면 */
		pcie->mhi = devm_ioremap_resource(dev, res); /* [한국어] 매핑한다 */
		if (IS_ERR(pcie->mhi)) { /* [한국어] 실패했으면 */
			ret = PTR_ERR(pcie->mhi); /* [한국어] 그 오류를 꺼내 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
		}
	}

	/* OPP table is optional */
	ret = devm_pm_opp_of_add_table(dev); /* [한국어] 상류 주석대로 OPP 표도 선택이다 */
	if (ret && ret != -ENODEV) { /* [한국어] -ENODEV(표 없음) 가 아닌 실제 오류이면 */
		dev_err_probe(dev, ret, "Failed to add OPP table\n"); /* [한국어] 알리고 */
		goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	/*
	 * Before the PCIe link is initialized, vote for highest OPP in the OPP
	 * table, so that we are voting for maximum voltage corner for the
	 * link to come up in maximum supported speed. At the end of the
	 * probe(), OPP will be updated using qcom_pcie_icc_opp_update().
	 */
	if (!ret) { /* [한국어] 표가 있었으면(ret 가 0) */
		opp = dev_pm_opp_find_freq_floor(dev, &max_freq); /* [한국어] 가장 높은 주파수의 OPP 를 찾는다 */
		if (IS_ERR(opp)) { /* [한국어] 못 찾았으면 */
			ret = PTR_ERR(opp); /* [한국어] 그 오류를 꺼내 */
			dev_err_probe(pci->dev, ret,
				      "Unable to find max freq OPP\n"); /* [한국어] 알리고 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
		} else {
			ret = dev_pm_opp_set_opp(dev, opp); /* [한국어] 상류 주석대로 링크가 최대 속도로 설 수 있도록 최고 전압 코너를 미리 건다 */
		}

		dev_pm_opp_put(opp); /* [한국어] 참조 카운트가 있는 객체라 반드시 놓는다. 적용 실패 여부와 무관하게 놓아야 한다 */
		if (ret) { /* [한국어] 적용이 실패했으면 */
			dev_err_probe(pci->dev, ret,
				      "Failed to set OPP for freq %lu\n",
				      max_freq); /* [한국어] 알리고 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
		}

		pcie->use_pm_opp = true; /* [한국어] 이후 전력 관리를 OPP 로 한다고 표시한다 */
	} else { /* [한국어] 표가 없었으면(-ENODEV) */
		/* Skip ICC init if OPP is supported as it is handled by OPP */
		ret = qcom_pcie_icc_init(pcie); /* [한국어] 상류 주석대로 인터커넥트를 직접 다룬다. OPP 를 쓰면 그쪽이 대역폭까지 함께 다루므로 이 갈래를 건너뛴다 */
		if (ret) /* [한국어] 실패했으면 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	ret = pcie->cfg->ops->get_resources(pcie); /* [한국어] 세대별 클록·리셋·레귤레이터를 확보한다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_pm_runtime_put; /* [한국어] 정리로 간다 */

	pp->ops = &qcom_pcie_dw_ops; /* [한국어] DWC 호스트 코어가 되부를 세 콜백을 꽂는다 */

	ret = qcom_pcie_parse_ports(pcie); /* [한국어] 새 바인딩으로 포트를 파싱한다 */
	if (ret) { /* [한국어] 실패했으면 */
		if (ret != -ENODEV) { /* [한국어] -ENODEV 가 아닌 실제 오류이면 */
			dev_err_probe(pci->dev, ret,
				      "Failed to parse Root Port: %d\n", ret); /* [한국어] 알리고 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
		}

		/*
		 * In the case of properties not populated in Root Port node,
		 * fallback to the legacy method of parsing the Host Bridge
		 * node. This is to maintain DT backwards compatibility.
		 */
		ret = qcom_pcie_parse_legacy_binding(pcie); /* [한국어] 상류 주석대로 포트 노드가 없는 경우 DT 하위 호환을 위해 옛 바인딩으로 되돌아간다 */
		if (ret) /* [한국어] 그것마저 실패하면 */
			goto err_pm_runtime_put; /* [한국어] 정리로 간다 */
	}

	platform_set_drvdata(pdev, pcie); /* [한국어] PM 콜백과 to_qcom_pcie 매크로가 되찾을 값을 걸어 둔다 */

	ret = dw_pcie_host_init(pp); /* [한국어] **여기서 DWC 코어로 넘어간다**. 그 안에서 qcom_pcie_host_init() 이 되불린다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err_probe(dev, ret, "cannot initialize host\n"); /* [한국어] 알리고 */
		goto err_phy_exit; /* [한국어] PHY 부터 되돌린다 */
	}

	qcom_pcie_icc_opp_update(pcie); /* [한국어] 링크가 선 뒤 대역폭·전압을 실제 링크에 맞춘다 */

	if (pcie->mhi) /* [한국어] MHI 창이 있을 때만 — 읽을 카운터가 그 안에 있기 때문이다 */
		qcom_pcie_init_debugfs(pcie); /* [한국어] debugfs 를 만든다 */

	return 0; /* [한국어] 컨트롤러가 완전히 준비되었다 */

err_phy_exit: /* [한국어] 호스트 초기화 실패가 여기로 온다 */
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) { /* [한국어] 만들어 둔 포트를 하나씩 */
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list)
			list_del(&perst->list); /* [한국어] 그 PERST# 목록을 비우고 */
		phy_exit(port->phy); /* [한국어] PHY 초기화를 되돌리고 */
		list_del(&port->list); /* [한국어] 포트를 목록에서 뺀다. qcom_pcie_parse_ports() 의 되돌리기와 같은 모양이다 */
	}
err_pm_runtime_put: /* [한국어] 그 앞 단계들의 실패가 여기로 온다 */
	pm_runtime_put(dev); /* [한국어] 런타임 PM 참조를 놓고 */
	pm_runtime_disable(dev); /* [한국어] 런타임 PM 자체를 끈다 — 가장 먼저 켠 것을 가장 나중에 되돌린다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_suspend_noirq - 서스펜드 직전 대역폭을 낮추고 조건부로 컨트롤러를 끈다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 인터커넥트 조작 실패는 그 오류.
 *
 * 이 파일에서 가장 판단이 많은 함수이며, 상류가 긴 주석 셋으로 그 근거를
 * 남겨 두었다.
 *
 *   1) pcie-mem 대역폭을 최소값으로 낮춘다. 상류 주석대로 서스펜드 동안에도
 *      데이터 경로를 살려 두기 위해서다 — 0 으로 끊지 않는다.
 *
 *   2) **핵심 판단**: 링크가 서 있지 않을 때만 컨트롤러 자원을 끈다.
 *      상류 주석이 이유를 둘 든다.
 *        - 활성 장치가 붙어 있는데 자원을 끄면, 커널이 서스펜드 막바지에
 *          MSI 를 마스크하려고 그 장치의 config 공간에 접근하다가 접근
 *          위반이 난다.
 *        - 링크를 L2/L3 로 내리는 것도 바람직하지 않은데, 그러면 VDD 가
 *          끊겨 장치가 파워다운되기 때문이다. 상류 주석이 그 영향을
 *          **"NVMe 같은 스토리지 장치의 수명"** 이라고 명시한다.
 *      그래서 장치가 붙어 있으면 자원을 켠 채로 두고, 링크가 L0/L1 서브
 *      상태에 머물기를 기대한다.
 *      끈 경우에만 suspended 를 참으로 세워, resume 이 되살릴지 판단한다.
 *
 *   3) cpu-pcie 경로는 S2RAM 이 아닐 때만 끊는다. 상류 주석대로 일부
 *      플랫폼에서 S2RAM 막바지에 DBI 접근이 일어날 수 있어, 그 경로가
 *      죽어 있으면 NoC 오류가 나기 때문이다. OPP 해제도 같은 조건 아래
 *      둔다.
 *
 * drvdata 가 NULL 이면 곧바로 0 을 돌려준다 — firmware_managed 갈래는
 * platform_set_drvdata 를 부르지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 비활성(noirq) 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → qcom_pcie_host_deinit()
 */
static int qcom_pcie_suspend_noirq(struct device *dev)
{
	struct qcom_pcie *pcie; /* [한국어] 이 파일의 컨트롤러 상태 */
	int ret = 0; /* [한국어] 인터커넥트 조작 결과. 아래 조건들을 건너뛸 수 있어 0 으로 시작한다 */

	pcie = dev_get_drvdata(dev); /* [한국어] probe 가 걸어 둔 상태를 되찾는다 */
	if (!pcie) /* [한국어] firmware_managed 갈래는 platform_set_drvdata 를 부르지 않아 NULL 이다 */
		return 0; /* [한국어] 그 경우 할 일이 없다 */

	/*
	 * Set minimum bandwidth required to keep data path functional during
	 * suspend.
	 */
	if (pcie->icc_mem) { /* [한국어] 인터커넥트를 직접 다루는 플랫폼이면 */
		ret = icc_set_bw(pcie->icc_mem, 0, kBps_to_icc(1)); /* [한국어] 상류 주석대로 서스펜드 동안에도 데이터 경로를 살려 두기 위해 0 이 아니라 최소값을 건다 */
		if (ret) { /* [한국어] 요구가 실패했으면 */
			dev_err(dev,
				"Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret); /* [한국어] 알리고 */
			return ret; /* [한국어] 서스펜드를 막는다 */
		}
	}

	/*
	 * Turn OFF the resources only for controllers without active PCIe
	 * devices. For controllers with active devices, the resources are kept
	 * ON and the link is expected to be in L0/L1 (sub)states.
	 *
	 * Turning OFF the resources for controllers with active PCIe devices
	 * will trigger access violation during the end of the suspend cycle,
	 * as kernel tries to access the PCIe devices config space for masking
	 * MSIs.
	 *
	 * Also, it is not desirable to put the link into L2/L3 state as that
	 * implies VDD supply will be removed and the devices may go into
	 * powerdown state. This will affect the lifetime of the storage devices
	 * like NVMe.
	 */
	if (!dw_pcie_link_up(pcie->pci)) { /* [한국어] **핵심 판단** — 링크가 서 있지 않을 때만 자원을 끈다 */
		qcom_pcie_host_deinit(&pcie->pci->pp); /* [한국어] 컨트롤러를 끈다 */
		pcie->suspended = true; /* [한국어] resume 이 되살릴 수 있게 표시해 둔다 */
	}

	/*
	 * Only disable CPU-PCIe interconnect path if the suspend is non-S2RAM.
	 * Because on some platforms, DBI access can happen very late during the
	 * S2RAM and a non-active CPU-PCIe interconnect path may lead to NoC
	 * error.
	 */
	if (pm_suspend_target_state != PM_SUSPEND_MEM) { /* [한국어] 상류 주석대로 S2RAM 이 아닐 때만 CPU 경로를 끊는다 */
		ret = icc_disable(pcie->icc_cpu); /* [한국어] 그 경로를 끈다 */
		if (ret)
			dev_err(dev, "Failed to disable CPU-PCIe interconnect path: %d\n", ret);

		if (pcie->use_pm_opp) /* [한국어] OPP 를 쓰는 플랫폼이면 */
			dev_pm_opp_set_opp(pcie->pci->dev, NULL); /* [한국어] OPP 를 해제해 전압 코너를 낮춘다. icc 갈래의 대역폭 축소와 대응된다 */
	}
	return ret;
}

/* [한국어]
 * qcom_pcie_resume_noirq - 서스펜드에서 깨어나 컨트롤러를 되살린다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * suspend 의 역순이며, 조건도 그대로 대칭이다.
 *
 *   1) S2RAM 이 아니었으면 cpu-pcie 경로를 다시 켠다. 끊은 조건과 같은
 *      조건으로 켜야 짝이 맞는다.
 *   2) suspend 가 실제로 컨트롤러를 껐을 때만(suspended 가 참) 되살린다.
 *      되살리는 수단이 qcom_pcie_host_init() 인 것이 눈에 띈다 — DWC 코어의
 *      콜백을 이 파일이 직접 부르는 것으로, probe 경로와 완전히 같은 절차를
 *      다시 밟는다는 뜻이다.
 *   3) 대역폭과 전압을 실제 링크에 맞춘다. 이것은 조건 없이 늘 한다 —
 *      링크가 살아 있던 경우에도 suspend 가 대역폭을 최소로 낮춰 두었기
 *      때문이다.
 *
 * drvdata 가 NULL 이면 곧바로 돌아가는 것도 suspend 와 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, noirq 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → qcom_pcie_host_init()
 *               → qcom_pcie_icc_opp_update()
 */
static int qcom_pcie_resume_noirq(struct device *dev)
{
	struct qcom_pcie *pcie;
	int ret; /* [한국어] 각 단계의 결과 */

	pcie = dev_get_drvdata(dev); /* [한국어] probe 가 걸어 둔 상태를 되찾는다 */
	if (!pcie) /* [한국어] firmware_managed 갈래는 NULL 이라 */
		return 0;

	if (pm_suspend_target_state != PM_SUSPEND_MEM) { /* [한국어] suspend 가 끊은 조건과 같은 조건으로 켜야 짝이 맞는다 */
		ret = icc_enable(pcie->icc_cpu); /* [한국어] CPU 경로를 다시 켠다 */
		if (ret) { /* [한국어] 못 켰으면 */
			dev_err(dev, "Failed to enable CPU-PCIe interconnect path: %d\n", ret); /* [한국어] 알리고 */
			return ret;
		}
	}

	if (pcie->suspended) { /* [한국어] suspend 가 실제로 컨트롤러를 껐을 때만 */
		ret = qcom_pcie_host_init(&pcie->pci->pp); /* [한국어] probe 와 같은 절차로 되살린다 — DWC 코어의 콜백을 이 파일이 직접 부르는 자리다 */
		if (ret) /* [한국어] 되살리지 못했으면 */
			return ret;

		pcie->suspended = false; /* [한국어] 되살렸으므로 표시를 지운다 */
	}

	qcom_pcie_icc_opp_update(pcie);

	return 0;
}

/* [한국어] DT compatible 문자열과 cfg 표를 잇는 매칭 표.
 * probe 가 of_device_get_match_data() 로 여기서 표를 꺼내며, 그 한 번의
 * 조회가 이 드라이버의 모든 세대 분기의 출발점이 된다.
 *
 * 이 표를 세로로 읽으면 이 파일의 성격이 한눈에 들어온다 — 2013년경의
 * apq8064 부터 최신 x1e80100 까지 스물여덟 개의 compatible 이 아홉 개의
 * cfg 를 나눠 쓴다. */
static const struct of_device_id qcom_pcie_match[] = {
	{ .compatible = "qcom,pcie-apq8064", .data = &cfg_2_1_0 }, /* [한국어] 2013년경 SoC. 이 파일이 담는 가장 오래된 세대다 */
	{ .compatible = "qcom,pcie-apq8084", .data = &cfg_1_0_0 }, /* [한국어] 1.0.0 세대 */
	{ .compatible = "qcom,pcie-ipq4019", .data = &cfg_2_4_0 }, /* [한국어] 2.4.0 세대(리셋 열둘) */
	{ .compatible = "qcom,pcie-ipq5018", .data = &cfg_2_9_0 }, /* [한국어] 2.9.0 세대 */
	{ .compatible = "qcom,pcie-ipq6018", .data = &cfg_2_9_0 }, /* [한국어] 2.9.0 세대 */
	{ .compatible = "qcom,pcie-ipq8064", .data = &cfg_2_1_0 }, /* [한국어] 2.1.0 세대 */
	{ .compatible = "qcom,pcie-ipq8064-v2", .data = &cfg_2_1_0 }, /* [한국어] 같은 2.1.0 세대의 개정판 */
	{ .compatible = "qcom,pcie-ipq8074", .data = &cfg_2_3_3 }, /* [한국어] 2.3.3 세대 */
	{ .compatible = "qcom,pcie-ipq8074-gen3", .data = &cfg_2_9_0 }, /* [한국어] 같은 SoC 의 Gen3 판은 2.9.0 세대를 쓴다 — compatible 이 갈리는 예다 */
	{ .compatible = "qcom,pcie-ipq9574", .data = &cfg_2_9_0 }, /* [한국어] 2.9.0 세대 */
	{ .compatible = "qcom,pcie-msm8996", .data = &cfg_2_3_2 }, /* [한국어] 2.3.2 세대(L0s 광고 없음) */
	{ .compatible = "qcom,pcie-qcs404", .data = &cfg_2_4_0 }, /* [한국어] 2.4.0 세대이되 리셋을 여섯만 쓴다 */
	{ .compatible = "qcom,pcie-sa8255p", .data = &cfg_fw_managed }, /* [한국어] **펌웨어 관리 갈래**. 이 표만 ops 가 없는 cfg 를 가리킨다 */
	{ .compatible = "qcom,pcie-sa8540p", .data = &cfg_sc8280xp }, /* [한국어] 1.21.0 세대(L0s 광고 없음) */
	{ .compatible = "qcom,pcie-sa8775p", .data = &cfg_1_34_0}, /* [한국어] 1.9.0 세대이되 캐시 스누핑을 강제한다 */
	{ .compatible = "qcom,pcie-sc7280", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sc8180x", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sc8280xp", .data = &cfg_sc8280xp }, /* [한국어] 1.21.0 세대 */
	{ .compatible = "qcom,pcie-sdm845", .data = &cfg_2_7_0 }, /* [한국어] 2.7.0 세대 */
	{ .compatible = "qcom,pcie-sdx55", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sm8150", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sm8250", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sm8350", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-sm8450-pcie0", .data = &cfg_1_9_0 }, /* [한국어] 같은 SoC 의 컨트롤러 0번 */
	{ .compatible = "qcom,pcie-sm8450-pcie1", .data = &cfg_1_9_0 }, /* [한국어] 그 1번. 컨트롤러마다 compatible 을 나눈 예다 */
	{ .compatible = "qcom,pcie-sm8550", .data = &cfg_1_9_0 }, /* [한국어] 1.9.0 세대 */
	{ .compatible = "qcom,pcie-x1e80100", .data = &cfg_sc8280xp }, /* [한국어] 가장 최신 SoC 도 1.21.0 세대를 쓴다 — 이 한 표가 십여 년을 잇는다 */
	{ } /* [한국어] 표의 끝을 알리는 빈 항목 */
};

/* [한국어]
 * qcom_fixup_class - RC 의 클래스 코드를 PCI-to-PCI 브리지로 고친다
 *
 * @dev: 방금 열거된 PCI 장치.
 *
 * PCI quirk 이며, 아래 DECLARE_PCI_FIXUP_EARLY 일곱 줄이 퀄컴 벤더 ID 와
 * 특정 장치 ID 조합에 이 함수를 건다.
 *
 * 이 컨트롤러들은 config 공간의 클래스 코드를 브리지가 아닌 값으로 보고한다.
 * 그대로 두면 PCI 코어가 루트 포트를 브리지로 인식하지 못해 그 아래 버스를
 * 열거하지 않는다. EARLY 단계에서 고치는 이유가 그것이다 — 코어가 그 값을
 * 보고 판단하기 전에 바꿔야 한다.
 *
 * 같은 문제를 rzg3s 나 brcmstb 는 초기화 중 DBI 에 직접 써서 해결하는데,
 * 여기서는 quirk 으로 처리한다. 컨트롤러가 여럿이고 세대마다 초기화 경로가
 * 달라, 열거 시점에 한 번 고치는 편이 단순하기 때문으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거 초기 단계).
 *
 * 호출 체인:  PCI 코어(pci_fixup_device, EARLY 단계) → [이 함수]
 */
static void qcom_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL; /* [한국어] 클래스 코드를 PCI-to-PCI 브리지로 덮어쓴다 */
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0101, qcom_fixup_class); /* [한국어] 0x0101 장치 ID 에 이 quirk 을 건다 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0104, qcom_fixup_class); /* [한국어] 0x0104 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0106, qcom_fixup_class); /* [한국어] 0x0106 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0107, qcom_fixup_class); /* [한국어] 0x0107 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0302, qcom_fixup_class); /* [한국어] 0x0302 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1000, qcom_fixup_class); /* [한국어] 0x1000 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1001, qcom_fixup_class); /* [한국어] 0x1001. 일곱 개의 루트 포트 장치 ID 가 같은 문제를 갖는다 */

/* [한국어] 이 드라이버의 절전 콜백 표.
 * NOIRQ 판을 쓰는 것이 요점이다 — 인터럽트가 이미 꺼진 뒤에 불려야
 * 컨트롤러를 끄는 도중 MSI 나 링크 이벤트가 들어오지 않는다. */
static const struct dev_pm_ops qcom_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(qcom_pcie_suspend_noirq, qcom_pcie_resume_noirq) /* [한국어] suspend/resume 을 noirq 단계에 건다 — 인터럽트가 꺼진 뒤에 컨트롤러를 다뤄야 하기 때문이다 */
};

/* [한국어] 플랫폼 드라이버 등록 정보.
 * remove 콜백이 없고 suppress_bind_attrs 가 켜져 있다 — 한 번 붙으면
 * 떼어지지 않는 것을 전제로 한다. EP 판(pcie-qcom-ep.c)은 remove 를 두는데,
 * 그쪽은 PERST# 로 언제든 꺼질 수 있는 구조라서다. */
static struct platform_driver qcom_pcie_driver = {
	.probe = qcom_pcie_probe, /* [한국어] DT 매칭이 성사되면 불린다. remove 콜백이 없다 */
	.driver = {
		.name = "qcom-pcie", /* [한국어] sysfs 등에 보일 드라이버 이름 */
		.suppress_bind_attrs = true, /* [한국어] 수동 bind/unbind 를 막는다 — remove 경로가 없는 것과 짝이 맞는다 */
		.of_match_table = qcom_pcie_match, /* [한국어] 위에서 정의한 compatible 매칭 표 */
		.pm = &qcom_pcie_pm_ops, /* [한국어] 위에서 정의한 절전 콜백 표 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS, /* [한국어] 부팅을 빠르게 하려고 비동기 probe 를 선호한다고 알린다 */
	},
};
builtin_platform_driver(qcom_pcie_driver); /* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다 */
