// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 */

#include <linux/align.h>			/* PCI/NVMe: 정렬 매크로, NVMe BAR/queue 정렬 시 사용 */
#include <linux/iopoll.h>			/* PCI/NVMe: 레지스터 폴링, NVMe 링크 업/다운 감지 시 사용 */
#include <linux/irqchip/chained_irq.h>	/* PCI/NVMe: 연결형 IRQ 진입, NVMe MSI-X/MSI 경로에 연결 */
#include <linux/irqchip/irq-msi-lib.h>	/* PCI/NVMe: MSI 라이브러리, NVMe 장치의 MSI 메시지 처리 지원 */
#include <linux/irqdomain.h>		/* PCI/NVMe: IRQ 도메인, NVMe MSI/MSI-X 벡터를 Linux IRQ에 매핑 */
#include <linux/msi.h>			/* PCI/NVMe: MSI/MSI-X API, NVMe가 요구하는 MSI-X 설정에 사용 */
#include <linux/of_address.h>		/* PCI/NVMe: DT 주소 파싱, NVMe 컨트롤러의 메모리 영역 확보 */
#include <linux/of_pci.h>		/* PCI/NVMe: DT PCIe 파싱, NVMe 장치 열거에 필요 */
#include <linux/pci_regs.h>		/* PCI/NVMe: PCI 레지스터 정의, NVMe PCIe capability 접근 */
#include <linux/platform_device.h>	/* PCI/NVMe: 플랫폼 디바이스, NVMe PCIe 호스트의 리소스 등록 */

#include "../../pci.h"			/* PCI/NVMe: PCI 핵심 헤더, NVMe 열거/바인딩 내포 */
#include "pcie-designware.h"		/* PCI/NVMe: DesignWare PCIe 정의, NVMe 호스트와의 연결점 */

static struct pci_ops dw_pcie_ops;		/* PCI/NVMe: RC 자체 config 접근 ops, NVMe 루트 포트 설정에 사용 */
static struct pci_ops dw_pcie_ecam_ops;		/* PCI/NVMe: ECAM config 접근 ops, NVMe 버스 스캔 시 사용 */
static struct pci_ops dw_child_pcie_ops;	/* PCI/NVMe: 하위 버스 config 접근 ops, NVMe 장치 탐색 시 사용 */

#ifdef CONFIG_SMP
static void dw_irq_noop(struct irq_data *d) { }	/* NVMe: SMP 시 irq_ack no-op, NVMe MSI affinity 이동 시 불필요한 ack 방지 */
#endif

static bool dw_pcie_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				      struct irq_domain *real_parent, struct msi_domain_info *info)
{
	/* PCI/NVMe: MSI 도메인 정보 초기화, NVMe 장치의 MSI/MSI-X 할당 전 설정 */
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;	/* NVMe: MSI 라이브러리 초기화 실패 시 NVMe MSI 할당 불가 */

#ifdef CONFIG_SMP
	info->chip->irq_ack = dw_irq_noop;	/* NVMe: SMP에서는 ack 무시, NVMe IRQ migration 중 이중 ack 방지 */
	info->chip->irq_pre_redirect = irq_chip_pre_redirect_parent;	/* NVMe: affinity 변경 전 parent ack, NVMe ISR 안정성 향상 */
#else
	info->chip->irq_ack = irq_chip_ack_parent;	/* NVMe: UP에서는 parent ack, NVMe MSI 수신 완료 처리 */
#endif
	return true;	/* NVMe: MSI chip 준비 완료, NVMe 장치에 MSI 메시지 전달 가능 */
}

#define DW_PCIE_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS		| \
				    MSI_FLAG_USE_DEF_CHIP_OPS		| \
				    MSI_FLAG_PCI_MSI_MASK_PARENT)	/* PCI/NVMe: 필수 MSI 플래그, NVMe MSI-X 사용 시 parent masking 지원 */
#define DW_PCIE_MSI_FLAGS_SUPPORTED (MSI_FLAG_MULTI_PCI_MSI		| \
				     MSI_FLAG_PCI_MSIX			| \
				     MSI_GENERIC_FLAGS_MASK)	/* PCI/NVMe: 지원 MSI 플래그, NVMe가 필요로 하는 다중 MSI/MSI-X 지원 */

#define IS_256MB_ALIGNED(x) IS_ALIGNED(x, SZ_256M)	/* PCI/NVMe: ECAM 256MB 정렬 검사, NVMe 장치의 256MB bus 단위 접근에 사용 */

static const struct msi_parent_ops dw_pcie_msi_parent_ops = {
	.required_flags		= DW_PCIE_MSI_FLAGS_REQUIRED,	/* NVMe: 필수 MSI 플래그, NVMe MSI 메시지 처리 보장 */
	.supported_flags	= DW_PCIE_MSI_FLAGS_SUPPORTED,	/* NVMe: NVMe MSI/MSI-X 기능 지원 선언 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,		/* NVMe: PCI MSI 버스 선택, NVMe MSI 도메인과 연결 */
	.prefix			= "DW-",			/* NVMe: IRQ 이름 prefix, NVMe MSI IRQ 식별에 도움 */
	.init_dev_msi_info	= dw_pcie_init_dev_msi_info,	/* NVMe: NVMe 장치별 MSI info 초기화 콜백 */
};

/* MSI int handler */
void dw_handle_msi_irq(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 인터럽트 핸들러, NVMe SSD가 발생시킨 MSI 메시지 처리 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie 구조체 획득, NVMe 호스트 레지스터 접근 기반 */
	unsigned int i, num_ctrls;			/* NVMe: MSI 컨트롤러 인덱스, NVMe MSI 벡터 그룹 관리 */

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 벡터당 컨트롤러 수, NVMe MSI-X 다중 벡터 지원 규모 계산 */

	for (i = 0; i < num_ctrls; i++) {	/* NVMe: 각 MSI 컨트롤러 순회, NVMe 다중 큐 MSI 벡터 처리 */
		unsigned int reg_off = i * MSI_REG_CTRL_BLOCK_SIZE;	/* NVMe: 상태 레지스터 오프셋, NVMe MSI 상태 확인 위치 */
		unsigned int irq_off = i * MAX_MSI_IRQS_PER_CTRL;	/* NVMe: IRQ 번호 오프셋, NVMe 큐별 IRQ 번호 계산 */
		unsigned long status, pos;				/* NVMe: MSI 상태/비트 위치, NVMe 활성 큐 식별 */

		status = dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_STATUS + reg_off);	/* NVMe: MSI 상태 레지스터 읽기, NVMe에서 복귀한 MSI TLP 감지 */
		if (!status)
			continue;	/* NVMe: 활성 MSI 없으면 다음 컨트롤러, NVMe 다른 큐 확인 */

		for_each_set_bit(pos, &status, MAX_MSI_IRQS_PER_CTRL)	/* NVMe: 활성 비트 순회, NVMe submission/completion 큐별 IRQ 분배 */
			generic_handle_demux_domain_irq(pp->irq_domain, irq_off + pos);	/* NVMe: Linux IRQ로 디먹스, NVMe 큐 ISR(nvme_irq) 호출 */
	}
}

/* Chained MSI interrupt service routine */
static void dw_chained_msi_isr(struct irq_desc *desc)
{
	/* NVMe: 연결형 MSI ISR, NVMe MSI 하드웨어 IRQ에서 상위 IRQ로 연결 */
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* NVMe: 상위 IRQ chip 획득, NVMe MSI 호스트 IRQ 제어 */
	struct dw_pcie_rp *pp;					/* NVMe: DesignWare RC 포트, NVMe 연결된 루트 포트 */

	chained_irq_enter(chip, desc);	/* NVMe: 연결형 IRQ 진입, NVMe MSI 처리 중 상위 mask 보장 */

	pp = irq_desc_get_handler_data(desc);	/* NVMe: 핸들러 데이터에서 RC 포트 획득, NVMe MSI 소속 포트 식별 */
	dw_handle_msi_irq(pp);			/* NVMe: 실제 MSI 처리, NVMe 큐별 인터럽트 디스패치 */

	chained_irq_exit(chip, desc);	/* NVMe: 연결형 IRQ 종료, NVMe MSI 처리 후 상위 unmask */
}

static void dw_pci_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	/* NVMe: MSI 메시지 구성, NVMe 장치가 MSI 메모리 쓰기할 주소/데이터 설정 */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: IRQ chip 데이터에서 RC 포트, NVMe MSI 소속 확인 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie 획득, NVMe 호스트 레지스터 접근 */
	u64 msi_target = (u64)pp->msi_data;			/* NVMe: MSI 타겟 주소, NVMe가 쓸 메모리 주소 */

	msg->address_lo = lower_32_bits(msi_target);	/* NVMe: MSI 주소 하위 32비트, 32-bit NVMe 장치 전달 */
	msg->address_hi = upper_32_bits(msi_target);	/* NVMe: MSI 주소 상위 32비트, 64-bit NVMe 장치 전달 */
	msg->data = d->hwirq;				/* NVMe: MSI 데이터, NVMe 큐별 벡터 번호 */

	dev_dbg(pci->dev, "msi#%d address_hi %#x address_lo %#x\n",
		(int)d->hwirq, msg->address_hi, msg->address_lo);	/* NVMe: 디버그 로그, NVMe MSI 메시지 트레이스 */
}

static void dw_pci_bottom_mask(struct irq_data *d)
{
	/* NVMe: MSI 벡터 마스크, NVMe 큐 ISR 일시 비활성화(예: MSI-X per-vector mask) */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;				/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	guard(raw_spinlock)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI mask 레지스터 동시 접근 보호 */
	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터가 속한 MSI 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: MASK 레지스터 오프셋, NVMe 벡터 마스크 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐에 대응하는 MSI 벡터 */

	pp->irq_mask[ctrl] |= BIT(bit);	/* NVMe: 소프트웨어 마스크 비트 설정, NVMe 해당 큐 MSI 차단 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + res, pp->irq_mask[ctrl]);	/* NVMe: 하드웨어 마스크 레지스터 갱신, NVMe MSI 수신 차단 */
}

static void dw_pci_bottom_unmask(struct irq_data *d)
{
	/* NVMe: MSI 벡터 언마스크, NVMe 큐 ISR 다시 활성화 */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;				/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	guard(raw_spinlock)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI unmask 동시 접근 보호 */
	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: MASK 레지스터 오프셋, NVMe 벡터 언마스크 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐 대응 벡터 */

	pp->irq_mask[ctrl] &= ~BIT(bit);	/* NVMe: 소프트웨어 마스크 비트 해제, NVMe 해당 큐 MSI 허용 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + res, pp->irq_mask[ctrl]);	/* NVMe: 하드웨어 마스크 레지스터 갱신, NVMe MSI 수신 허용 */
}

