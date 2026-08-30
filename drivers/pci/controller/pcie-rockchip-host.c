// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip AXI PCIe host controller driver
 *
 * Copyright (c) 2016 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *         Wenrui Li <wenrui.li@rock-chips.com>
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * ARM PCI Host generic driver.
 */

#include <linux/bitfield.h>   /* NVMe: include linux/bitfield.h */
#include <linux/bitrev.h>   /* NVMe: include linux/bitrev.h */
#include <linux/gpio/consumer.h>   /* NVMe: include linux/gpio/consumer.h */
#include <linux/interrupt.h>   /* NVMe: include linux/interrupt.h */
#include <linux/iopoll.h>   /* NVMe: include linux/iopoll.h */
#include <linux/irq.h>   /* NVMe: include linux/irq.h */
#include <linux/irqchip/chained_irq.h>   /* NVMe: include linux/irqchip/chained_irq.h */
#include <linux/irqdomain.h>   /* NVMe: include linux/irqdomain.h */
#include <linux/module.h>   /* NVMe: include linux/module.h */
#include <linux/of.h>   /* NVMe: include linux/of.h */
#include <linux/of_pci.h>   /* NVMe: include linux/of_pci.h */
#include <linux/phy/phy.h>   /* NVMe: include linux/phy/phy.h */
#include <linux/platform_device.h>   /* NVMe: include linux/platform_device.h */

#include "../pci.h"   /* NVMe: include Rockchip/PCI header */
#include "pcie-rockchip.h"   /* NVMe: include Rockchip/PCI header */

static void rockchip_pcie_enable_bw_int(struct rockchip_pcie *rockchip)   /* PCI/NVMe: enable link bandwidth change interrupts for NVMe link monitoring */
{   /* NVMe: begin block */
	u32 status;   /* NVMe: temp register value */

	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: read controller register */
	status |= (PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: write controller register */
}   /* NVMe: end of block */

static void rockchip_pcie_clr_bw_int(struct rockchip_pcie *rockchip)   /* PCI/NVMe: clear bandwidth status bits after link event */
{   /* NVMe: begin block */
	u32 status;   /* NVMe: temp register value */

	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: read controller register */
	status |= (PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS) << 16;   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: write controller register */
}   /* NVMe: end of block */

static void rockchip_pcie_update_txcredit_mui(struct rockchip_pcie *rockchip)   /* PCI/NVMe: update TX credit max update interval for ASPM L1s */
{   /* NVMe: begin block */
	u32 val;   /* NVMe: temporary value */

	/* Update Tx credit maximum update interval */
	val = rockchip_pcie_read(rockchip, PCIE_CORE_TXCREDIT_CFG1);   /* NVMe: read controller register */
	val &= ~PCIE_CORE_TXCREDIT_CFG1_MUI_MASK;   /* NVMe: update local variable */
	val |= PCIE_CORE_TXCREDIT_CFG1_MUI_ENCODE(24000);	/* ns */
	rockchip_pcie_write(rockchip, val, PCIE_CORE_TXCREDIT_CFG1);   /* NVMe: write controller register */
}   /* NVMe: end of block */

static int rockchip_pcie_valid_device(struct rockchip_pcie *rockchip,   /* NVMe: filter unreachable PCI devices during NVMe enumeration */
				      struct pci_bus *bus, int dev)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	/*
	 * Access only one slot on each root port.
	 * Do not read more than one device on the bus directly attached
	 * to RC's downstream side.
	 */
	if (pci_is_root_bus(bus) || pci_is_root_bus(bus->parent))   /* NVMe: check root bus */
		return dev == 0;   /* NVMe: allow/access check result */

	return 1;   /* NVMe: allow/access check result */
}   /* NVMe: end of block */

static u8 rockchip_pcie_lane_map(struct rockchip_pcie *rockchip)   /* NVMe: read negotiated lane map after link training */
{   /* NVMe: begin block */
	u32 val;   /* NVMe: temporary value */
	u8 map;   /* NVMe: mapped lane bitmask */

	if (rockchip->legacy_phy)   /* NVMe: old PHY path */
		return GENMASK(MAX_LANE_NUM - 1, 0);   /* NVMe: build bit mask */

	val = rockchip_pcie_read(rockchip, PCIE_CORE_LANE_MAP);   /* NVMe: read controller register */
	map = val & PCIE_CORE_LANE_MAP_MASK;   /* NVMe: update local variable */

	/* The link may be using a reverse-indexed mapping. */
	if (val & PCIE_CORE_LANE_MAP_REVERSE)   /* NVMe: conditional check */
		map = bitrev8(map) >> 4;   /* NVMe: update local variable */

	return map;   /* NVMe: return lane map */
}   /* NVMe: end of block */

static int rockchip_pcie_rd_own_conf(struct rockchip_pcie *rockchip,   /* NVMe: read root complex own PCI config space */
				     int where, int size, u32 *val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	void __iomem *addr;   /* NVMe: mapped register address */

	addr = rockchip->apb_base + PCIE_RC_CONFIG_NORMAL_BASE + where;   /* NVMe: update local variable */

	if (!IS_ALIGNED((uintptr_t)addr, size)) {   /* NVMe: check alignment */
		*val = 0;   /* NVMe: clear output value */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */
	}   /* NVMe: end of block */

	if (size == 4) {   /* NVMe: 32-bit access path */
		*val = readl(addr);   /* NVMe: MMIO register access */
	} else if (size == 2) {   /* NVMe: end of if, start else-if */
		*val = readw(addr);   /* NVMe: MMIO register access */
	} else if (size == 1) {   /* NVMe: end of if, start else-if */
		*val = readb(addr);   /* NVMe: MMIO register access */
	} else {   /* NVMe: end of if, start else */
		*val = 0;   /* NVMe: clear output value */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */
	}   /* NVMe: end of block */
	return PCIBIOS_SUCCESSFUL;   /* NVMe: return PCI BIOS status */
}   /* NVMe: end of block */

static int rockchip_pcie_wr_own_conf(struct rockchip_pcie *rockchip,   /* NVMe: write root complex own PCI config space */
				     int where, int size, u32 val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	u32 mask, tmp, offset;   /* NVMe: RMW temporaries */
	void __iomem *addr;   /* NVMe: mapped register address */

	offset = where & ~0x3;   /* NVMe: update local variable */
	addr = rockchip->apb_base + PCIE_RC_CONFIG_NORMAL_BASE + offset;   /* NVMe: update local variable */

	if (size == 4) {   /* NVMe: 32-bit access path */
		writel(val, addr);   /* NVMe: MMIO register access */
		return PCIBIOS_SUCCESSFUL;   /* NVMe: return PCI BIOS status */
	}   /* NVMe: end of block */

	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));   /* NVMe: update local variable */

	/*
	 * N.B. This read/modify/write isn't safe in general because it can
	 * corrupt RW1C bits in adjacent registers.  But the hardware
	 * doesn't support smaller writes.
	 */
	tmp = readl(addr) & mask;   /* NVMe: MMIO register access */
	tmp |= val << ((where & 0x3) * 8);   /* NVMe: update local variable */
	writel(tmp, addr);   /* NVMe: MMIO register access */

	return PCIBIOS_SUCCESSFUL;   /* NVMe: return PCI BIOS status */
}   /* NVMe: end of block */

