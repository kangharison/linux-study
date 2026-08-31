/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PLDA PCIe host controller driver
 */

/*
 * [한국어 설명] PLDA XpressRich PCIe 컨트롤러 공용 헤더 (pcie-plda.h)
 *
 * === 파일의 역할 ===
 * PLDA(현재 Rambus 에 인수된 IP 벤더) 의 XpressRich PCIe 컨트롤러 IP 를 쓰는
 * 모든 호스트 드라이버가 공유하는 단 하나의 헤더다. 이 파일에는 (1) 브리지
 * APB 레지스터 맵(GEN_SETTINGS 0x80, IMASK_LOCAL 0x180, ATR 테이블 0x600 및
 * 0x800 등)의 오프셋과 비트 정의, (2) 인터럽트 상태 레지스터 ISTATUS_LOCAL 의
 * 비트를 커널 hwirq 번호로 재배치한 enum plda_int_event, (3) 코어와 SoC 별
 * 드라이버가 주고받는 자료구조(struct plda_pcie_rp, plda_msi, plda_event,
 * plda_event_ops, plda_pcie_host_ops), (4) pcie-plda-host.c 가 EXPORT 하는
 * 함수들의 선언, (5) 레지스터 한두 개만 만지는 짧은 설정 헬퍼들의 static
 * inline 정의가 들어 있다. 이 파일 자체는 컴파일 단위가 아니므로 코드가
 * 실행되는 곳은 이 헤더를 include 하는 세 개의 .c 뿐이다.
 * 이 디렉터리(drivers/pci/controller/plda) 전체는 이 헤더 + 공용 코어 1개 +
 * SoC 드라이버 2개, 도합 네 개의 소스 파일로만 구성된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 PCI 계층은 위에서부터 [PCI 코어(drivers/pci/probe.c, pci.c)] →
 * [호스트 브리지 드라이버(drivers/pci/controller/ 아래)] → [실제 하드웨어]
 * 순으로 쌓인다. 이 헤더는 그 중 "호스트 브리지 드라이버" 층의 내부 규약이며,
 * PCI 코어에는 전혀 노출되지 않는다(include/ 아래가 아니라 드라이버 디렉터리
 * 안에 있는 이유다). PLDA IP 를 쓰는 SoC 드라이버는 자기 전용 구조체의 첫
 * 필드로 struct plda_pcie_rp 를 박아 두고, container_of 로 두 시점을 오간다 --
 * 예: struct mc_pcie(pcie-microchip-host.c), struct starfive_jh7110_pcie
 * (pcie-starfive.c). 이 "임베디드 구조체 상속" 은 drivers/pci/controller/dwc
 * 의 struct dw_pcie 나 cadence/ 의 struct cdns_pcie 와 같은 관용구다.
 * 다만 dwc/ 와 달리 여기서는 코어가 probe 전체를 소유하지 않는다(아래 참조).
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더를 include 하는 파일은 이 트리에서 정확히 세 개다.
 *  (1) pcie-plda-host.c -- 공용 코어. 여기 선언된 일곱 개의 비 inline 함수를
 *      정의하고 EXPORT_SYMBOL_GPL 한다.
 *  (2) pcie-microchip-host.c -- Microchip PolarFire SoC(MPFS). 흥미롭게도 이
 *      드라이버는 plda_pcie_host_init() 을 호출하지 "않는다". 대신 ECAM 공용
 *      코어인 pci_host_common_probe()(drivers/pci/controller/pci-host-common.c)
 *      로 진입하고, ECAM 생성 도중 불리는 pci_ecam_ops.init 훅
 *      (mc_platform_init)에서 plda_pcie_setup_window(), plda_pcie_setup_iomems(),
 *      plda_init_interrupts() 세 개만 라이브러리처럼 골라 쓴다.
 *  (3) pcie-starfive.c -- StarFive JH7110. 이쪽은 반대로 plda_pcie_host_init()
 *      에 제어를 통째로 넘겨 ioremap, 주소 변환 창, 인터럽트, pci_host_probe()
 *      까지 코어가 수행하게 한다.
 * 즉 이 헤더가 정의하는 코어는 "프레임워크(StarFive 경로)" 와 "헬퍼 라이브러리
 * (Microchip 경로)" 두 얼굴을 동시에 갖는다. 데이터 흐름은 항상 SoC 드라이버가
 * struct plda_pcie_rp 를 devm_kzalloc 으로 잡고 dev/num_events/events_bitmap/
 * host_ops/event_ops/event_irq_chip 을 채운 뒤 코어에 넘기는 한 방향이며,
 * 코어는 그 구조체에 bridge_addr, config_base, irq 도메인 포인터들을 되채워
 * 넣는다. 인터럽트 방향으로는 [SoC GIC/PLIC] → 코어의 chained handler
 * (plda_handle_event/intx/msi) → 코어가 만든 irq_domain → 개별 장치 드라이버
 * 순으로 흐른다. NVMe 와의 관계: 이 디렉터리 네 파일에는 nvme 라는 식별자가
 * 단 한 건도 없다. JH7110/PolarFire 슬롯에 NVMe SSD 를 꽂으면 그 SSD 는 이
 * 브리지 아래에 열거되고 SSD 의 MSI 는 여기서 만든 부모 MSI 도메인을 거치지만,
 * 그것은 버스 토폴로지상의 관계일 뿐 코드 호출 관계가 아니다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct plda_pcie_rp : 컨트롤러 인스턴스 하나를 표현하는 중심 구조체.
 *    SoC 드라이버 전용 구조체의 첫 필드로 임베드되어 container_of 로 승격된다.
 *  - struct plda_msi : MSI 벡터 비트맵과 MSI 부모 도메인, 벡터 주소를 담는다.
 *  - struct plda_event : SoC 가 코어에 알려 주는 "INTx/MSI 가 몇 번 이벤트
 *    hwirq 인가" 와 이벤트 IRQ 요청 방식(콜백)의 조합.
 *  - struct plda_event_ops.get_events : ISTATUS 레지스터들을 읽어 이벤트
 *    비트맵을 만드는 SoC 훅. 코어 기본값은 plda_get_events().
 *  - struct plda_pcie_host_ops : plda_pcie_host_init() 안에서만 불리는
 *    SoC 전원/클럭/PHY 초기화 및 해제 훅. Microchip 은 쓰지 않는다.
 *  - enum plda_int_event : ISTATUS_LOCAL 의 상위 비트들을 INTx 4비트를 하나로
 *    압축해 재번호 매긴 이벤트 번호. 실제 hwirq 는 여기에 SoC 별 오프셋
 *    (PLDA_NUM_DMA_EVENTS 또는 NUM_MC_EVENTS)을 더한 값이다.
 *  - plda_pcie_setup_window() / plda_pcie_setup_iomems() : AXI -> PCIe 방향
 *    (아웃바운드) 주소 변환 테이블(ATR) 프로그래밍.
 *  - plda_init_interrupts() : 이벤트/INTx/MSI 세 개의 irq_domain 을 만들고
 *    체인 핸들러를 꽂는 인터럽트 초기화 전체.
 *  - plda_pcie_host_init() / plda_pcie_host_deinit() : StarFive 전용 진입점.
 */

#ifndef _PCIE_PLDA_H
#define _PCIE_PLDA_H

/* Number of MSI IRQs */
/* [한국어] PLDA IP 가 하드웨어적으로 지원하는 MSI 벡터의 최대 개수(32). ISTATUS_MSI/IMSI_ADDR
 * 레지스터가 32비트 한 워드로 벡터 상태를 표현하기 때문에 32 가 상한이다.
 * struct plda_msi 의 used 비트맵 크기와 plda_set_default_msi() 의 기본 벡터 수로 쓰인다.
 * Microchip 은 이 기본값을 쓰지 않고 FPGA 비트파일이 알려 주는 값으로 덮어쓴다
 * (mc_host_probe 의 PCIE_PCI_IRQ_DW0 읽기 참조). */
#define PLDA_MAX_NUM_MSI_IRQS			32

/* PCIe Bridge Phy Regs */
/* [한국어] 브리지 APB 창 기준 오프셋 0x80 -- 루트 포트 전역 설정 레지스터.
 * 읽는 자/쓰는 자: plda_pcie_enable_root_port() 뿐이다. */
#define GEN_SETTINGS				0x80
/* [한국어] GEN_SETTINGS 의 bit0. 1 로 쓰면 이 PLDA 브리지가 Root Port 로 동작한다
 * (0 이면 Endpoint 로 동작하는 IP 구성). StarFive host_init 이 세운다. */
#define  RP_ENABLE				1
/* [한국어] 오프셋 0x9c -- PCI Configuration Space 의 Class Code/Revision ID DWORD(구성 공간
 * 오프셋 0x08 에 해당하는 값)를 브리지 쪽에서 직접 덮어쓰기 위한 창. */
#define PCIE_PCI_IDS_DW1			0x9c
/* [한국어] PCIE_PCI_IDS_DW1 안에서 Class Code 필드가 시작하는 비트 위치(16).
 * PCI 규격상 0x08 DWORD 는 [31:8]=Class Code(base/sub/prog-if), [7:0]=Revision ID 다. */
#define  IDS_CLASS_CODE_SHIFT			16
/* [한국어] 하위 8비트만 남기는 마스크 -- Revision ID 는 하드웨어가 정한 값이므로 보존한다.
 * plda_pcie_set_standard_class() 가 read-modify-write 할 때 쓴다. */
#define  REVISION_ID_MASK			GENMASK(7, 0)
/* [한국어] Class Code 필드(비트 31:8) 마스크. 이 트리의 어떤 .c 도 참조하지 않는 문서용
 * 정의다(플래그 계산은 REVISION_ID_MASK 로만 이루어진다). */
#define  CLASS_CODE_ID_MASK			GENMASK(31, 8)
/* [한국어] 오프셋 0xa8 -- MSI/MSI-X 능력(capability)에 대한 브리지 측 설정 레지스터.
 * Microchip 의 mc_host_probe() 만 읽고 쓴다. */
#define PCIE_PCI_IRQ_DW0			0xa8
/* [한국어] PCIE_PCI_IRQ_DW0 의 bit31. 1 이면 이 루트 포트가 MSI-X 능력을 광고한다.
 * Microchip 은 MSI 를 쓰기 위해 이 비트를 지워 MSI-X 를 감춘다. */
#define  MSIX_CAP_MASK				BIT(31)
/* [한국어] 비트 6:4 -- 이 브리지가 지원하는 MSI 메시지 수의 로그값(0..5).
 * FPGA 비트파일이 굽는 값이므로 보드마다 다르다. mc_host_probe 가 읽어
 * 1 << val 을 msi.num_vectors 로 삼는다. */
#define  NUM_MSI_MSGS_MASK			GENMASK(6, 4)
/* [한국어] 위 NUM_MSI_MSGS_MASK 필드의 시작 비트(4). 마스크 후 이만큼 오른쪽으로 민다. */
#define  NUM_MSI_MSGS_SHIFT			4
/* [한국어] 오프셋 0xb4 -- 물리 함수(PF) 관련 기타 제어 레지스터. */
#define PCI_MISC				0xb4
/* [한국어] PCI_MISC 의 bit15. 1 로 쓰면 현재 선택된 물리 함수를 비활성화한다.
 * StarFive 는 STG syscon 으로 대상 PF 를 고른 뒤 이 비트를 세워 PF1..PF3 을 끈다. */