static void dw_pci_bottom_ack(struct irq_data *d)
{
	/* NVMe: MSI 상태 비트 클리어, NVMe MSI 처리 완료 알림 */
	struct dw_pcie_rp *pp  = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);			/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;					/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: STATUS 레지스터 오프셋, NVMe MSI 완료 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐 대응 벡터 */

	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_STATUS + res, BIT(bit));	/* NVMe: 해당 MSI 상태 비트 클리어, NVMe 다음 MSI 준비 */
}

static struct irq_chip dw_pci_msi_bottom_irq_chip = {
	.name			= "DWPCI-MSI",		/* NVMe: chip 이름, NVMe MSI IRQ 식별 */
	.irq_compose_msi_msg	= dw_pci_setup_msi_msg,	/* NVMe: MSI 메시지 구성, NVMe 장치에 전달할 주소/데이터 */
	.irq_mask		= dw_pci_bottom_mask,	/* NVMe: 벡터 마스크, NVMe MSI-X per-vector mask 대응 */
	.irq_unmask		= dw_pci_bottom_unmask,	/* NVMe: 벡터 언마스크, NVMe 큐 ISR 활성화 */
#ifdef CONFIG_SMP
	.irq_ack		= dw_irq_noop,		/* NVMe: SMP ack no-op, NVMe affinity 이동 시 parent ack 사용 */
	.irq_pre_redirect	= dw_pci_bottom_ack,	/* NVMe: affinity 변경 전 ack, NVMe ISR 안정성 */
	.irq_set_affinity	= irq_chip_redirect_set_affinity,	/* NVMe: affinity 재지정, NVMe 큐 IRQ CPU 이동 */
#else
	.irq_ack		= dw_pci_bottom_ack,	/* NVMe: UP에서는 bottom ack, NVMe MSI 처리 완료 */
#endif
};

static int dw_pcie_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs, void *args)
{
	/* NVMe: MSI IRQ 도메인 할당, NVMe 큐당 MSI/MSI-X 벡터 배정 */
	struct dw_pcie_rp *pp = domain->host_data;	/* NVMe: 호스트 데이터에서 RC 포트, NVMe 연결 포트 */
	int bit;					/* NVMe: 할당된 MSI 비트, NVMe 큐 벡터 번호 */

	scoped_guard (raw_spinlock_irq, &pp->lock) {	/* NVMe: RC 포트 lock, NVMe MSI 벡터 할당 동시 접근 보호 */
		bit = bitmap_find_free_region(pp->msi_irq_in_use, pp->num_vectors,
					      order_base_2(nr_irqs));	/* NVMe: 연속된 비트 영역 탐색, NVMe 다중 MSI/MSI-X 벡터 그룹 할당 */
	}

	if (bit < 0)
		return -ENOSPC;	/* NVMe: MSI 벡터 고갈, NVMe 큐 수 초과 시 실패 */

	for (unsigned int i = 0; i < nr_irqs; i++) {	/* NVMe: 요청된 수만큼 벡터 설정, NVMe submission/completion 큐별 할당 */
		irq_domain_set_info(domain, virq + i, bit + i, pp->msi_irq_chip,
				    pp, handle_edge_irq, NULL, NULL);	/* NVMe: virq-hwirq 매핑, NVMe 큐 ISR 등록 */
	}
	return 0;	/* NVMe: MSI 벡터 할당 성공, NVMe 큐 인터럽트 준비 완료 */
}

static void dw_pcie_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs)
{
	/* NVMe: MSI IRQ 도메인 해제, NVMe 큐 제거 시 벡터 반납 */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);	/* NVMe: 첫 virq의 irq_data, NVMe 첫 큐 벡터 정보 */
	struct dw_pcie_rp *pp = domain->host_data;			/* NVMe: RC 포트, NVMe 연결 포트 */

	guard(raw_spinlock_irq)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI 벡터 해제 동시 접근 보호 */
	bitmap_release_region(pp->msi_irq_in_use, d->hwirq, order_base_2(nr_irqs));	/* NVMe: 벡터 비트맵 반납, NVMe 큐 벡터 재사용 가능 */
}

static const struct irq_domain_ops dw_pcie_msi_domain_ops = {
	.alloc	= dw_pcie_irq_domain_alloc,	/* NVMe: MSI 벡터 할당, NVMe 큐당 IRQ 생성 */
	.free	= dw_pcie_irq_domain_free,	/* NVMe: MSI 벡터 해제, NVMe 큐 제거 시 */
};

int dw_pcie_allocate_domains(struct dw_pcie_rp *pp)
{
	/* NVMe: MSI parent IRQ 도메인 생성, NVMe 장치의 MSI/MSI-X 할당 인프라 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(pci->dev),	/* NVMe: firmware node, NVMe MSI 도메인과 DT 연결 */
		.ops		= &dw_pcie_msi_domain_ops,	/* NVMe: MSI 도메인 ops, NVMe 벡터 alloc/free */
		.size		= pp->num_vectors,		/* NVMe: 최대 MSI 벡터 수, NVMe 큐 수 제한 */
		.host_data	= pp,				/* NVMe: RC 포트를 host data로, NVMe MSI 핸들러에서 포트 식별 */
	};

	pp->irq_domain = msi_create_parent_irq_domain(&info, &dw_pcie_msi_parent_ops);	/* NVMe: parent IRQ 도메인 생성, NVMe MSI 하드웨어 추상화 */
	if (!pp->irq_domain) {
		dev_err(pci->dev, "Failed to create IRQ domain\n");	/* NVMe: 도메인 생성 실패, NVMe MSI 할당 불가 */
		return -ENOMEM;	/* NVMe: 메모리 부족, NVMe 초기화 실패 */
	}

	return 0;	/* NVMe: IRQ 도메인 생성 완료, NVMe MSI 사용 가능 */
}
EXPORT_SYMBOL_GPL(dw_pcie_allocate_domains);

void dw_pcie_free_msi(struct dw_pcie_rp *pp)
{
	/* NVMe: MSI 인프라 해제, NVMe 장치 제거/호스트 종료 시 */
	u32 ctrl;	/* NVMe: MSI 컨트롤러 인덱스, NVMe 다중 MSI IRQ 해제 */

	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++) {	/* NVMe: 각 MSI 컨트롤러 순회, NVMe 연결된 모든 MSI IRQ */
		if (pp->msi_irq[ctrl] > 0)
			irq_set_chained_handler_and_data(pp->msi_irq[ctrl], NULL, NULL);	/* NVMe: 연결형 핸들러 제거, NVMe MSI IRQ 분리 */
	}

	irq_domain_remove(pp->irq_domain);	/* NVMe: MSI IRQ 도메인 제거, NVMe 벡터 매핑 해제 */
}
EXPORT_SYMBOL_GPL(dw_pcie_free_msi);

void dw_pcie_msi_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 하드웨어 초기화, NVMe 장치가 MSI TLP 본문 전송 가능하도록 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u64 msi_target = (u64)pp->msi_data;		/* NVMe: MSI 타겟 주소, NVMe가 메모리 쓰기할 주소 */
	u32 ctrl, num_ctrls;				/* NVMe: 컨트롤러 인덱스/개수, NVMe MSI 벡터 그룹 */

	if (!pci_msi_enabled() || !pp->use_imsi_rx)
		return;	/* NVMe: MSI 미지원 또는 외부 MSI 컨트롤러 사용 시, NVMe MSI 초기화 불필요 */

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 초기화할 컨트롤러 수, NVMe MSI 벡터 그룹 계산 */

	/* Initialize IRQ Status array */
	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {	/* NVMe: 각 MSI 컨트롤러 초기화, NVMe MSI 수신 가능 상태 설정 */
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK +
				    (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
				    pp->irq_mask[ctrl]);	/* NVMe: 마스크 레지스터 설정, NVMe MSI 벡터 마스크 초기 상태 */
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_ENABLE +
				    (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
				    ~0);	/* NVMe: MSI 인터럽트 활성화, NVMe MSI 수신 가능 */
	}

	/* Program the msi_data */
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_LO, lower_32_bits(msi_target));	/* NVMe: MSI 타겟 주소 LO, 32-bit NVMe 장치 전달 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_HI, upper_32_bits(msi_target));	/* NVMe: MSI 타겟 주소 HI, 64-bit NVMe 장치 전달 */
}
EXPORT_SYMBOL_GPL(dw_pcie_msi_init);

static int dw_pcie_parse_split_msi_irq(struct dw_pcie_rp *pp)
{
	/* NVMe: DT의 분할 MSI IRQ 파싱, NVMe 큐 수가 많을 때 다중 MSI IRQ 사용 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 호스트 리소스 */
	u32 ctrl, max_vectors;				/* NVMe: 컨트롤러 인덱스/최대 벡터, NVMe MSI 규모 */
	int irq;					/* NVMe: 파싱된 IRQ 번호, NVMe MSI 하드웨어 IRQ */

	/* Parse any "msiX" IRQs described in the devicetree */
	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++) {	/* NVMe: msi0..msiN 순회, NVMe 다중 MSI IRQ 등록 */
		char msi_name[] = "msiX";		/* NVMe: IRQ 이름 버퍼, NVMe MSI IRQ 이름 구성 */

		msi_name[3] = '0' + ctrl;	/* NVMe: msi0/msi1/... 이름 생성, NVMe MSI 컨트롤러별 IRQ */
		irq = platform_get_irq_byname_optional(pdev, msi_name);	/* NVMe: 이름으로 IRQ 획득, NVMe MSI 하드웨어 라인 */
		if (irq == -ENXIO)
			break;	/* NVMe: 더 이상 msiX 없음, NVMe MSI IRQ 파싱 종료 */
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "Failed to parse MSI IRQ '%s'\n",
					     msi_name);	/* NVMe: IRQ 파싱 오류, NVMe 초기화 실패 */

		pp->msi_irq[ctrl] = irq;	/* NVMe: 컨트롤러 IRQ 저장, NVMe MSI 인터럽트 라인 등록 */
	}

	/* If no "msiX" IRQs, caller should fallback to "msi" IRQ */
	if (ctrl == 0)
		return -ENXIO;	/* NVMe: 분할 MSI 없음, NVMe 단일 "msi" IRQ로 폴백 */

	max_vectors = ctrl * MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 사용 가능한 최대 벡터 수, NVMe 큐 수 상한 */
	if (pp->num_vectors > max_vectors) {
		dev_warn(dev, "Exceeding number of MSI vectors, limiting to %u\n",
			 max_vectors);	/* NVMe: 요청 벡터 초과 경고, NVMe 큐 수 제한 */
		pp->num_vectors = max_vectors;	/* NVMe: 벡터 수 제한, NVMe 큐 수 조정 */
	}
	if (!pp->num_vectors)
		pp->num_vectors = max_vectors;	/* NVMe: 기본값으로 최대 벡터, NVMe 큐 수 자동 설정 */

	return 0;	/* NVMe: 분할 MSI 파싱 완료, NVMe 다중 MSI IRQ 준비 */
}

