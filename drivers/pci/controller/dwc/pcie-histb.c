// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for HiSilicon STB SoCs
 *
 * Copyright (C) 2016-2017 HiSilicon Co., Ltd. http://www.hisilicon.com
 *
 * Authors: Ruqiang Ju <juruqiang@hisilicon.com>
 *          Jianguo Sun <sunjianguo1@huawei.com>
 */

#include <linux/clk.h>		/* PCI/NVMe: clock framework for PCIe ref clocks feeding NVMe link training */
#include <linux/delay.h>	/* PCI/NVMe: udelay/mdelay used during NVMe endpoint reset/link bring-up */
#include <linux/gpio/consumer.h>/* PCI/NVMe: optional PERST# GPIO to reset downstream NVMe slot/device */
#include <linux/interrupt.h>	/* PCI/NVMe: legacy/MSI/MSI-X interrupt plumbing for NVMe queues */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>		/* PCI/NVMe: DT parsing for NVMe host controller resources/IRQs */
#include <linux/pci.h>		/* PCI/NVMe: core PCI bus APIs that discover and bind nvme-pci devices */
#include <linux/phy/phy.h>	/* PCI/NVMe: PHY init for the SERDES lanes carrying NVMe PCIe signals */
#include <linux/platform_device.h>/* PCI/NVMe: platform bus registration of this RC */
#include <linux/resource.h>	/* PCI/NVMe: MMIO resource management for config/ATU windows */
#include <linux/reset.h>	/* PCI/NVMe: SoC reset sequencing before NVMe link can train */

#include "pcie-designware.h"	/* PCI/NVMe: DesignWare host core shared with nvme-pci enumeration path */

#define to_histb_pcie(x)	dev_get_drvdata((x)->dev)	/* PCI/NVMe: retrieve SoC wrapper from dw_pcie device for NVMe host init */

#define PCIE_SYS_CTRL0		0x0000	/* PCI/NVMe: SoC wrapper register: device type/DBI write enable */
#define PCIE_SYS_CTRL1		0x0004	/* PCI/NVMe: SoC wrapper register: DBI read enable */
#define PCIE_SYS_CTRL7		0x001C	/* PCI/NVMe: SoC wrapper register: LTSSM enable (starts NVMe link) */
#define PCIE_SYS_CTRL13		0x0034
#define PCIE_SYS_CTRL15		0x003C
#define PCIE_SYS_CTRL16		0x0040
#define PCIE_SYS_CTRL17		0x0044

#define PCIE_SYS_STAT0		0x0100	/* PCI/NVMe: link/PHY status used to detect NVMe presence */
#define PCIE_SYS_STAT4		0x0110	/* PCI/NVMe: LTSSM state used to confirm NVMe link is active */

#define PCIE_RDLH_LINK_UP	BIT(5)	/* PCI/NVMe: receiver-detection/link-up indicator for NVMe endpoint */
#define PCIE_XMLH_LINK_UP	BIT(15)	/* PCI/NVMe: MAC/data-link layer up; required before NVMe config cycles */
#define PCIE_ELBI_SLV_DBI_ENABLE BIT(21)	/* PCI/NVMe: gate DBI access to DesignWare core config registers used by nvme-pci bus ops */
#define PCIE_APP_LTSSM_ENABLE	BIT(11)	/* PCI/NVMe: starts Link Training and Status State Machine to reach NVMe L0 */

#define PCIE_DEVICE_TYPE_MASK	GENMASK(31, 28)	/* PCI/NVMe: RC/EP mode field; must be Root Complex for NVMe host operation */
#define PCIE_WM_EP		0	/* PCI/NVMe: Endpoint mode (not used when hosting NVMe) */
#define PCIE_WM_LEGACY		BIT(1)
#define PCIE_WM_RC		BIT(30)	/* PCI/NVMe: Root Complex mode so PCI core can enumerate NVMe below this bridge */

#define PCIE_LTSSM_STATE_MASK	GENMASK(5, 0)	/* PCI/NVMe: mask to read current LTSSM state during NVMe link training */
#define PCIE_LTSSM_STATE_ACTIVE	0x11	/* PCI/NVMe: L0 active state; NVMe device can respond to config TLPs */

