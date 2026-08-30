// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe host controller driver for Xilinx XDMA PCIe Bridge
 *
 * Copyright (C) 2023 Xilinx, Inc. All rights reserved.
 */
#include <linux/bitfield.h>		/* PCI/NVMe: GENMASK/FIELD_GET for parsing AER/capability registers */
#include <linux/interrupt.h>		/* PCI/NVMe: IRQ registration for NVMe MSI-X/MSI/INTx paths */
#include <linux/irq.h>			/* PCI/NVMe: irq_data, hwirq used when mapping NVMe interrupts */
#include <linux/irqchip/irq-msi-lib.h>	/* PCI/NVMe: generic MSI parent domain helpers shared with NVMe pdev */
#include <linux/irqdomain.h>		/* PCI/NVMe: irq domains bridge NVMe EP MSI vectors to Linux virqs */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msi.h>			/* PCI/NVMe: MSI_FLAG_* constraints for NVMe multi-vector MSI */
#include <linux/of_address.h>		/* PCI/NVMe: parse DT "reg" for ECAM/NVMe BAR-like windows */
#include <linux/of_pci.h>		/* PCI/NVMe: OF helpers for PCIe root bridge/NVMe enumeration */

#include "../pci.h"			/* PCI/NVMe: core PCI host bridge definitions NVMe driver relies on */
#include "pcie-xilinx-common.h"		/* PCI/NVMe: shared Xilinx ECAM offset macro used for NVMe config access */

/* Register definitions */
#define XILINX_PCIE_DMA_REG_IDR			0x00000138	/* PCI/NVMe: Interrupt Decode Register; reflects events from downstream NVMe */
#define XILINX_PCIE_DMA_REG_IMR			0x0000013c	/* PCI/NVMe: Interrupt Mask Register; gates AER/INTx/MSI events from NVMe EP */
#define XILINX_PCIE_DMA_REG_PSCR		0x00000144	/* PCI/NVMe: PHY status; LINKUP bit tells if NVMe link is usable */
#define XILINX_PCIE_DMA_REG_RPSC		0x00000148	/* PCI/NVMe: Root Port status/control; bridge enable bit */
#define XILINX_PCIE_DMA_REG_MSIBASE1		0x0000014c	/* PCI/NVMe: Upper 32b of MSI target address for NVMe MSI writes */
#define XILINX_PCIE_DMA_REG_MSIBASE2		0x00000150	/* PCI/NVMe: Lower 32b of MSI target address for NVMe MSI writes */
#define XILINX_PCIE_DMA_REG_RPEFR		0x00000154	/* PCI/NVMe: Root Port Error FIFO Register; AER-like error info from NVMe */
#define XILINX_PCIE_DMA_REG_IDRN		0x00000160	/* PCI/NVMe: Raw INTx request status from NVMe INTA..D */
#define XILINX_PCIE_DMA_REG_IDRN_MASK		0x00000164	/* PCI/NVMe: INTx mask; mirrors PCI Command INTX_DISABLE for NVMe legacy IRQs */
#define XILINX_PCIE_DMA_REG_MSI_LOW		0x00000170	/* PCI/NVMe: Pending low 32 MSI vectors from NVMe EP */
#define XILINX_PCIE_DMA_REG_MSI_HI		0x00000174	/* PCI/NVMe: Pending high 32 MSI vectors from NVMe EP */
#define XILINX_PCIE_DMA_REG_MSI_LOW_MASK	0x00000178	/* PCI/NVMe: Mask low 32 MSI vectors (NVMe MSI vs MSI-X) */
#define XILINX_PCIE_DMA_REG_MSI_HI_MASK		0x0000017c	/* PCI/NVMe: Mask high 32 MSI vectors */

#define IMR(x) BIT(XILINX_PCIE_INTR_ ##x)	/* PCI/NVMe: helper to build IMR bit for NVMe-related event x */

/* PCI/NVMe: IMR mask covering all root-port events that can affect NVMe traffic */
#define XILINX_PCIE_INTR_IMR_ALL_MASK	\
	(					\
		IMR(LINK_DOWN)		|	\
		IMR(HOT_RESET)		|	\
		IMR(CFG_TIMEOUT)	|	\
		IMR(CORRECTABLE)	|	\
		IMR(NONFATAL)		|	\
		IMR(FATAL)		|	\
		IMR(INTX)		|	\
		IMR(MSI)		|	\
		IMR(SLV_UNSUPP)		|	\
		IMR(SLV_UNEXP)		|	\
		IMR(SLV_COMPL)		|	\
		IMR(SLV_ERRP)		|	\
		IMR(SLV_CMPABT)		|	\
		IMR(SLV_ILLBUR)		|	\
		IMR(MST_DECERR)		|	\
		IMR(MST_SLVERR)		|	\
	)	/* PCI/NVMe: link/AER/INTx/MSI/DMA error events seen from NVMe EP */

#define XILINX_PCIE_DMA_IMR_ALL_MASK	0x0ff30fe9	/* PCI/NVMe: default mask value for NVMe-related IMR */
#define XILINX_PCIE_DMA_IDR_ALL_MASK	0xffffffff	/* PCI/NVMe: clear all pending IRQ bits affecting NVMe */
#define XILINX_PCIE_DMA_IDRN_MASK	GENMASK(19, 16)	/* PCI/NVMe: bits [19:16] hold NVMe INTA..INTD status */

/* Root Port Error Register definitions */
#define XILINX_PCIE_DMA_RPEFR_ERR_VALID	BIT(18)		/* PCI/NVMe: AER-like error valid bit from NVMe requester */
#define XILINX_PCIE_DMA_RPEFR_REQ_ID	GENMASK(15, 0)	/* PCI/NVMe: requester ID of NVMe EP that signaled the error */
#define XILINX_PCIE_DMA_RPEFR_ALL_MASK	0xffffffff	/* PCI/NVMe: clear all error FIFO bits */

/* Root Port Interrupt Register definitions */
#define XILINX_PCIE_DMA_IDRN_SHIFT	16		/* PCI/NVMe: INTx bits start at bit 16 in IDRN */

/* Root Port Status/control Register definitions */
#define XILINX_PCIE_DMA_REG_RPSC_BEN	BIT(0)		/* PCI/NVMe: bridge enable; must be set before NVMe enumeration */

