// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas RZ/G3S SoCs
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 *
 * Based on:
 *  drivers/pci/controller/pcie-rcar-host.c
 *  Copyright (C) 2009 - 2011  Paul Mundt
 */

/*
 * [한국어 설명] Renesas RZ/G3S · RZ/G3E SoC 의 PCIe 호스트 컨트롤러 (pcie-rzg3s-host.c)
 *
 * === 파일의 역할 ===
 * 르네사스 RZ/G3S(r9a08g045)와 RZ/G3E(r9a09g047)에 들어 있는 PCIe 컨트롤러를
 * 루트 컴플렉스로 모는 플랫폼 드라이버다. 파일 머리의 저작권 주석이 밝히듯
 * pcie-rcar-host.c 를 바탕으로 만들어졌지만, 레지스터 구성과 초기화 절차는
 * 이 SoC 고유의 것이다.
 *
 * 이 컨트롤러의 성격을 규정하는 것이 두 종류의 레지스터 창이다.
 *   - host->axi : 컨트롤러 자신의 제어 레지스터 묶음(요청 발행, 인터럽트,
 *     주소 창, PHY, 리셋). 이 파일의 거의 모든 readl/writel 이 여기를 향한다.
 *   - host->pcie: 그 안의 0x6000 부터 열리는 RC 자신의 PCI config 공간.
 *     probe 가 host->axi + RZG3S_PCI_CFG_BASE 로 계산해 둔다.
 *
 * 하위 버스의 config 접근이 특이하다. ECAM 창이 없어, 주소·바이트 인에이블·
 * 요청 종류를 레지스터에 차례로 적고 "요청 발행" 비트를 세운 뒤 완료를
 * 폴링하는 방식이다(rzg3s_pcie_child_issue_request). 그래서 pci_ops 가
 * 둘로 나뉜다 — RC 자신은 map_bus 로 주소만 주면 되고(rzg3s_pcie_root_ops),
 * 그 아래는 read/write 를 직접 구현한다(rzg3s_pcie_child_ops).
 *
 * SoC 차이는 struct rzg3s_pcie_soc_data 의 함수 포인터 넷(init_phy /
 * config_pre_init / config_post_init / config_deinit)과 리셋 이름 배열,
 * 그리고 SYSC 레지스터 좌표표로 흡수한다. RZ/G3S 는 PHY 를 소프트웨어로
 * 채워 넣고 리셋을 여러 개 다루며, RZ/G3E 는 PHY 설정이 없는 대신
 * RZG3S_PCI_RESET 레지스터의 비트를 순서대로 푸는 방식을 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층은 PCI 코어 -> pci_host_bridge -> 이 파일 -> AXI 레지스터 MMIO /
 * SYSC regmap / 리셋·클럭 -> SoC 하드웨어 순이다.
 *
 *   platform_driver -> rzg3s_pcie_probe()
 *     -> devm_pci_alloc_host_bridge()      브리지와 private 를 함께 할당
 *     -> device_get_match_data()           SoC 표(rzg3s_soc_data/rzg3e_soc_data)
 *     -> devm_platform_ioremap_resource()  AXI 창을 잡고 pcie 를 오프셋으로 얻음
 *     -> rzg3s_pcie_host_parse_port()      DT 자식 노드에서 vendor/device id 와 ref 클럭
 *     -> syscon_node_to_regmap()           SYSC 를 regmap 으로 연다
 *     -> rzg3s_sysc_config_func(MODE/RST_RSM_B)  RC 모드로 두고 리셋 신호 해제
 *     -> rzg3s_pcie_resets_prepare_and_get() / _power_resets_deassert()
 *     -> pm_runtime_resume_and_get()       클럭이 전원 도메인에 묶여 있어 RPM 으로 켠다
 *     -> rzg3s_pcie_host_setup()           창 설정 -> IRQ 도메인 -> HW 초기화 -> 속도
 *     -> pci_host_probe()                  PCI 코어에 열거를 넘긴다
 *
 * rzg3s_pcie_host_setup() 이 init/teardown 콜백을 인자로 받는 것이 이 파일의
 * 눈에 띄는 구조다. probe 는 IRQ 도메인을 새로 만드는 함수 쌍을 넘기고,
 * resume 은 도메인이 메모리에 그대로 남아 있으므로 MSI 하드웨어만 다시
 * 세우는 함수 쌍(rzg3s_pcie_msi_hw_setup / _hw_teardown)을 넘긴다.
 * 덕분에 창 설정과 링크 기동 절차를 두 경로가 그대로 공유한다.
 *
 * 인터럽트는 두 갈래다. INTx 는 DT 의 int a~d 네 선을 각각 체인 핸들러로
 * 받아 선형 도메인으로 넘기고, MSI 는 "msi" 라는 이름의 선 하나를 공유
 * 핸들러로 받아 상태 레지스터를 훑는다. MSI 목적지는 이 드라이버가 잡은
 * 페이지의 DMA 주소이며, 그 주소가 이미 열려 있는 안쪽(AXI) 창 안에
 * 들어가야 한다는 하드웨어 제약이 rzg3s_pcie_msi_setup() 의 탐색 루프를
 * 만든다.
 *
 * 실행 컨텍스트: probe/PM/config 접근은 프로세스 컨텍스트다. 다만 config
 * 접근은 폴링(readl_poll_timeout_atomic)으로 기다리므로 잠들지 않는다.
 * INTx 체인 핸들러와 MSI 핸들러, irq_chip 콜백은 인터럽트 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_host_probe(), drivers/pci/access.c 의
 *   config 접근 경로(그 pci_lock 이 이 파일 child read/write 의 직렬화를
 *   맡는다 — 상류 주석이 두 함수 위에 그렇게 적어 두었다), 그리고 ../pci.h 의
 *   PCIE_LINK_WAIT_SLEEP_MS / PCIE_LINK_WAIT_MAX_RETRIES /
 *   PCIE_RESET_CONFIG_WAIT_MS 와 pcie_get_link_speed().
 * 아래쪽: MSI 코어(msi_create_parent_irq_domain, msi_lib_init_dev_msi_info),
 *   irq_domain 코어, 리셋 컨트롤러(bulk API), 클럭, 런타임 PM,
 *   그리고 SYSC(시스템 컨트롤러)를 regmap 으로 다루는 syscon.
 * 공유 상태: struct rzg3s_pcie_host 가 컨트롤러 하나를 담고, 그 안에
 *   struct rzg3s_pcie_msi 가 값으로 박혀 있다(rzg3s_msi_to_host 매크로가
 *   container_of 로 되돌린다). 전역 가변 상태는 없다. host->hw_lock 이
 *   읽고-고쳐-쓰기가 필요한 인터럽트 인에이블/마스크 레지스터를 지킨다.
 *
 * === 주요 함수/구조체 요약 ===
 * rzg3s_pcie_probe()            : 진입점. 자원 확보부터 열거 시작까지 엮는다.
 * rzg3s_pcie_host_setup()       : 창 -> IRQ -> HW 초기화 -> 속도. probe/resume 공용.
 * rzg3s_pcie_host_init()        : 설정·PHY·인터럽트·링크업을 순서대로 세운다.
 * rzg3s_pcie_child_read/write() : 하위 버스 config 접근. 요청 발행 방식이다.
 * rzg3s_pcie_set_inbound_windows(): dma-ranges 를 2의 거듭제곱 창들로 쪼갠다.
 * rzg3s_pcie_set_outbound_window(): 바깥 창 하나를 연다.
 * rzg3s_pcie_msi_setup()        : MSI 목적지 페이지를 잡고 창 안인지 확인한다.
 * rzg3s_pcie_msi_irq()          : MSI 상태 레지스터를 훑는 공유 핸들러.
 * rzg3s_pcie_set_max_link_speed(): LTSSM 이 L0 일 때만 속도 변경을 요청한다.
 * rzg3s_soc_pcie_init_phy()     : RZ/G3S 의 PHY 레지스터를 표대로 채운다.
 * struct rzg3s_pcie_host        : 컨트롤러 하나의 모든 상태.
 * struct rzg3s_pcie_msi         : MSI 도메인·비트맵·목적지 페이지.
 * struct rzg3s_pcie_soc_data    : SoC 별 함수 포인터·리셋 이름·SYSC 좌표표.
 * struct rzg3s_sysc             : SYSC regmap 과 그 좌표표.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 드라이버는 버스를 만드는 쪽이고 NVMe 는 그 위에 열거되는 장치라
 * 계층이 다르다. 이 SoC 에 NVMe SSD 를 붙인다면 이 파일의 안쪽 창 설정이
 * SSD 의 DMA 가 DRAM 에 닿는 통로가 되고, MSI 창 설정이 완료 인터럽트의
 * 경로가 된다. 다만 그 처리에 NVMe 에 특화된 부분은 없고 모든 PCIe 장치에
 * 똑같이 적용된다.
 */

#include <linux/bitfield.h> /* [한국어] FIELD_GET/FIELD_PREP/field_prep — 레지스터 비트 필드 조작의 기본 도구 */
#include <linux/bitmap.h> /* [한국어] DECLARE_BITMAP 과 bitmap_find_free_region/release_region/write — MSI 벡터 관리 */
#include <linux/bitops.h> /* [한국어] BIT()/GENMASK()/__fls()/__ffs() — 마스크 상수와 창 크기·정렬 계산 */
#include <linux/cleanup.h> /* [한국어] guard()/scoped_guard()/__free() — 범위를 벗어날 때 락과 참조를 자동으로 놓는다 */
#include <linux/clk.h> /* [한국어] clk_prepare_enable/clk_disable_unprepare/clk_put — 포트 기준 클럭 제어 */
#include <linux/delay.h> /* [한국어] fsleep/msleep — 하드웨어가 요구하는 지연 */
#include <linux/iopoll.h> /* [한국어] readl_poll_timeout 과 그 atomic 판 — 요청 완료·링크업·속도 변경 대기 */
#include <linux/interrupt.h> /* [한국어] request_irq/free_irq 와 irqreturn_t — MSI 핸들러 등록 */
#include <linux/irq.h> /* [한국어] irq_data 와 irq_chip 정의 */
#include <linux/irqchip/chained_irq.h> /* [한국어] chained_irq_enter/exit — INTx 체인 핸들러가 상위 선을 마스크할 때 */
#include <linux/irqchip/irq-msi-lib.h> /* [한국어] msi_lib_init_dev_msi_info 와 MSI_FLAG_ 계열 — 상위 MSI 계층에 붙는 규약 */
#include <linux/irqdomain.h> /* [한국어] irq_domain / irq_domain_set_info / msi_create_parent_irq_domain */
#include <linux/kernel.h> /* [한국어] ARRAY_SIZE, min/max, ALIGN 등 공통 매크로 */
#include <linux/mfd/syscon.h> /* [한국어] syscon_node_to_regmap — SYSC 를 regmap 으로 연다 */
#include <linux/mutex.h> /* [한국어] devm_mutex_init 과 뮤텍스 정의 — MSI 비트맵 보호 */
#include <linux/msi.h> /* [한국어] struct msi_msg — MSI 주소/데이터 조립 */
#include <linux/of_irq.h> /* [한국어] DT 인터럽트 관련 정의 */
#include <linux/pci.h> /* [한국어] PCI 코어 API 와 PCI_EXP_ / PCIBIOS_ 계열 스펙 상수 */
#include <linux/platform_device.h> /* [한국어] platform_get_irq_byname, devm_platform_ioremap_resource 등 */
#include <linux/pm_runtime.h> /* [한국어] pm_runtime_ 계열 — 클럭 전원 도메인을 깨우고 재운다 */
#include <linux/regmap.h> /* [한국어] regmap_update_bits — SYSC 레지스터 갱신 */
#include <linux/reset.h> /* [한국어] reset_control_bulk_ 계열 — 전원/설정 리셋 묶음 조작 */
#include <linux/sizes.h> /* [한국어] SZ_4K — 창 최소 크기와 정렬 상수 */
#include <linux/slab.h> /* [한국어] devm_kmalloc_array, devm_kzalloc, devm_kasprintf */
#include <linux/units.h> /* [한국어] MILLI — 밀리초를 마이크로초로 바꾸는 배수 상수. 폴링 간격 계산에 쓰인다 */

#include "../pci.h" /* [한국어] PCI 서브시스템 내부 헤더. PCIE_LINK_WAIT_ 계열 상수와 pcie_get_link_speed() */

/* AXI registers */
#define RZG3S_PCI_REQDATA(id)			(0x80 + (id) * 0x4) /* [한국어] 하위 버스 config 쓰기에 실을 데이터 레지스터 셋(4바이트 간격). write_conf 가 0,1 을 0 으로 지우고 2 에 값을 넣는다 */
#define RZG3S_PCI_REQRCVDAT			0x8c /* [한국어] 하위 버스 config 읽기 결과가 담기는 레지스터 */

#define RZG3S_PCI_REQADR1			0x90 /* [한국어] 요청 대상 주소 레지스터. BDF 와 config 오프셋이 한 워드에 들어간다 */
#define RZG3S_PCI_REQADR1_BUS			GENMASK(31, 24) /* [한국어] 그중 버스 번호 필드 */
#define RZG3S_PCI_REQADR1_DEV			GENMASK(23, 19) /* [한국어] 그중 장치 번호 필드 */
#define RZG3S_PCI_REQADR1_FUNC			GENMASK(18, 16) /* [한국어] 그중 함수 번호 필드 */
#define RZG3S_PCI_REQADR1_REG			GENMASK(11, 0) /* [한국어] 그중 config 레지스터 오프셋 필드. prepare_bus 가 하위 2비트를 지워 넣는다 */

#define RZG3S_PCI_REQBE				0x98 /* [한국어] 요청의 바이트 인에이블 레지스터 */
#define RZG3S_PCI_REQBE_BYTE_EN			GENMASK(3, 0) /* [한국어] 그 필드. 이 드라이버는 늘 네 바이트 모두 유효로 적어, 좁은 쓰기가 인접 바이트를 함께 건드린다 */

#define RZG3S_PCI_REQISS			0x9c /* [한국어] 요청 발행/상태 레지스터 */
#define RZG3S_PCI_REQISS_MOR_STATUS		GENMASK(18, 16) /* [한국어] 요청 결과 상태 필드. 0 이 아니면 대상이 오류로 답한 것이라 issue_request 가 -EIO 를 돌려준다 */
#define RZG3S_PCI_REQISS_TR_TYPE		GENMASK(11, 8) /* [한국어] 요청 종류 필드 */
#define RZG3S_PCI_REQISS_TR_TP0_RD		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x4) /* [한국어] Type 0 읽기 — 루트 버스 바로 아래 장치를 향한다 */
#define RZG3S_PCI_REQISS_TR_TP0_WR		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x5) /* [한국어] Type 0 쓰기 */
#define RZG3S_PCI_REQISS_TR_TP1_RD		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x6) /* [한국어] Type 1 읽기 — 브리지가 중계해야 하는 더 깊은 버스를 향한다 */
#define RZG3S_PCI_REQISS_TR_TP1_WR		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x7) /* [한국어] Type 1 쓰기 */
#define RZG3S_PCI_REQISS_REQ_ISSUE		BIT(0) /* [한국어] 요청 발행 비트. 세우면 하드웨어가 보내고, 끝나면 스스로 내린다 */

#define RZG3S_PCI_MSIRCVWADRL			0x100 /* [한국어] MSI 수신 창 주소 하위 워드. 활성 비트들이 이 워드에 함께 들어 있다 */
#define RZG3S_PCI_MSIRCVWADRL_MASK		GENMASK(31, 3) /* [한국어] 그 워드에서 순수한 주소만 남기는 마스크. compose_msi_msg 가 이것으로 활성 비트를 떨어낸다 */
#define RZG3S_PCI_MSIRCVWADRL_MSG_DATA_ENA	BIT(1) /* [한국어] 메시지 데이터 사용 활성 비트 */
#define RZG3S_PCI_MSIRCVWADRL_ENA		BIT(0) /* [한국어] MSI 창 활성 비트. 이 워드를 쓰는 순간 창이 켜지므로 상위 워드를 먼저 써야 한다 */

#define RZG3S_PCI_MSIRCVWADRU			0x104 /* [한국어] MSI 수신 창 주소 상위 워드 */

#define RZG3S_PCI_MSIRCVWMSKL			0x108 /* [한국어] MSI 수신 창 크기 마스크 레지스터 */
#define RZG3S_PCI_MSIRCVWMSKL_MASK		GENMASK(31, 2) /* [한국어] 그 필드. hw_setup 이 벡터 수에서 1 을 뺀 값을 넣으면 하드웨어가 벡터 수 x 4바이트 창으로 해석한다 */

#define RZG3S_PCI_PINTRCVIE			0x110 /* [한국어] 상위 인터럽트 인에이블 레지스터. INTx 네 선과 MSI 가 한 워드를 나눠 쓴다 */
#define RZG3S_PCI_PINTRCVIE_INTX(i)		BIT(i) /* [한국어] 그중 i 번째 INTx 선의 비트 */
#define RZG3S_PCI_PINTRCVIE_MSI			BIT(4) /* [한국어] 그중 MSI 비트. 같은 워드를 나눠 쓰기 때문에 읽고-고쳐-쓰기에 hw_lock 이 필요하다 */

/* Register is R/W1C, it doesn't require locking. */
#define RZG3S_PCI_PINTRCVIS			0x114 /* [한국어] 상위 인터럽트 상태 레지스터. 위 상류 주석대로 R/W1C 라 락이 필요 없다 */
#define RZG3S_PCI_PINTRCVIS_INTX(i)		BIT(i) /* [한국어] 그중 i 번째 INTx 선의 비트 */
#define RZG3S_PCI_PINTRCVIS_MSI			BIT(4) /* [한국어] 그중 MSI 비트 */

#define RZG3S_PCI_MSGRCVIE			0x120 /* [한국어] 메시지 수신 인터럽트 인에이블 레지스터 */
#define RZG3S_PCI_MSGRCVIE_MSG_RCV		BIT(24) /* [한국어] 그중 메시지 수신 비트. MSI 는 메시지로 오므로 이 비트도 함께 켜야 한다 */

#define RZG3S_PCI_MSGRCVIS			0x124 /* [한국어] 메시지 수신 인터럽트 상태 레지스터 */
#define RZG3S_PCI_MSGRCVIS_MRI			BIT(24) /* [한국어] 그중 메시지 수신 상태 비트. MSI 핸들러가 벡터를 읽기 전에 지운다 */

#define RZG3S_PCI_PEIE0				0x200 /* [한국어] PCIe 이벤트 인터럽트 인에이블 0. irq_init 이 0 을 써서 모두 끈다 */

#define RZG3S_PCI_PEIS0				0x204 /* [한국어] PCIe 이벤트 인터럽트 상태 0 */
#define RZG3S_PCI_PEIS0_RX_DLLP_PM_ENTER	BIT(12) /* [한국어] PM 진입 DLLP 를 받았다는 상태 비트 */
#define RZG3S_PCI_PEIS0_DL_UPDOWN		BIT(9) /* [한국어] 데이터 링크가 오르내렸다는 상태 비트. 이 둘만 irq_init 이 명시적으로 지운다 */

#define RZG3S_PCI_PEIE1				0x208 /* [한국어] 패리티/ECC 오류 인터럽트 인에이블 */
#define RZG3S_PCI_PEIS1				0x20c /* [한국어] 같은 오류의 상태 레지스터 */
#define RZG3S_PCI_AMEIS				0x214 /* [한국어] AXI 마스터 오류 상태 레지스터 */
#define RZG3S_PCI_ASEIS1			0x224 /* [한국어] AXI 슬레이브 오류 상태 레지스터 */

#define RZG3S_PCI_PCSTAT1			0x408 /* [한국어] PCIe 링크 상태 레지스터 1 */
#define RZG3S_PCI_PCSTAT1_LTSSM_STATE		GENMASK(14, 10) /* [한국어] LTSSM 상태 필드. 속도 변경은 이 값이 L0(0xc)일 때만 할 수 있다 */
#define RZG3S_PCI_PCSTAT1_DL_DOWN_STS		BIT(0) /* [한국어] 데이터 링크가 내려가 있음을 뜻하는 비트. host_init 이 이것이 0 이 될 때까지 기다린다 */

#define RZG3S_PCI_PCCTRL2			0x410 /* [한국어] 링크 제어 레지스터 2 */
#define RZG3S_PCI_PCCTRL2_LS_CHG		GENMASK(9, 8) /* [한국어] 요청할 링크 속도 필드. LNKCTL2 의 코드에서 1 을 뺀 값이 들어간다 */
#define RZG3S_PCI_PCCTRL2_LS_CHG_REQ		BIT(0) /* [한국어] 속도 변경 요청 비트. 완료를 확인한 뒤 내려야 한다는 것이 매뉴얼 요구사항이다 */

#define RZG3S_PCI_PCSTAT2			0x414 /* [한국어] PCIe 링크 상태 레지스터 2 */
#define RZG3S_PCI_PCSTAT2_LS_CHG_DONE		BIT(28) /* [한국어] 속도 변경 완료 비트 */
#define RZG3S_PCI_PCSTAT2_SDRIRE		GENMASK(7, 1) /* [한국어] 상대 장치가 지원하는 속도들을 컨트롤러가 기록해 둔 필드 */

#define RZG3S_PCI_PERM				0x300 /* [한국어] config 공간 보호 해제 레지스터. 평소 읽기 전용인 필드를 쓰려면 먼저 열어야 한다 */
#define RZG3S_PCI_PERM_CFG_HWINIT_EN		BIT(2) /* [한국어] config 하드웨어 초기화 필드 쓰기 허용 비트(벤더/장치 ID, BAR 마스크, 클래스 코드) */
#define RZG3S_PCI_PERM_PIPE_PHY_REG_EN		BIT(1) /* [한국어] PHY 제어/상태 레지스터 접근 허용 비트 */

#define RZG3S_PCI_RESET				0x310 /* [한국어] 컨트롤러 리셋 레지스터. RZ/G3E 는 리셋 프레임워크 대신 이 비트들을 순서대로 푼다 */
#define RZG3S_PCI_RESET_RST_OUT_B		BIT(6) /* [한국어] OUT_B — 마지막 단계에서 푼다 */
#define RZG3S_PCI_RESET_RST_PS_B		BIT(5) /* [한국어] PS_B — 첫 단계에서 푼다 */
#define RZG3S_PCI_RESET_RST_LOAD_B		BIT(4) /* [한국어] LOAD_B — 설정 전에 푼다 */
#define RZG3S_PCI_RESET_RST_CFG_B		BIT(3) /* [한국어] CFG_B — 설정 전에 푼다 */
#define RZG3S_PCI_RESET_RST_RSM_B		BIT(2) /* [한국어] RSM_B — 마지막 단계에서 푼다 */
#define RZG3S_PCI_RESET_RST_GP_B		BIT(1) /* [한국어] GP_B — 첫 단계에서 푼다 */
#define RZG3S_PCI_RESET_RST_B			BIT(0) /* [한국어] RST_B — 첫 단계에서 푼다 */

#define RZG3S_PCI_MSIRE(id)			(0x600 + (id) * 0x10) /* [한국어] MSI 수신 활성 레지스터(창당 0x10 간격) */
#define RZG3S_PCI_MSIRE_ENA			BIT(0) /* [한국어] 그 활성 비트 */

#define RZG3S_PCI_MSIRM(id)			(0x608 + (id) * 0x10) /* [한국어] MSI 벡터 마스크 레지스터. mask/unmask 가 읽고-고쳐-쓰므로 hw_lock 이 필요하다 */

/* Register is R/W1C, it doesn't require locking. */
#define RZG3S_PCI_MSIRS(id)			(0x60c + (id) * 0x10) /* [한국어] MSI 벡터 상태 레지스터. 위 상류 주석대로 R/W1C 라 ack 가 락 없이 지울 수 있다 */

#define RZG3S_PCI_AWBASEL(id)			(0x1000 + (id) * 0x20) /* [한국어] 안쪽(AXI) 창 base 하위 워드 — 장치가 보는 PCI 주소. 창마다 0x20 간격이다 */
#define RZG3S_PCI_AWBASEL_WIN_ENA		BIT(0) /* [한국어] 그 워드의 창 활성 비트. 이 워드를 마지막에 써야 준비가 끝난 뒤 켜진다 */

#define RZG3S_PCI_AWBASEU(id)			(0x1004 + (id) * 0x20) /* [한국어] 안쪽 창 base 상위 워드 */
#define RZG3S_PCI_AWMASKL(id)			(0x1008 + (id) * 0x20) /* [한국어] 안쪽 창 크기 마스크 하위 워드(2^N - 1 형태) */
#define RZG3S_PCI_AWMASKU(id)			(0x100c + (id) * 0x20) /* [한국어] 안쪽 창 크기 마스크 상위 워드 */
#define RZG3S_PCI_ADESTL(id)			(0x1010 + (id) * 0x20) /* [한국어] 안쪽 창 목적지(CPU 주소) 하위 워드 */
#define RZG3S_PCI_ADESTU(id)			(0x1014 + (id) * 0x20) /* [한국어] 안쪽 창 목적지 상위 워드 */

#define RZG3S_PCI_PWBASEL(id)			(0x1100 + (id) * 0x20) /* [한국어] 바깥(PCIe) 창 base 하위 워드. 창마다 0x20 간격이다 */
#define RZG3S_PCI_PWBASEL_ENA			BIT(0) /* [한국어] 그 워드의 창 활성 비트 */

#define RZG3S_PCI_PWBASEU(id)			(0x1104 + (id) * 0x20) /* [한국어] 바깥 창 base 상위 워드 */
#define RZG3S_PCI_PDESTL(id)			(0x1110 + (id) * 0x20) /* [한국어] 바깥 창 목적지 하위 워드 */
#define RZG3S_PCI_PDESTU(id)			(0x1114 + (id) * 0x20) /* [한국어] 바깥 창 목적지 상위 워드 */
#define RZG3S_PCI_PWMASKL(id)			(0x1108 + (id) * 0x20) /* [한국어] 바깥 창 크기 마스크 하위 워드 */
#define RZG3S_PCI_PWMASKU(id)			(0x110c + (id) * 0x20) /* [한국어] 바깥 창 크기 마스크 상위 워드 */

/* PHY control registers */
#define RZG3S_PCI_PHY_XCFGD(id)			(0x2000 + (id) * 0x10) /* [한국어] PHY 디지털 설정 레지스터 배열(0x10 간격) */
#define RZG3S_PCI_PHY_XCFGD_NUM			39 /* [한국어] 그 개수. xcfgd_settings[] 배열 크기이자 루프 상한이다 */

#define RZG3S_PCI_PHY_XCFGA_CMN(id)		(0x2400 + (id) * 0x10) /* [한국어] PHY 아날로그 공통 설정 레지스터 배열 */
#define RZG3S_PCI_PHY_XCFGA_CMN_NUM		16 /* [한국어] 그 개수 */

#define RZG3S_PCI_PHY_XCFGA_RX(id)		(0x2500 + (id) * 0x10) /* [한국어] PHY 아날로그 수신 설정 레지스터 배열 */
#define RZG3S_PCI_PHY_XCFGA_RX_NUM		13 /* [한국어] 그 개수 */

#define RZG3S_PCI_PHY_XCFGA_TX			0x25d0 /* [한국어] PHY 아날로그 송신 설정 레지스터. 하나뿐이라 인덱스가 없다 */

#define RZG3S_PCI_PHY_XCFG_CTRL			0x2a20 /* [한국어] PHY 설정 제어 레지스터 */
#define RZG3S_PCI_PHY_XCFG_CTRL_PHYREG_SEL	BIT(0) /* [한국어] 써 넣은 값을 실제로 쓰라고 지시하는 선택 비트. 값을 모두 적은 뒤 마지막에 세운다 */

/* PCIe registers */
#define RZG3S_PCI_CFG_BASE			0x6000 /* [한국어] AXI 창 안에서 RC 자신의 config 공간이 시작하는 오프셋. probe 가 host->pcie 를 이 값으로 계산한다 */
#define RZG3S_PCI_CFG_BARMSK00L			0xa0 /* [한국어] RC config 공간 안의 BAR 마스크 하위 워드. 매뉴얼 권고대로 0xffffffff 를 적는다 */
#define RZG3S_PCI_CFG_BARMSK00U			0xa4 /* [한국어] 같은 BAR 마스크 상위 워드 */

#define RZG3S_PCI_CFG_PCIEC			0x60 /* [한국어] RC config 공간 안에서 PCIe capability 가 시작하는 오프셋. 속도 조정이 여기에 LNKSTA/LNKCAP/LNKCTL2 를 더해 접근한다 */

/* Maximum number of windows */
#define RZG3S_MAX_WINDOWS			8 /* [한국어] 안쪽·바깥쪽 창 각각의 최대 개수 */

/* Number of MSI interrupts per register */
#define RZG3S_PCI_MSI_INT_PER_REG		32 /* [한국어] 상태 레지스터 하나가 담는 MSI 벡터 수 */
/* The number of MSI interrupts */
#define RZG3S_PCI_MSI_INT_NR			RZG3S_PCI_MSI_INT_PER_REG /* [한국어] 전체 MSI 벡터 수. 지금은 레지스터 하나 분량과 같아 루프가 한 번만 돈다 */

/* Timeouts experimentally determined */
#define RZG3S_REQ_ISSUE_TIMEOUT_US		2500 /* [한국어] 요청 발행 완료를 기다리는 상한(us). 위 상류 주석대로 실험으로 정한 값이다 */

/**
 * struct rzg3s_sysc_function - System Controller function descriptor
 * @offset: Register offset from the System Controller base address
 * @mask: Bit mask for the function within the register
 */
/* [한국어] SYSC 레지스터 안에서 기능 하나가 놓인 자리.
 * 같은 기능이 SoC 마다 다른 레지스터·비트에 있어, 좌표를 코드에 박지 않고
 * 이 서술자의 배열로 표현한다. 전부 const 정적 인스턴스라 동기화가 없다. */
struct rzg3s_sysc_function {
		/* [한국어] SYSC 기준 주소에서의 레지스터 오프셋(상류 주석 그대로).
		 * 설정자: 파일 끝의 rzg3s_soc_data / rzg3e_soc_data 안의 sysc_info.
		 * 읽는 자: rzg3s_sysc_config_func() 이 regmap_update_bits 의 인자로 쓴다.
		 * 값 범위: SYSC 블록 안의 오프셋(예: 0xd74, 0x1020, 0x1024).
		 * 동기화: 읽기 전용 상수. */
	u32 offset;
		/* [한국어] 그 레지스터 안에서 이 기능이 차지하는 비트 마스크.
		 * 설정자: 위와 같다.
		 * 읽는 자: rzg3s_sysc_config_func() 이 마스크이자 "이 SoC 에 이 기능이
		 *          있는가" 의 판정에 쓴다.
		 * 값 범위: 0 이면 그 SoC 에 그 기능이 없다는 뜻이라 함수가 아무 일도 하지
		 *          않고 성공으로 돌아간다 — 채워지지 않은 배열 원소가 자동으로
		 *          그 상태가 된다.
		 * 동기화: 읽기 전용 상수. */
	u32 mask;
};

