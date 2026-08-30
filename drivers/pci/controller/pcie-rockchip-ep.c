// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip AXI PCIe endpoint controller driver
 *
 * Copyright (c) 2018 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *         Simon Xue <xxm@rock-chips.com>
 */

#include <linux/configfs.h>	/* NVMe: configfs support for endpoint function configuration. */
#include <linux/delay.h>	/* NVMe: udelay/mdelay used during link training and legacy INTx pulse timing. */
#include <linux/gpio/consumer.h>	/* PCI/NVMe: PERST# GPIO handling; PERST# resets the device during host hotplug/re-enumeration. */
#include <linux/iopoll.h>	/* PCI/NVMe: readl_poll_timeout waits for link training before host can access config space. */
#include <linux/kernel.h>	/* NVMe: kernel helpers (fls64, max_t, etc.) used for BAR/ATU sizing. */
#include <linux/irq.h>	/* PCI/NVMe: IRQ types for PERST# interrupt and legacy INTx message signaling. */
#include <linux/of.h>	/* NVMe: device-tree parsing for clocks, resets, PHY and memory resources. */
#include <linux/pci-epc.h>	/* PCI/NVMe: PCIe Endpoint Controller framework; bridge between EP function driver and host. */
#include <linux/platform_device.h>	/* NVMe: platform device/driver registration. */
#include <linux/pci-epf.h>	/* PCI/NVMe: Endpoint Function interface; an NVMe endpoint function driver binds here. */
#include <linux/sizes.h>	/* NVMe: SZ_1M defines outbound region granularity for host DMA windows. */
#include <linux/workqueue.h>	/* NVMe: delayed work for asynchronous link training after PERST# deassert. */

#include "pcie-rockchip.h"	/* PCI/NVMe: Rockchip register and config-space definitions visible to host during enumeration. */

/**
 * struct rockchip_pcie_ep - private data for PCIe endpoint controller driver
 * @rockchip: Rockchip PCIe controller
 * @epc: PCI EPC device
 * @max_regions: maximum number of regions supported by hardware
 * @ob_region_map: bitmask of mapped outbound regions
 * @ob_addr: base addresses in the AXI bus where the outbound regions start
 * @irq_phys_addr: base address on the AXI bus where the MSI/INTX IRQ
 *		   dedicated outbound regions is mapped.
 * @irq_cpu_addr: base address in the CPU space where a write access triggers
 *		  the sending of a memory write (MSI) / normal message (INTX
 *		  IRQ) TLP through the PCIe bus.
 * @irq_pci_addr: used to save the current mapping of the MSI/INTX IRQ
 *		  dedicated outbound region.
 * @irq_pci_fn: the latest PCI function that has updated the mapping of
 *		the MSI/INTX IRQ dedicated outbound region.
 * @irq_pending: bitmask of asserted INTX IRQs.
 * @perst_irq: IRQ used for the PERST# signal.
 * @perst_asserted: True if the PERST# signal was asserted.
 * @link_up: True if the PCI link is up.
 * @link_training: Work item to execute PCI link training.
 */
struct rockchip_pcie_ep {	/* PCI/NVMe: Host NVMe driver sees this endpoint as a PCIe device; fields below describe config/BAR/MSI/INTx/link state. */
	struct rockchip_pcie	rockchip;	/* NVMe: PHY/clock/link controller state; host observes resulting link status via PCI_EXP_LNKSTA. */
	struct pci_epc		*epc;	/* NVMe: EPC device used by endpoint function driver. */
	u32			max_regions;	/* NVMe: number of outbound ATU regions; limits distinct host DMA windows. */
	unsigned long		ob_region_map;	/* NVMe: allocation bitmask for outbound regions. */
	phys_addr_t		*ob_addr;	/* NVMe: AXI base address array for each allocated OB region. */
	phys_addr_t		irq_phys_addr;	/* PCI/NVMe: AXI base of dedicated OB region used to send MSI Memory Write TLPs. */
	void __iomem		*irq_cpu_addr;	/* PCI/NVMe: CPU alias for MSI region; a write here raises a host MSI interrupt. */
	u64			irq_pci_addr;	/* PCI/NVMe: current MSI Message Address programmed by host via config space. */
	u8			irq_pci_fn;	/* NVMe: function whose MSI outbound mapping is currently programmed. */
	u8			irq_pending;	/* PCI/NVMe: bitmask of asserted legacy INTx lines. */
	int			perst_irq;	/* PCI/NVMe: Linux IRQ for PERST# GPIO hotplug/reset events. */
	bool			perst_asserted;	/* PCI/NVMe: true while host asserts PERST#; device is in reset. */
	bool			link_up;	/* NVMe: true once link is up and function driver has been notified. */
	struct delayed_work	link_training;	/* NVMe: delayed work carrying out LTSSM link training. */
};

static void rockchip_pcie_clear_ep_ob_atu(struct rockchip_pcie *rockchip,
					  u32 region)
{	/* NVMe: invalidate one outbound ATU region before reuse. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0(region));	/* NVMe: clear lower 32-bit target PCI address. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR1(region));	/* NVMe: clear upper 32-bit PCI address. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC0(region));	/* NVMe: clear descriptor0 (devfn/TLP type). */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC1(region));	/* NVMe: clear descriptor1. */
}

static int rockchip_pcie_ep_ob_atu_num_bits(struct rockchip_pcie *rockchip,
					    u64 pci_addr, size_t size)
{	/* NVMe: calculate ATU address-pass bits for a host DMA window. */
	int num_pass_bits = fls64(pci_addr ^ (pci_addr + size - 1));	/* NVMe: find highest differing bit between start and end-1. */

	return clamp(num_pass_bits,
		     ROCKCHIP_PCIE_AT_MIN_NUM_BITS,
		     ROCKCHIP_PCIE_AT_MAX_NUM_BITS);	/* NVMe: clamp between 8 and 20 bits (256B..1MB). */
}

