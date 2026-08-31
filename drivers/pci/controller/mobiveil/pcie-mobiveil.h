/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCIe host controller driver for Mobiveil PCIe Host controller
 *
 * Copyright (c) 2018 Mobiveil Inc.
 * Copyright 2019 NXP
 *
 * Author: Subrahmanya Lingappa <l.subrahmanya@mobiveil.co.in>
 *	   Hou Zhiqiang <Zhiqiang.Hou@nxp.com>
 */

/*
 * [한국어 설명] Mobiveil PCIe IP 공통 헤더 — 레지스터 지도와 자료구조 (pcie-mobiveil.h)
 *
 * === 파일의 역할 ===
 * 이 디렉터리의 네 .c 파일이 공유하는 단 하나의 헤더다. 담고 있는 것은
 * 크게 세 가지다.
 *
 * 1) 레지스터 지도. PAB(PCIe AXI Bridge)의 오프셋과 비트 위치를 전부
 *    매크로로 적어 둔다. 이 IP 는 창(window)별 레지스터가 일정 간격으로
 *    반복 배치돼 있어서, 오프셋을 상수로 나열하는 대신
 *    PAB_REG_ADDR(기준, 창번호) 형태의 계산 매크로를 쓴다.
 *
 * 2) 자료구조. struct mobiveil_pcie 를 정점으로, 그 안에
 *    struct mobiveil_root_port 가 값으로 들어가고 다시 그 안에
 *    struct mobiveil_msi 가 값으로 들어가는 3단 중첩이다. 포인터가 아니라
 *    값이므로 별도 할당·해제가 없고 수명이 모두 같다.
 *
 * 3) SoC 별 콜백 표 두 개. struct mobiveil_pab_ops 는 레지스터 계층의
 *    링크 판정을, struct mobiveil_rp_ops 는 루트 포트 계층의 인터럽트
 *    초기화를 SoC 구현으로 바꿔 끼울 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * mobiveil 드라이버는 세 층으로 나뉘고, 이 헤더가 그 셋을 잇는 계약이다.
 *
 *   SoC 층 : pcie-layerscape-gen4.c (fsl,lx2160a-pcie)
 *            pcie-mobiveil-plat.c   (mbvl,gpex40-pcie)
 *      |     — probe 에서 struct mobiveil_pcie 를 채워 넘긴다
 *      v
 *   호스트 층 : pcie-mobiveil-host.c
 *      |        — config 접근 ops, INTx/MSI 도메인, 호스트 초기화
 *      v
 *   레지스터 층 : pcie-mobiveil.c
 *                 — 페이지 방식 레지스터 접근, 주소 창(AMAP), 링크 대기
 *
 * 실행 컨텍스트: 이 헤더 자체는 코드가 아니지만, 여기 선언된 것들은
 *   probe 의 프로세스 컨텍스트와 ISR 의 인터럽트 컨텍스트 양쪽에서 쓰인다.
 *   특히 mobiveil_csr_readl / writel 은 두 컨텍스트에서 모두 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위로는 linux/pci.h, linux/irq.h, linux/msi.h 의 코어 타입에 기댄다 —
 * pci_host_bridge, irq_domain, msi_msg 가 그 예다.
 * 아래로는 이 헤더가 정의하는 인라인 래퍼들이 pcie-mobiveil.c 의
 * mobiveil_csr_read() / mobiveil_csr_write() 로 내려간다.
 *
 * 데이터 흐름은 이렇다. DT 에서 읽은 리소스가 struct mobiveil_pcie 의
 * 각종 베이스 주소 필드에 담기고, 그 베이스에 이 헤더의 오프셋 매크로를
 * 더한 것이 실제 MMIO 접근이 된다. 인터럽트는 반대 방향으로 흐른다 —
 * PAB_INTP_AMBA_MISC_STAT 의 비트가 rp.intx_domain / rp.msi.dev_domain 을
 * 거쳐 장치 드라이버의 핸들러에 닿는다.
 *
 * NVMe 와의 관계는 코드 호출이 아니라 토폴로지상의 것이다. drivers/nvme 는
 * 이 헤더의 어떤 심볼도 부르지 않는다(트리 전수 확인). 다만 이 IP 를 쓴
 * 보드에 NVMe 를 붙이면, NVMe 의 config 접근은 PAB_AXI_AMAP_PEX_WIN_L 에
 * BDF 를 싣는 경로를 지나고, NVMe 의 완료 인터럽트는 위 MSI 경로를 탄다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct mobiveil_pcie      : 컨트롤러 한 대. 베이스 주소들과 창 개수,
 *                             SoC 콜백 표, 그리고 루트 포트 상태를 담는다.
 * struct mobiveil_root_port : 루트 포트 상태. config 베이스, INTx 도메인,
 *                             MSI 묶음, 호스트 브리지 핸들.
 * struct mobiveil_msi       : MSI 벡터 비트맵과 그것을 지키는 뮤텍스,
 *                             그리고 MSI 부모 도메인.
 * struct mobiveil_pab_ops   : link_up 콜백 하나. 링크 판정을 SoC 가 대신한다.
 * struct mobiveil_rp_ops    : interrupt_init 콜백 하나. 인터럽트 초기화를
 *                             SoC 가 대신한다.
 * mobiveil_csr_readl / writel 계열 : 크기별 레지스터 접근 인라인 래퍼.
 * PAB_REG_ADDR / PAB_EXT_REG_ADDR : 창 번호로 레지스터 오프셋을 만드는 매크로.
 * OFFSET_TO_PAGE_ADDR / OFFSET_TO_PAGE_IDX : 논리 오프셋을 페이지 번호와
 *                             페이지 내 주소로 쪼개는 매크로.
 *
 * === 이 헤더를 읽을 때 알아 둘 점 ===
 * 여기 선언된 두 ops 포인터는 pcie-layerscape-gen4.c 에서만 설정된다.
 * 그런데 읽는 쪽(pcie-mobiveil.c 의 mobiveil_pcie_link_up(),
 * pcie-mobiveil-host.c 의 mobiveil_pcie_interrupt_init())은 ops 자체가
 * NULL 인지 검사하지 않고 바로 그 안의 함수 포인터를 본다.
 * ops 를 채우지 않는 pcie-mobiveil-plat.c 경로에서는 두 필드가 NULL 이다.
 * 자세한 내용은 각 필드 주석과 host.c 쪽 주석에 적어 두었다.
 */

/* [한국어] 헤더 중복 포함 방지 가드 시작. 이 헤더는 mobiveil 디렉터리의 네 .c 파일이
 * 모두 포함하므로 가드가 없으면 구조체·매크로 재정의 오류가 난다. */
#ifndef _PCIE_MOBIVEIL_H
/* [한국어] 가드 매크로 정의. 두 번째 포함부터는 아래 내용이 통째로 건너뛰어진다. */
#define _PCIE_MOBIVEIL_H

/* [한국어] struct pci_host_bridge, struct pci_bus, PCI_NUM_INTX, pci_generic_config_read
 * 같은 PCI 코어 타입과 상수. 아래 struct mobiveil_root_port 의 bridge 필드와
 * host.c 의 config 접근 경로가 이 헤더의 정의를 쓴다. */