/* Phy Status/Control Register definitions */
#define XILINX_PCIE_DMA_REG_PSCR_LNKUP	BIT(11)		/* PCI/NVMe: link up status required for NVMe config/I/O */
#define QDMA_BRIDGE_BASE_OFF		0xcd8		/* PCI/NVMe: QDMA variant register offset for host bridge regs */

/* Number of MSI IRQs */
#define XILINX_NUM_MSI_IRQS	64			/* PCI/NVMe: max MSI vectors allocatable to one NVMe endpoint */

enum xilinx_pl_dma_version {
	XDMA,						/* PCI/NVMe: XDMA-based host bridge variant */
	QDMA,						/* PCI/NVMe: QDMA-based host bridge variant */
};

/**
 * struct xilinx_pl_dma_variant - PL DMA PCIe variant information
 * @version: DMA version
 */
struct xilinx_pl_dma_variant {
	enum xilinx_pl_dma_version version;	/* PCI/NVMe: selects register layout used during NVMe access */
};

struct xilinx_msi {
	unsigned long		*bitmap;	/* PCI/NVMe: tracks which MSI vectors are assigned to NVMe functions */
	struct irq_domain	*dev_domain;	/* PCI/NVMe: per-device MSI domain for NVMe pdev */
	struct mutex		lock;		/* Protect bitmap variable */
	int			irq_msi0;	/* PCI/NVMe: Linux IRQ for low 32 NVMe MSI vectors */
	int			irq_msi1;	/* PCI/NVMe: Linux IRQ for high 32 NVMe MSI vectors */
};

/**
 * struct pl_dma_pcie - PCIe port information
 * @dev: Device pointer
 * @reg_base: IO Mapped Register Base
 * @cfg_base: IO Mapped Configuration Base
 * @irq: Interrupt number
 * @cfg: Holds mappings of config space window
 * @phys_reg_base: Physical address of reg base
 * @intx_domain: Legacy IRQ domain pointer
 * @pldma_domain: PL DMA IRQ domain pointer
 * @resources: Bus Resources
 * @msi: MSI information
 * @intx_irq: INTx error interrupt number
 * @lock: Lock protecting shared register access
 * @variant: PL DMA PCIe version check pointer
 */
struct pl_dma_pcie {
	struct device			*dev;			/* PCI/NVMe: pdev dev, used for devm_* and NVMe dma_map_ops */
	void __iomem			*reg_base;		/* PCI/NVMe: host bridge register window for NVMe link/IRQ config */
	void __iomem			*cfg_base;		/* PCI/NVMe: ECAM base used for NVMe config read/write */
	int				irq;			/* PCI/NVMe: consolidated event IRQ line for NVMe errors/link */
	struct pci_config_window	*cfg;			/* PCI/NVMe: ECAM window descriptor; pci_generic_config_read uses this for NVMe */
	phys_addr_t			phys_reg_base;		/* PCI/NVMe: physical base used as MSI target address from NVMe EP */
	struct irq_domain		*intx_domain;		/* PCI/NVMe: domain translating NVMe INTA..INTD to Linux IRQs */
	struct irq_domain		*pldma_domain;		/* PCI/NVMe: domain for root-port events relevant to NVMe */
	struct list_head		resources;		/* PCI/NVMe: PCI bus resources (MEM/IO) handed to NVMe BAR allocation */
	struct xilinx_msi		msi;			/* PCI/NVMe: MSI controller state serving NVMe MSI requests */
	int				intx_irq;		/* PCI/NVMe: mapped Linux IRQ for NVMe INTx aggregation */
	raw_spinlock_t			lock;			/* PCI/NVMe: serializes host bridge reg access during NVMe IRQ setup */
	const struct xilinx_pl_dma_variant   *variant;	/* PCI/NVMe: XDMA/QDMA variant driving NVMe register layout */
};

static inline u32 pcie_read(struct pl_dma_pcie *port, u32 reg)
{
	if (port->variant->version == QDMA)		/* PCI/NVMe: QDMA shifts bridge regs, but ECAM/NVMe config path differs */
		return readl(port->reg_base + reg + QDMA_BRIDGE_BASE_OFF);

	return readl(port->reg_base + reg);		/* PCI/NVMe: XDMA reads host bridge register for NVMe status/control */
}

static inline void pcie_write(struct pl_dma_pcie *port, u32 val, u32 reg)
{
	if (port->variant->version == QDMA)		/* PCI/NVMe: QDMA writes bridge reg at fixed offset */
		writel(val, port->reg_base + reg + QDMA_BRIDGE_BASE_OFF);
	else						/* PCI/NVMe: XDMA writes directly to host bridge register affecting NVMe link/IRQs */
		writel(val, port->reg_base + reg);
}

static inline bool xilinx_pl_dma_pcie_link_up(struct pl_dma_pcie *port)
{
	return (pcie_read(port, XILINX_PCIE_DMA_REG_PSCR) &	/* PCI/NVMe: read PHY status while probing NVMe presence */
		XILINX_PCIE_DMA_REG_PSCR_LNKUP) ? true : false;	/* PCI/NVMe: link up == NVMe device reachable on PCIe */
}

static void xilinx_pl_dma_pcie_clear_err_interrupts(struct pl_dma_pcie *port)
{
	unsigned long val = pcie_read(port, XILINX_PCIE_DMA_REG_RPEFR);	/* PCI/NVMe: fetch AER-like error FIFO from NVMe EP */

	if (val & XILINX_PCIE_DMA_RPEFR_ERR_VALID) {			/* PCI/NVMe: only clear if NVMe error is present */
		dev_dbg(port->dev, "Requester ID %lu\n",
			val & XILINX_PCIE_DMA_RPEFR_REQ_ID);		/* PCI/NVMe: log NVMe requester ID for AER tracing */
		pcie_write(port, XILINX_PCIE_DMA_RPEFR_ALL_MASK,	/* PCI/NVMe: acknowledge/clear NVMe-reported error */
			   XILINX_PCIE_DMA_REG_RPEFR);
	}
}

