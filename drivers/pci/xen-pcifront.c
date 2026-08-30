// SPDX-License-Identifier: GPL-2.0
/*
 * Xen PCI Frontend
 *
 * Author: Ryan Wilson <hap9@epoch.ncsc.mil>
 */
#include <linux/module.h>		/* PCI/NVMe: module loading; NVMe host/pci.c is also a module */
#include <linux/init.h>			/* PCI/NVMe: __init/__exit markers, like nvme_pci_init */
#include <linux/mm.h>			/* PCI/NVMe: memory mapping/DMA page helpers used by NVMe BAR mapping */
#include <xen/xenbus.h>			/* PCI/NVMe: XenBus state machine for PCI passthrough; NVMe sees this as a normal PCI bus */
#include <xen/events.h>			/* PCI/NVMe: Xen event-channel used for MSI-X/MSI/AER notifications to NVMe queues */
#include <xen/grant_table.h>		/* PCI/NVMe: grants shared ring pages; backend forwards NVMe config/MSI/AER ops */
#include <xen/page.h>			/* PCI/NVMe: Xen page helpers; DMA mappings for NVMe command buffers go through Xen swiotlb */
#include <linux/spinlock.h>		/* PCI/NVMe: protects shared ring ops; NVMe probe config reads serialize here */
#include <linux/pci.h>			/* PCI/NVMe: core PCI API; NVMe driver binds via this subsystem */
#include <linux/msi.h>			/* PCI/NVMe: MSI/MSI-X descriptor helpers; NVMe queues request per-queue MSI-X vectors */
#include <xen/interface/io/pciif.h>	/* PCI/NVMe: Xen PV PCI protocol op codes (conf_read, enable_msix, AER) */
#include <asm/xen/pci.h>		/* PCI/NVMe: xen_pci_frontend_ops hook used to forward MSI/MSI-X setup */
#include <linux/interrupt.h>		/* PCI/NVMe: IRQ_HANDLED etc.; NVMe ISR registered via this layer under Xen */
#include <linux/atomic.h>		/* PCI/NVMe: atomic bit ops for shared ring flags and AER scheduling */
#include <linux/workqueue.h>		/* PCI/NVMe: AER recovery queued as work; NVMe err_handler resumes from here */
#include <linux/bitops.h>		/* PCI/NVMe: test/set/clear_bit on shared info flags */
#include <linux/time.h>			/* PCI/NVMe: timing helpers for config-op timeout */
#include <linux/ktime.h>		/* PCI/NVMe: high-res timeout while waiting for pciback response */
#include <xen/platform_pci.h>		/* PCI/NVMe: Xen platform device helpers */

#include <asm/xen/swiotlb-xen.h>	/* PCI/NVMe: Xen software I/O TLB; NVMe DMA mappings in a PV guest use this */

#define INVALID_EVTCHN    (-1)		/* PCI/NVMe: invalid event channel; MSI-X/AER paths depend on a valid one */

struct pci_bus_entry {
	struct list_head list;		/* PCI/NVMe: links root buses owned by this frontend */
	struct pci_bus *bus;		/* PCI/NVMe: root bus under which NVMe controllers appear after passthrough */
};

#define _PDEVB_op_active		(0)		/* PCI/NVMe: bit index for an in-flight AER service op */
#define PDEVB_op_active			(1 << (_PDEVB_op_active))	/* PCI/NVMe: AER work already scheduled? */

struct pcifront_device {
	struct xenbus_device *xdev;	/* PCI/NVMe: XenBus frontend device; represents the PCI passthrough channel */
	struct list_head root_buses;	/* PCI/NVMe: list of root buses, each may host NVMe controllers */

	int evtchn;			/* PCI/NVMe: Xen event channel; backend pokes this for config completions and AER */
	grant_ref_t gnt_ref;		/* PCI/NVMe: grant reference of shared ring; used for PCI ops */

	int irq;			/* PCI/NVMe: Linux IRQ bound to evtchn; AER handler runs on it */

	/* Lock this when doing any operations in sh_info */
	spinlock_t sh_info_lock;	/* PCI/NVMe: serializes do_pci_op; NVMe config reads/writes take this */
	struct xen_pci_sharedinfo *sh_info;	/* PCI/NVMe: shared page with backend holding op and MSI-X/AER state */
	struct work_struct op_work;	/* PCI/NVMe: AER recovery work; runs pcifront_do_aer and calls NVMe err_handler */
	unsigned long flags;		/* PCI/NVMe: PDEVB_op_active and backend flags */

};

struct pcifront_sd {
	struct pci_sysdata sd;		/* PCI/NVMe: per-bus sysdata attached to root bus; NVMe pdev->bus->sysdata points here */
	struct pcifront_device *pdev;	/* PCI/NVMe: back-pointer used to find frontend from any NVMe pci_dev */
};

static inline struct pcifront_device *
pcifront_get_pdev(struct pcifront_sd *sd)
{
	return sd->pdev;			/* PCI/NVMe: retrieve frontend context for this NVMe bus */
}

static inline void pcifront_init_sd(struct pcifront_sd *sd,
				    unsigned int domain, unsigned int bus,
				    struct pcifront_device *pdev)
{
	/* Because we do not expose that information via XenBus. */
	sd->sd.node = first_online_node;	/* PCI/NVMe: NUMA node for NVMe controller affinity */
	sd->sd.domain = domain;			/* PCI/NVMe: PCI domain where NVMe controller lives */
	sd->pdev = pdev;			/* PCI/NVMe: bind bus sysdata to frontend for later MSI/AER ops */
}

static DEFINE_SPINLOCK(pcifront_dev_lock);	/* PCI/NVMe: protects global pcifront_dev pointer (DMA/IOMMU context) */
static struct pcifront_device *pcifront_dev;	/* PCI/NVMe: singleton frontend; NVMe DMA via xen-swiotlb references this */

static int errno_to_pcibios_err(int errno)	/* PCI/NVMe: translate Xen PV errors to PCIBIOS codes NVMe core understands */
{
	switch (errno) {
	case XEN_PCI_ERR_success:
		return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: NVMe config access succeeded */

	case XEN_PCI_ERR_dev_not_found:
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: NVMe device/function absent on the virtual bus */

	case XEN_PCI_ERR_invalid_offset:
	case XEN_PCI_ERR_op_failed:
		return PCIBIOS_BAD_REGISTER_NUMBER;	/* PCI/NVMe: NVMe driver read an invalid config offset */

	case XEN_PCI_ERR_not_implemented:
		return PCIBIOS_FUNC_NOT_SUPPORTED;	/* PCI/NVMe: operation (e.g. MSI-X) not supported by backend */

	case XEN_PCI_ERR_access_denied:
		return PCIBIOS_SET_FAILED;	/* PCI/NVMe: backend denied NVMe config write */
	}
	return errno;
}

