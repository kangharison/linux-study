// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host bridge driver for Apple system-on-chips.
 *
 * The HW is ECAM compliant, so once the controller is initialized,
 * the driver mostly deals MSI mapping and handling of per-port
 * interrupts (INTx, management and error signals).
 *
 * Initialization requires enabling power and clocks, along with a
 * number of register pokes.
 *
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2021 Google LLC
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 *
 * Author: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Author: Marc Zyngier <maz@kernel.org>
 */

#include <linux/bitfield.h>		/* PCI/NVMe: FIELD_PREP/GENMASK for register bit packing */
#include <linux/gpio/consumer.h>	/* NVMe: PERST# GPIO used to reset NVMe SSD during probe */
#include <linux/kernel.h>
#include <linux/iopoll.h>		/* NVMe: readl_relaxed_poll_timeout waits for link/clock ready */
#include <linux/irqchip/chained_irq.h>	/* NVMe: root-port IRQs chain to MSI/INTx for NVMe queues */
#include <linux/irqchip/irq-msi-lib.h>	/* NVMe: MSI parent library; NVMe MSI-X uses this domain */
#include <linux/irqdomain.h>		/* NVMe: maps Linux virq to hwirq used by NVMe MSI vectors */
#include <linux/list.h>
#include <linux/module.h>
#include <linux/msi.h>			/* NVMe: MSI/MSI-X flag definitions for NVMe irq vectors */
#include <linux/of_irq.h>
#include <linux/pci-ecam.h>		/* NVMe: ECAM config access for NVMe BDF enumeration */

#include "pci-host-common.h"		/* NVMe: pci_host_common_init registers host bridge */

/* T8103 (original M1) and related SoCs */
#define CORE_RC_PHYIF_CTL		0x00024	/* PCI/NVMe: root-complex PHY interface control */
#define   CORE_RC_PHYIF_CTL_RUN		BIT(0)	/* NVMe: start PHY interface so NVMe link can train */
#define CORE_RC_PHYIF_STAT		0x00028	/* PCI/NVMe: root-complex PHY status register */
#define   CORE_RC_PHYIF_STAT_REFCLK	BIT(4)	/* NVMe: refclk stable; needed before NVMe PERST# deassert */
#define CORE_RC_CTL			0x00050	/* PCI/NVMe: root-complex main control */
#define   CORE_RC_CTL_RUN		BIT(0)	/* NVMe: release RC reset; first step to see NVMe device */
#define CORE_RC_STAT			0x00058	/* PCI/NVMe: root-complex ready status */
#define   CORE_RC_STAT_READY		BIT(0)	/* NVMe: RC ready before enumerating NVMe endpoints */
#define CORE_FABRIC_STAT		0x04000	/* PCI/NVMe: PCIe fabric status */
#define   CORE_FABRIC_STAT_MASK		0x001F001F

#define CORE_PHY_DEFAULT_BASE(port)	(0x84000 + 0x4000 * (port))	/* NVMe: per-port PHY register base */

#define PHY_LANE_CFG			0x00000	/* PCI/NVMe: PHY lane configuration */
#define   PHY_LANE_CFG_REFCLK0REQ	BIT(0)	/* NVMe: request refclk source 0 for NVMe port */
#define   PHY_LANE_CFG_REFCLK1REQ	BIT(1)	/* NVMe: request refclk source 1 for NVMe port */
#define   PHY_LANE_CFG_REFCLK0ACK	BIT(2)	/* NVMe: refclk0 acknowledged by PHY */
#define   PHY_LANE_CFG_REFCLK1ACK	BIT(3)	/* NVMe: refclk1 acknowledged by PHY */
#define   PHY_LANE_CFG_REFCLKEN		(BIT(9) | BIT(10))	/* NVMe: enable refclk output to NVMe slot */
#define   PHY_LANE_CFG_REFCLKCGEN	(BIT(30) | BIT(31))	/* NVMe: refclk clock-gating enable for ASPM */
#define PHY_LANE_CTL			0x00004	/* PCI/NVMe: PHY lane control */
#define   PHY_LANE_CTL_CFGACC		BIT(15)	/* NVMe: allow PHY config access during NVMe init */

