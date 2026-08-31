// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Rockchip SoCs.
 *
 * Copyright (C) 2021 Rockchip Electronics Co., Ltd.
 *		http://www.rock-chips.com
 *
 * Author: Simon Xue <xxm@rock-chips.com>
 */

/*
 * [한국어 설명] Rockchip SoC 용 DesignWare PCIe 글루 드라이버 (pcie-dw-rockchip.c)
 *
 * === 파일의 역할 ===
 * Rockchip RK3568/RK3588 계열 SoC 에 들어간 Synopsys DesignWare PCIe IP 를
 * 리눅스에 붙이는 글루(glue) 드라이버다. DWC 코어가 담당하지 못하는 것 --
 * SoC 고유의 APB 제어 레지스터, 클록/리셋/PHY/레귤레이터, PERST# GPIO,
 * 그리고 이 SoC 만의 INTx 수집 회로 -- 을 맡고, 나머지 PCIe 동작은 전부
 * pcie-designware-host.c / pcie-designware-ep.c 에 넘긴다.
 * 한 파일이 **루트 컴플렉스와 엔드포인트 두 역할을 모두** 지원하는 점이
 * 특징이다. DT compatible 에 딸려 오는 rockchip_pcie_of_data.mode 가 어느
 * 쪽으로 갈지 정하고, probe 가 그에 따라 configure_rc / configure_ep 로
 * 갈라진다.
 * 이 SoC 제어 레지스터의 독특한 규약이 파일 전체를 지배한다: **상위 16비트가
 * 하위 16비트에 대한 쓰기 마스크**여서, 값을 넣을 때 '바꿀 비트' 를 상위에
 * 함께 세워야 한다(바로 아래 상류 주석에 그 근거가 적혀 있다).
 * FIELD_PREP_WM16() 이 그 한 쌍을 만들어 준다. 덕분에 읽기-수정-쓰기 없이
 * 원하는 비트만 건드릴 수 있고, 다른 비트는 하드웨어가 무시한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 컨트롤러 드라이버 계층의 SoC 글루 단이다. 위로는 플랫폼 버스가
 * rockchip_pcie_probe() 를 부르고, 아래로는 DWC 코어의 진입점
 * dw_pcie_host_init() 또는 dw_pcie_ep_init() 을 부른다. 반대로 DWC 코어는
 * struct dw_pcie_ops(link_up/start_link/stop_link/get_ltssm)와
 * dw_pcie_host_ops.init, dw_pcie_ep_ops 를 통해 이 파일로 되돌아 들어온다 --
 * 즉 호출이 양방향이며, 그 접점이 이 파일의 콜백들이다.
 * 실행 컨텍스트는 넷이다: (1) probe/host_init 은 프로세스 문맥,
 * (2) rockchip_pcie_intx_handler 는 인터럽트 문맥의 연쇄 핸들러,
 * (3) rockchip_pcie_ep_sys_irq_thread 는 스레드 IRQ(프로세스 문맥),
 * (4) rockchip_pcie_ltssm_trace_work 는 지연 워크큐(프로세스 문맥).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 쪽: DWC 코어(pcie-designware.h), PCI 코어(../../pci.h),
 * clk / reset / phy / gpiod / regulator 서브시스템, irqdomain, 그리고
 * trace/events/pci_controller.h 의 트레이스포인트.
 * 이 파일에 의존하는 쪽은 없다 -- 심볼을 EXPORT 하지 않는 잎(leaf) 드라이버다.
 * 데이터 흐름: DT 가 준 자원(apb 레지스터, reset gpio, clk, phy, irq)이
 * struct rockchip_pcie 에 모이고, 그 안에 값으로 박힌 struct dw_pcie 를 통해
 * DWC 코어와 상태를 공유한다. 두 계층 사이의 포인터 변환은
 * to_rockchip_pcie() 매크로가 맡는데, 이것이 container_of 가 아니라
 * dev_get_drvdata 인 점이 이 드라이버의 특징이다 -- probe 가
 * platform_set_drvdata 로 심어 둔 값을 그대로 꺼내 쓴다.
 * 공유 상태의 핵심은 struct dw_pcie 안의 필드다: pci->l1ss_support 는 이
 * 파일이 세우고 DWC 코어가 읽어 L1 하위상태 광고 여부를 정하며,
 * pci->dbi_base2(BAR 마스크용 두 번째 DBI 창)도 이 파일이 채워 준다.
 * NVMe 와의 관계: 이 드라이버가 세우는 것은 **버스** 이고, NVMe 는 그 버스
 * 위에서 열거되는 여러 장치 중 하나일 뿐이다. drivers/nvme 는 이 파일의
 * 어떤 심볼도 호출하지 않으며(이 트리에서 확인), 이 파일의 코드에도 NVMe
 * 고유 동작은 하나도 없다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - rockchip_pcie_probe(): 진입점. 자원 확보 → 리셋 assert → 레귤레이터 →
 *    PHY → 리셋 deassert → 클록 순으로 올린 뒤 mode 에 따라 RC/EP 로 분기.
 *  - rockchip_pcie_configure_rc() / _configure_ep(): 각 모드의 설정. APB 의
 *    모드 비트를 바꾸고 DWC 코어의 init 을 부른다. 해당 Kconfig 가 꺼져 있으면
 *    -ENODEV 로 물러난다.
 *  - rockchip_pcie_start_link() / _stop_link(): LTSSM 을 켜고 끄며 PERST# GPIO
 *    를 다룬다. 시작 순서(PERST assert → LTSSM enable → 대기 → PERST deassert)
 *    가 규약 타이밍에 매여 있다.
 *  - rockchip_pcie_get_ltssm(): LTSSM 상태를 돌려준다. 단순히 상태 레지스터를
 *    읽는 것이 아니라 RAS-DES 정보를 먼저 보고 L1.1/L1.2 를 구별한다.
 *  - rockchip_pcie_intx_handler() 와 그 도메인: INTx 4선을 APB 상태 레지스터
 *    한 곳에서 모아 커널 IRQ 로 나눈다.
 *  - rockchip_pcie_ep_sys_irq_thread(): EP 모드에서 링크 상태 변화와 핫리셋을
 *    받아 DWC EP 코어에 알린다.
 *  - struct rockchip_pcie: 이 드라이버의 인스턴스. 첫 필드가 struct dw_pcie 다.
 *  - struct rockchip_pcie_of_data: DT compatible 마다 달라지는 것 두 가지
 *    (RC/EP 모드, EPC 기능표)를 담는다.
 */

/* [한국어] [한국어] GENMASK/BIT/FIELD_GET, 그리고 이 SoC 의 핵심인 FIELD_PREP 계열을
 * 쓰기 위해 필요하다. 이 파일의 거의 모든 레지스터 상수가 여기 매크로로 만들어진다. */
#include <linux/bitfield.h>
/* [한국어] [한국어] clk_bulk_* API. DT 에 나열된 PCIe 클록을 목록을 몰라도 한꺼번에
 * 얻고 켜기 위해 쓴다(rockchip_pcie_clk_init). */
#include <linux/clk.h>
/* [한국어] [한국어] gpiod_* API. PERST# 리셋 신호를 GPIO 로 제어하기 위해 필요하다
 * (rockchip_pcie_start_link 의 assert/deassert). */
#include <linux/gpio/consumer.h>
/* [한국어] [한국어] FIELD_PREP_WM16 을 제공한다. 이 SoC 레지스터의 '상위 16비트 =
 * 하위 16비트의 쓰기 마스크' 규약을 코드로 표현하는 핵심 매크로다. */
#include <linux/hw_bitfield.h>
/* [한국어] [한국어] chained_irq_enter/exit. INTx 4선을 GIC 선 하나에서 분해하는
 * 연쇄 핸들러를 안전하게 감싸기 위해 필요하다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] [한국어] irq_domain_create_linear 등 IRQ 도메인 API. INTx 를 커널 IRQ 번호로
 * 매핑할 도메인을 만드는 데 쓴다. */
#include <linux/irqdomain.h>
/* [한국어] [한국어] syscon/regmap 헤더. 이 파일에서 직접 참조하는 심볼은 없다 --
 * Rockchip 드라이버 묶음의 관례적 포함으로 보인다. */
#include <linux/mfd/syscon.h>
/* [한국어] [한국어] MODULE_* 매크로와 module_platform_driver 를 위해 필요하다. */
#include <linux/module.h>
/* [한국어] [한국어] of_device_is_compatible, of_property_read_bool 등 DT 조회 API.
 * RK3588 판별과 supports-clkreq 속성 읽기에 쓴다. */
#include <linux/of.h>
/* [한국어] [한국어] of_irq_get_byname. DT 의 'legacy' 인터럽트를 이름으로 찾는다. */
#include <linux/of_irq.h>
/* [한국어] [한국어] phy_init/phy_power_on 등 PHY 프레임워크 API. */
#include <linux/phy/phy.h>
/* [한국어] [한국어] platform_driver 등록과 platform_get_irq_byname,
 * devm_platform_ioremap_resource_byname 을 위해 필요하다. */
#include <linux/platform_device.h>
/* [한국어] [한국어] regmap API. syscon.h 와 마찬가지로 이 파일에서 직접 쓰는 심볼은 없다. */
#include <linux/regmap.h>
/* [한국어] [한국어] reset_control_assert/deassert. 컨트롤러 리셋 라인을 제어한다. */
#include <linux/reset.h>
/* [한국어] [한국어] delayed_work 와 schedule_delayed_work. LTSSM 추적 워크에 필요하다. */
#include <linux/workqueue.h>
/* [한국어] [한국어] trace_pcie_ltssm_state_transition 트레이스포인트 정의. 이 헤더를
 * 포함해야 trace_..._enabled() 판별 함수도 함께 생긴다. */
#include <trace/events/pci_controller.h>

/* [한국어] [한국어] PCI 코어 내부 헤더. PCIE_T_PVPERL_MS 같은 규약 타이밍 상수와
 * PCI_NUM_INTX 등을 여기서 가져온다. */
#include "../../pci.h"
/* [한국어] [한국어] DWC 코어의 공개 인터페이스. struct dw_pcie, dw_pcie_host_init,
 * dw_pcie_ep_init, DBI 접근자, enum dw_pcie_ltssm 이 전부 여기 있다. */
#include "pcie-designware.h"

/*
 * The upper 16 bits of PCIE_CLIENT_CONFIG are a write
 * mask for the lower 16 bits.
 */

/* [한국어] [한국어] dw_pcie 포인터에서 이 드라이버 인스턴스를 되찾는 매크로.
 * container_of 가 아니라 dev_get_drvdata 인 점이 특징이다 -- probe 가
 * platform_set_drvdata 로 심어 둔 값을 꺼낸다. 그래서 probe 에서 그 호출이
 * 다른 어떤 콜백보다 먼저 있어야 한다.
 * (struct rockchip_pcie 의 첫 필드가 pci 이므로 container_of 로도 가능했겠지만,
 * 상류는 drvdata 방식을 택했다.) */
#define to_rockchip_pcie(x) dev_get_drvdata((x)->dev)

/* General Control Register */
/* [한국어] [한국어] 일반 제어 레지스터. RC/EP 모드 선택과 LTSSM 스위치가 한 워드에 있다.
 * 바로 위 상류 주석의 '상위 16비트가 쓰기 마스크' 규약이 적용되는 대표 레지스터다. */
#define PCIE_CLIENT_GENERAL_CON		0x0
/* [한국어] [한국어] 모드 선택 필드(비트 4~7). */
#define  PCIE_CLIENT_MODE_MASK		GENMASK(7, 4)
/* [한국어] [한국어] 엔드포인트 모드 값(0). 이 값이 0 이라 EP 설정은 마스크만 세우고
 * 값은 0 을 쓰는 형태가 된다. */
#define  PCIE_CLIENT_MODE_EP		0x0UL
/* [한국어] [한국어] 루트 컴플렉스 모드 값(4). */
#define  PCIE_CLIENT_MODE_RC		0x4UL
/* [한국어] [한국어] 모드 값을 '쓰기 마스크 + 값' 쌍으로 만든다. FIELD_PREP_WM16 이
 * 하위 16비트에 값을, 상위 16비트에 해당 필드의 마스크를 넣어 주므로,
 * 이 한 번의 쓰기로 다른 비트를 건드리지 않고 모드만 바꾼다. */
#define  PCIE_CLIENT_SET_MODE(x)	FIELD_PREP_WM16(PCIE_CLIENT_MODE_MASK, (x))
/* [한국어] [한국어] 링크 다운 리셋 요청을 승인하는 비트(비트 3).
 * 이 상수를 참조하는 곳은 이 파일에 없다. */
#define  PCIE_CLIENT_LD_RQ_RST_GRT	FIELD_PREP_WM16(BIT(3), 1)
/* [한국어] [한국어] LTSSM 활성 비트(비트 2)를 1 로. 링크 학습을 시작시킨다. */
#define  PCIE_CLIENT_ENABLE_LTSSM	FIELD_PREP_WM16(BIT(2), 1)
/* [한국어] [한국어] 같은 비트를 0 으로. 마스크만 세우고 값은 0 이므로 링크 학습이 멈춘다. */
#define  PCIE_CLIENT_DISABLE_LTSSM	FIELD_PREP_WM16(BIT(2), 0)

/* Interrupt Status Register Related to Legacy Interrupt */
/* [한국어] [한국어] INTx 상태 레지스터. 하위 4비트가 INTA~INTD 각각에 대응하며,
 * 연쇄 핸들러가 이것을 읽어 어느 선이 울렸는지 가린다. */
#define PCIE_CLIENT_INTR_STATUS_LEGACY	0x8

/* Interrupt Status Register Related to Miscellaneous Operation */
/* [한국어] [한국어] 기타 사건 상태 레지스터. EP 모드에서 링크 변화와 핫리셋을 알린다.
 * write-1-to-clear 라서 핸들러가 읽은 값을 그대로 되써 지운다. */
#define PCIE_CLIENT_INTR_STATUS_MISC	0x10
/* [한국어] [한국어] RDLH(Receive Data Link Layer) 링크 업 상태가 바뀌었다는 비트.
 * 올라갈 때와 내려갈 때 모두 서므로, 방향은 따로 확인해야 한다. */
#define  PCIE_RDLH_LINK_UP_CHGED	BIT(1)
/* [한국어] [한국어] 호스트가 링크 리셋을 요청했다는 비트(핫리셋 또는 링크 다운 리셋).
 * EP 모드에서 이 신호를 받아 상위에 알리고 악수를 마쳐 줘야 한다. */
#define  PCIE_LINK_REQ_RST_NOT_INT	BIT(2)

/* Interrupt Mask Register Related to Legacy Interrupt */
/* [한국어] [한국어] INTx 마스크 레지스터. 하위 4비트가 각 INTx 의 차단 여부다. */
#define PCIE_CLIENT_INTR_MASK_LEGACY	0x1c
/* [한국어] [한국어] 유효한 마스크 비트 범위(하위 8비트). 아래 CLAMP 가 이 범위 밖을 자른다. */
#define  PCIE_INTR_MASK			GENMASK(7, 0)
/* [한국어] [한국어] BIT(_x) 에 유효 범위를 씌워, hwirq 가 8 이상이어도 엉뚱한 비트를
 * 건드리지 않게 하는 방어다. 실제로는 도메인 크기가 4 라 0~3 만 들어온다. */
#define  PCIE_INTR_CLAMP(_x)		((BIT((_x)) & PCIE_INTR_MASK))
/* [한국어] [한국어] '이 INTx 를 막는다': 하위 16비트에 값 1, 상위 16비트에 같은 위치의
 * 쓰기 마스크를 함께 넣는다. 두 번 넣는 이유가 바로 이 레지스터 규약이다. */
#define  PCIE_INTR_LEGACY_MASK(x)	(PCIE_INTR_CLAMP((x)) | \
					 (PCIE_INTR_CLAMP((x)) << 16))
/* [한국어] [한국어] '이 INTx 를 연다': 쓰기 마스크만 세우고 값 자리는 0 으로 둔다.
 * 값 0 을 쓰려면 상위만 세우면 되므로 MASK 와 이렇게 비대칭이 된다. */
#define  PCIE_INTR_LEGACY_UNMASK(x)	(PCIE_INTR_CLAMP((x)) << 16)

/* Interrupt Mask Register Related to Miscellaneous Operation */
/* [한국어] [한국어] 기타 사건의 마스크 레지스터. configure_ep 가 마지막에 두 통지
 * 비트를 여는 데 쓴다. */
#define PCIE_CLIENT_INTR_MASK_MISC	0x24

