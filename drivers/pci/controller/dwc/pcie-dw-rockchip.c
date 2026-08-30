// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Rockchip SoCs.
 *
 * Copyright (C) 2021 Rockchip Electronics Co., Ltd.
 *		http://www.rock-chips.com
 *
 * Author: Simon Xue <xxm@rock-chips.com>
 */

#include <linux/bitfield.h>		/* NVMe: 비트 필드 조작용; PCIe 레지스터 파싱 시 링크 속도/상태 추출에 사용 */
#include <linux/clk.h>			/* NVMe: PCIe 클록 활성화; NVMe 장치 DMA/MSI 동작 전 클럭이 켜져 있어야 함 */
#include <linux/gpio/consumer.h>	/* NVMe: PERST# 리셋 GPIO 제어; NVMe SSD 초기화/열거 시 필수 */
#include <linux/hw_bitfield.h>		/* NVMe: 하드웨어 레지스터 쓰기 마스크용; PCIe 클라이언트 레지스터 업데이트 시 사용 */
#include <linux/irqchip/chained_irq.h>	/* NVMe: 레거시 INTx 체인 IRQ 핸들러; NVMe가 MSI/MSI-X 없을 때 대체 인터럽트 경로 */
#include <linux/irqdomain.h>		/* NVMe: INTx IRQ 도메인; NVMe 함수의 legacy INT#A~D를 Linux irq 번호로 매핑 */
#include <linux/mfd/syscon.h>		/* NVMe: (미사용) SoC 시스템 컨트롤러 접근 */
#include <linux/module.h>		/* NVMe: 모듈 매크로; 호스트/EP 드라이버 빌드/로드 */
#include <linux/of.h>			/* NVMe: DT 파싱; NVMe SSD가 연결된 PCIe 컨트롤러 노드 매칭 */
#include <linux/of_irq.h>		/* NVMe: DT 기반 IRQ 조회; legacy 인터럽트 라인 획득 */
#include <linux/phy/phy.h>		/* NVMe: PCIe PHY 초기화/전원; NVMe 장치와의 물리적 링크 설정 */
#include <linux/platform_device.h>	/* NVMe: 플랫폼 드라이버 등록; SoC 내장 PCIe 컨트롤러 바인딩 */
#include <linux/regmap.h>		/* NVMe: (간접) 레지스터 맵 추상; APB 레지스터 접근 */
#include <linux/reset.h>		/* NVMe: PCIe 컨트롤러 리셋 라인; NVMe 열거 전 컨트롤러 리셋/해제 */
#include <linux/workqueue.h>		/* NVMe: 지연 워크; LTSSM 추적 워크로 NVMe 링크 상태 변화 모니터링 */
#include <trace/events/pci_controller.h> /* NVMe: PCIe 컨트롤러 트레이스포인트; LTSSM 상태 변화 추적 */

#include "../../pci.h"			/* NVMe: PCIe 코어 헤더; PCI cfg space, MSI/MSI-X, ASPM 상수 정의 */
#include "pcie-designware.h"		/* NVMe: DesignWare PCIe 코어 헬퍼; dw_pcie_host_init, ATU, DBI 접근 */

/*
 * The upper 16 bits of PCIE_CLIENT_CONFIG are a write
 * mask for the lower 16 bits.
 */

#define to_rockchip_pcie(x)	dev_get_drvdata((x)->dev)	/* NVMe: dw_pcie 구조체에서 rockchip_pcie 드라이버 데이터 추출; NVMe 열거 시 컨트롤러 컨텍스트 획득 */

/* General Control Register */
#define PCIE_CLIENT_GENERAL_CON		0x0			/* NVMe: PCIe 클라이언트 일반 제어 레지스터 오프셋; RC/EP 모드 및 LTSSM 제어 */
#define  PCIE_CLIENT_MODE_MASK		GENMASK(7, 4)		/* NVMe: RC/EP 모드 선택 비트 마스크; NVMe 호스트는 RC 모드로 동작 */
#define  PCIE_CLIENT_MODE_EP		0x0UL			/* NVMe: EP 모드 값; NVMe 타겟(디바이스) 모드 설정 */
#define  PCIE_CLIENT_MODE_RC		0x4UL			/* NVMe: RC 모드 값; NVMe SSD를 열거할 루트 컴플렉스 모드 */
#define  PCIE_CLIENT_SET_MODE(x)	FIELD_PREP_WM16(PCIE_CLIENT_MODE_MASK, (x))	/* NVMe: write-mask 방식으로 RC/EP 모드 설정; NVMe 호스트에서는 RC로 설정 */
#define  PCIE_CLIENT_LD_RQ_RST_GRT	FIELD_PREP_WM16(BIT(3), 1)	/* NVMe: 링크 다운 시 리셋 요청 승인; NVMe 장치 재열거/핫플러그 복구 관련 */
#define  PCIE_CLIENT_ENABLE_LTSSM	FIELD_PREP_WM16(BIT(2), 1)	/* NVMe: LTSSM 활성화; NVMe SSD와의 PCIe 링크 트레이닝 시작 */
#define  PCIE_CLIENT_DISABLE_LTSSM	FIELD_PREP_WM16(BIT(2), 0)	/* NVMe: LTSSM 정지; NVMe 링크 종료 또는 재초기화 시 사용 */

/* Interrupt Status Register Related to Legacy Interrupt */
#define PCIE_CLIENT_INTR_STATUS_LEGACY	0x8			/* NVMe: 레거시 INTx 인터럽트 상태 레지스터; NVMe가 MSI/MSI-X 미지원 시 INT# 상태 읽기 */

/* Interrupt Status Register Related to Miscellaneous Operation */
#define PCIE_CLIENT_INTR_STATUS_MISC	0x10			/* NVMe: 기타 인터럽트 상태; 링크 업/다운, 핫 리셋 이벤트 */
#define  PCIE_RDLH_LINK_UP_CHGED	BIT(1)			/* NVMe: DLL 링크 업 상태 변경; NVMe SSD 연결/단절 감지 */
#define  PCIE_LINK_REQ_RST_NOT_INT	BIT(2)			/* NVMe: 핫 리셋/링크 다운 리셋 요청; NVMe 재열거 또는 제거 시 발생 */

/* Interrupt Mask Register Related to Legacy Interrupt */
#define PCIE_CLIENT_INTR_MASK_LEGACY	0x1c			/* NVMe: 레거시 INTx 마스크 레지스터; NVMe legacy 인터럽트 개별 마스크/언마스크 */
#define  PCIE_INTR_MASK			GENMASK(7, 0)		/* NVMe: INTx 입력 비트 마스크; INTA~INTD(4bit) 및 대응 마스크 비트 포함 */
#define  PCIE_INTR_CLAMP(_x)		((BIT((_x)) & PCIE_INTR_MASK))	/* NVMe: 특정 INTx 라인 비트 추출; NVMe INTA=0, INTB=1 등 */
#define  PCIE_INTR_LEGACY_MASK(x)	(PCIE_INTR_CLAMP((x)) | \
					 (PCIE_INTR_CLAMP((x)) << 16))	/* NVMe: INTx 마스크 값 생성; 상위 16bit가 하위 16bit write-mask 역할 */
#define  PCIE_INTR_LEGACY_UNMASK(x)	(PCIE_INTR_CLAMP((x)) << 16)	/* NVMe: INTx 언마스크 값 생성; write-mask만 설정해 해당 비트를 0(언마스크)으로 만듦 */

/* Interrupt Mask Register Related to Miscellaneous Operation */
#define PCIE_CLIENT_INTR_MASK_MISC	0x24			/* NVMe: 기타 인터럽트 마스크 레지스터; 링크/리셋 이벤트 마스크 제어 */

/* Power Management Control Register */
#define PCIE_CLIENT_POWER_CON		0x2c			/* NVMe: 전력 관리 제어; ASPM L1/L1sub, CLKREQ# 설정으로 NVMe 대기 전력 결정 */
#define  PCIE_CLKREQ_READY		FIELD_PREP_WM16(BIT(0), 1)	/* NVMe: CLKREQ# 신호 활성화; ASPM L1sub 지원 시 NVMe 링크 클록 절약 */
#define  PCIE_CLKREQ_NOT_READY		FIELD_PREP_WM16(BIT(0), 0)	/* NVMe: CLKREQ# 비활성화; L1sub 사용 불가 상태로 ASPM 제한 */
#define  PCIE_CLKREQ_PULL_DOWN		FIELD_PREP_WM16(GENMASK(13, 12), 1)	/* NVMe: CLKREQ# 풀다운; L1sub 미지원 SoC에서 클록 요구 강제 제거 */

/* RASDES TBA information */
#define PCIE_CLIENT_CDM_RASDES_TBA_INFO_CMN	0x154		/* NVMe: CDM RASDES 공통 정보; L1.1/L1.2 상태 감지용 */
#define  PCIE_CLIENT_CDM_RASDES_TBA_L1_1	BIT(4)		/* NVMe: L1.1 서브스테이트 표시; NVMe ASPM L1.1 진입 여부 확인 */
#define  PCIE_CLIENT_CDM_RASDES_TBA_L1_2	BIT(5)		/* NVMe: L1.2 서브스테이트 표시; NVMe ASPM L1.2 진입 여부 확인 */

