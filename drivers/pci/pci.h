/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] PCI 코어 내부 전용 공용 헤더 (drivers/pci/pci.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 PCI 서브시스템 "내부"에서만 쓰이는 선언을 모아 놓은 비공개
 * 헤더다. 외부 드라이버가 쓰는 공개 API 는 include/linux/pci.h 에 있고,
 * 이 파일은 drivers/pci/ 아래 구현 파일들끼리만 공유하는 함수 프로토타입,
 * 내부 자료구조(struct pci_sriov, struct aer_err_info, struct
 * pci_cap_saved_state 등), 내부 전역 락, 그리고 PCI/PCIe 스펙 상수를
 * 담는다. 이 트리에서는 117개 파일이 이 헤더를 #include 한다(경로는
 * "pci.h" 또는 "../pci.h").
 * 이 헤더의 또 다른 역할은 CONFIG_* 로 켜고 끌 수 있는 기능(SR-IOV, AER,
 * DPC, ASPM, ACPI, OF, CardBus, DOE, IDE, TSM, PTM, TPH ...)마다 "기능이
 * 켜진 경우의 프로토타입"과 "꺼진 경우의 빈 스텁 static inline"을 짝으로
 * 제공하는 것이다. 덕분에 호출부는 #ifdef 없이 그냥 호출만 하면 되고,
 * 기능이 꺼져 있으면 컴파일러가 빈 함수를 인라인해 코드가 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 코어의 실행 흐름은 크게 (1) 열거(probe.c) → (2) 리소스 할당
 * (setup-bus.c, setup-res.c) → (3) 드라이버 바인딩(pci-driver.c) →
 * (4) 런타임 서비스(전원관리 pci.c/pci-acpi.c, 인터럽트 msi/, 오류복구
 * pcie/err.c·aer.c·dpc.c) → (5) 제거(remove.c) 순이다. 이 헤더는 그
 * 다섯 단계 전부가 서로를 부르기 위해 통과하는 "내부 ABI 선언표"다.
 * 실행 컨텍스트는 전부 커널 공간이며, 여기 선언된 함수 대부분은 프로세스
 * 컨텍스트(장치 열거/probe/sysfs write)에서 실행된다. 예외적으로
 * pci_dev_set_io_state()·pci_dev_set_disconnected() 계열은 AER/DPC
 * 인터럽트 후속 워크큐 컨텍스트에서도 불린다.
 * 이 헤더 자체에는 실행 코드가 거의 없고, 있는 것은 전부 static inline
 * 헬퍼와 문장식(statement-expression) 매크로다. 즉 "호출되는 곳에
 * 인라인 전개되는 판정 로직"이 이 파일의 실질적 코드다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 include/linux/pci.h(공개 API)와 include/uapi/linux/pci_regs.h
 * (스펙 레지스터 오프셋 정의)에 의존한다. 이 파일은 <linux/pci.h> 를
 * 직접 #include 하므로, pci_dev·pci_bus·pci_driver 같은 핵심 타입은 이미
 * 보인다는 전제로 쓰여 있다.
 * 아래쪽으로는 drivers/pci/ 의 거의 모든 .c 가 이 헤더를 포함한다:
 *   probe.c(열거·BAR 크기결정), setup-bus.c/setup-res.c(브리지 윈도우 할당),
 *   pci.c(전원상태·리셋·capability 탐색), access.c(config space 접근 락),
 *   iov.c(SR-IOV), msi/(MSI·MSI-X), pcie/aer.c·dpc.c·err.c(오류 복구),
 *   pcie/aspm.c(링크 전력 절감), pci-acpi.c(ACPI _PSx 연동), of.c(디바이스트리),
 *   quirks.c(스펙 위반 하드웨어 우회), 그리고 controller/ 아래 각 SoC 호스트
 *   브리지 드라이버들.
 * 데이터 흐름: config space 에서 읽은 원시 값(capability 위치, BAR 크기,
 * 링크 속도)이 이 헤더가 선언한 함수를 통해 struct pci_dev 의 필드와
 * resource[] 배열로 정착하고, 그 뒤 드라이버(예: NVMe)가 공개 API 로
 * 그 결과만 읽어 간다.
 * NVMe 와의 관계: drivers/nvme/host/pci.c 는 이 헤더를 포함하지 않는다
 * (<linux/pci.h> 만 포함한다 — drivers/nvme/host/pci.c:22 에서 확인).
 * 그러나 NVMe 가 보는 값의 "생산자"가 여기 선언된 함수들이다. 예를 들어
 * NVMe 는 drivers/nvme/host/pci.c:3001 에서
 * ioremap(pci_resource_start(pdev, 0), size) 로 BAR0 를 매핑하고
 * dev->dbs = dev->bar + NVME_REG_DBS 로 도어벨 창을 잡는데, 그
 * pci_resource_start(pdev, 0) 이 돌려주는 resource[0] 을 채우는 것이
 * 이 헤더가 선언한 __pci_read_base()/__pci_size_stdbars() 다.
 *
 * === 주요 함수/구조체 요약 ===
 * - PCI_FIND_NEXT_CAP / PCI_FIND_NEXT_EXT_CAP: config space 의
 *   capability 연결 리스트를 TTL 을 걸고 순회하는 매크로. 표준 capability 는
 *   0x34(Capabilities Pointer)에서 시작하는 8비트 링크, 확장 capability 는
 *   0x100 에서 시작하는 12비트 링크다.
 * - struct pci_sriov: SR-IOV PF 한 개의 상태 전부(VF 개수, Routing ID
 *   offset/stride, VF BAR 크기 등). drivers/pci/iov.c 가 유일한 관리자다.
 * - struct aer_err_info: AER 인터럽트 한 번에서 긁어 모은 오류 정보
 *   (오류 장치 목록, 심각도, 상태 비트, TLP 헤더 로그).
 * - struct pci_cap_saved_state/-_data: suspend 전 capability 레지스터를
 *   통째로 떠 두는 가변 길이 버퍼. pci_save_state() 경로가 쓴다.
 * - pci_dev_set_io_state() / pci_dev_set_disconnected(): 장치의
 *   error_state 를 원자적으로 전이시키는 상태 기계. NVMe 의
 *   pci_dev_is_disconnected() 판정이 읽는 바로 그 값이다.
 * - pci_power_manageable() / pci_no_d1d2() / PCI_PM_D*_WAIT: D0~D3cold
 *   전원 상태 전이 가능 여부와 전이 후 대기 시간을 결정한다.
 * - pci_resource_num() / pci_resource_is_iov() / pci_resource_is_bridge_win()
 *   / pci_resource_alignment(): pci_dev.resource[] 배열 인덱스를 해석하는
 *   헬퍼 묶음. BAR·ROM·VF BAR·브리지 윈도우가 한 배열에 섞여 있어 필요하다.
 */
#ifndef DRIVERS_PCI_H /* [한국어] 헤더 중복 포함 방지 가드 시작 — 117개 .c 가 이 파일을 포함하므로 필수 */
#define DRIVERS_PCI_H /* [한국어] 가드 매크로 정의. 이후 재포함 시 본문 전체가 건너뛰어진다 */

#include <linux/align.h> /* [한국어] <linux/align.h> — 아래 PCI_FIND_NEXT_CAP 이 쓰는 ALIGN_DOWN() 제공. capability 포인터를 DWORD(4바이트) 경계로 내림 정렬할 때 필요 */
#include <linux/bitfield.h> /* [한국어] <linux/bitfield.h> — FIELD_GET() 제공. capability 헤더에서 ID/next 필드를, LNKSTA 에서 링크 폭 필드를 마스크+시프트 없이 뽑을 때 쓴다 */
#include <linux/pci.h> /* [한국어] <linux/pci.h> — 공개 PCI API 헤더. struct pci_dev/pci_bus/pci_driver, pci_power_t, pci_channel_state_t, PCI_NUM_RESOURCES 등 이 파일이 전제하는 모든 기본 타입이 여기서 온다 */
#include <trace/events/pci.h> /* [한국어] <trace/events/pci.h> — 아래 __pcie_update_link_speed() 가 호출하는 trace_pcie_link_event() 트레이스포인트 정의 */

struct pcie_tlp_log; /* [한국어] struct pcie_tlp_log 전방 선언. 실제 정의는 이 트리에 없는 <linux/aer.h> 에 있고, 여기서는 아래 struct aer_err_info 의 tlp 필드와 pcie_read_tlp_log()/pcie_print_tlp_log() 원형에 쓰이기 때문에 이름만 알려 준다 */

/* Number of possible devfns: 0.0 to 1f.7 inclusive */
#define MAX_NR_DEVFNS 256 /* [한국어] BDF 의 devfn 은 device 5비트 + function 3비트 = 8비트이므로 0x00~0xff, 총 256가지. drivers/pci/pci.c:4925 에서 dma_alias_mask 비트맵 크기로, drivers/pci/search.c:41 에서 그 비트맵 순회 상한으로 쓰인다 */
#define PCI_MAX_NR_DEVS	32 /* [한국어] 한 버스에 붙을 수 있는 device 번호 개수(5비트 = 32). drivers/pci/probe.c:3788 의 장치 스캔 루프 상한 */

#define MAX_NR_LANES 16 /* [한국어] PCIe 링크 최대 레인 수 가정치. 아래 struct pci_eq_presets 의 레인별 이퀄라이제이션 프리셋 배열 크기로만 쓰인다 */

#define PCI_FIND_CAP_TTL	48 /* [한국어] capability 연결 리스트 순회 TTL(time to live). 링크가 사이클을 이루는 고장/악성 하드웨어에서 무한 루프에 빠지지 않도록 최대 48회로 끊는다. 표준 config space 256B 중 capability 가 놓일 수 있는 영역(0x40~0xff, 192B)을 최소 크기 4B 로 나눈 값 정도의 여유치다. drivers/pci/quirks.c:2251 등에서도 같은 상수를 직접 쓴다 */

#define PCI_VSEC_ID_INTEL_TBT	0x1234	/* Thunderbolt */ /* [한국어] Intel Thunderbolt 컨트롤러가 쓰는 VSEC(Vendor-Specific Extended Capability) ID. drivers/pci/probe.c:2125 에서 pci_find_vsec_capability(dev, PCI_VENDOR_ID_INTEL, ...) 로 이 VSEC 이 있는지 보고 Thunderbolt 장치를 식별한다 */

#define PCIE_LINK_RETRAIN_TIMEOUT_MS	1000 /* [한국어] 링크 재훈련(retrain) 완료를 기다리는 상한(ms). drivers/pci/pci.c:3539 에서 만료 시각 계산에, :3587 에서 최악의 경우 대기에 쓰인다 */

/*
 * Power stable to PERST# inactive.
 *
 * See the "Power Sequencing and Reset Signal Timings" table of the PCI Express
 * Card Electromechanical Specification, Revision 5.1, Section 2.9.2, Symbol
 * "T_PVPERL".
 */
#define PCIE_T_PVPERL_MS		100 /* [한국어] T_PVPERL: 전원이 안정된 뒤 PERST#(리셋) 을 해제하기까지 지켜야 하는 최소 시간 100ms. 위 영문 주석대로 PCIe CEM 스펙 r5.1 §2.9.2 의 값이며, 호스트 브리지 드라이버들이 리셋 시퀀스에서 msleep(PCIE_T_PVPERL_MS) 로 지킨다(drivers/pci/controller/dwc/pci-imx6.c:1116 등) */

/*
 * REFCLK stable before PERST# inactive.
 *
 * See the "Power Sequencing and Reset Signal Timings" table of the PCI Express
 * Card Electromechanical Specification, Revision 5.1, Section 2.9.2, Symbol
 * "T_PERST-CLK".
 */
#define PCIE_T_PERST_CLK_US		100 /* [한국어] T_PERST-CLK: REFCLK 이 안정된 뒤 PERST# 를 해제하기까지의 최소 100us. 이 트리 안에서는 이 상수를 참조하는 호출부가 확인되지 않는다(정의만 존재) */

/*
 * PCIe r6.0, sec 5.3.3.2.1 <PME Synchronization>
 * Recommends 1ms to 10ms timeout to check L2 ready.
 */
#define PCIE_PME_TO_L2_TIMEOUT_US	10000 /* [한국어] PME(Power Management Event) 동기화용 대기 상한 10ms. PME_Turn_Off 메시지를 보낸 뒤 링크가 L2 준비 상태가 되기를 기다릴 때 쓰며, PCIe r6.0 §5.3.3.2.1 권고(1~10ms)의 상한을 택했다. drivers/pci/controller/dwc/pci-imx6.c:1255 등에서 usleep_range() 상한으로 사용 */

/*
 * PCIe r6.0, sec 6.6.1 <Conventional Reset>
 *
 * - "With a Downstream Port that does not support Link speeds greater
 *    than 5.0 GT/s, software must wait a minimum of 100 ms following exit
 *    from a Conventional Reset before sending a Configuration Request to
 *    the device immediately below that Port."
 *
 * - "With a Downstream Port that supports Link speeds greater than
 *    5.0 GT/s, software must wait a minimum of 100 ms after Link training
 *    completes before sending a Configuration Request to the device
 *    immediately below that Port."
 */
#define PCIE_RESET_CONFIG_WAIT_MS	100 /* [한국어] Conventional Reset 이후 첫 Configuration Request 를 보내기 전까지의 최소 대기 100ms. 이 시간을 지키지 않으면 아직 준비되지 않은 장치가 CRS/RRS 를 돌려주거나 응답하지 않는다. drivers/pci/controller/dwc/pcie-designware.c:761 등에서 msleep 으로 지켜진다 */

/* Parameters for the waiting for link up routine */
/* [한국어] 아래 두 상수는 "링크가 올라올 때까지 기다리는" 폴링 루프의 파라미터다. */
#define PCIE_LINK_WAIT_MAX_RETRIES	10 /* [한국어] 링크 업 폴링 최대 반복 횟수. drivers/pci/controller/dwc/pcie-designware.c:732 의 for 루프 상한 */
#define PCIE_LINK_WAIT_SLEEP_MS		90 /* [한국어] 링크 업 폴링 1회당 대기 시간(ms). 위 상수와 곱해 최대 900ms 를 기다리는 셈이다 */

/* [한국어] --- TLP(Transaction Layer Packet) 헤더의 Fmt 필드 값들 ---
 * PCIe 패킷의 첫 DWORD 최상위에는 Fmt[2:0] 과 Type[4:0] 이 들어간다.
 * Fmt 는 "헤더가 3 DWORD 인가 4 DWORD 인가"(= 주소가 32비트인가 64비트인가)와
 * "데이터 페이로드가 붙는가"를 두 비트로 표현한다. 아래 4개 값이 그 조합이다.
 * 이 상수들은 SoC 호스트 브리지 드라이버가 하드웨어에 직접 TLP 를 만들어
 * 넣을 때(예: config 사이클을 손으로 조립할 때) 쓴다. */
/* Format of TLP; PCIe r7.0, sec 2.2.1 */
#define PCIE_TLP_FMT_3DW_NO_DATA	0x00 /* 3DW header, no data */ /* [한국어] Fmt=0b000: 헤더 3 DWORD(32비트 주소), 데이터 없음 — 예: 32비트 메모리 읽기 요청 */
#define PCIE_TLP_FMT_4DW_NO_DATA	0x01 /* 4DW header, no data */ /* [한국어] Fmt=0b001: 헤더 4 DWORD(64비트 주소), 데이터 없음 — 64비트 주소 공간 읽기 요청 */
#define PCIE_TLP_FMT_3DW_DATA		0x02 /* 3DW header, with data */ /* [한국어] Fmt=0b010: 헤더 3 DWORD, 데이터 있음 — 32비트 주소 쓰기 */
#define PCIE_TLP_FMT_4DW_DATA		0x03 /* 4DW header, with data */ /* [한국어] Fmt=0b011: 헤더 4 DWORD, 데이터 있음 — 64비트 주소 쓰기 */

/* [한국어] --- TLP Type 필드 값들 (Configuration Request) ---
 * Type 0 는 "이 링크 바로 아래 장치를 향한" config 요청이고, Type 1 은
 * "브리지를 더 내려가야 하는" config 요청이다. 버스 열거 시 루트 포트는
 * Type 1 로 내려보내고, 목적지 버스에 도달한 브리지가 Type 0 로 바꿔 준다.
 * 읽기와 쓰기는 Type 값이 같고 Fmt 의 "데이터 있음" 비트로 구분되므로,
 * 아래에서 RD 와 WR 의 값이 서로 같은 것이 정상이다. */
/* Type of TLP; PCIe r7.0, sec 2.2.1 */
#define PCIE_TLP_TYPE_CFG0_RD		0x04 /* Config Type 0 Read Request */ /* [한국어] Type=0x04, Config Type 0 읽기. drivers/pci/controller/pcie-aspeed.c:128 이 config 읽기 TLP 를 조립할 때 사용 */
#define PCIE_TLP_TYPE_CFG0_WR		0x04 /* Config Type 0 Write Request */ /* [한국어] Type=0x04, Config Type 0 쓰기 — 읽기와 Type 값이 같고 Fmt 로만 구분된다 */
#define PCIE_TLP_TYPE_CFG1_RD		0x05 /* Config Type 1 Read Request */ /* [한국어] Type=0x05, Config Type 1 읽기 — 브리지 하위 버스로 전달되어야 하는 요청 */
#define PCIE_TLP_TYPE_CFG1_WR		0x05 /* Config Type 1 Write Request */ /* [한국어] Type=0x05, Config Type 1 쓰기 */

/* [한국어] --- Message 요청의 Routing 서브필드 r[2:0] ---
 * Message TLP 는 Type[2:0] 자리에 "이 메시지를 어디로 보낼 것인가"를 넣는다.
 * 0=Root Complex 로, 1=주소 기반, 2=ID(BDF) 기반, 3=브로드캐스트(하향),
 * 4=로컬(수신 링크에서 종료), 5=Gather(상향 수집). */
/* Message Routing (r[2:0]); PCIe r6.0, sec 2.2.8 */
#define PCIE_MSG_TYPE_R_RC	0 /* [한국어] r=0: Routed to Root Complex — 상위로 올려 RC 가 처리(예: PME 통지) */
#define PCIE_MSG_TYPE_R_ADDR	1 /* [한국어] r=1: Routed by Address — 헤더의 주소 필드로 라우팅 */
#define PCIE_MSG_TYPE_R_ID	2 /* [한국어] r=2: Routed by ID — 목적지 BDF 로 라우팅 */
#define PCIE_MSG_TYPE_R_BC	3 /* [한국어] r=3: Broadcast from Root Complex — RC 가 아래로 전체 살포 */
#define PCIE_MSG_TYPE_R_LOCAL	4 /* [한국어] r=4: Local — Terminate at Receiver. 수신 포트에서 소비되고 더 전달되지 않는다. drivers/pci/controller/cadence/pcie-cadence-ep.c:350 에서 사용 */
#define PCIE_MSG_TYPE_R_GATHER	5 /* [한국어] r=5: Gathered and routed to Root Complex — 하위에서 모아 RC 로 올림 */

