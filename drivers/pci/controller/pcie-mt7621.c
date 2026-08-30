// SPDX-License-Identifier: GPL-2.0+
/*
 * BRIEF MODULE DESCRIPTION
 *     PCI init for Ralink RT2880 solution
 *
 * Copyright 2007 Ralink Inc. (bruce_chang@ralinktech.com.tw)
 *
 * May 2007 Bruce Chang
 * Initial Release
 *
 * May 2009 Bruce Chang
 * support RT2880/RT3883 PCIe
 *
 * May 2011 Bruce Chang
 * support RT6855/MT7620 PCIe
 */

#include <linux/bitops.h>          /* PCI/NVMe: bit manipulation for PCIe register masks */
#include <linux/clk.h>             /* PCI/NVMe: clock control for PCIe ports feeding NVMe links */
#include <linux/delay.h>           /* PCI/NVMe: reset/setup delays while NVMe SSD stabilizes */
#include <linux/gpio/consumer.h>   /* PCI/NVMe: PERST# GPIO control for downstream NVMe devices */
#include <linux/module.h>
#include <linux/of.h>              /* PCI/NVMe: DT parsing for PCIe ports that may host NVMe SSDs */
#include <linux/of_address.h>
#include <linux/of_pci.h>          /* PCI/NVMe: OF helpers to map PCIe slot/function for NVMe enumeration */
#include <linux/of_platform.h>
#include <linux/pci.h>             /* PCI/NVMe: core PCIe definitions used to enumerate NVMe functions */
#include <linux/phy/phy.h>         /* PCI/NVMe: PHY init for each PCIe lane carrying NVMe traffic */
#include <linux/platform_device.h>
#include <linux/reset.h>           /* PCI/NVMe: reset lines for RC and endpoint (NVMe SSD) */
#include <linux/sys_soc.h>

#include "../pci.h"                /* PCI/NVMe: internal PCI host helper declarations */

/* MediaTek-specific configuration registers */
#define PCIE_FTS_NUM			0x70c   /* PCI/NVMe: FTS (Fast Training Sequence) count cfg register; ASPM L0s exit timing affects NVMe latency */
#define PCIE_FTS_NUM_MASK		GENMASK(15, 8)  /* PCI/NVMe: FTS number field mask for L0s */
#define PCIE_FTS_NUM_L0(x)		(((x) & 0xff) << 8)  /* PCI/NVMe: encode FTS count used when leaving ASPM L0s */

/* Host-PCI bridge registers */
#define RALINK_PCI_PCICFG_ADDR		0x0000  /* PCI/NVMe: host bridge PCI config/status register base */
#define RALINK_PCI_PCIMSK_ADDR		0x000c  /* PCI/NVMe: PCIe port interrupt enable/mask register; gates legacy INTx signals that may reach NVMe */
#define RALINK_PCI_CONFIG_ADDR		0x0020  /* PCI/NVMe: ECAM/CONF1 address port for config cycles to NVMe devices */
#define RALINK_PCI_CONFIG_DATA		0x0024  /* PCI/NVMe: ECAM/CONF1 data port for NVMe config read/write */
#define RALINK_PCI_MEMBASE		0x0028  /* PCI/NVMe: outbound memory window base for NVMe BAR/MMIO access */
#define RALINK_PCI_IOBASE		0x002c  /* PCI/NVMe: outbound I/O window base (rarely used by NVMe) */

/* PCIe RC control registers */
#define RALINK_PCI_ID			0x0030  /* PCI/NVMe: RC vendor/device ID register exposed during NVMe enumeration */
#define RALINK_PCI_CLASS		0x0034  /* PCI/NVMe: RC class code register (PCIe root bridge) seen by NVMe stack */
#define RALINK_PCI_SUBID		0x0038  /* PCI/NVMe: subsystem ID register */
#define RALINK_PCI_STATUS		0x0050  /* PCI/NVMe: port status including PCIE_PORT_LINKUP for NVMe link detection */