static inline void schedule_pcifront_aer_op(struct pcifront_device *pdev)	/* PCI/NVMe: queue AER recovery work that will invoke NVMe err_handler callbacks */
{
	if (test_bit(_XEN_PCIB_active, (unsigned long *)&pdev->sh_info->flags)	/* PCI/NVMe: backend posted an AER request for an NVMe device */
	    && !test_and_set_bit(_PDEVB_op_active, &pdev->flags)) {	/* PCI/NVMe: only one AER work item at a time */
		dev_dbg(&pdev->xdev->dev, "schedule aer frontend job\n");
		schedule_work(&pdev->op_work);	/* PCI/NVMe: run AER recovery in process context; safe for NVMe resume/slot_reset */
	}
}

static int do_pci_op(struct pcifront_device *pdev, struct xen_pci_op *op)	/* PCI/NVMe: synchronous PV hypercall to pciback; used for NVMe config/MSI/AER */
{
	int err = 0;
	struct xen_pci_op *active_op = &pdev->sh_info->op;	/* PCI/NVMe: shared ring slot where NVMe config/MSI command is staged */
	unsigned long irq_flags;
	evtchn_port_t port = pdev->evtchn;	/* PCI/NVMe: notify backend via this port */
	unsigned int irq = pdev->irq;
	s64 ns, ns_timeout;

	spin_lock_irqsave(&pdev->sh_info_lock, irq_flags);	/* PCI/NVMe: serialize with AER/MSI handlers; NVMe probe is single-threaded per op */

	memcpy(active_op, op, sizeof(struct xen_pci_op));	/* PCI/NVMe: copy NVMe config/MSI op into shared ring */

	/* Go */
	wmb();							/* PCI/NVMe: make op visible to backend before flag */
	set_bit(_XEN_PCIF_active, (unsigned long *)&pdev->sh_info->flags);	/* PCI/NVMe: tell backend an NVMe op is pending */
	notify_remote_via_evtchn(port);					/* PCI/NVMe: kick pciback to service NVMe request */

	/*
	 * We set a poll timeout of 3 seconds but give up on return after
	 * 2 seconds. It is better to time out too late rather than too early
	 * (in the latter case we end up continually re-executing poll() with a
	 * timeout in the past). 1s difference gives plenty of slack for error.
	 */
	ns_timeout = ktime_get_ns() + 2 * (s64)NSEC_PER_SEC;	/* PCI/NVMe: deadline for NVMe config/MSI response */

	xen_clear_irq_pending(irq);	/* PCI/NVMe: clear stale AER interrupt before polling */

	while (test_bit(_XEN_PCIF_active,
			(unsigned long *)&pdev->sh_info->flags)) {	/* PCI/NVMe: wait until pciback clears flag for this NVMe op */
		xen_poll_irq_timeout(irq, jiffies + 3*HZ);	/* PCI/NVMe: block briefly; MSI-X/AER events share this irq */
		xen_clear_irq_pending(irq);			/* PCI/NVMe: ack any event that arrived while polling */
		ns = ktime_get_ns();
		if (ns > ns_timeout) {				/* PCI/NVMe: NVMe op timed out; treat as device not found */
			dev_err(&pdev->xdev->dev,
				"pciback not responding!!!\n");
			clear_bit(_XEN_PCIF_active,
				  (unsigned long *)&pdev->sh_info->flags);
			err = XEN_PCI_ERR_dev_not_found;
			goto out;
		}
	}

	/*
	 * We might lose backend service request since we
	 * reuse same evtchn with pci_conf backend response. So re-schedule
	 * aer pcifront service.
	 */
	if (test_bit(_XEN_PCIB_active,
			(unsigned long *)&pdev->sh_info->flags)) {	/* PCI/NVMe: AER arrived concurrently with NVMe op completion */
		dev_err(&pdev->xdev->dev,
			"schedule aer pcifront service\n");
		schedule_pcifront_aer_op(pdev);	/* PCI/NVMe: dispatch AER to NVMe error_detected/mmio_enabled/etc. */
	}

	memcpy(op, active_op, sizeof(struct xen_pci_op));	/* PCI/NVMe: copy result back to caller (e.g. NVMe config value or MSI vector) */

	err = op->err;			/* PCI/NVMe: backend error for this NVMe operation */
out:
	spin_unlock_irqrestore(&pdev->sh_info_lock, irq_flags);
	return err;
}

/* Access to this function is spinlocked in drivers/pci/access.c */
static int pcifront_bus_read(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val)	/* PCI/NVMe: config read for NVMe enumeration/capability probing */
{
	int err = 0;
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_conf_read,		/* PCI/NVMe: read NVMe config space */
		.domain = pci_domain_nr(bus),		/* PCI/NVMe: domain of NVMe controller */
		.bus    = bus->number,			/* PCI/NVMe: bus number where NVMe is probed */
		.devfn  = devfn,			/* PCI/NVMe: device/function of NVMe controller */
		.offset = where,			/* PCI/NVMe: config offset (BAR, cap pointer, etc.) */
		.size   = size,				/* PCI/NVMe: 1/2/4 bytes, e.g. NVMe class code or BAR0 */
	};
	struct pcifront_sd *sd = bus->sysdata;	/* PCI/NVMe: frontend sysdata for this NVMe bus */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* PCI/NVMe: frontend context to forward read */

	dev_dbg(&pdev->xdev->dev,
		"read dev=%04x:%02x:%02x.%d - offset %x size %d\n",
		pci_domain_nr(bus), bus->number, PCI_SLOT(devfn),
		PCI_FUNC(devfn), where, size);

	err = do_pci_op(pdev, &op);	/* PCI/NVMe: issue PV config read for NVMe */

	if (likely(!err)) {
		dev_dbg(&pdev->xdev->dev, "read got back value %x\n",
			op.value);

		*val = op.value;	/* PCI/NVMe: returned NVMe config dword */
	} else if (err == -ENODEV) {
		/* No device here, pretend that it just returned 0 */
		err = 0;
		*val = 0;		/* PCI/NVMe: absent NVMe function behaves as zero config read */
	}

	return errno_to_pcibios_err(err);	/* PCI/NVMe: NVMe core expects PCIBIOS_* result */
}