/* Debug FIFO information */
#define PCIE_CLIENT_DBG_FIFO_MODE_CON	0x310			/* NVMe: 디버그 FIFO 모드 제어; LTSSM 상태 추적 활성화 */
#define  PCIE_CLIENT_DBG_EN		0xffff0007		/* NVMe: 디버그 FIFO 활성화 값; 링크 트레이닝 이력 캡처 */
#define  PCIE_CLIENT_DBG_DIS		0xffff0000		/* NVMe: 디버그 FIFO 비활성화 값 */
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D0	0x320			/* NVMe: 디버그 패턴 히트 D0; LTSSM 전이 트리거 설정 */
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D1	0x324			/* NVMe: 디버그 패턴 히트 D1 */
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D0	0x328			/* NVMe: 디버그 전이 히트 D0 */
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D1	0x32c			/* NVMe: 디버그 전이 히트 D1 */
#define  PCIE_CLIENT_DBG_TRANSITION_DATA 0xffff0000		/* NVMe: 디버그 전이 데이터 패턴; LTSSM 변화 캡처 조건 */
#define PCIE_CLIENT_DBG_FIFO_STATUS	0x350			/* NVMe: 디버그 FIFO 상태 읽기; 현재/이전 LTSSM 상태, 링크 속도, L1sub */
#define  PCIE_DBG_FIFO_RATE_MASK	GENMASK(22, 20)		/* NVMe: 현재 링크 속도 필드; NVMe Gen1/2/3/4/5 상태 추적 */
#define  PCIE_DBG_FIFO_L1SUB_MASK	GENMASK(10, 8)		/* NVMe: L1 서브스테이트 필드; NVMe 절전 상태 모니터링 */
#define PCIE_DBG_LTSSM_HISTORY_CNT	64			/* NVMe: LTSSM 이력 FIFO 최대 읽기 횟수; 과도한 루프 방지 */

/* Hot Reset Control Register */
#define PCIE_CLIENT_HOT_RESET_CTRL	0x180			/* NVMe: 핫 리셋 제어; NVMe 장치 재설정 시 LTSSM 지연 및 강화 모드 */
#define  PCIE_LTSSM_APP_DLY2_EN		BIT(1)			/* NVMe: LTSSM 지연2 활성화; 핫 리셋/링크다운 후 재트레이닝 지연 */
#define  PCIE_LTSSM_APP_DLY2_DONE	BIT(3)			/* NVMe: LTSSM 지연2 완료; 리셋 처리 후 지연 해제 */
#define  PCIE_LTSSM_ENABLE_ENHANCE	BIT(4)			/* NVMe: LTSSM 강화 모드 활성화; 링크 안정성 향상 */

/* LTSSM Status Register */
#define PCIE_CLIENT_LTSSM_STATUS	0x300			/* NVMe: LTSSM 상태 레지스터; NVMe와의 PCIe 링크 상태 읽기 */
#define  PCIE_LINKUP			0x3			/* NVMe: 링크 업(L0) 상태 값; NVMe SSD가 정상 연결됨을 의미 */
#define  PCIE_LINKUP_MASK		GENMASK(17, 16)		/* NVMe: 링크 업 상태 필드 마스크 */
#define  PCIE_LTSSM_STATUS_MASK		GENMASK(5, 0)		/* NVMe: LTSSM 상태 코드 필드; NVMe 링크 트레이닝 단계 파악 */

#define PCIE_TYPE0_HDR_DBI2_OFFSET      0x100000		/* NVMe: DBI2 영역 오프셋; RC BAR 등 Type0 헤더 쓰기용 */

struct rockchip_pcie {						/* NVMe: Rockchip DWC PCIe 컨트롤러 드라이버 사설 구조체 */
	struct dw_pcie pci;					/* NVMe: DesignWare PCIe 코어 구조체; NVMe 열거/ATU/MSI 처리 공용 객체 */
	void __iomem *apb_base;					/* NVMe: SoC PCIe 클라이언트(APB) 레지스터 베이스; PERST/LTSSM/IRQ 제어 */
	struct phy *phy;					/* NVMe: PCIe PHY 핸들; NVMe 링크 물리 계층 초기화/전원 */
	struct clk_bulk_data *clks;				/* NVMe: PCIe 관련 클럭 배열; NVMe 동작에 필요한 모든 클럭 */
	unsigned int clk_cnt;					/* NVMe: 활성화된 클럭 개수; 종료 시 해제 개수로 사용 */
	struct reset_control *rst;				/* NVMe: PCIe 컨트롤러 리셋 제어; probe/제거 시 사용 */
	struct gpio_desc *rst_gpio;				/* NVMe: PERST# GPIO; NVMe SSD 하드웨어 리셋 라인 */
	struct irq_domain *irq_domain;				/* NVMe: INTx IRQ 도메인; NVMe legacy 인터럽트 번호 매핑 */
	const struct rockchip_pcie_of_data *data;		/* NVMe: DT 매칭 데이터(RC/EP 모드, EPF 기능); */
	bool supports_clkreq;					/* NVMe: CLKREQ# 지원 여부; ASPM L1sub 사용 가능 결정 */
	struct delayed_work trace_work;				/* NVMe: LTSSM 추적 지연 워크; NVMe 링크 상태 변화 주기적 로깅 */
};

struct rockchip_pcie_of_data {					/* NVMe: DT compatible별 데이터 */
	enum dw_pcie_device_mode mode;				/* NVMe: RC 또는 EP 모드; NVMe 호스트는 RC */
	const struct pci_epc_features *epc_features;		/* NVMe: EP 모드 기능(미사용 for NVMe host); */
};

static int rockchip_pcie_readl_apb(struct rockchip_pcie *rockchip, u32 reg)	/* NVMe: APB 레지스터 읽기; NVMe 컨트롤러 상태/IRQ/LTSSM 조회 */
{
	return readl_relaxed(rockchip->apb_base + reg);				/* NVMe: memory-mapped APB 레지스터 리드; */
}

static void rockchip_pcie_writel_apb(struct rockchip_pcie *rockchip, u32 val,
					     u32 reg)				/* NVMe: APB 레지스터 쓰기; LTSSM/IRQ/PM 제어 값 기록 */
{
	writel_relaxed(val, rockchip->apb_base + reg);				/* NVMe: memory-mapped APB 레지스터 라이트; */
}

static void rockchip_pcie_intx_handler(struct irq_desc *desc)			/* NVMe: 레거시 INTx 체인 IRQ 핸들러; NVMe MSI/MSI-X 미지원 시 호출 */
{
	struct irq_chip *chip = irq_desc_get_chip(desc);			/* NVMe: 상위 IRQ chip 획득; chained_irq_enter/exit에서 사용 */
	struct rockchip_pcie *rockchip = irq_desc_get_handler_data(desc);	/* NVMe: 핸들러에 등록된 rockchip_pcie 컨텍스트 획득 */
	unsigned long reg, hwirq;						/* NVMe: reg=레거시 상태, hwirq=INTA~INTD 인덱스 */

	chained_irq_enter(chip, desc);						/* NVMe: 상위 IRQ chip 락/마스크 시작; INTx 처리 동안 중첩 인터럽트 방지 */

	reg = rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_INTR_STATUS_LEGACY);	/* NVMe: INTx 상태 레지스터 읽기; 어느 INT#가 active인지 파악 */

	for_each_set_bit(hwirq, &reg, 4)					/* NVMe: 4개 INTx 비트(INTA~INTD) 순회; NVMe legacy INT 활성 비트 찾기 */
		generic_handle_domain_irq(rockchip->irq_domain, hwirq);	/* NVMe: 해당 INTx 라인을 Linux IRQ 도메인을 통해 NVMe 드라이버로 전달 */

	chained_irq_exit(chip, desc);						/* NVMe: 상위 IRQ chip 락/마스크 해제; */
}

static void rockchip_intx_mask(struct irq_data *data)				/* NVMe: 특정 INTx 라인 마스크 콜백; NVMe legacy 인터럽트 일시 비활성화 */
{
	rockchip_pcie_writel_apb(irq_data_get_irq_chip_data(data),		/* NVMe: irq_data에서 rockchip_pcie 드라이버 데이터 획득 */
				 PCIE_INTR_LEGACY_MASK(data->hwirq),	/* NVMe: 해당 INT#(hwirq) 마스크 값 생성 */
				 PCIE_CLIENT_INTR_MASK_LEGACY);		/* NVMe: INTx 마스크 레지스터에 기록; 해당 NVMe 인터럽트 차단 */
};