/**
 * enum rzg3s_sysc_func_id - System controller function IDs
 * @RZG3S_SYSC_FUNC_ID_RST_RSM_B: RST_RSM_B SYSC function ID
 * @RZG3S_SYSC_FUNC_ID_L1_ALLOW: L1 allow SYSC function ID
 * @RZG3S_SYSC_FUNC_ID_MODE: Mode SYSC function ID
 * @RZG3S_SYSC_FUNC_ID_MAX: Max SYSC function ID
 */
/* [한국어] 이 드라이버가 SYSC 를 통해 건드리는 기능들의 식별자.
 * rzg3s_sysc_info.functions[] 배열의 인덱스로 쓰이므로, 순서가 곧 표의
 * 자리다. 마지막 원소가 배열 크기를 겸한다.
 *
 * 설정자: 없음(컴파일 시점 상수).
 * 읽는 자: rzg3s_sysc_config_func() 의 인자와 SoC 표의 지정 초기화. */
enum rzg3s_sysc_func_id {
		/* [한국어] PHY 전원과 얽힌 리셋 신호(상류 주석의 RST_RSM_B).
		 * RZ/G3S 표만 이 자리를 채운다. probe/resume 이 1 로, suspend 와 probe 의
		 * 실패 경로가 0 으로 쓴다 — PHY 전원을 끄기 전에 반드시 0 이어야 한다. */
	RZG3S_SYSC_FUNC_ID_RST_RSM_B,
		/* [한국어] ASPM L1 전이를 허용하는 비트.
		 * RZ/G3E 표만 이 자리를 채운다. rzg3s_pcie_host_init() 이 1 로 쓴다. */
	RZG3S_SYSC_FUNC_ID_L1_ALLOW,
		/* [한국어] 컨트롤러를 RC 로 둘지 EP 로 둘지 고르는 비트.
		 * RZ/G3E 표만 이 자리를 채운다. probe 와 resume 이 1(RC)로 쓴다.
		 * RZ/G3S 는 이 자리가 비어 있어 그 호출이 아무 일도 하지 않는다. */
	RZG3S_SYSC_FUNC_ID_MODE,
		/* [한국어] 원소 개수를 겸하는 끝 표시.
		 * rzg3s_sysc_info.functions[] 의 크기이자, rzg3s_sysc_config_func() 의
		 * 범위 검사 상한이다. */
	RZG3S_SYSC_FUNC_ID_MAX,
};

/**
 * struct rzg3s_sysc_info - RZ/G3S System Controller info
 * @functions: SYSC function descriptors array
 */
/* [한국어] 한 SoC 의 SYSC 좌표표 전체.
 * struct rzg3s_pcie_soc_data 안에 값으로 박혀 있어, SoC 표를 고르는 것만으로
 * SYSC 좌표까지 함께 정해진다. */
struct rzg3s_sysc_info {
		/* [한국어] 기능 식별자를 인덱스로 쓰는 좌표 배열(상류 주석의 서술자 배열).
		 * 설정자: 파일 끝의 두 SoC 표가 지정 초기화로 필요한 자리만 채운다.
		 * 읽는 자: rzg3s_sysc_config_func().
		 * 값 범위: 채우지 않은 원소는 mask 가 0 이라 "없는 기능" 으로 동작한다.
		 * 동기화: 읽기 전용 상수. */
	const struct rzg3s_sysc_function functions[RZG3S_SYSC_FUNC_ID_MAX];
};

/**
 * struct rzg3s_sysc - RZ/G3S System Controller descriptor
 * @regmap: System controller regmap
 * @info: System controller info
 */
/* [한국어] 런타임에 SYSC 를 다루기 위한 서술자.
 * probe 가 devm 으로 잡아 host->sysc 에 매달고, regmap 과 SoC 표의 좌표를
 * 한데 묶는다. */
struct rzg3s_sysc {
		/* [한국어] SYSC 레지스터에 접근할 regmap(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 가 DT 의 "renesas,sysc" phandle 을
		 *          syscon_node_to_regmap() 으로 열어 넣는다.
		 * 읽는 자: rzg3s_sysc_config_func().
		 * 값 범위: IS_ERR 이면 probe 가 그 자리에서 실패하므로 이후에는 늘 유효.
		 * 동기화: regmap 이 자체 락을 갖는다. */
	struct regmap *regmap;
		/* [한국어] 이 SoC 의 SYSC 좌표표를 가리킨다.
		 * 설정자: rzg3s_pcie_probe() 가 &host->data->sysc_info 를 넣는다.
		 * 읽는 자: rzg3s_sysc_config_func().
		 * 값 범위: NULL 불가 — SoC 표 안에 값으로 들어 있는 필드의 주소다.
		 * 동기화: 읽기 전용 상수를 가리킨다. */
	const struct rzg3s_sysc_info *info;
};

/**
 * struct rzg3s_pcie_msi - RZ/G3S PCIe MSI data structure
 * @domain: IRQ domain
 * @map: bitmap with the allocated MSIs
 * @dma_addr: address of the allocated MSI window
 * @window_base: base address of the MSI window
 * @pages: allocated pages for MSI window mapping
 * @map_lock: lock for bitmap with the allocated MSIs
 * @irq: MSI interrupt
 */
/* [한국어] 이 컨트롤러의 MSI 상태 전부.
 * struct rzg3s_pcie_host 안에 값으로 박혀 있어, rzg3s_msi_to_host 매크로가
 * container_of 로 바깥 host 를 되찾는다. irq_chip 콜백들이 chip_data 로
 * 이 구조체를 받기 때문에 그 되찾기가 필요하다. */
struct rzg3s_pcie_msi {
		/* [한국어] MSI IRQ 도메인(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_msi_allocate_domains().
		 * 읽는 자: rzg3s_pcie_msi_irq() 가 generic_handle_domain_irq() 에 넘기고,
		 *          teardown/실패 경로가 irq_domain_remove() 로 없앤다.
		 * 값 범위: NULL 이면 생성 실패라 probe 가 거기서 멈춘다.
		 * 동기화: probe 에서 한 번 만들고 remove 까지 바뀌지 않는다. */
	struct irq_domain *domain;
		/* [한국어] 어느 MSI 벡터가 쓰이고 있는지 표시하는 비트맵(상류 주석 그대로).
		 * 설정자/읽는 자: rzg3s_pcie_msi_domain_alloc() 과 _free() 만 다룬다.
		 * 값 범위: 비트 수가 RZG3S_PCI_MSI_INT_NR(32)로 고정.
		 * 동기화: 아래 map_lock 뮤텍스가 지킨다. */
	DECLARE_BITMAP(map, RZG3S_PCI_MSI_INT_NR);
		/* [한국어] MSI 목적지로 잡은 페이지의 DMA 주소(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_msi_setup() 의 dma_map_single().
		 * 읽는 자: 같은 함수가 AXI 창 안에 드는지 확인하고 정렬해 window_base 를
		 *          만들며, teardown 이 dma_unmap_single() 의 인자로 쓴다.
		 * 값 범위: 매핑된 크기는 창 크기의 두 배다 — 아래 window_base 가 정렬
		 *          때문에 뒤로 밀릴 수 있어 그만큼 여유를 둔다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	dma_addr_t dma_addr;
		/* [한국어] 실제로 하드웨어에 알려 줄 MSI 창의 시작 주소(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_msi_setup() 이 dma_addr 를 창 크기 경계로 올려 만든다.
		 * 읽는 자: rzg3s_pcie_msi_hw_setup() 이 주소 레지스터에 적는다.
		 * 값 범위: 창 크기의 배수. 정렬 계산이 넘쳐 dma_addr 보다 작아지면
		 *          setup 이 -EINVAL 로 돌아간다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다 — resume 이 하드웨어만 다시
		 *          세울 수 있는 것도 이 값이 남아 있기 때문이다. */
	dma_addr_t window_base;
		/* [한국어] MSI 창으로 쓰려고 잡은 페이지의 커널 주소(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_msi_setup() 의 __get_free_pages().
		 * 읽는 자: 같은 함수의 DMA 매핑과 실패 경로, 그리고 teardown 의 free_pages().
		 * 값 범위: 0 이면 할당 실패. 페이지 하나(order 0)만 잡는다.
		 * 동기화: probe 에서 잡고 remove 까지 바뀌지 않는다. */
	unsigned long pages;
		/* [한국어] 위 비트맵을 지키는 뮤텍스(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_init_msi() 의 devm_mutex_init().
		 * 읽는 자: alloc 은 scoped_guard 로 비트맵 조작만 감싸고, free 는
		 *          guard 로 함수 전체를 감싼다.
		 * 값 범위: 잠들 수 있는 락. 벡터 할당이 프로세스 컨텍스트에서만
		 *          일어나므로 스핀락일 필요가 없다.
		 * 동기화: 이 락이 지키는 대상은 map 비트맵 하나뿐이다. */
	struct mutex map_lock;
		/* [한국어] MSI 를 물어 오는 인터럽트 번호(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_init_msi() 가 DT 에서 "msi" 이름으로 얻는다.
		 * 읽는 자: request_irq/free_irq 의 인자.
		 * 값 범위: 음수면 DT 에 그 인터럽트가 없다는 뜻이라 probe 가 실패한다.
		 * 동기화: 바뀌지 않는다. */
	int irq;
};

/* [한국어] struct rzg3s_pcie_host 의 전방 선언.
 * 바로 아래 struct rzg3s_pcie_soc_data 의 함수 포인터들이 host 를 인자로
 * 받는데, host 자신은 soc_data 포인터를 필드로 갖는다. 서로를 참조하므로
 * 한쪽을 먼저 이름만 알려 두어야 한다. */
struct rzg3s_pcie_host;

/**
 * struct rzg3s_pcie_soc_data - SoC specific data
 * @init_phy: PHY initialization function
 * @config_pre_init: Optional callback for SoC-specific pre-configuration
 * @config_post_init: Callback for SoC-specific post-configuration
 * @config_deinit: Callback for SoC-specific de-initialization
 * @power_resets: array with the resets that need to be de-asserted after
 *                power-on
 * @cfg_resets: array with the resets that need to be de-asserted after
 *              configuration
 * @sysc_info: SYSC info
 * @num_power_resets: number of power resets
 * @num_cfg_resets: number of configuration resets
 */
/* [한국어] SoC 하나마다 있는 상수 표.
 * 이 드라이버의 SoC 차이가 전부 이 구조체를 통해 흡수된다. probe 가
 * device_get_match_data() 로 골라 host->data 에 꽂아 두면, 그 뒤로는 코드가
 * SoC 이름을 직접 알 필요가 없다.
 *
 * 전부 const 정적 인스턴스라 런타임에 바뀌지 않으며, 어느 필드도 동기화가
 * 필요 없다. */
struct rzg3s_pcie_soc_data {
		/* [한국어] PHY 초기화 함수(상류 주석 그대로).
		 * 설정자: RZ/G3S 표만 rzg3s_soc_pcie_init_phy 를 넣는다.
		 * 읽는 자: rzg3s_pcie_host_init_port() 가 NULL 확인 후 부른다.
		 * 값 범위: NULL 이면 소프트웨어가 채울 PHY 설정이 없다는 뜻(RZ/G3E). */
	int (*init_phy)(struct rzg3s_pcie_host *host);
		/* [한국어] 설정 전에 할 SoC 전용 작업(상류 주석대로 선택적).
		 * 설정자: RZ/G3E 표만 rzg3e_pcie_config_pre_init 을 넣는다.
		 * 읽는 자: rzg3s_pcie_host_init() 이 NULL 확인 후 부르고,
		 *          rzg3s_pcie_suspend_noirq() 의 되돌리기도 같은 확인을 한다.
		 * 값 범위: NULL 이면 그 단계가 없다는 뜻. rzg3s_pcie_host_init() 의
		 *          마지막 되돌리기 조건으로도 쓰인다. */
	void (*config_pre_init)(struct rzg3s_pcie_host *host);
		/* [한국어] 설정 후에 할 SoC 전용 작업 — 실질적으로 리셋 해제다.
		 * 설정자: 두 SoC 표 모두 채운다(리셋 프레임워크 판과 레지스터 비트 판).
		 * 읽는 자: rzg3s_pcie_host_init() 과 suspend 의 되돌리기.
		 * 값 범위: NULL 불가 — 호출자가 확인 없이 부른다. */
	int (*config_post_init)(struct rzg3s_pcie_host *host);
		/* [한국어] 설정을 되돌리는 SoC 전용 작업.
		 * 설정자: 두 SoC 표 모두 채운다.
		 * 읽는 자: rzg3s_pcie_host_init() 의 되돌리기, suspend, probe 의 실패 경로.
		 * 값 범위: NULL 불가. RZ/G3E 판은 여러 번 불려도 결과가 같다. */
	int (*config_deinit)(struct rzg3s_pcie_host *host);
		/* [한국어] 전원 인가 후 풀어야 할 리셋들의 이름 배열(상류 주석 그대로).
		 * 설정자: 두 SoC 표. RZ/G3S 는 셋, RZ/G3E 는 하나다.
		 * 읽는 자: rzg3s_pcie_resets_prepare_and_get() 이 bulk 배열의 id 로 옮긴다.
		 * 값 범위: DT 의 reset-names 에 있는 이름들. exclusive 로 잡으므로
		 *          없으면 probe 가 실패한다. */
	const char * const *power_resets;
		/* [한국어] 설정 후에 풀어야 할 리셋들의 이름 배열(상류 주석 그대로).
		 * 설정자: RZ/G3S 표만 채운다. RZ/G3E 는 같은 일을 RESET 레지스터 비트로
		 *          하므로 이 목록이 비어 있고 개수도 0 이다.
		 * 읽는 자: 위와 같다. optional 판으로 잡으므로 비어 있어도 문제없다.
		 * 값 범위: NULL 이면 num_cfg_resets 도 0 이라 루프가 돌지 않는다. */
	const char * const *cfg_resets;
		/* [한국어] 이 SoC 의 SYSC 좌표표(상류 주석 그대로).
		 * 포인터가 아니라 값으로 박혀 있어, probe 가 그 주소를 host->sysc->info 에
		 * 넣는다.
		 * 읽는 자: rzg3s_sysc_config_func().
		 * 값 범위: 채우지 않은 기능은 mask 가 0 이라 자동으로 "없음" 이 된다. */
	struct rzg3s_sysc_info sysc_info;
		/* [한국어] power_resets 배열의 원소 수(상류 주석 그대로).
		 * 설정자: 각 SoC 표가 ARRAY_SIZE 로 채운다.
		 * 읽는 자: 배열 할당과 bulk API 의 개수 인자.
		 * 값 범위: 1 이상. u8 로 충분한 작은 수다. */
	u8 num_power_resets;
		/* [한국어] cfg_resets 배열의 원소 수(상류 주석 그대로).
		 * 설정자: RZ/G3S 표만 채우고 RZ/G3E 표는 채우지 않아 0 이 된다.
		 * 읽는 자: 위와 같다.
		 * 값 범위: 0 이면 그 묶음이 없다는 뜻이고, optional 조회라 문제없다. */
	u8 num_cfg_resets;
};

/**
 * struct rzg3s_pcie_port - RZ/G3S PCIe Root Port data structure
 * @refclk: PCIe reference clock
 * @vendor_id: Vendor ID
 * @device_id: Device ID
 */
/* [한국어] 루트 포트 하나의 정보.
 * DT 의 자식 노드에서 읽어 오며, struct rzg3s_pcie_host 안에 값으로 박혀 있다. */
struct rzg3s_pcie_port {
		/* [한국어] PCIe 기준 클럭(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_host_parse_port() 가 of_clk_get_by_name() 으로 잡는다.
		 *          devm 판이 아니라는 점이 중요하다 — 그래서 probe 의 실패 경로와
		 *          suspend 가 직접 clk_put/clk_disable_unprepare 를 부른다.
		 * 읽는 자: rzg3s_pcie_host_init_port() 가 켜고, suspend 와 오류 경로가 끈다.
		 * 값 범위: IS_ERR 이면 probe 가 그 자리에서 실패한다.
		 * 동기화: PM 코어가 suspend/resume 을 직렬화한다. */
	struct clk *refclk;
		/* [한국어] RC 의 config 공간에 써 넣을 벤더 ID(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_host_parse_port() 가 DT 의 "vendor-id" 에서 읽는다.
		 * 읽는 자: rzg3s_pcie_host_init_port() 가 PERM 문을 연 뒤 적는다.
		 * 값 범위: 16비트 값이지만 DT 읽기 API 에 맞춰 u32 로 담는다.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	u32 vendor_id;
		/* [한국어] RC 의 config 공간에 써 넣을 장치 ID(상류 주석 그대로).
		 * 설정자/읽는 자/값 범위/동기화 모두 위 vendor_id 와 같다. */
	u32 device_id;
};

/**
 * struct rzg3s_pcie_host - RZ/G3S PCIe data structure
 * @axi: base address for AXI registers
 * @pcie: base address for PCIe registers
 * @dev: struct device
 * @power_resets: reset control signals that should be set after power up
 * @cfg_resets: reset control signals that should be set after configuration
 * @sysc: SYSC descriptor
 * @intx_domain: INTx IRQ domain
 * @data: SoC specific data
 * @msi: MSI data structure
 * @port: PCIe Root Port
 * @hw_lock: lock for access to the HW resources
 * @intx_irqs: INTx interrupts
 * @max_link_speed: maximum supported link speed
 */
/* [한국어] 컨트롤러 하나의 모든 상태.
 * devm_pci_alloc_host_bridge() 가 브리지와 함께 잡아 주는 private 영역에
 * 얹히므로, pci_host_bridge_from_priv() 로 브리지를, bus->sysdata 로 이
 * 구조체를 서로 되찾을 수 있다. 전역 가변 상태는 없고, 인스턴스마다 독립적이다. */
struct rzg3s_pcie_host {
		/* [한국어] AXI 레지스터 창의 시작 주소(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 의 devm_platform_ioremap_resource().
		 * 읽는 자: 이 파일의 거의 모든 readl/writel 이 여기에 오프셋을 더한다.
		 * 값 범위: IS_ERR 이면 probe 가 그 자리에서 돌아간다.
		 * 동기화: 바뀌지 않는다. */
	void __iomem *axi;
		/* [한국어] RC 자신의 PCI config 공간 시작 주소(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 가 axi + RZG3S_PCI_CFG_BASE 로 계산한다.
		 *          별도의 DT 자원이 아니라 같은 창 안의 오프셋이다.
		 * 읽는 자: rzg3s_pcie_root_map_bus(), config_init, host_init_port,
		 *          set_max_link_speed.
		 * 값 범위: axi 가 유효하면 함께 유효하다.
		 * 동기화: 바뀌지 않는다. */
	void __iomem *pcie;
		/* [한국어] 플랫폼 device(상류 주석 그대로). 로그와 devm 할당의 기준이다.
		 * 설정자: rzg3s_pcie_probe(). 읽는 자: 거의 모든 함수.
		 * 값 범위: NULL 불가.
		 * 동기화: 바뀌지 않는다. */
	struct device *dev;
		/* [한국어] 전원 인가 후 풀 리셋들의 bulk 배열(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_resets_prepare_and_get() 이 devm 으로 잡고 이름을
		 *          채운 뒤 exclusive 로 조회한다.
		 * 읽는 자: rzg3s_pcie_power_resets_deassert(), suspend, probe 실패 경로.
		 * 값 범위: NULL 이면 할당 실패라 probe 가 거기서 멈춘다.
		 * 동기화: 리셋 프레임워크가 관리한다. */
	struct reset_control_bulk_data *power_resets;
		/* [한국어] 설정 후 풀 리셋들의 bulk 배열(상류 주석 그대로).
		 * 설정자: 위와 같으나 optional 판으로 조회한다.
		 * 읽는 자: rzg3s_pcie_config_post_init() 과 rzg3s_pcie_config_deinit()
		 *          — 즉 RZ/G3S 경로에서만 쓰인다.
		 * 값 범위: RZ/G3E 는 개수가 0 이라 내용이 비어 있다.
		 * 동기화: 리셋 프레임워크가 관리한다. */
	struct reset_control_bulk_data *cfg_resets;
		/* [한국어] SYSC 서술자(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 가 devm 으로 잡아 regmap 과 info 를 채운다.
		 * 읽는 자: rzg3s_sysc_config_func() 을 부르는 모든 곳.
		 * 값 범위: NULL 이면 probe 가 -ENOMEM 으로 돌아간다.
		 * 동기화: regmap 이 자체 락을 갖는다. */
	struct rzg3s_sysc *sysc;
		/* [한국어] INTx 용 IRQ 도메인(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_init_irqdomain() 의 irq_domain_create_linear().
		 * 읽는 자: rzg3s_pcie_intx_irq_handler() 가 벡터를 넘길 때,
		 *          teardown 이 없앨 때.
		 * 값 범위: NULL 이면 생성 실패라 probe 가 -EINVAL 로 돌아간다.
		 * 동기화: probe 에서 만들고 remove 까지 바뀌지 않는다. */
	struct irq_domain *intx_domain;
		/* [한국어] 이 SoC 의 상수 표(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 의 device_get_match_data().
		 * 읽는 자: SoC 분기가 있는 거의 모든 함수.
		 * 값 범위: 매칭 표가 반드시 데이터를 주므로 유효하다.
		 * 동기화: 읽기 전용 상수를 가리킨다. */
	const struct rzg3s_pcie_soc_data *data;
		/* [한국어] MSI 상태(상류 주석 그대로). 포인터가 아니라 값으로 박혀 있다.
		 * 그래서 rzg3s_msi_to_host 매크로가 container_of 로 이 필드에서 바깥
		 * host 를 되찾을 수 있고, irq_chip 콜백들이 그 방식을 쓴다.
		 * 설정자/읽는 자: MSI 관련 함수 전부.
		 * 동기화: 안의 map_lock 이 비트맵을, hw_lock 이 마스크 레지스터를 지킨다. */
	struct rzg3s_pcie_msi msi;
		/* [한국어] 루트 포트 정보(상류 주석 그대로). 역시 값으로 박혀 있다.
		 * 설정자: rzg3s_pcie_host_parse_port().
		 * 읽는 자: rzg3s_pcie_host_init_port(), suspend/resume, probe 실패 경로.
		 * 동기화: probe 에서 채우고 바뀌지 않는다(클럭 상태만 PM 이 바꾼다). */
	struct rzg3s_pcie_port port;
		/* [한국어] 하드웨어 자원 접근을 지키는 스핀락(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 가 인터럽트를 걸기 직전에 초기화한다.
		 * 읽는 자: 읽고-고쳐-쓰기가 필요한 인터럽트 인에이블/마스크 레지스터를
		 *          다루는 네 콜백 — MSI 의 mask/unmask 와 INTx 의 mask/unmask.
		 * 값 범위: raw 스핀락인 것은 인터럽트 문맥에서 잡히기 때문이다.
		 *          guard(raw_spinlock_irqsave) 로만 쓰여 놓기를 빠뜨릴 수 없다.
		 * 동기화: R/W1C 상태 레지스터는 이 락이 필요 없으며, 그 사실이 해당
		 *          레지스터 정의 위에 상류 주석으로 적혀 있다. */
	raw_spinlock_t hw_lock;
		/* [한국어] INTA~INTD 네 선의 인터럽트 번호(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_init_irqdomain() 이 DT 에서 "inta"~"intd" 순으로 넣는다.
		 * 읽는 자: rzg3s_pcie_intx_irq_handler() 가 [0] 을 기준으로 빼서 어느
		 *          선인지 알아낸다 — 그래서 네 번호가 연속이라는 가정이 깔려 있다.
		 * 값 범위: 음수면 DT 에 그 선이 없다는 뜻이라 probe 가 실패한다.
		 * 동기화: probe 에서 채우고 바뀌지 않는다. */
	int intx_irqs[PCI_NUM_INTX];
		/* [한국어] DT 가 지정한 최대 링크 속도(상류 주석 그대로).
		 * 설정자: rzg3s_pcie_probe() 의 of_pci_get_max_link_speed().
		 * 읽는 자: rzg3s_pcie_set_max_link_speed() 가 상한으로만 쓴다 — 0 이하
		 *          이거나 하드웨어 능력보다 크면 무시하고 하드웨어 값을 쓴다.
		 * 값 범위: 속성이 없으면 음수. 있으면 PCIe 세대 번호.
		 * 동기화: probe 에서 정하고 바뀌지 않는다. */
	int max_link_speed;
};

/* [한국어] MSI 상태 포인터에서 바깥 컨트롤러 상태를 되찾는 매크로.
 * MSI irq_chip 콜백들이 chip_data 로 &host->msi 를 받기 때문에, 레지스터에
 * 닿으려면 한 단계 거슬러 올라가야 한다. msi 가 host 안에 값으로 박혀 있어
 * container_of 가 성립한다. */
#define rzg3s_msi_to_host(_msi)	container_of(_msi, struct rzg3s_pcie_host, msi)

/* [한국어]
 * rzg3s_sysc_config_func - SYSC(시스템 컨트롤러)의 한 기능 비트를 쓴다
 *
 * @sysc: SYSC regmap 과 좌표표를 담은 서술자.
 * @fid:  건드릴 기능(enum rzg3s_sysc_func_id).
 * @val:  넣을 값. 지금은 모두 0/1 로만 쓰인다.
 * @return: 0 성공. 범위를 벗어난 fid 면 -EINVAL, 그 밖에는 regmap 오류.
 *
 * SYSC 는 PCIe 컨트롤러 바깥에 있는 SoC 전역 레지스터 블록이다. RC/EP 모드
 * 선택, ASPM L1 허용, 그리고 PHY 전원과 얽힌 RST_RSM_B 신호가 거기 있어,
 * 이 파일이 자기 레지스터 창이 아니라 syscon regmap 을 통해 건드린다.
 *
 * 좌표를 상수로 박지 않고 SoC 표(rzg3s_sysc_info.functions[])에서 꺼내는
 * 것이 요점이다. 같은 기능이 SoC 마다 다른 레지스터·비트에 있고, 아예 없는
 * SoC 도 있기 때문이다.
 *
 * mask 가 0 이면 "이 SoC 에는 그 기능이 없다" 는 뜻이라 아무 일도 하지 않고
 * 0 을 돌려준다. 그래서 호출자가 SoC 를 가리지 않고 그냥 부를 수 있다 —
 * 실제로 RZ/G3S 표는 RST_RSM_B 만, RZ/G3E 표는 L1_ALLOW 와 MODE 만 채워 둔다.
 *
 * 값을 넣을 때 대문자 FIELD_PREP 가 아니라 소문자 field_prep 를 쓴다.
 * 마스크가 표에서 런타임에 꺼내 오는 값이라 컴파일 시점 상수가 아니기
 * 때문으로 보이나, 두 매크로의 정확한 차이는 이 트리에 include/linux/bitfield.h
 * 가 없어 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/PM). regmap 접근이 잠들 수 있다.
 *
 * 호출 체인:  rzg3s_pcie_probe() / rzg3s_pcie_host_init() /
 *               rzg3s_pcie_suspend_noirq() / rzg3s_pcie_resume_noirq()
 *               → [이 함수] → regmap_update_bits()
 */
static int rzg3s_sysc_config_func(struct rzg3s_sysc *sysc,
				  enum rzg3s_sysc_func_id fid, u32 val)
{
	const struct rzg3s_sysc_info *info = sysc->info; /* [한국어] SoC 표가 담고 있는 SYSC 좌표표 */
	const struct rzg3s_sysc_function *functions = info->functions; /* [한국어] 그 안의 기능별 좌표 배열을 꺼내 둔다 */

	if (fid >= RZG3S_SYSC_FUNC_ID_MAX) /* [한국어] 표 밖의 식별자면 */
		return -EINVAL; /* [한국어] 배열을 넘어서 읽지 않도록 여기서 막는다 */

	if (!functions[fid].mask) /* [한국어] 마스크가 0 이면 이 SoC 에 그 기능이 없다는 뜻 */
		return 0; /* [한국어] 할 일이 없으므로 성공으로 돌아간다 — 호출자가 SoC 를 가리지 않아도 되는 이유다 */

	return regmap_update_bits(sysc->regmap, functions[fid].offset,
				  functions[fid].mask,
				  field_prep(functions[fid].mask, val)); /* [한국어] 표에서 꺼낸 오프셋과 마스크로 SYSC 레지스터의 그 비트만 갱신한다. 마스크가 런타임 값이라 소문자 field_prep 를 쓴다 */
}

/* [한국어]
 * rzg3s_pcie_update_bits - 레지스터의 일부 비트만 바꿔 쓴다
 *
 * @base:   레지스터 창의 기준 주소(host->axi 또는 host->pcie).
 * @offset: 그 창 안의 오프셋.
 * @mask:   바꿀 비트들.
 * @val:    넣을 값. 마스크 밖 비트는 무시된다.
 *
 * 이 파일 전체가 쓰는 읽고-고쳐-쓰기 헬퍼다. 인자로 base 를 받기 때문에
 * AXI 레지스터와 RC 의 config 공간 양쪽에 쓸 수 있다.
 *
 * val 을 마스크로 한 번 더 거르므로, 호출자가 넘긴 값이 마스크를 넘쳐도
 * 다른 필드를 망가뜨리지 않는다.
 *
 * readl_relaxed/writel_relaxed 를 쓴다. 이 컨트롤러의 레지스터 접근에는
 * 서로 간의 순서 제약이 없고, 순서가 필요한 지점에서는 호출자 쪽이
 * 별도의 더미 읽기로 밀어낸다(rzg3e_pcie_config_post_init 이 그 예다).
 *
 * 이 함수 자체는 락을 잡지 않는다. 경쟁이 있는 레지스터(인터럽트 인에이블
 * 계열)를 다루는 호출자가 host->hw_lock 으로 감싸며, R/W1C 라 경쟁이 없는
 * 상태 레지스터는 감싸지 않는다 — 그 사실이 해당 레지스터 정의 위에
 * 상류 주석으로 적혀 있다.
 *
 * 실행 컨텍스트: 제약 없음. 인터럽트 문맥에서도 불린다.
 *
 * 호출 체인:  이 파일의 거의 모든 초기화·인터럽트 함수 → [이 함수]
 */
