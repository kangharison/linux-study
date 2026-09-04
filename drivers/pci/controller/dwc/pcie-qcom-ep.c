// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm PCIe Endpoint controller driver
 *
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Author: Siddartha Mohanadoss <smohanad@codeaurora.org
 *
 * Copyright (c) 2021, Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org
 */

/*
 * [한국어 설명] Qualcomm PCIe 엔드포인트 컨트롤러 드라이버 (pcie-qcom-ep.c)
 *
 * === 파일의 역할 ===
 * 같은 퀄컴 PCIe IP 를 **엔드포인트(EP)** 로 모는 플랫폼 드라이버다.
 * RC 판(pcie-qcom.c)과 짝을 이루며, DWC 공통 코어를 감싸는 글루라는 성격도
 * 같다. 다만 감싸는 코어가 다르다 — 이 파일은
 * drivers/pci/controller/dwc/pcie-designware-ep.c 와 그 위의 EPC 코어
 * (drivers/pci/endpoint/pci-epc-core.c)에 붙는다.
 *
 * RC 판과 견주면 이 파일의 성격이 뚜렷해진다.
 *   - **세대 표가 없다.** RC 판이 IP 리비전마다 ops 표를 두는 것과 달리,
 *     여기에는 struct qcom_pcie_ep_cfg 의 불리언 넷(hdma_support /
 *     override_no_snoop / disable_mhi_ram_parity_check / firmware_managed)
 *     뿐이다. 지원 SoC 가 다섯 개뿐이고 모두 비슷한 세대이기 때문이다.
 *   - **상태 기계가 있다.** enum qcom_pcie_ep_link_status 의 네 상태
 *     (DISABLED / ENABLED / UP / DOWN)를 두 인터럽트가 옮긴다. RC 에는
 *     이런 것이 없는데, RC 는 자기가 링크를 세우지만 EP 는 호스트가
 *     언제 PERST# 를 풀고 언제 열거할지를 통보받는 쪽이기 때문이다.
 *   - **PERST# 를 출력이 아니라 입력으로 받는다.** RC 판은 PERST# 를
 *     GPIO 출력으로 걸고 푸는 데 반해, 여기서는 devm_gpiod_get(..., GPIOD_IN)
 *     으로 받아 그 선의 인터럽트를 건다. 호스트가 PERST# 를 놓으면
 *     qcom_pcie_perst_deassert() 가 컨트롤러 전체를 세우고, 걸면
 *     qcom_pcie_perst_assert() 가 자원을 모두 끈다. 즉 이 드라이버의
 *     기동 절차는 probe 가 아니라 **인터럽트 핸들러 안에** 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층은 EPF 드라이버 -> EPC 코어 -> DWC EP 코어 -> 이 파일 ->
 * PARF/DBI/ELBI MMIO 와 클록·리셋·PHY -> SoC 하드웨어 순이다.
 *
 *   platform_driver -> qcom_pcie_ep_probe()
 *     -> of_device_get_match_data()        SoC 표(선택)
 *     -> devm_pm_runtime_enable()
 *     -> qcom_pcie_ep_get_resources()      parf/dbi/mmio, PERST#/WAKE# GPIO,
 *                                          클록·core 리셋·PHY·icc
 *     -> dw_pcie_ep_init()                 (pcie-designware-ep.c:2981)
 *                                          EPC 를 만들어 EPF 가 붙을 수 있게 한다
 *     -> qcom_pcie_ep_enable_irq_resources()  global IRQ 와 PERST# IRQ 등록
 *     -> pm_runtime_put_sync()             링크가 설 때까지 잠들어 있는다
 *
 * 그다음은 인터럽트가 주도한다.
 *   [PERST# IRQ] qcom_pcie_ep_perst_irq_thread()
 *     PERST# 해제 -> qcom_pcie_perst_deassert() : 자원 기동 -> WAKE# 펄스로
 *       준비 완료 알림 -> PARF 프로그래밍 -> dw_pcie_ep_init_registers()
 *       -> 이퀄라이제이션/마진(공유 함수) -> pci_epc_init_notify()
 *       -> LTSSM 활성
 *     PERST# 인가 -> qcom_pcie_perst_assert() : 자원 정지, 상태 DISABLED
 *     처리 끝에 IRQ 트리거 극성을 뒤집어, 다음 반대 전이를 기다린다.
 *   [Global IRQ] qcom_pcie_ep_global_irq_thread()
 *     PARF_INT_ALL_STATUS 한 워드에서 링크다운 / BME / PM Turn-off /
 *     D-state 변경 / 링크업을 갈라 처리한다. 링크업이면 dw_pcie_ep_linkup()
 *     (pcie-designware-ep.c:2812), 링크다운이면 dw_pcie_ep_linkdown()
 *     (:2848), BME 이면 pci_epc_bus_master_enable_notify()
 *     (drivers/pci/endpoint/pci-epc-core.c:1475)로 위 계층에 알린다.
 *
 * EPF 가 이 파일을 되부르는 자리는 struct dw_pcie_ep_ops 의 두 개뿐이다.
 * 두 단 건너뛰기(two-hop) 구조라 경로가 이렇게 된다.
 *   EPF -> pci_epc_raise_irq()      (pci-epc-core.c:484)
 *       -> dw_pcie_ep_raise_irq()   (pcie-designware-ep.c:1774)
 *       -> qcom_pcie_ep_raise_irq()          <- 이 파일
 *   EPF -> pci_epc_get_features()   (pci-epc-core.c:381)
 *       -> dw_pcie_ep_get_features()(pcie-designware-ep.c:1882)
 *       -> qcom_pcie_epc_get_features()      <- 이 파일
 * struct dw_pcie_ep_ops 의 나머지 자리(pre_init / init / get_dbi_offset /
 * get_dbi2_offset)는 이 드라이버가 채우지 않아 DWC 코어의 기본 동작을 쓴다.
 *
 * 실행 컨텍스트: probe/remove 는 프로세스 컨텍스트다. 두 인터럽트는 모두
 * 스레드 핸들러(devm_request_threaded_irq 의 IRQF_ONESHOT)라 프로세스
 * 컨텍스트에서 돌며 잠들 수 있다 — PERST# 핸들러가 클록·PHY·regulator 를
 * 만지고 usleep 까지 하므로 그래야만 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: EPC 코어(drivers/pci/endpoint/pci-epc-core.c)와 그 위의 EPF 드라이버,
 *   그리고 DWC EP 코어(pcie-designware-ep.c)의 dw_pcie_ep_init() :2981,
 *   dw_pcie_ep_init_registers() :2629, dw_pcie_ep_cleanup() :2321,
 *   dw_pcie_ep_deinit() :2357.
 * 옆쪽: **pcie-qcom-common.c 를 RC 드라이버(pcie-qcom.c)와 공유한다.**
 *   공유하는 것은 정확히 두 함수 —  qcom_pcie_common_set_equalization() 과
 *   qcom_pcie_common_set_16gt_lane_margining() 이며, 둘 다 링크를 세우기
 *   직전에 같은 조건(Gen4 이면 마진까지)으로 불린다. 물리 계층 설정이라
 *   링크 방향과 무관해 그대로 나눌 수 있다. 반대로 PARF 레지스터는 두
 *   파일이 각자 #define 을 갖는다 — 같은 이름·같은 오프셋이라도 EP 는
 *   PARF_DEVICE_TYPE 에 EP 값을 쓰고 RC 는 RC 값을 쓰는 식으로 값과 순서가
 *   달라, 공유해서 얻을 것이 적기 때문이다.
 * 아래쪽: 클록, core 리셋, PHY(phy_set_mode_ext 로 EP 모드 지정), 인터커넥트,
 *   런타임 PM, GPIO(PERST# 입력 / WAKE# 출력), 그리고 TCSR 을 regmap 으로
 *   다루는 syscon(PERST 분리 설정).
 * 공유 상태: struct qcom_pcie_ep 하나가 컨트롤러 전체를 담으며, 그 첫 필드가
 *   struct dw_pcie 값이라 to_pcie_ep 매크로가 drvdata 로 되찾는다.
 *   전역 가변 상태는 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * qcom_pcie_ep_probe()               : 진입점. 자원 확보와 EPC 등록, IRQ 등록.
 * qcom_pcie_perst_deassert()         : 실질적 기동 절차 본체. PARF 전체를 세운다.
 * qcom_pcie_perst_assert()           : 그 역. 자원을 끄고 상태를 DISABLED 로.
 * qcom_pcie_ep_perst_irq_thread()    : PERST# 전이를 받아 위 둘을 부른다.
 * qcom_pcie_ep_global_irq_thread()   : 링크·BME·PM 이벤트를 갈라 위로 알린다.
 * qcom_pcie_ep_raise_irq()           : EPF 가 요청한 INTx/MSI 를 호스트로 올린다.
 * qcom_pcie_dw_write_dbi2()          : ELBI 게이트를 열고 DBI2 에 쓴다(EP 전용).
 * struct qcom_pcie_ep                : 컨트롤러 하나의 모든 상태.
 * struct qcom_pcie_ep_cfg            : SoC 차이를 담는 불리언 넷.
 * enum qcom_pcie_ep_link_status      : 이 파일에만 있는 링크 상태 기계.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 방향으로 봐도 접점이 없다 — 이 파일은 SoC 를 남의 버스에 붙는 장치로
 * 만드는 쪽이고, NVMe 호스트 드라이버는 자기 버스에 붙은 장치를 다루는
 * 쪽이다. 이 컨트롤러가 EP 로 노출하는 기능은 MHI(모뎀 계열 프로토콜)와
 * eDMA 이며, NVMe 함수를 노출하는 코드는 이 트리에 없다.
 */

#include <linux/clk.h> /* [한국어] clk_bulk_ 계열 — PERST# 해제 때 클록을 통째로 켜고 끈다 */
#include <linux/debugfs.h> /* [한국어] debugfs_create_dir 와 devm_seqfile — 링크 전이 카운터 노출 */
#include <linux/delay.h> /* [한국어] usleep_range — core 리셋과 WAKE# 펄스의 대기 */
#include <linux/gpio/consumer.h> /* [한국어] gpiod_ 계열 — PERST# 를 입력으로 읽고 WAKE# 를 출력으로 구동한다 */
#include <linux/interconnect.h> /* [한국어] icc_set_bw — 인터커넥트 대역폭 요구 */
#include <linux/mfd/syscon.h> /* [한국어] syscon_node_to_regmap — TCSR 을 regmap 으로 연다 */
#include <linux/phy/pcie.h> /* [한국어] PHY_MODE_PCIE_EP — PHY 를 엔드포인트 모드로 지정할 때 쓴다 */
#include <linux/phy/phy.h> /* [한국어] phy_init/phy_power_on/phy_set_mode_ext */
#include <linux/platform_device.h> /* [한국어] 플랫폼 디바이스와 자원·인터럽트 조회 */
#include <linux/pm_domain.h> /* [한국어] 전원 도메인 관련 정의. pm_runtime 경로가 기대하는 헤더다 */
#include <linux/regmap.h> /* [한국어] regmap_write — TCSR 의 PERST 분리 설정을 끈다 */
#include <linux/reset.h> /* [한국어] reset_control_ 계열 — core 리셋 조작 */
#include <linux/module.h> /* [한국어] MODULE_ 계열 매크로. 파일 끝의 작성자·설명·라이선스 선언에 쓴다 */

#include "../../pci.h" /* [한국어] PCI 서브시스템 내부 헤더. pcie_get_link_speed() 등 코어 비공개 도우미 */
#include "pcie-designware.h" /* [한국어] DWC 코어의 struct dw_pcie / dw_pcie_ep 와 dw_pcie_ep_ops 정의 */
#include "pcie-qcom-common.h" /* [한국어] RC 와 공유하는 두 함수의 선언(이퀄라이제이션, Gen4 레인 마진) */

/* PARF registers */
#define PARF_SYS_CTRL				0x00 /* [한국어] 시스템 제어. AUX 전원 감지·클록 게이팅·DBI 웨이크업 차단 비트가 모여 있다 */
#define PARF_DB_CTRL				0x10 /* [한국어] 디바운서 제어. 삽입/제거/웨이크업 디바운서를 막는 비트들이다 */
#define PARF_PM_CTRL				0x20 /* [한국어] 전력 관리 제어. L1 탈출 요청, L23 준비 완료, L1 진입 금지 비트가 있다 */
#define PARF_MHI_CLOCK_RESET_CTRL		0x174 /* [한국어] MHI 클록/리셋 제어. 마스터 AXI 클록 활성 비트가 있다 */
#define PARF_MHI_BASE_ADDR_LOWER		0x178 /* [한국어] BAR 로 노출할 MMIO 영역의 물리 주소 하위 워드 */
#define PARF_MHI_BASE_ADDR_UPPER		0x17c /* [한국어] 그 상위 워드. 이 드라이버는 32비트 주소만 쓰므로 0 을 적는다 */
#define PARF_DEBUG_INT_EN			0x190 /* [한국어] 디버그 인터럽트 활성. D-state 변경·버스 마스터·PM Turn-off 이벤트의 출처다 */
#define PARF_AXI_MSTR_RD_HALT_NO_WRITES		0x1a4 /* [한국어] 읽기가 쓰기를 막지 않게 하는 제어 */
#define PARF_AXI_MSTR_WR_ADDR_HALT		0x1a8 /* [한국어] 쓰기 후 쓰기 정지 제어 */
#define PARF_Q2A_FLUSH				0x1ac /* [한국어] Q2A 플러시 제어 */
#define PARF_LTSSM				0x1b0 /* [한국어] 링크 학습 활성 레지스터. RC 판은 같은 비트를 LTSSM_EN 상수로 쓰는데 이 파일은 BIT(8) 로 직접 쓴다 */
#define PARF_CFG_BITS				0x210 /* [한국어] 설정 비트 묶음. MSI/LTR 에서 L1SS 를 빠져나오게 하는 비트가 있다 */
#define PARF_INT_ALL_STATUS			0x224 /* [한국어] 모든 이벤트의 상태가 한 워드에 모이는 레지스터. global IRQ 핸들러가 이것을 읽어 갈라 처리한다 */
#define PARF_INT_ALL_CLEAR			0x228 /* [한국어] 그 상태를 지우는 레지스터(R/W1C) */
#define PARF_INT_ALL_MASK			0x22c /* [한국어] 그중 어떤 이벤트를 받을지 고르는 마스크 */
#define PARF_SLV_ADDR_MSB_CTRL			0x2c0 /* [한국어] 슬레이브 주소 상위 비트 제어. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_DBI_BASE_ADDR			0x350 /* [한국어] DBI 물리 주소 하위 워드. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 — RC 판은 같은 이름의 레지스터를 실제로 프로그래밍한다 */
#define PARF_DBI_BASE_ADDR_HI			0x354 /* [한국어] 그 상위 워드. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_SLV_ADDR_SPACE_SIZE		0x358 /* [한국어] 슬레이브 주소 공간 크기. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_SLV_ADDR_SPACE_SIZE_HI		0x35c /* [한국어] 그 상위 워드. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_NO_SNOOP_OVERRIDE			0x3d4 /* [한국어] TLP 의 NO_SNOOP 을 무시하게 하는 레지스터. cfg->override_no_snoop 인 SoC 만 쓴다 */
#define PARF_ATU_BASE_ADDR			0x634 /* [한국어] iATU 물리 주소 하위 워드. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_ATU_BASE_ADDR_HI			0x638 /* [한국어] 그 상위 워드. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_SRIS_MODE				0x644 /* [한국어] SRIS(분리 기준 클록) 모드 설정. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L2		0xc04 /* [한국어] L2 전이 횟수 카운터. debugfs 가 읽는다 */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L1		0xc0c /* [한국어] L1 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L0S		0xc10 /* [한국어] L0s 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1	0xc84 /* [한국어] L1.1 전이 횟수 카운터 */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2	0xc88 /* [한국어] L1.2 전이 횟수 카운터 */
#define PARF_DEVICE_TYPE			0x1000 /* [한국어] 컨트롤러를 RC 로 둘지 EP 로 둘지 정하는 레지스터. RC 판은 같은 자리에 RC 값을 쓴다 */
#define PARF_BDF_TO_SID_CFG			0x2c00 /* [한국어] BDF→SID 변환의 우회 설정. RC 판은 우회를 끄고 표를 채우는데, EP 는 반대로 우회를 켠다 */
#define PARF_INT_ALL_5_MASK			0x2dcc /* [한국어] 인터럽트 마스크 5번 묶음. MHI RAM 패리티 오류 비트가 여기 있다 */
#define PARF_INT_ALL_3_MASK			0x2e18 /* [한국어] 인터럽트 마스크 3번 묶음. PTM 갱신 비트가 여기 있다 */