#define  PHY_FUNCTION_DIS			BIT(15)
/* [한국어] 오프셋 0xfc -- 윈도우/ROM 관련 제어 레지스터. */
#define PCIE_WINROM				0xfc
/* [한국어] PCIE_WINROM 의 bit3. 1 이면 prefetchable memory window 가 64비트 주소를
 * 지원한다. JH7110 은 64비트 ATU 변환을 쓰기 위해 이 비트를 세운다. */
#define  PREF_MEM_WIN_64_SUPPORT		BIT(3)

/* [한국어] 오프셋 0x180 -- Local Interrupt Mask. 비트 = 1 이면 해당 이벤트가 CPU 로 올라온다.
 * 아래 정의된 모든 xxx_MASK 비트 위치가 이 레지스터와 ISTATUS_LOCAL(0x184) 에
 * 똑같이 적용된다. 마스크/언마스크는 read-modify-write 이므로 port->lock 이 필요하다. */
#define IMASK_LOCAL				0x180
/* [한국어] DMA 엔진 0 완료 -- 값이 0x00000000 이다. PLDA IP 는 비트 0..7 을 '벤더가 구현할
 * DMA 완료' 용으로 예약만 해 두었고 이 코어는 구현하지 않았다는 뜻이다.
 * 결과적으로 이 마스크를 쓰는 Microchip 의 이벤트 매핑 항목은 항상 0 을 돌려주어
 * 실제로는 절대 발생하지 않는 이벤트가 된다. */
#define  DMA_END_ENGINE_0_MASK			0x00000000u
/* [한국어] DMA 엔진 0 완료 이벤트의 비트 위치(0). 마스크가 0 이라 실제로는 쓰이지 않는
 * 문서용 값이다. */
#define  DMA_END_ENGINE_0_SHIFT			0
/* [한국어] DMA 엔진 1 완료 -- 위와 같은 이유로 0x00000000(미구현). */
#define  DMA_END_ENGINE_1_MASK			0x00000000u
/* [한국어] DMA 엔진 1 완료 이벤트의 비트 위치(1). 문서용. */
#define  DMA_END_ENGINE_1_SHIFT			1
/* [한국어] DMA 엔진 0 오류 -- bit8. 이쪽은 실제로 구현되어 있다. */
#define  DMA_ERROR_ENGINE_0_MASK		0x00000100u
/* [한국어] DMA 엔진 0 오류의 비트 위치(8). 문서용(코드는 마스크만 쓴다). */
#define  DMA_ERROR_ENGINE_0_SHIFT		8
/* [한국어] DMA 엔진 1 오류 -- bit9. */
#define  DMA_ERROR_ENGINE_1_MASK		0x00000200u
/* [한국어] DMA 엔진 1 오류의 비트 위치(9). 문서용. */
#define  DMA_ERROR_ENGINE_1_SHIFT		9
/* [한국어] A_ATR = AXI 쪽 주소 변환(ATR) 창에서 발생한 Posted 요청(쓰기) 오류 -- bit16.
 * CPU 가 PCIe 로 내보낸 쓰기가 변환 단계에서 실패했음을 뜻한다. */
#define  A_ATR_EVT_POST_ERR_MASK		0x00010000u
/* [한국어] AXI post 오류의 비트 위치(16). 이 값이 곧 enum plda_int_event 의 0번(PLDA_AXI_POST_ERR)에
 * PLDA_NUM_DMA_EVENTS(16)를 더한 값과 같다 -- 두 번호 체계가 여기서 맞물린다. */
#define  A_ATR_EVT_POST_ERR_SHIFT		16
/* [한국어] AXI 쪽 Non-posted 요청(읽기) 오류 -- bit17. */
#define  A_ATR_EVT_FETCH_ERR_MASK		0x00020000u
/* [한국어] AXI fetch 오류의 비트 위치(17). 문서용. */
#define  A_ATR_EVT_FETCH_ERR_SHIFT		17
/* [한국어] AXI 쪽 요청이 버려졌을 때(주로 읽기 타임아웃) -- bit18. */
#define  A_ATR_EVT_DISCARD_ERR_MASK		0x00040000u
/* [한국어] AXI discard 오류의 비트 위치(18). 문서용. */
#define  A_ATR_EVT_DISCARD_ERR_SHIFT		18
/* [한국어] AXI doorbell -- 값이 0x00000000. IP 가 예약만 하고 구현하지 않은 이벤트다.
 * StarFive 는 아예 events_bitmap 에서 이 비트를 빼 버린다. */
#define  A_ATR_EVT_DOORBELL_MASK		0x00000000u
/* [한국어] AXI doorbell 의 비트 위치(19). 마스크가 0 이므로 문서용. */
#define  A_ATR_EVT_DOORBELL_SHIFT		19
/* [한국어] P_ATR = PCIe 쪽 주소 변환 창에서 발생한 Posted 요청(쓰기) 오류 -- bit20.
 * 외부 장치가 DMA 로 보낸 쓰기가 인바운드 변환에서 실패한 경우다. */
#define  P_ATR_EVT_POST_ERR_MASK		0x00100000u
/* [한국어] PCIe post 오류의 비트 위치(20). 문서용. */
#define  P_ATR_EVT_POST_ERR_SHIFT		20
/* [한국어] PCIe 쪽 Non-posted 요청(읽기) 오류 -- bit21. */
#define  P_ATR_EVT_FETCH_ERR_MASK		0x00200000u
/* [한국어] PCIe fetch 오류의 비트 위치(21). 문서용. */
#define  P_ATR_EVT_FETCH_ERR_SHIFT		21
/* [한국어] PCIe 쪽 요청 폐기(읽기 타임아웃) -- bit22. */
#define  P_ATR_EVT_DISCARD_ERR_MASK		0x00400000u
/* [한국어] PCIe discard 오류의 비트 위치(22). 문서용. */
#define  P_ATR_EVT_DISCARD_ERR_SHIFT		22
/* [한국어] PCIe doorbell -- 미구현이라 0x00000000. */
#define  P_ATR_EVT_DOORBELL_MASK		0x00000000u
/* [한국어] PCIe doorbell 의 비트 위치(23). plda_get_events() 가 GENMASK(23,0) 를 만들 때
 * '레지스터 비트와 이벤트 번호가 1:1 로 같은 마지막 비트' 경계로 실제로 사용한다. */
#define  P_ATR_EVT_DOORBELL_SHIFT		23
/* [한국어] INTA 가상 와이어 인터럽트 -- bit24. */
#define  PM_MSI_INT_INTA_MASK			0x01000000u
/* [한국어] INTA 의 비트 위치(24). 개별 INTA..INTD 마스크/시프트는 이 트리의 어떤 .c 도
 * 참조하지 않는다. 코드는 항상 아래 PM_MSI_INT_INTX_MASK 로 네 개를 한꺼번에 다룬다. */
#define  PM_MSI_INT_INTA_SHIFT			24
/* [한국어] INTB -- bit25. */
#define  PM_MSI_INT_INTB_MASK			0x02000000u
/* [한국어] INTB 의 비트 위치(25). 문서용. */
#define  PM_MSI_INT_INTB_SHIFT			25
/* [한국어] INTC -- bit26. */
#define  PM_MSI_INT_INTC_MASK			0x04000000u
/* [한국어] INTC 의 비트 위치(26). 문서용. */
#define  PM_MSI_INT_INTC_SHIFT			26
/* [한국어] INTD -- bit27. */
#define  PM_MSI_INT_INTD_MASK			0x08000000u
/* [한국어] INTD 의 비트 위치(27). 문서용. */
#define  PM_MSI_INT_INTD_SHIFT			27
/* [한국어] INTA~INTD 네 비트를 한꺼번에 가리키는 마스크(비트 27:24).
 * plda_handle_intx() 가 이 마스크로 상태를 잘라 낸 뒤 비트별로 INTx 도메인에
 * 분배하고, plda_hwirq_to_mask() 는 압축된 INTx 이벤트 하나를 다시 이 4비트로 편다. */
#define  PM_MSI_INT_INTX_MASK			0x0f000000u
/* [한국어] INTx 묶음의 시작 비트(24). PLDA hwirq 번호 체계에서도 INTx 이벤트가 24 번이라
 * (EVENT_PM_MSI_INT_INTX = 16 + 8 = 24) 시프트 값과 이벤트 번호가 우연히 일치한다. */
#define  PM_MSI_INT_INTX_SHIFT			24
/* [한국어] MSI 수신 -- bit28. 이 비트가 서면 ISTATUS_MSI 에 어떤 벡터가 왔는지 들어 있다. */
#define  PM_MSI_INT_MSI_MASK			0x10000000u
/* [한국어] MSI 비트의 위치(28). plda_get_events() 가 비트 31:28 을 25 로 접어 넣을 때
 * 기준점으로 쓴다. */
#define  PM_MSI_INT_MSI_SHIFT			28
/* [한국어] AER(Advanced Error Reporting) 이벤트 -- bit29. */
#define  PM_MSI_INT_AER_EVT_MASK		0x20000000u
/* [한국어] AER 이벤트의 비트 위치(29). 문서용. */
#define  PM_MSI_INT_AER_EVT_SHIFT		29
/* [한국어] PM/LTR/Hotplug 등 기타 이벤트 -- bit30. */
#define  PM_MSI_INT_EVENTS_MASK			0x40000000u
/* [한국어] 기타 이벤트의 비트 위치(30). 문서용. */
#define  PM_MSI_INT_EVENTS_SHIFT		30
/* [한국어] System Error -- bit31. */
#define  PM_MSI_INT_SYS_ERR_MASK		0x80000000u
/* [한국어] System Error 의 비트 위치(31). 문서용. */
#define  PM_MSI_INT_SYS_ERR_SHIFT		31
/* [한국어] 비트 31:28 = MSI + AER + 기타 + SYS_ERR 네 개를 한 번에 잘라 내는 마스크.
 * plda_get_events() 가 이 4비트를 25~28 번 이벤트로 옮겨 붙일 때 쓴다. */
#define  SYS_AND_MSI_MASK			GENMASK(31, 28)
/* [한국어] ISTATUS_LOCAL 의 '로컬 이벤트' 개수(15)라고 적혀 있으나, 이 트리의 어떤 .c 도
 * 참조하지 않는다. 실제 개수 계산은 PLDA_INT_EVENT_NUM(13) 과
 * PLDA_MAX_EVENT_NUM(29) 이 담당한다. */
#define  NUM_LOCAL_EVENTS			15
/* [한국어] 오프셋 0x184 -- Local Interrupt Status. 비트 배치는 IMASK_LOCAL 과 동일하고,
 * write-1-to-clear 방식이다(핸들러들이 해당 비트를 그대로 되써서 ack 한다). */
#define ISTATUS_LOCAL				0x184
/* [한국어] 오프셋 0x188 -- Host Interrupt Mask. 이 트리에서는 Microchip 의
 * mc_disable_interrupts() 가 0 을 써서 전부 막는 용도로만 쓴다. */
