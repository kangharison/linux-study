// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe host controller driver for Xilinx XDMA PCIe Bridge
 *
 * Copyright (C) 2023 Xilinx, Inc. All rights reserved.
 */

/*
 * [한국어 설명] Xilinx XDMA/QDMA PCIe 브리지 호스트 드라이버 (pcie-xilinx-dma-pl.c)
 *
 * === 파일의 역할 ===
 * Xilinx 의 XDMA 와 QDMA PCIe 브리지를 리눅스 호스트 브리지로 물리는
 * 드라이버다. 파일 이름의 "pl" 은 드라이버 이름과 문자열
 * ("pl_dma:INTx", "pl_dma:MSI", "pl_dma-")에도 나타나는 접두사이며,
 * 그것이 무엇의 줄임인지는 이 소스에 적혀 있지 않다.
 * 하는 일이 넷이다. config 접근을 코어의 ECAM 골격에 맡기고, 컨트롤러
 * 레지스터를 초기 상태로 만들고, **인터럽트 도메인 셋**(이벤트·INTx·MSI)을
 * 세우고, 주 인터럽트 하나를 받아 그 셋으로 갈라 보낸다.
 * 변종이 둘(XDMA, QDMA)이며 그 차이가 **레지스터 접근 함수와 창 구성**에만
 * 나타난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [PCI 코어]  probe.c, setup-bus.c, msi/, ecam.c ...
 *        ^  struct pci_ecam_ops(map_bus 만 자체 구현), irq_domain 셋
 *   [이 파일]   pcie-xilinx-dma-pl.c
 *        ^  pcie_read/pcie_write 로 컨트롤러 레지스터 창에 직접
 *   [XDMA/QDMA 하드웨어] 브리지 레지스터, ECAM 창
 *
 * 위층과 맞닿는 지점이 넷이다.
 *   struct pci_ecam_ops — **코어의 ECAM 골격을 그대로 쓴다.**
 *       pci_ecam_create() 가 창을 만들고, 이 파일은 map_bus 만 채운다.
 *   이벤트 irq_domain(pldma_domain) — 컨트롤러 자신의 사건 32가지.
 *       버스 토큰이 DOMAIN_BUS_NEXUS 로, **가운데서 갈라 보내는 층**이다.
 *   INTx irq_domain(intx_domain) — 레거시 인터럽트 넷.
 *       버스 토큰이 DOMAIN_BUS_WIRED 다.
 *   MSI irq_domain(msi.dev_domain) — 벡터 64개.
 * 아래로는 clk 도 phy 도 reset GPIO 도 쓰지 않는다 — 이 파일이 다루는
 * 것은 레지스터와 인터럽트뿐이다.
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트이고 핸들러 넷은 인터럽트
 * 컨텍스트다. **연쇄(chained) 핸들러가 하나도 없고** 모두
 * devm_request_irq(IRQF_SHARED | IRQF_NO_THREAD)로 등록된다 —
 * NO_THREAD 라 강제 스레드화(threadirqs) 아래에서도 하드 인터럽트
 * 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 것:
 *   - pcie-xilinx-common.h — **XILINX_PCIE_INTR_* 인터럽트 비트 번호.**
 *     같은 디렉터리의 pcie-xilinx-cpm.c 와 공유하는 표다. 값이 BIT(n) 이
 *     아니라 번호 n 자체라, 이 파일은 IMR() 매크로로 감싸 비트로 쓰고
 *     동시에 이벤트 도메인의 hwirq 로도 쓴다.
 *   - ../pci.h — PCI 코어 내부 전용 헤더.
 *   - linux/pci-ecam.h — pci_ecam_create()/pci_ecam_free() 와
 *     PCIE_ECAM_OFFSET(). config 창 관리를 통째로 코어에 맡긴다.
 *   - linux/irqchip/irq-msi-lib.h, irqdomain.h, msi.h — MSI 부모 도메인.
 *   - linux/bitfield.h — FIELD_GET. INTx 상태를 떼어 낼 때 쓴다.
 * 이 파일에 의존하는 것: 없다. 심볼을 내보내지 않는 말단 플랫폼
 * 드라이버이며 builtin_platform_driver 로 등록된다.
 *
 * === 주요 함수/구조체 요약 ===
 * xilinx_pl_dma_pcie_probe()        : 진입점. ECAM·레지스터·도메인 셋.
 * xilinx_pl_dma_pcie_init_port()    : 컨트롤러 레지스터를 초기 상태로.
 * xilinx_pl_dma_pcie_setup_irq()    : 사건별 IRQ 를 만들고 핸들러를 건다.
 * xilinx_pl_dma_pcie_event_flow()   : 주 인터럽트를 받아 이벤트 도메인으로.
 * xilinx_pl_dma_pcie_intr_handler() : 사건 하나를 사람이 읽을 문장으로.
 * xilinx_pl_dma_pcie_intx_flow()    : INTx 넷을 INTx 도메인으로.
 * xilinx_pl_dma_pcie_msi_handler_low/high() : MSI 하위·상위 32벡터.
 * pcie_read()/pcie_write()          : **변종에 따라 오프셋을 더한다.**
 * struct pl_dma_pcie                : 이 드라이버의 인스턴스 상태 전부.
 * struct xilinx_pl_dma_variant      : XDMA 인지 QDMA 인지.
 *
 * === 변종 둘을 가르는 방식 ===
 * of_match 표가 compatible 문자열을 struct xilinx_pl_dma_variant 로 잇고,
 * 그 안의 version 필드가 XDMA 인지 QDMA 인지를 담는다. 그 값이 실제로
 * 쓰이는 곳은 셋뿐이다.
 *   pcie_read()/pcie_write() — QDMA 이면 모든 레지스터 오프셋에
 *       QDMA_BRIDGE_BASE_OFF(0xcd8)를 더한다. **레지스터 접근마다 분기가
 *       들어가는 구조**다.
 *   map_bus()               — QDMA 이면 cfg_base 를, 아니면 reg_base 를
 *       기준으로 ECAM 오프셋을 얹는다.
 *   parse_dt()              — QDMA 이면 "breg" 자원을 따로 받아
 *       reg_base 로 쓰고, cfg_base 는 ECAM 창으로 둔다.
 * 즉 **XDMA 에서는 레지스터 창과 ECAM 창이 같은 매핑**이고, QDMA 에서만
 * 둘이 갈라진다.
 *
 * === 인터럽트가 하나로 들어와 셋으로 갈라지는 구조 ===
 * 주 인터럽트(platform_get_irq(pdev, 0))가 컨트롤러의 모든 사건을 나른다.
 * 그것을 가운데 도메인 하나로 받아 다시 나눈다.
 *
 *   주 인터럽트
 *     -> xilinx_pl_dma_pcie_event_flow()
 *          IDR(상태) & IMR(마스크)로 처리 대상을 고르고
 *          비트 번호마다 pldma_domain 의 hwirq 를 부른다
 *       -> 사건별 가상 IRQ (setup_irq 가 미리 만들어 두었다)
 *          - 오류·링크 사건 열넷 -> xilinx_pl_dma_pcie_intr_handler()
 *              intr_cause[] 표를 보고 사람이 읽을 문장을 남긴다
 *          - XILINX_PCIE_INTR_INTX -> xilinx_pl_dma_pcie_intx_flow()
 *              IDRN 에서 INTx 넷을 떼어 intx_domain 으로 다시 나눈다
 *
 * MSI 는 이 흐름을 타지 않고 **별도의 인터럽트 둘**("msi0", "msi1")로
 * 들어온다. 벡터가 64개인데 상태 레지스터가 32비트짜리 둘이라 그렇다.
 *
 * 그래서 이 파일에는 "가운데서 갈라 보내는 층" 이 명시적으로 있고, 그
 * 성격을 irq_domain_update_bus_token() 이 표시한다 — 이벤트 도메인은
 * DOMAIN_BUS_NEXUS, INTx 도메인은 DOMAIN_BUS_WIRED 다.
 *
 * === intr_cause 표 ===
 * 사건 번호를 (심볼 이름, 사람이 읽을 문장) 쌍으로 잇는 32칸 표다.
 * _IC() 매크로가 지정 초기화로 그 자리를 채우므로, 비어 있는 칸은
 * str 이 NULL 이다. 그 NULL 을 두 곳이 다르게 쓴다.
 *   setup_irq()      — str 이 NULL 인 칸은 **IRQ 를 만들지 않는다.**
 *                      즉 이 표가 곧 "다룰 사건 목록" 이다.
 *   intr_handler()   — str 이 NULL 이면 "Unknown IRQ" 로 남긴다.
 * 표에 INTX 와 MSI 항목이 없는 것도 그래서다 — 그 둘은 로그로 끝낼
 * 사건이 아니라 다시 나눠 보낼 사건이라 따로 다룬다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 코드에서 읽히는 것만 적는다.
 *   - **XILINX_PCIE_INTR_IMR_ALL_MASK 는 정의만 되어 있고 쓰이지 않는다**
 *     (전수 grep 확인). 게다가 마지막 항목 뒤에도 `|` 가 남아 있어
 *     `... | )` 꼴이므로, 만약 어딘가에서 펼쳐지면 컴파일되지 않는다.
 *     실제로 쓰이는 것은 하드코딩된 XILINX_PCIE_DMA_IMR_ALL_MASK 다.
 *   - "모든 인터럽트를 끈다" 는 자리가 ~XILINX_PCIE_DMA_IDR_ALL_MASK 를
 *     쓰는데, 그 값이 ~0xffffffff 이므로 **0 을 쓰는 것**이다.
 *   - **init_irq_domain() 의 실패 경로가 노드 참조를 놓지 않는다.**
 *     of_get_child_by_name() 이 올린 참조는 성공 경로에서만 놓인다.
 *   - probe 가 xilinx_pl_dma_pcie_setup_irq() 의 반환값을 받아만 두고
 *     **확인하지 않는다.** 곧바로 다음 줄로 넘어간다.
 *   - MSI 핸들러 둘이 irq_find_mapping() + generic_handle_irq() 를 쓴다.
 *     같은 계열의 pcie-xilinx-nwl.c 는 generic_handle_domain_irq() 하나로
 *     처리한다.
 *   - **remove 콜백이 없다.** platform_driver 에 probe 만 있고,
 *     suppress_bind_attrs 로 수동 언바인드도 막혀 있다.
 *
 * === 같은 Xilinx 인 pcie-xilinx-nwl.c 와의 대비 ===
 * 코드에서 확인되는 차이만 적는다. 두 드라이버가 각각 어떤 형태의
 * 하드웨어를 겨냥했는지는 이 소스에 적혀 있지 않으므로 그 인과는 쓰지
 * 않는다.
 *   레지스터 창 : 저쪽은 breg/pcireg/cfg 세 자원이 따로다. 여기는
 *                 pci_ecam_create() 가 만든 창 하나를 레지스터와 ECAM 에
 *                 함께 쓰고(XDMA), QDMA 판만 "breg" 를 따로 받는다.
 *   config 접근 : 양쪽 다 ECAM + generic read/write 다. 여기는
 *                 pci_ecam_ops 로 코어의 ECAM 골격을 통째로 쓰고,
 *                 저쪽은 pci_ops 에 map_bus 만 채운다.
 *   인터럽트    : 저쪽은 하드웨어가 misc/intx/msi0/msi1 넷으로 나눠 주고
 *                 셋이 연쇄 핸들러다. 여기는 주 인터럽트 하나를 32칸짜리
 *                 이벤트 도메인으로 갈라 보내고 INTx 마저 그 도메인의 한
 *                 칸을 거쳐 다시 갈라진다. 연쇄 핸들러는 없다.
 *   INTx 마스크 : 양쪽 다 "마스크" 라는 이름의 레지스터가 실제로는
 *                 활성화 레지스터다(mask 가 비트를 지우고 unmask 가
 *                 세운다). 여기는 IDRN_MASK 의 상위 절반(시프트 16),
 *                 저쪽은 MSGF_LEG_MASK 의 하위 넷이다.
 *   MSI 비트맵  : 저쪽은 구조체 안에 정적으로, 여기는 kzalloc 으로 잡는다.
 *                 개수는 양쪽 다 64다.
 *   MSI 상위    : 여기는 상위 핸들러가 비트에 32 를 더해 hwirq 를 만든다.
 *                 저쪽은 더하지 않아 상위 32개도 0~31 로 전달된다.
 *   MSI-X       : 양쪽 다 지원 플래그에 MSI_FLAG_PCI_MSIX 가 없다.
 *   변종 처리   : 여기는 XDMA/QDMA 를 표로 갈라내고 레지스터 접근마다
 *                 오프셋을 더할지 판단한다. 저쪽은 compatible 이 하나다.
 *   락          : 저쪽은 INTx 마스크 전용 raw_spinlock 하나와 MSI 비트맵
 *                 뮤텍스 하나다. 여기는 raw_spinlock 하나를 **INTx
 *                 마스크와 이벤트 마스크가 함께 쓴다**.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 다만 이 브리지 뒤에 NVMe SSD 를 물리면 **MSI 벡터가 64개**로 제한되고,
 * MSI-X 를 알리지 않으므로 NVMe 는 MSI 로 떨어진다. config 접근은
 * 코어의 ECAM 경로라 열거 자체는 빠르다. 링크가 내려간 상태에서
 * 다운스트림에 접근하면 컨트롤러 전체를 리셋해야 한다는 점을
 * xilinx_pl_dma_pcie_valid_device() 의 상류 주석이 밝히는데, 그 검사와
 * 실제 요청 사이의 경쟁은 남는다고 같은 주석이 적어 두었다.
 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET — INTx 상태를 IDRN 레지스터에서 떼어 낼 때 쓴다 */
#include <linux/interrupt.h>	/* [한국어] devm_request_irq, IRQF_SHARED, IRQF_NO_THREAD, irqreturn_t 등 */
#include <linux/irq.h>	/* [한국어] irq_set_chip_and_handler, handle_level_irq, irq_set_status_flags 등 */
#include <linux/irqchip/irq-msi-lib.h>	/* [한국어] msi_lib_init_dev_msi_info — MSI 부모 도메인을 만들 때 쓰는 공용 헬퍼 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_create_linear, irq_domain_update_bus_token 등. **이 파일은 도메인을 셋 만든다** */
#include <linux/kernel.h>	/* [한국어] 커널 일반 정의 */
#include <linux/module.h>	/* [한국어] 모듈 뼈대. 이 파일은 builtin 으로 등록하지만 헤더는 필요하다 */
#include <linux/msi.h>	/* [한국어] struct msi_msg, msi_create_parent_irq_domain 등 MSI API */
#include <linux/of_address.h>	/* [한국어] of_address 헬퍼 */
#include <linux/of_pci.h>	/* [한국어] of_device_get_match_data 등. 변종을 고르는 데 쓴다 */

#include "../pci.h"	/* [한국어] drivers/pci 안쪽 전용 헤더 */
#include "pcie-xilinx-common.h"	/* [한국어] **Xilinx 계열 공용 인터럽트 비트 번호 표.** pcie-xilinx-cpm.c 와 공유하며, 값이 BIT(n) 이 아니라 번호 n 자체다 */

