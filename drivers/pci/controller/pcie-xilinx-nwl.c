// SPDX-License-Identifier: GPL-2.0+
/* [한국어] 위 SPDX 줄은 커널의 라이선스 표기 규약이며 파일 첫 줄에
 * 정확한 형식으로만 있어야 한다. 그 줄에는 아무것도 덧붙이지 않는다. */
/*
 * PCIe host controller driver for NWL PCIe Bridge
 * Based on pcie-xilinx.c, pci-tegra.c
 *
 * (C) Copyright 2014 - 2015, Xilinx, Inc.
 */

/*
 * [한국어 설명] Zynq UltraScale+ 의 NWL PCIe 브리지 호스트 드라이버 (pcie-xilinx-nwl.c)
 *
 * === 파일의 역할 ===
 * Xilinx Zynq UltraScale+ MPSoC 에 들어 있는 NWL PCIe 브리지를 리눅스
 * 호스트 브리지로 물리는 드라이버다. 바로 위 상류 주석이 출신을 밝힌다 —
 * pcie-xilinx.c 와 pci-tegra.c 를 바탕으로 쓰였다.
 * 하는 일이 넷이다. 브리지 레지스터를 초기 상태로 만들고(BREG·ECAM·MSI
 * 인그레스 창을 켜고 주소를 심는다), PHY 와 클럭을 올리고, config 접근을
 * ECAM 창으로 이어 주고, 인터럽트 넷(misc·INTx·MSI 하위·MSI 상위)을 각각
 * 받아 나눠 보낸다.
 * config 접근 자체는 **이 파일이 거의 하지 않는다** — 주소만 만들어 주고
 * 실제 읽기·쓰기는 PCI 코어의 pci_generic_config_read/write 가 맡는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [PCI 코어]  probe.c, setup-bus.c, msi/ ...
 *        ^  struct pci_ops(map_bus 만 자체 구현), irq_domain 둘
 *   [이 파일]   pcie-xilinx-nwl.c — 공용 컨트롤러 층이 없다
 *        ^  nwl_bridge_readl/writel 로 브리지 레지스터 창에 직접
 *   [NWL 하드웨어] 브리지 레지스터(breg), 컨트롤러 레지스터(pcireg),
 *                  ECAM 창(cfg), PHY 넷, 레퍼런스 클럭
 *
 * 위층과 맞닿는 지점이 셋이다.
 *   struct pci_ops nwl_pcie_ops — **map_bus 만 자체 구현**하고 읽기·쓰기는
 *       코어의 generic 함수를 그대로 쓴다. ECAM 창이 있어 가능한 구조다.
 *   INTx irq_domain(intx_irq_domain) — 레거시 인터럽트 넷.
 *   MSI irq_domain(msi.dev_domain) — 벡터 64개(INT_PCI_MSI_NR = 2 x 32).
 * 아래로는 phy 와 clk 서브시스템을 쓴다. reset GPIO 는 쓰지 않는다.
 *
 * 실행 컨텍스트: probe/remove 는 프로세스 컨텍스트이고, 인터럽트 핸들러
 * 넷은 인터럽트 컨텍스트다. 그중 셋(INTx, MSI 하위, MSI 상위)은
 * **연쇄(chained) 핸들러**로 등록되어 상위 인터럽트 컨트롤러의 흐름 안에서
 * 직접 불리고, misc 만 보통의 devm_request_irq 로 등록된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 것:
 *   - ../pci.h — PCI 코어 내부 전용 헤더.
 *   - linux/pci-ecam.h — PCIE_ECAM_OFFSET() 매크로. (버스, devfn, 오프셋)을
 *     ECAM 창 안의 오프셋으로 바꾼다.
 *   - linux/irqchip/chained_irq.h — chained_irq_enter/exit. 연쇄 핸들러
 *     셋이 이것으로 상위 컨트롤러를 감싼다.
 *   - linux/irqchip/irq-msi-lib.h, irqdomain.h, msi.h — MSI 부모 도메인을
 *     직접 만든다.
 *   - phy, clk, of_address/of_pci/of_platform.
 * 이 파일에 의존하는 것: 없다. 심볼을 내보내지 않는 말단 플랫폼
 * 드라이버이며, **builtin_platform_driver 로 등록되어 모듈이 될 수 없다**.
 * 데이터 흐름: 장치 트리의 세 MMIO 자원(breg/pcireg/cfg)과 네 이름 있는
 * 인터럽트(misc/intx/msi0/msi1) -> nwl_pcie_parse_dt() -> struct nwl_pcie.
 *
 * === 주요 함수/구조체 요약 ===
 * nwl_pcie_probe()          : 진입점. 클럭·PHY·브리지·도메인 순으로 세운다.
 * nwl_pcie_bridge_init()    : 브리지 레지스터 초기화 본체. 링크도 기다린다.
 * nwl_pcie_map_bus()        : ECAM 창 안의 주소를 만들어 준다.
 * nwl_pcie_misc_handler()   : 오류·상태 인터럽트 열넷을 로그로 남긴다.
 * nwl_pcie_leg_handler()    : INTx 넷을 연쇄 핸들러로 받아 나눈다.
 * nwl_pcie_handle_msi_irq() : MSI 상태 레지스터 하나를 훑어 나눈다.
 * nwl_pcie_enable_msi()     : MSI 인그레스 창과 두 상태 레지스터를 켠다.
 * nwl_pcie_phy_enable()     : PHY 를 최대 넷까지 순서대로 켠다.
 * struct nwl_pcie           : 이 드라이버의 인스턴스 상태 전부.
 * struct nwl_msi            : MSI 비트맵·도메인·인터럽트 번호 둘.
 *
 * === 레지스터 창이 셋인 구조 ===
 * 장치 트리가 이름으로 세 개의 MMIO 자원을 준다.
 *   "breg"   -> breg_base   브리지 레지스터. 이 파일의 nwl_bridge_readl/
 *              writel 이 모두 이 창을 본다. 오프셋 정의가 전부 이 기준이다.
 *   "pcireg" -> pcireg_base 컨트롤러 레지스터. 이 파일이 읽는 것은
 *              PS_LINKUP_OFFSET 하나뿐이지만, **이 창의 물리 주소가
 *              MSI 목적지 주소로 쓰인다**.
 *   "cfg"    -> ecam_base   ECAM 창. map_bus 가 이 위에 오프셋을 얹는다.
 * 세 창의 물리 주소를 각각 phys_* 필드에 남겨 두는데, 브리지에게
 * "이 창이 어디에 있는지" 를 레지스터로 알려 줘야 하기 때문이다 —
 * E_BREG_BASE_LO/HI, E_ECAM_BASE_LO/HI, I_MSII_BASE_LO/HI 가 그 자리다.
 *
 * === 브리지가 능력을 스스로 알리는 구조 ===
 * 이 하드웨어는 기능마다 CAPABILITIES 레지스터를 두고 있고, 드라이버는
 * 켜기 전에 PRESENT 비트를 먼저 확인한다.
 *   E_BREG_CAPABILITIES  & BREG_PRESENT   — 브리지 이그레스 창이 있는가
 *   E_ECAM_CAPABILITIES  & E_ECAM_PRESENT — ECAM 창이 있는가
 *   I_MSII_CAPABILITIES  & MSII_PRESENT   — MSI 인그레스 창이 있는가
 * 셋 다 없으면 그 자리에서 실패한다. 즉 같은 드라이버가 구성이 다른
 * 하드웨어를 만나면 무엇이 없는지 알고 멈춘다.
 *
 * === 인터럽트가 넷으로 나뉘어 들어오는 구조 ===
 * 장치 트리가 이름 있는 인터럽트 넷을 준다. 요약 인터럽트 하나를 받아
 * 소프트웨어가 가르는 방식이 아니라, **하드웨어가 이미 넷으로 나눠 준다**.
 *
 *   "misc" -> nwl_pcie_misc_handler()      보통 핸들러(IRQF_SHARED)
 *             오류·AER·링크 상태 열넷을 로그로만 남기고 상태를 지운다.
 *   "intx" -> nwl_pcie_leg_handler()       **연쇄** 핸들러
 *             MSGF_LEG_STATUS 의 비트 넷을 intx_irq_domain 으로 보낸다.
 *   "msi0" -> nwl_pcie_msi_handler_low()   **연쇄** 핸들러
 *             MSGF_MSI_STATUS_LO 의 32비트 = 벡터 0~31.
 *   "msi1" -> nwl_pcie_msi_handler_high()  **연쇄** 핸들러
 *             MSGF_MSI_STATUS_HI 의 32비트 = 벡터 32~63.
 *
 * 연쇄 핸들러 셋은 chained_irq_enter/exit 로 감싸여, 자신을 부른 상위
 * 인터럽트 컨트롤러의 처리 흐름 안에서 돈다. 그래서 별도의 IRQ 번호를
 * 소비하지 않고 상위 컨트롤러의 마스크·EOI 처리를 그대로 따른다.
 *
 * [관찰] MSGF_LEG_MASK 는 이름이 "MASK" 이지만 **활성화 레지스터로
 * 쓰인다** — nwl_mask_intx_irq() 가 비트를 지우고 unmask 가 세운다.
 * 같은 레지스터를 bridge_init 도 "Disable all INTX interrupts" 에서
 * ~MASKALL 을, "Enable all INTX interrupts" 에서 MASKALL 을 쓴다.
 * MSGF_MISC_MASK 도 같은 방식이다.
 *
 * === MSI 벡터를 나누는 방식 ===
 * 벡터가 64개(INT_PCI_MSI_NR = 2 x 32)이고, 상태·마스크 레지스터가
 * 32비트짜리 둘(LO/HI)로 나뉘어 있다. 그래서 인터럽트 선도 둘이고
 * 핸들러도 둘이다. 비트맵은 struct nwl_msi 안에 DECLARE_BITMAP 으로
 * **정적으로** 잡혀 있고, 할당·해제는 뮤텍스로 직렬화한다.
 * MSI 목적지 주소는 pcireg 창의 물리 주소이며, 그 값이 두 곳에 쓰인다 —
 * nwl_pcie_enable_msi() 가 I_MSII_BASE_LO/HI 에 심고,
 * nwl_compose_msi_msg() 가 장치에 같은 주소를 알려 준다. 즉 엔드포인트가
 * 그 주소로 쓰면 브리지의 MSI 인그레스 창이 가로채 인터럽트로 바꾼다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 코드에서 읽히는 것만 적는다.
 *   - **링크 판별이 두 가지다.** PS_LINKUP 레지스터의 서로 다른 비트를
 *     보며, nwl_pcie_link_up() 은 PCIE_PHY_LINKUP_BIT 를,
 *     nwl_phy_link_up() 은 PHY_RDY_LINKUP_BIT 를 본다. 링크를 기다리는
 *     nwl_wait_for_link() 는 뒤쪽(PHY 준비)을 보고, config 접근 가능
 *     여부를 가리는 nwl_pcie_valid_device() 는 앞쪽을 본다.
 *   - **PHY 개수가 가변이다.** 최대 넷을 인덱스로 얻되 -ENODEV 를 만나면
 *     그 자리에서 멈추고 나머지를 NULL 로 둔다. 켜고 끄는 쪽은 늘 넷을
 *     도는데, phy_init(NULL) 등이 안전하다는 데 기댄다.
 *   - **ECAM 창 크기를 소프트웨어가 정한다.** E_ECAM_CONTROL 의 크기
 *     필드에 NWL_ECAM_MAX_SIZE(16)를 넣는다.
 *   - of_dma_is_coherent() 가 참이면 BRCFG_PCIE_RX1 에 CFG_PCIE_CACHE 를
 *     세운다. 그 자리의 상류 주석대로 PCIe DMA 트래픽을 CCI 경로로
 *     보내는 설정이다.
 *   - **nwl_pcie_remove() 가 IRQ 도메인을 없애지 않는다.** PHY 를 끄고
 *     클럭을 내리는 것이 전부다.
 *   - probe 의 실패 경로에도 도메인을 되돌리는 자리가 없다.
 *
 * === 같은 Xilinx 인 pcie-xilinx-dma-pl.c 와의 대비 ===
 * 코드에서 확인되는 차이만 적는다. 두 드라이버가 각각 어떤 형태의
 * 하드웨어(고정 블록인지 FPGA 로직인지)를 겨냥했는지는 이 소스에 적혀
 * 있지 않으므로 그 인과는 쓰지 않는다.
 *   레지스터 창 : 여기는 breg/pcireg/cfg 세 자원이 따로다. 저쪽은
 *                 pci_ecam_create() 가 만든 창 하나를 레지스터와 ECAM 에
 *                 함께 쓰고(XDMA), QDMA 판만 "breg" 를 따로 받는다.
 *   config 접근 : 양쪽 다 ECAM + generic read/write 다. 다만 저쪽은
 *                 pci_ecam_ops 를 통해 코어의 ECAM 골격을 그대로 쓰고,
 *                 여기는 pci_ops 에 map_bus 만 채운다.
 *   인터럽트    : 여기는 하드웨어가 넷으로 나눠 주고 셋이 연쇄 핸들러다.
 *                 저쪽은 주 인터럽트 하나(event)를 받아 32칸짜리 자체
 *                 도메인으로 갈라 보내고, INTx 마저 그 도메인의 한 칸을
 *                 통해 다시 갈라진다. 저쪽에는 연쇄 핸들러가 없고 모두
 *                 devm_request_irq(IRQF_SHARED | IRQF_NO_THREAD)다.
 *   MSI 비트맵  : 여기는 구조체 안에 정적으로, 저쪽은 kzalloc 으로 잡는다.
 *                 개수는 양쪽 다 64다.
 *   MSI 배분    : 여기는 generic_handle_domain_irq() 를 쓰고, 저쪽은
 *                 irq_find_mapping() + generic_handle_irq() 를 쓴다.
 *   변종 처리   : 여기는 compatible 이 하나("xlnx,nwl-pcie-2.11")뿐이다.
 *                 저쪽은 XDMA/QDMA 두 변종을 표로 갈라내고 레지스터
 *                 접근 함수마다 오프셋을 더할지 판단한다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 다만 Zynq UltraScale+ 보드에 NVMe SSD 를 물리면 그 SSD 가 보이기까지의
 * 경로가 이 파일이다. **MSI 벡터가 64개**라 CPU 수만큼 큐를 만들려는
 * NVMe 초기화가 그 상한에 걸릴 수 있다 — 다만 이 컨트롤러는 MSI-X 를
 * 알리지 않으므로(nwl_msi_parent_ops 의 지원 플래그에 MSI_FLAG_PCI_MSIX
 * 가 없다) NVMe 는 MSI 로 떨어진다. config 접근이 ECAM 이라 열거 자체는
 * 빠르고, DMA 일관성은 of_dma_is_coherent() 가 참일 때 CCI 경로로
 * 처리된다.
 */

#include <linux/clk.h>	/* [한국어] clk_prepare_enable 등 클럭 API. 레퍼런스 클럭 하나를 켜고 끈다 */
#include <linux/delay.h>	/* [한국어] usleep_range — 링크 대기에 쓴다 */
#include <linux/interrupt.h>	/* [한국어] devm_request_irq, IRQF_SHARED, irqreturn_t 등 */
#include <linux/irq.h>	/* [한국어] irq_set_chip_and_handler, handle_level_irq, irq_set_status_flags 등 */
#include <linux/irqchip/irq-msi-lib.h>	/* [한국어] msi_lib_init_dev_msi_info — MSI 부모 도메인을 만들 때 쓰는 공용 헬퍼 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_create_linear 등. **이 파일은 도메인을 둘 만든다**(INTx, MSI) */
#include <linux/kernel.h>	/* [한국어] 커널 일반 정의 */
#include <linux/init.h>	/* [한국어] __init 표시. 이 파일이 직접 쓰는 심볼은 없다 */
#include <linux/msi.h>	/* [한국어] struct msi_msg, msi_create_parent_irq_domain 등 MSI API */
#include <linux/of_address.h>	/* [한국어] of_address 헬퍼 */
#include <linux/of_pci.h>	/* [한국어] pci_irqd_intx_xlate — INTx 도메인의 xlate 로 쓴다 */
#include <linux/of_platform.h>	/* [한국어] of_dma_is_coherent — CCI 경로 설정 여부를 정하는 데 쓴다 */
#include <linux/pci.h>	/* [한국어] struct pci_ops, pci_generic_config_read/write, pci_host_probe 등 */
#include <linux/pci-ecam.h>	/* [한국어] **PCIE_ECAM_OFFSET() 매크로.** map_bus 가 ECAM 창 안의 오프셋을 만든다 */
#include <linux/phy/phy.h>	/* [한국어] phy_init/phy_power_on/phy_exit 등 generic PHY API */
#include <linux/platform_device.h>	/* [한국어] 플랫폼 드라이버 뼈대 */
#include <linux/irqchip/chained_irq.h>	/* [한국어] **chained_irq_enter/exit.** 연쇄 핸들러 셋이 이것으로 상위 컨트롤러를 감싼다 */