/* [한국어] --- Power Management Message 의 Message Code ---
 * 시스템이 서스펜드로 들어갈 때 상위 포트가 하위로 PME_Turn_Off 를 보내고,
 * 하위는 PME_TO_Ack 로 답한 뒤 링크를 L2/L3 로 내린다. */
/* Power Management Messages; PCIe r6.0, sec 2.2.8.2 */
#define PCIE_MSG_CODE_PME_TURN_OFF	0x19 /* [한국어] Message Code 0x19 = PME_Turn_Off. drivers/pci/controller/dwc/pcie-designware-host.c:1123 에서 서스펜드 시 이 코드로 메시지를 만들어 보낸다 */

/* [한국어] --- INTx 에뮬레이션 Message 의 Message Code ---
 * PCIe 에는 물리 INTA#~INTD# 핀이 없으므로, 레거시 인터럽트는 Assert_INTx /
 * Deassert_INTx 메시지로 흉내 낸다. 0x20~0x23 이 assert(A~D), 0x24~0x27 이
 * deassert(A~D) 로 A/B/C/D 순서가 그대로 이어진다.
 * NVMe 는 보통 MSI-X 를 쓰지만, MSI-X 할당에 실패해 INTx 로 떨어지면
 * 이 경로의 메시지로 인터럽트가 전달된다. */
/* INTx Mechanism Messages; PCIe r6.0, sec 2.2.8.1 */
#define PCIE_MSG_CODE_ASSERT_INTA	0x20 /* [한국어] Assert_INTA — 레거시 INTA# 어서트에 대응 */
#define PCIE_MSG_CODE_ASSERT_INTB	0x21 /* [한국어] Assert_INTB */
#define PCIE_MSG_CODE_ASSERT_INTC	0x22 /* [한국어] Assert_INTC */
#define PCIE_MSG_CODE_ASSERT_INTD	0x23 /* [한국어] Assert_INTD */
#define PCIE_MSG_CODE_DEASSERT_INTA	0x24 /* [한국어] Deassert_INTA — 어서트 해제. 레벨 트리거 INTx 는 assert/deassert 쌍으로 관리된다 */
#define PCIE_MSG_CODE_DEASSERT_INTB	0x25 /* [한국어] Deassert_INTB */
#define PCIE_MSG_CODE_DEASSERT_INTC	0x26 /* [한국어] Deassert_INTC */
#define PCIE_MSG_CODE_DEASSERT_INTD	0x27 /* [한국어] Deassert_INTD */

/* [한국어] --- Completion TLP 의 Completion Status ---
 * config/메모리 읽기에 대한 응답(Completion)의 상태 코드. 0x00 만 성공이고
 * UR(Unsupported Request)/CA(Completer Abort)/RRS 는 0 이 아니다.
 * 응답이 성공이 아니면 소프트웨어는 보통 all-1(0xffffffff)을 읽은 것으로
 * 처리한다 — "장치 없음"과 "오류"가 같은 모습으로 보이는 이유다. */
/* Cpl. status of Complete; PCIe r7.0, sec 2.2.9.1 */
#define PCIE_CPL_STS_SUCCESS		0x00 /* Successful Completion */ /* [한국어] Completion Status = 0b000 (Successful Completion). drivers/pci/controller/pcie-aspeed.c:353 은 하드웨어가 잡아 둔 완료 상태가 이 값이 아니면 config 접근 실패로 처리한다 */

/* [한국어] --- struct pci_bus.resource[] 배열의 인덱스 ---
 * 주의: 아래 세 상수는 "버스"의 리소스 배열 인덱스이지,
 * struct pci_dev.resource[] 의 인덱스(PCI_BRIDGE_RESOURCES 계열)가 아니다.
 * P2P 브리지는 자기 아래 버스로 내려보낼 주소 범위를 세 종류(IO 창,
 * 논-prefetchable MEM 창, prefetchable MEM 창)로 광고하며, 그 세 창이
 * 하위 pci_bus 의 resource[0..2] 에 그대로 실린다.
 * 설정자: drivers/pci/probe.c:762~769 (브리지 config 에서 읽어 채움).
 * 읽는 자: drivers/pci/setup-bus.c:176~183 (pci_bus_resource_n 으로 꺼내
 * 하위 장치들의 BAR 를 이 창 안에 배치). */
#define PCI_BUS_BRIDGE_IO_WINDOW	0 /* [한국어] bus->resource[0] = 브리지의 I/O 창 (config 오프셋 0x1c/0x1d + 0x30/0x32 의 I/O Base/Limit) */
#define PCI_BUS_BRIDGE_MEM_WINDOW	1 /* [한국어] bus->resource[1] = 논-prefetchable 메모리 창 (config 0x20/0x22 의 Memory Base/Limit) */
#define PCI_BUS_BRIDGE_PREF_MEM_WINDOW	2 /* [한국어] bus->resource[2] = prefetchable 메모리 창 (config 0x24/0x26 + 0x28/0x2c 의 64비트 확장). NVMe 카드가 큰 BAR 를 요구하면 이 창의 크기가 문제가 된다 */

/* [한국어] AER 을 켤 때 Device Control 레지스터(PCI_EXP_DEVCTL)에 세울 4개 비트 묶음:
 *   CERE = Correctable Error Reporting Enable
 *   NFERE = Non-Fatal Error Reporting Enable
 *   FERE  = Fatal Error Reporting Enable
 *   URRE  = Unsupported Request Reporting Enable
 * 이 비트들이 서야 장치가 오류를 상위로 보고(ERR_COR/ERR_NONFATAL/ERR_FATAL
 * 메시지)한다.
 * 설정자: drivers/pci/pcie/aer.c:233 의
 *   pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS).
 * 읽는 자: drivers/pci/pci-acpi.c:322~323 은 ACPI _HPX 가 준 hotplug 파라미터가
 * 이 네 비트를 건드리지 못하도록 마스크로 쓴다(펌웨어보다 커널 정책 우선).
 * NVMe 접점: 이 비트가 서 있어야 NVMe 컨트롤러의 PCIe 오류가 AER 로 올라오고,
 * 그 결과 drivers/nvme/host/pci.c:5158 의 .error_detected 콜백이 불린다. */
#define PCI_EXP_AER_FLAGS	(PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE | \
				 PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE)

/* [한국어] 링크 속도 코드(LNKSTA 의 Current Link Speed 필드 값)를
 * enum pci_bus_speed 로 바꾸는 변환 표.
 * 설정자: drivers/pci/probe.c:970 에서 배열 리터럴로 정의되고 :989 에서
 *   EXPORT_SYMBOL_GPL 로 내보내진다(내부 헤더에 선언되어 있지만 심볼 자체는
 *   외부 모듈에도 열려 있다).
 * 읽는 자: drivers/pci/pci-sysfs.c:222, drivers/pci/pci.c:4649,
 *   그리고 아래 __pcie_update_link_speed().
 * 값 범위: 인덱스는 LNKSTA & PCI_EXP_LNKSTA_CLS (4비트).
 * 동기화: const 배열이라 읽기 전용 — 락 불필요. */
extern const unsigned char pcie_link_speed[];
/* [한국어] pcie_link_speed[] 를 범위 검사와 함께 감싼 접근자.
 * 정의는 drivers/pci/probe.c:994 이며, :996 에서 speed 가
 * ARRAY_SIZE(pcie_link_speed) 이상이면 PCI_SPEED_UNKNOWN 을 돌려준다. */
unsigned char pcie_get_link_speed(unsigned int speed);

/* [한국어] "pci=early_dump" 커널 파라미터로 켜지는 디버그 플래그.
 * 설정자: drivers/pci/pci.c:5320 (부팅 파라미터 파서). 실체는 pci.c:132.
 * 읽는 자: drivers/pci/probe.c:2527 — 참이면 장치를 발견하자마자 config
 *   space 256바이트를 통째로 덤프해 커널 로그에 찍는다.
 * 값 범위: true/false.
 * 동기화: 부팅 초기에 한 번만 쓰이고 이후 읽기 전용이라 락 없음. */
extern bool pci_early_dump;

/* [한국어] 버스 재스캔(rescan)과 장치 제거(remove)를 상호 배제하는 전역 뮤텍스.
 * 실체: drivers/pci/probe.c:4278 의 DEFINE_MUTEX(pci_rescan_remove_lock).
 * 설정자/읽는 자: probe.c:4285/:4295 의 pci_lock_rescan_remove()/
 *   pci_unlock_rescan_remove() 쌍, 그리고 remove.c:119 는
 *   lockdep_assert_held() 로 "이 락을 쥐고 들어와야 한다"를 강제한다.
 * 왜 필요한가: 열거가 pci_dev 를 만들어 버스 리스트에 넣는 동안 다른
 *   스레드(핫플러그 이벤트, sysfs remove)가 같은 장치를 지우면 use-after-free 가
 *   된다. 두 작업을 한 줄로 세우는 것이 이 락의 유일한 목적이다.
 * 동기화: 뮤텍스이므로 잠들 수 있는 프로세스 컨텍스트에서만 잡을 수 있다. */
extern struct mutex pci_rescan_remove_lock;

/* [한국어] 아래 세 함수는 "이 장치의 PCIe Capability 구조에 그 레지스터가
 * 실제로 존재하는가"를 판정한다. PCIe Capability 는 장치 타입(엔드포인트,
 * 루트 포트, 스위치 업/다운스트림 포트, RCiEP ...)과 Capability Version 에
 * 따라 일부 레지스터가 아예 없다. 없는 레지스터를 읽으면 쓰레기 값이
 * 나오므로, pcie_capability_read_word() 계열이 접근 전에 이 판정을 거친다.
 * 셋 다 정의는 drivers/pci/access.c 에 있다. */
bool pcie_cap_has_lnkctl(const struct pci_dev *dev);
bool pcie_cap_has_lnkctl2(const struct pci_dev *dev); /* [한국어] Link Control 2 는 Capability Version 이 2 이상일 때만 존재한다 — access.c:332 가 lnkctl 존재 여부 AND 버전>1 로 판정 */
bool pcie_cap_has_rtctl(const struct pci_dev *dev); /* [한국어] Root Control 은 루트 포트(PCI_EXP_TYPE_ROOT_PORT)와 RC Event Collector(RC_EC)에만 있다 — access.c:347~348 */

/* [한국어]
 * PCI_FIND_NEXT_CAP - 표준(레거시) capability 연결 리스트를 순회해 원하는 ID 를 찾는다
 *
 * @read_cfg: config 읽기 함수의 "이름 접두사". 매크로 안에서 read_cfg##_byte /
 *            read_cfg##_word 로 토큰 결합되므로, 실제로는 함수 포인터가 아니라
 *            "pci_bus_read_config" 같은 접두사 문자열이다. 이렇게 만든 이유는
 *            아직 struct pci_dev 가 없는 열거 초기(버스+devfn 만 아는 시점)와,
 *            pci_dev 도 pci_bus 도 없는 호스트 브리지 드라이버(자기만의 config
 *            접근 함수를 가진 controller/ 드라이버)가 같은 순회 로직을 공유하기
 *            위해서다.
 * @start:    순회를 시작할 config space 오프셋. 보통 0x34(PCI_CAPABILITY_LIST).
 * @cap:      찾을 Capability ID (예: PCI_CAP_ID_EXP=0x10, PCI_CAP_ID_MSIX=0x11).
 * @prev_ptr: NULL 이 아니면 "찾은 capability 바로 앞 capability 의 위치"를 여기에
 *            써 준다. 리스트에서 항목을 떼어낼 때(next 포인터를 고쳐 써야 할 때) 쓴다.
 * @args:     read_cfg 함수의 앞쪽 인자들 (예: bus, devfn 또는 컨트롤러 포인터).
 * @return:   찾았으면 그 capability 의 config space 오프셋, 못 찾으면 0.
 *            0 은 유효한 capability 위치가 될 수 없으므로(표준 헤더 영역이다)
 *            "없음" 표시로 안전하다.
 *
 * 왜 필요한가: PCI 표준 헤더(0x00~0x3f)를 넘어가는 기능들은 전부
 * "capability" 라는 단일 연결 리스트로 광고된다. 리스트 각 항목은
 *   +0: Capability ID (1바이트)
 *   +1: Next Capability Pointer (1바이트, 0 이면 끝)
 * 구조이고, 리스트의 머리는 config 오프셋 0x34 에 있다. 이 매크로가 그
 * 링크를 따라가며 원하는 ID 를 찾는다.
 *
 * 라인별 보충 — 매크로 연속행의 백슬래시 바로 앞에 주석을 넣으면 원본 코드
 * 텍스트가 바뀌므로, 아래 한 줄만 여기에 따로 적는다:
 *   `*(u8 *)prev_ptr = __prev_pos;`
 * prev_ptr 을 u8 * 로 캐스팅해 쓰는 이유는 표준 capability 의 Next Capability
 * Pointer 가 1바이트이기 때문이다. 호출자는 u8 변수의 주소를 넘겨야 한다.
 *
 * 실행 컨텍스트: 문장식(statement expression, GCC 확장 ({ ... }))이라
 * 호출한 자리에 그대로 펼쳐진다. 별도 락을 잡지 않고, config 읽기 함수
 * 내부의 pci_lock 에만 의존한다. 열거 중(프로세스 컨텍스트)에 불린다.
 *
 * 호출 체인:
 *   drivers/pci/pci.c:339 __pci_find_next_cap_ttl 계열 → [이 매크로] →
 *     pci_bus_read_config_byte/word → 버스 ops → 호스트 브리지 하드웨어
 *   drivers/pci/controller/dwc/pcie-designware.c:224 dw_pcie_find_capability →
 *     [이 매크로] → dw_pcie_read_cfg
 *
 * NVMe 접점: NVMe 컨트롤러의 MSI-X capability(ID 0x11)와 PCIe capability
 * (ID 0x10)를 찾는 것이 결국 이 순회다. 그 결과로 pci_dev->msix_cap /
 * pci_dev->pcie_cap 이 채워지고, drivers/nvme/host/pci.c 가 부르는
 * pci_alloc_irq_vectors() 가 그 값을 전제로 동작한다. */
/* Standard Capability finder */
/**
 * PCI_FIND_NEXT_CAP - Find a PCI standard capability
 * @read_cfg: Function pointer for reading PCI config space
 * @start: Starting position to begin search
 * @cap: Capability ID to find
 * @prev_ptr: Pointer to store position of preceding capability (optional)
 * @args: Arguments to pass to read_cfg function
 *
 * Search the capability list in PCI config space to find @cap. If
 * found, update *prev_ptr with the position of the preceding capability
 * (if prev_ptr != NULL)
 * Implements TTL (time-to-live) protection against infinite loops.
 *
 * Return: Position of the capability if found, 0 otherwise.
 */
#define PCI_FIND_NEXT_CAP(read_cfg, start, cap, prev_ptr, args...) /* [한국어] 가변 인자 매크로. args... 는 read_cfg 함수의 선행 인자들을 그대로 흘려보내기 위한 것 */ \
({ /* [한국어] GCC 문장식 시작 — 블록의 마지막 식(__found_pos)이 매크로 전체의 값이 된다 */ \
	int __ttl = PCI_FIND_CAP_TTL; /* [한국어] TTL 카운터 초기화(48). 링크가 사이클을 이루는 불량 하드웨어에서 무한 루프를 막는 유일한 장치 */ \
	u8 __id,  __found_pos = 0; /* [한국어] __id 는 현재 항목의 Capability ID, __found_pos 는 결과. 못 찾으면 0 그대로 반환된다 */ \
	u8 __prev_pos = (start); /* [한국어] "직전 항목" 위치. 첫 바퀴에서는 아직 앞 항목이 없으므로 리스트 머리(start)로 둔다 */ \
	u8 __pos = (start); /* [한국어] 현재 검사 위치. 아래에서 곧바로 이 오프셋의 내용을 읽어 첫 항목 위치로 덮어쓴다 */ \
	u16 __ent; /* [한국어] capability 항목의 첫 2바이트(ID + Next)를 한 번에 담을 변수 */ \
									\
	read_cfg##_byte(args, __pos, &__pos); /* [한국어] 리스트 머리(보통 0x34)의 1바이트를 읽어 첫 capability 의 오프셋을 얻는다. __pos 를 입력이자 출력으로 재사용 */ \
									\
	while (__ttl--) { /* [한국어] TTL 을 소진하며 반복. __ttl-- 은 후위 감소라 48회 실행 후 0 일 때 탈출 */ \
		if (__pos < PCI_STD_HEADER_SIZEOF) /* [한국어] 유효한 capability 는 표준 헤더(64바이트) 뒤에만 올 수 있다. 0 을 포함해 0x40 미만이면 리스트 끝이거나 쓰레기 값 */ \
			break; /* [한국어] 리스트 종료 — 못 찾은 채로 빠져나간다 */ \
									\
		__pos = ALIGN_DOWN(__pos, 4); /* [한국어] capability 는 DWORD 정렬이 규칙이므로 4바이트 경계로 내림. 하위 2비트가 오염된 값을 방어 */ \
		read_cfg##_word(args, __pos, &__ent); /* [한국어] ID(하위 8비트)와 Next(상위 8비트)를 word 한 번에 읽는다 — config 접근 횟수를 절반으로 줄인다 */ \
									\
		__id = FIELD_GET(PCI_CAP_ID_MASK, __ent); /* [한국어] PCI_CAP_ID_MASK = 0x00ff — 읽은 word 의 하위 바이트가 Capability ID */ \
		if (__id == 0xff) /* [한국어] 0xff 는 응답 없음(all-1)일 때 나오는 값. 장치가 사라졌거나 config 읽기가 실패했다는 신호 */ \
			break; /* [한국어] 더 따라가 봐야 의미 없으므로 중단 */ \
									\
		if (__id == (cap)) { /* [한국어] 찾던 ID 와 일치 */ \
			__found_pos = __pos; /* [한국어] 결과 위치 기록 */ \
			if (prev_ptr != NULL) /* [한국어] 호출자가 직전 항목 위치를 원했다면 */ \
				*(u8 *)prev_ptr = __prev_pos;		\
			break; /* [한국어] 찾았으니 순회 종료 */ \
		}							\
									\
		__prev_pos = __pos; /* [한국어] 다음 바퀴를 위해 현재 위치를 "직전"으로 밀어 둔다 */ \
		__pos = FIELD_GET(PCI_CAP_LIST_NEXT_MASK, __ent); /* [한국어] PCI_CAP_LIST_NEXT_MASK = 0xff00 — 읽은 word 의 상위 바이트가 다음 항목 오프셋 */ \
	}								\
	__found_pos; /* [한국어] 문장식의 마지막 식 — 이 값이 매크로의 결과가 된다 */ \
})