/* Some definition values */
#define PCIE_REVISION_ID		BIT(0)  /* PCI/NVMe: revision ID value written into RC class register */
#define PCIE_CLASS_CODE			(0x60400 << 8)  /* PCI/NVMe: class = PCI_CLASS_BRIDGE_PCI (0x060400) for root bridge under NVMe bus */
#define PCIE_BAR_MAP_MAX		GENMASK(30, 16)  /* PCI/NVMe: 2 GiB inbound memory map window for NVMe DMA/MMIO */
#define PCIE_BAR_ENABLE			BIT(0)  /* PCI/NVMe: enable BAR0 mapping so NVMe SSD BARs can be reached */
#define PCIE_PORT_INT_EN(x)		BIT(20 + (x))  /* PCI/NVMe: enable legacy INTx interrupt for port x; NVMe may fall back to INTx if MSI/MSI-X unavailable */
#define PCIE_PORT_LINKUP		BIT(0)  /* PCI/NVMe: link-up status bit; set when an NVMe SSD completes link training */
#define PCIE_PORT_CNT			3       /* PCI/NVMe: number of root ports; each may carry a downstream NVMe endpoint */

#define INIT_PORTS_DELAY_MS		100     /* PCI/NVMe: delay after PHY power-on before checking NVMe link status */
#define PERST_DELAY_MS			100     /* PCI/NVMe: PERST# assertion/deassertion pulse width for NVMe SSD reset */

/**
 * struct mt7621_pcie_port - PCIe port information
 * @base: I/O mapped register base
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @clk: pointer to the port clock gate
 * @phy: pointer to PHY control block
 * @pcie_rst: pointer to port reset control
 * @gpio_rst: gpio reset
 * @slot: port slot
 * @enabled: indicates if port is enabled
 */
struct mt7621_pcie_port {
	void __iomem *base;          /* PCI/NVMe: port-specific MMIO base for PCIe config/control registers */
	struct list_head list;       /* PCI/NVMe: linked list of enabled ports scanned during NVMe enumeration setup */
	struct mt7621_pcie *pcie;    /* PCI/NVMe: back-pointer to host bridge owning this NVMe-capable port */
	struct clk *clk;             /* PCI/NVMe: port reference clock; must be on for NVMe link training */
	struct phy *phy;             /* PCI/NVMe: SERDES/PHY handle for the lane connected to the NVMe SSD */
	struct reset_control *pcie_rst; /* PCI/NVMe: RC reset line used to reset the port before NVMe probe */
	struct gpio_desc *gpio_rst;  /* PCI/NVMe: optional GPIO PERST# driving the downstream NVMe device */
	u32 slot;                    /* PCI/NVMe: logical slot number mapped to PCI_SLOT() for NVMe devfn */
	bool enabled;                /* PCI/NVMe: true if link is up and port participates in NVMe enumeration */
};

/**
 * struct mt7621_pcie - PCIe host information
 * @base: IO Mapped Register Base
 * @dev: Pointer to PCIe device
 * @ports: pointer to PCIe port information
 * @resets_inverted: depends on chip revision
 * reset lines are inverted.
 */
struct mt7621_pcie {
	struct device *dev;          /* PCI/NVMe: device pointer used for logging and devm allocation during NVMe host setup */
	void __iomem *base;          /* PCI/NVMe: host bridge MMIO base for ECAM/config address/data ports used by NVMe */
	struct list_head ports;      /* PCI/NVMe: list of mt7621_pcie_port structures representing possible NVMe slots */
	bool resets_inverted;        /* PCI/NVMe: SoC revision quirk affecting PERST#/reset polarity for NVMe SSD reset */
};

static inline u32 pcie_read(struct mt7621_pcie *pcie, u32 reg)
{
	/* PCI/NVMe: read 32-bit host bridge register; used for config address/data and window setup for NVMe access */
	return readl_relaxed(pcie->base + reg);
}

static inline void pcie_write(struct mt7621_pcie *pcie, u32 val, u32 reg)
{
	/* PCI/NVMe: write 32-bit host bridge register; programs config/data ports and memory windows used by NVMe */
	writel_relaxed(val, pcie->base + reg);
}

static inline u32 pcie_port_read(struct mt7621_pcie_port *port, u32 reg)
{
	/* PCI/NVMe: read port-specific register such as RALINK_PCI_STATUS to check NVMe link-up */
	return readl_relaxed(port->base + reg);
}

static inline void pcie_port_write(struct mt7621_pcie_port *port,
				   u32 val, u32 reg)
{
	/* PCI/NVMe: write port-specific register such as BAR/class setup required before NVMe enumeration */
	writel_relaxed(val, port->base + reg);
}

