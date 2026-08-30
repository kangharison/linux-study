// SPDX-License-Identifier: GPL-2.0
/*
 * Support routines for initializing a PCI subsystem
 *
 * Extruded from code written by
 *      Dave Rusling (david.rusling@reo.mts.dec.com)
 *      David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *
 * Nov 2000, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     PCI-PCI bridges cleanup, sorted resource allocation.
 * Feb 2002, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     Converted to allocation in 3 passes, which gives
 *	     tighter packing. Prefetchable range support.
 */

#include <linux/align.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/bitops.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/bug.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/init.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/kernel.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/minmax.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/module.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/pci.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/errno.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/ioport.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/cache.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/limits.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/sizes.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/slab.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include <linux/acpi.h> /* NVMe: include kernel header for PCI/NVMe setup */
#include "pci.h" /* NVMe: include internal PCI header */

#define PCI_RES_TYPE_MASK /* NVMe: define constant/macro for PCI resource setup */\
	(IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH | /* NVMe: combine IORESOURCE flag bits */\
	 IORESOURCE_MEM_64) /* NVMe: NVMe PCIe host setup operation */

unsigned int pci_flags; /* NVMe: unsigned integer variable */
EXPORT_SYMBOL_GPL(pci_flags); /* NVMe: export symbol for module use */

struct pci_dev_resource { /* NVMe: start of structure definition */
	struct list_head list; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	resource_size_t start; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t end; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t add_size; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t min_align; /* NVMe: resource size/alignment value used for BAR/window sizing */
	unsigned long flags; /* NVMe: unsigned integer variable */
}; /* NVMe: end NVMe PCIe host resource structure/union/enum definition */

static void pci_dev_res_free_list(struct list_head *head) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *dev_res, *tmp; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry_safe(dev_res, tmp, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		list_del(&dev_res->list); /* NVMe: remove entry from resource list */
		kfree(dev_res); /* NVMe: free allocated tracker object */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * pci_dev_res_add_to_list() - Add a new resource tracker to the list
 * @head:	Head of the list
 * @dev:	Device to which the resource belongs
 * @res:	Resource to be tracked
 * @add_size:	Additional size to be optionally added to the resource
 * @min_align:	Minimum memory window alignment
 */
int pci_dev_res_add_to_list(struct list_head *head, struct pci_dev *dev, /* NVMe: function declaration/definition */
			    struct resource *res, resource_size_t add_size, /* NVMe: NVMe PCIe host resource structure member/variable */
			    resource_size_t min_align) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *tmp; /* NVMe: NVMe PCIe host resource structure member/variable */

	tmp = kzalloc_obj(*tmp); /* NVMe: allocate resource tracker object */
	if (!tmp) /* NVMe: conditional check */
		return -ENOMEM; /* NVMe: return error code */

	tmp->res = res; /* NVMe: access PCI device/resource member */
	tmp->dev = dev; /* NVMe: access PCI device/resource member */
	tmp->start = res->start; /* NVMe: access PCI device/resource member */
	tmp->end = res->end; /* NVMe: access PCI device/resource member */
	tmp->flags = res->flags; /* NVMe: access PCI device/resource member */
	tmp->add_size = add_size; /* NVMe: access PCI device/resource member */
	tmp->min_align = min_align; /* NVMe: access PCI device/resource member */

	list_add(&tmp->list, head); /* NVMe: insert entry into resource list */

	return 0; /* NVMe: success */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_dev_res_remove_from_list(struct list_head *head, /* NVMe: function declaration/definition */
					 struct resource *res) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *dev_res, *tmp; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry_safe(dev_res, tmp, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		if (dev_res->res == res) { /* NVMe: conditional check */
			list_del(&dev_res->list); /* NVMe: remove entry from resource list */
			kfree(dev_res); /* NVMe: free allocated tracker object */
			break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static struct pci_dev_resource *res_to_dev_res(struct list_head *head, /* NVMe: function declaration/definition */
					       struct resource *res) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		if (dev_res->res == res) /* NVMe: conditional check */
			return dev_res; /* NVMe: return result */
	} /* NVMe: end NVMe PCIe host setup code block */

	return NULL; /* NVMe: return NULL pointer */
} /* NVMe: end NVMe PCIe host setup code block */

static resource_size_t get_res_add_size(struct list_head *head, /* NVMe: function declaration/definition */
					struct resource *res) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	dev_res = res_to_dev_res(head, res); /* NVMe: find tracker entry for a resource */
	return dev_res ? dev_res->add_size : 0; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_dev_res_restore(struct pci_dev_resource *dev_res) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res = dev_res->res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev = dev_res->dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	int idx = pci_resource_num(dev, res); /* NVMe: integer variable, often index or return code */
	const char *res_name = pci_resource_name(dev, idx); /* NVMe: resource name string for log messages */

	if (WARN_ON_ONCE(resource_assigned(res))) /* NVMe: conditional check */
		return; /* NVMe: early return */

	res->start = dev_res->start; /* NVMe: access PCI device/resource member */
	res->end = dev_res->end; /* NVMe: access PCI device/resource member */
	res->flags = dev_res->flags; /* NVMe: access PCI device/resource member */

	pci_dbg(dev, "%s %pR: resource restored\n", res_name, res); /* NVMe: emit debug message */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * Helper function for sizing routines.  Assigned resources have non-NULL
 * parent resource.
 *
 * Return first unassigned resource of the correct type.  If there is none,
 * return first assigned resource of the correct type.  If none of the
 * above, return NULL.
 *
 * Returning an assigned resource of the correct type allows the caller to
 * distinguish between already assigned and no resource of the correct type.
 */
static struct resource *find_bus_resource_of_type(struct pci_bus *bus, /* NVMe: function declaration/definition */
						  unsigned long type_mask, /* NVMe: unsigned integer variable */
						  unsigned long type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *r, *r_assigned = NULL; /* NVMe: NVMe PCIe host resource structure member/variable */

	pci_bus_for_each_resource(bus, r) { /* NVMe: iterate over bus-level bridge resources */
		if (!r || r == &ioport_resource || r == &iomem_resource) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if ((r->flags & type_mask) != type) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (!resource_assigned(r)) /* NVMe: conditional check */
			return r; /* NVMe: return result */
		if (!r_assigned) /* NVMe: conditional check */
			r_assigned = r; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */
	return r_assigned; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * pbus_select_window_for_type - Select bridge window for a resource type
 * @bus: PCI bus
 * @type: Resource type (resource flags can be passed as is)
 *
 * Select the bridge window based on a resource @type.
 *
 * For memory resources, the selection is done as follows:
 *
 * Any non-prefetchable resource is put into the non-prefetchable window.
 *
 * If there is no prefetchable MMIO window, put all memory resources into the
 * non-prefetchable window.
 *
 * If there's a 64-bit prefetchable MMIO window, put all 64-bit prefetchable
 * resources into it and place 32-bit prefetchable memory into the
 * non-prefetchable window.
 *
 * Otherwise, put all prefetchable resources into the prefetchable window.
 *
 * Return: the bridge window resource or NULL if no bridge window is found.
 */
static struct resource *pbus_select_window_for_type(struct pci_bus *bus, /* NVMe: function declaration/definition */
						    unsigned long type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int iores_type = type & IORESOURCE_TYPE_BITS;	/* w/o 64bit & pref */
	struct resource *mmio, *mmio_pref, *win; /* NVMe: NVMe PCIe host resource structure member/variable */

	type &= PCI_RES_TYPE_MASK;			/* with 64bit & pref */

	if ((iores_type != IORESOURCE_IO) && (iores_type != IORESOURCE_MEM)) /* NVMe: conditional check */
		return NULL; /* NVMe: return NULL pointer */

	if (pci_is_root_bus(bus)) { /* NVMe: conditional check */
		win = find_bus_resource_of_type(bus, type, type); /* NVMe: find bus resource matching type mask */
		if (win) /* NVMe: conditional check */
			return win; /* NVMe: return result */

		type &= ~IORESOURCE_MEM_64; /* NVMe: clear resource flag bits */
		win = find_bus_resource_of_type(bus, type, type); /* NVMe: find bus resource matching type mask */
		if (win) /* NVMe: conditional check */
			return win; /* NVMe: return result */

		type &= ~IORESOURCE_PREFETCH; /* NVMe: clear resource flag bits */
		return find_bus_resource_of_type(bus, type, type); /* NVMe: return result */
	} /* NVMe: end NVMe PCIe host setup code block */

	switch (iores_type) { /* NVMe: dispatch based on header or resource type */
	case IORESOURCE_IO: /* NVMe: case label */
		win = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_IO_WINDOW); /* NVMe: set value for NVMe PCIe host resource setup */
		if (win && (win->flags & IORESOURCE_IO)) /* NVMe: conditional check */
			return win; /* NVMe: return result */
		return NULL; /* NVMe: return NULL pointer */

	case IORESOURCE_MEM: /* NVMe: case label */
		mmio = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_MEM_WINDOW); /* NVMe: set value for NVMe PCIe host resource setup */
		mmio_pref = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_PREF_MEM_WINDOW); /* NVMe: set value for NVMe PCIe host resource setup */

		if (mmio && !(mmio->flags & IORESOURCE_MEM)) /* NVMe: conditional check */
			mmio = NULL; /* NVMe: set value for NVMe PCIe host resource setup */
		if (mmio_pref && !(mmio_pref->flags & IORESOURCE_MEM)) /* NVMe: conditional check */
			mmio_pref = NULL; /* NVMe: set value for NVMe PCIe host resource setup */

		if (!(type & IORESOURCE_PREFETCH) || !mmio_pref) /* NVMe: conditional check */
			return mmio; /* NVMe: return result */

		if ((type & IORESOURCE_MEM_64) || /* NVMe: conditional check */
		    !(mmio_pref->flags & IORESOURCE_MEM_64)) /* NVMe: access PCI device/resource member */
			return mmio_pref; /* NVMe: return result */

		return mmio; /* NVMe: return result */
	default: /* NVMe: cleanup/error label */
		return NULL; /* NVMe: return NULL pointer */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * pbus_select_window - Select bridge window for a resource
 * @bus: PCI bus
 * @res: Resource
 *
 * Select the bridge window for @res. If the resource is already assigned,
 * return the current bridge window.
 *
 * For memory resources, the selection is done as follows:
 *
 * Any non-prefetchable resource is put into the non-prefetchable window.
 *
 * If there is no prefetchable MMIO window, put all memory resources into the
 * non-prefetchable window.
 *
 * If there's a 64-bit prefetchable MMIO window, put all 64-bit prefetchable
 * resources into it and place 32-bit prefetchable memory into the
 * non-prefetchable window.
 *
 * Otherwise, put all prefetchable resources into the prefetchable window.
 *
 * Return: the bridge window resource or NULL if no bridge window is found.
 */
struct resource *pbus_select_window(struct pci_bus *bus, /* NVMe: function declaration/definition */
				    const struct resource *res) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	if (resource_assigned(res)) /* NVMe: conditional check */
		return res->parent; /* NVMe: return result */

	return pbus_select_window_for_type(bus, res->flags); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

static bool pdev_resources_assignable(struct pci_dev *dev) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	u16 class = dev->class >> 8, command; /* NVMe: 16-bit config/register value */

	/* Don't touch classless devices or host bridges or IOAPICs */
	if (class == PCI_CLASS_NOT_DEFINED || class == PCI_CLASS_BRIDGE_HOST) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	/* Don't touch IOAPIC devices already enabled by firmware */
	if (class == PCI_CLASS_SYSTEM_PIC) { /* NVMe: conditional check */
		pci_read_config_word(dev, PCI_COMMAND, &command); /* NVMe: read PCI config space register */
		if (command & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) /* NVMe: conditional check */
			return false; /* NVMe: return false */
	} /* NVMe: end NVMe PCIe host setup code block */

	return true; /* NVMe: return true */
} /* NVMe: end NVMe PCIe host setup code block */

static bool pdev_resource_assignable(struct pci_dev *dev, struct resource *res) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int idx = pci_resource_num(dev, res); /* NVMe: integer variable, often index or return code */

	if (!res->flags) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	if (pci_resource_is_bridge_win(idx) && res->flags & IORESOURCE_DISABLED) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	return true; /* NVMe: return true */
} /* NVMe: end NVMe PCIe host setup code block */