static void rockchip_intx_unmask(struct irq_data *data)				/* NVMe: 특정 INTx 라인 언마스크 콜백; NVMe legacy 인터럽트 재활성화 */
{
	rockchip_pcie_writel_apb(irq_data_get_irq_chip_data(data),		/* NVMe: irq_data에서 rockchip_pcie 드라이버 데이터 획득 */
				 PCIE_INTR_LEGACY_UNMASK(data->hwirq),	/* NVMe: 해당 INT#(hwirq) 언마스크 값 생성 */
				 PCIE_CLIENT_INTR_MASK_LEGACY);		/* NVMe: INTx 마스크 레지스터에 기록; 해당 NVMe 인터럽트 허용 */
};

static struct irq_chip rockchip_intx_irq_chip = {				/* NVMe: INTx 라인용 irq_chip 정의; NVMe legacy 인터럽트 마스크/언마스크 연결 */
	.name			= "INTx",					/* NVMe: IRQ chip 이름 */
	.irq_mask		= rockchip_intx_mask,				/* NVMe: 마스크 콜백 등록 */
	.irq_unmask		= rockchip_intx_unmask,				/* NVMe: 언마스크 콜백 등록 */
	.flags			= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MASK_ON_SUSPEND,	/* NVMe: wake 설정 생략, suspend 시 마스크; NVMe 전력 관리 정책 반영 */
};

static int rockchip_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)			/* NVMe: IRQ 도메인 map 콜백; Linux irq 번호를 INT#A~D(hwirq)와 연결 */
{
	irq_set_chip_and_handler(irq, &rockchip_intx_irq_chip, handle_level_irq);	/* NVMe: 해당 irq에 rockchip INTx chip과 level 핸들러 설정; NVMe legacy INT는 level 트리거 */
	irq_set_chip_data(irq, domain->host_data);					/* NVMe: irq에 rockchip_pcie 포인터 저장; mask/unmask 시 사용 */

	return 0;									/* NVMe: 매핑 성공 */
}

static const struct irq_domain_ops intx_domain_ops = {				/* NVMe: INTx IRQ 도메인 연산 구조체 */
	.map = rockchip_pcie_intx_map,						/* NVMe: irq 매핑 함수 등록 */
};

static int rockchip_pcie_init_irq_domain(struct rockchip_pcie *rockchip)	/* NVMe: INTx IRQ 도메인 생성; NVMe legacy 인터럽트 지원 준비 */
{
	struct device *dev = rockchip->pci.dev;					/* NVMe: PCIe 컨트롤러 디바이스 포인터 */
	struct device_node *intc;						/* NVMe: DT legacy-interrupt-controller 노드 */

	intc = of_get_child_by_name(dev->of_node, "legacy-interrupt-controller");	/* NVMe: DT에서 legacy 인터럽트 컨트롤러 자식 노드 획득; NVMe INTA~D 매핑 출발점 */
	if (!intc) {									/* NVMe: 자식 노드가 없으면 */
		dev_err(dev, "missing child interrupt-controller node\n");		/* NVMe: 오류 로그; NVMe legacy IRQ를 설정할 수 없음 */
		return -EINVAL;								/* NVMe: 열거 실패 반환 */
	}

	rockchip->irq_domain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,	/* NVMe: INTx 4개(INTA~D)용 선형 IRQ 도메인 생성 */
							&intx_domain_ops, rockchip);	/* NVMe: map 콜백과 컨트롤러 컨텍스트 연결 */
	of_node_put(intc);								/* NVMe: DT 노드 참조 카운트 감소; 메모리 누수 방지 */
	if (!rockchip->irq_domain) {							/* NVMe: 도메인 생성 실패 시 */
		dev_err(dev, "failed to get a INTx IRQ domain\n");			/* NVMe: 오류 로그; NVMe legacy 인터럽트를 Linux에 등록하지 못함 */
		return -EINVAL;								/* NVMe: 실패 반환 */
	}

	return 0;									/* NVMe: IRQ 도메인 초기화 성공 */
}

static u32 rockchip_pcie_get_ltssm_reg(struct rockchip_pcie *rockchip)		/* NVMe: LTSSM 상태 레지스터 읽기; NVMe 링크 트레이닝 상태 확인 */
{
	return rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_LTSSM_STATUS);	/* NVMe: 0x300 레지스터 읽기; 링크 업/다운 및 LTSSM 코드 반환 */
}

static enum dw_pcie_ltssm rockchip_pcie_get_ltssm(struct dw_pcie *pci)		/* NVMe: DW 코어용 LTSSM 상태 획득 콜백; NVMe 링크/절전 상태 진단 */
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: dw_pcie에서 rockchip_pcie 컨텍스트 획득 */
	u32 val = rockchip_pcie_readl_apb(rockchip,
					PCIE_CLIENT_CDM_RASDES_TBA_INFO_CMN);	/* NVMe: RASDES TBA 레지스터 읽기; L1.1/L1.2 서브스테이트 여부 확인 */

	if (val & PCIE_CLIENT_CDM_RASDES_TBA_L1_1)				/* NVMe: L1.1 상태 비트가 설정되었으면 */
		return DW_PCIE_LTSSM_L1_1;					/* NVMe: NVMe 링크가 ASPM L1.1 절전 상태임을 보고 */

	if (val & PCIE_CLIENT_CDM_RASDES_TBA_L1_2)				/* NVMe: L1.2 상태 비트가 설정되었으면 */
		return DW_PCIE_LTSSM_L1_2;					/* NVMe: NVMe 링크가 ASPM L1.2 절전 상태임을 보고 */

	return rockchip_pcie_get_ltssm_reg(rockchip) & PCIE_LTSSM_STATUS_MASK;	/* NVMe: 일반 LTSSM 상태 코드 반환; NVMe 링크 트레이닝/데이터 상태 파악 */
}

#ifdef CONFIG_TRACING
static void rockchip_pcie_ltssm_trace_work(struct work_struct *work)		/* NVMe: LTSSM 추적 워크 함수; NVMe 링크 상태 이력을 트레이스포인트로 출력 */
{
	struct rockchip_pcie *rockchip = container_of(work,				/* NVMe: work_struct에서 rockchip_pcie 추출 */
						struct rockchip_pcie,
						trace_work.work);
	struct dw_pcie *pci = &rockchip->pci;					/* NVMe: dw_pcie 포인터 획득; 디바이스 이름/최대 링크 속도 참조 */
	enum dw_pcie_ltssm state;						/* NVMe: 변환된 LTSSM 상태 */
	u32 i, l1ss, prev_val = DW_PCIE_LTSSM_UNKNOWN, rate, val;		/* NVMe: i=루프, l1ss=L1sub 코드, prev_val=이전 상태, rate=링크 속도, val=원시 레지스터 값 */

	if (!trace_pcie_ltssm_state_transition_enabled())				/* NVMe: 트레이스포인트가 활성화되지 않았으면 */
		goto skip_trace;							/* NVMe: 상태 읽기 건추고 재스케줄링 */

	for (i = 0; i < PCIE_DBG_LTSSM_HISTORY_CNT; i++) {				/* NVMe: 최대 64개 LTSSM 이력 항목 순회; NVMe 링크 변화 이력 분석 */
		val = rockchip_pcie_readl_apb(rockchip,
					PCIE_CLIENT_DBG_FIFO_STATUS);			/* NVMe: 디버그 FIFO 상태 레지스터 읽기; 현재/이전 LTSSM 정보 포함 */
		rate = FIELD_GET(PCIE_DBG_FIFO_RATE_MASK, val);				/* NVMe: 상위 비트에서 현재 링크 속도 추출; NVMe Gen 속도 확인 */
		l1ss = FIELD_GET(PCIE_DBG_FIFO_L1SUB_MASK, val);			/* NVMe: L1sub 상태 코드 추출; NVMe 절전 상태 확인 */
		val = FIELD_GET(PCIE_LTSSM_STATUS_MASK, val);				/* NVMe: 하위 비트에서 LTSSM 상태 코드 추출 */

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
		if ((i > 0 && val == prev_val) || val > DW_PCIE_LTSSM_RCVRY_EQ3)	/* NVMe: 연속 동일 상태이거나 유효 범위를 벗어나면 */
			break;								/* NVMe: 더 이상 유효한 NVMe 링크 이력이 없음 */

		state = prev_val = val;							/* NVMe: 현재 상태 저장 및 변환 대상 설정 */
		if (val == DW_PCIE_LTSSM_L1_IDLE) {					/* NVMe: L1 idle 상태일 때 L1sub 세분화 */
			if (l1ss == 2)							/* NVMe: L1.2 서브스테이트 코드이면 */
				state = DW_PCIE_LTSSM_L1_2;				/* NVMe: NVMe 링크가 L1.2 절전 중임 */
			else if (l1ss == 1)						/* NVMe: L1.1 서브스테이트 코드이면 */
				state = DW_PCIE_LTSSM_L1_1;				/* NVMe: NVMe 링크가 L1.1 절전 중임 */
		}

		trace_pcie_ltssm_state_transition(dev_name(pci->dev),			/* NVMe: PCIe 컨트롤러 이름으로 트레이스 */
						dw_pcie_ltssm_status_string(state),	/* NVMe: 상태 문자열; NVMe 진단 도구에 표시 */
						((rate + 1) > pci->max_link_speed) ?
						PCI_SPEED_UNKNOWN : PCIE_SPEED_2_5GT + rate);	/* NVMe: 현재 링크 속도; NVMe 전송률 추정 */
	}

skip_trace:
	schedule_delayed_work(&rockchip->trace_work, msecs_to_jiffies(5000));	/* NVMe: 5초 후 다시 실행; NVMe 링크 상태 지속 모니터링 */
}

