// SPDX-License-Identifier: GPL-2.0+ /* NVMe: SPDX license; this file implements the PCIe root port that the NVMe driver uses */
/*
 * PCIe host controller driver for NWL PCIe Bridge
 * Based on pcie-xilinx.c, pci-tegra.c
 *
 * (C) Copyright 2014 - 2015, Xilinx, Inc.
 */ /* PCI/NVMe: end of file banner; this driver creates the PCI host bridge where NVMe SSDs appear */

#include <linux/clk.h> /* NVMe: clock API for enabling the PCIe reference clock before NVMe link training */
#include <linux/delay.h> /* NVMe: delay utilities used while polling PHY/link state before enumerating NVMe devices */
#include <linux/interrupt.h> /* NVMe: interrupt framework for legacy and MSI IRQs delivered to NVMe queues */
#include <linux/irq.h> /* NVMe: IRQ descriptor helpers used to chain legacy and MSI interrupts */
#include <linux/irqchip/irq-msi-lib.h> /* NVMe: generic MSI library support used when allocating MSI vectors for NVMe */
#include <linux/irqdomain.h> /* NVMe: irq_domain creates Linux IRQ numbers from MSI/INTx hwirq for NVMe */
#include <linux/kernel.h> /* NVMe: kernel logging macros */
#include <linux/init.h> /* NVMe: module init helpers */
#include <linux/msi.h> /* NVMe: MSI definitions shared with PCI core and NVMe MSI setup */
#include <linux/of_address.h> /* NVMe: OF address parsing for memory resources that back NVMe config space */
#include <linux/of_pci.h> /* NVMe: OF PCI helpers used during host bridge setup */
#include <linux/of_platform.h> /* NVMe: OF platform device registration */
#include <linux/pci.h> /* NVMe: PCI core header; defines structs and ops used by both host and NVMe driver */
#include <linux/pci-ecam.h> /* NVMe: ECAM offset helpers for PCI config reads issued during NVMe enumeration */
#include <linux/phy/phy.h> /* NVMe: PHY framework for serdes/link training that must succeed before NVMe probing */
#include <linux/platform_device.h> /* NVMe: platform device/driver registration */
#include <linux/irqchip/chained_irq.h> /* NVMe: chained_irq helpers for legacy and MSI cascaded interrupt handling */

#include "../pci.h" /* NVMe: internal PCI host driver header */

/* Bridge core config registers */ /* PCI/NVMe: bridge core register offsets; root port uses these to set up access to NVMe config space */
#define BRCFG_PCIE_RX0			0x00000000 /* NVMe: Bridge RX0 register offset; controls DMA register BAR for PCIe traffic */
#define BRCFG_PCIE_RX1			0x00000004 /* NVMe: Bridge RX1 register offset; used to set coherent cache attribute for NVMe DMA */
#define BRCFG_INTERRUPT			0x00000010 /* NVMe: bridge interrupt register offset */
#define BRCFG_PCIE_RX_MSG_FILTER	0x00000020 /* NVMe: message filter register offset; selects PM/ERR/INT messages relevant to NVMe ASPM and AER */

/* Egress - Bridge translation registers */ /* PCI/NVMe: egress bridge translation register offsets; map bridge/ECAM windows used to reach NVMe BARs and config space */
#define E_BREG_CAPABILITIES		0x00000200 /* NVMe: BREG capabilities offset */
#define E_BREG_CONTROL			0x00000208 /* NVMe: BREG control offset */
#define E_BREG_BASE_LO			0x00000210 /* NVMe: BREG base low offset */
#define E_BREG_BASE_HI			0x00000214 /* NVMe: BREG base high offset */
#define E_ECAM_CAPABILITIES		0x00000220 /* NVMe: ECAM capabilities offset */
#define E_ECAM_CONTROL			0x00000228 /* NVMe: ECAM control offset */
#define E_ECAM_BASE_LO			0x00000230 /* NVMe: ECAM base low offset */
#define E_ECAM_BASE_HI			0x00000234 /* NVMe: ECAM base high offset */

/* Ingress - address translations */ /* PCI/NVMe: ingress address translation register offsets; MSI target window used by NVMe MSI writes */
#define I_MSII_CAPABILITIES		0x00000300 /* NVMe: ingress MSI capabilities offset */
#define I_MSII_CONTROL			0x00000308 /* NVMe: ingress MSI control offset */
#define I_MSII_BASE_LO			0x00000310 /* NVMe: ingress MSI base low offset */
#define I_MSII_BASE_HI			0x00000314 /* NVMe: ingress MSI base high offset */

#define I_ISUB_CONTROL			0x000003E8 /* NVMe: ingress subtractive decode control offset */
#define SET_ISUB_CONTROL		BIT(0) /* NVMe: bit to enable subtractive decode; allows outbound NVMe MMIO and DMA transactions */
/* Rxed msg fifo  - Interrupt status registers */ /* NVMe: message FIFO and interrupt status register block offsets */
#define MSGF_MISC_STATUS		0x00000400 /* NVMe: miscellaneous interrupt status register */
#define MSGF_MISC_MASK			0x00000404 /* NVMe: miscellaneous interrupt mask register */
#define MSGF_LEG_STATUS			0x00000420 /* NVMe: legacy interrupt status register (INTx) */
#define MSGF_LEG_MASK			0x00000424 /* NVMe: legacy interrupt mask register (INTx) */
#define MSGF_MSI_STATUS_LO		0x00000440 /* NVMe: MSI status low register; pending vectors 31:0 */
#define MSGF_MSI_STATUS_HI		0x00000444 /* NVMe: MSI status high register; pending vectors 63:32 */
#define MSGF_MSI_MASK_LO		0x00000448 /* NVMe: MSI mask low register */
#define MSGF_MSI_MASK_HI		0x0000044C /* NVMe: MSI mask high register */

/* Msg filter mask bits */ /* PCI/NVMe: message filter mask bits; controls which PCIe messages are forwarded to the host */
#define CFG_ENABLE_PM_MSG_FWD		BIT(1) /* NVMe: enable forwarding of power-management messages including ASPM */
#define CFG_ENABLE_INT_MSG_FWD		BIT(2) /* NVMe: enable forwarding of interrupt messages */
#define CFG_ENABLE_ERR_MSG_FWD		BIT(3) /* NVMe: enable forwarding of error messages for AER handling */
#define CFG_ENABLE_MSG_FILTER_MASK	(CFG_ENABLE_PM_MSG_FWD |  /* NVMe: combined message filter mask for PM, INT and ERR messages */\
					CFG_ENABLE_INT_MSG_FWD |  /* NVMe: continuation of CFG_ENABLE_MSG_FILTER_MASK */\
					CFG_ENABLE_ERR_MSG_FWD) /* NVMe: end of CFG_ENABLE_MSG_FILTER_MASK */