/* [한국어]
 * PCI_FIND_NEXT_EXT_CAP - PCIe 확장(extended) capability 리스트를 순회한다
 *
 * @read_cfg: config 읽기 함수 이름 접두사. 여기서는 read_cfg##_dword 로만 쓴다.
 * @start:    시작 오프셋. 0 을 주면 확장 리스트의 규정 시작점인 0x100
 *            (PCI_CFG_SPACE_SIZE)에서 시작한다. 0 이 아니면 그 위치의 "다음"부터
 *            이어서 찾는 재개(resume) 검색이다.
 * @cap:      찾을 확장 Capability ID (예: PCI_EXT_CAP_ID_ERR=0x01 AER,
 *            PCI_EXT_CAP_ID_SRIOV=0x10, PCI_EXT_CAP_ID_DPC=0x1d).
 * @prev_ptr: NULL 이 아니면 직전 항목의 오프셋을 u16 으로 써 준다.
 * @args:     read_cfg 의 선행 인자들.
 * @return:   찾은 오프셋, 못 찾으면 0.
 *
 * 표준 capability 와의 차이 세 가지가 이 매크로의 존재 이유다:
 *  (1) 위치: 표준은 config 0x40~0xff, 확장은 0x100~0xfff (총 4KB). 확장 영역은
 *      레거시 config 사이클로는 닿지 않고 ECAM(memory-mapped config) 또는
 *      확장 config 메커니즘이 있어야 읽힌다.
 *  (2) 헤더 크기: 확장 capability 헤더는 4바이트(1 DWORD)이고
 *      [15:0]=Capability ID, [19:16]=Capability Version,
 *      [31:20]=Next Capability Offset 이다. 그래서 dword 로 읽고
 *      PCI_EXT_CAP_ID()/PCI_EXT_CAP_NEXT() 로 쪼갠다.
 *  (3) 오류 판정: 표준 쪽은 0xff 를, 여기서는 "헤더 전체가 0" 을 리스트 끝/부재로 본다.
 *
 * 라인별 보충 — `*(u16 *)prev_ptr = __prev_pos;`
 * 여기서는 u16 * 로 캐스팅한다. 확장 capability 의 Next Offset 은 12비트라
 * 1바이트로는 표현할 수 없기 때문이다(표준 쪽의 u8 캐스팅과 대비된다).
 *
 * 라인별 보충 — 아래 408행
 *   `if (PCI_EXT_CAP_ID(__header) == (cap) && __pos != start)`
 * 은 매크로 연속행 백슬래시 바로 앞에 주석을 넣으면 코드 텍스트가 바뀌어
 * 여기에 적는다: ID 가 일치해도 __pos 가 @start 와 같으면 건너뛴다. 재개 검색
 * (start != 0)에서 "출발점 자기 자신"을 다시 찾아 무한히 제자리를 돌지 않게
 * 하려는 방어다.
 *
 * 실행 컨텍스트: 문장식이라 호출 지점에 인라인 전개된다. 락 없음.
 *
 * 호출 체인:
 *   drivers/pci/pci.c:407 (pci_find_ext_capability 계열) → [이 매크로] →
 *     pci_bus_read_config_dword
 *   drivers/pci/controller/dwc/pcie-designware.c:232/264/298 → [이 매크로] →
 *     dw_pcie_read_cfg
 *
 * NVMe 접점: drivers/nvme/host/pci.c:5382 이 .sriov_configure 로 등록한
 * pci_sriov_configure_simple() 이 동작하려면 먼저 SR-IOV 확장 capability 를
 * 찾아야 하는데, 그 탐색이 drivers/pci/iov.c:961 의
 * pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV) → 이 매크로다. */
/* Extended Capability finder */
/**
 * PCI_FIND_NEXT_EXT_CAP - Find a PCI extended capability
 * @read_cfg: Function pointer for reading PCI config space
 * @start: Starting position to begin search (0 for initial search)
 * @cap: Extended capability ID to find
 * @prev_ptr: Pointer to store position of preceding capability (optional)
 * @args: Arguments to pass to read_cfg function
 *
 * Search the extended capability list in PCI config space to find @cap.
 * If found, update *prev_ptr with the position of the preceding capability
 * (if prev_ptr != NULL)
 * Implements TTL protection against infinite loops using a calculated
 * maximum search count.
 *
 * Return: Position of the capability if found, 0 otherwise.
 */
#define PCI_FIND_NEXT_EXT_CAP(read_cfg, start, cap, prev_ptr, args...) /* [한국어] 표준 capability 매크로와 같은 인자 규약. read_cfg 는 토큰 결합용 접두사 */ \
({ /* [한국어] GCC 문장식 시작 */ \
	u16 __pos = (start) ?: PCI_CFG_SPACE_SIZE; /* [한국어] GNU 확장 ?: — start 가 0(거짓)이면 PCI_CFG_SPACE_SIZE(=256, 즉 0x100)를 쓴다. 0x100 은 PCIe 확장 config 영역의 시작이자 첫 확장 capability 의 고정 위치 */ \
	u16 __found_pos = 0; /* [한국어] 결과. 못 찾으면 0 이 그대로 반환된다 */ \
	u16 __prev_pos; /* [한국어] 직전 항목 오프셋. 확장 리스트의 next 는 12비트라 u16 이 필요 */ \
	int __ttl, __ret; /* [한국어] __ttl 은 순회 상한, __ret 는 config 읽기 반환 코드 */ \
	u32 __header; /* [한국어] 확장 capability 헤더는 4바이트라 u32 로 한 번에 읽는다 */ \
									\
	__prev_pos = __pos; /* [한국어] 아직 한 칸도 못 갔으므로 직전 = 현재로 초기화 */ \
	__ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8; /* [한국어] (4096 - 256) / 8 = 480. 확장 config 영역 3840바이트를 최소 capability 크기 8바이트로 나눈 값 — 이론상 있을 수 있는 최대 항목 수이자 사이클 방어용 TTL */ \
	while (__ttl-- > 0 && __pos >= PCI_CFG_SPACE_SIZE) { /* [한국어] TTL 이 남아 있고 위치가 확장 영역(0x100 이상) 안일 때만 계속한다. 0x100 미만이면 리스트 끝(next=0)을 만난 것 */ \
		__ret = read_cfg##_dword(args, __pos, &__header); /* [한국어] 헤더 1 DWORD 를 읽는다. 여기만 dword 접근인 이유는 확장 헤더가 4바이트이기 때문 */ \
		if (__ret != PCIBIOS_SUCCESSFUL) /* [한국어] PCIBIOS_SUCCESSFUL(0)이 아니면 버스 오류/장치 부재 — 더 진행 불가 */ \
			break; /* [한국어] 읽기 실패 시 못 찾은 것으로 종료 */ \
									\
		if (__header == 0) /* [한국어] 헤더가 전부 0 이면 확장 capability 자체가 없는 장치다(PCIe r6.0: 미구현 시 0 을 반환) */ \
			break; /* [한국어] 리스트 없음 — 종료 */ \
									\
		if (PCI_EXT_CAP_ID(__header) == (cap) && __pos != start) {\
			__found_pos = __pos; /* [한국어] 찾은 위치 기록 */ \
			if (prev_ptr != NULL) /* [한국어] 호출자가 직전 위치를 요구했다면 */ \
				*(u16 *)prev_ptr = __prev_pos;		\
			break; /* [한국어] 찾았으므로 종료 */ \
		}							\
									\
		__prev_pos = __pos; /* [한국어] 현재를 직전으로 밀어 둔다 */ \
		__pos = PCI_EXT_CAP_NEXT(__header); /* [한국어] PCI_EXT_CAP_NEXT() 는 헤더 [31:20] 을 꺼낸다. 다음 항목이 없으면 0 이 되어 while 조건에서 걸린다 */ \
	}								\
	__found_pos; /* [한국어] 문장식의 결과 값 */ \
})

/* Functions internal to the PCI core code */

/* [한국어] CONFIG_DMI 분기: DMI(SMBIOS) 테이블을 파싱할 수 있는 아키텍처(주로 x86)
 * 에서만 존재하는 sysfs 속성 그룹 선언. 이 그룹은 메인보드 펌웨어가 슬롯에
 * 붙여 준 이름(label)을 /sys 에 노출한다.
 * 기능이 꺼져 있으면 이 심볼 자체가 선언되지 않고, 참조하는 코드도 함께
 * #ifdef 로 빠지므로 별도의 빈 스텁을 두지 않는다 — 이 파일에서 스텁 없이
 * 선언만 #ifdef 로 감싼 몇 안 되는 사례다. */
#ifdef CONFIG_DMI
extern const struct attribute_group pci_dev_smbios_attr_group; /* [한국어] DMI 기반 슬롯 라벨 sysfs 속성 그룹. 실체는 drivers/pci/pci-label.c 에 있다 */
#endif /* [한국어] CONFIG_DMI 분기 종료 */

/* [한국어] mmap 요청이 어느 유저스페이스 인터페이스로 들어왔는지 구분하는 열거형.
 * 두 인터페이스의 주소 해석 규칙이 다르기 때문에 필요하다.
 * 설정자: 호출부에서 상수로 넘긴다.
 * 읽는 자: pci_mmap_fits()(drivers/pci/mmap.c) 가 이 값에 따라 vma 의
 *   pgoff 를 "리소스 시작으로부터의 오프셋"으로 볼지 "절대 물리 페이지 번호"로
 *   볼지 결정한다.
 * 동기화: 값 전달용 열거형이라 공유 상태가 없다. */
enum pci_mmap_api {
	PCI_MMAP_SYSFS,	/* mmap on /sys/bus/pci/devices/<BDF>/resource<N> */ /* [한국어] sysfs 경로: /sys/bus/pci/devices/<BDF>/resource<N> 를 mmap 하는 경우. pgoff 가 "BAR 시작으로부터의 오프셋"이다 */
	PCI_MMAP_PROCFS	/* mmap on /proc/bus/pci/<BDF> */ /* [한국어] procfs 경로: /proc/bus/pci/<BDF> 를 mmap 하는 경우. 레거시 인터페이스라 주소 해석 규칙이 다르다 */
}; /* [한국어] enum 정의 종료 */
/* [한국어]
 * pci_mmap_fits - 유저가 요청한 mmap 범위가 해당 BAR 안에 들어오는지 검사
 *
 * @pdev: 대상 PCI 장치.
 * @resno: BAR 번호(resource[] 인덱스).
 * @vmai: 유저가 요청한 가상 메모리 영역 — 시작(pgoff)과 길이를 여기서 본다.
 * @mmap_api: 요청이 sysfs 경로인지 procfs 경로인지 (위 enum).
 * @return: 범위 안이면 1(참), 벗어나면 0. 호출자는 0 이면 -EINVAL 로 거절한다.
 *
 * 왜 필요한가: 유저스페이스가 /sys/.../resourceN 이나 /proc/bus/pci/ 를 통해
 * 장치 레지스터를 직접 매핑할 수 있는데, 검사 없이 허용하면 그 BAR 를 넘어
 * 다른 장치의 MMIO 나 시스템 메모리까지 매핑될 수 있다. 이 함수가 그 경계
 * 검사를 담당한다.
 * 실행 컨텍스트: 유저 프로세스의 mmap(2) 시스템 호출 문맥(프로세스 컨텍스트).
 * 호출 체인:
 *   sys_mmap → pci_mmap_resource(drivers/pci/pci-sysfs.c:1006) → [pci_mmap_fits]
 *   sys_mmap → proc_bus_pci_mmap(drivers/pci/proc.c:267) → [pci_mmap_fits]
 * 정의: drivers/pci/mmap.c:73 */
int pci_mmap_fits(struct pci_dev *pdev, int resno, struct vm_area_struct *vmai,
		  enum pci_mmap_api mmap_api);

/* [한국어] --- 장치/버스 리셋 관련 내부 API 묶음 ---
 * PCI 에는 여러 층위의 리셋이 있고, 커널은 "가능한 것 중 가장 덜 파괴적인
 * 것"을 골라 쓴다. 순서는 대략
 *   ACPI _RST → 장치 특화 리셋(quirk) → FLR(Function Level Reset) →
 *   PM D3hot→D0 왕복 → 상위 브리지의 Secondary Bus Reset(SBR)
 * 이며, 그 후보 목록이 pci_dev.reset_methods[] 에 담긴다.
 * NVMe 접점: drivers/nvme/host/pci.c:3059 는 컨트롤러가 응답하지 않을 때
 * pcie_reset_flr(pdev, false) 로 FLR 을 직접 건다. FLR 은 아래 목록 중
 * 하나이며, 그 지원 여부 판정이 pci_init_reset_methods() 에서 이뤄진다. */
bool pci_reset_supported(struct pci_dev *dev); /* [한국어] 이 장치에 쓸 수 있는 리셋 방법이 하나라도 있는지. 정의는 drivers/pci/pci.c:83 이며 dev->reset_methods[0] != 0 을 본다 — 배열 첫 칸이 0 이면 후보가 하나도 없다는 뜻 */
void pci_init_reset_methods(struct pci_dev *dev); /* [한국어] 장치별로 가능한 리셋 방법을 조사해 dev->reset_methods[] 를 우선순위 순으로 채운다. 정의 drivers/pci/pci.c:4006, 채우는 부분은 :4018, 끝을 0 으로 막는 부분은 :4023 */
int pci_bridge_secondary_bus_reset(struct pci_dev *dev); /* [한국어] 상위 브리지의 Secondary Bus Reset 비트(브리지 config 0x3e Bridge Control 의 BUS_RESET)를 토글해 하위 버스 전체를 리셋한다. 링크 아래 모든 장치가 함께 리셋되므로 가장 파괴적이다. 정의 drivers/pci/pci.c:3738 */
int pci_bus_error_reset(struct pci_dev *dev); /* [한국어] 버스 단위 오류 복구용 리셋. 정의 drivers/pci/pci.c:4455 */
int pci_try_reset_bridge(struct pci_dev *bridge); /* [한국어] 브리지에 대해 리셋을 시도한다. 정의 drivers/pci/pci.c:4461 */

/* [한국어]
 * struct pci_cap_saved_data - 한 capability 의 레지스터 스냅샷(가변 길이 몸통)
 *
 * suspend 전에 capability 레지스터를 통째로 떠 두었다가 resume 후 되돌리기
 * 위한 자료구조다. 커널이 되돌려 주지 않으면, D3cold 를 거친 장치는
 * MSI-X 설정도 PCIe Device Control 도 전부 하드웨어 기본값으로 초기화되어
 * 드라이버가 기대하는 상태와 어긋난다.
 * 이 구조체는 항상 아래 struct pci_cap_saved_state 의 마지막 멤버로만
 * 쓰이며, 단독으로 할당되지 않는다. */
struct pci_cap_saved_data {
	u16		cap_nr; /* [한국어] capability ID. 표준이면 8비트 ID(예: PCI_CAP_ID_EXP=0x10), 확장이면 16비트 ID(예: PCI_EXT_CAP_ID_LTR)라 u16 으로 통일. 설정자: drivers/pci/pci.c:2743. 읽는 자: pci.c:1274 의 _pci_find_saved_cap() 비교, :1562 의 상태 복사. 동기화: 장치 열거 시 한 번 쓰고 이후 읽기 전용 */
	bool		cap_extended; /* [한국어] 이 항목이 확장 capability 인지(true) 표준 capability 인지(false). 같은 숫자 ID 라도 두 공간이 별개라서 ID 만으로는 구분되지 않기 때문에 필요하다. 설정자: pci.c:2744. 읽는 자: pci.c:1274 의 비교식 tmp->cap.cap_extended == extended */
	unsigned int	size; /* [한국어] data[] 의 바이트 크기. 설정자: pci.c:2745 (호출자가 넘긴 size 그대로). 읽는 자: pci.c:1523/:1534 의 상태 직렬화 크기 계산, :1563 의 크기 일치 검증, :1566 의 memcpy 길이. 값 범위: capability 종류마다 고정(예: LTR 은 2*sizeof(u16)) */
	u32		data[]; /* [한국어] 가변 길이 배열(flexible array member) — 실제 레지스터 값이 여기 담긴다. 설정자/읽는 자: pci.c:1308/:1341/:1368/:1384 가 (u16 *) 로 캐스팅해 PCIe capability 레지스터들을 넣고 뺀다. 할당은 pci.c:2739 의 kzalloc(sizeof(*save_state) + size) 로 껍데기와 한 덩어리로 이뤄진다 */
};

/* [한국어]
 * struct pci_cap_saved_state - 위 스냅샷을 pci_dev 의 해시 리스트에 매다는 껍데기
 *
 * 한 장치가 저장해야 할 capability 는 여러 개(PCIe, PCI-X, LTR, VC ...)이고
 * 개수가 장치마다 다르므로, 고정 배열이 아니라 연결 리스트로 관리한다. */
struct pci_cap_saved_state {
	struct hlist_node		next; /* [한국어] 해시 리스트 연결 고리. 설정자: pci.c:2721 의 hlist_add_head(&new_cap->next, &pci_dev->saved_cap_space). 읽는 자: pci.c:1273/:1522/:1533/:2792 의 hlist_for_each_entry 순회. 동기화: 장치 열거/제거 시점에만 변경되고 그 사이에는 읽기만 하므로 별도 락이 없다 */
	struct pci_cap_saved_data	cap; /* [한국어] 실제 스냅샷 몸통. 마지막 멤버여야 하는 이유는 안쪽 data[] 가 가변 길이라서다 — 뒤에 다른 필드를 두면 배열이 그 필드를 덮는다 */
};

/* [한국어] --- capability 스냅샷 버퍼의 생명주기 API ---
 * 할당은 장치 열거 직후 한 번(pci_allocate_cap_save_buffers), 해제는 장치
 * 제거 시(pci_free_cap_save_buffers) 이뤄진다. 실제 레지스터 값을 채우고
 * 되돌리는 것은 pci_save_state()/pci_restore_state() 쪽이며, 그때 아래
 * pci_find_saved_cap()/pci_find_saved_ext_cap() 으로 해당 버퍼를 찾는다.
 * 미리 할당해 두는 이유: suspend 경로는 메모리 할당이 실패하면 안 되는
 * 구간이라, 잠들기 직전이 아니라 여유 있는 열거 시점에 미리 잡아 둔다. */
void pci_allocate_cap_save_buffers(struct pci_dev *dev); /* [한국어] 이 장치가 저장해야 할 capability 버퍼들을 한꺼번에 미리 할당한다. 정의 drivers/pci/pci.c:2765 — PCIe(PCI_CAP_ID_EXP), PCI-X, LTR 세 가지를 잡고 이어서 pci_allocate_vc_save_buffers() 를 부른다 */
void pci_free_cap_save_buffers(struct pci_dev *dev); /* [한국어] 장치 제거 시 saved_cap_space 리스트를 통째로 kfree 한다. 정의 drivers/pci/pci.c:2787 */
int pci_add_cap_save_buffer(struct pci_dev *dev, char cap, unsigned int size); /* [한국어] 표준 capability 하나에 대한 버퍼를 만든다. 정의 pci.c:2752 → _pci_add_cap_save_buffer(dev, cap, false, size). 해당 capability 가 장치에 없으면(pci_find_capability 가 0) 아무것도 하지 않고 0 을 반환한다(pci.c:2736) */
int pci_add_ext_cap_save_buffer(struct pci_dev *dev,
				u16 cap, unsigned int size); /* [한국어] 확장 capability 판. 정의 pci.c:2758 → _pci_add_cap_save_buffer(..., true, ...) 로 확장 공간에서 찾는다 */