static void rzg3s_pcie_update_bits(void __iomem *base, u32 offset, u32 mask,
				   u32 val)
{
	u32 tmp; /* [한국어] 읽어서 고칠 임시 값 */

	tmp = readl_relaxed(base + offset); /* [한국어] 다른 비트를 보존하려면 먼저 읽어야 한다 */
	tmp &= ~mask; /* [한국어] 바꿀 자리를 비운다 */
	tmp |= val & mask; /* [한국어] 새 값을 그 자리에 넣는다. 마스크로 한 번 더 걸러 이웃 필드를 침범하지 않는다 */
	writel_relaxed(tmp, base + offset); /* [한국어] 되쓴다. 이 읽고-고쳐-쓰기가 경쟁하는 레지스터는 호출자가 hw_lock 으로 감싼다 */
}

/* [한국어]
 * rzg3s_pcie_child_issue_request - 준비해 둔 config 요청을 발행하고 완료를 기다린다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 완료 비트가 시간 안에 내려가지 않으면 폴링 오류,
 *          하드웨어가 오류 상태를 보고하면 -EIO.
 *
 * 이 컨트롤러에는 하위 버스용 ECAM 창이 없다. 대신 주소·바이트 인에이블·
 * 요청 종류를 레지스터에 미리 적어 두고, 이 함수가 REQ_ISSUE 비트를 세워
 * "보내라" 고 지시한 뒤 그 비트가 스스로 내려갈 때까지 기다린다.
 *
 * 기다림이 readl_poll_timeout_atomic 인 것이 중요하다. config 접근은
 * 인터럽트 문맥에서도 불릴 수 있어 잠들면 안 되기 때문이다. 5us 간격으로
 * RZG3S_REQ_ISSUE_TIMEOUT_US 까지 기다리는데, 그 상한은 상수 정의 위의
 * 상류 주석대로 실험으로 정한 값이다.
 *
 * 판정 순서를 눈여겨볼 만하다. 폴링 결과보다 MOR_STATUS 필드를 먼저 본다.
 * 그 필드가 0 이 아니면 요청은 끝났지만 대상이 오류로 답한 것이라 -EIO 다.
 * 타임아웃이면 val 에 마지막으로 읽은 값이 남아 있고, 그 값에 오류 표시가
 * 없으면 폴링 오류가 그대로 올라간다.
 *
 * 락이 없다. 이 레지스터 묶음은 컨트롤러에 하나뿐인 공유 자원이지만,
 * config 접근을 PCI 코어의 pci_lock 이 직렬화한다 — 상류가 read/write
 * 콜백 위에 그 사실을 주석으로 남겨 두었다.
 *
 * 실행 컨텍스트: 아토믹. 바쁘게 기다린다.
 *
 * 호출 체인:  rzg3s_pcie_child_read_conf() / rzg3s_pcie_child_write_conf()
 *               → [이 함수]
 */
static int rzg3s_pcie_child_issue_request(struct rzg3s_pcie_host *host)
{
	u32 val; /* [한국어] 폴링이 읽어 담을 상태 워드 */
	int ret; /* [한국어] 폴링 결과 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_REQISS,
			       RZG3S_PCI_REQISS_REQ_ISSUE,
			       RZG3S_PCI_REQISS_REQ_ISSUE); /* [한국어] 요청 발행 비트를 세워 하드웨어에 "보내라" 고 지시한다 */
	ret = readl_poll_timeout_atomic(host->axi + RZG3S_PCI_REQISS, val,
					!(val & RZG3S_PCI_REQISS_REQ_ISSUE),
					5, RZG3S_REQ_ISSUE_TIMEOUT_US); /* [한국어] 그 비트가 스스로 내려갈 때까지 5us 간격으로 폴링한다. 잠들 수 없는 문맥이라 atomic 판이다 */

	if (val & RZG3S_PCI_REQISS_MOR_STATUS) /* [한국어] 폴링 결과보다 먼저 본다 — 요청이 끝났더라도 대상이 오류로 답했을 수 있다 */
		return -EIO; /* [한국어] 그 경우 호출자가 PCIBIOS_SET_FAILED 로 바꿔 올린다 */

	return ret; /* [한국어] 오류 표시가 없으면 폴링 결과(0 또는 타임아웃)를 그대로 올린다 */
}

/* [한국어]
 * rzg3s_pcie_child_prepare_bus - 요청의 대상 주소와 바이트 인에이블을 적는다
 *
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 공간 안의 오프셋.
 *
 * 읽기와 쓰기가 공유하는 준비 단계다. 두 레지스터를 채운다.
 *
 * REQADR1 에 버스/장치/함수/레지스터를 각각의 필드에 넣는다. devfn 하나에
 * 장치와 함수가 섞여 있어 PCI_SLOT/PCI_FUNC 로 나눈다.
 *
 * 레지스터 오프셋은 하위 2비트를 지워 4바이트 경계로 내린다. 이 컨트롤러가
 * 32비트 단위로만 접근하기 때문이며, 그 제약이 곧 읽기 쪽의 자리 이동과
 * 쓰기 쪽의 읽고-고쳐-쓰기를 낳는다.
 *
 * REQBE 에는 늘 네 바이트 모두 유효로 적는다. 폭을 좁히지 않으므로 쓰기가
 * 인접 바이트까지 건드리게 되고, 상류가 그 위험을 write 경로의 긴 주석으로
 * 설명해 둔다.
 *
 * 실행 컨텍스트: 아토믹. 레지스터 쓰기 두 번뿐이다.
 *
 * 호출 체인:  rzg3s_pcie_child_read_conf() / rzg3s_pcie_child_write_conf()
 *               → [이 함수]
 */
static void rzg3s_pcie_child_prepare_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	struct rzg3s_pcie_host *host = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태를 되찾는다 */
	unsigned int dev, func, reg; /* [한국어] devfn 에서 갈라낼 장치·함수 번호와, 4바이트로 내린 레지스터 오프셋 */

	dev = PCI_SLOT(devfn); /* [한국어] devfn 의 상위가 장치 번호 */
	func = PCI_FUNC(devfn); /* [한국어] devfn 의 하위가 함수 번호 */
	reg = where & ~0x3; /* [한국어] 하위 2비트를 지워 4바이트 경계로 내린다 — 이 컨트롤러가 32비트 단위로만 접근하기 때문이다 */

	/* Set the destination */
	writel_relaxed(FIELD_PREP(RZG3S_PCI_REQADR1_BUS, bus->number) |
		       FIELD_PREP(RZG3S_PCI_REQADR1_DEV, dev) |
		       FIELD_PREP(RZG3S_PCI_REQADR1_FUNC, func) |
		       FIELD_PREP(RZG3S_PCI_REQADR1_REG, reg),
		       host->axi + RZG3S_PCI_REQADR1); /* [한국어] 버스/장치/함수/오프셋을 각 필드에 넣어 한 워드로 적는다 */

	/* Set byte enable */
	writel_relaxed(RZG3S_PCI_REQBE_BYTE_EN, host->axi + RZG3S_PCI_REQBE); /* [한국어] 늘 네 바이트 모두 유효로 적는다. 그래서 좁은 쓰기가 인접 바이트까지 다시 쓰게 되고, 그 위험을 write 경로가 경고로 알린다 */
}

/* [한국어]
 * rzg3s_pcie_child_read_conf - 하위 버스에서 32비트 config 워드 하나를 읽는다
 *
 * @host:  컨트롤러 상태.
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: 오프셋.
 * @data:  읽은 워드를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * 세 단계다 — 대상을 적고, 요청 종류를 정하고, 발행해 기다린 뒤 데이터를
 * 꺼낸다.
 *
 * 요청 종류가 두 가지로 갈리는 것이 PCI 규격 그대로다. 부모가 루트 버스이면
 * 바로 아래 장치라 Type 0 config 요청이고, 그보다 깊으면 브리지가 중계해야
 * 하므로 Type 1 이다.
 *
 * 실패하면 PCIBIOS_SET_FAILED 를 돌려주는데, PCI 코어가 그것을 받으면
 * 읽은 값을 모두 1 로 채워 "장치 없음" 처럼 다룬다.
 *
 * 실행 컨텍스트: 아토믹.
 *
 * 호출 체인:  rzg3s_pcie_child_read() / rzg3s_pcie_child_write()
 *               → [이 함수] → rzg3s_pcie_child_issue_request()
 */
static int rzg3s_pcie_child_read_conf(struct rzg3s_pcie_host *host,
				      struct pci_bus *bus, unsigned int devfn,
				      int where, u32 *data)
{
	bool type0 = pci_is_root_bus(bus->parent) ? true : false; /* [한국어] 부모가 루트 버스이면 이 장치는 RC 바로 아래라 Type 0 요청이고, 더 깊으면 브리지가 중계할 Type 1 이다 */
	int ret; /* [한국어] 요청 발행 결과 */

	rzg3s_pcie_child_prepare_bus(bus, devfn, where); /* [한국어] 대상 주소와 바이트 인에이블을 먼저 적는다 */

	/* Set the type of request */
	writel_relaxed(type0 ? RZG3S_PCI_REQISS_TR_TP0_RD :
			       RZG3S_PCI_REQISS_TR_TP1_RD,
		       host->axi + RZG3S_PCI_REQISS); /* [한국어] 요청 종류를 적는다. 이 쓰기까지 끝나야 발행할 준비가 된다 */

	/* Issue the request and wait to finish */
	ret = rzg3s_pcie_child_issue_request(host); /* [한국어] 발행하고 완료를 기다린다 */
	if (ret) /* [한국어] 타임아웃이거나 대상이 오류로 답했으면 */
		return PCIBIOS_SET_FAILED; /* [한국어] PCI 코어가 이 값을 받으면 읽은 값을 모두 1 로 채워 "장치 없음" 처럼 다룬다 */

	/* Read the data */
	*data = readl_relaxed(host->axi + RZG3S_PCI_REQRCVDAT); /* [한국어] 수신 데이터 레지스터에서 32비트 워드를 꺼내 준다 */

	return PCIBIOS_SUCCESSFUL; /* [한국어] 정상 완료 */
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* [한국어]
 * rzg3s_pcie_child_read - 하위 버스 config 읽기 콜백
 *
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: 오프셋.
 * @size:  1, 2, 4 바이트.
 * @val:   읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 하위 함수가 준 오류.
 *
 * pci_ops.read 로 등록되어 PCI 코어가 직접 부른다.
 *
 * 하드웨어가 32비트 단위로만 읽으므로, 먼저 워드를 통째로 읽고 그다음
 * 요청한 폭만큼 잘라 낸다. 자리 이동량은 오프셋의 하위 2비트에서 나오고,
 * 마스크는 요청 폭에서 나온다.
 *
 * 읽기 쪽은 잘라 내기만 하면 되어 안전하다. 같은 제약이 쓰기 쪽에서는
 * 읽고-고쳐-쓰기가 되어 RW1C 비트를 건드릴 위험을 낳는다.
 *
 * 상류 주석대로 직렬화는 drivers/pci/access.c 의 pci_lock 이 맡는다.
 *
 * 실행 컨텍스트: 아토믹. 인터럽트 문맥에서도 불릴 수 있다.
 *
 * 호출 체인:  PCI 코어 → [이 함수] → rzg3s_pcie_child_read_conf()
 */
static int rzg3s_pcie_child_read(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val)
{
	struct rzg3s_pcie_host *host = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태를 되찾는다 */
	int ret; /* [한국어] 하위 함수의 결과 */

	ret = rzg3s_pcie_child_read_conf(host, bus, devfn, where, val); /* [한국어] 하드웨어가 32비트 단위로만 읽으므로 일단 워드를 통째로 읽는다 */
	if (ret != PCIBIOS_SUCCESSFUL) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 그 오류를 그대로 올린다 */

	if (size <= 2) /* [한국어] 1 또는 2 바이트 요청이면 워드에서 잘라 내야 한다 */
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1); /* [한국어] 오프셋의 하위 2비트로 자리를 옮기고 요청 폭만큼 마스크한다 */

	return PCIBIOS_SUCCESSFUL; /* [한국어] 4 바이트 요청은 워드 그대로가 답이다 */
}

/* [한국어]
 * rzg3s_pcie_child_write_conf - 하위 버스에 32비트 config 워드 하나를 쓴다
 *
 * @host:  컨트롤러 상태.
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: 오프셋.
 * @data:  쓸 워드.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * 읽기 판과 대칭이며, 데이터 레지스터를 채우는 단계가 하나 더 있다.
 *
 * REQDATA 세 개 중 앞의 둘을 0 으로 지우고 세 번째에 실제 데이터를 넣는다.
 * 그 배치의 근거(왜 인덱스 2 인지)는 이 트리에서 확인 못 함. 앞의 둘을
 * 지우는 것은 이전 요청의 값이 남아 있지 않게 하려는 것으로 보인다.
 *
 * 요청 종류가 Type 0 / Type 1 로 갈리는 규칙은 읽기 판과 같다.
 *
 * 실행 컨텍스트: 아토믹.
 *
 * 호출 체인:  rzg3s_pcie_child_write() → [이 함수]
 *               → rzg3s_pcie_child_issue_request()
 */
static int rzg3s_pcie_child_write_conf(struct rzg3s_pcie_host *host,
				       struct pci_bus *bus, unsigned int devfn,
				       int where, u32 data)
{
	bool type0 = pci_is_root_bus(bus->parent) ? true : false; /* [한국어] 읽기 판과 같은 규칙으로 요청 종류를 가른다 */
	int ret; /* [한국어] 요청 발행 결과 */

	rzg3s_pcie_child_prepare_bus(bus, devfn, where); /* [한국어] 대상 주소와 바이트 인에이블을 먼저 적는다 */

	/* Set the write data */
	writel_relaxed(0, host->axi + RZG3S_PCI_REQDATA(0)); /* [한국어] 이전 요청의 값이 남지 않도록 앞의 두 데이터 레지스터를 지운다 */
	writel_relaxed(0, host->axi + RZG3S_PCI_REQDATA(1)); /* [한국어] 같은 이유로 두 번째도 지운다 */
	writel_relaxed(data, host->axi + RZG3S_PCI_REQDATA(2)); /* [한국어] 실제 데이터는 세 번째에 넣는다. 그 자리인 근거는 이 트리에서 확인 못 함 */

	/* Set the type of request */
	writel_relaxed(type0 ? RZG3S_PCI_REQISS_TR_TP0_WR :
			       RZG3S_PCI_REQISS_TR_TP1_WR,
		       host->axi + RZG3S_PCI_REQISS); /* [한국어] 요청 종류를 적는다 */

	/* Issue the request and wait to finish */
	ret = rzg3s_pcie_child_issue_request(host); /* [한국어] 발행하고 완료를 기다린다 */
	if (ret) /* [한국어] 타임아웃이거나 대상이 오류로 답했으면 */
		return PCIBIOS_SET_FAILED; /* [한국어] 코어가 쓰기 실패로 처리한다 */

	return PCIBIOS_SUCCESSFUL; /* [한국어] 정상 완료 */
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* [한국어]
 * rzg3s_pcie_child_write - 하위 버스 config 쓰기 콜백
 *
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: 오프셋.
 * @size:  1, 2, 4 바이트.
 * @val:   쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 하위 함수가 준 오류.
 *
 * 4바이트 쓰기는 그대로 내보내면 되므로 곧바로 위임하고 끝낸다.
 *
 * 그보다 좁은 쓰기가 문제다. 상류가 긴 주석으로 그 위험을 설명해 둔다 —
 * 하드웨어가 32비트 단위로만 접근하므로 소프트웨어가 읽고-고쳐-쓰기를 해야
 * 하는데, 함께 다시 쓰이는 인접 비트 중에 RW1C(1을 쓰면 지워지는) 비트가
 * 있으면 의도치 않게 그것을 지워 버린다.
 *
 * 그래서 버스마다 한 번 경고를 남긴다. bus->unsafe_warn 플래그가 반복을
 * 막으므로, 로그가 넘치지 않으면서도 문제가 있었다는 사실은 남는다.
 *
 * 값 조립은 폭에 따라 갈린다. 1바이트는 오프셋의 하위 2비트로, 2바이트는
 * 2번 비트만으로 자리를 정한다 — 2바이트 접근은 짝수 경계에만 오기 때문이다.
 * [관찰] 마지막 else 갈래(data = val)는 size 가 1도 2도 아닐 때인데, 4는
 * 함수 첫머리에서 이미 돌아갔고 PCI 코어는 1/2/4 만 넘기므로 실제로는
 * 도달하지 않는 것으로 보인다. 상류 코드 그대로 두었다.
 *
 * 실행 컨텍스트: 아토믹. dev_warn 도 잠들지 않는다.
 *
 * 호출 체인:  PCI 코어 → [이 함수] → rzg3s_pcie_child_read_conf()
 *               → rzg3s_pcie_child_write_conf()
 */
static int rzg3s_pcie_child_write(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	struct rzg3s_pcie_host *host = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태를 되찾는다 */
	u32 data, shift; /* [한국어] 읽어 와서 고칠 워드와 그 안의 자리 */
	int ret; /* [한국어] 하위 함수의 결과 */

	if (size == 4) /* [한국어] 4 바이트 쓰기는 하드웨어 폭과 같아 읽고-고쳐-쓰기가 필요 없다 */
		return rzg3s_pcie_child_write_conf(host, bus, devfn, where, val); /* [한국어] 그대로 내보낸다 */

	/*
	 * Controller does 32 bit accesses. To do byte accesses software need
	 * to do read/modify/write. This may have potential side effects. For
	 * example, software may perform a 16-bit write. If the hardware only
	 * supports 32-bit accesses, we must do a 32-bit read, merge in the 16
	 * bits we intend to write, followed by a 32-bit write. If the 16 bits
	 * we *don't* intend to write happen to have any RW1C
	 * (write-one-to-clear) bits set, we just inadvertently cleared
	 * something we shouldn't have.
	 */
	if (!bus->unsafe_warn) { /* [한국어] 이 버스에서 아직 경고하지 않았으면 */
		dev_warn(&bus->dev, "%d-byte config write to %04x:%02x:%02x.%d offset %#x may corrupt adjacent RW1C bits\n",
			 size, pci_domain_nr(bus), bus->number,
			 PCI_SLOT(devfn), PCI_FUNC(devfn), where); /* [한국어] 어느 장치의 어느 오프셋에 좁은 쓰기가 갔는지 남긴다 */
		bus->unsafe_warn = 1; /* [한국어] 같은 버스에서 다시 찍지 않도록 표시해 둔다 */
	}

	ret = rzg3s_pcie_child_read_conf(host, bus, devfn, where, &data); /* [한국어] 고칠 바탕이 될 워드를 먼저 읽는다 — 이 읽기가 RW1C 비트를 함께 담아 오는 것이 위 경고의 근거다 */
	if (ret != PCIBIOS_SUCCESSFUL) /* [한국어] 읽기가 실패했으면 */
		return ret; /* [한국어] 쓸 수 없으므로 그 오류를 올린다 */

	if (size == 1) { /* [한국어] 1 바이트 쓰기 */
		shift = BITS_PER_BYTE * (where & 3); /* [한국어] 오프셋의 하위 2비트가 워드 안의 바이트 자리다 */
		data &= ~(0xff << shift); /* [한국어] 그 바이트를 비우고 */
		data |= ((val & 0xff) << shift); /* [한국어] 새 값을 넣는다 */
	} else if (size == 2) { /* [한국어] 2 바이트 쓰기 */
		shift = BITS_PER_BYTE * (where & 2); /* [한국어] 2번 비트만 본다 — 2바이트 접근은 짝수 경계에만 오기 때문이다 */
		data &= ~(0xffff << shift); /* [한국어] 그 하프워드를 비우고 */
		data |= ((val & 0xffff) << shift); /* [한국어] 새 값을 넣는다 */
	} else { /* [한국어] [관찰] size 가 1도 2도 아닌 경우인데, 4 는 위에서 이미 돌아갔고 PCI 코어는 1/2/4 만 넘기므로 실제로는 오지 않는 것으로 보인다 */
		data = val; /* [한국어] 그래도 정의된 동작을 두어, 값을 그대로 쓴다 */
	}

	return rzg3s_pcie_child_write_conf(host, bus, devfn, where, data); /* [한국어] 조립한 워드를 32비트 쓰기로 내보낸다 */
}

/* [한국어] RC 아래 버스용 pci_ops.
 * map_bus 가 없다 — 이 컨트롤러에는 하위 버스용 주소 창이 없어 주소를
 * 만들어 줄 수 없고, 대신 read/write 가 요청 발행 절차를 직접 수행한다.
 * probe 가 bridge->child_ops 에 꽂는다. */
static struct pci_ops rzg3s_pcie_child_ops = {
	.read		= rzg3s_pcie_child_read, /* [한국어] 요청을 발행해 워드를 읽고 폭에 맞춰 잘라 낸다 */
	.write		= rzg3s_pcie_child_write, /* [한국어] 좁은 쓰기는 읽고-고쳐-쓰기로 처리한다 */
};

/* [한국어]
 * rzg3s_pcie_root_map_bus - RC 자신의 config 접근 주소를 만든다
 *
 * @bus:   루트 버스.
 * @devfn: 장치/함수 번호.
 * @where: 오프셋.
 * @return: 읽고 쓸 주소. devfn 이 0 이 아니면 NULL.
 *
 * 루트 버스에는 RC 하나뿐이므로 devfn 이 0 이 아닌 접근은 존재하지 않는
 * 장치를 향한 것이다. NULL 을 돌려주면 코어가 0xffffffff 로 처리한다.
 *
 * RC 의 config 공간은 AXI 창 안에 그대로 열려 있어(host->pcie 가 그 시작),
 * 오프셋만 더하면 된다. 하위 버스처럼 요청을 발행할 필요가 없고, 그래서
 * 읽기/쓰기도 코어의 범용 구현을 그대로 쓴다.
 *
 * 이 함수가 map_bus 만 제공하고 read/write 를 두지 않는 것과, 하위 버스용
 * ops 가 read/write 를 직접 두고 map_bus 를 두지 않는 것이 이 컨트롤러의
 * 이원 구조를 그대로 보여 준다.
 *
 * 실행 컨텍스트: 아토믹.
 *
 * 호출 체인:  PCI 코어(pci_generic_config_read/write) → [이 함수]
 */
static void __iomem *rzg3s_pcie_root_map_bus(struct pci_bus *bus,
					     unsigned int devfn, int where)
{
	struct rzg3s_pcie_host *host = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태를 되찾는다 */

	if (devfn) /* [한국어] 루트 버스에는 RC 하나뿐이라 devfn 이 0 이 아닌 장치는 없다 */
		return NULL; /* [한국어] 코어가 NULL 을 받으면 0xffffffff 로 처리한다 */

	return host->pcie + where; /* [한국어] RC 의 config 공간은 AXI 창 안에 그대로 열려 있어 오프셋만 더하면 된다 */
}

/* [한국어] RC 자신용 pci_ops.
 * 하위 버스용과 정반대의 모양이다 — 주소만 만들어 주면 되므로 read/write 는
 * 코어의 범용 구현을 그대로 쓰고 map_bus 만 이 파일이 제공한다.
 * probe 가 bridge->ops 에 꽂는다. */
static struct pci_ops rzg3s_pcie_root_ops = {
	.read		= pci_generic_config_read, /* [한국어] 범용 읽기 */
	.write		= pci_generic_config_write, /* [한국어] 범용 쓰기 */
	.map_bus	= rzg3s_pcie_root_map_bus, /* [한국어] RC config 공간 안의 주소를 만든다 */
};

/* [한국어]
 * rzg3s_pcie_intx_irq_handler - INTx 네 선을 받아 도메인으로 넘기는 체인 핸들러
 *
 * @desc: 상위 IRQ 의 descriptor.
 *
 * INTA~INTD 네 선이 각각 별도의 상위 인터럽트로 오고, 네 선 모두 이 함수
 * 하나를 핸들러로 쓴다. 그래서 "지금 온 것이 어느 선인가" 를 스스로 알아
 * 내야 하는데, descriptor 에서 IRQ 번호를 꺼내 INTA 의 번호를 빼는 방식을
 * 쓴다 — rzg3s_pcie_init_irqdomain() 이 host->intx_irqs[] 를 DT 순서대로
 * 채워 두었고, 그 번호들이 연속이라는 가정이 여기에 깔려 있다.
 *
 * chained_irq_enter/exit 사이에서 상위 컨트롤러가 이 선을 마스크하므로
 * 재진입이 없다.
 *
 * 상태 비트를 여기서 지우지 않는다. 그 일은 irq_chip 의 ack 콜백
 * (rzg3s_pcie_intx_irq_ack)이 맡으며, 도메인이 handle_level_irq 를
 * 지정하므로 핸들러 전에 불린다.
 *
 * 실행 컨텍스트: 인터럽트.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq()
 */
static void rzg3s_pcie_intx_irq_handler(struct irq_desc *desc)
{
	struct rzg3s_pcie_host *host = irq_desc_get_handler_data(desc); /* [한국어] 네 선 모두 이 핸들러를 공유하므로, 등록 시 함께 넘긴 host 를 되찾는다 */
	struct irq_chip *chip = irq_desc_get_chip(desc); /* [한국어] 상위 인터럽트 컨트롤러의 irq_chip */
	unsigned int irq = irq_desc_get_irq(desc); /* [한국어] 지금 올라온 인터럽트 번호 */
	u32 intx = irq - host->intx_irqs[0]; /* [한국어] INTA 의 번호를 빼서 몇 번째 선인지 알아낸다 — DT 에서 얻은 네 번호가 연속이라는 가정이 깔려 있다 */

	chained_irq_enter(chip, desc); /* [한국어] 상위 선을 마스크해 이 핸들러가 도는 동안 재진입이 없게 한다 */
	generic_handle_domain_irq(host->intx_domain, intx); /* [한국어] 해당 INTx 의 가상 IRQ 핸들러를 부른다. 그 안에서 ack 콜백이 먼저 불려 상태 비트가 지워진다 */
	chained_irq_exit(chip, desc); /* [한국어] 상위 선의 마스크를 푼다 */
}

/* [한국어]
 * rzg3s_pcie_msi_irq - MSI 선 하나를 받아 대기 벡터를 모두 처리하는 핸들러
 *
 * @irq:  인터럽트 번호. 쓰지 않는다.
 * @data: request_irq 에 넘긴 host 포인터.
 * @return: 이 컨트롤러의 MSI 가 아니면 IRQ_NONE, 처리했으면 IRQ_HANDLED.
 *
 * 체인 핸들러가 아니라 보통의 인터럽트 핸들러다. 그래서 맨 먼저 상태
 * 레지스터를 보고 "내 것인가" 를 판정하고, 아니면 IRQ_NONE 으로 물러난다 —
 * 선을 공유하는 다른 장치가 있을 수 있기 때문이다.
 *
 * 내 것이면 두 개의 상위 상태 비트(PINTRCVIS 의 MSI, MSGRCVIS 의 MRI)를
 * 먼저 지운다. 지우기 전에 벡터를 읽으면 그사이 새로 온 MSI 를 놓칠 수
 * 있으므로, 상위 비트를 먼저 지우고 그다음 개별 벡터 상태를 읽는 순서다.
 *
 * 벡터 상태는 레지스터당 32비트씩 나뉘어 있다. 지금은 전체 벡터 수가 32라
 * 레지스터가 하나뿐이지만(RZG3S_PCI_MSI_INT_NR == _PER_REG), 루프와
 * bitmap_write 로 일반화해 두어 벡터 수가 늘어도 그대로 동작한다.
 *
 * 등록되지 않은 벡터가 올라오면 그 비트를 직접 지운다. 그렇게 하지 않으면
 * 같은 인터럽트가 끝없이 다시 올라와 시스템이 멈춘다.
 *
 * 개별 벡터 비트는 정상 경로에서는 irq_chip 의 ack 콜백이 지운다. 도메인이
 * handle_edge_irq 를 쓰므로 핸들러 전에 불린다.
 *
 * 실행 컨텍스트: 인터럽트(하드 IRQ). request_irq 에 플래그 0 을 넘기므로
 * 스레드로 나뉘지 않는다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq()
 */
static irqreturn_t rzg3s_pcie_msi_irq(int irq, void *data)
{
	u8 regs = RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 상태 레지스터 개수. 지금은 1 이라 아래 루프가 한 번만 돈다 */
	DECLARE_BITMAP(bitmap, RZG3S_PCI_MSI_INT_NR) = {0}; /* [한국어] 여러 레지스터의 상태를 모아 담을 비트맵. 0 으로 초기화한다 */
	struct rzg3s_pcie_host *host = data; /* [한국어] request_irq 에 넘긴 host 포인터 */
	struct rzg3s_pcie_msi *msi = &host->msi; /* [한국어] 그 안의 MSI 상태 */
	unsigned long bit; /* [한국어] 훑는 중인 벡터 번호 */
	u32 status; /* [한국어] 레지스터에서 읽은 상태 워드 */

	status = readl_relaxed(host->axi + RZG3S_PCI_PINTRCVIS); /* [한국어] 상위 상태 레지스터를 읽어 */
	if (!(status & RZG3S_PCI_PINTRCVIS_MSI)) /* [한국어] MSI 비트가 서 있지 않으면 이 컨트롤러가 올린 인터럽트가 아니다 */
		return IRQ_NONE; /* [한국어] 선을 공유하는 다른 장치의 것이므로 손대지 않고 물러난다 */

	/* Clear the MSI */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIS,
			       RZG3S_PCI_PINTRCVIS_MSI,
			       RZG3S_PCI_PINTRCVIS_MSI); /* [한국어] 상위 MSI 상태 비트를 먼저 지운다 — 개별 벡터를 읽기 전에 지워야 그사이 새로 온 것을 놓치지 않는다 */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSGRCVIS,
			       RZG3S_PCI_MSGRCVIS_MRI, RZG3S_PCI_MSGRCVIS_MRI); /* [한국어] MSI 는 메시지로 오므로 메시지 수신 상태 비트도 함께 지운다 */

	for (u8 reg_id = 0; reg_id < regs; reg_id++) { /* [한국어] 상태 레지스터를 차례로 훑는다 */
		status = readl_relaxed(host->axi + RZG3S_PCI_MSIRS(reg_id)); /* [한국어] 그 레지스터가 담은 32개 벡터의 대기 상태 */
		bitmap_write(bitmap, status, reg_id * RZG3S_PCI_MSI_INT_PER_REG,
			     RZG3S_PCI_MSI_INT_PER_REG); /* [한국어] 비트맵의 해당 구간에 그 32비트를 그대로 옮겨 넣는다 */
	}

	for_each_set_bit(bit, bitmap, RZG3S_PCI_MSI_INT_NR) { /* [한국어] 대기 중인 벡터만 골라 훑는다 */
		int ret; /* [한국어] 도메인 전달 결과 */

		ret = generic_handle_domain_irq(msi->domain, bit); /* [한국어] 해당 벡터의 가상 IRQ 핸들러를 부른다. 그 안에서 ack 콜백이 먼저 불려 비트가 지워진다 */
		if (ret) { /* [한국어] 등록되지 않은 벡터가 올라온 경우 */
			u8 reg_bit = bit % RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 그 벡터가 놓인 레지스터 안의 비트 자리 */
			u8 reg_id = bit / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 그리고 레지스터 번호 */

			/* Unknown MSI, just clear it */
			writel_relaxed(BIT(reg_bit),
				       host->axi + RZG3S_PCI_MSIRS(reg_id)); /* [한국어] 아무도 지워 주지 않으므로 직접 지운다. 그러지 않으면 같은 인터럽트가 끝없이 다시 올라온다 */
		}
	}

	return IRQ_HANDLED; /* [한국어] 이 컨트롤러의 인터럽트를 처리했음을 알린다 */
}