int dw_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 호스트 초기화, NVMe 장치가 사용할 MSI/MSI-X 인프라 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 호스트 리소스 */
	u64 *msi_vaddr = NULL;				/* NVMe: MSI 주소 가상 주소, NVMe MSI 타겟 메모리 */
	int ret;					/* NVMe: 반환값, NVMe 초기화 결과 */
	u32 ctrl, num_ctrls;				/* NVMe: 컨트롤러 인덱스/개수, NVMe MSI 그룹 */

	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++)
		pp->irq_mask[ctrl] = ~0;	/* NVMe: 초기 마스크 전체 설정, NVMe MSI 벡터 기본 차단 */

	if (!pp->msi_irq[0]) {
		ret = dw_pcie_parse_split_msi_irq(pp);	/* NVMe: 분할 MSI IRQ 파싱, NVMe 다중 MSI IRQ 지원 */
		if (ret < 0 && ret != -ENXIO)
			return ret;	/* NVMe: 치명적 오류, NVMe MSI 초기화 중단 */
	}

	if (!pp->num_vectors)
		pp->num_vectors = MSI_DEF_NUM_VECTORS;	/* NVMe: 기본 벡터 수, NVMe 큐 수 기본값 */
	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 수, NVMe MSI 그룹 계산 */

	if (!pp->msi_irq[0]) {
		pp->msi_irq[0] = platform_get_irq_byname_optional(pdev, "msi");	/* NVMe: "msi" 이름 IRQ 획득, NVMe MSI 하드웨어 라인 */
		if (pp->msi_irq[0] < 0) {
			pp->msi_irq[0] = platform_get_irq(pdev, 0);	/* NVMe: 이름 없으면 인덱스 0, NVMe MSI 폴백 */
			if (pp->msi_irq[0] < 0)
				return pp->msi_irq[0];	/* NVMe: MSI IRQ 없음, NVMe MSI 사용 불가 */
		}
	}

	dev_dbg(dev, "Using %d MSI vectors\n", pp->num_vectors);	/* NVMe: 사용 벡터 수 로그, NVMe 큐 수 트레이스 */

	pp->msi_irq_chip = &dw_pci_msi_bottom_irq_chip;	/* NVMe: bottom IRQ chip 연결, NVMe MSI mask/ack/unmask */

	ret = dw_pcie_allocate_domains(pp);	/* NVMe: MSI IRQ 도메인 생성, NVMe 벡터 할당 준비 */
	if (ret)
		return ret;	/* NVMe: 도메인 생성 실패, NVMe MSI 초기화 중단 */

	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {	/* NVMe: 각 MSI 컨트롤러에 연결형 핸들러 등록, NVMe MSI IRQ 라우팅 */
		if (pp->msi_irq[ctrl] > 0)
			irq_set_chained_handler_and_data(pp->msi_irq[ctrl],
						    dw_chained_msi_isr, pp);	/* NVMe: 하드웨어 IRQ에서 MSI ISR 연결, NVMe 인터럽트 디스패치 */
	}

	/*
	 * Even though the iMSI-RX Module supports 64-bit addresses some
	 * peripheral PCIe devices may lack 64-bit message support. In
	 * order not to miss MSI TLPs from those devices the MSI target
	 * address has to be within the lowest 4GB.
	 *
	 * Per DWC databook r6.21a, section 3.10.2.3, the incoming MWr TLP
	 * targeting the MSI_CTRL_ADDR is terminated by the iMSI-RX and never
	 * appears on the AXI bus. So MSI_CTRL_ADDR address doesn't need to be
	 * mapped and can be any memory that doesn't get allocated for the BAR
	 * memory. Since most of the platforms provide 32-bit address for
	 * 'config' region, try cfg0_base as the first option for the MSI target
	 * address if it's a 32-bit address. Otherwise, try 32-bit and 64-bit
	 * coherent memory allocation one by one.
	 */
	if (!(pp->cfg0_base & GENMASK_ULL(63, 32))) {
		pp->msi_data = pp->cfg0_base;	/* NVMe: 32-bit cfg0_base를 MSI 주소로 재사용, NVMe 32-bit MSI 장치 지원 */
		return 0;	/* NVMe: MSI 주소 설정 완료, NVMe MSI TLP 수신 준비 */
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));	/* NVMe: 32-bit coherent DMA 마스크 시도, NVMe 32-bit MSI 주소 할당 */
	if (!ret)
		msi_vaddr = dmam_alloc_coherent(dev, sizeof(u64), &pp->msi_data,
						GFP_KERNEL);	/* NVMe: 32-bit coherent MSI 주소 할당, NVMe MSI 타겟 메모리 */

	if (!msi_vaddr) {
		dev_warn(dev, "Failed to allocate 32-bit MSI address\n");	/* NVMe: 32-bit 할당 실패, NVMe 64-bit MSI 장치로 폴백 */
		dma_set_coherent_mask(dev, DMA_BIT_MASK(64));	/* NVMe: 64-bit coherent 마스크, NVMe 64-bit MSI 주소 준비 */
		msi_vaddr = dmam_alloc_coherent(dev, sizeof(u64), &pp->msi_data,
						GFP_KERNEL);	/* NVMe: 64-bit coherent MSI 주소 할당, NVMe MSI 타겟 메모리 */
		if (!msi_vaddr) {
			dev_err(dev, "Failed to allocate MSI address\n");	/* NVMe: MSI 주소 할당 실패, NVMe MSI 사용 불가 */
			dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 초기화 롤백 */
			return -ENOMEM;	/* NVMe: 메모리 부족, NVMe 초기화 실패 */
		}
	}

	return 0;	/* NVMe: MSI 호스트 초기화 완료, NVMe 장치 MSI 사용 가능 */
}
EXPORT_SYMBOL_GPL(dw_pcie_msi_host_init);

static void dw_pcie_host_request_msg_tlp_res(struct dw_pcie_rp *pp)
{
	/* NVMe: MSG TLP 전송용 메모리 영역 할당, NVMe PME/PM 메시지 전송 시 사용 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct resource_entry *win;			/* NVMe: 호스트 브리지 윈도우, NVMe 메모리 공간 */
	struct resource *res;				/* NVMe: 할당된 MSG 자원, NVMe PME TLP 주소 */

	win = resource_list_first_type(&pp->bridge->windows, IORESOURCE_MEM);	/* NVMe: 첫 MEM 윈도우 획득, NVMe 메모리 공간 내 MSG 영역 배치 */
	if (win) {
		res = devm_kzalloc(pci->dev, sizeof(*res), GFP_KERNEL);	/* NVMe: MSG 자원 구조체 할당, NVMe PME TLP 리소스 */
		if (!res)
			return;	/* NVMe: 메모리 부족, NVMe MSG TLP 기능 비활성화 */

		/*
		 * Allocate MSG TLP region of size 'region_align' at the end of
		 * the host bridge window.
		 */
		res->start = win->res->end - pci->region_align + 1;	/* NVMe: MSG 영역 시작, NVMe PME TLP 주소 범위 */
		res->end = win->res->end;				/* NVMe: MSG 영역 끝, NVMe PME TLP 주소 범위 */
		res->name = "msg";					/* NVMe: 자원 이름, NVMe PME/MSG TLP 식별 */
		res->flags = win->res->flags | IORESOURCE_BUSY;	/* NVMe: BUSY 플래그, NVMe BAR 할당에서 제외 */

		if (!devm_request_resource(pci->dev, win->res, res))
			pp->msg_res = res;	/* NVMe: MSG 자원 등록 성공, NVMe PME TLP 전송 가능 */
	}
}

static int dw_pcie_config_ecam_iatu(struct dw_pcie_rp *pp)
{
	/* NVMe: ECAM 모드에서 iATU CFG 설정, NVMe 버스/장치 config 접근 경로 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = {0};		/* NVMe: outbound ATU 설정, NVMe config 트랜잭션 경로 */
	resource_size_t bus_range_max;			/* NVMe: 버스 범위 최대, NVMe 탐색 버스 수 */
	struct resource_entry *bus;			/* NVMe: 버스 범위 엔트리, NVMe PCIe 버스 범위 */
	int ret;					/* NVMe: 반환값, NVMe iATU 프로그래밍 결과 */

	bus = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS);	/* NVMe: 버스 범위 획득, NVMe 장치가 속한 PCIe 버스 범위 */

	/*
	 * Root bus under the host bridge doesn't require any iATU configuration
	 * as DBI region will be used to access root bus config space.
	 * Immediate bus under Root Bus, needs type 0 iATU configuration and
	 * remaining buses need type 1 iATU configuration.
	 */
	atu.index = 0;				/* NVMe: outbound iATU 인덱스 0, NVMe type 0 config 전용 */
	atu.type = PCIE_ATU_TYPE_CFG0;		/* NVMe: type 0 config, NVMe 루트 버스 하위 장치 접근 */
	atu.parent_bus_addr = pp->cfg0_base + SZ_1M;	/* NVMe: CPU 주소, NVMe config space 매핑 */
	/* 1MiB is to cover 1 (bus) * 32 (devices) * 8 (functions) */
	atu.size = SZ_1M;				/* NVMe: 1MB 크기, NVMe 1 bus * 32 dev * 8 fn 커버 */
	atu.ctrl2 = PCIE_ATU_CFG_SHIFT_MODE_ENABLE;	/* NVMe: CFG shift 모드, NVMe BDF 자동 변환 */
	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: iATU 프로그램, NVMe type 0 config 경로 활성화 */
	if (ret)
		return ret;	/* NVMe: iATU 설정 실패, NVMe config 접근 불가 */

	bus_range_max = resource_size(bus->res);	/* NVMe: 버스 범위 크기, NVMe 탐색할 버스 수 */

	if (bus_range_max < 2)
		return 0;	/* NVMe: 버스 범위가 1개면 type 1 불필요, NVMe 추가 버스 없음 */

	/* Configure remaining buses in type 1 iATU configuration */
	atu.index = 1;				/* NVMe: outbound iATU 인덱스 1, NVMe type 1 config 전용 */
	atu.type = PCIE_ATU_TYPE_CFG1;		/* NVMe: type 1 config, NVMe 다중 버스 하위 장치 접근 */
	atu.parent_bus_addr = pp->cfg0_base + SZ_2M;	/* NVMe: CPU 주소, NVMe type 1 config space 시작 */
	atu.size = (SZ_1M * bus_range_max) - SZ_2M;	/* NVMe: 나머지 버스 커버, NVMe 추가 버스 config 접근 */
	atu.ctrl2 = PCIE_ATU_CFG_SHIFT_MODE_ENABLE;	/* NVMe: CFG shift 모드, NVMe BDF 자동 변환 */

	return dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: iATU 프로그램, NVMe type 1 config 경로 활성화 */
}

