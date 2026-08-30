// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for Xilinx AXI PCIe Bridge
 *
 * Copyright (c) 2012 - 2014 Xilinx, Inc.
 *
 * Based on the Tegra PCIe driver
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * ARM PCI Host generic driver.
 */
/* NVMe: 이 파일은 Xilinx AXI PCIe Root Complex(호스트 컨트롤러) 드라이버로,
 * NVMe SSD가 PCIe 버스에 연결되었을 때 PCI 코어의 물리적/전기적 계층과
 * NVMe 장치(drivers/nvme/host/pci.c)를 연결하는 다리 역할을 수행한다.
 */

#include <linux/interrupt.h>		/* PCI/NVMe: 하드웨어 인터럽트 처리. NVMe MSI/MSI-X/INTx 발생 시 호출됨 */
#include <linux/irq.h>			/* PCI/NVMe: IRQ 선언, NVMe MSI 벡터 할당/해제에 사용 */
#include <linux/irqchip/irq-msi-lib.h>	/* NVMe: MSI/MSI-X 라이브러리. NVMe 장치가 요구하는 MSI capability 설정 시 사용 */
#include <linux/irqdomain.h>		/* NVMe: 가상 IRQ -> 하드웨어 IRQ 매핑. NVMe MSI vector 할당의 핵심 자료구조 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/msi.h>			/* NVMe: MSI domain 생성 및 MSI 메시지 조합. NVMe MSI-X 대비 MSI 경로 */
#include <linux/of_address.h>		/* NVMe: DT에서 PCIe controller 레지스터 영역 획득. ECAM/NVMe BAR 매핑의 물리 주소 출처 */
#include <linux/of_pci.h>		/* NVMe: DT PCI 노드 파싱, NVMe 장치 트리 연결 정보 처리 */
#include <linux/of_platform.h>
#include <linux/of_irq.h>		/* NVMe: DT에서 컨트롤러 인터럽트 번호 획득. NVMe MSI/INTx 상향 통보 경로 */
#include <linux/pci.h>			/* NVMe: PCI 버스 열거, NVMe 장치 probe, BAR, capability 탐색의 기반 */
#include <linux/pci-ecam.h>		/* NVMe: ECAM(Configuration Space Access Method) 매크로. NVMe config read/write에 직접 사용 */
#include <linux/platform_device.h>

#include "../pci.h"			/* NVMe: PCI host bridge 날부 헤더. pci_host_probe() 등 NVMe 열거 시작점 포함 */

/* Register definitions */
#define XILINX_PCIE_REG_BIR		0x00000130	/* NVMe: Bridge Info Register. ECAM 윈도우 크기로 NVMe config space 접근 범위 결정 */
#define XILINX_PCIE_REG_IDR		0x00000138	/* NVMe: Interrupt Decode Register. 컨트롤러가 보고한 모든 인터럽트 원인(NVMe MSI/INTx/AER 등) */
#define XILINX_PCIE_REG_IMR		0x0000013c	/* NVMe: Interrupt Mask Register. NVMe 관련 인터럽트를 CPU로 전달할지 차단할지 설정 */
#define XILINX_PCIE_REG_PSCR		0x00000144	/* NVMe: Phy Status/Control Register. 링크 업 상태로 NVMe 장치 존재/접근 가능 여부 판단 */
#define XILINX_PCIE_REG_RPSC		0x00000148	/* NVMe: Root Port Status/Control Register. bridge enable. NVMe 트래픽 허용 스위치 */
#define XILINX_PCIE_REG_MSIBASE1	0x0000014c	/* NVMe: MSI 상위 주소 레지스터. NVMe 장치가 MSI 메모리 쓰기할 때 사용하는 주소 상위 32비트 */
#define XILINX_PCIE_REG_MSIBASE2	0x00000150	/* NVMe: MSI 하위 주소 레지스터. NVMe 장치가 MSI 메모리 쓰기할 때 사용하는 주소 하위 32비트 */
#define XILINX_PCIE_REG_RPEFR		0x00000154	/* NVMe: Root Port Error FIFO Read. AER 에러 메시지(NVMe 장치가 본인)의 requester ID */
#define XILINX_PCIE_REG_RPIFR1		0x00000158	/* NVMe: Root Port Interrupt FIFO Read 1. NVMe MSI/INTx 인터럽트 유효성 및 종류 디코딩 */
#define XILINX_PCIE_REG_RPIFR2		0x0000015c	/* NVMe: Root Port Interrupt FIFO Read 2. NVMe MSI 메시지 데이터(vector 번호) 저장 */

