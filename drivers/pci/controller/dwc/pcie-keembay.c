// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe controller driver for Intel Keem Bay
 * Copyright (C) 2020 Intel Corporation
 */

#include <linux/bitfield.h>	/* PCI/NVMe: field manipulation for PCIe controller registers */
#include <linux/bits.h>		/* PCI/NVMe: BIT/GENMASK helpers for register bit definitions */
#include <linux/clk.h>		/* PCI/NVMe: clock control for PCIe REFCLK used by NVMe SSD */
#include <linux/delay.h>	/* PCI/NVMe: PERST/link training delays before NVMe enumeration */
#include <linux/err.h>		/* PCI/NVMe: ERR_PTR error propagation during probe */
#include <linux/gpio/consumer.h>	/* PCI/NVMe: PERST# GPIO to reset downstream NVMe device */
#include <linux/init.h>
#include <linux/iopoll.h>	/* PCI/NVMe: poll for PLL/PHY lock before scanning NVMe bus */
#include <linux/irqchip/chained_irq.h>	/* PCI/NVMe: chain MSI controller IRQ to GIC for NVMe queues */
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/pci.h>		/* PCI/NVMe: PCI core types; NVMe binds to enumerated PCI functions */
#include <linux/platform_device.h>
#include <linux/property.h>

#include "pcie-designware.h"	/* PCI/NVMe: DesignWare core helpers shared with NVMe host bridge setup */

/* PCIE_REGS_APB_SLV Registers */
#define PCIE_REGS_PCIE_CFG		0x0004	/* PCI/NVMe: config reg; sets RC mode and RSTN for downstream NVMe */
#define  PCIE_DEVICE_TYPE		BIT(8)	/* PCI/NVMe: 1 = Root Complex mode to enumerate NVMe SSD */
#define  PCIE_RSTN			BIT(0)	/* PCI/NVMe: controller internal reset; released before NVMe link up */
#define PCIE_REGS_PCIE_APP_CNTRL	0x0008	/* PCI/NVMe: application control; LTSSM enable bit lives here */
#define  APP_LTSSM_ENABLE		BIT(0)	/* PCI/NVMe: starts Link Training so NVMe SSD can appear on bus */
#define PCIE_REGS_INTERRUPT_ENABLE	0x0028	/* PCI/NVMe: enables MSI/EDMA interrupts from NVMe SSD */
#define  MSI_CTRL_INT_EN		BIT(8)	/* PCI/NVMe: enable MSI message detection from NVMe endpoints */
#define  EDMA_INT_EN			GENMASK(7, 0)	/* PCI/NVMe: enable eDMA done/error events (non-NVMe path) */
#define PCIE_REGS_INTERRUPT_STATUS	0x002c	/* PCI/NVMe: sticky status; write-1-clear after handling NVMe MSI */
#define  MSI_CTRL_INT			BIT(8)	/* PCI/NVMe: MSI interrupt pending from NVMe device */
#define PCIE_REGS_PCIE_SII_PM_STATE	0x00b0	/* PCI/NVMe: link state monitor; SMLH/RDLH up bits indicate NVMe ready */
#define  SMLH_LINK_UP			BIT(19)	/* PCI/NVMe: LTSSM reports link up (PHY/mac side) */
#define  RDLH_LINK_UP			BIT(8)	/* PCI/NVMe: data link layer up; needed before NVMe config cycles */
#define  PCIE_REGS_PCIE_SII_LINK_UP	(SMLH_LINK_UP | RDLH_LINK_UP)	/* PCI/NVMe: both layers up => NVMe PCIe link usable */
#define PCIE_REGS_PCIE_PHY_CNTL		0x0164	/* PCI/NVMe: PHY control; SRAM bypass for REFCLK-based NVMe PHY */
#define  PHY0_SRAM_BYPASS		BIT(8)	/* PCI/NVMe: bypass PHY SRAM to shorten NVMe boot */
#define PCIE_REGS_PCIE_PHY_STAT		0x0168	/* PCI/NVMe: PHY status; MPLLA lock required for stable NVMe link */
#define  PHY0_MPLLA_STATE		BIT(1)	/* PCI/NVMe: MPLLA locked before enabling LTSSM for NVMe SSD */
#define PCIE_REGS_LJPLL_STA		0x016c	/* PCI/NVMe: Low Jitter PLL status; LOCK bit polled at probe */
#define  LJPLL_LOCK			BIT(0)	/* PCI/NVMe: reference clock PLL locked => safe to train NVMe link */
#define PCIE_REGS_LJPLL_CNTRL_0		0x0170	/* PCI/NVMe: LJPLL enable/output-enable for REFCLK to NVMe */
#define  LJPLL_EN			BIT(29)	/* PCI/NVMe: enable LJPLL to generate REFCLK for NVMe SSD */
#define  LJPLL_FOUT_EN			GENMASK(24, 21)	/* PCI/NVMe: enable specific LJPLL outputs feeding PCIe clocks */
#define PCIE_REGS_LJPLL_CNTRL_2		0x0178	/* PCI/NVMe: LJPLL reference/fb divider values */
#define  LJPLL_REF_DIV			GENMASK(17, 12)	/* PCI/NVMe: REFCLK divider; sets PCIe Gen speed compatibility for NVMe */
#define  LJPLL_FB_DIV			GENMASK(11, 0)	/* PCI/NVMe: feedback divider; sets output frequency seen by NVMe */
#define PCIE_REGS_LJPLL_CNTRL_3		0x017c	/* PCI/NVMe: LJPLL post dividers */
#define  LJPLL_POST_DIV3A		GENMASK(24, 22)	/* PCI/NVMe: post-divider for PCIe clock output group 3A */
#define  LJPLL_POST_DIV2A		GENMASK(18, 16)	/* PCI/NVMe: post-divider for PCIe clock output group 2A */

