// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI PCI HotPlug glue functions to ACPI CA subsystem
 *
 * Copyright (C) 2002,2003 Takayoshi Kochi (t-kochi@bq.jp.nec.com)
 * Copyright (C) 2002 Hiroshi Aono (h-aono@ap.jp.nec.com)
 * Copyright (C) 2002,2003 NEC Corporation
 * Copyright (C) 2003-2005 Matthew Wilcox (willy@infradead.org)
 * Copyright (C) 2003-2005 Hewlett Packard
 * Copyright (C) 2005 Rajesh Shah (rajesh.shah@intel.com)
 * Copyright (C) 2005 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 */

/*
 * Lifetime rules for pci_dev:
 *  - The one in acpiphp_bridge has its refcount elevated by pci_get_slot()
 *    when the bridge is scanned and it loses a refcount when the bridge
 *    is removed.
 *  - When a P2P bridge is present, we elevate the refcount on the subordinate
 *    bus. It loses the refcount when the driver unloads.
 */

#define pr_fmt(fmt) "acpiphp_glue: " fmt	/* NVMe: prefix for acpiphp_glue printk messages; NVMe hotplug events logged here affect NVMe probe/remove */

#include <linux/module.h>	/* NVMe: module infrastructure; ACPIPHP is a loadable helper for NVMe PCIe slots */

#include <linux/kernel.h>	/* NVMe: kernel core APIs used while handling NVMe device hotplug notifications */
#include <linux/pci.h>	/* NVMe: PCI core definitions used to scan/rescan buses that may carry NVMe SSDs */
#include <linux/pci_hotplug.h>	/* NVMe: PCI hotplug framework; bridges slots to user space for NVMe add/remove */
#include <linux/pci-acpi.h>	/* NVMe: ACPI<->PCI glue; translates ACPI slot events into PCI bus rescans for NVMe */
#include <linux/pm_runtime.h>	/* NVMe: runtime PM helpers; keep bridge awake while rescanning for an NVMe device */
#include <linux/mutex.h>	/* NVMe: mutex for bridge list; serializes slot structures backing NVMe hotplug */
#include <linux/slab.h>	/* NVMe: kernel memory allocator for hotplug context/slot/bridge objects */
#include <linux/acpi.h>	/* NVMe: ACPI evaluation of _ADR/_STA/_EJ0/_SUN for NVMe-bearing slots */

#include "../pci.h"	/* NVMe: internal PCI core headers used during NVMe bus enumeration */
#include "acpiphp.h"	/* NVMe: ACPIPHP local definitions for slot/func/bridge tracking */

static LIST_HEAD(bridge_list);	/* NVMe: list of ACPIPHP bridges; an NVMe SSD may sit below any of these */
static DEFINE_MUTEX(bridge_mutex);	/* NVMe: protects bridge_list modifications during NVMe slot registration/removal */

static int acpiphp_hotplug_notify(struct acpi_device *adev, u32 type);	/* NVMe: ACPI notify handler entry; dispatches add/check/eject for NVMe slots */
static void acpiphp_post_dock_fixup(struct acpi_device *adev);	/* NVMe: dock fixup repairs bridge bus numbers after NVMe dock event */
static void acpiphp_sanitize_bus(struct pci_bus *bus);	/* NVMe: removes NVMe functions that failed to obtain required BAR resources */
static void hotplug_event(u32 type, struct acpiphp_context *context);	/* NVMe: core ACPI hotplug event dispatch affecting NVMe PCIe devices */
static void free_bridge(struct kref *kref);	/* NVMe: releases bridge and its NVMe slot structures when refcount reaches zero */

/**
 * acpiphp_init_context - Create hotplug context and grab a reference to it.
 * @adev: ACPI device object to create the context for.
 *
 * Call under acpi_hp_context_lock.
 */
static struct acpiphp_context *acpiphp_init_context(struct acpi_device *adev)	/* NVMe: allocate and initialize ACPI hotplug context for a potential NVMe device */
{
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */

	context = kzalloc_obj(*context);	/* NVMe: allocate zeroed hotplug context; failure leaves NVMe slot unmonitored */
	if (!context)	/* NVMe: parent not tracked by ACPIPHP; ignore this NVMe bridge */
		return NULL;	/* NVMe: caller must handle missing hotplug context for this device/slot */

	context->refcount = 1;	/* NVMe: initial reference held by ACPI device until NVMe slot is removed */
	context->hp.notify = acpiphp_hotplug_notify;	/* NVMe: route ACPI device notifications to acpiphp for NVMe surprise add/remove */
	context->hp.fixup = acpiphp_post_dock_fixup;	/* NVMe: register post-dock fixup to repair bridge config used by NVMe */
	acpi_set_hp_context(adev, &context->hp);	/* NVMe: attach hotplug context to ACPI companion of possible NVMe device */
	return context;	/* NVMe: return from ACPI hotplug helper to caller */
}

/**
 * acpiphp_get_context - Get hotplug context and grab a reference to it.
 * @adev: ACPI device object to get the context for.
 *
 * Call under acpi_hp_context_lock.
 */
static struct acpiphp_context *acpiphp_get_context(struct acpi_device *adev)	/* NVMe: lookup and refcount an existing ACPIPHP context for an NVMe companion */
{
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */

	if (!adev->hp)	/* NVMe: device has no hotplug context yet (not a tracked NVMe slot) */
		return NULL;	/* NVMe: caller must handle missing hotplug context for this device/slot */

	context = to_acpiphp_context(adev->hp);	/* NVMe: convert generic ACPI hotplug context to ACPIPHP context */
	context->refcount++;	/* NVMe: take a reference while operating on this NVMe slot context */
	return context;	/* NVMe: return from ACPI hotplug helper to caller */
}

/**
 * acpiphp_put_context - Drop a reference to ACPI hotplug context.
 * @context: ACPI hotplug context to drop a reference to.
 *
 * The context object is removed if there are no more references to it.
 *
 * Call under acpi_hp_context_lock.
 */
static void acpiphp_put_context(struct acpiphp_context *context)	/* NVMe: drop reference and free context when last NVMe slot user leaves */
{
	if (--context->refcount)	/* NVMe: still referenced by other NVMe hotplug paths; keep context alive */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	WARN_ON(context->bridge);	/* NVMe: bridge should already be detached before freeing NVMe context */
	context->hp.self->hp = NULL;	/* NVMe: clear ACPI device hotplug pointer; no more NVMe events via this context */
	kfree(context);	/* NVMe: free ACPIPHP context after NVMe slot is fully gone */
}

static inline void get_bridge(struct acpiphp_bridge *bridge)	/* NVMe: take a reference on the bridge that may carry NVMe downstream ports */
{
	kref_get(&bridge->ref);	/* NVMe: increment bridge refcount before accessing NVMe slot list */
}

static inline void put_bridge(struct acpiphp_bridge *bridge)	/* NVMe: release bridge reference; may tear down NVMe slot structures */
{
	kref_put(&bridge->ref, free_bridge);	/* NVMe: last reference triggers free_bridge to release NVMe slots */
}

static struct acpiphp_context *acpiphp_grab_context(struct acpi_device *adev)	/* NVMe: lock-protected context lookup that also pins parent bridge for NVMe */
{
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */

	context = acpiphp_get_context(adev);	/* NVMe: get parent context for this bridge (was registered as a function) */
	if (!context)	/* NVMe: parent not tracked by ACPIPHP; ignore this NVMe bridge */
		goto unlock;	/* NVMe: release lock and return without touching NVMe bridge */

	if (context->func.parent->is_going_away) {	/* NVMe: parent bridge is disappearing; reject NVMe operations on it */
		acpiphp_put_context(context);	/* NVMe: roll back context allocation when slot cannot be created */
		context = NULL;	/* NVMe: signal caller that this NVMe slot context is stale */
		goto unlock;	/* NVMe: release lock and return without touching NVMe bridge */
	}

	get_bridge(context->func.parent);	/* NVMe: pin parent bridge for this NVMe sub-bridge */
	acpiphp_put_context(context);	/* NVMe: roll back context allocation when slot cannot be created */

unlock:	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
	return context;	/* NVMe: return from ACPI hotplug helper to caller */
}

