// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe controller driver for Intel Keem Bay
 * Copyright (C) 2020 Intel Corporation
 */

/*
 * [한국어 설명] 인텔 Keem Bay SoC 의 DesignWare PCIe 글루 드라이버 (pcie-keembay.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare(DWC) PCIe 코어를 인텔 Keem Bay SoC(VPU 를 얹은
 * 비전 처리용 SoC)에 붙이는 글루 드라이버다. config 공간 접근, ATU 창
 * 설정, 버스 열거, MSI 도메인 생성 같은 "PCIe 답게 동작하는 부분" 은 전부
 * pcie-designware-host.c / pcie-designware-ep.c 가 하고, 이 파일은 그
 * 공통 코어가 알 수 없는 SoC 고유의 것만 맡는다.
 *
 * 맡는 일이 다섯이다.
 *   1) APB 슬레이브 창(apb_base)의 SoC 전용 레지스터로 LTSSM 을 켜고 끄고,
 *      링크 상태를 읽는다. DWC 코어의 표준 LINK_ 계열 레지스터가 아니라
 *      SoC 가 따로 뽑아 놓은 SII(Sideband Interface) 상태 레지스터를 본다.
 *   2) SoC 내장 저지터 PLL(LJPLL)을 손으로 프로그래밍해 잠글 때까지 기다린다.
 *      레퍼런스 클럭을 이 PLL 이 만들기 때문에 링크를 켜기 전에 필수다.
 *   3) 하위 장치로 나가는 PERST# GPIO 를 규격이 요구하는 시간만큼 눌렀다 뗀다.
 *   4) MSI 를 위한 **체인 인터럽트 핸들러** 를 직접 단다. Keem Bay 는 표준
 *      DWC IP 위에 상태 레지스터에 1 을 써서 지우는 별도 로직을 얹었기 때문에,
 *      DWC 코어의 기본 체인 핸들러(dw_chained_msi_isr)를 쓸 수 없다.
 *   5) 같은 IP 를 엔드포인트(EP)로도 쓸 수 있게 EP 콜백 한 벌을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 "플랫폼 드라이버 <-> DWC 공통 코어" 사이의 얇은 층이다.
 * 부팅 시 RC(루트 컴플렉스) 모드 흐름:
 *   플랫폼 드라이버 코어 -> keembay_pcie_probe()
 *     -> device_get_match_data() 로 RC/EP 를 가른다
 *     -> "apb" 자원을 ioremap 해 SoC 레지스터 창을 얻는다
 *     -> keembay_pcie_add_pcie_port()
 *        -> keembay_pcie_setup_msi_irq()  : "pcie" IRQ 에 체인 핸들러를 건다
 *        -> devm_gpiod_get("reset")       : PERST# 선을 얻는다(HIGH = 눌린 상태)
 *        -> keembay_pcie_probe_clocks()   : master/aux 클럭 두 개를 켠다
 *        -> PHY0_SRAM_BYPASS 를 세워 PHY 펌웨어 로딩을 건너뛴다
 *        -> PCIE_DEVICE_TYPE 을 써서 RC 모드로 못박는다
 *        -> keembay_pcie_pll_init()       : LJPLL 을 프로그래밍하고 잠금을 기다린다
 *        -> PCIE_RSTN 을 세워 코어 리셋을 푼다
 *        -> keembay_ep_reset_deassert()   : PERST# 를 뗀다
 *        -> dw_pcie_host_init()           [pcie-designware-host.c]
 *           -> 그 안에서 dw_pcie_start_link() -> [이 파일] keembay_pcie_start_link()
 *              -> MPLLA 잠금을 기다린 뒤 LTSSM 을 켠다
 *           -> 링크 훈련, ATU 설정, 버스 스캔
 *        -> MSI_CTRL_INT_EN 을 세워 MSI 인터럽트를 연다
 *
 * EP 모드 흐름은 훨씬 짧다:
 *   keembay_pcie_probe() -> dw_pcie_ep_init() -> dw_pcie_ep_init_registers()
 *     -> pci_epc_init_notify()
 *   그 과정에서 dw_pcie_ep_ops.init 콜백으로 [이 파일] keembay_pcie_ep_init()
 *   이 불려 eDMA 인터럽트만 연다. **EP 모드에서는 클럭·PLL·PERST#·LTSSM 을
 *   이 파일이 전혀 건드리지 않는다** — 레퍼런스 클럭과 PERST# 를 상대편
 *   호스트가 주기 때문이다.
 *
 * 실행 컨텍스트: probe 경로와 그 하위(클럭·GPIO·PLL 대기)는 전부 프로세스
 * 컨텍스트이며 잠들 수 있다(msleep/usleep_range/gpiod_..._cansleep 를 쓴다).
 * 반대로 keembay_pcie_msi_irq_handler() 는 인터럽트 컨텍스트의 체인 핸들러라
 * 절대 잠들면 안 된다. keembay_pcie_link_up()/start_link()/stop_link() 는
 * DWC 코어가 부르며 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. RC 모드에서는 dw_pcie_host_init() 이 host bridge 를 등록해
 *   pci_host_probe() 로 버스를 스캔하게 한다. EP 모드에서는 pci-epc-core 의
 *   pci_epc_init_notify() 로 EPF 드라이버들에게 준비 완료를 알린다.
 * 아래쪽: pcie-designware.c / pcie-designware-host.c / pcie-designware-ep.c.
 *   접점이 콜백 표 세 벌이다 —
 *     dw_pcie_ops      (link_up, start_link, stop_link)
 *     dw_pcie_host_ops (내용이 하나도 없는 빈 표)
 *     dw_pcie_ep_ops   (init, raise_irq, get_features)
 *   빈 호스트 표를 굳이 두는 이유가 있다. pcie-designware-host.c:1511 이
 *   pp->ops 자체의 NULL 검사 없이 `pp->ops->init` 을 역참조하므로,
 *   pp->ops 는 반드시 NULL 이 아니어야 한다. 그래서 모든 필드가 NULL 인
 *   표를 하나 만들어 걸어 둔다.
 * 옆쪽: clk / gpio(consumer) / irqchip 계층. 셋 다 이 트리(drivers/{block,
 *   nvme,pci,s390,vfio} 만 있는 희소 체크아웃)에 없어 내부는 확인 대상 밖이며,
 *   이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리(compatible 로 RC/EP 결정, "apb" 레지스터 창, "pcie" 인터럽트,
 *                 "reset" GPIO, "master"/"aux" 클럭)
 *     -> keembay_pcie_probe() -> struct keembay_pcie
 *     -> 그 안의 apb_base 로 모든 SoC 레지스터 접근이 흐른다.
 *   MSI: 하위 장치의 MSI 쓰기 -> SoC 인터럽트 상태 레지스터에 MSI_CTRL_INT
 *     -> "pcie" IRQ -> keembay_pcie_msi_irq_handler() -> dw_handle_msi_irq()
 *     -> DWC 의 MSI IRQ 도메인 -> 장치 드라이버의 핸들러
 *
 * 공유 상태: struct keembay_pcie 하나뿐이다. probe 가 채운 뒤로는 사실상
 *   불변이고 별도 잠금이 없다. dev_get_drvdata() 로 어디서든 되찾는다 —
 *   dw_pcie 를 포인터가 아니라 **값으로** 품고 있으므로 container_of 로도
 *   되찾을 수 있지만, 이 파일은 일관되게 drvdata 를 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * keembay_pcie_probe()           : 진입점. RC/EP 를 갈라 각각의 초기화로 보낸다.
 * keembay_pcie_add_pcie_port()   : RC 모드 초기화 전부 — MSI 체인 핸들러, GPIO,
 *                                  클럭, PHY SRAM 우회, RC 모드 지정, PLL, 리셋 해제,
 *                                  dw_pcie_host_init() 까지.
 * keembay_pcie_pll_init()        : 내장 저지터 PLL(LJPLL)을 프로그래밍하고 잠금을 기다린다.
 * keembay_pcie_start_link()      : MPLLA 잠금을 기다린 뒤 LTSSM 을 켠다. EP 모드에서는 아무것도 안 한다.
 * keembay_pcie_msi_irq_handler() : SoC 고유의 W1C 상태 레지스터를 다루는 체인 핸들러.
 * keembay_pcie_probe_clock()     : 클럭 하나를 얻고 속도를 맞추고 켠 뒤, devm 액션으로 끄기를 예약한다.
 * struct keembay_pcie            : dw_pcie 를 **값으로** 품은 이 드라이버의 상태 전부.
 * struct keembay_pcie_of_data    : 디바이스 트리 match 데이터. RC/EP 를 구별하는 데만 쓴다.
 *
 * === NVMe 관점 ===
 * 이 루트 포트 아래에 NVMe 컨트롤러를 달면, 그 컨트롤러의 완료 인터럽트는
 * MSI/MSI-X 로 올라와 위의 MSI 경로를 그대로 탄다. 즉 NVMe 완료 큐 하나가
 * 인터럽트를 올릴 때마다 keembay_pcie_msi_irq_handler() 가 인터럽트
 * 컨텍스트에서 한 번 돌고, 거기서 dw_handle_msi_irq() 가 해당 벡터의
 * 리눅스 IRQ 를 재분배한다. config 공간 접근은 이 파일을 지나지 않고
 * DWC 코어의 ATU 기반 창으로 곧장 간다.
 */

/* [한국어] FIELD_PREP() 매크로. 아래 LJPLL 계열 필드처럼 '마스크와 값' 으로 표현된
 * 비트 필드에 값을 밀어 넣을 때 쓴다 — keembay_pcie_pll_init() 이 다섯 번 쓴다. */
#include <linux/bitfield.h>
/* [한국어] BIT(n) 과 GENMASK(hi, lo). 바로 아래 레지스터 비트 정의가 전부 이 둘로 쓰여 있다. */
#include <linux/bits.h>
/* [한국어] devm_clk_get(), clk_set_rate(), clk_prepare_enable(), clk_disable_unprepare().
 * keembay_pcie_probe_clock() 이 master/aux 클럭 두 개를 다루는 데 쓴다. */
#include <linux/clk.h>
/* [한국어] msleep() 과 usleep_range(). PERST# 를 규격이 요구하는 시간만큼 누르고 떼려면 필요하다. */
#include <linux/delay.h>
/* [한국어] IS_ERR()/PTR_ERR()/ERR_PTR(). 클럭 조회 실패를 포인터에 실어 나르는 관용구. */
#include <linux/err.h>
/* [한국어] gpiod_set_value_cansleep() 과 devm_gpiod_get(). PERST# 선을 GPIO 로 다룬다.
 * _cansleep 판을 쓴다는 것은 이 경로가 프로세스 컨텍스트임을 뜻한다. */
#include <linux/gpio/consumer.h>
/* [한국어] __init 계열 선언. 이 파일은 builtin_platform_driver 로 등록되므로 초기화 매크로 계열이 필요하다. */
#include <linux/init.h>
/* [한국어] readl_poll_timeout(). MPLLA 잠금과 LJPLL 잠금을 기다리는 두 곳에서 쓴다.
 * 주기적으로 레지스터를 읽어 조건이 참이 될 때까지 도는 헬퍼다. */
#include <linux/iopoll.h>
/* [한국어] chained_irq_enter()/chained_irq_exit(). keembay_pcie_msi_irq_handler() 가
 * 체인(계단식) 인터럽트 핸들러라서, 부모 irq_chip 의 마스크/ack 를 이 두
 * 함수로 감싸 줘야 한다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] USEC_PER_MSEC 같은 기본 상수와 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] struct of_device_id. 아래 keembay_pcie_of_match[] 의 타입이다. */
#include <linux/mod_devicetable.h>
/* [한국어] PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 상수. keembay_pcie_ep_raise_irq() 의 분기 값이다. */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_get_irq_byname(),
 * devm_platform_ioremap_resource_byname(), builtin_platform_driver(). */
#include <linux/platform_device.h>
/* [한국어] device_get_match_data(). 디바이스 트리 compatible 에 매달아 둔
 * keembay_pcie_of_data 를 꺼내 RC/EP 를 가른다. */
#include <linux/property.h>

/* [한국어] DWC 공통 코어의 선언 전부 — struct dw_pcie, struct dw_pcie_rp, struct dw_pcie_ep,
 * enum dw_pcie_device_mode, dw_pcie_ops/host_ops/ep_ops 콜백 표, dw_pcie_host_init(),
 * dw_pcie_ep_init(), dw_handle_msi_irq(), DWC_EPC_COMMON_FEATURES 매크로 등. */
#include "pcie-designware.h"

/* PCIE_REGS_APB_SLV Registers */
/* [한국어] 장치 종류와 코어 리셋을 함께 담은 설정 레지스터(APB 창 기준 0x0004).
 * 두 필드가 한 레지스터에 있어 순서가 중요하다 — 종류를 먼저 정하고 리셋을 푼다. */
#define PCIE_REGS_PCIE_CFG		0x0004
/* [한국어] 비트 8 = 장치 종류. 세우면 루트 컴플렉스(RC)로 동작한다.
 * keembay_pcie_add_pcie_port() 가 이 비트만 담아 레지스터를 통째로 덮어쓴다. */
#define  PCIE_DEVICE_TYPE		BIT(8)
/* [한국어] 비트 0 = 코어 리셋 해제(RSTN 은 active-low 리셋의 '해제' 신호).
 * 세우면 PCIe 코어가 리셋에서 풀린다. 반드시 장치 종류를 정한 뒤에 세운다. */
#define  PCIE_RSTN			BIT(0)
/* [한국어] 애플리케이션 제어 레지스터(0x0008). SoC 가 코어에 넣는 사이드밴드 신호들이 모여 있다. */
#define PCIE_REGS_PCIE_APP_CNTRL	0x0008
/* [한국어] 비트 0 = LTSSM(Link Training and Status State Machine) 기동 허가.
 * 이 비트가 0 이면 코어는 링크 훈련을 시작하지 않는다. keembay_pcie_ltssm_set() 이 다룬다. */