#include <linux/pci.h>
/* [한국어] struct irq_chip, struct irq_data, raw_spinlock_t 기반 IRQ 인터페이스.
 * mobiveil_root_port 의 intx_mask_lock 과 host.c 의 INTx irq_chip 콜백에 필요하다. */
#include <linux/irq.h>
/* [한국어] struct irq_domain, struct msi_msg 등 MSI 관련 타입. 아래 struct mobiveil_msi 가
 * irq_domain 포인터를 담고, host.c 가 msi_msg 를 조립한다. */
#include <linux/msi.h>
/* [한국어] drivers/pci/pci.h — PCI 코어 내부 전용 헤더. 컨트롤러 드라이버들이 관례적으로
 * 상대 경로로 끌어오는데, 이 파일에서 정확히 어떤 심볼 때문에 필요한지는
 * 이 트리에서 확인하지 못했다(코어의 PCI_NUM_INTX 등은 linux/pci.h 쪽에 있다). */
#include "../../pci.h"

/* register offsets and bit positions */

/*
 * translation tables are grouped into windows, each window registers are
 * grouped into blocks of 4 or 16 registers each
 */
/* [한국어] PAB 의 창(window)별 레지스터가 창 하나당 16바이트 간격으로 반복 배치돼 있다.
 * 아래 PAB_REG_ADDR 이 이 값을 곱해 n번 창의 오프셋을 만든다. */
#define PAB_REG_BLOCK_SIZE		16
/* [한국어] 확장(EXT) 영역의 창 레지스터는 창당 4바이트 간격이다.
 * PAB_EXT_REG_ADDR 이 이 값을 쓴다 — 확장 영역은 창당 레지스터가 하나뿐이기 때문. */
#define PAB_EXT_REG_BLOCK_SIZE		4

/* [한국어] n번 창의 레지스터 오프셋 = 기준 오프셋 + 창번호 x 16.
 * 아래의 PAB_AXI_AMAP_ 계열과 PAB_PEX_AMAP_ 계열이 모두 이 매크로를 거친다.
 * 인자에 괄호가 없어 식을 넘기면 우선순위 문제가 생길 수 있으나,
 * 이 트리의 호출자는 모두 단순 변수나 상수만 넘긴다. */
#define PAB_REG_ADDR(offset, win)	\
	(offset + (win * PAB_REG_BLOCK_SIZE))
/* [한국어] 위와 같되 창당 간격이 4바이트인 확장 영역용. */
#define PAB_EXT_REG_ADDR(offset, win)	\
	(offset + (win * PAB_EXT_REG_BLOCK_SIZE))

/* [한국어] LTSSM(Link Training and Status State Machine) 상태 레지스터의 논리 오프셋.
 * 0xc00 미만이라 페이지 전환 없이 직접 접근 영역에 속한다.
 * 읽는 자: pcie-mobiveil.c 의 mobiveil_pcie_link_up() — SoC 콜백이 없을 때의
 * 기본 링크 판정 경로. */
#define LTSSM_STATUS			0x0404
/* [한국어] 그 레지스터에서 LTSSM 상태 코드만 뽑아내는 마스크(하위 6비트). */
#define  LTSSM_STATUS_L0_MASK		0x3f
/* [한국어] LTSSM 이 L0(정상 데이터 전송 상태)일 때의 코드값(0x2d).
 * 읽은 값에 마스크를 씌운 결과가 이 값이면 링크가 올라온 것으로 본다.
 * gen4 드라이버도 자기 레지스터에 대해 같은 값(PF_DBG_LTSSM_L0)을 따로 정의한다. */
#define  LTSSM_STATUS_L0		0x2d

/* [한국어] PAB_CTRL — PAB(PCIe AXI Bridge) 전역 제어 레지스터.
 * PIO 활성화 비트와 '페이지 선택' 필드가 한 레지스터에 같이 들어 있어서
 * 이 IP 에서 가장 자주 건드리는 레지스터다.
 * 0xc00 미만이므로 이 레지스터 자체는 직접 접근된다 — 그렇지 않았다면
 * 페이지를 고르기 위해 페이지를 골라야 하는 순환이 생겼을 것이다. */
#define PAB_CTRL			0x0808
/* [한국어] AMBA(AXI) 쪽 PIO 를 켜는 비트 위치(0). CPU → PCIe 방향 접근을 연다.
 * 쓰는 자: pcie-mobiveil-host.c 의 mobiveil_host_init(). */
#define  AMBA_PIO_ENABLE_SHIFT		0
/* [한국어] PEX(PCIe) 쪽 PIO 를 켜는 비트 위치(1). PCIe → 시스템 메모리 방향을 연다.
 * 같은 곳에서 위 비트와 함께 한 번에 켠다. */
#define  PEX_PIO_ENABLE_SHIFT		1
/* [한국어] 페이지 선택 필드(pg_sel)가 시작하는 비트 위치(13).
 * pcie-mobiveil.c 의 mobiveil_pcie_sel_page() 가 여기에 페이지 번호를 쓴다. */
#define  PAGE_SEL_SHIFT			13
/* [한국어] pg_sel 필드의 폭을 나타내는 6비트 마스크 — 페이지 번호는 0~63 범위다.
 * 필드를 지울 때와 새 값을 넣을 때 양쪽에 쓰여 옆 비트 침범을 막는다. */
#define  PAGE_SEL_MASK			0x3f
/* [한국어] 페이지 안의 상대 오프셋으로 쓰는 하위 10비트 마스크.
 * 아래 OFFSET_TO_PAGE_ADDR 이 이 마스크로 논리 오프셋의 하위 부분만 남긴다. */
#define  PAGE_LO_MASK			0x3ff
/* [한국어] 논리 오프셋에서 페이지 번호를 뽑을 때 오른쪽으로 미는 비트 수(10).
 * 하위 10비트가 페이지 내 오프셋이므로 그 위쪽이 페이지 번호가 된다. */
#define  PAGE_SEL_OFFSET_SHIFT		10

/* [한국어] PAB 활동 상태 레지스터 — 아직 처리 중인 트랜잭션이 남았는지를 알린다.
 * 읽는 자: pcie-layerscape-gen4.c 의 ls_g4_pcie_reinit_hw() 가 리셋 뒤
 * PAB 가 조용해지기를 기다리는 폴링 조건으로 쓴다.
 * 0x81c 로, 다른 상수들과 달리 네 자리 0 채움 없이 적혀 있다(값은 동일). */
#define PAB_ACTIVITY_STAT		0x81c

/* [한국어] AXI 쪽 PIO 제어 레지스터. */
#define PAB_AXI_PIO_CTRL		0x0840
/* [한국어] 그 레지스터에서 한 번에 켜는 활성화 비트 묶음(하위 4비트).
 * PIO 활성화와 config 창 활성화가 이 안에 함께 들어 있어, 상류 코드는
 * 비트를 따로 나누지 않고 통째로 OR 한다. */
#define  APIO_EN_MASK			0xf

/* [한국어] PCIe 쪽 PIO 제어 레지스터 — PCIe 마스터 동작을 연다. */
#define PAB_PEX_PIO_CTRL		0x08c0
/* [한국어] 그 레지스터의 PIO 활성화 비트 위치(0). */
#define  PIO_ENABLE_SHIFT		0