static void __iomem *mt7621_pcie_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	struct mt7621_pcie *pcie = bus->sysdata;  /* PCI/NVMe: host private data attached to bus during NVMe enumeration */
	u32 address = PCI_CONF1_EXT_ADDRESS(bus->number, PCI_SLOT(devfn),
					    PCI_FUNC(devfn), where);
	/* PCI/NVMe: build ECAM/CONF1 address for the target NVMe function/slot and config offset */

	writel_relaxed(address, pcie->base + RALINK_PCI_CONFIG_ADDR);
	/* PCI/NVMe: latch address into config address port so the next data access targets the NVMe config space */

	return pcie->base + RALINK_PCI_CONFIG_DATA + (where & 3);
	/* PCI/NVMe: return data register offset (byte aligned) used by pci_generic_config_read/write for NVMe cfg */
}

static struct pci_ops mt7621_pcie_ops = {
	.map_bus	= mt7621_pcie_map_bus,    /* PCI/NVMe: translate PCI bus/devfn/offset to MMIO for NVMe config access */
	.read		= pci_generic_config_read, /* PCI/NVMe: generic PCI config read; used to fetch NVMe BAR, class, MSI-X caps */
	.write		= pci_generic_config_write, /* PCI/NVMe: generic PCI config write; used to assign NVMe BARs and enable MSI-X */
};

static u32 read_config(struct mt7621_pcie *pcie, unsigned int dev, u32 reg)
{
	u32 address = PCI_CONF1_EXT_ADDRESS(0, dev, 0, reg);
	/* PCI/NVMe: build config address for local root port 'dev' (not yet enumerated NVMe endpoint) */

	pcie_write(pcie, address, RALINK_PCI_CONFIG_ADDR);
	/* PCI/NVMe: select local port config register, e.g. PCIE_FTS_NUM for ASPM timing affecting NVMe power */

	return pcie_read(pcie, RALINK_PCI_CONFIG_DATA);
	/* PCI/NVMe: return raw config data from the selected root port register */
}

static void write_config(struct mt7621_pcie *pcie, unsigned int dev,
			 u32 reg, u32 val)
{
	u32 address = PCI_CONF1_EXT_ADDRESS(0, dev, 0, reg);
	/* PCI/NVMe: build config address for local root port 'dev' before NVMe devices are scanned */

	pcie_write(pcie, address, RALINK_PCI_CONFIG_ADDR);
	/* PCI/NVMe: select local port config register to be programmed */

	pcie_write(pcie, val, RALINK_PCI_CONFIG_DATA);
	/* PCI/NVMe: write value to selected root port config register (e.g. FTS for NVMe ASPM) */
}

static inline void mt7621_rst_gpio_pcie_assert(struct mt7621_pcie_port *port)
{
	if (port->gpio_rst)
		/* PCI/NVMe: assert PERST# (active-high GPIO) to hold downstream NVMe SSD in reset */
		gpiod_set_value(port->gpio_rst, 1);
}

static inline void mt7621_rst_gpio_pcie_deassert(struct mt7621_pcie_port *port)
{
	if (port->gpio_rst)
		/* PCI/NVMe: deassert PERSET# so the NVMe SSD can exit reset and begin link training */
		gpiod_set_value(port->gpio_rst, 0);
}

static inline bool mt7621_pcie_port_is_linkup(struct mt7621_pcie_port *port)
{
	/* PCI/NVMe: return true if the physical link to the NVMe endpoint is up (training completed) */
	return (pcie_port_read(port, RALINK_PCI_STATUS) & PCIE_PORT_LINKUP) != 0;
}

static inline void mt7621_control_assert(struct mt7621_pcie_port *port)
{
	struct mt7621_pcie *pcie = port->pcie;

	if (pcie->resets_inverted)
		/* PCI/NVMe: on E2 SoC, inverted reset logic: assert means deassert for NVMe SSD reset */
		reset_control_assert(port->pcie_rst);
	else
		/* PCI/NVMe: normal polarity: deassert RC reset to release NVMe port logic */
		reset_control_deassert(port->pcie_rst);
}