struct histb_pcie {
	struct dw_pcie *pci;	/* PCI/NVMe: DesignWare core instance; bridges to PCI core/nvme-pci */
	struct clk *aux_clk;	/* PCI/NVMe: auxiliary clock for DWC core logic servicing NVMe traffic */
	struct clk *pipe_clk;	/* PCI/NVMe: PIPE clock from PHY; required for NVMe Gen1/Gen2 operation */
	struct clk *sys_clk;	/* PCI/NVMe: system clock for transaction layer/DMA toward NVMe */
	struct clk *bus_clk;	/* PCI/NVMe: AXI/ahb bus clock carrying NVMe MMIO and DMA payloads */
	struct phy *phy;	/* PCI/NVMe: SERDES PHY for differential PCIe lanes to NVMe SSD */
	struct reset_control *soft_reset;	/* PCI/NVMe: DWC core reset released before NVMe link training */
	struct reset_control *sys_reset;	/* PCI/NVMe: system interconnect reset used before NVMe DMA paths work */
	struct reset_control *bus_reset;	/* PCI/NVMe: bus interface reset released before NVMe config accesses */
	void __iomem *ctrl;	/* PCI/NVMe: SoC wrapper MMIO for link control/status used by NVMe host */
	struct gpio_desc *reset_gpio;	/* PCI/NVMe: PERST# line to downstream NVMe slot or M.2 connector */
	struct regulator *vpcie;	/* PCI/NVMe: PCIe IO/domain regulator powering NVMe endpoint */
};

static u32 histb_pcie_readl(struct histb_pcie *histb_pcie, u32 reg)
{
	return readl(histb_pcie->ctrl + reg);	/* PCI/NVMe: read SoC wrapper status; e.g. link-up needed before NVMe probe */
}

static void histb_pcie_writel(struct histb_pcie *histb_pcie, u32 reg, u32 val)
{
	writel(val, histb_pcie->ctrl + reg);	/* PCI/NVMe: write SoC wrapper control; e.g. enable LTSSM to start NVMe link */
}

static void histb_pcie_dbi_w_mode(struct dw_pcie_rp *pp, bool enable)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* PCI/NVMe: get DWC core from root port for NVMe host config path */
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: get SoC wrapper to toggle DBI write gate */
	u32 val;

	val = histb_pcie_readl(hipcie, PCIE_SYS_CTRL0);	/* PCI/NVMe: fetch current control0 state */
	if (enable)	/* PCI/NVMe: allow writes to DWC internal config registers that describe the RC seen by NVMe */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	else	/* PCI/NVMe: lock DBI write to protect RC config from stray NVMe host writes */
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL0, val);	/* PCI/NVMe: apply DBI write gate change */
}

static void histb_pcie_dbi_r_mode(struct dw_pcie_rp *pp, bool enable)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* PCI/NVMe: get DWC core from root port for NVMe host config path */
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: get SoC wrapper to toggle DBI read gate */
	u32 val;

	val = histb_pcie_readl(hipcie, PCIE_SYS_CTRL1);	/* PCI/NVMe: fetch control1 state */
	if (enable)	/* PCI/NVMe: allow reads of DWC internal config registers (BAR, capability, MSI) used by NVMe enumeration */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	else	/* PCI/NVMe: close DBI read gate after config access */
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL1, val);	/* PCI/NVMe: apply DBI read gate change */
}

static u32 histb_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
			       u32 reg, size_t size)
{
	u32 val;

	histb_pcie_dbi_r_mode(&pci->pp, true);	/* PCI/NVMe: open DBI read window before accessing RC config registers */
	dw_pcie_read(base + reg, size, &val);	/* PCI/NVMe: perform config read used during RC setup for NVMe bus */
	histb_pcie_dbi_r_mode(&pci->pp, false);	/* PCI/NVMe: close DBI read window to protect RC config space */

	return val;	/* PCI/NVMe: return config value consumed by DWC core/PCI bus initialization before NVMe probe */
}

static void histb_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size, u32 val)
{
	histb_pcie_dbi_w_mode(&pci->pp, true);	/* PCI/NVMe: open DBI write window to program RC BARs, capabilities, ATU for NVMe */
	dw_pcie_write(base + reg, size, val);	/* PCI/NVMe: write config register; e.g. set up ATU windows for NVMe MMIO/DMA */
	histb_pcie_dbi_w_mode(&pci->pp, false);	/* PCI/NVMe: close DBI write window after RC config update */
}

