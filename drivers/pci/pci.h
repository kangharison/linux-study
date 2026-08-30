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
 * pci_dev_set_io_state()·pci_dev_set_disconnected 계열은 AER/DPC
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
 * probe.c(열거·BAR 크기결정), setup-bus.c/setup-res.c(브리지 윈도우 할당),
 * pci.c(전원상태·리셋·capability 탐색), access.c(config space 접근 락),
 * iov.c(SR-IOV), msi/(MSI·MSI-X), pcie/aer.c·dpc.c·err.c(오류 복구),
 * pcie/aspm.c(링크 전력 절감), pci-acpi.c(ACPI _PSx 연동), of.c(디바이스트리),
 * quirks.c(스펙 위반 하드웨어 우회), 그리고 controller/ 아래 각 SoC 호스트
 * 브리지 드라이버들.
 * 데이터 흐름: config space 에서 읽은 원시 값(capability 위치, BAR 크기,
 * 링크 속도)이 이 헤더가 선언한 함수를 통해 struct pci_dev 의 필드와
 * resource[] 배열로 정착하고, 그 뒤 드라이버(예: NVMe)가 공개 API 로
 * 그 결과만 읽어 간다.
 * NVMe 와의 관계: drivers/nvme/host/pci.c 는 이 헤더를 포함하지 않는다
 * (<linux/pci.h> 만 포함한다 — drivers/nvme/host/pci.c 에서 확인).
 * 그러나 NVMe 가 보는 값의 "생산자"가 여기 선언된 함수들이다. 예를 들어
 * NVMe 는 drivers/nvme/host/pci.c 에서
 * ioremap(pci_resource_start(pdev, 0), size) 로 BAR0 를 매핑하고
 * dev->dbs = dev->bar + NVME_REG_DBS 로 도어벨 창을 잡는데, 그
 * pci_resource_start(pdev, 0) 이 돌려주는 resource[0] 을 채우는 것이
 * 이 헤더가 선언한 __pci_read_base()/__pci_size_stdbars() 다.
 *
 * === 주요 함수/구조체 요약 ===
 * - PCI_FIND_NEXT_CAP / PCI_FIND_NEXT_EXT_CAP: config space 의
 * capability 연결 리스트를 TTL 을 걸고 순회하는 매크로. 표준 capability 는
 * 0x34(Capabilities Pointer)에서 시작하는 8비트 링크, 확장 capability 는
 * 0x100 에서 시작하는 12비트 링크다.
 * - struct pci_sriov: SR-IOV PF 한 개의 상태 전부(VF 개수, Routing ID
 * offset/stride, VF BAR 크기 등). drivers/pci/iov.c 가 유일한 관리자다.
 * - struct aer_err_info: AER 인터럽트 한 번에서 긁어 모은 오류 정보
 * (오류 장치 목록, 심각도, 상태 비트, TLP 헤더 로그).
 * - struct pci_cap_saved_state/-_data: suspend 전 capability 레지스터를
 * 통째로 떠 두는 가변 길이 버퍼. pci_save_state() 경로가 쓴다.
 * - pci_dev_set_io_state() / pci_dev_set_disconnected(): 장치의
 * error_state 를 원자적으로 전이시키는 상태 기계. NVMe 의
 * pci_dev_is_disconnected 판정이 읽는 바로 그 값이다.
 * - pci_power_manageable() / pci_no_d1d2() / PCI_PM_D*_WAIT: D0~D3cold
 * 전원 상태 전이 가능 여부와 전이 후 대기 시간을 결정한다.
 * - pci_resource_num() / pci_resource_is_iov() / pci_resource_is_bridge_win()
 * / pci_resource_alignment(): pci_dev.resource[] 배열 인덱스를 해석하는
 * 헬퍼 묶음. BAR·ROM·VF BAR·브리지 윈도우가 한 배열에 섞여 있어 필요하다.
 */
#ifndef DRIVERS_PCI_H /* [한국어] 헤더 중복 포함 방지 가드 시작 — 117개 .c 가 이 파일을 포함하므로 필수 */
#define DRIVERS_PCI_H /* [한국어] 가드 매크로 정의. 이후 재포함 시 본문 전체가 건너뛰어진다 */

#include <linux/align.h> /* [한국어] <linux/align.h> — 아래 PCI_FIND_NEXT_CAP 이 쓰는 ALIGN_DOWN 제공. capability 포인터를 DWORD(4바이트) 경계로 내림 정렬할 때 필요 */
#include <linux/bitfield.h> /* [한국어] <linux/bitfield.h> — FIELD_GET 제공. capability 헤더에서 ID/next 필드를, LNKSTA 에서 링크 폭 필드를 마스크+시프트 없이 뽑을 때 쓴다 */
#include <linux/pci.h> /* [한국어] <linux/pci.h> — 공개 PCI API 헤더. struct pci_dev/pci_bus/pci_driver, pci_power_t, pci_channel_state_t, PCI_NUM_RESOURCES 등 이 파일이 전제하는 모든 기본 타입이 여기서 온다 */
#include <trace/events/pci.h> /* [한국어] <trace/events/pci.h> — 아래 __pcie_update_link_speed 가 호출하는 trace_pcie_link_event 트레이스포인트 정의 */

struct pcie_tlp_log; /* [한국어] struct pcie_tlp_log 전방 선언. 실제 정의는 이 트리에 없는 <linux/aer.h> 에 있고, 여기서는 아래 struct aer_err_info 의 tlp 필드와 pcie_read_tlp_log()/pcie_print_tlp_log() 원형에 쓰이기 때문에 이름만 알려 준다 */

/* Number of possible devfns: 0.0 to 1f.7 inclusive */
#define MAX_NR_DEVFNS 256 /* [한국어] BDF 의 devfn 은 device 5비트 + function 3비트 = 8비트이므로 0x00~0xff, 총 256가지. drivers/pci/pci.c 에서 dma_alias_mask 비트맵 크기로, drivers/pci/search.c 에서 그 비트맵 순회 상한으로 쓰인다 */
#define PCI_MAX_NR_DEVS	32 /* [한국어] 한 버스에 붙을 수 있는 device 번호 개수(5비트 = 32). drivers/pci/probe.c 의 장치 스캔 루프 상한 */

#define MAX_NR_LANES 16 /* [한국어] PCIe 링크 최대 레인 수 가정치. 아래 struct pci_eq_presets 의 레인별 이퀄라이제이션 프리셋 배열 크기로만 쓰인다 */

#define PCI_FIND_CAP_TTL	48 /* [한국어] capability 연결 리스트 순회 TTL(time to live). 링크가 사이클을 이루는 고장/악성 하드웨어에서 무한 루프에 빠지지 않도록 최대 48회로 끊는다. 표준 config space 256B 중 capability 가 놓일 수 있는 영역(0x40~0xff, 192B)을 최소 크기 4B 로 나눈 값 정도의 여유치다. drivers/pci/quirks.c 등에서도 같은 상수를 직접 쓴다 */

#define PCI_VSEC_ID_INTEL_TBT	0x1234	/* Thunderbolt */ /* [한국어] Intel Thunderbolt 컨트롤러가 쓰는 VSEC(Vendor-Specific Extended Capability) ID. drivers/pci/probe.c 에서 pci_find_vsec_capability(dev, PCI_VENDOR_ID_INTEL, ...) 로 이 VSEC 이 있는지 보고 Thunderbolt 장치를 식별한다 */

#define PCIE_LINK_RETRAIN_TIMEOUT_MS	1000 /* [한국어] 링크 재훈련(retrain) 완료를 기다리는 상한(ms). drivers/pci/pci.c 의 pcie_wait_for_link_status() 가 만료 시각을 계산할 때, pcie_wait_for_link_delay() 와 pci_pm_reset() 이 최악의 경우 대기에 쓴다 */

/*
 * Power stable to PERST# inactive.
 *
 * See the "Power Sequencing and Reset Signal Timings" table of the PCI Express
 * Card Electromechanical Specification, Revision 5.1, Section 2.9.2, Symbol
 * "T_PVPERL".
 */
#define PCIE_T_PVPERL_MS		100 /* [한국어] T_PVPERL: 전원이 안정된 뒤 PERST#(리셋) 을 해제하기까지 지켜야 하는 최소 시간 100ms. 위 영문 주석대로 PCIe CEM 스펙 r5.1 §2.9.2 의 값이며, 호스트 브리지 드라이버들이 리셋 시퀀스에서 msleep(PCIE_T_PVPERL_MS) 로 지킨다(drivers/pci/controller/dwc/pci-imx6.c 등) */

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
#define PCIE_PME_TO_L2_TIMEOUT_US	10000 /* [한국어] PME(Power Management Event) 동기화용 대기 상한 10ms. PME_Turn_Off 메시지를 보낸 뒤 링크가 L2 준비 상태가 되기를 기다릴 때 쓰며, PCIe r6.0 §5.3.3.2.1 권고(1~10ms)의 상한을 택했다. drivers/pci/controller/dwc/pci-imx6.c 등에서 usleep_range 상한으로 사용 */

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
#define PCIE_RESET_CONFIG_WAIT_MS	100 /* [한국어] Conventional Reset 이후 첫 Configuration Request 를 보내기 전까지의 최소 대기 100ms. 이 시간을 지키지 않으면 아직 준비되지 않은 장치가 CRS/RRS 를 돌려주거나 응답하지 않는다. drivers/pci/controller/dwc/pcie-designware.c 등에서 msleep 으로 지켜진다 */

/* Parameters for the waiting for link up routine */
/* [한국어] 아래 두 상수는 "링크가 올라올 때까지 기다리는" 폴링 루프의 파라미터다. */
#define PCIE_LINK_WAIT_MAX_RETRIES	10 /* [한국어] 링크 업 폴링 최대 반복 횟수. drivers/pci/controller/dwc/pcie-designware.c 의 for 루프 상한 */
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
#define PCIE_TLP_TYPE_CFG0_RD		0x04 /* Config Type 0 Read Request */ /* [한국어] Type=0x04, Config Type 0 읽기. drivers/pci/controller/pcie-aspeed.c 이 config 읽기 TLP 를 조립할 때 사용 */
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
#define PCIE_MSG_TYPE_R_LOCAL	4 /* [한국어] r=4: Local — Terminate at Receiver. 수신 포트에서 소비되고 더 전달되지 않는다. drivers/pci/controller/cadence/pcie-cadence-ep.c 에서 사용 */
#define PCIE_MSG_TYPE_R_GATHER	5 /* [한국어] r=5: Gathered and routed to Root Complex — 하위에서 모아 RC 로 올림 */

/* [한국어] --- Power Management Message 의 Message Code ---
 * 시스템이 서스펜드로 들어갈 때 상위 포트가 하위로 PME_Turn_Off 를 보내고,
 * 하위는 PME_TO_Ack 로 답한 뒤 링크를 L2/L3 로 내린다. */
/* Power Management Messages; PCIe r6.0, sec 2.2.8.2 */
#define PCIE_MSG_CODE_PME_TURN_OFF	0x19 /* [한국어] Message Code 0x19 = PME_Turn_Off. drivers/pci/controller/dwc/pcie-designware-host.c 에서 서스펜드 시 이 코드로 메시지를 만들어 보낸다 */

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
#define PCIE_CPL_STS_SUCCESS		0x00 /* Successful Completion */ /* [한국어] Completion Status = 0b000 (Successful Completion). drivers/pci/controller/pcie-aspeed.c 은 하드웨어가 잡아 둔 완료 상태가 이 값이 아니면 config 접근 실패로 처리한다 */

/* [한국어] --- struct pci_bus.resource[] 배열의 인덱스 ---
 * 주의: 아래 세 상수는 "버스"의 리소스 배열 인덱스이지,
 * struct pci_dev.resource[] 의 인덱스(PCI_BRIDGE_RESOURCES 계열)가 아니다.
 * P2P 브리지는 자기 아래 버스로 내려보낼 주소 범위를 세 종류(IO 창,
 * 논-prefetchable MEM 창, prefetchable MEM 창)로 광고하며, 그 세 창이
 * 하위 pci_bus 의 resource[0..2] 에 그대로 실린다.
 * 설정자: drivers/pci/probe.c (브리지 config 에서 읽어 채움).
 * 읽는 자: drivers/pci/setup-bus.c (pci_bus_resource_n() 으로 꺼내
 * 하위 장치들의 BAR 를 이 창 안에 배치). */
#define PCI_BUS_BRIDGE_IO_WINDOW	0 /* [한국어] bus->resource[0] = 브리지의 I/O 창 (config 오프셋 0x1c/0x1d + 0x30/0x32 의 I/O Base/Limit) */
#define PCI_BUS_BRIDGE_MEM_WINDOW	1 /* [한국어] bus->resource[1] = 논-prefetchable 메모리 창 (config 0x20/0x22 의 Memory Base/Limit) */
#define PCI_BUS_BRIDGE_PREF_MEM_WINDOW	2 /* [한국어] bus->resource[2] = prefetchable 메모리 창 (config 0x24/0x26 + 0x28/0x2c 의 64비트 확장). NVMe 카드가 큰 BAR 를 요구하면 이 창의 크기가 문제가 된다 */

/* [한국어] AER 을 켤 때 Device Control 레지스터(PCI_EXP_DEVCTL)에 세울 4개 비트 묶음:
 * CERE = Correctable Error Reporting Enable
 * NFERE = Non-Fatal Error Reporting Enable
 * FERE = Fatal Error Reporting Enable
 * URRE = Unsupported Request Reporting Enable
 * 이 비트들이 서야 장치가 오류를 상위로 보고(ERR_COR/ERR_NONFATAL/ERR_FATAL
 * 메시지)한다.
 * 설정자: drivers/pci/pcie/aer.c 의
 * pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS).
 * 읽는 자: drivers/pci/pci-acpi.c 은 ACPI _HPX 가 준 hotplug 파라미터가
 * 이 네 비트를 건드리지 못하도록 마스크로 쓴다(펌웨어보다 커널 정책 우선).
 * NVMe 접점: 이 비트가 서 있어야 NVMe 컨트롤러의 PCIe 오류가 AER 로 올라오고,
 * 그 결과 drivers/nvme/host/pci.c 의 .error_detected 콜백이 불린다. */
#define PCI_EXP_AER_FLAGS	(PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE | \
				 PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE)

/* [한국어] 링크 속도 코드(LNKSTA 의 Current Link Speed 필드 값)를
 * enum pci_bus_speed 로 바꾸는 변환 표.
 * 설정자: drivers/pci/probe.c 에서 배열 리터럴로 정의되고 곧이어
 * EXPORT_SYMBOL_GPL 로 내보내진다(내부 헤더에 선언되어 있지만 심볼 자체는
 * 외부 모듈에도 열려 있다).
 * 읽는 자: drivers/pci/pci-sysfs.c 의 current_link_speed_show(),
 * drivers/pci/probe.c 의 pci_set_bus_speed(), 그리고 아래 __pcie_update_link_speed().
 * 값 범위: 인덱스는 LNKSTA & PCI_EXP_LNKSTA_CLS (4비트).
 * 동기화: const 배열이라 읽기 전용 — 락 불필요. */
extern const unsigned char pcie_link_speed[];
/* [한국어] pcie_link_speed[] 를 범위 검사와 함께 감싼 접근자.
 * 정의는 drivers/pci/probe.c 의 pcie_get_link_speed() 이며, speed 가
 * ARRAY_SIZE(pcie_link_speed) 이상이면 PCI_SPEED_UNKNOWN 을 돌려준다. */
unsigned char pcie_get_link_speed(unsigned int speed);

/* [한국어] "pci=early_dump" 커널 파라미터로 켜지는 디버그 플래그.
 * 설정자: drivers/pci/pci.c 의 pci_setup()(부팅 파라미터 파서). 실체(변수 정의)도
 *   같은 파일에 있다.
 * 읽는 자: drivers/pci/probe.c 의 pci_setup_device() — 참이면 config
 * space 256바이트를 통째로 덤프해 커널 로그에 찍는다.
 * 값 범위: true/false.
 * 동기화: 부팅 초기에 한 번만 쓰이고 이후 읽기 전용이라 락 없음. */
extern bool pci_early_dump;

/* [한국어] 버스 재스캔(rescan)과 장치 제거(remove)를 상호 배제하는 전역 뮤텍스.
 * 실체: drivers/pci/probe.c 의 DEFINE_MUTEX(pci_rescan_remove_lock).
 * 설정자/읽는 자: probe.c 의 pci_lock_rescan_remove()/
 * pci_unlock_rescan_remove() 쌍. remove.c 의 pci_stop_and_remove_bus_device() 는
 * lockdep_assert_held() 로 "이 락을 쥐고 들어와야 한다"를 강제한다.
 * 왜 필요한가: 열거가 pci_dev 를 만들어 버스 리스트에 넣는 동안 다른
 * 스레드(핫플러그 이벤트, sysfs remove)가 같은 장치를 지우면 use-after-free 가
 * 된다. 두 작업을 한 줄로 세우는 것이 이 락의 유일한 목적이다.
 * 동기화: 뮤텍스이므로 잠들 수 있는 프로세스 컨텍스트에서만 잡을 수 있다. */
extern struct mutex pci_rescan_remove_lock;

/* [한국어] 아래 세 함수는 "이 장치의 PCIe Capability 구조에 그 레지스터가
 * 실제로 존재하는가"를 판정한다. PCIe Capability 는 장치 타입(엔드포인트,
 * 루트 포트, 스위치 업/다운스트림 포트, RCiEP ...)과 Capability Version 에
 * 따라 일부 레지스터가 아예 없다. 없는 레지스터를 읽으면 쓰레기 값이
 * 나오므로, pcie_capability_read_word() 계열이 접근 전에 이 판정을 거친다.
 * 셋 다 정의는 drivers/pci/access.c 에 있다. */
bool pcie_cap_has_lnkctl(const struct pci_dev *dev); /* [한국어] Link Control 레지스터는 링크를 가진 장치 타입에만 존재한다 — access.c 이 엔드포인트/레거시 엔드포인트/루트 포트/스위치 상하향 포트/PCI 브리지/PCIe 브리지 여섯 타입만 참으로 본다 */
bool pcie_cap_has_lnkctl2(const struct pci_dev *dev); /* [한국어] Link Control 2 는 Capability Version 이 2 이상일 때만 존재한다 — access.c 가 lnkctl 존재 여부 AND 버전>1 로 판정 */
bool pcie_cap_has_rtctl(const struct pci_dev *dev); /* [한국어] Root Control 은 루트 포트(PCI_EXP_TYPE_ROOT_PORT)와 RC Event Collector(RC_EC)에만 있다 — access.c */

/* [한국어]
 * PCI_FIND_NEXT_CAP - 표준(레거시) capability 연결 리스트를 순회해 원하는 ID 를 찾는다
 *
 * @read_cfg: config 읽기 함수의 "이름 접두사". 매크로 안에서 read_cfg##_byte() /
 * read_cfg##_word 로 토큰 결합되므로, 실제로는 함수 포인터가 아니라
 * "pci_bus_read_config" 같은 접두사 문자열이다. 이렇게 만든 이유는
 * 아직 struct pci_dev 가 없는 열거 초기(버스+devfn 만 아는 시점)와,
 * pci_dev 도 pci_bus 도 없는 호스트 브리지 드라이버(자기만의 config
 * 접근 함수를 가진 controller/ 드라이버)가 같은 순회 로직을 공유하기
 * 위해서다.
 * @start: 순회를 시작할 config space 오프셋. 보통 0x34(PCI_CAPABILITY_LIST).
 * @cap: 찾을 Capability ID (예: PCI_CAP_ID_EXP=0x10, PCI_CAP_ID_MSIX=0x11).
 * @prev_ptr: NULL 이 아니면 "찾은 capability 바로 앞 capability 의 위치"를 여기에
 * 써 준다. 리스트에서 항목을 떼어낼 때(next 포인터를 고쳐 써야 할 때) 쓴다.
 * @args: read_cfg 함수의 앞쪽 인자들 (예: bus, devfn 또는 컨트롤러 포인터).
 * @return(): 찾았으면 그 capability 의 config space 오프셋, 못 찾으면 0.
 * 0 은 유효한 capability 위치가 될 수 없으므로(표준 헤더 영역이다)
 * "없음" 표시로 안전하다.
 *
 * 왜 필요한가: PCI 표준 헤더(0x00~0x3f)를 넘어가는 기능들은 전부
 * "capability" 라는 단일 연결 리스트로 광고된다. 리스트 각 항목은
 * +0: Capability ID (1바이트)
 * +1: Next Capability Pointer (1바이트, 0 이면 끝)
 * 구조이고, 리스트의 머리는 config 오프셋 0x34 에 있다. 이 매크로가 그
 * 링크를 따라가며 원하는 ID 를 찾는다.
 *
 * 라인별 보충 — 매크로 연속행의 백슬래시 바로 앞에 주석을 넣으면 원본 코드
 * 텍스트가 바뀌므로, 아래 한 줄만 여기에 따로 적는다:
 * `*(u8 *)prev_ptr = __prev_pos;`
 * prev_ptr 을 u8 * 로 캐스팅해 쓰는 이유는 표준 capability 의 Next Capability
 * Pointer 가 1바이트이기 때문이다. 호출자는 u8 변수의 주소를 넘겨야 한다.
 *
 * 실행 컨텍스트: 문장식(statement expression, GCC 확장 ({ ... }))이라
 * 호출한 자리에 그대로 펼쳐진다. 별도 락을 잡지 않고, config 읽기 함수
 * 내부의 pci_lock 에만 의존한다. 열거 중(프로세스 컨텍스트)에 불린다.
 *
 * 호출 체인:
 * drivers/pci/pci.c __pci_find_next_cap_ttl 계열 → [이 매크로] →
 * pci_bus_read_config_byte()/word → 버스 ops → 호스트 브리지 하드웨어
 * drivers/pci/controller/dwc/pcie-designware.c 의 dw_pcie_find_capability() →
 * [이 매크로] → dw_pcie_read_cfg
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
 * @start: 시작 오프셋. 0 을 주면 확장 리스트의 규정 시작점인 0x100
 * (PCI_CFG_SPACE_SIZE)에서 시작한다. 0 이 아니면 그 위치의 "다음"부터
 * 이어서 찾는 재개(resume) 검색이다.
 * @cap: 찾을 확장 Capability ID (예: PCI_EXT_CAP_ID_ERR=0x01 AER,
 * PCI_EXT_CAP_ID_SRIOV=0x10, PCI_EXT_CAP_ID_DPC=0x1d).
 * @prev_ptr: NULL 이 아니면 직전 항목의 오프셋을 u16 으로 써 준다.
 * @args: read_cfg 의 선행 인자들.
 * @return(): 찾은 오프셋, 못 찾으면 0.
 *
 * 표준 capability 와의 차이 세 가지가 이 매크로의 존재 이유다:
 * (1) 위치: 표준은 config 0x40~0xff, 확장은 0x100~0xfff (총 4KB). 확장 영역은
 * 레거시 config 사이클로는 닿지 않고 ECAM(memory-mapped config) 또는
 * 확장 config 메커니즘이 있어야 읽힌다.
 * (2) 헤더 크기: 확장 capability 헤더는 4바이트(1 DWORD)이고
 * [15:0]=Capability ID, [19:16]=Capability Version,
 * [31:20]=Next Capability Offset 이다. 그래서 dword 로 읽고
 * PCI_EXT_CAP_ID/PCI_EXT_CAP_NEXT 로 쪼갠다.
 * (3) 오류 판정: 표준 쪽은 0xff 를, 여기서는 "헤더 전체가 0" 을 리스트 끝/부재로 본다.
 *
 * 라인별 보충 — `*(u16 *)prev_ptr = __prev_pos;`
 * 여기서는 u16 * 로 캐스팅한다. 확장 capability 의 Next Offset 은 12비트라
 * 1바이트로는 표현할 수 없기 때문이다(표준 쪽의 u8 캐스팅과 대비된다).
 *
 * 라인별 보충 — 아래 408행
 * `if (PCI_EXT_CAP_ID(__header) == (cap) && __pos != start)`
 * 은 매크로 연속행 백슬래시 바로 앞에 주석을 넣으면 코드 텍스트가 바뀌어
 * 여기에 적는다: ID 가 일치해도 __pos 가 @start 와 같으면 건너뛴다. 재개 검색
 * (start != 0)에서 "출발점 자기 자신"을 다시 찾아 무한히 제자리를 돌지 않게
 * 하려는 방어다.
 *
 * 실행 컨텍스트: 문장식이라 호출 지점에 인라인 전개된다. 락 없음.
 *
 * 호출 체인 — @read_cfg 로 넘어온 접두사에 _dword 가 토큰 결합되어 실제 읽기
 * 함수가 정해진다는 점이 핵심이다:
 * drivers/pci/pci.c 의 pci_find_next_ext_capability() 계열은 접두사
 * pci_bus_read_config
 * 를 넘기므로 [이 매크로] 안에서 read_cfg##_dword 가
 * pci_bus_read_config_dword
 * 로 결합되어 그 함수가 불리고,
 * drivers/pci/controller/dwc/pcie-designware.c 의 dw_pcie_find_ext_capability() 와
 * drivers/pci/controller/dwc/pcie-designware-ep.c 의
 * dw_pcie_ep_find_ext_capability() 는 접두사
 * dw_pcie_read_cfg
 * 를 넘기므로 [이 매크로] → dw_pcie_read_cfg_dword()
 * (drivers/pci/controller/dwc/pcie-designware.h 에 정의)가 불린다.
 *
 * NVMe 접점: drivers/nvme/host/pci.c 이 .sriov_configure 로 등록한
 * pci_sriov_configure_simple() 이 동작하려면 먼저 SR-IOV 확장 capability 를
 * 찾아야 하는데, 그 탐색이 drivers/pci/iov.c 의
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
		__pos = PCI_EXT_CAP_NEXT(__header); /* [한국어] PCI_EXT_CAP_NEXT 는 헤더 [31:20] 을 꺼낸다. 다음 항목이 없으면 0 이 되어 while 조건에서 걸린다 */ \
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
 * 읽는 자: pci_mmap_fits(drivers/pci/mmap.c) 가 이 값에 따라 vma 의
 * pgoff 를 "리소스 시작으로부터의 오프셋"으로 볼지 "절대 물리 페이지 번호"로
 * 볼지 결정한다.
 * 동기화: 값 전달용 열거형이라 공유 상태가 없다. */
enum pci_mmap_api {
	/* [한국어] sysfs 경로: /sys/bus/pci/devices/<BDF>/resource<N> 를 mmap 하는 경우. pgoff 가 "BAR
 * 시작으로부터의 오프셋"이다 */
	PCI_MMAP_SYSFS,	/* mmap on /sys/bus/pci/devices/<BDF>/resource<N> */
	PCI_MMAP_PROCFS	/* mmap on /proc/bus/pci/<BDF> */ /* [한국어] procfs 경로: /proc/bus/pci/<BDF> 를 mmap 하는 경우. 레거시 인터페이스라 주소 해석 규칙이 다르다 */
}; /* [한국어] enum 정의 종료 */
/* [한국어]
 * pci_mmap_fits() - 유저가 요청한 mmap 범위가 해당 BAR 안에 들어오는지 검사
 *
 * @pdev: 대상 PCI 장치.
 * @resno: BAR 번호(resource[] 인덱스).
 * @vmai: 유저가 요청한 가상 메모리 영역 — 시작(pgoff)과 길이를 여기서 본다.
 * @mmap_api: 요청이 sysfs 경로인지 procfs 경로인지 (위 enum).
 * @return(): 범위 안이면 1(참), 벗어나면 0. 호출자는 0 이면 -EINVAL 로 거절한다.
 *
 * 왜 필요한가: 유저스페이스가 /sys/.../resourceN 이나 /proc/bus/pci/ 를 통해
 * 장치 레지스터를 직접 매핑할 수 있는데, 검사 없이 허용하면 그 BAR 를 넘어
 * 다른 장치의 MMIO 나 시스템 메모리까지 매핑될 수 있다. 이 함수가 그 경계
 * 검사를 담당한다.
 * 실행 컨텍스트: 유저 프로세스의 mmap(2) 시스템 호출 문맥(프로세스 컨텍스트).
 * 호출 체인:
 * sys_mmap → pci_mmap_resource(drivers/pci/pci-sysfs.c) → [pci_mmap_fits()]
 * sys_mmap → proc_bus_pci_mmap(drivers/pci/proc.c) → [pci_mmap_fits()]
 * 정의: drivers/pci/mmap.c */
int pci_mmap_fits(struct pci_dev *pdev, int resno, struct vm_area_struct *vmai,
		  enum pci_mmap_api mmap_api);

/* [한국어] --- 장치/버스 리셋 관련 내부 API 묶음 ---
 * PCI 에는 여러 층위의 리셋이 있고, 커널은 "가능한 것 중 가장 덜 파괴적인
 * 것"을 골라 쓴다. 순서는 대략
 * ACPI _RST → 장치 특화 리셋(quirk) → FLR(Function Level Reset) →
 * PM D3hot→D0 왕복 → 상위 브리지의 Secondary Bus Reset(SBR)
 * 이며, 그 후보 목록이 pci_dev.reset_methods[] 에 담긴다.
 * NVMe 접점: drivers/nvme/host/pci.c 는 컨트롤러가 응답하지 않을 때
 * pcie_reset_flr(pdev, false) 로 FLR 을 직접 건다. FLR 은 아래 목록 중
 * 하나이며, 그 지원 여부 판정이 pci_init_reset_methods() 에서 이뤄진다. */
bool pci_reset_supported(struct pci_dev *dev); /* [한국어] 이 장치에 쓸 수 있는 리셋 방법이 하나라도 있는지. 정의는 drivers/pci/pci.c 이며 dev->reset_methods[0] != 0 을 본다 — 배열 첫 칸이 0 이면 후보가 하나도 없다는 뜻 */
void pci_init_reset_methods(struct pci_dev *dev); /* [한국어] 장치별로 가능한 리셋 방법을 조사해 dev->reset_methods[] 를 우선순위 순으로 채운다. 정의 drivers/pci/pci.c, 채우는 부분은, 끝을 0 으로 막는 부분은 */
int pci_bridge_secondary_bus_reset(struct pci_dev *dev); /* [한국어] 상위 브리지의 Secondary Bus Reset 비트(브리지 config 0x3e Bridge Control 의 BUS_RESET)를 토글해 하위 버스 전체를 리셋한다. 링크 아래 모든 장치가 함께 리셋되므로 가장 파괴적이다. 정의 drivers/pci/pci.c */
int pci_bus_error_reset(struct pci_dev *dev); /* [한국어] 버스 단위 오류 복구용 리셋. 정의 drivers/pci/pci.c */
int pci_try_reset_bridge(struct pci_dev *bridge); /* [한국어] 브리지에 대해 리셋을 시도한다. 정의 drivers/pci/pci.c */

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
	/* [한국어] capability ID. 표준이면 8비트 ID(예: PCI_CAP_ID_EXP=0x10), 확장이면 16비트 ID(예:
 * PCI_EXT_CAP_ID_LTR)라 u16 으로 통일. 설정자: drivers/pci/pci.c. 읽는 자:
 * pci.c 의 _pci_find_saved_cap() 비교와 pci_load_saved_state() 의 상태 복사.
	 * 동기화: 장치 열거 시 한 번 쓰고 이후
 * 읽기 전용 */
	u16		cap_nr;
	/* [한국어] 이 항목이 확장 capability 인지(true) 표준 capability 인지(false). 같은 숫자 ID 라도 두 공간이 별개라서
 * ID 만으로는 구분되지 않기 때문에 필요하다. 설정자: pci.c. 읽는 자: pci.c 의 비교식
 * tmp->cap.cap_extended == extended */
	bool		cap_extended;
	/* [한국어] data[] 의 바이트 크기. 설정자: pci.c 의 _pci_add_cap_save_buffer()
	 * (호출자가 넘긴 size 그대로). 읽는 자: pci.c 의 pci_store_saved_state() 가 직렬화
	 * 크기를 계산할 때, pci_load_saved_state() 가 크기 일치를 검증하고 memcpy 길이로
	 * 쓸 때. 값 범위: capability 종류마다
 * 고정(예: LTR 은 2*sizeof(u16)) */
	unsigned int	size;
	/* [한국어] 가변 길이 배열(flexible array member) — 실제 레지스터 값이 여기 담긴다. 설정자/읽는 자:
 * pci.c 가 (u16 *) 로 캐스팅해 PCIe capability 레지스터들을 넣고 뺀다.
 * 할당은 pci.c 의 kzalloc(sizeof(*save_state) + size) 로 껍데기와 한 덩어리로 이뤄진다 */
	u32		data[];
};

/* [한국어]
 * struct pci_cap_saved_state - 위 스냅샷을 pci_dev 의 해시 리스트에 매다는 껍데기
 *
 * 한 장치가 저장해야 할 capability 는 여러 개(PCIe, PCI-X, LTR, VC ...)이고
 * 개수가 장치마다 다르므로, 고정 배열이 아니라 연결 리스트로 관리한다. */