/* Misc interrupt status mask bits */ /* NVMe: misc interrupt status bit definitions */
#define MSGF_MISC_SR_RXMSG_AVAIL	BIT(0) /* NVMe: received message available status */
#define MSGF_MISC_SR_RXMSG_OVER		BIT(1) /* NVMe: received message FIFO overflow */
#define MSGF_MISC_SR_SLAVE_ERR		BIT(4) /* NVMe: slave error status */
#define MSGF_MISC_SR_MASTER_ERR		BIT(5) /* NVMe: master error status */
#define MSGF_MISC_SR_I_ADDR_ERR		BIT(6) /* NVMe: ingress address translation error */
#define MSGF_MISC_SR_E_ADDR_ERR		BIT(7) /* NVMe: egress address translation error */
#define MSGF_MISC_SR_FATAL_AER		BIT(16) /* NVMe: fatal AER event status */
#define MSGF_MISC_SR_NON_FATAL_AER	BIT(17) /* NVMe: non-fatal AER event status */
#define MSGF_MISC_SR_CORR_AER		BIT(18) /* NVMe: correctable AER event status */
#define MSGF_MISC_SR_UR_DETECT		BIT(20) /* NVMe: unsupported request detected */
#define MSGF_MISC_SR_NON_FATAL_DEV	BIT(22) /* NVMe: non-fatal device error */
#define MSGF_MISC_SR_FATAL_DEV		BIT(23) /* NVMe: fatal device error */
#define MSGF_MISC_SR_LINK_DOWN		BIT(24) /* NVMe: link down event; may affect NVMe device availability */
#define MSGF_MISC_SR_LINK_AUTO_BWIDTH	BIT(25) /* NVMe: autonomous bandwidth change */
#define MSGF_MISC_SR_LINK_BWIDTH	BIT(26) /* NVMe: bandwidth management status */

#define MSGF_MISC_SR_MASKALL		(MSGF_MISC_SR_RXMSG_AVAIL |  /* NVMe: mask covering all misc interrupt sources */\
					MSGF_MISC_SR_RXMSG_OVER |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_SLAVE_ERR |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_MASTER_ERR |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_I_ADDR_ERR |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_E_ADDR_ERR |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_FATAL_AER |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_NON_FATAL_AER |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_CORR_AER |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_UR_DETECT |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_NON_FATAL_DEV |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_FATAL_DEV |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_LINK_DOWN |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_LINK_AUTO_BWIDTH |  /* NVMe: continuation of MSGF_MISC_SR_MASKALL */\
					MSGF_MISC_SR_LINK_BWIDTH) /* NVMe: end of MSGF_MISC_SR_MASKALL */

/* Legacy interrupt status mask bits */ /* NVMe: legacy interrupt status bit definitions (INTx) */
#define MSGF_LEG_SR_INTA		BIT(0) /* NVMe: INTA status */
#define MSGF_LEG_SR_INTB		BIT(1) /* NVMe: INTB status */
#define MSGF_LEG_SR_INTC		BIT(2) /* NVMe: INTC status */
#define MSGF_LEG_SR_INTD		BIT(3) /* NVMe: INTD status */
#define MSGF_LEG_SR_MASKALL		(MSGF_LEG_SR_INTA | MSGF_LEG_SR_INTB |  /* NVMe: mask covering all four legacy INTx lines */\
					MSGF_LEG_SR_INTC | MSGF_LEG_SR_INTD) /* NVMe: end of MSGF_LEG_SR_MASKALL */

/* MSI interrupt status mask bits */ /* NVMe: MSI status register mask definitions */
#define MSGF_MSI_SR_LO_MASK		GENMASK(31, 0) /* NVMe: lower 32-bit MSI status mask */
#define MSGF_MSI_SR_HI_MASK		GENMASK(31, 0) /* NVMe: upper 32-bit MSI status mask */

#define MSII_PRESENT			BIT(0) /* NVMe: MSI ingress capability present bit */
#define MSII_ENABLE			BIT(0) /* NVMe: MSI ingress enable bit */
#define MSII_STATUS_ENABLE		BIT(15) /* NVMe: MSI ingress status enable bit */

/* Bridge config interrupt mask */ /* NVMe: bridge config interrupt mask register */
#define BRCFG_INTERRUPT_MASK		BIT(0) /* NVMe: bridge interrupt mask bit */
#define BREG_PRESENT			BIT(0) /* NVMe: BREG present bit */
#define BREG_ENABLE			BIT(0) /* NVMe: BREG enable bit */
#define BREG_ENABLE_FORCE		BIT(1) /* NVMe: BREG enable force bit */

/* E_ECAM status mask bits */ /* NVMe: ECAM capability and control bit definitions */
#define E_ECAM_PRESENT			BIT(0) /* NVMe: ECAM present bit */
#define E_ECAM_CR_ENABLE		BIT(0) /* NVMe: ECAM control enable bit */
#define E_ECAM_SIZE_LOC			GENMASK(20, 16) /* NVMe: ECAM size field location */
#define E_ECAM_SIZE_SHIFT		16 /* NVMe: ECAM size field shift */
#define NWL_ECAM_MAX_SIZE		16 /* NVMe: maximum ECAM size encoded value */

#define CFG_DMA_REG_BAR			GENMASK(2, 0) /* NVMe: DMA register BAR field mask */
#define CFG_PCIE_CACHE			GENMASK(7, 0) /* NVMe: PCIe cache attribute mask used for coherent NVMe DMA */

#define INT_PCI_MSI_NR			(2 * 32) /* NVMe: total MSI IRQ count supported (64 vectors) for NVMe queues */

/* Readin the PS_LINKUP */ /* NVMe: PHY and link status register offset */
#define PS_LINKUP_OFFSET		0x00000238 /* NVMe: link-up offset within pcireg region */
#define PCIE_PHY_LINKUP_BIT		BIT(0) /* NVMe: PHY link-up bit */
#define PHY_RDY_LINKUP_BIT		BIT(1) /* NVMe: PHY ready link-up bit */

/* Parameters for the waiting for link up routine */ /* NVMe: link-up polling parameters */
#define LINK_WAIT_MAX_RETRIES          10 /* NVMe: maximum retries while waiting for NVMe link */
#define LINK_WAIT_USLEEP_MIN           90000 /* NVMe: minimum sleep between link polls */
#define LINK_WAIT_USLEEP_MAX           100000 /* NVMe: maximum sleep between link polls */

struct nwl_msi {			/* MSI information */ /* NVMe: MSI controller state; holds bitmap and domain used to allocate NVMe MSI vectors */
	DECLARE_BITMAP(bitmap, INT_PCI_MSI_NR); /* NVMe: bitmap of allocated MSI vectors; each bit can back one NVMe queue interrupt */
	struct irq_domain *dev_domain; /* NVMe: per-device irq_domain maps hwirq to Linux virq for NVMe MSI */
	struct mutex lock;		/* protect bitmap variable */ /* NVMe: mutex protects bitmap during NVMe MSI allocate and free */
	int irq_msi0; /* NVMe: hardware IRQ for upper 32 MSI status bits */
	int irq_msi1; /* NVMe: hardware IRQ for lower 32 MSI status bits */
};

struct nwl_pcie { /* PCI/NVMe: per-controller PCIe root-port state; bridge resources are shared with NVMe endpoints */
	struct device *dev; /* NVMe: device pointer used for logging and DMA configuration */
	void __iomem *breg_base; /* NVMe: iomapped bridge register base */
	void __iomem *pcireg_base; /* NVMe: iomapped PCIe controller register base */
	void __iomem *ecam_base; /* NVMe: iomapped ECAM base for NVMe config space access */
	struct phy *phy[4]; /* NVMe: PHY handles used to bring the PCIe link up before NVMe probe */
	phys_addr_t phys_breg_base;	/* Physical Bridge Register Base */ /* NVMe: physical base of bridge registers */
	phys_addr_t phys_pcie_reg_base;	/* Physical PCIe Controller Base */ /* NVMe: physical base of PCIe controller registers */
	phys_addr_t phys_ecam_base;	/* Physical Configuration Base */ /* NVMe: physical base of ECAM region */
	u32 breg_size; /* NVMe: size of bridge register region */
	u32 pcie_reg_size; /* NVMe: size of PCIe controller register region */
	u32 ecam_size; /* NVMe: size of ECAM region */
	int irq_intx; /* NVMe: chained IRQ for legacy INTx interrupts from NVMe */
	int irq_misc; /* NVMe: IRQ for misc events (AER, link down) affecting NVMe */
	struct nwl_msi msi; /* NVMe: embedded MSI controller state */
	struct irq_domain *intx_irq_domain; /* NVMe: irq_domain for legacy INTx lines */
	struct clk *clk; /* NVMe: PCIe reference clock */
	raw_spinlock_t leg_mask_lock; /* NVMe: raw spinlock protects legacy INTx mask register */
}; /* NVMe: end of nwl_pcie structure */