static bool xilinx_pl_dma_pcie_valid_device(struct pci_bus *bus,
					    unsigned int devfn)
{
	struct pl_dma_pcie *port = bus->sysdata;	/* PCI/NVMe: host bridge private data used for NVMe bus access */

	if (!pci_is_root_bus(bus)) {			/* PCI/NVMe: downstream NVMe device access below root port */
		/*
		 * Checking whether the link is up is the last line of
		 * defense, and this check is inherently racy by definition.
		 * Sending a PIO request to a downstream device when the link is
		 * down causes an unrecoverable error, and a reset of the entire
		 * PCIe controller will be needed. We can reduce the likelihood
		 * of that unrecoverable error by checking whether the link is
		 * up, but we can't completely prevent it because the link may
		 * go down between the link-up check and the PIO request.
		 */
		if (!xilinx_pl_dma_pcie_link_up(port))	/* PCI/NVMe: avoid NVMe config access if link dropped (e.g. hotplug) */
			return false;
	} else if (devfn > 0)				/* PCI/NVMe: root port exposes only devfn 0 to NVMe bus; skip others */
		/* Only one device down on each root port */
		return false;

	return true;					/* PCI/NVMe: device/address valid for NVMe config cycle */
}

static void __iomem *xilinx_pl_dma_pcie_map_bus(struct pci_bus *bus,
						unsigned int devfn, int where)
{
	struct pl_dma_pcie *port = bus->sysdata;	/* PCI/NVMe: bridge context for this NVMe bus */

	if (!xilinx_pl_dma_pcie_valid_device(bus, devfn))	/* PCI/NVMe: skip config mapping for absent NVMe slots */
		return NULL;

	if (port->variant->version == QDMA)		/* PCI/NVMe: QDMA uses separate ECAM window for NVMe config space */
		return port->cfg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);

	return port->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);	/* PCI/NVMe: XDMA maps NVMe ECAM directly in reg_base */
}

/* PCIe operations */
static struct pci_ecam_ops xilinx_pl_dma_pcie_ops = {
	.pci_ops = {
		.map_bus = xilinx_pl_dma_pcie_map_bus,	/* PCI/NVMe: maps NVMe config access to ECAM window */
		.read	= pci_generic_config_read,		/* PCI/NVMe: reads NVMe PCI config space (VID/DID/BAR/MSI-X caps) */
		.write	= pci_generic_config_write,		/* PCI/NVMe: writes NVMe PCI config space (BAR, MSI-X table, etc.) */
	}
};

static void xilinx_pl_dma_pcie_enable_msi(struct pl_dma_pcie *port)
{
	phys_addr_t msi_addr = port->phys_reg_base;	/* PCI/NVMe: address NVMe EP writes to when generating MSI */

	pcie_write(port, upper_32_bits(msi_addr), XILINX_PCIE_DMA_REG_MSIBASE1);	/* PCI/NVMe: program MSI target high 32 bits for NVMe */
	pcie_write(port, lower_32_bits(msi_addr), XILINX_PCIE_DMA_REG_MSIBASE2);	/* PCI/NVMe: program MSI target low 32 bits for NVMe */
}

static void xilinx_mask_intx_irq(struct irq_data *data)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(data);	/* PCI/NVMe: bridge owning this NVMe INTx line */
	unsigned long flags;
	u32 mask, val;

	mask = BIT(data->hwirq + XILINX_PCIE_DMA_IDRN_SHIFT);	/* PCI/NVMe: bit for this NVMe INTA..INTD line */
	raw_spin_lock_irqsave(&port->lock, flags);		/* PCI/NVMe: serialize INTx mask while NVMe may assert IRQ */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDRN_MASK);
	pcie_write(port, (val & (~mask)), XILINX_PCIE_DMA_REG_IDRN_MASK);	/* PCI/NVMe: mask NVMe legacy interrupt (like Command bit 10) */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static void xilinx_unmask_intx_irq(struct irq_data *data)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(data);	/* PCI/NVMe: bridge owning this NVMe INTx line */
	unsigned long flags;
	u32 mask, val;

	mask = BIT(data->hwirq + XILINX_PCIE_DMA_IDRN_SHIFT);	/* PCI/NVMe: bit for this NVMe INTA..INTD line */
	raw_spin_lock_irqsave(&port->lock, flags);
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDRN_MASK);
	pcie_write(port, (val | mask), XILINX_PCIE_DMA_REG_IDRN_MASK);	/* PCI/NVMe: unmask NVMe legacy interrupt */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static struct irq_chip xilinx_leg_irq_chip = {
	.name		= "pl_dma:INTx",		/* PCI/NVMe: chip name shown for NVMe legacy IRQs */
	.irq_mask	= xilinx_mask_intx_irq,		/* PCI/NVMe: masks one NVMe INTx line */
	.irq_unmask	= xilinx_unmask_intx_irq,	/* PCI/NVMe: unmasks one NVMe INTx line */
};

static int xilinx_pl_dma_pcie_intx_map(struct irq_domain *domain,
				       unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_leg_irq_chip, handle_level_irq);	/* PCI/NVMe: bind Linux IRQ to NVMe INTx chip */
	irq_set_chip_data(irq, domain->host_data);	/* PCI/NVMe: store bridge pointer for NVMe INTx mask/unmask */
	irq_set_status_flags(irq, IRQ_LEVEL);		/* PCI/NVMe: PCIe INTx from NVMe is level-triggered */

	return 0;
}

/* INTx IRQ Domain operations */
static const struct irq_domain_ops intx_domain_ops = {
	.map = xilinx_pl_dma_pcie_intx_map,	/* PCI/NVMe: maps NVMe INTA..INTD hwirq to Linux IRQ number */
};

static irqreturn_t xilinx_pl_dma_pcie_msi_handler_high(int irq, void *args)
{
	struct xilinx_msi *msi;
	unsigned long status;
	u32 bit, virq;
	struct pl_dma_pcie *port = args;	/* PCI/NVMe: bridge receiving MSI writes from NVMe EP */

	msi = &port->msi;

	while ((status = pcie_read(port, XILINX_PCIE_DMA_REG_MSI_HI)) != 0) {	/* PCI/NVMe: loop while high 32 NVMe MSI vectors pending */
		for_each_set_bit(bit, &status, 32) {	/* PCI/NVMe: iterate each asserted high MSI vector from NVMe */
			pcie_write(port, 1 << bit, XILINX_PCIE_DMA_REG_MSI_HI);	/* PCI/NVMe: clear NVMe MSI bit to allow reassert */
			bit = bit + 32;		/* PCI/NVMe: translate to global vector index 32..63 */
			virq = irq_find_mapping(msi->dev_domain, bit);	/* PCI/NVMe: lookup Linux IRQ assigned to this NVMe MSI vector */
			if (virq)
				generic_handle_irq(virq);	/* PCI/NVMe: deliver to NVMe MSI handler (e.g. nvme_irq) */
		}
	}

	return IRQ_HANDLED;	/* PCI/NVMe: all high MSI vectors from NVMe handled */
}