static void acpiphp_let_context_go(struct acpiphp_context *context)	/* NVMe: release bridge reference taken by acpiphp_grab_context for NVMe */
{
	put_bridge(context->func.parent);	/* NVMe: release parent bridge reference for this NVMe bridge */
}

static void free_bridge(struct kref *kref)	/* NVMe: tear down bridge and all NVMe slots/functions beneath it */
{
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */
	struct acpiphp_bridge *bridge;	/* NVMe: new bridge object representing the bus above NVMe slots */
	struct acpiphp_slot *slot, *next;	/* NVMe: iterate over slots that may hold NVMe SSDs */
	struct acpiphp_func *func, *tmp;	/* NVMe: iterate over functions (possibly NVMe functions) in a slot */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */

	bridge = container_of(kref, struct acpiphp_bridge, ref);	/* NVMe: recover bridge object from kref before releasing NVMe slots */

	list_for_each_entry_safe(slot, next, &bridge->slots, node) {	/* NVMe: walk every ACPIPHP slot under this bridge (potential NVMe slots) */
		list_for_each_entry_safe(func, tmp, &slot->funcs, sibling)	/* NVMe: walk each function in the slot, which may be an NVMe function */
			acpiphp_put_context(func_to_context(func));	/* NVMe: release reference on context backing this NVMe function */

		kfree(slot);	/* NVMe: free slot structure after all NVMe functions are gone */
	}

	context = bridge->context;	/* NVMe: retrieve bridge context to release parent bridge reference */
	/* Root bridges will not have hotplug context. */
	if (context) {	/* NVMe: non-root bridge has a context referencing its parent NVMe bridge */
		/* Release the reference taken by acpiphp_enumerate_slots(). */
		put_bridge(context->func.parent);	/* NVMe: release parent bridge reference for this NVMe bridge */
		context->bridge = NULL;	/* NVMe: detach bridge from context so NVMe events no longer reference it */
		acpiphp_put_context(context);	/* NVMe: roll back context allocation when slot cannot be created */
	}

	put_device(&bridge->pci_bus->dev);	/* NVMe: release subordinate bus device reference used for NVMe enumeration */
	pci_dev_put(bridge->pci_dev);	/* NVMe: release upstream bridge device reference for NVMe bridge */
	kfree(bridge);	/* NVMe: free bridge allocation after failed NVMe slot enumeration */

	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
}

/**
 * acpiphp_post_dock_fixup - Post-dock fixups for PCI devices.
 * @adev: ACPI device object corresponding to a PCI device.
 *
 * TBD - figure out a way to only call fixups for systems that require them.
 */
static void acpiphp_post_dock_fixup(struct acpi_device *adev)	/* NVMe: repair bridge bus numbers after docking/undocking an NVMe bay */
{
	struct acpiphp_context *context = acpiphp_grab_context(adev);	/* NVMe: grab hotplug context for ACPI companion of NVMe bridge/device */
	struct pci_bus *bus;	/* NVMe: PCI bus serving the slot that may contain an NVMe SSD */
	u32 buses;	/* NVMe: raw PRIMARY/SECONDARY/SUBORDINATE bus register value */

	if (!context)	/* NVMe: parent not tracked by ACPIPHP; ignore this NVMe bridge */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	bus = context->func.slot->bus;	/* NVMe: locate PCI bus associated with this NVMe slot context */
	if (!bus->self)	/* NVMe: root bus has no upstream bridge; nothing to fixup for NVMe */
		goto out;	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */

	/* fixup bad _DCK function that rewrites
	 * secondary bridge on slot
	 */
	pci_read_config_dword(bus->self, PCI_PRIMARY_BUS, &buses);	/* NVMe: read bridge bus register; used to reach downstream NVMe devices */

	if (((buses >> 8) & 0xff) != bus->busn_res.start) {	/* NVMe: detect buggy _DCK that rewrote secondary bus under NVMe bridge */
		buses = (buses & 0xff000000)	/* NVMe: preserve top byte of bridge bus register while fixing NVMe path */
			| ((unsigned int)(bus->primary)     <<  0)	/* NVMe: rewrite primary bus number for NVMe downstream traffic */
			| ((unsigned int)(bus->busn_res.start)   <<  8)	/* NVMe: rewrite secondary bus to match resource used by NVMe devices */
			| ((unsigned int)(bus->busn_res.end) << 16);	/* NVMe: rewrite subordinate bus to cover all NVMe downstream buses */
		pci_write_config_dword(bus->self, PCI_PRIMARY_BUS, buses);	/* NVMe: commit repaired bus numbers so NVMe config cycles route correctly */
	}

 out:	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
	acpiphp_let_context_go(context);	/* NVMe: release bridge reference after handling NVMe notify */
}

/**
 * acpiphp_add_context - Add ACPIPHP context to an ACPI device object.
 * @handle: ACPI handle of the object to add a context to.
 * @lvl: Not used.
 * @data: The object's parent ACPIPHP bridge.
 * @rv: Not used.
 */
