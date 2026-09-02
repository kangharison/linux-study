// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for Xilinx Versal CPM DMA Bridge
 *
 * (C) Copyright 2019 - 2020, Xilinx, Inc.
 */

/*
 * [한국어 설명] Xilinx Versal CPM(Coherent PCIe Module) DMA 브리지의 PCIe
 * 루트 포트 드라이버 — DesignWare 코어를 쓰지 않는 자체 구현 (pcie-xilinx-cpm.c)
 *
 * === 파일의 역할 ===
 * Xilinx Versal SoC 에 내장된 CPM 블록을 PCIe 루트 컴플렉스로 동작시키는
 * 드라이버다. 같은 디렉터리의 DWC 계열 글루들과 성격이 근본적으로 다르다 —
 * Synopsys DesignWare 코어를 쓰지 않으므로 기댈 공용 층이 없고, 설정공간
 * 접근·인터럽트 도메인·오류 보고가 모두 이 파일 안에 있거나 커널의 공용
 * ECAM 구현에 직접 연결된다.
 *
 * 다만 이 파일이 직접 짜는 것과 커널 공용 코드에 맡기는 것의 경계가 뚜렷하다.
 * 설정공간 접근은 하나도 직접 구현하지 않고 drivers/pci/ecam.c 의
 * pci_generic_ecam_ops 를 그대로 가져다 쓴다. CPM 의 설정공간이 표준 ECAM
 * 배치를 그대로 따르기 때문이다. 그래서 이 파일에 남은 일은 셋이다.
 *   1. 두 개의 인터럽트 도메인을 만들고 유지하는 일 — 32비트짜리 "이벤트"
 *      도메인(링크 다운, AER, AXI 오류 등 사건 전부)과 그 안의 한 비트에서
 *      갈라져 나오는 INTx 도메인 넷.
 *   2. 사건이 올라왔을 때 사람이 읽을 수 있는 문자열로 로그를 남기는 일
 *      (intr_cause[] 표).
 *   3. 브리지를 켜고(RPSC 의 BEN 비트) 판본별 오류 인터럽트 인에이블을
 *      맞추는 초기화.
 *
 * 레지스터 창을 둘 다룬다. reg_base 는 브리지 레지스터이고, cpm_base 는
 * CPM SLCR(System Level Control and Status Register) 블록이다. 후자가 따로
 * 필요한 이유는 오류 인터럽트의 상태·인에이블 레지스터가 PCIe 블록이 아니라
 * SoC 수준 제어 블록에 놓여 있기 때문이며, 상류 주석 둘이 그 사실을 짚어 둔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → xilinx_cpm_pcie_probe()
 *     → devm_pci_alloc_host_bridge()      : 브리지와 이 드라이버의 상태를 함께 잡는다
 *     → of_device_get_match_data()        : 판본(CPM/CPM5/CPM5_HOST1/CPM5NC_HOST)
 *     → xilinx_cpm_pcie_init_irq_domain() : 이벤트 도메인(32)과 INTx 도메인(4)
 *     → xilinx_cpm_pcie_parse_dt()        : "cpm_slcr"/"cfg"/"cpm_csr" 자원
 *                                           + pci_ecam_create()
 *     → xilinx_cpm_pcie_init_port()       : 링크 상태 보고, 인터럽트 정리, 브리지 켜기
 *     → xilinx_cpm_setup_irq()            : 사건마다 핸들러를 걸고 연쇄 핸들러 둘을 꽂는다
 *     → pci_host_probe()                  : 버스 스캔. 설정공간 접근은 공용 ECAM 이 한다
 *
 * 인터럽트가 올라오는 길은 두 갈래다.
 *
 *   (사건 전반) 상위 GIC → xilinx_cpm_pcie_event_flow()  [연쇄 핸들러]
 *       → IDR 과 IMR 을 AND 해 걸린 비트를 가리고
 *       → generic_handle_domain_irq(cpm_domain, 비트)
 *       → xilinx_cpm_pcie_intr_handler()  [사건마다 devm_request_irq 로 걸린 핸들러]
 *       → 문자열 로그. AER 3종은 오류 FIFO 도 비운다.
 *
 *   (INTx)   이벤트 도메인의 INTX 비트 → xilinx_cpm_pcie_intx_flow()  [연쇄 핸들러]
 *       → IDRN 의 4비트 필드를 읽어
 *       → generic_handle_domain_irq(intx_domain, 0~3)
 *       → 하위 장치 드라이버의 핸들러
 *
 * 즉 INTx 는 이벤트 도메인의 한 비트에 매달린 2단 구조다. 그래서 INTx 만은
 * intr_cause[] 표에 문자열이 없고(따라서 devm_request_irq 대상에서 빠지고),
 * 대신 연쇄 핸들러가 따로 꽂힌다.
 *
 * 실행 컨텍스트: probe 경로는 프로세스 컨텍스트다. 두 flow 함수와
 * intr_handler 는 인터럽트 컨텍스트에서 돌고, 그 안에서 irq_chip 콜백
 * (mask/unmask)이 port->lock 으로 레지스터 읽기-수정-쓰기를 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. pci_host_probe() 가 버스를 스캔하며, 설정공간 접근은
 *   bridge->ops 로 걸어 둔 pci_generic_ecam_ops.pci_ops 가 처리한다.
 * 아래쪽: 없다 — 이 드라이버 아래에 다른 컨트롤러 층이 없다. DWC 계열이
 *   pcie-designware-host.c 에 기대는 자리를 이 파일은 커널 공용 코드
 *   (drivers/pci/ecam.c, irqdomain, irqchip)로 직접 메운다.
 * 옆쪽: 같은 벤더의 pcie-xilinx-nwl.c(Zynq UltraScale+)와
 *   pcie-xilinx-dma-pl.c(PL 쪽 DMA 브리지)가 형제 세대다. 셋이
 *   pcie-xilinx-common.h 의 인터럽트 비트 번호 표를 공유하는데, 그중 이
 *   파일과 dma-pl 판 둘만 그 헤더를 포함한다.
 *   MSI 를 다루지 않는 것이 nwl/dma-pl 판과 가장 크게 다른 점이다 — 이
 *   파일에는 MSI 도메인도 msi 관련 코드도 없고, MSI 는 상위 GIC 의
 *   ITS 가 처리하는 구성으로 보이나 그 근거는 이 트리에 없다.
 *
 * 데이터 흐름:
 *   DT compatible → of_device_get_match_data() → variant
 *     → 판본이 오류 인터럽트 레지스터 오프셋(ir_status/ir_enable)과
 *       misc 인에이블 값(ir_misc_value)을 정하고, CPM5NC_HOST 여부가
 *       "인터럽트를 아예 다루지 않는다" 는 갈림을 만든다.
 *   DT "cfg" 자원 → pci_ecam_create() → port->cfg → bridge->sysdata
 *   IDR/IMR(브리지 창) → 이벤트 도메인 hwirq → intr_cause[] → 로그
 *   IDRN/IDRN_MASK(브리지 창) → INTx 도메인 hwirq 0~3 → 하위 장치
 *
 * 공유 상태: struct xilinx_cpm_pcie 하나. probe 후 대부분 불변이지만,
 *   두 irq_chip 콜백이 IMR 과 IDRN_MASK 를 읽기-수정-쓰기 하므로 그 둘만
 *   port->lock(raw_spinlock)으로 보호된다.
 *
 * === 주요 함수/구조체 요약 ===
 * xilinx_cpm_pcie_probe()           : 진입점. 판본에 따라 초기화 범위가 갈린다.
 * xilinx_cpm_pcie_init_irq_domain() : 이벤트 도메인(32)과 INTx 도메인(4)을 만든다.
 * xilinx_cpm_setup_irq()            : 사건마다 핸들러를 걸고 연쇄 핸들러 둘을 꽂는다.
 * xilinx_cpm_pcie_event_flow()      : 사건 연쇄 핸들러. IDR&IMR 을 훑어 도메인으로 넘긴다.
 * xilinx_cpm_pcie_intx_flow()       : INTx 연쇄 핸들러. IDRN 의 4비트를 훑는다.
 * xilinx_cpm_pcie_intr_handler()    : 사건 하나를 문자열로 보고한다.
 * xilinx_cpm_pcie_init_port()       : 링크 보고, 인터럽트 정리, 브리지 켜기.
 * xilinx_cpm_pcie_parse_dt()        : 레지스터 창들과 ECAM 창을 얻는다.
 * struct xilinx_cpm_pcie            : 이 드라이버의 상태 전부.
 * struct xilinx_cpm_variant         : 판본마다 달라지는 오프셋 셋과 판본 번호.
 * intr_cause[32]                    : hwirq 번호 → (심볼 이름, 사람이 읽을 문자열).
 *
 * === 네 판본이 나뉘는 지점 ===
 * of_match_table 의 compatible 넷이 각각 다른 variant 를 가리키고, 그 차이가
 * 코드에 나타나는 곳은 정확히 넷이다.
 *   - CPM        : ir_status/ir_enable 이 0 이라 event_flow 의 추가 상태 정리와
 *                  init_port 의 local 인에이블 쓰기를 건너뛴다. 브리지 레지스터
 *                  창이 따로 없어 reg_base 를 ECAM 창(cfg->win)으로 삼는다.
 *   - CPM5       : PCIe0 쪽 ir_status/ir_enable 을 갖고, "cpm_csr" 이라는
 *                  별도 자원에서 브리지 레지스터 창을 얻는다.
 *   - CPM5_HOST1 : CPM5 와 같되 PCIe1 쪽 오프셋과 misc 비트를 쓴다.
 *                  즉 한 SoC 의 두 번째 PCIe 컨트롤러다.
 *   - CPM5NC_HOST: 인터럽트를 아예 다루지 않는다. 도메인 생성, init_port 본문,
 *                  setup_irq, 그리고 되감기까지 전부 건너뛴다. 남는 것은
 *                  ECAM 창을 잡아 버스를 스캔하는 것뿐이다.
 */

/* [한국어] FIELD_GET(). IDRN 의 4비트 INTx 필드를 뽑는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] IRQ_HANDLED 등 인터럽트 핸들러 반환값과 devm_request_irq(). */
#include <linux/interrupt.h>
/* [한국어] irq_set_chip_and_handler(), handle_level_irq(), IRQ_LEVEL 등
 * irq_chip 계층의 기본 정의. */
#include <linux/irq.h>
/* [한국어] irqchip 공통 헤더. 이 파일이 직접 쓰는 이름은 없으나 상류가
 * 포함해 두었다(전수 확인). */
#include <linux/irqchip.h>
/* [한국어] chained_irq_enter()/exit(). 이 파일의 연쇄 핸들러 둘이 이것으로
 * 상위 컨트롤러의 처리를 열고 닫는다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear(), irq_create_mapping(),
 * generic_handle_domain_irq(). 이 파일은 도메인을 둘 만든다(이벤트, INTx). */
#include <linux/irqdomain.h>
/* [한국어] ARRAY_SIZE() 등 기본 관용구. */
#include <linux/kernel.h>
/* [한국어] builtin_platform_driver() 매크로. 이 드라이버는 모듈이 아니라
 * 커널에 붙박이로 들어간다. */
#include <linux/module.h>
/* [한국어] of_address 계열 헤더. 이 파일이 직접 쓰는 이름은 없다(전수 확인). */
#include <linux/of_address.h>
/* [한국어] of_pci 계열 헤더. 역시 직접 쓰는 이름은 없다. */
#include <linux/of_pci.h>
/* [한국어] of_device_get_match_data() 와 of_get_next_child(). 판본 갈림과
 * INTx 인터럽트 컨트롤러 자식 노드 찾기가 여기서 온다. */
#include <linux/of_platform.h>

/* [한국어] drivers/pci 내부 선언. 이 파일이 여기서 얻는 것은 없어 보이지만
 * 상류가 포함해 두었다. */
#include "../pci.h"
/* [한국어] 같은 벤더의 형제 드라이버들과 공유하는 인터럽트 비트 번호 표.
 * XILINX_PCIE_INTR_ 로 시작하는 상수들이 여기서 온다. 값이 BIT(n) 이 아니라
 * 번호 n 자체라, 이 파일은 그것을 IRQ 도메인의 hwirq 로도 쓰고 아래 IMR()
 * 매크로로 감싸 비트마스크로도 쓴다. */
#include "pcie-xilinx-common.h"

/* [한국어] 아래는 두 레지스터 창의 오프셋이 섞여 있다. 접근자가 구분해 준다 —
 * pcie_read()/pcie_write() 는 브리지 창(reg_base), 생 readl()/writel() 은
 * CPM SLCR 창(cpm_base) 이다. */
/* Register definitions */
/* [한국어] Interrupt Decode Register. 어떤 사건이 걸렸는지 알리는 상태
 * 레지스터이며, 비트 번호가 pcie-xilinx-common.h 의 상수와 일치한다.
 * 쓰기로 지운다(write-1-to-clear). */
#define XILINX_CPM_PCIE_REG_IDR		0x00000E10
/* [한국어] Interrupt Mask Register. IDR 의 어느 비트를 실제 인터럽트로
 * 내보낼지 정한다. 이벤트 도메인의 mask/unmask 콜백이 이 레지스터를 만진다. */
#define XILINX_CPM_PCIE_REG_IMR		0x00000E14
/* [한국어] PHY Status/Control Register. 링크 업 비트가 여기 있다. */
#define XILINX_CPM_PCIE_REG_PSCR	0x00000E1C
/* [한국어] Root Port Status/Control Register. 브리지 활성 비트(BEN)가 여기 있다. */
#define XILINX_CPM_PCIE_REG_RPSC	0x00000E20
/* [한국어] Root Port Error FIFO Read. AER 오류를 낸 요청자의 ID 를 담은
 * FIFO 창구다. 오류 핸들러가 읽고 비운다. */
#define XILINX_CPM_PCIE_REG_RPEFR	0x00000E2C
/* [한국어] INTx 상태 레지스터. 네 INTx 선의 현재 상태가 비트 19:16 에 있다. */
#define XILINX_CPM_PCIE_REG_IDRN	0x00000E38
/* [한국어] 위 IDRN 의 마스크 레지스터. INTx 도메인의 mask/unmask 콜백이
 * 이것을 만진다. IDR/IMR 짝과 같은 구조가 INTx 에도 한 겹 더 있는 셈이다. */
#define XILINX_CPM_PCIE_REG_IDRN_MASK	0x00000E3C
/* [한국어] 여기부터 둘은 CPM SLCR 창의 오프셋이다. 잡다한(miscellaneous)
 * 오류 인터럽트의 상태 레지스터. */
#define XILINX_CPM_PCIE_MISC_IR_STATUS	0x00000340
/* [한국어] 그 인에이블 짝. init_port 가 판본별 값을 써 넣는다. */
#define XILINX_CPM_PCIE_MISC_IR_ENABLE	0x00000348
/* [한국어] PCIe0 컨트롤러의 local 오류 비트(비트 1). CPM 과 CPM5 판본이 쓴다. */
#define XILINX_CPM_PCIE0_MISC_IR_LOCAL	BIT(1)
/* [한국어] PCIe1 컨트롤러의 local 오류 비트(비트 2). CPM5_HOST1 판본이 쓴다 —
 * 한 SoC 에 PCIe 컨트롤러가 둘 있고 misc 레지스터를 공유한다는 뜻이다. */
#define XILINX_CPM_PCIE1_MISC_IR_LOCAL	BIT(2)

