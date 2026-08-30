// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for handling the PCIe controller errors on
 * HiSilicon HIP SoCs.
 *
 * Copyright (c) 2020 HiSilicon Limited.
 */

#include <linux/acpi.h>           /* PCI/NVMe: ACPI enumeration used by PCIe root ports where NVMe SSDs are discovered */
#include <acpi/ghes.h>            /* PCI/NVMe: GHES firmware error records for PCIe AER-like fatal errors affecting NVMe */
#include <linux/bitops.h>         /* PCI/NVMe: bitmap parsing for error valid-bit masks */
#include <linux/delay.h>          /* PCI/NVMe: PCI spec recovery delays after root port reset */
#include <linux/pci.h>            /* PCI/NVMe: core PCI/NVMe bus, devfn, hotplug and rescan APIs */
#include <linux/platform_device.h> /* PCI/NVMe: platform driver binding for the error handler device */
#include <linux/kfifo.h>          /* PCI/NVMe: unused here, kept for possible firmware error queuing */
#include <linux/spinlock.h>       /* PCI/NVMe: unused here, kept for notifier concurrency protection */

/* HISI PCIe controller error definitions */
#define HISI_PCIE_ERR_MISC_REGS	33	/* PCI/NVMe: 33 extra misc regs logged by firmware per controller error; may belong to root port serving NVMe */

#define HISI_PCIE_LOCAL_VALID_VERSION		BIT(0)	/* PCI/NVMe: version field valid */
#define HISI_PCIE_LOCAL_VALID_SOC_ID		BIT(1)	/* PCI/NVMe: SoC ID valid */
#define HISI_PCIE_LOCAL_VALID_SOCKET_ID		BIT(2)	/* PCI/NVMe: socket ID valid; matches the "socket" DT/ACPI prop of this handler instance */
#define HISI_PCIE_LOCAL_VALID_NIMBUS_ID		BIT(3)	/* PCI/NVMe: nimbus/bridge ID valid */
#define HISI_PCIE_LOCAL_VALID_SUB_MODULE_ID	BIT(4)	/* PCI/NVMe: PCIe layer (AP/TL/MAC/DL/SDI) valid; layer where NVMe TLPs may fail */
#define HISI_PCIE_LOCAL_VALID_CORE_ID		BIT(5)	/* PCI/NVMe: controller core ID valid; groups root ports sharing NVMe downstream links */
#define HISI_PCIE_LOCAL_VALID_PORT_ID		BIT(6)	/* PCI/NVMe: port ID valid; identifies root port under which NVMe devices are enumerated */
#define HISI_PCIE_LOCAL_VALID_ERR_TYPE		BIT(7)	/* PCI/NVMe: vendor-specific error type valid */
#define HISI_PCIE_LOCAL_VALID_ERR_SEVERITY	BIT(8)	/* PCI/NVMe: severity valid; non-recoverable errors typically kill NVMe I/O */
#define HISI_PCIE_LOCAL_VALID_ERR_MISC		9	/* PCI/NVMe: start bit index of the 33 misc register dump */

static guid_t hisi_pcie_sec_guid =
	GUID_INIT(0xB2889FC9, 0xE7D7, 0x4F9D,
		  0xA8, 0x67, 0xAF, 0x42, 0xE9, 0x8B, 0xE7, 0x72);
		  /* NVMe: GUID of the firmware CPER section carrying HiSilicon PCIe controller errors;
		   * matches GHES records before handling an NVMe-related controller fault */

/*
 * Firmware reports the socket port ID where the error occurred.  These
 * macros convert that to the core ID and core port ID required by the
 * ACPI reset method.
 */
#define HISI_PCIE_PORT_ID(core, v)       (((v) >> 1) + ((core) << 3))	/* NVMe: converts firmware core/port into an absolute port ID used to locate the root port that may own an NVMe SSD */
#define HISI_PCIE_CORE_ID(v)             ((v) >> 3)			/* NVMe: extracts core ID from absolute port ID for ACPI RST */
#define HISI_PCIE_CORE_PORT_ID(v)        (((v) & 7) << 1)		/* NVMe: extracts per-core port ID for ACPI RST */