static inline u32 nwl_bridge_readl(struct nwl_pcie *pcie, u32 off) /* NVMe: helper to read a 32-bit bridge register used to inspect root-port status */
{ /* NVMe: perform the readl and return the value */
	return readl(pcie->breg_base + off); /* NVMe: return bridge register value */
}

static inline void nwl_bridge_writel(struct nwl_pcie *pcie, u32 val, u32 off) /* NVMe: helper to write a 32-bit bridge register used to configure root-port behavior */
{
	writel(val, pcie->breg_base + off); /* NVMe: perform the writel */
}

static bool nwl_pcie_link_up(struct nwl_pcie *pcie) /* NVMe: query whether the PCIe PHY/MAC link is up; required before NVMe config access */
{
	if (readl(pcie->pcireg_base + PS_LINKUP_OFFSET) & PCIE_PHY_LINKUP_BIT) /* NVMe: read link status and test the link-up bit */
		return true; /* NVMe: link is up */
	return false; /* NVMe: link is not up */
}

static bool nwl_phy_link_up(struct nwl_pcie *pcie) /* NVMe: query PHY ready status; link training must complete before NVMe enumeration */
{
	if (readl(pcie->pcireg_base + PS_LINKUP_OFFSET) & PHY_RDY_LINKUP_BIT) /* NVMe: read PHY ready bit */
		return true; /* NVMe: PHY is ready */
	return false; /* NVMe: PHY is not ready */
}

static int nwl_wait_for_link(struct nwl_pcie *pcie) /* NVMe: wait for PHY link up; NVMe device will not respond until this succeeds */
{
	struct device *dev = pcie->dev; /* NVMe: cache device pointer for error messages */
	int retries; /* NVMe: retry counter */

	/* check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) { /* NVMe: poll up to LINK_WAIT_MAX_RETRIES for PHY ready */
		if (nwl_phy_link_up(pcie)) /* NVMe: if PHY is ready, link is established for NVMe */
			return 0; /* NVMe: success */
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX); /* NVMe: short sleep before next poll */
	}

	dev_err(dev, "PHY link never came up\n"); /* NVMe: log error when NVMe link fails to come up */
	return -ETIMEDOUT; /* NVMe: return timeout so NVMe enumeration will be aborted */
}

static bool nwl_pcie_valid_device(struct pci_bus *bus, unsigned int devfn) /* PCI/NVMe: decide if a PCI config access target is valid; filters accesses to absent NVMe devices */
{
	struct nwl_pcie *pcie = bus->sysdata; /* NVMe: retrieve host bridge private data from pci_bus */

	/* Check link before accessing downstream ports */
	if (!pci_is_root_bus(bus)) { /* NVMe: for downstream buses, require link up before touching NVMe config space */
		if (!nwl_pcie_link_up(pcie)) /* NVMe: if link down, reject config access so NVMe probe does not hang */
			return false; /* NVMe: invalid downstream access */
	} else if (devfn > 0) /* NVMe: on root bus only device 0 function 0 exists */
		/* Only one device down on each root port */
		return false; /* NVMe: no device at this devfn on root bus */

	return true; /* NVMe: access allowed */
}

/**
 * nwl_pcie_map_bus - Get configuration base
 *
 * @bus: Bus structure of current bus
 * @devfn: Device/function
 * @where: Offset from base
 *
 * Return: Base address of the configuration space needed to be
 *	   accessed.
 */ /* PCI/NVMe: end of kernel-doc; ECAM mapping used for NVMe config reads and writes */
static void __iomem *nwl_pcie_map_bus(struct pci_bus *bus, unsigned int devfn, /* PCI/NVMe: map the ECAM address for a bus/devfn/where tuple used by the PCI core for NVMe enumeration */
				      int where)
{
	struct nwl_pcie *pcie = bus->sysdata; /* NVMe: retrieve host private data */

	if (!nwl_pcie_valid_device(bus, devfn)) /* NVMe: skip invalid device accesses to avoid NVMe probe stalls */
		return NULL; /* NVMe: NULL tells PCI core the access is invalid */

	return pcie->ecam_base + PCIE_ECAM_OFFSET(bus->number, devfn, where); /* NVMe: compute ECAM offset and return virtual base for config access */
}

/* PCIe operations */ /* PCI/NVMe: pci_ops structure used by pci_bus to access NVMe configuration space */
static struct pci_ops nwl_pcie_ops = { /* NVMe: ECAM map_bus callback */
	.map_bus = nwl_pcie_map_bus, /* NVMe: generic config read used when NVMe driver reads BAR, vendor, class code, etc */
	.read  = pci_generic_config_read, /* NVMe: generic config write used when NVMe driver enables MMIO, MSI, bus mastering, etc */
	.write = pci_generic_config_write,
};

static irqreturn_t nwl_pcie_misc_handler(int irq, void *data) /* NVMe: miscellaneous interrupt handler; reports AER/link events that may impact NVMe */
{
	struct nwl_pcie *pcie = data; /* NVMe: retrieve host private data from irq handler argument */
	struct device *dev = pcie->dev; /* NVMe: device pointer for ratelimited logging */
	u32 misc_stat; /* NVMe: local copy of misc status */

	/* Checking for misc interrupts */
	misc_stat = nwl_bridge_readl(pcie, MSGF_MISC_STATUS) & /* NVMe: read misc status and mask to enabled sources */
				     MSGF_MISC_SR_MASKALL;
	if (!misc_stat) /* NVMe: no interrupt pending for this handler */
		return IRQ_NONE; /* NVMe: tell core this interrupt was not from us */

	if (misc_stat & MSGF_MISC_SR_RXMSG_OVER) /* NVMe: log message FIFO overflow */
		dev_err_ratelimited(dev, "Received Message FIFO Overflow\n");

	if (misc_stat & MSGF_MISC_SR_SLAVE_ERR) /* NVMe: log slave error */
		dev_err_ratelimited(dev, "Slave error\n");

	if (misc_stat & MSGF_MISC_SR_MASTER_ERR) /* NVMe: log master error */
		dev_err_ratelimited(dev, "Master error\n");

	if (misc_stat & MSGF_MISC_SR_I_ADDR_ERR) /* NVMe: log ingress address translation error */
		dev_err_ratelimited(dev, "In Misc Ingress address translation error\n");

	if (misc_stat & MSGF_MISC_SR_E_ADDR_ERR) /* NVMe: log egress address translation error */
		dev_err_ratelimited(dev, "In Misc Egress address translation error\n");

	if (misc_stat & MSGF_MISC_SR_FATAL_AER) /* NVMe: log fatal AER; NVMe controller may need reset */
		dev_err_ratelimited(dev, "Fatal Error in AER Capability\n");

	if (misc_stat & MSGF_MISC_SR_NON_FATAL_AER) /* NVMe: log non-fatal AER */
		dev_err_ratelimited(dev, "Non-Fatal Error in AER Capability\n");

	if (misc_stat & MSGF_MISC_SR_CORR_AER) /* NVMe: log correctable AER */
		dev_err_ratelimited(dev, "Correctable Error in AER Capability\n");

	if (misc_stat & MSGF_MISC_SR_UR_DETECT) /* NVMe: log unsupported request */
		dev_err_ratelimited(dev, "Unsupported request Detected\n");

	if (misc_stat & MSGF_MISC_SR_NON_FATAL_DEV) /* NVMe: log non-fatal device error */
		dev_err_ratelimited(dev, "Non-Fatal Error Detected\n");

	if (misc_stat & MSGF_MISC_SR_FATAL_DEV) /* NVMe: log fatal device error */
		dev_err_ratelimited(dev, "Fatal Error Detected\n");

	if (misc_stat & MSGF_MISC_SR_LINK_AUTO_BWIDTH) /* NVMe: log autonomous bandwidth change */
		dev_info(dev, "Link Autonomous Bandwidth Management Status bit set\n");

	if (misc_stat & MSGF_MISC_SR_LINK_BWIDTH) /* NVMe: log bandwidth management change */
		dev_info(dev, "Link Bandwidth Management Status bit set\n");

	/* Clear misc interrupt status */
	nwl_bridge_writel(pcie, misc_stat, MSGF_MISC_STATUS); /* NVMe: write 1 to clear reported misc status bits */

	return IRQ_HANDLED; /* NVMe: interrupt handled */
}