/* Power Management Control Register */
/* [한국어] [한국어] 전력 관리 제어 레지스터. CLKREQ# 신호의 상태와 풀다운을 다룬다. */
#define PCIE_CLIENT_POWER_CON		0x2c
/* [한국어] [한국어] CLKREQ# 준비 완료(비트 0 = 1). 보드에 CLKREQ 배선이 있을 때 쓴다. */
#define  PCIE_CLKREQ_READY		FIELD_PREP_WM16(BIT(0), 1)
/* [한국어] [한국어] CLKREQ# 미준비(비트 0 = 0). */
#define  PCIE_CLKREQ_NOT_READY		FIELD_PREP_WM16(BIT(0), 0)
/* [한국어] [한국어] CLKREQ# 를 풀다운으로 고정(비트 12~13 필드에 값 1). 배선이 없을 때
 * 신호를 assert 상태로 묶어, 장치가 클록이 항상 있다고 보게 만든다. */
#define  PCIE_CLKREQ_PULL_DOWN		FIELD_PREP_WM16(GENMASK(13, 12), 1)

/* RASDES TBA information */
/* [한국어] [한국어] RAS-DES(Reliability/Availability/Serviceability -- Debug, Error
 * injection, Statistics) 의 TBA(Time-Based Analysis) 공통 정보 레지스터.
 * DWC 벤더 확장 영역이지만 이 SoC 는 그 일부를 APB 창에도 비춰 준다. */
#define PCIE_CLIENT_CDM_RASDES_TBA_INFO_CMN	0x154
/* [한국어] [한국어] 지금 L1.1 에 있다는 비트. LTSSM 상태 레지스터는 L1 을 뭉뚱그려
 * 보여 주므로, 하위상태 구별은 이 비트로만 가능하다. */
#define  PCIE_CLIENT_CDM_RASDES_TBA_L1_1	BIT(4)
/* [한국어] [한국어] 지금 L1.2 에 있다는 비트. */
#define  PCIE_CLIENT_CDM_RASDES_TBA_L1_2	BIT(5)

/* Debug FIFO information */
/* [한국어] [한국어] 디버그 FIFO 모드 제어 레지스터. LTSSM 전이 이력 수집을 켜고 끈다. */
#define PCIE_CLIENT_DBG_FIFO_MODE_CON	0x310
/* [한국어] [한국어] FIFO 를 켜는 값. 0xffff0007 = 상위 16비트 전부 쓰기 마스크,
 * 하위에 비트 0~2 활성. 마스크를 전부 세웠으므로 하위 16비트 전체를 이 값으로
 * 덮어쓴다. */
#define  PCIE_CLIENT_DBG_EN		0xffff0007
/* [한국어] [한국어] FIFO 를 끄는 값. 마스크는 전부, 값은 0 -- 하위 16비트를 모두 0 으로. */
#define  PCIE_CLIENT_DBG_DIS		0xffff0000
/* [한국어] [한국어] 패턴 히트 조건 레지스터 D0. 어떤 상태에서 기록할지 거르는 필터다. */
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D0	0x320
/* [한국어] [한국어] 패턴 히트 조건 레지스터 D1. */
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D1	0x324
/* [한국어] [한국어] 전이 히트 조건 레지스터 D0. 어떤 전이에서 기록할지 거르는 필터다. */
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D0	0x328
/* [한국어] [한국어] 전이 히트 조건 레지스터 D1. */
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D1	0x32c
/* [한국어] [한국어] 위 네 필터에 쓰는 값(0xffff0000). 마스크 전체 + 값 0 이므로 필터를
 * 비워 '모든 전이를 기록' 하게 만든다. */
#define  PCIE_CLIENT_DBG_TRANSITION_DATA 0xffff0000
/* [한국어] [한국어] FIFO 상태 레지스터. **읽을 때마다 한 항목씩 소비된다** -- 그래서
 * 트레이스가 꺼져 있을 때는 읽지 않는다. */
#define PCIE_CLIENT_DBG_FIFO_STATUS	0x350
/* [한국어] [한국어] 그 항목에 실린 링크 속도 등급(비트 20~22). 0 이 2.5GT/s 다. */
#define  PCIE_DBG_FIFO_RATE_MASK	GENMASK(22, 20)
/* [한국어] [한국어] 그 항목에 실린 L1 하위상태 표시(비트 8~10). 1 이면 L1.1, 2 면 L1.2. */
#define  PCIE_DBG_FIFO_L1SUB_MASK	GENMASK(10, 8)
/* [한국어] [한국어] 한 번의 워크에서 FIFO 를 읽을 최대 횟수(64). 고장난 하드웨어가
 * 끝없이 값을 뱉어도 워크가 CPU 를 물고 있지 않게 하는 상한이다. */
#define PCIE_DBG_LTSSM_HISTORY_CNT	64

/* Hot Reset Control Register */
/* [한국어] [한국어] 핫리셋 제어 레지스터. LTSSM 활성 방식과 EP 모드의 지연 악수를 다룬다. */
#define PCIE_CLIENT_HOT_RESET_CTRL	0x180
/* [한국어] [한국어] 애플리케이션 지연2 활성(비트 1). EP 전용 -- 핫리셋 때 LTSSM 이
 * 소프트웨어 처리를 기다리게 만든다. */
#define  PCIE_LTSSM_APP_DLY2_EN		BIT(1)
/* [한국어] [한국어] 그 지연이 끝났음을 알리는 비트(비트 3). ep_sys_irq_thread 가
 * 정리를 마친 뒤 이것을 세워 LTSSM 을 다음 단계로 보낸다. */
#define  PCIE_LTSSM_APP_DLY2_DONE	BIT(3)
/* [한국어] [한국어] LTSSM 활성 방식을 개선판으로 바꾸는 비트(비트 4). RC/EP 양쪽이
 * 모두 세운다. */
#define  PCIE_LTSSM_ENABLE_ENHANCE	BIT(4)

/* LTSSM Status Register */
/* [한국어] [한국어] LTSSM 상태 레지스터. 한 워드에 두 가지 정보가 들어 있다 --
 * 링크 업 여부(비트 16~17)와 LTSSM 상태 코드(하위 6비트). */
#define PCIE_CLIENT_LTSSM_STATUS	0x300
/* [한국어] [한국어] 링크 업을 뜻하는 값(3). link_up() 이 이 값과 정확히 견준다. */
#define  PCIE_LINKUP			0x3
/* [한국어] [한국어] 링크 업 필드의 자리(비트 16~17). */
#define  PCIE_LINKUP_MASK		GENMASK(17, 16)
/* [한국어] [한국어] LTSSM 상태 코드의 자리(하위 6비트). enum dw_pcie_ltssm 값과 대응한다. */
#define  PCIE_LTSSM_STATUS_MASK		GENMASK(5, 0)

/* [한국어] [한국어] DBI 창에서 DBI2 창까지의 거리(1MiB). DBI2 는 BAR 마스크(크기)를
 * 쓰는 두 번째 창이다 -- 같은 오프셋에 BAR 주소와 마스크를 둘 다 둘 수 없어
 * IP 가 창을 하나 더 노출하는데, 그 위치는 SoC 마다 달라 DWC 코어가 스스로
 * 알 수 없다. host_init 이 이 값으로 pci->dbi_base2 를 채워 준다. */
#define PCIE_TYPE0_HDR_DBI2_OFFSET      0x100000

/* [한국어] [한국어] 이 드라이버의 인스턴스 구조체. probe 가 devm_kzalloc 으로 하나
 * 만들어 platform_set_drvdata 로 매달아 둔다. */
struct rockchip_pcie {
	/* [한국어] [한국어] DWC 코어의 인스턴스. **포인터가 아니라 값으로** 품고 있다.
	 * 설정자: probe 가 dev/ops/n_fts 를 채운다.
	 * 읽는 자: DWC 코어 전체, 그리고 이 파일의 모든 콜백이 &rockchip->pci 로 접근.
	 * 값 범위: 항상 유효한 임베디드 구조체 -- NULL 이 될 수 없다.
	 * 동기화: 첫 필드라 &rockchip->pci 와 rockchip 의 주소가 같다. 다만 이 파일은
	 * 역방향 변환에 container_of 대신 dev_get_drvdata 를 쓴다. */
	struct dw_pcie pci;
	/* [한국어] [한국어] SoC 전용 APB 제어 레지스터 창의 커널 가상 주소.
	 * 설정자: rockchip_pcie_resource_get 이 DT 의 'apb' 자원을 ioremap.
	 * 읽는 자: rockchip_pcie_readl_apb / writel_apb 만이 직접 쓴다.
	 * 값 범위: 유효한 __iomem 포인터. 실패 시 probe 가 중단되므로 NULL 로 남지 않는다.
	 * 동기화: 없음. 이 창의 레지스터는 상위 16비트 쓰기 마스크 덕에 읽기-수정-쓰기가
	 * 필요 없어, 서로 다른 비트를 건드리는 동시 접근이 안전하다. */
	void __iomem *apb_base;
	/* [한국어] [한국어] PCIe PHY 핸들.
	 * 설정자: rockchip_pcie_phy_init 이 devm_phy_get 으로 얻는다.
	 * 읽는 자: phy_init/power_on/power_off/exit 를 부르는 두 함수뿐.
	 * 값 범위: 유효한 phy 포인터. devm 이라 해제는 자동이다.
	 * 동기화: probe 와 그 실패 되감기에서만 다뤄지므로 경쟁이 없다. */
	struct phy *phy;
	/* [한국어] [한국어] DT 에 나열된 클록들의 배열.
	 * 설정자: rockchip_pcie_clk_init 이 devm_clk_bulk_get_all 로 채운다.
	 * 읽는 자: clk_bulk_prepare_enable 과 probe 의 실패 경로의
	 * clk_bulk_disable_unprepare.
	 * 값 범위: clk_cnt 개의 항목. 개수를 드라이버가 미리 알 필요가 없다.
	 * 동기화: probe 문맥 전용. */
	struct clk_bulk_data *clks;
	/* [한국어] [한국어] 위 배열의 항목 수.
	 * 설정자: clk_init 이 devm_clk_bulk_get_all 의 반환값(개수)을 담는다.
	 * 읽는 자: clk_bulk_* 호출들.
	 * 값 범위: 0 이상. 음수는 실패로 걸러진 뒤라 들어오지 않는다.
	 * 동기화: probe 문맥 전용. */
	unsigned int clk_cnt;
	/* [한국어] [한국어] DT 에 나열된 리셋 라인들을 묶은 핸들.
	 * 설정자: resource_get 이 devm_reset_control_array_get_exclusive 로 얻는다.
	 * 읽는 자: probe 의 assert/deassert.
	 * 값 범위: 유효한 핸들. exclusive 라 다른 드라이버와 공유되지 않는다.
	 * 동기화: probe 문맥 전용. 실패 경로에서 deassert 하지 않는 것이 의도다 --
	 * 실패한 컨트롤러는 리셋 상태로 두는 편이 안전하다. */
	struct reset_control *rst;
	/* [한국어] [한국어] PERST# 신호를 내보내는 GPIO 서술자.
	 * 설정자: resource_get 이 devm_gpiod_get_optional 로 얻는다.
	 * 읽는 자: rockchip_pcie_start_link 의 assert/deassert.
	 * 값 범위: **NULL 일 수 있다.** 보드가 PERST# 를 전원 회로에 묶어 두어
	 * 소프트웨어 제어가 없는 경우다. gpiod_set_value_cansleep(NULL, ...) 은 아무
	 * 일도 하지 않으므로 start_link 가 그대로 동작한다.
	 * 동기화: start_link 는 링크 기동 경로에서만 불려 경쟁이 없다. */
	struct gpio_desc *rst_gpio;
	/* [한국어] [한국어] INTx 를 커널 IRQ 로 옮기는 도메인.
	 * 설정자: rockchip_pcie_init_irq_domain.
	 * 읽는 자: rockchip_pcie_intx_handler 가 generic_handle_domain_irq 에 넘긴다.
	 * 값 범위: 유효한 도메인. **다만 도메인 생성 실패 시 host_init 이 로그만 남기고
	 * 계속 진행하므로 NULL 로 남을 수 있다**(host_init 주석의 코드 관찰 참조).
	 * 동기화: 생성은 프로세스 문맥, 사용은 인터럽트 문맥. 생성이 핸들러 등록보다
	 * 먼저라는 것이 유일한 순서 보장이다. */
	struct irq_domain *irq_domain;
	/* [한국어] [한국어] DT compatible 에 매인 SoC 별 설정표.
	 * 설정자: probe 가 of_device_get_match_data 결과를 담는다.
	 * 읽는 자: probe 의 모드 분기와 rockchip_pcie_get_features.
	 * 값 범위: NULL 이 아님이 probe 초입에서 보장된다.
	 * 동기화: 읽기 전용(const)이라 경쟁이 없다. */
	const struct rockchip_pcie_of_data *data;
	/* [한국어] [한국어] DT 에 'supports-clkreq' 속성이 있었는지.
	 * 설정자: rockchip_pcie_resource_get.
	 * 읽는 자: rockchip_pcie_configure_l1ss 하나뿐.
	 * 값 범위: true 면 CLKREQ# 배선이 있어 L1 하위상태를 켤 수 있다.
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기만 한다. */
	bool supports_clkreq;
	/* [한국어] [한국어] LTSSM 전이 이력을 5초마다 수집하는 지연 워크.
	 * 설정자: rockchip_pcie_ltssm_trace(true) 가 INIT_DELAYED_WORK 로 초기화하고
	 * 예약한다. probe 가 아니라 켤 때마다 초기화하는 점이 특이한데, 앞선
	 * cancel_delayed_work_sync 가 워크를 완전히 멈춘 뒤이므로 안전하다.
	 * 읽는 자: rockchip_pcie_ltssm_trace_work 가 container_of 로 인스턴스를 되찾는다.
	 * 값 범위: CONFIG_TRACING 이 꺼지면 아무도 건드리지 않는다(필드 자체는 항상 존재).
	 * 동기화: cancel_delayed_work_sync 가 stop_link 경로에서 완료를 보장한다. */
	struct delayed_work trace_work;
};

/* [한국어] [한국어] DT compatible 마다 달라지는 것만 모은 설정표. 각 of_match 항목의
 * .data 가 이 구조체를 가리킨다. */
struct rockchip_pcie_of_data {
	/* [한국어] [한국어] 이 compatible 이 RC 인지 EP 인지.
	 * 설정자: 파일 끝의 정적 인스턴스들이 컴파일 시점에 정한다.
	 * 읽는 자: probe 의 switch 분기.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE. 그 외 값은 default 로 걸러진다.
	 * 동기화: const 정적 데이터라 경쟁이 없다. */
	enum dw_pcie_device_mode mode;
	/* [한국어] [한국어] EP 모드일 때 EPC 코어에 알려 줄 기능표.
	 * 설정자: 파일 끝의 정적 인스턴스.
	 * 읽는 자: rockchip_pcie_get_features.
	 * 값 범위: **RC 항목에서는 NULL 이다**(rockchip_pcie_rc_of_data_rk3568 는 이
	 * 필드를 채우지 않는다). get_features 는 EP 경로에서만 불리므로 문제가 되지 않는다.
	 * 동기화: const 정적 데이터. */
	const struct pci_epc_features *epc_features;
};

/* [한국어]
 * rockchip_pcie_readl_apb - SoC 전용 APB 제어 레지스터를 32비트 읽는다
 *
 * @rockchip: 이 드라이버 인스턴스. apb_base 가 매핑된 창의 시작이다.
 * @reg: 그 창 안의 바이트 오프셋(PCIE_CLIENT_* 상수).
 * @return: 읽은 32비트 값. 반환형이 int 인 점은 상류 그대로다 --
 *          호출자들이 u32 로 받거나 FIELD_GET 에 넘기므로 실질 차이는 없다.
 *
 * DWC 코어가 다루는 DBI 창과는 **완전히 다른 레지스터 공간**이다. DBI 는
 * PCIe 설정공간과 IP 내부 제어를, 이 APB 창은 Rockchip 이 IP 바깥에 덧붙인
 * SoC 고유 회로(모드 선택, LTSSM 스위치, INTx 수집, 디버그 FIFO)를 다룬다.
 * DT 의 "apb" reg 항목이 이 창이며, rockchip_pcie_resource_get() 이 매핑한다.
 *
 * readl_relaxed 를 쓰는 이유: 이 레지스터 접근은 DMA 버퍼 가시성과 순서를
 * 맞출 필요가 없다. 제어 레지스터끼리의 순서는 같은 APB 버스가 보장하므로,
 * 메모리 배리어를 포함한 readl() 대신 relaxed 판을 써 비용을 줄인다.
 *
 * 실행 컨텍스트: 프로세스 문맥과 인터럽트 문맥 양쪽에서 불린다
 * (intx_handler 가 이것으로 상태를 읽는다). 잠금은 없다.
 *
 * 호출 체인:
 *   거의 모든 이 파일의 함수 → [이 함수] → readl_relaxed
 */
static int rockchip_pcie_readl_apb(struct rockchip_pcie *rockchip, u32 reg)
{
	return readl_relaxed(rockchip->apb_base + reg);
}