#define PERST_DELAY_US		1000	/* PCI/NVMe: 1ms settle after PERST# edge before NVMe reset completes */
#define AUX_CLK_RATE_HZ		24000000	/* PCI/NVMe: 24MHz aux clock for DWC core logic during NVMe operation */

struct keembay_pcie {
	struct dw_pcie		pci;	/* PCI/NVMe: DesignWare core wrapper; hosts NVMe PCI bus enumeration */
	void __iomem		*apb_base;	/* PCI/NVMe: SoC-specific APB register base for link/PHY control */
	enum dw_pcie_device_mode mode;	/* PCI/NVMe: RC or EP mode; NVMe host uses DW_PCIE_RC_TYPE */

	struct clk		*clk_master;	/* PCI/NVMe: main AXI/PIPE clock for PCIe/NVMe datapath */
	struct clk		*clk_aux;	/* PCI/NVMe: auxiliary 24MHz clock for controller logic */
	struct gpio_desc	*reset;	/* PCI/NVMe: PERST# GPIO to hard-reset downstream NVMe SSD */
};

struct keembay_pcie_of_data {
	enum dw_pcie_device_mode mode;	/* PCI/NVMe: OF match data selects RC (NVMe host) or EP mode */
};

/* PCI/NVMe: assert PERST# to put NVMe SSD into reset before retraining */
static void keembay_ep_reset_assert(struct keembay_pcie *pcie)
{
	gpiod_set_value_cansleep(pcie->reset, 1);	/* PCI/NVMe: drive PERST# high -> NVMe endpoint resets */
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);	/* PCI/NVMe: wait for NVMe reset to propagate */
}

static void keembay_ep_reset_deassert(struct keembay_pcie *pcie)
{
	/*
	 * Ensure that PERST# is asserted for a minimum of 100ms.
	 *
	 * For more details, refer to PCI Express Card Electromechanical
	 * Specification Revision 1.1, Table-2.4.
	 */
	msleep(100);	/* PCI/NVMe: CEM spec requires 100ms PERST# assertion for NVMe */

	gpiod_set_value_cansleep(pcie->reset, 0);	/* PCI/NVMe: release PERST# so NVMe SSD can boot and train */
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);	/* PCI/NVMe: wait before LTSSM for NVMe endpoint to wake */
}

