// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Hot Plug Controller Driver for System z
 *
 * Copyright 2012 IBM Corp.
 *
 * Author(s):
 *   Jan Glauber <jang@linux.vnet.ibm.com>
 */

/* PCI/NVMe: zPCI hotplug slot ops affect NVMe SSDs behind zPCI functions */
#define pr_fmt(fmt) "zpci: " fmt

/* PCI/NVMe: core headers used by PCIe host drivers and NVMe pci.c */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <asm/pci_debug.h>
#include <asm/sclp.h>

/* PCI/NVMe: hotplug slot name buffer sized for "xxxxxxxx" fid strings */
#define SLOT_NAME_SIZE	10

/* PCI/NVMe: enable_slot() called by pci_hp core before NVMe probe can run */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	/* PCI/NVMe: derive the s390 zPCI device wrapping this hotplug slot */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	int rc;

	/* PCI/NVMe: serialize with disable/reset that may race NVMe bind/unbind */
	mutex_lock(&zdev->state_lock);
	/* PCI/NVMe: only allow enable from STANDBY, mirroring PCI slot power-up */
	if (zdev->state != ZPCI_FN_STATE_STANDBY) {
		rc = -EIO;
		goto out;
	}

	/* PCI/NVMe: ask firmware to make the PCIe function visible to the host */
	rc = sclp_pci_configure(zdev->fid);
	/* PCI/NVMe: tracepoint-like debug for enable failure path */
	zpci_dbg(3, "conf fid:%x, rc:%d\n", zdev->fid, rc);
	if (rc)
		goto out;
	/* PCI/NVMe: mark function CONFIGURED so pci_scan_child_bus can find it */
	zdev->state = ZPCI_FN_STATE_CONFIGURED;

	/* PCI/NVMe: trigger PCIe bus scan that can discover and probe NVMe pdev */
	rc = zpci_scan_configured_device(zdev, zdev->fh);
out:
	/* PCI/NVMe: release lock so pci_device_probe (e.g. nvme_probe) can proceed */
	mutex_unlock(&zdev->state_lock);
	return rc;
}

/* PCI/NVMe: disable_slot() is the inverse of enabling; stops NVMe traffic first */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	/* PCI/NVMe: locate zPCI device and, if already scanned, its pci_dev */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	struct pci_dev *pdev = NULL;
	int rc;

	/* PCI/NVMe: protect against concurrent enable/reset and NVMe remove */
	mutex_lock(&zdev->state_lock);
	/* PCI/NVMe: only disable when already configured; matches PCI remove flow */
	if (zdev->state != ZPCI_FN_STATE_CONFIGURED) {
		rc = -EIO;
		goto out;
	}

	/* PCI/NVMe: lookup pci_dev so we can block disable when SR-IOV VFs exist */
	pdev = pci_get_slot(zdev->zbus->bus, zdev->devfn);
	/* PCI/NVMe: refuse disable if NVMe PF still has VFs (like nvme-pci SR-IOV) */
	if (pdev && pci_num_vf(pdev)) {
		rc = -EBUSY;
		goto out;
	}

	/* PCI/NVMe: deconfigure function, quiesces DMA/MSI-X for attached NVMe */
	rc = zpci_deconfigure_device(zdev);
out:
	/* PCI/NVMe: drop pci_dev reference before releasing slot lock */
	if (pdev)
		pci_dev_put(pdev);
	/* PCI/NVMe: release state_lock so later enable/reset can run */
	mutex_unlock(&zdev->state_lock);
	return rc;
}

/* PCI/NVMe: reset_slot() performs PCIe function-level reset for a slot */
static int reset_slot(struct hotplug_slot *hotplug_slot, bool probe)
{
	/* PCI/NVMe: obtain zPCI device backing the hotplug slot */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	int rc = -EIO;

	/*
	 * If we can't get the zdev->state_lock the device state is
	 * currently undergoing a transition and we bail out - just
	 * the same as if the device's state is not configured at all.
	 */
	/* PCI/NVMe: avoid deadlock with NVMe probe/remove taking state_lock */
	if (!mutex_trylock(&zdev->state_lock))
		return rc;

	/* We can reset only if the function is configured */
	/* PCI/NVMe: FLR on unconfigured function is undefined; mirror PCIe spec */
	if (zdev->state != ZPCI_FN_STATE_CONFIGURED)
		goto out;

	/* PCI/NVMe: probe==true only checks readiness, no actual NVMe reset */
	if (probe) {
		rc = 0;
		goto out;
	}

	/* PCI/NVMe: issue platform reset; NVMe queues/registers must be re-init'd */
	rc = zpci_hot_reset_device(zdev);
out:
	/* PCI/NVMe: reset done; NVMe driver will see pdev again via re-enumeration */
	mutex_unlock(&zdev->state_lock);
	return rc;
}

/* PCI/NVMe: reports whether the zPCI function (and thus NVMe) is powered/configured */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* PCI/NVMe: convert hotplug slot to zPCI device */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);

	/* PCI/NVMe: 1 means device is configured and an NVMe controller could bind */
	*value = zpci_is_device_configured(zdev) ? 1 : 0;
	return 0;
}

static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* PCI/NVMe: zPCI slot always has a backing adapter; adapter is present */
	/* if the slot exists it always contains a function */
	*value = 1;
	return 0;
}

/* PCI/NVMe: slot operation callbacks registered with PCI hotplug core */
static const struct hotplug_slot_ops s390_hotplug_slot_ops = {
	/* PCI/NVMe: powers on/configures slot so NVMe can be discovered */
	.enable_slot =		enable_slot,
	/* PCI/NVMe: powers off/deconfigures slot after NVMe remove */
	.disable_slot =		disable_slot,
	/* PCI/NVMe: resets PCIe function; NVMe driver must re-setup BARs/MSI-X */
	.reset_slot =		reset_slot,
	/* PCI/NVMe: sysfs power status readable by user/admin managing NVMe */
	.get_power_status =	get_power_status,
	/* PCI/NVMe: sysfs adapter status: slot populated */
	.get_adapter_status =	get_adapter_status,
};

/* PCI/NVMe: register a zPCI function as a hotplug slot visible to userspace */
int zpci_init_slot(struct zpci_dev *zdev)
{
	/* PCI/NVMe: local buffer for slot name derived from function id */
	char name[SLOT_NAME_SIZE];
	/* PCI/NVMe: zPCI bus that will carry the NVMe pci_dev */
	struct zpci_bus *zbus = zdev->zbus;

	/* PCI/NVMe: attach s390 ops to generic hotplug_slot in zpci_dev */
	zdev->hotplug_slot.ops = &s390_hotplug_slot_ops;

	/* PCI/NVMe: slot name = fid in hex; identifies NVMe bay in sysfs */
	snprintf(name, SLOT_NAME_SIZE, "%08x", zdev->fid);
	/* PCI/NVMe: register slot on the zPCI bus at this devfn for NVMe scan */
	return pci_hp_register(&zdev->hotplug_slot, zbus->bus,
			       zdev->devfn, name);
}

/* PCI/NVMe: unregister slot, e.g. before destroying zPCI function backing NVMe */
void zpci_exit_slot(struct zpci_dev *zdev)
{
	/* PCI/NVMe: remove sysfs hotplug entries for the NVMe slot */
	pci_hp_deregister(&zdev->hotplug_slot);
}
