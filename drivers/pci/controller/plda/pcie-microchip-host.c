// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip AXI PCIe Bridge host controller driver
 *
 * Copyright (c) 2018 - 2020 Microchip Corporation. All rights reserved.
 *
 * Author: Daire McNamara <daire.mcnamara@microchip.com>
 */

/*
 * [한국어 설명] Microchip PolarFire SoC AXI PCIe 브리지 호스트 드라이버
 *               (pcie-microchip-host.c)
 *
 * === 파일의 역할 ===
 * Microchip PolarFire SoC(MPFS, RISC-V + FPGA 패브릭)의 PCIe 루트 포트를 다루는
 * 플랫폼 드라이버다. 같은 디렉터리의 StarFive 드라이버와 마찬가지로 PLDA
 * XpressRich IP 를 쓰지만, Microchip 은 그 IP 바깥에 자기 컨트롤러 레지스터
 * 블록을 하나 더 두었다. 그래서 이 파일의 절반은 "PLDA 표준 이벤트에 더해
 * Microchip 전용 이벤트 15개를 어떻게 하나의 이벤트 번호 공간으로 합칠 것인가" 를
 * 다루는 표와 코드다. 전용 이벤트는 (1) PCIe 상태 전이 3종(L2 이탈, Hot Reset
 * 이탈, Data Link Up 이탈), (2) 내부 버퍼 RAM 의 ECC 오류 -- SEC(Single Error
 * Corrected) 4종과 DED(Double Error Detected) 4종, (3) DMA 엔진 완료/오류 4종이다.
 * 그 밖에 인바운드(PCIe -> AXI) 주소 변환 창을 직접 프로그래밍하고, FPGA 패브릭
 * 인터페이스 클럭(fic0~fic3)을 켜며, MSI capability 를 손보는 일을 한다.
 * 코드에는 이 컨트롤러 인스턴스를 담는 static 전역 포인터가 하나 있어서,
 * 이 드라이버는 시스템에 PCIe 컨트롤러가 하나뿐이라고 가정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버의 진입 경로는 StarFive 와 결정적으로 다르다. StarFive 는
 * plda_pcie_host_init() 에 제어를 넘겨 PLDA 코어가 probe 전체를 주도하게 하지만,
 * Microchip 은 ECAM 공용 코어로 들어간다:
 *   mc_host_probe() -> pci_host_common_probe()(drivers/pci/controller/
 *   pci-host-common.c) -> pci_host_common_init() -> pci_host_common_ecam_create()
 *   -> pci_ecam_create()(drivers/pci/ecam.c) -> ops->init = mc_platform_init()
 *   -> 여기서 PLDA 코어의 헬퍼 세 개(plda_pcie_setup_window,
 *      plda_pcie_setup_iomems, plda_init_interrupts)만 골라 호출
 *   -> 돌아와서 pci_host_probe() 로 버스 스캔.
 * 즉 PLDA 코어를 "프레임워크" 가 아니라 "헬퍼 라이브러리" 로 쓴다.
 * plda_pcie_host_init() 과 plda_pcie_map_bus() 는 이 파일에서 전혀 호출되지 않고,
 * config space 접근은 ECAM 표준 구현(pci_ecam_map_bus + pci_generic_config_read/
 * write)이 담당한다. 실행 컨텍스트는 probe 계열의 프로세스 컨텍스트와,
 * irq_chip 콜백 및 mc_event_handler 의 하드 인터럽트 컨텍스트로 갈린다.
 *
 * === 타 모듈과의 연결 ===
 * 아래 방향 의존: pcie-plda.h(브리지 레지스터 맵, struct plda_pcie_rp,
 * enum plda_int_event), pcie-plda-host.c 의 EXPORT 심볼 세 개,
 * ../pci-host-common.h(pci_host_common_probe), linux/pci-ecam.h(struct
 * pci_config_window, struct pci_ecam_ops, pci_ecam_map_bus), ../../pci.h,
 * 그리고 clk / of_address / of_pci 서브시스템.
 * 위 방향 의존자: 없다. builtin_platform_driver 로 등록되는 최종 드라이버이며,
 * Kconfig 에서 tristate 로 선언되어 있지만 실제 등록 매크로는 builtin 판이라
 * 모듈로 뺄 수 없다(suppress_bind_attrs = true 로 언바인드도 막혀 있다).
 * 데이터 흐름: devicetree -> struct mc_pcie(레지스터 창 두 개) ->
 * struct plda_pcie_rp(코어에 넘길 설정) -> PLDA 코어 -> 하드웨어.
 * 이벤트 방향으로는 네 개의 서로 다른 상태 레지스터(PCIE_EVENT_INT,
 * SEC_ERROR_INT, DED_ERROR_INT, ISTATUS_LOCAL) -> mc_get_events() 가 만든
 * 단일 비트맵 -> PLDA 코어의 event_domain -> mc_event_handler 또는 코어의
 * INTx/MSI 체인 핸들러로 흐른다.
 * NVMe 와의 관계: 이 파일에 nvme 식별자는 없다. MPFS 보드의 PCIe 슬롯에 NVMe
 * SSD 를 꽂으면 이 루트 포트 아래에 열거되지만, 코드 호출 관계는 존재하지 않는다.
 * 다만 mc_pcie_setup_inbound_ranges() 가 설정하는 인바운드 변환 창은 EP 의 DMA 가
 * 시스템 메모리에 닿는 경로이므로, NVMe 컨트롤러의 데이터 전송이 실제로 지나가는
 * 하드웨어 경로이기는 하다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct mc_pcie : plda_pcie_rp 를 첫 필드로 감싸고 브리지/컨트롤러 두 개의
 *    레지스터 창 주소를 담는다. 인스턴스는 static 전역 port 하나뿐이다.
 *  - event_descs[] : 28개 이벤트 각각에 대해 "상태 레지스터 오프셋, 마스크
 *    레지스터 오프셋, 비트 마스크, 마스크 극성" 을 적어 둔 표. Microchip 이
 *    PLDA 코어의 plda_hwirq_to_mask() 를 못 쓰는 이유가 이 표의 존재다.
 *  - mc_get_events() : 네 개 상태 레지스터를 읽어 하나의 이벤트 비트맵으로 합친다.
 *  - mc_ack_event_irq / mc_mask_event_irq / mc_unmask_event_irq : event_descs[] 를
 *    보고 이벤트마다 다른 레지스터와 다른 극성으로 조작하는 irq_chip 구현.
 *  - mc_platform_init() : ECAM 코어가 부르는 훅. ATR 창과 인터럽트를 세운다.
 *  - mc_host_probe() : 드라이버 진입점. 레지스터 창을 잡고 MSI 벡터 수를 읽은 뒤
 *    ECAM 공용 probe 에 넘긴다.
 *  - mc_pcie_setup_inbound_ranges() : coherent/non-coherent 설계에 따라 인바운드
 *    주소 변환 창을 구성한다. MPFS 고유의 FIC(Fabric Interface Controller) 제약이
 *    반영된 부분이다.
 */

/* [한국어] ALIGN_DOWN() -- mc_pcie_setup_inbound_atr() 이 인바운드 소스 주소를 4KiB 로
 * 내림해 하위 12비트를 비우는 데 쓴다. 그 자리에 크기 필드와 enable 비트가 들어간다. */
#include <linux/align.h>
/* [한국어] BIT(), GENMASK() 매크로. 이 파일의 레지스터 정의 대부분이 이 둘로 쓰여 있다. */
#include <linux/bits.h>
/* [한국어] FIELD_PREP()/FIELD_GET() -- MSI capability 의 QMASK/QSIZE 필드와 ATR_SIZE_MASK
 * 조작에 쓴다. */
#include <linux/bitfield.h>
/* [한국어] devm_clk_get_optional(), clk_prepare_enable() -- FPGA 패브릭 인터페이스 클럭
 * (fic0~fic3)을 켜기 위해 필요하다. */
#include <linux/clk.h>
/* [한국어] chained_irq 관련 선언. 이 파일 자체에는 체인 핸들러가 없지만(전부 PLDA 코어에
 * 있다) 헤더 체인상 포함되어 있다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_get_irq_data() 등 irq 도메인 API. mc_event_handler 가 virq 로부터
 * hwirq 를 되찾는 데 쓴다. */
#include <linux/irqdomain.h>
/* [한국어] ilog2() -- 인바운드 ATR 창 크기를 로그값으로 바꾸는 데 쓴다. */
#include <linux/log2.h>
/* [한국어] MODULE_LICENSE 등. 등록은 builtin_platform_driver 로 하지만 모듈 메타데이터
 * 매크로는 이 헤더가 제공한다. */
#include <linux/module.h>
/* [한국어] PCI_MSI_FLAGS 등 MSI 관련 정의. mc_pcie_enable_msi() 가 config space 의
 * MSI capability 를 직접 손볼 때 필요하다. */
#include <linux/msi.h>
/* [한국어] of_pci_dma_range_parser_init(), for_each_of_range -- devicetree 의 dma-ranges 를
 * 순회해 인바운드 창을 만들 때 쓴다. */
#include <linux/of_address.h>
/* [한국어] PCI 관련 devicetree 헬퍼 선언. */
#include <linux/of_pci.h>
/* [한국어] struct pci_config_window, struct pci_ecam_ops, pci_ecam_map_bus 등 ECAM 코어.
 * 이 드라이버가 ECAM 공용 경로로 진입하기 때문에 반드시 필요하다. */
#include <linux/pci-ecam.h>
/* [한국어] platform_device / platform_driver 정의. */
#include <linux/platform_device.h>
/* [한국어] lower_32_bits()/upper_32_bits() -- 64비트 주소를 두 레지스터에 나눠 쓸 때 쓴다. */
#include <linux/wordpart.h>

/* [한국어] drivers/pci/pci.h (PCI 코어 내부 헤더). include/ 가 아니라 드라이버 트리 안에만
 * 있는 헤더라 상대 경로로 포함한다. */
#include "../../pci.h"
/* [한국어] pci_host_common_probe() 선언. 이 드라이버가 StarFive 와 갈라지는 지점이
 * 바로 이 헤더를 포함한다는 사실이다. */
#include "../pci-host-common.h"
/* [한국어] PLDA 공용 헤더. 브리지 레지스터 오프셋(ISTATUS_LOCAL, IMASK_LOCAL, ATR 테이블)과
 * enum plda_int_event, plda_ 로 시작하는 헬퍼 선언이 여기서 온다. */
#include "pcie-plda.h"

/* [한국어] 인바운드(PCIe -> AXI) 주소 변환 창의 최대 개수 8.
 * mc_pcie_setup_inbound_ranges() 가 devicetree 의 dma-ranges 항목이 이보다 많으면
 * -EINVAL 로 거부한다. 아웃바운드 쪽(plda_pcie_setup_iomems)에는 이런 검사가 없다. */
#define MC_MAX_NUM_INBOUND_WINDOWS		8
/* [한국어] non-coherent 설계에서 CPU 주소 0 을 PCIe 주소 0x80000000 으로 옮기는 바운스
 * 기준 주소. 아래 setup_inbound_ranges 의 영문 주석이 이유를 설명한다 --
 * MPFS 의 AXI-S 영역에 MSI 공간이 배치되는 방식 때문에 1GiB 창 두 개가 필요하다. */
#define MPFS_NC_BOUNCE_ADDR			0x80000000

/* PCIe Bridge Phy and Controller Phy offsets */
/* [한국어] 레거시 devicetree 바인딩(apb 창 하나만 있던 시절)에서 브리지 레지스터가
 * 시작하는 상대 오프셋 0x8000. */
#define MC_PCIE1_BRIDGE_ADDR			0x00008000u
/* [한국어] 같은 레거시 바인딩에서 컨트롤러 레지스터가 시작하는 상대 오프셋 0xa000.
 * 두 블록이 8KiB 떨어져 있다는 뜻이다. */
#define MC_PCIE1_CTRL_ADDR			0x0000a000u

/* PCIe Controller Phy Regs */
/* [한국어] 컨트롤러 창 오프셋 0x20 -- SEC(정정 가능한 단일 비트 ECC 오류) 발생 횟수 카운터.
 * mc_clear_secs() 가 0 을 써서 초기화한다. */
#define SEC_ERROR_EVENT_CNT			0x20
/* [한국어] 오프셋 0x24 -- DED(정정 불가능한 이중 비트 ECC 오류) 발생 횟수 카운터. */
#define DED_ERROR_EVENT_CNT			0x24
/* [한국어] 오프셋 0x28 -- SEC 오류 상태 레지스터. 네 개 버퍼(TX/RX/PCIE2AXI/AXI2PCIE)에
 * 대해 4비트씩 총 16비트를 쓴다. */
#define SEC_ERROR_INT				0x28
/* [한국어] TX 버퍼 RAM 의 SEC 오류 4비트(3:0). 4비트인 이유는 버퍼가 4뱅크로 나뉘어
 * 있기 때문으로 보이며(NUM_SEC_ERROR_INTS 가 4), 이 드라이버는 네 비트를
 * 하나의 이벤트로 묶어 다룬다. */
#define  SEC_ERROR_INT_TX_RAM_SEC_ERR_INT	GENMASK(3, 0)
/* [한국어] RX 버퍼 RAM 의 SEC 오류 4비트(7:4). */
#define  SEC_ERROR_INT_RX_RAM_SEC_ERR_INT	GENMASK(7, 4)
/* [한국어] PCIe -> AXI 방향 버퍼의 SEC 오류 4비트(11:8). */
#define  SEC_ERROR_INT_PCIE2AXI_RAM_SEC_ERR_INT	GENMASK(11, 8)
/* [한국어] AXI -> PCIe 방향 버퍼의 SEC 오류 4비트(15:12). */
#define  SEC_ERROR_INT_AXI2PCIE_RAM_SEC_ERR_INT	GENMASK(15, 12)
/* [한국어] 위 네 개를 한꺼번에 가리키는 마스크(15:0). mc_disable_interrupts() 와
 * mc_clear_secs() 가 전부 끄고 전부 지우는 데 쓴다. */
#define  SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT	GENMASK(15, 0)
/* [한국어] 버퍼당 SEC 인터럽트 비트 수 4. 이 트리의 코드에서 실제로 참조되지는 않고
 * 비트 폭을 설명하는 문서용 상수다. */
#define  NUM_SEC_ERROR_INTS			(4)
/* [한국어] 오프셋 0x2c -- SEC 오류 마스크 레지스터. 이쪽은 1 이 '마스크(차단)' 다
 * (event_descs 의 mask_high = 1 이 그 뜻이며, IMASK_LOCAL 의 1 = 허용 과 반대다). */
#define SEC_ERROR_INT_MASK			0x2c
/* [한국어] 오프셋 0x30 -- DED 오류 상태 레지스터. 비트 배치는 SEC 와 동일하다. */
#define DED_ERROR_INT				0x30
/* [한국어] TX 버퍼 RAM 의 DED 오류 4비트(3:0). */
#define  DED_ERROR_INT_TX_RAM_DED_ERR_INT	GENMASK(3, 0)
/* [한국어] RX 버퍼 RAM 의 DED 오류 4비트(7:4). */
#define  DED_ERROR_INT_RX_RAM_DED_ERR_INT	GENMASK(7, 4)
/* [한국어] PCIe -> AXI 버퍼의 DED 오류 4비트(11:8). */
#define  DED_ERROR_INT_PCIE2AXI_RAM_DED_ERR_INT	GENMASK(11, 8)
/* [한국어] AXI -> PCIe 버퍼의 DED 오류 4비트(15:12). */
#define  DED_ERROR_INT_AXI2PCIE_RAM_DED_ERR_INT	GENMASK(15, 12)
/* [한국어] 위 네 개 전체(15:0). */
#define  DED_ERROR_INT_ALL_RAM_DED_ERR_INT	GENMASK(15, 0)
/* [한국어] 버퍼당 DED 인터럽트 비트 수 4. 역시 참조되지 않는 문서용 상수다. */
#define  NUM_DED_ERROR_INTS			(4)
/* [한국어] 오프셋 0x34 -- DED 오류 마스크 레지스터. 1 이 마스크다. */
#define DED_ERROR_INT_MASK			0x34
/* [한국어] 오프셋 0x38 -- ECC 제어 레지스터. 오류 주입(테스트용)과 ECC 우회 설정이 들어 있다. */
#define ECC_CONTROL				0x38
/* [한국어] TX 버퍼 뱅크 0 에 고의로 ECC 오류를 주입하는 테스트 비트.
 * 이 드라이버는 주입 비트를 전혀 쓰지 않는다 -- 아래 16개 정의는 모두 문서용이다. */
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_0		BIT(0)
/* [한국어] TX 버퍼 뱅크 1 오류 주입. */
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_1		BIT(1)
/* [한국어] TX 버퍼 뱅크 2 오류 주입. */
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_2		BIT(2)
/* [한국어] TX 버퍼 뱅크 3 오류 주입. */
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_3		BIT(3)
/* [한국어] RX 버퍼 뱅크 0 오류 주입. */
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_0		BIT(4)
/* [한국어] RX 버퍼 뱅크 1 오류 주입. */
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_1		BIT(5)
/* [한국어] RX 버퍼 뱅크 2 오류 주입. */
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_2		BIT(6)
/* [한국어] RX 버퍼 뱅크 3 오류 주입. */
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_3		BIT(7)
/* [한국어] PCIe -> AXI 버퍼 뱅크 0 오류 주입. */
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_0	BIT(8)
/* [한국어] PCIe -> AXI 버퍼 뱅크 1 오류 주입. */
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_1	BIT(9)
/* [한국어] PCIe -> AXI 버퍼 뱅크 2 오류 주입. */
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_2	BIT(10)
/* [한국어] PCIe -> AXI 버퍼 뱅크 3 오류 주입. */
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_3	BIT(11)
/* [한국어] AXI -> PCIe 버퍼 뱅크 0 오류 주입. */
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_0	BIT(12)
/* [한국어] AXI -> PCIe 버퍼 뱅크 1 오류 주입. */
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_1	BIT(13)
/* [한국어] AXI -> PCIe 버퍼 뱅크 2 오류 주입. */
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_2	BIT(14)
/* [한국어] AXI -> PCIe 버퍼 뱅크 3 오류 주입. */
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_3	BIT(15)
/* [한국어] TX 버퍼의 ECC 검사를 우회한다. mc_disable_interrupts() 가 네 개 우회 비트를
 * 모두 세워 ECC 자체를 끈다 -- ECC 오류 인터럽트를 쓰지 않겠다는 결정이다. */
#define  ECC_CONTROL_TX_RAM_ECC_BYPASS		BIT(24)
/* [한국어] RX 버퍼 ECC 우회. */
#define  ECC_CONTROL_RX_RAM_ECC_BYPASS		BIT(25)
/* [한국어] PCIe -> AXI 버퍼 ECC 우회. */
#define  ECC_CONTROL_PCIE2AXI_RAM_ECC_BYPASS	BIT(26)
/* [한국어] AXI -> PCIe 버퍼 ECC 우회. */
#define  ECC_CONTROL_AXI2PCIE_RAM_ECC_BYPASS	BIT(27)
/* [한국어] 오프셋 0x14c -- PCIe 상태 전이 이벤트 레지스터. 상태 비트(2:0)와 마스크
 * 비트(18:16)가 한 레지스터에 함께 들어 있는 것이 특징이며, 그래서 이 이벤트만
 * event_descs 의 offset 과 mask_offset 이 같다. */