#include "../pci.h"	/* [한국어] drivers/pci 안쪽 전용 헤더 */

/* Bridge core config registers */
#define BRCFG_PCIE_RX0			0x00000000	/* [한국어] **브리지 코어 설정 레지스터 0.** DMA 채널 레지스터를 끄는 비트가 여기 있다 */
#define BRCFG_PCIE_RX1			0x00000004	/* [한국어] 같은 계열 1. CCI 캐시 경로 설정이 여기 있다 */
#define BRCFG_INTERRUPT			0x00000010	/* [한국어] 브리지 설정 인터럽트 레지스터 */
#define BRCFG_PCIE_RX_MSG_FILTER	0x00000020	/* [한국어] 수신 메시지 필터 레지스터. PM·INT·ERR 메시지를 전달할지 정한다 */

/* Egress - Bridge translation registers */
#define E_BREG_CAPABILITIES		0x00000200	/* [한국어] **이그레스(브리지 -> PCIe) 변환 창의 능력 레지스터.** PRESENT 비트를 확인하는 자리다 */
#define E_BREG_CONTROL			0x00000208	/* [한국어] 그 창의 제어 레지스터 */
#define E_BREG_BASE_LO			0x00000210	/* [한국어] 그 창의 물리 주소 하위 워드 */
#define E_BREG_BASE_HI			0x00000214	/* [한국어] 상위 워드 */
#define E_ECAM_CAPABILITIES		0x00000220	/* [한국어] **ECAM 창의 능력 레지스터** */
#define E_ECAM_CONTROL			0x00000228	/* [한국어] 그 창의 제어 레지스터. 활성화 비트와 크기 필드가 여기 있다 */
#define E_ECAM_BASE_LO			0x00000230	/* [한국어] ECAM 창의 물리 주소 하위 워드 */
#define E_ECAM_BASE_HI			0x00000234	/* [한국어] 상위 워드 */

/* Ingress - address translations */
#define I_MSII_CAPABILITIES		0x00000300	/* [한국어] **인그레스(PCIe -> 브리지) MSI 창의 능력 레지스터** */
#define I_MSII_CONTROL			0x00000308	/* [한국어] 그 창의 제어 레지스터. 활성화와 상태 활성화 비트가 여기 있다 */
#define I_MSII_BASE_LO			0x00000310	/* [한국어] **MSI 목적지 주소 하위 워드.** 여기 심은 주소로 오는 쓰기가 MSI 가 된다 */
#define I_MSII_BASE_HI			0x00000314	/* [한국어] 상위 워드 */

#define I_ISUB_CONTROL			0x000003E8	/* [한국어] 인그레스 subtractive 디코드 제어 레지스터 */
#define SET_ISUB_CONTROL		BIT(0)	/* [한국어] 그것을 켜는 비트 */
/* Rxed msg fifo  - Interrupt status registers */
#define MSGF_MISC_STATUS		0x00000400	/* [한국어] **수신 메시지 FIFO 의 기타 인터럽트 상태 레지스터** */
#define MSGF_MISC_MASK			0x00000404	/* [한국어] 그 마스크 레지스터. **이름과 달리 활성화 레지스터로 쓰인다** */
#define MSGF_LEG_STATUS			0x00000420	/* [한국어] INTx 인터럽트 상태 레지스터 */
#define MSGF_LEG_MASK			0x00000424	/* [한국어] 그 마스크(=활성화) 레지스터 */
#define MSGF_MSI_STATUS_LO		0x00000440	/* [한국어] **MSI 하위 32벡터의 상태 레지스터** */
#define MSGF_MSI_STATUS_HI		0x00000444	/* [한국어] 상위 32벡터의 상태 레지스터 */
#define MSGF_MSI_MASK_LO		0x00000448	/* [한국어] 하위 32벡터의 마스크(=활성화) 레지스터 */
#define MSGF_MSI_MASK_HI		0x0000044C	/* [한국어] 상위 32벡터의 마스크(=활성화) 레지스터 */

/* Msg filter mask bits */
#define CFG_ENABLE_PM_MSG_FWD		BIT(1)	/* [한국어] PM 메시지 전달 허용 */
#define CFG_ENABLE_INT_MSG_FWD		BIT(2)	/* [한국어] INT 메시지 전달 허용 */
#define CFG_ENABLE_ERR_MSG_FWD		BIT(3)	/* [한국어] ERR 메시지 전달 허용 */
/* [한국어] 위 셋을 묶은 마스크. nwl_pcie_bridge_init() 이 이 값을 그대로 써서
 * PM 과 INT 와 ERR 메시지 전달을 한꺼번에 연다. 줄 잇기 백슬래시로 세 줄에
 * 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define CFG_ENABLE_MSG_FILTER_MASK	(CFG_ENABLE_PM_MSG_FWD |\
					CFG_ENABLE_INT_MSG_FWD |\
					CFG_ENABLE_ERR_MSG_FWD)

/* Misc interrupt status mask bits */
#define MSGF_MISC_SR_RXMSG_AVAIL	BIT(0)	/* [한국어] 수신 메시지가 도착했다 */
#define MSGF_MISC_SR_RXMSG_OVER		BIT(1)	/* [한국어] 수신 메시지 FIFO 가 넘쳤다 */
#define MSGF_MISC_SR_SLAVE_ERR		BIT(4)	/* [한국어] 슬레이브 오류 */
#define MSGF_MISC_SR_MASTER_ERR		BIT(5)	/* [한국어] 마스터 오류 */
#define MSGF_MISC_SR_I_ADDR_ERR		BIT(6)	/* [한국어] 인그레스 주소 변환 오류 */
#define MSGF_MISC_SR_E_ADDR_ERR		BIT(7)	/* [한국어] 이그레스 주소 변환 오류 */
#define MSGF_MISC_SR_FATAL_AER		BIT(16)	/* [한국어] AER 의 치명적 오류 */
#define MSGF_MISC_SR_NON_FATAL_AER	BIT(17)	/* [한국어] AER 의 치명적이지 않은 오류 */
#define MSGF_MISC_SR_CORR_AER		BIT(18)	/* [한국어] AER 의 정정 가능 오류 */
#define MSGF_MISC_SR_UR_DETECT		BIT(20)	/* [한국어] 지원하지 않는 요청을 검출했다 */
#define MSGF_MISC_SR_NON_FATAL_DEV	BIT(22)	/* [한국어] 치명적이지 않은 오류를 검출했다 */
#define MSGF_MISC_SR_FATAL_DEV		BIT(23)	/* [한국어] 치명적 오류를 검출했다 */
#define MSGF_MISC_SR_LINK_DOWN		BIT(24)	/* [한국어] 링크가 내려갔다 */
#define MSGF_MISC_SR_LINK_AUTO_BWIDTH	BIT(25)	/* [한국어] 링크 대역폭 자율 관리 상태 비트가 섰다 */
#define MSGF_MISC_SR_LINK_BWIDTH	BIT(26)	/* [한국어] 링크 대역폭 관리 상태 비트가 섰다 */

/* [한국어] 위 열다섯 상태 비트를 모두 묶은 마스크. 쓰이는 곳이 둘이다 —
 * nwl_pcie_misc_handler() 가 읽은 상태에 이것을 씌워 우리 비트만 남기고,
 * nwl_pcie_bridge_init() 이 이 값과 그 보수로 misc 인터럽트를 켜고 끈다.
 * 줄 잇기 백슬래시로 여러 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define MSGF_MISC_SR_MASKALL		(MSGF_MISC_SR_RXMSG_AVAIL |\
					MSGF_MISC_SR_RXMSG_OVER |\
					MSGF_MISC_SR_SLAVE_ERR |\
					MSGF_MISC_SR_MASTER_ERR |\
					MSGF_MISC_SR_I_ADDR_ERR |\
					MSGF_MISC_SR_E_ADDR_ERR |\
					MSGF_MISC_SR_FATAL_AER |\
					MSGF_MISC_SR_NON_FATAL_AER |\
					MSGF_MISC_SR_CORR_AER |\
					MSGF_MISC_SR_UR_DETECT |\
					MSGF_MISC_SR_NON_FATAL_DEV |\
					MSGF_MISC_SR_FATAL_DEV |\
					MSGF_MISC_SR_LINK_DOWN |\
					MSGF_MISC_SR_LINK_AUTO_BWIDTH |\
					MSGF_MISC_SR_LINK_BWIDTH)

/* Legacy interrupt status mask bits */
#define MSGF_LEG_SR_INTA		BIT(0)	/* [한국어] INTA */
#define MSGF_LEG_SR_INTB		BIT(1)	/* [한국어] INTB */
#define MSGF_LEG_SR_INTC		BIT(2)	/* [한국어] INTC */
#define MSGF_LEG_SR_INTD		BIT(3)	/* [한국어] INTD */
/* [한국어] INTA~INTD 넷을 묶은 마스크. nwl_pcie_leg_handler() 가 읽은 상태에
 * 이것을 씌워 INTx 비트만 남기고, nwl_pcie_bridge_init() 이 이 값과 그
 * 보수로 INTx 를 켜고 끈다. 줄 잇기 백슬래시로 두 줄에 걸쳐 있어 각 줄에
 * 끝 주석을 붙일 수 없다. */
#define MSGF_LEG_SR_MASKALL		(MSGF_LEG_SR_INTA | MSGF_LEG_SR_INTB |\
					MSGF_LEG_SR_INTC | MSGF_LEG_SR_INTD)

/* MSI interrupt status mask bits */
#define MSGF_MSI_SR_LO_MASK		GENMASK(31, 0)	/* [한국어] MSI 하위 상태 레지스터의 전체 비트(32개) */
#define MSGF_MSI_SR_HI_MASK		GENMASK(31, 0)	/* [한국어] 상위 상태 레지스터의 전체 비트(32개) */

#define MSII_PRESENT			BIT(0)	/* [한국어] MSI 인그레스 창이 하드웨어에 있는지 알리는 비트 */
#define MSII_ENABLE			BIT(0)	/* [한국어] 그 창을 켜는 비트 */
#define MSII_STATUS_ENABLE		BIT(15)	/* [한국어] 그 창의 상태 보고를 켜는 비트 */

/* Bridge config interrupt mask */
#define BRCFG_INTERRUPT_MASK		BIT(0)	/* [한국어] 브리지 설정 인터럽트를 켜는 비트 */
#define BREG_PRESENT			BIT(0)	/* [한국어] 브리지 이그레스 창이 하드웨어에 있는지 알리는 비트 */
#define BREG_ENABLE			BIT(0)	/* [한국어] 그 창을 켜는 비트 */
#define BREG_ENABLE_FORCE		BIT(1)	/* [한국어] 강제 활성화 비트. bridge_init 이 **이 비트를 뺀** 값을 쓴다 */

/* E_ECAM status mask bits */
#define E_ECAM_PRESENT			BIT(0)	/* [한국어] ECAM 창이 하드웨어에 있는지 알리는 비트 */
#define E_ECAM_CR_ENABLE		BIT(0)	/* [한국어] ECAM 창을 켜는 비트 */
#define E_ECAM_SIZE_LOC			GENMASK(20, 16)	/* [한국어] ECAM 창의 크기 필드 */
#define E_ECAM_SIZE_SHIFT		16	/* [한국어] 그 필드의 비트 위치 */
#define NWL_ECAM_MAX_SIZE		16	/* [한국어] **심을 크기 값.** bridge_init 이 이 값을 그 필드에 넣는다 */

#define CFG_DMA_REG_BAR			GENMASK(2, 0)	/* [한국어] DMA 채널 레지스터를 끄는 비트들 */
#define CFG_PCIE_CACHE			GENMASK(7, 0)	/* [한국어] CCI 캐시 경로 설정 비트들 */

#define INT_PCI_MSI_NR			(2 * 32)	/* [한국어] **MSI 벡터 수. 32비트 상태 레지스터 둘이라 64개다** */

/* Readin the PS_LINKUP */
#define PS_LINKUP_OFFSET		0x00000238	/* [한국어] PS_LINKUP 레지스터의 오프셋. **breg 가 아니라 pcireg 창 기준이다** */
#define PCIE_PHY_LINKUP_BIT		BIT(0)	/* [한국어] PCIe 링크가 섰음을 알리는 비트. nwl_pcie_link_up() 이 본다 */
#define PHY_RDY_LINKUP_BIT		BIT(1)	/* [한국어] PHY 가 준비되어 링크가 올라왔음을 알리는 비트. nwl_phy_link_up() 이 본다 */

/* Parameters for the waiting for link up routine */
#define LINK_WAIT_MAX_RETRIES          10	/* [한국어] 링크를 기다리는 최대 횟수 */
#define LINK_WAIT_USLEEP_MIN           90000	/* [한국어] 그 대기의 하한 */
#define LINK_WAIT_USLEEP_MAX           100000	/* [한국어] 그 대기의 상한. **10회 x 90ms 로 최소 900ms** 다 */

struct nwl_msi {			/* MSI information */
	/* [한국어]
	 * DECLARE_BITMAP(bitmap, INT_PCI_MSI_NR);
	 * 64개 MSI 벡터 중 어느 것이 쓰이고 있는지를 담은 비트맵.
	 * 설정자: nwl_irq_domain_alloc() 의 bitmap_find_free_region() 이 구간을
	 * 잡고, nwl_irq_domain_free() 의 bitmap_release_region() 이 놓는다.
	 * 읽는 자: 같은 두 함수뿐이다.
	 * 값 범위: 비트 하나가 벡터 하나. **연속이고 2의 거듭제곱 개수인 구간
	 * 으로만 잡힌다** — 다중 MSI 의 규격 제약 때문이다.
	 * 동기화: 아래 lock 뮤텍스로 지킨다.
	 * [대비] 같은 계열의 pcie-xilinx-dma-pl.c 는 같은 크기의 비트맵을
	 * kzalloc 으로 잡는다. 여기는 구조체 안에 정적으로 둔다.
	 */
	DECLARE_BITMAP(bitmap, INT_PCI_MSI_NR);
	/* [한국어]
	 * struct irq_domain *dev_domain;
	 * MSI 부모 도메인. 크기가 INT_PCI_MSI_NR(64)이다.
	 * 설정자: nwl_pcie_init_msi_irq_domain() 의
	 * msi_create_parent_irq_domain(). CONFIG_PCI_MSI 가 꺼져 있으면 NULL 인
	 * 채로 남는다.
	 * 읽는 자: nwl_pcie_handle_msi_irq() 가 벡터마다
	 * generic_handle_domain_irq() 를 부른다.
	 * [관찰] 이 도메인을 없애는 코드는 이 파일에 없다.
	 */
	struct irq_domain *dev_domain;
	/* [한국어]
	 * struct mutex lock;
	 * 위 비트맵을 지키는 뮤텍스. 옆의 상류 주석이 그 용도를 밝힌다.
	 * 설정자: nwl_pcie_enable_msi() 의 mutex_init().
	 * 읽는 자: MSI 벡터 할당과 해제 두 곳뿐이다.
	 * 왜 스핀락이 아닌가: 그 두 경로가 프로세스 컨텍스트에서만 불려 잠들 수
	 * 있기 때문이다. 인터럽트 컨텍스트에서 도는 nwl_pcie_handle_msi_irq() 는
	 * 비트맵을 보지 않으므로 이 락과 무관하다.
	 * [관찰] mutex_init() 이 도메인을 만드는 함수가 아니라 enable_msi() 에
	 * 있어, CONFIG_PCI_MSI 가 켜져 있어도 probe 가 그 함수까지 가지 못하면
	 * 초기화되지 않는다.
	 */
	struct mutex lock;		/* protect bitmap variable */
	/* [한국어]
	 * int irq_msi0;
	 * 하위 32개 MSI 벡터를 나르는 인터럽트 번호.
	 * 설정자: nwl_pcie_enable_msi() 가 장치 트리의 "msi0" 이름으로 얻는다.
	 * 읽는 자: 같은 함수가 곧바로 irq_set_chained_handler_and_data() 에
	 * 넘겨 nwl_pcie_msi_handler_low() 를 건다. 그 뒤로 읽는 곳은 없다.
	 * 값 범위: 음수면 장치 트리에 그 이름이 없다는 뜻이라 probe 가 접힌다.
	 */
	int irq_msi0;
	/* [한국어]
	 * int irq_msi1;
	 * 상위 32개 MSI 벡터를 나르는 인터럽트 번호.
	 * 설정자/읽는 자는 irq_msi0 과 같으며, 이름이 "msi1" 이고 붙는 핸들러가
	 * nwl_pcie_msi_handler_high() 인 것만 다르다.
	 * **두 개인 이유**는 MSI 상태·마스크 레지스터가 32비트짜리 둘로 나뉘어
	 * 있고 벡터가 64개이기 때문이다.
	 */
	int irq_msi1;
};