/* [한국어] 아래 넷도 CPM SLCR 창의 오프셋이며, CPM5 이후 판본에만 있는
 * 컨트롤러별 local 오류 인터럽트 레지스터다. variant 표가 이 중 하나씩을 고른다. */
#define XILINX_CPM_PCIE0_IR_STATUS	0x000002A0
/* [한국어] PCIe1 쪽 상태. CPM5_HOST1 이 쓴다. */
#define XILINX_CPM_PCIE1_IR_STATUS	0x000002B4
/* [한국어] PCIe0 쪽 인에이블. */
#define XILINX_CPM_PCIE0_IR_ENABLE	0x000002A8
/* [한국어] PCIe1 쪽 인에이블. */
#define XILINX_CPM_PCIE1_IR_ENABLE	0x000002BC
/* [한국어] 위 두 인에이블 레지스터에 쓰는 값(비트 0). init_port 가 판본에
 * ir_enable 이 있을 때만 쓴다. */
#define XILINX_CPM_PCIE_IR_LOCAL	BIT(0)

/* [한국어] 인터럽트 "번호"를 "비트마스크"로 바꾸는 매크로. 공용 헤더의
 * XILINX_PCIE_INTR_ 상수들이 번호이기 때문에 필요하다.
 *
 * 토큰 붙이기(##)를 쓴다는 점이 중요하다. IMR(LINK_DOWN) 은 전처리 후에야
 * XILINX_PCIE_INTR_LINK_DOWN 이 되므로, 소스에서 그 상수 이름을 grep 해도
 * 이 참조는 잡히지 않는다. 아래 _IC() 매크로도 같은 방식이다.
 * (그래서 pcie-xilinx-common.h 에 "이 트리의 어느 드라이버도 참조하지 않는다"
 *  고 적힌 상수 몇 개는 실제로는 이 파일이 이 두 매크로로 참조하고 있다 —
 *  자세한 것은 아래 intr_cause[] 표의 주석을 보라.) */
#define IMR(x) BIT(XILINX_PCIE_INTR_ ##x)

/* [한국어] 이 드라이버가 관심 있는 모든 사건의 비트마스크를 한데 모은 것.
 * IMR() 로 번호를 비트로 바꿔 OR 로 합친다.
 *
 * 이름은 IMR(마스크 레지스터)에서 왔지만, 정작 IMR 레지스터에 이 값을 쓰는
 * 곳은 없다. 쓰이는 데는 init_port 에서 IDR 의 걸린 비트를 지울 때 한 곳뿐이다.
 * 즉 실질적으로는 "이 드라이버가 아는 사건 전체" 를 뜻하는 상수다.
 *
 * 아래 목록에 INTX 가 들어 있는 것에 유의할 것 — INTx 도 이벤트 도메인의
 * 한 비트로 올라오고, 거기서 다시 INTx 도메인으로 갈라진다.
 *
 * 줄 끝의 백슬래시는 매크로를 여러 줄로 잇는 표시다. 그 줄들에는 주석을
 * 붙일 수 없으므로(붙이면 연결이 끊긴다) 설명을 여기 모아 둔다.
 * 나열된 20개는 차례로: 링크 다운, 핫리셋, PCIe/일반 설정 타임아웃,
 * AER 3등급(correctable/non-fatal/fatal), 설정 오염 완료, PME_TO_Ack 수신,
 * INTx, PM_PME 수신, AXI 슬레이브 오류 7종, AXI 마스터 오류 2종,
 * 슬레이브 PCIe 타임아웃이다.
 *
 * [상류 코드 관찰] 원본 스냅숏(1f0e418bb6)에서는 이 매크로의 각 줄 끝
 * 백슬래시 앞에 탭이 하나씩 더 있었다. 이 저장소의 이전 정리 커밋
 * (0504a94278)이 줄 끝 주석을 걷어 내면서 그 정렬이 바뀐 것으로, 토큰은
 * 그대로이고 매크로 동작에도 영향이 없다. 이번 작업에서 코드는 손대지 않았다. */
#define XILINX_CPM_PCIE_IMR_ALL_MASK		\
	(					\
		IMR(LINK_DOWN)		|	\
		IMR(HOT_RESET)		|	\
		IMR(CFG_PCIE_TIMEOUT)	|	\
		IMR(CFG_TIMEOUT)	|	\
		IMR(CORRECTABLE)	|	\
		IMR(NONFATAL)		|	\
		IMR(FATAL)		|	\
		IMR(CFG_ERR_POISON)	|	\
		IMR(PME_TO_ACK_RCVD)	|	\
		IMR(INTX)		|	\
		IMR(PM_PME_RCVD)	|	\
		IMR(SLV_UNSUPP)		|	\
		IMR(SLV_UNEXP)		|	\
		IMR(SLV_COMPL)		|	\
		IMR(SLV_ERRP)		|	\
		IMR(SLV_CMPABT)		|	\
		IMR(SLV_ILLBUR)		|	\
		IMR(MST_DECERR)		|	\
		IMR(MST_SLVERR)		|	\
		IMR(SLV_PCIE_TIMEOUT)	\
	)

/* [한국어] 32비트 전부. init_port 가 ~ 를 붙여 0 으로 만든 뒤 IMR 에 쓴다 —
 * "모든 인터럽트 끄기" 를 이렇게 에둘러 표현한다. */
#define XILINX_CPM_PCIE_IDR_ALL_MASK		0xFFFFFFFF
/* [한국어] IDRN 안에서 INTx 네 선이 차지하는 필드(비트 19:16).
 * intx_flow 가 FIELD_GET 으로 이 필드만 뽑아낸다. */
#define XILINX_CPM_PCIE_IDRN_MASK		GENMASK(19, 16)
/* [한국어] 그 필드의 시작 비트(16). INTx 의 mask/unmask 콜백이 hwirq(0~3)에
 * 이 값을 더해 실제 비트 자리를 구한다. */
#define XILINX_CPM_PCIE_IDRN_SHIFT		16

/* Root Port Error FIFO Read Register definitions */
/* [한국어] FIFO 에 읽을 오류 기록이 있음을 알리는 비트(18). */
#define XILINX_CPM_PCIE_RPEFR_ERR_VALID		BIT(18)
/* [한국어] 오류를 낸 요청자의 ID(비트 15:0). BDF 를 담는다. */
#define XILINX_CPM_PCIE_RPEFR_REQ_ID		GENMASK(15, 0)
/* [한국어] FIFO 를 비울 때 쓰는 값. 32비트 전부를 써서 지운다. */
#define XILINX_CPM_PCIE_RPEFR_ALL_MASK		0xFFFFFFFF

/* Root Port Status/control Register definitions */
/* [한국어] Bridge Enable(비트 0). 이 비트를 세워야 브리지가 트랜잭션을
 * 통과시킨다. init_port 의 마지막 단계다. */
#define XILINX_CPM_PCIE_REG_RPSC_BEN		BIT(0)

/* Phy Status/Control Register definitions */
/* [한국어] 링크 업 비트(11). cpm_pcie_link_up() 이 이것 하나만 본다 —
 * 물리 계층과 데이터 링크 계층을 나눠 보는 DWC 계열과 다른 점이다. */
#define XILINX_CPM_PCIE_REG_PSCR_LNKUP		BIT(11)

/* [한국어] 이 드라이버가 지원하는 CPM 판본. 값 자체에는 의미가 없고
 * (0,1,2,3 순서), 코드가 보는 것은 오직 "이 값이 무엇과 같은가" 다.
 * 실제로 비교되는 곳은 세 군데뿐이다 — parse_dt 가 CPM5/CPM5_HOST1 인지,
 * init_port 와 probe 가 CPM5NC_HOST 인지. */
enum xilinx_cpm_version {
	/* [한국어] 1세대 CPM. 브리지 레지스터 창이 따로 없어 ECAM 창을 그대로 쓰고,
	 * 판본별 오류 인터럽트 레지스터(ir_status/ir_enable)가 없다.
	 * 설정자: cpm_host 정적 표.
	 * 읽는 자: parse_dt 가 "이 값이 아니면" 이라는 형태로 간접적으로 본다.
	 * 값 범위: 0.
	 * 동기화: 정적 상수 표라 불변. */
	CPM,
	/* [한국어] CPM5 세대의 첫 번째 PCIe 컨트롤러.
	 * 설정자: cpm5_host 정적 표.
	 * 읽는 자: xilinx_cpm_pcie_parse_dt() 가 "cpm_csr" 자원을 따로 얻을지 판단할 때.
	 * 값 범위: 1.
	 * 동기화: 불변. */
	CPM5,
	/* [한국어] CPM5 세대의 두 번째 PCIe 컨트롤러. 레지스터 배치는 CPM5 와 같고
	 * SLCR 안에서 PCIe1 쪽 오프셋과 비트를 쓴다는 점만 다르다.
	 * 설정자: cpm5_host1 정적 표.
	 * 읽는 자: parse_dt 가 CPM5 와 함께 묶어 본다.
	 * 값 범위: 2.
	 * 동기화: 불변. */
	CPM5_HOST1,
	/* [한국어] CPM5 의 "NC"(non-coherent 로 보이나 이 트리에 근거 없음) 판.
	 * 이 판본만 성격이 다르다 — 인터럽트를 아예 다루지 않는다.
	 * 설정자: cpm5n_host 정적 표. 그 표는 version 만 채우고 나머지 셋은 0 이다.
	 * 읽는 자: xilinx_cpm_pcie_init_port() 가 맨 앞에서 그냥 반환할지,
	 *   xilinx_cpm_pcie_probe() 가 도메인 생성·setup_irq·되감기를 건너뛸지 판단할 때.
	 * 값 범위: 3.
	 * 동기화: 불변. */
	CPM5NC_HOST,
};

/**
 * struct xilinx_cpm_variant - CPM variant information
 * @version: CPM version
 * @ir_status: Offset for the error interrupt status register
 * @ir_enable: Offset for the CPM5 local error interrupt enable register
 * @ir_misc_value: A bitmask for the miscellaneous interrupt status
 */
/* [한국어] 위 상류 kernel-doc 이 각 필드를 한 줄로 요약했고, 아래에 각
 * 필드의 설정자·읽는 자·값 범위·동기화를 덧붙인다.
 *
 * 이 표의 존재 이유는 "판본마다 다른 것" 을 코드 분기가 아니라 데이터로
 * 표현하기 위해서다. 그래서 이 파일에는 판본 비교가 세 군데밖에 없고,
 * 나머지 차이는 전부 이 구조체의 값이 0 인지 아닌지로 처리된다. */
struct xilinx_cpm_variant {
	/* [한국어] 어느 판본인가.
	 * 설정자: 네 정적 표(cpm_host / cpm5_host / cpm5_host1 / cpm5n_host).
	 * 읽는 자: parse_dt(CPM5 계열인지), init_port 와 probe(CPM5NC_HOST 인지).
	 * 값 범위: 위 enum 의 네 값.
	 * 동기화: 불변. */
	enum xilinx_cpm_version version;
	/* [한국어] 판본별 오류 인터럽트 "상태" 레지스터의 SLCR 창 안 오프셋.
	 * 설정자: cpm5_host 는 PCIe0 쪽, cpm5_host1 은 PCIe1 쪽 오프셋을 넣는다.
	 *   cpm_host 와 cpm5n_host 는 넣지 않아 0 으로 남는다.
	 * 읽는 자: xilinx_cpm_pcie_event_flow() 가 0 이 아닐 때만 그 레지스터를
	 *   읽고 되써서 비운다.
	 * 값 범위: 0(없음) 또는 유효한 오프셋.
	 * 동기화: 불변.
	 *
	 * 0 을 "이 판본에는 없다" 는 뜻으로 쓰는 것이 이 구조체의 관용구다. */
	u32 ir_status;
	/* [한국어] 판본별 오류 인터럽트 "인에이블" 레지스터의 오프셋. 위 상류
	 * kernel-doc 이 CPM5 전용임을 밝혀 두었다.
	 * 설정자: cpm5_host / cpm5_host1 만 채운다.
	 * 읽는 자: xilinx_cpm_pcie_init_port() 가 0 이 아닐 때만
	 *   XILINX_CPM_PCIE_IR_LOCAL 을 써 넣는다.
	 * 값 범위: 0(없음) 또는 유효한 오프셋.
	 * 동기화: 불변. */
	u32 ir_enable;
	/* [한국어] misc 인터럽트 인에이블 레지스터에 써 넣을 비트마스크.
	 * 설정자: cpm_host 와 cpm5_host 는 PCIe0 비트, cpm5_host1 은 PCIe1 비트.
	 *   cpm5n_host 는 넣지 않아 0 이다.
	 * 읽는 자: xilinx_cpm_pcie_init_port() 가 판본을 따지지 않고 무조건 쓴다 —
	 *   위 두 필드가 0 검사를 거치는 것과 다르다. 다만 CPM5NC_HOST 는 그
	 *   함수가 맨 앞에서 반환하므로 0 이 쓰이는 일은 없다.
	 * 값 범위: 0, 또는 PCIE0/PCIE1 MISC_IR_LOCAL 중 하나.
	 * 동기화: 불변. */
	u32 ir_misc_value;
};

/**
 * struct xilinx_cpm_pcie - PCIe port information
 * @dev: Device pointer
 * @reg_base: Bridge Register Base
 * @cpm_base: CPM System Level Control and Status Register(SLCR) Base
 * @intx_domain: Legacy IRQ domain pointer
 * @cpm_domain: CPM IRQ domain pointer
 * @cfg: Holds mappings of config space window
 * @intx_irq: legacy interrupt number
 * @irq: Error interrupt number
 * @lock: lock protecting shared register access
 * @variant: CPM version check pointer
 */
/* [한국어] 이 드라이버의 상태 전부. 위 상류 kernel-doc 이 각 필드를 한 줄로
 * 요약했고, 아래에 설정자·읽는 자·값 범위·동기화를 덧붙인다.
 *
 * 이 구조체는 따로 할당되지 않는다. devm_pci_alloc_host_bridge() 에 크기를
 * 넘겨 브리지 뒤에 딸려 잡히고, pci_host_bridge_priv() 로 그 자리를 얻는다.
 * 그래서 이 파일에는 kzalloc 도 kfree 도 없고, 0 초기화도 그쪽에서 보장된다. */