#define  APP_LTSSM_ENABLE		BIT(0)
/* [한국어] 인터럽트 허가 레지스터(0x0028). 아래 상태 레지스터와 짝을 이룬다. */
#define PCIE_REGS_INTERRUPT_ENABLE	0x0028
/* [한국어] 비트 8 = MSI 컨트롤러 인터럽트 허가. RC 모드에서 CONFIG_PCI_MSI 가 켜져 있을 때만 세운다. */
#define  MSI_CTRL_INT_EN		BIT(8)
/* [한국어] 비트 7..0 = 내장 eDMA 채널 여덟 개의 인터럽트 허가.
 * keembay_pcie_ep_init() 이 EP 모드에서 이 여덟 비트만 켠다(MSI 비트는 끈다). */
#define  EDMA_INT_EN			GENMASK(7, 0)
/* [한국어] 인터럽트 상태 레지스터(0x002c). **W1C(1 을 쓰면 지워짐)** 다 —
 * Keem Bay 가 표준 DWC IP 위에 얹은 추가 로직이며, 이 파일이 체인 핸들러를
 * 직접 두는 이유가 바로 이 지우기 동작 때문이다. */
#define PCIE_REGS_INTERRUPT_STATUS	0x002c
/* [한국어] 비트 8 = MSI 컨트롤러 인터럽트 계류 중. 허가 비트(MSI_CTRL_INT_EN)와 같은 자리다. */
#define  MSI_CTRL_INT			BIT(8)
/* [한국어] SII(Sideband Interface) 전원/링크 상태 레지스터(0x00b0).
 * DWC 표준 레지스터가 아니라 SoC 가 따로 뽑아 놓은 상태 창이다. */
#define PCIE_REGS_PCIE_SII_PM_STATE	0x00b0
/* [한국어] 비트 19 = SMLH(물리 계층 상태 머신) 링크 업. 물리 계층이 L0 에 도달했다는 뜻. */
#define  SMLH_LINK_UP			BIT(19)
/* [한국어] 비트 8 = RDLH(데이터 링크 계층) 링크 업. DL_Active 에 도달했다는 뜻. */
#define  RDLH_LINK_UP			BIT(8)
/* [한국어] 둘을 합친 마스크. keembay_pcie_link_up() 은 **두 비트가 모두** 서야 링크가 섰다고 본다.
 * 물리 계층만 올라오고 데이터 링크가 아직인 중간 상태를 링크 업으로 오인하지 않기 위해서다.
 * [상류 코드 관찰] 이 이름만 PCIE_REGS_ 접두사를 달고 있지만 레지스터 오프셋이 아니라
 * 비트 마스크다 — 위아래의 오프셋 상수들과 이름 규칙이 어긋난다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#define  PCIE_REGS_PCIE_SII_LINK_UP	(SMLH_LINK_UP | RDLH_LINK_UP)
/* [한국어] PHY 제어 레지스터(0x0164). */
#define PCIE_REGS_PCIE_PHY_CNTL		0x0164
/* [한국어] 비트 8 = PHY0 의 SRAM 우회. 세우면 PHY 펌웨어를 SRAM 에 올리는 단계를 건너뛰고
 * 내장 ROM 설정으로 곧장 동작한다. 부팅 시간을 줄이는 선택으로 보이나
 * 구체적 근거는 이 트리에서 확인 못 함. */
#define  PHY0_SRAM_BYPASS		BIT(8)
/* [한국어] PHY 상태 레지스터(0x0168). */
#define PCIE_REGS_PCIE_PHY_STAT		0x0168
/* [한국어] 비트 1 = MPLLA(PHY 안의 멀티 PLL A) 잠김.
 * keembay_pcie_start_link() 가 LTSSM 을 켜기 전에 이 비트를 최대 500ms 기다린다. */
#define  PHY0_MPLLA_STATE		BIT(1)
/* [한국어] 저지터 PLL 상태 레지스터(0x016c). LJPLL 은 Low Jitter PLL 의 약자로,
 * SoC 내장 PCIe 레퍼런스 클럭 생성기다. */
#define PCIE_REGS_LJPLL_STA		0x016c
/* [한국어] 비트 0 = LJPLL 잠김. keembay_pcie_pll_init() 이 최대 500ms 기다린다. */
#define  LJPLL_LOCK			BIT(0)
/* [한국어] LJPLL 제어 레지스터 0(0x0170). 켜기와 출력 허가가 들어 있어 **맨 마지막에** 쓴다. */
#define PCIE_REGS_LJPLL_CNTRL_0		0x0170
/* [한국어] 비트 29 = LJPLL 활성화. 이 비트를 세우는 순간 PLL 이 잠금 절차에 들어가므로,
 * 분주비를 모두 써 넣은 뒤에 세워야 한다. */
#define  LJPLL_EN			BIT(29)
/* [한국어] 비트 24..21 = 네 개의 출력 클럭 각각의 허가 비트.
 * keembay_pcie_pll_init() 이 0xc(=0b1100)를 넣어 위쪽 두 출력만 켠다. */
#define  LJPLL_FOUT_EN			GENMASK(24, 21)
/* [한국어] LJPLL 제어 레지스터 2(0x0178). 입력/피드백 분주비가 들어 있다. */
#define PCIE_REGS_LJPLL_CNTRL_2		0x0178
/* [한국어] 비트 17..12 = 레퍼런스 클럭 입력 분주비. 0 을 넣는다(= 분주하지 않음). */
#define  LJPLL_REF_DIV			GENMASK(17, 12)
/* [한국어] 비트 11..0 = 피드백 분주비. 0x32(=50)를 넣는다. 이 값이 곧 배수라서
 * VCO 주파수 = 입력 x 50 이 된다. */
#define  LJPLL_FB_DIV			GENMASK(11, 0)
/* [한국어] LJPLL 제어 레지스터 3(0x017c). 후단 분주비가 들어 있다. */
#define PCIE_REGS_LJPLL_CNTRL_3		0x017c
/* [한국어] 비트 24..22 = 후단 분주기 3A. 0x2 를 넣는다. */
#define  LJPLL_POST_DIV3A		GENMASK(24, 22)
/* [한국어] 비트 18..16 = 후단 분주기 2A. 역시 0x2 를 넣는다.
 * 두 후단 분주를 합쳐 VCO 출력이 최종 레퍼런스 클럭 주파수로 내려간다. */
#define  LJPLL_POST_DIV2A		GENMASK(18, 16)

/* [한국어] PERST# 를 누르거나 뗀 뒤 최소로 기다릴 시간(마이크로초).
 * usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500) 형태로 쓰여, 1ms~1.5ms 사이에 깨어난다. */
#define PERST_DELAY_US		1000
/* [한국어] 보조(aux) 클럭에 요구할 주파수 24MHz.
 * keembay_pcie_probe_clock() 에 rate 인자로 넘겨 clk_set_rate() 를 부르게 한다.
 * master 클럭은 rate 0 으로 넘겨 속도를 건드리지 않는다. */
#define AUX_CLK_RATE_HZ		24000000

/* [한국어] 이 드라이버가 컨트롤러 하나에 대해 들고 있는 상태 전부.
 * devm_kzalloc() 으로 keembay_pcie_probe() 가 한 번 잡고,
 * platform_set_drvdata() 로 심어 둔 뒤 dev_get_drvdata() 로 되찾는다.
 * 첫 필드가 dw_pcie 라 container_of 로도 되찾을 수 있지만 이 파일은 쓰지 않는다. */
struct keembay_pcie {
	/* [한국어] DWC 공통 코어가 요구하는 컨트롤러 서술자를 **포인터가 아니라 값으로** 품는다.
	 * 그래서 이 구조체를 한 번 할당하면 dw_pcie 도 함께 할당된다 — 별도 관리가 없다.
	 * 설정자: keembay_pcie_probe() 가 pci->dev 와 pci->ops 를 채운다.
	 *   RC 모드에서는 keembay_pcie_add_pcie_port() 가 pci->pp 를, EP 모드에서는
	 *   probe 가 pci->ep.ops 를 추가로 채운다.
	 * 읽는 자: DWC 코어 전부(pcie-designware-host.c / -ep.c / pcie-designware.c)와
	 *   이 파일의 to_dw_pcie_from_ep() 역참조.
	 * 값 범위: devm_kzalloc 으로 0 초기화된 뒤 probe 가 채운 유효한 구조체.
	 * 동기화: probe 이후 이 파일이 다시 쓰는 곳은 없다. 내부 필드의 동기화는 DWC 코어가 맡는다. */
	struct dw_pcie		pci;
	/* [한국어] SoC 전용 APB 슬레이브 레지스터 창의 매핑 주소.
	 * 위에 정의한 PCIE_REGS_ 계열 오프셋이 전부 이 주소를 기준으로 더해진다.
	 * 설정자: keembay_pcie_probe() 의 devm_platform_ioremap_resource_byname(pdev, "apb").
	 * 읽는 자: 이 파일의 모든 readl()/writel() — ltssm_set, link_up, start_link,
	 *   pll_init, msi_irq_handler, ep_init, add_pcie_port.
	 * 값 범위: 유효한 __iomem 포인터. 실패는 IS_ERR() 로 걸러 probe 가 즉시 접는다.
	 * 동기화: probe 이후 불변. 이 포인터로 하는 접근들 사이의 경쟁은
	 *   아래 keembay_pcie_msi_irq_handler() 주석 참고 — 별도 잠금이 없다. */
	void __iomem		*apb_base;
	/* [한국어] 이 인스턴스가 RC 로 동작하는지 EP 로 동작하는지.
	 * 왜 필요한가: 같은 IP 를 두 역할로 합성할 수 있어, 디바이스 트리 compatible
	 *   ("intel,keembay-pcie" 대 "intel,keembay-pcie-ep")로만 구분된다.
	 * 설정자: keembay_pcie_probe() 가 keembay_pcie_of_data.mode 를 그대로 복사한다.
	 * 읽는 자: keembay_pcie_probe() 의 switch 와 keembay_pcie_start_link() 의 조기 반환.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE. of_data 표가 둘 중 하나만
	 *   주므로 DW_PCIE_UNKNOWN_TYPE 은 실제로 오지 않지만, probe 의 default 가
	 *   그래도 -ENODEV 로 막아 둔다.
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기 전용. */
	enum dw_pcie_device_mode mode;

	/* [한국어] 컨트롤러 본체를 돌리는 주 클럭.
	 * 설정자: keembay_pcie_probe_clocks() 가 keembay_pcie_probe_clock(dev, "master", 0)
	 *   으로 얻는다. rate 가 0 이라 주파수는 건드리지 않고 켜기만 한다.
	 * 읽는 자: 이 파일에서 이 필드를 다시 읽는 곳은 없다(전수 확인). 보관만 한다 —
	 *   끄기는 devm_add_action_or_reset 으로 예약해 두었기 때문이다.
	 * 값 범위: 유효한 struct clk 포인터. 오류는 IS_ERR 로 걸러 add_pcie_port 가 접는다.
	 * 동기화: RC 모드 probe 에서만 설정. EP 모드에서는 **끝까지 NULL 로 남는다.** */
	struct clk		*clk_master;
	/* [한국어] 24MHz 로 맞춰 켜는 보조 클럭.
	 * 설정자: keembay_pcie_probe_clocks() 가 rate 를 AUX_CLK_RATE_HZ 로 주어 얻는다.
	 *   master 와 달리 clk_set_rate() 가 실제로 불린다.
	 * 읽는 자: 역시 이 파일에서 다시 읽지 않는다(전수 확인).
	 * 값 범위: 유효한 struct clk 포인터.
	 * 동기화: RC 모드 probe 에서만 설정. EP 모드에서는 NULL 로 남는다. */
	struct clk		*clk_aux;
	/* [한국어] 하위 슬롯으로 나가는 PERST# 리셋 선의 GPIO 서술자.
	 * 왜 필요한가: PCIe 규격은 레퍼런스 클럭이 안정된 뒤 PERST# 를 최소 100ms
	 *   눌렀다 떼도록 요구한다(PCIe CEM 1.1 Table 2-4). 그 신호를 SoC 가 아니라
	 *   보드의 GPIO 로 낸다.
	 * 설정자: keembay_pcie_add_pcie_port() 의 devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH).
	 *   초기값이 HIGH 라 얻는 순간 이미 리셋이 눌린 상태다.
	 * 읽는 자: keembay_ep_reset_assert()/deassert() 의 gpiod_set_value_cansleep().
	 * 값 범위: 유효한 gpio_desc 포인터. 이 드라이버는 optional 판을 쓰지 않으므로
	 *   디바이스 트리에 reset-gpios 가 없으면 RC 프로브가 실패한다.
	 * 동기화: RC 모드 probe 에서만 설정. EP 모드에서는 NULL 이며, 그래서 EP 경로가
	 *   reset assert/deassert 를 부르지 않는 것과 짝이 맞는다. */
	struct gpio_desc	*reset;
};

/* [한국어] 디바이스 트리 compatible 마다 매달아 두는 match 데이터.
 * 지금은 필드가 mode 하나뿐이라 사실상 enum 하나를 감싼 것이지만,
 * 구조체로 둔 덕에 나중에 SoC 별 차이가 늘어도 표만 고치면 된다.
 * 인스턴스는 아래 keembay_pcie_rc_of_data / keembay_pcie_ep_of_data 둘이며
 * 둘 다 static const 라 읽기 전용이다. */
struct keembay_pcie_of_data {
	/* [한국어] 이 compatible 이 뜻하는 역할.
	 * 설정자: 파일 아래쪽의 두 static const 인스턴스가 초기화자로 정한다.
	 * 읽는 자: keembay_pcie_probe() 가 device_get_match_data() 로 이 구조체를 얻은 뒤
	 *   data->mode 를 읽어 pcie->mode 로 옮긴다.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: 컴파일 시점 상수라 불변. 잠금이 필요 없다. */
	enum dw_pcie_device_mode mode;
};