static acpi_status acpiphp_add_context(acpi_handle handle, u32 lvl, void *data,	/* NVMe: ACPI namespace callback that registers a function in an NVMe slot */
				       void **rv)	/* NVMe: unused return slot for ACPI walk */
{
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);	/* NVMe: fetch ACPI device object that may companion an NVMe controller */
	struct acpiphp_bridge *bridge = data;	/* NVMe: parent bridge that owns the slot where an NVMe device may appear */
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */
	struct acpiphp_slot *slot;	/* NVMe: slot iterator for cleaning up NVMe slots */
	struct acpiphp_func *newfunc;	/* NVMe: new function entry being added to an NVMe slot */
	acpi_status status = AE_OK;	/* NVMe: assume success until an ACPI evaluation fails for NVMe slot */
	unsigned long long adr;	/* NVMe: _ADR value encodes PCI device/function of possible NVMe device */
	int device, function;	/* NVMe: decoded PCI device/function for NVMe slot/function registration */
	struct pci_bus *pbus = bridge->pci_bus;	/* NVMe: PCI bus under this bridge where NVMe devices will be scanned */
	struct pci_dev *pdev = bridge->pci_dev;	/* NVMe: upstream PCI bridge device above the NVMe slots */
	u32 val;	/* NVMe: temporary value read from device vendor ID register */

	if (!adev)	/* NVMe: no ACPI companion; cannot use ACPIPHP for NVMe hotplug */
		return AE_OK;	/* NVMe: finished registering this ACPIPHP slot/function for NVMe */

	status = acpi_evaluate_integer(handle, "_ADR", NULL, &adr);	/* NVMe: read ACPI _ADR to discover PCI location of possible NVMe device */
	if (ACPI_FAILURE(status)) {	/* NVMe: namespace walk failed; NVMe slots may not be registered */
		if (status != AE_NOT_FOUND)	/* NVMe: log unexpected _ADR failures that may break NVMe enumeration */
			acpi_handle_warn(handle,	/* NVMe: warn about _ADR problems that prevent NVMe slot tracking */
				"can't evaluate _ADR (%#x)\n", status);	/* NVMe: include ACPI status so NVMe debug can correlate failures */
		return AE_OK;	/* NVMe: finished registering this ACPIPHP slot/function for NVMe */
	}

	device = (adr >> 16) & 0xffff;	/* NVMe: extract PCI device number from ACPI _ADR for NVMe slot */
	function = adr & 0xffff;	/* NVMe: extract PCI function number from ACPI _ADR for NVMe function */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
	context = acpiphp_init_context(adev);	/* NVMe: create hotplug context for this ACPI companion of NVMe device */
	if (!context) {	/* NVMe: context allocation failed; NVMe hotplug will not be tracked */
		acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
		acpi_handle_err(handle, "No hotplug context\n");	/* NVMe: invoke PCI/ACPI helper used during NVMe enumeration/hotplug */
		return AE_NOT_EXIST;	/* NVMe: abort slot registration due to missing hotplug context */
	}
	newfunc = &context->func;	/* NVMe: function descriptor lives inside the ACPIPHP context */
	newfunc->function = function;	/* NVMe: record PCI function number for this possible NVMe function */
	newfunc->parent = bridge;	/* NVMe: link function back to parent bridge carrying NVMe downstream */
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */

	/*
	 * If this is a dock device, its _EJ0 should be executed by the dock
	 * notify handler after calling _DCK.
	 */
	if (!is_dock_device(adev) && acpi_has_method(handle, "_EJ0"))	/* NVMe: mark ejectable function unless handled by dock driver for NVMe bay */
		newfunc->flags = FUNC_HAS_EJ0;	/* NVMe: function supports ACPI _EJ0 ejection (NVMe bay removable) */

	if (acpi_has_method(handle, "_STA"))	/* NVMe: function has ACPI _STA status method for NVMe presence */
		newfunc->flags |= FUNC_HAS_STA;	/* NVMe: record that _STA can report NVMe device/slot status */

	/* search for objects that share the same slot */
	list_for_each_entry(slot, &bridge->slots, node)	/* NVMe: look for an existing slot matching this NVMe device number */
		if (slot->device == device)	/* NVMe: found existing slot for this device number; reuse it */
			goto slot_found;	/* NVMe: skip allocation and attach function to existing NVMe slot */

	slot = kzalloc_obj(struct acpiphp_slot);	/* NVMe: allocate new ACPIPHP slot structure for this NVMe device number */
	if (!slot) {	/* NVMe: slot allocation failed; cannot track NVMe hotplug here */
		acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
		acpiphp_put_context(context);	/* NVMe: roll back context allocation when slot cannot be created */
		acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
		return AE_NO_MEMORY;	/* NVMe: propagate memory exhaustion to ACPI walk for NVMe slot */
	}

	slot->bus = bridge->pci_bus;	/* NVMe: bind slot to the PCI bus that will scan for NVMe devices */
	slot->device = device;	/* NVMe: store PCI device number identifying this NVMe slot */
	INIT_LIST_HEAD(&slot->funcs);	/* NVMe: initialize list of functions in this NVMe slot */

	list_add_tail(&slot->node, &bridge->slots);	/* NVMe: add new slot to bridge's slot list for NVMe enumeration */

	/*
	 * Expose slots to user space for functions that have _EJ0 or _RMV or
	 * are located in dock stations.  Do not expose them for devices handled
	 * by the native PCIe hotplug (PCIeHP) or standard PCI hotplug
	 * (SHPCHP), because that code is supposed to expose slots to user
	 * space in those cases.
	 */
	if ((acpi_pci_check_ejectable(pbus, handle) || is_dock_device(adev))	/* NVMe: expose slot to userspace if NVMe bay is ejectable or docked */
	    && !(pdev && hotplug_is_native(pdev))) {	/* NVMe: skip user-space slot if native PCIeHP already handles NVMe */
		unsigned long long sun;	/* NVMe: ACPI _SUN slot unique number shown to user for NVMe bay */
		int retval;	/* NVMe: return value from hotplug slot registration for NVMe */

		bridge->nr_slots++;	/* NVMe: count newly exposed slot that may host an NVMe device */
		status = acpi_evaluate_integer(handle, "_SUN", NULL, &sun);	/* NVMe: read ACPI _SUN to obtain user-visible slot number for NVMe */
		if (ACPI_FAILURE(status))	/* NVMe: _SUN absent; fall back to sequential slot number for NVMe */
			sun = bridge->nr_slots;	/* NVMe: use bridge-local slot counter as user-visible NVMe slot ID */

		pr_debug("found ACPI PCI Hotplug slot %llu at PCI %04x:%02x:%02x\n",	/* NVMe: debug log identifying the slot that may enumerate an NVMe SSD */
		    sun, pci_domain_nr(pbus), pbus->number, device);	/* NVMe: print domain/bus/device of the NVMe slot being registered */

		retval = acpiphp_register_hotplug_slot(slot, sun);	/* NVMe: register slot with PCI hotplug core for NVMe add/remove events */
		if (retval) {	/* NVMe: slot registration failed; NVMe bay may be invisible to user */
			slot->slot = NULL;	/* NVMe: clear hotplug slot pointer so NVMe user space cannot use it */
			bridge->nr_slots--;	/* NVMe: revert slot counter after failed NVMe slot registration */
			if (retval == -EBUSY)	/* NVMe: another hotplug driver already owns this NVMe slot */
				pr_warn("Slot %llu already registered by another hotplug driver\n", sun);	/* NVMe: warn that NVMe slot is managed by competing hotplug driver */
			else	/* NVMe: some other registration error for this NVMe slot */
				pr_warn("acpiphp_register_hotplug_slot failed (err code = 0x%x)\n", retval);	/* NVMe: warn about unexpected slot registration failure for NVMe */
		}
		/* Even if the slot registration fails, we can still use it. */
	}

 slot_found:	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
	newfunc->slot = slot;	/* NVMe: associate this function with its physical NVMe slot */
	list_add_tail(&newfunc->sibling, &slot->funcs);	/* NVMe: add function to slot's function list (possible NVMe functions) */

	if (pci_bus_read_dev_vendor_id(pbus, PCI_DEVFN(device, function),	/* NVMe: probe config space to see if an NVMe device is already present */
				       &val, 60*1000))	/* NVMe: 60ms timeout waiting for NVMe device to respond on the bus */
		slot->flags |= SLOT_ENABLED;	/* NVMe: mark slot enabled after successful NVMe enumeration */

	return AE_OK;	/* NVMe: finished registering this ACPIPHP slot/function for NVMe */
}

static void cleanup_bridge(struct acpiphp_bridge *bridge)	/* NVMe: unregister slots and mark bridge going-away before NVMe removal */
{
	struct acpiphp_slot *slot;	/* NVMe: slot iterator for cleaning up NVMe slots */
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	list_for_each_entry(slot, &bridge->slots, node) {	/* NVMe: check every ACPIPHP slot under this bridge for NVMe changes */
		list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
			struct acpi_device *adev = func_to_acpi_device(func);	/* NVMe: ACPI device companion of this possible NVMe function */

			acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
			adev->hp->notify = NULL;	/* NVMe: stop delivering ACPI hotplug notify events for this NVMe function */
			adev->hp->fixup = NULL;	/* NVMe: stop post-dock fixups for this NVMe function */
			acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
		}
		slot->flags |= SLOT_IS_GOING_AWAY;	/* NVMe: mark slot so concurrent NVMe hotplug events skip it */
		if (slot->slot)	/* NVMe: unregister user-space hotplug slot if NVMe bay was exposed */
			acpiphp_unregister_hotplug_slot(slot);	/* NVMe: remove slot from PCI hotplug core; user can no longer eject NVMe */
	}

	mutex_lock(&bridge_mutex);	/* NVMe: search global bridge list under lock for NVMe bus */
	list_del(&bridge->list);	/* NVMe: remove bridge from global list so no new NVMe scans reference it */
	mutex_unlock(&bridge_mutex);	/* NVMe: release lock before dropping NVMe bridge */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
	bridge->is_going_away = true;	/* NVMe: flag bridge so NVMe hotplug handlers bail out early */
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
}

/**
 * acpiphp_max_busnr - return the highest reserved bus number under the given bus.
 * @bus: bus to start search with
 */
static unsigned char acpiphp_max_busnr(struct pci_bus *bus)	/* NVMe: find highest reserved bus number under bridge for NVMe scan */
{
	struct pci_bus *tmp;	/* NVMe: child bus iterator when computing bus range for NVMe */
	unsigned char max, n;	/* NVMe: max bus number tracked while sizing NVMe downstream hierarchy */

	/*
	 * pci_bus_max_busnr will return the highest
	 * reserved busnr for all these children.
	 * that is equivalent to the bus->subordinate
	 * value.  We don't want to use the parent's
	 * bus->subordinate value because it could have
	 * padding in it.
	 */
	max = bus->busn_res.start;	/* NVMe: start bus number scan at this bridge's secondary bus */

	list_for_each_entry(tmp, &bus->children, node) {	/* NVMe: walk child buses to find deepest bus used by NVMe topology */
		n = pci_bus_max_busnr(tmp);	/* NVMe: query PCI core for highest reserved bus under child bridge */
		if (n > max)	/* NVMe: update max to include buses reachable to downstream NVMe */
			max = n;	/* NVMe: record new highest bus number for NVMe resource planning */
	}
	return max;	/* NVMe: return from ACPI hotplug helper to caller */
}