/* [한국어]
 * rzg3s_pcie_msi_irq_ack - 이 MSI 벡터의 대기 비트를 지운다
 *
 * @d: IRQ 코어가 넘기는 irq_data. hwirq 가 벡터 번호다.
 *
 * 벡터 번호를 레지스터 번호와 그 안의 비트로 나눈다. 지금은 레지스터가
 * 하나뿐이라 reg_id 가 늘 0 이지만, 나누기·나머지로 적어 두어 벡터 수가
 * 늘어도 그대로 동작한다.
 *
 * R/W1C 레지스터라 해당 비트에 1 을 쓰면 지워진다. 읽고-고쳐-쓰기가 아니라
 * 경쟁이 없고, 그래서 mask/unmask 와 달리 hw_lock 을 잡지 않는다 —
 * 레지스터 정의 위의 상류 주석이 그 사실을 밝혀 둔다.
 *
 * chip_data 로 넣어 둔 것이 host 가 아니라 msi 라, rzg3s_msi_to_host
 * 매크로가 container_of 로 바깥 host 를 되찾는다.
 *
 * 실행 컨텍스트: 인터럽트. handle_edge_irq 가 핸들러 전에 부른다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → writel_relaxed()
 */
static void rzg3s_pcie_msi_irq_ack(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d); /* [한국어] 도메인 등록 시 chip_data 로 넣어 둔 MSI 상태 */
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi); /* [한국어] 거기서 바깥 컨트롤러 상태를 되찾는다 */
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 벡터 번호를 레지스터 안의 비트 자리로 바꾼다 */
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 그리고 레지스터 번호로. 지금은 레지스터가 하나뿐이라 늘 0 이다 */

	writel_relaxed(BIT(reg_bit), host->axi + RZG3S_PCI_MSIRS(reg_id)); /* [한국어] R/W1C 라 1 을 쓰면 지워진다. 읽고-고쳐-쓰기가 아니어서 락이 필요 없다 */
}

/* [한국어]
 * rzg3s_pcie_msi_irq_mask - 이 MSI 벡터를 마스크한다
 *
 * @d: IRQ 코어가 넘기는 irq_data.
 *
 * 마스크 레지스터의 해당 비트를 1 로 세운다. ack 와 달리 읽고-고쳐-쓰기라
 * 같은 레지스터를 건드리는 다른 벡터와 경쟁하므로 host->hw_lock 으로
 * 감싼다.
 *
 * guard(raw_spinlock_irqsave) 를 쓰는 것이 눈에 띈다. linux/cleanup.h 의
 * 정리 헬퍼로, 범위를 벗어날 때 자동으로 놓아 준다 — 이 함수처럼 반환
 * 경로가 하나뿐이어도 실수를 원천적으로 막는다. raw 판인 것은 이 락이
 * 인터럽트 문맥에서 잡히기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스 컨텍스트. 어느 쪽이든 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → rzg3s_pcie_update_bits()
 */
static void rzg3s_pcie_msi_irq_mask(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d); /* [한국어] chip_data 로 넣어 둔 MSI 상태 */
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi); /* [한국어] 바깥 컨트롤러 상태를 되찾는다 */
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 레지스터 안의 비트 자리 */
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 레지스터 번호 */

	guard(raw_spinlock_irqsave)(&host->hw_lock); /* [한국어] 마스크 레지스터는 읽고-고쳐-쓰기라 같은 레지스터의 다른 벡터와 경쟁한다. 범위를 벗어나면 자동으로 놓인다 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSIRM(reg_id), BIT(reg_bit),
			       BIT(reg_bit)); /* [한국어] 해당 비트를 1 로 세워 이 벡터를 막는다 */
}

/* [한국어]
 * rzg3s_pcie_msi_irq_unmask - 이 MSI 벡터의 마스크를 푼다
 *
 * @d: IRQ 코어가 넘기는 irq_data.
 *
 * mask 의 짝이며, 같은 비트에 0 을 넣는 것만 다르다. 락과 그 이유도 같다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스 컨텍스트.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → rzg3s_pcie_update_bits()
 */
static void rzg3s_pcie_msi_irq_unmask(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d); /* [한국어] chip_data 로 넣어 둔 MSI 상태 */
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi); /* [한국어] 바깥 컨트롤러 상태를 되찾는다 */
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 레지스터 안의 비트 자리 */
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 레지스터 번호 */

	guard(raw_spinlock_irqsave)(&host->hw_lock); /* [한국어] mask 와 같은 이유로 같은 락을 잡는다 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSIRM(reg_id), BIT(reg_bit),
			       0); /* [한국어] 해당 비트를 0 으로 내려 이 벡터를 다시 받는다 */
}

/* [한국어]
 * rzg3s_pcie_irq_compose_msi_msg - 장치가 MSI 를 보낼 주소와 데이터를 알려 준다
 *
 * @data: IRQ 코어가 넘기는 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채워 줄 주소/데이터.
 *
 * 주소를 상수나 소프트웨어 변수가 아니라 하드웨어 레지스터에서 되읽는 것이
 * 이 구현의 특징이다. 그래서 rzg3s_pcie_msi_hw_setup() 이 적어 둔 값과
 * 장치에 알려 주는 값이 어긋날 수 없다.
 *
 * 하위 워드에서 마스크로 걸러 내는 이유를 상류 주석이 밝힌다 — 그 워드의
 * 아래쪽 비트가 주소가 아니라 활성 비트와 메시지 데이터 활성 비트로
 * 전용되어 있어, 그것들과 쓰이지 않는 비트를 함께 떨어내야 순수한 주소가
 * 된다.
 *
 * 데이터는 벡터 번호 그대로다. 장치가 그 값을 목적지 주소에 쓰면
 * 컨트롤러가 어느 벡터인지 알아본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(벡터 할당 시).
 *
 * 호출 체인:  MSI 코어 → [이 함수] → readl_relaxed()
 */
static void rzg3s_pcie_irq_compose_msi_msg(struct irq_data *data,
					   struct msi_msg *msg)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(data); /* [한국어] chip_data 로 넣어 둔 MSI 상태 */
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi); /* [한국어] 바깥 컨트롤러 상태를 되찾는다 */
	u32 lo, hi; /* [한국어] 하드웨어에서 되읽을 주소의 두 워드 */

	/*
	 * Enable and msg data enable bits are part of the address lo. Drop
	 * them along with the unused bit.
	 */
	lo = readl_relaxed(host->axi + RZG3S_PCI_MSIRCVWADRL) &
	     RZG3S_PCI_MSIRCVWADRL_MASK; /* [한국어] 하드웨어가 실제로 쓰고 있는 주소를 되읽고, 활성 비트들을 마스크로 떨어낸다 */
	hi = readl_relaxed(host->axi + RZG3S_PCI_MSIRCVWADRU); /* [한국어] 상위 워드는 주소만 담고 있어 그대로 쓴다 */

	msg->address_lo = lo; /* [한국어] 장치가 쓸 목적지 주소의 하위 32비트 */
	msg->address_hi = hi; /* [한국어] 같은 주소의 상위 32비트 */
	msg->data = data->hwirq; /* [한국어] 데이터는 벡터 번호 그대로다. 장치가 그 값을 쓰면 컨트롤러가 어느 벡터인지 알아본다 */
}

/* [한국어] 이 MSI 벡터들의 irq_chip.
 * 콜백 넷을 모두 갖춘 것이 이 하드웨어의 능력을 보여 준다 — 벡터별 마스크
 * 레지스터가 있어 mask/unmask 를 실제로 구현할 수 있다. 다만 어피니티는
 * 없어, msi_parent_ops 가 MSI_FLAG_NO_AFFINITY 로 그 사실을 알린다. */
static struct irq_chip rzg3s_pcie_msi_bottom_chip = {
	.name			= "rzg3s-pcie-msi", /* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_ack		= rzg3s_pcie_msi_irq_ack, /* [한국어] 대기 비트를 지운다. handle_edge_irq 가 핸들러 전에 부른다 */
	.irq_mask		= rzg3s_pcie_msi_irq_mask, /* [한국어] 이 벡터를 막는다 */
	.irq_unmask		= rzg3s_pcie_msi_irq_unmask, /* [한국어] 다시 받는다 */
	.irq_compose_msi_msg	= rzg3s_pcie_irq_compose_msi_msg, /* [한국어] 장치에 알려 줄 주소/데이터를 만든다 */
};

/* [한국어]
 * rzg3s_pcie_msi_domain_alloc - MSI 벡터를 잡아 virq 들에 연결한다
 *
 * @domain:  MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 요청 개수.
 * @args:    쓰지 않는다.
 * @return: 0 성공, -ENOSPC 는 자리가 없는 경우.
 *
 * bitmap_find_free_region 에 order_base_2(nr_irqs) 를 넘긴다. multi-MSI 는
 * 벡터가 연속이고 개수가 2의 거듭제곱이며 시작이 그 크기에 정렬되어야
 * 하는데, 이 한 호출이 세 조건을 모두 만족하는 자리를 찾아 준다.
 *
 * 비트맵 조작만 scoped_guard 로 감싸고, 그 뒤의 도메인 등록은 락 밖에서
 * 한다. 락 구간을 최소로 유지하려는 것이다.
 *
 * irq_domain_set_info 에 넘기는 인자들이 이 도메인의 성격을 정한다 —
 * irq_chip 은 ack/mask/unmask/compose 를 갖춘 rzg3s_pcie_msi_bottom_chip,
 * chip_data 는 host_data(즉 &host->msi)라 콜백들이 문맥을 되찾고,
 * 흐름 제어는 handle_edge_irq 다. MSI 가 본질적으로 edge 이기 때문이다.
 *
 * 실패 시 되돌릴 것이 없다 — 비트맵 할당이 실패하면 아무것도 잡지 않은
 * 상태다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → bitmap_find_free_region()
 *               → irq_domain_set_info()
 */
static int rzg3s_pcie_msi_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct rzg3s_pcie_msi *msi = domain->host_data; /* [한국어] 도메인 생성 시 넣어 둔 MSI 상태를 되찾는다 */
	int hwirq; /* [한국어] 잡은 시작 벡터 번호 */

	scoped_guard(mutex, &msi->map_lock) { /* [한국어] 비트맵 조작만 락으로 감싼다 — 아래 도메인 등록은 락이 필요 없다 */
		hwirq = bitmap_find_free_region(msi->map, RZG3S_PCI_MSI_INT_NR,
						order_base_2(nr_irqs)); /* [한국어] 2의 거듭제곱 크기로 정렬된 연속 자리를 찾아 표시한다. multi-MSI 의 세 조건을 한 번에 만족시킨다 */
	}

	if (hwirq < 0) /* [한국어] 자리가 없으면 */
		return -ENOSPC; /* [한국어] 아직 아무것도 잡지 않은 상태라 되돌릴 것 없이 돌아간다 */

	for (unsigned int i = 0; i < nr_irqs; i++) { /* [한국어] 요청 개수만큼 virq 와 hwirq 를 하나씩 짝짓는다 */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &rzg3s_pcie_msi_bottom_chip,
				    domain->host_data, handle_edge_irq, NULL,
				    NULL); /* [한국어] irq_chip 은 위의 bottom chip, chip_data 는 콜백이 되찾을 msi, 흐름 제어는 MSI 의 성격에 맞는 edge 처리다 */
	}

	return 0; /* [한국어] 전부 등록했다 */
}

/* [한국어]
 * rzg3s_pcie_msi_domain_free - 잡아 두었던 MSI 벡터를 되돌린다
 *
 * @domain:  MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 개수.
 *
 * 대표 virq 의 irq_data 에서 hwirq 를 얻어 그 자리를 푼다. order 계산이
 * alloc 과 같아야 정확히 같은 범위가 비워진다.
 *
 * alloc 은 scoped_guard 로 구간을 좁혔지만 여기서는 함수 전체를 guard 로
 * 감싼다. 락 뒤에 할 일이 없어 구분할 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → bitmap_release_region()
 */
static void rzg3s_pcie_msi_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq); /* [한국어] 대표 virq 의 irq_data 를 얻어 hwirq 를 알아낸다 */
	struct rzg3s_pcie_msi *msi = domain->host_data; /* [한국어] 도메인에 넣어 둔 MSI 상태 */

	guard(mutex)(&msi->map_lock); /* [한국어] 비트맵을 고치므로 잡는다. 뒤에 할 일이 없어 함수 전체를 감싼다 */

	bitmap_release_region(msi->map, d->hwirq, order_base_2(nr_irqs)); /* [한국어] alloc 과 같은 order 로 풀어야 정확히 같은 범위가 비워진다 */
}

/* [한국어] 이 MSI 도메인의 벡터 할당/반납 콜백 표.
 * rzg3s_pcie_msi_allocate_domains() 가 irq_domain_info.ops 로 넘긴다.
 * 이 도메인이 하는 일이 비트맵 관리와 virq↔hwirq 연결뿐이라 둘로 충분하다. */
static const struct irq_domain_ops rzg3s_pcie_msi_domain_ops = {
	.alloc	= rzg3s_pcie_msi_domain_alloc, /* [한국어] 벡터를 잡아 virq 에 연결한다 */
	.free	= rzg3s_pcie_msi_domain_free, /* [한국어] 그 반대 */
};

/* [한국어] 이 MSI 도메인이 상위 계층에 요구하는 플래그.
 * USE_DEF_DOM_OPS/USE_DEF_CHIP_OPS 는 빈 콜백을 코어의 기본 구현으로 채워
 * 달라는 뜻이고, NO_AFFINITY 는 벡터별 CPU 지정을 지원하지 않음을 알린다
 * (실제로 위 irq_chip 에 set_affinity 콜백이 없다). MASK_PARENT 는 PCI 계층의
 * MSI 마스크 요청을 부모(즉 이 파일의 irq_mask/irq_unmask)로 내려 달라는
 * 뜻으로, 이 하드웨어에 벡터별 마스크 레지스터가 있어 가능한 선택이다. */
#define RZG3S_PCIE_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS	| \
					 MSI_FLAG_USE_DEF_CHIP_OPS	| \
					 MSI_FLAG_NO_AFFINITY		| \
					 MSI_FLAG_PCI_MSI_MASK_PARENT)

/* [한국어] 이 도메인이 지원할 수 있는 플래그의 상한.
 * MULTI_PCI_MSI 로 multi-MSI 를 받아들이는데, 그 지원의 실체가
 * rzg3s_pcie_msi_domain_alloc() 의 bitmap_find_free_region 이다 — 연속·정렬된
 * 자리를 찾아 준다. 나머지는 일반 플래그를 모두 허용한다. */
#define RZG3S_PCIE_MSI_FLAGS_SUPPORTED	(MSI_FLAG_MULTI_PCI_MSI		| \
					 MSI_GENERIC_FLAGS_MASK)

/* [한국어] 상위 MSI 계층(irq-msi-lib)이 이 도메인을 다룰 때 쓰는 규약 묶음.
 * msi_create_parent_irq_domain() 에 함께 넘겨, 부모 도메인 위에 PCI/MSI 용
 * 자식 도메인을 만들 때 쓰인다. */
static const struct msi_parent_ops rzg3s_pcie_msi_parent_ops = {
	.required_flags		= RZG3S_PCIE_MSI_FLAGS_REQUIRED, /* [한국어] 반드시 적용할 플래그 */
	.supported_flags	= RZG3S_PCIE_MSI_FLAGS_SUPPORTED, /* [한국어] 허용 가능한 플래그의 상한 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI, /* [한국어] 이 도메인이 PCI MSI 버스용임을 알리는 토큰 */
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK, /* [한국어] 이 irq_chip 이 ack 콜백을 갖고 있으니 써 달라는 표시 */
	.prefix			= "RZG3S-", /* [한국어] /proc/interrupts 등에 보일 이름 앞머리 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info, /* [한국어] 자식 도메인 정보를 채우는 표준 구현을 그대로 쓴다 */
};

/* [한국어]
 * rzg3s_pcie_msi_allocate_domains - 이 컨트롤러의 MSI irq_domain 을 만든다
 *
 * @msi: MSI 상태.
 * @return: 0 성공, -ENOMEM 은 도메인 생성 실패.
 *
 * msi_create_parent_irq_domain() 이 부모 도메인을 만들고, 그 위에 PCI/MSI 용
 * 자식 도메인을 붙이는 일까지 rzg3s_pcie_msi_parent_ops 를 통해 처리한다.
 *
 * info 에 채우는 네 값의 뜻은 - fwnode 는 이 컨트롤러의 DT 노드(도메인
 * 식별자), ops 는 alloc/free 콜백, size 는 벡터 수, host_data 는 콜백들이
 * 되찾을 msi 포인터다. host_data 가 host 가 아니라 msi 인 것이,
 * irq_chip 콜백들이 rzg3s_msi_to_host 로 한 단계 더 거슬러 올라가는
 * 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rzg3s_pcie_init_msi() → [이 함수]
 *               → msi_create_parent_irq_domain()
 */
static int rzg3s_pcie_msi_allocate_domains(struct rzg3s_pcie_msi *msi)
{
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi); /* [한국어] MSI 상태에서 바깥 컨트롤러 상태를 되찾는다 */
	struct device *dev = host->dev; /* [한국어] fwnode 와 로그의 기준이 될 device */
	struct irq_domain_info info = { /* [한국어] 도메인을 만들 때 코어에 넘길 정보 묶음 */
		.fwnode		= dev_fwnode(dev), /* [한국어] 도메인 식별자로 이 컨트롤러의 DT 노드를 쓴다 */
		.ops		= &rzg3s_pcie_msi_domain_ops, /* [한국어] 위에서 정의한 alloc/free 콜백 표 */
		.size		= RZG3S_PCI_MSI_INT_NR, /* [한국어] 이 컨트롤러가 지원하는 벡터 수 */
		.host_data	= msi, /* [한국어] 콜백들이 domain->host_data 로 되찾을 문맥. host 가 아니라 msi 라, irq_chip 콜백들이 한 단계 더 거슬러 올라간다 */
	};

	msi->domain = msi_create_parent_irq_domain(&info,
						   &rzg3s_pcie_msi_parent_ops); /* [한국어] 부모 도메인을 만들고 그 위에 PCI/MSI 자식 도메인까지 붙인다 */
	if (!msi->domain) /* [한국어] 만들지 못했으면 */
		return dev_err_probe(dev, -ENOMEM,
				     "failed to create IRQ domain\n"); /* [한국어] 원인이 대개 메모리 부족이라 그렇게 알리고 그 오류를 올린다 */

	return 0; /* [한국어] 도메인 준비 완료 */
}

/* [한국어]
 * rzg3s_pcie_msi_hw_setup - MSI 수신 하드웨어를 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 늘 0. 실패할 일이 없다.
 *
 * probe 경로에서는 rzg3s_pcie_msi_setup() 이 목적지 주소를 정한 뒤 이
 * 함수를 부르고, resume 경로에서는 rzg3s_pcie_host_setup() 의 init 콜백으로
 * 이 함수가 직접 불린다. 서스펜드로 레지스터만 날아가고 도메인·비트맵·
 * 목적지 페이지는 메모리에 그대로 남아 있으므로, 하드웨어 쪽만 다시 세우면
 * 되기 때문이다. 반환형이 int 인 것도 그 콜백 자리에 맞추기 위한 것으로
 * 보인다.
 *
 * 세우는 것이 다섯이다.
 *   1) 창 크기 마스크. 상류 주석대로 벡터 수에서 1 을 뺀 값을 넣으면
 *      하드웨어가 벡터 수 x 4바이트 크기의 창으로 해석한다.
 *   2) 창 주소. 상위 워드를 먼저 쓰고, 하위 워드를 쓸 때 활성 비트와
 *      메시지 데이터 활성 비트를 함께 얹는다. 하위 워드 쓰기가 곧 활성화라
 *      상위를 먼저 써야 순서가 맞는다.
 *   3) 벡터 수신 활성. 레지스터마다 하나씩 켠다.
 *   4) 메시지 수신 인터럽트 활성.
 *   5) 상위 인터럽트 인에이블의 MSI 비트.
 *
 * 세 갈래의 인에이블이 모두 필요한 구조라, 하나라도 빠지면 MSI 가 올라오지
 * 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:  rzg3s_pcie_msi_setup() / rzg3s_pcie_host_setup()(resume)
 *               → [이 함수]
 */
static int rzg3s_pcie_msi_hw_setup(struct rzg3s_pcie_host *host)
{
	u8 regs = RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 상태/활성 레지스터 개수. 지금은 1 이다 */
	struct rzg3s_pcie_msi *msi = &host->msi; /* [한국어] 목적지 주소를 담고 있는 MSI 상태 */

	/*
	 * Set MSI window size. HW will set the window to
	 * RZG3S_PCI_MSI_INT_NR * 4 bytes.
	 */
	writel_relaxed(FIELD_PREP(RZG3S_PCI_MSIRCVWMSKL_MASK,
				  RZG3S_PCI_MSI_INT_NR - 1),
		       host->axi + RZG3S_PCI_MSIRCVWMSKL); /* [한국어] 창 크기 마스크를 적는다. 벡터 수에서 1 을 뺀 값이다 */

	/* Set MSI window address and enable MSI window */
	writel_relaxed(upper_32_bits(msi->window_base),
		       host->axi + RZG3S_PCI_MSIRCVWADRU); /* [한국어] 주소 상위 워드를 먼저 쓴다 — 하위 워드 쓰기가 곧 활성화이기 때문이다 */
	writel_relaxed(lower_32_bits(msi->window_base) |
		       RZG3S_PCI_MSIRCVWADRL_ENA |
		       RZG3S_PCI_MSIRCVWADRL_MSG_DATA_ENA,
		       host->axi + RZG3S_PCI_MSIRCVWADRL); /* [한국어] 하위 워드에 주소와 두 활성 비트를 함께 써서 창을 켠다 */

	/* Set MSI receive enable */
	for (u8 reg_id = 0; reg_id < regs; reg_id++) { /* [한국어] 상태 레지스터마다 */
		writel_relaxed(RZG3S_PCI_MSIRE_ENA,
			       host->axi + RZG3S_PCI_MSIRE(reg_id)); /* [한국어] 그 레지스터가 담당하는 벡터들의 수신을 켠다 */
	}

	/* Enable message receive interrupts */
	writel_relaxed(RZG3S_PCI_MSGRCVIE_MSG_RCV,
		       host->axi + RZG3S_PCI_MSGRCVIE); /* [한국어] MSI 는 메시지로 오므로 메시지 수신 인터럽트도 켜야 한다 */

	/* Enable MSI */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE,
			       RZG3S_PCI_PINTRCVIE_MSI,
			       RZG3S_PCI_PINTRCVIE_MSI); /* [한국어] 마지막으로 상위 인터럽트 인에이블의 MSI 비트를 켠다. 세 갈래가 모두 켜져야 MSI 가 올라온다 */

	return 0; /* [한국어] 실패할 일이 없다. int 인 것은 host_setup 의 콜백 자리에 맞추기 위한 것으로 보인다 */
}

/* [한국어]
 * rzg3s_pcie_msi_setup - MSI 목적지 메모리를 잡고 하드웨어를 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 메모리·DMA 매핑 실패는 -ENOMEM, 창 제약을 못 맞추면 -EINVAL.
 *
 * MSI 는 장치가 특정 주소에 쓰는 것으로 구현되므로, 그 목적지가 될 메모리가
 * 필요하다. 이 함수가 그것을 잡고, 하드웨어 제약 두 가지를 확인한 뒤
 * 레지스터를 세운다.
 *
 *   1) 페이지 하나를 잡는다. GFP_DMA 를 붙여 장치가 닿을 수 있는 낮은
 *      영역에서 받는다.
 *   2) 그 페이지를 DMA 매핑한다. 크기를 size 의 두 배로 잡는 것이 요점이다 —
 *      아래에서 시작 주소를 size 경계로 올려 맞추므로, 최악의 경우 창이
 *      원래 주소보다 size-1 만큼 뒤에서 시작해 매핑 범위를 넘어설 수 있다.
 *      두 배로 잡아 두면 어느 경우에도 창 전체가 매핑 안에 들어온다.
 *   3) 상류 주석이 인용하는 하드웨어 매뉴얼 제약을 확인한다 — MSI 창은
 *      이미 열려 있는 AXI 창 하나 안에 들어가야 한다. 그래서 여덟 개의
 *      AXI 창을 훑어 활성화된 것 중 이 DMA 주소를 품는 것을 찾는다.
 *      끝까지 못 찾으면(id 가 상한에 닿으면) -EINVAL 이다.
 *      이 제약 때문에 이 함수가 반드시 안쪽 창 설정 뒤에 불려야 한다.
 *   4) 시작 주소를 창 크기 경계로 올려 맞춘다. 그 결과가 원래 주소보다
 *      작아졌다면 ALIGN 이 넘쳐 흐른 것이므로 -EINVAL 이다.
 *   5) 레지스터를 세운다.
 *
 * 되돌리기가 라벨 둘로 정확히 대칭이다 — 매핑 뒤 실패는 언매핑과 페이지
 * 반납을, 매핑 실패는 페이지 반납만 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 페이지 할당이 잠들 수 있다.
 *
 * 호출 체인:  rzg3s_pcie_init_msi() → [이 함수] → rzg3s_pcie_msi_hw_setup()
 */
static int rzg3s_pcie_msi_setup(struct rzg3s_pcie_host *host)
{
	size_t size = RZG3S_PCI_MSI_INT_NR * sizeof(u32); /* [한국어] MSI 창의 크기. 벡터 하나가 4바이트를 차지한다 */
	struct rzg3s_pcie_msi *msi = &host->msi; /* [한국어] 채워 넣을 MSI 상태 */
	struct device *dev = host->dev; /* [한국어] DMA 매핑과 로그의 기준 */
	int id, ret; /* [한국어] AXI 창 탐색 인덱스와 하위 호출 결과 */

	msi->pages = __get_free_pages(GFP_KERNEL | GFP_DMA, 0); /* [한국어] 페이지 하나를 잡는다. GFP_DMA 로 장치가 닿을 수 있는 낮은 영역에서 받는다 */
	if (!msi->pages) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 아직 잡은 것이 없다 */

	msi->dma_addr = dma_map_single(dev, (void *)msi->pages, size * 2,
				       DMA_BIDIRECTIONAL); /* [한국어] 장치가 쓸 수 있게 매핑한다. 크기를 두 배로 잡는 것은 아래 정렬로 창이 뒤로 밀릴 수 있어 그 여유를 두기 위해서다 */
	if (dma_mapping_error(dev, msi->dma_addr)) { /* [한국어] 매핑에 실패했으면 */
		ret = -ENOMEM; /* [한국어] 메모리 부족으로 처리하고 */
		goto free_pages; /* [한국어] 잡은 페이지를 되돌린다 */
	}

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section 34.4.5.2 Setting
	 * the MSI Window) the MSI window needs to fall within one of the
	 * enabled AXI windows. Find an enabled AXI window to setup the MSI
	 * window.
	 */
	for (id = 0; id < RZG3S_MAX_WINDOWS; id++) { /* [한국어] 여덟 개의 AXI 창을 훑는다 */
		u64 base, basel, baseu; /* [한국어] 그 창의 base 주소를 담을 변수들 */
		u64 mask, maskl, masku; /* [한국어] 그 창의 크기 마스크를 담을 변수들 */

		basel = readl_relaxed(host->axi + RZG3S_PCI_AWBASEL(id)); /* [한국어] base 하위 워드를 읽는다 */
		/* Skip checking this AXI window if it's not enabled */
		if (!(basel & RZG3S_PCI_AWBASEL_WIN_ENA)) /* [한국어] 활성 비트가 없으면 열려 있지 않은 창이다 */
			continue; /* [한국어] 상류 주석대로 확인할 필요가 없어 건너뛴다 */

		baseu = readl_relaxed(host->axi + RZG3S_PCI_AWBASEU(id)); /* [한국어] base 상위 워드 */
		base = baseu << 32 | basel; /* [한국어] 두 워드를 합쳐 64비트 시작 주소를 만든다 */

		maskl = readl_relaxed(host->axi + RZG3S_PCI_AWMASKL(id)); /* [한국어] 크기 마스크 하위 워드 */
		masku = readl_relaxed(host->axi + RZG3S_PCI_AWMASKU(id)); /* [한국어] 크기 마스크 상위 워드 */
		mask = masku << 32 | maskl; /* [한국어] 두 워드를 합친다. 마스크가 2^N - 1 이라 base + mask 가 곧 끝 주소다 */

		if (msi->dma_addr < base || msi->dma_addr > base + mask) /* [한국어] MSI 목적지가 이 창 밖이면 */
			continue; /* [한국어] 다음 창을 본다 */

		break; /* [한국어] 품는 창을 찾았다 */
	}

	if (id == RZG3S_MAX_WINDOWS) { /* [한국어] 끝까지 못 찾았으면 — 매뉴얼이 요구하는 제약을 만족할 수 없다 */
		ret = -EINVAL; /* [한국어] 설정 오류로 처리하고 */
		goto dma_unmap; /* [한국어] 매핑과 페이지를 모두 되돌린다 */
	}

	/* The MSI base address must be aligned to the MSI size */
	msi->window_base = ALIGN(msi->dma_addr, size); /* [한국어] 상류 주석대로 창 시작은 창 크기에 정렬되어야 한다 */
	if (msi->window_base < msi->dma_addr) { /* [한국어] 정렬 계산이 넘쳐 흘러 원래 주소보다 작아졌으면 */
		ret = -EINVAL; /* [한국어] 쓸 수 없는 값이다 */
		goto dma_unmap; /* [한국어] 되돌린다 */
	}

	rzg3s_pcie_msi_hw_setup(host); /* [한국어] 주소가 정해졌으므로 레지스터를 세운다 */

	return 0; /* [한국어] MSI 준비 완료 */