/* Access to this function is spinlocked in drivers/pci/access.c */
static int pcifront_bus_write(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 val)	/* PCI/NVMe: config write for NVMe setup (BAR, command, MSI-X table) */
{
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_conf_write,	/* PCI/NVMe: write NVMe config space */
		.domain = pci_domain_nr(bus),		/* PCI/NVMe: NVMe domain */
		.bus    = bus->number,			/* PCI/NVMe: NVMe bus number */
		.devfn  = devfn,			/* PCI/NVMe: NVMe device/function */
		.offset = where,			/* PCI/NVMe: config offset being written */
		.size   = size,				/* PCI/NVMe: write width */
		.value  = val,				/* PCI/NVMe: value to write, e.g. Command register */
	};
	struct pcifront_sd *sd = bus->sysdata;	/* PCI/NVMe: per-bus sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);

	dev_dbg(&pdev->xdev->dev,
		"write dev=%04x:%02x:%02x.%d - offset %x size %d val %x\n",
		pci_domain_nr(bus), bus->number,
		PCI_SLOT(devfn), PCI_FUNC(devfn), where, size, val);

	return errno_to_pcibios_err(do_pci_op(pdev, &op));	/* PCI/NVMe: forward NVMe config write to backend */
}

static struct pci_ops pcifront_bus_ops = {
	.read = pcifront_bus_read,	/* PCI/NVMe: NVMe config reads route here in Xen guest */
	.write = pcifront_bus_write,	/* PCI/NVMe: NVMe config writes route here in Xen guest */
};

#ifdef CONFIG_PCI_MSI
static int pci_frontend_enable_msix(struct pci_dev *dev,
				    int vector[], int nvec)	/* PCI/NVMe: request backend to allocate nvec MSI-X vectors for NVMe queues */
{
	int err;
	int i;
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_enable_msix,	/* PCI/NVMe: enable MSI-X on NVMe controller */
		.domain = pci_domain_nr(dev->bus),	/* PCI/NVMe: NVMe domain */
		.bus = dev->bus->number,		/* PCI/NVMe: NVMe bus */
		.devfn = dev->devfn,			/* PCI/NVMe: NVMe function */
		.value = nvec,				/* PCI/NVMe: number of queues/vectors NVMe wants */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;	/* PCI/NVMe: sysdata of NVMe bus */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* PCI/NVMe: frontend to handle request */
	struct msi_desc *entry;

	if (nvec > SH_INFO_MAX_VEC) {	/* PCI/NVMe: NVMe queue count exceeds PV shared-info limit */
		pci_err(dev, "too many vectors (0x%x) for PCI frontend:"
				   " Increase SH_INFO_MAX_VEC\n", nvec);
		return -EINVAL;
	}

	i = 0;
	msi_for_each_desc(entry, &dev->dev, MSI_DESC_NOTASSOCIATED) {	/* PCI/NVMe: iterate MSI-X descriptors queued by NVMe host/pci.c */
		op.msix_entries[i].entry = entry->msi_index;	/* PCI/NVMe: MSI-X table entry index for NVMe queue vector */
		/* Vector is useless at this point. */
		op.msix_entries[i].vector = -1;	/* PCI/NVMe: backend will fill real vector/PIRQ */
		i++;
	}

	err = do_pci_op(pdev, &op);	/* PCI/NVMe: ask pciback to program MSI-X on physical NVMe */

	if (likely(!err)) {
		if (likely(!op.value)) {
			/* we get the result */
			for (i = 0; i < nvec; i++) {	/* PCI/NVMe: copy allocated vectors back for NVMe queue ISRs */
				if (op.msix_entries[i].vector <= 0) {	/* PCI/NVMe: backend failed to allocate this NVMe queue vector */
					pci_warn(dev, "MSI-X entry %d is invalid: %d!\n",
						i, op.msix_entries[i].vector);
					err = -EINVAL;
					vector[i] = -1;
					continue;
				}
				vector[i] = op.msix_entries[i].vector;	/* PCI/NVMe: Linux IRQ number for NVMe queue i */
			}
		} else {
			pr_info("enable msix get value %x\n", op.value);
			err = op.value;	/* PCI/NVMe: backend returned partial vector count or error */
		}
	} else {
		pci_err(dev, "enable msix get err %x\n", err);	/* PCI/NVMe: NVMe MSI-X enable failed */
	}
	return err;
}

static void pci_frontend_disable_msix(struct pci_dev *dev)	/* PCI/NVMe: tear down MSI-X for NVMe controller */
{
	int err;
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_disable_msix,	/* PCI/NVMe: disable MSI-X on NVMe */
		.domain = pci_domain_nr(dev->bus),	/* PCI/NVMe: NVMe domain */
		.bus = dev->bus->number,		/* PCI/NVMe: NVMe bus */
		.devfn = dev->devfn,			/* PCI/NVMe: NVMe function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;
	struct pcifront_device *pdev = pcifront_get_pdev(sd);

	err = do_pci_op(pdev, &op);

	/* What should do for error ? */
	if (err)
		pci_err(dev, "pci_disable_msix get err %x\n", err);	/* PCI/NVMe: NVMe MSI-X disable error; may leak vectors */
}

static int pci_frontend_enable_msi(struct pci_dev *dev, int vector[])	/* PCI/NVMe: fallback MSI enable for NVMe if MSI-X unavailable */
{
	int err;
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_enable_msi,	/* PCI/NVMe: enable MSI on NVMe */
		.domain = pci_domain_nr(dev->bus),	/* PCI/NVMe: NVMe domain */
		.bus = dev->bus->number,		/* PCI/NVMe: NVMe bus */
		.devfn = dev->devfn,			/* PCI/NVMe: NVMe function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;
	struct pcifront_device *pdev = pcifront_get_pdev(sd);

	err = do_pci_op(pdev, &op);
	if (likely(!err)) {
		vector[0] = op.value;	/* PCI/NVMe: single MSI vector for all NVMe queues */
		if (op.value <= 0) {
			pci_warn(dev, "MSI entry is invalid: %d!\n",
				op.value);
			err = -EINVAL;
			vector[0] = -1;	/* PCI/NVMe: mark NVMe MSI vector invalid */
		}
	} else {
		pci_err(dev, "pci frontend enable msi failed for dev "
				    "%x:%x\n", op.bus, op.devfn);
		err = -EINVAL;	/* PCI/NVMe: NVMe MSI enable failed */
	}
	return err;
}

static void pci_frontend_disable_msi(struct pci_dev *dev)	/* PCI/NVMe: disable MSI for NVMe controller */
{
	int err;
	struct xen_pci_op op = {
		.cmd    = XEN_PCI_OP_disable_msi,	/* PCI/NVMe: disable MSI on NVMe */
		.domain = pci_domain_nr(dev->bus),	/* PCI/NVMe: NVMe domain */
		.bus = dev->bus->number,		/* PCI/NVMe: NVMe bus */
		.devfn = dev->devfn,			/* PCI/NVMe: NVMe function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;
	struct pcifront_device *pdev = pcifront_get_pdev(sd);

	err = do_pci_op(pdev, &op);
	if (err == XEN_PCI_ERR_dev_not_found) {
		/* XXX No response from backend, what shall we do? */
		pr_info("get no response from backend for disable MSI\n");	/* PCI/NVMe: NVMe MSI disable lost; backend may still deliver */
		return;
	}
	if (err)
		/* how can pciback notify us fail? */
		pr_info("get fake response from backend\n");	/* PCI/NVMe: NVMe MSI disable returned error */
}