static void nwl_pcie_leg_handler(struct irq_desc *desc) /* NVMe: chained legacy INTx handler; demultiplexes INTA-D from NVMe endpoints */
{
	struct irq_chip *chip = irq_desc_get_chip(desc); /* NVMe: get the parent irq_chip for chained flow */
	struct nwl_pcie *pcie; /* NVMe: host bridge pointer */
	unsigned long status; /* NVMe: pending INTx status bitmap */
	u32 bit; /* NVMe: current INTx bit */

	chained_irq_enter(chip, desc); /* NVMe: enter chained IRQ context */
	pcie = irq_desc_get_handler_data(desc); /* NVMe: obtain handler_data stored during setup */

	while ((status = nwl_bridge_readl(pcie, MSGF_LEG_STATUS) & /* NVMe: loop while any legacy INTx status bit is set */
				MSGF_LEG_SR_MASKALL) != 0) {
		for_each_set_bit(bit, &status, PCI_NUM_INTX) /* NVMe: iterate over pending INTx bits and dispatch to domain */
			generic_handle_domain_irq(pcie->intx_irq_domain, bit); /* NVMe: route to the Linux IRQ number for this INTx line */
	}

	chained_irq_exit(chip, desc); /* NVMe: exit chained IRQ context */
}

static void nwl_pcie_handle_msi_irq(struct nwl_pcie *pcie, u32 status_reg) /* NVMe: process pending MSI status bits and dispatch NVMe queue interrupts */
{
	struct nwl_msi *msi = &pcie->msi; /* NVMe: pointer to MSI controller state */
	unsigned long status; /* NVMe: pending MSI status */
	u32 bit; /* NVMe: current MSI bit */

	while ((status = nwl_bridge_readl(pcie, status_reg)) != 0) { /* NVMe: read pending MSI status for one 32-bit bank */
		for_each_set_bit(bit, &status, 32) { /* NVMe: iterate over set bits; each bit corresponds to one NVMe MSI vector */
			nwl_bridge_writel(pcie, 1 << bit, status_reg); /* NVMe: clear the pending bit by writing 1 */
			generic_handle_domain_irq(msi->dev_domain, bit); /* NVMe: dispatch Linux IRQ for this MSI vector */
		}
	}
}

static void nwl_pcie_msi_handler_high(struct irq_desc *desc) /* NVMe: chained handler for upper 32 MSI vectors (queues 32-63) */
{
	struct irq_chip *chip = irq_desc_get_chip(desc); /* NVMe: get parent irq_chip */
	struct nwl_pcie *pcie = irq_desc_get_handler_data(desc); /* NVMe: obtain handler_data */

	chained_irq_enter(chip, desc); /* NVMe: enter chained IRQ */
	nwl_pcie_handle_msi_irq(pcie, MSGF_MSI_STATUS_HI); /* NVMe: handle high MSI status bank */
	chained_irq_exit(chip, desc);
} /* NVMe: exit chained IRQ */

static void nwl_pcie_msi_handler_low(struct irq_desc *desc) /* NVMe: chained handler for lower 32 MSI vectors (queues 0-31) */
{
	struct irq_chip *chip = irq_desc_get_chip(desc); /* NVMe: get parent irq_chip */
	struct nwl_pcie *pcie = irq_desc_get_handler_data(desc); /* NVMe: obtain handler_data */

	chained_irq_enter(chip, desc); /* NVMe: enter chained IRQ */
	nwl_pcie_handle_msi_irq(pcie, MSGF_MSI_STATUS_LO); /* NVMe: handle low MSI status bank */
	chained_irq_exit(chip, desc);
} /* NVMe: exit chained IRQ */

static void nwl_mask_intx_irq(struct irq_data *data) /* NVMe: mask a legacy INTx IRQ when the NVMe driver disables it */
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data); /* NVMe: retrieve host private data from irq_data */
	unsigned long flags; /* NVMe: interrupt save flags */
	u32 mask; /* NVMe: bit mask for this INTx line */
	u32 val; /* NVMe: current mask register value */

	mask = 1 << data->hwirq; /* NVMe: compute mask bit from hardware IRQ number */
	raw_spin_lock_irqsave(&pcie->leg_mask_lock, flags); /* NVMe: protect mask register with raw spinlock */
	val = nwl_bridge_readl(pcie, MSGF_LEG_MASK); /* NVMe: read current legacy mask */
	nwl_bridge_writel(pcie, (val & (~mask)), MSGF_LEG_MASK); /* NVMe: clear bit to mask the interrupt for this NVMe INTx */
	raw_spin_unlock_irqrestore(&pcie->leg_mask_lock, flags); /* NVMe: release spinlock */
}

static void nwl_unmask_intx_irq(struct irq_data *data) /* NVMe: unmask a legacy INTx IRQ when the NVMe driver enables it */
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data); /* NVMe: retrieve host private data */
	unsigned long flags; /* NVMe: interrupt save flags */
	u32 mask; /* NVMe: mask bit for this INTx */
	u32 val; /* NVMe: current mask value */

	mask = 1 << data->hwirq; /* NVMe: compute mask bit */
	raw_spin_lock_irqsave(&pcie->leg_mask_lock, flags); /* NVMe: protect mask register */
	val = nwl_bridge_readl(pcie, MSGF_LEG_MASK); /* NVMe: read current mask */
	nwl_bridge_writel(pcie, (val | mask), MSGF_LEG_MASK); /* NVMe: set bit to unmask the interrupt for this NVMe INTx */
	raw_spin_unlock_irqrestore(&pcie->leg_mask_lock, flags); /* NVMe: release spinlock */
}

static struct irq_chip nwl_intx_irq_chip = { /* NVMe: irq_chip for legacy INTx lines exposed to NVMe */
	.name = "nwl_pcie:legacy", /* NVMe: chip name shown in /proc/interrupts */
	.irq_enable = nwl_unmask_intx_irq, /* NVMe: enabling an IRQ unmasks it */
	.irq_disable = nwl_mask_intx_irq, /* NVMe: disabling an IRQ masks it */
	.irq_mask = nwl_mask_intx_irq, /* NVMe: mask callback */
	.irq_unmask = nwl_unmask_intx_irq, /* NVMe: unmask callback */
};

static int nwl_intx_map(struct irq_domain *domain, unsigned int irq, /* NVMe: map a legacy INTx hwirq to a Linux virq for NVMe */
			irq_hw_number_t hwirq)
{ /* NVMe: assign level handler and chip to the Linux IRQ */
	irq_set_chip_and_handler(irq, &nwl_intx_irq_chip, handle_level_irq); /* NVMe: store host private data in irq_data */
	irq_set_chip_data(irq, domain->host_data); /* NVMe: mark IRQ as level triggered */
	irq_set_status_flags(irq, IRQ_LEVEL);
 /* NVMe: success */
	return 0;
}