/* [한국어]
 * rockchip_pcie_writel_apb - SoC 전용 APB 제어 레지스터에 32비트 쓴다
 *
 * @rockchip: 이 드라이버 인스턴스.
 * @val: 쓸 값. 대부분 FIELD_PREP_WM16 이 만든 '쓰기마스크+값' 쌍이다.
 * @reg: 창 안의 바이트 오프셋.
 * @return: 없음.
 *
 * 인자 순서가 val 다음 reg 인 점에 유의 -- writel(val, addr) 의 순서를 따른
 * 것이라 readl 계열과 반대로 보인다. 이 파일 안에서 일관되므로 헷갈릴 일은
 * 없지만, 다른 드라이버의 (reg, val) 관례와는 다르다.
 *
 * 이 함수가 읽기-수정-쓰기를 하지 않는 것이 중요하다. 상류 주석이 밝히듯
 * 이 레지스터들은 상위 16비트가 하위 16비트의 쓰기 마스크라, 바꾸려는 비트만
 * 마스크에 세우면 나머지는 하드웨어가 그대로 둔다. 그래서 값 하나만 쓰면
 * 되고, 그 덕에 동시 접근에 대한 잠금도 필요 없다 -- 서로 다른 비트를
 * 건드리는 두 쓰기가 겹쳐도 서로를 지우지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥과 인터럽트 문맥 양쪽.
 *
 * 호출 체인:
 *   이 파일의 설정/마스킹 함수들 → [이 함수] → writel_relaxed
 */
static void rockchip_pcie_writel_apb(struct rockchip_pcie *rockchip, u32 val,
				     u32 reg)
{
	writel_relaxed(val, rockchip->apb_base + reg);
}

/* [한국어]
 * rockchip_pcie_intx_handler - APB 상태 레지스터에 모인 INTx 4선을 커널 IRQ 로 나눈다
 *
 * @desc: 부모 IRQ 의 irq_desc. 핸들러 데이터에 rockchip 인스턴스가 있다.
 * @return: 없음.
 *
 * PCIe 의 레거시 인터럽트 INTA~INTD 는 각각 별개의 신호지만, 이 SoC 는 넷을
 * 하나의 GIC 선으로 묶어 내보내고 어느 것이 울렸는지는 APB 상태 레지스터의
 * 비트로 알려 준다. 그래서 '연쇄(chained) 핸들러' 형태가 필요하다 -- 상위
 * 선 하나를 받아 하위 넷으로 분해한다.
 *
 * chained_irq_enter/exit 로 감싸는 이유: 부모 인터럽트 컨트롤러에 따라 진입
 * 시 마스킹이나 EOI 처리가 필요한데, 그 차이를 이 두 헬퍼가 흡수한다. 이
 * 쌍을 빠뜨리면 부모 선이 계속 울리거나 반대로 다시 울리지 않는다.
 *
 * for_each_set_bit 의 상한이 4 인 것은 INTx 가 넷뿐이기 때문이다. hwirq 는
 * 0~3 이고, 도메인이 PCI_NUM_INTX(=4) 크기로 만들어져 있어 그대로 대응된다.
 *
 * 상태 비트를 여기서 지우지 않는 점에 유의. 이 SoC 의 INTx 는 레벨 트리거라
 * (도메인이 handle_level_irq 를 건다) 원인이 해소되면 하드웨어가 스스로
 * 상태를 내린다 -- 소프트웨어가 지울 대상이 아니다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   GIC → 부모 irq 핸들러 → [이 함수] → generic_handle_domain_irq
 *     → 디바이스 드라이버의 INTx 핸들러
 */
static void rockchip_pcie_intx_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct rockchip_pcie *rockchip = irq_desc_get_handler_data(desc);
	/* [한국어] [한국어] reg 에 상태 비트들을, hwirq 에 그중 한 비트의 위치를 담는다.
	 * reg 가 unsigned long 인 것은 for_each_set_bit 이 그 타입의 포인터를 받기 때문이다. */
	unsigned long reg, hwirq;

	chained_irq_enter(chip, desc);

	reg = rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_INTR_STATUS_LEGACY);

	for_each_set_bit(hwirq, &reg, 4)
		/* [한국어] [한국어] hwirq(0~3)를 INTx 도메인에 올린다. 도메인이 이 번호를 커널 virq 로
		 * 바꿔 해당 장치 드라이버의 핸들러를 부른다. 상태 비트는 여기서 지우지 않는다 --
		 * 레벨 트리거라 원인이 해소되면 하드웨어가 스스로 내린다. */
		generic_handle_domain_irq(rockchip->irq_domain, hwirq);

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * rockchip_intx_mask - INTx 한 선을 APB 마스크 레지스터에서 막는다
 *
 * @data: 막을 INTx 의 irq_data. hwirq 가 0~3 이고, chip_data 에 인스턴스가 있다.
 * @return: 없음.
 *
 * 값으로 PCIE_INTR_LEGACY_MASK(hwirq) 를 쓴다. 그 매크로는 BIT(hwirq) 를
 * **두 번** 넣는다 -- 하위 16비트에 '막음(1)' 이라는 값으로, 상위 16비트에
 * '이 비트를 바꾸겠다' 는 쓰기 마스크로. 이 SoC 레지스터 규약 덕에 다른
 * INTx 의 마스크 상태를 읽지 않고도 이 한 선만 안전하게 바꿀 수 있다.
 *
 * PCIE_INTR_CLAMP 로 BIT(x) 에 GENMASK(7,0) 을 씌우는 것은, hwirq 가 어떤
 * 이유로 8 이상이 되어도 엉뚱한 상위 비트를 건드리지 않게 하는 방어다.
 * 실제로는 도메인 크기가 4 라 0~3 만 들어온다.
 *
 * 함수 끝에 세미콜론이 붙어 있는데(상류 그대로, 수정하지 않음) 빈 문장일 뿐
 * 동작에는 영향이 없다.
 *
 * 실행 컨텍스트: disable_irq 계열의 프로세스 문맥과, 레벨 IRQ 처리 중의
 * 인터럽트 문맥 양쪽.
 *
 * 호출 체인:
 *   irq 코어 → chip->irq_mask → [이 함수] → rockchip_pcie_writel_apb
 */
static void rockchip_intx_mask(struct irq_data *data)
{
	rockchip_pcie_writel_apb(irq_data_get_irq_chip_data(data),
				 PCIE_INTR_LEGACY_MASK(data->hwirq),
				 PCIE_CLIENT_INTR_MASK_LEGACY);
};

/* [한국어]
 * rockchip_intx_unmask - INTx 한 선의 마스크를 풀어 다시 받게 한다
 *
 * @data: 풀 INTx 의 irq_data.
 * @return: 없음.
 *
 * mask 와의 차이가 매크로 한 글자에 있다. PCIE_INTR_LEGACY_UNMASK(x) 는
 * BIT(x) 를 **상위 16비트에만** 넣는다 -- 즉 '이 비트를 바꾸겠다(마스크=1)' 는
 * 표시만 하고 하위의 값 자리는 0 으로 둔다. 결과적으로 해당 INTx 의 마스크
 * 비트가 0(=허용)이 된다.
 *
 * 이 비대칭이 이 SoC 레지스터 규약의 핵심이다: 상위는 '무엇을 쓸지', 하위는
 * '무슨 값을 쓸지' 이므로, 값 0 을 쓰려면 상위만 세우면 된다.
 *
 * 실행 컨텍스트: enable_irq 계열의 프로세스 문맥과 인터럽트 문맥 양쪽.
 *
 * 호출 체인:
 *   irq 코어 → chip->irq_unmask → [이 함수] → rockchip_pcie_writel_apb
 */
static void rockchip_intx_unmask(struct irq_data *data)
{
	rockchip_pcie_writel_apb(irq_data_get_irq_chip_data(data),
				 PCIE_INTR_LEGACY_UNMASK(data->hwirq),
				 PCIE_CLIENT_INTR_MASK_LEGACY);
};

static struct irq_chip rockchip_intx_irq_chip = {
	/* [한국어] [한국어] /proc/interrupts 에 나타날 이름. */
	.name			= "INTx",
	/* [한국어] [한국어] 마스크/언마스크 콜백. 실제 차단은 APB 마스크 레지스터가 한다. */
	.irq_mask		= rockchip_intx_mask,
	/* [한국어] [한국어] 언마스크 콜백. */
	.irq_unmask		= rockchip_intx_unmask,
	/* [한국어] [한국어] IRQCHIP_SKIP_SET_WAKE 는 이 칩이 웨이크업 설정을 지원하지 않으니
	 * 요청을 조용히 넘기라는 뜻이고, IRQCHIP_MASK_ON_SUSPEND 는 서스펜드 시
	 * 커널이 이 인터럽트를 자동으로 막게 한다. 둘 다 INTx 를 웨이크업 원천으로
	 * 쓰지 않는다는 선언이다. */
	.flags			= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MASK_ON_SUSPEND,
};

/* [한국어]
 * rockchip_pcie_intx_map - INTx 도메인의 hwirq 하나를 커널 virq 에 붙인다
 *
 * @domain: INTx 선형 도메인. host_data 에 rockchip 인스턴스가 있다.
 * @irq: 커널이 배정한 가상 IRQ 번호.
 * @hwirq: 0~3 (INTA~INTD).
 * @return: 항상 0. 실패할 일이 없다.
 *
 * 도메인에 새 매핑이 생길 때마다 한 번씩 불린다. 여기서 두 가지를 건다:
 *   - irq_chip 과 흐름 핸들러: handle_level_irq 를 쓰는 것이 MSI 쪽
 *     (handle_edge_irq)과 다른 점이다. INTx 는 원인이 해소될 때까지 계속
 *     유지되는 레벨 신호이므로, 핸들러가 끝난 뒤 다시 확인해야 한다.
 *   - chip_data 에 도메인의 host_data(= rockchip 인스턴스)를 심는다.
 *     mask/unmask 가 irq_data_get_irq_chip_data() 로 이것을 되찾아
 *     APB 레지스터에 접근한다.
 *
 * 실행 컨텍스트: 장치가 INTx 를 요청할 때의 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_mapping → domain_ops->map → [이 함수]
 */
static int rockchip_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	/* [한국어] [한국어] handle_level_irq 를 거는 것이 MSI 쪽(handle_edge_irq)과 다른 점이다.
	 * INTx 는 원인이 해소될 때까지 유지되는 레벨 신호이므로, 핸들러가 끝난 뒤
	 * 다시 확인하는 흐름이 필요하다. */
	irq_set_chip_and_handler(irq, &rockchip_intx_irq_chip, handle_level_irq);
	/* [한국어] [한국어] 도메인의 host_data(= rockchip 인스턴스)를 이 IRQ 의 chip_data 로
	 * 심는다. mask/unmask 가 irq_data_get_irq_chip_data 로 되찾아 APB 레지스터에
	 * 닿는 통로다. */
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] [한국어] 이 도메인은 map 만 제공한다. 선형 도메인이라 hwirq 와 virq 의 대응은
	 * 커널이 관리하고, 드라이버는 새 매핑이 생길 때 칩과 핸들러를 붙이면 된다. */
	.map = rockchip_pcie_intx_map,
};

/* [한국어]
 * rockchip_pcie_init_irq_domain - DT 의 자식 인터럽트 컨트롤러 노드로 INTx 도메인을 만든다
 *
 * @rockchip: 이 인스턴스. 만들어진 도메인이 rockchip->irq_domain 에 저장된다.
 * @return: 0 성공, -EINVAL 은 DT 노드가 없거나 도메인 생성 실패.
 *
 * DT 관례상 PCIe 컨트롤러의 INTx 는 자식 노드
 * "legacy-interrupt-controller" 로 기술된다. 그래야 하위 장치 노드가
 * interrupt-parent 로 그 노드를 가리켜 INTA~D 를 요청할 수 있다. 그래서
 * 도메인의 fwnode 를 컨트롤러 자신이 아니라 **그 자식 노드**로 잡는다.
 *
 * of_get_child_by_name() 이 참조 카운트를 올리므로, 도메인을 만든 직후
 * of_node_put() 으로 내려놓는다. 도메인이 fwnode 를 따로 붙들기 때문에
 * 여기서 놓아도 안전하다. 실패 경로에서도 놓이도록 put 을 검사보다 **앞에**
 * 둔 순서가 의도적이다.
 *
 * 도메인 크기 PCI_NUM_INTX(=4)는 INTA~INTD 넷에 정확히 대응한다.
 * host_data 로 rockchip 을 넘겨 map 콜백이 되찾게 한다.
 *
 * 실행 컨텍스트: host_init 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_host_init → [이 함수] → irq_domain_create_linear
 */
static int rockchip_pcie_init_irq_domain(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->pci.dev;
	struct device_node *intc;

	intc = of_get_child_by_name(dev->of_node, "legacy-interrupt-controller");
	/* [한국어] [한국어] DT 에 자식 인터럽트 컨트롤러 노드가 없다. 그러면 하위 장치가
	 * INTx 를 요청할 대상이 없으므로 도메인을 만들 이유가 없다. */
	if (!intc) {
		/* [한국어] [한국어] DT 를 고쳐야 하는 문제이므로 명확히 로그로 알린다. */
		dev_err(dev, "missing child interrupt-controller node\n");
		/* [한국어] [한국어] '이 드라이버가 요구하는 DT 형태가 아니다' 라는 뜻으로 -EINVAL. */
		return -EINVAL;
	}

	rockchip->irq_domain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,
							/* [한국어] [한국어] 도메인 연산과 host_data 를 넘긴다. host_data(rockchip)는
							 * intx_map 이 irq_set_chip_data 로 각 IRQ 에 다시 심어, mask/unmask 가
							 * APB 레지스터에 닿을 수 있게 하는 통로다. */
							&intx_domain_ops, rockchip);
	of_node_put(intc);
	if (!rockchip->irq_domain) {
		/* [한국어] [한국어] 도메인 생성 실패. 메모리 부족이나 fwnode 중복 등록이 원인일 수 있다. */
		dev_err(dev, "failed to get a INTx IRQ domain\n");
		/* [한국어] [한국어] 여기서도 -EINVAL 을 쓴다. 호출자(host_init)는 이 값을 로그로만
		 * 남기고 계속 진행한다는 점에 유의(host_init 주석의 코드 관찰 참조). */
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * rockchip_pcie_get_ltssm_reg - LTSSM 상태 레지스터를 날것 그대로 읽는다
 *
 * @rockchip: 이 인스턴스.
 * @return: PCIE_CLIENT_LTSSM_STATUS 의 32비트 값. 필드가 둘 섞여 있다 --
 *          하위 6비트(PCIE_LTSSM_STATUS_MASK)가 LTSSM 상태 코드,
 *          비트 16~17(PCIE_LINKUP_MASK)이 링크 업 표시다.
 *
 * 이 얇은 래퍼가 따로 있는 이유는 같은 레지스터를 서로 다른 목적으로 세 곳이
 * 읽기 때문이다: rockchip_pcie_link_up() 은 링크 업 필드를,
 * rockchip_pcie_get_ltssm() 은 상태 코드를, ep_sys_irq_thread 는 디버그
 * 로그용으로 통째로 쓴다. 필드 해석을 각자에게 맡기고 읽기만 공유한다.
 *
 * 실행 컨텍스트: 프로세스 문맥과 스레드 IRQ 양쪽.
 *
 * 호출 체인:
 *   link_up / get_ltssm / ep_sys_irq_thread → [이 함수]
 *     → rockchip_pcie_readl_apb
 */
static u32 rockchip_pcie_get_ltssm_reg(struct rockchip_pcie *rockchip)
{
	return rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_LTSSM_STATUS);
}

/* [한국어]
 * rockchip_pcie_get_ltssm - DWC 코어에 LTSSM 상태를 알려 준다 (L1 하위상태 보정 포함)
 *
 * @pci: DWC 코어의 인스턴스. to_rockchip_pcie 로 이 드라이버 인스턴스를 되찾는다.
 * @return: enum dw_pcie_ltssm 값. L1.1/L1.2 이거나, 그 외에는 상태 레지스터의
 *          하위 6비트를 그대로 돌려준다.
 *
 * 단순히 상태 레지스터만 읽지 않는 이유가 이 함수의 존재 이유다. LTSSM
 * 레지스터는 L1 하위상태(L1.1 / L1.2)를 구별해 주지 않고 뭉뚱그린 L1 만
 * 보여 준다. 대신 이 SoC 는 RAS-DES(Reliability/Availability/Serviceability
 * -- Debug, Error injection, Statistics) 의 TBA(Time-Based Analysis) 정보
 * 레지스터에 두 비트를 따로 두어 어느 하위상태인지 알려 준다. 그래서
 * **그쪽을 먼저 보고**, 해당이 없을 때만 LTSSM 레지스터로 내려간다.
 *
 * 이 구별이 필요한 이유: L1.1 과 L1.2 는 절전 깊이와 복귀 지연이 크게 달라,
 * 디버깅과 트레이스에서 둘을 뭉치면 전력 문제의 원인을 찾을 수 없다.
 *
 * 두 비트가 동시에 서면 L1.1 이 먼저 반환된다(검사 순서 그대로). 하드웨어가
 * 두 상태를 동시에 보고하는 일은 없으므로 실질적인 모호함은 아니다.
 *
 * 실행 컨텍스트: DWC 코어가 부르는 프로세스 문맥. 트레이스 경로에서도 쓰인다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_get_ltssm) → pci->ops->get_ltssm → [이 함수]
 *     → rockchip_pcie_readl_apb / rockchip_pcie_get_ltssm_reg
 */