/* Register definitions */
#define XILINX_PCIE_DMA_REG_IDR			0x00000138	/* [한국어] **인터럽트 상태 레지스터(IDR).** event_flow 가 여기서 사건을 읽는다 */
#define XILINX_PCIE_DMA_REG_IMR			0x0000013c	/* [한국어] **인터럽트 마스크 레지스터(IMR).** 이름과 달리 활성화 레지스터로 쓰인다 */
#define XILINX_PCIE_DMA_REG_PSCR		0x00000144	/* [한국어] PHY 상태·제어 레지스터. 링크 여부가 여기 있다 */
#define XILINX_PCIE_DMA_REG_RPSC		0x00000148	/* [한국어] 루트 포트 상태·제어 레지스터. 브리지 활성화 비트가 여기 있다 */
#define XILINX_PCIE_DMA_REG_MSIBASE1		0x0000014c	/* [한국어] **MSI 목적지 주소 레지스터 1** — 실제로는 상위 워드가 들어간다 */
#define XILINX_PCIE_DMA_REG_MSIBASE2		0x00000150	/* [한국어] 같은 레지스터 2 — 하위 워드가 들어간다. **이름의 번호와 워드 순서가 반대다** */
#define XILINX_PCIE_DMA_REG_RPEFR		0x00000154	/* [한국어] 루트 포트 오류 FIFO. 마지막 오류의 요청자 ID 가 담긴다 */
#define XILINX_PCIE_DMA_REG_IDRN		0x00000160	/* [한국어] **INTx 상태 레지스터(IDRN).** 상위 절반에 INTA~INTD 가 있다 */
#define XILINX_PCIE_DMA_REG_IDRN_MASK		0x00000164	/* [한국어] 그 마스크(=활성화) 레지스터 */
#define XILINX_PCIE_DMA_REG_MSI_LOW		0x00000170	/* [한국어] **MSI 하위 32벡터의 상태 레지스터** */
#define XILINX_PCIE_DMA_REG_MSI_HI		0x00000174	/* [한국어] 상위 32벡터의 상태 레지스터 */
#define XILINX_PCIE_DMA_REG_MSI_LOW_MASK	0x00000178	/* [한국어] 하위 32벡터의 마스크(=활성화) 레지스터 */
#define XILINX_PCIE_DMA_REG_MSI_HI_MASK		0x0000017c	/* [한국어] 상위 32벡터의 마스크(=활성화) 레지스터 */

#define IMR(x) BIT(XILINX_PCIE_INTR_ ##x)	/* [한국어] **공용 헤더의 번호를 비트로 바꾸는 매크로.** ## 로 이름을 이어 붙여 XILINX_PCIE_INTR_<x> 를 만들고 BIT() 로 감싼다 */

/* [한국어] 다룰 사건 열여섯을 IMR() 로 비트로 바꿔 묶은 마스크.
 * [관찰] **이 매크로를 참조하는 곳이 이 트리에 없다**(전수 grep 확인).
 * 게다가 마지막 항목 IMR(MST_SLVERR) 뒤에도 `|` 가 남아 있어 펼치면
 * `... | )` 꼴이 되므로, 어딘가에서 쓰이면 컴파일되지 않는다.
 * 실제로 쓰이는 것은 아래의 하드코딩된 XILINX_PCIE_DMA_IMR_ALL_MASK 다.
 * 줄 잇기 백슬래시로 여러 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define XILINX_PCIE_INTR_IMR_ALL_MASK	\
	(					\
		IMR(LINK_DOWN)		|	\
		IMR(HOT_RESET)		|	\
		IMR(CFG_TIMEOUT)	|	\
		IMR(CORRECTABLE)	|	\
		IMR(NONFATAL)		|	\
		IMR(FATAL)		|	\
		IMR(INTX)		|	\
		IMR(MSI)		|	\
		IMR(SLV_UNSUPP)		|	\
		IMR(SLV_UNEXP)		|	\
		IMR(SLV_COMPL)		|	\
		IMR(SLV_ERRP)		|	\
		IMR(SLV_CMPABT)		|	\
		IMR(SLV_ILLBUR)		|	\
		IMR(MST_DECERR)		|	\
		IMR(MST_SLVERR)		|	\
	)

#define XILINX_PCIE_DMA_IMR_ALL_MASK	0x0ff30fe9	/* [한국어] **실제로 쓰이는 인터럽트 마스크.** init_port 가 남아 있던 상태를 지울 때 이 값으로 거른다. 위 매크로 조합이 아니라 하드코딩된 상수다 */
#define XILINX_PCIE_DMA_IDR_ALL_MASK	0xffffffff	/* [한국어] 전체 비트. init_port 가 이것의 보수(즉 0)를 IMR 에 써서 모든 사건을 막고, MSI 마스크 둘에는 이 값을 그대로 써서 전부 연다 */
#define XILINX_PCIE_DMA_IDRN_MASK	GENMASK(19, 16)	/* [한국어] **IDRN 안에서 INTx 넷이 차지하는 자리**(비트 19~16). intx_flow 가 FIELD_GET 으로 이 자리를 떼어 낸다 */

/* Root Port Error Register definitions */
#define XILINX_PCIE_DMA_RPEFR_ERR_VALID	BIT(18)	/* [한국어] 오류 FIFO 에 유효한 기록이 있는지 알리는 비트 */
#define XILINX_PCIE_DMA_RPEFR_REQ_ID	GENMASK(15, 0)	/* [한국어] 그 안의 요청자 ID 필드 */
#define XILINX_PCIE_DMA_RPEFR_ALL_MASK	0xffffffff	/* [한국어] 전체 비트. 1 을 써서 지우는 방식이라 이 값을 그대로 쓴다 */

/* Root Port Interrupt Register definitions */
#define XILINX_PCIE_DMA_IDRN_SHIFT	16	/* [한국어] **INTx 비트를 IDRN_MASK 에서 만들 때 더할 시프트.** hwirq(0~3)에 16 을 더해 비트 위치를 만든다 */

/* Root Port Status/control Register definitions */
#define XILINX_PCIE_DMA_REG_RPSC_BEN	BIT(0)	/* [한국어] **브리지 활성화 비트.** init_port 가 마지막에 이것을 세워야 컨트롤러가 트랜잭션을 넘긴다 */

/* Phy Status/Control Register definitions */
#define XILINX_PCIE_DMA_REG_PSCR_LNKUP	BIT(11)	/* [한국어] 링크가 섰음을 알리는 비트 */
#define QDMA_BRIDGE_BASE_OFF		0xcd8	/* [한국어] **QDMA 판에서 모든 레지스터 오프셋에 더할 값.** pcie_read/pcie_write 가 변종을 보고 더한다 */

/* Number of MSI IRQs */
#define XILINX_NUM_MSI_IRQS	64	/* [한국어] **MSI 벡터 수. 상태 레지스터가 32비트짜리 둘이라 64개다** */

enum xilinx_pl_dma_version {
	/* [한국어]
	 * XDMA - Xilinx DMA 브리지 판.
	 * 설정자: of_match 표의 "xlnx,xdma-host-3.00" 항목이 xdma_host 를 가리키고
	 * 그 안의 version 이 이 값이다.
	 * 읽는 자: pcie_read()/pcie_write()/map_bus()/parse_dt() 가 QDMA 인지
	 * 아닌지를 볼 때. 즉 **이 값은 "QDMA 가 아니다" 로만 쓰인다.**
	 * 특징: 레지스터 창과 ECAM 창이 같은 매핑이고 레지스터 오프셋 보정이 없다.
	 */
	XDMA,
	/* [한국어]
	 * QDMA - Xilinx Queue DMA 브리지 판.
	 * 설정자: of_match 표의 "xlnx,qdma-host-3.00" 항목.
	 * 읽는 자: 위 넷이 모두 이 값인지 비교한다.
	 * 특징: 장치 트리의 "breg" 자원을 따로 받아 레지스터 창으로 쓰고, 모든
	 * 레지스터 오프셋에 QDMA_BRIDGE_BASE_OFF(0xcd8)를 더한다.
	 */
	QDMA,
};

/**
 * struct xilinx_pl_dma_variant - PL DMA PCIe variant information
 * @version: DMA version
 */
struct xilinx_pl_dma_variant {
	/* [한국어]
	 * enum xilinx_pl_dma_version version;
	 * 이 항목이 XDMA 인지 QDMA 인지. 바로 위 상류 kernel-doc 이 "DMA version"
	 * 이라 적는다.
	 * 설정자: 파일 끝의 xdma_host / qdma_host 두 정적 구조체.
	 * 읽는 자: pcie_read(), pcie_write(), xilinx_pl_dma_pcie_map_bus(),
	 * xilinx_pl_dma_pcie_parse_dt() 네 곳뿐이다.
	 * 값 범위: XDMA 또는 QDMA.
	 * 동기화: 표가 const 라 읽기 전용이며 락이 없다.
	 * [관찰] **struct 에 필드가 이것 하나뿐이다.** 앞으로 변종별 차이가 늘 때
	 * 필드를 더할 자리로 남겨 둔 형태로 보이나, 코드에 그 근거는 없다.
	 */
	enum xilinx_pl_dma_version version;
};

struct xilinx_msi {
	/* [한국어]
	 * unsigned long *bitmap;
	 * 64개 MSI 벡터 중 어느 것이 쓰이고 있는지를 담은 비트맵.
	 * 설정자: xilinx_pl_dma_pcie_init_msi_irq_domain() 이 kzalloc 으로 잡는다.
	 * 크기는 BITS_TO_LONGS(XILINX_NUM_MSI_IRQS) x sizeof(long) 이다.
	 * 읽는 자: xilinx_irq_domain_alloc() 의 bitmap_find_free_region() 과
	 * xilinx_irq_domain_free() 의 bitmap_release_region().
	 * 값 범위: 비트 하나가 벡터 하나. **연속이고 2의 거듭제곱 개수인 구간으로만
	 * 잡힌다** — 다중 MSI 의 규격 제약 때문이다.
	 * 동기화: 아래 lock 뮤텍스로 지킨다.
	 * [관찰] kfree 하는 코드가 이 파일에 없다. 같은 계열의 pcie-xilinx-nwl.c 는
	 * 같은 크기의 비트맵을 구조체 안에 정적으로 둔다.
	 */
	unsigned long		*bitmap;
	/* [한국어]
	 * struct irq_domain *dev_domain;
	 * MSI 부모 도메인. 크기가 XILINX_NUM_MSI_IRQS(64)다.
	 * 설정자: init_msi_irq_domain() 의 msi_create_parent_irq_domain().
	 * xilinx_pl_dma_pcie_free_irq_domains() 가 없애고 NULL 로 둔다.
	 * 읽는 자: MSI 핸들러 둘이 irq_find_mapping() 의 인자로 쓴다.
	 */
	struct irq_domain	*dev_domain;
	/* [한국어]
	 * struct mutex lock;
	 * 위 비트맵을 지키는 뮤텍스. 옆의 상류 주석이 그 용도를 밝힌다.
	 * 설정자: init_msi_irq_domain() 의 mutex_init().
	 * 읽는 자: MSI 벡터 할당과 해제 두 곳뿐이다.
	 * 왜 스핀락이 아닌가: 그 두 경로가 프로세스 컨텍스트에서만 불려 잠들 수
	 * 있기 때문이다. 인터럽트 컨텍스트에서 도는 MSI 핸들러들은 비트맵을 보지
	 * 않으므로 이 락과 무관하다.
	 * **아래 struct pl_dma_pcie 의 lock(raw_spinlock)과는 다른 락**이다.
	 */
	struct mutex		lock;		/* Protect bitmap variable */
	/* [한국어]
	 * int irq_msi0;
	 * 하위 32개 MSI 벡터를 나르는 인터럽트 번호.
	 * 설정자: xilinx_request_msi_irq() 가 장치 트리의 "msi0" 이름으로 얻는다.
	 * 읽는 자: 같은 함수가 곧바로 devm_request_irq() 에 넘긴다. 그 뒤로 읽는
	 * 곳은 없다.
	 * 값 범위: 0 이하이면 실패로 다룬다.
	 */
	int			irq_msi0;
	/* [한국어]
	 * int irq_msi1;
	 * 상위 32개 MSI 벡터를 나르는 인터럽트 번호.
	 * 설정자/읽는 자는 irq_msi0 과 같으며 이름이 "msi1" 이고 붙는 핸들러가
	 * 상위 판인 것만 다르다.
	 * **두 개인 이유**는 MSI 상태 레지스터가 32비트짜리 둘이고 벡터가 64개이기
	 * 때문이다.
	 */
	int			irq_msi1;
};

/**
 * struct pl_dma_pcie - PCIe port information
 * @dev: Device pointer
 * @reg_base: IO Mapped Register Base
 * @cfg_base: IO Mapped Configuration Base
 * @irq: Interrupt number
 * @cfg: Holds mappings of config space window
 * @phys_reg_base: Physical address of reg base
 * @intx_domain: Legacy IRQ domain pointer
 * @pldma_domain: PL DMA IRQ domain pointer
 * @resources: Bus Resources
 * @msi: MSI information
 * @intx_irq: INTx error interrupt number
 * @lock: Lock protecting shared register access
 * @variant: PL DMA PCIe version check pointer
 */