struct nwl_pcie {
	/* [한국어]
	 * struct device *dev;
	 * 이 드라이버가 붙은 장치.
	 * 설정자: nwl_pcie_probe() 가 맨 처음 담는다.
	 * 읽는 자: 파일 전체에서 오류 메시지를 낼 장치와 of_node 의 출처로 쓴다.
	 * 동기화: probe 이후 바뀌지 않는다.
	 */
	struct device *dev;
	/* [한국어]
	 * void __iomem *breg_base;
	 * **브리지 레지스터 창의 커널 가상 주소.**
	 * 설정자: nwl_pcie_parse_dt() 가 장치 트리의 "breg" 자원을 매핑해 얻는다.
	 * 읽는 자: nwl_bridge_readl()/writel() 둘뿐이며, 이 파일의 거의 모든
	 * 레지스터 접근이 그 둘을 거친다. 파일 앞부분의 BRCFG_ 계열,
	 * E_BREG_ 계열, E_ECAM_ 계열, I_MSII_ 계열, MSGF_ 계열 정의가 모두 이
	 * 창 기준의 오프셋이다.
	 */
	void __iomem *breg_base;
	/* [한국어]
	 * void __iomem *pcireg_base;
	 * 컨트롤러 레지스터 창의 커널 가상 주소.
	 * 설정자: parse_dt 가 "pcireg" 자원을 매핑해 얻는다.
	 * 읽는 자: nwl_pcie_link_up() 과 nwl_phy_link_up() 뿐이며, 둘 다
	 * PS_LINKUP_OFFSET 하나만 읽는다. **이 창에서 읽는 레지스터는 그 하나뿐
	 * 이다**(전수 grep 확인).
	 */
	void __iomem *pcireg_base;
	/* [한국어]
	 * void __iomem *ecam_base;
	 * ECAM 창의 커널 가상 주소.
	 * 설정자: parse_dt 가 "cfg" 자원을 devm_pci_remap_cfg_resource() 로
	 * 매핑해 얻는다 — config 공간에 맞는 메모리 속성으로 잡는 전용 헬퍼다.
	 * 읽는 자: nwl_pcie_map_bus() 가 여기에 ECAM 오프셋을 얹어 주소를 만들고,
	 * 실제 읽기·쓰기는 PCI 코어의 generic config 함수가 한다.
	 */
	void __iomem *ecam_base;
	/* [한국어]
	 * struct phy *phy[4];
	 * 이 컨트롤러가 쓰는 PHY(레인)들. 최대 넷이다.
	 * 설정자: parse_dt 가 devm_of_phy_get_by_index() 로 0번부터 채우다가
	 * **-ENODEV 를 만나면 그 자리를 NULL 로 두고 멈춘다** — 즉 보드에 있는
	 * 레인 수만큼만 채워진다.
	 * 읽는 자: nwl_pcie_phy_enable()/disable() 과 그 하위 두 함수. 켜고 끄는
	 * 쪽은 늘 넷을 도는데, phy_init(NULL) 등이 안전하다는 데 기댄다.
	 * 값 범위: 앞에서부터 채워지고 뒤쪽이 NULL 이다.
	 */
	struct phy *phy[4];
	/* [한국어]
	 * phys_addr_t phys_breg_base;
	 * 브리지 레지스터 창의 **물리** 주소. 옆의 상류 주석이 그렇게 적는다.
	 * 설정자: parse_dt 가 자원의 start 를 담는다.
	 * 읽는 자: nwl_pcie_bridge_init() 이 E_BREG_BASE_LO/HI 에 심는다.
	 * 왜 필요한가: 브리지에게 "네 레지스터 창이 어느 물리 주소에 있는지" 를
	 * 알려 줘야 이그레스 변환이 성립하기 때문이다.
	 */
	phys_addr_t phys_breg_base;	/* Physical Bridge Register Base */
	/* [한국어]
	 * phys_addr_t phys_pcie_reg_base;
	 * 컨트롤러 레지스터 창의 물리 주소. 옆의 상류 주석이 그렇게 적는다.
	 * 설정자: parse_dt.
	 * 읽는 자: **두 곳이 같은 값을 쓴다.** nwl_pcie_enable_msi() 가
	 * I_MSII_BASE_LO/HI 에 심고, nwl_compose_msi_msg() 가 장치에 알려 줄 MSI
	 * 목적지 주소로 쓴다. 엔드포인트가 그 주소로 쓰면 브리지의 MSI 인그레스
	 * 창이 가로채 인터럽트로 바꾼다.
	 */
	phys_addr_t phys_pcie_reg_base;	/* Physical PCIe Controller Base */
	/* [한국어]
	 * phys_addr_t phys_ecam_base;
	 * ECAM 창의 물리 주소. 옆의 상류 주석이 그렇게 적는다.
	 * 설정자: parse_dt.
	 * 읽는 자: nwl_pcie_bridge_init() 이 E_ECAM_BASE_LO/HI 에 심는다.
	 */
	phys_addr_t phys_ecam_base;	/* Physical Configuration Base */
	/* [한국어]
	 * u32 breg_size;
	 * 브리지 레지스터 창의 크기.
	 * [관찰] 이 파일에서 읽거나 쓰는 곳이 없다(전수 grep 확인). 아래 두
	 * 필드도 같다.
	 */
	u32 breg_size;
	/* [한국어]
	 * u32 pcie_reg_size;
	 * 컨트롤러 레지스터 창의 크기. 읽거나 쓰는 곳이 없다.
	 */
	u32 pcie_reg_size;
	/* [한국어]
	 * u32 ecam_size;
	 * ECAM 창의 크기. 읽거나 쓰는 곳이 없다.
	 * [관찰] ECAM 창의 크기는 이 필드가 아니라 E_ECAM_CONTROL 의 크기 필드에
	 * NWL_ECAM_MAX_SIZE(16)를 직접 넣는 방식으로 정해진다.
	 */
	u32 ecam_size;
	/* [한국어]
	 * int irq_intx;
	 * INTx 넷을 나르는 인터럽트 번호.
	 * 설정자: nwl_pcie_parse_dt() 가 장치 트리의 "intx" 이름으로 얻는다.
	 * 읽는 자: 같은 함수가 곧바로 irq_set_chained_handler_and_data() 로
	 * nwl_pcie_leg_handler() 를 건다. 그 뒤로 읽는 곳은 없다.
	 */
	int irq_intx;
	/* [한국어]
	 * int irq_misc;
	 * 오류·상태 인터럽트를 나르는 번호.
	 * 설정자: nwl_pcie_bridge_init() 이 "misc" 이름으로 얻는다 — **다른 셋과
	 * 달리 parse_dt 가 아니라 여기서 얻는다**.
	 * 읽는 자: 같은 함수가 devm_request_irq() 에 넘기고 실패 메시지에도 쓴다.
	 * **이 인터럽트만 연쇄 핸들러가 아닌 보통의 핸들러**로 등록된다.
	 */
	int irq_misc;
	/* [한국어]
	 * struct nwl_msi msi;
	 * MSI 관련 상태를 모아 둔 하위 구조체(비트맵, 도메인, 뮤텍스, 인터럽트
	 * 번호 둘). 포인터가 아니라 값으로 품고 있어 별도 할당이 없다.
	 * 설정자/읽는 자: 위 struct nwl_msi 의 각 필드 설명을 따른다.
	 */
	struct nwl_msi msi;
	/* [한국어]
	 * struct irq_domain *intx_irq_domain;
	 * INTx(레거시) 인터럽트 도메인. 크기가 PCI_NUM_INTX(4)다.
	 * 설정자: nwl_pcie_init_irq_domain() 이 장치 트리의 인터럽트 컨트롤러
	 * 자식 노드에 붙여 만든다.
	 * 읽는 자: nwl_pcie_leg_handler() 가 세워진 비트마다
	 * generic_handle_domain_irq() 를 부른다.
	 * [관찰] 이 도메인을 없애는 코드는 이 파일에 없다.
	 */
	struct irq_domain *intx_irq_domain;
	/* [한국어]
	 * struct clk *clk;
	 * PCIe 레퍼런스 클럭.
	 * 설정자: nwl_pcie_probe() 가 devm_clk_get(dev, NULL) 로 **이름 없이
	 * 하나만** 얻는다.
	 * 읽는 자: probe 가 켜고, 실패 경로와 nwl_pcie_remove() 가 끈다.
	 * 동기화: 클럭 서브시스템이 자체 락을 갖는다.
	 */
	struct clk *clk;
	/* [한국어]
	 * raw_spinlock_t leg_mask_lock;
	 * INTx 마스크 레지스터(MSGF_LEG_MASK)를 지키는 락.
	 * 설정자: nwl_pcie_init_irq_domain() 의 raw_spin_lock_init().
	 * 읽는 자: nwl_mask_intx_irq()/unmask 가 irqsave 판으로 잡는다.
	 * 왜 raw 이고 irqsave 인가: 두 함수가 **인터럽트 컨텍스트에서도 불릴 수
	 * 있어** 잠들 수 없고, 읽고-고치고-쓰는 세 단계를 원자적으로 해야 하기
	 * 때문이다. INTx 는 레벨 인터럽트라 핸들러가 처리 중에 마스크를 건다.
	 * [관찰] MSI 마스크 레지스터를 지키는 락은 없다 — 개별 벡터를 마스크하는
	 * 코드 자체가 없기 때문이다.
	 */
	raw_spinlock_t leg_mask_lock;
};

/* [한국어]
 * nwl_bridge_readl - 브리지 레지스터 창에서 32비트를 읽는다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @off:  breg 창 시작으로부터의 오프셋.
 * @return: 읽은 값.
 *
 * 이 파일이 브리지를 만지는 두 통로 중 하나다. breg_base 는 parse_dt 가
 * 장치 트리의 "breg" 자원을 매핑해 얻은 커널 가상 주소이며, 파일 앞부분의
 * 레지스터 정의(BRCFG_*, E_BREG_*, E_ECAM_*, I_MSII_*, MSGF_*)가 모두 이
 * 창 기준의 오프셋이다.
 *
 * **pcireg 와 ecam 창에는 이 함수를 쓰지 않는다** — 그 둘은 각각
 * readl(pcie->pcireg_base + ...) 로 직접 읽거나, PCI 코어의 generic
 * config 함수가 다룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽. 그 자체로는
 * 직렬화하지 않으며, 필요한 곳에서 호출자가 락을 잡는다.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> readl()
 */
static inline u32 nwl_bridge_readl(struct nwl_pcie *pcie, u32 off)
{
	return readl(pcie->breg_base + off);	/* [한국어] breg 창 시작에 오프셋을 더한 곳에서 32비트를 읽는다 */
}

/* [한국어]
 * nwl_bridge_writel - 브리지 레지스터 창에 32비트를 쓴다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @val:  쓸 값.
 * @off:  breg 창 시작으로부터의 오프셋.
 *
 * nwl_bridge_readl() 의 짝이다. 인자 순서가 (값, 오프셋)이라 커널의
 * writel(val, addr) 과 같은 감각이지만, 읽기 쪽은 (인스턴스, 오프셋)이라
 * 인자 개수가 달라 헷갈리기 쉬운 부분이다.
 *
 * 이 컨트롤러의 브리지 레지스터는 모두 32비트 단위라 바이트나 워드
 * 접근 판이 따로 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> writel()
 */
static inline void nwl_bridge_writel(struct nwl_pcie *pcie, u32 val, u32 off)
{
	writel(val, pcie->breg_base + off);	/* [한국어] 같은 방식으로 32비트를 쓴다. 인자 순서가 커널의 writel(값, 주소)과 같다 */
}

/* [한국어]
 * nwl_pcie_link_up - PCIe 링크가 서 있는지 본다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 링크가 서 있다.
 *
 * **breg 가 아니라 pcireg 창을 읽는다.** PS_LINKUP 레지스터의
 * PCIE_PHY_LINKUP_BIT 하나만 본다.
 *
 * 아래 nwl_phy_link_up() 과 **보는 비트가 다르다** — 저쪽은
 * PHY_RDY_LINKUP_BIT 로 PHY 준비 여부를 보고, 이쪽은 PCIe 링크 자체가
 * 섰는지를 본다. 그래서 쓰이는 자리도 다르다. 이 함수는 config 접근을
 * 허용할지 가릴 때(nwl_pcie_valid_device)와 초기화 끝의 로그에 쓰이고,
 * 저쪽은 초기화 중 링크를 기다릴 때 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근, probe).
 *
 * 호출 체인:
 *   nwl_pcie_valid_device() / nwl_pcie_bridge_init() -> [이 함수] -> readl()
 */
static bool nwl_pcie_link_up(struct nwl_pcie *pcie)
{
	if (readl(pcie->pcireg_base + PS_LINKUP_OFFSET) & PCIE_PHY_LINKUP_BIT)	/* [한국어] **pcireg 창의 PS_LINKUP 레지스터를 읽어** PCIe 링크 비트를 본다 */
		return true;	/* [한국어] 서 있으면 링크가 섰다 */
	return false;	/* [한국어] 아니면 아니다 */
}

/* [한국어]
 * nwl_phy_link_up - PHY 가 준비되어 링크가 올라왔는지 본다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 PHY 쪽 링크가 올라왔다.
 *
 * 같은 PS_LINKUP 레지스터의 PHY_RDY_LINKUP_BIT 를 본다. 위
 * nwl_pcie_link_up() 과 레지스터는 같고 비트만 다르다.
 *
 * 부르는 곳은 nwl_wait_for_link() 하나뿐이다(전수 grep 확인) — 브리지
 * 초기화 중 링크가 올라오기를 기다리는 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  nwl_wait_for_link() -> [이 함수] -> readl()
 */
static bool nwl_phy_link_up(struct nwl_pcie *pcie)
{
	if (readl(pcie->pcireg_base + PS_LINKUP_OFFSET) & PHY_RDY_LINKUP_BIT)	/* [한국어] 같은 레지스터에서 **PHY 준비 비트**를 본다 — 위 함수와 보는 비트가 다르다 */
		return true;	/* [한국어] 서 있으면 PHY 쪽 링크가 올라왔다 */
	return false;	/* [한국어] 아니면 아니다 */
}

/* [한국어]
 * nwl_wait_for_link - PHY 링크가 올라올 때까지 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 올라왔다. 시간이 지나면 -ETIMEDOUT.
 *
 * 90~100ms 간격으로 최대 10회 보므로 **최소 900ms 를 기다린다.**
 * 첫 회차에서 이미 올라와 있으면 곧바로 0 을 돌려주므로 기다리지 않는다.
 *
 * nwl_pcie_bridge_init() 이 BREG 를 켜고 메시지 필터를 설정한 뒤,
 * ECAM 을 켜기 전에 부른다 — 링크가 없으면 ECAM 을 켜도 읽을 것이 없기
 * 때문이다. **실패하면 초기화가 그 자리에서 접힌다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). usleep 으로 잠든다.
 *
 * 호출 체인:  nwl_pcie_bridge_init() -> [이 함수] -> nwl_phy_link_up()
 */
static int nwl_wait_for_link(struct nwl_pcie *pcie)
{
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	int retries;	/* [한국어] 반복 횟수 */

	/* check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {	/* [한국어] 최대 10회 본다 */
		if (nwl_phy_link_up(pcie))	/* [한국어] 이미 올라와 있으면 */
			return 0;	/* [한국어] 곧바로 성공으로 나간다 — 기다리지 않는다 */
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);	/* [한국어] 90~100ms 쉬고 다시 본다. **10회면 최소 900ms** 다 */
	}

	dev_err(dev, "PHY link never came up\n");	/* [한국어] 끝까지 안 올라오면 알리고 */
	return -ETIMEDOUT;	/* [한국어] 시간 초과로 돌아간다. 브리지 초기화가 여기서 접힌다 */
}

/* [한국어]
 * nwl_pcie_valid_device - 이 (버스, devfn) 조합에 config 접근을 해도 되는지 가른다
 *
 * @bus:   대상 버스. sysdata 에 이 드라이버 인스턴스가 들어 있다.
 * @devfn: 대상 장치·함수 번호.
 * @return: true 면 접근해도 된다.
 *
 * PCI 코어는 열거할 때 있을 수 있는 모든 조합을 읽어 보므로, 애초에
 * 존재할 수 없거나 위험한 접근을 미리 거른다. 기준이 둘이다.
 *   - **루트 버스가 아니면 링크가 서 있어야 한다.** 그 자리의 상류 주석이
 *     "다운스트림 포트에 접근하기 전에 링크를 확인한다" 고 적는다.
 *   - **루트 버스에서는 devfn 이 0 이어야 한다.** 그 자리의 상류 주석대로
 *     루트 포트 하나에 장치 하나만 매달리기 때문이다. devfn 검사이므로
 *     슬롯뿐 아니라 함수까지 0 이어야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 *
 * 호출 체인:  nwl_pcie_map_bus() -> [이 함수] -> nwl_pcie_link_up()
 */