/* [한국어]
 * keembay_ep_reset_assert - 하위 슬롯으로 나가는 PERST# 를 누른다
 *
 * @pcie: 이 컨트롤러의 상태. pcie->reset 이 유효해야 한다.
 * @return: 없음.
 *
 * PCIe 규격의 PERST# 는 active-low 신호지만, 디바이스 트리가 GPIO 의 극성을
 * 기술해 주므로 여기서는 논리값 1 = "리셋을 건다" 로 다룬다. 실제 전기적
 * 레벨이 무엇인지는 gpiod 계층이 알아서 뒤집는다.
 *
 * 동작은 두 단계다.
 *   1) GPIO 를 1 로 몰아 리셋을 건다.
 *   2) 1ms~1.5ms 기다려 하위 장치가 신호를 확실히 인식하게 한다.
 *
 * 이름이 keembay_"ep"_reset 이지만 EP 모드와는 무관하다. 이 함수를 부르는
 * 곳은 RC 경로 하나뿐이며, 여기서 ep 는 "이 루트 포트 아래에 달린
 * 엔드포인트" 를 가리킨다. EP 모드 probe 경로는 이 함수를 부르지 않는다
 * (pcie->reset 이 NULL 이라 부를 수도 없다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. gpiod_set_value_cansleep() 과
 * usleep_range() 가 모두 잠들 수 있으므로 인터럽트 컨텍스트에서 부르면 안 된다.
 *
 * 에러 경로: 없다. 반환값이 없어 GPIO 조작 실패를 알릴 방법이 애초에 없다.
 *
 * 호출 체인:
 *   keembay_pcie_add_pcie_port() (dw_pcie_host_init 실패 시)
 *     → [이 함수] → gpiod_set_value_cansleep() → usleep_range()
 */
static void keembay_ep_reset_assert(struct keembay_pcie *pcie)
{
	/* [한국어] 논리값 1 = 리셋을 건다. GPIO 의 실제 극성 뒤집기는 디바이스 트리 서술을 보고
	 * gpiod 계층이 알아서 한다. _cansleep 판이라 이 경로는 잠들 수 있어야 한다. */
	gpiod_set_value_cansleep(pcie->reset, 1);
	/* [한국어] 신호가 하위 장치에 확실히 인식되도록 1ms~1.5ms 기다린다. 범위를 주는 것은
	 * 커널이 다른 타이머와 묶어 깨울 수 있게 해 전력·인터럽트 비용을 줄이기 위해서다. */
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

/* [한국어]
 * keembay_ep_reset_deassert - PERST# 를 규격 시간만큼 누른 뒤 뗀다
 *
 * @pcie: 이 컨트롤러의 상태. pcie->reset 이 유효해야 한다.
 * @return: 없음.
 *
 * 이름은 "뗀다" 지만 실제로는 **먼저 100ms 를 기다린 뒤** 뗀다. 그 100ms 가
 * 이 함수의 핵심이다. PCIe CEM(Card Electromechanical) 1.1 Table 2-4 는
 * 전원과 레퍼런스 클럭이 안정된 시점부터 PERST# 를 최소 100ms 유지하도록
 * 요구한다. 호출자가 이미 GPIOD_OUT_HIGH 로 리셋을 걸어 둔 상태이므로,
 * 여기서 남은 시간을 채우고 떼는 것이다.
 *
 * 동작은 세 단계다.
 *   1) msleep(100) 으로 규격이 요구하는 유지 시간을 채운다.
 *   2) GPIO 를 0 으로 내려 리셋을 뗀다.
 *   3) 다시 1ms~1.5ms 기다려 하위 장치가 링크 훈련을 준비할 틈을 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 이 있으므로 반드시 잠들 수 있는
 * 곳에서만 불러야 한다. 실제 호출 지점은 probe 경로 하나다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   keembay_pcie_add_pcie_port() → [이 함수]
 *     → msleep() → gpiod_set_value_cansleep() → usleep_range()
 */
static void keembay_ep_reset_deassert(struct keembay_pcie *pcie)
{
	/*
	 * Ensure that PERST# is asserted for a minimum of 100ms.
	 *
	 * For more details, refer to PCI Express Card Electromechanical
	 * Specification Revision 1.1, Table-2.4.
	 */
	/* [한국어] PCIe CEM 1.1 Table 2-4 가 요구하는 최소 유지 시간. 호출자가 GPIOD_OUT_HIGH 로
	 * 이미 리셋을 걸어 둔 상태에서 이 100ms 를 채운다. msleep 이므로 프로세스 컨텍스트 전용이다. */
	msleep(100);

	/* [한국어] 논리값 0 = 리셋을 뗀다. 이 시점부터 하위 장치가 링크 훈련을 시작할 수 있다. */
	gpiod_set_value_cansleep(pcie->reset, 0);
	/* [한국어] 뗀 뒤에도 1ms~1.5ms 준다. 곧바로 LTSSM 을 켜면 하위 장치가 아직 준비되지 않았을 수 있다. */
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

/* [한국어]
 * keembay_pcie_ltssm_set - LTSSM 기동 허가 비트를 켜거나 끈다
 *
 * @pcie: 이 컨트롤러의 상태. apb_base 가 유효해야 한다.
 * @enable: true 면 링크 훈련을 시작하게 하고, false 면 멈춘다.
 * @return: 없음.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크를 실제로
 * 훈련시키는 상태 머신이다. DWC 코어 자체는 이 비트를 모르며, SoC 가
 * APB 창의 애플리케이션 제어 레지스터에 뽑아 놓았다. 그래서 start_link /
 * stop_link 훅을 이 파일이 채워야 한다.
 *
 * **읽기-수정-쓰기가 필수다.** 같은 레지스터에 다른 사이드밴드 신호가 함께
 * 있을 수 있어 통째로 쓰면 그것들이 지워진다.
 *
 * 실행 컨텍스트: RC 프로브 경로(프로세스 컨텍스트)와, DWC 코어가 부르는
 * start_link/stop_link 경로 양쪽에서 불린다. 잠들지 않는다.
 *
 * 에러 경로: 없다. readl/writel 은 실패를 보고하지 않는다.
 *
 * 호출 체인:
 *   keembay_pcie_start_link() / keembay_pcie_stop_link()
 *     → [이 함수] → readl() → writel()
 */
static void keembay_pcie_ltssm_set(struct keembay_pcie *pcie, bool enable)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 값. 레지스터의 나머지 비트를 지키려면 통째로 쓸 수 없다. */
	u32 val;

	/* [한국어] 현재 애플리케이션 제어 레지스터 값을 읽는다(APB 창 기준 0x0008). */
	val = readl(pcie->apb_base + PCIE_REGS_PCIE_APP_CNTRL);
	/* [한국어] 켜라는 요청이면, */
	if (enable)
		/* [한국어] LTSSM 기동 허가 비트(비트 0)를 세운다. 이 순간부터 코어가 링크 훈련을 시작한다. */
		val |= APP_LTSSM_ENABLE;
	/* [한국어] 끄라는 요청이면 — */
	else
		/* [한국어] 같은 비트를 지운다. 링크가 서 있었다면 내려간다. */
		val &= ~APP_LTSSM_ENABLE;
	/* [한국어] 수정한 값을 되쓴다. 나머지 사이드밴드 비트는 읽은 그대로 보존된다. */
	writel(val, pcie->apb_base + PCIE_REGS_PCIE_APP_CNTRL);
}

/* [한국어]
 * keembay_pcie_link_up - 링크가 실제로 올라왔는지 SII 상태로 판정한다
 *
 * @pci: DWC 코어의 컨트롤러 서술자. dev_get_drvdata(pci->dev) 로 이 드라이버
 *   상태를 되찾는다.
 * @return: 두 링크 비트가 **모두** 서 있으면 true, 아니면 false.
 *
 * dw_pcie_ops.link_up 콜백이다. DWC 코어는 링크 상태를 확인할 때마다 이
 * 훅을 거치므로, 코어 쪽 코드는 SoC 가 상태를 어디에 뽑아 놓았는지 몰라도 된다.
 *
 * 판정이 두 비트를 요구하는 것이 중요하다.
 *   SMLH_LINK_UP(비트 19) : 물리 계층 상태 머신이 L0 에 도달
 *   RDLH_LINK_UP(비트 8)  : 데이터 링크 계층이 활성(DL_Active)
 * 둘 중 하나만 서 있는 상태는 훈련이 아직 끝나지 않은 중간 단계다. 마스크와
 * 같은지 비교(== PCIE_REGS_PCIE_SII_LINK_UP)하므로 "둘 다" 라는 조건이
 * 정확히 표현된다 — 단순히 & 결과가 0 이 아닌지만 봤다면 중간 상태를
 * 링크 업으로 오인했을 것이다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 폴링 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. 레지스터 읽기가 실패해도(창이 죽어 ~0 이 읽혀도) 그대로
 * true 를 돌려준다 — 이 판정 방식의 구조적 한계다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_link_up) → dw_pcie_ops.link_up == [이 함수] → readl()
 */
static bool keembay_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] dw_pcie 포인터에서 이 드라이버 상태를 되찾는다. probe 가 platform_set_drvdata 로
	 * 심어 두었기에 가능하다. dw_pcie 가 keembay_pcie 의 첫 필드라 container_of 도
	 * 가능하지만 이 파일은 일관되게 drvdata 를 쓴다. */
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);
	/* [한국어] SII 상태 레지스터에서 읽어 올 값. */
	u32 val;

	/* [한국어] SII 전원/링크 상태 레지스터(0x00b0)를 한 번 읽는다. 표준 DWC 레지스터가 아니라
	 * SoC 가 따로 뽑아 놓은 창이다. */
	val = readl(pcie->apb_base + PCIE_REGS_PCIE_SII_PM_STATE);

	/* [한국어] 두 비트가 **모두** 서 있는지 검사한다. 마스크와 같은지 비교하므로 '둘 다' 라는
	 * 조건이 정확히 표현된다 — SMLH 만 서고 RDLH 가 아직인 훈련 중간 상태를
	 * 링크 업으로 오인하지 않는다. */
	return (val & PCIE_REGS_PCIE_SII_LINK_UP) == PCIE_REGS_PCIE_SII_LINK_UP;
}

/* [한국어]
 * keembay_pcie_start_link - PHY 의 MPLLA 잠금을 기다린 뒤 LTSSM 을 켠다
 *
 * @pci: DWC 코어의 컨트롤러 서술자.
 * @return: 0 = 성공. 음수 = MPLLA 가 제한 시간 안에 잠기지 않음(readl_poll_timeout
 *   의 -ETIMEDOUT). 호출자인 DWC 코어가 그대로 프로브 실패로 올린다.
 *
 * dw_pcie_ops.start_link 콜백이다. 링크 훈련을 시작하려면 PHY 안의
 * MPLLA(멀티 PLL A)가 먼저 잠겨 있어야 한다 — 잠기지 않은 상태에서 LTSSM 을
 * 켜면 훈련이 엉뚱하게 실패한다. 그래서 이 함수는 "끄기 → 기다리기 → 켜기"
 * 순서를 밟는다.
 *
 * 단계별로:
 *   1) EP 모드면 아무것도 하지 않고 0 을 돌려준다. **EP 에서는 이 드라이버가
 *      LTSSM 을 켜지 않는다** — 링크를 여는 쪽은 상대편 호스트다.
 *   2) LTSSM 을 일단 끈다. 부트로더가 켜 놓았을 수 있어, 잠금을 기다리는
 *      동안 훈련이 진행되는 일을 막는 것이다.
 *   3) PHY 상태 레지스터에서 PHY0_MPLLA_STATE 를 20us 간격으로 최대 500ms 폴링한다.
 *   4) 잠겼으면 LTSSM 을 켠다. 이 시점부터 링크 훈련이 실제로 시작된다.
 *
 * 실행 컨텍스트: RC 프로브 경로에서 dw_pcie_host_init() 안쪽으로 불린다.
 * readl_poll_timeout() 은 udelay 기반이라 원자 문맥에서도 쓸 수 있지만,
 * 여기서는 프로세스 컨텍스트다. EP 모드에서는 pci_epc_start() 경로로도
 * 불릴 수 있으나(pcie-designware-ep.c:1847) 곧바로 0 을 돌려준다.
 *
 * 에러 경로: 잠금 실패 시 dev_err 로 "MPLLA is not locked" 를 남기고
 * -ETIMEDOUT 을 올린다. 이때 **LTSSM 은 꺼진 채로 남는다** — 2단계에서 껐고
 * 4단계에 도달하지 못했기 때문이다. 되감을 것이 없는 깔끔한 실패다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_start_link()
 *     → dw_pcie_ops.start_link == [이 함수]
 *     → keembay_pcie_ltssm_set(false) → readl_poll_timeout() → keembay_pcie_ltssm_set(true)
 */
static int keembay_pcie_start_link(struct dw_pcie *pci)
{
	/* [한국어] DWC 서술자에서 이 드라이버 상태를 되찾는다. mode 를 봐야 하기 때문이다. */
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);
	/* [한국어] readl_poll_timeout 이 매 회 읽은 값을 담을 변수. 매크로가 이 이름을 그대로 쓴다. */
	u32 val;
	/* [한국어] 폴링 결과(0 또는 -ETIMEDOUT). */
	int ret;

	/* [한국어] EP 모드면 — 링크를 여는 쪽은 상대편 호스트다. */
	if (pcie->mode == DW_PCIE_EP_TYPE)
		/* [한국어] 아무것도 하지 않고 성공으로 돌아간다. 이 드라이버는 EP 에서 LTSSM 을 켜지 않는다. */
		return 0;

	/* [한국어] 먼저 LTSSM 을 끈다. 부트로더가 켜 두었을 수 있어, MPLLA 잠금을 기다리는 동안
	 * 훈련이 진행되는 일을 막는 것이다. */
	keembay_pcie_ltssm_set(pcie, false);

	/* [한국어] PHY 상태 레지스터를 20us 간격으로 최대 500ms(500 x USEC_PER_MSEC) 폴링해
	 * PHY0_MPLLA_STATE(비트 1)가 설 때까지 기다린다. MPLLA 가 잠기지 않은 채
	 * LTSSM 을 켜면 훈련이 엉뚱하게 실패한다. */
	ret = readl_poll_timeout(pcie->apb_base + PCIE_REGS_PCIE_PHY_STAT,
				 val, val & PHY0_MPLLA_STATE, 20,
				 500 * USEC_PER_MSEC);
	/* [한국어] 제한 시간 안에 잠기지 않았다. */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 남긴다 — 이 단계의 실패는 로그 없이는 추적이 어렵다. */
		dev_err(pci->dev, "MPLLA is not locked\n");
		/* [한국어] -ETIMEDOUT 을 그대로 올린다. LTSSM 은 위에서 꺼 둔 채로 남으므로 되감을 것이 없다. */
		return ret;
	}

	/* [한국어] MPLLA 가 잠겼다. 이제 LTSSM 을 켜 링크 훈련을 실제로 시작한다. */
	keembay_pcie_ltssm_set(pcie, true);

	/* [한국어] 성공. 호출자인 DWC 코어가 이어서 링크가 설 때까지 기다린다. */
	return 0;
}