#define PCIE_EVENT_INT				0x14c
/* [한국어] bit0 -- L2 저전력 상태에서 빠져나왔음. 상태 비트이며 write-1-to-clear 다. */
#define  PCIE_EVENT_INT_L2_EXIT_INT		BIT(0)
/* [한국어] bit1 -- Hot Reset 에서 빠져나왔음. */
#define  PCIE_EVENT_INT_HOTRST_EXIT_INT		BIT(1)
/* [한국어] bit2 -- Data Link Up 상태에서 이탈했음(링크가 끊어짐). */
#define  PCIE_EVENT_INT_DLUP_EXIT_INT		BIT(2)
/* [한국어] 상태 비트 세 개를 묶은 마스크(2:0). 이 트리에서 참조되지 않는 문서용 정의다. */
#define  PCIE_EVENT_INT_MASK			GENMASK(2, 0)
/* [한국어] bit16 -- L2 이탈 이벤트의 인터럽트 마스크. 1 을 쓰면 차단된다
 * (mc_disable_interrupts 가 세 개 상태 비트와 함께 이 세 마스크 비트를 써서
 * '끄고 지운다' 는 것이 그 근거다). */
#define  PCIE_EVENT_INT_L2_EXIT_INT_MASK	BIT(16)
/* [한국어] bit17 -- Hot Reset 이탈 이벤트의 마스크. */
#define  PCIE_EVENT_INT_HOTRST_EXIT_INT_MASK	BIT(17)
/* [한국어] bit18 -- DLUP 이탈 이벤트의 마스크. */
#define  PCIE_EVENT_INT_DLUP_EXIT_INT_MASK	BIT(18)
/* [한국어] 마스크 비트 세 개를 묶은 것(18:16). mask/unmask 콜백이 결과를 이 범위로
 * 가두는 데 쓴다. */
#define  PCIE_EVENT_INT_ENB_MASK		GENMASK(18, 16)
/* [한국어] 상태 비트(0..2)를 마스크 비트(16..18)로 옮기는 시프트 값 16.
 * mc_mask_event_irq/mc_unmask_event_irq 가 event_descs 의 mask 를 이만큼 민다. */
#define  PCIE_EVENT_INT_ENB_SHIFT		16
/* [한국어] PCIe 상태 전이 이벤트의 개수 3. 참조되지 않는 문서용 상수다. */
#define  NUM_PCIE_EVENTS			(3)

/* PCIe Config space MSI capability structure */
/* [한국어] 루트 포트 자신의 config space 에서 MSI capability 구조체가 시작하는 오프셋 0xe0.
 * mc_pcie_enable_msi() 가 여기에 PCI_MSI_FLAGS(0x02), PCI_MSI_ADDRESS_LO(0x04),
 * PCI_MSI_ADDRESS_HI(0x08) 를 더해 접근한다. */
#define MC_MSI_CAP_CTRL_OFFSET			0xe0u

/* Events */
/* [한국어] 이벤트 번호 0 -- PCIe L2 이탈. 이 파일의 이벤트 번호 체계는 0..14 가 Microchip
 * 전용, 15..27 이 PLDA 표준(enum plda_int_event + 15)이다. */
#define EVENT_PCIE_L2_EXIT			0
/* [한국어] 이벤트 번호 1 -- Hot Reset 이탈. */
#define EVENT_PCIE_HOTRST_EXIT			1
/* [한국어] 이벤트 번호 2 -- Data Link Up 이탈. */
#define EVENT_PCIE_DLUP_EXIT			2
/* [한국어] 이벤트 번호 3 -- TX 버퍼 SEC 오류. */
#define EVENT_SEC_TX_RAM_SEC_ERR		3
/* [한국어] 이벤트 번호 4 -- RX 버퍼 SEC 오류. */
#define EVENT_SEC_RX_RAM_SEC_ERR		4
/* [한국어] 이벤트 번호 5 -- PCIe -> AXI 버퍼 SEC 오류. */
#define EVENT_SEC_PCIE2AXI_RAM_SEC_ERR		5
/* [한국어] 이벤트 번호 6 -- AXI -> PCIe 버퍼 SEC 오류. */
#define EVENT_SEC_AXI2PCIE_RAM_SEC_ERR		6
/* [한국어] 이벤트 번호 7 -- TX 버퍼 DED 오류. */
#define EVENT_DED_TX_RAM_DED_ERR		7
/* [한국어] 이벤트 번호 8 -- RX 버퍼 DED 오류. */
#define EVENT_DED_RX_RAM_DED_ERR		8
/* [한국어] 이벤트 번호 9 -- PCIe -> AXI 버퍼 DED 오류. */
#define EVENT_DED_PCIE2AXI_RAM_DED_ERR		9
/* [한국어] 이벤트 번호 10 -- AXI -> PCIe 버퍼 DED 오류. */
#define EVENT_DED_AXI2PCIE_RAM_DED_ERR		10
/* [한국어] 이벤트 번호 11 -- DMA 엔진 0 완료. 대응 마스크 DMA_END_ENGINE_0_MASK 가
 * 0x00000000 이라 실제로는 절대 보고되지 않는 이벤트다. event_cause[] 에도
 * 이 번호의 항목이 없어 NULL 로 남는다. */
#define EVENT_LOCAL_DMA_END_ENGINE_0		11
/* [한국어] 이벤트 번호 12 -- DMA 엔진 1 완료. 위와 같은 이유로 실효가 없다. */
#define EVENT_LOCAL_DMA_END_ENGINE_1		12
/* [한국어] 이벤트 번호 13 -- DMA 엔진 0 오류. 이쪽 마스크는 0x00000100 으로 실재한다. */
#define EVENT_LOCAL_DMA_ERROR_ENGINE_0		13
/* [한국어] 이벤트 번호 14 -- DMA 엔진 1 오류. */
#define EVENT_LOCAL_DMA_ERROR_ENGINE_1		14
/* [한국어] Microchip 전용 이벤트의 개수 15. PLDA 표준 이벤트 번호에 더해지는 오프셋으로,
 * StarFive 가 쓰는 PLDA_NUM_DMA_EVENTS(16)에 해당하는 자리다.
 * 이 값이 다르기 때문에 코어가 INTx/MSI 이벤트 번호를 하드코딩하지 못하고
 * struct plda_event 로 전달받는다. */
#define NUM_MC_EVENTS				15
/* [한국어] 이벤트 번호 15 = 15 + PLDA_AXI_POST_ERR(0). 여기서부터가 PLDA 표준 이벤트다.
 * 주의: 같은 이벤트가 StarFive 에서는 16 번인데, 앞에 놓인 SoC 전용 이벤트 개수가
 * 16 이냐 15 냐로 갈리기 때문이다. 이것이 이벤트 번호를 하드코딩하면 안 되는 이유다. */
#define EVENT_LOCAL_A_ATR_EVT_POST_ERR		(NUM_MC_EVENTS + PLDA_AXI_POST_ERR)
/* [한국어] 이벤트 16 -- AXI 쪽 읽기 요청 오류(ISTATUS_LOCAL bit17). */
#define EVENT_LOCAL_A_ATR_EVT_FETCH_ERR		(NUM_MC_EVENTS + PLDA_AXI_FETCH_ERR)
/* [한국어] 이벤트 17 -- AXI 쪽 요청 폐기/읽기 타임아웃(bit18). */
#define EVENT_LOCAL_A_ATR_EVT_DISCARD_ERR	(NUM_MC_EVENTS + PLDA_AXI_DISCARD_ERR)
/* [한국어] 이벤트 18 -- AXI doorbell. 마스크가 0x00000000 이라 실제로는 보고되지 않으며
 * event_cause[] 에도 항목이 없어 NULL 로 남는다. */
#define EVENT_LOCAL_A_ATR_EVT_DOORBELL		(NUM_MC_EVENTS + PLDA_AXI_DOORBELL)
/* [한국어] 이벤트 19 -- PCIe 쪽 쓰기 요청 오류(bit20). */
#define EVENT_LOCAL_P_ATR_EVT_POST_ERR		(NUM_MC_EVENTS + PLDA_PCIE_POST_ERR)
/* [한국어] 이벤트 20 -- PCIe 쪽 읽기 요청 오류(bit21). */
#define EVENT_LOCAL_P_ATR_EVT_FETCH_ERR		(NUM_MC_EVENTS + PLDA_PCIE_FETCH_ERR)
/* [한국어] 이벤트 21 -- PCIe 쪽 요청 폐기(bit22). */
#define EVENT_LOCAL_P_ATR_EVT_DISCARD_ERR	(NUM_MC_EVENTS + PLDA_PCIE_DISCARD_ERR)
/* [한국어] 이벤트 22 -- PCIe doorbell. 역시 마스크가 0 이라 실효가 없다. */
#define EVENT_LOCAL_P_ATR_EVT_DOORBELL		(NUM_MC_EVENTS + PLDA_PCIE_DOORBELL)
/* [한국어] 이벤트 23 -- INTx 묶음. mc_event 의 intx_event 로 코어에 전달되어
 * plda_handle_intx 체인 핸들러가 이 virq 에 걸린다. */
#define EVENT_LOCAL_PM_MSI_INT_INTX		(NUM_MC_EVENTS + PLDA_INTX)
/* [한국어] 이벤트 24 -- MSI 묶음. mc_event 의 msi_event 로 전달되어
 * plda_handle_msi 가 걸린다. */
#define EVENT_LOCAL_PM_MSI_INT_MSI		(NUM_MC_EVENTS + PLDA_MSI)
/* [한국어] 이벤트 25 -- AER 이벤트(bit29). */
#define EVENT_LOCAL_PM_MSI_INT_AER_EVT		(NUM_MC_EVENTS + PLDA_AER_EVENT)
/* [한국어] 이벤트 26 -- PM/LTR/Hotplug 등 기타 이벤트(bit30). */
#define EVENT_LOCAL_PM_MSI_INT_EVENTS		(NUM_MC_EVENTS + PLDA_MISC_EVENTS)
/* [한국어] 이벤트 27 -- 시스템 오류(bit31). */
#define EVENT_LOCAL_PM_MSI_INT_SYS_ERR		(NUM_MC_EVENTS + PLDA_SYS_ERR)
/* [한국어] 전체 이벤트 개수 28 = 15 + 13. plda->num_events 와 events_bitmap 의 폭이 되고,
 * event_cause[] 와 event_descs[] 배열의 크기 기준이기도 하다. */
#define NUM_EVENTS				(NUM_MC_EVENTS + PLDA_INT_EVENT_NUM)

/* [한국어] event_cause[] 항목을 지정 초기화자로 만드는 매크로.
 * __stringify(x) 가 매크로 인자를 그대로 문자열로 바꿔 sym 필드에 넣는다.
 * 예: PCIE_EVENT_CAUSE(L2_EXIT, "...") -> [0] = { "L2_EXIT", "..." }.
 * sym 은 /proc/interrupts 에 나올 IRQ 이름으로, str 은 오류 로그 문구로 쓰인다. */