/* Interrupt registers definitions */
#define XILINX_PCIE_INTR_LINK_DOWN	BIT(0)		/* NVMe: 링크 다운. NVMe SSD 분리/재부팅 시 감지 */
#define XILINX_PCIE_INTR_ECRC_ERR	BIT(1)		/* NVMe: ECRC 오류. NVMe TLP 데이터 무결성 문제 */
#define XILINX_PCIE_INTR_STR_ERR	BIT(2)		/* NVMe: 스트리밍 오류. NVMe DMA streaming anomaly */
#define XILINX_PCIE_INTR_HOT_RESET	BIT(3)		/* NVMe: 핫 리셋. NVMe 장치 FLR/리셋 수행 시 발생 */
#define XILINX_PCIE_INTR_CFG_TIMEOUT	BIT(8)		/* NVMe: ECAM 접근 타임아웃. NVMe config read/write 실패(장치 미응답) */
#define XILINX_PCIE_INTR_CORRECTABLE	BIT(9)		/* NVMe: PCIe AER correctable 메시지. NVMe BER/CE 등 경미한 오류 */
#define XILINX_PCIE_INTR_NONFATAL	BIT(10)		/* NVMe: PCIe AER non-fatal 메시지. NVMe 복구 가능 심각 오류 */
#define XILINX_PCIE_INTR_FATAL		BIT(11)		/* NVMe: PCIe AER fatal 메시지. NVMe 치명적 오류, 보통 링크 재초기화 필요 */
#define XILINX_PCIE_INTR_INTX		BIT(16)		/* NVMe: 레거시 INTx 인터럽트. MSI/MSI-X 미지원 NVMe 장치나 초기 enumeration 시 사용 */
#define XILINX_PCIE_INTR_MSI		BIT(17)		/* NVMe: MSI 인터럽트. NVMe 장치가 MSI vector로 완료 큐/Admin 큐 알림 */
#define XILINX_PCIE_INTR_SLV_UNSUPP	BIT(20)		/* NVMe: Slave unsupported request. NVMe가 요청한 unimplemented 영역 접근 */
#define XILINX_PCIE_INTR_SLV_UNEXP	BIT(21)		/* NVMe: Slave unexpected completion. NVMe가 예상치 못한 completion 수신 */
#define XILINX_PCIE_INTR_SLV_COMPL	BIT(22)		/* NVMe: Slave completion timeout. NVMe 요청에 대한 completion 지연/손실 */
#define XILINX_PCIE_INTR_SLV_ERRP	BIT(23)		/* NVMe: Slave error poison. NVMe TLP EP 비트 설정 수신 */
#define XILINX_PCIE_INTR_SLV_CMPABT	BIT(24)		/* NVMe: Slave completer abort. NVMe 장치가 요청 중단 */
#define XILINX_PCIE_INTR_SLV_ILLBUR	BIT(25)		/* NVMe: Slave illegal burst. NVMe 비정상 버스트 트랜잭션 */
#define XILINX_PCIE_INTR_MST_DECERR	BIT(26)		/* NVMe: Master decode error. Root가 NVMe 영역 외 주소 핏코딩 */
#define XILINX_PCIE_INTR_MST_SLVERR	BIT(27)		/* NVMe: Master slave error. NVMe 장치가 Root master 요청 에러 응답 */
#define XILINX_PCIE_INTR_MST_ERRP	BIT(28)		/* NVMe: Master error poison. Root -> NVMe 경로에서 EP 표기 전송 */
#define XILINX_PCIE_IMR_ALL_MASK	0x1FF30FED	/* NVMe: IMR에 존재하는 모든 인터럽트 비트 마스크 */
#define XILINX_PCIE_IMR_ENABLE_MASK	0x1FF30F0D	/* NVMe: 실제 enable할 인터럽트 마스크. NVMe MSI/INTx/AER/링크다운 등 */
#define XILINX_PCIE_IDR_ALL_MASK	0xFFFFFFFF	/* NVMe: IDR 클리어 시 모든 비트 제거 */

/* Root Port Error FIFO Read Register definitions */
#define XILINX_PCIE_RPEFR_ERR_VALID	BIT(18)		/* NVMe: AER 에러 FIFO에 유효한 NVMe requester ID 존재 */
#define XILINX_PCIE_RPEFR_REQ_ID	GENMASK(15, 0)	/* NVMe: AER를 발생시킨 NVMe 장치의 Bus/Dev/Function requester ID */
#define XILINX_PCIE_RPEFR_ALL_MASK	0xFFFFFFFF	/* NVMe: RPEFR 클리어 시 모든 비트 쓰기 */

/* Root Port Interrupt FIFO Read Register 1 definitions */
#define XILINX_PCIE_RPIFR1_INTR_VALID	BIT(31)		/* NVMe: RPIFR1에 유효한 NVMe MSI/INTx 정보 존재 */
#define XILINX_PCIE_RPIFR1_MSI_INTR	BIT(30)		/* NVMe: 이 인터럽트가 NVMe 장치의 MSI 메모리 쓰기에 의한 것인지 표시 */
#define XILINX_PCIE_RPIFR1_INTR_MASK	GENMASK(28, 27)	/* NVMe: INTx vector 번호(INTA/B/C/D) 추출 마스크 */
#define XILINX_PCIE_RPIFR1_ALL_MASK	0xFFFFFFFF	/* NVMe: RPIFR1 클리어 값 */
#define XILINX_PCIE_RPIFR1_INTR_SHIFT	27		/* NVMe: INTx 번호를 LSB로 이동시킬 비트 수 */

/* Bridge Info Register definitions */
#define XILINX_PCIE_BIR_ECAM_SZ_MASK	GENMASK(18, 16)	/* NVMe: ECAM 윈도우 크기 필드. NVMe config 공간 접근 가능 범위 */
#define XILINX_PCIE_BIR_ECAM_SZ_SHIFT	16		/* NVMe: ECAM 크기 필드 시프트 값 */