static void acpiphp_set_acpi_region(struct acpiphp_slot *slot)	/* NVMe: notify ACPI that PCI config region is connected for NVMe slot */
{
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
		/* _REG is optional, we don't care about if there is failure */
		acpi_evaluate_reg(func_to_handle(func),	/* NVMe: evaluate _REG so AML can access config space of NVMe device */
				  ACPI_ADR_SPACE_PCI_CONFIG,	/* NVMe: operate in PCI configuration space region for NVMe */
				  ACPI_REG_CONNECT);	/* NVMe: tell AML that PCI config space for NVMe slot is available */
	}
}

static void check_hotplug_bridge(struct acpiphp_slot *slot, struct pci_dev *dev)	/* NVMe: mark downstream switch/bridge as hotplug-capable for NVMe bays */
{
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	/* quirk, or pcie could set it already */
	if (dev->is_hotplug_bridge)	/* NVMe: bridge already marked hotplug-capable (e.g., by quirk or PCIeHP) */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	/*
	 * In the PCIe case, only Root Ports and Downstream Ports are capable of
	 * accommodating hotplug devices, so avoid marking Upstream Ports as
	 * "hotplug bridges".
	 */
	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM)	/* NVMe: upstream ports cannot be hotplug bridges; skip for NVMe topology */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
		if (PCI_FUNC(dev->devfn) == func->function) {	/* NVMe: this PCI device matches the ACPI function for NVMe slot */
			dev->is_hotplug_bridge = 1;	/* NVMe: flag bridge so PCI core treats downstream NVMe slots as hotpluggable */
			break;	/* NVMe: stop checking this device after first unassigned resource */
		}
	}
}

static int acpiphp_rescan_slot(struct acpiphp_slot *slot)	/* NVMe: rescan an ACPI slot to discover a newly inserted NVMe device */
{
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
		struct acpi_device *adev = func_to_acpi_device(func);	/* NVMe: ACPI device companion of this possible NVMe function */

		acpi_bus_scan(adev->handle);	/* NVMe: re-scan ACPI namespace for new NVMe companion device objects */
		if (acpi_device_enumerated(adev))	/* NVMe: if ACPI device now exists, power it on so NVMe can respond */
			acpi_device_set_power(adev, ACPI_STATE_D0);	/* NVMe: transition ACPI device to D0 before scanning for NVMe controller */
	}
	return pci_scan_slot(slot->bus, PCI_DEVFN(slot->device, 0));	/* NVMe: ask PCI core to scan all functions at this NVMe device number */
}

static void acpiphp_native_scan_bridge(struct pci_dev *bridge)	/* NVMe: scan non-native-hotplug bridges below a native-hotplug bridge for NVMe */
{
	struct pci_bus *bus = bridge->subordinate;	/* NVMe: bus below this bridge where NVMe devices may appear */
	struct pci_dev *dev;	/* NVMe: PCI device iterator for scanning NVMe functions/bridges */
	int max;	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */

	if (!bus)	/* NVMe: bridge has no subordinate bus; nothing to scan for NVMe */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	max = bus->busn_res.start;	/* NVMe: start bus number scan at this bridge's secondary bus */
	/* Scan already configured non-hotplug bridges */
	for_each_pci_bridge(dev, bus) {	/* NVMe: scan bridges in this slot that may carry NVMe downstream */
		if (!hotplug_is_native(dev))	/* NVMe: only scan bridges not already managed by native PCIeHP for NVMe */
			max = pci_scan_bridge(bus, dev, max, 0);	/* NVMe: scan already-configured non-hotplug bridge for NVMe downstream */
	}

	/* Scan non-hotplug bridges that need to be reconfigured */
	for_each_pci_bridge(dev, bus) {	/* NVMe: scan bridges in this slot that may carry NVMe downstream */
		if (hotplug_is_native(dev))	/* NVMe: conditional check on ACPI/NVMe hotplug state */
			continue;	/* NVMe: proceed to next item in NVMe hotplug iteration */

		max = pci_scan_bridge(bus, dev, max, 1);	/* NVMe: scan/reconfigure non-hotplug bridge to make room for NVMe */
		if (dev->subordinate) {	/* NVMe: if bridge now has a subordinate bus, allocate NVMe resources */
			pcibios_resource_survey_bus(dev->subordinate);	/* NVMe: survey host resources for this NVMe downstream bus */
			pci_bus_size_bridges(dev->subordinate);	/* NVMe: calculate bridge window sizes needed by NVMe devices */
			pci_bus_assign_resources(dev->subordinate);	/* NVMe: assign bus numbers and MMIO/IO windows for NVMe hierarchy */
		}
	}
}

/**
 * enable_slot - enable, configure a slot
 * @slot: slot to be enabled
 * @bridge: true if enable is for the whole bridge (not a single slot)
 *
 * This function should be called per *physical slot*,
 * not per each slot object in ACPI namespace.
 */
static void enable_slot(struct acpiphp_slot *slot, bool bridge)	/* NVMe: power on and enumerate a slot that may contain an NVMe SSD */
{
	struct pci_dev *dev;	/* NVMe: PCI device iterator for scanning NVMe functions/bridges */
	struct pci_bus *bus = slot->bus;	/* NVMe: bus serving this NVMe slot */
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	if (bridge && bus->self && hotplug_is_native(bus->self)) {	/* NVMe: native PCIeHP handles resource allocation; ACPIPHP only scans non-native bridges for NVMe */
		/*
		 * If native hotplug is used, it will take care of hotplug
		 * slot management and resource allocation for hotplug
		 * bridges. However, ACPI hotplug may still be used for
		 * non-hotplug bridges to bring in additional devices such
		 * as a Thunderbolt host controller.
		 */
		for_each_pci_bridge(dev, bus) {	/* NVMe: scan bridges in this slot that may carry NVMe downstream */
			if (PCI_SLOT(dev->devfn) == slot->device)	/* NVMe: only remove stale devices matching this slot */
				acpiphp_native_scan_bridge(dev);	/* NVMe: scan non-native bridges under this native-hotplug bridge for NVMe */
		}
	} else {	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
		LIST_HEAD(add_list);	/* NVMe: list of PCI buses needing resource allocation for NVMe */
		int max, pass;	/* NVMe: max bus number and two-pass scan control for NVMe enumeration */

		acpiphp_rescan_slot(slot);	/* NVMe: first, rescan ACPI and PCI slot for newly inserted NVMe */
		max = acpiphp_max_busnr(bus);	/* NVMe: compute current highest bus number before extending for NVMe */
		for (pass = 0; pass < 2; pass++) {	/* NVMe: two-pass scan: discover bridges, then allocate resources for NVMe */
			for_each_pci_bridge(dev, bus) {	/* NVMe: scan bridges in this slot that may carry NVMe downstream */
				if (PCI_SLOT(dev->devfn) != slot->device)	/* NVMe: skip bridges belonging to other slots, not this NVMe slot */
					continue;	/* NVMe: proceed to next item in NVMe hotplug iteration */

				max = pci_scan_bridge(bus, dev, max, pass);	/* NVMe: scan bridge; pass 0 discovers, pass 1 assigns resources for NVMe */
				if (pass && dev->subordinate) {	/* NVMe: on second pass, size/assign resources for NVMe downstream bus */
					check_hotplug_bridge(slot, dev);	/* NVMe: mark newly found bridge as hotplug-capable for future NVMe bays */
					pcibios_resource_survey_bus(dev->subordinate);	/* NVMe: survey host resources for this NVMe downstream bus */
					__pci_bus_size_bridges(dev->subordinate,	/* NVMe: compute bridge window requirements for NVMe BARs */
							       &add_list);	/* NVMe: collect buses needing resource assignment for NVMe */
				}
			}
		}
		__pci_bus_assign_resources(bus, &add_list, NULL);	/* NVMe: assign BARs and bus numbers so NVMe can map registers/DMA */
	}

	acpiphp_sanitize_bus(bus);	/* NVMe: remove NVMe functions that did not get required resources */
	pcie_bus_configure_settings(bus);	/* NVMe: configure MPS/MRRS for PCIe links carrying NVMe traffic */
	acpiphp_set_acpi_region(slot);	/* NVMe: run _REG so AML can access config space of the NVMe slot */

	list_for_each_entry(dev, &bus->devices, bus_list) {	/* NVMe: iterate devices just discovered on this bus (possible NVMe) */
		/* Assume that newly added devices are powered on already. */
		if (!pci_dev_is_added(dev))	/* NVMe: only update state for devices not yet added (fresh NVMe) */
			dev->current_state = PCI_D0;	/* NVMe: record that NVMe device is in D0 before driver binding */
	}

	pci_bus_add_devices(bus);	/* NVMe: bind drivers (including nvme-pci) to newly enumerated devices */

	slot->flags |= SLOT_ENABLED;	/* NVMe: mark slot enabled after successful NVMe enumeration */
	list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
		dev = pci_get_slot(bus, PCI_DEVFN(slot->device,	/* NVMe: lookup PCI device for this function in the NVMe slot */
						  func->function));	/* NVMe: complete device lookup using function number of NVMe slot */
		if (!dev) {	/* NVMe: expected function did not appear; NVMe device may be incomplete */
			/* Do not set SLOT_ENABLED flag if some funcs
			   are not added. */	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
			slot->flags &= ~SLOT_ENABLED;	/* NVMe: mark slot disabled so it can be re-enabled for next NVMe */
			continue;	/* NVMe: proceed to next item in NVMe hotplug iteration */
		}
		pci_dev_put(dev);	/* NVMe: drop reference obtained while checking NVMe function presence */
	}
}

