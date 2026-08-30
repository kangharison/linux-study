// SPDX-License-Identifier: GPL-2.0+
/* NVMe: Xilinx Versal CPM PCIe RC 드라이버 - NVMe SSD가 이 RC 아래에 열거되고 바인딩됨 */
/*
 * PCIe host controller driver for Xilinx Versal CPM DMA Bridge
 *
 * (C) Copyright 2019 - 2020, Xilinx, Inc.
 */

#include <linux/bitfield.h>      /* PCI/NVMe: 레지스터 비트필드 파싱에 사용 */
#include <linux/interrupt.h>     /* NVMe: INTx/MSI-X 외에 RC 자체 이벤트 IRQ 처리 */
#include <linux/irq.h>           /* NVMe: 레거시 INTx 가상 IRQ 생성에 사용 */
#include <linux/irqchip.h>       /* PCI/NVMe: RC 낮은 수준 인터럽트 chip 등록 */
#include <linux/irqchip/chained_irq.h> /* NVMe: RC IRQ -> INTx/event 체인 핸들러 */
#include <linux/irqdomain.h>     /* NVMe: INTx용 IRQ domain 할당 */
#include <linux/kernel.h>        /* PCI/NVMe: 커널 기반 헤더 */
#include <linux/module.h>        /* NVMe: PCIe host controller 모듈화 */
#include <linux/of_address.h>    /* NVMe: DT로부터 cpm_slcr/cfg 리소스 파싱 */
#include <linux/of_pci.h>        /* NVMe: PCI bus-range 등 DT 파싱 지원 */
#include <linux/of_platform.h>   /* PCI/NVMe: platform_driver 등록 */

#include "../pci.h"              /* NVMe: PCI 코어 헤더, ECAM, host bridge 공통 정의 */
#include "pcie-xilinx-common.h"  /* NVMe: Xilinx PCIe 공통 인터럽트 정의 공유 */

/* Register definitions */
#define XILINX_CPM_PCIE_REG_IDR		0x00000E10 /* PCI/NVMe: RC 이벤트 인터럽트 상태 레지스터 */
#define XILINX_CPM_PCIE_REG_IMR		0x00000E14 /* PCI/NVMe: RC 이벤트 인터럽트 마스크 레지스터 */
#define XILINX_CPM_PCIE_REG_PSCR	0x00000E1C /* NVMe: PHY/link 상태 레지스터 - NVMe SSD 연결 감지 */
#define XILINX_CPM_PCIE_REG_RPSC	0x00000E20 /* NVMe: RC bridge enable/상태 제어 레지스터 */
#define XILINX_CPM_PCIE_REG_RPEFR	0x00000E2C /* NVMe: Root Port Error FIFO - AER/비치별 에러 정보 */
#define XILINX_CPM_PCIE_REG_IDRN	0x00000E38 /* PCI/NVMe: INTx 디어세션(raw status) 레지스터 */
#define XILINX_CPM_PCIE_REG_IDRN_MASK	0x00000E3C /* NVMe: INTx 마스크 - 레거시 INTA~D 제어 */
#define XILINX_CPM_PCIE_MISC_IR_STATUS	0x00000340 /* PCI/NVMe: CPM SLCR misc 인터럽트 상태 */
#define XILINX_CPM_PCIE_MISC_IR_ENABLE	0x00000348 /* NVMe: CPM SLCR misc 인터럽트 enable */
#define XILINX_CPM_PCIE0_MISC_IR_LOCAL	BIT(1)  /* NVMe: CPM0 local misc IRQ 비트 */
#define XILINX_CPM_PCIE1_MISC_IR_LOCAL	BIT(2)  /* NVMe: CPM1 local misc IRQ 비트 */

#define XILINX_CPM_PCIE0_IR_STATUS	0x000002A0 /* NVMe: CPM0 error IR status (CPM5) */
#define XILINX_CPM_PCIE1_IR_STATUS	0x000002B4 /* NVMe: CPM1 error IR status (CPM5) */
#define XILINX_CPM_PCIE0_IR_ENABLE	0x000002A8 /* NVMe: CPM0 error IR enable (CPM5) */
#define XILINX_CPM_PCIE1_IR_ENABLE	0x000002BC /* NVMe: CPM1 error IR enable (CPM5) */
#define XILINX_CPM_PCIE_IR_LOCAL	BIT(0)  /* PCI/NVMe: per-host local error IRQ enable 비트 */

#define IMR(x) BIT(XILINX_PCIE_INTR_ ##x) /* NVMe: IMR용 인터럽트 비트 매크로 */