/* PCI/NVMe: control LTSSM start/stop; directly affects NVMe link availability */
static void keembay_pcie_ltssm_set(struct keembay_pcie *pcie, bool enable)
{
	u32 val;

	val = readl(pcie->apb_base + PCIE_REGS_PCIE_APP_CNTRL);	/* PCI/NVMe: read APP control to modify LTSSM bit */
	if (enable)
		val |= APP_LTSSM_ENABLE;	/* PCI/NVMe: start link training so NVMe SSD can appear */
	else
		val &= ~APP_LTSSM_ENABLE;	/* PCI/NVMe: stop link training, e.g. for NVMe power down */
	writel(val, pcie->apb_base + PCIE_REGS_PCIE_APP_CNTRL);	/* PCI/NVMe: commit LTSSM state */
}

/* PCI/NVMe: dw_pcie_ops.link_up callback; PCIe core asks if NVMe link is ready */
static bool keembay_pcie_link_up(struct dw_pcie *pci)
{
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);	/* PCI/NVMe: retrieve Keem Bay private state from platform dev */
	u32 val;

	val = readl(pcie->apb_base + PCIE_REGS_PCIE_SII_PM_STATE);	/* PCI/NVMe: poll SII link state register */

	return (val & PCIE_REGS_PCIE_SII_LINK_UP) == PCIE_REGS_PCIE_SII_LINK_UP;	/* PCI/NVMe: true only when both SMLH and RDLH up for NVMe */
}

/* PCI/NVMe: dw_pcie_ops.start_link callback; called before scanning NVMe bus */
static int keembay_pcie_start_link(struct dw_pcie *pci)
{
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);	/* PCI/NVMe: get SoC-specific state */
	u32 val;
	int ret;

	if (pcie->mode == DW_PCIE_EP_TYPE)
		return 0;	/* PCI/NVMe: endpoint mode has no downstream NVMe to train */

	keembay_pcie_ltssm_set(pcie, false);	/* PCI/NVMe: hold LTSSM while PLL/PHY stabilizes for NVMe */

	ret = readl_poll_timeout(pcie->apb_base + PCIE_REGS_PCIE_PHY_STAT,
				 val, val & PHY0_MPLLA_STATE, 20,
				 500 * USEC_PER_MSEC);	/* PCI/NVMe: wait up to 500ms for MPLLA lock before NVMe link training */
	if (ret) {
		dev_err(pci->dev, "MPLLA is not locked\n");	/* PCI/NVMe: NVMe link cannot be reliable without MPLLA lock */
		return ret;
	}

	keembay_pcie_ltssm_set(pcie, true);	/* PCI/NVMe: begin link training toward NVMe SSD */

	return 0;
}

/* PCI/NVMe: dw_pcie_ops.stop_link callback; used when shutting down NVMe path */
static void keembay_pcie_stop_link(struct dw_pcie *pci)
{
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);

	keembay_pcie_ltssm_set(pcie, false);	/* PCI/NVMe: drop NVMe PCIe link by stopping LTSSM */
}

/* PCI/NVMe: DesignWare core callbacks for NVMe host link management */
static const struct dw_pcie_ops keembay_pcie_ops = {
	.link_up	= keembay_pcie_link_up,	/* PCI/NVMe: tells core if NVMe downstream link is up */
	.start_link	= keembay_pcie_start_link,	/* PCI/NVMe: called to bring up NVMe link during enumeration */
	.stop_link	= keembay_pcie_stop_link,	/* PCI/NVMe: called to tear down NVMe link */
};

/* PCI/NVMe: devm action to disable clocks on driver remove/rollback */
static inline void keembay_pcie_disable_clock(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);	/* PCI/NVMe: gate PCIe clocks; NVMe device becomes inaccessible after this */
}

/* PCI/NVMe: request, set rate, enable, and register teardown for a PCIe clock */
static inline struct clk *keembay_pcie_probe_clock(struct device *dev,
						   const char *id, u64 rate)
{
	struct clk *clk;
	int ret;

	clk = devm_clk_get(dev, id);	/* PCI/NVMe: lookup "master" or "aux" clock needed by NVMe controller */
	if (IS_ERR(clk))
		return clk;	/* PCI/NVMe: defer probe if clock provider not yet ready */

	if (rate) {
		ret = clk_set_rate(clk, rate);	/* PCI/NVMe: enforce required clock rate for NVMe/PCIe PHY */
		if (ret)
			return ERR_PTR(ret);
	}

	ret = clk_prepare_enable(clk);	/* PCI/NVMe: turn on clock before accessing NVMe link registers */
	if (ret)
		return ERR_PTR(ret);

	ret = devm_add_action_or_reset(dev, keembay_pcie_disable_clock, clk);	/* PCI/NVMe: auto-disable on probe failure or remove */
	if (ret)
		return ERR_PTR(ret);

	return clk;
}