/* [한국어] 인터럽트 활성화(마스크) 레지스터 — 어떤 이벤트를 IRQ 로 올릴지 고른다.
 * 쓰는 자: host.c 의 INTx 마스크/언마스크와 인터럽트 초기화,
 * 그리고 gen4.c 의 enable/disable interrupt 함수들. */
#define PAB_INTP_AMBA_MISC_ENB		0x0b0c
/* [한국어] 인터럽트 상태 레지스터. 활성화 레지스터와 같은 비트 배치를 쓰며,
 * 해당 비트에 1을 써서 지우는 write-1-to-clear 방식이다.
 * 읽는 자: host.c 의 mobiveil_pcie_isr(), gen4.c 의 ls_g4_pcie_isr(). */
#define PAB_INTP_AMBA_MISC_STAT		0x0b1c
/* [한국어] PCIe 링크에서 리셋(hot reset)이 관측됐음을 알리는 비트(1).
 * 이 트리에서 이 비트를 켜고 처리하는 것은 gen4 드라이버뿐이다 —
 * 리셋을 감지하면 워크큐로 하드웨어를 다시 세운다. */
#define  PAB_INTP_RESET			BIT(1)
/* [한국어] MSI 메시지를 받았음을 알리는 비트(3). */
#define  PAB_INTP_MSI			BIT(3)
/* [한국어] INTA 를 받았음을 알리는 비트(5). 아래 PAB_INTX_START 가 이 5와 같은 값이다. */
#define  PAB_INTP_INTA			BIT(5)
/* [한국어] INTB 비트(6). */
#define  PAB_INTP_INTB			BIT(6)
/* [한국어] INTC 비트(7). */
#define  PAB_INTP_INTC			BIT(7)
/* [한국어] INTD 비트(8). INTA~INTD 가 5~8 에 연속 배치돼 있어 시프트로 다룰 수 있다. */
#define  PAB_INTP_INTD			BIT(8)
/* [한국어] PCIe uncorrectable error 비트(9). gen4 만 이 비트를 활성화한다. */
#define  PAB_INTP_PCIE_UE		BIT(9)
/* [한국어] PM/REDI 계열 이벤트 비트(29). gen4 만 켠다.
 * 이름이 가리키는 정확한 이벤트는 Mobiveil IP 문서가 있어야 알 수 있고,
 * 이 트리 안에서는 확인하지 못했다. */
#define  PAB_INTP_IE_PMREDI		BIT(29)
/* [한국어] EC 계열 이벤트 비트(30). 역시 gen4 만 켠다. 위와 같은 이유로
 * 이름 이상의 의미는 이 트리에서 확인하지 못했다. */
#define  PAB_INTP_IE_EC			BIT(30)
/* [한국어] MSI 비트 하나만 담은 마스크. host.c 가 인터럽트를 켤 때
 * INTx 마스크와 OR 해서 한 번에 기록한다. */
#define  PAB_INTP_MSI_MASK		PAB_INTP_MSI
/* [한국어] INTA~INTD 네 비트를 묶은 마스크.
 * ISR 이 'INTx 가 하나라도 왔는가'를 한 번에 판정할 때, 그리고
 * 인터럽트를 켤 때 네 핀을 한꺼번에 다루는 데 쓴다. */
#define  PAB_INTP_INTX_MASK		(PAB_INTP_INTA | PAB_INTP_INTB |\
					PAB_INTP_INTC | PAB_INTP_INTD)

/* [한국어] n번 아웃바운드(AXI → PCIe) 창의 제어 레지스터.
 * 설정자: pcie-mobiveil.c 의 program_ob_windows(). */
#define PAB_AXI_AMAP_CTRL(win)		PAB_REG_ADDR(0x0ba0, win)
/* [한국어] 그 제어 레지스터에서 창을 켜는 활성화 비트(0). */
#define  WIN_ENABLE_SHIFT		0
/* [한국어] 창 종류 필드가 시작하는 비트 위치(1). */
#define  WIN_TYPE_SHIFT			1
/* [한국어] 창 종류 필드의 폭(2비트). CFG(0)/IO(1)/MEM(2) 세 종류를 구분한다. */
#define  WIN_TYPE_MASK			0x3
/* [한국어] 창 크기 필드 마스크(상위 22비트).
 * 이 IP 는 창 크기를 '비트 수'가 아니라 ~(size - 1) 마스크로 표현한다 —
 * DesignWare 의 iATU 가 한계 주소를 쓰는 것과 다른 방식이다. */
#define  WIN_SIZE_MASK			0xfffffc00

/* [한국어] n번 아웃바운드 창 크기의 나머지 상위 비트(확장 영역).
 * 64비트 크기를 본 레지스터와 이 확장 레지스터 둘로 나눠 담는다. */
#define PAB_EXT_AXI_AMAP_SIZE(win)	PAB_EXT_REG_ADDR(0xbaf0, win)

/* [한국어] n번 아웃바운드 창의 AXI(CPU) 쪽 시작 주소 상위 32비트(확장 영역). */
#define PAB_EXT_AXI_AMAP_AXI_WIN(win)	PAB_EXT_REG_ADDR(0x80a0, win)
/* [한국어] 같은 창의 AXI 쪽 시작 주소 하위 32비트. */
#define PAB_AXI_AMAP_AXI_WIN(win)	PAB_REG_ADDR(0x0ba4, win)
/* [한국어] AXI 창 시작 주소의 하위 2비트는 주소가 아닌 다른 용도라
 * 기록 전에 이 마스크로 지운다. */
#define  AXI_WINDOW_ALIGN_MASK		3

/* [한국어] n번 아웃바운드 창의 PCIe 쪽 목적지 주소 하위 32비트.
 * config 접근용 창(0번)에서는 여기에 주소 대신 대상 BDF 를 싣는다 —
 * host.c 의 mobiveil_pcie_map_bus() 가 config 접근마다 이 레지스터를 다시 쓴다. */
#define PAB_AXI_AMAP_PEX_WIN_L(win)	PAB_REG_ADDR(0x0ba8, win)
/* [한국어] 그 BDF 인코딩에서 버스 번호가 들어가는 비트 위치(24). */
#define  PAB_BUS_SHIFT			24
/* [한국어] 장치(device) 번호가 들어가는 비트 위치(19). */
#define  PAB_DEVICE_SHIFT		19
/* [한국어] 기능(function) 번호가 들어가는 비트 위치(16).
 * 19-16 = 3비트가 function, 24-19 = 5비트가 device 로 PCI 의 8:5:3 배치와 맞는다. */
#define  PAB_FUNCTION_SHIFT		16

/* [한국어] n번 아웃바운드 창의 PCIe 쪽 목적지 주소 상위 32비트. */
#define PAB_AXI_AMAP_PEX_WIN_H(win)	PAB_REG_ADDR(0x0bac, win)
/* [한국어] RC 자신의 config 헤더 중 class code 를 덮어쓰는 shadow 레지스터.
 * 쓰는 자: host.c 의 mobiveil_host_init() 끝부분 — 헤더가 PCI 브리지로
 * 보이도록 클래스 코드를 0x060400 으로 고쳐 넣는다. */
#define PAB_INTP_AXI_PIO_CLASS		0x474

/* [한국어] n번 인바운드(PCIe → AXI) 창의 제어 레지스터.
 * 설정자: pcie-mobiveil.c 의 program_ib_windows(). */
