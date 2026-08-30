// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */
/* NVMe: R-Car PCIe host controller helper layer used by the PCI core before
 *       an NVMe SSD (or any PCIe endpoint) is enumerated and bound to
 *       drivers/nvme/host/pci.c.
 */

#include <linux/delay.h>	/* NVMe: msleep()/udelay() during link training
				 *       before the NVMe endpoint is reachable.
				 */
#include <linux/pci.h>		/* PCI/NVMe: core PCI infrastructure that also
				 *          probes nvme_pci_probe() once an
				 *          NVMe function is discovered.
				 */

#include "pcie-rcar.h"		/* NVMe: register offsets and struct rcar_pcie
				 *       shared with the platform-specific
				 *       R-Car host controller driver.
				 */

void rcar_pci_write_reg(struct rcar_pcie *pcie, u32 val, unsigned int reg)
{
	/* PCI/NVMe: raw MMIO write to the R-Car host controller register.
	 *           Used later to program outbound windows that expose the
	 *           NVMe SSD's BAR0/1 (doorbells/CQ/SQ registers) to the CPU.
	 */
	writel(val, pcie->base + reg);
}

u32 rcar_pci_read_reg(struct rcar_pcie *pcie, unsigned int reg)
{
	/* PCI/NVMe: raw MMIO read from the host controller register.
	 *           Poll results tell the PCI core whether the NVMe link is
	 *           up, PHY is ready, or an AER/UE event occurred.
	 */
	return readl(pcie->base + reg);
}

void rcar_rmw32(struct rcar_pcie *pcie, int where, u32 mask, u32 data)
{
	unsigned int shift = BITS_PER_BYTE * (where & 3);	/* NVMe: byte lane within the 32-bit
							 *       aligned config register.
							 */
	u32 val = rcar_pci_read_reg(pcie, where & ~3);	/* PCI/NVMe: fetch the whole 32-bit dword
							 *          containing the requested
							 *          PCIe config offset.
							 */

	val &= ~(mask << shift);	/* NVMe: clear only the target bit field
					 *       (e.g. PCI_COMMAND memory-enable).
					 */
	val |= data << shift;		/* NVMe: set the new value
					 *       (e.g. enable NVMe MSI/MSI-X).
					 */
	rcar_pci_write_reg(pcie, val, where & ~3);	/* PCI/NVMe: write back; affects NVMe
							 *          enumeration registers such
							 *          as BAR, CMD, MSI_CTRL.
							 */
}

int rcar_pcie_wait_for_phyrdy(struct rcar_pcie *pcie)
{
	unsigned int timeout = 10;	/* NVMe: up to 50 ms for PHY ready;
					 *       until then no NVMe config/Tlp
					 *       traffic can flow.
					 */

	while (timeout--) {		/* NVMe: poll loop before NVMe endpoint
					 *       link training can start.
					 */
		if (rcar_pci_read_reg(pcie, PCIEPHYSR) & PHYRDY)
			return 0;	/* NVMe: PHY lanes stable; PCI core may
					 *       now enumerate the NVMe SSD.
					 */

		msleep(5);		/* NVMe: back off between PHY status
					 *       polls.
					 */
	}

	return -ETIMEDOUT;		/* NVMe: PHY never came up; NVMe probe
					 *       will fail because config cycles
					 *       cannot be completed.
					 */
}

int rcar_pcie_wait_for_dl(struct rcar_pcie *pcie)
{
	unsigned int timeout = 10000;	/* NVMe: up to ~50 ms waiting for data
					 *       link layer; NVMe TLPs are
					 *       dropped until this is set.
					 */

	while (timeout--) {		/* NVMe: poll data-link-active status
					 *       before scanning the NVMe bus.
					 */
		if ((rcar_pci_read_reg(pcie, PCIETSTR) & DATA_LINK_ACTIVE))
			return 0;	/* NVMe: link layer active; config
					 *       reads/writes to the NVMe
					 *       endpoint will now complete.
					 */

		udelay(5);		/* NVMe: short delay; link training
					 *       finishes quickly once started.
					 */
		cpu_relax();		/* NVMe: hint to the CPU while spinning
					 *       on link state.
					 */
	}

	return -ETIMEDOUT;		/* NVMe: data link never active; the
					 *       PCI core cannot see the NVMe
					 *       device and nvme_pci_probe()
					 *       will not be called.
					 */
}

void rcar_pcie_set_outbound(struct rcar_pcie *pcie, int win,
			    struct resource_entry *window)
{
	/* Setup PCIe address space mappings for each resource */
	struct resource *res = window->res;	/* NVMe: resource describes the CPU-side
						 *       view of a PCI MMIO/IO window.
						 */
	resource_size_t res_start;		/* NVMe: CPU physical base of the
						 *       outbound window.
						 */
	resource_size_t size;			/* NVMe: window size determines the
						 *       address-mask granularity.
						 */
	u32 mask;

