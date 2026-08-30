// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Based on:
 *  arch/sh/drivers/pci/pcie-sh7786.c
 *  arch/sh/drivers/pci/ops-sh7786.c
 *  Copyright (C) 2009 - 2011  Paul Mundt
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */

#include <linux/bitops.h>          /* PCI/NVMe: 비트 마스크/옵션 처리용 */
#include <linux/cleanup.h>         /* PCI/NVMe: scoped_guard 등 정리 매크로 */
#include <linux/clk.h>             /* PCI/NVMe: PCIe 버스 클록 제어 */
#include <linux/clk-provider.h>    /* PCI/NVMe: 클록 공급자 인터페이스 */
#include <linux/delay.h>           /* PCI/NVMe: PHY/링크 대기 시 msleep/udelay */
#include <linux/interrupt.h>       /* PCI/NVMe: MSI/INTx 인터럽트 핸들링 */
#include <linux/irq.h>             /* PCI/NVMe: IRQ 타입 및 irq_data */
#include <linux/irqchip/irq-msi-lib.h> /* PCI/NVMe: MSI 라이브러리, msi_lib_init_dev_msi_info */
#include <linux/irqdomain.h>       /* PCI/NVMe: MSI IRQ 도메인 생성 */
#include <linux/kernel.h>          /* PCI/NVMe: 커널 기반 매크로 및 함수 */
#include <linux/init.h>            /* PCI/NVMe: 모듈 초기화 매크로 */
#include <linux/iopoll.h>          /* PCI/NVMe: 레지스터 폴링 */
#include <linux/msi.h>             /* PCI/NVMe: MSI 메시지 구성 */
#include <linux/of_address.h>      /* PCI/NVMe: DT 주소/IRQ 리소스 파싱 */
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pci.h>             /* PCI/NVMe: PCI 핵심 열거 및 구조체 */
#include <linux/phy/phy.h>         /* PCI/NVMe: Gen3 PHY 제어 */
#include <linux/platform_device.h> /* PCI/NVMe: 플랫폼 드라이버 등록 */
#include <linux/pm_runtime.h>      /* PCI/NVMe: 전력/런타임 PM */
#include <linux/regulator/consumer.h> /* PCI/NVMe: PCIe 레귤레이터 활성화 */

#include "pcie-rcar.h"             /* PCI/NVMe: R-Car PCIe 레지스터 정의 */

/* PCI/NVMe: R-Car PCIe MSI 컨트롤러 상태; NVMe 장치가 MSI/MSI-X IRQ를 요청할 때
 * drivers/nvme/host/pci.c -> pci_alloc_irq_vectors() 경로에서 이 domain을 통해
 * hwirq를 할당받고 최종적으로 NVMe queue interrupt가 연결된다.
 */
struct rcar_msi {
	DECLARE_BITMAP(used, INT_PCI_MSI_NR); /* NVMe: 사용 중인 MSI 벡터 비트맵 */
	struct irq_domain *domain;            /* NVMe: Linux irq_domain, MSI 라우팅 허브 */
	struct mutex map_lock;                /* NVMe: used 비트맵 보호 */
	raw_spinlock_t mask_lock;             /* NVMe: MSI enable/mask 레지스터 보호 */
	int irq1;                             /* NVMe: MSI 하드웨어 IRQ 라인 1 */
	int irq2;                             /* NVMe: MSI 하드웨어 IRQ 라인 2 */
};

/* Structure representing the PCIe interface */
/* PCI/NVMe: RC(root complex) PCIe 인터페이스를 표현; pci_host_bridge_priv로
 * 할당되며 drivers/nvme/host/pci.c가 등록된 NVMe endpoint를 탐색할 때
 * 이 host의 pci_ops가 config read/write를 처리한다.
 */
struct rcar_pcie_host {
	struct rcar_pcie	pcie;          /* NVMe: 레지스터 베이스 등 PCIe 공통 상태 */
	struct phy		*phy;           /* NVMe: Gen3 PHY 핸들 */
	struct clk		*bus_clk;       /* NVMe: PCIe 버스 클록 */
	struct			rcar_msi msi;   /* NVMe: MSI 컨트롤러 상태 */
	int			(*phy_init_fn)(struct rcar_pcie_host *host); /* NVMe: SoC별 PHY 초기화 콜백 */
};

/* PCI/NVMe: L1 ASPM 절전 상태에서 깨우는 함수. NVMe SSD가 ASPM L1.2/L1.1로
 * 진입한 뒤 config access가 필요할 때(예: NVMe BAR 탐색, capability 읽기)
 * 컨트롤러가 L1에 갇히지 않도록 복구한다.
 */
static int rcar_pcie_wakeup(struct device *pcie_dev, void __iomem *pcie_base)
{
	u32 pmsr, val;
	int ret = 0;

	/* NVMe: 레지스터 베이스가 없거나 이미 suspend면 config 접근 불가 */
	if (!pcie_base || pm_runtime_suspended(pcie_dev))
		return -EINVAL;

	pmsr = readl(pcie_base + PMSR);  /* NVMe: PM 상태 레지스터 읽기 */

	/*
	 * Test if the PCIe controller received PM_ENTER_L1 DLLP and
	 * the PCIe controller is not in L1 link state. If true, apply
	 * fix, which will put the controller into L1 link state, from
	 * which it can return to L0s/L0 on its own.
	 */
	/* NVMe: 상대편(NVMe SSD)이 L1 진입을 요청했으나 RC가 아직 L1이 아니면
	 * 수동으로 L1로 복구; 그렇지 않으면 후속 NVMe config 트랜잭션이 실패한다.
	 */
	if ((pmsr & PMEL1RX) && ((pmsr & PMSTATE) != PMSTATE_L1)) {
		writel(L1IATN, pcie_base + PMCTLR);  /* NVMe: L1 진입 요청 */
		ret = readl_poll_timeout_atomic(pcie_base + PMSR, val,
						val & L1FAEG, 10, 1000);
		if (ret) {
			dev_warn_ratelimited(pcie_dev,
					     "Timeout waiting for L1 link state, ret=%d\n",
					     ret);
		}
		writel(L1FAEG | PMEL1RX, pcie_base + PMSR); /* NVMe: L1 플래그 클리어 */
	}

	return ret;
}

/* PCI/NVMe: rcar_msi 포인터로부터 상위 rcar_pcie_host 포인터 획득 */
static struct rcar_pcie_host *msi_to_host(struct rcar_msi *msi)
{
	return container_of(msi, struct rcar_pcie_host, msi);
}

/* PCI/NVMe: 4바이트 정렬된 레지스터에서 원하는 바이트 오프셋만 추출.
 * NVMe의 PCIe capability(예: MSI-X cap offset)를 1/2바이트 단위로 읽을 때
 * 사용된다.
 */
static u32 rcar_read_conf(struct rcar_pcie *pcie, int where)
{
	unsigned int shift = BITS_PER_BYTE * (where & 3);
	u32 val = rcar_pci_read_reg(pcie, where & ~3);

	return val >> shift;
}

#ifdef CONFIG_ARM
/* PCI/NVMe: ARM 32비트에서 PCIe 메모리 접근이 버스 에러를 일으킬 수 있으므로
 * fixup 예외 핸들러를 포함한 인라인 어셈블리로 감싼다. NVMe 열거 중 config
 * read/write가 잘못된 장치에 접근할 때도 시스템이 죽지 않게 한다.
 */