static int rockchip_pcie_rd_other_conf(struct rockchip_pcie *rockchip,   /* NVMe: read downstream device config space (NVMe SSD) */
				       struct pci_bus *bus, u32 devfn,   /* NVMe: function parameter */
				       int where, int size, u32 *val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	void __iomem *addr;   /* NVMe: mapped register address */

	addr = rockchip->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);   /* NVMe: update local variable */

	if (!IS_ALIGNED((uintptr_t)addr, size)) {   /* NVMe: check alignment */
		*val = 0;   /* NVMe: clear output value */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */
	}   /* NVMe: end of block */

	if (pci_is_root_bus(bus->parent))   /* NVMe: check root bus */
		rockchip_pcie_cfg_configuration_accesses(rockchip,   /* NVMe: select CFG access type */
						AXI_WRAPPER_TYPE0_CFG);   /* NVMe: host controller operation */
	else   /* NVMe: alternative branch */
		rockchip_pcie_cfg_configuration_accesses(rockchip,   /* NVMe: select CFG access type */
						AXI_WRAPPER_TYPE1_CFG);   /* NVMe: host controller operation */

	if (size == 4) {   /* NVMe: 32-bit access path */
		*val = readl(addr);   /* NVMe: MMIO register access */
	} else if (size == 2) {   /* NVMe: end of if, start else-if */
		*val = readw(addr);   /* NVMe: MMIO register access */
	} else if (size == 1) {   /* NVMe: end of if, start else-if */
		*val = readb(addr);   /* NVMe: MMIO register access */
	} else {   /* NVMe: end of if, start else */
		*val = 0;   /* NVMe: clear output value */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */
	}   /* NVMe: end of block */
	return PCIBIOS_SUCCESSFUL;   /* NVMe: return PCI BIOS status */
}   /* NVMe: end of block */

static int rockchip_pcie_wr_other_conf(struct rockchip_pcie *rockchip,   /* NVMe: write downstream device config space (NVMe SSD) */
				       struct pci_bus *bus, u32 devfn,   /* NVMe: function parameter */
				       int where, int size, u32 val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	void __iomem *addr;   /* NVMe: mapped register address */

	addr = rockchip->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);   /* NVMe: update local variable */

	if (!IS_ALIGNED((uintptr_t)addr, size))   /* NVMe: check alignment */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */

	if (pci_is_root_bus(bus->parent))   /* NVMe: check root bus */
		rockchip_pcie_cfg_configuration_accesses(rockchip,   /* NVMe: select CFG access type */
						AXI_WRAPPER_TYPE0_CFG);   /* NVMe: host controller operation */
	else   /* NVMe: alternative branch */
		rockchip_pcie_cfg_configuration_accesses(rockchip,   /* NVMe: select CFG access type */
						AXI_WRAPPER_TYPE1_CFG);   /* NVMe: host controller operation */

	if (size == 4)   /* NVMe: 32-bit access path */
		writel(val, addr);   /* NVMe: MMIO register access */
	else if (size == 2)   /* NVMe: 16-bit access path */
		writew(val, addr);   /* NVMe: MMIO register access */
	else if (size == 1)   /* NVMe: 8-bit access path */
		writeb(val, addr);   /* NVMe: MMIO register access */
	else   /* NVMe: alternative branch */
		return PCIBIOS_BAD_REGISTER_NUMBER;   /* NVMe: return PCI BIOS status */

	return PCIBIOS_SUCCESSFUL;   /* NVMe: return PCI BIOS status */
}   /* NVMe: end of block */

static int rockchip_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,   /* NVMe: pci_ops read callback used by PCI core to probe NVMe */
				 int size, u32 *val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = bus->sysdata;   /* NVMe: host private from bus sysdata */

	if (!rockchip_pcie_valid_device(rockchip, bus, PCI_SLOT(devfn)))   /* NVMe: extract PCI slot number */
		return PCIBIOS_DEVICE_NOT_FOUND;   /* NVMe: return PCI BIOS status */

	if (pci_is_root_bus(bus))   /* NVMe: check root bus */
		return rockchip_pcie_rd_own_conf(rockchip, where, size, val);   /* NVMe: read RC config */

	return rockchip_pcie_rd_other_conf(rockchip, bus, devfn, where, size,   /* NVMe: read device config */
					   val);   /* NVMe: host controller operation */
}   /* NVMe: end of block */

static int rockchip_pcie_wr_conf(struct pci_bus *bus, u32 devfn,   /* NVMe: pci_ops write callback used to configure NVMe BARs */
				 int where, int size, u32 val)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = bus->sysdata;   /* NVMe: host private from bus sysdata */

	if (!rockchip_pcie_valid_device(rockchip, bus, PCI_SLOT(devfn)))   /* NVMe: extract PCI slot number */
		return PCIBIOS_DEVICE_NOT_FOUND;   /* NVMe: return PCI BIOS status */

	if (pci_is_root_bus(bus))   /* NVMe: check root bus */
		return rockchip_pcie_wr_own_conf(rockchip, where, size, val);   /* NVMe: write RC config */

	return rockchip_pcie_wr_other_conf(rockchip, bus, devfn, where, size,   /* NVMe: write device config */
					   val);   /* NVMe: host controller operation */
}   /* NVMe: end of block */

static struct pci_ops rockchip_pcie_ops = {   /* NVMe: pci_ops for NVMe enumeration and BAR setup */
	.read = rockchip_pcie_rd_conf,   /* NVMe: host controller operation */
	.write = rockchip_pcie_wr_conf,   /* NVMe: host controller operation */
};   /* NVMe: host controller operation */

static void rockchip_pcie_set_power_limit(struct rockchip_pcie *rockchip)   /* NVMe: set slot power limit for NVMe SSD */
{   /* NVMe: begin block */
	int curr;   /* NVMe: regulator current limit */
	u32 status, scale, power;   /* NVMe: function parameter */

	if (IS_ERR(rockchip->vpcie3v3))   /* NVMe: check pointer/error */
		return;   /* NVMe: host controller operation */

	/*
	 * Set RC's captured slot power limit and scale if
	 * vpcie3v3 available. The default values are both zero
	 * which means the software should set these two according
	 * to the actual power supply.
	 */
	curr = regulator_get_current_limit(rockchip->vpcie3v3);   /* NVMe: manage PCIe power rail */
	if (curr <= 0)   /* NVMe: check valid current */
		return;   /* NVMe: host controller operation */

	scale = 3; /* 0.001x */
	curr = curr / 1000; /* convert to mA */
	power = (curr * 3300) / 1000; /* milliwatt */
	while (power > FIELD_MAX(PCI_EXP_DEVCAP_PWR_VAL)) {   /* NVMe: max bitfield value */
		if (!scale) {   /* NVMe: check pointer/resource present */
			dev_warn(rockchip->dev, "invalid power supply\n");   /* NVMe: log warning */
			return;   /* NVMe: host controller operation */
		}   /* NVMe: end of block */
		scale--;   /* NVMe: host controller operation */
		power = power / 10;   /* NVMe: update local variable */
	}   /* NVMe: end of block */

	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCAP);   /* NVMe: read controller register */
	status |= FIELD_PREP(PCI_EXP_DEVCAP_PWR_VAL, power);   /* NVMe: prepare bitfield value */
	status |= FIELD_PREP(PCI_EXP_DEVCAP_PWR_SCL, scale);   /* NVMe: prepare bitfield value */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCAP);   /* NVMe: write controller register */
}   /* NVMe: end of block */