static const struct irq_domain_ops intx_domain_ops = { /* NVMe: irq_domain ops for legacy INTx */
	.map = nwl_intx_map, /* NVMe: map callback */
	.xlate = pci_irqd_intx_xlate, /* NVMe: translate INTA-D for PCI devices */
};

#ifdef CONFIG_PCI_MSI /* NVMe: compile MSI support only when CONFIG_PCI_MSI is enabled */

#define NWL_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	|  /* NVMe: required MSI domain and chip flags for this controller */\
				MSI_FLAG_USE_DEF_CHIP_OPS	|  /* NVMe: continuation of NWL_MSI_FLAGS_REQUIRED */\
				MSI_FLAG_NO_AFFINITY) /* NVMe: end of NWL_MSI_FLAGS_REQUIRED */

#define NWL_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		|  /* NVMe: supported MSI flags; multi-MSI allows several NVMe queue vectors */\
				 MSI_FLAG_MULTI_PCI_MSI) /* NVMe: end of NWL_MSI_FLAGS_SUPPORTED */

static const struct msi_parent_ops nwl_msi_parent_ops = { /* NVMe: MSI parent operations registered with PCI/MSI core */
	.required_flags		= NWL_MSI_FLAGS_REQUIRED, /* NVMe: flags that must be set by callers */
	.supported_flags	= NWL_MSI_FLAGS_SUPPORTED, /* NVMe: flags this controller supports */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI, /* NVMe: select PCI_MSI bus token */
	.prefix			= "nwl-", /* NVMe: IRQ name prefix */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info, /* NVMe: use generic MSI library initialization */
};

#endif /* NVMe: end of CONFIG_PCI_MSI MSI data */

static void nwl_compose_msi_msg(struct irq_data *data, struct msi_msg *msg) /* NVMe: compose the MSI message (address and data) that NVMe writes to trigger an interrupt */
{
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data); /* NVMe: retrieve host private data */
	phys_addr_t msi_addr = pcie->phys_pcie_reg_base; /* NVMe: MSI target address is the PCIe controller register base */

	msg->address_lo = lower_32_bits(msi_addr); /* NVMe: lower 32 bits of MSI write address */
	msg->address_hi = upper_32_bits(msi_addr); /* NVMe: upper 32 bits of MSI write address */
	msg->data = data->hwirq; /* NVMe: MSI data payload is the hwirq/vector number */
}

static struct irq_chip nwl_irq_chip = { /* NVMe: irq_chip for MSI vectors allocated to NVMe queues */
	.name = "Xilinx MSI", /* NVMe: chip name shown in /proc/interrupts */
	.irq_compose_msi_msg = nwl_compose_msi_msg, /* NVMe: compose MSI message callback */
};

static int nwl_irq_domain_alloc(struct irq_domain *domain, unsigned int virq, /* NVMe: allocate one or more contiguous MSI vectors for NVMe MSI setup */
				unsigned int nr_irqs, void *args)
{
	struct nwl_pcie *pcie = domain->host_data; /* NVMe: retrieve host private data */
	struct nwl_msi *msi = &pcie->msi; /* NVMe: pointer to MSI state */
	int bit; /* NVMe: first free bit */
	int i; /* NVMe: loop index */

	mutex_lock(&msi->lock); /* NVMe: take MSI bitmap lock */
	bit = bitmap_find_free_region(msi->bitmap, INT_PCI_MSI_NR, /* NVMe: find a contiguous free region for the requested vector count */
				      get_count_order(nr_irqs));
	if (bit < 0) { /* NVMe: no free vectors for NVMe */
		mutex_unlock(&msi->lock); /* NVMe: release lock before error return */
		return -ENOSPC; /* NVMe: report no space */
	}

	for (i = 0; i < nr_irqs; i++) { /* NVMe: bind each allocated Linux IRQ to the MSI chip */
		irq_domain_set_info(domain, virq + i, bit + i, &nwl_irq_chip, /* NVMe: set domain info for virq */
				    domain->host_data, handle_simple_irq, /* NVMe: use simple IRQ handler for MSI */
				    NULL, NULL); /* NVMe: no private per-irq data */
	}
	mutex_unlock(&msi->lock); /* NVMe: release MSI bitmap lock */
	return 0; /* NVMe: success */
}

static void nwl_irq_domain_free(struct irq_domain *domain, unsigned int virq, /* NVMe: free previously allocated MSI vectors when NVMe teardown occurs */
				unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq); /* NVMe: get irq_data for the first virq */
	struct nwl_pcie *pcie = irq_data_get_irq_chip_data(data); /* NVMe: retrieve host private data */
	struct nwl_msi *msi = &pcie->msi; /* NVMe: pointer to MSI state */

	mutex_lock(&msi->lock); /* NVMe: take MSI bitmap lock */
	bitmap_release_region(msi->bitmap, data->hwirq, /* NVMe: release the bit region */
			      get_count_order(nr_irqs));
	mutex_unlock(&msi->lock); /* NVMe: release lock */
}

static const struct irq_domain_ops dev_msi_domain_ops = { /* NVMe: irq_domain ops for MSI device domain */
	.alloc  = nwl_irq_domain_alloc, /* NVMe: allocate callback */
	.free   = nwl_irq_domain_free, /* NVMe: free callback */
};

static int nwl_pcie_init_msi_irq_domain(struct nwl_pcie *pcie) /* NVMe: create the MSI parent irq_domain used by the PCI core for NVMe MSI allocation */
{
#ifdef CONFIG_PCI_MSI
	struct device *dev = pcie->dev; /* NVMe: device pointer */
	struct nwl_msi *msi = &pcie->msi; /* NVMe: pointer to MSI state */
	struct irq_domain_info info = { /* NVMe: irq_domain_info template for MSI domain */
		.fwnode		= dev_fwnode(dev), /* NVMe: firmware node for MSI domain */
		.ops		= &dev_msi_domain_ops, /* NVMe: domain operations */
		.host_data	= pcie, /* NVMe: host private data passed to alloc and free */
		.size		= INT_PCI_MSI_NR, /* NVMe: domain size equals number of supported MSI vectors */
	};

	msi->dev_domain  = msi_create_parent_irq_domain(&info, &nwl_msi_parent_ops); /* NVMe: create parent MSI domain; failures prevent NVMe MSI usage */
	if (!msi->dev_domain) { /* NVMe: domain creation failed */
		dev_err(dev, "failed to create dev IRQ domain\n"); /* NVMe: log error */
		return -ENOMEM; /* NVMe: return no memory */
	}
#endif
	return 0; /* NVMe: success (or MSI disabled at compile time) */
}

static void nwl_pcie_phy_power_off(struct nwl_pcie *pcie, int i) /* NVMe: power off one PHY during teardown */
{
	int err = phy_power_off(pcie->phy[i]); /* NVMe: attempt PHY power off */

	if (err) /* NVMe: log PHY power-off error */
		dev_err(pcie->dev, "could not power off phy %d (err=%d)\n", i, /* NVMe: error message with PHY index */
			err);
}

static void nwl_pcie_phy_exit(struct nwl_pcie *pcie, int i) /* NVMe: exit one PHY during teardown */
{
	int err = phy_exit(pcie->phy[i]); /* NVMe: attempt PHY exit */

	if (err) /* NVMe: log PHY exit error */
		dev_err(pcie->dev, "could not exit phy %d (err=%d)\n", i, err); /* NVMe: error message with PHY index */
}