static int histb_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);	/* PCI/NVMe: DWC core behind this bus; PCI core uses it to reach NVMe downstream */

	if (PCI_SLOT(devfn))	/* PCI/NVMe: only Bus 0 Dev 0 is the local RC; other slots are downstream NVMe devices handled elsewhere */
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = dw_pcie_read_dbi(pci, where, size);	/* PCI/NVMe: read local RC config space; part of the bridge seen by nvme-pci enumeration */
	return PCIBIOS_SUCCESSFUL;
}

static int histb_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);	/* PCI/NVMe: DWC core behind this bus */

	if (PCI_SLOT(devfn))	/* PCI/NVMe: reject writes to non-local slots; NVMe SSD config writes go through the secondary bus */
		return PCIBIOS_DEVICE_NOT_FOUND;

	dw_pcie_write_dbi(pci, where, size, val);	/* PCI/NVMe: write local RC config space during bridge setup for NVMe */
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops histb_pci_ops = {
	.read = histb_pcie_rd_own_conf,	/* PCI/NVMe: pci_bus_read_config_* entry point used by PCI core to enumerate NVMe */
	.write = histb_pcie_wr_own_conf,	/* PCI/NVMe: pci_bus_write_config_* entry point used by PCI core before binding nvme-pci */
};

static bool histb_pcie_link_up(struct dw_pcie *pci)
{
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: SoC wrapper containing link status needed for NVMe presence */
	u32 regval;
	u32 status;

	regval = histb_pcie_readl(hipcie, PCIE_SYS_STAT0);	/* PCI/NVMe: PHY/MAC link-up flags */
	status = histb_pcie_readl(hipcie, PCIE_SYS_STAT4);	/* PCI/NVMe: LTSSM state reflecting NVMe link negotiation */
	status &= PCIE_LTSSM_STATE_MASK;	/* PCI/NVMe: isolate LTSSM state to compare with L0 */
	return ((regval & PCIE_XMLH_LINK_UP) && (regval & PCIE_RDLH_LINK_UP) &&
		(status == PCIE_LTSSM_STATE_ACTIVE));	/* PCI/NVMe: true only when NVMe endpoint has reached L0 and data link is up */
}

static int histb_pcie_start_link(struct dw_pcie *pci)
{
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: SoC wrapper to start NVMe link training */
	u32 regval;

	/* assert LTSSM enable */
	regval = histb_pcie_readl(hipcie, PCIE_SYS_CTRL7);	/* PCI/NVMe: read LTSSM control register */
	regval |= PCIE_APP_LTSSM_ENABLE;	/* PCI/NVMe: set bit to begin PCIe link training toward NVMe L0 */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL7, regval);	/* PCI/NVMe: kick off link training; nvme-pci probe waits for this to complete */

	return 0;	/* PCI/NVMe: link training initiated; actual up state checked later by histb_pcie_link_up */
}

static int histb_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* PCI/NVMe: DWC core for this NVMe host controller */
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: SoC wrapper */
	u32 regval;

	pp->bridge->ops = &histb_pci_ops;	/* PCI/NVMe: attach pci_ops so PCI core can read config headers of downstream NVMe devices */

	/* PCIe RC work mode */
	regval = histb_pcie_readl(hipcie, PCIE_SYS_CTRL0);	/* PCI/NVMe: read current device-type field */
	regval &= ~PCIE_DEVICE_TYPE_MASK;	/* PCI/NVMe: clear previous mode before setting Root Complex */
	regval |= PCIE_WM_RC;	/* PCI/NVMe: configure as Root Complex so PCI core can enumerate NVMe endpoints below it */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL0, regval);	/* PCI/NVMe: apply RC mode; required before NVMe config space becomes reachable */

	return 0;	/* PCI/NVMe: host init done; dw_pcie_host_init() will continue bridge setup for NVMe */
}

static const struct dw_pcie_host_ops histb_pcie_host_ops = {
	.init = histb_pcie_host_init,	/* PCI/NVMe: called by dw_pcie_host_init() during RC setup before NVMe enumeration */
};