/* PARF_INT_ALL_{STATUS/CLEAR/MASK} register fields */
#define PARF_INT_ALL_LINK_DOWN			BIT(1) /* [한국어] 링크가 끊겼다 */
#define PARF_INT_ALL_BME			BIT(2) /* [한국어] 호스트가 버스 마스터를 켰다 — 열거가 끝나 DMA 를 시작해도 된다는 뜻이다 */
#define PARF_INT_ALL_PM_TURNOFF			BIT(3) /* [한국어] 호스트가 링크를 재우려 한다 */
#define PARF_INT_ALL_DEBUG			BIT(4) /* [한국어] 디버그 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_LTR			BIT(5) /* [한국어] LTR 메시지. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_MHI_Q6			BIT(6) /* [한국어] MHI Q6 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_MHI_A7			BIT(7) /* [한국어] MHI A7 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_DSTATE_CHANGE		BIT(8) /* [한국어] 호스트가 이 장치의 D-state 를 바꿨다 */
#define PARF_INT_ALL_L1SUB_TIMEOUT		BIT(9) /* [한국어] L1 서브스테이트 타임아웃. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_MMIO_WRITE			BIT(10) /* [한국어] MMIO 쓰기 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_CFG_WRITE			BIT(11) /* [한국어] config 쓰기 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_BRIDGE_FLUSH_N		BIT(12) /* [한국어] 브리지 플러시 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_LINK_UP			BIT(13) /* [한국어] 링크가 올라왔다 — 열거가 시작될 수 있다 */
#define PARF_INT_ALL_AER_LEGACY			BIT(14) /* [한국어] legacy AER 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_PLS_ERR			BIT(15) /* [한국어] 물리 계층 오류. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_PME_LEGACY			BIT(16) /* [한국어] legacy PME 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_PLS_PME			BIT(17) /* [한국어] 물리 계층 PME 이벤트. [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define PARF_INT_ALL_EDMA			BIT(22) /* [한국어] eDMA 이벤트. 마스크에는 넣지만 핸들러가 따로 다루지 않아 unknown 경고로 간다 */

/* PARF_BDF_TO_SID_CFG register fields */
#define PARF_BDF_TO_SID_BYPASS			BIT(0) /* [한국어] 변환 우회 비트. PERST# 해제 때 이것을 세워 변환을 끈다 */

/* PARF_DEBUG_INT_EN register fields */
#define PARF_DEBUG_INT_PM_DSTATE_CHANGE		BIT(1) /* [한국어] D-state 변경 이벤트를 올리게 한다 */
#define PARF_DEBUG_INT_CFG_BUS_MASTER_EN	BIT(2) /* [한국어] 버스 마스터 활성 이벤트를 올리게 한다 */
#define PARF_DEBUG_INT_RADM_PM_TURNOFF		BIT(3) /* [한국어] PM Turn-off 이벤트를 올리게 한다. 이 셋이 위 INT_ALL 세 비트의 출처다 */

/* PARF_NO_SNOOP_OVERRIDE register fields */
#define WR_NO_SNOOP_OVERRIDE_EN			BIT(1) /* [한국어] 쓰기 TLP 의 NO_SNOOP 무시 */
#define RD_NO_SNOOP_OVERRIDE_EN			BIT(3) /* [한국어] 읽기 TLP 의 NO_SNOOP 무시 */

/* PARF_DEVICE_TYPE register fields */
#define PARF_DEVICE_TYPE_EP			0x0 /* [한국어] PARF_DEVICE_TYPE 에 넣을 EP 값. RC 판의 DEVICE_TYPE_RC(0x4) 와 짝을 이룬다 */

/* PARF_PM_CTRL register fields */
#define PARF_PM_CTRL_REQ_EXIT_L1		BIT(1) /* [한국어] L1 에서 빠져나오라는 요청. 호스트가 D3 로 보낼 때 세운다 */
#define PARF_PM_CTRL_READY_ENTR_L23		BIT(2) /* [한국어] L23 으로 들어갈 준비가 됐다는 응답. PM Turn-off 를 받았을 때 세운다 */
#define PARF_PM_CTRL_REQ_NOT_ENTR_L1		BIT(5) /* [한국어] L1 진입 금지. PERST# 해제 때 지워 L1 을 허용한다 */

/* PARF_MHI_CLOCK_RESET_CTRL fields */
#define PARF_MSTR_AXI_CLK_EN			BIT(1) /* [한국어] 마스터 AXI 클록 활성. L1SS 동안 게이트하려고 지운다 */

/* PARF_AXI_MSTR_RD_HALT_NO_WRITES register fields */
#define PARF_AXI_MSTR_RD_HALT_NO_WRITE_EN	BIT(0) /* [한국어] 읽기가 쓰기를 막지 않게 하는 비트. 지운다 */

/* PARF_AXI_MSTR_WR_ADDR_HALT register fields */
#define PARF_AXI_MSTR_WR_ADDR_HALT_EN		BIT(31) /* [한국어] 쓰기 후 쓰기 정지 비트. 세운다 */

/* PARF_Q2A_FLUSH register fields */
#define PARF_Q2A_FLUSH_EN			BIT(16) /* [한국어] Q2A 플러시 활성 비트. 지운다 */

/* PARF_SYS_CTRL register fields */
#define PARF_SYS_CTRL_AUX_PWR_DET		BIT(4) /* [한국어] 호스트에 AUX 전원이 있다고 보고 */
#define PARF_SYS_CTRL_CORE_CLK_CGC_DIS		BIT(6) /* [한국어] 코어 클록 게이팅 금지 */
#define PARF_SYS_CTRL_MSTR_ACLK_CGC_DIS		BIT(10) /* [한국어] 마스터 AXI 클록 게이팅 금지. 이 비트만 지우고 나머지 셋은 세운다 */
#define PARF_SYS_CTRL_SLV_DBI_WAKE_DISABLE	BIT(11) /* [한국어] DBI 접근이 코어를 L1 에서 깨우지 못하게 한다 */

/* PARF_DB_CTRL register fields */
#define PARF_DB_CTRL_INSR_DBNCR_BLOCK		BIT(0) /* [한국어] 삽입 디바운서 차단 */
#define PARF_DB_CTRL_RMVL_DBNCR_BLOCK		BIT(1) /* [한국어] 제거 디바운서 차단 */
#define PARF_DB_CTRL_DBI_WKP_BLOCK		BIT(4) /* [한국어] DBI 웨이크업 차단 */
#define PARF_DB_CTRL_SLV_WKP_BLOCK		BIT(5) /* [한국어] 슬레이브 웨이크업 차단 */
#define PARF_DB_CTRL_MST_WKP_BLOCK		BIT(6) /* [한국어] 마스터 웨이크업 차단. 다섯을 한 번에 세워 모든 디바운서를 끈다 */

/* PARF_CFG_BITS register fields */
#define PARF_CFG_BITS_REQ_EXIT_L1SS_MSI_LTR_EN	BIT(1) /* [한국어] MSI 와 LTR 메시지에서 L1SS 를 빠져나오게 한다 */

/* PARF_INT_ALL_5_MASK fields */
#define PARF_INT_ALL_5_MHI_RAM_DATA_PARITY_ERR	BIT(0) /* [한국어] MHI RAM 데이터 패리티 오류. cfg 가 요구하면 이 인터럽트를 끈다 */

/* PARF_INT_ALL_3_MASK fields */
#define PARF_INT_ALL_3_PTM_UPDATING		BIT(4) /* [한국어] PTM 갱신 인터럽트. 늘 끈다 */

/* ELBI registers */
#define ELBI_SYS_STTS				0x08 /* [한국어] ELBI 상태 레지스터. 링크업 판정에 쓴다 */
#define ELBI_CS2_ENABLE				0xa4 /* [한국어] DBI2 게이트 제어. 1 을 쓰면 이후 DBI 접근이 DBI2 로 간다 */

/* DBI registers */
#define DBI_CON_STATUS				0x44 /* [한국어] config 공간의 전력 상태 레지스터. D-state 를 여기서 읽는다 */

/* DBI register fields */
#define DBI_CON_STATUS_POWER_STATE_MASK		GENMASK(1, 0) /* [한국어] 그중 전력 상태 필드(D0~D3) */

#define XMLH_LINK_UP				0x400 /* [한국어] ELBI 상태의 링크업 비트. RC 판이 표준 LNKSTA 를 읽는 것과 대비된다 */
#define CORE_RESET_TIME_US_MIN			1000 /* [한국어] core 리셋 인가/해제 뒤 대기의 하한(us) */
#define CORE_RESET_TIME_US_MAX			1005 /* [한국어] 그 상한. 근거는 이 트리에서 확인 못 함 */
#define WAKE_DELAY_US				2000 /* [한국어] WAKE# 펄스 길이(us). 상류가 2ms 라고 적어 두었다 */

/* [한국어] 링크 속도 코드를 인터커넥트 대역폭 값으로 바꾸는 매크로.
 * RC 판(pcie-qcom.c)에도 같은 이름·같은 정의가 따로 있다 — 두 파일이
 * PARF 정의와 마찬가지로 이 매크로도 공유하지 않는다. */
#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
			Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed)))

/* [한국어] dw_pcie 포인터에서 이 파일의 EP 상태를 되찾는 매크로.
 * DWC 코어의 콜백들이 struct dw_pcie 만 넘겨 주므로 그 dev 의 drvdata 로
 * 거슬러 올라간다. probe 의 platform_set_drvdata(pdev, pcie_ep) 와 짝을 이룬다.
 * struct qcom_pcie_ep 의 첫 필드가 dw_pcie 값이라 container_of 로도 되겠지만,
 * 상류는 drvdata 방식을 쓴다. */
#define to_pcie_ep(x)				dev_get_drvdata((x)->dev)

/* [한국어] **RC 판에는 없는 링크 상태 기계.**
 * RC 는 자기가 링크를 세우므로 상태를 따로 추적할 필요가 없지만, EP 는
 * 호스트가 언제 PERST# 를 풀고 언제 열거할지를 통보받는 쪽이라 지금 어느
 * 단계인지를 기억해야 한다.
 *
 * 상태를 옮기는 것은 두 인터럽트다.
 *   PERST# 인가        → DISABLED (qcom_pcie_perst_assert)
 *   BME 이벤트         → ENABLED  (호스트가 열거를 마치고 DMA 를 허용)
 *   링크업 이벤트      → UP
 *   링크다운 이벤트    → DOWN
 *
 * [관찰] 실제로 이 값을 **읽는** 곳은 qcom_pcie_ep_remove() 한 곳뿐이다.
 * 나머지는 모두 쓰기만 한다. 함수 위의 상류 TODO 주석("클라이언트에 PCIe
 * 상태 변화를 알릴 것")이 그 이유를 짐작하게 한다. */
enum qcom_pcie_ep_link_status {
	QCOM_PCIE_EP_LINK_DISABLED, /* [한국어] PERST# 가 걸려 있거나 아직 한 번도 풀리지 않은 상태. 0 이라 구조체를 0 으로 잡으면 자동으로 이 값이다 */
	QCOM_PCIE_EP_LINK_ENABLED, /* [한국어] 호스트가 버스 마스터를 켠 상태 */
	QCOM_PCIE_EP_LINK_UP, /* [한국어] 링크가 올라온 상태 */
	QCOM_PCIE_EP_LINK_DOWN, /* [한국어] 링크가 끊긴 상태 */
};

/**
 * struct qcom_pcie_ep_cfg - Per SoC config struct
 * @hdma_support: HDMA support on this SoC
 * @override_no_snoop: Override NO_SNOOP attribute in TLP to enable cache snooping
 * @disable_mhi_ram_parity_check: Disable MHI RAM data parity error check
 * @firmware_managed: Set if the controller is firmware managed
 */
struct qcom_pcie_ep_cfg {
	bool hdma_support; /* [한국어] 이 SoC 가 HDMA 를 지원하는지(상류 주석 @hdma_support). probe 가 이 값으로 eDMA 를 native HDMA 모드로 바꾼다 */
	bool override_no_snoop; /* [한국어] TLP 의 NO_SNOOP 을 무시해 캐시 스누핑을 강제할지(상류 주석 @override_no_snoop). PERST# 해제의 마지막에 반영된다 */
	bool disable_mhi_ram_parity_check; /* [한국어] MHI RAM 데이터 패리티 오류 검사를 끌지(상류 주석 @disable_mhi_ram_parity_check) */
	bool firmware_managed; /* [한국어] 컨트롤러를 펌웨어가 관리하는지(상류 주석 @firmware_managed). 참이면 클록·리셋·PHY 를 잡지도 켜지도 않는다 */
};

/**
 * struct qcom_pcie_ep - Qualcomm PCIe Endpoint Controller
 * @pci: Designware PCIe controller struct
 * @parf: Qualcomm PCIe specific PARF register base
 * @mmio: MMIO register base
 * @perst_map: PERST regmap
 * @mmio_res: MMIO region resource
 * @core_reset: PCIe Endpoint core reset
 * @reset: PERST# GPIO
 * @wake: WAKE# GPIO
 * @phy: PHY controller block
 * @debugfs: PCIe Endpoint Debugfs directory
 * @icc_mem: Handle to an interconnect path between PCIe and MEM
 * @clks: PCIe clocks
 * @num_clks: PCIe clocks count
 * @perst_en: Flag for PERST enable
 * @perst_sep_en: Flag for PERST separation enable
 * @cfg: PCIe EP config struct
 * @link_status: PCIe Link status
 * @global_irq: Qualcomm PCIe specific Global IRQ
 * @perst_irq: PERST# IRQ
 */
/* [한국어] 컨트롤러 하나의 모든 상태.
 * 첫 필드가 struct dw_pcie **값**이라는 점이 RC 판과 다르다 — RC 는 그것을
 * 포인터로 따로 잡는다. EP 쪽 DWC 코어가 dw_pcie 를 감싸는 구조를 전제로
 * 하기 때문이다.
 * devm 으로 잡아 platform_set_drvdata 로 걸어 두며, to_pcie_ep 매크로가
 * 되찾는다. 전역 가변 상태는 없다. */
struct qcom_pcie_ep {
	struct dw_pcie pci; /* [한국어] DWC 코어의 컨트롤러 구조체(상류 주석 @pci). 포인터가 아니라 값으로 박혀 있다. probe 가 dev 와 두 ops 표를 여기 채운다 */