dma_unmap: /* [한국어] 매핑 뒤 실패가 여기로 모인다 */
	dma_unmap_single(dev, msi->dma_addr, size * 2, DMA_BIDIRECTIONAL); /* [한국어] 매핑 때와 같은 크기로 푼다 */
free_pages: /* [한국어] 매핑 실패는 여기로 온다 */
	free_pages(msi->pages, 0); /* [한국어] 잡은 페이지를 놓는다 */
	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_msi_hw_teardown - MSI 수신 하드웨어를 끈다
 *
 * @host: 컨트롤러 상태.
 *
 * rzg3s_pcie_msi_hw_setup() 의 역순이다. 상위 인에이블부터 끄고 창을 마지막에
 * 닫는데, 반대로 하면 창이 닫힌 뒤에도 잠깐 인터럽트가 올라올 수 있다.
 *
 * setup 과 마찬가지로 resume/suspend 경로에서 rzg3s_pcie_host_setup() 의
 * teardown 콜백 자리에 그대로 쓰인다.
 *
 * 창 주소 레지스터에 0 을 쓰면 활성 비트까지 함께 0 이 되어 창이 닫힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_teardown_msi() / rzg3s_pcie_host_setup()(되돌리기)
 *               → [이 함수]
 */
static void rzg3s_pcie_msi_hw_teardown(struct rzg3s_pcie_host *host)
{
	u8 regs = RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; /* [한국어] 활성 레지스터 개수 */

	/* Disable MSI */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE,
			       RZG3S_PCI_PINTRCVIE_MSI, 0); /* [한국어] 상위 인터럽트 인에이블의 MSI 비트부터 끈다 — 순서가 setup 의 역이다 */

	/* Disable message receive interrupts */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSGRCVIE,
			       RZG3S_PCI_MSGRCVIE_MSG_RCV, 0); /* [한국어] 메시지 수신 인터럽트도 끈다 */

	/* Disable MSI receive enable */
	for (u8 reg_id = 0; reg_id < regs; reg_id++) /* [한국어] 상태 레지스터마다 */
		writel_relaxed(0, host->axi + RZG3S_PCI_MSIRE(reg_id)); /* [한국어] 그 벡터들의 수신을 끈다 */

	/* Disable MSI window */
	writel_relaxed(0, host->axi + RZG3S_PCI_MSIRCVWADRL); /* [한국어] 마지막으로 창 주소 워드에 0 을 써서 창을 닫는다. 활성 비트가 그 워드에 있어 함께 꺼진다 */
}

/* [한국어]
 * rzg3s_pcie_teardown_msi - MSI 관련 자원을 모두 놓는다
 *
 * @host: 컨트롤러 상태.
 *
 * 잡은 역순이다 — 하드웨어를 먼저 끄고, 인터럽트를 떼고, 도메인을 없애고,
 * 마지막에 메모리를 놓는다.
 *
 * 순서가 중요하다. 하드웨어를 끄기 전에 핸들러를 떼면 그사이 올라온
 * 인터럽트를 아무도 처리하지 않아 선이 계속 서 있게 되고, 도메인을 먼저
 * 없애면 핸들러가 사라진 도메인을 참조한다.
 *
 * dma_unmap_single 에 넘기는 크기가 매핑 때와 같은 size * 2 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(제거/오류 경로).
 *
 * 호출 체인:  rzg3s_pcie_teardown_irqdomain() → [이 함수]
 */
static void rzg3s_pcie_teardown_msi(struct rzg3s_pcie_host *host)
{
	size_t size = RZG3S_PCI_MSI_INT_NR * sizeof(u32); /* [한국어] 매핑 때 쓴 것과 같은 크기 */
	struct rzg3s_pcie_msi *msi = &host->msi; /* [한국어] 놓을 자원들이 담긴 MSI 상태 */

	rzg3s_pcie_msi_hw_teardown(host); /* [한국어] 하드웨어를 먼저 끈다 — 그 뒤 핸들러를 떼야 처리되지 않는 인터럽트가 남지 않는다 */

	free_irq(msi->irq, host); /* [한국어] 핸들러를 뗀다 */
	irq_domain_remove(msi->domain); /* [한국어] 그다음 도메인을 없앤다 */

	/* Free unused memory */
	dma_unmap_single(host->dev, msi->dma_addr, size * 2, DMA_BIDIRECTIONAL);
	free_pages(msi->pages, 0);
}

/* [한국어]
 * rzg3s_pcie_init_msi - MSI 전체(락·인터럽트·도메인·하드웨어)를 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 순서가 의존 관계 그대로다.
 *
 *   1) 비트맵 뮤텍스를 초기화한다. 도메인이 만들어지면 곧바로 벡터 할당이
 *      들어올 수 있으므로 그 전이어야 한다.
 *   2) DT 에서 "msi" 라는 이름의 인터럽트를 얻는다. 이름으로 찾으므로
 *      INTx 선들과 순서가 뒤바뀔 걱정이 없다.
 *   3) /proc/interrupts 에 보일 이름을 만든다.
 *   4) 도메인을 만든다.
 *   5) 핸들러를 건다. 상류 주석이 devm_request_irq 를 쓰지 않는 이유를
 *      밝힌다 — 이 드라이버가 클럭을 devm 이 아닌 방식으로 다루므로 둘을
 *      섞으면 해제 순서가 어긋나 미묘한 버그가 생긴다.
 *   6) 목적지 메모리를 잡고 하드웨어를 세운다.
 *
 * 5) 를 4) 뒤에 두는 것이 중요하다. 핸들러가 걸린 뒤 인터럽트가 오면 곧바로
 * 도메인을 참조하기 때문이다. 반대로 6) 이 마지막인 것은 그때까지 하드웨어가
 * MSI 를 올리지 않기 때문이다.
 *
 * 되돌리기가 라벨 둘로 대칭을 이룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rzg3s_pcie_init_irqdomain() → [이 함수]
 *               → rzg3s_pcie_msi_allocate_domains() → rzg3s_pcie_msi_setup()
 */
static int rzg3s_pcie_init_msi(struct rzg3s_pcie_host *host)
{
	struct platform_device *pdev = to_platform_device(host->dev); /* [한국어] DT 인터럽트를 이름으로 찾으려면 플랫폼 디바이스가 필요하다 */
	struct rzg3s_pcie_msi *msi = &host->msi; /* [한국어] 채울 MSI 상태 */
	struct device *dev = host->dev; /* [한국어] 로그와 devm 할당의 기준 */
	const char *devname; /* [한국어] /proc/interrupts 에 보일 이름 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = devm_mutex_init(dev, &msi->map_lock); /* [한국어] 비트맵 뮤텍스를 먼저 초기화한다 — 도메인이 생기면 곧바로 벡터 할당이 들어올 수 있다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 그대로 올린다 */

	msi->irq = platform_get_irq_byname(pdev, "msi"); /* [한국어] DT 에서 "msi" 라는 이름의 인터럽트를 찾는다. 이름으로 찾으므로 INTx 선들과 순서가 뒤바뀔 걱정이 없다 */
	if (msi->irq < 0) /* [한국어] 그런 인터럽트가 없으면 */
		return dev_err_probe(dev, msi->irq, "Failed to get MSI IRQ!\n"); /* [한국어] MSI 를 쓸 수 없다 */

	devname = devm_kasprintf(dev, GFP_KERNEL, "%s-msi", dev_name(dev)); /* [한국어] "<장치이름>-msi" 형태로 만든다 */
	if (!devname) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 이름 없이 등록할 수 없다 */

	ret = rzg3s_pcie_msi_allocate_domains(msi); /* [한국어] 도메인을 먼저 만든다 — 핸들러보다 앞서야 인터럽트가 갈 곳이 있다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] devm 자원만 잡은 상태라 되돌릴 것이 없다 */

	/*
	 * Don't use devm_request_irq() as the driver uses non-devm helpers
	 * to control clocks. Mixing them may lead to subtle bugs.
	 */
	ret = request_irq(msi->irq, rzg3s_pcie_msi_irq, 0, devname, host); /* [한국어] 상류 주석대로 devm 판을 쓰지 않는다. 핸들러가 걸리는 순간부터 인터럽트가 들어올 수 있다 */
	if (ret) { /* [한국어] 등록에 실패했으면 */
		dev_err_probe(dev, ret, "Failed to request IRQ: %d\n", ret); /* [한국어] 알리고 */
		goto free_domains; /* [한국어] 만든 도메인을 되돌린다 */
	}

	ret = rzg3s_pcie_msi_setup(host); /* [한국어] 마지막으로 목적지 메모리를 잡고 하드웨어를 켠다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err_probe(dev, ret, "Failed to setup MSI!\n"); /* [한국어] 알리고 */
		goto free_irq; /* [한국어] 핸들러와 도메인을 차례로 되돌린다 */
	}

	return 0; /* [한국어] MSI 준비 완료 */

free_irq: /* [한국어] setup 실패가 여기로 온다 */
	free_irq(msi->irq, host); /* [한국어] 핸들러를 뗀다 */
free_domains: /* [한국어] 핸들러 등록 실패는 여기로 온다 */
	irq_domain_remove(msi->domain); /* [한국어] 도메인을 없앤다 */
	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_intx_irq_ack - 이 INTx 선의 상태 비트를 지운다
 *
 * @d: IRQ 코어가 넘기는 irq_data. hwirq 가 0~3(INTA~INTD)이다.
 *
 * R/W1C 레지스터라 해당 비트에 1 을 쓰면 지워진다. 읽고-고쳐-쓰기가 아니라
 * 경쟁이 없고, 그래서 mask/unmask 와 달리 hw_lock 을 잡지 않는다 —
 * 레지스터 정의 위의 상류 주석이 그 사실을 밝혀 둔다.
 *
 * MSI 판과 달리 chip_data 가 곧바로 host 다. INTx 도메인은
 * irq_set_chip_data 로 domain->host_data(즉 host)를 넣기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트. handle_level_irq 가 핸들러 전에 부른다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → rzg3s_pcie_update_bits()
 */
static void rzg3s_pcie_intx_irq_ack(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d); /* [한국어] INTx 도메인은 chip_data 에 host 를 직접 넣으므로 한 번에 되찾는다 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIS,
			       RZG3S_PCI_PINTRCVIS_INTX(d->hwirq),
			       RZG3S_PCI_PINTRCVIS_INTX(d->hwirq)); /* [한국어] R/W1C 라 해당 선의 비트에 1 을 써서 지운다. 읽고-고쳐-쓰기가 아니어서 락이 필요 없다 */
}

/* [한국어]
 * rzg3s_pcie_intx_irq_mask - 이 INTx 선을 마스크한다
 *
 * @d: IRQ 코어가 넘기는 irq_data.
 *
 * 인에이블 레지스터의 해당 비트를 0 으로 만든다. 읽고-고쳐-쓰기라 같은
 * 레지스터의 다른 비트(다른 INTx 선과 MSI 비트)를 건드리는 경로와 경쟁하고,
 * 그래서 host->hw_lock 으로 감싼다. 실제로 rzg3s_pcie_msi_hw_setup() 이
 * 같은 레지스터의 MSI 비트를 건드린다.
 *
 * level 인터럽트라 마스크가 실질적인 흐름 제어 수단이다 — 핸들러가 끝날
 * 때까지 코어가 이 선을 막아 둔다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스 컨텍스트.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → rzg3s_pcie_update_bits()
 */
static void rzg3s_pcie_intx_irq_mask(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d); /* [한국어] chip_data 로 넣어 둔 컨트롤러 상태 */

	guard(raw_spinlock_irqsave)(&host->hw_lock); /* [한국어] 인에이블 레지스터는 읽고-고쳐-쓰기라 다른 선·MSI 비트와 경쟁한다 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE,
			       RZG3S_PCI_PINTRCVIE_INTX(d->hwirq), 0); /* [한국어] 해당 선의 인에이블 비트를 0 으로 내려 막는다 */
}

/* [한국어]
 * rzg3s_pcie_intx_irq_unmask - 이 INTx 선의 마스크를 푼다
 *
 * @d: IRQ 코어가 넘기는 irq_data.
 *
 * mask 의 짝이며 같은 비트를 1 로 만든다. 락과 그 이유도 같다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스 컨텍스트.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → rzg3s_pcie_update_bits()
 */
static void rzg3s_pcie_intx_irq_unmask(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d); /* [한국어] chip_data 로 넣어 둔 컨트롤러 상태 */

	guard(raw_spinlock_irqsave)(&host->hw_lock); /* [한국어] mask 와 같은 이유로 같은 락을 잡는다 */

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE,
			       RZG3S_PCI_PINTRCVIE_INTX(d->hwirq),
			       RZG3S_PCI_PINTRCVIE_INTX(d->hwirq)); /* [한국어] 해당 선의 인에이블 비트를 1 로 세워 다시 받는다 */
}

/* [한국어] INTx 네 선의 irq_chip.
 * compose_msi_msg 가 없는 것이 MSI 쪽과의 차이다 — INTx 는 메시지가 아니라
 * 실제 신호선이라 장치에 알려 줄 주소가 없다. rzg3s_pcie_intx_map() 이
 * 이것을 각 virq 에 붙인다. */
static struct irq_chip rzg3s_pcie_intx_irq_chip = {
	.name = "PCIe INTx", /* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_ack = rzg3s_pcie_intx_irq_ack, /* [한국어] 상태 비트를 지운다. handle_level_irq 가 핸들러 전에 부른다 */
	.irq_mask = rzg3s_pcie_intx_irq_mask, /* [한국어] 이 선을 막는다 */
	.irq_unmask = rzg3s_pcie_intx_irq_unmask, /* [한국어] 다시 받는다 */
};

/* [한국어]
 * rzg3s_pcie_intx_map - INTx 도메인의 hwirq 하나를 virq 에 연결한다
 *
 * @domain: INTx 도메인.
 * @irq:    가상 IRQ 번호.
 * @hwirq:  0~3(INTA~INTD).
 * @return: 늘 0.
 *
 * 선형 도메인은 vector 를 미리 잡지 않고, virq 가 필요할 때마다 이 map
 * 콜백으로 하나씩 설정한다. MSI 쪽이 alloc/free 로 여러 개를 한꺼번에
 * 다루는 것과 대비된다.
 *
 * 두 가지를 설정한다 — irq_chip 과 흐름 제어를 handle_level_irq 로 두고,
 * chip_data 에 host 를 넣어 위 세 콜백이 문맥을 되찾게 한다.
 *
 * handle_level_irq 인 것이 INTx 의 성격 그대로다. INTx 는 레벨 신호라
 * 장치가 내릴 때까지 유지되며, 코어가 핸들러 동안 선을 마스크해 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 열거 중 매핑 시).
 *
 * 호출 체인:  irq_domain 코어 → [이 함수] → irq_set_chip_and_handler()
 */
static int rzg3s_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &rzg3s_pcie_intx_irq_chip,
				 handle_level_irq); /* [한국어] irq_chip 과 흐름 제어를 지정한다. INTx 는 레벨 신호라 handle_level_irq 다 */
	irq_set_chip_data(irq, domain->host_data); /* [한국어] 콜백들이 되찾을 문맥으로 host 를 넣는다 */

	return 0; /* [한국어] 선형 도메인의 map 은 실패할 일이 없다 */
}

/* [한국어] INTx 도메인의 콜백 표.
 * map 은 virq 하나를 설정하고, xlate 는 DT 의 인터럽트 지정(#interrupt-cells
 * 가 1 이든 2 든)을 hwirq 로 해석하는 표준 구현이다. MSI 쪽이 alloc/free 로
 * 여러 벡터를 한꺼번에 다루는 것과 대비된다. */
static const struct irq_domain_ops rzg3s_pcie_intx_domain_ops = {
	.map = rzg3s_pcie_intx_map, /* [한국어] virq 하나를 설정한다 */
	.xlate = irq_domain_xlate_onetwocell, /* [한국어] DT 의 인터럽트 지정을 hwirq 로 바꾼다 */
};

/* [한국어]
 * rzg3s_pcie_init_irqdomain - INTx 체인 핸들러와 도메인, 그리고 MSI 를 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. DT 에 INTx 선이 없거나 도메인 생성이 실패하면 -EINVAL,
 *          MSI 쪽 실패는 그 오류.
 *
 * probe 가 rzg3s_pcie_host_setup() 의 init 콜백으로 넘기는 함수다.
 *
 * INTx 는 선 네 개를 각각 DT 에서 이름으로 찾는다. 이름을 "inta"~"intd" 로
 * 그때그때 만들어 쓰는데, 버퍼가 5바이트인 것이 네 글자와 종결 문자에
 * 정확히 맞는다.
 *
 * 찾은 번호를 host->intx_irqs[] 에 순서대로 넣어 두는 것이 중요하다.
 * 체인 핸들러가 "현재 IRQ 번호 - intx_irqs[0]" 로 어느 선인지 알아내기
 * 때문이다.
 *
 * 도메인은 선형(linear)으로 만든다. hwirq 가 0~3 로 조밀해 트리가 필요
 * 없기 때문이다. 만든 뒤 버스 토큰을 WIRED 로 표시해, MSI 도메인과 구분되게
 * 한다.
 *
 * MSI 는 빌드 설정으로 갈린다. 실패하면 방금 만든 INTx 도메인을 되돌린
 * 뒤 오류를 올린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rzg3s_pcie_host_setup() → [이 함수] → rzg3s_pcie_init_msi()
 */
static int rzg3s_pcie_init_irqdomain(struct rzg3s_pcie_host *host)
{
	struct device *dev = host->dev; /* [한국어] 로그와 도메인 fwnode 의 기준 */
	struct platform_device *pdev = to_platform_device(dev); /* [한국어] DT 인터럽트를 이름으로 찾으려면 플랫폼 디바이스가 필요하다 */

	for (int i = 0; i < PCI_NUM_INTX; i++) { /* [한국어] INTA~INTD 네 선을 차례로 잡는다 */
		char irq_name[5] = {0}; /* [한국어] "inta" 네 글자와 종결 문자에 정확히 맞는 버퍼 */
		int irq; /* [한국어] 이번 선의 인터럽트 번호 */

		scnprintf(irq_name, ARRAY_SIZE(irq_name), "int%c", 'a' + i); /* [한국어] i 에 맞춰 소문자 이름을 만든다 — DT 의 interrupt-names 가 소문자다 */

		irq = platform_get_irq_byname(pdev, irq_name); /* [한국어] 그 이름으로 인터럽트를 찾는다 */
		if (irq < 0) /* [한국어] 없으면 */
			return dev_err_probe(dev, -EINVAL,
					     "Failed to parse and map INT%c IRQ\n",
					     'A' + i); /* [한국어] 로그에는 대문자로 보여 준다 */

		host->intx_irqs[i] = irq; /* [한국어] 번호를 기억해 둔다 — 체인 핸들러가 [0] 을 기준으로 몇 번째 선인지 계산한다 */
		irq_set_chained_handler_and_data(irq,
						 rzg3s_pcie_intx_irq_handler,
						 host); /* [한국어] 네 선 모두 같은 체인 핸들러에 host 를 함께 걸어 둔다 */
	}

	host->intx_domain = irq_domain_create_linear(dev_fwnode(dev),
						     PCI_NUM_INTX,
						     &rzg3s_pcie_intx_domain_ops,
						     host); /* [한국어] hwirq 가 0~3 로 조밀하므로 선형 도메인이면 충분하다 */
	if (!host->intx_domain) /* [한국어] 만들지 못했으면 */
		return dev_err_probe(dev, -EINVAL,
				     "Failed to add irq domain for INTx IRQs\n"); /* [한국어] 앞서 건 체인 핸들러를 떼지 않고 돌아가는데, 이 지점의 상류 코드가 그렇다 */
	irq_domain_update_bus_token(host->intx_domain, DOMAIN_BUS_WIRED); /* [한국어] MSI 도메인과 구분되도록 이 도메인이 실제 신호선용임을 표시한다 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* [한국어] 커널에 MSI 지원이 들어 있을 때만 */
		int ret = rzg3s_pcie_init_msi(host); /* [한국어] MSI 쪽 전체를 세운다 */

		if (ret) { /* [한국어] 실패했으면 */
			irq_domain_remove(host->intx_domain); /* [한국어] 방금 만든 INTx 도메인을 되돌리고 */
			return ret; /* [한국어] 오류를 올린다 */
		}
	}

	return 0; /* [한국어] 인터럽트 준비 완료 */
}

/* [한국어]
 * rzg3s_pcie_teardown_irqdomain - MSI 와 INTx 도메인을 되돌린다
 *
 * @host: 컨트롤러 상태.
 *
 * init 의 역순이다. MSI 를 먼저 정리하고 INTx 도메인을 없앤다.
 *
 * INTx 체인 핸들러를 여기서 떼지 않는다는 점을 짚어 둔다. 도메인이 사라진
 * 뒤에도 핸들러는 걸려 있는 셈인데, 이 지점에서는 이미 컨트롤러가 꺼지거나
 * 곧 꺼지므로 인터럽트가 올라오지 않는 것으로 보인다. 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_setup()(되돌리기) / rzg3s_pcie_probe()(실패 경로)
 *               → [이 함수] → rzg3s_pcie_teardown_msi()
 */
static void rzg3s_pcie_teardown_irqdomain(struct rzg3s_pcie_host *host)
{
	if (IS_ENABLED(CONFIG_PCI_MSI)) /* [한국어] MSI 를 세웠던 구성에서만 */
		rzg3s_pcie_teardown_msi(host); /* [한국어] 그 자원을 모두 놓는다 */

	irq_domain_remove(host->intx_domain); /* [한국어] 그다음 INTx 도메인을 없앤다 */
}

/* [한국어]
 * rzg3s_pcie_set_max_link_speed - 링크 속도를 가능한 최대치로 올린다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공(또는 바꿀 필요가 없었음). LTSSM 대기·속도 변경 대기가
 *          시간을 넘기면 폴링 오류, 지원 속도를 해석할 수 없으면 -EINVAL.
 *
 * 링크가 선 뒤 한 번 불려 속도를 끌어올린다. 실패해도 링크는 살아 있으므로
 * 호출자가 정보 로그만 남기고 계속 간다.
 *
 * 절차가 이렇다.
 *
 *   1) LTSSM 이 L0 가 될 때까지 기다린다. 상류 주석이 매뉴얼 절을 인용해
 *      그 조건을 밝힌다 — 속도 변경은 L0 에서만 할 수 있다.
 *   2) 하드웨어가 광고하는 최대 속도를 링크 능력 레지스터에서 읽는다.
 *   3) DT 의 max-link-speed 는 상한으로만 쓴다. 상류 주석대로, 지정되어
 *      있고 하드웨어 능력보다 낮을 때만 그 값으로 낮춘다.
 *   4) 그 속도에 대응하는 "이 속도까지 지원함" 마스크와 LNKCTL2 에 넣을
 *      코드를 정한다. 8.0GT/5.0GT 둘만 다루며, 그 밖의 값은 상류 주석대로
 *      일어나지 않아야 하는 경우다.
 *   5) 현재 속도가 이미 목표와 같거나, 상대 장치가 그 속도를 지원하지
 *      않으면 아무 일도 하지 않고 돌아간다. 상대의 지원 속도는 컨트롤러가
 *      PCSTAT2 에 기록해 둔 값에서 읽고, 예약 비트를 떨어내려고 위에서
 *      만든 마스크로 한 번 거른다.
 *   6) 목표 속도를 LNKCTL2 에 적고, 컨트롤러에 변경을 요청한다. 요청
 *      레지스터에 넣는 값이 link_speed - 1 인데, LNKCTL2 의 코드가 1부터
 *      시작하고 이 필드는 0부터 세기 때문으로 보인다.
 *   7) 완료 비트를 기다린다.
 *   8) 요청 비트를 내린다. 상류 주석이 매뉴얼을 인용해, 완료를 확인한
 *      뒤에 내려야 한다고 밝힌다. 그래서 폴링이 실패했더라도 이 단계는
 *      건너뛰지 않고 반드시 실행된다 — ret 를 곧바로 반환하지 않고 아래에
 *      두는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. readl_poll_timeout(비-atomic 판)이라
 * 잠들 수 있다.
 *
 * 호출 체인:  rzg3s_pcie_host_setup() → [이 함수]
 */
static int rzg3s_pcie_set_max_link_speed(struct rzg3s_pcie_host *host)
{
	u32 remote_supported_link_speeds, max_supported_link_speeds; /* [한국어] 상대 장치가 지원하는 속도들과, 목표 속도까지의 지원 마스크 */
	u32 cs2, tmp, pcie_cap = RZG3S_PCI_CFG_PCIEC; /* [한국어] 상태 레지스터 2 의 값, 폴링용 임시 값, 그리고 RC config 안의 PCIe capability 오프셋 */
	u32 cur_link_speed, link_speed, hw_max_speed; /* [한국어] 현재 속도, LNKCTL2 에 넣을 코드, 목표로 삼을 최대 속도 */
	u8 ltssm_state_l0 = 0xc; /* [한국어] LTSSM 의 L0 상태 값. 속도 변경은 이 상태에서만 할 수 있다 */
	u32 lnkcap; /* [한국어] 링크 능력 레지스터 값 */
	int ret; /* [한국어] 폴링 결과 */
	u16 ls; /* [한국어] 링크 상태 레지스터 값(16비트) */

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section 34.6.3 Caution
	 * when Changing the Speed Spontaneously) link speed change can be done
	 * only when the LTSSM is in L0.
	 */
	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT1, tmp,
				 FIELD_GET(RZG3S_PCI_PCSTAT1_LTSSM_STATE, tmp) == ltssm_state_l0,
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI,
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI *
				 PCIE_LINK_WAIT_MAX_RETRIES); /* [한국어] LTSSM 이 L0 가 될 때까지 90ms 씩 최대 10번 기다린다 */
	if (ret) /* [한국어] 끝내 L0 가 아니면 */
		return ret; /* [한국어] 속도를 바꿀 수 없으므로 물러난다 */

	ls = readw_relaxed(host->pcie + pcie_cap + PCI_EXP_LNKSTA); /* [한국어] 현재 링크 속도가 담긴 표준 상태 레지스터 */
	cs2 = readl_relaxed(host->axi + RZG3S_PCI_PCSTAT2); /* [한국어] 상대 장치의 지원 속도가 담긴 컨트롤러 상태 레지스터 */

	/* Read hardware supported link speed from Link Capabilities Register */
	lnkcap = readl_relaxed(host->pcie + pcie_cap + PCI_EXP_LNKCAP); /* [한국어] 하드웨어가 광고하는 능력 */
	hw_max_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, lnkcap); /* [한국어] 그중 지원 속도 필드 */

	/*
	 * Use DT max-link-speed only as a limit. If specified and lower
	 * than hardware capability, cap to that value.
	 */
	if (host->max_link_speed > 0 && host->max_link_speed < hw_max_speed) /* [한국어] DT 값이 있고 하드웨어 능력보다 낮을 때만 */
		hw_max_speed = host->max_link_speed; /* [한국어] 그 값으로 낮춘다 — 상류 주석대로 DT 는 상한으로만 쓴다 */

	switch (pcie_get_link_speed(hw_max_speed)) { /* [한국어] 목표 속도를 세대별로 갈라 처리한다 */
	case PCIE_SPEED_8_0GT: /* [한국어] Gen3(8.0GT/s) */
		max_supported_link_speeds = GENMASK(PCI_EXP_LNKSTA_CLS_8_0GB - 1, 0); /* [한국어] 그 세대까지의 모든 속도를 나타내는 마스크 */
		link_speed = PCI_EXP_LNKCTL2_TLS_8_0GT; /* [한국어] LNKCTL2 의 목표 속도 코드 */
		break; /* [한국어] 다음 단계로 */
	case PCIE_SPEED_5_0GT: /* [한국어] Gen2(5.0GT/s) */
		max_supported_link_speeds = GENMASK(PCI_EXP_LNKSTA_CLS_5_0GB - 1, 0); /* [한국어] 그 세대까지의 마스크 */
		link_speed = PCI_EXP_LNKCTL2_TLS_5_0GT; /* [한국어] 그 세대의 코드 */
		break; /* [한국어] 다음 단계로 */
	default: /* [한국어] 그 밖의 속도 */
		/* Should not happen */
		return -EINVAL; /* [한국어] 상류 주석대로 일어나지 않아야 하는 경우다 */
	}

	cur_link_speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, ls); /* [한국어] 지금 실제로 협상된 속도 */
	remote_supported_link_speeds = FIELD_GET(RZG3S_PCI_PCSTAT2_SDRIRE, cs2); /* [한국어] 컨트롤러가 기록해 둔 상대 장치의 지원 속도들 */
	/* Drop reserved bits */
	remote_supported_link_speeds &= max_supported_link_speeds; /* [한국어] 상류 주석대로 예약 비트를 떨어내려고 위에서 만든 마스크로 거른다 */

	/*
	 * Return if target link speed is already set or the connected device
	 * doesn't support it.
	 */
	if (cur_link_speed == hw_max_speed ||
	    remote_supported_link_speeds != max_supported_link_speeds) /* [한국어] 이미 목표 속도이거나, 상대가 그 속도를 모두 지원하지는 않으면 */
		return 0; /* [한국어] 바꿀 이유가 없거나 바꿀 수 없으므로 성공으로 돌아간다 */

	/* Set target Link speed */
	rzg3s_pcie_update_bits(host->pcie, pcie_cap + PCI_EXP_LNKCTL2,
			       PCI_EXP_LNKCTL2_TLS,
			       FIELD_PREP(PCI_EXP_LNKCTL2_TLS, link_speed)); /* [한국어] 표준 LNKCTL2 의 목표 속도 필드를 정한다 */

	/* Request link speed change */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PCCTRL2,
			       RZG3S_PCI_PCCTRL2_LS_CHG_REQ |
			       RZG3S_PCI_PCCTRL2_LS_CHG,
			       RZG3S_PCI_PCCTRL2_LS_CHG_REQ |
			       FIELD_PREP(RZG3S_PCI_PCCTRL2_LS_CHG,
					  link_speed - 1)); /* [한국어] 컨트롤러에 변경을 요청한다. 필드 값이 코드에서 1 을 뺀 것인데, LNKCTL2 의 코드가 1부터 시작하는 반면 이 필드는 0부터 세기 때문으로 보인다 */

	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT2, cs2,
				 (cs2 & RZG3S_PCI_PCSTAT2_LS_CHG_DONE),
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI,
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI *
				 PCIE_LINK_WAIT_MAX_RETRIES); /* [한국어] 완료 비트가 설 때까지 기다린다 */

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section 34.6.3 Caution
	 * when Changing the Speed Spontaneously) the PCI_PCCTRL2_LS_CHG_REQ
	 * should be de-asserted after checking for PCI_PCSTAT2_LS_CHG_DONE.
	 */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PCCTRL2,
			       RZG3S_PCI_PCCTRL2_LS_CHG_REQ, 0); /* [한국어] 상류 주석대로 완료를 확인한 뒤에 요청 비트를 내려야 한다. 폴링이 실패했더라도 이 단계는 건너뛰지 않는다 */

	return ret; /* [한국어] 폴링 결과를 올린다. 실패해도 링크는 살아 있어 호출자가 정보 로그만 남긴다 */
}