struct pci_cap_saved_state *pci_find_saved_cap(struct pci_dev *dev, char cap); /* [한국어] 표준 capability 의 저장 버퍼를 ID 로 찾아 준다. 정의 pci.c:1281 */
struct pci_cap_saved_state *pci_find_saved_ext_cap(struct pci_dev *dev,
						   u16 cap); /* [한국어] 확장 capability 판. 정의 pci.c:1287 */

/* [한국어] --- 전원 상태 전이 후 지켜야 하는 대기 시간 ---
 * PCI 전원 상태는 D0(완전 동작) → D1 → D2 → D3hot(전원은 들어와 있으나
 * config 접근만 가능) → D3cold(전원 차단) 순으로 깊어진다. 상태를 바꾼 뒤
 * 곧바로 접근하면 장치가 아직 준비되지 않아 응답하지 않으므로, 스펙이
 * 정한 최소 대기 시간을 지켜야 한다.
 * NVMe 와의 구분: NVMe 의 APST(Autonomous Power State Transition)는
 * "NVMe 컨트롤러가 스스로 자기 내부 전력 상태를 바꾸는" NVMe 스펙 기능이고,
 * 여기 D0~D3 는 "PCI 버스가 함수(function) 단위로 전원을 관리하는" PCI 스펙
 * 기능이다. 서로 다른 계층이며 값도 무관하다. */
#define PCI_PM_D2_DELAY         200	/* usec; see PCIe r4.0, sec 5.9.1 */ /* [한국어] D2 상태에 들어가거나 나올 때 200us 를 기다린다. PCIe r4.0 §5.9.1 의 값이며 usec 단위. drivers/pci/pci.c:1073 과 :1188 에서 udelay(PCI_PM_D2_DELAY) 로 지킨다 */
#define PCI_PM_D3HOT_WAIT       10	/* msec */ /* [한국어] D3hot → D0 복귀 후 최소 10ms. 설정자: drivers/pci/pci.c:2482 가 dev->d3hot_delay 의 기본값으로 넣는다(장치별 quirk 가 이 값을 늘릴 수 있다). drivers/pci/pci-acpi.c:1273 은 ACPI 펌웨어가 이보다 짧은 값을 주면 무시한다 */
#define PCI_PM_D3COLD_WAIT      100	/* msec */ /* [한국어] D3cold → D0(즉 전원 재인가) 후 최소 100ms. drivers/pci/pci-acpi.c:1268 이 같은 하한 검사를 하고, 여러 SoC 호스트 브리지가 리셋 지연으로 재사용한다 */

/* [한국어] --- 전원 관리(PM)와 PME 관련 내부 API 묶음 ---
 * 이 블록의 함수들은 세 갈래다.
 *  (1) 상태 추적: pci_update_current_state()/pci_refresh_power_state() 는
 *      pci_dev.current_state 를 하드웨어 실제 값과 맞춘다.
 *  (2) 상태 전이: pci_power_up() 은 D0 로 올리고,
 *      pci_finish_runtime_suspend() 는 런타임 서스펜드를 마무리한다.
 *  (3) PME(Power Management Event): 잠든 장치가 "나 깨워 줘"라고 알리는
 *      메커니즘. pci_check_pme_status() 가 PME_Status 비트를 확인하고
 *      pci_pme_wakeup_bus() 가 버스 아래를 훑는다.
 * NVMe 접점: drivers/nvme/host/pci.c 의 nvme_suspend()/nvme_resume() 는
 * 공개 API(pci_save_state/pci_restore_state 등)만 부르지만, 그 안쪽에서
 * 실제로 D-state 를 바꾸고 되돌리는 것이 이 블록의 함수들이다. */
void pci_update_current_state(struct pci_dev *dev, pci_power_t state); /* [한국어] 하드웨어의 실제 전원 상태를 읽어 dev->current_state 를 갱신한다. 정의 drivers/pci/pci.c:902 */
void pci_refresh_power_state(struct pci_dev *dev); /* [한국어] 플랫폼(ACPI 등)에게 물어 현재 상태를 다시 확인한다. 정의 drivers/pci/pci.c:922 */
int pci_power_up(struct pci_dev *dev); /* [한국어] 장치를 D0 로 끌어올린다. 정의 drivers/pci/pci.c:1027. NVMe 가 부르는 pci_enable_device_mem() 의 안쪽 경로에서 결국 이 함수가 D0 를 보장한다 */
void pci_disable_enabled_device(struct pci_dev *dev); /* [한국어] 이미 enable 된 장치를 disable 한다(중복 disable 방지 로직 포함). 정의 drivers/pci/pci.c:1771 */
int pci_finish_runtime_suspend(struct pci_dev *dev); /* [한국어] 런타임 서스펜드의 마무리 — 웨이크업 설정을 반영하고 목표 D-state 로 내린다. 정의 drivers/pci/pci.c:2153 */
void pcie_clear_device_status(struct pci_dev *dev); /* [한국어] PCIe Device Status 레지스터의 오류 비트를 write-1-to-clear 로 지운다. 정의 drivers/pci/pci.c:1812 */
void pcie_clear_root_pme_status(struct pci_dev *dev); /* [한국어] 루트 포트의 Root Status 에 남은 PME 상태를 지운다. 정의 drivers/pci/pci.c:1822 */
bool pci_check_pme_status(struct pci_dev *dev); /* [한국어] PM Capability 의 PME_Status 비트가 서 있는지(= 이 장치가 깨워 달라고 했는지) 확인한다. 정의 drivers/pci/pci.c:1829 */
void pci_pme_wakeup_bus(struct pci_bus *bus); /* [한국어] 버스 아래 장치들을 훑으며 PME 를 올린 장치를 깨운다. 정의 drivers/pci/pci.c:1872 */
void pci_pme_restore(struct pci_dev *dev); /* [한국어] resume 후 PME 활성화 비트를 원래대로 되돌린다. 정의 drivers/pci/pci.c:1953 */
bool pci_dev_need_resume(struct pci_dev *dev); /* [한국어] 이 장치가 시스템 resume 때 반드시 깨어나야 하는지 판정한다. 정의 drivers/pci/pci.c:2207 */
void pci_dev_adjust_pme(struct pci_dev *dev); /* [한국어] PME 설정이 실제 요구와 어긋나 있으면 맞춰 준다. 정의 drivers/pci/pci.c:2225 */
void pci_dev_complete_resume(struct pci_dev *pci_dev); /* [한국어] resume 완료 처리(런타임 PM 참조 계수 정리 포함). 정의 drivers/pci/pci.c:2240 */
void pci_config_pm_runtime_get(struct pci_dev *dev); /* [한국어] config space 를 건드리기 전에 장치를 런타임 PM 으로 깨워 참조를 잡는다. 정의 drivers/pci/pci.c:2267. 잠든 장치의 config 를 읽으면 all-1 이 나오므로 sysfs 읽기 경로가 이 쌍으로 감싼다 */
void pci_config_pm_runtime_put(struct pci_dev *dev); /* [한국어] 위의 짝 — 참조를 놓아 다시 잠들 수 있게 한다. 정의 drivers/pci/pci.c:2283 */
void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev); /* [한국어] D0 로 올린 뒤 정말 D0 가 되었는지 확인까지 한다. 정의 drivers/pci/pci.c:2449 */
void pci_pm_init(struct pci_dev *dev); /* [한국어] 장치의 PM Capability 를 찾아 pci_dev 의 pm_cap/d3hot_delay 등을 초기화한다. 정의 drivers/pci/pci.c:2457 */
void pci_ea_init(struct pci_dev *dev); /* [한국어] EA(Enhanced Allocation) capability 초기화 — BAR 대신 capability 로 주소를 광고하는 방식. 정의 drivers/pci/pci.c:2690 */
bool pci_ea_fixed_busnrs(struct pci_dev *dev, u8 *sec, u8 *sub); /* [한국어] EA 가 브리지의 버스 번호를 고정해 두었는지 확인한다. 정의 drivers/pci/probe.c:1702 */
void pci_msi_init(struct pci_dev *dev); /* [한국어] MSI capability 위치를 찾아 dev->msi_cap 에 저장한다. 정의 drivers/pci/msi/pcidev_msi.c:10 */
void pci_msix_init(struct pci_dev *dev); /* [한국어] MSI-X capability 위치를 찾아 dev->msix_cap 에 저장한다. 정의 drivers/pci/msi/pcidev_msi.c:34. NVMe 가 pci_alloc_irq_vectors() 로 큐당 벡터를 받으려면 이 값이 먼저 채워져 있어야 한다 */
bool pci_bridge_d3_possible(struct pci_dev *dev); /* [한국어] 이 브리지를 D3 로 내려도 되는지 판정한다. 정의 drivers/pci/pci.c:2327 */
void pci_bridge_d3_update(struct pci_dev *dev); /* [한국어] 하위 장치 상황이 바뀌었을 때 브리지의 bridge_d3 플래그를 다시 계산한다. 정의 drivers/pci/pci.c:2396 */
int pci_bridge_wait_for_secondary_bus(struct pci_dev *dev, char *reset_type); /* [한국어] 리셋/전원 복구 후 하위(secondary) 버스의 장치가 config 요청에 답할 수 있을 때까지 기다린다. 정의 drivers/pci/pci.c:3640. 위 PCIE_RESET_CONFIG_WAIT_MS(100ms) 규칙이 여기서 지켜진다 */

/* [한국어]
 * pci_bus_rrs_vendor_id - 읽어 온 Vendor/Device ID 가 "아직 준비 안 됨" 표식인지 판정
 *
 * @l: config 오프셋 0x00 에서 dword 로 읽은 값. 하위 16비트가 Vendor ID,
 *     상위 16비트가 Device ID 다.
 * @return: Vendor ID 가 0x0001 이면 true(= 장치가 아직 초기화 중), 아니면 false.
 *
 * 왜 필요한가: 리셋 직후나 전원 인가 직후의 장치는 config 요청에 대해
 * RRS(Configuration Request Retry Status, 예전 이름 CRS)를 돌려준다.
 * 루트 포트가 "RRS Software Visibility" 기능을 켜 두면, 하드웨어는 이
 * 재시도 상태를 소프트웨어에게 Vendor ID = 0x0001 (PCI-SIG 가 이 용도로
 * 예약한 값)로 보여 준다. 즉 이 값은 "장치 없음(0xffff)"도 "정상 장치"도
 * 아닌 제3의 상태다. 이 셋을 구분하지 못하면, 커널은 부팅이 느린 장치를
 * "없는 장치"로 오판해 버린다.
 * 실행 컨텍스트: 열거/리셋 대기 루프(프로세스 컨텍스트). 락 없음.
 * 호출 체인:
 *   drivers/pci/probe.c:3165 pci_bus_wait_rrs() → [이 함수] (:3173 조기 판정,
 *     :3181 루프 조건) — 준비될 때까지 지수적으로 늘려 가며 재시도
 *   drivers/pci/pci.c:985 (리셋 후 대기 루프, root->config_rrs_sv 가 참일 때)
 *     → [이 함수]
 * NVMe 접점: 전원 인가 후 초기화가 오래 걸리는 NVMe SSD 가 부팅 시 열거에서
 * 누락되지 않는 것이 이 판정 덕분이다. */
static inline bool pci_bus_rrs_vendor_id(u32 l)
{
	return (l & 0xffff) == PCI_VENDOR_ID_PCI_SIG; /* [한국어] 0x0001 은 PCI-SIG 가 "RRS 응답"을 소프트웨어에 보여 주기 위해 예약한 Vendor ID 다. 0xffff(장치 없음)와 다른 값이라는 점이 핵심. 하위 16비트만 보므로 Device ID 는 무시한다 */
}

/* [한국어]
 * pci_wakeup_event - 이 PCI 장치가 시스템을 깨웠음을 PM 코어에 보고한다
 *
 * @dev: 웨이크업 이벤트를 발생시킨 PCI 장치.
 * @return: 없음.
 *
 * 왜 필요한가: 장치가 PME 로 시스템을 깨운 직후에 곧바로 다시 서스펜드가
 * 진행되면, 깨운 이유(예: 들어온 패킷, 눌린 버튼)를 처리할 틈이 없다.
 * pm_wakeup_event() 에 유예 시간을 주면 그 시간 동안 서스펜드 시도가 막힌다.
 * 실행 컨텍스트: PME 인터럽트 후속 처리 경로. 아래 호출자들이 모두
 * 커널 스레드/워크큐 문맥이다.
 * 호출 체인:
 *   drivers/pci/pci-acpi.c:828/:837 (ACPI 웨이크업 통지) → [이 함수] →
 *     pm_wakeup_event()
 *   drivers/pci/pci.c:1864 (PME 상태를 확인한 뒤) → [이 함수] */
static inline void pci_wakeup_event(struct pci_dev *dev)
{
	/* Wait 100 ms before the system can be put into a sleep state. */
	pm_wakeup_event(&dev->dev, 100); /* [한국어] PM 코어에 웨이크업 이벤트를 등록하고 100ms 동안 서스펜드를 유예한다. 원문 주석이 그 100ms 의 의미를 설명한다 */
}

/* [한국어]
 * pci_bar_index_is_valid - BAR 인덱스가 resource[] 배열 범위 안인지 검사
 *
 * @bar: 검사할 BAR 번호. 보통 유저스페이스나 드라이버가 넘긴 값이라 신뢰할 수 없다.
 * @return: 0 이상 PCI_NUM_RESOURCES 미만이면 true.
 *
 * 왜 필요한가: pci_dev.resource[] 는 고정 크기 배열이고, 인덱스는
 *   0~5   표준 BAR (PCI_STD_RESOURCES ~ PCI_STD_RESOURCE_END)
 *   6     확장 ROM (PCI_ROM_RESOURCE)
 *   7~12  VF BAR (CONFIG_PCI_IOV 일 때만, PCI_IOV_RESOURCES ~ _END)
 *   그 뒤  브리지 윈도우 (PCI_BRIDGE_RESOURCES ~ _END)
 * 로 구획되어 있다. 범위를 벗어난 인덱스로 접근하면 인접 필드를 짓밟는
 * 메모리 손상이 된다. 그래서 외부에서 온 인덱스는 반드시 이 검사를 거친다.
 * 실행 컨텍스트: 인라인 판정만 하므로 어디서든 안전. 락 불필요.
 * 호출 체인:
 *   drivers/pci/devres.c:348/:384/:400/:453/:538/:609 (pcim_* 관리형 BAR API)
 *     → [이 함수] → 거짓이면 -EINVAL 로 거절
 * NVMe 접점: NVMe 는 BAR 0 만 쓰므로 이 검사에 걸릴 일이 없지만,
 * pcim_iomap_regions() 계열을 쓰는 드라이버는 모두 이 문을 통과한다. */
/**
 * pci_bar_index_is_valid - Check whether a BAR index is within valid range
 * @bar: BAR index
 *
 * Protects against overflowing &struct pci_dev.resource array.
 *
 * Return: true for valid index, false otherwise.
 */
static inline bool pci_bar_index_is_valid(int bar)
{
	if (bar >= 0 && bar < PCI_NUM_RESOURCES) /* [한국어] 음수 인덱스(배열 앞을 침범)와 PCI_NUM_RESOURCES 이상(뒤를 침범)을 동시에 막는다. PCI_NUM_RESOURCES 는 <linux/pci.h> 의 리소스 인덱스 enum 마지막 값 */
		return true; /* [한국어] 범위 안 — 사용 가능 */

	return false; /* [한국어] 범위 밖 — 호출자가 거절해야 한다 */
}

/* [한국어]
 * pci_has_subordinate - 이 장치가 하위 버스를 거느린 브리지인지 판정
 *
 * @pci_dev: 검사할 장치.
 * @return: subordinate(하위 pci_bus) 포인터가 있으면 true.
 *
 * 왜 필요한가: 브리지와 엔드포인트는 전원 관리·제거·리소스 계산 규칙이
 * 전혀 다르다. pci_dev.subordinate 는 열거 과정에서 브리지에만 채워지므로,
 * 이 한 줄이 "브리지인가?" 를 판정하는 가장 값싼 방법이다.
 * !!() 로 감싼 것은 포인터를 bool 로 정규화하기 위해서다.
 * 실행 컨텍스트: 순수 판정. 락 없음. 다만 subordinate 는 열거/제거 중
 * 바뀌므로, 그 두 경로와 경쟁할 수 있는 호출자는 pci_rescan_remove_lock 을
 * 이미 쥐고 있어야 한다.
 * 호출 체인:
 *   drivers/pci/pci-driver.c:600/:1051 (서스펜드 경로에서 브리지 예외 처리),
 *   drivers/pci/hotplug/acpiphp_glue.c:552 (하위 트리 disconnect 전파),
 *   그리고 바로 아래 pci_power_manageable() → [이 함수] */
static inline bool pci_has_subordinate(struct pci_dev *pci_dev)
{
	return !!(pci_dev->subordinate); /* [한국어] subordinate 는 "이 브리지 아래에 만들어진 pci_bus" 포인터. 엔드포인트에는 NULL 이다 */
}

/* [한국어]
 * pci_power_manageable - 이 장치를 저전력 D-state 로 내려도 되는지 판정
 *
 * @pci_dev: 검사할 장치.
 * @return: 내려도 되면 true.
 *
 * 판정 규칙(원문 주석 그대로):
 *  - 하위 버스가 없는 보통 장치(엔드포인트)는 언제나 허용한다.
 *  - 브리지는 bridge_d3 플래그가 서 있을 때만 허용한다. 브리지를 D3 로
 *    내리면 그 아래 링크가 끊겨 하위 장치 전체가 접근 불가가 되므로,
 *    "하위 트리 전부가 D3 를 견딜 수 있는가"를 미리 계산해 둔 결과가
 *    bridge_d3 다(계산 주체는 위에서 선언한 pci_bridge_d3_update()).
 * 실행 컨텍스트: 순수 판정. bridge_d3 는 pci_rescan_remove_lock 아래에서
 * 갱신되므로, 락 없이 읽는 이 함수는 "그 시점의 스냅샷"을 본다.
 * 호출 체인:
 *   drivers/pci/pci-driver.c:777 (버스 PM 콜백에서 스킵 여부 결정),
 *   drivers/pci/pci.c:2019, :2387 → [이 함수] → pci_has_subordinate()
 * NVMe 접점: NVMe SSD 는 엔드포인트라 pci_has_subordinate() 가 거짓 →
 * 항상 true 다. 즉 NVMe 장치 자체는 언제나 D3 후보이고, 실제로 내려갈지는
 * 드라이버의 nvme_suspend() 와 상위 브리지 사정이 결정한다. */
