// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe controller driver for Renesas R-Car Gen4 Series SoCs
 * Copyright (C) 2022-2023 Renesas Electronics Corporation
 *
 * The r8a779g0 (R-Car V4H) controller requires a specific firmware to be
 * provided, to initialize the PHY. Otherwise, the PCIe controller will not
 * work.
 */

/*
 * [한국어 설명] Renesas R-Car Gen4 SoC 의 DesignWare PCIe 글루 드라이버 —
 * 호스트(RC)와 엔드포인트(EP)를 한 파일에서 함께 다룬다 (pcie-rcar-gen4.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare(DWC) PCIe 코어를 Renesas R-Car Gen4 계열 SoC 에 붙이는
 * 글루다. 설정공간 접근, iATU(주소 변환), MSI, 버스 스캔은 RC 쪽에서
 * pcie-designware-host.c 가 하고, BAR 관리·인바운드 창·EPC 등록은 EP 쪽에서
 * pcie-designware-ep.c 가 한다. 이 파일이 맡는 것은 그 둘이 알 수 없는
 * R-Car 고유의 것뿐이다.
 *
 * 그 고유한 것이 셋이다. 첫째, "app" 이라는 Renesas 전용 레지스터 창이다 —
 * RC/EP 모드 선택, 레인 분기(bifurcation) 설정, LTSSM 시작·정지, 링크 상태
 * 보고, MSI 인터럽트 신호 인에이블, eDMA 인터럽트 인에이블, 클럭 전원관리가
 * 모두 이 창에 있다. 둘째, "phy" 창이다 — 데이터시트가 레지스터 이름을
 * 밝히지 않아 이 파일에는 생 오프셋 숫자로만 남아 있고, 상류 주석이 그
 * 사정을 그대로 적어 두었다. 셋째, PHY 펌웨어 내려받기다 — 파일 맨 위
 * 상류 주석이 말하는 "specific firmware" 로, request_firmware() 로 받은
 * 이진 파일을 DBI 창의 포트 로직 레지스터 두 개(PRTLGC89/PRTLGC90)를 통해
 * 16비트씩 밀어 넣는다. 이 펌웨어가 없으면 컨트롤러가 동작하지 않는다.
 *
 * 구조의 요점은 RC 와 EP 가 공통 초기화를 한 벌만 공유한다는 것이다.
 * rcar_gen4_pcie_common_init() 이 클럭·전원 리셋·모드 비트·레인 분기까지
 * 다 세우고, 그 위에 모드별로 얇은 층(host_init / ep_pre_init)만 얹는다.
 * 그래서 이 파일을 읽을 때는 "공통 층이 어디까지 하고 모드별 층이 무엇을
 * 더하는가" 를 축으로 보면 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → rcar_gen4_pcie_probe()
 *     → rcar_gen4_pcie_alloc()         : 상태 구조체를 잡고 dw_pcie_ops 를 건다
 *     → rcar_gen4_pcie_get_resources() : "phy" 와 "app" 두 레지스터 창을 얻는다
 *     → rcar_gen4_pcie_prepare()       : 런타임 PM 을 켜고 장치를 깨운다
 *     → rcar_gen4_add_dw_pcie()        : DT match data 의 mode 로 갈라진다
 *         ├ DW_PCIE_RC_TYPE → rcar_gen4_add_dw_pcie_rp() → dw_pcie_host_init()
 *         │    → (콜백) rcar_gen4_pcie_host_init()  : PERST# 를 내렸다 올린다
 *         │    → dw_pcie_setup_rc() → dw_pcie_start_link()
 *         │       → (콜백) rcar_gen4_pcie_start_link() : LTSSM 을 켜고 속도를 올린다
 *         └ DW_PCIE_EP_TYPE → rcar_gen4_add_dw_pcie_ep() → dw_pcie_ep_init()
 *              → (콜백) rcar_gen4_pcie_ep_pre_init() : 공통 초기화 + eDMA 인터럽트
 *              → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 이 파일에는 인터럽트 핸들러가
 * 하나도 없다 — MSI 와 INTx 는 DWC 코어가 다루고, 이 파일은 app 창의
 * 인터럽트 인에이블 비트를 한 번 세워 줄 뿐이다. 대신 잠들 수 있는 경로가
 * 많다: msleep(), usleep_range(), fsleep(), request_firmware(),
 * gpiod_set_value_cansleep() 이 모두 여기서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: RC 모드에서는 PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 *   EP 모드에서는 PCI 엔드포인트 프레임워크(drivers/pci/endpoint)가 EPC 로
 *   등록된 이 컨트롤러 위에 EPF(기능 드라이버)를 붙인다.
 * 아래쪽: pcie-designware.c(공통), pcie-designware-host.c(RC),
 *   pcie-designware-ep.c(EP). 접점은 세 벌의 콜백 표다 —
 *   dw_pcie_ops(start_link/stop_link/link_up),
 *   dw_pcie_host_ops(init/deinit),
 *   dw_pcie_ep_ops(pre_init/raise_irq/get_features/get_dbi_offset/get_dbi2_offset).
 * 옆쪽: 직접 이어지는 드라이버는 없다. 다만 PHY 펌웨어는 request_firmware()
 *   를 통해 유저스페이스 펌웨어 로더에서 들어온다.
 *
 * 데이터 흐름:
 *   디바이스 트리 compatible → of_device_get_match_data() → drvdata
 *     → drvdata->mode 가 RC/EP 갈림의 유일한 근거가 되고,
 *       drvdata->ltssm_control 이 SoC 세대별 LTSSM 절차를 고른다.
 *   "app"/"phy" 자원 → rcar->base / rcar->phy_base → 이 파일의 모든 readl/writel.
 *   펌웨어 파일 → fw->data → PRTLGC89/PRTLGC90 → PHY 내부 SRAM.
 *
 * 공유 상태: struct rcar_gen4_pcie 하나뿐이다. probe 에서 채우고 나면 사실상
 *   불변이며, 이 파일에는 락이 하나도 없다 — 모든 함수가 probe/remove 경로나
 *   DWC 코어의 초기화 콜백에서만 불리기 때문이다.
 *
 * === 주요 함수/구조체 요약 ===
 * rcar_gen4_pcie_common_init()   : RC/EP 공통 초기화. 클럭을 켜고 전원 리셋을
 *                                  걸었다 풀며 PCIEMSR0 에 모드와 레인 분기를 쓴다.
 * rcar_gen4_pcie_start_link()    : LTSSM 을 켜고, 필요한 횟수만큼 속도 변경을
 *                                  수동으로 반복시킨다.
 * rcar_gen4_pcie_speed_change()  : 속도 변경 한 번. 트리거 비트를 세우고
 *                                  하드웨어가 지울 때까지 기다린다.
 * rcar_gen4_pcie_host_init()     : RC 전용. PERST# 를 내렸다 올리고 BAR 두 개를
 *                                  죽이며 MSI 인터럽트 신호를 연다.
 * rcar_gen4_pcie_ep_pre_init()   : EP 전용. 공통 초기화 뒤 eDMA 인터럽트를 연다.
 * rcar_gen4_pcie_ltssm_control() : R-Car V4H 계열의 LTSSM 절차. SRIS 설정,
 *                                  PHY 레지스터 조정, 펌웨어 내려받기가 다 여기 있다.
 * r8a779f0_pcie_ltssm_control()  : R-Car S4 계열의 LTSSM 절차. 비트 하나만 만진다.
 * rcar_gen4_pcie_download_phy_firmware() : PHY 펌웨어를 16비트씩 밀어 넣는다.
 * struct rcar_gen4_pcie          : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태 전부.
 * struct rcar_gen4_pcie_drvdata  : DT compatible 마다 달라지는 것 — 모드와
 *                                  LTSSM 절차, 추가 공통 초기화 훅.
 *
 * === RC 모드와 EP 모드가 나뉘는 지점 ===
 * 공유하는 것: rcar_gen4_pcie_common_init()/deinit(), dw_pcie_ops 세 콜백,
 *   drvdata->ltssm_control(펌웨어 내려받기 포함), "app"/"phy" 창 접근 헬퍼,
 *   probe/remove 의 자원 획득과 런타임 PM.
 * 갈리는 것:
 *   - PERST#: RC 만 dw->pe_rst GPIO 를 흔든다. EP 는 상대(호스트)가 주는
 *     신호를 받는 쪽이므로 만지지 않는다.
 *   - BAR: RC 는 dbi2 로 BAR0/BAR1 을 0 으로 죽여 열거 중 메모리가 배정되지
 *     않게 한다. EP 는 반대로 pci_epc_features 로 BAR 능력을 광고한다.
 *   - 인터럽트: RC 는 PCIEINTSTS0EN 의 MSI_CTRL_INT 를 연다. EP 는
 *     PCIEDMAINTSTSEN 으로 eDMA 인터럽트를 연다.
 *   - 속도 변경 횟수: RC 는 dw_pcie_setup_rc() 가 이미 한 번 걸어 두므로
 *     start_link 에서 한 번을 뺀다. EP 는 빼지 않는다.
 *   - 빌드: CONFIG_PCIE_RCAR_GEN4_HOST 와 CONFIG_PCIE_RCAR_GEN4_EP 가 각각
 *     따로 있고 둘 다 CONFIG_PCIE_RCAR_GEN4 를 select 한다(같은 디렉터리의
 *     Kconfig 에서 확인). 그래서 한쪽만 켜도 이 파일 하나가 빌드되며,
 *     꺼진 쪽 경로는 IS_ENABLED() 검사에서 -ENODEV 로 막힌다.
 */

/* [한국어] msleep()/usleep_range()/fsleep(). PERST# 유지 시간, 속도 변경 재시도
 * 간격, 리셋 래치 대기가 모두 여기서 온다. */
#include <linux/delay.h>
/* [한국어] request_firmware()/release_firmware()와 struct firmware.
 * PHY 펌웨어를 유저스페이스 로더에서 받아 오기 위해 필요하다. */
#include <linux/firmware.h>
/* [한국어] 인터럽트 헤더. 이 파일에는 핸들러가 없지만 상류가 포함해 두었다
 * (전수 확인: request_irq 나 IRQ_HANDLED 가 이 파일에 없다). */
#include <linux/interrupt.h>
/* [한국어] readl()/writel(). app 창과 phy 창은 DBI 가 아니므로 DWC 접근자가
 * 아니라 생 MMIO 접근자를 쓴다. */
#include <linux/io.h>
/* [한국어] readl_poll_timeout(). PHY 초기화 완료 비트를 기다리는 데 한 번 쓴다. */
#include <linux/iopoll.h>
/* [한국어] MODULE_FIRMWARE/MODULE_DESCRIPTION/MODULE_LICENSE 와
 * module_platform_driver() 매크로. */
#include <linux/module.h>
/* [한국어] of_device_get_match_data(). DT compatible 에 딸린 drvdata 를 꺼내
 * RC/EP 갈림을 결정하는 유일한 입구다. */
#include <linux/of.h>
/* [한국어] PCI_BASE_ADDRESS_0/1 같은 설정공간 오프셋 상수와 PCI_IRQ_INTX/
 * PCI_IRQ_MSI 인터럽트 종류 상수. */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_set_drvdata(),
 * devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] 런타임 전원관리. probe 에서 장치를 깨우고 remove 에서 놓아 준다. */
#include <linux/pm_runtime.h>
/* [한국어] reset_control_assert()/deassert()/status(). DWC 코어가 벌크로 얻어 둔
 * core_rsts[] 중 전원 리셋 하나를 이 파일이 직접 조작한다. */
#include <linux/reset.h>

/* [한국어] drivers/pci 안에서만 쓰는 내부 선언. 여기서 필요한 것은
 * PCIE_T_PVPERL_MS(전원 유효 후 PERST# 해제까지의 100ms 규약 시간) 하나다. */
#include "../../pci.h"
/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_ops,
 * dw_pcie_host_ops, dw_pcie_ep_ops, dw_pcie_host_init(), dw_pcie_ep_init(),
 * 그리고 DBI 접근자들. clk.h 와 gpio/consumer.h 도 이 헤더가 끌어온다 —
 * 그래서 이 파일이 직접 포함하지 않고도 clk_bulk_prepare_enable() 과
 * gpiod_set_value_cansleep() 을 쓸 수 있다. */
#include "pcie-designware.h"

/* [한국어] 여기서부터 Renesas 전용 레지스터 정의다. 아래 오프셋들은 두 창에
 * 흩어져 있으니 구분해서 봐야 한다.
 *   - "app" 창(rcar->base): PCIEMSR0, PCIEINTSTS0, PCIEINTSTS0EN,
 *     PCIEDMAINTSTSEN, PCIERSTCTRL1, PCIEPWRMNGCTRL.
 *   - DBI 창(dw_pcie_readl_dbi 로 접근): PRTLGC89, PRTLGC90.
 *   - "phy" 창(rcar->phy_base): 이름 없는 생 오프셋들(0x0f8, 0x148, 0x700 등).
 * 같은 파일 안에 세 창의 오프셋이 섞여 있어서, 접근자가 readl 인지
 * dw_pcie_readl_dbi 인지가 어느 창인지 가려 주는 유일한 단서다. */
/* Renesas-specific */
/* PCIe Mode Setting Register 0 */
/* [한국어] 모드 설정 레지스터. app 창의 맨 앞(오프셋 0)에 있으며,
 * 이 컨트롤러가 RC 로 설지 EP 로 설지가 여기서 결정된다. */
#define PCIEMSR0		0x0000
/* [한국어] SRIS(Separate Reference clock with Independent Spread) 모드.
 * 링크 양쪽이 각자 기준 클럭을 쓰는 구성을 뜻하며, 이때는 레인 간 도착
 * 시간 차를 따로 보정해야 한다. R-Car V4H 계열의 ltssm_control 이 이 비트와
 * DBI 쪽 PORT_FORCE_DO_DESKEW_FOR_SRIS 를 짝지어 세운다. */
#define APP_SRIS_MODE		BIT(6)
/* [한국어] 엔드포인트 모드 = 값 0. 비트가 아니라 "아무 비트도 안 세움" 이다.
 * 그래서 아래 common_init 의 `val |= DEVICE_TYPE_EP` 는 실제로는 아무 일도
 * 하지 않는다. 자세한 것은 그 함수의 [상류 코드 관찰] 을 보라. */
#define DEVICE_TYPE_EP		0
/* [한국어] 루트 컴플렉스 모드 = 비트 4. EP 값(0)과 달리 실제 비트를 세운다. */
#define DEVICE_TYPE_RC		BIT(4)
/* [한국어] 레인 분기(bifurcation) 켜기. 컨트롤러의 레인을 통째로 한 링크에
 * 쓰지 않고 쪼개 쓰는 구성일 때 세운다. common_init 이 num_lanes < 4 일 때
 * 이 비트를 켜는 것이 그 판단 기준이다. */
#define BIFUR_MOD_SET_ON	BIT(0)

/* [한국어] app 창의 인터럽트 상태 레지스터 0. 이름은 인터럽트 상태지만
 * 링크 상태 비트(SMLH/RDLH)도 여기 실려 있어, link_up 콜백이 이것을 읽는다. */
/* PCIe Interrupt Status 0 */
#define PCIEINTSTS0		0x0084

/* [한국어] 위 상태 레지스터의 인에이블 짝. 상태 비트가 실제 인터럽트로
 * 나가려면 여기서 열어 줘야 한다. */
/* PCIe Interrupt Status 0 Enable */
#define PCIEINTSTS0EN		0x0310
/* [한국어] MSI 컨트롤 인터럽트(비트 26). RC 모드에서만 연다 — DWC 코어의
 * MSI 처리기가 인터럽트를 받으려면 이 신호가 SoC 쪽에서 열려 있어야 한다. */
#define MSI_CTRL_INT		BIT(26)
/* [한국어] SMLH(물리 계층 MAC 쪽 상태기계) 링크 업, 비트 7.
 * LTSSM 이 L0 에 이르렀다는 뜻이다. */
#define SMLH_LINK_UP		BIT(7)
/* [한국어] RDLH(데이터 링크 계층) 링크 업, 비트 6. 물리 계층이 서더라도
 * 데이터 링크 계층이 활성이 되기 전에는 설정공간 접근이 통하지 않으므로,
 * link_up 은 이 둘을 함께 본다. */
#define RDLH_LINK_UP		BIT(6)

/* [한국어] eDMA 인터럽트 상태 인에이블. EP 모드에서만 쓴다 — DWC 코어의
 * 내장 DMA 엔진(eDMA)이 완료 인터럽트를 낼 수 있게 여는 문이다. */
/* PCIe DMA Interrupt Status Enable */
#define PCIEDMAINTSTSEN		0x0314
/* [한국어] 초기값으로 하위 16비트를 전부 연다. 채널마다 비트가 하나씩
 * 배정되는 구조라, 채널 수를 따지지 않고 통째로 여는 것이다. */
#define PCIEDMAINTSTSEN_INIT	GENMASK(15, 0)

/* [한국어] 포트 로직 레지스터 89. DBI 창에 있으며, PHY 펌웨어를 밀어 넣을 때
 * "주소" 를 쓰는 창구다. 비트 30 은 그 쓰기가 아직 처리 중인지 알려 준다. */
/* Port Logic Registers 89 */
#define PRTLGC89		0x0b70

/* [한국어] 포트 로직 레지스터 90. PRTLGC89 가 주소라면 이쪽은 "데이터" 다.
 * 확인 단계에서는 비트 0 이 검사 대상이 된다. */
/* Port Logic Registers 90 */
#define PRTLGC90		0x0b74

/* [한국어] 리셋 제어 레지스터 1. LTSSM 시작 비트와 PHY 리셋 유지 비트가
 * 함께 있어, 이 파일의 두 ltssm_control 이 모두 이 레지스터를 만진다. */
/* PCIe Reset Control Register 1 */
#define PCIERSTCTRL1		0x0014
/* [한국어] PHY 리셋을 잡아 둔다(비트 16). 지워야 PHY 가 풀려 초기화를 시작한다. */
#define APP_HOLD_PHY_RST	BIT(16)
/* [한국어] LTSSM 시작(비트 0). 이 비트를 세우는 순간 링크 훈련이 시작된다.
 * 그 전에 모드·SRIS·PHY 설정이 모두 끝나 있어야 한다. */
#define APP_LTSSM_ENABLE	BIT(0)

/* [한국어] 전원관리 제어 레지스터. R-Car V4H 계열의 추가 공통 초기화가
 * 여기에 두 비트를 세운다. */
/* PCIe Power Management Control */
#define PCIEPWRMNGCTRL		0x0070
/* [한국어] CLKREQ# 신호를 애플리케이션 쪽에서 구동한다(비트 11). */
#define APP_CLK_REQ_N		BIT(11)
/* [한국어] 클럭 전원관리 활성(비트 10). 위 비트와 함께 세워 저전력 상태에서
 * 기준 클럭을 멈출 수 있게 한다. */