static irqreturn_t xilinx_pl_dma_pcie_msi_handler_low(int irq, void *args)
{
	struct pl_dma_pcie *port = args;	/* PCI/NVMe: bridge receiving low MSI vectors from NVMe EP */
	struct xilinx_msi *msi;
	unsigned long status;
	u32 bit, virq;

	msi = &port->msi;

	while ((status = pcie_read(port, XILINX_PCIE_DMA_REG_MSI_LOW)) != 0) {	/* PCI/NVMe: loop while low 32 NVMe MSI vectors pending */
		for_each_set_bit(bit, &status, 32) {	/* PCI/NVMe: iterate each asserted low MSI vector from NVMe */
			pcie_write(port, 1 << bit, XILINX_PCIE_DMA_REG_MSI_LOW);	/* PCI/NVMe: clear NVMe MSI bit */
			virq = irq_find_mapping(msi->dev_domain, bit);	/* PCI/NVMe: lookup Linux IRQ for NVMe vector 0..31 */
			if (virq)
				generic_handle_irq(virq);	/* PCI/NVMe: run NVMe queue completion interrupt handler */
		}
	}

	return IRQ_HANDLED;	/* PCI/NVMe: all low MSI vectors from NVMe handled */
}

static irqreturn_t xilinx_pl_dma_pcie_event_flow(int irq, void *args)
{
	struct pl_dma_pcie *port = args;	/* PCI/NVMe: bridge context for NVMe root-port events */
	unsigned long val;
	int i;

	val = pcie_read(port, XILINX_PCIE_DMA_REG_IDR);	/* PCI/NVMe: read raw event status affecting NVMe */
	val &= pcie_read(port, XILINX_PCIE_DMA_REG_IMR);	/* PCI/NVMe: apply mask to find enabled NVMe events */
	for_each_set_bit(i, &val, 32)				/* PCI/NVMe: dispatch each active NVMe-related event */
		generic_handle_domain_irq(port->pldma_domain, i);	/* PCI/NVMe: route to per-event handler (link/AER/INTx) */

	pcie_write(port, val, XILINX_PCIE_DMA_REG_IDR);		/* PCI/NVMe: clear handled events so NVMe can re-trigger */

	return IRQ_HANDLED;	/* PCI/NVMe: root port events for NVMe handled */
}

/* PCI/NVMe: helper to populate intr_cause[] for NVMe-related root-port events */
#define _IC(x, s)                              \
	[XILINX_PCIE_INTR_ ## x] = { __stringify(x), s }

static const struct {
	const char	*sym;
	const char	*str;
} intr_cause[32] = {
	_IC(LINK_DOWN,		"Link Down"),			/* PCI/NVMe: NVMe link down -> device lost */
	_IC(HOT_RESET,		"Hot reset"),			/* PCI/NVMe: NVMe EP reset event */
	_IC(CFG_TIMEOUT,	"ECAM access timeout"),		/* PCI/NVMe: NVMe config space access timeout */
	_IC(CORRECTABLE,	"Correctable error message"),	/* PCI/NVMe: NVMe correctable AER message */
	_IC(NONFATAL,		"Non fatal error message"),	/* PCI/NVMe: NVMe non-fatal AER message */
	_IC(FATAL,		"Fatal error message"),		/* PCI/NVMe: NVMe fatal AER message */
	_IC(SLV_UNSUPP,		"Slave unsupported request"),	/* PCI/NVMe: NVMe issued UR to host DMA */
	_IC(SLV_UNEXP,		"Slave unexpected completion"),	/* PCI/NVMe: NVMe completion mismatch */
	_IC(SLV_COMPL,		"Slave completion timeout"),	/* PCI/NVMe: NVMe completion timeout */
	_IC(SLV_ERRP,		"Slave Error Poison"),		/* PCI/NVMe: NVMe poisoned TLP */
	_IC(SLV_CMPABT,		"Slave Completer Abort"),	/* PCI/NVMe: NVMe completer abort */
	_IC(SLV_ILLBUR,		"Slave Illegal Burst"),		/* PCI/NVMe: NVMe illegal burst */
	_IC(MST_DECERR,		"Master decode error"),		/* PCI/NVMe: host DMA to NVMe decode error */
	_IC(MST_SLVERR,		"Master slave error"),		/* PCI/NVMe: host DMA to NVMe slave error */
};

static irqreturn_t xilinx_pl_dma_pcie_intr_handler(int irq, void *dev_id)
{
	struct pl_dma_pcie *port = (struct pl_dma_pcie *)dev_id;	/* PCI/NVMe: bridge for this NVMe root port */
	struct device *dev = port->dev;
	struct irq_data *d;

	d = irq_domain_get_irq_data(port->pldma_domain, irq);	/* PCI/NVMe: get hwirq index for NVMe event */
	switch (d->hwirq) {
	case XILINX_PCIE_INTR_CORRECTABLE:
	case XILINX_PCIE_INTR_NONFATAL:
	case XILINX_PCIE_INTR_FATAL:
		xilinx_pl_dma_pcie_clear_err_interrupts(port);	/* PCI/NVMe: clear AER error FIFO from NVMe EP */
		fallthrough;					/* PCI/NVMe: also log the AER event below */

	default:
		if (intr_cause[d->hwirq].str)
			dev_warn(dev, "%s\n", intr_cause[d->hwirq].str);	/* PCI/NVMe: warn about NVMe-related event */
		else
			dev_warn(dev, "Unknown IRQ %ld\n", d->hwirq);	/* PCI/NVMe: unexpected event during NVMe operation */
	}

	return IRQ_HANDLED;	/* PCI/NVMe: root port event for NVMe logged/cleared */
}

/* PCI/NVMe: required MSI flags: generic domain/chip ops, no affinity for FPGA NVMe */
#define XILINX_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				   MSI_FLAG_USE_DEF_CHIP_OPS	| \
				   MSI_FLAG_NO_AFFINITY)