#define IMASK_HOST				0x188
/* [한국어] 오프셋 0x18c -- Host Interrupt Status. 역시 Microchip 이 초기화 시
 * GENMASK(31,0) 를 써서 잔여 상태를 모두 지우는 데만 쓴다. */
#define ISTATUS_HOST				0x18c
/* [한국어] 오프셋 0x190 -- MSI 벡터 수신 주소 레지스터. 장치가 MSI 를 쓸 때 목적지로 삼는
 * 주소다. 기본값으로는 이 오프셋 값 자체(0x190)가 plda_set_default_msi() 에서
 * msi.vector_phy 로 들어가고, Microchip 은 이 레지스터를 실제로 읽어 값을 얻는다. */
#define IMSI_ADDR				0x190
/* [한국어] 오프셋 0x194 -- MSI Interrupt Status. 비트 n = 벡터 n 도착. write-1-to-clear. */
#define ISTATUS_MSI				0x194
/* [한국어] 오프셋 0x3f0 -- PCIe Message Reception 설정. 어떤 PCIe 메시지를 받아들이고
 * 전달할지 고른다. */
#define PMSG_SUPPORT_RX				0x3f0
/* [한국어] PMSG_SUPPORT_RX 의 bit2 -- LTR(Latency Tolerance Reporting) 메시지 수신/전달.
 * JH7110 은 전달 주소가 초기화되지 않은 채 켜져 있어 커널이 멈추므로 반드시 끈다
 * (plda_pcie_disable_ltr 의 주석 참조). */
#define  PMSG_LTR_SUPPORT			BIT(2)

/* PCIe Master table init defines */
/* [한국어] 아웃바운드가 아니라 '인바운드'(PCIe -> AXI) 변환 테이블 0번의 소스 주소/파라미터
 * 레지스터, 오프셋 0x600. PLDA 용어로 PCIe Master table 이다. 창 하나가
 * ATR_ENTRY_SIZE(32바이트) 씩 떨어져 있다. */
#define ATR0_PCIE_WIN0_SRCADDR_PARAM		0x600u
/* [한국어] 인바운드 창 크기 필드에 넣을 값 0x25(=37). 크기 = 2 의 (37+1) 승 = 2^38 = 256GiB.
 * plda_pcie_setup_inbound_address_translation() 이 이 상수를 그대로 OR 한다. */
#define  ATR0_PCIE_ATR_SIZE			0x25
/* [한국어] 위 크기 필드의 시작 비트(1). ATR_SIZE_MASK(비트 6:1)와 같은 자리를 가리킨다. */
#define  ATR0_PCIE_ATR_SIZE_SHIFT		1
/* [한국어] 오프셋 0x604 -- 인바운드 소스(PCIe) 주소의 상위 32비트. 하위 32비트는
 * 0x600 레지스터의 상위쪽에 들어간다(하위 12비트는 크기/enable 필드가 쓴다). */
#define ATR0_PCIE_WIN0_SRC_ADDR			0x604u
/* [한국어] 오프셋 0x608 -- 변환 결과(AXI) 주소의 하위 32비트. */
#define ATR0_PCIE_WIN0_TRSL_ADDR_LSB		0x608u
/* [한국어] 오프셋 0x60c -- 변환 결과(AXI) 주소의 상위 32비트. UDW = Upper DWord. */
#define ATR0_PCIE_WIN0_TRSL_ADDR_UDW		0x60cu
/* [한국어] 오프셋 0x610 -- 변환 결과를 어느 AXI 마스터로 보낼지 등 파라미터.
 * Microchip 의 mc_pcie_setup_inbound_atr() 이 TRSL_ID_AXI4_MASTER_0 을 쓴다. */
#define ATR0_PCIE_WIN0_TRSL_PARAM		0x610u

/* PCIe AXI slave table init defines */
/* [한국어] 아웃바운드(AXI -> PCIe) 변환 테이블 0번의 소스 주소/파라미터, 오프셋 0x800.
 * PLDA 용어로 AXI slave table. 이 레지스터 하나에 소스 주소 하위 32비트와
 * 크기 필드, enable 비트가 함께 들어간다. */
#define ATR0_AXI4_SLV0_SRCADDR_PARAM		0x800u
/* [한국어] 비트 6:1 -- 변환 창 크기. 값 n 은 2 의 (n+1) 승 바이트를 뜻한다
 * (plda_pcie_setup_window 의 ilog2(size) - 1 계산이 그 역이다). */
#define  ATR_SIZE_MASK				GENMASK(6, 1)
/* [한국어] bit0 -- 이 변환 창을 실제로 동작시키는 enable 비트.
 * 소스 주소를 4KiB 로 정렬해 하위 12비트를 비워 두기 때문에 크기/enable 필드를
 * 같은 워드에 겹쳐 넣을 수 있다. */
#define  ATR_IMPL_ENABLE			BIT(0)
/* [한국어] 오프셋 0x804 -- 소스(AXI) 주소의 상위 32비트. */
#define ATR0_AXI4_SLV0_SRC_ADDR			0x804u
/* [한국어] 오프셋 0x808 -- 변환 결과(PCIe) 주소의 하위 32비트. */
#define ATR0_AXI4_SLV0_TRSL_ADDR_LSB		0x808u
/* [한국어] 오프셋 0x80c -- 변환 결과(PCIe) 주소의 상위 32비트. */
#define ATR0_AXI4_SLV0_TRSL_ADDR_UDW		0x80cu
/* [한국어] 오프셋 0x810 -- 이 창으로 나가는 트랜잭션이 PCIe 쪽의 어느 인터페이스로
 * 향할지 고르는 파라미터 레지스터. */
#define ATR0_AXI4_SLV0_TRSL_PARAM		0x810u
/* [한국어] TRSL_PARAM 값 0 -- 일반 TX/RX 인터페이스, 즉 메모리 트랜잭션용.
 * plda_pcie_setup_window() 가 index 가 0 이 아닌 모든 창에 쓴다. */
#define  PCIE_TX_RX_INTERFACE			0x00000000u
/* [한국어] TRSL_PARAM 값 1 -- Configuration 인터페이스. 0번 창은 항상 config space 접근용
 * 이므로 이 값을 쓴다. 이 덕분에 CPU 가 config 창 주소를 읽고 쓰면 브리지가
 * PCIe Configuration Request TLP 를 만들어 낸다. */
#define  PCIE_CONFIG_INTERFACE			0x00000001u
/* [한국어] 인바운드 TRSL_PARAM 값 4 -- 변환 목적지를 AXI4 마스터 포트 0 으로 지정.
 * EP 장치가 보낸 DMA 가 이 포트를 통해 시스템 메모리로 들어간다. */
#define  TRSL_ID_AXI4_MASTER_0			0x00000004u

/* [한국어] 브리지 APB 창 안에서 '브리지 자신의 PCI Configuration Space' 가 시작하는 오프셋
 * 0x1000. plda_pcie_write_rc_bar() 가 여기에 PCI_BASE_ADDRESS_0/1 을 더해
 * 루트 포트 자신의 BAR 을 쓴다. */
#define CONFIG_SPACE_ADDR_OFFSET		0x1000u

/* [한국어] ATR 테이블 한 항목(창 하나)의 크기 32바이트. 창 index 번째 레지스터 묶음은
 * 기준 오프셋 + index * 32 에 있다. 아웃바운드/인바운드 모두 같은 간격이다. */
#define ATR_ENTRY_SIZE				32

/*
 * [한국어]
 * enum plda_int_event - ISTATUS_LOCAL 레지스터 비트를 커널 이벤트 번호로 재배치한 목록
 *
 * 왜 필요한가: ISTATUS_LOCAL(0x184) 한 레지스터에 DMA, 주소 변환 오류, INTx,
 * MSI, AER, 시스템 오류가 전부 섞여 있다. 커널 쪽에서는 이들을 irq_domain 의
 * hwirq 로 다뤄야 하는데, INTA~INTD 네 비트는 커널이 이미 별도의 INTx 도메인을
 * 갖고 있으므로 이벤트 층에서는 하나로 합치는 편이 낫다. 그래서 레지스터 비트를
 * 그대로 쓰지 않고 이 열거형으로 한 번 접는다.
 *
 * 번호 대응(레지스터 비트 -> 이 enum 값):
 *   비트 16~23 -> 0~7   (그대로 16 을 뺀 값)
 *   비트 27~24 -> 8     (INTx 네 개를 하나로 압축)
 *   비트 28~31 -> 9~12  (압축 때문에 3 칸씩 앞당겨짐)
 * 실제 irq_domain 의 hwirq 는 여기에 SoC 별 앞 이벤트 개수를 더한 값이다:
 *   StarFive : + PLDA_NUM_DMA_EVENTS(16)  -> hwirq 16..28
 *   Microchip: + NUM_MC_EVENTS(15)        -> hwirq 15..27
 * 변환 함수는 plda_hwirq_to_mask()(pcie-plda-host.c) 이며, 그 역방향(레지스터 ->
 * 이벤트 비트맵)은 plda_get_events() 다. Microchip 은 이 두 함수를 쓰지 않고
 * mc_get_events() 와 event_descs[] 표로 같은 일을 직접 한다.
 *
 * 실행 컨텍스트: 컴파일 타임 상수 목록이므로 실행 컨텍스트가 없다.
 * 동기화: 상수이므로 필요 없다.
 */
enum plda_int_event {
	/* [한국어] 값 0 -- AXI 쪽 ATR 창에서 posted(쓰기) 요청이 실패.
	 * 설정자: 컴파일 타임 상수(암시적 0). 읽는 자: plda_get_events() 가 만드는 이벤트
	 * 비트맵의 비트 번호로, 그리고 Microchip 의 EVENT_LOCAL_A_ATR_EVT_POST_ERR 계산에
	 * 쓰인다. 값 범위: 0. 동기화: 상수이므로 없음.
	 * 대응 레지스터 비트는 A_ATR_EVT_POST_ERR_SHIFT(16) 이고,
	 * 0 + PLDA_NUM_DMA_EVENTS(16) = 16 이라 두 체계가 일치한다. */
	PLDA_AXI_POST_ERR,
	/* [한국어] 값 1 -- AXI 쪽 non-posted(읽기) 요청 실패. 레지스터 비트 17. */
	PLDA_AXI_FETCH_ERR,
	/* [한국어] 값 2 -- AXI 쪽 요청 폐기(읽기 타임아웃). 레지스터 비트 18. */
	PLDA_AXI_DISCARD_ERR,
	/* [한국어] 값 3 -- AXI doorbell. 대응 마스크 A_ATR_EVT_DOORBELL_MASK 가 0 이라 실제로는
	 * 결코 보고되지 않는다. StarFive 는 events_bitmap 에서 이 비트를 명시적으로 뺀다. */
	PLDA_AXI_DOORBELL,
	/* [한국어] 값 4 -- PCIe 쪽 posted(쓰기) 요청 실패. 레지스터 비트 20. */
	PLDA_PCIE_POST_ERR,
	/* [한국어] 값 5 -- PCIe 쪽 non-posted(읽기) 요청 실패. 레지스터 비트 21. */
	PLDA_PCIE_FETCH_ERR,
	/* [한국어] 값 6 -- PCIe 쪽 요청 폐기. 레지스터 비트 22. */
	PLDA_PCIE_DISCARD_ERR,
	/* [한국어] 값 7 -- PCIe doorbell. 마스크가 0 이라 미구현이며 StarFive 가 제외한다. */
	PLDA_PCIE_DOORBELL,
	/* [한국어] 값 8 -- INTx. 여기서 번호 체계가 레지스터와 어긋나기 시작한다.
	 * 레지스터에서는 INTA..INTD 가 비트 27:24 네 칸을 차지하지만 이벤트 번호로는
	 * 한 칸으로 압축된다. plda_get_events() 가 압축하고 plda_hwirq_to_mask() 가 편다. */
	PLDA_INTX,
	/* [한국어] 값 9 -- MSI 수신(레지스터 비트 28). 압축 때문에 번호가 3 칸 앞당겨져 있다. */
	PLDA_MSI,
	/* [한국어] 값 10 -- AER 이벤트(레지스터 비트 29). */
	PLDA_AER_EVENT,
	/* [한국어] 값 11 -- PM/LTR/Hotplug 등 기타 이벤트(레지스터 비트 30). */
	PLDA_MISC_EVENTS,
	/* [한국어] 값 12 -- System Error(레지스터 비트 31). */
	PLDA_SYS_ERR,
	/* [한국어] 값 13 -- 열거형의 개수를 세기 위한 sentinel. 실제 이벤트가 아니다.
	 * 읽는 자: StarFive 가 GENMASK(PLDA_INT_EVENT_NUM - 1, 0) 으로 13비트 마스크를
	 * 만들 때, 그리고 PLDA_MAX_EVENT_NUM 정의에 쓰인다. */
	PLDA_INT_EVENT_NUM
};