static enum dw_pcie_ltssm rockchip_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);
	u32 val = rockchip_pcie_readl_apb(rockchip,
			/* [한국어] [한국어] RAS-DES TBA 공통 정보 레지스터. LTSSM 상태 레지스터보다 **먼저**
			 * 읽는 이유는, 그쪽이 L1 하위상태를 구별해 주지 않기 때문이다. */
			PCIE_CLIENT_CDM_RASDES_TBA_INFO_CMN);

	if (val & PCIE_CLIENT_CDM_RASDES_TBA_L1_1)
		/* [한국어] [한국어] L1.1 -- 클록은 꺼지지만 공통 모드 전압은 유지되는 상태. */
		return DW_PCIE_LTSSM_L1_1;

	if (val & PCIE_CLIENT_CDM_RASDES_TBA_L1_2)
		/* [한국어] [한국어] L1.2 -- 공통 모드 전압까지 내려가는 더 깊은 절전. 복귀가 더 느리다. */
		return DW_PCIE_LTSSM_L1_2;

	return rockchip_pcie_get_ltssm_reg(rockchip) & PCIE_LTSSM_STATUS_MASK;
}

/* [한국어] 아래부터 #else 까지는 CONFIG_TRACING 이 켜졌을 때의 LTSSM 전이
 * 추적 구현이다. 트레이스포인트가 빌드에서 빠지면 하드웨어 디버그 FIFO 를
 * 다룰 이유가 없으므로, #else 쪽에 아무 일도 하지 않는 같은 이름의 함수를
 * 두어 호출부(start_link/stop_link)가 조건 없이 부를 수 있게 한다. */
#ifdef CONFIG_TRACING
/* [한국어]
 * rockchip_pcie_ltssm_trace_work - 디버그 FIFO 에 쌓인 LTSSM 전이 이력을 트레이스로 뽑는다
 *
 * @work: 지연 워크 항목. container_of 로 rockchip 인스턴스를 되찾는다.
 * @return: 없음.
 *
 * 링크가 왜 특정 상태에서 맴도는지는 '지금 상태' 만 봐서는 알 수 없고 전이
 * 이력이 필요하다. 이 SoC 는 그 이력을 하드웨어 링 FIFO 에 쌓아 두고,
 * PCIE_CLIENT_DBG_FIFO_STATUS 를 읽을 때마다 한 항목씩 꺼내 준다. 이 워크가
 * 5초마다 깨어 그것을 비우며 트레이스포인트로 흘려보낸다.
 *
 * 트레이스포인트가 꺼져 있으면 FIFO 를 읽지 않고 곧장 재예약으로 간다
 * (skip_trace). 읽는 행위 자체가 FIFO 를 소비하므로, 아무도 안 볼 때 비우면
 * 나중에 트레이스를 켜도 직전 이력이 사라진다.
 *
 * 루프 종료 조건 두 가지:
 *  - 상류 주석이 설명하는 하드웨어 규약: FIFO 는 'last-read-point' 와
 *    'last-valid-point' 두 카운터로 관리되어, 연속된 두 항목이 같은 상태로
 *    나오면 그 뒤는 유효하지 않은 데이터라는 뜻이다. 그래서 val == prev_val
 *    이면 남은 항목을 건너뛴다. 다음 전이가 생기면 사용자의 마지막 읽기
 *    위치부터 다시 이어 읽을 수 있다는 것이 이 이중 카운터 설계의 이점이다.
 *  - 상태 코드가 DW_PCIE_LTSSM_RCVRY_EQ3 를 넘으면 정의되지 않은 값이므로 중단.
 *  - 최대 PCIE_DBG_LTSSM_HISTORY_CNT(64)회로 상한을 둬, 고장난 하드웨어가
 *    무한히 값을 뱉어도 워크가 CPU 를 물고 있지 않게 한다.
 *
 * L1_IDLE 로 나온 항목은 같은 읽기에서 얻은 l1ss 필드로 L1.1/L1.2 를 구별해
 * 준다 -- get_ltssm() 이 RAS-DES 로 하는 일과 같은 보정을, 여기서는 FIFO 가
 * 함께 실어 준 정보로 한다.
 *
 * 속도 보고에 방어가 하나 있다: (rate + 1) 이 max_link_speed 를 넘으면
 * PCI_SPEED_UNKNOWN 을 넘긴다. FIFO 가 쓰레기 값을 줬을 때 존재하지 않는
 * 속도 등급을 트레이스에 남기지 않기 위해서다.
 *
 * 실행 컨텍스트: 시스템 워크큐의 프로세스 문맥. 5초 주기로 스스로를 다시
 * 예약하므로, 멈추는 것은 stop_link 경로의 cancel_delayed_work_sync 뿐이다.
 *
 * 호출 체인:
 *   워크큐 → [이 함수] → rockchip_pcie_readl_apb
 *     → trace_pcie_ltssm_state_transition → schedule_delayed_work(자기 자신)
 */
static void rockchip_pcie_ltssm_trace_work(struct work_struct *work)
{
	struct rockchip_pcie *rockchip = container_of(work,
						struct rockchip_pcie,
						trace_work.work);
	struct dw_pcie *pci = &rockchip->pci;
	/* [한국어] [한국어] 트레이스에 넘길 최종 상태. val 을 그대로 쓰지 않고 별도 변수를 두는
	 * 이유는 아래에서 L1_IDLE 을 하위상태로 보정하기 때문이다. */
	enum dw_pcie_ltssm state;
	/* [한국어] [한국어] prev_val 을 DW_PCIE_LTSSM_UNKNOWN 으로 시작하는 것이 중요하다.
	 * 첫 항목(i == 0)은 아래 종료 조건에서 i > 0 으로 제외되므로 이 초기값과
	 * 비교되지 않지만, 유효하지 않은 값으로 시작해 두는 편이 안전하다. */
	u32 i, l1ss, prev_val = DW_PCIE_LTSSM_UNKNOWN, rate, val;

	if (!trace_pcie_ltssm_state_transition_enabled())
		/* [한국어] [한국어] 트레이스포인트가 꺼져 있으면 FIFO 를 읽지 않고 곧장 재예약으로 간다.
		 * 읽는 행위 자체가 FIFO 를 한 항목 소비하므로, 아무도 보지 않을 때 비우면
		 * 나중에 트레이스를 켜도 직전 이력이 사라진다. */
		goto skip_trace;

	for (i = 0; i < PCIE_DBG_LTSSM_HISTORY_CNT; i++) {
		/* [한국어] [한국어] FIFO 상태 레지스터를 읽는다. **읽을 때마다 한 항목이 소비된다.**
		 * 한 번의 읽기로 상태 코드, 속도, L1 하위상태를 모두 얻으므로 아래에서
		 * 같은 val 에 FIELD_GET 을 세 번 적용한다. */
		val = rockchip_pcie_readl_apb(rockchip,
				PCIE_CLIENT_DBG_FIFO_STATUS);
		rate = FIELD_GET(PCIE_DBG_FIFO_RATE_MASK, val);
		/* [한국어] [한국어] 같은 워드에서 L1 하위상태 표시를 뽑는다. 1 이면 L1.1, 2 면 L1.2. */
		l1ss = FIELD_GET(PCIE_DBG_FIFO_L1SUB_MASK, val);
		/* [한국어] [한국어] 마지막으로 상태 코드를 뽑아 val 을 덮어쓴다. rate 와 l1ss 를 먼저
		 * 뽑아 둔 뒤라야 원본 워드가 필요 없어진다 -- 순서가 바뀌면 안 된다. */
		val = FIELD_GET(PCIE_LTSSM_STATUS_MASK, val);

		/*
		 * Hardware Mechanism: The ring FIFO employs two tracking
		 * counters:
		 * - 'last-read-point': maintains the user's last read position
		 * - 'last-valid-point': tracks the HW's last state update
		 *
		 * Software Handling: When two consecutive LTSSM states are
		 * identical, it indicates invalid subsequent data in the FIFO.
		 * In this case, we skip the remaining entries. The dual counter
		 * design ensures that on the next state transition, reading can
		 * resume from the last user position.
		 */
		if ((i > 0 && val == prev_val) || val > DW_PCIE_LTSSM_RCVRY_EQ3)
			break;

		state = prev_val = val;
		/* [한국어] [한국어] L1_IDLE 로 보고된 항목만 하위상태 보정 대상이다. 다른 상태에서는
		 * l1ss 필드가 의미를 갖지 않는다. */
		if (val == DW_PCIE_LTSSM_L1_IDLE) {
			/* [한국어] [한국어] l1ss == 2 는 L1.2 를 뜻한다. */
			if (l1ss == 2)
				/* [한국어] [한국어] state 만 바꾸고 prev_val 은 원래 코드(L1_IDLE)를 유지한다.
				 * 다음 항목과의 동일성 비교는 하드웨어가 준 원본 코드로 해야 맞기 때문이다. */
				state = DW_PCIE_LTSSM_L1_2;
			/* [한국어] [한국어] l1ss == 1 은 L1.1. */
			else if (l1ss == 1)
				state = DW_PCIE_LTSSM_L1_1;
		}

		trace_pcie_ltssm_state_transition(dev_name(pci->dev),
				dw_pcie_ltssm_status_string(state),
				((rate + 1) > pci->max_link_speed) ?
				PCI_SPEED_UNKNOWN : PCIE_SPEED_2_5GT + rate);
	}

skip_trace:
	schedule_delayed_work(&rockchip->trace_work, msecs_to_jiffies(5000));
}

/* [한국어]
 * rockchip_pcie_ltssm_trace - LTSSM 전이 추적을 켜거나 끈다 (CONFIG_TRACING 판)
 *
 * @rockchip: 이 인스턴스.
 * @enable: true 면 디버그 FIFO 를 켜고 주기 워크를 시작, false 면 반대.
 * @return: 없음.
 *
 * 켤 때 하는 일:
 *  1. 패턴 히트(PTN_HIT) 와 전이 히트(TRN_HIT) 레지스터 네 개에
 *     PCIE_CLIENT_DBG_TRANSITION_DATA(0xffff0000)를 쓴다. 이 값은 쓰기
 *     마스크만 전부 세우고 값은 0 이라는 뜻이므로, 네 레지스터의 하위
 *     16비트를 모두 0 으로 초기화하는 효과다 -- 이전에 남은 필터 조건을
 *     지워 모든 전이를 FIFO 에 담게 만든다.
 *  2. FIFO 모드를 켠다(PCIE_CLIENT_DBG_EN = 0xffff0007: 마스크 전체 +
 *     하위 세 비트 활성).
 *  3. 워크를 초기화하고 지연 0 으로 즉시 예약해, 첫 수집이 바로 시작되게 한다.
 *
 * 끌 때는 FIFO 모드를 끄고(0xffff0000: 마스크 전체 + 값 0)
 * cancel_delayed_work_sync 로 진행 중인 워크가 끝날 때까지 기다린다. 이
 * 동기 대기가 있어야, 돌아온 뒤 클록이나 PHY 를 내려도 워크가 사라진
 * 레지스터를 읽지 않는다.
 *
 * INIT_DELAYED_WORK 를 probe 가 아니라 켤 때마다 부르는 점이 특이하다.
 * 앞선 cancel_delayed_work_sync 가 워크를 완전히 정지시킨 뒤이므로 재초기화가
 * 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥(취소가 잠들 수 있다).
 *
 * 호출 체인:
 *   rockchip_pcie_start_link / _stop_link → [이 함수]
 *     → rockchip_pcie_writel_apb → schedule_delayed_work
 */
static void rockchip_pcie_ltssm_trace(struct rockchip_pcie *rockchip,
				      bool enable)
{
	if (enable) {
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_TRANSITION_DATA,
					 PCIE_CLIENT_DBG_FIFO_PTN_HIT_D0);
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_TRANSITION_DATA,
					 PCIE_CLIENT_DBG_FIFO_PTN_HIT_D1);
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_TRANSITION_DATA,
					 PCIE_CLIENT_DBG_FIFO_TRN_HIT_D0);
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_TRANSITION_DATA,
					 PCIE_CLIENT_DBG_FIFO_TRN_HIT_D1);
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_EN,
					 PCIE_CLIENT_DBG_FIFO_MODE_CON);

		INIT_DELAYED_WORK(&rockchip->trace_work,
				  rockchip_pcie_ltssm_trace_work);
		schedule_delayed_work(&rockchip->trace_work, 0);
	/* [한국어] [한국어] enable 이 거짓 -- 추적을 끄는 경로. */
	} else {
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_DIS,
					 PCIE_CLIENT_DBG_FIFO_MODE_CON);
		cancel_delayed_work_sync(&rockchip->trace_work);
	}
}
#else
/* [한국어]
 * rockchip_pcie_ltssm_trace - 아무 일도 하지 않는 판 (CONFIG_TRACING 꺼짐)
 *
 * @rockchip: 쓰지 않는다.
 * @enable: 쓰지 않는다.
 * @return: 없음.
 *
 * 트레이스포인트가 빌드에 없으면 디버그 FIFO 를 켤 이유도, 주기 워크를 돌릴
 * 이유도 없다. 그렇다고 호출부에 #ifdef 를 흩뿌리면 start_link/stop_link 가
 * 읽기 어려워지므로, 같은 시그니처의 빈 함수를 두어 호출부는 조건 없이
 * 부르게 하고 컴파일러가 그 호출을 통째로 없애도록 맡긴다.
 *
 * 이 판이 쓰일 때는 struct rockchip_pcie 의 trace_work 필드도 아무도 건드리지
 * 않는다(필드 자체는 #ifdef 없이 항상 존재한다).
 *
 * 실행 컨텍스트: 호출부와 동일하나 실제로는 코드가 남지 않는다.
 *
 * 호출 체인:
 *   rockchip_pcie_start_link / _stop_link → [이 함수] (아무 동작 없음)
 */
static void rockchip_pcie_ltssm_trace(struct rockchip_pcie *rockchip,
				      bool enable)
{
}
#endif

/* [한국어]
 * rockchip_pcie_enable_ltssm - LTSSM(링크 학습 상태 기계)을 돌리기 시작한다
 *
 * @rockchip: 이 인스턴스.
 * @return: 없음.
 *
 * PCIE_CLIENT_ENABLE_LTSSM 은 FIELD_PREP_WM16(BIT(2), 1) 로 만들어진 값이라,
 * 일반 제어 레지스터의 비트 2 만 1 로 바꾸고 나머지(모드 선택 비트 등)는
 * 건드리지 않는다. 읽기-수정-쓰기가 필요 없는 이유가 여기 있다.
 *
 * 이 비트를 세우기 전까지 하드웨어는 링크 학습을 시작하지 않는다. 그래서
 * start_link 가 PERST# 타이밍을 맞추는 기준점으로 이 시점을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_start_link → [이 함수] → rockchip_pcie_writel_apb
 */
static void rockchip_pcie_enable_ltssm(struct rockchip_pcie *rockchip)
{
	rockchip_pcie_writel_apb(rockchip, PCIE_CLIENT_ENABLE_LTSSM,
				 PCIE_CLIENT_GENERAL_CON);
}

/* [한국어]
 * rockchip_pcie_disable_ltssm - LTSSM 을 멈춰 링크 학습을 중단시킨다
 *
 * @rockchip: 이 인스턴스.
 * @return: 없음.
 *
 * PCIE_CLIENT_DISABLE_LTSSM 은 FIELD_PREP_WM16(BIT(2), 0) -- 같은 비트를
 * 0 으로 쓴다. enable 과 대칭이며, 마찬가지로 다른 비트는 손대지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_stop_link → [이 함수] → rockchip_pcie_writel_apb
 */
static void rockchip_pcie_disable_ltssm(struct rockchip_pcie *rockchip)
{
	rockchip_pcie_writel_apb(rockchip, PCIE_CLIENT_DISABLE_LTSSM,
				 PCIE_CLIENT_GENERAL_CON);
}