/* [한국어]
 * rzg3s_pcie_config_init - RC 의 config 공간 기본값을 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, -ENODEV 는 DT 에 버스 번호 범위가 없는 경우.
 *
 * RC 자신의 config 공간에는 보통 쓸 수 없는 필드가 있고, 그것을 열어 주는
 * 것이 PERM 레지스터다. 이 함수는 그 문을 열고 필요한 것을 적은 뒤 닫는다.
 *
 *   - BAR 마스크 두 워드에 모두 1 을 적는다. 상류 주석대로 매뉴얼이 초기화
 *      시 그렇게 하라고 권고한다.
 *   - 클래스 코드를 PCI-to-PCI 브리지로 적는다. 상류 주석이 밝히듯 RZ/G3E 는
 *      이 설정이 반드시 필요하고, RZ/G3S 에서는 하드웨어 기본값과 같아
 *      해가 없다. 그래서 SoC 를 가리지 않고 무조건 적는다.
 *   - 버스 번호 셋을 적는다. DT 의 버스 자원에서 시작이 primary, 그다음이
 *      secondary, 끝이 subordinate 다.
 *
 * 버스 번호는 PERM 문 밖에서 적는다는 점을 짚어 둔다. 그 필드들은 보호
 * 대상이 아니어서 언제든 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(host_init).
 *
 * 호출 체인:  rzg3s_pcie_host_init() → [이 함수]
 */
static int rzg3s_pcie_config_init(struct rzg3s_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host); /* [한국어] DT 의 버스 번호 자원을 얻기 위해 바깥 브리지를 되찾는다 */
	u32 mask = GENMASK(31, 8); /* [한국어] 클래스 코드가 놓인 상위 24비트. 하위 8비트는 리비전이라 건드리면 안 된다 */
	struct resource_entry *ft; /* [한국어] 찾은 버스 자원 항목 */
	struct resource *bus; /* [한국어] 그 안의 자원 */
	u8 subordinate_bus; /* [한국어] 이 브리지 아래 가장 높은 버스 번호 */
	u8 secondary_bus; /* [한국어] 바로 아래 버스 번호 */
	u8 primary_bus; /* [한국어] 이 브리지가 붙어 있는 버스 번호 */

	ft = resource_list_first_type(&bridge->windows, IORESOURCE_BUS); /* [한국어] 브리지 창 목록에서 버스 번호 자원을 찾는다 */
	if (!ft) /* [한국어] DT 가 버스 범위를 주지 않았으면 */
		return -ENODEV; /* [한국어] 브리지 레지스터를 채울 수 없다 */

	bus = ft->res; /* [한국어] 찾은 자원 */
	primary_bus = bus->start; /* [한국어] 범위의 시작이 이 브리지 자신이 붙은 버스다 */
	secondary_bus = bus->start + 1; /* [한국어] 그다음이 이 브리지가 만드는 버스다 */
	subordinate_bus = bus->end; /* [한국어] 범위의 끝이 아래로 뻗을 수 있는 마지막 버스다 */

	/* Enable access control to the CFGU */
	writel_relaxed(RZG3S_PCI_PERM_CFG_HWINIT_EN,
		       host->axi + RZG3S_PCI_PERM); /* [한국어] 평소 읽기 전용인 config 필드를 쓸 수 있게 문을 연다 */

	/* HW manual recommends to write 0xffffffff on initialization */
	writel_relaxed(0xffffffff, host->pcie + RZG3S_PCI_CFG_BARMSK00L); /* [한국어] 상류 주석대로 매뉴얼이 초기화 시 이 값을 권고한다 */
	writel_relaxed(0xffffffff, host->pcie + RZG3S_PCI_CFG_BARMSK00U); /* [한국어] 상위 워드도 같다 */

	/*
	 * Explicitly program class code. RZ/G3E requires this configuration.
	 * Harmless for RZ/G3S where this matches the hardware default.
	 */
	rzg3s_pcie_update_bits(host->pcie, PCI_CLASS_REVISION, mask,
			       field_prep(mask, PCI_CLASS_BRIDGE_PCI_NORMAL)); /* [한국어] 클래스 코드를 PCI-to-PCI 브리지로 못박는다. 상류 주석대로 RZ/G3E 에는 필수이고 RZ/G3S 에서는 기본값과 같아 무해하다 */

	/* Disable access control to the CFGU */
	writel_relaxed(0, host->axi + RZG3S_PCI_PERM); /* [한국어] 문을 닫는다. 열어 둔 채로 두면 뜻하지 않은 쓰기가 보호 필드를 바꿀 수 있다 */

	/* Update bus info */
	writeb_relaxed(primary_bus, host->pcie + PCI_PRIMARY_BUS); /* [한국어] 이 브리지가 붙은 버스 번호 */
	writeb_relaxed(secondary_bus, host->pcie + PCI_SECONDARY_BUS); /* [한국어] 이 브리지가 만드는 버스 번호 */
	writeb_relaxed(subordinate_bus, host->pcie + PCI_SUBORDINATE_BUS); /* [한국어] 아래로 뻗는 마지막 버스 번호. 이 셋은 보호 대상이 아니라 문 밖에서 쓴다 */

	return 0; /* [한국어] config 기본값 설정 완료 */
}

/* [한국어]
 * rzg3s_pcie_config_post_init - RZ/G3S 의 설정 후 리셋 해제
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 리셋 프레임워크의 오류.
 *
 * RZ/G3S 표의 config_post_init 자리에 들어간다. 이 SoC 는 설정이 끝난 뒤
 * 풀어야 할 리셋들을 DT 에서 이름으로 받아 두고(rst_b, rst_ps_b, rst_gp_b,
 * rst_rsm_b), 그것을 한꺼번에 푼다.
 *
 * 같은 일을 RZ/G3E 는 리셋 프레임워크가 아니라 컨트롤러의 RESET 레지스터
 * 비트로 한다(rzg3e_pcie_config_post_init). 같은 단계가 SoC 마다 전혀 다른
 * 수단으로 이뤄지는 것이 이 함수 포인터가 존재하는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_init() → data->config_post_init → [이 함수]
 */
static int rzg3s_pcie_config_post_init(struct rzg3s_pcie_host *host)
{
	return reset_control_bulk_deassert(host->data->num_cfg_resets,
					   host->cfg_resets); /* [한국어] DT 에서 이름으로 잡아 둔 설정 리셋들을 한꺼번에 푼다 */
}

/* [한국어]
 * rzg3s_pcie_config_deinit - RZ/G3S 의 설정 리셋을 다시 건다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 리셋 프레임워크의 오류.
 *
 * config_post_init 의 역이다. 같은 리셋 묶음을 assert 로 되돌린다.
 *
 * 서스펜드와 오류 되돌리기 양쪽에서 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_init()(되돌리기) / rzg3s_pcie_suspend_noirq() /
 *               rzg3s_pcie_probe()(실패 경로) → data->config_deinit → [이 함수]
 */
static int rzg3s_pcie_config_deinit(struct rzg3s_pcie_host *host)
{
	return reset_control_bulk_assert(host->data->num_cfg_resets,
					 host->cfg_resets); /* [한국어] 같은 묶음을 다시 건다 */
}

/* [한국어]
 * rzg3e_pcie_config_pre_init - RZ/G3E 에서 설정 전에 풀어야 할 리셋 비트
 *
 * @host: 컨트롤러 상태.
 *
 * RZ/G3E 표에만 있는 콜백이다. 이 SoC 는 리셋이 컨트롤러의 RESET 레지스터
 * 안에 비트로 있고, 그 비트를 정해진 순서로 풀어야 한다. 그중 LOAD_B 와
 * CFG_B 를 설정 전에 먼저 푸는 것이 이 함수다.
 *
 * RZ/G3S 표에는 이 자리가 비어 있어(NULL) 호출자가 건너뛴다. 그 SoC 는
 * 같은 두 리셋을 DT 의 power_resets 목록("rst_cfg_b", "rst_load_b")에 넣어
 * 전원 리셋 해제 단계에서 함께 푼다.
 *
 * 이 콜백의 유무가 rzg3s_pcie_host_init() 의 되돌리기 조건으로도 쓰인다는
 * 점이 눈에 띈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_init() / rzg3s_pcie_suspend_noirq()(되돌리기)
 *               → data->config_pre_init → [이 함수]
 */
static void rzg3e_pcie_config_pre_init(struct rzg3s_pcie_host *host)
{
	u32 mask = RZG3S_PCI_RESET_RST_LOAD_B | RZG3S_PCI_RESET_RST_CFG_B; /* [한국어] 설정 전에 풀어야 할 두 비트 */

	/* De-assert LOAD_B and CFG_B */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_RESET, mask, mask); /* [한국어] 그 비트들을 1 로 만들어 리셋을 푼다(이름의 _B 가 active-low 를 뜻한다) */
}

/* [한국어]
 * rzg3e_pcie_config_deinit - RZ/G3E 의 모든 리셋 비트를 다시 건다
 *
 * @host: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * RESET 레지스터에 0 을 통째로 써서 모든 리셋을 걸어 버린다. 비트별로
 * 되돌리지 않으므로 여러 번 불려도 결과가 같다 — rzg3s_pcie_host_init() 의
 * 되돌리기가 이 함수를 두 번 부를 수 있는데, 그래도 문제가 없는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  data->config_deinit → [이 함수]
 */
static int rzg3e_pcie_config_deinit(struct rzg3s_pcie_host *host)
{
	writel_relaxed(0, host->axi + RZG3S_PCI_RESET); /* [한국어] 모든 리셋 비트를 한꺼번에 0 으로 만들어 건다. 비트별로 되돌리지 않으므로 여러 번 불려도 결과가 같다 */
	return 0; /* [한국어] 실패할 일이 없다 */
}

/* [한국어]
 * rzg3e_pcie_config_post_init - RZ/G3E 의 리셋 비트를 순서대로 푼다
 *
 * @host: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * 이 SoC 의 기동 절차가 그대로 드러나는 함수다.
 *
 *   1) PS_B, GP_B, RST_B 를 먼저 푼다.
 *   2) 같은 레지스터를 더미로 읽어 그 쓰기를 하드웨어까지 밀어낸다.
 *      writel_relaxed 를 쓰므로 이 읽기가 없으면 아래 지연이 실제 반영
 *      시점보다 먼저 끝날 수 있다.
 *   3) 상류 주석이 매뉴얼 표를 인용해 밝히듯 500us 이상 기다린다.
 *   4) 그다음에야 OUT_B 와 RSM_B 를 푼다.
 *
 * 순서와 지연이 하드웨어 요구사항이라 어느 하나도 생략할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. fsleep 으로 잠든다.
 *
 * 호출 체인:  rzg3s_pcie_host_init() → data->config_post_init → [이 함수]
 */
static int rzg3e_pcie_config_post_init(struct rzg3s_pcie_host *host)
{
	u32 mask = RZG3S_PCI_RESET_RST_PS_B | RZG3S_PCI_RESET_RST_GP_B |
		   RZG3S_PCI_RESET_RST_B; /* [한국어] 첫 단계에서 풀 세 비트 */

	/* De-assert PS_B, GP_B, RST_B */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_RESET, mask, mask); /* [한국어] 그 셋을 푼다 */

	/* Flush deassert */
	readl_relaxed(host->axi + RZG3S_PCI_RESET); /* [한국어] 더미 읽기로 위 쓰기를 하드웨어까지 밀어낸다. relaxed 쓰기라 이것이 없으면 아래 지연이 실제 반영보다 먼저 끝날 수 있다 */

	/*
	 * According to the RZ/G3E HW manual (Rev.1.15, Table 6.6-130
	 * Initialization Procedure (RC)), hardware requires >= 500us delay
	 * before final reset deassert.
	 */
	fsleep(500); /* [한국어] 상류 주석대로 매뉴얼이 요구하는 500us 이상의 지연 */

	/* De-assert OUT_B and RSM_B */
	mask = RZG3S_PCI_RESET_RST_OUT_B | RZG3S_PCI_RESET_RST_RSM_B; /* [한국어] 마지막 단계에서 풀 두 비트 */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_RESET, mask, mask); /* [한국어] 그 둘을 푼다. 이 시점에서 링크가 서기 시작한다 */

	return 0; /* [한국어] 실패할 일이 없다 */
}

/* [한국어]
 * rzg3s_pcie_irq_init - 컨트롤러의 인터럽트 상태를 깨끗이 하고 모두 끈다
 *
 * @host: 컨트롤러 상태.
 *
 * 링크를 세우기 전에 남아 있을 수 있는 인터럽트 상태를 지우고, 이 드라이버가
 * 쓰지 않는 인터럽트를 모두 꺼 둔다. 그렇게 하지 않으면 부트로더가 남긴
 * 상태 때문에 엉뚱한 인터럽트가 올라온다.
 *
 * 상류 주석이 ~0U 를 쓰는 근거를 밝힌다 — 매뉴얼상 하드웨어가 쓰이지 않는
 * 비트에 쓴 값을 무시하므로, 비트를 하나씩 세지 않고 전부 1 을 써도 안전하다.
 *
 * 지우는 것과 끄는 것이 짝을 이룬다. 상태 레지스터(...IS)에는 1 을 써서
 * 지우고, 인에이블 레지스터(...IE)에는 0 을 써서 끈다.
 *
 * 첫 번째 쓰기만 ~0U 가 아니라 두 비트를 명시하는데, 그 레지스터에는 이
 * 드라이버가 관심 있는 두 상태(데이터 링크 상승/하강, PM 진입 DLLP 수신)만
 * 지우면 되기 때문으로 보인다.
 *
 * INTx 와 MSI 의 인에이블은 여기서 건드리지 않는다. 그것은 각각 irq_chip
 * 의 unmask 와 rzg3s_pcie_msi_hw_setup() 이 맡는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(host_init).
 *
 * 호출 체인:  rzg3s_pcie_host_init() → [이 함수]
 */
static void rzg3s_pcie_irq_init(struct rzg3s_pcie_host *host)
{
	/*
	 * According to the HW manual of the RZ/G3S (Rev.1.10, sections
	 * corresponding to all registers written with ~0U), the hardware
	 * ignores value written to unused bits. Writing ~0U to these registers
	 * should be safe.
	 */

	/* Clear the link state and PM transitions */
	writel_relaxed(RZG3S_PCI_PEIS0_DL_UPDOWN |
		       RZG3S_PCI_PEIS0_RX_DLLP_PM_ENTER,
		       host->axi + RZG3S_PCI_PEIS0); /* [한국어] 이 드라이버가 보는 두 상태(데이터 링크 상승/하강, PM 진입 DLLP 수신)를 지운다 */

	/* Disable all interrupts */
	writel_relaxed(0, host->axi + RZG3S_PCI_PEIE0); /* [한국어] 그 인터럽트들은 쓰지 않으므로 모두 끈다 */

	/* Clear all parity and ecc error interrupts */
	writel_relaxed(~0U, host->axi + RZG3S_PCI_PEIS1); /* [한국어] 패리티/ECC 오류 상태를 모두 지운다. 상류 주석대로 하드웨어가 쓰이지 않는 비트를 무시하므로 전부 1 을 써도 안전하다 */

	/* Disable all parity and ecc error interrupts */
	writel_relaxed(0, host->axi + RZG3S_PCI_PEIE1); /* [한국어] 그 인터럽트도 끈다 */

	/* Clear all AXI master error interrupts */
	writel_relaxed(~0U, host->axi + RZG3S_PCI_AMEIS); /* [한국어] AXI 마스터 오류 상태를 지운다 */

	/* Clear all AXI slave error interrupts */
	writel_relaxed(~0U, host->axi + RZG3S_PCI_ASEIS1); /* [한국어] AXI 슬레이브 오류 상태를 지운다 */

	/* Clear all message receive interrupts */
	writel_relaxed(~0U, host->axi + RZG3S_PCI_MSGRCVIS); /* [한국어] 메시지 수신 상태를 지운다. MSI 를 켤 때 남은 상태가 없어야 한다 */
}

/* [한국어]
 * rzg3s_pcie_power_resets_deassert - 전원 인가 후 규정 시간을 기다린 뒤 리셋을 푼다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 그 밖에는 리셋 프레임워크의 오류.
 *
 * 상류 주석이 매뉴얼 절을 인용해 밝히듯, 전원이 들어온 뒤 리셋 해제까지
 * 5ms 를 기다려야 한다. 그 지연이 이 함수의 존재 이유이며, 그래서
 * reset_control_bulk_deassert 를 직접 부르지 않고 이 얇은 함수를 거친다.
 *
 * probe 와 resume 이 모두 이 함수를 쓰므로 지연 규칙을 한 곳에서 지킨다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. fsleep 으로 잠든다.
 *
 * 호출 체인:  rzg3s_pcie_probe() / rzg3s_pcie_resume_noirq() → [이 함수]
 */
static int rzg3s_pcie_power_resets_deassert(struct rzg3s_pcie_host *host)
{
	const struct rzg3s_pcie_soc_data *data = host->data; /* [한국어] 리셋 개수를 담고 있는 SoC 표 */

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section
	 * 34.5.1.2 De-asserting the Reset) the PCIe IP needs to wait 5ms from
	 * power on to the de-assertion of reset.
	 */
	fsleep(5000); /* [한국어] 상류 주석대로 전원 인가 후 리셋 해제까지 5ms 를 기다려야 한다 */
	return reset_control_bulk_deassert(data->num_power_resets,
					   host->power_resets); /* [한국어] 그다음에야 전원 리셋들을 푼다 */
}

/* [한국어]
 * rzg3s_pcie_resets_prepare_and_get - SoC 표에 적힌 리셋들을 이름으로 잡는다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, -ENOMEM 은 배열 할당 실패, 그 밖에는 리셋 조회 오류.
 *
 * 리셋 이름이 SoC 마다 다르고 개수도 달라, 표(data->power_resets 등)를 보고
 * bulk API 가 요구하는 배열을 런타임에 만든다.
 *
 * 두 묶음의 성격이 다르다.
 *   - power_resets 는 exclusive 로 잡는다. 반드시 있어야 하는 리셋이라,
 *     DT 에 없으면 그 자리에서 실패한다.
 *   - cfg_resets 는 optional 판으로 잡는다. RZ/G3E 표는 이 목록이 비어
 *     있고 개수도 0 이라(구조체 초기화가 두 필드를 채우지 않는다) 잡을
 *     것이 없기 때문이다.
 *
 * devm 으로 잡으므로 실패 경로에서 배열을 따로 놓지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rzg3s_pcie_probe() → [이 함수]
 *               → devm_reset_control_bulk_get_exclusive()
 */
static int rzg3s_pcie_resets_prepare_and_get(struct rzg3s_pcie_host *host)
{
	const struct rzg3s_pcie_soc_data *data = host->data; /* [한국어] 리셋 이름과 개수를 담고 있는 SoC 표 */
	unsigned int i; /* [한국어] 이름 복사 루프 인덱스 */
	int ret; /* [한국어] 리셋 조회 결과 */

	host->power_resets = devm_kmalloc_array(host->dev,
						data->num_power_resets,
						sizeof(*host->power_resets),
						GFP_KERNEL); /* [한국어] bulk API 가 요구하는 배열을 개수만큼 잡는다. devm 이라 실패 경로에서 놓지 않아도 된다 */
	if (!host->power_resets) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	for (i = 0; i < data->num_power_resets; i++) /* [한국어] 전원 리셋 개수만큼 */
		host->power_resets[i].id = data->power_resets[i]; /* [한국어] 표의 이름을 배열 항목에 꽂는다. bulk 조회가 이 이름으로 DT 를 찾는다 */

	host->cfg_resets = devm_kmalloc_array(host->dev,
					      data->num_cfg_resets,
					      sizeof(*host->cfg_resets),
					      GFP_KERNEL); /* [한국어] 설정 리셋 쪽 배열도 같은 방식으로 잡는다 */
	if (!host->cfg_resets) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	for (i = 0; i < data->num_cfg_resets; i++) /* [한국어] 설정 리셋 개수만큼. RZ/G3E 는 0 이라 돌지 않는다 */
		host->cfg_resets[i].id = data->cfg_resets[i]; /* [한국어] 표의 이름을 꽂는다 */

	ret = devm_reset_control_bulk_get_exclusive(host->dev,
						    data->num_power_resets,
						    host->power_resets); /* [한국어] 전원 리셋은 반드시 있어야 하므로 exclusive 로 잡는다 */
	if (ret) /* [한국어] DT 에 없으면 */
		return ret; /* [한국어] 그 오류를 그대로 올려 probe 가 실패한다 */

	return devm_reset_control_bulk_get_optional_exclusive(host->dev,
							      data->num_cfg_resets,
							      host->cfg_resets); /* [한국어] 설정 리셋은 optional 판으로 잡는다 — RZ/G3E 는 개수가 0 이라 잡을 것이 없다 */
}

/* [한국어]
 * rzg3s_pcie_host_parse_port - DT 자식 노드에서 포트 정보를 읽는다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 속성이 없으면 of_property_read_u32 의 오류,
 *          클럭을 못 얻으면 그 오류.
 *
 * DT 에서 PCIe 컨트롤러 노드 아래 자식 노드 하나가 루트 포트를 나타낸다.
 * 그 노드에서 세 가지를 읽는다 — 벤더 ID, 장치 ID, 그리고 "ref" 라는 이름의
 * 기준 클럭.
 *
 * 벤더/장치 ID 를 DT 에서 받는 것이 눈에 띈다. 이 컨트롤러는 그 값이
 * 하드웨어에 고정되어 있지 않아, rzg3s_pcie_host_init_port() 가 config
 * 공간에 직접 써 넣는다.
 *
 * of_get_next_child 의 결과를 __free(device_node) 로 받아, 함수를 벗어날 때
 * 참조 카운트가 자동으로 놓인다. 반환 경로가 넷이라 손으로 관리하면
 * 빠뜨리기 쉬운 자리다.
 *
 * 클럭은 devm 이 아닌 of_clk_get_by_name 으로 잡는다. 그래서 probe 의
 * 실패 경로와 remove 대신, 이 파일이 직접 clk_put 을 부른다 —
 * rzg3s_pcie_init_msi() 의 상류 주석이 밝히는 "devm 과 비-devm 을 섞지
 * 않는다" 는 방침과 같은 맥락이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rzg3s_pcie_probe() → [이 함수] → of_clk_get_by_name()
 */
static int rzg3s_pcie_host_parse_port(struct rzg3s_pcie_host *host)
{
	struct device_node *of_port __free(device_node) =
		of_get_next_child(host->dev->of_node, NULL); /* [한국어] DT 에서 첫 자식 노드(루트 포트)를 얻는다. __free 로 받아 함수를 벗어날 때 참조가 자동으로 놓인다 */
	struct rzg3s_pcie_port *port = &host->port; /* [한국어] 채울 포트 정보 */
	int ret; /* [한국어] DT 읽기 결과 */

	ret = of_property_read_u32(of_port, "vendor-id", &port->vendor_id); /* [한국어] RC 의 config 공간에 써 넣을 벤더 ID */
	if (ret) /* [한국어] 속성이 없으면 */
		return ret; /* [한국어] 그 오류를 올린다 */

	ret = of_property_read_u32(of_port, "device-id", &port->device_id); /* [한국어] 장치 ID 도 같은 방식으로 읽는다 */
	if (ret) /* [한국어] 속성이 없으면 */
		return ret; /* [한국어] 그 오류를 올린다 */

	port->refclk = of_clk_get_by_name(of_port, "ref"); /* [한국어] "ref" 라는 이름의 기준 클럭. devm 판이 아니므로 실패 경로와 suspend 가 직접 놓는다 */
	if (IS_ERR(port->refclk)) /* [한국어] 클럭을 못 얻었으면 */
		return PTR_ERR(port->refclk); /* [한국어] 그 오류를 올린다. of_port 는 __free 가 알아서 놓는다 */

	return 0; /* [한국어] 포트 정보 준비 완료 */
}

/* [한국어]
 * rzg3s_pcie_host_init_port - 포트 ID 를 써 넣고 클럭과 PHY 를 켠다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 클럭이나 PHY 가 실패하면 그 오류.
 *
 * 세 가지를 한다.
 *
 *   1) PERM 문을 열고 벤더/장치 ID 를 RC 의 config 공간에 써 넣은 뒤 닫는다.
 *      그 필드들은 보통 읽기 전용이라 이 문을 통해야 쓸 수 있다.
 *   2) 기준 클럭을 켠다.
 *   3) SoC 표에 PHY 초기화 함수가 있으면 부른다.
 *
 * 순서가 중요하다. PHY 설정은 클럭이 돌아야 반영되므로 2) 가 3) 보다 먼저다.
 *
 * PHY 가 실패하면 방금 켠 클럭을 되돌린다. ID 쓰기는 되돌리지 않는데,
 * 되돌릴 것이 없는 단순한 값 쓰기이고 컨트롤러가 곧 리셋될 것이기 때문이다.
 *
 * RZ/G3E 표에는 init_phy 가 없어 3) 을 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(host_init).
 *
 * 호출 체인:  rzg3s_pcie_host_init() → [이 함수] → data->init_phy()
 */