/* [한국어] DMA 관련 이벤트가 차지하는 레지스터 비트 수(16 = DMA end 8개 + DMA error 8개).
 * enum plda_int_event 는 이 16개를 빼고 번호를 매기므로, 실제 hwirq 로 바꿀 때
 * 항상 이 값을 더해야 한다. Microchip 은 자기 전용 이벤트가 앞에 15개 있어서
 * 이 상수 대신 NUM_MC_EVENTS(15) 를 더한다 -- 즉 오프셋이 SoC 마다 다르다. */
#define PLDA_NUM_DMA_EVENTS			16

/* [한국어] INTx 이벤트의 실제 hwirq 번호 = 16 + 8 = 24. StarFive 가 struct plda_event 의
 * intx_event 로 코어에 알려 주고, plda_hwirq_to_mask() 가 이 값과 비교해
 * INTx 만 4비트로 펼치는 특수 처리를 한다. */
#define EVENT_PM_MSI_INT_INTX			(PLDA_NUM_DMA_EVENTS + PLDA_INTX)
/* [한국어] MSI 이벤트의 실제 hwirq 번호 = 16 + 9 = 25. StarFive 의 msi_event 값. */
#define EVENT_PM_MSI_INT_MSI			(PLDA_NUM_DMA_EVENTS + PLDA_MSI)
/* [한국어] StarFive 계열이 쓰는 전체 이벤트 개수 = 16 + 13 = 29 (hwirq 0..28).
 * plda_pcie_init_irq_domains() 가 이 크기로 event_domain 을 만든다. */
#define PLDA_MAX_EVENT_NUM			(PLDA_NUM_DMA_EVENTS + PLDA_INT_EVENT_NUM)

/*
 * PLDA interrupt register
 *
 * 31         27     23              15           7          0
 * +--+--+--+-+------+-+-+-+-+-+-+-+-+-----------+-----------+
 * |12|11|10|9| intx |7|6|5|4|3|2|1|0| DMA error | DMA end   |
 * +--+--+--+-+------+-+-+-+-+-+-+-+-+-----------+-----------+
 * event  bit
 * 0-7   (0-7)   DMA interrupt end : reserved for vendor implement
 * 8-15  (8-15)  DMA error : reserved for vendor implement
 * 16    (16)    AXI post error (PLDA_AXI_POST_ERR)
 * 17    (17)    AXI fetch error (PLDA_AXI_FETCH_ERR)
 * 18    (18)    AXI discard error (PLDA_AXI_DISCARD_ERR)
 * 19    (19)    AXI doorbell (PLDA_PCIE_DOORBELL)
 * 20    (20)    PCIe post error (PLDA_PCIE_POST_ERR)
 * 21    (21)    PCIe fetch error (PLDA_PCIE_FETCH_ERR)
 * 22    (22)    PCIe discard error (PLDA_PCIE_DISCARD_ERR)
 * 23    (23)    PCIe doorbell (PLDA_PCIE_DOORBELL)
 * 24    (27-24) INTx interruts (PLDA_INTX)
 * 25    (28):   MSI interrupt (PLDA_MSI)
 * 26    (29):   AER event (PLDA_AER_EVENT)
 * 27    (30):   PM/LTR/Hotplug (PLDA_MISC_EVENTS)
 * 28    (31):   System error (PLDA_SYS_ERR)
 */

/*
 * [한국어] 위 영문 다이어그램 보충 설명
 *
 * 다이어그램 두 번째 줄의 |12|11|10|9| 와 |7|6|5|4|3|2|1|0| 은 레지스터 비트가
 * 아니라 enum plda_int_event 의 값이다. 즉 레지스터 비트 31 이 enum 12
 * (PLDA_SYS_ERR), 비트 30 이 enum 11, 비트 29 가 enum 10, 비트 28 이 enum 9,
 * 비트 27:24 네 칸이 통째로 enum 8(intx), 비트 23:16 이 enum 7..0 이다.
 * 그 아래 표에서 왼쪽 숫자는 "이벤트 번호"(= enum 값 + 16), 괄호 안이 레지스터
 * 비트 번호다.
 *
 * 상류 주석의 오기 한 곳: "19 (19) AXI doorbell (PLDA_PCIE_DOORBELL)" 줄에서
 * 괄호 안 이름은 PLDA_AXI_DOORBELL 이어야 한다. 레지스터 비트 19 는
 * A_ATR_EVT_DOORBELL_SHIFT 이고, 이벤트 19 = 16 + PLDA_AXI_DOORBELL(3) 이다.
 * PLDA_PCIE_DOORBELL(7) 은 그 아래 줄의 이벤트 23 / 비트 23 에 해당한다.
 * 영문 원문은 상류 그대로 두고 여기에 정정만 남긴다.
 *
 * 비트 0~15(DMA end / DMA error 중 end 쪽 8비트와 doorbell 두 비트)는 마스크
 * 상수가 0x00000000 으로 정의되어 있어 이 코어에서는 결코 보고되지 않는다.
 */

/* [한국어] 전방 선언 -- 아래 두 ops 구조체의 콜백이 struct plda_pcie_rp 포인터를 받는데,
 * 정작 struct plda_pcie_rp 자신이 그 ops 포인터를 필드로 갖고 있어 순환 참조가
 * 생긴다. 이 한 줄이 그 고리를 끊어 준다. */
struct plda_pcie_rp;

/*
 * [한국어]
 * struct plda_event_ops - 이벤트 상태 수집 방식을 SoC 가 갈아끼우기 위한 ops 테이블
 *
 * 왜 필요한가: PLDA 기본 구성에서는 이벤트가 ISTATUS_LOCAL 레지스터 하나에 다
 * 모여 있지만, Microchip 은 여기에 컨트롤러 PHY 쪽 레지스터 세 개(PCIE_EVENT_INT,
 * SEC_ERROR_INT, DED_ERROR_INT)의 이벤트를 더 얹었다. 그래서 "상태를 읽어 이벤트
 * 비트맵을 만드는" 단 한 가지 동작만 훅으로 빼 두었다.
 *
 * 설정자: SoC 드라이버 또는 plda_init_interrupts() 의 기본값 대입.
 * 읽는 자: plda_handle_event() (하드 인터럽트 컨텍스트).
 * 동기화: const 테이블이므로 그 자체에 락은 없다.
 *
 * 호출 체인:
 *   SoC 인터럽트 -> plda_handle_event() -> port->event_ops->get_events()
 *                                        -> plda_get_events() 또는 mc_get_events()
 */
struct plda_event_ops {
	/* [한국어] 역할: ISTATUS 레지스터(들)를 읽어 '지금 어떤 이벤트가 대기 중인가' 를
	 * hwirq 비트맵 하나로 만들어 돌려주는 SoC 별 훅.
	 * 설정자: plda_init_interrupts() 가 port->event_ops 가 NULL 이면 코어 기본
	 * plda_event_ops(get_events = plda_get_events) 를 꽂고, Microchip 은
	 * mc_platform_init() 에서 mc_event_ops(get_events = mc_get_events) 로 미리 채운다.
	 * 읽는 자: plda_handle_event() 체인 핸들러가 인터럽트마다 호출한다.
	 * 값 범위: NULL 불가(코어가 기본값을 보장). 반환 비트맵의 유효 폭은
	 * port->num_events 비트.
	 * 동기화: 하드 인터럽트 컨텍스트에서 불리며 락을 쥐지 않는다. 순수 읽기라
	 * 경쟁이 없고, 실제 ack 는 irq_chip 의 irq_ack 콜백이 따로 한다. */
	u32 (*get_events)(struct plda_pcie_rp *pcie);
};

/*
 * [한국어]
 * struct plda_pcie_host_ops - SoC 별 전원/클럭/PHY 초기화 및 해제 훅 묶음
 *
 * 왜 필요한가: PLDA IP 자체는 레지스터 맵만 같을 뿐, 그 IP 를 깨우는 방법
 * (클럭 게이팅 해제, 리셋 디어서트, PHY 초기화, PERST 핀 토글, 링크 대기)은
 * 전적으로 SoC 마다 다르다. plda_pcie_host_init() 은 그 SoC 고유 부분을 이
 * 두 훅으로만 부르고 나머지는 자기가 처리한다.
 *
 * 설정자: SoC 드라이버가 probe 에서 port->host_ops 에 지정.
 * 읽는 자: plda_pcie_host_init() / plda_pcie_host_deinit() 뿐이다.
 * 동기화: probe/remove 프로세스 컨텍스트에서만 불린다.
 *
 * 이 트리에서 이 ops 를 채우는 드라이버는 pcie-starfive.c 하나뿐이다.
 * Microchip 은 plda_pcie_host_init() 자체를 거치지 않고 자기 probe 에서
 * 클럭을 직접 켠다(mc_pcie_init_clks).
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> plda_pcie_host_init() -> host_ops->host_init()
 *                                                   = starfive_pcie_host_init()
 */