#define __rcar_pci_rw_reg_workaround(instr)				\
		"	.arch armv7-a\n"				\
		"1:	" instr " %1, [%2]\n"				\
		"2:	isb\n"						\
		"3:	.pushsection .text.fixup,\"ax\"\n"		\
		"	.align	2\n"					\
		"4:	mov	%0, #" __stringify(PCIBIOS_SET_FAILED) "\n" \
		"	b	3b\n"					\
		"	.popsection\n"					\
		"	.pushsection __ex_table,\"a\"\n"		\
		"	.align	3\n"					\
		"	.long	1b, 4b\n"				\
		"	.long	2b, 4b\n"				\
		"	.popsection\n"
#endif

/* PCI/NVMe: ARM 32비트에서 PCIe 레지스터 쓰기 버스 에러를 fixup으로 복구.
 * NVMe config write가 정상 완료되면 PCIBIOS_SUCCESSFUL을, 아니면 실패 코드를
 * 반환하여 NVMe 드라이버가 오류를 인지하게 한다.
 */
static int rcar_pci_write_reg_workaround(struct rcar_pcie *pcie, u32 val,
					 unsigned int reg)
{
	int error = PCIBIOS_SUCCESSFUL;
#ifdef CONFIG_ARM
	asm volatile(
		__rcar_pci_rw_reg_workaround("str")
	: "+r"(error):"r"(val), "r"(pcie->base + reg) : "memory");
#else
	rcar_pci_write_reg(pcie, val, reg);
#endif
	return error;
}

/* PCI/NVMe: ARM 32비트에서 PCIe 레지스터 읽기 버스 에러를 fixup으로 복구.
 * NVMe config read 실패 시 PCI_SET_ERROR_RESPONSE로 0xffffffff 채워
 * 상위 PCI 코드가 DEVICE_NOT_FOUND 처리를 할 수 있게 한다.
 */
