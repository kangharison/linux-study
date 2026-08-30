// SPDX-License-Identifier: GPL-2.0
/*
 * Interface with platform TEE Security Manager (TSM) objects as defined by
 * PCIe r7.0 section 11 TEE Device Interface Security Protocol (TDISP)
 *
 * Copyright(c) 2024-2025 Intel Corporation. All rights reserved.
 */
/* PCI/NVMe: TDISP/TSM secures the PCIe link to NVMe endpoints; an NVMe host
 * driver (drivers/nvme/host/pci.c) later binds only after the PCI core has
 * enumerated the function, negotiated TDISP state here, and established DMA
 * trust boundaries that affect dma_set_mask_and_coherent() mapping behavior.
 */

#define dev_fmt(fmt) "PCI/TSM: " fmt       /* PCI/NVMe: prefix for printk lines in this file */

#include <linux/bitfield.h>                 /* PCI/NVMe: FIELD_GET/FIELD_PREP helpers used by PCI caps */
#include <linux/pci.h>                      /* PCI/NVMe: core PCIe definitions, struct pci_dev, capability access */
#include <linux/pci-doe.h>                  /* PCI/NVMe: Data Object Exchange mailbox for TDISP protocol negotiation */
#include <linux/pci-tsm.h>                  /* PCI/NVMe: TSM/TDISP internal types exported to drivers/pci */
#include <linux/sysfs.h>                    /* PCI/NVMe: device_attribute and attribute_group for tsm/ sysfs */
#include <linux/tsm.h>                      /* PCI/NVMe: generic Trusted Execution Environment Security Manager type */
#include <linux/xarray.h>                   /* PCI/NVMe: sparse array storage for TSM device lookups */
#include "pci.h"                            /* PCI/NVMe: private PCI core declarations including TSM helpers */

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a TSM instance
 */
static DECLARE_RWSEM(pci_tsm_rwsem);        /* PCI/NVMe: protects TSM attach/detach while NVMe probe may run concurrently */

/*
 * Count of TSMs registered that support physical link operations vs device
 * security state management.
 */
static int pci_tsm_link_count;              /* PCI/NVMe: number of link-level TSMs; enables tsm/ sysfs visibility */
static int pci_tsm_devsec_count;            /* PCI/NVMe: number of device-security TSMs; separate counter for non-link paths */

static const struct pci_tsm_ops *to_pci_tsm_ops(struct pci_tsm *tsm)
/* PCI/NVMe: fetch operation vector from TSM, used to drive bind/unbind for an NVMe function */
{
	return tsm->tsm_dev->pci_ops;           /* PCI/NVMe: pci_ops supplied by the platform TSM module */
}

static inline bool is_dsm(struct pci_dev *pdev)
/* PCI/NVMe: true if @pdev hosts the Device Security Manager for its TDISP domain */
{
	return pdev->tsm && pdev->tsm->dsm_dev == pdev;
	                                       /* PCI/NVMe: DSM is the authoritative security manager for this function */
}

static inline bool has_tee(struct pci_dev *pdev)
/* PCI/NVMe: check PCIe Device Capabilities TEE bit, relevant for NVMe endpoints claiming TDISP support */
{
	return pdev->devcap & PCI_EXP_DEVCAP_TEE;
	                                       /* PCI/NVMe: TEE capability tells the NVMe host that link-level TDISP is available */
}

/* 'struct pci_tsm_pf0' wraps 'struct pci_tsm' when ->dsm_dev == ->pdev (self) */
static struct pci_tsm_pf0 *to_pci_tsm_pf0(struct pci_tsm *tsm)
/* PCI/NVMe: convert a generic TSM context to the PF0/DSM wrapper that owns the mutex and DOE mailbox */
{
	/*
	 * All "link" TSM contexts reference the device that hosts the DSM
	 * interface for a set of devices. Walk to the DSM device and cast its
	 * ->tsm context to a 'struct pci_tsm_pf0 *'.
	 */
	struct pci_dev *pf0 = tsm->dsm_dev;     /* PCI/NVMe: DSM device for this NVMe function's TDISP domain */

	if (!is_pci_tsm_pf0(pf0) || !is_dsm(pf0)) {
		pci_WARN_ONCE(tsm->pdev, 1, "invalid context object\n");
		                                       /* PCI/NVMe: mismatch between DSM and TSM wrapper is a driver bug */
		return NULL;
	}

	return container_of(pf0->tsm, struct pci_tsm_pf0, base_tsm);
	                                       /* PCI/NVMe: PF0 wrapper embeds struct pci_tsm at ->base_tsm */
}

static void tsm_remove(struct pci_tsm *tsm)
/* PCI/NVMe: tear down the TSM context established during NVMe device enumeration */
{
	struct pci_dev *pdev;

	if (!tsm)                               /* PCI/NVMe: tolerate NULL from cleanup paths */
		return;

	pdev = tsm->pdev;                       /* PCI/NVMe: NVMe PCIe function whose TSM state is being removed */
	to_pci_tsm_ops(tsm)->remove(tsm);       /* PCI/NVMe: platform TSM callback releases per-function TDISP resources */
	pdev->tsm = NULL;                       /* PCI/NVMe: detach TSM from pci_dev so later NVMe re-probe starts clean */
}
DEFINE_FREE(tsm_remove, struct pci_tsm *, if (_T) tsm_remove(_T))
/* PCI/NVMe: cleanup helper used with __free so a local tsm pointer is removed on scope exit */

static void pci_tsm_walk_fns(struct pci_dev *pdev,
			     int (*cb)(struct pci_dev *pdev, void *data),
			     void *data)