#define APP_CLK_PM_EN		BIT(10)

/* [한국어] 속도 변경 한 번을 기다리는 최대 재시도 횟수(10회 x 10ms = 약 100ms).
 * 이 횟수를 넘기면 speed_change 가 -ETIMEDOUT 을 돌려준다. */
#define RCAR_NUM_SPEED_CHANGE_RETRIES	10
/* [한국어] 이 컨트롤러가 지원하는 최대 링크 세대(Gen4). start_link 가
 * DT 의 max-link-speed 와 이 값 중 작은 쪽을 골라 속도 변경 횟수를 정한다. */
#define RCAR_MAX_LINK_SPEED		4

/* [한국어] EP 모드에서 함수(function) 번호 하나당 DBI 창이 밀리는 간격.
 * 다기능 엔드포인트에서 func_no 번째 함수의 설정공간이 이만큼 뒤에 있다. */
#define RCAR_GEN4_PCIE_EP_FUNC_DBI_OFFSET	0x1000
/* [한국어] 같은 것의 DBI2(섀도) 창 판. DBI2 는 BAR 마스크처럼 "크기" 를 쓰는
 * 별도 창이라 간격이 DBI 와 다르다. */
#define RCAR_GEN4_PCIE_EP_FUNC_DBI2_OFFSET	0x800

/* [한국어] PHY 펌웨어 파일 이름. 유저스페이스의 /lib/firmware 아래에서 찾는다. */
#define RCAR_GEN4_PCIE_FIRMWARE_NAME		"rcar_gen4_pcie.bin"
/* [한국어] 펌웨어를 밀어 넣을 PHY 내부 SRAM 의 시작 주소. 내려받기 루프가
 * 이 값에 인덱스를 더한 주소를 PRTLGC89 에 쓴다. */
#define RCAR_GEN4_PCIE_FIRMWARE_BASE_ADDR	0xc000
/* [한국어] 이 모듈이 어떤 펌웨어 파일을 필요로 하는지 모듈 메타데이터에
 * 박아 둔다. initramfs 를 만드는 도구가 이 정보를 보고 파일을 함께 넣는다. */
MODULE_FIRMWARE(RCAR_GEN4_PCIE_FIRMWARE_NAME);

/* [한국어] 전방 선언. 바로 아래 drvdata 의 콜백 두 개가 이 타입의 포인터를
 * 인자로 받는데, 정작 struct rcar_gen4_pcie 는 drvdata 포인터를 필드로 갖는다.
 * 서로를 참조하는 순환이라 어느 한쪽을 먼저 완전히 정의할 수 없어, 포인터로만
 * 쓰는 이 단계에서는 불완전 타입으로 통과시킨다. */
struct rcar_gen4_pcie;
/* [한국어] DT compatible 하나마다 달라지는 것들을 모아 둔 표. 이 파일에는
 * 이 구조체의 인스턴스가 넷 있고(r8a779f0 RC/EP, rcar-gen4 RC/EP),
 * of_device_get_match_data() 가 그중 하나를 골라 rcar->drvdata 에 꽂는다.
 * "어느 SoC 세대인가" 와 "RC 인가 EP 인가" 라는 두 축이 여기서 만난다. */
struct rcar_gen4_pcie_drvdata {
	/* [한국어] 공통 초기화 끝머리에 SoC 세대별로 더 할 일이 있으면 거는 훅.
	 * 설정자: 아래 drvdata_rcar_gen4_pcie / _ep 두 표만 이 훅을 채운다
	 *   (rcar_gen4_pcie_additional_common_init). r8a779f0 쪽 두 표는 비워 둔다.
	 * 읽는 자: rcar_gen4_pcie_common_init() 이 마지막에 NULL 검사 후 호출.
	 * 값 범위: 유효한 함수 포인터 또는 NULL. NULL 이면 추가 작업 없음.
	 * 동기화: 정적 상수 표에 들어 있어 런타임에 바뀌지 않는다. */
	void (*additional_common_init)(struct rcar_gen4_pcie *rcar);
	/* [한국어] LTSSM(링크 훈련 상태기계)을 켜고 끄는 SoC 세대별 절차.
	 * 설정자: 네 drvdata 표 모두 채운다 — r8a779f0 두 개는
	 *   r8a779f0_pcie_ltssm_control, rcar-gen4 두 개는 rcar_gen4_pcie_ltssm_control.
	 * 읽는 자: rcar_gen4_pcie_start_link() 이 enable=true 로,
	 *   rcar_gen4_pcie_stop_link() 이 enable=false 로 부른다.
	 * 값 범위: 유효한 함수 포인터 또는 NULL. 두 호출자 모두 NULL 검사를 한다.
	 * 동기화: 정적 상수 표. 불변.
	 *
	 * 두 구현의 무게 차이가 이 파일의 핵심이다. r8a779f0 판은 PCIERSTCTRL1 의
	 * 비트 하나만 만지고 끝나지만, rcar_gen4 판은 SRIS 설정과 PHY 레지스터
	 * 여덟 군데 조정, 그리고 PHY 펌웨어 내려받기까지 한다. */
	int (*ltssm_control)(struct rcar_gen4_pcie *rcar, bool enable);
	/* [한국어] 이 컨트롤러 인스턴스가 RC 로 설지 EP 로 설지.
	 * 설정자: 네 drvdata 표가 각각 DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE 로 못박는다.
	 * 읽는 자: rcar_gen4_pcie_common_init()(PCIEMSR0 모드 비트),
	 *   rcar_gen4_add_dw_pcie()(RC/EP 등록 갈림),
	 *   rcar_gen4_remove_dw_pcie()(해제 갈림),
	 *   rcar_gen4_pcie_start_link()(속도 변경 횟수 보정).
	 * 값 범위: DWC 코어가 정의한 enum dw_pcie_device_mode. 이 파일은 RC/EP 두
	 *   값만 쓰고 나머지는 -EINVAL 로 거른다.
	 * 동기화: 정적 상수 표. 불변이라 어디서 읽어도 같은 값이다. */
	enum dw_pcie_device_mode mode;
};

/* [한국어] 이 드라이버의 상태 전부. 컨트롤러 인스턴스 하나마다 하나씩
 * devm_kzalloc 으로 잡는다. */
struct rcar_gen4_pcie {
	/* [한국어] DWC 코어가 다루는 부분. 맨 앞에 두어 이 구조체의 주소가 그대로
	 * struct dw_pcie 의 주소가 되게 했다 — 그래야 to_rcar_gen4_pcie 매크로의
	 * container_of 가 오프셋 0 으로 돌아온다.
	 * 설정자: rcar_gen4_pcie_alloc() 이 dev/ops/edma.mf 를 채우고,
	 *   나머지는 DWC 코어(dw_pcie_get_resources 등)가 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 거의 모든 함수.
	 * 값 범위: DWC 코어가 정의한 구조체. 이 파일이 직접 보는 필드는
	 *   dev, ops, num_lanes, max_link_speed, core_clks, core_rsts, pe_rst, pp, ep.
	 * 동기화: 코어가 관리한다. 이 파일에는 락이 없다. */
	struct dw_pcie dw;
	/* [한국어] Renesas 전용 "app" 레지스터 창의 가상 주소.
	 * 설정자: rcar_gen4_pcie_get_resources() 의
	 *   devm_platform_ioremap_resource_byname(pdev, "app").
	 * 읽는 자: PCIEMSR0/PCIEINTSTS0/PCIEINTSTS0EN/PCIEDMAINTSTSEN/
	 *   PCIERSTCTRL1/PCIEPWRMNGCTRL 를 만지는 모든 readl/writel.
	 * 값 범위: 유효한 iomem 포인터. devres 가 수명을 관리한다.
	 * 동기화: probe 후 불변. 창 안의 레지스터는 읽기-수정-쓰기로 다루지만
	 *   경합할 다른 컨텍스트가 없어 락을 쓰지 않는다. */
	void __iomem *base;
	/* [한국어] "phy" 레지스터 창의 가상 주소.
	 * 설정자: rcar_gen4_pcie_get_resources() 의 ioremap("phy").
	 * 읽는 자: rcar_gen4_pcie_phy_reg_update_bits() 와,
	 *   rcar_gen4_pcie_ltssm_control() 의 readl_poll_timeout(phy_base + 0x0f8, ...).
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변.
	 *
	 * 이 창의 오프셋에는 이름이 하나도 없다. 상류 주석이 밝히듯 데이터시트가
	 * 레지스터 이름을 적어 두지 않고 초기화 절차만 오프셋으로 제시했기 때문이다. */
	void __iomem *phy_base;
	/* [한국어] 이 컨트롤러의 플랫폼 디바이스.
	 * 설정자: rcar_gen4_pcie_alloc().
	 * 읽는 자: rcar_gen4_pcie_get_resources()(자원 조회)와
	 *   rcar_gen4_add_dw_pcie()(of_device_get_match_data 의 인자).
	 * 값 범위: 유효한 platform_device 포인터. NULL 이 될 수 없다.
	 * 동기화: probe 후 불변.
	 *
	 * dw.dev 와 &pdev->dev 가 같은 장치를 가리키므로 둘 중 하나만 있어도 될 것
	 * 같지만, 자원 조회 API 가 platform_device 를 받기 때문에 따로 들고 있다. */
	struct platform_device *pdev;
	/* [한국어] 이 인스턴스에 매칭된 DT match data.
	 * 설정자: rcar_gen4_add_dw_pcie() 의 of_device_get_match_data().
	 * 읽는 자: common_init/start_link/stop_link/add_dw_pcie/remove_dw_pcie.
	 * 값 범위: 이 파일 아래쪽의 정적 표 넷 중 하나. NULL 이면 -EINVAL 로 실패한다.
	 * 동기화: probe 에서 한 번 채우고 이후 불변.
	 *
	 * 이 필드가 채워지는 시점이 probe 의 한참 뒤(add_dw_pcie)라는 점에 주의해야
	 * 한다. 그보다 앞선 alloc/get_resources/prepare 는 drvdata 를 보지 않는다. */
	const struct rcar_gen4_pcie_drvdata *drvdata;
};
/* [한국어] DWC 코어가 넘겨주는 struct dw_pcie 포인터에서 이 드라이버의 상태를
 * 되찾는 매크로. dw 가 struct rcar_gen4_pcie 의 첫 필드라 실제 오프셋은 0 이지만,
 * 그 사실에 기대지 않고 container_of 를 써서 필드 순서가 바뀌어도 깨지지 않게 했다.
 * DWC 코어의 콜백은 모두 struct dw_pcie 만 받으므로, 이 매크로가 콜백 안에서
 * SoC 상태로 돌아오는 유일한 통로다. */
#define to_rcar_gen4_pcie(_dw)	container_of(_dw, struct rcar_gen4_pcie, dw)

/* Common */
/* [한국어]
 * rcar_gen4_pcie_link_up - 링크가 서 있는지 DWC 코어에 답한다
 *
 * @dw: DWC 코어의 컨트롤러 문맥. 이 파일의 상태는 매크로로 되찾는다.
 * @return: true = 물리 계층과 데이터 링크 계층이 모두 올라왔다, false = 아니다.
 *
 * dw_pcie_ops.link_up 콜백이다. DWC 코어가 링크를 기다릴 때
 * (dw_pcie_wait_for_link) 반복해서 부르고, 설정공간 접근 전에도 확인한다.
 *
 * 판정은 app 창의 PCIEINTSTS0 한 번 읽기로 끝난다. DBI 쪽 LTSSM 상태를
 * 뒤지지 않아도 되는 것은, Renesas 가 그 두 상태를 이 레지스터의 비트로
 * 뽑아 두었기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화 경로). 잠들지 않으며 부수효과도 없다.
 *
 * 에러 경로: 없다. 읽기 실패라는 개념이 없는 MMIO 읽기 하나뿐이다.
 *
 * 호출 체인:
 *   dw_pcie_link_up() / dw_pcie_wait_for_link() → [이 함수] → readl()
 */
static bool rcar_gen4_pcie_link_up(struct dw_pcie *dw)
{
	/* [한국어] 콜백은 struct dw_pcie 만 받으므로 container_of 로 SoC 상태를 되찾는다.
	 * rcar->base(app 창)가 필요해서다. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);
	/* [한국어] val 은 읽어 온 상태 레지스터 값, mask 는 그중 봐야 할 두 비트. */
	u32 val, mask;

	/* [한국어] app 창의 인터럽트 상태 0 을 읽는다. DBI 가 아니라 Renesas 전용
	 * 창이므로 dw_pcie_readl_dbi 가 아니라 생 readl 이다. */
	val = readl(rcar->base + PCIEINTSTS0);
	/* [한국어] 물리 계층(SMLH)과 데이터 링크 계층(RDLH) 두 비트를 함께 본다.
	 * 물리 계층만 서고 데이터 링크가 아직이면 설정공간 접근이 통하지 않으므로,
	 * 한쪽만 보고 링크 업이라고 답하면 안 된다. */
	mask = RDLH_LINK_UP | SMLH_LINK_UP;

	/* [한국어] 부분 일치가 아니라 두 비트가 모두 서 있을 때만 참이다.
	 * (val & mask) 를 그대로 반환하면 한쪽만 선 중간 상태도 참이 되어 버린다. */
	return (val & mask) == mask;
}

/*
 * Manually initiate the speed change. Return 0 if change succeeded; otherwise
 * -ETIMEDOUT.
 */
/* [한국어]
 * rcar_gen4_pcie_speed_change - 링크 속도 변경을 한 번 지시하고 끝날 때까지 기다린다
 *
 * @dw: DWC 코어의 컨트롤러 문맥. DBI 접근만 쓰므로 SoC 상태는 필요 없다.
 * @return: 0 = 변경이 끝났다, -ETIMEDOUT = 재시도 횟수를 다 써도 끝나지 않았다.
 *
 * 바로 위 상류 주석이 반환값의 뜻을 그대로 적어 두었다.
 *
 * DWC 의 PORT_LOGIC_SPEED_CHANGE(Directed Speed Change) 비트는 소프트웨어가
 * 세우면 하드웨어가 속도 재협상을 시작하고, 끝나면 스스로 지우는 성격이다.
 * 그래서 절차가 "0 으로 지웠다가 → 1 로 세우고 → 0 이 될 때까지 폴링" 이 된다.
 * 먼저 지우는 것은 이전 요청의 잔상이 남아 있을 때 새 요청이 무시되지 않게
 * 하기 위한 것이다.
 *
 * 왜 한 번으로 끝나지 않는가: 이 하드웨어는 한 번의 지시로 한 세대씩만
 * 올라간다. 그래서 호출자인 start_link 가 목표 세대까지 이 함수를 반복해 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range() 로 잠들 수 있다.
 *
 * 에러 경로: 폴링이 끝나도 비트가 남아 있으면 -ETIMEDOUT. 호출자는 그것을
 * 치명적 실패로 보지 않고 반복을 중단하기만 한다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_start_link() → [이 함수]
 *     → dw_pcie_readl_dbi(), dw_pcie_writel_dbi(), usleep_range()
 */