static void rockchip_pcie_prog_ep_ob_atu(struct rockchip_pcie *rockchip, u8 fn,
					 u32 r, u64 cpu_addr, u64 pci_addr,
					 size_t size)
{	/* NVMe: program one outbound ATU translation entry. */
	int num_pass_bits;	/* NVMe: ATU register scratch values. */
	u32 addr0, addr1, desc0;

	num_pass_bits = rockchip_pcie_ep_ob_atu_num_bits(rockchip,
							 pci_addr, size);	/* NVMe: derive number of address bits passed unchanged. */

	addr0 = ((num_pass_bits - 1) & PCIE_CORE_OB_REGION_ADDR0_NUM_BITS) |	/* NVMe: encode pass-bits and lower PCI bus address. */
		(lower_32_bits(pci_addr) & PCIE_CORE_OB_REGION_ADDR0_LO_ADDR);
	addr1 = upper_32_bits(pci_addr);	/* NVMe: upper 32 bits of target bus address. */
	desc0 = ROCKCHIP_PCIE_AT_OB_REGION_DESC0_DEVFN(fn) | AXI_WRAPPER_MEM_WRITE;	/* NVMe: target devfn and Memory Write TLP type for host-side access. */

	/* PCI bus address region */
	rockchip_pcie_write(rockchip, addr0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0(r));	/* NVMe: write ATU PCI address low register. */
	rockchip_pcie_write(rockchip, addr1,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR1(r));	/* NVMe: write ATU PCI address high register. */
	rockchip_pcie_write(rockchip, desc0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC0(r));	/* NVMe: write ATU descriptor register. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC1(r));	/* NVMe: descriptor1 unused for simple memory mapping. */
}

static int rockchip_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
					 struct pci_epf_header *hdr)
{	/* PCI/NVMe: program Type0 config header seen by host during enumeration. */
	u32 reg;
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);	/* NVMe: recover EP private data from EPC. */
	struct rockchip_pcie *rockchip = &ep->rockchip;	/* NVMe: register access shortcut. */

	/* All functions share the same vendor ID with function 0 */
	if (fn == 0) {	/* PCI/NVMe: function 0 owns vendor/subsystem vendor IDs. */
		rockchip_pcie_write(rockchip,
				    hdr->vendorid | hdr->subsys_vendor_id << 16,
				    PCIE_CORE_CONFIG_VENDOR);	/* PCI/NVMe: write Vendor ID and Subsystem Vendor ID. */
	}

	reg = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_DID_VID);	/* PCI/NVMe: read existing VID/DID register. */
	reg = (reg & 0xFFFF) | (hdr->deviceid << 16);	/* PCI/NVMe: keep VID, set Device ID for host pci_match_id(). */
	rockchip_pcie_write(rockchip, reg, PCIE_EP_CONFIG_DID_VID);	/* PCI/NVMe: commit VID/DID to config space. */

	rockchip_pcie_write(rockchip,
			    hdr->revid |
			    hdr->progif_code << 8 |
			    hdr->subclass_code << 16 |
			    hdr->baseclass_code << 24,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) + PCI_REVISION_ID);	/* PCI/NVMe: set Revision/ProgIF/Subclass/Baseclass; host uses class code to detect NVMe. */
	rockchip_pcie_write(rockchip, hdr->cache_line_size,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_CACHE_LINE_SIZE);	/* PCI/NVMe: cache line size for host DMA alignment. */
	rockchip_pcie_write(rockchip, hdr->subsys_id << 16,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_SUBSYSTEM_VENDOR_ID);	/* PCI/NVMe: subsystem ID. */
	rockchip_pcie_write(rockchip, hdr->interrupt_pin << 8,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_INTERRUPT_LINE);	/* PCI/NVMe: legacy interrupt pin (INTA#) for host INTx fallback. */

	return 0;
}

static int rockchip_pcie_ep_set_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				    struct pci_epf_bar *epf_bar)
{	/* PCI/NVMe: configure a BAR so host can map NVMe MMIO registers via pci_iomap(). */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);	/* NVMe: BAR physical base in endpoint memory. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	dma_addr_t bar_phys = epf_bar->phys_addr;
	enum pci_barno bar = epf_bar->barno;	/* PCI/NVMe: BAR index; NVMe controllers normally expose registers in BAR0. */
	int flags = epf_bar->flags;	/* NVMe: BAR attribute flags (MEM/IO, prefetch, 64-bit). */
	u32 addr0, addr1, reg, cfg, b, aperture, ctrl;
	u64 sz;

	/* BAR size is 2^(aperture + 7) */
	sz = max_t(size_t, epf_bar->size, MIN_EP_APERTURE);	/* NVMe: enforce minimum aperture size. */

	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values.
	 */
	sz = 1ULL << fls64(sz - 1);	/* NVMe: round up to power of two as required by PCI BAR decoding. */
	aperture = ilog2(sz) - 7; /* 128B -> 0, 256B -> 1, 512B -> 2, ... */	/* NVMe: encode size as aperture field (128B->0, 256B->1, ...). */

	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {	/* PCI/NVMe: handle I/O BAR type (rare for NVMe). */
		ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_IO_32BITS;
	} else {
		bool is_prefetch = !!(flags & PCI_BASE_ADDRESS_MEM_PREFETCH);	/* NVMe: decode prefetchable flag. */
		bool is_64bits = !!(flags & PCI_BASE_ADDRESS_MEM_TYPE_64);	/* NVMe: decode 64-bit BAR flag. */

		if (is_64bits && (bar & 1))	/* PCI/NVMe: 64-bit BARs must start at an even BAR index. */
			return -EINVAL;

		if (is_64bits && is_prefetch)	/* PCI/NVMe: prefetchable 64-bit memory BAR. */
			ctrl =
			    ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_64BITS;
		else if (is_prefetch)	/* PCI/NVMe: prefetchable 32-bit memory BAR. */
			ctrl =
			    ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_32BITS;
		else if (is_64bits)	/* PCI/NVMe: non-prefetchable 64-bit memory BAR. */
			ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_64BITS;
		else
			ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_32BITS;	/* PCI/NVMe: non-prefetchable 32-bit memory BAR. */
	}

	if (bar < BAR_4) {	/* PCI/NVMe: BARs 0-3 configured via BAR_CFG0 register. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG0(fn);
		b = bar;
	} else {
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG1(fn);	/* PCI/NVMe: BARs 4-5 configured via BAR_CFG1 register. */
		b = bar - BAR_4;
	}

	addr0 = lower_32_bits(bar_phys);	/* PCI/NVMe: lower 32 bits of BAR base for inbound ATU. */
	addr1 = upper_32_bits(bar_phys);	/* PCI/NVMe: upper 32 bits for 64-bit BAR. */

	cfg = rockchip_pcie_read(rockchip, reg);	/* NVMe: read current BAR configuration. */
	cfg &= ~(ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	cfg |= (ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE(b, aperture) |
		ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl));

	rockchip_pcie_write(rockchip, cfg, reg);
	rockchip_pcie_write(rockchip, addr0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar));
	rockchip_pcie_write(rockchip, addr1,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar));	/* PCI/NVMe: program inbound ATU so host pci_iomap() accesses land in EP memory. */

	return 0;
}