static inline bool pci_power_manageable(struct pci_dev *pci_dev)
{
	/*
	 * Currently we allow normal PCI devices and PCI bridges transition
	 * into D3 if their bridge_d3 is set.
	 */
	return !pci_has_subordinate(pci_dev) || pci_dev->bridge_d3; /* [한국어] 엔드포인트(하위 버스 없음)면 무조건 허용, 브리지면 bridge_d3 가 서 있을 때만 허용 */
}

/* [한국어]
 * pcie_downstream_port - 이 장치가 "아래로 링크를 내보내는" 포트인지 판정
 *
 * @dev: 검사할 PCIe 장치.
 * @return: 루트 포트/스위치 다운스트림 포트/PCI-to-PCIe 브리지면 true.
 *
 * 왜 필요한가: PCIe Capability 의 Slot Control/Slot Status, Link Control 같은
 * 레지스터는 "링크를 소유하고 아래에 슬롯을 둘 수 있는 쪽"에만 존재한다.
 * 엔드포인트에서 그 레지스터를 읽으면 의미 없는 값이 나오므로, 접근 전에
 * 이 판정을 거친다.
 * 세 가지 타입의 의미:
 *   PCI_EXP_TYPE_ROOT_PORT   = Root Complex 의 포트(CPU 쪽 최상단)
 *   PCI_EXP_TYPE_DOWNSTREAM  = 스위치의 하향 포트
 *   PCI_EXP_TYPE_PCIE_BRIDGE = PCIe→PCI/PCI-X 브리지 (PCIe 쪽이 위)
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인:
 *   drivers/pci/access.c:338 pcie_cap_has_sltctl() → [이 함수]
 *   drivers/pci/access.c:408/:433 (capability 읽기/쓰기 전 검사)
 *   drivers/pci/pci.c:3678, drivers/pci/pcie/aspm.c:92/:129 → [이 함수]
 * NVMe 접점: NVMe SSD 자신은 PCI_EXP_TYPE_ENDPOINT 라 항상 false 다.
 * true 가 되는 것은 그 SSD 가 꽂힌 상위 루트 포트/스위치 포트이며,
 * AER·DPC·핫플러그·ASPM 정책이 실제로 걸리는 곳이 바로 그 포트다. */
static inline bool pcie_downstream_port(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev); /* [한국어] PCIe Capability 의 Device/Port Type 필드([7:4] of PCI_EXP_FLAGS)를 읽어 온다. PCIe 장치가 아니면 의미 없는 값이므로, 호출자들은 대개 pci_is_pcie() 로 먼저 거른다(예: drivers/pci/access.c:408) */

	return type == PCI_EXP_TYPE_ROOT_PORT || /* [한국어] 루트 포트 — Root Complex 가 내보내는 링크의 상단 */
	       type == PCI_EXP_TYPE_DOWNSTREAM || /* [한국어] 스위치의 다운스트림 포트 */
	       type == PCI_EXP_TYPE_PCIE_BRIDGE; /* [한국어] PCIe→PCI 브리지 — PCIe 쪽이 상위이므로 아래로 링크를 내보내는 쪽에 속한다 */
}

/* [한국어] --- VPD(Vital Product Data) ---
 * VPD 는 capability 를 통해 시리얼 번호·부품 번호 같은 문자열을 노출하는
 * 선택적 기능이다. 데이터가 config space 가 아니라 "주소를 써 넣고 준비
 * 비트를 폴링해서 4바이트씩 읽는" 창을 통해 나오기 때문에 별도 파일
 * (drivers/pci/vpd.c)이 필요하다. */
void pci_vpd_init(struct pci_dev *dev); /* [한국어] VPD capability 를 찾아 접근 창을 초기화한다. 정의 drivers/pci/vpd.c:262 */
extern const struct attribute_group pci_dev_vpd_attr_group; /* [한국어] VPD 내용을 /sys 에 노출하는 속성 그룹. 실체도 drivers/pci/vpd.c 에 있다 */

/* [한국어] --- VC(Virtual Channel) ---
 * VC 는 하나의 물리 링크를 여러 트래픽 클래스로 나눠 QoS 를 주는 확장
 * capability 다. suspend/resume 을 건너뛰면 설정이 날아가므로 저장·복원
 * 함수가 따로 있다. 구현은 drivers/pci/vc.c. */
/* PCI Virtual Channel */
int pci_save_vc_state(struct pci_dev *dev); /* [한국어] 현재 VC 설정을 저장 버퍼에 뜬다. 정의 drivers/pci/vc.c:391 */
void pci_restore_vc_state(struct pci_dev *dev); /* [한국어] resume 시 저장해 둔 VC 설정을 되돌린다. 정의 drivers/pci/vc.c:434 */
void pci_allocate_vc_save_buffers(struct pci_dev *dev); /* [한국어] VC 저장에 필요한 버퍼를 미리 잡는다. 정의 drivers/pci/vc.c:461. 위 pci_allocate_cap_save_buffers() 가 마지막에 이 함수를 부른다 */

/* [한국어] --- /proc/bus/pci 인터페이스 (CONFIG_PROC_FS) ---
 * 레거시 유저스페이스 인터페이스라 CONFIG_PROC_FS 가 꺼지면 통째로 빠진다.
 * 꺼졌을 때의 스텁은 전부 "아무 일도 안 하고 0(성공) 반환" 이다 —
 * /proc 노드를 만들지 못한 것이 장치 등록 실패로 번지면 안 되기 때문이다. */
/* PCI /proc functions */
#ifdef CONFIG_PROC_FS
int pci_proc_attach_device(struct pci_dev *dev); /* [한국어] /proc/bus/pci/<BDF> 노드를 만든다. 정의 drivers/pci/proc.c:426 */
int pci_proc_detach_device(struct pci_dev *dev); /* [한국어] 장치 제거 시 그 노드를 없앤다. 정의 drivers/pci/proc.c:459 */
int pci_proc_detach_bus(struct pci_bus *bus); /* [한국어] 버스 단위 디렉터리를 없앤다. 정의 drivers/pci/proc.c:467 */
#else /* [한국어] CONFIG_PROC_FS 가 꺼진 경우 */
static inline int pci_proc_attach_device(struct pci_dev *dev) { return 0; } /* [한국어] 스텁: /proc 노드를 만들지 않고 성공만 반환 */
static inline int pci_proc_detach_device(struct pci_dev *dev) { return 0; } /* [한국어] 스텁: 지울 것이 없으므로 성공 */
static inline int pci_proc_detach_bus(struct pci_bus *bus) { return 0; } /* [한국어] 스텁: 지울 것이 없으므로 성공 */
#endif /* [한국어] CONFIG_PROC_FS 분기 종료 */

/* [한국어] --- 핫플러그 드라이버가 쓰는 내부 함수 ---
 * drivers/pci/hotplug/ 아래의 pciehp/acpiphp 등이 이 둘을 쓴다. */
/* Functions for PCI Hotplug drivers to use */
int pci_hp_add_bridge(struct pci_dev *dev); /* [한국어] 핫플러그로 새로 나타난 브리지를 버스 트리에 편입한다. 정의 drivers/pci/probe.c:4340 */
bool pci_hp_spurious_link_change(struct pci_dev *pdev); /* [한국어] 링크 상태 변화가 실제 카드 삽입/제거가 아닌 잡음인지 판정한다. 정의 drivers/pci/hotplug/pci_hotplug_core.c:452 */

/* [한국어] --- 레거시 I/O/메모리 공간 sysfs 파일 ---
 * HAVE_PCI_LEGACY 는 아키텍처가 "버스별 레거시 I/O 포트 공간"을 매핑할 수
 * 있을 때만 정의된다(모든 아키텍처가 그렇지는 않다). sysfs 도 함께 켜져
 * 있어야 의미가 있으므로 두 조건의 AND 로 갈린다.
 * 조건이 맞지 않으면 아무 일도 하지 않는 빈 스텁이 쓰인다 — 호출부인
 * 버스 추가/제거 경로가 #ifdef 없이 그대로 컴파일되게 하기 위해서다. */
#if defined(CONFIG_SYSFS) && defined(HAVE_PCI_LEGACY)
void pci_create_legacy_files(struct pci_bus *bus); /* [한국어] 버스별 레거시 I/O 공간을 여는 sysfs 파일을 만든다. 정의 drivers/pci/pci-sysfs.c:926 */
void pci_remove_legacy_files(struct pci_bus *bus); /* [한국어] 그 파일들을 없앤다. 정의 drivers/pci/pci-sysfs.c:978 */
#else /* [한국어] 조건이 맞지 않는 경우 */
static inline void pci_create_legacy_files(struct pci_bus *bus) { }
static inline void pci_remove_legacy_files(struct pci_bus *bus) { }
#endif

/* [한국어] --- PCI 코어의 전역 락 세 개 ---
 * 세 락은 보호 대상과 컨텍스트 제약이 서로 다르다. 이 구분을 놓치면
 * 데드락이나 "잠들 수 없는 문맥에서 잠드는" 버그가 난다. */
/* Lock for read/write access to pci device and bus lists */
extern struct rw_semaphore pci_bus_sem; /* [한국어] 장치 리스트와 버스 리스트를 보호하는 읽기/쓰기 세마포어. 순회는 down_read(drivers/pci/bus.c:425/:437 등), 트리 변경은 down_write 로 잡는다. 세마포어이므로 잠들 수 있는 문맥 전용 */
extern struct mutex pci_slot_mutex; /* [한국어] pci_slot(물리 슬롯) 리스트를 보호하는 뮤텍스. 실체는 drivers/pci/pci.c:32 의 DEFINE_MUTEX, 사용은 pci.c:4423/:4443 의 lock/unlock 쌍 */

/* [한국어] config space 접근 자체를 직렬화하는 raw 스핀락.
 * 실체: drivers/pci/access.c:14 의 DEFINE_RAW_SPINLOCK(pci_lock).
 * 읽는 자/설정자: access.c:26~27 의 pci_lock_config()/pci_unlock_config()
 *   매크로가 raw_spin_lock_irqsave 로 감싸며, 모든 pci_read_config_byte
 *   /word/dword 와 pci_write_config_ 계열이 이 안에서 실행된다.
 * 왜 raw 스핀락인가: 레거시 config 메커니즘 #1 은 "주소 포트(0xcf8)에 쓰고
 *   데이터 포트(0xcfc)에서 읽는" 두 단계라, 두 단계 사이에 다른 CPU 가
 *   끼어들면 엉뚱한 장치를 읽는다. 이 임계구역은 인터럽트 문맥에서도
 *   들어올 수 있고 매우 짧아야 하므로 잠들 수 없는 raw 스핀락을 쓴다.
 * 동기화 주의: 이 락을 쥔 채로는 절대 잠들면 안 된다. */
extern raw_spinlock_t pci_lock;

/* [한국어] D3hot→D0 복귀 대기 시간의 "시스템 전역 하한"(ms).
 * 실체: drivers/pci/pci.c:47.
 * 설정자: drivers/pci/quirks.c:1649 가 특정 브로큰 하드웨어를 만나면
 *   120ms 로 올린다 — 스펙상 10ms 면 되지만 실제로는 더 걸리는 칩이 있어서다.
 * 읽는 자: drivers/pci/pci.c:71 의 max(dev->d3hot_delay, pci_pm_d3hot_delay)
 *   — 장치별 값과 전역 값 중 큰 쪽을 쓴다.
 * 동기화: 부팅 중 quirk 가 한 번 쓰고 이후 읽기 전용. */
extern unsigned int pci_pm_d3hot_delay;

/* [한국어] CONFIG_PCI_MSI 분기. MSI 지원이 컴파일에서 빠지면 "MSI 를 쓰지
 * 말라"는 명령 자체가 무의미하므로 빈 스텁으로 대체된다. */
#ifdef CONFIG_PCI_MSI
void pci_no_msi(void); /* [한국어] MSI 사용을 전역으로 끈다("pci=nomsi"). 정의 drivers/pci/msi/msi.c:878. NVMe 는 MSI-X 를 못 쓰면 큐당 인터럽트를 포기하고 단일 벡터/INTx 로 떨어진다 */
#else /* [한국어] CONFIG_PCI_MSI 가 꺼진 경우 */
static inline void pci_no_msi(void) { } /* [한국어] 스텁: 끌 MSI 자체가 없으므로 아무 일도 하지 않는다 */
#endif /* [한국어] CONFIG_PCI_MSI 분기 종료 */

/* [한국어] "pci=realloc" 커널 파라미터 파서.
 * 정의: drivers/pci/setup-bus.c:1541 (__init).
 * 호출자: drivers/pci/pci.c:5322/:5324 의 부팅 파라미터 처리.
 * 무엇을 켜는가: 펌웨어(BIOS/UEFI)가 배정한 BAR 주소를 그대로 두지 않고
 *   커널이 전부 다시 배정하도록 만든다. 펌웨어가 큰 BAR 를 요구하는 카드에
 *   충분한 공간을 주지 않았을 때의 구제 수단이다.
 * 인자 이름이 없는 선언(char *)인 이유는 헤더에서는 타입만 알면 되기 때문. */
void pci_realloc_get_opt(char *);

/* [한국어]
 * pci_no_d1d2 - 이 장치에 대해 D1/D2 상태 사용을 금지해야 하는지 판정
 *
 * @dev: 검사할 장치.
 * @return: 0 이 아니면 D1/D2 를 쓰지 말아야 한다.
 *
 * 왜 필요한가: D1 과 D2 는 선택적 상태이고, 구현이 부실해 여기서 복귀하지
 * 못하는 하드웨어가 적지 않다. 그런 장치에는 quirk 가 pci_dev.no_d1d2 를
 * 세워 두고, 커널은 D0 와 D3hot 만 오간다.
 * 부모까지 보는 이유: 상위 브리지가 D1/D2 를 제대로 못 하면 그 아래 장치의
 * 전원 상태 전이도 함께 깨진다. 그래서 "나 또는 내 부모 중 하나라도
 * no_d1d2 면 금지"로 판정한다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인:
 *   drivers/pci/pci.c:1213 (요청된 상태가 D1/D2 인지 검사해 거절),
 *   drivers/pci/pci.c:2086, :2489 → [이 함수] */
static inline int pci_no_d1d2(struct pci_dev *dev)
{
	unsigned int parent_dstates = 0; /* [한국어] 부모(상위 브리지)의 no_d1d2 를 담을 변수. 부모가 없으면 0 으로 남는다 */

	if (dev->bus->self) /* [한국어] dev->bus->self 는 이 버스를 만든 상위 브리지. 루트 버스에서는 NULL 이라 검사가 필요하다 */
		parent_dstates = dev->bus->self->no_d1d2; /* [한국어] 부모 브리지의 no_d1d2 플래그를 가져온다 */
	return (dev->no_d1d2 || parent_dstates); /* [한국어] 자신 또는 부모 중 하나라도 금지면 금지. 반환형이 int 라 bool 이 아닌 정수 논리합이 그대로 나간다 */

}

/* [한국어] --- sysfs 속성 그룹 (CONFIG_SYSFS) ---
 * /sys/bus/pci/devices/<BDF>/ 아래에 노출되는 파일들의 정의 묶음이다.
 * CONFIG_SYSFS 가 꺼지면 함수는 빈 스텁으로, 속성 그룹 배열은 NULL 매크로로
 * 대체된다. 배열을 NULL 로 #define 하는 방식이 쓰인 이유는, 이 심볼들이
 * struct bus_type 초기화자 안에서 값으로 쓰이기 때문이다 — 스텁 함수로는
 * 대체할 수 없고 "널 포인터"여야 한다. */
#ifdef CONFIG_SYSFS
int pci_create_sysfs_dev_files(struct pci_dev *pdev); /* [한국어] 장치별 sysfs 파일(config, resource, rom 등)을 만든다. 정의 drivers/pci/pci-sysfs.c:1539 */
void pci_remove_sysfs_dev_files(struct pci_dev *pdev); /* [한국어] 장치 제거 시 그 파일들을 없앤다. 정의 drivers/pci/pci-sysfs.c:1548 */
extern const struct attribute_group *pci_dev_groups[]; /* [한국어] PCI 장치 공통 속성 그룹 배열 — vendor/device/class 같은 기본 파일들 */
extern const struct attribute_group *pci_dev_attr_groups[]; /* [한국어] 드라이버 바인딩 전에도 보여야 하는 속성 그룹 배열 */
extern const struct attribute_group *pcibus_groups[]; /* [한국어] pci_bus 용 속성 그룹 배열 */
extern const struct attribute_group *pci_bus_groups[]; /* [한국어] PCI 버스 타입에 붙는 속성 그룹 배열 */
extern const struct attribute_group pci_doe_sysfs_group; /* [한국어] DOE(Data Object Exchange) 관련 sysfs 그룹. CONFIG_PCI_DOE 와 CONFIG_SYSFS 가 모두 켜졌을 때만 실체가 있다(아래 DOE 블록 참조) */
#else /* [한국어] CONFIG_SYSFS 가 꺼진 경우 */
static inline int pci_create_sysfs_dev_files(struct pci_dev *pdev) { return 0; } /* [한국어] 스텁: 만들 파일이 없으므로 성공만 반환 */
static inline void pci_remove_sysfs_dev_files(struct pci_dev *pdev) { } /* [한국어] 스텁: 지울 파일이 없다 */
#define pci_dev_groups NULL /* [한국어] 속성 그룹 배열을 NULL 로 치환 — 구조체 초기화자 자리에 들어가는 값이라 함수 스텁으로는 대체할 수 없다 */
#define pci_dev_attr_groups NULL /* [한국어] 같은 이유로 NULL */
#define pcibus_groups NULL /* [한국어] 같은 이유로 NULL */
#define pci_bus_groups NULL /* [한국어] 같은 이유로 NULL */
#endif /* [한국어] CONFIG_SYSFS 분기 종료 */

/* [한국어] --- 핫플러그 슬롯에 미리 잡아 둘 여유 공간 크기 ---
 * 빈 핫플러그 슬롯 뒤에 나중에 카드가 꽂힐 것에 대비해, 브리지 윈도우를
 * 실제 필요보다 크게 잡아 둔다. 그렇게 하지 않으면 카드가 꽂힐 때마다
 * 버스 전체 리소스를 재배치해야 하고, 그 사이 동작 중인 장치가 끊긴다.
 * 넷 다 실체는 drivers/pci/pci.c 에 있고(기본값 DEFAULT_HOTPLUG_ 계열 매크로),
 * "pci=hpiosize=", "pci=hpmmiosize=", "pci=hpmmioprefsize=", "pci=hpbussize="
 * 커널 파라미터로 바꿀 수 있으며(drivers/pci/pci.c:5336 이하), 소비자는
 * drivers/pci/setup-bus.c:1212 부근의 additional_ 크기 계산이다. */