static void histb_pcie_host_disable(struct histb_pcie *hipcie)
{
	reset_control_assert(hipcie->soft_reset);	/* PCI/NVMe: hold DWC core reset; stops NVMe config/DMA traffic */
	reset_control_assert(hipcie->sys_reset);	/* PCI/NVMe: hold system reset; disconnects DMA paths used by NVMe queues */
	reset_control_assert(hipcie->bus_reset);	/* PCI/NVMe: hold bus reset; halts AXI transactions from NVMe host */

	clk_disable_unprepare(hipcie->aux_clk);	/* PCI/NVMe: gate aux clock; no NVMe link layer can run */
	clk_disable_unprepare(hipcie->pipe_clk);	/* PCI/NVMe: gate PIPE clock; PHY stops, NVMe link drops */
	clk_disable_unprepare(hipcie->sys_clk);	/* PCI/NVMe: gate system clock; transaction layer/DMA stops */
	clk_disable_unprepare(hipcie->bus_clk);	/* PCI/NVMe: gate bus clock; MMIO/DMA fabric idles */

	if (hipcie->reset_gpio)
		gpiod_set_value_cansleep(hipcie->reset_gpio, 1);	/* PCI/NVMe: assert PERST# to reset downstream NVMe slot/device */

	if (hipcie->vpcie)
		regulator_disable(hipcie->vpcie);	/* PCI/NVMe: power down PCIe domain when no NVMe device is needed */
}

static int histb_pcie_host_enable(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* PCI/NVMe: DWC core for this NVMe host controller */
	struct histb_pcie *hipcie = to_histb_pcie(pci);	/* PCI/NVMe: SoC wrapper resources */
	struct device *dev = pci->dev;	/* PCI/NVMe: device struct for logging during NVMe host bring-up */
	int ret;

	/* power on PCIe device if have */
	if (hipcie->vpcie) {	/* PCI/NVMe: check whether a PCIe regulator powers the NVMe endpoint */
		ret = regulator_enable(hipcie->vpcie);	/* PCI/NVMe: power up NVMe slot/domain before link training */
		if (ret) {
			dev_err(dev, "failed to enable regulator: %d\n", ret);
			return ret;	/* PCI/NVMe: cannot power NVMe; abort host enable */
		}
	}

	if (hipcie->reset_gpio)
		gpiod_set_value_cansleep(hipcie->reset_gpio, 0);	/* PCI/NVMe: deassert PERST# so downstream NVMe device can boot and train */

	ret = clk_prepare_enable(hipcie->bus_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable bus clk\n");
		goto err_bus_clk;	/* PCI/NVMe: unwind regulator if bus clock fails before NVMe DMA path exists */
	}

	ret = clk_prepare_enable(hipcie->sys_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable sys clk\n");
		goto err_sys_clk;	/* PCI/NVMe: unwind bus clock; no NVMe transaction layer available */
	}

	ret = clk_prepare_enable(hipcie->pipe_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable pipe clk\n");
		goto err_pipe_clk;	/* PCI/NVMe: unwind sys clock; PHY cannot lock for NVMe link */
	}

	ret = clk_prepare_enable(hipcie->aux_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clk\n");
		goto err_aux_clk;	/* PCI/NVMe: unwind pipe clock; DWC core cannot service NVMe */
	}

	reset_control_assert(hipcie->soft_reset);
	reset_control_deassert(hipcie->soft_reset);	/* PCI/NVMe: pulse soft reset to put DWC core in known state for NVMe enumeration */

	reset_control_assert(hipcie->sys_reset);
	reset_control_deassert(hipcie->sys_reset);	/* PCI/NVMe: pulse sys reset so DMA/IOMMU paths align before NVMe queues run */

	reset_control_assert(hipcie->bus_reset);
	reset_control_deassert(hipcie->bus_reset);	/* PCI/NVMe: pulse bus reset so AXI fabric is clean before NVMe MMIO/DMA */

	return 0;	/* PCI/NVMe: clocks/resets ready; dw_pcie_host_init() will now set up bridge for NVMe */

err_aux_clk:
	clk_disable_unprepare(hipcie->pipe_clk);
err_pipe_clk:
	clk_disable_unprepare(hipcie->sys_clk);
err_sys_clk:
	clk_disable_unprepare(hipcie->bus_clk);
err_bus_clk:
	if (hipcie->vpcie)
		regulator_disable(hipcie->vpcie);	/* PCI/NVMe: power down NVMe domain on enable failure */

	return ret;	/* PCI/NVMe: propagate failure so nvme-pci never probes on a broken link */
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.read_dbi = histb_pcie_read_dbi,	/* PCI/NVMe: DWC core calls this to read RC config; used before NVMe device enumeration */
	.write_dbi = histb_pcie_write_dbi,	/* PCI/NVMe: DWC core calls this to write RC config; programs ATU/BARs for NVMe MMIO/DMA */
	.link_up = histb_pcie_link_up,	/* PCI/NVMe: DWC core polls this to wait for NVMe endpoint link during enumeration */
	.start_link = histb_pcie_start_link,	/* PCI/NVMe: DWC core calls this to start NVMe link training */
};