static void rockchip_pcie_ep_clear_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				       struct pci_epf_bar *epf_bar)
{	/* PCI/NVMe: disable a BAR and clear its inbound mapping. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u32 reg, cfg, b, ctrl;
	enum pci_barno bar = epf_bar->barno;

	if (bar < BAR_4) {	/* NVMe: select BAR config register. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG0(fn);
		b = bar;
	} else {
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG1(fn);
		b = bar - BAR_4;
	}

	ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_DISABLED;	/* NVMe: mark BAR as disabled. */
	cfg = rockchip_pcie_read(rockchip, reg);	/* NVMe: read current config. */
	cfg &= ~(ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	cfg |= ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl);

	rockchip_pcie_write(rockchip, cfg, reg);	/* PCI/NVMe: commit disabled BAR. */
	rockchip_pcie_write(rockchip, 0x0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar));
	rockchip_pcie_write(rockchip, 0x0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar));	/* PCI/NVMe: clear inbound ATU base address. */
}

static inline u32 rockchip_ob_region(phys_addr_t addr)	/* PCI/NVMe: 1MB region index from AXI address. */
{
	return (addr >> ilog2(SZ_1M)) & 0x1f;	/* NVMe: each OB region is 1 MiB, up to 32 regions. */
}

static u64 rockchip_pcie_ep_align_addr(struct pci_epc *epc, u64 pci_addr,
				       size_t *pci_size, size_t *addr_offset)
{	/* PCI/NVMe: align a host DMA target to ATU boundaries. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);	/* PCI/NVMe: recover EP private data. */
	size_t size = *pci_size;
	u64 offset, mask;
	int num_bits;

	num_bits = rockchip_pcie_ep_ob_atu_num_bits(&ep->rockchip,
						    pci_addr, size);	/* PCI/NVMe: compute address-pass bits. */
	mask = (1ULL << num_bits) - 1;	/* PCI/NVMe: mask for low bits that must be zero in aligned base. */

	offset = pci_addr & mask;	/* PCI/NVMe: offset of original address within aligned window. */
	if (size + offset > SZ_1M)	/* PCI/NVMe: clip request to stay inside the 1MB region. */
		size = SZ_1M - offset;

	*pci_size = ALIGN(offset + size, ROCKCHIP_PCIE_AT_SIZE_ALIGN);	/* PCI/NVMe: return aligned window size to caller. */
	*addr_offset = offset;	/* PCI/NVMe: return offset within aligned window. */

	return pci_addr & ~mask;	/* PCI/NVMe: return aligned PCI bus address. */
}

static int rockchip_pcie_ep_map_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				     phys_addr_t addr, u64 pci_addr,
				     size_t size)
{	/* PCI/NVMe: allocate and program an outbound DMA window. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *pcie = &ep->rockchip;
	u32 r = rockchip_ob_region(addr);	/* PCI/NVMe: select 1MB OB region by AXI base. */

	if (test_bit(r, &ep->ob_region_map))	/* PCI/NVMe: region already used by another mapping. */
		return -EBUSY;

	rockchip_pcie_prog_ep_ob_atu(pcie, fn, r, addr, pci_addr, size);	/* PCI/NVMe: program ATU for host memory access (NVMe PRP/SGL target). */

	set_bit(r, &ep->ob_region_map);	/* PCI/NVMe: mark region as allocated. */
	ep->ob_addr[r] = addr;	/* PCI/NVMe: store AXI base for exact unmap matching. */

	return 0;
}

static void rockchip_pcie_ep_unmap_addr(struct pci_epc *epc, u8 fn, u8 vfn,
					phys_addr_t addr)
{	/* PCI/NVMe: release an outbound DMA window. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u32 r = rockchip_ob_region(addr);	/* PCI/NVMe: region index from AXI base. */

	if (addr != ep->ob_addr[r] || !test_bit(r, &ep->ob_region_map))	/* PCI/NVMe: ignore unmap if region is not allocated or base mismatches. */
		return;

	rockchip_pcie_clear_ep_ob_atu(rockchip, r);	/* PCI/NVMe: invalidate ATU entry. */

	ep->ob_addr[r] = 0;	/* PCI/NVMe: clear stored base. */
	clear_bit(r, &ep->ob_region_map);	/* PCI/NVMe: mark region free. */
}

static int rockchip_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn,
				    u8 nr_irqs)
{	/* PCI/NVMe: configure MSI capability exposed to host. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u8 mmc = order_base_2(nr_irqs);	/* PCI/NVMe: Multiple Message Capable = log2(requested vectors). */
	u32 flags;

	flags = rockchip_pcie_read(rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);	/* PCI/NVMe: read current MSI capability register. */
	flags &= ~ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_MASK;	/* PCI/NVMe: clear previous MMC value. */
	flags |=
	   (mmc << ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_OFFSET) |
	   (PCI_MSI_FLAGS_64BIT << ROCKCHIP_PCIE_EP_MSI_FLAGS_OFFSET);	/* PCI/NVMe: set MMC and 64-bit capable flag for NVMe host. */
	flags &= ~ROCKCHIP_PCIE_EP_MSI_CTRL_MASK_MSI_CAP;	/* PCI/NVMe: unmask MSI capability so host can enable it. */
	rockchip_pcie_write(rockchip, flags,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    ROCKCHIP_PCIE_EP_MSI_CTRL_REG);	/* PCI/NVMe: commit MSI capability to config space. */
	return 0;
}