extern unsigned long pci_hotplug_io_size; /* [한국어] 핫플러그 슬롯용 예약 I/O 공간 크기. 실체 drivers/pci/pci.c:96 */
extern unsigned long pci_hotplug_mmio_size; /* [한국어] 예약 논-prefetchable MMIO 크기 */
extern unsigned long pci_hotplug_mmio_pref_size; /* [한국어] 예약 prefetchable MMIO 크기 */
extern unsigned long pci_hotplug_bus_size; /* [한국어] 예약할 버스 번호 개수. drivers/pci/pci.c:5347 에서 0xff 를 넘으면 잘라 낸다 — 버스 번호가 8비트이기 때문 */

/* [한국어]
 * pci_is_cardbus_bridge - 이 장치가 CardBus 브리지인지 판정
 *
 * @dev: 검사할 장치.
 * @return: 헤더 타입이 CardBus(0x02)면 true.
 *
 * config space 오프셋 0x0e 의 Header Type 필드는 헤더 레이아웃을 결정한다:
 *   0x00 = 일반 장치(BAR 6개), 0x01 = PCI-to-PCI 브리지,
 *   0x02 = CardBus 브리지. 레이아웃이 다르므로 BAR 개수와 리소스 해석이
 * 전부 달라진다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인: drivers/pci/probe.c:1808 (버스 스캔 중 CardBus 브리지 분기)
 *   → [이 함수]
 * 참고: CardBus 는 노트북 PC Card 시대의 유물이라 오늘날 NVMe 환경과는
 * 무관하다. 이 파일에 남아 있는 이유는 아직 지원 코드가 트리에 있어서다. */
static inline bool pci_is_cardbus_bridge(struct pci_dev *dev)
{
	return dev->hdr_type == PCI_HEADER_TYPE_CARDBUS; /* [한국어] PCI_HEADER_TYPE_CARDBUS = 0x02. hdr_type 은 config 0x0e 에서 읽어 온 값에서 multifunction 비트(0x80)를 뺀 나머지다 */
}
/* [한국어] CONFIG_CARDBUS 분기. 아래 #else 의 스텁들은 각각
 * "정렬 요구 0", "-EOPNOTSUPP", "버스 번호를 그대로 반환(max)", "-ENOENT" 를
 * 돌려주어, CardBus 를 모르는 커널에서도 열거/리소스 코드가 그대로 돌아가게
 * 한다. 특히 pci_cardbus_scan_bridge_extend() 스텁이 인자 max 를 그대로
 * 반환하는 것은 "버스 번호를 하나도 소비하지 않았다"는 뜻이다. */
#ifdef CONFIG_CARDBUS
unsigned long pci_cardbus_resource_alignment(struct resource *res); /* [한국어] CardBus 브리지 윈도우의 정렬 요구를 계산한다. 정의 drivers/pci/setup-cardbus.c:23 */
int pci_bus_size_cardbus_bridge(struct pci_bus *bus, /* [한국어] CardBus 브리지가 필요로 하는 창 크기를 산정한다. 정의 drivers/pci/setup-cardbus.c:32 */
				struct list_head *realloc_head);
int pci_cardbus_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev, /* [한국어] CardBus 브리지 아래를 스캔하며 버스 번호를 소비한다. 정의 drivers/pci/setup-cardbus.c:184 */
				   u32 buses, int max,
				   unsigned int available_buses, int pass);
int pci_setup_cardbus(char *str); /* [한국어] "pci=cbiosize=" 등 CardBus 관련 부팅 파라미터 파서. 정의 drivers/pci/setup-cardbus.c:171 */

#else /* [한국어] CONFIG_CARDBUS 가 꺼진 경우 */
static inline unsigned long pci_cardbus_resource_alignment(struct resource *res) /* [한국어] 스텁 시작 — 정렬 요구 없음 */
{
	return 0;
}
static inline int pci_bus_size_cardbus_bridge(struct pci_bus *bus,
					      struct list_head *realloc_head)
{
	return -EOPNOTSUPP;
}
static inline int pci_cardbus_scan_bridge_extend(struct pci_bus *bus,
						 struct pci_dev *dev,
						 u32 buses, int max,
						 unsigned int available_buses,
						 int pass)
{
	return max;
}
static inline int pci_setup_cardbus(char *str) { return -ENOENT; }
#endif /* CONFIG_CARDBUS */ /* [한국어] CONFIG_CARDBUS 분기 종료 — 원문 주석이 어느 #ifdef 의 짝인지 밝혀 준다 */

/* [한국어]
 * pci_match_one_device - pci_device_id 항목 하나가 이 장치와 맞는지 검사
 *
 * @id: 드라이버가 등록한 ID 표의 한 줄.
 * @dev: 매칭 대상 장치.
 * @return: 맞으면 @id 를 그대로, 아니면 NULL.
 *
 * 왜 필요한가: 커널이 새 PCI 장치를 발견하면 등록된 모든 드라이버의
 * id_table 을 훑어 담당자를 찾는다. 그 한 줄 비교가 이 함수다.
 * 다섯 조건을 모두 만족해야 매칭이다:
 *   vendor / device / subvendor / subdevice — 각각 PCI_ANY_ID(0xffffffff)면
 *     "아무거나"라는 와일드카드이므로 무조건 통과.
 *   class — XOR 후 class_mask 로 마스킹해 0 이어야 한다. 즉 마스크가 1 인
 *     비트들만 정확히 일치하면 되고, class_mask 가 0 이면 클래스는 아예
 *     보지 않는다는 뜻이 된다.
 * 실행 컨텍스트: 장치 등록/드라이버 등록 시 프로세스 컨텍스트. 락 없음
 * (호출자인 pci-driver.c 쪽이 필요한 락을 이미 쥔다).
 * 호출 체인:
 *   drivers/pci/pci-driver.c:80 pci_match_id() → [이 함수]
 *   drivers/pci/pci-driver.c:113 (sysfs new_id 로 동적 추가된 ID) → [이 함수]
 *   drivers/pci/search.c:231 (ID 로 장치 찾기) → [이 함수]
 * NVMe 접점: NVMe 드라이버의 ID 표는 두 층이다. 앞쪽에는 quirk 가 필요한
 * 특정 VID:DID 행들이 있고, 맨 끝(drivers/nvme/host/pci.c:5361)에
 *   { PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) }
 * 라는 포괄 클래스 행이 있다. class_mask 가 0xffffff 이므로 Class Code
 * 24비트 전체가 정확히 NVMe 인터페이스여야 매칭된다. 즉 새로 나온 SSD 도
 * 드라이버 수정 없이 이 마지막 행에 걸려 붙고, quirk 가 필요한 모델만
 * 앞쪽 VID:DID 행이 먼저 잡아 준다. 그 우선순위 판정이 결국 이 함수를
 * 표의 앞에서부터 반복 호출하는 방식으로 이뤄진다. */
/**
 * pci_match_one_device - Tell if a PCI device structure has a matching
 *			  PCI device id structure
 * @id: single PCI device id structure to match
 * @dev: the PCI device structure to match against
 *
 * Returns the matching pci_device_id structure or %NULL if there is no match.
 */
static inline const struct pci_device_id *
pci_match_one_device(const struct pci_device_id *id, const struct pci_dev *dev)
{
	if ((id->vendor == PCI_ANY_ID || id->vendor == dev->vendor) && /* [한국어] PCI_ANY_ID 는 와일드카드. 아니면 config 0x00 의 Vendor ID 와 일치해야 한다 */
	    (id->device == PCI_ANY_ID || id->device == dev->device) && /* [한국어] config 0x02 의 Device ID 비교 */
	    (id->subvendor == PCI_ANY_ID || id->subvendor == dev->subsystem_vendor) && /* [한국어] config 0x2c 의 Subsystem Vendor ID 비교 — 같은 칩을 여러 보드 제조사가 쓸 때 구분하는 값 */
	    (id->subdevice == PCI_ANY_ID || id->subdevice == dev->subsystem_device) && /* [한국어] config 0x2e 의 Subsystem ID 비교 */
	    !((id->class ^ dev->class) & id->class_mask)) /* [한국어] XOR 로 다른 비트만 남긴 뒤 class_mask 로 관심 비트만 본다. 결과가 0 이면 관심 비트가 전부 같다는 뜻. class 는 config 0x09~0x0b 의 Class Code(base/sub/prog-if 3바이트) */
		return id; /* [한국어] 다섯 조건을 모두 통과 — 이 ID 항목이 담당자다 */
	return NULL; /* [한국어] 하나라도 어긋나면 매칭 실패 */
}

/* [한국어] --- 물리 슬롯(struct pci_slot)의 sysfs 지원 ---
 * pci_slot 은 "메인보드의 물리 슬롯"을 나타내는 객체로, 그 슬롯에 꽂힌
 * 장치(pci_dev)와는 별개다. 슬롯은 비어 있을 수도 있고, 하나의 슬롯에
 * 멀티펑션 장치의 여러 함수가 들어갈 수도 있다. */
/* PCI slot sysfs helper code */
#define to_pci_slot(s) container_of(s, struct pci_slot, kobj) /* [한국어] kobject 포인터에서 그것을 품은 struct pci_slot 을 역산한다. sysfs 콜백은 kobject 만 넘겨받기 때문에 필요하다. 사용처: drivers/pci/slot.c */

extern struct kset *pci_slots_kset; /* [한국어] 모든 pci_slot 의 kobject 를 담는 kset(=/sys/bus/pci/slots). drivers/pci/hotplug/acpiphp_ibm.c:324/:372 와 rpadlpar_sysfs.c:99 가 이 kset 의 kobject 를 부모로 삼아 파일을 단다 */

/* [한국어]
 * struct pci_slot_attribute - pci_slot 전용 sysfs 속성 서술자
 *
 * 커널의 일반 sysfs 콜백은 struct kobject 를 받지만, 슬롯 코드는
 * struct pci_slot 을 받고 싶어 한다. 그래서 struct attribute 를 첫 멤버로
 * 품은 확장 구조체를 만들고, 위 to_pci_slot_attr() 로 되돌려 받는다. */
struct pci_slot_attribute {
	struct attribute attr; /* [한국어] 커널 sysfs 가 요구하는 표준 attribute. 반드시 첫 멤버여야 to_pci_slot_attr() 의 container_of 계산이 오프셋 0 으로 단순해진다. 설정자/읽는 자: sysfs 코어. 동기화: 읽기 전용 정적 서술자 */
	ssize_t (*show)(struct pci_slot *, char *); /* [한국어] 슬롯 속성 파일을 read 할 때 불릴 콜백. 설정자: 각 속성 정의부(drivers/pci/slot.c). 읽는 자: slot.c:20 의 sysfs show 디스패처. 값 범위: NULL 이면 읽기 불가 */
	ssize_t (*store)(struct pci_slot *, const char *, size_t); /* [한국어] write 할 때 불릴 콜백. 읽는 자: slot.c:29 의 store 디스패처. 값 범위: NULL 이면 쓰기 불가(읽기 전용 속성) */
};
#define to_pci_slot_attr(s) container_of(s, struct pci_slot_attribute, attr) /* [한국어] struct attribute 포인터에서 이를 품은 pci_slot_attribute 를 역산 */

/* [한국어]
 * enum pci_bar_type - BAR 하나를 어떤 종류로 해석할지 나타내는 값
 *
 * 이 값은 __pci_read_base() 의 @type 인자로 전달되며, "BAR 값을 직접 보고
 * 판단할 것인가(pci_bar_unknown), 아니면 호출자가 이미 아는 종류로
 * 확정할 것인가"를 가른다. 브리지 윈도우나 CardBus 처럼 레이아웃이 고정된
 * 자리를 읽을 때는 후자가 쓰인다.
 *
 * BAR 의 하위 비트가 종류를 알려 준다(PCI 스펙, Base Address Register):
 *   bit0 = 1 → I/O 공간 BAR (나머지는 I/O 포트 주소)
 *   bit0 = 0 → 메모리 공간 BAR
 *     bit[2:1] = 00 → 32비트 주소
 *     bit[2:1] = 10 → 64비트 주소 (BAR 두 칸을 잡아먹는다)
 *     bit3     = 1  → prefetchable (읽기 부작용이 없어 캐시/합병 가능)
 * 그래서 아래 네 값이 필요하다.
 *
 * NVMe 접점: NVMe 컨트롤러의 BAR0 는 64비트 논-prefetchable 메모리 BAR 이며,
 * BAR0/BAR1 두 칸을 함께 써서 64비트 주소를 표현한다. 즉 여기서
 * pci_bar_mem64 로 분류되는 쪽이다. 그 결과가 resource[0] 에 정착하고,
 * drivers/nvme/host/pci.c:3001 의 pci_resource_start(pdev, 0) 이 그 값을 읽는다. */
enum pci_bar_type {
	pci_bar_unknown,	/* Standard PCI BAR probe */ /* [한국어] BAR 값을 아직 해석하지 않은 상태 — __pci_read_base() 가 직접 비트를 보고 종류를 판정하라는 뜻. 일반 BAR 를 읽을 때 쓰는 값이다 */
	pci_bar_io,		/* An I/O port BAR */ /* [한국어] I/O 포트 BAR 로 확정(BAR bit0=1) */
	pci_bar_mem32,		/* A 32-bit memory BAR */ /* [한국어] 32비트 메모리 BAR 로 확정 */
	pci_bar_mem64,		/* A 64-bit memory BAR */ /* [한국어] 64비트 메모리 BAR 로 확정 — 연속한 BAR 두 칸이 하나의 64비트 주소를 이룬다 */
};

/* [한국어] --- 호스트 브리지 device 객체 참조 ---
 * 장치가 어느 호스트 브리지 아래에 있는지 거슬러 올라가 그 struct device 를
 * 얻는다. DMA 마스크나 IOMMU 설정처럼 "버스 전체에 걸린 제약"을 물어볼 때
 * 필요하다. get 은 참조를 올리고 put 은 내린다 — 반드시 짝으로 쓴다. */
struct device *pci_get_host_bridge_device(struct pci_dev *dev); /* [한국어] 이 장치의 호스트 브리지 struct device 를 참조 계수를 올려 얻는다. 정의 drivers/pci/host-bridge.c:31 */
void pci_put_host_bridge_device(struct device *dev); /* [한국어] 위에서 올린 참조를 되돌린다. 정의 drivers/pci/host-bridge.c:42 */

/* [한국어] --- Resizable BAR 와 리소스 재배치 ---
 * 일부 장치는 BAR 크기를 소프트웨어가 바꿀 수 있다(Resizable BAR 확장
 * capability). 크기를 바꾸면 기존 주소 배정이 무효가 되므로, 해제 →
 * 크기 변경 → 재배치의 절차가 필요하다. 아래 함수들이 그 절차를 이룬다. */
void pci_resize_resource_set_size(struct pci_dev *dev, int resno, int size); /* [한국어] Resizable BAR 로 BAR 크기를 바꾼 뒤 resource 의 크기를 그에 맞춰 갱신한다. 정의 drivers/pci/rebar.c:208 */
int pci_do_resource_release_and_resize(struct pci_dev *dev, int resno, int size, /* [한국어] 해당 BAR 를 놓고 크기를 바꾼 뒤 다시 잡는 전체 절차. exclude_bars 는 이 과정에서 건드리면 안 되는 BAR 비트마스크. 정의 drivers/pci/setup-bus.c:2063 */
				       int exclude_bars);
unsigned int pci_rescan_bus_bridge_resize(struct pci_dev *bridge); /* [한국어] 브리지 아래를 재스캔하며 윈도우 크기를 다시 잡는다. 정의 drivers/pci/probe.c:4245 */
int __must_check pci_reassign_resource(struct pci_dev *dev, int i, resource_size_t add_size, resource_size_t align); /* [한국어] 리소스 하나를 더 큰 크기/정렬로 다시 배정한다. __must_check 가 붙은 이유는 실패를 무시하면 장치가 주소 없이 남기 때문 */

/* [한국어] --- 장치 열거의 핵심 진입점들 ---
 * 여기부터는 "config space 를 읽어 struct pci_dev 를 완성하는" 함수들이다.
 * 이 헤더에서 NVMe 독자가 가장 눈여겨볼 부분이기도 하다: NVMe 드라이버가
 * 보는 pci_resource_start(pdev, 0) 값이 만들어지는 자리가 여기다. */
int pci_configure_extended_tags(struct pci_dev *dev, void *ign); /* [한국어] PCIe Device Control 의 Extended Tag Field Enable 을 켠다. 태그 비트가 5비트에서 8비트로 늘어 미완료 요청을 32개에서 256개까지 띄울 수 있다. 정의 drivers/pci/probe.c:2841. NVMe 처럼 깊은 큐로 많은 읽기를 동시에 던지는 장치에서 성능에 직접 영향을 준다 */
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *pl, /* [한국어] devfn 위치에 장치가 있는지 Vendor ID 를 읽어 확인한다. rrs_timeout 은 위 pci_bus_rrs_vendor_id() 판정에 걸렸을 때 얼마나 기다려 줄지(ms). 정의 drivers/pci/probe.c:3236 */
				int rrs_timeout);
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *pl, /* [한국어] 위 함수의 아키텍처 비의존 일반 구현. 아키텍처가 특별한 방법을 갖고 있으면 위 함수만 대체한다. 정의 drivers/pci/probe.c:3214 */
					int rrs_timeout);

int pci_setup_device(struct pci_dev *dev); /* [한국어] config space 를 읽어 struct pci_dev 의 vendor/device/class/hdr_type/BAR 등을 모두 채우는 열거의 본체. 정의 drivers/pci/probe.c:2460 */
void __pci_size_stdbars(struct pci_dev *dev, int count, /* [한국어] 표준 BAR 들의 크기를 한꺼번에 구해 sizes[] 에 담는다. 정의 drivers/pci/probe.c:229 → __pci_size_bars(probe.c:207) */
			unsigned int pos, u32 *sizes);
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type, /* [한국어] sizes[] 로 받은 원시 값과 BAR 원본 값을 조합해 struct resource 하나를 완성한다. 정의 drivers/pci/probe.c:245 */
		    struct resource *res, unsigned int reg, u32 *sizes);
void pci_configure_ari(struct pci_dev *dev); /* [한국어] ARI(Alternative Routing-ID Interpretation)를 켤 수 있으면 켠다. devfn 8비트를 전부 function 번호로 써서 한 장치가 256 함수를 가질 수 있게 하는 기능으로, SR-IOV 로 VF 를 많이 만들 때 필요하다. 정의 drivers/pci/pci.c:2798 */

/* [한국어] --- 브리지 윈도우 크기 산정과 리소스 배정 ---
 * PCI 리소스 할당은 두 번 훑는다.
 *  1) __pci_bus_size_bridges(): 아래에서 위로 올라가며 "이 브리지 아래
 *     장치들이 필요로 하는 총 크기와 정렬"을 계산해 브리지 윈도우 크기를 정한다.
 *  2) __pci_bus_assign_resources(): 위에서 아래로 내려가며 실제 주소를
 *     배정하고 BAR 에 써 넣는다.
 * 배정에 실패한 리소스는 fail_head 리스트에, 나중에 더 줄 수 있는 것은
 * realloc_head 리스트에 모아 두었다가 두 번째 시도에서 다시 다룬다. */