/* PCI/NVMe: supported MSI flags: generic mask plus multi-vector MSI for NVMe */
#define XILINX_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				    MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops xilinx_msi_parent_ops = {
	.required_flags		= XILINX_MSI_FLAGS_REQUIRED,	/* PCI/NVMe: mandatory flags for NVMe MSI allocation */
	.supported_flags	= XILINX_MSI_FLAGS_SUPPORTED,	/* PCI/NVMe: flags NVMe may request (e.g. multi MSI) */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,		/* PCI/NVMe: this domain serves PCI/NVMe MSI devices */
	.prefix			= "pl_dma-",			/* PCI/NVMe: IRQ name prefix for NVMe MSI lines */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* PCI/NVMe: populate msi_device_info for NVMe pdev */
};
static void xilinx_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct pl_dma_pcie *pcie = irq_data_get_irq_chip_data(data);	/* PCI/NVMe: bridge owning this NVMe MSI vector */
	phys_addr_t msi_addr = pcie->phys_reg_base;			/* PCI/NVMe: target address NVMe EP must write */

	msg->address_lo = lower_32_bits(msi_addr);	/* PCI/NVMe: low 32 bits of NVMe MSI target address */
	msg->address_hi = upper_32_bits(msi_addr);	/* PCI/NVMe: high 32 bits of NVMe MSI target address */
	msg->data = data->hwirq;			/* PCI/NVMe: MSI data == vector index NVMe EP writes */
}

static struct irq_chip xilinx_irq_chip = {
	.name = "pl_dma:MSI",				/* PCI/NVMe: chip name for NVMe MSI IRQs */
	.irq_compose_msi_msg = xilinx_compose_msi_msg,	/* PCI/NVMe: builds MSI message programmed into NVMe Message Control */
};

static int xilinx_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	struct pl_dma_pcie *pcie = domain->host_data;	/* PCI/NVMe: bridge allocating vectors for NVMe */
	struct xilinx_msi *msi = &pcie->msi;
	int bit, i;

	mutex_lock(&msi->lock);	/* PCI/NVMe: serialize vector allocation across multiple NVMe functions */
	bit = bitmap_find_free_region(msi->bitmap, XILINX_NUM_MSI_IRQS,
				      get_count_order(nr_irqs));	/* PCI/NVMe: find contiguous free vectors for NVMe MSI group */
	if (bit < 0) {
		mutex_unlock(&msi->lock);
		return -ENOSPC;			/* PCI/NVMe: no MSI vectors left for NVMe */
	}

	for (i = 0; i < nr_irqs; i++) {	/* PCI/NVMe: bind each NVMe MSI vector to Linux IRQ chip/domain */
		irq_domain_set_info(domain, virq + i, bit + i, &xilinx_irq_chip,
				    domain->host_data, handle_simple_irq,
				    NULL, NULL);
	}
	mutex_unlock(&msi->lock);

	return 0;	/* PCI/NVMe: NVMe MSI vectors allocated successfully */
}

static void xilinx_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);	/* PCI/NVMe: first Linux IRQ of NVMe MSI group */
	struct pl_dma_pcie *pcie = irq_data_get_irq_chip_data(data);	/* PCI/NVMe: bridge freeing NVMe vectors */
	struct xilinx_msi *msi = &pcie->msi;

	mutex_lock(&msi->lock);
	bitmap_release_region(msi->bitmap, data->hwirq,
			      get_count_order(nr_irqs));	/* PCI/NVMe: return NVMe MSI vectors to free pool */
	mutex_unlock(&msi->lock);
}

static const struct irq_domain_ops dev_msi_domain_ops = {
	.alloc	= xilinx_irq_domain_alloc,	/* PCI/NVMe: allocate MSI vectors for NVMe endpoint */
	.free	= xilinx_irq_domain_free,	/* PCI/NVMe: free MSI vectors when NVMe driver releases them */
};

static void xilinx_pl_dma_pcie_free_irq_domains(struct pl_dma_pcie *port)
{
	struct xilinx_msi *msi = &port->msi;

	if (port->intx_domain) {
		irq_domain_remove(port->intx_domain);	/* PCI/NVMe: tear down NVMe INTx domain */
		port->intx_domain = NULL;		/* PCI/NVMe: prevent use-after-free for NVMe INTx */
	}

	if (msi->dev_domain) {
		irq_domain_remove(msi->dev_domain);	/* PCI/NVMe: tear down NVMe MSI domain */
		msi->dev_domain = NULL;			/* PCI/NVMe: prevent use-after-free for NVMe MSI */
	}
}

static int xilinx_pl_dma_pcie_init_msi_irq_domain(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;
	struct xilinx_msi *msi = &port->msi;
	int size = BITS_TO_LONGS(XILINX_NUM_MSI_IRQS) * sizeof(long);	/* PCI/NVMe: bitmap size for 64 NVMe MSI vectors */
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(port->dev),	/* PCI/NVMe: firmware node backing NVMe MSI domain */
		.ops		= &dev_msi_domain_ops,		/* PCI/NVMe: alloc/free callbacks for NVMe MSI */
		.host_data	= port,				/* PCI/NVMe: bridge passed to NVMe MSI ops */
		.size		= XILINX_NUM_MSI_IRQS,		/* PCI/NVMe: domain size limits NVMe MSI vector count */
	};

	msi->dev_domain  = msi_create_parent_irq_domain(&info, &xilinx_msi_parent_ops);	/* PCI/NVMe: create MSI parent domain for NVMe pdev */
	if (!msi->dev_domain)
		goto out;

	mutex_init(&msi->lock);	/* PCI/NVMe: init lock protecting NVMe MSI bitmap */
	msi->bitmap = kzalloc(size, GFP_KERNEL);	/* PCI/NVMe: allocate vector bitmap for NVMe endpoints */
	if (!msi->bitmap)
		goto out;

	raw_spin_lock_init(&port->lock);
	xilinx_pl_dma_pcie_enable_msi(port);	/* PCI/NVMe: program MSI target address for NVMe EP writes */

	return 0;