#define PORT_LTSSMCTL			0x00080	/* PCI/NVMe: LTSSM control; starts NVMe link training */
#define   PORT_LTSSMCTL_START		BIT(0)	/* NVMe: begin Link Training and Status State Machine */
#define PORT_INTSTAT			0x00100	/* PCI/NVMe: aggregated port interrupt status */
#define   PORT_INT_TUNNEL_ERR		31	/* NVMe: tunnel error; may affect NVMe command completion */
#define   PORT_INT_CPL_TIMEOUT		23	/* NVMe: completion timeout; NVMe cmds may stall */
#define   PORT_INT_RID2SID_MAPERR	22	/* NVMe: RID->SID map error breaks NVMe IOMMU DMA */
#define   PORT_INT_CPL_ABORT		21	/* NVMe: completer abort; NVMe controller signalled error */
#define   PORT_INT_MSI_BAD_DATA		19	/* NVMe: MSI payload corrupted; NVMe irq may misfire */
#define   PORT_INT_MSI_ERR		18	/* NVMe: MSI error; NVMe queue interrupts can be lost */
#define   PORT_INT_REQADDR_GT32		17	/* NVMe: 32-bit DMA limit violation for NVMe payloads */
#define   PORT_INT_AF_TIMEOUT		15	/* NVMe: AXI fabric timeout; affects NVMe register/DMA */
#define   PORT_INT_LINK_DOWN		14	/* NVMe: link down; NVMe controller disconnected */
#define   PORT_INT_LINK_UP		12	/* NVMe: link up; NVMe SSD ready for enumeration */
#define   PORT_INT_LINK_BWMGMT		11	/* NVMe: bandwidth management event (ASPM related) */
#define   PORT_INT_AER_MASK		(15 << 4)	/* NVMe: AER interrupt mask; used for NVMe AER reporting */
#define   PORT_INT_PORT_ERR		4	/* NVMe: port error; potential NVMe fatal error */
#define   PORT_INT_INTx(i)		i	/* NVMe: legacy INTx line i (NVMe rarely uses) */
#define   PORT_INT_INTx_MASK		15	/* NVMe: mask for INTx[0..3] from NVMe device */
#define PORT_INTMSK			0x00104	/* NVMe: per-bit IRQ mask; controls NVMe port events */
#define PORT_INTMSKSET			0x00108	/* NVMe: set IRQ mask bits */
#define PORT_INTMSKCLR			0x0010c	/* NVMe: clear IRQ mask bits */
#define PORT_MSICFG			0x00124	/* PCI/NVMe: MSI engine configuration */
#define   PORT_MSICFG_EN		BIT(0)	/* NVMe: enable MSI generation for NVMe vectors */
#define   PORT_MSICFG_L2MSINUM_SHIFT	4	/* NVMe: log2(number of MSI vectors) shift */
#define PORT_MSIBASE			0x00128	/* PCI/NVMe: MSI base index register (T8103) */
#define   PORT_MSIBASE_1_SHIFT		16	/* NVMe: upper 16-bit base for multi-vector NVMe MSI */
#define PORT_MSIADDR			0x00168	/* PCI/NVMe: lower 32-bit MSI doorbell address */
#define PORT_LINKSTS			0x00208	/* PCI/NVMe: link status */
#define   PORT_LINKSTS_UP		BIT(0)	/* NVMe: Data Link Layer up; NVMe config access safe */
#define   PORT_LINKSTS_BUSY		BIT(2)	/* NVMe: link busy during retrain/hotplug */
#define PORT_LINKCMDSTS			0x00210	/* PCI/NVMe: link command/status; cleared during init */
#define PORT_OUTS_NPREQS		0x00284	/* PCI/NVMe: outstanding non-posted requests status */
#define   PORT_OUTS_NPREQS_REQ		BIT(24)	/* NVMe: NP request pending; NVMe config/cmd may wait */
#define   PORT_OUTS_NPREQS_CPL		BIT(16)	/* NVMe: NP completion pending */
#define PORT_RXWR_FIFO			0x00288	/* PCI/NVMe: receiver write FIFO status */
#define   PORT_RXWR_FIFO_HDR		GENMASK(15, 10)
#define   PORT_RXWR_FIFO_DATA		GENMASK(9, 0)
#define PORT_RXRD_FIFO			0x0028C	/* PCI/NVMe: receiver read FIFO status */
#define   PORT_RXRD_FIFO_REQ		GENMASK(6, 0)
#define PORT_OUTS_CPLS			0x00290	/* PCI/NVMe: outstanding completions */
#define   PORT_OUTS_CPLS_SHRD		GENMASK(14, 8)
#define   PORT_OUTS_CPLS_WAIT		GENMASK(6, 0)
#define PORT_APPCLK			0x00800	/* PCI/NVMe: application clock control */
#define   PORT_APPCLK_EN		BIT(0)	/* NVMe: enable port logic clock before NVMe access */
#define   PORT_APPCLK_CGDIS		BIT(8)	/* NVMe: disable clock gating; cleared after init */
#define PORT_STATUS			0x00804	/* PCI/NVMe: port status */
#define   PORT_STATUS_READY		BIT(0)	/* NVMe: port ready; safe to enumerate NVMe */
#define PORT_REFCLK			0x00810	/* PCI/NVMe: port reference clock control */
#define   PORT_REFCLK_EN		BIT(0)	/* NVMe: enable refclk to NVMe PHY */
#define   PORT_REFCLK_CGDIS		BIT(8)	/* NVMe: refclk clock-gating disable */
#define PORT_PERST			0x00814	/* PCI/NVMe: PERST# control */
#define   PORT_PERST_OFF		BIT(0)	/* NVMe: deassert PERST# to release NVMe reset */
#define PORT_RID2SID			0x00828	/* PCI/NVMe: RequesterID -> StreamID mapping table */
#define   PORT_RID2SID_VALID		BIT(31)	/* NVMe: entry valid; enables IOMMU translation for NVMe */
#define   PORT_RID2SID_SID_SHIFT	16	/* NVMe: IOMMU StreamID for this NVMe RID */
#define   PORT_RID2SID_BUS_SHIFT	8	/* NVMe: PCIe bus number of NVMe device */
#define   PORT_RID2SID_DEV_SHIFT	3	/* NVMe: PCIe device number of NVMe device */
#define   PORT_RID2SID_FUNC_SHIFT	0	/* NVMe: PCIe function number of NVMe device */
#define PORT_OUTS_PREQS_HDR		0x00980
#define   PORT_OUTS_PREQS_HDR_MASK	GENMASK(9, 0)
#define PORT_OUTS_PREQS_DATA		0x00984
#define   PORT_OUTS_PREQS_DATA_MASK	GENMASK(15, 0)
#define PORT_TUNCTRL			0x00988	/* PCI/NVMe: PERST tunnel control */
#define   PORT_TUNCTRL_PERST_ON		BIT(0)	/* NVMe: assert PERST# via tunnel (e.g. downstream switch) */
#define   PORT_TUNCTRL_PERST_ACK_REQ	BIT(1)	/* NVMe: request PERST acknowledge */
#define PORT_TUNSTAT			0x0098c	/* PCI/NVMe: PERST tunnel status */
#define   PORT_TUNSTAT_PERST_ON		BIT(0)	/* NVMe: PERST tunnel is active */
#define   PORT_TUNSTAT_PERST_ACK_PEND	BIT(1)	/* NVMe: PERST acknowledge pending */
#define PORT_PREFMEM_ENABLE		0x00994	/* NVMe: enable prefetchable memory for NVMe BARs */

/* T602x (M2-pro and co) */
#define PORT_T602X_MSIADDR	0x016c	/* NVMe: T602x lower MSI doorbell address */
#define PORT_T602X_MSIADDR_HI	0x0170	/* NVMe: T602x upper MSI doorbell address */
#define PORT_T602X_PERST	0x082c	/* NVMe: T602x PERST# register offset */
#define PORT_T602X_RID2SID	0x3000	/* NVMe: T602x RID->SID table base */
#define PORT_T602X_MSIMAP	0x3800	/* NVMe: T602x per-vector MSI target map */