/* Root Port Interrupt FIFO Read Register 2 definitions */
#define XILINX_PCIE_RPIFR2_MSG_DATA	GENMASK(15, 0)	/* NVMe: MSI 메시지 데이터, 즉 NVMe 장치가 쓴 MSI vector 번호 */

/* Root Port Status/control Register definitions */
#define XILINX_PCIE_REG_RPSC_BEN	BIT(0)		/* NVMe: Bridge Enable. NVMe 트래픽을 허용하는 최상위 스위치 */

/* Phy Status/Control Register definitions */
#define XILINX_PCIE_REG_PSCR_LNKUP	BIT(11)		/* NVMe: PCIe 링크 업 플래그. NVMe SSD가 전기적으로 연결되었는지 표시 */

/* Number of MSI IRQs */
#define XILINX_NUM_MSI_IRQS		128		/* NVMe: 이 Root Port가 지원하는 MSI vector 총 개수. NVMe MSI 할당 한계 */

/**
 * struct xilinx_pcie - PCIe port information
 * @dev: Device pointer
 * @reg_base: IO Mapped Register Base
 * @msi_map: Bitmap of allocated MSIs
 * @map_lock: Mutex protecting the MSI allocation
 * @msi_domain: MSI IRQ domain pointer
 * @leg_domain: Legacy IRQ domain pointer
 * @resources: Bus Resources
 */
struct xilinx_pcie {
	struct device *dev;			/* NVMe: 이 PCIe host bridge에 대응하는 struct device. DMA/IOMMU 매핑 시 사용 */
	void __iomem *reg_base;			/* NVMe: iomapped PCIe 컨트롤러 레지스터 베이스. NVMe config/MSI/INTx 레지스터 접근 출발점 */
	unsigned long msi_map[BITS_TO_LONGS(XILINX_NUM_MSI_IRQS)];	/* NVMe: MSI vector 할당 비트맵. NVMe가 요청한 MSI vector 개수만큼 영역 예약 */
	struct mutex map_lock;			/* NVMe: msi_map 동시 접근 보호. 다중 NVMe 장치의 MSI alloc/free 경쟁 방지 */
	struct irq_domain *msi_domain;		/* NVMe: MSI IRQ domain. NVMe 장치의 MSI vector를 Linux virq로 변환 */
	struct irq_domain *leg_domain;		/* NVMe: Legacy INTx IRQ domain. MSI/MSI-X 미지원 NVMe 장치의 INTx 처리 */
	struct list_head resources;		/* NVMe: PCI host bridge 자원 목록. NVMe BAR/MMIO/IO/ prefetchable 메모리 윈도우 포함 */
};

static inline u32 pcie_read(struct xilinx_pcie *pcie, u32 reg)
{
	/* NVMe: PCIe 컨트롤러 날부 레지스터를 읽음. NVMe MSI 베이스/INTx FIFO/AER 상태 등 조회에 사용 */
	return readl(pcie->reg_base + reg);
}

static inline void pcie_write(struct xilinx_pcie *pcie, u32 val, u32 reg)
{
	/* NVMe: PCIe 컨트롤러 날부 레지스터에 씀. NVMe MSI 주소 설정/인터럽트 클리어/bridge enable 등 */
	writel(val, pcie->reg_base + reg);
}

static inline bool xilinx_pcie_link_up(struct xilinx_pcie *pcie)
{
	/* NVMe: PCIe 물리 링크가 up인지 확인. NVMe SSD가 연결되지 않았거나 L0/L0s/L1 상태일 때 false */
	return (pcie_read(pcie, XILINX_PCIE_REG_PSCR) &
		XILINX_PCIE_REG_PSCR_LNKUP) ? 1 : 0;
}

/**
 * xilinx_pcie_clear_err_interrupts - Clear Error Interrupts
 * @pcie: PCIe port information
 */
static void xilinx_pcie_clear_err_interrupts(struct xilinx_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* NVMe: Root Port Error FIFO에서 AER 에러 원인 및 requester ID 읽기 */
	unsigned long val = pcie_read(pcie, XILINX_PCIE_REG_RPEFR);

	if (val & XILINX_PCIE_RPEFR_ERR_VALID) {
		/* NVMe: AER 메시지를 본인 NVMe 장치의 requester ID 기록. AER 로그와 대조 가능 */
		dev_dbg(dev, "Requester ID %lu\n",
			val & XILINX_PCIE_RPEFR_REQ_ID);
		/* NVMe: RPEFR 클리어로 다음 AER 인터럽트 수신 가능 */
		pcie_write(pcie, XILINX_PCIE_RPEFR_ALL_MASK,
			   XILINX_PCIE_REG_RPEFR);
	}
}

/**
 * xilinx_pcie_valid_device - Check if a valid device is present on bus
 * @bus: PCI Bus structure
 * @devfn: device/function
 *
 * Return: 'true' on success and 'false' if invalid device is found
 */