/* [한국어]
 * keembay_pcie_stop_link - LTSSM 을 꺼 링크를 내린다
 *
 * @pci: DWC 코어의 컨트롤러 서술자.
 * @return: 없음.
 *
 * dw_pcie_ops.stop_link 콜백이다. start_link 의 짝으로, 코어가 호스트를
 * 해제하거나(dw_pcie_host_deinit) EP 를 멈출 때(dw_pcie_ep_stop) 불린다.
 *
 * start_link 와 달리 EP/RC 를 가리지 않는다. EP 모드에서 이 드라이버가
 * LTSSM 을 켠 적이 없으므로 여기서 끄는 것은 무해한 no-op 에 가깝지만,
 * 부트로더가 켜 두었다면 실제로 꺼진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(해제 경로). 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_stop_link) → dw_pcie_ops.stop_link == [이 함수]
 *     → keembay_pcie_ltssm_set(false)
 */
static void keembay_pcie_stop_link(struct dw_pcie *pci)
{
	/* [한국어] DWC 서술자에서 이 드라이버 상태를 되찾는다. */
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);

	/* [한국어] LTSSM 을 꺼 링크를 내린다. start_link 와 달리 EP/RC 를 가리지 않는다. */
	keembay_pcie_ltssm_set(pcie, false);
}

/* [한국어] DWC 코어가 컨트롤러 수준에서 쓰는 콜백 표. keembay_pcie_probe() 가 pci->ops 에
 * 건다. 세 훅만 채우고 read_dbi/write_dbi 같은 나머지는 NULL 로 남겨 코어의
 * 기본 접근을 그대로 쓰게 한다.
 * 설정자: 컴파일 시점 초기화자. static const 라 이후 불변.
 * 읽는 자: pcie-designware.c 의 dw_pcie_link_up()/start_link()/stop_link().
 * 값 범위: 세 함수 포인터 모두 이 파일 안의 정의를 가리킨다.
 * 동기화: 읽기 전용 상수라 잠금이 필요 없다. */
static const struct dw_pcie_ops keembay_pcie_ops = {
	/* [한국어] 링크 상태 판정 훅. 코어가 링크를 기다릴 때마다 부른다. */
	.link_up	= keembay_pcie_link_up,
	/* [한국어] 링크 기동 훅. dw_pcie_host_init() 과 pci_epc_start() 경로에서 불린다. */
	.start_link	= keembay_pcie_start_link,
	/* [한국어] 링크 정지 훅. 호스트/EP 해제 경로에서 불린다. */
	.stop_link	= keembay_pcie_stop_link,
};

/* [한국어]
 * keembay_pcie_disable_clock - devm 액션으로 등록해 두는 클럭 끄기 콜백
 *
 * @data: 등록 시 함께 넘긴 struct clk 포인터. void * 로 지워져 있어 여기서
 *   다시 struct clk 로 되돌린다.
 * @return: 없음.
 *
 * devm_add_action_or_reset() 이 받는 콜백은 void (*)(void *) 시그니처라야
 * 하므로, clk_disable_unprepare() 를 그대로 넘길 수 없다. 이 한 줄짜리
 * 래퍼가 그 타입 차이만 메운다.
 *
 * 왜 devm 액션인가: 이 드라이버에는 .remove 가 없고 suppress_bind_attrs 로
 * 언바인드도 막아 두었지만, **프로브 실패 시의 되감기** 는 여전히 필요하다.
 * 클럭을 켠 뒤 뒷단계(PLL, 호스트 초기화)가 실패하면 devm 이 등록 역순으로
 * 이 콜백을 불러 클럭을 꺼 준다.
 *
 * 실행 컨텍스트: devres 정리 경로 — 프로브 실패 직후 또는 장치 해제 시의
 * 프로세스 컨텍스트다. clk_disable_unprepare() 가 잠들 수 있으므로 그래야 한다.
 *
 * 에러 경로: 없다. 정리 콜백은 실패를 보고할 수 없다.
 *
 * 호출 체인:
 *   devres 정리(devres_release_all) → [이 함수] → clk_disable_unprepare()
 */
static inline void keembay_pcie_disable_clock(void *data)
{
	/* [한국어] void 로 지워져 온 인자를 struct clk 로 되돌린다. devm 액션 콜백 규약이
	 * void (*)(void *) 라서 생긴 한 단계다. */
	struct clk *clk = data;

	/* [한국어] 클럭을 끄고 준비 상태도 푼다. prepare 와 enable 을 한 번에 되감는 짝 함수다. */
	clk_disable_unprepare(clk);
}

/* [한국어]
 * keembay_pcie_probe_clock - 클럭 하나를 얻어 속도를 맞추고 켠 뒤 끄기를 예약한다
 *
 * @dev: 클럭을 조회할 기준 장치. 디바이스 트리의 clock-names 를 이 장치에서 찾는다.
 * @id: 클럭 이름("master" 또는 "aux").
 * @rate: 0 이 아니면 clk_set_rate() 로 이 주파수를 요구한다. 0 이면 속도를
 *   건드리지 않고 켜기만 한다.
 * @return: 성공하면 struct clk 포인터. 실패하면 ERR_PTR 로 감싼 음수 errno.
 *   호출자는 IS_ERR() 로 검사한 뒤 PTR_ERR() 로 이유를 꺼낸다.
 *
 * 같은 절차(얻기 → 속도 → 켜기 → 끄기 예약)를 클럭마다 되풀이하지 않으려고
 * 뽑아낸 헬퍼다. 반환 규약이 포인터 하나라, 네 단계 중 어디서 실패하든
 * ERR_PTR 로 감싸 한 갈래로 올린다.
 *
 * 단계별로:
 *   1) devm_clk_get() — 실패 시 그 오류 포인터를 그대로 돌려준다. 여기서는
 *      아직 아무것도 켜지 않았으므로 되감을 것이 없다.
 *   2) rate 가 0 이 아니면 clk_set_rate(). aux 클럭에만 해당한다(24MHz).
 *   3) clk_prepare_enable() 로 실제로 켠다.
 *   4) devm_add_action_or_reset() 으로 keembay_pcie_disable_clock 을 예약한다.
 *      _or_reset 판이라 **등록 자체가 실패하면 그 자리에서 콜백을 한 번 불러 준다** —
 *      즉 클럭이 켜진 채 새는 일이 없다.
 *
 * [상류 코드 관찰] 2단계와 3단계가 실패하면 앞 단계를 되감지 않고 곧장
 * ERR_PTR 을 돌려준다. 2단계 실패는 아직 켜기 전이라 무해하지만,
 * devm_clk_get 이 잡은 참조는 devm 이 프로브 실패 때 정리하므로 결과적으로
 * 누수는 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 컨텍스트. clk_prepare_enable() 이
 * 잠들 수 있다.
 *
 * 에러 경로: 위 네 단계 모두 ERR_PTR 한 갈래로 모인다. 호출자
 * keembay_pcie_probe_clocks() 가 dev_err_probe() 로 로그를 남기며,
 * -EPROBE_DEFER 면 조용히 재시도로 이어진다.
 *
 * 호출 체인:
 *   keembay_pcie_probe_clocks() → [이 함수]
 *     → devm_clk_get() → clk_set_rate() → clk_prepare_enable()
 *     → devm_add_action_or_reset(keembay_pcie_disable_clock)
 */
static inline struct clk *keembay_pcie_probe_clock(struct device *dev,
						   const char *id, u64 rate)
{
	/* [한국어] 얻어 낸 클럭 핸들. 성공 시 그대로, 실패 시 ERR_PTR 로 감싸 반환한다. */
	struct clk *clk;
	/* [한국어] 하위 호출들의 반환값을 담을 임시 변수. */
	int ret;

	/* [한국어] 디바이스 트리의 clock-names 에서 id 에 해당하는 클럭을 찾는다.
	 * devm 판이라 프로브 실패나 장치 해제 시 참조가 자동으로 반납된다. */
	clk = devm_clk_get(dev, id);
	/* [한국어] 조회 실패 — 클럭 공급자가 아직 없으면 -EPROBE_DEFER 가 담겨 온다. */
	if (IS_ERR(clk))
		/* [한국어] 오류 포인터를 그대로 올린다. 아직 아무것도 켜지 않아 되감을 것이 없다. */
		return clk;

	/* [한국어] rate 가 0 이 아니면(aux 클럭) 주파수를 명시적으로 맞춘다. master 는 0 을 받아 건너뛴다. */
	if (rate) {
		/* [한국어] 클럭 공급자에게 이 주파수를 요구한다. 정확히 그 값이 나오지 않을 수도 있으나
		 * 이 드라이버는 반환값의 성공 여부만 본다. */
		ret = clk_set_rate(clk, rate);
		/* [한국어] 속도 설정 실패. */
		if (ret)
			/* [한국어] errno 를 포인터에 실어 올린다. 아직 켜기 전이라 역시 되감을 것이 없다. */
			return ERR_PTR(ret);
	}

	/* [한국어] 클럭을 실제로 켠다. prepare(느린 준비 단계)와 enable(빠른 게이트 열기)을 한 번에 한다.
	 * prepare 가 잠들 수 있어 이 함수 전체가 프로세스 컨텍스트 전용이 된다. */
	ret = clk_prepare_enable(clk);
	/* [한국어] 켜기 실패. */
	if (ret)
		/* [한국어] errno 를 포인터에 실어 올린다. */
		return ERR_PTR(ret);

	/* [한국어] 켜기의 짝을 devres 에 등록해 둔다. **_or_reset 판이라 등록 자체가 실패하면
	 * 그 자리에서 콜백을 한 번 불러 준다** — 즉 클럭이 켜진 채 새는 일이 없다.
	 * .remove 가 없는 이 드라이버에서 프로브 실패 되감기를 맡는 장치다. */
	ret = devm_add_action_or_reset(dev, keembay_pcie_disable_clock, clk);
	/* [한국어] 등록 실패(이 시점에 클럭은 위 규칙대로 이미 꺼져 있다). */
	if (ret)
		/* [한국어] errno 를 포인터에 실어 올린다. */
		return ERR_PTR(ret);

	/* [한국어] 네 단계 모두 통과. 켜진 클럭 핸들을 돌려준다. */
	return clk;
}

/* [한국어]
 * keembay_pcie_probe_clocks - master 와 aux 두 클럭을 순서대로 켠다
 *
 * @pcie: 이 컨트롤러의 상태. 얻은 클럭을 pcie->clk_master / pcie->clk_aux 에 담는다.
 * @return: 0 = 둘 다 성공. 음수 errno = 둘 중 하나가 실패(-EPROBE_DEFER 포함).
 *
 * RC 모드 초기화에서 하드웨어를 실제로 깨우는 첫 단계다. 클럭이 없으면
 * 이후의 PHY·PLL·레지스터 접근이 모두 무의미하므로 가장 먼저 한다.
 *
 * 두 클럭의 처리가 다르다.
 *   "master" : rate 0 — 속도를 건드리지 않고 켜기만 한다. 컨트롤러 본체용이다.
 *   "aux"    : rate AUX_CLK_RATE_HZ(24MHz) — 속도를 명시적으로 맞춘다.
 *
 * dev_err_probe() 를 쓰는 것이 요점이다. 이 헬퍼는 -EPROBE_DEFER 일 때는
 * 오류 로그를 남기지 않고 지연 사유만 기록한다. 클럭 공급자가 아직
 * 프로브되지 않은 흔한 상황에서 dmesg 가 오류로 더럽혀지지 않게 해 준다.
 *
 * 실행 컨텍스트: RC 프로브 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 클럭이 실패하면 두 번째는 시도조차 하지 않고 반환한다.
 * 첫 클럭이 성공한 뒤 두 번째가 실패하면, 첫 클럭은 이미 devm 액션으로
 * 끄기가 예약되어 있으므로 프로브 실패 시 자동으로 꺼진다 — 여기서 손으로
 * 되감을 것이 없다.
 *
 * 호출 체인:
 *   keembay_pcie_add_pcie_port() → [이 함수]
 *     → keembay_pcie_probe_clock() x2 → dev_err_probe()
 */
static int keembay_pcie_probe_clocks(struct keembay_pcie *pcie)
{
	/* [한국어] 이 파일의 상태에서 DWC 서술자를 꺼낸다. dev 를 얻기 위한 한 단계다. */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 클럭을 조회할 기준 장치. probe 가 pci->dev 에 심어 두었다. */
	struct device *dev = pci->dev;

	/* [한국어] 주 클럭. rate 0 이라 속도는 건드리지 않고 켜기만 한다. */
	pcie->clk_master = keembay_pcie_probe_clock(dev, "master", 0);
	/* [한국어] 오류 포인터인지 검사한다 — 이 헬퍼는 실패를 ERR_PTR 로 실어 온다. */
	if (IS_ERR(pcie->clk_master))
		/* [한국어] dev_err_probe 는 -EPROBE_DEFER 일 때 오류 로그를 남기지 않고 지연 사유만
		 * 기록한다. 클럭 공급자가 아직 프로브되지 않은 흔한 상황에서 dmesg 를
		 * 더럽히지 않기 위해서다. 인자로 받은 errno 를 그대로 반환값으로 돌려준다. */
		return dev_err_probe(dev, PTR_ERR(pcie->clk_master),
				     "Failed to enable master clock");

	/* [한국어] 보조 클럭. 이쪽은 24MHz 를 명시해 clk_set_rate() 가 실제로 불린다. */
	pcie->clk_aux = keembay_pcie_probe_clock(dev, "aux", AUX_CLK_RATE_HZ);
	/* [한국어] 역시 오류 포인터 검사. */
	if (IS_ERR(pcie->clk_aux))
		/* [한국어] 같은 이유로 dev_err_probe 를 쓴다. 앞서 켠 master 클럭은 devm 액션이
		 * 프로브 실패 때 자동으로 꺼 주므로 여기서 손으로 되감지 않는다. */
		return dev_err_probe(dev, PTR_ERR(pcie->clk_aux),
				     "Failed to enable auxiliary clock");

	/* [한국어] 둘 다 켜졌다. */
	return 0;
}

