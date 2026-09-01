/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2023, Xilinx, Inc.
 */

/*
 * [한국어 설명] Xilinx PCIe 컨트롤러 공용 인터럽트 비트 번호 (pcie-xilinx-common.h)
 *
 * === 파일의 역할 ===
 * Xilinx 계열 PCIe 컨트롤러 드라이버들이 공유하는 인터럽트 **비트 번호**를
 * 정의한 헤더다. 실행 코드도 구조체도 없고 상수 21개가 전부다.
 * 값들이 BIT(n) 이 아니라 **번호 n 자체**인 점이 이 파일의 핵심이다.
 * 그래서 쓰는 쪽이 필요에 따라 BIT() 로 감싸거나 그대로 hwirq 번호로
 * 쓸 수 있다 -- 실제로 pcie-xilinx-cpm.c 는 IRQ 도메인의 hwirq 로,
 * pcie-xilinx-dma-pl.c 는 마스크 계산에 쓴다.
 * 같은 IP 계열이라도 판본마다 지원하는 인터럽트가 달라, 이 표에는 어느
 * 드라이버도 쓰지 않는 항목이 몇 개 섞여 있다(아래 각 상수 주석 참조).
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 컨트롤러 드라이버 계층의 Xilinx 계열 공용 정의다. 이 헤더를 포함하는
 * 것은 pcie-xilinx-cpm.c 와 pcie-xilinx-dma-pl.c 둘뿐이다.
 * (같은 디렉터리의 pcie-xilinx.c 는 이 헤더를 쓰지 않고 자기 안에서
 *  같은 이름의 상수를 BIT() 형태로 따로 정의한다 -- 이름이 겹치지만 값의
 *  형태가 다르므로 혼동하지 않도록 주의해야 한다.)
 * 실행 컨텍스트는 없다 -- 전처리 단계에서만 존재하는 파일이다.
 *
 * === 타 모듈과의 연결 ===
 * 세 헤더를 포함한다: <linux/pci.h>, <linux/pci-ecam.h>,
 * <linux/platform_device.h>. 다만 이 파일 자체는 그 헤더들의 어떤 타입도
 * 쓰지 않는다 -- 상수 정의뿐이기 때문이다. 이 헤더를 포함하는 두 .c 파일이
 * 그 타입들을 필요로 해, 편의상 여기에 모아 둔 형태로 보인다.
 * 데이터 흐름: 컨트롤러의 인터럽트 상태 레지스터에서 읽은 비트를 이
 * 번호들과 견주어 어떤 사건인지 가리고, INTx 와 MSI 는 각각 하위 도메인으로
 * 넘긴다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수도 구조체도 없다. 상수 21개가 성격별로 나뉜다.
 *  - 링크 사건: LINK_DOWN(0), HOT_RESET(3)
 *  - 설정 트랜잭션 오류: CFG_PCIE_TIMEOUT(4), CFG_TIMEOUT(8),
 *    CFG_ERR_POISON(12)
 *  - AER 등급별 오류: CORRECTABLE(9), NONFATAL(10), FATAL(11)
 *  - 전원 관리: PME_TO_ACK_RCVD(15), PM_PME_RCVD(17)
 *  - 인터럽트 전달: INTX(16), MSI(17)
 *  - AXI 슬레이브/마스터 오류: SLV_*(20~25, 28), MST_*(26~27)
 * **PM_PME_RCVD 와 MSI 가 둘 다 17 인 점**에 유의 -- 같은 비트를 판본에
 * 따라 다른 뜻으로 쓴다는 뜻이고, 그래서 두 드라이버 중 하나만 그 비트를
 * MSI 로 해석한다.
 */

/* [한국어] 아래 세 헤더는 이 파일 자체가 쓰는 타입이 없다 -- 상수 정의뿐이기
 * 때문이다. 이 헤더를 포함하는 두 .c 파일이 필요로 하는 것을 편의상 여기
 * 모아 둔 형태로 보인다.
 * <linux/pci.h>: PCI 공통 타입과 상수. */
#include <linux/pci.h>
/* [한국어] ECAM 접근 관련 정의. pcie-xilinx-cpm.c 가 ECAM 창을 쓴다. */
#include <linux/pci-ecam.h>
/* [한국어] 두 드라이버 모두 플랫폼 드라이버로 등록되므로 필요하다. */
#include <linux/platform_device.h>