static int histb_pcie_probe(struct platform_device *pdev)
{
	struct histb_pcie *hipcie;	/* PCI/NVMe: per-controller SoC wrapper state for one NVMe host */
	struct dw_pcie *pci;	/* PCI/NVMe: DesignWare core state shared with PCI enumeration path */
	struct dw_pcie_rp *pp;	/* PCI/NVMe: root port representation; parent of downstream NVMe buses */
	struct device *dev = &pdev->dev;	/* PCI/NVMe: platform device used for devm_* allocations for NVMe host */
	int ret;

	hipcie = devm_kzalloc(dev, sizeof(*hipcie), GFP_KERNEL);	/* PCI/NVMe: allocate SoC wrapper; failure prevents NVMe host instantiation */
	if (!hipcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);	/* PCI/NVMe: allocate DWC core state; bridge between SoC wrapper and nvme-pci */
	if (!pci)
		return -ENOMEM;

	hipcie->pci = pci;	/* PCI/NVMe: link wrapper to core so pci_ops callbacks can find SoC registers */
	pp = &pci->pp;	/* PCI/NVMe: root port pointer used by dw_pcie_host_init() to enumerate NVMe */
	pci->dev = dev;	/* PCI/NVMe: attach device for logging and resource management */
	pci->ops = &dw_pcie_ops;	/* PCI/NVMe: register DBI/link callbacks used by DWC core during NVMe host setup */

	hipcie->ctrl = devm_platform_ioremap_resource_byname(pdev, "control");
	if (IS_ERR(hipcie->ctrl)) {
		dev_err(dev, "cannot get control reg base\n");
		return PTR_ERR(hipcie->ctrl);	/* PCI/NVMe: cannot access SoC wrapper; NVMe host cannot start */
	}

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "rc-dbi");
	if (IS_ERR(pci->dbi_base)) {
		dev_err(dev, "cannot get rc-dbi base\n");
		return PTR_ERR(pci->dbi_base);	/* PCI/NVMe: cannot access DWC DBI; RC config/ATU for NVMe unreachable */
	}

	hipcie->vpcie = devm_regulator_get_optional(dev, "vpcie");
	if (IS_ERR(hipcie->vpcie)) {
		if (PTR_ERR(hipcie->vpcie) != -ENODEV)
			return PTR_ERR(hipcie->vpcie);	/* PCI/NVMe: real regulator error blocks NVMe power-up */
		hipcie->vpcie = NULL;	/* PCI/NVMe: no regulator in DT; assume NVMe endpoint is always powered */
	}

	hipcie->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	ret = PTR_ERR_OR_ZERO(hipcie->reset_gpio);
	if (ret) {
		dev_err(dev, "unable to request reset gpio: %d\n", ret);
		return ret;	/* PCI/NVMe: PERST# GPIO request failed; NVMe device cannot be reset cleanly */
	}

	ret = gpiod_set_consumer_name(hipcie->reset_gpio,
				      "PCIe device power control");
	if (ret) {
		dev_err(dev, "unable to set reset gpio name: %d\n", ret);
		return ret;	/* PCI/NVMe: diagnostic label setup failed; non-fatal but logged */
	}

	hipcie->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(hipcie->aux_clk)) {
		dev_err(dev, "Failed to get PCIe aux clk\n");
		return PTR_ERR(hipcie->aux_clk);	/* PCI/NVMe: aux clock missing; DWC core cannot run NVMe path */
	}

	hipcie->pipe_clk = devm_clk_get(dev, "pipe");
	if (IS_ERR(hipcie->pipe_clk)) {
		dev_err(dev, "Failed to get PCIe pipe clk\n");
		return PTR_ERR(hipcie->pipe_clk);	/* PCI/NVMe: pipe clock missing; PHY cannot lock for NVMe link */
	}

	hipcie->sys_clk = devm_clk_get(dev, "sys");
	if (IS_ERR(hipcie->sys_clk)) {
		dev_err(dev, "Failed to get PCIEe sys clk\n");
		return PTR_ERR(hipcie->sys_clk);	/* PCI/NVMe: sys clock missing; transaction layer/DMA for NVMe fails */
	}

	hipcie->bus_clk = devm_clk_get(dev, "bus");
	if (IS_ERR(hipcie->bus_clk)) {
		dev_err(dev, "Failed to get PCIe bus clk\n");
		return PTR_ERR(hipcie->bus_clk);	/* PCI/NVMe: bus clock missing; AXI fabric cannot move NVMe MMIO/DMA */
	}

	hipcie->soft_reset = devm_reset_control_get(dev, "soft");
	if (IS_ERR(hipcie->soft_reset)) {
		dev_err(dev, "couldn't get soft reset\n");
		return PTR_ERR(hipcie->soft_reset);	/* PCI/NVMe: cannot reset DWC core; NVMe host init unsafe */
	}

	hipcie->sys_reset = devm_reset_control_get(dev, "sys");
	if (IS_ERR(hipcie->sys_reset)) {
		dev_err(dev, "couldn't get sys reset\n");
		return PTR_ERR(hipcie->sys_reset);	/* PCI/NVMe: cannot reset system/DMA path for NVMe */
	}

	hipcie->bus_reset = devm_reset_control_get(dev, "bus");
	if (IS_ERR(hipcie->bus_reset)) {
		dev_err(dev, "couldn't get bus reset\n");
		return PTR_ERR(hipcie->bus_reset);	/* PCI/NVMe: cannot reset bus fabric for NVMe traffic */
	}

	hipcie->phy = devm_phy_get(dev, "phy");
	if (IS_ERR(hipcie->phy)) {
		dev_info(dev, "no pcie-phy found\n");
		hipcie->phy = NULL;
		/* fall through here!
		 * if no pcie-phy found, phy init
		 * should be done under boot!
		 */
	} else {
		phy_init(hipcie->phy);	/* PCI/NVMe: initialize SERDES PHY so NVMe link can train at target speed */
	}

	pp->ops = &histb_pcie_host_ops;	/* PCI/NVMe: register host init callback used by dw_pcie_host_init() before NVMe enumeration */

	platform_set_drvdata(pdev, hipcie);	/* PCI/NVMe: store wrapper for remove/shutdown; also used by to_histb_pcie() */

	ret = histb_pcie_host_enable(pp);
	if (ret) {
		dev_err(dev, "failed to enable host\n");
		goto err_exit_phy;	/* PCI/NVMe: clocks/resets failed; cannot bring up NVMe link */
	}

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		goto err_exit_phy;	/* PCI/NVMe: DWC host init failed; PCI bus never enumerates NVMe device */
	}

	return 0;	/* PCI/NVMe: host ready; PCI core will now scan bus and may bind nvme-pci driver */