struct pci_cap_saved_state {
	/* [한국어] 해시 리스트 연결 고리. 설정자: pci.c 의 hlist_add_head(&new_cap->next,
 * &pci_dev->saved_cap_space). 읽는 자: pci.c 의
 * hlist_for_each_entry 순회. 동기화: 장치 열거/제거 시점에만 변경되고 그 사이에는 읽기만 하므로 별도 락이 없다 */
	struct hlist_node		next;
	/* [한국어] 실제 스냅샷 몸통. 마지막 멤버여야 하는 이유는 안쪽 data[] 가 가변 길이라서다 — 뒤에 다른 필드를 두면 배열이 그 필드를 덮는다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	struct pci_cap_saved_data	cap;
};

/* [한국어] --- capability 스냅샷 버퍼의 생명주기 API ---
 * 할당은 장치 열거 직후 한 번(pci_allocate_cap_save_buffers()), 해제는 장치
 * 제거 시(pci_free_cap_save_buffers()) 이뤄진다. 실제 레지스터 값을 채우고
 * 되돌리는 것은 pci_save_state()/pci_restore_state() 쪽이며, 그때 아래
 * pci_find_saved_cap()/pci_find_saved_ext_cap() 으로 해당 버퍼를 찾는다.
 * 미리 할당해 두는 이유: suspend 경로는 메모리 할당이 실패하면 안 되는
 * 구간이라, 잠들기 직전이 아니라 여유 있는 열거 시점에 미리 잡아 둔다. */
void pci_allocate_cap_save_buffers(struct pci_dev *dev); /* [한국어] 이 장치가 저장해야 할 capability 버퍼들을 한꺼번에 미리 할당한다. 정의 drivers/pci/pci.c — PCIe(PCI_CAP_ID_EXP), PCI-X, LTR 세 가지를 잡고 이어서 pci_allocate_vc_save_buffers() 를 부른다 */
void pci_free_cap_save_buffers(struct pci_dev *dev); /* [한국어] 장치 제거 시 saved_cap_space 리스트를 통째로 kfree 한다. 정의 drivers/pci/pci.c */
int pci_add_cap_save_buffer(struct pci_dev *dev, char cap, unsigned int size); /* [한국어] 표준 capability 하나에 대한 버퍼를 만든다. 정의는 pci.c 의 pci_add_cap_save_buffer() 이며 _pci_add_cap_save_buffer(dev, cap, false, size) 를 부른다. 해당 capability 가 장치에 없으면(pci_find_capability() 가 0) 아무것도 하지 않고 0 을 반환한다(pci.c) */
int pci_add_ext_cap_save_buffer(struct pci_dev *dev,
				u16 cap, unsigned int size); /* [한국어] 확장 capability 판. 정의는 pci.c 의 pci_add_ext_cap_save_buffer() 이며 _pci_add_cap_save_buffer(..., true, ...) 로 확장 공간에서 찾는다 */
struct pci_cap_saved_state *pci_find_saved_cap(struct pci_dev *dev, char cap); /* [한국어] 표준 capability 의 저장 버퍼를 ID 로 찾아 준다. 정의는 drivers/pci/pci.c 의 pci_find_saved_cap() 이며 내부적으로 _pci_find_saved_cap() 을 부른다 */
struct pci_cap_saved_state *pci_find_saved_ext_cap(struct pci_dev *dev,
						   u16 cap); /* [한국어] 확장 capability 판. 정의는 drivers/pci/pci.c 의 pci_find_saved_ext_cap() */

/* [한국어] --- 전원 상태 전이 후 지켜야 하는 대기 시간 ---
 * PCI 전원 상태는 D0(완전 동작) → D1 → D2 → D3hot(전원은 들어와 있으나
 * config 접근만 가능) → D3cold(전원 차단) 순으로 깊어진다. 상태를 바꾼 뒤
 * 곧바로 접근하면 장치가 아직 준비되지 않아 응답하지 않으므로, 스펙이
 * 정한 최소 대기 시간을 지켜야 한다.
 * NVMe 와의 구분: NVMe 의 APST(Autonomous Power State Transition)는
 * "NVMe 컨트롤러가 스스로 자기 내부 전력 상태를 바꾸는" NVMe 스펙 기능이고,
 * 여기 D0~D3 는 "PCI 버스가 함수(function) 단위로 전원을 관리하는" PCI 스펙
 * 기능이다. 서로 다른 계층이며 값도 무관하다. */
#define PCI_PM_D2_DELAY         200	/* usec; see PCIe r4.0, sec 5.9.1 */ /* [한국어] D2 상태에 들어가거나 나올 때 200us 를 기다린다. PCIe r4.0 §5.9.1 의 값이며 usec 단위. drivers/pci/pci.c 과 에서 udelay(PCI_PM_D2_DELAY) 로 지킨다 */
#define PCI_PM_D3HOT_WAIT       10	/* msec */ /* [한국어] D3hot → D0 복귀 후 최소 10ms. 설정자: drivers/pci/pci.c 가 dev->d3hot_delay 의 기본값으로 넣는다(장치별 quirk 가 이 값을 늘릴 수 있다). drivers/pci/pci-acpi.c 은 ACPI 펌웨어가 이보다 짧은 값을 주면 무시한다 */
#define PCI_PM_D3COLD_WAIT      100	/* msec */ /* [한국어] D3cold → D0(즉 전원 재인가) 후 최소 100ms. drivers/pci/pci-acpi.c 이 같은 하한 검사를 하고, 여러 SoC 호스트 브리지가 리셋 지연으로 재사용한다 */

/* [한국어] --- 전원 관리(PM)와 PME 관련 내부 API 묶음 ---
 * 이 블록의 함수들은 세 갈래다.
 * (1) 상태 추적: pci_update_current_state()/pci_refresh_power_state() 는
 * pci_dev.current_state 를 하드웨어 실제 값과 맞춘다.
 * (2) 상태 전이: pci_power_up() 은 D0 로 올리고,
 * pci_finish_runtime_suspend() 는 런타임 서스펜드를 마무리한다.
 * (3) PME(Power Management Event): 잠든 장치가 "나 깨워 줘"라고 알리는
 * 메커니즘. pci_check_pme_status() 가 PME_Status 비트를 확인하고
 * pci_pme_wakeup_bus() 가 버스 아래를 훑는다.
 * NVMe 접점: drivers/nvme/host/pci.c 의 nvme_suspend() 와 nvme_resume() 은
 * 공개 API(pci_save_state()/pci_restore_state() 등)만 부르지만, 그 안쪽에서
 * 실제로 D-state 를 바꾸고 되돌리는 것이 이 블록의 함수들이다. */
void pci_update_current_state(struct pci_dev *dev, pci_power_t state); /* [한국어] 하드웨어의 실제 전원 상태를 읽어 dev->current_state 를 갱신한다. 정의 drivers/pci/pci.c */
void pci_refresh_power_state(struct pci_dev *dev); /* [한국어] 플랫폼(ACPI 등)에게 물어 현재 상태를 다시 확인한다. 정의 drivers/pci/pci.c */
int pci_power_up(struct pci_dev *dev); /* [한국어] 장치를 D0 로 끌어올린다. 정의 drivers/pci/pci.c. NVMe 가 부르는 pci_enable_device_mem() 의 안쪽 경로에서 결국 이 함수가 D0 를 보장한다 */
void pci_disable_enabled_device(struct pci_dev *dev); /* [한국어] 이미 enable 된 장치를 disable 한다(중복 disable 방지 로직 포함). 정의 drivers/pci/pci.c */
int pci_finish_runtime_suspend(struct pci_dev *dev); /* [한국어] 런타임 서스펜드의 마무리 — 웨이크업 설정을 반영하고 목표 D-state 로 내린다. 정의 drivers/pci/pci.c */
void pcie_clear_device_status(struct pci_dev *dev); /* [한국어] PCIe Device Status 레지스터의 오류 비트를 write-1-to-clear 로 지운다. 정의 drivers/pci/pci.c */
void pcie_clear_root_pme_status(struct pci_dev *dev); /* [한국어] 루트 포트의 Root Status 에 남은 PME 상태를 지운다. 정의 drivers/pci/pci.c */
bool pci_check_pme_status(struct pci_dev *dev); /* [한국어] PM Capability 의 PME_Status 비트가 서 있는지(= 이 장치가 깨워 달라고 했는지) 확인한다. 정의 drivers/pci/pci.c */
void pci_pme_wakeup_bus(struct pci_bus *bus); /* [한국어] 버스 아래 장치들을 훑으며 PME 를 올린 장치를 깨운다. 정의 drivers/pci/pci.c */
void pci_pme_restore(struct pci_dev *dev); /* [한국어] resume 후 PME 활성화 비트를 원래대로 되돌린다. 정의 drivers/pci/pci.c */
bool pci_dev_need_resume(struct pci_dev *dev); /* [한국어] 이 장치가 시스템 resume 때 반드시 깨어나야 하는지 판정한다. 정의 drivers/pci/pci.c */
void pci_dev_adjust_pme(struct pci_dev *dev); /* [한국어] PME 설정이 실제 요구와 어긋나 있으면 맞춰 준다. 정의 drivers/pci/pci.c */
void pci_dev_complete_resume(struct pci_dev *pci_dev); /* [한국어] resume 완료 처리(런타임 PM 참조 계수 정리 포함). 정의 drivers/pci/pci.c */
void pci_config_pm_runtime_get(struct pci_dev *dev); /* [한국어] config space 를 건드리기 전에 장치를 런타임 PM 으로 깨워 참조를 잡는다. 정의 drivers/pci/pci.c. 잠든 장치의 config 를 읽으면 all-1 이 나오므로 sysfs 읽기 경로가 이 쌍으로 감싼다 */
void pci_config_pm_runtime_put(struct pci_dev *dev); /* [한국어] 위의 짝 — 참조를 놓아 다시 잠들 수 있게 한다. 정의 drivers/pci/pci.c */
void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev); /* [한국어] D0 로 올린 뒤 정말 D0 가 되었는지 확인까지 한다. 정의 drivers/pci/pci.c */
void pci_pm_init(struct pci_dev *dev); /* [한국어] 장치의 PM Capability 를 찾아 pci_dev 의 pm_cap/d3hot_delay 등을 초기화한다. 정의 drivers/pci/pci.c */
void pci_ea_init(struct pci_dev *dev); /* [한국어] EA(Enhanced Allocation) capability 초기화 — BAR 대신 capability 로 주소를 광고하는 방식. 정의 drivers/pci/pci.c */
bool pci_ea_fixed_busnrs(struct pci_dev *dev, u8 *sec, u8 *sub); /* [한국어] EA 가 브리지의 버스 번호를 고정해 두었는지 확인한다. 정의 drivers/pci/probe.c */
void pci_msi_init(struct pci_dev *dev); /* [한국어] MSI capability 위치를 찾아 dev->msi_cap 에 저장한다. 정의 drivers/pci/msi/pcidev_msi.c */
void pci_msix_init(struct pci_dev *dev); /* [한국어] MSI-X capability 위치를 찾아 dev->msix_cap 에 저장한다. 정의 drivers/pci/msi/pcidev_msi.c. NVMe 가 pci_alloc_irq_vectors() 로 큐당 벡터를 받으려면 이 값이 먼저 채워져 있어야 한다 */
bool pci_bridge_d3_possible(struct pci_dev *dev); /* [한국어] 이 브리지를 D3 로 내려도 되는지 판정한다. 정의 drivers/pci/pci.c */
void pci_bridge_d3_update(struct pci_dev *dev); /* [한국어] 하위 장치 상황이 바뀌었을 때 브리지의 bridge_d3 플래그를 다시 계산한다. 정의 drivers/pci/pci.c */
int pci_bridge_wait_for_secondary_bus(struct pci_dev *dev, char *reset_type); /* [한국어] 리셋/전원 복구 후 하위(secondary) 버스의 장치가 config 요청에 답할 수 있을 때까지 기다린다. 정의 drivers/pci/pci.c. 위 PCIE_RESET_CONFIG_WAIT_MS(100ms) 규칙이 여기서 지켜진다 */

/* [한국어]
 * pci_bus_rrs_vendor_id() - 읽어 온 Vendor/Device ID 가 "아직 준비 안 됨" 표식인지 판정
 *
 * @l: config 오프셋 0x00 에서 dword 로 읽은 값. 하위 16비트가 Vendor ID,
 * 상위 16비트가 Device ID 다.
 * @return(): Vendor ID 가 0x0001 이면 true(= 장치가 아직 초기화 중), 아니면 false.
 *
 * 왜 필요한가: 리셋 직후나 전원 인가 직후의 장치는 config 요청에 대해
 * RRS(Configuration Request Retry Status, 예전 이름 CRS)를 돌려준다.
 * 루트 포트가 "RRS Software Visibility" 기능을 켜 두면, 하드웨어는 이
 * 재시도 상태를 소프트웨어에게 Vendor ID = 0x0001 (PCI-SIG 가 이 용도로
 * 예약한 값)로 보여 준다. 즉 이 값은 "장치 없음(0xffff)"도 "정상 장치"도
 * 아닌 제3의 상태다. 이 셋을 구분하지 못하면, 커널은 부팅이 느린 장치를
 * "없는 장치"로 오판해 버린다.
 * drivers/pci/probe.c 의 pci_bus_wait_rrs() → [이 함수] — 진입 시 한 번 검사하고,
 * 이어서 while 루프 조건으로 다시 검사하며 준비될 때까지 지수적으로 대기를 늘려 재시도
 * drivers/pci/pci.c 의 pci_dev_wait()(리셋 후 대기 루프, root->config_rrs_sv 가 참일 때)
 * 루프 조건) — 준비될 때까지 지수적으로 늘려 가며 재시도
 * drivers/pci/pci.c (리셋 후 대기 루프, root->config_rrs_sv 가 참일 때)
 * → [이 함수]
 * NVMe 접점: 전원 인가 후 초기화가 오래 걸리는 NVMe SSD 가 부팅 시 열거에서
 * 누락되지 않는 것이 이 판정 덕분이다. */
static inline bool pci_bus_rrs_vendor_id(u32 l)
{
	return (l & 0xffff) == PCI_VENDOR_ID_PCI_SIG; /* [한국어] 0x0001 은 PCI-SIG 가 "RRS 응답"을 소프트웨어에 보여 주기 위해 예약한 Vendor ID 다. 0xffff(장치 없음)와 다른 값이라는 점이 핵심. 하위 16비트만 보므로 Device ID 는 무시한다 */
}

/* [한국어]
 * pci_wakeup_event() - 이 PCI 장치가 시스템을 깨웠음을 PM 코어에 보고한다
 *
 * @dev: 웨이크업 이벤트를 발생시킨 PCI 장치.
 * @return(): 없음.
 *
 * 왜 필요한가: 장치가 PME 로 시스템을 깨운 직후에 곧바로 다시 서스펜드가
 * 진행되면, 깨운 이유(예: 들어온 패킷, 눌린 버튼)를 처리할 틈이 없다.
 * pm_wakeup_event 에 유예 시간을 주면 그 시간 동안 서스펜드 시도가 막힌다.
 * 실행 컨텍스트: PME 인터럽트 후속 처리 경로. 아래 호출자들이 모두
 * 커널 스레드/워크큐 문맥이다.
 * 호출 체인:
 * drivers/pci/pci-acpi.c (ACPI 웨이크업 통지) → [이 함수] →
 * pm_wakeup_event()
 * drivers/pci/pci.c (PME 상태를 확인한 뒤) → [이 함수] */
static inline void pci_wakeup_event(struct pci_dev *dev)
{
	/* Wait 100 ms before the system can be put into a sleep state. */
	pm_wakeup_event(&dev->dev, 100); /* [한국어] PM 코어에 웨이크업 이벤트를 등록하고 100ms 동안 서스펜드를 유예한다. 원문 주석이 그 100ms 의 의미를 설명한다 */
}

/* [한국어]
 * pci_bar_index_is_valid() - BAR 인덱스가 resource[] 배열 범위 안인지 검사
 *
 * @bar: 검사할 BAR 번호. 보통 유저스페이스나 드라이버가 넘긴 값이라 신뢰할 수 없다.
 * @return(): 0 이상 PCI_NUM_RESOURCES 미만이면 true.
 *
 * 왜 필요한가: pci_dev.resource[] 는 고정 크기 배열이고, 인덱스는
 * 0~5 표준 BAR (PCI_STD_RESOURCES ~ PCI_STD_RESOURCE_END)
 * 6 확장 ROM (PCI_ROM_RESOURCE)
 * 7~12 VF BAR (CONFIG_PCI_IOV 일 때만, PCI_IOV_RESOURCES ~ _END)
 * 그 뒤 브리지 윈도우 (PCI_BRIDGE_RESOURCES ~ _END)
 * 로 구획되어 있다. 범위를 벗어난 인덱스로 접근하면 인접 필드를 짓밟는
 * 메모리 손상이 된다. 그래서 외부에서 온 인덱스는 반드시 이 검사를 거친다.
 * 실행 컨텍스트: 인라인 판정만 하므로 어디서든 안전. 락 불필요.
 * 호출 체인:
 * drivers/pci/devres.c (pcim_* 관리형 BAR API)
 * → [이 함수] → 거짓이면 -EINVAL 로 거절
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
 * pci_has_subordinate() - 이 장치가 하위 버스를 거느린 브리지인지 판정
 *
 * @pci_dev: 검사할 장치.
 * @return(): subordinate(하위 pci_bus) 포인터가 있으면 true.
 *
 * 왜 필요한가: 브리지와 엔드포인트는 전원 관리·제거·리소스 계산 규칙이
 * 전혀 다르다. pci_dev.subordinate 는 열거 과정에서 브리지에만 채워지므로,
 * 이 한 줄이 "브리지인가?" 를 판정하는 가장 값싼 방법이다.
 * !! 로 감싼 것은 포인터를 bool 로 정규화하기 위해서다.
 * 실행 컨텍스트: 순수 판정. 락 없음. 다만 subordinate 는 열거/제거 중
 * 바뀌므로, 그 두 경로와 경쟁할 수 있는 호출자는 pci_rescan_remove_lock 을
 * 이미 쥐고 있어야 한다.
 * 호출 체인:
 * drivers/pci/pci-driver.c (서스펜드 경로에서 브리지 예외 처리),
 * drivers/pci/hotplug/acpiphp_glue.c (하위 트리 disconnect 전파),
 * 그리고 바로 아래 pci_power_manageable() → [이 함수] */
static inline bool pci_has_subordinate(struct pci_dev *pci_dev)
{
	return !!(pci_dev->subordinate); /* [한국어] subordinate 는 "이 브리지 아래에 만들어진 pci_bus" 포인터. 엔드포인트에는 NULL 이다 */
}

/* [한국어]
 * pci_power_manageable() - 이 장치를 저전력 D-state 로 내려도 되는지 판정
 *
 * @pci_dev: 검사할 장치.
 * @return(): 내려도 되면 true.
 *
 * 판정 규칙(원문 주석 그대로):
 * - 하위 버스가 없는 보통 장치(엔드포인트)는 언제나 허용한다.
 * - 브리지는 bridge_d3 플래그가 서 있을 때만 허용한다. 브리지를 D3 로
 * 내리면 그 아래 링크가 끊겨 하위 장치 전체가 접근 불가가 되므로,
 * "하위 트리 전부가 D3 를 견딜 수 있는가"를 미리 계산해 둔 결과가
 * bridge_d3 다(계산 주체는 위에서 선언한 pci_bridge_d3_update()).
 * 실행 컨텍스트: 순수 판정. bridge_d3 는 pci_rescan_remove_lock 아래에서
 * 갱신되므로, 락 없이 읽는 이 함수는 "그 시점의 스냅샷"을 본다.
 * 호출 체인:
 * drivers/pci/pci-driver.c (버스 PM 콜백에서 스킵 여부 결정),
 * drivers/pci/pci.c 의 pci_target_state() 와 pci_dev_check_d3cold()
 * → [이 함수] → pci_has_subordinate()
 * NVMe 접점: NVMe SSD 는 엔드포인트라 pci_has_subordinate 가 거짓 →
 * 항상 true 다. 즉 NVMe 장치 자체는 언제나 D3 후보이고, 실제로 내려갈지는
 * 드라이버의 nvme_suspend 와 상위 브리지 사정이 결정한다. */
static inline bool pci_power_manageable(struct pci_dev *pci_dev)
{
	/*
	 * Currently we allow normal PCI devices and PCI bridges transition
	 * into D3 if their bridge_d3 is set.
	 */
	return !pci_has_subordinate(pci_dev) || pci_dev->bridge_d3; /* [한국어] 엔드포인트(하위 버스 없음)면 무조건 허용, 브리지면 bridge_d3 가 서 있을 때만 허용 */
}

/* [한국어]
 * pcie_downstream_port() - 이 장치가 "아래로 링크를 내보내는" 포트인지 판정
 *
 * @dev: 검사할 PCIe 장치.
 * @return(): 루트 포트/스위치 다운스트림 포트/PCI-to-PCIe 브리지면 true.
 *
 * 왜 필요한가: PCIe Capability 의 Slot Control/Slot Status, Link Control 같은
 * 레지스터는 "링크를 소유하고 아래에 슬롯을 둘 수 있는 쪽"에만 존재한다.
 * 엔드포인트에서 그 레지스터를 읽으면 의미 없는 값이 나오므로, 접근 전에
 * 이 판정을 거친다.
 * 세 가지 타입의 의미:
 * PCI_EXP_TYPE_ROOT_PORT = Root Complex 의 포트(CPU 쪽 최상단)
 * PCI_EXP_TYPE_DOWNSTREAM = 스위치의 하향 포트
 * PCI_EXP_TYPE_PCIE_BRIDGE = PCIe→PCI/PCI-X 브리지 (PCIe 쪽이 위)
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인:
 * drivers/pci/access.c 의 pcie_cap_has_sltctl() → [이 함수]
 * drivers/pci/access.c (capability 읽기/쓰기 전 검사)
 * drivers/pci/pci.c, drivers/pci/pcie/aspm.c → [이 함수]
 * NVMe 접점: NVMe SSD 자신은 PCI_EXP_TYPE_ENDPOINT 라 항상 false 다.
 * true 가 되는 것은 그 SSD 가 꽂힌 상위 루트 포트/스위치 포트이며,
 * AER·DPC·핫플러그·ASPM 정책이 실제로 걸리는 곳이 바로 그 포트다. */
static inline bool pcie_downstream_port(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev); /* [한국어] PCIe Capability 의 Device/Port Type 필드([7:4] of PCI_EXP_FLAGS)를 읽어 온다. PCIe 장치가 아니면 의미 없는 값이므로, 호출자들은 대개 pci_is_pcie 로 먼저 거른다(예: drivers/pci/access.c) */

	return type == PCI_EXP_TYPE_ROOT_PORT || /* [한국어] 루트 포트 — Root Complex 가 내보내는 링크의 상단 */
	       type == PCI_EXP_TYPE_DOWNSTREAM || /* [한국어] 스위치의 다운스트림 포트 */
	       type == PCI_EXP_TYPE_PCIE_BRIDGE; /* [한국어] PCIe→PCI 브리지 — PCIe 쪽이 상위이므로 아래로 링크를 내보내는 쪽에 속한다 */
}

/* [한국어] --- VPD(Vital Product Data) ---
 * VPD 는 capability 를 통해 시리얼 번호·부품 번호 같은 문자열을 노출하는
 * 선택적 기능이다. 데이터가 config space 가 아니라 "주소를 써 넣고 준비
 * 비트를 폴링해서 4바이트씩 읽는" 창을 통해 나오기 때문에 별도 파일
 * (drivers/pci/vpd.c)이 필요하다. */
void pci_vpd_init(struct pci_dev *dev); /* [한국어] VPD capability 를 찾아 접근 창을 초기화한다. 정의 drivers/pci/vpd.c */
extern const struct attribute_group pci_dev_vpd_attr_group; /* [한국어] VPD 내용을 /sys 에 노출하는 속성 그룹. 실체도 drivers/pci/vpd.c 에 있다 */

/* [한국어] --- VC(Virtual Channel) ---
 * VC 는 하나의 물리 링크를 여러 트래픽 클래스로 나눠 QoS 를 주는 확장
 * capability 다. suspend/resume 을 건너뛰면 설정이 날아가므로 저장·복원
 * 함수가 따로 있다. 구현은 drivers/pci/vc.c. */
/* PCI Virtual Channel */
int pci_save_vc_state(struct pci_dev *dev); /* [한국어] 현재 VC 설정을 저장 버퍼에 뜬다. 정의 drivers/pci/vc.c */
void pci_restore_vc_state(struct pci_dev *dev); /* [한국어] resume 시 저장해 둔 VC 설정을 되돌린다. 정의 drivers/pci/vc.c */
void pci_allocate_vc_save_buffers(struct pci_dev *dev); /* [한국어] VC 저장에 필요한 버퍼를 미리 잡는다. 정의 drivers/pci/vc.c. 위 pci_allocate_cap_save_buffers() 가 마지막에 이 함수를 부른다 */

/* [한국어] --- /proc/bus/pci 인터페이스 (CONFIG_PROC_FS) ---
 * 레거시 유저스페이스 인터페이스라 CONFIG_PROC_FS 가 꺼지면 통째로 빠진다.
 * 꺼졌을 때의 스텁은 전부 "아무 일도 안 하고 0(성공) 반환" 이다 —
 * /proc 노드를 만들지 못한 것이 장치 등록 실패로 번지면 안 되기 때문이다. */
/* PCI /proc functions */
#ifdef CONFIG_PROC_FS /* [한국어] /proc 파일시스템이 커널에 포함된 경우에만 실체가 있다 */
int pci_proc_attach_device(struct pci_dev *dev); /* [한국어] /proc/bus/pci/<BDF> 노드를 만든다. 정의 drivers/pci/proc.c */
int pci_proc_detach_device(struct pci_dev *dev); /* [한국어] 장치 제거 시 그 노드를 없앤다. 정의 drivers/pci/proc.c */
int pci_proc_detach_bus(struct pci_bus *bus); /* [한국어] 버스 단위 디렉터리를 없앤다. 정의 drivers/pci/proc.c */
#else /* [한국어] CONFIG_PROC_FS 가 꺼진 경우 */
static inline int pci_proc_attach_device(struct pci_dev *dev) { return 0; } /* [한국어] 스텁: /proc 노드를 만들지 않고 성공만 반환 */
static inline int pci_proc_detach_device(struct pci_dev *dev) { return 0; } /* [한국어] 스텁: 지울 것이 없으므로 성공 */
static inline int pci_proc_detach_bus(struct pci_bus *bus) { return 0; } /* [한국어] 스텁: 지울 것이 없으므로 성공 */
#endif /* [한국어] CONFIG_PROC_FS 분기 종료 */

/* [한국어] --- 핫플러그 드라이버가 쓰는 내부 함수 ---
 * drivers/pci/hotplug/ 아래의 pciehp/acpiphp 등이 이 둘을 쓴다. */
/* Functions for PCI Hotplug drivers to use */
int pci_hp_add_bridge(struct pci_dev *dev); /* [한국어] 핫플러그로 새로 나타난 브리지를 버스 트리에 편입한다. 정의 drivers/pci/probe.c */
bool pci_hp_spurious_link_change(struct pci_dev *pdev); /* [한국어] 링크 상태 변화가 실제 카드 삽입/제거가 아닌 잡음인지 판정한다. 정의 drivers/pci/hotplug/pci_hotplug_core.c */

/* [한국어] --- 레거시 I/O/메모리 공간 sysfs 파일 ---
 * HAVE_PCI_LEGACY 는 아키텍처가 "버스별 레거시 I/O 포트 공간"을 매핑할 수
 * 있을 때만 정의된다(모든 아키텍처가 그렇지는 않다). sysfs 도 함께 켜져
 * 있어야 의미가 있으므로 두 조건의 AND 로 갈린다.
 * 조건이 맞지 않으면 아무 일도 하지 않는 빈 스텁이 쓰인다 — 호출부인
 * 버스 추가/제거 경로가 #ifdef 없이 그대로 컴파일되게 하기 위해서다. */
#if defined(CONFIG_SYSFS) && defined(HAVE_PCI_LEGACY)
void pci_create_legacy_files(struct pci_bus *bus); /* [한국어] 버스별 레거시 I/O 공간을 여는 sysfs 파일을 만든다. 정의 drivers/pci/pci-sysfs.c */
void pci_remove_legacy_files(struct pci_bus *bus); /* [한국어] 그 파일들을 없앤다. 정의 drivers/pci/pci-sysfs.c */
#else /* [한국어] 조건이 맞지 않는 경우 */
static inline void pci_create_legacy_files(struct pci_bus *bus) { } /* [한국어] 스텁: 레거시 I/O 공간 sysfs 파일을 만들지 않는다 */
static inline void pci_remove_legacy_files(struct pci_bus *bus) { } /* [한국어] 스텁: 지울 파일이 없다 */
#endif /* [한국어] CONFIG_SYSFS + HAVE_PCI_LEGACY 분기 종료 */

/* [한국어] --- PCI 코어의 전역 락 세 개 ---
 * 세 락은 보호 대상과 컨텍스트 제약이 서로 다르다. 이 구분을 놓치면
 * 데드락이나 "잠들 수 없는 문맥에서 잠드는" 버그가 난다. */
/* Lock for read/write access to pci device and bus lists */
extern struct rw_semaphore pci_bus_sem; /* [한국어] 장치 리스트와 버스 리스트를 보호하는 읽기/쓰기 세마포어. 순회는 down_read(drivers/pci/bus.c 등), 트리 변경은 down_write 로 잡는다. 세마포어이므로 잠들 수 있는 문맥 전용 */
extern struct mutex pci_slot_mutex; /* [한국어] pci_slot(물리 슬롯) 리스트를 보호하는 뮤텍스. 실체는 drivers/pci/pci.c 의 DEFINE_MUTEX, 사용은 pci.c 의 lock/unlock 쌍 */

/* [한국어] config space 접근 자체를 직렬화하는 raw 스핀락.
 * 실체: drivers/pci/access.c 의 DEFINE_RAW_SPINLOCK(pci_lock).
 * 읽는 자/설정자: access.c 의 pci_lock_config()/pci_unlock_config()
 * 매크로가 raw_spin_lock_irqsave 로 감싸며, 모든 pci_read_config_byte()
 * /word/dword 와 pci_write_config_ 계열이 이 안에서 실행된다.
 * 왜 raw 스핀락인가: 레거시 config 메커니즘 #1 은 "주소 포트(0xcf8)에 쓰고
 * 데이터 포트(0xcfc)에서 읽는" 두 단계라, 두 단계 사이에 다른 CPU 가
 * 끼어들면 엉뚱한 장치를 읽는다. 이 임계구역은 인터럽트 문맥에서도
 * 들어올 수 있고 매우 짧아야 하므로 잠들 수 없는 raw 스핀락을 쓴다.
 * 동기화 주의: 이 락을 쥔 채로는 절대 잠들면 안 된다. */
extern raw_spinlock_t pci_lock;

/* [한국어] D3hot→D0 복귀 대기 시간의 "시스템 전역 하한"(ms).
 * 실체: drivers/pci/pci.c.
 * 설정자: drivers/pci/quirks.c 가 특정 브로큰 하드웨어를 만나면
 * 120ms 로 올린다 — 스펙상 10ms 면 되지만 실제로는 더 걸리는 칩이 있어서다.
 * 읽는 자: drivers/pci/pci.c 의 max(dev->d3hot_delay, pci_pm_d3hot_delay)
 * — 장치별 값과 전역 값 중 큰 쪽을 쓴다.
 * 동기화: 부팅 중 quirk 가 한 번 쓰고 이후 읽기 전용. */
extern unsigned int pci_pm_d3hot_delay;

/* [한국어] CONFIG_PCI_MSI 분기. MSI 지원이 컴파일에서 빠지면 "MSI 를 쓰지
 * 말라"는 명령 자체가 무의미하므로 빈 스텁으로 대체된다. */
#ifdef CONFIG_PCI_MSI
void pci_no_msi(void); /* [한국어] MSI 사용을 전역으로 끈다("pci=nomsi"). 정의 drivers/pci/msi/msi.c. NVMe 는 MSI-X 를 못 쓰면 큐당 인터럽트를 포기하고 단일 벡터/INTx 로 떨어진다 */
#else /* [한국어] CONFIG_PCI_MSI 가 꺼진 경우 */
static inline void pci_no_msi(void) { } /* [한국어] 스텁: 끌 MSI 자체가 없으므로 아무 일도 하지 않는다 */
#endif /* [한국어] CONFIG_PCI_MSI 분기 종료 */

/* [한국어] "pci=realloc" 커널 파라미터 파서.
 * 정의: drivers/pci/setup-bus.c (__init).
 * 호출자: drivers/pci/pci.c 의 부팅 파라미터 처리.
 * 무엇을 켜는가: 펌웨어(BIOS/UEFI)가 배정한 BAR 주소를 그대로 두지 않고
 * 커널이 전부 다시 배정하도록 만든다. 펌웨어가 큰 BAR 를 요구하는 카드에
 * 충분한 공간을 주지 않았을 때의 구제 수단이다.
 * 인자 이름이 없는 선언(char *)인 이유는 헤더에서는 타입만 알면 되기 때문. */
void pci_realloc_get_opt(char *);

/* [한국어]
 * pci_no_d1d2() - 이 장치에 대해 D1/D2 상태 사용을 금지해야 하는지 판정
 *
 * @dev: 검사할 장치.
 * @return(): 0 이 아니면 D1/D2 를 쓰지 말아야 한다.
 *
 * 왜 필요한가: D1 과 D2 는 선택적 상태이고, 구현이 부실해 여기서 복귀하지
 * 못하는 하드웨어가 적지 않다. 그런 장치에는 quirk 가 pci_dev.no_d1d2 를
 * 세워 두고, 커널은 D0 와 D3hot 만 오간다.
 * 부모까지 보는 이유: 상위 브리지가 D1/D2 를 제대로 못 하면 그 아래 장치의
 * 전원 상태 전이도 함께 깨진다. 그래서 "나 또는 내 부모 중 하나라도
 * no_d1d2 면 금지"로 판정한다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인:
 * drivers/pci/pci.c (요청된 상태가 D1/D2 인지 검사해 거절),
 * drivers/pci/pci.c 의 __pci_set_power_state(), pci_target_state(), pci_pm_init() → [이 함수] */
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
int pci_create_sysfs_dev_files(struct pci_dev *pdev); /* [한국어] 장치별 sysfs 파일(config, resource, rom 등)을 만든다. 정의 drivers/pci/pci-sysfs.c */
void pci_remove_sysfs_dev_files(struct pci_dev *pdev); /* [한국어] 장치 제거 시 그 파일들을 없앤다. 정의 drivers/pci/pci-sysfs.c */
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
 * 커널 파라미터로 바꿀 수 있으며(drivers/pci/pci.c 이하), 소비자는
 * drivers/pci/setup-bus.c 부근의 additional_ 크기 계산이다. */