#define XILINX_CPM_PCIE_IMR_ALL_MASK		\
	(					\
		IMR(LINK_DOWN)		|	/* NVMe: 링크 다운 - NVMe SSD 분리/핫플러그 감지 */	\
		IMR(HOT_RESET)		|	/* NVMe: 핫 리셋 수신 - NVMe 장치 재초기화 */	\
		IMR(CFG_PCIE_TIMEOUT)	|	/* NVMe: PCIe ECAM 접근 타임아웃 */	\
		IMR(CFG_TIMEOUT)	|	/* NVMe: ECAM 타임아웃 */	\
		IMR(CORRECTABLE)	|	/* NVMe: PCIe correctable error (AER) */	\
		IMR(NONFATAL)		|	/* NVMe: PCIe non-fatal error (AER) */	\
		IMR(FATAL)		|	/* NVMe: PCIe fatal error (AER) */	\
		IMR(CFG_ERR_POISON)	|	/* NVMe: ECAM poisoned completion */	\
		IMR(PME_TO_ACK_RCVD)	|	/* NVMe: PME_Turn_Off Ack - NVMe 전력 관리 */	\
		IMR(INTX)		|	/* NVMe: 레거시 INTx 발생 - NVMe MSI 미사용 시 */	\
		IMR(PM_PME_RCVD)	|	/* NVMe: NVMe 장치로부터 PME 메시지 수신 */	\
		IMR(SLV_UNSUPP)		|	/* NVMe: unsupported request from/to NVMe */	\
		IMR(SLV_UNEXP)		|	/* NVMe: unexpected completion */	\
		IMR(SLV_COMPL)		|	/* NVMe: completion timeout */	\
		IMR(SLV_ERRP)		|	/* NVMe: error poisoned TLP */	\
		IMR(SLV_CMPABT)		|	/* NVMe: completer abort */	\
		IMR(SLV_ILLBUR)		|	/* NVMe: illegal burst */	\
		IMR(MST_DECERR)		|	/* NVMe: master decode error */	\
		IMR(MST_SLVERR)		|	/* NVMe: master slave error */	\
		IMR(SLV_PCIE_TIMEOUT)		/* NVMe: PCIe completion timeout */	\
	)

#define XILINX_CPM_PCIE_IDR_ALL_MASK		0xFFFFFFFF /* NVMe: IDR 레지스터 전체 클리어용 */
#define XILINX_CPM_PCIE_IDRN_MASK		GENMASK(19, 16) /* NVMe: IDRN[19:16] = INTx 상태 */
#define XILINX_CPM_PCIE_IDRN_SHIFT		16          /* NVMe: INTx 비트 시작 위치 */

/* Root Port Error FIFO Read Register definitions */
#define XILINX_CPM_PCIE_RPEFR_ERR_VALID		BIT(18)     /* NVMe: 에러 FIFO 유효 비트 */
#define XILINX_CPM_PCIE_RPEFR_REQ_ID		GENMASK(15, 0) /* NVMe: 에러를 유발한 requester ID (BDF) */
#define XILINX_CPM_PCIE_RPEFR_ALL_MASK		0xFFFFFFFF /* NVMe: 에러 FIFO 클리어용 */

/* Root Port Status/control Register definitions */
#define XILINX_CPM_PCIE_REG_RPSC_BEN		BIT(0)  /* NVMe: bridge enable - RC 동작 허가 */

/* Phy Status/Control Register definitions */
#define XILINX_CPM_PCIE_REG_PSCR_LNKUP		BIT(11) /* NVMe: PCIe link up 상태 비트 - NVMe 연결 여부 */

enum xilinx_cpm_version {
	CPM,		/* NVMe: Versal CPM 1세대 RC */
	CPM5,		/* NVMe: Versal CPM5 RC, host0 */
	CPM5_HOST1,	/* NVMe: Versal CPM5 RC, host1 */
	CPM5NC_HOST,	/* NVMe: CPM5 no-cache host 변형 - IRQ 초기화 생략 */
};

/**
 * struct xilinx_cpm_variant - CPM variant information
 * @version: CPM version
 * @ir_status: Offset for the error interrupt status register
 * @ir_enable: Offset for the CPM5 local error interrupt enable register
 * @ir_misc_value: A bitmask for the miscellaneous interrupt status
 */
/* NVMe: 버전별 레지스터 오프셋 차이를 캡슐화 - NVMe RC 초기화 경로에 사용 */
struct xilinx_cpm_variant {
	enum xilinx_cpm_version version;
	u32 ir_status;
	u32 ir_enable;
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
/* NVMe: PCIe host bridge당 하나의 포트 구조체 - NVMe SSD가 탑재된 RC의 모든 상태 보유 */
struct xilinx_cpm_pcie {
	struct device			*dev;        /* NVMe: struct device - IOMMU/SMMU DMA domain 연결 지점 */
	void __iomem			*reg_base;   /* NVMe: PCIe host controller 브릿지 레지스터 공간 */
	void __iomem			*cpm_base;   /* NVMe: CPM SLCR 공유 레지스터 공간 */
	struct irq_domain		*intx_domain; /* NVMe: INTx(INTA~D) 가상 IRQ domain */
	struct irq_domain		*cpm_domain;  /* NVMe: RC 이벤트 IRQ domain */
	struct pci_config_window	*cfg;         /* NVMe: ECAM config window - NVMe BAR/MSI 등 탐색 */
	int				intx_irq;     /* NVMe: 레거시 INTx chained IRQ 번호 */
	int				irq;          /* NVMe: RC 에러/이벤트 IRQ 번호 */
	raw_spinlock_t			lock;         /* NVMe: 레지스터 접근과 INTx mask 동기화 */
	const struct xilinx_cpm_variant   *variant; /* NVMe: CPM 버전별 특성 포인터 */
};

static u32 pcie_read(struct xilinx_cpm_pcie *port, u32 reg)
{
	return readl_relaxed(port->reg_base + reg); /* NVMe: RC 브릿지 레지스터 읽기 - NVMe 접근 전 상태 확인 */
}

static void pcie_write(struct xilinx_cpm_pcie *port,
		       u32 val, u32 reg)
{
	writel_relaxed(val, port->reg_base + reg); /* NVMe: RC 브릿지 레지스터 쓰기 - IRQ/link 제어 */
}

static bool cpm_pcie_link_up(struct xilinx_cpm_pcie *port)
{
	return (pcie_read(port, XILINX_CPM_PCIE_REG_PSCR) &
		XILINX_CPM_PCIE_REG_PSCR_LNKUP); /* NVMe: PHY link up? - NVMe SSD 연결됨 */
}

static void cpm_pcie_clear_err_interrupts(struct xilinx_cpm_pcie *port)
{
	unsigned long val = pcie_read(port, XILINX_CPM_PCIE_REG_RPEFR); /* NVMe: AER 에러 FIFO 읽기 */

	if (val & XILINX_CPM_PCIE_RPEFR_ERR_VALID) { /* NVMe: 유효 에러가 있으면 */
		dev_dbg(port->dev, "Requester ID %lu\n",
			val & XILINX_CPM_PCIE_RPEFR_REQ_ID); /* NVMe: 에러 낸 EP BDF 출력 - NVMe 장치 식별 */
		pcie_write(port, XILINX_CPM_PCIE_RPEFR_ALL_MASK,
			   XILINX_CPM_PCIE_REG_RPEFR); /* NVMe: 에러 FIFO 클리어 */
	}
}

static void xilinx_cpm_mask_leg_irq(struct irq_data *data)
{
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(data); /* NVMe: INTx에 대응하는 RC 포트 획득 */
	unsigned long flags;
	u32 mask;
	u32 val;

	mask = BIT(data->hwirq + XILINX_CPM_PCIE_IDRN_SHIFT); /* NVMe: INTA~D 중 해당 INTx 비트 선택 */
	raw_spin_lock_irqsave(&port->lock, flags); /* NVMe: INTx mask 레지스터 접근 보호 */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IDRN_MASK); /* NVMe: 현재 INTx mask 읽기 */
	pcie_write(port, (val & (~mask)), XILINX_CPM_PCIE_REG_IDRN_MASK); /* NVMe: 해당 INTx 마스크 - NVMe 레거시 인터럽트 억제 */
	raw_spin_unlock_irqrestore(&port->lock, flags); /* NVMe: lock 해제 */
}