static int rockchip_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)	/* PCI/NVMe: return number of enabled MSI vectors. */
{
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u32 flags;

	flags = rockchip_pcie_read(rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);	/* PCI/NVMe: read MSI control register. */
	if (!(flags & ROCKCHIP_PCIE_EP_MSI_CTRL_ME))	/* PCI/NVMe: MSI Enable must be set by host. */
		return -EINVAL;

	return 1 << ((flags & ROCKCHIP_PCIE_EP_MSI_CTRL_MME_MASK) >>
		     ROCKCHIP_PCIE_EP_MSI_CTRL_MME_OFFSET);	/* PCI/NVMe: MME field gives log2(enabled vectors). */
}

static void rockchip_pcie_ep_assert_intx(struct rockchip_pcie_ep *ep, u8 fn,
					 u8 intx, bool do_assert)
{	/* PCI/NVMe: assert or deassert a legacy INTx message. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	intx &= 3;	/* PCI/NVMe: INTx number is 0..3 (A-D). */

	if (do_assert) {	/* PCI/NVMe: send Assert_INTx message TLP. */
		ep->irq_pending |= BIT(intx);	/* PCI/NVMe: record asserted line. */
		rockchip_pcie_write(rockchip,
				    PCIE_CLIENT_INT_IN_ASSERT |
				    PCIE_CLIENT_INT_PEND_ST_PEND,
				    PCIE_CLIENT_LEGACY_INT_CTRL);	/* PCI/NVMe: trigger controller to send assert message to host. */
	} else {
		ep->irq_pending &= ~BIT(intx);	/* PCI/NVMe: send Deassert_INTx message TLP. */
		rockchip_pcie_write(rockchip,
				    PCIE_CLIENT_INT_IN_DEASSERT |
				    PCIE_CLIENT_INT_PEND_ST_NORMAL,
				    PCIE_CLIENT_LEGACY_INT_CTRL);
	}
}

static int rockchip_pcie_ep_send_intx_irq(struct rockchip_pcie_ep *ep, u8 fn,
					  u8 intx)
{	/* PCI/NVMe: raise a legacy INTx interrupt toward the host. */
	u16 cmd;

	cmd = rockchip_pcie_read(&ep->rockchip,
				 ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				 ROCKCHIP_PCIE_EP_CMD_STATUS);	/* PCI/NVMe: read PCI_COMMAND to check INTx disable. */

	if (cmd & PCI_COMMAND_INTX_DISABLE)	/* PCI/NVMe: host masked INTx; do not signal. */
		return -EINVAL;

	/*
	 * Should add some delay between toggling INTx per TRM vaguely saying
	 * it depends on some cycles of the AHB bus clock to function it. So
	 * add sufficient 1ms here.
	 */
	rockchip_pcie_ep_assert_intx(ep, fn, intx, true);	/* PCI/NVMe: assert INTx line. */
	mdelay(1);	/* PCI/NVMe: 1ms pulse required by TRM. */
	rockchip_pcie_ep_assert_intx(ep, fn, intx, false);	/* PCI/NVMe: deassert INTx line. */
	return 0;
}

static int rockchip_pcie_ep_send_msi_irq(struct rockchip_pcie_ep *ep, u8 fn,
					 u8 interrupt_num)
{	/* PCI/NVMe: raise an MSI interrupt toward the host. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u32 flags, mme, data, data_mask;
	size_t irq_pci_size, offset;
	u64 irq_pci_addr;
	u8 msi_count;
	u64 pci_addr;

	/* Check MSI enable bit */
	flags = rockchip_pcie_read(&ep->rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);	/* PCI/NVMe: read MSI control/status register. */
	if (!(flags & ROCKCHIP_PCIE_EP_MSI_CTRL_ME))	/* PCI/NVMe: abort if MSI is not enabled by host. */
		return -EINVAL;

	/* Get MSI numbers from MME */
	mme = ((flags & ROCKCHIP_PCIE_EP_MSI_CTRL_MME_MASK) >>	/* PCI/NVMe: extract Multiple Message Enabled field. */
			ROCKCHIP_PCIE_EP_MSI_CTRL_MME_OFFSET);
	msi_count = 1 << mme;	/* PCI/NVMe: enabled vector count is 2^MME. */
	if (!interrupt_num || interrupt_num > msi_count)	/* PCI/NVMe: validate vector index against host allocation. */
		return -EINVAL;

	/* Set MSI private data */
	data_mask = msi_count - 1;	/* PCI/NVMe: mask data to MME width. */
	data = rockchip_pcie_read(rockchip,
				  ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				  ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				  PCI_MSI_DATA_64);	/* PCI/NVMe: read Message Data register written by host. */
	data = (data & ~data_mask) | ((interrupt_num - 1) & data_mask);	/* PCI/NVMe: merge vector index with host Message Data. */

	/* Get MSI PCI address */
	pci_addr = rockchip_pcie_read(rockchip,
				      ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				      ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				      PCI_MSI_ADDRESS_HI);
	pci_addr <<= 32;
	pci_addr |= rockchip_pcie_read(rockchip,
				       ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				       ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				       PCI_MSI_ADDRESS_LO);	/* PCI/NVMe: read 64-bit Message Address written by host. */

	/* Set the outbound region if needed. */
	irq_pci_size = ~PCIE_ADDR_MASK + 1;	/* PCI/NVMe: default MSI window size from address mask. */
	irq_pci_addr = rockchip_pcie_ep_align_addr(ep->epc,
						   pci_addr & PCIE_ADDR_MASK,
						   &irq_pci_size, &offset);	/* PCI/NVMe: align target address to ATU constraints. */
	if (unlikely(ep->irq_pci_addr != irq_pci_addr ||	/* PCI/NVMe: reprogram only when target address or function changes. */
		     ep->irq_pci_fn != fn)) {
		rockchip_pcie_prog_ep_ob_atu(rockchip, fn,
					rockchip_ob_region(ep->irq_phys_addr),
					ep->irq_phys_addr,
					irq_pci_addr, irq_pci_size);	/* PCI/NVMe: map OB region to host IRQ controller address. */
		ep->irq_pci_addr = irq_pci_addr;	/* PCI/NVMe: cache aligned target address. */
		ep->irq_pci_fn = fn;	/* PCI/NVMe: cache function number. */
	}

	writew(data, ep->irq_cpu_addr + offset + (pci_addr & ~PCIE_ADDR_MASK));	/* PCI/NVMe: Memory Write TLP carrying MSI data raises host interrupt. */
	return 0;
}