	void __iomem *parf; /* [한국어] 퀄컴 고유 제어 레지스터 창(상류 주석 @parf). 이 파일의 거의 모든 writel_relaxed 가 여기를 향한다. 설정자는 get_io_resources, NULL 불가 */
	void __iomem *mmio; /* [한국어] BAR 로 노출할 MMIO 창(상류 주석 @mmio). debugfs 카운터도 이 창에서 읽는다 — RC 판이 별도 mhi 창을 쓰는 것과 다르다 */
	struct regmap *perst_map; /* [한국어] TCSR 을 다룰 regmap(상류 주석 @perst_map). DT 에 "qcom,perst-regs" 가 없으면 NULL 이고, qcom_pcie_ep_configure_tcsr() 이 그때 아무 일도 하지 않는다 */
		/* [한국어] MMIO 영역의 자원 서술자(상류 주석 @mmio_res).
		 * 매핑된 주소가 아니라 struct resource 를 통째로 들고 있는 이유가 있다 —
		 * qcom_pcie_perst_deassert() 가 그 **물리 주소**(res->start)를 MHI BASE
		 * 레지스터에 적어야 하기 때문이다. 상류 주석대로 그 영역이 BAR 로
		 * 노출되므로 호스트 쪽에서 볼 주소를 컨트롤러에 알려 주어야 한다.
		 * 설정자: qcom_pcie_ep_get_io_resources(). NULL 이면 probe 가 -EINVAL.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	struct resource *mmio_res;

	struct reset_control *core_reset; /* [한국어] 컨트롤러 코어 리셋(상류 주석 @core_reset). 이름이 "core" 인 하나뿐이라, RC 판이 세대마다 개수를 달리하는 것과 대비된다. firmware_managed 구성에서는 잡지 않는다 */
		/* [한국어] PERST# GPIO(상류 주석 @reset). **입력이다.**
		 * devm_gpiod_get(..., GPIOD_IN) 으로 잡으며, 이 선을 구동하는 것은
		 * 호스트다. RC 판(pcie-qcom.c)이 같은 신호를 GPIOD_OUT_HIGH 로 잡아
		 * 직접 거는 것과 정반대이며, 이 한 줄이 RC/EP 의 역할 차이를 그대로
		 * 보여 준다.
		 * 읽는 자: qcom_pcie_ep_perst_irq_thread() 의 gpiod_get_value(), 그리고
		 *          gpiod_to_irq() 로 인터럽트 번호를 얻는 곳.
		 * 값 범위: 필수라 없으면 probe 가 실패한다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	struct gpio_desc *reset;
	struct gpio_desc *wake; /* [한국어] WAKE# GPIO(상류 주석 @wake). 출력이며, 준비가 끝났음을 호스트에 알리는 펄스에 쓴다. 선택이라 없으면 NULL 이고 GPIO API 가 무해하게 처리한다 */
	struct phy *phy; /* [한국어] PCIe PHY(상류 주석 @phy). qcom_pcie_enable_resources() 가 EP 모드로 지정해 켠다. optional 조회이며 firmware_managed 구성에서는 잡지 않는다 */
	struct dentry *debugfs; /* [한국어] 이 컨트롤러의 debugfs 디렉터리(상류 주석 @debugfs). probe 가 만들고 remove 가 재귀로 지운다 — RC 판이 지우지 않는 것과 다르다 */

	struct icc_path *icc_mem; /* [한국어] PCIe ↔ 메모리 인터커넥트 경로(상류 주석 @icc_mem). enable_resources 가 초기 대역폭을, icc_update 가 실제 대역폭을 요구한다. RC 판과 달리 OPP 갈래가 없다 */

	struct clk_bulk_data *clks; /* [한국어] PCIe 클록 배열(상류 주석 @clks). 이름을 따지지 않고 DT 가 준 것을 통째로 받는다 */
	int num_clks; /* [한국어] 그 개수(상류 주석 @num_clks). 음수면 조회 실패라 probe 가 그 자리에서 돌아간다 */

	u32 perst_en; /* [한국어] TCSR 안의 PERST 활성 레지스터 오프셋(상류 주석 @perst_en). DT 의 "qcom,perst-regs" 배열 인덱스 1 에서 읽는다 */
	u32 perst_sep_en; /* [한국어] 같은 배열 인덱스 2 의 PERST 분리 활성 오프셋(상류 주석 @perst_sep_en). 둘 다 configure_tcsr 이 0 을 써서 자동 리셋 연동을 끊는다 */

	const struct qcom_pcie_ep_cfg *cfg; /* [한국어] 이 SoC 의 설정 표(상류 주석 @cfg). NULL 일 수 있어 이 파일의 모든 참조가 `cfg && cfg->...` 형태로 확인한다 — 매칭 표의 세 항목이 data 를 주지 않기 때문이다 */
	enum qcom_pcie_ep_link_status link_status; /* [한국어] 현재 링크 상태(상류 주석 @link_status). 두 인터럽트가 옮기며, 실제로 읽는 곳은 remove 하나뿐이다 */
	int global_irq; /* [한국어] PARF 이벤트를 물어 오는 인터럽트 번호(상류 주석 @global_irq). DT 에서 "global" 이름으로 얻는다 */
	int perst_irq; /* [한국어] PERST# 선의 인터럽트 번호(상류 주석 @perst_irq). GPIO 에서 유도하며, IRQ_NOAUTOEN 으로 등록해 start_link 가 켤 때까지 닫혀 있다 */
};

/* [한국어]
 * qcom_pcie_ep_core_reset - 컨트롤러 코어 리셋을 한 번 순환시킨다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 리셋 프레임워크 오류.
 *
 * 걸고 → 기다리고 → 풀고 → 기다린다. 두 지연 모두 CORE_RESET_TIME_US_MIN/MAX
 * 로 1000~1005us 이며, 그 값의 근거는 이 트리에서 확인 못 함.
 *
 * RC 판(pcie-qcom.c)이 세대마다 리셋 개수와 순서를 달리하는 것과 달리, 여기서는
 * "core" 리셋 하나만 다룬다. 지원 SoC 가 다섯 개뿐이고 모두 비슷한 세대라
 * 세대 표 자체가 없기 때문이다.
 *
 * 두 번째 지연이 리셋 해제 뒤에 있는 것에 주의 — 리셋이 풀린 뒤 하드웨어가
 * 안정될 시간을 준다. 완료를 볼 상태 비트가 없어 시간으로 갈음하는 방식이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PERST# 해제 스레드). usleep_range 로 잠든다.
 *
 * 호출 체인:  qcom_pcie_enable_resources() → [이 함수] → reset_control_assert()
 */
static int qcom_pcie_ep_core_reset(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] 구조체 첫 필드인 DWC 구조체의 주소를 얻는다 — 포인터가 아니라 값이라 & 를 쓴다 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = reset_control_assert(pcie_ep->core_reset); /* [한국어] 코어 리셋을 건다 */
	if (ret) { /* [한국어] 못 걸었으면 */
		dev_err(dev, "Cannot assert core reset\n"); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 올린다 */
	}

	usleep_range(CORE_RESET_TIME_US_MIN, CORE_RESET_TIME_US_MAX); /* [한국어] 리셋이 전파될 시간을 준다 */

	ret = reset_control_deassert(pcie_ep->core_reset); /* [한국어] 리셋을 푼다 */
	if (ret) { /* [한국어] 못 풀었으면 */
		dev_err(dev, "Cannot de-assert core reset\n"); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 올린다 */
	}

	usleep_range(CORE_RESET_TIME_US_MIN, CORE_RESET_TIME_US_MAX); /* [한국어] 해제가 반영될 시간을 다시 준다. 완료를 볼 상태 비트가 없어 시간으로 갈음한다 */

	return 0; /* [한국어] 코어 리셋 순환 완료 */
}

/*
 * Delatch PERST_EN and PERST_SEPARATION_ENABLE with TCSR to avoid
 * device reset during host reboot and hibernation. The driver is
 * expected to handle this situation.
 */
/* [한국어]
 * qcom_pcie_ep_configure_tcsr - PERST 자동 리셋 연동을 TCSR 에서 끊는다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 *
 * 바로 위 상류 주석이 목적을 밝힌다 — TCSR 의 PERST_EN 과
 * PERST_SEPARATION_ENABLE 을 풀어(delatch), 호스트가 재부팅하거나 하이버네이션
 * 할 때 장치가 하드웨어적으로 리셋되는 것을 막는다. 그 상황은 드라이버가
 * 직접 처리하도록 남겨 두겠다는 뜻이며, 실제로
 * qcom_pcie_ep_perst_irq_thread() 가 PERST# 전이를 받아 처리한다.
 *
 * TCSR 은 컨트롤러 바깥의 SoC 전역 레지스터 블록이라, 자기 MMIO 창이 아니라
 * syscon regmap 으로 접근한다. 두 오프셋은 DT 의 "qcom,perst-regs" 배열에서
 * qcom_pcie_ep_get_io_resources() 가 읽어 둔 값이다.
 *
 * regmap 이 없으면(그 DT 속성이 없는 보드) 아무 일도 하지 않는다. 그래서
 * 호출자가 보드 구성을 가릴 필요가 없다.
 *
 * RC 판(pcie-qcom.c)에는 대응하는 것이 없다 — RC 는 PERST# 를 받는 쪽이
 * 아니라 내보내는 쪽이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PERST# 해제 스레드). regmap 접근이
 * 잠들 수 있다.
 *
 * 호출 체인:  qcom_pcie_perst_deassert() → [이 함수] → regmap_write()
 */
static void qcom_pcie_ep_configure_tcsr(struct qcom_pcie_ep *pcie_ep)
{
	if (pcie_ep->perst_map) { /* [한국어] DT 가 TCSR 을 주지 않은 보드에서는 아무 일도 하지 않는다 */
		regmap_write(pcie_ep->perst_map, pcie_ep->perst_en, 0); /* [한국어] PERST 활성 연동을 끈다 */
		regmap_write(pcie_ep->perst_map, pcie_ep->perst_sep_en, 0); /* [한국어] PERST 분리 활성 연동도 끈다. 둘을 끊어야 호스트 재부팅 때 장치가 하드웨어적으로 리셋되지 않는다 */
	}
}

/* [한국어]
 * qcom_pcie_dw_link_up - ELBI 상태 레지스터로 링크가 섰는지 본다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 * @return: 링크가 올라왔으면 참.
 *
 * struct dw_pcie_ops 의 link_up 으로 등록되어 DWC 코어가 부른다.
 *
 * RC 판(pcie-qcom.c 의 qcom_pcie_link_up)과 대조적이다. RC 는 표준 config
 * 공간의 링크 상태 레지스터에서 DLLLA 비트를 읽는데, EP 는 ELBI 블록의
 * 벤더 전용 비트(XMLH_LINK_UP)를 읽는다. EP 의 config 공간은 호스트가
 * 읽고 쓰는 대상이라 자기 링크 상태를 그 경로로 알기 어렵기 때문으로
 * 보이나, 근거 문서는 이 트리에서 확인 못 함.
 *
 * readl_relaxed 를 쓴다. 다른 레지스터 접근과의 순서 제약이 없기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음. 레지스터 읽기 하나다.
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.link_up → [이 함수]
 */
static bool qcom_pcie_dw_link_up(struct dw_pcie *pci)
{
	u32 reg; /* [한국어] ELBI 상태 워드 */

	reg = readl_relaxed(pci->elbi_base + ELBI_SYS_STTS); /* [한국어] 벤더 전용 상태 레지스터를 읽는다. RC 판은 같은 목적에 표준 config 공간의 LNKSTA 를 쓴다 */

	return reg & XMLH_LINK_UP; /* [한국어] 링크업 비트를 그대로 돌려준다 */
}

/* [한국어]
 * qcom_pcie_dw_start_link - PERST# 인터럽트를 켜서 호스트의 신호를 기다린다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 * @return: 늘 0.
 *
 * 이 함수 하나가 RC 와 EP 의 근본적 차이를 보여 준다. RC 판의 start_link 는
 * LTSSM 을 직접 켜서 링크를 세우지만, EP 는 그럴 수 없다 — 링크를 언제
 * 세울지는 호스트가 PERST# 로 정한다.
 *
 * 그래서 여기서 하는 일은 **PERST# 인터럽트를 활성화하는 것뿐**이다. 그
 * 인터럽트가 오면 qcom_pcie_ep_perst_irq_thread() 가 실제 기동 절차를
 * 수행한다. 즉 이 드라이버의 "링크 시작" 은 신호를 기다리기 시작하는 것이다.
 *
 * 그 인터럽트는 등록 시 IRQ_NOAUTOEN 으로 자동 활성을 막아 두었으므로,
 * 이 호출이 있어야 비로소 열린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.start_link → [이 함수] → enable_irq()
 */
static int qcom_pcie_dw_start_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* [한국어] DWC 구조체에서 이 파일의 상태를 되찾는다 */

	enable_irq(pcie_ep->perst_irq); /* [한국어] PERST# 인터럽트를 연다 — 이것이 EP 의 "링크 시작" 이다. RC 는 같은 자리에서 LTSSM 을 직접 켠다 */

	return 0; /* [한국어] DWC 코어는 반환값으로 링크 성공을 판정하지 않는다 */
}

/* [한국어]
 * qcom_pcie_dw_stop_link - PERST# 인터럽트를 꺼서 신호를 무시한다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 *
 * start_link 의 짝이다. 인터럽트를 끄면 호스트가 PERST# 를 놓아도 이
 * 드라이버가 반응하지 않는다.
 *
 * RC 판(pcie-qcom.c)에는 stop_link 구현이 아예 없다는 점이 대비된다.
 * RC 는 링크를 끊을 일이 있으면 PERST# 를 직접 걸면 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  DWC 코어 → dw_pcie_ops.stop_link → [이 함수] → disable_irq()
 */
static void qcom_pcie_dw_stop_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* [한국어] DWC 구조체에서 이 파일의 상태를 되찾는다 */

	disable_irq(pcie_ep->perst_irq); /* [한국어] PERST# 인터럽트를 닫는다. 이제 호스트가 신호를 바꿔도 반응하지 않는다 */
}

/* [한국어]
 * qcom_pcie_dw_write_dbi2 - ELBI 게이트를 열고 DBI2 영역에 쓴다
 *
 * @pci:  DWC 코어의 컨트롤러 구조체.
 * @base: 쓰기 기준 주소(코어가 넘기지만 이 구현은 pci->dbi_base2 를 쓴다).
 * @reg:  레지스터 오프셋.
 * @size: 쓰기 폭.
 * @val:  쓸 값.
 *
 * struct dw_pcie_ops 의 write_dbi2 로 등록된다. **EP 에만 있는 콜백**이며
 * RC 판에는 없다.
 *
 * DBI2 는 BAR 의 크기 마스크처럼 EP 가 자기 config 공간의 "그림자" 를
 * 설정할 때 쓰는 영역이다. 이 하드웨어에서는 그 영역이 DBI 와 같은 주소에
 * 겹쳐 있고(probe 가 dbi_base2 = dbi_base 로 둔다), ELBI 의 CS2_ENABLE
 * 비트로 어느 쪽을 볼지 전환한다.
 *
 * 그래서 이 함수의 모양이 "게이트를 열고 → 쓰고 → 닫는" 세 줄이 된다.
 * 게이트를 닫지 않으면 이후의 평범한 DBI 쓰기가 모두 DBI2 로 가 버린다.
 *
 * 쓰기 실패를 오류 로그로만 알리고 반환하지 않는데, 콜백 시그니처가 void
 * 이기 때문이다.
 *
 * 락이 없다. 이 경로가 EPF 설정 시점에만 쓰이고 그 시점은 직렬화되어 있는
 * 것으로 보이나, 그 보장의 근거는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(EPF 가 BAR 를 설정할 때).
 *
 * 호출 체인:  DWC EP 코어(dw_pcie_writel_dbi2 등) → dw_pcie_ops.write_dbi2
 *               → [이 함수]
 */
static void qcom_pcie_dw_write_dbi2(struct dw_pcie *pci, void __iomem *base,
				    u32 reg, size_t size, u32 val)
{
	int ret; /* [한국어] DBI2 쓰기 결과 */

	writel(1, pci->elbi_base + ELBI_CS2_ENABLE); /* [한국어] ELBI 게이트를 연다 — 이후 DBI 접근이 DBI2 로 간다 */

	ret = dw_pcie_write(pci->dbi_base2 + reg, size, val); /* [한국어] 실제 쓰기. dbi_base2 는 probe 가 dbi_base 와 같은 값으로 둔 것이다 */
	if (ret) /* [한국어] 실패했으면 */
		dev_err(pci->dev, "Failed to write DBI2 register (0x%x): %d\n", reg, ret); /* [한국어] 콜백 시그니처가 void 라 오류를 올릴 수 없어 로그로만 알린다 */

	writel(0, pci->elbi_base + ELBI_CS2_ENABLE); /* [한국어] 게이트를 반드시 닫는다. 열어 둔 채로 두면 이후의 평범한 DBI 쓰기가 모두 DBI2 로 간다 */
}

/* [한국어]
 * qcom_pcie_ep_icc_update - 협상된 링크에 맞춰 인터커넥트 대역폭을 다시 요구한다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 *
 * RC 판의 qcom_pcie_icc_opp_update() 와 같은 목적이되 훨씬 단순하다 —
 * OPP 갈래가 없고 인터커넥트만 다룬다. EP 쪽은 OPP 표를 쓰지 않기 때문이다.
 *
 * 링크가 섰는지도 확인하지 않는다. RC 판은 DLLLA 비트를 보고 서 있을 때만
 * 갱신하는데, 여기서는 이 함수가 **BME(Bus Master Enable) 이벤트에서만**
 * 불리기 때문이다. 호스트가 버스 마스터를 켰다는 것은 링크가 이미 서고
 * 열거까지 끝났다는 뜻이라, 따로 확인할 이유가 없다.
 *
 * icc 경로가 없으면(firmware_managed 구성 등) 곧바로 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(global IRQ 스레드).
 *
 * 호출 체인:  qcom_pcie_ep_global_irq_thread()(BME 이벤트) → [이 함수]
 */