out:
	xilinx_pl_dma_pcie_free_irq_domains(port);	/* PCI/NVMe: rollback MSI/INTx domains on NVMe setup failure */
	dev_err(dev, "Failed to allocate MSI IRQ domains\n");

	return -ENOMEM;	/* PCI/NVMe: NVMe MSI initialization failed */
}

/*
 * INTx error interrupts are Xilinx controller specific interrupt, used to
 * notify user about errors such as cfg timeout, slave unsupported requests,
 * fatal and non fatal error etc.
 */

static irqreturn_t xilinx_pl_dma_pcie_intx_flow(int irq, void *args)
{
	unsigned long val;
	int i;
	struct pl_dma_pcie *port = args;	/* PCI/NVMe: bridge handling NVMe legacy INTx */

	val = FIELD_GET(XILINX_PCIE_DMA_IDRN_MASK,
			pcie_read(port, XILINX_PCIE_DMA_REG_IDRN));	/* PCI/NVMe: extract NVMe INTA..INTD status */

	for_each_set_bit(i, &val, PCI_NUM_INTX)			/* PCI/NVMe: dispatch each asserted NVMe INTx line */
		generic_handle_domain_irq(port->intx_domain, i);	/* PCI/NVMe: run NVMe INTx handler */
	return IRQ_HANDLED;	/* PCI/NVMe: NVMe INTx event handled */
}

static void xilinx_pl_dma_pcie_mask_event_irq(struct irq_data *d)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(d);	/* PCI/NVMe: bridge for NVMe root event */
	u32 val;

	raw_spin_lock(&port->lock);				/* PCI/NVMe: serialize IMR update for NVMe events */
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IMR);
	val &= ~BIT(d->hwirq);					/* PCI/NVMe: clear IMR bit to mask this NVMe event */
	pcie_write(port, val, XILINX_PCIE_DMA_REG_IMR);
	raw_spin_unlock(&port->lock);
}

static void xilinx_pl_dma_pcie_unmask_event_irq(struct irq_data *d)
{
	struct pl_dma_pcie *port = irq_data_get_irq_chip_data(d);	/* PCI/NVMe: bridge for NVMe root event */
	u32 val;

	raw_spin_lock(&port->lock);
	val = pcie_read(port, XILINX_PCIE_DMA_REG_IMR);
	val |= BIT(d->hwirq);					/* PCI/NVMe: set IMR bit to unmask this NVMe event */
	pcie_write(port, val, XILINX_PCIE_DMA_REG_IMR);
	raw_spin_unlock(&port->lock);
}

static struct irq_chip xilinx_pl_dma_pcie_event_irq_chip = {
	.name		= "pl_dma:RC-Event",			/* PCI/NVMe: chip for root-port events affecting NVMe */
	.irq_mask	= xilinx_pl_dma_pcie_mask_event_irq,	/* PCI/NVMe: mask an NVMe-related root event */
	.irq_unmask	= xilinx_pl_dma_pcie_unmask_event_irq,	/* PCI/NVMe: unmask an NVMe-related root event */
};

static int xilinx_pl_dma_pcie_event_map(struct irq_domain *domain,
					unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &xilinx_pl_dma_pcie_event_irq_chip,
				 handle_level_irq);	/* PCI/NVMe: bind Linux IRQ to NVMe root event chip */
	irq_set_chip_data(irq, domain->host_data);	/* PCI/NVMe: store bridge pointer for event mask/unmask */
	irq_set_status_flags(irq, IRQ_LEVEL);		/* PCI/NVMe: root events are level-triggered */

	return 0;
}

static const struct irq_domain_ops event_domain_ops = {
	.map = xilinx_pl_dma_pcie_event_map,	/* PCI/NVMe: maps NVMe root event hwirq to Linux IRQ */
};

/**
 * xilinx_pl_dma_pcie_init_irq_domain - Initialize IRQ domain
 * @port: PCIe port information
 *
 * Return: '0' on success and error value on failure.
 */
static int xilinx_pl_dma_pcie_init_irq_domain(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;
	struct device_node *node = dev->of_node;	/* PCI/NVMe: DT node describing this NVMe root bridge */
	struct device_node *pcie_intc_node;
	int ret;

	/* Setup INTx */
	pcie_intc_node = of_get_child_by_name(node, "interrupt-controller");	/* PCI/NVMe: find interrupt-controller subnode for NVMe IRQs */
	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -EINVAL;	/* PCI/NVMe: DT lacks interrupt controller for NVMe */
	}

	port->pldma_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), 32,
						      &event_domain_ops, port);	/* PCI/NVMe: create domain for 32 root events affecting NVMe */
	if (!port->pldma_domain)
		return -ENOMEM;	/* PCI/NVMe: cannot create NVMe event domain */

	irq_domain_update_bus_token(port->pldma_domain, DOMAIN_BUS_NEXUS);	/* PCI/NVMe: mark domain as nexus for NVMe root events */

	port->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, port);	/* PCI/NVMe: create domain for NVMe INTA..INTD */
	if (!port->intx_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENOMEM;	/* PCI/NVMe: cannot create NVMe INTx domain */
	}

	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED);	/* PCI/NVMe: mark domain as wired for NVMe legacy IRQs */

	ret = xilinx_pl_dma_pcie_init_msi_irq_domain(port);	/* PCI/NVMe: create MSI domain for NVMe endpoints */
	if (ret != 0) {
		irq_domain_remove(port->intx_domain);		/* PCI/NVMe: rollback INTx domain on MSI failure */
		return -ENOMEM;
	}

	of_node_put(pcie_intc_node);
	raw_spin_lock_init(&port->lock);

	return 0;	/* PCI/NVMe: all NVMe IRQ domains initialized */
}