static void xilinx_cpm_unmask_leg_irq(struct irq_data *data)
{
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(data); /* NVMe: INTx에 대응하는 RC 포트 획득 */
	unsigned long flags;
	u32 mask;
	u32 val;

	mask = BIT(data->hwirq + XILINX_CPM_PCIE_IDRN_SHIFT); /* NVMe: INTA~D 중 해당 INTx 비트 선택 */
	raw_spin_lock_irqsave(&port->lock, flags); /* NVMe: INTx mask 레지스터 접근 보호 */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IDRN_MASK); /* NVMe: 현재 INTx mask 읽기 */
	pcie_write(port, (val | mask), XILINX_CPM_PCIE_REG_IDRN_MASK); /* NVMe: 해당 INTx 언마스크 - NVMe 레거시 인터럽트 허용 */
	raw_spin_unlock_irqrestore(&port->lock, flags); /* NVMe: lock 해제 */
}

static struct irq_chip xilinx_cpm_leg_irq_chip = {
	.name		= "INTx",          /* NVMe: INTx 인터럽트 chip 이름 */
	.irq_mask	= xilinx_cpm_mask_leg_irq,   /* NVMe: irq_mask 콜백 - NVMe INTA~D 끄기 */
	.irq_unmask	= xilinx_cpm_unmask_leg_irq, /* NVMe: irq_unmask 콜백 - NVMe INTA~D 켜기 */
};

/**
 * xilinx_cpm_pcie_intx_map - Set the handler for the INTx and mark IRQ as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: HW interrupt number
 *
 * Return: Always returns 0.
 */
/* NVMe: PCI INTx(INTA~D)를 Linux virq로 매핑 - NVMe MSI 미지원 시 레거시 인터럽트 경로 */
static int xilinx_cpm_pcie_intx_map(struct irq_domain *domain,
				    unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_cpm_leg_irq_chip,
				 handle_level_irq); /* NVMe: virq에 INTx chip과 level 핸들러 연결 */
	irq_set_chip_data(irq, domain->host_data); /* NVMe: virq가 RC 포트 구조체를 참조하도록 설정 */
	irq_set_status_flags(irq, IRQ_LEVEL); /* NVMe: level-triggered 인터럽트로 표시 - NVMe INTx 특성 */

	return 0;
}

/* INTx IRQ Domain operations */
/* NVMe: INTx domain ops - PCI INTx 4개(INTA~D)를 Linux IRQ 번호 공간으로 변환 */
static const struct irq_domain_ops intx_domain_ops = {
	.map = xilinx_cpm_pcie_intx_map,
};

static void xilinx_cpm_pcie_intx_flow(struct irq_desc *desc)
{
	struct xilinx_cpm_pcie *port = irq_desc_get_handler_data(desc); /* NVMe: chained handler용 RC 포트 */
	struct irq_chip *chip = irq_desc_get_chip(desc); /* NVMe: 상위 GIC/irqchip */
	unsigned long val;
	int i;

	chained_irq_enter(chip, desc); /* NVMe: 상위 IRQ chip mask/ack 시작 */

	val = FIELD_GET(XILINX_CPM_PCIE_IDRN_MASK,
			pcie_read(port, XILINX_CPM_PCIE_REG_IDRN)); /* NVMe: IDRN[19:16]에서 INTA~D 상태 추출 */

	for_each_set_bit(i, &val, PCI_NUM_INTX)
		generic_handle_domain_irq(port->intx_domain, i); /* NVMe: 활성 INTx마다 virq 핸들러 호출 - NVMe ISR로 연결 */

	chained_irq_exit(chip, desc); /* NVMe: 상위 IRQ chip unmask/종료 */
}