struct xilinx_cpm_pcie {
	/* [한국어] 이 컨트롤러의 장치. 로그와 자원 조회에 두루 쓴다.
	 * 설정자: xilinx_cpm_pcie_probe().
	 * 읽는 자: 거의 모든 함수.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device			*dev;
	/* [한국어] 브리지 레지스터 창. IDR/IMR/PSCR/RPSC/RPEFR/IDRN 이 모두 여기 있다.
	 * 설정자: xilinx_cpm_pcie_parse_dt(). CPM5 계열은 "cpm_csr" 자원을 매핑하고,
	 *   그 밖의 판본은 ECAM 창(cfg->win)을 그대로 가리킨다.
	 * 읽는 자: pcie_read()/pcie_write() 전부.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 창 안의 레지스터는 port->lock 이 보호한다.
	 *
	 * 판본에 따라 "자기 자원" 일 수도 "ECAM 창의 일부" 일 수도 있다는 점이
	 * 이 필드의 특징이다. 후자일 때는 devres 가 아니라 pci_ecam_free() 가
	 * 수명을 쥔다. */
	void __iomem			*reg_base;
	/* [한국어] CPM SLCR(System Level Control and Status Register) 창.
	 * 설정자: xilinx_cpm_pcie_parse_dt() 의 ioremap("cpm_slcr").
	 * 읽는 자: init_port 의 misc 인에이블 쓰기, event_flow 의 상태 정리 두 곳.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 이 창의 접근에는 port->lock 을 쓰지 않는다 —
	 *   읽기-수정-쓰기가 아니라 통째로 쓰기뿐이기 때문이다.
	 *
	 * 브리지 창과 별개로 필요한 이유는, 오류 인터럽트의 상태·인에이블이
	 * PCIe 블록이 아니라 SoC 수준 제어 블록에 놓여 있기 때문이다.
	 * 상류 주석 둘이 그 사실을 각각의 접근 지점에서 짚어 둔다. */
	void __iomem			*cpm_base;
	/* [한국어] INTx(레거시 인터럽트) 도메인. 크기가 PCI_NUM_INTX(4)다.
	 * 설정자: xilinx_cpm_pcie_init_irq_domain().
	 * 읽는 자: xilinx_cpm_pcie_intx_flow() 가 generic_handle_domain_irq 에 넘긴다.
	 * 값 범위: 유효한 도메인 포인터, 또는 해제 후 NULL.
	 * 동기화: probe 경로에서 만들고 오류 경로에서 없앤다.
	 *
	 * 이 도메인의 hwirq 0~3 이 INTA~INTD 에 대응하며, IDRN 레지스터의
	 * 비트 16~19 가 그 자리다. */
	struct irq_domain		*intx_domain;
	/* [한국어] 사건(이벤트) 도메인. 크기가 32 로, 브리지의 IDR 레지스터 폭과 같다.
	 * 설정자: xilinx_cpm_pcie_init_irq_domain().
	 * 읽는 자: xilinx_cpm_setup_irq()(매핑 생성), xilinx_cpm_pcie_event_flow()
	 *   (사건 전달), xilinx_cpm_pcie_intr_handler()(hwirq 되찾기).
	 * 값 범위: 유효한 도메인 포인터, 또는 해제 후 NULL.
	 * 동기화: 위와 같다.
	 *
	 * 이름은 cpm_domain 이지만 irq_chip 이름은 "RC-Event" 다. INTx 도메인이
	 * 이 도메인의 한 비트(INTX)에 매달린 2단 구조라는 것이 요점이다. */
	struct irq_domain		*cpm_domain;
	/* [한국어] ECAM 설정공간 창의 매핑 정보.
	 * 설정자: xilinx_cpm_pcie_parse_dt() 의 pci_ecam_create().
	 * 읽는 자: probe 가 bridge->sysdata 로 넘기고, 공용 ECAM ops 가 그것으로
	 *   설정공간 주소를 계산한다. 판본에 따라 reg_base 도 여기서 나온다.
	 * 값 범위: 유효한 pci_config_window 포인터.
	 * 동기화: probe 후 불변.
	 *
	 * devm 이 아니라서 실패 경로에서 pci_ecam_free() 를 손으로 불러야 한다. */
	struct pci_config_window	*cfg;
	/* [한국어] INTx 연쇄 핸들러가 걸린 가상 IRQ 번호.
	 * 설정자: xilinx_cpm_setup_irq() 의 irq_create_mapping(cpm_domain, INTX).
	 * 읽는 자: 같은 함수가 연쇄 핸들러를 꽂을 때와,
	 *   xilinx_cpm_free_interrupts() 가 그것을 뽑을 때.
	 * 값 범위: 0 이 아닌 가상 IRQ 번호. 0 이면 매핑 실패다.
	 * 동기화: probe 후 불변. */
	int				intx_irq;
	/* [한국어] 상위 인터럽트 컨트롤러에서 이 PCIe 블록으로 오는 IRQ 번호.
	 * 설정자: xilinx_cpm_setup_irq() 의 platform_get_irq(pdev, 0).
	 * 읽는 자: 같은 함수가 사건 연쇄 핸들러를 꽂을 때와, free_interrupts 가 뽑을 때.
	 * 값 범위: 유효한 IRQ 번호. 음수면 조회 실패다.
	 * 동기화: probe 후 불변.
	 *
	 * 이 하나의 IRQ 에 32가지 사건이 모두 실려 오고, event_flow 가 그것을
	 * 도메인으로 흩는다. */
	int				irq;
	/* [한국어] 레지스터 읽기-수정-쓰기를 보호하는 raw 스핀락.
	 * 설정자: xilinx_cpm_pcie_init_irq_domain() 이 마지막에 초기화한다.
	 * 읽는 자: irq_chip 콜백 넷 — INTx 의 mask/unmask 는 IDRN_MASK 를,
	 *   이벤트의 mask/unmask 는 IMR 을 각각 읽고 고쳐 되쓴다.
	 * 값 범위: raw_spinlock_t.
	 * 동기화: 이 필드가 곧 동기화 수단이다. raw 판인 것은 irq_chip 콜백이
	 *   PREEMPT_RT 에서도 잠들 수 없는 문맥에서 불리기 때문이다.
	 *
	 * [상류 코드 관찰] 네 콜백의 잠금 방식이 갈린다. INTx 쪽 둘은
	 * raw_spin_lock_irqsave 를 쓰고 이벤트 쪽 둘은 그냥 raw_spin_lock 을 쓴다.
	 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
	 *
	 * [상류 코드 관찰] 초기화가 xilinx_cpm_pcie_init_irq_domain() 의 맨 끝,
	 * 즉 두 도메인을 만든 뒤에 온다. 또 CPM5NC_HOST 판본은 그 함수 자체를
	 * 건너뛰므로 이 락이 초기화되지 않는데, 그 판본은 도메인도 핸들러도 없어
	 * 콜백에 닿을 길이 없다. 구조체가 0 초기화되어 있다는 점도 함께 봐야 한다. */
	raw_spinlock_t			lock;
	/* [한국어] 이 인스턴스에 매칭된 판본 정보 표.
	 * 설정자: xilinx_cpm_pcie_probe() 의 of_device_get_match_data().
	 * 읽는 자: parse_dt, init_port, event_flow, probe.
	 * 값 범위: 이 파일 아래쪽 네 정적 표 중 하나.
	 * 동기화: probe 후 불변.
	 *
	 * [상류 코드 관찰] probe 가 이 값을 받은 뒤 NULL 검사를 하지 않고 곧바로
	 * variant->version 을 역참조한다. of_match_table 의 모든 항목이 .data 를
	 * 채우고 있어 실제로는 NULL 이 될 수 없는 구조이나, 검사가 없는 것은
	 * 사실이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
	const struct xilinx_cpm_variant   *variant;
};

/* [한국어]
 * pcie_read - 브리지 레지스터 창에서 32비트를 읽는다
 *
 * @port: 이 드라이버의 상태. reg_base 가 채워져 있어야 한다.
 * @reg: 브리지 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * 이 파일에서 브리지 창에 닿는 두 통로 중 읽기 쪽이다. 이름에 벤더 접두사가
 * 없어 짧지만, 가리키는 창은 reg_base 하나로 고정이다 — CPM SLCR 창은 이
 * 헬퍼를 거치지 않고 생 readl_relaxed 로 접근하므로, 어느 함수가 어느 창을
 * 만지는지는 접근자 이름만 보면 가려진다.
 *
 * _relaxed 판을 쓰는 것이 이 파일 전체에 일관된다. 순서가 중요한 곳이 없고,
 * 인터럽트 처리에서 매번 배리어를 치면 비용만 늘기 때문으로 보인다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다. MMIO 읽기에는 실패라는 개념이 없다.
 *
 * 호출 체인:
 *   cpm_pcie_link_up() / 두 flow 함수 / irq_chip 콜백 넷 / init_port
 *     → [이 함수] → readl_relaxed()
 */
static u32 pcie_read(struct xilinx_cpm_pcie *port, u32 reg)
{
	/* [한국어] 브리지 창 시작에 오프셋을 더한 주소를 읽는다. */
	return readl_relaxed(port->reg_base + reg);
}

/* [한국어]
 * pcie_write - 브리지 레지스터 창에 32비트를 쓴다
 *
 * @port: 이 드라이버의 상태.
 * @val: 쓸 값.
 * @reg: 브리지 창 안의 오프셋.
 * @return: 없음.
 *
 * pcie_read() 의 짝이다. 인자 순서가 (값, 오프셋)이라는 점에 유의할 것 —
 * 커널의 writel(값, 주소) 관용구를 따른 것이지만, 읽기 쪽이 (port, reg)이라
 * 두 함수를 나란히 읽을 때 헷갈리기 쉬운 배치다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_chip 콜백 넷 / event_flow / init_port / clear_err_interrupts
 *     → [이 함수] → writel_relaxed()
 */
static void pcie_write(struct xilinx_cpm_pcie *port,
		       u32 val, u32 reg)
{
	/* [한국어] 브리지 창 시작에 오프셋을 더한 주소에 쓴다. */
	writel_relaxed(val, port->reg_base + reg);
}

/* [한국어]
 * cpm_pcie_link_up - 링크가 서 있는지 본다
 *
 * @port: 이 드라이버의 상태.
 * @return: 0 이 아니면 링크가 섰다.
 *
 * PHY 상태 레지스터의 비트 하나만 본다. DWC 계열 글루들이 물리 계층과
 * 데이터 링크 계층 두 비트를 함께 보는 것과 달리 여기서는 비트가 하나뿐인데,
 * 그 비트가 어느 계층을 뜻하는지는 이 트리에서 확인 못 함.
 *
 * 쓰이는 곳이 xilinx_cpm_pcie_init_port() 한 곳뿐이고, 거기서도 로그를
 * 남기는 데만 쓴다. 즉 이 드라이버는 링크가 서지 않아도 초기화를 계속
 * 진행한다 — 링크를 기다리는 루프가 이 파일에 없다.
 *
 * [상류 코드 관찰] 반환 타입이 bool 인데 반환식은 마스킹한 u32 다. bool 로
 * 변환되면서 0/1 로 정규화되므로 동작에는 문제가 없다.
 *
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_init_port() → [이 함수] → pcie_read()
 */
static bool cpm_pcie_link_up(struct xilinx_cpm_pcie *port)
{
	/* [한국어] PHY 상태 레지스터를 읽어 링크 업 비트만 남긴다. */
	return (pcie_read(port, XILINX_CPM_PCIE_REG_PSCR) &
		XILINX_CPM_PCIE_REG_PSCR_LNKUP);
}

/* [한국어]
 * cpm_pcie_clear_err_interrupts - 오류 FIFO 에 기록이 있으면 읽어 로그를 남기고 비운다
 *
 * @port: 이 드라이버의 상태.
 * @return: 없음.
 *
 * AER 등급 오류(correctable/non-fatal/fatal) 세 가지가 올라왔을 때만 불린다.
 * 브리지는 오류를 낸 요청자의 ID 를 FIFO 에 담아 두는데, 그것을 읽어 디버그
 * 로그로 남기고 FIFO 를 비우는 것이 이 함수의 전부다.
 *
 * "비운다" 가 곧 "32비트 전부를 되쓴다" 인 점이 이 하드웨어의 관용구다 —
 * write-1-to-clear 레지스터라 무엇을 쓰든 세워진 비트가 지워진다.
 *
 * dev_dbg 라서 기본 빌드에서는 아무것도 출력되지 않는다. 즉 실질적인 효과는
 * FIFO 를 비워 다음 오류가 기록될 자리를 만드는 것이다.
 *
 * [상류 코드 관찰] 유효 비트가 서 있지 않으면 FIFO 를 비우지 않고 그냥
 * 돌아간다. 그래서 AER 인터럽트가 걸렸는데 FIFO 가 비어 있는 경우에는
 * 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(xilinx_cpm_pcie_intr_handler 안).
 * 잠들지 않는다. port->lock 을 잡지 않는데, 이 레지스터를 만지는 곳이
 * 여기 하나뿐이라 경합할 상대가 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_intr_handler() → [이 함수] → pcie_read(), pcie_write()
 */
static void cpm_pcie_clear_err_interrupts(struct xilinx_cpm_pcie *port)
{
	/* [한국어] 오류 FIFO 창구를 읽는다. unsigned long 인 것은 아래 dev_dbg 의
	 * %lu 서식과 맞추기 위해서다. */
	unsigned long val = pcie_read(port, XILINX_CPM_PCIE_REG_RPEFR);

	/* [한국어] 유효 비트가 서 있을 때만 기록이 있다는 뜻이다. */
	if (val & XILINX_CPM_PCIE_RPEFR_ERR_VALID) {
		/* [한국어] 하위 16비트가 오류를 낸 요청자의 ID(BDF)다. 어느 장치가 오류를
		 * 냈는지 가리는 유일한 단서다. */
		dev_dbg(port->dev, "Requester ID %lu\n",
			val & XILINX_CPM_PCIE_RPEFR_REQ_ID);
		/* [한국어] 32비트 전부를 되써 FIFO 를 비운다. write-1-to-clear 라 이렇게
		 * 통째로 쓰는 것이 이 하드웨어의 지우기 방식이다. */
		pcie_write(port, XILINX_CPM_PCIE_RPEFR_ALL_MASK,
			   XILINX_CPM_PCIE_REG_RPEFR);
	}
}

/* [한국어]
 * xilinx_cpm_mask_leg_irq - INTx 한 선을 마스크한다 (irq_chip.irq_mask)
 *
 * @data: 이 IRQ 의 irq_data. hwirq 가 0~3(INTA~INTD)이고, chip_data 에
 *        이 드라이버의 상태가 들어 있다.
 * @return: 없음.
 *
 * INTx 도메인의 irq_chip 콜백이다. IDRN_MASK 레지스터에서 해당 비트를 지워
 * 그 선의 인터럽트가 올라오지 않게 한다.
 *
 * hwirq 에 XILINX_CPM_PCIE_IDRN_SHIFT(16)를 더하는 것이 요점이다. 도메인의
 * hwirq 는 0~3 이지만 레지스터에서 그 네 선이 차지하는 자리는 비트 16~19 라,
 * 두 좌표계를 옮겨 주어야 한다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 인터럽트 문맥일 수도 프로세스
 * 문맥일 수도 있어, 읽기-수정-쓰기를 port->lock 으로 감싸고 로컬 인터럽트도
 * 함께 막는다(irqsave 판).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(disable_irq 등) → irq_chip->irq_mask → [이 함수]
 *     → pcie_read(), pcie_write()
 */
static void xilinx_cpm_mask_leg_irq(struct irq_data *data)
{
	/* [한국어] 도메인을 만들 때 host_data 로 넘긴 port 를 chip_data 에서 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(data);
	/* [한국어] irqsave/irqrestore 가 주고받을 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 지울 비트 하나. */
	u32 mask;
	/* [한국어] 레지스터의 현재 값. */
	u32 val;

	/* [한국어] hwirq(0~3)를 레지스터 안의 자리(비트 16~19)로 옮긴다. */
	mask = BIT(data->hwirq + XILINX_CPM_PCIE_IDRN_SHIFT);
	/* [한국어] 읽기-수정-쓰기를 원자적으로 만든다. 같은 레지스터를 다른 INTx
	 * 선의 mask/unmask 가 동시에 만질 수 있기 때문이다. raw 판인 것은 이
	 * 콜백이 PREEMPT_RT 에서도 잠들 수 없는 문맥에서 불리기 때문이다. */
	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 다른 세 선의 마스크 상태를 보존해야 하므로 먼저 읽는다. */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IDRN_MASK);
	/* [한국어] 해당 비트만 지워 되쓴다. */
	pcie_write(port, (val & (~mask)), XILINX_CPM_PCIE_REG_IDRN_MASK);
	/* [한국어] 잠금을 풀고 인터럽트 상태를 되돌린다. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/* [한국어]
 * xilinx_cpm_unmask_leg_irq - INTx 한 선의 마스크를 푼다 (irq_chip.irq_unmask)
 *
 * @data: 이 IRQ 의 irq_data.
 * @return: 없음.
 *
 * 바로 위 마스크 판의 거울이며, 비트를 지우는 대신 세운다는 것 하나만 다르다.
 * 좌표 변환(hwirq + 16)도 잠금 방식도 같다.
 *
 * 하위 장치 드라이버가 request_irq() 로 INTx 를 잡으면 IRQ 코어가 이 콜백을
 * 불러 그 선을 연다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(enable_irq, request_irq 등) → irq_chip->irq_unmask → [이 함수]
 *     → pcie_read(), pcie_write()
 */
static void xilinx_cpm_unmask_leg_irq(struct irq_data *data)
{
	/* [한국어] chip_data 에서 이 드라이버의 상태를 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(data);
	/* [한국어] irqsave/irqrestore 가 주고받을 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 세울 비트 하나. */
	u32 mask;
	/* [한국어] 레지스터의 현재 값. */
	u32 val;

	/* [한국어] hwirq(0~3)를 레지스터 안의 자리(비트 16~19)로 옮긴다. */
	mask = BIT(data->hwirq + XILINX_CPM_PCIE_IDRN_SHIFT);
	/* [한국어] 마스크 판과 같은 이유로 읽기-수정-쓰기를 감싼다. */
	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 다른 선의 상태를 보존하기 위해 먼저 읽는다. */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IDRN_MASK);
	/* [한국어] 해당 비트를 세워 되쓴다. 이 순간부터 그 INTx 선이 올라올 수 있다. */
	pcie_write(port, (val | mask), XILINX_CPM_PCIE_REG_IDRN_MASK);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/* [한국어] INTx 도메인이 쓸 irq_chip. 이름 "INTx" 가 /proc/interrupts 에 그대로
 * 나타난다. mask/unmask 둘뿐이고 ack 나 eoi 가 없는 것은, 레벨 트리거 신호라
 * 하위 장치가 원인을 없애야 신호가 내려가기 때문이다 — 컨트롤러 쪽에서
 * 지울 것이 없다. */
static struct irq_chip xilinx_cpm_leg_irq_chip = {
	/* [한국어] /proc/interrupts 등에 표시될 이름. */
	.name		= "INTx",
	/* [한국어] 그 선을 닫는다. */
	.irq_mask	= xilinx_cpm_mask_leg_irq,
	/* [한국어] 그 선을 연다. */
	.irq_unmask	= xilinx_cpm_unmask_leg_irq,
};

/**
 * xilinx_cpm_pcie_intx_map - Set the handler for the INTx and mark IRQ as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: HW interrupt number
 *
 * Return: Always returns 0.
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * irq_domain_ops.map 콜백이다. 도메인에 새 hwirq 매핑이 생길 때 IRQ 코어가
 * 한 번 불러, 그 가상 IRQ 에 irq_chip 과 흐름 핸들러를 매단다.
 *
 * handle_level_irq 를 고른 것이 요점이다. INTx 는 레벨 트리거 신호라, 핸들러가
 * 돌기 전에 마스크하고 끝난 뒤 언마스크하는 이 흐름이 맞다. 에지 트리거용
 * 흐름을 쓰면 원인이 남아 있는 동안 인터럽트가 계속 재진입한다.
 *
 * IRQ_LEVEL 상태 플래그도 같은 사실을 IRQ 코어에 알리는 표시다.
 *
 * 실행 컨텍스트: 매핑이 만들어질 때. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 위 상류 주석이 밝히듯 언제나 0 을 돌려준다.
 *
 * 호출 체인:
 *   irq_create_mapping() / irq_domain 코어 → domain->ops->map → [이 함수]
 *     → irq_set_chip_and_handler(), irq_set_chip_data(), irq_set_status_flags()
 */
static int xilinx_cpm_pcie_intx_map(struct irq_domain *domain,
				    unsigned int irq, irq_hw_number_t hwirq)
{
	/* [한국어] 이 가상 IRQ 에 INTx irq_chip 과 레벨 트리거 흐름 핸들러를 매단다. */
	irq_set_chip_and_handler(irq, &xilinx_cpm_leg_irq_chip,
				 handle_level_irq);
	/* [한국어] 도메인 생성 시 넘긴 host_data(= port)를 chip_data 로 옮겨 둔다.
	 * mask/unmask 콜백이 irq_data_get_irq_chip_data 로 이것을 되찾는다. */
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] 레벨 트리거임을 IRQ 코어에 표시한다. */
	irq_set_status_flags(irq, IRQ_LEVEL);