static int rcar_gen4_pcie_speed_change(struct dw_pcie *dw)
{
	/* [한국어] 레지스터 읽기-수정-쓰기에 쓸 임시 값. */
	u32 val;
	/* [한국어] 폴링 반복 카운터. */
	int i;

	/* [한국어] 링크 폭/속도 제어 레지스터를 읽는다. 다른 비트를 보존해야 하므로
	 * 통째로 읽어 두고 필요한 비트만 손댄다. */
	val = dw_pcie_readl_dbi(dw, PCIE_LINK_WIDTH_SPEED_CONTROL);
	/* [한국어] 속도 변경 트리거를 일단 지운다. 이전 요청의 잔상이 남아 있으면
	 * 다음의 세우기가 상승 에지로 인식되지 않을 수 있다. */
	val &= ~PORT_LOGIC_SPEED_CHANGE;
	/* [한국어] 지운 상태를 실제 레지스터에 반영한다. */
	dw_pcie_writel_dbi(dw, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

	/* [한국어] 다시 읽는다. 위 쓰기가 반영된 값을 기준으로 삼기 위해서다. */
	val = dw_pcie_readl_dbi(dw, PCIE_LINK_WIDTH_SPEED_CONTROL);
	/* [한국어] 이번에는 트리거를 세운다. 이 순간 하드웨어가 속도 재협상을 시작한다. */
	val |= PORT_LOGIC_SPEED_CHANGE;
	/* [한국어] 트리거를 실제로 건다. */
	dw_pcie_writel_dbi(dw, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

	/* [한국어] 최대 RCAR_NUM_SPEED_CHANGE_RETRIES(10)회까지 완료를 기다린다. */
	for (i = 0; i < RCAR_NUM_SPEED_CHANGE_RETRIES; i++) {
		/* [한국어] 매 회 레지스터를 다시 읽어 트리거 비트의 현재 상태를 본다. */
		val = dw_pcie_readl_dbi(dw, PCIE_LINK_WIDTH_SPEED_CONTROL);
		/* [한국어] 하드웨어가 비트를 스스로 지웠다 = 속도 변경이 끝났다. */
		if (!(val & PORT_LOGIC_SPEED_CHANGE))
			return 0;
		/* [한국어] 아직이면 10~11ms 쉬었다 다시 본다. usleep_range 는 잠들 수 있으므로
		 * 이 함수가 프로세스 컨텍스트 전용임을 못박는 지점이기도 하다. */
		usleep_range(10000, 11000);
	}

	/* [한국어] 열 번을 다 쓰고도 비트가 남아 있다. 호출자는 이 값을 보고 더
	 * 올리기를 포기한다(EP 모드에서는 아직 상대가 없을 수 있어 정상 상황이다). */
	return -ETIMEDOUT;
}

/*
 * Enable LTSSM of this controller and manually initiate the speed change.
 * Always return 0.
 */
/* [한국어]
 * rcar_gen4_pcie_start_link - LTSSM 을 켜고 목표 세대까지 속도를 끌어올린다
 *
 * @dw: DWC 코어의 컨트롤러 문맥.
 * @return: 0 이 기본. 다만 ltssm_control 훅이 실패하면 그 값을 그대로 올린다.
 *
 * dw_pcie_ops.start_link 콜백이다. RC 모드에서는 dw_pcie_host_init() 이
 * dw_pcie_setup_rc() 를 마친 뒤 링크가 아직 서 있지 않을 때 부르고,
 * EP 모드에서는 EPC 쪽 start 요청이 이어진다.
 *
 * 하는 일이 둘이다. 첫째, SoC 세대별 LTSSM 절차를 호출한다 — 이 안에서
 * PHY 펌웨어 내려받기까지 일어날 수 있다. 둘째, 목표 세대에 이를 때까지
 * 속도 변경을 손으로 반복시킨다.
 *
 * 반복 횟수 계산이 이 함수의 요점이다. Gen4 까지 올리려면 Gen1 에서 세 번을
 * 올려야 하므로 (세대 - 1) 번이 필요하고, RC 모드에서는 dw_pcie_setup_rc()
 * 가 이미 트리거를 한 번 걸어 두므로 거기서 한 번을 더 뺀다. 상류 주석 둘이
 * 그 두 가지 사정을 각각 적어 두었다.
 *
 * [상류 코드 관찰] 바로 위 상류 주석은 "Always return 0" 이라고 적혀 있지만,
 * 실제로는 ltssm_control 훅이 실패하면 그 오류를 그대로 반환한다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. 주석이 코드보다
 * 오래된 것으로 보인다.
 *
 * [상류 코드 관찰] dw->max_link_speed 는 DT 에 max-link-speed 가 없으면
 * of_pci_get_max_link_speed() 가 돌려준 음수 errno 가 그대로 들어 있다
 * (pcie-designware.c 의 dw_pcie_get_resources 에서 확인). 그 경우
 * min_not_zero 는 음수를 고르고 changes 도 음수가 되어 아래 루프가 한 번도
 * 돌지 않는다. 즉 DT 가 속도를 지정하지 않으면 수동 속도 변경은 통째로
 * 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 하위에서 잠들 수 있다(펌웨어 로드 포함).
 *
 * 에러 경로: ltssm_control 실패만 위로 전한다. 속도 변경 실패는 루프를
 * 끊기만 하고 오류로 보지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_start_link() → [이 함수]
 *     → drvdata->ltssm_control(), rcar_gen4_pcie_speed_change()
 */
static int rcar_gen4_pcie_start_link(struct dw_pcie *dw)
{
	/* [한국어] app 창과 drvdata 가 필요하므로 SoC 상태를 되찾는다. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);
	/* [한국어] i 는 루프 인덱스, changes 는 남은 속도 변경 횟수, ret 는 훅의 결과. */
	int i, changes, ret;

	/* [한국어] 네 drvdata 표 모두 이 훅을 채우고 있지만, 구조체상 NULL 일 수 있으므로
	 * 검사한다. 여기서 실제 LTSSM 이 켜지고 rcar_gen4 계열에서는 PHY 펌웨어까지 올라간다. */
	if (rcar->drvdata->ltssm_control) {
		/* [한국어] enable=true 로 부른다. 세대에 따라 비트 하나일 수도, 펌웨어
		 * 내려받기를 포함한 긴 절차일 수도 있다. */
		ret = rcar->drvdata->ltssm_control(rcar, true);
		/* [한국어] LTSSM 을 켜지 못했으면 속도를 논할 단계가 아니다. */
		if (ret)
			/* [한국어] 실패를 그대로 올린다 — 위의 [상류 코드 관찰] 이 가리키는 지점이다. */
			return ret;
	}

	/*
	 * Require direct speed change with retrying here if the max_link_speed
	 * is PCIe Gen2 or higher.
	 */
	/* [한국어] 목표 세대를 정한다. DT 값과 하드웨어 상한(Gen4) 중 작은 쪽을 고르되,
	 * DT 값이 0(미지정)이면 하드웨어 상한을 쓴다. Gen1 에서 시작하므로 필요한
	 * 변경 횟수는 세대 수보다 하나 적다. */
	changes = min_not_zero(dw->max_link_speed, RCAR_MAX_LINK_SPEED) - 1;

	/*
	 * Since dw_pcie_setup_rc() sets it once, PCIe Gen2 will be trained.
	 * So, this needs remaining times for up to PCIe Gen4 if RC mode.
	 */
	/* [한국어] RC 모드에서는 dw_pcie_setup_rc() 가 이미 트리거를 한 번 걸어 두었다
	 * (pcie-designware-host.c 에서 PORT_LOGIC_SPEED_CHANGE 를 세우는 것을 확인).
	 * 그래서 그 한 번을 빼야 총 횟수가 맞는다. EP 모드에는 그 사전 트리거가 없다.
	 * changes 가 0 이하이면 뺄 것도 없으므로 조건에 changes 를 함께 본다. */
	if (changes && rcar->drvdata->mode == DW_PCIE_RC_TYPE)
		/* [한국어] 이미 걸린 한 번을 제한다. */
		changes--;

	/* [한국어] 남은 횟수만큼 한 세대씩 올린다. */
	for (i = 0; i < changes; i++) {
		/* It may not be connected in EP mode yet. So, break the loop */
		/* [한국어] 한 번 실패하면 더 시도해도 소용이 없다. EP 모드에서는 아직 상대가
		 * 붙지 않아 실패하는 것이 정상이라는 것이 바로 위 상류 주석의 취지다. */
		if (rcar_gen4_pcie_speed_change(dw))
			break;
	}

	/* [한국어] 속도 변경이 몇 번 실패했든 링크 시작 자체는 성공으로 보고한다.
	 * 실제 링크 성립 여부는 DWC 코어가 link_up 콜백을 폴링해 판정한다. */
	return 0;
}

/* [한국어]
 * rcar_gen4_pcie_stop_link - LTSSM 을 꺼서 링크를 내린다
 *
 * @dw: DWC 코어의 컨트롤러 문맥.
 * @return: 없음.
 *
 * dw_pcie_ops.stop_link 콜백이다. 호스트 초기화가 실패했을 때의 되감기와
 * dw_pcie_host_deinit() 경로에서 불린다.
 *
 * start_link 와 짝이지만 훨씬 짧다. 속도 변경 같은 절차가 없고, 세대별 훅에
 * enable=false 를 넘겨 LTSSM 비트를 지우게 하는 것이 전부다.
 *
 * [상류 코드 관찰] 훅의 반환값을 확인하지 않는다. 반환 타입이 void 라 올릴
 * 곳이 없기 때문이며, 원본 스냅숏(1f0e418bb6)에서도 같다. 실제로 두 구현 모두
 * enable=false 경로에서는 항상 0 을 돌려주므로 잃는 정보는 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다(disable 경로는 MMIO 쓰기뿐).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_stop_link() → [이 함수] → drvdata->ltssm_control(rcar, false)
 */
static void rcar_gen4_pcie_stop_link(struct dw_pcie *dw)
{
	/* [한국어] drvdata 에 닿기 위해 SoC 상태를 되찾는다. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);

	/* [한국어] 훅이 없는 표는 이 파일에 없지만, 구조체상 NULL 이 가능하므로 검사한다. */
	if (rcar->drvdata->ltssm_control)
		/* [한국어] enable=false — 두 구현 모두 PCIERSTCTRL1 의 APP_LTSSM_ENABLE 만 지운다. */
		rcar->drvdata->ltssm_control(rcar, false);
}

/* [한국어]
 * rcar_gen4_pcie_common_init - RC/EP 가 공유하는 하드웨어 초기화
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 0 성공. 클럭 실패나 알 수 없는 모드면 음수.
 *
 * 이 파일에서 RC 와 EP 가 만나는 유일한 지점이다. RC 는
 * rcar_gen4_pcie_host_init() 에서, EP 는 rcar_gen4_pcie_ep_pre_init() 에서
 * 각각 이 함수를 부른다.
 *
 * 절차의 순서가 전부다.
 *   1. 코어 클럭 넷(pipe/core/aux/ref)을 벌크로 켠다. 클럭 없이 레지스터를
 *      만지면 접근 자체가 걸린다.
 *   2. 전원 리셋이 아직 풀려 있으면 다시 건다. HSC 도메인 규칙상 리셋을 건
 *      뒤 1ms 를 반드시 기다려야 한다(상류 주석이 매뉴얼 쪽수까지 적어 두었다).
 *   3. 리셋을 건 상태에서 PCIEMSR0 에 모드(RC/EP)와 레인 분기를 쓴다.
 *      이 값은 리셋이 풀릴 때 하드웨어가 읽어 가는 것이라 순서가 중요하다.
 *   4. 전원 리셋을 푼다.
 *   5. 리셋 상태를 되읽고 1ms 더 기다린다 — 비동기 리셋을 동기 리셋처럼
 *      만들어 DBI 접근이 SError 를 내지 않게 하려는 것이다(상류 주석).
 *   6. 세대별 추가 초기화 훅이 있으면 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. fsleep() 로 잠든다.
 *
 * 에러 경로: 클럭 실패는 그대로 반환(되감을 것 없음). 모드가 RC/EP 가 아니거나
 * 리셋 해제가 실패하면 err_unprepare 로 가서 방금 켠 클럭을 되돌린다.
 * 다만 그 경로는 이미 건 리셋을 풀지 않는다는 점에 유의할 것.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_host_init() / rcar_gen4_pcie_ep_pre_init() → [이 함수]
 *     → clk_bulk_prepare_enable(), reset_control_status/assert/deassert(),
 *       fsleep(), drvdata->additional_common_init()
 */
static int rcar_gen4_pcie_common_init(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] 클럭·리셋 배열과 num_lanes 가 DWC 쪽에 있어 지역 별칭을 둔다. */
	struct dw_pcie *dw = &rcar->dw;
	/* [한국어] PCIEMSR0 읽기-수정-쓰기용 임시 값. */
	u32 val;
	/* [한국어] 하위 호출들의 결과. */
	int ret;

	/* [한국어] 코어 클럭 넷(pipe/core/aux/ref)을 한 번에 prepare+enable 한다.
	 * 이 배열은 DWC 코어가 REQ_RES capability 를 보고 미리 얻어 둔 것이다 —
	 * rcar_gen4_pcie_alloc() 이 그 capability 를 켜기 때문에 여기 값이 들어 있다. */
	ret = clk_bulk_prepare_enable(DW_PCIE_NUM_CORE_CLKS, dw->core_clks);
	/* [한국어] 클럭을 못 켜면 이후 레지스터 접근이 모두 무의미하다. */
	if (ret) {
		/* [한국어] 어느 클럭인지까지는 벌크 API 가 알려 주지 않으므로 뭉뚱그려 알린다. */
		dev_err(dw->dev, "Enabling core clocks failed\n");
		/* [한국어] 아직 아무것도 잡지 않았으므로 되감을 것이 없다. */
		return ret;
	}

	/* [한국어] 전원 리셋이 "걸려 있지 않다"(status 가 0)면 지금 걸어 둔다.
	 * 아래에서 모드 비트를 쓴 뒤 풀어야 하므로, 시작 상태를 리셋이 걸린 쪽으로
	 * 맞추는 것이다. 이미 걸려 있으면 다시 걸 필요가 없어 이 블록을 건너뛴다. */
	if (!reset_control_status(dw->core_rsts[DW_PCIE_PWR_RST].rstc)) {
		/* [한국어] 전원 리셋을 건다. core_rsts[] 는 DWC 코어가 벌크로 얻어 둔 배열이고
		 * 그중 DW_PCIE_PWR_RST 하나만 이 파일이 직접 다룬다. */
		reset_control_assert(dw->core_rsts[DW_PCIE_PWR_RST].rstc);
		/*
		 * R-Car V4H Reference Manual R19UH0186EJ0130 Rev.1.30 Apr.
		 * 21, 2025 page 585 Figure 9.3.2 Software Reset flow (B)
		 * indicates that for peripherals in HSC domain, after
		 * reset has been asserted by writing a matching reset bit
		 * into register SRCR, it is mandatory to wait 1ms.
		 */
		/* [한국어] 위 상류 주석이 인용한 매뉴얼 규칙대로 1ms 를 반드시 쉰다.
		 * fsleep 은 길이에 따라 udelay/usleep_range 를 알아서 고르는 헬퍼다. */
		fsleep(1000);
	}

	/* [한국어] 모드 설정 레지스터를 읽는다. 다른 비트를 보존하기 위한 읽기-수정-쓰기다. */
	val = readl(rcar->base + PCIEMSR0);
	/* [한국어] drvdata 가 정한 모드에 따라 갈린다. 이 갈림이 같은 파일 하나로
	 * RC 와 EP 를 모두 지원하게 하는 핵심이다. */
	if (rcar->drvdata->mode == DW_PCIE_RC_TYPE) {
		/* [한국어] 루트 컴플렉스: 비트 4 를 세운다. */
		val |= DEVICE_TYPE_RC;
	/* [한국어] 엔드포인트 모드. */
	} else if (rcar->drvdata->mode == DW_PCIE_EP_TYPE) {
		/* [한국어] [상류 코드 관찰] DEVICE_TYPE_EP 가 0 이므로 이 OR 는 아무 비트도
		 * 바꾸지 않는다. 즉 리셋 직후 PCIEMSR0 에 DEVICE_TYPE_RC 가 서 있었다면
		 * EP 모드에서도 그대로 남는다 — 코드가 그 비트를 지우지 않기 때문이다.
		 * 원본 스냅숏(1f0e418bb6)에서 확인했고 코드는 손대지 않았다. 실제로는
		 * 바로 위에서 전원 리셋을 걸어 레지스터가 기본값으로 돌아온 상태이므로
		 * 문제가 드러나지 않는 것으로 보이나, 그 전제는 이 파일에 적혀 있지 않다. */
		val |= DEVICE_TYPE_EP;
	/* [한국어] RC 도 EP 도 아닌 모드는 이 드라이버가 다루지 않는다. */
	} else {
		/* [한국어] 잘못된 DT match data 이므로 인자 오류로 처리한다. */
		ret = -EINVAL;
		/* [한국어] 방금 켠 클럭을 되돌리러 간다. */
		goto err_unprepare;
	}

	/* [한국어] 레인이 4 개 미만이면 컨트롤러의 레인을 쪼개 쓰는 구성이다.
	 * num_lanes 는 DWC 코어가 DT 의 "num-lanes" 에서 읽어 둔 값이다. */
	if (dw->num_lanes < 4)
		/* [한국어] 레인 분기 비트를 켠다. */
		val |= BIFUR_MOD_SET_ON;

	/* [한국어] 모드와 분기 설정을 실제 레지스터에 반영한다. 아직 전원 리셋이
	 * 걸린 상태여야 하며, 그래야 리셋 해제 시 하드웨어가 이 값을 집어 간다. */
	writel(val, rcar->base + PCIEMSR0);

	/* [한국어] 전원 리셋을 푼다. 여기서부터 컨트롤러가 살아난다. */
	ret = reset_control_deassert(dw->core_rsts[DW_PCIE_PWR_RST].rstc);
	/* [한국어] 리셋 해제 실패. */
	if (ret)
		/* [한국어] 클럭만 되돌리고 나간다. */
		goto err_unprepare;

	/*
	 * Assure the reset is latched and the core is ready for DBI access.
	 * On R-Car V4H, the PCIe reset is asynchronous and does not take
	 * effect immediately, but needs a short time to complete. In case
	 * DBI access happens in that short time, that access generates an
	 * SError. To make sure that condition can never happen, read back the
	 * state of the reset, which should turn the asynchronous reset into
	 * synchronous one, and wait a little over 1ms to add additional
	 * safety margin.
	 */
	/* [한국어] 위 상류 주석대로, 되읽기로 비동기 리셋을 동기화한다. 반환값을
	 * 쓰지 않는 것이 요점이며, 목적은 값이 아니라 "읽기가 완료될 때까지 기다린다"
	 * 는 부수효과 자체다. */
	reset_control_status(dw->core_rsts[DW_PCIE_PWR_RST].rstc);
	/* [한국어] 안전 여유로 1ms 를 더 쉰다. 이 대기 없이 DBI 에 접근하면
	 * SError 가 날 수 있다는 것이 위 상류 주석의 경고다. */
	fsleep(1000);

	/* [한국어] 세대별 추가 초기화. rcar_gen4 계열만 이 훅을 채우고,
	 * r8a779f0 계열은 비워 두어 여기를 그냥 지나간다. */
	if (rcar->drvdata->additional_common_init)
		/* [한국어] 레인 skew 보정과 클럭 전원관리 비트를 세운다. */
		rcar->drvdata->additional_common_init(rcar);

	/* [한국어] 여기까지 오면 RC/EP 공통 하드웨어가 준비된 것이다. */
	return 0;

/* [한국어] 클럭을 켠 뒤 실패한 경로가 모이는 곳. */
err_unprepare:
	/* [한국어] 켠 순서의 역순으로 되돌린다. [상류 코드 관찰] 이 경로는 위에서
	 * 건 전원 리셋을 풀지 않는다. 원본 스냅숏에서 확인했으며 코드는 손대지 않았다. */
	clk_bulk_disable_unprepare(DW_PCIE_NUM_CORE_CLKS, dw->core_clks);

	/* [한국어] 실패 원인을 그대로 호출자에게 올린다. */
	return ret;
}

/* [한국어]
 * rcar_gen4_pcie_common_deinit - common_init 이 켠 것을 되돌린다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * RC 는 rcar_gen4_pcie_host_deinit() 에서, EP 는 rcar_gen4_pcie_ep_deinit()
 * 에서 부른다. common_init 과 정확히 짝을 이루는 되감기이며, 순서도 역순이다 —
 * 리셋을 먼저 걸어 컨트롤러를 멈춘 뒤 클럭을 끈다. 반대로 하면 클럭이 없는
 * 상태에서 리셋 레지스터를 만지게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(remove 또는 초기화 실패 되감기).
 *
 * 에러 경로: 없다. 되감기 경로라 실패해도 할 수 있는 일이 없으므로 반환값을
 * 보지 않는다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_host_deinit() / rcar_gen4_pcie_ep_deinit() → [이 함수]
 *     → reset_control_assert(), clk_bulk_disable_unprepare()
 */
static void rcar_gen4_pcie_common_deinit(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] 클럭/리셋 배열이 DWC 쪽에 있어 지역 별칭을 둔다. */
	struct dw_pcie *dw = &rcar->dw;

	/* [한국어] 전원 리셋을 걸어 컨트롤러를 정지시킨다. 클럭을 끄기 전에 해야 한다. */
	reset_control_assert(dw->core_rsts[DW_PCIE_PWR_RST].rstc);
	/* [한국어] 코어 클럭 넷을 한꺼번에 끈다. */
	clk_bulk_disable_unprepare(DW_PCIE_NUM_CORE_CLKS, dw->core_clks);
}