static void qcom_pcie_ep_icc_update(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] DBI 창과 device 를 얻기 위한 DWC 구조체 */
	u32 offset, status; /* [한국어] capability 오프셋과 링크 상태 워드 */
	int speed, width; /* [한국어] 협상된 속도와 폭 */
	int ret; /* [한국어] 요구 결과 */

	if (!pcie_ep->icc_mem) /* [한국어] 인터커넥트를 쓰지 않는 구성이면 */
		return; /* [한국어] 할 일이 없다 */

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 config 공간 오프셋 */
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* [한국어] 표준 링크 상태 레지스터를 읽는다. RC 판과 달리 링크업 여부를 확인하지 않는데, 이 함수가 BME 이벤트에서만 불려 이미 링크가 서 있음이 보장되기 때문이다 */

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status); /* [한국어] 협상된 링크 속도 */
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status); /* [한국어] 협상된 레인 수 */

	ret = icc_set_bw(pcie_ep->icc_mem, 0, width * QCOM_PCIE_LINK_SPEED_TO_BW(speed)); /* [한국어] 폭 x 속도당 대역폭을 요구한다 */
	if (ret) /* [한국어] 요구가 실패했으면 */
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret); /* [한국어] 알리기만 한다 — 링크는 이미 서 있어 치명적이지 않다 */
}

/* [한국어]
 * qcom_pcie_enable_resources - 클록·리셋·PHY·인터커넥트를 순서대로 켠다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 호스트가 PERST# 를 놓았을 때 하드웨어를 깨우는 앞 절반이다.
 *
 *   1) 클록을 켠다.
 *   2) 코어 리셋을 순환시킨다.
 *   3) PHY 를 초기화하고, **EP 모드**로 지정한 뒤 켠다.
 *      phy_set_mode_ext 의 PHY_MODE_PCIE_EP 가 이 파일과 RC 파일을 가르는
 *      한 줄이다 — pcie-qcom.c 의 qcom_pcie_phy_power_on() 은 같은 자리에
 *      PHY_MODE_PCIE_RC 를 넘긴다.
 *   4) 인터커넥트에 초기 대역폭을 요구한다. 상류 주석이 RC 판과 똑같은
 *      근거를 든다 — 일부 플랫폼이 인터커넥트 클록을 켜기 전에 대역폭
 *      제약을 먼저 설정하도록 요구하므로, 1레인 Gen1 값을 미리 건다.
 *      실제 값은 링크가 선 뒤 qcom_pcie_ep_icc_update() 가 갱신한다.
 *
 * 되돌리기가 라벨 셋으로 정확히 대칭이다.
 *
 * RC 판은 이 일이 세대별 init 콜백 여럿으로 나뉘어 있는데, EP 는 세대 표가
 * 없어 함수 하나로 끝난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PERST# 스레드). 잠든다.
 *
 * 호출 체인:  qcom_pcie_perst_deassert() → [이 함수]
 *               → qcom_pcie_ep_core_reset() → phy_set_mode_ext()
 */
static int qcom_pcie_enable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] 로그의 기준을 얻기 위한 DWC 구조체 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = clk_bulk_prepare_enable(pcie_ep->num_clks, pcie_ep->clks); /* [한국어] 클록을 켠다 */
	if (ret) /* [한국어] 못 켰으면 */
		return ret; /* [한국어] 아직 아무것도 켜지 않았다 */

	ret = qcom_pcie_ep_core_reset(pcie_ep); /* [한국어] 코어 리셋을 순환시킨다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_disable_clk; /* [한국어] 클록을 되돌린다 */

	ret = phy_init(pcie_ep->phy); /* [한국어] PHY 를 초기화한다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_disable_clk; /* [한국어] 클록을 되돌린다 */

	ret = phy_set_mode_ext(pcie_ep->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_EP); /* [한국어] PHY 를 **EP 모드**로 지정한다. RC 판은 같은 자리에 PHY_MODE_PCIE_RC 를 넘긴다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_phy_exit; /* [한국어] PHY 초기화부터 되돌린다 */

	ret = phy_power_on(pcie_ep->phy); /* [한국어] PHY 를 켠다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_phy_exit; /* [한국어] PHY 초기화부터 되돌린다 */

	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(pcie_ep->icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1)); /* [한국어] 상류 주석대로 1레인 Gen1 에 해당하는 초기 대역폭을 미리 건다. RC 판의 icc_init 과 같은 근거다 */
	if (ret) { /* [한국어] 요구가 실패했으면 */
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret); /* [한국어] 알리고 */
		goto err_phy_off; /* [한국어] PHY 부터 되돌린다 */
	}

	return 0; /* [한국어] 자원 기동 완료 */

err_phy_off: /* [한국어] 인터커넥트 실패가 여기로 온다 */
	phy_power_off(pcie_ep->phy); /* [한국어] PHY 를 끈다 */
err_phy_exit: /* [한국어] PHY 모드/전원 실패가 여기로 온다 */
	phy_exit(pcie_ep->phy); /* [한국어] PHY 초기화를 되돌린다 */
err_disable_clk: /* [한국어] 리셋/PHY 초기화 실패가 여기로 온다 */
	clk_bulk_disable_unprepare(pcie_ep->num_clks, pcie_ep->clks); /* [한국어] 클록을 끈다 — 켠 역순이다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_disable_resources - 켜 둔 자원을 모두 놓는다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 *
 * enable 의 역순이다 — 인터커넥트 요구를 0 으로 내리고, PHY 를 끄고,
 * 클록을 끈다.
 *
 * 맨 앞의 pm_runtime_put 이 조건 밖에 있는 것에 주의. firmware_managed 든
 * 아니든 참조는 놓는다 — qcom_pcie_perst_deassert() 가 그 갈래에서도
 * pm_runtime_resume_and_get 을 부르기 때문에 짝이 맞는다.
 *
 * firmware_managed 구성에서는 나머지를 건너뛴다. 펌웨어가 클록과 PHY 를
 * 소유하므로 드라이버가 끄면 안 되기 때문이다. 실제로 그 구성에서는
 * qcom_pcie_ep_get_resources() 도 클록·리셋·PHY 를 아예 잡지 않아, 끌
 * 대상 자체가 없다.
 *
 * 각 단계의 실패를 확인하지 않는다. 정리 경로라 되돌릴 방법이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  qcom_pcie_perst_assert() / qcom_pcie_perst_deassert()(실패 경로) /
 *               qcom_pcie_ep_remove() → [이 함수]
 */
static void qcom_pcie_disable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = pcie_ep->pci.dev; /* [한국어] 런타임 PM 과 로그의 기준 */

	pm_runtime_put(dev); /* [한국어] 조건 밖에 있다 — firmware_managed 갈래도 perst_deassert 에서 참조를 잡으므로 짝이 맞는다 */

	/* Skip resource disablement if controller is firmware-managed */
	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* [한국어] 상류 주석대로 펌웨어가 클록과 PHY 를 소유하는 구성이면 */
		return; /* [한국어] 드라이버가 끄면 안 되므로 여기서 끝낸다. 실제로 그 구성에서는 잡은 것도 없다 */

	icc_set_bw(pcie_ep->icc_mem, 0, 0); /* [한국어] 인터커넥트 요구를 0 으로 내린다 */
	phy_power_off(pcie_ep->phy); /* [한국어] PHY 를 끈다 */
	phy_exit(pcie_ep->phy); /* [한국어] PHY 초기화를 되돌린다 */
	clk_bulk_disable_unprepare(pcie_ep->num_clks, pcie_ep->clks); /* [한국어] 클록을 끈다 — enable 의 역순이다. 각 단계의 실패를 확인하지 않는 것은 정리 경로라 되돌릴 방법이 없기 때문이다 */
}

/* [한국어]
 * qcom_pcie_perst_deassert - 호스트가 PERST# 를 놓았을 때 컨트롤러 전체를 세운다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일에서 가장 긴 함수이며, **이 드라이버의 실질적 기동 절차 본체**다.
 * RC 판이라면 probe 안에 있었을 일이 여기 인터럽트 스레드 안에 있다 —
 * EP 는 호스트가 준비되기 전에는 아무것도 할 수 없기 때문이다.
 *
 * [자원]
 *   1) 런타임 PM 참조를 잡아 전원 도메인을 깨운다.
 *   2) firmware_managed 가 아니면 클록·리셋·PHY·인터커넥트를 켠다.
 *      그 갈래는 펌웨어가 이미 해 두었으므로 건너뛴다.
 *
 * [이전 상태 정리]
 *   3) pci_epc_deinit_notify()(drivers/pci/endpoint/pci-epc-core.c:1439) 와
 *      dw_pcie_ep_cleanup()(pcie-designware-ep.c:2321) 을 부른다. 상류 주석이
 *      "refclk 이 필요한 정리" 라고 밝히는데, 호스트가 리셋을 걸었다 놓는
 *      것은 재부팅일 수 있어 이전 세션의 상태를 먼저 지워야 하기 때문이다.
 *      자원을 켠 **뒤에** 정리하는 순서가 그 주석의 뜻이다.
 *
 * [호스트에 알림]
 *   4) WAKE# 를 2ms 동안 펄스로 눌러 "장치가 준비됐다" 고 호스트에 알린다.
 *      RC 판에는 이 신호가 없다 — 방향이 반대이기 때문이다.
 *
 * [PARF 프로그래밍]
 *   5) TCSR 의 PERST 분리 설정을 되돌린다.
 *   6) BDF→SID 변환을 **우회로 둔다**. RC 판의 qcom_pcie_config_sid_1_9_0()
 *      이 같은 레지스터의 우회를 꺼서 변환표를 채우는 것과 정확히 반대다 —
 *      EP 는 요청자가 아니라 대상이라 변환이 필요 없다.
 *   7) 디버그 인터럽트 셋(PM D-state 변경, 버스 마스터 활성, PM Turn-off)을
 *      켠다. 이 셋이 아래 global IRQ 핸들러가 다루는 이벤트의 출처다.
 *   8) PARF_DEVICE_TYPE 에 **EP 값**을 쓴다. pcie-qcom.c 는 같은 레지스터에
 *      RC 값을 쓴다. 같은 IP 가 어느 쪽으로 동작할지를 정하는 한 줄이다.
 *   9) L1 진입 허용, 읽기가 쓰기를 막지 않게, 쓰기 후 쓰기 정지, Q2A 플러시
 *      해제 — 데이터 경로의 흐름 제어 설정 넷.
 *  10) 상류 주석이 네 항목을 한 문단으로 설명하는 SYS_CTRL 설정. 유휴 시
 *      마스터 AXI 클록을 끄고, DBI 접근이 코어를 L1 에서 깨우지 못하게 하고,
 *      PIPE 클록이 코어 클록으로 전파되는 것을 막는 게이팅을 끄고,
 *      호스트에 Vaux 가 있다고 보고한다.
 *  11) 디바운서 다섯을 모두 막는다.
 *  12) MSI 와 LTR 메시지에서 L1SS 를 빠져나오도록 요청한다.
 *  13) 쓰기 창을 열고 L0s/L1 진입 지연(exit latency)을 각각 0x6 으로 적는다.
 *      호스트가 이 값을 보고 ASPM 을 켤지 판단한다.
 *  14) 인터럽트 마스크를 0 으로 지운 뒤 다룰 이벤트만 다시 세운다.
 *      링크다운·BME·PM Turn-off·D-state 변경·링크업·eDMA 여섯이다.
 *  15) SoC 표가 요구하면 MHI RAM 패리티 오류 인터럽트를 끄고, PTM 갱신
 *      인터럽트는 늘 끈다.
 *
 * [EP 등록과 링크]
 *  16) dw_pcie_ep_init_registers()(pcie-designware-ep.c:2629) 로 EP 의
 *      config 공간을 실제로 채운다.
 *  17) **RC 와 공유하는 두 함수**로 이퀄라이제이션과 Gen4 레인 마진을
 *      설정한다. pcie-qcom.c 의 qcom_pcie_start_link() 와 같은 조건·같은
 *      순서다.
 *  18) MMIO 영역의 물리 주소를 MHI BASE 레지스터에 적는다. 상류 주석대로
 *      그 영역이 BAR 로 노출되기 때문이다.
 *  19) L1SS 동안 MHI 버스로 가는 마스터 AXI 클록을 게이트한다.
 *  20) pci_epc_init_notify()(pci-epc-core.c:1365) 로 EPF 드라이버에
 *      "이제 BAR 를 설정해도 된다" 고 알린다.
 *  21) LTSSM 을 켠다. 여기서 비트 8 을 상수 이름 없이 직접 쓰는데,
 *      pcie-qcom.c 는 같은 비트를 LTSSM_EN 으로 정의해 쓴다 — 두 파일이
 *      PARF 정의를 공유하지 않는 데서 온 차이다.
 *  22) SoC 표가 요구하면 NO_SNOOP 무시를 켠다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PERST# IRQ 스레드). 잠든다 — 그래서
 * 이 인터럽트가 스레드 핸들러여야 한다.
 *
 * 호출 체인:  qcom_pcie_ep_perst_irq_thread() → [이 함수]
 *               → qcom_pcie_enable_resources() → dw_pcie_ep_init_registers()
 *               → qcom_pcie_common_set_equalization() → pci_epc_init_notify()
 */
static int qcom_pcie_perst_deassert(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* [한국어] DWC 구조체에서 이 파일의 상태를 되찾는다 */
	struct device *dev = pci->dev; /* [한국어] 로그와 런타임 PM 의 기준 */
	u32 val, offset; /* [한국어] 읽고-고쳐-쓸 임시 값과 capability 오프셋 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = pm_runtime_resume_and_get(dev); /* [한국어] 전원 도메인을 깨운다. 아래 모든 레지스터 접근의 전제다 */
	if (ret < 0) { /* [한국어] 못 깨웠으면 */
		dev_err(dev, "Failed to enable device: %d\n", ret); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 돌아간다 */
	}

	/* Skip resource enablement if controller is firmware-managed */
	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* [한국어] 상류 주석대로 펌웨어가 관리하는 구성이면 */
		goto skip_resources_enable; /* [한국어] 자원 기동을 건너뛴다 */

	ret = qcom_pcie_enable_resources(pcie_ep); /* [한국어] 클록·리셋·PHY·인터커넥트를 켠다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err(dev, "Failed to enable resources: %d\n", ret); /* [한국어] 알리고 */
		pm_runtime_put(dev); /* [한국어] 방금 잡은 런타임 PM 참조를 놓고 */
		return ret; /* [한국어] 그대로 돌아간다 */
	}