	/* [한국어] 위 상류 주석대로 언제나 성공이다. */
	return 0;
}

/* INTx IRQ Domain operations */
/* [한국어] INTx 도메인의 동작. map 만 있고 xlate 가 없다 — 같은 계열의
 * pcie-xilinx-nwl.c 는 pci_irqd_intx_xlate 를 채운다. */
static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 새 매핑이 생길 때 irq_chip 과 흐름 핸들러를 매단다. */
	.map = xilinx_cpm_pcie_intx_map,
};

/* [한국어]
 * xilinx_cpm_pcie_intx_flow - INTx 연쇄 핸들러. 걸린 INTx 선을 하위 도메인으로 넘긴다
 *
 * @desc: 이 연쇄 핸들러가 걸린 상위 IRQ 의 서술자. handler_data 에 port 가 있다.
 * @return: 없음.
 *
 * 이 파일의 2단 인터럽트 구조에서 아래쪽 단이다. 이벤트 도메인의 INTX 비트에
 * 매핑된 가상 IRQ 에 연쇄 핸들러로 꽂혀 있어, 그 사건이 올라오면 불린다.
 *
 * 하는 일은 IDRN 레지스터의 4비트 필드를 읽어 세워진 비트마다 INTx 도메인의
 * 해당 hwirq 를 부르는 것이다. 그러면 그 선에 request_irq 한 하위 장치
 * 드라이버의 핸들러가 돈다.
 *
 * 상태 비트를 지우지 않는다는 점이 중요하다. INTx 는 레벨 트리거라 하위
 * 장치가 원인을 없애야 신호가 내려간다 — 컨트롤러 쪽에서 지울 수 있는
 * 것이 아니다. 같은 계열의 pcie-xilinx-nwl.c 도 같은 이유로 지우지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. chained_irq_enter/exit 로 상위
 * 컨트롤러의 처리를 열고 닫는다 — 연쇄 핸들러의 규약이다.
 *
 * 에러 경로: generic_handle_domain_irq 의 반환값을 보지 않는다. 매핑되지
 * 않은 hwirq 면 그 함수가 오류를 돌려주지만 여기서는 무시한다.
 *
 * 호출 체인:
 *   IRQ 코어 → [이 함수] → generic_handle_domain_irq(intx_domain, 0~3)
 *     → 하위 장치 드라이버의 핸들러
 */
static void xilinx_cpm_pcie_intx_flow(struct irq_desc *desc)
{
	/* [한국어] irq_set_chained_handler_and_data 로 함께 넘겨 둔 port 를 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 컨트롤러의 irq_chip. 아래 enter/exit 가 이것을 조작한다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] IDRN 에서 뽑아낸 4비트 필드. for_each_set_bit 이 요구하는
	 * unsigned long 타입이다. */
	unsigned long val;
	/* [한국어] 세워진 비트를 훑는 커서. 그대로 INTx 도메인의 hwirq 가 된다. */
	int i;

	/* [한국어] 상위 컨트롤러의 처리를 연다. 연쇄 핸들러의 규약이다. */
	chained_irq_enter(chip, desc);

	/* [한국어] IDRN 을 읽어 INTx 네 선이 차지하는 비트 19:16 만 뽑아 0~3 자리로
	 * 내린다. FIELD_GET 이 마스킹과 시프트를 한 번에 해 준다. */
	val = FIELD_GET(XILINX_CPM_PCIE_IDRN_MASK,
			pcie_read(port, XILINX_CPM_PCIE_REG_IDRN));

	/* [한국어] 세워진 비트마다 — 최대 넷(PCI_NUM_INTX)이다. */
	for_each_set_bit(i, &val, PCI_NUM_INTX)
		/* [한국어] INTx 도메인의 그 hwirq 를 부른다. 여기서 하위 장치 드라이버의
		 * 핸들러로 넘어간다. 상태 비트는 지우지 않는다 — 레벨 신호이기 때문이다. */
		generic_handle_domain_irq(port->intx_domain, i);

	/* [한국어] 상위 컨트롤러의 처리를 닫는다. */
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * xilinx_cpm_mask_event_irq - 사건 하나를 마스크한다 (irq_chip.irq_mask)
 *
 * @d: 이 IRQ 의 irq_data. hwirq 가 0~31 이며 IDR/IMR 의 비트 번호와 같다.
 * @return: 없음.
 *
 * 이벤트 도메인의 irq_chip 콜백이다. IMR 레지스터에서 해당 비트를 지워 그
 * 사건이 인터럽트로 나가지 않게 한다.
 *
 * INTx 판과 달리 좌표 변환이 없다. 이벤트 도메인의 hwirq 가 곧 IDR/IMR 의
 * 비트 번호이기 때문이며, 그것이 pcie-xilinx-common.h 가 상수를 BIT(n) 이
 * 아니라 번호 n 으로 정의해 둔 이유이기도 하다.
 *
 * [상류 코드 관찰] 잠금이 raw_spin_lock 이라 로컬 인터럽트를 막지 않는다.
 * 같은 파일의 INTx 판 둘은 raw_spin_lock_irqsave 를 쓴다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어 → irq_chip->irq_mask → [이 함수] → pcie_read(), pcie_write()
 */
static void xilinx_cpm_mask_event_irq(struct irq_data *d)
{
	/* [한국어] chip_data 에서 이 드라이버의 상태를 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(d);
	/* [한국어] IMR 의 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] 다른 사건들의 마스크 상태를 지키기 위해 읽기-수정-쓰기를 감싼다.
	 * 위 [상류 코드 관찰] 대로 irqsave 판이 아니다. */
	raw_spin_lock(&port->lock);
	/* [한국어] 현재 마스크를 읽는다. */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IMR);
	/* [한국어] hwirq 를 그대로 비트 번호로 써서 그 사건만 끈다. */
	val &= ~BIT(d->hwirq);
	/* [한국어] 되쓴다. */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IMR);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock(&port->lock);
}

/* [한국어]
 * xilinx_cpm_unmask_event_irq - 사건 하나의 마스크를 푼다 (irq_chip.irq_unmask)
 *
 * @d: 이 IRQ 의 irq_data.
 * @return: 없음.
 *
 * 바로 위 마스크 판의 거울이며 비트를 세운다는 것만 다르다.
 * xilinx_cpm_setup_irq() 가 사건마다 devm_request_irq 를 부르면 IRQ 코어가
 * 이 콜백을 통해 그 사건을 열어 준다 — 즉 init_port 가 IMR 을 0 으로 만들어
 * 다 꺼 두어도, 핸들러를 거는 과정에서 필요한 비트들이 다시 켜진다.
 *
 * 잠금이 raw_spin_lock 인 것은 마스크 판과 같다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(request_irq 등) → irq_chip->irq_unmask → [이 함수]
 *     → pcie_read(), pcie_write()
 */
static void xilinx_cpm_unmask_event_irq(struct irq_data *d)
{
	/* [한국어] chip_data 에서 이 드라이버의 상태를 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(d);
	/* [한국어] IMR 의 읽기-수정-쓰기용 임시 값. */
	u32 val;

	/* [한국어] 마스크 판과 같은 이유로 감싼다. */
	raw_spin_lock(&port->lock);
	/* [한국어] 현재 마스크를 읽는다. */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IMR);
	/* [한국어] 그 사건 비트를 세운다. */
	val |= BIT(d->hwirq);
	/* [한국어] 되쓴다. 이 순간부터 그 사건이 인터럽트로 나갈 수 있다. */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IMR);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock(&port->lock);
}

/* [한국어] 이벤트 도메인이 쓸 irq_chip. 이름 "RC-Event"(루트 컴플렉스 사건)가
 * /proc/interrupts 에 나타난다. INTx 판과 마찬가지로 mask/unmask 둘뿐이지만,
 * 이쪽은 레벨 신호라서가 아니라 상태 지우기를 연쇄 핸들러(event_flow)가
 * 한꺼번에 하기 때문이다. */
static struct irq_chip xilinx_cpm_event_irq_chip = {
	/* [한국어] 표시 이름. */
	.name		= "RC-Event",
	/* [한국어] 그 사건을 끈다. */
	.irq_mask	= xilinx_cpm_mask_event_irq,
	/* [한국어] 그 사건을 켠다. */
	.irq_unmask	= xilinx_cpm_unmask_event_irq,
};

/* [한국어]
 * xilinx_cpm_pcie_event_map - 이벤트 도메인의 새 매핑에 irq_chip 을 매단다
 *
 * @domain: 이벤트 IRQ 도메인.
 * @irq: 가상 IRQ 번호.
 * @hwirq: 하드웨어 인터럽트 번호(0~31, IDR 의 비트 번호).
 * @return: 언제나 0.
 *
 * irq_domain_ops.map 콜백이며, 위 INTx 판(xilinx_cpm_pcie_intx_map)과 본문이
 * 거의 같다 — irq_chip 만 다르다. 상류가 INTx 판에는 kernel-doc 을 붙이고
 * 이쪽에는 붙이지 않은 것도 그 때문으로 보인다.
 *
 * 여기서도 handle_level_irq 를 고른다. 사건 비트는 event_flow 가 처리한 뒤
 * IDR 에 되써서 지우므로, 핸들러가 도는 동안 마스크되어 있는 레벨 흐름이
 * 재진입을 막아 준다.
 *
 * 실행 컨텍스트: 매핑이 만들어질 때(xilinx_cpm_setup_irq 안). 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_create_mapping() → domain->ops->map → [이 함수]
 *     → irq_set_chip_and_handler(), irq_set_chip_data(), irq_set_status_flags()
 */
static int xilinx_cpm_pcie_event_map(struct irq_domain *domain,
				     unsigned int irq, irq_hw_number_t hwirq)
{
	/* [한국어] 이 가상 IRQ 에 "RC-Event" irq_chip 과 레벨 흐름 핸들러를 매단다. */
	irq_set_chip_and_handler(irq, &xilinx_cpm_event_irq_chip,
				 handle_level_irq);
	/* [한국어] 도메인의 host_data(= port)를 chip_data 로 옮겨, mask/unmask 가
	 * 되찾을 수 있게 한다. */
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] 레벨 트리거임을 표시한다. */
	irq_set_status_flags(irq, IRQ_LEVEL);
	/* [한국어] 언제나 성공이다. */
	return 0;
}

/* [한국어] 이벤트 도메인의 동작. INTx 도메인과 마찬가지로 map 하나뿐이다. */
static const struct irq_domain_ops event_domain_ops = {
	/* [한국어] 새 매핑에 irq_chip 과 흐름 핸들러를 매단다. */
	.map = xilinx_cpm_pcie_event_map,
};