/**
 * disable_slot - disable a slot
 * @slot: ACPI PHP slot
 */
static void disable_slot(struct acpiphp_slot *slot)	/* NVMe: power off/eject a slot and remove its NVMe device(s) */
{
	struct pci_bus *bus = slot->bus;	/* NVMe: bus serving this NVMe slot */
	struct pci_dev *dev, *prev;	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	/*
	 * enable_slot() enumerates all functions in this device via
	 * pci_scan_slot(), whether they have associated ACPI hotplug
	 * methods (_EJ0, etc.) or not.  Therefore, we remove all functions
	 * here.
	 */
	list_for_each_entry_safe_reverse(dev, prev, &bus->devices, bus_list)	/* NVMe: remove devices in reverse order so downstream NVMe goes first */
		if (PCI_SLOT(dev->devfn) == slot->device)	/* NVMe: only remove stale devices matching this slot */
			pci_stop_and_remove_bus_device(dev);	/* NVMe: remove broken NVMe device so others can be assigned resources */

	list_for_each_entry(func, &slot->funcs, sibling)	/* NVMe: trim ACPI namespace objects associated with NVMe functions */
		acpi_bus_trim(func_to_acpi_device(func));	/* NVMe: remove ACPI companion nodes after NVMe device removal */

	slot->flags &= ~SLOT_ENABLED;	/* NVMe: mark slot disabled so it can be re-enabled for next NVMe */
}

static bool slot_no_hotplug(struct acpiphp_slot *slot)	/* NVMe: check whether any function in the slot ignores hotplug events */
{
	struct pci_bus *bus = slot->bus;	/* NVMe: bus serving this NVMe slot */
	struct pci_dev *dev;	/* NVMe: PCI device iterator for scanning NVMe functions/bridges */

	list_for_each_entry(dev, &bus->devices, bus_list) {	/* NVMe: iterate devices just discovered on this bus (possible NVMe) */
		if (PCI_SLOT(dev->devfn) == slot->device && dev->ignore_hotplug)	/* NVMe: device in this slot told PCI core to ignore hotplug (e.g., NVMe passthrough) */
			return true;	/* NVMe: skip hotplug processing for this NVMe slot */
	}
	return false;	/* NVMe: slot is eligible for ACPI hotplug events */
}

/**
 * get_slot_status - get ACPI slot status
 * @slot: ACPI PHP slot
 *
 * If a slot has _STA for each function and if any one of them
 * returned non-zero status, return it.
 *
 * If a slot doesn't have _STA and if any one of its functions'
 * configuration space is configured, return 0x0f as a _STA.
 *
 * Otherwise return 0.
 */
static unsigned int get_slot_status(struct acpiphp_slot *slot)	/* NVMe: determine presence/enabled status of NVMe slot via _STA or config */
{
	unsigned long long sta = 0;	/* NVMe: ACPI _STA status bits for NVMe slot presence/enabled/functioning */
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */
	u32 dvid;	/* NVMe: device/vendor ID read from config space to detect NVMe presence */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* NVMe: verify each expected function in the slot was added */
		if (func->flags & FUNC_HAS_STA) {	/* NVMe: use ACPI _STA if available to detect NVMe bay status */
			acpi_status status;	/* NVMe: status of ACPI namespace walk for NVMe slots */

			status = acpi_evaluate_integer(func_to_handle(func),	/* NVMe: evaluate _STA method for this NVMe function */
						       "_STA", NULL, &sta);	/* NVMe: read status returned by ACPI for NVMe slot */
			if (ACPI_SUCCESS(status) && sta)	/* NVMe: nonzero _STA means NVMe slot is present/enabled */
				break;	/* NVMe: stop checking this device after first unassigned resource */
		} else {	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
			if (pci_bus_read_dev_vendor_id(slot->bus,	/* NVMe: also check function 0 when per-function status is absent */
					PCI_DEVFN(slot->device, func->function),	/* NVMe: form PCI devfn for this NVMe function */
					&dvid, 0)) {	/* NVMe: config read success implies an NVMe device is present */
				sta = ACPI_STA_ALL;	/* NVMe: synthesize full ACPI status when config space responds */
				break;	/* NVMe: stop checking this device after first unassigned resource */
			}
		}
	}

	if (!sta) {	/* NVMe: conditional check on ACPI/NVMe hotplug state */
		/*
		 * Check for the slot itself since it may be that the
		 * ACPI slot is a device below PCIe upstream port so in
		 * that case it may not even be reachable yet.
		 */
		if (pci_bus_read_dev_vendor_id(slot->bus,	/* NVMe: also check function 0 when per-function status is absent */
				PCI_DEVFN(slot->device, 0), &dvid, 0)) {	/* NVMe: function 0 responding indicates an NVMe device in the slot */
			sta = ACPI_STA_ALL;	/* NVMe: synthesize full ACPI status when config space responds */
		}
	}

	return (unsigned int)sta;	/* NVMe: return ACPI status used to decide NVMe slot enable/disable */
}

static inline bool device_status_valid(unsigned int sta)	/* NVMe: interpret _STA to decide if NVMe device is enabled/functioning */
{
	/*
	 * ACPI spec says that _STA may return bit 0 clear with bit 3 set
	 * if the device is valid but does not require a device driver to be
	 * loaded (Section 6.3.7 of ACPI 5.0A).
	 */
	unsigned int mask = ACPI_STA_DEVICE_ENABLED | ACPI_STA_DEVICE_FUNCTIONING;	/* NVMe: require both enabled and functioning bits for a usable NVMe device */
	return (sta & mask) == mask;	/* NVMe: true when NVMe slot reports device enabled and functioning */
}

/**
 * trim_stale_devices - remove PCI devices that are not responding.
 * @dev: PCI device to start walking the hierarchy from.
 */