static inline void mt7621_control_deassert(struct mt7621_pcie_port *port)
{
	struct mt7621_pcie *pcie = port->pcie;

	if (pcie->resets_inverted)
		/* PCI/NVMe: on E2 SoC, inverted reset logic: deassert means assert for NVMe port reset */
		reset_control_deassert(port->pcie_rst);
	else
		/* PCI/NVMe: normal polarity: assert RC reset to hold NVMe port logic in reset */
		reset_control_assert(port->pcie_rst);
}

static int mt7621_pcie_parse_port(struct mt7621_pcie *pcie,
				  struct device_node *node,
				  int slot)
{
	struct mt7621_pcie_port *port;
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	char name[11];
	int err;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* PCI/NVMe: allocate per-port context that tracks one potential NVMe slot */
	if (!port)
		return -ENOMEM;

	port->base = devm_platform_ioremap_resource(pdev, slot + 1);
	/* PCI/NVMe: ioremap port registers so driver can access config/status for this NVMe slot */
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);

	port->clk = devm_get_clk_from_child(dev, node, NULL);
	if (IS_ERR(port->clk)) {
		dev_err(dev, "failed to get pcie%d clock\n", slot);
		return PTR_ERR(port->clk);
	}
	/* PCI/NVMe: clock is required to run the PHY and link layer for the NVMe endpoint */

	port->pcie_rst = of_reset_control_get_exclusive(node, NULL);
	if (PTR_ERR(port->pcie_rst) == -EPROBE_DEFER) {
		dev_err(dev, "failed to get pcie%d reset control\n", slot);
		return PTR_ERR(port->pcie_rst);
	}
	/* PCI/NVMe: reset controller used to reset the RC port logic before NVMe link training */

	snprintf(name, sizeof(name), "pcie-phy%d", slot);
	port->phy = devm_of_phy_get(dev, node, name);
	if (IS_ERR(port->phy)) {
		dev_err(dev, "failed to get pcie-phy%d\n", slot);
		err = PTR_ERR(port->phy);
		goto remove_reset;
	}
	/* PCI/NVMe: PHY is required to establish the electrical link to the NVMe SSD */

	port->gpio_rst = devm_gpiod_get_index_optional(dev, "reset", slot,
						       GPIOD_OUT_LOW);
	if (IS_ERR(port->gpio_rst)) {
		dev_err(dev, "failed to get GPIO for PCIe%d\n", slot);
		err = PTR_ERR(port->gpio_rst);
		goto remove_reset;
	}
	/* PCI/NVMe: optional GPIO PERST# for holding the NVMe SSD in reset during probe */

	port->slot = slot;
	/* PCI/NVMe: record slot number used as PCI_SLOT() when building config cycles for NVMe */

	port->pcie = pcie;
	/* PCI/NVMe: link port back to host bridge so config/data access can reach NVMe devices */

	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);
	/* PCI/NVMe: add port to host's port list so probe/enable can iterate over NVMe slots */

	return 0;

remove_reset:
	reset_control_put(port->pcie_rst);
	/* PCI/NVMe: release reset reference on PHY/GPIO error so NVMe port is not half-probed */
	return err;
}

static int mt7621_pcie_parse_dt(struct mt7621_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *node = dev->of_node;
	int err;

	pcie->base = devm_platform_ioremap_resource(pdev, 0);
	/* PCI/NVMe: ioremap host bridge registers (config addr/data, memory/io windows) for NVMe access */
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	for_each_available_child_of_node_scoped(node, child) {
		int slot;

		err = of_pci_get_devfn(child);
		/* PCI/NVMe: parse DT child devfn; determines PCI_SLOT used when addressing an NVMe SSD */
		if (err < 0)
			return dev_err_probe(dev, err, "failed to parse devfn\n");

		slot = PCI_SLOT(err);
		/* PCI/NVMe: extract slot number that will identify this NVMe root port on the bus */

		err = mt7621_pcie_parse_port(pcie, child, slot);
		if (err)
			return err;
	}

	return 0;
}

static int mt7621_pcie_init_port(struct mt7621_pcie_port *port)
{
	struct mt7621_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
	u32 slot = port->slot;
	int err;

	err = phy_init(port->phy);
	/* PCI/NVMe: initialize SERDES/PHY for this NVMe-capable port */
	if (err) {
		dev_err(dev, "failed to initialize port%d phy\n", slot);
		return err;
	}

	err = phy_power_on(port->phy);
	/* PCI/NVMe: power on PHY so the differential pair to the NVMe SSD is active */
	if (err) {
		dev_err(dev, "failed to power on port%d phy\n", slot);
		phy_exit(port->phy);
		return err;
	}

	port->enabled = true;
	/* PCI/NVMe: mark PHY-powered port as candidate for NVMe link training */

	return 0;
}