static int rockchip_pcie_ep_raise_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				      unsigned int type, u16 interrupt_num)
{	/* PCI/NVMe: dispatch interrupt request from endpoint function. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);

	switch (type) {	/* PCI/NVMe: choose legacy or MSI delivery. */
	case PCI_IRQ_INTX:	/* PCI/NVMe: only INTA# is implemented. */
		return rockchip_pcie_ep_send_intx_irq(ep, fn, 0);
	case PCI_IRQ_MSI:	/* PCI/NVMe: MSI vector index matches NVMe queue completion interrupt. */
		return rockchip_pcie_ep_send_msi_irq(ep, fn, interrupt_num);
	default:	/* PCI/NVMe: unsupported interrupt type. */
		return -EINVAL;
	}
}

static int rockchip_pcie_ep_start(struct pci_epc *epc)	/* PCI/NVMe: enable functions and start PCIe link for host enumeration. */
{
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	struct pci_epf *epf;
	u32 cfg;

	cfg = BIT(0);	/* PCI/NVMe: always enable function 0. */
	list_for_each_entry(epf, &epc->pci_epf, list)	/* PCI/NVMe: enable every registered endpoint function. */
		cfg |= BIT(epf->func_no);

	rockchip_pcie_write(rockchip, cfg, PCIE_CORE_PHY_FUNC_CFG);	/* PCI/NVMe: commit function-enable mask. */

	if (rockchip->perst_gpio)	/* PCI/NVMe: start listening for host PERST# events. */
		enable_irq(ep->perst_irq);

	/* Enable configuration and start link training */
	rockchip_pcie_write(rockchip,
			    PCIE_CLIENT_LINK_TRAIN_ENABLE |
			    PCIE_CLIENT_CONF_ENABLE,
			    PCIE_CLIENT_CONFIG);	/* PCI/NVMe: enable config space and LTSSM so host can enumerate. */

	if (!rockchip->perst_gpio)	/* PCI/NVMe: no PERST GPIO, begin link training immediately. */
		schedule_delayed_work(&ep->link_training, 0);

	return 0;
}

static void rockchip_pcie_ep_stop(struct pci_epc *epc)	/* PCI/NVMe: stop link and disable functions. */
{
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;

	if (rockchip->perst_gpio) {	/* PCI/NVMe: synchronize with possible PERST# handling. */
		ep->perst_asserted = true;
		disable_irq(ep->perst_irq);	/* PCI/NVMe: disable PERST# IRQ. */
	}

	cancel_delayed_work_sync(&ep->link_training);	/* PCI/NVMe: stop asynchronous link training. */

	/* Stop link training and disable configuration */
	rockchip_pcie_write(rockchip,
			    PCIE_CLIENT_CONF_DISABLE |
			    PCIE_CLIENT_LINK_TRAIN_DISABLE,
			    PCIE_CLIENT_CONFIG);	/* PCI/NVMe: disable config access and LTSSM; host sees link down. */
}

static void rockchip_pcie_ep_retrain_link(struct rockchip_pcie *rockchip)	/* PCI/NVMe: retrain PCIe link to a higher speed or after reset. */
{
	u32 status;

	status = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE + PCI_EXP_LNKCTL);	/* PCI/NVMe: read PCI_EXP_LNKCTL from EP config space. */
	status |= PCI_EXP_LNKCTL_RL;	/* PCI/NVMe: set Retrain Link bit. */
	rockchip_pcie_write(rockchip, status, PCIE_EP_CONFIG_BASE + PCI_EXP_LNKCTL);	/* PCI/NVMe: write LNKCTL; host will see link retrain in progress. */
}

static bool rockchip_pcie_ep_link_up(struct rockchip_pcie *rockchip)	/* PCI/NVMe: return true if PCIe link is in L0. */
{
	u32 val = rockchip_pcie_read(rockchip, PCIE_CLIENT_BASIC_STATUS1);	/* PCI/NVMe: read link status register. */

	return PCIE_LINK_UP(val);	/* PCI/NVMe: host can access config space only when link is up. */
}

static void rockchip_pcie_ep_link_training(struct work_struct *work)	/* PCI/NVMe: delayed work that brings the PCIe link up. */
{
	struct rockchip_pcie_ep *ep =
		container_of(work, struct rockchip_pcie_ep, link_training.work);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	struct device *dev = rockchip->dev;
	u32 val;
	int ret;

	/* Enable Gen1 training and wait for its completion */
	ret = readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,
				 val, PCIE_LINK_TRAINING_DONE(val), 50,
				 LINK_TRAIN_TIMEOUT);	/* PCI/NVMe: wait for link training done in CORE_CTRL. */
	if (ret)
		goto again;

	/* Make sure that the link is up */
	ret = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_BASIC_STATUS1,
				 val, PCIE_LINK_UP(val), 50,
				 LINK_TRAIN_TIMEOUT);	/* PCI/NVMe: wait for link status up in BASIC_STATUS1. */
	if (ret)
		goto again;

	/*
	 * Check the current speed: if gen2 speed was requested and we are not
	 * at gen2 speed yet, retrain again for gen2.
	 */
	val = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL);	/* PCI/NVMe: read current negotiated speed. */
	if (!PCIE_LINK_IS_GEN2(val) && rockchip->link_gen == 2) {	/* PCI/NVMe: if Gen2 requested but not reached, retrain. */
		/* Enable retrain for gen2 */
		rockchip_pcie_ep_retrain_link(rockchip);	/* PCI/NVMe: request Gen2 retrain. */
		readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,
				   val, PCIE_LINK_IS_GEN2(val), 50,
				   LINK_TRAIN_TIMEOUT);	/* PCI/NVMe: wait until Gen2 speed is reached. */
	}

	/* Check again that the link is up */
	if (!rockchip_pcie_ep_link_up(rockchip))	/* PCI/NVMe: final link-up check. */
		goto again;

	/*
	 * If PERST# was asserted while polling the link, do not notify
	 * the function.
	 */
	if (ep->perst_asserted)	/* PCI/NVMe: abort if PERST# asserted during training. */
		return;

	val = rockchip_pcie_read(rockchip, PCIE_CLIENT_BASIC_STATUS0);	/* PCI/NVMe: read negotiated speed/width. */
	dev_info(dev,
		 "link up (negotiated speed: %sGT/s, width: x%lu)\n",
		 (val & PCIE_CLIENT_NEG_LINK_SPEED) ? "5" : "2.5",
		 ((val & PCIE_CLIENT_NEG_LINK_WIDTH_MASK) >>
		  PCIE_CLIENT_NEG_LINK_WIDTH_SHIFT) << 1);	/* PCI/NVMe: print negotiated link parameters seen by host. */

	/* Notify the function */
	pci_epc_linkup(ep->epc);	/* PCI/NVMe: notify function drivers that host can now enumerate. */
	ep->link_up = true;	/* PCI/NVMe: mark link up. */

	return;