static int xilinx_pl_dma_pcie_setup_irq(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;
	struct platform_device *pdev = to_platform_device(dev);	/* PCI/NVMe: platform device backing this NVMe root bridge */
	int i, irq, err;

	port->irq = platform_get_irq(pdev, 0);	/* PCI/NVMe: get consolidated event IRQ for NVMe root port */
	if (port->irq < 0)
		return port->irq;	/* PCI/NVMe: missing event IRQ, NVMe enumeration blocked */

	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) {	/* PCI/NVMe: create Linux IRQ for each NVMe-relevant root event */
		int err;

		if (!intr_cause[i].str)
			continue;	/* PCI/NVMe: skip unused event slots */

		irq = irq_create_mapping(port->pldma_domain, i);	/* PCI/NVMe: map root event i to Linux IRQ */
		if (!irq) {
			dev_err(dev, "Failed to map interrupt\n");
			return -ENXIO;	/* PCI/NVMe: cannot map NVMe root event */
		}

		err = devm_request_irq(dev, irq,
				       xilinx_pl_dma_pcie_intr_handler,
				       IRQF_SHARED | IRQF_NO_THREAD,
				       intr_cause[i].sym, port);
		if (err) {
			dev_err(dev, "Failed to request IRQ %d\n", irq);
			return err;	/* PCI/NVMe: cannot register NVMe event handler */
		}
	}

	port->intx_irq = irq_create_mapping(port->pldma_domain,
					    XILINX_PCIE_INTR_INTX);	/* PCI/NVMe: map INTx aggregator event to Linux IRQ */
	if (!port->intx_irq) {
		dev_err(dev, "Failed to map INTx interrupt\n");
		return -ENXIO;	/* PCI/NVMe: cannot map NVMe INTx aggregator */
	}

	err = devm_request_irq(dev, port->intx_irq, xilinx_pl_dma_pcie_intx_flow,
			       IRQF_SHARED | IRQF_NO_THREAD, NULL, port);
	if (err) {
		dev_err(dev, "Failed to request INTx IRQ %d\n", port->intx_irq);
		return err;	/* PCI/NVMe: cannot register NVMe INTx flow handler */
	}

	err = devm_request_irq(dev, port->irq, xilinx_pl_dma_pcie_event_flow,
			       IRQF_SHARED | IRQF_NO_THREAD, NULL, port);
	if (err) {
		dev_err(dev, "Failed to request event IRQ %d\n", port->irq);
		return err;	/* PCI/NVMe: cannot register NVMe root event flow handler */
	}

	return 0;	/* PCI/NVMe: all NVMe IRQ handlers registered */
}

static void xilinx_pl_dma_pcie_init_port(struct pl_dma_pcie *port)
{
	if (xilinx_pl_dma_pcie_link_up(port))	/* PCI/NVMe: verify NVMe link before enabling bridge */
		dev_info(port->dev, "PCIe Link is UP\n");	/* PCI/NVMe: NVMe device likely present */
	else
		dev_info(port->dev, "PCIe Link is DOWN\n");	/* PCI/NVMe: no NVMe device or link not trained */

	/* Disable all interrupts */
	pcie_write(port, ~XILINX_PCIE_DMA_IDR_ALL_MASK,		/* PCI/NVMe: mask all NVMe-related events initially */
		   XILINX_PCIE_DMA_REG_IMR);

	/* Clear pending interrupts */
	pcie_write(port, pcie_read(port, XILINX_PCIE_DMA_REG_IDR) &	/* PCI/NVMe: read pending NVMe events */
		   XILINX_PCIE_DMA_IMR_ALL_MASK,			/* PCI/NVMe: only clear enabled NVMe event bits */
		   XILINX_PCIE_DMA_REG_IDR);

	/* Needed for MSI DECODE MODE */
	pcie_write(port, XILINX_PCIE_DMA_IDR_ALL_MASK,			/* PCI/NVMe: unmask low MSI bits so NVMe MSI can arrive */
		   XILINX_PCIE_DMA_REG_MSI_LOW_MASK);
	pcie_write(port, XILINX_PCIE_DMA_IDR_ALL_MASK,			/* PCI/NVMe: unmask high MSI bits so NVMe MSI can arrive */
		   XILINX_PCIE_DMA_REG_MSI_HI_MASK);

	/* Set the Bridge enable bit */
	pcie_write(port, pcie_read(port, XILINX_PCIE_DMA_REG_RPSC) |	/* PCI/NVMe: read RPSC before enabling bridge for NVMe */
		   XILINX_PCIE_DMA_REG_RPSC_BEN,			/* PCI/NVMe: enable bridge so NVMe config cycles complete */
		   XILINX_PCIE_DMA_REG_RPSC);
}

static int xilinx_request_msi_irq(struct pl_dma_pcie *port)
{
	struct device *dev = port->dev;
	struct platform_device *pdev = to_platform_device(dev);	/* PCI/NVMe: platform device for this NVMe root bridge */
	int ret;

	port->msi.irq_msi0 = platform_get_irq_byname(pdev, "msi0");	/* PCI/NVMe: get IRQ for low NVMe MSI vectors */
	if (port->msi.irq_msi0 <= 0)
		return port->msi.irq_msi0;	/* PCI/NVMe: missing msi0, NVMe MSI blocked */

	ret = devm_request_irq(dev, port->msi.irq_msi0, xilinx_pl_dma_pcie_msi_handler_low,
			       IRQF_SHARED | IRQF_NO_THREAD, "xlnx-pcie-dma-pl",
			       port);
	if (ret) {
		dev_err(dev, "Failed to register interrupt\n");
		return ret;	/* PCI/NVMe: cannot register low NVMe MSI handler */
	}

	port->msi.irq_msi1 = platform_get_irq_byname(pdev, "msi1");	/* PCI/NVMe: get IRQ for high NVMe MSI vectors */
	if (port->msi.irq_msi1 <= 0)
		return port->msi.irq_msi1;	/* PCI/NVMe: missing msi1, high NVMe MSI vectors blocked */

	ret = devm_request_irq(dev, port->msi.irq_msi1, xilinx_pl_dma_pcie_msi_handler_high,
			       IRQF_SHARED | IRQF_NO_THREAD, "xlnx-pcie-dma-pl",
			       port);
	if (ret) {
		dev_err(dev, "Failed to register interrupt\n");
		return ret;	/* PCI/NVMe: cannot register high NVMe MSI handler */
	}

	return 0;	/* PCI/NVMe: both NVMe MSI IRQs registered */
}