/**
 * rockchip_pcie_host_init_port - Initialize hardware
 * @rockchip: PCIe port information
 */
static int rockchip_pcie_host_init_port(struct rockchip_pcie *rockchip)   /* NVMe: initialize PCIe RC and train link for NVMe */
{   /* NVMe: begin block */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	int err, i = MAX_LANE_NUM;   /* NVMe: error code and lane counter */
	u32 status;   /* NVMe: temp register value */

	gpiod_set_value_cansleep(rockchip->perst_gpio, 0);   /* NVMe: assert/deassert PERST# to NVMe device */

	err = rockchip_pcie_init_port(rockchip);   /* NVMe: init PHY/clock/reset */
	if (err)   /* NVMe: check error code */
		return err;   /* NVMe: propagate error */

	/* Fix the transmitted FTS count desired to exit from L0s. */
	status = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL_PLC1);   /* NVMe: read controller register */
	status = (status & ~PCIE_CORE_CTRL_PLC1_FTS_MASK) |   /* NVMe: update local variable */
		 (PCIE_CORE_CTRL_PLC1_FTS_CNT << PCIE_CORE_CTRL_PLC1_FTS_SHIFT);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, status, PCIE_CORE_CTRL_PLC1);   /* NVMe: write controller register */

	rockchip_pcie_set_power_limit(rockchip);   /* NVMe: set slot power limit */

	/* Set RC's clock architecture as common clock */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: read controller register */
	status |= PCI_EXP_LNKSTA_SLC << 16;   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: write controller register */

	/* Set RC's RCB to 128 */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: read controller register */
	status |= PCI_EXP_LNKCTL_RCB;   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: write controller register */

	/* Enable Gen1 training */
	rockchip_pcie_write(rockchip, PCIE_CLIENT_LINK_TRAIN_ENABLE,   /* NVMe: write controller register */
			    PCIE_CLIENT_CONFIG);   /* NVMe: host controller operation */

	msleep(PCIE_T_PVPERL_MS);   /* NVMe: wait for NVMe/PHY timing */
	gpiod_set_value_cansleep(rockchip->perst_gpio, 1);   /* NVMe: assert/deassert PERST# to NVMe device */

	msleep(PCIE_RESET_CONFIG_WAIT_MS);   /* NVMe: wait for NVMe/PHY timing */

	/* 500ms timeout value should be enough for Gen1/2 training */
	err = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_BASIC_STATUS1,   /* NVMe: poll hardware status */
				 status, PCIE_LINK_UP(status), 20,   /* NVMe: host controller operation */
				 500 * USEC_PER_MSEC);   /* NVMe: host controller operation */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "PCIe link training gen1 timeout!\n");   /* NVMe: log error */
		goto err_power_off_phy;   /* NVMe: jump to error path */
	}   /* NVMe: end of block */

	if (rockchip->link_gen == 2) {   /* NVMe: Gen2 requested */
		/*
		 * Enable retrain for gen2. This should be configured only after
		 * gen1 finished.
		 */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL2);   /* NVMe: read controller register */
		status &= ~PCI_EXP_LNKCTL2_TLS;   /* NVMe: update local variable */
		status |= PCI_EXP_LNKCTL2_TLS_5_0GT;   /* NVMe: update local variable */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL2);   /* NVMe: write controller register */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: read controller register */
		status |= PCI_EXP_LNKCTL_RL;   /* NVMe: update local variable */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);   /* NVMe: write controller register */

		err = readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,   /* NVMe: poll hardware status */
					 status, PCIE_LINK_IS_GEN2(status), 20,   /* NVMe: host controller operation */
					 500 * USEC_PER_MSEC);   /* NVMe: host controller operation */
		if (err)   /* NVMe: check error code */
			dev_dbg(dev, "PCIe link training gen2 timeout, fall back to gen1!\n");   /* NVMe: log debug */
	}   /* NVMe: end of block */

	/* Check the final link width from negotiated lane counter from MGMT */
	status = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL);   /* NVMe: read controller register */
	status = 0x1 << ((status & PCIE_CORE_PL_CONF_LANE_MASK) >>   /* NVMe: update local variable */
			  PCIE_CORE_PL_CONF_LANE_SHIFT);   /* NVMe: host controller operation */
	dev_dbg(dev, "current link width is x%d\n", status);   /* NVMe: log debug */

	/* Power off unused lane(s) */
	rockchip->lanes_map = rockchip_pcie_lane_map(rockchip);   /* NVMe: read active lane map */
	for (i = 0; i < MAX_LANE_NUM; i++) {   /* NVMe: iterate lanes */
		if (!(rockchip->lanes_map & BIT(i))) {   /* NVMe: single bit mask */
			dev_dbg(dev, "idling lane %d\n", i);   /* NVMe: log debug */
			phy_power_off(rockchip->phys[i]);   /* NVMe: power down unused PHY lane */
		}   /* NVMe: end of block */
	}   /* NVMe: end of block */

	rockchip_pcie_write(rockchip, PCI_VENDOR_ID_ROCKCHIP,   /* NVMe: write controller register */
			    PCIE_CORE_CONFIG_VENDOR);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip,   /* NVMe: write controller register */
			    PCI_CLASS_BRIDGE_PCI_NORMAL << 8,   /* NVMe: host controller operation */
			    PCIE_RC_CONFIG_RID_CCR);   /* NVMe: host controller operation */

	/* Clear THP cap's next cap pointer to remove L1 substate cap */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_THP_CAP);   /* NVMe: read controller register */
	status &= ~PCIE_RC_CONFIG_THP_CAP_NEXT_MASK;   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_THP_CAP);   /* NVMe: write controller register */

	/* Clear L0s from RC's link cap */
	if (of_property_read_bool(dev->of_node, "aspm-no-l0s")) {   /* NVMe: read DT property */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCAP);   /* NVMe: read controller register */
		status &= ~PCI_EXP_LNKCAP_ASPM_L0S;   /* NVMe: update local variable */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCAP);   /* NVMe: write controller register */
	}   /* NVMe: end of block */

	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCTL);   /* NVMe: read controller register */
	status &= ~PCI_EXP_DEVCTL_PAYLOAD;   /* NVMe: update local variable */
	status |= PCI_EXP_DEVCTL_PAYLOAD_256B;   /* NVMe: update local variable */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCTL);   /* NVMe: write controller register */

	return 0;   /* NVMe: success */
err_power_off_phy:   /* NVMe: host controller operation */
	while (i--)   /* NVMe: iterate lanes */
		phy_power_off(rockchip->phys[i]);   /* NVMe: power down unused PHY lane */
	i = MAX_LANE_NUM;   /* NVMe: update local variable */
	while (i--)   /* NVMe: iterate lanes */
		phy_exit(rockchip->phys[i]);   /* NVMe: release PHY */
	return err;   /* NVMe: propagate error */
}   /* NVMe: end of block */