extern unsigned long pci_hotplug_io_size; /* [한국어] 핫플러그 슬롯용 예약 I/O 공간 크기. 실체 drivers/pci/pci.c */
extern unsigned long pci_hotplug_mmio_size; /* [한국어] 예약 논-prefetchable MMIO 크기 */
extern unsigned long pci_hotplug_mmio_pref_size; /* [한국어] 예약 prefetchable MMIO 크기 */
extern unsigned long pci_hotplug_bus_size; /* [한국어] 예약할 버스 번호 개수. drivers/pci/pci.c 에서 0xff 를 넘으면 잘라 낸다 — 버스 번호가 8비트이기 때문 */

/* [한국어]
 * pci_is_cardbus_bridge() - 이 장치가 CardBus 브리지인지 판정
 *
 * @dev: 검사할 장치.
 * @return(): 헤더 타입이 CardBus(0x02)면 true.
 *
 * config space 오프셋 0x0e 의 Header Type 필드는 헤더 레이아웃을 결정한다:
 * 0x00 = 일반 장치(BAR 6개), 0x01 = PCI-to-PCI 브리지,
 * 0x02 = CardBus 브리지. 레이아웃이 다르므로 BAR 개수와 리소스 해석이
 * 전부 달라진다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 * 호출 체인: drivers/pci/probe.c (버스 스캔 중 CardBus 브리지 분기)
 * → [이 함수]
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
unsigned long pci_cardbus_resource_alignment(struct resource *res); /* [한국어] CardBus 브리지 윈도우의 정렬 요구를 계산한다. 정의 drivers/pci/setup-cardbus.c */
int pci_bus_size_cardbus_bridge(struct pci_bus *bus, /* [한국어] CardBus 브리지가 필요로 하는 창 크기를 산정한다. 정의 drivers/pci/setup-cardbus.c */
				struct list_head *realloc_head);
int pci_cardbus_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev, /* [한국어] CardBus 브리지 아래를 스캔하며 버스 번호를 소비한다. 정의 drivers/pci/setup-cardbus.c */
				   u32 buses, int max,
				   unsigned int available_buses, int pass);
/* [한국어] 아래는 함수 정의가 아니라 선언이다(세미콜론으로 끝난다).
 * pci_setup_cardbus() - CardBus 관련 커널 부팅 파라미터를 해석한다
 * @str: "pci=" 뒤에 붙어 온 옵션 문자열.
 * @return(): 이 파서가 처리한 옵션이면 0, 아니면 음수 errno.
 * 실행 컨텍스트: 부팅 초기(__init). 호출 체인: pci_setup() → [이 함수] */
int pci_setup_cardbus(char *str); /* [한국어] "pci=cbiosize=" 등 CardBus 관련 부팅 파라미터 파서. 정의 drivers/pci/setup-cardbus.c */

#else /* [한국어] CONFIG_CARDBUS 가 꺼진 경우 */
static inline unsigned long pci_cardbus_resource_alignment(struct resource *res) /* [한국어] 스텁 시작 — 정렬 요구 없음 */
{
	return 0;
}
/* [한국어]
 * pci_bus_size_cardbus_bridge() 스텁 (CONFIG_CARDBUS 꺼짐)
 * @bus: 크기를 산정하려던 버스. @realloc_head: 재배정 후보 리스트.
 * @return(): 항상 -EOPNOTSUPP("이 연산은 지원되지 않음").
 * CardBus 지원이 없는 커널에서는 CardBus 브리지 자체가 만들어지지 않으므로
 * 이 함수가 실제로 불릴 일이 없다. 그래도 리소스 산정 코드가 #ifdef 없이
 * 컴파일되도록 스텁이 필요하며, 만약 불린다면 호출자는 -EOPNOTSUPP 를 보고
 * 이 브리지를 건너뛴다.
 * 실행 컨텍스트: 리소스 산정(프로세스 컨텍스트). 락 없음.
 * 호출 체인: drivers/pci/setup-bus.c 의 브리지 크기 산정 → [이 스텁] */
static inline int pci_bus_size_cardbus_bridge(struct pci_bus *bus,
					      struct list_head *realloc_head)
{
	return -EOPNOTSUPP;
}
/* [한국어]
 * pci_cardbus_scan_bridge_extend() 스텁 (CONFIG_CARDBUS 꺼짐)
 * @bus/@dev/@buses/@available_buses/@pass: 실제 구현의 스캔 파라미터들.
 * @max: 지금까지 사용된 최대 버스 번호.
 * @return(): @max 를 그대로 반환 — "버스 번호를 하나도 소비하지 않았다"는 뜻.
 * 이 반환 규약이 중요하다. 열거 루프는 반환값을 다음 버스 번호의 기준으로
 * 삼으므로, 0 이나 음수를 돌려주면 버스 번호 배정이 망가진다.
 * 실행 컨텍스트: 버스 열거(프로세스 컨텍스트, pci_rescan_remove_lock 보유).
 * 호출 체인: drivers/pci/probe.c 의 버스 스캔 → [이 스텁] */
static inline int pci_cardbus_scan_bridge_extend(struct pci_bus *bus,
						 struct pci_dev *dev,
						 u32 buses, int max,
						 unsigned int available_buses,
						 int pass)
{
	return max;
}
/* [한국어]
 * pci_setup_cardbus() 스텁 (CONFIG_CARDBUS 꺼짐)
 * @str: 부팅 파라미터 문자열.
 * @return(): -ENOENT("그런 옵션 없음").
 * 커널 파라미터 파서는 이 값을 보고 "이 옵션을 처리할 주체가 없다"고
 * 판단해 사용자에게 알 수 없는 옵션으로 보고한다.
 * 실행 컨텍스트: 부팅 초기 파라미터 파싱. 호출 체인: pci_setup() → [이 스텁] */
static inline int pci_setup_cardbus(char *str) { return -ENOENT; }
#endif /* CONFIG_CARDBUS */ /* [한국어] CONFIG_CARDBUS 분기 종료 — 원문 주석이 어느 #ifdef 의 짝인지 밝혀 준다 */

/* [한국어]
 * pci_match_one_device() - pci_device_id 항목 하나가 이 장치와 맞는지 검사
 *
 * @id: 드라이버가 등록한 ID 표의 한 줄.
 * @dev: 매칭 대상 장치.
 * @return(): 맞으면 @id 를 그대로, 아니면 NULL.
 *
 * 왜 필요한가: 커널이 새 PCI 장치를 발견하면 등록된 모든 드라이버의
 * id_table 을 훑어 담당자를 찾는다. 그 한 줄 비교가 이 함수다.
 * 다섯 조건을 모두 만족해야 매칭이다:
 * vendor / device / subvendor / subdevice — 각각 PCI_ANY_ID(0xffffffff)면
 * "아무거나"라는 와일드카드이므로 무조건 통과.
 * class — XOR 후 class_mask 로 마스킹해 0 이어야 한다. 즉 마스크가 1 인
 * 비트들만 정확히 일치하면 되고, class_mask 가 0 이면 클래스는 아예
 * 보지 않는다는 뜻이 된다.
 * 실행 컨텍스트: 장치 등록/드라이버 등록 시 프로세스 컨텍스트. 락 없음
 * (호출자인 pci-driver.c 쪽이 필요한 락을 이미 쥔다).
 * 호출 체인:
 * drivers/pci/pci-driver.c 의 pci_match_id() → [이 함수]
 * drivers/pci/pci-driver.c (sysfs new_id 로 동적 추가된 ID) → [이 함수]
 * drivers/pci/search.c (ID 로 장치 찾기) → [이 함수]
 * NVMe 접점: NVMe 드라이버의 ID 표는 두 층이다. 앞쪽에는 quirk 가 필요한
 * 특정 VID:DID 행들이 있고, 맨 끝(drivers/nvme/host/pci.c)에
 * { PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) }
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

extern struct kset *pci_slots_kset; /* [한국어] 모든 pci_slot 의 kobject 를 담는 kset(=/sys/bus/pci/slots). drivers/pci/hotplug/acpiphp_ibm.c 와 rpadlpar_sysfs.c 가 이 kset 의 kobject 를 부모로 삼아 파일을 단다 */

/* [한국어]
 * struct pci_slot_attribute - pci_slot 전용 sysfs 속성 서술자
 *
 * 커널의 일반 sysfs 콜백은 struct kobject 를 받지만, 슬롯 코드는
 * struct pci_slot 을 받고 싶어 한다. 그래서 struct attribute 를 첫 멤버로
 * 품은 확장 구조체를 만들고, 위 to_pci_slot_attr 로 되돌려 받는다. */
struct pci_slot_attribute {
	/* [한국어] 커널 sysfs 가 요구하는 표준 attribute. 반드시 첫 멤버여야 to_pci_slot_attr 의 container_of()
 * 계산이 오프셋 0 으로 단순해진다. 설정자/읽는 자: sysfs 코어. 동기화: 읽기 전용 정적 서술자 */
	struct attribute attr;
	/* [한국어] 슬롯 속성 파일을 read 할 때 불릴 콜백.
	 * 설정자: 각 속성 정의부(drivers/pci/slot.c).
	 * 읽는 자: drivers/pci/slot.c 의 pci_slot_attr_show() 디스패처.
	 * 값 범위: NULL 이면 읽기 불가.
	 * 동기화: 정적 서술자라 읽기 전용. */
	ssize_t (*show)(struct pci_slot *, char *);
	/* [한국어] write 할 때 불릴 콜백. 읽는 자: slot.c 의 store 디스패처. 값 범위: NULL 이면 쓰기 불가(읽기 전용 속성)
 * (위 한 줄이 이 필드 설명의 전부다.) */
	ssize_t (*store)(struct pci_slot *, const char *, size_t);
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
 * bit0 = 1 → I/O 공간 BAR (나머지는 I/O 포트 주소)
 * bit0 = 0 → 메모리 공간 BAR
 * bit[2:1] = 00 → 32비트 주소
 * bit[2:1] = 10 → 64비트 주소 (BAR 두 칸을 잡아먹는다)
 * bit3 = 1 → prefetchable (읽기 부작용이 없어 캐시/합병 가능)
 * 그래서 아래 네 값이 필요하다.
 *
 * NVMe 접점: NVMe 스펙은 컨트롤러 레지스터 창을 BAR0/BAR1 한 쌍
 * (MLBAR/MUBAR)으로 규정하므로, NVMe SSD 의 BAR0 는 64비트 메모리 BAR 로
 * 잡히는 것이 일반적이다 — 즉 여기서 pci_bar_mem64 에 해당한다.
 * 그 해석 결과가 pci_dev.resource[0] 에 정착하고,
 * drivers/nvme/host/pci.c 의
 * dev->bar = ioremap(pci_resource_start(pdev, 0), size);
 * 가 바로 그 값을 읽어 도어벨을 포함한 레지스터 창을 매핑한다. */
enum pci_bar_type {
	/* [한국어] BAR 값을 아직 해석하지 않은 상태 — __pci_read_base() 가 직접 비트를 보고 종류를 판정하라는 뜻. 일반 BAR 를
 * 읽을 때 쓰는 값이다 */
	pci_bar_unknown,	/* Standard PCI BAR probe */
	/* [한국어] I/O 포트 BAR 로 확정(BAR bit0=1)
 * (위 한 줄이 이 필드 설명의 전부다.) */
	pci_bar_io,		/* An I/O port BAR */
	/* [한국어] 32비트 메모리 BAR 로 확정
 * (위 한 줄이 이 필드 설명의 전부다.) */
	pci_bar_mem32,		/* A 32-bit memory BAR */
	/* [한국어] 64비트 메모리 BAR 로 확정 — 연속한 BAR 두 칸이 하나의 64비트 주소를 이룬다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	pci_bar_mem64,		/* A 64-bit memory BAR */
};

/* [한국어] --- 호스트 브리지 device 객체 참조 ---
 * 장치가 어느 호스트 브리지 아래에 있는지 거슬러 올라가 그 struct device 를
 * 얻는다. DMA 마스크나 IOMMU 설정처럼 "버스 전체에 걸린 제약"을 물어볼 때
 * 필요하다. get 은 참조를 올리고 put 은 내린다 — 반드시 짝으로 쓴다. */
struct device *pci_get_host_bridge_device(struct pci_dev *dev); /* [한국어] 이 장치의 호스트 브리지 struct device 를 참조 계수를 올려 얻는다. 정의 drivers/pci/host-bridge.c */
void pci_put_host_bridge_device(struct device *dev); /* [한국어] 위에서 올린 참조를 되돌린다. 정의 drivers/pci/host-bridge.c */

/* [한국어] --- Resizable BAR 와 리소스 재배치 ---
 * 일부 장치는 BAR 크기를 소프트웨어가 바꿀 수 있다(Resizable BAR 확장
 * capability). 크기를 바꾸면 기존 주소 배정이 무효가 되므로, 해제 →
 * 크기 변경 → 재배치의 절차가 필요하다. 아래 함수들이 그 절차를 이룬다. */
void pci_resize_resource_set_size(struct pci_dev *dev, int resno, int size); /* [한국어] Resizable BAR 로 BAR 크기를 바꾼 뒤 resource 의 크기를 그에 맞춰 갱신한다. 정의 drivers/pci/rebar.c */
int pci_do_resource_release_and_resize(struct pci_dev *dev, int resno, int size, /* [한국어] 해당 BAR 를 놓고 크기를 바꾼 뒤 다시 잡는 전체 절차. exclude_bars 는 이 과정에서 건드리면 안 되는 BAR 비트마스크. 정의 drivers/pci/setup-bus.c */
				       int exclude_bars);
unsigned int pci_rescan_bus_bridge_resize(struct pci_dev *bridge); /* [한국어] 브리지 아래를 재스캔하며 윈도우 크기를 다시 잡는다. 정의 drivers/pci/probe.c */
int __must_check pci_reassign_resource(struct pci_dev *dev, int i, resource_size_t add_size, resource_size_t align); /* [한국어] 리소스 하나를 더 큰 크기/정렬로 다시 배정한다. __must_check 가 붙은 이유는 실패를 무시하면 장치가 주소 없이 남기 때문 */

/* [한국어] --- 장치 열거의 핵심 진입점들 ---
 * 여기부터는 "config space 를 읽어 struct pci_dev 를 완성하는" 함수들이다.
 * 이 헤더에서 NVMe 독자가 가장 눈여겨볼 부분이기도 하다: NVMe 드라이버가
 * 보는 pci_resource_start(pdev, 0) 값이 만들어지는 자리가 여기다. */
int pci_configure_extended_tags(struct pci_dev *dev, void *ign); /* [한국어] PCIe Device Control 의 Extended Tag Field Enable 을 켠다. 태그 비트가 5비트에서 8비트로 늘어 미완료 요청을 32개에서 256개까지 띄울 수 있다. 정의 drivers/pci/probe.c. NVMe 처럼 깊은 큐로 많은 읽기를 동시에 던지는 장치에서 성능에 직접 영향을 준다 */
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *pl, /* [한국어] devfn 위치에 장치가 있는지 Vendor ID 를 읽어 확인한다. rrs_timeout 은 위 pci_bus_rrs_vendor_id 판정에 걸렸을 때 얼마나 기다려 줄지(ms). 정의 drivers/pci/probe.c */
				int rrs_timeout);
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *pl, /* [한국어] 위 함수의 아키텍처 비의존 일반 구현. 아키텍처가 특별한 방법을 갖고 있으면 위 함수만 대체한다. 정의 drivers/pci/probe.c */
					int rrs_timeout);

int pci_setup_device(struct pci_dev *dev); /* [한국어] config space 를 읽어 struct pci_dev 의 vendor/device/class/hdr_type/BAR 등을 모두 채우는 열거의 본체. 정의 drivers/pci/probe.c */
/* [한국어] BAR 크기는 레지스터에 적혀 있지 않다. 알아내는 유일한 방법은
 * "모두 1을 써 넣고 되읽는" 것이다(PCI 스펙이 규정한 절차).
 * 장치는 자기가 디코드하지 않는 하위 비트만 0 으로 되돌려 주므로, 되읽은
 * 값에서 종류 비트를 걷어 내고 반전한 뒤 1을 더하면 크기가 나온다
 * (크기는 항상 2의 거듭제곱이고 그 크기만큼 정렬되어야 한다).
 * drivers/pci/probe.c 의 __pci_size_bars() 가 BAR 하나마다
 * 원본 읽기 → 전부 1 쓰기 → 되읽기 → 원본 복구
 * 를 수행하고, 되읽은 원시 값을 sizes[] 에 담아 온다. 원본을 반드시
 * 복구하는 이유는, 이 조사 도중 BAR 가 엉뚱한 주소를 가리키게 되면
 * 실제 메모리 접근이 잘못된 곳으로 갈 수 있기 때문이다(그래서 조사 전에
 * Command 레지스터의 MEM/IO 디코딩을 꺼 두는 것이 규칙이다).
 * ROM BAR 만은 하위 비트가 "Enable" 이라 마스크가 다르며
 * (probe.c 의 PCI_ROM_ADDRESS_MASK), 그 갈래가 __pci_size_rom() 이다. */
void __pci_size_stdbars(struct pci_dev *dev, int count, /* [한국어] 표준 BAR 들(count 개)의 크기 원시 값을 한꺼번에 구해 sizes[] 에 담는다. @pos 는 첫 BAR 의 config 오프셋(일반 장치는 0x10). 정의 drivers/pci/probe.c → __pci_size_bars(probe.c) */
			unsigned int pos, u32 *sizes);
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type, /* [한국어] sizes[] 로 받은 원시 값과 BAR 원본 값을 조합해 struct resource 하나를 완성한다. 정의 drivers/pci/probe.c */
		    struct resource *res, unsigned int reg, u32 *sizes);
void pci_configure_ari(struct pci_dev *dev); /* [한국어] ARI(Alternative Routing-ID Interpretation)를 켤 수 있으면 켠다. devfn 8비트를 전부 function 번호로 써서 한 장치가 256 함수를 가질 수 있게 하는 기능으로, SR-IOV 로 VF 를 많이 만들 때 필요하다. 정의 drivers/pci/pci.c */

/* [한국어] --- 브리지 윈도우 크기 산정과 리소스 배정 ---
 * PCI 리소스 할당은 두 번 훑는다.
 * 1) __pci_bus_size_bridges(): 아래에서 위로 올라가며 "이 브리지 아래
 * 장치들이 필요로 하는 총 크기와 정렬"을 계산해 브리지 윈도우 크기를 정한다.
 * 2) __pci_bus_assign_resources(): 위에서 아래로 내려가며 실제 주소를
 * 배정하고 BAR 에 써 넣는다.
 * 배정에 실패한 리소스는 fail_head 리스트에, 나중에 더 줄 수 있는 것은
 * realloc_head 리스트에 모아 두었다가 두 번째 시도에서 다시 다룬다. */
int pci_dev_res_add_to_list(struct list_head *head, struct pci_dev *dev, /* [한국어] 재배정 후보 리소스를 리스트에 추가한다. add_size 는 추가로 더 주고 싶은 크기, min_align 은 최소 정렬 요구. 정의 drivers/pci/setup-bus.c */
			    struct resource *res, resource_size_t add_size,
			    resource_size_t min_align);
void __pci_bus_size_bridges(struct pci_bus *bus, /* [한국어] 브리지 윈도우 크기 산정(아래에서 위로). 정의 drivers/pci/setup-bus.c */
			struct list_head *realloc_head);
void __pci_bus_assign_resources(const struct pci_bus *bus, /* [한국어] 산정된 크기에 맞춰 실제 주소를 배정(위에서 아래로). 정의 drivers/pci/setup-bus.c */
				struct list_head *realloc_head,
				struct list_head *fail_head);
bool pci_bus_clip_resource(struct pci_dev *dev, int idx); /* [한국어] 리소스가 상위 창 밖으로 삐져나오면 창 안으로 잘라 낸다. 정의 drivers/pci/bus.c */
void pci_walk_bus_locked(struct pci_bus *top, /* [한국어] pci_bus_sem 을 이미 쥔 상태에서 버스 트리를 순회하며 콜백을 부른다(중복 잠금 방지판). 정의 drivers/pci/bus.c */
			 int (*cb)(struct pci_dev *, void *),
			 void *userdata);

/* [한국어] --- pci_dev.resource[] 인덱스를 해석하는 헬퍼들 ---
 * 한 배열에 성격이 다른 리소스가 섞여 있어서(표준 BAR / ROM / VF BAR /
 * 브리지 윈도우) "이 번호가 무엇인가"를 묻는 헬퍼가 필요하다.
 * 배열 구획(<linux/pci.h> 의 리소스 enum):
 * [PCI_STD_RESOURCES .. PCI_STD_RESOURCE_END] 표준 BAR 0~5
 * [PCI_ROM_RESOURCE] 확장 ROM BAR
 * [PCI_IOV_RESOURCES .. PCI_IOV_RESOURCE_END] VF BAR (CONFIG_PCI_IOV)
 * [PCI_BRIDGE_RESOURCES .. PCI_BRIDGE_RESOURCE_END] 브리지 윈도우
 * PCI_NUM_RESOURCES 배열 크기 */
const char *pci_resource_name(struct pci_dev *dev, unsigned int i); /* [한국어] 리소스 번호를 사람이 읽을 이름으로 바꾼다("BAR 0", "ROM", "bridge window" 등). 정의 drivers/pci/pci.c — 로그 메시지에 쓰인다 */
bool pci_resource_is_optional(const struct pci_dev *dev, int resno); /* [한국어] 이 리소스가 없어도 장치가 동작할 수 있는지(= 배정 실패를 치명적으로 보지 않아도 되는지). 정의 drivers/pci/setup-bus.c */
static inline bool pci_resource_is_bridge_win(int resno)
{
	return resno >= PCI_BRIDGE_RESOURCES && /* [한국어] PCI_BRIDGE_RESOURCES 이상이고 */
	       resno <= PCI_BRIDGE_RESOURCE_END; /* [한국어] PCI_BRIDGE_RESOURCE_END 이하이면 브리지 윈도우 구획이다. 사용처: drivers/pci/pci-sysfs.c, setup-bus.c */
}

/* [한국어] 아래 함수의 한국어 해설:
 * pci_resource_num() - resource 포인터에서 그 인덱스를 역산한다
 *
 * @dev: 그 resource 를 소유한 장치.
 * @res: 반드시 @dev->resource[] 안의 원소여야 한다(원문 주석의 경고).
 * @return(): 배열 인덱스(0 이상 PCI_NUM_RESOURCES 미만).
 *
 * 왜 필요한가: 리소스 배정 코드는 resource 포인터를 들고 다니는데,
 * "이게 몇 번 BAR 냐"를 알아야 하는 지점이 자주 나온다(예: 브리지 윈도우인지,
 * VF BAR 인지 판정할 때). 배열 원소 주소에서 시작 주소를 빼는 포인터 산술로
 * 인덱스를 복원한다 — 별도 필드를 두지 않기 위한 선택이다.
 * 왜 이런 계산이 성립하는가: dev->resource[] 는 고정 크기 배열이고 원소가
 * 연속 배치되므로, (res - &dev->resource[0]) 이 정확히 인덱스가 된다.
 * 실행 컨텍스트: 순수 계산. 락 없음.
 * 호출 체인:
 * drivers/pci/setup-bus.c (리소스 정렬/윈도우 계산) → [이 함수]
 * 아래 pci_resource_alignment() → [이 함수] */
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
	int resno = res - &dev->resource[0]; /* [한국어] 포인터 뺄셈으로 인덱스 복원. C 의 포인터 산술은 자동으로 sizeof(struct resource) 로 나누므로 그대로 원소 번호가 된다 */

	/* Passing a resource that is not among dev's resources? */
	WARN_ON_ONCE(resno >= PCI_NUM_RESOURCES); /* [한국어] 범위를 넘으면 한 번만 경고를 찍는다. WARN_ON_ONCE 인 이유는 이 함수가 리소스 루프 안에서 매우 자주 불려 로그가 폭주하기 때문. 음수 쪽(res 가 배열보다 앞)은 검사하지 않는데, 그 경우는 호출 규약 위반이라 원문 주석이 호출자 책임으로 못박아 두었다 */

	return resno; /* [한국어] 복원한 인덱스 반환 */
}

/* [한국어] --- 버스 번호 검증과 브리지 윈도우 선택 ---
 * 이 묶음은 열거 후 정리 단계에서 쓰인다. */
void pbus_validate_busn(struct pci_bus *bus); /* [한국어] 하위 버스 번호 범위(secondary/subordinate)가 모순이 없는지 확인한다. 정의 drivers/pci/probe.c */
struct resource *pbus_select_window(struct pci_bus *bus, /* [한국어] 주어진 리소스를 담을 수 있는 상위 브리지 창을 고른다(I/O 인지 MEM 인지 prefetchable 인지에 따라). 정의 drivers/pci/setup-bus.c */
				    const struct resource *res);
void pci_reassigndev_resource_alignment(struct pci_dev *dev); /* [한국어] "pci=resource_alignment()=" 파라미터에 맞춰 특정 장치의 BAR 를 더 큰 경계에 다시 배정한다. VFIO 패스스루에서 BAR 를 페이지 단위로 격리하려 할 때 쓴다. 정의 drivers/pci/pci.c */
void pci_disable_bridge_window(struct pci_dev *dev); /* [한국어] 브리지 윈도우를 비활성화한다(base > limit 로 만들어 디코딩을 끈다). 정의 drivers/pci/setup-res.c */
struct pci_bus *pci_bus_get(struct pci_bus *bus); /* [한국어] pci_bus 참조 계수를 올린다. 정의 drivers/pci/bus.c */
void pci_bus_put(struct pci_bus *bus); /* [한국어] 올린 참조를 내린다. 정의 drivers/pci/bus.c */

/* [한국어]
 * PCIE_LNKCAP_SLS2SPEED - Link Capabilities 의 Supported Link Speeds 필드를
 * enum pci_bus_speed 로 변환
 *
 * @lnkcap: PCIe Capability 의 Link Capabilities 레지스터 값(32비트).
 * @return(): PCIE_SPEED_2_5GT ~ PCIE_SPEED_64_0GT, 모르면 PCI_SPEED_UNKNOWN.
 *
 * 왜 필요한가: 링크 속도는 레지스터에 "GT/s 숫자"가 아니라 인코딩된 코드로
 * 들어 있다. PCI_EXP_LNKCAP_SLS 로 그 필드([3:0])만 잘라 낸 뒤, 알려진
 * 코드값과 하나씩 비교해 커널 내부 표현으로 바꾼다.
 * 높은 속도부터 비교하는 이유는 없다 — 값이 서로 배타적이라 순서는 무관하고,
 * 단지 최신 세대부터 적는 관례를 따랐다.
 * 이 매크로가 다루는 필드는 "이 포트가 지원하는 최대 속도"이며, 아래
 * PCIE_LNKCAP2_SLS2SPEED 가 다루는 Link Capabilities 2 의 벡터와 다르다:
 * 전자는 하나의 코드값, 후자는 지원 속도를 나열한 비트마스크다.
 * NVMe 접점: NVMe SSD 가 Gen4 x4 인데 Gen3 슬롯에 꽂혀 성능이 반토막 나는
 * 상황은, 이 값(장치 지원 속도)과 실제 링크 속도의 차이로 드러난다.
 * 그 비교와 경고 출력이 아래 pcie_report_downtraining() 이다. */
#define PCIE_LNKCAP_SLS2SPEED(lnkcap) /* [한국어] 문장식 매크로 — lnkcap 을 여러 번 평가하지 않으려고 지역 변수에 한 번만 담는다 */ \
({ /* [한국어] 문장식 시작 */ \
	u32 lnkcap_sls = (lnkcap) & PCI_EXP_LNKCAP_SLS; /* [한국어] PCI_EXP_LNKCAP_SLS 는 Link Capabilities 의 Supported Link Speeds 필드 마스크([3:0]) */ \
									\
	(lnkcap_sls == PCI_EXP_LNKCAP_SLS_64_0GB ? PCIE_SPEED_64_0GT : /* [한국어] 코드값 6 = 64.0 GT/s (Gen6) */ \
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_32_0GB ? PCIE_SPEED_32_0GT : /* [한국어] 코드값 5 = 32.0 GT/s (Gen5) */ \
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_16_0GB ? PCIE_SPEED_16_0GT : /* [한국어] 코드값 4 = 16.0 GT/s (Gen4) */ \
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_8_0GB ? PCIE_SPEED_8_0GT : /* [한국어] 코드값 3 = 8.0 GT/s (Gen3) */ \
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_5_0GB ? PCIE_SPEED_5_0GT : /* [한국어] 코드값 2 = 5.0 GT/s (Gen2) */ \
	 lnkcap_sls == PCI_EXP_LNKCAP_SLS_2_5GB ? PCIE_SPEED_2_5GT : /* [한국어] 코드값 1 = 2.5 GT/s (Gen1) */ \
	 PCI_SPEED_UNKNOWN); /* [한국어] 알려지지 않은 코드 — 새 세대이거나 고장난 값 */ \
}) /* [한국어] 문장식 종료 */

/* [한국어]
 * PCIE_LNKCAP2_SLS2SPEED - Link Capabilities 2 의 지원 속도 "벡터"를 최고 속도로 변환
 *
 * @lnkcap2: Link Capabilities 2 레지스터 값.
 * @return(): 지원하는 것 중 가장 빠른 속도, 아무것도 안 걸리면 PCI_SPEED_UNKNOWN.
 *
 * 위 PCIE_LNKCAP_SLS2SPEED 와의 결정적 차이: 이쪽은 == 비교가 아니라 & 비트
 * 검사다. Link Capabilities 2 의 Supported Link Speeds Vector 는 "지원하는
 * 속도마다 비트 하나"인 비트마스크라서, 한 장치가 여러 비트를 동시에 세운다.
 * 그래서 가장 빠른 것부터 차례로 검사해 처음 걸리는 것을 답으로 삼는다 —
 * 여기서는 순서가 결과를 바꾼다(위 매크로와 달리 순서가 의미를 가진다).
 * 왜 레지스터가 둘인가: 초기 PCIe 는 Link Capabilities 의 4비트 코드 하나로
 * 최고 속도만 표현했는데, 중간 세대를 건너뛰고 지원하는 하드웨어가 나오면서
 * 표현력이 부족해졌다. 그래서 r3.0 에서 벡터 방식의 Link Capabilities 2 가
 * 추가되었고, 커널은 둘 다 읽어 종합한다. */
/* PCIe link information from Link Capabilities 2 */
#define PCIE_LNKCAP2_SLS2SPEED(lnkcap2) /* [한국어] 문장식이 아니라 순수 조건식 매크로 — lnkcap2 가 매크로 안에서 여러 번 전개되므로 부작용 있는 식을 넘기면 안 된다 */ \
	((lnkcap2) & PCI_EXP_LNKCAP2_SLS_64_0GB ? PCIE_SPEED_64_0GT : /* [한국어] 64.0 GT/s 지원 비트가 서 있으면 Gen6 */ \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_32_0GB ? PCIE_SPEED_32_0GT : /* [한국어] 32.0 GT/s 비트 — Gen5 */ \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_16_0GB ? PCIE_SPEED_16_0GT : /* [한국어] 16.0 GT/s 비트 — Gen4 */ \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_8_0GB ? PCIE_SPEED_8_0GT : /* [한국어] 8.0 GT/s 비트 — Gen3 */ \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_5_0GB ? PCIE_SPEED_5_0GT : /* [한국어] 5.0 GT/s 비트 — Gen2 */ \
	 (lnkcap2) & PCI_EXP_LNKCAP2_SLS_2_5GB ? PCIE_SPEED_2_5GT : /* [한국어] 2.5 GT/s 비트 — Gen1. 모든 PCIe 장치가 이 속도는 지원해야 한다 */ \
	 PCI_SPEED_UNKNOWN) /* [한국어] 벡터가 비어 있음 — Link Capabilities 2 를 구현하지 않는 구형 장치 */

/* [한국어]
 * PCIE_LNKCTL2_TLS2SPEED - Link Control 2 의 Target Link Speed 를 변환
 *
 * @lnkctl2: Link Control 2 레지스터 값(16비트).
 * @return(): 목표 속도에 해당하는 enum pci_bus_speed.
 *
 * Target Link Speed([3:0])는 "지금 지원하는 속도"가 아니라 "다음 재훈련 때
 * 올라갈 목표 속도"다. 소프트웨어가 이 값을 낮춰 쓰면 링크를 일부러 느리게
 * 만들 수 있다 — 신호 품질이 나빠 상위 속도에서 링크가 불안정한 하드웨어를
 * 위한 quirk 나, 디바이스트리의 max-link-speed 설정이 이 경로로 반영된다.
 * 상수 이름이 TLS 인 것은 Target Link Speed 의 약자이며, 위 두 매크로의
 * SLS(Supported Link Speeds)와 레지스터가 다르다. */