skip_resources_enable: /* [한국어] 두 갈래가 여기서 다시 만난다 */
	/* Perform cleanup that requires refclk */
	pci_epc_deinit_notify(pci->ep.epc); /* [한국어] 상류 주석대로 refclk 이 필요한 정리다 — 이전 세션의 EPC 상태를 지운다(drivers/pci/endpoint/pci-epc-core.c:1439) */
	dw_pcie_ep_cleanup(&pci->ep); /* [한국어] DWC EP 코어의 정리(pcie-designware-ep.c:2321). 호스트 재부팅일 수 있어 자원을 켠 뒤 이전 상태를 먼저 지운다 */

	/* Assert WAKE# to RC to indicate device is ready */
	gpiod_set_value_cansleep(pcie_ep->wake, 1); /* [한국어] 상류 주석대로 WAKE# 를 눌러 준비 완료를 호스트에 알린다 */
	usleep_range(WAKE_DELAY_US, WAKE_DELAY_US + 500); /* [한국어] 2ms 동안 유지한다 */
	gpiod_set_value_cansleep(pcie_ep->wake, 0); /* [한국어] 놓는다 — 펄스다. RC 판에는 이 신호가 없다 */

	qcom_pcie_ep_configure_tcsr(pcie_ep); /* [한국어] TCSR 의 PERST 자동 리셋 연동을 끊는다 */

	/* Disable BDF to SID mapping */
	val = readl_relaxed(pcie_ep->parf + PARF_BDF_TO_SID_CFG); /* [한국어] BDF→SID 설정 레지스터를 읽는다 */
	val |= PARF_BDF_TO_SID_BYPASS; /* [한국어] 상류 주석대로 변환을 끈다. RC 판은 반대로 우회를 꺼서 변환표를 채운다 — EP 는 요청자가 아니라 대상이라 변환이 필요 없다 */
	writel_relaxed(val, pcie_ep->parf + PARF_BDF_TO_SID_CFG); /* [한국어] 되쓴다 */

	/* Enable debug IRQ */
	val = readl_relaxed(pcie_ep->parf + PARF_DEBUG_INT_EN); /* [한국어] 디버그 인터럽트 활성 레지스터를 읽는다 */
	val |= PARF_DEBUG_INT_RADM_PM_TURNOFF |
	       PARF_DEBUG_INT_CFG_BUS_MASTER_EN |
	       PARF_DEBUG_INT_PM_DSTATE_CHANGE; /* [한국어] PM Turn-off·버스 마스터 활성·D-state 변경 셋을 켠다. 이 셋이 global IRQ 가 다루는 이벤트의 출처다 */
	writel_relaxed(val, pcie_ep->parf + PARF_DEBUG_INT_EN); /* [한국어] 되쓴다 */

	/* Configure PCIe to endpoint mode */
	writel_relaxed(PARF_DEVICE_TYPE_EP, pcie_ep->parf + PARF_DEVICE_TYPE); /* [한국어] 컨트롤러를 **EP 로** 둔다. RC 판은 같은 레지스터에 RC 값을 쓴다 */

	/* Allow entering L1 state */
	val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 전력 관리 제어 레지스터를 읽는다 */
	val &= ~PARF_PM_CTRL_REQ_NOT_ENTR_L1; /* [한국어] 상류 주석대로 L1 진입 금지를 지워 L1 을 허용한다 */
	writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 되쓴다 */

	/* Read halts write */
	val = readl_relaxed(pcie_ep->parf + PARF_AXI_MSTR_RD_HALT_NO_WRITES); /* [한국어] 읽기 정지 제어 레지스터를 읽는다 */
	val &= ~PARF_AXI_MSTR_RD_HALT_NO_WRITE_EN; /* [한국어] 상류 주석대로 읽기가 쓰기를 막지 않게 한다 */
	writel_relaxed(val, pcie_ep->parf + PARF_AXI_MSTR_RD_HALT_NO_WRITES); /* [한국어] 되쓴다 */

	/* Write after write halt */
	val = readl_relaxed(pcie_ep->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* [한국어] 쓰기 주소 정지 레지스터를 읽는다 */
	val |= PARF_AXI_MSTR_WR_ADDR_HALT_EN; /* [한국어] 상류 주석대로 쓰기 후 쓰기 정지를 켠다 */
	writel_relaxed(val, pcie_ep->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* [한국어] 되쓴다 */

	/* Q2A flush disable */
	val = readl_relaxed(pcie_ep->parf + PARF_Q2A_FLUSH); /* [한국어] Q2A 플러시 레지스터를 읽는다 */
	val &= ~PARF_Q2A_FLUSH_EN; /* [한국어] 상류 주석대로 플러시를 끈다 */
	writel_relaxed(val, pcie_ep->parf + PARF_Q2A_FLUSH); /* [한국어] 되쓴다 */

	/*
	 * Disable Master AXI clock during idle.  Do not allow DBI access
	 * to take the core out of L1.  Disable core clock gating that
	 * gates PIPE clock from propagating to core clock.  Report to the
	 * host that Vaux is present.
	 */
	val = readl_relaxed(pcie_ep->parf + PARF_SYS_CTRL); /* [한국어] 시스템 제어 레지스터를 읽는다 */
	val &= ~PARF_SYS_CTRL_MSTR_ACLK_CGC_DIS; /* [한국어] 상류 주석의 첫 항목 — 유휴 시 마스터 AXI 클록을 끌 수 있게 게이팅 금지를 지운다 */
	val |= PARF_SYS_CTRL_SLV_DBI_WAKE_DISABLE |
	       PARF_SYS_CTRL_CORE_CLK_CGC_DIS |
	       PARF_SYS_CTRL_AUX_PWR_DET; /* [한국어] 나머지 셋을 켠다 — DBI 접근이 코어를 L1 에서 깨우지 못하게, 코어 클록 게이팅을 금지, 호스트에 Vaux 보고 */
	writel_relaxed(val, pcie_ep->parf + PARF_SYS_CTRL); /* [한국어] 되쓴다 */

	/* Disable the debouncers */
	val = readl_relaxed(pcie_ep->parf + PARF_DB_CTRL); /* [한국어] 디바운서 제어 레지스터를 읽는다 */
	val |= PARF_DB_CTRL_INSR_DBNCR_BLOCK | PARF_DB_CTRL_RMVL_DBNCR_BLOCK |
	       PARF_DB_CTRL_DBI_WKP_BLOCK | PARF_DB_CTRL_SLV_WKP_BLOCK |
	       PARF_DB_CTRL_MST_WKP_BLOCK; /* [한국어] 상류 주석대로 다섯 디바운서를 모두 막는다 */
	writel_relaxed(val, pcie_ep->parf + PARF_DB_CTRL); /* [한국어] 되쓴다 */

	/* Request to exit from L1SS for MSI and LTR MSG */
	val = readl_relaxed(pcie_ep->parf + PARF_CFG_BITS); /* [한국어] 설정 비트 레지스터를 읽는다 */
	val |= PARF_CFG_BITS_REQ_EXIT_L1SS_MSI_LTR_EN; /* [한국어] 상류 주석대로 MSI 와 LTR 메시지에서 L1SS 를 빠져나오게 한다 */
	writel_relaxed(val, pcie_ep->parf + PARF_CFG_BITS); /* [한국어] 되쓴다 */

	dw_pcie_dbi_ro_wr_en(pci); /* [한국어] 아래 능력 필드를 고치려고 쓰기 창을 연다 */

	/* Set the L0s Exit Latency to 2us-4us = 0x6 */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 의 오프셋을 찾는다 */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP); /* [한국어] 링크 능력 레지스터를 읽는다 */
	val &= ~PCI_EXP_LNKCAP_L0SEL; /* [한국어] L0s 탈출 지연 필드를 비우고 */
	val |= FIELD_PREP(PCI_EXP_LNKCAP_L0SEL, 0x6); /* [한국어] 상류 주석대로 2~4us 를 뜻하는 0x6 을 넣는다 */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, val); /* [한국어] 되쓴다 */

	/* Set the L1 Exit Latency to be 32us-64 us = 0x6 */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* [한국어] 같은 오프셋을 다시 찾는다 — 위에서 이미 구했지만 상류 코드가 그대로 반복한다 */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP); /* [한국어] 같은 레지스터를 다시 읽는다 */
	val &= ~PCI_EXP_LNKCAP_L1EL; /* [한국어] L1 탈출 지연 필드를 비우고 */
	val |= FIELD_PREP(PCI_EXP_LNKCAP_L1EL, 0x6); /* [한국어] 상류 주석대로 32~64us 를 뜻하는 0x6 을 넣는다 */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, val); /* [한국어] 되쓴다. 호스트가 이 값들을 보고 ASPM 을 켤지 판단한다 */

	dw_pcie_dbi_ro_wr_dis(pci); /* [한국어] 쓰기 창을 닫는다 */

	writel_relaxed(0, pcie_ep->parf + PARF_INT_ALL_MASK); /* [한국어] 마스크를 먼저 0 으로 지운다 */
	val = PARF_INT_ALL_LINK_DOWN | PARF_INT_ALL_BME |
	      PARF_INT_ALL_PM_TURNOFF | PARF_INT_ALL_DSTATE_CHANGE |
	      PARF_INT_ALL_LINK_UP | PARF_INT_ALL_EDMA; /* [한국어] 다룰 이벤트만 다시 세운다 — 링크다운·BME·PM Turn-off·D-state 변경·링크업·eDMA */
	writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_MASK); /* [한국어] 되쓴다 */

	if (pcie_ep->cfg && pcie_ep->cfg->disable_mhi_ram_parity_check) { /* [한국어] SoC 표가 요구하면 */
		val = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_5_MASK); /* [한국어] 5번 마스크를 읽는다 */
		val &= ~PARF_INT_ALL_5_MHI_RAM_DATA_PARITY_ERR; /* [한국어] MHI RAM 패리티 오류 인터럽트를 끈다 */
		writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_5_MASK); /* [한국어] 되쓴다 */
	}

	val = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_3_MASK); /* [한국어] 3번 마스크를 읽는다 */
	val &= ~PARF_INT_ALL_3_PTM_UPDATING; /* [한국어] PTM 갱신 인터럽트는 SoC 를 가리지 않고 끈다 */
	writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_3_MASK); /* [한국어] 되쓴다 */

	ret = dw_pcie_ep_init_registers(&pcie_ep->pci.ep); /* [한국어] EP 의 config 공간을 실제로 채운다(pcie-designware-ep.c:2629) */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err(dev, "Failed to complete initialization: %d\n", ret); /* [한국어] 알리고 */
		goto err_disable_resources; /* [한국어] 켠 자원을 모두 되돌린다 */
	}

	qcom_pcie_common_set_equalization(pci); /* [한국어] **RC 파일과 공유하는 함수**. 이퀄라이제이션 preset 을 적는다 */

	if (pcie_get_link_speed(pci->max_link_speed) == PCIE_SPEED_16_0GT) /* [한국어] 최대 속도가 Gen4 이면 */
		qcom_pcie_common_set_16gt_lane_margining(pci); /* [한국어] 같은 공유 파일의 레인 마진 설정도 켠다. RC 판의 qcom_pcie_start_link() 와 같은 조건·순서다 */

	/*
	 * The physical address of the MMIO region which is exposed as the BAR
	 * should be written to MHI BASE registers.
	 */
	writel_relaxed(pcie_ep->mmio_res->start,
		       pcie_ep->parf + PARF_MHI_BASE_ADDR_LOWER); /* [한국어] 상류 주석대로 BAR 로 노출할 MMIO 영역의 물리 주소를 MHI BASE 에 적는다 */
	writel_relaxed(0, pcie_ep->parf + PARF_MHI_BASE_ADDR_UPPER); /* [한국어] 상위 워드는 0 — 이 드라이버가 32비트 주소만 쓴다는 뜻이다 */

	/* Gate Master AXI clock to MHI bus during L1SS */
	val = readl_relaxed(pcie_ep->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] MHI 클록/리셋 제어를 읽는다 */
	val &= ~PARF_MSTR_AXI_CLK_EN; /* [한국어] 상류 주석대로 L1SS 동안 MHI 버스로 가는 마스터 AXI 클록을 게이트한다 */
	writel_relaxed(val, pcie_ep->parf + PARF_MHI_CLOCK_RESET_CTRL); /* [한국어] 되쓴다 */

	pci_epc_init_notify(pcie_ep->pci.ep.epc); /* [한국어] EPF 드라이버에 "이제 BAR 를 설정해도 된다" 고 알린다(pci-epc-core.c:1365) */

	/* Enable LTSSM */
	val = readl_relaxed(pcie_ep->parf + PARF_LTSSM); /* [한국어] 링크 학습 레지스터를 읽는다 */
	val |= BIT(8); /* [한국어] 활성 비트를 세운다. RC 판은 같은 비트를 LTSSM_EN 상수로 쓰는데 여기서는 직접 BIT(8) 이다 */
	writel_relaxed(val, pcie_ep->parf + PARF_LTSSM); /* [한국어] 되쓴다 — 이제 링크가 서기 시작한다 */

	if (pcie_ep->cfg && pcie_ep->cfg->override_no_snoop) /* [한국어] SoC 표가 요구하면 */
		writel_relaxed(WR_NO_SNOOP_OVERRIDE_EN | RD_NO_SNOOP_OVERRIDE_EN,
				pcie_ep->parf + PARF_NO_SNOOP_OVERRIDE); /* [한국어] 읽기·쓰기 TLP 의 NO_SNOOP 을 무시하게 한다 */

	return 0; /* [한국어] 기동 절차 완료 */

err_disable_resources: /* [한국어] EP 레지스터 초기화 실패가 여기로 온다 */
	qcom_pcie_disable_resources(pcie_ep); /* [한국어] 자원을 모두 되돌린다(런타임 PM 참조 포함) */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_perst_assert - 호스트가 PERST# 를 걸었을 때 자원을 모두 끈다
 *
 * @pci: DWC 코어의 컨트롤러 구조체.
 *
 * deassert 의 짝이며, 극단적으로 짧다 — 자원을 끄고 상태를 DISABLED 로
 * 되돌리는 두 줄이다.
 *
 * deassert 가 한 PARF 프로그래밍을 되돌리지 않는데, 자원이 꺼지면 그
 * 레지스터들이 어차피 초기값으로 돌아가고 다음 deassert 가 처음부터 다시
 * 쓰기 때문이다.
 *
 * link_status 를 여기서 DISABLED 로 두는 것이 상태 기계의 출발점이다.
 * 이후 global IRQ 가 BME 를 받으면 ENABLED, 링크업이면 UP, 링크다운이면
 * DOWN 으로 옮겨 간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PERST# IRQ 스레드).
 *
 * 호출 체인:  qcom_pcie_ep_perst_irq_thread() → [이 함수]
 *               → qcom_pcie_disable_resources()
 */
static void qcom_pcie_perst_assert(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci);

	qcom_pcie_disable_resources(pcie_ep);
	pcie_ep->link_status = QCOM_PCIE_EP_LINK_DISABLED; /* [한국어] 상태 기계를 출발점으로 되돌린다. 이후 global IRQ 가 BME/링크업/링크다운으로 옮긴다 */
}

/* Common DWC controller ops */
/* [한국어] DWC 코어가 되부르는 컨트롤러 동작 표(상류 주석의 "공통 DWC 컨트롤러 ops").
 * **넷을 채운다** — RC 판(pcie-qcom.c 의 dw_pcie_ops)이 link_up 과 start_link
 * 둘만 채우는 것과 대비된다. 더해진 stop_link 와 write_dbi2 는 둘 다 EP
 * 에만 필요한 동작이다. */
static const struct dw_pcie_ops pci_ops = {
	.link_up = qcom_pcie_dw_link_up, /* [한국어] ELBI 의 벤더 전용 비트로 링크업을 판정한다 */
	.start_link = qcom_pcie_dw_start_link, /* [한국어] PERST# 인터럽트를 연다 */
	.stop_link = qcom_pcie_dw_stop_link, /* [한국어] 그것을 닫는다. RC 판에는 이 콜백이 없다 */
	.write_dbi2 = qcom_pcie_dw_write_dbi2, /* [한국어] ELBI 게이트를 열고 DBI2 에 쓴다. 이 콜백도 EP 에만 있다 */
};

/* [한국어]
 * qcom_pcie_ep_get_io_resources - MMIO 창 셋과 TCSR regmap 을 잡는다
 *
 * @pdev:    플랫폼 디바이스.
 * @pcie_ep: 채울 EP 컨트롤러 상태.
 * @return: 0 성공, -EINVAL 은 mmio 자원이 없는 경우, 그 밖에는 매핑 오류.
 *
 * 창 셋을 이름으로 잡는다.
 *   - parf : 퀄컴 고유 제어 레지스터.
 *   - dbi  : DWC 의 config 공간. 잡은 뒤 **dbi_base2 를 같은 주소로 둔다** —
 *     이 하드웨어는 DBI2 가 별도 창이 아니라 ELBI 게이트로 전환되는
 *     구조이기 때문이며, 그 전환을 qcom_pcie_dw_write_dbi2() 가 한다.
 *   - mmio : BAR 로 노출할 영역. 주소뿐 아니라 struct resource 포인터를
 *     통째로 보관하는데, qcom_pcie_perst_deassert() 가 그 **물리 주소**를
 *     MHI BASE 레지스터에 적어야 하기 때문이다.
 *
 * TCSR("qcom,perst-regs")은 선택이다. 없으면 디버그 로그만 남기고 0 으로
 * 돌아간다 — PERST 분리 기능이 없는 보드다. 있으면 regmap 과 함께 두 개의
 * 오프셋(perst_en, perst_sep_en)을 DT 배열에서 인덱스로 읽는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_ep_get_resources() → [이 함수]
 */