static int rcar_pci_read_reg_workaround(struct rcar_pcie *pcie, u32 *val,
					unsigned int reg)
{
	int error = PCIBIOS_SUCCESSFUL;
#ifdef CONFIG_ARM
	asm volatile(
		__rcar_pci_rw_reg_workaround("ldr")
	: "+r"(error), "=r"(*val) : "r"(pcie->base + reg) : "memory");

	if (error != PCIBIOS_SUCCESSFUL)
		PCI_SET_ERROR_RESPONSE(val);
#else
	*val = rcar_pci_read_reg(pcie, reg);
#endif
	return error;
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* PCI/NVMe: PCI config space read/write의 실제 수행자. drivers/nvme/host/pci.c
 * 쪽에서 NVMe 장치의 Vendor/Device ID, BAR, MSI-X capability 등을 읽을 때
 * pci_bus_read_config_xxx() -> rcar_pcie_read_conf -> 이 함수로 진입한다.
 */
static int rcar_pcie_config_access(struct rcar_pcie_host *host,
		unsigned char access_type, struct pci_bus *bus,
		unsigned int devfn, int where, u32 *data)
{
	struct rcar_pcie *pcie = &host->pcie;
	unsigned int dev, func, reg, index;
	int ret;

	/* Wake the bus up in case it is in L1 state. */
	/* NVMe: ASPM L1 상태에서 config 접근하면 실패할 수 있으므로 먼저 깨운다. */
	ret = rcar_pcie_wakeup(pcie->dev, pcie->base);
	if (ret) {
		PCI_SET_ERROR_RESPONSE(data);
		return PCIBIOS_SET_FAILED;
	}

	dev = PCI_SLOT(devfn);     /* NVMe: 장치 번호 추출 */
	func = PCI_FUNC(devfn);    /* NVMe: 기능 번호 추출(0~7) */
	reg = where & ~3;          /* NVMe: 4바이트 정렬된 config 오프셋 */
	index = reg / 4;           /* NVMe: 낶의 4바이트 워드 인덱스 */

	/*
	 * While each channel has its own memory-mapped extended config
	 * space, it's generally only accessible when in endpoint mode.
	 * When in root complex mode, the controller is unable to target
	 * itself with either type 0 or type 1 accesses, and indeed, any
	 * controller-initiated target transfer to its own config space
	 * results in a completer abort.
	 *
	 * Each channel effectively only supports a single device, but as
	 * the same channel <-> device access works for any PCI_SLOT()
	 * value, we cheat a bit here and bind the controller's config
	 * space to devfn 0 in order to enable self-enumeration. In this
	 * case the regular ECAR/ECDR path is sidelined and the mangled
	 * config access itself is initiated as an internal bus transaction.
	 */
	/* NVMe: RC 자신의 config space(루트 브리지)를 devfn 0에 매핑. PCI 열거 시
	 * 루트 브리지의 헤더/캐패빌리티를 읽고, 이를 통해 NVMe endpoint로 향하는
	 * 버스 번호, 메모리 공간이 설정된다.
	 */
	if (pci_is_root_bus(bus)) {
		if (dev != 0)
			return PCIBIOS_DEVICE_NOT_FOUND;

		if (access_type == RCAR_PCI_ACCESS_READ)
			*data = rcar_pci_read_reg(pcie, PCICONF(index));
		else
			rcar_pci_write_reg(pcie, *data, PCICONF(index));

		return PCIBIOS_SUCCESSFUL;
	}

	/* Clear errors */
	/* NVMe: 이전 config 트랜잭션의 에러 플래그 클리어(UR, CA 등) */
	rcar_pci_write_reg(pcie, rcar_pci_read_reg(pcie, PCIEERRFR), PCIEERRFR);

	/* Set the PIO address */
	/* NVMe: 버스/장치/함수/레지스터로 PCIe config 주소 구성 */
	rcar_pci_write_reg(pcie, PCIE_CONF_BUS(bus->number) |
		PCIE_CONF_DEV(dev) | PCIE_CONF_FUNC(func) | reg, PCIECAR);

	/* Enable the configuration access */
	/* NVMe: 상위 버스(루트 버스 직속)면 Type0, 하위 버스로 브리지드 되면 Type1 */
	if (pci_is_root_bus(bus->parent))
		rcar_pci_write_reg(pcie, PCIECCTLR_CCIE | TYPE0, PCIECCTLR);
	else
		rcar_pci_write_reg(pcie, PCIECCTLR_CCIE | TYPE1, PCIECCTLR);

	/* Check for errors */
	/* NVMe: 지원되지 않는 요청(UR) 발생 시 해당 NVMe 장치가 응답하지 않음 */
	if (rcar_pci_read_reg(pcie, PCIEERRFR) & UNSUPPORTED_REQUEST)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* Check for master and target aborts */
	/* NVMe: 루트 브리지 PCI STATUS의 master/target abort 체크 */
	if (rcar_read_conf(pcie, RCONF(PCI_STATUS)) &
		(PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT))
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (access_type == RCAR_PCI_ACCESS_READ)
		ret = rcar_pci_read_reg_workaround(pcie, data, PCIECDR);
	else
		ret = rcar_pci_write_reg_workaround(pcie, *data, PCIECDR);

	/* Disable the configuration access */
	/* NVMe: config 트랜잭션 완료 후 컨트롤러 config 인터페이스 비활성화 */
	rcar_pci_write_reg(pcie, 0, PCIECCTLR);

	return ret;
}

/* PCI/NVMe: pci_ops.read 콜백. drivers/nvme/host/pci.c가 NVMe BAR나 capability
 * 읽기를 요청하면 pci_bus_read_config_dword()를 통해 이 함수가 호출된다.
 */
static int rcar_pcie_read_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	struct rcar_pcie_host *host = bus->sysdata;
	int ret;

	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_READ,
				      bus, devfn, where, val);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/* NVMe: 1/2바이트 읽기 시 상위 비트를 시프트로 제거하여 리턴 */
	if (size == 1)
		*val = (*val >> (BITS_PER_BYTE * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (BITS_PER_BYTE * (where & 2))) & 0xffff;

	dev_dbg(&bus->dev, "pcie-config-read: bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		bus->number, devfn, where, size, *val);

	return ret;
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* PCI/NVMe: pci_ops.write 콜백. NVMe BAR enable, bus master enable, MSI-X
 * enable 등을 쓸 때 호출된다. read-modify-write로 1/2/4바이트 쓰기를 처리.
 */
static int rcar_pcie_write_conf(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	struct rcar_pcie_host *host = bus->sysdata;
	unsigned int shift;
	u32 data;
	int ret;

	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_READ,
				      bus, devfn, where, &data);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	dev_dbg(&bus->dev, "pcie-config-write: bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		bus->number, devfn, where, size, val);

	/* NVMe: 쓰기 크기에 따라 기존 값의 해당 바이트만 교체(RMW) */
	if (size == 1) {
		shift = BITS_PER_BYTE * (where & 3);
		data &= ~(0xff << shift);
		data |= ((val & 0xff) << shift);
	} else if (size == 2) {
		shift = BITS_PER_BYTE * (where & 2);
		data &= ~(0xffff << shift);
		data |= ((val & 0xffff) << shift);
	} else
		data = val;

	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_WRITE,
				      bus, devfn, where, &data);

	return ret;
}

/* PCI/NVMe: 이 RC용 pci_ops. Linux PCI 코어가 이 host 아래 NVMe 장치를
 * 열거할 때 read/write_conf를 호출하여 config space를 접근한다.
 */
static struct pci_ops rcar_pcie_ops = {
	.read	= rcar_pcie_read_conf,
	.write	= rcar_pcie_write_conf,
};

/* PCI/NVMe: 링크를 2.5GT/s에서 5GT/s로 강제 전환. NVMe SSD 성능에 직접적인
 * 영향을 주는 링크 속도 설정 단계.
 */
static void rcar_pcie_force_speedup(struct rcar_pcie *pcie)
{
	struct device *dev = pcie->dev;
	unsigned int timeout = 1000;
	u32 macsr;

	/* NVMe: 5GT/s 지원하지 않는 버전이면 조기 리턴 */
	if ((rcar_pci_read_reg(pcie, MACS2R) & LINK_SPEED) != LINK_SPEED_5_0GTS)
		return;

	/* NVMe: 이미 속도 변경 중이면 중복 시도 방지 */
	if (rcar_pci_read_reg(pcie, MACCTLR) & SPEED_CHANGE) {
		dev_err(dev, "Speed change already in progress\n");
		return;
	}

	macsr = rcar_pci_read_reg(pcie, MACSR);
	/* NVMe: 이미 5GT/s면 종료 */
	if ((macsr & LINK_SPEED) == LINK_SPEED_5_0GTS)
		goto done;

	/* Set target link speed to 5.0 GT/s */
	/* NVMe: Link Control2 레지스터의 target speed를 5GT/s로 설정 */
	rcar_rmw32(pcie, EXPCAP(12), PCI_EXP_LNKSTA_CLS,
		   PCI_EXP_LNKSTA_CLS_5_0GB);

	/* Set speed change reason as intentional factor */
	rcar_rmw32(pcie, MACCGSPSETR, SPCNGRSN, 0);

	/* Clear SPCHGFIN, SPCHGSUC, and SPCHGFAIL */
	/* NVMe: 이전 속도 변경 플래그 클리어 */
	if (macsr & (SPCHGFIN | SPCHGSUC | SPCHGFAIL))
		rcar_pci_write_reg(pcie, macsr, MACSR);

	/* Start link speed change */
	/* NVMe: 속도 변경 시작 */
	rcar_rmw32(pcie, MACCTLR, SPEED_CHANGE, SPEED_CHANGE);

	/* NVMe: 변경 완료(SPCHGFIN) 폴링, 실패 시 NVMe 링크 속도가 느려짐 */
	while (timeout--) {
		macsr = rcar_pci_read_reg(pcie, MACSR);
		if (macsr & SPCHGFIN) {
			/* Clear the interrupt bits */
			rcar_pci_write_reg(pcie, macsr, MACSR);

			if (macsr & SPCHGFAIL)
				dev_err(dev, "Speed change failed\n");

			goto done;
		}

		msleep(1);
	}

	dev_err(dev, "Speed change timed out\n");

done:
	dev_info(dev, "Current link speed is %s GT/s\n",
		 (macsr & LINK_SPEED) == LINK_SPEED_5_0GTS ? "5" : "2.5");
}

/* PCI/NVMe: PCIe 컨트롤러 하드웨어 활성화. outbound 윈도우를 설정하여
 * NVMe 장치의 MMIO BAR와 DMA가 시스템 메모리를 볼 수 있도록 한다.
 */
static void rcar_pcie_hw_enable(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	struct resource_entry *win;
	LIST_HEAD(res);
	int i = 0;

	/* Try setting 5 GT/s link speed */
	/* NVMe: 최고 링크 속도로 설정해 NVMe throughput 향상 시도 */
	rcar_pcie_force_speedup(pcie);

	/* Setup PCI resources */
	/* NVMe: bridge->windows에 등록된 I/O/MEM 자원을 outbound ATU로 변환;
	 * NVMe SSD의 BAR가 이 범위에 매핑되고, DMA/CPU 모두 접근 가능해진다.
	 */
	resource_list_for_each_entry(win, &bridge->windows) {
		struct resource *res = win->res;

		if (!res->flags)
			continue;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
		case IORESOURCE_MEM:
			rcar_pcie_set_outbound(pcie, i, win);
			i++;
			break;
		}
	}
}

/* PCI/NVMe: PCI host bridge 등록 및 열거 시작. pci_host_probe()가
 * NVMe endpoint를 발견하고 drivers/nvme/host/pci.c를 bind한다.
 */
static int rcar_pcie_enable(struct rcar_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);

	rcar_pcie_hw_enable(host);

	/* NVMe: 버스 번호를 재할당해 NVMe 장치가 올바른 bus/devfn을 얻게 함 */
	pci_add_flags(PCI_REASSIGN_ALL_BUS);

	/* NVMe: host bridge가 이 드라이버의 pci_ops를 사용하도록 연결 */
	bridge->sysdata = host;
	bridge->ops = &rcar_pcie_ops;

	/* NVMe: PCI 코어 열수행; NVMe SSD가 발견되면 nvme_probe_pci가 호출됨 */
	return pci_host_probe(bridge);
}

/* PCI/NVMe: PHY 접근 ACK 폴링. PHY 레지스터 쓰기/읽기가 완료될 때까지 기다림 */
static int phy_wait_for_ack(struct rcar_pcie *pcie)
{
	struct device *dev = pcie->dev;
	unsigned int timeout = 100;

	while (timeout--) {
		if (rcar_pci_read_reg(pcie, H1_PCIEPHYADRR) & PHY_ACK)
			return 0;

		udelay(100);
	}

	dev_err(dev, "Access to PCIe phy timed out\n");

	return -ETIMEDOUT;
}