/* [한국어]
 * rcar_gen4_pcie_prepare - 런타임 전원관리를 켜고 장치를 깨운다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 0 이상이면 성공(pm_runtime_resume_and_get 의 반환 규약),
 *          음수면 실패이며 이때는 이미 pm_runtime_disable 까지 되돌려 두었다.
 *
 * probe 가 자원을 얻은 직후 부른다. 이 SoC 에서 PCIe 블록의 전원 도메인은
 * 런타임 PM 이 관리하므로, 레지스터를 만지기 전에 장치를 깨워 두어야 한다.
 * 클럭과 리셋은 그다음 단계인 common_init 이 따로 다룬다.
 *
 * pm_runtime_resume_and_get() 을 쓰는 것이 요점이다. 이 헬퍼는 실패하면
 * 사용 카운트를 스스로 되돌려 주므로, 호출자가 put 을 짝 맞춰 줄 필요가 없다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 실패하면 여기서 pm_runtime_disable() 까지 마치고 오류를 올린다.
 * 그래서 probe 는 곧바로 return err 만 하면 된다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_probe() → [이 함수]
 *     → pm_runtime_enable(), pm_runtime_resume_and_get(), pm_runtime_disable()
 */
static int rcar_gen4_pcie_prepare(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] PM API 는 struct device 를 받으므로 미리 꺼내 둔다. */
	struct device *dev = rcar->dw.dev;
	/* [한국어] resume 결과. 0 이상이 성공이다. */
	int err;

	/* [한국어] 이 장치에 런타임 PM 을 허용한다. 이 호출 전에는 resume 요청이
	 * 의미를 갖지 않는다. */
	pm_runtime_enable(dev);
	/* [한국어] 장치를 깨우고 사용 카운트를 올린다. 실패 시 카운트를 스스로
	 * 되돌려 주는 판이라 호출자가 put 을 신경 쓰지 않아도 된다. */
	err = pm_runtime_resume_and_get(dev);
	/* [한국어] 음수면 전원 도메인을 켜지 못한 것이다. */
	if (err < 0) {
		/* [한국어] 어느 단계에서 막혔는지 알 수 없으므로 뭉뚱그려 알린다. */
		dev_err(dev, "Runtime resume failed\n");
		/* [한국어] 방금 켠 런타임 PM 을 되돌린다. 이것까지 해 두어야 호출자가
		 * 추가 정리 없이 그대로 반환할 수 있다. */
		pm_runtime_disable(dev);
	}

	/* [한국어] 성공값(0 또는 양수)을 그대로 올린다. probe 는 err 만 보면 된다. */
	return err;
}

/* [한국어]
 * rcar_gen4_pcie_unprepare - prepare 가 켠 런타임 PM 을 되돌린다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * probe 의 실패 되감기와 remove 양쪽에서 불린다. prepare 와 정확히 역순이다 —
 * 사용 카운트를 내려 장치가 잠들 수 있게 한 뒤 런타임 PM 자체를 끈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 없다. 되감기라 반환값을 볼 이유가 없다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_probe()(실패 시) / rcar_gen4_pcie_remove() → [이 함수]
 *     → pm_runtime_put(), pm_runtime_disable()
 */
static void rcar_gen4_pcie_unprepare(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] PM API 가 받는 struct device 를 꺼낸다. */
	struct device *dev = rcar->dw.dev;

	/* [한국어] prepare 가 올린 사용 카운트를 내린다. */
	pm_runtime_put(dev);
	/* [한국어] 런타임 PM 자체를 끈다. 순서를 바꾸면 카운트가 남은 채 비활성화된다. */
	pm_runtime_disable(dev);
}

/* [한국어]
 * rcar_gen4_pcie_get_resources - "phy" 와 "app" 두 레지스터 창을 얻는다
 *
 * @rcar: 이 드라이버의 상태. pdev 가 이미 채워져 있어야 한다.
 * @return: 0 성공. 둘 중 하나라도 매핑에 실패하면 그 오류 코드.
 *
 * 이 파일이 직접 얻는 자원은 이 둘뿐이다. DBI 창, 클럭, 리셋, PERST# GPIO 는
 * 모두 DWC 코어의 dw_pcie_get_resources() 가 나중에 얻는다 — 그래서
 * rcar_gen4_pcie_alloc() 이 REQ_RES capability 를 켜 두는 것이다.
 *
 * 이름으로 자원을 찾는 방식(_byname)이라 DT 의 reg-names 순서에 의존하지 않는다.
 *
 * 실행 컨텍스트: probe. devm 매핑이라 잠들 수 있다.
 *
 * 에러 경로: 첫 매핑 실패는 PTR_ERR 로 올리고, 둘째는 PTR_ERR_OR_ZERO 로
 * 성공/실패를 한 줄에 담는다. devm 이므로 되감을 것이 없다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname()
 */
static int rcar_gen4_pcie_get_resources(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] PHY 레지스터 창을 이름으로 찾아 매핑한다. 이 창의 오프셋에는
	 * 이름이 없어 이 파일 곳곳에 생 숫자로 나타난다. */
	rcar->phy_base = devm_platform_ioremap_resource_byname(rcar->pdev, "phy");
	/* [한국어] 자원이 없거나 매핑에 실패했다. */
	if (IS_ERR(rcar->phy_base))
		/* [한국어] ERR_PTR 에서 오류 코드를 꺼내 올린다. */
		return PTR_ERR(rcar->phy_base);

	/* Renesas-specific registers */
	/* [한국어] Renesas 전용 app 창. 모드·LTSSM·링크 상태·인터럽트 인에이블이
	 * 모두 이 창에 있어, 이 파일에서 가장 많이 쓰인다. */
	rcar->base = devm_platform_ioremap_resource_byname(rcar->pdev, "app");

	/* [한국어] 오류 포인터면 코드를, 아니면 0 을 돌려주는 헬퍼로 한 줄에 담았다. */
	return PTR_ERR_OR_ZERO(rcar->base);
}

/* [한국어] DWC 코어가 부르는 코어 콜백 표. 셋뿐인 것이 이 SoC 가 DWC 표준에서
 * 크게 벗어나지 않는다는 뜻이다 — 설정공간 접근자(read_dbi/write_dbi)나 주소
 * 보정(cpu_addr_fixup) 을 따로 두지 않았으므로, 그 일들은 코어의 기본 구현이 한다.
 * 이 표는 RC 와 EP 가 함께 쓴다.
 *
 * [상류 코드 관찰] 표의 이름이 `dw_pcie_ops` 라서 DWC 코어 쪽 이름과 겹쳐 보이지만,
 * 이것은 이 파일 안의 static 변수이므로 충돌하지 않는다. 원본 스냅숏에서도 같다. */
static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 훈련 시작. LTSSM 켜기와 수동 속도 변경이 여기서 일어난다. */
	.start_link = rcar_gen4_pcie_start_link,
	/* [한국어] 링크 정지. LTSSM 비트를 지운다. */
	.stop_link = rcar_gen4_pcie_stop_link,
	/* [한국어] 링크 상태 질의. 코어가 링크를 기다릴 때 반복해 부른다. */
	.link_up = rcar_gen4_pcie_link_up,
};

/* [한국어]
 * rcar_gen4_pcie_alloc - 상태 구조체를 잡고 DWC 코어와 이어 붙인다
 *
 * @pdev: 플랫폼 코어가 넘겨준 이 컨트롤러의 디바이스.
 * @return: 성공 시 struct rcar_gen4_pcie 포인터, 실패 시 ERR_PTR(-ENOMEM).
 *
 * probe 의 첫 단계다. 메모리를 잡는 것 말고도, DWC 코어가 나중에 이 드라이버를
 * 어떻게 다룰지를 결정하는 설정 셋을 여기서 미리 심어 둔다.
 *   - dw.ops: 위의 세 콜백 표.
 *   - dw.edma.mf: eDMA 매핑 형식을 unroll 로 못박는다. 이렇게 미리 정해 두면
 *     dw_pcie_edma_detect() 가 형식을 탐색하는 단계를 건너뛴다
 *     (pcie-designware.c 의 dw_pcie_edma_find_mf 에서 확인).
 *   - REQ_RES capability: "클럭과 리셋은 이 글루가 직접 켜고 끄겠다" 는 선언이다.
 *     이 비트가 서 있어야 코어가 core_clks/core_rsts 를 얻어 두고, 그 배열을
 *     common_init 이 쓴다.
 *
 * devm_kzalloc 이라 해제는 devres 가 한다 — 그래서 이 파일 어디에도 kfree 가 없다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다(GFP_KERNEL).
 *
 * 에러 경로: 할당 실패만 있으며 ERR_PTR 로 알린다. 호출자는 IS_ERR 로 받는다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_probe() → [이 함수]
 *     → devm_kzalloc(), dw_pcie_cap_set(), platform_set_drvdata()
 */
static struct rcar_gen4_pcie *rcar_gen4_pcie_alloc(struct platform_device *pdev)
{
	/* [한국어] devm 할당과 dw.dev 설정에 쓸 struct device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 새로 잡을 상태 구조체. */
	struct rcar_gen4_pcie *rcar;

	/* [한국어] 0 으로 초기화된 상태 구조체를 devres 로 잡는다. 0 초기화 덕에
	 * drvdata 등 아직 안 채운 필드가 NULL 로 남는다. */
	rcar = devm_kzalloc(dev, sizeof(*rcar), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!rcar)
		/* [한국어] 포인터 반환 함수이므로 오류도 포인터로 포장해 돌려준다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 코어 콜백 표를 건다. 이후 DWC 코어의 모든 링크 조작이 이 표를 거친다. */
	rcar->dw.ops = &dw_pcie_ops;
	/* [한국어] 코어가 dev_err 등에 쓸 장치를 알려 준다. */
	rcar->dw.dev = dev;
	/* [한국어] 자원 조회 API 가 platform_device 를 받으므로 따로 보관한다. */
	rcar->pdev = pdev;
	/* [한국어] eDMA 매핑 형식을 unroll 로 못박는다. 채널마다 레지스터 블록이
	 * 따로 있는 배치라는 뜻이며, 미리 정해 두면 코어가 형식을 탐색하지 않는다. */
	rcar->dw.edma.mf = EDMA_MF_EDMA_UNROLL;
	/* [한국어] "클럭·리셋을 글루가 직접 관리한다" 는 capability. 이것이 켜져 있어야
	 * dw_pcie_get_resources() 가 core_clks/core_rsts 를 채워 주고,
	 * common_init 이 그 배열을 쓸 수 있다. */
	dw_pcie_cap_set(&rcar->dw, REQ_RES);
	/* [한국어] platform_get_drvdata() 로 되찾을 수 있게 등록한다. remove 가 이것을 쓴다. */
	platform_set_drvdata(pdev, rcar);

	/* [한국어] 여기서부터 rcar 는 유효한 상태 구조체다. */
	return rcar;
}

/* Host mode */
/* [한국어]
 * rcar_gen4_pcie_host_init - RC 모드 전용 초기화 (dw_pcie_host_ops.init 콜백)
 *
 * @pp: DWC 코어의 루트 포트 문맥. 여기서 컨트롤러와 SoC 상태를 되찾는다.
 * @return: 0 성공. 공통 초기화가 실패하면 그 오류 코드.
 *
 * dw_pcie_host_init() 이 브리지 ops 와 설정공간 접근 방식을 정한 직후,
 * dw_pcie_setup_rc() 로 넘어가기 전에 부르는 콜백이다. 즉 "이 시점에는
 * 하드웨어가 살아 있어야 한다" 는 계약을 이 함수가 이행한다.
 *
 * 절차는 넷이다.
 *   1. PERST# 를 건다 — 하위 장치를 리셋에 잡아 둔 채로 컨트롤러를 세운다.
 *   2. RC/EP 공통 초기화(클럭·리셋·모드 비트).
 *   3. BAR0/BAR1 을 dbi2 로 죽인다. 루트 포트는 자기 BAR 로 메모리를 받을
 *      이유가 없는데, 죽여 두지 않으면 열거 과정에서 공간이 배정된다
 *      (근거 문서는 위 상류 주석이 DWC 데이터북 절 번호까지 적어 두었다).
 *   4. MSI 인터럽트 신호를 열고, 100ms 를 기다린 뒤 PERST# 를 푼다.
 *
 * 왜 100ms 인가: PCIe CEM 규격의 T_PVPERL 로, 전원이 안정된 뒤 PERST# 를
 * 해제하기까지 지켜야 하는 최소 시간이다. 상수는 drivers/pci/pci.h 의
 * PCIE_T_PVPERL_MS 에서 온다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. msleep() 과
 * gpiod_set_value_cansleep() 이 있어 잠들 수 있다.
 *
 * 에러 경로: 공통 초기화 실패만 위로 올린다. 그 경우 PERST# 는 건 상태로
 * 남는데, 하위 장치를 리셋에 잡아 두는 쪽이 안전하므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pp->ops->init → [이 함수]
 *     → rcar_gen4_pcie_common_init(), dw_pcie_writel_dbi2(), msleep()
 */
static int rcar_gen4_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 루트 포트 문맥에서 컨트롤러 문맥으로 올라간다. */
	struct dw_pcie *dw = to_dw_pcie_from_pp(pp);
	/* [한국어] 다시 거기서 SoC 상태로 내려온다. 콜백 인자가 pp 뿐이라 두 단계를 밟는다. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);
	/* [한국어] 공통 초기화의 결과. */
	int ret;
	/* [한국어] 인터럽트 인에이블 레지스터의 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] PERST#(하위 장치 리셋)를 건다. 컨트롤러를 세우는 동안 상대를
	 * 리셋에 잡아 두어, 절반만 초기화된 컨트롤러에 상대가 반응하지 않게 한다.
	 * dw->pe_rst 는 DWC 코어가 devm_gpiod_get_optional("reset") 으로 얻은 것이라
	 * DT 에 reset-gpios 가 없으면 NULL 일 수 있다(pcie-designware.c 에서 확인).
	 * NULL 서술자를 gpiod API 가 어떻게 다루는지는 drivers/gpio 가 이 트리에
	 * 없어 확인 못 함. _cansleep 판이므로 이 경로는 잠들 수 있다. */
	gpiod_set_value_cansleep(dw->pe_rst, 1);

	/* [한국어] RC/EP 공통 하드웨어 초기화. 클럭을 켜고 전원 리셋을 걸었다 풀며
	 * PCIEMSR0 에 DEVICE_TYPE_RC 를 쓴다. */
	ret = rcar_gen4_pcie_common_init(rcar);
	/* [한국어] 공통 초기화 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. dw_pcie_host_init() 이 되감기를 맡는다. */
		return ret;

	/*
	 * According to the section 3.5.7.2 "RC Mode" in DWC PCIe Dual Mode
	 * Rev.5.20a and 3.5.6.1 "RC mode" in DWC PCIe RC databook v5.20a, we
	 * should disable two BARs to avoid unnecessary memory assignment
	 * during device enumeration.
	 */
	/* [한국어] BAR0 의 마스크를 0 으로 만들어 BAR 를 없앤다. dbi2 는 BAR 의 "크기" 를
	 * 쓰는 섀도 창이라, 여기에 0 을 쓰면 그 BAR 가 존재하지 않는 것이 된다. */
	dw_pcie_writel_dbi2(dw, PCI_BASE_ADDRESS_0, 0x0);
	/* [한국어] BAR1 도 같은 이유로 죽인다. 위 상류 주석이 근거로 든 DWC 데이터북
	 * 절이 이 둘을 지목한다. */
	dw_pcie_writel_dbi2(dw, PCI_BASE_ADDRESS_1, 0x0);

	/* Enable MSI interrupt signal */
	/* [한국어] 인터럽트 인에이블 레지스터를 읽는다. 다른 비트를 보존해야 한다. */
	val = readl(rcar->base + PCIEINTSTS0EN);
	/* [한국어] MSI 컨트롤 인터럽트를 연다. 이 SoC 레벨 신호가 열려 있어야 DWC
	 * 코어의 MSI 처리기가 인터럽트를 받는다. EP 모드에는 이 단계가 없다. */
	val |= MSI_CTRL_INT;
	/* [한국어] 열린 상태를 반영한다. */
	writel(val, rcar->base + PCIEINTSTS0EN);

	/* [한국어] 전원 유효 후 PERST# 해제까지의 규약 시간 100ms 를 지킨다.
	 * 상수는 drivers/pci/pci.h 의 PCIE_T_PVPERL_MS 이며, 옆의 상류 주석이
	 * 그 목적을 그대로 적어 두었다. */
	msleep(PCIE_T_PVPERL_MS);	/* pe_rst requires 100msec delay */

	/* [한국어] PERST# 를 푼다. 이 순간 하위 장치가 살아나기 시작하고, 이어서
	 * DWC 코어가 dw_pcie_setup_rc() 와 start_link 로 넘어간다. */
	gpiod_set_value_cansleep(dw->pe_rst, 0);

	/* [한국어] 여기까지 오면 RC 쪽 SoC 준비가 끝난 것이다. */
	return 0;
}

/* [한국어]
 * rcar_gen4_pcie_host_deinit - RC 모드 정리 (dw_pcie_host_ops.deinit 콜백)
 *
 * @pp: DWC 코어의 루트 포트 문맥.
 * @return: 없음.
 *
 * host_init 의 역순이다. 먼저 PERST# 를 걸어 하위 장치를 멈춰 세운 뒤,
 * 공통 정리로 리셋을 걸고 클럭을 끈다. 순서를 바꾸면 아직 살아 있는 하위
 * 장치가 사라진 링크로 트랜잭션을 시도할 수 있다.
 *
 * host_init 이 연 MSI 인터럽트 비트나 죽여 둔 BAR 마스크를 되돌리지 않는데,
 * 그 뒤 곧바로 전원 리셋이 걸려 레지스터가 통째로 초기화되므로 되돌릴 이유가
 * 없다.
 *
 * 실행 컨텍스트: remove 또는 초기화 실패 되감기. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_deinit() → pp->ops->deinit → [이 함수]
 *     → gpiod_set_value_cansleep(), rcar_gen4_pcie_common_deinit()
 */
static void rcar_gen4_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	/* [한국어] 루트 포트 문맥 → 컨트롤러 문맥. */
	struct dw_pcie *dw = to_dw_pcie_from_pp(pp);
	/* [한국어] 컨트롤러 문맥 → SoC 상태. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);

	/* [한국어] PERST# 를 걸어 하위 장치를 리셋에 잡아 둔다. 클럭을 끄기 전에 해야 한다. */
	gpiod_set_value_cansleep(dw->pe_rst, 1);
	/* [한국어] 공통 정리 — 전원 리셋을 걸고 코어 클럭을 끈다. */
	rcar_gen4_pcie_common_deinit(rcar);
}

/* [한국어] RC 모드에서 DWC 호스트 코어가 부르는 훅 표. 둘뿐이며, 이 파일이
 * DWC 표준 호스트 동작에 거의 손대지 않는다는 뜻이다 — 설정공간 접근이나
 * MSI 도메인을 따로 두지 않았으므로 그 일들은 pcie-designware-host.c 가 한다. */