static bool nwl_pcie_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	struct nwl_pcie *pcie = bus->sysdata;	/* [한국어] 버스의 sysdata 에 이 드라이버 인스턴스가 들어 있다. probe 가 담아 둔 값이다 */

	/* Check link before accessing downstream ports */
	if (!pci_is_root_bus(bus)) {	/* [한국어] **루트 버스가 아니면 다운스트림 접근이다** — 바로 위 상류 주석이 그렇게 적는다 */
		if (!nwl_pcie_link_up(pcie))	/* [한국어] 링크가 서 있지 않으면 */
			return false;	/* [한국어] 접근을 거절한다 */
	} else if (devfn > 0)	/* [한국어] **루트 버스인데 devfn 이 0 이 아니면** */
		/* Only one device down on each root port */
		return false;	/* [한국어] 거절한다. 바로 위 상류 주석대로 루트 포트 하나에 장치 하나만 매달린다 */

	return true;	/* [한국어] 두 관문을 지났으면 접근해도 된다 */
}

/**
 * nwl_pcie_map_bus - Get configuration base
 *
 * @bus: Bus structure of current bus
 * @devfn: Device/function
 * @where: Offset from base
 *
 * Return: Base address of the configuration space needed to be
 *	   accessed.
 */
/* [한국어]
 * nwl_pcie_map_bus - config 공간 접근에 쓸 ECAM 주소를 만들어 준다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치·함수 번호.
 * @where: 읽거나 쓸 오프셋.
 * @return: 접근할 커널 가상 주소. 접근하면 안 되는 조합이면 NULL.
 *
 * 바로 위 상류 kernel-doc 이 "config base 를 얻는다" 고 적는다.
 *
 * **이 컨트롤러에는 ECAM 창이 있어 config 접근이 단순한 메모리 접근이다.**
 * 그래서 이 파일이 하는 일은 주소를 만들어 주는 것뿐이고, 실제 읽기·쓰기는
 * struct pci_ops 에 매단 pci_generic_config_read/write 가 처리한다.
 * PIO 레지스터 시퀀스로 config 를 만들어야 하는 컨트롤러(예: aardvark)와
 * 대비되는 부분이다.
 *
 * 주소는 ecam_base 에 PCIE_ECAM_OFFSET(버스, devfn, 오프셋)을 더한 값이다.
 * 그 매크로가 (버스 << 20 | devfn << 12 | 오프셋) 꼴의 ECAM 규격 배치를
 * 만든다.
 *
 * NULL 을 돌려주면 코어가 접근을 접고 "장치 없음" 으로 다룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거와 그 뒤의 모든 config 접근).
 *
 * 호출 체인:
 *   PCI 코어 -> pci_ops.map_bus -> [이 함수] -> nwl_pcie_valid_device()
 */
static void __iomem *nwl_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	struct nwl_pcie *pcie = bus->sysdata;	/* [한국어] 버스의 sysdata 에서 이 드라이버 인스턴스를 꺼낸다 */

	if (!nwl_pcie_valid_device(bus, devfn))	/* [한국어] 존재할 수 없거나 위험한 조합이면 */
		return NULL;	/* [한국어] NULL 을 돌려준다 — 코어가 "장치 없음" 으로 다룬다 */

	return pcie->ecam_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);	/* [한국어] **ECAM 창 시작에 (버스, devfn, 오프셋)으로 만든 오프셋을 더한다.** 실제 읽기·쓰기는 코어의 generic 함수가 이 주소로 한다 */
}

/* PCIe operations */
static struct pci_ops nwl_pcie_ops = {	/* [한국어] **PCI 코어가 config 접근에 쓸 동작 묶음** */
	.map_bus = nwl_pcie_map_bus,	/* [한국어] 주소 만들기만 자체 구현하고 */
	.read  = pci_generic_config_read,	/* [한국어] 읽기와 */
	.write = pci_generic_config_write,	/* [한국어] 쓰기는 코어의 generic 함수에 맡긴다. **ECAM 창이 있어 가능한 구조**이며, PIO 시퀀스로 config 를 만드는 컨트롤러와 대비된다 */
};

/* [한국어]
 * nwl_pcie_misc_handler - 오류·상태 인터럽트 열넷을 로그로 남긴다
 *
 * @irq:  리눅스 IRQ 번호. 쓰지 않는다.
 * @data: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 우리 것이면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * **이 파일의 인터럽트 넷 중 유일하게 보통의 핸들러**다(나머지 셋은 연쇄
 * 핸들러). IRQF_SHARED 로 등록되므로 선을 공유하는 다른 장치의 인터럽트를
 * 받을 수 있고, 그래서 첫 줄에서 우리 상태 비트가 하나라도 서 있는지
 * 확인하고 없으면 IRQ_NONE 을 돌려준다.
 *
 * 하는 일은 **알리는 것뿐**이다 — 상태 비트마다 대응하는 메시지를 남기고,
 * 마지막에 읽은 상태를 그대로 되써서 지운다(write-1-to-clear 방식).
 * 복구 동작은 없다.
 *
 * 메시지가 둘로 나뉜다. 오류성 열둘은 dev_err_ratelimited 로, 링크 대역폭
 * 관련 둘(LINK_AUTO_BWIDTH, LINK_BWIDTH)은 dev_info 로 남긴다. 오류 쪽에
 * 속도 제한이 걸린 이유는 같은 오류가 끊임없이 반복될 수 있기 때문이다.
 *
 * [관찰] MSGF_MISC_SR_MASKALL 에는 RXMSG_AVAIL 과 LINK_DOWN 도 들어 있어
 * 그 둘이 서면 IRQ_HANDLED 로 처리되지만, 대응하는 메시지 출력은 없다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * 호출 체인:
 *   GIC -> IRQ 코어 -> [이 함수] -> nwl_bridge_readl()/writel()
 */