struct hisi_pcie_error_data {
	u64	val_bits;		/* PCI/NVMe: bitmask telling which fields below are valid in this firmware error record */
	u8	version;		/* PCI/NVMe: structure version of the error record */
	u8	soc_id;			/* PCI/NVMe: SoC identifier */
	u8	socket_id;		/* PCI/NVMe: socket where the failing PCIe controller/ root port resides; must match this handler instance */
	u8	nimbus_id;		/* PCI/NVMe: nimbus/bridge within the socket */
	u8	sub_module_id;		/* PCI/NVMe: PCIe protocol layer reporting the error; NVMe traffic traverses all these layers */
	u8	core_id;		/* PCI/NVMe: PCIe controller core number; a core may host multiple root ports with NVMe endpoints */
	u8	port_id;		/* PCI/NVMe: port within core; maps to root port whose downstream NVMe devices may disappear */
	u8	err_severity;		/* PCI/NVMe: recoverable/fatal/corrected/none; only recoverable triggers root-port reset and NVMe re-enumeration */
	u16	err_type;		/* PCI/NVMe: vendor-specific error type code for diagnostics */
	u8	reserv[2];		/* PCI/NVMe: padding to align err_misc[] */
	u32	err_misc[HISI_PCIE_ERR_MISC_REGS];	/* PCI/NVMe: raw controller registers captured by firmware; useful when NVMe AER logs are insufficient */
};

struct hisi_pcie_error_private {
	struct notifier_block	nb;	/* PCI/NVMe: GHES vendor-record notifier registered with APEI */
	struct device *dev;		/* PCI/NVMe: platform_device dev pointer; used for property read and logging */
};

enum hisi_pcie_submodule_id {
	HISI_PCIE_SUB_MODULE_ID_AP,	/* PCI/NVMe: Application layer (NVMe submission/completion queue semantics visible here only via TLP) */
	HISI_PCIE_SUB_MODULE_ID_TL,	/* PCI/NVMe: Transaction layer; NVMe MRd/MWr TLPs and completions are handled here */
	HISI_PCIE_SUB_MODULE_ID_MAC,	/* PCI/NVMe: MAC/PHY logical layer; link errors abort NVMe DMA */
	HISI_PCIE_SUB_MODULE_ID_DL,	/* PCI/NVMe: Data link layer; Ack/Nak and replay affect NVMe reliability */
	HISI_PCIE_SUB_MODULE_ID_SDI,	/* PCI/NVMe: SDI/fabric interface layer on SoC side */
};

static const char * const hisi_pcie_sub_module[] = {
	[HISI_PCIE_SUB_MODULE_ID_AP]	= "AP Layer",
	[HISI_PCIE_SUB_MODULE_ID_TL]	= "TL Layer",
	[HISI_PCIE_SUB_MODULE_ID_MAC]	= "MAC Layer",
	[HISI_PCIE_SUB_MODULE_ID_DL]	= "DL Layer",
	[HISI_PCIE_SUB_MODULE_ID_SDI]	= "SDI Layer",
};

enum hisi_pcie_err_severity {
	HISI_PCIE_ERR_SEV_RECOVERABLE,	/* PCI/NVMe: recoverable; driver will reset the root port and re-enumerate downstream NVMe devices */
	HISI_PCIE_ERR_SEV_FATAL,	/* PCI/NVMe: fatal; no automated recovery, NVMe I/O on this link is expected to fail */
	HISI_PCIE_ERR_SEV_CORRECTED,	/* PCI/NVMe: corrected; logged only, NVMe traffic continues */
	HISI_PCIE_ERR_SEV_NONE,		/* PCI/NVMe: informational severity */
};

static const char * const hisi_pcie_error_sev[] = {
	[HISI_PCIE_ERR_SEV_RECOVERABLE]	= "recoverable",
	[HISI_PCIE_ERR_SEV_FATAL]	= "fatal",
	[HISI_PCIE_ERR_SEV_CORRECTED]	= "corrected",
	[HISI_PCIE_ERR_SEV_NONE]	= "none",
};

static const char *hisi_pcie_get_string(const char * const *array,
					size_t n, u32 id)
{
	u32 index;	/* PCI/NVMe: loop index for translating numeric IDs to human-readable layer/severity strings */

	for (index = 0; index < n; index++) {	/* PCI/NVMe: scan descriptor table */
		if (index == id && array[index])	/* PCI/NVMe: return matching string if ID is within bounds and slot is populated */
			return array[index];
	}

	return "unknown";	/* PCI/NVMe: firmware reported an unrecognized submodule or severity */
}