#define PORT_MSIMAP_ENABLE	BIT(31)		/* NVMe: enable this MSI target map entry */
#define PORT_MSIMAP_TARGET	GENMASK(7, 0)	/* NVMe: target MSI vector index for NVMe MSI-X */

/*
 * The doorbell address is set to 0xfffff000, which by convention
 * matches what MacOS does, and it is possible to use any other
 * address (in the bottom 4GB, as the base register is only 32bit).
 * However, it has to be excluded from the IOVA range, and the DART
 * driver has to know about it.
 */
#define DOORBELL_ADDR		CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR	/* NVMe: MSI doorbell; DART reserves this from NVMe IOVA */

struct hw_info {
	u32 phy_lane_ctl;	/* NVMe: PHY lane control register offset per generation */
	u32 port_msiaddr;	/* NVMe: lower 32-bit MSI doorbell address register offset */
	u32 port_msiaddr_hi;	/* NVMe: upper 32-bit MSI doorbell address register offset */
	u32 port_refclk;	/* NVMe: refclk control register offset */
	u32 port_perst;		/* NVMe: PERST# register offset */
	u32 port_rid2sid;	/* NVMe: RID->SID mapping table base offset */
	u32 port_msimap;	/* NVMe: per-vector MSI map base offset (T602x) */
	u32 max_rid2sid;	/* NVMe: max RID->SID entries limits NVMe devices per port */
};

static const struct hw_info t8103_hw = {
	.phy_lane_ctl		= PHY_LANE_CTL,		/* NVMe: T8103 has explicit PHY lane control */
	.port_msiaddr		= PORT_MSIADDR,		/* NVMe: doorbell at 0x168 */
	.port_msiaddr_hi	= 0,			/* NVMe: 32-bit doorbell only */
	.port_refclk		= PORT_REFCLK,		/* NVMe: per-port refclk control */
	.port_perst		= PORT_PERST,		/* NVMe: per-port PERST# */
	.port_rid2sid		= PORT_RID2SID,		/* NVMe: 64-entry RID->SID table */
	.port_msimap		= 0,			/* NVMe: shared MSI config, no per-vector map */
	.max_rid2sid		= 64,			/* NVMe: up to 64 RID->SID contexts */
};

static const struct hw_info t602x_hw = {
	.phy_lane_ctl		= 0,			/* NVMe: PHY lane control elsewhere on T602x */
	.port_msiaddr		= PORT_T602X_MSIADDR,	/* NVMe: doorbell at 0x16c */
	.port_msiaddr_hi	= PORT_T602X_MSIADDR_HI,	/* NVMe: supports 64-bit doorbell address */
	.port_refclk		= 0,			/* NVMe: refclk handled differently */
	.port_perst		= PORT_T602X_PERST,	/* NVMe: PERST# at 0x82c */
	.port_rid2sid		= PORT_T602X_RID2SID,	/* NVMe: larger RID->SID table at 0x3000 */
	.port_msimap		= PORT_T602X_MSIMAP,	/* NVMe: per-vector MSI target map */
	/* 16 on t602x, guess for autodetect on future HW */
	.max_rid2sid		= 512,			/* NVMe: probe actual entry count */
};

struct apple_pcie {
	struct mutex		lock;		/* NVMe: protects MSI bitmap and RID->SID maps */
	struct device		*dev;		/* NVMe: platform device; parent of NVMe PCI bus */
	void __iomem            *base;		/* NVMe: root-complex MMIO; config space lives here via ECAM */
	const struct hw_info	*hw;		/* NVMe: generation-specific register layout */
	unsigned long		*bitmap;	/* NVMe: MSI vector allocation bitmap for all NVMe devices */
	struct list_head	ports;		/* NVMe: list of root ports each may host an NVMe SSD */
	struct completion	event;		/* NVMe: wait for link-up event during NVMe probe */
	struct irq_fwspec	fwspec;		/* NVMe: parent IRQ fwspec for MSI domain allocation */
	u32			nvecs;		/* NVMe: total MSI vectors available across ports */
};

struct apple_pcie_port {
	raw_spinlock_t		lock;		/* NVMe: protects port IRQ mask/status registers */
	struct apple_pcie	*pcie;		/* NVMe: back pointer to host bridge */
	struct device_node	*np;		/* NVMe: OF node for this NVMe root port */
	void __iomem		*base;		/* NVMe: port MMIO base */
	void __iomem		*phy;		/* NVMe: PHY MMIO base for refclk/reset */
	struct irq_domain	*domain;	/* NVMe: port IRQ domain for INTx/link/AER events */
	struct list_head	entry;		/* NVMe: membership in apple_pcie.ports list */
	unsigned long		*sid_map;	/* NVMe: bitmap of allocated RID->SID entries */
	int			sid_map_sz;	/* NVMe: actual number of usable RID->SID entries */
	int			idx;		/* NVMe: port index derived from reg property */
};

static void rmw_set(u32 set, void __iomem *addr)	/* NVMe: read-modify-write set helper for PCIe regs */
{
	writel_relaxed(readl_relaxed(addr) | set, addr);	/* NVMe: set bits without affecting others */
}

static void rmw_clear(u32 clr, void __iomem *addr)	/* NVMe: read-modify-write clear helper */
{
	writel_relaxed(readl_relaxed(addr) & ~clr, addr);	/* NVMe: clear bits without affecting others */
}

static void apple_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)	/* NVMe: builds MSI message for NVMe vector */
{
	msg->address_hi = upper_32_bits(DOORBELL_ADDR);	/* NVMe: upper 32 bits of MSI doorbell (zero here) */
	msg->address_lo = lower_32_bits(DOORBELL_ADDR);	/* NVMe: lower 32 bits point to shared doorbell page */
	msg->data = data->hwirq;			/* NVMe: vector index becomes MSI payload; NVMe driver wires this to queue irq */
}