static irqreturn_t nwl_pcie_misc_handler(int irq, void *data)
{
	struct nwl_pcie *pcie = data;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	u32 misc_stat;	/* [한국어] 읽은 상태에서 우리 비트만 남긴 값 */

	/* Checking for misc interrupts */
	misc_stat = nwl_bridge_readl(pcie, MSGF_MISC_STATUS) &	/* [한국어] **기타 인터럽트 상태를 읽어** */
				     MSGF_MISC_SR_MASKALL;	/* [한국어] 우리가 다루는 열다섯 비트만 남긴다 */
	if (!misc_stat)	/* [한국어] 하나도 없으면 우리 인터럽트가 아니다 — IRQF_SHARED 로 등록되어 선을 공유하기 때문이다 */
		return IRQ_NONE;	/* [한국어] 다른 핸들러가 처리하도록 넘긴다 */

	if (misc_stat & MSGF_MISC_SR_RXMSG_OVER)	/* [한국어] 수신 메시지 FIFO 가 넘쳤으면 */
		dev_err_ratelimited(dev, "Received Message FIFO Overflow\n");	/* [한국어] 알린다. 같은 오류가 반복될 수 있어 속도 제한이 걸린 판을 쓴다 */

	if (misc_stat & MSGF_MISC_SR_SLAVE_ERR)	/* [한국어] 슬레이브 오류이면 */
		dev_err_ratelimited(dev, "Slave error\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_MASTER_ERR)	/* [한국어] 마스터 오류이면 */
		dev_err_ratelimited(dev, "Master error\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_I_ADDR_ERR)	/* [한국어] 인그레스 주소 변환 오류이면 */
		dev_err_ratelimited(dev, "In Misc Ingress address translation error\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_E_ADDR_ERR)	/* [한국어] 이그레스 주소 변환 오류이면 */
		dev_err_ratelimited(dev, "In Misc Egress address translation error\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_FATAL_AER)	/* [한국어] AER 의 치명적 오류이면 */
		dev_err_ratelimited(dev, "Fatal Error in AER Capability\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_NON_FATAL_AER)	/* [한국어] AER 의 치명적이지 않은 오류이면 */
		dev_err_ratelimited(dev, "Non-Fatal Error in AER Capability\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_CORR_AER)	/* [한국어] AER 의 정정 가능 오류이면 */
		dev_err_ratelimited(dev, "Correctable Error in AER Capability\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_UR_DETECT)	/* [한국어] 지원하지 않는 요청을 검출했으면 */
		dev_err_ratelimited(dev, "Unsupported request Detected\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_NON_FATAL_DEV)	/* [한국어] 치명적이지 않은 오류를 검출했으면 */
		dev_err_ratelimited(dev, "Non-Fatal Error Detected\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_FATAL_DEV)	/* [한국어] 치명적 오류를 검출했으면 */
		dev_err_ratelimited(dev, "Fatal Error Detected\n");	/* [한국어] 알린다 */

	if (misc_stat & MSGF_MISC_SR_LINK_AUTO_BWIDTH)	/* [한국어] 링크 대역폭 자율 관리 상태가 섰으면 */
		dev_info(dev, "Link Autonomous Bandwidth Management Status bit set\n");	/* [한국어] **오류가 아니라 정보 수준으로** 알린다 */

	if (misc_stat & MSGF_MISC_SR_LINK_BWIDTH)	/* [한국어] 링크 대역폭 관리 상태가 섰으면 */
		dev_info(dev, "Link Bandwidth Management Status bit set\n");	/* [한국어] 역시 정보 수준으로 알린다 */

	/* Clear misc interrupt status */
	nwl_bridge_writel(pcie, misc_stat, MSGF_MISC_STATUS);	/* [한국어] **읽은 상태를 그대로 되써서 지운다**(1 을 써서 지우는 방식). 복구 동작은 없고 알리기만 한다 */

	return IRQ_HANDLED;	/* [한국어] 우리가 처리했다고 알린다 */
}

/* [한국어]
 * nwl_pcie_leg_handler - INTx 넷을 받아 각 가상 IRQ 로 나눠 보낸다
 *
 * @desc: 이 연쇄 핸들러가 달린 상위 인터럽트의 서술자. 여기서 상위
 *        irq_chip 과 handler_data(드라이버 인스턴스)를 꺼낸다.
 *
 * **연쇄(chained) 핸들러**다. 보통의 핸들러와 달리 반환값이 없고,
 * chained_irq_enter/exit 로 상위 인터럽트 컨트롤러의 처리를 감싼다.
 * 그 덕에 별도의 IRQ 번호를 소비하지 않고 상위 컨트롤러의 마스크·EOI
 * 절차를 그대로 따른다. 등록은 parse_dt 의
 * irq_set_chained_handler_and_data() 가 한다.
 *
 * 상태 레지스터가 **0 이 될 때까지 while 로 반복**한다 — 처리 도중에 새
 * INTx 가 어서트되면 다시 훑기 위해서다. 안쪽에서는 세워진 비트마다
 * intx_irq_domain 의 해당 hwirq 를 부른다.
 *
 * [관찰] 여기서 상태 비트를 지우지 않는다. INTx 는 레벨 신호라 장치가
 * 원인을 없애야 비트가 내려가며, 그 사이 언마스크가 늦으면 이 while 이
 * 계속 돌 수 있다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**, 상위 컨트롤러의 흐름 안이다.
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 -> [이 함수] -> chained_irq_enter()
 *     -> generic_handle_domain_irq() -> chained_irq_exit()
 */
static void nwl_pcie_leg_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] 이 연쇄 핸들러를 부른 상위 인터럽트 컨트롤러의 irq_chip */
	struct nwl_pcie *pcie;	/* [한국어] 드라이버 인스턴스 */
	unsigned long status;	/* [한국어] 읽은 INTx 상태 */
	u32 bit;	/* [한국어] 세워진 비트의 번호 */

	chained_irq_enter(chip, desc);	/* [한국어] **상위 컨트롤러의 처리를 연다.** 연쇄 핸들러의 규약이다 */
	pcie = irq_desc_get_handler_data(desc);	/* [한국어] 등록할 때 함께 넘긴 드라이버 인스턴스를 꺼낸다 */

	while ((status = nwl_bridge_readl(pcie, MSGF_LEG_STATUS) &	/* [한국어] **상태가 0 이 될 때까지 반복한다** — 처리 도중에 새 INTx 가 어서트되면 다시 훑기 위해서다 */
				MSGF_LEG_SR_MASKALL) != 0) {	/* [한국어] INTx 넷의 비트만 남긴다 */
		for_each_set_bit(bit, &status, PCI_NUM_INTX)	/* [한국어] 세워진 비트마다 */
			generic_handle_domain_irq(pcie->intx_irq_domain, bit);	/* [한국어] INTx 도메인의 해당 hwirq 를 부른다. **여기서 상태 비트를 지우지 않는다** — 레벨 신호라 장치가 원인을 없애야 내려간다 */
	}

	chained_irq_exit(chip, desc);	/* [한국어] 상위 컨트롤러의 처리를 닫는다 */
}

/* [한국어]
 * nwl_pcie_handle_msi_irq - MSI 상태 레지스터 하나를 훑어 벡터별로 나눠 보낸다
 *
 * @pcie:       이 드라이버 인스턴스.
 * @status_reg: 볼 상태 레지스터의 오프셋. MSGF_MSI_STATUS_LO 또는 _HI.
 *
 * MSI 상태·마스크 레지스터가 32비트짜리 둘로 나뉘어 있어 핸들러도 둘인데,
 * 하는 일이 같아 이 함수 하나로 모으고 레지스터 오프셋만 인자로 받는다.
 *
 * 상태가 0 이 될 때까지 반복하며, 세워진 비트마다 **먼저 그 비트를 지우고**
 * 해당 벡터의 가상 IRQ 를 부른다. 순서가 반대이면 핸들러가 도는 동안 온
 * 같은 벡터의 인터럽트를 지워 버릴 수 있다.
 *
 * [관찰] 넘겨받은 비트 번호를 그대로 hwirq 로 쓴다. 상위 레지스터를 보는
 * 호출자도 32 를 더하지 않으므로, **상위 32개 벡터가 0~31 번 hwirq 로
 * 전달된다.** 같은 계열의 pcie-xilinx-dma-pl.c 는 상위 핸들러에서 32 를
 * 더한다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**, 연쇄 핸들러 안이다.
 *
 * 호출 체인:
 *   nwl_pcie_msi_handler_low()/high() -> [이 함수]
 *     -> nwl_bridge_readl()/writel() -> generic_handle_domain_irq()
 */
static void nwl_pcie_handle_msi_irq(struct nwl_pcie *pcie, u32 status_reg)
{
	struct nwl_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	unsigned long status;	/* [한국어] 읽은 상태 */
	u32 bit;	/* [한국어] 세워진 비트의 번호 */

	while ((status = nwl_bridge_readl(pcie, status_reg)) != 0) {	/* [한국어] **상태가 0 이 될 때까지 반복한다** */
		for_each_set_bit(bit, &status, 32) {	/* [한국어] 32비트를 훑는다 — 이 레지스터 하나가 벡터 32개에 대응한다 */
			nwl_bridge_writel(pcie, 1 << bit, status_reg);	/* [한국어] **먼저 그 비트를 지운다** — 핸들러가 도는 동안 온 같은 벡터의 인터럽트를 잃지 않기 위해서다 */
			generic_handle_domain_irq(msi->dev_domain, bit);	/* [한국어] 그 다음 해당 벡터의 가상 IRQ 를 부른다. **비트 번호를 그대로 hwirq 로 쓰므로 상위 레지스터의 32개도 0~31 로 전달된다** */
		}
	}
}

/* [한국어]
 * nwl_pcie_msi_handler_high - 상위 32개 MSI 벡터를 받는 연쇄 핸들러
 *
 * @desc: 이 연쇄 핸들러가 달린 상위 인터럽트의 서술자.
 *
 * 장치 트리의 "msi1" 인터럽트에 붙는다. 하는 일은 상위 상태 레지스터
 * (MSGF_MSI_STATUS_HI)를 공통 처리 함수에 넘기는 것뿐이며, 앞뒤를
 * chained_irq_enter/exit 로 감싼다.
 *
 * MSI 벡터가 64개인데 상태 레지스터가 32비트라 인터럽트 선이 둘로 나뉜
 * 결과다 — 하드웨어가 이미 상위·하위를 갈라 준다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 -> [이 함수] -> nwl_pcie_handle_msi_irq()
 */
static void nwl_pcie_msi_handler_high(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] 상위 인터럽트 컨트롤러의 irq_chip */
	struct nwl_pcie *pcie = irq_desc_get_handler_data(desc);	/* [한국어] 등록할 때 함께 넘긴 드라이버 인스턴스 */

	chained_irq_enter(chip, desc);	/* [한국어] 상위 컨트롤러의 처리를 연다 */
	nwl_pcie_handle_msi_irq(pcie, MSGF_MSI_STATUS_HI);	/* [한국어] **상위 32벡터의 상태 레지스터**를 공통 처리 함수에 넘긴다 */
	chained_irq_exit(chip, desc);	/* [한국어] 상위 컨트롤러의 처리를 닫는다 */
}

/* [한국어]
 * nwl_pcie_msi_handler_low - 하위 32개 MSI 벡터를 받는 연쇄 핸들러
 *
 * @desc: 이 연쇄 핸들러가 달린 상위 인터럽트의 서술자.
 *
 * 장치 트리의 "msi0" 인터럽트에 붙는다. 상위 판과 구조가 같고 넘기는
 * 레지스터만 MSGF_MSI_STATUS_LO 로 다르다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 -> [이 함수] -> nwl_pcie_handle_msi_irq()
 */
static void nwl_pcie_msi_handler_low(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] 상위 인터럽트 컨트롤러의 irq_chip */
	struct nwl_pcie *pcie = irq_desc_get_handler_data(desc);	/* [한국어] 등록할 때 함께 넘긴 드라이버 인스턴스 */

	chained_irq_enter(chip, desc);	/* [한국어] 상위 컨트롤러의 처리를 연다 */
	nwl_pcie_handle_msi_irq(pcie, MSGF_MSI_STATUS_LO);	/* [한국어] **하위 32벡터의 상태 레지스터**를 넘긴다 — 상위 판과 이 한 줄만 다르다 */
	chained_irq_exit(chip, desc);	/* [한국어] 상위 컨트롤러의 처리를 닫는다 */
}

/* [한국어]
 * nwl_mask_intx_irq - INTx 하나를 마스크한다
 *
 * @data: 마스크할 인터럽트의 irq_data. hwirq 가 INTA~INTD 중 어느 것인지다.
 *
 * **MSGF_LEG_MASK 는 이름과 달리 활성화 레지스터로 쓰인다** — 이 함수가
 * 비트를 지우고(val & ~mask) 언마스크가 세운다. 즉 비트가 서 있으면 그
 * INTx 가 통과하고, 지워져 있으면 막힌다.
 *
 * 읽고-고치고-쓰는 세 단계라 raw_spinlock 으로 감싼다. raw 판이면서
 * irqsave 인 것은 **레벨 인터럽트라 핸들러가 처리 중에 마스크를 걸기
 * 때문**이다 — 인터럽트 컨텍스트에서도 불린다.
 *
 * irq_chip 의 irq_mask 와 irq_disable 두 자리에 모두 등록되어 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_mask/irq_disable -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> nwl_bridge_readl()/writel()
 */
static void nwl_mask_intx_irq(struct irq_data *data)
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 만질 비트 */
	u32 val;	/* [한국어] 읽고 고칠 레지스터 값 */

	mask = 1 << data->hwirq;	/* [한국어] hwirq(0~3)를 그대로 비트 자리로 쓴다 */
	raw_spin_lock_irqsave(&pcie->leg_mask_lock, flags);	/* [한국어] **읽고-고치고-쓰기를 원자적으로 한다.** 레벨 인터럽트라 핸들러가 처리 중에 마스크를 걸어 인터럽트 컨텍스트에서도 불린다 */
	val = nwl_bridge_readl(pcie, MSGF_LEG_MASK);	/* [한국어] 현재 값을 읽고 */
	nwl_bridge_writel(pcie, (val & (~mask)), MSGF_LEG_MASK);	/* [한국어] **그 비트를 지운다.** 이 레지스터는 이름과 달리 활성화 레지스터라 지우는 것이 막는 것이다 */
	raw_spin_unlock_irqrestore(&pcie->leg_mask_lock, flags);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * nwl_unmask_intx_irq - INTx 하나의 마스크를 푼다
 *
 * @data: 마스크를 풀 인터럽트의 irq_data.
 *
 * nwl_mask_intx_irq() 의 짝이며 비트를 세우는 것만 다르다. 같은
 * raw_spinlock 으로 읽고-고치고-쓰기를 보호한다.
 *
 * 레벨 인터럽트에서는 장치가 원인을 없앤 뒤 이 함수가 불려야 다음
 * 인터럽트를 받을 수 있으므로, 실질적으로 인터럽트 처리 주기를 닫는
 * 역할을 한다.
 *
 * irq_chip 의 irq_unmask 와 irq_enable 두 자리에 모두 등록되어 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_unmask/irq_enable -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> nwl_bridge_readl()/writel()
 */
static void nwl_unmask_intx_irq(struct irq_data *data)
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 만질 비트 */
	u32 val;	/* [한국어] 읽고 고칠 레지스터 값 */

	mask = 1 << data->hwirq;	/* [한국어] hwirq 를 그대로 비트 자리로 쓴다 */
	raw_spin_lock_irqsave(&pcie->leg_mask_lock, flags);	/* [한국어] 같은 락으로 감싼다 */
	val = nwl_bridge_readl(pcie, MSGF_LEG_MASK);	/* [한국어] 현재 값을 읽고 */
	nwl_bridge_writel(pcie, (val | mask), MSGF_LEG_MASK);	/* [한국어] **그 비트를 세운다.** 세우는 것이 통과시키는 것이다 */
	raw_spin_unlock_irqrestore(&pcie->leg_mask_lock, flags);	/* [한국어] 락을 푼다 */
}

static struct irq_chip nwl_intx_irq_chip = {	/* [한국어] **INTx 용 irq_chip. 인스턴스마다가 아니라 정적 구조체 하나를 공유한다** */
	.name = "nwl_pcie:legacy",	/* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_enable = nwl_unmask_intx_irq,	/* [한국어] 켜기와 */
	.irq_disable = nwl_mask_intx_irq,	/* [한국어] 끄기, */
	.irq_mask = nwl_mask_intx_irq,	/* [한국어] 마스크와 */
	.irq_unmask = nwl_unmask_intx_irq,	/* [한국어] 언마스크에 같은 함수 쌍을 매단다 — 이 하드웨어에서는 둘이 같은 동작이다 */
};

/* [한국어]
 * nwl_intx_map - INTx 가상 IRQ 하나를 이 컨트롤러에 잇는다
 *
 * @domain: INTx irq 도메인.
 * @irq:    커널이 배정한 가상 IRQ 번호.
 * @hwirq:  INTA~INTD 중 몇 번째인지(0~3).
 * @return: 항상 0.
 *
 * 장치 트리의 interrupt-map 을 따라 어떤 장치의 INTx 가 이 도메인의 어느
 * hwirq 로 오는지 정해지면, 커널이 그 가상 IRQ 를 만들면서 이 콜백을
 * 부른다.
 *
 * 셋을 한다 — irq_chip 과 handle_level_irq 를 매달고, chip_data 에
 * 드라이버 인스턴스를 담고, **IRQ_LEVEL 상태 플래그를 세운다.** 마지막
 * 것이 없으면 /proc/interrupts 표기와 일부 코어 처리가 에지로 다뤄진다.
 *
 * **irq_chip 이 정적 구조체(nwl_intx_irq_chip)다.** 같은 계열의
 * pci-aardvark.c 가 인스턴스마다 이름을 지어 넣는 것과 달리, 여기서는
 * 이름이 "nwl_pcie:legacy" 로 고정이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(가상 IRQ 생성 시).
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_domain_ops.map -> [이 함수]
 *     -> irq_set_chip_and_handler() -> irq_set_status_flags()
 */
static int nwl_intx_map(struct irq_domain *domain, unsigned int irq,
			irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &nwl_intx_irq_chip, handle_level_irq);	/* [한국어] irq_chip 과 레벨 핸들러를 매단다. 레벨 핸들러는 처리 전에 마스크를 걸고 처리 후 푼다 */
	irq_set_chip_data(irq, domain->host_data);	/* [한국어] 마스크·언마스크에서 꺼내 쓸 수 있게 인스턴스를 담아 둔다 */
	irq_set_status_flags(irq, IRQ_LEVEL);	/* [한국어] **INTx 가 레벨 트리거임을 알린다.** 이 표시가 없으면 코어와 /proc/interrupts 가 에지로 다룬다 */
	return 0;	/* [한국어] 매핑에 실패할 일이 없어 늘 성공이다 */
}

static const struct irq_domain_ops intx_domain_ops = {	/* [한국어] **INTx 도메인의 동작** */
	.map = nwl_intx_map,	/* [한국어] 가상 IRQ 를 만들 때 부를 함수 */
	.xlate = pci_irqd_intx_xlate,	/* [한국어] **PCI 전용 INTx 해석 헬퍼.** 장치 트리의 interrupt 속성을 INTA~INTD 로 푼다 */
};

#ifdef CONFIG_PCI_MSI	/* [한국어] **MSI 관련 정의는 이 옵션이 켜져 있을 때만 컴파일된다** */

/* [한국어] MSI 부모 도메인이 반드시 갖는 성질. 기본 도메인 동작과 기본 칩
 * 동작을 쓰고, 어피니티 설정을 지원하지 않는다고 알린다. 줄 잇기
 * 백슬래시로 여러 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define NWL_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	|\
				MSI_FLAG_USE_DEF_CHIP_OPS	|\
				MSI_FLAG_NO_AFFINITY)

/* [한국어] MSI 부모 도메인이 지원할 수 있는 성질. 일반 플래그 전부와 다중
 * MSI 다. **MSI_FLAG_PCI_MSIX 가 없어 이 컨트롤러는 MSI-X 를 알리지
 * 않는다** — MSI-X 를 쓰려는 장치는 MSI 로 떨어진다. 줄 잇기 백슬래시로
 * 두 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define NWL_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		|\
				 MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops nwl_msi_parent_ops = {	/* [한국어] **MSI 부모 도메인의 성질을 커널 공용 코드에 알린다.** 이것을 보고 공용 코드가 PCI MSI 자식 도메인을 만들어 준다 */
	.required_flags		= NWL_MSI_FLAGS_REQUIRED,	/* [한국어] 반드시 필요한 플래그 묶음 — 기본 도메인·칩 동작을 쓰고 어피니티 설정은 지원하지 않는다 */
	.supported_flags	= NWL_MSI_FLAGS_SUPPORTED,	/* [한국어] 지원 가능한 플래그 묶음 — 일반 플래그 전부와 다중 MSI. **MSI_FLAG_PCI_MSIX 가 없어 MSI-X 는 알리지 않는다** */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,	/* [한국어] PCI MSI 버스에 붙는 도메인이라고 알린다 */
	.prefix			= "nwl-",	/* [한국어] 도메인 이름에 붙일 접두사 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* [한국어] 자식 도메인 정보를 채우는 공용 헬퍼 */
};

#endif

/* [한국어]
 * nwl_compose_msi_msg - 장치에 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 인터럽트의 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채울 MSI 메시지.
 *
 * 엔드포인트가 인터럽트를 낼 때 **어느 주소에 어떤 값을 쓸지**를 정한다.
 * 그 주소와 값이 장치의 MSI capability 에 프로그래밍된다.
 *
 * 주소는 pcie->phys_pcie_reg_base — 장치 트리의 "pcireg" 자원의 물리
 * 주소다. 같은 값을 nwl_pcie_enable_msi() 가 I_MSII_BASE_LO/HI 에 심어
 * 두므로, 엔드포인트가 그 주소로 쓰면 브리지의 MSI 인그레스 창이 가로채
 * 인터럽트로 바꾼다. **두 자리가 같은 값을 쓰기로 약속한 구조**다.
 *
 * 데이터는 hwirq 즉 64개 중 몇 번째 벡터인지다.
 *
 * [관찰] irq_chip 에 마스크·언마스크가 없다. 이 컨트롤러의 MSI 마스크
 * 레지스터(MSGF_MSI_MASK_LO/HI)는 bridge 초기화 때 전부 열어 두고 이후
 * 개별 벡터를 막지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_chip.irq_compose_msi_msg -> [이 함수]
 */
static void nwl_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	phys_addr_t msi_addr = pcie->phys_pcie_reg_base;	/* [한국어] **MSI 목적지는 pcireg 창의 물리 주소다.** nwl_pcie_enable_msi() 가 같은 값을 I_MSII_BASE_LO/HI 에 심어 두어, 그 주소로 오는 쓰기를 브리지가 가로챈다 */

	msg->address_lo = lower_32_bits(msi_addr);	/* [한국어] 주소 하위 워드 */
	msg->address_hi = upper_32_bits(msi_addr);	/* [한국어] 상위 워드 */
	msg->data = data->hwirq;	/* [한국어] **데이터는 벡터 번호다.** 인터럽트가 왔을 때 어느 벡터인지 되짚는 근거가 된다 */
}

static struct irq_chip nwl_irq_chip = {	/* [한국어] **MSI 벡터에 매달릴 irq_chip** */
	.name = "Xilinx MSI",	/* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_compose_msi_msg = nwl_compose_msi_msg,	/* [한국어] 장치에 알려 줄 주소와 데이터를 만든다. **마스크·언마스크가 없다** — 개별 벡터를 막는 코드가 이 파일에 없다 */
};

/* [한국어]
 * nwl_irq_domain_alloc - MSI 벡터를 요청 개수만큼 잡아 준다
 *
 * @domain:  MSI 부모 도메인.
 * @virq:    커널이 배정한 가상 IRQ 번호의 시작.
 * @nr_irqs: 요청한 벡터 수.
 * @args:    쓰지 않는다.
 * @return: 0 이면 성공, 빈 자리가 없으면 -ENOSPC.
 *
 * **벡터가 64개(INT_PCI_MSI_NR = 2 x 32)뿐이라 비트맵으로 관리한다.**
 * 그 비트맵은 struct nwl_msi 안에 DECLARE_BITMAP 으로 정적으로 잡혀 있다.
 *
 * bitmap_find_free_region() 이 요청 개수를 2의 거듭제곱으로 올린 크기의
 * 연속 구간을 찾는다 — 다중 MSI 는 연속이고 2의 거듭제곱 개수여야 하며
 * 시작도 그 크기에 정렬되어야 한다는 규격 제약 때문이며,
 * get_count_order() 가 그 올림을 한다.
 *
 * 찾은 구간의 각 벡터에 hwirq 를 잇고 irq_chip 과 handle_simple_irq 를
 * 매단다. MSI 는 에지 성격이라 레벨 처리가 필요 없다.
 *
 * 동기화: 비트맵 조작을 msi->lock 뮤텍스로 감싼다. 잠들 수 있는 락을 쓸
 * 수 있는 것은 이 경로가 프로세스 컨텍스트에서만 불리기 때문이다.
 * **실패 경로에서도 잠금을 반드시 푼다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.alloc -> [이 함수]
 *     -> bitmap_find_free_region() -> irq_domain_set_info()
 */
static int nwl_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *args)
{
	struct nwl_pcie *pcie = domain->host_data;	/* [한국어] 도메인의 host_data 에 이 드라이버 인스턴스가 들어 있다 */
	struct nwl_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	int bit;	/* [한국어] 잡은 구간의 시작 벡터 */
	int i;	/* [한국어] 순회 인덱스 */

	mutex_lock(&msi->lock);	/* [한국어] **비트맵을 뮤텍스로 감싼다.** 이 경로는 프로세스 컨텍스트에서만 불려 잠들 수 있다 */
	bit = bitmap_find_free_region(msi->bitmap, INT_PCI_MSI_NR,	/* [한국어] **연속된 빈 구간을 찾는다** */
				      get_count_order(nr_irqs));	/* [한국어] **요청 개수를 2의 거듭제곱으로 올린 크기**로 찾는다 — 다중 MSI 는 연속이고 2의 거듭제곱 개수여야 하며 시작도 그 크기에 정렬되어야 한다 */
	if (bit < 0) {	/* [한국어] 빈 구간이 없으면 */
		mutex_unlock(&msi->lock);	/* [한국어] **락을 먼저 풀고** */
		return -ENOSPC;	/* [한국어] 공간 없음으로 알린다 */
	}

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 잡은 구간의 벡터마다 */
		irq_domain_set_info(domain, virq + i, bit + i, &nwl_irq_chip,	/* [한국어] 가상 IRQ 와 hwirq 를 잇고 irq_chip 을 매단다 */
				    domain->host_data, handle_simple_irq,	/* [한국어] chip_data 로 드라이버 인스턴스를, 핸들러로 handle_simple_irq 를 준다 — MSI 는 에지 성격이라 레벨 처리가 필요 없다 */
				    NULL, NULL);	/* [한국어] 나머지 두 인자는 쓰지 않는다 */
	}
	mutex_unlock(&msi->lock);	/* [한국어] 락을 푼다 */
	return 0;	/* [한국어] 구간을 잡았으면 성공 */
}