static int rzg3s_pcie_host_init_port(struct rzg3s_pcie_host *host)
{
	struct rzg3s_pcie_port *port = &host->port; /* [한국어] 써 넣을 ID 와 켤 클럭이 담긴 포트 정보 */
	struct device *dev = host->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	/* Enable access control to the CFGU */
	writel_relaxed(RZG3S_PCI_PERM_CFG_HWINIT_EN,
		       host->axi + RZG3S_PCI_PERM); /* [한국어] 벤더/장치 ID 는 보통 읽기 전용이라 문을 열어야 쓸 수 있다 */

	/* Update vendor ID and device ID */
	writew_relaxed(port->vendor_id, host->pcie + PCI_VENDOR_ID); /* [한국어] DT 가 지정한 벤더 ID 를 적는다 */
	writew_relaxed(port->device_id, host->pcie + PCI_DEVICE_ID); /* [한국어] 장치 ID 도 적는다 */

	/* Disable access control to the CFGU */
	writel_relaxed(0, host->axi + RZG3S_PCI_PERM); /* [한국어] 문을 닫는다 */

	ret = clk_prepare_enable(port->refclk); /* [한국어] PHY 설정이 반영되려면 클럭이 돌아야 하므로 먼저 켠다 */
	if (ret) /* [한국어] 못 켰으면 */
		return dev_err_probe(dev, ret, "Failed to enable refclk!\n"); /* [한국어] PHY 도 세울 수 없다 */

	/* Set the PHY, if any */
	if (host->data->init_phy) { /* [한국어] 소프트웨어가 채울 PHY 설정이 있는 SoC 에서만 */
		ret = host->data->init_phy(host); /* [한국어] 그 표를 써 넣는다 */
		if (ret) { /* [한국어] 실패했으면 */
			dev_err_probe(dev, ret, "Failed to set the PHY!\n"); /* [한국어] 알리고 */
			goto refclk_disable; /* [한국어] 방금 켠 클럭을 되돌린다 */
		}
	}

	return 0; /* [한국어] 포트 준비 완료 */

refclk_disable: /* [한국어] PHY 실패가 여기로 온다 */
	clk_disable_unprepare(port->refclk); /* [한국어] 클럭을 끈다. ID 쓰기는 되돌리지 않는데, 컨트롤러가 곧 리셋될 것이기 때문이다 */
	return ret; /* [한국어] 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_host_init - 컨트롤러를 설정하고 링크가 설 때까지 기다린다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일의 기동 절차 본체다.
 *
 *   1) SoC 전용 사전 설정(RZ/G3E 의 LOAD_B/CFG_B 해제). 없으면 건너뛴다.
 *   2) config 공간 기본값(BAR 마스크, 클래스 코드, 버스 번호).
 *   3) 포트 ID 와 클럭, PHY.
 *   4) SYSC 의 ASPM L1 허용. 그 기능이 없는 SoC 에서는 아무 일도 하지 않는다.
 *   5) 인터럽트 상태 초기화.
 *   6) SoC 전용 사후 설정(리셋 해제). 이 단계에서 링크가 서기 시작한다.
 *   7) 데이터 링크가 내려가 있지 않을 때까지 폴링한다. 다 서면 상태
 *      레지스터 값을 로그로 남긴다.
 *
 * 되돌리기가 세 라벨의 폭포 구조다. 실패 지점이 뒤일수록 더 많은 단계를
 * 되돌린다 — 링크 대기 실패는 사후 설정부터, 사후 설정·L1 허용 실패는
 * 클럭부터, 앞쪽 실패는 사전 설정만 되돌린다.
 *
 * 마지막 라벨의 조건이 눈에 띈다. config_pre_init 이 있을 때만 config_deinit
 * 을 부르므로, 그 콜백이 없는 RZ/G3S 에서는 앞쪽 실패 시 아무것도 되돌리지
 * 않는다. 그리고 링크 대기 실패로 들어온 경우 RZ/G3E 는 config_deinit 이
 * 두 번 불리는데, 그 구현이 RESET 레지스터에 0 을 통째로 쓰는 것이라 두 번
 * 불려도 결과가 같다. 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. readl_poll_timeout 으로 잠든다.
 *
 * 호출 체인:  rzg3s_pcie_host_setup() → [이 함수]
 *               → rzg3s_pcie_config_init() → rzg3s_pcie_host_init_port()
 *               → rzg3s_pcie_irq_init() → data->config_post_init()
 */
static int rzg3s_pcie_host_init(struct rzg3s_pcie_host *host)
{
	u32 val; /* [한국어] 링크 상태 폴링에 쓸 임시 값 */
	int ret; /* [한국어] 각 단계의 결과 */

	/* SoC-specific pre-configuration */
	if (host->data->config_pre_init) /* [한국어] RZ/G3E 처럼 설정 전에 리셋 비트를 풀어야 하는 SoC 에서만 */
		host->data->config_pre_init(host); /* [한국어] 그 작업을 한다 */

	/* Initialize the PCIe related registers */
	ret = rzg3s_pcie_config_init(host); /* [한국어] BAR 마스크·클래스 코드·버스 번호를 세운다 */
	if (ret) /* [한국어] 실패했으면 */
		goto config_deinit; /* [한국어] 사전 설정만 되돌린다 */

	ret = rzg3s_pcie_host_init_port(host); /* [한국어] 포트 ID 를 적고 클럭과 PHY 를 켠다 */
	if (ret) /* [한국어] 실패했으면 */
		goto config_deinit; /* [한국어] 그 안에서 클럭은 이미 되돌렸으므로 사전 설정만 되돌린다 */

	/* Enable ASPM L1 transition for SoCs that use it */
	ret = rzg3s_sysc_config_func(host->sysc,
				     RZG3S_SYSC_FUNC_ID_L1_ALLOW, 1); /* [한국어] 그 기능이 있는 SoC 에서만 ASPM L1 전이를 허용한다. 없는 SoC 에서는 아무 일도 하지 않고 성공한다 */
	if (ret) /* [한국어] regmap 접근이 실패했으면 */
		goto config_deinit_and_refclk; /* [한국어] 클럭까지 되돌린다 */

	/* Initialize the interrupts */
	rzg3s_pcie_irq_init(host); /* [한국어] 남아 있던 인터럽트 상태를 지우고 쓰지 않는 인터럽트를 끈다 */

	/* SoC-specific post-configuration */
	ret = host->data->config_post_init(host); /* [한국어] 리셋을 풀어 링크를 세우기 시작한다 */
	if (ret) /* [한국어] 실패했으면 */
		goto config_deinit_and_refclk; /* [한국어] 클럭과 사전 설정을 되돌린다 */

	/* Wait for link up */
	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT1, val,
				 !(val & RZG3S_PCI_PCSTAT1_DL_DOWN_STS),
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI,
				 PCIE_LINK_WAIT_SLEEP_MS * MILLI *
				 PCIE_LINK_WAIT_MAX_RETRIES); /* [한국어] 데이터 링크가 내려가 있지 않을 때까지 90ms 씩 최대 10번 기다린다 */
	if (ret) /* [한국어] 끝내 서지 않았으면 */
		goto config_deinit_post; /* [한국어] 사후 설정까지 되돌린다 */

	val = readl_relaxed(host->axi + RZG3S_PCI_PCSTAT2); /* [한국어] 선 링크의 상태 워드를 읽어 */
	dev_info(host->dev, "PCIe link status [0x%x]\n", val); /* [한국어] 사용자에게 알린다 */

	return 0; /* [한국어] 컨트롤러 초기화 완료 */

config_deinit_post: /* [한국어] 링크 대기 실패가 여기로 온다 */
	host->data->config_deinit(host); /* [한국어] 사후 설정을 되돌린다 */
config_deinit_and_refclk: /* [한국어] L1 허용/사후 설정 실패가 여기로 온다 */
	clk_disable_unprepare(host->port.refclk); /* [한국어] 클럭을 끈다 */
config_deinit: /* [한국어] 앞쪽 실패가 여기로 온다 */
	if (host->data->config_pre_init) /* [한국어] 사전 설정을 한 SoC 에서만 */
		host->data->config_deinit(host); /* [한국어] 그것을 되돌린다. 위에서 흘러 내려온 RZ/G3E 는 여기서 한 번 더 불리지만, 그 구현이 RESET 레지스터에 0 을 통째로 쓰는 것이라 결과가 같다 */
	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_set_inbound_window - 안쪽(AXI) 창 하나를 레지스터에 적는다
 *
 * @host:     컨트롤러 상태.
 * @cpu_addr: 이 창이 가리킬 CPU(시스템 메모리) 주소.
 * @pci_addr: 장치가 보게 될 PCI 주소.
 * @size:     창 크기 마스크. 호출자가 이미 크기-1 로 만들어 넘긴다.
 * @id:       창 번호(0~7).
 *
 * 이름이 헷갈릴 수 있는데, "AW" 계열 레지스터가 안쪽(장치 → 메모리) 창이다.
 * ADEST 가 목적지(CPU 쪽), AWBASE 가 장치가 보는 주소, AWMASK 가 크기다.
 *
 * 순서가 중요하다. 목적지와 크기를 먼저 적고, 마지막에 AWBASEL 을 쓰면서
 * 활성 비트를 함께 세운다. 그러면 창이 완전히 준비된 뒤에야 켜진다.
 *
 * 각 값이 상위/하위 두 워드로 나뉘는 것은 이 컨트롤러가 64비트 주소를
 * 다루기 때문이다. 활성 비트가 하위 워드에 있어, 그 워드를 마지막에 쓰는
 * 것이 자연스럽게 성립한다.
 *
 * size 를 마스크 형태(2^N - 1)로 받는 규약이라, 호출자가 크기에서 1 을 빼
 * 넘긴다. 매뉴얼의 요구사항이며 그 근거를 호출자 쪽 상류 주석이 인용한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(창 설정).
 *
 * 호출 체인:  rzg3s_pcie_set_inbound_windows() → [이 함수]
 */
static void rzg3s_pcie_set_inbound_window(struct rzg3s_pcie_host *host,
					  u64 cpu_addr, u64 pci_addr, u64 size,
					  int id)
{
	/* Set CPU window base address */
	writel_relaxed(upper_32_bits(cpu_addr),
		       host->axi + RZG3S_PCI_ADESTU(id)); /* [한국어] CPU 쪽 목적지 주소의 상위 워드 */
	writel_relaxed(lower_32_bits(cpu_addr),
		       host->axi + RZG3S_PCI_ADESTL(id)); /* [한국어] 하위 워드. 활성 비트가 없는 레지스터라 순서가 자유롭다 */

	/* Set window size */
	writel_relaxed(upper_32_bits(size), host->axi + RZG3S_PCI_AWMASKU(id)); /* [한국어] 창 크기 마스크의 상위 워드 */
	writel_relaxed(lower_32_bits(size), host->axi + RZG3S_PCI_AWMASKL(id)); /* [한국어] 하위 워드 */

	/* Set PCIe window base address and enable the window */
	writel_relaxed(upper_32_bits(pci_addr),
		       host->axi + RZG3S_PCI_AWBASEU(id)); /* [한국어] 장치가 볼 PCI 주소의 상위 워드를 먼저 쓴다 */
	writel_relaxed(lower_32_bits(pci_addr) | RZG3S_PCI_AWBASEL_WIN_ENA,
		       host->axi + RZG3S_PCI_AWBASEL(id)); /* [한국어] 하위 워드에 활성 비트를 함께 써서 마지막에 창을 켠다 — 그래야 완전히 준비된 뒤 열린다 */
}

/* [한국어]
 * rzg3s_pcie_set_inbound_windows - dma-ranges 항목 하나를 창 여럿으로 쪼개 연다
 *
 * @host:  컨트롤러 상태.
 * @entry: dma-ranges 의 한 항목.
 * @index: 지금까지 쓴 창 번호. 이 함수가 쓴 만큼 늘려 돌려준다.
 * @return: 0 성공, -ENOSPC 는 창이 모자란 경우.
 *
 * 이 파일에서 가장 계산이 많은 함수다. 상류 주석이 두 SoC 의 매뉴얼 절을
 * 인용해 하드웨어 제약 셋을 밝힌다.
 *   - 창 하나의 크기는 2의 거듭제곱이어야 한다.
 *   - 마스크 레지스터에는 2^N - 1 을 넣어야 한다.
 *   - base 와 mask 를 더할 때 자리 올림이 생기면 안 된다. 즉 시작 주소가
 *     창 크기에 정렬되어 있어야 한다.
 *
 * DT 의 dma-ranges 는 그 제약을 지킬 이유가 없으므로, 한 항목을 여러 창으로
 * 쪼개 덮는다. 매 반복에서 크기를 두 단계로 정한다.
 *
 *   1) 남은 길이에 들어가는 가장 큰 2의 거듭제곱(__fls).
 *   2) CPU 주소와 PCI 주소 각각의 자연 정렬(__ffs, 가장 낮은 세워진 비트)
 *      중 작은 쪽. 둘 다 그 크기에 정렬되어 있어야 자리 올림이 없다.
 *      주소가 0 이면 __ffs 가 정의되지 않으므로 제한 없음(~0ULL)으로 둔다.
 *
 * 둘 중 작은 값을 택한 뒤, 상류 주석이 인용하는 최소 창 크기 4KB 로 한 번
 * 더 올린다. 그래서 4KB 미만의 자투리가 남으면 그만큼 넘겨 덮게 되는데,
 * dma-ranges 가 페이지 단위로 주어지는 한 그 상황은 오지 않는다.
 *
 * 창 번호가 상한에 닿으면 -ENOSPC 로 그만둔다. 이때 이미 연 창들을 닫지
 * 않지만, 호출자가 오류를 그대로 올려 probe 가 실패하므로 컨트롤러가
 * 쓰이지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:  rzg3s_pcie_parse_map_dma_ranges() → [이 함수]
 *               → rzg3s_pcie_set_inbound_window()
 */
static int rzg3s_pcie_set_inbound_windows(struct rzg3s_pcie_host *host,
					  struct resource_entry *entry,
					  int *index)
{
	u64 pci_addr = entry->res->start - entry->offset; /* [한국어] 장치가 볼 PCI 주소. CPU 주소에서 오프셋을 빼서 얻는다 */
	u64 cpu_addr = entry->res->start; /* [한국어] 이 구간의 CPU 쪽 시작 */
	u64 cpu_end = entry->res->end; /* [한국어] 이 구간의 CPU 쪽 끝(포함) */
	int id = *index; /* [한국어] 쓸 창 번호. 호출자가 누적으로 관리한다 */
	u64 size; /* [한국어] 이번 반복에서 정할 창 크기 */

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section 34.6.6.7) and
	 * RZ/G3E HW manual (Rev.1.15, section 6.6.7.6):
	 * - Each window must be a single memory size of power of two
	 * - Mask registers must be set to (2^N - 1)
	 * - Bit carry must not occur when adding base and mask registers,
	 *   meaning the base address must be aligned to the window size
	 *
	 * Split non-power-of-2 regions into multiple windows to satisfy
	 * these constraints without over-mapping.
	 */
	while (cpu_addr <= cpu_end) { /* [한국어] 구간을 다 덮을 때까지 창을 하나씩 만든다 */
		u64 remaining_size = cpu_end - cpu_addr + 1; /* [한국어] 아직 덮지 못한 길이. 끝이 포함 경계라 1 을 더한다 */
		u64 align_limit; /* [한국어] 정렬이 허용하는 최대 창 크기 */

		if (id >= RZG3S_MAX_WINDOWS) /* [한국어] 쓸 창이 남아 있지 않으면 */
			return dev_err_probe(host->dev, -ENOSPC,
					     "Failed to map inbound window for resource (%s)\n",
					     entry->res->name); /* [한국어] 어느 자원에서 모자랐는지 알리고 -ENOSPC 로 돌아간다 */

		/* Start with largest power-of-two that fits in remaining size */
		size = 1ULL << __fls(remaining_size); /* [한국어] 남은 길이에 들어가는 가장 큰 2의 거듭제곱 */

		/*
		 * The "no bit carry" rule requires base addresses to be
		 * aligned to the window size. Find the maximum window size
		 * that both addresses can support based on their natural
		 * alignment (lowest set bit).
		 */
		align_limit = min(cpu_addr ? (1ULL << __ffs(cpu_addr)) : ~0ULL,
				  pci_addr ? (1ULL << __ffs(pci_addr)) : ~0ULL); /* [한국어] CPU 와 PCI 두 주소의 자연 정렬 중 작은 쪽. 주소가 0 이면 __ffs 가 정의되지 않으므로 제한 없음으로 둔다 */

		size = min(size, align_limit); /* [한국어] 둘 중 작은 값을 택해 자리 올림 제약을 지킨다 */

		/*
		 * Minimum window size is 4KB.
		 * See RZ/G3S HW manual (Rev.1.10, section 34.3.1.71) and
		 * RZ/G3E HW manual (Rev.1.15, section 6.6.4.1.3.(74)).
		 */
		size = max(size, SZ_4K); /* [한국어] 상류 주석이 인용하는 최소 창 크기로 한 번 더 올린다 */

		rzg3s_pcie_set_inbound_window(host, cpu_addr, pci_addr,
					      size - 1, id); /* [한국어] 크기에서 1 을 빼 마스크 형태로 넘긴다 — 하드웨어가 2^N - 1 을 요구한다 */

		pci_addr += size; /* [한국어] PCI 주소를 창 크기만큼 앞으로 */
		cpu_addr += size; /* [한국어] CPU 주소도 같은 만큼 앞으로. 둘이 나란히 움직여 대응 관계가 유지된다 */
		id++; /* [한국어] 다음 창 번호로 */
	}
	*index = id; /* [한국어] 쓴 만큼 늘어난 창 번호를 호출자에게 돌려준다 */

	return 0; /* [한국어] 이 자원을 모두 덮었다 */
}

/* [한국어]
 * rzg3s_pcie_parse_map_dma_ranges - DT 의 dma-ranges 를 모두 안쪽 창으로 연다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 하위 함수가 준 오류.
 *
 * 창 번호를 지역 변수 하나로 들고 다니며 항목마다 넘겨 준다. 한 항목이
 * 여러 창을 쓸 수 있으므로, 번호를 항목 인덱스가 아니라 누적값으로 관리해야
 * 한다 — 그래서 포인터로 주고받는다.
 *
 * 이 함수가 rzg3s_pcie_msi_setup() 보다 먼저 불려야 한다. MSI 창이 이미
 * 열려 있는 AXI 창 안에 들어가야 한다는 하드웨어 제약 때문이며, 실제로
 * rzg3s_pcie_host_setup() 이 창 설정을 IRQ 도메인 초기화보다 앞에 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_setup() → [이 함수]
 *               → rzg3s_pcie_set_inbound_windows()
 */
static int rzg3s_pcie_parse_map_dma_ranges(struct rzg3s_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host); /* [한국어] dma_ranges 목록을 얻기 위해 바깥 브리지를 되찾는다 */
	struct resource_entry *entry; /* [한국어] 순회용 항목 */
	int i = 0, ret; /* [한국어] 누적 창 번호와 하위 호출 결과 */

	resource_list_for_each_entry(entry, &bridge->dma_ranges) { /* [한국어] DT 의 dma-ranges 항목을 하나씩 훑는다 */
		ret = rzg3s_pcie_set_inbound_windows(host, entry, &i); /* [한국어] 한 항목을 창 여럿으로 쪼개 연다. 창 번호를 포인터로 넘겨 누적시킨다 */
		if (ret) /* [한국어] 창이 모자랐으면 */
			return ret; /* [한국어] 그 오류를 그대로 올린다 */
	}

	return 0; /* [한국어] 안쪽 창 설정 완료. 이제 MSI 목적지가 이 창들 안에 들어갈 수 있다 */
}

/* [한국어]
 * rzg3s_pcie_set_outbound_window - 바깥(PCIe) 창 하나를 연다
 *
 * @host: 컨트롤러 상태.
 * @win:  브리지 창 자원 하나.
 * @id:   창 번호(0~7).
 *
 * "PW"/"PDEST" 계열이 바깥(CPU → 장치) 창이다.
 *
 * I/O 자원과 메모리 자원의 시작 주소를 구하는 방법이 다르다. I/O 는 커널이
 * 가상 포트 번호로 관리하므로 pci_pio_to_address 로 실제 주소를 되찾아야
 * 한다.
 *
 * 상류 주석이 인용하듯 창 시작은 4KB 정렬이어야 한다. 크기도 4KB 로 올린
 * 뒤 2의 거듭제곱으로 올리고 1 을 빼 마스크 형태로 만든다.
 *
 * 여기서 목적지(PDEST)와 시작(PWBASE)에 같은 값을 넣는 점을 짚어 둔다.
 * CPU 주소와 PCI 주소가 같은 항등 매핑이 되는데, 그것은 res_start 를
 * 계산할 때 이미 win->offset 을 빼 두었기 때문이다 — 즉 PCI 쪽 주소를
 * 양쪽에 쓰는 셈이다.
 *
 * 안쪽 창과 마찬가지로 활성 비트가 있는 하위 워드를 마지막에 써서, 창이
 * 완전히 준비된 뒤 켜지게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_parse_map_ranges() → [이 함수]
 */
static void rzg3s_pcie_set_outbound_window(struct rzg3s_pcie_host *host,
					   struct resource_entry *win, int id)
{
	struct resource *res = win->res; /* [한국어] 이 항목의 자원 */
	resource_size_t size = resource_size(res); /* [한국어] 그 크기 */
	resource_size_t res_start; /* [한국어] 창에 적을 시작 주소 */

	if (res->flags & IORESOURCE_IO) /* [한국어] I/O 자원이면 */
		res_start = pci_pio_to_address(res->start) - win->offset; /* [한국어] 커널의 가상 포트 번호를 실제 주소로 되돌린 뒤 오프셋을 뺀다 */
	else
		res_start = res->start - win->offset; /* [한국어] 메모리 자원은 오프셋만 빼면 된다 */

	/*
	 * According to the RZ/G3S HW manual (Rev.1.10, section 34.3.1.75 PCIe
	 * Window Base (Lower) Registers) the window base address need to be 4K
	 * aligned.
	 */
	res_start = ALIGN(res_start, SZ_4K); /* [한국어] 상류 주석이 인용하듯 창 시작은 4KB 정렬이어야 한다 */

	size = ALIGN(size, SZ_4K); /* [한국어] 크기도 4KB 로 올린 뒤 */
	size = roundup_pow_of_two(size) - 1; /* [한국어] 2의 거듭제곱으로 올리고 1 을 빼 마스크 형태로 만든다 */

	/* Set PCIe destination */
	writel_relaxed(upper_32_bits(res_start),
		       host->axi + RZG3S_PCI_PDESTU(id)); /* [한국어] 목적지 주소의 상위 워드 */
	writel_relaxed(lower_32_bits(res_start),
		       host->axi + RZG3S_PCI_PDESTL(id)); /* [한국어] 하위 워드. 앞서 오프셋을 뺐으므로 여기 들어가는 값이 곧 PCI 쪽 주소다 */

	/* Set PCIe window mask */
	writel_relaxed(upper_32_bits(size), host->axi + RZG3S_PCI_PWMASKU(id)); /* [한국어] 창 크기 마스크의 상위 워드 */
	writel_relaxed(lower_32_bits(size), host->axi + RZG3S_PCI_PWMASKL(id)); /* [한국어] 하위 워드 */

	/* Set PCIe window base and enable the window */
	writel_relaxed(upper_32_bits(res_start),
		       host->axi + RZG3S_PCI_PWBASEU(id)); /* [한국어] 창 시작 주소의 상위 워드를 먼저 쓴다 */
	writel_relaxed(lower_32_bits(res_start) | RZG3S_PCI_PWBASEL_ENA,
		       host->axi + RZG3S_PCI_PWBASEL(id)); /* [한국어] 하위 워드에 활성 비트를 함께 써서 마지막에 창을 켠다 */
}

/* [한국어]
 * rzg3s_pcie_parse_map_ranges - DT 의 ranges 를 모두 바깥 창으로 연다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, -ENOSPC 는 창이 모자란 경우.
 *
 * 브리지의 창 자원을 훑어 I/O 와 메모리만 창으로 연다. 버스 번호 자원처럼
 * 주소 창이 아닌 항목은 switch 에서 걸러진다.
 *
 * 창 번호는 실제로 연 것만 센다. 그래서 걸러진 자원이 있어도 번호에
 * 구멍이 생기지 않는다.
 *
 * [관찰] 창 수 확인이 flags 확인보다 앞에 있어, flags 가 0 인 항목이
 * 상한 근처에 있으면 실제로 창을 쓰지 않는데도 -ENOSPC 가 날 수 있다.
 * 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * 안쪽 창과 달리 자원 하나를 쪼개지 않는다. 바깥 창은 크기를 2의 거듭제곱으로
 * 올려 덮으면 되고, 넘겨 덮은 부분은 어차피 다른 창이 쓰지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rzg3s_pcie_host_setup() → [이 함수]
 *               → rzg3s_pcie_set_outbound_window()
 */
static int rzg3s_pcie_parse_map_ranges(struct rzg3s_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host); /* [한국어] 브리지 창 목록을 얻기 위해 바깥 브리지를 되찾는다 */
	struct resource_entry *win; /* [한국어] 순회용 항목 */
	int i = 0; /* [한국어] 실제로 연 창의 개수 */

	resource_list_for_each_entry(win, &bridge->windows) { /* [한국어] DT 의 ranges 가 만든 창 자원을 하나씩 훑는다 */
		struct resource *res = win->res; /* [한국어] 이 항목의 자원 */

		if (i >= RZG3S_MAX_WINDOWS) /* [한국어] 쓸 창이 남아 있지 않으면. [관찰] 이 확인이 아래 flags 확인보다 앞에 있어, 주소 창이 아닌 항목이 상한 근처에 있으면 실제로 창을 쓰지 않는데도 오류가 날 수 있다 */
			return dev_err_probe(host->dev, -ENOSPC,
					     "Failed to map outbound window for resource (%s)\n",
					     res->name); /* [한국어] 어느 자원에서 모자랐는지 알리고 -ENOSPC 로 돌아간다 */

		if (!res->flags) /* [한국어] 플래그가 없는 빈 항목이면 */
			continue; /* [한국어] 열 창이 없다 */

		switch (resource_type(res)) { /* [한국어] 자원 종류로 갈린다 */
		case IORESOURCE_IO: /* [한국어] I/O 창 */
		case IORESOURCE_MEM: /* [한국어] 메모리 창. 이 둘만 바깥 창으로 연다 */
			rzg3s_pcie_set_outbound_window(host, win, i); /* [한국어] 창 하나를 연다 */
			i++; /* [한국어] 실제로 연 것만 세므로 창 번호에 구멍이 생기지 않는다 */
			break; /* [한국어] 다음 자원으로 */
		}
	}

	return 0; /* [한국어] 바깥 창 설정 완료 */
}

/* [한국어]
 * rzg3s_soc_pcie_init_phy - RZ/G3S 의 PHY 레지스터를 표대로 채운다
 *
 * @host: 컨트롤러 상태.
 * @return: 늘 0.
 *
 * 이 SoC 는 PHY 설정을 소프트웨어가 통째로 써 넣어야 한다. 값들은 세 개의
 * 정적 표에 들어 있으며, 각 값의 의미는 르네사스의 PHY 문서에 있을 것으로
 * 보이나 이 트리에서 확인 못 함.
 *
 * xcfgd 표의 초기화가 눈에 띈다. 지정 초기화([8], [17], [19])와 이어지는
 * 값들을 섞어 써, 값이 있는 구간만 적고 나머지는 0 으로 둔다. 39개 자리 중
 * 실제로 의미 있는 것이 일부뿐이라는 뜻이다. 0 인 자리도 루프가 그대로
 * 써 넣으므로, 그 자리에 0 을 쓰는 것이 의도된 동작이다.
 *
 * 절차는 문을 열고 → 네 묶음을 쓰고 → "이제 이 값을 쓰라" 는 선택 비트를
 * 세우고 → 문을 닫는 순이다. 선택 비트를 값보다 먼저 세우면 반쯤 채워진
 * 설정이 적용되므로 순서가 중요하다.
 *
 * RZ/G3E 표에는 이 자리가 비어 있어 호출자가 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(host_init).
 *
 * 호출 체인:  rzg3s_pcie_host_init_port() → data->init_phy → [이 함수]
 */
static int rzg3s_soc_pcie_init_phy(struct rzg3s_pcie_host *host)
{
		/* [한국어] PHY 디지털 설정 값 표.
		 * 지정 초기화([8], [17], [19])와 이어지는 값들을 섞어 써, 의미 있는
		 * 구간만 적고 나머지 자리는 0 으로 둔다. 아래 루프가 39개 자리를 모두
		 * 쓰므로 0 인 자리에도 0 이 실제로 적힌다. 각 값의 의미는 르네사스의
		 * PHY 문서에 있을 것으로 보이나 이 트리에서 확인 못 함. */
	static const u32 xcfgd_settings[RZG3S_PCI_PHY_XCFGD_NUM] = {
		[8]  = 0xe0006801, 0x007f7e30, 0x183e0000, 0x978ff500,
		       0xec000000, 0x009f1400, 0x0000d009,
		[17] = 0x78000000,
		[19] = 0x00880000, 0x000005c0, 0x07000000, 0x00780920,
		       0xc9400ce2, 0x90000c0c, 0x000c1414, 0x00005034,
		       0x00006000, 0x00000001,
	}; /* [한국어] PHY 디지털 설정 값들. 지정 초기화로 값이 있는 구간만 적고 나머지는 0 이며, 각 값의 의미는 이 트리에서 확인 못 함 */
	static const u32 xcfga_cmn_settings[RZG3S_PCI_PHY_XCFGA_CMN_NUM] = {
		0x00000d10, 0x08310100, 0x00c21404, 0x013c0010, 0x01874440,
		0x1a216082, 0x00103440, 0x00000080, 0x00000010, 0x0c1000c1,
		0x1000c100, 0x0222000c, 0x00640019, 0x00a00028, 0x01d11228,
		0x0201001d,
	}; /* [한국어] PHY 아날로그 공통 설정 값들. 16개 자리를 모두 채운다 */
	static const u32 xcfga_rx_settings[RZG3S_PCI_PHY_XCFGA_RX_NUM] = {
		0x07d55000, 0x030e3f00, 0x00000288, 0x102c5880, 0x0000000b,
		0x04141441, 0x00641641, 0x00d63d63, 0x00641641, 0x01970377,
		0x00190287, 0x00190028, 0x00000028,
	}; /* [한국어] PHY 아날로그 수신 설정 값들. 13개 자리를 모두 채운다 */
	unsigned int i; /* [한국어] 세 루프가 공유하는 인덱스 */

	/*
	 * Enable access permission for physical layer control and status
	 * registers.
	 */
	writel_relaxed(RZG3S_PCI_PERM_PIPE_PHY_REG_EN,
		       host->axi + RZG3S_PCI_PERM); /* [한국어] 상류 주석대로 PHY 제어/상태 레지스터에 쓸 수 있게 문을 연다 */

	for (i = 0; i < RZG3S_PCI_PHY_XCFGD_NUM; i++) { /* [한국어] 디지털 설정 자리를 처음부터 끝까지 */
		writel_relaxed(xcfgd_settings[i],
			       host->axi + RZG3S_PCI_PHY_XCFGD(i)); /* [한국어] 표의 값을 그대로 써 넣는다. 표에서 0 인 자리도 0 을 쓰는 것이 의도된 동작이다 */
	}

	for (i = 0; i < RZG3S_PCI_PHY_XCFGA_CMN_NUM; i++) { /* [한국어] 아날로그 공통 설정 자리를 */
		writel_relaxed(xcfga_cmn_settings[i],
			       host->axi + RZG3S_PCI_PHY_XCFGA_CMN(i)); /* [한국어] 차례로 채운다 */
	}

	for (i = 0; i < RZG3S_PCI_PHY_XCFGA_RX_NUM; i++) { /* [한국어] 아날로그 수신 설정 자리를 */
		writel_relaxed(xcfga_rx_settings[i],
			       host->axi + RZG3S_PCI_PHY_XCFGA_RX(i)); /* [한국어] 차례로 채운다 */
	}

	writel_relaxed(0x107, host->axi + RZG3S_PCI_PHY_XCFGA_TX); /* [한국어] 송신 설정은 레지스터 하나뿐이라 값을 직접 쓴다. 그 값의 의미는 이 트리에서 확인 못 함 */

	/* Select PHY settings values */
	writel_relaxed(RZG3S_PCI_PHY_XCFG_CTRL_PHYREG_SEL,
		       host->axi + RZG3S_PCI_PHY_XCFG_CTRL); /* [한국어] 써 넣은 값을 실제로 쓰라고 지시한다. 값을 모두 적은 뒤여야 반쯤 채워진 설정이 적용되지 않는다 */

	/*
	 * Disable access permission for physical layer control and status
	 * registers.
	 */
	writel_relaxed(0, host->axi + RZG3S_PCI_PERM); /* [한국어] 상류 주석대로 문을 다시 닫는다 */

	return 0; /* [한국어] 실패할 일이 없다. int 인 것은 cfg->init_phy 의 형에 맞추기 위한 것으로 보인다 */
}

/* [한국어]
 * rzg3s_pcie_host_setup - 창 설정부터 링크 기동까지를 probe/resume 이 공유한다
 *
 * @host:              컨트롤러 상태.
 * @init_irqdomain:    인터럽트 쪽을 세우는 콜백.
 * @teardown_irqdomain: 그 되돌리기 콜백.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 콜백 두 개를 인자로 받는 것이 이 파일의 구조적 요점이다.
 *
 *   probe 는 rzg3s_pcie_init_irqdomain / rzg3s_pcie_teardown_irqdomain 을
 *   넘긴다 — 도메인부터 새로 만들어야 하기 때문이다.
 *
 *   resume 은 rzg3s_pcie_msi_hw_setup / rzg3s_pcie_msi_hw_teardown 을
 *   넘긴다 — 서스펜드로 레지스터만 날아가고 도메인·비트맵·목적지 페이지는
 *   메모리에 그대로 남아 있으므로, MSI 하드웨어만 다시 세우면 된다.
 *
 * 덕분에 창 설정과 링크 기동, 속도 조정이라는 공통 절차를 두 경로가
 * 그대로 나눠 쓴다.
 *
 * 순서에 의존 관계가 있다. 안쪽 창이 먼저 열려야 MSI 목적지가 그 안에
 * 들어갈 수 있고(하드웨어 제약), 인터럽트가 준비된 뒤에 링크를 세워야
 * 링크 이벤트를 놓치지 않는다.
 *
 * 속도 조정 실패는 치명적이지 않게 다룬다. 링크는 이미 서 있으므로
 * 정보 로그만 남기고 계속 간다.
 *
 * 마지막의 msleep 은 ../pci.h 의 PCIE_RESET_CONFIG_WAIT_MS 로, 리셋 뒤
 * 첫 config 요청을 보내기 전에 지켜야 하는 대기 시간이다. 이 대기 뒤에
 * 호출자가 pci_host_probe() 로 열거를 시작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:  rzg3s_pcie_probe() / rzg3s_pcie_resume_noirq() → [이 함수]
 *               → rzg3s_pcie_parse_map_dma_ranges() → rzg3s_pcie_parse_map_ranges()
 *               → init_irqdomain() → rzg3s_pcie_host_init()
 *               → rzg3s_pcie_set_max_link_speed()
 */