#define PAB_PEX_AMAP_CTRL(win)		PAB_REG_ADDR(0x4ba0, win)
/* [한국어] 그 제어 레지스터의 창 활성화 비트(0). */
#define  AMAP_CTRL_EN_SHIFT		0
/* [한국어] 창 종류 필드가 시작하는 비트 위치(1). */
#define  AMAP_CTRL_TYPE_SHIFT		1
/* [한국어] 창 종류 필드 폭(2비트). 아웃바운드 쪽과 같은 CFG/IO/MEM 구분이다. */
#define  AMAP_CTRL_TYPE_MASK		3

/* [한국어] n번 인바운드 창의 크기(확장 영역). 아웃바운드와 달리 크기가
 * 확장 레지스터 한 곳에만 들어간다. */
#define PAB_EXT_PEX_AMAP_SIZEN(win)	PAB_EXT_REG_ADDR(0xbef0, win)
/* [한국어] n번 인바운드 창의 AXI(시스템 메모리) 쪽 목적지 주소 상위 32비트. */
#define PAB_EXT_PEX_AMAP_AXI_WIN(win)	PAB_EXT_REG_ADDR(0xb4a0, win)
/* [한국어] 같은 창의 AXI 쪽 목적지 주소 하위 32비트. */
#define PAB_PEX_AMAP_AXI_WIN(win)	PAB_REG_ADDR(0x4ba4, win)
/* [한국어] n번 인바운드 창의 PCIe 쪽 시작 주소 하위 32비트. */
#define PAB_PEX_AMAP_PEX_WIN_L(win)	PAB_REG_ADDR(0x4ba8, win)
/* [한국어] 같은 창의 PCIe 쪽 시작 주소 상위 32비트. */
#define PAB_PEX_AMAP_PEX_WIN_H(win)	PAB_REG_ADDR(0x4bac, win)

/* starting offset of INTX bits in status register */
/* [한국어] 상태·활성화 레지스터에서 INTx 비트가 시작하는 위치(5) —
 * 위 PAB_INTP_INTA 와 같은 값이다.
 * host.c 는 hwirq(1~4)에 이 값을 더한 뒤 1을 빼서 비트 번호를 만들고,
 * ISR 은 반대로 상태값을 이만큼 오른쪽으로 밀어 0부터 시작하게 만든다. */
#define PAB_INTX_START			5

/* supported number of MSI interrupts */
/* [한국어] 이 드라이버가 지원하는 MSI 벡터 개수(16).
 * 아래 msi_irq_in_use 비트맵의 크기이자, host.c 가 만드는 MSI 도메인의 크기다. */
#define PCI_NUM_MSI			16

/* MSI registers */
/* [한국어] MSI 창 기준 주소의 하위 32비트를 넣는 레지스터.
 * 오프셋은 csr 영역이 아니라 apb_csr 영역 기준이다 — host.c 가
 * apb_csr_base 에 더해서 접근한다. */
#define MSI_BASE_LO_OFFSET		0x04
/* [한국어] MSI 창 기준 주소 상위 32비트. */
#define MSI_BASE_HI_OFFSET		0x08
/* [한국어] MSI 창 크기. host.c 가 4096 을 쓴다. */
#define MSI_SIZE_OFFSET			0x0c
/* [한국어] MSI 기능 전체 활성화 레지스터. 1을 쓰면 켜진다. */
#define MSI_ENABLE_OFFSET		0x14
/* [한국어] MSI FIFO 에 꺼낼 항목이 남았는지 알리는 상태 레지스터.
 * bit0 이 1이면 아직 대기 중인 MSI 가 있다는 뜻이라 ISR 이 이 비트로 루프를 돈다. */
#define MSI_STATUS_OFFSET		0x18
/* [한국어] FIFO 에서 MSI 데이터(이 드라이버에서는 곧 벡터 번호)를 꺼내는 레지스터. */
#define MSI_DATA_OFFSET			0x20
/* [한국어] FIFO 에서 MSI 목적지 주소 하위 32비트를 꺼내는 레지스터.
 * host.c 는 그 값을 쓰지 않지만, 읽어야만 FIFO 항목이 완전히 빠지므로
 * 일부러 읽는다(상류 주석이 그 이유를 밝히고 있다). */
#define MSI_ADDR_L_OFFSET		0x24
/* [한국어] 같은 이유로 읽는 목적지 주소 상위 32비트. */
#define MSI_ADDR_H_OFFSET		0x28

/* outbound and inbound window definitions */
/* [한국어] 0번 창. config 접근용 아웃바운드 창과 기본 인바운드 창이 모두 0번을 쓴다. */
#define WIN_NUM_0			0
/* [한국어] 1번 창을 가리키는 상수. 다만 이 트리 안에서 이 상수를 실제로 참조하는
 * 코드는 찾지 못했다 — 정의만 남아 있다. */
#define WIN_NUM_1			1
/* [한국어] 창 종류 = config 트랜잭션. 아웃바운드 0번 창을 이 종류로 잡아
 * EP config 접근 경로를 만든다. */
#define CFG_WINDOW_TYPE			0
/* [한국어] 창 종류 = I/O 트랜잭션. DT 의 ranges 에 I/O 영역이 있을 때 쓰인다. */
#define IO_WINDOW_TYPE			1
/* [한국어] 창 종류 = 메모리 트랜잭션. 기본 인바운드 창과 대부분의 아웃바운드 창이 이 종류다. */
#define MEM_WINDOW_TYPE			2
/* [한국어] 기본 인바운드 창의 크기 256GB.
 * u64 캐스팅이 없으면 32비트 빌드에서 int 곱셈이 넘쳐 값이 망가진다. */
#define IB_WIN_SIZE			((u64)256 * 1024 * 1024 * 1024)
/* [한국어] DT 에 apio-wins / ppio-wins 속성이 없을 때 쓰는 기본 창 개수(8). */
#define MAX_PIO_WINDOWS			8

/* Parameters for the waiting for link up routine */
/* [한국어] 링크가 올라오기를 기다리는 최대 반복 횟수(10). */
#define LINK_WAIT_MAX_RETRIES		10
/* [한국어] 한 번 기다릴 때 usleep_range 에 넘기는 하한(90000us = 90ms). */
#define LINK_WAIT_MIN			90000
/* [한국어] 같은 호출의 상한(100000us = 100ms).
 * 최대 10회이므로 링크 대기는 길어야 1초 남짓이다. */
#define LINK_WAIT_MAX			100000

/* [한국어] 페이지 방식이 시작되는 경계 오프셋(0xc00).
 * 이 값 미만은 직접 접근, 이상은 페이지 전환을 거쳐야 하는 영역이다. */
#define PAGED_ADDR_BNDRY		0xc00
/* [한국어] 논리 오프셋을 '페이지 창 안의 실제 접근 주소'로 바꾼다 —
 * 하위 10비트만 남기고 그 위에 0xc00 을 얹는다. */
#define OFFSET_TO_PAGE_ADDR(off)	\
	((off & PAGE_LO_MASK) | PAGED_ADDR_BNDRY)
/* [한국어] 논리 오프셋에서 페이지 번호를 뽑는다 —
 * 10비트 오른쪽으로 민 뒤 6비트만 취한다. */