#define PCIE_LNKCTL2_TLS2SPEED(lnkctl2) /* [한국어] lnkctl2 를 한 번만 평가하기 위해 문장식으로 감쌌다 */ \
({ /* [한국어] 문장식 시작 */ \
	u16 lnkctl2_tls = (lnkctl2) & PCI_EXP_LNKCTL2_TLS; /* [한국어] PCI_EXP_LNKCTL2_TLS 는 Target Link Speed 필드 마스크([3:0]) */ \
									\
	(lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_64_0GT ? PCIE_SPEED_64_0GT : /* [한국어] 목표가 Gen6 */ \
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_32_0GT ? PCIE_SPEED_32_0GT : /* [한국어] 목표가 Gen5 */ \
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_16_0GT ? PCIE_SPEED_16_0GT : /* [한국어] 목표가 Gen4 */ \
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_8_0GT ? PCIE_SPEED_8_0GT : /* [한국어] 목표가 Gen3 */ \
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_5_0GT ? PCIE_SPEED_5_0GT : /* [한국어] 목표가 Gen2 */ \
	 lnkctl2_tls == PCI_EXP_LNKCTL2_TLS_2_5GT ? PCIE_SPEED_2_5GT : /* [한국어] 목표가 Gen1 */ \
	 PCI_SPEED_UNKNOWN); /* [한국어] 알 수 없는 목표값 */ \
}) /* [한국어] 문장식 종료 */

/* [한국어]
 * PCIE_SPEED2MBS_ENC - 링크 속도를 "인코딩 오버헤드를 뺀 실효 Mb/s" 로 변환
 *
 * @speed: enum pci_bus_speed 값.
 * @return(): 레인 1개당 실효 Mb/s. 모르는 값이면 0.
 *
 * 왜 분수 곱셈이 붙는가: PCIe 는 세대마다 라인 인코딩이 다르다.
 * Gen1(2.5GT/s), Gen2(5GT/s) : 8b/10b — 10비트를 보내 8비트를 얻으므로 8/10
 * Gen3~Gen5(8/16/32GT/s) : 128b/130b — 130비트에 128비트가 실리므로 128/130
 * Gen6(64GT/s) : PAM4 + FLIT 모드라 이 표에서는 1/1 로 둔다
 * 예: Gen3 는 8000 * 128 / 130 = 7876 Mb/s (정수 나눗셈).
 * 이 값에 레인 수를 곱한 것이 링크의 실효 대역폭이며,
 * __pcie_print_link_status() 가 그 수치를 커널 로그에 찍는다.
 * NVMe 접점: "이 SSD 가 링크 때문에 막히는가"를 따질 때 쓰는 상한이 이 값이다.
 * Gen4 x4 라면 16000 * 128/130 * 4 = 약 63000 Mb/s (약 7.9 GB/s). */
/* PCIe speed to Mb/s reduced by encoding overhead */
#define PCIE_SPEED2MBS_ENC(speed) /* [한국어] 순수 조건식 매크로 */ \
	((speed) == PCIE_SPEED_64_0GT ? 64000*1/1 : /* [한국어] Gen6 는 64000 Mb/s 를 그대로 — PAM4/FLIT 이라 이 표에서는 오버헤드를 반영하지 않는다 */ \
	 (speed) == PCIE_SPEED_32_0GT ? 32000*128/130 : /* [한국어] Gen5: 32000 * 128/130 */ \
	 (speed) == PCIE_SPEED_16_0GT ? 16000*128/130 : /* [한국어] Gen4: 16000 * 128/130 */ \
	 (speed) == PCIE_SPEED_8_0GT  ?  8000*128/130 : /* [한국어] Gen3: 8000 * 128/130 */ \
	 (speed) == PCIE_SPEED_5_0GT  ?  5000*8/10 : /* [한국어] Gen2: 5000 * 8/10 — 8b/10b 라 20%가 인코딩에 쓰인다 */ \
	 (speed) == PCIE_SPEED_2_5GT  ?  2500*8/10 : /* [한국어] Gen1: 2500 * 8/10 */ \
	 0) /* [한국어] 알 수 없는 속도는 0 — 호출자가 대역폭 계산을 건너뛰게 한다 */

static inline int pcie_dev_speed_mbps(enum pci_bus_speed speed)
{
	switch (speed) {
	case PCIE_SPEED_2_5GT: /* [한국어] 2.5 GT/s → 2500 Mb/s */
		return 2500; /* [한국어] GT/s 숫자에 1000 을 곱한 값 그대로 — 인코딩 손실을 빼지 않는다 */
	case PCIE_SPEED_5_0GT: /* [한국어] 5.0 GT/s */
		return 5000; /* [한국어] 5000 Mb/s */
	case PCIE_SPEED_8_0GT: /* [한국어] 8.0 GT/s */
		return 8000; /* [한국어] 8000 Mb/s */
	case PCIE_SPEED_16_0GT: /* [한국어] 16.0 GT/s */
		return 16000; /* [한국어] 16000 Mb/s */
	case PCIE_SPEED_32_0GT: /* [한국어] 32.0 GT/s */
		return 32000; /* [한국어] 32000 Mb/s */
	case PCIE_SPEED_64_0GT: /* [한국어] 64.0 GT/s */
		return 64000; /* [한국어] 64000 Mb/s */
	default: /* [한국어] 위 목록에 없는 값(PCI_SPEED_UNKNOWN 또는 PCI/PCI-X 속도) */
		break; /* [한국어] switch 를 빠져나가 아래 에러 반환으로 간다 */
	} /* [한국어] switch 종료 */

	return -EINVAL; /* [한국어] 알 수 없는 속도 — 음수 errno 로 실패를 알린다. 반환형이 int 인 이유가 여기 있다 */
}

/* [한국어] --- 링크 상태 조회/보고 ---
 * 아래 넷은 "링크가 지금 어떤 상태인가"를 알아내고 사람에게 보여 주는 함수들이다. */
u8 pcie_get_supported_speeds(struct pci_dev *dev); /* [한국어] 이 장치가 지원하는 속도들을 비트마스크(u8)로 돌려준다. Link Capabilities 와 Link Capabilities 2 를 종합한 결과다. 정의 drivers/pci/pci.c */
const char *pci_speed_string(enum pci_bus_speed speed); /* [한국어] 속도를 사람이 읽는 문자열("8.0 GT/s PCIe" 등)로 바꾼다. 정의 drivers/pci/probe.c */
void __pcie_print_link_status(struct pci_dev *dev, bool verbose); /* [한국어] 링크 속도·폭·실효 대역폭을 커널 로그에 찍는다. verbose 면 상위 링크까지 훑는다. 정의 drivers/pci/pci.c */
void pcie_report_downtraining(struct pci_dev *dev); /* [한국어] 장치가 지원하는 최대 속도/폭보다 실제 링크가 낮게 잡혔으면 경고를 찍는다. 정의 drivers/pci/probe.c. NVMe SSD 가 기대보다 느릴 때 dmesg 에 남는 그 메시지의 출처다 */

/* [한국어]
 * enum pcie_link_change_reason - 링크 속도/폭이 갱신된 계기
 *
 * 이 값은 트레이스포인트(trace_pcie_link_event())에 그대로 실려 나가며,
 * 링크가 언제 왜 바뀌었는지를 사후에 추적할 수 있게 한다.
 * 설정자: 아래 __pcie_update_link_speed 를 부르는 각 호출부가 상수로 넘긴다.
 * 읽는 자: 트레이스포인트 소비자(ftrace/perf).
 * 동기화: 값 전달용이라 공유 상태 없음. */
enum pcie_link_change_reason {
	/* [한국어] 링크 재훈련(retrain) 직후
 * (위 한 줄이 이 필드 설명의 전부다.) */
	PCIE_LINK_RETRAIN,
	/* [한국어] 버스를 새로 추가하며 링크 상태를 처음 읽었을 때
 * (위 한 줄이 이 필드 설명의 전부다.) */
	PCIE_ADD_BUS,
	/* [한국어] 대역폭 제어(bwctrl) 기능을 켤 때
 * (위 한 줄이 이 필드 설명의 전부다.) */
	PCIE_BWCTRL_ENABLE,
	/* [한국어] 대역폭 변경 인터럽트(Link Bandwidth Management)로 알림을 받았을 때
 * (위 한 줄이 이 필드 설명의 전부다.) */
	PCIE_BWCTRL_IRQ,
	/* [한국어] 핫플러그 이벤트로 링크가 바뀌었을 때
 * (위 한 줄이 이 필드 설명의 전부다.) */
	PCIE_HOTPLUG,
};

/* [한국어]
 * __pcie_update_link_speed() - 읽어 온 Link Status 값으로 버스의 링크 정보를 갱신한다
 *
 * @bus: 갱신 대상 버스(이 버스로 내려가는 링크의 상태다).
 * @reason: 갱신 계기(위 enum) — 트레이스에 그대로 실린다.
 * @linksta: Link Status 레지스터 값(16비트).
 * @linksta2: Link Status 2 레지스터 값(16비트).
 * @return(): 없음.
 *
 * 왜 "__" 접두사인가: 이 함수는 레지스터를 직접 읽지 않고, 호출자가 이미
 * 읽어 둔 값을 받아 반영만 한다. 레지스터를 읽는 일까지 하는 판이
 * 아래 pcie_update_link_speed() 다. 핫플러그 인터럽트 처리처럼 이미 값을
 * 손에 쥔 경로가 config 읽기를 한 번 더 하지 않도록 나눠 둔 것이다.
 * 실행 컨텍스트: 호출자에 따라 프로세스 컨텍스트(열거)일 수도, 인터럽트
 * 후속 처리(핫플러그/대역폭 제어 IRQ)일 수도 있다. 락은 잡지 않는다 —
 * bus->cur_bus_speed 는 통계/표시용이라 순간적으로 어긋나도 무해하다.
 * 호출 체인:
 * drivers/pci/probe.c 의 pcie_update_link_speed() → [이 함수]
 * drivers/pci/hotplug/pciehp_hpc.c (핫플러그 링크 이벤트, reason=PCIE_HOTPLUG)
 * → [이 함수] → trace_pcie_link_event() */
static inline void __pcie_update_link_speed(struct pci_bus *bus,
					    enum pcie_link_change_reason reason,
					    u16 linksta, u16 linksta2)
{
	bus->cur_bus_speed = pcie_link_speed[linksta & PCI_EXP_LNKSTA_CLS]; /* [한국어] Link Status 의 Current Link Speed 필드([3:0])를 인덱스로 삼아 위 pcie_link_speed[] 표에서 enum 값을 꺼내 버스에 기록 */
	bus->flit_mode = (linksta2 & PCI_EXP_LNKSTA2_FLIT) ? 1 : 0; /* [한국어] Link Status 2 의 FLIT Mode Status 비트. FLIT(Flow Control Unit) 모드는 Gen6 에서 도입된 새 패킷 포맷으로, 켜지면 TLP 헤더 로그 해석 방식이 달라진다 — 그래서 AER 코드가 이 플래그를 본다 */

	trace_pcie_link_event(bus, /* [한국어] 트레이스포인트 발화. ftrace 로 링크 이벤트를 시간순으로 추적할 수 있게 한다 */
			     reason, /* [한국어] 갱신 계기(위 enum) */
			     FIELD_GET(PCI_EXP_LNKSTA_NLW, linksta), /* [한국어] PCI_EXP_LNKSTA_NLW 는 Negotiated Link Width 필드 마스크. FIELD_GET 이 마스크와 시프트를 함께 처리해 레인 수를 뽑아 준다(x1/x2/x4/x8/x16) */
			     linksta & PCI_EXP_LNKSTA_LINK_STATUS_MASK); /* [한국어] Link Status 의 링크 상태 비트들(Link Training, Data Link Layer Link Active 등)을 마스크해 함께 기록 */
}

void pcie_update_link_speed(struct pci_bus *bus, enum pcie_link_change_reason reason); /* [한국어] 레지스터를 직접 읽은 뒤 위 __pcie_update_link_speed 를 부르는 상위 판. 정의 drivers/pci/probe.c */

/* [한국어]
 * struct pci_sriov - SR-IOV PF 한 개의 상태 전부
 *
 * SR-IOV(Single Root I/O Virtualization)는 물리 함수 하나(PF)가 자신을
 * 여러 개의 가벼운 가상 함수(VF)로 복제해 보여 주는 PCIe 기능이다. 각 VF 는
 * 독립된 BDF 와 자기 BAR 를 가져 게스트 VM 에 직접 할당할 수 있다.
 * 이 구조체는 PF 의 pci_dev.sriov 에 매달리며, 관리자는 사실상
 * drivers/pci/iov.c 하나뿐이다. 값은 대부분 SR-IOV 확장 capability
 * (PCI_EXT_CAP_ID_SRIOV) 레지스터를 읽어 채운다 — 그래서 아래 필드 이름이
 * 스펙의 레지스터 이름과 거의 1:1로 대응한다.
 *
 * 생명주기:
 * drivers/pci/iov.c 의 pci_iov_init() → sriov_init() 에서 kzalloc() 되고 필드가 채워짐
 * → sysfs 의 sriov_numvfs 에 값을 쓰면 sriov_enable()/sriov_disable()
 * → pci_iov_release(iov.c) 에서 해제
 *
 * 동기화: 이 구조체 자체를 지키는 전용 락은 없다. VF 개수를 바꾸는 경로는
 * sysfs store 콜백이라 device_lock 아래에서 직렬화되고(iov.c 등),
 * 나머지 필드는 초기화 후 읽기 전용이다.
 *
 * NVMe 접점: drivers/nvme/host/pci.c 은
 * .sriov_configure = pci_sriov_configure_simple
 * 을 등록한다. 즉 NVMe 는 VF 관리를 PCI 코어에 통째로 위임하고, 사용자가
 * /sys/.../sriov_numvfs 에 숫자를 쓰면 코어가 이 구조체를 근거로 VF
 * pci_dev 들을 만들어 낸다. NVMe 드라이버 코드에는 VF 생성 로직이 없다. */
/* Single Root I/O Virtualization */
struct pci_sriov {
	/* [한국어] SR-IOV 확장 capability 의 config space 오프셋.
 * 역할: iov->pos + PCI_SRIOV_CTRL 처럼 SR-IOV 레지스터 접근의 기준점.
 * 설정자: drivers/pci/iov.c 의 sriov_init().
 * 읽는 자: iov.c 의 pci_iov_set_numvfs(), sriov_enable(), sriov_disable(),
 * sriov_restore_state(), pci_iov_update_resource() 등 거의 모든 함수.
 * 값 범위: 0x100 이상의 확장 capability 오프셋(0 이면 SR-IOV 없음).
 * 동기화: sriov_init() 에서 한 번 쓰고 이후 읽기 전용. */
	int		pos;		/* Capability position */
	/* [한국어] VF BAR 중 실제로 크기가 0 이 아닌 것의 개수.
 * 설정자: iov.c 의 sriov_init().
 * 읽는 자: iov.c 의 sriov_enable() — VF 활성화 직전에 다시 세어 보고 값이
 * 달라졌으면 실패로 처리한다. 그 사이 리소스 배정이 바뀌었다는 뜻이기 때문.
 * 값 범위: 0~PCI_SRIOV_NUM_BARS(6).
 * 동기화: 초기화 후 불변. */
	int		nres;		/* Number of resources */
	/* [한국어] SR-IOV Capabilities 레지스터(오프셋 pos + PCI_SRIOV_CAP)의 사본.
 * 설정자: iov.c 의 sriov_init() 이 pci_read_config_dword() 로 읽어 담는다.
 * 읽는 자: iov.c 의 sriov_enable() 이 PCI_SRIOV_CAP_VFM(VF Migration) 비트를
 * 검사한다 — 이 비트가 없으면 활성화할 VF 수가 InitialVFs 를 넘을 수 없다.
 * 값 범위: 스펙이 정의한 capability 비트 조합.
 * 동기화: 초기화 후 불변. */
	u32		cap;		/* SR-IOV Capabilities */
	/* [한국어] SR-IOV Control 레지스터의 소프트웨어 사본.
 * 설정자: iov.c 의 sriov_init(초기값), sriov_enable(VFE|MSE 를 세움),
 * sriov_disable()(그 비트들을 지움), pci_sriov_set_totalvfs().
 * 읽는 자: 같은 함수들이 이 값을 실제 레지스터에 되쓰고,
 * sriov_restore_state() 가 resume 후 복원에 쓴다.
 * 값 범위: PCI_SRIOV_CTRL_VFE(VF Enable), _MSE(VF Memory Space Enable),
 * _ARI 비트 조합.
 * 동기화: 값을 바꾸는 경로가 sysfs store 콜백이라 device_lock 으로 직렬화된다. */
	u16		ctrl;		/* SR-IOV Control */
	/* [한국어] PF 가 만들 수 있는 VF 의 하드웨어 상한(TotalVFs 레지스터).
 * 설정자: iov.c 의 sriov_init().
 * 읽는 자: sriov_enable(상한 검사), pci_iov_remove(driver_max_VFs 복원),
 * pci_sriov_set_totalvfs(드라이버가 낮추려는 값의 검증),
 * compute_max_vf_buses().
 * 값 범위: 1~65535(하드웨어가 광고한 값).
 * 동기화: 초기화 후 불변. */
	u16		total_VFs;	/* Total VFs associated with the PF */
	/* [한국어] 펌웨어/하드웨어가 처음 광고한 InitialVFs 값.
 * 설정자: iov.c 의 sriov_enable() 이 PCI_SRIOV_INITIAL_VF 를 읽어 검증한 뒤 저장.
 * 읽는 자: sriov_enable() — VF Migration 을 지원하지 않는 장치에서는 활성화
 * 가능한 VF 수가 이 값을 넘을 수 없다.
 * 값 범위: 0~total_VFs.
 * 동기화: 초기화 후 불변. */
	u16		initial_VFs;	/* Initial VFs associated with the PF */
	/* [한국어] 지금 실제로 켜져 있는 VF 개수.
 * 설정자: iov.c 의 sriov_enable(요청받은 개수), sriov_disable(0).
 * 읽는 자: sriov_numvfs_show() 와 sriov_numvfs_store()(중복 설정 방지와 조회),
 * sriov_del_vfs(정리 루프), pci_num_vf(외부 조회),
 * sriov_release() 와 pci_iov_remove()("드라이버가 VF 를 켜 둔 채 떠났다" 경고),
 * sriov_restore_state(resume 후 개수 복원).
 * 값 범위: 0~total_VFs.
 * 동기화: sysfs store 경로의 device_lock. */
	u16		num_VFs;	/* Number of VFs available */
	/* [한국어] 첫 VF 의 Routing ID 오프셋(VF Offset 레지스터).
 * 설정자: iov.c 의 pci_iov_set_numvfs() — NumVFs 를 쓴 직후 하드웨어가 갱신해
 * 준 값을 다시 읽어 온다(NumVFs 에 따라 값이 달라지기 때문).
 * 읽는 자: pci_iov_virtfn_bus() 와 pci_iov_virtfn_devfn()(VF 의 BDF 계산),
 * pci_iov_vf_id()(역산), compute_max_vf_buses(), sriov_offset_show().
 * 왜 필요한가: VF 의 BDF 는 PF 의 BDF 에 이 오프셋을 더해 결정된다. 오프셋이
 * 커서 devfn 8비트를 넘어가면 VF 가 다음 버스 번호에 나타난다.
 * 동기화: VF 활성화 경로에서만 갱신되며 device_lock 아래에서 직렬화된다. */
	u16		offset;		/* First VF Routing ID offset */
	/* [한국어] VF 사이의 Routing ID 간격(VF Stride 레지스터).
 * 설정자: iov.c 의 pci_iov_set_numvfs(offset 과 함께 되읽는다).
 * 읽는 자: pci_iov_virtfn_bus()/pci_iov_virtfn_devfn() 이
 * PF devfn + offset + stride * vf_id 로 각 VF 의 devfn 을 만든다.
 * pci_iov_vf_id() 는 같은 식을 뒤집어 VF 번호를 되찾고,
 * sriov_stride_show() 는 sysfs 로 노출한다.
 * 값 범위: VF 가 2개 이상이면 0 이 될 수 없다(pci_iov_set_numvfs() 가 검증).
 * 동기화: offset 과 같다. */
	u16		stride;		/* Following VF stride */
	/* [한국어] VF 가 광고할 Device ID(VF Device ID 레지스터).
 * 설정자: iov.c 의 sriov_init().
 * 읽는 자: pci_iov_scan_device() 가 새로 만든 VF 의 pci_dev.device 에 넣고,
 * pci_vfs_assigned() 와 sriov_vf_device_show() 가 조회에 쓴다.
 * 왜 별도 필드인가: VF 의 config space 에서는 Device ID 를 직접 읽을 수 없어
 * PF 의 SR-IOV capability 에서 가져와야 한다.
 * 동기화: 초기화 후 불변. */
	u16		vf_device;	/* VF device ID */
	/* [한국어] VF BAR 정렬에 쓰는 시스템 페이지 크기(System Page Size 레지스터).
 * 설정자: iov.c 의 sriov_init().
 * 읽는 자: sriov_restore_state() 가 resume 후 PCI_SRIOV_SYS_PGSIZE 에 되쓴다.
 * 왜 필요한가: VF BAR 는 이 크기 단위로 정렬되어야 게스트 VM 에 페이지 단위로
 * 안전하게 매핑된다.
 * 동기화: 초기화 후 불변. */
	u32		pgsz;		/* Page size for BAR alignment */
	/* [한국어] Function Dependency Link — 이 PF 가 다른 PF 와 함께 움직여야 할 때
 * 그 대표 PF 를 가리킨다.
 * 설정자: iov.c 의 sriov_init() 이 PCI_SRIOV_FUNC_LINK 를 읽어 PCI_DEVFN()
 * 형태의 devfn 으로 바꿔 저장한다.
 * 읽는 자: sriov_enable()/sriov_disable() 이 자기 devfn 과 다르면 대표 PF 를
 * 찾아가 그 PF 를 통해 VF 를 켜고 끈다.
 * 값 범위: 같은 슬롯 안의 devfn.
 * 동기화: 초기화 후 불변. */
	u8		link;		/* Function Dependency Link */
	/* [한국어] VF 들이 잡아먹는 버스 번호의 최대 개수.
 * 설정자: iov.c 의 compute_max_vf_buses() 가 가능한 VF 수를 훑으며 최댓값을 남긴다.
 * 읽는 자: pci_iov_bus_range() 가 열거 단계에서 버스 번호를 얼마나 예약할지 정한다.
 * 왜 필요한가: VF 가 많으면 PF 의 버스를 넘어 다음 버스 번호까지 쓰므로,
 * 미리 자리를 비워 두지 않으면 나중에 VF 를 켤 수 없다.
 * 동기화: 초기화 후 불변. */
	u8		max_VF_buses;	/* Max buses consumed by VFs */
	/* [한국어] 드라이버가 허용하는 VF 개수 상한.
 * 설정자: iov.c 의 sriov_init() 이 total_VFs 로 초기화하고, PF 드라이버가
 * pci_sriov_set_totalvfs() 로 더 낮출 수 있다. pci_iov_remove() 는
 * 드라이버가 떠날 때 다시 total_VFs 로 되돌린다.
 * 읽는 자: pci_sriov_get_totalvfs() 와 sriov_totalvfs sysfs 조회.
 * 값 범위: 0~total_VFs.
 * 동기화: PF 드라이버 바인딩/해제 시점에만 바뀐다. */
	u16		driver_max_VFs;	/* Max num VFs driver supports */
	/* [한국어] 이 SR-IOV 그룹에서 가장 번호가 낮은 PF.
 * 설정자: iov.c 의 sriov_init() — 대표 PF 가 따로 있으면 pci_dev_get() 으로
 * 참조를 잡고, 아니면 자기 자신을 넣는다.
 * 읽는 자: sriov_release() 가 self 와 다를 때만 pci_dev_put() 한다.
 * 왜 필요한가: Function Dependency Link 로 묶인 PF 들은 VF 활성화를 대표 PF 를
 * 통해서만 할 수 있다.
 * 동기화: 참조 계수로 수명을 보장. */
	struct pci_dev	*dev;		/* Lowest numbered PF */
	/* [한국어] 이 구조체를 소유한 PF 자신.
 * 설정자: iov.c 의 sriov_init().
 * 읽는 자: sriov_release() 가 dev 와 비교해 참조 해제 여부를 정한다.
 * 값 범위: 항상 유효한 PF 포인터. 자기 자신이므로 참조 계수를 올리지 않는다.
 * 동기화: 초기화 후 불변. */
	struct pci_dev	*self;		/* This PF */
	/* [한국어] VF 의 Class Code 캐시.
 * 설정자: iov.c 의 pci_read_vf_config_common() 이 첫 VF 의 config 0x08 을 읽어
 * PF 의 sriov 에 담는다.
 * 읽는 자: 이후 만들어지는 VF pci_dev 들이 config 를 다시 읽지 않고 이 값을 쓴다.
 * 왜 캐시하는가: 모든 VF 가 같은 값이므로 VF 마다 config 를 읽을 이유가 없다.
 * 동기화: 첫 VF 생성 시 한 번 쓰고 이후 읽기 전용. */
	u32		class;		/* VF device */
	/* [한국어] VF 의 Header Type 캐시(config 0x0e).
 * 설정자/읽는 자/동기화: 위 class 필드와 동일한 경로. */
	u8		hdr_type;	/* VF header type */
	/* [한국어] VF 의 Subsystem Vendor ID 캐시(config 0x2c).
 * 설정자/읽는 자/동기화: 위 class 필드와 동일한 경로. */
	u16		subsystem_vendor; /* VF subsystem vendor */
	/* [한국어] VF 의 Subsystem ID 캐시(config 0x2e).
 * 설정자/읽는 자/동기화: 위 class 필드와 동일한 경로. */
	u16		subsystem_device; /* VF subsystem device */
	/* [한국어] VF BAR 6개 각각의 크기.
 * 설정자: iov.c 의 sriov_init() 이 resource_size() 로 채우고,
 * pci_iov_resource_set_size() 가 VF Resizable BAR 변경 시 갱신한다.
 * 읽는 자: pci_iov_resource_size() 가 resource[] 인덱스를 VF BAR 번호로 바꿔
 * 조회하고, sriov_restore_vf_rebar_state() 가 현재 크기를 rebar 코드값으로 환산한다.
 * 값 범위: 2의 거듭제곱 바이트 수(0 이면 그 VF BAR 미구현).
 * 배열 크기 PCI_SRIOV_NUM_BARS 는 6 — VF 도 표준 장치처럼 BAR 를 6칸 갖는다.
 * 동기화: 초기화와 rebar 변경 시점에만 쓰인다. */
	resource_size_t	barsz[PCI_SRIOV_NUM_BARS];	/* VF BAR size */
	/* [한국어] VF Resizable BAR 확장 capability 의 오프셋(없으면 0).
 * 설정자: iov.c 의 sriov_init() 이
 * pci_find_ext_capability(dev, PCI_EXT_CAP_ID_VF_REBAR) 결과를 담는다.
 * 읽는 자: 위에서 정의한 pci_iov_vf_rebar_cap() 을 거쳐 iov.c 와
 * drivers/pci/rebar.c 가 쓴다.
 * 값 범위: 0x100 이상 또는 0.
 * 동기화: 초기화 후 불변. */
	u16		vf_rebar_cap;	/* VF Resizable BAR capability offset */
	/* [한국어] 새로 만들어진 VF 에 드라이버를 자동으로 바인딩할지.
 * 설정자: iov.c 의 sriov_init(true 로 초기화),
 * sriov_drivers_autoprobe_store(sysfs 쓰기), pci_vf_drivers_autoprobe(API).
 * 읽는 자: sriov_drivers_autoprobe_show() 와 VF 생성 경로,
 * pci_restore_iov_state().
 * 왜 끄는가: VF 를 호스트 드라이버에 붙이지 않고 곧바로 VFIO 로 게스트에
 * 넘기고 싶을 때 미리 꺼 둔다.
 * 동기화: sysfs store 의 device_lock. */
	bool		drivers_autoprobe; /* Auto probing of VFs by driver */
};

/* [한국어] --- DOE(Data Object Exchange), CONFIG_PCI_DOE ---
 * DOE 는 config space 의 작은 창(확장 capability)을 통해 장치와 임의 크기의
 * "데이터 객체"를 주고받는 메일박스 프로토콜이다. CMA/SPDM 기반 장치 인증,
 * CXL 의 CDAT(성능 서술 테이블) 조회 등이 이 위에서 돈다.
 * 기능이 꺼지면 세 함수 모두 빈 스텁이 되어, 장치 생성/제거/연결끊김 경로가
 * #ifdef 없이 그대로 컴파일된다. 특히 pci_doe_disconnected() 는 아래
 * pci_dev_set_disconnected 안에서 무조건 호출되므로 스텁이 반드시 필요하다. */
#ifdef CONFIG_PCI_DOE
void pci_doe_init(struct pci_dev *pdev); /* [한국어] DOE 확장 capability 를 찾아 메일박스를 초기화한다. 정의 drivers/pci/doe.c */
void pci_doe_destroy(struct pci_dev *pdev); /* [한국어] 장치 제거 시 DOE 자원을 해제한다. 정의 drivers/pci/doe.c */
void pci_doe_disconnected(struct pci_dev *pdev); /* [한국어] 장치가 사라졌을 때 진행 중인 DOE 요청을 실패 처리한다. 정의 drivers/pci/doe.c */
#else /* [한국어] CONFIG_PCI_DOE 가 꺼진 경우 */
static inline void pci_doe_init(struct pci_dev *pdev) { } /* [한국어] 스텁: 초기화할 메일박스가 없다 */
static inline void pci_doe_destroy(struct pci_dev *pdev) { } /* [한국어] 스텁: 해제할 것이 없다 */
static inline void pci_doe_disconnected(struct pci_dev *pdev) { } /* [한국어] 스텁: 아래 pci_dev_set_disconnected 가 무조건 부르므로 반드시 존재해야 한다 */
#endif /* [한국어] CONFIG_PCI_DOE 분기 종료 */

/* [한국어] --- NPEM(Native PCIe Enclosure Management), CONFIG_PCI_NPEM ---
 * NPEM 은 드라이브 베이의 상태 LED(정상/장애/위치확인/재구축 등)를 표준
 * 방식으로 제어하는 확장 capability 다.
 * NVMe 접점: 서버 백플레인에 꽂힌 NVMe SSD 의 장애 LED 를 켜는 일이
 * 이 계층의 일이다. 다만 이 헤더는 그 등록/해제 진입점만 선언하며,
 * NVMe 드라이버가 직접 부르는 함수는 아니다(호출자는 PCI 코어). */
#ifdef CONFIG_PCI_NPEM
void pci_npem_create(struct pci_dev *dev); /* [한국어] NPEM capability 가 있으면 LED 제어 인터페이스를 등록한다. 정의 drivers/pci/npem.c */
void pci_npem_remove(struct pci_dev *dev); /* [한국어] 그 인터페이스를 해제한다. 정의 drivers/pci/npem.c */
#else /* [한국어] CONFIG_PCI_NPEM 이 꺼진 경우 */
static inline void pci_npem_create(struct pci_dev *dev) { } /* [한국어] 스텁: LED 제어를 제공하지 않는다 */
static inline void pci_npem_remove(struct pci_dev *dev) { } /* [한국어] 스텁: 해제할 것이 없다 */
#endif /* [한국어] CONFIG_PCI_NPEM 분기 종료 */

/* [한국어] --- DOE 의 sysfs 노출 (CONFIG_PCI_DOE + CONFIG_SYSFS) ---
 * 두 조건이 모두 켜져야 의미가 있으므로 AND 로 갈린다. DOE 기능은 있는데
 * sysfs 가 없는 커널에서는 프로토콜은 돌지만 사용자에게 보여 줄 곳이 없다. */
#if defined(CONFIG_PCI_DOE) && defined(CONFIG_SYSFS)
void pci_doe_sysfs_init(struct pci_dev *pci_dev); /* [한국어] DOE 상태를 보여 주는 sysfs 파일을 만든다. 정의 drivers/pci/doe.c */
void pci_doe_sysfs_teardown(struct pci_dev *pdev); /* [한국어] 그 파일들을 없앤다. 정의 drivers/pci/doe.c */
#else /* [한국어] 둘 중 하나라도 꺼진 경우 */
static inline void pci_doe_sysfs_init(struct pci_dev *pdev) { } /* [한국어] 스텁: 보여 줄 곳이 없다 */
static inline void pci_doe_sysfs_teardown(struct pci_dev *pdev) { } /* [한국어] 스텁: 지울 것이 없다 */
#endif /* [한국어] 조건 분기 종료 */

/* [한국어] --- IDE(Integrity and Data Encryption), CONFIG_PCI_IDE ---
 * PCIe IDE 는 링크 위를 흐르는 TLP 자체를 암호화·무결성 보호하는 기능이다
 * (같은 이름의 옛 ATA/IDE 디스크 인터페이스와는 전혀 관계가 없다 — 이름
 * 충돌에 주의).
 * 호스트 브리지 단위 초기화(pci_ide_init_host_bridge())와 장치 단위
 * 초기화/해제가 나뉘어 있는 이유는, 스트림 자원이 브리지에 속하기 때문이다. */