static void xilinx_cpm_mask_event_irq(struct irq_data *d)
{
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(d); /* NVMe: 이벤트 IRQ chip 데이터 획득 */
	u32 val;

	raw_spin_lock(&port->lock); /* NVMe: RC IMR 레지스터 접근 보호 */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IMR); /* NVMe: 현재 이벤트 마스크 읽기 */
	val &= ~BIT(d->hwirq); /* NVMe: 해당 이벤트 비트 마스크 */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IMR); /* NVMe: IMR 갱신 - 해당 RC 이벤트 차단 */
	raw_spin_unlock(&port->lock); /* NVMe: lock 해제 */
}

static void xilinx_cpm_unmask_event_irq(struct irq_data *d)
{
	struct xilinx_cpm_pcie *port = irq_data_get_irq_chip_data(d); /* NVMe: 이벤트 IRQ chip 데이터 획득 */
	u32 val;

	raw_spin_lock(&port->lock); /* NVMe: RC IMR 레지스터 접근 보호 */
	val = pcie_read(port, XILINX_CPM_PCIE_REG_IMR); /* NVMe: 현재 이벤트 마스크 읽기 */
	val |= BIT(d->hwirq); /* NVMe: 해당 이벤트 비트 언마스크 */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IMR); /* NVMe: IMR 갱신 - 해당 RC 이벤트 허용 */
	raw_spin_unlock(&port->lock); /* NVMe: lock 해제 */
}

static struct irq_chip xilinx_cpm_event_irq_chip = {
	.name		= "RC-Event",         /* NVMe: RC 이벤트 IRQ chip 이름 */
	.irq_mask	= xilinx_cpm_mask_event_irq,   /* NVMe: 이벤트 마스크 콜백 */
	.irq_unmask	= xilinx_cpm_unmask_event_irq, /* NVMe: 이벤트 언마스크 콜백 */
};

static int xilinx_cpm_pcie_event_map(struct irq_domain *domain,
				     unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_cpm_event_irq_chip,
				 handle_level_irq); /* NVMe: RC 이벤트 virq에 chip/handler 연결 */
	irq_set_chip_data(irq, domain->host_data); /* NVMe: virq가 RC 포트를 가리키도록 설정 */
	irq_set_status_flags(irq, IRQ_LEVEL); /* NVMe: level-triggered 표시 */
	return 0;
}

static const struct irq_domain_ops event_domain_ops = {
	.map = xilinx_cpm_pcie_event_map,
};

static void xilinx_cpm_pcie_event_flow(struct irq_desc *desc)
{
	struct xilinx_cpm_pcie *port = irq_desc_get_handler_data(desc); /* NVMe: RC 포트 획득 */
	struct irq_chip *chip = irq_desc_get_chip(desc); /* NVMe: 상위 IRQ chip */
	const struct xilinx_cpm_variant *variant = port->variant; /* NVMe: 버전별 레지스터 설정 */
	unsigned long val;
	int i;

	chained_irq_enter(chip, desc); /* NVMe: 상위 IRQ 진입 */
	val =  pcie_read(port, XILINX_CPM_PCIE_REG_IDR); /* NVMe: RC 이벤트 상태 읽기 */
	val &= pcie_read(port, XILINX_CPM_PCIE_REG_IMR); /* NVMe: 마스크된 비트 제외 - 실제 처리할 이벤트만 */
	for_each_set_bit(i, &val, 32)
		generic_handle_domain_irq(port->cpm_domain, i); /* NVMe: 활성 이벤트마다 virq 핸들러 호출 - AER/PME/INTx 처리 */
	pcie_write(port, val, XILINX_CPM_PCIE_REG_IDR); /* NVMe: 처리한 이벤트 클리어 */

	if (variant->ir_status) { /* NVMe: CPM5 버전이면 추가 local error status 존재 */
		val = readl_relaxed(port->cpm_base + variant->ir_status); /* NVMe: local error status 읽기 */
		if (val)
			writel_relaxed(val, port->cpm_base +
				       variant->ir_status); /* NVMe: local error status 클리어 */
	}

	/*
	 * XILINX_CPM_PCIE_MISC_IR_STATUS register is mapped to
	 * CPM SLCR block.
	 */
	val = readl_relaxed(port->cpm_base + XILINX_CPM_PCIE_MISC_IR_STATUS); /* NVMe: CPM SLCR misc 인터럽트 상태 읽기 */
	if (val)
		writel_relaxed(val,
			       port->cpm_base + XILINX_CPM_PCIE_MISC_IR_STATUS); /* NVMe: misc 인터럽트 클리어 */

	chained_irq_exit(chip, desc); /* NVMe: 상위 IRQ 종료 */
}