/* [한국어]
 * rockchip_pcie_link_up - 링크가 실제로 올라왔는지 DWC 코어에 답한다
 *
 * @pci: DWC 코어 인스턴스.
 * @return: true 면 링크 업. false 면 아직 학습 중이거나 내려가 있다.
 *
 * LTSSM 상태 코드(하위 6비트)가 아니라 **별도의 링크 업 필드**(비트 16~17)를
 * 본다. 그 값이 PCIE_LINKUP(0x3)일 때만 참이다. 상태 코드로 판단하면 L0 에
 * 들어가기 직전의 과도 상태를 링크 업으로 오인할 수 있어, 하드웨어가 따로
 * 내주는 이 필드를 쓰는 편이 확실하다.
 *
 * DWC 코어의 dw_pcie_wait_for_link() 가 이 함수를 반복해 폴링하므로, 여기서
 * 잠들거나 오래 걸리는 일을 하면 안 된다. APB 읽기 한 번으로 끝나는 이유다.
 *
 * 실행 컨텍스트: 프로세스 문맥(링크 대기 폴링)과 스레드 IRQ(EP 모드의
 * 링크 변화 처리) 양쪽.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_link_up) 또는 ep_sys_irq_thread
 *     → [이 함수] → rockchip_pcie_get_ltssm_reg
 */
static bool rockchip_pcie_link_up(struct dw_pcie *pci)
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);
	u32 val = rockchip_pcie_get_ltssm_reg(rockchip);
/* [한국어] [한국어] 이 상수를 참조하는 곳이 없다는 사실은 코드 흐름과 무관하지만,
 * 아래 링크 업 판정이 상태 코드가 아니라 별도 필드를 쓴다는 점이 요점이다. */

	return FIELD_GET(PCIE_LINKUP_MASK, val) == PCIE_LINKUP;
}

/*
 * See e.g. section '11.6.6.4 L1 Substate' in the RK3588 TRM V1.0 for the steps
 * needed to support L1 substates. Currently, just enable L1 substates for RC
 * mode if CLKREQ# is properly connected and supports-clkreq is present in DT.
 * For EP mode, there are more things should be done to actually save power in
 * L1 substates, so disable L1 substates until there is proper support.
 */
/* [한국어]
 * rockchip_pcie_configure_l1ss - CLKREQ# 배선 여부에 따라 L1 하위상태를 켜거나 봉인한다
 *
 * @pci: DWC 코어 인스턴스.
 * @return: 없음.
 *
 * L1 하위상태(L1.1/L1.2)는 CLKREQ# 신호선이 있어야 동작한다. 이 신호로
 * 장치가 '클록을 다시 달라' 고 요청하기 때문이다. 보드에 그 배선이 없으면
 * 기능을 광고해서는 안 되고, 잘못 광고하면 링크가 L1.2 에서 깨어나지 못한다.
 *
 * 그래서 DT 의 "supports-clkreq" 속성을 판단 근거로 삼는다(resource_get 이
 * 읽어 rockchip->supports_clkreq 에 담아 둔다):
 *  - 있으면: CLKREQ 준비 완료를 하드웨어에 알리고 pci->l1ss_support 를
 *    세운다. 이 플래그를 DWC 코어가 읽어 L1 하위상태 능력을 광고한다.
 *  - 없으면: 상류 주석대로 CLKREQ# 를 무조건 assert 해 둔다(풀다운 + not
 *    ready). l1ss_support 를 세우지 않으므로 DWC 코어가 광고 자체를 막는다.
 *
 * 상류 주석이 RK3588 TRM V1.0 의 '11.6.6.4 L1 Substate' 절을 근거로 든다.
 * 또한 지금은 **RC 모드에서만** 켜며, EP 모드는 실제로 절전이 되려면 더 할
 * 일이 있어 지원 전까지 꺼 둔다고 밝힌다 -- 실제로 이 함수는 host_init 에서만
 * 불린다.
 *
 * 실행 컨텍스트: host_init 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_host_init → [이 함수] → rockchip_pcie_writel_apb
 */
static void rockchip_pcie_configure_l1ss(struct dw_pcie *pci)
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);

	/* Enable L1 substates if CLKREQ# is properly connected */
	if (rockchip->supports_clkreq) {
		rockchip_pcie_writel_apb(rockchip, PCIE_CLKREQ_READY,
					 /* [한국어] [한국어] CLKREQ# 를 풀다운으로 묶고 '준비 안 됨' 으로 표시한다. 두 값을
					  * OR 하는 것이 안전한 이유는 각자 다른 비트의 마스크와 값만 담고 있어
					  * 서로를 덮어쓰지 않기 때문이다. */
					 PCIE_CLIENT_POWER_CON);
		pci->l1ss_support = true;
		/* [한국어] [한국어] l1ss_support 를 세우지 않고 끝낸다. 그러면 DWC 코어가 L1 하위상태
		 * 능력을 광고하지 않으므로, 호스트가 그 상태로 들어가려 시도하지 않는다. */
		return;
	}

	/*
	 * Otherwise, assert CLKREQ# unconditionally.  Since
	 * pci->l1ss_support is not set, the DWC core will prevent L1
	 * Substates support from being advertised.
	 */
	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLKREQ_PULL_DOWN | PCIE_CLKREQ_NOT_READY,
				 PCIE_CLIENT_POWER_CON);
}

/* [한국어]
 * rockchip_pcie_enable_l0s - 링크 능력 레지스터에 L0s 지원을 직접 써 넣는다
 *
 * @pci: DWC 코어 인스턴스.
 * @return: 없음.
 *
 * L0s 는 링크가 잠깐 쉬는 얕은 절전 상태다. 이 IP 는 실제로 L0s 를 할 수
 * 있지만 Link Capabilities 레지스터의 ASPM 지원 필드가 그렇게 나오지 않아,
 * 소프트웨어가 그 비트를 직접 세워 준다.
 *
 * 순서가 중요하다:
 *  1. PCI_CAP_ID_EXP 능력 구조를 찾는다. 없으면 아무것도 하지 않는다 --
 *     cap 이 0 이면 오프셋 0 을 건드리게 되므로 이 검사가 꼭 필요하다.
 *  2. LNKCAP 을 읽어 PCI_EXP_LNKCAP_ASPM_L0S 를 얹는다.
 *  3. **dw_pcie_dbi_ro_wr_en() 으로 감싼다.** LNKCAP 은 규약상 읽기 전용이라
 *     DWC 의 전용 스위치로 쓰기를 잠깐 허용해야 값이 실제로 들어간다.
 *
 * RC 와 EP 양쪽에서 불린다 -- host_init 과 ep_init 이 각각 부른다.
 *
 * 실행 컨텍스트: 초기화 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_host_init / rockchip_pcie_ep_init → [이 함수]
 *     → dw_pcie_find_capability → dw_pcie_dbi_ro_wr_en → dw_pcie_writel_dbi
 */
static void rockchip_pcie_enable_l0s(struct dw_pcie *pci)
{
	u32 cap, lnkcap;

	/* Enable L0S capability for all SoCs */
	cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	if (cap) {
		/* [한국어] [한국어] 현재 Link Capabilities 값을 읽는다. */
		lnkcap = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);
		/* [한국어] [한국어] L0s 지원 비트를 얹는다. 이 IP 는 실제로 L0s 를 할 수 있는데
		 * 레지스터가 그렇게 나오지 않아 소프트웨어가 채워 준다. */
		lnkcap |= PCI_EXP_LNKCAP_ASPM_L0S;
		/* [한국어] [한국어] LNKCAP 은 규약상 읽기 전용이라, DWC 의 전용 스위치로 쓰기를
		 * 잠깐 허용해야 값이 실제로 들어간다. 이 감싸기가 없으면 아래 쓰기가 무시된다. */
		dw_pcie_dbi_ro_wr_en(pci);
		dw_pcie_writel_dbi(pci, cap + PCI_EXP_LNKCAP, lnkcap);
		/* [한국어] [한국어] 쓰기 허용을 다시 닫는다. 열어 둔 채로 두면 이후 실수로 읽기 전용
		 * 레지스터를 바꿀 수 있다. */
		dw_pcie_dbi_ro_wr_dis(pci);
	}
}

/* [한국어]
 * rockchip_pcie_start_link - PERST# 타이밍을 지키며 링크 학습을 시작한다
 *
 * @pci: DWC 코어 인스턴스.
 * @return: 항상 0. 이 경로에서 실패할 수 있는 동작이 없다.
 *
 * PCIe 카드 전기·기계 규약이 요구하는 순서를 지키는 것이 이 함수의 전부다.
 * 순서와 근거(상류 주석에 자세히 적혀 있다):
 *
 *  1. gpiod_set_value_cansleep(rst_gpio, 0) -- PERST# 를 assert 한다.
 *     GPIO 서술자 관례상 0 이 '리셋 걸림' 쪽이다(DT 의 active-low 설정에
 *     따라 실제 전기 레벨은 뒤집힐 수 있다).
 *  2. LTSSM 을 켠다. 이 시점이 기준점인 이유: refclk 이 RC 의 PHY 에서
 *     나오는 경우, 클록이 실제로 나가기 시작하는 때가 여기다.
 *  3. msleep(PCIE_T_PVPERL_MS) 로 기다린다. 규약(PCI Express Card
 *     Electromechanical Spec 1.1, 2.6.2 절 표 2-4)은 PERST# 를 놓기 전에
 *     refclk 이 100us 동안 안정돼 있어야 한다고 한다. 상류 주석이 밝히듯
 *     refclk 이 RC PHY 에서 오는지 외부 발진기에서 오는지 알 수 없고
 *     장치가 리셋을 마치는 데 얼마나 걸릴지도 모르므로, 100us 가 아니라
 *     훨씬 넉넉한 T_PVPERL 값을 쓴다. (PCIE_T_PVPERL_MS 의 정의는 이
 *     스파스 체크아웃에 없다.)
 *  4. 트레이스를 켠다. 링크 학습이 시작되기 직전이라야 첫 전이부터 잡힌다.
 *  5. PERST# 를 놓아 장치를 깨운다.
 *
 * msleep 을 쓰므로 잠들 수 있는 문맥에서만 불릴 수 있다. DWC 코어의
 * dw_pcie_host_init / resume 경로가 그 조건을 만족한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 반드시 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_start_link) → pci->ops->start_link → [이 함수]
 *     → gpiod_set_value_cansleep → rockchip_pcie_enable_ltssm
 *       → msleep → rockchip_pcie_ltssm_trace
 */
static int rockchip_pcie_start_link(struct dw_pcie *pci)
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);

	/* Reset device */
	gpiod_set_value_cansleep(rockchip->rst_gpio, 0);

	rockchip_pcie_enable_ltssm(rockchip);

	/*
	 * PCIe requires the refclk to be stable for 100µs prior to releasing
	 * PERST. See table 2-4 in section 2.6.2 AC Specifications of the PCI
	 * Express Card Electromechanical Specification, 1.1. However, we don't
	 * know if the refclk is coming from RC's PHY or external OSC. If it's
	 * from RC, so enabling LTSSM is the just right place to release #PERST.
	 * We need more extra time as before, rather than setting just
	 * 100us as we don't know how long should the device need to reset.
	 */
	msleep(PCIE_T_PVPERL_MS);

	/* [한국어] [한국어] PERST# 를 놓기 **직전**에 추적을 켠다. 링크 학습이 시작되기 전이라야
	 * 첫 전이부터 FIFO 에 담긴다. */
	rockchip_pcie_ltssm_trace(rockchip, true);

	/* [한국어] [한국어] PERST# 를 놓아(deassert) 장치를 깨운다. 위 msleep 으로 refclk 안정
	 * 시간을 벌어 둔 뒤라야 규약을 지킨 순서가 된다. */
	gpiod_set_value_cansleep(rockchip->rst_gpio, 1);

	return 0;
}

/* [한국어]
 * rockchip_pcie_stop_link - LTSSM 을 끄고 트레이스 워크를 정리한다
 *
 * @pci: DWC 코어 인스턴스.
 * @return: 없음.
 *
 * start_link 의 역순이되 PERST# 는 건드리지 않는다 -- 링크를 멈추는 것과
 * 장치를 리셋하는 것은 별개이고, 리셋 제어는 상위 경로의 몫이다.
 *
 * 트레이스 정리를 여기 두는 것이 중요하다. ltssm_trace(false) 안의
 * cancel_delayed_work_sync 가 진행 중인 워크가 끝날 때까지 기다리므로,
 * 이 함수가 돌아온 뒤에는 APB 레지스터를 읽는 주체가 남아 있지 않다.
 * 그래서 이후 클록/PHY 를 내려도 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥(워크 취소가 잠들 수 있다).
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_stop_link) → pci->ops->stop_link → [이 함수]
 *     → rockchip_pcie_disable_ltssm → rockchip_pcie_ltssm_trace(false)
 */
static void rockchip_pcie_stop_link(struct dw_pcie *pci)
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);

	rockchip_pcie_disable_ltssm(rockchip);
	rockchip_pcie_ltssm_trace(rockchip, false);
}

/* [한국어]
 * rockchip_pcie_host_init - RC 모드에서 DWC 코어가 되부르는 SoC 초기화 훅
 *
 * @pp: DWC 루트 포트. to_dw_pcie_from_pp 로 dw_pcie 를, 다시
 *      to_rockchip_pcie 로 이 드라이버 인스턴스를 되찾는다.
 * @return: 0 성공, 음수는 "legacy" IRQ 조회 실패값.
 *
 * dw_pcie_host_init() 이 브리지와 자원을 갖춘 뒤, 링크를 올리기 전에 부른다.
 * 그래서 여기서 하는 일은 전부 '링크가 서기 전에 끝나 있어야 하는 것' 이다.
 *
 *  1. DT 의 "legacy" 인터럽트를 찾는다. 실패하면 INTx 를 받을 길이 없으므로
 *     초기화를 중단한다.
 *  2. pci->dbi_base2 를 dbi_base + PCIE_TYPE0_HDR_DBI2_OFFSET 로 채운다.
 *     DBI2 는 BAR 마스크(크기)를 쓰는 두 번째 창이다 -- 같은 오프셋에 BAR
 *     주소와 BAR 마스크를 둘 다 둘 수 없어 IP 가 창을 하나 더 노출하는데,
 *     그 창이 이 SoC 에서는 DBI 로부터 0x100000 떨어져 있다. DWC 코어는 이
 *     값을 스스로 알 수 없으므로 글루가 알려 줘야 한다.
 *  3. INTx 도메인을 만든다.
 *  4. 부모 IRQ 에 연쇄 핸들러를 건다.
 *  5. L1 하위상태와 L0s 를 설정한다.
 *  6. DBI2 를 통해 BAR0/BAR1 마스크를 0 으로 만든다. 루트 포트는 BAR 를
 *     노출하지 않아야 하는데 기본값이 0 이 아닐 수 있어 명시적으로 지운다.
 *     이 쓰기가 2번에서 dbi_base2 를 채운 뒤라야 올바른 창에 닿는다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 3번의 실패는 dev_err 로 로그만
 * 남기고 **반환하지 않는다**. 그대로 4번으로 내려가 연쇄 핸들러를 걸므로,
 * 이후 INTx 가 들어오면 rockchip_pcie_intx_handler 가 NULL 도메인을
 * generic_handle_domain_irq() 에 넘기게 된다. 다른 실패(1번)는 즉시
 * 반환하는 것과 대조된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → pp->ops->init → [이 함수]
 *     → of_irq_get_byname → rockchip_pcie_init_irq_domain
 *       → irq_set_chained_handler_and_data → rockchip_pcie_configure_l1ss
 *         → rockchip_pcie_enable_l0s → dw_pcie_writel_dbi2
 */
static int rockchip_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);
	/* [한국어] [한국어] of_irq_get_byname 실패를 로그로 남길 장치. */
	struct device *dev = rockchip->pci.dev;
	/* [한국어] [한국어] irq 는 'legacy' 인터럽트 번호, ret 은 도메인 생성 결과. */
	int irq, ret;

	irq = of_irq_get_byname(dev->of_node, "legacy");
	/* [한국어] [한국어] INTx 를 받을 선이 없으면 이 초기화의 의미가 없으므로 중단한다.
	 * -EPROBE_DEFER 도 여기 포함되어 그대로 위로 전달된다. */
	if (irq < 0)
		/* [한국어] [한국어] DWC 코어가 이 값을 받아 host_init 전체를 실패로 처리한다. */
		return irq;

	pci->dbi_base2 = pci->dbi_base + PCIE_TYPE0_HDR_DBI2_OFFSET;

	ret = rockchip_pcie_init_irq_domain(rockchip);
	/* [한국어] [한국어] 도메인 생성 실패. */
	if (ret < 0)
		/* [한국어] [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): 로그만 남기고 **반환하지
		 * 않는다.** 그대로 아래 연쇄 핸들러 등록으로 내려가므로, 이후 INTx 가 들어오면
		 * intx_handler 가 NULL 도메인을 generic_handle_domain_irq 에 넘기게 된다. */
		dev_err(dev, "failed to init irq domain\n");

	irq_set_chained_handler_and_data(irq, rockchip_pcie_intx_handler,
					 /* [한국어] [한국어] 핸들러 데이터로 rockchip 을 심는다. 인터럽트 문맥의 핸들러가
					  * irq_desc_get_handler_data 로 이것을 되찾는다. */
					 rockchip);

	rockchip_pcie_configure_l1ss(pci);
	rockchip_pcie_enable_l0s(pci);

	/* Disable Root Ports BAR0 and BAR1 as they report bogus size */
	/* [한국어] [한국어] DBI2 창을 통해 BAR0 마스크를 0 으로 만든다. 루트 포트는 BAR 를
	 * 노출하지 않아야 하는데 기본값이 0 이 아닐 수 있어 명시적으로 지운다.
	 * 앞에서 pci->dbi_base2 를 채운 뒤라야 올바른 창에 닿는다. */
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_0, 0x0);
	/* [한국어] [한국어] BAR1 도 같은 이유로 지운다. 두 칸을 지우는 것은 64비트 BAR 로
	 * 합쳐질 수 있는 쌍이기 때문이다. */
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_1, 0x0);

	return 0;
}