static int hisi_pcie_port_reset(struct platform_device *pdev,
				u32 chip_id, u32 port_id)
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: device used for logging and ACPI handle lookup */
	acpi_handle handle = ACPI_HANDLE(dev);	/* PCI/NVMe: ACPI handle of this error handler platform device */
	union acpi_object arg[3];		/* PCI/NVMe: arguments for the ACPI _RST method: chip, core, core-port */
	struct acpi_object_list arg_list;	/* PCI/NVMe: object list passed to acpi_evaluate_integer() */
	acpi_status s;				/* PCI/NVMe: ACPI status */
	unsigned long long data = 0;		/* PCI/NVMe: reset return value; zero means success */

	arg[0].type = ACPI_TYPE_INTEGER;	/* PCI/NVMe: first argument type */
	arg[0].integer.value = chip_id;		/* PCI/NVMe: chip/socket argument for ACPI reset */
	arg[1].type = ACPI_TYPE_INTEGER;	/* PCI/NVMe: second argument type */
	arg[1].integer.value = HISI_PCIE_CORE_ID(port_id);	/* PCI/NVMe: core ID derived from absolute port ID */
	arg[2].type = ACPI_TYPE_INTEGER;	/* PCI/NVMe: third argument type */
	arg[2].integer.value = HISI_PCIE_CORE_PORT_ID(port_id);	/* PCI/NVMe: per-core port ID derived from absolute port ID */

	arg_list.count = 3;			/* PCI/NVMe: three arguments to _RST */
	arg_list.pointer = arg;			/* PCI/NVMe: point to argument array */

	s = acpi_evaluate_integer(handle, "RST", &arg_list, &data);	/* PCI/NVMe: invoke firmware reset for the root port; downstream NVMe devices lose link briefly */
	if (ACPI_FAILURE(s)) {
		dev_err(dev, "No RST method\n");				/* PCI/NVMe: firmware lacks reset method, cannot recover NVMe link */
		return -EIO;
	}

	if (data) {
		dev_err(dev, "Failed to Reset\n");	/* PCI/NVMe: reset failed; NVMe endpoint may stay unreachable */
		return -EIO;
	}

	return 0;	/* PCI/NVMe: root port reset succeeded, link may retrain so NVMe can come back */
}

static int hisi_pcie_port_do_recovery(struct platform_device *dev,
				      u32 chip_id, u32 port_id)
{
	acpi_status s;				/* PCI/NVMe: ACPI status */
	struct device *device = &dev->dev;	/* PCI/NVMe: device for logging */
	acpi_handle root_handle = ACPI_HANDLE(device);	/* PCI/NVMe: handle of the error handler platform device */
	struct acpi_pci_root *pci_root;		/* PCI/NVMe: ACPI PCI root descriptor for the segment */
	struct pci_bus *root_bus;		/* PCI/NVMe: root bus under which NVMe endpoints were enumerated */
	struct pci_dev *pdev;			/* PCI/NVMe: root port PCI device that may host an NVMe SSD downstream */
	u32 domain, busnr, devfn;		/* PCI/NVMe: PCI domain, root bus number, and root port device/function */

	s = acpi_get_parent(root_handle, &root_handle);	/* PCI/NVMe: walk up to the ACPI PCI root object associated with this controller */
	if (ACPI_FAILURE(s))
		return -ENODEV;	/* PCI/NVMe: cannot find PCI root, recovery impossible */
	pci_root = acpi_pci_find_root(root_handle);	/* PCI/NVMe: lookup kernel struct for the PCI host bridge */
	if (!pci_root)
		return -ENODEV;	/* PCI/NVMe: no host bridge, cannot locate NVMe root port */
	root_bus = pci_root->bus;		/* PCI/NVMe: root PCI bus where NVMe devices were originally discovered */
	domain = pci_root->segment;		/* PCI/NVMe: PCI segment/domain number used to address root port uniquely */

	busnr = root_bus->number;		/* PCI/NVMe: root bus number (typically 0) */
	devfn = PCI_DEVFN(port_id, 0);		/* PCI/NVMe: build root port devfn from absolute port ID; NVMe SSDs are behind this root port */
	pdev = pci_get_domain_bus_and_slot(domain, busnr, devfn);	/* PCI/NVMe: find the root port pci_dev; NVMe downstream devices hang off this bridge */
	if (!pdev) {
		dev_info(device, "Fail to get root port %04x:%02x:%02x.%d device\n",
			 domain, busnr, PCI_SLOT(devfn), PCI_FUNC(devfn));
		return -ENODEV;	/* PCI/NVMe: root port already gone, nothing to reset */
	}

	pci_stop_and_remove_bus_device_locked(pdev);	/* PCI/NVMe: tear down root port and all downstream devices (including NVMe SSDs) so they can be rescanned cleanly */
	pci_dev_put(pdev);				/* PCI/NVMe: drop reference taken by pci_get_domain_bus_and_slot() */

	if (hisi_pcie_port_reset(dev, chip_id, port_id))	/* PCI/NVMe: reset the hardware root port via ACPI _RST */
		return -EIO;	/* PCI/NVMe: reset failed, NVMe link remains down */

	/*
	 * The initialization time of subordinate devices after
	 * hot reset is no more than 1s, which is required by
	 * the PCI spec v5.0 sec 6.6.1. The time will shorten
	 * if Readiness Notifications mechanisms are used. But
	 * wait 1s here to adapt any conditions.
	 */
	ssleep(1UL);	/* PCI/NVMe: wait for root port and downstream NVMe SSD to finish link training and readiness */

	/* add root port and downstream devices */
	pci_lock_rescan_remove();	/* PCI/NVMe: serialize PCI rescan/removal to protect NVMe probe/remove callbacks */
	pci_rescan_bus(root_bus);	/* PCI/NVMe: re-enumerate the root bus so the NVMe driver can re-probe the SSD */
	pci_unlock_rescan_remove();	/* PCI/NVMe: release the rescan lock */

	return 0;	/* PCI/NVMe: recovery sequence complete; NVMe device should be back on the bus */
}