/* [한국어]
 * nwl_irq_domain_free - 잡아 두었던 MSI 벡터를 놓는다
 *
 * @domain:  MSI 부모 도메인.
 * @virq:    놓을 가상 IRQ 번호의 시작.
 * @nr_irqs: 놓을 벡터 수.
 *
 * alloc 의 짝이다. irq_data 에서 hwirq 를 되찾아 그 자리부터
 * get_count_order(nr_irqs) 크기만큼을 비트맵에서 놓는다 — 잡을 때와 같은
 * 크기 계산을 써야 짝이 맞는다.
 *
 * [관찰] 인스턴스를 domain->host_data 가 아니라 **irq_data 의 chip_data**
 * 에서 얻는다. alloc 쪽이 두 자리에 같은 값을 넣어 두어 성립한다.
 *
 * 동기화: alloc 과 같은 뮤텍스로 감싼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 제거 또는 MSI 해제 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.free -> [이 함수]
 *     -> irq_domain_get_irq_data() -> bitmap_release_region()
 */
static void nwl_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);	/* [한국어] **해제 전에 irq_data 에서 hwirq 를 되찾는다** — 그것이 비트맵에서의 위치다 */
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] **alloc 이 chip_data 에도 넣어 둔 인스턴스를 꺼낸다**(host_data 가 아니다) */
	struct nwl_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */

	mutex_lock(&msi->lock);	/* [한국어] 할당 때와 같은 뮤텍스로 감싼다 */
	bitmap_release_region(msi->bitmap, data->hwirq,	/* [한국어] 그 자리부터 */
			      get_count_order(nr_irqs));	/* [한국어] **잡을 때와 같은 크기 계산으로 놓는다** — 그래야 짝이 맞는다 */
	mutex_unlock(&msi->lock);	/* [한국어] 락을 푼다 */
}

static const struct irq_domain_ops dev_msi_domain_ops = {	/* [한국어] **MSI 부모 도메인의 동작.** 마스크·언마스크는 irq_chip 쪽에 없고 여기에는 할당과 해제만 있다 */
	.alloc  = nwl_irq_domain_alloc,	/* [한국어] 벡터 잡기 */
	.free   = nwl_irq_domain_free,	/* [한국어] 벡터 놓기 */
};

/* [한국어]
 * nwl_pcie_init_msi_irq_domain - MSI 인터럽트 도메인을 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공(CONFIG_PCI_MSI 가 꺼져 있는 경우 포함), 도메인을 못
 *          만들면 -ENOMEM.
 *
 * **함수 몸통 전체가 #ifdef CONFIG_PCI_MSI 안에 있다.** 그 옵션이 꺼져
 * 있으면 아무 일도 하지 않고 0 을 돌려주므로, 호출자는 조건부 컴파일을
 * 신경 쓰지 않아도 된다.
 *
 * 크기가 INT_PCI_MSI_NR(64)로 고정이다 — 하드웨어의 MSI 상태·마스크
 * 레지스터가 32비트짜리 둘이라 그 이상은 다룰 수 없다.
 *
 * msi_create_parent_irq_domain() 은 이 도메인을 **부모**로 두고, PCI MSI
 * 쪽 자식 도메인은 커널 공용 코드가 nwl_msi_parent_ops 의 플래그에 따라
 * 만들어 준다. 그 플래그에 **MSI_FLAG_PCI_MSIX 가 없어 MSI-X 는 알리지
 * 않고**, MSI_FLAG_NO_AFFINITY 로 어피니티 설정도 지원하지 않는다고
 * 알린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   nwl_pcie_init_irq_domain() -> [이 함수] -> msi_create_parent_irq_domain()
 */
static int nwl_pcie_init_msi_irq_domain(struct nwl_pcie *pcie)
{
#ifdef CONFIG_PCI_MSI	/* [한국어] **함수 몸통 전체가 이 조건부 안에 있다.** 꺼져 있으면 아무 일도 하지 않고 0 을 돌려준다 */
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치이자 fwnode 의 출처 */
	struct nwl_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	struct irq_domain_info info = {	/* [한국어] 만들 도메인의 명세 */
		.fwnode		= dev_fwnode(dev),	/* [한국어] 장치 트리 노드에서 온 fwnode */
		.ops		= &dev_msi_domain_ops,	/* [한국어] 할당·해제 동작 */
		.host_data	= pcie,	/* [한국어] 콜백에서 꺼내 쓸 인스턴스 */
		.size		= INT_PCI_MSI_NR,	/* [한국어] **벡터 64개. 상태 레지스터가 32비트짜리 둘인 데서 온 상한이다** */
	};

	msi->dev_domain  = msi_create_parent_irq_domain(&info, &nwl_msi_parent_ops);	/* [한국어] **이 도메인을 부모로 두고 PCI MSI 자식 도메인까지 만들어 달라고 한다** */
	if (!msi->dev_domain) {	/* [한국어] 못 만들면 */
		dev_err(dev, "failed to create dev IRQ domain\n");	/* [한국어] 그 사실을 알리고 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */
	}
#endif
	return 0;	/* [한국어] MSI 가 꺼져 있거나 도메인을 만들었으면 성공 */
}

/* [한국어]
 * nwl_pcie_phy_power_off - PHY 하나의 전원을 내리고 실패하면 알린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @i:    몇 번째 PHY 인지(0~3).
 *
 * phy_power_off() 를 부르고 실패하면 몇 번 PHY 가 왜 실패했는지 남긴다.
 * **오류를 호출자에게 알리지 않는다** — 반환값이 없다. 정리 경로에서만
 * 쓰이므로 중간에 멈출 이유가 없기 때문이다.
 *
 * pcie->phy[i] 가 NULL 일 수 있는데(장치 트리가 넷을 다 주지 않은 경우)
 * phy_power_off(NULL) 이 안전하다는 데 기댄다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   nwl_pcie_phy_enable() 실패 경로 / nwl_pcie_phy_disable()
 *     -> [이 함수] -> phy_power_off()
 */
static void nwl_pcie_phy_power_off(struct nwl_pcie *pcie, int i)
{
	int err = phy_power_off(pcie->phy[i]);	/* [한국어] PHY 전원을 내리고 결과를 받는다. **phy[i] 가 NULL 이어도 안전하다** */

	if (err)	/* [한국어] 실패하면 */
		dev_err(pcie->dev, "could not power off phy %d (err=%d)\n", i,	/* [한국어] 몇 번 PHY 가 왜 실패했는지 남긴다 */
			err);	/* [한국어] 오류 코드 */
}

/* [한국어]
 * nwl_pcie_phy_exit - PHY 하나를 해제하고 실패하면 알린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @i:    몇 번째 PHY 인지(0~3).
 *
 * nwl_pcie_phy_power_off() 와 같은 구조로 phy_exit() 를 감싼다. 역시
 * 반환값이 없어 실패를 알리지 않는다.
 *
 * 전원 내리기와 해제를 두 함수로 나눈 이유는 **되돌릴 깊이가 다른 경우가
 * 있기 때문**이다 — nwl_pcie_phy_enable() 은 phy_power_on() 이 실패했을 때
 * 그 PHY 에 대해서는 exit 만 부르고(전원이 안 들어와 있으므로), 앞서 성공한
 * PHY 들에 대해서는 둘 다 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   nwl_pcie_phy_enable() 실패 경로 / nwl_pcie_phy_disable()
 *     -> [이 함수] -> phy_exit()
 */
static void nwl_pcie_phy_exit(struct nwl_pcie *pcie, int i)
{
	int err = phy_exit(pcie->phy[i]);	/* [한국어] PHY 를 해제하고 결과를 받는다 */

	if (err)	/* [한국어] 실패하면 */
		dev_err(pcie->dev, "could not exit phy %d (err=%d)\n", i, err);	/* [한국어] 몇 번 PHY 가 왜 실패했는지 남긴다. **호출자에게는 알리지 않는다** — 반환값이 없다 */
}

/* [한국어]
 * nwl_pcie_phy_enable - PHY 를 최대 넷까지 순서대로 켠다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * PHY 배열 넷을 돌며 각각 phy_init -> phy_power_on 순으로 켠다.
 * parse_dt 가 장치 트리에 있는 만큼만 채우고 나머지를 NULL 로 두었으므로,
 * 없는 자리에서는 phy_init(NULL) 등이 조용히 성공한다.
 *
 * **되돌리기가 두 겹이다.**
 *   - phy_power_on() 이 실패하면 **그 PHY 에 대해서만 먼저 exit** 를 부른다.
 *     전원이 안 들어와 있으므로 power_off 는 부르지 않는다.
 *   - 그 다음 err 라벨의 while (i--) 가 **앞서 성공한 PHY 들**을 역순으로
 *     power_off + exit 한다.
 *   - phy_init() 이 실패한 경우에는 그 PHY 에 대해 아무것도 되돌리지 않고
 *     곧바로 err 로 간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   nwl_pcie_probe() -> [이 함수] -> phy_init() -> phy_power_on()
 *     -> nwl_pcie_phy_exit() / nwl_pcie_phy_power_off()
 */
static int nwl_pcie_phy_enable(struct nwl_pcie *pcie)
{
	int i, ret;	/* [한국어] i 는 순회 인덱스이자 되돌릴 지점, ret 은 하위 호출 결과 */

	for (i = 0; i < ARRAY_SIZE(pcie->phy); i++) {	/* [한국어] **PHY 배열 넷을 돈다.** 장치 트리가 덜 준 자리는 NULL 이고 그 경우 하위 호출이 조용히 성공한다 */
		ret = phy_init(pcie->phy[i]);	/* [한국어] 먼저 초기화하고 */
		if (ret)	/* [한국어] 실패하면 */
			goto err;	/* [한국어] **그 PHY 에 대해서는 되돌릴 것이 없으므로** 곧바로 정리 라벨로 간다 */

		ret = phy_power_on(pcie->phy[i]);	/* [한국어] 그 다음 전원을 넣는다 */
		if (ret) {	/* [한국어] 실패하면 */
			nwl_pcie_phy_exit(pcie, i);	/* [한국어] **그 PHY 만 해제한다** — 전원이 안 들어왔으므로 power_off 는 부르지 않는다 */
			goto err;	/* [한국어] 그리고 앞서 성공한 것들을 되돌리는 라벨로 간다 */
		}
	}

	return 0;	/* [한국어] 넷을 모두 켰으면 성공 */

err:	/* [한국어] **되돌리기 경로** — 앞서 성공한 PHY 들을 역순으로 정리한다 */
	while (i--) {	/* [한국어] i 를 하나 줄이며 0 까지 내려간다. 실패한 자리 자신은 위에서 이미 처리했다 */
		nwl_pcie_phy_power_off(pcie, i);	/* [한국어] 전원을 내리고 */
		nwl_pcie_phy_exit(pcie, i);	/* [한국어] 해제한다 — 켤 때(init -> power_on)와 정확히 반대 순서다 */
	}

	return ret;	/* [한국어] 담아 둔 오류 코드를 돌려준다 */
}

/* [한국어]
 * nwl_pcie_phy_disable - PHY 넷을 역순으로 끈다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * nwl_pcie_phy_enable() 의 짝이다. **역순으로 도는 것**이 특징으로,
 * for (i = ARRAY_SIZE(...); i--;) 가 3, 2, 1, 0 순서를 만든다.
 *
 * 각 PHY 에 대해 power_off 다음 exit 를 부른다 — 켤 때의 init -> power_on
 * 과 정확히 반대다.
 *
 * 부르는 곳이 둘이다 — probe 의 err_phy 경로와 remove.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   nwl_pcie_probe() 실패 경로 / nwl_pcie_remove() -> [이 함수]
 *     -> nwl_pcie_phy_power_off() -> nwl_pcie_phy_exit()
 */
static void nwl_pcie_phy_disable(struct nwl_pcie *pcie)
{
	int i;	/* [한국어] 순회 인덱스 */

	for (i = ARRAY_SIZE(pcie->phy); i--;) {	/* [한국어] **역순으로 돈다** — i 가 4 에서 시작해 3, 2, 1, 0 순서로 몸통을 돈다 */
		nwl_pcie_phy_power_off(pcie, i);	/* [한국어] 전원을 내리고 */
		nwl_pcie_phy_exit(pcie, i);	/* [한국어] 해제한다. 켤 때(init -> power_on)와 정확히 반대다 */
	}
}

/* [한국어]
 * nwl_pcie_init_irq_domain - INTx 도메인을 만들고 MSI 도메인까지 이어서 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 장치 트리에 인터럽트 컨트롤러 자식 노드가 없으면
 *          -EINVAL, 도메인을 못 만들면 -ENOMEM.
 *
 * INTA~INTD 넷을 장치 드라이버에게 중계하는 도메인을 만든다. **장치
 * 트리의 자식 노드가 필요하다** — 엔드포인트의 노드가 interrupt-parent 로
 * 그 노드를 가리켜야 어느 도메인으로 갈지 정해지기 때문이다. 그래서
 * of_get_next_child() 로 첫 자식 노드를 찾고, 없으면 실패한다.
 *
 * 찾은 노드의 참조는 도메인을 만든 **직후 곧바로** 놓는다 — 도메인이
 * fwnode 를 자기 방식으로 붙잡기 때문이다.
 *
 * 그 뒤 INTx 마스크용 raw_spinlock 을 초기화하고 MSI 도메인 생성으로
 * 이어진다.
 *
 * [관찰] nwl_pcie_init_msi_irq_domain() 의 반환값을 확인하지 않는다.
 * MSI 도메인을 못 만들어도 이 함수는 0 을 돌려주고 probe 가 계속된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   nwl_pcie_probe() -> [이 함수] -> of_get_next_child()
 *     -> irq_domain_create_linear() -> nwl_pcie_init_msi_irq_domain()
 */
static int nwl_pcie_init_irq_domain(struct nwl_pcie *pcie)
{
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct device_node *node = dev->of_node;	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *intc_node;	/* [한국어] 그 아래의 인터럽트 컨트롤러 자식 노드 */

	intc_node = of_get_next_child(node, NULL);	/* [한국어] **첫 자식 노드를 찾는다** — 엔드포인트가 interrupt-parent 로 그 노드를 가리켜야 이 도메인으로 온다 */
	if (!intc_node) {	/* [한국어] 없으면 장치 트리가 이 드라이버가 기대하는 모양이 아니다 */
		dev_err(dev, "No legacy intc node found\n");	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	pcie->intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(intc_node), PCI_NUM_INTX,	/* [한국어] **INTx 도메인을 만든다.** 찾은 자식 노드에 붙이고 크기는 PCI_NUM_INTX(4)다 */
							 &intx_domain_ops, pcie);	/* [한국어] 동작 묶음과 인스턴스를 준다 */
	of_node_put(intc_node);	/* [한국어] **도메인을 만든 직후 곧바로 노드 참조를 놓는다** — 도메인이 fwnode 를 자기 방식으로 붙잡기 때문이다 */
	if (!pcie->intx_irq_domain) {	/* [한국어] 못 만들었으면 */
		dev_err(dev, "failed to create IRQ domain\n");	/* [한국어] 그 사실을 알리고 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */
	}

	raw_spin_lock_init(&pcie->leg_mask_lock);	/* [한국어] **INTx 마스크 레지스터를 지킬 스핀락을 초기화한다** */
	nwl_pcie_init_msi_irq_domain(pcie);	/* [한국어] 이어서 MSI 도메인도 만든다. **반환값을 확인하지 않아** 실패해도 probe 가 계속된다 */
	return 0;	/* [한국어] INTx 도메인을 만들었으면 성공 */
}

/* [한국어]
 * nwl_pcie_enable_msi - MSI 인그레스 창과 두 상태 레지스터를 켠다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 인터럽트 번호를 못 얻으면 -EINVAL, 하드웨어에
 *          MSI 인그레스 창이 없으면 -EIO.
 *
 * 하는 일이 넷이다.
 *
 *   1. **연쇄 핸들러 둘을 건다.** 장치 트리의 "msi1" 과 "msi0" 을 이름으로
 *      얻어 각각 상위·하위 핸들러를 붙인다. 보통의 devm_request_irq 가
 *      아니라 irq_set_chained_handler_and_data() 이므로, 별도의 IRQ 번호를
 *      소비하지 않고 상위 컨트롤러의 흐름 안에서 불린다.
 *   2. **하드웨어에 MSI 인그레스 창이 있는지 확인한다.**
 *      I_MSII_CAPABILITIES 의 PRESENT 비트를 보고 없으면 -EIO 로 끝낸다.
 *   3. **그 창을 켜고 주소를 심는다.** MSII_ENABLE 과 MSII_STATUS_ENABLE 을
 *      차례로 세우고, pcireg 창의 물리 주소를 I_MSII_BASE_LO/HI 에 쓴다.
 *      그 주소가 nwl_compose_msi_msg() 가 장치에 알려 줄 값과 같다.
 *   4. **두 상태 레지스터를 초기화한다.** 상위·하위 각각에 대해 상류
 *      주석대로 "끄고, 남은 것을 지우고, 켠다" 순서를 밟는다 — 마스크를
 *      0 으로 써서 막고, 상태를 되써서 지우고, 마스크를 전체로 열어 준다.
 *      **MSGF_MSI_MASK_* 도 이름과 달리 활성화 레지스터**라 전체 비트를
 *      쓰는 것이 여는 것이다.
 *
 * 시작 부분의 mutex_init() 이 MSI 비트맵을 지킬 뮤텍스를 초기화한다.
 *
 * [관찰] "setup AFI/FPCI range" 라는 상류 주석은 이 파일이 바탕으로 삼은
 * pci-tegra.c 의 용어다. 이 하드웨어에는 AFI/FPCI 라는 블록이 없고, 그
 * 자리는 I_MSII_BASE_LO/HI 를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   nwl_pcie_probe() -> [이 함수] -> platform_get_irq_byname()
 *     -> irq_set_chained_handler_and_data() -> nwl_bridge_readl()/writel()
 */