static const struct dw_pcie_host_ops rockchip_pcie_host_ops = {
	/* [한국어] [한국어] DWC 호스트 코어가 되부를 훅은 init 하나뿐이다. 이 SoC 는 링크가
	 * 선 뒤에 추가로 할 일(post_init)도, 자체 MSI 구현(msi_init)도 없다. */
	.init = rockchip_pcie_host_init,
};

/*
 * ATS does not work on RK3588 when running in EP mode.
 *
 * After the host has enabled ATS on the EP side, it will send an IOTLB
 * invalidation request to the EP side. However, the RK3588 will never send
 * a completion back and eventually the host will print an IOTLB_INV_TIMEOUT
 * error, and the EP will not be operational. If we hide the ATS capability,
 * things work as expected.
 */
/* [한국어]
 * rockchip_pcie_ep_hide_broken_ats_cap_rk3588 - RK3588 EP 모드에서 고장난 ATS 능력을 감춘다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * 상류 주석이 증상을 그대로 적어 두었다: RK3588 을 EP 로 쓸 때 호스트가 ATS
 * (Address Translation Services)를 켜면, 호스트가 보내는 IOTLB 무효화 요청에
 * RK3588 이 **완료 응답을 영영 보내지 않는다.** 호스트는 결국
 * IOTLB_INV_TIMEOUT 오류를 찍고 이 엔드포인트는 못 쓰게 된다.
 *
 * 고칠 방법이 없으므로 능력 자체를 설정공간에서 지워, 호스트가 ATS 를 켤
 * 생각조차 하지 않게 만든다. dw_pcie_remove_ext_capability() 가 확장 능력
 * 사슬에서 해당 항목을 빼낸다.
 *
 * of_device_is_compatible 로 RK3588 인지 다시 확인하는 이유: 이 함수는
 * RK3568 을 포함한 모든 EP 초기화 경로에서 불리지만, 문제가 있는 것은
 * RK3588 뿐이다. 멀쩡한 SoC 에서 능력을 지우면 쓸 수 있는 기능을 잃는다.
 *
 * 실행 컨텍스트: EP 초기화 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_init → [이 함수] → dw_pcie_remove_ext_capability
 */
static void rockchip_pcie_ep_hide_broken_ats_cap_rk3588(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;
/* [한국어] [한국어] 이 아래부터는 엔드포인트 모드 전용 코드다. */

	/* Only hide the ATS capability for RK3588 running in EP mode. */
	if (!of_device_is_compatible(dev->of_node, "rockchip,rk3588-pcie-ep"))
		return;

	dw_pcie_remove_ext_capability(pci, PCI_EXT_CAP_ID_ATS);
}

/* [한국어]
 * rockchip_pcie_ep_init - EP 모드에서 DWC EP 코어가 되부르는 SoC 초기화 훅
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음. 실패를 알릴 방법이 없는 통지형 콜백이다.
 *
 * RC 쪽 host_init 에 대응하는 EP 쪽 훅이지만 하는 일이 훨씬 적다. EP 는
 * INTx 도메인도, BAR 마스크 정리도 필요 없고, L1 하위상태는 상류 주석대로
 * 아직 지원하지 않기 때문이다(configure_l1ss 를 부르지 않는다).
 *
 * 남는 것은 둘: L0s 를 광고하도록 링크 능력을 고치고, RK3588 이면 ATS 능력을
 * 감춘다.
 *
 * 함수 끝의 세미콜론은 상류 그대로다(빈 문장, 동작 영향 없음).
 *
 * 실행 컨텍스트: dw_pcie_ep_init_registers 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   DWC EP 코어 → ep->ops->init → [이 함수]
 *     → rockchip_pcie_enable_l0s → rockchip_pcie_ep_hide_broken_ats_cap_rk3588
 */
static void rockchip_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	rockchip_pcie_enable_l0s(pci);
	rockchip_pcie_ep_hide_broken_ats_cap_rk3588(ep);
};

/* [한국어]
 * rockchip_pcie_raise_irq - EP 가 호스트에게 인터럽트를 올린다 (종류별 분배)
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 어느 물리 함수가 보내는지.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX.
 * @interrupt_num: MSI/MSI-X 벡터 번호(1-기반). INTx 에서는 쓰이지 않는다.
 * @return: 각 DWC 헬퍼의 반환값. 알 수 없는 종류이면 오류를 찍고 0 을 돌려준다.
 *
 * 이 SoC 에는 인터럽트를 올리는 자체 회로가 없어, 세 경로 모두 DWC 코어의
 * 공용 구현으로 넘긴다. 그래서 이 함수는 사실상 분배기다 -- 그럼에도 EPC
 * 규약이 raise_irq 콜백을 요구하므로 존재해야 한다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): default 분기가 오류를 로그로만
 * 남기고 switch 를 빠져나가 **0(성공)** 을 반환한다. 상위인 pci_epc_raise_irq
 * 는 이 값을 성공으로 받아들이므로, 알 수 없는 종류를 요청한 EPF 는 인터럽트가
 * 나가지 않았다는 사실을 알 수 없다.
 *
 * 실행 컨텍스트: EPF 드라이버가 인터럽트를 요청하는 프로세스 문맥.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → epc_ops->raise_irq → [이 함수]
 *     → dw_pcie_ep_raise_intx_irq / _msi_irq / _msix_irq
 */
static int rockchip_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				   unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	/* [한국어] [한국어] 레거시 INTx. 벡터 번호가 필요 없다. */
	case PCI_IRQ_INTX:
		/* [한국어] [한국어] DWC 공용 구현으로 넘긴다. 이 SoC 에는 자체 인터럽트 발생 회로가 없다. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:
		/* [한국어] [한국어] MSI. interrupt_num 은 1-기반 벡터 번호다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	case PCI_IRQ_MSIX:
		/* [한국어] [한국어] MSI-X. 테이블 항목 번호로 메시지를 만든다. */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features rockchip_pcie_epc_features_rk3568 = {
	/* [한국어] [한국어] DWC 코어가 공통으로 제공하는 기능들(동적 인바운드 매핑,
	 * 부분 범위 매핑)을 한 번에 켠다. */
	DWC_EPC_COMMON_FEATURES,
	.linkup_notifier = true,
	.msi_capable = true,
	.msix_capable = true,
	.align = SZ_64K,
	.bar[BAR_0] = { .type = BAR_RESIZABLE, },
	.bar[BAR_1] = { .type = BAR_RESIZABLE, },
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	.bar[BAR_3] = { .type = BAR_RESIZABLE, },
	.bar[BAR_4] = { .type = BAR_RESIZABLE, },
	.bar[BAR_5] = { .type = BAR_RESIZABLE, },
};

static const struct pci_epc_bar_rsvd_region rk3588_bar4_rsvd[] = {
	/* [한국어] [한국어] BAR4 안에서 EPF 가 쓸 수 없는 구간의 서술. 배열이지만 항목이
	 * 하나뿐이다. */
	{
		/* DMA_CAP (BAR4: DMA Port Logic Structure) */
		.type = PCI_EPC_BAR_RSVD_DMA_CTRL_MMIO,
		.offset = 0x0,
		.size = 0x2000,
	},
};

/*
 * BAR4 on rk3588 exposes the ATU Port Logic Structure to the host regardless of
 * iATU settings for BAR4. This means that BAR4 cannot be used by an EPF driver,
 * so mark it as RESERVED.
 */
static const struct pci_epc_features rockchip_pcie_epc_features_rk3588 = {
	DWC_EPC_COMMON_FEATURES,
	.linkup_notifier = true,
	.msi_capable = true,
	.msix_capable = true,
	.align = SZ_64K,
	.bar[BAR_0] = { .type = BAR_RESIZABLE, },
	.bar[BAR_1] = { .type = BAR_RESIZABLE, },
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	.bar[BAR_3] = { .type = BAR_RESIZABLE, },
	.bar[BAR_4] = {
		.type = BAR_RESERVED,
		/* [한국어] [한국어] 예약 구간 개수를 배열 크기에서 얻는다. 배열을 늘려도 이 값이
		 * 따라오므로 어긋날 수 없다. */
		.nr_rsvd_regions = ARRAY_SIZE(rk3588_bar4_rsvd),
		.rsvd_regions = rk3588_bar4_rsvd,
	},
	.bar[BAR_5] = { .type = BAR_RESIZABLE, },
};

/* [한국어]
 * rockchip_pcie_get_features - 이 SoC 의 EPC 기능표를 EPC 코어에 알려 준다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: DT compatible 에 매인 pci_epc_features 포인터. 도달 가능한 NULL 이
 *          아니다 -- probe 가 of_device_get_match_data 결과를 NULL 검사한 뒤에만
 *          진행하고, 이 파일의 모든 of_data 항목이 epc_features 를 갖고 있다.
 *
 * EPC 코어는 이 표를 보고 무엇을 허용할지 정한다: MSI/MSI-X 가능 여부,
 * BAR 정렬 단위(SZ_64K), BAR 별 종류(가변/예약). 그래서 configfs 로 EPF 를
 * 붙일 때 잘못된 크기나 쓸 수 없는 BAR 를 미리 거를 수 있다.
 *
 * RK3568 과 RK3588 이 서로 다른 표를 쓰는 유일한 차이는 BAR4 다 -- RK3588 은
 * 그 자리에 내장 DMA 컨트롤러의 MMIO 가 얹혀 있어 EPF 가 쓸 수 없다.
 *
 * 실행 컨텍스트: EPC 코어가 묻는 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_epc_get_features → epc_ops->get_features → [이 함수]
 */
static const struct pci_epc_features *
rockchip_pcie_get_features(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);
/* [한국어] [한국어] 기능표는 SoC 마다 다르지만 고르는 방법은 같으므로, 표 자체를
 * of_data 에 담아 두고 여기서는 꺼내기만 한다. */

	return rockchip->data->epc_features;
}

static const struct dw_pcie_ep_ops rockchip_pcie_ep_ops = {
	/* [한국어] [한국어] EP 초기화 훅. */
	.init = rockchip_pcie_ep_init,
	/* [한국어] [한국어] 인터럽트 발생 훅. */
	.raise_irq = rockchip_pcie_raise_irq,
	.get_features = rockchip_pcie_get_features,
};

/* [한국어]
 * rockchip_pcie_clk_init - DT 가 나열한 클록 전부를 한꺼번에 얻어 켠다
 *
 * @rockchip: 이 인스턴스. 얻은 클록 배열과 개수를 여기 저장한다.
 * @return: 0 성공, 음수는 dev_err_probe 를 거친 실패값(-EPROBE_DEFER 포함).
 *
 * 이 SoC 의 PCIe 는 여러 클록(aclk, pclk, aux 등)을 필요로 하고 그 목록이
 * SoC 판본마다 다르다. 그래서 이름으로 하나씩 얻는 대신
 * devm_clk_bulk_get_all() 로 **DT 에 적힌 것을 전부** 가져온다 -- 드라이버가
 * 목록을 알 필요가 없어진다.
 *
 * 반환값이 개수라는 점이 특징이다. 그래서 음수 검사로 실패를 가리고, 성공
 * 시에는 그 값을 clk_cnt 에 담아 이후 enable/disable 에 쓴다.
 *
 * dev_err_probe 를 쓰는 이유: -EPROBE_DEFER 일 때는 로그를 남기지 않고
 * 조용히 물러나야 부팅 로그가 재시도 메시지로 덮이지 않는다.
 *
 * 실패 시 되감기: devm_ 계열이라 clk 핸들 자체는 자동 해제된다. 다만
 * prepare_enable 이 실패하면 이미 켜진 것은 벌크 API 가 되돌려 준다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe → [이 함수] → devm_clk_bulk_get_all
 *     → clk_bulk_prepare_enable
 */
static int rockchip_pcie_clk_init(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->pci.dev;
	int ret;
/* [한국어] [한국어] devm_clk_bulk_get_all 은 **얻은 개수** 를 돌려준다. 그래서 성공/실패
 * 판정을 0 이 아니라 음수로 한다. */

	ret = devm_clk_bulk_get_all(dev, &rockchip->clks);
	/* [한국어] [한국어] 클록 조회 실패. -EPROBE_DEFER 인 경우가 흔하다(클록 공급자가 아직
	 * 프로브되지 않음). */
	if (ret < 0)
		/* [한국어] [한국어] dev_err_probe 는 -EPROBE_DEFER 일 때 로그를 남기지 않아,
		 * 재시도 메시지로 부팅 로그가 덮이지 않는다. */
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	rockchip->clk_cnt = ret;
/* [한국어] [한국어] 얻은 개수를 보관한다. 이후 enable/disable 이 이 값을 쓴다. */

	ret = clk_bulk_prepare_enable(rockchip->clk_cnt, rockchip->clks);
	/* [한국어] [한국어] 클록 인가 실패. */
	if (ret)
		/* [한국어] [한국어] 벌크 API 가 중간 실패 시 이미 켠 것을 스스로 되돌리므로,
		 * 여기서 추가로 정리할 것이 없다. */
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	return 0;
}

/* [한국어]
 * rockchip_pcie_resource_get - APB 창, PERST# GPIO, 리셋 라인, CLKREQ 속성을 확보한다
 *
 * @pdev: 플랫폼 디바이스. DT 자원의 출처다.
 * @rockchip: 채워 넣을 인스턴스.
 * @return: 0 성공, 음수는 dev_err_probe 를 거친 실패값.
 *
 * probe 가 가장 먼저 부르는 자원 수집 함수다. 네 가지를 챙긴다:
 *
 *  1. "apb" 메모리 자원 -- 이 드라이버가 직접 다루는 SoC 제어 레지스터 창.
 *     없으면 아무것도 할 수 없으므로 실패로 끝난다. (DWC 코어가 쓰는
 *     "dbi"/"config" 는 여기서 다루지 않는다 -- dw_pcie_get_resources 의 몫이다.)
 *  2. "reset" GPIO -- PERST# 선. **optional 이다.** 보드에 따라 PERST# 가
 *     전원 회로에 묶여 있어 소프트웨어 제어가 없을 수 있다. 없으면 NULL 이
 *     들어오고, gpiod_set_value_cansleep(NULL, ...) 은 아무 일도 하지 않으므로
 *     start_link 가 그대로 동작한다.
 *     GPIOD_OUT_LOW 로 얻는 것은 처음부터 PERST# 를 assert 한 상태로 두기
 *     위해서다 -- 리셋을 건 채로 나머지 초기화를 진행한다.
 *  3. 리셋 라인 -- array_get_exclusive 로 DT 에 적힌 리셋을 전부 묶어 얻는다.
 *     클록과 같은 이유로 개수를 드라이버가 몰라도 된다.
 *  4. "supports-clkreq" 불리언 속성 -- L1 하위상태를 켤지 정하는 근거로
 *     configure_l1ss 가 읽는다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe → [이 함수] → devm_platform_ioremap_resource_byname
 *     → devm_gpiod_get_optional → devm_reset_control_array_get_exclusive
 *       → of_property_read_bool
 */