static void hisi_pcie_handle_error(struct platform_device *pdev,
				   const struct hisi_pcie_error_data *edata)
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: device for logging */
	int idx, rc;				/* PCI/NVMe: loop index and recovery return code */
	const unsigned long valid_bits[] = {BITMAP_FROM_U64(edata->val_bits)};
						/* PCI/NVMe: convert u64 valid bitmask into a bitmap for for_each_set_bit_from() */

	if (edata->val_bits == 0) {	/* PCI/NVMe: firmware sent an empty record, no useful NVMe-related information */
		dev_warn(dev, "%s: no valid error information\n", __func__);
		return;
	}

	dev_info(dev, "\nHISI : HIP : PCIe controller error\n");
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SOC_ID)
		dev_info(dev, "Table version = %d\n", edata->version);
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SOCKET_ID)
		dev_info(dev, "Socket ID = %d\n", edata->socket_id);
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_NIMBUS_ID)
		dev_info(dev, "Nimbus ID = %d\n", edata->nimbus_id);
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SUB_MODULE_ID)
		dev_info(dev, "Sub Module = %s\n",
			 hisi_pcie_get_string(hisi_pcie_sub_module,
				      ARRAY_SIZE(hisi_pcie_sub_module),
				      edata->sub_module_id));
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_CORE_ID)
		dev_info(dev, "Core ID = core%d\n", edata->core_id);
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_PORT_ID)
		dev_info(dev, "Port ID = port%d\n", edata->port_id);
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_ERR_SEVERITY)
		dev_info(dev, "Error severity = %s\n",
			 hisi_pcie_get_string(hisi_pcie_error_sev,
				      ARRAY_SIZE(hisi_pcie_error_sev),
				      edata->err_severity));
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_ERR_TYPE)
		dev_info(dev, "Error type = 0x%x\n", edata->err_type);

	dev_info(dev, "Reg Dump:\n");
	idx = HISI_PCIE_LOCAL_VALID_ERR_MISC;	/* PCI/NVMe: start at the first misc register bit */
	for_each_set_bit_from(idx, valid_bits,
			      HISI_PCIE_LOCAL_VALID_ERR_MISC + HISI_PCIE_ERR_MISC_REGS)	/* PCI/NVMe: iterate over every valid misc register reported by firmware; helps diagnose NVMe DMA/TLP faults */
		dev_info(dev, "ERR_MISC_%d = 0x%x\n", idx - HISI_PCIE_LOCAL_VALID_ERR_MISC,
			 edata->err_misc[idx - HISI_PCIE_LOCAL_VALID_ERR_MISC]);

	if (edata->err_severity != HISI_PCIE_ERR_SEV_RECOVERABLE)
		return;	/* PCI/NVMe: fatal/corrected/none errors do not trigger root-port reset; only recoverable ones attempt NVMe recovery */

	/* Recovery for the PCIe controller errors, try reset
	 * PCI port for the error recovery
	 */
	rc = hisi_pcie_port_do_recovery(pdev, edata->socket_id,
			HISI_PCIE_PORT_ID(edata->core_id, edata->port_id));
			/* PCI/NVMe: reset the affected root port and rescan so NVMe driver can rebind the SSD */
	if (rc)
		dev_info(dev, "fail to do hisi pcie port reset\n");
		/* PCI/NVMe: recovery failed; NVMe device may need manual rescan or reboot */
}