static int dw_pcie_create_ecam_window(struct dw_pcie_rp *pp, struct resource *res)
{
	/* NVMe: ECAM config 윈도우 생성, NVMe 버스 스캔 시 ECAM 기반 config 접근 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct resource_entry *bus;			/* NVMe: 버스 범위 엔트리, NVMe PCIe 버스 범위 */

	bus = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS);	/* NVMe: 버스 범위 획득, NVMe 장치 버스 범위 */
	if (!bus)
		return -ENODEV;	/* NVMe: 버스 범위 없음, NVMe PCIe 열거 불가 */

	pp->cfg = pci_ecam_create(dev, res, bus->res, &pci_generic_ecam_ops);	/* NVMe: ECAM 윈도우 생성, NVMe config space 매핑 */
	if (IS_ERR(pp->cfg))
		return PTR_ERR(pp->cfg);	/* NVMe: ECAM 생성 실패, NVMe config 접근 불가 */

	return 0;	/* NVMe: ECAM 윈도우 생성 완료, NVMe 버스 스캔 가능 */
}

static bool dw_pcie_ecam_enabled(struct dw_pcie_rp *pp, struct resource *config_res)
{
	/* NVMe: ECAM 사용 가능 여부 판단, NVMe config 접근 방식 결정 */
	struct resource *bus_range;	/* NVMe: 버스 범위, NVMe PCIe 버스 범위 */
	u64 nr_buses;			/* NVMe: config 리소스의 버스 수, NVMe 탐색 가능 버스 수 */

	/* Vendor glue drivers may implement their own ECAM mechanism */
	if (pp->native_ecam)
		return false;	/* NVMe: vendor ECAM 사용 시 generic ECAM 불필요, NVMe config 접근 vendor 담당 */

	/*
	 * PCIe spec r6.0, sec 7.2.2 mandates the base address used for ECAM to
	 * be aligned on a 2^(n+20) byte boundary, where n is the number of bits
	 * used for representing 'bus' in BDF. Since the DWC cores always use 8
	 * bits for representing 'bus', the base address has to be aligned to
	 * 2^28 byte boundary, which is 256 MiB.
	 */
	if (!IS_256MB_ALIGNED(config_res->start))
		return false;	/* NVMe: 256MB 미정렬, NVMe ECAM 사용 불가, iATU 사용 */

	bus_range = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS)->res;
	if (!bus_range)
		return false;	/* NVMe: 버스 범위 없음, NVMe ECAM 사용 불가 */

	nr_buses = resource_size(config_res) >> PCIE_ECAM_BUS_SHIFT;	/* NVMe: config 리소스 크기에서 버스 수, NVMe ECAM 커버 버스 수 */

	return nr_buses >= resource_size(bus_range);	/* NVMe: ECAM이 버스 범위 커버 시 true, NVMe ECAM 기반 열거 결정 */
}

static int dw_pcie_host_get_resources(struct dw_pcie_rp *pp)
{
	/* NVMe: 호스트 리소스 파싱/매핑, NVMe PCIe 호스트의 메모리/config/IO 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 리소스 */
	struct resource_entry *win;			/* NVMe: 리소스 엔트리, NVMe 메모리/IO 윈도우 */
	struct resource *res;				/* NVMe: 플랫폼 리소스, NVMe config 공간 */
	int ret;					/* NVMe: 반환값, NVMe 리소스 설정 결과 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "config");	/* NVMe: "config" 메모리 리소스 획득, NVMe PCIe config 공간 */
	if (!res) {
		dev_err(dev, "Missing \"config\" reg space\n");	/* NVMe: config 리소스 누락, NVMe PCIe 초기화 불가 */
		return -ENODEV;	/* NVMe: 리소스 부재, NVMe 호스트 초기화 실패 */
	}

	pp->cfg0_size = resource_size(res);	/* NVMe: config 공간 크기, NVMe config 접근 범위 */
	pp->cfg0_base = res->start;		/* NVMe: config 공간 물리 주소, NVMe config mapping 기준 */

	pp->ecam_enabled = dw_pcie_ecam_enabled(pp, res);	/* NVMe: ECAM 사용 여부, NVMe config 접근 방식 */
	if (pp->ecam_enabled) {
		ret = dw_pcie_create_ecam_window(pp, res);	/* NVMe: ECAM 윈도우 생성, NVMe config ECAM 매핑 */
		if (ret)
			return ret;	/* NVMe: ECAM 생성 실패, NVMe config 접근 불가 */

		pp->bridge->ops = &dw_pcie_ecam_ops;	/* NVMe: ECAM config ops 설정, NVMe 버스 스캔 */
		pp->bridge->sysdata = pp->cfg;		/* NVMe: ECAM sysdata, NVMe config 접근 컨텍스트 */
		pp->cfg->priv = pp;			/* NVMe: dw_pcie_rp 역참조, NVMe ECAM->host 매핑 */
	} else {
		pp->va_cfg0_base = devm_pci_remap_cfg_resource(dev, res);	/* NVMe: config 공간 가상 매핑, NVMe config 레지스터 직접 접근 */
		if (IS_ERR(pp->va_cfg0_base))
			return PTR_ERR(pp->va_cfg0_base);	/* NVMe: 매핑 실패, NVMe config 접근 불가 */

		/* Set default bus ops */
		pp->bridge->ops = &dw_pcie_ops;		/* NVMe: 기본 RC config ops, NVMe 루트 포트 config 접근 */
		pp->bridge->child_ops = &dw_child_pcie_ops;	/* NVMe: 하위 버스 config ops, NVMe 장치 탐색 */
		pp->bridge->sysdata = pp;			/* NVMe: RC 포트 sysdata, NVMe config 접근 컨텍스트 */
	}

	ret = dw_pcie_get_resources(pci);	/* NVMe: DesignWare 공통 리소스 획득, NVMe 호스트 DBI/ATU 등 */
	if (ret) {
		if (pp->cfg)
			pci_ecam_free(pp->cfg);	/* NVMe: ECAM 해제, NVMe 리소스 롤백 */
		return ret;	/* NVMe: 리소스 획득 실패, NVMe 초기화 실패 */
	}

	/* Get the I/O range from DT */
	win = resource_list_first_type(&pp->bridge->windows, IORESOURCE_IO);	/* NVMe: DT에서 IO 범위 획득, NVMe IO 공간 설정 */
	if (win) {
		pp->io_size = resource_size(win->res);	/* NVMe: IO 공간 크기, NVMe IO 매핑 범위 */
		pp->io_bus_addr = win->res->start - win->offset;	/* NVMe: 버스 상대 IO 주소, NVMe IO 트랜잭션 주소 */
		pp->io_base = pci_pio_to_address(win->res->start);	/* NVMe: CPU PIO 주소, NVMe IO 공간 가상 주소 */
	}

	/*
	 * visconti_pcie_cpu_addr_fixup() uses pp->io_base, so we have to
	 * call dw_pcie_parent_bus_offset() after setting pp->io_base.
	 */
	pci->parent_bus_offset = dw_pcie_parent_bus_offset(pci, "config",
							   pp->cfg0_base);	/* NVMe: 부모 버스 오프셋, NVMe CPU/PCI 주소 변환 */
	return 0;	/* NVMe: 호스트 리소스 설정 완료, NVMe PCIe 열거 준비 */
}