#define OFFSET_TO_PAGE_IDX(off)		\
	((off >> PAGE_SEL_OFFSET_SHIFT) & PAGE_SEL_MASK)

/* [한국어] MSI 관련 상태를 한데 모은 구조체.
 * struct mobiveil_root_port 안에 값으로 들어가며, 별도 할당하지 않는다.
 * host.c 의 통합 MSI 경로(mobiveil_pcie_integrated_interrupt_init)에서만 채워진다 —
 * gen4 처럼 외부 MSI 컨트롤러를 쓰는 경로에서는 비어 있는 채로 남는다. */
struct mobiveil_msi {			/* MSI information */
	/* [한국어] msi_irq_in_use 비트맵을 보호하는 뮤텍스.
	 * 설정자: mobiveil_allocate_msi_domains() 가 mutex_init 으로 초기화.
	 * 읽는 자: MSI 도메인의 alloc/free 콜백이 잡고 푼다.
	 * 값 범위: 일반 뮤텍스. 인터럽트 컨텍스트에서 잡지 않는다 —
	 * 실제로 ISR 은 이 락을 건드리지 않고 비트맵도 보지 않는다.
	 * 동기화: 벡터 할당과 해제가 동시에 일어나 같은 비트를 다투는 것을 막는다. */
	struct mutex lock;		/* protect bitmap variable */
	/* [한국어] 이 컨트롤러가 만든 MSI 부모 IRQ 도메인.
	 * 설정자: mobiveil_allocate_msi_domains() 의 msi_create_parent_irq_domain().
	 * 읽는 자: mobiveil_pcie_isr() 이 generic_handle_domain_irq 로 하드웨어에서
	 * 꺼낸 벡터 번호를 이 도메인에 넘겨 해당 핸들러를 부른다.
	 * 값 범위: 유효 포인터 또는 생성 실패 시 NULL(그 경우 probe 가 -ENOMEM 으로 끝난다).
	 * 동기화: 생성 이후에는 읽기 전용으로만 쓰인다. */
	struct irq_domain *dev_domain;
	/* [한국어] MSI 메시지가 향하는 물리 주소.
	 * 설정자: mobiveil_pcie_enable_msi() 가 pcie->pcie_reg_base 를 그대로 넣는다.
	 * 읽는 자: 이 트리에서 이 필드를 다시 읽는 코드는 찾지 못했다 —
	 * 실제 메시지 주소 계산은 mobiveil_compose_msi_msg() 가 pcie_reg_base 로 직접 한다.
	 * 값 범위: csr_axi_slave 리소스의 시작 물리 주소.
	 * 동기화: 초기화 때 한 번 쓰고 끝이라 보호가 필요 없다. */
	phys_addr_t msi_pages_phys;
	/* [한국어] 사용 가능한 MSI 벡터 수.
	 * 설정자: mobiveil_pcie_enable_msi() 가 PCI_NUM_MSI(16)로 설정.
	 * 읽는 자: mobiveil_allocate_msi_domains() 가 도메인 크기로,
	 * MSI alloc 콜백이 빈 비트를 찾는 상한으로 쓴다.
	 * 값 범위: 이 트리에서는 항상 16.
	 * 동기화: 초기화 순서가 중요하다 — enable_msi 가 반드시 도메인 생성보다
	 * 먼저 불려야 도메인 크기가 0 이 되지 않는다. */
	int num_of_vectors;
	/* [한국어] 어떤 MSI 벡터가 쓰이고 있는지 표시하는 비트맵(PCI_NUM_MSI 비트).
	 * 설정자: MSI alloc 콜백이 set_bit, free 콜백이 __clear_bit.
	 * 읽는 자: alloc 콜백의 find_first_zero_bit, free 콜백의 test_bit.
	 * 값 범위: 비트 0~15. 1이면 사용 중.
	 * 동기화: 위 lock 뮤텍스로 보호된다. */
	DECLARE_BITMAP(msi_irq_in_use, PCI_NUM_MSI);
};

/* [한국어] struct mobiveil_pcie 의 전방 선언.
 * 아래 두 ops 구조체가 이 타입의 포인터를 인자로 받는데, 정작
 * struct mobiveil_pcie 자신이 그 ops 를 필드로 가지므로 서로를 참조한다.
 * 그 순환을 끊으려면 이 전방 선언이 필요하다. */
struct mobiveil_pcie;

/* [한국어] 루트 포트 계층에서 SoC 마다 달라지는 동작을 담는 콜백 표.
 * 이 트리에서 이 표를 실제로 채우는 것은 pcie-layerscape-gen4.c 하나뿐이다. */
struct mobiveil_rp_ops {
	/* [한국어] 인터럽트 초기화를 SoC 구현으로 대체하는 콜백.
	 * 설정자: pcie-layerscape-gen4.c 의 ls_g4_pcie_rp_ops.
	 * 읽는 자: host.c 의 mobiveil_pcie_interrupt_init() — 이 포인터가 있으면
	 * 내장 INTx/MSI 초기화 대신 이것을 부른다.
	 * 값 범위: 유효 함수 포인터 또는 NULL(NULL 이면 내장 경로로 떨어진다).
	 * 동기화: probe 시점에 한 번 설정되고 이후 읽기 전용이다. */
	int (*interrupt_init)(struct mobiveil_pcie *pcie);
};

/* [한국어] 루트 포트 한 대에 딸린 상태 — 설정 공간 베이스, 인터럽트 도메인,
 * 브리지 핸들 등 '호스트 브리지로서의 역할'에 필요한 것들을 모은다.
 * struct mobiveil_pcie 안에 값으로 박혀 있다. */