/* [한국어]
 * xilinx_cpm_pcie_event_flow - 사건 연쇄 핸들러. 걸린 사건을 모두 도메인으로 흩는다
 *
 * @desc: 이 연쇄 핸들러가 걸린 상위 IRQ 의 서술자. handler_data 에 port 가 있다.
 * @return: 없음.
 *
 * 이 파일 인터럽트 구조의 위쪽 단이다. 상위 인터럽트 컨트롤러에서 이 PCIe
 * 블록으로 오는 IRQ 하나에 꽂혀 있고, 그 하나에 32가지 사건이 모두 실려 온다.
 *
 * 절차는 넷이다.
 *   1. IDR(상태)과 IMR(마스크)을 AND 해서 "걸렸고 또 열려 있는" 비트만 남긴다.
 *      마스크된 사건까지 처리하면 disable_irq 한 쪽의 기대를 깨기 때문이다.
 *   2. 세워진 비트마다 이벤트 도메인의 해당 hwirq 를 부른다. 그중 INTX 비트는
 *      xilinx_cpm_pcie_intx_flow() 로 이어져 2단이 된다.
 *   3. 처리한 비트들을 IDR 에 되써서 지운다(write-1-to-clear). 처리 전이 아니라
 *      처리 후에 지우는 순서다.
 *   4. 판본별 오류 상태 레지스터와 misc 오류 상태 레지스터를 각각 확인해
 *      값이 있으면 되써서 비운다. 이 둘은 브리지 창이 아니라 CPM SLCR 창에
 *      있어 pcie_read/write 가 아니라 생 readl/writel_relaxed 를 쓴다 —
 *      아래 상류 주석이 그 사실을 짚어 둔다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. chained_irq_enter/exit 로 상위 컨트롤러의
 * 처리를 열고 닫는다.
 *
 * 에러 경로: generic_handle_domain_irq 의 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   IRQ 코어 → [이 함수]
 *     → generic_handle_domain_irq(cpm_domain, 0~31)
 *        → xilinx_cpm_pcie_intr_handler() 또는 xilinx_cpm_pcie_intx_flow()
 */
static void xilinx_cpm_pcie_event_flow(struct irq_desc *desc)
{
	/* [한국어] 연쇄 핸들러를 꽂을 때 함께 넘긴 port 를 되찾는다. */
	struct xilinx_cpm_pcie *port = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 컨트롤러의 irq_chip. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 판본 정보. 아래에서 ir_status 가 있는 판본인지 가린다. */
	const struct xilinx_cpm_variant *variant = port->variant;
	/* [한국어] 걸린 사건 비트마스크. for_each_set_bit 이 요구하는 타입이다. */
	unsigned long val;
	/* [한국어] 세워진 비트를 훑는 커서이자 그대로 hwirq 다. */
	int i;

	/* [한국어] 상위 컨트롤러의 처리를 연다. */
	chained_irq_enter(chip, desc);
	/* [한국어] 어떤 사건이 걸렸는지 읽는다. */
	val =  pcie_read(port, XILINX_CPM_PCIE_REG_IDR);
	/* [한국어] 마스크와 AND 해서 "열려 있는" 사건만 남긴다. 마스크된 사건까지
	 * 처리하면 disable_irq 로 닫아 둔 쪽의 기대가 깨진다. */
	val &= pcie_read(port, XILINX_CPM_PCIE_REG_IMR);
	/* [한국어] 남은 비트를 하나씩 — IDR 이 32비트라 상한이 32 다. */
	for_each_set_bit(i, &val, 32)
		/* [한국어] 이벤트 도메인의 그 hwirq 를 부른다. 대부분은
		 * xilinx_cpm_pcie_intr_handler() 로 가고, INTX 비트만 intx_flow 로 간다. */
		generic_handle_domain_irq(port->cpm_domain, i);
	/* [한국어] 처리한 비트들을 되써서 지운다. 처리 전이 아니라 후에 지우는
	 * 순서라, 핸들러가 도는 동안 같은 사건이 다시 걸리면 그것도 함께 지워진다. */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IDR);

	/* [한국어] CPM5 계열만 이 레지스터를 갖는다. 0 이면 없는 판본이다. */
	if (variant->ir_status) {
		/* [한국어] SLCR 창의 판본별 오류 상태를 읽는다. 브리지 창이 아니므로
		 * pcie_read 가 아니라 생 readl_relaxed 다. */
		val = readl_relaxed(port->cpm_base + variant->ir_status);
		/* [한국어] 걸린 것이 있으면. */
		if (val)
			/* [한국어] 읽은 값을 그대로 되써서 지운다(write-1-to-clear). */
			writel_relaxed(val, port->cpm_base +
				       variant->ir_status);
	}

	/*
	 * XILINX_CPM_PCIE_MISC_IR_STATUS register is mapped to
	 * CPM SLCR block.
	 */
	/* [한국어] 위 상류 주석대로 이 레지스터는 CPM SLCR 블록에 있다. 판본을
	 * 따지지 않고 언제나 확인한다 — 위 ir_status 가 0 검사를 거치는 것과 다르다. */
	val = readl_relaxed(port->cpm_base + XILINX_CPM_PCIE_MISC_IR_STATUS);
	/* [한국어] 걸린 것이 있으면. */
	if (val)
		/* [한국어] 되써서 지운다. */
		writel_relaxed(val,
			       port->cpm_base + XILINX_CPM_PCIE_MISC_IR_STATUS);

	/* [한국어] 상위 컨트롤러의 처리를 닫는다. */
	chained_irq_exit(chip, desc);
}

/* [한국어] 아래 intr_cause[] 표의 한 줄을 만드는 매크로. 이름 하나로
 * "배열의 어느 자리인가" 와 "그 자리에 넣을 심볼 문자열" 을 함께 만들어 낸다.
 *   _IC(LINK_DOWN, "Link Down")
 *     → [XILINX_PCIE_INTR_LINK_DOWN] = { "LINK_DOWN", "Link Down" }
 * 앞의 __stringify(x) 가 심볼 이름을 그대로 문자열로 바꾸고, 그 문자열은
 * devm_request_irq 의 이름 인자로 쓰여 /proc/interrupts 에 나타난다.
 *
 * 줄 끝 백슬래시로 이어지는 매크로라 그 줄들에는 주석을 붙일 수 없어
 * 설명을 여기 모아 둔다.
 *
 * 위 IMR() 과 마찬가지로 토큰 붙이기(##)를 쓴다는 점이 중요하다.
 * XILINX_PCIE_INTR_CFG_PCIE_TIMEOUT 같은 이름은 전처리 후에야 생기므로
 * 소스에서 그 이름을 grep 해도 이 참조는 잡히지 않는다. */