/*
 * Initialize the internal PCIe PLL in Host mode.
 * See the following sections in Keem Bay data book,
 * (1) 6.4.6.1 PCIe Subsystem Example Initialization,
 * (2) 6.8 PCIe Low Jitter PLL for Ref Clk Generation.
 */
/* [한국어]
 * keembay_pcie_pll_init - 내장 저지터 PLL(LJPLL)을 프로그래밍하고 잠금을 기다린다
 *
 * @pcie: 이 컨트롤러의 상태. apb_base 로 LJPLL 레지스터에 접근한다.
 * @return: 0 = 잠금 성공. 음수 = 제한 시간(500ms) 안에 잠기지 않음.
 *
 * LJPLL(Low Jitter PLL)은 SoC 안에서 PCIe 레퍼런스 클럭을 만드는 PLL 이다.
 * 보드에 외부 레퍼런스 클럭 발진기를 두지 않는 구성이므로, 링크를 켜기
 * 전에 이 PLL 이 반드시 잠겨 있어야 한다. 그래서 이 함수는 RC 초기화에서
 * PCIE_RSTN 을 풀기 직전에 불린다.
 *
 * 쓰기 **순서가 중요하다.** 분주비를 먼저 다 써 넣고, 켜기 비트를 맨
 * 마지막에 세운다 — LJPLL_EN 을 세우는 순간 PLL 이 잠금 절차에 들어가기
 * 때문이다.
 *   1) CNTRL_2 : 입력 분주비 0, 피드백 분주비 0x32(=50).
 *   2) CNTRL_3 : 후단 분주기 3A 와 2A 를 각각 0x2 로.
 *   3) CNTRL_0 : LJPLL_EN 을 1 로, 네 출력 중 위쪽 둘(0xc = 0b1100)을 허가.
 *   4) STA 레지스터의 LJPLL_LOCK 을 20us 간격으로 최대 500ms 폴링.
 *
 * 세 번의 writel 이 모두 읽기-수정-쓰기가 아니라 **통째로 쓰기** 다.
 * 각 제어 레지스터의 나머지 비트를 0 으로 밀어 버리는 셈인데, 이 함수가
 * PLL 설정의 유일한 주인이라 문제가 되지 않는다.
 *
 * FIELD_PREP(LJPLL_EN, 0x1) 은 단일 비트 마스크에 FIELD_PREP 을 쓴 것으로,
 * 결과는 BIT(29) 와 같다. 옆의 필드들과 표기를 맞추려는 선택이다.
 *
 * 주석에 적힌 근거 문서(Keem Bay 데이터북 6.4.6.1, 6.8)는 이 트리에
 * 없으므로 각 상수의 물리적 의미까지는 확인 못 함.
 *
 * 실행 컨텍스트: RC 프로브 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 폴링이 실패하면 dev_err 로 "Low jitter PLL is not locked" 를
 * 남기고 ret 를 그대로 돌려준다. 호출자 keembay_pcie_add_pcie_port() 가
 * 곧바로 프로브를 접는다. **이때 PLL 을 다시 끄지는 않는다** —
 * LJPLL_EN 이 선 채로 남지만, 프로브가 실패하면 장치가 쓰이지 않으므로
 * 실질적 영향은 없다.
 *
 * 호출 체인:
 *   keembay_pcie_add_pcie_port() → [이 함수]
 *     → writel() x3 → readl_poll_timeout() → (실패 시) dev_err()
 */
static int keembay_pcie_pll_init(struct keembay_pcie *pcie)
{
	/* [한국어] dev_err 에 넘길 장치를 얻기 위한 한 단계. */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 레지스터에 쓸 값을 조립하는 임시 변수이자, 아래 폴링 매크로가 읽은 값을 담는 변수. */
	u32 val;
	/* [한국어] 폴링 결과(0 또는 -ETIMEDOUT). */
	int ret;

	/* [한국어] 입력 분주비 0(분주 없음)과 피드백 분주비 0x32(=50)를 한 워드로 조립한다.
	 * FIELD_PREP 이 각 값을 해당 마스크 자리로 밀어 넣는다. */
	val = FIELD_PREP(LJPLL_REF_DIV, 0) | FIELD_PREP(LJPLL_FB_DIV, 0x32);
	/* [한국어] CNTRL_2 에 통째로 쓴다. 이 함수가 PLL 설정의 유일한 주인이라 나머지 비트를
	 * 0 으로 미는 것이 문제되지 않는다. */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_2);

	/* [한국어] 후단 분주기 3A 와 2A 를 각각 0x2 로. 두 분주를 거쳐 VCO 출력이 최종
	 * 레퍼런스 클럭 주파수로 내려간다. 구체적 주파수 계산 근거는 이 트리에서 확인 못 함. */
	val = FIELD_PREP(LJPLL_POST_DIV3A, 0x2) |
		FIELD_PREP(LJPLL_POST_DIV2A, 0x2);
	/* [한국어] CNTRL_3 에 통째로 쓴다. */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_3);

	/* [한국어] **켜기 비트를 맨 마지막에 조립한다.** LJPLL_EN 을 세우는 순간 PLL 이 잠금
	 * 절차에 들어가므로, 분주비를 모두 써 넣은 뒤여야 한다. 0xc(=0b1100)는 네 출력 중
	 * 위쪽 둘만 허가한다는 뜻이다. FIELD_PREP(LJPLL_EN, 0x1) 은 단일 비트 마스크에
	 * FIELD_PREP 을 쓴 것으로 결과는 BIT(29) 와 같다 — 옆 필드들과 표기를 맞춘 선택이다. */
	val = FIELD_PREP(LJPLL_EN, 0x1) | FIELD_PREP(LJPLL_FOUT_EN, 0xc);
	/* [한국어] CNTRL_0 에 쓰는 순간 PLL 이 잠금 절차를 시작한다. */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_0);

	/* [한국어] 상태 레지스터를 20us 간격으로 최대 500ms 폴링해 LJPLL_LOCK(비트 0)이 설 때까지
	 * 기다린다. 잠기지 않은 클럭으로 링크를 켜면 훈련이 실패한다. */
	ret = readl_poll_timeout(pcie->apb_base + PCIE_REGS_LJPLL_STA,
				 val, val & LJPLL_LOCK, 20,
				 500 * USEC_PER_MSEC);
	/* [한국어] 제한 시간 안에 잠기지 않았다. */
	if (ret)
		/* [한국어] 무엇이 실패했는지 남긴다. **여기서 PLL 을 다시 끄지는 않는다** — 프로브가
		 * 실패하면 장치가 쓰이지 않으므로 실질적 영향이 없다는 판단으로 보인다. */
		dev_err(pci->dev, "Low jitter PLL is not locked\n");

	/* [한국어] 성공이면 0, 실패면 -ETIMEDOUT. 위 if 가 로그만 남기고 값은 그대로 흘려보낸다. */
	return ret;
}

/* [한국어]
 * keembay_pcie_msi_irq_handler - SoC 고유 W1C 상태 레지스터를 다루는 MSI 체인 핸들러
 *
 * @desc: 이 인터럽트의 irq_desc. handler_data 로 심어 둔 struct keembay_pcie 와
 *   부모 irq_chip 을 여기서 꺼낸다.
 * @return: 없음(체인 핸들러 규약).
 *
 * **이 파일이 존재하는 가장 큰 이유 중 하나다.** DWC 코어에는 이미
 * dw_chained_msi_isr 이라는 기본 MSI 체인 핸들러가 있지만, Keem Bay 는 표준
 * DWC IP 위에 "상태 레지스터의 해당 비트에 1 을 써야 인터럽트가 지워지는"
 * 추가 로직을 얹었다. 그 지우기를 코어는 모르므로, 이 파일이 자기 체인
 * 핸들러를 직접 달아 지우기까지 책임진다. 그래서 keembay_pcie_add_pcie_port()
 * 가 pp->msi_irq[0] 에 -ENODEV 를 미리 넣어, 코어가 자기 핸들러를 거는
 * 경로(pcie-designware-host.c:912~941, 955)를 통째로 건너뛰게 만든다.
 *
 * 동작 순서:
 *   1) chained_irq_enter() — 부모 인터럽트 컨트롤러 쪽에서 이 선을 마스크하고
 *      필요하면 ack 한다. 이걸 빼면 중첩 인터럽트로 폭주할 수 있다.
 *   2) 상태 레지스터와 허가 레지스터를 각각 읽어 AND 한다. 허가되지 않은
 *      비트가 서 있어도 무시하기 위해서다.
 *   3) MSI_CTRL_INT 가 서 있으면 dw_handle_msi_irq() 로 DWC 의 MSI 디코딩에
 *      넘긴다 — 거기서 어떤 벡터가 왔는지 풀어 해당 리눅스 IRQ 를 재분배한다.
 *   4) 상태 레지스터에 다시 써서 W1C 로 지운다.
 *   5) chained_irq_exit() — 부모 쪽 마스크를 푼다.
 *
 * [상류 코드 관찰] 4단계가 MSI_CTRL_INT 만이 아니라 status 전체를 되쓴다.
 * status 는 "계류 중이면서 허가된" 모든 비트이므로, MSI 를 처리하는 김에
 * 같은 순간 서 있던 eDMA 비트(EDMA_INT_EN, 비트 7..0)까지 함께 지운다.
 * RC 모드에서는 keembay_pcie_add_pcie_port() 가 MSI 비트만 추가로 켜므로
 * eDMA 허가가 꺼져 있는 한 실제로 문제가 되지 않지만, 두 허가가 함께 켜진
 * 구성에서는 eDMA 인터럽트를 삼킬 수 있다. 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] 이 핸들러는 keembay_pcie_setup_msi_irq() 가
 * irq_set_chained_handler_and_data() 로 걸어 두기만 하고, 어디서도 떼지
 * 않는다. 프로브가 그 뒤 단계에서 실패하면 devm 이 struct keembay_pcie 를
 * 해제하는데도 핸들러 데이터는 그 주소를 그대로 가리킨다. 실제로 .remove 가
 * 없고 suppress_bind_attrs 로 언바인드도 막혀 있어 정상 동작 중에는 드러나지
 * 않지만, 프로브 실패 경로에서는 남는다. 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트의 체인 핸들러.** 잠들 수 없고, 아래로
 * 부르는 dw_handle_msi_irq() 도 같은 문맥에서 돈다.
 *
 * 에러 경로: 없다. status 에 아무 비트도 없으면(스퓨리어스) 아무것도 하지
 * 않고 조용히 나간다.
 *
 * 호출 체인:
 *   하위 장치의 MSI 쓰기 → SoC 인터럽트 로직 → 부모 irq_chip
 *     → [이 함수] → chained_irq_enter() → dw_handle_msi_irq()
 *     → writel(W1C) → chained_irq_exit()
 */
static void keembay_pcie_msi_irq_handler(struct irq_desc *desc)
{
	/* [한국어] keembay_pcie_setup_msi_irq() 가 irq_set_chained_handler_and_data() 로 심어 둔
	 * 드라이버 상태를 되찾는다. 체인 핸들러가 자기 문맥을 얻는 표준 방법이다. */
	struct keembay_pcie *pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 이 인터럽트 선을 관리하는 부모 irq_chip. 아래 chained_irq_enter/exit 가 이것으로
	 * 마스크·ack 를 조작한다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 각각 상태 레지스터 원본, 허가 마스크, 둘을 AND 한 결과. */
	u32 val, mask, status;
	/* [한국어] dw_handle_msi_irq() 에 넘길 DWC 루트 포트 문맥. */
	struct dw_pcie_rp *pp;

	/*
	 * Keem Bay PCIe Controller provides an additional IP logic on top of
	 * standard DWC IP to clear MSI IRQ by writing '1' to the respective
	 * bit of the status register.
	 *
	 * So, a chained irq handler is defined to handle this additional
	 * IP logic.
	 */

	/* [한국어] 부모 인터럽트 컨트롤러 쪽에서 이 선을 마스크하고 필요하면 ack 한다.
	 * 체인 핸들러의 필수 관용구로, 빠뜨리면 처리 중 같은 선이 다시 들어와 폭주한다. */
	chained_irq_enter(chip, desc);

	/* [한국어] dw_pcie 안의 루트 포트 문맥 주소를 잡는다. pci 가 값으로 박혀 있어 단순한
	 * 주소 계산이며 역참조가 아니다. */
	pp = &pcie->pci.pp;
	/* [한국어] 인터럽트 상태 레지스터(0x002c)를 읽는다 — 어떤 원인이 계류 중인지. */
	val = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_STATUS);
	/* [한국어] 인터럽트 허가 레지스터(0x0028)를 읽는다 — 그중 무엇을 받기로 했는지. */
	mask = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);

	/* [한국어] 둘을 AND 해 '계류 중이면서 허가된' 비트만 남긴다. 허가하지 않은 원인이
	 * 상태에 서 있어도 무시하기 위해서다. */
	status = val & mask;

	/* [한국어] MSI 컨트롤러 인터럽트(비트 8)가 서 있으면 — */
	if (status & MSI_CTRL_INT) {
		/* [한국어] DWC 코어에 넘겨 어떤 MSI 벡터가 왔는지 풀게 한다. 거기서 해당 리눅스 IRQ 로
		 * 재분배되어 결국 장치 드라이버의 핸들러가 돈다. 인터럽트 컨텍스트에서 돈다. */
		dw_handle_msi_irq(pp);
		/* [한국어] **W1C 지우기.** 이 상태 레지스터는 1 을 써야 지워지므로, 방금 읽은 status 를
		 * 그대로 되쓴다. Keem Bay 가 표준 DWC IP 위에 얹은 이 동작 때문에 이 파일이
		 * 체인 핸들러를 직접 두는 것이다. status 전체를 쓰므로 같은 순간 서 있던
		 * eDMA 비트까지 함께 지워진다(위 함수 주석의 관찰 참고). */
		writel(status, pcie->apb_base + PCIE_REGS_INTERRUPT_STATUS);
	}

	/* [한국어] 부모 쪽 마스크를 풀어 다음 인터럽트를 받을 수 있게 한다. enter 와 반드시 짝을 이룬다. */
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * keembay_pcie_setup_msi_irq - "pcie" 인터럽트에 위 체인 핸들러를 건다
 *
 * @pcie: 이 컨트롤러의 상태. 핸들러 데이터로 이 포인터를 심는다.
 * @return: 0 = 성공. 음수 = platform_get_irq_byname() 실패(-EPROBE_DEFER 포함).
 *
 * 디바이스 트리의 interrupt-names 에서 "pcie" 라는 이름의 인터럽트를 찾아,
 * 거기에 keembay_pcie_msi_irq_handler 를 체인 핸들러로 건다. 핸들러 데이터로
 * struct keembay_pcie 를 심어 두므로, 인터럽트 컨텍스트에서
 * irq_desc_get_handler_data() 로 되찾을 수 있다.
 *
 * irq_set_chained_handler_and_data() 는 반환값이 없어 실패를 알리지 않는다.
 * 그래서 이 함수가 검사할 수 있는 실패는 IRQ 번호 조회 하나뿐이다.
 *
 * 실행 컨텍스트: RC 프로브 경로, 프로세스 컨텍스트. 이 함수가 부르는
 * platform_get_irq_byname() 은 IRQ 도메인 매핑을 만들 수 있어 잠들 수 있다.
 *
 * 에러 경로: IRQ 를 못 찾으면 그 음수를 그대로 올린다. 호출자
 * keembay_pcie_add_pcie_port() 가 곧바로 반환하므로 이 시점에는 아직 클럭도
 * GPIO 도 잡지 않은 상태다 — 되감을 것이 없다.
 *
 * 호출 체인:
 *   keembay_pcie_add_pcie_port() → [이 함수]
 *     → platform_get_irq_byname() → irq_set_chained_handler_and_data()
 */