/* PCI/NVMe: iterate PFs and their VFs, applying @cb to set up TDISP context after DSM connect */
{
	/* Walk subordinate physical functions */
	for (int i = 0; i < 8; i++) {          /* PCI/NVMe: 8 functions per PCIe device slot */
		struct pci_dev *pf __free(pci_dev_put) = pci_get_slot(
			pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), i));
		                                       /* PCI/NVMe: look up sibling function i on the same bus/slot */

		if (!pf)                        /* PCI/NVMe: function not populated, skip */
			continue;

		/* on entry function 0 has already run @cb */
		if (i > 0)                      /* PCI/NVMe: PF0 (the DSM) is handled by the caller */
			cb(pf, data);           /* PCI/NVMe: probe TSM for this PF before walking its VFs */

		/* walk virtual functions of each pf */
		for (int j = 0; j < pci_num_vf(pf); j++) {
			                                       /* PCI/NVMe: for each VF of this PF (e.g. NVMe SR-IOV VFs) */
			struct pci_dev *vf __free(pci_dev_put) =
				pci_get_domain_bus_and_slot(
					pci_domain_nr(pf->bus),
					pci_iov_virtfn_bus(pf, j),
					pci_iov_virtfn_devfn(pf, j));
			                                       /* PCI/NVMe: resolve VF BDF; NVMe VFs probed by nvme_probe() later depend on this */

			if (!vf)                /* PCI/NVMe: VF may be absent or not yet enumerated */
				continue;

			cb(vf, data);           /* PCI/NVMe: attach TSM context to the VF before NVMe driver sees it */
		}
	}

	/*
	 * Walk downstream devices, assumes that an upstream DSM is
	 * limited to downstream physical functions
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM && is_dsm(pdev))
		                                       /* PCI/NVMe: upstream port acting as DSM for downstream NVMe endpoints */
		pci_walk_bus(pdev->subordinate, cb, data);
		                                       /* PCI/NVMe: recursively set up TDISP for all devices behind the upstream port */
}

static void pci_tsm_walk_fns_reverse(struct pci_dev *pdev,
				     int (*cb)(struct pci_dev *pdev,
					       void *data),
				     void *data)
/* PCI/NVMe: reverse-order teardown; VFs removed before PFs, downstream before upstream */
{
	/* Reverse walk downstream devices */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM && is_dsm(pdev))
		                                       /* PCI/NVMe: match forward walk scope for upstream DSM */
		pci_walk_bus_reverse(pdev->subordinate, cb, data);
		                                       /* PCI/NVMe: tear down TSM contexts behind this upstream port first */

	/* Reverse walk subordinate physical functions */
	for (int i = 7; i >= 0; i--) {         /* PCI/NVMe: walk functions from highest to lowest */
		struct pci_dev *pf __free(pci_dev_put) = pci_get_slot(
			pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), i));
		                                       /* PCI/NVMe: sibling function i on the same slot */

		if (!pf)                        /* PCI/NVMe: skip absent functions */
			continue;

		/* reverse walk virtual functions */
		for (int j = pci_num_vf(pf) - 1; j >= 0; j--) {
			                                       /* PCI/NVMe: highest VF index first to avoid use-after-free during teardown */
			struct pci_dev *vf __free(pci_dev_put) =
				pci_get_domain_bus_and_slot(
					pci_domain_nr(pf->bus),
					pci_iov_virtfn_bus(pf, j),
					pci_iov_virtfn_devfn(pf, j));
			                                       /* PCI/NVMe: VF BDF for NVMe SR-IOV virtual function */

			if (!vf)                /* PCI/NVMe: VF not present */
				continue;
			cb(vf, data);           /* PCI/NVMe: remove TSM context from VF before the PF disappears */
		}

		/* on exit, caller will run @cb on function 0 */
		if (i > 0)                      /* PCI/NVMe: PF0 teardown is performed by the caller */
			cb(pf, data);           /* PCI/NVMe: remove TSM context from non-zero PFs */
	}
}

static void link_sysfs_disable(struct pci_dev *pdev)
/* PCI/NVMe: hide tsm/ and authenticated sysfs attributes when link TSM goes away */
{
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	                                       /* PCI/NVMe: remove authenticated attribute from sysfs */
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	                                       /* PCI/NVMe: remove connect/disconnect/bound/dsm attributes */
}

static void link_sysfs_enable(struct pci_dev *pdev)
/* PCI/NVMe: expose tsm/ sysfs attributes once a link TSM is available for this NVMe device */
{
	bool tee = has_tee(pdev);               /* PCI/NVMe: remember whether this function advertises TEE capability */

	pci_dbg(pdev, "%s Security Manager detected (%s%s%s)\n",
		pdev->tsm ? "Device" : "Platform TEE",
		pdev->ide_cap ? "IDE" : "", pdev->ide_cap && tee ? " " : "",
		tee ? "TEE" : "");
	                                       /* PCI/NVMe: log IDE/TEE detection; affects how NVMe DMA/IOMMU paths trust this link */

	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	                                       /* PCI/NVMe: publish authenticated attribute */
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	                                       /* PCI/NVMe: publish connect/disconnect/bound/dsm controls */
}

static int probe_fn(struct pci_dev *pdev, void *dsm)
/* PCI/NVMe: per-function TSM probe called for PFs/VFs under an established DSM */
{
	struct pci_dev *dsm_dev = dsm;          /* PCI/NVMe: the DSM (usually PF0) that will manage @pdev security */
	const struct pci_tsm_ops *ops = to_pci_tsm_ops(dsm_dev->tsm);
	                                       /* PCI/NVMe: TSM operations from the DSM's platform TSM */

	pdev->tsm = ops->probe(dsm_dev->tsm->tsm_dev, pdev);
	                                       /* PCI/NVMe: create a struct pci_tsm for this NVMe function */
	pci_dbg(pdev, "setup TSM context: DSM: %s status: %s\n",
		pci_name(dsm_dev), pdev->tsm ? "success" : "failed");
	                                       /* PCI/NVMe: diagnose TDISP setup before NVMe driver binds */
	if (pdev->tsm)                          /* PCI/NVMe: TSM context established */
		link_sysfs_enable(pdev);        /* PCI/NVMe: expose sysfs knobs for this NVMe function */
	return 0;                               /* PCI/NVMe: non-fatal; NVMe probe can continue without TSM */
}