static bool pdev_resource_should_fit(struct pci_dev *dev, struct resource *res) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	if (resource_assigned(res)) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	return pdev_resource_assignable(dev, res); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/* Sort resources by alignment */
static void pdev_sort_resources(struct pci_dev *dev, struct list_head *head) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *r; /* NVMe: NVMe PCIe host resource structure member/variable */
	int i; /* NVMe: integer variable, often index or return code */

	if (!pdev_resources_assignable(dev)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	pci_dev_for_each_resource(dev, r, i) { /* NVMe: iterate over device BARs and bridge windows */
		const char *r_name = pci_resource_name(dev, i); /* NVMe: resource name string for log messages */
		struct pci_dev_resource *dev_res, *tmp; /* NVMe: NVMe PCIe host resource structure member/variable */
		resource_size_t r_align; /* NVMe: resource size/alignment value used for BAR/window sizing */
		struct list_head *n; /* NVMe: NVMe PCIe host resource structure member/variable */

		if (!pdev_resource_should_fit(dev, r)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		r_align = pci_resource_alignment(dev, r); /* NVMe: get required alignment for this BAR/window */
		if (!r_align) { /* NVMe: conditional check */
			pci_warn(dev, "%s %pR: alignment must not be zero\n", /* NVMe: emit warning message about resource issue */
				 r_name, r); /* NVMe: NVMe PCIe host resource setup statement */
			continue; /* NVMe: next loop iteration */
		} /* NVMe: end NVMe PCIe host setup code block */

		tmp = kzalloc_obj(*tmp); /* NVMe: allocate resource tracker object */
		if (!tmp) /* NVMe: conditional check */
			panic("%s: kzalloc() failed!\n", __func__); /* NVMe: fatal allocation failure path */
		tmp->res = r; /* NVMe: access PCI device/resource member */
		tmp->dev = dev; /* NVMe: access PCI device/resource member */
		tmp->start = r->start; /* NVMe: access PCI device/resource member */
		tmp->end = r->end; /* NVMe: access PCI device/resource member */
		tmp->flags = r->flags; /* NVMe: access PCI device/resource member */

		/* Fallback is smallest one or list is empty */
		n = head; /* NVMe: set value for NVMe PCIe host resource setup */
		list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
			resource_size_t align; /* NVMe: resource size/alignment value used for BAR/window sizing */

			align = pci_resource_alignment(dev_res->dev, /* NVMe: get required alignment for this BAR/window */
							 dev_res->res); /* NVMe: access PCI device/resource member */

			if (r_align > align) { /* NVMe: conditional check */
				n = &dev_res->list; /* NVMe: access PCI device/resource member */
				break; /* NVMe: exit switch/loop */
			} /* NVMe: end NVMe PCIe host setup code block */
		} /* NVMe: end NVMe PCIe host setup code block */
		/* Insert it just before n */
		list_add_tail(&tmp->list, n); /* NVMe: insert entry into resource list */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

bool pci_resource_is_optional(const struct pci_dev *dev, int resno) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	const struct resource *res = pci_resource_n(dev, resno); /* NVMe: set value for NVMe PCIe host resource setup */

	if (pci_resource_is_iov(resno)) /* NVMe: conditional check */
		return true; /* NVMe: return true */
	if (resno == PCI_ROM_RESOURCE && !(res->flags & IORESOURCE_ROM_ENABLE)) /* NVMe: conditional check */
		return true; /* NVMe: return true */
	if (pci_resource_is_bridge_win(resno) && !resource_size(res)) /* NVMe: conditional check */
		return true; /* NVMe: return true */

	return false; /* NVMe: return false */
} /* NVMe: end NVMe PCIe host setup code block */

static void reset_resource(struct pci_dev *dev, struct resource *res) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int idx = pci_resource_num(dev, res); /* NVMe: integer variable, often index or return code */
	const char *res_name = pci_resource_name(dev, idx); /* NVMe: resource name string for log messages */

	if (pci_resource_is_bridge_win(idx)) { /* NVMe: conditional check */
		res->flags |= IORESOURCE_UNSET; /* NVMe: set resource flag bits */
		return; /* NVMe: early return */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_dbg(dev, "%s %pR: resetting resource\n", res_name, res); /* NVMe: emit debug message */

	res->start = 0; /* NVMe: access PCI device/resource member */
	res->end = 0; /* NVMe: access PCI device/resource member */
	res->flags = 0; /* NVMe: access PCI device/resource member */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * reassign_resources_sorted() - Satisfy any additional resource requests
 *
 * @realloc_head:	Head of the list tracking requests requiring
 *			additional resources
 * @head:		Head of the list tracking requests with allocated
 *			resources
 *
 * Walk through each element of the realloc_head and try to procure additional
 * resources for the element, provided the element is in the head list.
 */
static void reassign_resources_sorted(struct list_head *realloc_head, /* NVMe: function declaration/definition */
				      struct list_head *head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *add_res, *tmp; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	const char *res_name; /* NVMe: resource name string for log messages */
	resource_size_t add_size, align; /* NVMe: resource size/alignment value used for BAR/window sizing */
	int idx; /* NVMe: integer variable, often index or return code */

	list_for_each_entry_safe(add_res, tmp, realloc_head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = add_res->res; /* NVMe: access PCI device/resource member */
		dev = add_res->dev; /* NVMe: access PCI device/resource member */
		idx = pci_resource_num(dev, res); /* NVMe: convert resource pointer to BAR/bridge index */

		/* Skip this resource if not found in head list */
		if (!res_to_dev_res(head, res)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		/*
		 * Skip resource that failed the earlier assignment and is
		 * not optional as it would just fail again.
		 */
		if (!resource_assigned(res) && resource_size(res) && /* NVMe: conditional check */
		    !pci_resource_is_optional(dev, idx)) /* NVMe: NVMe PCIe host setup function call/header */
			goto out; /* NVMe: jump to cleanup/error label */

		res_name = pci_resource_name(dev, idx); /* NVMe: resource name string for log messages */
		add_size = add_res->add_size; /* NVMe: access PCI device/resource member */
		align = add_res->min_align; /* NVMe: access PCI device/resource member */
		if (!resource_assigned(res)) { /* NVMe: conditional check */
			resource_set_range(res, align, /* NVMe: set resource start/end and alignment */
					   resource_size(res) + add_size); /* NVMe: get current size of a resource */
			if (pci_assign_resource(dev, idx)) { /* NVMe: conditional check */
				pci_dbg(dev, /* NVMe: emit debug message */
					"%s %pR: ignoring failure in optional allocation\n", /* NVMe: continue NVMe PCIe host setup argument list */
					res_name, res); /* NVMe: NVMe PCIe host resource setup statement */
			} /* NVMe: end NVMe PCIe host setup code block */
		} else if (add_size > 0 || !IS_ALIGNED(res->start, align)) { /* NVMe: check address alignment */
			res->flags |= add_res->flags & /* NVMe: set resource flag bits */
				 (IORESOURCE_STARTALIGN|IORESOURCE_SIZEALIGN); /* NVMe: combine IORESOURCE flag bits */
			if (pci_reassign_resource(dev, idx, add_size, align)) /* NVMe: conditional check */
				pci_info(dev, "%s %pR: failed to add optional %llx\n", /* NVMe: emit informational message */
					 res_name, res, /* NVMe: continue NVMe PCIe host setup argument list */
					 (unsigned long long) add_size); /* NVMe: NVMe PCIe host setup function call */
		} /* NVMe: end NVMe PCIe host setup code block */
out: /* NVMe: cleanup/error label */
		list_del(&add_res->list); /* NVMe: remove entry from resource list */
		kfree(add_res); /* NVMe: free allocated tracker object */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * assign_requested_resources_sorted() - Satisfy resource requests
 *
 * @head:	Head of the list tracking requests for resources
 * @fail_head:	Head of the list tracking requests that could not be
 *		allocated
 * @optional:	Assign also optional resources
 *
 * Satisfy resource requests of each element in the list.  Add requests that
 * could not be satisfied to the failed_list.
 */
static void assign_requested_resources_sorted(struct list_head *head, /* NVMe: function declaration/definition */
					      struct list_head *fail_head, /* NVMe: NVMe PCIe host resource structure member/variable */
					      bool optional) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	bool optional_res; /* NVMe: boolean flag */
	int idx; /* NVMe: integer variable, often index or return code */

	list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = dev_res->res; /* NVMe: access PCI device/resource member */
		dev = dev_res->dev; /* NVMe: access PCI device/resource member */
		idx = pci_resource_num(dev, res); /* NVMe: convert resource pointer to BAR/bridge index */
		optional_res = pci_resource_is_optional(dev, idx); /* NVMe: set value for NVMe PCIe host resource setup */

		if (!resource_size(res)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (!optional && optional_res) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (pci_assign_resource(dev, idx)) { /* NVMe: conditional check */
			if (fail_head) { /* NVMe: conditional check */
				pci_dev_res_add_to_list(fail_head, dev, res, /* NVMe: track resource for allocation or reallocation */
							0 /* don't care */,
							0 /* don't care */);
			} /* NVMe: end NVMe PCIe host setup code block */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static unsigned long pci_fail_res_type_mask(struct list_head *fail_head) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *fail_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	unsigned long mask = 0; /* NVMe: unsigned integer variable */

	/* Check failed type */
	list_for_each_entry(fail_res, fail_head, list) /* NVMe: iterate over a linked list of resources/devices */
		mask |= fail_res->flags; /* NVMe: set resource flag bits */

	/*
	 * One pref failed resource will set IORESOURCE_MEM, as we can
	 * allocate pref in non-pref range.  Will release all assigned
	 * non-pref sibling resources according to that bit.
	 */
	return mask & (IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

static bool pci_need_to_release(unsigned long mask, struct resource *res) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	if (res->flags & IORESOURCE_IO) /* NVMe: conditional check */
		return !!(mask & IORESOURCE_IO); /* NVMe: return result */

	/* Check pref at first */
	if (res->flags & IORESOURCE_PREFETCH) { /* NVMe: conditional check */
		if (mask & IORESOURCE_PREFETCH) /* NVMe: conditional check */
			return true; /* NVMe: return true */
		/* Count pref if its parent is non-pref */
		else if ((mask & IORESOURCE_MEM) && /* NVMe: additional conditional branch */
			 !(res->parent->flags & IORESOURCE_PREFETCH)) /* NVMe: access PCI device/resource member */
			return true; /* NVMe: return true */
		else /* NVMe: alternative code path */
			return false; /* NVMe: return false */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (res->flags & IORESOURCE_MEM) /* NVMe: conditional check */
		return !!(mask & IORESOURCE_MEM); /* NVMe: return result */

	return false;	/* Should not get here */
} /* NVMe: end NVMe PCIe host setup code block */

/* Return: @true if assignment of a required resource failed. */
static bool pci_required_resource_failed(struct list_head *fail_head, /* NVMe: function declaration/definition */
					 unsigned long type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *fail_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	type &= PCI_RES_TYPE_MASK; /* NVMe: clear resource flag bits */

	list_for_each_entry(fail_res, fail_head, list) { /* NVMe: iterate over a linked list of resources/devices */
		int idx = pci_resource_num(fail_res->dev, fail_res->res); /* NVMe: integer variable, often index or return code */

		if (type && (fail_res->flags & PCI_RES_TYPE_MASK) != type) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (!pci_resource_is_optional(fail_res->dev, idx)) /* NVMe: conditional check */
			return true; /* NVMe: return true */
	} /* NVMe: end NVMe PCIe host setup code block */
	return false; /* NVMe: return false */
} /* NVMe: end NVMe PCIe host setup code block */

static void __assign_resources_sorted(struct list_head *head, /* NVMe: function declaration/definition */
				      struct list_head *realloc_head, /* NVMe: NVMe PCIe host resource structure member/variable */
				      struct list_head *fail_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	/*
	 * Should not assign requested resources at first.  They could be
	 * adjacent, so later reassign can not reallocate them one by one in
	 * parent resource window.
	 *
	 * Try to assign required and any optional resources at beginning
	 * (add_size included). If all required resources were successfully
	 * assigned, get out early. If could not do that, we still try to
	 * assign required at first, then try to reassign some optional
	 * resources.
	 *
	 * Separate three resource type checking if we need to release
	 * assigned resource after requested + add_size try.
	 *
	 *	1. If IO port assignment fails, will release assigned IO
	 *	   port.
	 *	2. If pref MMIO assignment fails, release assigned pref
	 *	   MMIO.  If assigned pref MMIO's parent is non-pref MMIO
	 *	   and non-pref MMIO assignment fails, will release that
	 *	   assigned pref MMIO.
	 *	3. If non-pref MMIO assignment fails or pref MMIO
	 *	   assignment fails, will release assigned non-pref MMIO.
	 */
	LIST_HEAD(save_head); /* NVMe: NVMe PCIe host setup function call */
	LIST_HEAD(local_fail_head); /* NVMe: NVMe PCIe host setup function call */
	LIST_HEAD(dummy_head); /* NVMe: NVMe PCIe host setup function call */
	struct pci_dev_resource *save_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev_resource *dev_res, *tmp_res, *dev_res2, *addsize_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	unsigned long fail_type; /* NVMe: unsigned integer variable */
	resource_size_t align; /* NVMe: resource size/alignment value used for BAR/window sizing */

	if (!realloc_head) /* NVMe: conditional check */
		realloc_head = &dummy_head; /* NVMe: set value for NVMe PCIe host resource setup */

	/* Check if optional add_size is there */
	if (list_empty(realloc_head)) /* NVMe: conditional check */
		goto assign; /* NVMe: jump to cleanup/error label */

	/* Save original start, end, flags etc at first */
	list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		if (pci_dev_res_add_to_list(&save_head, dev_res->dev, /* NVMe: conditional check */
					    dev_res->res, 0, 0)) { /* NVMe: access PCI device/resource member */
			pci_dev_res_free_list(&save_head); /* NVMe: free all resource tracker nodes */
			goto assign; /* NVMe: jump to cleanup/error label */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	/* Update res in head list with add_size in realloc_head list */
	list_for_each_entry_safe(dev_res, tmp_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = dev_res->res; /* NVMe: access PCI device/resource member */

		addsize_res = res_to_dev_res(realloc_head, res); /* NVMe: find tracker entry for a resource */
		if (!addsize_res) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		res->end += addsize_res->add_size; /* NVMe: access PCI device/resource member */
		/*
		 * There are two kinds of additional resources in the list:
		 * 1. bridge resource  -- IORESOURCE_STARTALIGN
		 * 2. SR-IOV resource  -- IORESOURCE_SIZEALIGN
		 * Here just fix the additional alignment for bridge
		 */
		if (!(res->flags & IORESOURCE_STARTALIGN)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (addsize_res->min_align <= res->start) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */
		/*
		 * The "head" list is sorted by alignment so resources with
		 * bigger alignment will be assigned first.  After we
		 * change the alignment of a dev_res in "head" list, we
		 * need to reorder the list by alignment to make it
		 * consistent.
		 */
		resource_set_range(res, addsize_res->min_align, /* NVMe: set resource start/end and alignment */
				   resource_size(res)); /* NVMe: get current size of a resource */

		list_for_each_entry(dev_res2, head, list) { /* NVMe: iterate over a linked list of resources/devices */
			align = pci_resource_alignment(dev_res2->dev, /* NVMe: get required alignment for this BAR/window */
						       dev_res2->res); /* NVMe: access PCI device/resource member */
			if (addsize_res->min_align > align) { /* NVMe: conditional check */
				list_move_tail(&dev_res->list, &dev_res2->list); /* NVMe: reorder list node by alignment */
				break; /* NVMe: exit switch/loop */
			} /* NVMe: end NVMe PCIe host setup code block */
		} /* NVMe: end NVMe PCIe host setup code block */

	} /* NVMe: end NVMe PCIe host setup code block */

assign: /* NVMe: cleanup/error label */
	assign_requested_resources_sorted(head, &local_fail_head, true); /* NVMe: assign required BARs in alignment order */

	/* All non-optional resources assigned? */
	if (list_empty(&local_fail_head)) { /* NVMe: conditional check */
		/* Remove head list from realloc_head list */
		list_for_each_entry(dev_res, head, list) /* NVMe: iterate over a linked list of resources/devices */
			pci_dev_res_remove_from_list(realloc_head, /* NVMe: remove resource tracker from list */
						     dev_res->res); /* NVMe: access PCI device/resource member */
		pci_dev_res_free_list(&save_head); /* NVMe: free all resource tracker nodes */
		goto out; /* NVMe: jump to cleanup/error label */
	} /* NVMe: end NVMe PCIe host setup code block */

	/* Without realloc_head and only optional fails, nothing more to do. */
	if (!pci_required_resource_failed(&local_fail_head, 0) && /* NVMe: conditional check */
	    list_empty(realloc_head)) { /* NVMe: NVMe PCIe host setup function call/header */
		list_for_each_entry(save_res, &save_head, list) { /* NVMe: iterate over a linked list of resources/devices */
			struct resource *res = save_res->res; /* NVMe: NVMe PCIe host resource structure member/variable */

			if (resource_assigned(res)) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */

			pci_dev_res_restore(save_res); /* NVMe: restore original resource start/end/flags */
		} /* NVMe: end NVMe PCIe host setup code block */
		pci_dev_res_free_list(&local_fail_head); /* NVMe: free all resource tracker nodes */
		pci_dev_res_free_list(&save_head); /* NVMe: free all resource tracker nodes */
		goto out; /* NVMe: jump to cleanup/error label */
	} /* NVMe: end NVMe PCIe host setup code block */

	/* Check failed type */
	fail_type = pci_fail_res_type_mask(&local_fail_head); /* NVMe: compute bitmask of failed resource types */
	/* Remove not need to be released assigned res from head list etc */
	list_for_each_entry_safe(dev_res, tmp_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = dev_res->res; /* NVMe: access PCI device/resource member */

		if (resource_assigned(res) && /* NVMe: conditional check */
		    !pci_need_to_release(fail_type, res)) { /* NVMe: decide if assigned sibling must be released */
			/* Remove it from realloc_head list */
			pci_dev_res_remove_from_list(realloc_head, res); /* NVMe: remove resource tracker from list */
			pci_dev_res_remove_from_list(&save_head, res); /* NVMe: remove resource tracker from list */
			list_del(&dev_res->list); /* NVMe: remove entry from resource list */
			kfree(dev_res); /* NVMe: free allocated tracker object */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_dev_res_free_list(&local_fail_head); /* NVMe: free all resource tracker nodes */
	/* Release assigned resource */
	list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = dev_res->res; /* NVMe: access PCI device/resource member */
		dev = dev_res->dev; /* NVMe: access PCI device/resource member */

		pci_release_resource(dev, pci_resource_num(dev, res)); /* NVMe: release a BAR or bridge window back to parent */
		pci_dev_res_restore(dev_res); /* NVMe: restore original resource start/end/flags */
	} /* NVMe: end NVMe PCIe host setup code block */
	/* Restore start/end/flags from saved list */
	list_for_each_entry(save_res, &save_head, list) /* NVMe: iterate over a linked list of resources/devices */
		pci_dev_res_restore(save_res); /* NVMe: restore original resource start/end/flags */
	pci_dev_res_free_list(&save_head); /* NVMe: free all resource tracker nodes */

	/* Satisfy the must-have resource requests */
	assign_requested_resources_sorted(head, NULL, false); /* NVMe: assign required BARs in alignment order */

	/* Try to satisfy any additional optional resource requests */
	if (!list_empty(realloc_head)) /* NVMe: conditional check */
		reassign_resources_sorted(realloc_head, head); /* NVMe: try to satisfy additional optional resource requests */

out: /* NVMe: cleanup/error label */
	/* Reset any failed resource, cannot use fail_head as it can be NULL. */
	list_for_each_entry(dev_res, head, list) { /* NVMe: iterate over a linked list of resources/devices */
		res = dev_res->res; /* NVMe: access PCI device/resource member */
		dev = dev_res->dev; /* NVMe: access PCI device/resource member */

		if (resource_assigned(res)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (fail_head) { /* NVMe: conditional check */
			pci_dev_res_add_to_list(fail_head, dev, res, /* NVMe: track resource for allocation or reallocation */
						0 /* don't care */,
						0 /* don't care */);
		} /* NVMe: end NVMe PCIe host setup code block */

		reset_resource(dev, res); /* NVMe: reset BAR/start/end flags to unassigned state */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_dev_res_free_list(head); /* NVMe: free all resource tracker nodes */
} /* NVMe: end NVMe PCIe host setup code block */

static void pdev_assign_resources_sorted(struct pci_dev *dev, /* NVMe: function declaration/definition */
					 struct list_head *add_head, /* NVMe: NVMe PCIe host resource structure member/variable */
					 struct list_head *fail_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	LIST_HEAD(head); /* NVMe: NVMe PCIe host setup function call */

	pdev_sort_resources(dev, &head); /* NVMe: sort device BARs by descending alignment */
	__assign_resources_sorted(&head, add_head, fail_head); /* NVMe: core resource assignment with retry logic */

} /* NVMe: end NVMe PCIe host setup code block */

static void pbus_assign_resources_sorted(const struct pci_bus *bus, /* NVMe: function declaration/definition */
					 struct list_head *realloc_head, /* NVMe: NVMe PCIe host resource structure member/variable */
					 struct list_head *fail_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	LIST_HEAD(head); /* NVMe: NVMe PCIe host setup function call */

	list_for_each_entry(dev, &bus->devices, bus_list) /* NVMe: iterate over a linked list of resources/devices */
		pdev_sort_resources(dev, &head); /* NVMe: sort device BARs by descending alignment */

	__assign_resources_sorted(&head, realloc_head, fail_head); /* NVMe: core resource assignment with retry logic */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * Initialize bridges with base/limit values we have collected.  PCI-to-PCI
 * Bridge Architecture Specification rev. 1.1 (1998) requires that if there
 * are no I/O ports or memory behind the bridge, the corresponding range
 * must be turned off by writing base value greater than limit to the
 * bridge's base/limit registers.
 *
 * Note: care must be taken when updating I/O base/limit registers of
 * bridges which support 32-bit I/O.  This update requires two config space
 * writes, so it's quite possible that an I/O window of the bridge will
 * have some undesirable address (e.g. 0) after the first write.  Ditto
 * 64-bit prefetchable MMIO.
 */
static void pci_setup_bridge_io(struct pci_dev *bridge) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	const char *res_name; /* NVMe: resource name string for log messages */
	struct pci_bus_region region; /* NVMe: NVMe PCIe host resource structure member/variable */
	unsigned long io_mask; /* NVMe: unsigned integer variable */
	u8 io_base_lo, io_limit_lo; /* NVMe: 8-bit config/register value */
	u16 l; /* NVMe: 16-bit config/register value */
	u32 io_upper16; /* NVMe: 32-bit config/register or BAR value */

	io_mask = PCI_IO_RANGE_MASK; /* NVMe: set value for NVMe PCIe host resource setup */
	if (bridge->io_window_1k) /* NVMe: conditional check */
		io_mask = PCI_IO_1K_RANGE_MASK; /* NVMe: set value for NVMe PCIe host resource setup */

	/* Set up the top and bottom of the PCI I/O segment for this bus */
	res = &bridge->resource[PCI_BRIDGE_IO_WINDOW]; /* NVMe: access PCI device/resource member */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_IO_WINDOW); /* NVMe: resource name string for log messages */
	pcibios_resource_to_bus(bridge->bus, &region, res); /* NVMe: translate CPU address to bus-relative address */
	if (resource_assigned(res) && res->flags & IORESOURCE_IO) { /* NVMe: conditional check */
		pci_read_config_word(bridge, PCI_IO_BASE, &l); /* NVMe: read PCI config space register */
		io_base_lo = (region.start >> 8) & io_mask; /* NVMe: shift bits to extract field */
		io_limit_lo = (region.end >> 8) & io_mask; /* NVMe: shift bits to extract field */
		l = ((u16) io_limit_lo << 8) | io_base_lo; /* NVMe: shift bits to extract field */
		/* Set up upper 16 bits of I/O base/limit */
		io_upper16 = (region.end & 0xffff0000) | (region.start >> 16); /* NVMe: shift bits to extract field */
		pci_info(bridge, "  %s %pR\n", res_name, res); /* NVMe: emit informational message */
	} else { /* NVMe: NVMe PCIe host setup operation */
		/* Clear upper 16 bits of I/O base/limit */
		io_upper16 = 0; /* NVMe: set value for NVMe PCIe host resource setup */
		l = 0x00f0; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */
	/* Temporarily disable the I/O range before updating PCI_IO_BASE */
	pci_write_config_dword(bridge, PCI_IO_BASE_UPPER16, 0x0000ffff); /* NVMe: write PCI config space register */
	/* Update lower 16 bits of I/O base/limit */
	pci_write_config_word(bridge, PCI_IO_BASE, l); /* NVMe: write PCI config space register */
	/* Update upper 16 bits of I/O base/limit */
	pci_write_config_dword(bridge, PCI_IO_BASE_UPPER16, io_upper16); /* NVMe: write PCI config space register */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_setup_bridge_mmio(struct pci_dev *bridge) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	const char *res_name; /* NVMe: resource name string for log messages */
	struct pci_bus_region region; /* NVMe: NVMe PCIe host resource structure member/variable */
	u32 l; /* NVMe: 32-bit config/register or BAR value */

	/* Set up the top and bottom of the PCI Memory segment for this bus */
	res = &bridge->resource[PCI_BRIDGE_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_MEM_WINDOW); /* NVMe: resource name string for log messages */
	pcibios_resource_to_bus(bridge->bus, &region, res); /* NVMe: translate CPU address to bus-relative address */
	if (resource_assigned(res) && res->flags & IORESOURCE_MEM) { /* NVMe: conditional check */
		l = (region.start >> 16) & 0xfff0; /* NVMe: shift bits to extract field */
		l |= region.end & 0xfff00000; /* NVMe: set resource flag bits */
		pci_info(bridge, "  %s %pR\n", res_name, res); /* NVMe: emit informational message */
	} else { /* NVMe: NVMe PCIe host setup operation */
		l = 0x0000fff0; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */
	pci_write_config_dword(bridge, PCI_MEMORY_BASE, l); /* NVMe: write PCI config space register */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_setup_bridge_mmio_pref(struct pci_dev *bridge) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	const char *res_name; /* NVMe: resource name string for log messages */
	struct pci_bus_region region; /* NVMe: NVMe PCIe host resource structure member/variable */
	u32 l, bu, lu; /* NVMe: 32-bit config/register or BAR value */

	/*
	 * Clear out the upper 32 bits of PREF limit.  If
	 * PCI_PREF_BASE_UPPER32 was non-zero, this temporarily disables
	 * PREF range, which is ok.
	 */
	pci_write_config_dword(bridge, PCI_PREF_LIMIT_UPPER32, 0); /* NVMe: write PCI config space register */

	/* Set up PREF base/limit */
	bu = lu = 0; /* NVMe: set value for NVMe PCIe host resource setup */
	res = &bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_PREF_MEM_WINDOW); /* NVMe: resource name string for log messages */
	pcibios_resource_to_bus(bridge->bus, &region, res); /* NVMe: translate CPU address to bus-relative address */
	if (resource_assigned(res) && res->flags & IORESOURCE_PREFETCH) { /* NVMe: conditional check */
		l = (region.start >> 16) & 0xfff0; /* NVMe: shift bits to extract field */
		l |= region.end & 0xfff00000; /* NVMe: set resource flag bits */
		if (res->flags & IORESOURCE_MEM_64) { /* NVMe: conditional check */
			bu = upper_32_bits(region.start); /* NVMe: extract upper 32 bits of 64-bit address */
			lu = upper_32_bits(region.end); /* NVMe: extract upper 32 bits of 64-bit address */
		} /* NVMe: end NVMe PCIe host setup code block */
		pci_info(bridge, "  %s %pR\n", res_name, res); /* NVMe: emit informational message */
	} else { /* NVMe: NVMe PCIe host setup operation */
		l = 0x0000fff0; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */
	pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE, l); /* NVMe: write PCI config space register */

	/* Set the upper 32 bits of PREF base & limit */
	pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32, bu); /* NVMe: write PCI config space register */
	pci_write_config_dword(bridge, PCI_PREF_LIMIT_UPPER32, lu); /* NVMe: write PCI config space register */
} /* NVMe: end NVMe PCIe host setup code block */

static void __pci_setup_bridge(struct pci_bus *bus, unsigned long type) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *bridge = bus->self; /* NVMe: NVMe PCIe host resource structure member/variable */

	pci_info(bridge, "PCI bridge to %pR\n", &bus->busn_res); /* NVMe: emit informational message */

	if (type & IORESOURCE_IO) /* NVMe: conditional check */
		pci_setup_bridge_io(bridge); /* NVMe: program I/O bridge base/limit registers */

	if (type & IORESOURCE_MEM) /* NVMe: conditional check */
		pci_setup_bridge_mmio(bridge); /* NVMe: program non-prefetchable memory bridge registers */

	if (type & IORESOURCE_PREFETCH) /* NVMe: conditional check */
		pci_setup_bridge_mmio_pref(bridge); /* NVMe: program prefetchable memory bridge registers */

	pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, bus->bridge_ctl); /* NVMe: write PCI config space register */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_setup_one_bridge_window(struct pci_dev *bridge, int resno) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	switch (resno) { /* NVMe: dispatch based on header or resource type */
	case PCI_BRIDGE_IO_WINDOW: /* NVMe: case label */
		pci_setup_bridge_io(bridge); /* NVMe: program I/O bridge base/limit registers */
		break; /* NVMe: exit switch/loop */
	case PCI_BRIDGE_MEM_WINDOW: /* NVMe: case label */
		pci_setup_bridge_mmio(bridge); /* NVMe: program non-prefetchable memory bridge registers */
		break; /* NVMe: exit switch/loop */
	case PCI_BRIDGE_PREF_MEM_WINDOW: /* NVMe: case label */
		pci_setup_bridge_mmio_pref(bridge); /* NVMe: program prefetchable memory bridge registers */
		break; /* NVMe: exit switch/loop */
	default: /* NVMe: cleanup/error label */
		return; /* NVMe: early return */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void __weak pcibios_setup_bridge(struct pci_bus *bus, unsigned long type) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_setup_bridge(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	unsigned long type = IORESOURCE_IO | IORESOURCE_MEM | /* NVMe: unsigned integer variable */
				  IORESOURCE_PREFETCH; /* NVMe: NVMe PCIe host resource setup statement */

	pcibios_setup_bridge(bus, type); /* NVMe: platform-specific bridge setup hook */
	__pci_setup_bridge(bus, type); /* NVMe: write bridge base/limit registers to hardware */
} /* NVMe: end NVMe PCIe host setup code block */


int pci_claim_bridge_resource(struct pci_dev *bridge, int i) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int ret = -EINVAL; /* NVMe: integer variable, often index or return code */

	if (!pci_resource_is_bridge_win(i)) /* NVMe: conditional check */
		return 0; /* NVMe: success */

	if (pci_claim_resource(bridge, i) == 0) /* NVMe: conditional check */
		return 0;	/* Claimed the window */

	if ((bridge->class >> 8) != PCI_CLASS_BRIDGE_PCI) /* NVMe: conditional check */
		return 0; /* NVMe: success */

	if (i > PCI_BRIDGE_PREF_MEM_WINDOW) /* NVMe: conditional check */
		return -EINVAL; /* NVMe: return error code */

	/* Try to clip the resource and claim the smaller window */
	if (pci_bus_clip_resource(bridge, i)) /* NVMe: conditional check */
		ret = pci_claim_resource(bridge, i); /* NVMe: claim a BAR from the parent bridge window */

	pci_setup_one_bridge_window(bridge, i); /* NVMe: program one bridge window register set */

	return ret; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * Check whether the bridge supports optional I/O and prefetchable memory
 * ranges.  If not, the respective base/limit registers must be read-only
 * and read as 0.
 */
static void pci_bridge_check_ranges(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *bridge = bus->self; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *b_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	b_res = &bridge->resource[PCI_BRIDGE_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
	b_res->flags |= IORESOURCE_MEM; /* NVMe: set resource flag bits */

	if (bridge->io_window) { /* NVMe: conditional check */
		b_res = &bridge->resource[PCI_BRIDGE_IO_WINDOW]; /* NVMe: access PCI device/resource member */
		b_res->flags |= IORESOURCE_IO; /* NVMe: set resource flag bits */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (bridge->pref_window) { /* NVMe: conditional check */
		b_res = &bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
		b_res->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH; /* NVMe: set resource flag bits */
		if (bridge->pref_64_window) { /* NVMe: conditional check */
			b_res->flags |= IORESOURCE_MEM_64 | /* NVMe: set resource flag bits */
					PCI_PREF_RANGE_TYPE_64; /* NVMe: NVMe PCIe host resource setup statement */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static resource_size_t calculate_iosize(resource_size_t size, /* NVMe: function declaration/definition */
					resource_size_t min_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					resource_size_t size1, /* NVMe: resource size/alignment value used for BAR/window sizing */
					resource_size_t add_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					resource_size_t children_add_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					resource_size_t old_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					resource_size_t align) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	if (size < min_size) /* NVMe: conditional check */
		size = min_size; /* NVMe: set value for NVMe PCIe host resource setup */
	if (old_size == 1) /* NVMe: conditional check */
		old_size = 0; /* NVMe: set value for NVMe PCIe host resource setup */
	/*
	 * To be fixed in 2.5: we should have sort of HAVE_ISA flag in the
	 * struct pci_bus.
	 */
#if defined(CONFIG_ISA) || defined(CONFIG_EISA) /* NVMe: conditional compilation branch */
	size = (size & 0xff) + ((size & ~0xffUL) << 2); /* NVMe: shift bits to extract field */
#endif /* NVMe: end of conditional compilation */
	size = size + size1; /* NVMe: set value for NVMe PCIe host resource setup */

	size = max(size, add_size) + children_add_size; /* NVMe: take maximum of two values */
	return ALIGN(max(size, old_size), align); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

static resource_size_t calculate_memsize(resource_size_t size, /* NVMe: function declaration/definition */
					 resource_size_t min_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					 resource_size_t children_add_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
					 resource_size_t align) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	size = max(size, min_size) + children_add_size; /* NVMe: take maximum of two values */
	return ALIGN(size, align); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

resource_size_t __weak pcibios_window_alignment(struct pci_bus *bus, /* NVMe: function declaration/definition */
						unsigned long type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	return 1; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

#define PCI_P2P_DEFAULT_MEM_ALIGN	SZ_1M /* NVMe: define constant/macro for PCI resource setup */
#define PCI_P2P_DEFAULT_IO_ALIGN	SZ_4K /* NVMe: define constant/macro for PCI resource setup */
#define PCI_P2P_DEFAULT_IO_ALIGN_1K	SZ_1K /* NVMe: define constant/macro for PCI resource setup */

resource_size_t pci_min_window_alignment(struct pci_bus *bus, unsigned long type) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	resource_size_t align = 1, arch_align; /* NVMe: resource size/alignment value used for BAR/window sizing */

	if (type & IORESOURCE_MEM) /* NVMe: conditional check */
		align = PCI_P2P_DEFAULT_MEM_ALIGN; /* NVMe: set value for NVMe PCIe host resource setup */
	else if (type & IORESOURCE_IO) { /* NVMe: additional conditional branch */
		/*
		 * Per spec, I/O windows are 4K-aligned, but some bridges have
		 * an extension to support 1K alignment.
		 */
		if (bus->self && bus->self->io_window_1k) /* NVMe: conditional check */
			align = PCI_P2P_DEFAULT_IO_ALIGN_1K; /* NVMe: set value for NVMe PCIe host resource setup */
		else /* NVMe: alternative code path */
			align = PCI_P2P_DEFAULT_IO_ALIGN; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */

	arch_align = pcibios_window_alignment(bus, type); /* NVMe: arch-specific window alignment */
	return max(align, arch_align); /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * pbus_size_io() - Size the I/O window of a given bus
 *
 * @bus:		The bus
 * @add_size:		Additional I/O window
 * @realloc_head:	Track the additional I/O window on this list
 *
 * Sizing the I/O windows of the PCI-PCI bridge is trivial, since these
 * windows have 1K or 4K granularity and the I/O ranges of non-bridge PCI
 * devices are limited to 256 bytes.  We must be careful with the ISA
 * aliasing though.
 */
static void pbus_size_io(struct pci_bus *bus, resource_size_t add_size, /* NVMe: function declaration/definition */
			 struct list_head *realloc_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *b_res = pbus_select_window_for_type(bus, IORESOURCE_IO); /* NVMe: NVMe PCIe host resource structure member/variable */
	resource_size_t size = 0, size0 = 0, size1 = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t children_add_size = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t min_align, align; /* NVMe: resource size/alignment value used for BAR/window sizing */

	if (!b_res) /* NVMe: conditional check */
		return; /* NVMe: early return */

	/* If resource is already assigned, nothing more to do */
	if (resource_assigned(b_res)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	min_align = pci_min_window_alignment(bus, IORESOURCE_IO); /* NVMe: update minimum alignment */
	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		struct resource *r; /* NVMe: NVMe PCIe host resource structure member/variable */

		pci_dev_for_each_resource(dev, r) { /* NVMe: iterate over device BARs and bridge windows */
			unsigned long r_size; /* NVMe: unsigned integer variable */

			if (resource_assigned(r) || !(r->flags & IORESOURCE_IO)) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */

			if (!pdev_resource_assignable(dev, r)) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */

			r_size = resource_size(r); /* NVMe: get current size of a resource */
			if (r_size < SZ_1K) /* NVMe: conditional check */
				/* Might be re-aligned for ISA */
				size += r_size; /* NVMe: set value for NVMe PCIe host resource setup */
			else /* NVMe: alternative code path */
				size1 += r_size; /* NVMe: set value for NVMe PCIe host resource setup */

			align = pci_resource_alignment(dev, r); /* NVMe: get required alignment for this BAR/window */
			if (align > min_align) /* NVMe: conditional check */
				min_align = align; /* NVMe: set value for NVMe PCIe host resource setup */

			if (realloc_head) /* NVMe: conditional check */
				children_add_size += get_res_add_size(realloc_head, r); /* NVMe: fetch additional size requested for a resource */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	size0 = calculate_iosize(size, realloc_head ? 0 : add_size, size1, 0, 0, /* NVMe: calculate I/O window size with ISA aliasing */
			resource_size(b_res), min_align); /* NVMe: get current size of a resource */

	if (size0) /* NVMe: conditional check */
		b_res->flags &= ~IORESOURCE_DISABLED; /* NVMe: clear resource flag bits */

	size1 = size0; /* NVMe: set value for NVMe PCIe host resource setup */
	if (realloc_head && (add_size > 0 || children_add_size > 0)) { /* NVMe: conditional check */
		size1 = calculate_iosize(size, 0, size1, add_size, /* NVMe: calculate I/O window size with ISA aliasing */
					 children_add_size, resource_size(b_res), /* NVMe: get current size of a resource */
					 min_align); /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (!size0 && !size1) { /* NVMe: conditional check */
		if (bus->self && (b_res->start || b_res->end)) /* NVMe: conditional check */
			pci_info(bus->self, "disabling bridge window %pR to %pR (unused)\n", /* NVMe: emit informational message */
				 b_res, &bus->busn_res); /* NVMe: access PCI device/resource member */
		b_res->flags |= IORESOURCE_DISABLED; /* NVMe: set resource flag bits */
		return; /* NVMe: early return */
	} /* NVMe: end NVMe PCIe host setup code block */

	resource_set_range(b_res, min_align, size0); /* NVMe: set resource start/end and alignment */
	b_res->flags |= IORESOURCE_STARTALIGN; /* NVMe: set resource flag bits */
	if (bus->self && size1 > size0 && realloc_head) { /* NVMe: conditional check */
		b_res->flags &= ~IORESOURCE_DISABLED; /* NVMe: clear resource flag bits */
		pci_dev_res_add_to_list(realloc_head, bus->self, b_res, /* NVMe: track resource for allocation or reallocation */
					size1 - size0, min_align); /* NVMe: NVMe PCIe host resource setup statement */
		pci_info(bus->self, "bridge window %pR to %pR add_size %llx\n", /* NVMe: emit informational message */
			 b_res, &bus->busn_res, /* NVMe: access PCI device/resource member */
			 (unsigned long long) size1 - size0); /* NVMe: NVMe PCIe host setup function call */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static inline resource_size_t calculate_mem_align(resource_size_t *aligns, /* NVMe: static pointer variable */
						  int max_order) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	resource_size_t align = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t min_align = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	int order; /* NVMe: integer variable, often index or return code */

	for (order = 0; order <= max_order; order++) { /* NVMe: loop over devices/resources */
		resource_size_t align1 = 1; /* NVMe: resource size/alignment value used for BAR/window sizing */

		align1 <<= order + __ffs(SZ_1M); /* NVMe: find first set bit for alignment order */

		if (!align) /* NVMe: conditional check */
			min_align = align1; /* NVMe: set value for NVMe PCIe host resource setup */
		else if (ALIGN(align + min_align, min_align) < align1) /* NVMe: additional conditional branch */
			min_align = align1 >> 1; /* NVMe: shift bits to extract field */
		align += aligns[order]; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */

	return min_align; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * Calculate bridge window head alignment that leaves no gaps in between
 * resources.
 */
static resource_size_t calculate_head_align(resource_size_t *aligns, /* NVMe: function declaration/definition */
					    int max_order) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	resource_size_t head_align = 1; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t remainder = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	int order; /* NVMe: integer variable, often index or return code */

	/* Take the largest alignment as the starting point. */
	head_align <<= max_order + __ffs(SZ_1M); /* NVMe: find first set bit for alignment order */

	for (order = max_order - 1; order >= 0; order--) { /* NVMe: loop over devices/resources */
		resource_size_t align1 = 1; /* NVMe: resource size/alignment value used for BAR/window sizing */

		align1 <<= order + __ffs(SZ_1M); /* NVMe: find first set bit for alignment order */

		/*
		 * Account smaller resources with alignment < max_order that
		 * could be used to fill head room if alignment less than
		 * max_order is used.
		 */
		remainder += aligns[order]; /* NVMe: set value for NVMe PCIe host resource setup */

		/*
		 * Test if head fill is enough to satisfy the alignment of
		 * the larger resources after reducing the alignment.
		 */
		while ((head_align > align1) && (remainder >= head_align / 2)) { /* NVMe: loop while condition holds */
			head_align /= 2; /* NVMe: set value for NVMe PCIe host resource setup */
			remainder -= head_align; /* NVMe: set value for NVMe PCIe host resource setup */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	return head_align; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * pbus_size_mem_optional - Account optional resources in bridge window
 *
 * Account an optional resource or the optional part of the resource in bridge
 * window size.
 *
 * Return: %true if the resource is entirely optional.
 */
static bool pbus_size_mem_optional(struct pci_dev *dev, int resno, /* NVMe: function declaration/definition */
				   resource_size_t align, /* NVMe: resource size/alignment value used for BAR/window sizing */
				   struct list_head *realloc_head, /* NVMe: NVMe PCIe host resource structure member/variable */
				   resource_size_t *add_align, /* NVMe: resource size/alignment value used for BAR/window sizing */
				   resource_size_t *children_add_size) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: NVMe PCIe host resource structure member/variable */
	bool optional = pci_resource_is_optional(dev, resno); /* NVMe: boolean flag */
	resource_size_t r_size = resource_size(res); /* NVMe: resource size/alignment value used for BAR/window sizing */
	struct pci_dev_resource *dev_res = NULL; /* NVMe: NVMe PCIe host resource structure member/variable */

	if (!realloc_head) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	/*
	 * Only bridges have optional sizes in realloc_head at this
	 * point. As res_to_dev_res() walks the entire realloc_head
	 * list, skip calling it when known unnecessary.
	 */
	if (pci_resource_is_bridge_win(resno)) { /* NVMe: conditional check */
		dev_res = res_to_dev_res(realloc_head, res); /* NVMe: find tracker entry for a resource */
		if (dev_res) { /* NVMe: conditional check */
			*children_add_size += dev_res->add_size; /* NVMe: access PCI device/resource member */
			*add_align = max(*add_align, dev_res->min_align); /* NVMe: take maximum of two values */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (!optional) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	/*
	 * Put requested res to the optional list if not there yet (SR-IOV,
	 * disabled ROM). Bridge windows with an optional part are already
	 * on the list.
	 */
	if (!dev_res) /* NVMe: conditional check */
		pci_dev_res_add_to_list(realloc_head, dev, res, 0, align); /* NVMe: track resource for allocation or reallocation */
	*children_add_size += r_size; /* NVMe: set value for NVMe PCIe host resource setup */
	*add_align = max(align, *add_align); /* NVMe: take maximum of two values */

	return true; /* NVMe: return true */
} /* NVMe: end NVMe PCIe host setup code block */

/**
 * pbus_size_mem() - Size the memory window of a given bus
 *
 * @bus:		The bus
 * @b_res:		The bridge window resource
 * @add_size:		Additional memory window
 * @realloc_head:	Track the additional memory window on this list
 *
 * Calculate the size of the bridge window @b_res and minimal alignment
 * which guarantees that all child resources fit in this size.
 *
 * Set the bus resource start/end to indicate the required size if there an
 * available unassigned bus resource of the desired @type.
 *
 * Add optional resource requests to the @realloc_head list if it is
 * supplied.
 */
static void pbus_size_mem(struct pci_bus *bus, struct resource *b_res, /* NVMe: function declaration/definition */
			  resource_size_t add_size, /* NVMe: resource size/alignment value used for BAR/window sizing */
			  struct list_head *realloc_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	resource_size_t min_align, win_align, align, size, size0, size1 = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t aligns[28] = {}; /* Alignments from 1MB to 128TB */
	int order, max_order; /* NVMe: integer variable, often index or return code */
	resource_size_t children_add_size = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */
	resource_size_t add_align = 0; /* NVMe: resource size/alignment value used for BAR/window sizing */

	if (!b_res) /* NVMe: conditional check */
		return; /* NVMe: early return */

	/* If resource is already assigned, nothing more to do */
	if (resource_assigned(b_res)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	max_order = 0; /* NVMe: set value for NVMe PCIe host resource setup */
	size = 0; /* NVMe: set value for NVMe PCIe host resource setup */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		struct resource *r; /* NVMe: NVMe PCIe host resource structure member/variable */
		int i; /* NVMe: integer variable, often index or return code */

		pci_dev_for_each_resource(dev, r, i) { /* NVMe: iterate over device BARs and bridge windows */
			const char *r_name = pci_resource_name(dev, i); /* NVMe: resource name string for log messages */
			resource_size_t r_size; /* NVMe: resource size/alignment value used for BAR/window sizing */

			if (!pdev_resources_assignable(dev) || /* NVMe: conditional check */
			    !pdev_resource_should_fit(dev, r)) /* NVMe: check if this BAR needs assignment */
				continue; /* NVMe: next loop iteration */
			if (b_res != pbus_select_window(bus, r)) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */

			align = pci_resource_alignment(dev, r); /* NVMe: get required alignment for this BAR/window */
			/*
			 * aligns[0] is for 1MB (since bridge memory
			 * windows are always at least 1MB aligned), so
			 * keep "order" from being negative for smaller
			 * resources.
			 */
			order = max_t(int, __ffs(align) - __ffs(SZ_1M), 0); /* NVMe: find first set bit for alignment order */
			if (order >= ARRAY_SIZE(aligns)) { /* NVMe: conditional check */
				pci_warn(dev, "%s %pR: disabling; bad alignment %#llx\n", /* NVMe: emit warning message about resource issue */
					 r_name, r, (unsigned long long) align); /* NVMe: NVMe PCIe host setup function call */
				r->flags = 0; /* NVMe: access PCI device/resource member */
				continue; /* NVMe: next loop iteration */
			} /* NVMe: end NVMe PCIe host setup code block */

			if (pbus_size_mem_optional(dev, i, align, /* NVMe: conditional check */
						   realloc_head, &add_align, /* NVMe: continue NVMe PCIe host setup argument list */
						   &children_add_size)) /* NVMe: NVMe PCIe host setup operation */
				continue; /* NVMe: next loop iteration */

			r_size = resource_size(r); /* NVMe: get current size of a resource */
			size += max(r_size, align); /* NVMe: take maximum of two values */

			/*
			 * If resource's size is larger than its alignment,
			 * some configurations result in an unwanted gap in
			 * the head space that the larger resource cannot
			 * fill.
			 */
			if (r_size <= align) /* NVMe: conditional check */
				aligns[order] += align; /* NVMe: set value for NVMe PCIe host resource setup */
			if (order > max_order) /* NVMe: conditional check */
				max_order = order; /* NVMe: set value for NVMe PCIe host resource setup */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	win_align = pci_min_window_alignment(bus, b_res->flags); /* NVMe: update minimum alignment */
	min_align = calculate_head_align(aligns, max_order); /* NVMe: compute bridge window head alignment */
	min_align = max(min_align, win_align); /* NVMe: take maximum of two values */
	size0 = calculate_memsize(size, realloc_head ? 0 : add_size, /* NVMe: calculate memory window size */
				  0, win_align); /* NVMe: NVMe PCIe host resource setup statement */

	if (size0) { /* NVMe: conditional check */
		resource_set_range(b_res, min_align, size0); /* NVMe: set resource start/end and alignment */
		b_res->flags &= ~IORESOURCE_DISABLED; /* NVMe: clear resource flag bits */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (realloc_head && (add_size > 0 || children_add_size > 0)) { /* NVMe: conditional check */
		add_align = max(min_align, add_align); /* NVMe: take maximum of two values */
		size1 = calculate_memsize(size, add_size, children_add_size, /* NVMe: calculate memory window size */
					  win_align); /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (!size0 && !size1) { /* NVMe: conditional check */
		if (bus->self && (b_res->start || b_res->end)) /* NVMe: conditional check */
			pci_info(bus->self, "disabling bridge window %pR to %pR (unused)\n", /* NVMe: emit informational message */
				 b_res, &bus->busn_res); /* NVMe: access PCI device/resource member */
		b_res->flags |= IORESOURCE_DISABLED; /* NVMe: set resource flag bits */
		return; /* NVMe: early return */
	} /* NVMe: end NVMe PCIe host setup code block */

	resource_set_range(b_res, min_align, size0); /* NVMe: set resource start/end and alignment */
	b_res->flags |= IORESOURCE_STARTALIGN; /* NVMe: set resource flag bits */
	if (bus->self && realloc_head && (size1 > size0 || add_align > min_align)) { /* NVMe: conditional check */
		b_res->flags &= ~IORESOURCE_DISABLED; /* NVMe: clear resource flag bits */
		add_size = size1 > size0 ? size1 - size0 : 0; /* NVMe: set value for NVMe PCIe host resource setup */
		pci_dev_res_add_to_list(realloc_head, bus->self, b_res, /* NVMe: track resource for allocation or reallocation */
					add_size, add_align); /* NVMe: NVMe PCIe host resource setup statement */
		pci_info(bus->self, "bridge window %pR to %pR add_size %llx add_align %llx\n", /* NVMe: emit informational message */
			   b_res, &bus->busn_res, /* NVMe: access PCI device/resource member */
			   (unsigned long long) add_size, /* NVMe: continue NVMe PCIe host setup argument list */
			   (unsigned long long) add_align); /* NVMe: NVMe PCIe host setup function call */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void __pci_bus_size_bridges(struct pci_bus *bus, struct list_head *realloc_head) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	resource_size_t additional_io_size = 0, additional_mmio_size = 0, /* NVMe: resource size/alignment value used for BAR/window sizing */
			additional_mmio_pref_size = 0; /* NVMe: set value for NVMe PCIe host resource setup */
	struct resource *b_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_host_bridge *host; /* NVMe: NVMe PCIe host resource structure member/variable */
	int hdr_type; /* NVMe: integer variable, often index or return code */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		struct pci_bus *b = dev->subordinate; /* NVMe: NVMe PCIe host resource structure member/variable */
		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		switch (dev->hdr_type) { /* NVMe: dispatch based on header or resource type */
		case PCI_HEADER_TYPE_CARDBUS: /* NVMe: case label */
			if (pci_bus_size_cardbus_bridge(b, realloc_head)) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */
			break; /* NVMe: exit switch/loop */

		case PCI_HEADER_TYPE_BRIDGE: /* NVMe: case label */
		default: /* NVMe: cleanup/error label */
			__pci_bus_size_bridges(b, realloc_head); /* NVMe: calculate required bridge window sizes */
			break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	/* The root bus? */
	if (pci_is_root_bus(bus)) { /* NVMe: conditional check */
		host = to_pci_host_bridge(bus->bridge); /* NVMe: convert bus bridge to host bridge */
		if (!host->size_windows) /* NVMe: conditional check */
			return; /* NVMe: early return */
		hdr_type = -1;	/* Intentionally invalid - not a PCI device. */
	} else { /* NVMe: NVMe PCIe host setup operation */
		hdr_type = bus->self->hdr_type; /* NVMe: access PCI device/resource member */
	} /* NVMe: end NVMe PCIe host setup code block */

	switch (hdr_type) { /* NVMe: dispatch based on header or resource type */
	case PCI_HEADER_TYPE_CARDBUS: /* NVMe: case label */
		/* Don't size CardBuses yet */
		break; /* NVMe: exit switch/loop */

	case PCI_HEADER_TYPE_BRIDGE: /* NVMe: case label */
		pci_bridge_check_ranges(bus); /* NVMe: detect which bridge windows are implemented */
		if (bus->self->is_hotplug_bridge) { /* NVMe: conditional check */
			additional_io_size  = pci_hotplug_io_size; /* NVMe: set value for NVMe PCIe host resource setup */
			additional_mmio_size = pci_hotplug_mmio_size; /* NVMe: set value for NVMe PCIe host resource setup */
			additional_mmio_pref_size = pci_hotplug_mmio_pref_size; /* NVMe: set value for NVMe PCIe host resource setup */
		} /* NVMe: end NVMe PCIe host setup code block */
		fallthrough; /* NVMe: intentional fallthrough to next case */
	default: /* NVMe: cleanup/error label */
		pbus_size_io(bus, additional_io_size, realloc_head); /* NVMe: size I/O aperture behind the bridge */

		b_res = pbus_select_window_for_type(bus, IORESOURCE_MEM | /* NVMe: choose bridge window matching IORESOURCE_* flags */
							 IORESOURCE_PREFETCH | /* NVMe: combine IORESOURCE flag bits */
							 IORESOURCE_MEM_64); /* NVMe: NVMe PCIe host resource setup statement */
		if (b_res && (b_res->flags & IORESOURCE_PREFETCH)) { /* NVMe: conditional check */
			pbus_size_mem(bus, b_res, additional_mmio_pref_size, /* NVMe: size memory aperture behind the bridge */
				      realloc_head); /* NVMe: NVMe PCIe host resource setup statement */
		} /* NVMe: end NVMe PCIe host setup code block */

		b_res = pbus_select_window_for_type(bus, IORESOURCE_MEM); /* NVMe: choose bridge window matching IORESOURCE_* flags */
		if (b_res) { /* NVMe: conditional check */
			pbus_size_mem(bus, b_res, additional_mmio_size, /* NVMe: size memory aperture behind the bridge */
				      realloc_head); /* NVMe: NVMe PCIe host resource setup statement */
		} /* NVMe: end NVMe PCIe host setup code block */
		break; /* NVMe: exit switch/loop */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_bus_size_bridges(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	__pci_bus_size_bridges(bus, NULL); /* NVMe: calculate required bridge window sizes */
} /* NVMe: end NVMe PCIe host setup code block */
EXPORT_SYMBOL(pci_bus_size_bridges); /* NVMe: export symbol for module use */

static void assign_fixed_resource_on_bus(struct pci_bus *b, struct resource *r) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *parent_r; /* NVMe: NVMe PCIe host resource structure member/variable */
	unsigned long mask = IORESOURCE_IO | IORESOURCE_MEM | /* NVMe: unsigned integer variable */
			     IORESOURCE_PREFETCH; /* NVMe: NVMe PCIe host resource setup statement */

	pci_bus_for_each_resource(b, parent_r) { /* NVMe: iterate over bus-level bridge resources */
		if (!parent_r) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if ((r->flags & mask) == (parent_r->flags & mask) && /* NVMe: conditional check */
		    resource_contains(parent_r, r)) /* NVMe: check if parent resource contains child */
			request_resource(parent_r, r); /* NVMe: request resource from parent */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * Try to assign any resources marked as IORESOURCE_PCI_FIXED, as they are
 * skipped by pbus_assign_resources_sorted().
 */
static void pdev_assign_fixed_resources(struct pci_dev *dev) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *r; /* NVMe: NVMe PCIe host resource structure member/variable */

	pci_dev_for_each_resource(dev, r) { /* NVMe: iterate over device BARs and bridge windows */
		struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */

		if (resource_assigned(r) || /* NVMe: conditional check */
		    !(r->flags & IORESOURCE_PCI_FIXED) || /* NVMe: combine IORESOURCE flag bits */
		    !(r->flags & (IORESOURCE_IO | IORESOURCE_MEM))) /* NVMe: combine IORESOURCE flag bits */
			continue; /* NVMe: next loop iteration */

		b = dev->bus; /* NVMe: access PCI device/resource member */
		while (b && !resource_assigned(r)) { /* NVMe: loop while condition holds */
			assign_fixed_resource_on_bus(b, r); /* NVMe: assign a fixed resource on parent bus */
			b = b->parent; /* NVMe: access PCI device/resource member */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void __pci_bus_assign_resources(const struct pci_bus *bus, /* NVMe: function declaration/definition */
				struct list_head *realloc_head, /* NVMe: NVMe PCIe host resource structure member/variable */
				struct list_head *fail_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */

	pbus_assign_resources_sorted(bus, realloc_head, fail_head); /* NVMe: assign resources sorted by alignment */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		pdev_assign_fixed_resources(dev); /* NVMe: assign resources marked IORESOURCE_PCI_FIXED */

		b = dev->subordinate; /* NVMe: access PCI device/resource member */
		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		__pci_bus_assign_resources(b, realloc_head, fail_head); /* NVMe: assign resources to PCI bus hierarchy */

		switch (dev->hdr_type) { /* NVMe: dispatch based on header or resource type */
		case PCI_HEADER_TYPE_BRIDGE: /* NVMe: case label */
			if (!pci_is_enabled(dev)) /* NVMe: conditional check */
				pci_setup_bridge(b); /* NVMe: write bridge base/limit registers to hardware */
			break; /* NVMe: exit switch/loop */

		case PCI_HEADER_TYPE_CARDBUS: /* NVMe: case label */
			pci_setup_cardbus_bridge(b); /* NVMe: configure CardBus bridge windows */
			break; /* NVMe: exit switch/loop */

		default: /* NVMe: cleanup/error label */
			pci_info(dev, "not setting up bridge for bus %04x:%02x\n", /* NVMe: emit informational message */
				 pci_domain_nr(b), b->number); /* NVMe: get PCI domain number */
			break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_bus_assign_resources(const struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	__pci_bus_assign_resources(bus, NULL, NULL); /* NVMe: assign resources to PCI bus hierarchy */
} /* NVMe: end NVMe PCIe host setup code block */
EXPORT_SYMBOL(pci_bus_assign_resources); /* NVMe: export symbol for module use */

static void pci_claim_device_resources(struct pci_dev *dev) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int i; /* NVMe: integer variable, often index or return code */

	for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) { /* NVMe: loop over devices/resources */
		struct resource *r = &dev->resource[i]; /* NVMe: NVMe PCIe host resource structure member/variable */

		if (!r->flags || resource_assigned(r)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pci_claim_resource(dev, i); /* NVMe: claim a BAR from the parent bridge window */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_claim_bridge_resources(struct pci_dev *dev) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int i; /* NVMe: integer variable, often index or return code */

	for (i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) { /* NVMe: loop over devices/resources */
		struct resource *r = &dev->resource[i]; /* NVMe: NVMe PCIe host resource structure member/variable */

		if (!r->flags || resource_assigned(r)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */
		if (r->flags & IORESOURCE_DISABLED) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pci_claim_bridge_resource(dev, i); /* NVMe: claim a bridge window resource */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bus_allocate_dev_resources(struct pci_bus *b) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_bus *child; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry(dev, &b->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		pci_claim_device_resources(dev); /* NVMe: claim device BARs from parent */

		child = dev->subordinate; /* NVMe: access PCI device/resource member */
		if (child) /* NVMe: conditional check */
			pci_bus_allocate_dev_resources(child); /* NVMe: allocate device BARs recursively */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bus_allocate_resources(struct pci_bus *b) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *child; /* NVMe: NVMe PCIe host resource structure member/variable */

	/*
	 * Carry out a depth-first search on the PCI bus tree to allocate
	 * bridge apertures.  Read the programmed bridge bases and
	 * recursively claim the respective bridge resources.
	 */
	if (b->self) { /* NVMe: conditional check */
		pci_read_bridge_bases(b); /* NVMe: read programmed bridge base/limit registers */
		pci_claim_bridge_resources(b->self); /* NVMe: claim bridge windows from parent */
	} /* NVMe: end NVMe PCIe host setup code block */

	list_for_each_entry(child, &b->children, node) /* NVMe: iterate over a linked list of resources/devices */
		pci_bus_allocate_resources(child); /* NVMe: allocate bridge resources recursively */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_bus_claim_resources(struct pci_bus *b) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	pci_bus_allocate_resources(b); /* NVMe: allocate bridge resources recursively */
	pci_bus_allocate_dev_resources(b); /* NVMe: allocate device BARs recursively */
} /* NVMe: end NVMe PCIe host setup code block */
EXPORT_SYMBOL(pci_bus_claim_resources); /* NVMe: export symbol for module use */

static void __pci_bridge_assign_resources(const struct pci_dev *bridge, /* NVMe: function declaration/definition */
					  struct list_head *add_head, /* NVMe: NVMe PCIe host resource structure member/variable */
					  struct list_head *fail_head) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */

	pdev_assign_resources_sorted((struct pci_dev *)bridge, /* NVMe: assign one device's BARs in order */
					 add_head, fail_head); /* NVMe: NVMe PCIe host resource setup statement */

	b = bridge->subordinate; /* NVMe: access PCI device/resource member */
	if (!b) /* NVMe: conditional check */
		return; /* NVMe: early return */

	__pci_bus_assign_resources(b, add_head, fail_head); /* NVMe: assign resources to PCI bus hierarchy */

	switch (bridge->class >> 8) { /* NVMe: dispatch based on header or resource type */
	case PCI_CLASS_BRIDGE_PCI: /* NVMe: case label */
		pci_setup_bridge(b); /* NVMe: write bridge base/limit registers to hardware */
		break; /* NVMe: exit switch/loop */

	case PCI_CLASS_BRIDGE_CARDBUS: /* NVMe: case label */
		pci_setup_cardbus_bridge(b); /* NVMe: configure CardBus bridge windows */
		break; /* NVMe: exit switch/loop */

	default: /* NVMe: cleanup/error label */
		pci_info(bridge, "not setting up bridge for bus %04x:%02x\n", /* NVMe: emit informational message */
			 pci_domain_nr(b), b->number); /* NVMe: get PCI domain number */
		break; /* NVMe: exit switch/loop */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bridge_release_resources(struct pci_bus *bus, /* NVMe: function declaration/definition */
					 struct resource *b_win) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev = bus->self; /* NVMe: NVMe PCIe host resource structure member/variable */
	int idx, ret; /* NVMe: integer variable, often index or return code */

	if (!resource_assigned(b_win)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	idx = pci_resource_num(dev, b_win); /* NVMe: convert resource pointer to BAR/bridge index */

	/* If there are children, release them all */
	release_child_resources(b_win); /* NVMe: release all child resources from a window */

	ret = pci_release_resource(dev, idx); /* NVMe: release a BAR or bridge window back to parent */
	if (ret) /* NVMe: conditional check */
		return; /* NVMe: early return */

	pci_setup_one_bridge_window(dev, idx); /* NVMe: program one bridge window register set */
} /* NVMe: end NVMe PCIe host setup code block */

enum release_type { /* NVMe: start of enumeration definition */
	leaf_only, /* NVMe: continue NVMe PCIe host setup argument list */
	whole_subtree, /* NVMe: continue NVMe PCIe host setup argument list */
}; /* NVMe: end NVMe PCIe host resource structure/union/enum definition */

/*
 * Try to release PCI bridge resources from leaf bridge, so we can allocate
 * a larger window later.
 */
static void pci_bus_release_bridge_resources(struct pci_bus *bus, /* NVMe: function declaration/definition */
					     struct resource *b_win, /* NVMe: NVMe PCIe host resource structure member/variable */
					     enum release_type rel_type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	bool is_leaf_bridge = true; /* NVMe: boolean flag */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		struct pci_bus *b = dev->subordinate; /* NVMe: NVMe PCIe host resource structure member/variable */
		struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */

		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		is_leaf_bridge = false; /* NVMe: set value for NVMe PCIe host resource setup */

		if ((dev->class >> 8) != PCI_CLASS_BRIDGE_PCI) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (rel_type != whole_subtree) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pci_bus_for_each_resource(b, res) { /* NVMe: iterate over bus-level bridge resources */
			if (res->parent != b_win) /* NVMe: conditional check */
				continue; /* NVMe: next loop iteration */

			pci_bus_release_bridge_resources(b, res, rel_type); /* NVMe: release bridge window for reallocation */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (pci_is_root_bus(bus)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	if ((bus->self->class >> 8) != PCI_CLASS_BRIDGE_PCI) /* NVMe: conditional check */
		return; /* NVMe: early return */

	if ((rel_type == whole_subtree) || is_leaf_bridge) /* NVMe: conditional check */
		pci_bridge_release_resources(bus, b_win); /* NVMe: release one bridge window */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bus_dump_res(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
	int i; /* NVMe: integer variable, often index or return code */

	pci_bus_for_each_resource(bus, res, i) { /* NVMe: iterate over bus-level bridge resources */
		if (!res || !res->end || !res->flags) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		dev_info(&bus->dev, "resource %d %pR\n", i, res); /* NVMe: device info message */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bus_dump_resources(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */


	pci_bus_dump_res(bus); /* NVMe: dump one bus resource */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		b = dev->subordinate; /* NVMe: access PCI device/resource member */
		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pci_bus_dump_resources(b); /* NVMe: dump bus resource tree */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static int pci_bus_get_depth(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int depth = 0; /* NVMe: integer variable, often index or return code */
	struct pci_bus *child_bus; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry(child_bus, &bus->children, node) { /* NVMe: iterate over a linked list of resources/devices */
		int ret; /* NVMe: integer variable, often index or return code */

		ret = pci_bus_get_depth(child_bus); /* NVMe: compute maximum depth of bus tree */
		if (ret + 1 > depth) /* NVMe: conditional check */
			depth = ret + 1; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */

	return depth; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * -1: undefined, will auto detect later
 *  0: disabled by user
 *  1: disabled by auto detect
 *  2: enabled by user
 *  3: enabled by auto detect
 */
enum enable_type { /* NVMe: start of enumeration definition */
	undefined = -1, /* NVMe: continue NVMe PCIe host setup argument list */
	user_disabled, /* NVMe: continue NVMe PCIe host setup argument list */
	auto_disabled, /* NVMe: continue NVMe PCIe host setup argument list */
	user_enabled, /* NVMe: continue NVMe PCIe host setup argument list */
	auto_enabled, /* NVMe: continue NVMe PCIe host setup argument list */
}; /* NVMe: end NVMe PCIe host resource structure/union/enum definition */

static enum enable_type pci_realloc_enable = undefined; /* NVMe: set value for NVMe PCIe host resource setup */
void __init pci_realloc_get_opt(char *str) /* NVMe: parse pci=realloc kernel option */
{ /* NVMe: begin NVMe PCIe host setup code block */
	if (!strncmp(str, "off", 3)) /* NVMe: conditional check */
		pci_realloc_enable = user_disabled; /* NVMe: set value for NVMe PCIe host resource setup */
	else if (!strncmp(str, "on", 2)) /* NVMe: additional conditional branch */
		pci_realloc_enable = user_enabled; /* NVMe: set value for NVMe PCIe host resource setup */
} /* NVMe: end NVMe PCIe host setup code block */
static bool pci_realloc_enabled(enum enable_type enable) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	return enable >= user_enabled; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */

#if defined(CONFIG_PCI_IOV) && defined(CONFIG_PCI_REALLOC_ENABLE_AUTO) /* NVMe: conditional compilation branch */
static int iov_resources_unassigned(struct pci_dev *dev, void *data) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	int i; /* NVMe: integer variable, often index or return code */
	bool *unassigned = data; /* NVMe: boolean flag */

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) { /* NVMe: loop over devices/resources */
		int idx = pci_resource_num_from_vf_bar(i); /* NVMe: integer variable, often index or return code */
		struct resource *r = &dev->resource[idx]; /* NVMe: NVMe PCIe host resource structure member/variable */
		struct pci_bus_region region; /* NVMe: NVMe PCIe host resource structure member/variable */

		/* Not assigned or rejected by kernel? */
		if (!r->flags) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pcibios_resource_to_bus(dev->bus, &region, r); /* NVMe: translate CPU address to bus-relative address */
		if (!region.start) { /* NVMe: conditional check */
			*unassigned = true; /* NVMe: set value for NVMe PCIe host resource setup */
			return 1; /* Return early from pci_walk_bus() */
		} /* NVMe: end NVMe PCIe host setup code block */
	} /* NVMe: end NVMe PCIe host setup code block */

	return 0; /* NVMe: success */
} /* NVMe: end NVMe PCIe host setup code block */

static enum enable_type pci_realloc_detect(struct pci_bus *bus, /* NVMe: function declaration/definition */
					   enum enable_type enable_local) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	bool unassigned = false; /* NVMe: boolean flag */
	struct pci_host_bridge *host; /* NVMe: NVMe PCIe host resource structure member/variable */

	if (enable_local != undefined) /* NVMe: conditional check */
		return enable_local; /* NVMe: return result */

	host = pci_find_host_bridge(bus); /* NVMe: find host bridge for this bus */
	if (host->preserve_config) /* NVMe: conditional check */
		return auto_disabled; /* NVMe: return result */

	pci_walk_bus(bus, iov_resources_unassigned, &unassigned); /* NVMe: walk PCI bus tree looking for unassigned resources */
	if (unassigned) /* NVMe: conditional check */
		return auto_enabled; /* NVMe: return result */

	return enable_local; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */
#else /* NVMe: opposite conditional compilation branch */
static enum enable_type pci_realloc_detect(struct pci_bus *bus, /* NVMe: function declaration/definition */
					   enum enable_type enable_local) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	return enable_local; /* NVMe: return result */
} /* NVMe: end NVMe PCIe host setup code block */
#endif /* NVMe: end of conditional compilation */

static void adjust_bridge_window(struct pci_dev *bridge, struct resource *res, /* NVMe: function declaration/definition */
				 struct list_head *add_list, /* NVMe: NVMe PCIe host resource structure member/variable */
				 resource_size_t new_size) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	resource_size_t add_size, size = resource_size(res); /* NVMe: resource size/alignment value used for BAR/window sizing */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	if (resource_assigned(res)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	if (!new_size) /* NVMe: conditional check */
		return; /* NVMe: early return */

	if (new_size > size) { /* NVMe: conditional check */
		add_size = new_size - size; /* NVMe: set value for NVMe PCIe host resource setup */
		pci_dbg(bridge, "bridge window %pR extended by %pa\n", res, /* NVMe: emit debug message */
			&add_size); /* NVMe: NVMe PCIe host resource setup statement */
	} else if (new_size < size) { /* NVMe: NVMe PCIe host setup function call/header */
		int idx = pci_resource_num(bridge, res); /* NVMe: integer variable, often index or return code */

		/*
		 * hpio/mmio/mmioprefsize hasn't been included at all? See the
		 * add_size param at the callsites of calculate_memsize().
		 */
		if (!add_list) /* NVMe: conditional check */
			return; /* NVMe: early return */

		/* Only shrink if the hotplug extra relates to window size. */
		switch (idx) { /* NVMe: dispatch based on header or resource type */
			case PCI_BRIDGE_IO_WINDOW: /* NVMe: case label */
				if (size > pci_hotplug_io_size) /* NVMe: conditional check */
					return; /* NVMe: early return */
				break; /* NVMe: exit switch/loop */
			case PCI_BRIDGE_MEM_WINDOW: /* NVMe: case label */
				if (size > pci_hotplug_mmio_size) /* NVMe: conditional check */
					return; /* NVMe: early return */
				break; /* NVMe: exit switch/loop */
			case PCI_BRIDGE_PREF_MEM_WINDOW: /* NVMe: case label */
				if (size > pci_hotplug_mmio_pref_size) /* NVMe: conditional check */
					return; /* NVMe: early return */
				break; /* NVMe: exit switch/loop */
			default: /* NVMe: cleanup/error label */
				break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */

		dev_res = res_to_dev_res(add_list, res); /* NVMe: find tracker entry for a resource */
		add_size = size - new_size; /* NVMe: set value for NVMe PCIe host resource setup */
		if (add_size < dev_res->add_size) { /* NVMe: conditional check */
			dev_res->add_size -= add_size; /* NVMe: access PCI device/resource member */
			pci_dbg(bridge, "bridge window %pR optional size shrunken by %pa\n", /* NVMe: emit debug message */
				res, &add_size); /* NVMe: NVMe PCIe host resource setup statement */
		} else { /* NVMe: NVMe PCIe host setup operation */
			pci_dbg(bridge, "bridge window %pR optional size removed\n", /* NVMe: emit debug message */
				res); /* NVMe: NVMe PCIe host resource setup statement */
			pci_dev_res_remove_from_list(add_list, res); /* NVMe: remove resource tracker from list */
		} /* NVMe: end NVMe PCIe host setup code block */
		return; /* NVMe: early return */

	} else { /* NVMe: NVMe PCIe host setup operation */
		return; /* NVMe: early return */
	} /* NVMe: end NVMe PCIe host setup code block */

	resource_set_size(res, new_size); /* NVMe: set resource size */

	/* If the resource is part of the add_list, remove it now */
	if (add_list) /* NVMe: conditional check */
		pci_dev_res_remove_from_list(add_list, res); /* NVMe: remove resource tracker from list */
} /* NVMe: end NVMe PCIe host setup code block */

static void remove_dev_resource(struct resource *avail, struct pci_dev *dev, /* NVMe: function declaration/definition */
				struct resource *res) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	resource_size_t size, align, tmp; /* NVMe: resource size/alignment value used for BAR/window sizing */

	size = resource_size(res); /* NVMe: get current size of a resource */
	if (!size) /* NVMe: conditional check */
		return; /* NVMe: early return */

	align = pci_resource_alignment(dev, res); /* NVMe: get required alignment for this BAR/window */
	align = align ? ALIGN(avail->start, align) - avail->start : 0; /* NVMe: round up to alignment */
	tmp = align + size; /* NVMe: set value for NVMe PCIe host resource setup */
	avail->start = min(avail->start + tmp, avail->end + 1); /* NVMe: take minimum of two values */
} /* NVMe: end NVMe PCIe host setup code block */

static void remove_dev_resources(struct pci_dev *dev, /* NVMe: function declaration/definition */
				 struct resource available[PCI_P2P_BRIDGE_RESOURCE_NUM]) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res, *b_win; /* NVMe: NVMe PCIe host resource structure member/variable */
	int idx; /* NVMe: integer variable, often index or return code */

	pci_dev_for_each_resource(dev, res) { /* NVMe: iterate over device BARs and bridge windows */
		b_win = pbus_select_window(dev->bus, res); /* NVMe: select parent bridge window for this resource type */
		if (!b_win) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		idx = pci_resource_num(dev->bus->self, b_win); /* NVMe: convert resource pointer to BAR/bridge index */
		idx -= PCI_BRIDGE_RESOURCES; /* NVMe: set value for NVMe PCIe host resource setup */

		remove_dev_resource(&available[idx], dev, res); /* NVMe: subtract BAR size from available space */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

#define ALIGN_DOWN_IF_NONZERO(addr, align) /* NVMe: define constant/macro for PCI resource setup */\
			((align) ? ALIGN_DOWN((addr), (align)) : (addr)) /* NVMe: round down to alignment */

/*
 * io, mmio and mmio_pref contain the total amount of bridge window space
 * available. This includes the minimal space needed to cover all the
 * existing devices on the bus and the possible extra space that can be
 * shared with the bridges.
 */
static void pci_bus_distribute_available_resources(struct pci_bus *bus, /* NVMe: function declaration/definition */
		    struct list_head *add_list, /* NVMe: NVMe PCIe host resource structure member/variable */
		    struct resource available_in[PCI_P2P_BRIDGE_RESOURCE_NUM]) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource available[PCI_P2P_BRIDGE_RESOURCE_NUM]; /* NVMe: NVMe PCIe host resource setup statement */
	unsigned int normal_bridges = 0, hotplug_bridges = 0; /* NVMe: unsigned integer variable */
	struct pci_dev *dev, *bridge = bus->self; /* NVMe: NVMe PCIe host resource structure member/variable */
	resource_size_t per_bridge[PCI_P2P_BRIDGE_RESOURCE_NUM]; /* NVMe: NVMe PCIe host resource setup statement */
	resource_size_t align; /* NVMe: resource size/alignment value used for BAR/window sizing */
	int i; /* NVMe: integer variable, often index or return code */

	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: loop over devices/resources */
		struct resource *res = /* NVMe: NVMe PCIe host resource structure member/variable */
			pci_resource_n(bridge, PCI_BRIDGE_RESOURCES + i); /* NVMe: NVMe PCIe host setup function call */

		available[i] = available_in[i]; /* NVMe: set value for NVMe PCIe host resource setup */

		/*
		 * The alignment of this bridge is yet to be considered,
		 * hence it must be done now before extending its bridge
		 * window.
		 */
		align = pci_resource_alignment(bridge, res); /* NVMe: get required alignment for this BAR/window */
		if (!resource_assigned(res) && align) /* NVMe: conditional check */
			available[i].start = min(ALIGN(available[i].start, align), /* NVMe: round up to alignment */
						 available[i].end + 1); /* NVMe: NVMe PCIe host resource setup statement */

		/*
		 * Now that we have adjusted for alignment, update the
		 * bridge window resources to fill as much remaining
		 * resource space as possible.
		 */
		adjust_bridge_window(bridge, res, add_list, /* NVMe: extend or shrink bridge window size */
				     resource_size(&available[i])); /* NVMe: get current size of a resource */
	} /* NVMe: end NVMe PCIe host setup code block */

	/*
	 * Calculate how many hotplug bridges and normal bridges there
	 * are on this bus.  We will distribute the additional available
	 * resources between hotplug bridges.
	 */
	for_each_pci_bridge(dev, bus) { /* NVMe: NVMe PCIe host setup function call/header */
		if (dev->is_hotplug_bridge) /* NVMe: conditional check */
			hotplug_bridges++; /* NVMe: NVMe PCIe host resource setup statement */
		else /* NVMe: alternative code path */
			normal_bridges++; /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (!(hotplug_bridges + normal_bridges)) /* NVMe: conditional check */
		return; /* NVMe: early return */

	/*
	 * Calculate the amount of space we can forward from "bus" to any
	 * downstream buses, i.e., the space left over after assigning the
	 * BARs and windows on "bus".
	 */
	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: iterate over a linked list of resources/devices */
		if (!dev->is_virtfn) /* NVMe: conditional check */
			remove_dev_resources(dev, available); /* NVMe: subtract all device BARs from available space */
	} /* NVMe: end NVMe PCIe host setup code block */

	/*
	 * If there is at least one hotplug bridge on this bus it gets all
	 * the extra resource space that was left after the reductions
	 * above.
	 *
	 * If there are no hotplug bridges the extra resource space is
	 * split between non-hotplug bridges. This is to allow possible
	 * hotplug bridges below them to get the extra space as well.
	 */
	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: loop over devices/resources */
		per_bridge[i] = div64_ul(resource_size(&available[i]), /* NVMe: get current size of a resource */
					 hotplug_bridges ?: normal_bridges); /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	for_each_pci_bridge(dev, bus) { /* NVMe: NVMe PCIe host setup function call/header */
		struct resource *res; /* NVMe: NVMe PCIe host resource structure member/variable */
		struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */

		b = dev->subordinate; /* NVMe: access PCI device/resource member */
		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */
		if (hotplug_bridges && !dev->is_hotplug_bridge) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: loop over devices/resources */
			res = pci_resource_n(dev, PCI_BRIDGE_RESOURCES + i); /* NVMe: set value for NVMe PCIe host resource setup */

			/*
			 * Make sure the split resource space is properly
			 * aligned for bridge windows (align it down to
			 * avoid going above what is available).
			 */
			align = pci_resource_alignment(dev, res); /* NVMe: get required alignment for this BAR/window */
			resource_set_size(&available[i], /* NVMe: set resource size */
					  ALIGN_DOWN_IF_NONZERO(per_bridge[i], /* NVMe: continue NVMe PCIe host setup argument list */
								align)); /* NVMe: NVMe PCIe host resource setup statement */

			/*
			 * The per_bridge holds the extra resource space
			 * that can be added for each bridge but there is
			 * the minimal already reserved as well so adjust
			 * x.start down accordingly to cover the whole
			 * space.
			 */
			available[i].start -= resource_size(res); /* NVMe: get current size of a resource */
		} /* NVMe: end NVMe PCIe host setup code block */

		pci_bus_distribute_available_resources(b, add_list, available); /* NVMe: distribute spare space to downstream bridges */

		for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) /* NVMe: loop over devices/resources */
			available[i].start += available[i].end + 1; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_bridge_distribute_available_resources(struct pci_dev *bridge, /* NVMe: function declaration/definition */
						      struct list_head *add_list) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res, available[PCI_P2P_BRIDGE_RESOURCE_NUM]; /* NVMe: NVMe PCIe host resource structure member/variable */
	unsigned int i; /* NVMe: unsigned integer variable */

	if (!bridge->is_hotplug_bridge) /* NVMe: conditional check */
		return; /* NVMe: early return */

	pci_dbg(bridge, "distributing available resources\n"); /* NVMe: emit debug message */

	/* Take the initial extra resources from the hotplug port */
	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: loop over devices/resources */
		res = pci_resource_n(bridge, PCI_BRIDGE_RESOURCES + i); /* NVMe: set value for NVMe PCIe host resource setup */
		available[i] = *res; /* NVMe: set value for NVMe PCIe host resource setup */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_bus_distribute_available_resources(bridge->subordinate, /* NVMe: distribute spare space to downstream bridges */
					       add_list, available); /* NVMe: NVMe PCIe host resource setup statement */
} /* NVMe: end NVMe PCIe host setup code block */

static bool pci_bridge_resources_not_assigned(struct pci_dev *dev) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	const struct resource *r; /* NVMe: NVMe PCIe host resource setup statement */

	/*
	 * If the child device's resources are not yet assigned it means we
	 * are configuring them (not the boot firmware), so we should be
	 * able to extend the upstream bridge resources in the same way we
	 * do with the normal hotplug case.
	 */
	r = &dev->resource[PCI_BRIDGE_IO_WINDOW]; /* NVMe: access PCI device/resource member */
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN)) /* NVMe: conditional check */
		return false; /* NVMe: return false */
	r = &dev->resource[PCI_BRIDGE_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN)) /* NVMe: conditional check */
		return false; /* NVMe: return false */
	r = &dev->resource[PCI_BRIDGE_PREF_MEM_WINDOW]; /* NVMe: access PCI device/resource member */
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN)) /* NVMe: conditional check */
		return false; /* NVMe: return false */

	return true; /* NVMe: return true */
} /* NVMe: end NVMe PCIe host setup code block */

static void /* NVMe: NVMe PCIe host setup operation */
pci_root_bus_distribute_available_resources(struct pci_bus *bus, /* NVMe: distribute from root bus */
					    struct list_head *add_list) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev, *bridge = bus->self; /* NVMe: NVMe PCIe host resource structure member/variable */

	for_each_pci_bridge(dev, bus) { /* NVMe: NVMe PCIe host setup function call/header */
		struct pci_bus *b; /* NVMe: NVMe PCIe host resource structure member/variable */

		b = dev->subordinate; /* NVMe: access PCI device/resource member */
		if (!b) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		/*
		 * Need to check "bridge" here too because it is NULL
		 * in case of root bus.
		 */
		if (bridge && pci_bridge_resources_not_assigned(dev)) /* NVMe: conditional check */
			pci_bridge_distribute_available_resources(dev, add_list); /* NVMe: start distribution from hotplug bridge */
		else /* NVMe: alternative code path */
			pci_root_bus_distribute_available_resources(b, add_list); /* NVMe: distribute from root bus */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

static void pci_prepare_next_assign_round(struct list_head *fail_head, /* NVMe: function declaration/definition */
					  int tried_times, /* NVMe: integer variable, often index or return code */
					  enum release_type rel_type) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev_resource *fail_res; /* NVMe: NVMe PCIe host resource structure member/variable */

	pr_info("PCI: No. %d try to assign unassigned res\n", tried_times + 1); /* NVMe: kernel info message */

	/*
	 * Try to release leaf bridge's resources that aren't big
	 * enough to contain child device resources.
	 */
	list_for_each_entry(fail_res, fail_head, list) { /* NVMe: iterate over a linked list of resources/devices */
		struct pci_bus *bus = fail_res->dev->bus; /* NVMe: NVMe PCIe host resource structure member/variable */
		struct resource *b_win; /* NVMe: NVMe PCIe host resource structure member/variable */

		b_win = pbus_select_window_for_type(bus, fail_res->flags); /* NVMe: choose bridge window matching IORESOURCE_* flags */
		if (!b_win) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */
		pci_bus_release_bridge_resources(bus, b_win, rel_type); /* NVMe: release bridge window for reallocation */
	} /* NVMe: end NVMe PCIe host setup code block */

	/* Restore size and flags */
	list_for_each_entry(fail_res, fail_head, list) /* NVMe: iterate over a linked list of resources/devices */
		pci_dev_res_restore(fail_res); /* NVMe: restore original resource start/end/flags */

	pci_dev_res_free_list(fail_head); /* NVMe: free all resource tracker nodes */
} /* NVMe: end NVMe PCIe host setup code block */

/*
 * First try will not touch PCI bridge res.
 * Second and later try will clear small leaf bridge res.
 * Will stop till to the max depth if can not find good one.
 */
void pci_assign_unassigned_root_bus_resources(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	LIST_HEAD(realloc_head); /* NVMe: NVMe PCIe host setup function call */
	/* List of resources that want additional resources */
	struct list_head *add_list = NULL; /* NVMe: NVMe PCIe host resource structure member/variable */
	int tried_times = 0; /* NVMe: integer variable, often index or return code */
	enum release_type rel_type = leaf_only; /* NVMe: enumeration variable */
	LIST_HEAD(fail_head); /* NVMe: NVMe PCIe host setup function call */
	int pci_try_num = 1; /* NVMe: integer variable, often index or return code */
	enum enable_type enable_local; /* NVMe: enumeration variable */

	/* Don't realloc if asked to do so */
	enable_local = pci_realloc_detect(bus, pci_realloc_enable); /* NVMe: detect whether resource reallocation is needed */
	if (pci_realloc_enabled(enable_local)) { /* NVMe: conditional check */
		int max_depth = pci_bus_get_depth(bus); /* NVMe: integer variable, often index or return code */

		pci_try_num = max_depth + 1; /* NVMe: set value for NVMe PCIe host resource setup */
		dev_info(&bus->dev, "max bus depth: %d pci_try_num: %d\n", /* NVMe: device info message */
			 max_depth, pci_try_num); /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	while (1) { /* NVMe: loop while condition holds */
		/*
		 * Last try will use add_list, otherwise will try good to
		 * have as must have, so can realloc parent bridge resource
		 */
		if (tried_times + 1 == pci_try_num) /* NVMe: conditional check */
			add_list = &realloc_head; /* NVMe: set value for NVMe PCIe host resource setup */
		/*
		 * Depth first, calculate sizes and alignments of all
		 * subordinate buses.
		 */
		__pci_bus_size_bridges(bus, add_list); /* NVMe: calculate required bridge window sizes */

		pci_root_bus_distribute_available_resources(bus, add_list); /* NVMe: distribute from root bus */

		/* Depth last, allocate resources and update the hardware. */
		__pci_bus_assign_resources(bus, add_list, &fail_head); /* NVMe: assign resources to PCI bus hierarchy */
		if (WARN_ON_ONCE(add_list && !list_empty(add_list))) /* NVMe: conditional check */
			pci_dev_res_free_list(add_list); /* NVMe: free all resource tracker nodes */
		tried_times++; /* NVMe: NVMe PCIe host resource setup statement */

		/* Any device complain? */
		if (list_empty(&fail_head)) /* NVMe: conditional check */
			break; /* NVMe: exit switch/loop */

		if (tried_times >= pci_try_num) { /* NVMe: conditional check */
			if (enable_local == undefined) { /* NVMe: conditional check */
				dev_info(&bus->dev, /* NVMe: device info message */
					 "Some PCI device resources are unassigned, try booting with pci=realloc\n"); /* NVMe: set value for NVMe PCIe host resource setup */
			} else if (enable_local == auto_enabled) { /* NVMe: NVMe PCIe host setup function call/header */
				dev_info(&bus->dev, /* NVMe: device info message */
					 "Automatically enabled pci realloc, if you have problem, try booting with pci=realloc=off\n"); /* NVMe: set value for NVMe PCIe host resource setup */
			} /* NVMe: end NVMe PCIe host setup code block */
			pci_dev_res_free_list(&fail_head); /* NVMe: free all resource tracker nodes */
			break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */

		/* Third times and later will not check if it is leaf */
		if (tried_times + 1 > 2) /* NVMe: conditional check */
			rel_type = whole_subtree; /* NVMe: set value for NVMe PCIe host resource setup */

		pci_prepare_next_assign_round(&fail_head, tried_times, rel_type); /* NVMe: prepare another allocation attempt */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_bus_dump_resources(bus); /* NVMe: dump bus resource tree */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_assign_unassigned_resources(void) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *root_bus; /* NVMe: NVMe PCIe host resource structure member/variable */

	list_for_each_entry(root_bus, &pci_root_buses, node) { /* NVMe: iterate over a linked list of resources/devices */
		pci_assign_unassigned_root_bus_resources(root_bus); /* NVMe: top-level resource assignment for root bus */

		/* Make sure the root bridge has a companion ACPI device */
		if (ACPI_HANDLE(root_bus->bridge)) /* NVMe: conditional check */
			acpi_ioapic_add(ACPI_HANDLE(root_bus->bridge)); /* NVMe: register IOAPIC ACPI companion */
	} /* NVMe: end NVMe PCIe host setup code block */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_assign_unassigned_bridge_resources(struct pci_dev *bridge) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_bus *parent = bridge->subordinate; /* NVMe: NVMe PCIe host resource structure member/variable */
	/* List of resources that want additional resources */
	LIST_HEAD(add_list); /* NVMe: NVMe PCIe host setup function call */
	int tried_times = 0; /* NVMe: integer variable, often index or return code */
	LIST_HEAD(fail_head); /* NVMe: NVMe PCIe host setup function call */
	int ret; /* NVMe: integer variable, often index or return code */

	while (1) { /* NVMe: loop while condition holds */
		__pci_bus_size_bridges(parent, &add_list); /* NVMe: calculate required bridge window sizes */

		/*
		 * Distribute remaining resources (if any) equally between
		 * hotplug bridges below. This makes it possible to extend
		 * the hierarchy later without running out of resources.
		 */
		pci_bridge_distribute_available_resources(bridge, &add_list); /* NVMe: start distribution from hotplug bridge */

		__pci_bridge_assign_resources(bridge, &add_list, &fail_head); /* NVMe: assign one device's BARs in order */
		if (WARN_ON_ONCE(!list_empty(&add_list))) /* NVMe: conditional check */
			pci_dev_res_free_list(&add_list); /* NVMe: free all resource tracker nodes */
		tried_times++; /* NVMe: NVMe PCIe host resource setup statement */

		if (list_empty(&fail_head)) /* NVMe: conditional check */
			break; /* NVMe: exit switch/loop */

		if (tried_times >= 2) { /* NVMe: conditional check */
			/* Still fail, don't need to try more */
			pci_dev_res_free_list(&fail_head); /* NVMe: free all resource tracker nodes */
			break; /* NVMe: exit switch/loop */
		} /* NVMe: end NVMe PCIe host setup code block */

		pci_prepare_next_assign_round(&fail_head, tried_times, /* NVMe: prepare another allocation attempt */
					      whole_subtree); /* NVMe: NVMe PCIe host resource setup statement */
	} /* NVMe: end NVMe PCIe host setup code block */

	ret = pci_reenable_device(bridge); /* NVMe: re-enable bridge after resource changes */
	if (ret) /* NVMe: conditional check */
		pci_err(bridge, "Error reenabling bridge (%d)\n", ret); /* NVMe: emit error message */
	pci_set_master(bridge); /* NVMe: enable PCI bus mastering on the bridge */
} /* NVMe: end NVMe PCIe host setup code block */
EXPORT_SYMBOL_GPL(pci_assign_unassigned_bridge_resources); /* NVMe: export symbol for module use */

/*
 * Walk to the root bus, find the bridge window relevant for @res and
 * release it when possible. If the bridge window contains assigned
 * resources, it cannot be released.
 */
static int pbus_reassign_bridge_resources(struct pci_bus *bus, struct resource *res, /* NVMe: function declaration/definition */
					  struct list_head *saved) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	unsigned long type = res->flags; /* NVMe: unsigned integer variable */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev *bridge = NULL; /* NVMe: NVMe PCIe host resource structure member/variable */
	LIST_HEAD(added); /* NVMe: NVMe PCIe host setup function call */
	LIST_HEAD(failed); /* NVMe: NVMe PCIe host setup function call */
	unsigned int i; /* NVMe: unsigned integer variable */
	int ret = 0; /* NVMe: integer variable, often index or return code */

	while (!pci_is_root_bus(bus)) { /* NVMe: loop while condition holds */
		bridge = bus->self; /* NVMe: access PCI device/resource member */
		res = pbus_select_window(bus, res); /* NVMe: select parent bridge window for this resource type */
		if (!res) /* NVMe: conditional check */
			break; /* NVMe: exit switch/loop */

		i = pci_resource_num(bridge, res); /* NVMe: convert resource pointer to BAR/bridge index */

		/* Ignore BARs which are still in use */
		if (!res->child) { /* NVMe: conditional check */
			ret = pci_dev_res_add_to_list(saved, bridge, res, 0, 0); /* NVMe: track resource for allocation or reallocation */
			if (ret) /* NVMe: conditional check */
				return ret; /* NVMe: return result */

			pci_release_resource(bridge, i); /* NVMe: release a BAR or bridge window back to parent */
		} else { /* NVMe: NVMe PCIe host setup operation */
			const char *res_name = pci_resource_name(bridge, i); /* NVMe: resource name string for log messages */

			pci_warn(bridge, /* NVMe: emit warning message about resource issue */
				 "%s %pR: was not released (still contains assigned resources)\n", /* NVMe: continue NVMe PCIe host setup argument list */
				 res_name, res); /* NVMe: NVMe PCIe host resource setup statement */
		} /* NVMe: end NVMe PCIe host setup code block */

		bus = bus->parent; /* NVMe: access PCI device/resource member */
	} /* NVMe: end NVMe PCIe host setup code block */

	if (!bridge) /* NVMe: conditional check */
		return -ENOENT; /* NVMe: return error code */

	__pci_bus_size_bridges(bridge->subordinate, &added); /* NVMe: calculate required bridge window sizes */
	__pci_bridge_assign_resources(bridge, &added, &failed); /* NVMe: assign one device's BARs in order */
	if (WARN_ON_ONCE(!list_empty(&added))) /* NVMe: conditional check */
		pci_dev_res_free_list(&added); /* NVMe: free all resource tracker nodes */

	if (!list_empty(&failed)) { /* NVMe: conditional check */
		if (pci_required_resource_failed(&failed, type)) /* NVMe: conditional check */
			ret = -ENOSPC; /* NVMe: set value for NVMe PCIe host resource setup */
		pci_dev_res_free_list(&failed); /* NVMe: free all resource tracker nodes */
		if (ret) /* NVMe: conditional check */
			return ret; /* NVMe: return result */

		/* Only resources with unrelated types failed (again) */
	} /* NVMe: end NVMe PCIe host setup code block */

	list_for_each_entry(dev_res, saved, list) { /* NVMe: iterate over a linked list of resources/devices */
		struct pci_dev *dev = dev_res->dev; /* NVMe: NVMe PCIe host resource structure member/variable */

		/* Skip the bridge we just assigned resources for */
		if (bridge == dev) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (!dev->subordinate) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		pci_setup_bridge(dev->subordinate); /* NVMe: write bridge base/limit registers to hardware */
	} /* NVMe: end NVMe PCIe host setup code block */

	return 0; /* NVMe: success */
} /* NVMe: end NVMe PCIe host setup code block */

int pci_do_resource_release_and_resize(struct pci_dev *pdev, int resno, int size, /* NVMe: function declaration/definition */
				       int exclude_bars) /* NVMe: NVMe PCIe host setup operation */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct resource *res = pci_resource_n(pdev, resno); /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_dev_resource *dev_res; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct pci_bus *bus = pdev->bus; /* NVMe: NVMe PCIe host resource structure member/variable */
	struct resource *b_win, *r; /* NVMe: NVMe PCIe host resource structure member/variable */
	LIST_HEAD(saved); /* NVMe: NVMe PCIe host setup function call */
	unsigned int i; /* NVMe: unsigned integer variable */
	int old, ret; /* NVMe: integer variable, often index or return code */

	b_win = pbus_select_window(bus, res); /* NVMe: select parent bridge window for this resource type */
	if (!b_win) /* NVMe: conditional check */
		return -EINVAL; /* NVMe: return error code */

	old = pci_rebar_get_current_size(pdev, resno); /* NVMe: read current Resizable BAR size */
	if (old < 0) /* NVMe: conditional check */
		return old; /* NVMe: return result */

	ret = pci_rebar_set_size(pdev, resno, size); /* NVMe: program Resizable BAR size */
	if (ret) /* NVMe: conditional check */
		return ret; /* NVMe: return result */

	pci_dev_for_each_resource(pdev, r, i) { /* NVMe: iterate over device BARs and bridge windows */
		if (i >= PCI_BRIDGE_RESOURCES) /* NVMe: conditional check */
			break; /* NVMe: exit switch/loop */

		if (exclude_bars & BIT(i)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (b_win != pbus_select_window(bus, r)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		ret = pci_dev_res_add_to_list(&saved, pdev, r, 0, 0); /* NVMe: track resource for allocation or reallocation */
		if (ret) /* NVMe: conditional check */
			goto restore; /* NVMe: jump to cleanup/error label */
		pci_release_resource(pdev, i); /* NVMe: release a BAR or bridge window back to parent */
	} /* NVMe: end NVMe PCIe host setup code block */

	pci_resize_resource_set_size(pdev, resno, size); /* NVMe: set resource size */

	if (!bus->self) /* NVMe: conditional check */
		goto out; /* NVMe: jump to cleanup/error label */

	down_read(&pci_bus_sem); /* NVMe: NVMe PCIe host setup function call */
	ret = pbus_reassign_bridge_resources(bus, res, &saved); /* NVMe: reassign bridge resources from leaf upward */
	if (ret) /* NVMe: conditional check */
		goto restore; /* NVMe: jump to cleanup/error label */

out: /* NVMe: cleanup/error label */
	up_read(&pci_bus_sem); /* NVMe: NVMe PCIe host setup function call */
	pci_dev_res_free_list(&saved); /* NVMe: free all resource tracker nodes */
	return ret; /* NVMe: return result */

restore: /* NVMe: cleanup/error label */
	/*
	 * Revert to the old configuration.
	 *
	 * BAR Size must be restored first because it affects the read-only
	 * bits in BAR (the old address might not be restorable otherwise
	 * due to low address bits).
	 */
	pci_rebar_set_size(pdev, resno, old); /* NVMe: program Resizable BAR size */

	list_for_each_entry(dev_res, &saved, list) { /* NVMe: iterate over a linked list of resources/devices */
		struct resource *res = dev_res->res; /* NVMe: NVMe PCIe host resource structure member/variable */
		struct pci_dev *dev = dev_res->dev; /* NVMe: NVMe PCIe host resource structure member/variable */

		i = pci_resource_num(dev, res); /* NVMe: convert resource pointer to BAR/bridge index */

		if (resource_assigned(res)) { /* NVMe: conditional check */
			release_child_resources(res); /* NVMe: release all child resources from a window */
			pci_release_resource(dev, i); /* NVMe: release a BAR or bridge window back to parent */
		} /* NVMe: end NVMe PCIe host setup code block */

		pci_dev_res_restore(dev_res); /* NVMe: restore original resource start/end/flags */

		if (pci_claim_resource(dev, i)) /* NVMe: conditional check */
			continue; /* NVMe: next loop iteration */

		if (i < PCI_BRIDGE_RESOURCES) { /* NVMe: conditional check */
			const char *res_name = pci_resource_name(dev, i); /* NVMe: resource name string for log messages */

			pci_update_resource(dev, i); /* NVMe: update hardware BAR register */
			pci_info(dev, "%s %pR: old value restored\n", /* NVMe: emit informational message */
				 res_name, res); /* NVMe: NVMe PCIe host resource setup statement */
		} /* NVMe: end NVMe PCIe host setup code block */
		if (dev->subordinate) /* NVMe: conditional check */
			pci_setup_bridge(dev->subordinate); /* NVMe: write bridge base/limit registers to hardware */
	} /* NVMe: end NVMe PCIe host setup code block */
	goto out; /* NVMe: jump to cleanup/error label */
} /* NVMe: end NVMe PCIe host setup code block */

void pci_assign_unassigned_bus_resources(struct pci_bus *bus) /* NVMe: function declaration/definition */
{ /* NVMe: begin NVMe PCIe host setup code block */
	struct pci_dev *dev; /* NVMe: NVMe PCIe host resource structure member/variable */
	/* List of resources that want additional resources */
	LIST_HEAD(add_list); /* NVMe: NVMe PCIe host setup function call */

	down_read(&pci_bus_sem); /* NVMe: NVMe PCIe host setup function call */
	for_each_pci_bridge(dev, bus) /* NVMe: NVMe PCIe host setup function call/header */
		if (pci_has_subordinate(dev)) /* NVMe: conditional check */
			__pci_bus_size_bridges(dev->subordinate, &add_list); /* NVMe: calculate required bridge window sizes */
	up_read(&pci_bus_sem); /* NVMe: NVMe PCIe host setup function call */
	__pci_bus_assign_resources(bus, &add_list, NULL); /* NVMe: assign resources to PCI bus hierarchy */
	if (WARN_ON_ONCE(!list_empty(&add_list))) /* NVMe: conditional check */
		pci_dev_res_free_list(&add_list); /* NVMe: free all resource tracker nodes */
} /* NVMe: end NVMe PCIe host setup code block */
EXPORT_SYMBOL_GPL(pci_assign_unassigned_bus_resources); /* NVMe: export symbol for module use */