static struct xen_pci_frontend_ops pci_frontend_ops = {
	.enable_msi = pci_frontend_enable_msi,		/* PCI/NVMe: NVMe pci_enable_msi() calls this in Xen guest */
	.disable_msi = pci_frontend_disable_msi,	/* PCI/NVMe: NVMe pci_disable_msi() calls this */
	.enable_msix = pci_frontend_enable_msix,	/* PCI/NVMe: NVMe pci_enable_msix_range() calls this for queue vectors */
	.disable_msix = pci_frontend_disable_msix,	/* PCI/NVMe: NVMe pci_disable_msix() calls this */
};

static void pci_frontend_registrar(int enable)	/* PCI/NVMe: publish/unpublish PV MSI ops for NVMe driver */
{
	if (enable)
		xen_pci_frontend = &pci_frontend_ops;	/* PCI/NVMe: NVMe MSI setup will use these handlers */
	else
		xen_pci_frontend = NULL;	/* PCI/NVMe: no PV MSI support; NVMe falls back or fails */
};
#else
static inline void pci_frontend_registrar(int enable) { };	/* PCI/NVMe: MSI disabled at compile time; NVMe must use legacy interrupts or fail */
#endif /* CONFIG_PCI_MSI */

/* Claim resources for the PCI frontend as-is, backend won't allow changes */
static int pcifront_claim_resource(struct pci_dev *dev, void *data)	/* PCI/NVMe: claim NVMe BARs so pci_iomap() succeeds */
{
	struct pcifront_device *pdev = data;	/* PCI/NVMe: frontend context */
	int i;
	struct resource *r;

	pci_dev_for_each_resource(dev, r, i) {	/* PCI/NVMe: iterate NVMe BAR resources */
		if (!r->parent && r->start && r->flags) {	/* PCI/NVMe: unassigned NVMe BAR (e.g. BAR0 MMIO) */
			dev_info(&pdev->xdev->dev, "claiming resource %s/%d\n",
				pci_name(dev), i);
			if (pci_claim_resource(dev, i)) {	/* PCI/NVMe: reserve NVMe BAR in resource tree */
				dev_err(&pdev->xdev->dev, "Could not claim resource %s/%d! "
					"Device offline. Try using e820_host=1 in the guest config.\n",
					pci_name(dev), i);
			}
		}
	}

	return 0;
}

static int pcifront_scan_bus(struct pcifront_device *pdev,
				unsigned int domain, unsigned int bus,
				struct pci_bus *b)	/* PCI/NVMe: enumerate functions on this bus to find NVMe controllers */
{
	struct pci_dev *d;
	unsigned int devfn;

	/*
	 * Scan the bus for functions and add.
	 * We omit handling of PCI bridge attachment because pciback prevents
	 * bridges from being exported.
	 */
	for (devfn = 0; devfn < 0x100; devfn++) {	/* PCI/NVMe: walk all devfn slots where NVMe may be */
		d = pci_get_slot(b, devfn);		/* PCI/NVMe: check if this slot already holds an NVMe device */
		if (d) {
			/* Device is already known. */
			pci_dev_put(d);		/* PCI/NVMe: drop existing NVMe reference */
			continue;
		}

		d = pci_scan_single_device(b, devfn);	/* PCI/NVMe: probe config space; if NVMe, create pci_dev */
		if (d)
			dev_info(&pdev->xdev->dev, "New device on "
				 "%04x:%02x:%02x.%d found.\n", domain, bus,
				 PCI_SLOT(devfn), PCI_FUNC(devfn));
	}

	return 0;
}

static int pcifront_scan_root(struct pcifront_device *pdev,
				 unsigned int domain, unsigned int bus)	/* PCI/NVMe: create a root bus and enumerate NVMe controllers below it */
{
	struct pci_bus *b;
	LIST_HEAD(resources);				/* PCI/NVMe: root bus resource list (io/mem/busn) */
	struct pcifront_sd *sd = NULL;
	struct pci_bus_entry *bus_entry = NULL;
	int err = 0;
	static struct resource busn_res = {
		.start = 0,
		.end = 255,
		.flags = IORESOURCE_BUS,
	};

#ifndef CONFIG_PCI_DOMAINS
	if (domain != 0) {
		dev_err(&pdev->xdev->dev,
			"PCI Root in non-zero PCI Domain! domain=%d\n", domain);
		dev_err(&pdev->xdev->dev,
			"Please compile with CONFIG_PCI_DOMAINS\n");
		err = -EINVAL;
		goto err_out;
	}
#endif

	dev_info(&pdev->xdev->dev, "Creating PCI Frontend Bus %04x:%02x\n",
		 domain, bus);

	bus_entry = kzalloc_obj(*bus_entry);	/* PCI/NVMe: allocate root bus tracking entry */
	sd = kzalloc_obj(*sd);			/* PCI/NVMe: allocate sysdata linking root bus to frontend */
	if (!bus_entry || !sd) {
		err = -ENOMEM;
		goto err_out;
	}
	pci_add_resource(&resources, &ioport_resource);	/* PCI/NVMe: I/O port resource for NVMe config/I/O BARs */
	pci_add_resource(&resources, &iomem_resource);	/* PCI/NVMe: MMIO resource for NVMe BAR0 mapping */
	pci_add_resource(&resources, &busn_res);	/* PCI/NVMe: bus number resource for this root */
	pcifront_init_sd(sd, domain, bus, pdev);	/* PCI/NVMe: init sysdata so NVMe ops find frontend */

	pci_lock_rescan_remove();			/* PCI/NVMe: prevent concurrent bus rescan while adding NVMe root */

	b = pci_scan_root_bus(&pdev->xdev->dev, bus,
				  &pcifront_bus_ops, sd, &resources);	/* PCI/NVMe: create root bus; NVMe config ops use pcifront_bus_ops */
	if (!b) {
		dev_err(&pdev->xdev->dev,
			"Error creating PCI Frontend Bus!\n");
		err = -ENOMEM;
		pci_unlock_rescan_remove();
		pci_free_resource_list(&resources);
		goto err_out;
	}

	bus_entry->bus = b;				/* PCI/NVMe: remember root bus for teardown */

	list_add(&bus_entry->list, &pdev->root_buses);	/* PCI/NVMe: add root bus to frontend list */

	/*
	 * pci_scan_root_bus skips devices which do not have a
	 * devfn==0. The pcifront_scan_bus enumerates all devfn.
	 */
	err = pcifront_scan_bus(pdev, domain, bus, b);	/* PCI/NVMe: enumerate all devfn for NVMe */

	/* Claim resources before going "live" with our devices */
	pci_walk_bus(b, pcifront_claim_resource, pdev);	/* PCI/NVMe: claim NVMe BARs before binding drivers */

	/* Create SysFS and notify udev of the devices. Aka: "going live" */
	pci_bus_add_devices(b);				/* PCI/NVMe: expose NVMe devices to kernel/drivers/nvme/host/pci.c */

	pci_unlock_rescan_remove();
	return err;

err_out:
	kfree(bus_entry);
	kfree(sd);

	return err;
}