static int pci_tsm_connect(struct pci_dev *pdev, struct tsm_dev *tsm_dev)
/* PCI/NVMe: user-space connect request to bind @pdev to a link TSM */
{
	int rc;
	struct pci_tsm_pf0 *tsm_pf0;
	const struct pci_tsm_ops *ops = tsm_dev->pci_ops;
	struct pci_tsm *pci_tsm __free(tsm_remove) = ops->probe(tsm_dev, pdev);
	                                       /* PCI/NVMe: allocate TSM context; freed on error paths via tsm_remove */

	/* connect() mutually exclusive with subfunction pci_tsm_init() */
	lockdep_assert_held_write(&pci_tsm_rwsem);
	                                       /* PCI/NVMe: caller must hold write lock to serialize with bus scan/NVMe probe */

	if (!pci_tsm)                           /* PCI/NVMe: platform TSM rejected this NVMe function */
		return -ENXIO;

	pdev->tsm = pci_tsm;                    /* PCI/NVMe: attach context to pci_dev before connect callback */
	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);    /* PCI/NVMe: get PF0 wrapper containing the per-DSM mutex */

	/* mutex_intr assumes connect() is always sysfs/user driven */
	ACQUIRE(mutex_intr, lock)(&tsm_pf0->lock);
	if ((rc = ACQUIRE_ERR(mutex_intr, &lock)))
		return rc;                      /* PCI/NVMe: could not lock DSM; TSM state remains via __free */

	rc = ops->connect(pdev);                /* PCI/NVMe: platform TSM establishes TDISP session for this NVMe device */
	if (rc)                                 /* PCI/NVMe: TDISP session setup failed */
		return rc;                      /* PCI/NVMe: pci_tsm still freed by __free on return */

	pdev->tsm = no_free_ptr(pci_tsm);       /* PCI/NVMe: ownership now held by pdev->tsm; cancel auto-free */

	/*
	 * Now that the DSM is established, probe() all the potential
	 * dependent functions. Failure to probe a function is not fatal
	 * to connect(), it just disables subsequent security operations
	 * for that function.
	 *
	 * Note this is done unconditionally, without regard to finding
	 * PCI_EXP_DEVCAP_TEE on the dependent function, for robustness. The DSM
	 * is the ultimate arbiter of security state relative to a given
	 * interface id, and if it says it can manage TDISP state of a function,
	 * let it.
	 */
	if (has_tee(pdev))                      /* PCI/NVMe: if the DSM device itself advertises TEE, propagate context */
		pci_tsm_walk_fns(pdev, probe_fn, pdev);
	                                       /* PCI/NVMe: set up TSM contexts for sibling PFs/VFs before NVMe driver binds them */
	return 0;
}

static ssize_t connect_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
/* PCI/NVMe: sysfs read of currently connected TSM for this NVMe device */
{
	struct pci_dev *pdev = to_pci_dev(dev); /* PCI/NVMe: convert sysfs device back to PCI device */
	struct tsm_dev *tsm_dev;
	int rc;

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))
		return rc;                      /* PCI/NVMe: lock acquisition failed, e.g. signal pending */

	if (!pdev->tsm)                         /* PCI/NVMe: no TSM context attached */
		return sysfs_emit(buf, "\n");   /* PCI/NVMe: show empty line when unconnected */

	tsm_dev = pdev->tsm->tsm_dev;           /* PCI/NVMe: platform TSM device managing this NVMe function */
	return sysfs_emit(buf, "%s\n", dev_name(&tsm_dev->dev));
	                                       /* PCI/NVMe: emit TSM device name visible to userspace */
}

/* Is @tsm_dev managing physical link / session properties... */
static bool is_link_tsm(struct tsm_dev *tsm_dev)
/* PCI/NVMe: determine whether this TSM handles link-level TDISP sessions */
{
	return tsm_dev && tsm_dev->pci_ops && tsm_dev->pci_ops->link_ops.probe;
	                                       /* PCI/NVMe: link TSMs can bind NVMe TDIs for passthrough */
}

/* ...or is @tsm_dev managing device security state ? */
static bool is_devsec_tsm(struct tsm_dev *tsm_dev)
/* PCI/NVMe: determine whether this TSM handles device-security locks */
{
	return tsm_dev && tsm_dev->pci_ops && tsm_dev->pci_ops->devsec_ops.lock;
	                                       /* PCI/NVMe: devsec TSMs are orthogonal to NVMe binding paths */
}

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
/* PCI/NVMe: sysfs write to connect an NVMe PCIe function to a link TSM */
{
	struct pci_dev *pdev = to_pci_dev(dev); /* PCI/NVMe: target NVMe/Switch/DSM PCI device */
	int rc, id;

	rc = sscanf(buf, "tsm%d\n", &id);       /* PCI/NVMe: parse "tsmN" from userspace */
	if (rc != 1)                            /* PCI/NVMe: malformed input */
		return -EINVAL;

	ACQUIRE(rwsem_write_kill, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &lock)))
		return rc;                      /* PCI/NVMe: could not take write lock */

	if (pdev->tsm)                          /* PCI/NVMe: already connected to a TSM */
		return -EBUSY;

	struct tsm_dev *tsm_dev __free(put_tsm_dev) = find_tsm_dev(id);
	                                       /* PCI/NVMe: lookup platform TSM by id; reference released on exit */
	if (!is_link_tsm(tsm_dev))              /* PCI/NVMe: only link TSMs can be connected via this sysfs path */
		return -ENXIO;

	rc = pci_tsm_connect(pdev, tsm_dev);    /* PCI/NVMe: perform TDISP connect for this NVMe device */
	if (rc)                                 /* PCI/NVMe: connect failed, tsm_dev reference dropped by __free */
		return rc;
	return len;                             /* PCI/NVMe: success, report all bytes consumed */
}
static DEVICE_ATTR_RW(connect);             /* PCI/NVMe: define tsm/connect sysfs attribute */

static int remove_fn(struct pci_dev *pdev, void *data)
/* PCI/NVMe: per-function TSM removal used during disconnect or teardown */
{
	tsm_remove(pdev->tsm);                  /* PCI/NVMe: tear down TSM context on this NVMe function */
	link_sysfs_disable(pdev);               /* PCI/NVMe: hide tsm/ sysfs knobs */
	return 0;
}

/*
 * Note, this helper only returns an error code and takes an argument for
 * compatibility with the pci_walk_bus() callback prototype. pci_tsm_unbind()
 * always succeeds.
 */
static int __pci_tsm_unbind(struct pci_dev *pdev, void *data)
/* PCI/NVMe: internal helper to unbind a TDI (NVMe function from a VM) */
{
	struct pci_tdi *tdi;
	struct pci_tsm_pf0 *tsm_pf0;

	lockdep_assert_held(&pci_tsm_rwsem);    /* PCI/NVMe: caller must hold rwsem, matching bind/unbind lifetimes */

	if (!pdev->tsm)                         /* PCI/NVMe: no TSM state, nothing to unbind */
		return 0;

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);    /* PCI/NVMe: DSM wrapper that serializes TDI state changes */
	guard(mutex)(&tsm_pf0->lock);           /* PCI/NVMe: protect TDI pointer while unbinding */

	tdi = pdev->tsm->tdi;                   /* PCI/NVMe: currently bound Trusted Device Interface */
	if (!tdi)                               /* PCI/NVMe: not currently bound to a VM */
		return 0;

	to_pci_tsm_ops(pdev->tsm)->unbind(tdi); /* PCI/NVMe: platform TSM releases the TDI and its DMA isolation */
	pdev->tsm->tdi = NULL;                  /* PCI/NVMe: clear binding before the NVMe function can be rebound */

	return 0;
}