static void rockchip_pcie_ltssm_trace(struct rockchip_pcie *rockchip,
				      bool enable)				/* NVMe: LTSSM 추적 활성화/비활성화; NVMe 링크 디버깅 시 사용 */
{
	if (enable) {									/* NVMe: 추적 활성화 요청 시 */
		rockchip_pcie_writel_apb(rockchip,					/* NVMe: 디버그 패턴/전이 히트 레지스터 설정 */
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
					 PCIE_CLIENT_DBG_FIFO_MODE_CON);			/* NVMe: 디버그 FIFO 모드 활성화; LTSSM 이력 캡처 시작 */

		INIT_DELAYED_WORK(&rockchip->trace_work,				/* NVMe: 지연 워크 초기화 */
				  rockchip_pcie_ltssm_trace_work);			/* NVMe: 워크 핸들러 등록 */
		schedule_delayed_work(&rockchip->trace_work, 0);			/* NVMe: 즉시 첫 실행 예약; NVMe 링크 상태 모니터링 시작 */
	} else {									/* NVMe: 추적 비활성화 요청 시 */
		rockchip_pcie_writel_apb(rockchip,
					 PCIE_CLIENT_DBG_DIS,
					 PCIE_CLIENT_DBG_FIFO_MODE_CON);			/* NVMe: 디버그 FIFO 모드 비활성화 */
		cancel_delayed_work_sync(&rockchip->trace_work);			/* NVMe: 진행 중인 워크 동기 취소; 자원 정리 */
	}
}
#else
static void rockchip_pcie_ltssm_trace(struct rockchip_pcie *rockchip,
				      bool enable)				/* NVMe: TRACING 비활성화 시 no-op; NVMe 링크 추적 미지원 */
{
}
#endif

static void rockchip_pcie_enable_ltssm(struct rockchip_pcie *rockchip)		/* NVMe: LTSSM 활성화; NVMe SSD와의 PCIe 링크 트레이닝 시작 */
{
	rockchip_pcie_writel_apb(rockchip, PCIE_CLIENT_ENABLE_LTSSM,
				 PCIE_CLIENT_GENERAL_CON);				/* NVMe: GENERAL_CON 레지스터에 LTSSM enable 기록; */
}

static void rockchip_pcie_disable_ltssm(struct rockchip_pcie *rockchip)		/* NVMe: LTSSM 정지; NVMe 링크 해제/재초기화 시 사용 */
{
	rockchip_pcie_writel_apb(rockchip, PCIE_CLIENT_DISABLE_LTSSM,
				 PCIE_CLIENT_GENERAL_CON);				/* NVMe: GENERAL_CON 레지스터에 LTSSM disable 기록; */
}

static bool rockchip_pcie_link_up(struct dw_pcie *pci)				/* NVMe: DW 코어용 링크 업 콜백; NVMe 장치 연결 여부 판단 */
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: dw_pcie에서 rockchip_pcie 컨텍스트 획득 */
	u32 val = rockchip_pcie_get_ltssm_reg(rockchip);				/* NVMe: LTSSM 상태 레지스터 읽기 */

	return FIELD_GET(PCIE_LINKUP_MASK, val) == PCIE_LINKUP;			/* NVMe: 링크 업 비트가 L0(0x3)이면 true; NVMe SSD 준비 완료 */
}

/*
 * See e.g. section '11.6.6.4 L1 Substate' in the RK3588 TRM V1.0 for the steps
 * needed to support L1 substates. Currently, just enable L1 substates for RC
 * mode if CLKREQ# is properly connected and supports-clkreq is present in DT.
 * For EP mode, there are more things should be done to actually save power in
 * L1 substates, so disable L1 substates until there is proper support.
 */
static void rockchip_pcie_configure_l1ss(struct dw_pcie *pci)			/* NVMe: ASPM L1 서브스테이트 설정; NVMe 대기 전력 및 지연 시간에 영향 */
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: rockchip_pcie 컨텍스트 획득 */

	/* Enable L1 substates if CLKREQ# is properly connected */
	if (rockchip->supports_clkreq) {						/* NVMe: DT에 supports-clkreq가 있고 CLKREQ# 회로 연결 시 */
		rockchip_pcie_writel_apb(rockchip, PCIE_CLKREQ_READY,
					 PCIE_CLIENT_POWER_CON);				/* NVMe: CLKREQ# ready 설정; ASPM L1sub 활성화 준비 */
		pci->l1ss_support = true;						/* NVMe: DW 코어에 L1sub 지원 표시; NVMe 장치의 ASPM 협상에 반영 */
		return;									/* NVMe: 설정 완료 */
	}

	/*
	 * Otherwise, assert CLKREQ# unconditionally.  Since
	 * pci->l1ss_support is not set, the DWC core will prevent L1
	 * Substates support from being advertised.
	 */
	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLKREQ_PULL_DOWN | PCIE_CLKREQ_NOT_READY,
				 PCIE_CLIENT_POWER_CON);					/* NVMe: CLKREQ# 풀다운 및 not ready; L1sub 비활성화, NVMe ASPM L1sub 금지 */
}

static void rockchip_pcie_enable_l0s(struct dw_pcie *pci)			/* NVMe: ASPM L0s 활성화; NVMe idle 시 링크 절전 허용 */
{
	u32 cap, lnkcap;								/* NVMe: cap=PCIe capability 오프셋, lnkcap=LNKCAP 레지스터 값 */

	/* Enable L0S capability for all SoCs */
	cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);				/* NVMe: PCIe Express capability 오프셋 검색; NVMe 장치/RC 구성 공간 탐색 */
	if (cap) {									/* NVMe: PCIe capability가 존재하면 */
		lnkcap = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);		/* NVMe: LNKCAP 레지스터 읽기; ASPM 지원 비트 포함 */
		lnkcap |= PCI_EXP_LNKCAP_ASPM_L0S;					/* NVMe: L0s 지원 비트 설정; NVMe ASPM L0s 협상 가능 */
		dw_pcie_dbi_ro_wr_en(pci);							/* NVMe: DBI read-only 쓰기 활성화; LNKCAP는 일반적으로 RO */
		dw_pcie_writel_dbi(pci, cap + PCI_EXP_LNKCAP, lnkcap);			/* NVMe: LNKCAP에 L0s 활성화 기록; NVMe ASPM L0s advertise */
		dw_pcie_dbi_ro_wr_dis(pci);							/* NVMe: DBI read-only 쓰기 비활성화; 이후 우발적 수정 방지 */
	}
}

static int rockchip_pcie_start_link(struct dw_pcie *pci)			/* NVMe: PCIe 링크 시작 콜백; NVMe SSD PERST 해제 및 트레이닝 개시 */
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: rockchip_pcie 컨텍스트 획득 */

	/* Reset device */
	gpiod_set_value_cansleep(rockchip->rst_gpio, 0);				/* NVMe: PERST# GPIO low로 NVMe SSD 하드웨어 리셋 유지 */

	rockchip_pcie_enable_ltssm(rockchip);						/* NVMe: LTSSM 활성화; 링크 트레이닝 준비 */

	/*
	 * PCIe requires the refclk to be stable for 100µs prior to releasing
	 * PERST. See table 2-4 in section 2.6.2 AC Specifications of the PCI
	 * Express Card Electromechanical Specification, 1.1. However, we don't
	 * know if the refclk is coming from RC's PHY or external OSC. If it's
	 * from RC, so enabling LTSSM is the just right place to release #PERST.
	 * We need more extra time as before, rather than setting just
	 * 100us as we don't know how long should the device need to reset.
	 */
	msleep(PCIE_T_PVPERL_MS);							/* NVMe: PERST 해제 전 안정화 대기; NVMe SSD가 리셋 완료하도록 충분한 시간 확보 */

	rockchip_pcie_ltssm_trace(rockchip, true);					/* NVMe: LTSSM 추적 시작; NVMe 링크 상태 모니터링 */

	gpiod_set_value_cansleep(rockchip->rst_gpio, 1);				/* NVMe: PERST# GPIO high로 NVMe SSD 리셋 해제; 장치가 열거 시작 */

	return 0;										/* NVMe: 링크 시작 성공 */
}

static void rockchip_pcie_stop_link(struct dw_pcie *pci)			/* NVMe: PCIe 링크 중지 콜백; NVMe 장치 분리/재초기화 */
{
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: rockchip_pcie 컨텍스트 획득 */

	rockchip_pcie_disable_ltssm(rockchip);						/* NVMe: LTSSM 정지; NVMe 링크 다운 유도 */
	rockchip_pcie_ltssm_trace(rockchip, false);					/* NVMe: LTSSM 추적 중지; 자원 정리 */
}