static const struct dw_pcie_host_ops rcar_gen4_pcie_host_ops = {
	/* [한국어] 링크 훈련 직전에 불린다. PERST# 조작과 BAR 죽이기가 여기 있다. */
	.init = rcar_gen4_pcie_host_init,
	/* [한국어] 정리 때 불린다. init 의 역순. */
	.deinit = rcar_gen4_pcie_host_deinit,
};

/* [한국어]
 * rcar_gen4_add_dw_pcie_rp - 이 컨트롤러를 루트 포트로 등록한다
 *
 * @rcar: 이 드라이버의 상태. drvdata 가 이미 채워져 있다.
 * @return: 0 성공. 호스트 지원이 빌드에서 빠졌으면 -ENODEV,
 *          그 밖에는 dw_pcie_host_init() 의 실패값.
 *
 * DT 가 RC 용 compatible 을 지정했을 때 rcar_gen4_add_dw_pcie() 가 여기로 온다.
 * 하는 일은 두 가지 설정을 심고 DWC 호스트 코어에 넘기는 것뿐이다.
 *
 * IS_ENABLED 검사가 앞에 있는 것이 이 파일의 구조를 보여 준다. RC 지원과 EP
 * 지원이 각각 별도의 Kconfig 항목이면서 둘 다 같은 오브젝트 파일을 빌드하므로
 * (같은 디렉터리 Kconfig 에서 확인), 꺼진 쪽 경로를 런타임에 막아야 한다.
 * 컴파일 타임 상수라 최적화로 통째로 사라진다.
 *
 * 실행 컨텍스트: probe. dw_pcie_host_init() 안에서 잠들 수 있다.
 *
 * 에러 경로: 실패값을 그대로 올린다. probe 가 받아 unprepare 로 되감는다.
 *
 * 호출 체인:
 *   rcar_gen4_add_dw_pcie() → [이 함수] → dw_pcie_host_init()
 */
static int rcar_gen4_add_dw_pcie_rp(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] DWC 코어 안에 박혀 있는 루트 포트 문맥을 가리킨다. */
	struct dw_pcie_rp *pp = &rcar->dw.pp;

	/* [한국어] 호스트 지원이 빌드에 없으면 이 경로 자체가 성립하지 않는다. */
	if (!IS_ENABLED(CONFIG_PCIE_RCAR_GEN4_HOST))
		/* [한국어] "이 장치를 다룰 수 없다" 는 뜻으로 -ENODEV 를 돌려준다. */
		return -ENODEV;

	/* [한국어] MSI 벡터 수를 DWC 가 지원하는 최대치(256)로 잡는다. 이 값이
	 * msi_irq_in_use 비트맵과 MSI 컨트롤 블록 개수를 결정한다. */
	pp->num_vectors = MAX_MSI_IRQS;
	/* [한국어] 위의 init/deinit 훅 표를 건다. */
	pp->ops = &rcar_gen4_pcie_host_ops;

	/* [한국어] 여기서 DWC 호스트 코어로 넘어간다. 자원 확보, 설정공간 접근 방식
	 * 결정, MSI 준비, host_ops->init 콜백, setup_rc, start_link, 버스 스캔이
	 * 모두 이 한 호출 안에서 일어난다. */
	return dw_pcie_host_init(pp);
}

/* [한국어]
 * rcar_gen4_remove_dw_pcie_rp - 루트 포트 등록을 되돌린다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * rcar_gen4_add_dw_pcie_rp() 의 짝이다. 한 줄짜리 얇은 래퍼지만, remove 쪽에서
 * RC/EP 를 같은 모양으로 갈라 부르기 위해 따로 둔 것이다.
 *
 * dw_pcie_host_deinit() 안에서 host_ops->deinit 콜백이 불려
 * rcar_gen4_pcie_host_deinit() 까지 이어진다.
 *
 * 실행 컨텍스트: remove. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_remove_dw_pcie() → [이 함수] → dw_pcie_host_deinit()
 *     → pp->ops->deinit → rcar_gen4_pcie_host_deinit()
 */
static void rcar_gen4_remove_dw_pcie_rp(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] DWC 호스트 코어에 버스 제거와 자원 해제를 맡긴다. */
	dw_pcie_host_deinit(&rcar->dw.pp);
}

/* Endpoint mode */
/* [한국어]
 * rcar_gen4_pcie_ep_pre_init - EP 모드 전용 초기화 (dw_pcie_ep_ops.pre_init 콜백)
 *
 * @ep: DWC 코어의 엔드포인트 문맥.
 * @return: 없음(콜백 원형이 void 다).
 *
 * dw_pcie_ep_init() 이 EPC 를 만들고 자원을 얻은 뒤, 레지스터를 만지기 직전에
 * 부르는 훅이다(pcie-designware-ep.c 에서 확인). RC 쪽 host_init 과 같은
 * 자리를 차지하지만 하는 일은 더 적다.
 *
 * RC 와 갈리는 점이 여기서 뚜렷하다.
 *   - PERST# 를 만지지 않는다. EP 는 그 신호를 받는 쪽이다.
 *   - BAR 를 죽이지 않는다. 오히려 아래 epc_features 표로 BAR 능력을 광고한다.
 *   - MSI 인터럽트 대신 eDMA 인터럽트를 연다.
 *
 * [상류 코드 관찰] 반환 타입이 void 라, 공통 초기화가 실패해도 그 사실을
 * 위로 알릴 방법이 없다. 코드는 `return;` 으로 조용히 빠져나가고
 * dw_pcie_ep_init() 은 초기화가 성공한 줄 알고 계속 진행한다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 공통 초기화 안에서 잠들 수 있다.
 *
 * 에러 경로: 위 관찰대로 사실상 없다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init() → ep->ops->pre_init → [이 함수]
 *     → rcar_gen4_pcie_common_init(), writel()
 */
static void rcar_gen4_pcie_ep_pre_init(struct dw_pcie_ep *ep)
{
	/* [한국어] 엔드포인트 문맥에서 컨트롤러 문맥으로 올라간다. */
	struct dw_pcie *dw = to_dw_pcie_from_ep(ep);
	/* [한국어] 거기서 SoC 상태로 내려온다. app 창이 필요해서다. */
	struct rcar_gen4_pcie *rcar = to_rcar_gen4_pcie(dw);
	/* [한국어] 공통 초기화의 결과. 다만 위로 올릴 길이 없다. */
	int ret;

	/* [한국어] RC 와 같은 공통 초기화. PCIEMSR0 에는 DEVICE_TYPE_EP 가 쓰인다. */
	ret = rcar_gen4_pcie_common_init(rcar);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] void 반환이라 조용히 빠져나가는 수밖에 없다 — 위 [상류 코드 관찰] 참조. */
		return;

	/* [한국어] eDMA 인터럽트를 하위 16비트 통째로 연다. EP 모드에서는 내장 DMA
	 * 엔진이 호스트 메모리와의 전송을 담당하므로 그 완료 신호가 필요하다.
	 * RC 모드에는 대응하는 단계가 없다. */
	writel(PCIEDMAINTSTSEN_INIT, rcar->base + PCIEDMAINTSTSEN);
}

/* [한국어]
 * rcar_gen4_pcie_ep_deinit - EP 모드 정리
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * ep_pre_init 의 역순이다. eDMA 인터럽트를 닫고 공통 정리를 부른다.
 * RC 쪽 host_deinit 과 달리 DWC 콜백 표에 걸려 있지 않고, 이 파일 안에서
 * 직접 불린다 — dw_pcie_ep_ops 에 deinit 자리가 없기 때문이다.
 *
 * 그래서 호출 지점이 셋이다: EP 등록 중 두 번의 실패 되감기와, remove 한 번.
 *
 * 실행 컨텍스트: probe 실패 되감기 또는 remove. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_add_dw_pcie_ep()(실패 시) / rcar_gen4_remove_dw_pcie_ep()
 *     → [이 함수] → writel(), rcar_gen4_pcie_common_deinit()
 */
static void rcar_gen4_pcie_ep_deinit(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] eDMA 인터럽트를 전부 닫는다. 정리 도중 인터럽트가 올라오지 않게 한다. */
	writel(0, rcar->base + PCIEDMAINTSTSEN);
	/* [한국어] 전원 리셋을 걸고 코어 클럭을 끈다. */
	rcar_gen4_pcie_common_deinit(rcar);
}

/* [한국어]
 * rcar_gen4_pcie_ep_raise_irq - 호스트 쪽으로 인터럽트를 쏜다
 *
 * @ep: DWC 코어의 엔드포인트 문맥.
 * @func_no: 어느 물리 함수가 쏘는지. 다기능 엔드포인트에서 의미가 있다.
 * @type: PCI_IRQ_INTX 또는 PCI_IRQ_MSI.
 * @interrupt_num: MSI 벡터 번호(1부터). INTX 에서는 쓰이지 않는다.
 * @return: 0 성공, -EINVAL 이면 이 컨트롤러가 다루지 않는 종류.
 *
 * dw_pcie_ep_ops.raise_irq 콜백이다. EPF(기능 드라이버)가 호스트에 사건을
 * 알리고 싶을 때 EPC 코어를 거쳐 여기까지 내려온다.
 *
 * 실제 발사는 전부 DWC 코어가 한다 — INTX 는 Assert_INTx 메시지를,
 * MSI 는 미리 잡아 둔 아웃바운드 창에 메모리 쓰기를 내보내는 식이다.
 * 이 함수가 하는 일은 종류에 따라 코어의 어느 함수를 부를지 고르는 것뿐이라,
 * 이 SoC 에 인터럽트 발사와 관련해 특별한 것이 없다는 뜻이기도 하다.
 *
 * MSI-X 가 없는 것은 아래 epc_features 표가 msi_capable 만 켜고
 * msix_capable 을 켜지 않은 것과 짝을 이룬다 — EPC 코어가 그 표를 보고
 * 애초에 MSI-X 요청을 걸러 주므로 default 로 떨어질 일이 없다.
 *
 * [상류 코드 관찰] switch 의 모든 갈래가 return 으로 끝나므로 마지막
 * `return 0;` 에는 도달할 수 없다. 원본 스냅숏(1f0e418bb6)에서 확인했으며
 * 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: EPF 가 부르는 경로. 하위 구현이 MMIO 쓰기를 하므로
 * 잠들지는 않지만, 이 트리만으로 모든 호출자의 컨텍스트를 단정할 수는 없다.
 *
 * 에러 경로: 모르는 종류면 로그를 남기고 -EINVAL.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → ep->ops->raise_irq → [이 함수]
 *     → dw_pcie_ep_raise_intx_irq() / dw_pcie_ep_raise_msi_irq()
 */
static int rcar_gen4_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				       unsigned int type, u16 interrupt_num)
{
	/* [한국어] dev_err 에 쓸 장치를 얻기 위해 컨트롤러 문맥으로 올라간다.
	 * SoC 상태(rcar)는 필요 없다 — 이 경로에 SoC 고유 동작이 없기 때문이다. */
	struct dw_pcie *dw = to_dw_pcie_from_ep(ep);

	/* [한국어] 인터럽트 종류로 갈린다. */
	switch (type) {
	/* [한국어] 레거시 INTx. 가상 와이어 인터럽트 메시지를 보낸다. */
	case PCI_IRQ_INTX:
		/* [한국어] 코어가 Assert_INTx 메시지를 내보낸다. 벡터 번호가 없는 방식이라
		 * interrupt_num 을 넘기지 않는다. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	/* [한국어] MSI. 메모리 쓰기 트랜잭션으로 인터럽트를 전달한다. */
	case PCI_IRQ_MSI:
		/* [한국어] 호스트가 설정해 둔 MSI 주소/데이터로 쓰기를 내보낸다.
		 * 벡터 번호가 어느 메시지인지를 고른다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	/* [한국어] MSI-X 를 포함해 그 밖의 종류. */
	default:
		/* [한국어] 어떤 종류였는지는 남기지 않는다 — 상류 코드가 그렇게 되어 있다. */
		dev_err(dw->dev, "Unknown IRQ type\n");
		/* [한국어] 인자가 잘못됐다는 뜻으로 -EINVAL. EPC 코어가 EPF 에 전한다. */
		return -EINVAL;
	}

	/* [한국어] 위 [상류 코드 관찰] 대로 도달할 수 없는 줄이다. */
	return 0;
}

/* [한국어] 이 엔드포인트가 호스트에 무엇을 광고할 수 있는지 적어 둔 능력표.
 * EPC 코어(drivers/pci/endpoint)와 EPF 가 이 표를 보고 BAR 크기와 종류를
 * 검증하며, 맞지 않는 요청은 하드웨어에 닿기 전에 걸러진다. */
static const struct pci_epc_features rcar_gen4_pcie_epc_features = {
	/* [한국어] DWC 코어가 공통으로 지원하는 두 능력을 한 번에 켜는 매크로 —
	 * dynamic_inbound_mapping(BAR 의 로컬 물리 주소를 나중에 바꿔 달 수 있음)과
	 * subrange_mapping(BAR 하나를 여러 조각으로 나눠 매핑). 정의는
	 * pcie-designware.h 에 있다. */
	DWC_EPC_COMMON_FEATURES,
	/* [한국어] MSI 를 지원한다고 광고한다. msix_capable 은 켜지 않았으므로
	 * MSI-X 요청은 EPC 코어 단계에서 걸러진다 — 위 raise_irq 의 default 갈래가
	 * 사실상 죽어 있는 이유다. */
	.msi_capable = true,
	/* [한국어] BAR0 은 크기 조절 가능(Resizable BAR). EPF 가 원하는 크기를 요청할 수
	 * 있으며, EPC 코어가 1MB~128TB 범위를 강제한다. */
	.bar[BAR_0] = { .type = BAR_RESIZABLE, },
	/* [한국어] BAR1 은 아예 없는 자리로 신고한다. 64비트 BAR 는 다음 번호의 BAR
	 * 레지스터를 함께 소비하므로(그래서 pci-epc-core.c 가 BAR_5 에 64비트를 금지한다)
	 * BAR0 을 64비트로 쓰면 이 자리가 비게 된다. 다만 이 파일이 그 이유를 적어 두지는 않았다. */
	.bar[BAR_1] = { .type = BAR_DISABLED, },
	/* [한국어] BAR2 도 크기 조절 가능. BAR0 과 같은 짜임이다. */
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	/* [한국어] BAR3 도 없는 자리 — BAR2 를 64비트로 쓰는 배치와 짝을 이룬다. */
	.bar[BAR_3] = { .type = BAR_DISABLED, },
	/* [한국어] BAR4 는 크기가 256바이트로 고정이다. EPF 가 다른 크기를 요청하면
	 * EPC 코어가 거절한다. */
	.bar[BAR_4] = { .type = BAR_FIXED, .fixed_size = 256 },
	/* [한국어] BAR5 는 없는 자리. */
	.bar[BAR_5] = { .type = BAR_DISABLED, },
	/* [한국어] BAR 주소가 지켜야 할 정렬 단위 4KB. EPF 가 백업 메모리를 잡을 때
	 * 이 값에 맞춘다(pci-epf-core.c 에서 확인). */
	.align = SZ_4K,
};

/* [한국어]
 * rcar_gen4_pcie_ep_get_features - 이 엔드포인트의 능력표를 돌려준다
 *
 * @ep: DWC 코어의 엔드포인트 문맥. 이 구현은 쓰지 않는다.
 * @return: 위에서 정의한 정적 능력표의 주소. 실패라는 개념이 없다.
 *
 * dw_pcie_ep_ops.get_features 콜백이다. EPC 코어가 BAR 요청을 검증할 때마다
 * 이 표를 얻어 간다.
 *
 * 인스턴스마다 능력이 달라지지 않으므로 정적 표를 그대로 돌려준다. 인자 ep 를
 * 쓰지 않는 것이 그 사실을 드러낸다 — 이 컨트롤러의 BAR 구성은 SoC 가 정한
 * 고정 사양이지 DT 나 런타임 상태에 따라 바뀌는 것이 아니다.
 *
 * 실행 컨텍스트: EPC 코어의 검증 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_epc_get_features() → ep->ops->get_features → [이 함수]
 */
static const struct pci_epc_features*
rcar_gen4_pcie_ep_get_features(struct dw_pcie_ep *ep)
{
	/* [한국어] 정적 표의 주소를 그대로 준다. 호출자는 읽기만 한다. */
	return &rcar_gen4_pcie_epc_features;
}

/* [한국어]
 * rcar_gen4_pcie_ep_get_dbi_offset - 함수 번호에 대응하는 DBI 창 오프셋을 계산한다
 *
 * @ep: DWC 코어의 엔드포인트 문맥. 이 구현은 쓰지 않는다.
 * @func_no: 물리 함수 번호(0부터).
 * @return: DBI 창 시작에서 그 함수의 설정공간까지의 바이트 오프셋.
 *
 * dw_pcie_ep_ops.get_dbi_offset 콜백이다. 다기능 엔드포인트에서 함수마다
 * 설정공간이 따로 있는데, 그 배치 간격은 IP 통합 방식에 따라 달라서 DWC
 * 코어가 알 수 없다. 그래서 글루에 물어보는 구조다.
 *
 * R-Car Gen4 는 함수 하나당 0x1000(4KB)씩 떨어져 있다. 곱셈 한 번이 전부이며,
 * 함수 0 이면 0 이 나와 단일 기능 엔드포인트에서도 자연스럽게 맞는다.
 *
 * 이 훅이 없으면 코어는 오프셋 0 을 쓴다(pcie-designware.h 의
 * dw_pcie_ep_get_dbi_offset 에서 확인) — 즉 이 함수는 다기능 지원을 위해서만
 * 존재한다.
 *
 * 실행 컨텍스트: 모든 EP DBI 접근의 앞단. 잠들지 않는다.
 *
 * 에러 경로: 없다. 범위 검사도 하지 않는데, func_no 의 유효성은 상위에서
 * 이미 걸러진다.
 *
 * 호출 체인:
 *   dw_pcie_ep_readl_dbi() 등 → dw_pcie_ep_get_dbi_offset()
 *     → ep->ops->get_dbi_offset → [이 함수]
 */
static unsigned int rcar_gen4_pcie_ep_get_dbi_offset(struct dw_pcie_ep *ep,
						       u8 func_no)
{
	/* [한국어] 함수 하나당 4KB 간격. 곱셈만으로 오프셋이 나온다. */
	return func_no * RCAR_GEN4_PCIE_EP_FUNC_DBI_OFFSET;
}