void pci_tsm_unbind(struct pci_dev *pdev)
/* PCI/NVMe: exported helper called by VFIO before returning an NVMe device to the host */
{
	guard(rwsem_read)(&pci_tsm_rwsem);      /* PCI/NVMe: read lock permits concurrent NVMe probe and other unbinds */
	__pci_tsm_unbind(pdev, NULL);           /* PCI/NVMe: drop the TDI binding for this function */
}
EXPORT_SYMBOL_GPL(pci_tsm_unbind);          /* PCI/NVMe: used by VFIO PCI driver when releasing an NVMe passthrough device */

/**
 * pci_tsm_bind() - Bind @pdev as a TDI for @kvm
 * @pdev: PCI device function to bind
 * @kvm: Private memory attach context
 * @tdi_id: Identifier (virtual BDF) for the TDI as referenced by the TSM and DSM
 *
 * Returns 0 on success, or a negative error code on failure.
 *
 * Context: Caller is responsible for constraining the bind lifetime to the
 * registered state of the device. For example, pci_tsm_bind() /
 * pci_tsm_unbind() limited to the VFIO driver bound state of the device.
 */
int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u32 tdi_id)
/* PCI/NVMe: bind an NVMe PCIe function as a Trusted Device Interface for a KVM guest */
{
	struct pci_tsm_pf0 *tsm_pf0;
	struct pci_tdi *tdi;

	if (!kvm)                               /* PCI/NVMe: no guest context, cannot establish private memory binding */
		return -EINVAL;

	guard(rwsem_read)(&pci_tsm_rwsem);      /* PCI/NVMe: read lock; concurrent NVMe probe may attach TSM contexts */

	if (!pdev->tsm)                         /* PCI/NVMe: TSM must have been connected before binding */
		return -EINVAL;

	if (!is_link_tsm(pdev->tsm->tsm_dev))   /* PCI/NVMe: binding only supported by link TSMs */
		return -ENXIO;

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);    /* PCI/NVMe: DSM wrapper that owns the per-domain mutex */
	guard(mutex)(&tsm_pf0->lock);           /* PCI/NVMe: serialize bind/unbind for NVMe TDIs under this DSM */

	/* Resolve races to bind a TDI */
	if (pdev->tsm->tdi) {                   /* PCI/NVMe: already bound */
		if (pdev->tsm->tdi->kvm != kvm) /* PCI/NVMe: bound to a different VM */
			return -EBUSY;
		return 0;                       /* PCI/NVMe: already bound to the same VM */
	}

	tdi = to_pci_tsm_ops(pdev->tsm)->bind(pdev, kvm, tdi_id);
	                                       /* PCI/NVMe: platform TSM creates TDI, programming IOMMU/ATS/PRS for this NVMe function */
	if (IS_ERR(tdi))                        /* PCI/NVMe: TDISP bind failed, e.g. IDE key negotiation error */
		return PTR_ERR(tdi);

	pdev->tsm->tdi = tdi;                   /* PCI/NVMe: record active TDI used for guest TDISP requests */

	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_bind);            /* PCI/NVMe: consumed by VFIO PCI driver to assign an NVMe device to a VM */

/**
 * pci_tsm_guest_req() - helper to marshal guest requests to the TSM driver
 * @pdev: @pdev representing a bound tdi
 * @scope: caller asserts this passthrough request is limited to TDISP operations
 * @req_in: Input payload forwarded from the guest
 * @in_len: Length of @req_in
 * @req_out: Output payload buffer response to the guest
 * @out_len: Length of @req_out on input, bytes filled in @req_out on output
 * @tsm_code: Optional TSM arch specific result code for the guest TSM
 *
 * This is a common entry point for requests triggered by userspace KVM-exit
 * service handlers responding to TDI information or state change requests. The
 * scope parameter limits requests to TDISP state management, or limited debug.
 * This path is only suitable for commands and results that are the host kernel
 * has no use, the host is only facilitating guest to TSM communication.
 *
 * Returns 0 on success and -error on failure and positive "residue" on success
 * but @req_out is filled with less then @out_len, or @req_out is NULL and a
 * residue number of bytes were not consumed from @req_in.  On success or
 * failure @tsm_code may be populated with a TSM implementation specific result
 * code for the guest to consume.
 *
 * Context: Caller is responsible for calling this within the pci_tsm_bind()
 * state of the TDI.
 */
ssize_t pci_tsm_guest_req(struct pci_dev *pdev, enum pci_tsm_req_scope scope,
			  sockptr_t req_in, size_t in_len, sockptr_t req_out,
			  size_t out_len, u64 *tsm_code)
/* PCI/NVMe: forward a TDISP request from a guest VM to the platform TSM for this NVMe device */
{
	struct pci_tsm_pf0 *tsm_pf0;
	struct pci_tdi *tdi;
	int rc;

	/* Forbid requests that are not directly related to TDISP operations */
	if (scope > PCI_TSM_REQ_STATE_CHANGE)   /* PCI/NVMe: only allow TDISP info/state-change requests from the guest */
		return -EINVAL;

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))
		return rc;                      /* PCI/NVMe: lock interrupted while NVMe device state may be changing */

	if (!pdev->tsm)                         /* PCI/NVMe: device no longer attached to a TSM */
		return -ENXIO;

	if (!is_link_tsm(pdev->tsm->tsm_dev))   /* PCI/NVMe: only link TSMs support guest TDISP passthrough */
		return -ENXIO;

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);    /* PCI/NVMe: DSM wrapper for serialization */
	ACQUIRE(mutex_intr, ops_lock)(&tsm_pf0->lock);
	if ((rc = ACQUIRE_ERR(mutex_intr, &ops_lock)))
		return rc;                      /* PCI/NVMe: interrupted while another TDI operation holds the DSM mutex */

	tdi = pdev->tsm->tdi;                   /* PCI/NVMe: active TDI created by pci_tsm_bind() */
	if (!tdi)                               /* PCI/NVMe: not bound; guest request is invalid */
		return -ENXIO;
	return to_pci_tsm_ops(pdev->tsm)->guest_req(tdi, scope, req_in, in_len,
						    req_out, out_len, tsm_code);
	                                       /* PCI/NVMe: platform TSM handles the guest TDISP message and returns response */
}
EXPORT_SYMBOL_GPL(pci_tsm_guest_req);       /* PCI/NVMe: used by VFIO when an NVMe passthrough device triggers a TDISP exit */