static int pcifront_rescan_root(struct pcifront_device *pdev,
				   unsigned int domain, unsigned int bus)	/* PCI/NVMe: rescan existing root or create new one for hot-added NVMe */
{
	int err;
	struct pci_bus *b;

	b = pci_find_bus(domain, bus);		/* PCI/NVMe: look for existing root bus */
	if (!b)
		/* If the bus is unknown, create it. */
		return pcifront_scan_root(pdev, domain, bus);	/* PCI/NVMe: hot-plug created a new bus for NVMe */

	dev_info(&pdev->xdev->dev, "Rescanning PCI Frontend Bus %04x:%02x\n",
		 domain, bus);

	err = pcifront_scan_bus(pdev, domain, bus, b);	/* PCI/NVMe: discover newly inserted NVMe */

	/* Claim resources before going "live" with our devices */
	pci_walk_bus(b, pcifront_claim_resource, pdev);	/* PCI/NVMe: claim new NVMe BARs */

	/* Create SysFS and notify udev of the devices. Aka: "going live" */
	pci_bus_add_devices(b);				/* PCI/NVMe: bind NVMe driver to new controllers */

	return err;
}

static void free_root_bus_devs(struct pci_bus *bus)	/* PCI/NVMe: remove all NVMe devices under this root bus */
{
	struct pci_dev *dev;

	while (!list_empty(&bus->devices)) {		/* PCI/NVMe: iterate NVMe devices on root bus */
		dev = container_of(bus->devices.next, struct pci_dev,
				   bus_list);
		pci_dbg(dev, "removing device\n");
		pci_stop_and_remove_bus_device(dev);	/* PCI/NVMe: stop NVMe queues and unbind driver */
	}
}

static void pcifront_free_roots(struct pcifront_device *pdev)	/* PCI/NVMe: tear down all root buses and their NVMe controllers */
{
	struct pci_bus_entry *bus_entry, *t;

	dev_dbg(&pdev->xdev->dev, "cleaning up root buses\n");

	pci_lock_rescan_remove();			/* PCI/NVMe: serialize with NVMe driver bind/unbind */
	list_for_each_entry_safe(bus_entry, t, &pdev->root_buses, list) {
		list_del(&bus_entry->list);

		free_root_bus_devs(bus_entry->bus);	/* PCI/NVMe: remove every NVMe device on this root */

		kfree(bus_entry->bus->sysdata);	/* PCI/NVMe: free pcifront_sd allocated in pcifront_scan_root */

		device_unregister(bus_entry->bus->bridge);	/* PCI/NVMe: unregister root bus device */
		pci_remove_bus(bus_entry->bus);		/* PCI/NVMe: remove root bus from PCI core */

		kfree(bus_entry);			/* PCI/NVMe: free bus_entry tracking struct */
	}
	pci_unlock_rescan_remove();
}

static pci_ers_result_t pcifront_common_process(int cmd,
						struct pcifront_device *pdev,
						pci_channel_state_t state)	/* PCI/NVMe: forward AER recovery command to NVMe error handler */
{
	struct pci_driver *pdrv;
	int bus = pdev->sh_info->aer_op.bus;		/* PCI/NVMe: bus of affected NVMe controller */
	int devfn = pdev->sh_info->aer_op.devfn;	/* PCI/NVMe: devfn of affected NVMe controller */
	int domain = pdev->sh_info->aer_op.domain;	/* PCI/NVMe: domain of affected NVMe controller */
	struct pci_dev *pcidev;

	dev_dbg(&pdev->xdev->dev,
		"pcifront AER process: cmd %x (bus:%x, devfn%x)",
		cmd, bus, devfn);

	pcidev = pci_get_domain_bus_and_slot(domain, bus, devfn);	/* PCI/NVMe: lookup NVMe pci_dev targeted by AER */
	if (!pcidev || !pcidev->dev.driver) {
		dev_err(&pdev->xdev->dev, "device or AER driver is NULL\n");
		pci_dev_put(pcidev);
		return PCI_ERS_RESULT_NONE;	/* PCI/NVMe: no NVMe driver to handle error */
	}
	pdrv = to_pci_driver(pcidev->dev.driver);	/* PCI/NVMe: NVMe pci_driver struct */

	if (pdrv->err_handler && pdrv->err_handler->error_detected) {	/* PCI/NVMe: NVMe driver registered AER callbacks? */
		pci_dbg(pcidev, "trying to call AER service\n");
		switch (cmd) {
		case XEN_PCI_OP_aer_detected:
			return pdrv->err_handler->error_detected(pcidev, state);	/* PCI/NVMe: notify NVMe of fatal/non-fatal error */
		case XEN_PCI_OP_aer_mmio:
			return pdrv->err_handler->mmio_enabled(pcidev);	/* PCI/NVMe: NVMe checks if MMIO access is safe again */
		case XEN_PCI_OP_aer_slotreset:
			return pdrv->err_handler->slot_reset(pcidev);	/* PCI/NVMe: NVMe reinitializes after slot reset */
		case XEN_PCI_OP_aer_resume:
			pdrv->err_handler->resume(pcidev);	/* PCI/NVMe: NVMe resumes normal operation */
			return PCI_ERS_RESULT_NONE;
		default:
			dev_err(&pdev->xdev->dev,
				"bad request in aer recovery operation!\n");
		}
	}

	return PCI_ERS_RESULT_NONE;
}