int dw_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare RC 호스트 초기화, NVMe SSD를 위한 PCIe 루트 컴플렉스 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct device_node *np = dev->of_node;		/* NVMe: DT 노드, NVMe 호스트 DT 속성 */
	struct pci_host_bridge *bridge;			/* NVMe: PCI 호스트 브리지, NVMe PCIe 버스 트리 루트 */
	int ret;					/* NVMe: 반환값, NVMe 초기화 결과 */

	raw_spin_lock_init(&pp->lock);	/* NVMe: RC 포트 spinlock 초기화, NVMe MSI/리소스 동시 접근 보호 */

	bridge = devm_pci_alloc_host_bridge(dev, 0);	/* NVMe: PCI 호스트 브리지 할당, NVMe PCIe 버스 트리 생성 */
	if (!bridge)
		return -ENOMEM;	/* NVMe: 브리지 할당 실패, NVMe PCIe 초기화 불가 */

	pp->bridge = bridge;	/* NVMe: RC 포트에 브리지 연결, NVMe 호스트 상태 저장 */

	ret = dw_pcie_host_get_resources(pp);	/* NVMe: 호스트 리소스 파싱, NVMe config/메모리/IO 설정 */
	if (ret)
		return ret;	/* NVMe: 리소스 설정 실패, NVMe PCIe 초기화 중단 */

	if (pp->ops->init) {
		ret = pp->ops->init(pp);	/* NVMe: vendor 초기화 콜백, NVMe SoC 특화 설정 */
		if (ret)
			goto err_free_ecam;	/* NVMe: vendor 초기화 실패, NVMe 호스트 정리 */
	}

	if (pci_msi_enabled()) {
		/* NVMe: MSI 사용 가능 시, NVMe 장치의 MSI/MSI-X 활성화 여부 결정 */
		pp->use_imsi_rx = !(pp->ops->msi_init ||
				     of_property_present(np, "msi-parent") ||
				     of_property_present(np, "msi-map"));	/* NVMe: 내장 iMSI-RX 사용 여부, NVMe MSI 컨트롤러 선택 */

		/*
		 * For the use_imsi_rx case the default assignment is handled
		 * in the dw_pcie_msi_host_init().
		 */
		if (!pp->use_imsi_rx && !pp->num_vectors) {
			pp->num_vectors = MSI_DEF_NUM_VECTORS;	/* NVMe: 외부 MSI 시 기본 벡터, NVMe 큐 수 기본값 */
		} else if (pp->num_vectors > MAX_MSI_IRQS) {
			dev_err(dev, "Invalid number of vectors\n");	/* NVMe: 벡터 수 초과, NVMe 큐 수 제한 위반 */
			ret = -EINVAL;	/* NVMe: 잘못된 인자, NVMe MSI 초기화 실패 */
			goto err_deinit_host;	/* NVMe: 호스트 deinit 후 종료, NVMe 정리 */
		}

		if (pp->ops->msi_init) {
			ret = pp->ops->msi_init(pp);	/* NVMe: vendor MSI 초기화, NVMe SoC 특화 MSI 설정 */
			if (ret < 0)
				goto err_deinit_host;	/* NVMe: MSI 초기화 실패, NVMe 정리 */
		} else if (pp->use_imsi_rx) {
			ret = dw_pcie_msi_host_init(pp);	/* NVMe: 내장 iMSI-RX 초기화, NVMe MSI 인프라 구축 */
			if (ret < 0)
				goto err_deinit_host;	/* NVMe: MSI 초기화 실패, NVMe 정리 */
		}
	}

	dw_pcie_version_detect(pci);	/* NVMe: DWC IP 버전 감지, NVMe 호스트 기능 결정 */

	dw_pcie_iatu_detect(pci);	/* NVMe: iATU 윈도우 수/제한 감지, NVMe 메모리/IO 매핑 능력 */

	if (pci->num_lanes < 1)
		pci->num_lanes = dw_pcie_link_get_max_link_width(pci);	/* NVMe: 링크 폭 기본값, NVMe 대역폭 결정 */

	ret = of_pci_get_equalization_presets(dev, &pp->presets, pci->num_lanes);	/* NVMe: equalization preset 파싱, NVMe Gen3/4/5/7 링크 품질 */
	if (ret)
		goto err_free_msi;	/* NVMe: preset 파싱 실패, NVMe 정리 */

	/*
	 * Allocate the resource for MSG TLP before programming the iATU
	 * outbound window in dw_pcie_setup_rc(). Since the allocation depends
	 * on the value of 'region_align', this has to be done after
	 * dw_pcie_iatu_detect().
	 *
	 * Glue drivers need to set 'use_atu_msg' before dw_pcie_host_init() to
	 * make use of the generic MSG TLP implementation.
	 */
	if (pp->use_atu_msg)
		dw_pcie_host_request_msg_tlp_res(pp);	/* NVMe: MSG TLP 영역 할당, NVMe PME/PM 메시지 전송 준비 */

	ret = dw_pcie_edma_detect(pci);	/* NVMe: eDMA 감지, NVMe 고성능 DMA 엔진 사용 가능 여부 */
	if (ret)
		goto err_free_msi;	/* NVMe: eDMA 감지 실패, NVMe 정리 */

	ret = dw_pcie_setup_rc(pp);	/* NVMe: RC 모드 설정, NVMe PCIe 루트 포트 구성 */
	if (ret)
		goto err_remove_edma;	/* NVMe: RC 설정 실패, NVMe 정리 */

	if (!dw_pcie_link_up(pci)) {
		ret = dw_pcie_start_link(pci);	/* NVMe: PCIe 링크 시작, NVMe SSD와 물리 연결 설정 */
		if (ret)
			goto err_remove_edma;	/* NVMe: 링크 시작 실패, NVMe 연결 불가 */
	}

	/*
	 * Only fail on timeout error. Other errors indicate the device may
	 * become available later, so continue without failing.
	 */
	ret = dw_pcie_wait_for_link(pci);	/* NVMe: 링크 업 대기, NVMe SSD 연결 완료 확인 */
	if (ret == -ETIMEDOUT)
		goto err_stop_link;	/* NVMe: 타임아웃 시 실패, NVMe 장치 미연결 */

	ret = pci_host_probe(bridge);	/* NVMe: PCI 호스트 프로브, NVMe PCIe 버스 스캔 및 장치 열거 */
	if (ret)
		goto err_stop_link;	/* NVMe: 버스 프로브 실패, NVMe 열거 불가 */

	if (pp->ops->post_init)
		pp->ops->post_init(pp);	/* NVMe: vendor 후처리, NVMe SoC 특화 마무리 */

	dwc_pcie_debugfs_init(pci, DW_PCIE_RC_TYPE);	/* NVMe: debugfs 초기화, NVMe 런타임 디버깅 인프라 */

	return 0;	/* NVMe: 호스트 초기화 완료, NVMe PCIe 사용 가능 */

err_stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe 연결 해제 */

err_remove_edma:
	dw_pcie_edma_remove(pci);	/* NVMe: eDMA 제거, NVMe DMA 엔진 정리 */

err_free_msi:
	if (pp->use_imsi_rx)
		dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 벡터 정리 */

err_deinit_host:
	if (pp->ops->deinit)
		pp->ops->deinit(pp);	/* NVMe: vendor deinit, NVMe SoC 정리 */

err_free_ecam:
	if (pp->cfg)
		pci_ecam_free(pp->cfg);	/* NVMe: ECAM 윈도우 해제, NVMe config 매핑 정리 */

	return ret;	/* NVMe: 초기화 실패 코드 반환, NVMe 호스트 초기화 종료 */
}
EXPORT_SYMBOL_GPL(dw_pcie_host_init);

void dw_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare RC 호스트 해제, NVMe SSD 제거/시스템 종료 시 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */

	dwc_pcie_debugfs_deinit(pci);	/* NVMe: debugfs 해제, NVMe 디버깅 인프라 정리 */

	pci_stop_root_bus(pp->bridge->bus);	/* NVMe: 루트 버스 정지, NVMe 버스 트래픽 중단 */
	pci_remove_root_bus(pp->bridge->bus);	/* NVMe: 루트 버스 제거, NVMe PCIe 장치 언바인딩 */

	dw_pcie_stop_link(pci);	/* NVMe: PCIe 링크 정지, NVMe 물리 연결 해제 */

	dw_pcie_edma_remove(pci);	/* NVMe: eDMA 제거, NVMe DMA 엔진 정리 */

	if (pp->use_imsi_rx)
		dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 벡터 정리 */

	if (pp->ops->deinit)
		pp->ops->deinit(pp);	/* NVMe: vendor deinit, NVMe SoC 정리 */

	if (pp->cfg)
		pci_ecam_free(pp->cfg);	/* NVMe: ECAM 윈도우 해제, NVMe config 매핑 정리 */
}
EXPORT_SYMBOL_GPL(dw_pcie_host_deinit);

static void __iomem *dw_pcie_other_conf_map_bus(struct pci_bus *bus,
						unsigned int devfn, int where)
{
	/* NVMe: 하위 버스 config 공간 버스 매핑, NVMe 장치의 PCIe config 접근 경로 설정 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe config 트랜잭션 경로 */
	int type, ret;					/* NVMe: ATU 타입/반환값, NVMe config 경로 설정 */
	u32 busdev;					/* NVMe: BDF 조합, NVMe 장치/함수 선택 */

	/*
	 * Checking whether the link is up here is a last line of defense
	 * against platforms that forward errors on the system bus as
	 * SError upon PCI configuration transactions issued when the link
	 * is down. This check is racy by definition and does not stop
	 * the system from triggering an SError if the link goes down
	 * after this check is performed.
	 */
	if (!dw_pcie_link_up(pci))
		return NULL;	/* NVMe: 링크 다운 시 NULL, NVMe config 접근 방지(SError 방지) */

	busdev = PCIE_ATU_BUS(bus->number) | PCIE_ATU_DEV(PCI_SLOT(devfn)) |
		 PCIE_ATU_FUNC(PCI_FUNC(devfn));	/* NVMe: BDF를 ATU 포맷으로, NVMe 대상 장치/함수 지정 */

	if (pci_is_root_bus(bus->parent))
		type = PCIE_ATU_TYPE_CFG0;	/* NVMe: 루트 버스 직속은 type 0, NVMe 직접 연결 장치 */
	else
		type = PCIE_ATU_TYPE_CFG1;	/* NVMe: 다단 버스는 type 1, NVMe 스위치 하위 장치 */

	atu.type = type;				/* NVMe: ATU 타입 설정, NVMe config 트랜잭션 유형 */
	atu.parent_bus_addr = pp->cfg0_base - pci->parent_bus_offset;	/* NVMe: CPU config 주소, NVMe config 공간 시작 */
	atu.pci_addr = busdev;							/* NVMe: PCI BDF 주소, NVMe 대상 장치 */
	atu.size = pp->cfg0_size;						/* NVMe: config 공간 크기, NVMe config 접근 범위 */

	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: outbound ATU 프로그램, NVMe config 접근 경로 설정 */
	if (ret)
		return NULL;	/* NVMe: ATU 설정 실패, NVMe config 접근 불가 */

	return pp->va_cfg0_base + where;	/* NVMe: 가상 config 주소 반환, NVMe 레지스터 read/write */
}

static int dw_pcie_rd_other_conf(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val)
{
	/* NVMe: 하위 버스 config read, NVMe 장치의 PCIe capability/레지스터 읽기 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe IO 공간 복원 */
	int ret;					/* NVMe: 반환값, NVMe config read 결과 */

	ret = pci_generic_config_read(bus, devfn, where, size, val);	/* NVMe: 표준 PCI config read, NVMe 장치 레지스터 읽기 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* NVMe: read 실패, NVMe 레지스터 읽기 오류 */

	if (pp->cfg0_io_shared) {
		atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 공간 ATU 타입, NVMe IO 복원 */
		atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
		atu.pci_addr = pp->io_bus_addr;	/* NVMe: 버스 IO 주소, NVMe IO 트랜잭션 */
		atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

		ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 복원, NVMe config 후 IO 공간 복구 */
		if (ret)
			return PCIBIOS_SET_FAILED;	/* NVMe: IO 복원 실패, NVMe IO 접근 불가 */
	}

	return PCIBIOS_SUCCESSFUL;	/* NVMe: config read 성공, NVMe 레지스터 값 획득 */
}

static int dw_pcie_wr_other_conf(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	/* NVMe: 하위 버스 config write, NVMe 장치의 PCIe 레지스터 쓰기(예: BAR, command) */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe IO 공간 복원 */
	int ret;					/* NVMe: 반환값, NVMe config write 결과 */

	ret = pci_generic_config_write(bus, devfn, where, size, val);	/* NVMe: 표준 PCI config write, NVMe 장치 레지스터 쓰기 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* NVMe: write 실패, NVMe 레지스터 쓰기 오류 */

	if (pp->cfg0_io_shared) {
		atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 공간 ATU 타입, NVMe IO 복원 */
		atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
		atu.pci_addr = pp->io_bus_addr;	/* NVMe: 버스 IO 주소, NVMe IO 트랜잭션 */
		atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

		ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 복원, NVMe config 후 IO 공간 복구 */
		if (ret)
			return PCIBIOS_SET_FAILED;	/* NVMe: IO 복원 실패, NVMe IO 접근 불가 */
	}

	return PCIBIOS_SUCCESSFUL;	/* NVMe: config write 성공, NVMe 레지스터 갱신 완료 */
}

static struct pci_ops dw_child_pcie_ops = {
	.map_bus = dw_pcie_other_conf_map_bus,	/* NVMe: 하위 버스 매핑, NVMe 장치 config 접근 경로 */
	.read = dw_pcie_rd_other_conf,		/* NVMe: 하위 버스 read, NVMe 레지스터 읽기 */
	.write = dw_pcie_wr_other_conf,		/* NVMe: 하위 버스 write, NVMe 레지스터 쓰기 */
};