static int rockchip_pcie_host_init(struct dw_pcie_rp *pp)			/* NVMe: RC 모드 초기화 콜백; NVMe 호스트 설정의 핵심 */
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);					/* NVMe: dw_pcie 포인터 획득; */
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);			/* NVMe: rockchip_pcie 컨텍스트 획득 */
	struct device *dev = rockchip->pci.dev;					/* NVMe: 디바이스 포인터 */
	int irq, ret;									/* NVMe: irq=레거시 IRQ 번호, ret=반환 코드 */

	irq = of_irq_get_byname(dev->of_node, "legacy");				/* NVMe: DT에서 "legacy" IRQ 획득; NVMe INTx 라인이 연결된 interrupt-parent의 IRQ */
	if (irq < 0)									/* NVMe: IRQ 획득 실패 시 */
		return irq;								/* NVMe: 오류 반환; NVMe legacy 인터럽트 없이는 열거 불가 */

	pci->dbi_base2 = pci->dbi_base + PCIE_TYPE0_HDR_DBI2_OFFSET;		/* NVMe: DBI2 베이스 설정; RC BAR(BAR0/1) 쓰기용 */

	ret = rockchip_pcie_init_irq_domain(rockchip);				/* NVMe: INTx IRQ 도메인 생성; NVMe legacy 인터럽트 Linux 등록 */
	if (ret < 0)									/* NVMe: 생성 실패 시 */
		dev_err(dev, "failed to init irq domain\n");				/* NVMe: 오류 로그; NVMe INTx 지원 실패 */

	irq_set_chained_handler_and_data(irq, rockchip_pcie_intx_handler,		/* NVMe: "legacy" IRQ에 체인 핸들러 등록; NVMe INTx 이벤트 처리 연결 */
					 rockchip);					/* NVMe: 핸들러 데이터로 rockchip_pcie 전달 */

	rockchip_pcie_configure_l1ss(pci);						/* NVMe: ASPM L1sub 설정; NVMe 대기 전력 최적화 */
	rockchip_pcie_enable_l0s(pci);							/* NVMe: ASPM L0s 활성화; NVMe idle 절전 허용 */

	/* Disable Root Ports BAR0 and BAR1 as they report bogus size */
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_0, 0x0);				/* NVMe: RC BAR0 비활성화; 가짜 크기로 인한 NVMe 메모리 매핑 오류 방지 */
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_1, 0x0);				/* NVMe: RC BAR1 비활성화; */

	return 0;										/* NVMe: RC 초기화 성공; 이후 dw_pcie_host_init가 버스 스캔/ATU/MSI 설정 */
}

static const struct dw_pcie_host_ops rockchip_pcie_host_ops = {			/* NVMe: DW 호스트 연산 테이블 */
	.init = rockchip_pcie_host_init,						/* NVMe: RC 초기화 콜백 등록; NVMe 열거 전 호출 */
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
static void rockchip_pcie_ep_hide_broken_ats_cap_rk3588(struct dw_pcie_ep *ep)	/* NVMe: RK3588 EP 모드에서 고장난 ATS capability 숨김; NVMe 호스트 측 IOMMU/ATS 안정성 */
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);					/* NVMe: dw_pcie 포인터 획득 */
	struct device *dev = pci->dev;							/* NVMe: 디바이스 포인터 */

	/* Only hide the ATS capability for RK3588 running in EP mode. */
	if (!of_device_is_compatible(dev->of_node, "rockchip,rk3588-pcie-ep"))	/* NVMe: RK3588 EP compatible이 아니면 */
		return;										/* NVMe: ATS 숨김 처리 안함 */

	dw_pcie_remove_ext_capability(pci, PCI_EXT_CAP_ID_ATS);				/* NVMe: ATS 확장 capability 제거; NVMe 호스트가 ATS 활성화 시도하지 못하도록 차단 */
}

static void rockchip_pcie_ep_init(struct dw_pcie_ep *ep)			/* NVMe: EP 모드 초기화 콜백; NVMe 타겟 역할 시 링크/ATS 설정 */
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);					/* NVMe: dw_pcie 포인터 획득 */

	rockchip_pcie_enable_l0s(pci);							/* NVMe: ASPM L0s 활성화; NVMe 호스트와의 절전 협상 */
	rockchip_pcie_ep_hide_broken_ats_cap_rk3588(ep);				/* NVMe: RK3588 EP ATS 버그 회피; */
};

static int rockchip_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				   unsigned int type, u16 interrupt_num)		/* NVMe: EP 모드에서 상향 인터럽트 발생; NVMe 장치가 호스트로 MSI/MSI-X/INTx 전송 */
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);					/* NVMe: dw_pcie 포인터 획득 */

	switch (type) {										/* NVMe: 인터럽트 유형별 분기; NVMe 호스트가 요청한 방식 */
	case PCI_IRQ_INTX:									/* NVMe: 레거시 INTx 인터럽트 */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);				/* NVMe: INTx 어서트; NVMe 호스트의 legacy INT 핸들러 호출 */
	case PCI_IRQ_MSI:									/* NVMe: MSI 인터럽트 */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);		/* NVMe: MSI 메시지 전송; NVMe 호스트의 MSI 핸들러 트리거 */
	case PCI_IRQ_MSIX:									/* NVMe: MSI-X 인터럽트 */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);		/* NVMe: MSI-X 메시지 전송; NVMe 다중 큐별 인터럽트 처리 */
	default:										/* NVMe: 알 수 없는 인터럽트 유형 */
		dev_err(pci->dev, "UNKNOWN IRQ type\n");					/* NVMe: 오류 로그; NVMe 호스트 요청과 불일치 */
	}

	return 0;											/* NVMe: 기본 반환; (PCI_IRQ_INTX/MSI/MSIX가 아닌 경우) */
}

static const struct pci_epc_features rockchip_pcie_epc_features_rk3568 = {	/* NVMe: RK3568 EP 컨트롤러 기능 선언; (NVMe host 관점에서는 대상 특성) */
	DWC_EPC_COMMON_FEATURES,							/* NVMe: DesignWare EP 공통 기능 */
	.linkup_notifier = true,							/* NVMe: 링크 업/다운 알림 지원; NVMe 호스트가 상태 변화 감지 */
	.msi_capable = true,								/* NVMe: MSI 인터럽트 지원; NVMe 호스트의 MSI 요청 처리 가능 */
	.msix_capable = true,								/* NVMe: MSI-X 인터럽트 지원; NVMe 다중 큐 인터럽트 처리 가능 */
	.align = SZ_64K,									/* NVMe: BAR 정렬 요구; NVMe 호스트 메모리 매핑 정렬 기준 */
	.bar[BAR_0] = { .type = BAR_RESIZABLE, },					/* NVMe: BAR0 크기 조정 가능; */
	.bar[BAR_1] = { .type = BAR_RESIZABLE, },
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	.bar[BAR_3] = { .type = BAR_RESIZABLE, },
	.bar[BAR_4] = { .type = BAR_RESIZABLE, },
	.bar[BAR_5] = { .type = BAR_RESIZABLE, },
};

static const struct pci_epc_bar_rsvd_region rk3588_bar4_rsvd[] = {		/* NVMe: RK3588 BAR4 예약 영역; DMA 포트 로직이 노출됨 */
	{
		/* DMA_CAP (BAR4: DMA Port Logic Structure) */
		.type = PCI_EPC_BAR_RSVD_DMA_CTRL_MMIO,					/* NVMe: BAR4의 DMA 제어 MMIO 영역으로 예약; NVMe 호스트가 일반 BAR로 사용하지 못함 */
		.offset = 0x0,									/* NVMe: BAR4 시작 오프셋 */
		.size = 0x2000,									/* NVMe: 예약 영역 크기 8KB; */
	},
};

/*
 * BAR4 on rk3588 exposes the ATU Port Logic Structure to the host regardless of
 * iATU settings for BAR4. This means that BAR4 cannot be used by an EPF driver,
 * so mark it as RESERVED.
 */
static const struct pci_epc_features rockchip_pcie_epc_features_rk3588 = {	/* NVMe: RK3588 EP 컨트롤러 기능 선언; */
	DWC_EPC_COMMON_FEATURES,							/* NVMe: DesignWare EP 공통 기능 */
	.linkup_notifier = true,							/* NVMe: 링크 상태 알림 지원 */
	.msi_capable = true,								/* NVMe: MSI 지원 */
	.msix_capable = true,								/* NVMe: MSI-X 지원 */
	.align = SZ_64K,									/* NVMe: 64KB 정렬 */
	.bar[BAR_0] = { .type = BAR_RESIZABLE, },
	.bar[BAR_1] = { .type = BAR_RESIZABLE, },
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	.bar[BAR_3] = { .type = BAR_RESIZABLE, },
	.bar[BAR_4] = {
		.type = BAR_RESERVED,								/* NVMe: BAR4 예약; NVMe 호스트는 해당 영역을 표준 MMIO BAR로 취급하지 않음 */
		.nr_rsvd_regions = ARRAY_SIZE(rk3588_bar4_rsvd),				/* NVMe: 예약 영역 개수 */
		.rsvd_regions = rk3588_bar4_rsvd,						/* NVMe: 예약 영역 배열 참조 */
	},
	.bar[BAR_5] = { .type = BAR_RESIZABLE, },
};