/* PCI/NVMe: H1 SoC용 PHY 레지스터 쓰기. 링크 물리 계층 튜닝.
 * 링크 품질이 좋아야 NVMe SSD의 안정적인 DMA 및 인터럽트가 보장된다.
 */
static void phy_write_reg(struct rcar_pcie *pcie,
			  unsigned int rate, u32 addr,
			  unsigned int lane, u32 data)
{
	u32 phyaddr;

	/* NVMe: rate/lane/addr로 PHY 주소 구성, WRITE_CMD 포함 */
	phyaddr = WRITE_CMD |
		((rate & 1) << RATE_POS) |
		((lane & 0xf) << LANE_POS) |
		((addr & 0xff) << ADR_POS);

	/* Set write data */
	rcar_pci_write_reg(pcie, data, H1_PCIEPHYDOUTR);
	rcar_pci_write_reg(pcie, phyaddr, H1_PCIEPHYADRR);

	/* Ignore errors as they will be dealt with if the data link is down */
	phy_wait_for_ack(pcie);

	/* Clear command */
	rcar_pci_write_reg(pcie, 0, H1_PCIEPHYDOUTR);
	rcar_pci_write_reg(pcie, 0, H1_PCIEPHYADRR);

	/* Ignore errors as they will be dealt with if the data link is down */
	phy_wait_for_ack(pcie);
}

/* PCI/NVMe: R-Car PCIe 컨트롤러의 공통 하드웨어 초기화. Root Port로 설정하고
 * PCIe 링크를 수립. 이 링크가 NVMe SSD와의 물리적 연결이다.
 */
static int rcar_pcie_hw_init(struct rcar_pcie *pcie)
{
	int err;

	/* Begin initialization */
	/* NVMe: 트랜잭션 레이어 제어 레지스터 초기화 */
	rcar_pci_write_reg(pcie, 0, PCIETCTLR);

	/* Set mode */
	/* NVMe: PCIe RC(Root Complex) 모드로 설정 */
	rcar_pci_write_reg(pcie, 1, PCIEMSR);

	err = rcar_pcie_wait_for_phyrdy(pcie); /* NVMe: PHY ready 대기 */
	if (err)
		return err;

	/*
	 * Initial header for port config space is type 1, set the device
	 * class to match. Hardware takes care of propagating the IDSETR
	 * settings, so there is no need to bother with a quirk.
	 */
	/* NVMe: 루트 포트 클래스 코드(PCI bridge) 설정; NVMe SSD가 위에 탑재됨 */
	rcar_pci_write_reg(pcie, PCI_CLASS_BRIDGE_PCI_NORMAL << 8, IDSETR1);

	/*
	 * Setup Secondary Bus Number & Subordinate Bus Number, even though
	 * they aren't used, to avoid bridge being detected as broken.
	 */
	/* NVMe: 루트 브리지의 secondary/subordinate 버스 번호 초기화;
	 * 이 번호들이 정상이어야 NVMe endpoint의 bus 1 탐색이 이루어진다.
	 */
	rcar_rmw32(pcie, RCONF(PCI_SECONDARY_BUS), 0xff, 1);
	rcar_rmw32(pcie, RCONF(PCI_SUBORDINATE_BUS), 0xff, 1);

	/* Initialize default capabilities. */
	/* NVMe: PCIe capability 헤더를 루트 포트용으로 설정 */
	rcar_rmw32(pcie, REXPCAP(0), 0xff, PCI_CAP_ID_EXP);
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_FLAGS),
		PCI_EXP_FLAGS_TYPE, PCI_EXP_TYPE_ROOT_PORT << 4);
	rcar_rmw32(pcie, RCONF(PCI_HEADER_TYPE), PCI_HEADER_TYPE_MASK,
		PCI_HEADER_TYPE_BRIDGE);

	/* Enable data link layer active state reporting */
	/* NVMe: 데이터 링크 레이어 상태 보고 활성화, NVMe ASPM/PM 관리에 사용 */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_LNKCAP), PCI_EXP_LNKCAP_DLLLARC,
		PCI_EXP_LNKCAP_DLLLARC);

	/* Write out the physical slot number = 0 */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_SLTCAP), PCI_EXP_SLTCAP_PSN, 0);

	/* Set the completion timer timeout to the maximum 50ms. */
	/* NVMe: completion timeout 최대 50ms; 느린 NVMe 응답 시 UR 방지 */
	rcar_rmw32(pcie, TLCTLR + 1, 0x3f, 50);

	/* Terminate list of capabilities (Next Capability Offset=0) */
	/* NVMe: capability 체인 종료 */
	rcar_rmw32(pcie, RVCCAP(0), 0xfff00000, 0);

	/* Enable MSI */
	/* NVMe: MSI 기능을 RC 측에서 활성화; NVMe SSD가 MSI 메시지 본능 */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		rcar_pci_write_reg(pcie, 0x801f0000, PCIEMSITXR);

	rcar_pci_write_reg(pcie, MACCTLR_INIT_VAL, MACCTLR);

	/* Finish initialization - establish a PCI Express link */
	/* NVMe: 링크 트레이닝 시작; NVMe SSD가 연결되어 있으면 L0 상태 진입 */
	rcar_pci_write_reg(pcie, CFINIT, PCIETCTLR);

	/* This will timeout if we don't have a link. */
	/* NVMe: 데이터 링크 업(dl_up) 폴링; NVMe SSD가 부팅 시 준비되지 않았으면 실패 */
	err = rcar_pcie_wait_for_dl(pcie);
	if (err)
		return err;

	/* Enable INTx interrupts */
	/* NVMe: 레거시 INTx 인터럽트 활성화; MSI/MSI-X를 사용하지 않는 NVMe fallback용 */
	rcar_rmw32(pcie, PCIEINTXR, 0, 0xF << 8);

	wmb(); /* NVMe: 레지스터 쓰기 완료 보장 */

	return 0;
}

/* PCI/NVMe: R-Car H1 SoC PHY 시퀀스. PHY 튜닝 값은 링크 마진에 영향을
 * 줘 NVMe SSD의 안정성에 기여한다.
 */
static int rcar_pcie_phy_init_h1(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/* Initialize the phy */
	phy_write_reg(pcie, 0, 0x42, 0x1, 0x0EC34191);
	phy_write_reg(pcie, 1, 0x42, 0x1, 0x0EC34180);
	phy_write_reg(pcie, 0, 0x43, 0x1, 0x00210188);
	phy_write_reg(pcie, 1, 0x43, 0x1, 0x00210188);
	phy_write_reg(pcie, 0, 0x44, 0x1, 0x015C0014);
	phy_write_reg(pcie, 1, 0x44, 0x1, 0x015C0014);
	phy_write_reg(pcie, 1, 0x4C, 0x1, 0x786174A0);
	phy_write_reg(pcie, 1, 0x4D, 0x1, 0x048000BB);
	phy_write_reg(pcie, 0, 0x51, 0x1, 0x079EC062);
	phy_write_reg(pcie, 0, 0x52, 0x1, 0x20000000);
	phy_write_reg(pcie, 1, 0x52, 0x1, 0x20000000);
	phy_write_reg(pcie, 1, 0x56, 0x1, 0x00003806);

	phy_write_reg(pcie, 0, 0x60, 0x1, 0x004B03A5);
	phy_write_reg(pcie, 0, 0x64, 0x1, 0x3F0F1F0F);
	phy_write_reg(pcie, 0, 0x66, 0x1, 0x00008000);

	return 0;
}