#define PCIE_EVENT_CAUSE(x, s)	\
	[EVENT_PCIE_ ## x] = { __stringify(x), s }

/* [한국어] SEC 오류용 같은 매크로. 인덱스가 EVENT_SEC_ 접두사로 만들어진다. */
#define SEC_ERROR_CAUSE(x, s) \
	[EVENT_SEC_ ## x] = { __stringify(x), s }

/* [한국어] DED 오류용 같은 매크로. */
#define DED_ERROR_CAUSE(x, s) \
	[EVENT_DED_ ## x] = { __stringify(x), s }

/* [한국어] PLDA 로컬 이벤트용 같은 매크로. 인덱스가 EVENT_LOCAL_ 접두사다. */
#define LOCAL_EVENT_CAUSE(x, s) \
	[EVENT_LOCAL_ ## x] = { __stringify(x), s }

/* [한국어] event_descs[] 항목 중 'PCIe 상태 전이 이벤트' 세 개를 위한 필드 묶음.
 * 특징 두 가지: (1) offset 과 mask_offset 이 같은 레지스터(0x14c)다 -- 상태 비트와
 * 마스크 비트가 한 워드에 함께 있기 때문이다. (2) enb_mask 가 0 이 아니어서
 * mask/unmask 콜백이 마스크를 16비트 왼쪽으로 밀고 결과를 비트 18:16 으로 가둔다.
 * mask_high = 1 은 '마스크 레지스터에 1 을 쓰면 차단' 이라는 극성 표시다. */
#define PCIE_EVENT(x) \
	.offset = PCIE_EVENT_INT, \
	.mask_offset = PCIE_EVENT_INT, \
	.mask_high = 1, \
	.mask = PCIE_EVENT_INT_ ## x ## _INT, \
	.enb_mask = PCIE_EVENT_INT_ENB_MASK

/* [한국어] SEC 오류 이벤트용 필드 묶음. 상태 레지스터(0x28)와 마스크 레지스터(0x2c)가
 * 따로 있고 enb_mask 는 0 이다. mask_high = 1 이라 1 이 차단이다. */
#define SEC_EVENT(x) \
	.offset = SEC_ERROR_INT, \
	.mask_offset = SEC_ERROR_INT_MASK, \
	.mask = SEC_ERROR_INT_ ## x ## _INT, \
	.mask_high = 1, \
	.enb_mask = 0

/* [한국어] DED 오류 이벤트용 필드 묶음. 구조는 SEC 와 동일하고 레지스터만 0x30/0x34 다. */
#define DED_EVENT(x) \
	.offset = DED_ERROR_INT, \
	.mask_offset = DED_ERROR_INT_MASK, \
	.mask_high = 1, \
	.mask = DED_ERROR_INT_ ## x ## _INT, \
	.enb_mask = 0

/* [한국어] PLDA 로컬 이벤트용 필드 묶음. 여기만 mask_high 가 0 인데, IMASK_LOCAL 은
 * 1 이 '허용' 이고 0 이 '차단' 이라 극성이 반대이기 때문이다. 이 한 필드 때문에
 * mask/unmask 콜백에 if 분기가 들어간다.
 * 'x ## _MASK' 토큰 붙이기로 pcie-plda.h 의 마스크 상수를 끌어 쓴다 -- 그래서
 * PM_MSI_INT_INTX_MASK 처럼 이 파일에 직접 등장하지 않는 상수도 실제로는 쓰인다. */
#define LOCAL_EVENT(x) \
	.offset = ISTATUS_LOCAL, \
	.mask_offset = IMASK_LOCAL, \
	.mask_high = 0, \
	.mask = x ## _MASK, \
	.enb_mask = 0

/* [한국어] reg_to_event() 가 쓸 변환 표 항목 하나를 만든다.
 * { 레지스터 비트 마스크, 그 비트가 뜻하는 이벤트 번호 } 쌍이다. */
#define PCIE_EVENT_TO_EVENT_MAP(x) \
	{ PCIE_EVENT_INT_ ## x ## _INT, EVENT_PCIE_ ## x }

/* [한국어] SEC 오류용 같은 매크로. */
#define SEC_ERROR_TO_EVENT_MAP(x) \
	{ SEC_ERROR_INT_ ## x ## _INT, EVENT_SEC_ ## x }

/* [한국어] DED 오류용 변환 표 항목 매크로. */
#define DED_ERROR_TO_EVENT_MAP(x) \
	{ DED_ERROR_INT_ ## x ## _INT, EVENT_DED_ ## x }

/* [한국어] PLDA 로컬 이벤트용 변환 표 항목 매크로. 'x ## _MASK' 로 pcie-plda.h 의
 * 마스크 상수를 끌어 쓴다. */
#define LOCAL_STATUS_TO_EVENT_MAP(x) \
	{ x ## _MASK, EVENT_LOCAL_ ## x }

/*
 * [한국어]
 * struct event_map - "레지스터 비트 -> 이벤트 번호" 변환 표의 한 항목
 *
 * 왜 필요한가: Microchip 은 네 개의 서로 다른 상태 레지스터에서 이벤트를 모아야
 * 하는데, 각 레지스터의 비트 배치가 제각각이고 이벤트 번호와의 대응도 규칙적이지
 * 않다(SEC/DED 는 4비트가 이벤트 하나, INTx 는 4비트가 이벤트 하나, 나머지는 1:1).
 * 그래서 계산식 대신 표를 쓴다. StarFive 쪽 plda_get_events() 가 시프트 연산으로
 * 같은 일을 하는 것과 대조된다.
 *
 * 설정자: 네 개의 TO_EVENT_MAP 매크로가 컴파일 타임에 채운다.
 * 읽는 자: reg_to_event() 하나뿐이며, 네 개의 배열
 * (pcie_event_to_event, sec_error_to_event, ded_error_to_event,
 * local_status_to_event)이 이 타입의 원소를 담는다.
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트에서 읽힌다(mc_get_events 경로).
 * 동기화: 전부 상수처럼 쓰이므로 락이 없다.
 */
struct event_map {
	/* [한국어] 역할: 이 항목이 대응하는 상태 레지스터 비트 마스크.
	 * 설정자: 위 네 개의 TO_EVENT_MAP 매크로가 컴파일 타임에 채운다.
	 * 읽는 자: reg_to_event() 가 (reg & reg_mask) 로 검사한다.
	 * 값 범위: 단일 비트일 수도, 여러 비트 묶음일 수도 있다(SEC/DED 는 4비트씩).
	 * 주의: DMA_END_ENGINE_0/1 과 두 doorbell 항목은 이 값이 0x00000000 이라
	 * 검사가 항상 거짓이 되어 해당 이벤트는 결코 보고되지 않는다.
	 * 동기화: 상수 배열이므로 없음. */
	u32 reg_mask;
	/* [한국어] 역할: 그 비트가 세워졌을 때 보고할 이벤트 번호(0..27).
	 * 설정자: 같은 매크로. 읽는 자: reg_to_event() 가 BIT(event_bit) 를 만든다.
	 * 값 범위: 0 이상 NUM_EVENTS(28) 미만.
	 * 동기화: 상수 배열. */
	u32 event_bit;
};


/*
 * [한국어]
 * struct mc_pcie - Microchip PolarFire PCIe 루트 포트 인스턴스
 *
 * PLDA 코어의 struct plda_pcie_rp 를 첫 필드로 감싸는 임베디드 상속 구조체다.
 * StarFive 의 struct starfive_jh7110_pcie 와 같은 관용구이지만 담는 것이 다르다 --
 * StarFive 는 전원/클럭 자원을, Microchip 은 레지스터 창 두 개를 담는다.
 * 창이 둘인 이유는 PLDA IP 의 브리지 레지스터와 Microchip 이 덧붙인 컨트롤러
 * 레지스터가 물리적으로 다른 블록이기 때문이다.
 *
 * 인스턴스: static 전역 port 하나뿐이다(아래 참조). devm_kzalloc 으로 할당되어
 * 디바이스와 수명을 같이한다.
 * container_of 로 이 구조체를 되찾는 곳: mc_get_events(), mc_ack_event_irq(),
 * mc_mask_event_irq(), mc_unmask_event_irq() 네 곳이다.
 * 실행 컨텍스트: 두 창 주소는 probe 에서 확정되고 이후 읽기 전용이라, 인터럽트
 * 컨텍스트에서 락 없이 읽어도 안전하다.
 * 동기화: 이 구조체 자체에는 락이 없고, plda.lock 이 IMASK_LOCAL 접근을 보호한다.
 */
struct mc_pcie {
	/* [한국어] 역할: PLDA 공용 코어와 주고받는 컨트롤러 상태. 반드시 첫 필드여야 한다 --
	 * mc_get_events(), mc_ack_event_irq(), mc_mask_event_irq(), mc_unmask_event_irq()
	 * 네 곳이 container_of 로 이 구조체를 되찾기 때문이다.
	 * 설정자: mc_host_probe() 가 dev/num_events/msi 를, mc_platform_init() 이
	 * event_ops/event_irq_chip/events_bitmap 을 채운다.
	 * 읽는 자: PLDA 코어 전체.
	 * 동기화: plda 내부의 lock 이 IMASK_LOCAL 접근을 보호한다. 이 파일의
	 * mc_mask_event_irq / mc_unmask_event_irq 도 그 락을 잡는다. */
	struct plda_pcie_rp plda;
	/* [한국어] 역할: PLDA 브리지 레지스터 창(ISTATUS_LOCAL, IMASK_LOCAL, ATR 테이블 등)의
	 * 커널 가상 주소.
	 * 설정자: mc_host_probe() 가 devicetree 의 'bridge' 리소스를 ioremap 하거나,
	 * 레거시 바인딩이면 'apb' 창에 MC_PCIE1_BRIDGE_ADDR(0x8000)을 더해 만든다.
	 * 읽는 자: local_events(), mc_disable_interrupts(), mc_pcie_setup_inbound_atr(),
	 * mc_platform_init(), 그리고 plda.bridge_addr 로 복사되어 코어 전체.
	 * 값 범위: 유효 __iomem 포인터.
	 * 동기화: probe 이후 읽기 전용 포인터. */
	void __iomem *bridge_base_addr;
	/* [한국어] 역할: Microchip 전용 컨트롤러 레지스터 창(PCIE_EVENT_INT, SEC/DED 오류,
	 * ECC 제어)의 커널 가상 주소. PLDA IP 바깥에 있는 블록이라 창이 별도다.
	 * 설정자: 'ctrl' 리소스 또는 'apb' + MC_PCIE1_CTRL_ADDR(0xa000).
	 * 읽는 자: pcie_events(), sec_errors(), ded_errors(), mc_disable_interrupts(),
	 * mc_clear_secs/deds(), 그리고 event_descs 의 offset 이 ISTATUS_LOCAL 이 아닌
	 * 모든 irq_chip 콜백 경로.
	 * 값 범위: 유효 __iomem 포인터.
	 * 동기화: probe 이후 읽기 전용 포인터. */
	void __iomem *ctrl_base_addr;
};

/*
 * [한국어]
 * struct cause - 이벤트 번호에 붙는 사람이 읽을 이름과 설명
 *
 * 왜 필요한가: 이 드라이버가 이벤트에 대해 실제로 하는 일은 "로그를 남기는 것"
 * 뿐이다(복구 정책이 없다). 그 로그를 쓸모 있게 만들려면 이벤트 번호가 아니라
 * 이름이 나와야 하고, /proc/interrupts 에도 이름이 보여야 한다. 그 두 문자열을
 * 담는 구조체다.
 *
 * 설정자: event_cause[] 배열의 지정 초기화자(네 개의 CAUSE 매크로).
 * 읽는 자: mc_event_handler()(str), mc_request_event_irq()(sym).
 * 실행 컨텍스트: sym 은 probe 에서, str 은 하드 인터럽트 컨텍스트에서 읽힌다.
 * 동기화: const 배열이라 없음.
 */
struct cause {
	/* [한국어] 역할: 이 이벤트의 짧은 심볼 이름. __stringify 로 매크로 인자에서 만들어진다.
	 * 설정자: event_cause[] 초기화자. 읽는 자: mc_request_event_irq() 가
	 * devm_request_irq 의 devname 으로 넘겨 /proc/interrupts 에 표시되게 한다.
	 * 값 범위: 문자열 리터럴 또는 NULL. 항목이 없는 이벤트 번호
	 * (11, 12, 18, 22, 23, 24)는 NULL 로 남으며, 그 경우 이름 없이 IRQ 가 요청된다.
	 * 동기화: const 배열. */
	const char *sym;
	/* [한국어] 역할: 사람이 읽을 오류 설명 문구.
	 * 설정자: event_cause[] 초기화자. 읽는 자: mc_event_handler() 가 오류 로그에 찍는다.
	 * 값 범위: 문자열 리터럴 또는 NULL. NULL 이면 핸들러가
	 * 'bad event IRQ %ld' 라는 대체 문구를 쓴다.
	 * 동기화: const 배열. */
	const char *str;
};

/* [한국어] 이벤트 번호 -> (심볼 이름, 설명 문구) 대응표. 크기는 NUM_EVENTS(28)이지만
 * 항목은 22개만 채워져 있다. 지정 초기화자를 쓰므로 나머지 6개
 * (11 DMA_END_ENGINE_0, 12 DMA_END_ENGINE_1, 18 A_ATR doorbell,
 * 22 P_ATR doorbell, 23 INTX, 24 MSI)는 두 필드가 모두 NULL 이다.
 * 앞 넷은 하드웨어가 구현하지 않은 이벤트라 어차피 발생하지 않고,
 * INTX/MSI 는 mc_event_handler 대신 코어의 체인 핸들러가 처리하므로
 * 설명이 필요 없다. */
static const struct cause event_cause[NUM_EVENTS] = {
	/* [한국어] 이벤트 0. sym 은 "L2_EXIT", str 은 로그 문구가 된다. */
	PCIE_EVENT_CAUSE(L2_EXIT, "L2 exit event"),
	/* [한국어] 이벤트 1 -- Hot Reset 이탈. */
	PCIE_EVENT_CAUSE(HOTRST_EXIT, "Hot reset exit event"),
	/* [한국어] 이벤트 2 -- Data Link Up 이탈. */
	PCIE_EVENT_CAUSE(DLUP_EXIT, "DLUP exit event"),
	/* [한국어] 이벤트 3 -- TX 버퍼의 정정 가능한 ECC 오류. */
	SEC_ERROR_CAUSE(TX_RAM_SEC_ERR,  "sec error in tx buffer"),
	/* [한국어] 이벤트 4 -- RX 버퍼 SEC 오류. */
	SEC_ERROR_CAUSE(RX_RAM_SEC_ERR,  "sec error in rx buffer"),
	/* [한국어] 이벤트 5 -- PCIe -> AXI 버퍼 SEC 오류. */
	SEC_ERROR_CAUSE(PCIE2AXI_RAM_SEC_ERR,  "sec error in pcie2axi buffer"),
	/* [한국어] 이벤트 6 -- AXI -> PCIe 버퍼 SEC 오류. */
	SEC_ERROR_CAUSE(AXI2PCIE_RAM_SEC_ERR,  "sec error in axi2pcie buffer"),
	/* [한국어] 이벤트 7 -- TX 버퍼의 정정 불가능한 ECC 오류. */
	DED_ERROR_CAUSE(TX_RAM_DED_ERR,  "ded error in tx buffer"),
	/* [한국어] 이벤트 8 -- RX 버퍼 DED 오류. */
	DED_ERROR_CAUSE(RX_RAM_DED_ERR,  "ded error in rx buffer"),
	/* [한국어] 이벤트 9 -- PCIe -> AXI 버퍼 DED 오류. */
	DED_ERROR_CAUSE(PCIE2AXI_RAM_DED_ERR,  "ded error in pcie2axi buffer"),
	/* [한국어] 이벤트 10 -- AXI -> PCIe 버퍼 DED 오류. 여기까지가 Microchip 전용 앞 11개다. */
	DED_ERROR_CAUSE(AXI2PCIE_RAM_DED_ERR,  "ded error in axi2pcie buffer"),
	/* [한국어] 이벤트 13 -- DMA 엔진 0 오류. 11번과 12번(DMA 완료)을 건너뛰는 것이
	 * 지정 초기화자를 쓰는 이유다. */
	LOCAL_EVENT_CAUSE(DMA_ERROR_ENGINE_0, "dma engine 0 error"),
	/* [한국어] 이벤트 14 -- DMA 엔진 1 오류. */
	LOCAL_EVENT_CAUSE(DMA_ERROR_ENGINE_1, "dma engine 1 error"),
	/* [한국어] 이벤트 15 -- AXI 쪽 쓰기 요청 오류. 여기서부터 PLDA 표준 이벤트다. */
	LOCAL_EVENT_CAUSE(A_ATR_EVT_POST_ERR, "axi write request error"),
	/* [한국어] 이벤트 16 -- AXI 쪽 읽기 요청 오류. */
	LOCAL_EVENT_CAUSE(A_ATR_EVT_FETCH_ERR, "axi read request error"),
	/* [한국어] 이벤트 17 -- AXI 쪽 읽기 타임아웃. */
	LOCAL_EVENT_CAUSE(A_ATR_EVT_DISCARD_ERR, "axi read timeout"),
	/* [한국어] 이벤트 19 -- PCIe 쪽 쓰기 요청 오류. 18번(AXI doorbell)은 건너뛴다. */
	LOCAL_EVENT_CAUSE(P_ATR_EVT_POST_ERR, "pcie write request error"),
	/* [한국어] 이벤트 20 -- PCIe 쪽 읽기 요청 오류. */
	LOCAL_EVENT_CAUSE(P_ATR_EVT_FETCH_ERR, "pcie read request error"),
	/* [한국어] 이벤트 21 -- PCIe 쪽 읽기 타임아웃. */
	LOCAL_EVENT_CAUSE(P_ATR_EVT_DISCARD_ERR, "pcie read timeout"),
	/* [한국어] 이벤트 25 -- AER 이벤트. 22(PCIe doorbell), 23(INTX), 24(MSI)는 건너뛴다. */
	LOCAL_EVENT_CAUSE(PM_MSI_INT_AER_EVT, "aer event"),
	/* [한국어] 이벤트 26 -- PM/LTR/Hotplug 이벤트. */
	LOCAL_EVENT_CAUSE(PM_MSI_INT_EVENTS, "pm/ltr/hotplug event"),
	/* [한국어] 이벤트 27 -- 시스템 오류. */
	LOCAL_EVENT_CAUSE(PM_MSI_INT_SYS_ERR, "system error"),
};

/* [한국어] PCIE_EVENT_INT(0x14c) 레지스터의 비트를 이벤트 번호로 바꾸는 표.
 * 설정자: 컴파일 타임 상수. 읽는 자: pcie_events() 의 순회 루프.
 * const 가 아닌 것은 상류 코드 그대로이며, 실제로는 수정되지 않는다. */
static struct event_map pcie_event_to_event[] = {
	/* [한국어] bit0 -> 이벤트 0. */
	PCIE_EVENT_TO_EVENT_MAP(L2_EXIT),
	/* [한국어] bit1 -> 이벤트 1. */
	PCIE_EVENT_TO_EVENT_MAP(HOTRST_EXIT),
	/* [한국어] bit2 -> 이벤트 2. */
	PCIE_EVENT_TO_EVENT_MAP(DLUP_EXIT),
};

/* [한국어] SEC_ERROR_INT(0x28) 레지스터용 변환 표. 읽는 자: sec_errors().
 * 각 항목의 마스크가 4비트 묶음이라, 네 뱅크 중 하나만 오류여도 같은 이벤트가 뜬다. */
static struct event_map sec_error_to_event[] = {
	/* [한국어] 비트 3:0 -> 이벤트 3. */
	SEC_ERROR_TO_EVENT_MAP(TX_RAM_SEC_ERR),
	/* [한국어] 비트 7:4 -> 이벤트 4. */
	SEC_ERROR_TO_EVENT_MAP(RX_RAM_SEC_ERR),
	/* [한국어] 비트 11:8 -> 이벤트 5. */
	SEC_ERROR_TO_EVENT_MAP(PCIE2AXI_RAM_SEC_ERR),
	/* [한국어] 비트 15:12 -> 이벤트 6. */
	SEC_ERROR_TO_EVENT_MAP(AXI2PCIE_RAM_SEC_ERR),
};

/* [한국어] DED_ERROR_INT(0x30) 레지스터용 변환 표. 읽는 자: ded_errors(). */
static struct event_map ded_error_to_event[] = {
	/* [한국어] 비트 3:0 -> 이벤트 7. */
	DED_ERROR_TO_EVENT_MAP(TX_RAM_DED_ERR),
	/* [한국어] 비트 7:4 -> 이벤트 8. */
	DED_ERROR_TO_EVENT_MAP(RX_RAM_DED_ERR),
	/* [한국어] 비트 11:8 -> 이벤트 9. */
	DED_ERROR_TO_EVENT_MAP(PCIE2AXI_RAM_DED_ERR),
	/* [한국어] 비트 15:12 -> 이벤트 10. */
	DED_ERROR_TO_EVENT_MAP(AXI2PCIE_RAM_DED_ERR),
};

/* [한국어] ISTATUS_LOCAL(0x184) 레지스터용 변환 표. 읽는 자: local_events().
 * 17개 항목으로 PLDA 로컬 이벤트 전부를 다룬다. StarFive 가 쓰는
 * plda_get_events() 의 비트 접기 대신 이 표로 같은 일을 명시적으로 한다 --
 * 번호 오프셋이 15 라 코어의 접기 규칙(16 기준)을 쓸 수 없기 때문이다. */
static struct event_map local_status_to_event[] = {
	/* [한국어] DMA_END_ENGINE_0_MASK 는 0x00000000 이므로 이 항목은 항상 0 을 돌려준다.
	 * 즉 이벤트 11 은 절대 보고되지 않는다. */
	LOCAL_STATUS_TO_EVENT_MAP(DMA_END_ENGINE_0),
	/* [한국어] 마찬가지로 이벤트 12 도 실효가 없다. */
	LOCAL_STATUS_TO_EVENT_MAP(DMA_END_ENGINE_1),
	/* [한국어] bit8 -> 이벤트 13(DMA 엔진 0 오류). 이쪽은 실제로 동작한다. */
	LOCAL_STATUS_TO_EVENT_MAP(DMA_ERROR_ENGINE_0),
	/* [한국어] bit9 -> 이벤트 14(DMA 엔진 1 오류). */
	LOCAL_STATUS_TO_EVENT_MAP(DMA_ERROR_ENGINE_1),
	/* [한국어] bit16 -> 이벤트 15. */
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_POST_ERR),
	/* [한국어] bit17 -> 이벤트 16. */
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_FETCH_ERR),
	/* [한국어] bit18 -> 이벤트 17. */
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_DISCARD_ERR),
	/* [한국어] A_ATR_EVT_DOORBELL_MASK 가 0 이라 이벤트 18 도 실효가 없다. */
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_DOORBELL),
	/* [한국어] bit20 -> 이벤트 19. */
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_POST_ERR),
	/* [한국어] bit21 -> 이벤트 20. */
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_FETCH_ERR),
	/* [한국어] bit22 -> 이벤트 21. */
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_DISCARD_ERR),
	/* [한국어] P_ATR_EVT_DOORBELL_MASK 가 0 이라 이벤트 22 도 실효가 없다. */
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_DOORBELL),
	/* [한국어] 비트 27:24(INTA~INTD 네 개) -> 이벤트 23 하나. StarFive 의
	 * plda_get_events() 가 하는 INTx 압축과 같은 효과를 표로 표현한 것이다. */
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_INTX),
	/* [한국어] bit28 -> 이벤트 24(MSI 묶음). */
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_MSI),
	/* [한국어] bit29 -> 이벤트 25(AER). */
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_AER_EVT),
	/* [한국어] bit30 -> 이벤트 26(PM/LTR/Hotplug). */
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_EVENTS),
	/* [한국어] bit31 -> 이벤트 27(시스템 오류). */
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_SYS_ERR),
};

/*
 * [한국어]
 * event_descs[] - 이벤트마다 "어느 레지스터의 어느 비트를 어떤 극성으로" 를 적은 표
 *
 * 이 파일에서 가장 중요한 자료구조다. PLDA 기본판은 모든 이벤트가 ISTATUS_LOCAL /
 * IMASK_LOCAL 한 쌍에 모여 있어 plda_hwirq_to_mask() 라는 계산 함수 하나로
 * ack/mask/unmask 를 다 처리할 수 있다. 그러나 Microchip 은 이벤트에 따라
 *  - 상태 레지스터가 네 종류(0x14c / 0x28 / 0x30 / 0x184),
 *  - 그 레지스터가 브리지 창에 있는지 컨트롤러 창에 있는지가 다르고,
 *  - 마스크 레지스터가 상태와 같을 수도 다를 수도 있으며,
 *  - 마스크 극성마저 반대(IMASK_LOCAL 만 1 = 허용)
 * 이라서 계산으로 풀 수 없다. 그래서 표로 서술하고, 세 개의 irq_chip 콜백이
 * 이 표를 읽어 분기한다. 익명 구조체로 선언된 것은 이 배열 말고는 이 타입을
 * 쓸 곳이 없기 때문이다.
 *
 * 배열 크기는 초기화자 개수(28)로 정해지며 NUM_EVENTS 와 일치한다 --
 * 다만 컴파일 타임에 그 일치를 강제하는 장치는 없다.
 * 설정자: 컴파일 타임 상수(PCIE_EVENT / SEC_EVENT / DED_EVENT / LOCAL_EVENT 매크로).
 * 읽는 자: mc_ack_event_irq(), mc_mask_event_irq(), mc_unmask_event_irq().
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트에서 읽힌다.
 * 동기화: const 는 아니지만 수정되지 않으므로 락이 없다.
 */