again:
	schedule_delayed_work(&ep->link_training, msecs_to_jiffies(5));	/* PCI/NVMe: retry link training after 5ms. */
}

static void rockchip_pcie_ep_perst_assert(struct rockchip_pcie_ep *ep)	/* PCI/NVMe: handle host asserting PERST# (device reset/hotplug remove). */
{
	struct rockchip_pcie *rockchip = &ep->rockchip;

	dev_dbg(rockchip->dev, "PERST# asserted, link down\n");	/* PCI/NVMe: log PERST# assertion. */

	if (ep->perst_asserted)	/* PCI/NVMe: ignore repeated assertions. */
		return;

	ep->perst_asserted = true;	/* PCI/NVMe: remember device is in reset. */

	cancel_delayed_work_sync(&ep->link_training);	/* PCI/NVMe: cancel pending link training. */

	if (ep->link_up) {	/* PCI/NVMe: if link was up, notify function drivers of removal. */
		pci_epc_linkdown(ep->epc);	/* PCI/NVMe: host will see the device disappear from the bus. */
		ep->link_up = false;	/* PCI/NVMe: clear link-up flag. */
	}
}

static void rockchip_pcie_ep_perst_deassert(struct rockchip_pcie_ep *ep)	/* PCI/NVMe: handle host releasing PERST# (end of reset). */
{
	struct rockchip_pcie *rockchip = &ep->rockchip;

	dev_dbg(rockchip->dev, "PERST# de-asserted, starting link training\n");	/* PCI/NVMe: log PERST# deassert. */

	if (!ep->perst_asserted)	/* PCI/NVMe: ignore if not previously asserted. */
		return;

	ep->perst_asserted = false;	/* PCI/NVMe: device is no longer in reset. */

	/* Enable link re-training */
	rockchip_pcie_ep_retrain_link(rockchip);	/* PCI/NVMe: retrain link so host can re-enumerate. */

	/* Start link training */
	schedule_delayed_work(&ep->link_training, 0);	/* PCI/NVMe: schedule link training immediately. */
}

static irqreturn_t rockchip_pcie_ep_perst_irq_thread(int irq, void *data)	/* PCI/NVMe: threaded IRQ handler for PERST# GPIO. */
{
	struct pci_epc *epc = data;	/* PCI/NVMe: recover EPC from handler data. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);	/* PCI/NVMe: recover EP private data. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	u32 perst = gpiod_get_value(rockchip->perst_gpio);	/* PCI/NVMe: logical PERST# level (1 = asserted for active-low GPIO). */

	if (perst)	/* PCI/NVMe: host asserted reset; handle as hotplug removal. */
		rockchip_pcie_ep_perst_assert(ep);
	else	/* PCI/NVMe: host released reset; retrain and re-enumerate. */
		rockchip_pcie_ep_perst_deassert(ep);

	irq_set_irq_type(ep->perst_irq,
			 (perst ? IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW));	/* PCI/NVMe: set trigger for next edge. */

	return IRQ_HANDLED;
}

static int rockchip_pcie_ep_setup_irq(struct pci_epc *epc)	/* PCI/NVMe: register PERST# GPIO IRQ for hotplug events. */
{
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	struct rockchip_pcie *rockchip = &ep->rockchip;
	struct device *dev = rockchip->dev;
	int ret;

	if (!rockchip->perst_gpio)	/* PCI/NVMe: no PERST# wired; hotplug handled entirely by host. */
		return 0;

	/* PCIe reset interrupt */
	ep->perst_irq = gpiod_to_irq(rockchip->perst_gpio);	/* PCI/NVMe: get Linux IRQ number for PERST# GPIO. */
	if (ep->perst_irq < 0) {	/* PCI/NVMe: propagate gpiod_to_irq error. */
		dev_err(dev,
			"failed to get IRQ for PERST# GPIO: %d\n",
			ep->perst_irq);

		return ep->perst_irq;
	}

	/*
	 * The perst_gpio is active low, so when it is inactive on start, it
	 * is high and will trigger the perst_irq handler. So treat this initial
	 * IRQ as a dummy one by faking the host asserting PERST#.
	 */
	ep->perst_asserted = true;	/* PCI/NVMe: assume PERST# asserted at probe to avoid early enumeration. */
	irq_set_status_flags(ep->perst_irq, IRQ_NOAUTOEN);	/* PCI/NVMe: keep IRQ disabled until endpoint is started. */
	ret = devm_request_threaded_irq(dev, ep->perst_irq, NULL,
					rockchip_pcie_ep_perst_irq_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"pcie-ep-perst", epc);	/* PCI/NVMe: request threaded IRQ for slow PERST# signal. */
	if (ret) {	/* PCI/NVMe: propagate request_threaded_irq error. */
		dev_err(dev,
			"failed to request IRQ for PERST# GPIO: %d\n",
			ret);

		return ret;
	}

	return 0;
}