err_exit_phy:
	phy_exit(hipcie->phy);	/* PCI/NVMe: power down PHY on failure so NVMe link stays quiet */

	return ret;	/* PCI/NVMe: propagate probe error so no NVMe device is exposed */
}

static void histb_pcie_remove(struct platform_device *pdev)
{
	struct histb_pcie *hipcie = platform_get_drvdata(pdev);	/* PCI/NVMe: retrieve wrapper before tearing down NVMe host */

	histb_pcie_host_disable(hipcie);	/* PCI/NVMe: disable clocks/resets/regulator; powers off NVMe link */

	phy_exit(hipcie->phy);	/* PCI/NVMe: shut down PHY after NVMe endpoint is reset and powered down */
}

static const struct of_device_id histb_pcie_of_match[] = {
	{ .compatible = "hisilicon,hi3798cv200-pcie", },	/* PCI/NVMe: match DT node for this RC; child NVMe devices are discovered after probe */
	{},
};
MODULE_DEVICE_TABLE(of, histb_pcie_of_match);

static struct platform_driver histb_pcie_platform_driver = {
	.probe	= histb_pcie_probe,	/* PCI/NVMe: instantiate RC; later PCI scan may attach nvme-pci to downstream endpoint */
	.remove = histb_pcie_remove,	/* PCI/NVMe: remove RC; resets/PERST# stop NVMe device */
	.driver = {
		.name = "histb-pcie",
		.of_match_table = histb_pcie_of_match,
	},
};
module_platform_driver(histb_pcie_platform_driver);	/* PCI/NVMe: register driver; on match, enables the PCIe root port that can host NVMe SSDs */

MODULE_DESCRIPTION("HiSilicon STB PCIe host controller driver");