static bool xilinx_pcie_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	struct xilinx_pcie *pcie = bus->sysdata;

	/* Check if link is up when trying to access downstream pcie ports */
	/* NVMe: downstream(NVMe 장치 쪽) 버스 접근 전 링크 상태 확인. 링크 다운 시 config read 실패 방지 */
	if (!pci_is_root_bus(bus)) {
		if (!xilinx_pcie_link_up(pcie))
			return false;
	} else if (devfn > 0) {
		/* Only one device down on each root port */
		/* NVMe: Root Port 자신 외에는 bus 0에 다른 장치가 없음. NVMe는 downstream 버스에서 발견됨 */
		return false;
	}
	return true;
}

/**
 * xilinx_pcie_map_bus - Get configuration base
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 *
 * Return: Base address of the configuration space needed to be
 *	   accessed.
 */
static void __iomem *xilinx_pcie_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	struct xilinx_pcie *pcie = bus->sysdata;

	/* NVMe: 요청한 bus/devfn이 유효한 NVMe 장치인지 먼저 검증 */
	if (!xilinx_pcie_valid_device(bus, devfn))
		return NULL;

	/* NVMe: ECAM 공식을 이용해 NVMe 장치의 config space 주소 계산.
	 * 이 주소로 pci_generic_config_read/write가 NVMe Vendor ID/Capability/MSI 등을 읽음
	 */
	return pcie->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);
}

/* PCIe operations */
static struct pci_ops xilinx_pcie_ops = {
	.map_bus = xilinx_pcie_map_bus,	/* NVMe: config space 접근 전 버스/장치 유효성 및 ECAM 주소 반환 */
	.read	= pci_generic_config_read,	/* NVMe: 표준 ECAM read. NVMe config header/BAR/MSI cap 읽기 */
	.write	= pci_generic_config_write,	/* NVMe: 표준 ECAM write. NVMe BAR 할당/MSI enable/FLR 등 쓰기 */
};

/* MSI functions */

static void xilinx_msi_top_irq_ack(struct irq_data *d)
{
	/*
	 * xilinx_pcie_intr_handler() will have performed the Ack.
	 * Eventually, this should be fixed and the Ack be moved in
	 * the respective callbacks for INTx and MSI.
	 */
	/* NVMe: 현재는 상위 IRQ chip의 ack가 하드웨어 클리어로직에서 이미 처리됨.
	 * NVMe MSI 인터럽트 처리 후 EOI/ack는 interrupt handler 날부에서 담당.
	 */
}

static void xilinx_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct xilinx_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* NVMe: MSI 메시지가 전달될 물리 주소. NVMe 장치가 이 주소로 memory write(MWI)를 수행 */
	phys_addr_t pa = ALIGN_DOWN(virt_to_phys(pcie), SZ_4K);

	/* NVMe: MSI address 하위 32비트. NVMe는 이 주소에 vector 데이터를 쓰며 인터럽트 발생 */
	msg->address_lo = lower_32_bits(pa);
	/* NVMe: 64비트 시스템에서 MSI address 상위 32비트 */
	msg->address_hi = upper_32_bits(pa);
	/* NVMe: MSI vector 번호. NVMe 장치의 MSI Capability에 프로그래밍되어 큐 완료 알림에 사용 */
	msg->data = data->hwirq;
}

static struct irq_chip xilinx_msi_bottom_chip = {
	.name			= "Xilinx MSI",
	.irq_compose_msi_msg	= xilinx_compose_msi_msg,	/* NVMe: Linux IRQ -> NVMe MSI message 변환 */
};

static int xilinx_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct xilinx_pcie *pcie = domain->host_data;
	int hwirq, i;

	/* NVMe: 다중 NVMe 장치가 동시에 MSI vector를 요청할 때 msi_map 보호 */
	mutex_lock(&pcie->map_lock);

	/* NVMe: 요청한 개수(nr_irqs, 보통 2의 거듭제곱)만큼 연속된 MSI vector 검색.
	 * NVMe Admin Queues + I/O Queues에 필요한 vector 수만큼 할당.
	 */
	hwirq = bitmap_find_free_region(pcie->msi_map, XILINX_NUM_MSI_IRQS, order_base_2(nr_irqs));

	mutex_unlock(&pcie->map_lock);

	/* NVMe: 사용 가능한 MSI vector가 없으면 NVMe 장치는 MSI 모드로 바인딩 불가 */
	if (hwirq < 0)
		return -ENOSPC;

	/* NVMe: 할당된 hwirq 범위를 Linux virq에 연결. 각 큐 완료 인터럽트가 이 virq를 통해 전달 */
	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &xilinx_msi_bottom_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);

	return 0;
}

static void xilinx_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct xilinx_pcie *pcie = domain->host_data;

	/* NVMe: MSI vector 반환 시 동시성 보호. NVMe 제거(hotplug)와 새 NVMe 추가 경쟁 방지 */
	mutex_lock(&pcie->map_lock);

	/* NVMe: Linux virq에 연결된 hwirq 범위를 msi_map에서 반납. NVMe MSI vector 재활용 가능 */
	bitmap_release_region(pcie->msi_map, d->hwirq, order_base_2(nr_irqs));

	mutex_unlock(&pcie->map_lock);
}

static const struct irq_domain_ops xilinx_msi_domain_ops = {
	.alloc	= xilinx_msi_domain_alloc,	/* NVMe: NVMe 장치 probe 시 MSI vector 할당 콜백 */
	.free	= xilinx_msi_domain_free,	/* NVMe: NVMe 장치 remove 시 MSI vector 해제 콜백 */
};