#ifdef CONFIG_PCI_IDE
void pci_ide_init(struct pci_dev *dev); /* [한국어] 장치의 IDE capability 를 초기화한다. 정의 drivers/pci/ide.c */
void pci_ide_init_host_bridge(struct pci_host_bridge *hb); /* [한국어] 호스트 브리지 단위로 IDE 스트림 자원을 준비한다. 정의 drivers/pci/ide.c */
void pci_ide_destroy(struct pci_dev *dev); /* [한국어] 장치 제거 시 IDE 자원을 해제한다. 정의 drivers/pci/ide.c */
extern const struct attribute_group pci_ide_attr_group; /* [한국어] IDE 상태를 노출하는 sysfs 속성 그룹 */
#else /* [한국어] CONFIG_PCI_IDE 가 꺼진 경우 */
static inline void pci_ide_init(struct pci_dev *dev) { } /* [한국어] 스텁: 링크 암호화를 제공하지 않는다 */
static inline void pci_ide_init_host_bridge(struct pci_host_bridge *hb) { } /* [한국어] 스텁: 브리지 쪽 준비도 없다 */
static inline void pci_ide_destroy(struct pci_dev *dev) { } /* [한국어] 스텁: 해제할 것이 없다 */
#endif /* [한국어] CONFIG_PCI_IDE 분기 종료 */

/* [한국어] --- TSM(TEE Security Manager), CONFIG_PCI_TSM ---
 * 기밀 컴퓨팅(Confidential Computing)에서 장치를 신뢰 실행 환경에 안전하게
 * 연결하기 위한 계층이다. 장치 인증(위 DOE/SPDM)과 링크 암호화(위 IDE)를
 * 묶어 "이 장치를 게스트 TEE 에 직접 붙여도 되는가"를 관리한다. */
#ifdef CONFIG_PCI_TSM
void pci_tsm_init(struct pci_dev *pdev); /* [한국어] TSM 관련 상태를 초기화한다. 정의 drivers/pci/tsm.c */
void pci_tsm_destroy(struct pci_dev *pdev); /* [한국어] 해제한다. 정의 drivers/pci/tsm.c */
extern const struct attribute_group pci_tsm_attr_group; /* [한국어] TSM 상태를 노출하는 sysfs 속성 그룹 */
extern const struct attribute_group pci_tsm_auth_attr_group; /* [한국어] 장치 인증 결과를 노출하는 sysfs 속성 그룹 */
#else /* [한국어] CONFIG_PCI_TSM 이 꺼진 경우 */
static inline void pci_tsm_init(struct pci_dev *pdev) { } /* [한국어] 스텁: TEE 연동을 제공하지 않는다 */
static inline void pci_tsm_destroy(struct pci_dev *pdev) { } /* [한국어] 스텁: 해제할 것이 없다 */
#endif /* [한국어] CONFIG_PCI_TSM 분기 종료 */

/* [한국어] 아래 함수의 한국어 해설:
 * pci_dev_set_io_state() - 장치의 오류 채널 상태를 원자적으로 전이시킨다
 *
 * @dev: 상태를 바꿀 장치.
 * @new: 목표 상태.
 * @return(): 요청한 상태로 바뀌었으면(또는 이미 그 상태였고 영구 실패가
 * 아니면) true. 영구 실패 상태에 갇혀 있으면 false.
 *
 * 상태 세 가지(enum pci_channel_state, <linux/pci.h>):
 * pci_channel_io_normal 정상 — I/O 가능
 * pci_channel_io_frozen 동결 — 오류가 감지되어 I/O 가 막힘. 복구 가능
 * pci_channel_io_perm_failure 영구 실패 — 장치가 사라졌거나 복구 불가
 *
 * 전이 규칙: perm_failure 는 흡수 상태(absorbing state)다. 한 번 들어가면
 * 절대 못 나온다. 그래서 아래 cmpxchg 들은 모두 "기대한 이전 상태"를 명시해
 * 놓고, 결과가 perm_failure 였으면 실패로 보고한다.
 *
 * 왜 원자적 연산인가: error_state 는 AER 인터럽트 후속 워크큐, DPC 처리,
 * 핫플러그 제거 경로, 그리고 그것을 읽는 드라이버 I/O 경로가 락 없이
 * 동시에 건드린다. 공통 락이 없으므로 xchg/cmpxchg 로 경쟁을 해결한다.
 * (읽는 쪽인 pci_dev_is_disconnected 도 같은 이유로 READ_ONCE 를 쓴다.)
 *
 * 실행 컨텍스트: 인터럽트 후속 워크큐, 프로세스 컨텍스트 모두 가능. 잠들지 않는다.
 *
 * 호출 체인:
 *   drivers/pci/pcie/err.c 의 report_error_detected() (오류 감지 시 frozen 으로)
 *     → [이 함수]
 *   drivers/pci/pcie/err.c 의 report_slot_reset()/report_resume()
 *     (복구 성공 시 normal 로) → [이 함수]
 *   아래 pci_dev_set_disconnected() → [이 함수] (perm_failure 로)
 *
 * NVMe 접점: 이 함수가 쓴 값을 NVMe 가 두 곳에서 읽는다.
 *   drivers/nvme/host/pci.c 의 nvme_timeout() 이 pci_dev_is_disconnected(pdev) 로
 *     "config 접근조차 불가"인지 먼저 확인하고,
 *   같은 nvme_timeout() 이 pci_channel_offline(pdev) 로 frozen 인지 보아,
 *     frozen 이면 드라이버가 리셋을 걸지 않고 slot_reset 복구를 기다린다.
 *   그리고 상태를 frozen 으로 만든 drivers/pci/pcie/err.c 의 복구 흐름이
 *   NVMe 가 등록한 .error_detected/.slot_reset 콜백
 *   (drivers/nvme/host/pci.c 의 nvme_err_handler)을 차례로 부른다. */
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
	pci_channel_state_t old; /* [한국어] 비교·교환의 결과로 받은 "이전 값"을 담을 변수 */

	switch (new) { /* [한국어] 목표 상태에 따라 갈린다 */
	case pci_channel_io_perm_failure: /* [한국어] 영구 실패로 못 박는 경우 */
		xchg(&dev->error_state, pci_channel_io_perm_failure); /* [한국어] xchg 는 조건 없이 덮어쓴다 — 어떤 상태에 있었든 영구 실패로 간다. 이 전이만 무조건 성공하므로 아래에서 곧바로 true 를 반환한다 */
		return true; /* [한국어] 항상 성공 */
	case pci_channel_io_frozen: /* [한국어] 동결 상태로 보내는 경우(오류 감지) */
		old = cmpxchg(&dev->error_state, pci_channel_io_normal, /* [한국어] normal 일 때만 frozen 으로 바꾼다. 이미 frozen 이면 아무 일도 일어나지 않고 old 에 frozen 이 담긴다 */
			      pci_channel_io_frozen);
		return old != pci_channel_io_perm_failure; /* [한국어] old 가 perm_failure 만 아니면 성공으로 본다 — 이미 frozen 이었던 경우도 "원하는 상태에 있다"이므로 참이다 */
	case pci_channel_io_normal: /* [한국어] 정상으로 되돌리는 경우(복구 성공) */
		old = cmpxchg(&dev->error_state, pci_channel_io_frozen, /* [한국어] frozen 일 때만 normal 로 되돌린다. perm_failure 는 이 cmpxchg 에 걸리지 않아 그대로 남는다 */
			      pci_channel_io_normal);
		return old != pci_channel_io_perm_failure; /* [한국어] perm_failure 였다면 복구 실패로 보고 */
	default: /* [한국어] 위 세 가지가 아닌 값(예: 정의되지 않은 상태) */
		return false; /* [한국어] 전이하지 않고 실패 반환 */
	}
}

/* [한국어]
 * pci_dev_set_disconnected() - 장치를 "영구 실패"로 못 박고 부속 자원을 정리한다
 *
 * @dev: 사라진 장치.
 * @unused: 쓰이지 않는다. 시그니처가 int (*)(struct pci_dev *, void *) 여야
 * pci_walk_bus() 의 콜백으로 그대로 넘길 수 있기 때문에 존재한다.
 * @return(): 항상 0. pci_walk_bus() 는 콜백이 0 이 아닌 값을 주면 순회를
 * 멈추므로, 0 은 "끝까지 계속하라"는 뜻이다.
 *
 * 왜 필요한가: 카드가 뽑히거나 링크가 죽으면 그 아래 모든 장치의 config
 * 접근이 all-1 을 돌려주기 시작한다. 이 사실을 각 pci_dev 에 미리 박아 두면,
 * 드라이버들이 쓸데없이 타임아웃을 기다리지 않고 즉시 포기할 수 있다.
 * 그래서 이 함수는 보통 pci_walk_bus() 로 하위 트리 전체에 적용된다.
 * 실행 컨텍스트: 핫플러그 이벤트 처리, 드라이버 제거 경로. 잠들지 않는다.
 * 호출 체인:
 * drivers/pci/hotplug/pciehp_core.c 와 pciehp_pci.c 의 pciehp_unconfigure_device(),
 * drivers/pci/hotplug/acpiphp_glue.c, drivers/pci/pci-driver.c
 * → pci_walk_bus(..., pci_dev_set_disconnected(), NULL) → [이 함수]
 * → pci_dev_set_io_state() / pci_doe_disconnected()
 * NVMe 접점: 이 함수가 세운 상태를 drivers/nvme/host/pci.c 의
 * pci_dev_is_disconnected 가 읽는다 — 뽑힌 SSD 에 대해 NVMe 가 리셋을
 * 시도하지 않고 곧바로 정리로 넘어가는 근거가 여기서 만들어진다. */
static inline int pci_dev_set_disconnected(struct pci_dev *dev, void *unused)
{
	pci_dev_set_io_state(dev, pci_channel_io_perm_failure); /* [한국어] 영구 실패로 표시 — 반환값은 무시한다. 어차피 이 전이는 항상 성공하기 때문 */
	pci_doe_disconnected(dev); /* [한국어] DOE 메일박스에 걸려 있던 요청들을 즉시 실패시킨다. CONFIG_PCI_DOE 가 꺼져 있으면 위의 빈 스텁이라 비용이 0 이다 */

	return 0; /* [한국어] pci_walk_bus() 콜백 규약상 0 = 계속 순회 */
}

/* [한국어] --- pci_dev.priv_flags 비트 번호 정의 ---
 * priv_flags 는 unsigned long 하나에 여러 개의 상태 비트를 모아 둔 필드이고,
 * 아래 상수들은 "몇 번째 비트인가"를 나타낸다(마스크가 아니라 비트 번호다 —
 * set_bit()/test_bit 계열이 번호를 받기 때문).
 * 원자적 비트 연산으로만 다루므로 별도 락 없이 여러 문맥이 동시에 만질 수 있다. */
/* pci_dev priv_flags */
#define PCI_DEV_ADDED 0 /* [한국어] 비트 0: 이 장치가 버스에 정식 추가되었다(pci_bus_add_device() 완료). 중복 추가/제거를 막는 데 쓴다 */
#define PCI_DPC_RECOVERED 1 /* [한국어] 비트 1: DPC 로부터 복구를 마쳤다. drivers/pci/pcie/dpc.c 가 세우고 pci_dpc_recovered() 가 읽는다 */
#define PCI_DPC_RECOVERING 2 /* [한국어] 비트 2: DPC 복구가 진행 중이다. 복구 중에 다른 리셋이 끼어들지 못하게 막는 용도 */
#define PCI_DEV_REMOVED 3 /* [한국어] 비트 3: 이미 제거 절차에 들어갔다. 두 경로가 동시에 지우는 것을 막는다 */
#define PCI_LINK_CHANGED 4 /* [한국어] 비트 4: 링크 상태가 바뀌었음이 관측되었다 */
#define PCI_LINK_CHANGING 5 /* [한국어] 비트 5: 링크 상태 변경 처리가 진행 중이다 */
#define PCI_LINK_LBMS_SEEN	6 /* [한국어] 비트 6: LBMS(Link Bandwidth Management Status)가 관측되었다. 링크가 스스로 속도를 낮춘 흔적으로, 대역폭 제어 코드가 본다 */
#define PCI_DEV_ALLOW_BINDING 7 /* [한국어] 비트 7: 이 장치에 드라이버를 붙여도 된다. 열거가 끝나기 전에 드라이버가 붙는 것을 막기 위한 관문 */

/* [한국어]
 * pci_dev_assign_added() - 장치가 버스에 정식 등록되었음을 표시한다
 *
 * @dev: 방금 등록을 마친 장치.
 * @return(): 없음.
 *
 * 메모리 배리어가 앞뒤로 붙은 이유가 이 함수의 핵심이다.
 * set_bit 자체는 원자적이지만 순서를 보장하지는 않는다. 등록 과정에서
 * 채워 넣은 다른 필드들(sysfs 노드, 드라이버 바인딩 상태 등)이 이 비트보다
 * 먼저 다른 CPU 에 보여야, ADDED 를 본 쪽이 반쯤 만들어진 장치를 만지지
 * 않는다. smp_mb__before_atomic 이 그 순서를 강제하고,
 * smp_mb__after_atomic 은 이 비트를 본 뒤의 작업이 앞당겨지지 않게 한다.
 * 실행 컨텍스트: 장치 추가 경로(프로세스 컨텍스트, pci_rescan_remove_lock 보유).
 * 호출 체인: drivers/pci/bus.c 의 pci_bus_add_device() → [이 함수] */
static inline void pci_dev_assign_added(struct pci_dev *dev)
{
	smp_mb__before_atomic(); /* [한국어] 비트를 세우기 전 배리어 — 이전에 쓴 값들이 먼저 보이도록 순서를 강제한다 */
	set_bit(PCI_DEV_ADDED, &dev->priv_flags); /* [한국어] PCI_DEV_ADDED 비트를 원자적으로 세운다 */
	smp_mb__after_atomic(); /* [한국어] 비트를 세운 뒤 배리어 — 이후 작업이 앞당겨지지 않게 한다 */
}

/* [한국어]
 * pci_dev_test_and_clear_added() - ADDED 비트를 원자적으로 읽고 지운다
 *
 * @dev: 제거하려는 장치.
 * @return(): 지우기 전에 비트가 서 있었으면 true.
 *
 * 왜 test_and_clear 인가: "등록된 적 있는가"를 확인하는 것과 "이제 등록
 * 해제한다"를 표시하는 것을 한 번의 원자적 연산으로 합쳐, 두 스레드가
 * 동시에 제거를 시도해도 실제 제거 작업은 한 번만 일어나게 한다.
 * 여기서는 배리어가 없다 — 제거 경로는 pci_rescan_remove_lock 아래에서만
 * 실행되므로 락이 순서를 보장한다.
 * 호출 체인: drivers/pci/remove.c 의 pci_stop_bus_device() → [이 함수]
 * (false 면 아무것도 하지 않고 즉시 반환) */
static inline bool pci_dev_test_and_clear_added(struct pci_dev *dev)
{
	return test_and_clear_bit(PCI_DEV_ADDED, &dev->priv_flags); /* [한국어] ADDED 비트를 읽고 지우는 원자적 연산. 반환값은 지우기 전의 상태 */
}

/* [한국어]
 * pci_dev_is_added() - 이 장치가 버스에 정식 등록된 상태인지 확인
 *
 * @dev: 검사할 장치(const — 상태를 바꾸지 않는다).
 * @return(): ADDED 비트가 서 있으면 true.
 *
 * 왜 필요한가: 열거 도중이거나 이미 제거된 장치를 다시 추가/제거하려는
 * 시도를 걸러 내기 위해서다. 특히 핫플러그와 버스 재스캔이 겹칠 때
 * 같은 장치를 두 번 등록하는 사고를 막는다.
 * 호출 체인:
 * drivers/pci/bus.c (이미 추가된 장치는 건너뛴다),
 * drivers/pci/bus.c (아직 안 된 장치만 추가),
 * drivers/pci/hotplug/acpiphp_glue.c → [이 함수] */
static inline bool pci_dev_is_added(const struct pci_dev *dev)
{
	return test_bit(PCI_DEV_ADDED, &dev->priv_flags); /* [한국어] const 포인터이므로 읽기만 한다. test_bit 은 원자적 읽기 */
}

/* [한국어]
 * pci_dev_test_and_set_removed() - REMOVED 비트를 원자적으로 읽고 세운다
 *
 * @dev: 제거 중인 장치.
 * @return(): 세우기 전에 이미 서 있었으면 true(= 다른 경로가 먼저 시작했다).
 *
 * 왜 필요한가: 장치 제거는 sysfs 의 remove 파일, 핫플러그 인터럽트,
 * 드라이버 언로드 등 여러 경로에서 시작될 수 있다. 이 함수가 true 를
 * 돌려준 쪽은 "내가 늦었다"는 뜻이므로 조용히 물러난다 — 이중 해제를 막는
 * 관문이다.
 * 호출 체인: drivers/pci/remove.c → [이 함수] */
static inline bool pci_dev_test_and_set_removed(struct pci_dev *dev)
{
	return test_and_set_bit(PCI_DEV_REMOVED, &dev->priv_flags); /* [한국어] REMOVED 비트를 세우고 이전 값을 돌려준다. 이전 값이 참이면 이미 다른 경로가 제거를 시작했다는 뜻 */
}

/* [한국어]
 * pci_dev_allow_binding() - 이제 이 장치에 드라이버를 붙여도 좋다고 표시한다
 *
 * @dev: 열거와 리소스 배정이 모두 끝난 장치.
 * @return(): 없음.
 *
 * 왜 필요한가: 장치가 만들어지자마자 드라이버가 붙으면, BAR 주소가 아직
 * 배정되지 않았거나 sysfs 노드가 준비되지 않은 상태에서 probe 가 돌 수 있다.
 * 그래서 커널은 "준비 완료" 표시를 따로 두고, 그 전에는 바인딩을 거부한다.
 * 배리어가 없는 이유: 이 경로도 pci_rescan_remove_lock 아래에서 실행된다.
 * 호출 체인: drivers/pci/bus.c 의 pci_bus_add_device() → [이 함수]
 * (같은 함수 안에서 곧이어 pci_dev_assign_added() 도 불린다) */
static inline void pci_dev_allow_binding(struct pci_dev *dev)
{
	set_bit(PCI_DEV_ALLOW_BINDING, &dev->priv_flags); /* [한국어] ALLOW_BINDING 비트를 세운다. 배리어가 없는 것은 상위 락이 순서를 보장하기 때문 */
}

/* [한국어]
 * pci_dev_binding_disallowed() - 아직 드라이버를 붙이면 안 되는 장치인지 확인
 *
 * @dev: 바인딩을 시도하려는 장치.
 * @return(): ALLOW_BINDING 비트가 아직 서지 않았으면 true(= 붙이면 안 됨).
 *
 * 위 pci_dev_allow_binding 의 짝이며, 이름 그대로 부정형이라 반환값의
 * 의미가 뒤집혀 있다는 점에 주의한다.
 * 호출 체인: drivers/pci/pci-driver.c (probe 직전 관문) → [이 함수]
 * NVMe 접점: NVMe 드라이버의 nvme_probe 가 불리기 전에 이 관문을 통과해야
 * 한다. 즉 NVMe 가 pci_resource_start(pdev, 0) 을 읽는 시점에는 BAR 배정이
 * 이미 끝나 있음이 이 관문으로 보장된다. */
static inline bool pci_dev_binding_disallowed(struct pci_dev *dev)
{
	return !test_bit(PCI_DEV_ALLOW_BINDING, &dev->priv_flags); /* [한국어] 비트가 서 있지 않으면(=아직 허용 안 됨) true. 부정 논리이므로 이름과 반환값의 방향에 주의 */
}

/* [한국어] --- AER(Advanced Error Reporting), CONFIG_PCIEAER ---
 * AER 은 PCIe 링크에서 일어난 오류를 표준 확장 capability 를 통해 보고하고
 * 기록하는 기능이다. 오류는 세 등급으로 나뉜다:
 * Correctable 하드웨어가 스스로 고쳤다(재전송 등). 기록만 한다.
 * Uncorrectable Non-Fatal 해당 트랜잭션은 실패했지만 링크는 살아 있다.
 * Uncorrectable Fatal 링크 자체가 신뢰할 수 없다. 리셋이 필요하다.
 * NVMe 접점: 이 등급이 곧 drivers/pci/pcie/err.c 의 복구 흐름을 결정하고,
 * 그 흐름이 NVMe 의 .error_detected/.slot_reset/.resume 콜백
 * (drivers/nvme/host/pci.c 의 nvme_err_handler 에 등록된 것들)을 부른다. */
#ifdef CONFIG_PCIEAER
#include <linux/aer.h> /* [한국어] AER 이 켜졌을 때만 <linux/aer.h> 를 끌어온다. struct pcie_tlp_log 의 실제 정의와 aer_capability_regs 등이 여기서 온다 — 파일 맨 앞의 전방 선언이 여기서 완성된다 */

#define AER_MAX_MULTI_ERR_DEVICES	5	/* Not likely to have more */ /* [한국어] 한 번의 AER 사건에서 추적할 오류 장치의 최대 개수. 원문 주석대로 "이보다 많을 일은 거의 없다"는 경험적 상한이며, 아래 dev[]/ratelimit_print[] 배열 크기를 정한다. 넘치면 초과분은 기록되지 않는다 */

/* [한국어] 아래 구조체의 한국어 해설:
 * struct aer_err_info - AER 인터럽트 한 번에서 긁어 모은 오류 정보 묶음
 *
 * 루트 포트가 AER 인터럽트를 받으면, 오류를 낸 장치가 하나가 아닐 수 있다
 * (하나의 사건이 여러 장치에 동시에 영향을 준다). 그래서 이 구조체는
 * 장치 목록과 그 개수를 배열로 들고 다닌다.
 * 생명주기: 인터럽트 처리 함수의 스택에 잡혀 그 안에서만 산다 —
 * drivers/pci/pcie/aer.c 의 처리 루틴이 채우고 곧바로 소비한다.
 * 동기화: 스택 지역 변수이므로 공유되지 않는다. 락 불필요.
 * 비트필드를 많이 쓴 이유는 이 구조체가 자주 스택에 잡히기 때문이다. */
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
	/* [한국어] 오류를 보고한 장치들. 설정자: drivers/pci/pcie/aer.c 의 오류 수집 루틴. 읽는 자:
 * aer_print_error(aer.c)와 aer_get_device_error_info(aer.c)가 인덱스 i
 * 로 접근한다. 값 범위: 앞에서부터 error_dev_num 개만 유효. 동기화: 스택 지역 구조체라 공유되지 않음 */
	struct pci_dev *dev[AER_MAX_MULTI_ERR_DEVICES];
	/* [한국어] 장치마다 "이번 오류를 로그에 찍을 것인가"(0=안 찍음, 1=찍음). 설정자: ratelimit 판정 루틴. 읽는 자: 출력 루틴. 왜
 * 필요한가: 오류가 초당 수천 번 쏟아지는 고장 하드웨어에서 커널 로그가 폭주해 시스템이 멈추는 것을 막는다 */
	int ratelimit_print[AER_MAX_MULTI_ERR_DEVICES];
	/* [한국어] dev[] 에 실제로 담긴 장치 수. 값 범위: 0~AER_MAX_MULTI_ERR_DEVICES(5)
 * (위 한 줄이 이 필드 설명의 전부다.) */
	int error_dev_num;
	/* [한국어] 이 오류를 찍을 printk 레벨 문자열(KERN_ERR / KERN_WARNING 등). 심각도에 따라 달라진다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	const char *level;

	/* [한국어] PCI_ERR_ROOT_ERR_SRC 레지스터에서 읽은 값 — 오류를 낸 장치의 Requester ID(BDF)다. 16비트 비트필드인
 * 이유는 BDF 가 16비트이기 때문 */
	unsigned int id:16;

	/* [한국어] 오류 심각도. 값 범위(원문 주석 그대로): 0=Uncorrectable Non-fatal, 1=Uncorrectable Fatal,
 * 2=Correctable. 이 값이 복구 정책을 좌우한다 */
	unsigned int severity:2;
	/* [한국어] 루트 포트 자신의 오류를 찍을지 여부(위 장치별 플래그와 별개)
 * (위 한 줄이 이 필드 설명의 전부다.) */
	unsigned int root_ratelimit_print:1;
	/* [한국어] 정렬용 패딩. 아래 비트필드들이 워드 경계에 맞게 배치되도록 4비트를 비워 둔다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	unsigned int __pad1:4;
	/* [한국어] 여러 오류가 동시에 보고되었는지. PCI_ERR_ROOT_STATUS 의 Multiple ERR_* Received 비트에서 온다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	unsigned int multi_error_valid:1;

	/* [한국어] 가장 먼저 보고된 오류의 비트 번호(First Error Pointer). 5비트인 이유는 오류 상태 레지스터가 32비트라 비트 번호가
 * 0~31 이기 때문 */
	unsigned int first_error:5;
	/* [한국어] 정렬용 패딩 1비트
 * (위 한 줄이 이 필드 설명의 전부다.) */
	unsigned int __pad2:1;
	/* [한국어] 이 오류가 PCI 버스 오류인지 CXL 버스 오류인지(0=PCIe, 1=CXL). 아래 aer_err_bus 가 이 비트로 문자열을
 * 고른다 */
	unsigned int is_cxl:1;
	/* [한국어] tlp 필드에 유효한 TLP 헤더 로그가 담겼는지. AER 은 오류를 유발한 TLP 의 헤더를 저장해 주는데, 그 기능은 선택적이라
 * 유효성 표시가 필요하다 */
	unsigned int tlp_header_valid:1;

	/* [한국어] 오류 상태 레지스터(Correctable 또는 Uncorrectable Error Status)의 값. 어떤 오류가 났는지 비트로 알려
 * 준다 */
	unsigned int status;
	/* [한국어] 오류 마스크 레지스터의 값. 마스크된 비트는 보고되지 않으므로, status 를 해석할 때 함께 봐야 한다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	unsigned int mask;
	/* [한국어] 오류를 유발한 TLP 의 헤더 로그. tlp_header_valid 가 참일 때만 의미가 있다. 실제 정의는 <linux/aer.h> 에
 * 있다 */
	struct pcie_tlp_log tlp;
};

int aer_get_device_error_info(struct aer_err_info *info, int i); /* [한국어] 한 장치의 AER 레지스터들을 읽어 info 의 i 번째 항목을 채운다. 정의 drivers/pci/pcie/aer.c */
void aer_print_error(struct aer_err_info *info, int i); /* [한국어] 수집한 오류 정보를 커널 로그에 사람이 읽는 형태로 찍는다. 정의 drivers/pci/pcie/aer.c */

/* [한국어]
 * aer_err_bus() - 이 오류가 어느 버스 종류에서 났는지 문자열로 돌려준다
 * @info: 수집된 AER 오류 정보.
 * @return(): "CXL" 또는 "PCIe" 문자열 리터럴(해제 불필요).
 * 왜 필요한가: CXL 장치는 PCIe 위에 얹혀 있어 같은 AER 경로로 오류가
 * 올라오지만, 로그를 읽는 사람에게는 둘을 구분해 보여 주는 편이 훨씬 유용하다.
 * 위 struct aer_err_info 의 is_cxl 비트 하나가 그 구분을 담는다.
 * 실행 컨텍스트: AER 오류 출력 경로. 락 없음, 잠들지 않는다.
 * 호출 체인: drivers/pci/pcie/aer.c 의 aer_print_error() 와 pci_print_aer() → [이 함수] */
static inline const char *aer_err_bus(struct aer_err_info *info)
{
	return info->is_cxl ? "CXL" : "PCIe"; /* [한국어] is_cxl 비트 하나로 로그에 찍을 버스 이름을 고른다. 사용처: drivers/pci/pcie/aer.c 과 */
}

int pcie_read_tlp_log(struct pci_dev *dev, int where, int where2, /* [한국어] TLP 헤더 로그 레지스터들을 읽어 struct pcie_tlp_log 를 채운다. where/where2 는 읽을 레지스터 오프셋 두 곳(헤더 로그와 프리픽스 로그), flit 은 FLIT 모드 여부로 레이아웃이 달라진다. 정의 drivers/pci/pcie/tlp.c */
		      unsigned int tlp_len, bool flit,
		      struct pcie_tlp_log *log);
unsigned int aer_tlp_log_len(struct pci_dev *dev, u32 aercc); /* [한국어] AER Capabilities & Control 값(aercc)을 보고 TLP 로그가 몇 DWORD 인지 계산한다. 정의 drivers/pci/pcie/tlp.c */
void pcie_print_tlp_log(const struct pci_dev *dev, /* [한국어] TLP 로그를 사람이 읽는 형태로 찍는다. 정의 drivers/pci/pcie/tlp.c */
			const struct pcie_tlp_log *log, const char *level,
			const char *pfx);
#endif	/* CONFIG_PCIEAER */ /* [한국어] CONFIG_PCIEAER 분기 종료 — 원문 주석이 짝을 밝혀 준다 */

/* [한국어] --- RCEC(Root Complex Event Collector) 연관 정보, CONFIG_PCIEPORTBUS ---
 * RCiEP(Root Complex integrated Endpoint)는 링크 없이 Root Complex 안에
 * 통합된 장치라 자기 오류를 위로 보낼 링크가 없다. 대신 RCEC 라는 별도
 * 장치가 그 오류들을 대신 모아 보고한다.
 * 이 구조체는 "어느 RCEC 가 어느 RCiEP 들을 담당하는가"를 config space 에서
 * 한 번 읽어 캐시해 둔 것이다(RCEC Endpoint Association 확장 capability). */
#ifdef CONFIG_PCIEPORTBUS
/* Cached RCEC Endpoint Association */
struct rcec_ea {
	/* [한국어] 이 RCEC 가 담당하는 버스 번호 범위의 시작. 설정자: drivers/pci/pcie/rcec.c 의 초기화(RCEC Endpoint
 * Association capability 를 읽음). 읽는 자: rcec.c 의 순회 */
	u8		nextbusn;
	/* [한국어] 담당 버스 번호 범위의 끝
 * (위 한 줄이 이 필드 설명의 전부다.) */
	u8		lastbusn;
	/* [한국어] RCEC 와 같은 버스에 있는 RCiEP 들을 device 번호 비트맵으로 표시한 것.
	 * 설정자: drivers/pci/pcie/rcec.c 의 pci_rcec_init().
	 * 읽는 자: 같은 파일의 rcec_assoc_rciep() 가 이 비트맵을 순회해 대상 RCiEP 를 찾는다.
	 * 값 범위: 32비트 = 한 버스의 device 번호 0~31.
	 * 동기화: 초기화 후 읽기 전용. */
	u32		bitmap;
};
#endif /* [한국어] CONFIG_PCIEPORTBUS 분기 종료 */

/* [한국어] --- DPC(Downstream Port Containment), CONFIG_PCIE_DPC ---
 * DPC 는 하위 포트에서 치명적 오류가 감지되면 하드웨어가 즉시 링크를
 * 끊어 버리는 기능이다. 오염된 데이터가 시스템 메모리로 퍼지기 전에
 * 물리적으로 차단하는 것이 목적이며, AER 보다 훨씬 빠르다(소프트웨어가
 * 개입하기 전에 하드웨어가 먼저 끊는다).
 * 끊긴 뒤 소프트웨어는 원인을 읽고(dpc_process_error()), 링크를 되살리고
 * (dpc_reset_link()), 드라이버들에게 복구를 알린다.
 * 꺼졌을 때의 스텁 중 pci_dpc_recovered() 만 값을 돌려주며, false 는
 * "DPC 복구를 기다릴 일이 없다"는 뜻이라 호출자가 곧바로 진행한다.
 * NVMe 접점: DPC 로 링크가 끊기면 NVMe 컨트롤러는 config 접근조차 불가가
 * 되고, 그 상태가 pci_channel_io_frozen 또는 perm_failure 로 나타나
 * drivers/nvme/host/pci.c 의 nvme_timeout() 이 하는 pci_channel_offline() 판정에 걸린다. */
#ifdef CONFIG_PCIE_DPC
void pci_save_dpc_state(struct pci_dev *dev); /* [한국어] DPC 설정을 suspend 전에 저장한다. 정의 drivers/pci/pcie/dpc.c */
void pci_restore_dpc_state(struct pci_dev *dev); /* [한국어] resume 후 되돌린다. 정의 drivers/pci/pcie/dpc.c */
void pci_dpc_init(struct pci_dev *pdev); /* [한국어] DPC capability 를 찾아 초기화한다. 정의 drivers/pci/pcie/dpc.c */
void dpc_process_error(struct pci_dev *pdev); /* [한국어] DPC 가 걸린 원인(오류 소스와 상태)을 읽어 로그에 남긴다. 정의 drivers/pci/pcie/dpc.c */
pci_ers_result_t dpc_reset_link(struct pci_dev *pdev); /* [한국어] DPC 로 끊긴 링크를 되살린다. 반환형 pci_ers_result_t 로 복구 결과를 알리며, 이 값이 pcie_do_recovery() 의 다음 단계를 결정한다. 정의 drivers/pci/pcie/dpc.c */
bool pci_dpc_recovered(struct pci_dev *pdev); /* [한국어] DPC 복구가 끝났는지 확인한다(PCI_DPC_RECOVERED 비트를 본다). 정의 drivers/pci/pcie/dpc.c */
unsigned int dpc_tlp_log_len(struct pci_dev *dev); /* [한국어] DPC 가 저장한 TLP 로그의 길이를 계산한다. 정의 drivers/pci/pcie/tlp.c */
#else /* [한국어] CONFIG_PCIE_DPC 가 꺼진 경우 */
static inline void pci_save_dpc_state(struct pci_dev *dev) { } /* [한국어] 스텁: 저장할 DPC 상태가 없다 */
static inline void pci_restore_dpc_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 것이 없다 */
static inline void pci_dpc_init(struct pci_dev *pdev) { } /* [한국어] 스텁: 초기화할 것이 없다 */
static inline bool pci_dpc_recovered(struct pci_dev *pdev) { return false; } /* [한국어] 스텁: DPC 복구를 기다릴 일이 없으므로 항상 false */
#endif /* [한국어] CONFIG_PCIE_DPC 분기 종료 */