/* [한국어]
 * rcar_gen4_pcie_ep_get_dbi2_offset - 함수 번호에 대응하는 DBI2 창 오프셋을 계산한다
 *
 * @ep: DWC 코어의 엔드포인트 문맥. 이 구현은 쓰지 않는다.
 * @func_no: 물리 함수 번호(0부터).
 * @return: DBI2 창 시작에서 그 함수까지의 바이트 오프셋.
 *
 * 바로 위 DBI 판의 짝이다. DBI2 는 BAR 의 "크기(마스크)" 를 쓰는 별도 섀도
 * 창인데, 이 하드웨어에서는 함수당 간격이 0x800(2KB)으로 DBI 쪽 0x1000 과
 * 다르다. 간격이 다르기 때문에 훅을 따로 둔 것이다 — 같았다면 코어가 DBI 훅을
 * 대신 쓰는 하위 호환 경로로 충분했을 것이다(pcie-designware.h 의
 * dw_pcie_ep_get_dbi2_offset 에서 확인).
 *
 * 실행 컨텍스트: BAR 설정 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_ep_writel_dbi2() → dw_pcie_ep_get_dbi2_offset()
 *     → ep->ops->get_dbi2_offset → [이 함수]
 */
static unsigned int rcar_gen4_pcie_ep_get_dbi2_offset(struct dw_pcie_ep *ep,
						      u8 func_no)
{
	/* [한국어] 함수 하나당 2KB 간격. DBI 쪽(4KB)과 값이 다른 것이 요점이다. */
	return func_no * RCAR_GEN4_PCIE_EP_FUNC_DBI2_OFFSET;
}

/* [한국어] EP 모드에서 DWC 엔드포인트 코어가 부르는 훅 표. 다섯 개이며,
 * RC 쪽 표(둘)보다 많은 것은 엔드포인트가 컨트롤러 내부 배치(함수별 DBI 오프셋)와
 * 능력 광고까지 글루에 물어야 하기 때문이다. */
static const struct dw_pcie_ep_ops pcie_ep_ops = {
	/* [한국어] EPC 를 만든 직후, 레지스터를 만지기 전에 불린다. */
	.pre_init = rcar_gen4_pcie_ep_pre_init,
	/* [한국어] EPF 가 호스트에 인터럽트를 쏠 때 불린다. */
	.raise_irq = rcar_gen4_pcie_ep_raise_irq,
	/* [한국어] BAR 능력표를 묻는다. */
	.get_features = rcar_gen4_pcie_ep_get_features,
	/* [한국어] 함수별 DBI 창 오프셋을 묻는다. */
	.get_dbi_offset = rcar_gen4_pcie_ep_get_dbi_offset,
	/* [한국어] 함수별 DBI2 창 오프셋을 묻는다. 간격이 DBI 와 달라 따로 둔다. */
	.get_dbi2_offset = rcar_gen4_pcie_ep_get_dbi2_offset,
};

/* [한국어]
 * rcar_gen4_add_dw_pcie_ep - 이 컨트롤러를 엔드포인트로 등록한다
 *
 * @rcar: 이 드라이버의 상태. drvdata 가 이미 채워져 있다.
 * @return: 0 성공. EP 지원이 빌드에서 빠졌으면 -ENODEV,
 *          그 밖에는 dw_pcie_ep_init()/init_registers() 의 실패값.
 *
 * DT 가 EP 용 compatible 을 지정했을 때 rcar_gen4_add_dw_pcie() 가 여기로 온다.
 * RC 쪽 짝(add_dw_pcie_rp)이 한 호출로 끝나는 것과 달리, 등록이 두 단계로 나뉜다.
 *   1. dw_pcie_ep_init(): EPC 객체를 만들고 자원을 얻고 pre_init 훅을 부른다.
 *   2. dw_pcie_ep_init_registers(): 실제 설정공간 레지스터를 초기화한다.
 * 그 뒤 pci_epc_init_notify() 로 이미 붙어 있는 EPF 들에게 "이제 BAR 를 설정해도
 * 된다" 고 알린다.
 *
 * [상류 코드 관찰] 2단계가 실패했을 때의 흐름이 특이하다. if 블록 안에서
 * dw_pcie_ep_deinit() 과 rcar_gen4_pcie_ep_deinit() 까지 부르지만 return 을
 * 하지 않아, 그대로 아래로 흘러 pci_epc_init_notify(ep->epc) 를 실행한 뒤
 * 실패값을 돌려준다. dw_pcie_ep_deinit() 은 이미 pci_epc_mem_exit() 까지 마친
 * 상태다(pcie-designware-ep.c 에서 확인). 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 1단계 실패는 SoC 쪽 정리만 하고 반환. 2단계 실패는 위 관찰 참조.
 *
 * 호출 체인:
 *   rcar_gen4_add_dw_pcie() → [이 함수]
 *     → dw_pcie_ep_init() → ep->ops->pre_init → rcar_gen4_pcie_ep_pre_init()
 *     → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 */
static int rcar_gen4_add_dw_pcie_ep(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] DWC 코어 안에 박혀 있는 엔드포인트 문맥을 가리킨다. */
	struct dw_pcie_ep *ep = &rcar->dw.ep;
	/* [한국어] dev_err 용 장치. */
	struct device *dev = rcar->dw.dev;
	/* [한국어] 두 초기화 단계의 결과. */
	int ret;

	/* [한국어] EP 지원이 빌드에 없으면 이 경로가 성립하지 않는다. */
	if (!IS_ENABLED(CONFIG_PCIE_RCAR_GEN4_EP))
		/* [한국어] "이 장치를 다룰 수 없다". */
		return -ENODEV;

	/* [한국어] 위의 훅 다섯을 건다. 이 대입 뒤에야 코어가 pre_init 을 부를 수 있다. */
	ep->ops = &pcie_ep_ops;

	/* [한국어] EPC 객체 생성, 자원 획득, pre_init 훅 호출까지가 이 한 줄이다. */
	ret = dw_pcie_ep_init(ep);
	/* [한국어] 1단계 실패. */
	if (ret) {
		/* [한국어] pre_init 이 이미 공통 초기화를 마쳤을 수 있으므로 SoC 쪽을 되돌린다.
		 * DWC 쪽 되감기는 dw_pcie_ep_init() 이 스스로 했다. */
		rcar_gen4_pcie_ep_deinit(rcar);
		/* [한국어] 실패값을 그대로 올린다. */
		return ret;
	}

	/* [한국어] 2단계 — 설정공간 레지스터(BAR 마스크, 능력 구조 등)를 초기화한다. */
	ret = dw_pcie_ep_init_registers(ep);
	/* [한국어] 2단계 실패. 아래 [상류 코드 관찰] 대로 여기서 반환하지 않는다. */
	if (ret) {
		/* [한국어] 어느 레지스터에서 막혔는지는 하위가 이미 로그로 남긴다. */
		dev_err(dev, "Failed to initialize DWC endpoint registers\n");
		/* [한국어] DWC 쪽 EP 자원(MSI 발사용 페이지, EPC 메모리 할당자)을 되돌린다. */
		dw_pcie_ep_deinit(ep);
		/* [한국어] SoC 쪽(eDMA 인터럽트, 클럭, 리셋)도 되돌린다. */
		rcar_gen4_pcie_ep_deinit(rcar);
	}

	/* [한국어] 이미 붙어 있는 EPF 들에게 EPC 초기화 완료를 알린다. EPF 는 이때부터
	 * BAR 를 만들고 설정 헤더를 쓸 수 있다. 실패 경로에서도 이 줄에 도달한다는
	 * 점이 위 [상류 코드 관찰] 이 가리키는 곳이다. */
	pci_epc_init_notify(ep->epc);

	/* [한국어] 2단계의 결과를 그대로 올린다. */
	return ret;
}

/* [한국어]
 * rcar_gen4_remove_dw_pcie_ep - 엔드포인트 등록을 되돌린다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * rcar_gen4_add_dw_pcie_ep() 의 짝이며, 등록의 역순으로 두 층을 벗긴다 —
 * 먼저 DWC 쪽 EP 자원을, 그다음 SoC 쪽(eDMA 인터럽트·클럭·리셋)을 되돌린다.
 *
 * RC 쪽 짝(remove_dw_pcie_rp)이 한 줄인 것과 달리 두 줄인 이유는,
 * dw_pcie_ep_ops 에 deinit 훅 자리가 없어 SoC 정리를 코어에 맡길 수 없기
 * 때문이다. RC 는 host_ops.deinit 이 있어 코어가 알아서 불러 준다.
 *
 * 실행 컨텍스트: remove. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_remove_dw_pcie() → [이 함수]
 *     → dw_pcie_ep_deinit(), rcar_gen4_pcie_ep_deinit()
 */
static void rcar_gen4_remove_dw_pcie_ep(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] DWC 쪽 EP 자원을 먼저 반납한다. */
	dw_pcie_ep_deinit(&rcar->dw.ep);
	/* [한국어] 그다음 SoC 쪽을 정리한다. 순서를 바꾸면 클럭이 없는 상태에서
	 * DWC 정리가 레지스터를 만지게 된다. */
	rcar_gen4_pcie_ep_deinit(rcar);
}

/* Common */
/* [한국어]
 * rcar_gen4_add_dw_pcie - DT match data 를 읽어 RC/EP 중 한쪽으로 등록한다
 *
 * @rcar: 이 드라이버의 상태. 이 함수가 drvdata 를 채운다.
 * @return: 0 성공. match data 가 없거나 모드가 RC/EP 가 아니면 -EINVAL,
 *          그 밖에는 하위 등록 함수의 실패값.
 *
 * 이 파일에서 RC 와 EP 가 갈리는 첫 지점이다. probe 는 여기까지 모드를 모른 채
 * 진행하고, 이 함수가 of_device_get_match_data() 로 drvdata 를 꺼낸 순간
 * 비로소 갈래가 정해진다.
 *
 * 그래서 rcar->drvdata 가 채워지는 시점을 기억해 둘 필요가 있다 — 이보다 앞선
 * alloc/get_resources/prepare 는 drvdata 를 보지 않고, 이후의 모든 함수는
 * 그것에 의존한다.
 *
 * 실행 컨텍스트: probe. 하위에서 잠들 수 있다.
 *
 * 에러 경로: match data 가 없으면 -EINVAL(DT 나 of_match_table 이 잘못된 경우),
 * 모드가 둘 중 어느 것도 아니면 역시 -EINVAL.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_probe() → [이 함수]
 *     → of_device_get_match_data()
 *     → rcar_gen4_add_dw_pcie_rp() 또는 rcar_gen4_add_dw_pcie_ep()
 */
static int rcar_gen4_add_dw_pcie(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] compatible 문자열에 매칭된 정적 표(이 파일 아래쪽의 넷 중 하나)를 얻는다. */
	rcar->drvdata = of_device_get_match_data(&rcar->pdev->dev);
	/* [한국어] 매칭 데이터가 없다 — of_match_table 과 DT 가 어긋난 경우다. */
	if (!rcar->drvdata)
		/* [한국어] 이후 모든 분기가 drvdata 에 의존하므로 여기서 끊는다. */
		return -EINVAL;

	/* [한국어] 모드로 갈린다. 이 switch 가 RC/EP 두 세계를 나누는 분기점이다. */
	switch (rcar->drvdata->mode) {
	/* [한국어] 루트 컴플렉스. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 루트 포트로 등록한다. */
		return rcar_gen4_add_dw_pcie_rp(rcar);
	/* [한국어] 엔드포인트. */
	case DW_PCIE_EP_TYPE:
		/* [한국어] 엔드포인트로 등록한다. */
		return rcar_gen4_add_dw_pcie_ep(rcar);
	/* [한국어] 이 드라이버가 다루지 않는 모드. */
	default:
		/* [한국어] drvdata 표에 잘못된 값이 들어 있다는 뜻이다. */
		return -EINVAL;
	}
}

/* [한국어]
 * rcar_gen4_pcie_probe - 이 컨트롤러를 붙인다 (플랫폼 드라이버 진입점)
 *
 * @pdev: 플랫폼 코어가 DT 노드와 매칭해 넘겨준 디바이스.
 * @return: 0 성공, 음수 실패.
 *
 * 네 단계를 순서대로 밟는다. 순서가 곧 의존 관계다.
 *   1. alloc          — 상태 구조체를 잡고 DWC 코어와 이어 붙인다.
 *   2. get_resources  — "phy"/"app" 창을 매핑한다. 상태 구조체가 있어야 한다.
 *   3. prepare        — 런타임 PM 으로 전원 도메인을 켠다. 레지스터를 만지기 전에.
 *   4. add_dw_pcie    — 모드를 읽고 RC 또는 EP 로 등록한다. 전원이 켜져 있어야 한다.
 *
 * 되감기가 필요한 단계는 3번뿐이다. 1·2번은 devm 이라 자동으로 풀리고,
 * 4번이 실패하면 3번만 손으로 되돌리면 된다 — 그래서 err_unprepare 라벨이 하나다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 프로세스 컨텍스트이며 잠들 수 있다.
 * PROBE_PREFER_ASYNCHRONOUS 로 등록되어 있어 부팅 중 병렬로 불릴 수 있다.
 *
 * 에러 경로: 각 단계의 실패값을 그대로 올린다. 플랫폼 코어가 -EPROBE_DEFER 를
 * 받으면 나중에 다시 부른다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → rcar_gen4_pcie_alloc(), rcar_gen4_pcie_get_resources(),
 *       rcar_gen4_pcie_prepare(), rcar_gen4_add_dw_pcie()
 */
static int rcar_gen4_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 이번 프로브에서 만들 상태 구조체. */
	struct rcar_gen4_pcie *rcar;
	/* [한국어] 각 단계의 결과. */
	int err;

	/* [한국어] 1단계 — 상태 구조체를 잡고 dw_pcie_ops 와 REQ_RES 를 심는다. */
	rcar = rcar_gen4_pcie_alloc(pdev);
	/* [한국어] 할당 실패는 오류 포인터로 온다. */
	if (IS_ERR(rcar))
		/* [한국어] 코드를 꺼내 올린다. 아직 잡은 것이 없다. */
		return PTR_ERR(rcar);

	/* [한국어] 2단계 — "phy"/"app" 레지스터 창을 매핑한다. */
	err = rcar_gen4_pcie_get_resources(rcar);
	/* [한국어] 자원이 없거나 매핑 실패. DT 가 아직 준비되지 않아 -EPROBE_DEFER 가
	 * 올라올 수도 있다. */
	if (err)
		/* [한국어] devm 매핑이라 되감을 것이 없다. */
		return err;

	/* [한국어] 3단계 — 런타임 PM 을 켜고 전원 도메인을 깨운다. */
	err = rcar_gen4_pcie_prepare(rcar);
	/* [한국어] 실패 시 prepare 가 이미 pm_runtime_disable 까지 되돌려 두었다. */
	if (err)
		/* [한국어] 그래서 여기서는 추가 정리 없이 그대로 반환한다. */
		return err;

	/* [한국어] 4단계 — DT 모드에 따라 RC 또는 EP 로 등록한다. 이 안에서 DWC 코어의
	 * 긴 초기화(콜백 → 링크 훈련 → 버스 스캔 또는 EPC 등록)가 모두 일어난다. */
	err = rcar_gen4_add_dw_pcie(rcar);
	/* [한국어] 등록 실패. 여기서부터는 전원이 켜져 있으므로 되감기가 필요하다. */
	if (err)
		/* [한국어] 런타임 PM 을 되돌리러 간다. */
		goto err_unprepare;

	/* [한국어] 여기까지 오면 컨트롤러가 동작 중이다. */
	return 0;

/* [한국어] 4단계 실패만 이 라벨로 온다. */
err_unprepare:
	/* [한국어] prepare 가 켠 런타임 PM 을 되돌린다. 1·2단계는 devm 이 처리한다. */
	rcar_gen4_pcie_unprepare(rcar);

	/* [한국어] 실패 원인을 그대로 올린다. */
	return err;
}

/* [한국어]
 * rcar_gen4_remove_dw_pcie - 모드에 따라 RC 또는 EP 등록을 되돌린다
 *
 * @rcar: 이 드라이버의 상태. drvdata 가 채워져 있어야 한다.
 * @return: 없음.
 *
 * rcar_gen4_add_dw_pcie() 의 거울이다. 같은 switch 로 갈라 각 모드의 해제
 * 함수를 부른다.
 *
 * add 쪽과 다른 점이 하나 있다. add 는 default 에서 -EINVAL 을 돌려주지만
 * 여기서는 아무것도 하지 않고 지나간다 — 등록이 성공했다면 모드는 이미 RC/EP
 * 둘 중 하나임이 보장되므로, 되감기 경로에서 그 경우를 따질 이유가 없다.
 *
 * 실행 컨텍스트: remove. 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_remove() → [이 함수]
 *     → rcar_gen4_remove_dw_pcie_rp() 또는 rcar_gen4_remove_dw_pcie_ep()
 */
static void rcar_gen4_remove_dw_pcie(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] 등록 때와 같은 기준(drvdata->mode)으로 갈린다. */
	switch (rcar->drvdata->mode) {
	/* [한국어] 루트 컴플렉스였다. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 버스를 내리고 호스트 자원을 반납한다. */
		rcar_gen4_remove_dw_pcie_rp(rcar);
		break;
	/* [한국어] 엔드포인트였다. */
	case DW_PCIE_EP_TYPE:
		/* [한국어] EPC 자원과 SoC 쪽 설정을 되돌린다. */
		rcar_gen4_remove_dw_pcie_ep(rcar);
		break;
	/* [한국어] 등록이 성공했다면 도달할 수 없는 갈래다. */
	default:
		break;
	}
}

/* [한국어]
 * rcar_gen4_pcie_remove - 이 컨트롤러를 뗀다 (플랫폼 드라이버 remove 콜백)
 *
 * @pdev: 떼어 낼 디바이스.
 * @return: 없음(최근 커널의 remove 콜백은 void 를 돌려준다).
 *
 * probe 의 역순이다. 등록을 먼저 되돌리고, 그다음 런타임 PM 을 되돌린다.
 * 자원 매핑과 상태 구조체는 devm 이 이 함수가 끝난 뒤 알아서 푼다 — 그래서
 * 여기에 iounmap 이나 kfree 가 하나도 없다.
 *
 * 실행 컨텍스트: 드라이버 언바인드. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. remove 는 실패할 수 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → rcar_gen4_remove_dw_pcie(), rcar_gen4_pcie_unprepare()
 */
static void rcar_gen4_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] alloc 이 platform_set_drvdata 로 심어 둔 상태 구조체를 되찾는다. */
	struct rcar_gen4_pcie *rcar = platform_get_drvdata(pdev);

	/* [한국어] RC/EP 등록을 되돌린다. 이 안에서 링크가 내려가고 클럭이 꺼진다. */
	rcar_gen4_remove_dw_pcie(rcar);
	/* [한국어] 마지막으로 런타임 PM 을 되돌려 전원 도메인을 놓아 준다. */
	rcar_gen4_pcie_unprepare(rcar);
}