static void trim_stale_devices(struct pci_dev *dev)	/* NVMe: remove PCI devices that no longer respond (surprise NVMe removal) */
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);	/* NVMe: ACPI companion of this NVMe device, if any */
	struct pci_bus *bus = dev->subordinate;	/* NVMe: subordinate bus if this NVMe device is a bridge */
	bool alive = dev->ignore_hotplug;	/* NVMe: keep device if it opts out of hotplug removal (e.g., VFIO NVMe) */

	if (adev) {	/* NVMe: conditional check on ACPI/NVMe hotplug state */
		acpi_status status;	/* NVMe: status of ACPI namespace walk for NVMe slots */
		unsigned long long sta;	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */

		status = acpi_evaluate_integer(adev->handle, "_STA", NULL, &sta);	/* NVMe: ask ACPI whether NVMe device is still present/enabled */
		alive = alive || (ACPI_SUCCESS(status) && device_status_valid(sta));	/* NVMe: device is alive if ACPI says it is functioning */
	}
	if (!alive)	/* NVMe: conditional check on ACPI/NVMe hotplug state */
		alive = pci_device_is_present(dev);	/* NVMe: or if PCI config space still responds for this NVMe device */

	if (!alive) {	/* NVMe: conditional check on ACPI/NVMe hotplug state */
		pci_dev_set_disconnected(dev, NULL);	/* NVMe: mark NVMe device disconnected so drivers stop accessing it */
		if (pci_has_subordinate(dev))	/* NVMe: if device is a bridge, disconnect all downstream NVMe devices */
			pci_walk_bus(dev->subordinate, pci_dev_set_disconnected,	/* NVMe: walk subordinate bus marking every NVMe function disconnected */
				     NULL);	/* NVMe: no private data needed for disconnect walk */

		pci_stop_and_remove_bus_device(dev);	/* NVMe: remove broken NVMe device so others can be assigned resources */
		if (adev)	/* NVMe: conditional check on ACPI/NVMe hotplug state */
			acpi_bus_trim(adev);	/* NVMe: remove ACPI namespace node for the gone NVMe device */
	} else if (bus) {	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
		struct pci_dev *child, *tmp;	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */

		/* The device is a bridge. so check the bus below it. */
		pm_runtime_get_sync(&dev->dev);	/* NVMe: keep bridge awake while checking its children for stale NVMe */
		list_for_each_entry_safe_reverse(child, tmp, &bus->devices, bus_list)	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
			trim_stale_devices(child);	/* NVMe: recursively check/remove stale NVMe devices below bridge */

		pm_runtime_put(&dev->dev);	/* NVMe: allow bridge to sleep after NVMe stale scan */
	}
}

/**
 * acpiphp_check_bridge - re-enumerate devices
 * @bridge: where to begin re-enumeration
 *
 * Iterate over all slots under this bridge and make sure that if a
 * card is present they are enabled, and if not they are disabled.
 */
static void acpiphp_check_bridge(struct acpiphp_bridge *bridge)	/* NVMe: re-enumerate slots under bridge to sync NVMe device state */
{
	struct acpiphp_slot *slot;	/* NVMe: slot iterator for cleaning up NVMe slots */

	/* Bail out if the bridge is going away. */
	if (bridge->is_going_away)	/* NVMe: bridge removal in progress; skip NVMe rescan */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	if (bridge->pci_dev)	/* NVMe: runtime PM the upstream bridge while scanning for NVMe */
		pm_runtime_get_sync(&bridge->pci_dev->dev);	/* NVMe: keep upstream bridge powered during NVMe slot check */

	list_for_each_entry(slot, &bridge->slots, node) {	/* NVMe: check every ACPIPHP slot under this bridge for NVMe changes */
		struct pci_bus *bus = slot->bus;	/* NVMe: bus serving this NVMe slot */
		struct pci_dev *dev, *tmp;	/* NVMe: device iterators for removing stale NVMe functions */

		if (slot_no_hotplug(slot)) {	/* NVMe: skip slots whose devices ignore hotplug (e.g., passthrough NVMe) */
			; /* do nothing */	/* NVMe: no hotplug action for this NVMe slot */
		} else if (device_status_valid(get_slot_status(slot))) {	/* NVMe: slot reports present/enabled; rescan for NVMe devices */
			/* remove stale devices if any */
			list_for_each_entry_safe_reverse(dev, tmp,	/* NVMe: remove stale NVMe devices before enabling slot */
							 &bus->devices, bus_list)	/* NVMe: scan bus device list for this NVMe slot */
				if (PCI_SLOT(dev->devfn) == slot->device)	/* NVMe: only remove stale devices matching this slot */
					trim_stale_devices(dev);	/* NVMe: remove any NVMe device that no longer responds */

			/* configure all functions */
			enable_slot(slot, true);	/* NVMe: power on and enumerate NVMe devices in this slot */
		} else {	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
			disable_slot(slot);	/* NVMe: remove NVMe device from PCI core before ejection */
		}
	}

	if (bridge->pci_dev)	/* NVMe: runtime PM the upstream bridge while scanning for NVMe */
		pm_runtime_put(&bridge->pci_dev->dev);	/* NVMe: release runtime PM reference after NVMe slot check */
}

/*
 * Remove devices for which we could not assign resources, call
 * arch specific code to fix-up the bus
 */
static void acpiphp_sanitize_bus(struct pci_bus *bus)	/* NVMe: remove functions that failed resource assignment (BAR allocation) */
{
	struct pci_dev *dev, *tmp;	/* NVMe: device iterators for removing stale NVMe functions */
	int i;	/* NVMe: resource index iterator for each NVMe device */
	unsigned long type_mask = IORESOURCE_IO | IORESOURCE_MEM;	/* NVMe: match MMIO/IO BARs that NVMe controllers need */

	list_for_each_entry_safe_reverse(dev, tmp, &bus->devices, bus_list) {	/* NVMe: inspect every device on bus; remove ones with broken resources */
		for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) {	/* NVMe: check each BAR/resource entry of the NVMe device */
			struct resource *res = &dev->resource[i];	/* NVMe: resource descriptor for this NVMe BAR */
			if ((res->flags & type_mask) && !res->start &&	/* NVMe: MMIO/IO resource was requested but not assigned */
					res->end) {	/* NVMe: non-zero end means the NVMe device wanted this BAR */
				/* Could not assign a required resources
				 * for this device, remove it */
				pci_stop_and_remove_bus_device(dev);	/* NVMe: remove broken NVMe device so others can be assigned resources */
				break;	/* NVMe: stop checking this device after first unassigned resource */
			}
		}
	}
}

/*
 * ACPI event handlers
 */

void acpiphp_check_host_bridge(struct acpi_device *adev)	/* NVMe: entry from ACPI host bridge notification to rescan NVMe topology */
{
	struct acpiphp_bridge *bridge = NULL;	/* NVMe: host bridge context used to rescan NVMe slots */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
	if (adev->hp) {	/* NVMe: check if this host bridge has ACPIPHP root context */
		bridge = to_acpiphp_root_context(adev->hp)->root_bridge;	/* NVMe: obtain bridge from root context to scan NVMe slots */
		if (bridge)	/* NVMe: bus check targeted at an NVMe bridge */
			get_bridge(bridge);	/* NVMe: pin bridge for duration of NVMe hotplug event handling */
	}
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
	if (bridge) {	/* NVMe: device check for an NVMe bridge */
		pci_lock_rescan_remove();	/* NVMe: serialize PCI device removal with nvme-pci */

		acpiphp_check_bridge(bridge);	/* NVMe: rescan NVMe slots under the bridge */

		pci_unlock_rescan_remove();	/* NVMe: allow PCI/NVMe probe/remove again */
		put_bridge(bridge);	/* NVMe: release bridge reference to free NVMe bridge state */
	}
}

static int acpiphp_disable_and_eject_slot(struct acpiphp_slot *slot);	/* NVMe: forward declaration for eject helper used on NVMe removal */