/* PCI/NVMe: enable master + aux clocks required for NVMe SSD enumeration */
static int keembay_pcie_probe_clocks(struct keembay_pcie *pcie)
{
	struct dw_pcie *pci = &pcie->pci;	/* PCI/NVMe: pointer to DWC core state */
	struct device *dev = pci->dev;	/* PCI/NVMe: device used for devm resources */

	pcie->clk_master = keembay_pcie_probe_clock(dev, "master", 0);	/* PCI/NVMe: enable master datapath clock for NVMe */
	if (IS_ERR(pcie->clk_master))
		return dev_err_probe(dev, PTR_ERR(pcie->clk_master),
				     "Failed to enable master clock");

	pcie->clk_aux = keembay_pcie_probe_clock(dev, "aux", AUX_CLK_RATE_HZ);	/* PCI/NVMe: enable 24MHz aux clock for DWC logic */
	if (IS_ERR(pcie->clk_aux))
		return dev_err_probe(dev, PTR_ERR(pcie->clk_aux),
				     "Failed to enable auxiliary clock");

	return 0;
}

/*
 * Initialize the internal PCIe PLL in Host mode.
 * See the following sections in Keem Bay data book,
 * (1) 6.4.6.1 PCIe Subsystem Example Initialization,
 * (2) 6.8 PCIe Low Jitter PLL for Ref Clk Generation.
 */
/* PCI/NVMe: generate clean REFCLK for NVMe SSD before link training */
static int keembay_pcie_pll_init(struct keembay_pcie *pcie)
{
	struct dw_pcie *pci = &pcie->pci;	/* PCI/NVMe: DWC core handle for error messages */
	u32 val;
	int ret;

	val = FIELD_PREP(LJPLL_REF_DIV, 0) | FIELD_PREP(LJPLL_FB_DIV, 0x32);	/* PCI/NVMe: configure PLL dividers for PCIe REFCLK frequency */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_2);

	val = FIELD_PREP(LJPLL_POST_DIV3A, 0x2) |
		FIELD_PREP(LJPLL_POST_DIV2A, 0x2);	/* PCI/NVMe: set post-dividers to deliver correct clock to NVMe PHY */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_3);

	val = FIELD_PREP(LJPLL_EN, 0x1) | FIELD_PREP(LJPLL_FOUT_EN, 0xc);	/* PCI/NVMe: enable PLL and required output clocks for NVMe */
	writel(val, pcie->apb_base + PCIE_REGS_LJPLL_CNTRL_0);

	ret = readl_poll_timeout(pcie->apb_base + PCIE_REGS_LJPLL_STA,
				 val, val & LJPLL_LOCK, 20,
				 500 * USEC_PER_MSEC);	/* PCI/NVMe: wait for PLL lock before allowing NVMe link training */
	if (ret)
		dev_err(pci->dev, "Low jitter PLL is not locked\n");	/* PCI/NVMe: NVMe REFCLK unusable if PLL fails to lock */

	return ret;
}

/*
 * Keem Bay PCIe Controller provides an additional IP logic on top of
 * standard DWC IP to clear MSI IRQ by writing '1' to the respective
 * bit of the status register.
 *
 * So, a chained irq handler is defined to handle this additional
 * IP logic.
 */