	rcar_pci_write_reg(pcie, 0x00000000, PCIEPTCTLR(win));
	/* PCI/NVMe: disable the outbound window before reprogramming it;
	 *           stale translations could route NVMe MMIO accesses to
	 *           the wrong PCI address.
	 */

	/*
	 * The PAMR mask is calculated in units of 128Bytes, which
	 * keeps things pretty simple.
	 */
	size = resource_size(res);		/* NVMe: total size of this outbound
						 *       aperture.
						 */
	if (size > 128)
		mask = (roundup_pow_of_two(size) / SZ_128) - 1;
	/* NVMe: 128-byte granularity mask; larger windows get a bigger
	 *       mask so the whole NVMe BAR region is translated.
	 */
	else
		mask = 0x0;			/* NVMe: minimum 128-byte window. */
	rcar_pci_write_reg(pcie, mask << 7, PCIEPAMR(win));
	/* PCI/NVMe: program the PCIe address mask; together with PALR/PAUR
	 *           it decides which PCI addresses hit this outbound window
	 *           and therefore reach the NVMe device's BARs.
	 */

	if (res->flags & IORESOURCE_IO)
		res_start = pci_pio_to_address(res->start) - window->offset;
	/* PCI/NVMe: PIO resources need conversion from legacy ISA-style I/O
	 *           numbers to the CPU physical address used by the host.
	 */
	else
		res_start = res->start - window->offset;
	/* PCI/NVMe: MMIO resources are already CPU physical; subtract the
	 *           resource-entry offset to get the window base.
	 */

	rcar_pci_write_reg(pcie, upper_32_bits(res_start), PCIEPAUR(win));
	/* PCI/NVMe: high 32 bits of the PCIe outbound start address;
	 *           required for 64-bit NVMe BAR1 mappings.
	 */
	rcar_pci_write_reg(pcie, lower_32_bits(res_start) & ~0x7F,
			   PCIEPALR(win));
	/* PCI/NVMe: low 32 bits, aligned to 128-byte boundary; BAR0/1 of an
	 *           NVMe SSD is mapped through this translated PCI address.
	 */

	/* First resource is for IO */
	mask = PAR_ENABLE;			/* NVMe: enable the outbound ATU
						 *       window.
						 */
	if (res->flags & IORESOURCE_IO)
		mask |= IO_SPACE;		/* PCI/NVMe: mark as I/O space so
						 *          legacy INTx/PIO accesses
						 *          are routed correctly.
						 */

	rcar_pci_write_reg(pcie, mask, PCIEPTCTLR(win));
	/* PCI/NVMe: commit the window; after this, CPU accesses inside
	 *           res_start..res_start+size are translated to the PCI
	 *           bus and can reach the NVMe SSD's BAR registers.
	 */
}

void rcar_pcie_set_inbound(struct rcar_pcie *pcie, u64 cpu_addr,
			   u64 pci_addr, u64 flags, int idx, bool host)
{
	/*
	 * Set up 64-bit inbound regions as the range parser doesn't
	 * distinguish between 32 and 64-bit types.
	 */
	if (host)
		rcar_pci_write_reg(pcie, lower_32_bits(pci_addr),
				   PCIEPRAR(idx));
	/* PCI/NVMe: on the root port, program the low 32 bits of the PCI
	 *           address seen by the NVMe device for inbound DMA; NVMe
	 *           PRP/SGL entries reference this bus address.
	 */
	rcar_pci_write_reg(pcie, lower_32_bits(cpu_addr), PCIELAR(idx));
	/* PCI/NVMe: low 32 bits of the CPU physical address that inbound
	 *           NVMe DMA (reads/writes from the SSD) lands on.
	 */
	rcar_pci_write_reg(pcie, flags, PCIELAMR(idx));
	/* PCI/NVMe: inbound attributes (prefetch, 64-bit, enable); must
	 *           match the DMA mask advertised by the NVMe host driver.
	 */

	if (host)
		rcar_pci_write_reg(pcie, upper_32_bits(pci_addr),
				   PCIEPRAR(idx + 1));
	/* PCI/NVMe: high 32 bits of the PCI bus address for 64-bit NVMe
	 *           DMA above 4 GiB.
	 */
	rcar_pci_write_reg(pcie, upper_32_bits(cpu_addr), PCIELAR(idx + 1));
	/* PCI/NVMe: high 32 bits of the CPU physical target for 64-bit
	 *           inbound NVMe DMA above 4 GiB.
	 */
	rcar_pci_write_reg(pcie, 0, PCIELAMR(idx + 1));
	/* PCI/NVMe: no extra attributes on the upper half of a 64-bit
	 *           inbound mapping; the lower half's flags control the
	 *           whole region used for NVMe DMA.
	 */
}