static void mt7621_pcie_reset_assert(struct mt7621_pcie *pcie)
{
	struct mt7621_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list) {
		/* PCIe RC reset assert */
		mt7621_control_assert(port);
		/* PCI/NVMe: assert reset to the RC port logic before NVMe SSD reset sequence */

		/* PCIe EP reset assert */
		mt7621_rst_gpio_pcie_assert(port);
		/* PCI/NVMe: assert PERST# to the downstream NVMe endpoint */
	}

	msleep(PERST_DELAY_MS);
	/* PCI/NVMe: keep PERST# asserted long enough for the NVMe SSD to enter reset */
}

static void mt7621_pcie_reset_rc_deassert(struct mt7621_pcie *pcie)
{
	struct mt7621_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		mt7621_control_deassert(port);
		/* PCI/NVMe: release RC port reset so link training toward NVMe SSD can start */
}

static void mt7621_pcie_reset_ep_deassert(struct mt7621_pcie *pcie)
{
	struct mt7621_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		mt7621_rst_gpio_pcie_deassert(port);
		/* PCI/NVMe: release PERST# so the NVMe SSD can begin link training */

	msleep(PERST_DELAY_MS);
	/* PCI/NVMe: wait for NVMe SSD to come out of reset and train the link */
}

static int mt7621_pcie_init_ports(struct mt7621_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct mt7621_pcie_port *port, *tmp;
	u8 num_disabled = 0;
	int err;

	mt7621_pcie_reset_assert(pcie);
	/* PCI/NVMe: start with both RC and NVMe endpoint held in reset */

	mt7621_pcie_reset_rc_deassert(pcie);
	/* PCI/NVMe: release RC reset first while keeping NVMe SSD in reset */

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		u32 slot = port->slot;

		if (slot == 1) {
			port->enabled = true;
			/* PCI/NVMe: slot 1 is always enabled (no PHY control), assume NVMe may be present */
			continue;
		}

		err = mt7621_pcie_init_port(port);
		/* PCI/NVMe: power on PHY for this potential NVMe slot */
		if (err) {
			dev_err(dev, "initializing port %d failed\n", slot);
			list_del(&port->list);
			/* PCI/NVMe: remove broken port from NVMe enumeration candidate list */
		}
	}

	msleep(INIT_PORTS_DELAY_MS);
	/* PCI/NVMe: delay after PHY power-on before sampling NVMe link-up status */

	mt7621_pcie_reset_ep_deassert(pcie);
	/* PCI/NVMe: release NVMe endpoint PERST# and let links train */

	tmp = NULL;
	list_for_each_entry(port, &pcie->ports, list) {
		u32 slot = port->slot;

		if (!mt7621_pcie_port_is_linkup(port)) {
			dev_info(dev, "pcie%d no card, disable it (RST & CLK)\n",
				 slot);
			/* PCI/NVMe: no NVMe device detected on this port */

			mt7621_control_assert(port);
			/* PCI/NVMe: put port back into reset since no NVMe SSD is present */

			port->enabled = false;
			/* PCI/NVMe: exclude empty slot from NVMe bus enumeration */

			num_disabled++;

			if (slot == 0) {
				tmp = port;
				/* PCI/NVMe: remember slot 0 so slot-1 PHY can be gated if slot 0 is empty */
				continue;
			}

			if (slot == 1 && tmp && !tmp->enabled)
				phy_power_off(tmp->phy);
				/* PCI/NVMe: power off slot 0 PHY when slot 1 is also empty (SoC sharing quirk) */
		}
	}

	return (num_disabled != PCIE_PORT_CNT) ? 0 : -ENODEV;
	/* PCI/NVMe: fail probe only if no NVMe-capable port has a link; otherwise enumeration proceeds */
}