static void pci_tsm_unbind_all(struct pci_dev *pdev)
/* PCI/NVMe: unbind all TDIs under @pdev before disconnecting the DSM */
{
	pci_tsm_walk_fns_reverse(pdev, __pci_tsm_unbind, NULL);
	                                       /* PCI/NVMe: unbind VFs first, then PFs, then downstream devices */
	__pci_tsm_unbind(pdev, NULL);           /* PCI/NVMe: finally unbind the DSM device itself */
}

static void __pci_tsm_disconnect(struct pci_dev *pdev)
/* PCI/NVMe: internal disconnect; tears down TDISP session and all dependent contexts */
{
	struct pci_tsm_pf0 *tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);
	                                       /* PCI/NVMe: DSM wrapper for this NVMe domain */
	const struct pci_tsm_ops *ops = to_pci_tsm_ops(pdev->tsm);
	                                       /* PCI/NVMe: platform TSM operation vector */

	/* disconnect() mutually exclusive with subfunction pci_tsm_init() */
	lockdep_assert_held_write(&pci_tsm_rwsem);
	                                       /* PCI/NVMe: write lock prevents racing with NVMe bus scan TSM init */

	pci_tsm_unbind_all(pdev);               /* PCI/NVMe: release VM bindings before closing the TDISP session */

	/*
	 * disconnect() is uninterruptible as it may be called for device
	 * teardown
	 */
	guard(mutex)(&tsm_pf0->lock);           /* PCI/NVMe: hold DSM mutex while disconnecting */
	pci_tsm_walk_fns_reverse(pdev, remove_fn, NULL);
	                                       /* PCI/NVMe: remove TSM contexts from dependent functions */
	ops->disconnect(pdev);                  /* PCI/NVMe: platform TSM closes the TDISP session on this NVMe device */
}

static void pci_tsm_disconnect(struct pci_dev *pdev)
/* PCI/NVMe: disconnect this NVMe PCIe function from its TSM and release the context */
{
	__pci_tsm_disconnect(pdev);             /* PCI/NVMe: tear down dependent contexts and TDISP session */
	tsm_remove(pdev->tsm);                  /* PCI/NVMe: free the per-function TSM context */
}

static ssize_t disconnect_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
/* PCI/NVMe: sysfs write to disconnect an NVMe device from its TSM */
{
	struct pci_dev *pdev = to_pci_dev(dev); /* PCI/NVMe: target NVMe device */
	struct tsm_dev *tsm_dev;
	int rc;

	ACQUIRE(rwsem_write_kill, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &lock)))
		return rc;                      /* PCI/NVMe: could not acquire write lock */

	if (!pdev->tsm)                         /* PCI/NVMe: nothing connected to disconnect */
		return -ENXIO;

	tsm_dev = pdev->tsm->tsm_dev;           /* PCI/NVMe: currently attached TSM */
	if (!sysfs_streq(buf, dev_name(&tsm_dev->dev)))
		return -EINVAL;                 /* PCI/NVMe: user must specify the exact TSM device name */

	pci_tsm_disconnect(pdev);               /* PCI/NVMe: perform disconnect and cleanup */
	return len;                             /* PCI/NVMe: success */
}
static DEVICE_ATTR_WO(disconnect);          /* PCI/NVMe: define tsm/disconnect sysfs attribute */

static ssize_t bound_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
/* PCI/NVMe: sysfs read showing which TSM an NVMe function is bound to as a TDI */
{
	struct pci_dev *pdev = to_pci_dev(dev); /* PCI/NVMe: NVMe/Switch device */
	struct pci_tsm_pf0 *tsm_pf0;
	struct pci_tsm *tsm;
	int rc;

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))
		return rc;                      /* PCI/NVMe: lock acquisition interrupted */

	tsm = pdev->tsm;                        /* PCI/NVMe: snapshot current TSM pointer */
	if (!tsm)                               /* PCI/NVMe: not connected */
		return sysfs_emit(buf, "\n");
	tsm_pf0 = to_pci_tsm_pf0(tsm);          /* PCI/NVMe: DSM wrapper for mutex */

	ACQUIRE(mutex_intr, ops_lock)(&tsm_pf0->lock);
	if ((rc = ACQUIRE_ERR(mutex_intr, &ops_lock)))
		return rc;                      /* PCI/NVMe: interrupted while locking DSM mutex */

	if (!tsm->tdi)                          /* PCI/NVMe: connected but not yet bound to a VM */
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%s\n", dev_name(&tsm->tsm_dev->dev));
	                                       /* PCI/NVMe: emit the TSM device name when bound */
}
static DEVICE_ATTR_RO(bound);               /* PCI/NVMe: define tsm/bound sysfs attribute */

static ssize_t dsm_show(struct device *dev, struct device_attribute *attr,
			char *buf)
/* PCI/NVMe: sysfs read showing the DSM PCI device that manages this function */
{
	struct pci_dev *pdev = to_pci_dev(dev); /* PCI/NVMe: NVMe device */
	struct pci_tsm *tsm;
	int rc;

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))
		return rc;                      /* PCI/NVMe: lock interrupted */

	tsm = pdev->tsm;                        /* PCI/NVMe: current TSM context */
	if (!tsm)                               /* PCI/NVMe: not connected */
		return sysfs_emit(buf, "\n");

	return sysfs_emit(buf, "%s\n", pci_name(tsm->dsm_dev));
	                                       /* PCI/NVMe: print DSM BDF (e.g. 0000:00:00.0) for this NVMe domain */
}
static DEVICE_ATTR_RO(dsm);                 /* PCI/NVMe: define tsm/dsm sysfs attribute */

/* The 'authenticated' attribute is exclusive to the presence of a 'link' TSM */
static bool pci_tsm_link_group_visible(struct kobject *kobj)
/* PCI/NVMe: decide whether authenticated/ link TSM sysfs attributes should appear */
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));
	                                       /* PCI/NVMe: device represented by this sysfs kobject */

	if (!pci_tsm_link_count)                /* PCI/NVMe: no link TSMs registered at all */
		return false;

	if (!pci_is_pcie(pdev))                 /* PCI/NVMe: attribute only meaningful for PCIe devices such as NVMe SSDs */
		return false;

	if (is_pci_tsm_pf0(pdev))               /* PCI/NVMe: PF0 (DSM) always shows the link group */
		return true;

	/*
	 * Show 'authenticated' and other attributes for the managed
	 * sub-functions of a DSM.
	 */
	if (pdev->tsm)                          /* PCI/NVMe: function is managed by a DSM */
		return true;

	return false;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_tsm_link);