static const struct pci_epc_features rockchip_pcie_epc_features = {	/* PCI/NVMe: static feature descriptor reported to endpoint function driver. */
	.linkup_notifier = true,
	.msi_capable = true,
	.intx_capable = true,
	.align = ROCKCHIP_PCIE_AT_SIZE_ALIGN,
};	/* PCI/NVMe: EPC notifies function driver on link up/down. */	/* PCI/NVMe: controller supports MSI interrupts. */

static const struct pci_epc_features*	/* PCI/NVMe: return static feature descriptor for a function. */
rockchip_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{	/* PCI/NVMe: controller supports legacy INTx interrupts. */	/* PCI/NVMe: outbound address alignment requirement (256B). */
	return &rockchip_pcie_epc_features;	/* PCI/NVMe: all functions share the same capabilities. */
}

static const struct pci_epc_ops rockchip_pcie_epc_ops = {	/* PCI/NVMe: EPC operation vector used by the endpoint framework. */
	.write_header	= rockchip_pcie_ep_write_header,
	.set_bar	= rockchip_pcie_ep_set_bar,
	.clear_bar	= rockchip_pcie_ep_clear_bar,
	.align_addr	= rockchip_pcie_ep_align_addr,
	.map_addr	= rockchip_pcie_ep_map_addr,
	.unmap_addr	= rockchip_pcie_ep_unmap_addr,
	.set_msi	= rockchip_pcie_ep_set_msi,
	.get_msi	= rockchip_pcie_ep_get_msi,
	.raise_irq	= rockchip_pcie_ep_raise_irq,
	.start		= rockchip_pcie_ep_start,
	.stop		= rockchip_pcie_ep_stop,
	.get_features	= rockchip_pcie_ep_get_features,
};	/* PCI/NVMe: write Type0 config header fields. */	/* PCI/NVMe: configure a BAR. */

static int rockchip_pcie_ep_get_resources(struct rockchip_pcie *rockchip,
					  struct rockchip_pcie_ep *ep)
{	/* PCI/NVMe: disable a BAR. */	/* PCI/NVMe: align OB DMA target address. */
	struct device *dev = rockchip->dev;	/* PCI/NVMe: map an outbound DMA window. */	/* PCI/NVMe: unmap an outbound DMA window. */
	int err;	/* PCI/NVMe: set MSI capability. */	/* PCI/NVMe: get enabled MSI vector count. */

	err = rockchip_pcie_parse_dt(rockchip);	/* PCI/NVMe: raise an interrupt toward host. */	/* PCI/NVMe: start controller and link. */	/* PCI/NVMe: parse clocks/resets/PHY/memory from DT. */
	if (err)
		return err;	/* PCI/NVMe: stop controller and link. */	/* PCI/NVMe: report features. */

	err = rockchip_pcie_get_phys(rockchip);	/* PCI/NVMe: parse DT and initialize controller resources. */	/* PCI/NVMe: acquire PHY instances. */
	if (err)
		return err;

	err = of_property_read_u32(dev->of_node,
				   "rockchip,max-outbound-regions",
				   &ep->max_regions);
	if (err < 0 || ep->max_regions > MAX_REGION_LIMIT)
		ep->max_regions = MAX_REGION_LIMIT;	/* PCI/NVMe: limit outbound ATU regions to hardware maximum (32). */

	ep->ob_region_map = 0;	/* PCI/NVMe: no OB regions allocated yet. */

	err = of_property_read_u8(dev->of_node, "max-functions",
				  &ep->epc->max_functions);
	if (err < 0)
		ep->epc->max_functions = 1;	/* PCI/NVMe: default to single function unless DT says otherwise. */

	return 0;
}

static const struct of_device_id rockchip_pcie_ep_of_match[] = {	/* PCI/NVMe: device-tree compatible table. */
	{ .compatible = "rockchip,rk3399-pcie-ep"},
	{},
};	/* PCI/NVMe: match Rockchip RK3399 PCIe endpoint controller. */

static int rockchip_pcie_ep_init_ob_mem(struct rockchip_pcie_ep *ep)	/* PCI/NVMe: initialize outbound memory windows and MSI region. */
{
	struct rockchip_pcie *rockchip = &ep->rockchip;
	struct device *dev = rockchip->dev;
	struct pci_epc_mem_window *windows = NULL;
	int err, i;

	ep->ob_addr = devm_kcalloc(dev, ep->max_regions, sizeof(*ep->ob_addr),
				   GFP_KERNEL);	/* PCI/NVMe: allocate per-region AXI base tracking array. */

	if (!ep->ob_addr)
		return -ENOMEM;

	windows = devm_kcalloc(dev, ep->max_regions,
			       sizeof(struct pci_epc_mem_window), GFP_KERNEL);	/* PCI/NVMe: allocate window descriptor array. */
	if (!windows)
		return -ENOMEM;

	for (i = 0; i < ep->max_regions; i++) {	/* PCI/NVMe: fill 1MB OB windows from the controller memory resource. */
		windows[i].phys_base = rockchip->mem_res->start + (SZ_1M * i);	/* PCI/NVMe: physical base of each 1MB window. */
		windows[i].size = SZ_1M;	/* PCI/NVMe: each window is 1 MiB. */
		windows[i].page_size = SZ_1M;	/* PCI/NVMe: EPC allocator uses 1MB pages. */
	}
	err = pci_epc_multi_mem_init(ep->epc, windows, ep->max_regions);	/* PCI/NVMe: register memory windows with EPC framework. */
	devm_kfree(dev, windows);	/* PCI/NVMe: free temporary descriptor array after init. */

	if (err < 0) {	/* PCI/NVMe: propagate EPC memory init error. */
		dev_err(dev, "failed to initialize the memory space\n");
		return err;
	}

	ep->irq_cpu_addr = pci_epc_mem_alloc_addr(ep->epc, &ep->irq_phys_addr,
						  SZ_1M);	/* PCI/NVMe: reserve 1MB OB window for MSI Memory Writes. */
	if (!ep->irq_cpu_addr) {	/* PCI/NVMe: failed to reserve MSI window. */
		dev_err(dev, "failed to reserve memory space for MSI\n");
		err = -ENOMEM;
		goto err_epc_mem_exit;
	}

	ep->irq_pci_addr = ROCKCHIP_PCIE_EP_DUMMY_IRQ_ADDR;	/* PCI/NVMe: initialize with invalid sentinel until host programs MSI address. */

	return 0;

err_epc_mem_exit:
	pci_epc_mem_exit(ep->epc);	/* PCI/NVMe: clean up EPC memory windows on error. */

	return err;
}