static struct {
	/* [한국어] 역할: 이 이벤트의 '상태' 레지스터 오프셋.
	 * 설정자: PCIE_EVENT/SEC_EVENT/DED_EVENT/LOCAL_EVENT 매크로.
	 * 읽는 자: mc_ack_event_irq() 가 쓸 주소로 쓰고, 세 콜백 모두 이 값이
	 * ISTATUS_LOCAL 인지로 브리지 창과 컨트롤러 창 중 어느 쪽인지 판별한다.
	 * 값 범위: PCIE_EVENT_INT, SEC_ERROR_INT, DED_ERROR_INT, ISTATUS_LOCAL 중 하나.
	 * 동기화: 상수 배열. */
	u32 offset;
	/* [한국어] 역할: 이 이벤트에 해당하는 비트 마스크.
	 * 설정자: 위 매크로들. 읽는 자: ack/mask/unmask 세 콜백 전부.
	 * 값 범위: 단일 비트 또는 4비트 묶음. 일부 항목은 0(미구현 이벤트).
	 * 동기화: 상수 배열. */
	u32 mask;
	/* [한국어] 역할: (사용되지 않는 필드) 어떤 매크로도 이 필드를 채우지 않고, 이 파일의
	 * 어떤 코드도 읽지 않는다. 지정 초기화자를 쓰므로 항상 0 이다.
	 * 상류 코드에 남아 있는 잔재로 보인다.
	 * 설정자: 없음. 읽는 자: 없음. 값 범위: 항상 0. 동기화: 해당 없음. */
	u32 shift;
	/* [한국어] 역할: 이 이벤트의 마스크 비트가 별도의 'enable' 필드에 있을 때 그 필드 마스크.
	 * PCIe 상태 전이 이벤트에서만 0 이 아니다(PCIE_EVENT_INT_ENB_MASK).
	 * 설정자: PCIE_EVENT 매크로만 값을 넣고 나머지는 0.
	 * 읽는 자: mc_ack_event_irq() 가 ack 값에 OR 하고, mask/unmask 가 '16비트 시프트가
	 * 필요한가' 를 판단하는 조건으로 쓴다.
	 * 값 범위: 0 또는 GENMASK(18, 16).
	 * 동기화: 상수 배열. */
	u32 enb_mask;
	/* [한국어] 역할: 마스크 레지스터의 극성. 1 이면 '비트를 세우면 차단', 0 이면 '비트를
	 * 세우면 허용'(즉 지워야 차단)이다.
	 * 설정자: PCIE/SEC/DED 는 1, LOCAL 은 0. 이 차이가 IMASK_LOCAL 만 반대 극성이기
	 * 때문에 생긴다.
	 * 읽는 자: mc_mask_event_irq() 와 mc_unmask_event_irq() 의 if 분기 두 곳씩.
	 * 값 범위: 0 또는 1.
	 * 동기화: 상수 배열. */
	u32 mask_high;
	/* [한국어] 역할: 이 이벤트의 '마스크' 레지스터 오프셋. 상태 레지스터와 같을 수도
	 * (PCIe 상태 전이) 다를 수도(SEC/DED/LOCAL) 있다.
	 * 설정자: 위 매크로들. 읽는 자: mask/unmask 콜백이 접근할 주소로 쓴다.
	 * 값 범위: PCIE_EVENT_INT, SEC_ERROR_INT_MASK, DED_ERROR_INT_MASK, IMASK_LOCAL.
	 * 동기화: 상수 배열. */
	u32 mask_offset;
} event_descs[] = {
	/* [한국어] 이벤트 0 의 서술자. 상태와 마스크가 같은 레지스터(0x14c)에 있고 enb_mask 를 쓴다. */
	{ PCIE_EVENT(L2_EXIT) },
	/* [한국어] 이벤트 1. */
	{ PCIE_EVENT(HOTRST_EXIT) },
	/* [한국어] 이벤트 2. */
	{ PCIE_EVENT(DLUP_EXIT) },
	/* [한국어] 이벤트 3 -- 상태 0x28, 마스크 0x2c, 1 이 차단. */
	{ SEC_EVENT(TX_RAM_SEC_ERR) },
	/* [한국어] 이벤트 4. */
	{ SEC_EVENT(RX_RAM_SEC_ERR) },
	/* [한국어] 이벤트 5. */
	{ SEC_EVENT(PCIE2AXI_RAM_SEC_ERR) },
	/* [한국어] 이벤트 6. */
	{ SEC_EVENT(AXI2PCIE_RAM_SEC_ERR) },
	/* [한국어] 이벤트 7 -- 상태 0x30, 마스크 0x34. */
	{ DED_EVENT(TX_RAM_DED_ERR) },
	/* [한국어] 이벤트 8. */
	{ DED_EVENT(RX_RAM_DED_ERR) },
	/* [한국어] 이벤트 9. */
	{ DED_EVENT(PCIE2AXI_RAM_DED_ERR) },
	/* [한국어] 이벤트 10. */
	{ DED_EVENT(AXI2PCIE_RAM_DED_ERR) },
	/* [한국어] 이벤트 11 -- 마스크 값이 0 이라 ack/mask/unmask 가 사실상 아무 비트도 건드리지
	 * 않는다(0 을 쓰거나 ~0 으로 AND 하는 것은 무해하다). */
	{ LOCAL_EVENT(DMA_END_ENGINE_0) },
	/* [한국어] 이벤트 12 -- 위와 같다. */
	{ LOCAL_EVENT(DMA_END_ENGINE_1) },
	/* [한국어] 이벤트 13 -- 상태 ISTATUS_LOCAL, 마스크 IMASK_LOCAL, 0 이 차단(극성 반대). */
	{ LOCAL_EVENT(DMA_ERROR_ENGINE_0) },
	/* [한국어] 이벤트 14. */
	{ LOCAL_EVENT(DMA_ERROR_ENGINE_1) },
	/* [한국어] 이벤트 15. */
	{ LOCAL_EVENT(A_ATR_EVT_POST_ERR) },
	/* [한국어] 이벤트 16. */
	{ LOCAL_EVENT(A_ATR_EVT_FETCH_ERR) },
	/* [한국어] 이벤트 17. */
	{ LOCAL_EVENT(A_ATR_EVT_DISCARD_ERR) },
	/* [한국어] 이벤트 18 -- 마스크 0. */
	{ LOCAL_EVENT(A_ATR_EVT_DOORBELL) },
	/* [한국어] 이벤트 19. */
	{ LOCAL_EVENT(P_ATR_EVT_POST_ERR) },
	/* [한국어] 이벤트 20. */
	{ LOCAL_EVENT(P_ATR_EVT_FETCH_ERR) },
	/* [한국어] 이벤트 21. */
	{ LOCAL_EVENT(P_ATR_EVT_DISCARD_ERR) },
	/* [한국어] 이벤트 22 -- 마스크 0. */
	{ LOCAL_EVENT(P_ATR_EVT_DOORBELL) },
	/* [한국어] 이벤트 23 -- INTx 묶음. 마스크가 4비트(0x0f000000)라 이 이벤트를 차단하면
	 * INTA~INTD 가 한꺼번에 막힌다. */
	{ LOCAL_EVENT(PM_MSI_INT_INTX) },
	/* [한국어] 이벤트 24 -- MSI 묶음. */
	{ LOCAL_EVENT(PM_MSI_INT_MSI) },
	/* [한국어] 이벤트 25 -- AER. */
	{ LOCAL_EVENT(PM_MSI_INT_AER_EVT) },
	/* [한국어] 이벤트 26 -- PM/LTR/Hotplug. */
	{ LOCAL_EVENT(PM_MSI_INT_EVENTS) },
	/* [한국어] 이벤트 27 -- 시스템 오류. */
	{ LOCAL_EVENT(PM_MSI_INT_SYS_ERR) },
};

/* [한국어] 탐색할 FPGA 패브릭 인터페이스 클럭 이름들. FIC(Fabric Interface Controller)는
 * MPFS 에서 하드 프로세서와 FPGA 패브릭을 잇는 통로이고, PCIe 블록이 패브릭 안에
 * 있으므로 그 클럭이 필요하다. 배열 폭이 5 인 것은 "fic0" 네 글자 + NUL 이기 때문이다.
 * 설정자: 컴파일 타임 상수. 읽는 자: mc_pcie_init_clks() 의 순회 루프.
 * 네 개를 모두 optional 로 찾으므로, 실제로 devicetree 에 있는 것만 켜진다. */
static char poss_clks[][5] = { "fic0", "fic1", "fic2", "fic3" };

/* [한국어] 이 드라이버의 유일한 컨트롤러 인스턴스를 담는 static 전역 포인터.
 * 설정자: mc_host_probe() 의 devm_kzalloc 결과.
 * 읽는 자: mc_platform_init() -- ECAM 코어가 부르는 훅이라 인자로 컨트롤러를
 * 받을 방법이 없어 전역에 의존한다. 이것이 전역 변수를 쓰는 이유이며,
 * 동시에 이 드라이버가 시스템에 PCIe 컨트롤러 하나만 가정하는 이유이기도 하다
 * (두 번째 컨트롤러가 probe 되면 이 포인터를 덮어쓴다).
 * 값 범위: 유효 포인터 또는 NULL(첫 probe 전).
 * 동기화: probe 는 직렬화되므로 별도 락은 없다. */
static struct mc_pcie *port;

/*
 * [한국어]
 * mc_pcie_enable_msi - 루트 포트 자신의 MSI capability 를 손으로 켠다
 *
 * @port: msi.vector_phy 가 이미 채워진 컨트롤러(mc_host_probe 가 IMSI_ADDR 을 읽어 둠).
 * @ecam: ECAM 창의 시작 주소(cfg->win). 버스 0, 장치 0, 함수 0 의 config space 가
 *        오프셋 0 부터 놓이므로, 여기에 MC_MSI_CAP_CTRL_OFFSET(0xe0)을 더하면
 *        루트 포트 자신의 MSI capability 구조체에 닿는다.
 * @return: 없음.
 *
 * 왜 필요한가: 보통 MSI capability 는 커널 PCI 코어가 장치를 열거하며 설정한다.
 * 그러나 여기 대상은 루트 포트 자신이고, MPFS 에서는 리셋 직후 이 capability 의
 * enable 비트와 주소 필드가 올바르지 않다. 그래서 열거가 시작되기 전에
 * (mc_platform_init 안에서) 드라이버가 직접 고쳐 놓는다. 상류 주석이 세 군데
 * 모두 "Fixup" 이라고 표현하는 이유가 그것이다.
 *
 * 동작 단계:
 *  1. Message Control 워드(오프셋 0x02)를 읽어 PCI_MSI_FLAGS_ENABLE(bit0)을 켜고 되쓴다.
 *  2. 같은 워드에서 QMASK(비트 3:1, Multiple Message Capable = 하드웨어가 지원하는
 *     벡터 수의 로그값)를 꺼내 QSIZE(비트 6:4, Multiple Message Enable = 실제로
 *     쓸 벡터 수의 로그값)에 그대로 넣는다. 즉 "지원하는 만큼 전부 쓴다".
 *  3. msi.vector_phy 를 Message Address(0x04)와 Message Upper Address(0x08)에
 *     하위/상위 32비트로 나눠 쓴다.
 *
 * 주의: 2번에서 reg 를 다시 읽지 않고 1번에서 ENABLE 을 켠 값을 그대로 쓰므로,
 * 두 번째 writew 에는 ENABLE 과 QSIZE 가 함께 들어간다. 결과적으로 문제는 없다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(ECAM 초기화 훅 안). 락 없음 --
 * 아직 PCI 버스가 스캔되기 전이라 이 config space 에 접근하는 다른 주체가 없다.
 * 호출자: mc_platform_init() 하나뿐.
 * 피호출자: readw_relaxed, writew_relaxed, writel_relaxed, FIELD_GET, FIELD_PREP,
 * lower_32_bits, upper_32_bits.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   mc_host_probe() -> pci_host_common_probe() -> pci_ecam_create()
 *     -> mc_platform_init() -> [이 함수]
 */
static void mc_pcie_enable_msi(struct mc_pcie *port, void __iomem *ecam)
{
	struct plda_msi *msi = &port->plda.msi;
	/* [한국어] MSI Message Control 워드(16비트)를 담을 변수. config space 오프셋 0xe0+0x02. */
	u16 reg;
	/* [한국어] Multiple Message Capable 필드에서 꺼낸 값(지원 벡터 수의 로그값, 0~5). */
	u8 queue_size;

	/* Fixup MSI enable flag */
	reg = readw_relaxed(ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);
	/* [한국어] PCI_MSI_FLAGS_ENABLE(bit0)을 세워 이 루트 포트의 MSI 기능을 켠다. */
	reg |= PCI_MSI_FLAGS_ENABLE;
	/* [한국어] 되쓴다. 리셋 직후 이 비트가 꺼져 있어 드라이버가 직접 켜야 한다. */
	writew_relaxed(reg, ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);

	/* Fixup PCI MSI queue flags */
	queue_size = FIELD_GET(PCI_MSI_FLAGS_QMASK, reg);
	/* [한국어] QMASK(비트 3:1)에서 읽은 값을 QSIZE(비트 6:4)에 넣는다. 즉 하드웨어가 지원하는
	 * 벡터 수를 그대로 '실제로 쓸 벡터 수' 로 설정한다. reg 를 다시 읽지 않았으므로
	 * 이 쓰기에는 앞서 켠 ENABLE 비트도 함께 들어간다. */
	reg |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, queue_size);
	/* [한국어] 되쓴다. */
	writew_relaxed(reg, ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);

	/* Fixup MSI addr fields */
	writel_relaxed(lower_32_bits(msi->vector_phy),
		       ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_ADDRESS_LO);
	writel_relaxed(upper_32_bits(msi->vector_phy),
		       ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_ADDRESS_HI);
}

/*
 * [한국어]
 * reg_to_event - 레지스터 값에서 한 이벤트 비트를 뽑아낸다
 *
 * @reg: 방금 읽은 상태 레지스터 값.
 * @field: 검사할 항목 -- 어떤 비트(reg_mask)가 어떤 이벤트(event_bit)인지.
 * @return: 해당 비트가 서 있으면 BIT(event_bit), 아니면 0.
 *
 * 왜 필요한가: 네 개의 이벤트 수집 함수(pcie_events, sec_errors, ded_errors,
 * local_events)가 똑같은 "표를 훑으며 OR 로 모으기" 를 하므로, 그 한 항목 처리를
 * 여기로 뺐다. inline 이라 함수 호출 비용은 없다.
 *
 * 동작: (reg & field.reg_mask) 가 참이면 BIT(field.event_bit) 를 돌려준다.
 * 마스크가 여러 비트여도(SEC/DED 의 4비트, INTx 의 4비트) 하나라도 서 있으면
 * 이벤트 하나로 보고된다 -- 이것이 "4비트를 이벤트 하나로 압축" 하는 실제 구현이다.
 * 마스크가 0 인 항목(DMA 완료, 두 도어벨)은 조건이 항상 거짓이라 0 만 돌려준다.
 * 구조체를 값으로 받는데, 8바이트라 포인터와 비용 차이가 없고 inline 되면
 * 사라진다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 순수 계산이라 재진입 안전하다.
 * 호출자: pcie_events(), sec_errors(), ded_errors(), local_events() 네 곳.
 * 피호출자: 없음.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> mc_get_events() -> pcie_events() 등 -> [이 함수]
 */
static inline u32 reg_to_event(u32 reg, struct event_map field)
{
	return (reg & field.reg_mask) ? BIT(field.event_bit) : 0;
}

/*
 * [한국어]
 * pcie_events - PCIE_EVENT_INT 레지스터에서 PCIe 상태 전이 이벤트를 모은다
 *
 * @port: ctrl_base_addr 이 매핑된 컨트롤러.
 * @return: 이벤트 0~2 에 해당하는 비트만 설 수 있는 비트맵.
 *
 * 왜 필요한가: L2 이탈, Hot Reset 이탈, DLUP 이탈은 PLDA IP 가 아니라 Microchip
 * 컨트롤러 블록이 보고하는 이벤트다. 그래서 별도 레지스터를 읽어야 한다.
 *
 * 동작: 컨트롤러 창의 PCIE_EVENT_INT(0x14c)를 한 번 읽고,
 * pcie_event_to_event[] 세 항목을 순회하며 reg_to_event 로 OR 해 모은다.
 * 레지스터를 한 번만 읽는 것이 중요하다 -- 항목마다 다시 읽으면 그 사이 상태가
 * 바뀌어 일관되지 않은 스냅숏이 된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 읽기 전용이라 락이 없다.
 * 호출자: mc_get_events() 하나뿐.
 * 피호출자: readl_relaxed, reg_to_event.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> event_ops->get_events = mc_get_events() -> [이 함수]
 */
static u32 pcie_events(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + PCIE_EVENT_INT);
	/* [한국어] OR 로 모을 결과. 0 으로 시작해야 한다. */
	u32 val = 0;
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] pcie_event_to_event[] 세 항목을 순회한다. ARRAY_SIZE 로 개수를 얻으므로
	 * 표에 항목을 추가해도 이 루프는 그대로 동작한다. */
	for (i = 0; i < ARRAY_SIZE(pcie_event_to_event); i++)
		/* [한국어] 각 항목에 대해 '그 비트가 서 있으면 그 이벤트 비트' 를 OR 한다. */
		val |= reg_to_event(reg, pcie_event_to_event[i]);

	/* [한국어] 이벤트 0~2 비트만 설 수 있는 비트맵을 돌려준다. */
	return val;
}

/*
 * [한국어]
 * sec_errors - SEC_ERROR_INT 레지스터에서 정정 가능한 ECC 오류를 모은다
 *
 * @port: ctrl_base_addr 이 매핑된 컨트롤러.
 * @return: 이벤트 3~6 에 해당하는 비트만 설 수 있는 비트맵.
 *
 * 왜 필요한가: SEC(Single Error Corrected)은 브리지 내부 버퍼 RAM 에서 1비트
 * 오류가 발생해 ECC 로 정정된 사건이다. 데이터는 무사하지만 반복되면 하드웨어
 * 열화의 신호이므로 로그로 남긴다.
 *
 * 동작: 컨트롤러 창의 SEC_ERROR_INT(0x28)를 읽고 sec_error_to_event[] 네 항목을
 * 순회한다. 각 항목의 마스크가 4비트라, 한 버퍼의 네 뱅크 중 어느 것이 오류여도
 * 같은 이벤트로 보고된다.
 *
 * 주의: mc_disable_interrupts() 가 probe 초반에 ECC 자체를 우회(bypass)시키고
 * SEC 인터럽트를 전부 마스크하므로, 이 이벤트가 실제로 올라오려면 그 뒤에
 * 누군가 마스크를 풀어야 한다. plda_init_interrupts() 가 모든 이벤트에 대해
 * IRQ 를 요청하면서 unmask 를 부르므로 실제로는 풀린다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 락 없음.
 * 호출자: mc_get_events() 하나뿐.
 * 피호출자: readl_relaxed, reg_to_event.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> mc_get_events() -> [이 함수]
 */
static u32 sec_errors(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + SEC_ERROR_INT);
	/* [한국어] OR 로 모을 결과. */
	u32 val = 0;
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] sec_error_to_event[] 네 항목을 순회한다. */
	for (i = 0; i < ARRAY_SIZE(sec_error_to_event); i++)
		/* [한국어] 각 항목의 4비트 마스크 중 하나라도 서 있으면 해당 이벤트 비트를 세운다. */
		val |= reg_to_event(reg, sec_error_to_event[i]);

	/* [한국어] 이벤트 3~6 비트만 설 수 있는 비트맵. */
	return val;
}

/*
 * [한국어]
 * ded_errors - DED_ERROR_INT 레지스터에서 정정 불가능한 ECC 오류를 모은다
 *
 * @port: ctrl_base_addr 이 매핑된 컨트롤러.
 * @return: 이벤트 7~10 에 해당하는 비트만 설 수 있는 비트맵.
 *
 * 왜 필요한가: DED(Double Error Detected)는 2비트 오류라 ECC 로 정정할 수 없다.
 * 즉 그 버퍼를 지나간 데이터가 손상되었다는 뜻이므로 SEC 보다 심각하다.
 * 다만 이 드라이버는 로그를 남길 뿐 별도 복구를 하지 않는다.
 *
 * 동작: sec_errors() 와 완전히 같은 구조이며 레지스터만 DED_ERROR_INT(0x30),
 * 표만 ded_error_to_event[] 로 다르다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 락 없음.
 * 호출자: mc_get_events() 하나뿐.
 * 피호출자: readl_relaxed, reg_to_event.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> mc_get_events() -> [이 함수]
 */
static u32 ded_errors(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + DED_ERROR_INT);
	/* [한국어] OR 로 모을 결과. */
	u32 val = 0;
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] ded_error_to_event[] 네 항목을 순회한다. */
	for (i = 0; i < ARRAY_SIZE(ded_error_to_event); i++)
		/* [한국어] 각 항목을 이벤트 비트로 바꿔 모은다. */
		val |= reg_to_event(reg, ded_error_to_event[i]);

	/* [한국어] 이벤트 7~10 비트만 설 수 있는 비트맵. */
	return val;
}

/*
 * [한국어]
 * local_events - ISTATUS_LOCAL 레지스터에서 PLDA 표준 이벤트를 모은다
 *
 * @port: bridge_base_addr 이 매핑된 컨트롤러.
 * @return: 이벤트 15~27 에 해당하는 비트가 설 수 있는 비트맵
 *          (11~14 자리는 DMA 항목이지만 완료 두 개는 마스크가 0 이라 뜨지 않는다).
 *
 * 왜 필요한가: 이 함수만 브리지 창(PLDA IP)을 읽고, 앞의 세 함수는 컨트롤러 창을
 * 읽는다. 즉 여기가 Microchip 확장과 PLDA 표준이 만나는 지점이다.
 *
 * 동작: 브리지 창의 ISTATUS_LOCAL(0x184)을 한 번 읽고 local_status_to_event[]
 * 17개 항목을 순회한다. 이 표가 StarFive 쪽 plda_get_events() 의 시프트 계산을
 * 대체하며, 특히 INTx 항목이 4비트 마스크라 네 핀을 이벤트 하나로 압축하는
 * 효과가 표 안에 들어 있다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 락 없음.
 * 호출자: mc_get_events() 하나뿐.
 * 피호출자: readl_relaxed, reg_to_event.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> mc_get_events() -> [이 함수]
 */
static u32 local_events(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->bridge_base_addr + ISTATUS_LOCAL);
	/* [한국어] OR 로 모을 결과. */
	u32 val = 0;
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] local_status_to_event[] 열일곱 항목을 순회한다. */
	for (i = 0; i < ARRAY_SIZE(local_status_to_event); i++)
		/* [한국어] 각 항목을 이벤트 비트로 바꿔 모은다. INTx 항목은 마스크가 4비트라
		 * 네 핀 중 하나만 서도 이벤트 하나로 압축된다. */
		val |= reg_to_event(reg, local_status_to_event[i]);

	/* [한국어] 이벤트 11~27 비트가 설 수 있는 비트맵(단 11, 12, 18, 22 는 마스크가 0 이라 뜨지 않는다). */
	return val;
}