/* PCI/NVMe: Gen2 SoC PHY 초기화. datasheet 50.3.1절 값 사용.
 * PHY 레지스터 값이 좋아야 NVMe DMA/인터럽트가 안정적이다.
 */
static int rcar_pcie_phy_init_gen2(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/*
	 * These settings come from the R-Car Series, 2nd Generation User's
	 * Manual, section 50.3.1 (2) Initialization of the physical layer.
	 */
	rcar_pci_write_reg(pcie, 0x000f0030, GEN2_PCIEPHYADDR);
	rcar_pci_write_reg(pcie, 0x00381203, GEN2_PCIEPHYDATA);
	rcar_pci_write_reg(pcie, 0x00000001, GEN2_PCIEPHYCTRL);
	rcar_pci_write_reg(pcie, 0x00000006, GEN2_PCIEPHYCTRL);

	rcar_pci_write_reg(pcie, 0x000f0054, GEN2_PCIEPHYADDR);
	/* The following value is for DC connection, no termination resistor */
	rcar_pci_write_reg(pcie, 0x13802007, GEN2_PCIEPHYDATA);
	rcar_pci_write_reg(pcie, 0x00000001, GEN2_PCIEPHYCTRL);
	rcar_pci_write_reg(pcie, 0x00000006, GEN2_PCIEPHYCTRL);

	return 0;
}

/* PCI/NVMe: Gen3 SoC용 PHY framework 초기화. PHY 전원이 켜져야 PCIe 링크가
 * 활성화되고 NVMe SSD가 탐지된다.
 */
static int rcar_pcie_phy_init_gen3(struct rcar_pcie_host *host)
{
	int err;

	err = phy_init(host->phy);      /* NVMe: PHY 초기화 */
	if (err)
		return err;

	err = phy_power_on(host->phy);  /* NVMe: PHY 전원 인가 */
	if (err)
		phy_exit(host->phy);

	return err;
}

/* PCI/NVMe: MSI 하드웨어 인터럽트 핸들러. NVMe SSD가 MSI 메모리 쓰기로
 * 알린 queue completion interrupt를 수신한다.
 */
static irqreturn_t rcar_pcie_msi_irq(int irq, void *data)
{
	struct rcar_pcie_host *host = data;
	struct rcar_pcie *pcie = &host->pcie;
	struct rcar_msi *msi = &host->msi;
	struct device *dev = pcie->dev;
	unsigned long reg;

	/* NVMe: MSI pending 상태 레지스터 읽기 */
	reg = rcar_pci_read_reg(pcie, PCIEMSIFR);

	/* MSI & INTx share an interrupt - we only handle MSI here */
	/* NVMe: pending이 없으면 IRQ_NONE으로 INTx 핸들러에게 양도 */
	if (!reg)
		return IRQ_NONE;

	/* NVMe: pending MSI 벡터를 순회하며 Linux irq_domain으로 dispatch;
	 * drivers/nvme/host/pci.c가 등록한 queue irq handler가 호출된다.
	 */
	while (reg) {
		unsigned int index = find_first_bit(&reg, 32);
		int ret;

		ret = generic_handle_domain_irq(msi->domain, index);
		if (ret) {
			/* Unknown MSI, just clear it */
			dev_dbg(dev, "unexpected MSI\n");
			rcar_pci_write_reg(pcie, BIT(index), PCIEMSIFR);
		}

		/* see if there's any more pending in this vector */
		reg = rcar_pci_read_reg(pcie, PCIEMSIFR);
	}

	return IRQ_HANDLED;
}

/* PCI/NVMe: MSI 인터럽트 acknowledge. NVMe queue ISR 종료 후 해당 벡터
 * pending 비트를 클리어한다.
 */
static void rcar_msi_irq_ack(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;

	/* clear the interrupt */
	rcar_pci_write_reg(pcie, BIT(d->hwirq), PCIEMSIFR);
}

/* PCI/NVMe: MSI 벡터 mask. NVMe 드라이버가 특정 queue interrupt를 비활성화할
 * 때 호출된다. mask_lock으로 레지스터 RMW를 보호한다.
 */
static void rcar_msi_irq_mask(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	u32 value;

	/* NVMe: MSI enable 레지스터에서 해당 비트만 클리어 */
	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		value = rcar_pci_read_reg(pcie, PCIEMSIIER);
		value &= ~BIT(d->hwirq);
		rcar_pci_write_reg(pcie, value, PCIEMSIIER);
	}
}

/* PCI/NVMe: MSI 벡터 unmask. NVMe 드라이버가 queue interrupt를 다시 활성화
 * 할 때 호출된다.
 */
static void rcar_msi_irq_unmask(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	u32 value;

	/* NVMe: MSI enable 레지스터에서 해당 비트만 설정 */
	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		value = rcar_pci_read_reg(pcie, PCIEMSIIER);
		value |= BIT(d->hwirq);
		rcar_pci_write_reg(pcie, value, PCIEMSIIER);
	}
}

/* PCI/NVMe: MSI 메시지(address/data) 구성. pci_write_msi_msg() 경로에서
 * NVMe endpoint의 MSI capability에 쓰일 주소/데이터를 만든다.
 */
static void rcar_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(data);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;

	/* NVMe: MSI target 주소 하위 32비트; MSIFE 플래그 제거 */
	msg->address_lo = rcar_pci_read_reg(pcie, PCIEMSIALR) & ~MSIFE;
	/* NVMe: MSI target 주소 상위 32비트 */
	msg->address_hi = rcar_pci_read_reg(pcie, PCIEMSIAUR);
	/* NVMe: MSI 데이터 = hwirq 번호; NVMe가 쓰면 해당 virq로 라우팅 */
	msg->data = data->hwirq;
}

/* PCI/NVMe: MSI 하위(bottom) irq_chip. irq_mask/unmask/ack과 MSI 메시지
 * 구성을 담당한다.
 */
static struct irq_chip rcar_msi_bottom_chip = {
	.name			= "R-Car MSI",
	.irq_ack		= rcar_msi_irq_ack,
	.irq_mask		= rcar_msi_irq_mask,
	.irq_unmask		= rcar_msi_irq_unmask,
	.irq_compose_msi_msg	= rcar_compose_msi_msg,
};

/* PCI/NVMe: irq_domain alloc 콜백. pci_alloc_irq_vectors()가 NVMe queue들에
 * 필요한 MSI 벡터를 요청할 때 bitmap에서 hwirq를 할당한다.
 */
static int rcar_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct rcar_msi *msi = domain->host_data;
	unsigned int i;
	int hwirq;

	/* NVMe: map_lock 획득 후 free region 탐색 */
	mutex_lock(&msi->map_lock);

	/* NVMe: INT_PCI_MSI_NR 범위 내에서 연속된 nr_irqs개의 MSI 벡터 할당;
	 * NVMe가 요구하는 queue 개수만큼의 벡터가 확볼됨.
	 */
	hwirq = bitmap_find_free_region(msi->used, INT_PCI_MSI_NR, order_base_2(nr_irqs));

	mutex_unlock(&msi->map_lock);

	if (hwirq < 0)
		return -ENOSPC;

	/* NVMe: 할당된 hwirq와 bottom chip를 각 virq에 연결 */
	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &rcar_msi_bottom_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);

	return 0;
}