/* [한국어] --- RCEC 관련 API (CONFIG_PCIEPORTBUS) ---
 * 위에서 선언한 struct rcec_ea 를 채우고 활용하는 함수들이다.
 * pcie_walk_rcec() 은 pci_walk_bus() 의 RCEC 판으로, "이 RCEC 가 담당하는
 * RCiEP 전부"에 콜백을 적용한다 — AER 복구가 하위 트리를 훑을 때 링크가
 * 없는 RCiEP 들도 빠뜨리지 않기 위해 필요하다.
 * 꺼졌을 때의 스텁은 모두 무동작이며, pcie_walk_rcec() 스텁은 콜백을 아예
 * 부르지 않는다(담당 RCiEP 가 없다는 뜻과 같다). */
#ifdef CONFIG_PCIEPORTBUS
void pci_rcec_init(struct pci_dev *dev); /* [한국어] RCEC 장치를 발견했을 때 rcec_ea 캐시를 만든다. 정의 drivers/pci/pcie/rcec.c */
void pci_rcec_exit(struct pci_dev *dev); /* [한국어] 그 캐시를 해제한다. 정의 drivers/pci/pcie/rcec.c */
void pcie_link_rcec(struct pci_dev *rcec); /* [한국어] 이 RCEC 와 담당 RCiEP 들을 서로 연결한다. 정의 drivers/pci/pcie/rcec.c */
void pcie_walk_rcec(struct pci_dev *rcec, /* [한국어] 이 RCEC 가 담당하는 RCiEP 전부에 콜백을 적용한다. 정의 drivers/pci/pcie/rcec.c */
		    int (*cb)(struct pci_dev *, void *),
		    void *userdata);
#else /* [한국어] CONFIG_PCIEPORTBUS 가 꺼진 경우 */
static inline void pci_rcec_init(struct pci_dev *dev) { } /* [한국어] 스텁: 캐시할 것이 없다 */
static inline void pci_rcec_exit(struct pci_dev *dev) { } /* [한국어] 스텁: 해제할 것이 없다 */
static inline void pcie_link_rcec(struct pci_dev *rcec) { } /* [한국어] 스텁: 연결할 것이 없다 */
static inline void pcie_walk_rcec(struct pci_dev *rcec, /* [한국어] 스텁 시작 — 콜백을 한 번도 부르지 않는다(담당 RCiEP 가 없음과 같다) */
				  int (*cb)(struct pci_dev *, void *),
				  void *userdata) { }
#endif /* [한국어] CONFIG_PCIEPORTBUS 분기 종료 */

/* [한국어] --- ATS(Address Translation Service), CONFIG_PCI_ATS ---
 * ATS 는 장치가 IOMMU 에 "이 IOVA 의 실제 물리 주소가 뭐냐"고 미리 물어
 * 그 결과를 자기 안(ATC, Address Translation Cache)에 캐시해 두는 기능이다.
 * 그러면 매 DMA 마다 IOMMU 를 거치지 않아 지연이 줄어든다.
 * 저장/복원 함수가 따로 있는 이유는 D3cold 를 거치면 ATS Enable 비트가
 * 날아가기 때문이다. */
#ifdef CONFIG_PCI_ATS
/* Address Translation Service */
void pci_ats_init(struct pci_dev *dev); /* [한국어] ATS capability 를 찾아 dev->ats_cap 등을 초기화한다. 정의 drivers/pci/ats.c */
void pci_restore_ats_state(struct pci_dev *dev); /* [한국어] resume 후 ATS Enable 상태와 STU(Smallest Translation Unit)를 되돌린다. 정의 drivers/pci/ats.c */
#else /* [한국어] CONFIG_PCI_ATS 가 꺼진 경우 */
static inline void pci_ats_init(struct pci_dev *d) { } /* [한국어] 스텁: ATS 를 초기화하지 않는다. 인자 이름이 d 로 짧은 것은 원본 그대로다 */
static inline void pci_restore_ats_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 상태가 없다 */
#endif /* CONFIG_PCI_ATS */ /* [한국어] CONFIG_PCI_ATS 분기 종료 */

/* [한국어] --- PRI(Page Request Interface), CONFIG_PCI_PRI ---
 * PRI 는 장치가 "지금 접근하려는 페이지가 스왑아웃되어 있으니 올려 달라"고
 * 호스트에 요청할 수 있게 하는 기능이다. 이것이 있어야 장치 DMA 대상 메모리를
 * 미리 전부 핀(pin) 해 두지 않아도 되며, SVA(Shared Virtual Addressing)의
 * 전제 조건이다. ATS 와 짝을 이룬다. */
#ifdef CONFIG_PCI_PRI
void pci_pri_init(struct pci_dev *dev); /* [한국어] PRI capability 를 찾아 초기화한다. 정의 drivers/pci/ats.c */
void pci_restore_pri_state(struct pci_dev *pdev); /* [한국어] resume 후 PRI Enable 과 미해결 페이지 요청 한도를 되돌린다. 정의 drivers/pci/ats.c */
#else /* [한국어] CONFIG_PCI_PRI 가 꺼진 경우 */
static inline void pci_pri_init(struct pci_dev *dev) { } /* [한국어] 스텁: PRI 없음 */
static inline void pci_restore_pri_state(struct pci_dev *pdev) { } /* [한국어] 스텁: 복원할 것 없음 */
#endif /* [한국어] CONFIG_PCI_PRI 분기 종료 */

/* [한국어] --- PASID(Process Address Space ID), CONFIG_PCI_PASID ---
 * PASID 는 TLP 에 20비트 주소 공간 식별자를 붙여, 하나의 장치가 여러 프로세스의
 * 가상 주소 공간에 동시에 DMA 할 수 있게 한다. ATS/PRI 와 함께 SVA 를 이룬다. */
#ifdef CONFIG_PCI_PASID
void pci_pasid_init(struct pci_dev *dev); /* [한국어] PASID capability 를 찾아 초기화한다. 정의 drivers/pci/ats.c */
void pci_restore_pasid_state(struct pci_dev *pdev); /* [한국어] resume 후 PASID Enable 과 권한 설정을 되돌린다. 정의 drivers/pci/ats.c */
#else /* [한국어] CONFIG_PCI_PASID 가 꺼진 경우 */
static inline void pci_pasid_init(struct pci_dev *dev) { } /* [한국어] 스텁: PASID 없음 */
static inline void pci_restore_pasid_state(struct pci_dev *pdev) { } /* [한국어] 스텁: 복원할 것 없음 */
#endif /* [한국어] CONFIG_PCI_PASID 분기 종료 */

/* [한국어] --- SR-IOV API (CONFIG_PCI_IOV) ---
 * 위 struct pci_sriov 를 다루는 함수들이다. 이 블록의 #else 스텁들이
 * 특히 교훈적이다:
 * - pci_iov_init() 스텁은 -ENODEV 를 준다 → "SR-IOV 없음"으로 조용히 실패.
 * - pci_resource_is_iov 스텁은 항상 false → VF BAR 구획 자체가 없다는 뜻.
 * - pci_resource_num_from_vf_bar()/_to_vf_bar 스텁은 WARN_ON_ONCE(1) 로
 * 경고까지 찍는다. 앞의 두 스텁이 제대로 걸렀다면 이 두 함수는 애초에
 * 불릴 수 없기 때문이다 — 즉 "여기 도달했다면 호출부에 논리 오류가 있다"는
 * 표시이며, 스텁이 단순히 조용히 넘어가는 것만이 능사가 아님을 보여 준다.
 * NVMe 접점: NVMe 는 .sriov_configure 로 pci_sriov_configure_simple() 만
 * 등록하므로(drivers/nvme/host/pci.c), 이 블록의 함수들을 직접 부르지
 * 않는다. CONFIG_PCI_IOV 가 꺼지면 NVMe 의 sriov_numvfs sysfs 자체가 생기지
 * 않는다. */
#ifdef CONFIG_PCI_IOV
int pci_iov_init(struct pci_dev *dev); /* [한국어] SR-IOV capability 를 찾아 struct pci_sriov 를 만든다. 정의 drivers/pci/iov.c */
void pci_iov_release(struct pci_dev *dev); /* [한국어] PF 의 SR-IOV 자원을 해제한다. 정의 drivers/pci/iov.c */
void pci_iov_remove(struct pci_dev *dev); /* [한국어] PF 드라이버가 떠난 뒤 상태를 정리한다(VF 가 남아 있으면 경고). 정의 drivers/pci/iov.c */
void pci_iov_update_resource(struct pci_dev *dev, int resno); /* [한국어] PF 의 VF BAR 레지스터를 현재 resource 값에 맞춰 다시 쓴다. 정의 drivers/pci/iov.c */
resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev, int resno); /* [한국어] VF BAR 구획의 정렬 요구를 계산한다. 아래 pci_resource_alignment 의 첫 갈래가 이 함수다. 정의 drivers/pci/iov.c */
void pci_restore_iov_state(struct pci_dev *dev); /* [한국어] resume 후 SR-IOV 레지스터 전체를 되돌린다. 정의 drivers/pci/iov.c */
int pci_iov_bus_range(struct pci_bus *bus); /* [한국어] VF 들이 필요로 하는 버스 번호 개수를 돌려준다(max_VF_buses 기반). 정의 drivers/pci/iov.c */
void pci_iov_resource_set_size(struct pci_dev *dev, int resno, int size); /* [한국어] VF Resizable BAR 로 VF BAR 크기를 바꾼 뒤 barsz[] 를 갱신한다. 정의 drivers/pci/iov.c */
bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev); /* [한국어] SR-IOV Control 의 VF Memory Space Enable(MSE) 비트가 서 있는지 확인한다. 정의 drivers/pci/iov.c — 켜져 있으면 VF BAR 를 옮길 수 없다 */
/* [한국어]
 * pci_iov_vf_rebar_cap() - 이 PF 의 VF Resizable BAR capability 오프셋을 얻는다
 *
 * @dev: 질의 대상 장치.
 * @return(): PF 이고 capability 가 있으면 그 오프셋, 아니면 0.
 *
 * PF 가 아니면 dev->sriov 가 NULL 이라 곧바로 역참조하면 커널이 죽는다.
 * is_physfn 검사가 그 방어다.
 * 호출 체인: drivers/pci/iov.c 의 sriov_restore_vf_rebar_state() 와
 * drivers/pci/rebar.c 의 pci_rebar_find_pos() → [이 함수] */
static inline u16 pci_iov_vf_rebar_cap(struct pci_dev *dev)
{
	if (!dev->is_physfn) /* [한국어] PF 가 아니면 dev->sriov 가 NULL 이므로 역참조 전에 반드시 걸러야 한다 */
		return 0; /* [한국어] PF 가 아니면 "capability 없음"을 뜻하는 0 */

	return dev->sriov->vf_rebar_cap; /* [한국어] PF 이므로 sriov 가 유효하다 — 캐시해 둔 오프셋을 그대로 돌려준다 */
}
/* [한국어]
 * pci_resource_is_iov() - 이 resource[] 인덱스가 VF BAR 구획인지 판정
 *
 * @resno: 검사할 인덱스.
 * @return(): VF BAR 구획이면 true.
 *
 * VF BAR 는 PF 의 resource[] 안에 PCI_IOV_RESOURCES ~ PCI_IOV_RESOURCE_END
 * 구간으로 따로 자리를 잡는다(총 PCI_SRIOV_NUM_BARS = 6칸). 이 구간은
 * "VF 하나의 BAR" 가 아니라 "모든 VF 의 BAR 를 합친 전체 영역"을 나타내며,
 * 그래서 정렬 요구가 표준 BAR 와 달라 아래 pci_resource_alignment 가
 * 갈래를 나눈다.
 * 호출 체인: drivers/pci/iov.c 의 pci_iov_resource_set_size(),
 * drivers/pci/rebar.c 의 pci_rebar_find_pos()/pci_rebar_set_size()/
 * pci_resize_resource_set_size(),
 * 그리고 아래 pci_resource_alignment() → [이 함수] */
static inline bool pci_resource_is_iov(int resno)
{
	return resno >= PCI_IOV_RESOURCES && resno <= PCI_IOV_RESOURCE_END; /* [한국어] VF BAR 구획의 시작과 끝 사이인지. 두 상수는 <linux/pci.h> 의 리소스 enum 에서 오며, CONFIG_PCI_IOV 가 켜졌을 때만 정의된다 */
}
/* [한국어]
 * pci_resource_num_from_vf_bar() - VF BAR 번호(0~5)를 resource[] 인덱스로 변환
 *
 * @resno: VF BAR 번호(0~5).
 * @return(): 대응하는 resource[] 인덱스.
 *
 * 호출 체인: drivers/pci/iov.c (VF BAR 순회) → [이 함수] */
static inline int pci_resource_num_from_vf_bar(int resno)
{
	return resno + PCI_IOV_RESOURCES; /* [한국어] VF BAR 번호에 구획 시작 인덱스를 더한다 */
}
/* [한국어]
 * pci_resource_num_to_vf_bar() - resource[] 인덱스를 VF BAR 번호(0~5)로 되돌린다
 *
 * @resno: resource[] 인덱스(반드시 VF BAR 구획이어야 한다).
 * @return(): VF BAR 번호.
 *
 * 위 함수의 역변환이다. 호출 전에 pci_resource_is_iov() 로 구획을 확인하는
 * 것이 호출자의 책임이며, 실제로 drivers/pci/iov.c 등이 그렇게 한다.
 * 호출 체인: drivers/pci/iov.c 의 pci_iov_resource_size()(barsz[] 조회),
 * pci_iov_resource_set_size(), pci_iov_update_resource() → [이 함수] */
static inline int pci_resource_num_to_vf_bar(int resno)
{
	return resno - PCI_IOV_RESOURCES; /* [한국어] 구획 시작 인덱스를 빼서 0~5 로 되돌린다 */
}
extern const struct attribute_group sriov_pf_dev_attr_group; /* [한국어] PF 쪽 SR-IOV sysfs 속성 그룹(sriov_numvfs, sriov_totalvfs 등). 실체 drivers/pci/iov.c */
extern const struct attribute_group sriov_vf_dev_attr_group; /* [한국어] VF 쪽 SR-IOV sysfs 속성 그룹(sriov_vf_msix_count 등). 실체 drivers/pci/iov.c */
#else /* [한국어] CONFIG_PCI_IOV 가 꺼진 경우 — 아래 스텁들이 대신한다 */
static inline int pci_iov_init(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] SR-IOV 를 지원하지 않으므로 "장치 없음"으로 실패. 호출자(probe.c)는 이 실패를 정상으로 취급한다 */
}
/* [한국어]
 * pci_iov_release() 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @dev: 제거되는 장치. @return(): 없음.
 * SR-IOV 가 없으면 dev->sriov 도 만들어지지 않으므로 해제할 것이 없다.
 * 장치 제거 경로가 #ifdef 없이 이 함수를 부를 수 있게 하는 것이 존재 이유다.
 * 호출 체인: drivers/pci/probe.c 의 장치 해제 → [이 스텁] */
static inline void pci_iov_release(struct pci_dev *dev) { } /* [한국어] 스텁: 해제할 SR-IOV 자원이 없다 */
static inline void pci_iov_remove(struct pci_dev *dev) { } /* [한국어] 스텁: 정리할 것이 없다 */
static inline void pci_iov_update_resource(struct pci_dev *dev, int resno) { } /* [한국어] 스텁: 갱신할 VF BAR 가 없다 */
static inline resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev, /* [한국어] 스텁 시작 */
							   int resno)
{
	return 0; /* [한국어] VF BAR 구획 자체가 없으므로 정렬 요구도 0 */
}
/* [한국어]
 * pci_restore_iov_state() 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @dev: resume 중인 장치. @return(): 없음.
 * 복원할 SR-IOV 레지스터가 없다. pci_restore_state() 는 이 함수를 조건 없이
 * 부르므로 스텁이 반드시 있어야 한다.
 * 호출 체인: pci_restore_state(drivers/pci/pci.c) → [이 스텁] */
static inline void pci_restore_iov_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 SR-IOV 상태가 없다 */
static inline int pci_iov_bus_range(struct pci_bus *bus) /* [한국어] 스텁 시작 */
{
	return 0; /* [한국어] VF 가 없으므로 버스 번호를 하나도 소비하지 않는다 */
}
/* [한국어]
 * pci_iov_resource_set_size() 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @dev: 대상 장치. @resno: resource[] 인덱스. @size: 새 크기 코드. @return(): 없음.
 * VF BAR 구획이 존재하지 않으므로 크기를 바꿀 대상도 없다.
 * 호출 체인: drivers/pci/rebar.c 의 크기 변경 경로 → [이 스텁] */
static inline void pci_iov_resource_set_size(struct pci_dev *dev, int resno, /* [한국어] 스텁: 크기를 바꿀 VF BAR 가 없다 */
					     int size) { }
static inline bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] VF 메모리 공간 자체가 없으므로 항상 거짓 */
}
/* [한국어]
 * pci_iov_vf_rebar_cap 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @dev: 질의 대상 장치. @return(): 항상 0("capability 없음").
 * SR-IOV 자체가 없으니 VF Resizable BAR capability 도 있을 수 없다.
 * 0 은 유효한 확장 capability 오프셋이 될 수 없어(확장 영역은 0x100 부터)
 * "없음" 표시로 안전하다.
 * 호출 체인: drivers/pci/rebar.c → [이 스텁] */
static inline u16 pci_iov_vf_rebar_cap(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return 0; /* [한국어] VF Resizable BAR capability 도 없다 */
}
/* [한국어]
 * pci_resource_is_iov 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @resno: 검사할 resource[] 인덱스. @return(): 항상 false.
 * CONFIG_PCI_IOV 가 꺼지면 <linux/pci.h> 의 리소스 enum 에서 PCI_IOV_RESOURCES
 * 구획 자체가 사라진다. 따라서 어떤 인덱스도 VF BAR 일 수 없고, 이 스텁이
 * 아래 두 변환 함수를 사실상 도달 불가로 만든다.
 * 호출 체인: pci_resource_alignment(), drivers/pci/rebar.c → [이 스텁] */
static inline bool pci_resource_is_iov(int resno) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] VF BAR 구획이 없으므로 어떤 인덱스도 VF BAR 가 아니다 */
}
/* [한국어]
 * pci_resource_num_from_vf_bar 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @resno: VF BAR 번호. @return(): -ENODEV (그리고 한 번만 커널 경고).
 * 위 pci_resource_is_iov 스텁이 항상 false 이므로 정상 흐름에서는 이 함수에
 * 도달할 수 없다. 도달했다면 호출부가 SR-IOV 유무를 확인하지 않았다는 뜻이라
 * WARN_ON_ONCE 로 알린다 — 스텁이 조용히 넘어가기만 하면 그런 버그가 묻힌다.
 * 호출 체인: drivers/pci/iov.c 의 VF BAR 순회(정상적으로는 도달 불가) */
static inline int pci_resource_num_from_vf_bar(int resno) /* [한국어] 스텁 시작 — 아래 경고가 이 스텁의 핵심이다 */
{
	WARN_ON_ONCE(1); /* [한국어] 여기에 도달했다는 것 자체가 호출부의 논리 오류다. 위 pci_resource_is_iov 스텁이 항상 false 를 주므로 정상 흐름이라면 이 함수는 불릴 수 없다. ONCE 인 이유는 같은 버그로 로그가 폭주하지 않게 하려는 것 */
	return -ENODEV; /* [한국어] 유효하지 않은 인덱스 대신 음수 errno 를 돌려준다 */
}
/* [한국어]
 * pci_resource_num_to_vf_bar 스텁 (CONFIG_PCI_IOV 꺼짐)
 * @resno: resource[] 인덱스. @return(): -ENODEV (그리고 한 번만 커널 경고).
 * 위 함수와 같은 이유로 도달 자체가 버그다. */
static inline int pci_resource_num_to_vf_bar(int resno) /* [한국어] 스텁 시작 — 위와 같은 이유로 경고를 찍는다 */
{
	WARN_ON_ONCE(1); /* [한국어] 도달 자체가 버그임을 한 번만 알린다 */
	return -ENODEV; /* [한국어] 음수 errno 반환 */
}
#endif /* CONFIG_PCI_IOV */ /* [한국어] CONFIG_PCI_IOV 분기 종료 */

/* [한국어] --- TPH(TLP Processing Hints), CONFIG_PCIE_TPH ---
 * TPH 는 장치가 TLP 에 "이 데이터를 어느 캐시/메모리 계층에 두면 좋은지"
 * 힌트를 붙여 보내는 기능이다. 그러면 DMA 로 들어온 데이터가 곧 쓰일 CPU 의
 * 캐시(LLC)에 미리 자리 잡아, 소비하는 쪽의 캐시 미스가 줄어든다.
 * "pci=notph" 로 끌 수 있고(pci_no_tph()), suspend 를 넘기려면 저장/복원이
 * 필요하다. */
#ifdef CONFIG_PCIE_TPH
void pci_restore_tph_state(struct pci_dev *dev); /* [한국어] resume 후 TPH 설정(Steering Tag 표 포함)을 되돌린다. 정의 drivers/pci/tph.c */
void pci_save_tph_state(struct pci_dev *dev); /* [한국어] suspend 전에 TPH 설정을 저장한다. 정의 drivers/pci/tph.c */
void pci_no_tph(void); /* [한국어] TPH 를 전역으로 끈다("pci=notph"). 정의 drivers/pci/tph.c */
void pci_tph_init(struct pci_dev *dev); /* [한국어] TPH capability 를 찾아 초기화한다. 정의 drivers/pci/tph.c */
#else /* [한국어] CONFIG_PCIE_TPH 가 꺼진 경우 */
static inline void pci_restore_tph_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 것 없음 */
static inline void pci_save_tph_state(struct pci_dev *dev) { } /* [한국어] 스텁: 저장할 것 없음 */
static inline void pci_no_tph(void) { } /* [한국어] 스텁: 끌 대상 없음 */
static inline void pci_tph_init(struct pci_dev *dev) { } /* [한국어] 스텁: 초기화할 것 없음 */
#endif /* [한국어] CONFIG_PCIE_TPH 분기 종료 */

/* [한국어] --- PTM(Precision Time Measurement), CONFIG_PCIE_PTM ---
 * PTM 은 루트 컴플렉스와 장치가 나노초 단위로 시계를 맞추는 기능이다.
 * 여기에는 suspend/resume 뿐 아니라 별도의 suspend/resume 훅이 더 있는데,
 * PTM 은 링크가 저전력 상태로 내려가기 "전에" 꺼야 하고 링크가 올라온 "뒤에"
 * 다시 켜야 하는 순서 제약이 있기 때문이다(save/restore 만으로는 부족하다). */
#ifdef CONFIG_PCIE_PTM
void pci_ptm_init(struct pci_dev *dev); /* [한국어] PTM capability 를 찾아 초기화한다. 정의 drivers/pci/pcie/ptm.c */
void pci_save_ptm_state(struct pci_dev *dev); /* [한국어] suspend 전에 PTM 설정을 저장한다. 정의 drivers/pci/pcie/ptm.c */
void pci_restore_ptm_state(struct pci_dev *dev); /* [한국어] resume 후 되돌린다. 정의 drivers/pci/pcie/ptm.c */
void pci_suspend_ptm(struct pci_dev *dev); /* [한국어] 링크를 저전력으로 내리기 전에 PTM 을 끈다. 정의 drivers/pci/pcie/ptm.c */
void pci_resume_ptm(struct pci_dev *dev); /* [한국어] 링크가 올라온 뒤 PTM 을 다시 켠다. 정의 drivers/pci/pcie/ptm.c */
#else /* [한국어] CONFIG_PCIE_PTM 이 꺼진 경우 */
static inline void pci_ptm_init(struct pci_dev *dev) { } /* [한국어] 스텁: 초기화할 것 없음 */
static inline void pci_save_ptm_state(struct pci_dev *dev) { } /* [한국어] 스텁: 저장할 것 없음 */
static inline void pci_restore_ptm_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 것 없음 */
static inline void pci_suspend_ptm(struct pci_dev *dev) { } /* [한국어] 스텁: 끌 것 없음 */
static inline void pci_resume_ptm(struct pci_dev *dev) { } /* [한국어] 스텁: 켤 것 없음 */
#endif /* [한국어] CONFIG_PCIE_PTM 분기 종료 */

/* [한국어]
 * pci_resource_alignment() - 이 리소스가 요구하는 주소 정렬을 계산한다
 *
 * @dev: 리소스를 소유한 장치.
 * @res: 대상 리소스.
 * @return(): 이 리소스를 배치할 때 지켜야 할 정렬(바이트).
 *
 * 왜 갈래가 셋인가: 리소스 종류마다 정렬 규칙이 다르기 때문이다.
 * 1) VF BAR 구획 → pci_sriov_resource_alignment(). VF BAR 는 "VF 하나의 크기"
 * 가 아니라 System Page Size 와 VF 개수에 얽힌 별도 규칙을 따른다.
 * 2) CardBus 브리지 → pci_cardbus_resource_alignment(). CardBus 창은
 * 고유한 정렬 요구가 있다.
 * 3) 그 외 일반 BAR → resource_alignment(). 보통은 IORESOURCE_SIZEALIGN
 * 플래그에 따라 "크기와 같은 정렬"이 된다 — PCI BAR 는 자기 크기의
 * 배수 주소에만 놓일 수 있다는 스펙 규칙이 그렇게 표현된 것이다.
 * 실행 컨텍스트: 리소스 배정 계산 중(프로세스 컨텍스트). 락 없음.
 * 호출 체인:
 * drivers/pci/setup-bus.c (브리지 윈도우 크기·정렬 산정)
 * → [이 함수] → pci_resource_num() / pci_resource_is_iov() / ...
 * NVMe 접점: NVMe 의 BAR0 가 "자기 크기의 배수 주소"에만 놓이는 이유가
 * 3) 갈래다. 그래서 큰 BAR 를 요구하는 장치일수록 상위 브리지 창도
 * 그만큼 크고 잘 정렬되어야 하며, 자리가 없으면 BAR 배정이 실패한다. */
static inline resource_size_t pci_resource_alignment(struct pci_dev *dev,
						     struct resource *res)
{
	int resno = pci_resource_num(dev, res); /* [한국어] 먼저 resource 포인터에서 인덱스를 역산한다(위 pci_resource_num()) */

	if (pci_resource_is_iov(resno)) /* [한국어] VF BAR 구획이면 */
		return pci_sriov_resource_alignment(dev, resno); /* [한국어] SR-IOV 전용 정렬 규칙을 쓴다 */
	if (dev->class >> 8 == PCI_CLASS_BRIDGE_CARDBUS) /* [한국어] class 를 8비트 오른쪽으로 민 값이 base+sub class 16비트. PCI_CLASS_BRIDGE_CARDBUS 와 비교해 CardBus 브리지인지 본다(prog-if 바이트는 버린다) */
		return pci_cardbus_resource_alignment(res); /* [한국어] CardBus 전용 정렬 규칙 */
	return resource_alignment(res); /* [한국어] 일반 리소스 — resource 코어의 기본 정렬 계산(IORESOURCE_SIZEALIGN 이면 크기와 같은 정렬) */
}

resource_size_t pci_min_window_alignment(struct pci_bus *bus, /* [한국어] 브리지 윈도우가 지켜야 하는 최소 정렬을 타입(IORESOURCE_IO/MEM/PREFETCH)별로 돌려준다. 정의 drivers/pci/setup-bus.c. I/O 창은 4KB, 메모리 창은 1MB 경계라는 브리지 스펙 규칙이 여기 반영된다 */
					 unsigned long type);

/* [한국어] --- ACS(Access Control Services) ---
 * ACS 는 "같은 스위치 아래 두 장치가 서로 직접(peer-to-peer) 통신하는 것을
 * 막고, 모든 트래픽을 루트 컴플렉스(=IOMMU)를 거치게 강제"하는 기능이다.
 * 왜 중요한가: ACS 없이 두 장치가 P2P 로 통신하면 IOMMU 를 우회하게 되어,
 * 한 장치를 게스트 VM 에 넘겼을 때 그 게스트가 옆 장치의 메모리를 읽고 쓸 수
 * 있다. 그래서 IOMMU 그룹은 ACS 로 격리가 보장되는 경계에서만 쪼개진다 —
 * VFIO 패스스루에서 "장치가 같은 IOMMU 그룹에 묶여 따로 넘길 수 없다"는
 * 흔한 문제의 근원이 여기다.
 * NVMe 접점: 반대로 NVMe 의 P2PDMA(drivers/nvme/host/pci.c 의
 * pci_p2pdma_add_resource() 사용)는 GPU 등과의 직접 전송이 목적이라 ACS 격리와
 * 상충한다 — 그래서 커널에 ACS redirect 를 꺼 주는 옵션이 따로 있다. */
void pci_acs_init(struct pci_dev *dev); /* [한국어] ACS capability 를 찾아 dev->acs_cap 과 acs_capabilities 를 채운다. 정의 drivers/pci/pci.c */
void pci_enable_acs(struct pci_dev *dev); /* [한국어] ACS 의 격리 관련 비트들을 실제로 켠다. 정의 drivers/pci/pci.c */
/* [한국어] --- ACS 관련 quirk (CONFIG_PCI_QUIRKS) ---
 * quirk 는 "스펙을 어긴 하드웨어를 위한 우회 코드"다. ACS 쪽 quirk 가 특히
 * 많은 이유는, 실제로는 격리를 보장하면서도 ACS capability 를 구현하지 않은
 * 칩셋(대표적으로 여러 인텔 PCH 의 루트 포트)이 많기 때문이다. 그런 장치를
 * quirk 로 "격리 가능"이라고 알려 주지 않으면 IOMMU 그룹이 쓸데없이 크게
 * 묶여 버린다.
 * 꺼졌을 때 스텁이 모두 -ENOTTY 를 돌려주는 것에 주목: -ENOTTY 는 여기서
 * "이 장치에 대한 특별 처리가 없다"는 뜻으로 쓰이며, 호출자는 이를 오류가
 * 아니라 "일반 경로로 진행하라"는 신호로 해석한다. */
#ifdef CONFIG_PCI_QUIRKS
int pci_dev_specific_acs_enabled(struct pci_dev *dev, u16 acs_flags); /* [한국어] 이 장치가 (ACS capability 없이도) 요구한 격리를 제공하는지 quirk 표로 판정한다. 정의 drivers/pci/quirks.c. -ENOTTY 면 "해당 quirk 없음" */
int pci_dev_specific_enable_acs(struct pci_dev *dev); /* [한국어] 장치 전용 방법으로 ACS 를 켠다. 정의 drivers/pci/quirks.c */
int pci_dev_specific_disable_acs_redir(struct pci_dev *dev); /* [한국어] ACS 의 P2P Request Redirect 만 선택적으로 끈다. 정의 drivers/pci/quirks.c — P2PDMA 성능을 위해 격리를 일부 포기하는 경로 */
void pci_disable_broken_acs_cap(struct pci_dev *pdev); /* [한국어] ACS capability 를 광고하지만 실제로는 동작하지 않는 하드웨어에서 그 capability 를 무시하게 만든다. 정의 drivers/pci/quirks.c */
int pcie_failed_link_retrain(struct pci_dev *dev); /* [한국어] 링크 훈련이 두 속도 사이를 무한히 오가며 절대 활성 상태에 이르지 못하는 하드웨어를 손으로 재훈련시킨다. 정의 drivers/pci/quirks.c — quirks.c 원문 주석이 ASMedia ASM2824(Gen3)와 Pericom PI7C9X2G304(Gen2) 스위치를 직결했을 때 관측된 문제라고 밝히고 있다 */
#else /* [한국어] CONFIG_PCI_QUIRKS 가 꺼진 경우 */
static inline int pci_dev_specific_acs_enabled(struct pci_dev *dev, /* [한국어] 스텁 시작 */
					       u16 acs_flags)
{
	return -ENOTTY; /* [한국어] -ENOTTY = "이 장치를 위한 특별 처리 없음". 호출자는 오류가 아니라 일반 경로로 해석한다 */
}
/* [한국어]
 * pci_dev_specific_enable_acs() 스텁 (CONFIG_PCI_QUIRKS 꺼짐)
 * @dev: 대상 장치. @return(): -ENOTTY("장치 전용 방법 없음").
 * quirk 가 없으면 표준 ACS capability 조작만 가능하다. 호출자는 이 값을
 * 오류가 아니라 "표준 경로로 가라"로 읽는다.
 * 호출 체인: drivers/pci/pci.c 의 pci_enable_acs() → [이 스텁] */
static inline int pci_dev_specific_enable_acs(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return -ENOTTY; /* [한국어] 켤 장치 전용 방법이 없다 */
}
/* [한국어]
 * pci_dev_specific_disable_acs_redir() 스텁 (CONFIG_PCI_QUIRKS 꺼짐)
 * @dev: 대상 장치. @return(): -ENOTTY.
 * ACS 의 P2P Request Redirect 를 장치 고유 방식으로 끄는 우회가 없다는 뜻.
 * P2PDMA 성능을 위해 격리를 일부 완화하려던 호출자는 표준 방법만 쓴다. */
static inline int pci_dev_specific_disable_acs_redir(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return -ENOTTY; /* [한국어] 끌 장치 전용 방법이 없다 */
}
/* [한국어]
 * pci_disable_broken_acs_cap() 스텁 (CONFIG_PCI_QUIRKS 꺼짐)
 * @dev: 대상 장치. @return(): 없음.
 * "ACS capability 를 광고하지만 실제로는 동작하지 않는 하드웨어" 목록이
 * quirk 표에 있는데, quirk 가 빠지면 그 목록도 없으므로 할 일이 없다.
 * 호출 체인: drivers/pci/pci.c 의 pci_acs_init() → [이 스텁] */