struct pl_dma_pcie {
	/* [한국어]
	 * struct device *dev;
	 * 이 드라이버가 붙은 장치. 바로 위 상류 kernel-doc 이 "Device pointer"
	 * 라 적는다.
	 * 설정자: xilinx_pl_dma_pcie_probe() 가 맨 처음 담는다.
	 * 읽는 자: 파일 전체에서 오류 메시지를 낼 장치와 of_node 의 출처로 쓴다.
	 */
	struct device			*dev;
	/* [한국어]
	 * void __iomem *reg_base;
	 * **컨트롤러 레지스터 창의 커널 가상 주소.** 상류 kernel-doc 이 "IO Mapped
	 * Register Base" 라 적는다.
	 * 설정자: parse_dt 가 변종에 따라 다르게 채운다 — XDMA 이면 ECAM 창과 같은
	 * cfg->win, QDMA 이면 "breg" 자원을 따로 매핑한 것이다.
	 * 읽는 자: pcie_read()/pcie_write() 둘과, XDMA 경로의 map_bus().
	 * **QDMA 에서는 여기에 QDMA_BRIDGE_BASE_OFF 가 더해진다.**
	 */
	void __iomem			*reg_base;
	/* [한국어]
	 * void __iomem *cfg_base;
	 * ECAM 창의 커널 가상 주소. 상류 kernel-doc 이 "IO Mapped Configuration
	 * Base" 라 적는다.
	 * 설정자: parse_dt 가 **QDMA 일 때만** 채운다(cfg->win).
	 * 읽는 자: map_bus() 의 QDMA 갈래뿐이다.
	 * 값 범위: XDMA 에서는 0 인 채로 남고 쓰이지도 않는다.
	 */
	void __iomem			*cfg_base;
	/* [한국어]
	 * int irq;
	 * 주 인터럽트 번호. 컨트롤러의 모든 사건이 이 하나로 들어온다.
	 * 상류 kernel-doc 이 "Interrupt number" 라 적는다.
	 * 설정자: xilinx_pl_dma_pcie_setup_irq() 가 platform_get_irq(pdev, 0) 로
	 * 얻는다 — **이름이 아니라 번호 0 으로** 얻는다.
	 * 읽는 자: 같은 함수가 event_flow 핸들러를 걸고 실패 메시지에도 쓴다.
	 */
	int				irq;
	/* [한국어]
	 * struct pci_config_window *cfg;
	 * 코어가 만들어 준 ECAM 창 관리 구조체. 상류 kernel-doc 이 "Holds mappings
	 * of config space window" 라 적는다.
	 * 설정자: parse_dt 의 pci_ecam_create().
	 * 읽는 자: parse_dt 가 cfg->win 을 꺼내 쓰고, 실패 경로와 probe 의
	 * err_irq_domain 이 pci_ecam_free() 에 넘긴다.
	 * **config 창 관리를 통째로 코어에 맡긴 결과**이며, 같은 계열의
	 * pcie-xilinx-nwl.c 가 창 셋을 손으로 매핑하는 것과 다르다.
	 */
	struct pci_config_window	*cfg;
	/* [한국어]
	 * phys_addr_t phys_reg_base;
	 * 레지스터 창의 **물리** 주소. 상류 kernel-doc 이 그렇게 적는다.
	 * 설정자: parse_dt 가 첫 MEM 자원의 start 를 담고, QDMA 이면 "breg"
	 * 자원의 start 로 덮어쓴다.
	 * 읽는 자: **두 곳이 같은 값을 쓴다.** xilinx_pl_dma_pcie_enable_msi() 가
	 * MSIBASE1/2 에 심고, xilinx_compose_msi_msg() 가 장치에 알려 줄 MSI
	 * 목적지 주소로 쓴다.
	 */
	phys_addr_t			phys_reg_base;
	/* [한국어]
	 * struct irq_domain *intx_domain;
	 * INTx(레거시) 인터럽트 도메인. 크기가 PCI_NUM_INTX(4)다. 상류
	 * kernel-doc 이 "Legacy IRQ domain pointer" 라 적는다.
	 * 설정자: init_irq_domain() 이 만들고 버스 토큰을 DOMAIN_BUS_WIRED 로
	 * 바꾼다. free_irq_domains() 가 없애고 NULL 로 둔다.
	 * 읽는 자: xilinx_pl_dma_pcie_intx_flow() 가
	 * generic_handle_domain_irq() 의 인자로 쓴다.
	 */
	struct irq_domain		*intx_domain;
	/* [한국어]
	 * struct irq_domain *pldma_domain;
	 * **컨트롤러 자신의 사건 도메인.** 크기가 32이고 hwirq 가
	 * XILINX_PCIE_INTR_* 번호와 같다. 상류 kernel-doc 이 "PL DMA IRQ domain
	 * pointer" 라 적는다.
	 * 설정자: init_irq_domain() 이 만들고 버스 토큰을 **DOMAIN_BUS_NEXUS** 로
	 * 바꾼다 — 가운데서 갈라 보내는 층이라는 표시다.
	 * 읽는 자: event_flow() 가 사건을 보낼 때, setup_irq() 가
	 * irq_create_mapping() 으로 IRQ 를 만들 때, intr_handler() 가 hwirq 를
	 * 되짚을 때.
	 * [관찰] **이 도메인을 없애는 코드는 이 파일에 없다.**
	 * free_irq_domains() 도 INTx 와 MSI 만 다룬다.
	 */
	struct irq_domain		*pldma_domain;
	/* [한국어]
	 * struct list_head resources;
	 * 상류 kernel-doc 이 "Bus Resources" 라 적는다.
	 * [관찰] **이 파일에서 읽거나 쓰는 곳이 없다**(전수 grep 확인). 버스
	 * 자원은 probe 가 bridge->windows 에서 직접 꺼내 쓴다.
	 */
	struct list_head		resources;
	/* [한국어]
	 * struct xilinx_msi msi;
	 * MSI 관련 상태를 모아 둔 하위 구조체(비트맵, 도메인, 뮤텍스, 인터럽트
	 * 번호 둘). 상류 kernel-doc 이 "MSI information" 이라 적는다.
	 * 포인터가 아니라 값으로 품고 있어 별도 할당이 없다.
	 */
	struct xilinx_msi		msi;
	/* [한국어]
	 * int intx_irq;
	 * INTx 사건에 배정된 **가상** IRQ 번호. 상류 kernel-doc 이 "INTx error
	 * interrupt number" 라 적는다.
	 * 설정자: setup_irq() 가 irq_create_mapping(pldma_domain,
	 * XILINX_PCIE_INTR_INTX) 로 만든다 — 하드웨어 인터럽트 번호가 아니라
	 * 이벤트 도메인 위의 가상 번호다.
	 * 읽는 자: 같은 함수가 devm_request_irq() 에 넘기고 실패 메시지에도 쓴다.
	 */
	int				intx_irq;
	/* [한국어]
	 * raw_spinlock_t lock;
	 * 공유 레지스터 접근을 지키는 락. 상류 kernel-doc 이 "Lock protecting
	 * shared register access" 라 적는다.
	 * 설정자: init_msi_irq_domain() 과 init_irq_domain() **두 곳이 초기화한다**
	 * (호출 순서상 앞이 먼저, 뒤가 나중에 다시).
	 * 읽는 자: **두 종류가 함께 쓴다** — INTx 마스크·언마스크(IDRN_MASK)는
	 * irqsave 판으로, 이벤트 마스크·언마스크(IMR)는 irqsave 없는 판으로 잡는다.
	 * 왜 raw 인가: 레벨 인터럽트라 핸들러가 처리 중에 마스크를 걸어 인터럽트
	 * 컨텍스트에서도 불리기 때문이다.
	 * [관찰] 지키는 레지스터가 둘인데 락은 하나이므로, INTx 마스크가 이벤트
	 * 마스크를 막는다. 같은 계열의 pcie-xilinx-nwl.c 는 INTx 전용 락 하나만
	 * 둔다(이벤트 마스크에 해당하는 것이 없다).
	 */
	raw_spinlock_t			lock;
	/* [한국어]
	 * const struct xilinx_pl_dma_variant *variant;
	 * **이 인스턴스가 XDMA 인지 QDMA 인지를 가리키는 표 항목.** 상류
	 * kernel-doc 이 "PL DMA PCIe version check pointer" 라 적는다.
	 * 설정자: probe 의 of_device_get_match_data(dev) 한 줄.
	 * 읽는 자: pcie_read(), pcie_write(), map_bus(), parse_dt() 넷.
	 * **레지스터 접근마다 이 포인터를 따라간다.**
	 * 값 범위: 파일 끝의 xdma_host 또는 qdma_host 를 가리키며 const 라
	 * 바뀌지 않는다.
	 */
	const struct xilinx_pl_dma_variant   *variant;
};

/* [한국어]
 * pcie_read - 컨트롤러 레지스터에서 32비트를 읽는다(변종에 따라 오프셋 보정)
 *
 * @port: 이 드라이버 인스턴스.
 * @reg:  레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 이 파일이 하드웨어를 만지는 두 통로 중 하나이며, **변종 분기가 여기
 * 들어 있다.** QDMA 이면 오프셋에 QDMA_BRIDGE_BASE_OFF(0xcd8)를 더하고,
 * XDMA 이면 그대로 쓴다. 즉 레지스터 접근 하나하나가 변종을 확인한다.
 *
 * reg_base 가 가리키는 곳도 변종에 따라 다르다 — XDMA 에서는
 * pci_ecam_create() 가 만든 ECAM 창과 같은 매핑이고, QDMA 에서는
 * 장치 트리의 "breg" 자원을 따로 매핑한 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽. 그 자체로는
 * 직렬화하지 않으며, 필요한 곳에서 호출자가 락을 잡는다.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> readl()
 */
static inline u32 pcie_read(struct pl_dma_pcie *port, u32 reg)
{
	if (port->variant->version == QDMA)	/* [한국어] **변종 분기가 레지스터 접근마다 들어 있다** */
		return readl(port->reg_base + reg + QDMA_BRIDGE_BASE_OFF);	/* [한국어] QDMA 이면 오프셋에 브리지 기준 오프셋을 더한다 */

	return readl(port->reg_base + reg);	/* [한국어] XDMA 이면 그대로 읽는다 */
}

/* [한국어]
 * pcie_write - 컨트롤러 레지스터에 32비트를 쓴다(변종에 따라 오프셋 보정)
 *
 * @port: 이 드라이버 인스턴스.
 * @val:  쓸 값.
 * @reg:  레지스터 오프셋.
 *
 * pcie_read() 의 짝이며 같은 변종 분기를 갖는다. 읽기 쪽이 삼항 없이
 * early return 을 쓰는 반면 이쪽은 if/else 로 갈라 두었지만 뜻은 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> writel()
 */
static inline void pcie_write(struct pl_dma_pcie *port, u32 val, u32 reg)
{
	if (port->variant->version == QDMA)	/* [한국어] 같은 변종 분기 */
		writel(val, port->reg_base + reg + QDMA_BRIDGE_BASE_OFF);	/* [한국어] QDMA 이면 오프셋을 보정해 쓰고 */
	else	/* [한국어] 아니면 */
		writel(val, port->reg_base + reg);	/* [한국어] 그대로 쓴다. 읽기 쪽이 early return 을 쓴 반면 이쪽은 if/else 로 갈랐을 뿐 뜻은 같다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_link_up - PCIe 링크가 서 있는지 본다
 *
 * @port: 이 드라이버 인스턴스.
 * @return: true 면 링크가 서 있다.
 *
 * PHY 상태·제어 레지스터(PSCR)의 LNKUP 비트 하나만 본다. 같은 계열의
 * pcie-xilinx-nwl.c 가 링크 판별을 둘로 나눈 것(PCIe 링크와 PHY 준비)과
 * 달리 여기는 하나뿐이다.
 *
 * 쓰이는 곳이 둘이다 — config 접근을 허용할지 가릴 때
 * (xilinx_pl_dma_pcie_valid_device)와 초기화 끝의 로그다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근, probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_valid_device() / xilinx_pl_dma_pcie_init_port()
 *     -> [이 함수] -> pcie_read()
 */
static inline bool xilinx_pl_dma_pcie_link_up(struct pl_dma_pcie *port)
{
	return (pcie_read(port, XILINX_PCIE_DMA_REG_PSCR) &	/* [한국어] PHY 상태·제어 레지스터를 읽어 */
		XILINX_PCIE_DMA_REG_PSCR_LNKUP) ? true : false;	/* [한국어] 링크 비트가 서 있는지 본다. **같은 계열의 pcie-xilinx-nwl.c 가 링크 판별을 둘로 나눈 것과 달리 여기는 하나뿐이다** */
}

/* [한국어]
 * xilinx_pl_dma_pcie_clear_err_interrupts - 루트 포트 오류 FIFO 를 읽고 지운다
 *
 * @port: 이 드라이버 인스턴스.
 *
 * RPEFR(Root Port Error FIFO Register)에 마지막 오류의 요청자 ID 가 담긴다.
 * 그 안의 ERR_VALID 비트가 서 있을 때만 요청자 ID 를 디버그 로그로 남기고,
 * **전체 비트를 써서 지운다**(1 을 써서 지우는 방식).
 *
 * 부르는 곳은 xilinx_pl_dma_pcie_intr_handler() 의 AER 세 갈래
 * (CORRECTABLE, NONFATAL, FATAL)뿐이다 — 그 세 사건에서만 오류를 낸
 * 장치가 누구인지 알 수 있기 때문이다.
 *
 * 로그가 dev_dbg 라 평소에는 보이지 않는다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_intr_handler() -> [이 함수]
 *     -> pcie_read() -> pcie_write()
 */
static void xilinx_pl_dma_pcie_clear_err_interrupts(struct pl_dma_pcie *port)
{
	unsigned long val = pcie_read(port, XILINX_PCIE_DMA_REG_RPEFR);	/* [한국어] 루트 포트 오류 FIFO 를 읽는다. 마지막 오류의 요청자 ID 가 담겨 있다 */

	if (val & XILINX_PCIE_DMA_RPEFR_ERR_VALID) {	/* [한국어] **유효한 기록이 있을 때만** */
		dev_dbg(port->dev, "Requester ID %lu\n",	/* [한국어] 요청자 ID 를 디버그 로그로 남긴다 — dev_dbg 라 평소에는 보이지 않는다 */
			val & XILINX_PCIE_DMA_RPEFR_REQ_ID);	/* [한국어] 하위 16비트가 그 필드다 */
		pcie_write(port, XILINX_PCIE_DMA_RPEFR_ALL_MASK,	/* [한국어] **전체 비트를 써서 지운다**(1 을 써서 지우는 방식) */
			   XILINX_PCIE_DMA_REG_RPEFR);	/* [한국어] 대상 레지스터 */
	}
}

/* [한국어]
 * xilinx_pl_dma_pcie_valid_device - 이 (버스, devfn) 조합에 config 접근을 해도 되는지 가른다
 *
 * @bus:   대상 버스. sysdata 에 이 드라이버 인스턴스가 들어 있다.
 * @devfn: 대상 장치·함수 번호.
 * @return: true 면 접근해도 된다.
 *
 * 기준이 둘이다.
 *   - **루트 버스가 아니면 링크가 서 있어야 한다.** 그 자리의 긴 상류
 *     주석이 이유와 한계를 함께 밝힌다 — 링크가 내려간 상태에서
 *     다운스트림 장치에 PIO 요청을 보내면 복구 불가능한 오류가 나고
 *     PCIe 컨트롤러 전체를 리셋해야 한다. 링크를 확인하면 그 가능성을
 *     줄일 수 있지만, **확인과 요청 사이에 링크가 내려갈 수 있어 완전히
 *     막지는 못한다**고 같은 주석이 적는다.
 *   - **루트 버스에서는 devfn 이 0 이어야 한다.** 그 자리의 상류 주석대로
 *     루트 포트 하나에 장치 하나만 매달린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_map_bus() -> [이 함수]
 *     -> xilinx_pl_dma_pcie_link_up()
 */
static bool xilinx_pl_dma_pcie_valid_device(struct pci_bus *bus,
					    unsigned int devfn)
{
	struct pl_dma_pcie *port = bus->sysdata;	/* [한국어] 버스의 sysdata 에 이 드라이버 인스턴스가 들어 있다 */

	if (!pci_is_root_bus(bus)) {	/* [한국어] **루트 버스가 아니면 다운스트림 접근이다** */
		/*
		 * Checking whether the link is up is the last line of
		 * defense, and this check is inherently racy by definition.
		 * Sending a PIO request to a downstream device when the link is
		 * down causes an unrecoverable error, and a reset of the entire
		 * PCIe controller will be needed. We can reduce the likelihood
		 * of that unrecoverable error by checking whether the link is
		 * up, but we can't completely prevent it because the link may
		 * go down between the link-up check and the PIO request.
		 */
		if (!xilinx_pl_dma_pcie_link_up(port))	/* [한국어] 바로 위 상류 주석대로 링크가 내려간 상태에서 요청을 보내면 복구 불가능한 오류가 나므로 먼저 확인한다 */
			return false;	/* [한국어] 링크가 없으면 접근을 거절한다 */
	} else if (devfn > 0)	/* [한국어] **루트 버스인데 devfn 이 0 이 아니면** */
		/* Only one device down on each root port */
		return false;	/* [한국어] 거절한다. 바로 위 상류 주석대로 루트 포트 하나에 장치 하나만 매달린다 */

	return true;	/* [한국어] 두 관문을 지났으면 접근해도 된다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_map_bus - config 접근에 쓸 ECAM 주소를 만들어 준다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치·함수 번호.
 * @where: 읽거나 쓸 오프셋.
 * @return: 접근할 커널 가상 주소. 접근하면 안 되는 조합이면 NULL.
 *
 * **ECAM 창이 있어 config 접근이 단순한 메모리 접근이다.** 이 파일이
 * 하는 일은 주소를 만들어 주는 것뿐이고, 실제 읽기·쓰기는
 * struct pci_ecam_ops 안에 매단 pci_generic_config_read/write 가 한다.
 *
 * 기준이 되는 창이 **변종에 따라 다르다** — QDMA 이면 cfg_base,
 * XDMA 이면 reg_base 다. XDMA 에서는 그 둘이 같은 매핑이므로 결과가
 * 같지만, QDMA 에서는 레지스터 창과 ECAM 창이 갈라져 있어 구분이 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거와 그 뒤의 모든 config 접근).
 *
 * 호출 체인:
 *   PCI 코어 -> pci_ops.map_bus -> [이 함수]
 *     -> xilinx_pl_dma_pcie_valid_device()
 */
static void __iomem *xilinx_pl_dma_pcie_map_bus(struct pci_bus *bus,
						unsigned int devfn, int where)
{
	struct pl_dma_pcie *port = bus->sysdata;	/* [한국어] 버스의 sysdata 에서 이 드라이버 인스턴스를 꺼낸다 */

	if (!xilinx_pl_dma_pcie_valid_device(bus, devfn))	/* [한국어] 존재할 수 없거나 위험한 조합이면 */
		return NULL;	/* [한국어] NULL 을 돌려준다 — 코어가 "장치 없음" 으로 다룬다 */

	if (port->variant->version == QDMA)	/* [한국어] **변종에 따라 기준 창이 다르다** */
		return port->cfg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);	/* [한국어] QDMA 이면 ECAM 창을 기준으로 오프셋을 얹고 */

	return port->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);	/* [한국어] XDMA 이면 레지스터 창을 기준으로 얹는다 — 그쪽에서는 두 창이 같은 매핑이라 결과가 같다 */
}