/* PCI/NVMe: irq_domain free 콜백. NVMe 장치 제거 시 pci_free_irq_vectors()를
 * 통해 할당받은 MSI 벡터를 반납한다.
 */
static void rcar_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct rcar_msi *msi = domain->host_data;

	mutex_lock(&msi->map_lock);

	/* NVMe: used 비트맵에서 해당 region 해제 */
	bitmap_release_region(msi->used, d->hwirq, order_base_2(nr_irqs));

	mutex_unlock(&msi->map_lock);
}

/* PCI/NVMe: MSI irq_domain ops. alloc/free만 직접 구현한다. */
static const struct irq_domain_ops rcar_msi_domain_ops = {
	.alloc	= rcar_msi_domain_alloc,
	.free	= rcar_msi_domain_free,
};

/* PCI/NVMe: MSI 상위(parent) 도메인에 필요한 플래그. 기본 domain/chip ops를
 * 사용하고 PCI MSI 마스크 parent 동작을 요구한다. affinity는 지원하지 않음.
 */
#define RCAR_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_PCI_MSI_MASK_PARENT	| \
				 MSI_FLAG_NO_AFFINITY)

/* PCI/NVMe: 지원하는 MSI 플래그. NVMe가 요구하는 multi MSI도 지원한다. */
#define RCAR_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_MULTI_PCI_MSI)

/* PCI/NVMe: MSI parent ops. PCI 코어가 NVMe 장치의 MSI alloc/free를 이 parent
 * 를 통해 처리하도록 연결한다.
 */
static const struct msi_parent_ops rcar_msi_parent_ops = {
	.required_flags		= RCAR_MSI_FLAGS_REQUIRED,
	.supported_flags	= RCAR_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "RCAR-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* PCI/NVMe: MSI irq_domain 생성. 성공 시 msi->domain이 설정되어 NVMe
 * 장치들이 MSI 벡터를 할당받을 수 있다.
 */
static int rcar_allocate_domains(struct rcar_msi *msi)
{
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(pcie->dev),
		.ops		= &rcar_msi_domain_ops,
		.host_data	= msi,
		.size		= INT_PCI_MSI_NR,
	};

	/* NVMe: msi_create_parent_irq_domain()로 PCI MSI parent domain 생성 */
	msi->domain = msi_create_parent_irq_domain(&info, &rcar_msi_parent_ops);
	if (!msi->domain) {
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}

	return 0;
}

/* PCI/NVMe: MSI irq_domain 제거. 드라이버 해제 시 호출. */
static void rcar_free_domains(struct rcar_msi *msi)
{
	irq_domain_remove(msi->domain);
}

/* PCI/NVMe: MSI 컨트롤러 활성화. mutex/spinlock 초기화, irq_domain 생성,
 * 하드웨어 MSI IRQ 등록, MSI target address 설정을 수행한다.
 */
static int rcar_pcie_enable_msi(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;
	struct device *dev = pcie->dev;
	struct rcar_msi *msi = &host->msi;
	struct resource res;
	int err;

	/* NVMe: MSI 벡터 할당용 mutex 및 mask 레지스터용 raw spinlock 초기화 */
	mutex_init(&msi->map_lock);
	raw_spin_lock_init(&msi->mask_lock);

	err = of_address_to_resource(dev->of_node, 0, &res);
	if (err)
		return err;

	/* NVMe: irq_domain 생성; NVMe MSI 할당의 입구점 */
	err = rcar_allocate_domains(msi);
	if (err)
		return err;

	/* Two IRQs are for MSI, but they are also used for non-MSI IRQs */
	/* NVMe: MSI/INTx 공유 IRQ 등록; threaded IRQ는 사용하지 않음(NVMe low latency) */
	err = devm_request_irq(dev, msi->irq1, rcar_pcie_msi_irq,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       rcar_msi_bottom_chip.name, host);
	if (err < 0) {
		dev_err(dev, "failed to request IRQ: %d\n", err);
		goto err;
	}

	err = devm_request_irq(dev, msi->irq2, rcar_pcie_msi_irq,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       rcar_msi_bottom_chip.name, host);
	if (err < 0) {
		dev_err(dev, "failed to request IRQ: %d\n", err);
		goto err;
	}

	/* Disable all MSIs */
	/* NVMe: 모든 MSI 벡터를 mask 상태로 시작 */
	rcar_pci_write_reg(pcie, 0, PCIEMSIIER);

	/*
	 * Setup MSI data target using RC base address, which is guaranteed
	 * to be in the low 32bit range on any R-Car HW.
	 */
	/* NVMe: NVMe SSD가 MSI 메모리 쓰기할 목적지 주소를 RC base로 설정;
	 * NVMe가 이 주소로 쓰면 RC가 MSI IRQ를 발생시킨다.
	 */
	rcar_pci_write_reg(pcie, lower_32_bits(res.start) | MSIFE, PCIEMSIALR);
	rcar_pci_write_reg(pcie, upper_32_bits(res.start), PCIEMSIAUR);

	return 0;

err:
	rcar_free_domains(msi);
	return err;
}

/* PCI/NVMe: MSI 컨트롤ler 해제. 모든 MSI를 비활성화하고 MSI target address
 * 를 클리어하여 NVMe 장치가 더 이상 MSI를 발산하지 못하게 한다.
 */
static void rcar_pcie_teardown_msi(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/* Disable all MSI interrupts */
	rcar_pci_write_reg(pcie, 0, PCIEMSIIER);

	/* Disable address decoding of the MSI interrupt, MSIFE */
	rcar_pci_write_reg(pcie, 0, PCIEMSIALR);

	rcar_free_domains(&host->msi);
}

/* PCI/NVMe: DT에서 PHY, MMIO, 클록, IRQ 리소스를 획득. NVMe SSD가 동작하기
 * 위한 모든 하드웨어 리소스가 여기서 준비된다.
 */
static int rcar_pcie_get_resources(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;
	struct device *dev = pcie->dev;
	struct resource res;
	int err, i;

	/* NVMe: optional PHY 획득(Gen3 SoC에서 사용) */
	host->phy = devm_phy_optional_get(dev, "pcie");
	if (IS_ERR(host->phy))
		return PTR_ERR(host->phy);

	/* NVMe: DT reg 0으로 MMIO 리소스 획득; config/MSI 레지스터 접근에 사용 */
	err = of_address_to_resource(dev->of_node, 0, &res);
	if (err)
		return err;

	pcie->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	/* NVMe: PCIe 버스 클록 획득 및 활성화 준비 */
	host->bus_clk = devm_clk_get(dev, "pcie_bus");
	if (IS_ERR(host->bus_clk)) {
		dev_err(dev, "cannot get pcie bus clock\n");
		return PTR_ERR(host->bus_clk);
	}

	/* NVMe: 첫 번째 DT IRQ는 MSI/INTx용; 실패 시 mapping 해제 필요 */
	i = irq_of_parse_and_map(dev->of_node, 0);
	if (!i) {
		dev_err(dev, "cannot get platform resources for msi interrupt\n");
		err = -ENOENT;
		goto err_irq1;
	}
	host->msi.irq1 = i;

	/* NVMe: 두 번째 DT IRQ는 MSI/INTx용 */
	i = irq_of_parse_and_map(dev->of_node, 1);
	if (!i) {
		dev_err(dev, "cannot get platform resources for msi interrupt\n");
		err = -ENOENT;
		goto err_irq2;
	}
	host->msi.irq2 = i;

	return 0;

err_irq2:
	irq_dispose_mapping(host->msi.irq1);
err_irq1:
	return err;
}