static irqreturn_t rockchip_pcie_subsys_irq_handler(int irq, void *arg)   /* NVMe: subsystem IRQ handler (link/parity errors) */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = arg;   /* NVMe: host private from handler arg */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	u32 reg;   /* NVMe: interrupt status register */
	u32 sub_reg;   /* NVMe: core interrupt status register */

	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);   /* NVMe: read controller register */
	if (reg & PCIE_CLIENT_INT_LOCAL) {   /* NVMe: conditional check */
		dev_dbg(dev, "local interrupt received\n");   /* NVMe: log debug */
		sub_reg = rockchip_pcie_read(rockchip, PCIE_CORE_INT_STATUS);   /* NVMe: read controller register */
		if (sub_reg & PCIE_CORE_INT_PRFPE)   /* NVMe: conditional check */
			dev_dbg(dev, "parity error detected while reading from the PNP receive FIFO RAM\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_CRFPE)   /* NVMe: conditional check */
			dev_dbg(dev, "parity error detected while reading from the Completion Receive FIFO RAM\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_RRPE)   /* NVMe: conditional check */
			dev_dbg(dev, "parity error detected while reading from replay buffer RAM\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_PRFO)   /* NVMe: conditional check */
			dev_dbg(dev, "overflow occurred in the PNP receive FIFO\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_CRFO)   /* NVMe: conditional check */
			dev_dbg(dev, "overflow occurred in the completion receive FIFO\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_RT)   /* NVMe: conditional check */
			dev_dbg(dev, "replay timer timed out\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_RTR)   /* NVMe: conditional check */
			dev_dbg(dev, "replay timer rolled over after 4 transmissions of the same TLP\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_PE)   /* NVMe: conditional check */
			dev_dbg(dev, "phy error detected on receive side\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_MTR)   /* NVMe: conditional check */
			dev_dbg(dev, "malformed TLP received from the link\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_UCR)   /* NVMe: conditional check */
			dev_dbg(dev, "Unexpected Completion received from the link\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_FCE)   /* NVMe: conditional check */
			dev_dbg(dev, "an error was observed in the flow control advertisements from the other side\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_CT)   /* NVMe: conditional check */
			dev_dbg(dev, "a request timed out waiting for completion\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_UTC)   /* NVMe: conditional check */
			dev_dbg(dev, "unmapped TC error\n");   /* NVMe: log debug */

		if (sub_reg & PCIE_CORE_INT_MMVC)   /* NVMe: conditional check */
			dev_dbg(dev, "MSI mask register changes\n");   /* NVMe: log debug */

		rockchip_pcie_write(rockchip, sub_reg, PCIE_CORE_INT_STATUS);   /* NVMe: write controller register */
	} else if (reg & PCIE_CLIENT_INT_PHY) {   /* NVMe: end of if, start else-if */
		dev_dbg(dev, "phy link changes\n");   /* NVMe: log debug */
		rockchip_pcie_update_txcredit_mui(rockchip);   /* NVMe: update TX credit MUI */
		rockchip_pcie_clr_bw_int(rockchip);   /* NVMe: clear bandwidth status */
	}   /* NVMe: end of block */

	rockchip_pcie_write(rockchip, reg & PCIE_CLIENT_INT_LOCAL,   /* NVMe: write controller register */
			    PCIE_CLIENT_INT_STATUS);   /* NVMe: host controller operation */

	return IRQ_HANDLED;   /* NVMe: IRQ handled */
}   /* NVMe: end of block */

static irqreturn_t rockchip_pcie_client_irq_handler(int irq, void *arg)   /* NVMe: client IRQ handler (legacy/PM/AER events) */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = arg;   /* NVMe: host private from handler arg */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	u32 reg;   /* NVMe: interrupt status register */

	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);   /* NVMe: read controller register */
	if (reg & PCIE_CLIENT_INT_LEGACY_DONE)   /* NVMe: conditional check */
		dev_dbg(dev, "legacy done interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_MSG)   /* NVMe: conditional check */
		dev_dbg(dev, "message done interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_HOT_RST)   /* NVMe: conditional check */
		dev_dbg(dev, "hot reset interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_DPA)   /* NVMe: conditional check */
		dev_dbg(dev, "dpa interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_FATAL_ERR)   /* NVMe: conditional check */
		dev_dbg(dev, "fatal error interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_NFATAL_ERR)   /* NVMe: conditional check */
		dev_dbg(dev, "non fatal error interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_CORR_ERR)   /* NVMe: conditional check */
		dev_dbg(dev, "correctable error interrupt received\n");   /* NVMe: log debug */

	if (reg & PCIE_CLIENT_INT_PHY)   /* NVMe: conditional check */
		dev_dbg(dev, "phy interrupt received\n");   /* NVMe: log debug */

	rockchip_pcie_write(rockchip, reg & (PCIE_CLIENT_INT_LEGACY_DONE |   /* NVMe: write controller register */
			      PCIE_CLIENT_INT_MSG | PCIE_CLIENT_INT_HOT_RST |   /* NVMe: host controller operation */
			      PCIE_CLIENT_INT_DPA | PCIE_CLIENT_INT_FATAL_ERR |   /* NVMe: host controller operation */
			      PCIE_CLIENT_INT_NFATAL_ERR |   /* NVMe: host controller operation */
			      PCIE_CLIENT_INT_CORR_ERR |   /* NVMe: host controller operation */
			      PCIE_CLIENT_INT_PHY),   /* NVMe: host controller operation */
		   PCIE_CLIENT_INT_STATUS);   /* NVMe: host controller operation */

	return IRQ_HANDLED;   /* NVMe: IRQ handled */
}   /* NVMe: end of block */

static void rockchip_pcie_intx_handler(struct irq_desc *desc)   /* NVMe: INTx chained IRQ handler for legacy NVMe interrupts */
{   /* NVMe: begin block */
	struct irq_chip *chip = irq_desc_get_chip(desc);   /* NVMe: parent irq_chip */
	struct rockchip_pcie *rockchip = irq_desc_get_handler_data(desc);   /* NVMe: function parameter */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	u32 reg;   /* NVMe: interrupt status register */
	u32 hwirq;   /* NVMe: hardware IRQ line */
	int ret;   /* NVMe: return value */

	chained_irq_enter(chip, desc);   /* NVMe: enter chained IRQ */

	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);   /* NVMe: read controller register */
	reg = (reg & PCIE_CLIENT_INTR_MASK) >> PCIE_CLIENT_INTR_SHIFT;   /* NVMe: update local variable */

	while (reg) {   /* NVMe: process asserted INTx bits */
		hwirq = ffs(reg) - 1;   /* NVMe: find first set bit */
		reg &= ~BIT(hwirq);   /* NVMe: single bit mask */

		ret = generic_handle_domain_irq(rockchip->irq_domain, hwirq);   /* NVMe: dispatch INTx to Linux IRQ */
		if (ret)   /* NVMe: check return value */
			dev_err(dev, "unexpected IRQ, INT%d\n", hwirq);   /* NVMe: log error */
	}   /* NVMe: end of block */

	chained_irq_exit(chip, desc);   /* NVMe: exit chained IRQ */
}   /* NVMe: end of block */

static int rockchip_pcie_setup_irq(struct rockchip_pcie *rockchip)   /* NVMe: request system/legacy/client IRQs */
{   /* NVMe: begin block */
	int irq, err;   /* NVMe: function parameter */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	struct platform_device *pdev = to_platform_device(dev);   /* NVMe: platform device */

	irq = platform_get_irq_byname(pdev, "sys");   /* NVMe: get platform IRQ from DT */
	if (irq < 0)   /* NVMe: conditional check */
		return irq;   /* NVMe: return IRQ number */

	err = devm_request_irq(dev, irq, rockchip_pcie_subsys_irq_handler,   /* NVMe: register IRQ handler */
			       IRQF_SHARED, "pcie-sys", rockchip);   /* NVMe: host controller operation */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "failed to request PCIe subsystem IRQ\n");   /* NVMe: log error */
		return err;   /* NVMe: propagate error */
	}   /* NVMe: end of block */

	irq = platform_get_irq_byname(pdev, "legacy");   /* NVMe: get platform IRQ from DT */
	if (irq < 0)   /* NVMe: conditional check */
		return irq;   /* NVMe: return IRQ number */

	irq_set_chained_handler_and_data(irq,   /* NVMe: IRQ setup */
					 rockchip_pcie_intx_handler,   /* NVMe: host controller operation */
					 rockchip);   /* NVMe: host controller operation */

	irq = platform_get_irq_byname(pdev, "client");   /* NVMe: get platform IRQ from DT */
	if (irq < 0)   /* NVMe: conditional check */
		return irq;   /* NVMe: return IRQ number */

	err = devm_request_irq(dev, irq, rockchip_pcie_client_irq_handler,   /* NVMe: register IRQ handler */
			       IRQF_SHARED, "pcie-client", rockchip);   /* NVMe: host controller operation */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "failed to request PCIe client IRQ\n");   /* NVMe: log error */
		return err;   /* NVMe: propagate error */
	}   /* NVMe: end of block */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

/**
 * rockchip_pcie_parse_host_dt - Parse Device Tree
 * @rockchip: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
static int rockchip_pcie_parse_host_dt(struct rockchip_pcie *rockchip)   /* NVMe: parse DT and regulators for NVMe host */
{   /* NVMe: begin block */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	int err;   /* NVMe: error code */

	err = rockchip_pcie_parse_dt(rockchip);   /* NVMe: parse common DT */
	if (err)   /* NVMe: check error code */
		return err;   /* NVMe: propagate error */

	rockchip->vpcie12v = devm_regulator_get_optional(dev, "vpcie12v");   /* NVMe: host controller operation */
	if (IS_ERR(rockchip->vpcie12v)) {   /* NVMe: check pointer/error */
		if (PTR_ERR(rockchip->vpcie12v) != -ENODEV)   /* NVMe: extract error code */
			return PTR_ERR(rockchip->vpcie12v);   /* NVMe: extract error code */
		dev_info(dev, "no vpcie12v regulator found\n");   /* NVMe: log info */
	}   /* NVMe: end of block */

	rockchip->vpcie3v3 = devm_regulator_get_optional(dev, "vpcie3v3");   /* NVMe: host controller operation */
	if (IS_ERR(rockchip->vpcie3v3)) {   /* NVMe: check pointer/error */
		if (PTR_ERR(rockchip->vpcie3v3) != -ENODEV)   /* NVMe: extract error code */
			return PTR_ERR(rockchip->vpcie3v3);   /* NVMe: extract error code */
		dev_info(dev, "no vpcie3v3 regulator found\n");   /* NVMe: log info */
	}   /* NVMe: end of block */

	rockchip->vpcie1v8 = devm_regulator_get(dev, "vpcie1v8");   /* NVMe: host controller operation */
	if (IS_ERR(rockchip->vpcie1v8))   /* NVMe: check pointer/error */
		return PTR_ERR(rockchip->vpcie1v8);   /* NVMe: extract error code */

	rockchip->vpcie0v9 = devm_regulator_get(dev, "vpcie0v9");   /* NVMe: host controller operation */
	if (IS_ERR(rockchip->vpcie0v9))   /* NVMe: check pointer/error */
		return PTR_ERR(rockchip->vpcie0v9);   /* NVMe: extract error code */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static int rockchip_pcie_set_vpcie(struct rockchip_pcie *rockchip)   /* NVMe: enable PCIe voltage regulators */
{   /* NVMe: begin block */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	int err;   /* NVMe: error code */

	if (!IS_ERR(rockchip->vpcie12v)) {   /* NVMe: check pointer/error */
		err = regulator_enable(rockchip->vpcie12v);   /* NVMe: manage PCIe power rail */
		if (err) {   /* NVMe: check error code */
			dev_err(dev, "fail to enable vpcie12v regulator\n");   /* NVMe: log error */
			goto err_out;   /* NVMe: jump to error path */
		}   /* NVMe: end of block */
	}   /* NVMe: end of block */

	if (!IS_ERR(rockchip->vpcie3v3)) {   /* NVMe: check pointer/error */
		err = regulator_enable(rockchip->vpcie3v3);   /* NVMe: manage PCIe power rail */
		if (err) {   /* NVMe: check error code */
			dev_err(dev, "fail to enable vpcie3v3 regulator\n");   /* NVMe: log error */
			goto err_disable_12v;   /* NVMe: jump to error path */
		}   /* NVMe: end of block */
	}   /* NVMe: end of block */

	err = regulator_enable(rockchip->vpcie1v8);   /* NVMe: manage PCIe power rail */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "fail to enable vpcie1v8 regulator\n");   /* NVMe: log error */
		goto err_disable_3v3;   /* NVMe: jump to error path */
	}   /* NVMe: end of block */

	err = regulator_enable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "fail to enable vpcie0v9 regulator\n");   /* NVMe: log error */
		goto err_disable_1v8;   /* NVMe: jump to error path */
	}   /* NVMe: end of block */

	return 0;   /* NVMe: success */

err_disable_1v8:   /* NVMe: host controller operation */
	regulator_disable(rockchip->vpcie1v8);   /* NVMe: manage PCIe power rail */
err_disable_3v3:   /* NVMe: host controller operation */
	if (!IS_ERR(rockchip->vpcie3v3))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie3v3);   /* NVMe: manage PCIe power rail */
err_disable_12v:   /* NVMe: host controller operation */
	if (!IS_ERR(rockchip->vpcie12v))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie12v);   /* NVMe: manage PCIe power rail */
err_out:   /* NVMe: host controller operation */
	return err;   /* NVMe: propagate error */
}   /* NVMe: end of block */

static void rockchip_pcie_enable_interrupts(struct rockchip_pcie *rockchip)   /* NVMe: unmask PCIe client/core interrupts */
{   /* NVMe: begin block */
	rockchip_pcie_write(rockchip, (PCIE_CLIENT_INT_CLI << 16) &   /* NVMe: write controller register */
			    (~PCIE_CLIENT_INT_CLI), PCIE_CLIENT_INT_MASK);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, (u32)(~PCIE_CORE_INT),   /* NVMe: write controller register */
			    PCIE_CORE_INT_MASK);   /* NVMe: host controller operation */

	rockchip_pcie_enable_bw_int(rockchip);   /* NVMe: enable bandwidth interrupts */
}   /* NVMe: end of block */

static int rockchip_pcie_intx_map(struct irq_domain *domain, unsigned int irq,   /* NVMe: map INTx hwirq to Linux irq number */
				  irq_hw_number_t hwirq)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);   /* NVMe: IRQ setup */
	irq_set_chip_data(irq, domain->host_data);   /* NVMe: IRQ setup */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static const struct irq_domain_ops intx_domain_ops = {   /* NVMe: INTx irqdomain operations */
	.map = rockchip_pcie_intx_map,   /* NVMe: host controller operation */
};   /* NVMe: host controller operation */

static int rockchip_pcie_init_irq_domain(struct rockchip_pcie *rockchip)   /* NVMe: create INTx irqdomain for legacy interrupts */
{   /* NVMe: begin block */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	struct device_node *intc = of_get_next_child(dev->of_node, NULL);   /* NVMe: interrupt-controller DT node */

	if (!intc) {   /* NVMe: check pointer/resource present */
		dev_err(dev, "missing child interrupt-controller node\n");   /* NVMe: log error */
		return -EINVAL;   /* NVMe: return error code */
	}   /* NVMe: end of block */

	rockchip->irq_domain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,   /* NVMe: irqdomain operation */
							&intx_domain_ops, rockchip);   /* NVMe: host controller operation */
	of_node_put(intc);   /* NVMe: drop DT node reference */
	if (!rockchip->irq_domain) {   /* NVMe: conditional check */
		dev_err(dev, "failed to get a INTx IRQ domain\n");   /* NVMe: log error */
		return -EINVAL;   /* NVMe: return error code */
	}   /* NVMe: end of block */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static int rockchip_pcie_prog_ob_atu(struct rockchip_pcie *rockchip,   /* NVMe: program outbound ATU region for NVMe MMIO/IO */
				     int region_no, int type, u8 num_pass_bits,   /* NVMe: function parameter */
				     u32 lower_addr, u32 upper_addr)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	u32 ob_addr_0;   /* NVMe: outbound ATU addr0 temp */
	u32 ob_addr_1;   /* NVMe: outbound ATU addr1 temp */
	u32 ob_desc_0;   /* NVMe: outbound ATU desc0 temp */
	u32 aw_offset;   /* NVMe: ATU region register offset */

	if (region_no >= MAX_AXI_WRAPPER_REGION_NUM)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */
	if (num_pass_bits + 1 < 8)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */
	if (num_pass_bits > 63)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */
	if (region_no == 0) {   /* NVMe: conditional check */
		if (AXI_REGION_0_SIZE < (2ULL << num_pass_bits))   /* NVMe: conditional check */
			return -EINVAL;   /* NVMe: return error code */
	}   /* NVMe: end of block */
	if (region_no != 0) {   /* NVMe: conditional check */
		if (AXI_REGION_SIZE < (2ULL << num_pass_bits))   /* NVMe: conditional check */
			return -EINVAL;   /* NVMe: return error code */
	}   /* NVMe: end of block */

	aw_offset = (region_no << OB_REG_SIZE_SHIFT);   /* NVMe: update local variable */

	ob_addr_0 = num_pass_bits & PCIE_CORE_OB_REGION_ADDR0_NUM_BITS;   /* NVMe: update local variable */
	ob_addr_0 |= lower_addr & PCIE_CORE_OB_REGION_ADDR0_LO_ADDR;   /* NVMe: update local variable */
	ob_addr_1 = upper_addr;   /* NVMe: update local variable */
	ob_desc_0 = (1 << 23 | type);   /* NVMe: update local variable */

	rockchip_pcie_write(rockchip, ob_addr_0,   /* NVMe: write controller register */
			    PCIE_CORE_OB_REGION_ADDR0 + aw_offset);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, ob_addr_1,   /* NVMe: write controller register */
			    PCIE_CORE_OB_REGION_ADDR1 + aw_offset);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, ob_desc_0,   /* NVMe: write controller register */
			    PCIE_CORE_OB_REGION_DESC0 + aw_offset);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, 0,   /* NVMe: write controller register */
			    PCIE_CORE_OB_REGION_DESC1 + aw_offset);   /* NVMe: host controller operation */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static int rockchip_pcie_prog_ib_atu(struct rockchip_pcie *rockchip,   /* NVMe: program inbound ATU region for NVMe DMA */
				     int region_no, u8 num_pass_bits,   /* NVMe: function parameter */
				     u32 lower_addr, u32 upper_addr)   /* NVMe: function parameter */
{   /* NVMe: begin block */
	u32 ib_addr_0;   /* NVMe: inbound ATU addr0 temp */
	u32 ib_addr_1;   /* NVMe: inbound ATU addr1 temp */
	u32 aw_offset;   /* NVMe: ATU region register offset */

	if (region_no > MAX_AXI_IB_ROOTPORT_REGION_NUM)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */
	if (num_pass_bits + 1 < MIN_AXI_ADDR_BITS_PASSED)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */
	if (num_pass_bits > 63)   /* NVMe: conditional check */
		return -EINVAL;   /* NVMe: return error code */

	aw_offset = (region_no << IB_ROOT_PORT_REG_SIZE_SHIFT);   /* NVMe: update local variable */

	ib_addr_0 = num_pass_bits & PCIE_CORE_IB_REGION_ADDR0_NUM_BITS;   /* NVMe: update local variable */
	ib_addr_0 |= (lower_addr << 8) & PCIE_CORE_IB_REGION_ADDR0_LO_ADDR;   /* NVMe: update local variable */
	ib_addr_1 = upper_addr;   /* NVMe: update local variable */

	rockchip_pcie_write(rockchip, ib_addr_0, PCIE_RP_IB_ADDR0 + aw_offset);   /* NVMe: write controller register */
	rockchip_pcie_write(rockchip, ib_addr_1, PCIE_RP_IB_ADDR1 + aw_offset);   /* NVMe: write controller register */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static int rockchip_pcie_cfg_atu(struct rockchip_pcie *rockchip)   /* NVMe: configure all ATU windows for NVMe access */
{   /* NVMe: begin block */
	struct device *dev = rockchip->dev;   /* NVMe: device pointer */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rockchip);   /* NVMe: host bridge from private */
	struct resource_entry *entry;   /* NVMe: resource window entry */
	u64 pci_addr, size;   /* NVMe: PCI address and window size */
	int offset;   /* NVMe: IO region offset */
	int err;   /* NVMe: error code */
	int reg_no;   /* NVMe: ATU region index */

	rockchip_pcie_cfg_configuration_accesses(rockchip,   /* NVMe: select CFG access type */
						 AXI_WRAPPER_TYPE0_CFG);   /* NVMe: host controller operation */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_MEM);   /* NVMe: get first resource window */
	if (!entry)   /* NVMe: check pointer/resource present */
		return -ENODEV;   /* NVMe: return error code */

	size = resource_size(entry->res);   /* NVMe: compute resource size */
	pci_addr = entry->res->start - entry->offset;   /* NVMe: update local variable */
	rockchip->msg_bus_addr = pci_addr;   /* NVMe: host controller operation */

	for (reg_no = 0; reg_no < (size >> 20); reg_no++) {   /* NVMe: iterate 1MB ATU regions */
		err = rockchip_pcie_prog_ob_atu(rockchip, reg_no + 1,   /* NVMe: program outbound ATU */
						AXI_WRAPPER_MEM_WRITE,   /* NVMe: host controller operation */
						20 - 1,   /* NVMe: host controller operation */
						pci_addr + (reg_no << 20),   /* NVMe: host controller operation */
						0);   /* NVMe: host controller operation */
		if (err) {   /* NVMe: check error code */
			dev_err(dev, "program RC mem outbound ATU failed\n");   /* NVMe: log error */
			return err;   /* NVMe: propagate error */
		}   /* NVMe: end of block */
	}   /* NVMe: end of block */

	err = rockchip_pcie_prog_ib_atu(rockchip, 2, 32 - 1, 0x0, 0);   /* NVMe: program inbound ATU */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "program RC mem inbound ATU failed\n");   /* NVMe: log error */
		return err;   /* NVMe: propagate error */
	}   /* NVMe: end of block */

	entry = resource_list_first_type(&bridge->windows, IORESOURCE_IO);   /* NVMe: get first resource window */
	if (!entry)   /* NVMe: check pointer/resource present */
		return -ENODEV;   /* NVMe: return error code */

	/* store the register number offset to program RC io outbound ATU */
	offset = size >> 20;   /* NVMe: update local variable */

	size = resource_size(entry->res);   /* NVMe: compute resource size */
	pci_addr = entry->res->start - entry->offset;   /* NVMe: update local variable */

	for (reg_no = 0; reg_no < (size >> 20); reg_no++) {   /* NVMe: iterate 1MB ATU regions */
		err = rockchip_pcie_prog_ob_atu(rockchip,   /* NVMe: program outbound ATU */
						reg_no + 1 + offset,   /* NVMe: host controller operation */
						AXI_WRAPPER_IO_WRITE,   /* NVMe: host controller operation */
						20 - 1,   /* NVMe: host controller operation */
						pci_addr + (reg_no << 20),   /* NVMe: host controller operation */
						0);   /* NVMe: host controller operation */
		if (err) {   /* NVMe: check error code */
			dev_err(dev, "program RC io outbound ATU failed\n");   /* NVMe: log error */
			return err;   /* NVMe: propagate error */
		}   /* NVMe: end of block */
	}   /* NVMe: end of block */

	/* assign message regions */
	rockchip_pcie_prog_ob_atu(rockchip, reg_no + 1 + offset,   /* NVMe: program outbound ATU */
				  AXI_WRAPPER_NOR_MSG,   /* NVMe: host controller operation */
				  20 - 1, 0, 0);   /* NVMe: host controller operation */

	rockchip->msg_bus_addr += ((reg_no + offset) << 20);   /* NVMe: host controller operation */
	return err;   /* NVMe: propagate error */
}   /* NVMe: end of block */