static const struct pci_epc_features *
rockchip_pcie_get_features(struct dw_pcie_ep *ep)				/* NVMe: EP 기능 반환 콜백; NVMe 호스트가 EP capability 파악 */
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);					/* NVMe: dw_pcie 포인터 획득 */
	struct rockchip_pcie *rockchip = to_rockchip_pcie(pci);				/* NVMe: rockchip_pcie 컨텍스트 획득 */

	return rockchip->data->epc_features;						/* NVMe: DT 매칭된 EP 기능 반환; */
}

static const struct dw_pcie_ep_ops rockchip_pcie_ep_ops = {			/* NVMe: DW EP 모드 연산 테이블 */
	.init = rockchip_pcie_ep_init,							/* NVMe: EP 초기화 콜백 */
	.raise_irq = rockchip_pcie_raise_irq,						/* NVMe: 상향 인터럽트 발생 콜백 */
	.get_features = rockchip_pcie_get_features,					/* NVMe: EP 기능 조회 콜백 */
};

static int rockchip_pcie_clk_init(struct rockchip_pcie *rockchip)		/* NVMe: PCIe 클럭 초기화; NVMe DMA/링크 동작에 필요한 모든 클럭 획득/활성화 */
{
	struct device *dev = rockchip->pci.dev;						/* NVMe: 디바이스 포인터 */
	int ret;											/* NVMe: 반환 코드 */

	ret = devm_clk_bulk_get_all(dev, &rockchip->clks);					/* NVMe: DT에 정의된 모든 PCIe 클럭 획득; NVMe 컨트롤러 동작에 필수 */
	if (ret < 0)										/* NVMe: 클럭 획득 실패 시 */
		return dev_err_probe(dev, ret, "failed to get clocks\n");			/* NVMe: 오류 로그; NVMe 컨트롤러가 동작 불가 */

	rockchip->clk_cnt = ret;									/* NVMe: 획득한 클럭 개수 저장; 해제 시 사용 */

	ret = clk_bulk_prepare_enable(rockchip->clk_cnt, rockchip->clks);		/* NVMe: 모든 클럭 prepare/enable; NVMe 링크 트레이닝 전 클럭 공급 */
	if (ret)											/* NVMe: 클럭 활성화 실패 시 */
		return dev_err_probe(dev, ret, "failed to enable clocks\n");			/* NVMe: 오류 로그; NVMe 동작 중단 */

	return 0;											/* NVMe: 클럭 초기화 성공 */
}

static int rockchip_pcie_resource_get(struct platform_device *pdev,
				      struct rockchip_pcie *rockchip)		/* NVMe: 플랫폼 리소스 획득; APB 메모리, PERST GPIO, 리셋, CLKREQ# 플래그 */
{
	rockchip->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");	/* NVMe: "apb" 메모리 리소스 ioremap; PCIe 클라이언트 레지스터 접근 베이스 확보 */
	if (IS_ERR(rockchip->apb_base))							/* NVMe: ioremap 실패 시 */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->apb_base),
				     "failed to map apb registers\n");				/* NVMe: 오류 반환; LTSSM/IRQ/PM 제어 불가 */

	rockchip->rst_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
						     GPIOD_OUT_LOW);				/* NVMe: "reset" GPIO(PERST#) 획득; NVMe SSD 리셋 라인, 기본 low로 설정 */
	if (IS_ERR(rockchip->rst_gpio))							/* NVMe: GPIO 획득 실패 시 */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->rst_gpio),
				     "failed to get reset gpio\n");				/* NVMe: 오류 반환; NVMe 하드웨어 리셋 불가 */

	rockchip->rst = devm_reset_control_array_get_exclusive(&pdev->dev);		/* NVMe: PCIe 컨트롤러 리셋 라인 배열 획득; SoC 내장 컨트롤러 초기화 */
	if (IS_ERR(rockchip->rst))								/* NVMe: 리셋 획득 실패 시 */
		return dev_err_probe(&pdev->dev, PTR_ERR(rockchip->rst),
				     "failed to get reset lines\n");				/* NVMe: 오류 반환; 컨트롤러 브링업 불가 */

	rockchip->supports_clkreq = of_property_read_bool(pdev->dev.of_node,
							  "supports-clkreq");				/* NVMe: DT supports-clkreq 속성 존재 여부; ASPM L1sub 지원 결정 */

	return 0;											/* NVMe: 리소스 획득 성공 */
}

static int rockchip_pcie_phy_init(struct rockchip_pcie *rockchip)		/* NVMe: PCIe PHY 초기화; NVMe 장치와의 물리적 링크 계층 준비 */
{
	struct device *dev = rockchip->pci.dev;						/* NVMe: 디바이스 포인터 */
	int ret;											/* NVMe: 반환 코드 */

	rockchip->phy = devm_phy_get(dev, "pcie-phy");					/* NVMe: "pcie-phy" PHY 핸들 획득; 트랜시버/PLL 설정 */
	if (IS_ERR(rockchip->phy))								/* NVMe: PHY 획득 실패 시 */
		return dev_err_probe(dev, PTR_ERR(rockchip->phy),
				     "missing PHY\n");						/* NVMe: 오류 반환; NVMe 물리 링크 형성 불가 */

	ret = phy_init(rockchip->phy);								/* NVMe: PHY 초기화; NVMe 링크 트레이닝을 위한 물리 계층 설정 */
	if (ret < 0)										/* NVMe: PHY 초기화 실패 시 */
		return ret;									/* NVMe: 오류 반환; NVMe PCIe 신호 생성 불가 */

	ret = phy_power_on(rockchip->phy);							/* NVMe: PHY 전원 켜기; NVMe 링크에 대한 전기 신호 활성화 */
	if (ret)											/* NVMe: PHY 전원 실패 시 */
		phy_exit(rockchip->phy);								/* NVMe: PHY 초기화 롤백; 자원 누수 방지 */

	return ret;											/* NVMe: 성공(0) 또는 실패 코드 반환 */
}

static void rockchip_pcie_phy_deinit(struct rockchip_pcie *rockchip)		/* NVMe: PCIe PHY 해제; NVMe 링크 종료 및 전력 절감 */
{
	phy_power_off(rockchip->phy);								/* NVMe: PHY 전원 끄기; NVMe 장치와의 물리 신호 중단 */
	phy_exit(rockchip->phy);									/* NVMe: PHY 초기화 해제; 자원 반납 */
}

static const struct dw_pcie_ops dw_pcie_ops = {					/* NVMe: DW PCIe 코어에 등록할 SoC 특화 연산 */
	.link_up = rockchip_pcie_link_up,							/* NVMe: 링크 업 콜백; NVMe 장치 연결 여부 */
	.start_link = rockchip_pcie_start_link,						/* NVMe: 링크 시작 콜백; NVMe PERST 해제/트레이닝 */
	.stop_link = rockchip_pcie_stop_link,							/* NVMe: 링크 중지 콜백; NVMe 장치 분리 */
	.get_ltssm = rockchip_pcie_get_ltssm,							/* NVMe: LTSSM 상태 콜백; NVMe 링크/절전 상태 진단 */
};

static irqreturn_t rockchip_pcie_ep_sys_irq_thread(int irq, void *arg)		/* NVMe: EP 모드 시스템 IRQ 스레드 핸들러; 링크 업/다운/핫리셋 처리 */
{
	struct rockchip_pcie *rockchip = arg;							/* NVMe: 핸들러 인자에서 rockchip_pcie 획득 */
	struct dw_pcie *pci = &rockchip->pci;							/* NVMe: dw_pcie 포인터 획득 */
	struct device *dev = pci->dev;								/* NVMe: 디바이스 포인터 */
	u32 reg, val;											/* NVMe: reg=MISC 상태, val=쓰기용 임시 값 */

	reg = rockchip_pcie_readl_apb(rockchip, PCIE_CLIENT_INTR_STATUS_MISC);	/* NVMe: 기타 인터럽트 상태 읽기; 링크/리셋 이벤트 확인 */
	rockchip_pcie_writel_apb(rockchip, reg, PCIE_CLIENT_INTR_STATUS_MISC);	/* NVMe: 읽은 값을 다시 써서 인터럽트 클리어; W1C 동작 */

	dev_dbg(dev, "PCIE_CLIENT_INTR_STATUS_MISC: %#x\n", reg);			/* NVMe: 디버그 로그; NVMe 상태 분석 시 참고 */
	dev_dbg(dev, "LTSSM_STATUS: %#x\n", rockchip_pcie_get_ltssm_reg(rockchip));	/* NVMe: LTSSM 상태 로그; NVMe 링크 상태 추적 */

	if (reg & PCIE_LINK_REQ_RST_NOT_INT) {							/* NVMe: 핫 리셋 또는 링크 다운 리셋 요청 수신 시 */
		dev_dbg(dev, "hot reset or link-down reset\n");					/* NVMe: 디버그 로그; NVMe 호스트가 재열거/장치 제거를 요구함 */
		dw_pcie_ep_linkdown(&pci->ep);							/* NVMe: EP 링크 다운 처리; NVMe 호스트와의 연결 종료 알림 */
		/* Stop delaying link training. */
		val = FIELD_PREP_WM16(PCIE_LTSSM_APP_DLY2_DONE, 1);				/* NVMe: LTSSM 지연2 완료 값 준비 */
		rockchip_pcie_writel_apb(rockchip, val,
					 PCIE_CLIENT_HOT_RESET_CTRL);				/* NVMe: 지연 완료 비트 설정; 핫 리셋 처리 후 재트레이닝 진행 */
	}

	if (reg & PCIE_RDLH_LINK_UP_CHGED) {							/* NVMe: DLL 링크 업 상태 변화 시 */
		if (rockchip_pcie_link_up(pci)) {						/* NVMe: 실제로 링크가 up이면 */
			dev_dbg(dev, "link up\n");							/* NVMe: 디버그 로그; NVMe 호스트와의 링크 복구/연결 완료 */
			dw_pcie_ep_linkup(&pci->ep);							/* NVMe: EP 링크 업 처리; NVMe 호스트에 연결 가능 알림 */
		}
	}

	return IRQ_HANDLED;										/* NVMe: IRQ 처리 완료 반환 */
}