int pci_dev_res_add_to_list(struct list_head *head, struct pci_dev *dev, /* [한국어] 재배정 후보 리소스를 리스트에 추가한다. add_size 는 추가로 더 주고 싶은 크기, min_align 은 최소 정렬 요구. 정의 drivers/pci/setup-bus.c:50 */
			    struct resource *res, resource_size_t add_size,
			    resource_size_t min_align);
void __pci_bus_size_bridges(struct pci_bus *bus, /* [한국어] 브리지 윈도우 크기 산정(아래에서 위로). 정의 drivers/pci/setup-bus.c:1167 */
			struct list_head *realloc_head);
void __pci_bus_assign_resources(const struct pci_bus *bus, /* [한국어] 산정된 크기에 맞춰 실제 주소를 배정(위에서 아래로). 정의 drivers/pci/setup-bus.c:1280 */
				struct list_head *realloc_head,
				struct list_head *fail_head);
bool pci_bus_clip_resource(struct pci_dev *dev, int idx); /* [한국어] 리소스가 상위 창 밖으로 삐져나오면 창 안으로 잘라 낸다. 정의 drivers/pci/bus.c:278 */
void pci_walk_bus_locked(struct pci_bus *top, /* [한국어] pci_bus_sem 을 이미 쥔 상태에서 버스 트리를 순회하며 콜백을 부른다(중복 잠금 방지판). 정의 drivers/pci/bus.c:444 */
			 int (*cb)(struct pci_dev *, void *),
			 void *userdata);

const char *pci_resource_name(struct pci_dev *dev, unsigned int i);
bool pci_resource_is_optional(const struct pci_dev *dev, int resno);
static inline bool pci_resource_is_bridge_win(int resno)
{
	return resno >= PCI_BRIDGE_RESOURCES &&
	       resno <= PCI_BRIDGE_RESOURCE_END;
}

/**
 * pci_resource_num - Reverse lookup resource number from device resources
 * @dev: PCI device
 * @res: Resource to lookup index for (MUST be a @dev's resource)
 *
 * Perform reverse lookup to determine the resource number for @res within
 * @dev resource array. NOTE: The caller is responsible for ensuring @res is
 * among @dev's resources!
 *
 * Returns: resource number.
 */
static inline int pci_resource_num(const struct pci_dev *dev,
				   const struct resource *res)
{
	int resno = res - &dev->resource[0];

	/* Passing a resource that is not among dev's resources? */
	WARN_ON_ONCE(resno >= PCI_NUM_RESOURCES);

	return resno;
}

void pbus_validate_busn(struct pci_bus *bus);
struct resource *pbus_select_window(struct pci_bus *bus,
				    const struct resource *res);
void pci_reassigndev_resource_alignment(struct pci_dev *dev);
void pci_disable_bridge_window(struct pci_dev *dev);
struct pci_bus *pci_bus_get(struct pci_bus *bus);
void pci_bus_put(struct pci_bus *bus);

#define PCIE_LNKCAP_SLS2SPEED(lnkcap)					\
({									\
	u32 lnkcap_sls = (lnkcap) & PCI_EXP_LNKCAP_SLS;			\
									\
	(lnkcap_sls == PCI_EXP_LNKCAP_SLS_64_0GB ? PCIE_SPEED_64_0GT :	\
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_32_0GB ? PCIE_SPEED_32_0GT :	\
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_16_0GB ? PCIE_SPEED_16_0GT :	\
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_8_0GB ? PCIE_SPEED_8_0GT :	\
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_5_0GB ? PCIE_SPEED_5_0GT :	\
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_2_5GB ? PCIE_SPEED_2_5GT :	\
	 PCI_SPEED_UNKNOWN);						\
})

/* PCIe link information from Link Capabilities 2 */
#define PCIE_LNKCAP2_SLS2SPEED(lnkcap2) \
	((lnkcap2) & PCI_EXP_LNKCAP2_SLS_64_0GB ? PCIE_SPEED_64_0GT : \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_32_0GB ? PCIE_SPEED_32_0GT : \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_16_0GB ? PCIE_SPEED_16_0GT : \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_8_0GB ? PCIE_SPEED_8_0GT : \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_5_0GB ? PCIE_SPEED_5_0GT : \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_2_5GB ? PCIE_SPEED_2_5GT : \
	 PCI_SPEED_UNKNOWN)

#define PCIE_LNKCTL2_TLS2SPEED(lnkctl2) \
({									\
	u16 lnkctl2_tls = (lnkctl2) & PCI_EXP_LNKCTL2_TLS;		\
									\
	(lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_64_0GT ? PCIE_SPEED_64_0GT :	\
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_32_0GT ? PCIE_SPEED_32_0GT :	\
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_16_0GT ? PCIE_SPEED_16_0GT :	\
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_8_0GT ? PCIE_SPEED_8_0GT :	\
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_5_0GT ? PCIE_SPEED_5_0GT :	\
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_2_5GT ? PCIE_SPEED_2_5GT :	\
	 PCI_SPEED_UNKNOWN);						\
})

/* PCIe speed to Mb/s reduced by encoding overhead */
#define PCIE_SPEED2MBS_ENC(speed) \
	((speed) == PCIE_SPEED_64_0GT ? 64000*1/1 : \
	 (speed) == PCIE_SPEED_32_0GT ? 32000*128/130 : \
	 (speed) == PCIE_SPEED_16_0GT ? 16000*128/130 : \
	 (speed) == PCIE_SPEED_8_0GT  ?  8000*128/130 : \
	 (speed) == PCIE_SPEED_5_0GT  ?  5000*8/10 : \
	 (speed) == PCIE_SPEED_2_5GT  ?  2500*8/10 : \
	 0)

static inline int pcie_dev_speed_mbps(enum pci_bus_speed speed)
{
	switch (speed) {
	case PCIE_SPEED_2_5GT:
		return 2500;
	case PCIE_SPEED_5_0GT:
		return 5000;
	case PCIE_SPEED_8_0GT:
		return 8000;
	case PCIE_SPEED_16_0GT:
		return 16000;
	case PCIE_SPEED_32_0GT:
		return 32000;
	case PCIE_SPEED_64_0GT:
		return 64000;
	default:
		break;
	}

	return -EINVAL;
}

u8 pcie_get_supported_speeds(struct pci_dev *dev);
const char *pci_speed_string(enum pci_bus_speed speed);
void __pcie_print_link_status(struct pci_dev *dev, bool verbose);
void pcie_report_downtraining(struct pci_dev *dev);

enum pcie_link_change_reason {
	PCIE_LINK_RETRAIN,
	PCIE_ADD_BUS,
	PCIE_BWCTRL_ENABLE,
	PCIE_BWCTRL_IRQ,
	PCIE_HOTPLUG,
};

static inline void __pcie_update_link_speed(struct pci_bus *bus,
					    enum pcie_link_change_reason reason,
					    u16 linksta, u16 linksta2)
{
	bus->cur_bus_speed = pcie_link_speed[linksta & PCI_EXP_LNKSTA_CLS];
	bus->flit_mode = (linksta2 & PCI_EXP_LNKSTA2_FLIT) ? 1 : 0;

	trace_pcie_link_event(bus,
			     reason,
			     FIELD_GET(PCI_EXP_LNKSTA_NLW, linksta),
			     linksta & PCI_EXP_LNKSTA_LINK_STATUS_MASK);
}

void pcie_update_link_speed(struct pci_bus *bus, enum pcie_link_change_reason reason);

/* Single Root I/O Virtualization */
struct pci_sriov {
	int		pos;		/* Capability position */
	int		nres;		/* Number of resources */
	u32		cap;		/* SR-IOV Capabilities */
	u16		ctrl;		/* SR-IOV Control */
	u16		total_VFs;	/* Total VFs associated with the PF */
	u16		initial_VFs;	/* Initial VFs associated with the PF */
	u16		num_VFs;	/* Number of VFs available */
	u16		offset;		/* First VF Routing ID offset */
	u16		stride;		/* Following VF stride */
	u16		vf_device;	/* VF device ID */
	u32		pgsz;		/* Page size for BAR alignment */
	u8		link;		/* Function Dependency Link */
	u8		max_VF_buses;	/* Max buses consumed by VFs */
	u16		driver_max_VFs;	/* Max num VFs driver supports */
	struct pci_dev	*dev;		/* Lowest numbered PF */
	struct pci_dev	*self;		/* This PF */
	u32		class;		/* VF device */
	u8		hdr_type;	/* VF header type */
	u16		subsystem_vendor; /* VF subsystem vendor */
	u16		subsystem_device; /* VF subsystem device */
	resource_size_t	barsz[PCI_SRIOV_NUM_BARS];	/* VF BAR size */
	u16		vf_rebar_cap;	/* VF Resizable BAR capability offset */
	bool		drivers_autoprobe; /* Auto probing of VFs by driver */
};

#ifdef CONFIG_PCI_DOE
void pci_doe_init(struct pci_dev *pdev);
void pci_doe_destroy(struct pci_dev *pdev);
void pci_doe_disconnected(struct pci_dev *pdev);
#else
static inline void pci_doe_init(struct pci_dev *pdev) { }
static inline void pci_doe_destroy(struct pci_dev *pdev) { }
static inline void pci_doe_disconnected(struct pci_dev *pdev) { }
#endif

#ifdef CONFIG_PCI_NPEM
void pci_npem_create(struct pci_dev *dev);
void pci_npem_remove(struct pci_dev *dev);
#else
static inline void pci_npem_create(struct pci_dev *dev) { }
static inline void pci_npem_remove(struct pci_dev *dev) { }
#endif

#if defined(CONFIG_PCI_DOE) && defined(CONFIG_SYSFS)
void pci_doe_sysfs_init(struct pci_dev *pci_dev);
void pci_doe_sysfs_teardown(struct pci_dev *pdev);
#else
static inline void pci_doe_sysfs_init(struct pci_dev *pdev) { }
static inline void pci_doe_sysfs_teardown(struct pci_dev *pdev) { }
#endif

#ifdef CONFIG_PCI_IDE
void pci_ide_init(struct pci_dev *dev);
void pci_ide_init_host_bridge(struct pci_host_bridge *hb);
void pci_ide_destroy(struct pci_dev *dev);
extern const struct attribute_group pci_ide_attr_group;
#else
static inline void pci_ide_init(struct pci_dev *dev) { }
static inline void pci_ide_init_host_bridge(struct pci_host_bridge *hb) { }
static inline void pci_ide_destroy(struct pci_dev *dev) { }
#endif

#ifdef CONFIG_PCI_TSM
void pci_tsm_init(struct pci_dev *pdev);
void pci_tsm_destroy(struct pci_dev *pdev);
extern const struct attribute_group pci_tsm_attr_group;
extern const struct attribute_group pci_tsm_auth_attr_group;
#else
static inline void pci_tsm_init(struct pci_dev *pdev) { }
static inline void pci_tsm_destroy(struct pci_dev *pdev) { }
#endif

/**
 * pci_dev_set_io_state - Set the new error state if possible.
 *
 * @dev: PCI device to set new error_state
 * @new: the state we want dev to be in
 *
 * If the device is experiencing perm_failure, it has to remain in that state.
 * Any other transition is allowed.
 *
 * Returns true if state has been changed to the requested state.
 */
static inline bool pci_dev_set_io_state(struct pci_dev *dev,
					pci_channel_state_t new)
{
	pci_channel_state_t old;

	switch (new) {
	case pci_channel_io_perm_failure:
		xchg(&dev->error_state, pci_channel_io_perm_failure);
		return true;
	case pci_channel_io_frozen:
		old = cmpxchg(&dev->error_state, pci_channel_io_normal,
			      pci_channel_io_frozen);
		return old != pci_channel_io_perm_failure;
	case pci_channel_io_normal:
		old = cmpxchg(&dev->error_state, pci_channel_io_frozen,
			      pci_channel_io_normal);
		return old != pci_channel_io_perm_failure;
	default:
		return false;
	}
}

static inline int pci_dev_set_disconnected(struct pci_dev *dev, void *unused)
{
	pci_dev_set_io_state(dev, pci_channel_io_perm_failure);
	pci_doe_disconnected(dev);

	return 0;
}

/* pci_dev priv_flags */
#define PCI_DEV_ADDED 0
#define PCI_DPC_RECOVERED 1
#define PCI_DPC_RECOVERING 2
#define PCI_DEV_REMOVED 3
#define PCI_LINK_CHANGED 4
#define PCI_LINK_CHANGING 5
#define PCI_LINK_LBMS_SEEN	6
#define PCI_DEV_ALLOW_BINDING 7

static inline void pci_dev_assign_added(struct pci_dev *dev)
{
	smp_mb__before_atomic();
	set_bit(PCI_DEV_ADDED, &dev->priv_flags);
	smp_mb__after_atomic();
}

static inline bool pci_dev_test_and_clear_added(struct pci_dev *dev)
{
	return test_and_clear_bit(PCI_DEV_ADDED, &dev->priv_flags);
}

static inline bool pci_dev_is_added(const struct pci_dev *dev)
{
	return test_bit(PCI_DEV_ADDED, &dev->priv_flags);
}

static inline bool pci_dev_test_and_set_removed(struct pci_dev *dev)
{
	return test_and_set_bit(PCI_DEV_REMOVED, &dev->priv_flags);
}

static inline void pci_dev_allow_binding(struct pci_dev *dev)
{
	set_bit(PCI_DEV_ALLOW_BINDING, &dev->priv_flags);
}

static inline bool pci_dev_binding_disallowed(struct pci_dev *dev)
{
	return !test_bit(PCI_DEV_ALLOW_BINDING, &dev->priv_flags);
}

#ifdef CONFIG_PCIEAER
#include <linux/aer.h>

#define AER_MAX_MULTI_ERR_DEVICES	5	/* Not likely to have more */

/**
 * struct aer_err_info - AER Error Information
 * @dev: Devices reporting error
 * @ratelimit_print: Flag to log or not log the devices' error. 0=NotLog/1=Log
 * @__pad1: Padding for alignment
 * @error_dev_num: Number of devices reporting an error
 * @level: printk level to use in logging
 * @id: Value from register PCI_ERR_ROOT_ERR_SRC
 * @severity: AER severity, 0-UNCOR Non-fatal, 1-UNCOR fatal, 2-COR
 * @root_ratelimit_print: Flag to log or not log the root's error. 0=NotLog/1=Log
 * @multi_error_valid: If multiple errors are reported
 * @first_error: First reported error
 * @__pad2: Padding for alignment
 * @is_cxl: Bus type error: 0-PCI Bus error, 1-CXL Bus error
 * @tlp_header_valid: Indicates if TLP field contains error information
 * @status: COR/UNCOR error status
 * @mask: COR/UNCOR mask
 * @tlp: Transaction packet information
 */
struct aer_err_info {
	struct pci_dev *dev[AER_MAX_MULTI_ERR_DEVICES];
	int ratelimit_print[AER_MAX_MULTI_ERR_DEVICES];
	int error_dev_num;
	const char *level;

	unsigned int id:16;

	unsigned int severity:2;
	unsigned int root_ratelimit_print:1;
	unsigned int __pad1:4;
	unsigned int multi_error_valid:1;

	unsigned int first_error:5;
	unsigned int __pad2:1;
	unsigned int is_cxl:1;
	unsigned int tlp_header_valid:1;

	unsigned int status;
	unsigned int mask;
	struct pcie_tlp_log tlp;
};

int aer_get_device_error_info(struct aer_err_info *info, int i);
void aer_print_error(struct aer_err_info *info, int i);

static inline const char *aer_err_bus(struct aer_err_info *info)
{
	return info->is_cxl ? "CXL" : "PCIe";
}

int pcie_read_tlp_log(struct pci_dev *dev, int where, int where2,
		      unsigned int tlp_len, bool flit,
		      struct pcie_tlp_log *log);
unsigned int aer_tlp_log_len(struct pci_dev *dev, u32 aercc);
void pcie_print_tlp_log(const struct pci_dev *dev,
			const struct pcie_tlp_log *log, const char *level,
			const char *pfx);
#endif	/* CONFIG_PCIEAER */

#ifdef CONFIG_PCIEPORTBUS
/* Cached RCEC Endpoint Association */
struct rcec_ea {
	u8		nextbusn;
	u8		lastbusn;
	u32		bitmap;
};
#endif

#ifdef CONFIG_PCIE_DPC
void pci_save_dpc_state(struct pci_dev *dev);
void pci_restore_dpc_state(struct pci_dev *dev);
void pci_dpc_init(struct pci_dev *pdev);
void dpc_process_error(struct pci_dev *pdev);
pci_ers_result_t dpc_reset_link(struct pci_dev *pdev);
bool pci_dpc_recovered(struct pci_dev *pdev);
unsigned int dpc_tlp_log_len(struct pci_dev *dev);
#else
static inline void pci_save_dpc_state(struct pci_dev *dev) { }
static inline void pci_restore_dpc_state(struct pci_dev *dev) { }
static inline void pci_dpc_init(struct pci_dev *pdev) { }
static inline bool pci_dpc_recovered(struct pci_dev *pdev) { return false; }
#endif

#ifdef CONFIG_PCIEPORTBUS
void pci_rcec_init(struct pci_dev *dev);
void pci_rcec_exit(struct pci_dev *dev);
void pcie_link_rcec(struct pci_dev *rcec);
void pcie_walk_rcec(struct pci_dev *rcec,
		    int (*cb)(struct pci_dev *, void *),
		    void *userdata);
#else
static inline void pci_rcec_init(struct pci_dev *dev) { }
static inline void pci_rcec_exit(struct pci_dev *dev) { }
static inline void pcie_link_rcec(struct pci_dev *rcec) { }
static inline void pcie_walk_rcec(struct pci_dev *rcec,
				  int (*cb)(struct pci_dev *, void *),
				  void *userdata) { }
#endif

#ifdef CONFIG_PCI_ATS
/* Address Translation Service */
void pci_ats_init(struct pci_dev *dev);
void pci_restore_ats_state(struct pci_dev *dev);
#else
static inline void pci_ats_init(struct pci_dev *d) { }
static inline void pci_restore_ats_state(struct pci_dev *dev) { }
#endif /* CONFIG_PCI_ATS */

#ifdef CONFIG_PCI_PRI
void pci_pri_init(struct pci_dev *dev);
void pci_restore_pri_state(struct pci_dev *pdev);
#else
static inline void pci_pri_init(struct pci_dev *dev) { }
static inline void pci_restore_pri_state(struct pci_dev *pdev) { }
#endif