/* PCIe operations */
static struct pci_ecam_ops xilinx_pl_dma_pcie_ops = {	/* [한국어] **PCI 코어의 ECAM 골격을 그대로 쓴다.** 같은 계열의 pcie-xilinx-nwl.c 가 pci_ops 만 채우는 것과 달리 여기는 pci_ecam_ops 를 통째로 넘긴다 */
	.pci_ops = {	/* [한국어] 그 안에 담긴 config 동작 */
		.map_bus = xilinx_pl_dma_pcie_map_bus,	/* [한국어] 주소 만들기만 자체 구현하고 */
		.read	= pci_generic_config_read,	/* [한국어] 읽기와 */
		.write	= pci_generic_config_write,	/* [한국어] 쓰기는 코어의 generic 함수에 맡긴다 */
	}
};

/* [한국어]
 * xilinx_pl_dma_pcie_enable_msi - MSI 목적지 주소를 컨트롤러에 심는다
 *
 * @port: 이 드라이버 인스턴스.
 *
 * 엔드포인트가 MSI 를 낼 때 쓸 주소를 하드웨어에 알린다. 그 값은
 * port->phys_reg_base — 레지스터 창의 물리 주소다. 같은 값을
 * xilinx_compose_msi_msg() 가 장치에 알려 주므로, **두 자리가 같은 값을
 * 쓰기로 약속한 구조**다.
 *
 * [관찰] 상위 워드를 MSIBASE1 에, 하위 워드를 MSIBASE2 에 쓴다 —
 * **이름의 번호와 워드의 순서가 반대**다.
 *
 * 부르는 곳은 xilinx_pl_dma_pcie_init_msi_irq_domain() 하나뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_init_msi_irq_domain() -> [이 함수] -> pcie_write()
 */
static void xilinx_pl_dma_pcie_enable_msi(struct pl_dma_pcie *port)
{
	phys_addr_t msi_addr = port->phys_reg_base;	/* [한국어] **MSI 목적지는 레지스터 창의 물리 주소다.** xilinx_compose_msi_msg() 가 장치에 같은 주소를 알려 준다 */

	pcie_write(port, upper_32_bits(msi_addr), XILINX_PCIE_DMA_REG_MSIBASE1);	/* [한국어] **상위 워드를 MSIBASE1 에** */
	pcie_write(port, lower_32_bits(msi_addr), XILINX_PCIE_DMA_REG_MSIBASE2);	/* [한국어] 하위 워드를 MSIBASE2 에 쓴다 — **이름의 번호와 워드 순서가 반대다** */
}

/* [한국어]
 * xilinx_mask_intx_irq - INTx 하나를 마스크한다
 *
 * @data: 마스크할 인터럽트의 irq_data. hwirq 가 INTA~INTD 중 어느 것인지다.
 *
 * **IDRN_MASK 레지스터의 상위 절반이 INTx 자리다** — hwirq 에
 * XILINX_PCIE_DMA_IDRN_SHIFT(16)를 더해 비트 위치를 만든다.
 *
 * **이름과 달리 활성화 레지스터로 쓰인다** — 이 함수가 비트를 지우고
 * 언마스크가 세운다.
 *
 * 읽고-고치고-쓰는 세 단계라 raw_spinlock 으로 감싼다. raw 판이면서
 * irqsave 인 것은 레벨 인터럽트라 핸들러가 처리 중에 마스크를 걸어
 * 인터럽트 컨텍스트에서도 불리기 때문이다.
 *
 * [관찰] 그 락(port->lock)을 이벤트 마스크 함수들도 함께 쓴다 — 지키는
 * 레지스터가 IDRN_MASK 와 IMR 로 다른데 락은 하나다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_mask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> pcie_read()/pcie_write()
 */
static void xilinx_mask_intx_irq(struct irq_data *data)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask, val;	/* [한국어] 만질 비트와 읽고 고칠 레지스터 값 */

	mask = BIT(data->hwirq + XILINX_PCIE_DMA_IDRN_SHIFT);	/* [한국어] **hwirq(0~3)에 16 을 더해 IDRN_MASK 안의 자리를 만든다** — INTx 넷이 상위 절반에 있다 */
	raw_spin_lock_irqsave(&port->lock, flags);	/* [한국어] **읽고-고치고-쓰기를 원자적으로 한다.** 레벨 인터럽트라 인터럽트 컨텍스트에서도 불린다 */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDRN_MASK);	/* [한국어] 현재 값을 읽고 */
	pcie_write(port, (val & (~mask)), XILINX_PCIE_DMA_REG_IDRN_MASK);	/* [한국어] **그 비트를 지운다.** 이 레지스터는 이름과 달리 활성화 레지스터라 지우는 것이 막는 것이다 */
	raw_spin_unlock_irqrestore(&port->lock, flags);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * xilinx_unmask_intx_irq - INTx 하나의 마스크를 푼다
 *
 * @data: 마스크를 풀 인터럽트의 irq_data.
 *
 * xilinx_mask_intx_irq() 의 짝이며 비트를 세우는 것만 다르다. 같은
 * raw_spinlock 으로 읽고-고치고-쓰기를 보호한다.
 *
 * 레벨 인터럽트에서는 장치가 원인을 없앤 뒤 이 함수가 불려야 다음
 * 인터럽트를 받을 수 있으므로, 실질적으로 인터럽트 처리 주기를 닫는
 * 역할을 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_unmask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> pcie_read()/pcie_write()
 */
static void xilinx_unmask_intx_irq(struct irq_data *data)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask, val;	/* [한국어] 만질 비트와 읽고 고칠 레지스터 값 */

	mask = BIT(data->hwirq + XILINX_PCIE_DMA_IDRN_SHIFT);	/* [한국어] 같은 방식으로 비트 자리를 만든다 */
	raw_spin_lock_irqsave(&port->lock, flags);	/* [한국어] 같은 락으로 감싼다 */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDRN_MASK);	/* [한국어] 현재 값을 읽고 */
	pcie_write(port, (val | mask), XILINX_PCIE_DMA_REG_IDRN_MASK);	/* [한국어] **그 비트를 세운다.** 세우는 것이 통과시키는 것이다 */
	raw_spin_unlock_irqrestore(&port->lock, flags);	/* [한국어] 락을 푼다 */
}

static struct irq_chip xilinx_leg_irq_chip = {	/* [한국어] **INTx 용 irq_chip. 정적 구조체 하나를 공유한다** */
	.name		= "pl_dma:INTx",	/* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_mask	= xilinx_mask_intx_irq,	/* [한국어] 마스크와 */
	.irq_unmask	= xilinx_unmask_intx_irq,	/* [한국어] 언마스크만 매단다 — enable/disable 은 코어가 이 둘로 대신한다 */
};

/* [한국어]
 * xilinx_pl_dma_pcie_intx_map - INTx 가상 IRQ 하나를 이 컨트롤러에 잇는다
 *
 * @domain: INTx irq 도메인.
 * @irq:    커널이 배정한 가상 IRQ 번호.
 * @hwirq:  INTA~INTD 중 몇 번째인지(0~3).
 * @return: 항상 0.
 *
 * 셋을 한다 — irq_chip 과 handle_level_irq 를 매달고, chip_data 에
 * 드라이버 인스턴스를 담고, IRQ_LEVEL 상태 플래그를 세운다.
 *
 * [관찰] 이 도메인의 ops 에는 **xlate 가 없다.** 같은 계열의
 * pcie-xilinx-nwl.c 는 pci_irqd_intx_xlate 를 채운다. 그래서 이 도메인의
 * hwirq 는 장치 트리 해석이 아니라 이 파일의
 * xilinx_pl_dma_pcie_intx_flow() 가 직접 넘기는 번호로 정해진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(가상 IRQ 생성 시).
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_domain_ops.map -> [이 함수]
 *     -> irq_set_chip_and_handler() -> irq_set_status_flags()
 */
static int xilinx_pl_dma_pcie_intx_map(struct irq_domain *domain,
				       unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_leg_irq_chip, handle_level_irq);	/* [한국어] irq_chip 과 레벨 핸들러를 매단다 */
	irq_set_chip_data(irq, domain->host_data);	/* [한국어] 마스크·언마스크에서 꺼내 쓸 수 있게 인스턴스를 담아 둔다 */
	irq_set_status_flags(irq, IRQ_LEVEL);	/* [한국어] **INTx 가 레벨 트리거임을 알린다** */

	return 0;	/* [한국어] 매핑에 실패할 일이 없어 늘 성공이다 */
}

/* INTx IRQ Domain operations */
static const struct irq_domain_ops intx_domain_ops = {	/* [한국어] **INTx 도메인의 동작. map 만 있고 xlate 가 없다** — 같은 계열의 pcie-xilinx-nwl.c 는 pci_irqd_intx_xlate 를 채운다 */
	.map = xilinx_pl_dma_pcie_intx_map,	/* [한국어] 가상 IRQ 를 만들 때 부를 함수 */
};

/* [한국어]
 * xilinx_pl_dma_pcie_msi_handler_high - 상위 32개 MSI 벡터를 받는다
 *
 * @irq:  리눅스 IRQ 번호. 쓰지 않는다.
 * @args: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 항상 IRQ_HANDLED.
 *
 * 장치 트리의 "msi1" 인터럽트에 붙는다. **연쇄 핸들러가 아니라 보통의
 * 핸들러**로 등록되며, 그 점이 pcie-xilinx-nwl.c 와 다르다.
 *
 * 상태 레지스터가 0 이 될 때까지 반복하며, 세워진 비트마다 **먼저 그
 * 비트를 지우고** 해당 벡터의 가상 IRQ 를 부른다.
 *
 * **비트 번호에 32 를 더해 hwirq 를 만든다** — 상위 레지스터의 0번 비트가
 * 벡터 32번이기 때문이다. 같은 계열의 pcie-xilinx-nwl.c 는 이 덧셈을 하지
 * 않는다.
 *
 * 배분에 irq_find_mapping() + generic_handle_irq() 두 단계를 쓰고,
 * 매핑이 없으면(virq 가 0이면) 조용히 넘어간다.
 *
 * [관찰] 우리 인터럽트가 아니어도 늘 IRQ_HANDLED 를 돌려준다.
 * IRQF_SHARED 로 등록되므로 선을 공유할 수 있는데, 상태가 0 이면
 * while 을 한 번도 돌지 않고 그대로 HANDLED 로 나간다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**(IRQF_NO_THREAD).
 *
 * 호출 체인:
 *   GIC -> IRQ 코어 -> [이 함수] -> pcie_read()/pcie_write()
 *     -> irq_find_mapping() -> generic_handle_irq()
 */