static bool xilinx_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				     struct irq_domain *real_parent, struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;

	/* NVMe: MSI 라이브러리가 제공하는 default chip 설정을 먼저 초기화 */
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;

	/* NVMe: NVMe 장치별 MSI top-level ack 콜백 설정. 하드웨어 ack가 이미 되어 있어 빈 함수 */
	chip->irq_ack = xilinx_msi_top_irq_ack;
	return true;
}

#define XILINX_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				   MSI_FLAG_USE_DEF_CHIP_OPS	| \
				   MSI_FLAG_NO_AFFINITY)
				/* NVMe: 이 컨트롤러가 요구하는 MSI domain 플래그.
				 * NVMe MSI affinity 미지원, default domain/chip ops 사용
				 */

static const struct msi_parent_ops xilinx_msi_parent_ops = {
	.required_flags		= XILINX_MSI_FLAGS_REQUIRED,	/* NVMe: 필수 MSI 플래그 */
	.supported_flags	= MSI_GENERIC_FLAGS_MASK,	/* NVMe: 지원하는 MSI 관련 플래그 집합 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,		/* NVMe: PCI MSI bus용 domain으로 등록. NVMe MSI 할당 시 선택 */
	.prefix			= "xilinx-",
	.init_dev_msi_info	= xilinx_init_dev_msi_info,	/* NVMe: NVMe 장치별 MSI chip 초기화 콜백 */
};

static int xilinx_allocate_msi_domains(struct xilinx_pcie *pcie)
{
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(pcie->dev),	/* NVMe: MSI domain의 firmware node. ACPI/OF에서 NVMe MSI 연결 정보 */
		.ops		= &xilinx_msi_domain_ops,	/* NVMe: MSI alloc/free ops */
		.host_data	= pcie,				/* NVMe: MSI ops에서 컨트롤러 상태 접근용 */
		.size		= XILINX_NUM_MSI_IRQS,		/* NVMe: 최대 MSI vector 수. NVMe 큐 수 제한에 영향 */
	};

	/* NVMe: PCI MSI parent IRQ domain 생성. 이후 NVMe 장치가 pci_enable_msi()로 vector 요청 */
	pcie->msi_domain = msi_create_parent_irq_domain(&info, &xilinx_msi_parent_ops);
	if (!pcie->msi_domain) {
		dev_err(pcie->dev, "failed to create MSI domain\n");
		return -ENOMEM;
	}

	return 0;
}

static void xilinx_free_irq_domains(struct xilinx_pcie *pcie)
{
	/* NVMe: MSI domain 제거. NVMe 장치 remove 후 MSI vector 할당 불가 처리 */
	irq_domain_remove(pcie->msi_domain);
	/* NVMe: Legacy INTx domain 제거. MSI 미지원 NVMe 장치의 INTx 경로 폐쇄 */
	irq_domain_remove(pcie->leg_domain);
}

/* INTx Functions */

/**
 * xilinx_pcie_intx_map - Set the handler for the INTx and mark IRQ as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: HW interrupt number
 *
 * Return: Always returns 0.
 */
static int xilinx_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	/* NVMe: INTx용 dummy irq chip과 simple handler 설정. NVMe INTx 발생 시 domain 핸들링 */
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	/* NVMe: irq chip data로 host bridge private 데이터 연결. INTx 처리에서 controller 상태 접근 */
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

/* INTx IRQ Domain operations */
static const struct irq_domain_ops intx_domain_ops = {
	.map = xilinx_pcie_intx_map,		/* NVMe: INTx virq -> hwirq 매핑 콜백 */
	.xlate = pci_irqd_intx_xlate,		/* NVMe: PCI INTx(INTA/B/C/D) 번호를 hwirq로 변환. NVMe legacy INT 핀 매핑 */
};

/* PCIe HW Functions */

/**
 * xilinx_pcie_intr_handler - Interrupt Service Handler
 * @irq: IRQ number
 * @data: PCIe port information
 *
 * Return: IRQ_HANDLED on success and IRQ_NONE on failure
 */