static void mt7621_pcie_enable_port(struct mt7621_pcie_port *port)
{
	struct mt7621_pcie *pcie = port->pcie;
	u32 slot = port->slot;
	u32 val;

	/* enable pcie interrupt */
	val = pcie_read(pcie, RALINK_PCI_PCIMSK_ADDR);
	/* PCI/NVMe: read legacy interrupt mask; NVMe MSI/MSI-X is handled by PCI core, INTx fallback depends on this bit */

	val |= PCIE_PORT_INT_EN(slot);
	/* PCI/NVMe: enable legacy INTx for this port so an NVMe SSD without MSI/MSI-X can raise interrupts */

	pcie_write(pcie, val, RALINK_PCI_PCIMSK_ADDR);
	/* PCI/NVMe: apply updated interrupt mask */

	/* map 2G DDR region */
	pcie_port_write(port, PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE,
			PCI_BASE_ADDRESS_0);
	/* PCI/NVMe: program root port BAR0 to map 2 GiB inbound window; used for NVMe DMA and MMIO accesses */

	/* configure class code and revision ID */
	pcie_port_write(port, PCIE_CLASS_CODE | PCIE_REVISION_ID,
			RALINK_PCI_CLASS);
	/* PCI/NVMe: expose root bridge class code 0x060400 so NVMe enumeration recognizes a PCIe bridge */

	/* configure RC FTS number to 250 when it leaves L0s */
	val = read_config(pcie, slot, PCIE_FTS_NUM);
	/* PCI/NVMe: read current FTS register; affects ASPM L0s exit latency reported to NVMe */

	val &= ~PCIE_FTS_NUM_MASK;
	/* PCI/NVMe: clear old FTS number field */

	val |= PCIE_FTS_NUM_L0(0x50);
	/* PCI/NVMe: set FTS count to 0x50 (80) per lane; influences NVMe ASPM active-state power management */

	write_config(pcie, slot, PCIE_FTS_NUM, val);
	/* PCI/NVMe: write back updated FTS value for NVMe link power behavior */
}

static int mt7621_pcie_enable_ports(struct pci_host_bridge *host)
{
	struct mt7621_pcie *pcie = pci_host_bridge_priv(host);
	/* PCI/NVMe: retrieve host private data from the PCI host bridge created for NVMe enumeration */

	struct device *dev = pcie->dev;
	struct mt7621_pcie_port *port;
	struct resource_entry *entry;
	int err;

	entry = resource_list_first_type(&host->windows, IORESOURCE_IO);
	/* PCI/NVMe: locate the I/O aperture parsed from DT; usually unused by NVMe (BAR0 is MMIO) */
	if (!entry) {
		dev_err(dev, "cannot get io resource\n");
		return -EINVAL;
	}

	/* Setup MEMWIN and IOWIN */
	pcie_write(pcie, 0xffffffff, RALINK_PCI_MEMBASE);
	/* PCI/NVMe: open outbound memory window to full 4 GiB so NVMe BAR/MMIO and DMA can use it */

	pcie_write(pcie, entry->res->start - entry->offset, RALINK_PCI_IOBASE);
	/* PCI/NVMe: program I/O window base for legacy I/O BARs (rarely used by NVMe) */

	list_for_each_entry(port, &pcie->ports, list) {
		if (port->enabled) {
			err = clk_prepare_enable(port->clk);
			/* PCI/NVMe: enable reference clock to the NVMe-capable port */
			if (err) {
				dev_err(dev, "enabling clk pcie%d\n",
					port->slot);
				return err;
			}

			mt7621_pcie_enable_port(port);
			/* PCI/NVMe: configure interrupts, BAR, class code, ASPM FTS for the NVMe root port */

			dev_info(dev, "PCIE%d enabled\n", port->slot);
			/* PCI/NVMe: port is ready for PCI core to enumerate an NVMe SSD below it */
		}
	}

	return 0;
}

static int mt7621_pcie_register_host(struct pci_host_bridge *host)
{
	struct mt7621_pcie *pcie = pci_host_bridge_priv(host);
	/* PCI/NVMe: get host private data before handing the bridge to PCI core for NVMe bus scan */

	host->ops = &mt7621_pcie_ops;
	/* PCI/NVMe: attach config access ops so PCI core can read/write NVMe config space */

	host->sysdata = pcie;
	/* PCI/NVMe: pass host private data to bus->sysdata for use by map_bus during NVMe cfg cycles */

	return pci_host_probe(host);
	/* PCI/NVMe: start PCI bus enumeration; if an NVMe SSD is present the core will discover and bind nvme-pci */
}