static struct irq_chip apple_msi_bottom_chip = {
	.name			= "MSI",			/* NVMe: bottom IRQ chip for NVMe MSI/MSI-X */
	.irq_mask		= irq_chip_mask_parent,		/* NVMe: mask at parent; affects NVMe interrupt delivery */
	.irq_unmask		= irq_chip_unmask_parent,	/* NVMe: unmask at parent; enables NVMe queue interrupt */
	.irq_eoi		= irq_chip_eoi_parent,		/* NVMe: end-of-intercept for NVMe MSI */
	.irq_set_affinity	= irq_chip_set_affinity_parent,	/* NVMe: move NVMe MSI vector to another CPU */
	.irq_set_type		= irq_chip_set_type_parent,	/* NVMe: edge/level for NVMe MSI (edge) */
	.irq_compose_msi_msg	= apple_msi_compose_msg,	/* NVMe: fills MSI msg for NVMe pci_write_msi_msg() */
};

static int apple_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)	/* NVMe: allocate MSI vectors for NVMe MSI/MSI-X */
{
	struct apple_pcie *pcie = domain->host_data;	/* NVMe: host bridge owning MSI vector pool */
	struct irq_fwspec fwspec = pcie->fwspec;	/* NVMe: copy parent fwspec then adjust base vector */
	unsigned int i;
	int ret, hwirq;

	mutex_lock(&pcie->lock);	/* NVMe: serialize MSI bitmap access across NVMe devices */

	hwirq = bitmap_find_free_region(pcie->bitmap, pcie->nvecs,
					order_base_2(nr_irqs));	/* NVMe: find contiguous free vectors for NVMe queues */

	mutex_unlock(&pcie->lock);

	if (hwirq < 0)
		return -ENOSPC;	/* NVMe: no free MSI vectors left for NVMe */

	fwspec.param[fwspec.param_count - 2] += hwirq;	/* NVMe: redirect parent to first allocated hw vector */

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);	/* NVMe: allocate parent wired irqs for NVMe vectors */
	if (ret)
		return ret;	/* NVMe: parent allocation failed; leak handled by caller */

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &apple_msi_bottom_chip, pcie);		/* NVMe: bind Linux virq to hwirq and MSI chip for NVMe queue irq */
	}

	return 0;	/* NVMe: MSI vectors allocated; pci_enable_msi_range can program them */
}

static void apple_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)	/* NVMe: free MSI vectors when NVMe driver releases MSI/MSI-X */
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);	/* NVMe: first irq data holds base hwirq */
	struct apple_pcie *pcie = domain->host_data;

	mutex_lock(&pcie->lock);	/* NVMe: serialize MSI bitmap release */

	bitmap_release_region(pcie->bitmap, d->hwirq, order_base_2(nr_irqs));	/* NVMe: return vectors to pool for other NVMe devices */

	mutex_unlock(&pcie->lock);
}

static const struct irq_domain_ops apple_msi_domain_ops = {
	.alloc	= apple_msi_domain_alloc,	/* NVMe: called by PCI/MSI core for NVMe MSI/MSI-X */
	.free	= apple_msi_domain_free,	/* NVMe: called on pci_disable_msi/msix */
};

static void apple_port_irq_mask(struct irq_data *data)	/* NVMe: mask a port IRQ (link/AER/INTx) */
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);	/* NVMe: port whose interrupt is masked */

	guard(raw_spinlock_irqsave)(&port->lock);	/* NVMe: protect PORT_INTMSK */
	rmw_set(BIT(data->hwirq), port->base + PORT_INTMSK);	/* NVMe: set mask bit; stops this NVMe-related event */
}

static void apple_port_irq_unmask(struct irq_data *data)	/* NVMe: unmask a port IRQ */
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	guard(raw_spinlock_irqsave)(&port->lock);
	rmw_clear(BIT(data->hwirq), port->base + PORT_INTMSK);	/* NVMe: clear mask bit; re-enable event for NVMe */
}

static bool hwirq_is_intx(unsigned int hwirq)	/* NVMe: true for legacy INTx hwirq 0..3 */
{
	return BIT(hwirq) & PORT_INT_INTx_MASK;	/* NVMe: NVMe SSDs normally use MSI-X, INTx fallback rare */
}

static void apple_port_irq_ack(struct irq_data *data)	/* NVMe: ack edge-triggered port IRQs */
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	if (!hwirq_is_intx(data->hwirq))	/* NVMe: INTx are level, do not write status; others are edge */
		writel_relaxed(BIT(data->hwirq), port->base + PORT_INTSTAT);	/* NVMe: clear edge status for link/AER/MSI error events */
}

static int apple_port_irq_set_type(struct irq_data *data, unsigned int type)	/* NVMe: validate trigger type for NVMe port events */
{
	/*
	 * It doesn't seem that there is any way to configure the
	 * trigger, so assume INTx have to be level (as per the spec),
	 * and the rest is edge (which looks likely).
	 */
	if (hwirq_is_intx(data->hwirq) ^ !!(type & IRQ_TYPE_LEVEL_MASK))
		return -EINVAL;	/* NVMe: reject wrong trigger for INTx vs MSI/AER/link */

	irqd_set_trigger_type(data, type);	/* NVMe: record trigger type for NVMe irq handling */
	return 0;
}

static struct irq_chip apple_port_irqchip = {
	.name		= "PCIe",			/* NVMe: chip for root-port events affecting NVMe */
	.irq_ack	= apple_port_irq_ack,		/* NVMe: ack edge events like link down/AER */
	.irq_mask	= apple_port_irq_mask,		/* NVMe: mask port event */
	.irq_unmask	= apple_port_irq_unmask,	/* NVMe: unmask port event */
	.irq_set_type	= apple_port_irq_set_type,	/* NVMe: enforce level for INTx, edge for others */
};

static int apple_port_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)	/* NVMe: allocate Linux virq for port event */
{
	struct apple_pcie_port *port = domain->host_data;	/* NVMe: root port owning this domain */
	struct irq_fwspec *fwspec = args;			/* NVMe: OF irq specifier from device tree */
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_flow_handler_t flow = handle_edge_irq;	/* NVMe: AER/link events are edge */
		unsigned int type = IRQ_TYPE_EDGE_RISING;

		if (hwirq_is_intx(fwspec->param[0] + i)) {	/* NVMe: legacy INTx from NVMe device */
			flow = handle_level_irq;		/* NVMe: INTx must be level per PCIe spec */
			type = IRQ_TYPE_LEVEL_HIGH;
		}

		irq_domain_set_info(domain, virq + i, fwspec->param[0] + i,
				    &apple_port_irqchip, port, flow,
				    NULL, NULL);			/* NVMe: wire virq to port IRQ chip and handler */

		irq_set_irq_type(virq + i, type);		/* NVMe: set Linux trigger type */
	}

	return 0;
}