struct plda_pcie_host_ops {
	/* [한국어] 역할: 컨트롤러를 실제로 켜는 SoC 초기화 훅(전원, 클럭, 리셋, PHY, 링크 대기).
	 * 설정자: SoC 드라이버가 probe 에서 host_ops 를 통째로 지정한다
	 * (StarFive 의 sf_host_ops.host_init = starfive_pcie_host_init).
	 * 읽는 자: plda_pcie_host_init() 이 ATR 창을 프로그래밍하기 직전에 한 번 호출.
	 * 값 범위: NULL 허용 -- 코어가 host_ops 와 이 필드 둘 다 NULL 검사를 한다.
	 * 반환 0 이 성공, 음수 errno 면 코어가 즉시 그 값을 반환하고 probe 를 접는다.
	 * 동기화: probe 프로세스 컨텍스트에서 한 번만 실행되므로 락 불필요.
	 * Microchip 은 plda_pcie_host_init() 자체를 쓰지 않으므로 이 훅도 쓰지 않는다. */
	int (*host_init)(struct plda_pcie_rp *pcie);
	/* [한국어] 역할: host_init 의 반대 -- PHY 끄기, 클럭/리셋 해제, 레귤레이터 끄기.
	 * 설정자: 위와 같이 SoC 드라이버(StarFive 의 starfive_pcie_host_deinit).
	 * 읽는 자: 두 곳이다. (1) plda_pcie_host_init() 의 err_host / err_probe 에러 경로,
	 * (2) 정상 종료 경로인 plda_pcie_host_deinit().
	 * 값 범위: NULL 허용. 반환값 없음 -- 해제 실패를 보고할 수단이 없다.
	 * 동기화: probe/remove 프로세스 컨텍스트 전용. */
	void (*host_deinit)(struct plda_pcie_rp *pcie);
};

/*
 * [한국어]
 * struct plda_msi - 이 컨트롤러가 관리하는 MSI 벡터 풀 전체의 상태
 *
 * 왜 필요한가: PLDA 브리지는 MSI 를 "주소 하나에 벡터 번호를 데이터로 쓰는"
 * 단순한 방식으로 받는다(IMSI_ADDR 에 쓰면 ISTATUS_MSI 의 해당 비트가 선다).
 * 따라서 커널이 직접 벡터 번호를 할당/회수해야 하고, 그 장부가 이 구조체다.
 *
 * 수명: struct plda_pcie_rp 안에 값으로 박혀 있어 컨트롤러와 수명이 같다.
 * 실행 컨텍스트: 할당/해제는 프로세스 컨텍스트(뮤텍스), 수신 처리는 하드
 * 인터럽트 컨텍스트(락 없음)로 갈린다.
 *
 * 관련 함수: plda_set_default_msi(), plda_allocate_msi_domains(),
 * plda_irq_msi_domain_alloc(), plda_irq_msi_domain_free(),
 * plda_compose_msi_msg(), plda_handle_msi(), plda_msi_bottom_irq_ack().
 */
struct plda_msi {
	/* [한국어] 역할: 아래 used 비트맵을 보호하는 뮤텍스. 상류 영문 주석 그대로다.
	 * 설정자: plda_allocate_msi_domains() 가 mutex_init() 으로 초기화.
	 * 읽는 자: plda_irq_msi_domain_alloc()/free() 가 잠근다.
	 * 값 범위: 초기화된 mutex.
	 * 동기화: 뮤텍스이므로 잠들 수 있는 프로세스 컨텍스트에서만 잡을 수 있다.
	 * MSI 할당/해제는 장치 probe 시점(프로세스 컨텍스트)에만 일어나므로 문제없다.
	 * 인터럽트 경로(plda_handle_msi, plda_msi_bottom_irq_ack)는 이 락을 쓰지 않는다 --
	 * 그쪽은 비트맵이 아니라 하드웨어 상태 레지스터만 만지기 때문이다. */
	struct mutex lock;		/* Protect used bitmap */
	/* [한국어] 역할: 이 컨트롤러의 MSI 부모 irq_domain. 개별 PCI 장치의 MSI 도메인이
	 * 이 도메인을 부모로 삼아 만들어진다.
	 * 설정자: plda_allocate_msi_domains() 가 msi_create_parent_irq_domain() 결과를 저장.
	 * 읽는 자: plda_handle_msi() 가 generic_handle_domain_irq() 로 벡터를 분배할 때,
	 * plda_pcie_irq_domain_deinit() 이 irq_domain_remove() 할 때.
	 * 값 범위: 유효 포인터 또는 NULL(생성 실패 시 -ENOMEM 으로 probe 중단).
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기 전용. */
	struct irq_domain *dev_domain;
	/* [한국어] 역할: 이 컨트롤러가 실제로 제공하는 MSI 벡터 개수.
	 * 설정자: 기본값은 plda_set_default_msi() 의 PLDA_MAX_NUM_MSI_IRQS(32).
	 * Microchip 은 mc_host_probe() 에서 PCIE_PCI_IRQ_DW0 의 NUM_MSI_MSGS 필드를 읽어
	 * 1 << val 로 덮어쓴다(FPGA 비트파일이 정하는 값).
	 * 읽는 자: irq_domain_info.size, plda_irq_msi_domain_alloc() 의 탐색 상한,
	 * plda_handle_msi() 의 for_each_set_bit 상한.
	 * 값 범위: 1 ~ PLDA_MAX_NUM_MSI_IRQS(32).
	 * 동기화: probe 에서 확정된 뒤 읽기 전용. */
	u32 num_vectors;
	/* [한국어] 역할: 장치가 MSI 를 쓸 때 목적지로 삼을 물리 주소.
	 * 설정자: 기본값은 plda_set_default_msi() 가 넣는 IMSI_ADDR(0x190) -- 이는
	 * '레지스터 오프셋을 그대로 주소로 쓴다' 는 뜻이라 실제 버스 주소는 아니지만
	 * JH7110 에서는 이 값이 그대로 통한다. Microchip 은 IMSI_ADDR 레지스터를
	 * readl_relaxed 로 읽어 진짜 주소를 얻는다.
	 * 읽는 자: plda_compose_msi_msg() 가 msi_msg 의 address_lo/hi 로 쪼개 넣고,
	 * Microchip 은 mc_pcie_enable_msi() 에서 config space MSI capability 에도 쓴다.
	 * 값 범위: 64비트 물리 주소.
	 * 동기화: probe 이후 읽기 전용. */
	u64 vector_phy;
	/* [한국어] 역할: 어느 MSI 벡터 번호가 이미 할당됐는지 표시하는 비트맵(최대 32비트).
	 * 설정자: plda_irq_msi_domain_alloc() 이 set_bit, free 가 __clear_bit.
	 * 읽는 자: 같은 두 함수의 find_first_zero_bit / test_bit.
	 * 값 범위: 비트 0..num_vectors-1 만 의미가 있다.
	 * 동기화: 위 lock 뮤텍스가 보호한다. free 쪽이 원자적이지 않은 __clear_bit 을
	 * 쓰는 것도 락 안이기 때문이다. */
	DECLARE_BITMAP(used, PLDA_MAX_NUM_MSI_IRQS);
};

/*
 * [한국어]
 * struct plda_pcie_rp - PLDA XpressRich 루트 포트(Root Port) 인스턴스 하나
 *
 * 이 디렉터리 전체의 중심 자료구조다. SoC 드라이버는 자기 전용 구조체의 첫
 * 필드에 이것을 값으로 박아 두고(struct mc_pcie, struct starfive_jh7110_pcie),
 * 코어에는 이 구조체의 주소만 넘긴다. 코어에서 SoC 쪽으로 돌아올 때는
 * container_of(plda, struct starfive_jh7110_pcie, plda) 로 승격한다.
 * 첫 필드이므로 주소 값 자체는 같지만, 코드는 항상 container_of 를 쓴다.
 *
 * 채우는 쪽/읽는 쪽이 필드마다 다르므로 아래 필드별 주석을 참고하라. 대략
 * 나누면 dev/num_events/events_bitmap/host_ops/event_ops/event_irq_chip 는
 * SoC 드라이버가 채워 넣는 "입력" 이고, bridge/intx_domain/event_domain/
 * msi.dev_domain/irq/msi_irq/intx_irq 는 코어가 되채우는 "출력" 이다.
 * bridge_addr 은 경로에 따라 양쪽 다 될 수 있다.
 *
 * 실행 컨텍스트: 필드 대부분은 probe(프로세스 컨텍스트)에서 확정되고 이후
 * 읽기 전용이다. 인터럽트 컨텍스트에서 변하는 상태는 하드웨어 레지스터 쪽에
 * 있고, 그 접근만 lock 이 보호한다.
 *
 * 동기화 요약: lock(raw_spinlock_t)은 IMASK_LOCAL read-modify-write 전용,
 * msi.lock(mutex)은 MSI used 비트맵 전용이며 서로 겹치지 않는다.
 */