/* PCI/NVMe: chained IRQ handler for MSI interrupts raised by NVMe queues */
static void keembay_pcie_msi_irq_handler(struct irq_desc *desc)
{
	struct keembay_pcie *pcie = irq_desc_get_handler_data(desc);	/* PCI/NVMe: SoC state retrieved from chained IRQ data */
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* PCI/NVMe: parent irq_chip (usually GIC) */
	u32 val, mask, status;
	struct dw_pcie_rp *pp;

	/*
	 * Keem Bay PCIe Controller provides an additional IP logic on top of
	 * standard DWC IP to clear MSI IRQ by writing '1' to the respective
	 * bit of the status register.
	 *
	 * So, a chained irq handler is defined to handle this additional
	 * IP logic.
	 */

	chained_irq_enter(chip, desc);	/* PCI/NVMe: mask parent IRQ while handling NVMe MSI storm */

	pp = &pcie->pci.pp;	/* PCI/NVMe: DWC Root Port state where MSI IRQ domain lives */
	val = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_STATUS);	/* PCI/NVMe: read pending MSI status from NVMe endpoint path */
	mask = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);	/* PCI/NVMe: only handle MSI sources enabled for NVMe */

	status = val & mask;	/* PCI/NVMe: compute actionable MSI interrupts for NVMe */

	if (status & MSI_CTRL_INT) {
		dw_handle_msi_irq(pp);	/* PCI/NVMe: dispatch to MSI IRQ domain; wakes NVMe queue completions */
		writel(status, pcie->apb_base + PCIE_REGS_INTERRUPT_STATUS);	/* PCI/NVMe: write-1-clear MSI pending so NVMe can raise again */
	}

	chained_irq_exit(chip, desc);	/* PCI/NVMe: unmask parent IRQ for next NVMe MSI */
}

/* PCI/NVMe: register the chained IRQ for DWC MSI used by NVMe SSD */
static int keembay_pcie_setup_msi_irq(struct keembay_pcie *pcie)
{
	struct dw_pcie *pci = &pcie->pci;	/* PCI/NVMe: DWC core state */
	struct device *dev = pci->dev;	/* PCI/NVMe: device for resource accounting */
	struct platform_device *pdev = to_platform_device(dev);	/* PCI/NVMe: platform device carrying IRQ resources */
	int irq;

	irq = platform_get_irq_byname(pdev, "pcie");	/* PCI/NVMe: fetch "pcie" IRQ that carries MSI from NVMe SSD */
	if (irq < 0)
		return irq;	/* PCI/NVMe: defer if IRQ not yet available */

	irq_set_chained_handler_and_data(irq, keembay_pcie_msi_irq_handler,
					 pcie);	/* PCI/NVMe: route GIC irq to Keem Bay MSI handler for NVMe */

	return 0;
}

/* PCI/NVMe: endpoint mode init (not NVMe host path) */
static void keembay_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* PCI/NVMe: back-pointer to DWC core */
	struct keembay_pcie *pcie = dev_get_drvdata(pci->dev);

	writel(EDMA_INT_EN, pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);	/* PCI/NVMe: enable eDMA interrupts for EP role */
}

/* PCI/NVMe: endpoint IRQ raise helper (irrelevant for NVMe host RC mode) */
static int keembay_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	case PCI_IRQ_INTX:
		/* INTx interrupts are not supported in Keem Bay */
		dev_err(pci->dev, "INTx IRQ is not supported\n");	/* PCI/NVMe: legacy INTx not available for NVMe fallback here */
		return -EINVAL;
	case PCI_IRQ_MSI:
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);	/* PCI/NVMe: raise MSI as EP (test/back-to-back scenario) */
	case PCI_IRQ_MSIX:
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);	/* PCI/NVMe: raise MSI-X as EP (test/back-to-back scenario) */
	default:
		dev_err(pci->dev, "Unknown IRQ type %d\n", type);
		return -EINVAL;
	}
}

/* PCI/NVMe: endpoint controller feature flags (not used by NVMe host driver) */
static const struct pci_epc_features keembay_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES,
	.msi_capable		= true,	/* PCI/NVMe: EP supports MSI if probed as endpoint */
	.msix_capable		= true,	/* PCI/NVMe: EP supports MSI-X if probed as endpoint */
	.bar[BAR_0]		= { .only_64bit = true, },
	.bar[BAR_2]		= { .only_64bit = true, },
	.bar[BAR_4]		= { .only_64bit = true, },
	.align			= SZ_16K,
};