static void apple_port_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)	/* NVMe: tear down port virq mappings */
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		irq_set_handler(virq + i, NULL);	/* NVMe: remove handler */
		irq_domain_reset_irq_data(d);		/* NVMe: clear mapping */
	}
}

static const struct irq_domain_ops apple_port_irq_domain_ops = {
	.translate	= irq_domain_translate_onecell,	/* NVMe: single-cell irq translation for port events */
	.alloc		= apple_port_irq_domain_alloc,	/* NVMe: allocate port IRQs (link/AER/INTx) */
	.free		= apple_port_irq_domain_free,	/* NVMe: free port IRQs */
};

static void apple_port_irq_handler(struct irq_desc *desc)	/* NVMe: chained handler for root port aggregate IRQ */
{
	struct apple_pcie_port *port = irq_desc_get_handler_data(desc);	/* NVMe: port receiving event */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long stat;
	int i;

	chained_irq_enter(chip, desc);	/* NVMe: acknowledge parent IRQ */

	stat = readl_relaxed(port->base + PORT_INTSTAT);	/* NVMe: read pending port events (link/AER/INTx/MSI errors) */

	for_each_set_bit(i, &stat, 32)
		generic_handle_domain_irq(port->domain, i);	/* NVMe: dispatch each event to its virq; link-down affects NVMe */

	chained_irq_exit(chip, desc);	/* NVMe: re-enable parent IRQ */
}

static int apple_pcie_port_setup_irq(struct apple_pcie_port *port)	/* NVMe: initialize port IRQs and MSI engine */
{
	struct fwnode_handle *fwnode = &port->np->fwnode;	/* NVMe: fwnode for port IRQ domain */
	struct apple_pcie *pcie = port->pcie;
	unsigned int irq;
	u32 val = 0;

	/* FIXME: consider moving each interrupt under each port */
	irq = irq_of_parse_and_map(to_of_node(dev_fwnode(port->pcie->dev)),
				   port->idx);		/* NVMe: get parent wired IRQ for this port's aggregate events */
	if (!irq)
		return -ENXIO;	/* NVMe: no parent IRQ, cannot handle NVMe link/AER events */

	port->domain = irq_domain_create_linear(fwnode, 32,
						&apple_port_irq_domain_ops,
						port);	/* NVMe: create 32-line domain for port events and INTx */
	if (!port->domain)
		return -ENOMEM;

	/* Disable all interrupts */
	writel_relaxed(~0, port->base + PORT_INTMSK);	/* NVMe: mask all port events before enabling any */
	writel_relaxed(~0, port->base + PORT_INTSTAT);	/* NVMe: clear all pending status bits */
	writel_relaxed(~0, port->base + PORT_LINKCMDSTS);	/* NVMe: clear link command/status */

	irq_set_chained_handler_and_data(irq, apple_port_irq_handler, port);	/* NVMe: parent IRQ chains into port handler */

	/* Configure MSI base address */
	BUILD_BUG_ON(upper_32_bits(DOORBELL_ADDR));	/* NVMe: doorbell must fit in 32 bits for T8103 MSI addr */
	writel_relaxed(lower_32_bits(DOORBELL_ADDR),
		       port->base + pcie->hw->port_msiaddr);	/* NVMe: program MSI doorbell address; NVMe writes here to signal interrupts */
	if (pcie->hw->port_msiaddr_hi)
		writel_relaxed(0, port->base + pcie->hw->port_msiaddr_hi);	/* NVMe: clear upper doorbell address on T602x */

	/* Enable MSIs, shared between all ports */
	if (pcie->hw->port_msimap) {
		for (int i = 0; i < pcie->nvecs; i++)	/* NVMe: T602x needs per-vector target map */
			writel_relaxed(FIELD_PREP(PORT_MSIMAP_TARGET, i) |
				       PORT_MSIMAP_ENABLE,
				       port->base + pcie->hw->port_msimap + 4 * i);	/* NVMe: map hwirq i to target vector i for NVMe MSI-X */
	} else {
		writel_relaxed(0, port->base + PORT_MSIBASE);	/* NVMe: T8103 shared MSI base starts at 0 */
		val = ilog2(pcie->nvecs) << PORT_MSICFG_L2MSINUM_SHIFT;	/* NVMe: log2(total vectors) for shared MSI engine */
	}

	writel_relaxed(val | PORT_MSICFG_EN, port->base + PORT_MSICFG);	/* NVMe: enable MSI engine so NVMe can raise MSI/MSI-X */
	return 0;
}

static irqreturn_t apple_pcie_port_irq(int irq, void *data)	/* NVMe: handler for link up/down events */
{
	struct apple_pcie_port *port = data;
	unsigned int hwirq = irq_domain_get_irq_data(port->domain, irq)->hwirq;
								/* NVMe: which port event fired */

	switch (hwirq) {
	case PORT_INT_LINK_UP:
		dev_info_ratelimited(port->pcie->dev, "Link up on %pOF\n",
				     port->np);
		complete_all(&port->pcie->event);	/* NVMe: wake probe waiting for NVMe link */
		break;
	case PORT_INT_LINK_DOWN:
		dev_info_ratelimited(port->pcie->dev, "Link down on %pOF\n",
				     port->np);		/* NVMe: NVMe SSD may be removed or failed */
		break;
	default:
		return IRQ_NONE;	/* NVMe: not our event */
	}

	return IRQ_HANDLED;
}