struct mobiveil_root_port {
	/* [한국어] EP(다운스트림 장치)의 config 공간에 접근할 때 쓰는 AXI 창의 가상 주소.
	 * 설정자: mobiveil_pcie_parse_dt() 가 DT 의 'config_axi_slave' 리소스를 매핑.
	 * 읽는 자: mobiveil_pcie_map_bus() — 루트 버스가 아닌 접근이면 이 베이스에
	 * 레지스터 오프셋을 더해 돌려준다.
	 * 값 범위: 유효한 __iomem 포인터. 매핑 실패는 probe 단계에서 걸러진다.
	 * 동기화: 이 베이스로 실제 접근이 가리키는 대상은 PAB_AXI_AMAP_PEX_WIN_L 에
	 * 방금 쓴 BDF 에 달려 있다 — 그 직렬화는 PCI 코어의 config 접근 락에 기댄다
	 * (상류 주석이 'Relies on pci_lock serialization' 이라고 밝히고 있다). */
	void __iomem *config_axi_slave_base;	/* endpoint config base */
	/* [한국어] config 접근용 아웃바운드 창을 만들 때 쓴 리소스.
	 * 설정자: mobiveil_pcie_parse_dt() 가 'config_axi_slave' 리소스 포인터를 저장.
	 * 읽는 자: mobiveil_host_init() 이 0번 아웃바운드 창의 CPU 쪽 시작 주소와
	 * 크기를 여기서 가져온다.
	 * 값 범위: platform 리소스 배열 안을 가리키는 포인터 — 드라이버가 소유하지 않는다.
	 * 동기화: 초기화 때만 읽힌다. */
	struct resource *ob_io_res;
	/* [한국어] SoC 별 루트 포트 콜백 표.
	 * 설정자: pcie-layerscape-gen4.c 의 ls_g4_pcie_probe() 한 곳뿐이다.
	 * 읽는 자: host.c 의 mobiveil_pcie_interrupt_init().
	 * 값 범위: 유효 포인터, 또는 설정하지 않은 드라이버에서는 NULL.
	 * 동기화: probe 이후 읽기 전용.
	 * 주의: 읽는 쪽이 rp->ops->interrupt_init 을 ops 자체의 NULL 검사 없이
	 * 역참조한다. pcie-mobiveil-plat.c 는 이 필드를 채우지 않으므로
	 * 그 경로에서는 NULL 을 역참조하게 된다 — 자세한 것은 host.c 쪽 주석 참조. */
	const struct mobiveil_rp_ops *ops;
	/* [한국어] 이 루트 포트가 쓰는 Linux IRQ 번호(virq).
	 * 설정자: 통합 경로에서는 mobiveil_pcie_integrated_interrupt_init() 이
	 * platform_get_irq(pdev, 0) 로 얻는다.
	 * 읽는 자: 같은 함수가 irq_set_chained_handler_and_data 로 체인 핸들러를 건다.
	 * 값 범위: 양수 virq. 음수는 오류라 그 자리에서 반환된다.
	 * 동기화: 초기화 때만 쓰인다.
	 * 주의: gen4 는 자기 struct ls_g4_pcie 안에 별도의 irq 필드를 두고
	 * 이 필드는 쓰지 않는다. */
	int irq;
	/* [한국어] INTx 마스크 레지스터(PAB_INTP_AMBA_MISC_ENB) 읽고-고치고-쓰기를 보호하는 락.
	 * 설정자: mobiveil_pcie_init_irq_domain() 의 raw_spin_lock_init.
	 * 읽는 자: mobiveil_mask_intx_irq() / mobiveil_unmask_intx_irq().
	 * 값 범위: raw spinlock — irqsave 판으로 잡는다.
	 * 동기화: 마스크 갱신이 read-modify-write 라, 두 INTx 핀을 동시에 조작하면
	 * 한쪽 갱신이 사라질 수 있다. 그것을 막는 것이 이 락의 목적이다.
	 * 다만 이 락은 ENB 레지스터 접근만 보호할 뿐, 페이지 선택과 실제 접근
	 * 사이의 경쟁까지 막지는 못한다(pcie-mobiveil.c 주석 참조). */
	raw_spinlock_t intx_mask_lock;
	/* [한국어] INTx 네 핀을 위한 IRQ 도메인.
	 * 설정자: mobiveil_pcie_init_irq_domain() 의 irq_domain_create_linear(크기 PCI_NUM_INTX).
	 * 읽는 자: mobiveil_pcie_isr() 이 generic_handle_domain_irq 로 hwirq 를 넘긴다.
	 * 값 범위: 유효 포인터 또는 생성 실패 시 NULL(그 경우 probe 가 -ENOMEM).
	 * 동기화: 생성 이후 읽기 전용. */
	struct irq_domain *intx_domain;
	/* [한국어] MSI 관련 상태 묶음(위 struct mobiveil_msi).
	 * 포인터가 아니라 값이므로 루트 포트와 수명이 같다.
	 * 채워지는 것은 통합 인터럽트 경로에서뿐이다. */
	struct mobiveil_msi msi;
	/* [한국어] 이 컨트롤러의 PCI 호스트 브리지 객체.
	 * 설정자: 각 SoC 드라이버의 probe 가 devm_pci_alloc_host_bridge() 결과를 넣는다
	 * (gen4 와 plat 양쪽 모두).
	 * 읽는 자: mobiveil_host_init() 이 bridge->windows 로 DT 의 ranges 를 훑고,
	 * mobiveil_pcie_host_probe() 가 sysdata/ops 를 채운 뒤 pci_host_probe() 에 넘긴다.
	 * 값 범위: 유효 포인터 — 이 필드가 비어 있으면 호스트 초기화가 성립하지 않는다.
	 * 동기화: probe 컨텍스트에서만 다뤄진다. */
	struct pci_host_bridge *bridge;
};

/* [한국어] PAB 계층에서 SoC 마다 달라지는 동작을 담는 콜백 표.
 * 위 mobiveil_rp_ops 가 루트 포트 계층이라면 이쪽은 그보다 아래,
 * 레지스터 계층에 해당한다. */
struct mobiveil_pab_ops {
	/* [한국어] 링크 상태 판정을 SoC 구현으로 대체하는 콜백.
	 * 설정자: pcie-layerscape-gen4.c 의 ls_g4_pcie_pab_ops.
	 * 읽는 자: pcie-mobiveil.c 의 mobiveil_pcie_link_up() — 이 포인터가 있으면
	 * 공통 LTSSM_STATUS 판정 대신 이것을 부른다.
	 * 값 범위: 유효 함수 포인터 또는 NULL.
	 * 동기화: probe 이후 읽기 전용.
	 * 주의: 읽는 쪽이 pcie->ops->link_up 을 ops 자체의 NULL 검사 없이 역참조한다. */
	bool (*link_up)(struct mobiveil_pcie *pcie);
};

/* [한국어] 이 IP 를 쓰는 컨트롤러 한 대를 나타내는 최상위 구조체.
 * SoC 별 드라이버는 이것을 자기 구조체 첫 필드로 감싸(gen4 의 struct ls_g4_pcie)
 * container_of 나 drvdata 로 오간다.
 * 실체는 devm_pci_alloc_host_bridge() 가 잡아 준 브리지 private 영역에 있다. */