static int nwl_pcie_phy_enable(struct nwl_pcie *pcie) /* NVMe: initialize and power on all PHYs needed for NVMe link */
{
	int i, ret; /* NVMe: PHY index and return value */

	for (i = 0; i < ARRAY_SIZE(pcie->phy); i++) { /* NVMe: loop over up to four PHYs */
		ret = phy_init(pcie->phy[i]); /* NVMe: initialize PHY */
		if (ret) /* NVMe: if init fails begin rollback */
			goto err; /* NVMe: continue to next PHY */

		ret = phy_power_on(pcie->phy[i]); /* NVMe: power on PHY */
		if (ret) { /* NVMe: if power-on fails, exit this PHY and rollback */
			nwl_pcie_phy_exit(pcie, i); /* NVMe: exit PHY before rollback */
			goto err; /* NVMe: jump to rollback path */
		}
	}

	return 0; /* NVMe: all PHYs enabled successfully */

err: /* NVMe: rollback label */
	while (i--) { /* NVMe: reverse loop to disable previously enabled PHYs */
		nwl_pcie_phy_power_off(pcie, i); /* NVMe: power off PHY i */
		nwl_pcie_phy_exit(pcie, i); /* NVMe: exit PHY i */
	}

	return ret; /* NVMe: return first error */
}

static void nwl_pcie_phy_disable(struct nwl_pcie *pcie) /* NVMe: disable all PHYs when removing the controller */
{
	int i; /* NVMe: PHY index */

	for (i = ARRAY_SIZE(pcie->phy); i--;) { /* NVMe: iterate PHYs in reverse */
		nwl_pcie_phy_power_off(pcie, i); /* NVMe: power off PHY i */
		nwl_pcie_phy_exit(pcie, i); /* NVMe: exit PHY i */
	}
}

static int nwl_pcie_init_irq_domain(struct nwl_pcie *pcie) /* NVMe: create legacy INTx and MSI irq_domains for NVMe interrupts */
{
	struct device *dev = pcie->dev; /* NVMe: device pointer */
	struct device_node *node = dev->of_node; /* NVMe: device node for finding intc child */
	struct device_node *intc_node; /* NVMe: child node for legacy interrupt controller */

	intc_node = of_get_next_child(node, NULL); /* NVMe: get first child node of the PCIe controller */
	if (!intc_node) { /* NVMe: missing interrupt controller node */
		dev_err(dev, "No legacy intc node found\n"); /* NVMe: log error */
		return -EINVAL; /* NVMe: invalid device tree */
	}

	pcie->intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(intc_node), PCI_NUM_INTX, /* NVMe: create a linear irq_domain for four INTx lines */
							 &intx_domain_ops, pcie); /* NVMe: pass host private data as host_data */
	of_node_put(intc_node); /* NVMe: drop reference to child node */
	if (!pcie->intx_irq_domain) { /* NVMe: domain creation failed */
		dev_err(dev, "failed to create IRQ domain\n"); /* NVMe: log error */
		return -ENOMEM; /* NVMe: return no memory */
	}

	raw_spin_lock_init(&pcie->leg_mask_lock); /* NVMe: initialize legacy mask spinlock */
	nwl_pcie_init_msi_irq_domain(pcie); /* NVMe: create MSI domain (NOP if MSI disabled) */
	return 0; /* NVMe: success */
}

static int nwl_pcie_enable_msi(struct nwl_pcie *pcie) /* NVMe: enable MSI interrupt controller so NVMe can use MSI vectors */
{
	struct device *dev = pcie->dev; /* NVMe: device pointer */
	struct platform_device *pdev = to_platform_device(dev); /* NVMe: platform device pointer */
	struct nwl_msi *msi = &pcie->msi; /* NVMe: pointer to MSI state */
	unsigned long base; /* NVMe: physical base for MSI target window */
	int ret; /* NVMe: return value */

	mutex_init(&msi->lock); /* NVMe: initialize MSI bitmap mutex */

	/* Get msi_1 IRQ number */
	msi->irq_msi1 = platform_get_irq_byname(pdev, "msi1"); /* NVMe: obtain platform IRQ named msi1 for high MSI bank */
	if (msi->irq_msi1 < 0) /* NVMe: invalid IRQ prevents MSI usage */
		return -EINVAL; /* NVMe: report invalid parameter */

	irq_set_chained_handler_and_data(msi->irq_msi1, /* NVMe: chain msi1 to high MSI handler */
					 nwl_pcie_msi_handler_high, pcie);

	/* Get msi_0 IRQ number */
	msi->irq_msi0 = platform_get_irq_byname(pdev, "msi0"); /* NVMe: obtain platform IRQ named msi0 for low MSI bank */
	if (msi->irq_msi0 < 0) /* NVMe: invalid IRQ prevents MSI usage */
		return -EINVAL; /* NVMe: report invalid parameter */

	irq_set_chained_handler_and_data(msi->irq_msi0, /* NVMe: chain msi0 to low MSI handler */
					 nwl_pcie_msi_handler_low, pcie);

	/* Check for msii_present bit */
	ret = nwl_bridge_readl(pcie, I_MSII_CAPABILITIES) & MSII_PRESENT; /* NVMe: check that MSI ingress hardware is present */
	if (!ret) { /* NVMe: MSI capability not available */
		dev_err(dev, "MSI not present\n"); /* NVMe: log error */
		return -EIO; /* NVMe: return I/O error */
	}

	/* Enable MSII */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, I_MSII_CONTROL) | /* NVMe: set MSI ingress enable bit */
			  MSII_ENABLE, I_MSII_CONTROL);

	/* Enable MSII status */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, I_MSII_CONTROL) | /* NVMe: set MSI ingress status-enable bit */
			  MSII_STATUS_ENABLE, I_MSII_CONTROL);

	/* setup AFI/FPCI range */
	base = pcie->phys_pcie_reg_base; /* NVMe: MSI writes target the PCIe controller base */
	nwl_bridge_writel(pcie, lower_32_bits(base), I_MSII_BASE_LO); /* NVMe: program low 32 bits of MSI target address */
	nwl_bridge_writel(pcie, upper_32_bits(base), I_MSII_BASE_HI); /* NVMe: program high 32 bits of MSI target address */

	/*
	 * For high range MSI interrupts: disable, clear any pending,
	 * and enable
	 */ /* NVMe: end of high-range MSI init comment */
	nwl_bridge_writel(pcie, 0, MSGF_MSI_MASK_HI); /* NVMe: disable high-range MSI mask */

	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie,  MSGF_MSI_STATUS_HI) & /* NVMe: clear any pending high-range MSI status */
			  MSGF_MSI_SR_HI_MASK, MSGF_MSI_STATUS_HI); /* NVMe: mask value for high bank */

	nwl_bridge_writel(pcie, MSGF_MSI_SR_HI_MASK, MSGF_MSI_MASK_HI); /* NVMe: re-enable all high-range MSI vectors */

	/*
	 * For low range MSI interrupts: disable, clear any pending,
	 * and enable
	 */ /* NVMe: end of low-range MSI init comment */
	nwl_bridge_writel(pcie, 0, MSGF_MSI_MASK_LO); /* NVMe: disable low-range MSI mask */

	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_MSI_STATUS_LO) & /* NVMe: clear any pending low-range MSI status */
			  MSGF_MSI_SR_LO_MASK, MSGF_MSI_STATUS_LO); /* NVMe: mask value for low bank */

	nwl_bridge_writel(pcie, MSGF_MSI_SR_LO_MASK, MSGF_MSI_MASK_LO); /* NVMe: re-enable all low-range MSI vectors */

	return 0; /* NVMe: MSI controller ready for NVMe MSI allocation */
}