static irqreturn_t xilinx_pcie_intr_handler(int irq, void *data)
{
	struct xilinx_pcie *pcie = (struct xilinx_pcie *)data;
	struct device *dev = pcie->dev;
	u32 val, mask, status;

	/* Read interrupt decode and mask registers */
	/* NVMe: 컨트롤러가 보고한 모든 인터럽트 원인 읽기. NVMe MSI/INTx/AER 등 포함 */
	val = pcie_read(pcie, XILINX_PCIE_REG_IDR);
	/* NVMe: 현재 enable된 인터럽트 마스크 읽기. mask off된 NVMe 이벤트는 무시 */
	mask = pcie_read(pcie, XILINX_PCIE_REG_IMR);

	/* NVMe: 실제 처리해야 할 인터럽트 = 원인 & 마스크. 0이면 이 handler는 처리하지 않음 */
	status = val & mask;
	if (!status)
		return IRQ_NONE;

	/* NVMe: PCIe 링크 다운. NVMe SSD 분리/전원 손실 시 처리(핫플러그 관련) */
	if (status & XILINX_PCIE_INTR_LINK_DOWN)
		dev_warn(dev, "Link Down\n");

	/* NVMe: ECRC 오류. NVMe TLP 데이터 무결성 침해 가능성 */
	if (status & XILINX_PCIE_INTR_ECRC_ERR)
		dev_warn(dev, "ECRC failed\n");

	/* NVMe: 스트리밍 오류. NVMe DMA streaming anomaly */
	if (status & XILINX_PCIE_INTR_STR_ERR)
		dev_warn(dev, "Streaming error\n");

	/* NVMe: 핫 리셋. NVMe 장치 FLR 또는 secondary bus reset 발생 */
	if (status & XILINX_PCIE_INTR_HOT_RESET)
		dev_info(dev, "Hot reset\n");

	/* NVMe: ECAM 접근 타임아웃. NVMe config read/write 실패, 장치 미응답 */
	if (status & XILINX_PCIE_INTR_CFG_TIMEOUT)
		dev_warn(dev, "ECAM access timeout\n");

	/* NVMe: PCIe AER correctable 메시지 수신. NVMe 장치에서 복구 가능한 오류 보고 */
	if (status & XILINX_PCIE_INTR_CORRECTABLE) {
		dev_warn(dev, "Correctable error message\n");
		/* NVMe: AER 에러 FIFO 클리어. NVMe requester ID 로깅 */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* NVMe: PCIe AER non-fatal 메시지 수신. NVMe에서 복구 가능한 심각 오류 */
	if (status & XILINX_PCIE_INTR_NONFATAL) {
		dev_warn(dev, "Non fatal error message\n");
		/* NVMe: AER FIFO 클리어 */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* NVMe: PCIe AER fatal 메시지 수신. NVMe 링크 재초기화 등 조치 필요 */
	if (status & XILINX_PCIE_INTR_FATAL) {
		dev_warn(dev, "Fatal error message\n");
		/* NVMe: AER FIFO 클리어 */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* NVMe: INTx 또는 MSI 인터럽트 발생. NVMe 장치가 완료 큐 처리를 요청한 경우 */
	if (status & (XILINX_PCIE_INTR_INTX | XILINX_PCIE_INTR_MSI)) {
		struct irq_domain *domain;

		/* NVMe: 인터럽트 FIFO Read 1에서 MSI/INTx 상세 정보 획득 */
		val = pcie_read(pcie, XILINX_PCIE_REG_RPIFR1);

		/* Check whether interrupt valid */
		/* NVMe: FIFO에 유효한 NVMe 인터럽트 정보가 있는지 확인. 없으면 오류 처리 */
		if (!(val & XILINX_PCIE_RPIFR1_INTR_VALID)) {
			dev_warn(dev, "RP Intr FIFO1 read error\n");
			goto error;
		}

		/* Decode the IRQ number */
		/* NVMe: MSI인지 INTx인지 구분. NVMe는 MSI/MSI-X 지원 시 MSI 경로로 처리 */
		if (val & XILINX_PCIE_RPIFR1_MSI_INTR) {
			/* NVMe: MSI vector 번호(RPIFR2의 message data) 획득 */
			val = pcie_read(pcie, XILINX_PCIE_REG_RPIFR2) &
				XILINX_PCIE_RPIFR2_MSG_DATA;
			/* NVMe: MSI IRQ domain 사용. 해당 virq로 NVMe 완료 큐 ISR 디스패치 */
			domain = pcie->msi_domain;
		} else {
			/* NVMe: INTx 번호(INTA/B/C/D) 추출. legacy NVMe 인터럽트 핀 매핑 */
			val = (val & XILINX_PCIE_RPIFR1_INTR_MASK) >>
				XILINX_PCIE_RPIFR1_INTR_SHIFT;
			/* NVMe: Legacy INTx domain 사용 */
			domain = pcie->leg_domain;
		}

		/* Clear interrupt FIFO register 1 */
		/* NVMe: RPIFR1 클리어로 다음 NVMe MSI/INTx 이벤트 수신 대기 */
		pcie_write(pcie, XILINX_PCIE_RPIFR1_ALL_MASK,
			   XILINX_PCIE_REG_RPIFR1);

		/* NVMe: 추출된 hwirq로 Linux IRQ 핸들러 호출. NVMe ISR(nvme_irq) 실행 */
		generic_handle_domain_irq(domain, val);
	}

	/* NVMe: Slave unsupported request. NVMe가 Root에 요청한 unimplemented 영역 */
	if (status & XILINX_PCIE_INTR_SLV_UNSUPP)
		dev_warn(dev, "Slave unsupported request\n");

	/* NVMe: Slave unexpected completion. NVMe가 예상하지 않은 completion 수신 */
	if (status & XILINX_PCIE_INTR_SLV_UNEXP)
		dev_warn(dev, "Slave unexpected completion\n");

	/* NVMe: Slave completion timeout. NVMe 요청 completion 손실/지연 */
	if (status & XILINX_PCIE_INTR_SLV_COMPL)
		dev_warn(dev, "Slave completion timeout\n");

	/* NVMe: Slave Error Poison. NVMe TLP에 EP 비트 설정됨 */
	if (status & XILINX_PCIE_INTR_SLV_ERRP)
		dev_warn(dev, "Slave Error Poison\n");

	/* NVMe: Slave Completer Abort. NVMe 장치가 Root 요청 중단 */
	if (status & XILINX_PCIE_INTR_SLV_CMPABT)
		dev_warn(dev, "Slave Completer Abort\n");

	/* NVMe: Slave Illegal Burst. NVMe 비정상 버스트 트랜잭션 */
	if (status & XILINX_PCIE_INTR_SLV_ILLBUR)
		dev_warn(dev, "Slave Illegal Burst\n");

	/* NVMe: Master decode error. Root가 NVMe BAR 영역 외 주소 접근 */
	if (status & XILINX_PCIE_INTR_MST_DECERR)
		dev_warn(dev, "Master decode error\n");

	/* NVMe: Master slave error. NVMe 장치가 Root master 요청에 에러 응답 */
	if (status & XILINX_PCIE_INTR_MST_SLVERR)
		dev_warn(dev, "Master slave error\n");

	/* NVMe: Master error poison. Root -> NVMe 경로에서 EP 표기 전송 */
	if (status & XILINX_PCIE_INTR_MST_ERRP)
		dev_warn(dev, "Master error poison\n");

error:
	/* Clear the Interrupt Decode register */
	/* NVMe: 처리한 인터럽트 비트 클리어. NVMe 다음 인터럽트가 다시 상승할 수 있도록 함 */
	pcie_write(pcie, status, XILINX_PCIE_REG_IDR);

	return IRQ_HANDLED;
}

/**
 * xilinx_pcie_init_irq_domain - Initialize IRQ domain
 * @pcie: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
static int xilinx_pcie_init_irq_domain(struct xilinx_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *pcie_intc_node;
	int ret;

	/* Setup INTx */
	/* NVMe: DT의 interrupt-controller 자식 노드 획득. NVMe INTx 라우팅 정보 출처 */
	pcie_intc_node = of_get_next_child(dev->of_node, NULL);
	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODEV;
	}

	/* NVMe: INTx IRQ domain 생성. PCI_NUM_INTX(4)개 INTA~D. MSI 미지원 NVMe 장치 사용 */
	pcie->leg_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						    &intx_domain_ops, pcie);
	of_node_put(pcie_intc_node);
	if (!pcie->leg_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENODEV;
	}

	/* Setup MSI */
	/* NVMe: 커널이 PCI_MSI를 지원하면 MSI domain 및 MSI 주소 레지스터 설정. NVMe 성능 향상 핵심 */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* NVMe: MSI 메시지가 쓰일 물리 주소. NVMe 장치는 이 주소로 memory write(MWI) */
		phys_addr_t pa = ALIGN_DOWN(virt_to_phys(pcie), SZ_4K);

		/* NVMe: MSI parent IRQ domain 생성. NVMe 장치가 pci_enable_msi()로 vector 요청 가능해짐 */
		ret = xilinx_allocate_msi_domains(pcie);
		if (ret) {
			/* NVMe: MSI domain 생성 실패 시 INTx domain도 롤백. NVMe는 INTx로 폴백 */
			irq_domain_remove(pcie->leg_domain);
			return ret;
		}

		/* NVMe: 컨트롤러에 MSI 메시지 대상 주소 상하위 32비트 설정.
		 * NVMe 장치가 이 주소로 vector 번호를 써서 인터럽트를 발생시킴
		 */
		pcie_write(pcie, upper_32_bits(pa), XILINX_PCIE_REG_MSIBASE1);
		pcie_write(pcie, lower_32_bits(pa), XILINX_PCIE_REG_MSIBASE2);
	}

	return 0;
}