#ifdef CONFIG_PCI_PASID
void pci_pasid_init(struct pci_dev *dev);
void pci_restore_pasid_state(struct pci_dev *pdev);
#else
static inline void pci_pasid_init(struct pci_dev *dev) { }
static inline void pci_restore_pasid_state(struct pci_dev *pdev) { }
#endif

#ifdef CONFIG_PCI_IOV
int pci_iov_init(struct pci_dev *dev);
void pci_iov_release(struct pci_dev *dev);
void pci_iov_remove(struct pci_dev *dev);
void pci_iov_update_resource(struct pci_dev *dev, int resno);
resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev, int resno);
void pci_restore_iov_state(struct pci_dev *dev);
int pci_iov_bus_range(struct pci_bus *bus);
void pci_iov_resource_set_size(struct pci_dev *dev, int resno, int size);
bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev);
static inline u16 pci_iov_vf_rebar_cap(struct pci_dev *dev)
{
	if (!dev->is_physfn)
		return 0;

	return dev->sriov->vf_rebar_cap;
}
static inline bool pci_resource_is_iov(int resno)
{
	return resno >= PCI_IOV_RESOURCES && resno <= PCI_IOV_RESOURCE_END;
}
static inline int pci_resource_num_from_vf_bar(int resno)
{
	return resno + PCI_IOV_RESOURCES;
}
static inline int pci_resource_num_to_vf_bar(int resno)
{
	return resno - PCI_IOV_RESOURCES;
}
extern const struct attribute_group sriov_pf_dev_attr_group;
extern const struct attribute_group sriov_vf_dev_attr_group;
#else
static inline int pci_iov_init(struct pci_dev *dev)
{
	return -ENODEV;
}
static inline void pci_iov_release(struct pci_dev *dev) { }
static inline void pci_iov_remove(struct pci_dev *dev) { }
static inline void pci_iov_update_resource(struct pci_dev *dev, int resno) { }
static inline resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev,
							   int resno)
{
	return 0;
}
static inline void pci_restore_iov_state(struct pci_dev *dev) { }
static inline int pci_iov_bus_range(struct pci_bus *bus)
{
	return 0;
}
static inline void pci_iov_resource_set_size(struct pci_dev *dev, int resno,
					     int size) { }
static inline bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev)
{
	return false;
}
static inline u16 pci_iov_vf_rebar_cap(struct pci_dev *dev)
{
	return 0;
}
static inline bool pci_resource_is_iov(int resno)
{
	return false;
}
static inline int pci_resource_num_from_vf_bar(int resno)
{
	WARN_ON_ONCE(1);
	return -ENODEV;
}
static inline int pci_resource_num_to_vf_bar(int resno)
{
	WARN_ON_ONCE(1);
	return -ENODEV;
}
#endif /* CONFIG_PCI_IOV */

#ifdef CONFIG_PCIE_TPH
void pci_restore_tph_state(struct pci_dev *dev);
void pci_save_tph_state(struct pci_dev *dev);
void pci_no_tph(void);
void pci_tph_init(struct pci_dev *dev);
#else
static inline void pci_restore_tph_state(struct pci_dev *dev) { }
static inline void pci_save_tph_state(struct pci_dev *dev) { }
static inline void pci_no_tph(void) { }
static inline void pci_tph_init(struct pci_dev *dev) { }
#endif

#ifdef CONFIG_PCIE_PTM
void pci_ptm_init(struct pci_dev *dev);
void pci_save_ptm_state(struct pci_dev *dev);
void pci_restore_ptm_state(struct pci_dev *dev);
void pci_suspend_ptm(struct pci_dev *dev);
void pci_resume_ptm(struct pci_dev *dev);
#else
static inline void pci_ptm_init(struct pci_dev *dev) { }
static inline void pci_save_ptm_state(struct pci_dev *dev) { }
static inline void pci_restore_ptm_state(struct pci_dev *dev) { }
static inline void pci_suspend_ptm(struct pci_dev *dev) { }
static inline void pci_resume_ptm(struct pci_dev *dev) { }
#endif

static inline resource_size_t pci_resource_alignment(struct pci_dev *dev,
						     struct resource *res)
{
	int resno = pci_resource_num(dev, res);

	if (pci_resource_is_iov(resno))
		return pci_sriov_resource_alignment(dev, resno);
	if (dev->class >> 8 == PCI_CLASS_BRIDGE_CARDBUS)
		return pci_cardbus_resource_alignment(res);
	return resource_alignment(res);
}

resource_size_t pci_min_window_alignment(struct pci_bus *bus,
					 unsigned long type);

void pci_acs_init(struct pci_dev *dev);
void pci_enable_acs(struct pci_dev *dev);
#ifdef CONFIG_PCI_QUIRKS
int pci_dev_specific_acs_enabled(struct pci_dev *dev, u16 acs_flags);
int pci_dev_specific_enable_acs(struct pci_dev *dev);
int pci_dev_specific_disable_acs_redir(struct pci_dev *dev);
void pci_disable_broken_acs_cap(struct pci_dev *pdev);
int pcie_failed_link_retrain(struct pci_dev *dev);
#else
static inline int pci_dev_specific_acs_enabled(struct pci_dev *dev,
					       u16 acs_flags)
{
	return -ENOTTY;
}
static inline int pci_dev_specific_enable_acs(struct pci_dev *dev)
{
	return -ENOTTY;
}
static inline int pci_dev_specific_disable_acs_redir(struct pci_dev *dev)
{
	return -ENOTTY;
}
static inline void pci_disable_broken_acs_cap(struct pci_dev *dev) { }
static inline int pcie_failed_link_retrain(struct pci_dev *dev)
{
	return -ENOTTY;
}
#endif

/* PCI error reporting and recovery */
pci_ers_result_t pcie_do_recovery(struct pci_dev *dev,
		pci_channel_state_t state,
		pci_ers_result_t (*reset_subordinates)(struct pci_dev *pdev));

bool pcie_wait_for_link(struct pci_dev *pdev, bool active);
int pcie_retrain_link(struct pci_dev *pdev, bool use_lt);

/* ASPM-related functionality we need even without CONFIG_PCIEASPM */
void pci_save_ltr_state(struct pci_dev *dev);
void pci_restore_ltr_state(struct pci_dev *dev);
void pci_configure_aspm_l1ss(struct pci_dev *dev);
void pci_save_aspm_l1ss_state(struct pci_dev *dev);
void pci_restore_aspm_l1ss_state(struct pci_dev *dev);

#ifdef CONFIG_PCIEASPM
void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap);
void pcie_aspm_init_link_state(struct pci_dev *pdev);
void pcie_aspm_exit_link_state(struct pci_dev *pdev);
void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked);
void pcie_aspm_powersave_config_link(struct pci_dev *pdev);
void pci_configure_ltr(struct pci_dev *pdev);
void pci_bridge_reconfigure_ltr(struct pci_dev *pdev);
#else
static inline void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap) { }
static inline void pcie_aspm_init_link_state(struct pci_dev *pdev) { }
static inline void pcie_aspm_exit_link_state(struct pci_dev *pdev) { }
static inline void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked) { }
static inline void pcie_aspm_powersave_config_link(struct pci_dev *pdev) { }
static inline void pci_configure_ltr(struct pci_dev *pdev) { }
static inline void pci_bridge_reconfigure_ltr(struct pci_dev *pdev) { }
#endif

#ifdef CONFIG_PCIE_ECRC
void pcie_set_ecrc_checking(struct pci_dev *dev);
void pcie_ecrc_get_policy(char *str);
#else
static inline void pcie_set_ecrc_checking(struct pci_dev *dev) { }
static inline void pcie_ecrc_get_policy(char *str) { }
#endif

#ifdef CONFIG_PCIEPORTBUS
void pcie_reset_lbms(struct pci_dev *port);
#else
static inline void pcie_reset_lbms(struct pci_dev *port) {}
#endif

struct pci_dev_reset_methods {
	u16 vendor;
	u16 device;
	int (*reset)(struct pci_dev *dev, bool probe);
};

struct pci_reset_fn_method {
	int (*reset_fn)(struct pci_dev *pdev, bool probe);
	char *name;
};
extern const struct pci_reset_fn_method pci_reset_fn_methods[];

#ifdef CONFIG_PCI_QUIRKS
int pci_dev_specific_reset(struct pci_dev *dev, bool probe);
#else
static inline int pci_dev_specific_reset(struct pci_dev *dev, bool probe)
{
	return -ENOTTY;
}
#endif

#if defined(CONFIG_PCI_QUIRKS) && defined(CONFIG_ARM64)
int acpi_get_rc_resources(struct device *dev, const char *hid, u16 segment,
			  struct resource *res);
#else
static inline int acpi_get_rc_resources(struct device *dev, const char *hid,
					u16 segment, struct resource *res)
{
	return -ENODEV;
}
#endif

void pci_rebar_init(struct pci_dev *pdev);
void pci_restore_rebar_state(struct pci_dev *pdev);
int pci_rebar_get_current_size(struct pci_dev *pdev, int bar);
int pci_rebar_set_size(struct pci_dev *pdev, int bar, int size);

struct device_node;

#define PCI_EQ_RESV	0xff

enum equalization_preset_type {
	EQ_PRESET_TYPE_8GTS,
	EQ_PRESET_TYPE_16GTS,
	EQ_PRESET_TYPE_32GTS,
	EQ_PRESET_TYPE_64GTS,
	EQ_PRESET_TYPE_MAX
};

struct pci_eq_presets {
	u16 eq_presets_8gts[MAX_NR_LANES];
	u8 eq_presets_Ngts[EQ_PRESET_TYPE_MAX - 1][MAX_NR_LANES];
};

#ifdef CONFIG_OF
int of_get_pci_domain_nr(struct device_node *node);
int of_pci_get_max_link_speed(struct device_node *node);
u32 of_pci_get_slot_power_limit(struct device_node *node,
				u8 *slot_power_limit_value,
				u8 *slot_power_limit_scale);
bool of_pci_preserve_config(struct device_node *node);
int pci_set_of_node(struct pci_dev *dev);
void pci_release_of_node(struct pci_dev *dev);
void pci_set_bus_of_node(struct pci_bus *bus);
void pci_release_bus_of_node(struct pci_bus *bus);

int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge);
bool of_pci_supply_present(struct device_node *np);
int of_pci_get_equalization_presets(struct device *dev,
				    struct pci_eq_presets *presets,
				    int num_lanes);
#else
static inline int
of_get_pci_domain_nr(struct device_node *node)
{
	return -1;
}

static inline int
of_pci_get_max_link_speed(struct device_node *node)
{
	return -EINVAL;
}

static inline u32
of_pci_get_slot_power_limit(struct device_node *node,
			    u8 *slot_power_limit_value,
			    u8 *slot_power_limit_scale)
{
	if (slot_power_limit_value)
		*slot_power_limit_value = 0;
	if (slot_power_limit_scale)
		*slot_power_limit_scale = 0;
	return 0;
}

static inline bool of_pci_preserve_config(struct device_node *node)
{
	return false;
}

static inline int pci_set_of_node(struct pci_dev *dev) { return 0; }
static inline void pci_release_of_node(struct pci_dev *dev) { }
static inline void pci_set_bus_of_node(struct pci_bus *bus) { }
static inline void pci_release_bus_of_node(struct pci_bus *bus) { }

static inline int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge)
{
	return 0;
}

static inline bool of_pci_supply_present(struct device_node *np)
{
	return false;
}

static inline int of_pci_get_equalization_presets(struct device *dev,
						  struct pci_eq_presets *presets,
						  int num_lanes)
{
	presets->eq_presets_8gts[0] = PCI_EQ_RESV;
	for (int i = 0; i < EQ_PRESET_TYPE_MAX - 1; i++)
		presets->eq_presets_Ngts[i][0] = PCI_EQ_RESV;

	return 0;
}
#endif /* CONFIG_OF */

struct of_changeset;

#ifdef CONFIG_PCI_DYNAMIC_OF_NODES
void of_pci_make_dev_node(struct pci_dev *pdev);
void of_pci_remove_node(struct pci_dev *pdev);
int of_pci_add_properties(struct pci_dev *pdev, struct of_changeset *ocs,
			  struct device_node *np);
void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge);
void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge);
int of_pci_add_host_bridge_properties(struct pci_host_bridge *bridge,
				      struct of_changeset *ocs,
				      struct device_node *np);
#else
static inline void of_pci_make_dev_node(struct pci_dev *pdev) { }
static inline void of_pci_remove_node(struct pci_dev *pdev) { }
static inline void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge) { }
static inline void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge) { }
#endif

#ifdef CONFIG_PCIEAER
void pci_no_aer(void);
void pci_aer_init(struct pci_dev *dev);
void pci_aer_exit(struct pci_dev *dev);
extern const struct attribute_group aer_stats_attr_group;
extern const struct attribute_group aer_attr_group;
void pci_aer_clear_fatal_status(struct pci_dev *dev);
int pci_aer_clear_status(struct pci_dev *dev);
int pci_aer_raw_clear_status(struct pci_dev *dev);
void pci_save_aer_state(struct pci_dev *dev);
void pci_restore_aer_state(struct pci_dev *dev);
#else
static inline void pci_no_aer(void) { }
static inline void pci_aer_init(struct pci_dev *d) { }
static inline void pci_aer_exit(struct pci_dev *d) { }
static inline void pci_aer_clear_fatal_status(struct pci_dev *dev) { }
static inline int pci_aer_clear_status(struct pci_dev *dev) { return -EINVAL; }
static inline int pci_aer_raw_clear_status(struct pci_dev *dev) { return -EINVAL; }
static inline void pci_save_aer_state(struct pci_dev *dev) { }
static inline void pci_restore_aer_state(struct pci_dev *dev) { }
#endif

#ifdef CONFIG_ACPI
bool pci_acpi_preserve_config(struct pci_host_bridge *bridge);
int pci_acpi_program_hp_params(struct pci_dev *dev);
extern const struct attribute_group pci_dev_acpi_attr_group;
void pci_set_acpi_fwnode(struct pci_dev *dev);
int pci_dev_acpi_reset(struct pci_dev *dev, bool probe);
bool acpi_pci_power_manageable(struct pci_dev *dev);
bool acpi_pci_bridge_d3(struct pci_dev *dev);
int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state);
pci_power_t acpi_pci_get_power_state(struct pci_dev *dev);
void acpi_pci_refresh_power_state(struct pci_dev *dev);
int acpi_pci_wakeup(struct pci_dev *dev, bool enable);
bool acpi_pci_need_resume(struct pci_dev *dev);
pci_power_t acpi_pci_choose_state(struct pci_dev *pdev);
#else
static inline bool pci_acpi_preserve_config(struct pci_host_bridge *bridge)
{
	return false;
}
static inline int pci_dev_acpi_reset(struct pci_dev *dev, bool probe)
{
	return -ENOTTY;
}
static inline void pci_set_acpi_fwnode(struct pci_dev *dev) { }
static inline int pci_acpi_program_hp_params(struct pci_dev *dev)
{
	return -ENODEV;
}
static inline bool acpi_pci_power_manageable(struct pci_dev *dev)
{
	return false;
}
static inline bool acpi_pci_bridge_d3(struct pci_dev *dev)
{
	return false;
}
static inline int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	return -ENODEV;
}
static inline pci_power_t acpi_pci_get_power_state(struct pci_dev *dev)
{
	return PCI_UNKNOWN;
}
static inline void acpi_pci_refresh_power_state(struct pci_dev *dev) { }
static inline int acpi_pci_wakeup(struct pci_dev *dev, bool enable)
{
	return -ENODEV;
}
static inline bool acpi_pci_need_resume(struct pci_dev *dev)
{
	return false;
}
static inline pci_power_t acpi_pci_choose_state(struct pci_dev *pdev)
{
	return PCI_POWER_ERROR;
}
#endif

#ifdef CONFIG_PCIEASPM
extern const struct attribute_group aspm_ctrl_attr_group;
#endif

#ifdef CONFIG_X86_INTEL_MID
bool pci_use_mid_pm(void);
int mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state);
pci_power_t mid_pci_get_power_state(struct pci_dev *pdev);
#else
static inline bool pci_use_mid_pm(void)
{
	return false;
}
static inline int mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state)
{
	return -ENODEV;
}
static inline pci_power_t mid_pci_get_power_state(struct pci_dev *pdev)
{
	return PCI_UNKNOWN;
}
#endif

#ifdef CONFIG_PCI_MSI
int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag);
#else
static inline int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	return -ENODEV;
}
#endif

/*
 * Config Address for PCI Configuration Mechanism #1
 *
 * See PCI Local Bus Specification, Revision 3.0,
 * Section 3.2.2.3.2, Figure 3-2, p. 50.
 */

#define PCI_CONF1_BUS_SHIFT	16 /* Bus number */
#define PCI_CONF1_DEV_SHIFT	11 /* Device number */
#define PCI_CONF1_FUNC_SHIFT	8  /* Function number */

#define PCI_CONF1_BUS_MASK	0xff
#define PCI_CONF1_DEV_MASK	0x1f
#define PCI_CONF1_FUNC_MASK	0x7
#define PCI_CONF1_REG_MASK	0xfc /* Limit aligned offset to a maximum of 256B */

#define PCI_CONF1_ENABLE	BIT(31)
#define PCI_CONF1_BUS(x)	(((x) & PCI_CONF1_BUS_MASK) << PCI_CONF1_BUS_SHIFT)
#define PCI_CONF1_DEV(x)	(((x) & PCI_CONF1_DEV_MASK) << PCI_CONF1_DEV_SHIFT)
#define PCI_CONF1_FUNC(x)	(((x) & PCI_CONF1_FUNC_MASK) << PCI_CONF1_FUNC_SHIFT)
#define PCI_CONF1_REG(x)	((x) & PCI_CONF1_REG_MASK)

#define PCI_CONF1_ADDRESS(bus, dev, func, reg) \
	(PCI_CONF1_ENABLE | \
	 PCI_CONF1_BUS(bus) | \
	 PCI_CONF1_DEV(dev) | \
	 PCI_CONF1_FUNC(func) | \
	 PCI_CONF1_REG(reg))

/*
 * Extension of PCI Config Address for accessing extended PCIe registers
 *
 * No standardized specification, but used on lot of non-ECAM-compliant ARM SoCs
 * or on AMD Barcelona and new CPUs. Reserved bits [27:24] of PCI Config Address
 * are used for specifying additional 4 high bits of PCI Express register.
 */

#define PCI_CONF1_EXT_REG_SHIFT	16
#define PCI_CONF1_EXT_REG_MASK	0xf00
#define PCI_CONF1_EXT_REG(x)	(((x) & PCI_CONF1_EXT_REG_MASK) << PCI_CONF1_EXT_REG_SHIFT)

#define PCI_CONF1_EXT_ADDRESS(bus, dev, func, reg) \
	(PCI_CONF1_ADDRESS(bus, dev, func, reg) | \
	 PCI_CONF1_EXT_REG(reg))

#endif /* DRIVERS_PCI_H */