#define _IC(x, s)                              \
	[XILINX_PCIE_INTR_ ## x] = { __stringify(x), s }

/* [한국어] hwirq 번호(0~31)를 사람이 읽을 수 있는 두 문자열로 옮기는 표.
 * 지정 초기화(designated initializer)라 나열 순서가 아니라 대괄호 안의
 * 상수가 자리를 정하며, 나열되지 않은 자리는 두 포인터가 모두 NULL 이다.
 * 그 NULL 여부가 코드에서 실제 의미를 갖는다 — xilinx_cpm_setup_irq() 가
 * str 이 NULL 인 자리를 건너뛰기 때문이다.
 *
 * 그래서 표에 없는 두 자리가 중요하다.
 *   - INTX(16): 일부러 뺐다. 이 사건은 문자열 로그가 아니라 연쇄 핸들러
 *     (xilinx_cpm_pcie_intx_flow)로 이어져야 하므로, setup_irq 의 일반
 *     핸들러 등록 루프에서 빠지고 그 아래에서 따로 다뤄진다.
 *   - 그 밖의 미정의 비트: 이 하드웨어가 쓰지 않는 자리다. 만약 그런 비트가
 *     걸리면 xilinx_cpm_pcie_intr_handler() 가 "Unknown IRQ" 로 보고한다.
 *
 * [상류 코드 관찰] 이 표와 위 IMR() 매크로가 토큰 붙이기로 참조하는 탓에,
 * pcie-xilinx-common.h 에 이미 달려 있는 한국어 주석이 다섯 상수
 * (CFG_PCIE_TIMEOUT, CFG_ERR_POISON, PME_TO_ACK_RCVD, PM_PME_RCVD,
 * SLV_PCIE_TIMEOUT)를 두고 "이 트리의 어느 드라이버도 참조하지 않는다" 고
 * 적어 두었는데, 실제로는 다섯 모두 이 파일이 IMR() 과 _IC() 로 각각 한 번씩
 * 참조한다. 이름 그대로의 grep 으로는 잡히지 않아 생긴 오해로 보인다.
 * 그 헤더는 이번 작업의 대상이 아니라 고치지 않았다.
 *
 * PM_PME_RCVD 와 MSI 가 둘 다 번호 17 인데 이 파일은 PM_PME_RCVD 쪽 이름만
 * 쓴다는 점도 함께 볼 것 — 이 드라이버가 MSI 를 다루지 않는다는 사실이
 * 이름 선택에 드러나 있다. */
static const struct {
	/* [한국어] 심볼 이름 그대로의 문자열("LINK_DOWN" 등).
	 * 설정자: _IC 매크로의 __stringify(x).
	 * 읽는 자: xilinx_cpm_setup_irq() 가 devm_request_irq 의 이름 인자로 넘긴다.
	 * 값 범위: 정적 문자열 리터럴, 또는 표에 없는 자리에서는 NULL.
	 * 동기화: 정적 상수 표라 불변. */
	const char      *sym;
	/* [한국어] 사람이 읽을 설명 문자열("Link Down" 등).
	 * 설정자: _IC 매크로의 두 번째 인자.
	 * 읽는 자: xilinx_cpm_pcie_intr_handler() 가 dev_warn 으로 출력하고,
	 * xilinx_cpm_setup_irq() 는 이 값이 NULL 인지로 "핸들러를 걸 자리인가" 를 판단한다.
	 * 값 범위: 정적 문자열 리터럴, 또는 NULL.
	 * 동기화: 불변. */
	const char      *str;
} intr_cause[32] = {
	/* [한국어] 비트 0. 링크가 끊어졌다. */
	_IC(LINK_DOWN,		"Link Down"),
	/* [한국어] 비트 3. 호스트가 핫리셋을 걸었다. */
	_IC(HOT_RESET,		"Hot reset"),
	/* [한국어] 비트 8. 설정 트랜잭션이 시간 초과됐다. */
	_IC(CFG_TIMEOUT,	"ECAM access timeout"),
	/* [한국어] 비트 9. AER correctable — 링크가 스스로 복구한 오류다.
	 * 아래 핸들러가 이 셋에 대해서만 오류 FIFO 를 비운다. */
	_IC(CORRECTABLE,	"Correctable error message"),
	/* [한국어] 비트 10. AER non-fatal — 그 트랜잭션은 실패했지만 링크는 살아 있다. */
	_IC(NONFATAL,		"Non fatal error message"),
	/* [한국어] 비트 11. AER fatal — 링크 자체를 믿을 수 없는 상태다. */
	_IC(FATAL,		"Fatal error message"),
	/* [한국어] 비트 20. 아래 SLV_ 계열은 컨트롤러가 AXI 슬레이브로 동작할 때
	 * 나는 오류들이다. 지원하지 않는 요청을 받았다. */
	_IC(SLV_UNSUPP,		"Slave unsupported request"),
	/* [한국어] 비트 21. 예상하지 못한 완료를 받았다. */
	_IC(SLV_UNEXP,		"Slave unexpected completion"),
	/* [한국어] 비트 22. 완료를 기다리다 시간이 초과됐다. */
	_IC(SLV_COMPL,		"Slave completion timeout"),
	/* [한국어] 비트 23. 오염(poison) 표시가 붙은 응답을 받았다. */
	_IC(SLV_ERRP,		"Slave Error Poison"),
	/* [한국어] 비트 24. 완료자 중단(Completer Abort)을 받았다. */
	_IC(SLV_CMPABT,		"Slave Completer Abort"),
	/* [한국어] 비트 25. 허용되지 않는 버스트 형태의 요청을 받았다. */
	_IC(SLV_ILLBUR,		"Slave Illegal Burst"),
	/* [한국어] 비트 26. 아래 둘은 컨트롤러가 AXI 마스터로 동작할 때 나는
	 * 오류다. 주소에 해당하는 대상이 없다. */
	_IC(MST_DECERR,		"Master decode error"),
	/* [한국어] 비트 27. 슬레이브가 오류 응답을 돌려주었다. */
	_IC(MST_SLVERR,		"Master slave error"),
	/* [한국어] 비트 4. PCIe 쪽 설정 트랜잭션이 시간 초과됐다. 위 CFG_TIMEOUT(8)과
	 * 다른 사건이다. */
	_IC(CFG_PCIE_TIMEOUT,	"PCIe ECAM access timeout"),
	/* [한국어] 비트 12. 설정 트랜잭션의 완료에 오염 표시가 붙어 왔다. */
	_IC(CFG_ERR_POISON,	"ECAM poisoned completion received"),
	/* [한국어] 비트 15. 서스펜드 절차에서 하위 장치가 PME_TO_Ack 로 응답했다. */
	_IC(PME_TO_ACK_RCVD,	"PME_TO_ACK message received"),
	/* [한국어] 비트 17. 하위 장치가 절전 상태에서 깨어나고 싶다는 PM_PME 를 보냈다.
	 * 공용 헤더에서 MSI 와 같은 번호를 쓰는 자리이며, 이 파일은 PM_PME 쪽으로 해석한다. */
	_IC(PM_PME_RCVD,	"PM_PME message received"),
	/* [한국어] 비트 28. 슬레이브 쪽 PCIe 트랜잭션이 시간 초과됐다. */
	_IC(SLV_PCIE_TIMEOUT,	"PCIe completion timeout received"),
};

/* [한국어]
 * xilinx_cpm_pcie_intr_handler - 사건 하나를 사람이 읽을 문자열로 보고한다
 *
 * @irq: 이 핸들러가 걸린 가상 IRQ 번호.
 * @dev_id: devm_request_irq 에 넘긴 port.
 * @return: 언제나 IRQ_HANDLED.
 *
 * xilinx_cpm_setup_irq() 가 intr_cause[] 의 문자열이 있는 모든 사건에 대해
 * 같은 함수를 등록한다. 그래서 이 함수 하나가 열아홉 가지 사건을 모두 받고,
 * 어느 사건인지는 hwirq 로 가린다.
 *
 * 그 hwirq 를 얻는 방법이 특이하다. 핸들러 인자로는 가상 IRQ 번호만 오므로,
 * 이벤트 도메인에서 irq_data 를 되찾아 거기 담긴 hwirq 를 본다. 같은 계열의
 * pcie-xilinx-dma-pl.c 도 같은 방식을 쓴다.
 *
 * 하는 일은 로그 한 줄이 거의 전부다. 다만 AER 3종(correctable/non-fatal/
 * fatal)만은 오류 FIFO 를 먼저 비우고, fallthrough 로 default 갈래에 흘러가
 * 로그도 함께 남긴다 — case 를 따로 두지 않고 fallthrough 를 쓴 것이
 * "FIFO 정리는 추가 작업이지 대체 작업이 아니다" 를 표현한다.
 *
 * 즉 이 드라이버는 오류를 복구하지 않는다. AER 복구는 상위 PCI 코어의
 * 몫이고, 여기서는 무슨 일이 있었는지 알리기만 한다.
 *
 * [상류 코드 관찰] irq_domain_get_irq_data() 의 반환을 NULL 검사 없이
 * 곧바로 역참조한다. 이 핸들러가 걸린 IRQ 는 모두 그 도메인에서 만든
 * 매핑이라 실제로는 NULL 이 될 수 없는 구조이나, 검사가 없는 것은 사실이다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들지 않는다.
 *
 * 에러 경로: 없다. 표에 없는 hwirq 는 "Unknown IRQ" 로 보고한다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_event_flow() → generic_handle_domain_irq() → [이 함수]
 *     → cpm_pcie_clear_err_interrupts()(AER 3종만), dev_warn()
 */
static irqreturn_t xilinx_cpm_pcie_intr_handler(int irq, void *dev_id)
{
	/* [한국어] devm_request_irq 에 넘긴 port 를 되찾는다. */
	struct xilinx_cpm_pcie *port = dev_id;
	/* [한국어] dev_warn 에 쓸 장치. */
	struct device *dev = port->dev;
	/* [한국어] 아래에서 hwirq 를 꺼낼 irq_data. */
	struct irq_data *d;

	/* [한국어] 같은 함수가 여러 IRQ 에 걸리므로, 이벤트 도메인에서 irq_data 를
	 * 되찾아 hwirq 로 어느 사건인지 가린다. */
	d = irq_domain_get_irq_data(port->cpm_domain, irq);

	/* [한국어] hwirq 가 곧 IDR 의 비트 번호이자 intr_cause[] 의 첨자다. */
	switch (d->hwirq) {
	/* [한국어] AER correctable. */
	case XILINX_PCIE_INTR_CORRECTABLE:
	/* [한국어] AER non-fatal. */
	case XILINX_PCIE_INTR_NONFATAL:
	/* [한국어] AER fatal. 위 셋만 아래 FIFO 정리를 함께 한다. */
	case XILINX_PCIE_INTR_FATAL:
		/* [한국어] 오류를 낸 요청자 ID 를 읽어 로그로 남기고 FIFO 를 비운다. */
		cpm_pcie_clear_err_interrupts(port);
		/* [한국어] 일부러 아래로 흘려보낸다 — FIFO 정리는 추가 작업이고,
		 * 문자열 보고는 이 셋에도 그대로 필요하기 때문이다. */
		fallthrough;

	/* [한국어] 나머지 사건 전부와, 위 셋이 흘러 들어오는 자리. */
	default:
		/* [한국어] 표에 문자열이 있는 사건인가. */
		if (intr_cause[d->hwirq].str)
			/* [한국어] 있으면 그대로 경고로 남긴다. 복구는 하지 않는다 —
			 * 이 드라이버가 오류에 대해 하는 일은 보고가 전부다. */
			dev_warn(dev, "%s\n", intr_cause[d->hwirq].str);
		/* [한국어] 표에 없는 비트가 걸렸다 — 이 하드웨어가 쓰지 않는 자리다. */
		else
			/* [한국어] 번호만이라도 남겨 둔다. */
			dev_warn(dev, "Unknown IRQ %ld\n", d->hwirq);
	}

	/* [한국어] 이 사건을 처리했다고 알린다. 실제 상태 비트 지우기는 상위
	 * 연쇄 핸들러(event_flow)가 돌아가서 한다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * xilinx_cpm_free_irq_domains - 만든 IRQ 도메인 둘을 없앤다
 *
 * @port: 이 드라이버의 상태.
 * @return: 없음.
 *
 * 두 도메인을 각각 NULL 검사 후 없애고 포인터를 NULL 로 되돌린다. NULL 로
 * 되돌리기 때문에 두 번 불려도 안전한데, 실제로 그럴 수 있는 구조다 —
 * xilinx_cpm_pcie_init_irq_domain() 이 실패 경로에서 이 함수를 부르고,
 * probe 의 err_free_irq_domains 라벨도 같은 함수를 부른다.
 *
 * 다만 init_irq_domain 이 실패하면 probe 는 곧바로 반환하므로 두 경로가
 * 겹치지는 않는다.
 *
 * 실행 컨텍스트: probe 경로의 되감기. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_init_irq_domain()(실패 시) / xilinx_cpm_pcie_probe()
 *     → [이 함수] → irq_domain_remove()
 */
static void xilinx_cpm_free_irq_domains(struct xilinx_cpm_pcie *port)
{
	/* [한국어] 만들어졌을 때만 없앤다. */
	if (port->intx_domain) {
		/* [한국어] INTx 도메인을 없앤다. */
		irq_domain_remove(port->intx_domain);
		/* [한국어] 두 번 없애지 않도록 표시를 지운다. */
		port->intx_domain = NULL;
	}

	/* [한국어] 이벤트 도메인도 마찬가지로. */
	if (port->cpm_domain) {
		/* [한국어] 없앤다. */
		irq_domain_remove(port->cpm_domain);
		/* [한국어] 표시를 지운다. */
		port->cpm_domain = NULL;
	}
}

/**
 * xilinx_cpm_pcie_init_irq_domain - Initialize IRQ domain
 * @port: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 인터럽트 도메인 둘을 만든다. 둘 다 DT 의 첫 자식 노드(인터럽트 컨트롤러
 * 노드)를 fwnode 로 삼는데, 이는 하위 장치의 DT 가 interrupt-parent 로
 * 그 노드를 가리키면 여기서 만든 도메인에 이어지게 하기 위해서다.
 *
 * 두 도메인이 같은 fwnode 를 공유하므로 서로 구분할 표시가 필요하다. 그것이
 * irq_domain_update_bus_token() 이며, 이벤트 도메인에는 NEXUS, INTx 도메인에는
 * WIRED 를 붙인다. 토큰이 다르면 같은 노드를 가리키는 조회에서도 원하는
 * 도메인을 골라낼 수 있다.
 *
 * 크기가 각각 32 와 PCI_NUM_INTX(4)인 것은 IDR 의 폭과 INTx 선 수에서 온다.
 *
 * [상류 코드 관찰] raw_spin_lock_init() 이 이 함수의 끝, 즉 두 도메인을
 * 만든 뒤에 온다. 그래서 CPM5NC_HOST 판본은 이 함수를 통째로 건너뛰어
 * 락이 초기화되지 않는다 — 그 판본에는 도메인도 핸들러도 없어 락을 쓰는
 * 콜백에 닿을 길이 없고, 구조체가 0 으로 초기화되어 있기도 하다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] out 라벨은 언제나 -ENOMEM 을 돌려준다. 그리로 오는 두
 * 경로가 모두 도메인 생성 실패라 맞는 값이다. 다만 자식 노드가 없을 때의
 * -EINVAL 반환은 of_node_put 을 거치지 않는데, 그 경우 노드 참조를 아직
 * 얻지 못했으므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 자식 노드가 없으면 -EINVAL, 도메인 생성이 실패하면 out 으로 가서
 * 이미 만든 도메인을 없애고 -ENOMEM 을 돌려준다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_probe() → [이 함수]
 *     → of_get_next_child(), irq_domain_create_linear(),
 *       irq_domain_update_bus_token(), raw_spin_lock_init()
 */
static int xilinx_cpm_pcie_init_irq_domain(struct xilinx_cpm_pcie *port)
{
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = port->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. 아래에서 첫 자식을 찾는 출발점이다. */
	struct device_node *node = dev->of_node;
	/* [한국어] 인터럽트 컨트롤러 역할을 하는 자식 노드. 두 도메인이 이것을 공유한다. */
	struct device_node *pcie_intc_node;

	/* Setup INTx */
	/* [한국어] 첫 자식을 가져온다. 참조 카운트가 올라가므로 아래에서 반드시
	 * of_node_put 으로 놓아 주어야 한다. */
	pcie_intc_node = of_get_next_child(node, NULL);
	/* [한국어] 자식이 없다 = DT 에 인터럽트 컨트롤러 노드가 빠졌다. */
	if (!pcie_intc_node) {
		/* [한국어] DT 가 잘못된 경우라 사용자에게 알린다. */
		dev_err(dev, "No PCIe Intc node found\n");
		/* [한국어] 아직 참조를 얻지 못했으므로 of_node_put 없이 반환한다. */
		return -EINVAL;
	}

	/* [한국어] 사건 도메인. 크기 32 는 IDR 레지스터의 폭과 같고, 마지막 인자
	 * port 가 host_data 로 저장되어 map 콜백이 chip_data 로 옮긴다. */
	port->cpm_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), 32,
						    &event_domain_ops, port);
	/* [한국어] 도메인 생성 실패. */
	if (!port->cpm_domain)
		/* [한국어] 아직 만든 것이 없지만 out 이 통일된 정리 경로다. */
		goto out;

	/* [한국어] NEXUS 토큰을 붙인다. 아래 INTx 도메인과 같은 fwnode 를 쓰므로,
	 * 토큰이 둘을 구분하는 표시가 된다. */
	irq_domain_update_bus_token(port->cpm_domain, DOMAIN_BUS_NEXUS);

	/* [한국어] INTx 도메인. 크기가 PCI_NUM_INTX(4)로 INTA~INTD 넷이다. */
	port->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, port);
	/* [한국어] 도메인 생성 실패. */
	if (!port->intx_domain)
		/* [한국어] 이미 만든 이벤트 도메인을 없애러 간다. */
		goto out;

	/* [한국어] WIRED 토큰 — 하드웨어 배선으로 이어진 인터럽트라는 뜻이다. */
	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED);

	/* [한국어] of_get_next_child 가 올린 참조를 놓아 준다. */
	of_node_put(pcie_intc_node);
	/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점. 도메인을 다 만든 뒤에
	 * 락을 초기화한다. 아직 핸들러가 꽂히지 않아 콜백이 불릴 수 없는 시점이다. */
	raw_spin_lock_init(&port->lock);

	/* [한국어] 두 도메인이 준비됐다. */
	return 0;
/* [한국어] 도메인 생성 실패가 모이는 곳. */
out:
	/* [한국어] 만들어진 도메인이 있으면 없앤다. NULL 검사가 안에 있어 안전하다. */
	xilinx_cpm_free_irq_domains(port);
	/* [한국어] 노드 참조를 놓아 준다. 성공 경로와 중복되지 않는다 —
	 * 성공 경로는 이미 위에서 반환했다. */
	of_node_put(pcie_intc_node);
	/* [한국어] 실패 사유를 알린다. */
	dev_err(dev, "Failed to allocate IRQ domains\n");

	/* [한국어] 두 경로 모두 도메인 할당 실패이므로 -ENOMEM 이 맞다. */
	return -ENOMEM;
}

/* [한국어]
 * xilinx_cpm_setup_irq - 사건마다 핸들러를 걸고 연쇄 핸들러 둘을 꽂는다
 *
 * @port: 이 드라이버의 상태. 두 도메인이 이미 만들어져 있어야 한다.
 * @return: 0 성공, 음수 실패.
 *
 * 이 파일 인터럽트 구조가 실제로 조립되는 자리다. 세 단계를 밟는다.
 *   1. DT 에서 상위 IRQ 번호를 얻는다 — 32가지 사건이 모두 이 하나로 온다.
 *   2. intr_cause[] 에 문자열이 있는 자리마다 이벤트 도메인 매핑을 만들고
 *      devm_request_irq 로 xilinx_cpm_pcie_intr_handler 를 건다. 여기서
 *      IRQ 코어가 unmask 콜백을 불러 IMR 의 해당 비트를 켜 준다 —
 *      init_port 가 IMR 을 0 으로 만들어 두었는데도 사건이 올라오는 이유가
 *      이것이다.
 *   3. INTX 비트에도 매핑을 만들되 request_irq 가 아니라 연쇄 핸들러를 꽂고,
 *      상위 IRQ 에도 사건 연쇄 핸들러를 꽂는다.
 *
 * 2단계 루프가 str 이 NULL 인 자리를 건너뛰는 것이 요점이다. INTX(16)는
 * 표에 일부러 넣지 않았으므로 이 루프에서 빠지고, 대신 3단계에서 연쇄
 * 핸들러를 받는다. 그 두 처리 방식의 차이가 곧 "로그만 남기는 사건" 과
 * "다시 흩어야 하는 사건" 의 차이다.
 *
 * [상류 코드 관찰] 실패해도 이미 만든 매핑(irq_create_mapping)을 되돌리지
 * 않는다. devm_request_irq 로 건 핸들러는 devres 가 풀어 주지만, 매핑 자체는
 * 도메인이 없어질 때 함께 정리되는 것에 기대는 구조다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: IRQ 조회 실패는 그 값을, 매핑 실패는 -ENXIO, 핸들러 등록 실패는
 * 그 값을 올린다. 호출자는 err_setup_irq 로 되감는다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_probe() → [이 함수]
 *     → platform_get_irq(), irq_create_mapping(), devm_request_irq(),
 *       irq_set_chained_handler_and_data()
 */
static int xilinx_cpm_setup_irq(struct xilinx_cpm_pcie *port)
{
	/* [한국어] 로그와 devm 등록에 쓸 장치. */
	struct device *dev = port->dev;
	/* [한국어] platform_get_irq 가 받는 플랫폼 디바이스로 거슬러 올라간다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] i 는 사건 번호 커서, irq 는 그때그때 만든 가상 IRQ 번호. */
	int i, irq;

	/* [한국어] DT 의 첫 인터럽트가 이 PCIe 블록의 상위 IRQ 다. 32가지 사건이
	 * 모두 이 하나에 실려 온다. */
	port->irq = platform_get_irq(pdev, 0);
	/* [한국어] 조회 실패. DT 에 인터럽트가 없거나 상위 컨트롤러가 아직 없다. */
	if (port->irq < 0)
		/* [한국어] -EPROBE_DEFER 를 포함한 값을 그대로 올린다. */
		return port->irq;

	/* [한국어] 표 전체(32자리)를 훑는다. */
	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) {
		/* [한국어] 이번 자리의 등록 결과. */
		int err;

		/* [한국어] 문자열이 없는 자리는 이 하드웨어가 쓰지 않는 비트이거나
		 * INTX 처럼 따로 다루는 사건이다. */
		if (!intr_cause[i].str)
			/* [한국어] 건너뛴다. INTX 는 아래에서 연쇄 핸들러를 받는다. */
			continue;

		/* [한국어] 이벤트 도메인에 이 hwirq 매핑을 만든다. 이때 map 콜백이 불려
		 * irq_chip 과 흐름 핸들러가 매달린다. */
		irq = irq_create_mapping(port->cpm_domain, i);
		/* [한국어] 0 은 매핑 실패를 뜻한다. */
		if (!irq) {
			/* [한국어] 어느 사건인지는 남기지 않는다 — 상류 코드가 그렇게 되어 있다. */
			dev_err(dev, "Failed to map interrupt\n");
			/* [한국어] 장치가 없다는 뜻의 -ENXIO 로 알린다. */
			return -ENXIO;
		}

		/* [한국어] 그 가상 IRQ 에 공용 핸들러를 건다. 이름 인자로 심볼 문자열을
		 * 넘겨 /proc/interrupts 에 사건 이름이 보이게 하고, port 를 dev_id 로 넘겨
		 * 핸들러가 되찾을 수 있게 한다. 이 등록 과정에서 IRQ 코어가 unmask 를
		 * 불러 IMR 의 해당 비트가 켜진다. */
		err = devm_request_irq(dev, irq, xilinx_cpm_pcie_intr_handler,
				       0, intr_cause[i].sym, port);
		/* [한국어] 등록 실패. */
		if (err) {
			/* [한국어] 어느 IRQ 였는지 남긴다. */
			dev_err(dev, "Failed to request IRQ %d\n", irq);
			/* [한국어] 그대로 올린다. 앞서 등록한 것들은 devres 가 풀어 준다. */
			return err;
		}
	}

	/* [한국어] INTX 사건에도 매핑을 만든다. 위 루프에서는 문자열이 없어
	 * 건너뛰었던 자리다. */
	port->intx_irq = irq_create_mapping(port->cpm_domain,
					    XILINX_PCIE_INTR_INTX);
	/* [한국어] 매핑 실패. */
	if (!port->intx_irq) {
		/* [한국어] INTx 를 쓸 수 없다는 뜻이다. */
		dev_err(dev, "Failed to map INTx interrupt\n");
		/* [한국어] -ENXIO 로 알린다. */
		return -ENXIO;
	}

	/* Plug the INTx chained handler */
	/* [한국어] 위 상류 주석대로, 이 IRQ 에는 일반 핸들러가 아니라 연쇄 핸들러를
	 * 꽂는다. INTx 사건은 로그로 끝나는 것이 아니라 다시 넷으로 갈라져야 하기
	 * 때문이다. 함께 넘긴 port 를 그 핸들러가 handler_data 로 되찾는다. */
	irq_set_chained_handler_and_data(port->intx_irq,
					 xilinx_cpm_pcie_intx_flow, port);

	/* Plug the main event chained handler */
	/* [한국어] 상위 IRQ 에도 사건 연쇄 핸들러를 꽂는다. 이 줄이 걸리는 순간부터
	 * 하드웨어의 인터럽트가 이 파일로 들어오기 시작한다. */
	irq_set_chained_handler_and_data(port->irq,
					 xilinx_cpm_pcie_event_flow, port);

	/* [한국어] 인터럽트 경로가 모두 조립됐다. */
	return 0;
}