static int rockchip_pcie_wait_l2(struct rockchip_pcie *rockchip)   /* NVMe: wait for link L2 entry during suspend */
{   /* NVMe: begin block */
	u32 value;   /* NVMe: polled register value */
	int err;   /* NVMe: error code */

	/* send PME_TURN_OFF message */
	writel(0x0, rockchip->msg_region + PCIE_RC_SEND_PME_OFF);   /* NVMe: MMIO register access */

	/* read LTSSM and wait for falling into L2 link state */
	err = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_DEBUG_OUT_0,   /* NVMe: poll hardware status */
				 value, PCIE_LINK_IS_L2(value), 20,   /* NVMe: host controller operation */
				 jiffies_to_usecs(5 * HZ));   /* NVMe: host controller operation */
	if (err) {   /* NVMe: check error code */
		dev_err(rockchip->dev, "PCIe link enter L2 timeout!\n");   /* NVMe: log error */
		return err;   /* NVMe: propagate error */
	}   /* NVMe: end of block */

	return 0;   /* NVMe: success */
}   /* NVMe: end of block */

static int rockchip_pcie_suspend_noirq(struct device *dev)   /* NVMe: noirq suspend for NVMe host */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);   /* NVMe: host private from driver data */
	int ret;   /* NVMe: return value */

	/* disable core and cli int since we don't need to ack PME_ACK */
	rockchip_pcie_write(rockchip, (PCIE_CLIENT_INT_CLI << 16) |   /* NVMe: write controller register */
			    PCIE_CLIENT_INT_CLI, PCIE_CLIENT_INT_MASK);   /* NVMe: host controller operation */
	rockchip_pcie_write(rockchip, (u32)PCIE_CORE_INT, PCIE_CORE_INT_MASK);   /* NVMe: write controller register */

	ret = rockchip_pcie_wait_l2(rockchip);   /* NVMe: wait for L2 link state */
	if (ret) {   /* NVMe: check return value */
		rockchip_pcie_enable_interrupts(rockchip);   /* NVMe: enable interrupts */
		return ret;   /* NVMe: return status */
	}   /* NVMe: end of block */

	rockchip_pcie_deinit_phys(rockchip);   /* NVMe: deinit PHYs */

	rockchip_pcie_disable_clocks(rockchip);   /* NVMe: disable controller clocks */

	regulator_disable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */

	return ret;   /* NVMe: return status */
}   /* NVMe: end of block */