#define _IC(x, s)                              \
	[XILINX_PCIE_INTR_ ## x] = { __stringify(x), s } /* NVMe: 인터럽트 번호 -> 심볼/문자열 매핑 */

static const struct {
	const char      *sym; /* NVMe: 인터럽트 심볼 이름 - /proc/irq 등 디버깅 */
	const char      *str; /* NVMe: 인터럽트 설명 문자열 - 커널 로그 출력 */
} intr_cause[32] = {
	_IC(LINK_DOWN,		"Link Down"),          /* NVMe: 링크 다운 - NVMe SSD 물리적 분리 */
	_IC(HOT_RESET,		"Hot reset"),          /* NVMe: 핫 리셋 - NVMe 재열거/재초기화 */
	_IC(CFG_TIMEOUT,	"ECAM access timeout"), /* NVMe: ECAM 접근 타임아웃 - NVMe config 접근 실패 */
	_IC(CORRECTABLE,	"Correctable error message"), /* NVMe: PCIe AER correctable - NVMe 에러 보고 */
	_IC(NONFATAL,		"Non fatal error message"),   /* NVMe: PCIe AER non-fatal - NVMe 복구 가능 에러 */
	_IC(FATAL,		"Fatal error message"),       /* NVMe: PCIe AER fatal - NVMe 치명적 에러 */
	_IC(SLV_UNSUPP,		"Slave unsupported request"), /* NVMe: NVMe로부터 unsupported request */
	_IC(SLV_UNEXP,		"Slave unexpected completion"), /* NVMe: unexpected completion */
	_IC(SLV_COMPL,		"Slave completion timeout"),    /* NVMe: completion timeout - NVMe 응답 지연 */
	_IC(SLV_ERRP,		"Slave Error Poison"),          /* NVMe: poisoned TLP - NVMe 데이터 무결성 */
	_IC(SLV_CMPABT,		"Slave Completer Abort"),       /* NVMe: completer abort - NVMe 처리 거부 */
	_IC(SLV_ILLBUR,		"Slave Illegal Burst"),         /* NVMe: illegal burst */
	_IC(MST_DECERR,		"Master decode error"),         /* NVMe: RC master decode error - DMA 주소 문제 */
	_IC(MST_SLVERR,		"Master slave error"),          /* NVMe: RC master slave error */
	_IC(CFG_PCIE_TIMEOUT,	"PCIe ECAM access timeout"),    /* NVMe: PCIe ECAM timeout */
	_IC(CFG_ERR_POISON,	"ECAM poisoned completion received"), /* NVMe: ECAM read에서 poisoned completion */
	_IC(PME_TO_ACK_RCVD,	"PME_TO_ACK message received"), /* NVMe: NVMe의 PME_Turn_Off ack - 전력 관리 */
	_IC(PM_PME_RCVD,	"PM_PME message received"),     /* NVMe: NVMe로부터 PM_PME - wake 이벤트 */
	_IC(SLV_PCIE_TIMEOUT,	"PCIe completion timeout received"), /* NVMe: PCIe completion timeout */
};

static irqreturn_t xilinx_cpm_pcie_intr_handler(int irq, void *dev_id)
{
	struct xilinx_cpm_pcie *port = dev_id; /* NVMe: RC 포트 획득 */
	struct device *dev = port->dev; /* NVMe: device 포인터 - 로그 출력용 */
	struct irq_data *d;

	d = irq_domain_get_irq_data(port->cpm_domain, irq); /* NVMe: virq에 대응하는 hwirq 번호 획득 */

	switch (d->hwirq) { /* NVMe: 이벤트 종류별 분기 */
	case XILINX_PCIE_INTR_CORRECTABLE: /* NVMe: AER correctable - NVMe에서 보고 */
	case XILINX_PCIE_INTR_NONFATAL:    /* NVMe: AER non-fatal - NVMe에서 보고 */
	case XILINX_PCIE_INTR_FATAL:       /* NVMe: AER fatal - NVMe에서 보고 */
		cpm_pcie_clear_err_interrupts(port); /* NVMe: Root Port Error FIFO 클리어 - AER 상태 해제 */
		fallthrough; /* NVMe: 클리어 후에도 로그는 출력 */

	default:
		if (intr_cause[d->hwirq].str)
			dev_warn(dev, "%s\n", intr_cause[d->hwirq].str); /* NVMe: 이벤트 문자열 경고 출력 - NVMe 장치 문제 추적 */
		else
			dev_warn(dev, "Unknown IRQ %ld\n", d->hwirq); /* NVMe: 정의되지 않은 이벤트 */
	}

	return IRQ_HANDLED; /* NVMe: 이벤트 처리 완료 */
}

static void xilinx_cpm_free_irq_domains(struct xilinx_cpm_pcie *port)
{
	if (port->intx_domain) { /* NVMe: INTx domain 할당되어 있으면 */
		irq_domain_remove(port->intx_domain); /* NVMe: INTx IRQ domain 제거 - NVMe INTA~D 매핑 해제 */
		port->intx_domain = NULL; /* NVMe: dangling 방지 */
	}

	if (port->cpm_domain) { /* NVMe: RC event domain 할당되어 있으면 */
		irq_domain_remove(port->cpm_domain); /* NVMe: RC event IRQ domain 제거 */
		port->cpm_domain = NULL; /* NVMe: dangling 방지 */
	}
}

/**
 * xilinx_cpm_pcie_init_irq_domain - Initialize IRQ domain
 * @port: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
/* NVMe: INTx와 RC event용 IRQ domain 생성 - NVMe 레거시 인터럽트와 RC AER/PME 경로 준비 */
static int xilinx_cpm_pcie_init_irq_domain(struct xilinx_cpm_pcie *port)
{
	struct device *dev = port->dev; /* NVMe: device 포인터 */
	struct device_node *node = dev->of_node; /* NVMe: DT 노드 - interrupt-controller 자식 찾기 */
	struct device_node *pcie_intc_node;

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL); /* NVMe: DT의 interrupt-controller 자식 노드 획득 */
	if (!pcie_intc_node) { /* NVMe: interrupt-controller 노드 없으면 INTx 구성 불가 */
		dev_err(dev, "No PCIe Intc node found\n"); /* NVMe: NVMe 레거시 인터럽트 설정 불가 오류 */
		return -EINVAL;
	}

	port->cpm_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), 32,
						    &event_domain_ops, port); /* NVMe: 32개 RC event virq 공간 생성 - AER/PME/INTx routing */
	if (!port->cpm_domain)
		goto out; /* NVMe: domain 생성 실패 시 정리 */

	irq_domain_update_bus_token(port->cpm_domain, DOMAIN_BUS_NEXUS); /* NVMe: bus nexus로 표시 - RC 낮은 수준 이벤트 domain */

	port->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, port); /* NVMe: 4개 INTx(INTA~D) virq 공간 생성 - NVMe 레거시 INTx용 */
	if (!port->intx_domain)
		goto out; /* NVMe: domain 생성 실패 시 정리 */

	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED); /* NVMe: wired interrupt bus로 표시 - PCIe INTx 특성 */

	of_node_put(pcie_intc_node); /* NVMe: DT 노드 참조 카운트 감소 */
	raw_spin_lock_init(&port->lock); /* NVMe: RC 레지스터/INTx mask 보호용 lock 초기화 */

	return 0; /* NVMe: IRQ domain 초기화 성공 */