static int rockchip_pcie_resource_get(struct platform_device *pdev,
				      struct rockchip_pcie *rockchip)
{
	rockchip->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");
	if (IS_ERR(rockchip->apb_base))
		/* [한국어] [한국어] APB 창이 없으면 이 드라이버가 다룰 레지스터가 없다. */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->apb_base),
				     /* [한국어] [한국어] DT 의 reg-names 에 'apb' 항목이 있어야 한다는 뜻이다. */
				     "failed to map apb registers\n");

	rockchip->rst_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
						     /* [한국어] [한국어] GPIOD_OUT_LOW 로 얻어 **처음부터 PERST# 를 assert 한 상태**로 둔다.
						      * 리셋을 건 채로 나머지 초기화를 진행하고, start_link 가 마지막에 놓아 준다. */
						     GPIOD_OUT_LOW);
	if (IS_ERR(rockchip->rst_gpio))
		/* [한국어] [한국어] optional 이므로 '없음' 은 NULL 이고 오류가 아니다. 여기 걸리는 것은
		 * 진짜 오류(잘못된 DT 표기 등)뿐이다. */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->rst_gpio),
				     /* [한국어] [한국어] GPIO 표기가 잘못됐다는 뜻이므로 명시적으로 알린다. */
				     "failed to get reset gpio\n");

	rockchip->rst = devm_reset_control_array_get_exclusive(&pdev->dev);
	/* [한국어] [한국어] 리셋 라인 조회 실패. */
	if (IS_ERR(rockchip->rst))
		/* [한국어] [한국어] exclusive 로 얻으므로, 다른 드라이버가 같은 리셋을 이미 쥐고 있으면
		 * 여기서 실패한다. */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->rst),
				     /* [한국어] [한국어] DT 의 resets 속성을 확인해야 한다는 뜻이다. */
				     "failed to get reset lines\n");

	rockchip->supports_clkreq = of_property_read_bool(pdev->dev.of_node,
							  /* [한국어] [한국어] 이 속성이 있으면 보드에 CLKREQ# 배선이 있다는 뜻이고, 그때만
							   * L1 하위상태를 켤 수 있다(configure_l1ss 참조). */
							  "supports-clkreq");

	return 0;
}

/* [한국어]
 * rockchip_pcie_phy_init - PCIe PHY 를 얻어 초기화하고 전원을 넣는다
 *
 * @rockchip: 이 인스턴스.
 * @return: 0 성공, 음수는 실패값.
 *
 * PHY 는 링크의 물리 계층(직렬화, 이퀄라이저, 클록 복원)을 담당한다. LTSSM
 * 을 켜기 전에 반드시 살아 있어야 하므로 probe 의 이른 단계에서 처리한다.
 *
 * phy_init 과 phy_power_on 이 나뉜 이유는 PHY 프레임워크의 규약이다 --
 * init 은 레지스터 설정, power_on 은 전원/클록 인가로, 서스펜드 경로에서
 * 후자만 껐다 켜는 일이 있기 때문이다.
 *
 * 에러 되감기: power_on 이 실패하면 방금 성공한 phy_init 을 phy_exit 로
 * 되돌린다. 다만 그 뒤 `return ret` 이 실행되어 실패값이 그대로 올라간다 --
 * phy_exit 의 성패는 보지 않는다(되돌리기 실패에 대응할 방법이 없다).
 * devm_phy_get 은 devm 이라 핸들 자체는 자동 해제된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe → [이 함수] → devm_phy_get → phy_init → phy_power_on
 */
static int rockchip_pcie_phy_init(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->pci.dev;
	int ret;
/* [한국어] [한국어] PHY 는 optional 이 아니다 -- 없으면 링크를 세울 수 없다. */

	rockchip->phy = devm_phy_get(dev, "pcie-phy");
	/* [한국어] [한국어] PHY 조회 실패. 클록과 마찬가지로 -EPROBE_DEFER 가 흔하다. */
	if (IS_ERR(rockchip->phy))
		/* [한국어] [한국어] dev_err_probe 로 defer 시 조용히 물러난다. */
		return dev_err_probe(dev, PTR_ERR(rockchip->phy),
				     "missing PHY\n");

	ret = phy_init(rockchip->phy);
	/* [한국어] [한국어] phy_init 실패. 레지스터 설정 단계의 실패다. */
	if (ret < 0)
		/* [한국어] [한국어] 아직 잡은 것이 없으므로(devm_phy_get 은 자동 해제) 바로 반환한다. */
		return ret;

	ret = phy_power_on(rockchip->phy);
	/* [한국어] [한국어] phy_power_on 실패. */
	if (ret)
		/* [한국어] [한국어] 방금 성공한 phy_init 을 되돌린다. 그 뒤 ret(실패값)이 그대로
		 * 반환되므로, phy_exit 의 성패는 보지 않는다 -- 되돌리기 실패에 대응할
		 * 방법이 없기 때문이다. */
		phy_exit(rockchip->phy);

	return ret;
}

/* [한국어]
 * rockchip_pcie_phy_deinit - PHY 전원을 내리고 초기화를 되돌린다
 *
 * @rockchip: 이 인스턴스.
 * @return: 없음. 되감기 경로라 실패를 전할 곳이 없다.
 *
 * phy_init 의 역순이다: power_off 먼저, exit 나중. 반대로 하면 아직 전원이
 * 들어간 상태에서 설정을 지우게 된다.
 *
 * probe 의 deinit_phy 라벨에서만 불린다 -- 프로브가 성공하면 PHY 는 계속
 * 켜져 있고, 이 드라이버에는 remove 콜백이 없다.
 *
 * 실행 컨텍스트: 프로브 실패 되감기의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe(에러 경로) → [이 함수] → phy_power_off → phy_exit
 */
static void rockchip_pcie_phy_deinit(struct rockchip_pcie *rockchip)
{
	phy_power_off(rockchip->phy);
	phy_exit(rockchip->phy);
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] [한국어] DWC 코어가 링크 상태를 물을 때 부를 콜백. */
	.link_up = rockchip_pcie_link_up,
	/* [한국어] [한국어] 링크 기동/정지 콜백. 이 두 개가 있어야 DWC 코어가 링크를 직접
	 * 올리고 내릴 수 있다. */
	.start_link = rockchip_pcie_start_link,
	.stop_link = rockchip_pcie_stop_link,
	.get_ltssm = rockchip_pcie_get_ltssm,
};

/* [한국어]
 * rockchip_pcie_ep_sys_irq_thread - EP 모드의 링크 상태 변화와 핫리셋을 처리한다
 *
 * @irq: 발생한 IRQ 번호. 쓰지 않는다.
 * @arg: devm_request_threaded_irq 에 넘긴 rockchip 인스턴스.
 * @return: 항상 IRQ_HANDLED. 이 선은 이 드라이버 전용이라 공유 판정이 필요 없다.
 *
 * EP 는 호스트가 링크를 올리고 내리는 쪽이므로, 그 변화를 인터럽트로 받아야
 * DWC EP 코어와 그 위의 EPF 드라이버에 알릴 수 있다. DT 의 "sys" 인터럽트가
 * 그 통로다.
 *
 * 첫 두 줄이 중요하다: 상태 레지스터를 읽고 **읽은 값을 그대로 되쓴다.**
 * 이 레지스터는 write-1-to-clear 라, 방금 본 비트만 정확히 지우는 관용구다.
 * 이렇게 하면 읽은 뒤 되쓰기 전에 새로 선 비트를 실수로 지우지 않는다.
 *
 * 두 사건을 처리한다:
 *  - PCIE_LINK_REQ_RST_NOT_INT: 핫리셋 또는 링크 다운 리셋. EP 코어에
 *    dw_pcie_ep_linkdown 으로 알려 EPF 가 상태를 접게 한 뒤,
 *    PCIE_LTSSM_APP_DLY2_DONE 을 세워 '애플리케이션 쪽 처리가 끝났다' 고
 *    하드웨어에 통보한다. configure_ep 가 켜 둔 APP_DLY2_EN 과 짝을 이루는
 *    악수(handshake)로, 이 통보가 있어야 LTSSM 이 다음 단계로 진행한다.
 *  - PCIE_RDLH_LINK_UP_CHGED: 링크 업 상태가 바뀌었다. 실제로 올라온
 *    경우에만 dw_pcie_ep_linkup 을 부른다 -- 이 비트는 오르내림 양쪽에서
 *    서므로 방향을 다시 확인해야 한다.
 *
 * 스레드 IRQ 인 이유: dw_pcie_ep_linkup/linkdown 이 EPF 콜백을 부르고 그
 * 안에서 잠들 수 있다. IRQF_ONESHOT 으로 상위 핸들러가 없는 스레드 전용
 * 요청임을 표시한다.
 *
 * 실행 컨텍스트: IRQ 스레드(프로세스 문맥). 잠들 수 있다.
 *
 * 호출 체인:
 *   GIC → IRQ 스레드 → [이 함수] → dw_pcie_ep_linkdown / dw_pcie_ep_linkup
 */
static irqreturn_t rockchip_pcie_ep_sys_irq_thread(int irq, void *arg)
{
	struct rockchip_pcie *rockchip = arg;
	struct dw_pcie *pci = &rockchip->pci;
	/* [한국어] [한국어] 디버그 로그의 주체. */
	struct device *dev = pci->dev;
	/* [한국어] [한국어] reg 는 읽은 상태 비트들, val 은 되쓸 악수 값. */
	u32 reg, val;

	reg = rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_INTR_STATUS_MISC);
	/* [한국어] [한국어] **읽은 값을 그대로 되쓴다.** 이 레지스터는 write-1-to-clear 라,
	 * 방금 본 비트만 정확히 지우는 관용구다. 이렇게 하면 읽은 뒤 되쓰기 전에
	 * 새로 선 비트를 실수로 지우지 않는다. */
	rockchip_pcie_writel_apb(rockchip, reg, PCIE_CLIENT_INTR_STATUS_MISC);

	dev_dbg(dev, "PCIE_CLIENT_INTR_STATUS_MISC: %#x\n", reg);
	/* [한국어] [한국어] LTSSM 레지스터를 통째로 찍어, 어느 상태에서 이 사건이 왔는지
	 * 디버깅에 남긴다. */
	dev_dbg(dev, "LTSSM_STATUS: %#x\n", rockchip_pcie_get_ltssm_reg(rockchip));

	if (reg & PCIE_LINK_REQ_RST_NOT_INT) {
		/* [한국어] [한국어] 호스트가 링크 리셋을 요청했다. 핫리셋과 링크 다운 리셋을
		 * 구별하지 않고 같은 경로로 처리한다. */
		dev_dbg(dev, "hot reset or link-down reset\n");
		/* [한국어] [한국어] EP 코어에 알려 EPF 드라이버들이 상태를 접게 한다. 이 호출이
		 * 잠들 수 있어 이 핸들러가 스레드 IRQ 여야 한다. */
		dw_pcie_ep_linkdown(&pci->ep);
		/* Stop delaying link training. */
		val = FIELD_PREP_WM16(PCIE_LTSSM_APP_DLY2_DONE, 1);
		rockchip_pcie_writel_apb(rockchip, val,
					 /* [한국어] [한국어] APP_DLY2_DONE 을 세워 '애플리케이션 쪽 정리가 끝났다' 고 알린다.
					  * configure_ep 가 켜 둔 APP_DLY2_EN 과 짝을 이루는 악수로, 이 통보가 있어야
					  * LTSSM 이 리셋 처리를 다음 단계로 진행한다. 위 linkdown 통지 **뒤에** 두는
					  * 순서가 핵심이다. */
					 PCIE_CLIENT_HOT_RESET_CTRL);
	}

	if (reg & PCIE_RDLH_LINK_UP_CHGED) {
		/* [한국어] [한국어] 이 비트는 링크가 오를 때와 내릴 때 모두 서므로, 실제로 올라왔는지
		 * 다시 확인해야 한다. 내려간 경우는 위의 리셋 비트 경로가 처리한다. */
		if (rockchip_pcie_link_up(pci)) {
			/* [한국어] [한국어] 링크 업을 로그로 남긴다. */
			dev_dbg(dev, "link up\n");
			/* [한국어] [한국어] EP 코어에 알려 EPF 드라이버가 동작을 시작하게 한다. */
			dw_pcie_ep_linkup(&pci->ep);
		}
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * rockchip_pcie_configure_rc - 루트 컴플렉스 모드로 하드웨어를 세우고 DWC 호스트 코어에 넘긴다
 *
 * @rockchip: 이 인스턴스.
 * @return: 0 성공, -ENODEV 는 RC 지원이 빌드에서 빠짐,
 *          그 외 음수는 dw_pcie_host_init 의 실패값.
 *
 * IS_ENABLED 검사가 맨 앞인 이유: 이 파일 하나가 RC/EP 를 모두 담고 있어,
 * 한쪽 Kconfig 만 켠 커널에서도 컴파일은 된다. 그래서 실행 시점에 지원 여부를
 * 확인해 -ENODEV 로 물러난다. IS_ENABLED 는 상수라 컴파일러가 꺼진 쪽의
 * 나머지 코드를 통째로 없앨 수 있다는 이점도 있다.
 *
 * 하드웨어 설정은 두 번의 APB 쓰기뿐이다:
 *  1. PCIE_LTSSM_ENABLE_ENHANCE -- LTSSM 활성화 방식을 개선판으로 바꾼다.
 *     EP 경로도 같은 비트를 세우므로 모드 공통 설정이다.
 *  2. 모드 비트를 PCIE_CLIENT_MODE_RC(0x4)로 -- 이 한 번의 쓰기가 IP 를
 *     루트 컴플렉스로 만든다.
 *
 * 그 뒤 pp->ops 를 걸고 dw_pcie_host_init 을 부르면, 코어가 브리지를 만들고
 * 자원을 파악한 뒤 rockchip_pcie_host_init 을 되부르고, 링크를 올려 버스를
 * 열거한다. 즉 이 함수가 돌아올 때는 이미 모든 하위 장치가 붙어 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe → [이 함수] → rockchip_pcie_writel_apb
 *     → dw_pcie_host_init → (되돌아) rockchip_pcie_host_init
 */
static int rockchip_pcie_configure_rc(struct rockchip_pcie *rockchip)
{
	struct dw_pcie_rp *pp;
	u32 val;
/* [한국어] [한국어] 이 파일 하나가 RC/EP 를 모두 담고 있어, 한쪽 Kconfig 만 켠 커널에서도
 * 컴파일은 된다. 그래서 실행 시점에 지원 여부를 확인한다. */

	if (!IS_ENABLED(CONFIG_PCIE_ROCKCHIP_DW_HOST))
		/* [한국어] [한국어] RC 지원이 빌드에 없다. probe 가 이 값을 받아 실패로 끝낸다.
		 * IS_ENABLED 는 상수라, 꺼진 경우 컴파일러가 아래 코드를 통째로 없앨 수 있다. */
		return -ENODEV;

	/* LTSSM enable control mode */
	val = FIELD_PREP_WM16(PCIE_LTSSM_ENABLE_ENHANCE, 1);
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_HOT_RESET_CTRL);
/* [한국어] [한국어] LTSSM 활성 방식을 개선판으로 바꾼다. EP 경로도 같은 비트를 세우므로
 * 모드 공통 설정이다. */

	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLIENT_SET_MODE(PCIE_CLIENT_MODE_RC),
				 PCIE_CLIENT_GENERAL_CON);

	pp = &rockchip->pci.pp;
	/* [한국어] [한국어] 호스트 훅을 걸어 둔다. dw_pcie_host_init 이 자원을 갖춘 뒤
	 * rockchip_pcie_host_init 을 되부르는 통로다. */
	pp->ops = &rockchip_pcie_host_ops;

	return dw_pcie_host_init(pp);
}

/* [한국어]
 * rockchip_pcie_configure_ep - 엔드포인트 모드로 하드웨어를 세우고 DWC EP 코어에 넘긴다
 *
 * @pdev: 플랫폼 디바이스. "sys" 인터럽트를 여기서 얻는다.
 * @rockchip: 이 인스턴스.
 * @return: 0 성공, -ENODEV 는 EP 지원이 빌드에서 빠짐, 그 외 음수는 실패값.
 *
 * RC 쪽과 대칭이지만 순서에 하드웨어 제약이 얽혀 있다.
 *
 *  1. IS_ENABLED 로 EP 지원 확인.
 *  2. "sys" 인터럽트를 **먼저** 등록한다. 아래에서 EP 초기화를 하는 도중
 *     호스트가 링크를 올릴 수 있으므로, 그 전에 받을 준비가 되어 있어야 한다.
 *     스레드 IRQ + IRQF_ONESHOT 인 이유는 핸들러가 잠들 수 있기 때문이다.
 *  3. LTSSM_ENABLE_ENHANCE 와 함께 **APP_DLY2_EN** 을 켠다. 이것이 EP 전용
 *     설정으로, 핫리셋 때 LTSSM 이 소프트웨어의 처리를 기다리게 만든다.
 *     그 기다림을 풀어 주는 것이 ep_sys_irq_thread 의 APP_DLY2_DONE 쓰기다.
 *  4. 모드 비트를 PCIE_CLIENT_MODE_EP(0x0)로.
 *  5. ep.ops 와 page_size(SZ_64K)를 채운다. page_size 는 아웃바운드 창의
 *     최소 단위로, epc_features 의 align 과 같은 값이다.
 *  6. dma_set_mask_and_coherent(64비트) -- EP 가 시스템 메모리에 DMA 할 때
 *     64비트 주소를 쓸 수 있음을 알린다.
 *  7. dw_pcie_ep_init → dw_pcie_ep_init_registers 순으로 EP 코어를 세운다.
 *     후자가 실패하면 전자를 dw_pcie_ep_deinit 으로 되감는다.
 *  8. pci_epc_init_notify 로 EPC 코어에 준비 완료를 알린다. 이 시점에
 *     바인딩된 EPF 의 epc_init 콜백이 불린다.
 *  9. 마지막에 두 통지 인터럽트의 마스크를 **푼다**(FIELD_PREP_WM16(bit, 0)).
 *     8번까지 끝난 뒤에 여는 것이 핵심이다 -- 그 전에 열면 EPF 가 준비되기
 *     전에 링크 업 통지가 들어올 수 있다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 6번 dma_set_mask_and_coherent 의
 * 반환값을 검사하지 않는다. 실패하면 마스크가 기본값으로 남지만 초기화는
 * 그대로 진행된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   rockchip_pcie_probe → [이 함수] → devm_request_threaded_irq
 *     → rockchip_pcie_writel_apb → dw_pcie_ep_init
 *       → dw_pcie_ep_init_registers → pci_epc_init_notify
 */