static int rockchip_pcie_configure_rc(struct rockchip_pcie *rockchip)		/* NVMe: RC(Root Complex) 모드 구성; NVMe SSD를 열거할 호스트 설정 */
{
	struct dw_pcie_rp *pp;										/* NVMe: DW PCIe RP 구조체 포인터 */
	u32 val;												/* NVMe: 레지스터 쓰기용 값 */

	if (!IS_ENABLED(CONFIG_PCIE_ROCKCHIP_DW_HOST))					/* NVMe: RC 호스트 드라이버 커널 설정이 꺼져 있으면 */
		return -ENODEV;										/* NVMe: NVMe 호스트 기능 사용 불가 */

	/* LTSSM enable control mode */
	val = FIELD_PREP_WM16(PCIE_LTSSM_ENABLE_ENHANCE, 1);				/* NVMe: LTSSM 강화 모드 활성화 값 준비; 링크 안정성 향상 */
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_HOT_RESET_CTRL);	/* NVMe: 핫 리셋 제어 레지스터에 기록; */

	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLIENT_SET_MODE(PCIE_CLIENT_MODE_RC),
				 PCIE_CLIENT_GENERAL_CON);							/* NVMe: PCIe 클라이언트를 RC 모드로 설정; NVMe 호스트(루트 컴플렉스) 동작 */

	pp = &rockchip->pci.pp;										/* NVMe: dw_pcie 내 RP 포인터 획득 */
	pp->ops = &rockchip_pcie_host_ops;								/* NVMe: Rockchip 호스트 연산 등록; NVMe 열거 초기화 시 호출 */

	return dw_pcie_host_init(pp);									/* NVMe: DW PCIe 호스트 초기화; NVMe 버스 스캔, ATU, MSI/MSI-X 설정 수행 */
}

static int rockchip_pcie_configure_ep(struct platform_device *pdev,
				      struct rockchip_pcie *rockchip)		/* NVMe: EP(Endpoint) 모드 구성; NVMe 타겟 장치로 동작 */
{
	struct device *dev = &pdev->dev;								/* NVMe: 플랫폼 디바이스 포인터 */
	int irq, ret;												/* NVMe: irq=sys IRQ, ret=반환 코드 */
	u32 val;													/* NVMe: 레지스터 값 */

	if (!IS_ENABLED(CONFIG_PCIE_ROCKCHIP_DW_EP))						/* NVMe: EP 드라이버 커널 설정이 꺼져 있으면 */
		return -ENODEV;											/* NVMe: NVMe 타겟 모드 사용 불가 */

	irq = platform_get_irq_byname(pdev, "sys");							/* NVMe: "sys" 이름의 시스템 IRQ 획득; 링크/리셋 이벤트용 */
	if (irq < 0)												/* NVMe: IRQ 획득 실패 시 */
		return irq;												/* NVMe: 오류 반환; NVMe EP 이벤트 처리 불가 */

	ret = devm_request_threaded_irq(dev, irq, NULL,
					rockchip_pcie_ep_sys_irq_thread,
					IRQF_ONESHOT, "pcie-sys-ep", rockchip);				/* NVMe: 시스템 IRQ에 스레드 핸들러 등록; NVMe 링크 변화/리셋 이벤트 처리 */
	if (ret) {													/* NVMe: IRQ 등록 실패 시 */
		dev_err(dev, "failed to request PCIe sys IRQ\n");					/* NVMe: 오류 로그; NVMe EP 상태 변화 감시 불가 */
		return ret;												/* NVMe: 실패 반환 */
	}

	/*
	 * LTSSM enable control mode, and automatically delay link training on
	 * hot reset/link-down reset.
	 */
	val = FIELD_PREP_WM16(PCIE_LTSSM_ENABLE_ENHANCE, 1) |
	      FIELD_PREP_WM16(PCIE_LTSSM_APP_DLY2_EN, 1);						/* NVMe: LTSSM 강화 모드 + 지연2 활성화 값 준비; 핫 리셋 후 재트레이닝 지연 */
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_HOT_RESET_CTRL);		/* NVMe: 핫 리셋 제어 레지스터에 기록; */

	rockchip_pcie_writel_apb(rockchip,
				 PCIE_CLIENT_SET_MODE(PCIE_CLIENT_MODE_EP),
				 PCIE_CLIENT_GENERAL_CON);								/* NVMe: PCIe 클라이언트를 EP 모드로 설정; NVMe 타겟 동작 */

	rockchip->pci.ep.ops = &rockchip_pcie_ep_ops;							/* NVMe: EP 연산 테이블 등록; */
	rockchip->pci.ep.page_size = SZ_64K;									/* NVMe: EP 페이지 크기 64KB; NVMe 호스트와의 BAR/매핑 정렬 */

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));						/* NVMe: EP DMA 주소 마스크 64비트 설정; NVMe 호스트가 64비트 DMA 주소 사용 가능 */

	ret = dw_pcie_ep_init(&rockchip->pci.ep);								/* NVMe: DW EP 초기화; NVMe 호스트가 인식할 EP capability/BAR 설정 */
	if (ret) {													/* NVMe: EP 초기화 실패 시 */
		dev_err(dev, "failed to initialize endpoint\n");						/* NVMe: 오류 로그; NVMe 타겟 준비 실패 */
		return ret;												/* NVMe: 실패 반환 */
	}

	ret = dw_pcie_ep_init_registers(&rockchip->pci.ep);						/* NVMe: DW EP 레지스터 초기화; NVMe 호스트가 읽을 Type0/1 cfg space 구성 */
	if (ret) {													/* NVMe: EP 레지스터 초기화 실패 시 */
		dev_err(dev, "failed to initialize DWC endpoint registers\n");			/* NVMe: 오류 로그; */
		dw_pcie_ep_deinit(&rockchip->pci.ep);								/* NVMe: EP 초기화 롤백; 자원 정리 */
		return ret;												/* NVMe: 실패 반환 */
	}

	pci_epc_init_notify(rockchip->pci.ep.epc);								/* NVMe: EP 컨트롤러 초기화 완료 알림; NVMe 호스트 측 EPF 드라이버에 ready 전파 */

	/* unmask DLL up/down indicator and hot reset/link-down reset */
	val = FIELD_PREP_WM16(PCIE_RDLH_LINK_UP_CHGED, 0) |
	      FIELD_PREP_WM16(PCIE_LINK_REQ_RST_NOT_INT, 0);						/* NVMe: 링크 업/다운 및 핫 리셋 인터럽트 언마스크 값 준비 */
	rockchip_pcie_writel_apb(rockchip, val, PCIE_CLIENT_INTR_MASK_MISC);		/* NVMe: MISC 인터럽트 마스크 해제; NVMe EP 상태 변화 감시 시작 */

	return ret;													/* NVMe: 성공(0) 반환 */
}

