// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for HiSilicon SoCs
 *
 * Copyright (C) 2015 HiSilicon Co., Ltd. http://www.hisilicon.com
 *
 * Authors: Zhou Wang <wangzhou1@hisilicon.com>
 *          Dacai Zhu <zhudacai@hisilicon.com>
 *          Gabriele Paoloni <gabriele.paoloni@huawei.com>
 */
#include <linux/interrupt.h>        /* PCI/NVMe: IRQ, MSI-X/MSI bottom-half handling for NVMe completion queues */
#include <linux/init.h>             /* PCI/NVMe: module init/exit macros; host driver lifetime management */
#include <linux/platform_device.h>  /* PCI/NVMe: platform bus binding, DT/ACPI probe path for PCIe RC */
#include <linux/pci.h>              /* PCI/NVMe: core PCI definitions, config space, BARs, capabilities */
#include <linux/pci-acpi.h>         /* PCI/NVMe: ACPI _SEG/_CRS parsing, root bridge resources for NVMe DMA */
#include <linux/pci-ecam.h>         /* PCI/NVMe: ECAM (Enhanced Config Access Mechanism) ops for config cycles */
#include "../../pci.h"              /* PCI/NVMe: internal PCI core helpers, e.g. acpi_get_rc_resources */
#include "../pci-host-common.h"     /* PCI/NVMe: shared host controller probe, bridge setup, resource parsing */

#if defined(CONFIG_PCI_HISI) || (defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS))
/* PCI/NVMe: compile this driver only when HiSilicon RC or ACPI quirks are enabled;
 *           otherwise no NVMe device behind this RC can be enumerated. */

struct hisi_pcie {
	void __iomem	*reg_base;
	/* PCI/NVMe: kernel virtual address of the RC own (non-ECAM) register region;
	 *           used to access root-port specific registers during NVMe enumeration. */
};

static int hisi_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
			     int size, u32 *val)
/* PCI/NVMe: config read callback invoked by pci_bus_read_config_* when
 *           NVMe host pci.c enumerates/buses the PCIe hierarchy. */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* PCI/NVMe: per-host config window; holds bus range, MMIO cfg resource, and drvdata. */
	int dev = PCI_SLOT(devfn);
	/* PCI/NVMe: physical device number on the PCIe bus; NVMe SSD will usually be devfn 0. */

	if (bus->number == cfg->busr.start) {
		/* PCI/NVMe: on the root bus only one slot (the RC itself / downstream link) exists;
		 *           NVMe devices sit behind a switch or endpoint, not on bus 0 directly. */
		/* access only one slot on each root port */
		if (dev > 0)
			return PCIBIOS_DEVICE_NOT_FOUND;
		/* PCI/NVMe: no device at this devfn, so NVMe probe will skip it. */
		else
			return pci_generic_config_read32(bus, devfn, where,
							 size, val);
		/* PCI/NVMe: use aligned 32-bit ECAM read for root-port registers;
		 *           needed to read RC capability/header fields affecting NVMe link. */
	}

	return pci_generic_config_read(bus, devfn, where, size, val);
	/* PCI/NVMe: for subordinate buses use the generic ECAM read path;
	 *           this reaches the NVMe endpoint's config space (BAR0, MSI-X cap, etc.). */
}

static int hisi_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
			     int where, int size, u32 val)
/* PCI/NVMe: config write callback used when NVMe host driver or PCI core
 *           programs endpoint/switch config space (BARs, command, MSI-X table). */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* PCI/NVMe: host controller config window carrying bus range and private data. */
	int dev = PCI_SLOT(devfn);
	/* PCI/NVMe: slot portion of devfn; NVMe SSD endpoint devfn is typically 0x0. */

	if (bus->number == cfg->busr.start) {
		/* access only one slot on each root port */
		if (dev > 0)
			return PCIBIOS_DEVICE_NOT_FOUND;
		/* PCI/NVMe: root bus has no other device; tell PCI core to move on. */
		else
			return pci_generic_config_write32(bus, devfn, where,
							  size, val);
		/* PCI/NVMe: 32-bit aligned write for root-port config registers;
		 *           used when PCI core enables RC bus mastering / IO/MEM decodes. */
	}

	return pci_generic_config_write(bus, devfn, where, size, val);
	/* PCI/NVMe: generic ECAM write targeting downstream NVMe device config space;
	 *           e.g. writing NVMe PCIe Command register to enable MMIO/BME. */
}