static int hisi_pcie_notify_error(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct acpi_hest_generic_data *gdata = data;	/* PCI/NVMe: GHES generic error data from firmware CPER */
	const struct hisi_pcie_error_data *error_data = acpi_hest_get_payload(gdata);	/* PCI/NVMe: extract HiSilicon PCIe payload; may describe a root-port error that affects NVMe */
	struct hisi_pcie_error_private *priv;	/* PCI/NVMe: per-device private data */
	struct device *dev;			/* PCI/NVMe: platform device dev pointer */
	struct platform_device *pdev;		/* PCI/NVMe: platform_device retrieved from dev for recovery */
	guid_t err_sec_guid;			/* PCI/NVMe: GUID of the CPER section_type */
	u8 socket;				/* PCI/NVMe: socket number of this handler instance */

	import_guid(&err_sec_guid, gdata->section_type);	/* PCI/NVMe: parse the section_type GUID from the firmware record */
	if (!guid_equal(&err_sec_guid, &hisi_pcie_sec_guid))
		return NOTIFY_DONE;	/* PCI/NVMe: not a HiSilicon PCIe controller error, ignore it */

	priv = container_of(nb, struct hisi_pcie_error_private, nb);	/* PCI/NVMe: recover private data from the notifier_block */
	dev = priv->dev;	/* PCI/NVMe: device associated with this handler */

	if (device_property_read_u8(dev, "socket", &socket))
		return NOTIFY_DONE;	/* PCI/NVMe: cannot read socket property, skip to avoid wrong NVMe root-port reset */

	if (error_data->socket_id != socket)
		return NOTIFY_DONE;	/* PCI/NVMe: error belongs to a different socket, do not disturb NVMe devices on this socket */

	pdev = container_of(dev, struct platform_device, dev);	/* PCI/NVMe: obtain platform_device from device */
	hisi_pcie_handle_error(pdev, error_data);	/* PCI/NVMe: decode error and attempt root-port reset if recoverable, enabling NVMe re-enumeration */

	return NOTIFY_OK;	/* PCI/NVMe: handled the notification */
}

static int hisi_pcie_error_handler_probe(struct platform_device *pdev)
{
	struct hisi_pcie_error_private *priv;	/* PCI/NVMe: per-instance private data */
	int ret;				/* PCI/NVMe: return value */

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);	/* PCI/NVMe: allocate memory bound to the platform device lifecycle */
	if (!priv)
		return -ENOMEM;	/* PCI/NVMe: out of memory, NVMe error handling unavailable for this socket */

	priv->nb.notifier_call = hisi_pcie_notify_error;	/* PCI/NVMe: register callback invoked for every GHES vendor record; filters for HiSilicon PCIe errors affecting NVMe links */
	priv->dev = &pdev->dev;					/* PCI/NVMe: remember device for socket match and recovery */
	ret = devm_ghes_register_vendor_record_notifier(&pdev->dev, &priv->nb);	/* PCI/NVMe: subscribe to firmware error notifications via APEI/GHES */
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to register hisi pcie controller error handler with apei\n");
		return ret;	/* PCI/NVMe: registration failed, no automatic NVMe recovery for this controller */
	}

	return 0;	/* PCI/NVMe: handler registered, ready to recover NVMe root ports on controller errors */
}

static const struct acpi_device_id hisi_pcie_acpi_match[] = {
	{ "HISI0361", 0 },	/* PCI/NVMe: ACPI HID for the HiSilicon PCIe controller error handler device */
	{ }
};

static struct platform_driver hisi_pcie_error_handler_driver = {
	.driver = {
		.name	= "hisi-pcie-error-handler",			/* PCI/NVMe: driver name shown in sysfs for the error handler */
		.acpi_match_table = hisi_pcie_acpi_match,	/* PCI/NVMe: bind to HISI0361 ACPI devices */
	},
	.probe		= hisi_pcie_error_handler_probe,	/* PCI/NVMe: called at boot/hotplug to register the GHES notifier for this controller's NVMe links */
};
module_platform_driver(hisi_pcie_error_handler_driver);	/* PCI/NVMe: register the platform driver; enables runtime NVMe root-port recovery on these SoCs */

MODULE_DESCRIPTION("HiSilicon HIP PCIe controller error handling driver");