static int keembay_pcie_setup_msi_irq(struct keembay_pcie *pcie)
{
	/* [한국어] 이 파일의 상태에서 DWC 서술자를 꺼낸다 — dev 를 얻기 위한 한 단계. */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] IRQ 를 조회할 기준 장치. */
	struct device *dev = pci->dev;
	/* [한국어] platform_get_irq_byname() 이 struct platform_device 를 요구하므로 되돌린다.
	 * probe 가 pci->dev 에 심은 것이 원래 &pdev->dev 였기에 안전한 변환이다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 조회한 리눅스 IRQ 번호. */
	int irq;

	/* [한국어] 디바이스 트리의 interrupt-names 에서 "pcie" 를 찾는다. _optional 판이 아니라
	 * 못 찾으면 로그가 남는다 — 이 드라이버에는 이 인터럽트가 필수이기 때문이다. */
	irq = platform_get_irq_byname(pdev, "pcie");
	/* [한국어] 음수는 실패(공급자가 아직이면 -EPROBE_DEFER). */
	if (irq < 0)
		/* [한국어] 그대로 올린다. 아직 아무 자원도 잡지 않아 되감을 것이 없다. */
		return irq;

	/* [한국어] 체인 핸들러를 건다. 세 번째 인자 pcie 가 핸들러 데이터로 심겨,
	 * 인터럽트 컨텍스트에서 irq_desc_get_handler_data() 로 되찾힌다.
	 * **반환값이 없어 실패를 알 수 없다.** 또 이 등록을 떼는 코드가 이 파일에 없다. */
	irq_set_chained_handler_and_data(irq, keembay_pcie_msi_irq_handler,
					 pcie);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * keembay_pcie_ep_init - EP 모드에서 eDMA 인터럽트만 허가한다
 *
 * @ep: DWC 의 엔드포인트 문맥. to_dw_pcie_from_ep() 로 컨트롤러 서술자를,
 *   다시 dev_get_drvdata() 로 이 드라이버 상태를 되찾는다.
 * @return: 없음.
 *
 * dw_pcie_ep_ops.init 콜백이며, EP 모드 초기화에서 이 파일이 하는 SoC 고유
 * 작업의 **전부** 다. 하는 일은 인터럽트 허가 레지스터에 EDMA_INT_EN
 * (비트 7..0, eDMA 채널 여덟 개)만 써 넣는 것 하나다.
 *
 * 읽기-수정-쓰기가 아니라 통째로 쓴다는 점이 의도적이다. MSI_CTRL_INT_EN
 * (비트 8)이 함께 지워지는데, EP 모드에서는 MSI 를 받는 쪽이 아니라 보내는
 * 쪽이므로 그 인터럽트가 필요 없다.
 *
 * EP 모드에서 이 파일이 클럭·PLL·PERST#·LTSSM 을 전혀 다루지 않는 것과
 * 짝을 이룬다 — 레퍼런스 클럭과 리셋을 상대편 호스트가 주기 때문이다.
 * 그 eDMA 인터럽트를 실제로 받아 처리하는 드라이버는 이 트리(희소 체크아웃)에
 * 없어 확인 못 함.
 *
 * 실행 컨텍스트: EP 프로브 경로에서 DWC 코어가 부른다. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 라 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   keembay_pcie_probe() → dw_pcie_ep_init() → dw_pcie_ep_ops.init == [이 함수]
 *     → writel()
 */
static void keembay_pcie_ep_init(struct dw_pcie_ep *ep)
{
	/* [한국어] EP 문맥에서 컨트롤러 서술자로 올라간다. dw_pcie 안에 ep 가 값으로 박혀 있어
	 * container_of 로 되돌리는 헬퍼다. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 거기서 다시 이 드라이버 상태를 되찾는다. apb_base 가 필요하기 때문이다. */
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);

	/* [한국어] eDMA 채널 여덟 개(비트 7..0)의 인터럽트만 허가한다. **읽기-수정-쓰기가 아니라
	 * 통째로 쓰기** 라 MSI_CTRL_INT_EN(비트 8)이 함께 지워지는데, EP 는 MSI 를 받는
	 * 쪽이 아니라 보내는 쪽이라 의도된 결과다. */
	writel(EDMA_INT_EN, pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);
}

/* [한국어]
 * keembay_pcie_ep_raise_irq - EP 가 호스트로 인터럽트를 올린다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @func_no: 다중 물리 함수 중 몇 번인지. DWC 코어가 해당 함수의 config 공간을
 *   찾는 데 쓴다.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 중 하나.
 * @interrupt_num: MSI 는 1 기준 벡터 번호, MSI-X 는 1 기준 테이블 인덱스.
 * @return: 0 = 성공. -EINVAL = 지원하지 않는 종류. 그 밖의 음수는 DWC 코어의 실패.
 *
 * dw_pcie_ep_ops.raise_irq 콜백이다. EPF(엔드포인트 기능) 드라이버가
 * pci_epc_raise_irq() 로 인터럽트를 요청하면 결국 여기로 내려온다.
 *
 * 세 갈래 중 두 갈래는 DWC 코어 함수로 그대로 넘긴다. **INTx 만 이 하드웨어가
 * 지원하지 않아** -EINVAL 로 막는다 — Keem Bay 의 EP 는 레거시 인터럽트 선을
 * 내지 않는다. 알 수 없는 종류도 같은 -EINVAL 로 떨어뜨리되, 로그 문구를
 * 달리해 어느 쪽인지 구별할 수 있게 했다.
 *
 * 실행 컨텍스트: EPF 드라이버의 호출 문맥을 그대로 물려받는다. 인터럽트를
 * 올리는 경로라 잠들지 않는 것이 보통이다.
 *
 * 에러 경로: 두 갈래 모두 dev_err 로 남기고 -EINVAL. 호출자 pci_epc_raise_irq()
 * 가 그대로 EPF 드라이버에 올린다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq() → dw_pcie_ep_ops.raise_irq == [이 함수]
 *     → dw_pcie_ep_raise_msi_irq() 또는 dw_pcie_ep_raise_msix_irq()
 */
static int keembay_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     unsigned int type, u16 interrupt_num)
{
	/* [한국어] dev_err 에 넘길 장치를 얻기 위한 한 단계. 인터럽트를 실제로 올리는 일은
	 * 아래 DWC 코어 함수들이 한다. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* [한국어] EPF 드라이버가 요청한 인터럽트 종류로 갈린다. */
	switch (type) {
	/* [한국어] 레거시 INTx 요청 — */
	case PCI_IRQ_INTX:
		/* INTx interrupts are not supported in Keem Bay */
		/* [한국어] Keem Bay 의 EP 는 레거시 인터럽트 선을 내지 않는다. 왜 지원하지 않는지에 대한
		 * 하드웨어적 근거는 이 트리에서 확인 못 함. */
		dev_err(pci->dev, "INTx IRQ is not supported\n");
		/* [한국어] 지원하지 않음을 -EINVAL 로 알린다. 호출자 pci_epc_raise_irq() 가 EPF 드라이버에 올린다. */
		return -EINVAL;
	/* [한국어] MSI 요청 — */
	case PCI_IRQ_MSI:
		/* [한국어] DWC 코어가 MSI capability 를 읽어 주소/데이터를 만들고 메모리 쓰기를 낸다.
		 * interrupt_num 은 1 기준 벡터 번호다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	/* [한국어] MSI-X 요청 — */
	case PCI_IRQ_MSIX:
		/* [한국어] DWC 코어가 MSI-X 테이블에서 해당 항목을 읽어 메모리 쓰기를 낸다.
		 * interrupt_num 은 1 기준 테이블 인덱스다. */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	/* [한국어] 위 셋 중 어느 것도 아닌 값 — */
	default:
		/* [한국어] 어떤 값이 들어왔는지 남긴다. INTx 와 문구를 달리해 둘을 구별할 수 있게 했다. */
		dev_err(pci->dev, "Unknown IRQ type %d\n", type);
		/* [한국어] 역시 -EINVAL. */
		return -EINVAL;
	}
}

/* [한국어] 이 EP 의 능력표. EPC 코어와 EPF 드라이버가 BAR 배치와 인터럽트 종류를 정할 때 본다.
 * 설정자: 컴파일 시점 초기화자. static const 라 이후 불변.
 * 읽는 자: keembay_pcie_get_features() 가 주소를 그대로 돌려주고, 그것을
 *   dw_pcie_ep_get_features() 와 EPC 코어가 읽는다.
 * 값 범위: 아래 필드 조합 그대로. 인스턴스가 하나뿐이라 모든 EP 가 같은 표를 본다.
 * 동기화: 읽기 전용 상수라 잠금이 필요 없다. */
static const struct pci_epc_features keembay_pcie_epc_features = {
	/* [한국어] DWC 공통 항목 두 개를 한 번에 채우는 매크로 —
	 * dynamic_inbound_mapping 과 subrange_mapping 을 참으로 만든다
	 * (pcie-designware.h:649). DWC IP 라면 공통으로 갖는 성질이라 매크로로 묶어 두었다. */
	DWC_EPC_COMMON_FEATURES,
	/* [한국어] MSI 를 올릴 수 있다. 위 raise_irq 의 PCI_IRQ_MSI 갈래와 짝이 맞는다. */
	.msi_capable		= true,
	/* [한국어] MSI-X 도 올릴 수 있다. PCI_IRQ_MSIX 갈래와 짝이 맞는다.
	 * INTx 는 여기에 대응하는 항목이 없고 raise_irq 가 -EINVAL 로 막는다. */
	.msix_capable		= true,
	/* [한국어] BAR0 은 64비트 전용 — 32비트 BAR 로는 쓸 수 없다. 64비트 BAR 는 인접한 두
	 * BAR 슬롯을 함께 먹으므로, BAR0 을 잡으면 BAR1 이 사라진다. 그래서 아래도
	 * 0/2/4 만 지정되어 있다. */
	.bar[BAR_0]		= { .only_64bit = true, },
	/* [한국어] BAR2 도 64비트 전용(BAR3 을 함께 먹는다). */
	.bar[BAR_2]		= { .only_64bit = true, },
	/* [한국어] BAR4 도 64비트 전용(BAR5 를 함께 먹는다). */
	.bar[BAR_4]		= { .only_64bit = true, },
	/* [한국어] BAR 크기와 인바운드 주소의 정렬 요구 16KB. 이보다 잘게 잡을 수 없다는 뜻으로,
	 * ATU 인바운드 창의 하드웨어 제약에서 온다. */
	.align			= SZ_16K,
};

/* [한국어]
 * keembay_pcie_get_features - 이 EP 가 무엇을 할 수 있는지 알려 준다
 *
 * @ep: DWC 의 엔드포인트 문맥. **이 함수는 인자를 쓰지 않는다** — 능력표가
 *   인스턴스마다 다르지 않고 컴파일 시점 상수 하나뿐이기 때문이다.
 * @return: 파일 아래에 정의된 static const 능력표의 주소. NULL 을 돌려주지 않는다.
 *
 * dw_pcie_ep_ops.get_features 콜백이다. EPC(엔드포인트 컨트롤러) 코어와
 * EPF 드라이버가 BAR 배치를 정하거나 인터럽트 종류를 고를 때 이 표를 본다.
 *
 * 인자를 무시하는 한 줄짜리 함수지만 콜백 규약상 필요하다 — 코어는 함수
 * 포인터만 알 뿐, 그 뒤에 상수가 있는지 계산이 있는지 모른다.
 *
 * 실행 컨텍스트: EP 초기화·설정 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_ep_get_features) 또는 EPC 코어
 *     → dw_pcie_ep_ops.get_features == [이 함수] → 상수 표 반환
 */
static const struct pci_epc_features *
keembay_pcie_get_features(struct dw_pcie_ep *ep)
{
	/* [한국어] 인스턴스마다 다를 것이 없으므로 상수 표의 주소를 그대로 돌려준다.
	 * ep 인자는 쓰지 않지만 콜백 규약상 받아야 한다. */
	return &keembay_pcie_epc_features;
}