void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	/* NVMe: RC 자체 config 공간 매핑, NVMe 루트 포트 capability 접근 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 DBI 접근 */

	if (PCI_SLOT(devfn) > 0)
		return NULL;	/* NVMe: RC는 devfn 0만 유효, NVMe 루트 포트 외 접근 거부 */

	return pci->dbi_base + where;	/* NVMe: DBI 레지스터 주소 반환, NVMe 루트 포트 설정 */
}
EXPORT_SYMBOL_GPL(dw_pcie_own_conf_map_bus);

static void __iomem *dw_pcie_ecam_conf_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	/* NVMe: ECAM 모드 config 공간 매핑, NVMe 버스/장치 레지스터 접근 */
	struct pci_config_window *cfg = bus->sysdata;	/* NVMe: ECAM 윈도우, NVMe config 공간 매핑 */
	struct dw_pcie_rp *pp = cfg->priv;		/* NVMe: RC 포트, NVMe 연결된 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 DBI 접근 */
	unsigned int busn = bus->number;		/* NVMe: 현재 버스 번호, NVMe 대상 버스 */

	if (busn > 0)
		return pci_ecam_map_bus(bus, devfn, where);	/* NVMe: bus > 0은 ECAM 사용, NVMe 다단 버스 장치 접근 */

	if (PCI_SLOT(devfn) > 0)
		return NULL;	/* NVMe: 루트 버스는 devfn 0만, NVMe 루트 포트 외 거부 */

	return pci->dbi_base + where;	/* NVMe: DBI로 루트 포트 접근, NVMe 루트 포트 설정 */
}

static struct pci_ops dw_pcie_ops = {
	.map_bus = dw_pcie_own_conf_map_bus,	/* NVMe: RC 자체 매핑, NVMe 루트 포트 config 접근 */
	.read = pci_generic_config_read,	/* NVMe: 표준 config read, NVMe 루트 포트 레지스터 읽기 */
	.write = pci_generic_config_write,	/* NVMe: 표준 config write, NVMe 루트 포트 레지스터 쓰기 */
};

static struct pci_ops dw_pcie_ecam_ops = {
	.map_bus = dw_pcie_ecam_conf_map_bus,	/* NVMe: ECAM 매핑, NVMe 버스/장치 config 접근 */
	.read = pci_generic_config_read,	/* NVMe: 표준 config read, NVMe 장치 레지스터 읽기 */
	.write = pci_generic_config_write,	/* NVMe: 표준 config write, NVMe 장치 레지스터 쓰기 */
};

static int dw_pcie_iatu_setup(struct dw_pcie_rp *pp)
{
	/* NVMe: iATU 인바운드/아웃바운드 윈도우 설정, NVMe BAR 매핑 및 DMA 메모리 공간 구성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe MEM/IO/CFG 매핑 */
	struct resource_entry *entry;			/* NVMe: 리소스 엔트리, NVMe 메모리/DMA 범위 */
	int ob_iatu_index;				/* NVMe: outbound iATU 인덱스, NVMe 메모리/IO/MSG 할당 */
	int ib_iatu_index;				/* NVMe: inbound iATU 인덱스, NVMe DMA 메모리 매핑 */
	int i, ret;					/* NVMe: 루프 인덱스/반환값, NVMe ATU 설정 */

	if (!pci->num_ob_windows) {
		dev_err(pci->dev, "No outbound iATU found\n");	/* NVMe: outbound 윈도우 없음, NVMe BAR/IO 접근 불가 */
		return -EINVAL;	/* NVMe: 잘못된 상태, NVMe 초기화 실패 */
	}

	/*
	 * Ensure all out/inbound windows are disabled before proceeding with
	 * the MEM/IO (dma-)ranges setups.
	 */
	for (i = 0; i < pci->num_ob_windows; i++)
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_OB, i);	/* NVMe: outbound 윈도우 비활성화, NVMe 이전 매핑 클리어 */

	for (i = 0; i < pci->num_ib_windows; i++)
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, i);	/* NVMe: inbound 윈도우 비활성화, NVMe 이전 DMA 매핑 클리어 */

	/*
	 * NOTE: For outbound address translation, outbound iATU at index 0 is
	 * reserved for CFG IOs (dw_pcie_other_conf_map_bus()), thus start at
	 * index 1.
	 *
	 * If using ECAM, outbound iATU at index 0 and index 1 is reserved for
	 * CFG IOs.
	 */
	if (pp->ecam_enabled) {
		ob_iatu_index = 2;	/* NVMe: ECAM 시 0,1은 CFG용, NVMe MEM/IO는 2부터 */
		ret = dw_pcie_config_ecam_iatu(pp);	/* NVMe: ECAM CFG iATU 설정, NVMe config 접근 경로 */
		if (ret) {
			dev_err(pci->dev, "Failed to configure iATU in ECAM mode\n");	/* NVMe: ECAM iATU 실패, NVMe config 접근 불가 */
			return ret;	/* NVMe: 초기화 실패, NVMe 열거 불가 */
		}
	} else {
		ob_iatu_index = 1;	/* NVMe: non-ECAM 시 0은 CFG용, NVMe MEM/IO는 1부터 */
	}

	resource_list_for_each_entry(entry, &pp->bridge->windows) {	/* NVMe: 호스트 브리지 윈도우 순회, NVMe BAR 메모리 공간 매핑 */
		resource_size_t res_size;	/* NVMe: 리소스 크기, NVMe BAR 매핑 크기 */

		if (resource_type(entry->res) != IORESOURCE_MEM)
			continue;	/* NVMe: MEM이 아니면 skip, NVMe IO는 별도 처리 */

		atu.type = PCIE_ATU_TYPE_MEM;	/* NVMe: MEM 타입 ATU, NVMe BAR 메모리 매핑 */
		atu.parent_bus_addr = entry->res->start - pci->parent_bus_offset;	/* NVMe: CPU 물리 주소, NVMe 호스트 메모리 공간 */
		atu.pci_addr = entry->res->start - entry->offset;	/* NVMe: PCI 버스 주소, NVMe 장치가 보는 BAR 주소 */

		/* Adjust iATU size if MSG TLP region was allocated before */
		if (pp->msg_res && pp->msg_res->parent == entry->res)
			res_size = resource_size(entry->res) -
					resource_size(pp->msg_res);	/* NVMe: MSG 영역 제외, NVMe PME TLP 주소는 BAR 할당에서 제외 */
		else
			res_size = resource_size(entry->res);	/* NVMe: 전체 리소스 크기, NVMe BAR 매핑 크기 */

		while (res_size > 0) {
			/*
			 * Return failure if we run out of windows in the
			 * middle. Otherwise, we would end up only partially
			 * mapping a single resource.
			 */
			if (ob_iatu_index >= pci->num_ob_windows) {
				dev_err(pci->dev, "Cannot add outbound window for region: %pr\n",
					entry->res);	/* NVMe: outbound 윈도우 고갈, NVMe BAR 일부 매핑 실패 */
				return -ENOMEM;	/* NVMe: 메모리/리소스 부족, NVMe 메모리 매핑 불완전 */
			}

			atu.index = ob_iatu_index;	/* NVMe: 현재 outbound 인덱스, NVMe MEM 윈도우 할당 */
			atu.size = MIN(pci->region_limit + 1, res_size);	/* NVMe: ATU 윈도우 크기, NVMe BAR 매핑 단위 */

			ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: outbound ATU 프로그램, NVMe 장치 메모리 접근 가능 */
			if (ret) {
				dev_err(pci->dev, "Failed to set MEM range %pr\n",
					entry->res);	/* NVMe: MEM ATU 설정 실패, NVMe BAR 접근 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe 메모리 매핑 실패 */
			}

			ob_iatu_index++;	/* NVMe: 다음 outbound 인덱스, NVMe 다음 MEM 세그먼트 */
			atu.parent_bus_addr += atu.size;	/* NVMe: 다음 CPU 주소, NVMe 다음 MEM 세그먼트 */
			atu.pci_addr += atu.size;		/* NVMe: 다음 PCI 주소, NVMe 다음 MEM 세그먼트 */
			res_size -= atu.size;			/* NVMe: 남은 크기, NVMe MEM 매핑 진행 */
		}
	}

	if (pp->io_size) {
		if (ob_iatu_index < pci->num_ob_windows) {
			atu.index = ob_iatu_index;	/* NVMe: IO용 outbound 인덱스, NVMe IO 공간 매핑 */
			atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 타입 ATU, NVMe IO 트랜잭션 */
			atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
			atu.pci_addr = pp->io_bus_addr;	/* NVMe: PCI IO 주소, NVMe 장치 IO 공간 */
			atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

			ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 프로그램, NVMe IO 접근 가능 */
			if (ret) {
				dev_err(pci->dev, "Failed to set IO range %pr\n",
					entry->res);	/* NVMe: IO ATU 설정 실패, NVMe IO 접근 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe IO 매핑 실패 */
			}
			ob_iatu_index++;	/* NVMe: 다음 outbound 인덱스, NVMe MSG/추가 공간 */
		} else {
			/*
			 * If there are not enough outbound windows to give I/O
			 * space its own iATU, the outbound iATU at index 0 will
			 * be shared between I/O space and CFG IOs, by
			 * temporarily reconfiguring the iATU to CFG space, in
			 * order to do a CFG IO, and then immediately restoring
			 * it to I/O space. This is only implemented when using
			 * dw_pcie_other_conf_map_bus(), which is not the case
			 * when using ECAM.
			 */
			if (pp->ecam_enabled) {
				dev_err(pci->dev, "Cannot add outbound window for I/O\n");
				return -ENOMEM;	/* NVMe: ECAM 시 IO 공유 불가, NVMe IO 매핑 실패 */
			}
			pp->cfg0_io_shared = true;	/* NVMe: CFG/IO 공유 플래그, NVMe config 후 IO 복원 */
		}
	}

	if (pp->use_atu_msg) {
		if (ob_iatu_index >= pci->num_ob_windows) {
			dev_err(pci->dev, "Cannot add outbound window for MSG TLP\n");	/* NVMe: MSG TLP 윈도우 부족, NVMe PME 메시지 전송 불가 */
			return -ENOMEM;	/* NVMe: 리소스 부족, NVMe 전원 관리 기능 제한 */
		}
		pp->msg_atu_index = ob_iatu_index++;	/* NVMe: MSG TLP 전용 ATU 인덱스, NVMe PME/PM 메시지 전송 */
	}

	ib_iatu_index = 0;	/* NVMe: inbound iATU 인덱스 0부터, NVMe DMA 메모리 매핑 */
	resource_list_for_each_entry(entry, &pp->bridge->dma_ranges) {	/* NVMe: dma-ranges 순회, NVMe 장치 DMA 주소 변환 설정 */
		resource_size_t res_start, res_size, window_size;	/* NVMe: DMA 범위 시작/크기/윈도우, NVMe DMA 메모리 공간 */

		if (resource_type(entry->res) != IORESOURCE_MEM)
			continue;	/* NVMe: MEM이 아니면 skip, NVMe DMA는 메모리 공간 */

		res_size = resource_size(entry->res);	/* NVMe: DMA 범위 크기, NVMe DMA 가능 메모리 크기 */
		res_start = entry->res->start;		/* NVMe: DMA 범위 시작, NVMe DMA 물리 주소 시작 */
		while (res_size > 0) {
			/*
			 * Return failure if we run out of windows in the
			 * middle. Otherwise, we would end up only partially
			 * mapping a single resource.
			 */
			if (ib_iatu_index >= pci->num_ib_windows) {
				dev_err(pci->dev, "Cannot add inbound window for region: %pr\n",
					entry->res);	/* NVMe: inbound 윈도우 고갈, NVMe DMA 메모리 일부 매핑 실패 */
				return -ENOMEM;	/* NVMe: 리소스 부족, NVMe DMA 매핑 불완전(IOMMU/오류 가능) */
			}

			window_size = MIN(pci->region_limit + 1, res_size);	/* NVMe: inbound 윈도우 크기, NVMe DMA 매핑 단위 */
			ret = dw_pcie_prog_inbound_atu(pci, ib_iatu_index,
						       PCIE_ATU_TYPE_MEM, res_start,
						       res_start - entry->offset, window_size);	/* NVMe: inbound ATU 프로그램, NVMe 장치 DMA 주소->CPU 주소 변환 */
			if (ret) {
				dev_err(pci->dev, "Failed to set DMA range %pr\n",
					entry->res);	/* NVMe: DMA ATU 설정 실패, NVMe DMA 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe DMA 메모리 매핑 실패 */
			}

			ib_iatu_index++;	/* NVMe: 다음 inbound 인덱스, NVMe 다음 DMA 세그먼트 */
			res_start += window_size;	/* NVMe: 다음 DMA 시작 주소, NVMe 다음 세그먼트 */
			res_size -= window_size;	/* NVMe: 남은 DMA 크기, NVMe DMA 매핑 진행 */
		}
	}

	return 0;	/* NVMe: iATU 설정 완료, NVMe 메모리/IO/DMA 매핑 완료 */
}