static int rockchip_pcie_configure_ep(struct platform_device *pdev,
				      struct rockchip_pcie *rockchip)
{
	struct device *dev = &pdev->dev;
	int irq, ret;
	/* [한국어] [한국어] APB 에 쓸 '쓰기 마스크 + 값' 조합을 담는다. */
	u32 val;

	if (!IS_ENABLED(CONFIG_PCIE_ROCKCHIP_DW_EP))
		/* [한국어] [한국어] EP 지원이 빌드에 없다. */
		return -ENODEV;

	irq = platform_get_irq_byname(pdev, "sys");
	/* [한국어] [한국어] 'sys' 인터럽트가 없으면 링크 변화를 알 수 없어 EP 로 동작할 수 없다. */
	if (irq < 0)
		/* [한국어] [한국어] -EPROBE_DEFER 포함해 그대로 위로 전달한다. */
		return irq;

	ret = devm_request_threaded_irq(dev, irq, NULL,
					/* [한국어] [한국어] 상위 핸들러 없이 스레드만 등록한다(첫 인자 NULL). 핸들러가
					 * dw_pcie_ep_linkup/linkdown 을 부르며 잠들 수 있기 때문이다.
					 * IRQF_ONESHOT 이 그 형태를 명시한다.
					 * **EP 초기화보다 먼저** 등록하는 것이 중요하다 -- 초기화 도중 호스트가
					 * 링크를 올릴 수 있다. */
					rockchip_pcie_ep_sys_irq_thread,
					IRQF_ONESHOT, "pcie-sys-ep", rockchip);
	if (ret) {
		/* [한국어] [한국어] IRQ 등록 실패. */
		dev_err(dev, "failed to request PCIe sys IRQ\n");
		/* [한국어] [한국어] 아직 하드웨어를 EP 로 바꾸기 전이므로 되감을 것이 없다. */
		return ret;
	}

	/*
	 * LTSSM enable control mode, and automatically delay link training on
	 * hot reset/link-down reset.
	 */
	val = FIELD_PREP_WM16(PCIE_LTSSM_ENABLE_ENHANCE, 1) |
	      FIELD_PREP_WM16(PCIE_LTSSM_APP_DLY2_EN, 1);
	/* [한국어] [한국어] 두 비트를 한 번에 쓴다. 각자 다른 비트의 마스크와 값만 담고 있어
	 * OR 로 합쳐도 서로를 덮어쓰지 않는다. */
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_HOT_RESET_CTRL);

	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLIENT_SET_MODE(PCIE_CLIENT_MODE_EP),
				 PCIE_CLIENT_GENERAL_CON);

	rockchip->pci.ep.ops = &rockchip_pcie_ep_ops;
	/* [한국어] [한국어] 아웃바운드 창의 최소 단위. epc_features 의 align 과 같은 값이라야
	 * EPC 코어의 검증과 실제 하드웨어 동작이 어긋나지 않는다. */
	rockchip->pci.ep.page_size = SZ_64K;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	ret = dw_pcie_ep_init(&rockchip->pci.ep);
	/* [한국어] [한국어] EP 코어 초기화 실패. */
	if (ret) {
		/* [한국어] [한국어] 실패 원인은 EP 코어가 이미 로그로 남겼을 수 있지만, 어느 단계에서
		 * 실패했는지 구별하려고 여기서도 남긴다. */
		dev_err(dev, "failed to initialize endpoint\n");
		/* [한국어] [한국어] 이 시점에 되감을 것은 IRQ(devm 이라 자동)뿐이다. */
		return ret;
	}

	ret = dw_pcie_ep_init_registers(&rockchip->pci.ep);
	/* [한국어] [한국어] 레지스터 초기화 실패. */
	if (ret) {
		/* [한국어] [한국어] 앞 단계와 구별되는 메시지를 남긴다. */
		dev_err(dev, "failed to initialize DWC endpoint registers\n");
		/* [한국어] [한국어] 여기서는 되감을 것이 있다 -- 방금 성공한 dw_pcie_ep_init 을
		 * 명시적으로 되돌려야 EPC 등록이 남지 않는다. */
		dw_pcie_ep_deinit(&rockchip->pci.ep);
		return ret;
	}

	pci_epc_init_notify(rockchip->pci.ep.epc);

	/* unmask DLL up/down indicator and hot reset/link-down reset */
	val = FIELD_PREP_WM16(PCIE_RDLH_LINK_UP_CHGED, 0) |
	      FIELD_PREP_WM16(PCIE_LINK_REQ_RST_NOT_INT, 0);
	/* [한국어] [한국어] 마지막에 두 통지 인터럽트의 마스크를 **푼다**(값 0 = 허용).
	 * 위의 pci_epc_init_notify 까지 끝난 뒤에 여는 순서가 핵심이다 -- 그 전에
	 * 열면 EPF 가 준비되기 전에 링크 업 통지가 들어올 수 있다. */
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_INTR_MASK_MISC);

	return ret;
}

/* [한국어]
 * rockchip_pcie_probe - 드라이버 진입점. 전원 계통을 순서대로 올리고 모드로 분기한다
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수는 실패값. 실패 시 goto 라벨로 그때까지 잡은 것만 푼다.
 *
 * of_device_get_match_data 로 얻는 rockchip_pcie_of_data 가 이 드라이버의
 * 분기점이다 -- 같은 코드가 compatible 에 따라 RC 도 EP 도 된다.
 *
 * 초기화 순서가 하드웨어 의존성 그대로다:
 *  1. of_data 확인. 없으면 -EINVAL(있을 수 없는 상황이지만 방어).
 *  2. 인스턴스 할당(devm)과 platform_set_drvdata. 이 drvdata 가
 *     to_rockchip_pcie() 매크로의 근거이므로, 이후 어떤 콜백보다 먼저
 *     심어져 있어야 한다.
 *  3. pci.dev / pci.ops / data 를 채운다. pci.ops 를 여기서 걸어야 DWC
 *     코어가 링크 제어를 이 파일로 되돌릴 수 있다.
 *  4. n_fts[0]=n_fts[1]=255 -- FTS(Fast Training Sequence) 개수를 최대로
 *     둔다. 값이 클수록 L0s 에서 깨어날 때 더 많은 학습 시퀀스를 보내
 *     복귀가 확실해진다. 두 칸은 Gen1 과 Gen2 이상용이다.
 *  5. 자원 확보(APB, GPIO, 리셋, DT 속성).
 *  6. **리셋 assert.** PHY 와 클록을 만지기 전에 컨트롤러를 리셋 상태로
 *     묶어 둔다.
 *  7. vpcie3v3 레귤레이터 -- 슬롯 전원. **optional 이라 -ENODEV 는 통과**
 *     시킨다(보드에 별도 레귤레이터가 없는 경우). devm_..._get_enable_
 *     optional 이므로 해제도 자동이다.
 *  8. PHY 초기화. 리셋이 걸린 상태에서 해야 안전하다.
 *  9. **리셋 deassert.** 이제 컨트롤러가 깨어난다.
 * 10. 클록 인가.
 * 11. mode 에 따라 configure_rc / configure_ep 로 분기.
 *
 * 에러 되감기: deinit_clk → deinit_phy 사슬. 리셋은 되돌리지 않는데,
 * 실패한 컨트롤러는 리셋 상태로 두는 편이 안전하기 때문이다. 레귤레이터와
 * 인스턴스는 devm 이 처리한다.
 *
 * 이 드라이버에 remove 콜백이 없다는 점에 유의 -- 성공적으로 프로브된 뒤에는
 * 내려가지 않는다는 전제다.
 *
 * 실행 컨텍스트: 드라이버 프로브의 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → rockchip_pcie_resource_get
 *     → reset_control_assert → devm_regulator_get_enable_optional
 *       → rockchip_pcie_phy_init → reset_control_deassert
 *         → rockchip_pcie_clk_init → configure_rc / configure_ep
 */
static int rockchip_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_pcie *rockchip;
	/* [한국어] [한국어] compatible 에 매인 SoC 별 설정표를 받을 포인터. */
	const struct rockchip_pcie_of_data *data;
	/* [한국어] [한국어] 각 단계의 실패값을 담는 변수. */
	int ret;

	data = of_device_get_match_data(dev);
	/* [한국어] [한국어] of_match_table 로 매칭됐다면 data 가 있어야 한다. 없다면 표 자체가
	 * 잘못된 것이므로 방어적으로 걸러 낸다. */
	if (!data)
		/* [한국어] [한국어] 프로브를 진행할 근거가 없다는 뜻으로 -EINVAL. */
		return -EINVAL;

	rockchip = devm_kzalloc(dev, sizeof(*rockchip), GFP_KERNEL);
	/* [한국어] [한국어] 인스턴스 할당 실패. */
	if (!rockchip)
		/* [한국어] [한국어] devm 이라 이후 자동 해제되므로, 여기서만 실패를 보면 된다. */
		return -ENOMEM;

	platform_set_drvdata(pdev, rockchip);
/* [한국어] [한국어] **다른 어떤 초기화보다 먼저** 심는다. to_rockchip_pcie() 매크로가
 * dev_get_drvdata 이므로, 이 호출 전에 DWC 콜백이 불리면 NULL 을 얻는다. */

	rockchip->pci.dev = dev;
	/* [한국어] [한국어] 링크 제어 콜백 테이블을 건다. 이것이 있어야 DWC 코어가
	 * start_link/stop_link/link_up/get_ltssm 을 이 파일로 되돌릴 수 있다. */
	rockchip->pci.ops = &dw_pcie_ops;
	/* [한국어] [한국어] 기능표를 보관한다. get_features 가 EP 경로에서 이것을 읽는다. */
	rockchip->data = data;

	/* Default N_FTS value (210) is broken, override it to 255 */
	rockchip->pci.n_fts[0] = 255; /* Gen1 */
	rockchip->pci.n_fts[1] = 255; /* Gen2+ */
/* [한국어] [한국어] FTS(Fast Training Sequence) 개수를 최대로 둔다. 값이 클수록
 * L0s 에서 깨어날 때 더 많은 학습 시퀀스를 보내 복귀가 확실해진다.
 * 두 칸은 각각 Gen1 과 Gen2 이상용이다. */

	ret = rockchip_pcie_resource_get(pdev, rockchip);
	/* [한국어] [한국어] 자원 확보 실패. */
	if (ret)
		/* [한국어] [한국어] devm 자원만 잡혔으므로 되감을 것이 없다. */
		return ret;

	ret = reset_control_assert(rockchip->rst);
	/* [한국어] [한국어] 리셋 assert 실패. */
	if (ret)
		/* [한국어] [한국어] PHY 와 클록을 만지기 전이라 되감을 것이 없다. */
		return ret;

	/* DON'T MOVE ME: must be enable before PHY init */
	ret = devm_regulator_get_enable_optional(dev, "vpcie3v3");
	if (ret < 0 && ret != -ENODEV)
		/* [한국어] [한국어] 슬롯 전원 레귤레이터. **optional 이라 -ENODEV 는 통과시킨다** --
		 * 보드에 별도 레귤레이터가 없는 경우다. 그 외 오류만 여기 걸린다. */
		return dev_err_probe(dev, ret,
				     /* [한국어] [한국어] devm_..._get_enable_optional 이므로 해제도 자동이다. */
				     "failed to enable vpcie3v3 regulator\n");

	ret = rockchip_pcie_phy_init(rockchip);
	/* [한국어] [한국어] PHY 초기화 실패. */
	if (ret)
		/* [한국어] [한국어] PHY 없이는 링크를 세울 수 없으므로 여기서 끝낸다. */
		return dev_err_probe(dev, ret,
				     "failed to initialize the phy\n");

	ret = reset_control_deassert(rockchip->rst);
	/* [한국어] [한국어] 리셋 deassert 실패. 이제 되감을 것(PHY)이 생겼으므로 라벨로 간다. */
	if (ret)
		/* [한국어] [한국어] PHY 만 되돌린다. 클록은 아직 켜지 않았다. */
		goto deinit_phy;

	ret = rockchip_pcie_clk_init(rockchip);
	/* [한국어] [한국어] 클록 인가 실패. */
	if (ret)
		/* [한국어] [한국어] 마찬가지로 PHY 만 되돌린다. */
		goto deinit_phy;

	switch (data->mode) {
	/* [한국어] [한국어] 루트 컴플렉스로 동작할 compatible. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] [한국어] 이 호출이 돌아올 때는 이미 링크가 서고 버스 열거까지 끝나 있다. */
		ret = rockchip_pcie_configure_rc(rockchip);
		if (ret)
			/* [한국어] [한국어] 클록과 PHY 를 차례로 되돌린다. */
			goto deinit_clk;
		break;
	case DW_PCIE_EP_TYPE:
		/* [한국어] [한국어] 엔드포인트로 동작할 compatible. pdev 도 넘기는 이유는 'sys'
		 * 인터럽트를 그쪽에서 얻기 때문이다. */
		ret = rockchip_pcie_configure_ep(pdev, rockchip);
		if (ret)
			/* [한국어] [한국어] 같은 되감기 사슬을 탄다. */
			goto deinit_clk;
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", data->mode);
		ret = -EINVAL;
		goto deinit_clk;
	}

	return 0;

deinit_clk:
	clk_bulk_disable_unprepare(rockchip->clk_cnt, rockchip->clks);
deinit_phy:
	rockchip_pcie_phy_deinit(rockchip);

	return ret;
}

static const struct rockchip_pcie_of_data rockchip_pcie_rc_of_data_rk3568 = {
	/* [한국어] [한국어] RK3568 RC: epc_features 를 채우지 않는다(RC 는 EPC 기능표가 없다). */
	.mode = DW_PCIE_RC_TYPE,
};

static const struct rockchip_pcie_of_data rockchip_pcie_ep_of_data_rk3568 = {
	/* [한국어] [한국어] RK3568 EP. */
	.mode = DW_PCIE_EP_TYPE,
	/* [한국어] [한국어] BAR4 를 포함해 여섯 BAR 모두 가변 크기로 쓸 수 있는 표. */
	.epc_features = &rockchip_pcie_epc_features_rk3568,
};

static const struct rockchip_pcie_of_data rockchip_pcie_ep_of_data_rk3588 = {
	/* [한국어] [한국어] RK3588 EP. */
	.mode = DW_PCIE_EP_TYPE,
	/* [한국어] [한국어] BAR4 에 내장 DMA 컨트롤러 MMIO 가 얹혀 있어 예약으로 표시된 표. */
	.epc_features = &rockchip_pcie_epc_features_rk3588,
};

static const struct of_device_id rockchip_pcie_of_match[] = {
	/* [한국어] [한국어] compatible 문자열과 그에 매인 설정표의 짝. 이 표가 한 드라이버를
	 * 세 가지 역할로 갈라 준다. */
	{
		.compatible = "rockchip,rk3568-pcie",
		/* [한국어] [한국어] RK3568 을 RC 로 쓰는 노드. */
		.data = &rockchip_pcie_rc_of_data_rk3568,
	},
	{
		.compatible = "rockchip,rk3568-pcie-ep",
		/* [한국어] [한국어] RK3568 을 EP 로 쓰는 노드. */
		.data = &rockchip_pcie_ep_of_data_rk3568,
	},
	{
		.compatible = "rockchip,rk3588-pcie-ep",
		/* [한국어] [한국어] RK3588 을 EP 로 쓰는 노드. RK3588 의 RC 항목이 없는 것은,
		 * RK3588 RC 가 별도 드라이버로 다뤄지기 때문이다. */
		.data = &rockchip_pcie_ep_of_data_rk3588,
	},
	{},
};

static struct platform_driver rockchip_pcie_driver = {
	/* [한국어] [한국어] 플랫폼 드라이버 등록 정보. */
	.driver = {
		/* [한국어] [한국어] sysfs 에 나타날 드라이버 이름. */
		.name	= "rockchip-dw-pcie",
		/* [한국어] [한국어] 위 표를 걸어, DT 노드의 compatible 이 맞으면 probe 가 불린다. */
		.of_match_table = rockchip_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = rockchip_pcie_probe,
};
builtin_platform_driver(rockchip_pcie_driver);