static void __iomem *hisi_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				       int where)
/* PCI/NVMe: returns the virtual address used by pci_generic_config_read/write
 *           to perform the actual MMIO config access to an NVMe endpoint. */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* PCI/NVMe: host controller config window storing ECAM base and drvdata. */
	struct hisi_pcie *pcie = cfg->priv;
	/* PCI/NVMe: HiSilicon private state, holds the RC register base address. */

	if (bus->number == cfg->busr.start)
		return pcie->reg_base + where;
	/* PCI/NVMe: root bus config cycles are served from the RC private register region
	 *           instead of standard ECAM; required before NVMe link comes up. */
	else
		return pci_ecam_map_bus(bus, devfn, where);
	/* PCI/NVMe: subordinate buses use standard ECAM offset calculation;
	 *           maps bus:devfn:offset to the physical config space MMIO window
	 *           that ultimately reaches the NVMe SSD's configuration headers. */
}

#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)
/* PCI/NVMe: ACPI boot path (e.g. server platforms with NVMe SSDs behind HiSilicon RC);
 *           PCI_QUIRKS is required because this controller deviates from pure ECAM. */

static int hisi_pcie_init(struct pci_config_window *cfg)
/* PCI/NVMe: ACPI probe callback registered in hisi_pcie_ops;
 *           sets up the RC register mapping so PCI core can enumerate NVMe devices. */
{
	struct device *dev = cfg->parent;
	/* PCI/NVMe: device struct representing the ACPI PCI root bridge / host controller. */
	struct hisi_pcie *pcie;
	/* PCI/NVMe: per-controller state holding reg_base for root-port config access. */
	struct acpi_device *adev = to_acpi_device(dev);
	/* PCI/NVMe: ACPI handle used to look up _HID/HISI0081 resources. */
	struct acpi_pci_root *root = acpi_driver_data(adev);
	/* PCI/NVMe: root bridge data containing segment number for this PCIe domain;
	 *           NVMe SSDs are identified by segment:bus:dev.fn during enumeration. */
	struct resource *res;
	/* PCI/NVMe: stores the RC register resource retrieved from ACPI _CRS. */
	int ret;
	/* PCI/NVMe: return value used for error propagation to PCI core probe. */

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* PCI/NVMe: allocate zeroed driver-private state; devm ensures release on driver detach. */
	if (!pcie)
		return -ENOMEM;
	/* PCI/NVMe: allocation failure aborts host probe; no NVMe device can be discovered. */

	/*
	 * Retrieve RC base and size from a HISI0081 device with _UID
	 * matching our segment.
	 */
	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	/* PCI/NVMe: allocate a resource to receive RC MMIO region from ACPI. */
	if (!res)
		return -ENOMEM;
	/* PCI/NVMe: out of memory during ACPI resource setup; NVMe enumeration cannot proceed. */

	ret = acpi_get_rc_resources(dev, "HISI0081", root->segment, res);
	/* PCI/NVMe: ask ACPI for the HiSilicon PCIe host register range tied to this segment;
	 *           this region is used for root-port config access and link status. */
	if (ret) {
		/* PCI/NVMe: failed to parse _CRS for RC registers; link may be down or DSDT broken. */
		dev_err(dev, "can't get rc base address\n");
		return -ENOMEM;
		/* PCI/NVMe: returning error prevents pci-host-common-probe from creating the bus,
		 *           so NVMe endpoints behind this RC are never seen. */
	}

	pcie->reg_base = devm_pci_remap_cfgspace(dev, res->start, resource_size(res));
	/* PCI/NVMe: ioremap the RC register range as PCI config space;
	 *           writes here configure the root port that the NVMe drive is attached to. */
	if (!pcie->reg_base)
		return -ENOMEM;
	/* PCI/NVMe: remap failure means the RC cannot be programmed; abort host probe. */

	cfg->priv = pcie;
	/* PCI/NVMe: attach private state to config window; later used by hisi_pcie_map_bus()
	 *           when PCI/NVMe core walks the bus scanning for endpoints. */
	return 0;
	/* PCI/NVMe: success; PCI core will now enumerate buses and discover NVMe devices. */
}

const struct pci_ecam_ops hisi_pcie_ops = {
	/* PCI/NVMe: ECAM operation table exported to ACPI PCI host core;
	 *           selected via acpi_match_hisi_pcie() quirk for HiSilicon RCs. */
	.init         =  hisi_pcie_init,
	/* PCI/NVMe: called during ACPI root bridge creation to map RC registers. */
	.pci_ops      = {
		/* PCI/NVMe: low-level PCI bus operations used for every config access. */
		.map_bus    = hisi_pcie_map_bus,
		/* PCI/NVMe: converts bus/devfn/offset to MMIO address for NVMe config cycles. */
		.read       = hisi_pcie_rd_conf,
		/* PCI/NVMe: reads NVMe endpoint/switch/RC config space (BAR, cap, status). */
		.write      = hisi_pcie_wr_conf,
		/* PCI/NVMe: writes NVMe endpoint/switch/RC config space (CMD, MSI-X, AER). */
	}
};

#endif

#ifdef CONFIG_PCI_HISI
/* PCI/NVMe: DT/ACPI-less platform driver path for HiSilicon PCIe host controller;
 *           used on systems where NVMe SSDs are described in device tree. */