struct mobiveil_pcie {
	/* [한국어] 이 컨트롤러를 만든 platform 장치.
	 * 설정자: 각 SoC 드라이버의 probe.
	 * 읽는 자: DT 파싱, 리소스 획득, dev_err 계열 로그가 모두 pdev->dev 를 쓴다.
	 * 값 범위: 유효 포인터.
	 * 동기화: 드라이버 수명 동안 바뀌지 않는다. */
	struct platform_device *pdev;
	/* [한국어] 루트 컴플렉스 자신의 레지스터(config 헤더 + PAB 레지스터) 가상 주소.
	 * 설정자: mobiveil_pcie_parse_dt() 가 DT 의 'csr_axi_slave' 리소스를 매핑.
	 * 읽는 자: 모든 mobiveil_csr_read/write 접근의 기준점이며,
	 * gen4 는 여기에 PCIE_PF_OFF 를 더해 자기 PF 레지스터에 접근한다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 이 베이스를 통한 접근은 페이지 선택과 짝을 이루는데,
	 * 그 둘 사이가 원자적이지 않다는 문제가 pcie-mobiveil.c 에 남아 있다. */
	void __iomem *csr_axi_slave_base;	/* root port config base */
	/* [한국어] MSI 전용 레지스터 블록의 가상 주소.
	 * 설정자: mobiveil_pcie_integrated_interrupt_init() 이 DT 의 'apb_csr'
	 * 리소스를 매핑 — 즉 통합 MSI 경로에서만 채워진다.
	 * 읽는 자: mobiveil_pcie_enable_msi() 와 mobiveil_pcie_isr() 이
	 * MSI_ 로 시작하는 오프셋들을 여기에 더해 접근한다.
	 * 값 범위: 유효한 __iomem 포인터, 또는 통합 경로를 타지 않았다면 NULL.
	 * 동기화: gen4 는 이 필드를 채우지 않지만 자기 ISR 도 이 필드를 쓰지 않으므로
	 * 서로 마주치지 않는다. */
	void __iomem *apb_csr_base;	/* MSI register base */
	/* [한국어] csr_axi_slave 영역의 물리 주소.
	 * 설정자: mobiveil_pcie_parse_dt() 가 리소스의 start 를 그대로 저장.
	 * 읽는 자: mobiveil_pcie_enable_msi() 가 MSI 창의 기준 주소로,
	 * mobiveil_compose_msi_msg() 가 벡터별 메시지 주소의 기준으로 쓴다.
	 * 값 범위: 물리 주소. 가상 주소가 아니라는 점이 중요하다 —
	 * MSI 는 장치가 버스 주소로 써야 하는 값이기 때문이다.
	 * 동기화: 초기화 때 한 번 쓰고 이후 읽기 전용. */
	phys_addr_t pcie_reg_base;	/* Physical PCIe Controller Base */
	/* [한국어] 이 컨트롤러가 가진 아웃바운드(AXI PIO) 창의 개수.
	 * 설정자: mobiveil_pcie_parse_dt() — DT 의 'apio-wins', 없으면 MAX_PIO_WINDOWS(8).
	 * 읽는 자: program_ob_windows() 가 요청한 창 번호가 이 한도를 넘는지 검사.
	 * 값 범위: 보통 1~8.
	 * 동기화: 초기화 때만 쓰인다. */
	int apio_wins;
	/* [한국어] 인바운드(PEX PIO) 창의 개수. 위와 같은 방식으로 정해지며
	 * program_ib_windows() 의 한도 검사에 쓰인다. */
	int ppio_wins;
	/* [한국어] 지금까지 설정한 아웃바운드 창의 수 — 다음에 쓸 창 번호 역할도 겸한다.
	 * 설정자: mobiveil_host_init() 이 0 으로 초기화하고,
	 * program_ob_windows() 가 창 하나를 잡을 때마다 증가시킨다.
	 * 읽는 자: mobiveil_host_init() 이 DT 의 ranges 를 훑으며 이 값을
	 * 다음 창 번호로 넘긴다.
	 * 값 범위: 0 이상 apio_wins 이하.
	 * 동기화: 단일 스레드 초기화 경로에서만 다뤄진다.
	 * 재초기화(gen4 의 리셋 복구) 때 다시 0 으로 돌아간다. */
	int ob_wins_configured;		/* configured outbound windows */
	/* [한국어] 지금까지 설정한 인바운드 창의 수. 위와 같은 방식으로 관리된다.
	 * 다만 이 드라이버는 인바운드 창을 0번 하나만 잡는다. */
	int ib_wins_configured;		/* configured inbound windows */
	/* [한국어] SoC 별 PAB 계층 콜백 표(위 struct mobiveil_pab_ops).
	 * 설정자: pcie-layerscape-gen4.c 의 ls_g4_pcie_probe() 한 곳뿐이다.
	 * 읽는 자: pcie-mobiveil.c 의 mobiveil_pcie_link_up().
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: probe 이후 읽기 전용. */
	const struct mobiveil_pab_ops *ops;
	/* [한국어] 이 컨트롤러의 루트 포트 상태(위 struct mobiveil_root_port).
	 * 포인터가 아니라 값이라 별도 할당·해제가 없고 수명이 컨트롤러와 같다. */
	struct mobiveil_root_port rp;
};

/* [한국어] mobiveil_pcie_host_probe - SoC 드라이버가 공통 호스트 초기화를 위임하는 진입점.
 * @pcie: SoC 드라이버가 pdev / rp.bridge (그리고 쓸 경우 ops) 를 채워 넘긴 컨트롤러.
 * @return: 0 성공, 음수 errno 실패.
 * 구현: pcie-mobiveil-host.c. 호출자: pcie-layerscape-gen4.c 의 ls_g4_pcie_probe(),
 * pcie-mobiveil-plat.c 의 mobiveil_pcie_probe(). */
int mobiveil_pcie_host_probe(struct mobiveil_pcie *pcie);
/* [한국어] mobiveil_host_init - 창·PIO·클래스 코드 등 호스트 하드웨어를 세운다.
 * @pcie: 대상 컨트롤러.
 * @reinit: true 면 버스 번호 재설정을 건너뛴다 — 리셋 복구용 재진입 경로.
 * @return: 이 트리 구현은 언제나 0.
 * 구현: pcie-mobiveil-host.c. 호출자: 같은 파일의 mobiveil_pcie_host_probe()(reinit=false)와
 * pcie-layerscape-gen4.c 의 ls_g4_pcie_reinit_hw()(reinit=true). */
int mobiveil_host_init(struct mobiveil_pcie *pcie, bool reinit);
/* [한국어] mobiveil_pcie_link_up - 링크가 올라왔는지 판정한다.
 * @pcie: 대상 컨트롤러.
 * @return: true 면 링크 상승.
 * 구현: pcie-mobiveil.c. SoC 콜백(ops->link_up)이 있으면 그것을 우선한다. */
bool mobiveil_pcie_link_up(struct mobiveil_pcie *pcie);
/* [한국어] mobiveil_bringup_link - 링크가 올라올 때까지 기다린다.
 * @pcie: 대상 컨트롤러.
 * @return: 0 이면 링크 상승, 음수면 시간 초과.
 * 구현: pcie-mobiveil.c. 호출자: mobiveil_pcie_host_probe(). */
int mobiveil_bringup_link(struct mobiveil_pcie *pcie);
/* [한국어] program_ob_windows - 아웃바운드(CPU → PCIe) 주소 창 하나를 설정한다.
 * @pcie: 대상 컨트롤러.  @win_num: 창 번호(apio_wins 미만이어야 한다).
 * @cpu_addr: AXI 쪽 시작 주소.  @pci_addr: PCIe 쪽 목적지 주소.
 * @type: CFG/IO/MEM 중 하나.  @size: 창 크기(마스크로 변환돼 기록된다).
 * @return: 없음 — 창 번호가 한도를 넘으면 로그만 남기고 조용히 돌아간다.
 * 구현: pcie-mobiveil.c. 호출자: mobiveil_host_init(). */
void program_ob_windows(struct mobiveil_pcie *pcie, int win_num, u64 cpu_addr,
			u64 pci_addr, u32 type, u64 size);
/* [한국어] program_ib_windows - 인바운드(PCIe → 시스템 메모리) 주소 창 하나를 설정한다.
 * @pcie: 대상 컨트롤러.  @win_num: 창 번호(ppio_wins 미만).
 * @cpu_addr: AXI 쪽 목적지 주소.  @pci_addr: PCIe 쪽 시작 주소.
 * @type: 창 종류.  @size: 창 크기.
 * @return: 없음.
 * 구현: pcie-mobiveil.c. 호출자: mobiveil_host_init() 이 0번 창 하나만 잡는다. */
void program_ib_windows(struct mobiveil_pcie *pcie, int win_num, u64 cpu_addr,
			u64 pci_addr, u32 type, u64 size);