/*
 * [한국어]
 * mc_get_events - 네 개의 상태 레지스터를 하나의 이벤트 비트맵으로 합친다
 *
 * @port: PLDA 코어 구조체 포인터. container_of 로 struct mc_pcie 를 되찾는다.
 * @return: 이벤트 0~27 비트맵. 코어가 이것을 events_bitmap 과 AND 해서 쓴다.
 *
 * 왜 필요한가: PLDA 코어의 plda_get_events() 는 ISTATUS_LOCAL 하나만 보고,
 * 번호 오프셋도 16 을 전제한다. Microchip 은 레지스터가 넷이고 오프셋이 15 라
 * 그 구현을 쓸 수 없다. 그래서 struct plda_event_ops.get_events 훅으로
 * 이 함수를 끼워 넣는다 -- 코어를 SoC 중립으로 유지하는 설계의 핵심 사용례다.
 *
 * 동작: 네 수집 함수를 차례로 부르고 결과를 OR 한다. 이벤트 번호 공간이 겹치지
 * 않도록 미리 배정되어 있어(0~2 PCIe, 3~6 SEC, 7~10 DED, 11~27 로컬) 단순 OR 로
 * 충돌 없이 합쳐진다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(plda_handle_event 안). 락 없음.
 * 네 레지스터를 각각 다른 시점에 읽으므로 완전한 원자적 스냅숏은 아니지만,
 * 놓친 이벤트는 상태 비트가 그대로 남아 다음 인터럽트로 다시 보고된다.
 * 호출자: plda_handle_event() 가 port->event_ops->get_events 로 간접 호출.
 * mc_event_ops 를 통해 이 함수가 연결되며, 그 연결은 mc_platform_init() 이 한다.
 * 피호출자: container_of, pcie_events, sec_errors, ded_errors, local_events.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   SoC 인터럽트 -> plda_handle_event() -> [이 함수] -> 네 수집 함수
 */
static u32 mc_get_events(struct plda_pcie_rp *port)
{
	struct mc_pcie *mc_port = container_of(port, struct mc_pcie, plda);
	/* [한국어] 네 수집 함수의 결과를 OR 로 합칠 누산기. */
	u32 events = 0;

	/* [한국어] PCIe 상태 전이 이벤트(컨트롤러 창의 0x14c). */
	events |= pcie_events(mc_port);
	/* [한국어] 정정 가능한 ECC 오류(컨트롤러 창의 0x28). */
	events |= sec_errors(mc_port);
	/* [한국어] 정정 불가능한 ECC 오류(컨트롤러 창의 0x30). */
	events |= ded_errors(mc_port);
	/* [한국어] PLDA 표준 로컬 이벤트(브리지 창의 0x184). 네 이벤트 번호 공간이 서로 겹치지
	 * 않게 미리 배정되어 있어 단순 OR 로 충돌 없이 합쳐진다. */
	events |= local_events(mc_port);

	/* [한국어] 28비트 폭의 통합 이벤트 비트맵. 코어가 events_bitmap 과 AND 해서 쓴다. */
	return events;
}

/*
 * [한국어]
 * mc_event_handler - 이벤트가 발생했음을 로그로 남기는 핸들러
 *
 * @irq: 발생한 가상 IRQ 번호. 이 번호로 hwirq(이벤트 번호)를 되찾는다.
 * @dev_id: devm_request_irq 에 넘긴 값 -- mc_request_event_irq() 가 plda 를 넘긴다.
 * @return: 항상 IRQ_HANDLED.
 *
 * 왜 필요한가: PLDA 코어의 기본 핸들러 plda_event_handler() 는 아무것도 하지 않고
 * 반환만 한다. Microchip 은 오류 이벤트가 많아 "무엇이 일어났는지" 를 남기는 것이
 * 유일한 대응책이므로 자체 핸들러를 둔다.
 *
 * 동작 단계:
 *  1. irq_domain_get_irq_data(port->event_domain, irq) 로 irq_data 를 얻는다.
 *     인자로 hwirq 가 오지 않으므로 도메인을 되짚어 찾아야 한다.
 *  2. event_cause[hwirq].str 이 있으면 그 문구를, 없으면 "bad event IRQ %ld" 를
 *     ratelimited 로 남긴다. str 이 NULL 인 이벤트 번호는 11, 12, 18, 22, 23, 24 로,
 *     앞 넷은 하드웨어가 구현하지 않아 발생하지 않고 뒤 둘(INTx/MSI)은 이 핸들러가
 *     아니라 코어의 체인 핸들러가 처리하도록 덮어써진다.
 *  3. IRQ_HANDLED 반환. IRQ_NONE 을 돌려주면 커널이 spurious 로 보고 결국 라인을
 *     비활성화하므로 반드시 HANDLED 여야 한다.
 *
 * 상태 비트 클리어는 이 함수가 아니라 mc_ack_event_irq() 가 handle_level_irq
 * 흐름 안에서 수행한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. dev_err_ratelimited 는 인터럽트에서도
 * 안전하며, 오류가 폭주해도 로그가 시스템을 마비시키지 않게 막아 준다.
 * 호출자: 커널 IRQ 코어. 등록은 mc_request_event_irq() 가 한다.
 * 피호출자: irq_domain_get_irq_data, dev_err_ratelimited.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> generic_handle_domain_irq() -> handle_level_irq()
 *     -> [이 함수]
 */
static irqreturn_t mc_event_handler(int irq, void *dev_id)
{
	struct plda_pcie_rp *port = dev_id;
	/* [한국어] 로그 대상 device. */
	struct device *dev = port->dev;
	/* [한국어] virq 로부터 얻을 irq_data. 여기에 hwirq(이벤트 번호)가 들어 있다. */
	struct irq_data *data;

	/* [한국어] 핸들러 인자로는 virq 만 오므로, 이벤트 도메인을 되짚어 irq_data 를 찾는다.
	 * 이 한 줄이 '어떤 이벤트가 발생했는가' 를 알아내는 유일한 수단이다. */
	data = irq_domain_get_irq_data(port->event_domain, irq);

	/* [한국어] 이 이벤트에 대한 설명 문구가 표에 있으면 */
	if (event_cause[data->hwirq].str)
		/* [한국어] 그 문구를 오류 로그로 남긴다. ratelimited 판이라 오류가 폭주해도
		 * 로그가 시스템을 마비시키지 않는다. */
		dev_err_ratelimited(dev, "%s\n", event_cause[data->hwirq].str);
	else
		/* [한국어] 표에 없는 이벤트 번호(11, 12, 18, 22, 23, 24)면 대체 문구를 남긴다.
		 * 앞 넷은 하드웨어가 구현하지 않아 실제로는 발생하지 않고,
		 * 뒤 둘(INTx/MSI)은 코어의 체인 핸들러로 덮어써져 이 함수에 오지 않는다. */
		dev_err_ratelimited(dev, "bad event IRQ %ld\n", data->hwirq);

	return IRQ_HANDLED;
}

/*
 * [한국어]
 * mc_ack_event_irq - event_descs[] 를 보고 해당 이벤트의 상태 비트를 지운다
 *
 * @data: 이벤트 irq_data. hwirq 가 이벤트 번호, chip_data 가 struct plda_pcie_rp
 *        (plda_pcie_event_map 이 심어 둔 값).
 * @return: 없음.
 *
 * 왜 필요한가: PLDA 코어의 plda_ack_event_irq() 는 항상 ISTATUS_LOCAL 에 쓴다.
 * Microchip 은 이벤트마다 상태 레지스터가 다르므로 표를 보고 주소를 골라야 한다.
 *
 * 동작 단계:
 *  1. container_of 로 struct mc_pcie 를 얻는다.
 *  2. event_descs[event].offset 이 ISTATUS_LOCAL 이면 브리지 창을, 아니면
 *     컨트롤러 창을 기준 주소로 고른다. 이 한 줄이 "두 개의 레지스터 창" 구조를
 *     흡수하는 지점이다.
 *  3. 기준 주소에 offset 을 더한다.
 *  4. 쓸 값은 mask 와 enb_mask 의 OR 다. PLDA 로컬/SEC/DED 이벤트는 enb_mask 가
 *     0 이라 순수한 상태 비트 클리어(write-1-to-clear)가 된다.
 *     PCIe 상태 전이 이벤트만 enb_mask 가 PCIE_EVENT_INT_ENB_MASK(비트 18:16)라,
 *     이 쓰기가 상태 비트를 지우는 동시에 세 개의 마스크 비트를 모두 1 로 만든다.
 *     mc_disable_interrupts() 가 같은 레지스터에 상태 비트와 마스크 비트를 함께
 *     써서 "끄고 지운다" 고 주석을 달아 둔 것으로 보아 마스크 비트 1 은 차단을
 *     뜻하며, 따라서 이 ack 는 나머지 두 PCIe 이벤트까지 차단하는 부작용을 갖는다.
 *     그것이 의도된 동작인지는 데이터시트가 없어 확정할 수 없으므로 사실만 적어 둔다.
 *  5. writel_relaxed 로 한 번 쓴다. 읽지 않으므로 락이 필요 없다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(handle_level_irq 내부).
 * 호출자: 커널 IRQ 코어가 mc_event_irq_chip.irq_ack 로 간접 호출.
 * 피호출자: container_of, writel_relaxed.
 * 에러 경로: 없음. hwirq 범위 검사는 없으나 도메인 크기가 NUM_EVENTS 라 안전하다.
 *
 * 호출 체인:
 *   plda_handle_event() -> handle_level_irq() -> chip->irq_ack = [이 함수]
 */
static void mc_ack_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] 코어 시점 포인터에서 Microchip 구조체를 되찾는다. plda 가 첫 필드라 주소는
	 * 같지만 타입 안전을 위해 매크로를 쓴다. */
	struct mc_pcie *mc_port = container_of(port, struct mc_pcie, plda);
	/* [한국어] 이벤트 번호. event_descs[] 의 인덱스로 쓰인다. */
	u32 event = data->hwirq;
	/* [한국어] 쓸 레지스터의 최종 주소. */
	void __iomem *addr;
	/* [한국어] 쓸 값(상태 비트 + enb_mask). */
	u32 mask;

	/* [한국어] 이 이벤트의 상태 레지스터가 PLDA 브리지 쪽인지 판별한다. */
	if (event_descs[event].offset == ISTATUS_LOCAL)
		/* [한국어] ISTATUS_LOCAL 이면 브리지 창을 기준으로 삼는다. */
		addr = mc_port->bridge_base_addr;
	else
		/* [한국어] 아니면(PCIE_EVENT_INT / SEC_ERROR_INT / DED_ERROR_INT) Microchip 컨트롤러 창이다.
		 * 이 두 줄이 '레지스터 창이 둘' 이라는 구조를 흡수하는 지점이다. */
		addr = mc_port->ctrl_base_addr;

	/* [한국어] ack 는 '상태' 레지스터에 쓰므로 mask_offset 이 아니라 offset 을 더한다. */
	addr += event_descs[event].offset;
	/* [한국어] 이 이벤트의 상태 비트 마스크. */
	mask = event_descs[event].mask;
	/* [한국어] enb_mask 를 OR 한다. PLDA 로컬/SEC/DED 는 이 값이 0 이라 순수한 상태 클리어지만,
	 * PCIe 상태 전이 이벤트는 비트 18:16 전체가 함께 들어간다. 그 세 비트는
	 * mc_disable_interrupts() 의 용례로 보아 '1 = 차단' 이므로, 이 ack 는 나머지 두
	 * PCIe 이벤트까지 차단하게 된다. 의도된 동작인지는 데이터시트 없이 판단할 수 없다. */
	mask |= event_descs[event].enb_mask;

	/* [한국어] 한 번 쓴다. 읽고 고쳐 쓰는 것이 아니므로 락이 필요 없다 --
	 * 상태 레지스터가 write-1-to-clear 라 0 을 쓴 비트는 영향을 받지 않기 때문이다. */
	writel_relaxed(mask, addr);
}

/*
 * [한국어]
 * mc_mask_event_irq - event_descs[] 를 보고 해당 이벤트를 마스크한다
 *
 * @data: 이벤트 irq_data(hwirq = 이벤트 번호, chip_data = 컨트롤러).
 * @return: 없음.
 *
 * 왜 필요한가: handle_level_irq 는 핸들러 실행 전에 소스를 마스크한다. Microchip 은
 * 마스크 레지스터의 위치와 극성이 이벤트마다 달라 표를 봐야 한다.
 *
 * 동작 단계:
 *  1. container_of 로 struct mc_pcie 를 얻는다.
 *  2. offset 으로 브리지 창/컨트롤러 창을 고르고, 거기에 "mask_offset" 을 더한다.
 *     ack 와 달리 offset 이 아니라 mask_offset 을 더한다는 점이 핵심이다 --
 *     SEC/DED 는 상태와 마스크가 다른 레지스터이기 때문이다.
 *     (창 선택 자체는 여전히 offset 으로 판단한다. 두 레지스터가 같은 창에 있으니
 *      결과는 맞는다.)
 *  3. enb_mask 가 0 이 아니면(= PCIe 상태 전이 이벤트) 마스크를 16비트 왼쪽으로
 *     밀어 상태 비트 위치(2:0)를 마스크 비트 위치(18:16)로 옮기고, ENB_MASK 로
 *     잘라 그 필드 밖을 건드리지 않게 한다.
 *  4. mask_high 가 0 이면(= IMASK_LOCAL, 1 이 허용) 마스크를 비트 반전한다.
 *     이후 AND 연산으로 해당 비트를 지우기 위한 준비다.
 *  5. port->lock 을 잡는다 -- IMASK_LOCAL 은 PLDA 코어의 INTx mask/unmask 와
 *     공유하는 레지스터라 read-modify-write 가 겹치면 갱신이 유실된다.
 *     irqsave 가 아닌 것은 이 콜백이 인터럽트가 이미 막힌 경로에서만 불린다는
 *     전제 때문이며, PLDA 코어의 plda_mask_event_irq() 도 같은 선택을 했다.
 *  6. 읽고, mask_high 면 OR(1 이 차단), 아니면 AND(0 이 차단)로 고쳐 되쓴다.
 *  7. 락 해제.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트.
 * 호출자: 커널 IRQ 코어가 irq_mask 로 간접 호출.
 * 피호출자: container_of, raw_spin_lock/unlock, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_level_irq() -> chip->irq_mask = [이 함수]
 */
static void mc_mask_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] Microchip 구조체를 되찾는다. */
	struct mc_pcie *mc_port = container_of(port, struct mc_pcie, plda);
	/* [한국어] 이벤트 번호. */
	u32 event = data->hwirq;
	/* [한국어] 조작할 마스크 레지스터의 최종 주소. */
	void __iomem *addr;
	/* [한국어] 이 이벤트에 해당하는 비트 마스크(가공 전). */
	u32 mask;
	/* [한국어] read-modify-write 임시 변수. */
	u32 val;

	/* [한국어] 상태 레지스터 오프셋으로 어느 창인지 판별한다. 마스크 레지스터도 같은 창에
	 * 있으므로 이 판별로 충분하다. */
	if (event_descs[event].offset == ISTATUS_LOCAL)
		/* [한국어] 브리지 창. */
		addr = mc_port->bridge_base_addr;
	else
		/* [한국어] 컨트롤러 창. */
		addr = mc_port->ctrl_base_addr;

	/* [한국어] ack 와 달리 '마스크' 레지스터 오프셋을 더한다. SEC/DED 는 상태(0x28/0x30)와
	 * 마스크(0x2c/0x34)가 다른 레지스터이기 때문이다. */
	addr += event_descs[event].mask_offset;
	/* [한국어] 표에서 이 이벤트의 비트 마스크를 가져온다. */
	mask = event_descs[event].mask;
	/* [한국어] enb_mask 가 있는 경우 -- PCIe 상태 전이 이벤트뿐이다. */
	if (event_descs[event].enb_mask) {
		/* [한국어] 상태 비트 위치(2:0)를 마스크 비트 위치(18:16)로 옮긴다. */
		mask <<= PCIE_EVENT_INT_ENB_SHIFT;
		/* [한국어] ENB_MASK 로 잘라 그 필드 밖(상태 비트 등)을 건드리지 않게 한다. */
		mask &= PCIE_EVENT_INT_ENB_MASK;
	}

	/* [한국어] mask_high 가 0 이면 IMASK_LOCAL 이라 극성이 반대다(1 = 허용). */
	if (!event_descs[event].mask_high)
		/* [한국어] 비트를 반전한다. 아래 AND 연산으로 해당 비트만 지우기 위한 준비다. */
		mask = ~mask;

	raw_spin_lock(&port->lock);
	/* [한국어] 현재 마스크 레지스터 값을 읽는다. 여기서부터 락 안이다 -- IMASK_LOCAL 은
	 * PLDA 코어의 INTx mask/unmask 와 공유하는 레지스터라 갱신이 겹치면 유실된다.
	 * irqsave 가 아닌 것은 이 콜백이 인터럽트가 이미 막힌 경로에서만 불린다는 전제다. */
	val = readl_relaxed(addr);
	/* [한국어] 1 이 차단인 레지스터(PCIE_EVENT_INT, SEC/DED 마스크)면 */
	if (event_descs[event].mask_high)
		/* [한국어] 비트를 세워 차단한다. */
		val |= mask;
	else
		/* [한국어] 0 이 차단인 IMASK_LOCAL 이면 반전된 마스크와 AND 해 비트를 지운다. */
		val &= mask;

	/* [한국어] 되쓴다. */
	writel_relaxed(val, addr);
	raw_spin_unlock(&port->lock);
}

/*
 * [한국어]
 * mc_unmask_event_irq - event_descs[] 를 보고 해당 이벤트의 마스크를 푼다
 *
 * @data: 이벤트 irq_data(hwirq = 이벤트 번호, chip_data = 컨트롤러).
 * @return: 없음.
 *
 * 왜 필요한가: mask 의 짝이다. 최초 request_irq 시점에도 이 경로로 이벤트가 처음
 * 켜지므로, mc_disable_interrupts() 가 probe 초반에 전부 막아 둔 것을
 * plda_init_interrupts() 의 IRQ 요청이 다시 여는 구조다.
 *
 * 동작 단계(mask 판과 비트 연산 순서가 미묘하게 다르다):
 *  1~2. container_of 와 창/주소 선택은 mask 판과 동일하다.
 *  3. enb_mask 가 있으면 먼저 16비트 왼쪽으로 민다.
 *  4. mask_high 가 1 이면 반전한다. mask 판은 mask_high 가 0 일 때 반전했는데,
 *     여기서는 반대다 -- 마스크를 "푸는" 동작이므로 극성이 뒤집히기 때문이다.
 *  5. enb_mask 가 있으면 그때서야 ENB_MASK 로 자른다. 4번의 반전 뒤에 자르는
 *     순서라, 결과 값은 비트 18:16 만 남고 나머지는 0 이 된다. 그 값을 6번에서
 *     AND 하므로, PCIe 상태 전이 이벤트의 unmask 는 이 레지스터에서 마스크 필드
 *     세 비트 중 이 이벤트의 것만 0 으로 만들고 나머지 필드(상태 비트 포함)를
 *     0 으로 쓴다. 상태 비트는 write-1-to-clear 라 0 을 써도 지워지지 않으므로
 *     실질적인 손상은 없다.
 *  6. 락을 잡고 읽어, mask_high 면 AND, 아니면 OR 로 고쳐 되쓴다.
 *     mask 판과 정확히 반대의 연산이다.
 *  7. 락 해제.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트 또는 request_irq 경로(프로세스 컨텍스트).
 * 호출자: 커널 IRQ 코어가 irq_unmask 로 간접 호출.
 * 피호출자: container_of, raw_spin_lock/unlock, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   request_irq() 또는 handle_level_irq() 종료 -> chip->irq_unmask = [이 함수]
 */