static int rockchip_pcie_resume_noirq(struct device *dev)   /* NVMe: noirq resume for NVMe host */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);   /* NVMe: host private from driver data */
	int err;   /* NVMe: error code */

	err = regulator_enable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "fail to enable vpcie0v9 regulator\n");   /* NVMe: log error */
		return err;   /* NVMe: propagate error */
	}   /* NVMe: end of block */

	err = rockchip_pcie_enable_clocks(rockchip);   /* NVMe: enable controller clocks */
	if (err)   /* NVMe: check error code */
		goto err_disable_0v9;   /* NVMe: jump to error path */

	err = rockchip_pcie_host_init_port(rockchip);   /* NVMe: init port and train link */
	if (err)   /* NVMe: check error code */
		goto err_pcie_resume;   /* NVMe: jump to error path */

	err = rockchip_pcie_cfg_atu(rockchip);   /* NVMe: config ATU windows */
	if (err)   /* NVMe: check error code */
		goto err_err_deinit_port;   /* NVMe: jump to error path */

	/* Need this to enter L1 again */
	rockchip_pcie_update_txcredit_mui(rockchip);   /* NVMe: update TX credit MUI */
	rockchip_pcie_enable_interrupts(rockchip);   /* NVMe: enable interrupts */

	return 0;   /* NVMe: success */