/* [한국어] mobiveil_csr_read - 논리 오프셋으로 컨트롤러 레지스터를 읽는다.
 * @pcie: 대상 컨트롤러.  @off: 논리 오프셋(0xc00 이상이면 페이지 전환이 끼어든다).
 * @size: 1/2/4 바이트.
 * @return: 읽은 값. 정렬이 어긋나면 0 을 돌려주고 오류 로그를 남긴다.
 * 구현: pcie-mobiveil.c. 아래 readl/readw/readb 래퍼가 이것을 감싼다. */
u32 mobiveil_csr_read(struct mobiveil_pcie *pcie, u32 off, size_t size);
/* [한국어] mobiveil_csr_write - 논리 오프셋으로 컨트롤러 레지스터에 쓴다.
 * @pcie: 대상 컨트롤러.  @val: 쓸 값.  @off: 논리 오프셋.  @size: 1/2/4 바이트.
 * @return: 없음.
 * 구현: pcie-mobiveil.c. 아래 writel/writew/writeb 래퍼가 이것을 감싼다. */
void mobiveil_csr_write(struct mobiveil_pcie *pcie, u32 val, u32 off,
			size_t size);

/* [한국어] mobiveil_csr_readl - 32비트 레지스터 읽기 래퍼.
 * @pcie: 대상 컨트롤러.  @off: 논리 오프셋.
 * @return: 읽은 32비트 값.
 * 크기 인자를 매번 적지 않게 해 주는 인라인 래퍼다. 이 IP 의 레지스터는
 * 대부분 32비트라 mobiveil 디렉터리에서 가장 많이 쓰이는 접근자이기도 하다.
 * 실행 컨텍스트: 호출자를 따른다 — 초기화 경로와 ISR 양쪽에서 불린다.
 * 호출자: host.c, gen4.c, pcie-mobiveil.c 전반.
 * 에러 경로: 하위 mobiveil_csr_read() 가 정렬 오류를 걸러 0 을 돌려준다.
 * 호출 체인:
 *   각 드라이버 → [이 함수] → mobiveil_csr_read() → mobiveil_pcie_read() */
static inline u32 mobiveil_csr_readl(struct mobiveil_pcie *pcie, u32 off)
{
	return mobiveil_csr_read(pcie, off, 0x4);
}

/* [한국어] mobiveil_csr_readw - 16비트 레지스터 읽기 래퍼.
 * @pcie: 대상 컨트롤러.  @off: 논리 오프셋(2바이트 정렬이어야 한다).
 * @return: 읽은 16비트 값.
 * config 헤더의 16비트 필드를 다룰 때 쓴다.
 * 실행 컨텍스트: 호출자를 따른다.
 * 호출자: 이 트리에서는 pcie-layerscape-gen4.c 의 ls_g4_pcie_reset() 하나뿐 —
 * PCI_BRIDGE_CONTROL 을 읽어 버스 리셋 비트를 내릴 때 쓴다.
 * 에러 경로: 정렬이 어긋나면 하위 함수가 0 을 돌려준다.
 * 호출 체인:
 *   ls_g4_pcie_reset() → [이 함수] → mobiveil_csr_read() */
static inline u16 mobiveil_csr_readw(struct mobiveil_pcie *pcie, u32 off)
{
	return mobiveil_csr_read(pcie, off, 0x2);
}

/* [한국어] mobiveil_csr_readb - 8비트 레지스터 읽기 래퍼.
 * @pcie: 대상 컨트롤러.  @off: 논리 오프셋(정렬 제약 없음).
 * @return: 읽은 8비트 값.
 * 실행 컨텍스트: 호출자를 따른다.
 * 호출자: 이 트리에서는 host.c 의 mobiveil_pcie_is_bridge() 하나뿐 —
 * PCI_HEADER_TYPE 을 읽어 이 장치가 브리지 헤더인지 확인한다.
 * 에러 경로: 1바이트 접근은 정렬 검사에 걸리지 않는다.
 * 호출 체인:
 *   mobiveil_pcie_is_bridge() → [이 함수] → mobiveil_csr_read() */
static inline u8 mobiveil_csr_readb(struct mobiveil_pcie *pcie, u32 off)
{
	return mobiveil_csr_read(pcie, off, 0x1);
}


/* [한국어] mobiveil_csr_writel - 32비트 레지스터 쓰기 래퍼.
 * @pcie: 대상 컨트롤러.  @val: 쓸 값.  @off: 논리 오프셋.
 * @return: 없음.
 * 이 디렉터리에서 가장 많이 쓰이는 쓰기 접근자다. PAB 레지스터 설정,
 * 인터럽트 마스크 갱신, 창 설정이 모두 이것을 지난다.
 * 실행 컨텍스트: 초기화 경로와 ISR 양쪽. ISR 에서도 상태 레지스터를 지우려고 부른다.
 * 호출자: host.c, gen4.c, pcie-mobiveil.c 전반.
 * 에러 경로: 정렬 오류는 하위 함수가 로그만 남기고 쓰기를 건너뛴다.
 * 호출 체인:
 *   각 드라이버 → [이 함수] → mobiveil_csr_write() → mobiveil_pcie_write() */
static inline void mobiveil_csr_writel(struct mobiveil_pcie *pcie, u32 val,
				       u32 off)
{
	mobiveil_csr_write(pcie, val, off, 0x4);
}

/* [한국어] mobiveil_csr_writew - 16비트 레지스터 쓰기 래퍼.
 * @pcie: 대상 컨트롤러.  @val: 쓸 16비트 값.  @off: 논리 오프셋(2바이트 정렬).
 * @return: 없음.
 * 실행 컨텍스트: 호출자를 따른다.
 * 호출자: 이 트리에서는 pcie-layerscape-gen4.c 의 ls_g4_pcie_reset() 하나뿐 —
 * 버스 리셋 비트를 내린 PCI_BRIDGE_CONTROL 값을 되쓴다.
 * 에러 경로: 정렬이 어긋나면 하위 함수가 쓰기를 건너뛴다.
 * 호출 체인:
 *   ls_g4_pcie_reset() → [이 함수] → mobiveil_csr_write() */
static inline void mobiveil_csr_writew(struct mobiveil_pcie *pcie, u16 val,
				       u32 off)
{
	mobiveil_csr_write(pcie, val, off, 0x2);
}

/* [한국어] mobiveil_csr_writeb - 8비트 레지스터 쓰기 래퍼.
 * @pcie: 대상 컨트롤러.  @val: 쓸 8비트 값.  @off: 논리 오프셋.
 * @return: 없음.
 * 대칭성을 맞추려고 둔 래퍼로 보인다 — 읽기 세 종류와 쓰기 세 종류가 짝을 이룬다.
 * 실행 컨텍스트: 해당 없음.
 * 호출자: 이 트리 안에서 이 함수를 부르는 곳은 찾지 못했다(정의만 있다).
 * 에러 경로: 1바이트 접근은 정렬 검사에 걸리지 않는다.
 * 호출 체인:
 *   (호출자 없음) → [이 함수] → mobiveil_csr_write() */
static inline void mobiveil_csr_writeb(struct mobiveil_pcie *pcie, u8 val,
				       u32 off)
{
	mobiveil_csr_write(pcie, val, off, 0x1);
}

/* [한국어] 헤더 중복 포함 방지 가드의 끝. */
#endif /* _PCIE_MOBIVEIL_H */