static const struct pci_epc_features *
keembay_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &keembay_pcie_epc_features;
}

/* PCI/NVMe: endpoint mode operations (not NVMe host RC path) */
static const struct dw_pcie_ep_ops keembay_pcie_ep_ops = {
	.init		= keembay_pcie_ep_init,
	.raise_irq	= keembay_pcie_ep_raise_irq,
	.get_features	= keembay_pcie_get_features,
};

/* PCI/NVMe: host mode operations; mostly empty because DWC core handles NVMe enumeration */
static const struct dw_pcie_host_ops keembay_pcie_host_ops = {
};

/* PCI/NVMe: initialize Root Port for downstream NVMe SSD enumeration */
static int keembay_pcie_add_pcie_port(struct keembay_pcie *pcie,
				      struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;	/* PCI/NVMe: DWC core state */
	struct dw_pcie_rp *pp = &pci->pp;	/* PCI/NVMe: Root Port state used to scan NVMe bus */
	struct device *dev = &pdev->dev;	/* PCI/NVMe: platform device for devm resources */
	u32 val;
	int ret;

	pp->ops = &keembay_pcie_host_ops;	/* PCI/NVMe: attach host ops so DWC can enumerate NVMe */
	pp->msi_irq[0] = -ENODEV;	/* PCI/NVMe: no direct MSI vector; Keem Bay uses chained handler above */

	ret = keembay_pcie_setup_msi_irq(pcie);	/* PCI/NVMe: wire MSI controller IRQ for NVMe queue interrupts */
	if (ret)
		return ret;

	pcie->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);	/* PCI/NVMe: request PERST# GPIO, initially asserted to reset NVMe SSD */
	if (IS_ERR(pcie->reset))
		return PTR_ERR(pcie->reset);

	ret = keembay_pcie_probe_clocks(pcie);	/* PCI/NVMe: turn on master/aux clocks needed for NVMe link */
	if (ret)
		return ret;

	val = readl(pcie->apb_base + PCIE_REGS_PCIE_PHY_CNTL);
	val |= PHY0_SRAM_BYPASS;	/* PCI/NVMe: bypass PHY SRAM for faster NVMe bring-up */
	writel(val, pcie->apb_base + PCIE_REGS_PCIE_PHY_CNTL);

	writel(PCIE_DEVICE_TYPE, pcie->apb_base + PCIE_REGS_PCIE_CFG);	/* PCI/NVMe: configure controller as Root Complex for NVMe enumeration */

	ret = keembay_pcie_pll_init(pcie);	/* PCI/NVMe: start REFCLK PLL for NVMe SSD */
	if (ret)
		return ret;

	val = readl(pcie->apb_base + PCIE_REGS_PCIE_CFG);
	writel(val | PCIE_RSTN, pcie->apb_base + PCIE_REGS_PCIE_CFG);	/* PCI/NVMe: release controller internal reset */
	keembay_ep_reset_deassert(pcie);	/* PCI/NVMe: release PERST# so NVMe SSD can train */

	ret = dw_pcie_host_init(pp);	/* PCI/NVMe: enumerate PCI bus; NVMe driver may bind to detected function */
	if (ret) {
		keembay_ep_reset_assert(pcie);	/* PCI/NVMe: reassert PERST# to reset NVMe on enumeration failure */
		dev_err(dev, "Failed to initialize host: %d\n", ret);
		return ret;
	}

	val = readl(pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);
	if (IS_ENABLED(CONFIG_PCI_MSI))
		val |= MSI_CTRL_INT_EN;	/* PCI/NVMe: enable MSI forwarding once NVMe is enumerated */
	writel(val, pcie->apb_base + PCIE_REGS_INTERRUPT_ENABLE);

	return 0;
}