out:
	xilinx_cpm_free_irq_domains(port); /* NVMe: 실패 시 생성된 domain 정리 */
	of_node_put(pcie_intc_node); /* NVMe: DT 노드 참조 카운트 감소 */
	dev_err(dev, "Failed to allocate IRQ domains\n"); /* NVMe: domain 할당 실패 로그 */

	return -ENOMEM; /* NVMe: 메모리 부족 반환 */
}

static int xilinx_cpm_setup_irq(struct xilinx_cpm_pcie *port)
{
	struct device *dev = port->dev; /* NVMe: device 포인터 */
	struct platform_device *pdev = to_platform_device(dev); /* NVMe: platform device 포인터 */
	int i, irq;

	port->irq = platform_get_irq(pdev, 0); /* NVMe: RC의 단일 물리 IRQ 번호 획득 - 모든 RC 이벤트 집결점 */
	if (port->irq < 0)
		return port->irq; /* NVMe: IRQ 획득 실패 시 probe 중단 - NVMe 초기화 불가 */

	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) { /* NVMe: 32개 RC 이벤트별 virq 생성/요청 루프 */
		int err;

		if (!intr_cause[i].str) /* NVMe: 문자열이 없는 미사용 이벤트는 skip */
			continue;

		irq = irq_create_mapping(port->cpm_domain, i); /* NVMe: hwirq i -> virq 매핑 - AER/PME/INTx 이벤트별 ISR */
		if (!irq) { /* NVMe: virq 매핑 실패 */
			dev_err(dev, "Failed to map interrupt\n"); /* NVMe: NVMe 이벤트 IRQ 매핑 실패 */
			return -ENXIO;
		}

		err = devm_request_irq(dev, irq, xilinx_cpm_pcie_intr_handler,
				       0, intr_cause[i].sym, port); /* NVMe: 이벤트별 ISR 등록 - NVMe AER/PME 로깅 처리 */
		if (err) { /* NVMe: ISR 등록 실패 */
			dev_err(dev, "Failed to request IRQ %d\n", irq); /* NVMe: NVMe IRQ 등록 실패 */
			return err;
		}
	}

	port->intx_irq = irq_create_mapping(port->cpm_domain,
					    XILINX_PCIE_INTR_INTX); /* NVMe: INTx 집계 이벤트를 virq로 매핑 */
	if (!port->intx_irq) { /* NVMe: INTx virq 매핑 실패 */
		dev_err(dev, "Failed to map INTx interrupt\n"); /* NVMe: NVMe INTx 경로 구성 실패 */
		return -ENXIO;
	}

	/* Plug the INTx chained handler */
	irq_set_chained_handler_and_data(port->intx_irq,
					 xilinx_cpm_pcie_intx_flow, port); /* NVMe: INTx virq에 chained handler 연결 - NVMe INTA~D 분배 */

	/* Plug the main event chained handler */
	irq_set_chained_handler_and_data(port->irq,
					 xilinx_cpm_pcie_event_flow, port); /* NVMe: RC 물리 IRQ에 event flow 연결 - 모든 RC 이벤트 진입점 */

	return 0; /* NVMe: IRQ setup 성공 */
}

/**
 * xilinx_cpm_pcie_init_port - Initialize hardware
 * @port: PCIe port information
 */
/* NVMe: PCIe RC 하드웨어 초기화 - NVMe SSD와의 link 상태 확인 및 IRQ enable */
static void xilinx_cpm_pcie_init_port(struct xilinx_cpm_pcie *port)
{
	const struct xilinx_cpm_variant *variant = port->variant; /* NVMe: 버전별 설정 */

	if (variant->version == CPM5NC_HOST)
		return; /* NVMe: CPM5NC_HOST는 별도 초기화 생략 - IRQ domain 없음 */

	if (cpm_pcie_link_up(port)) /* NVMe: link up 상태 확인 */
		dev_info(port->dev, "PCIe Link is UP\n"); /* NVMe: NVMe SSD 연결됨 로그 */
	else
		dev_info(port->dev, "PCIe Link is DOWN\n"); /* NVMe: NVMe SSD 미연결/미검출 로그 */

	/* Disable all interrupts */
	pcie_write(port, ~XILINX_CPM_PCIE_IDR_ALL_MASK,
		   XILINX_CPM_PCIE_REG_IMR); /* NVMe: IMR를 0으로 - 초기화 중 RC 이벤트 모두 차단 */

	/* Clear pending interrupts */
	pcie_write(port, pcie_read(port, XILINX_CPM_PCIE_REG_IDR) &
		   XILINX_CPM_PCIE_IMR_ALL_MASK,
		   XILINX_CPM_PCIE_REG_IDR); /* NVMe: 초기화 전 pending RC 이벤트 클리어 */

	/*
	 * XILINX_CPM_PCIE_MISC_IR_ENABLE register is mapped to
	 * CPM SLCR block.
	 */
	writel(variant->ir_misc_value,
	       port->cpm_base + XILINX_CPM_PCIE_MISC_IR_ENABLE); /* NVMe: CPM SLCR misc 인터럽트 enable - RC local IRQ 허용 */

	if (variant->ir_enable) { /* NVMe: CPM5이면 추가 local error IRQ enable 레지스터 존재 */
		writel(XILINX_CPM_PCIE_IR_LOCAL,
		       port->cpm_base + variant->ir_enable); /* NVMe: per-host local error IRQ enable */
	}

	/* Set Bridge enable bit */
	pcie_write(port, pcie_read(port, XILINX_CPM_PCIE_REG_RPSC) |
		   XILINX_CPM_PCIE_REG_RPSC_BEN,
		   XILINX_CPM_PCIE_REG_RPSC); /* NVMe: RC bridge enable - NVMe PCIe 트래픽 포워딩 시작 */
}