static int xilinx_pl_dma_pcie_parse_dt(struct pl_dma_pcie *port,
				       struct resource *bus_range)
{
	struct device *dev = port->dev;
	struct platform_device *pdev = to_platform_device(dev);	/* PCI/NVMe: platform device describing NVMe root bridge */
	struct resource *res;
	int err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);	/* PCI/NVMe: first MMIO region = host bridge regs or ECAM */
	if (!res) {
		dev_err(dev, "Missing \"reg\" property\n");
		return -ENXIO;	/* PCI/NVMe: cannot locate NVMe root bridge MMIO */
	}
	port->phys_reg_base = res->start;			/* PCI/NVMe: physical base used as NVMe MSI target address */

	port->cfg = pci_ecam_create(dev, res, bus_range, &xilinx_pl_dma_pcie_ops);	/* PCI/NVMe: create ECAM window for NVMe config access */
	if (IS_ERR(port->cfg))
		return PTR_ERR(port->cfg);	/* PCI/NVMe: ECAM setup failed, NVMe enumeration impossible */

	port->reg_base = port->cfg->win;			/* PCI/NVMe: XDMA: regs and ECAM share window for NVMe config */

	if (port->variant->version == QDMA) {			/* PCI/NVMe: QDMA separates ECAM and bridge registers */
		port->cfg_base = port->cfg->win;		/* PCI/NVMe: QDMA ECAM base for NVMe config cycles */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "breg");	/* PCI/NVMe: QDMA bridge registers for NVMe link/MSI */
		port->reg_base = devm_ioremap_resource(dev, res);	/* PCI/NVMe: map QDMA bridge regs separately */
		if (IS_ERR(port->reg_base))
			return PTR_ERR(port->reg_base);	/* PCI/NVMe: QDMA bridge reg mapping failed */
		port->phys_reg_base = res->start;	/* PCI/NVMe: QDMA MSI target address */
	}

	err = xilinx_request_msi_irq(port);	/* PCI/NVMe: register hardware IRQ lines carrying NVMe MSIs */
	if (err) {
		pci_ecam_free(port->cfg);	/* PCI/NVMe: free ECAM if NVMe MSI IRQ registration failed */
		return err;
	}

	return 0;	/* PCI/NVMe: DT parsing and ECAM/MSI IRQ setup done */
}

static int xilinx_pl_dma_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: Linux device for this NVMe root bridge */
	struct pl_dma_pcie *port;
	struct pci_host_bridge *bridge;		/* PCI/NVMe: host bridge exposing NVMe device to PCI bus */
	struct resource_entry *bus;
	int err;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*port));	/* PCI/NVMe: allocate host bridge with NVMe port private data */
	if (!bridge)
		return -ENODEV;	/* PCI/NVMe: cannot allocate host bridge for NVMe */

	port = pci_host_bridge_priv(bridge);	/* PCI/NVMe: get Xilinx port struct from bridge private area */

	port->dev = dev;				/* PCI/NVMe: link bridge to device for NVMe DMA/IOMMU ops */

	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);	/* PCI/NVMe: fetch bus range for NVMe enumeration */
	if (!bus)
		return -ENODEV;	/* PCI/NVMe: missing bus range, cannot enumerate NVMe */

	port->variant = of_device_get_match_data(dev);	/* PCI/NVMe: detect XDMA/QDMA variant for NVMe register layout */

	err = xilinx_pl_dma_pcie_parse_dt(port, bus->res);	/* PCI/NVMe: parse DT, map ECAM, register NVMe MSI IRQs */
	if (err) {
		dev_err(dev, "Parsing DT failed\n");
		return err;
	}

	xilinx_pl_dma_pcie_init_port(port);	/* PCI/NVMe: enable bridge and unmask NVMe MSI */

	err = xilinx_pl_dma_pcie_init_irq_domain(port);	/* PCI/NVMe: create INTx/MSI/event domains for NVMe */
	if (err)
		goto err_irq_domain;

	err = xilinx_pl_dma_pcie_setup_irq(port);	/* PCI/NVMe: request Linux IRQs for NVMe events/INTx/MSI */

	bridge->sysdata = port;				/* PCI/NVMe: pass bridge context to NVMe config ops */
	bridge->ops = &xilinx_pl_dma_pcie_ops.pci_ops;	/* PCI/NVMe: generic PCI ops used to read NVMe config space */

	err = pci_host_probe(bridge);			/* PCI/NVMe: enumerate PCI bus and bind NVMe endpoint */
	if (err < 0)
		goto err_host_bridge;

	return 0;	/* PCI/NVMe: root bridge ready, NVMe enumeration complete */

err_host_bridge:
	xilinx_pl_dma_pcie_free_irq_domains(port);

err_irq_domain:
	pci_ecam_free(port->cfg);	/* PCI/NVMe: release ECAM window on NVMe probe failure */
	return err;
}

static const struct xilinx_pl_dma_variant xdma_host = {
	.version = XDMA,				/* PCI/NVMe: XDMA host bridge variant for NVMe */
};

static const struct xilinx_pl_dma_variant qdma_host = {
	.version = QDMA,				/* PCI/NVMe: QDMA host bridge variant for NVMe */
};

static const struct of_device_id xilinx_pl_dma_pcie_of_match[] = {
	{
		.compatible = "xlnx,xdma-host-3.00",	/* PCI/NVMe: match XDMA root bridge serving NVMe */
		.data = &xdma_host,
	},
	{
		.compatible = "xlnx,qdma-host-3.00",	/* PCI/NVMe: match QDMA root bridge serving NVMe */
		.data = &qdma_host,
	},
	{}						/* PCI/NVMe: terminate OF match table */
};

static struct platform_driver xilinx_pl_dma_pcie_driver = {
	.driver = {
		.name = "xilinx-xdma-pcie",		/* PCI/NVMe: driver name shown for NVMe root bridge */
		.of_match_table = xilinx_pl_dma_pcie_of_match,	/* PCI/NVMe: binds DT nodes to NVMe host bridge driver */
		.suppress_bind_attrs = true,		/* PCI/NVMe: no manual bind/unbind for NVMe root bridge */
	},
	.probe = xilinx_pl_dma_pcie_probe,		/* PCI/NVMe: initializes host bridge and enumerates NVMe */
};

builtin_platform_driver(xilinx_pl_dma_pcie_driver);	/* PCI/NVMe: register root bridge driver early for NVMe boot */