/* PCI/NVMe: inbound(DMA) 범위 설정. NVMe SSD가 시스템 DRAM으로 DMA를 수행할
 * 때 CPU 물리 주소를 PCIe 주소로 변환하는 mapping을 만든다. IOMMU가 없으면
 * 이 설정이 DMA 통로의 핵심이다.
 */
static int rcar_pcie_inbound_ranges(struct rcar_pcie *pcie,
				    struct resource_entry *entry,
				    int *index)
{
	u64 restype = entry->res->flags;
	u64 cpu_addr = entry->res->start;
	u64 cpu_end = entry->res->end;
	u64 pci_addr = entry->res->start - entry->offset;
	u32 flags = LAM_64BIT | LAR_ENABLE;
	u64 mask;
	u64 size = resource_size(entry->res);
	int idx = *index;

	/* NVMe: prefetchable 메모리 영역 표시(64비트 BAR 등에 유리) */
	if (restype & IORESOURCE_PREFETCH)
		flags |= LAM_PREFETCH;

	/* NVMe: 큰 DMA 범위를 HW 한계에 맞춰 여러 inbound window로 분할 */
	while (cpu_addr < cpu_end) {
		if (idx >= MAX_NR_INBOUND_MAPS - 1) {
			dev_err(pcie->dev, "Failed to map inbound regions!\n");
			return -EINVAL;
		}

		/*
		 * If the size of the range is larger than the alignment of
		 * the start address, we have to use multiple entries to
		 * perform the mapping.
		 */
		/* NVMe: 시작 주소 정렬에 맞춰 크기를 자름; 1:1 매핑을 유지 */
		if (cpu_addr > 0) {
			unsigned long nr_zeros = __ffs64(cpu_addr);
			u64 alignment = 1ULL << nr_zeros;

			size = min(size, alignment);
		}

		/* Hardware supports max 4GiB inbound region */
		/* NVMe: 한 inbound window당 최대 4GiB로 제한 */
		size = min(size, 1ULL << 32);

		mask = roundup_pow_of_two(size) - 1;
		mask &= ~0xf;

		/* NVMe: PCIe 주소 -> CPU 주소 1:1 inbound 매핑 설정;
		 * NVMe의 PRP/SGL DMA 주소가 이 mapping으로 DRAM에 도달한다.
		 */
		rcar_pcie_set_inbound(pcie, cpu_addr, pci_addr,
				      lower_32_bits(mask) | flags, idx, true);

		pci_addr += size;
		cpu_addr += size;
		idx += 2;
	}
	*index = idx;

	return 0;
}

/* PCI/NVMe: DT dma-ranges를 파싱하여 inbound mapping을 구성. IOMMU가 없을
 * 때 NVMe DMA가 시스템 메모리에 접근하려면 이 inbound window가 반드시 필요.
 */
static int rcar_pcie_parse_map_dma_ranges(struct rcar_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	struct resource_entry *entry;
	int index = 0, err = 0;

	/* NVMe: bridge->dma_ranges의 각 entry를 inbound window로 변환 */
	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		err = rcar_pcie_inbound_ranges(&host->pcie, entry, &index);
		if (err)
			break;
	}

	return err;
}

/* PCI/NVMe: DT compatible 목록. SoC별 PHY 초기화 함수를 .data에 연결. */
static const struct of_device_id rcar_pcie_of_match[] = {
	{ .compatible = "renesas,pcie-r8a7779",
	  .data = rcar_pcie_phy_init_h1 },
	{ .compatible = "renesas,pcie-r8a7790",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-r8a7791",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-rcar-gen2",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-r8a7795",
	  .data = rcar_pcie_phy_init_gen3 },
	{ .compatible = "renesas,pcie-rcar-gen3",
	  .data = rcar_pcie_phy_init_gen3 },
	{},
};

/* Design note 346 from Linear Technology says order is not important. */
/* PCI/NVMe: PCIe 슬롯 전원 레일. 안정적인 NVMe 동작을 위해 필요한 전압 레일 */
static const char * const rcar_pcie_supplies[] = {
	"vpcie1v5",
	"vpcie3v3",
	"vpcie12v",
};

/* PCI/NVMe: 플랫폼 드라이버 probe. R-Car PCIe host를 초기화하고 PCI/NVMe
 * 장치를 열거할 준비를 한다.
 */
static int rcar_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct rcar_pcie_host *host;
	struct rcar_pcie *pcie;
	unsigned int i;
	u32 data;
	int err;

	/* NVMe: host bridge 메모리 할당; priv 영역에 rcar_pcie_host 저장 */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*host));
	if (!bridge)
		return -ENOMEM;

	host = pci_host_bridge_priv(bridge);
	pcie = &host->pcie;
	pcie->dev = dev;
	platform_set_drvdata(pdev, host);

	/* NVMe: PCIe 전원 레일 enable; 전원 불량 시 NVMe 링크 불량 원인 */
	for (i = 0; i < ARRAY_SIZE(rcar_pcie_supplies); i++) {
		err = devm_regulator_get_enable_optional(dev, rcar_pcie_supplies[i]);
		if (err < 0 && err != -ENODEV)
			return dev_err_probe(dev, err, "failed to enable regulator: %s\n",
					     rcar_pcie_supplies[i]);
	}

	/* NVMe: runtime PM 활성화; NVMe ASPM/절전과 연계됨 */
	pm_runtime_enable(pcie->dev);
	err = pm_runtime_get_sync(pcie->dev);
	if (err < 0) {
		dev_err(pcie->dev, "pm_runtime_get_sync failed\n");
		goto err_pm_put;
	}

	/* NVMe: PHY/MMIO/clk/IRQ 리소스 획득 */
	err = rcar_pcie_get_resources(host);
	if (err < 0) {
		dev_err(dev, "failed to request resources: %d\n", err);
		goto err_pm_put;
	}

	/* NVMe: PCIe 버스 클록 활성화; 클록 없이는 config/MSI 동작 불가 */
	err = clk_prepare_enable(host->bus_clk);
	if (err) {
		dev_err(dev, "failed to enable bus clock: %d\n", err);
		goto err_unmap_msi_irqs;
	}

	/* NVMe: DMA inbound window 설정; NVMe DMA 주소 변환 준비 */
	err = rcar_pcie_parse_map_dma_ranges(host);
	if (err)
		goto err_clk_disable;

	/* NVMe: SoC별 PHY 초기화 함수 선택 및 실행 */
	host->phy_init_fn = of_device_get_match_data(dev);
	err = host->phy_init_fn(host);
	if (err) {
		dev_err(dev, "failed to init PCIe PHY\n");
		goto err_clk_disable;
	}

	/* Failure to get a link might just be that no cards are inserted */
	/* NVMe: PCIe 링크 트레이닝; NVMe SSD가 없거나 준비 안 되면 -ENODEV */
	if (rcar_pcie_hw_init(pcie)) {
		dev_info(dev, "PCIe link down\n");
		err = -ENODEV;
		goto err_phy_shutdown;
	}

	/* NVMe: 링크 업 상태 및 lane 수 로그 */
	data = rcar_pci_read_reg(pcie, MACSR);
	dev_info(dev, "PCIe x%d: link up\n", (data >> 20) & 0x3f);

	/* NVMe: MSI 지원 시 MSI 컨트롤러 초기화; NVMe가 MSI/MSI-x를 사용하게 됨 */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		err = rcar_pcie_enable_msi(host);
		if (err < 0) {
			dev_err(dev,
				"failed to enable MSI support: %d\n",
				err);
			goto err_phy_shutdown;
		}
	}

	/* NVMe: PCI host bridge 활성화 및 열거; NVMe endpoint 발견/바인딩 */
	err = rcar_pcie_enable(host);
	if (err)
		goto err_msi_teardown;

	return 0;