/* [한국어] EP 모드에서 DWC 코어가 쓰는 콜백 표. probe 의 EP 갈래가 pci->ep.ops 에 건다.
 * 설정자: 컴파일 시점 초기화자. static const 라 이후 불변.
 * 읽는 자: pcie-designware-ep.c 의 각 단계.
 * 값 범위: 세 함수 포인터 모두 이 파일 안의 정의를 가리킨다. pre_init 이나
 *   get_dbi_offset 같은 나머지 훅은 NULL 로 남겨 코어의 기본 동작을 쓴다.
 * 동기화: 읽기 전용 상수. */
static const struct dw_pcie_ep_ops keembay_pcie_ep_ops = {
	/* [한국어] EP 초기화 훅 — eDMA 인터럽트만 연다. */
	.init		= keembay_pcie_ep_init,
	/* [한국어] 인터럽트 올리기 훅 — INTx 는 막고 MSI/MSI-X 는 코어에 넘긴다. */
	.raise_irq	= keembay_pcie_ep_raise_irq,
	/* [한국어] 능력표 조회 훅 — 위 상수 표를 돌려준다. */
	.get_features	= keembay_pcie_get_features,
};

/* [한국어] **내용이 하나도 없는 호스트 콜백 표.** 그런데도 필요하다 —
 * pcie-designware-host.c:1511 이 pp->ops 자체의 NULL 검사 없이 pp->ops->init 을
 * 역참조하므로, pp->ops 는 반드시 NULL 이 아니어야 한다. 모든 필드가 NULL 인
 * 표를 하나 두어 그 요구를 만족시키고, 동시에 '이 SoC 는 호스트 단계에서
 * 따로 할 일이 없다' 는 사실을 표현한다.
 * 설정자: 컴파일 시점 초기화자(빈 초기화자라 모든 필드가 NULL).
 * 읽는 자: keembay_pcie_add_pcie_port() 가 pp->ops 에 걸고, DWC 호스트 코어가 읽는다.
 * 값 범위: 모든 콜백이 NULL.
 * 동기화: 읽기 전용 상수. */
static const struct dw_pcie_host_ops keembay_pcie_host_ops = {
};

/* [한국어]
 * keembay_pcie_add_pcie_port - RC 모드 초기화 전부를 순서대로 수행한다
 *
 * @pcie: 이 컨트롤러의 상태. apb_base 는 이미 매핑되어 있어야 한다.
 * @pdev: 플랫폼 장치. IRQ 와 GPIO 를 이 장치에서 찾는다.
 * @return: 0 = 호스트 브리지 등록까지 성공. 음수 errno = 단계 중 하나가 실패.
 *
 * RC 모드의 실질적 진입점이다. 하드웨어를 깨우는 순서가 곧 이 함수의 본문이며,
 * **순서를 바꾸면 동작하지 않는다.**
 *
 * 단계별로:
 *    1) pp->ops 에 빈 호스트 콜백 표를 건다. 내용이 하나도 없지만 NULL 이면
 *       안 된다 — pcie-designware-host.c:1511 이 pp->ops 자체의 NULL 검사 없이
 *       pp->ops->init 을 역참조하기 때문이다.
 *    2) pp->msi_irq[0] 에 -ENODEV 를 넣어, DWC 코어가 디바이스 트리에서 MSI
 *       인터럽트를 찾아 자기 체인 핸들러를 거는 경로를 건너뛰게 만든다.
 *       이 SoC 의 MSI 는 아래 3)에서 이 파일이 직접 맡는다.
 *    3) "pcie" 인터럽트에 이 파일의 체인 핸들러를 건다.
 *    4) PERST# GPIO 를 얻는다. GPIOD_OUT_HIGH 라 **얻는 순간 리셋이 걸린다.**
 *    5) master/aux 클럭을 켠다. 이제부터 레지스터 접근이 의미를 갖는다.
 *    6) PHY0_SRAM_BYPASS 를 세워 PHY 펌웨어 로딩 단계를 건너뛴다.
 *       여기만 읽기-수정-쓰기다 — PHY 제어 레지스터의 다른 비트를 지키기 위해서다.
 *    7) PCIE_DEVICE_TYPE 을 통째로 써서 RC 모드로 못박는다. 같은 레지스터의
 *       PCIE_RSTN 이 0 으로 밀리는데, 이는 의도적이다 — 종류를 정한 뒤에
 *       리셋을 풀어야 하기 때문이다.
 *    8) LJPLL 을 프로그래밍하고 잠금을 기다린다. 레퍼런스 클럭이 여기서 나온다.
 *    9) 이제 PCIE_RSTN 을 세워 코어 리셋을 푼다. 이번에는 읽기-수정-쓰기라
 *       7)에서 세운 PCIE_DEVICE_TYPE 이 살아남는다.
 *   10) PERST# 를 100ms 유지한 뒤 뗀다(PCIe CEM 1.1 Table 2-4).
 *   11) dw_pcie_host_init() — 여기서 DWC 코어가 ATU 를 설정하고,
 *       keembay_pcie_start_link() 를 거쳐 링크를 훈련시키고, 버스를 스캔한다.
 *   12) 마지막으로 MSI_CTRL_INT_EN 을 세워 MSI 인터럽트를 연다.
 *       CONFIG_PCI_MSI 가 꺼져 있으면 읽은 값을 그대로 되쓰는 셈이 된다.
 *
 * [상류 코드 관찰] 되감기가 부분적이다. 11) 이 실패하면 PERST# 만 다시 누를 뿐
 * (keembay_ep_reset_assert), LTSSM·PCIE_RSTN·LJPLL·PHY 우회 비트는 켠 채로 둔다.
 * 클럭만은 devm 액션 덕에 자동으로 꺼진다. 그리고 3)~10) 어디서 실패하든
 * 3)에서 건 체인 핸들러는 떼지 않는다. 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep/usleep_range/clk_prepare_enable 이
 * 모두 잠들 수 있다.
 *
 * 에러 경로: 각 단계가 음수를 그대로 위로 올린다. 11) 만 dev_err 로 이유를
 * 남긴다 — 나머지는 하위 함수가 이미 로그를 남기거나(probe_clocks, pll_init),
 * 로그 없이 -EPROBE_DEFER 로 재시도될 수 있어서다.
 *
 * 호출 체인:
 *   keembay_pcie_probe() → [이 함수]
 *     → keembay_pcie_setup_msi_irq() → devm_gpiod_get() → keembay_pcie_probe_clocks()
 *     → keembay_pcie_pll_init() → keembay_ep_reset_deassert() → dw_pcie_host_init()
 */
static int keembay_pcie_add_pcie_port(struct keembay_pcie *pcie,
				      struct platform_device *pdev)
{
	/* [한국어] DWC 서술자. keembay_pcie 안에 값으로 박혀 있어 주소만 잡으면 된다. */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 그 안의 루트 포트 문맥. 아래에서 ops 와 msi_irq 를 채우고, 마지막에
	 * dw_pcie_host_init() 에 넘긴다. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 로그와 자원 조회의 기준 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 레지스터 읽기-수정-쓰기에 쓸 임시 값. */
	u32 val;
	/* [한국어] 하위 호출들의 반환값. */
	int ret;

	/* [한국어] 빈 호스트 콜백 표를 건다. 내용이 없어도 NULL 이면 안 된다 —
	 * pcie-designware-host.c:1511 이 pp->ops 의 NULL 검사 없이 역참조한다. */
	pp->ops = &keembay_pcie_host_ops;
	/* [한국어] **-ENODEV 를 미리 넣어 DWC 코어의 MSI IRQ 설정을 통째로 건너뛴다.**
	 * 코어는 msi_irq[0] 이 0 일 때만 디바이스 트리를 뒤지고(:912, :929),
	 * 또 값이 0 보다 클 때만 자기 체인 핸들러를 건다(:955). -ENODEV 는 둘 다
	 * 피해 가므로, 이 파일이 아래에서 자기 핸들러를 다는 길이 열린다. */
	pp->msi_irq[0] = -ENODEV;

	/* [한국어] "pcie" 인터럽트에 이 파일의 체인 핸들러를 건다. 이 SoC 의 W1C 지우기 때문에
	 * 코어의 기본 핸들러를 쓸 수 없다. */
	ret = keembay_pcie_setup_msi_irq(pcie);
	/* [한국어] IRQ 조회 실패(-EPROBE_DEFER 포함). */
	if (ret)
		/* [한국어] 그대로 올린다. 아직 클럭도 GPIO 도 잡지 않았다. */
		return ret;

	/* [한국어] PERST# GPIO 를 얻는다. **GPIOD_OUT_HIGH 라 얻는 순간 리셋이 걸린다** —
	 * 이후 초기화가 끝날 때까지 하위 장치를 잡아 두는 셈이다.
	 * optional 판이 아니라 디바이스 트리에 reset-gpios 가 없으면 여기서 실패한다. */
	pcie->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	/* [한국어] 오류 포인터 검사. */
	if (IS_ERR(pcie->reset))
		/* [한국어] errno 를 꺼내 올린다. [상류 코드 관찰] 이 시점에 이미 걸어 둔 체인 핸들러를
		 * 떼지 않는다 — devm 이 pcie 를 해제해도 핸들러 데이터는 그 주소를 가리킨 채
		 * 남는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		return PTR_ERR(pcie->reset);

	/* [한국어] master/aux 클럭을 켠다. 이제부터 레지스터 접근이 의미를 갖는다. */
	ret = keembay_pcie_probe_clocks(pcie);
	/* [한국어] 클럭 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. 켜진 클럭이 있었다면 devm 액션이 알아서 끈다. */
		return ret;

	/* [한국어] PHY 제어 레지스터(0x0164)를 읽는다 — 다른 비트를 지키려면 읽기-수정-쓰기여야 한다. */
	val = readl(pcie->apb_base + PCIE_REGS_PCIE_PHY_CNTL);
	/* [한국어] PHY0_SRAM_BYPASS(비트 8)를 세운다. PHY 펌웨어를 SRAM 에 올리는 단계를 건너뛰고
	 * 내장 ROM 설정으로 곧장 동작하게 한다. */
	val |= PHY0_SRAM_BYPASS;
	/* [한국어] 수정한 값을 되쓴다. */
	writel(val, pcie->apb_base + PCIE_REGS_PCIE_PHY_CNTL);

	/* [한국어] **통째로 쓰기다.** PCIE_DEVICE_TYPE(비트 8)만 담아 쓰므로 같은 레지스터의
	 * PCIE_RSTN(비트 0)이 0 으로 밀린다. 이는 의도적이다 — 장치 종류를 먼저 정하고
	 * 그다음에 리셋을 풀어야 하기 때문이다. 아래 1325~1326 이 그 순서를 완성한다. */
	writel(PCIE_DEVICE_TYPE, pcie->apb_base + PCIE_REGS_PCIE_CFG);

	/* [한국어] LJPLL 을 프로그래밍하고 잠금을 기다린다. PCIe 레퍼런스 클럭이 여기서 나오므로
	 * 코어 리셋을 풀기 전에 반드시 잠겨 있어야 한다. */
	ret = keembay_pcie_pll_init(pcie);
	/* [한국어] PLL 잠금 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. pll_init 이 이미 dev_err 로 이유를 남겼다. */
		return ret;

	/* [한국어] 이번에는 읽고 — */
	val = readl(pcie->apb_base + PCIE_REGS_PCIE_CFG);
	/* [한국어] PCIE_RSTN 을 **OR 로** 더해 쓴다. 그래서 1319 에서 세운 PCIE_DEVICE_TYPE 이
	 * 살아남는다. 이 순간 PCIe 코어가 리셋에서 풀린다. */
	writel(val | PCIE_RSTN, pcie->apb_base + PCIE_REGS_PCIE_CFG);
	/* [한국어] PERST# 를 규격이 요구하는 100ms 만큼 유지한 뒤 뗀다. 코어 리셋을 푼 직후여야
	 * 레퍼런스 클럭이 하위 장치에 안정적으로 공급된다. */
	keembay_ep_reset_deassert(pcie);

	/* [한국어] DWC 호스트 코어에 나머지를 맡긴다 — 여기서 ATU 창을 잡고,
	 * keembay_pcie_start_link() 를 거쳐 링크를 훈련시키고, 버스를 스캔한다. */
	ret = dw_pcie_host_init(pp);
	/* [한국어] 호스트 초기화 실패. */
	if (ret) {
		/* [한국어] PERST# 를 다시 눌러 하위 장치를 리셋 상태로 잡아 둔다.
		 * [상류 코드 관찰] 되감기가 여기까지다 — LTSSM, PCIE_RSTN, LJPLL, PHY SRAM 우회
		 * 비트는 켠 채로 남고, 걸어 둔 체인 핸들러도 떼지 않는다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		keembay_ep_reset_assert(pcie);
		/* [한국어] 실패 이유를 오류 코드와 함께 남긴다. 이 단계는 실패 원인이 여럿이라 값이 중요하다. */
		dev_err(dev, "Failed to initialize host: %d\n", ret);
		/* [한국어] 코어의 오류를 그대로 올린다. */
		return ret;
	}

	/* [한국어] 인터럽트 허가 레지스터를 읽는다 — EP 경로와 달리 기존 값을 보존한다. */
	val = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);
	/* [한국어] 커널이 MSI 를 지원하도록 빌드되었을 때만 — */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		/* [한국어] MSI 컨트롤러 인터럽트(비트 8)를 허가한다. IS_ENABLED 는 컴파일 시점 상수라
		 * CONFIG_PCI_MSI 가 꺼져 있으면 이 줄 자체가 사라진다. */
		val |= MSI_CTRL_INT_EN;
	/* [한국어] 되쓴다. 이 순간부터 위의 체인 핸들러가 실제로 불리기 시작한다.
	 * 링크와 버스 스캔이 모두 끝난 **뒤에** 인터럽트를 여는 순서다. */
	writel(val, pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);

	/* [한국어] RC 초기화 완료. */
	return 0;
}