err_err_deinit_port:   /* NVMe: host controller operation */
	rockchip_pcie_deinit_phys(rockchip);   /* NVMe: deinit PHYs */
err_pcie_resume:   /* NVMe: host controller operation */
	rockchip_pcie_disable_clocks(rockchip);   /* NVMe: disable controller clocks */
err_disable_0v9:   /* NVMe: host controller operation */
	regulator_disable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */
	return err;   /* NVMe: propagate error */
}   /* NVMe: end of block */

static int rockchip_pcie_probe(struct platform_device *pdev)   /* NVMe: platform probe, enumerate PCI and bind NVMe */
{   /* NVMe: begin block */
	struct rockchip_pcie *rockchip;   /* NVMe: host private data */
	struct device *dev = &pdev->dev;   /* NVMe: platform device pointer */
	struct pci_host_bridge *bridge;   /* NVMe: PCI host bridge */
	int err;   /* NVMe: error code */

	if (!dev->of_node)   /* NVMe: conditional check */
		return -ENODEV;   /* NVMe: return error code */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rockchip));   /* NVMe: allocate PCI host bridge */
	if (!bridge)   /* NVMe: check pointer/resource present */
		return -ENOMEM;   /* NVMe: return error code */

	rockchip = pci_host_bridge_priv(bridge);   /* NVMe: get host bridge private area */

	platform_set_drvdata(pdev, rockchip);   /* NVMe: store driver private data */
	rockchip->dev = dev;   /* NVMe: host controller operation */
	rockchip->is_rc = true;   /* NVMe: host controller operation */

	err = rockchip_pcie_parse_host_dt(rockchip);   /* NVMe: update local variable */
	if (err)   /* NVMe: check error code */
		return err;   /* NVMe: propagate error */

	err = rockchip_pcie_enable_clocks(rockchip);   /* NVMe: enable controller clocks */
	if (err)   /* NVMe: check error code */
		return err;   /* NVMe: propagate error */

	err = rockchip_pcie_set_vpcie(rockchip);   /* NVMe: update local variable */
	if (err) {   /* NVMe: check error code */
		dev_err(dev, "failed to set vpcie regulator\n");   /* NVMe: log error */
		goto err_set_vpcie;   /* NVMe: jump to error path */
	}   /* NVMe: end of block */

	err = rockchip_pcie_host_init_port(rockchip);   /* NVMe: init port and train link */
	if (err)   /* NVMe: check error code */
		goto err_vpcie;   /* NVMe: jump to error path */

	err = rockchip_pcie_init_irq_domain(rockchip);   /* NVMe: init INTx domain */
	if (err < 0)   /* NVMe: check negative error */
		goto err_deinit_port;   /* NVMe: jump to error path */

	err = rockchip_pcie_cfg_atu(rockchip);   /* NVMe: config ATU windows */
	if (err)   /* NVMe: check error code */
		goto err_remove_irq_domain;   /* NVMe: jump to error path */

	rockchip->msg_region = devm_ioremap(dev, rockchip->msg_bus_addr, SZ_1M);   /* NVMe: map MMIO region for messages */
	if (!rockchip->msg_region) {   /* NVMe: conditional check */
		err = -ENOMEM;   /* NVMe: update local variable */
		goto err_remove_irq_domain;   /* NVMe: jump to error path */
	}   /* NVMe: end of block */

	bridge->sysdata = rockchip;   /* NVMe: host controller operation */
	bridge->ops = &rockchip_pcie_ops;   /* NVMe: host controller operation */

	err = rockchip_pcie_setup_irq(rockchip);   /* NVMe: setup IRQs */
	if (err)   /* NVMe: check error code */
		goto err_remove_irq_domain;   /* NVMe: jump to error path */

	rockchip_pcie_enable_interrupts(rockchip);   /* NVMe: enable interrupts */

	err = pci_host_probe(bridge);   /* NVMe: enumerate PCI bus and bind NVMe */
	if (err < 0)   /* NVMe: check negative error */
		goto err_remove_irq_domain;   /* NVMe: jump to error path */

	return 0;   /* NVMe: success */