static int rockchip_pcie_probe(struct platform_device *pdev)				/* NVMe: 플랫폼 드라이버 probe; SoC PCIe 컨트롤러와 NVMe 호스트/EP 바인딩 */
{
	struct device *dev = &pdev->dev;									/* NVMe: 플랫폼 디바이스 포인터 */
	struct rockchip_pcie *rockchip;									/* NVMe: Rockchip PCIe 드라이버 사설 구조체 포인터 */
	const struct rockchip_pcie_of_data *data;							/* NVMe: DT 매칭 데이터 포인터 */
	int ret;														/* NVMe: 반환 코드 */

	data = of_device_get_match_data(dev);									/* NVMe: DT compatible에 해당하는 rockchip_pcie_of_data 획득; RC/EP 모드 결정 */
	if (!data)														/* NVMe: 매칭 데이터 없으면 */
		return -EINVAL;												/* NVMe: probe 실패; NVMe 컨트롤러 인식 불가 */

	rockchip = devm_kzalloc(dev, sizeof(*rockchip), GFP_KERNEL);				/* NVMe: 드라이버 구조체 메모리 할당; probe 시점의 NVMe 컨트롤러 컨텍스트 */
	if (!rockchip)														/* NVMe: 메모리 할당 실패 시 */
		return -ENOMEM;												/* NVMe: 메모리 부족 반환 */

	platform_set_drvdata(pdev, rockchip);									/* NVMe: 플랫폼 디바이스에 rockchip_pcie 저장; suspend/resume 및 제거 시 사용 */

	rockchip->pci.dev = dev;											/* NVMe: dw_pcie에 디바이스 포인터 연결; DMA/IRQ/로깅에 사용 */
	rockchip->pci.ops = &dw_pcie_ops;									/* NVMe: DW 코어에 SoC 연산 등록; 링크 업/시작/정지/LTSSM */
	rockchip->data = data;												/* NVMe: DT 매칭 데이터 저장; */

	/* Default N_FTS value (210) is broken, override it to 255 */
	rockchip->pci.n_fts[0] = 255; /* Gen1 */								/* NVMe: Gen1 N_FTS 255로 오버라이드; NVMe 링크 복구 시 FTS(Fast Training Sequence) 안정화 */
	rockchip->pci.n_fts[1] = 255; /* Gen2+ */								/* NVMe: Gen2 이상 N_FTS 255로 오버라이드; */

	ret = rockchip_pcie_resource_get(pdev, rockchip);						/* NVMe: APB, GPIO, reset, clkreq 플래그 획득; NVMe 컨트롤러 하드웨어 리소스 */
	if (ret)															/* NVMe: 리소스 획득 실패 시 */
		return ret;														/* NVMe: probe 실패; NVMe 컨트롤러 초기화 불가 */

	ret = reset_control_assert(rockchip->rst);								/* NVMe: PCIe 컨트롤러 리셋 어서트; 초기화 전 컨트롤러를 알려진 상태로 */
	if (ret)															/* NVMe: 리셋 어서트 실패 시 */
		return ret;														/* NVMe: probe 실패; NVMe 컨트롤러 브링업 불가 */

	/* DON'T MOVE ME: must be enable before PHY init */
	ret = devm_regulator_get_enable_optional(dev, "vpcie3v3");					/* NVMe: 3.3V PCIe 전원 레귤레이터 선택적 활성화; PHY/장치 전원 공급, PHY init 전에 반드시 수행 */
	if (ret < 0 && ret != -ENODEV)											/* NVMe: 실제 오류이면(없는 경우 -ENODEV 제외) */
		return dev_err_probe(dev, ret,
				     "failed to enable vpcie3v3 regulator\n");						/* NVMe: 오류 반환; NVMe 장치/PHY 전원 부족 */

	ret = rockchip_pcie_phy_init(rockchip);									/* NVMe: PHY 초기화/전원 켜기; NVMe 물리 링크 준비 */
	if (ret)															/* NVMe: PHY 초기화 실패 시 */
		return dev_err_probe(dev, ret,
				     "failed to initialize the phy\n");							/* NVMe: 오류 반환; NVMe PCIe 신호 생성 불가 */

	ret = reset_control_deassert(rockchip->rst);								/* NVMe: PCIe 컨트롤러 리셋 해제; PHY 활성화 후 컨트롤러 동작 시작 */
	if (ret)															/* NVMe: 리셋 해제 실패 시 */
		goto deinit_phy;												/* NVMe: PHY 해제 후 반환; 자원 정리 */

	ret = rockchip_pcie_clk_init(rockchip);									/* NVMe: PCIe 클럭 획득/활성화; NVMe DMA/링크 동작에 필요 */
	if (ret)															/* NVMe: 클럭 초기화 실패 시 */
		goto deinit_phy;												/* NVMe: PHY 해제 후 반환; */

	switch (data->mode) {												/* NVMe: DT에 지정된 RC/EP 모드별 분기; NVMe 호스트는 RC */
	case DW_PCIE_RC_TYPE:												/* NVMe: RC 모드: NVMe SSD를 열거할 호스트 */
		ret = rockchip_pcie_configure_rc(rockchip);							/* NVMe: RC 모드 구성 및 dw_pcie_host_init 호출; NVMe 버스 열거 시작 */
		if (ret)														/* NVMe: RC 구성 실패 시 */
			goto deinit_clk;											/* NVMe: 클럭/PHY 해제 후 반환 */
		break;														/* NVMe: RC 분기 종료 */
	case DW_PCIE_EP_TYPE:												/* NVMe: EP 모드: NVMe 타겟 장치 */
		ret = rockchip_pcie_configure_ep(pdev, rockchip);						/* NVMe: EP 모드 구성; NVMe 호스트에 의해 발견/바인딩 준비 */
		if (ret)														/* NVMe: EP 구성 실패 시 */
			goto deinit_clk;											/* NVMe: 클럭/PHY 해제 후 반환 */
		break;														/* NVMe: EP 분기 종료 */
	default:															/* NVMe: 알 수 없는 모드 */
		dev_err(dev, "INVALID device type %d\n", data->mode);						/* NVMe: 오류 로그; NVMe 컨트롤러를 RC/EP로 설정 불가 */
		ret = -EINVAL;													/* NVMe: 무효 인자 반환 */
		goto deinit_clk;												/* NVMe: 자원 정리 */
	}

	return 0;															/* NVMe: probe 성공; NVMe PCIe 컨트롤러 준비 완료 */

deinit_clk:
	clk_bulk_disable_unprepare(rockchip->clk_cnt, rockchip->clks);				/* NVMe: 에러 경로: 활성화된 클럭 모두 비활성화/unprepare; */
deinit_phy:
	rockchip_pcie_phy_deinit(rockchip);										/* NVMe: 에러 경로: PHY 전원 끄고 초기화 해제; */

	return ret;															/* NVMe: 실패 코드 반환 */
}

static const struct rockchip_pcie_of_data rockchip_pcie_rc_of_data_rk3568 = {	/* NVMe: RK3568 RC 모드 DT 데이터 */
	.mode = DW_PCIE_RC_TYPE,												/* NVMe: RC 모드 지정; NVMe 호스트로 동작 */
};

static const struct rockchip_pcie_of_data rockchip_pcie_ep_of_data_rk3568 = {	/* NVMe: RK3568 EP 모드 DT 데이터 */
	.mode = DW_PCIE_EP_TYPE,												/* NVMe: EP 모드 지정; */
	.epc_features = &rockchip_pcie_epc_features_rk3568,							/* NVMe: EP 기능 테이블 연결; */
};

static const struct rockchip_pcie_of_data rockchip_pcie_ep_of_data_rk3588 = {	/* NVMe: RK3588 EP 모드 DT 데이터 */
	.mode = DW_PCIE_EP_TYPE,												/* NVMe: EP 모드 지정; */
	.epc_features = &rockchip_pcie_epc_features_rk3588,							/* NVMe: RK3588 EP 기능(ATS 버그 회피/예약 BAR) 연결; */
};

static const struct of_device_id rockchip_pcie_of_match[] = {					/* NVMe: DT compatible 매칭 테이블; NVMe 컨트롤러 바인딩 기준 */
	{
		.compatible = "rockchip,rk3568-pcie",								/* NVMe: RK3568 PCIe host compatible; NVMe SSD를 연결할 RC */
		.data = &rockchip_pcie_rc_of_data_rk3568,
	},
	{
		.compatible = "rockchip,rk3568-pcie-ep",							/* NVMe: RK3568 PCIe EP compatible; */
		.data = &rockchip_pcie_ep_of_data_rk3568,
	},
	{
		.compatible = "rockchip,rk3588-pcie-ep",							/* NVMe: RK3588 PCIe EP compatible; */
		.data = &rockchip_pcie_ep_of_data_rk3588,
	},
	{},																/* NVMe: 테이블 종료 */
};

static struct platform_driver rockchip_pcie_driver = {						/* NVMe: 플랫폼 드라이버 구조체 */
	.driver = {
		.name	= "rockchip-dw-pcie",										/* NVMe: 드라이버 이름 */
		.of_match_table = rockchip_pcie_of_match,								/* NVMe: DT 매칭 테이블 등록; NVMe PCIe 컨트롤러 자동 바인딩 */
		.suppress_bind_attrs = true,										/* NVMe: sysfs manual bind/unbind 억제; */
	},
	.probe = rockchip_pcie_probe,												/* NVMe: probe 콜백 등록; NVMe 컨트롤러 발견 시 실행 */
};
builtin_platform_driver(rockchip_pcie_driver);										/* NVMe: 내장 플랫폼 드라이버 등록; 부팅 시 NVMe PCIe 컨트롤러 초기화 */