static void rockchip_pcie_ep_exit_ob_mem(struct rockchip_pcie_ep *ep)	/* PCI/NVMe: release all EPC memory windows. */
{
	pci_epc_mem_exit(ep->epc);	/* PCI/NVMe: undo pci_epc_multi_mem_init. */
}

static void rockchip_pcie_ep_hide_broken_msix_cap(struct rockchip_pcie *rockchip)	/* PCI/NVMe: remove MSI-X capability from linked list so host does not use it. */
{
	u32 cfg_msi, cfg_msix_cp;

	/*
	 * MSI-X is not supported but the controller still advertises the MSI-X
	 * capability by default, which can lead to the Root Complex side
	 * allocating MSI-X vectors which cannot be used. Avoid this by skipping
	 * the MSI-X capability entry in the PCIe capabilities linked-list: get
	 * the next pointer from the MSI-X entry and set that in the MSI
	 * capability entry (which is the previous entry). This way the MSI-X
	 * entry is skipped (left out of the linked-list) and not advertised.
	 */
	cfg_msi = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE +	/* PCI/NVMe: read MSI capability register including next-pointer. */
				     ROCKCHIP_PCIE_EP_MSI_CTRL_REG);

	cfg_msi &= ~ROCKCHIP_PCIE_EP_MSI_CP1_MASK;	/* PCI/NVMe: clear next-pointer field of MSI capability. */

	cfg_msix_cp = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE +
					 ROCKCHIP_PCIE_EP_MSIX_CAP_REG) &
					 ROCKCHIP_PCIE_EP_MSIX_CAP_CP_MASK;	/* PCI/NVMe: read next-pointer from MSI-X capability. */

	cfg_msi |= cfg_msix_cp;	/* PCI/NVMe: point MSI cap to the capability after MSI-X, skipping MSI-X. */

	rockchip_pcie_write(rockchip, cfg_msi,
			    PCIE_EP_CONFIG_BASE + ROCKCHIP_PCIE_EP_MSI_CTRL_REG);	/* PCI/NVMe: write back updated MSI capability; MSI-X is now hidden. */
}

static int rockchip_pcie_ep_probe(struct platform_device *pdev)	/* PCI/NVMe: platform probe: create EPC and initialize hardware. */
{
	struct device *dev = &pdev->dev;
	struct rockchip_pcie_ep *ep;
	struct rockchip_pcie *rockchip;
	struct pci_epc *epc;
	int err;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);	/* PCI/NVMe: allocate and zero EP private data. */
	if (!ep)
		return -ENOMEM;

	rockchip = &ep->rockchip;	/* PCI/NVMe: initialize controller sub-structure. */
	rockchip->is_rc = false;	/* PCI/NVMe: this probe runs for endpoint mode, not root complex. */
	rockchip->dev = dev;	/* PCI/NVMe: store device pointer for logging/DMA. */
	INIT_DELAYED_WORK(&ep->link_training, rockchip_pcie_ep_link_training);	/* PCI/NVMe: initialize link training delayed work. */

	epc = devm_pci_epc_create(dev, &rockchip_pcie_epc_ops);	/* PCI/NVMe: create and register EPC device with the framework. */
	if (IS_ERR(epc)) {	/* PCI/NVMe: handle EPC creation failure. */
		dev_err(dev, "failed to create EPC device\n");
		return PTR_ERR(epc);
	}

	ep->epc = epc;	/* PCI/NVMe: link EPC back to EP private data. */
	epc_set_drvdata(epc, ep);	/* PCI/NVMe: allow epc_get_drvdata() to retrieve EP data. */

	err = rockchip_pcie_ep_get_resources(rockchip, ep);	/* PCI/NVMe: parse DT resources. */
	if (err)
		return err;

	err = rockchip_pcie_ep_init_ob_mem(ep);	/* PCI/NVMe: initialize outbound memory/MSI windows. */
	if (err)
		return err;

	err = rockchip_pcie_enable_clocks(rockchip);	/* PCI/NVMe: enable controller clocks. */
	if (err)
		goto err_exit_ob_mem;

	err = rockchip_pcie_init_port(rockchip);	/* PCI/NVMe: initialize PHY and controller port. */
	if (err)
		goto err_disable_clocks;

	rockchip_pcie_ep_hide_broken_msix_cap(rockchip);	/* PCI/NVMe: hide unsupported MSI-X capability before host enumeration. */

	/* Only enable function 0 by default */
	rockchip_pcie_write(rockchip, BIT(0), PCIE_CORE_PHY_FUNC_CFG);	/* PCI/NVMe: only function 0 enabled by default. */

	pci_epc_init_notify(epc);	/* PCI/NVMe: notify bound function drivers that EPC is ready. */

	err = rockchip_pcie_ep_setup_irq(epc);	/* PCI/NVMe: register PERST# IRQ for hotplug events. */
	if (err < 0)
		goto err_uninit_port;

	return 0;
err_uninit_port:	/* PCI/NVMe: error cleanup path. */
	rockchip_pcie_deinit_phys(rockchip);	/* PCI/NVMe: undo PHY initialization. */
err_disable_clocks:
	rockchip_pcie_disable_clocks(rockchip);	/* PCI/NVMe: disable clocks. */
err_exit_ob_mem:
	rockchip_pcie_ep_exit_ob_mem(ep);	/* PCI/NVMe: release outbound memory windows. */
	return err;
}

static struct platform_driver rockchip_pcie_ep_driver = {	/* PCI/NVMe: platform driver structure. */
	.driver = {
		.name = "rockchip-pcie-ep",
		.of_match_table = rockchip_pcie_ep_of_match,
	},
	.probe = rockchip_pcie_ep_probe,
};	/* PCI/NVMe: driver name used for sysfs/platform matching. */	/* PCI/NVMe: DT compatible matching. */

builtin_platform_driver(rockchip_pcie_ep_driver);	/* PCI/NVMe: probe entry point. */	/* PCI/NVMe: register driver at built-in init time. */