err_remove_irq_domain:   /* NVMe: host controller operation */
	irq_domain_remove(rockchip->irq_domain);   /* NVMe: irqdomain operation */
err_deinit_port:   /* NVMe: host controller operation */
	rockchip_pcie_deinit_phys(rockchip);   /* NVMe: deinit PHYs */
err_vpcie:   /* NVMe: host controller operation */
	if (!IS_ERR(rockchip->vpcie12v))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie12v);   /* NVMe: manage PCIe power rail */
	if (!IS_ERR(rockchip->vpcie3v3))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie3v3);   /* NVMe: manage PCIe power rail */
	regulator_disable(rockchip->vpcie1v8);   /* NVMe: manage PCIe power rail */
	regulator_disable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */
err_set_vpcie:   /* NVMe: host controller operation */
	rockchip_pcie_disable_clocks(rockchip);   /* NVMe: disable controller clocks */
	return err;   /* NVMe: propagate error */
}   /* NVMe: end of block */

static void rockchip_pcie_remove(struct platform_device *pdev)   /* NVMe: platform remove, unbind NVMe and power off */
{   /* NVMe: begin block */
	struct device *dev = &pdev->dev;   /* NVMe: platform device pointer */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);   /* NVMe: host private from driver data */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rockchip);   /* NVMe: host bridge from private */

	pci_stop_root_bus(bridge->bus);   /* NVMe: stop PCI root bus */
	pci_remove_root_bus(bridge->bus);   /* NVMe: remove PCI root bus */
	irq_domain_remove(rockchip->irq_domain);   /* NVMe: irqdomain operation */

	rockchip_pcie_deinit_phys(rockchip);   /* NVMe: deinit PHYs */

	rockchip_pcie_disable_clocks(rockchip);   /* NVMe: disable controller clocks */

	if (!IS_ERR(rockchip->vpcie12v))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie12v);   /* NVMe: manage PCIe power rail */
	if (!IS_ERR(rockchip->vpcie3v3))   /* NVMe: check pointer/error */
		regulator_disable(rockchip->vpcie3v3);   /* NVMe: manage PCIe power rail */
	regulator_disable(rockchip->vpcie1v8);   /* NVMe: manage PCIe power rail */
	regulator_disable(rockchip->vpcie0v9);   /* NVMe: manage PCIe power rail */
}   /* NVMe: end of block */

static const struct dev_pm_ops rockchip_pcie_pm_ops = {   /* NVMe: power management callbacks */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(rockchip_pcie_suspend_noirq,   /* NVMe: host controller operation */
				  rockchip_pcie_resume_noirq)   /* NVMe: host controller operation */
};   /* NVMe: host controller operation */

static const struct of_device_id rockchip_pcie_of_match[] = {   /* NVMe: DT compatible IDs */
	{ .compatible = "rockchip,rk3399-pcie", },   /* NVMe: host controller operation */
	{}   /* NVMe: host controller operation */
};   /* NVMe: host controller operation */
MODULE_DEVICE_TABLE(of, rockchip_pcie_of_match);   /* NVMe: host controller operation */

static struct platform_driver rockchip_pcie_driver = {   /* NVMe: platform driver registration */
	.driver = {   /* NVMe: host controller operation */
		.name = "rockchip-pcie",   /* NVMe: host controller operation */
		.of_match_table = rockchip_pcie_of_match,   /* NVMe: host controller operation */
		.pm = &rockchip_pcie_pm_ops,   /* NVMe: host controller operation */
	},   /* NVMe: host controller operation */
	.probe = rockchip_pcie_probe,   /* NVMe: host controller operation */
	.remove = rockchip_pcie_remove,   /* NVMe: host controller operation */
};   /* NVMe: host controller operation */
module_platform_driver(rockchip_pcie_driver);   /* NVMe: register Rockchip PCIe platform driver */

MODULE_AUTHOR("Rockchip Inc");   /* NVMe: module author */
MODULE_DESCRIPTION("Rockchip AXI PCIe driver");   /* NVMe: module description */
MODULE_LICENSE("GPL v2");   /* NVMe: GPL v2 license */