static int apple_pcie_port_register_irqs(struct apple_pcie_port *port)	/* NVMe: request link up/down irqs */
{
	static struct {
		unsigned int	hwirq;
		const char	*name;
	} port_irqs[] = {
		{ PORT_INT_LINK_UP,	"Link up",	},	/* NVMe: detect when NVMe SSD becomes available */
		{ PORT_INT_LINK_DOWN,	"Link down",	},	/* NVMe: detect NVMe SSD removal/failure */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(port_irqs); i++) {
		struct irq_fwspec fwspec = {
			.fwnode		= &port->np->fwnode,
			.param_count	= 1,
			.param		= {
				[0]	= port_irqs[i].hwirq,
			},
		};
		unsigned int irq;
		int ret;

		irq = irq_domain_alloc_irqs(port->domain, 1, NUMA_NO_NODE,
					    &fwspec);		/* NVMe: allocate virq for link event */
		if (WARN_ON(!irq))
			continue;	/* NVMe: cannot monitor this event */

		ret = request_irq(irq, apple_pcie_port_irq, 0,
				  port_irqs[i].name, port);	/* NVMe: register handler for NVMe hotplug events */
		WARN_ON(ret);
	}

	return 0;
}

static int apple_pcie_setup_refclk(struct apple_pcie *pcie,
				   struct apple_pcie_port *port)	/* NVMe: bring up reference clock before NVMe reset release */
{
	u32 stat;
	int res;

	if (pcie->hw->phy_lane_ctl)
		rmw_set(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);	/* NVMe: allow PHY config access on T8103 */

	rmw_set(PHY_LANE_CFG_REFCLK0REQ, port->phy + PHY_LANE_CFG);	/* NVMe: request refclk source 0 for NVMe PHY */

	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK0ACK,
					 100, 50000);	/* NVMe: wait up to 50ms for refclk0 ack */
	if (res < 0)
		return res;	/* NVMe: refclk failed; NVMe cannot train */

	rmw_set(PHY_LANE_CFG_REFCLK1REQ, port->phy + PHY_LANE_CFG);
	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK1ACK,
					 100, 50000);	/* NVMe: wait for refclk1 ack */

	if (res < 0)
		return res;	/* NVMe: second refclk failed */

	if (pcie->hw->phy_lane_ctl)
		rmw_clear(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);	/* NVMe: close PHY config access */

	rmw_set(PHY_LANE_CFG_REFCLKEN, port->phy + PHY_LANE_CFG);	/* NVMe: enable refclk output to NVMe device */

	if (pcie->hw->port_refclk)
		rmw_set(PORT_REFCLK_EN, port->base + pcie->hw->port_refclk);	/* NVMe: enable port refclk on T8103 */

	return 0;	/* NVMe: refclk stable, can deassert PERST# */
}

static void __iomem *port_rid2sid_addr(struct apple_pcie_port *port, int idx)	/* NVMe: compute RID->SID register address for an NVMe BDF */
{
	return port->base + port->pcie->hw->port_rid2sid + 4 * idx;	/* NVMe: each entry is 32 bits */
}

static u32 apple_pcie_rid2sid_write(struct apple_pcie_port *port,
				    int idx, u32 val)	/* NVMe: program RID->SID entry for NVMe IOMMU stream */
{
	writel_relaxed(val, port_rid2sid_addr(port, idx));	/* NVMe: write mapping for NVMe device */
	/* Read back to ensure completion of the write */
	return readl_relaxed(port_rid2sid_addr(port, idx));	/* NVMe: verify mapping latched before DMA enabled */
}

static int apple_pcie_setup_port(struct apple_pcie *pcie,
				 struct device_node *np)	/* NVMe: initialize one root port that may connect an NVMe SSD */
{
	struct platform_device *platform = to_platform_device(pcie->dev);	/* NVMe: platform device for resource lookup */
	struct apple_pcie_port *port;	/* NVMe: per-root-port state */
	struct gpio_desc *reset;	/* NVMe: PERST# GPIO for NVMe slot */
	struct resource *res;
	char name[16];
	u32 stat, idx;
	int ret, i;

	reset = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_LOW, "PERST#");
							/* NVMe: acquire PERST# GPIO, initial low (deasserted) */
	if (IS_ERR(reset))
		return PTR_ERR(reset);	/* NVMe: no reset GPIO, cannot safely probe NVMe */

	port = devm_kzalloc(pcie->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->sid_map = devm_bitmap_zalloc(pcie->dev, pcie->hw->max_rid2sid, GFP_KERNEL);
							/* NVMe: track allocated RID->SID contexts */
	if (!port->sid_map)
		return -ENOMEM;

	ret = of_property_read_u32_index(np, "reg", 0, &idx);
	if (ret)
		return ret;

	/* Use the first reg entry to work out the port index */
	port->idx = idx >> 11;				/* NVMe: port index encoded in high bits of reg */
	port->pcie = pcie;				/* NVMe: link port to host bridge */
	port->np = np;					/* NVMe: remember OF node */

	raw_spin_lock_init(&port->lock);

	snprintf(name, sizeof(name), "port%d", port->idx);
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	if (!res)
		res = platform_get_resource(platform, IORESOURCE_MEM, port->idx + 2);
							/* NVMe: fallback resource index for port MMIO */

	port->base = devm_ioremap_resource(&platform->dev, res);
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);		/* NVMe: cannot access port registers */

	snprintf(name, sizeof(name), "phy%d", port->idx);
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	if (res)
		port->phy = devm_ioremap_resource(&platform->dev, res);
							/* NVMe: dedicated PHY MMIO */
	else
		port->phy = pcie->base + CORE_PHY_DEFAULT_BASE(port->idx);
							/* NVMe: shared PHY register block */

	rmw_set(PORT_APPCLK_EN, port->base + PORT_APPCLK);	/* NVMe: enable port application clock */

	/* Assert PERST# before setting up the clock */
	gpiod_set_value_cansleep(reset, 1);	/* NVMe: hold NVMe SSD in reset while clocks stabilize */

	ret = apple_pcie_setup_refclk(pcie, port);
	if (ret < 0)
		return ret;				/* NVMe: refclk failure prevents NVMe bringup */

	/* The minimal Tperst-clk value is 100us (PCIe CEM r5.0, 2.9.2) */
	usleep_range(100, 200);	/* NVMe: ensure stable clocks before releasing NVMe reset */

	/* Deassert PERST# */
	rmw_set(PORT_PERST_OFF, port->base + pcie->hw->port_perst);	/* NVMe: release NVMe SSD from reset via register */
	gpiod_set_value_cansleep(reset, 0);				/* NVMe: release PERST# via GPIO */

	/* Wait for 100ms after PERST# deassertion (PCIe r5.0, 6.6.1) */
	msleep(100);	/* NVMe: give NVMe controller time to exit reset and train link */

	ret = readl_relaxed_poll_timeout(port->base + PORT_STATUS, stat,
					 stat & PORT_STATUS_READY, 100, 250000);
	if (ret < 0) {
		dev_err(pcie->dev, "port %pOF ready wait timeout\n", np);
		return ret;				/* NVMe: port not ready, NVMe enumeration fails */
	}

	if (pcie->hw->port_refclk)
		rmw_clear(PORT_REFCLK_CGDIS, port->base + pcie->hw->port_refclk);
							/* NVMe: allow refclk clock gating for ASPM on T8103 */
	else
		rmw_set(PHY_LANE_CFG_REFCLKCGEN, port->phy + PHY_LANE_CFG);
							/* NVMe: enable PHY refclk gating for ASPM on T602x */

	rmw_clear(PORT_APPCLK_CGDIS, port->base + PORT_APPCLK);	/* NVMe: allow app clock gating for power saving */

	ret = apple_pcie_port_setup_irq(port);
	if (ret)
		return ret;				/* NVMe: IRQ setup failed, no NVMe interrupts */

	/* Reset all RID/SID mappings, and check for RAZ/WI registers */
	for (i = 0; i < pcie->hw->max_rid2sid; i++) {
		if (apple_pcie_rid2sid_write(port, i, 0xbad1d) != 0xbad1d)
			break;				/* NVMe: register is RAZ/WI; stop probing */
		apple_pcie_rid2sid_write(port, i, 0);	/* NVMe: clear stale NVMe IOMMU mapping */
	}

	dev_dbg(pcie->dev, "%pOF: %d RID/SID mapping entries\n", np, i);

	port->sid_map_sz = i;	/* NVMe: actual number of RID->SID slots available */

	list_add_tail(&port->entry, &pcie->ports);	/* NVMe: add port to host bridge list */
	init_completion(&pcie->event);			/* NVMe: completion used while waiting for link up */

	/* In the success path, we keep a reference to np around */
	of_node_get(np);

	ret = apple_pcie_port_register_irqs(port);
	WARN_ON(ret);					/* NVMe: link event registration failed */

	writel_relaxed(PORT_LTSSMCTL_START, port->base + PORT_LTSSMCTL);
							/* NVMe: start link training so NVMe SSD can respond */

	if (!wait_for_completion_timeout(&pcie->event, HZ / 10))
		dev_warn(pcie->dev, "%pOF link didn't come up\n", np);
							/* NVMe: link training timeout; NVMe may be absent/failed */

	return 0;
}