static void mc_unmask_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] Microchip 구조체를 되찾는다. */
	struct mc_pcie *mc_port = container_of(port, struct mc_pcie, plda);
	/* [한국어] 이벤트 번호. */
	u32 event = data->hwirq;
	/* [한국어] 조작할 마스크 레지스터의 최종 주소. */
	void __iomem *addr;
	/* [한국어] 비트 마스크(가공 전). */
	u32 mask;
	/* [한국어] read-modify-write 임시 변수. */
	u32 val;

	/* [한국어] 어느 창인지 판별한다. */
	if (event_descs[event].offset == ISTATUS_LOCAL)
		/* [한국어] 브리지 창. */
		addr = mc_port->bridge_base_addr;
	else
		/* [한국어] 컨트롤러 창. */
		addr = mc_port->ctrl_base_addr;

	/* [한국어] 마스크 레지스터 오프셋을 더한다. */
	addr += event_descs[event].mask_offset;
	/* [한국어] 표에서 비트 마스크를 가져온다. */
	mask = event_descs[event].mask;

	/* [한국어] PCIe 상태 전이 이벤트면 */
	if (event_descs[event].enb_mask)
		/* [한국어] 마스크 비트 위치로 16비트 민다. */
		mask <<= PCIE_EVENT_INT_ENB_SHIFT;

	/* [한국어] mask_high 가 1 이면(1 = 차단) 마스크를 푸는 동작이므로 반전한다.
	 * mask 판에서는 mask_high 가 0 일 때 반전했는데, 여기서는 조건이 반대인 것이
	 * '푼다' 는 동작의 대칭성 때문이다. */
	if (event_descs[event].mask_high)
		/* [한국어] 비트 반전. */
		mask = ~mask;

	/* [한국어] PCIe 상태 전이 이벤트면 */
	if (event_descs[event].enb_mask)
		/* [한국어] 반전 뒤에 ENB_MASK 로 자른다. 그 결과 값은 비트 18:16 중 이 이벤트의 것만 0 이고
		 * 나머지 두 비트는 1, 그 밖의 모든 비트는 0 이 된다. 아래 AND 연산이 그 값을 쓰므로
		 * 상태 비트(2:0)에는 0 이 쓰이는데, write-1-to-clear 라 실질적인 손상은 없다. */
		mask &= PCIE_EVENT_INT_ENB_MASK;

	raw_spin_lock(&port->lock);
	/* [한국어] 현재 값을 읽는다. 락 안이다. */
	val = readl_relaxed(addr);
	/* [한국어] 1 이 차단인 레지스터면 */
	if (event_descs[event].mask_high)
		/* [한국어] 반전된 마스크와 AND 해 그 비트만 0(허용)으로 만든다. */
		val &= mask;
	else
		/* [한국어] 0 이 차단인 IMASK_LOCAL 이면 OR 로 비트를 세워 허용한다. */
		val |= mask;
	/* [한국어] 되쓴다. 이 순간부터 해당 이벤트가 CPU 로 올라온다. */
	writel_relaxed(val, addr);
	raw_spin_unlock(&port->lock);
}

/*
 * [한국어] Microchip 이벤트 계층의 irq_chip -- PLDA 코어 기본판을 대체한다.
 *
 * 코어의 plda_event_irq_chip 은 세 콜백 모두 plda_hwirq_to_mask() 로 계산해
 * ISTATUS_LOCAL/IMASK_LOCAL 만 건드린다. Microchip 은 레지스터가 넷이고 마스크
 * 극성도 두 가지라 그 방식이 통하지 않아 event_descs[] 표를 보는 자체 구현을 쓴다.
 * 이 chip 이 꽂히는 경로: mc_platform_init() 이 plda.event_irq_chip 에 대입하고,
 * plda_init_interrupts() 가 그것이 비어 있지 않으므로 기본값을 넣지 않으며,
 * plda_pcie_event_map() 이 이벤트 virq 마다 이 chip 을 붙인다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: 위 경로.
 * 동기화: 상수 테이블이지만 mask/unmask 콜백 내부에서 plda.lock 을 잡는다.
 */
static struct irq_chip mc_event_irq_chip = {
	.name = "Microchip PCIe EVENT",
	.irq_ack = mc_ack_event_irq,
	.irq_mask = mc_mask_event_irq,
	.irq_unmask = mc_unmask_event_irq,
};

/*
 * [한국어]
 * mc_pcie_deinit_clk - devm 정리 콜백. 클럭 하나를 끈다
 *
 * @data: devm_add_action_or_reset 에 등록할 때 넘긴 struct clk 포인터.
 *        시그니처가 void 포인터인 것은 devm 콜백 규약 때문이다.
 * @return: 없음.
 *
 * 왜 필요한가: mc_pcie_init_clk() 이 켠 클럭을 디바이스 해제 시 자동으로 끄기
 * 위한 것이다. devm 액션으로 등록해 두면 probe 실패든 정상 해제든 커널이
 * 알아서 불러 주므로, 드라이버가 해제 경로를 직접 관리하지 않아도 된다.
 * 이 드라이버에 remove 함수가 없는 이유이기도 하다.
 *
 * 동작: void 포인터를 struct clk 로 되돌려 clk_disable_unprepare 를 부른다.
 *
 * 실행 컨텍스트: 디바이스 해제 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: devm 코어(devres 해제 시). 등록은 mc_pcie_init_clk() 이 한다.
 * 피호출자: clk_disable_unprepare.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   디바이스 해제 -> devres 정리 -> [이 함수]
 */
static inline void mc_pcie_deinit_clk(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
}

/*
 * [한국어]
 * mc_pcie_init_clk - 이름으로 클럭을 찾아 켜고, 해제 액션까지 등록한다
 *
 * @dev: 클럭을 찾을 기준 device.
 * @id: devicetree 의 clock-names 에서 찾을 이름("fic0" ~ "fic3").
 * @return: 성공하면 struct clk 포인터(그 클럭이 없으면 NULL), 실패하면 ERR_PTR.
 *          호출자는 IS_ERR 로만 검사하고 NULL 은 성공으로 취급한다.
 *
 * 왜 필요한가: MPFS 에서 PCIe 블록은 FPGA 패브릭 안에 있고, 패브릭과 하드
 * 프로세서를 잇는 FIC 는 설계에 따라 1~4개가 쓰인다. 어느 것이 쓰이는지는
 * 비트파일마다 다르므로 네 개를 모두 optional 로 찾아보는 방식을 쓴다.
 *
 * 동작 단계:
 *  1. devm_clk_get_optional 로 클럭을 찾는다. 없으면 NULL, 오류면 ERR_PTR.
 *  2. ERR_PTR 이면 그대로 반환(호출자가 실패로 처리).
 *  3. NULL 이면 그대로 반환 -- 이 설계에 그 FIC 가 없다는 뜻이라 정상이다.
 *  4. clk_prepare_enable 로 켠다. 실패하면 ERR_PTR 로 감싸 반환한다.
 *  5. devm_add_action_or_reset 으로 해제 콜백을 등록한다. 이 등록이 실패하면
 *     커널이 즉시 콜백을 불러 클럭을 되돌리므로(_or_reset 의 의미) 누수가 없다.
 *     다만 이 함수는 그 반환값을 검사하지 않고 clk 을 그대로 돌려준다 --
 *     등록 실패 시 이미 꺼진 클럭 핸들이 성공으로 보고되는 셈이다.
 *     코드가 그렇게 되어 있다는 사실만 적어 둔다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: mc_pcie_init_clks() 의 순회 루프 하나뿐.
 * 피호출자: devm_clk_get_optional, clk_prepare_enable, devm_add_action_or_reset.
 * 에러 경로: 위 2번과 4번.
 *
 * 호출 체인:
 *   mc_host_probe() -> mc_pcie_init_clks() -> [이 함수] (이름마다 1회)
 */
static inline struct clk *mc_pcie_init_clk(struct device *dev, const char *id)
{
	struct clk *clk;
	/* [한국어] clk_prepare_enable 의 반환값. */
	int ret;

	/* [한국어] 이름으로 클럭을 찾는다. optional 판이라 devicetree 에 없으면 오류가 아니라
	 * NULL 을 돌려준다. */
	clk = devm_clk_get_optional(dev, id);
	/* [한국어] 오류 포인터면 */
	if (IS_ERR(clk))
		/* [한국어] 그대로 돌려준다. 호출자가 IS_ERR 로 검사해 probe 를 접는다. */
		return clk;
	/* [한국어] NULL 이면 이 설계에 그 FIC 클럭이 없다는 뜻이므로 */
	if (!clk)
		/* [한국어] 그대로 돌려준다. 호출자는 NULL 을 성공으로 취급한다. */
		return clk;

	/* [한국어] 찾은 클럭을 준비하고 켠다. */
	ret = clk_prepare_enable(clk);
	/* [한국어] 실패하면 */
	if (ret)
		return ERR_PTR(ret);

	/* [한국어] 디바이스 해제 시 이 클럭을 끌 콜백을 등록한다. _or_reset 판이라 등록 자체가
	 * 실패하면 커널이 즉시 콜백을 불러 클럭을 되돌린다. 다만 이 함수는 반환값을
	 * 검사하지 않아, 그 경우 이미 꺼진 클럭 핸들이 성공으로 보고된다. */
	devm_add_action_or_reset(dev, mc_pcie_deinit_clk, clk);

	/* [한국어] 켜진 클럭 핸들을 돌려준다. 호출자는 값 자체를 쓰지 않고 오류 여부만 본다. */
	return clk;
}

/*
 * [한국어]
 * mc_pcie_init_clks - fic0~fic3 네 개의 패브릭 클럭을 모두 시도해 켠다
 *
 * @dev: 클럭을 찾을 기준 device.
 * @return: 0 성공(하나도 없어도 성공), 음수 errno 실패.
 *
 * 왜 필요한가: 위 영문 주석대로 PCIe 블록이 Fabric Interface 를 통해 1~4개의
 * 클럭을 받을 수 있고, 어느 것이 연결되는지는 FPGA 설계마다 다르다. 그래서
 * devicetree 를 뒤져 있는 것만 켜는 방식을 쓴다.
 *
 * 동작: poss_clks[] 네 이름을 순회하며 mc_pcie_init_clk 을 부르고, ERR_PTR 이면
 * 그 오류를 반환한다. NULL(그 클럭 없음)은 오류가 아니므로 계속 진행한다.
 * 이미 켠 클럭을 되돌리는 코드가 없는데, devm 액션으로 등록해 두었기 때문에
 * probe 실패 시 커널이 자동으로 되돌린다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: mc_host_probe() 하나뿐. 호출 위치가 레지스터 창 설정 뒤인데,
 * 클럭 없이 레지스터를 건드리는 셈이라 순서가 특이하다. MPFS 에서는 부트로더가
 * 이미 필요한 클럭을 켜 두었을 가능성이 있으나 코드에 근거는 없다.
 * 피호출자: mc_pcie_init_clk.
 * 에러 경로: 첫 실패에서 즉시 반환. 호출자는 그것을 -ENODEV 로 바꿔 버린다.
 *
 * 호출 체인:
 *   mc_host_probe() -> [이 함수] -> mc_pcie_init_clk() x4
 */
static int mc_pcie_init_clks(struct device *dev)
{
	int i;
	/* [한국어] 각 시도의 결과를 받을 임시 포인터. FIC = Fabric Interface Controller. */
	struct clk *fic;

	/*
	 * PCIe may be clocked via Fabric Interface using between 1 and 4
	 * clocks. Scan DT for clocks and enable them if present
	 */
	for (i = 0; i < ARRAY_SIZE(poss_clks); i++) {
		/* [한국어] "fic0" ~ "fic3" 을 차례로 시도한다. 있는 것만 켜지고 없는 것은 조용히 넘어간다. */
		fic = mc_pcie_init_clk(dev, poss_clks[i]);
		/* [한국어] 오류면 그 값을 그대로 반환한다. 이미 켠 클럭은 devm 액션이 자동으로 되돌린다. */
		if (IS_ERR(fic))
			return PTR_ERR(fic);
	}

	return 0;
}

/*
 * [한국어]
 * mc_request_event_irq - 이벤트 IRQ 를 이름과 함께 등록하는 SoC 훅
 *
 * @plda: 컨트롤러(코어 시점). devm 의 기준 device 와 핸들러 인자로 쓰인다.
 * @event_irq: irq_create_mapping 이 돌려준 가상 IRQ 번호.
 * @event: 이벤트 번호. event_cause[] 에서 이름을 찾는 데 쓴다.
 * @return: devm_request_irq 의 반환값. 0 이 성공.
 *
 * 왜 필요한가: PLDA 코어의 기본 등록은 devname 에 NULL 을 넘겨
 * /proc/interrupts 에 이름이 나오지 않는다. 이벤트가 28개나 되는 Microchip
 * 에서는 어느 줄이 무슨 이벤트인지 구별할 수 없으면 곤란하므로, 이름을 붙이기
 * 위해 struct plda_event.request_event_irq 훅을 채운다. 이 훅의 존재 이유가
 * 사실상 이 한 가지다.
 *
 * 동작: devm_request_irq(dev, event_irq, mc_event_handler, 0,
 * event_cause[event].sym, plda). 플래그 0 은 공유하지 않는 일반 인터럽트라는 뜻이고,
 * 마지막 plda 가 핸들러의 dev_id 로 전달되어 mc_event_handler 가 event_domain 을
 * 찾는 데 쓰인다.
 * 주의: event_cause[event].sym 은 이벤트 11, 12, 18, 22, 23, 24 에 대해 NULL 이다.
 * 그 경우 이름 없이 등록된다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(plda_init_interrupts 의 루프 안).
 * 호출자: plda_init_interrupts() 가 event->request_event_irq 로 간접 호출.
 * 피호출자: devm_request_irq.
 * 에러 경로: 실패하면 코어가 그 값을 그대로 올려보내 probe 가 중단된다.
 *
 * 호출 체인:
 *   mc_platform_init() -> plda_init_interrupts() -> event->request_event_irq
 *     = [이 함수]
 */
static int mc_request_event_irq(struct plda_pcie_rp *plda, int event_irq,
				int event)
{
	return devm_request_irq(plda->dev, event_irq, mc_event_handler,
				0, event_cause[event].sym, plda);
}

/*
 * [한국어] Microchip 이벤트 수집 ops -- 코어의 plda_event_ops 를 대체한다.
 *
 * get_events 하나만 채우며, 그 구현 mc_get_events() 가 네 레지스터를 합친다.
 * 설정자: 컴파일 타임 상수. 읽는 자: mc_platform_init() 이 plda.event_ops 에
 * 대입하고, plda_handle_event() 가 매 인터럽트마다 역참조한다.
 * 동기화: 상수.
 */
static const struct plda_event_ops mc_event_ops = {
	.get_events = mc_get_events,
};

/*
 * [한국어] 코어에 넘기는 "이벤트 번호 규약 + IRQ 요청 방식".
 *
 *  - request_event_irq = mc_request_event_irq : 이벤트 이름을 /proc/interrupts 에
 *    남기기 위한 자체 등록 함수. StarFive 의 stf_pcie_event 는 이 필드를 비워 둔다.
 *  - intx_event = EVENT_LOCAL_PM_MSI_INT_INTX = 15 + PLDA_INTX(8) = 23
 *  - msi_event  = EVENT_LOCAL_PM_MSI_INT_MSI  = 15 + PLDA_MSI(9)  = 24
 * StarFive 의 24/25 와 한 칸씩 다른데, 앞에 놓인 SoC 전용 이벤트 개수가
 * 15 대 16 으로 다르기 때문이다. 이 구조체가 존재하는 이유가 정확히 이 차이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_init_interrupts() 가 그 자리에서만 쓴다.
 * 동기화: 상수.
 */
static const struct plda_event mc_event = {
	.request_event_irq = mc_request_event_irq,
	.intx_event        = EVENT_LOCAL_PM_MSI_INT_INTX,
	.msi_event         = EVENT_LOCAL_PM_MSI_INT_MSI,
};

/*
 * [한국어]
 * mc_clear_secs - SEC 오류 상태와 카운터를 모두 지운다
 *
 * @port: ctrl_base_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 부팅 시점에 이전 실행이나 리셋 과정에서 쌓인 ECC 오류 기록이
 * 남아 있을 수 있다. 그대로 두면 인터럽트를 켜는 순간 가짜 오류가 보고되므로
 * 깨끗이 지우고 시작한다.
 *
 * 동작: SEC_ERROR_INT(0x28)에 16비트 전체(SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT)를
 * 써서 상태를 지우고(write-1-to-clear), SEC_ERROR_EVENT_CNT(0x20)에 0 을 써서
 * 누적 카운터를 초기화한다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음(아직 인터럽트가 없다).
 * 호출자: mc_disable_interrupts() 하나뿐.
 * 피호출자: writel_relaxed 두 번.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   mc_host_probe() -> mc_disable_interrupts() -> [이 함수]
 */
static inline void mc_clear_secs(struct mc_pcie *port)
{
	writel_relaxed(SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT,
		       port->ctrl_base_addr + SEC_ERROR_INT);
	/* [한국어] 누적 SEC 오류 카운터를 0 으로 초기화한다. 상태 비트만 지우고 카운터를 두면
	 * 다음 진단 때 이전 값이 섞이기 때문이다. */
	writel_relaxed(0, port->ctrl_base_addr + SEC_ERROR_EVENT_CNT);
}

/*
 * [한국어]
 * mc_clear_deds - DED 오류 상태와 카운터를 모두 지운다
 *
 * @port: ctrl_base_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: mc_clear_secs() 와 같은 이유이며 대상만 DED 쪽이다.
 *
 * 동작: DED_ERROR_INT(0x30)에 16비트 전체를 써서 상태를 지우고,
 * DED_ERROR_EVENT_CNT(0x24)에 0 을 써서 카운터를 초기화한다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: mc_disable_interrupts() 하나뿐.
 * 피호출자: writel_relaxed 두 번.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   mc_host_probe() -> mc_disable_interrupts() -> [이 함수]
 */
static inline void mc_clear_deds(struct mc_pcie *port)
{
	writel_relaxed(DED_ERROR_INT_ALL_RAM_DED_ERR_INT,
		       port->ctrl_base_addr + DED_ERROR_INT);
	/* [한국어] 누적 DED 오류 카운터를 0 으로 초기화한다. */
	writel_relaxed(0, port->ctrl_base_addr + DED_ERROR_EVENT_CNT);
}