static void dw_pcie_program_presets(struct dw_pcie_rp *pp, enum pci_bus_speed speed)
{
	/* NVMe: 링크 equalization preset 값 프로그램, NVMe Gen3/4/5/7 링크 품질/안정성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u8 lane_eq_offset, lane_reg_size, cap_id;	/* NVMe: EQ 레지스터 오프셋/크기/cap ID, NVMe 링크 속도별 설정 */
	u8 *presets;					/* NVMe: preset 배열, NVMe equalization 값 */
	u32 cap;					/* NVMe: capability 오프셋, NVMe EQ capability 위치 */
	int i;						/* NVMe: 레인 인덱스, NVMe 다중 레인 preset 설정 */

	if (speed == PCIE_SPEED_8_0GT) {
		presets = (u8 *)pp->presets.eq_presets_8gts;	/* NVMe: 8GT/s preset, NVMe Gen3 링크 */
		lane_eq_offset =  PCI_SECPCI_LE_CTRL;		/* NVMe: 8GT/s EQ 레지스터, NVMe Gen3 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_SECPCI;			/* NVMe: Secondary PCI capability, NVMe Gen3 EQ */
		/* For data rate of 8 GT/S each lane equalization control is 16bits wide*/
		lane_reg_size = 0x2;	/* NVMe: 8GT/s 레인당 2바이트, NVMe Gen3 preset 크기 */
	} else if (speed == PCIE_SPEED_16_0GT) {
		presets = pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_16GTS - 1];	/* NVMe: 16GT/s preset, NVMe Gen4 링크 */
		lane_eq_offset = PCI_PL_16GT_LE_CTRL;					/* NVMe: 16GT/s EQ 레지스터, NVMe Gen4 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_16GT;					/* NVMe: 16GT PHY capability, NVMe Gen4 EQ */
		lane_reg_size = 0x1;	/* NVMe: 16GT/s 레인당 1바이트, NVMe Gen4 preset 크기 */
	} else if (speed == PCIE_SPEED_32_0GT) {
		presets =  pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_32GTS - 1];	/* NVMe: 32GT/s preset, NVMe Gen5 링크 */
		lane_eq_offset = PCI_PL_32GT_LE_CTRL;					/* NVMe: 32GT/s EQ 레지스터, NVMe Gen5 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_32GT;					/* NVMe: 32GT PHY capability, NVMe Gen5 EQ */
		lane_reg_size = 0x1;	/* NVMe: 32GT/s 레인당 1바이트, NVMe Gen5 preset 크기 */
	} else if (speed == PCIE_SPEED_64_0GT) {
		presets =  pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_64GTS - 1];	/* NVMe: 64GT/s preset, NVMe Gen7 링크 */
		lane_eq_offset = PCI_PL_64GT_LE_CTRL;					/* NVMe: 64GT/s EQ 레지스터, NVMe Gen7 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_64GT;					/* NVMe: 64GT PHY capability, NVMe Gen7 EQ */
		lane_reg_size = 0x1;	/* NVMe: 64GT/s 레인당 1바이트, NVMe Gen7 preset 크기 */
	} else {
		return;	/* NVMe: 지원하지 않는 속도, NVMe preset 설정 불필요 */
	}

	if (presets[0] == PCI_EQ_RESV)
		return;	/* NVMe: 예약 값이면 skip, NVMe platform preset 미지정 */

	cap = dw_pcie_find_ext_capability(pci, cap_id);	/* NVMe: EQ extended capability 탐색, NVMe 링크 품질 설정 위치 */
	if (!cap)
		return;	/* NVMe: capability 없음, NVMe preset 설정 불가 */

	/*
	 * Write preset values to the registers byte-by-byte for the given
	 * number of lanes and register size.
	 */
	for (i = 0; i < pci->num_lanes * lane_reg_size; i++)
		dw_pcie_writeb_dbi(pci, cap + lane_eq_offset + i, presets[i]);	/* NVMe: 레인별 preset 쓰기, NVMe 링크 equalization 적용 */
}

static void dw_pcie_config_presets(struct dw_pcie_rp *pp)
{
	/* NVMe: 지원하는 속도별 preset 구성, NVMe 링크 협상/품질 최적화 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	enum pci_bus_speed speed = pcie_get_link_speed(pci->max_link_speed);	/* NVMe: 최대 링크 속도, NVMe Gen3/4/5/7 지원 */

	/*
	 * Lane equalization settings need to be applied for all data rates the
	 * controller supports and for all supported lanes.
	 */

	if (speed >= PCIE_SPEED_8_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_8_0GT);	/* NVMe: Gen3(8GT/s) preset, NVMe Gen3 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_16_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_16_0GT);	/* NVMe: Gen4(16GT/s) preset, NVMe Gen4 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_32_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_32_0GT);	/* NVMe: Gen5(32GT/s) preset, NVMe Gen5 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_64_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_64_0GT);	/* NVMe: Gen7(64GT/s) preset, NVMe Gen7 SSD 링크 안정화 */
}

int dw_pcie_setup_rc(struct dw_pcie_rp *pp)
{
	/* NVMe: RC 모드 레지스터 설정, NVMe SSD를 위한 PCIe 루트 포트 구성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u32 val;					/* NVMe: 레지스터 값, NVMe RC 레지스터 구성 */
	int ret;					/* NVMe: 반환값, NVMe RC 설정 결과 */

	/*
	 * Enable DBI read-only registers for writing/updating configuration.
	 * Write permission gets disabled towards the end of this function.
	 */
	dw_pcie_dbi_ro_wr_en(pci);	/* NVMe: DBI 쓰기 가능, NVMe RC 레지스터 수정 */

	dw_pcie_setup(pci);	/* NVMe: DesignWave 공통 설정, NVMe 호스트 기본 구성 */

	dw_pcie_msi_init(pp);	/* NVMe: MSI 하드웨어 초기화, NVMe MSI TLP 수신 준비 */

	/* Setup RC BARs */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0x00000004);	/* NVMe: BAR0 32-bit MEM, NVMe 루트 포트 BAR 초기화 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_1, 0x00000000);	/* NVMe: BAR1 0, NVMe 루트 포트 BAR 초기화 */

	/* Setup interrupt pins */
	val = dw_pcie_readl_dbi(pci, PCI_INTERRUPT_LINE);	/* NVMe: INTERRUPT_LINE 읽기, NVMe INTx 라우팅 설정 */
	val &= 0xffff00ff;	/* NVMe: interrupt pin 필드 클리어, NVMe INTx pin 초기화 */
	val |= 0x00000100;	/* NVMe: INTA# 설정, NVMe 루트 포트 INTx pin */
	dw_pcie_writel_dbi(pci, PCI_INTERRUPT_LINE, val);	/* NVMe: interrupt pin 레지스터 쓰기, NVMe INTx 라우팅 */

	/* Setup bus numbers */
	val = dw_pcie_readl_dbi(pci, PCI_PRIMARY_BUS);	/* NVMe: PRIMARY_BUS 읽기, NVMe 버스 번호 설정 */
	val &= 0xff000000;	/* NVMe: 하위 버스 번호 필드 클리어, NVMe 버스 범위 초기화 */
	val |= 0x00ff0100;	/* NVMe: primary=0, secondary=1, subordinate=255, NVMe 버스 트리 */
	dw_pcie_writel_dbi(pci, PCI_PRIMARY_BUS, val);	/* NVMe: 버스 번호 쓰기, NVMe PCIe 버스 열거 범위 */

	/* Setup command register */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);	/* NVMe: COMMAND 읽기, NVMe 루트 포트 명령 설정 */
	val &= 0xffff0000;	/* NVMe: 하위 16비트 클리어, NVMe command 초기화 */
	val |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		PCI_COMMAND_MASTER | PCI_COMMAND_SERR;	/* NVMe: IO/MEM/Bus Master/SERR 활성화, NVMe BAR/IO/DMA/에러 허용 */
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);	/* NVMe: command 레지스터 쓰기, NVMe 루트 포트 트랜잭션 활성화 */

	dw_pcie_hide_unsupported_l1ss(pci);	/* NVMe: 미지원 L1SS 숨김, NVMe ASPM/전원 관리 capability 정리 */

	dw_pcie_config_presets(pp);	/* NVMe: equalization preset 구성, NVMe 링크 품질 최적화 */
	/*
	 * If the platform provides its own child bus config accesses, it means
	 * the platform uses its own address translation component rather than
	 * ATU, so we should not program the ATU here.
	 */
	if (pp->bridge->child_ops == &dw_child_pcie_ops || pp->ecam_enabled) {
		ret = dw_pcie_iatu_setup(pp);	/* NVMe: iATU 설정, NVMe 메모리/IO/DMA 매핑 */
		if (ret)
			return ret;	/* NVMe: iATU 설정 실패, NVMe 메모리/DMA 불가 */
	}

	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0);	/* NVMe: BAR0 비활성화, NVMe 루트 포트 BAR 클리어 */

	/* Program correct class for RC */
	dw_pcie_writew_dbi(pci, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);	/* NVMe: PCI-PCI bridge 클래스, NVMe 루트 포트 클래스 설정 */

	val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);	/* NVMe: 링크 폭/속도 제어 읽기, NVMe 링크 협상 제어 */
	val |= PORT_LOGIC_SPEED_CHANGE;	/* NVMe: 속도 변경 요청, NVMe Gen3/4/5/7 협상 */
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);	/* NVMe: 링크 속도 변경 설정, NVMe 최대 속도로 협상 */

	dw_pcie_dbi_ro_wr_dis(pci);	/* NVMe: DBI 쓰기 금지, NVMe RC 레지스터 보호 */

	/*
	 * The iMSI-RX module does not support receiving MSI or MSI-X generated
	 * by the Root Port. If iMSI-RX is used as the MSI controller, remove
	 * the MSI and MSI-X capabilities of the Root Port to allow the drivers
	 * to fall back to INTx instead.
	 */
	if (pp->use_imsi_rx && !pp->keep_rp_msi_en) {
		dw_pcie_remove_capability(pci, PCI_CAP_ID_MSI);	/* NVMe: 루트 포트 MSI capability 제거, NVMe RP INTx 폴백 */
		dw_pcie_remove_capability(pci, PCI_CAP_ID_MSIX);	/* NVMe: 루트 포트 MSI-X capability 제거, NVMe RP INTx 폴백 */
	}

	return 0;	/* NVMe: RC 설정 완료, NVMe PCIe 루트 포트 준비 */
}
EXPORT_SYMBOL_GPL(dw_pcie_setup_rc);