/**
 * xilinx_pcie_init_port - Initialize hardware
 * @pcie: PCIe port information
 */
static void xilinx_pcie_init_port(struct xilinx_pcie *pcie)
{
	struct device *dev = pcie->dev;

	/* NVMe: PCIe 링크 상태 출력. NVMe SSD가 물리적으로 인식되는지 초기 확인 */
	if (xilinx_pcie_link_up(pcie))
		dev_info(dev, "PCIe Link is UP\n");
	else
		dev_info(dev, "PCIe Link is DOWN\n");

	/* Disable all interrupts */
	/* NVMe: 초기화 중 인터럽트 마스크 0으로 설정. NVMe MSI/INTx/AER 일단 차단 */
	pcie_write(pcie, ~XILINX_PCIE_IDR_ALL_MASK,
		   XILINX_PCIE_REG_IMR);

	/* Clear pending interrupts */
	/* NVMe: 이전에 남아 있던 NVMe/PCIe 인터럽트 원인 레지스터 클리어 */
	pcie_write(pcie, pcie_read(pcie, XILINX_PCIE_REG_IDR) &
			 XILINX_PCIE_IMR_ALL_MASK,
		   XILINX_PCIE_REG_IDR);

	/* Enable all interrupts we handle */
	/* NVMe: NVMe 운용에 필요한 인터럽트(MSI/INTx/AER/링크다운/타임아웃 등) enable */
	pcie_write(pcie, XILINX_PCIE_IMR_ENABLE_MASK, XILINX_PCIE_REG_IMR);

	/* Enable the Bridge enable bit */
	/* NVMe: PCIe bridge enable. NVMe 장치로의 메모리/IO/구성 공간 트래픽 허용 */
	pcie_write(pcie, pcie_read(pcie, XILINX_PCIE_REG_RPSC) |
			 XILINX_PCIE_REG_RPSC_BEN,
		   XILINX_PCIE_REG_RPSC);
}