/* PCI/NVMe: register the visibility helper for the authenticated attribute group */

/*
 * 'link' and 'devsec' TSMs share the same 'tsm/' sysfs group, so the TSM type
 * specific attributes need individual visibility checks.
 */
static umode_t pci_tsm_attr_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
/* PCI/NVMe: per-attribute visibility for tsm/connect, disconnect, bound, dsm */
{
	if (pci_tsm_link_group_visible(kobj)) { /* PCI/NVMe: only relevant when a link TSM is present */
		struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));
	                                       /* PCI/NVMe: NVMe device owning this sysfs node */

		if (attr == &dev_attr_bound.attr) {
			if (is_pci_tsm_pf0(pdev) && has_tee(pdev))
				return attr->mode; /* PCI/NVMe: bound visible on PF0 DSM with TEE */
			if (pdev->tsm && has_tee(pdev->tsm->dsm_dev))
				return attr->mode; /* PCI/NVMe: bound visible on managed functions whose DSM has TEE */
		}

		if (attr == &dev_attr_dsm.attr) {
			if (is_pci_tsm_pf0(pdev))
				return attr->mode; /* PCI/NVMe: dsm visible on the DSM device */
			if (pdev->tsm && has_tee(pdev->tsm->dsm_dev))
				return attr->mode; /* PCI/NVMe: dsm visible on functions whose DSM has TEE */
		}

		if (attr == &dev_attr_connect.attr ||
		    attr == &dev_attr_disconnect.attr) {
			if (is_pci_tsm_pf0(pdev))
				return attr->mode; /* PCI/NVMe: connect/disconnect only on DSM device */
		}
	}

	return 0;                               /* PCI/NVMe: attribute hidden for this NVMe device */
}

static bool pci_tsm_group_visible(struct kobject *kobj)
/* PCI/NVMe: visibility wrapper for the tsm/ group */
{
	return pci_tsm_link_group_visible(kobj); /* PCI/NVMe: same rule as authenticated group */
}
DEFINE_SYSFS_GROUP_VISIBLE(pci_tsm);        /* PCI/NVMe: register tsm/ group visibility helper */

static struct attribute *pci_tsm_attrs[] = {
	&dev_attr_connect.attr,                 /* PCI/NVMe: connect sysfs file */
	&dev_attr_disconnect.attr,              /* PCI/NVMe: disconnect sysfs file */
	&dev_attr_bound.attr,                   /* PCI/NVMe: bound sysfs file */
	&dev_attr_dsm.attr,                     /* PCI/NVMe: dsm sysfs file */
	NULL
};

const struct attribute_group pci_tsm_attr_group = {
	.name = "tsm",                          /* PCI/NVMe: creates /sys/bus/pci/devices/.../tsm/ for NVMe functions */
	.attrs = pci_tsm_attrs,                 /* PCI/NVMe: connect/disconnect/bound/dsm attributes */
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),
	                                       /* PCI/NVMe: dynamic visibility based on link TSM presence */
};

static ssize_t authenticated_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
/* PCI/NVMe: sysfs read of TDISP authenticated state, mirrors connect state */
{
	/*
	 * When the SPDM session established via TSM the 'authenticated' state
	 * of the device is identical to the connect state.
	 */
	return connect_show(dev, attr, buf);    /* PCI/NVMe: SPDM/TDISP authentication is equivalent to being connected */
}
static DEVICE_ATTR_RO(authenticated);       /* PCI/NVMe: define authenticated sysfs attribute */

static struct attribute *pci_tsm_auth_attrs[] = {
	&dev_attr_authenticated.attr,            /* PCI/NVMe: authenticated attribute for TDISP link state */
	NULL
};

const struct attribute_group pci_tsm_auth_attr_group = {
	.attrs = pci_tsm_auth_attrs,            /* PCI/NVMe: only the authenticated attribute */
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm_link),
	                                       /* PCI/NVMe: visible only when a link TSM is registered */
};

/*
 * Retrieve physical function0 device whether it has TEE capability or not
 */
static struct pci_dev *pf0_dev_get(struct pci_dev *pdev)
/* PCI/NVMe: return PF0 of the device, used to locate the DSM for an NVMe VF/PF */
{
	struct pci_dev *pf_dev = pci_physfn(pdev);
	                                       /* PCI/NVMe: physical function underlying this device (may be itself) */

	if (PCI_FUNC(pf_dev->devfn) == 0)       /* PCI/NVMe: already PF0 */
		return pci_dev_get(pf_dev);     /* PCI/NVMe: take a reference before returning */

	return pci_get_slot(pf_dev->bus,
			    pf_dev->devfn - PCI_FUNC(pf_dev->devfn));
	                                       /* PCI/NVMe: compute and look up PF0 by zeroing the function number */
}

/*
 * Find the PCI Device instance that serves as the Device Security Manager (DSM)
 * for @pdev. Note that no additional reference is held for the resulting device
 * because that resulting object always has a registered lifetime
 * greater-than-or-equal to that of the @pdev argument. This is by virtue of
 * @pdev being a descendant of, or identical to, the returned DSM device.
 */
static struct pci_dev *find_dsm_dev(struct pci_dev *pdev)
/* PCI/NVMe: discover which device in the NVMe topology acts as the DSM */
{
	struct device *grandparent;
	struct pci_dev *uport;

	if (is_pci_tsm_pf0(pdev))               /* PCI/NVMe: @pdev itself is the PF0 DSM candidate */
		return pdev;

	struct pci_dev *pf0 __free(pci_dev_put) = pf0_dev_get(pdev);
	                                       /* PCI/NVMe: get PF0 of this NVMe function */
	if (!pf0)                               /* PCI/NVMe: PF0 not found */
		return NULL;

	if (is_dsm(pf0))                        /* PCI/NVMe: PF0 is the DSM */
		return pf0;

	/*
	 * For cases where a switch may be hosting TDISP services on behalf of
	 * downstream devices, check the first upstream port relative to this
	 * endpoint.
	 */
	if (!pdev->dev.parent)                  /* PCI/NVMe: cannot walk topology without parent */
		return NULL;
	grandparent = pdev->dev.parent->parent; /* PCI/NVMe: upstream port parent of the endpoint's parent */
	if (!grandparent)                       /* PCI/NVMe: no upstream port */
		return NULL;
	if (!dev_is_pci(grandparent))           /* PCI/NVMe: grandparent is not a PCI device */
		return NULL;
	uport = to_pci_dev(grandparent);        /* PCI/NVMe: candidate upstream port */
	if (!pci_is_pcie(uport) ||
	    pci_pcie_type(uport) != PCI_EXP_TYPE_UPSTREAM)
		return NULL;                    /* PCI/NVMe: not a PCIe upstream port */

	if (is_dsm(uport))                      /* PCI/NVMe: upstream port hosts the DSM for downstream NVMe devices */
		return uport;
	return NULL;                            /* PCI/NVMe: no DSM found in topology */
}