/* Interrupt registers definitions */
/* [한국어] 링크가 끊어졌다. 비트 0 -- 가장 중요한 사건이라 맨 앞자리를 차지한다. */
#define XILINX_PCIE_INTR_LINK_DOWN		0
/* [한국어] 호스트가 핫리셋을 걸었다. */
#define XILINX_PCIE_INTR_HOT_RESET		3
/* [한국어] 설정 트랜잭션이 PCIe 쪽에서 시간 초과됐다.
 * **이 트리의 어느 드라이버도 참조하지 않는다** -- 판본에 따라 존재하는
 * 사건을 표에만 적어 둔 것으로 보인다. */
#define XILINX_PCIE_INTR_CFG_PCIE_TIMEOUT	4
/* [한국어] 설정 트랜잭션이 시간 초과됐다. 위 CFG_PCIE_TIMEOUT 과 달리 두
 * 드라이버가 모두 쓴다. */
#define XILINX_PCIE_INTR_CFG_TIMEOUT		8
/* [한국어] AER 의 correctable 오류를 받았다. 링크가 스스로 복구한 오류라
 * 보고만 하고 넘어간다. */
#define XILINX_PCIE_INTR_CORRECTABLE		9
/* [한국어] AER 의 non-fatal 오류. 해당 트랜잭션은 실패했지만 링크는 살아 있다. */
#define XILINX_PCIE_INTR_NONFATAL		10
/* [한국어] AER 의 fatal 오류. 링크 자체가 신뢰할 수 없는 상태다. */
#define XILINX_PCIE_INTR_FATAL			11
/* [한국어] 설정 트랜잭션에 오염(poisoned) 표시가 붙어 왔다.
 * **이 트리의 어느 드라이버도 참조하지 않는다.** */
#define XILINX_PCIE_INTR_CFG_ERR_POISON		12
/* [한국어] PME_TO_Ack 를 받았다 -- 서스펜드 절차에서 하위 장치가 응답했다는 뜻이다.
 * **이 트리의 어느 드라이버도 참조하지 않는다.** */
#define XILINX_PCIE_INTR_PME_TO_ACK_RCVD	15
/* [한국어] 레거시 INTx 인터럽트가 도착했다. 컨트롤러가 이 비트로 알리면 드라이버가
 * INTx 하위 도메인으로 넘긴다. */
#define XILINX_PCIE_INTR_INTX			16
/* [한국어] PM_PME 메시지를 받았다 -- 하위 장치가 절전 상태에서 깨어나고 싶다는
 * 신호다. **값이 아래 MSI 와 같은 17 이고, 이 트리의 어느 드라이버도
 * 이 이름으로는 참조하지 않는다.** */
#define XILINX_PCIE_INTR_PM_PME_RCVD		17
/* [한국어] MSI 가 도착했다. **위 PM_PME_RCVD 와 같은 비트 17 이다** -- 같은 비트를
 * 판본에 따라 다른 뜻으로 쓴다는 뜻이고, 실제로 이 이름을 쓰는 드라이버와
 * 저 이름을 쓰는 드라이버가 갈린다. */
#define XILINX_PCIE_INTR_MSI			17
/* [한국어] AXI 슬레이브 쪽에서 지원하지 않는 요청을 받았다. 아래 SLV_* 는 모두
 * 컨트롤러가 AXI 버스의 슬레이브로 동작할 때 나는 오류다. */
#define XILINX_PCIE_INTR_SLV_UNSUPP		20
/* [한국어] 예상하지 못한 완료(completion)를 받았다. */
#define XILINX_PCIE_INTR_SLV_UNEXP		21
/* [한국어] 완료 처리 중 오류가 났다. */
#define XILINX_PCIE_INTR_SLV_COMPL		22
/* [한국어] 오류 응답(error poison)을 받았다. */
#define XILINX_PCIE_INTR_SLV_ERRP		23
/* [한국어] 완료자 중단(Completer Abort)을 받았다. */
#define XILINX_PCIE_INTR_SLV_CMPABT		24
/* [한국어] 허용되지 않는 버스트 형태의 요청을 받았다. */
#define XILINX_PCIE_INTR_SLV_ILLBUR		25
/* [한국어] AXI **마스터** 쪽에서 디코드 오류가 났다 -- 주소에 해당하는 대상이 없다. */
#define XILINX_PCIE_INTR_MST_DECERR		26
/* [한국어] AXI 마스터 쪽에서 슬레이브 오류 응답을 받았다. */
#define XILINX_PCIE_INTR_MST_SLVERR		27
/* [한국어] 슬레이브 쪽 PCIe 트랜잭션이 시간 초과됐다.
 * **이 트리의 어느 드라이버도 참조하지 않는다.** */
#define XILINX_PCIE_INTR_SLV_PCIE_TIMEOUT	28