static irqreturn_t xilinx_pl_dma_pcie_msi_handler_high(int irq, void *args)
{
	struct xilinx_msi *msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	unsigned long status;	/* [한국어] 읽은 상태 */
	u32 bit, virq;	/* [한국어] 세워진 비트 번호와 찾아낸 가상 IRQ */
	struct pl_dma_pcie *port = args;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */

	msi = &port->msi;	/* [한국어] 하위 구조체의 위치를 잡아 둔다 */

	while ((status = pcie_read(port, XILINX_PCIE_DMA_REG_MSI_HI)) != 0) {	/* [한국어] **상태가 0 이 될 때까지 반복한다** */
		for_each_set_bit(bit, &status, 32) {	/* [한국어] 32비트를 훑는다 */
			pcie_write(port, 1 << bit, XILINX_PCIE_DMA_REG_MSI_HI);	/* [한국어] **먼저 그 비트를 지운다** — 핸들러가 도는 동안 온 같은 벡터를 잃지 않기 위해서다 */
			bit = bit + 32;	/* [한국어] **비트 번호에 32 를 더해 벡터 번호를 만든다** — 상위 레지스터의 0번이 벡터 32번이다 */
			virq = irq_find_mapping(msi->dev_domain, bit);	/* [한국어] 그 벡터에 매핑된 가상 IRQ 를 찾는다 */
			if (virq)	/* [한국어] 있으면 */
				generic_handle_irq(virq);	/* [한국어] 그 핸들러를 부른다. 없으면 조용히 넘어간다 */
		}
	}

	return IRQ_HANDLED;	/* [한국어] **우리 것이 아니어도 늘 처리했다고 답한다** — 상태가 0 이면 while 을 한 번도 돌지 않고 그대로 나간다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_msi_handler_low - 하위 32개 MSI 벡터를 받는다
 *
 * @irq:  리눅스 IRQ 번호. 쓰지 않는다.
 * @args: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 항상 IRQ_HANDLED.
 *
 * 장치 트리의 "msi0" 인터럽트에 붙는다. 상위 판과 구조가 같고 두 가지가
 * 다르다 — 보는 레지스터가 MSI_LOW 이고, **비트 번호에 32 를 더하지
 * 않는다**(하위 레지스터의 0번 비트가 곧 벡터 0번이다).
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**(IRQF_NO_THREAD).
 *
 * 호출 체인:
 *   GIC -> IRQ 코어 -> [이 함수] -> pcie_read()/pcie_write()
 *     -> irq_find_mapping() -> generic_handle_irq()
 */
static irqreturn_t xilinx_pl_dma_pcie_msi_handler_low(int irq, void *args)
{
	struct pl_dma_pcie *port = args;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */
	struct xilinx_msi *msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	unsigned long status;	/* [한국어] 읽은 상태 */
	u32 bit, virq;	/* [한국어] 세워진 비트 번호와 찾아낸 가상 IRQ */

	msi = &port->msi;	/* [한국어] 하위 구조체의 위치를 잡아 둔다 */

	while ((status = pcie_read(port, XILINX_PCIE_DMA_REG_MSI_LOW)) != 0) {	/* [한국어] **하위 상태 레지스터**를 상태가 0 이 될 때까지 읽는다 */
		for_each_set_bit(bit, &status, 32) {	/* [한국어] 32비트를 훑는다 */
			pcie_write(port, 1 << bit, XILINX_PCIE_DMA_REG_MSI_LOW);	/* [한국어] 먼저 그 비트를 지우고 */
			virq = irq_find_mapping(msi->dev_domain, bit);	/* [한국어] **32 를 더하지 않고** 그대로 벡터 번호로 쓴다 — 하위 레지스터의 0번이 곧 벡터 0번이다 */
			if (virq)	/* [한국어] 매핑이 있으면 */
				generic_handle_irq(virq);	/* [한국어] 그 핸들러를 부른다 */
		}
	}

	return IRQ_HANDLED;	/* [한국어] 상위 판과 마찬가지로 늘 처리했다고 답한다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_event_flow - 주 인터럽트를 받아 사건별 가상 IRQ 로 갈라 보낸다
 *
 * @irq:  리눅스 IRQ 번호. 쓰지 않는다.
 * @args: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 항상 IRQ_HANDLED.
 *
 * **이 드라이버의 인터럽트 구조에서 가운데 층**이다. 컨트롤러의 모든
 * 사건이 이 하나의 인터럽트로 들어오고, 여기서 32칸짜리 이벤트 도메인의
 * hwirq 로 나뉜다.
 *
 *   1. IDR(상태)을 읽고 IMR(마스크)과 AND 해 **처리 대상만 남긴다** —
 *      소프트웨어가 열어 둔 사건만 다루겠다는 뜻이다.
 *   2. 세워진 비트마다 pldma_domain 의 해당 hwirq 를 부른다. 그 가상
 *      IRQ 들은 setup_irq() 가 미리 만들어 두었으며, 사건에 따라
 *      intr_handler() 또는 intx_flow() 로 이어진다.
 *   3. **마지막에 처리한 비트만 지운다** — 읽은 원본이 아니라 마스크를
 *      씌운 값을 되쓰므로, 마스크된 사건의 상태는 남는다.
 *
 * [관찰] 순서가 "부르고 나서 지우기" 다. MSI 핸들러 둘이 "지우고 나서
 * 부르기" 인 것과 반대다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**(IRQF_NO_THREAD).
 *
 * 호출 체인:
 *   GIC -> IRQ 코어 -> [이 함수] -> pcie_read()
 *     -> generic_handle_domain_irq() -> pcie_write()
 */
static irqreturn_t xilinx_pl_dma_pcie_event_flow(int irq, void *args)
{
	struct pl_dma_pcie *port = args;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */
	unsigned long val;	/* [한국어] 처리 대상 사건 비트들 */
	int i;	/* [한국어] 순회 인덱스 */

	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDR);	/* [한국어] **인터럽트 상태를 읽고** */
	val &= pcie_read(port, XILINX_PCIE_DMA_REG_IMR);	/* [한국어] **마스크와 AND 해 소프트웨어가 열어 둔 사건만 남긴다** */
	for_each_set_bit(i, &val, 32)	/* [한국어] 세워진 비트마다 */
		generic_handle_domain_irq(port->pldma_domain, i);	/* [한국어] **이벤트 도메인의 해당 hwirq 를 부른다** — 그 가상 IRQ 들은 setup_irq 가 미리 만들어 두었다 */

	pcie_write(port, val, XILINX_PCIE_DMA_REG_IDR);	/* [한국어] **처리한 뒤 그 비트들을 지운다.** 읽은 원본이 아니라 마스크를 씌운 값을 되쓰므로 마스크된 사건의 상태는 남는다. MSI 핸들러들이 "지우고 나서 부르기" 인 것과 순서가 반대다 */

	return IRQ_HANDLED;	/* [한국어] 처리했다고 답한다 */
}

/* [한국어] 사건 번호를 (심볼 이름, 사람이 읽을 문장) 쌍으로 잇는 매크로.
 * __stringify(x) 가 이름을 문자열로 바꾸고, 지정 초기화로 그 번호 자리에
 * 넣는다. 그래서 아래 표는 32칸 중 나열된 것만 채워지고 나머지는 두
 * 포인터가 NULL 이다. 줄 잇기 백슬래시로 두 줄에 걸쳐 있어 각 줄에 끝
 * 주석을 붙일 수 없다. */
#define _IC(x, s)                              \
	[XILINX_PCIE_INTR_ ## x] = { __stringify(x), s }

static const struct {	/* [한국어] 사건 설명 표. 익명 구조체 배열이다 */
	const char	*sym;	/* [한국어] 심볼 이름 — devm_request_irq 의 등록 이름으로 쓰여 /proc/interrupts 에 보인다 */
	const char	*str;	/* [한국어] 사람이 읽을 문장 — intr_handler 가 로그로 남긴다. **NULL 이면 setup_irq 가 그 사건의 IRQ 를 만들지 않는다** */
} intr_cause[32] = {	/* [한국어] 32칸이며 비어 있는 칸은 두 포인터가 모두 NULL 이다 */
	_IC(LINK_DOWN,		"Link Down"),	/* [한국어] 링크가 끊어졌다 */
	_IC(HOT_RESET,		"Hot reset"),	/* [한국어] 호스트가 핫리셋을 걸었다 */
	_IC(CFG_TIMEOUT,	"ECAM access timeout"),	/* [한국어] ECAM 접근이 시간 초과됐다 */
	_IC(CORRECTABLE,	"Correctable error message"),	/* [한국어] AER 의 정정 가능 오류 */
	_IC(NONFATAL,		"Non fatal error message"),	/* [한국어] AER 의 치명적이지 않은 오류 */
	_IC(FATAL,		"Fatal error message"),	/* [한국어] AER 의 치명적 오류 */
	_IC(SLV_UNSUPP,		"Slave unsupported request"),	/* [한국어] AXI 슬레이브 쪽이 지원하지 않는 요청을 받았다 */
	_IC(SLV_UNEXP,		"Slave unexpected completion"),	/* [한국어] 예상하지 못한 완료를 받았다 */
	_IC(SLV_COMPL,		"Slave completion timeout"),	/* [한국어] 완료가 시간 초과됐다 */
	_IC(SLV_ERRP,		"Slave Error Poison"),	/* [한국어] 오류 응답을 받았다 */
	_IC(SLV_CMPABT,		"Slave Completer Abort"),	/* [한국어] 완료자 중단을 받았다 */
	_IC(SLV_ILLBUR,		"Slave Illegal Burst"),	/* [한국어] 허용되지 않는 버스트 요청을 받았다 */
	_IC(MST_DECERR,		"Master decode error"),	/* [한국어] AXI 마스터 쪽 디코드 오류 — 주소에 해당하는 대상이 없다 */
	_IC(MST_SLVERR,		"Master slave error"),	/* [한국어] AXI 마스터 쪽이 슬레이브 오류 응답을 받았다. **INTX 와 MSI 는 이 표에 없다** — 로그로 끝낼 사건이 아니라 다시 나눠 보낼 사건이기 때문이다 */
};

/* [한국어]
 * xilinx_pl_dma_pcie_intr_handler - 사건 하나를 사람이 읽을 문장으로 남긴다
 *
 * @irq:    이 사건에 배정된 가상 IRQ 번호.
 * @dev_id: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 항상 IRQ_HANDLED.
 *
 * intr_cause[] 표에 문장이 있는 사건 열넷에 각각 이 함수가 걸린다.
 * **같은 함수가 여러 IRQ 에 걸리므로**, 어느 사건인지는 irq_data 의
 * hwirq 로 되짚는다.
 *
 * AER 세 갈래(CORRECTABLE, NONFATAL, FATAL)에서만 오류 FIFO 를 읽어
 * 요청자 ID 를 남기고, **fallthrough 로 아래 default 로 떨어져** 문장
 * 출력을 공유한다. 즉 AER 사건은 두 가지를 다 한다.
 *
 * 표에 문장이 없는 hwirq 로 불리면 "Unknown IRQ" 로 남긴다 — 다만
 * setup_irq() 가 문장이 있는 칸에만 IRQ 를 만들므로 정상 흐름에서는
 * 그 갈래에 닿지 않는다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**(IRQF_NO_THREAD).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_event_flow() -> IRQ 코어 -> [이 함수]
 *     -> xilinx_pl_dma_pcie_clear_err_interrupts() -> dev_warn()
 */
static irqreturn_t xilinx_pl_dma_pcie_intr_handler(int irq, void *dev_id)
{
	struct pl_dma_pcie *port = (struct pl_dma_pcie *)dev_id;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */
	struct device *dev = port->dev;	/* [한국어] 로그를 낼 장치 */
	struct irq_data *d;	/* [한국어] 어느 사건인지 되짚을 irq_data */

	d = irq_domain_get_irq_data(port->pldma_domain, irq);	/* [한국어] **같은 함수가 여러 IRQ 에 걸리므로** 이벤트 도메인에서 irq_data 를 되찾아 hwirq 로 사건을 가린다 */
	switch (d->hwirq) {	/* [한국어] 사건 번호에 따라 갈린다 */
	case XILINX_PCIE_INTR_CORRECTABLE:	/* [한국어] AER 의 정정 가능 오류 */
	case XILINX_PCIE_INTR_NONFATAL:	/* [한국어] 치명적이지 않은 오류 */
	case XILINX_PCIE_INTR_FATAL:	/* [한국어] 치명적 오류 — 이 셋만 */
		xilinx_pl_dma_pcie_clear_err_interrupts(port);	/* [한국어] **오류 FIFO 를 읽어 요청자 ID 를 남기고 지운다** */
		fallthrough;	/* [한국어] 그리고 아래 문장 출력을 공유하도록 떨어진다 */

	default:	/* [한국어] 그 밖의 사건은 문장만 남긴다 */
		if (intr_cause[d->hwirq].str)	/* [한국어] 표에 문장이 있으면 */
			dev_warn(dev, "%s\n", intr_cause[d->hwirq].str);	/* [한국어] 그 문장을 경고로 남긴다 */
		else	/* [한국어] 없으면 */
			dev_warn(dev, "Unknown IRQ %ld\n", d->hwirq);	/* [한국어] 번호만 남긴다. **setup_irq 가 문장 있는 칸에만 IRQ 를 만들므로 정상 흐름에서는 닿지 않는다** */
	}

	return IRQ_HANDLED;	/* [한국어] 처리했다고 답한다 */
}

/* [한국어] MSI 부모 도메인이 반드시 갖는 성질. 기본 도메인 동작과 기본 칩
 * 동작을 쓰고, 어피니티 설정을 지원하지 않는다고 알린다. 줄 잇기
 * 백슬래시로 여러 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define XILINX_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				   MSI_FLAG_USE_DEF_CHIP_OPS	| \
				   MSI_FLAG_NO_AFFINITY)

/* [한국어] MSI 부모 도메인이 지원할 수 있는 성질. 일반 플래그 전부와 다중
 * MSI 다. **MSI_FLAG_PCI_MSIX 가 없어 이 컨트롤러는 MSI-X 를 알리지
 * 않는다** — 같은 계열의 pcie-xilinx-nwl.c 도 마찬가지다. 줄 잇기
 * 백슬래시로 두 줄에 걸쳐 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define XILINX_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				    MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops xilinx_msi_parent_ops = {	/* [한국어] **MSI 부모 도메인의 성질을 커널 공용 코드에 알린다** */
	.required_flags		= XILINX_MSI_FLAGS_REQUIRED,	/* [한국어] 반드시 필요한 플래그 묶음 */
	.supported_flags	= XILINX_MSI_FLAGS_SUPPORTED,	/* [한국어] 지원 가능한 플래그 묶음 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,	/* [한국어] PCI MSI 버스에 붙는 도메인이라고 알린다 */
	.prefix			= "pl_dma-",	/* [한국어] 도메인 이름에 붙일 접두사 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* [한국어] 자식 도메인 정보를 채우는 공용 헬퍼 */
};
/* [한국어]
 * xilinx_compose_msi_msg - 장치에 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 인터럽트의 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채울 MSI 메시지.
 *
 * 주소는 pcie->phys_reg_base — 레지스터 창의 물리 주소다.
 * xilinx_pl_dma_pcie_enable_msi() 가 같은 값을 MSIBASE1/2 에 심어 두므로,
 * 엔드포인트가 그 주소로 쓰면 컨트롤러가 가로채 MSI 로 바꾼다.
 *
 * 데이터는 hwirq 즉 64개 중 몇 번째 벡터인지다.
 *
 * [관찰] irq_chip 에 마스크·언마스크가 없다. 이 컨트롤러의 MSI 마스크
 * 레지스터(MSI_LOW_MASK/MSI_HI_MASK)는 init_port 가 전부 열어 두고 이후
 * 개별 벡터를 막지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_chip.irq_compose_msi_msg -> [이 함수]
 */
static void xilinx_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct pl_dma_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	phys_addr_t msi_addr = pcie->phys_reg_base;	/* [한국어] **MSI 목적지는 레지스터 창의 물리 주소다.** enable_msi() 가 같은 값을 MSIBASE1/2 에 심어 둔다 */

	msg->address_lo = lower_32_bits(msi_addr);	/* [한국어] 주소 하위 워드 */
	msg->address_hi = upper_32_bits(msi_addr);	/* [한국어] 상위 워드 */
	msg->data = data->hwirq;	/* [한국어] **데이터는 벡터 번호다** */
}

static struct irq_chip xilinx_irq_chip = {	/* [한국어] **MSI 벡터에 매달릴 irq_chip** */
	.name = "pl_dma:MSI",	/* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_compose_msi_msg = xilinx_compose_msi_msg,	/* [한국어] 장치에 알려 줄 주소와 데이터를 만든다. **마스크·언마스크가 없다** — 개별 벡터를 막는 코드가 이 파일에 없다 */
};

/* [한국어]
 * xilinx_irq_domain_alloc - MSI 벡터를 요청 개수만큼 잡아 준다
 *
 * @domain:  MSI 부모 도메인.
 * @virq:    커널이 배정한 가상 IRQ 번호의 시작.
 * @nr_irqs: 요청한 벡터 수.
 * @args:    쓰지 않는다.
 * @return: 0 이면 성공, 빈 자리가 없으면 -ENOSPC.
 *
 * **벡터가 64개(XILINX_NUM_MSI_IRQS)뿐이라 비트맵으로 관리한다.**
 * 그 비트맵은 init_msi_irq_domain() 이 kzalloc 으로 잡은 것이다.
 *
 * bitmap_find_free_region() 이 요청 개수를 2의 거듭제곱으로 올린 크기의
 * 연속 구간을 찾는다 — 다중 MSI 는 연속이고 2의 거듭제곱 개수여야 하며
 * 시작도 그 크기에 정렬되어야 한다는 규격 제약 때문이다.
 *
 * 찾은 구간의 각 벡터에 hwirq 를 잇고 irq_chip 과 handle_simple_irq 를
 * 매단다. MSI 는 에지 성격이라 레벨 처리가 필요 없다.
 *
 * 동기화: 비트맵 조작을 msi->lock 뮤텍스로 감싼다. **실패 경로에서도
 * 잠금을 반드시 푼다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.alloc -> [이 함수]
 *     -> bitmap_find_free_region() -> irq_domain_set_info()
 */
static int xilinx_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	struct pl_dma_pcie *pcie = domain->host_data;	/* [한국어] 도메인의 host_data 에 이 드라이버 인스턴스가 들어 있다 */
	struct xilinx_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	int bit, i;	/* [한국어] bit 는 잡은 구간의 시작 벡터, i 는 순회 인덱스 */

	mutex_lock(&msi->lock);	/* [한국어] **비트맵을 뮤텍스로 감싼다.** 이 경로는 프로세스 컨텍스트에서만 불려 잠들 수 있다 */
	bit = bitmap_find_free_region(msi->bitmap, XILINX_NUM_MSI_IRQS,	/* [한국어] **연속된 빈 구간을 찾는다** */
				      get_count_order(nr_irqs));	/* [한국어] **요청 개수를 2의 거듭제곱으로 올린 크기**로 찾는다 — 다중 MSI 의 규격 제약 때문이다 */
	if (bit < 0) {	/* [한국어] 빈 구간이 없으면 */
		mutex_unlock(&msi->lock);	/* [한국어] **락을 먼저 풀고** */
		return -ENOSPC;	/* [한국어] 공간 없음으로 알린다 */
	}

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 잡은 구간의 벡터마다 */
		irq_domain_set_info(domain, virq + i, bit + i, &xilinx_irq_chip,	/* [한국어] 가상 IRQ 와 hwirq 를 잇고 irq_chip 을 매단다 */
				    domain->host_data, handle_simple_irq,	/* [한국어] chip_data 로 인스턴스를, 핸들러로 handle_simple_irq 를 준다 — MSI 는 에지 성격이다 */
				    NULL, NULL);	/* [한국어] 나머지 두 인자는 쓰지 않는다 */
	}
	mutex_unlock(&msi->lock);	/* [한국어] 락을 푼다 */

	return 0;	/* [한국어] 구간을 잡았으면 성공 */
}