static inline void pci_disable_broken_acs_cap(struct pci_dev *dev) { } /* [한국어] 스텁: 무시할 브로큰 capability 목록이 없다 */
static inline int pcie_failed_link_retrain(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return -ENOTTY; /* [한국어] 손으로 재훈련할 대상이 아니다 */
}
#endif /* [한국어] CONFIG_PCI_QUIRKS 분기 종료 */

/* [한국어] --- PCIe 오류 보고와 복구의 중심 진입점 ---
 * pcie_do_recovery() 는 AER 과 DPC, EDR 이 공유하는 복구 상태 기계다.
 * 하는 일(구현: drivers/pci/pcie/err.c):
 * 1) 영향받는 하위 트리의 모든 드라이버에게 .error_detected(state) 통지
 * 2) 드라이버들의 응답을 병합(merge_result())해 다음 행동을 결정
 * 3) 필요하면 @reset_subordinates 콜백으로 링크/버스 리셋
 * 4) 리셋 후 .slot_reset 통지 → 마지막으로 .resume 통지
 * 호출자:
 *   drivers/pci/pcie/aer.c 의 pci_aer_handle_error()/aer_recover_work_func()
 *     (AER, reset_subordinates=aer_root_reset)
 *   drivers/pci/pcie/dpc.c 의 dpc_handler 경로 (DPC, reset_subordinates=dpc_reset_link)
 *   drivers/pci/pcie/edr.c (펌웨어 우선 오류 보고 EDR)
 *
 * NVMe 접점: 이 함수가 부르는 콜백이 정확히
 * drivers/nvme/host/pci.c 의 nvme_err_handler 안에 있는
 *   .error_detected = nvme_error_detected
 *   .slot_reset     = nvme_slot_reset
 * 다. 즉 "NVMe 컨트롤러가 PCIe 오류에서 살아 돌아오는" 전 과정의 지휘자가
 * 이 함수이며, NVMe 드라이버는 그 지휘에 응답만 한다. */
/* PCI error reporting and recovery */
pci_ers_result_t pcie_do_recovery(struct pci_dev *dev, /* [한국어] 오류 복구 상태 기계의 진입점. @state 는 통지할 채널 상태, @reset_subordinates 는 하위를 실제로 리셋하는 콜백이다 */
		pci_channel_state_t state,
		pci_ers_result_t (*reset_subordinates)(struct pci_dev *pdev));

bool pcie_wait_for_link(struct pci_dev *pdev, bool active); /* [한국어] 링크가 활성(active=true) 또는 비활성(false) 상태가 될 때까지 기다린다. 정의 drivers/pci/pci.c */
int pcie_retrain_link(struct pci_dev *pdev, bool use_lt); /* [한국어] 링크 재훈련을 요청하고 완료를 기다린다. use_lt 가 참이면 Link Training 비트로, 거짓이면 Data Link Layer Link Active 비트로 완료를 판정한다. 정의 drivers/pci/pci.c */

/* [한국어] --- LTR 과 ASPM L1 서브스테이트 (CONFIG_PCIEASPM 없이도 필요) ---
 * 이 다섯 함수가 #ifdef 밖에 있는 이유를 원문 주석이 밝혀 두었다:
 * ASPM 정책 자체는 CONFIG_PCIEASPM 으로 끌 수 있지만, 펌웨어가 이미 켜 둔
 * L1 서브스테이트와 LTR 설정은 suspend/resume 을 거치며 반드시 보존해야 한다.
 * 보존하지 않으면 resume 후 링크가 잘못된 저전력 설정으로 남아 장치가
 * 응답하지 않을 수 있다.
 * LTR(Latency Tolerance Reporting): 장치가 "나는 이 정도 지연까지는 견딘다"를
 * 호스트에 알리는 기능. 이 값을 보고 플랫폼이 얼마나 깊은 절전으로 들어갈지
 * 정한다.
 * L1 서브스테이트(L1.1/L1.2): L1 보다 더 깊은 절전 단계로, L1.2 는 참조
 * 클록까지 끈다. 복귀에 수십~수백 마이크로초가 걸린다.
 * NVMe 접점: NVMe SSD 의 유휴 전력은 대부분 이 링크 절전에서 나온다.
 * NVMe 의 APST 가 "컨트롤러 내부 전력 상태"를 다루는 것과 달리, 여기는
 * "링크 자체"를 재우는 것이라 계층이 다르다. 노트북에서 NVMe 지연이 튀는
 * 문제가 종종 L1.2 복귀 시간과 얽힌다. */
/* ASPM-related functionality we need even without CONFIG_PCIEASPM */
void pci_save_ltr_state(struct pci_dev *dev); /* [한국어] LTR 설정을 저장 버퍼에 뜬다. 정의 drivers/pci/pcie/aspm.c */
void pci_restore_ltr_state(struct pci_dev *dev); /* [한국어] resume 후 되돌린다. 정의 drivers/pci/pcie/aspm.c */
void pci_configure_aspm_l1ss(struct pci_dev *dev); /* [한국어] L1 서브스테이트를 장치 능력에 맞게 설정한다. 정의 drivers/pci/pcie/aspm.c */
void pci_save_aspm_l1ss_state(struct pci_dev *dev); /* [한국어] L1 서브스테이트 설정을 저장한다. 정의 drivers/pci/pcie/aspm.c */
void pci_restore_aspm_l1ss_state(struct pci_dev *dev); /* [한국어] resume 후 되돌린다. 정의 drivers/pci/pcie/aspm.c */

/* [한국어] --- ASPM(Active State Power Management) 본체, CONFIG_PCIEASPM ---
 * ASPM 은 링크가 유휴일 때 하드웨어가 자동으로 저전력 상태(L0s/L1)로
 * 내려가게 하는 기능이다. 소프트웨어는 정책만 정하고, 실제 진입/복귀는
 * 하드웨어가 알아서 한다.
 * L0s: 한 방향만 재운다. 복귀가 빠르다(수백 나노초).
 * L1 : 양방향을 재운다. 더 많이 아끼지만 복귀가 느리다(수 마이크로초).
 * 꺼졌을 때의 스텁은 모두 무동작이며, 그 결과 링크는 펌웨어가 설정해 둔
 * 상태 그대로 남는다(커널이 켜지도 끄지도 않는다). */
#ifdef CONFIG_PCIEASPM
void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap); /* [한국어] quirk 등이 특정 ASPM 능력을 "없는 것으로 치라"고 알릴 때 쓴다. 정의 drivers/pci/pcie/aspm.c */
void pcie_aspm_init_link_state(struct pci_dev *pdev); /* [한국어] 장치가 나타났을 때 링크의 ASPM 상태 객체를 만들고 정책을 적용한다. 정의 drivers/pci/pcie/aspm.c */
void pcie_aspm_exit_link_state(struct pci_dev *pdev); /* [한국어] 장치가 사라질 때 그 상태를 정리한다. 정의 drivers/pci/pcie/aspm.c */
void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked); /* [한국어] 전원 상태가 바뀔 때 ASPM 설정을 조정한다. locked 는 호출자가 이미 관련 뮤텍스를 쥐었는지 알려 주는 플래그로, 중복 잠금을 피하기 위한 것. 정의 drivers/pci/pcie/aspm.c */
void pcie_aspm_powersave_config_link(struct pci_dev *pdev); /* [한국어] 절전 정책에 맞춰 링크 ASPM 을 다시 설정한다. 정의 drivers/pci/pcie/aspm.c */
void pci_configure_ltr(struct pci_dev *pdev); /* [한국어] 장치의 LTR 능력을 보고 LTR 을 켤지 정한다. 정의 drivers/pci/pcie/aspm.c */
void pci_bridge_reconfigure_ltr(struct pci_dev *pdev); /* [한국어] 브리지의 LTR 설정을 다시 계산한다(하위 장치가 바뀌었을 때). 정의 drivers/pci/pcie/aspm.c */
#else /* [한국어] CONFIG_PCIEASPM 이 꺼진 경우 */
static inline void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap) { } /* [한국어] 스텁: 제거할 ASPM 능력 정보가 없다 */
static inline void pcie_aspm_init_link_state(struct pci_dev *pdev) { } /* [한국어] 스텁: ASPM 상태를 만들지 않는다 */
static inline void pcie_aspm_exit_link_state(struct pci_dev *pdev) { } /* [한국어] 스텁: 정리할 것이 없다 */
static inline void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked) { } /* [한국어] 스텁: 조정할 것이 없다 */
static inline void pcie_aspm_powersave_config_link(struct pci_dev *pdev) { } /* [한국어] 스텁: 재설정할 것이 없다 */
static inline void pci_configure_ltr(struct pci_dev *pdev) { } /* [한국어] 스텁: LTR 을 건드리지 않는다 */
static inline void pci_bridge_reconfigure_ltr(struct pci_dev *pdev) { } /* [한국어] 스텁: 브리지 LTR 재계산 없음 */
#endif /* [한국어] CONFIG_PCIEASPM 분기 종료 */

/* [한국어] --- ECRC(End-to-End CRC), CONFIG_PCIE_ECRC ---
 * ECRC 는 TLP 끝에 32비트 CRC 를 붙여, 링크 계층 CRC 가 잡지 못하는
 * "스위치 내부에서 생긴 데이터 손상"까지 종단 간에 검출하는 기능이다.
 * 링크 CRC 는 홉마다 새로 계산되므로 중간 장치가 데이터를 망가뜨리면
 * 검출되지 않는데, ECRC 는 송신자가 계산해 최종 수신자가 검사하므로
 * 그 구멍을 막는다. 대가는 TLP 마다 4바이트와 계산 비용이다.
 * "pcie_ecrc=" 커널 파라미터로 정책을 정한다(pcie_ecrc_get_policy()). */
#ifdef CONFIG_PCIE_ECRC
void pcie_set_ecrc_checking(struct pci_dev *dev); /* [한국어] ECRC 생성/검사를 정책에 따라 켜거나 끈다. 정의 drivers/pci/pcie/aer.c */
void pcie_ecrc_get_policy(char *str); /* [한국어] "pcie_ecrc=" 부팅 파라미터를 해석해 정책을 정한다. 정의 drivers/pci/pcie/aer.c */
#else /* [한국어] CONFIG_PCIE_ECRC 가 꺼진 경우 */
static inline void pcie_set_ecrc_checking(struct pci_dev *dev) { } /* [한국어] 스텁: ECRC 를 건드리지 않는다 */
static inline void pcie_ecrc_get_policy(char *str) { } /* [한국어] 스텁: 파라미터를 무시한다 */
#endif /* [한국어] CONFIG_PCIE_ECRC 분기 종료 */

/* [한국어] --- LBMS(Link Bandwidth Management Status) 초기화 ---
 * LBMS 는 "링크 대역폭이 바뀌었다"를 알리는 상태 비트다. 링크가 스스로
 * 속도를 낮추면 이 비트가 서고, 커널은 그것을 링크 품질 문제의 단서로 삼는다.
 * pcie_reset_lbms() 는 그 비트와 위 priv_flags 의 PCI_LINK_LBMS_SEEN 을
 * 함께 정리해, 이전 사건의 흔적이 새 판정에 섞이지 않게 한다. */
#ifdef CONFIG_PCIEPORTBUS
void pcie_reset_lbms(struct pci_dev *port); /* [한국어] Link Status 의 LBMS 비트와 priv_flags 의 PCI_LINK_LBMS_SEEN 을 초기화한다. 정의 drivers/pci/pcie/bwctrl.c */
#else /* [한국어] CONFIG_PCIEPORTBUS 가 꺼진 경우 */
static inline void pcie_reset_lbms(struct pci_dev *port) {} /* [한국어] 스텁: 정리할 LBMS 상태가 없다 */
#endif /* [한국어] CONFIG_PCIEPORTBUS 분기 종료 */

/* [한국어]
 * struct pci_dev_reset_methods - 특정 하드웨어 전용 리셋 방법 표의 한 줄
 *
 * 스펙 표준 리셋(FLR, D3hot 왕복, Secondary Bus Reset)이 통하지 않거나
 * 위험한 하드웨어가 있어, 벤더/장치별로 다른 절차를 등록해 둔다.
 * 실체: drivers/pci/quirks.c 의 pci_dev_reset_methods[] 배열.
 * 소비자: drivers/pci/quirks.c 의 __pci_dev_specific_reset() 이 이 표를 훑어
 * 현재 장치와 맞는 줄을 찾아 .reset 을 부른다.
 * 동기화: const 정적 배열이라 읽기 전용. */
struct pci_dev_reset_methods {
	/* [한국어] 이 항목이 적용될 Vendor ID. 설정자: quirks.c 의 정적 표. 읽는 자: quirks.c 의 매칭 함수.
 * 값 범위: PCI_ANY_ID 면 모든 벤더 */
	u16 vendor;
	/* [한국어] 이 항목이 적용될 Device ID. PCI_ANY_ID 면 해당 벤더의 모든 장치
 * (위 한 줄이 이 필드 설명의 전부다.) */
	u16 device;
	/* [한국어] 실제 리셋을 수행하는 콜백. @probe 가 참이면 "실제로 리셋하지 말고 지원 여부만 알려 달라"는 조회 모드다 — 이 두 용도를 한
 * 함수에 합친 것이 커널 리셋 API 의 공통 관례이며, pci_init_reset_methods() 가 probe 모드로 후보를 추려
 * 낸다 */
	int (*reset)(struct pci_dev *dev, bool probe);
};

/* [한국어]
 * struct pci_reset_fn_method - 커널이 쓸 수 있는 리셋 방법 하나의 서술자
 *
 * 위 pci_dev_reset_methods 가 "특정 하드웨어 전용 우회"인 반면, 이쪽은
 * "일반적으로 시도해 볼 리셋 방법들의 목록"이다.
 * 실체: pci_reset_fn_methods[] 배열(선언은 바로 아래, 정의는 drivers/pci/pci.c).
 * 소비자: drivers/pci/pci-sysfs.c 이 이름들을 sysfs 의 reset_method
 * 파일에 나열하고, 이 사용자가 쓴 이름을 찾아 매칭하며,
 * 이 probe 모드로 각 방법의 지원 여부를 조사한다.
 * 배열의 순서가 곧 우선순위이며, pci_init_reset_methods(drivers/pci/pci.c)
 * 가 이 순서대로 훑어 pci_dev.reset_methods[] 를 채운다.
 * 동기화: const 정적 배열이라 읽기 전용. */
struct pci_reset_fn_method {
	/* [한국어] 리셋을 수행하는 함수 포인터. @probe 는 위와 같은 조회/실행 겸용 플래그. 설정자: pci_reset_fn_methods[] 정적
 * 초기화. 읽는 자: drivers/pci/pci-sysfs.c (probe 조회)와 실제 리셋 경로 */
	int (*reset_fn)(struct pci_dev *pdev, bool probe);
	/* [한국어] 이 방법의 이름 문자열("flr", "pm", "bus" 등).
	 * 설정자: pci_reset_fn_methods[] 의 정적 초기화.
	 * 읽는 자: drivers/pci/pci-sysfs.c 의 reset_method_show() 가 sysfs 에 나열하고,
	 *   reset_method_lookup() 이 sysfs_streq() 로 사용자가 쓴 이름과 비교한다.
	 * 값 범위: NULL 이 아닌 짧은 소문자 문자열.
	 * 동기화: const 정적 배열이라 읽기 전용. */
	char *name;
};
extern const struct pci_reset_fn_method pci_reset_fn_methods[]; /* [한국어] 리셋 방법 목록 자체. 배열 순서가 우선순위다 */

/* [한국어] --- 장치 특화 리셋 (CONFIG_PCI_QUIRKS) ---
 * 위 pci_dev_reset_methods[] 표를 실제로 검색해 적용하는 함수다.
 * -ENOTTY 는 "이 장치에는 특화 리셋이 없으니 표준 방법을 쓰라"는 뜻이며,
 * quirk 가 컴파일에서 빠졌을 때의 스텁도 같은 값을 돌려주어 동작이 같아진다. */
#ifdef CONFIG_PCI_QUIRKS
int pci_dev_specific_reset(struct pci_dev *dev, bool probe); /* [한국어] quirk 표에서 이 장치 전용 리셋을 찾아 수행하거나(probe=false), 지원 여부만 확인한다(probe=true). 정의 drivers/pci/quirks.c */
#else /* [한국어] CONFIG_PCI_QUIRKS 가 꺼진 경우 */
static inline int pci_dev_specific_reset(struct pci_dev *dev, bool probe) /* [한국어] 스텁 시작 */
{
	return -ENOTTY; /* [한국어] -ENOTTY = 특화 리셋 없음. 호출자는 표준 리셋으로 넘어간다 */
}
#endif /* [한국어] CONFIG_PCI_QUIRKS 분기 종료 */

/* [한국어] --- ARM64 + quirk 전용: ACPI 로 Root Complex 리소스 얻기 ---
 * 두 조건의 AND 인 이유: 이 함수는 일부 ARM64 서버 SoC 의 호스트 브리지가
 * 표준 ACPI 방식으로 자기 레지스터 영역을 알려 주지 않아 생긴 우회 코드다.
 * @hid 로 ACPI 장치를 찾아 그 _CRS 에서 리소스를 꺼내 온다.
 * x86 에서는 아예 필요 없으므로 스텁(-ENODEV)만 남는다. */
#if defined(CONFIG_PCI_QUIRKS) && defined(CONFIG_ARM64)
int acpi_get_rc_resources(struct device *dev, const char *hid, u16 segment, /* [한국어] ACPI HID 로 장치를 찾아 그 리소스를 res 에 담아 온다. 정의 drivers/pci/pci-acpi.c */
			  struct resource *res);
#else /* [한국어] 두 조건 중 하나라도 빠진 경우 */
/* [한국어]
 * acpi_get_rc_resources() 스텁 (CONFIG_PCI_QUIRKS + CONFIG_ARM64 중 하나라도 꺼짐)
 * @dev: 요청 주체. @hid: 찾을 ACPI 하드웨어 ID. @segment: PCI 세그먼트 번호.
 * @res: 찾은 리소스를 담을 곳. @return(): -ENODEV.
 * x86 을 비롯한 대부분의 플랫폼에서는 호스트 브리지가 표준 방식으로 자기
 * 리소스를 알려 주므로 이 우회가 필요 없다. -ENODEV 를 받은 호출자는
 * 표준 경로로 진행한다.
 * 호출 체인: ARM64 SoC 호스트 브리지 드라이버 → [이 스텁] */
static inline int acpi_get_rc_resources(struct device *dev, const char *hid, /* [한국어] 스텁 시작 */
					u16 segment, struct resource *res)
{
	return -ENODEV; /* [한국어] -ENODEV = 그런 ACPI 장치가 없다. 호출자는 일반 경로로 진행한다 */
}
#endif /* [한국어] 조건 분기 종료 */

/* [한국어] --- Resizable BAR (rebar) ---
 * Resizable BAR 확장 capability 는 장치가 지원하는 BAR 크기 후보들을
 * 비트마스크로 광고하고, 소프트웨어가 그중 하나를 골라 쓰게 한다.
 * 크기는 2의 거듭제곱 코드로 표현되며(코드 n = 2^(n+20) 바이트, 즉 0=1MB),
 * 아래 get/set 함수가 그 코드를 다룬다.
 * 대표적 용도는 GPU 의 "Resizable BAR/Smart Access Memory" — 프레임버퍼
 * 전체를 CPU 주소 공간에 노출하는 것이다. 구현은 drivers/pci/rebar.c. */
void pci_rebar_init(struct pci_dev *pdev); /* [한국어] Resizable BAR capability 를 찾아 dev->rebar_cap 을 채운다. 정의 drivers/pci/rebar.c */
void pci_restore_rebar_state(struct pci_dev *pdev); /* [한국어] resume 후 선택해 둔 BAR 크기를 다시 써 넣는다. 정의 drivers/pci/rebar.c */
int pci_rebar_get_current_size(struct pci_dev *pdev, int bar); /* [한국어] 지금 설정된 BAR 크기를 코드값으로 돌려준다. 정의 drivers/pci/rebar.c */
int pci_rebar_set_size(struct pci_dev *pdev, int bar, int size); /* [한국어] BAR 크기를 코드값으로 설정한다. 정의 drivers/pci/rebar.c */

/* [한국어] struct device_node 전방 선언. 디바이스트리 노드 타입으로,
 * CONFIG_OF 가 꺼져 있어도 아래 스텁들의 인자 타입으로 이름이 필요하다.
 * 실제 정의는 <linux/of.h> 에 있으며 이 헤더는 그것을 포함하지 않는다. */
struct device_node;

/* [한국어] "이 자리에는 프리셋 값이 없다"를 나타내는 예약값.
 * 설정자/읽는 자: drivers/pci/of.c 의 of_pci_get_equalization_presets() 가
 * 디바이스트리에서 프리셋을 읽기 전에 배열 첫 칸을 이 값으로 채워 두고,
 * drivers/pci/controller/dwc/pcie-designware-host.c 의 dw_pcie_program_presets() 가
 * presets[0] == PCI_EQ_RESV 인지 보고 "설정 없음"이면 건드리지 않는다.
 * 0xff 를 고른 이유: 실제 프리셋 코드는 훨씬 작은 값이라 겹치지 않는다. */
#define PCI_EQ_RESV	0xff

/* [한국어]
 * enum equalization_preset_type - 링크 이퀄라이제이션 프리셋의 속도별 갈래
 *
 * 이퀄라이제이션이란 무엇인가: 8 GT/s 이상에서는 기판 배선의 신호 감쇠가
 * 커서, 송수신단이 파형을 보정하지 않으면 비트 오류가 난다. 그 보정 계수를
 * 링크 훈련 중에 협상하는 과정이 이퀄라이제이션이고, "프리셋"은 그 협상의
 * 출발점으로 쓸 미리 정해진 계수 조합이다. 보드 설계자가 자기 기판에 맞는
 * 값을 디바이스트리에 적어 두면 훈련이 빨리 수렴한다.
 * 속도마다 요구 보정이 달라 갈래가 나뉜다. 2.5/5.0 GT/s 는 이퀄라이제이션
 * 자체가 없어 목록에 없다.
 * 값 범위: EQ_PRESET_TYPE_MAX 는 개수를 세는 보초값이며, 아래 배열 크기
 * 계산(EQ_PRESET_TYPE_MAX - 1)에 쓰인다. */
enum equalization_preset_type {
	/* [한국어] 8 GT/s(Gen3)용 프리셋. 레인당 16비트가 필요해 아래 배열이 따로 있다
 * (위 한 줄이 이 필드 설명의 전부다.) */
	EQ_PRESET_TYPE_8GTS,
	/* [한국어] 16 GT/s(Gen4)용
 * (위 한 줄이 이 필드 설명의 전부다.) */
	EQ_PRESET_TYPE_16GTS,
	/* [한국어] 32 GT/s(Gen5)용
 * (위 한 줄이 이 필드 설명의 전부다.) */
	EQ_PRESET_TYPE_32GTS,
	/* [한국어] 64 GT/s(Gen6)용
 * (위 한 줄이 이 필드 설명의 전부다.) */
	EQ_PRESET_TYPE_64GTS,
	EQ_PRESET_TYPE_MAX /* [한국어] 개수를 세는 보초값. 실제 프리셋 타입 개수는 4이고, 아래 배열 첫 차원은 8GTS 를 뺀 3이다 */
};

/* [한국어]
 * struct pci_eq_presets - 레인별 이퀄라이제이션 프리셋 값 보관함
 *
 * 왜 8GTS 만 따로 떨어져 있는가: 8 GT/s(Gen3)의 프리셋은 송신단 프리셋과
 * 수신단 힌트를 함께 담아야 해서 레인당 16비트가 필요하고, 16 GT/s 이상은
 * 레인당 8비트면 충분하다. 타입이 달라 한 배열에 담을 수 없어 둘로 나뉘었다.
 * 설정자: drivers/pci/of.c 의 of_pci_get_equalization_presets() 가
 * 디바이스트리 속성에서 읽어 채운다.
 * 읽는 자: drivers/pci/controller/dwc/pcie-designware-host.c 의
 * dw_pcie_program_presets() 가 속도에 맞는 배열을 골라 컨트롤러 레지스터에 써 넣는다.
 * 동기화: 호스트 브리지 초기화 때 한 번 채우고 이후 읽기 전용
 * (drivers/pci/controller/dwc/pcie-designware.h 의 struct dw_pcie_rp 가
 * 이 구조체를 값으로 품는다). */
struct pci_eq_presets {
	/* [한국어] Gen3(8 GT/s) 프리셋: 레인마다 16비트.
	 * 설정자: drivers/pci/of.c 의 of_pci_get_equalization_presets().
	 * 읽는 자: drivers/pci/controller/dwc/pcie-designware-host.c 의
	 *   dw_pcie_program_presets() 가 (u8 *) 로 캐스팅해 바이트 스트림으로
	 *   하드웨어에 밀어 넣는다.
	 * 값 범위: 각 칸이 프리셋 코드, 첫 칸이 PCI_EQ_RESV 면 "설정 없음".
	 * 동기화: 호스트 브리지 초기화 때 한 번 채우고 이후 읽기 전용. */
	u16 eq_presets_8gts[MAX_NR_LANES];
	/* [한국어] Gen4/5/6 프리셋: [타입][레인] 2차원 배열, 레인당 8비트. 첫 차원 크기가 EQ_PRESET_TYPE_MAX - 1 인 것은
 * 8GTS 를 위 배열이 따로 맡기 때문이고, 그래서 인덱싱할 때 EQ_PRESET_TYPE_16GTS - 1 처럼 1을 빼야
 * 한다(drivers/pci/controller/dwc/pcie-designware-host.c) */
	u8 eq_presets_Ngts[EQ_PRESET_TYPE_MAX - 1][MAX_NR_LANES];
};

/* [한국어] --- 디바이스트리(OF, Open Firmware) 연동, CONFIG_OF ---
 * x86 은 ACPI 로 PCI 토폴로지를 서술하지만, ARM/RISC-V 등 임베디드 계열은
 * 디바이스트리로 서술한다. 이 블록은 그 디바이스트리에서 PCI 관련 속성을
 * 읽어 오는 함수들이다.
 * 꺼졌을 때의 스텁들이 각각 어떤 값을 주는지가 중요하다:
 * of_get_pci_domain_nr() → -1 : "도메인 번호 지정 없음"
 * of_pci_get_max_link_speed() → -EINVAL : "속도 제한 없음"
 * of_pci_get_slot_power_limit() → 0 (출력 인자도 0으로) : "전력 한도 없음"
 * of_pci_preserve_config() → false : "펌웨어 설정을 보존하지 말고 재배정하라"
 * of_pci_supply_present() → false : "전원 공급 노드 없음"
 * of_pci_get_equalization_presets() → 배열을 PCI_EQ_RESV 로 채우고 0
 * 즉 전부 "정보 없음 = 기본 동작"으로 수렴하도록 값이 골라져 있다. */
#ifdef CONFIG_OF
int of_get_pci_domain_nr(struct device_node *node); /* [한국어] 디바이스트리의 linux,pci-domain 속성에서 PCI 도메인 번호를 읽는다. 정의 drivers/pci/of.c */
int of_pci_get_max_link_speed(struct device_node *node); /* [한국어] max-link-speed 속성을 읽는다. 보드 배선이 감당 못 하는 속도로 링크가 올라가지 않게 제한하는 값이다. 정의 drivers/pci/of.c */
u32 of_pci_get_slot_power_limit(struct device_node *node, /* [한국어] 슬롯이 공급할 수 있는 전력 한도를 읽는다. PCIe 의 Slot Capabilities 에는 전력 한도를 value(8비트)와 scale(2비트)로 나눠 표현하는 규칙이 있어 출력 인자가 둘이다. 정의 drivers/pci/of.c */
				u8 *slot_power_limit_value,
				u8 *slot_power_limit_scale);
bool of_pci_preserve_config(struct device_node *node); /* [한국어] 디바이스트리가 "펌웨어가 해 둔 BAR 배정을 그대로 두라"고 지시했는지 확인한다. 정의 drivers/pci/of.c — 펌웨어가 이미 동작 중인 장치를 재배정하면 화면이 꺼지는 등의 문제가 생기는 플랫폼을 위한 것 */
int pci_set_of_node(struct pci_dev *dev); /* [한국어] 이 pci_dev 에 대응하는 디바이스트리 노드를 찾아 연결한다. 정의 drivers/pci/of.c */
void pci_release_of_node(struct pci_dev *dev); /* [한국어] 그 연결을 해제한다(노드 참조 계수 감소). 정의 drivers/pci/of.c */
void pci_set_bus_of_node(struct pci_bus *bus); /* [한국어] pci_bus 에 디바이스트리 노드를 연결한다. 정의 drivers/pci/of.c */
void pci_release_bus_of_node(struct pci_bus *bus); /* [한국어] 해제한다. 정의 drivers/pci/of.c */

int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge); /* [한국어] 디바이스트리 정보로 호스트 브리지를 초기화한다(devm_ 접두사이므로 device 수명에 묶여 자동 해제된다). 정의 drivers/pci/of.c */
bool of_pci_supply_present(struct device_node *np); /* [한국어] 이 노드에 전원 공급(regulator) 서술이 있는지 확인한다. 정의 drivers/pci/of.c */
int of_pci_get_equalization_presets(struct device *dev, /* [한국어] 위 struct pci_eq_presets 를 디바이스트리에서 채운다. num_lanes 는 실제 링크 폭. 정의 drivers/pci/of.c */
				    struct pci_eq_presets *presets,
				    int num_lanes);
#else /* [한국어] CONFIG_OF 가 꺼진 경우 — 아래 스텁들이 모두 "정보 없음"에 해당하는 값을 돌려준다 */
static inline int /* [한국어] 스텁 시작 */
of_get_pci_domain_nr(struct device_node *node)
{
	return -1; /* [한국어] -1 = 도메인 번호 지정 없음. 호출자는 커널이 알아서 번호를 매긴다 */
}

static inline int /* [한국어] 스텁 시작 */
of_pci_get_max_link_speed(struct device_node *node)
{
	return -EINVAL; /* [한국어] -EINVAL = 속도 제한 지정 없음. 호출자는 하드웨어가 협상한 속도를 그대로 쓴다 */
}

/* [한국어] of_pci_get_slot_power_limit() 스텁: 디바이스트리가 없으므로
 * "전력 한도 정보 없음"을 뜻하는 0 을 출력 인자 두 곳에 모두 써 주고 0 을
 * 반환한다. 아래 두 대입문
 * *slot_power_limit_value = 0;
 * *slot_power_limit_scale = 0;
 * 은 각각 "값 필드를 0 으로", "스케일 필드를 0 으로" 두는 것이며, 인라인
 * 주석 대신 여기에 적은 이유는 그 줄들의 첫 글자가 * 라서 주석 줄로 오인되는
 * 형태이기 때문이다(원문 텍스트를 그대로 보존해야 한다).
 * 0 은 PCIe Slot Capabilities 의 Slot Power Limit Value/Scale 자리에 그대로
 * 들어가며, 스펙상 "제한을 광고하지 않음"에 해당한다. */
static inline u32 /* [한국어] 스텁 시작 */
of_pci_get_slot_power_limit(struct device_node *node,
			    u8 *slot_power_limit_value,
			    u8 *slot_power_limit_scale)
{
	if (slot_power_limit_value) /* [한국어] 출력 인자가 NULL 이 아닐 때만 쓴다 — 호출자가 둘 중 하나만 원할 수 있다 */
		*slot_power_limit_value = 0;
	if (slot_power_limit_scale) /* [한국어] 스케일 인자도 같은 방식으로 방어 */
		*slot_power_limit_scale = 0;
	return 0; /* [한국어] 반환값 0 = 디바이스트리에 전력 한도 속성이 없었다 */
}

/* [한국어]
 * of_pci_preserve_config() 스텁 (CONFIG_OF 꺼짐)
 * @node: 디바이스트리 노드. @return(): false.
 * 디바이스트리가 없으니 "펌웨어 설정을 보존하라"는 지시도 있을 수 없다.
 * false 는 커널이 BAR 와 브리지 윈도우를 자유롭게 재배정해도 좋다는 뜻이다.
 * 호출 체인: drivers/pci/probe.c 의 호스트 브리지 초기화 → [이 스텁] */
static inline bool of_pci_preserve_config(struct device_node *node) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 펌웨어 설정을 보존하라는 지시가 없다 → 커널이 리소스를 자유롭게 재배정해도 된다 */
}

/* [한국어]
 * pci_set_of_node() 스텁 (CONFIG_OF 꺼짐)
 * @dev: 방금 만들어진 pci_dev. @return(): 0(성공).
 * 연결할 디바이스트리 노드가 없지만, 이것은 오류가 아니다. 0 이 아닌 값을
 * 돌려주면 장치 생성 자체가 실패하므로 반드시 0 이어야 한다.
 * 호출 체인: drivers/pci/probe.c 의 pci_setup_device() → [이 스텁]
 * (바로 아래 세 스텁도 같은 이유로 무동작이다.) */
static inline int pci_set_of_node(struct pci_dev *dev) { return 0; } /* [한국어] 스텁: 연결할 디바이스트리 노드가 없으므로 성공(0)만 반환 */
static inline void pci_release_of_node(struct pci_dev *dev) { } /* [한국어] 스텁: 해제할 노드가 없다 */
static inline void pci_set_bus_of_node(struct pci_bus *bus) { } /* [한국어] 스텁: 버스에 연결할 노드가 없다 */
static inline void pci_release_bus_of_node(struct pci_bus *bus) { } /* [한국어] 스텁: 해제할 것이 없다 */