/**
 * xilinx_cpm_pcie_init_port - Initialize hardware
 * @port: PCIe port information
 */
/* [한국어] 위 상류 kernel-doc 이 인자를 요약했고, 아래에 배경을 덧붙인다.
 *
 * 하드웨어를 실제로 켜는 함수다. 순서가 곧 의미다.
 *   1. CPM5NC_HOST 판본이면 아무것도 하지 않고 돌아간다 — 그 판본은
 *      인터럽트도 브리지 활성화도 이 드라이버가 다루지 않는다.
 *   2. 링크 상태를 읽어 로그만 남긴다. 기다리지도, 실패로 보지도 않는다.
 *   3. 모든 인터럽트를 끄고(IMR 에 0), 이미 걸려 있던 상태를 지운다.
 *   4. CPM SLCR 의 misc 인에이블에 판본별 값을 쓰고, 판본에 local 인에이블
 *      레지스터가 있으면 거기에도 쓴다.
 *   5. 브리지 활성 비트(BEN)를 세운다. 이 마지막 한 줄이 있어야 브리지가
 *      트랜잭션을 통과시킨다.
 *
 * 3단계에서 인터럽트를 다 꺼 두는데도 나중에 사건이 올라오는 이유는,
 * xilinx_cpm_setup_irq() 가 devm_request_irq 를 부를 때마다 IRQ 코어가
 * unmask 콜백을 통해 IMR 의 해당 비트를 다시 켜기 때문이다. 즉 "기본은 꺼짐,
 * 핸들러를 건 것만 켜짐" 이라는 규칙이 두 함수에 나뉘어 구현되어 있다.
 *
 * [상류 코드 관찰] 3단계의 "모두 끄기" 를 ~XILINX_CPM_PCIE_IDR_ALL_MASK 로
 * 쓴다. 그 상수가 0xFFFFFFFF 이므로 결과는 0 이며, IMR 에 0 을 쓰는 것과 같다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] misc 인에이블 쓰기는 판본을 따지지 않고 무조건 실행된다.
 * 위 두 필드(ir_status/ir_enable)가 0 검사를 거치는 것과 대비되는데,
 * ir_misc_value 가 0 인 판본은 CPM5NC_HOST 뿐이고 그 판본은 이 함수 맨 앞에서
 * 반환하므로 0 이 쓰이는 일은 없다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 반환값이 void 이며, 링크가 서지 않아도 그대로 진행한다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_probe() → [이 함수]
 *     → cpm_pcie_link_up(), pcie_read(), pcie_write(), writel()
 */
static void xilinx_cpm_pcie_init_port(struct xilinx_cpm_pcie *port)
{
	/* [한국어] 판본 정보. 이 함수의 갈림 셋이 모두 여기서 나온다. */
	const struct xilinx_cpm_variant *variant = port->variant;

	/* [한국어] 인터럽트도 브리지 활성화도 다루지 않는 판본이다. */
	if (variant->version == CPM5NC_HOST)
		/* [한국어] 아무것도 하지 않고 돌아간다. 남는 것은 ECAM 창으로 버스를
		 * 스캔하는 일뿐이다. */
		return;

	/* [한국어] 링크 상태를 확인한다. 결과는 로그로만 쓰인다 — 이 드라이버에는
	 * 링크를 기다리는 루프가 없다. */
	if (cpm_pcie_link_up(port))
		/* [한국어] 섰다고 알린다. */
		dev_info(port->dev, "PCIe Link is UP\n");
	else
		/* [한국어] 서지 않았어도 초기화를 계속한다. 나중에 장치가 붙을 수 있고,
		 * 링크가 없어도 설정공간 접근 자체는 성립하기 때문이다. */
		dev_info(port->dev, "PCIe Link is DOWN\n");

	/* Disable all interrupts */
	/* [한국어] 위 상류 주석대로 모든 인터럽트를 끈다. ~0xFFFFFFFF 는 0 이므로
	 * IMR 에 0 을 쓰는 것과 같다 — 자세한 것은 위 [상류 코드 관찰] 참조.
	 * 여기서 다 꺼 두고, 핸들러를 거는 쪽에서 필요한 비트만 다시 켠다. */
	pcie_write(port, ~XILINX_CPM_PCIE_IDR_ALL_MASK,
		   XILINX_CPM_PCIE_REG_IMR);

	/* Clear pending interrupts */
	/* [한국어] 위 상류 주석대로 이미 걸려 있던 사건을 지운다. IDR 을 읽어
	 * 이 드라이버가 아는 비트만 남긴 뒤 되쓴다(write-1-to-clear).
	 * 부팅 전 펌웨어가 남겼을 수 있는 상태를 치우는 단계다. */
	pcie_write(port, pcie_read(port, XILINX_CPM_PCIE_REG_IDR) &
		   XILINX_CPM_PCIE_IMR_ALL_MASK,
		   XILINX_CPM_PCIE_REG_IDR);

	/*
	 * XILINX_CPM_PCIE_MISC_IR_ENABLE register is mapped to
	 * CPM SLCR block.
	 */
	/* [한국어] 위 상류 주석대로 이 레지스터는 CPM SLCR 블록에 있으므로 생 writel 을
	 * 쓴다. 판본별 misc 인에이블 비트를 켠다 — 위 [상류 코드 관찰] 참조.
	 * _relaxed 가 아닌 writel 인 것이 이 파일의 다른 SLCR 접근과 다르다. */
	writel(variant->ir_misc_value,
	       port->cpm_base + XILINX_CPM_PCIE_MISC_IR_ENABLE);

	/* [한국어] CPM5 계열만 컨트롤러별 local 인에이블 레지스터를 갖는다. */
	if (variant->ir_enable) {
		/* [한국어] 그 레지스터에 local 비트를 켠다. */
		writel(XILINX_CPM_PCIE_IR_LOCAL,
		       port->cpm_base + variant->ir_enable);
	}

	/* Set Bridge enable bit */
	/* [한국어] 위 상류 주석대로 브리지 활성 비트를 세운다. 다른 비트를 지키기
	 * 위해 읽기-수정-쓰기를 한다. 이 한 줄이 있어야 브리지가 트랜잭션을
	 * 통과시키므로, 초기화의 마지막 단계로 놓인 것이다. */
	pcie_write(port, pcie_read(port, XILINX_CPM_PCIE_REG_RPSC) |
		   XILINX_CPM_PCIE_REG_RPSC_BEN,
		   XILINX_CPM_PCIE_REG_RPSC);
}

/**
 * xilinx_cpm_pcie_parse_dt - Parse Device tree
 * @port: PCIe port information
 * @bus_range: Bus resource
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * DT 에서 세 가지 자원을 가져온다.
 *   - "cpm_slcr" : CPM SLCR 창. 판본을 가리지 않고 언제나 필요하다.
 *   - "cfg"      : ECAM 설정공간 창. pci_ecam_create() 에 넘겨 매핑 객체를 만든다.
 *   - "cpm_csr"  : 브리지 레지스터 창. CPM5 계열에만 있다.
 *
 * 마지막 갈림이 이 함수의 요점이다. CPM5 계열은 브리지 레지스터가 별도
 * 자원으로 나와 있지만, 그 밖의 판본은 그것이 ECAM 창 안에 들어 있어
 * cfg->win 을 그대로 reg_base 로 삼는다. 즉 같은 pcie_read/pcie_write 가
 * 판본에 따라 서로 다른 매핑을 두드리게 된다.
 *
 * 그 결과 reg_base 의 수명 관리도 판본마다 다르다. CPM5 계열은 devm 이
 * 풀어 주지만, 그 밖의 판본은 pci_ecam_free() 가 창을 없애면 함께 무효가 된다.
 *
 * pci_ecam_create() 에 pci_generic_ecam_ops 를 넘긴다는 점도 중요하다.
 * 이 드라이버가 설정공간 접근을 한 줄도 직접 구현하지 않는다는 뜻이며,
 * CPM 의 설정공간이 표준 ECAM 배치를 그대로 따른다는 사실을 드러낸다.
 *
 * [상류 코드 관찰] pci_ecam_create() 가 성공한 뒤 "cpm_csr" 매핑이 실패하면
 * 그대로 오류를 반환하는데, 호출자 probe 는 그 경우 err_free_irq_domains 로
 * 가서 IRQ 도메인만 없앤다. 즉 이미 만들어진 port->cfg 는 pci_ecam_free()
 * 되지 않는다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 그대로 올린다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname(),
 *       platform_get_resource_byname(), pci_ecam_create()
 */
static int xilinx_cpm_pcie_parse_dt(struct xilinx_cpm_pcie *port,
				    struct resource *bus_range)
{
	/* [한국어] 자원 조회에 쓸 장치. */
	struct device *dev = port->dev;
	/* [한국어] 자원 조회 API 가 받는 플랫폼 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] "cfg" 자원의 서술자. 매핑은 pci_ecam_create 가 대신 한다. */
	struct resource *res;

	/* [한국어] CPM SLCR 창을 매핑한다. 오류 인터럽트의 상태·인에이블이 여기 있어
	 * 판본을 가리지 않고 필요하다. */
	port->cpm_base = devm_platform_ioremap_resource_byname(pdev,
						       "cpm_slcr");
	/* [한국어] 자원이 없거나 매핑 실패. */
	if (IS_ERR(port->cpm_base))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(port->cpm_base);

	/* [한국어] ECAM 창의 자원 서술자를 이름으로 찾는다. 여기서는 매핑하지 않고
	 * 서술자만 얻는데, 매핑은 pci_ecam_create 가 버스 범위까지 고려해 해야 하기
	 * 때문이다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	/* [한국어] "cfg" 자원이 DT 에 없다. */
	if (!res)
		/* [한국어] 설정공간이 없으면 아무것도 할 수 없다. */
		return -ENXIO;

	/* [한국어] ECAM 매핑 객체를 만든다. 버스 범위(bus_range)와 자원 크기를
	 * 견주어 창을 잡고, pci_generic_ecam_ops 를 그대로 쓴다 — 이 드라이버가
	 * 설정공간 접근을 직접 구현하지 않는다는 뜻이다.
	 * devm 이 아니므로 실패 경로에서 pci_ecam_free 를 손으로 불러야 한다. */
	port->cfg = pci_ecam_create(dev, res, bus_range,
				    &pci_generic_ecam_ops);
	/* [한국어] 창 생성 실패. */
	if (IS_ERR(port->cfg))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(port->cfg);

	/* [한국어] CPM5 계열인가. 이 두 판본만 브리지 레지스터가 별도 자원이다. */
	if (port->variant->version == CPM5 ||
	    port->variant->version == CPM5_HOST1) {
		/* [한국어] "cpm_csr" 창을 따로 매핑한다. */
		port->reg_base = devm_platform_ioremap_resource_byname(pdev,
							    "cpm_csr");
		/* [한국어] 매핑 실패. 위 [상류 코드 관찰] 이 가리키는 지점 —
		 * 이미 만든 port->cfg 는 여기서도 호출자에서도 해제되지 않는다. */
		if (IS_ERR(port->reg_base))
			/* [한국어] 코드를 꺼내 올린다. */
			return PTR_ERR(port->reg_base);
	/* [한국어] 그 밖의 판본. */
	} else {
		/* [한국어] 브리지 레지스터가 ECAM 창 안에 있으므로 그 매핑을 그대로 쓴다.
		 * 이 경우 reg_base 의 수명은 devres 가 아니라 pci_ecam_free 가 쥔다. */
		port->reg_base = port->cfg->win;
	}

	/* [한국어] 세 자원이 모두 준비됐다. */
	return 0;
}

/* [한국어]
 * xilinx_cpm_free_interrupts - 꽂아 둔 연쇄 핸들러 둘을 뽑는다
 *
 * @port: 이 드라이버의 상태.
 * @return: 없음.
 *
 * xilinx_cpm_setup_irq() 의 마지막 두 줄을 되돌린다. 핸들러와 데이터를 모두
 * NULL 로 덮어써서, 이후 인터럽트가 올라와도 이 파일의 코드로 들어오지
 * 않게 한다.
 *
 * devm_request_irq 로 건 열아홉 개의 핸들러는 여기서 풀지 않는다 — devres 가
 * 장치 해제 때 알아서 풀어 주기 때문이다. 즉 이 함수가 손으로 되돌리는 것은
 * devres 가 모르는 연쇄 핸들러 둘뿐이다.
 *
 * 쓰이는 곳이 probe 의 err_host_bridge 라벨 하나뿐이라는 점도 특징이다.
 * 이 드라이버에는 remove 경로가 없다.
 *
 * 실행 컨텍스트: probe 실패 되감기. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_cpm_pcie_probe()(실패 시) → [이 함수]
 *     → irq_set_chained_handler_and_data()
 */
static void xilinx_cpm_free_interrupts(struct xilinx_cpm_pcie *port)
{
	/* [한국어] INTx 연쇄 핸들러를 뽑는다. */
	irq_set_chained_handler_and_data(port->intx_irq, NULL, NULL);
	/* [한국어] 사건 연쇄 핸들러도 뽑는다. 이 줄 뒤로는 하드웨어 인터럽트가
	 * 이 파일로 들어오지 않는다. */
	irq_set_chained_handler_and_data(port->irq, NULL, NULL);
}