/* [한국어]
 * xilinx_irq_domain_free - 잡아 두었던 MSI 벡터를 놓는다
 *
 * @domain:  MSI 부모 도메인.
 * @virq:    놓을 가상 IRQ 번호의 시작.
 * @nr_irqs: 놓을 벡터 수.
 *
 * alloc 의 짝이다. irq_data 에서 hwirq 를 되찾아 그 자리부터
 * get_count_order(nr_irqs) 크기만큼을 비트맵에서 놓는다 — 잡을 때와 같은
 * 크기 계산을 써야 짝이 맞는다.
 *
 * 인스턴스를 domain->host_data 가 아니라 **irq_data 의 chip_data** 에서
 * 얻는다. alloc 쪽이 두 자리에 같은 값을 넣어 두어 성립한다.
 *
 * 동기화: alloc 과 같은 뮤텍스로 감싼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 제거 또는 MSI 해제 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.free -> [이 함수]
 *     -> irq_domain_get_irq_data() -> bitmap_release_region()
 */
static void xilinx_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);	/* [한국어] **해제 전에 irq_data 에서 hwirq 를 되찾는다** — 그것이 비트맵에서의 위치다 */
	struct pl_dma_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] **alloc 이 chip_data 에도 넣어 둔 인스턴스를 꺼낸다**(host_data 가 아니다) */
	struct xilinx_msi *msi = &pcie->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */

	mutex_lock(&msi->lock);	/* [한국어] 할당 때와 같은 뮤텍스로 감싼다 */
	bitmap_release_region(msi->bitmap, data->hwirq,	/* [한국어] 그 자리부터 */
			      get_count_order(nr_irqs));	/* [한국어] **잡을 때와 같은 크기 계산으로 놓는다** */
	mutex_unlock(&msi->lock);	/* [한국어] 락을 푼다 */
}

static const struct irq_domain_ops dev_msi_domain_ops = {	/* [한국어] **MSI 부모 도메인의 동작.** 할당과 해제만 있다 */
	.alloc	= xilinx_irq_domain_alloc,	/* [한국어] 벡터 잡기 */
	.free	= xilinx_irq_domain_free,	/* [한국어] 벡터 놓기 */
};

/* [한국어]
 * xilinx_pl_dma_pcie_free_irq_domains - 만든 IRQ 도메인을 없앤다
 *
 * @port: 이 드라이버 인스턴스.
 *
 * INTx 도메인과 MSI 도메인을 각각 있으면 없애고 포인터를 NULL 로 둔다.
 * NULL 로 두는 덕에 두 번 불려도 안전하다.
 *
 * [관찰] **이벤트 도메인(pldma_domain)은 없애지 않는다.** 셋 중 둘만
 * 정리한다.
 *
 * 부르는 곳이 둘이다 — init_msi_irq_domain() 의 실패 경로와 probe 의
 * err_host_bridge 경로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_init_msi_irq_domain() 실패 경로 /
 *   xilinx_pl_dma_pcie_probe() 실패 경로 -> [이 함수] -> irq_domain_remove()
 */
static void xilinx_pl_dma_pcie_free_irq_domains(struct pl_dma_pcie *port)
{
	struct xilinx_msi *msi = &port->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */

	if (port->intx_domain) {	/* [한국어] INTx 도메인이 있으면 */
		irq_domain_remove(port->intx_domain);	/* [한국어] 없애고 */
		port->intx_domain = NULL;	/* [한국어] **포인터를 NULL 로 둔다** — 그 덕에 두 번 불려도 안전하다 */
	}

	if (msi->dev_domain) {	/* [한국어] MSI 도메인이 있으면 */
		irq_domain_remove(msi->dev_domain);	/* [한국어] 없애고 */
		msi->dev_domain = NULL;	/* [한국어] 포인터를 NULL 로 둔다. **이벤트 도메인은 여기서 다루지 않는다** */
	}
}

/* [한국어]
 * xilinx_pl_dma_pcie_init_msi_irq_domain - MSI 도메인을 만들고 비트맵과 목적지 주소를 세운다
 *
 * @port: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, 실패면 -ENOMEM.
 *
 * 넷을 순서대로 한다.
 *   1. **MSI 부모 도메인을 만든다.** 크기가 XILINX_NUM_MSI_IRQS(64)다.
 *      msi_create_parent_irq_domain() 이 이 도메인을 부모로 두고 PCI MSI
 *      자식 도메인은 xilinx_msi_parent_ops 의 플래그에 따라 공용 코드가
 *      만든다. 그 플래그에 **MSI_FLAG_PCI_MSIX 가 없어 MSI-X 는 알리지
 *      않는다.**
 *   2. 벡터 비트맵을 지킬 뮤텍스를 초기화한다.
 *   3. **비트맵을 kzalloc 으로 잡는다.** 크기는 64비트를 담을 long 배열
 *      바이트 수다. 같은 계열의 pcie-xilinx-nwl.c 가 구조체 안에 정적으로
 *      두는 것과 다르다.
 *   4. 락을 초기화하고 MSI 목적지 주소를 하드웨어에 심는다.
 *
 * 에러 경로: 1번이나 3번이 실패하면 out 라벨로 가서 도메인들을 없애고
 * 오류를 남긴 뒤 -ENOMEM 을 돌려준다.
 *
 * [관찰] raw_spin_lock_init(&port->lock) 이 여기와 init_irq_domain() 두
 * 곳에 있다. 호출 순서상 여기가 먼저이고 저쪽이 나중에 다시 초기화한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_init_irq_domain() -> [이 함수]
 *     -> msi_create_parent_irq_domain() -> kzalloc()
 *     -> xilinx_pl_dma_pcie_enable_msi()
 */
static int xilinx_pl_dma_pcie_init_msi_irq_domain(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct xilinx_msi *msi = &port->msi;	/* [한국어] MSI 상태를 담아 둔 하위 구조체 */
	int size = BITS_TO_LONGS(XILINX_NUM_MSI_IRQS) * sizeof(long);	/* [한국어] **비트맵에 필요한 바이트 수** — 64비트를 담을 long 배열 크기다 */
	struct irq_domain_info info = {	/* [한국어] 만들 도메인의 명세 */
		.fwnode		= dev_fwnode(port->dev),	/* [한국어] 장치 트리 노드에서 온 fwnode */
		.ops		= &dev_msi_domain_ops,	/* [한국어] 할당·해제 동작 */
		.host_data	= port,	/* [한국어] 콜백에서 꺼내 쓸 인스턴스 */
		.size		= XILINX_NUM_MSI_IRQS,	/* [한국어] **벡터 64개** */
	};

	msi->dev_domain  = msi_create_parent_irq_domain(&info, &xilinx_msi_parent_ops);	/* [한국어] **이 도메인을 부모로 두고 PCI MSI 자식 도메인까지 만들어 달라고 한다** */
	if (!msi->dev_domain)	/* [한국어] 못 만들면 */
		goto out;	/* [한국어] 정리 라벨로 간다 */

	mutex_init(&msi->lock);	/* [한국어] 벡터 비트맵을 지킬 뮤텍스를 초기화한다 */
	msi->bitmap = kzalloc(size, GFP_KERNEL);	/* [한국어] **비트맵을 동적으로 잡는다** — 같은 계열의 pcie-xilinx-nwl.c 는 구조체 안에 정적으로 둔다 */
	if (!msi->bitmap)	/* [한국어] 못 잡으면 */
		goto out;	/* [한국어] 정리 라벨로 간다 */

	raw_spin_lock_init(&port->lock);	/* [한국어] **공유 레지스터 락을 초기화한다.** init_irq_domain() 도 나중에 같은 락을 다시 초기화한다 */
	xilinx_pl_dma_pcie_enable_msi(port);	/* [한국어] MSI 목적지 주소를 하드웨어에 심는다 */

	return 0;	/* [한국어] 모두 세웠으면 성공 */

out:	/* [한국어] **정리 경로** */
	xilinx_pl_dma_pcie_free_irq_domains(port);	/* [한국어] 만들어 둔 도메인들을 없앤다 */
	dev_err(dev, "Failed to allocate MSI IRQ domains\n");	/* [한국어] 그 사실을 알리고 */

	return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */
}

/*
 * INTx error interrupts are Xilinx controller specific interrupt, used to
 * notify user about errors such as cfg timeout, slave unsupported requests,
 * fatal and non fatal error etc.
 */

/* [한국어]
 * xilinx_pl_dma_pcie_intx_flow - INTx 넷을 INTx 도메인으로 다시 나눈다
 *
 * @irq:  이 사건에 배정된 가상 IRQ 번호. 쓰지 않는다.
 * @args: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 항상 IRQ_HANDLED.
 *
 * **이벤트 도메인의 한 칸(XILINX_PCIE_INTR_INTX)에 걸린 핸들러**다.
 * 즉 인터럽트가 두 번 갈라진다 — 주 인터럽트가 event_flow 를 거쳐 이
 * 사건으로 오고, 여기서 다시 INTA~INTD 로 나뉜다.
 *
 * IDRN 레지스터에서 FIELD_GET 으로 INTx 자리(상위 절반)만 떼어 내고,
 * 세워진 비트마다 intx_domain 의 hwirq 를 부른다.
 *
 * 바로 위 상류 주석이 이 계열 인터럽트의 성격을 밝힌다 — "INTx error
 * interrupt" 는 Xilinx 컨트롤러 고유의 것으로 cfg 시간 초과, 슬레이브
 * 미지원 요청, 치명적·비치명적 오류 같은 것을 사용자에게 알리는 데
 * 쓰인다는 내용이다.
 *
 * [관찰] 여기서 상태 비트를 지우지 않는다. INTx 는 레벨 신호라 장치가
 * 원인을 없애야 내려간다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트**(IRQF_NO_THREAD).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_event_flow() -> IRQ 코어 -> [이 함수]
 *     -> pcie_read() -> generic_handle_domain_irq()
 */
static irqreturn_t xilinx_pl_dma_pcie_intx_flow(int irq, void *args)
{
	unsigned long val;	/* [한국어] INTx 상태 비트들 */
	int i;	/* [한국어] 순회 인덱스 */
	struct pl_dma_pcie *port = args;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */

	val = FIELD_GET(XILINX_PCIE_DMA_IDRN_MASK,	/* [한국어] **IDRN 에서 INTx 자리(비트 19~16)만 떼어 낸다** */
			pcie_read(port, XILINX_PCIE_DMA_REG_IDRN));	/* [한국어] 그 레지스터를 읽어 */

	for_each_set_bit(i, &val, PCI_NUM_INTX)	/* [한국어] 세워진 비트마다 */
		generic_handle_domain_irq(port->intx_domain, i);	/* [한국어] INTx 도메인의 해당 hwirq 를 부른다. **여기서 상태 비트를 지우지 않는다** — 레벨 신호라 장치가 원인을 없애야 내려간다 */
	return IRQ_HANDLED;	/* [한국어] 처리했다고 답한다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_mask_event_irq - 컨트롤러 사건 하나를 마스크한다
 *
 * @d: 마스크할 사건의 irq_data. hwirq 가 사건 번호(0~31)다.
 *
 * IMR(Interrupt Mask Register)의 해당 비트를 지운다. **이름과 달리
 * 활성화 레지스터로 쓰인다** — 지우는 것이 막는 것이다.
 *
 * [관찰] raw_spin_lock() 을 쓴다 — **irqsave 판이 아니다.** 같은 락을
 * 쓰는 INTx 마스크 함수들은 irqsave 판을 쓴다. 이 함수가 인터럽트가 이미
 * 꺼진 문맥에서만 불린다는 전제로 보이나, 코드에 그 근거가 적혀 있지는
 * 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_mask -> [이 함수]
 *     -> raw_spin_lock() -> pcie_read()/pcie_write()
 */
static void xilinx_pl_dma_pcie_mask_event_irq(struct irq_data *d)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(d);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	u32 val;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock(&port->lock);	/* [한국어] **irqsave 판이 아닌 raw_spin_lock 이다** — 같은 락을 쓰는 INTx 마스크 함수들은 irqsave 판을 쓴다 */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IMR);	/* [한국어] 현재 마스크를 읽고 */
	val &= ~BIT(d->hwirq);	/* [한국어] 해당 사건 비트를 지운다. **이름과 달리 활성화 레지스터라 지우는 것이 막는 것이다** */
	pcie_write(port, val, XILINX_PCIE_DMA_REG_IMR);	/* [한국어] 고친 값을 되쓴다 */
	raw_spin_unlock(&port->lock);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_unmask_event_irq - 컨트롤러 사건 하나의 마스크를 푼다
 *
 * @d: 마스크를 풀 사건의 irq_data.
 *
 * mask 판의 짝이며 비트를 세우는 것만 다르다. 같은 raw_spin_lock() 을
 * 쓴다(역시 irqsave 판이 아니다).
 *
 * **이 함수가 실질적으로 사건을 켜는 자리다.** init_port() 가 IMR 을 0 으로
 * 써서 모든 사건을 막아 두므로, setup_irq() 가 만든 가상 IRQ 를 IRQ 코어가
 * 활성화하면서 이 함수를 불러야 그 사건이 실제로 event_flow 에 잡힌다 —
 * event_flow 가 IDR 과 IMR 을 AND 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_unmask -> [이 함수]
 *     -> raw_spin_lock() -> pcie_read()/pcie_write()
 */
static void xilinx_pl_dma_pcie_unmask_event_irq(struct irq_data *d)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(d);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	u32 val;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock(&port->lock);	/* [한국어] 같은 락을 같은 방식으로 잡는다 */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IMR);	/* [한국어] 현재 마스크를 읽고 */
	val |= BIT(d->hwirq);	/* [한국어] 해당 사건 비트를 세운다 — **이 함수가 실질적으로 사건을 켜는 자리다** */
	pcie_write(port, val, XILINX_PCIE_DMA_REG_IMR);	/* [한국어] 고친 값을 되쓴다 */
	raw_spin_unlock(&port->lock);	/* [한국어] 락을 푼다 */
}

static struct irq_chip xilinx_pl_dma_pcie_event_irq_chip = {	/* [한국어] **컨트롤러 사건용 irq_chip** */
	.name		= "pl_dma:RC-Event",	/* [한국어] /proc/interrupts 에 보일 이름. "RC-Event" 가 루트 컴플렉스 사건이라는 뜻이다 */
	.irq_mask	= xilinx_pl_dma_pcie_mask_event_irq,	/* [한국어] 마스크와 */
	.irq_unmask	= xilinx_pl_dma_pcie_unmask_event_irq,	/* [한국어] 언마스크만 매단다 */
};