struct plda_pcie_rp {
	/* [한국어] 역할: 이 컨트롤러의 struct device. 모든 dev_err/dev_dbg 와 devm_ 할당의 기준.
	 * 설정자: SoC 드라이버가 probe 맨 앞에서 plda->dev = &pdev->dev 로 채운다
	 * (starfive_pcie_probe, mc_host_probe 둘 다).
	 * 읽는 자: 코어 전반(plda_pcie_host_init, plda_allocate_msi_domains,
	 * plda_pcie_init_irq_domains 등)이 dev_fwnode, of_node, devm_ 계열에 쓴다.
	 * 값 범위: NULL 불가 -- 코어는 검사하지 않고 바로 쓴다.
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기 전용. */
	struct device *dev;
	/* [한국어] 역할: 커널 PCI 코어가 쓰는 호스트 브리지 객체.
	 * 설정자: plda_pcie_host_init() 이 devm_pci_alloc_host_bridge() 결과를 저장한다.
	 * Microchip 경로에서는 채워지지 않는다 -- 그쪽 bridge 는 pci_host_common_probe()
	 * 가 만들고 platform drvdata 로 들고 다니기 때문이다.
	 * 읽는 자: plda_pcie_host_deinit() 이 bridge->bus 로 루트 버스를 떼어낼 때.
	 * 값 범위: 유효 포인터 또는 (Microchip 경로에서) NULL.
	 * 동기화: probe/remove 프로세스 컨텍스트 전용. */
	struct pci_host_bridge *bridge;
	/* [한국어] 역할: INTA~INTD 네 개의 legacy 인터럽트를 나눠 주는 irq_domain.
	 * 설정자: plda_pcie_init_irq_domains() 가 devicetree 의 첫 자식 노드
	 * (pcie-intc)를 fwnode 로 삼아 크기 PCI_NUM_INTX(4) 로 만든다.
	 * 읽는 자: plda_handle_intx() 의 generic_handle_domain_irq(),
	 * plda_pcie_irq_domain_deinit() 의 irq_domain_remove().
	 * 값 범위: 유효 포인터 또는 NULL(생성 실패 시 probe 중단).
	 * 동기화: probe 에서 확정 후 읽기 전용. */
	struct irq_domain *intx_domain;
	/* [한국어] 역할: PLDA 이벤트(주소 변환 오류, DMA 오류, AER, INTx 묶음, MSI 묶음 등)를
	 * 나눠 주는 상위 irq_domain. INTx/MSI 도메인보다 한 단계 위에 있다.
	 * 설정자: plda_pcie_init_irq_domains() 가 크기 port->num_events 로 만든다.
	 * 읽는 자: plda_handle_event() 의 분배, plda_init_interrupts() 의
	 * irq_create_mapping(), Microchip 의 mc_event_handler() 가 hwirq 를 되찾을 때.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: probe 에서 확정 후 읽기 전용. */
	struct irq_domain *event_domain;
	/* [한국어] 역할: IMASK_LOCAL 레지스터의 read-modify-write 를 보호하는 raw 스핀락.
	 * 설정자: plda_pcie_init_irq_domains() 의 raw_spin_lock_init().
	 * 읽는 자: plda_mask_intx_irq/plda_unmask_intx_irq(irqsave 판),
	 * plda_mask_event_irq/plda_unmask_event_irq(그냥 raw_spin_lock),
	 * Microchip 의 mc_mask_event_irq/mc_unmask_event_irq.
	 * 값 범위: 초기화된 raw_spinlock_t.
	 * 동기화: raw_ 접두사는 PREEMPT_RT 에서도 잠들지 않는 진짜 스핀락이라는 뜻이다.
	 * irq_chip 콜백은 인터럽트가 막힌 상태에서 불릴 수 있어 잠들면 안 되기 때문에
	 * 일반 spinlock_t 가 아니라 raw 판을 쓴다. INTx 쪽만 irqsave 를 쓰는 이유는
	 * irq_mask/irq_unmask 가 인터럽트가 열린 프로세스 컨텍스트에서도 불릴 수 있기
	 * 때문이고, 이벤트 쪽은 이미 인터럽트가 막힌 경로에서만 불린다는 가정이다. */
	raw_spinlock_t lock;
	/* [한국어] 역할: MSI 관련 상태 전체(비트맵, 부모 도메인, 벡터 수/주소)를 담은 하위 구조체.
	 * 포인터가 아니라 값으로 박혀 있어 plda_pcie_rp 와 수명이 같다.
	 * 설정자: plda_set_default_msi() 또는 SoC 가 직접(Microchip),
	 * 그리고 plda_allocate_msi_domains() 가 lock 과 dev_domain 을 채운다.
	 * 읽는 자: MSI 경로 전체.
	 * 값 범위: struct plda_msi 참조.
	 * 동기화: 내부 lock 필드가 used 비트맵만 보호한다. 나머지 필드는 probe 이후 상수. */
	struct plda_msi msi;
	/* [한국어] 역할: get_events 훅을 담은 ops 테이블 포인터.
	 * 설정자: Microchip 은 mc_platform_init() 에서 미리 채우고, 비어 있으면
	 * plda_init_interrupts() 가 코어 기본 plda_event_ops 를 넣는다.
	 * 읽는 자: plda_handle_event() 가 매 인터럽트마다 역참조한다.
	 * 값 범위: plda_init_interrupts() 통과 후에는 NULL 불가.
	 * 동기화: probe 에서 확정 후 읽기 전용 -- 인터럽트 경로에서 락 없이 읽는 것이
	 * 안전한 이유가 이것이다. */
	const struct plda_event_ops *event_ops;
	/* [한국어] 역할: 이벤트 도메인에 매핑되는 각 hwirq 에 붙일 irq_chip(ack/mask/unmask 구현).
	 * 설정자: Microchip 은 mc_event_irq_chip 을 미리 지정하고, 비어 있으면
	 * plda_init_interrupts() 가 코어의 plda_event_irq_chip 을 넣는다.
	 * 읽는 자: plda_pcie_event_map() 이 irq_set_chip_and_handler() 에 넘긴다.
	 * 값 범위: plda_init_interrupts() 통과 후 NULL 불가.
	 * 동기화: 상수 테이블을 가리키므로 없음.
	 * 두 SoC 가 다른 chip 을 쓰는 이유: PLDA 기본판은 IMASK_LOCAL 한 레지스터만
	 * 다루지만 Microchip 은 이벤트마다 다른 레지스터(브리지 vs 컨트롤러)와 다른
	 * 마스크 극성을 써야 해서 event_descs[] 표를 참조하는 별도 구현이 필요하다. */
	const struct irq_chip *event_irq_chip;
	/* [한국어] 역할: SoC 전원/클럭/PHY 초기화 훅 테이블.
	 * 설정자: StarFive 가 probe 에서 sf_host_ops 를 지정. Microchip 은 지정하지 않는다.
	 * 읽는 자: plda_pcie_host_init() 과 plda_pcie_host_deinit() 이 두 겹 NULL 검사
	 * (host_ops 와 그 안의 함수 포인터)를 거쳐 호출한다.
	 * 값 범위: NULL 허용.
	 * 동기화: probe/remove 프로세스 컨텍스트 전용. */
	const struct plda_pcie_host_ops *host_ops;
	/* [한국어] 역할: 브리지 APB 레지스터 창의 커널 가상 주소. 이 헤더의 거의 모든 오프셋
	 * (GEN_SETTINGS, IMASK_LOCAL, ATR 테이블 등)이 이 주소에 더해진다.
	 * 설정자: StarFive 경로에서는 plda_pcie_host_init() 이 devicetree 의 'apb'
	 * 리소스를 ioremap 해서 채운다. Microchip 경로에서는 mc_host_probe() 가
	 * 자기 bridge_base_addr 을 그대로 대입한다.
	 * 읽는 자: 코어의 모든 readl/writel, 헤더의 static inline 헬퍼들 전부.
	 * 값 범위: 유효 __iomem 포인터. IS_ERR 로 검사된 뒤에만 저장된다.
	 * 동기화: probe 이후 읽기 전용 포인터. 가리키는 레지스터의 동시 접근은 lock 이 담당. */
	void __iomem *bridge_addr;
	/* [한국어] 역할: PCI Configuration Space 접근 창(ECAM 형태)의 커널 가상 주소.
	 * 설정자: plda_pcie_host_init() 이 'cfg' 리소스를 devm_ioremap_resource 로 매핑.
	 * Microchip 경로에서는 채워지지 않는다 -- 그쪽은 ECAM 코어가 만든
	 * pci_config_window 를 sysdata 로 쓰고 pci_ecam_map_bus() 를 쓰기 때문이다.
	 * 읽는 자: plda_pcie_map_bus() 하나뿐이다.
	 * 값 범위: 유효 __iomem 포인터 또는 (Microchip 경로) NULL.
	 * 동기화: probe 이후 읽기 전용. */
	void __iomem *config_base;
	/* [한국어] 역할: 이 컨트롤러에서 실제로 쓸 이벤트 hwirq 집합. 여기 켜진 비트만
	 * plda_init_interrupts() 가 IRQ 를 매핑하고, plda_handle_event() 가 보고한다.
	 * 설정자: SoC 드라이버가 직접 계산한다. StarFive 는 13비트 마스크에서 도어벨
	 * 두 개를 뺀 뒤 16비트 왼쪽 시프트, Microchip 은 GENMASK(NUM_EVENTS - 1, 0).
	 * 읽는 자: plda_init_interrupts() 의 for_each_set_bit, plda_handle_event() 의
	 * events 마스킹.
	 * 값 범위: unsigned long -- 32비트 아키텍처에서도 num_events 가 29 또는 28 이라
	 * 한 워드에 들어간다.
	 * 동기화: probe 에서 확정 후 읽기 전용. for_each_set_bit 가 주소를 받으므로
	 * 타입이 unsigned long 이어야 한다. */
	unsigned long events_bitmap;
	/* [한국어] 역할: 컨트롤러 전체를 대표하는 부모 인터럽트(플랫폼 IRQ 0번). SoC 의
	 * GIC/PLIC 에 물려 있고, 여기에 plda_handle_event() 체인 핸들러가 붙는다.
	 * 설정자: plda_init_interrupts() 의 platform_get_irq(pdev, 0).
	 * 읽는 자: irq_set_chained_handler_and_data(), 해제 시 같은 함수에 NULL 전달.
	 * 값 범위: 양의 virq 번호. 음수면 -ENODEV 로 probe 중단.
	 * 동기화: probe/remove 전용. */
	int irq;
	/* [한국어] 역할: 이벤트 도메인 안에서 'MSI 묶음' 이벤트에 할당된 virq.
	 * 설정자: plda_init_interrupts() 의 irq_create_mapping(event_domain,
	 * event->msi_event).
	 * 읽는 자: 여기에 plda_handle_msi() 체인 핸들러를 붙이고, deinit 에서 뗀다.
	 * 값 범위: 0 이면 실패로 보고 -ENXIO.
	 * 동기화: probe/remove 전용. */
	int msi_irq;
	/* [한국어] 역할: 이벤트 도메인 안에서 'INTx 묶음' 이벤트에 할당된 virq.
	 * 설정자/읽는 자: msi_irq 와 같은 방식(plda_handle_intx 를 붙인다).
	 * 값 범위: 0 이면 -ENXIO.
	 * 동기화: probe/remove 전용.
	 * 주의: 이 virq 는 바로 앞 for_each_set_bit 루프에서 이미 devm_request_irq
	 * (또는 SoC 의 request_event_irq)로 한 번 요청된 상태이며,
	 * irq_set_chained_handler_and_data() 가 그 핸들러를 체인 핸들러로 갈아끼운다. */
	int intx_irq;
	/* [한국어] 역할: 이벤트 도메인의 크기이자 유효 hwirq 개수.
	 * 설정자: SoC 드라이버가 probe 에서 지정 -- StarFive 는 PLDA_MAX_EVENT_NUM(29),
	 * Microchip 은 NUM_EVENTS(28).
	 * 읽는 자: irq_domain_create_linear() 의 size, plda_handle_event() 와
	 * plda_init_interrupts() 의 for_each_set_bit 상한.
	 * 값 범위: 1 이상. 0 이면 도메인 생성이 실패한다.
	 * 동기화: probe 에서 확정 후 읽기 전용. */
	int num_events;
};

/*
 * [한국어]
 * struct plda_event - SoC 가 코어에 넘기는 "이벤트 번호 규약 + IRQ 요청 방식"
 *
 * 왜 필요한가: 이벤트 hwirq 번호는 SoC 마다 오프셋이 다르다(StarFive 는 앞에
 * DMA 이벤트 16개, Microchip 은 자기 전용 이벤트 15개가 놓인다). 그래서 코어는
 * "INTx 는 몇 번, MSI 는 몇 번" 을 스스로 알 수 없고 이 구조체로 통보받는다.
 * 또한 Microchip 은 /proc/interrupts 에 이벤트 이름을 남기고 싶어 하므로
 * IRQ 요청 자체도 훅으로 뺐다.
 *
 * 설정자: SoC 드라이버의 static const 인스턴스(stf_pcie_event, mc_event).
 * 읽는 자: plda_init_interrupts() 하나뿐이며, 값을 저장하지 않고 그 자리에서
 * 다 쓰고 버린다(따라서 수명 문제를 신경 쓸 필요가 없다).
 * 동기화: const 이므로 없음.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> plda_pcie_host_init(..., &stf_pcie_event)
 *                         -> plda_init_interrupts(pdev, port, event)
 *   mc_platform_init()    -> plda_init_interrupts(pdev, &port->plda, &mc_event)
 */