static const struct soc_device_attribute mt7621_pcie_quirks_match[] = {
	{ .soc_id = "mt7621", .revision = "E2" },
	/* PCI/NVMe: E2 revision has inverted reset polarity for the NVMe endpoint/RC reset sequence */
	{ /* sentinel */ }
};

static int mt7621_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct soc_device_attribute *attr;
	struct mt7621_pcie_port *port;
	struct mt7621_pcie *pcie;
	struct pci_host_bridge *bridge;
	int err;

	if (!dev->of_node)
		return -ENODEV;
	/* PCI/NVMe: DT is required to describe which ports may host NVMe SSDs */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* PCI/NVMe: allocate PCI host bridge structure that will carry the NVMe-capable root bus */
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	/* PCI/NVMe: host private area embedded inside the bridge; stores NVMe port list and MMIO base */

	pcie->dev = dev;
	/* PCI/NVMe: store device pointer for logging and resource allocation during NVMe setup */

	platform_set_drvdata(pdev, pcie);
	/* PCI/NVMe: allow remove() to retrieve host state and clean up NVMe port resources */

	INIT_LIST_HEAD(&pcie->ports);
	/* PCI/NVMe: initialize empty list of discovered NVMe-capable root ports */

	attr = soc_device_match(mt7621_pcie_quirks_match);
	/* PCI/NVMe: detect SoC revision to apply reset-polarity quirk affecting NVMe SSD reset */
	if (attr)
		pcie->resets_inverted = true;

	err = mt7621_pcie_parse_dt(pcie);
	/* PCI/NVMe: parse DT to ioremap host bridge and discover each NVMe root port */
	if (err) {
		dev_err(dev, "parsing DT failed\n");
		return err;
	}

	err = mt7621_pcie_init_ports(pcie);
	/* PCI/NVMe: reset RC/NVMe endpoints, power PHYs, and detect link-up for each NVMe slot */
	if (err) {
		dev_err(dev, "nothing connected in virtual bridges\n");
		return 0;
		/* PCI/NVMe: no NVMe device found but probe succeeds; kernel will not register a PCI bus */
	}

	err = mt7621_pcie_enable_ports(bridge);
	/* PCI/NVMe: enable clocks, interrupts, BAR mapping, and FTS for ports with NVMe links */
	if (err) {
		dev_err(dev, "error enabling pcie ports\n");
		goto remove_resets;
	}

	return mt7621_pcie_register_host(bridge);
	/* PCI/NVMe: register host with PCI core; triggers bus scan that binds nvme-pci to any NVMe SSD */

remove_resets:
	list_for_each_entry(port, &pcie->ports, list)
		reset_control_put(port->pcie_rst);
		/* PCI/NVMe: release reset references on enable failure so NVMe port resources are not leaked */

	return err;
}

static void mt7621_pcie_remove(struct platform_device *pdev)
{
	struct mt7621_pcie *pcie = platform_get_drvdata(pdev);
	/* PCI/NVMe: retrieve host state to tear down resources used by the NVMe root bus */

	struct mt7621_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		reset_control_put(port->pcie_rst);
		/* PCI/NVMe: release RC reset references; downstream NVMe devices should already be unbound */
}

static const struct of_device_id mt7621_pcie_ids[] = {
	{ .compatible = "mediatek,mt7621-pci" },
	/* PCI/NVMe: match MT7621 PCIe host controller nodes that can expose NVMe root ports */
	{},
};
MODULE_DEVICE_TABLE(of, mt7621_pcie_ids);

static struct platform_driver mt7621_pcie_driver = {
	.probe = mt7621_pcie_probe,
	/* PCI/NVMe: called when a compatible PCIe host node is found; sets up NVMe enumeration path */

	.remove = mt7621_pcie_remove,
	/* PCI/NVMe: called on driver unbind; releases reset resources backing the NVMe root bus */

	.driver = {
		.name = "mt7621-pci",
		.of_match_table = mt7621_pcie_ids,
	},
};
builtin_platform_driver(mt7621_pcie_driver);
/* PCI/NVMe: register as built-in driver so PCIe host is ready before nvme-pci probes NVMe SSDs */

MODULE_DESCRIPTION("MediaTek MT7621 PCIe host controller driver");
MODULE_LICENSE("GPL v2");