/**
 * pci_tsm_tdi_constructor() - base 'struct pci_tdi' initialization for link TSMs
 * @pdev: PCI device function representing the TDI
 * @tdi: context to initialize
 * @kvm: Private memory attach context
 * @tdi_id: Identifier (virtual BDF) for the TDI as referenced by the TSM and DSM
 */
void pci_tsm_tdi_constructor(struct pci_dev *pdev, struct pci_tdi *tdi,
			     struct kvm *kvm, u32 tdi_id)
/* PCI/NVMe: initialize the common part of a Trusted Device Interface for an NVMe function */
{
	tdi->pdev = pdev;                       /* PCI/NVMe: back pointer to the NVMe PCIe function */
	tdi->kvm = kvm;                         /* PCI/NVMe: guest KVM context for private memory/DMA isolation */
	tdi->tdi_id = tdi_id;                   /* PCI/NVMe: virtual BDF used by TSM/DSM to identify this TDI */
}
EXPORT_SYMBOL_GPL(pci_tsm_tdi_constructor); /* PCI/NVMe: used by platform TSM modules when creating an NVMe TDI */

/**
 * pci_tsm_link_constructor() - base 'struct pci_tsm' initialization for link TSMs
 * @pdev: The PCI device
 * @tsm: context to initialize
 * @tsm_dev: Platform TEE Security Manager, initiator of security operations
 */
int pci_tsm_link_constructor(struct pci_dev *pdev, struct pci_tsm *tsm,
			     struct tsm_dev *tsm_dev)
/* PCI/NVMe: initialize common TSM context for a link TSM attached to an NVMe device */
{
	if (!is_link_tsm(tsm_dev))              /* PCI/NVMe: ensure caller passed a link TSM */
		return -EINVAL;

	tsm->dsm_dev = find_dsm_dev(pdev);      /* PCI/NVMe: discover DSM in NVMe topology */
	if (!tsm->dsm_dev) {                    /* PCI/NVMe: no DSM available, cannot manage this function */
		pci_warn(pdev, "failed to find Device Security Manager\n");
		return -ENXIO;
	}
	tsm->pdev = pdev;                       /* PCI/NVMe: record the NVMe PCIe function */
	tsm->tsm_dev = tsm_dev;                 /* PCI/NVMe: record the platform TSM */

	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_link_constructor); /* PCI/NVMe: used by platform TSM modules initializing per-NVMe TSM state */

/**
 * pci_tsm_pf0_constructor() - common 'struct pci_tsm_pf0' (DSM) initialization
 * @pdev: Physical Function 0 PCI device (as indicated by is_pci_tsm_pf0())
 * @tsm: context to initialize
 * @tsm_dev: Platform TEE Security Manager, initiator of security operations
 */
int pci_tsm_pf0_constructor(struct pci_dev *pdev, struct pci_tsm_pf0 *tsm,
			    struct tsm_dev *tsm_dev)
/* PCI/NVMe: initialize the DSM wrapper (PF0) that owns the DOE mailbox for TDISP */
{
	mutex_init(&tsm->lock);                 /* PCI/NVMe: per-DSM mutex serializes bind/unbind/disconnect for NVMe TDIs */
	tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					   PCI_DOE_FEATURE_CMA);
	                                       /* PCI/NVMe: locate CMA (Component Measurement and Authentication) DOE mailbox */
	if (!tsm->doe_mb) {                     /* PCI/NVMe: CMA mailbox required for TDISP SPDM session */
		pci_warn(pdev, "TSM init failure, no CMA mailbox\n");
		return -ENODEV;
	}

	return pci_tsm_link_constructor(pdev, &tsm->base_tsm, tsm_dev);
	                                       /* PCI/NVMe: initialize base TSM context pointing to this DSM */
}
EXPORT_SYMBOL_GPL(pci_tsm_pf0_constructor); /* PCI/NVMe: used by platform TSM modules when creating a DSM for NVMe domain */

void pci_tsm_pf0_destructor(struct pci_tsm_pf0 *pf0_tsm)
/* PCI/NVMe: release resources allocated by pci_tsm_pf0_constructor */
{
	mutex_destroy(&pf0_tsm->lock);          /* PCI/NVMe: destroy DSM mutex after all NVMe TDIs are unbound */
}
EXPORT_SYMBOL_GPL(pci_tsm_pf0_destructor);  /* PCI/NVMe: used by platform TSM modules destroying a DSM */

int pci_tsm_register(struct tsm_dev *tsm_dev)
/* PCI/NVMe: register a platform TSM; enables TDISP sysfs for matching NVMe devices */
{
	struct pci_dev *pdev = NULL;

	if (!tsm_dev)                           /* PCI/NVMe: NULL TSM device is invalid */
		return -EINVAL;

	/* The TSM device must only implement one of link_ops or devsec_ops */
	if (!is_link_tsm(tsm_dev) && !is_devsec_tsm(tsm_dev))
		return -EINVAL;                 /* PCI/NVMe: must implement at least one PCI TSM operation set */

	if (is_link_tsm(tsm_dev) && is_devsec_tsm(tsm_dev))
		return -EINVAL;                 /* PCI/NVMe: must not implement both; keeps NVMe bind/devsec paths distinct */

	guard(rwsem_write)(&pci_tsm_rwsem);     /* PCI/NVMe: protect TSM list and sysfs updates from concurrent NVMe probe */

	/* On first enable, update sysfs groups */
	if (is_link_tsm(tsm_dev) && pci_tsm_link_count++ == 0) {
		                                       /* PCI/NVMe: first link TSM registration */
		for_each_pci_dev(pdev)
			if (is_pci_tsm_pf0(pdev))
				link_sysfs_enable(pdev);
			                       /* PCI/NVMe: expose tsm/ on existing PF0 DSMs so users can connect NVMe devices */
	} else if (is_devsec_tsm(tsm_dev)) {    /* PCI/NVMe: devsec TSM registration */
		pci_tsm_devsec_count++;         /* PCI/NVMe: increment devsec TSM count */
	}

	return 0;
}