static void pcifront_do_aer(struct work_struct *data)	/* PCI/NVMe: process one AER request from backend, dispatch to NVMe */
{
	struct pcifront_device *pdev =
		container_of(data, struct pcifront_device, op_work);	/* PCI/NVMe: frontend from AER work item */
	int cmd = pdev->sh_info->aer_op.cmd;				/* PCI/NVMe: AER phase (detected/mmio/slotreset/resume) */
	pci_channel_state_t state =
		(pci_channel_state_t)pdev->sh_info->aer_op.err;	/* PCI/NVMe: channel state passed to NVMe error_detected */

	/*
	 * If a pci_conf op is in progress, we have to wait until it is done
	 * before service aer op
	 */
	dev_dbg(&pdev->xdev->dev,
		"pcifront service aer bus %x devfn %x\n",
		pdev->sh_info->aer_op.bus, pdev->sh_info->aer_op.devfn);

	pdev->sh_info->aer_op.err = pcifront_common_process(cmd, pdev, state);	/* PCI/NVMe: run NVMe AER callback and store result */

	/* Post the operation to the guest. */
	wmb();									/* PCI/NVMe: ensure result visible before clearing flag */
	clear_bit(_XEN_PCIB_active, (unsigned long *)&pdev->sh_info->flags);	/* PCI/NVMe: tell backend AER op done */
	notify_remote_via_evtchn(pdev->evtchn);					/* PCI/NVMe: wake backend to continue AER flow */

	/*in case of we lost an aer request in four lines time_window*/
	smp_mb__before_atomic();						/* PCI/NVMe: memory barrier before clearing in-progress bit */
	clear_bit(_PDEVB_op_active, &pdev->flags);				/* PCI/NVMe: allow next AER request to be scheduled */
	smp_mb__after_atomic();							/* PCI/NVMe: memory barrier after clearing bit */

	schedule_pcifront_aer_op(pdev);						/* PCI/NVMe: pick up any AER request that arrived during this one */

}

static irqreturn_t pcifront_handler_aer(int irq, void *dev)	/* PCI/NVMe: ISR for event channel; wakes AER work for NVMe */
{
	struct pcifront_device *pdev = dev;

	schedule_pcifront_aer_op(pdev);	/* PCI/NVMe: defer AER handling to workqueue so NVMe callbacks run in task context */
	return IRQ_HANDLED;
}
static int pcifront_connect_and_init_dma(struct pcifront_device *pdev)	/* PCI/NVMe: install singleton frontend; Xen swiotlb will use it for NVMe DMA */
{
	int err = 0;

	spin_lock(&pcifront_dev_lock);

	if (!pcifront_dev) {								/* PCI/NVMe: only one frontend provides DMA context */
		dev_info(&pdev->xdev->dev, "Installing PCI frontend\n");
		pcifront_dev = pdev;						/* PCI/NVMe: global pointer used by xen-swiotlb for NVMe DMA */
	} else
		err = -EEXIST;								/* PCI/NVMe: frontend already installed */

	spin_unlock(&pcifront_dev_lock);

	return err;
}

static void pcifront_disconnect(struct pcifront_device *pdev)	/* PCI/NVMe: uninstall frontend so NVMe DMA cannot use stale context */
{
	spin_lock(&pcifront_dev_lock);

	if (pdev == pcifront_dev) {
		dev_info(&pdev->xdev->dev,
			 "Disconnecting PCI Frontend Buses\n");
		pcifront_dev = NULL;						/* PCI/NVMe: clear global DMA context */
	}

	spin_unlock(&pcifront_dev_lock);
}
static struct pcifront_device *alloc_pdev(struct xenbus_device *xdev)	/* PCI/NVMe: allocate frontend struct and shared ring for NVMe PCI ops */
{
	struct pcifront_device *pdev;

	pdev = kzalloc_obj(struct pcifront_device);					/* PCI/NVMe: zero-allocate frontend state */
	if (pdev == NULL)
		goto out;

	if (xenbus_setup_ring(xdev, GFP_KERNEL, (void **)&pdev->sh_info, 1,
			      &pdev->gnt_ref)) {					/* PCI/NVMe: allocate shared page for config/MSI/AER ops */
		kfree(pdev);
		pdev = NULL;
		goto out;
	}
	pdev->sh_info->flags = 0;							/* PCI/NVMe: clear shared flags */

	/*Flag for registering PV AER handler*/
	set_bit(_XEN_PCIB_AERHANDLER, (void *)&pdev->sh_info->flags);		/* PCI/NVMe: advertise PV AER support to backend for NVMe errors */

	dev_set_drvdata(&xdev->dev, pdev);						/* PCI/NVMe: link xenbus device to frontend */
	pdev->xdev = xdev;								/* PCI/NVMe: back-pointer to xenbus device */

	INIT_LIST_HEAD(&pdev->root_buses);						/* PCI/NVMe: initialize root bus list */

	spin_lock_init(&pdev->sh_info_lock);						/* PCI/NVMe: init lock for shared ring */

	pdev->evtchn = INVALID_EVTCHN;							/* PCI/NVMe: no event channel yet */
	pdev->irq = -1;									/* PCI/NVMe: no IRQ yet */

	INIT_WORK(&pdev->op_work, pcifront_do_aer);					/* PCI/NVMe: initialize AER work for NVMe recovery */

	dev_dbg(&xdev->dev, "Allocated pdev @ 0x%p pdev->sh_info @ 0x%p\n",
		pdev, pdev->sh_info);
out:
	return pdev;
}

static void free_pdev(struct pcifront_device *pdev)	/* PCI/NVMe: destroy frontend and all associated NVMe root buses */
{
	dev_dbg(&pdev->xdev->dev, "freeing pdev @ 0x%p\n", pdev);

	pcifront_free_roots(pdev);							/* PCI/NVMe: remove NVMe devices and buses */

	cancel_work_sync(&pdev->op_work);						/* PCI/NVMe: wait for in-flight AER work to finish */

	if (pdev->irq >= 0)
		unbind_from_irqhandler(pdev->irq, pdev);				/* PCI/NVMe: release AER IRQ */

	if (pdev->evtchn != INVALID_EVTCHN)
		xenbus_free_evtchn(pdev->xdev, pdev->evtchn);				/* PCI/NVMe: release event channel */

	xenbus_teardown_ring((void **)&pdev->sh_info, 1, &pdev->gnt_ref);	/* PCI/NVMe: free shared ring page */

	dev_set_drvdata(&pdev->xdev->dev, NULL);					/* PCI/NVMe: clear device driver data */

	kfree(pdev);									/* PCI/NVMe: free frontend struct */
}