/*
 * [한국어]
 * mc_disable_interrupts - 모든 인터럽트 소스를 끄고 잔여 상태를 전부 지운다
 *
 * @port: 두 레지스터 창이 모두 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: probe 초반, 아직 irq_domain 도 핸들러도 없는 상태에서 인터럽트가
 * 들어오면 커널이 처리할 방법이 없다(spurious 로 라인이 꺼지거나 최악의 경우
 * NULL 역참조). 그래서 레지스터 창을 잡자마자 "전부 끄고 전부 지운다" 를
 * 실행한다. 이 함수가 끝난 뒤에야 나중에 plda_init_interrupts() 의 unmask 가
 * 필요한 것만 다시 연다.
 *
 * 동작 단계(다섯 덩어리):
 *  1. ECC 우회 비트 네 개를 세운다. ECC 검사 자체를 끄는 것으로, SEC/DED 오류가
 *     아예 발생하지 않게 만든다.
 *  2. SEC 마스크 레지스터에 16비트 전체를 써서 모두 차단하고(1 이 차단),
 *     mc_clear_secs() 로 상태와 카운터를 지운다.
 *  3. DED 도 같은 방식으로 처리한다.
 *  4. IMASK_LOCAL 에 0 을 써서 모든 로컬 이벤트를 차단하고(여기는 0 이 차단),
 *     ISTATUS_LOCAL 과 ISTATUS_MSI 에 32비트 전체를 써서 잔여 상태를 지운다.
 *  5. PCIE_EVENT_INT 에 상태 비트 세 개와 마스크 비트 세 개를 함께 써서
 *     PCIe 상태 전이 이벤트를 끄고 지운다. 이 한 줄이 "마스크 비트에 1 을 쓰면
 *     차단" 이라는 극성의 근거다.
 *  6. IMASK_HOST 에 0, ISTATUS_HOST 에 32비트 전체를 써서 호스트 인터럽트도
 *     끄고 지운다. 이 두 레지스터는 이 트리에서 여기서만 쓰인다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락이 없는데, 아직 인터럽트 핸들러가
 * 하나도 등록되지 않아 경쟁자가 없기 때문이다.
 * 호출자: mc_host_probe() 의 addrs_set 라벨 직후 하나뿐.
 * 피호출자: writel_relaxed, mc_clear_secs, mc_clear_deds.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   mc_host_probe() -> [이 함수] -> mc_clear_secs() / mc_clear_deds()
 */
static void mc_disable_interrupts(struct mc_pcie *port)
{
	u32 val;

	/* Ensure ECC bypass is enabled */
	val = ECC_CONTROL_TX_RAM_ECC_BYPASS |
	      /* [한국어] RX 버퍼 ECC 우회. */
	      ECC_CONTROL_RX_RAM_ECC_BYPASS |
	      /* [한국어] PCIe -> AXI 버퍼 ECC 우회. */
	      ECC_CONTROL_PCIE2AXI_RAM_ECC_BYPASS |
	      /* [한국어] AXI -> PCIe 버퍼 ECC 우회. 네 비트를 모두 세워 ECC 검사 자체를 끈다. */
	      ECC_CONTROL_AXI2PCIE_RAM_ECC_BYPASS;
	/* [한국어] ECC_CONTROL 에 써 넣는다. 이 시점부터 SEC/DED 오류가 발생하지 않는다. */
	writel_relaxed(val, port->ctrl_base_addr + ECC_CONTROL);

	/* Disable SEC errors and clear any outstanding */
	writel_relaxed(SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT,
		       port->ctrl_base_addr + SEC_ERROR_INT_MASK);
	mc_clear_secs(port);

	/* Disable DED errors and clear any outstanding */
	writel_relaxed(DED_ERROR_INT_ALL_RAM_DED_ERR_INT,
		       port->ctrl_base_addr + DED_ERROR_INT_MASK);
	mc_clear_deds(port);

	/* Disable local interrupts and clear any outstanding */
	writel_relaxed(0, port->bridge_base_addr + IMASK_LOCAL);
	/* [한국어] ISTATUS_LOCAL 에 32비트 전체를 써서 모든 잔여 상태를 지운다(write-1-to-clear). */
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_LOCAL);
	/* [한국어] ISTATUS_MSI 에도 32비트 전체를 써서 대기 중이던 MSI 벡터 상태를 지운다. */
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_MSI);

	/* Disable PCIe events and clear any outstanding */
	val = PCIE_EVENT_INT_L2_EXIT_INT |
	      /* [한국어] Hot Reset 이탈 상태 비트. */
	      PCIE_EVENT_INT_HOTRST_EXIT_INT |
	      /* [한국어] DLUP 이탈 상태 비트. 여기까지가 비트 2:0(상태 클리어)이다. */
	      PCIE_EVENT_INT_DLUP_EXIT_INT |
	      /* [한국어] L2 이탈 마스크 비트(bit16). */
	      PCIE_EVENT_INT_L2_EXIT_INT_MASK |
	      /* [한국어] Hot Reset 이탈 마스크 비트(bit17). */
	      PCIE_EVENT_INT_HOTRST_EXIT_INT_MASK |
	      /* [한국어] DLUP 이탈 마스크 비트(bit18). 상태 비트와 마스크 비트를 한 워드에 함께 써서
	       * '끄고 지운다' 를 한 번에 처리한다. 이 용례가 '마스크 비트 1 = 차단' 이라는
	       * 극성의 근거이며, event_descs 의 mask_high = 1 과 일치한다. */
	      PCIE_EVENT_INT_DLUP_EXIT_INT_MASK;
	/* [한국어] PCIE_EVENT_INT 에 써 넣는다. */
	writel_relaxed(val, port->ctrl_base_addr + PCIE_EVENT_INT);

	/* Disable host interrupts and clear any outstanding */
	writel_relaxed(0, port->bridge_base_addr + IMASK_HOST);
	/* [한국어] ISTATUS_HOST 에 32비트 전체를 써서 잔여 상태를 지운다. IMASK_HOST 와
	 * ISTATUS_HOST 는 이 트리에서 이 함수에서만 쓰인다. */
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_HOST);
}

/*
 * [한국어]
 * mc_pcie_setup_inbound_atr - 인바운드(PCIe -> AXI) 주소 변환 창 하나를 설정한다
 *
 * @port: bridge_base_addr 이 매핑된 컨트롤러.
 * @window_index: 창 번호(0 ~ MC_MAX_NUM_INBOUND_WINDOWS-1).
 * @axi_addr: 변환 결과가 될 AXI(시스템 메모리) 주소.
 * @pcie_addr: 변환 대상이 되는 PCIe 버스 주소.
 * @size: 창 크기. 2의 거듭제곱이어야 하며 검사는 없다.
 * @return: 없음.
 *
 * 왜 필요한가: 엔드포인트가 DMA 로 보낸 주소를 시스템 메모리 주소로 되돌리는
 * 경로다. 이것이 없으면 EP 의 DMA 가 엉뚱한 곳을 가리키거나 아예 통하지 않는다.
 * PLDA 코어에도 plda_pcie_setup_inbound_address_translation() 이 있지만 그것은
 * 크기 필드만 손대는 반쪽짜리라, Microchip 은 자기 구현을 따로 두었다
 * (그리고 그 결과 코어 쪽 함수는 이 트리에서 호출자가 없다).
 *
 * 동작 단계(아웃바운드 plda_pcie_setup_window 와 방향만 반대다):
 *  1. table_addr = 브리지 창 + window_index * ATR_ENTRY_SIZE(32).
 *     인바운드 테이블의 기준 오프셋(0x600)은 아래 레지스터 이름에 들어 있다.
 *  2. atr_sz = ilog2(size) - 1. 하드웨어는 크기를 2^(값+1) 로 해석한다.
 *  3. 소스는 "PCIe 주소" 다. 하위 32비트를 4KiB 로 내림해 하위 12비트를 비우고
 *     크기 필드(비트 6:1)와 ATR_IMPL_ENABLE(비트 0)을 OR 해 SRCADDR_PARAM 에 쓴다.
 *  4. PCIe 주소 상위 32비트를 SRC_ADDR 에 쓴다.
 *  5. 변환 결과인 AXI 주소의 하위/상위 32비트를 TRSL_ADDR_LSB / _UDW 에 쓴다.
 *  6. TRSL_PARAM 에 TRSL_ID_AXI4_MASTER_0(4)을 써서 변환된 트랜잭션이 AXI4
 *     마스터 포트 0 으로 나가게 한다. EP 의 DMA 가 시스템 메모리에 닿는 통로다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(ECAM init 훅 안). 락 없음.
 * 호출자: mc_pcie_setup_inbound_ranges() 세 곳(non-coherent 두 번, coherent 기본
 * 한 번, dma-ranges 순회에서 반복).
 * 피호출자: ilog2, ALIGN_DOWN, FIELD_PREP, lower_32_bits, upper_32_bits, writel.
 * 에러 경로: 없음 -- window_index 나 size 의 유효성을 검사하지 않는다.
 *
 * 호출 체인:
 *   mc_platform_init() -> mc_pcie_setup_inbound_ranges() -> [이 함수]
 */
static void mc_pcie_setup_inbound_atr(struct mc_pcie *port, int window_index,
				      u64 axi_addr, u64 pcie_addr, u64 size)
{
	u32 table_offset = window_index * ATR_ENTRY_SIZE;
	/* [한국어] 이 창의 레지스터 묶음 시작 주소 = 브리지 창 + 창번호 * 32.
	 * 인바운드 테이블의 기준 오프셋(0x600)은 아래 레지스터 이름 상수에 포함되어 있다. */
	void __iomem *table_addr = port->bridge_base_addr + table_offset;
	/* [한국어] 크기 필드에 넣을 로그값. */
	u32 atr_sz;
	/* [한국어] 각 레지스터에 쓸 값을 만드는 임시 변수. */
	u32 val;

	/* [한국어] 크기를 log2(size)-1 로 바꾼다. 하드웨어는 2^(값+1) 바이트로 해석한다. */
	atr_sz = ilog2(size) - 1;

	/* [한국어] 인바운드에서는 '소스' 가 PCIe 주소다. 하위 32비트를 4KiB 로 내림해 하위 12비트를
	 * 비운다 -- 그 자리에 크기와 enable 이 들어가기 때문이다. */
	val = ALIGN_DOWN(lower_32_bits(pcie_addr), SZ_4K);
	/* [한국어] 크기 필드(비트 6:1)를 넣는다. */
	val |= FIELD_PREP(ATR_SIZE_MASK, atr_sz);
	/* [한국어] bit0 을 세워 이 인바운드 창을 실제로 켠다. PLDA 코어의
	 * plda_pcie_setup_inbound_address_translation() 이 이 비트를 세우지 않는 것과
	 * 대조적이며, Microchip 이 자체 구현을 쓰는 이유 중 하나다. */
	val |= ATR_IMPL_ENABLE;

	/* [한국어] 합쳐진 값을 소스 주소/파라미터 레지스터에 쓴다. */
	writel(val, table_addr + ATR0_PCIE_WIN0_SRCADDR_PARAM);

	/* [한국어] 소스(PCIe) 주소 상위 32비트를 별도 레지스터에 쓴다. */
	writel(upper_32_bits(pcie_addr), table_addr + ATR0_PCIE_WIN0_SRC_ADDR);

	/* [한국어] 변환 결과(AXI) 주소 하위 32비트. */
	writel(lower_32_bits(axi_addr), table_addr + ATR0_PCIE_WIN0_TRSL_ADDR_LSB);
	/* [한국어] 변환 결과(AXI) 주소 상위 32비트. */
	writel(upper_32_bits(axi_addr), table_addr + ATR0_PCIE_WIN0_TRSL_ADDR_UDW);

	/* [한국어] 변환된 트랜잭션을 AXI4 마스터 포트 0 으로 보낸다. 이것이 엔드포인트의 DMA 가
	 * 시스템 메모리에 닿는 실제 통로다. */
	writel(TRSL_ID_AXI4_MASTER_0, table_addr + ATR0_PCIE_WIN0_TRSL_PARAM);
}

/*
 * [한국어]
 * mc_pcie_setup_inbound_ranges - coherent / non-coherent 설계에 맞춰 인바운드 창을 구성한다
 *
 * @pdev: devicetree 노드를 갖고 있는 플랫폼 디바이스.
 * @port: bridge_base_addr 이 매핑된 컨트롤러.
 * @return: 0 성공, -EINVAL 은 dma-ranges 항목이 하드웨어 창 수(8)를 넘음.
 *
 * 왜 필요한가: 아래 영문 주석이 배경을 자세히 설명한다. 요지는 MPFS 의 PCIe
 * 루트 포트가 32비트 전용이고, FPGA 패브릭의 FIC(Fabric Interface Controller)
 * 블록 뒤에 있어서 지원되는 구성이 사실상 두 가지뿐이라는 것이다.
 *  - 구성 1(fully coherent): CPU 공간 0 부터 지정된 PCIe 공간으로 가는 창 하나.
 *  - 구성 2(non-coherent): CPU 공간으로 가는 1GiB 창 두 개. 하나는 CPU 0 을
 *    PCIe 0x80000000 으로, 다른 하나는 CPU 0x40000000 을 PCIe 0xc0000000 으로
 *    매핑한다. 창이 둘이어야 하는 이유는 MPFS 의 AXI-S 영역에서 MSI 공간이
 *    할당되는 방식 때문이며(영문 주석), 주소와 크기 사이의 하드웨어 상호작용
 *    때문에 하나로 합칠 수 없다.
 * 그리고 PCIe 블록 바깥의 FIC 인터페이스가 MCHP MPFS FPGA 설계 지침에 따라
 * 인바운드 변환을 완성해 주어야 한다는 전제가 붙는다.
 *
 * 동작 단계:
 *  1. devicetree 에 dma-noncoherent 속성이 있으면 구성 2 -- 위에서 설명한
 *     1GiB 창 두 개를 고정으로 만든다.
 *  2. 없으면(coherent) dma-ranges 파서를 초기화한다.
 *     실패하면 dma-ranges 자체가 없다는 뜻이므로, 0 부터 4GiB 를 1:1 로
 *     매핑하는 기본 창 하나를 만들고 끝낸다.
 *  3. dma-ranges 가 있으면 각 항목마다 창을 하나씩 만든다. 창 번호가
 *     MC_MAX_NUM_INBOUND_WINDOWS(8) 이상이 되면 -EINVAL 로 거부한다.
 *     이때 axi_addr 을 0 으로 고정해 넘기는데, 실제 AXI 주소 변환은 FIC 가
 *     완성한다는 위 전제 때문이다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(ECAM init 훅 안). 잠들 수 있다.
 * 호출자: mc_platform_init() 하나뿐.
 * 피호출자: device_property_read_bool, of_pci_dma_range_parser_init,
 * for_each_of_range, mc_pcie_setup_inbound_atr, dev_err.
 * 에러 경로: 창 개수 초과만 오류로 처리하며, 그 값은 mc_platform_init 을 거쳐
 * ECAM 코어까지 올라가 probe 를 중단시킨다.
 *
 * 호출 체인:
 *   mc_platform_init() -> [이 함수] -> mc_pcie_setup_inbound_atr()
 */
static int mc_pcie_setup_inbound_ranges(struct platform_device *pdev,
					struct mc_pcie *port)
{
	struct device *dev = &pdev->dev;
	/* [한국어] devicetree 노드. dma-ranges 파싱의 대상이다. */
	struct device_node *dn = dev->of_node;
	/* [한국어] of_range 순회 상태를 담을 파서. */
	struct of_range_parser parser;
	/* [한국어] 순회 중 현재 항목을 받을 구조체. */
	struct of_range range;
	/* [한국어] 다음에 쓸 인바운드 창 번호. 아웃바운드와 달리 0번부터 쓴다 --
	 * 인바운드 테이블은 config space 를 위한 예약이 없기 때문이다. */
	int atr_index = 0;

	/*
	 * MPFS PCIe Root Port is 32-bit only, behind a Fabric Interface
	 * Controller FPGA logic block which contains the AXI-S interface.
	 *
	 * From the point of view of the PCIe Root Port, there are only two
	 * supported Root Port configurations:
	 *
	 * Configuration 1: for use with fully coherent designs; supports a
	 * window from 0x0 (CPU space) to specified PCIe space.
	 *
	 * Configuration 2: for use with non-coherent designs; supports two
	 * 1 GB windows to CPU space; one mapping CPU space 0 to PCIe space
	 * 0x80000000 and a second mapping CPU space 0x40000000 to PCIe
	 * space 0xc0000000. This cfg needs two windows because of how the
	 * MSI space is allocated in the AXI-S range on MPFS.
	 *
	 * The FIC interface outside the PCIe block *must* complete the
	 * inbound address translation as per MCHP MPFS FPGA design
	 * guidelines.
	 */
	if (device_property_read_bool(dev, "dma-noncoherent")) {
		/*
		 * Always need same two tables in this case.  Need two tables
		 * due to hardware interactions between address and size.
		 */
		mc_pcie_setup_inbound_atr(port, 0, 0,
					  MPFS_NC_BOUNCE_ADDR, SZ_1G);
		/* [한국어] 두 번째 1GiB 창: AXI 0x40000000 -> PCIe 0xc0000000.
		 * 창을 둘로 나눠야 하는 이유는 위 영문 주석대로 주소와 크기 사이의 하드웨어
		 * 상호작용, 그리고 MPFS 의 AXI-S 영역에서 MSI 공간이 배치되는 방식 때문이다. */
		mc_pcie_setup_inbound_atr(port, 1, SZ_1G,
					  MPFS_NC_BOUNCE_ADDR + SZ_1G, SZ_1G);
	} else {
		/* Find any DMA ranges */
		if (of_pci_dma_range_parser_init(&parser, dn)) {
			/* No DMA range property - setup default */
			mc_pcie_setup_inbound_atr(port, 0, 0, 0, SZ_4G);
			return 0;
		}

		/* [한국어] devicetree 의 dma-ranges 항목을 하나씩 순회한다. */
		for_each_of_range(&parser, &range) {
			/* [한국어] 하드웨어 인바운드 창은 8개뿐이므로 그보다 많은 항목은 처리할 수 없다. */
			if (atr_index >= MC_MAX_NUM_INBOUND_WINDOWS) {
				/* [한국어] 몇 개까지 가능한지 알려 주는 오류 메시지. */
				dev_err(dev, "too many inbound ranges; %d available tables\n",
					MC_MAX_NUM_INBOUND_WINDOWS);
				return -EINVAL;
			}
			/* [한국어] 이 항목을 창으로 만든다. axi_addr 을 0 으로 고정해 넘기는데, 실제 AXI 주소
			 * 변환은 PCIe 블록 바깥의 FIC 가 완성한다는 위 영문 주석의 전제 때문이다. */
			mc_pcie_setup_inbound_atr(port, atr_index, 0,
						  range.pci_addr, range.size);
			/* [한국어] 다음 창 번호로 넘어간다. */
			atr_index++;
		}
	}

	return 0;
}