static void pci_tsm_fn_exit(struct pci_dev *pdev)
/* PCI/NVMe: per-function teardown helper used at NVMe device removal or TSM unregister */
{
	__pci_tsm_unbind(pdev, NULL);           /* PCI/NVMe: release any active VM binding for this function */
	tsm_remove(pdev->tsm);                  /* PCI/NVMe: free the TSM context attached to this NVMe device */
}

/**
 * __pci_tsm_destroy() - destroy the TSM context for @pdev
 * @pdev: device to cleanup
 * @tsm_dev: the TSM device being removed, or NULL if @pdev is being removed.
 *
 * At device removal or TSM unregistration all established context
 * with the TSM is torn down. Additionally, if there are no more TSMs
 * registered, the PCI tsm/ sysfs attributes are hidden.
 */
static void __pci_tsm_destroy(struct pci_dev *pdev, struct tsm_dev *tsm_dev)
/* PCI/NVMe: internal destroy called when an NVMe device or its TSM disappears */
{
	struct pci_tsm *tsm = pdev->tsm;        /* PCI/NVMe: snapshot current TSM context */

	lockdep_assert_held_write(&pci_tsm_rwsem);
	                                       /* PCI/NVMe: caller holds write lock during removal/unregister */

	/*
	 * First, handle the TSM removal case to shutdown @pdev sysfs, this is
	 * skipped if the device itself is being removed since sysfs goes away
	 * naturally at that point
	 */
	if (is_link_tsm(tsm_dev) && is_pci_tsm_pf0(pdev) && !pci_tsm_link_count)
		                                       /* PCI/NVMe: last link TSM gone and @pdev is a DSM */
		link_sysfs_disable(pdev);       /* PCI/NVMe: hide tsm/ authenticated attributes */

	/* Nothing else to do if this device never attached to the departing TSM */
	if (!tsm)                               /* PCI/NVMe: no TSM context on this NVMe device */
		return;

	/* Now lookup the tsm_dev to destroy TSM context */
	if (!tsm_dev)                           /* PCI/NVMe: device removal path, destroy its attached TSM */
		tsm_dev = tsm->tsm_dev;
	else if (tsm_dev != tsm->tsm_dev)       /* PCI/NVMe: this device is attached to a different TSM */
		return;

	if (is_link_tsm(tsm_dev) && is_pci_tsm_pf0(pdev))
		pci_tsm_disconnect(pdev);       /* PCI/NVMe: DSM device disconnects, tearing down dependent NVMe contexts */
	else
		pci_tsm_fn_exit(pdev);          /* PCI/NVMe: non-DSM function releases its own TSM context */
}

void pci_tsm_destroy(struct pci_dev *pdev)
/* PCI/NVMe: public destroy entry for NVMe device removal (e.g. hot-unplug, unbind) */
{
	guard(rwsem_write)(&pci_tsm_rwsem);     /* PCI/NVMe: serialize with connect/bind and concurrent NVMe probe */
	__pci_tsm_destroy(pdev, NULL);          /* PCI/NVMe: remove the TSM context attached to this device */
}

void pci_tsm_init(struct pci_dev *pdev)
/* PCI/NVMe: late TSM attach for NVMe functions discovered after a link TSM registered */
{
	guard(rwsem_read)(&pci_tsm_rwsem);      /* PCI/NVMe: read lock allows concurrent sysfs show and NVMe probe */

	/*
	 * Subfunctions are either probed synchronous with connect() or later
	 * when either the SR-IOV configuration is changed, or, unlikely,
	 * connect() raced initial bus scanning.
	 */
	if (pdev->tsm)                          /* PCI/NVMe: already has a TSM context (e.g. from connect()) */
		return;

	if (pci_tsm_link_count) {               /* PCI/NVMe: at least one link TSM is registered */
		struct pci_dev *dsm = find_dsm_dev(pdev);
		                                       /* PCI/NVMe: find DSM for this newly enumerated NVMe function */

		if (!dsm)                       /* PCI/NVMe: no DSM in topology, cannot manage */
			return;

		/*
		 * The only path to init a Device Security Manager capable
		 * device is via connect().
		 */
		if (!dsm->tsm)                  /* PCI/NVMe: DSM itself is not connected yet, defer */
			return;

		probe_fn(pdev, dsm);            /* PCI/NVMe: attach TSM context to this NVMe function before driver bind */
	}
}

void pci_tsm_unregister(struct tsm_dev *tsm_dev)
/* PCI/NVMe: unregister a platform TSM and tear down contexts on all NVMe devices it managed */
{
	struct pci_dev *pdev = NULL;

	guard(rwsem_write)(&pci_tsm_rwsem);     /* PCI/NVMe: exclusive lock while destroying per-device TSM state */
	if (is_link_tsm(tsm_dev))
		pci_tsm_link_count--;           /* PCI/NVMe: decrement link TSM count before destroying contexts */
	if (is_devsec_tsm(tsm_dev))
		pci_tsm_devsec_count--;         /* PCI/NVMe: decrement devsec TSM count */
	for_each_pci_dev_reverse(pdev)
		__pci_tsm_destroy(pdev, tsm_dev);
	                                       /* PCI/NVMe: walk all PCI devices in reverse, destroying contexts for this TSM */
}

int pci_tsm_doe_transfer(struct pci_dev *pdev, u8 type, const void *req,
			 size_t req_sz, void *resp, size_t resp_sz)
/* PCI/NVMe: issue a Data Object Exchange transfer on the DSM's CMA mailbox */
{
	struct pci_tsm_pf0 *tsm;

	if (!pdev->tsm || !is_pci_tsm_pf0(pdev))
		return -ENXIO;                  /* PCI/NVMe: only the DSM device can issue DOE transfers */

	tsm = to_pci_tsm_pf0(pdev->tsm);        /* PCI/NVMe: get PF0 wrapper with DOE mailbox */
	if (!tsm->doe_mb)                       /* PCI/NVMe: CMA mailbox missing, cannot perform TDISP protocol exchange */
		return -ENXIO;

	return pci_doe(tsm->doe_mb, PCI_VENDOR_ID_PCI_SIG, type, req, req_sz,
		       resp, resp_sz);           /* PCI/NVMe: submit SPDM/TDISP message and wait for response from the NVMe device */
}
EXPORT_SYMBOL_GPL(pci_tsm_doe_transfer);    /* PCI/NVMe: used by platform TSM modules to negotiate TDISP with the NVMe DSM */