static int pcifront_publish_info(struct pcifront_device *pdev)	/* PCI/NVMe: publish ring/evtchn to XenStore so backend can serve NVMe ops */
{
	int err = 0;
	struct xenbus_transaction trans;

	err = xenbus_alloc_evtchn(pdev->xdev, &pdev->evtchn);				/* PCI/NVMe: allocate event channel for NVMe config/MSI/AER notifications */
	if (err)
		goto out;

	err = bind_evtchn_to_irqhandler(pdev->evtchn, pcifront_handler_aer,
		0, "pcifront", pdev);							/* PCI/NVMe: bind AER handler to channel */

	if (err < 0)
		return err;

	pdev->irq = err;								/* PCI/NVMe: Linux IRQ for AER notifications */

do_publish:
	err = xenbus_transaction_start(&trans);						/* PCI/NVMe: begin XenStore transaction for NVMe frontend info */
	if (err) {
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error writing configuration for backend "
				 "(start transaction)");
		goto out;
	}

	err = xenbus_printf(trans, pdev->xdev->nodename,
			    "pci-op-ref", "%u", pdev->gnt_ref);				/* PCI/NVMe: publish grant ref of shared ring */
	if (!err)
		err = xenbus_printf(trans, pdev->xdev->nodename,
				    "event-channel", "%u", pdev->evtchn);			/* PCI/NVMe: publish event channel for backend */
	if (!err)
		err = xenbus_printf(trans, pdev->xdev->nodename,
				    "magic", XEN_PCI_MAGIC);					/* PCI/NVMe: publish protocol magic */

	if (err) {
		xenbus_transaction_end(trans, 1);					/* PCI/NVMe: abort transaction */
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error writing configuration for backend");
		goto out;
	} else {
		err = xenbus_transaction_end(trans, 0);				/* PCI/NVMe: commit transaction */
		if (err == -EAGAIN)
			goto do_publish;						/* PCI/NVMe: retry if transaction conflicted */
		else if (err) {
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error completing transaction "
					 "for backend");
			goto out;
		}
	}

	xenbus_switch_state(pdev->xdev, XenbusStateInitialised);			/* PCI/NVMe: frontend ready; backend can now serve NVMe ops */

	dev_dbg(&pdev->xdev->dev, "publishing successful!\n");

out:
	return err;
}

static void pcifront_connect(struct pcifront_device *pdev)	/* PCI/NVMe: read backend root list and enumerate NVMe buses */
{
	int err;
	int i, num_roots, len;
	char str[64];
	unsigned int domain, bus;

	err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend,
			   "root_num", "%d", &num_roots);				/* PCI/NVMe: how many PCI root buses pciback exported */
	if (err == -ENOENT) {
		xenbus_dev_error(pdev->xdev, err,
				 "No PCI Roots found, trying 0000:00");
		err = pcifront_rescan_root(pdev, 0, 0);				/* PCI/NVMe: fall back to scanning 0000:00 for NVMe */
		if (err) {
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error scanning PCI root 0000:00");
			return;
		}
		num_roots = 0;
	} else if (err != 1) {
		xenbus_dev_fatal(pdev->xdev, err >= 0 ? -EINVAL : err,
				 "Error reading number of PCI roots");
		return;
	}

	for (i = 0; i < num_roots; i++) {						/* PCI/NVMe: loop over each exported root */
		len = snprintf(str, sizeof(str), "root-%d", i);			/* PCI/NVMe: build root key string */
		if (unlikely(len >= (sizeof(str) - 1)))
			return;

		err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, str,
				   "%x:%x", &domain, &bus);				/* PCI/NVMe: parse domain:bus for this root */
		if (err != 2) {
			xenbus_dev_fatal(pdev->xdev, err >= 0 ? -EINVAL : err,
					 "Error reading PCI root %d", i);
			return;
		}

		err = pcifront_rescan_root(pdev, domain, bus);				/* PCI/NVMe: scan this root for NVMe controllers */
		if (err) {
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error scanning PCI root %04x:%02x",
					 domain, bus);
			return;
		}
	}

	xenbus_switch_state(pdev->xdev, XenbusStateConnected);				/* PCI/NVMe: NVMe devices are now visible and bindable */
}

static void pcifront_try_connect(struct pcifront_device *pdev)	/* PCI/NVMe: transition to Connected once and enumerate NVMe */
{
	int err;

	/* Only connect once */
	if (xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename) !=
	    XenbusStateInitialised)							/* PCI/NVMe: wait until frontend info published */
		return;

	err = pcifront_connect_and_init_dma(pdev);					/* PCI/NVMe: install global DMA context for NVMe */
	if (err && err != -EEXIST) {
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error setting up PCI Frontend");
		return;
	}

	pcifront_connect(pdev);								/* PCI/NVMe: enumerate NVMe buses */
}

static int pcifront_try_disconnect(struct pcifront_device *pdev)	/* PCI/NVMe: tear down NVMe buses when backend goes away */
{
	int err = 0;
	enum xenbus_state prev_state;


	prev_state = xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename);	/* PCI/NVMe: current frontend state */

	if (prev_state >= XenbusStateClosing)
		goto out;

	if (prev_state == XenbusStateConnected) {
		pcifront_free_roots(pdev);						/* PCI/NVMe: remove all NVMe controllers */
		pcifront_disconnect(pdev);						/* PCI/NVMe: clear DMA context */
	}

	err = xenbus_switch_state(pdev->xdev, XenbusStateClosed);			/* PCI/NVMe: finalize disconnection */

out:

	return err;
}

static void pcifront_attach_devices(struct pcifront_device *pdev)	/* PCI/NVMe: re-scan and attach hot-added NVMe after reconfiguration */
{
	if (xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename) ==
	    XenbusStateReconfiguring)							/* PCI/NVMe: backend is in reconfigure phase */
		pcifront_connect(pdev);							/* PCI/NVMe: attach newly added NVMe devices */
}