static int nwl_pcie_enable_msi(struct nwl_pcie *pcie)
{
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct platform_device *pdev = to_platform_device(dev);	/* [한국어] 인터럽트 번호를 이름으로 얻기 위한 플랫폼 장치 */
	struct nwl_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	unsigned long base;	/* [한국어] MSI 목적지로 심을 물리 주소 */
	int ret;	/* [한국어] 하위 호출 결과 */

	mutex_init(&msi->lock);	/* [한국어] **MSI 비트맵을 지킬 뮤텍스를 초기화한다** */

	/* Get msi_1 IRQ number */
	msi->irq_msi1 = platform_get_irq_byname(pdev, "msi1");	/* [한국어] **상위 32벡터를 나르는 인터럽트 번호를 이름으로 얻는다** */
	if (msi->irq_msi1 < 0)	/* [한국어] 없으면 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */

	irq_set_chained_handler_and_data(msi->irq_msi1,	/* [한국어] **연쇄 핸들러로 건다** — 보통의 request_irq 가 아니라 상위 컨트롤러의 흐름 안에서 불리게 한다 */
					 nwl_pcie_msi_handler_high, pcie);	/* [한국어] 상위 벡터 핸들러와 함께 넘길 인스턴스 */

	/* Get msi_0 IRQ number */
	msi->irq_msi0 = platform_get_irq_byname(pdev, "msi0");	/* [한국어] 하위 32벡터를 나르는 인터럽트 번호도 얻는다 */
	if (msi->irq_msi0 < 0)	/* [한국어] 없으면 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */

	irq_set_chained_handler_and_data(msi->irq_msi0,	/* [한국어] 같은 방식으로 연쇄 핸들러를 건다 */
					 nwl_pcie_msi_handler_low, pcie);	/* [한국어] 하위 벡터 핸들러와 인스턴스 */

	/* Check for msii_present bit */
	ret = nwl_bridge_readl(pcie, I_MSII_CAPABILITIES) & MSII_PRESENT;	/* [한국어] **하드웨어에 MSI 인그레스 창이 있는지 확인한다** */
	if (!ret) {	/* [한국어] 없으면 */
		dev_err(dev, "MSI not present\n");	/* [한국어] 그 사실을 알리고 */
		return -EIO;	/* [한국어] 입출력 오류로 돌아간다 */
	}

	/* Enable MSII */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, I_MSII_CONTROL) |	/* [한국어] **그 창을 켠다** */
			  MSII_ENABLE, I_MSII_CONTROL);	/* [한국어] 활성화 비트를 얹어 되쓴다 */

	/* Enable MSII status */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, I_MSII_CONTROL) |	/* [한국어] **그 창의 상태 보고도 켠다** */
			  MSII_STATUS_ENABLE, I_MSII_CONTROL);	/* [한국어] 상태 활성화 비트를 얹어 되쓴다 */

	/* setup AFI/FPCI range */
	base = pcie->phys_pcie_reg_base;	/* [한국어] **MSI 목적지로 pcireg 창의 물리 주소를 쓴다.** nwl_compose_msi_msg() 가 장치에 알려 줄 값과 같다 */
	nwl_bridge_writel(pcie, lower_32_bits(base), I_MSII_BASE_LO);	/* [한국어] 하위 워드 */
	nwl_bridge_writel(pcie, upper_32_bits(base), I_MSII_BASE_HI);	/* [한국어] 상위 워드 */

	/*
	 * For high range MSI interrupts: disable, clear any pending,
	 * and enable
	 */
	nwl_bridge_writel(pcie, 0, MSGF_MSI_MASK_HI);	/* [한국어] **상위 벡터: 먼저 전부 막는다.** 이 레지스터는 이름과 달리 활성화 레지스터라 0 을 쓰는 것이 막는 것이다 */

	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie,  MSGF_MSI_STATUS_HI) &	/* [한국어] 현재 상태를 읽어 */
			  MSGF_MSI_SR_HI_MASK, MSGF_MSI_STATUS_HI);	/* [한국어] 그대로 되써서 남아 있던 것을 지운다 */

	nwl_bridge_writel(pcie, MSGF_MSI_SR_HI_MASK, MSGF_MSI_MASK_HI);	/* [한국어] **전체 비트를 써서 32벡터를 모두 연다** */

	/*
	 * For low range MSI interrupts: disable, clear any pending,
	 * and enable
	 */
	nwl_bridge_writel(pcie, 0, MSGF_MSI_MASK_LO);	/* [한국어] **하위 벡터: 같은 순서를 밟는다.** 먼저 전부 막고 */

	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_MSI_STATUS_LO) &	/* [한국어] 현재 상태를 읽어 */
			  MSGF_MSI_SR_LO_MASK, MSGF_MSI_STATUS_LO);	/* [한국어] 되써서 지우고 */

	nwl_bridge_writel(pcie, MSGF_MSI_SR_LO_MASK, MSGF_MSI_MASK_LO);	/* [한국어] 전체 비트를 써서 모두 연다 */

	return 0;	/* [한국어] 여기까지 왔으면 성공 */
}

/* [한국어]
 * nwl_pcie_bridge_init - 브리지 레지스터를 초기 상태로 만들고 링크를 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. BREG 나 ECAM 이 없으면 0(그 PRESENT 값), 링크가
 *          안 서면 -ETIMEDOUT, 인터럽트를 못 얻거나 못 걸면 그 코드.
 *
 * 이 파일의 하드웨어 초기화 본체다. 순서대로 이렇게 한다.
 *
 *   1. **BREG 가 있는지 확인하고 켠다.** CAPABILITIES 의 PRESENT 비트를
 *      보고, breg 창의 물리 주소를 E_BREG_BASE_LO/HI 에 심은 뒤
 *      E_BREG_CONTROL 에 ENABLE 을 쓴다.
 *   2. **DMA 채널 레지스터를 끈다**(BRCFG_PCIE_RX0 에 CFG_DMA_REG_BAR).
 *      이 드라이버는 브리지의 DMA 기능을 쓰지 않는다.
 *   3. **인그레스 subtractive 디코드 변환을 켠다**(I_ISUB_CONTROL).
 *   4. **메시지 필터를 연다** — PM·INT·ERR 메시지를 브리지가 전달하도록
 *      한다. 그래야 misc 핸들러가 볼 상태 비트가 생긴다.
 *   5. 장치 트리가 dma-coherent 를 밝혔으면 BRCFG_PCIE_RX1 에
 *      CFG_PCIE_CACHE 를 세운다. 그 자리의 상류 주석대로 PCIe DMA 트래픽을
 *      CCI 경로로 보내는 설정이다.
 *   6. **링크를 기다린다.** 실패하면 그 자리에서 접는다.
 *   7. **ECAM 이 있는지 확인하고 켠다.** CR_ENABLE 을 세우고, 크기 필드에
 *      NWL_ECAM_MAX_SIZE 를 넣고, ecam 창의 물리 주소를 심는다.
 *      **이 단계가 끝나야 map_bus 가 만든 주소가 실제로 동작한다.**
 *   8. 링크 상태를 로그로 남긴다.
 *   9. **misc 인터럽트를 건다.** 이쪽만 devm_request_irq(IRQF_SHARED)다.
 *  10. **misc 와 INTx 인터럽트를 각각 "끄고 지우고 켠다".** 두 MASK
 *      레지스터 모두 활성화 레지스터로 쓰이므로, ~MASKALL 이 끄는 것이고
 *      MASKALL 이 켜는 것이다.
 *  11. 마지막으로 브리지 설정 인터럽트를 켠다(BRCFG_INTERRUPT).
 *
 * [관찰] 1번과 7번의 실패 반환값이 `breg_val`/`ecam_val` 인데, 그 자리에
 * 도달하는 조건이 그 값이 0 일 때이므로 **0(성공)을 돌려준다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 6번에서 최대 1초 가까이 잠든다.
 *
 * 호출 체인:
 *   nwl_pcie_probe() -> [이 함수] -> nwl_bridge_readl()/writel()
 *     -> nwl_wait_for_link() -> devm_request_irq()
 */
static int nwl_pcie_bridge_init(struct nwl_pcie *pcie)
{
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct platform_device *pdev = to_platform_device(dev);	/* [한국어] 인터럽트 번호를 이름으로 얻기 위한 플랫폼 장치 */
	u32 breg_val, ecam_val;	/* [한국어] PRESENT 비트 검사와 ECAM 제어 값을 담을 자리 */
	int err;	/* [한국어] 하위 호출 결과 */

	breg_val = nwl_bridge_readl(pcie, E_BREG_CAPABILITIES) & BREG_PRESENT;	/* [한국어] **브리지 이그레스 창이 하드웨어에 있는지 확인한다** */
	if (!breg_val) {	/* [한국어] 없으면 */
		dev_err(dev, "BREG is not present\n");	/* [한국어] 그 사실을 알리고 */
		return breg_val;	/* [한국어] 그 값을 돌려준다. **여기 도달하는 조건이 그 값이 0 일 때이므로 결과적으로 0(성공)을 돌려준다** */
	}

	/* Write bridge_off to breg base */
	nwl_bridge_writel(pcie, lower_32_bits(pcie->phys_breg_base),	/* [한국어] **breg 창의 물리 주소를 브리지에 알린다** */
			  E_BREG_BASE_LO);	/* [한국어] 하위 워드 */
	nwl_bridge_writel(pcie, upper_32_bits(pcie->phys_breg_base),	/* [한국어] 상위 워드에 */
			  E_BREG_BASE_HI);	/* [한국어] 각각 심는다 */

	/* Enable BREG */
	nwl_bridge_writel(pcie, ~BREG_ENABLE_FORCE & BREG_ENABLE,	/* [한국어] **그 창을 켠다.** 강제 활성화 비트는 빼고 일반 활성화만 쓴다 */
			  E_BREG_CONTROL);	/* [한국어] 제어 레지스터에 */

	/* Disable DMA channel registers */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_PCIE_RX0) |	/* [한국어] **DMA 채널 레지스터를 끈다** — 이 드라이버는 브리지의 DMA 기능을 쓰지 않는다 */
			  CFG_DMA_REG_BAR, BRCFG_PCIE_RX0);	/* [한국어] 해당 비트들을 얹어 되쓴다 */

	/* Enable Ingress subtractive decode translation */
	nwl_bridge_writel(pcie, SET_ISUB_CONTROL, I_ISUB_CONTROL);	/* [한국어] **인그레스 subtractive 디코드 변환을 켠다** */

	/* Enable msg filtering details */
	nwl_bridge_writel(pcie, CFG_ENABLE_MSG_FILTER_MASK,	/* [한국어] **메시지 필터를 연다** — PM·INT·ERR 메시지를 브리지가 전달하게 한다 */
			  BRCFG_PCIE_RX_MSG_FILTER);	/* [한국어] 그래야 misc 핸들러가 볼 상태 비트가 생긴다 */

	/* This routes the PCIe DMA traffic to go through CCI path */
	if (of_dma_is_coherent(dev->of_node))	/* [한국어] **장치 트리가 dma-coherent 를 밝혔으면** */
		nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_PCIE_RX1) |	/* [한국어] 바로 위 상류 주석대로 PCIe DMA 트래픽을 CCI 경로로 보내는 설정을 켠다 */
				  CFG_PCIE_CACHE, BRCFG_PCIE_RX1);	/* [한국어] 캐시 비트들을 얹어 되쓴다 */

	err = nwl_wait_for_link(pcie);	/* [한국어] **링크가 올라오기를 기다린다** */
	if (err)	/* [한국어] 안 올라오면 */
		return err;	/* [한국어] 그 코드로 초기화를 접는다 */

	ecam_val = nwl_bridge_readl(pcie, E_ECAM_CAPABILITIES) & E_ECAM_PRESENT;	/* [한국어] **ECAM 창이 하드웨어에 있는지 확인한다** */
	if (!ecam_val) {	/* [한국어] 없으면 */
		dev_err(dev, "ECAM is not present\n");	/* [한국어] 그 사실을 알리고 */
		return ecam_val;	/* [한국어] 그 값을 돌려준다. 위 BREG 검사와 같은 이유로 결과적으로 0 이다 */
	}

	/* Enable ECAM */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, E_ECAM_CONTROL) |	/* [한국어] **ECAM 창을 켠다** */
			  E_ECAM_CR_ENABLE, E_ECAM_CONTROL);	/* [한국어] 활성화 비트를 얹어 되쓴다 */

	ecam_val = nwl_bridge_readl(pcie, E_ECAM_CONTROL);	/* [한국어] 제어 레지스터를 다시 읽어 */
	ecam_val &= ~E_ECAM_SIZE_LOC;	/* [한국어] 기존 크기 필드를 지우고 */
	ecam_val |= NWL_ECAM_MAX_SIZE << E_ECAM_SIZE_SHIFT;	/* [한국어] **NWL_ECAM_MAX_SIZE(16)를 그 자리에 넣는다** — 창 크기를 소프트웨어가 정한다 */
	nwl_bridge_writel(pcie, ecam_val, E_ECAM_CONTROL);	/* [한국어] 고친 값을 되쓴다 */

	nwl_bridge_writel(pcie, lower_32_bits(pcie->phys_ecam_base),	/* [한국어] **ecam 창의 물리 주소를 브리지에 알린다** */
			  E_ECAM_BASE_LO);	/* [한국어] 하위 워드 */
	nwl_bridge_writel(pcie, upper_32_bits(pcie->phys_ecam_base),	/* [한국어] 상위 워드에 */
			  E_ECAM_BASE_HI);	/* [한국어] 각각 심는다. **이 단계가 끝나야 map_bus 가 만든 주소가 실제로 동작한다** */

	if (nwl_pcie_link_up(pcie))	/* [한국어] 링크가 섰는지 보고 */
		dev_info(dev, "Link is UP\n");	/* [한국어] 섰다고 알리거나 */
	else
		dev_info(dev, "Link is DOWN\n");	/* [한국어] 안 섰다고 알린다. 어느 쪽이든 초기화는 계속된다 */

	/* Get misc IRQ number */
	pcie->irq_misc = platform_get_irq_byname(pdev, "misc");	/* [한국어] **기타 인터럽트 번호를 이름으로 얻는다** */
	if (pcie->irq_misc < 0)	/* [한국어] 없으면 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */

	err = devm_request_irq(dev, pcie->irq_misc,	/* [한국어] **이 인터럽트만 보통의 핸들러로 등록한다**(나머지 셋은 연쇄 핸들러) */
			       nwl_pcie_misc_handler, IRQF_SHARED,	/* [한국어] 선을 공유할 수 있다고 알린다 — 그래서 핸들러가 첫 줄에서 우리 것인지 확인한다 */
			       "nwl_pcie:misc", pcie);	/* [한국어] /proc/interrupts 에 보일 이름과 넘길 인스턴스 */
	if (err) {	/* [한국어] 등록에 실패하면 */
		dev_err(dev, "fail to register misc IRQ#%d\n",	/* [한국어] 그 사실을 알리고 */
			pcie->irq_misc);
		return err;	/* [한국어] 그 코드를 돌려준다 */
	}

	/* Disable all misc interrupts */
	nwl_bridge_writel(pcie, (u32)~MSGF_MISC_SR_MASKALL, MSGF_MISC_MASK);	/* [한국어] **기타 인터럽트를 전부 막는다.** 이 레지스터도 활성화 레지스터라 보수를 쓰는 것이 막는 것이다 */

	/* Clear pending misc interrupts */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_MISC_STATUS) &	/* [한국어] 현재 상태를 읽어 */
			  MSGF_MISC_SR_MASKALL, MSGF_MISC_STATUS);	/* [한국어] 되써서 남아 있던 것을 지운다 */

	/* Enable all misc interrupts */
	nwl_bridge_writel(pcie, MSGF_MISC_SR_MASKALL, MSGF_MISC_MASK);	/* [한국어] **전체를 열어 준다** */

	/* Disable all INTX interrupts */
	nwl_bridge_writel(pcie, (u32)~MSGF_LEG_SR_MASKALL, MSGF_LEG_MASK);	/* [한국어] **INTx 도 같은 순서를 밟는다.** 먼저 전부 막고 */

	/* Clear pending INTX interrupts */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_LEG_STATUS) &	/* [한국어] 현재 상태를 읽어 */
			  MSGF_LEG_SR_MASKALL, MSGF_LEG_STATUS);	/* [한국어] 되써서 지우고 */

	/* Enable all INTX interrupts */
	nwl_bridge_writel(pcie, MSGF_LEG_SR_MASKALL, MSGF_LEG_MASK);	/* [한국어] 전체를 열어 준다 */

	/* Enable the bridge config interrupt */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_INTERRUPT) |	/* [한국어] **마지막으로 브리지 설정 인터럽트를 켠다** */
			  BRCFG_INTERRUPT_MASK, BRCFG_INTERRUPT);	/* [한국어] 해당 비트를 얹어 되쓴다 */

	return 0;	/* [한국어] 모든 단계를 지났으면 성공 */
}