struct plda_event {
	/* [한국어] 역할: 이벤트 hwirq 하나에 대해 IRQ 핸들러를 어떻게 요청할지 정하는 SoC 훅.
	 * 설정자: Microchip 이 mc_request_event_irq 를 지정한다(이벤트 이름 문자열을
	 * /proc/interrupts 에 남기기 위해). StarFive 는 지정하지 않는다.
	 * 읽는 자: plda_init_interrupts() 의 루프. NULL 이면 코어가 대신
	 * devm_request_irq(..., plda_event_handler, 0, NULL, port) 를 쓴다.
	 * 값 범위: NULL 허용. 반환 0 이 성공, 음수면 probe 중단.
	 * 동기화: probe 프로세스 컨텍스트에서만 호출.
	 * @pcie: 이 컨트롤러. @event_irq: irq_create_mapping 이 돌려준 virq.
	 * @event: 이벤트 hwirq 번호(Microchip 은 이것으로 event_cause[] 이름을 찾는다). */
	int (*request_event_irq)(struct plda_pcie_rp *pcie,
				 int event_irq, int event);
	/* [한국어] 역할: 이 SoC 에서 'INTx 묶음' 이벤트의 hwirq 번호.
	 * 설정자: SoC 드라이버의 static const 테이블 -- StarFive 는
	 * EVENT_PM_MSI_INT_INTX(24), Microchip 은 EVENT_LOCAL_PM_MSI_INT_INTX(23).
	 * 읽는 자: plda_init_interrupts() 가 irq_create_mapping 에 넘긴다.
	 * 값 범위: 0 .. num_events-1.
	 * 동기화: const 테이블이므로 없음.
	 * 두 SoC 의 값이 다른 이유는 앞에 놓인 SoC 전용 이벤트 개수(16 vs 15)가
	 * 다르기 때문이며, 이것이 이 필드가 하드코딩 대신 존재하는 이유다. */
	int intx_event;
	/* [한국어] 역할: 이 SoC 에서 'MSI 묶음' 이벤트의 hwirq 번호.
	 * 설정자/읽는 자/동기화: intx_event 와 동일. StarFive 25, Microchip 24. */
	int msi_event;
};

/* [한국어] 선언: ECAM 규칙으로 config space 접근 주소를 계산해 돌려주는 pci_ops.map_bus.
 * 정의는 pcie-plda-host.c. 이 트리에서 쓰는 곳은 pcie-starfive.c 의
 * starfive_pcie_ops 하나뿐이다(Microchip 은 pci_ecam_map_bus 를 쓴다). */
void __iomem *plda_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				int where);
/* [한국어] 선언: 이벤트/INTx/MSI 세 도메인 생성과 체인 핸들러 연결 전체.
 * 정의는 pcie-plda-host.c. 호출자는 두 SoC 드라이버 모두 --
 * StarFive 는 간접적으로(plda_pcie_host_init 안에서), Microchip 은 직접
 * mc_platform_init() 에서 호출한다. */
int plda_init_interrupts(struct platform_device *pdev,
			 struct plda_pcie_rp *port,
			 const struct plda_event *event);
/* [한국어] 선언: 아웃바운드(AXI -> PCIe) 주소 변환 창 하나를 프로그래밍한다.
 * 정의는 pcie-plda-host.c. 호출자: plda_pcie_host_init(0번=config 창),
 * plda_pcie_setup_iomems(1번 이후 메모리 창), Microchip 의 mc_platform_init. */
void plda_pcie_setup_window(void __iomem *bridge_base_addr, u32 index,
			    phys_addr_t axi_addr, phys_addr_t pci_addr,
			    size_t size);
/* [한국어] 선언: 인바운드(PCIe -> AXI) 변환 창 0번의 크기 필드만 설정한다.
 * 정의는 pcie-plda-host.c 이고 EXPORT 도 되어 있으나,
 * 이 트리 안에서 이 함수를 호출하는 곳은 한 군데도 없다(grep 결과 0건).
 * Microchip 은 자체 구현 mc_pcie_setup_inbound_atr() 을 쓰고 StarFive 는
 * 인바운드 변환을 건드리지 않는다. 트리 밖 사용자가 있는지는 확인할 수 없다. */
void plda_pcie_setup_inbound_address_translation(struct plda_pcie_rp *port);
/* [한국어] 선언: 브리지의 모든 IORESOURCE_MEM 창을 순회하며 1번부터 ATR 창을 채운다.
 * 정의는 pcie-plda-host.c. 호출자: plda_pcie_host_init(StarFive 경로),
 * mc_platform_init(Microchip 경로) 둘 다. */
int plda_pcie_setup_iomems(struct pci_host_bridge *bridge,
			   struct plda_pcie_rp *port);
/* [한국어] 선언: StarFive 경로의 통합 진입점 -- ioremap 부터 pci_host_probe 까지.
 * 정의는 pcie-plda-host.c. 이 트리의 호출자는 starfive_pcie_probe() 하나뿐이다. */
int plda_pcie_host_init(struct plda_pcie_rp *port, struct pci_ops *ops,
			const struct plda_event *plda_event);
/* [한국어] 선언: 위 함수의 짝. 루트 버스를 떼고 도메인을 지운 뒤 SoC 훅으로 전원을 내린다.
 * 정의는 pcie-plda-host.c. 이 트리의 호출자는 starfive_pcie_remove() 하나뿐이다. */
void plda_pcie_host_deinit(struct plda_pcie_rp *pcie);

/*
 * [한국어]
 * plda_set_default_msi - MSI 벡터 주소와 개수를 PLDA IP 의 기본값으로 채운다
 *
 * @msi: 채울 MSI 상태 구조체. 보통 &port->msi 로 전달된다.
 * @return: 없음.
 *
 * 왜 필요한가: MSI 도메인을 만들기 전에 "벡터가 몇 개이고 장치가 어느 주소로
 * MSI 를 써야 하는가" 가 정해져 있어야 한다. Microchip 처럼 하드웨어에서 그 값을
 * 읽어 오는 SoC 도 있지만, StarFive 처럼 IP 기본값을 그대로 쓰는 경우를 위한
 * 한 줄짜리 기본값 설정이다.
 *
 * 동작: vector_phy 에 IMSI_ADDR(0x190)을, num_vectors 에
 * PLDA_MAX_NUM_MSI_IRQS(32)를 대입한다. vector_phy 에 레지스터 오프셋 값을 그대로
 * 넣는 것이 이상해 보이지만, 이 값은 CPU 가 접근할 주소가 아니라 PCIe 장치가
 * 쓰기 요청을 보낼 주소로서 브리지가 해석하는 값이다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 재진입/동시성 고려 대상이 아니다.
 * 호출자: plda_pcie_host_init()(pcie-plda-host.c) 한 곳뿐이다. 따라서 실질적으로
 * StarFive 경로 전용이며, Microchip 은 이 함수를 거치지 않고 mc_host_probe() 에서
 * 두 필드를 직접 채운다.
 * 피호출자: 없음(대입 두 개).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> plda_pcie_host_init() -> [이 함수]
 */
static inline void plda_set_default_msi(struct plda_msi *msi)
{
	/* [한국어] 장치가 MSI 를 쓸 목적지 주소를 IMSI_ADDR(0x190)로 둔다. CPU 가 읽을 주소가 아니라
	 * 브리지가 MSI 수신으로 해석하는 주소이므로 오프셋 값을 그대로 넣는 것이 맞다. */
	msi->vector_phy = IMSI_ADDR;
	/* [한국어] 벡터 개수를 IP 최대치 32 로 둔다. 이후 plda_allocate_msi_domains() 가 이 값을
	 * irq_domain_info.size 로 써서 도메인 크기를 정한다. */
	msi->num_vectors = PLDA_MAX_NUM_MSI_IRQS;
}

/*
 * [한국어]
 * plda_pcie_enable_root_port - GEN_SETTINGS.RP_ENABLE 을 세워 Root Port 모드로 켠다
 *
 * @plda: bridge_addr 이 이미 ioremap 되어 있는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: PLDA XpressRich IP 는 Root Port 로도 Endpoint 로도 합성될 수 있고,
 * 리셋 직후에는 루트 포트 동작이 꺼져 있다. 이 비트를 세우지 않으면 브리지가
 * 하위 링크를 훈련시키지도, config 요청을 만들어 내지도 않는다.
 *
 * 동작: GEN_SETTINGS(0x80)를 읽어 bit0(RP_ENABLE)만 OR 한 뒤 되쓴다. 다른 비트는
 * 하드웨어/부트로더가 정한 값이므로 보존해야 해서 read-modify-write 를 쓴다.
 * readl_relaxed / writel_relaxed 를 쓰는 이유: 이 시점에는 DMA 버퍼 가시성 같은
 * 메모리 순서 보장이 필요 없고, 같은 APB 창에 대한 접근끼리는 순서가 유지되므로
 * 비싼 배리어를 넣을 이유가 없다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(SoC host_init 훅 안). 잠들지 않는다.
 * 이 함수는 락을 잡지 않는데, 초기화 시점에는 이 레지스터를 건드리는 다른
 * 실행 흐름이 없기 때문이다.
 * 호출자: starfive_pcie_host_init()(pcie-starfive.c) 하나뿐이다.
 * 피호출자: readl_relaxed / writel_relaxed.
 * 에러 경로: 없음 -- 쓰기 결과를 확인하지 않는다.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static inline void plda_pcie_enable_root_port(struct plda_pcie_rp *plda)
{
	/* [한국어] GEN_SETTINGS 의 현재 값을 담을 임시 변수. read-modify-write 를 해야 하므로 필요하다. */
	u32 value;

	/* [한국어] GEN_SETTINGS(0x80) 를 읽는다. relaxed 판이라 메모리 배리어가 없다 -- 같은 APB
	 * 영역 접근끼리는 순서가 보장되고 DMA 와의 순서 문제가 없기 때문이다. */
	value = readl_relaxed(plda->bridge_addr + GEN_SETTINGS);
	/* [한국어] bit0(RP_ENABLE)만 세운다. 나머지 비트는 하드웨어/부트로더 설정이라 보존한다. */
	value |= RP_ENABLE;
	/* [한국어] 되쓰기. 이 순간부터 브리지가 Root Port 로 동작하기 시작한다. */
	writel_relaxed(value, plda->bridge_addr + GEN_SETTINGS);
}