static int dw_pcie_pme_turn_off(struct dw_pcie *pci)
{
	/* NVMe: PME_Turn_Off 메시지 전송, NVMe 장치 절전 상태 진입 유도 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };	/* NVMe: outbound ATU 설정, NVMe PME MSG TLP 경로 */
	void __iomem *mem;			/* NVMe: MSG 영역 가상 주소, NVMe PME TLP 전송 주소 */
	int ret;				/* NVMe: 반환값, NVMe PME 전송 결과 */

	if (pci->num_ob_windows <= pci->pp.msg_atu_index)
		return -ENOSPC;	/* NVMe: MSG TLP ATU 없음, NVMe PME 전송 불가 */

	if (!pci->pp.msg_res)
		return -ENOSPC;	/* NVMe: MSG 리소스 없음, NVMe PME 전송 불가 */

	atu.code = PCIE_MSG_CODE_PME_TURN_OFF;	/* NVMe: PME_Turn_Off 메시지 코드, NVMe 절전 메시지 */
	atu.routing = PCIE_MSG_TYPE_R_BC;	/* NVMe: broadcast 라우팅, NVMe 모든 장치에 PME 전송 */
	atu.type = PCIE_ATU_TYPE_MSG;		/* NVMe: MSG TLP 타입, NVMe PME 메시지 TLP */
	atu.size = resource_size(pci->pp.msg_res);	/* NVMe: MSG 영역 크기, NVMe PME TLP 전송 범위 */
	atu.index = pci->pp.msg_atu_index;		/* NVMe: MSG 전용 ATU 인덱스, NVMe PME 경로 */

	atu.parent_bus_addr = pci->pp.msg_res->start - pci->parent_bus_offset;	/* NVMe: CPU MSG 주소, NVMe PME TLP 전송 주소 */

	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: MSG TLP ATU 프로그램, NVMe PME 메시지 경로 설정 */
	if (ret)
		return ret;	/* NVMe: ATU 설정 실패, NVMe PME 전송 불가 */

	mem = ioremap(pci->pp.msg_res->start, pci->region_align);	/* NVMe: MSG 영역 매핑, NVMe PME TLP 전송용 */
	if (!mem)
		return -ENOMEM;	/* NVMe: 매핑 실패, NVMe PME 전송 불가 */

	/* A dummy write is converted to a Msg TLP */
	writel(0, mem);	/* NVMe: dummy write -> PME_Turn_Off MSG TLP, NVMe 장치 절전 요청 */

	iounmap(mem);	/* NVMe: MSG 영역 언매핑, NVMe PME TLP 전송 완료 */

	return 0;	/* NVMe: PME_Turn_Off 전송 완료, NVMe 절전 진행 */
}

int dw_pcie_suspend_noirq(struct dw_pcie *pci)
{
	/* NVMe: suspend_noirq 콜백, NVMe 시스템 절전 시 PCIe 링크/장치 처리 */
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);	/* NVMe: PCIe capability 오프셋, NVMe 링크 제어 접근 */
	int ret = 0;							/* NVMe: 반환값, NVMe suspend 결과 */
	u32 val;							/* NVMe: 레지스터 값, NVMe LTSSM 상태 */

	if (!dw_pcie_link_up(pci))
		goto stop_link;	/* NVMe: 링크 다운이면 바로 정지, NVMe 절전 진행 */

	/*
	 * If L1SS is supported, then do not put the link into L2 as some
	 * devices such as NVMe expect low resume latency.
	 */
	if (dw_pcie_readw_dbi(pci, offset + PCI_EXP_LNKCTL) & PCI_EXP_LNKCTL_ASPM_L1)
		return 0;	/* NVMe: L1 ASPM 활성 시 L2 진입 skip, NVMe 빠른 resume 지원 */

	if (pci->pp.ops->pme_turn_off) {
		pci->pp.ops->pme_turn_off(&pci->pp);	/* NVMe: vendor PME_Turn_Off, NVMe SoC 특화 절전 */
	} else {
		ret = dw_pcie_pme_turn_off(pci);	/* NVMe: generic PME_Turn_Off, NVMe 장치 절전 메시지 */
		if (ret)
			return ret;	/* NVMe: PME 전송 실패, NVMe suspend 실패 */
	}

	/*
	 * Some SoCs do not support reading the LTSSM register after
	 * PME_Turn_Off broadcast. For those SoCs, skip waiting for L2/L3 Ready
	 * state and wait 10ms as recommended in PCIe spec r6.0, sec 5.3.3.2.1.
	 */
	if (pci->pp.skip_l23_ready) {
		mdelay(PCIE_PME_TO_L2_TIMEOUT_US/1000);	/* NVMe: 10ms 대기, NVMe L2 진입 대체 */
		goto stop_link;	/* NVMe: 링크 정지로 이동, NVMe 절전 완료 */
	}

	ret = read_poll_timeout(dw_pcie_get_ltssm, val,
				val == DW_PCIE_LTSSM_L2_IDLE ||
				val <= DW_PCIE_LTSSM_DETECT_WAIT,
				PCIE_PME_TO_L2_TIMEOUT_US/10,
				PCIE_PME_TO_L2_TIMEOUT_US, false, pci);	/* NVMe: L2 idle 또는 detect wait 폴링, NVMe 링크 절전 상태 확인 */
	if (ret) {
		/*
		 * Failure is non-fatal since spec r7.0, sec 5.3.3.2.1,
		 * recommends proceeding with L2/L3 sequence even if one or more
		 * devices do not respond with PME_TO_Ack after 10ms timeout.
		 */
		dev_warn(pci->dev, "Timeout waiting for L2 entry! LTSSM: 0x%x\n", val);	/* NVMe: L2 진입 타임아웃, NVMe 절전 계속 진행 */
		ret = 0;	/* NVMe: non-fatal, NVMe suspend 계속 */
	}

	/*
	 * Per PCIe r6.0, sec 5.3.3.2.1, software should wait at least
	 * 100ns after L2/L3 Ready before turning off refclock and
	 * main power. This is harmless when no endpoint is connected.
	 */
	udelay(1);	/* NVMe: 1us 대기, NVMe refclock/power off 안전 여유 */

stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe 물리 연결 해제 */
	if (pci->pp.ops->deinit)
		pci->pp.ops->deinit(&pci->pp);	/* NVMe: vendor deinit, NVMe SoC 절전 설정 */

	pci->suspended = true;	/* NVMe: suspend 상태 표시, NVMe resume 시 재초기화 플래그 */

	return ret;	/* NVMe: suspend 결과 반환, NVMe 절전 완료/실패 */
}
EXPORT_SYMBOL_GPL(dw_pcie_suspend_noirq);

int dw_pcie_resume_noirq(struct dw_pcie *pci)
{
	/* NVMe: resume_noirq 콜백, NVMe 시스템 resume 시 PCIe 링크/장치 복구 */
	int ret;	/* NVMe: 반환값, NVMe resume 결과 */

	if (!pci->suspended)
		return 0;	/* NVMe: suspend 상태 아니면 skip, NVMe resume 불필요 */

	pci->suspended = false;	/* NVMe: suspend 플래그 클리어, NVMe resume 진행 */

	if (pci->pp.ops->init) {
		ret = pci->pp.ops->init(&pci->pp);	/* NVMe: vendor 초기화, NVMe SoC resume 설정 */
		if (ret) {
			dev_err(pci->dev, "Host init failed: %d\n", ret);	/* NVMe: resume 초기화 실패, NVMe 복구 불가 */
			return ret;	/* NVMe: resume 실패, NVMe 장치 비활성 */
		}
	}

	dw_pcie_setup_rc(&pci->pp);	/* NVMe: RC 재설정, NVMe 루트 포트 복구 */

	ret = dw_pcie_start_link(pci);	/* NVMe: 링크 재시작, NVMe SSD 물리 연결 복구 */
	if (ret)
		goto err_deinit;	/* NVMe: 링크 시작 실패, NVMe resume 실패 처리 */

	ret = dw_pcie_wait_for_link(pci);	/* NVMe: 링크 업 대기, NVMe SSD 연결 복구 확인 */
	if (ret == -ETIMEDOUT)
		goto err_stop_link;	/* NVMe: 링크 업 타임아웃, NVMe resume 실패 처리 */

	if (pci->pp.ops->post_init)
		pci->pp.ops->post_init(&pci->pp);	/* NVMe: vendor 후처리, NVMe SoC resume 마무리 */

	return 0;	/* NVMe: resume 완료, NVMe PCIe 복구 */

err_stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe resume 실패 시 연결 해제 */

err_deinit:
	if (pci->pp.ops->deinit)
		pci->pp.ops->deinit(&pci->pp);	/* NVMe: vendor deinit, NVMe resume 실패 시 정리 */

	return ret;	/* NVMe: resume 실패 코드 반환, NVMe 복구 실패 */
}
EXPORT_SYMBOL_GPL(dw_pcie_resume_noirq);