/**
 * xilinx_cpm_pcie_parse_dt - Parse Device tree
 * @port: PCIe port information
 * @bus_range: Bus resource
 *
 * Return: '0' on success and error value on failure
 */
/* NVMe: DT에서 cpm_slcr/cfg 리소스 파싱 및 ECAM window 생성 - NVMe BAR/config 접근 기반 */
static int xilinx_cpm_pcie_parse_dt(struct xilinx_cpm_pcie *port,
				    struct resource *bus_range)
{
	struct device *dev = port->dev; /* NVMe: device 포인터 */
	struct platform_device *pdev = to_platform_device(dev); /* NVMe: platform device */
	struct resource *res;

	port->cpm_base = devm_platform_ioremap_resource_byname(pdev,
						       "cpm_slcr"); /* NVMe: CPM SLCR 공간 ioremap - misc/local IRQ 레지스터 접근 */
	if (IS_ERR(port->cpm_base))
		return PTR_ERR(port->cpm_base); /* NVMe: cpm_slcr 매핑 실패 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg"); /* NVMe: ECAM config space 리소스 획득 - NVMe config read/write에 사용 */
	if (!res)
		return -ENXIO; /* NVMe: cfg 리소스 없음 - NVMe 열거 불가 */

	port->cfg = pci_ecam_create(dev, res, bus_range,
				    &pci_generic_ecam_ops); /* NVMe: ECAM config window 생성 - NVMe SSD BDF 탐색, BAR/MSI-X 설정 */
	if (IS_ERR(port->cfg))
		return PTR_ERR(port->cfg); /* NVMe: ECAM 생성 실패 - NVMe 열거 불가 */

	if (port->variant->version == CPM5 ||
	    port->variant->version == CPM5_HOST1) { /* NVMe: CPM5는 별도 cpm_csr 공간 사용 */
		port->reg_base = devm_platform_ioremap_resource_byname(pdev,
							    "cpm_csr"); /* NVMe: cpm_csr 공간 ioremap - RC 브릿지 레지스터 접근 */
		if (IS_ERR(port->reg_base))
			return PTR_ERR(port->reg_base); /* NVMe: cpm_csr 매핑 실패 */
	} else {
		port->reg_base = port->cfg->win; /* NVMe: CPM1은 ECAM 윈도우 내에 RC 레지스터 존재 */
	}

	return 0; /* NVMe: DT 파싱 성공 */
}

static void xilinx_cpm_free_interrupts(struct xilinx_cpm_pcie *port)
{
	irq_set_chained_handler_and_data(port->intx_irq, NULL, NULL); /* NVMe: INTx chained handler 분리 */
	irq_set_chained_handler_and_data(port->irq, NULL, NULL);      /* NVMe: RC event chained handler 분리 */
}

/**
 * xilinx_cpm_pcie_probe - Probe function
 * @pdev: Platform device pointer
 *
 * Return: '0' on success and error value on failure
 */
/* NVMe: PCIe RC platform 드라이버 probe - NVMe SSD를 위한 host bridge 생성 및 열거 개시 */
static int xilinx_cpm_pcie_probe(struct platform_device *pdev)
{
	struct xilinx_cpm_pcie *port; /* NVMe: RC 포트 구조체 */
	struct device *dev = &pdev->dev; /* NVMe: platform device의 device - IOMMU/SMMU 연결 지점 */
	struct pci_host_bridge *bridge; /* NVMe: PCI host bridge - NVMe 장치를 PCI bus에 연결 */
	struct resource_entry *bus; /* NVMe: bus-range 리소스 엔트리 */
	int err;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*port)); /* NVMe: host bridge + port 구조체 할당 */
	if (!bridge)
		return -ENODEV; /* NVMe: host bridge 할당 실패 - NVMe 초기화 불가 */

	port = pci_host_bridge_priv(bridge); /* NVMe: bridge 뒤에 port private data 연결 */

	port->dev = dev; /* NVMe: device 포인터 저장 - DMA/IOMMU/로그용 */

	port->variant = of_device_get_match_data(dev); /* NVMe: DT compatible에 따른 CPM variant 선택 */

	if (port->variant->version != CPM5NC_HOST) { /* NVMe: CPM5NC_HOST 제외하고 IRQ domain 생성 */
		err = xilinx_cpm_pcie_init_irq_domain(port); /* NVMe: INTx/RC event IRQ domain 초기화 */
		if (err)
			return err; /* NVMe: IRQ domain 실패 - NVMe 인터럽트 없이는 동작 불가 */
	}

	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS); /* NVMe: DT bus-range에서 PCI bus 번호 범위 획득 */
	if (!bus) { /* NVMe: bus-range 없으면 */
		err = -ENODEV; /* NVMe: NVMe 열거를 위한 bus 번호 부족 */
		goto err_free_irq_domains; /* NVMe: 정리 */
	}

	err = xilinx_cpm_pcie_parse_dt(port, bus->res); /* NVMe: cpm_slcr/cfg ioremap 및 ECAM window 생성 */
	if (err) { /* NVMe: DT 파싱 실패 */
		dev_err(dev, "Parsing DT failed\n"); /* NVMe: NVMe RC 리소스 파싱 실패 */
		goto err_free_irq_domains; /* NVMe: 정리 */
	}

	xilinx_cpm_pcie_init_port(port); /* NVMe: RC 하드웨어 초기화 - link/IRQ/bridge enable */

	if (port->variant->version != CPM5NC_HOST) { /* NVMe: CPM5NC_HOST 제외하고 IRQ 등록 */
		err = xilinx_cpm_setup_irq(port); /* NVMe: RC 이벤트/INTx IRQ 등록 - NVMe 인터럽트 경로 활성화 */
		if (err) { /* NVMe: IRQ setup 실패 */
			dev_err(dev, "Failed to set up interrupts\n"); /* NVMe: NVMe 인터럽트 활성화 실패 */
			goto err_setup_irq; /* NVMe: 정리 */
		}
	}

	bridge->sysdata = port->cfg; /* NVMe: PCI config 접근용 sysdata 설정 - pci_bus->sysdata로 전달됨 */
	bridge->ops = (struct pci_ops *)&pci_generic_ecam_ops.pci_ops; /* NVMe: ECAM 기반 pci_ops 설정 - NVMe config read/write */

	err = pci_host_probe(bridge); /* NVMe: PCI host bridge 등록 및 버스 스캔 시작 - NVMe SSD 열거/바인딩 */
	if (err < 0)
		goto err_host_bridge; /* NVMe: PCI 버스 스캔 실패 - NVMe 검색 불가 */

	return 0; /* NVMe: probe 성공 - NVMe SSD가 PCIe bus에서 사용 가능 */