static inline int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge) /* [한국어] 스텁 시작 */
{
	return 0; /* [한국어] 0 = 초기화할 디바이스트리 정보가 없지만 실패는 아니다 */
}

/* [한국어]
 * of_pci_supply_present() 스텁 (CONFIG_OF 꺼짐)
 * @np: 디바이스트리 노드. @return(): false.
 * 디바이스트리로 서술된 전원 공급(regulator) 노드가 없다는 뜻. 호출자는
 * 전원을 직접 켜는 절차를 건너뛴다. */
static inline bool of_pci_supply_present(struct device_node *np) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 전원 공급 노드가 없다 */
}

/* [한국어]
 * of_pci_get_equalization_presets() 스텁 (CONFIG_OF 꺼짐)
 * @dev: 요청 주체. @presets: 채울 프리셋 보관함. @num_lanes: 링크 폭.
 * @return(): 항상 0(성공).
 * 이 블록의 스텁 중 유일하게 실제로 무언가를 한다. 그냥 0 만 돌려주면
 * presets 안이 쓰레기 값으로 남아, 호출자가 그것을 진짜 프리셋으로 알고
 * 하드웨어에 써 넣을 수 있다. 그래서 각 배열의 첫 칸에 PCI_EQ_RESV 를
 * 채워 "값 없음"을 명시적으로 표시한다 — 스텁이라도 출력 인자는 반드시
 * 정의된 상태로 만들어 줘야 한다는 원칙의 예다.
 * 호출 체인: drivers/pci/controller/dwc/pcie-designware-host.c → [이 스텁] */
static inline int of_pci_get_equalization_presets(struct device *dev, /* [한국어] 스텁 시작 — 이 스텁만 유일하게 실제 동작을 한다 */
						  struct pci_eq_presets *presets,
						  int num_lanes)
{
	presets->eq_presets_8gts[0] = PCI_EQ_RESV; /* [한국어] 배열 첫 칸을 예약값으로 채워 "프리셋 없음"을 표시한다. 호출자(pcie-designware-host.c)가 presets[0] == PCI_EQ_RESV 인지로 판정하므로, 첫 칸만 채우면 충분하다 */
	for (int i = 0; i < EQ_PRESET_TYPE_MAX - 1; i++) /* [한국어] Gen4/5/6 세 갈래에 대해서도 같은 표시를 남긴다. EQ_PRESET_TYPE_MAX - 1 = 3 */
		presets->eq_presets_Ngts[i][0] = PCI_EQ_RESV; /* [한국어] 각 타입의 첫 레인 자리에 예약값을 넣는다 */

	return 0; /* [한국어] 0 = 오류 없음. "값이 없다"는 것은 위 예약값으로 전달된다 */
}
#endif /* CONFIG_OF */ /* [한국어] CONFIG_OF 분기 종료 — 원문 주석이 짝을 밝혀 준다 */

/* [한국어] struct of_changeset 전방 선언. 디바이스트리를 런타임에 고칠 때
 * 변경 사항을 원자적으로 모아 두는 객체다. 아래 of_pci_add_properties() 의
 * 인자 타입으로만 필요하므로 이름만 알려 준다. */
struct of_changeset;

/* [한국어] --- 런타임 디바이스트리 노드 생성, CONFIG_PCI_DYNAMIC_OF_NODES ---
 * 열거로 발견한 PCI 장치에 대해 디바이스트리 노드를 커널이 즉석에서 만들어
 * 준다. 왜 필요한가: PCI 장치는 핫플러그로 나타나므로 정적 디바이스트리에
 * 미리 적어 둘 수 없는데, 그 장치에 매달린 하위 장치(예: PCIe 카드 위의
 * I2C 센서)를 디바이스트리 방식으로 서술하려면 부모 노드가 있어야 한다.
 * 꺼지면 노드를 만들지 않는 빈 스텁만 남고, 노드를 쓰는 코드도 함께 빠진다
 * (of_pci_add_properties() 계열에는 스텁이 없는데, 그 함수들은 위 make_*_node
 * 안에서만 불리기 때문이다). */
#ifdef CONFIG_PCI_DYNAMIC_OF_NODES
void of_pci_make_dev_node(struct pci_dev *pdev); /* [한국어] 이 pci_dev 에 대응하는 디바이스트리 노드를 런타임에 만든다. 정의 drivers/pci/of.c */
void of_pci_remove_node(struct pci_dev *pdev); /* [한국어] 장치 제거 시 그 노드를 없앤다. 정의 drivers/pci/of.c */
int of_pci_add_properties(struct pci_dev *pdev, struct of_changeset *ocs, /* [한국어] 만든 노드에 PCI 관련 속성(reg, device_type, ranges 등)을 채워 넣는다. 변경은 of_changeset 에 모았다가 한꺼번에 적용된다. 정의 drivers/pci/of_property.c */
			  struct device_node *np);
void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge); /* [한국어] 호스트 브리지용 노드를 만든다. 정의 drivers/pci/of.c */
void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge); /* [한국어] 그 노드를 없앤다. 정의 drivers/pci/of.c */
int of_pci_add_host_bridge_properties(struct pci_host_bridge *bridge, /* [한국어] 호스트 브리지 노드의 속성을 채운다. 정의 drivers/pci/of_property.c */
				      struct of_changeset *ocs,
				      struct device_node *np);
#else /* [한국어] CONFIG_PCI_DYNAMIC_OF_NODES 가 꺼진 경우 */
static inline void of_pci_make_dev_node(struct pci_dev *pdev) { } /* [한국어] 스텁: 노드를 만들지 않는다 */
static inline void of_pci_remove_node(struct pci_dev *pdev) { } /* [한국어] 스텁: 없앨 노드가 없다 */
static inline void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge) { } /* [한국어] 스텁: 호스트 브리지용 디바이스트리 노드를 만들지 않는다 */
static inline void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge) { } /* [한국어] 스텁: 없앨 노드가 없다. of_pci_add_properties() 와 of_pci_add_host_bridge_properties() 에는 스텁이 없는데, 그 둘은 위 make 계열 함수 안에서만 불리므로 여기서는 도달할 수 없기 때문이다 */
#endif /* [한국어] CONFIG_PCI_DYNAMIC_OF_NODES 분기 종료 */

/* [한국어] --- AER 초기화/상태 정리 API (CONFIG_PCIEAER) ---
 * 위쪽 CONFIG_PCIEAER 블록이 자료구조와 로그 출력이었다면, 이 블록은
 * 생명주기와 상태 레지스터 조작이다.
 * AER 상태 레지스터는 write-1-to-clear 방식이라, 읽어 처리한 뒤 같은 값을
 * 다시 써 넣어야 비트가 지워진다. 지우지 않으면 같은 오류가 계속 재보고된다.
 * 꺼졌을 때의 스텁 중 clear 계열만 -EINVAL 을 돌려주는데, "지울 AER 상태가
 * 없다"는 뜻이며 호출자는 이를 치명적 오류로 보지 않는다. */
#ifdef CONFIG_PCIEAER
void pci_no_aer(void); /* [한국어] AER 을 전역으로 끈다("pci=noaer"). 정의 drivers/pci/pcie/aer.c */
void pci_aer_init(struct pci_dev *dev); /* [한국어] 장치의 AER capability 를 찾아 초기화하고 보고를 활성화한다. 정의 drivers/pci/pcie/aer.c */
void pci_aer_exit(struct pci_dev *dev); /* [한국어] 장치 제거 시 AER 관련 자원을 정리한다. 정의 drivers/pci/pcie/aer.c */
extern const struct attribute_group aer_stats_attr_group; /* [한국어] 누적 AER 통계를 보여 주는 sysfs 속성 그룹 */
extern const struct attribute_group aer_attr_group; /* [한국어] AER 제어용 sysfs 속성 그룹 */
void pci_aer_clear_fatal_status(struct pci_dev *dev); /* [한국어] Uncorrectable Fatal 상태 비트만 골라 지운다. 정의 drivers/pci/pcie/aer.c */
int pci_aer_clear_status(struct pci_dev *dev); /* [한국어] 정상 경로에서 AER 상태를 지운다(장치가 접근 가능할 때). 정의 drivers/pci/pcie/aer.c */
int pci_aer_raw_clear_status(struct pci_dev *dev); /* [한국어] "raw" 판 — 상위 정책 검사 없이 상태 레지스터를 곧바로 지운다. 정의 drivers/pci/pcie/aer.c */
void pci_save_aer_state(struct pci_dev *dev); /* [한국어] suspend 전에 AER 마스크/심각도 설정을 저장한다. 정의 drivers/pci/pcie/aer.c */
void pci_restore_aer_state(struct pci_dev *dev); /* [한국어] resume 후 되돌린다. 정의 drivers/pci/pcie/aer.c */
#else /* [한국어] CONFIG_PCIEAER 가 꺼진 경우 */
static inline void pci_no_aer(void) { } /* [한국어] 스텁: 끌 AER 이 없다 */
static inline void pci_aer_init(struct pci_dev *d) { } /* [한국어] 스텁: 초기화할 것이 없다. 인자 이름 d 는 원본 그대로 */
static inline void pci_aer_exit(struct pci_dev *d) { } /* [한국어] 스텁: 정리할 것이 없다 */
static inline void pci_aer_clear_fatal_status(struct pci_dev *dev) { } /* [한국어] 스텁: 지울 상태가 없다 */
static inline int pci_aer_clear_status(struct pci_dev *dev) { return -EINVAL; } /* [한국어] 스텁: -EINVAL = 지울 AER 상태가 없다. 호출자는 실패로 취급하지 않는다 */
static inline int pci_aer_raw_clear_status(struct pci_dev *dev) { return -EINVAL; } /* [한국어] 스텁: 같은 이유로 -EINVAL */
static inline void pci_save_aer_state(struct pci_dev *dev) { } /* [한국어] 스텁: 저장할 AER 상태가 없다 */
static inline void pci_restore_aer_state(struct pci_dev *dev) { } /* [한국어] 스텁: 복원할 것이 없다 */
#endif /* [한국어] CONFIG_PCIEAER 분기 종료 */

/* [한국어] --- ACPI 연동 (CONFIG_ACPI) ---
 * x86 서버/PC 에서는 펌웨어(ACPI)가 PCI 장치의 전원 상태를 직접 제어하는
 * 경우가 많다. 장치의 ACPI 노드에 있는 _PS0/_PS3 메서드가 D0/D3 전이를
 * 수행하고, _PR0/_PR3 가 전원 자원을 서술하며, _S0W/_DSW 가 웨이크업 능력을
 * 알려 준다. 그래서 커널의 전원 관리 코드는 "PCI PM Capability 로 직접
 * 바꿀 것인가, 아니면 ACPI 에게 맡길 것인가"를 매번 판단해야 하고,
 * 아래 acpi_pci_* 함수들이 그 판단과 위임을 담당한다.
 * 특히 D3cold(전원 완전 차단)는 PCI 레지스터만으로는 도달할 수 없다 —
 * 슬롯 전원을 끊을 수 있는 것은 플랫폼뿐이라, 반드시 ACPI 를 거친다.
 * 꺼졌을 때의 스텁들은 모두 "플랫폼이 아무것도 해 주지 않는다"에 해당하는
 * 값(false / -ENODEV / PCI_UNKNOWN / PCI_POWER_ERROR)을 돌려주어,
 * 커널이 PCI 레지스터만으로 할 수 있는 일을 하도록 유도한다.
 * NVMe 접점: 노트북에서 NVMe SSD 를 D3cold 까지 내리는 절전(그리고 그와
 * 얽힌 resume 실패 문제)은 이 ACPI 경로를 탄다. NVMe 드라이버 자신이
 * 이 함수들을 부르지는 않지만, nvme_suspend 가 결정한 정책이
 * PCI 코어를 거쳐 여기로 흘러든다. */
#ifdef CONFIG_ACPI
bool pci_acpi_preserve_config(struct pci_host_bridge *bridge); /* [한국어] ACPI 가 "펌웨어가 배정한 리소스를 그대로 두라"고 지시했는지 확인한다(_DSM 기반). 정의 drivers/pci/pci-acpi.c */
int pci_acpi_program_hp_params(struct pci_dev *dev); /* [한국어] ACPI _HPX/_HPP 가 준 핫플러그 파라미터를 장치에 적용한다. 정의 drivers/pci/pci-acpi.c — 이 경로가 PCI_EXP_AER_FLAGS 를 마스크로 쓰는 곳이다 */
extern const struct attribute_group pci_dev_acpi_attr_group; /* [한국어] ACPI 관련 정보를 노출하는 sysfs 속성 그룹 */
void pci_set_acpi_fwnode(struct pci_dev *dev); /* [한국어] pci_dev 에 대응하는 ACPI 노드를 찾아 fwnode 로 연결한다. 정의 drivers/pci/pci-acpi.c */
int pci_dev_acpi_reset(struct pci_dev *dev, bool probe); /* [한국어] ACPI _RST 메서드로 장치를 리셋한다. probe=true 면 지원 여부만 확인. 정의 drivers/pci/pci-acpi.c — 리셋 후보 목록의 첫 번째 방법이다 */
bool acpi_pci_power_manageable(struct pci_dev *dev); /* [한국어] 플랫폼이 이 장치의 전원을 관리할 수 있는지(_PS0/_PS3 또는 전원 자원이 있는지). 정의 drivers/pci/pci-acpi.c */
bool acpi_pci_bridge_d3(struct pci_dev *dev); /* [한국어] 이 브리지를 D3 로 내려도 되는지 플랫폼에 물어본다. 정의 drivers/pci/pci-acpi.c */
int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state); /* [한국어] ACPI 를 통해 전원 상태를 바꾼다. D3cold 는 이 경로로만 가능하다. 정의 drivers/pci/pci-acpi.c */
pci_power_t acpi_pci_get_power_state(struct pci_dev *dev); /* [한국어] 플랫폼이 보는 현재 전원 상태를 읽는다. 정의 drivers/pci/pci-acpi.c */
void acpi_pci_refresh_power_state(struct pci_dev *dev); /* [한국어] 플랫폼 쪽 상태를 다시 확인해 커널의 인식과 맞춘다. 정의 drivers/pci/pci-acpi.c */
int acpi_pci_wakeup(struct pci_dev *dev, bool enable); /* [한국어] 이 장치의 웨이크업(GPE)을 켜거나 끈다. 정의 drivers/pci/pci-acpi.c */
bool acpi_pci_need_resume(struct pci_dev *dev); /* [한국어] 플랫폼 관점에서 이 장치가 resume 되어야 하는지 판정한다. 정의 drivers/pci/pci-acpi.c */
pci_power_t acpi_pci_choose_state(struct pci_dev *pdev); /* [한국어] 시스템 sleep 상태(S3 등)에 대응하는 장치 D-state 를 플랫폼(_SxD/_SxW)에 물어 고른다. 정의 drivers/pci/pci-acpi.c */
#else /* [한국어] CONFIG_ACPI 가 꺼진 경우 */
static inline bool pci_acpi_preserve_config(struct pci_host_bridge *bridge) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 보존 지시 없음 → 커널이 리소스를 재배정해도 된다 */
}
/* [한국어]
 * pci_dev_acpi_reset() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 리셋 대상. @probe: 참이면 지원 여부만 조회. @return(): -ENOTTY.
 * ACPI _RST 메서드를 부를 수 없다는 뜻. 리셋 후보 목록을 만드는
 * pci_init_reset_methods() 는 이 값을 보고 ACPI 리셋을 후보에서 제외하고
 * FLR 등 다음 방법으로 넘어간다.
 * 호출 체인: pci_init_reset_methods(drivers/pci/pci.c) → [이 스텁] */
static inline int pci_dev_acpi_reset(struct pci_dev *dev, bool probe) /* [한국어] 스텁 시작 */
{
	return -ENOTTY; /* [한국어] -ENOTTY = ACPI 리셋 방법 없음 → 다음 리셋 후보로 넘어간다 */
}
/* [한국어]
 * pci_set_acpi_fwnode() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): 없음.
 * 연결할 ACPI 노드가 없으므로 fwnode 를 그대로 둔다. 장치 생성 경로가
 * #ifdef 없이 호출할 수 있게 하는 것이 존재 이유다. */
static inline void pci_set_acpi_fwnode(struct pci_dev *dev) { } /* [한국어] 스텁: 연결할 ACPI 노드가 없다 */
/* [한국어]
 * pci_acpi_program_hp_params() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): -ENODEV.
 * 펌웨어가 준 핫플러그 파라미터(_HPX/_HPP)가 없으므로 적용할 것도 없다.
 * 호출자는 커널 기본값으로 장치를 설정한다. */
static inline int pci_acpi_program_hp_params(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] -ENODEV = 적용할 펌웨어 파라미터가 없다 */
}
/* [한국어]
 * acpi_pci_power_manageable() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): false.
 * 플랫폼이 이 장치의 전원을 관리해 주지 않는다는 뜻. 그 결과 커널은
 * PCI PM Capability 로 직접 할 수 있는 범위(D0~D3hot)만 다루고,
 * 플랫폼만 할 수 있는 D3cold 는 시도하지 않는다. */
static inline bool acpi_pci_power_manageable(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 플랫폼 전원 관리 없음 → 커널이 PCI PM Capability 만으로 처리한다 */
}
/* [한국어]
 * acpi_pci_bridge_d3() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 브리지. @return(): false.
 * 플랫폼이 이 브리지의 D3 를 보장하지 않는다는 뜻이라, pci_bridge_d3_possible()
 * 은 다른 근거가 없으면 브리지를 D3 로 내리지 않는다. */
static inline bool acpi_pci_bridge_d3(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 브리지 D3 를 플랫폼이 보장하지 않는다 */
}
/* [한국어]
 * acpi_pci_set_power_state() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @state: 목표 D-state. @return(): -ENODEV.
 * 플랫폼이 전원 상태를 바꿔 줄 수 없다. 특히 D3cold 는 슬롯 전원을 끊는
 * 것이라 PCI 레지스터만으로는 불가능하므로, 이 스텁이 있는 커널에서는
 * D3cold 로 갈 방법이 없다. */
static inline int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] -ENODEV = 플랫폼이 전원 상태를 바꿔 줄 수 없다. 그 결과 D3cold 로는 갈 수 없다 */
}
/* [한국어]
 * acpi_pci_get_power_state() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): PCI_UNKNOWN.
 * 플랫폼이 상태를 알려 주지 않으므로 "모름"을 반환한다. 호출자는
 * PCI PM Capability 의 PMCSR 을 직접 읽어 판단한다. */
static inline pci_power_t acpi_pci_get_power_state(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return PCI_UNKNOWN; /* [한국어] PCI_UNKNOWN = 플랫폼이 상태를 알려 주지 않는다 */
}
/* [한국어]
 * acpi_pci_refresh_power_state() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): 없음. 갱신할 플랫폼 상태가 없다. */
static inline void acpi_pci_refresh_power_state(struct pci_dev *dev) { } /* [한국어] 스텁: 갱신할 플랫폼 상태가 없다 */
/* [한국어]
 * acpi_pci_wakeup() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @enable: 켤지 끌지. @return(): -ENODEV.
 * 플랫폼 웨이크업(ACPI GPE) 설정을 할 수 없다는 뜻. PCI PME 자체는
 * 여전히 PM Capability 로 켤 수 있지만, 그 PME 를 시스템 웨이크업으로
 * 승격시켜 주는 플랫폼 경로가 없다. */
static inline int acpi_pci_wakeup(struct pci_dev *dev, bool enable) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] -ENODEV = 플랫폼 웨이크업 설정 불가 */
}
/* [한국어]
 * acpi_pci_need_resume() 스텁 (CONFIG_ACPI 꺼짐)
 * @dev: 대상 장치. @return(): false.
 * 플랫폼이 "이 장치는 반드시 깨워야 한다"고 요구하지 않는다는 뜻. */
static inline bool acpi_pci_need_resume(struct pci_dev *dev) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = 플랫폼이 resume 을 요구하지 않는다 */
}
/* [한국어]
 * acpi_pci_choose_state() 스텁 (CONFIG_ACPI 꺼짐)
 * @pdev: 대상 장치. @return(): PCI_POWER_ERROR.
 * 시스템 sleep 상태에 대응할 D-state 를 플랫폼이 골라 주지 못했다는 뜻이며,
 * 호출자는 이 값을 보고 커널 기본 정책(대개 D3hot)으로 결정한다.
 * PCI_POWER_ERROR 는 유효한 D-state 가 아니라 "결정 실패" 표식이다. */
static inline pci_power_t acpi_pci_choose_state(struct pci_dev *pdev) /* [한국어] 스텁 시작 */
{
	return PCI_POWER_ERROR; /* [한국어] PCI_POWER_ERROR = 플랫폼이 목표 상태를 골라 주지 못했다. 호출자는 커널 기본 정책으로 결정한다 */
}
#endif /* [한국어] CONFIG_ACPI 분기 종료 */

/* [한국어] ASPM 제어용 sysfs 속성 그룹(link_pm 관련 파일들).
 * CONFIG_PCIEASPM 이 꺼지면 보여 줄 것이 없으므로 선언 자체가 사라지고,
 * 이 심볼을 참조하는 코드도 같은 #ifdef 안에 있어 스텁이 필요 없다. */
#ifdef CONFIG_PCIEASPM
extern const struct attribute_group aspm_ctrl_attr_group; /* [한국어] ASPM 제어용 sysfs 속성 그룹. 실체는 drivers/pci/pcie/aspm.c 에 있다 */
#endif /* [한국어] CONFIG_PCIEASPM 분기 종료 */

/* [한국어] --- Intel MID 플랫폼 전용 전원 관리 (CONFIG_X86_INTEL_MID) ---
 * MID(Mobile Internet Device)는 인텔의 옛 Atom 기반 모바일 SoC 계열이다.
 * 이 플랫폼의 PCI 장치들은 표준 PM Capability 대신 SoC 고유의 전원 관리
 * 유닛을 통해 D-state 를 바꿔야 해서, 별도 진입점이 필요하다.
 * pci_use_mid_pm() 이 참일 때만 나머지 둘이 쓰이며, 그 함수가 거짓이면
 * 커널은 표준 경로로 간다. 꺼졌을 때 스텁이 false 를 돌려주는 것이
 * 정확히 그 "표준 경로로 가라"를 뜻한다.
 * 오늘날 NVMe 환경과는 무관한 레거시 플랫폼이다. */
#ifdef CONFIG_X86_INTEL_MID
bool pci_use_mid_pm(void); /* [한국어] 이 플랫폼에서 MID 전용 전원 관리를 써야 하는지. 정의 drivers/pci/pci-mid.c */
int mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state); /* [한국어] MID 전용 방식으로 D-state 를 바꾼다. 정의 drivers/pci/pci-mid.c */
pci_power_t mid_pci_get_power_state(struct pci_dev *pdev); /* [한국어] MID 전용 방식으로 현재 D-state 를 읽는다. 정의 drivers/pci/pci-mid.c */
#else /* [한국어] CONFIG_X86_INTEL_MID 가 꺼진 경우 */
static inline bool pci_use_mid_pm(void) /* [한국어] 스텁 시작 */
{
	return false; /* [한국어] false = MID 경로를 쓰지 않는다 → 표준 PCI PM 경로로 간다 */
}
/* [한국어]
 * mid_pci_set_power_state() 스텁 (CONFIG_X86_INTEL_MID 꺼짐)
 * @pdev: 대상 장치. @state: 목표 D-state. @return(): -ENODEV.
 * MID 전원 관리 유닛이 없다. 위 pci_use_mid_pm() 스텁이 false 를 주므로
 * 정상 흐름에서는 이 함수가 불리지 않는다. */
static inline int mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] -ENODEV = MID 전원 관리 없음 */
}
/* [한국어]
 * mid_pci_get_power_state() 스텁 (CONFIG_X86_INTEL_MID 꺼짐)
 * @pdev: 대상 장치. @return(): PCI_UNKNOWN("모름").
 * 위와 같은 이유로 정상 흐름에서는 도달하지 않는다. */
static inline pci_power_t mid_pci_get_power_state(struct pci_dev *pdev) /* [한국어] 스텁 시작 */
{
	return PCI_UNKNOWN; /* [한국어] PCI_UNKNOWN = 알 수 없음 */
}
#endif /* [한국어] CONFIG_X86_INTEL_MID 분기 종료 */

/* [한국어] --- MSI-X 항목에 TPH Steering Tag 쓰기 (CONFIG_PCI_MSI) ---
 * 위에서 본 TPH 는 "이 데이터를 어느 캐시에 두라"는 힌트인데, 그 힌트에
 * 쓰일 Steering Tag 를 MSI-X 테이블 항목에 넣어 두는 방식이 있다.
 * 그러면 인터럽트를 처리할 CPU 와 데이터가 놓일 캐시를 맞출 수 있다.
 * MSI 지원이 없으면 MSI-X 테이블 자체가 없으므로 스텁이 -ENODEV 를 준다. */
#ifdef CONFIG_PCI_MSI
int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag); /* [한국어] MSI-X 테이블의 index 번째 항목에 TPH Steering Tag 를 써 넣는다. 정의 drivers/pci/msi/msi.c */
#else /* [한국어] CONFIG_PCI_MSI 가 꺼진 경우 */
static inline int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag) /* [한국어] 스텁 시작 */
{
	return -ENODEV; /* [한국어] -ENODEV = MSI-X 테이블이 없다 */
}
#endif /* [한국어] CONFIG_PCI_MSI 분기 종료 */

/* [한국어] === 이 파일의 마지막 절: Configuration Mechanism #1 주소 조립 ===
 *
 * PCI 는 config space 에 접근하는 방법으로 두 가지 "메커니즘"을 정의했고,
 * 살아남은 것은 #1 이다. 원리는 단순하다: x86 I/O 포트 두 개를 쓴다.
 * 0xcf8 (CONFIG_ADDRESS) — 어느 장치의 어느 오프셋인지를 32비트로 적는다
 * 0xcfc (CONFIG_DATA) — 그 자리에 대해 실제 읽기/쓰기를 한다
 * 두 단계로 나뉘어 있으므로 그 사이에 다른 CPU 가 끼어들면 안 된다 —
 * 이것이 이 파일 앞부분의 pci_lock 이 raw 스핀락인 이유다.
 *
 * CONFIG_ADDRESS(0xcf8)의 32비트 레이아웃 (PCI Local Bus Spec r3.0
 * §3.2.2.3.2, Figure 3-2, p.50 — 원문 주석이 밝힌 출처):
 * bit 31 Enable — 1 이어야 이 접근이 config 사이클로 해석된다
 * bit 30:24 예약(0)
 * bit 23:16 Bus Number (8비트)
 * bit 15:11 Device Number (5비트)
 * bit 10:8 Function Number (3비트)
 * bit 7:2 Register Number (6비트 = DWORD 단위 오프셋)
 * bit 1:0 항상 0 (DWORD 정렬)
 * 그래서 아래 SHIFT 값들이 각각 16, 11, 8 이고, REG 마스크가 0xfc
 * (하위 2비트를 잘라 DWORD 정렬하고 상위는 8비트까지만)인 것이다.
 * 이 레이아웃으로는 오프셋을 8비트까지만 표현할 수 있어, 접근 가능한
 * config 공간이 256바이트로 제한된다 — PCIe 의 4KB 확장 공간에 닿으려면
 * ECAM(메모리 매핑 config)이나 아래의 비표준 확장이 필요하다.
 *
 * 누가 쓰는가: 이 매크로들은 레거시 x86 포트 접근뿐 아니라, 같은 주소
 * 형식을 하드웨어 레지스터로 노출하는 여러 SoC 호스트 브리지 드라이버가
 * config 주소를 조립할 때도 쓴다.
 *
 * NVMe 접점: NVMe SSD 의 Vendor ID 를 읽어 장치를 발견하는 그 첫 접근이
 * 결국 이 주소 조립을 거친다. 다만 현대 시스템의 열거는 대부분 ECAM 을
 * 쓰므로, 이 매크로는 레거시 경로와 일부 SoC 에서만 실제로 쓰인다. */
/*
 * Config Address for PCI Configuration Mechanism #1
 *
 * See PCI Local Bus Specification, Revision 3.0,
 * Section 3.2.2.3.2, Figure 3-2, p. 50.
 */

#define PCI_CONF1_BUS_SHIFT	16 /* Bus number */ /* [한국어] Bus Number 는 CONFIG_ADDRESS 의 bit 23:16 이므로 16비트 왼쪽 시프트 */
#define PCI_CONF1_DEV_SHIFT	11 /* Device number */ /* [한국어] Device Number 는 bit 15:11 이므로 11비트 시프트 */
#define PCI_CONF1_FUNC_SHIFT	8  /* Function number */ /* [한국어] Function Number 는 bit 10:8 이므로 8비트 시프트 */

#define PCI_CONF1_BUS_MASK	0xff /* [한국어] 버스 번호는 8비트(0~255) */
#define PCI_CONF1_DEV_MASK	0x1f /* [한국어] device 번호는 5비트(0~31) */
#define PCI_CONF1_FUNC_MASK	0x7 /* [한국어] function 번호는 3비트(0~7) */
#define PCI_CONF1_REG_MASK	0xfc /* Limit aligned offset to a maximum of 256B */ /* [한국어] 레지스터 오프셋 마스크 0xfc = 0b11111100. 하위 2비트를 0 으로 만들어 DWORD 정렬하고, 상위로는 8비트까지만 남겨 접근 범위를 256바이트로 제한한다 */

#define PCI_CONF1_ENABLE	BIT(31) /* [한국어] bit 31 Enable. 이 비트가 0 이면 0xcf8 에 쓴 값이 config 사이클로 해석되지 않는다 */
#define PCI_CONF1_BUS(x)	(((x) & PCI_CONF1_BUS_MASK) << PCI_CONF1_BUS_SHIFT) /* [한국어] 버스 번호를 마스크한 뒤 제자리로 민다. 마스크를 먼저 하는 이유는 범위를 넘는 인자가 이웃 필드를 침범하지 않게 하려는 것 */
#define PCI_CONF1_DEV(x)	(((x) & PCI_CONF1_DEV_MASK) << PCI_CONF1_DEV_SHIFT) /* [한국어] device 번호 조립 */
#define PCI_CONF1_FUNC(x)	(((x) & PCI_CONF1_FUNC_MASK) << PCI_CONF1_FUNC_SHIFT) /* [한국어] function 번호 조립 */
#define PCI_CONF1_REG(x)	((x) & PCI_CONF1_REG_MASK) /* [한국어] 레지스터 오프셋 조립(시프트 없음 — 이미 제자리에 있다) */

#define PCI_CONF1_ADDRESS(bus, dev, func, reg) /* [한국어] 위 조각들을 OR 로 합쳐 완성된 CONFIG_ADDRESS 값을 만든다 */ \
	(PCI_CONF1_ENABLE | /* [한국어] Enable 비트를 반드시 포함 */ \
	 PCI_CONF1_BUS(bus) | /* [한국어] 버스 필드 */ \
	 PCI_CONF1_DEV(dev) | /* [한국어] device 필드 */ \
	 PCI_CONF1_FUNC(func) | /* [한국어] function 필드 */ \
	 PCI_CONF1_REG(reg)) /* [한국어] 레지스터 필드 */

/* [한국어] --- 비표준 확장: CONFIG_ADDRESS 로 4KB 확장 공간에 닿기 ---
 * 원문 주석이 분명히 밝히듯 이것은 표준이 아니다. 위 레이아웃에서 예약된
 * bit 27:24 를 빌려 레지스터 오프셋의 상위 4비트를 실어 보내는 방식으로,
 * ECAM 을 갖추지 못한 여러 ARM SoC 와 AMD Barcelona 계열에서 쓰인다.
 * 이렇게 하면 오프셋이 8비트 + 4비트 = 12비트가 되어 4096바이트 전체를
 * 가리킬 수 있다 — 즉 PCIe 확장 capability(0x100 이상)에 닿을 수 있다.
 * 표준이 아니므로, 이 방식이 통하는지는 호스트 브리지 구현에 달려 있다. */
/*
 * Extension of PCI Config Address for accessing extended PCIe registers
 *
 * No standardized specification, but used on lot of non-ECAM-compliant ARM SoCs
 * or on AMD Barcelona and new CPUs. Reserved bits [27:24] of PCI Config Address
 * are used for specifying additional 4 high bits of PCI Express register.
 */

#define PCI_CONF1_EXT_REG_SHIFT	16 /* [한국어] 확장 오프셋 상위 4비트를 옮길 시프트 양. 오프셋의 bit 11:8 을 주소의 bit 27:24 로 보내야 하므로 16비트 이동이다 */
#define PCI_CONF1_EXT_REG_MASK	0xf00 /* [한국어] 0xf00 = 오프셋의 bit 11:8 만 남기는 마스크(확장 영역을 가리키는 상위 4비트) */
#define PCI_CONF1_EXT_REG(x)	(((x) & PCI_CONF1_EXT_REG_MASK) << PCI_CONF1_EXT_REG_SHIFT) /* [한국어] 마스크한 상위 4비트를 예약 영역(bit 27:24)으로 옮긴다 */

#define PCI_CONF1_EXT_ADDRESS(bus, dev, func, reg) /* [한국어] 표준 주소에 위 확장 필드를 OR 로 덧붙인다 */ \
	(PCI_CONF1_ADDRESS(bus, dev, func, reg) | /* [한국어] 하위 8비트 오프셋까지 포함한 표준 주소 */ \
	 PCI_CONF1_EXT_REG(reg)) /* [한국어] 상위 4비트 오프셋을 예약 영역에 실어 합친다 */

#endif /* DRIVERS_PCI_H */ /* [한국어] 헤더 가드 종료 — 원문 주석이 어느 #ifndef 의 짝인지 밝혀 준다 */