/**
 * xilinx_cpm_pcie_probe - Probe function
 * @pdev: Platform device pointer
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 순서가 곧 의존 관계다.
 *   1. 브리지와 이 드라이버의 상태를 한 덩어리로 잡는다. 따로 kzalloc 하지
 *      않고 브리지 뒤에 딸려 붙이는 방식이라, 이 파일에는 kfree 가 없다.
 *   2. 판본을 읽는다. 이후 모든 갈림이 여기서 나온다.
 *   3. (CPM5NC_HOST 가 아니면) IRQ 도메인 둘을 만든다.
 *   4. DT 의 버스 범위를 얻어 parse_dt 에 넘긴다 — ECAM 창을 잡으려면
 *      버스 개수를 알아야 하기 때문이다.
 *   5. 하드웨어를 켠다.
 *   6. (CPM5NC_HOST 가 아니면) 인터럽트 경로를 조립한다.
 *   7. 브리지에 ECAM ops 와 sysdata 를 걸고 버스를 스캔한다.
 *
 * CPM5NC_HOST 검사가 세 번 나오는 것이 이 함수의 모양을 결정한다. 그 판본은
 * 인터럽트를 다루지 않으므로 3·6단계와 되감기 두 곳을 모두 건너뛴다.
 *
 * 되감기 라벨 셋이 층을 이룬다 — 뒤에서 실패할수록 더 많이 되돌린다.
 * 다만 아래 관찰대로 그 층이 완전하지는 않다.
 *
 * [상류 코드 관찰] parse_dt 가 pci_ecam_create() 성공 뒤에 실패하면
 * err_free_irq_domains 로 가는데, 그 라벨은 pci_ecam_free() 를 부르지 않는다.
 * ECAM 창이 해제되지 않은 채 프로브가 끝난다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] devm_pci_alloc_host_bridge() 가 NULL 을 돌려줬을 때
 * -ENOMEM 이 아니라 -ENODEV 를 반환한다. 실패 원인이 메모리 부족인 점을
 * 생각하면 어긋난 코드이나, 원본 스냅숏에서도 같으며 손대지 않았다.
 *
 * [상류 코드 관찰] pci_host_probe() 뒤에 remove 콜백이 없다. 아래
 * platform_driver 에 .remove 가 없고 suppress_bind_attrs 가 켜져 있으며
 * builtin_platform_driver 로 등록되므로, 이 드라이버는 한 번 붙으면 떨어지지
 * 않는다. 그래서 위 되감기 라벨들이 유일한 정리 경로다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 위 설명대로 세 층으로 되감는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_pci_alloc_host_bridge(), of_device_get_match_data(),
 *       xilinx_cpm_pcie_init_irq_domain(), xilinx_cpm_pcie_parse_dt(),
 *       xilinx_cpm_pcie_init_port(), xilinx_cpm_setup_irq(), pci_host_probe()
 */
static int xilinx_cpm_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 이 드라이버의 상태. 아래에서 브리지 뒤의 자리를 가리키게 된다. */
	struct xilinx_cpm_pcie *port;
	/* [한국어] 로그와 자원 조회에 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] PCI 코어가 관리하는 호스트 브리지 객체. */
	struct pci_host_bridge *bridge;
	/* [한국어] DT 의 버스 범위 자원. ECAM 창 크기를 견주는 데 쓴다. */
	struct resource_entry *bus;
	/* [한국어] 각 단계의 결과. */
	int err;

	/* [한국어] 브리지와 이 드라이버의 상태를 한 번에 잡는다. sizeof(*port) 만큼
	 * 여분을 붙여 달라는 뜻이며, 0 초기화도 여기서 보장된다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*port));
	/* [한국어] 할당 실패. */
	if (!bridge)
		/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — 메모리 부족인데
		 * -ENODEV 를 돌려준다. */
		return -ENODEV;

	/* [한국어] 브리지 뒤에 붙은 여분 공간의 주소를 얻는다. 이것이 곧 이
	 * 드라이버의 상태 구조체다. */
	port = pci_host_bridge_priv(bridge);

	/* [한국어] 이후 거의 모든 함수가 쓰는 장치 포인터를 채운다. */
	port->dev = dev;

	/* [한국어] compatible 에 딸린 판본 표를 얻는다. 이 줄 이후로 모든 갈림이
	 * variant 에 의존한다. NULL 검사가 없는 점은 구조체 필드 주석 참조. */
	port->variant = of_device_get_match_data(dev);

	/* [한국어] 인터럽트를 다루는 판본인가. CPM5NC_HOST 만 아니다. */
	if (port->variant->version != CPM5NC_HOST) {
		/* [한국어] 이벤트 도메인과 INTx 도메인을 만든다. */
		err = xilinx_cpm_pcie_init_irq_domain(port);
		/* [한국어] 도메인 생성 실패. */
		if (err)
			/* [한국어] 아직 잡은 것이 없으므로 그대로 반환한다. */
			return err;
	}

	/* [한국어] DT 의 버스 범위를 브리지의 창 목록에서 찾는다. pci_ecam_create 가
	 * 이것을 받아 창 크기가 버스 개수를 감당하는지 본다. */
	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] 버스 범위가 없다 — DT 의 bus-range 가 빠진 경우다. */
	if (!bus) {
		/* [한국어] 장치를 다룰 수 없다는 뜻으로. */
		err = -ENODEV;
		/* [한국어] 방금 만든 도메인을 없애러 간다. */
		goto err_free_irq_domains;
	}

	/* [한국어] SLCR 창, ECAM 창, (판본에 따라) 브리지 레지스터 창을 얻는다. */
	err = xilinx_cpm_pcie_parse_dt(port, bus->res);
	/* [한국어] 자원 획득 실패. */
	if (err) {
		/* [한국어] 어느 자원인지는 하위가 남기지 않으므로 뭉뚱그려 알린다. */
		dev_err(dev, "Parsing DT failed\n");
		/* [한국어] 도메인만 없애러 간다 — 위 [상류 코드 관찰] 대로 이 경로는
		 * 이미 만들어졌을 수 있는 ECAM 창을 풀지 않는다. */
		goto err_free_irq_domains;
	}

	/* [한국어] 하드웨어를 켠다. 링크 보고, 인터럽트 정리, 브리지 활성화가
	 * 이 안에서 일어난다. 반환값이 없어 실패라는 개념이 없다. */
	xilinx_cpm_pcie_init_port(port);

	/* [한국어] 인터럽트를 다루는 판본인가. */
	if (port->variant->version != CPM5NC_HOST) {
		/* [한국어] 핸들러들과 연쇄 핸들러 둘을 꽂는다. */
		err = xilinx_cpm_setup_irq(port);
		/* [한국어] 조립 실패. */
		if (err) {
			/* [한국어] 사용자에게 알린다. */
			dev_err(dev, "Failed to set up interrupts\n");
			/* [한국어] ECAM 창부터 되돌리러 간다. */
			goto err_setup_irq;
		}
	}

	/* [한국어] 공용 ECAM 코드가 설정공간 주소를 계산할 때 쓸 문맥을 넘긴다.
	 * DWC 계열이 여기에 자기 루트 포트 문맥을 넣는 것과 달리, 이 드라이버는
	 * ECAM 매핑 객체를 그대로 넘긴다. */
	bridge->sysdata = port->cfg;
	/* [한국어] 설정공간 접근을 공용 ECAM 구현에 통째로 맡긴다.
	 * [상류 코드 관찰] pci_generic_ecam_ops 는 const 로 정의되어 있어
	 * (drivers/pci/ecam.c 에서 확인) 여기서 const 를 캐스팅으로 떼어 낸다.
	 * bridge->ops 의 타입이 const 가 아니기 때문인데, 실제로 그 내용을 고치지는
	 * 않는다. 원본 스냅숏에서도 같으며 코드는 손대지 않았다. */
	bridge->ops = (struct pci_ops *)&pci_generic_ecam_ops.pci_ops;

	/* [한국어] 버스를 스캔한다. 이 안에서 설정공간을 읽어 장치를 찾고,
	 * 자원을 배정하고, 하위 드라이버를 붙인다. */
	err = pci_host_probe(bridge);
	/* [한국어] 스캔 실패. */
	if (err < 0)
		/* [한국어] 인터럽트부터 되돌리러 간다. */
		goto err_host_bridge;

	/* [한국어] 컨트롤러가 동작 중이다. 이 드라이버에는 떼는 경로가 없다. */
	return 0;

/* [한국어] pci_host_probe 실패만 이 라벨로 온다. */
err_host_bridge:
	/* [한국어] 인터럽트를 다루는 판본일 때만 되돌릴 것이 있다. */
	if (port->variant->version != CPM5NC_HOST)
		/* [한국어] 연쇄 핸들러 둘을 뽑는다. */
		xilinx_cpm_free_interrupts(port);
/* [한국어] setup_irq 실패와 위에서 흘러온 경로가 모이는 곳. */
err_setup_irq:
	/* [한국어] ECAM 창을 없앤다. devm 이 아니라서 손으로 불러야 한다. */
	pci_ecam_free(port->cfg);
/* [한국어] 버스 범위 없음, parse_dt 실패, 그리고 위에서 흘러온 경로가 모이는 곳. */
err_free_irq_domains:
	/* [한국어] 인터럽트를 다루는 판본일 때만 도메인이 있다. */
	if (port->variant->version != CPM5NC_HOST)
		/* [한국어] 두 도메인을 없앤다. */
		xilinx_cpm_free_irq_domains(port);
	/* [한국어] 실패 원인을 그대로 올린다. */
	return err;
}

/* [한국어] 1세대 CPM. ir_status 와 ir_enable 을 채우지 않아 0 으로 남고,
 * 그 0 이 "이 판본에는 그 레지스터가 없다" 는 뜻으로 쓰인다.
 * 브리지 레지스터 창도 따로 없어 parse_dt 가 ECAM 창을 그대로 쓴다. */
static const struct xilinx_cpm_variant cpm_host = {
	/* [한국어] 판본 번호. */
	.version = CPM,
	/* [한국어] SLCR 의 misc 인에이블에 쓸 PCIe0 비트. */
	.ir_misc_value = XILINX_CPM_PCIE0_MISC_IR_LOCAL,
};

/* [한국어] CPM5 세대의 첫 번째 PCIe 컨트롤러. 위와 달리 세 필드를 모두 채운다. */
static const struct xilinx_cpm_variant cpm5_host = {
	/* [한국어] 판본 번호. parse_dt 가 "cpm_csr" 자원을 따로 얻게 만든다. */
	.version = CPM5,
	/* [한국어] misc 인에이블의 PCIe0 비트. */
	.ir_misc_value = XILINX_CPM_PCIE0_MISC_IR_LOCAL,
	/* [한국어] PCIe0 쪽 오류 상태 레지스터 오프셋. event_flow 가 이것을 비운다. */
	.ir_status = XILINX_CPM_PCIE0_IR_STATUS,
	/* [한국어] PCIe0 쪽 오류 인에이블 오프셋. init_port 가 여기에 local 비트를 쓴다. */
	.ir_enable = XILINX_CPM_PCIE0_IR_ENABLE,
};

/* [한국어] CPM5 세대의 두 번째 PCIe 컨트롤러. 위와 구조가 같고 PCIe1 쪽
 * 오프셋과 비트만 다르다 — 한 SoC 에 컨트롤러가 둘이고 SLCR 을 공유한다는 뜻이다. */
static const struct xilinx_cpm_variant cpm5_host1 = {
	/* [한국어] 판본 번호. parse_dt 에서 CPM5 와 함께 묶여 처리된다. */
	.version = CPM5_HOST1,
	/* [한국어] misc 인에이블의 PCIe1 비트. */
	.ir_misc_value = XILINX_CPM_PCIE1_MISC_IR_LOCAL,
	/* [한국어] PCIe1 쪽 오류 상태 오프셋. */
	.ir_status = XILINX_CPM_PCIE1_IR_STATUS,
	/* [한국어] PCIe1 쪽 오류 인에이블 오프셋. */
	.ir_enable = XILINX_CPM_PCIE1_IR_ENABLE,
};

/* [한국어] CPM5 의 NC 판. version 하나만 채우고 나머지 셋은 0 이다.
 * 그 비어 있음이 곧 "인터럽트를 다루지 않는다" 는 선언인데, 실제 갈림은
 * 0 검사가 아니라 version 비교 세 곳으로 구현되어 있다. */
static const struct xilinx_cpm_variant cpm5n_host = {
	/* [한국어] 판본 번호. init_port 와 probe 가 이 값을 직접 비교한다. */
	.version = CPM5NC_HOST,
};

/* [한국어] DT compatible 과 위 네 표를 잇는 매칭 목록. 판본마다 compatible 이
 * 따로 있어, 어느 판본인지는 런타임 탐지가 아니라 DT 가 못박는다. */
static const struct of_device_id xilinx_cpm_pcie_of_match[] = {
	/* [한국어] 첫 항목: 1세대 CPM. */
	{
		/* [한국어] DT 노드가 이 문자열을 쓰면 매칭된다. 버전 접미사가 붙은 유일한 항목이다. */
		.compatible = "xlnx,versal-cpm-host-1.00",
		/* [한국어] 매칭 시 of_device_get_match_data() 가 돌려줄 표. */
		.data = &cpm_host,
	},
	/* [한국어] 둘째 항목: CPM5 의 첫 컨트롤러. */
	{
		/* [한국어] CPM5 용 compatible. */
		.compatible = "xlnx,versal-cpm5-host",
		/* [한국어] CPM5 표. */
		.data = &cpm5_host,
	},
	/* [한국어] 셋째 항목: CPM5 의 두 번째 컨트롤러. */
	{
		/* [한국어] 접미사 "host1" 이 두 번째임을 나타낸다. */
		.compatible = "xlnx,versal-cpm5-host1",
		/* [한국어] PCIe1 쪽 오프셋을 쓰는 표. */
		.data = &cpm5_host1,
	},
	/* [한국어] 넷째 항목: CPM5 NC 판. */
	{
		/* [한국어] CPM5NC 용 compatible. */
		.compatible = "xlnx,versal-cpm5nc-host",
		/* [한국어] 인터럽트를 다루지 않는 표. */
		.data = &cpm5n_host,
	},
	/* [한국어] 목록의 끝을 알리는 빈 항목.
	 * [상류 코드 관찰] 이 파일에는 MODULE_DEVICE_TABLE 선언이 없다.
	 * builtin_platform_driver 로 커널에 붙박이로 들어가므로 모듈 자동 로딩용
	 * 별칭이 필요 없기 때문이다. */
	{}
};

/* [한국어] 플랫폼 드라이버 등록 정보.
 * [상류 코드 관찰] .remove 가 없다. suppress_bind_attrs 로 sysfs 언바인드도
 * 막혀 있고 아래 builtin_platform_driver 로 등록되므로, 이 드라이버는 한 번
 * 붙으면 떨어지지 않는다. probe 의 되감기 라벨들이 유일한 정리 경로인 이유다. */
static struct platform_driver xilinx_cpm_pcie_driver = {
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name = "xilinx-cpm-pcie",
		/* [한국어] 위의 DT 매칭 목록. */
		.of_match_table = xilinx_cpm_pcie_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. PCIe 호스트 브리지를
		 * 임의로 떼면 그 아래 장치가 모두 사라지므로 막아 둔 것이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 매칭된 디바이스마다 불릴 진입점. remove 짝은 두지 않았다. */
	.probe = xilinx_cpm_pcie_probe,
};

/* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다. module_platform_driver 와
 * 달리 모듈 언로드 경로를 만들지 않으므로, 위에 .remove 가 없는 것과 짝이 맞는다. */
builtin_platform_driver(xilinx_cpm_pcie_driver);