static int qcom_pcie_ep_get_io_resources(struct platform_device *pdev,
					 struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev; /* [한국어] 자원 조회와 로그의 기준 */
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] dbi_base 를 채워 넣을 DWC 구조체 */
	struct device_node *syscon; /* [한국어] TCSR 노드 */
	struct resource *res; /* [한국어] MMIO 자원 서술자 */
	int ret; /* [한국어] 각 단계의 결과 */

	pcie_ep->parf = devm_platform_ioremap_resource_byname(pdev, "parf"); /* [한국어] 퀄컴 고유 제어 레지스터 창을 매핑한다 */
	if (IS_ERR(pcie_ep->parf)) /* [한국어] 실패했으면 */
		return PTR_ERR(pcie_ep->parf); /* [한국어] 그 오류를 올린다 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi"); /* [한국어] DWC 의 config 공간 자원을 찾는다 */
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, res); /* [한국어] config 접근에 맞게 매핑한다 */
	if (IS_ERR(pci->dbi_base)) /* [한국어] 실패했으면 */
		return PTR_ERR(pci->dbi_base); /* [한국어] 그 오류를 올린다 */
	pci->dbi_base2 = pci->dbi_base; /* [한국어] **DBI2 를 같은 주소로 둔다** — 이 하드웨어는 DBI2 가 별도 창이 아니라 ELBI 게이트로 전환되는 구조다 */

	pcie_ep->mmio_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, /* [한국어] BAR 로 노출할 MMIO 자원을 찾는다 */
							 "mmio");
	if (!pcie_ep->mmio_res) { /* [한국어] 없으면 */
		dev_err(dev, "Failed to get mmio resource\n"); /* [한국어] 알리고 */
		return -EINVAL;
	}

	pcie_ep->mmio = devm_pci_remap_cfg_resource(dev, pcie_ep->mmio_res); /* [한국어] 그 자원을 매핑한다. 서술자 자체는 위에서 보관해 두었다 — 물리 주소가 나중에 필요하기 때문이다 */
	if (IS_ERR(pcie_ep->mmio)) /* [한국어] 실패했으면 */
		return PTR_ERR(pcie_ep->mmio); /* [한국어] 그 오류를 올린다 */

	syscon = of_parse_phandle(dev->of_node, "qcom,perst-regs", 0); /* [한국어] TCSR 노드를 phandle 로 찾는다 */
	if (!syscon) { /* [한국어] 없으면 */
		dev_dbg(dev, "PERST separation not available\n"); /* [한국어] PERST 분리 기능이 없는 보드라는 뜻이다 */
		return 0;
	}

	pcie_ep->perst_map = syscon_node_to_regmap(syscon); /* [한국어] 그 노드를 regmap 으로 연다 */
	of_node_put(syscon);
	if (IS_ERR(pcie_ep->perst_map)) /* [한국어] 열지 못했으면 */
		return PTR_ERR(pcie_ep->perst_map); /* [한국어] 그 오류를 올린다 */

	ret = of_property_read_u32_index(dev->of_node, "qcom,perst-regs",
					 1, &pcie_ep->perst_en); /* [한국어] 같은 속성의 인덱스 1 에서 PERST 활성 오프셋을 읽는다 */
	if (ret < 0) { /* [한국어] 없으면 */
		dev_err(dev, "No Perst Enable offset in syscon\n"); /* [한국어] 알리고 */
		return ret;
	}

	ret = of_property_read_u32_index(dev->of_node, "qcom,perst-regs",
					 2, &pcie_ep->perst_sep_en); /* [한국어] 인덱스 2 에서 PERST 분리 활성 오프셋을 읽는다 */
	if (ret < 0) { /* [한국어] 없으면 */
		dev_err(dev, "No Perst Separation Enable offset in syscon\n"); /* [한국어] 알리고 */
		return ret;
	}

	return 0;
}

/* [한국어]
 * qcom_pcie_ep_get_resources - GPIO·클록·리셋·PHY·인터커넥트까지 모두 잡는다
 *
 * @pdev:    플랫폼 디바이스.
 * @pcie_ep: 채울 EP 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 자원 조회 오류.
 *
 * 먼저 MMIO 창과 TCSR 을 잡고, 그다음 나머지를 잡는다.
 *
 * GPIO 둘의 방향이 이 파일의 성격을 그대로 보여 준다.
 *   - reset(PERST#) : **GPIOD_IN** — 입력이다. 호스트가 이 선을 구동하고
 *     이 드라이버는 그 변화를 인터럽트로 받는다. RC 판(pcie-qcom.c)이
 *     같은 신호를 GPIOD_OUT_HIGH 로 잡아 직접 구동하는 것과 정반대다.
 *     필수라 없으면 probe 가 실패한다.
 *   - wake(WAKE#)   : GPIOD_OUT_LOW — 출력이다. 준비가 끝났음을 호스트에
 *     알리는 데 쓰며, 선택이다.
 *
 * firmware_managed 구성이면 여기서 돌아간다. 클록·리셋·PHY·인터커넥트를
 * 펌웨어가 소유하므로 잡지 않으며, 그래서 qcom_pcie_disable_resources() 도
 * 그 구성에서 그것들을 건드리지 않는다.
 *
 * [관찰] 마지막 두 조회(phy, icc)는 실패해도 곧바로 반환하지 않고 ret 에
 * 담아 두기만 한다. 그래서 phy 가 실패해도 icc 조회가 성공하면 ret 가
 * 덮여 성공으로 돌아간다. 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_ep_probe() → [이 함수] → qcom_pcie_ep_get_io_resources()
 */
static int qcom_pcie_ep_get_resources(struct platform_device *pdev,
				      struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev; /* [한국어] 자원 조회와 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = qcom_pcie_ep_get_io_resources(pdev, pcie_ep); /* [한국어] 먼저 MMIO 창 셋과 TCSR 을 잡는다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err(dev, "Failed to get io resources %d\n", ret); /* [한국어] 알리고 */
		return ret;
	}

	pcie_ep->reset = devm_gpiod_get(dev, "reset", GPIOD_IN); /* [한국어] PERST# 를 **입력**으로 잡는다. RC 판이 GPIOD_OUT_HIGH 로 직접 구동하는 것과 정반대이며, 이 한 줄이 RC/EP 의 역할 차이를 보여 준다 */
	if (IS_ERR(pcie_ep->reset)) /* [한국어] 없으면 */
		return PTR_ERR(pcie_ep->reset); /* [한국어] 필수라 그대로 실패한다 */

	pcie_ep->wake = devm_gpiod_get_optional(dev, "wake", GPIOD_OUT_LOW); /* [한국어] WAKE# 를 출력으로 잡는다. 선택이라 없으면 NULL 이다 */
	if (IS_ERR(pcie_ep->wake)) /* [한국어] 실제 오류이면 */
		return PTR_ERR(pcie_ep->wake); /* [한국어] 그대로 올린다 */

	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* [한국어] 펌웨어가 클록·PHY 를 소유하는 구성이면 */
		return 0;

	pcie_ep->num_clks = devm_clk_bulk_get_all(dev, &pcie_ep->clks); /* [한국어] 클록을 통째로 받는다 */
	if (pcie_ep->num_clks < 0) { /* [한국어] 조회가 실패하면 음수가 온다 */
		dev_err(dev, "Failed to get clocks\n"); /* [한국어] 알리고 */
		return pcie_ep->num_clks; /* [한국어] 그 음수를 올린다 */
	}

	pcie_ep->core_reset = devm_reset_control_get_exclusive(dev, "core"); /* [한국어] "core" 리셋 하나. RC 판이 세대마다 개수를 달리하는 것과 대비된다 */
	if (IS_ERR(pcie_ep->core_reset)) /* [한국어] 없으면 */
		return PTR_ERR(pcie_ep->core_reset); /* [한국어] 그 오류를 올린다 */

	pcie_ep->phy = devm_phy_optional_get(dev, "pciephy"); /* [한국어] PHY 를 optional 로 잡는다 */
	if (IS_ERR(pcie_ep->phy)) /* [한국어] 실패했으면 */
		ret = PTR_ERR(pcie_ep->phy); /* [한국어] [관찰] 곧바로 반환하지 않고 ret 에 담아만 둔다 — 아래 icc 조회가 성공하면 이 값이 덮인다. 상류 코드 그대로다 */

	pcie_ep->icc_mem = devm_of_icc_get(dev, "pcie-mem"); /* [한국어] 인터커넥트 경로를 잡는다 */
	if (IS_ERR(pcie_ep->icc_mem)) /* [한국어] 실패했으면 */
		ret = PTR_ERR(pcie_ep->icc_mem); /* [한국어] 역시 ret 에 담아만 둔다 */

	return ret;
}

/* TODO: Notify clients about PCIe state change */
/* [한국어]
 * qcom_pcie_ep_global_irq_thread - 링크·전력 이벤트를 갈라 상태 기계를 옮긴다
 *
 * @irq:  인터럽트 번호. 쓰지 않는다.
 * @data: 등록 시 넘긴 EP 컨트롤러 상태.
 * @return: 늘 IRQ_HANDLED.
 *
 * PARF_INT_ALL_STATUS 한 워드에 여러 이벤트가 비트로 모여 있고, 이 함수가
 * 그것을 갈라 처리한다. **RC 판에는 대응하는 것이 없다** — RC 는 자기가
 * 링크를 세우므로 통보받을 것이 없기 때문이다.
 *
 * 맨 먼저 상태를 그대로 되써서 지운다(R/W1C). 처리 전에 지우므로, 처리 중
 * 새로 온 이벤트를 놓치지 않는다.
 *
 * 이어지는 것이 else-if 사슬이라는 점이 중요하다. 한 번에 여러 비트가 서
 * 있어도 **첫 번째 것만 처리**하고 나머지는 이미 지워진 뒤라 사라진다.
 * 상류 함수 위의 TODO 주석("클라이언트에 PCIe 상태 변화를 알릴 것")과 함께,
 * 이 처리 방식이 완성형이 아님을 보여 준다.
 *
 * 다루는 이벤트 다섯:
 *   - 링크다운 : 상태를 DOWN 으로 두고 dw_pcie_ep_linkdown()
 *     (pcie-designware-ep.c:2848) 으로 EPF 에 알린다.
 *   - BME(버스 마스터 활성) : 호스트가 열거를 마치고 DMA 를 허용했다는 뜻이라
 *     상태를 ENABLED 로 두고, 대역폭을 실제 링크에 맞추고,
 *     pci_epc_bus_master_enable_notify()(pci-epc-core.c:1475) 로 알린다.
 *   - PM Turn-off : 호스트가 링크를 재우려 한다. READY_ENTR_L23 을 세워
 *     "L23 으로 들어갈 준비가 됐다" 고 응답한다.
 *   - D-state 변경 : DBI 의 전력 상태 필드를 읽어 D3 이면 L1 에서 빠져나오도록
 *     요청한다. D3 로 들어가는 중에는 링크가 깨어 있어야 하기 때문으로
 *     보이나, 근거 문서는 이 트리에서 확인 못 함.
 *   - 링크업 : dw_pcie_ep_linkup()(pcie-designware-ep.c:2812) 으로 알리고
 *     상태를 UP 으로 둔다.
 *   - 그 밖 : 한 번만 경고한다.
 *
 * 링크업 갈래에서 알림이 상태 갱신보다 먼저인 것에 주의 — 다른 갈래들은
 * 상태를 먼저 바꾼다. 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(스레드 IRQ, IRQF_ONESHOT). 잠들 수 있다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → dw_pcie_ep_linkup() /
 *               pci_epc_bus_master_enable_notify()
 */
static irqreturn_t qcom_pcie_ep_global_irq_thread(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data; /* [한국어] 등록 시 넘긴 EP 상태 */
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] DBI 접근과 EPC 를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	u32 status = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_STATUS); /* [한국어] 모든 이벤트가 모인 상태 워드를 선언과 동시에 읽는다 */
	u32 dstate, val; /* [한국어] D-state 값과 읽고-고쳐-쓸 임시 값 */

	writel_relaxed(status, pcie_ep->parf + PARF_INT_ALL_CLEAR); /* [한국어] 읽은 값을 그대로 되써서 지운다(R/W1C). 처리 전에 지우므로 처리 중 새로 온 이벤트를 놓치지 않는다 */

	if (FIELD_GET(PARF_INT_ALL_LINK_DOWN, status)) { /* [한국어] 링크가 끊겼으면. 아래가 else-if 사슬이라 여러 비트가 서 있어도 첫 하나만 처리된다 */
		dev_dbg(dev, "Received Linkdown event\n");
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_DOWN; /* [한국어] 상태 기계를 DOWN 으로 옮기고 */
		dw_pcie_ep_linkdown(&pci->ep); /* [한국어] EPF 에 알린다(pcie-designware-ep.c:2848) */
	} else if (FIELD_GET(PARF_INT_ALL_BME, status)) { /* [한국어] 호스트가 버스 마스터를 켰으면 — 열거가 끝나 DMA 를 허용했다는 뜻이다 */
		dev_dbg(dev, "Received Bus Master Enable event\n");
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_ENABLED; /* [한국어] 상태 기계를 ENABLED 로 옮기고 */
		qcom_pcie_ep_icc_update(pcie_ep); /* [한국어] 대역폭을 실제 링크에 맞춘다 */
		pci_epc_bus_master_enable_notify(pci->ep.epc); /* [한국어] EPF 에 알린다(drivers/pci/endpoint/pci-epc-core.c:1475) */
	} else if (FIELD_GET(PARF_INT_ALL_PM_TURNOFF, status)) { /* [한국어] 호스트가 링크를 재우려 하면 */
		dev_dbg(dev, "Received PM Turn-off event! Entering L23\n");
		val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 전력 관리 제어 레지스터를 읽는다 */
		val |= PARF_PM_CTRL_READY_ENTR_L23; /* [한국어] L23 으로 들어갈 준비가 됐다고 응답한다 */
		writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 되쓴다 */
	} else if (FIELD_GET(PARF_INT_ALL_DSTATE_CHANGE, status)) { /* [한국어] 호스트가 이 장치의 D-state 를 바꿨으면 */
		dstate = dw_pcie_readl_dbi(pci, DBI_CON_STATUS) &
				   DBI_CON_STATUS_POWER_STATE_MASK; /* [한국어] config 공간의 전력 상태 필드를 읽는다 */
		dev_dbg(dev, "Received D%d state event\n", dstate); /* [한국어] 어느 상태로 갔는지 남긴다 */
		if (dstate == 3) { /* [한국어] D3 이면 */
			val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 전력 관리 제어 레지스터를 읽고 */
			val |= PARF_PM_CTRL_REQ_EXIT_L1; /* [한국어] L1 에서 빠져나오도록 요청한다. D3 로 들어가는 중에는 링크가 깨어 있어야 하는 것으로 보이나 근거 문서는 이 트리에서 확인 못 함 */
			writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL); /* [한국어] 되쓴다 */
		}
	} else if (FIELD_GET(PARF_INT_ALL_LINK_UP, status)) { /* [한국어] 링크가 올라왔으면 */
		dev_dbg(dev, "Received Linkup event. Enumeration complete!\n");
		dw_pcie_ep_linkup(&pci->ep); /* [한국어] EPF 에 알린다(pcie-designware-ep.c:2812) */
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_UP; /* [한국어] 상태 기계를 UP 으로 옮긴다. 다른 갈래와 달리 알림이 상태 갱신보다 먼저인데, 상류 코드 그대로다 */
	} else {
		dev_WARN_ONCE(dev, 1, "Received unknown event. INT_STATUS: 0x%08x\n",
			      status); /* [한국어] 한 번만 경고한다. 함수 위의 상류 TODO 주석과 함께 이 처리가 완성형이 아님을 보여 준다 */
	}

	return IRQ_HANDLED; /* [한국어] 스레드 핸들러라 늘 처리했다고 알린다 */
}