static void hotplug_event(u32 type, struct acpiphp_context *context)	/* NVMe: dispatch ACPI notify event that may add/remove an NVMe device */
{
	acpi_handle handle = context->hp.self->handle;	/* NVMe: ACPI handle used for debug messages during NVMe hotplug */
	struct acpiphp_func *func = &context->func;	/* NVMe: function descriptor for the NVMe slot being notified */
	struct acpiphp_slot *slot = func->slot;	/* NVMe: slot that may contain the NVMe device generating the event */
	struct acpiphp_bridge *bridge;	/* NVMe: new bridge object representing the bus above NVMe slots */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
	bridge = context->bridge;	/* NVMe: get bridge from context (set for bridge contexts) */
	if (bridge)	/* NVMe: bus check targeted at an NVMe bridge */
		get_bridge(bridge);	/* NVMe: pin bridge for duration of NVMe hotplug event handling */

	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */

	pci_lock_rescan_remove();	/* NVMe: serialize PCI device removal with nvme-pci */

	switch (type) {	/* NVMe: branch on ACPI notify type for this NVMe slot/bridge */
	case ACPI_NOTIFY_BUS_CHECK:	/* NVMe: bus re-enumeration requested for NVMe topology */
		/* bus re-enumerate */
		acpi_handle_debug(handle, "Bus check in %s()\n", __func__);	/* NVMe: debug log bus-check event for NVMe slot/bridge */
		if (bridge)	/* NVMe: bus check targeted at an NVMe bridge */
			acpiphp_check_bridge(bridge);	/* NVMe: rescan NVMe slots under the bridge */
		else if (!(slot->flags & SLOT_IS_GOING_AWAY))	/* NVMe: bus check for a single slot that is not being removed */
			enable_slot(slot, false);	/* NVMe: rescan and enable NVMe devices in this slot */

		break;	/* NVMe: stop checking this device after first unassigned resource */

	case ACPI_NOTIFY_DEVICE_CHECK:	/* NVMe: device check event, often triggered by NVMe insertion */
		/* device check */
		acpi_handle_debug(handle, "Device check in %s()\n", __func__);	/* NVMe: debug log device-check event for NVMe slot */
		if (bridge) {	/* NVMe: device check for an NVMe bridge */
			acpiphp_check_bridge(bridge);	/* NVMe: rescan NVMe slots under the bridge */
		} else if (!(slot->flags & SLOT_IS_GOING_AWAY)) {	/* NVMe: device check for a single NVMe slot not going away */
			/*
			 * Check if anything has changed in the slot and rescan
			 * from the parent if that's the case.
			 */
			if (acpiphp_rescan_slot(slot))	/* NVMe: rescan slot; nonzero if new NVMe function was discovered */
				acpiphp_check_bridge(func->parent);	/* NVMe: new device appeared; recheck parent bridge for more NVMe */
		}
		break;	/* NVMe: stop checking this device after first unassigned resource */

	case ACPI_NOTIFY_EJECT_REQUEST:	/* NVMe: user/firmware requests ejection of NVMe device */
		/* request device eject */
		acpi_handle_debug(handle, "Eject request in %s()\n", __func__);	/* NVMe: debug log eject request for NVMe bay */
		acpiphp_disable_and_eject_slot(slot);	/* NVMe: power off and eject the NVMe slot */
		break;	/* NVMe: stop checking this device after first unassigned resource */
	}

	pci_unlock_rescan_remove();	/* NVMe: allow PCI/NVMe probe/remove again */
	if (bridge)	/* NVMe: bus check targeted at an NVMe bridge */
		put_bridge(bridge);	/* NVMe: release bridge reference to free NVMe bridge state */
}

static int acpiphp_hotplug_notify(struct acpi_device *adev, u32 type)	/* NVMe: ACPI notify handler registered for each ACPIPHP NVMe device */
{
	struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */

	context = acpiphp_grab_context(adev);	/* NVMe: lookup and pin context for ACPI companion of NVMe device */
	if (!context)	/* NVMe: parent not tracked by ACPIPHP; ignore this NVMe bridge */
		return -ENODATA;	/* NVMe: tell ACPI core there is no hotplug context for this NVMe device */

	hotplug_event(type, context);	/* NVMe: dispatch the actual add/check/eject event for NVMe */
	acpiphp_let_context_go(context);	/* NVMe: release bridge reference after handling NVMe notify */
	return 0;	/* NVMe: ACPI notify handled successfully for NVMe slot */
}

/**
 * acpiphp_enumerate_slots - Enumerate PCI slots for a given bus.
 * @bus: PCI bus to enumerate the slots for.
 *
 * A "slot" is an object associated with a PCI device number.  All functions
 * (PCI devices) with the same bus and device number belong to the same slot.
 */
void acpiphp_enumerate_slots(struct pci_bus *bus)	/* NVMe: called during PCI bus enumeration to discover NVMe hotplug slots */
{
	struct acpiphp_bridge *bridge;	/* NVMe: new bridge object representing the bus above NVMe slots */
	struct acpi_device *adev;	/* NVMe: ACPI companion of host bridge */
	acpi_handle handle;	/* NVMe: ACPI namespace handle for scanning NVMe slot objects */
	acpi_status status;	/* NVMe: status of ACPI namespace walk for NVMe slots */

	if (acpiphp_disabled)	/* NVMe: ACPIPHP disabled; no NVMe hotplug slot tracking */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	adev = ACPI_COMPANION(bus->bridge);	/* NVMe: get ACPI companion of the PCI host/bridge for NVMe */
	if (!adev)	/* NVMe: no ACPI companion; cannot use ACPIPHP for NVMe hotplug */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	handle = adev->handle;	/* NVMe: ACPI handle used to walk child objects for NVMe slots */
	bridge = kzalloc_obj(struct acpiphp_bridge);	/* NVMe: allocate bridge object to hold NVMe slot list */
	if (!bridge)	/* NVMe: bridge allocation failed; cannot track NVMe slots on this bus */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	INIT_LIST_HEAD(&bridge->slots);	/* NVMe: initialize list of NVMe slots under this bridge */
	kref_init(&bridge->ref);	/* NVMe: initialize bridge refcount for NVMe slot references */
	bridge->pci_dev = pci_dev_get(bus->self);	/* NVMe: reference upstream bridge device above NVMe slots */
	bridge->pci_bus = bus;	/* NVMe: record subordinate bus that carries NVMe devices */

	/*
	 * Grab a ref to the subordinate PCI bus in case the bus is
	 * removed via PCI core logical hotplug. The ref pins the bus
	 * (which we access during module unload).
	 */
	get_device(&bus->dev);	/* NVMe: pin bus device so it outlives logical NVMe hotplug operations */

	acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
	if (pci_is_root_bus(bridge->pci_bus)) {	/* NVMe: root bridge needs root context teardown */
		struct acpiphp_root_context *root_context;	/* NVMe: root context of host bridge for NVMe slots */

		root_context = kzalloc_obj(*root_context);	/* NVMe: allocate root context for host bridge above NVMe topology */
		if (!root_context)	/* NVMe: root context allocation failed; cannot track NVMe host bridge */
			goto err;	/* NVMe: roll back bridge allocation on root context failure */

		root_context->root_bridge = bridge;	/* NVMe: link root context to this host bridge for NVMe rescan */
		acpi_set_hp_context(adev, &root_context->hp);	/* NVMe: attach root hotplug context to host bridge ACPI device */
	} else {	/* NVMe: ACPI PHP glue step related to NVMe PCIe hotplug/enumeration */
		struct acpiphp_context *context;	/* NVMe: non-root bridge context referencing parent NVMe bridge */

		/*
		 * This bridge should have been registered as a hotplug function
		 * under its parent, so the context should be there, unless the
		 * parent is going to be handled by pciehp, in which case this
		 * bridge is not interesting to us either.
		 */
		context = acpiphp_get_context(adev);	/* NVMe: get parent context for this bridge (was registered as a function) */
		if (!context)	/* NVMe: parent not tracked by ACPIPHP; ignore this NVMe bridge */
			goto err;	/* NVMe: roll back bridge allocation on root context failure */

		bridge->context = context;	/* NVMe: bridge stores context used for parent reference release */
		context->bridge = bridge;	/* NVMe: context now points to this bridge for NVMe event dispatch */
		/* Get a reference to the parent bridge. */
		get_bridge(context->func.parent);	/* NVMe: pin parent bridge for this NVMe sub-bridge */
	}
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */

	/* Must be added to the list prior to calling acpiphp_add_context(). */
	mutex_lock(&bridge_mutex);	/* NVMe: search global bridge list under lock for NVMe bus */
	list_add(&bridge->list, &bridge_list);	/* NVMe: add bridge to global list so NVMe scans can locate slots */
	mutex_unlock(&bridge_mutex);	/* NVMe: release lock before dropping NVMe bridge */

	/* register all slot objects under this bridge */
	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,	/* NVMe: traverse ACPI devices one level below bridge for NVMe slots */
				     acpiphp_add_context, NULL, bridge, NULL);	/* NVMe: callback registers each ACPI device as an NVMe slot/function */
	if (ACPI_FAILURE(status)) {	/* NVMe: namespace walk failed; NVMe slots may not be registered */
		acpi_handle_err(handle, "failed to register slots\n");	/* NVMe: log failure to register NVMe slots under bridge */
		cleanup_bridge(bridge);	/* NVMe: undo partial slot/bridge registration for NVMe */
		put_bridge(bridge);	/* NVMe: release bridge reference to free NVMe bridge state */
	}
	return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

 err:	/* NVMe: error path for bridge/context allocation failures */
	acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
	put_device(&bus->dev);	/* NVMe: release bus device reference taken for NVMe bridge */
	pci_dev_put(bridge->pci_dev);	/* NVMe: release upstream bridge device reference for NVMe bridge */
	kfree(bridge);	/* NVMe: free bridge allocation after failed NVMe slot enumeration */
}