/* [한국어]
 * keembay_pcie_probe - 플랫폼 드라이버 진입점. RC 와 EP 를 갈라 각자의 길로 보낸다
 *
 * @pdev: 디바이스 트리가 만들어 준 플랫폼 장치. compatible 로 역할이 갈린다.
 * @return: 0 = 성공. 음수 errno = 실패(-ENODEV, -ENOMEM, 하위 단계의 오류).
 *
 * 같은 IP 가 루트 컴플렉스로도 엔드포인트로도 합성될 수 있어, 이 함수는
 * "어느 쪽인지 알아내고 → 공통 준비를 한 뒤 → 갈라진다" 는 모양을 갖는다.
 *
 * 공통 준비 단계:
 *   1) device_get_match_data() 로 compatible 에 매달아 둔 keembay_pcie_of_data 를
 *      얻는다. 없으면 -ENODEV — 이 드라이버는 match 데이터 없이 붙을 수 없다.
 *   2) struct keembay_pcie 를 devm 으로 할당한다. 첫 필드가 dw_pcie 라
 *      이 한 번의 할당이 DWC 서술자까지 함께 잡는다.
 *   3) pci->dev 와 pci->ops(link_up/start_link/stop_link)를 채운다.
 *   4) "apb" 자원을 ioremap 한다. 이 창이 없으면 SoC 레지스터에 손댈 수 없다.
 *   5) platform_set_drvdata() 로 심는다. 이후 dev_get_drvdata() 로 어디서든
 *      되찾을 수 있게 되며, keembay_pcie_link_up() 등이 이에 의존한다.
 *
 * 갈라진 뒤:
 *   RC : CONFIG_PCIE_KEEMBAY_HOST 가 꺼져 있으면 -ENODEV.
 *        켜져 있으면 keembay_pcie_add_pcie_port() 의 결과를 그대로 반환한다.
 *   EP : CONFIG_PCIE_KEEMBAY_EP 가 꺼져 있으면 -ENODEV.
 *        ep.ops 를 걸고 dw_pcie_ep_init() → dw_pcie_ep_init_registers() →
 *        pci_epc_init_notify() 순으로 진행한 뒤 break 로 빠져 0 을 반환한다.
 *        **클럭도 PLL 도 PERST# 도 건드리지 않는다** — 전부 상대편 호스트가 준다.
 *   그 밖 : dev_err 후 -ENODEV. of_data 표가 둘 중 하나만 주므로 실제로는
 *        도달하지 않지만, 방어적으로 남겨 두었다.
 *
 * IS_ENABLED() 로 하는 검사가 요점이다. 하나의 모듈이 두 역할을 다 담고
 * 있으므로, 커널 설정에서 한쪽만 켰다면 나머지 compatible 은 조용히 거절해야 한다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 컨텍스트.
 *
 * 에러 경로: EP 경로에서 dw_pcie_ep_init_registers() 가 실패하면
 * dw_pcie_ep_deinit() 로 되감는다. dw_pcie_ep_init() 자체의 실패는 그 함수가
 * 자기 것을 정리한다는 전제로 그냥 반환한다. 그 밖의 실패는 devm 이 정리한다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어(driver_probe_device) → [이 함수]
 *     → device_get_match_data() → devm_platform_ioremap_resource_byname()
 *     → keembay_pcie_add_pcie_port()  (RC)
 *     → dw_pcie_ep_init() → dw_pcie_ep_init_registers() → pci_epc_init_notify()  (EP)
 */
static int keembay_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 디바이스 트리 compatible 에 매달린 match 데이터. const 라 읽기 전용이다. */
	const struct keembay_pcie_of_data *data;
	/* [한국어] 로그와 자원 조회의 기준 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버가 잡을 상태 구조체. */
	struct keembay_pcie *pcie;
	/* [한국어] 그 안의 DWC 서술자를 가리킬 임시 포인터. */
	struct dw_pcie *pci;
	/* [한국어] data->mode 를 잠시 담아 둘 지역 변수. */
	enum dw_pcie_device_mode mode;
	/* [한국어] EP 경로에서 쓰는 반환값. RC 경로는 곧바로 반환하므로 쓰지 않는다. */
	int ret;

	/* [한국어] compatible 에 매달아 둔 keembay_pcie_of_data 를 얻는다. OF 든 ACPI 든
	 * 같은 방식으로 동작하는 device property 계층의 헬퍼다. */
	data = device_get_match_data(dev);
	/* [한국어] match 데이터가 없다 — 이 드라이버는 그것 없이 붙을 수 없다. */
	if (!data)
		/* [한국어] 장치 없음으로 거절한다. */
		return -ENODEV;

	/* [한국어] RC/EP 를 지역 변수로 옮긴다. data->mode 가 이미 같은 enum 타입이라
	 * 이 캐스트는 값을 바꾸지 않는다 — 의도를 드러내려는 표기로 보인다. */
	mode = (enum dw_pcie_device_mode)data->mode;

	/* [한국어] 상태 구조체를 잡는다. **첫 필드가 dw_pcie 라 이 한 번의 할당이 DWC 서술자까지
	 * 함께 잡는다.** devm 판이라 프로브 실패나 장치 해제 시 자동으로 반납된다.
	 * kzalloc 계열이라 mode 는 일단 DW_PCIE_UNKNOWN_TYPE(0)이 된다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!pcie)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] 구조체 안의 DWC 서술자 주소를 잡는다. */
	pci = &pcie->pci;
	/* [한국어] DWC 코어가 로그·자원 조회에 쓸 장치를 심는다. 이후 dev_get_drvdata(pci->dev) 가
	 * 이 파일 상태를 되찾는 근거가 된다. */
	pci->dev = dev;
	/* [한국어] 컨트롤러 수준 콜백 표(link_up/start_link/stop_link)를 건다. RC 든 EP 든 공통이다. */
	pci->ops = &keembay_pcie_ops;

	/* [한국어] 역할을 기록한다. keembay_pcie_start_link() 가 이 값으로 EP 를 걸러 낸다. */
	pcie->mode = mode;

	/* [한국어] 디바이스 트리의 reg-names 에서 "apb" 자원을 찾아 ioremap 한다.
	 * 이 창이 이 파일의 모든 SoC 레지스터 접근의 기준이 된다. */
	pcie->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");
	/* [한국어] 매핑 실패는 오류 포인터로 온다. */
	if (IS_ERR(pcie->apb_base))
		/* [한국어] errno 를 꺼내 올린다. */
		return PTR_ERR(pcie->apb_base);

	/* [한국어] 상태를 장치에 심는다. **이 줄 뒤라야 dev_get_drvdata() 가 유효해지므로**,
	 * 그것에 의존하는 keembay_pcie_link_up() 등이 불리기 전에 있어야 한다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 역할에 따라 갈라진다. */
	switch (pcie->mode) {
	/* [한국어] 루트 컴플렉스 — */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 커널 설정에서 호스트 지원을 끄고 빌드했다면, */
		if (!IS_ENABLED(CONFIG_PCIE_KEEMBAY_HOST))
			/* [한국어] 이 compatible 을 조용히 거절한다. 하나의 모듈이 두 역할을 다 담고 있어
			 * 필요한 검사다. */
			return -ENODEV;

		/* [한국어] RC 초기화 전체를 맡기고 그 결과를 그대로 반환한다. */
		return keembay_pcie_add_pcie_port(pcie, pdev);
	/* [한국어] 엔드포인트 — */
	case DW_PCIE_EP_TYPE:
		/* [한국어] 커널 설정에서 EP 지원을 끄고 빌드했다면, */
		if (!IS_ENABLED(CONFIG_PCIE_KEEMBAY_EP))
			/* [한국어] 역시 거절한다. */
			return -ENODEV;

		/* [한국어] EP 콜백 표를 건다. dw_pcie_ep_init() 이 이 표를 통해 이 파일을 다시 부른다. */
		pci->ep.ops = &keembay_pcie_ep_ops;
		/* [한국어] DWC EP 코어를 초기화한다 — EPC 를 등록하고 내부 자료구조를 잡는다.
		 * 그 안에서 dw_pcie_ep_ops.init 콜백으로 keembay_pcie_ep_init() 이 불린다. */
		ret = dw_pcie_ep_init(&pci->ep);
		/* [한국어] EP 초기화 실패. */
		if (ret)
			/* [한국어] 그대로 올린다. 되감기는 그 함수가 자기 것을 정리한다는 전제다. */
			return ret;

		/* [한국어] EP 의 config 공간 레지스터(BAR 마스크 등)를 실제로 써 넣는다.
		 * dw_pcie_ep_init() 과 분리된 것은, 링크가 선 뒤에야 레지스터를 쓸 수 있는
		 * 하드웨어를 위해 순서를 나눌 수 있게 하려는 것이다. */
		ret = dw_pcie_ep_init_registers(&pci->ep);
		/* [한국어] 레지스터 초기화 실패. */
		if (ret) {
			/* [한국어] 어느 단계였는지 남긴다. */
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			/* [한국어] **여기서는 되감는다** — 위 dw_pcie_ep_init() 이 잡은 것을 푼다.
			 * 두 단계를 나눈 대가로 생긴 정리 책임이다. */
			dw_pcie_ep_deinit(&pci->ep);
			/* [한국어] 오류를 올린다. */
			return ret;
		}

		/* [한국어] EPF 드라이버들에게 '컨트롤러 준비 완료' 를 알린다. 이 알림을 받아야
		 * EPF 가 BAR 를 잡고 실제 기능을 올린다. */
		pci_epc_init_notify(pci->ep.epc);

		/* [한국어] EP 경로 정상 종료. switch 를 빠져나가 아래 공통 return 으로 간다. */
		break;
	/* [한국어] RC 도 EP 도 아닌 값 — of_data 표가 둘 중 하나만 주므로 실제로는 도달하지
	 * 않지만, 방어적으로 남겨 두었다. */
	default:
		/* [한국어] 어떤 값이었는지 남긴다. */
		dev_err(dev, "Invalid device type %d\n", pcie->mode);
		/* [한국어] 장치 없음으로 거절한다. */
		return -ENODEV;
	}

	/* [한국어] EP 경로의 성공 반환. RC 경로는 위에서 이미 반환했다. */
	return 0;
}

/* [한국어] "intel,keembay-pcie" 에 매달릴 match 데이터.
 * 설정자: 컴파일 시점 초기화자. static const 라 불변.
 * 읽는 자: keembay_pcie_probe() 가 device_get_match_data() 로 얻어 mode 만 읽는다.
 * 값 범위: mode = DW_PCIE_RC_TYPE 고정.
 * 동기화: 읽기 전용 상수. */
static const struct keembay_pcie_of_data keembay_pcie_rc_of_data = {
	/* [한국어] 이 compatible 은 루트 컴플렉스를 뜻한다. */
	.mode = DW_PCIE_RC_TYPE,
};

/* [한국어] "intel,keembay-pcie-ep" 에 매달릴 match 데이터.
 * 설정자: 컴파일 시점 초기화자. static const 라 불변.
 * 읽는 자: 위와 같다.
 * 값 범위: mode = DW_PCIE_EP_TYPE 고정.
 * 동기화: 읽기 전용 상수. */
static const struct keembay_pcie_of_data keembay_pcie_ep_of_data = {
	/* [한국어] 이 compatible 은 엔드포인트를 뜻한다. */
	.mode = DW_PCIE_EP_TYPE,
};

/* [한국어] 디바이스 트리 매칭 표. compatible 문자열과 위 두 상수를 짝지어,
 * 같은 드라이버가 두 역할로 붙을 수 있게 한다.
 * 설정자: 컴파일 시점 초기화자. static const 라 불변.
 * 읽는 자: 드라이버 코어의 매칭 로직과 device_get_match_data().
 * 값 범위: 항목 둘과 종료 표시 하나.
 * 동기화: 읽기 전용 상수. */
static const struct of_device_id keembay_pcie_of_match[] = {
	{
		/* [한국어] 루트 컴플렉스용 compatible 문자열. */
		.compatible = "intel,keembay-pcie",
		/* [한국어] 이 노드를 만나면 RC 로 동작하라는 표시. */
		.data = &keembay_pcie_rc_of_data,
	},
	{
		/* [한국어] 엔드포인트용 compatible 문자열. */
		.compatible = "intel,keembay-pcie-ep",
		/* [한국어] 이 노드를 만나면 EP 로 동작하라는 표시. */
		.data = &keembay_pcie_ep_of_data,
	},
	/* [한국어] 표의 끝을 알리는 빈 항목. 드라이버 코어가 이것을 만나면 순회를 멈춘다.
	 * MODULE_DEVICE_TABLE 이 없는 것은 이 드라이버가 모듈이 아니라
	 * 빌트인 전용이기 때문이다(아래 builtin_platform_driver 참고). */
	{}
};

/* [한국어] 플랫폼 드라이버 서술자.
 * 설정자: 컴파일 시점 초기화자. const 가 아닌 것은 드라이버 코어가 등록 과정에서
 *   내부 필드를 갱신하기 때문이다.
 * 읽는 자: 드라이버 코어(platform_driver_register).
 * 값 범위: 아래 필드 조합 그대로.
 * 동기화: 등록 이후 코어가 관리한다. */
static struct platform_driver keembay_pcie_driver = {
	.driver = {
		/* [한국어] sysfs 와 로그에 보이는 드라이버 이름. */
		.name = "keembay-pcie",
		/* [한국어] 위 매칭 표를 건다. 이것이 있어야 디바이스 트리 노드와 짝지어진다. */
		.of_match_table = keembay_pcie_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. **손으로 언바인드할 수 없게 막는 것** 으로,
		 * 이 드라이버에 .remove 가 없어 안전하게 떼어 낼 방법이 없기 때문이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 프로브 진입점. .remove 가 없다는 점과 짝을 이룬다. */
	.probe  = keembay_pcie_probe,
};
/* [한국어] 모듈이 아니라 **빌트인 전용** 으로 등록한다. 초기화 시점이 device_initcall 이라
 * 부팅 중 한 번 등록되고, 모듈 언로드 경로가 아예 존재하지 않는다.
 * 이 파일에 MODULE_ 계열 매크로가 하나도 없는 이유이기도 하다. */
builtin_platform_driver(keembay_pcie_driver);