/* [한국어]
 * qcom_pcie_ep_perst_irq_thread - PERST# 전이를 받아 기동/정지를 수행한다
 *
 * @irq:  인터럽트 번호. 쓰지 않는다.
 * @data: 등록 시 넘긴 EP 컨트롤러 상태.
 * @return: 늘 IRQ_HANDLED.
 *
 * 이 드라이버의 실질적인 제어 흐름이 여기서 시작된다. probe 는 자원을 잡고
 * 기다릴 준비만 하며, 실제 기동은 호스트가 PERST# 를 놓는 순간 이 핸들러가
 * 수행한다.
 *
 * GPIO 값을 직접 읽어 방향을 판정한다. PERST# 가 걸려 있으면(참) 링크를
 * 내리고, 놓여 있으면(거짓) 세운다.
 *
 * 마지막 줄이 이 함수의 요령이다. 처리한 뒤 **인터럽트 트리거 극성을
 * 뒤집는다** — 방금 걸린 것을 처리했으면 다음에는 놓이는 것을 기다리고,
 * 그 반대도 마찬가지다. 레벨 트리거로 양쪽 전이를 모두 잡는 방법이며,
 * 이렇게 하지 않으면 같은 레벨에서 인터럽트가 끝없이 다시 올라온다.
 *
 * 등록 시의 초기 극성이 IRQF_TRIGGER_HIGH 인 것과 짝이 맞는다 — 처음에는
 * 호스트가 PERST# 를 거는 것을 기다린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(스레드 IRQ). 아래로 부르는 함수들이
 * 클록·PHY·regulator 를 만지고 잠들므로 반드시 스레드여야 한다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → qcom_pcie_perst_assert() /
 *               qcom_pcie_perst_deassert()
 */
static irqreturn_t qcom_pcie_ep_perst_irq_thread(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data; /* [한국어] 등록 시 넘긴 EP 상태 */
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] 아래 두 함수에 넘길 DWC 구조체 */
	struct device *dev = pci->dev; /* [한국어] 로그의 기준 */
	u32 perst; /* [한국어] 읽은 PERST# 값 */

	perst = gpiod_get_value(pcie_ep->reset); /* [한국어] GPIO 값을 직접 읽어 방향을 판정한다 */
	if (perst) { /* [한국어] PERST# 가 걸려 있으면 */
		dev_dbg(dev, "PERST asserted by host. Shutting down the PCIe link!\n");
		qcom_pcie_perst_assert(pci); /* [한국어] 링크를 내리고 자원을 끈다 */
	} else {
		dev_dbg(dev, "PERST de-asserted by host. Starting link training!\n");
		qcom_pcie_perst_deassert(pci); /* [한국어] 컨트롤러 전체를 세운다 — 이 드라이버의 실질적 기동이 여기서 일어난다 */
	}

	irq_set_irq_type(gpiod_to_irq(pcie_ep->reset),
			 (perst ? IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW)); /* [한국어] **트리거 극성을 뒤집는다.** 방금 걸린 것을 처리했으면 다음에는 놓이는 것을 기다린다. 이렇게 하지 않으면 같은 레벨에서 인터럽트가 끝없이 다시 올라온다 */

	return IRQ_HANDLED; /* [한국어] 스레드 핸들러라 늘 처리했다고 알린다 */
}

/* [한국어]
 * qcom_pcie_ep_enable_irq_resources - global 과 PERST# 두 인터럽트를 등록한다
 *
 * @pdev:    플랫폼 디바이스.
 * @pcie_ep: EP 컨트롤러 상태.
 * @return: 0 성공, -ENOMEM 은 이름 할당 실패, 그 밖에는 IRQ 등록 오류.
 *
 * 이름을 EPC 의 도메인 번호로 짓는다. EP 컨트롤러가 여럿인 SoC 에서
 * /proc/interrupts 의 이름이 겹치지 않게 하기 위해서다. 그래서 이 함수가
 * dw_pcie_ep_init() 뒤에 불려야 한다 — 그전에는 epc 가 없다.
 *
 *   [global IRQ] DT 에서 "global" 이름으로 찾아 스레드 핸들러로 등록한다.
 *
 *   [PERST# IRQ] GPIO 에서 유도한다. 등록 **전에** IRQ_NOAUTOEN 을 세우는
 *   것이 요점이다 — 그러면 등록해도 자동으로 켜지지 않는다. 실제로 켜는
 *   것은 DWC 코어가 부르는 qcom_pcie_dw_start_link() 이며, 그래서 EP 는
 *   코어가 준비되기 전에 PERST# 를 처리하는 일이 없다.
 *   초기 트리거가 HIGH 인 것은 PERST# 가 걸리는 쪽을 먼저 기다린다는 뜻이다.
 *
 * 둘 다 IRQF_ONESHOT 이라 하드 IRQ 단계에서 마스크된 채 스레드가 돌고,
 * 스레드가 끝나야 풀린다.
 *
 * 두 번째 등록이 실패하면 첫 번째를 disable 한다. free 가 아니라 disable
 * 인데, devm 으로 등록해 해제는 코어가 맡기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_ep_probe() → [이 함수] → devm_request_threaded_irq()
 */
static int qcom_pcie_ep_enable_irq_resources(struct platform_device *pdev,
					     struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = pcie_ep->pci.dev; /* [한국어] 로그와 devm 할당의 기준 */
	char *name; /* [한국어] /proc/interrupts 에 보일 이름 */
	int ret; /* [한국어] 각 단계의 결과 */

	name = devm_kasprintf(dev, GFP_KERNEL, "qcom_pcie_ep_global_irq%d",
			      pcie_ep->pci.ep.epc->domain_nr); /* [한국어] EPC 의 도메인 번호를 붙여 이름을 만든다 — EP 컨트롤러가 여럿인 SoC 에서 겹치지 않게 한다. 그래서 이 함수가 dw_pcie_ep_init() 뒤에 불려야 한다 */
	if (!name) /* [한국어] 이름을 못 만들었으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	pcie_ep->global_irq = platform_get_irq_byname(pdev, "global"); /* [한국어] DT 에서 "global" 이름의 인터럽트를 찾는다 */
	if (pcie_ep->global_irq < 0) /* [한국어] 없으면 */
		return pcie_ep->global_irq; /* [한국어] 그 오류를 그대로 올린다 */

	ret = devm_request_threaded_irq(&pdev->dev, pcie_ep->global_irq, NULL,
					qcom_pcie_ep_global_irq_thread,
					IRQF_ONESHOT,
					name, pcie_ep); /* [한국어] 스레드 핸들러로 등록한다. ONESHOT 이라 스레드가 끝날 때까지 하드 IRQ 단계에서 마스크된다 */
	if (ret) { /* [한국어] 등록에 실패했으면 */
		dev_err(&pdev->dev, "Failed to request Global IRQ\n"); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 올린다 */
	}

	name = devm_kasprintf(dev, GFP_KERNEL, "qcom_pcie_ep_perst_irq%d",
			      pcie_ep->pci.ep.epc->domain_nr); /* [한국어] PERST# 쪽 이름도 같은 방식으로 만든다 */
	if (!name) /* [한국어] 이름을 못 만들었으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	pcie_ep->perst_irq = gpiod_to_irq(pcie_ep->reset); /* [한국어] GPIO 에서 인터럽트 번호를 유도한다 */
	irq_set_status_flags(pcie_ep->perst_irq, IRQ_NOAUTOEN); /* [한국어] **등록 전에** 자동 활성을 막는다 — 실제로 켜는 것은 qcom_pcie_dw_start_link() 이며, 그래야 DWC 코어가 준비되기 전에 PERST# 를 처리하지 않는다 */
	ret = devm_request_threaded_irq(&pdev->dev, pcie_ep->perst_irq, NULL,
					qcom_pcie_ep_perst_irq_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					name, pcie_ep); /* [한국어] 초기 트리거가 HIGH 인 것은 PERST# 가 걸리는 쪽을 먼저 기다린다는 뜻이다 */
	if (ret) { /* [한국어] 등록에 실패했으면 */
		dev_err(&pdev->dev, "Failed to request PERST IRQ\n"); /* [한국어] 알리고 */
		disable_irq(pcie_ep->global_irq); /* [한국어] 앞서 등록한 global 인터럽트를 닫는다. free 가 아니라 disable 인 것은 devm 등록이라 해제를 코어가 맡기 때문이다 */
		return ret; /* [한국어] 그대로 올린다 */
	}

	return 0; /* [한국어] 두 인터럽트가 모두 준비되었다 */
}

/* [한국어]
 * qcom_pcie_ep_raise_irq - EPF 가 요청한 인터럽트를 호스트로 올린다
 *
 * @ep:            DWC EP 구조체.
 * @func_no:       물리 기능 번호.
 * @type:          PCI_IRQ_INTX 또는 PCI_IRQ_MSI.
 * @interrupt_num: MSI 벡터 번호.
 * @return: 0 성공, -EINVAL 은 알 수 없는 종류.
 *
 * struct dw_pcie_ep_ops 의 raise_irq 로 등록되며, 이 파일이 채우는 두 콜백
 * 중 하나다. 두 단 건너뛰기로 도달한다 —
 * EPF → pci_epc_raise_irq()(drivers/pci/endpoint/pci-epc-core.c:484)
 *     → dw_pcie_ep_raise_irq()(pcie-designware-ep.c:1774) → 이 함수.
 *
 * 하는 일은 종류에 따라 DWC 코어의 구현으로 되돌려 보내는 것뿐이다.
 * 퀄컴 고유 처리가 없다는 뜻이며, 그래서 함수가 switch 하나로 끝난다.
 *
 * MSI-X 를 다루지 않는다. 아래 qcom_pcie_epc_features 가 msi_capable 만
 * 켜고 msix 를 켜지 않으므로, EPC 코어가 애초에 MSI-X 요청을 보내지 않는다.
 *
 * 실행 컨텍스트: EPF 드라이버가 부르는 문맥. 잠들지 않는다.
 *
 * 호출 체인:  EPF → pci_epc_raise_irq() → dw_pcie_ep_raise_irq()
 *               → dw_pcie_ep_ops.raise_irq → [이 함수]
 */
static int qcom_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				  unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep); /* [한국어] 오류 로그의 기준을 얻기 위한 DWC 구조체 */

	switch (type) { /* [한국어] 요청한 인터럽트 종류로 갈린다 */
	case PCI_IRQ_INTX: /* [한국어] INTx 요청이면 */
		return dw_pcie_ep_raise_intx_irq(ep, func_no); /* [한국어] DWC 코어의 구현으로 그대로 넘긴다 — 퀄컴 고유 처리가 없다 */
	case PCI_IRQ_MSI: /* [한국어] MSI 요청이면 */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num); /* [한국어] 벡터 번호와 함께 DWC 코어로 넘긴다 */
	default: /* [한국어] 그 밖의 종류이면. MSI-X 가 여기로 오는데, 아래 능력표가 msix 를 켜지 않아 EPC 코어가 애초에 보내지 않는다 */
		dev_err(pci->dev, "Unknown IRQ type\n"); /* [한국어] 알리고 */
		return -EINVAL; /* [한국어] 잘못된 인자로 처리한다 */
	}
}

/* [한국어]
 * qcom_pcie_ep_link_transition_count - 링크 절전 상태 전이 횟수를 debugfs 로 보여 준다
 *
 * @s:    seq_file.
 * @data: 쓰지 않는다.
 * @return: 늘 0.
 *
 * 하드웨어가 세어 둔 카운터 다섯(L0s, L1, L1.1, L1.2, L2)을 그대로 찍는다.
 *
 * pcie-qcom.c 의 같은 이름 함수와 거의 같은 코드다. 다른 점은 읽는 기준
 * 주소로, RC 는 pcie->mhi(별도 MHI 창)를 쓰고 여기서는 pcie_ep->mmio(BAR 로
 * 노출하는 그 창)를 쓴다. 같은 레지스터 오프셋 상수를 두 파일이 각자
 * #define 으로 갖고 있는 것도 그 때문이다 — 창이 달라 공유해도 얻을 것이 없다.
 *
 * s->private 가 device 이고 거기서 drvdata 로 상태를 되찾는 것은
 * debugfs_create_devm_seqfile 의 규약이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 파일을 읽을 때).
 *
 * 호출 체인:  debugfs → [이 함수] → readl_relaxed()
 */