static int nwl_pcie_bridge_init(struct nwl_pcie *pcie) /* PCI/NVMe: initialize bridge egress, ECAM, link, and interrupts for NVMe */
{
	struct device *dev = pcie->dev; /* NVMe: device pointer */
	struct platform_device *pdev = to_platform_device(dev); /* NVMe: platform device pointer */
	u32 breg_val, ecam_val; /* NVMe: local copies of BREG and ECAM capability values */
	int err; /* NVMe: return value */

	breg_val = nwl_bridge_readl(pcie, E_BREG_CAPABILITIES) & BREG_PRESENT; /* NVMe: check bridge register egress is present */
	if (!breg_val) { /* NVMe: BREG missing */
		dev_err(dev, "BREG is not present\n"); /* NVMe: log error */
		return breg_val; /* NVMe: return capability value (zero) as error */
	}

	/* Write bridge_off to breg base */
	nwl_bridge_writel(pcie, lower_32_bits(pcie->phys_breg_base), /* NVMe: write low 32 bits of BREG physical base */
			  E_BREG_BASE_LO); /* NVMe: write high 32 bits of BREG physical base */
	nwl_bridge_writel(pcie, upper_32_bits(pcie->phys_breg_base),
			  E_BREG_BASE_HI);

	/* Enable BREG */
	nwl_bridge_writel(pcie, ~BREG_ENABLE_FORCE & BREG_ENABLE, /* NVMe: enable BREG without forcing */
			  E_BREG_CONTROL);

	/* Disable DMA channel registers */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_PCIE_RX0) | /* NVMe: disable DMA channel register BAR */
			  CFG_DMA_REG_BAR, BRCFG_PCIE_RX0);

	/* Enable Ingress subtractive decode translation */
	nwl_bridge_writel(pcie, SET_ISUB_CONTROL, I_ISUB_CONTROL); /* NVMe: enable subtractive decode for outbound NVMe transactions */

	/* Enable msg filtering details */
	nwl_bridge_writel(pcie, CFG_ENABLE_MSG_FILTER_MASK, /* NVMe: enable PCIe message filtering for PM, INT and ERR */
			  BRCFG_PCIE_RX_MSG_FILTER);

	/* This routes the PCIe DMA traffic to go through CCI path */
	if (of_dma_is_coherent(dev->of_node)) /* NVMe: if the DT marks DMA as coherent, set cache attributes for NVMe DMA */
		nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_PCIE_RX1) | /* NVMe: read RX1 and set cache bits */
				  CFG_PCIE_CACHE, BRCFG_PCIE_RX1); /* NVMe: write back to enable snooping or coherency for NVMe traffic */

	err = nwl_wait_for_link(pcie); /* NVMe: wait for PHY and link up before accessing NVMe */
	if (err) /* NVMe: if link fails, abort bridge init */
		return err;

	ecam_val = nwl_bridge_readl(pcie, E_ECAM_CAPABILITIES) & E_ECAM_PRESENT; /* NVMe: check ECAM capability present */
	if (!ecam_val) { /* NVMe: ECAM missing */
		dev_err(dev, "ECAM is not present\n"); /* NVMe: log error */
		return ecam_val; /* NVMe: return zero value as error */
	}

	/* Enable ECAM */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, E_ECAM_CONTROL) | /* NVMe: set ECAM enable bit */
			  E_ECAM_CR_ENABLE, E_ECAM_CONTROL);

	ecam_val = nwl_bridge_readl(pcie, E_ECAM_CONTROL); /* NVMe: re-read ECAM control to program size */
	ecam_val &= ~E_ECAM_SIZE_LOC; /* NVMe: clear size field */
	ecam_val |= NWL_ECAM_MAX_SIZE << E_ECAM_SIZE_SHIFT; /* NVMe: program maximum ECAM size */
	nwl_bridge_writel(pcie, ecam_val, E_ECAM_CONTROL); /* NVMe: write updated ECAM control */

	nwl_bridge_writel(pcie, lower_32_bits(pcie->phys_ecam_base), /* NVMe: write low 32 bits of ECAM physical base */
			  E_ECAM_BASE_LO); /* NVMe: write high 32 bits of ECAM physical base */
	nwl_bridge_writel(pcie, upper_32_bits(pcie->phys_ecam_base),
			  E_ECAM_BASE_HI);

	if (nwl_pcie_link_up(pcie)) /* NVMe: report link status to dmesg */
		dev_info(dev, "Link is UP\n"); /* NVMe: link up message */
	else /* NVMe: link not up path */
		dev_info(dev, "Link is DOWN\n"); /* NVMe: link down message */

	/* Get misc IRQ number */
	pcie->irq_misc = platform_get_irq_byname(pdev, "misc"); /* NVMe: obtain platform IRQ named misc */
	if (pcie->irq_misc < 0) /* NVMe: invalid misc IRQ */
		return -EINVAL; /* NVMe: report invalid parameter */

	err = devm_request_irq(dev, pcie->irq_misc, /* NVMe: register misc IRQ handler */
			       nwl_pcie_misc_handler, IRQF_SHARED, /* NVMe: handler function and shared flag */
			       "nwl_pcie:misc", pcie); /* NVMe: IRQ name and host private data */
	if (err) { /* NVMe: if registration failed */
		dev_err(dev, "fail to register misc IRQ#%d\n", /* NVMe: log error with IRQ number */
			pcie->irq_misc); /* NVMe: return error */
		return err;
	}

	/* Disable all misc interrupts */
	nwl_bridge_writel(pcie, (u32)~MSGF_MISC_SR_MASKALL, MSGF_MISC_MASK); /* NVMe: disable all misc interrupts by writing inverted mask */

	/* Clear pending misc interrupts */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_MISC_STATUS) & /* NVMe: clear pending misc status bits */
			  MSGF_MISC_SR_MASKALL, MSGF_MISC_STATUS); /* NVMe: mask value */

	/* Enable all misc interrupts */
	nwl_bridge_writel(pcie, MSGF_MISC_SR_MASKALL, MSGF_MISC_MASK); /* NVMe: unmask all misc interrupts */

	/* Disable all INTX interrupts */
	nwl_bridge_writel(pcie, (u32)~MSGF_LEG_SR_MASKALL, MSGF_LEG_MASK); /* NVMe: disable all legacy INTx interrupts */

	/* Clear pending INTX interrupts */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, MSGF_LEG_STATUS) & /* NVMe: clear pending legacy INTx status */
			  MSGF_LEG_SR_MASKALL, MSGF_LEG_STATUS); /* NVMe: mask value */

	/* Enable all INTX interrupts */
	nwl_bridge_writel(pcie, MSGF_LEG_SR_MASKALL, MSGF_LEG_MASK); /* NVMe: enable all legacy INTx interrupts */

	/* Enable the bridge config interrupt */
	nwl_bridge_writel(pcie, nwl_bridge_readl(pcie, BRCFG_INTERRUPT) | /* NVMe: enable bridge config interrupt */
			  BRCFG_INTERRUPT_MASK, BRCFG_INTERRUPT); /* NVMe: OR with existing value */

	return 0; /* NVMe: bridge initialization complete */
}