/* PCI/NVMe: platform driver probe; chooses NVMe host RC or EP mode */
static int keembay_pcie_probe(struct platform_device *pdev)
{
	const struct keembay_pcie_of_data *data;	/* PCI/NVMe: OF match data for RC/EP selection */
	struct device *dev = &pdev->dev;	/* PCI/NVMe: platform device representing this PCIe controller */
	struct keembay_pcie *pcie;	/* PCI/NVMe: SoC-private state */
	struct dw_pcie *pci;	/* PCI/NVMe: DWC core state that NVMe host driver will bind through */
	enum dw_pcie_device_mode mode;
	int ret;

	data = device_get_match_data(dev);	/* PCI/NVMe: look up compatible "intel,keembay-pcie" vs "...-ep" */
	if (!data)
		return -ENODEV;

	mode = (enum dw_pcie_device_mode)data->mode;	/* PCI/NVMe: determine RC or EP mode for this controller */

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);	/* PCI/NVMe: allocate SoC controller state */
	if (!pcie)
		return -ENOMEM;

	pci = &pcie->pci;	/* PCI/NVMe: init DWC core state */
	pci->dev = dev;	/* PCI/NVMe: link DWC core to platform device so NVMe path can use dev */
	pci->ops = &keembay_pcie_ops;	/* PCI/NVMe: register link_up/start/stop ops used during NVMe enumeration */

	pcie->mode = mode;	/* PCI/NVMe: remember RC or EP mode */

	pcie->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");	/* PCI/NVMe: map SoC APB registers for link/PHY/IRQ control */
	if (IS_ERR(pcie->apb_base))
		return PTR_ERR(pcie->apb_base);

	platform_set_drvdata(pdev, pcie);	/* PCI/NVMe: store state for remove/suspend and dev_get_drvdata */

	switch (pcie->mode) {
	case DW_PCIE_RC_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_KEEMBAY_HOST))
			return -ENODEV;	/* PCI/NVMe: host driver built-out; cannot enumerate NVMe */

		return keembay_pcie_add_pcie_port(pcie, pdev);	/* PCI/NVMe: bring up RC and enumerate NVMe SSD */
	case DW_PCIE_EP_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_KEEMBAY_EP))
			return -ENODEV;	/* PCI/NVMe: EP driver built-out; cannot act as NVMe target */

		pci->ep.ops = &keembay_pcie_ep_ops;	/* PCI/NVMe: attach EP ops for endpoint role */
		ret = dw_pcie_ep_init(&pci->ep);	/* PCI/NVMe: init DWC endpoint state */
		if (ret)
			return ret;

		ret = dw_pcie_ep_init_registers(&pci->ep);	/* PCI/NVMe: program endpoint capability registers */
		if (ret) {
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			dw_pcie_ep_deinit(&pci->ep);
			return ret;
		}

		pci_epc_init_notify(pci->ep.epc);	/* PCI/NVMe: notify PCI endpoint framework */

		break;
	default:
		dev_err(dev, "Invalid device type %d\n", pcie->mode);
		return -ENODEV;
	}

	return 0;
}

/* PCI/NVMe: OF data for Root Complex mode (NVMe host path) */
static const struct keembay_pcie_of_data keembay_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,
};

/* PCI/NVMe: OF data for Endpoint mode (not the NVMe host path) */
static const struct keembay_pcie_of_data keembay_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,
};

/* PCI/NVMe: device tree match table selecting RC/EP mode */
static const struct of_device_id keembay_pcie_of_match[] = {
	{
		.compatible = "intel,keembay-pcie",	/* PCI/NVMe: host controller to enumerate NVMe SSD */
		.data = &keembay_pcie_rc_of_data,
	},
	{
		.compatible = "intel,keembay-pcie-ep",	/* PCI/NVMe: endpoint controller mode */
		.data = &keembay_pcie_ep_of_data,
	},
	{}
};

/* PCI/NVMe: platform driver structure registered at boot */
static struct platform_driver keembay_pcie_driver = {
	.driver = {
		.name = "keembay-pcie",	/* PCI/NVMe: platform driver name visible in sysfs */
		.of_match_table = keembay_pcie_of_match,	/* PCI/NVMe: match against DT nodes for NVMe host controller */
		.suppress_bind_attrs = true,	/* PCI/NVMe: manual bind/unbind disabled for this PCIe bridge */
	},
	.probe  = keembay_pcie_probe,	/* PCI/NVMe: probe routine that sets up NVMe host RC or EP */
};
builtin_platform_driver(keembay_pcie_driver);	/* PCI/NVMe: register driver early so NVMe SSD can be enumerated during boot */