static const struct msi_parent_ops apple_msi_parent_ops = {
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX		|
				   MSI_FLAG_MULTI_PCI_MSI),	/* NVMe: support MSI, multi-MSI, and MSI-X for NVMe queues */
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   MSI_FLAG_PCI_MSI_MASK_PARENT),	/* NVMe: use default domain/chip ops and parent masking */
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,	/* NVMe: need EOI for NVMe MSI delivery */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,		/* NVMe: this domain serves PCI MSI bus */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* NVMe: initialize MSI info for NVMe pci_dev */
};

static int apple_msi_init(struct apple_pcie *pcie)	/* NVMe: create MSI parent domain for NVMe devices */
{
	struct fwnode_handle *fwnode = dev_fwnode(pcie->dev);
	struct irq_domain_info info = {
		.fwnode		= fwnode,		/* NVMe: device fwnode backs MSI domain */
		.ops		= &apple_msi_domain_ops,	/* NVMe: custom alloc/free for NVMe vector bitmap */
		.size		= pcie->nvecs,		/* NVMe: domain size set from DT below */
		.host_data	= pcie,			/* NVMe: host bridge stored in domain */
	};
	struct of_phandle_args args = {};
	int ret;

	ret = of_parse_phandle_with_args(to_of_node(fwnode), "msi-ranges",
					 "#interrupt-cells", 0, &args);
							/* NVMe: parse DT msi-ranges for parent IRQ */
	if (ret)
		return ret;

	ret = of_property_read_u32_index(to_of_node(fwnode), "msi-ranges",
					 args.args_count + 1, &pcie->nvecs);
							/* NVMe: number of MSI vectors available to NVMe */
	if (ret)
		return ret;

	of_phandle_args_to_fwspec(args.np, args.args, args.args_count,
				  &pcie->fwspec);	/* NVMe: build parent fwspec for vector allocation */

	pcie->bitmap = devm_bitmap_zalloc(pcie->dev, pcie->nvecs, GFP_KERNEL);
	if (!pcie->bitmap)
		return -ENOMEM;

	info.parent = irq_find_matching_fwspec(&pcie->fwspec, DOMAIN_BUS_WIRED);
							/* NVMe: locate parent wired domain for MSI doorbell */
	if (!info.parent) {
		dev_err(pcie->dev, "failed to find parent domain\n");
		return -ENXIO;
	}

	if (!msi_create_parent_irq_domain(&info, &apple_msi_parent_ops)) {
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}
	return 0;	/* NVMe: MSI domain ready; pci_enable_msi/msix can now work for NVMe */
}

static struct apple_pcie *apple_pcie_lookup(struct device *dev)	/* NVMe: get host bridge private data */
{
	return pci_host_bridge_priv(dev_get_drvdata(dev));	/* NVMe: host bridge private stores apple_pcie state */
}

static struct apple_pcie_port *apple_pcie_get_port(struct pci_dev *pdev)	/* NVMe: find root port serving this NVMe device */
{
	struct pci_config_window *cfg = pdev->sysdata;	/* NVMe: ECAM window sysdata from host bridge */
	struct apple_pcie *pcie;
	struct pci_dev *port_pdev;
	struct apple_pcie_port *port;

	pcie = apple_pcie_lookup(cfg->parent);
	if (WARN_ON(!pcie))
		return NULL;	/* NVMe: host bridge not found */

	/* Find the root port this device is on */
	port_pdev = pcie_find_root_port(pdev);
							/* NVMe: root port upstream of NVMe SSD */

	/* If finding the port itself, nothing to do */
	if (WARN_ON(!port_pdev) || pdev == port_pdev)
		return NULL;	/* NVMe: called on root port itself, no RID->SID needed */