static int qcom_pcie_ep_link_transition_count(struct seq_file *s, void *data)
{
	struct qcom_pcie_ep *pcie_ep = (struct qcom_pcie_ep *)
				     dev_get_drvdata(s->private); /* [한국어] debugfs_create_devm_seqfile 의 규약대로 s->private 가 device 이고 거기서 drvdata 로 상태를 되찾는다 */

	seq_printf(s, "L0s transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L0S)); /* [한국어] L0s 로 들어간 횟수. 기준 주소가 mmio 인 것이 RC 판(mhi 창)과 다르다 */

	seq_printf(s, "L1 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L1)); /* [한국어] L1 로 들어간 횟수 */

	seq_printf(s, "L1.1 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1)); /* [한국어] L1.1 로 들어간 횟수 */

	seq_printf(s, "L1.2 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2)); /* [한국어] L1.2 로 들어간 횟수 */

	seq_printf(s, "L2 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L2)); /* [한국어] L2 로 들어간 횟수 */

	return 0; /* [한국어] seq_file 출력 완료 */
}

/* [한국어]
 * qcom_pcie_ep_init_debugfs - 전이 카운터 파일을 debugfs 에 만든다
 *
 * @pcie_ep: EP 컨트롤러 상태.
 *
 * 디렉터리는 이미 probe 가 만들어 두었고(pcie_ep->debugfs), 여기서는 그
 * 안에 파일 하나를 얹는다. RC 판(qcom_pcie_init_debugfs)이 디렉터리 생성까지
 * 함께 하는 것과 나뉘어 있는데, EP 쪽은 probe 가 이름을 만들고 실패를
 * 처리해야 해서 그렇게 갈린 것으로 보인다.
 *
 * devm 판 seqfile 이라 파일은 device 수명에 묶인다. 디렉터리는
 * qcom_pcie_ep_remove() 가 재귀로 지운다 — RC 판이 디렉터리를 지우지 않는
 * 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  qcom_pcie_ep_probe() → [이 함수]
 */
static void qcom_pcie_ep_init_debugfs(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci; /* [한국어] device 를 얻기 위한 DWC 구조체 */

	debugfs_create_devm_seqfile(pci->dev, "link_transition_count", pcie_ep->debugfs,
				    qcom_pcie_ep_link_transition_count); /* [한국어] probe 가 만들어 둔 디렉터리 안에 카운터 파일을 얹는다. RC 판이 디렉터리 생성까지 함께 하는 것과 나뉘어 있다 */
}

/* [한국어] 이 EP 컨트롤러가 EPF 드라이버에 알리는 능력표.
 * EPF 는 이 표를 보고 어떤 BAR 를 어떤 크기·정렬로 쓸 수 있는지, MSI 를
 * 쓸 수 있는지, 링크업 알림을 받을 수 있는지를 판단한다.
 * 정적 상수 하나뿐이라 SoC 별 차이가 없으며, cfg 표의 플래그들은 EPC 능력이
 * 아니라 eDMA 설정과 PARF 프로그래밍에만 쓰인다. */
static const struct pci_epc_features qcom_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES, /* [한국어] DWC 공통 능력(동적 인바운드 매핑, 서브레인지 매핑)을 한 번에 넣는 매크로 */
	.linkup_notifier = true, /* [한국어] 링크업 알림을 EPF 가 받을 수 있다고 알린다 — 이 파일이 dw_pcie_ep_linkup() 을 부르기 때문이다 */
	.msi_capable = true, /* [한국어] MSI 를 쓸 수 있다. msix 는 켜지 않아 raise_irq 의 default 갈래가 실제로는 오지 않는다 */
	.align = SZ_4K, /* [한국어] BAR 정렬 요구가 4KB 다 */
	.bar[BAR_0] = { .only_64bit = true, }, /* [한국어] BAR0 은 64비트 전용이라 짝을 이루는 BAR1 을 함께 쓴다 */
	.bar[BAR_2] = { .only_64bit = true, }, /* [한국어] BAR2 도 마찬가지다. 그래서 EPF 가 쓸 수 있는 독립 BAR 수가 줄어든다 */
};

/* [한국어]
 * qcom_pcie_epc_get_features - 이 EP 컨트롤러의 능력표를 돌려준다
 *
 * @pci_ep: DWC EP 구조체. 쓰지 않는다.
 * @return: 아래 정적 상수 qcom_pcie_epc_features 의 주소.
 *
 * struct dw_pcie_ep_ops 의 get_features 로 등록되며, 이 파일이 채우는 두
 * 콜백 중 나머지 하나다. 도달 경로도 두 단 건너뛰기다 —
 * EPF → pci_epc_get_features()(drivers/pci/endpoint/pci-epc-core.c:381)
 *     → dw_pcie_ep_get_features()(pcie-designware-ep.c:1882) → 이 함수.
 *
 * 인자를 쓰지 않고 늘 같은 상수를 돌려준다. SoC 마다 능력이 다르지 않다는
 * 뜻이며, cfg 표에 hdma_support 같은 플래그가 있어도 그것은 eDMA 설정에만
 * 쓰이고 EPC 능력에는 반영되지 않는다.
 *
 * EPF 는 이 표를 보고 어떤 BAR 를 어떤 크기·정렬로 쓸 수 있는지, MSI 를
 * 쓸 수 있는지, 링크업 알림을 받을 수 있는지를 판단한다.
 *
 * 실행 컨텍스트: EPF 가 부르는 문맥. 잠들지 않는다.
 *
 * 호출 체인:  EPF → pci_epc_get_features() → dw_pcie_ep_get_features()
 *               → dw_pcie_ep_ops.get_features → [이 함수]
 */
static const struct pci_epc_features *
qcom_pcie_epc_get_features(struct dw_pcie_ep *pci_ep)
{
	return &qcom_pcie_epc_features; /* [한국어] 인자를 쓰지 않고 늘 같은 상수를 돌려준다 — SoC 마다 능력이 다르지 않다는 뜻이다 */
}

static const struct dw_pcie_ep_ops pci_ep_ops = {
	.raise_irq = qcom_pcie_ep_raise_irq, /* [한국어] EPF 가 요청한 INTx/MSI 를 호스트로 올린다 */
	.get_features = qcom_pcie_epc_get_features,
};

/* [한국어]
 * qcom_pcie_ep_probe - 플랫폼 디바이스를 받아 EP 컨트롤러를 등록한다
 *
 * @pdev: DT 가 만든 플랫폼 디바이스.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * RC 판의 probe 와 견주면 이 함수가 **하지 않는 일**이 눈에 띈다. 클록도
 * PHY 도 켜지 않고, PARF 레지스터를 하나도 건드리지 않는다. 그 일은 모두
 * 호스트가 PERST# 를 놓은 뒤 qcom_pcie_perst_deassert() 가 한다. 여기서는
 * "기다릴 준비" 까지만 한다.
 *
 *   1) qcom_pcie_ep 를 잡는다. 첫 필드가 struct dw_pcie 값이라 그 안에
 *      dev 와 두 ops 표를 꽂아 둔다.
 *   2) SoC 표를 얻고, hdma_support 이면 eDMA 설정을 native HDMA 로 바꾼다.
 *      이것이 cfg 표가 실제로 쓰이는 세 자리 중 하나다(나머지 둘은
 *      perst_deassert 안의 패리티 검사와 NO_SNOOP).
 *   3) 런타임 PM 을 켠다. get_noresume + set_active 로 **참조를 든 채**
 *      시작하는 것이 요점이다 — 아래 dw_pcie_ep_init() 이 레지스터에
 *      접근하기 때문이다. 그 참조는 마지막에 put_sync 로 놓아, 링크가
 *      설 때까지 전원 도메인이 잠들 수 있게 한다.
 *   4) 자원을 잡는다.
 *   5) dw_pcie_ep_init()(pcie-designware-ep.c:2981) 으로 EPC 를 만든다.
 *      이제 EPF 드라이버가 붙을 수 있다.
 *   6) 두 인터럽트를 등록한다. epc 의 도메인 번호로 이름을 짓기 때문에
 *      반드시 5) 뒤여야 한다.
 *   7) debugfs 디렉터리와 파일을 만든다.
 *
 * 되돌리기가 라벨 둘이다. err_disable_irqs 가 free 가 아니라 disable 인
 * 것은 devm 등록이라 해제를 코어가 맡기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → qcom_pcie_ep_get_resources()
 *               → dw_pcie_ep_init() → qcom_pcie_ep_enable_irq_resources()
 */
static int qcom_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev; /* [한국어] 로그와 devm 할당의 기준 */
	struct qcom_pcie_ep *pcie_ep; /* [한국어] 이 파일의 EP 상태 */
	char *name; /* [한국어] debugfs 디렉터리 이름 */
	int ret; /* [한국어] 각 단계의 결과 */

	pcie_ep = devm_kzalloc(dev, sizeof(*pcie_ep), GFP_KERNEL); /* [한국어] 상태 구조체를 잡는다. 0 으로 초기화되므로 link_status 가 DISABLED 로 시작한다 */
	if (!pcie_ep) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	pcie_ep->pci.dev = dev; /* [한국어] 첫 필드인 DWC 구조체에 device 를 채운다 */
	pcie_ep->pci.ops = &pci_ops; /* [한국어] 링크 동작 콜백 표를 꽂는다(넷) */
	pcie_ep->pci.ep.ops = &pci_ep_ops; /* [한국어] EP 콜백 표를 꽂는다(둘). 이 둘이 DWC 코어와의 접점 전부다 */

	pcie_ep->cfg = of_device_get_match_data(dev); /* [한국어] SoC 표를 얻는다. 매칭 표의 세 항목은 data 를 주지 않아 NULL 일 수 있다 */
	if (pcie_ep->cfg && pcie_ep->cfg->hdma_support) { /* [한국어] HDMA 를 지원하는 SoC 이면 */
		pcie_ep->pci.edma.ll_wr_cnt = 8; /* [한국어] 쓰기 링크 리스트 채널 수 */
		pcie_ep->pci.edma.ll_rd_cnt = 8; /* [한국어] 읽기 링크 리스트 채널 수 */
		pcie_ep->pci.edma.mf = EDMA_MF_HDMA_NATIVE; /* [한국어] eDMA 를 native HDMA 모드로 바꾼다. cfg 표가 실제로 쓰이는 세 자리 중 하나다 */
	}

	platform_set_drvdata(pdev, pcie_ep); /* [한국어] to_pcie_ep 매크로와 remove 가 되찾을 값을 걸어 둔다 */

	pm_runtime_get_noresume(dev); /* [한국어] **참조를 든 채로** 시작한다 — 아래 dw_pcie_ep_init() 이 레지스터에 접근하기 때문이다 */
	pm_runtime_set_active(dev); /* [한국어] 전원 도메인이 이미 켜져 있다고 코어에 알린다 */
	ret = devm_pm_runtime_enable(dev); /* [한국어] 런타임 PM 을 켠다. devm 판이라 해제를 코어가 맡는다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 그대로 돌아간다 */

	ret = qcom_pcie_ep_get_resources(pdev, pcie_ep); /* [한국어] MMIO 창·GPIO·클록·리셋·PHY·인터커넥트를 잡는다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 그대로 돌아간다 */

	ret = dw_pcie_ep_init(&pcie_ep->pci.ep); /* [한국어] EPC 를 만든다(pcie-designware-ep.c:2981). 이제 EPF 드라이버가 붙을 수 있다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err(dev, "Failed to initialize endpoint: %d\n", ret); /* [한국어] 알리고 */
		return ret; /* [한국어] 그대로 돌아간다 */
	}

	ret = qcom_pcie_ep_enable_irq_resources(pdev, pcie_ep); /* [한국어] 두 인터럽트를 등록한다. 이름을 epc 의 도메인 번호로 짓기 때문에 반드시 위 단계 뒤여야 한다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_ep_deinit; /* [한국어] EPC 를 되돌린다 */

	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node); /* [한국어] debugfs 디렉터리 이름을 DT 노드 경로로 만든다 */
	if (!name) { /* [한국어] 이름을 못 만들었으면 */
		ret = -ENOMEM; /* [한국어] 메모리 부족으로 처리하고 */
		goto err_disable_irqs; /* [한국어] 인터럽트부터 되돌린다 */
	}

	ret = pm_runtime_put_sync(dev); /* [한국어] **참조를 놓는다** — 링크가 설 때까지 전원 도메인이 잠들 수 있게 한다. 실제 기동은 PERST# 인터럽트가 맡는다 */
	if (ret < 0) { /* [한국어] 실패했으면 */
		dev_err(dev, "Failed to suspend device: %d\n", ret); /* [한국어] 알리고 */
		goto err_disable_irqs; /* [한국어] 인터럽트부터 되돌린다 */
	}

	pcie_ep->debugfs = debugfs_create_dir(name, NULL); /* [한국어] 그 이름으로 디렉터리를 만든다 */
	qcom_pcie_ep_init_debugfs(pcie_ep); /* [한국어] 그 안에 카운터 파일을 얹는다 */

	return 0; /* [한국어] 등록 완료. 이제 호스트가 PERST# 를 놓기를 기다린다 */

err_disable_irqs: /* [한국어] 이름 생성/PM 실패가 여기로 온다 */
	disable_irq(pcie_ep->global_irq); /* [한국어] global 인터럽트를 닫고 */
	disable_irq(pcie_ep->perst_irq); /* [한국어] PERST# 인터럽트도 닫는다. devm 등록이라 free 가 아니라 disable 이다 */

err_ep_deinit:
	dw_pcie_ep_deinit(&pcie_ep->pci.ep); /* [한국어] EPC 를 없앤다(pcie-designware-ep.c:2357) */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * qcom_pcie_ep_remove - 인터럽트를 끄고 자원을 놓는다
 *
 * @pdev: 플랫폼 디바이스.
 *
 * 인터럽트를 먼저 끈다. 그러지 않으면 정리 도중 PERST# 나 링크 이벤트가
 * 들어와 이미 놓은 자원을 건드릴 수 있다.
 *
 * debugfs 디렉터리를 재귀로 지운다. probe 가 만든 것의 짝이다.
 *
 * 마지막 조건이 상태 기계를 실제로 활용하는 유일한 자리다 — link_status 가
 * DISABLED 이면 자원이 이미 꺼져 있다는 뜻이라 그냥 돌아간다. 그 값은
 * qcom_pcie_perst_assert() 가 세우거나, 애초에 PERST# 가 한 번도 풀리지
 * 않아 0(=DISABLED) 인 채로 남은 경우다. 두 경우 모두 켠 적이 없거나
 * 이미 끈 상태이므로 두 번 끄지 않는다.
 *
 * RC 판(pcie-qcom.c)에는 remove 자체가 없다. 그쪽 platform_driver 는
 * suppress_bind_attrs 로 언바인드를 막아 두었기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → qcom_pcie_disable_resources()
 */
static void qcom_pcie_ep_remove(struct platform_device *pdev)
{
	struct qcom_pcie_ep *pcie_ep = platform_get_drvdata(pdev); /* [한국어] probe 가 걸어 둔 상태를 되찾는다 */

	disable_irq(pcie_ep->global_irq); /* [한국어] global 인터럽트를 먼저 닫는다 */
	disable_irq(pcie_ep->perst_irq); /* [한국어] PERST# 인터럽트도 닫는다 — 정리 도중 이벤트가 들어와 이미 놓은 자원을 건드리지 않게 한다 */

	debugfs_remove_recursive(pcie_ep->debugfs); /* [한국어] probe 가 만든 디렉터리를 재귀로 지운다 */

	if (pcie_ep->link_status == QCOM_PCIE_EP_LINK_DISABLED) /* [한국어] **상태 기계를 실제로 읽는 유일한 자리**. DISABLED 이면 자원이 이미 꺼져 있거나 한 번도 켠 적이 없다 */
		return; /* [한국어] 두 번 끄지 않는다 */

	qcom_pcie_disable_resources(pcie_ep); /* [한국어] 켜져 있었으면 자원을 놓는다 */
}

/* [한국어] sa8775p 계열의 SoC 표.
 * RC 판이 IP 리비전마다 ops 표를 두는 것과 달리, EP 쪽은 불리언 넷뿐이다.
 * 지원 SoC 가 다섯이고 모두 비슷한 세대라 세대 표 자체가 필요 없었다. */
static const struct qcom_pcie_ep_cfg cfg_1_34_0 = {
	.hdma_support = true, /* [한국어] HDMA 지원 */
	.override_no_snoop = true, /* [한국어] 캐시 스누핑 강제 */
	.disable_mhi_ram_parity_check = true, /* [한국어] MHI RAM 패리티 검사 끄기 */
};

/* [한국어] 위 표에 firmware_managed 만 더한 표.
 * 같은 하드웨어를 펌웨어가 관리하는 플랫폼(sa8255p)용이다. RC 판이 같은
 * 상황에서 ops 를 통째로 비우고 ECAM 갈래로 빠지는 것과 달리, EP 는 같은
 * 코드 경로를 쓰면서 자원 확보·기동만 건너뛴다. */
static const struct qcom_pcie_ep_cfg cfg_1_34_0_fw_managed = {
	.hdma_support = true, /* [한국어] HDMA 지원 */
	.override_no_snoop = true, /* [한국어] 캐시 스누핑 강제 */
	.disable_mhi_ram_parity_check = true, /* [한국어] MHI RAM 패리티 검사 끄기. 여기까지 위와 같다 */
	.firmware_managed = true, /* [한국어] **이 한 줄만 다르다** — 클록·리셋·PHY 를 잡지도 켜지도 않게 한다 */
};

/* [한국어] DT compatible 문자열과 SoC 표를 잇는 매칭 표.
 * 다섯 항목 중 둘만 data 를 갖고 셋은 비어 있다 — 그래서 이 파일의 모든
 * cfg 참조가 `pcie_ep->cfg && ...` 형태로 NULL 을 먼저 확인한다. */
static const struct of_device_id qcom_pcie_ep_match[] = {
	{ .compatible = "qcom,sa8255p-pcie-ep", .data = &cfg_1_34_0_fw_managed}, /* [한국어] 펌웨어가 관리하는 구성 */
	{ .compatible = "qcom,sa8775p-pcie-ep", .data = &cfg_1_34_0}, /* [한국어] 같은 SoC 계열이되 드라이버가 직접 관리한다 */
	{ .compatible = "qcom,sdx55-pcie-ep", }, /* [한국어] data 가 없어 cfg 가 NULL 이다 — 이 파일의 모든 cfg 참조가 NULL 확인을 동반하는 이유다 */
	{ .compatible = "qcom,sm8450-pcie-ep", }, /* [한국어] 같은 이유로 cfg 가 NULL */
	{ .compatible = "qcom,sar2130p-pcie-ep", }, /* [한국어] 같은 이유로 cfg 가 NULL */
	{ } /* [한국어] 표의 끝을 알리는 빈 항목 */
};
MODULE_DEVICE_TABLE(of, qcom_pcie_ep_match); /* [한국어] 매칭 표를 모듈 별칭으로 내보낸다. RC 판에는 이 선언이 없다 */

/* [한국어] 플랫폼 드라이버 등록 정보.
 * RC 판과 달리 remove 를 두고 suppress_bind_attrs 를 켜지 않는다 — EP 는
 * 호스트의 PERST# 로 언제든 꺼질 수 있는 구조라 정리 경로가 필요하다. */
static struct platform_driver qcom_pcie_ep_driver = {
	.probe	= qcom_pcie_ep_probe, /* [한국어] DT 매칭이 성사되면 불린다 */
	.remove = qcom_pcie_ep_remove, /* [한국어] 장치가 사라질 때 불린다. **RC 판에는 이 콜백이 없다** — 그쪽은 suppress_bind_attrs 로 언바인드를 막아 두었기 때문이다 */
	.driver	= {
		.name = "qcom-pcie-ep", /* [한국어] sysfs 등에 보일 드라이버 이름 */
		.of_match_table	= qcom_pcie_ep_match, /* [한국어] 위에서 정의한 compatible 매칭 표 */
	},
};
builtin_platform_driver(qcom_pcie_ep_driver); /* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다 */

MODULE_AUTHOR("Siddartha Mohanadoss <smohanad@codeaurora.org>"); /* [한국어] 원 작성자 */
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>"); /* [한국어] 현재 유지보수자 */
MODULE_DESCRIPTION("Qualcomm PCIe Endpoint controller driver"); /* [한국어] modinfo 에 보일 설명 */
MODULE_LICENSE("GPL v2"); /* [한국어] 라이선스 선언. GPL 전용 심볼을 쓰려면 필요하다 */