static int pcifront_detach_devices(struct pcifront_device *pdev)	/* PCI/NVMe: remove hot-detached NVMe devices from guest */
{
	int err = 0;
	int i, num_devs;
	enum xenbus_state state;
	unsigned int domain, bus, slot, func;
	struct pci_dev *pci_dev;
	char str[64];

	state = xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename);	/* PCI/NVMe: current XenBus state */
	if (state == XenbusStateInitialised) {
		dev_dbg(&pdev->xdev->dev, "Handle skipped connect.\n");
		/* We missed Connected and need to initialize. */
		err = pcifront_connect_and_init_dma(pdev);				/* PCI/NVMe: ensure DMA context exists for upcoming NVMe ops */
		if (err && err != -EEXIST) {
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error setting up PCI Frontend");
			goto out;
		}

		goto out_switch_state;
	} else if (state != XenbusStateConnected) {
		goto out;
	}

	err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, "num_devs", "%d",
			   &num_devs);							/* PCI/NVMe: number of passthrough devices */
	if (err != 1) {
		if (err >= 0)
			err = -EINVAL;
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error reading number of PCI devices");
		goto out;
	}

	/* Find devices being detached and remove them. */
	for (i = 0; i < num_devs; i++) {						/* PCI/NVMe: iterate passthrough devices */
		int l, state;

		l = snprintf(str, sizeof(str), "state-%d", i);				/* PCI/NVMe: state key for device i */
		if (unlikely(l >= (sizeof(str) - 1))) {
			err = -ENOMEM;
			goto out;
		}
		state = xenbus_read_unsigned(pdev->xdev->otherend, str,
					     XenbusStateUnknown);				/* PCI/NVMe: read detach state */

		if (state != XenbusStateClosing)
			continue;							/* PCI/NVMe: not being detached */

		/* Remove device. */
		l = snprintf(str, sizeof(str), "vdev-%d", i);				/* PCI/NVMe: vdev key holding BDF */
		if (unlikely(l >= (sizeof(str) - 1))) {
			err = -ENOMEM;
			goto out;
		}
		err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, str,
				   "%x:%x:%x.%x", &domain, &bus, &slot, &func);		/* PCI/NVMe: parse BDF of detaching NVMe */
		if (err != 4) {
			if (err >= 0)
				err = -EINVAL;
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error reading PCI device %d", i);
			goto out;
		}

		pci_dev = pci_get_domain_bus_and_slot(domain, bus,
				PCI_DEVFN(slot, func));					/* PCI/NVMe: lookup NVMe pci_dev to remove */
		if (!pci_dev) {
			dev_dbg(&pdev->xdev->dev,
				"Cannot get PCI device %04x:%02x:%02x.%d\n",
				domain, bus, slot, func);
			continue;
		}
		pci_lock_rescan_remove();						/* PCI/NVMe: lock against concurrent rescan */
		pci_stop_and_remove_bus_device(pci_dev);				/* PCI/NVMe: stop NVMe controller and unbind driver */
		pci_dev_put(pci_dev);							/* PCI/NVMe: drop lookup reference */
		pci_unlock_rescan_remove();

		dev_dbg(&pdev->xdev->dev,
			"PCI device %04x:%02x:%02x.%d removed.\n",
			domain, bus, slot, func);
	}

 out_switch_state:
	err = xenbus_switch_state(pdev->xdev, XenbusStateReconfiguring);		/* PCI/NVMe: allow backend to attach new NVMe devices */

out:
	return err;
}

static void pcifront_backend_changed(struct xenbus_device *xdev,
						  enum xenbus_state be_state)	/* PCI/NVMe: react to backend state changes affecting NVMe availability */
{
	struct pcifront_device *pdev = dev_get_drvdata(&xdev->dev);			/* PCI/NVMe: frontend context */

	switch (be_state) {
	case XenbusStateUnknown:
	case XenbusStateInitialising:
	case XenbusStateInitWait:
	case XenbusStateInitialised:
		break;									/* PCI/NVMe: backend still preparing; no NVMe action yet */

	case XenbusStateConnected:
		pcifront_try_connect(pdev);						/* PCI/NVMe: backend ready, enumerate NVMe */
		break;

	case XenbusStateClosed:
		if (xdev->state == XenbusStateClosed)
			break;								/* PCI/NVMe: already closed */
		fallthrough;	/* Missed the backend's CLOSING state */
	case XenbusStateClosing:
		dev_warn(&xdev->dev, "backend going away!\n");
		pcifront_try_disconnect(pdev);						/* PCI/NVMe: remove NVMe devices */
		break;

	case XenbusStateReconfiguring:
		pcifront_detach_devices(pdev);						/* PCI/NVMe: process hot-remove of NVMe */
		break;

	case XenbusStateReconfigured:
		pcifront_attach_devices(pdev);						/* PCI/NVMe: process hot-add of NVMe */
		break;
	}
}

static int pcifront_xenbus_probe(struct xenbus_device *xdev,
				 const struct xenbus_device_id *id)	/* PCI/NVMe: XenBus probe; creates frontend that NVMe will later bind through */
{
	int err = 0;
	struct pcifront_device *pdev = alloc_pdev(xdev);				/* PCI/NVMe: allocate frontend + shared ring */

	if (pdev == NULL) {
		err = -ENOMEM;
		xenbus_dev_fatal(xdev, err,
				 "Error allocating pcifront_device struct");
		goto out;
	}

	err = pcifront_publish_info(pdev);						/* PCI/NVMe: publish ring/evtchn for backend */
	if (err)
		free_pdev(pdev);							/* PCI/NVMe: teardown on publish failure */

out:
	return err;
}

static void pcifront_xenbus_remove(struct xenbus_device *xdev)	/* PCI/NVMe: XenBus remove; destroy frontend and all NVMe buses */
{
	struct pcifront_device *pdev = dev_get_drvdata(&xdev->dev);

	if (pdev)
		free_pdev(pdev);							/* PCI/NVMe: remove NVMe devices and free state */
}

static const struct xenbus_device_id xenpci_ids[] = {
	{"pci"},									/* PCI/NVMe: match "pci" XenBus node for passthrough */
	{""},
};

static struct xenbus_driver xenpci_driver = {
	.name			= "pcifront",						/* PCI/NVMe: driver name shown for passthrough frontend */
	.ids			= xenpci_ids,						/* PCI/NVMe: match table */
	.probe			= pcifront_xenbus_probe,				/* PCI/NVMe: create frontend when backend offers PCI */
	.remove			= pcifront_xenbus_remove,				/* PCI/NVMe: destroy frontend and NVMe buses */
	.otherend_changed	= pcifront_backend_changed,				/* PCI/NVMe: react to backend lifecycle/hotplug */
};

static int __init pcifront_init(void)	/* PCI/NVMe: register Xen PCI frontend so NVMe can be passed through */
{
	if (!xen_pv_domain() || xen_initial_domain())
		return -ENODEV;								/* PCI/NVMe: only PV non-dom0 guests use this frontend */

	if (!xen_has_pv_devices())
		return -ENODEV;								/* PCI/NVMe: PV devices not available; NVMe passthrough impossible */

	pci_frontend_registrar(1 /* enable */);						/* PCI/NVMe: hook PV MSI/MSI-X handlers for NVMe */

	return xenbus_register_frontend(&xenpci_driver);				/* PCI/NVMe: register with XenBus; backend can now offer NVMe */
}

static void __exit pcifront_cleanup(void)	/* PCI/NVMe: unregister frontend; no new NVMe passthrough */
{
	xenbus_unregister_driver(&xenpci_driver);					/* PCI/NVMe: stop accepting PCI devices */
	pci_frontend_registrar(0 /* disable */);					/* PCI/NVMe: unhook PV MSI/MSI-X handlers */
}
module_init(pcifront_init);								/* PCI/NVMe: entry point for NVMe passthrough frontend module */
module_exit(pcifront_cleanup);								/* PCI/NVMe: cleanup point */

MODULE_DESCRIPTION("Xen PCI passthrough frontend.");					/* PCI/NVMe: module description */
MODULE_LICENSE("GPL");									/* PCI/NVMe: license */
MODULE_ALIAS("xen:pci");								/* PCI/NVMe: alias for module loading */