err_host_bridge:
	if (port->variant->version != CPM5NC_HOST)
		xilinx_cpm_free_interrupts(port); /* NVMe: chained handler 분리 */
err_setup_irq:
	pci_ecam_free(port->cfg); /* NVMe: ECAM window 해제 - NVMe config 접근 불가 */
err_free_irq_domains:
	if (port->variant->version != CPM5NC_HOST)
		xilinx_cpm_free_irq_domains(port); /* NVMe: IRQ domain 제거 - NVMe INTx/이벤트 매핑 해제 */
	return err; /* NVMe: probe 실패 반환 */
}

static const struct xilinx_cpm_variant cpm_host = {
	.version = CPM,                              /* NVMe: CPM 1세대 호스트 */
	.ir_misc_value = XILINX_CPM_PCIE0_MISC_IR_LOCAL, /* NVMe: CPM0 misc IRQ enable 값 */
};

static const struct xilinx_cpm_variant cpm5_host = {
	.version = CPM5,                             /* NVMe: CPM5 host0 */
	.ir_misc_value = XILINX_CPM_PCIE0_MISC_IR_LOCAL, /* NVMe: CPM0 misc IRQ enable 값 */
	.ir_status = XILINX_CPM_PCIE0_IR_STATUS,     /* NVMe: CPM0 local error status offset */
	.ir_enable = XILINX_CPM_PCIE0_IR_ENABLE,     /* NVMe: CPM0 local error enable offset */
};

static const struct xilinx_cpm_variant cpm5_host1 = {
	.version = CPM5_HOST1,                       /* NVMe: CPM5 host1 */
	.ir_misc_value = XILINX_CPM_PCIE1_MISC_IR_LOCAL, /* NVMe: CPM1 misc IRQ enable 값 */
	.ir_status = XILINX_CPM_PCIE1_IR_STATUS,     /* NVMe: CPM1 local error status offset */
	.ir_enable = XILINX_CPM_PCIE1_IR_ENABLE,     /* NVMe: CPM1 local error enable offset */
};

static const struct xilinx_cpm_variant cpm5n_host = {
	.version = CPM5NC_HOST,                      /* NVMe: CPM5 no-cache host - IRQ 생략 */
};

static const struct of_device_id xilinx_cpm_pcie_of_match[] = {
	{
		.compatible = "xlnx,versal-cpm-host-1.00", /* NVMe: Versal CPM host compatible */
		.data = &cpm_host,                         /* NVMe: cpm_host variant 연결 */
	},
	{
		.compatible = "xlnx,versal-cpm5-host",     /* NVMe: Versal CPM5 host0 compatible */
		.data = &cpm5_host,                        /* NVMe: cpm5_host variant 연결 */
	},
	{
		.compatible = "xlnx,versal-cpm5-host1",    /* NVMe: Versal CPM5 host1 compatible */
		.data = &cpm5_host1,                       /* NVMe: cpm5_host1 variant 연결 */
	},
	{
		.compatible = "xlnx,versal-cpm5nc-host",   /* NVMe: Versal CPM5NC host compatible */
		.data = &cpm5n_host,                       /* NVMe: cpm5n_host variant 연결 */
	},
	{}
};

static struct platform_driver xilinx_cpm_pcie_driver = {
	.driver = {
		.name = "xilinx-cpm-pcie",              /* NVMe: platform 드라이버 이름 */
		.of_match_table = xilinx_cpm_pcie_of_match, /* NVMe: DT compatible 매칭 테이블 */
		.suppress_bind_attrs = true,              /* NVMe: 사용자 공간 bind/unbind 금지 - RC 드라이버 안정성 */
	},
	.probe = xilinx_cpm_pcie_probe,               /* NVMe: RC probe 콜백 - NVMe host bridge 초기화 */
};

builtin_platform_driver(xilinx_cpm_pcie_driver);  /* NVMe: 빌트인 드라이버 등록 - 부팅 시 NVMe RC 초기화 */