static int hisi_pcie_platform_init(struct pci_config_window *cfg)
/* PCI/NVMe: platform probe init callback registered in hisi_pcie_platform_ops;
 *           maps RC registers from DT "reg" so NVMe enumeration can start. */
{
	struct device *dev = cfg->parent;
	/* PCI/NVMe: platform_device dev backing this PCIe root complex. */
	struct hisi_pcie *pcie;
	/* PCI/NVMe: per-RC private state. */
	struct platform_device *pdev = to_platform_device(dev);
	/* PCI/NVMe: platform device carrying DT resources and OF node. */
	struct resource *res;
	/* PCI/NVMe: DT resource describing the RC own register region. */

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* PCI/NVMe: allocate controller private state. */
	if (!pcie)
		return -ENOMEM;
	/* PCI/NVMe: abort if driver state cannot be allocated. */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	/* PCI/NVMe: fetch the second "reg" entry (index 1) which holds RC registers;
	 *           reg[0] is usually the ECAM config space itself. */
	if (!res) {
		/* PCI/NVMe: missing RC register region; root port cannot be programmed. */
		dev_err(dev, "missing \"reg[1]\"property\n");
		return -EINVAL;
		/* PCI/NVMe: invalid DT prevents host probe and NVMe device discovery. */
	}

	pcie->reg_base = devm_pci_remap_cfgspace(dev, res->start, resource_size(res));
	/* PCI/NVMe: ioremap the RC register region; this is where link training,
	 *           root port control, and bus-numbering state live. */
	if (!pcie->reg_base)
		return -ENOMEM;
	/* PCI/NVMe: mapping failed; cannot access RC registers, fail probe. */

	cfg->priv = pcie;
	/* PCI/NVMe: store private state in config window for map_bus callback. */
	return 0;
	/* PCI/NVMe: host probe succeeds; PCI bus scan will look for NVMe endpoints. */
}

static const struct pci_ecam_ops hisi_pcie_platform_ops = {
	/* PCI/NVMe: ECAM ops registered for the platform driver (DT boot). */
	.init         =  hisi_pcie_platform_init,
	/* PCI/NVMe: platform-specific setup of RC register mapping. */
	.pci_ops      = {
		/* PCI/NVMe: config-space access methods used by PCI/NVMe enumeration. */
		.map_bus    = hisi_pcie_map_bus,
		/* PCI/NVMe: maps config cycles to ECAM or RC private register region. */
		.read       = hisi_pcie_rd_conf,
		/* PCI/NVMe: reads config headers/capabilities of NVMe SSD and switches. */
		.write      = hisi_pcie_wr_conf,
		/* PCI/NVMe: writes config headers/capabilities (e.g. enabling NVMe BAR). */
	}
};

static const struct of_device_id hisi_pcie_almost_ecam_of_match[] = {
	/* PCI/NVMe: OF match table binding compatible DT nodes to this driver;
	 *           determines which HiSilicon SoCs use this host controller. */
	{
		.compatible =  "hisilicon,hip06-pcie-ecam",
		/* PCI/NVMe: HiSilicon HIP06 SoC PCIe host compatible string. */
		.data	    =  &hisi_pcie_platform_ops,
		/* PCI/NVMe: associate this SoC with the platform ECAM ops above. */
	},
	{
		.compatible =  "hisilicon,hip07-pcie-ecam",
		/* PCI/NVMe: HiSilicon HIP07 SoC PCIe host compatible string. */
		.data       =  &hisi_pcie_platform_ops,
		/* PCI/NVMe: same ECAM ops for HIP07. */
	},
	{},
	/* PCI/NVMe: sentinel terminating the match table. */
};

static struct platform_driver hisi_pcie_almost_ecam_driver = {
	/* PCI/NVMe: platform driver structure binding this controller to the device tree;
	 *           when bound, the RC is ready to enumerate PCIe NVMe endpoints. */
	.probe  = pci_host_common_probe,
	/* PCI/NVMe: generic PCI host probe parses DT ranges/IRQs, creates root bus,
	 *           then scans downstream buses where the NVMe SSD will appear. */
	.driver = {
		   .name = "hisi-pcie-almost-ecam",
		   /* PCI/NVMe: driver name visible in sysfs / modules. */
		   .of_match_table = hisi_pcie_almost_ecam_of_match,
		   /* PCI/NVMe: DT compatible matching for HIP06/HIP07. */
		   .suppress_bind_attrs = true,
		   /* PCI/NVMe: disable manual bind/unbind via sysfs;
		    *           PCIe host must stay bound while NVMe drives are in use. */
	},
};
builtin_platform_driver(hisi_pcie_almost_ecam_driver);
/* PCI/NVMe: register the platform driver at core_initcall time so PCIe root bus
 *           is created early and NVMe SSDs can be probed during normal device_initcall. */

#endif
#endif