/* [한국어]
 * xilinx_pl_dma_pcie_event_map - 컨트롤러 사건 하나를 가상 IRQ 에 잇는다
 *
 * @domain: 이벤트 irq 도메인.
 * @irq:    커널이 배정한 가상 IRQ 번호.
 * @hwirq:  사건 번호(0~31). XILINX_PCIE_INTR_* 값과 같다.
 * @return: 항상 0.
 *
 * setup_irq() 가 irq_create_mapping() 을 부를 때 커널이 이 콜백을 부른다.
 *
 * irq_chip 과 handle_level_irq 를 매달고, chip_data 에 인스턴스를 담고,
 * IRQ_LEVEL 상태 플래그를 세운다 — INTx 매핑 함수와 구조가 같고 매다는
 * irq_chip 만 다르다.
 *
 * **레벨 핸들러라 처리 전에 마스크가 걸리고 처리 후 풀린다.** 그래서 위
 * mask/unmask 함수가 사건 처리 주기마다 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(가상 IRQ 생성 시).
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_domain_ops.map -> [이 함수]
 *     -> irq_set_chip_and_handler() -> irq_set_status_flags()
 */
static int xilinx_pl_dma_pcie_event_map(struct irq_domain *domain,
					unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_pl_dma_pcie_event_irq_chip,	/* [한국어] irq_chip 과 */
				 handle_level_irq);	/* [한국어] 레벨 핸들러를 매단다 — 처리 전에 마스크가 걸리고 처리 후 풀린다 */
	irq_set_chip_data(irq, domain->host_data);	/* [한국어] 마스크·언마스크에서 꺼내 쓸 수 있게 인스턴스를 담아 둔다 */
	irq_set_status_flags(irq, IRQ_LEVEL);	/* [한국어] 레벨 트리거임을 알린다 */

	return 0;	/* [한국어] 매핑에 실패할 일이 없어 늘 성공이다 */
}

static const struct irq_domain_ops event_domain_ops = {	/* [한국어] **이벤트 도메인의 동작. map 만 있고 xlate 가 없다** */
	.map = xilinx_pl_dma_pcie_event_map,	/* [한국어] 가상 IRQ 를 만들 때 부를 함수 */
};

/**
 * xilinx_pl_dma_pcie_init_irq_domain - Initialize IRQ domain
 * @port: PCIe port information
 *
 * Return: '0' on success and error value on failure.
 */
/* [한국어]
 * xilinx_pl_dma_pcie_init_irq_domain - 도메인 셋(이벤트·INTx·MSI)을 만든다
 *
 * @port: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 장치 트리에 인터럽트 컨트롤러 노드가 없으면
 *          -EINVAL, 도메인을 못 만들면 -ENOMEM.
 *
 * 바로 위 상류 kernel-doc 이 "IRQ 도메인을 초기화한다" 고 적는다.
 *
 * **장치 트리에서 "interrupt-controller" 라는 이름의 자식 노드를 찾는다.**
 * 같은 계열의 pcie-xilinx-nwl.c 가 of_get_next_child() 로 첫 자식을
 * 가져오는 것과 달리, 여기는 이름으로 콕 집는다.
 *
 * 도메인을 셋 만든다.
 *   1. **이벤트 도메인(pldma_domain)** — 크기 32. 컨트롤러 자신의 사건
 *      번호가 hwirq 다. 만든 뒤 버스 토큰을 DOMAIN_BUS_NEXUS 로 바꾼다 —
 *      **가운데서 갈라 보내는 층**이라는 표시다.
 *   2. **INTx 도메인(intx_domain)** — 크기 PCI_NUM_INTX(4). 버스 토큰이
 *      DOMAIN_BUS_WIRED 다.
 *   3. MSI 도메인 — 아래 함수에 맡긴다.
 *
 * 에러 경로가 고르지 않다.
 *   - 이벤트 도메인 실패: **찾은 노드 참조를 놓지 않고** -ENOMEM 으로 나간다.
 *   - INTx 도메인 실패: 역시 노드 참조를 놓지 않고, 앞서 만든 이벤트
 *     도메인도 없애지 않는다.
 *   - MSI 실패: INTx 도메인만 없애고 나간다(이벤트 도메인과 노드 참조는
 *     그대로).
 * 성공 경로에서만 of_node_put() 이 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_probe() -> [이 함수] -> of_get_child_by_name()
 *     -> irq_domain_create_linear() x2 -> irq_domain_update_bus_token()
 *     -> xilinx_pl_dma_pcie_init_msi_irq_domain()
 */
static int xilinx_pl_dma_pcie_init_irq_domain(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct device_node *node = dev->of_node;	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *pcie_intc_node;	/* [한국어] 그 아래의 인터럽트 컨트롤러 노드 */
	int ret;	/* [한국어] 하위 호출 결과 */

	/* Setup INTx */
	pcie_intc_node = of_get_child_by_name(node, "interrupt-controller");	/* [한국어] **"interrupt-controller" 라는 이름으로 콕 집어 찾는다** — 같은 계열의 pcie-xilinx-nwl.c 는 첫 자식을 가져온다 */
	if (!pcie_intc_node) {	/* [한국어] 없으면 */
		dev_err(dev, "No PCIe Intc node found\n");	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다. 아직 참조를 잡기 전이라 놓을 것이 없다 */
	}

	port->pldma_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), 32,	/* [한국어] **이벤트 도메인을 만든다. 크기가 32** 이고 hwirq 가 사건 번호다 */
						      &event_domain_ops, port);	/* [한국어] 동작 묶음과 인스턴스를 준다 */
	if (!port->pldma_domain)	/* [한국어] 못 만들면 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다. **[관찰] 찾은 노드 참조를 놓지 않는다** */

	irq_domain_update_bus_token(port->pldma_domain, DOMAIN_BUS_NEXUS);	/* [한국어] **버스 토큰을 NEXUS 로 바꾼다** — 가운데서 갈라 보내는 층이라는 표시다 */

	port->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,	/* [한국어] **INTx 도메인을 만든다.** 같은 노드에 붙이고 크기는 PCI_NUM_INTX(4)다 */
						     &intx_domain_ops, port);	/* [한국어] 동작 묶음과 인스턴스를 준다 */
	if (!port->intx_domain) {	/* [한국어] 못 만들면 */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");	/* [한국어] 그 사실을 알리고 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다. **[관찰] 노드 참조도 이벤트 도메인도 되돌리지 않는다** */
	}

	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED);	/* [한국어] **버스 토큰을 WIRED 로 바꾼다** — 배선된 인터럽트라는 표시다 */

	ret = xilinx_pl_dma_pcie_init_msi_irq_domain(port);	/* [한국어] MSI 도메인도 만든다 */
	if (ret != 0) {	/* [한국어] 실패하면 */
		irq_domain_remove(port->intx_domain);	/* [한국어] **INTx 도메인만 없애고** */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다. 이벤트 도메인과 노드 참조는 그대로 남는다 */
	}

	of_node_put(pcie_intc_node);	/* [한국어] **성공 경로에서만 노드 참조를 놓는다** */
	raw_spin_lock_init(&port->lock);	/* [한국어] 공유 레지스터 락을 초기화한다. init_msi_irq_domain() 이 이미 한 번 했다 */

	return 0;	/* [한국어] 도메인 셋을 모두 만들었으면 성공 */
}

/* [한국어]
 * xilinx_pl_dma_pcie_setup_irq - 사건별 가상 IRQ 를 만들고 핸들러를 건다
 *
 * @port: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * 이벤트 도메인 위에 실제 IRQ 들을 얹는 자리다. 셋을 한다.
 *
 *   1. **intr_cause[] 표에 문장이 있는 칸마다** 가상 IRQ 를 만들고
 *      xilinx_pl_dma_pcie_intr_handler() 를 건다. **표가 곧 "다룰 사건
 *      목록" 이며**, 문장이 없는 칸은 건너뛴다. 등록 이름으로 표의 심볼
 *      문자열을 쓰므로 /proc/interrupts 에 사건 이름이 그대로 보인다.
 *   2. **INTx 사건에 별도의 IRQ 를 만들고** xilinx_pl_dma_pcie_intx_flow()
 *      를 건다. 그 사건은 표에 없으므로 1번 루프에서 만들어지지 않는다.
 *   3. **주 인터럽트에 xilinx_pl_dma_pcie_event_flow() 를 건다.**
 *      이것이 위 IRQ 들을 부르는 상위 흐름이다.
 *
 * [관찰] 등록 순서가 거꾸로다 — 사건별 IRQ 를 먼저 만들고 나서 주
 * 인터럽트를 건다. 그 덕에 주 인터럽트가 처음 들어올 때는 이미 모든
 * 사건별 IRQ 가 준비되어 있다.
 *
 * [관찰] 2번과 3번의 devm_request_irq() 는 이름 인자로 NULL 을 넘긴다.
 *
 * [관찰] 함수 안에 `int err` 가 둘 있다 — 바깥의 것과 1번 루프 안의
 * 것이다. 루프 안에서는 안쪽 것이 가려 쓴다.
 *
 * 모든 IRQ 를 devm_ 판으로 등록하므로 장치가 사라질 때 커널이 자동으로
 * 되돌린다. **IRQF_SHARED | IRQF_NO_THREAD** 로, 선을 공유할 수 있고
 * 강제 스레드화 아래에서도 하드 인터럽트 컨텍스트에서 돈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_probe() -> [이 함수] -> platform_get_irq()
 *     -> irq_create_mapping() -> devm_request_irq()
 */
static int xilinx_pl_dma_pcie_setup_irq(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct platform_device *pdev = to_platform_device(dev);	/* [한국어] 인터럽트 번호를 얻기 위한 플랫폼 장치 */
	int i, irq, err;	/* [한국어] i 는 표 순회 인덱스, irq 는 만든 가상 IRQ, err 는 결과 */

	port->irq = platform_get_irq(pdev, 0);	/* [한국어] **주 인터럽트를 번호 0 으로 얻는다** — 이름이 아니다 */
	if (port->irq < 0)	/* [한국어] 없으면 */
		return port->irq;	/* [한국어] 그 오류를 그대로 돌려준다 */

	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) {	/* [한국어] **사건 설명 표 32칸을 훑는다** */
		int err;	/* [한국어] 바깥의 err 를 가리는 안쪽 변수다 */

		if (!intr_cause[i].str)	/* [한국어] **문장이 없는 칸은 다루지 않는 사건이다** */
			continue;	/* [한국어] 건너뛴다 — 즉 이 표가 곧 "다룰 사건 목록" 이다 */

		irq = irq_create_mapping(port->pldma_domain, i);	/* [한국어] 이벤트 도메인 위에 그 사건의 가상 IRQ 를 만든다 */
		if (!irq) {	/* [한국어] 못 만들면 */
			dev_err(dev, "Failed to map interrupt\n");	/* [한국어] 그 사실을 알리고 */
			return -ENXIO;	/* [한국어] 입출력 오류로 돌아간다 */
		}

		err = devm_request_irq(dev, irq,	/* [한국어] 그 IRQ 에 사건 로그 핸들러를 건다 */
				       xilinx_pl_dma_pcie_intr_handler,	/* [한국어] 모든 사건이 같은 함수를 쓴다 — 어느 사건인지는 hwirq 로 되짚는다 */
				       IRQF_SHARED | IRQF_NO_THREAD,	/* [한국어] 선을 공유할 수 있고 강제 스레드화 아래에서도 하드 인터럽트 컨텍스트에서 돈다 */
				       intr_cause[i].sym, port);	/* [한국어] **등록 이름으로 표의 심볼 문자열을 쓴다** — /proc/interrupts 에 사건 이름이 그대로 보인다 */
		if (err) {	/* [한국어] 등록에 실패하면 */
			dev_err(dev, "Failed to request IRQ %d\n", irq);	/* [한국어] 그 사실을 알리고 */
			return err;	/* [한국어] 그 코드를 돌려준다 */
		}
	}

	port->intx_irq = irq_create_mapping(port->pldma_domain,	/* [한국어] **INTx 사건에 별도의 IRQ 를 만든다** */
					    XILINX_PCIE_INTR_INTX);	/* [한국어] 그 사건은 표에 없으므로 위 루프에서 만들어지지 않는다 */
	if (!port->intx_irq) {	/* [한국어] 못 만들면 */
		dev_err(dev, "Failed to map INTx interrupt\n");	/* [한국어] 그 사실을 알리고 */
		return -ENXIO;	/* [한국어] 입출력 오류로 돌아간다 */
	}

	err = devm_request_irq(dev, port->intx_irq, xilinx_pl_dma_pcie_intx_flow,	/* [한국어] **그 IRQ 에 INTx 분배 핸들러를 건다** */
			       IRQF_SHARED | IRQF_NO_THREAD, NULL, port);	/* [한국어] 등록 이름으로 NULL 을 넘긴다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to request INTx IRQ %d\n", port->intx_irq);	/* [한국어] 그 사실을 알리고 */
		return err;	/* [한국어] 그 코드를 돌려준다 */
	}

	err = devm_request_irq(dev, port->irq, xilinx_pl_dma_pcie_event_flow,	/* [한국어] **마지막으로 주 인터럽트에 상위 분배 핸들러를 건다** — 이것이 위 IRQ 들을 부르는 흐름이다 */
			       IRQF_SHARED | IRQF_NO_THREAD, NULL, port);	/* [한국어] 역시 이름 없이 등록한다 */
	if (err) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to request event IRQ %d\n", port->irq);	/* [한국어] 그 사실을 알리고 */
		return err;	/* [한국어] 그 코드를 돌려준다 */
	}

	return 0;
}

/* [한국어]
 * xilinx_pl_dma_pcie_init_port - 컨트롤러 레지스터를 초기 상태로 만든다
 *
 * @port: 이 드라이버 인스턴스.
 *
 * 반환값이 없어 실패를 알리지 않는다 — 레지스터 쓰기뿐이라 실패할 자리가
 * 없다. 넷을 한다.
 *
 *   1. 링크 상태를 로그로 남긴다.
 *   2. **모든 인터럽트를 끈다.** ~XILINX_PCIE_DMA_IDR_ALL_MASK 를 IMR 에
 *      쓰는데, 그 값이 ~0xffffffff 이므로 **결국 0 을 쓴다.** IMR 은
 *      활성화 레지스터라 0 이 전부 막는 것이다.
 *   3. **남아 있던 상태를 지운다.** IDR 을 읽어 IMR_ALL_MASK(0x0ff30fe9)로
 *      거른 뒤 되쓴다 — 1 을 써서 지우는 방식이다.
 *   4. **MSI 마스크 둘을 전부 연다.** 옆의 상류 주석이 "MSI DECODE MODE 에
 *      필요하다" 고 적는다. 이 뒤로 개별 MSI 벡터를 막는 코드는 없다.
 *   5. **브리지 활성화 비트를 세운다**(RPSC 의 BEN). 이 줄이 있어야
 *      컨트롤러가 실제로 트랜잭션을 넘긴다.
 *
 * [관찰] 2번에서 IMR 을 0 으로 막아 두지만, 사건별 IRQ 가 활성화되면서
 * xilinx_pl_dma_pcie_unmask_event_irq() 가 필요한 비트를 다시 연다.
 * 즉 **어떤 사건을 열지는 intr_cause[] 표가 결정한다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). **도메인을 만들기 전에**
 * 불리므로 이 시점에는 아직 인터럽트가 등록되어 있지 않다.
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_probe() -> [이 함수] -> pcie_read()/pcie_write()
 */