static int
rzg3s_pcie_host_setup(struct rzg3s_pcie_host *host,
		      int (*init_irqdomain)(struct rzg3s_pcie_host *host),
		      void (*teardown_irqdomain)(struct rzg3s_pcie_host *host))
{
	struct device *dev = host->dev; /* [한국어] 로그의 기준 */
	int ret; /* [한국어] 각 단계의 결과 */

	/* Set inbound windows */
	ret = rzg3s_pcie_parse_map_dma_ranges(host); /* [한국어] 장치가 DRAM 을 보는 창을 연다. MSI 목적지가 이 창 안에 들어야 하므로 가장 먼저다 */
	if (ret) /* [한국어] 실패했으면 */
		return dev_err_probe(dev, ret,
				     "Failed to set inbound windows!\n"); /* [한국어] 창을 못 열면 DMA 가 불가능하다 */

	/* Set outbound windows */
	ret = rzg3s_pcie_parse_map_ranges(host); /* [한국어] CPU 가 장치를 보는 창을 연다 */
	if (ret) /* [한국어] 실패했으면 */
		return dev_err_probe(dev, ret,
				     "Failed to set outbound windows!\n"); /* [한국어] 창을 못 열면 장치에 접근할 수 없다 */

	ret = init_irqdomain(host); /* [한국어] 인터럽트 쪽을 세운다. probe 와 resume 이 서로 다른 함수를 넘긴다 */
	if (ret) /* [한국어] 실패했으면 */
		return dev_err_probe(dev, ret, "Failed to init IRQ domain\n"); /* [한국어] 창은 열려 있지만 되돌리지 않는데, 이 지점의 상류 코드가 그렇다 */

	ret = rzg3s_pcie_host_init(host); /* [한국어] 컨트롤러를 초기화하고 링크가 설 때까지 기다린다 */
	if (ret) { /* [한국어] 실패했으면 */
		dev_err_probe(dev, ret, "Failed to initialize the HW!\n"); /* [한국어] 알리고 */
		goto teardown_irqdomain; /* [한국어] 인터럽트 쪽을 되돌린다 */
	}

	ret = rzg3s_pcie_set_max_link_speed(host); /* [한국어] 링크가 선 뒤 속도를 끌어올린다 */
	if (ret) /* [한국어] 실패해도 */
		dev_info(dev, "Failed to set max link speed\n"); /* [한국어] 링크는 살아 있으므로 정보 로그만 남기고 계속 간다 */

	msleep(PCIE_RESET_CONFIG_WAIT_MS); /* [한국어] 리셋 뒤 첫 config 요청까지 지켜야 하는 대기 시간. 이 뒤에 호출자가 열거를 시작한다 */

	return 0; /* [한국어] 공통 절차 완료 */

teardown_irqdomain: /* [한국어] HW 초기화 실패가 여기로 온다 */
	teardown_irqdomain(host); /* [한국어] 넘겨받은 되돌리기 콜백을 부른다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_probe - 플랫폼 디바이스를 받아 PCIe 호스트를 세운다
 *
 * @pdev: DT 가 만든 플랫폼 디바이스.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일의 입구다. [자원 확보] → [SYSC·리셋·전원] → [설정과 열거] 세
 * 토막으로 읽으면 된다.
 *
 * [자원 확보]
 *   브리지를 잡을 때 sizeof(*host) 를 함께 달라고 해서, rzg3s_pcie_host 를
 *   브리지의 private 영역에 얹는다. 그래서 어디서든
 *   pci_host_bridge_from_priv() 로 브리지를, bus->sysdata 로 host 를
 *   되찾을 수 있다.
 *
 *   SoC 표를 device_get_match_data 로 가져온다. 이 한 줄이 아래 모든 SoC
 *   분기의 출발점이다.
 *
 *   레지스터 창은 하나만 매핑하고, RC 의 config 공간은 그 안의 고정
 *   오프셋으로 얻는다.
 *
 *   DT 의 max-link-speed 를 host->max_link_speed 에 저장해 두면
 *   rzg3s_pcie_set_max_link_speed() 가 그것을 상한으로 쓴다.
 *
 * [SYSC·리셋·전원]
 *   SYSC 는 컨트롤러 바깥의 SoC 전역 블록이라 phandle 로 찾아 regmap 으로
 *   연다. 그것을 통해 컨트롤러를 RC 모드로 두고 RST_RSM_B 를 해제한다.
 *
 *   리셋을 이름으로 잡고, 규정 지연 뒤에 전원 리셋을 푼다.
 *
 *   클럭은 직접 켜지 않고 런타임 PM 으로 켠다. 상류 주석이 그 이유를
 *   밝힌다 — 컨트롤러 클럭이 클럭 전원 도메인에 속해 있기 때문이다.
 *
 *   하드웨어 락은 인터럽트가 걸리기 직전에 초기화한다.
 *
 * [설정과 열거]
 *   공통 절차(창 → IRQ → HW → 속도)를 rzg3s_pcie_host_setup() 에 맡기고,
 *   그다음 pci_ops 두 개를 브리지에 꽂아 열거를 넘긴다. ops 가 RC 자신용,
 *   child_ops 가 그 아래 버스용이라는 점이 이 컨트롤러의 이원 구조 그대로다.
 *
 * 되돌리기가 라벨 다섯의 폭포 구조다. 그중 sysc_signal_restore 위의 상류
 * 주석이 순서 제약을 밝힌다 — RST_RSM_B 는 PHY 전원을 끄기 전에 걸어야 한다.
 *
 * port_refclk_put 이 마지막인 것은 그 클럭만 devm 이 아닌
 * of_clk_get_by_name 으로 잡았기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → rzg3s_pcie_host_parse_port()
 *               → rzg3s_pcie_resets_prepare_and_get()
 *               → rzg3s_pcie_host_setup() → pci_host_probe()
 */
static int rzg3s_pcie_probe(struct platform_device *pdev)
{
	struct pci_host_bridge *bridge; /* [한국어] PCI 코어에 넘길 호스트 브리지 */
	struct device *dev = &pdev->dev; /* [한국어] 로그와 devm 할당의 기준 */
	struct device_node *np = dev->of_node; /* [한국어] 이 컨트롤러의 DT 노드 */
	struct device_node *sysc_np __free(device_node) =
		of_parse_phandle(np, "renesas,sysc", 0); /* [한국어] SYSC 노드를 phandle 로 찾는다. __free 로 받아 함수를 벗어날 때 참조가 자동으로 놓인다 */
	struct rzg3s_pcie_host *host; /* [한국어] 브리지 private 영역에 얹힐 이 드라이버의 상태 */
	struct rzg3s_sysc *sysc; /* [한국어] SYSC 서술자 */
	int ret; /* [한국어] 각 단계의 결과 */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*host)); /* [한국어] host 를 담을 자리까지 함께 달라고 해서 한 번에 잡는다 — 그래서 브리지와 host 를 서로 되찾을 수 있다 */
	if (!bridge) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 아직 잡은 것이 없다 */

	host = pci_host_bridge_priv(bridge); /* [한국어] 브리지의 private 영역이 곧 이 드라이버의 상태다 */
	host->dev = dev; /* [한국어] 로그와 devm 의 기준을 기억해 둔다 */
	host->data = device_get_match_data(dev); /* [한국어] compatible 에 대응하는 SoC 표를 꺼낸다. 이 한 줄이 모든 SoC 분기의 출발점이다 */
	platform_set_drvdata(pdev, host); /* [한국어] PM 콜백이 dev_get_drvdata 로 되찾을 값. 아래 실패 경로가 remove 와 같은 정리를 하려면 이 시점에 있어야 한다 */

	host->sysc = devm_kzalloc(dev, sizeof(*host->sysc), GFP_KERNEL); /* [한국어] SYSC 서술자를 잡는다 */
	if (!host->sysc) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	sysc = host->sysc; /* [한국어] 아래에서 여러 번 쓰므로 지역 변수로 꺼내 둔다 */
	sysc->info = &host->data->sysc_info; /* [한국어] SoC 표 안에 값으로 들어 있는 좌표표의 주소를 넣는다 */

	host->axi = devm_platform_ioremap_resource(pdev, 0); /* [한국어] DT 의 첫 번째 레지스터 자원(AXI 창)을 매핑한다 */
	if (IS_ERR(host->axi)) /* [한국어] 매핑에 실패했으면 */
		return PTR_ERR(host->axi); /* [한국어] 그 오류를 그대로 올린다 */
	host->pcie = host->axi + RZG3S_PCI_CFG_BASE; /* [한국어] RC 의 config 공간은 별도 자원이 아니라 같은 창 안의 고정 오프셋이다 */

	host->max_link_speed = of_pci_get_max_link_speed(np); /* [한국어] DT 의 max-link-speed 를 기억해 둔다. 속도 조정이 이것을 상한으로 쓴다 */

	ret = rzg3s_pcie_host_parse_port(host); /* [한국어] DT 자식 노드에서 벤더/장치 ID 와 기준 클럭을 얻는다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 클럭을 잡기 전이거나 이미 놓은 상태라 되돌릴 것이 없다 */

	sysc->regmap = syscon_node_to_regmap(sysc_np); /* [한국어] SYSC 노드를 regmap 으로 연다 */
	if (IS_ERR(sysc->regmap)) { /* [한국어] 열지 못했으면 */
		ret = PTR_ERR(sysc->regmap); /* [한국어] 그 오류를 꺼내 */
		goto port_refclk_put; /* [한국어] 앞서 잡은 기준 클럭을 놓고 돌아간다 */
	}

	/* Put controller in RC mode */
	ret = rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_MODE, 1); /* [한국어] 컨트롤러를 RC 로 둔다. 그 기능이 없는 SoC 에서는 아무 일도 하지 않는다 */
	if (ret) /* [한국어] regmap 접근이 실패했으면 */
		goto port_refclk_put; /* [한국어] 클럭을 놓고 돌아간다 */

	ret = rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_RST_RSM_B, 1); /* [한국어] PHY 전원과 얽힌 리셋 신호를 해제한다 */
	if (ret) /* [한국어] 실패했으면 */
		goto port_refclk_put; /* [한국어] 클럭을 놓고 돌아간다 */

	ret = rzg3s_pcie_resets_prepare_and_get(host); /* [한국어] SoC 표에 적힌 리셋들을 이름으로 잡는다 */
	if (ret) /* [한국어] 실패했으면 */
		goto sysc_signal_restore; /* [한국어] 방금 해제한 SYSC 신호부터 되돌린다 */

	ret = rzg3s_pcie_power_resets_deassert(host); /* [한국어] 규정 지연 뒤 전원 리셋을 푼다 */
	if (ret) /* [한국어] 실패했으면 */
		goto sysc_signal_restore; /* [한국어] SYSC 신호를 되돌린다 */

	pm_runtime_enable(dev); /* [한국어] 런타임 PM 을 켠다. 아직 참조를 잡지는 않는다 */

	/*
	 * Controller clocks are part of a clock power domain. Enable them
	 * through runtime PM.
	 */
	ret = pm_runtime_resume_and_get(dev); /* [한국어] 상류 주석대로 컨트롤러 클럭이 클럭 전원 도메인에 속해 있어 런타임 PM 으로 켠다 */
	if (ret) /* [한국어] 깨우지 못했으면 */
		goto rpm_disable; /* [한국어] 런타임 PM 을 다시 끄고 아래로 흘러 되돌린다 */

	raw_spin_lock_init(&host->hw_lock); /* [한국어] 인터럽트 콜백이 이 락을 잡으므로, 인터럽트를 걸기 직전에 초기화한다 */

	ret = rzg3s_pcie_host_setup(host, rzg3s_pcie_init_irqdomain,
				    rzg3s_pcie_teardown_irqdomain); /* [한국어] 창 → IRQ → HW → 속도의 공통 절차. probe 는 도메인을 새로 만드는 콜백 쌍을 넘긴다 */
	if (ret) /* [한국어] 실패했으면 */
		goto rpm_put; /* [한국어] 런타임 PM 참조부터 되돌린다 */

	bridge->sysdata = host; /* [한국어] pci_ops 콜백들이 bus->sysdata 로 되찾을 값 */
	bridge->ops = &rzg3s_pcie_root_ops; /* [한국어] RC 자신용 ops */
	bridge->child_ops = &rzg3s_pcie_child_ops; /* [한국어] 그 아래 버스용 ops. 이 이원 구조가 이 컨트롤러의 특징이다 */
	ret = pci_host_probe(bridge); /* [한국어] PCI 코어에 열거를 넘긴다 */
	if (ret) /* [한국어] 열거가 실패했으면 */
		goto host_probe_teardown; /* [한국어] 인터럽트와 설정까지 되돌린다 */

	return 0; /* [한국어] 컨트롤러가 완전히 준비되었다 */

host_probe_teardown: /* [한국어] 열거 실패가 여기로 온다 */
	rzg3s_pcie_teardown_irqdomain(host); /* [한국어] 인터럽트 쪽을 되돌린다 */
	host->data->config_deinit(host); /* [한국어] 설정 리셋을 다시 건다 */
rpm_put: /* [한국어] host_setup 실패가 여기로 온다 */
	pm_runtime_put_sync(dev); /* [한국어] 런타임 PM 참조를 놓는다 */
rpm_disable: /* [한국어] PM 깨우기 실패가 여기로 온다 */
	pm_runtime_disable(dev); /* [한국어] 런타임 PM 을 끈다 */
	reset_control_bulk_assert(host->data->num_power_resets,
				  host->power_resets); /* [한국어] 전원 리셋을 다시 건다 */
sysc_signal_restore: /* [한국어] 리셋 조회/해제 실패가 여기로 온다 */
	/*
	 * SYSC RST_RSM_B signal need to be asserted before turning off the
	 * power to the PHY.
	 */
	rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_RST_RSM_B, 0); /* [한국어] 상류 주석대로 PHY 전원을 끄기 전에 이 신호를 걸어야 한다 */
port_refclk_put: /* [한국어] 그 앞 단계들의 실패가 여기로 온다 */
	clk_put(host->port.refclk); /* [한국어] devm 이 아닌 방식으로 잡은 유일한 자원이라 직접 놓는다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어]
 * rzg3s_pcie_suspend_noirq - 서스펜드 직전 컨트롤러를 끈다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 되돌릴 수 없는 실패는 그 오류를 올려 서스펜드를 막는다.
 *
 * noirq 단계라 인터럽트가 이미 꺼진 뒤에 불린다. 그래서 MSI 나 INTx 가
 * 들어오는 도중에 컨트롤러를 끄는 일이 없다.
 *
 *   1) 런타임 PM 참조를 놓아 클럭 전원 도메인을 재운다.
 *   2) 기준 클럭을 끈다.
 *   3) SoC 전용 정리(리셋 assert 또는 RESET 레지스터 0).
 *   4) 전원 리셋을 건다.
 *   5) SYSC 의 RST_RSM_B 를 건다. probe 의 되돌리기와 같은 이유로 마지막이다.
 *
 * 되돌리기가 이 파일에서 가장 촘촘하다 — 상류 주석대로 어느 단계에서든
 * 실패하면 그 이전 상태로 되돌려 시스템이 계속 동작하게 한다. 특히
 * config_reinit 라벨이 config_pre_init 과 config_post_init 을 순서대로
 * 다시 불러 3) 을 정확히 되짚는다.
 *
 * MSI 도메인이나 목적지 페이지는 건드리지 않는다. 그것들은 메모리에 남아
 * 있고, resume 이 하드웨어만 다시 세우면 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 비활성(noirq) 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → data->config_deinit()
 *               → rzg3s_sysc_config_func()
 */
static int rzg3s_pcie_suspend_noirq(struct device *dev)
{
	struct rzg3s_pcie_host *host = dev_get_drvdata(dev); /* [한국어] probe 가 걸어 둔 컨트롤러 상태를 되찾는다 */
	const struct rzg3s_pcie_soc_data *data = host->data; /* [한국어] SoC 표. 리셋 개수와 콜백을 꺼내 쓴다 */
	struct rzg3s_pcie_port *port = &host->port; /* [한국어] 기준 클럭이 담긴 포트 정보 */
	struct rzg3s_sysc *sysc = host->sysc; /* [한국어] SYSC 서술자 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = pm_runtime_put_sync(dev); /* [한국어] 런타임 PM 참조를 놓아 클럭 전원 도메인을 재운다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아직 아무것도 되돌릴 것이 없다 */

	clk_disable_unprepare(port->refclk); /* [한국어] 기준 클럭을 끈다 */

	/* SoC-specific de-initialization */
	ret = data->config_deinit(host); /* [한국어] SoC 전용 정리 — 리셋을 걸거나 RESET 레지스터를 0 으로 만든다 */
	if (ret) /* [한국어] 실패했으면 */
		goto refclk_restore; /* [한국어] 클럭부터 되살린다 */

	ret = reset_control_bulk_assert(data->num_power_resets,
					host->power_resets); /* [한국어] 전원 리셋을 건다 */
	if (ret) /* [한국어] 실패했으면 */
		goto config_reinit; /* [한국어] 설정과 클럭을 되살린다 */

	ret = rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_RST_RSM_B, 0); /* [한국어] 마지막으로 SYSC 신호를 건다 — PHY 전원을 끄기 전에 필요하다 */
	if (ret) /* [한국어] 실패했으면 */
		goto power_resets_restore; /* [한국어] 전원 리셋부터 되살린다 */

	return 0; /* [한국어] 서스펜드 준비 완료 */

	/* Restore the previous state if any error happens */
power_resets_restore: /* [한국어] SYSC 신호 실패가 여기로 온다 */
	reset_control_bulk_deassert(data->num_power_resets,
				    host->power_resets); /* [한국어] 전원 리셋을 다시 푼다 */
config_reinit: /* [한국어] 전원 리셋 실패가 여기로 온다 */
	if (data->config_pre_init) /* [한국어] 사전 설정이 있는 SoC 에서만 */
		data->config_pre_init(host); /* [한국어] 그것을 먼저 되돌린다 */
	data->config_post_init(host); /* [한국어] 그다음 사후 설정을 다시 부른다. 두 단계 순서가 원래 초기화와 같다 */
refclk_restore: /* [한국어] SoC 정리 실패가 여기로 온다 */
	clk_prepare_enable(port->refclk); /* [한국어] 클럭을 다시 켠다 */
	pm_runtime_resume_and_get(dev); /* [한국어] 런타임 PM 참조도 다시 잡는다. 이로써 서스펜드 전 상태로 완전히 돌아간다 */
	return ret; /* [한국어] 원래 실패 원인을 올려 서스펜드를 막는다 */
}

/* [한국어]
 * rzg3s_pcie_resume_noirq - 서스펜드에서 깨어나 컨트롤러를 되살린다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 실패하면 되돌린 뒤 그 오류.
 *
 * suspend 의 역순이다.
 *
 *   1) SYSC 로 RC 모드와 RST_RSM_B 를 되돌린다.
 *   2) 규정 지연 뒤 전원 리셋을 푼다.
 *   3) 런타임 PM 으로 클럭 전원 도메인을 깨운다.
 *   4) 공통 절차를 다시 돈다. 이때 넘기는 콜백이 probe 와 다른 것이 핵심
 *      이다 — 도메인과 비트맵, 목적지 페이지는 메모리에 그대로 남아 있으므로
 *      MSI 하드웨어만 다시 세우는 함수 쌍을 넘긴다.
 *
 * 되돌리기 라벨 위의 상류 주석이 그 성격을 밝힌다 — 여기서 실패하면 IP 를
 * 되살릴 방법이 없으므로, 가능한 가장 낮은 전력 상태로 두는 것이 목적이다.
 * 그래서 되돌리기 결과를 확인하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, noirq 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → rzg3s_pcie_power_resets_deassert()
 *               → rzg3s_pcie_host_setup()
 */
static int rzg3s_pcie_resume_noirq(struct device *dev)
{
	struct rzg3s_pcie_host *host = dev_get_drvdata(dev); /* [한국어] probe 가 걸어 둔 컨트롤러 상태를 되찾는다 */
	const struct rzg3s_pcie_soc_data *data = host->data; /* [한국어] SoC 표. 되돌리기에서 리셋 개수를 쓴다 */
	struct rzg3s_sysc *sysc = host->sysc; /* [한국어] SYSC 서술자 */
	int ret; /* [한국어] 각 단계의 결과 */

	ret = rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_MODE, 1); /* [한국어] 컨트롤러를 다시 RC 모드로 둔다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아직 아무것도 잡지 않았다 */

	ret = rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_RST_RSM_B, 1); /* [한국어] PHY 전원과 얽힌 리셋 신호를 해제한다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 되돌릴 것이 없다 — MODE 는 되돌리지 않는데, 이 지점의 상류 코드가 그렇다 */

	ret = rzg3s_pcie_power_resets_deassert(host); /* [한국어] 규정 지연 뒤 전원 리셋을 푼다 */
	if (ret) /* [한국어] 실패했으면 */
		goto assert_rst_rsm_b; /* [한국어] SYSC 신호를 다시 건다 */

	ret = pm_runtime_resume_and_get(dev); /* [한국어] 클럭 전원 도메인을 깨운다 */
	if (ret) /* [한국어] 실패했으면 */
		goto assert_power_resets; /* [한국어] 전원 리셋부터 되돌린다 */

	ret = rzg3s_pcie_host_setup(host, rzg3s_pcie_msi_hw_setup,
				    rzg3s_pcie_msi_hw_teardown); /* [한국어] 공통 절차를 다시 돈다. probe 와 달리 MSI 하드웨어만 다시 세우는 콜백 쌍을 넘긴다 — 도메인과 비트맵은 메모리에 남아 있다 */
	if (ret) /* [한국어] 실패했으면 */
		goto rpm_put; /* [한국어] 런타임 PM 참조부터 되돌린다 */

	return 0; /* [한국어] 복귀 완료 */

	/*
	 * If any error happens there is no way to recover the IP. Put it in the
	 * lowest possible power state.
	 */
rpm_put: /* [한국어] host_setup 실패가 여기로 온다 */
	pm_runtime_put_sync(dev); /* [한국어] 런타임 PM 참조를 놓는다 */
assert_power_resets: /* [한국어] PM 깨우기 실패가 여기로 온다 */
	reset_control_bulk_assert(data->num_power_resets,
				  host->power_resets); /* [한국어] 전원 리셋을 다시 건다 */
assert_rst_rsm_b: /* [한국어] 전원 리셋 해제 실패가 여기로 온다 */
	rzg3s_sysc_config_func(sysc, RZG3S_SYSC_FUNC_ID_RST_RSM_B, 0); /* [한국어] SYSC 신호를 다시 건다. 되돌리기 결과를 확인하지 않는 것은 상류 주석대로 복구가 불가능하기 때문이다 */
	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* [한국어] 이 드라이버의 절전 콜백 표.
 * NOIRQ_SYSTEM_SLEEP_PM_OPS 는 두 함수를 시스템 절전의 noirq 단계에 다는
 * 매크로다. 인터럽트가 꺼진 뒤에 불려야 MSI 나 INTx 가 들어오는 도중에
 * 컨트롤러를 끄는 일이 없다. */
static const struct dev_pm_ops rzg3s_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(rzg3s_pcie_suspend_noirq,
				  rzg3s_pcie_resume_noirq) /* [한국어] suspend 와 resume 을 noirq 단계에 건다 */
};

/* [한국어] RZ/G3S 에서 전원 인가 후 풀어야 할 리셋들의 DT 이름.
 * 셋인 것이 RZ/G3E(하나)와의 차이다 — 이 SoC 는 설정 관련 리셋 둘까지
 * 리셋 프레임워크로 다루기 때문이다. */
static const char * const rzg3s_soc_power_resets[] = {
	"aresetn", "rst_cfg_b", "rst_load_b", /* [한국어] AXI 버스 리셋과, RZ/G3E 가 RESET 레지스터 비트로 다루는 CFG_B/LOAD_B 에 해당하는 리셋들 */
};

/* [한국어] RZ/G3S 에서 설정 후에 풀 리셋들의 DT 이름.
 * 이 넷이 RZ/G3E 의 rzg3e_pcie_config_post_init() 이 RESET 레지스터 비트로
 * 푸는 것들에 대응한다. */
static const char * const rzg3s_soc_cfg_resets[] = {
	"rst_b", "rst_ps_b", "rst_gp_b", "rst_rsm_b", /* [한국어] 컨트롤러 본체·PS·GP·RSM 리셋 */
};

/* [한국어] RZ/G3S(r9a08g045)의 SoC 표.
 * 리셋을 모두 리셋 프레임워크로 다루고, PHY 설정을 소프트웨어가 채워 넣는
 * 것이 이 SoC 의 성격이다. 그래서 config_pre_init 이 없고 init_phy 가 있다. */
static const struct rzg3s_pcie_soc_data rzg3s_soc_data = {
	.power_resets = rzg3s_soc_power_resets, /* [한국어] 전원 리셋 이름 배열 */
	.num_power_resets = ARRAY_SIZE(rzg3s_soc_power_resets), /* [한국어] 그 개수 */
	.cfg_resets = rzg3s_soc_cfg_resets, /* [한국어] 설정 리셋 이름 배열 */
	.num_cfg_resets = ARRAY_SIZE(rzg3s_soc_cfg_resets), /* [한국어] 그 개수 */
	.config_post_init = rzg3s_pcie_config_post_init, /* [한국어] 설정 후 리셋 해제(리셋 프레임워크 판) */
	.config_deinit = rzg3s_pcie_config_deinit, /* [한국어] 그 역 */
	.init_phy = rzg3s_soc_pcie_init_phy, /* [한국어] PHY 레지스터를 표대로 채운다 */
	.sysc_info = { /* [한국어] 이 SoC 의 SYSC 좌표표 */
		.functions = { /* [한국어] 기능 식별자를 인덱스로 쓰는 지정 초기화 */
			[RZG3S_SYSC_FUNC_ID_RST_RSM_B] = { /* [한국어] 이 SoC 는 RST_RSM_B 만 SYSC 에 있다 */
				.offset = 0xd74, /* [한국어] 그 레지스터 오프셋 */
				.mask = BIT(0), /* [한국어] 그 안의 비트. 나머지 기능은 채우지 않아 mask 가 0 이 되고, 그 호출들은 아무 일도 하지 않는다 */
			},
		},
	},
};

/* [한국어] RZ/G3E 에서 전원 인가 후 풀 리셋의 DT 이름.
 * 하나뿐인 것은 나머지 리셋을 RESET 레지스터 비트로 다루기 때문이다. */
static const char * const rzg3e_soc_power_resets[] = { "aresetn" };

/* [한국어] RZ/G3E(r9a09g047)의 SoC 표.
 * 리셋을 대부분 컨트롤러의 RESET 레지스터 비트로 다루고 PHY 설정이 필요
 * 없는 것이 이 SoC 의 성격이다. 그래서 config_pre_init 이 있고 init_phy 가
 * 없으며, cfg_resets 목록과 개수가 아예 채워지지 않는다. */
static const struct rzg3s_pcie_soc_data rzg3e_soc_data = {
	.power_resets = rzg3e_soc_power_resets, /* [한국어] 전원 리셋 이름 배열 */
	.num_power_resets = ARRAY_SIZE(rzg3e_soc_power_resets), /* [한국어] 그 개수. cfg_resets 쪽은 채우지 않아 개수가 0 이 되고, optional 조회라 문제없다 */
	.config_pre_init = rzg3e_pcie_config_pre_init, /* [한국어] 설정 전에 LOAD_B/CFG_B 를 푼다 */
	.config_post_init = rzg3e_pcie_config_post_init, /* [한국어] 설정 후에 나머지 비트를 순서대로 푼다 */
	.config_deinit = rzg3e_pcie_config_deinit, /* [한국어] RESET 레지스터를 0 으로 만들어 모두 되돌린다 */
	.sysc_info = { /* [한국어] 이 SoC 의 SYSC 좌표표 */
		.functions = { /* [한국어] 기능 식별자를 인덱스로 쓰는 지정 초기화 */
			[RZG3S_SYSC_FUNC_ID_L1_ALLOW] = { /* [한국어] 이 SoC 는 ASPM L1 허용이 SYSC 에 있다 */
				.offset = 0x1020, /* [한국어] 그 레지스터 오프셋 */
				.mask = BIT(0), /* [한국어] 그 안의 비트 */
			},
			[RZG3S_SYSC_FUNC_ID_MODE] = { /* [한국어] RC/EP 모드 선택도 SYSC 에 있다 */
				.offset = 0x1024, /* [한국어] 그 레지스터 오프셋 */
				.mask = BIT(0), /* [한국어] 그 안의 비트. RST_RSM_B 는 채우지 않아 그 호출이 아무 일도 하지 않는다 */
			},
		},
	},
};

/* [한국어] DT compatible 문자열과 위 SoC 표를 잇는 매칭 표.
 * probe 가 device_get_match_data() 로 여기서 표를 꺼내며, 그 한 번의 조회가
 * 이 드라이버의 모든 SoC 분기의 출발점이 된다. */
static const struct of_device_id rzg3s_pcie_of_match[] = {
	{
		.compatible = "renesas,r9a08g045-pcie", /* [한국어] RZ/G3S */
		.data = &rzg3s_soc_data, /* [한국어] 그 SoC 표 */
	},
	{
		.compatible = "renesas,r9a09g047-pcie", /* [한국어] RZ/G3E */
		.data = &rzg3e_soc_data, /* [한국어] 그 SoC 표 */
	},
	{} /* [한국어] 표의 끝을 알리는 빈 항목 */
};

/* [한국어] 플랫폼 드라이버 등록 정보.
 * remove 콜백이 없다는 점이 눈에 띄는데, 아래 suppress_bind_attrs 와 함께
 * 이 드라이버가 한 번 붙으면 떼어지지 않는 것을 전제로 함을 보여 준다. */
static struct platform_driver rzg3s_pcie_driver = {
	.driver = {
		.name = "rzg3s-pcie-host", /* [한국어] sysfs 등에 보일 드라이버 이름 */
		.of_match_table = rzg3s_pcie_of_match, /* [한국어] 위에서 정의한 compatible 매칭 표 */
		.pm = pm_ptr(&rzg3s_pcie_pm_ops), /* [한국어] 절전 콜백 표. pm_ptr 는 CONFIG_PM 이 꺼져 있으면 NULL 로 접어 준다 */
		.suppress_bind_attrs = true, /* [한국어] sysfs 를 통한 수동 bind/unbind 를 막는다 — 떼는 경로가 없기 때문이다 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS, /* [한국어] 부팅을 빠르게 하려고 비동기 probe 를 선호한다고 알린다 */
	},
	.probe = rzg3s_pcie_probe, /* [한국어] DT 매칭이 성사되면 불린다 */
};
builtin_platform_driver(rzg3s_pcie_driver); /* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다. remove 가 없는 것과 같은 맥락이다 */