/* [한국어]
 * r8a779f0_pcie_ltssm_control - R-Car S4(r8a779f0) 계열의 LTSSM 켜기/끄기
 *
 * @rcar: 이 드라이버의 상태.
 * @enable: true 면 링크 훈련 시작, false 면 정지.
 * @return: 항상 0. 실패할 수 있는 동작이 없다.
 *
 * drvdata_r8a779f0_pcie 와 drvdata_r8a779f0_pcie_ep 두 표가 이 함수를 가리킨다.
 * 아래의 rcar_gen4_pcie_ltssm_control() 과 대비해서 봐야 하는 함수다 —
 * 같은 자리를 차지하지만 하는 일의 양이 크게 다르다. 이쪽은 PCIERSTCTRL1
 * 레지스터 하나를 읽고 비트를 손봐 되쓰는 것이 전부이고, PHY 펌웨어도
 * SRIS 설정도 없다. 그 차이가 두 SoC 세대의 PHY 통합 방식 차이를 드러낸다.
 *
 * 켤 때는 LTSSM 을 켜면서 PHY 리셋 유지 비트를 함께 지운다. 반면 끌 때는
 * PHY 리셋을 다시 걸지 않는데, 그 이유를 상류 주석이 아래에 적어 두었다.
 *
 * 실행 컨텍스트: start_link/stop_link 콜백. 잠들지 않는다(MMIO 접근뿐).
 *
 * 에러 경로: 없다. 반환값이 int 인 것은 콜백 원형을 맞추기 위해서다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_start_link() / rcar_gen4_pcie_stop_link()
 *     → drvdata->ltssm_control → [이 함수] → readl(), writel()
 */
static int r8a779f0_pcie_ltssm_control(struct rcar_gen4_pcie *rcar, bool enable)
{
	/* [한국어] 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] 리셋 제어 레지스터를 읽는다. 다른 비트를 보존해야 한다. */
	val = readl(rcar->base + PCIERSTCTRL1);
	/* [한국어] 켜는 경우. */
	if (enable) {
		/* [한국어] LTSSM 시작 비트를 세운다. 이 쓰기와 동시에 링크 훈련이 시작된다. */
		val |= APP_LTSSM_ENABLE;
		/* [한국어] PHY 리셋 유지 비트를 지워 PHY 를 풀어 준다. 두 비트를 한 번의
		 * 쓰기로 함께 반영하는 것이 이 함수의 전부다. */
		val &= ~APP_HOLD_PHY_RST;
	/* [한국어] 끄는 경우. */
	} else {
		/*
		 * Since the datasheet of R-Car doesn't mention how to assert
		 * the APP_HOLD_PHY_RST, don't assert it again. Otherwise,
		 * hang-up issue happened in the dw_edma_core_off() when
		 * the controller didn't detect a PCI device.
		 */
		/* [한국어] LTSSM 만 끈다. 위 상류 주석대로 APP_HOLD_PHY_RST 는 다시 세우지
		 * 않는다 — 데이터시트에 거는 방법이 없고, 걸었더니 장치가 없을 때 eDMA
		 * 정지 경로에서 멈춰 버렸다는 것이 그 이유다. 그 함수(dw_edma_core_off)는
		 * drivers/dma 쪽에 있어 이 트리에서 확인 못 함. */
		val &= ~APP_LTSSM_ENABLE;
	}
	/* [한국어] 수정한 값을 반영한다. enable 이든 아니든 쓰기는 한 번뿐이다. */
	writel(val, rcar->base + PCIERSTCTRL1);

	/* [한국어] 실패할 수 있는 동작이 없으므로 항상 성공이다. */
	return 0;
}

/* [한국어]
 * rcar_gen4_pcie_additional_common_init - R-Car V4H 계열의 추가 공통 초기화
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 없음.
 *
 * drvdata_rcar_gen4_pcie 와 drvdata_rcar_gen4_pcie_ep 두 표만 이 훅을 채운다.
 * r8a779f0 계열은 비워 두므로, common_init 은 NULL 검사에서 그냥 지나간다.
 *
 * 두 가지를 한다.
 *   1. DBI 의 레인 skew 삽입 필드를 지우고, 레인이 4개 미만일 때만 비트 6 을
 *      세운다. 레인 분기 구성에서 보정이 필요하다는 뜻으로 읽히지만, 비트 6 이
 *      정확히 무엇을 가리키는지는 R-Car 데이터시트에도 DWC 헤더에도 이름이
 *      없어 이 트리에서 확인 못 함.
 *   2. app 창의 전원관리 레지스터에 CLKREQ# 구동과 클럭 전원관리 활성을 켠다.
 *
 * 이 훅이 common_init 의 맨 끝, 즉 전원 리셋이 풀리고 DBI 접근이 안전해진
 * 뒤에 불린다는 점이 중요하다. DBI 를 만지기 때문이다.
 *
 * 실행 컨텍스트: common_init 안. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_common_init() → drvdata->additional_common_init → [이 함수]
 *     → dw_pcie_readl_dbi(), dw_pcie_writel_dbi(), readl(), writel()
 */
static void rcar_gen4_pcie_additional_common_init(struct rcar_gen4_pcie *rcar)
{
	/* [한국어] DBI 접근자가 struct dw_pcie 를 받으므로 지역 별칭을 둔다. */
	struct dw_pcie *dw = &rcar->dw;
	/* [한국어] 읽기-수정-쓰기용 임시 값. 두 레지스터에 재사용한다. */
	u32 val;

	/* [한국어] 레인 skew 레지스터를 읽는다. DBI 창에 있으므로 DWC 접근자를 쓴다 —
	 * app 창을 다루는 아래 두 줄이 생 readl 인 것과 대비된다. */
	val = dw_pcie_readl_dbi(dw, PCIE_PORT_LANE_SKEW);
	/* [한국어] skew 삽입 필드(하위 24비트)를 통째로 지워 기준 상태로 만든다. */
	val &= ~PORT_LANE_SKEW_INSERT_MASK;
	/* [한국어] 레인이 4개 미만, 즉 레인 분기 구성일 때만. */
	if (dw->num_lanes < 4)
		/* [한국어] 비트 6 을 세운다. 위 함수 주석의 관찰대로 이 비트의 의미는
		 * 이 트리에서 확인 못 함. */
		val |= BIT(6);
	/* [한국어] 수정한 skew 값을 반영한다. */
	dw_pcie_writel_dbi(dw, PCIE_PORT_LANE_SKEW, val);

	/* [한국어] app 창의 전원관리 레지스터를 읽는다. */
	val = readl(rcar->base + PCIEPWRMNGCTRL);
	/* [한국어] CLKREQ# 를 애플리케이션 쪽에서 구동하고 클럭 전원관리를 켠다.
	 * 저전력 상태에서 기준 클럭을 멈출 수 있게 하는 설정이다. */
	val |= APP_CLK_REQ_N | APP_CLK_PM_EN;
	/* [한국어] 반영한다. */
	writel(val, rcar->base + PCIEPWRMNGCTRL);
}

/* [한국어]
 * rcar_gen4_pcie_phy_reg_update_bits - "phy" 창 레지스터의 일부 비트만 바꾼다
 *
 * @rcar: 이 드라이버의 상태.
 * @offset: phy 창 안의 오프셋. 이 파일에서는 모두 이름 없는 생 숫자다.
 * @mask: 바꿀 비트들.
 * @val: 그 자리에 넣을 값. 이미 mask 위치에 정렬되어 있어야 한다.
 * @return: 없음.
 *
 * 읽기-수정-쓰기를 한 줄로 만들어 주는 헬퍼다. 이 함수가 없으면
 * rcar_gen4_pcie_ltssm_control() 이 같은 네 줄을 여덟 번 반복하게 된다.
 *
 * 인자에 검증이 없다는 점을 유의할 것 — val 이 mask 밖의 비트를 담고 있으면
 * 그대로 쓰인다. 호출자가 모두 이 파일 안의 고정된 상수 조합이라 문제가
 * 생기지 않는 구조다.
 *
 * 실행 컨텍스트: ltssm_control 과 download_phy_firmware 안. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_ltssm_control() / rcar_gen4_pcie_download_phy_firmware()
 *     → [이 함수] → readl(), writel()
 */
static void rcar_gen4_pcie_phy_reg_update_bits(struct rcar_gen4_pcie *rcar,
					       u32 offset, u32 mask, u32 val)
{
	/* [한국어] 읽어 둘 현재 값. */
	u32 tmp;

	/* [한국어] phy 창에서 현재 값을 읽는다. DBI 가 아니므로 생 readl 이다. */
	tmp = readl(rcar->phy_base + offset);
	/* [한국어] 바꿀 자리를 먼저 비운다. */
	tmp &= ~mask;
	/* [한국어] 비운 자리에 새 값을 얹는다. */
	tmp |= val;
	/* [한국어] 되쓴다. 이 세 줄이 이 헬퍼의 존재 이유다. */
	writel(tmp, rcar->phy_base + offset);
}

/*
 * SoC datasheet suggests checking port logic register bits during firmware
 * write. If read returns non-zero value, then this function returns -EAGAIN
 * indicating that the write needs to be done again. If read returns zero,
 * then return 0 to indicate success.
 */
/* [한국어]
 * rcar_gen4_pcie_reg_test_bit - 포트 로직 레지스터의 비트가 지워졌는지 본다
 *
 * @rcar: 이 드라이버의 상태.
 * @offset: DBI 창 안의 오프셋. 이 파일에서는 PRTLGC89 또는 PRTLGC90.
 * @mask: 검사할 비트.
 * @return: 0 = 비트가 지워져 있다(성공), -EAGAIN = 아직 서 있다(다시 시도하라).
 *
 * 반환값의 뜻은 바로 위 상류 주석이 그대로 적어 두었다.
 *
 * 이름에는 "phy" 가 없지만 phy 창이 아니라 DBI 창을 읽는다는 점에 주의할 것.
 * PHY 펌웨어를 밀어 넣는 통로가 DBI 안의 포트 로직 레지스터 두 개이기 때문이다.
 *
 * -EAGAIN 을 쓰는 이유는 이것이 오류가 아니라 "아직 처리 중" 이라는 신호이기
 * 때문이다. 두 호출자 모두 이 값을 보고 재시도 루프를 계속한다.
 *
 * 실행 컨텍스트: 펌웨어 내려받기 루프 안. 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값 자체가 상태 보고다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_download_phy_firmware() → [이 함수] → dw_pcie_readl_dbi()
 */
static int rcar_gen4_pcie_reg_test_bit(struct rcar_gen4_pcie *rcar,
				       u32 offset, u32 mask)
{
	/* [한국어] DBI 접근자가 struct dw_pcie 를 받으므로 지역 별칭을 둔다. */
	struct dw_pcie *dw = &rcar->dw;

	/* [한국어] 해당 비트가 아직 서 있으면 하드웨어가 이전 요청을 처리 중이다. */
	if (dw_pcie_readl_dbi(dw, offset) & mask)
		/* [한국어] 오류가 아니라 "다시 시도하라" 는 신호다. */
		return -EAGAIN;

	/* [한국어] 비트가 지워졌다 = 요청이 받아들여졌다. */
	return 0;
}

/* [한국어]
 * rcar_gen4_pcie_download_phy_firmware - PHY 펌웨어를 16비트씩 밀어 넣는다
 *
 * @rcar: 이 드라이버의 상태.
 * @return: 0 성공, request_firmware 실패값, 또는 -ETIMEDOUT.
 *
 * 파일 맨 위 상류 주석이 말하는 "specific firmware" 를 실제로 올리는 함수다.
 * 이것이 없으면 R-Car V4H 의 PCIe 컨트롤러는 동작하지 않는다.
 *
 * 통로가 특이하다. PHY 창에 직접 쓰는 것이 아니라 DBI 안의 포트 로직 레지스터
 * 두 개를 쓴다 — PRTLGC89 에 "PHY 내부 SRAM 의 주소" 를, PRTLGC90 에 "그 자리에
 * 넣을 16비트 데이터" 를 쓰면 하드웨어가 옮겨 준다. 요청이 받아들여졌는지는
 * PRTLGC89 의 비트 30 이 지워졌는지로 확인한다.
 *
 * 절차는 셋이다.
 *   1. 펌웨어 파일을 유저스페이스 로더에서 받는다.
 *   2. 2바이트씩 리틀엔디안으로 묶어 0xc000 부터 차례로 밀어 넣는다.
 *      한 워드마다 최대 100회(각 100~200us) 재시도한다.
 *   3. phy 창 0x0f8 의 비트 17 을 세워 하드웨어에 "다 넣었다" 고 알린 뒤,
 *      정해진 네 주소를 읽어 검증한다.
 *
 * check_addr 의 네 값이 무엇을 뜻하는지는 상류 주석이 "데이터시트의 매직 넘버"
 * 라고만 적어 두었다. 이 트리에서 그 이상은 확인 못 함.
 *
 * 실행 컨텍스트: start_link → ltssm_control 안. request_firmware() 와
 * usleep_range() 가 있어 반드시 잠들 수 있는 컨텍스트여야 한다.
 *
 * 에러 경로: 펌웨어를 못 받으면 그 값을 그대로 올린다(이때는 release 도 없다).
 * 두 루프의 재시도가 다 떨어지면 -ETIMEDOUT 으로 exit 라벨로 뛰어, 어느
 * 경우든 펌웨어를 반드시 반납한다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_ltssm_control() → [이 함수]
 *     → request_firmware(), dw_pcie_writel_dbi(),
 *       rcar_gen4_pcie_reg_test_bit(), rcar_gen4_pcie_phy_reg_update_bits(),
 *       release_firmware()
 */
static int rcar_gen4_pcie_download_phy_firmware(struct rcar_gen4_pcie *rcar)
{
	/* The check_addr values are magical numbers in the datasheet */
	/* [한국어] 내려받기가 끝난 뒤 검증할 네 주소. 위 상류 주석이 밝혔듯 데이터시트가
	 * 제시한 값 그대로이며, 각각이 무엇을 가리키는지는 이 트리에서 확인 못 함.
	 * static const 라 함수를 다시 불러도 다시 만들지 않는다. */
	static const u32 check_addr[] = {
		0x00101018,
		0x00101118,
		0x00101021,
		0x00101121,
	};
	/* [한국어] DBI 접근자와 dev_err 에 쓸 컨트롤러 문맥. */
	struct dw_pcie *dw = &rcar->dw;
	/* [한국어] 로더가 채워 줄 펌웨어 서술자(data 와 size 를 담는다). */
	const struct firmware *fw;
	/* [한국어] i 는 워드/주소 인덱스, timeout 은 한 워드당 남은 재시도 횟수. */
	unsigned int i, timeout;
	/* [한국어] 밀어 넣을 16비트 값. */
	u32 data;
	/* [한국어] 각 단계의 결과. exit 라벨에서 그대로 반환된다. */
	int ret;

	/* [한국어] 유저스페이스 펌웨어 로더에게 파일을 요청한다. 이 호출은 파일을
	 * 읽어 오는 동안 잠들 수 있고, 로더가 아직 준비되지 않았으면 실패한다. */
	ret = request_firmware(&fw, RCAR_GEN4_PCIE_FIRMWARE_NAME, dw->dev);
	/* [한국어] 펌웨어를 못 받았다. */
	if (ret) {
		/* [한국어] 파일 이름과 오류 코드를 함께 남긴다 — 대개 파일이 없는 경우라
		 * 사용자가 조치할 수 있는 정보다. */
		dev_err(dw->dev, "Failed to load firmware (%s): %d\n",
			RCAR_GEN4_PCIE_FIRMWARE_NAME, ret);
		/* [한국어] 받은 것이 없으므로 release_firmware 도 필요 없다. exit 로 가지 않는다. */
		return ret;
	}

	/* [한국어] 파일 크기를 2로 나눈 만큼, 즉 16비트 워드 개수만큼 돈다.
	 * 홀수 바이트가 남으면 그 마지막 바이트는 버려진다. */
	for (i = 0; i < (fw->size / 2); i++) {
		/* [한국어] 두 바이트를 리틀엔디안으로 묶는다 — 뒤 바이트가 상위 8비트다. */
		data = fw->data[(i * 2) + 1] << 8 | fw->data[i * 2];
		/* [한국어] 이 워드에 허용된 재시도 횟수. 100회 x 100us 이상 = 최소 10ms. */
		timeout = 100;
		/* [한국어] 하드웨어가 받아들일 때까지 같은 쌍을 반복해 쓴다. */
		do {
			/* [한국어] 먼저 목적지 주소를 쓴다. 0xc000 이 PHY 내부 SRAM 의 시작이고
			 * 거기에 워드 인덱스를 더한 것이 이번 워드의 자리다. */
			dw_pcie_writel_dbi(dw, PRTLGC89, RCAR_GEN4_PCIE_FIRMWARE_BASE_ADDR + i);
			/* [한국어] 이어서 데이터를 쓴다. 주소-데이터 순서가 뒤바뀌면 안 된다. */
			dw_pcie_writel_dbi(dw, PRTLGC90, data);
			/* [한국어] 비트 30 이 지워졌으면 하드웨어가 이 쌍을 받아들인 것이다. */
			if (!rcar_gen4_pcie_reg_test_bit(rcar, PRTLGC89, BIT(30)))
				/* [한국어] 다음 워드로 넘어간다. */
				break;
			/* [한국어] 아직 처리 중이다. 남은 재시도를 하나 깎아 본다. */
			if (!(--timeout)) {
				/* [한국어] 다 썼으면 하드웨어가 응답하지 않는 것이다. */
				ret = -ETIMEDOUT;
				/* [한국어] 펌웨어를 반납해야 하므로 return 이 아니라 exit 로 뛴다. */
				goto exit;
			}
			/* [한국어] 100~200us 쉬었다 다시 시도한다. 이 대기 때문에 이 함수는
			 * 잠들 수 있는 컨텍스트에서만 불릴 수 있다. */
			usleep_range(100, 200);
		} while (1);
	}

	/* [한국어] 내려받기가 끝났음을 PHY 에 알린다. phy 창 0x0f8 의 비트 17 을
	 * 세우는 것인데, 이 오프셋과 비트에 이름이 없는 것은 데이터시트가 절차만
	 * 제시하고 레지스터 이름을 밝히지 않았기 때문이다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x0f8, BIT(17), BIT(17));

	/* [한국어] 검증 단계 — 정해진 네 주소를 차례로 읽어 본다. */
	for (i = 0; i < ARRAY_SIZE(check_addr); i++) {
		/* [한국어] 주소마다 재시도 예산을 새로 준다. */
		timeout = 100;
		/* [한국어] 두 조건이 모두 만족될 때까지 반복한다. */
		do {
			/* [한국어] 검증할 주소를 쓴다. 데이터는 쓰지 않는다 — 읽기 요청이기 때문이다. */
			dw_pcie_writel_dbi(dw, PRTLGC89, check_addr[i]);
			/* [한국어] 주소 레지스터의 비트 30 — 요청이 처리됐는가. */
			ret = rcar_gen4_pcie_reg_test_bit(rcar, PRTLGC89, BIT(30));
			/* [한국어] 데이터 레지스터의 비트 0 — 결과가 기대한 값인가.
			 * [상류 코드 관찰] 두 결과를 비트 OR 로 합친다. 각 결과가 0 또는 -EAGAIN
			 * 뿐이라 결과적으로 "둘 다 0 일 때만 0" 이 되어 의도대로 동작하지만,
			 * 오류 코드를 비트 OR 하는 것은 흔한 관용구가 아니다. 원본
			 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
			ret |= rcar_gen4_pcie_reg_test_bit(rcar, PRTLGC90, BIT(0));
			/* [한국어] 둘 다 0 이면 이 주소는 통과다. */
			if (!ret)
				/* [한국어] 다음 주소로. */
				break;
			/* [한국어] 아직이면 예산을 깎는다. */
			if (!(--timeout)) {
				/* [한국어] 예산이 떨어졌다. */
				ret = -ETIMEDOUT;
				/* [한국어] 펌웨어 반납을 위해 exit 로. */
				goto exit;
			}
			/* [한국어] 같은 간격으로 쉬었다 다시 본다. */
			usleep_range(100, 200);
		} while (1);
	}

/* [한국어] 성공이든 타임아웃이든 반드시 지나는 지점. 펌웨어를 여기서만 반납한다. */
exit:
	/* [한국어] 로더가 잡아 둔 펌웨어 버퍼를 놓아 준다. 이것을 빼먹으면 누수가 된다. */
	release_firmware(fw);

	/* [한국어] 성공이면 마지막 reg_test_bit 이 남긴 0, 실패면 -ETIMEDOUT 이다. */
	return ret;
}