static void xilinx_pl_dma_pcie_init_port(struct pl_dma_pcie *port)
{
	if (xilinx_pl_dma_pcie_link_up(port))
		dev_info(port->dev, "PCIe Link is UP\n");
	/* [한국어] 링크가 서 있지 않으면 — */
	else
		dev_info(port->dev, "PCIe Link is DOWN\n");

	/* Disable all interrupts */
	pcie_write(port, ~XILINX_PCIE_DMA_IDR_ALL_MASK,
		   XILINX_PCIE_DMA_REG_IMR);

	/* Clear pending interrupts */
	pcie_write(port, pcie_read(port, XILINX_PCIE_DMA_REG_IDR) &
		   XILINX_PCIE_DMA_IMR_ALL_MASK,
		   XILINX_PCIE_DMA_REG_IDR);

	/* Needed for MSI DECODE MODE */
	pcie_write(port, XILINX_PCIE_DMA_IDR_ALL_MASK,
		   XILINX_PCIE_DMA_REG_MSI_LOW_MASK);
	pcie_write(port, XILINX_PCIE_DMA_IDR_ALL_MASK,
		   /* [한국어] MSI 디코드 모드를 위해 하위·상위 마스크를 모두 연다(옆의 상류 주석).
		    * 이 컨트롤러가 MSI 를 32개씩 두 묶음으로 나눠 다루기 때문에 두 줄이 필요하다. */
		   XILINX_PCIE_DMA_REG_MSI_HI_MASK);

	/* Set the Bridge enable bit */
	pcie_write(port, pcie_read(port, XILINX_PCIE_DMA_REG_RPSC) |
		   XILINX_PCIE_DMA_REG_RPSC_BEN,
		   XILINX_PCIE_DMA_REG_RPSC);
}

/* [한국어]
 * xilinx_request_msi_irq - MSI 인터럽트 둘을 이름으로 얻어 핸들러를 건다
 *
 * @port: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 인터럽트를 못 얻으면 그 값, 등록이 실패하면 그 코드.
 *
 * 장치 트리의 "msi0" 과 "msi1" 을 각각 얻어
 * xilinx_pl_dma_pcie_msi_handler_low()/high() 를 건다. **벡터가 64개인데
 * 상태 레지스터가 32비트짜리 둘이라 인터럽트도 둘**이다.
 *
 * **연쇄 핸들러가 아니라 보통의 devm_request_irq** 이며, 같은 이름
 * ("xlnx-pcie-dma-pl")으로 둘 다 등록한다.
 *
 * [관찰] 실패 판정이 `<= 0` 이다. platform_get_irq_byname() 이 0 을
 * 돌려주는 경우까지 실패로 보되, **그때는 0(성공)을 반환한다.**
 *
 * 부르는 곳이 parse_dt() 라는 점이 특이하다 — 장치 트리를 읽는 함수가
 * 인터럽트 등록까지 함께 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_parse_dt() -> [이 함수]
 *     -> platform_get_irq_byname() -> devm_request_irq()
 */
static int xilinx_request_msi_irq(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 인터럽트 등록의 결과. */
	int ret;

	port->msi.irq_msi0 = platform_get_irq_byname(pdev, "msi0");
	/* [한국어] 하위 MSI 인터럽트를 얻지 못하면 — */
	if (port->msi.irq_msi0 <= 0)
		/* [한국어] 그 값을 그대로 돌려준다. 0 이면 0 이 나가 성공으로 읽히는데,
		 * platform_get_irq_byname() 이 0 을 돌려주지 않는다는 전제 위에 있다. */
		return port->msi.irq_msi0;

	ret = devm_request_irq(dev, port->msi.irq_msi0, xilinx_pl_dma_pcie_msi_handler_low,
			       /* [한국어] 공유 인터럽트로 걸고 스레드화를 막는다. IRQF_NO_THREAD 는 강제 스레드
			        * 인터럽트 설정(PREEMPT_RT 등)에서도 이 핸들러를 하드 IRQ 문맥에 남기라는 뜻이다. */
			       IRQF_SHARED | IRQF_NO_THREAD, "xlnx-pcie-dma-pl",
			       port);
	if (ret) {
		/* [한국어] 등록이 실패하면 그 사실을 남기고, */
		dev_err(dev, "Failed to register interrupt\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;
	}

	port->msi.irq_msi1 = platform_get_irq_byname(pdev, "msi1");
	/* [한국어] 상위 MSI 인터럽트를 얻지 못하면 — */
	if (port->msi.irq_msi1 <= 0)
		/* [한국어] 그 값을 그대로 돌려준다. 이 경우 위에서 이미 등록한 하위 핸들러는
		 * devm 판이라 드라이버가 떨어질 때 자동으로 풀린다. */
		return port->msi.irq_msi1;

	ret = devm_request_irq(dev, port->msi.irq_msi1, xilinx_pl_dma_pcie_msi_handler_high,
			       /* [한국어] 상위 묶음도 같은 조건으로 건다. 핸들러만 다르다. */
			       IRQF_SHARED | IRQF_NO_THREAD, "xlnx-pcie-dma-pl",
			       port);
	if (ret) {
		/* [한국어] 등록이 실패하면 그 사실을 남기고, */
		dev_err(dev, "Failed to register interrupt\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;
	}

	return 0;
}

/* [한국어]
 * xilinx_pl_dma_pcie_parse_dt - ECAM 창을 만들고 변종에 따라 레지스터 창을 정한다
 *
 * @port:      이 드라이버 인스턴스.
 * @bus_range: 이 호스트 브리지가 담당할 버스 번호 범위.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * **config 창 관리를 코어에 통째로 맡긴다.** pci_ecam_create() 가 첫
 * MEM 자원과 버스 범위를 받아 struct pci_config_window 를 만들고 매핑까지
 * 한다. 같은 계열의 pcie-xilinx-nwl.c 가 창 셋을 손으로 매핑하는 것과
 * 다르다.
 *
 * **변종에 따라 레지스터 창이 갈린다.**
 *   - XDMA: reg_base = cfg->win. 즉 **레지스터 창과 ECAM 창이 같은
 *     매핑**이고, phys_reg_base 는 그 자원의 시작 주소다.
 *   - QDMA: cfg_base = cfg->win 으로 두고, 장치 트리의 "breg" 자원을 따로
 *     매핑해 reg_base 로 쓴다. phys_reg_base 도 그쪽으로 덮어쓴다.
 *     그래서 pcie_read/write 가 더하는 QDMA_BRIDGE_BASE_OFF 는 이 breg
 *     창 기준의 오프셋이다.
 *
 * 마지막에 MSI 인터럽트 둘을 등록하고, 실패하면 앞서 만든 ECAM 창을
 * pci_ecam_free() 로 되돌린다.
 *
 * [관찰] QDMA 경로에서 platform_get_resource_byname() 이 NULL 을 돌려줄
 * 수 있는데 검사 없이 devm_ioremap_resource() 에 넘긴다. 그 함수가 NULL 을
 * 오류로 다루므로 결과적으로는 걸러진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   xilinx_pl_dma_pcie_probe() -> [이 함수] -> platform_get_resource()
 *     -> pci_ecam_create() -> devm_ioremap_resource()
 *     -> xilinx_request_msi_irq()
 */
static int xilinx_pl_dma_pcie_parse_dt(struct pl_dma_pcie *port,
				       struct resource *bus_range)
{
	struct device *dev = port->dev;
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 레지스터 창을 담을 자원. */
	struct resource *res;
	/* [한국어] 각 단계의 결과. */
	int err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	/* [한국어] 첫 메모리 자원이 없으면 — */
	if (!res) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Missing \"reg\" property\n");
		/* [한국어] 장치 없음으로 답한다. */
		return -ENXIO;
	}
	port->phys_reg_base = res->start;

	port->cfg = pci_ecam_create(dev, res, bus_range, &xilinx_pl_dma_pcie_ops);
	/* [한국어] ECAM 매핑 생성이 실패하면, */
	if (IS_ERR(port->cfg))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(port->cfg);

	port->reg_base = port->cfg->win;
/* [한국어] XDMA 판은 레지스터 창이 ECAM 창과 같다 — 하나의 매핑을 두 용도로 쓴다. */

	if (port->variant->version == QDMA) {
		/* [한국어] QDMA 판은 그 창을 config 전용으로 삼고, */
		port->cfg_base = port->cfg->win;
		/* [한국어] 레지스터를 "breg" 라는 별도 창에서 얻는다. 두 판의 차이가 여기 하나로 드러난다. */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "breg");
		/* [한국어] 그 창을 따로 매핑한다. */
		port->reg_base = devm_ioremap_resource(dev, res);
		/* [한국어] 매핑이 실패하면, */
		if (IS_ERR(port->reg_base))
			/* [한국어] 그 오류를 올려보낸다. */
			return PTR_ERR(port->reg_base);
		/* [한국어] 물리 주소도 그 창의 것으로 덮는다 — 위에서 담아 둔 ECAM 창의 값은 QDMA 에서 맞지 않다. */
		port->phys_reg_base = res->start;
	/* [한국어] QDMA 전용 처리 끝. */
	}

	err = xilinx_request_msi_irq(port);
	/* [한국어] MSI 인터럽트 등록이 실패하면, */
	if (err) {
		/* [한국어] 방금 만든 ECAM 매핑을 되돌린다. 이것만 devm 판이 아니라 수동 해제가 필요하다. */
		pci_ecam_free(port->cfg);
		return err;
	}

	return 0;
}

/* [한국어]
 * xilinx_pl_dma_pcie_probe - 진입점. ECAM·레지스터·도메인 셋을 세우고 버스를 연다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * 순서가 이렇다.
 *
 *   1. **호스트 브리지 뒤에 이 드라이버 상태를 붙여 한 번에 잡는다.**
 *   2. bridge->windows 에서 **버스 번호 범위**를 찾는다. ECAM 창을 만들
 *      때 필요한 값이라 parse_dt 에 넘긴다.
 *   3. of_device_get_match_data() 로 **변종(XDMA/QDMA)을 정한다.**
 *      이 한 줄이 이후 모든 레지스터 접근의 오프셋을 좌우한다.
 *   4. parse_dt — ECAM 창, 레지스터 창, MSI 인터럽트 둘.
 *   5. init_port — 컨트롤러 레지스터를 초기 상태로.
 *   6. init_irq_domain — 이벤트·INTx·MSI 도메인 셋.
 *   7. setup_irq — 사건별 IRQ 와 주 인터럽트.
 *   8. bridge 에 sysdata 와 ops 를 매달고 pci_host_probe().
 *
 * [관찰] **7번의 반환값을 받아만 두고 확인하지 않는다.** err 에 담기지만
 * 곧바로 다음 줄로 넘어가므로, 인터럽트 등록이 실패해도 버스를 연다.
 *
 * 에러 경로: err_host_bridge 는 도메인들을 없애고 err_irq_domain 으로
 * 떨어져 ECAM 창을 되돌린다. **다만 free_irq_domains() 가 이벤트 도메인은
 * 없애지 않으므로 그 하나는 남는다.**
 *
 * **remove 콜백이 없다.** platform_driver 에 probe 만 있고
 * suppress_bind_attrs 로 수동 언바인드도 막혀 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> devm_pci_alloc_host_bridge()
 *     -> of_device_get_match_data() -> xilinx_pl_dma_pcie_parse_dt()
 *     -> xilinx_pl_dma_pcie_init_port()
 *     -> xilinx_pl_dma_pcie_init_irq_domain()
 *     -> xilinx_pl_dma_pcie_setup_irq() -> pci_host_probe()
 */
static int xilinx_pl_dma_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pl_dma_pcie *port;
	/* [한국어] 이 컨트롤러가 등록할 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 디바이스 트리가 준 버스 번호 범위. */
	struct resource_entry *bus;
	/* [한국어] 각 단계의 결과. */
	int err;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*port));
	/* [한국어] 브리지를 잡지 못하면 — */
	if (!bridge)
		/* [한국어] 장치 없음으로 답한다. 사설 영역을 함께 할당하는 판이라,
		 * 이 한 번의 할당이 브리지와 드라이버 상태를 모두 만든다. */
		return -ENODEV;

	port = pci_host_bridge_priv(bridge);
/* [한국어] 그 사설 영역이 이 드라이버의 상태다. */

	port->dev = dev;
/* [한국어] 로그와 디바이스 트리 조회에 쓸 device. */

	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] 버스 번호 범위가 없으면 — */
	if (!bus)
		/* [한국어] 장치 없음으로 답한다. ECAM 매핑의 크기가 그 범위로 정해지므로 필수다. */
		return -ENODEV;

	port->variant = of_device_get_match_data(dev);
/* [한국어] 매칭된 항목의 데이터가 XDMA 인지 QDMA 인지 알려 준다.
 * 이 값이 위 parse_dt 의 갈림을 정한다. */

	err = xilinx_pl_dma_pcie_parse_dt(port, bus->res);
	/* [한국어] 디바이스 트리 파싱이 실패하면, */
	if (err) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Parsing DT failed\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return err;
	}

	xilinx_pl_dma_pcie_init_port(port);

	err = xilinx_pl_dma_pcie_init_irq_domain(port);
	/* [한국어] 인터럽트 도메인 생성이 실패하면, */
	if (err)
		/* [한국어] ECAM 매핑을 되돌리는 자리로 뛴다. */
		goto err_irq_domain;

	err = xilinx_pl_dma_pcie_setup_irq(port);
/* [한국어] [상류 코드 관찰] 이 반환값을 **확인하지 않는다.** 다음 줄부터 err 이
 * 덮이므로, 인터럽트 설정이 실패해도 그대로 진행해 버스를 스캔한다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */

	bridge->sysdata = port;
	/* [한국어] 이 파일의 config 접근 함수들을 코어에 알린다. */
	bridge->ops = &xilinx_pl_dma_pcie_ops.pci_ops;

	err = pci_host_probe(bridge);
	/* [한국어] 버스 스캔이 실패하면, */
	if (err < 0)
		/* [한국어] 인터럽트 도메인과 ECAM 매핑을 모두 되돌리는 자리로 뛴다. */
		goto err_host_bridge;

	return 0;

err_host_bridge:
	xilinx_pl_dma_pcie_free_irq_domains(port);

err_irq_domain:
	pci_ecam_free(port->cfg);
	return err;
}

static const struct xilinx_pl_dma_variant xdma_host = {
	/* [한국어] XDMA 판 — 레지스터 창과 ECAM 창이 같다. */
	.version = XDMA,
/* [한국어] 이 구조체에 필드가 version 하나뿐이라, 두 판의 차이가 코드의 조건문으로만 나타난다. */
};

static const struct xilinx_pl_dma_variant qdma_host = {
	/* [한국어] QDMA 판 — 레지스터를 "breg" 창에서 따로 얻는다. */
	.version = QDMA,
/* [한국어] 두 판의 서술이 여기서 끝난다. */
};

static const struct of_device_id xilinx_pl_dma_pcie_of_match[] = {
	/* [한국어] 첫 항목. */
	{
		.compatible = "xlnx,xdma-host-3.00",
		/* [한국어] XDMA 판을 가리킨다. */
		.data = &xdma_host,
	},
	{
		.compatible = "xlnx,qdma-host-3.00",
		/* [한국어] QDMA 판을 가리킨다. */
		.data = &qdma_host,
	},
	{}
};

static struct platform_driver xilinx_pl_dma_pcie_driver = {
	/* [한국어] 드라이버 정보. */
	.driver = {
		/* [한국어] sysfs 에 나올 이름. QDMA 도 이 드라이버가 다루지만 이름은 xdma 쪽이다. */
		.name = "xilinx-xdma-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = xilinx_pl_dma_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = xilinx_pl_dma_pcie_probe,
};

builtin_platform_driver(xilinx_pl_dma_pcie_driver);