	list_for_each_entry(port, &pcie->ports, entry) {
		if (port->idx == PCI_SLOT(port_pdev->devfn))
			return port;			/* NVMe: match root port by slot number */
	}

	return NULL;	/* NVMe: no matching root port */
}

static int apple_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
	/* NVMe: called when NVMe device is enabled; sets up IOMMU RID->SID mapping */
{
	u32 sid, rid = pci_dev_id(pdev);			/* NVMe: rid = bus:dev:func of NVMe SSD */
	struct apple_pcie_port *port;
	int idx, err;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return 0;				/* NVMe: root port itself, skip mapping */

	dev_dbg(&pdev->dev, "added to bus %s, index %d\n",
		pci_name(pdev->bus->self), port->idx);

	err = of_map_id(port->pcie->dev->of_node, rid, "iommu-map",
			"iommu-map-mask", NULL, &sid);
							/* NVMe: translate RID to IOMMU StreamID for NVMe DMA */
	if (err)
		return err;

	mutex_lock(&port->pcie->lock);				/* NVMe: protect RID->SID table and bitmap */

	idx = bitmap_find_free_region(port->sid_map, port->sid_map_sz, 0);
							/* NVMe: allocate a free RID->SID slot for NVMe */
	if (idx >= 0) {
		apple_pcie_rid2sid_write(port, idx,
					 PORT_RID2SID_VALID |
					 (sid << PORT_RID2SID_SID_SHIFT) | rid);
							/* NVMe: program valid mapping so NVMe DMA is translated */

		dev_dbg(&pdev->dev, "mapping RID%x to SID%x (index %d)\n",
			rid, sid, idx);
	}

	mutex_unlock(&port->pcie->lock);

	return idx >= 0 ? 0 : -ENOSPC;				/* NVMe: no free RID->SID entries */
}

static void apple_pcie_disable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
	/* NVMe: tear down IOMMU RID->SID mapping when NVMe device is disabled */
{
	struct apple_pcie_port *port;
	u32 rid = pci_dev_id(pdev);				/* NVMe: RID of NVMe SSD being removed */
	int idx;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return;						/* NVMe: root port itself */

	mutex_lock(&port->pcie->lock);				/* NVMe: protect RID->SID table */

	for_each_set_bit(idx, port->sid_map, port->sid_map_sz) {
		u32 val;

		val = readl_relaxed(port_rid2sid_addr(port, idx));
		if ((val & 0xffff) == rid) {
			apple_pcie_rid2sid_write(port, idx, 0);		/* NVMe: invalidate mapping so NVMe DMA is no longer translated */
			bitmap_release_region(port->sid_map, idx, 0);	/* NVMe: free slot for next NVMe device */
			dev_dbg(&pdev->dev, "Released %x (%d)\n", val, idx);
			break;
		}
	}

	mutex_unlock(&port->pcie->lock);
}

static int apple_pcie_init(struct pci_config_window *cfg)	/* NVMe: ECAM ops init; sets up all NVMe root ports */
{
	struct device *dev = cfg->parent;			/* NVMe: host bridge device */
	struct apple_pcie *pcie;
	int ret;

	pcie = apple_pcie_lookup(dev);
	if (WARN_ON(!pcie))
		return -ENOENT;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		ret = apple_pcie_setup_port(pcie, of_port);
		if (ret) {
			dev_err(dev, "Port %pOF setup fail: %d\n", of_port, ret);
			return ret;				/* NVMe: abort probe if any NVMe port fails */
		}
	}

	return 0;	/* NVMe: all root ports initialized; ECAM enumeration can proceed */
}

static const struct pci_ecam_ops apple_pcie_cfg_ecam_ops = {
	.init		= apple_pcie_init,			/* NVMe: initialize root ports before NVMe enumeration */
	.enable_device	= apple_pcie_enable_device,		/* NVMe: setup IOMMU mapping when NVMe driver probes */
	.disable_device	= apple_pcie_disable_device,		/* NVMe: cleanup IOMMU mapping on NVMe remove/suspend */
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,		/* NVMe: map ECAM config space for NVMe config reads/writes */
		.read		= pci_generic_config_read,	/* NVMe: read NVMe config space (VID/DID/BAR/etc) */
		.write		= pci_generic_config_write,	/* NVMe: write NVMe config space (BAR, command, MSI) */
	}
};

static int apple_pcie_probe(struct platform_device *pdev)	/* NVMe: platform probe; registers Apple PCIe host bridge */
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct apple_pcie *pcie;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);			/* NVMe: private data attached to host bridge */
	pcie->dev = dev;					/* NVMe: platform device */
	pcie->hw = of_device_get_match_data(dev);		/* NVMe: select T8103 or T602x register layout */
	if (!pcie->hw)
		return -ENODEV;
	pcie->base = devm_platform_ioremap_resource(pdev, 1);
								/* NVMe: ioremap root-complex registers */
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	mutex_init(&pcie->lock);				/* NVMe: init host bridge lock */
	INIT_LIST_HEAD(&pcie->ports);				/* NVMe: empty port list */

	ret = apple_msi_init(pcie);
	if (ret)
		return ret;					/* NVMe: MSI domain required for NVMe interrupts */

	return pci_host_common_init(pdev, bridge, &apple_pcie_cfg_ecam_ops);
								/* NVMe: register host bridge; starts ECAM enumeration of NVMe SSDs */
}

static const struct of_device_id apple_pcie_of_match[] = {
	{ .compatible = "apple,t6020-pcie",	.data = &t602x_hw },	/* NVMe: T602x (M2 Pro/Max) host bridge */
	{ .compatible = "apple,pcie",		.data = &t8103_hw },	/* NVMe: T8103 (M1) host bridge */
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pcie_of_match);

static struct platform_driver apple_pcie_driver = {
	.probe	= apple_pcie_probe,				/* NVMe: probe host bridge for NVMe SSD */
	.driver	= {
		.name			= "pcie-apple",
		.of_match_table		= apple_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(apple_pcie_driver);

MODULE_DESCRIPTION("Apple PCIe host bridge driver");
MODULE_LICENSE("GPL v2");