/* [한국어]
 * rcar_gen4_pcie_ltssm_control - R-Car V4H 계열의 LTSSM 켜기/끄기
 *
 * @rcar: 이 드라이버의 상태.
 * @enable: true 면 링크 훈련 시작, false 면 정지.
 * @return: 0 성공. PHY 준비 대기가 실패하면 -ETIMEDOUT, 펌웨어 단계가
 *          실패하면 그 값.
 *
 * drvdata_rcar_gen4_pcie 와 drvdata_rcar_gen4_pcie_ep 두 표가 가리킨다.
 * 위쪽의 r8a779f0 판과 같은 자리를 차지하지만 무게가 전혀 다르다 —
 * 이 파일에서 가장 긴 절차가 여기 있다.
 *
 * 끌 때는 r8a779f0 판과 마찬가지로 LTSSM 비트 하나만 지우고 곧바로 끝낸다.
 * PHY 리셋을 다시 걸지 않는 것도 같으며, 그 이유는 r8a779f0 판에 달린 상류
 * 주석이 설명한다.
 *
 * 켤 때의 순서는 이렇다.
 *   1. SRIS(양쪽이 각자 기준 클럭을 쓰는 구성)로 못박는다 — DBI 쪽에
 *      deskew 강제 비트를, app 창에 SRIS 모드 비트를 세운다. DT 를 보지 않고
 *      무조건 켠다는 점이 특징이다.
 *   2. phy 창의 이름 없는 오프셋 여덟 군데를 데이터시트가 제시한 값으로 맞춘다.
 *      상류 주석이 이름이 없는 사정을 그대로 적어 두었다.
 *   3. PHY 리셋 유지 비트를 지워 PHY 를 풀어 준다.
 *   4. phy 창 0x0f8 의 비트 18 이 설 때까지 최대 10ms 기다린다 — PHY 가
 *      펌웨어를 받을 준비가 됐다는 신호로 보인다.
 *   5. 펌웨어를 밀어 넣는다.
 *   6. 마지막으로 LTSSM 을 켠다.
 *
 * 순서가 곧 의존 관계다. 펌웨어를 올리기 전에 LTSSM 을 켜면 PHY 가 준비되지
 * 않은 채로 링크 훈련이 시작된다.
 *
 * 실행 컨텍스트: start_link/stop_link 콜백. 켜는 경로는 폴링과 펌웨어 로드
 * 때문에 잠들 수 있다. 끄는 경로는 MMIO 쓰기뿐이라 잠들지 않는다.
 *
 * 에러 경로: 4·5단계 실패는 그대로 올린다. 그 경우 LTSSM 은 켜지지 않으며,
 * 이미 지운 APP_HOLD_PHY_RST 를 되돌리지도 않는다.
 *
 * 호출 체인:
 *   rcar_gen4_pcie_start_link() / rcar_gen4_pcie_stop_link()
 *     → drvdata->ltssm_control → [이 함수]
 *     → dw_pcie_readl_dbi(), rcar_gen4_pcie_phy_reg_update_bits(),
 *       readl_poll_timeout(), rcar_gen4_pcie_download_phy_firmware()
 */
static int rcar_gen4_pcie_ltssm_control(struct rcar_gen4_pcie *rcar, bool enable)
{
	/* [한국어] DBI 접근자가 받는 컨트롤러 문맥. */
	struct dw_pcie *dw = &rcar->dw;
	/* [한국어] 읽기-수정-쓰기와 폴링에 함께 쓰는 임시 값. */
	u32 val;
	/* [한국어] 폴링과 펌웨어 단계의 결과. */
	int ret;

	/* [한국어] 끄는 경로는 여기서 끝난다. 아래의 긴 절차는 켤 때만 필요하다. */
	if (!enable) {
		/* [한국어] 리셋 제어 레지스터를 읽는다. */
		val = readl(rcar->base + PCIERSTCTRL1);
		/* [한국어] LTSSM 비트만 지운다. PHY 리셋은 다시 걸지 않는다. */
		val &= ~APP_LTSSM_ENABLE;
		/* [한국어] 반영한다. */
		writel(val, rcar->base + PCIERSTCTRL1);

		/* [한국어] 실패할 수 있는 동작이 없으므로 성공이다. */
		return 0;
	}

	/* [한국어] 여기서부터 켜는 경로다. 먼저 DBI 의 포트 강제 레지스터를 읽는다. */
	val = dw_pcie_readl_dbi(dw, PCIE_PORT_FORCE);
	/* [한국어] SRIS 구성에서 레인 간 도착 시간 차를 보정하도록 강제한다.
	 * 양쪽이 각자 클럭을 쓰면 레인마다 지연이 달라지기 때문이다. */
	val |= PORT_FORCE_DO_DESKEW_FOR_SRIS;
	/* [한국어] 반영한다. */
	dw_pcie_writel_dbi(dw, PCIE_PORT_FORCE, val);

	/* [한국어] app 창의 모드 레지스터를 읽는다. */
	val = readl(rcar->base + PCIEMSR0);
	/* [한국어] SRIS 모드를 켠다. DT 속성을 보지 않고 무조건 켜므로, 이 SoC 세대는
	 * SRIS 구성으로 고정되어 있다는 뜻이 된다. */
	val |= APP_SRIS_MODE;
	/* [한국어] 반영한다. 위 DBI 쪽 설정과 짝을 이룬다. */
	writel(val, rcar->base + PCIEMSR0);

	/*
	 * The R-Car Gen4 datasheet doesn't describe the PHY registers' name.
	 * But, the initialization procedure describes these offsets. So,
	 * this driver has magical offset numbers.
	 */
	/* [한국어] 아래 여덟 줄은 위 상류 주석이 밝힌 대로 데이터시트가 이름 없이
	 * 오프셋으로만 제시한 초기화 절차다. 0x700 의 네 비트(28/20/12/4)를 지우는데,
	 * 8비트 간격으로 규칙적인 것으로 보아 레인별로 같은 자리를 끄는 것으로 읽힌다.
	 * 다만 그 해석을 뒷받침할 근거는 이 트리에 없다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x700, BIT(28), 0);
	/* [한국어] 두 번째 레인 자리. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x700, BIT(20), 0);
	/* [한국어] 세 번째 레인 자리. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x700, BIT(12), 0);
	/* [한국어] 네 번째 레인 자리. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x700, BIT(4), 0);

	/* [한국어] 0x148 의 필드 넷을 각각 정해진 값으로 맞춘다. 마스크와 값이 모두
	 * 데이터시트가 준 상수라 의미를 풀어 쓸 근거가 이 트리에 없다.
	 * 비트 23:22 필드를 0b01 로. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x148, GENMASK(23, 22), BIT(22));
	/* [한국어] 비트 18:16 필드를 0b011 로. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x148, GENMASK(18, 16), GENMASK(17, 16));
	/* [한국어] 비트 7:6 필드를 0b01 로. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x148, GENMASK(7, 6), BIT(6));
	/* [한국어] 비트 2:0 필드를 0b011 로. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x148, GENMASK(2, 0), GENMASK(1, 0));
	/* [한국어] 0x1d4 의 비트 16:15 를 모두 세운다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x1d4, GENMASK(16, 15), GENMASK(16, 15));
	/* [한국어] 0x514 의 비트 26 을 세운다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x514, BIT(26), BIT(26));
	/* [한국어] 0x0f8 의 비트 16 을 지운다. 이 오프셋은 아래에서 펌웨어 준비 대기와
	 * 내려받기 완료 통지에도 쓰이므로, PHY 제어의 중심 레지스터로 보인다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x0f8, BIT(16), 0);
	/* [한국어] 같은 레지스터의 비트 19 를 세운다. */
	rcar_gen4_pcie_phy_reg_update_bits(rcar, 0x0f8, BIT(19), BIT(19));

	/* [한국어] 리셋 제어 레지스터를 읽는다. */
	val = readl(rcar->base + PCIERSTCTRL1);
	/* [한국어] PHY 리셋 유지를 푼다. 이 순간부터 PHY 가 스스로 초기화를 시작한다.
	 * 위의 phy 창 설정이 모두 끝난 뒤여야 한다. */
	val &= ~APP_HOLD_PHY_RST;
	/* [한국어] 반영한다. */
	writel(val, rcar->base + PCIERSTCTRL1);

	/* [한국어] phy 창 0x0f8 의 비트 18 이 설 때까지 100us 간격으로 최대 10ms
	 * 기다린다. PHY 가 펌웨어를 받을 준비가 됐다는 신호로 쓰이는 비트다.
	 * 이 매크로는 usleep_range 를 쓰므로 잠들 수 있다. */
	ret = readl_poll_timeout(rcar->phy_base + 0x0f8, val, val & BIT(18), 100, 10000);
	/* [한국어] 10ms 안에 비트가 서지 않았다 = PHY 가 살아나지 않았다. */
	if (ret < 0)
		/* [한국어] -ETIMEDOUT 을 그대로 올린다. LTSSM 은 켜지 않는다. */
		return ret;

	/* [한국어] 준비가 됐으니 펌웨어를 밀어 넣는다. 이 파일에서 가장 오래 걸리는 단계다. */
	ret = rcar_gen4_pcie_download_phy_firmware(rcar);
	/* [한국어] 펌웨어 로드나 검증이 실패했다. */
	if (ret)
		/* [한국어] 실패값을 그대로 올린다. */
		return ret;

	/* [한국어] 모든 준비가 끝났다. 리셋 제어 레지스터를 다시 읽는다. */
	val = readl(rcar->base + PCIERSTCTRL1);
	/* [한국어] 이제 LTSSM 을 켠다. 이 쓰기 이후 링크 훈련이 시작된다. */
	val |= APP_LTSSM_ENABLE;
	/* [한국어] 반영한다. 여기까지가 켜는 절차의 끝이다. */
	writel(val, rcar->base + PCIERSTCTRL1);

	/* [한국어] 성공. 이후 속도 변경은 호출자인 start_link 가 맡는다. */
	return 0;
}

/* [한국어] 아래 넷은 DT compatible 마다 하나씩 대응하는 정적 표다. 두 축으로
 * 읽으면 된다 — 세로는 SoC 세대(r8a779f0 = R-Car S4 계열 / rcar-gen4 = V4H 계열),
 * 가로는 역할(RC / EP). 세대가 ltssm_control 과 additional_common_init 을 정하고,
 * 역할이 mode 를 정한다.
 *
 * [상류 코드 관찰] 넷 다 const 가 아니다. 내용이 런타임에 바뀌는 곳은 없고
 * of_device_id 의 .data 를 거쳐 const 포인터로만 읽히지만, 정의 자체는
 * `static struct` 다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */

/* [한국어] R-Car S4 계열, 루트 컴플렉스. 추가 공통 초기화 훅이 없다. */
static struct rcar_gen4_pcie_drvdata drvdata_r8a779f0_pcie = {
	/* [한국어] 비트 하나만 만지는 가벼운 LTSSM 절차. */
	.ltssm_control = r8a779f0_pcie_ltssm_control,
	/* [한국어] 루트 컴플렉스로 동작한다. */
	.mode = DW_PCIE_RC_TYPE,
};

/* [한국어] R-Car S4 계열, 엔드포인트. 위와 mode 만 다르다. */
static struct rcar_gen4_pcie_drvdata drvdata_r8a779f0_pcie_ep = {
	/* [한국어] 같은 LTSSM 절차를 쓴다 — 세대가 같으므로. */
	.ltssm_control = r8a779f0_pcie_ltssm_control,
	/* [한국어] 엔드포인트로 동작한다. */
	.mode = DW_PCIE_EP_TYPE,
};

/* [한국어] R-Car V4H 계열, 루트 컴플렉스. 추가 공통 초기화와 무거운 LTSSM 절차를 쓴다. */
static struct rcar_gen4_pcie_drvdata drvdata_rcar_gen4_pcie = {
	/* [한국어] 레인 skew 와 클럭 전원관리를 손보는 훅. S4 계열에는 없다. */
	.additional_common_init = rcar_gen4_pcie_additional_common_init,
	/* [한국어] SRIS 설정과 PHY 펌웨어까지 포함하는 절차. */
	.ltssm_control = rcar_gen4_pcie_ltssm_control,
	/* [한국어] 루트 컴플렉스로 동작한다. */
	.mode = DW_PCIE_RC_TYPE,
};

/* [한국어] R-Car V4H 계열, 엔드포인트. 위와 mode 만 다르다. */
static struct rcar_gen4_pcie_drvdata drvdata_rcar_gen4_pcie_ep = {
	/* [한국어] 같은 추가 초기화 훅. */
	.additional_common_init = rcar_gen4_pcie_additional_common_init,
	/* [한국어] 같은 LTSSM 절차 — 펌웨어 내려받기는 EP 모드에서도 필요하다. */
	.ltssm_control = rcar_gen4_pcie_ltssm_control,
	/* [한국어] 엔드포인트로 동작한다. */
	.mode = DW_PCIE_EP_TYPE,
};

/* [한국어] DT compatible 문자열과 위 네 표를 잇는 매칭 목록. RC 용과 EP 용
 * compatible 을 아예 다른 문자열로 나눈 것이 이 드라이버의 설계다 — 즉 한
 * 컨트롤러가 어느 역할로 설지는 런타임 협상이 아니라 DT 가 못박는다. */
static const struct of_device_id rcar_gen4_pcie_of_match[] = {
	/* [한국어] 첫 항목: R-Car S4 계열 RC. */
	{
		/* [한국어] DT 노드가 이 문자열을 쓰면 매칭된다. */
		.compatible = "renesas,r8a779f0-pcie",
		/* [한국어] 매칭 시 of_device_get_match_data() 가 돌려줄 표. */
		.data = &drvdata_r8a779f0_pcie,
	},
	/* [한국어] 둘째 항목: R-Car S4 계열 EP. */
	{
		/* [한국어] EP 전용 compatible. 접미사 "-ep" 가 역할을 가른다. */
		.compatible = "renesas,r8a779f0-pcie-ep",
		/* [한국어] EP 모드 표. */
		.data = &drvdata_r8a779f0_pcie_ep,
	},
	/* [한국어] 셋째 항목: R-Car V4H 계열 RC. */
	{
		/* [한국어] 세대 이름이 SoC 품번이 아니라 계열명인 것이 위 둘과 다르다. */
		.compatible = "renesas,rcar-gen4-pcie",
		/* [한국어] V4H RC 표. */
		.data = &drvdata_rcar_gen4_pcie,
	},
	/* [한국어] 넷째 항목: R-Car V4H 계열 EP. */
	{
		/* [한국어] V4H EP 전용 compatible. */
		.compatible = "renesas,rcar-gen4-pcie-ep",
		/* [한국어] V4H EP 표. */
		.data = &drvdata_rcar_gen4_pcie_ep,
	},
	/* [한국어] 목록의 끝을 알리는 빈 항목. 매칭 코드가 이것을 보고 멈춘다. */
	{},
};
/* [한국어] 모듈 자동 로딩용 메타데이터. modpost 가 이 목록을 읽어 modules.alias 에
 * compatible 문자열을 넣어 준다. */
MODULE_DEVICE_TABLE(of, rcar_gen4_pcie_of_match);

/* [한국어] 플랫폼 드라이버 등록 정보. probe/remove 와 매칭 목록을 묶는다. */
static struct platform_driver rcar_gen4_pcie_driver = {
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name = "pcie-rcar-gen4",
		/* [한국어] 위의 DT 매칭 목록. 이것이 있어야 DT 노드와 이어진다. */
		.of_match_table = rcar_gen4_pcie_of_match,
		/* [한국어] 비동기 프로브를 선호한다고 알린다. 이 드라이버의 프로브가
		 * 100ms 대기와 펌웨어 로드로 오래 걸리므로, 부팅을 붙잡지 않게 하려는 것이다. */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	/* [한국어] 매칭된 디바이스마다 불릴 진입점. */
	.probe = rcar_gen4_pcie_probe,
	/* [한국어] 언바인드 시 불릴 정리 함수. */
	.remove = rcar_gen4_pcie_remove,
};
/* [한국어] 모듈 초기화/종료 함수를 자동 생성해 위 드라이버를 등록한다.
 * 이 한 줄이 module_init/module_exit 한 쌍을 대신한다. */
module_platform_driver(rcar_gen4_pcie_driver);

/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("Renesas R-Car Gen4 PCIe controller driver");
/* [한국어] 라이선스 선언. 파일 맨 위 SPDX 와 함께 GPL-2.0-only 임을 알린다.
 * 이 선언이 없으면 커널이 모듈을 오염(tainted)으로 표시한다. */
MODULE_LICENSE("GPL");