err_msi_teardown:
	if (IS_ENABLED(CONFIG_PCI_MSI))
		rcar_pcie_teardown_msi(host);

err_phy_shutdown:
	if (host->phy) {
		phy_power_off(host->phy);
		phy_exit(host->phy);
	}

err_clk_disable:
	clk_disable_unprepare(host->bus_clk);

err_unmap_msi_irqs:
	irq_dispose_mapping(host->msi.irq2);
	irq_dispose_mapping(host->msi.irq1);

err_pm_put:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return err;
}

/* PCI/NVMe: 시스템 resume 시 DMA inbound window, PHY, 링크, MSI를 복구.
 * NVMe SSD가 suspend/resume 후에도 정상 동작하도록 상태를 되돌린다.
 */
static int rcar_pcie_resume(struct device *dev)
{
	struct rcar_pcie_host *host = dev_get_drvdata(dev);
	struct rcar_pcie *pcie = &host->pcie;
	unsigned int data;
	int err;

	/* NVMe: DMA inbound mapping 복구; 없으면 resume 후 NVMe DMA 실패 */
	err = rcar_pcie_parse_map_dma_ranges(host);
	if (err)
		return 0;

	/* Failure to get a link might just be that no cards are inserted */
	/* NVMe: PHY 재초기화; NVMe SSD가 슬롯에 없으면 0 반환 */
	err = host->phy_init_fn(host);
	if (err) {
		dev_info(dev, "PCIe link down\n");
		return 0;
	}

	/* NVMe: 링크 상태 로그 */
	data = rcar_pci_read_reg(pcie, MACSR);
	dev_info(dev, "PCIe x%d: link up\n", (data >> 20) & 0x3f);

	/* Enable MSI */
	/* NVMe: resume 시 MSI target address와 사용 중이던 벡터 마스크 복구;
	 * 그렇지 않으면 NVMe queue interrupt가 먹통이 됨.
	 */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		struct resource res;
		u32 val;

		of_address_to_resource(dev->of_node, 0, &res);
		rcar_pci_write_reg(pcie, upper_32_bits(res.start), PCIEMSIAUR);
		rcar_pci_write_reg(pcie, lower_32_bits(res.start) | MSIFE, PCIEMSIALR);

		/* NVMe: 사용 중인 MSI 벡터 마스크 복구 */
		bitmap_to_arr32(&val, host->msi.used, INT_PCI_MSI_NR);
		rcar_pci_write_reg(pcie, val, PCIEMSIIER);
	}

	/* NVMe: outbound window 및 링크 속도 복구 */
	rcar_pcie_hw_enable(host);

	return 0;
}

/* PCI/NVMe: noirq resume 단계에서 PCIe 링크가 끊어진 경우 재수립.
 * NVMe SSD가 resume 직후에도 바로 접근 가능하도록 한다.
 */
static int rcar_pcie_resume_noirq(struct device *dev)
{
	struct rcar_pcie_host *host = dev_get_drvdata(dev);
	struct rcar_pcie *pcie = &host->pcie;

	/* NVMe: PMSR가 0이 아니고 DL_DOWN이 아니면 이미 링크 살아있음 */
	if (rcar_pci_read_reg(pcie, PMSR) &&
	    !(rcar_pci_read_reg(pcie, PCIETCTLR) & DL_DOWN))
		return 0;

	/* Re-establish the PCIe link */
	/* NVMe: 링크가 다운되었으면 MACCTLR/CFINIT으로 재수립 */
	rcar_pci_write_reg(pcie, MACCTLR_INIT_VAL, MACCTLR);
	rcar_pci_write_reg(pcie, CFINIT, PCIETCTLR);
	return rcar_pcie_wait_for_dl(pcie);
}

/* PCI/NVMe: R-Car PCIe용 dev_pm_ops. resume 시 noirq부터 link/MSI 복구. */
static const struct dev_pm_ops rcar_pcie_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(NULL, rcar_pcie_resume)
	.resume_noirq = rcar_pcie_resume_noirq,
};

/* PCI/NVMe: R-Car PCIe 플랫폼 드라이버 구조체. probe 시 NVMe 장치를
 * 발견할 수 있도록 host bridge를 등록한다.
 */
static struct platform_driver rcar_pcie_driver = {
	.driver = {
		.name = "rcar-pcie",
		.of_match_table = rcar_pcie_of_match,
		.pm = &rcar_pcie_pm_ops,
		.suppress_bind_attrs = true,
	},
	.probe = rcar_pcie_probe,
};

#ifdef CONFIG_ARM
/* PCI/NVMe: ARM 32비트에서 PCIe 메모리 접근 버스 에러 핸들러. fixup이
 * 성공하면 1을 반환하여 handler가 SIGBUS를 보내지 않게 한다. NVMe 열거 중
 * 잘못된 config/메모리 접근 시 시스템 충돌을 방지한다.
 */
static int rcar_pcie_aarch32_abort_handler(unsigned long addr,
		unsigned int fsr, struct pt_regs *regs)
{
	return !fixup_exception(regs);
}

/* PCI/NVMe: abort handler를 등록할 SoC compatible 목록 */
static const struct of_device_id rcar_pcie_abort_handler_of_match[] __initconst = {
	{ .compatible = "renesas,pcie-r8a7779" },
	{ .compatible = "renesas,pcie-r8a7790" },
	{ .compatible = "renesas,pcie-r8a7791" },
	{ .compatible = "renesas,pcie-rcar-gen2" },
	{},
};

/* PCI/NVMe: ARM 32비트에서 PCIe abort handler 등록 후 플랫폼 드라이버 등록.
 * 이후 NVMe PCIe SSD가 시스템에서 검색될 수 있다.
 */
static int __init rcar_pcie_init(void)
{
	if (of_find_matching_node(NULL, rcar_pcie_abort_handler_of_match)) {
#ifdef CONFIG_ARM_LPAE
		hook_fault_code(17, rcar_pcie_aarch32_abort_handler, SIGBUS, 0,
				"asynchronous external abort");
#else
		hook_fault_code(22, rcar_pcie_aarch32_abort_handler, SIGBUS, 0,
				"imprecise external abort");
#endif
	}

	return platform_driver_register(&rcar_pcie_driver);
}
device_initcall(rcar_pcie_init);
#else
/* PCI/NVMe: ARM64 등에서 바로 플랫폼 드라이버 등록 */
builtin_platform_driver(rcar_pcie_driver);
#endif