static void acpiphp_drop_bridge(struct acpiphp_bridge *bridge)	/* NVMe: tear down a bridge and its NVMe slots on bus removal */
{
	if (pci_is_root_bus(bridge->pci_bus)) {	/* NVMe: root bridge needs root context teardown */
		struct acpiphp_root_context *root_context;	/* NVMe: root context of host bridge for NVMe slots */
		struct acpi_device *adev;	/* NVMe: ACPI companion of host bridge */

		acpi_lock_hp_context();	/* NVMe: protect context list while adding a new NVMe slot context */
		adev = ACPI_COMPANION(bridge->pci_bus->bridge);	/* NVMe: fetch ACPI device of host bridge carrying NVMe slots */
		root_context = to_acpiphp_root_context(adev->hp);	/* NVMe: get root context from ACPI hotplug pointer */
		adev->hp = NULL;	/* NVMe: detach hotplug context from host bridge ACPI device */
		acpi_unlock_hp_context();	/* NVMe: context is initialized; release lock before slot lookup */
		kfree(root_context);	/* NVMe: free root context after host bridge NVMe slots are gone */
	}
	cleanup_bridge(bridge);	/* NVMe: undo partial slot/bridge registration for NVMe */
	put_bridge(bridge);	/* NVMe: release bridge reference to free NVMe bridge state */
}

/**
 * acpiphp_remove_slots - Remove slot objects associated with a given bus.
 * @bus: PCI bus to remove the slot objects for.
 */
void acpiphp_remove_slots(struct pci_bus *bus)	/* NVMe: remove slot objects when a PCI bus (with NVMe slots) is removed */
{
	struct acpiphp_bridge *bridge;	/* NVMe: new bridge object representing the bus above NVMe slots */

	if (acpiphp_disabled)	/* NVMe: ACPIPHP disabled; no NVMe hotplug slot tracking */
		return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */

	mutex_lock(&bridge_mutex);	/* NVMe: search global bridge list under lock for NVMe bus */
	list_for_each_entry(bridge, &bridge_list, list)	/* NVMe: iterate registered bridges to find one matching this bus */
		if (bridge->pci_bus == bus) {	/* NVMe: found bridge whose subordinate bus matches the removed NVMe bus */
			mutex_unlock(&bridge_mutex);	/* NVMe: release lock before dropping NVMe bridge */
			acpiphp_drop_bridge(bridge);	/* NVMe: unregister slots and free bridge for removed NVMe bus */
			return;	/* NVMe: skip enumeration when ACPIPHP module is disabled */
		}

	mutex_unlock(&bridge_mutex);	/* NVMe: release lock before dropping NVMe bridge */
}

/**
 * acpiphp_enable_slot - power on slot
 * @slot: ACPI PHP slot
 */
int acpiphp_enable_slot(struct acpiphp_slot *slot)	/* NVMe: user-space enable request for an NVMe hotplug slot */
{
	pci_lock_rescan_remove();	/* NVMe: serialize PCI device removal with nvme-pci */

	if (slot->flags & SLOT_IS_GOING_AWAY) {	/* NVMe: refuse enable if slot is already being removed */
		pci_unlock_rescan_remove();	/* NVMe: allow PCI/NVMe probe/remove again */
		return -ENODEV;	/* NVMe: eject not possible while slot is going away */
	}

	/* configure all functions */
	if (!(slot->flags & SLOT_ENABLED))	/* NVMe: only enable if not already enabled */
		enable_slot(slot, false);	/* NVMe: rescan and enable NVMe devices in this slot */

	pci_unlock_rescan_remove();	/* NVMe: allow PCI/NVMe probe/remove again */
	return 0;	/* NVMe: ACPI notify handled successfully for NVMe slot */
}

/**
 * acpiphp_disable_and_eject_slot - power off and eject slot
 * @slot: ACPI PHP slot
 */
static int acpiphp_disable_and_eject_slot(struct acpiphp_slot *slot)	/* NVMe: disable NVMe slot and run ACPI _EJ0 to power it off */
{
	struct acpiphp_func *func;	/* NVMe: function iterator to verify all NVMe functions were added */

	if (slot->flags & SLOT_IS_GOING_AWAY)	/* NVMe: slot already being removed; reject duplicate eject */
		return -ENODEV;	/* NVMe: eject not possible while slot is going away */

	/* unconfigure all functions */
	disable_slot(slot);	/* NVMe: remove NVMe device from PCI core before ejection */

	list_for_each_entry(func, &slot->funcs, sibling)	/* NVMe: trim ACPI namespace objects associated with NVMe functions */
		if (func->flags & FUNC_HAS_EJ0) {	/* NVMe: only run _EJ0 on functions that support ejection */
			acpi_handle handle = func_to_handle(func);	/* NVMe: ACPI handle for evaluating _EJ0 on NVMe function */

			if (ACPI_FAILURE(acpi_evaluate_ej0(handle)))	/* NVMe: _EJ0 may fail if NVMe device is still in use */
				acpi_handle_err(handle, "_EJ0 failed\n");	/* NVMe: log ejection failure for NVMe slot */

			break;	/* NVMe: stop checking this device after first unassigned resource */
		}

	return 0;	/* NVMe: ACPI notify handled successfully for NVMe slot */
}

int acpiphp_disable_slot(struct acpiphp_slot *slot)	/* NVMe: user-space disable/eject wrapper for NVMe hotplug slot */
{
	int ret;	/* NVMe: return value from disable/eject operation */

	/*
	 * Acquire acpi_scan_lock to ensure that the execution of _EJ0 in
	 * acpiphp_disable_and_eject_slot() will be synchronized properly.
	 */
	acpi_scan_lock_acquire();	/* NVMe: serialize _EJ0 evaluation with ACPI scan for NVMe */
	pci_lock_rescan_remove();	/* NVMe: serialize PCI device removal with nvme-pci */
	ret = acpiphp_disable_and_eject_slot(slot);	/* NVMe: perform disable and optional _EJ0 for NVMe slot */
	pci_unlock_rescan_remove();	/* NVMe: allow PCI/NVMe probe/remove again */
	acpi_scan_lock_release();	/* NVMe: release ACPI scan lock after NVMe eject */
	return ret;	/* NVMe: return result of NVMe slot disable/eject */
}

/*
 * slot enabled:  1
 * slot disabled: 0
 */
u8 acpiphp_get_power_status(struct acpiphp_slot *slot)	/* NVMe: report whether slot is enabled (powered) for user-space tools */
{
	return (slot->flags & SLOT_ENABLED);	/* NVMe: SLOT_ENABLED reflects whether NVMe slot is powered/enumerated */
}

/*
 * latch   open:  1
 * latch closed:  0
 */
u8 acpiphp_get_latch_status(struct acpiphp_slot *slot)	/* NVMe: report physical latch/attention button status for NVMe bay */
{
	return !(get_slot_status(slot) & ACPI_STA_DEVICE_UI);	/* NVMe: UI bit from _STA indicates whether NVMe bay latch is open */
}

/*
 * adapter presence : 1
 *          absence : 0
 */
u8 acpiphp_get_adapter_status(struct acpiphp_slot *slot)	/* NVMe: report whether an NVMe adapter is present in the slot */
{
	return !!get_slot_status(slot);	/* NVMe: nonzero status means an NVMe device is present in the bay */
}