static int nwl_pcie_parse_dt(struct nwl_pcie *pcie, /* PCI/NVMe: parse device tree resources and IRQs needed for NVMe host operation */
			     struct platform_device *pdev)
{
	struct device *dev = pcie->dev; /* NVMe: device pointer */
	struct resource *res; /* NVMe: resource pointer for iomapping */
	int i; /* NVMe: loop index for PHYs */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "breg"); /* NVMe: get breg memory resource */
	pcie->breg_base = devm_ioremap_resource(dev, res); /* NVMe: iomap bridge registers */
	if (IS_ERR(pcie->breg_base)) /* NVMe: check iomap error */
		return PTR_ERR(pcie->breg_base); /* NVMe: propagate error */
	pcie->phys_breg_base = res->start; /* NVMe: save physical base of BREG */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcireg"); /* NVMe: get pcireg memory resource */
	pcie->pcireg_base = devm_ioremap_resource(dev, res); /* NVMe: iomap PCIe controller registers */
	if (IS_ERR(pcie->pcireg_base)) /* NVMe: check iomap error */
		return PTR_ERR(pcie->pcireg_base); /* NVMe: propagate error */
	pcie->phys_pcie_reg_base = res->start; /* NVMe: save physical base of PCIe registers */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg"); /* NVMe: get cfg ECAM memory resource */
	pcie->ecam_base = devm_pci_remap_cfg_resource(dev, res); /* NVMe: iomap ECAM using PCI config remap helper */
	if (IS_ERR(pcie->ecam_base)) /* NVMe: check remap error */
		return PTR_ERR(pcie->ecam_base); /* NVMe: propagate error */
	pcie->phys_ecam_base = res->start; /* NVMe: save physical base of ECAM */

	/* Get intx IRQ number */
	pcie->irq_intx = platform_get_irq_byname(pdev, "intx"); /* NVMe: obtain platform IRQ named intx */
	if (pcie->irq_intx < 0) /* NVMe: invalid intx IRQ */
		return pcie->irq_intx; /* NVMe: propagate error */

	irq_set_chained_handler_and_data(pcie->irq_intx, /* NVMe: chain intx IRQ to legacy handler */
					 nwl_pcie_leg_handler, pcie); /* NVMe: pass host private data */


	for (i = 0; i < ARRAY_SIZE(pcie->phy); i++) { /* NVMe: loop over PHY array */
		pcie->phy[i] = devm_of_phy_get_by_index(dev, dev->of_node, i); /* NVMe: request PHY by index from device tree */
		if (PTR_ERR(pcie->phy[i]) == -ENODEV) { /* NVMe: if PHY is absent, stop scanning */
			pcie->phy[i] = NULL; /* NVMe: mark slot as NULL */
			break; /* NVMe: break out of PHY loop */
		}

		if (IS_ERR(pcie->phy[i])) /* NVMe: other PHY probe errors are fatal */
			return PTR_ERR(pcie->phy[i]); /* NVMe: propagate PHY error */
	}

	return 0; /* NVMe: device tree parsing succeeded */
}

static const struct of_device_id nwl_pcie_of_match[] = { /* NVMe: OF match table; compatible string binds this driver to the NWL PCIe controller */
	{ .compatible = "xlnx,nwl-pcie-2.11", }, /* NVMe: compatible string for NWL PCIe 2.11 */
	{} /* NVMe: sentinel */
};

static int nwl_pcie_probe(struct platform_device *pdev) /* PCI/NVMe: probe routine; creates the host bridge that the NVMe driver will later bind to */
{
	struct device *dev = &pdev->dev; /* NVMe: device pointer from platform_device */
	struct nwl_pcie *pcie; /* NVMe: host bridge private data pointer */
	struct pci_host_bridge *bridge; /* NVMe: PCI host bridge structure */
	int err; /* NVMe: error value */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie)); /* NVMe: allocate host bridge with embedded nwl_pcie private data */
	if (!bridge) /* NVMe: allocation failed */
		return -ENODEV; /* NVMe: return no device */

	pcie = pci_host_bridge_priv(bridge); /* NVMe: get pointer to embedded private data */
	platform_set_drvdata(pdev, pcie); /* NVMe: store private data for remove and suspend or resume */

	pcie->dev = dev; /* NVMe: initialize device pointer */

	err = nwl_pcie_parse_dt(pcie, pdev); /* NVMe: parse memory, IRQ and PHY resources from DT */
	if (err) { /* NVMe: if DT parsing fails */
		dev_err(dev, "Parsing DT failed\n"); /* NVMe: log error */
		return err; /* NVMe: return error */
	}

	pcie->clk = devm_clk_get(dev, NULL); /* NVMe: get PCIe reference clock */
	if (IS_ERR(pcie->clk)) /* NVMe: clock get failed */
		return PTR_ERR(pcie->clk); /* NVMe: return error */

	err = clk_prepare_enable(pcie->clk); /* NVMe: prepare and enable PCIe reference clock */
	if (err) { /* NVMe: clock enable failed */
		dev_err(dev, "can't enable PCIe ref clock\n"); /* NVMe: log error */
		return err; /* NVMe: return error */
	}

	err = nwl_pcie_phy_enable(pcie); /* NVMe: initialize and power on PHYs */
	if (err) { /* NVMe: PHY enable failed */
		dev_err(dev, "could not enable PHYs\n"); /* NVMe: log error */
		goto err_clk; /* NVMe: jump to clock disable */
	}

	err = nwl_pcie_bridge_init(pcie); /* NVMe: initialize bridge, link, and interrupts */
	if (err) { /* NVMe: bridge init failed */
		dev_err(dev, "HW Initialization failed\n"); /* NVMe: log error */
		goto err_phy; /* NVMe: jump to PHY disable */
	}

	err = nwl_pcie_init_irq_domain(pcie); /* NVMe: create INTx and MSI IRQ domains */
	if (err) { /* NVMe: IRQ domain creation failed */
		dev_err(dev, "Failed creating IRQ Domain\n"); /* NVMe: log error */
		goto err_phy; /* NVMe: jump to PHY disable */
	}

	bridge->sysdata = pcie; /* NVMe: attach host private data to bridge */
	bridge->ops = &nwl_pcie_ops; /* NVMe: assign PCI config ops used to enumerate NVMe */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* NVMe: if MSI support is compiled in */
		err = nwl_pcie_enable_msi(pcie); /* NVMe: enable MSI hardware and domain */
		if (err < 0) { /* NVMe: MSI enable failed */
			dev_err(dev, "failed to enable MSI support: %d\n", err); /* NVMe: log error */
			goto err_phy; /* NVMe: jump to PHY disable */
		}
	}

	err = pci_host_probe(bridge); /* NVMe: probe the PCI host bus; this enumerates downstream devices including NVMe SSDs */
	if (!err) /* NVMe: host probe succeeded */
		return 0; /* NVMe: return success */

err_phy: /* NVMe: error path label for PHY disable */
	nwl_pcie_phy_disable(pcie); /* NVMe: power off PHYs */
err_clk: /* NVMe: error path label for clock disable */
	clk_disable_unprepare(pcie->clk); /* NVMe: disable PCIe reference clock */
	return err; /* NVMe: return probe error */
}

static void nwl_pcie_remove(struct platform_device *pdev) /* NVMe: remove routine; tears down resources when the host bridge is unbound */
{
	struct nwl_pcie *pcie = platform_get_drvdata(pdev); /* NVMe: retrieve private data stored in probe */

	nwl_pcie_phy_disable(pcie); /* NVMe: disable PHYs */
	clk_disable_unprepare(pcie->clk); /* NVMe: disable reference clock */
}

static struct platform_driver nwl_pcie_driver = { /* NVMe: platform driver structure */
	.driver = { /* NVMe: driver name */
		.name = "nwl-pcie", /* NVMe: suppress manual bind and unbind attributes */
		.suppress_bind_attrs = true, /* NVMe: OF match table for device tree probing */
		.of_match_table = nwl_pcie_of_match,
	}, /* NVMe: probe callback */
	.probe = nwl_pcie_probe, /* NVMe: remove callback */
	.remove = nwl_pcie_remove,
}; /* NVMe: register platform driver at init */
builtin_platform_driver(nwl_pcie_driver);