/*
 * [한국어]
 * mc_platform_init - ECAM 코어가 config 창을 만든 직후 부르는 SoC 초기화 훅
 *
 * @cfg: pci_ecam_create() 가 방금 만든 config 창 정보. res 에 config space 의
 *       물리 주소 범위가, win 에 그것을 ioremap 한 가상 주소가 들어 있다.
 * @return: 0 성공, 음수 errno 실패(그대로 probe 실패로 이어진다).
 *
 * 왜 필요한가: 이 드라이버가 PLDA 코어의 헬퍼들과 만나는 유일한 지점이다.
 * ECAM 공용 코어는 config 창을 만들고 버스를 스캔할 줄만 알지, PLDA 브리지의
 * 주소 변환 테이블이나 인터럽트는 모른다. 그 빈틈을 이 훅이 채운다.
 * 호출 시점이 중요하다 -- pci_ecam_create() 안에서 창을 만든 "직후",
 * pci_host_probe() 로 버스를 스캔하기 "전" 이라, 여기서 주소 변환과 인터럽트를
 * 세워 두면 스캔이 정상적으로 동작한다.
 *
 * 동작 단계:
 *  1. cfg->parent 에서 device 를, 거기서 platform_device 를 얻는다.
 *  2. platform_get_drvdata(pdev) 로 호스트 브리지를 얻는다. 이것이 가능한 이유는
 *     pci_host_common_init() 이 ECAM 창을 만들기 "전에"
 *     platform_set_drvdata(pdev, bridge) 를 해 두기 때문이다.
 *  3. ATR 0번 창을 config space 로 프로그래밍한다. axi_addr 과 pci_addr 을 모두
 *     cfg->res.start 로 주어 1:1 매핑한다 -- StarFive 가 pci_addr 에 0 을 주는
 *     것과 다른 선택이며, MPFS 에서는 config 창이 그렇게 배치되어 있다는 뜻이다.
 *  4. 루트 포트 자신의 MSI capability 를 고친다(cfg->win 이 ECAM 창 시작 주소).
 *  5. 나머지 메모리 창들을 ATR 1번 이후에 채운다.
 *  6. 인바운드 변환 창을 구성한다.
 *  7. plda 구조체에 Microchip 전용 훅과 이벤트 비트맵을 채운다. 이 세 줄이 있어야
 *     다음 줄의 plda_init_interrupts() 가 코어 기본값 대신 Microchip 구현을 쓴다.
 *     events_bitmap 은 GENMASK(NUM_EVENTS-1, 0) 즉 28개 전부 -- StarFive 가
 *     도어벨 두 개를 빼는 것과 달리 여기서는 걸러내지 않는다(어차피 마스크가
 *     0 이라 발생하지 않으므로 무해하다).
 *  8. 주소 변환이 준비된 뒤에야 인터럽트를 켠다는 상류 주석 그대로,
 *     마지막으로 plda_init_interrupts() 를 부른다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 잠들 수 있다.
 * 이 함수는 전역 변수 port 를 쓴다 -- 인자로 컨트롤러를 받을 방법이 없는
 * 콜백이기 때문이며, 그래서 이 드라이버는 컨트롤러 하나만 지원한다.
 * 호출자: pci_ecam_create()(drivers/pci/ecam.c)가 ops->init 으로 간접 호출.
 * 피호출자: plda_pcie_setup_window, mc_pcie_enable_msi, plda_pcie_setup_iomems,
 * mc_pcie_setup_inbound_ranges, plda_init_interrupts.
 * 에러 경로: 각 실패를 그대로 반환하면 pci_ecam_create 가 ERR_PTR 로 바꿔
 * pci_host_common_init 이 probe 를 접는다. 되돌리는 코드는 없고 devm 에 맡긴다.
 *
 * 호출 체인:
 *   mc_host_probe() -> pci_host_common_probe() -> pci_host_common_init()
 *     -> pci_host_common_ecam_create() -> pci_ecam_create() -> ops->init
 *     = [이 함수] -> plda_init_interrupts()
 */
static int mc_platform_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	/* [한국어] cfg->parent 가 가리키는 device 로부터 플랫폼 디바이스를 얻는다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 호스트 브리지를 drvdata 에서 꺼낸다. 이것이 가능한 것은 pci_host_common_init()
	 * 이 ECAM 창을 만들기 '전에' platform_set_drvdata(pdev, bridge) 를 해 두기 때문이다. */
	struct pci_host_bridge *bridge = platform_get_drvdata(pdev);
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* Configure address translation table 0 for PCIe config space */
	plda_pcie_setup_window(port->bridge_base_addr, 0, cfg->res.start,
			       cfg->res.start,
			       resource_size(&cfg->res));

	/* Need some fixups in config space */
	mc_pcie_enable_msi(port, cfg->win);

	/* Configure non-config space outbound ranges */
	ret = plda_pcie_setup_iomems(bridge, &port->plda);
	/* [한국어] 아웃바운드 메모리 창 설정 실패. */
	if (ret)
		return ret;

	/* [한국어] 엔드포인트 DMA 가 시스템 메모리에 닿을 인바운드 창을 구성한다. */
	ret = mc_pcie_setup_inbound_ranges(pdev, port);
	/* [한국어] 창 개수 초과 등의 실패. */
	if (ret)
		return ret;

	/* [한국어] 이벤트 수집을 Microchip 구현으로 바꾼다. 이 대입이 없으면
	 * plda_init_interrupts() 가 코어 기본 plda_event_ops 를 꽂아
	 * ISTATUS_LOCAL 만 보게 되어 전용 이벤트 15개를 놓친다. */
	port->plda.event_ops = &mc_event_ops;
	/* [한국어] 이벤트 irq_chip 도 Microchip 구현으로 바꾼다. event_descs[] 를 보는 구현이라야
	 * 네 레지스터와 두 가지 마스크 극성을 다룰 수 있다. */
	port->plda.event_irq_chip = &mc_event_irq_chip;
	/* [한국어] 28개 이벤트를 전부 켠다. StarFive 가 도어벨 두 개를 빼는 것과 달리 걸러내지
	 * 않는데, 그 이벤트들은 마스크 상수가 0 이라 어차피 보고되지 않으므로 무해하다. */
	port->plda.events_bitmap = GENMASK(NUM_EVENTS - 1, 0);

	/* Address translation is up; safe to enable interrupts */
	ret = plda_init_interrupts(pdev, &port->plda, &mc_event);
	/* [한국어] 인터럽트 설정 실패는 그대로 올려보낸다. */
	if (ret)
		return ret;

	return 0;
}

/*
 * [한국어]
 * mc_host_probe - 드라이버 진입점. 레지스터 창을 잡고 ECAM 공용 probe 에 넘긴다
 *
 * @pdev: 플랫폼 버스가 microchip,pcie-host-1.0 노드와 매칭시켜 만든 디바이스.
 * @return: pci_host_common_probe() 의 반환값, 또는 그 전 단계의 음수 errno.
 *
 * 왜 필요한가: StarFive 의 probe 가 PLDA 코어에 제어를 넘기는 것과 달리, 여기서는
 * ECAM 공용 코어에 넘긴다. 그 전에 이 함수가 해야 할 일은 (1) 레지스터 창 두 개
 * 확보(신규/레거시 바인딩 양쪽 지원), (2) 인터럽트 전부 끄기, (3) MSI 관련 값을
 * 하드웨어에서 읽어 오기, (4) 패브릭 클럭 켜기다.
 *
 * 동작 단계:
 *  1. 컨트롤러 구조체를 devm_kzalloc 으로 잡아 전역 port 에 대입한다.
 *  2. plda->dev 를 채운다.
 *  3. 'bridge' 와 'ctrl' 이름의 리소스를 각각 ioremap 한다. 둘 다 성공하면
 *     addrs_set 로 건너뛴다.
 *  4. 하나라도 실패하면 레거시 바인딩을 시도한다 -- 위 영문 주석대로, 예전
 *     (잘못된) 바인딩은 두 블록을 'apb' 하나로 뭉뚱그렸다. 그 창을 잡고
 *     +0x8000 과 +0xa000 을 더해 두 주소를 만든다. 이 하위 호환을 위해
 *     MC_PCIE1_BRIDGE_ADDR / MC_PCIE1_CTRL_ADDR 상수가 존재한다.
 *  5. 모든 인터럽트를 끄고 잔여 상태를 지운다.
 *  6. plda->bridge_addr 에 브리지 창을, plda->num_events 에 NUM_EVENTS(28)를 넣는다.
 *  7. PCIE_PCI_IRQ_DW0 에서 MSIX_CAP 비트를 지운다 -- MSI-X 를 감춰야 커널이
 *     MSI 를 쓰기 때문이다(상류 주석 그대로).
 *  8. 같은 레지스터에서 NUM_MSI_MSGS 필드를 읽어 1 << val 을 벡터 수로 삼는다.
 *     이 값은 FPGA 비트파일이 굽는 것이라 보드마다 다르다 -- StarFive 가
 *     고정값 32 를 쓰는 것과 대비된다.
 *  9. IMSI_ADDR 레지스터를 읽어 MSI 수신 주소를 얻는다. 역시 설계에서 정해진다.
 * 10. 패브릭 클럭(fic0~fic3)을 켠다. 실패하면 로그를 남기고 -ENODEV 로 바꿔
 *     반환한다(원래 오류 코드가 버려진다).
 * 11. pci_host_common_probe() 에 넘긴다. 이 안에서 ECAM 창이 만들어지고
 *     mc_platform_init() 이 불리고 버스가 스캔된다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: 커널 플랫폼 버스 코어(driver->probe).
 * 피호출자: devm_kzalloc, devm_platform_ioremap_resource_byname,
 * mc_disable_interrupts, readl/writel, mc_pcie_init_clks, pci_host_common_probe.
 * 에러 경로: 각 실패에서 즉시 반환하며 devm 이 자원을 회수한다. remove 함수가
 * 없는데, builtin 이고 suppress_bind_attrs 로 언바인드가 막혀 있어 해제 경로가
 * 사실상 존재하지 않기 때문이다.
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 -> [이 함수] -> pci_host_common_probe()
 *     -> pci_ecam_create() -> mc_platform_init() -> pci_host_probe()
 */
static int mc_host_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] 레거시 바인딩에서 쓸 통합 창 주소. 신규 바인딩 경로에서는 쓰이지 않는다. */
	void __iomem *apb_base_addr;
	/* [한국어] 코어 구조체를 가리킬 지역 포인터. */
	struct plda_pcie_rp *plda;
	/* [한국어] 반환값 임시 변수. */
	int ret;
	/* [한국어] 레지스터 읽기/쓰기용 임시 변수. */
	u32 val;

	/* [한국어] 컨트롤러 구조체를 할당해 static 전역 port 에 대입한다. 전역인 이유는
	 * mc_platform_init() 이 인자로 컨트롤러를 받을 수 없는 콜백이기 때문이며,
	 * 그 대가로 이 드라이버는 컨트롤러 하나만 지원한다. */
	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!port)
		return -ENOMEM;

	/* [한국어] 코어 구조체는 이 구조체의 첫 필드다. */
	plda = &port->plda;
	/* [한국어] 코어가 쓸 device 포인터를 채운다. */
	plda->dev = dev;

	/* [한국어] 신규 바인딩: PLDA 브리지 레지스터 창을 'bridge' 이름으로 찾아 매핑한다. */
	port->bridge_base_addr = devm_platform_ioremap_resource_byname(pdev,
								    "bridge");
	/* [한국어] 신규 바인딩: Microchip 컨트롤러 레지스터 창을 'ctrl' 이름으로 찾아 매핑한다. */
	port->ctrl_base_addr = devm_platform_ioremap_resource_byname(pdev,
								    "ctrl");
	/* [한국어] 둘 다 성공했으면 레거시 처리를 건너뛴다. 두 창이 따로 있는 것이 올바른 바인딩이다. */
	if (!IS_ERR(port->bridge_base_addr) && !IS_ERR(port->ctrl_base_addr))
		goto addrs_set;

	/*
	 * The original, incorrect, binding that lumped the control and
	 * bridge addresses together still needs to be handled by the driver.
	 */
	apb_base_addr = devm_platform_ioremap_resource_byname(pdev, "apb");
	/* [한국어] 레거시 'apb' 창마저 없으면 이 노드로는 아무것도 할 수 없다. */
	if (IS_ERR(apb_base_addr))
		/* [한국어] 두 바인딩 모두 실패했음을 알리고 반환한다. */
		return dev_err_probe(dev, PTR_ERR(apb_base_addr),
				     "both legacy apb register and ctrl/bridge regions missing");

	/* [한국어] 레거시 창 안에서 브리지 블록이 시작하는 위치(+0x8000). */
	port->bridge_base_addr = apb_base_addr + MC_PCIE1_BRIDGE_ADDR;
	/* [한국어] 레거시 창 안에서 컨트롤러 블록이 시작하는 위치(+0xa000).
	 * 이 두 줄이 예전 devicetree 와의 하위 호환을 유지하는 전부다. */
	port->ctrl_base_addr = apb_base_addr + MC_PCIE1_CTRL_ADDR;

addrs_set:
	mc_disable_interrupts(port);

	/* [한국어] 코어가 쓸 브리지 창 주소를 복사한다. 이 시점 이후 코어의 모든 레지스터 접근이
	 * 이 주소를 기준으로 이루어진다. */
	plda->bridge_addr = port->bridge_base_addr;
	/* [한국어] 이벤트 도메인의 크기를 28 로 지정한다. StarFive 의 29 와 다른 값이다. */
	plda->num_events = NUM_EVENTS;

	/* Allow enabling MSI by disabling MSI-X */
	val = readl(port->bridge_base_addr + PCIE_PCI_IRQ_DW0);
	/* [한국어] MSIX_CAP(bit31)을 지워 MSI-X 능력을 감춘다. 상류 주석대로, MSI-X 가 보이면
	 * 커널이 MSI-X 를 선택해 이 드라이버의 MSI 경로가 쓰이지 않기 때문이다. */
	val &= ~MSIX_CAP_MASK;
	/* [한국어] 되쓴다. */
	writel(val, port->bridge_base_addr + PCIE_PCI_IRQ_DW0);

	/* Pick num vectors from bitfile programmed onto FPGA fabric */
	val = readl(port->bridge_base_addr + PCIE_PCI_IRQ_DW0);
	/* [한국어] NUM_MSI_MSGS 필드(비트 6:4)만 남긴다. */
	val &= NUM_MSI_MSGS_MASK;
	/* [한국어] 시프트해 로그값(0~5)을 얻는다. */
	val >>= NUM_MSI_MSGS_SHIFT;

	/* [한국어] 1 << val 이 실제 벡터 수다. FPGA 비트파일이 정하는 값이라 보드마다 다르며,
	 * 이것이 StarFive 처럼 plda_set_default_msi() 의 고정값 32 를 쓰지 않는 이유다. */
	plda->msi.num_vectors = 1 << val;

	/* Pick vector address from design */
	plda->msi.vector_phy = readl_relaxed(port->bridge_base_addr + IMSI_ADDR);

	/* [한국어] FPGA 패브릭 인터페이스 클럭을 켠다. 레지스터 접근 뒤에 오는 순서가 특이한데,
	 * 부트로더가 이미 필요한 클럭을 켜 두었을 가능성이 있으나 코드에 근거는 없다. */
	ret = mc_pcie_init_clks(dev);
	/* [한국어] 클럭 설정 실패. */
	if (ret) {
		/* [한국어] 원래 오류 코드를 로그에만 남기고 */
		dev_err(dev, "failed to get clock resources, error %d\n", ret);
		return -ENODEV;
	}

	/* [한국어] ECAM 공용 probe 에 넘긴다. 이 안에서 config 창이 만들어지고 mc_platform_init()
	 * 이 불리고 pci_host_probe() 로 버스가 스캔된다 -- 즉 이 한 줄이 끝나면
	 * PCIe 장치들이 이미 발견되어 동작 중이다. */
	return pci_host_common_probe(pdev);
}

/*
 * [한국어] ECAM 공용 코어에 넘기는 동작 정의. 이 드라이버가 StarFive 와 갈라지는 핵심.
 *
 *  - init = mc_platform_init : ECAM 창 생성 직후 불리는 SoC 훅. PLDA 헬퍼들을
 *    호출하는 유일한 지점이다.
 *  - pci_ops.map_bus = pci_ecam_map_bus : ECAM 표준 주소 계산. PLDA 코어의
 *    plda_pcie_map_bus() 를 쓰지 않는데, sysdata 가 struct plda_pcie_rp 가 아니라
 *    struct pci_config_window 이기 때문이다.
 *  - read / write = pci_generic_config_read / write : 표준 구현 그대로.
 *    StarFive 처럼 루트 포트 BAR 을 감추는 래퍼가 없다.
 *  bus_shift 를 지정하지 않아 0 이며, pci_ecam_create 가 그 경우 기본값을 쓴다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: of_device_id.data 를 통해
 * pci_host_common_probe() 가 of_device_get_match_data 로 꺼내 쓴다.
 * 동기화: 상수.
 */
static const struct pci_ecam_ops mc_ecam_ops = {
	.init = mc_platform_init,
	.pci_ops = {
		/* [한국어] ECAM 표준 주소 계산. sysdata 가 struct pci_config_window 이므로
		 * PLDA 코어의 plda_pcie_map_bus() 가 아니라 이쪽을 써야 한다. */
		.map_bus = pci_ecam_map_bus,
		.read = pci_generic_config_read,
		.write = pci_generic_config_write,
	}
};

/*
 * [한국어] devicetree compatible 매칭 표.
 *
 * "microchip,pcie-host-1.0" 하나만 받으며, .data 에 위 mc_ecam_ops 를 실어 보낸다.
 * StarFive 판이 .data 를 쓰지 않는 것과 다른데, ECAM 공용 probe 가 ops 를
 * of_device_get_match_data() 로 얻는 구조이기 때문에 여기서는 필수다.
 * 마지막 빈 항목은 배열 끝 sentinel 이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: 플랫폼 버스 매칭과 pci_host_common_probe().
 * 동기화: 상수.
 */
static const struct of_device_id mc_pcie_of_match[] = {
	{
		/* [한국어] 이 드라이버가 받아들이는 유일한 compatible 문자열. */
		.compatible = "microchip,pcie-host-1.0",
		/* [한국어] 이 노드에 대해 쓸 ECAM 동작 정의를 실어 보낸다.
		 * pci_host_common_probe() 가 of_device_get_match_data() 로 이 값을 꺼내 쓰므로,
		 * 이 한 줄이 없으면 -ENODEV 로 probe 가 실패한다. */
		.data = &mc_ecam_ops,
	},
	/* [한국어] 배열 끝 sentinel. StarFive 판이 주석으로 sentinel 이라고 표시한 것과 같은 역할이며,
	 * 여기서는 표시 없이 빈 초기화자만 두었다. */
	{},
};

/* [한국어] 매치 표를 모듈 메타데이터에 심는다. 이 드라이버는 builtin 이라 모듈 자동 로드에
 * 쓰이지는 않지만, 매크로는 그대로 두어 다른 도구가 매치 정보를 읽을 수 있게 한다. */
MODULE_DEVICE_TABLE(of, mc_pcie_of_match);

/*
 * [한국어] 이 파일이 커널에 등록하는 플랫폼 드라이버 정의.
 *
 *  - probe = mc_host_probe. remove 는 없다 -- 아래 두 이유로 해제 경로가
 *    사실상 존재하지 않기 때문이다.
 *  - of_match_table : of_match_ptr 로 감싸지 않았다. builtin 이라 CONFIG_OF 가
 *    항상 켜져 있다고 보는 것이다.
 *  - suppress_bind_attrs = true : sysfs 의 bind/unbind 파일을 만들지 않아
 *    사용자 공간에서 이 드라이버를 떼어낼 수 없게 한다. 전역 변수 port 에
 *    의존하는 구조라 재바인드가 안전하지 않기 때문으로 보인다.
 * 등록 매크로도 module_platform_driver 가 아니라 builtin_platform_driver 다.
 * Kconfig 는 tristate 이지만 실제로는 모듈로 뺄 수 없다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: builtin_platform_driver 가 만드는
 * initcall 이 platform_driver_register 에 넘긴다.
 * 동기화: 상수.
 */
static struct platform_driver mc_pcie_driver = {
	.probe = mc_host_probe,
	.driver = {
		/* [한국어] sysfs 의 /sys/bus/platform/drivers 아래에 나타나는 이름. */
		.name = "microchip-pcie",
		.of_match_table = mc_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};

/* [한국어] module_platform_driver 가 아니라 builtin 판이다. 커널 initcall 로 등록되며
 * 모듈로 뺄 수 없고, 위 suppress_bind_attrs 와 함께 이 드라이버가 한 번 올라오면
 * 내려가지 않음을 보장한다. remove 함수가 없는 이유이기도 하다. */
builtin_platform_driver(mc_pcie_driver);
/* [한국어] 모듈 라이선스. builtin 이지만 EXPORT_SYMBOL_GPL 로 노출된 PLDA 코어 심볼을
 * 쓰므로 GPL 계열이어야 한다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 나타나는 설명. */
MODULE_DESCRIPTION("Microchip PCIe host controller driver");
/* [한국어] 원저자 표기. 같은 사람이 pcie-plda-host.c 의 저자이기도 하다 --
 * 두 파일이 함께 설계되었음을 보여 준다. */
MODULE_AUTHOR("Daire McNamara <daire.mcnamara@microchip.com>");