/*
 * [한국어]
 * plda_pcie_set_standard_class - 루트 포트의 Class Code 를 PCI-to-PCI 브리지로 고친다
 *
 * @plda: bridge_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: PLDA IP 의 기본 Class Code 는 표준 브리지 값이 아니어서, 그대로
 * 두면 커널 PCI 코어가 이 장치를 브리지로 인식하지 못해 하위 버스를 열거하지
 * 않는다. PCI_CLASS_BRIDGE_PCI(0x0604)를 써 넣어 base class 0x06(Bridge),
 * sub class 0x04(PCI-to-PCI), prog-if 0x00(Normal decode) 으로 만든다.
 *
 * 동작 단계:
 *  1. PCIE_PCI_IDS_DW1(0x9c)을 읽는다. 이 워드는 PCI 규격의 config 오프셋 0x08
 *     (Revision ID + Class Code)에 대응한다.
 *  2. REVISION_ID_MASK(하위 8비트)로 잘라 Revision ID 만 남긴다 -- 이 값은
 *     실리콘/비트파일이 정한 것이므로 보존해야 한다.
 *  3. 0x0604 를 IDS_CLASS_CODE_SHIFT(16)만큼 밀어 0x06040000 을 만들어 OR 한다.
 *     결과적으로 비트 31:8 이 060400 이 된다.
 *  4. 같은 오프셋에 되쓴다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음(초기화 중 단독 접근).
 * 호출자: starfive_pcie_host_init() 하나뿐이다.
 * 피호출자: readl_relaxed / writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static inline void plda_pcie_set_standard_class(struct plda_pcie_rp *plda)
{
	/* [한국어] PCIE_PCI_IDS_DW1 값을 담을 임시 변수. */
	u32 value;

	/* set class code and reserve revision id */
	value = readl_relaxed(plda->bridge_addr + PCIE_PCI_IDS_DW1);
	/* [한국어] 하위 8비트(Revision ID)만 남기고 나머지를 지운다. Class Code 자리를 비워
	 * 다음 줄의 OR 가 깨끗하게 들어가도록 하기 위함이다. */
	value &= REVISION_ID_MASK;
	/* [한국어] PCI_CLASS_BRIDGE_PCI(0x0604)를 16비트 밀어 0x06040000 을 만들어 OR 한다.
	 * 결과: base class 0x06(Bridge), sub class 0x04(PCI-to-PCI), prog-if 0x00.
	 * 이 값이라야 커널 PCI 코어가 브리지로 인식하고 하위 버스를 열거한다. */
	value |= (PCI_CLASS_BRIDGE_PCI << IDS_CLASS_CODE_SHIFT);
	/* [한국어] 되쓰기. 이후 config 읽기에서 이 장치는 표준 PCI-to-PCI 브리지로 보인다. */
	writel_relaxed(value, plda->bridge_addr + PCIE_PCI_IDS_DW1);
}

/*
 * [한국어]
 * plda_pcie_set_pref_win_64bit - prefetchable 메모리 창의 64비트 주소 지원을 켠다
 *
 * @plda: bridge_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 이 비트가 꺼져 있으면 브리지가 prefetchable 창을 32비트로만
 * 광고하고, ATR(주소 변환) 테이블에 넣은 64비트 상위 주소가 무시된다. JH7110 은
 * 4GiB 위쪽에 prefetchable 창을 두므로 반드시 켜야 한다(호출부의 영문 주석 참조).
 *
 * 동작: PCIE_WINROM(0xfc)을 읽어 bit3(PREF_MEM_WIN_64_SUPPORT)만 OR 하고 되쓴다.
 * 다른 비트를 보존해야 하므로 read-modify-write 다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: starfive_pcie_host_init() 하나뿐이다.
 * 피호출자: readl_relaxed / writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static inline void plda_pcie_set_pref_win_64bit(struct plda_pcie_rp *plda)
{
	/* [한국어] PCIE_WINROM 값을 담을 임시 변수. */
	u32 value;

	/* [한국어] PCIE_WINROM(0xfc) 을 읽는다. */
	value = readl_relaxed(plda->bridge_addr + PCIE_WINROM);
	/* [한국어] bit3(PREF_MEM_WIN_64_SUPPORT)을 세워 prefetchable 창의 64비트 주소 지정을 켠다. */
	value |= PREF_MEM_WIN_64_SUPPORT;
	/* [한국어] 되쓰기. 이 뒤에야 ATR 의 상위 32비트 주소 설정이 실제로 먹는다. */
	writel_relaxed(value, plda->bridge_addr + PCIE_WINROM);
}

/*
 * [한국어]
 * plda_pcie_disable_ltr - LTR 메시지 수신/전달을 끈다
 *
 * @plda: bridge_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: LTR(Latency Tolerance Reporting)은 엔드포인트가 "나는 이만큼의
 * 지연을 견딜 수 있다" 고 알리는 PCIe 메시지다. PLDA IP 는 이 메시지 수신이 기본
 * 켜짐이면서 전달(forward) 대상 주소는 초기화되지 않은 상태로 리셋된다. 그 상태로
 * 장치가 LTR 을 보내면 브리지가 쓰레기 주소로 쓰기를 시도해 커널이 멈춘다 --
 * 호출부(starfive_pcie_host_init)의 영문 주석이 이 현상을 그대로 기술하고 있다.
 * 그래서 기능을 쓰기 전에 일단 꺼 두는 것이 workaround 다.
 *
 * 동작: PMSG_SUPPORT_RX(0x3f0)를 읽어 bit2(PMSG_LTR_SUPPORT)를 AND-NOT 으로 지우고
 * 되쓴다. 여기만 OR 가 아니라 비트 클리어라는 점에 주의.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: starfive_pcie_host_init() 하나뿐이다.
 * 피호출자: readl_relaxed / writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static inline void plda_pcie_disable_ltr(struct plda_pcie_rp *plda)
{
	/* [한국어] PMSG_SUPPORT_RX 값을 담을 임시 변수. */
	u32 value;

	/* [한국어] PMSG_SUPPORT_RX(0x3f0) 를 읽는다. */
	value = readl_relaxed(plda->bridge_addr + PMSG_SUPPORT_RX);
	/* [한국어] bit2(PMSG_LTR_SUPPORT)를 지운다. OR 가 아니라 AND-NOT 인 것에 주의 -- 여기서는
	 * 기능을 끄는 것이 목적이다. 켜 둔 채로 두면 전달 주소가 초기화되지 않아 커널이 멈춘다. */
	value &= ~PMSG_LTR_SUPPORT;
	/* [한국어] 되쓰기. 이후 브리지는 LTR 메시지를 받아 전달하지 않는다. */
	writel_relaxed(value, plda->bridge_addr + PMSG_SUPPORT_RX);
}

/*
 * [한국어]
 * plda_pcie_disable_func - 현재 선택된 물리 함수(PF)를 비활성화한다
 *
 * @plda: bridge_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: PLDA IP 는 루트 포트를 여러 개의 물리 함수로 합성할 수 있는데,
 * JH7110 은 PF0 하나만 쓰고 나머지 PF1~PF3 은 보이지 않게 꺼야 한다. 그렇지
 * 않으면 PCI 코어가 존재하지도 않는 함수를 열거하게 된다.
 *
 * 중요한 함정: 이 함수 자체는 "어느 PF 를" 끌지 고르지 못한다. 대상 선택은
 * 호출자가 미리 해 두어야 하며, StarFive 는 STG syscon 의 AXI4 slave AR/AW 필드에
 * 함수 번호를 넣어 그 다음 브리지 레지스터 접근이 해당 PF 의 레지스터로 가도록
 * 만든다(starfive_pcie_host_init 의 루프 참조). 즉 이 함수는 반드시 그 설정 뒤에
 * 호출되어야 의미가 있다. 이 "AR/AW 필드가 대상 PF 를 고른다" 는 해석은 사용
 * 패턴에서 추론한 것이며, JH7110 STG 데이터시트는 이 트리에 없다.
 *
 * 동작: PCI_MISC(0xb4)를 읽어 bit15(PHY_FUNCTION_DIS)를 OR 하고 되쓴다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: starfive_pcie_host_init() 의 PF 순회 루프 하나뿐이다.
 * 피호출자: readl_relaxed / writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수] (PF 마다 1회)
 */
static inline void plda_pcie_disable_func(struct plda_pcie_rp *plda)
{
	/* [한국어] PCI_MISC 값을 담을 임시 변수. */
	u32 value;

	/* [한국어] PCI_MISC(0xb4) 를 읽는다. 이때 어느 PF 의 레지스터가 읽히는지는 호출자가
	 * STG syscon 으로 미리 골라 둔 상태에 달려 있다. */
	value = readl_relaxed(plda->bridge_addr + PCI_MISC);
	/* [한국어] bit15(PHY_FUNCTION_DIS)를 세워 그 PF 를 비활성화한다. */
	value |= PHY_FUNCTION_DIS;
	/* [한국어] 되쓰기. PF1~PF3 에 대해 이 세 줄이 반복되어 PF0 만 남는다. */
	writel_relaxed(value, plda->bridge_addr + PCI_MISC);
}

/*
 * [한국어]
 * plda_pcie_write_rc_bar - 루트 컴플렉스 자신의 BAR0/BAR1 에 64비트 값을 써 넣는다
 *
 * @plda: bridge_addr 이 매핑된 컨트롤러.
 * @val: BAR0(하위 32비트)과 BAR1(상위 32비트)에 나눠 쓸 64비트 값.
 *       이 트리의 유일한 호출자는 0 을 넘긴다.
 * @return: 없음.
 *
 * 왜 필요한가: JH7110 에서 루트 포트의 BAR0/BAR1 은 PLDA 가 말하는 'Bridge
 * Configuration space'(인터럽트/DMA/ATU 레지스터가 들어 있는 내부 창)로 라우팅된다.
 * 이 BAR 이 유효한 주소를 갖고 있으면 엔드포인트가 DMA 로 브리지 내부 레지스터를
 * 건드릴 수 있게 되므로, 0 을 써서 창을 무력화한다. 같은 목적의 짝이
 * starfive_pcie_hide_rc_bar() 로, 그쪽은 소프트웨어가 config 로 읽고 쓰는 것을
 * 가로막는다 -- 즉 하드웨어 쪽(이 함수)과 소프트웨어 쪽(hide_rc_bar) 양쪽을 함께
 * 막는 구성이다.
 *
 * 동작 단계:
 *  1. bridge_addr + CONFIG_SPACE_ADDR_OFFSET(0x1000)으로 브리지 자신의 PCI
 *     config space 창 시작 주소를 구한다.
 *  2. 거기에 PCI_BASE_ADDRESS_0(0x10)과 PCI_BASE_ADDRESS_1(0x14)을 더해
 *     lower_32_bits / upper_32_bits 를 각각 쓴다.
 *  두 BAR 을 짝으로 쓰는 것은 64비트 BAR 이 연속한 두 DWORD 를 차지한다는
 *  PCI 규격 때문이다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: starfive_pcie_host_init() 하나뿐이다.
 * 피호출자: writel_relaxed 두 번.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static inline void plda_pcie_write_rc_bar(struct plda_pcie_rp *plda, u64 val)
{
	/* [한국어] 브리지 자신의 PCI config space 창 시작 주소 = APB 창 기준 + 0x1000.
	 * EP 의 config space 가 아니라 '이 루트 포트 자신' 의 config space 라는 점이 핵심이다. */
	void __iomem *addr = plda->bridge_addr + CONFIG_SPACE_ADDR_OFFSET;

	/* [한국어] BAR0(오프셋 0x10)에 하위 32비트를 쓴다. */
	writel_relaxed(lower_32_bits(val), addr + PCI_BASE_ADDRESS_0);
	/* [한국어] BAR1(오프셋 0x14)에 상위 32비트를 쓴다. 64비트 BAR 은 연속한 두 DWORD 를
	 * 차지한다는 PCI 규격에 따른 짝 쓰기다. 호출자가 0 을 넘기므로 실제로는
	 * 두 BAR 을 모두 0 으로 만들어 EP 의 DMA 가 브리지 내부 레지스터에 닿지 못하게 한다. */
	writel_relaxed(upper_32_bits(val), addr + PCI_BASE_ADDRESS_1);
}
#endif /* _PCIE_PLDA_H */