/* [한국어]
 * nwl_pcie_parse_dt - 장치 트리에서 레지스터 창 셋, INTx 인터럽트, PHY 를 모은다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * **이름으로 세 MMIO 자원을 얻는다.** 각각 매핑한 가상 주소와 물리
 * 주소를 함께 남기는데, 브리지에게 "이 창이 어디에 있는지" 를 레지스터로
 * 알려 줘야 하기 때문이다.
 *   "breg"   -> breg_base / phys_breg_base      브리지 레지스터
 *   "pcireg" -> pcireg_base / phys_pcie_reg_base 컨트롤러 레지스터
 *               (**이 물리 주소가 MSI 목적지가 된다**)
 *   "cfg"    -> ecam_base / phys_ecam_base       ECAM 창
 *   ECAM 창만 devm_pci_remap_cfg_resource() 로 매핑한다 — config 공간에
 *   맞는 메모리 속성으로 잡아 주는 전용 헬퍼다.
 *
 * **INTx 인터럽트에 연쇄 핸들러를 여기서 건다.** MSI 쪽 둘은
 * nwl_pcie_enable_msi() 가 따로 걸고, misc 는 bridge_init 이 건다 —
 * 인터럽트 넷을 거는 자리가 세 함수에 흩어져 있다.
 *
 * **PHY 는 개수가 가변이다.** 최대 넷을 인덱스로 얻되 -ENODEV 를 만나면
 * 그 자리를 NULL 로 두고 멈춘다. 그 밖의 오류는 실패로 다룬다.
 *
 * [관찰] platform_get_resource_byname() 이 NULL 을 돌려줄 수 있는데
 * 그것을 검사하지 않고 devm_ioremap_resource() 에 넘긴다. 그 함수가
 * NULL 을 오류로 다루므로 결과적으로는 걸러진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   nwl_pcie_probe() -> [이 함수] -> platform_get_resource_byname()
 *     -> devm_ioremap_resource() -> irq_set_chained_handler_and_data()
 *     -> devm_of_phy_get_by_index()
 */
static int nwl_pcie_parse_dt(struct nwl_pcie *pcie,
			     struct platform_device *pdev)
{
	struct device *dev = pcie->dev;	/* [한국어] 오류 메시지를 낼 장치이자 of_node 의 출처 */
	struct resource *res;	/* [한국어] 이름으로 얻은 MMIO 자원을 잠시 담을 자리 */
	int i;	/* [한국어] PHY 순회 인덱스 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "breg");	/* [한국어] **"breg" 자원 — 브리지 레지스터 창** */
	pcie->breg_base = devm_ioremap_resource(dev, res);	/* [한국어] 매핑해 가상 주소를 얻는다 */
	if (IS_ERR(pcie->breg_base))	/* [한국어] 실패하면 */
		return PTR_ERR(pcie->breg_base);	/* [한국어] 그 오류를 돌려준다 */
	pcie->phys_breg_base = res->start;	/* [한국어] **물리 주소도 남긴다** — 브리지에게 이 창의 위치를 알려 줘야 하기 때문이다 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcireg");	/* [한국어] **"pcireg" 자원 — 컨트롤러 레지스터 창** */
	pcie->pcireg_base = devm_ioremap_resource(dev, res);	/* [한국어] 매핑해 가상 주소를 얻는다 */
	if (IS_ERR(pcie->pcireg_base))	/* [한국어] 실패하면 */
		return PTR_ERR(pcie->pcireg_base);	/* [한국어] 그 오류를 돌려준다 */
	pcie->phys_pcie_reg_base = res->start;	/* [한국어] **이 물리 주소가 MSI 목적지가 된다** */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");	/* [한국어] **"cfg" 자원 — ECAM 창** */
	pcie->ecam_base = devm_pci_remap_cfg_resource(dev, res);	/* [한국어] **config 공간 전용 매핑 헬퍼**를 쓴다 — 그에 맞는 메모리 속성으로 잡아 준다 */
	if (IS_ERR(pcie->ecam_base))	/* [한국어] 실패하면 */
		return PTR_ERR(pcie->ecam_base);	/* [한국어] 그 오류를 돌려준다 */
	pcie->phys_ecam_base = res->start;	/* [한국어] 물리 주소도 남긴다 — 브리지의 ECAM 베이스 레지스터에 심을 값이다 */

	/* Get intx IRQ number */
	pcie->irq_intx = platform_get_irq_byname(pdev, "intx");	/* [한국어] **INTx 인터럽트 번호를 이름으로 얻는다** */
	if (pcie->irq_intx < 0)	/* [한국어] 없으면 */
		return pcie->irq_intx;	/* [한국어] 그 오류를 그대로 돌려준다 */

	irq_set_chained_handler_and_data(pcie->irq_intx,	/* [한국어] **연쇄 핸들러로 건다.** MSI 쪽 둘은 nwl_pcie_enable_msi() 가, misc 는 bridge_init 이 따로 건다 — 인터럽트 넷을 거는 자리가 세 함수에 흩어져 있다 */
					 nwl_pcie_leg_handler, pcie);	/* [한국어] INTx 핸들러와 함께 넘길 인스턴스 */


	for (i = 0; i < ARRAY_SIZE(pcie->phy); i++) {	/* [한국어] **PHY 를 최대 넷까지 인덱스로 얻는다** */
		pcie->phy[i] = devm_of_phy_get_by_index(dev, dev->of_node, i);	/* [한국어] 장치 트리의 i 번째 PHY */
		if (PTR_ERR(pcie->phy[i]) == -ENODEV) {	/* [한국어] **-ENODEV 면 보드에 그만큼의 레인이 없다는 뜻이다** */
			pcie->phy[i] = NULL;	/* [한국어] 그 자리를 비워 두고 */
			break;	/* [한국어] 더 찾지 않는다 */
		}

		if (IS_ERR(pcie->phy[i]))	/* [한국어] 그 밖의 오류이면 */
			return PTR_ERR(pcie->phy[i]);	/* [한국어] 그 오류를 돌려준다 */
	}

	return 0;	/* [한국어] 세 창과 INTx, PHY 를 모두 얻었으면 성공 */
}

static const struct of_device_id nwl_pcie_of_match[] = {	/* [한국어] **장치 트리의 compatible 문자열 표.** 이 드라이버가 맡는 것은 하나뿐이다 */
	{ .compatible = "xlnx,nwl-pcie-2.11", },	/* [한국어] NWL PCIe 브리지 2.11 판 */
	{}	/* [한국어] 표의 끝을 알리는 빈 항목 */
};

/* [한국어]
 * nwl_pcie_probe - 진입점. 클럭·PHY·브리지·도메인 순으로 세우고 버스를 연다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * 순서가 이렇다.
 *
 *   1. **호스트 브리지 뒤에 이 드라이버 상태를 붙여 한 번에 잡는다.**
 *      devm_pci_alloc_host_bridge() 가 그렇게 해 주므로
 *      pci_host_bridge_priv() 로 서로를 오갈 수 있다.
 *   2. nwl_pcie_parse_dt() — 레지스터 창 셋, INTx 연쇄 핸들러, PHY.
 *   3. **레퍼런스 클럭을 얻어 켠다.** 이름 없이 하나만 얻는다.
 *   4. nwl_pcie_phy_enable() — PHY 를 최대 넷까지.
 *   5. nwl_pcie_bridge_init() — 브리지 레지스터와 링크, misc 인터럽트.
 *   6. nwl_pcie_init_irq_domain() — INTx 도메인과 MSI 도메인.
 *   7. bridge 에 sysdata 와 ops 를 매단다.
 *   8. CONFIG_PCI_MSI 가 켜져 있으면 nwl_pcie_enable_msi() — MSI 인그레스
 *      창과 연쇄 핸들러 둘. **IS_ENABLED 로 실행 시점에 가른다**
 *      (init_msi_irq_domain 이 #ifdef 로 가르는 것과 대비된다).
 *   9. pci_host_probe() 로 열거를 시작한다.
 *
 * 에러 경로가 둘이다. err_phy 는 PHY 를 끄고 err_clk 로 떨어져 클럭까지
 * 내린다. 4번 이전의 실패는 되돌릴 것이 없어 곧바로 돌아간다.
 *
 * [관찰] 9번이 실패하면 **아무것도 되돌리지 않고** 그 코드를 돌려준다 —
 * 성공했을 때만 0 으로 나가고, 실패는 err_phy 를 거치지 않는다. 6번에서
 * 만든 IRQ 도메인들도 어느 실패 경로에서도 제거되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 5번의 링크 대기에서 1초 가까이 잠든다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> devm_pci_alloc_host_bridge()
 *     -> nwl_pcie_parse_dt() -> clk_prepare_enable()
 *     -> nwl_pcie_phy_enable() -> nwl_pcie_bridge_init()
 *     -> nwl_pcie_init_irq_domain() -> nwl_pcie_enable_msi()
 *     -> pci_host_probe()
 */
static int nwl_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* [한국어] 플랫폼 장치의 device 구조체 */
	struct nwl_pcie *pcie;	/* [한국어] 이 드라이버의 인스턴스 상태 */
	struct pci_host_bridge *bridge;	/* [한국어] PCI 코어 쪽 호스트 브리지 */
	int err;	/* [한국어] 하위 호출 결과 */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));	/* [한국어] **호스트 브리지 뒤에 이 드라이버 상태를 붙여 한 번에 잡는다** */
	if (!bridge)	/* [한국어] 못 잡으면 */
		return -ENODEV;	/* [한국어] 장치 없음으로 돌아간다 */

	pcie = pci_host_bridge_priv(bridge);	/* [한국어] 브리지 뒤에 붙은 이 드라이버 상태의 위치를 얻는다 */
	platform_set_drvdata(pdev, pcie);	/* [한국어] 인스턴스를 플랫폼 장치에 심는다 — remove 가 이 값을 꺼낸다 */

	pcie->dev = dev;	/* [한국어] 오류 메시지와 of_node 에 쓸 장치를 담는다 */

	err = nwl_pcie_parse_dt(pcie, pdev);	/* [한국어] **1단계** 레지스터 창 셋, INTx 연쇄 핸들러, PHY 를 모은다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "Parsing DT failed\n");	/* [한국어] 그 사실을 알리고 */
		return err;	/* [한국어] 그 코드를 돌려준다. 아직 되돌릴 것이 없다 */
	}

	pcie->clk = devm_clk_get(dev, NULL);	/* [한국어] **2단계** 레퍼런스 클럭을 이름 없이 하나 얻는다 */
	if (IS_ERR(pcie->clk))	/* [한국어] 오류이면 */
		return PTR_ERR(pcie->clk);	/* [한국어] 그 오류를 돌려준다 */

	err = clk_prepare_enable(pcie->clk);	/* [한국어] 그 클럭을 켠다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "can't enable PCIe ref clock\n");	/* [한국어] 그 사실을 알리고 */
		return err;	/* [한국어] 그 코드를 돌려준다 */
	}

	err = nwl_pcie_phy_enable(pcie);	/* [한국어] **3단계** PHY 를 최대 넷까지 켠다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "could not enable PHYs\n");	/* [한국어] 그 사실을 알리고 */
		goto err_clk;	/* [한국어] **클럭을 되돌리는 라벨로 간다** */
	}

	err = nwl_pcie_bridge_init(pcie);	/* [한국어] **4단계** 브리지 레지스터를 세우고 링크를 기다린다. misc 인터럽트도 여기서 걸린다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "HW Initialization failed\n");	/* [한국어] 그 사실을 알리고 */
		goto err_phy;	/* [한국어] PHY 까지 되돌리는 라벨로 간다 */
	}

	err = nwl_pcie_init_irq_domain(pcie);	/* [한국어] **5단계** INTx 도메인과 MSI 도메인을 만든다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed creating IRQ Domain\n");	/* [한국어] 그 사실을 알리고 */
		goto err_phy;	/* [한국어] PHY 까지 되돌리는 라벨로 간다 */
	}

	bridge->sysdata = pcie;	/* [한국어] **config 접근에서 되찾을 수 있게 인스턴스를 담는다** — map_bus 가 bus->sysdata 로 꺼낸다 */
	bridge->ops = &nwl_pcie_ops;	/* [한국어] ECAM 기반 config 동작 묶음을 매단다 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) {	/* [한국어] **MSI 가 켜져 있을 때만.** 실행 시점 조건이라 #ifdef 와 달리 코드가 늘 컴파일된다 */
		err = nwl_pcie_enable_msi(pcie);	/* [한국어] MSI 인그레스 창과 연쇄 핸들러 둘을 세운다 */
		if (err < 0) {	/* [한국어] 실패하면 */
			dev_err(dev, "failed to enable MSI support: %d\n", err);	/* [한국어] 그 사실을 알리고 */
			goto err_phy;	/* [한국어] PHY 까지 되돌리는 라벨로 간다 */
		}
	}

	err = pci_host_probe(bridge);	/* [한국어] **버스를 열고 장치를 열거한다** */
	if (!err)	/* [한국어] 성공했으면 */
		return 0;	/* [한국어] 그대로 나간다 */

err_phy:	/* [한국어] **PHY 까지 되돌리는 경로** */
	nwl_pcie_phy_disable(pcie);	/* [한국어] PHY 넷을 역순으로 끄고 아래로 이어진다 */
err_clk:	/* [한국어] **클럭까지 되돌리는 경로** */
	clk_disable_unprepare(pcie->clk);	/* [한국어] 레퍼런스 클럭을 내린다 */
	return err;	/* [한국어] 담아 둔 오류 코드를 돌려준다. **pci_host_probe() 실패도 이 경로를 지난다** */
}

/* [한국어]
 * nwl_pcie_remove - PHY 를 끄고 클럭을 내린다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 가 켠 것 중 **PHY 와 클럭만** 되돌린다. 브리지 레지스터는 그대로
 * 두고, IRQ 도메인도 없애지 않는다.
 *
 * [관찰] 이 드라이버는 builtin_platform_driver 로 등록되고
 * suppress_bind_attrs = true 라 sysfs 를 통한 수동 언바인드가 막혀 있다.
 * 즉 이 함수가 불릴 경로가 좁다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 언바인드).
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> nwl_pcie_phy_disable()
 *     -> clk_disable_unprepare()
 */
static void nwl_pcie_remove(struct platform_device *pdev)
{
	struct nwl_pcie *pcie = platform_get_drvdata(pdev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */

	nwl_pcie_phy_disable(pcie);	/* [한국어] **PHY 를 끈다** */
	clk_disable_unprepare(pcie->clk);	/* [한국어] 클럭을 내린다. **IRQ 도메인과 브리지 레지스터는 그대로 둔다** */
}

static struct platform_driver nwl_pcie_driver = {	/* [한국어] **플랫폼 드라이버 등록 구조체** */
	.driver = {	/* [한국어] 드라이버 코어 쪽 정보 */
		.name = "nwl-pcie",	/* [한국어] 드라이버 이름 */
		.suppress_bind_attrs = true,	/* [한국어] sysfs 로 수동 바인드·언바인드를 막는다 — PCIe 호스트 브리지를 임의로 떼면 안 되기 때문이다 */
		.of_match_table = nwl_pcie_of_match,	/* [한국어] 위 compatible 표 */
	},
	.probe = nwl_pcie_probe,	/* [한국어] 진입점 */
	.remove = nwl_pcie_remove,	/* [한국어] 언바인드 시 정리 */
};
builtin_platform_driver(nwl_pcie_driver);	/* [한국어] **builtin 으로 등록한다** — 이 드라이버는 모듈이 될 수 없고 커널에 붙박이로 들어간다 */