/**
 * xilinx_pcie_parse_dt - Parse Device tree
 * @pcie: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
static int xilinx_pcie_parse_dt(struct xilinx_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *node = dev->of_node;
	struct resource regs;
	unsigned int irq;
	int err;

	/* NVMe: DT의 "reg" 속성에서 PCIe 컨트롤러 물리 레지스터 영역 획득. ECAM/NVMe 접근의 기반 */
	err = of_address_to_resource(node, 0, &regs);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	/* NVMe: 컨트롤터 레지스터를 ioremap. NVMe config space/MSI/INTx 레지스터 접근용 가상 주소 */
	pcie->reg_base = devm_pci_remap_cfg_resource(dev, &regs);
	if (IS_ERR(pcie->reg_base))
		return PTR_ERR(pcie->reg_base);

	/* NVMe: DT에서 컨트롤러 인터럽트 번호 획득. NVMe MSI/INTx/AER의 상위 IRQ 라인 */
	irq = irq_of_parse_and_map(node, 0);
	/* NVMe: 상위 PCIe 인터럽트 핸들러 등록. NVMe MSI/INTx/AER가 공유하는 IRQ */
	err = devm_request_irq(dev, irq, xilinx_pcie_intr_handler,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       "xilinx-pcie", pcie);
	if (err) {
		dev_err(dev, "unable to request irq %d\n", irq);
		return err;
	}

	return 0;
}

/**
 * xilinx_pcie_probe - Probe function
 * @pdev: Platform device pointer
 *
 * Return: '0' on success and error value on failure
 */
static int xilinx_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xilinx_pcie *pcie;
	struct pci_host_bridge *bridge;
	int err;

	/* NVMe: OF 기반 플랫폼 장치가 아니면 NVMe PCIe 호스트로 사용 불가 */
	if (!dev->of_node)
		return -ENODEV;

	/* NVMe: PCI host bridge 메모리 할당. NVMe 장치 열거를 위한 bridge 구조체 */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENODEV;

	/* NVMe: bridge private 영역에서 xilinx_pcie 상태 획득. NVMe와의 sysdata 연결 준비 */
	pcie = pci_host_bridge_priv(bridge);
	/* NVMe: MSI vector 비트맵 보호용 mutex 초기화. 다중 NVMe 장치 동시 probe 대비 */
	mutex_init(&pcie->map_lock);
	/* NVMe: device 포인터 저장. DMA/IOMMU/에러 메시지 출력에 활용 */
	pcie->dev = dev;

	/* NVMe: DT 파싱으로 레지스터 base/irq 획득. NVMe 접근 하드웨어 준비 */
	err = xilinx_pcie_parse_dt(pcie);
	if (err) {
		dev_err(dev, "Parsing DT failed\n");
		return err;
	}

	/* NVMe: PCIe 포트 하드웨어 초기화. 링크/인터럽트/bridge enable로 NVMe 준비 완료 */
	xilinx_pcie_init_port(pcie);

	/* NVMe: INTx/MSI IRQ domain 초기화. NVMe 장치가 인터럽트를 받을 수 있게 함 */
	err = xilinx_pcie_init_irq_domain(pcie);
	if (err) {
		dev_err(dev, "Failed creating IRQ Domain\n");
		return err;
	}

	/* NVMe: bridge에 sysdata 연결. pci_host_probe()가 NVMe 장치를 탐색할 때 사용 */
	bridge->sysdata = pcie;
	/* NVMe: PCI config read/write ops 연결. NVMe BAR/MSI capability 등을 읽고 쓸 수 있게 함 */
	bridge->ops = &xilinx_pcie_ops;

	/* NVMe: PCI 버스 열거 시작. NVMe SSD를 발견하면 drivers/nvme/host/pci.c의 nvme_probe()가 바인딩됨 */
	err = pci_host_probe(bridge);
	if (err)
		/* NVMe: 버스 열거 실패 시 IRQ domain 정리. NVMe 바인딩 기회 상실 */
		xilinx_free_irq_domains(pcie);

	return err;
}

static const struct of_device_id xilinx_pcie_of_match[] = {
	{ .compatible = "xlnx,axi-pcie-host-1.00.a", },	/* NVMe: Xilinx AXI PCIe Host IP 호환 문자열. 매치 시 NVMe 호스트로 구동 */
	{}
};

static struct platform_driver xilinx_pcie_driver = {
	.driver = {
		.name = "xilinx-pcie",
		.of_match_table = xilinx_pcie_of_match,	/* NVMe: DT compatible 매칭. NVMe PCIe host 등록 */
		.suppress_bind_attrs = true,		/* NVMe: sysfs bind/unbind 속성 억제. platform driver 자동 로드 */
	},
	.probe = xilinx_pcie_probe,				/* NVMe: PCIe host controller probe. NVMe 장치 열거의 시작점 */
};
/* NVMe: 부팅 시 내장 플랫폼 드라이버로 등록. DT 매치 시 자동으로 NVMe PCIe 호스트로 활성화 */
builtin_platform_driver(xilinx_pcie_driver);
