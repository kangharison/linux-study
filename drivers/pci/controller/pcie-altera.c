// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright Altera Corporation (C) 2013-2015. All rights reserved
 *
 * Author: Ley Foon Tan <lftan@altera.com>
 * Description: Altera PCIe host controller driver
 */

#include <linux/bitfield.h>	/* PCI/NVMe: FIELD_PREP/FIELD_GET for TLP/target encoding */
#include <linux/delay.h>	/* PCI/NVMe: udelay() for TLP polling while scanning NVMe */
#include <linux/interrupt.h>	/* PCI/NVMe: legacy/INTx path shared with NVMe fallback */
#include <linux/irqchip/chained_irq.h>	/* PCI/NVMe: chained irqchip for root-port -> INTx -> NVMe */
#include <linux/irqdomain.h>	/* PCI/NVMe: domain that maps legacy INTA..D for NVMe */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>		/* PCI/NVMe: OF parse of root port that hosts NVMe */
#include <linux/of_pci.h>	/* PCI/NVMe: pci_irqd_intx_xlate for legacy pins */
#include <linux/pci.h>		/* PCI/NVMe: pci_host_bridge, pci_ops, PCI config space */
#include <linux/platform_device.h>	/* PCI/NVMe: platform probe of the host controller */
#include <linux/slab.h>

#include "../pci.h"		/* PCI/NVMe: internal PCI headers, pci_host_probe helpers */

#define RP_TX_REG0			0x2000	/* PCI/NVMe: TLP TX register 0 (header/data) */
#define RP_TX_REG1			0x2004	/* PCI/NVMe: TLP TX register 1 (header/data) */
#define RP_TX_CNTRL			0x2008	/* PCI/NVMe: TLP TX SOP/EOP control */
#define RP_TX_EOP			0x2	/* PCI/NVMe: end-of-packet flag for config TLP */
#define RP_TX_SOP			0x1	/* PCI/NVMe: start-of-packet flag for config TLP */
#define RP_RXCPL_STATUS			0x2010	/* PCI/NVMe: completion status / SOP/EOP */
#define RP_RXCPL_EOP			0x2	/* PCI/NVMe: completion EOP detected */
#define RP_RXCPL_SOP			0x1	/* PCI/NVMe: completion SOP detected */
#define RP_RXCPL_REG0			0x2014	/* PCI/NVMe: completion data low */
#define RP_RXCPL_REG1			0x2018	/* PCI/NVMe: completion data high / status */
#define P2A_INT_STATUS			0x3060	/* PCI/NVMe: root-port legacy INTx status */
#define P2A_INT_STS_ALL			0xf	/* PCI/NVMe: INTA..INTD bits */
#define P2A_INT_ENABLE			0x3070	/* PCI/NVMe: root-port legacy INTx enable */
#define P2A_INT_ENA_ALL			0xf	/* PCI/NVMe: enable INTA..INTD */
#define RP_LTSSM			0x3c64	/* PCI/NVMe: LTSSM state register */
#define RP_LTSSM_MASK			0x1f	/* PCI/NVMe: LTSSM field mask */
#define LTSSM_L0			0xf	/* PCI/NVMe: L0 = link active, required for NVMe probe */

#define S10_RP_TX_CNTRL			0x2004	/* PCI/NVMe: Stratix10 TLP TX control offset */
#define S10_RP_RXCPL_REG		0x2008	/* PCI/NVMe: Stratix10 completion data register */
#define S10_RP_RXCPL_STATUS		0x200C	/* PCI/NVMe: Stratix10 completion status */
#define S10_RP_CFG_ADDR(pcie, reg)	/* PCI/NVMe: access root-port config space via BAR aperture */ \
	(((pcie)->hip_base) + (reg) + (1 << 20))
#define S10_RP_SECONDARY(pcie)		/* PCI/NVMe: secondary bus number used in Type1 routing */ \
	readb(S10_RP_CFG_ADDR(pcie, PCI_SECONDARY_BUS))

/* TLP configuration type 0 and 1 */
#define TLP_FMTTYPE_CFGRD0		0x04	/* PCI/NVMe: Config Read Type 0 (same bus as root) */
#define TLP_FMTTYPE_CFGWR0		0x44	/* PCI/NVMe: Config Write Type 0 (same bus as root) */
#define TLP_FMTTYPE_CFGRD1		0x05	/* PCI/NVMe: Config Read Type 1 (downstream/subordinate bus) */
#define TLP_FMTTYPE_CFGWR1		0x45	/* PCI/NVMe: Config Write Type 1 (downstream/subordinate bus) */
#define TLP_PAYLOAD_SIZE		0x01	/* PCI/NVMe: one DW payload for config TLPs */
#define TLP_READ_TAG			0x1d	/* PCI/NVMe: tag used for config read completions */
#define TLP_WRITE_TAG			0x10	/* PCI/NVMe: tag used for config write completions */
#define RP_DEVFN			0	/* PCI/NVMe: root port is always dev 0 fn 0 */
#define TLP_CFG_DW0(pcie, cfg)		/* PCI/NVMe: DW0: fmt/type + payload size for config TLP */ \
			(((cfg) << 24) |	\
			  TLP_PAYLOAD_SIZE)
#define TLP_CFG_DW1(pcie, tag, be)	/* PCI/NVMe: DW1: requester ID (root bus/dev/fn), tag, byte-enables */ \
	(((PCI_DEVID(pcie->root_bus_nr,  RP_DEVFN)) << 16) | (tag << 8) | (be))
#define TLP_CFG_DW2(bus, devfn, offset)	/* PCI/NVMe: DW2: target bus/dev/fn and register offset */ \
				(((bus) << 24) | ((devfn) << 16) | (offset))
#define TLP_COMP_STATUS(s)		(((s) >> 13) & 7)	/* PCI/NVMe: completion status field (CA/UR/CRS) */
#define TLP_BYTE_COUNT(s)		(((s) >> 0) & 0xfff)	/* PCI/NVMe: byte count returned in completion */
#define TLP_HDR_SIZE			3	/* PCI/NVMe: 3 DW config TLP header */
#define TLP_LOOP			500	/* PCI/NVMe: poll up to 2.5 ms for config completion */

#define LINK_UP_TIMEOUT			HZ	/* PCI/NVMe: wait up to 1s for link up before NVMe scan */
#define LINK_RETRAIN_TIMEOUT		HZ	/* PCI/NVMe: wait up to 1s for retrain before NVMe scan */

#define DWORD_MASK			3	/* PCI/NVMe: lower 2 bits of offset must be clear for DW access */

#define S10_TLP_FMTTYPE_CFGRD0		0x05	/* PCI/NVMe: Stratix10 Type0 config read (swapped encoding) */
#define S10_TLP_FMTTYPE_CFGRD1		0x04	/* PCI/NVMe: Stratix10 Type1 config read (swapped encoding) */
#define S10_TLP_FMTTYPE_CFGWR0		0x45	/* PCI/NVMe: Stratix10 Type0 config write (swapped encoding) */
#define S10_TLP_FMTTYPE_CFGWR1		0x44	/* PCI/NVMe: Stratix10 Type1 config write (swapped encoding) */

#define AGLX_RP_CFG_ADDR(pcie, reg)	(((pcie)->hip_base) + (reg))	/* PCI/NVMe: Agilex direct root-port config access */
#define AGLX_RP_SECONDARY(pcie)		/* PCI/NVMe: Agilex secondary bus number */ \
	readb(AGLX_RP_CFG_ADDR(pcie, PCI_SECONDARY_BUS))

#define AGLX_BDF_REG			0x00002004	/* PCI/NVMe: Agilex BDF selector for downstream config */
#define AGLX_ROOT_PORT_IRQ_STATUS	0x14c	/* PCI/NVMe: Agilex root-port IRQ status */
#define AGLX_ROOT_PORT_IRQ_ENABLE	0x150	/* PCI/NVMe: Agilex root-port IRQ enable */
#define CFG_AER				BIT(4)	/* PCI/NVMe: AER event interrupt enable/status for NVMe AER handling */

#define AGLX_CFG_TARGET			GENMASK(13, 12)	/* PCI/NVMe: target type field in config address */
#define AGLX_CFG_TARGET_TYPE0		0	/* PCI/NVMe: Type0 config access (target bus) */
#define AGLX_CFG_TARGET_TYPE1		1	/* PCI/NVMe: Type1 config access (forward beyond secondary) */
#define AGLX_CFG_TARGET_LOCAL_2000	2	/* PCI/NVMe: local config window at 0x2000 */
#define AGLX_CFG_TARGET_LOCAL_3000	3	/* PCI/NVMe: local config window at 0x3000 */

enum altera_pcie_version {
	ALTERA_PCIE_V1 = 0,	/* PCI/NVMe: original Altera PCIe, TLP-based config */
	ALTERA_PCIE_V2,		/* PCI/NVMe: Stratix10, TLP-based + direct RP config */
	ALTERA_PCIE_V3,		/* PCI/NVMe: Agilex, direct config + AER irq path */
};

struct altera_pcie {
	struct platform_device	*pdev;		/* PCI/NVMe: platform device backing the host bridge */
	void __iomem		*cra_base;	/* PCI/NVMe: CRA register window for TLP/legacy IRQ control */
	void __iomem		*hip_base;	/* PCI/NVMe: HIP register window for RP config (S10/Agilex) */
	int			irq;		/* PCI/NVMe: root port wired IRQ -> legacy INTA/AER events */
	u8			root_bus_nr;	/* PCI/NVMe: bus number assigned to root port (NVMe bus) */
	struct irq_domain	*irq_domain;	/* PCI/NVMe: INTx domain used when NVMe falls back to legacy IRQ */
	struct resource		bus_range;	/* PCI/NVMe: bus number resource published to PCI core */
	const struct altera_pcie_data	*pcie_data;	/* PCI/NVMe: per-variant ops and offsets */
};

struct altera_pcie_ops {
	int (*tlp_read_pkt)(struct altera_pcie *pcie, u32 *value);		/* PCI/NVMe: read config completion returned by NVMe/EP */
	void (*tlp_write_pkt)(struct altera_pcie *pcie, u32 *headers,		/* PCI/NVMe: emit config request TLP toward NVMe/EP */
			      u32 data, bool align);
	bool (*get_link_status)(struct altera_pcie *pcie);			/* PCI/NVMe: DLL link active, gate NVMe access */
	int (*rp_read_cfg)(struct altera_pcie *pcie, int where,			/* PCI/NVMe: read root-port own config (used during NVMe enum) */
			   int size, u32 *value);
	int (*rp_write_cfg)(struct altera_pcie *pcie, u8 busno,			/* PCI/NVMe: write root-port config */
			    int where, int size, u32 value);
	int (*ep_read_cfg)(struct altera_pcie *pcie, u8 busno,			/* PCI/NVMe: read downstream NVMe/EP config space */
			   unsigned int devfn, int where, int size, u32 *value);
	int (*ep_write_cfg)(struct altera_pcie *pcie, u8 busno,			/* PCI/NVMe: write downstream NVMe/EP config space */
			    unsigned int devfn, int where, int size, u32 value);
	void (*rp_isr)(struct irq_desc *desc);					/* PCI/NVMe: root-port ISR, dispatches NVMe legacy/AER IRQs */
};

struct altera_pcie_data {
	const struct altera_pcie_ops *ops;	/* PCI/NVMe: variant-specific callbacks */
	enum altera_pcie_version version;	/* PCI/NVMe: IP version (affects TLP encoding) */
	u32 cap_offset;		/* PCIe capability structure register offset */	/* PCI/NVMe: offset of PCIe cap in root-port config */
	u32 cfgrd0;					/* PCI/NVMe: Type0 config read fmt/type for this IP */
	u32 cfgrd1;					/* PCI/NVMe: Type1 config read fmt/type for this IP */
	u32 cfgwr0;					/* PCI/NVMe: Type0 config write fmt/type for this IP */
	u32 cfgwr1;					/* PCI/NVMe: Type1 config write fmt/type for this IP */
	u32 port_conf_offset;				/* PCI/NVMe: Agilex port config block base */
	u32 port_irq_status_offset;			/* PCI/NVMe: Agilex port IRQ status offset */
	u32 port_irq_enable_offset;			/* PCI/NVMe: Agilex port IRQ enable offset */
};

struct tlp_rp_regpair_t {
	u32 ctrl;	/* PCI/NVMe: TX control (SOP/EOP) */
	u32 reg0;	/* PCI/NVMe: TX data/header low */
	u32 reg1;	/* PCI/NVMe: TX data/header high */
};

static inline void cra_writel(struct altera_pcie *pcie, const u32 value,		/* PCI/NVMe: 32-bit relaxed CRA write, e.g. TLP TX for NVMe config */
			      const u32 reg)
{
	writel_relaxed(value, pcie->cra_base + reg);	/* PCI/NVMe: write to CRA aperture used for legacy/TLP control */
}

static inline u32 cra_readl(struct altera_pcie *pcie, const u32 reg)		/* PCI/NVMe: 32-bit relaxed CRA read, e.g. TLP completion for NVMe */
{
	return readl_relaxed(pcie->cra_base + reg);	/* PCI/NVMe: read CRA aperture */
}

static inline void cra_writew(struct altera_pcie *pcie, const u32 value,		/* PCI/NVMe: 16-bit CRA write for Agilex config window */
			      const u32 reg)
{
	writew_relaxed(value, pcie->cra_base + reg);	/* PCI/NVMe: write 16 bits to CRA aperture */
}

static inline u32 cra_readw(struct altera_pcie *pcie, const u32 reg)		/* PCI/NVMe: 16-bit CRA read for Agilex config window */
{
	return readw_relaxed(pcie->cra_base + reg);	/* PCI/NVMe: read 16 bits from CRA aperture */
}

static inline void cra_writeb(struct altera_pcie *pcie, const u32 value,		/* PCI/NVMe: 8-bit CRA write for Agilex config window */
			      const u32 reg)
{
	writeb_relaxed(value, pcie->cra_base + reg);	/* PCI/NVMe: write 8 bits to CRA aperture */
}

static inline u32 cra_readb(struct altera_pcie *pcie, const u32 reg)		/* PCI/NVMe: 8-bit CRA read for Agilex config window */
{
	return readb_relaxed(pcie->cra_base + reg);	/* PCI/NVMe: read 8 bits from CRA aperture */
}

static bool altera_pcie_link_up(struct altera_pcie *pcie)			/* PCI/NVMe: check LTSSM L0 before touching NVMe config space */
{
	return !!((cra_readl(pcie, RP_LTSSM) & RP_LTSSM_MASK) == LTSSM_L0);	/* PCI/NVMe: link must be L0 to enumerate/bind NVMe */
}

static bool s10_altera_pcie_link_up(struct altera_pcie *pcie)			/* PCI/NVMe: Stratix10 link status via PCI_EXP_LNKSTA_DLLLA */
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie,				/* PCI/NVMe: root-port PCIe capability address */
				   pcie->pcie_data->cap_offset +
				   PCI_EXP_LNKSTA);

	return !!(readw(addr) & PCI_EXP_LNKSTA_DLLLA);	/* PCI/NVMe: DLL link active = NVMe reachable */
}

static bool aglx_altera_pcie_link_up(struct altera_pcie *pcie)			/* PCI/NVMe: Agilex link status via PCI_EXP_LNKSTA_DLLLA */
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie,				/* PCI/NVMe: root-port PCIe capability address */
				   pcie->pcie_data->cap_offset +
				   PCI_EXP_LNKSTA);

	return (readw_relaxed(addr) & PCI_EXP_LNKSTA_DLLLA);	/* PCI/NVMe: DLLLA set when NVMe training done */
}

/*
 * Altera PCIe port uses BAR0 of RC's configuration space as the translation
 * from PCI bus to native BUS.  Entire DDR region is mapped into PCIe space
 * using these registers, so it can be reached by DMA from EP devices.
 * This BAR0 will also access to MSI vector when receiving MSI/MSI-X interrupt
 * from EP devices, eventually trigger interrupt to GIC.  The BAR0 of bridge
 * should be hidden during enumeration to avoid the sizing and resource
 * allocation by PCIe core.
 */
static bool altera_pcie_hide_rc_bar(struct pci_bus *bus, unsigned int  devfn,		/* PCI/NVMe: hide RC BAR0 so NVMe DMA aperture is not sized as MMIO */
				    int offset)
{
	if (pci_is_root_bus(bus) && (devfn == 0) &&	/* PCI/NVMe: only root port dev 0 fn 0 */
	    (offset == PCI_BASE_ADDRESS_0))		/* PCI/NVMe: only BAR0, used for inbound DMA/MSI translation */
		return true;	/* PCI/NVMe: pretend BAR0 does not exist to PCI core */

	return false;	/* PCI/NVMe: expose all other config offsets normally */
}

static void tlp_write_tx(struct altera_pcie *pcie,				/* PCI/NVMe: push one TLP beat toward NVMe/EP */
			 struct tlp_rp_regpair_t *tlp_rp_regdata)
{
	cra_writel(pcie, tlp_rp_regdata->reg0, RP_TX_REG0);	/* PCI/NVMe: header/data DW0 */
	cra_writel(pcie, tlp_rp_regdata->reg1, RP_TX_REG1);	/* PCI/NVMe: header/data DW1 */
	cra_writel(pcie, tlp_rp_regdata->ctrl, RP_TX_CNTRL);	/* PCI/NVMe: SOP/EOP control beat */
}

static void s10_tlp_write_tx(struct altera_pcie *pcie, u32 reg0, u32 ctrl)	/* PCI/NVMe: Stratix10 single-beat TLP TX */
{
	cra_writel(pcie, reg0, RP_TX_REG0);	/* PCI/NVMe: header/data DW */
	cra_writel(pcie, ctrl, S10_RP_TX_CNTRL);	/* PCI/NVMe: Stratix10 control beat */
}

static bool altera_pcie_valid_device(struct altera_pcie *pcie,			/* PCI/NVMe: decide whether a config cycle may reach NVMe */
				     struct pci_bus *bus, int dev)
{
	/* If there is no link, then there is no device */
	if (bus->number != pcie->root_bus_nr) {	/* PCI/NVMe: downstream bus accesses depend on trained link */
		if (!pcie->pcie_data->ops->get_link_status(pcie))	/* PCI/NVMe: if link down, NVMe not present */
			return false;	/* PCI/NVMe: suppress config cycles to absent NVMe */
	}

	/* access only one slot on each root port */
	if (bus->number == pcie->root_bus_nr && dev > 0)	/* PCI/NVMe: root port has only slot 0; skip dev > 0 */
		return false;	/* PCI/NVMe: pci core will see -ENODEV for other slots */

	return true;	/* PCI/NVMe: proceed with config cycle; may discover NVMe */
}

static int tlp_read_packet(struct altera_pcie *pcie, u32 *value)		/* PCI/NVMe: receive completion TLP from NVMe/EP config read */
{
	int i;						/* PCI/NVMe: loop counter, up to TLP_LOOP */
	bool sop = false;				/* PCI/NVMe: SOP seen? */
	u32 ctrl;					/* PCI/NVMe: completion status register */
	u32 reg0, reg1;					/* PCI/NVMe: completion payload/status DWs */
	u32 comp_status = 1;				/* PCI/NVMe: default UR/CA until proven OK */

	/*
	 * Minimum 2 loops to read TLP headers and 1 loop to read data
	 * payload.
	 */
	for (i = 0; i < TLP_LOOP; i++) {		/* PCI/NVMe: poll ~2.5ms for NVMe config completion */
		ctrl = cra_readl(pcie, RP_RXCPL_STATUS);	/* PCI/NVMe: check completion status */
		if ((ctrl & RP_RXCPL_SOP) || (ctrl & RP_RXCPL_EOP) || sop) {	/* PCI/NVMe: data beats present? */
			reg0 = cra_readl(pcie, RP_RXCPL_REG0);	/* PCI/NVMe: low DW */
			reg1 = cra_readl(pcie, RP_RXCPL_REG1);	/* PCI/NVMe: high DW / status */

			if (ctrl & RP_RXCPL_SOP) {	/* PCI/NVMe: start of completion */
				sop = true;	/* PCI/NVMe: mark SOP received */
				comp_status = TLP_COMP_STATUS(reg1);	/* PCI/NVMe: extract status (0=OK) */
			}

			if (ctrl & RP_RXCPL_EOP) {	/* PCI/NVMe: end of completion */
				if (comp_status)	/* PCI/NVMe: completion with UR/CA -> NVMe not responding */
					return PCIBIOS_DEVICE_NOT_FOUND;

				if (value)	/* PCI/NVMe: return config DWORD to NVMe host driver path */
					*value = reg0;

				return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: config read succeeded */
			}
		}
		udelay(5);	/* PCI/NVMe: 5 us back-off while waiting for NVMe/EP */
	}

	return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: timeout -> NVMe invisible to pci core */
}

static int s10_tlp_read_packet(struct altera_pcie *pcie, u32 *value)		/* PCI/NVMe: Stratix10 completion receiver for NVMe config reads */
{
	u32 ctrl;					/* PCI/NVMe: status */
	u32 comp_status;				/* PCI/NVMe: completion status field */
	u32 dw[4];					/* PCI/NVMe: up to 4 DW completion buffer */
	u32 count;					/* PCI/NVMe: received DW count */
	struct device *dev = &pcie->pdev->dev;	/* PCI/NVMe: for malformed TLP warning */

	for (count = 0; count < TLP_LOOP; count++) {	/* PCI/NVMe: poll for SOP from NVMe/EP */
		ctrl = cra_readl(pcie, S10_RP_RXCPL_STATUS);	/* PCI/NVMe: Stratix10 status register */
		if (ctrl & RP_RXCPL_SOP) {		/* PCI/NVMe: start of completion */
			/* Read first DW */
			dw[0] = cra_readl(pcie, S10_RP_RXCPL_REG);	/* PCI/NVMe: first header DW */
			break;	/* PCI/NVMe: exit poll loop */
		}

		udelay(5);	/* PCI/NVMe: wait for NVMe/EP response */
	}

	/* SOP detection failed, return error */
	if (count == TLP_LOOP)		/* PCI/NVMe: no completion from NVMe/EP */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: pci core will skip this device */

	count = 1;	/* PCI/NVMe: header DW0 consumed */

	/* Poll for EOP */
	while (count < ARRAY_SIZE(dw)) {	/* PCI/NVMe: read up to 3 more DWs */
		ctrl = cra_readl(pcie, S10_RP_RXCPL_STATUS);	/* PCI/NVMe: check EOP */
		dw[count++] = cra_readl(pcie, S10_RP_RXCPL_REG);	/* PCI/NVMe: next DW */
		if (ctrl & RP_RXCPL_EOP) {	/* PCI/NVMe: completion finished */
			comp_status = TLP_COMP_STATUS(dw[1]);	/* PCI/NVMe: status in header DW2-ish */
			if (comp_status)	/* PCI/NVMe: error completion -> NVMe unreachable */
				return PCIBIOS_DEVICE_NOT_FOUND;

			if (value && TLP_BYTE_COUNT(dw[1]) == sizeof(u32) &&	/* PCI/NVMe: only 4-byte payload accepted */
			    count == 4)
				*value = dw[3];	/* PCI/NVMe: return data DW to NVMe config path */

			return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: NVMe config read succeeded */
		}
	}

	dev_warn(dev, "Malformed TLP packet\n");	/* PCI/NVMe: warn if NVMe/EP sent bad completion */

	return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: treat malformed as no device */
}

static void tlp_write_packet(struct altera_pcie *pcie, u32 *headers,		/* PCI/NVMe: emit 3-DW config request TLP to NVMe/EP */
			     u32 data, bool align)
{
	struct tlp_rp_regpair_t tlp_rp_regdata;	/* PCI/NVMe: TX beat buffer */

	tlp_rp_regdata.reg0 = headers[0];	/* PCI/NVMe: TLP DW0 (fmt/type) */
	tlp_rp_regdata.reg1 = headers[1];	/* PCI/NVMe: TLP DW1 (requester ID/tag/byte-en) */
	tlp_rp_regdata.ctrl = RP_TX_SOP;	/* PCI/NVMe: start of packet */
	tlp_write_tx(pcie, &tlp_rp_regdata);	/* PCI/NVMe: send header first beat */

	if (align) {	/* PCI/NVMe: Qword-aligned write: data goes in separate beat */
		tlp_rp_regdata.reg0 = headers[2];	/* PCI/NVMe: TLP DW2 (target BDF/offset) */
		tlp_rp_regdata.reg1 = 0;	/* PCI/NVMe: padding */
		tlp_rp_regdata.ctrl = 0;	/* PCI/NVMe: middle beat */
		tlp_write_tx(pcie, &tlp_rp_regdata);	/* PCI/NVMe: send target address */

		tlp_rp_regdata.reg0 = data;	/* PCI/NVMe: payload data */
		tlp_rp_regdata.reg1 = 0;	/* PCI/NVMe: padding */
	} else {	/* PCI/NVMe: non-Qword-aligned: data packed with target address */
		tlp_rp_regdata.reg0 = headers[2];	/* PCI/NVMe: TLP DW2 */
		tlp_rp_regdata.reg1 = data;	/* PCI/NVMe: payload data */
	}

	tlp_rp_regdata.ctrl = RP_TX_EOP;	/* PCI/NVMe: end of packet */
	tlp_write_tx(pcie, &tlp_rp_regdata);	/* PCI/NVMe: send final beat to NVMe/EP */
}

static void s10_tlp_write_packet(struct altera_pcie *pcie, u32 *headers,	/* PCI/NVMe: Stratix10 4-beat config request to NVMe/EP */
				 u32 data, bool dummy)
{
	s10_tlp_write_tx(pcie, headers[0], RP_TX_SOP);	/* PCI/NVMe: DW0 + SOP */
	s10_tlp_write_tx(pcie, headers[1], 0);		/* PCI/NVMe: DW1 */
	s10_tlp_write_tx(pcie, headers[2], 0);		/* PCI/NVMe: DW2 */
	s10_tlp_write_tx(pcie, data, RP_TX_EOP);	/* PCI/NVMe: payload + EOP */
}

static void get_tlp_header(struct altera_pcie *pcie, u8 bus, u32 devfn,		/* PCI/NVMe: build config TLP header targeting NVMe/EP */
			   int where, u8 byte_en, bool read, u32 *headers)
{
	u8 cfg;					/* PCI/NVMe: fmt/type selected per bus */
	u8 cfg0 = read ? pcie->pcie_data->cfgrd0 : pcie->pcie_data->cfgwr0;	/* PCI/NVMe: Type0 read/write */
	u8 cfg1 = read ? pcie->pcie_data->cfgrd1 : pcie->pcie_data->cfgwr1;	/* PCI/NVMe: Type1 read/write */
	u8 tag = read ? TLP_READ_TAG : TLP_WRITE_TAG;	/* PCI/NVMe: read vs write tag */

	if (pcie->pcie_data->version == ALTERA_PCIE_V1)	/* PCI/NVMe: V1 uses root_bus_nr as Type0 boundary */
		cfg = (bus == pcie->root_bus_nr) ? cfg0 : cfg1;	/* PCI/NVMe: Type0 for root bus, Type1 beyond */
	else
		cfg = (bus > S10_RP_SECONDARY(pcie)) ? cfg0 : cfg1;	/* PCI/NVMe: V2 uses secondary bus as boundary */

	headers[0] = TLP_CFG_DW0(pcie, cfg);	/* PCI/NVMe: encode fmt/type + payload size */
	headers[1] = TLP_CFG_DW1(pcie, tag, byte_en);	/* PCI/NVMe: requester ID = root port, tag, byte enables */
	headers[2] = TLP_CFG_DW2(bus, devfn, where);	/* PCI/NVMe: target bus/dev/fn and register offset */
}

static int tlp_cfg_dword_read(struct altera_pcie *pcie, u8 bus, u32 devfn,	/* PCI/NVMe: one-DWORD config read used by NVMe enumeration */
			      int where, u8 byte_en, u32 *value)
{
	u32 headers[TLP_HDR_SIZE];	/* PCI/NVMe: 3-DW TLP header */

	get_tlp_header(pcie, bus, devfn, where, byte_en, true,	/* PCI/NVMe: build read TLP header */
		       headers);

	pcie->pcie_data->ops->tlp_write_pkt(pcie, headers, 0, false);	/* PCI/NVMe: send config read TLP to NVMe/EP */

	return pcie->pcie_data->ops->tlp_read_pkt(pcie, value);	/* PCI/NVMe: wait for completion and return DWORD */
}

static int tlp_cfg_dword_write(struct altera_pcie *pcie, u8 bus, u32 devfn,	/* PCI/NVMe: one-DWORD config write used to configure NVMe */
			       int where, u8 byte_en, u32 value)
{
	u32 headers[TLP_HDR_SIZE];	/* PCI/NVMe: 3-DW TLP header */
	int ret;

	get_tlp_header(pcie, bus, devfn, where, byte_en, false,	/* PCI/NVMe: build write TLP header */
		       headers);

	/* check alignment to Qword */
	if ((where & 0x7) == 0)		/* PCI/NVMe: Qword-aligned offset -> separate data beat */
		pcie->pcie_data->ops->tlp_write_pkt(pcie, headers,
					    value, true);
	else				/* PCI/NVMe: non-Qword-aligned -> packed data beat */
		pcie->pcie_data->ops->tlp_write_pkt(pcie, headers,
					    value, false);

	ret = pcie->pcie_data->ops->tlp_read_pkt(pcie, NULL);	/* PCI/NVMe: wait for write completion (no data) */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* PCI/NVMe: propagate UR/CA/timeout to pci core */

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on root port
	 * and update local copy of root bus number accordingly.
	 */
	if ((bus == pcie->root_bus_nr) && (where == PCI_PRIMARY_BUS))	/* PCI/NVMe: bus numbering changed by pci core during NVMe enum */
		pcie->root_bus_nr = (u8)(value);	/* PCI/NVMe: keep root_bus_nr in sync for later NVMe config cycles */

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: config write accepted by NVMe/EP */
}

static int s10_rp_read_cfg(struct altera_pcie *pcie, int where,		/* PCI/NVMe: read Stratix10 root-port config during NVMe enum */
			   int size, u32 *value)
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie, where);	/* PCI/NVMe: RP config aperture at 1MB offset in hip_base */

	switch (size) {
	case 1:
		*value = readb(addr);	/* PCI/NVMe: byte access, e.g. interrupt line/pin */
		break;
	case 2:
		*value = readw(addr);	/* PCI/NVMe: word access, e.g. PCIe cap registers */
		break;
	default:
		*value = readl(addr);	/* PCI/NVMe: dword access, e.g. BARs/status/command */
		break;
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: root-port config read ok */
}

static int s10_rp_write_cfg(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: write Stratix10 root-port config (bus enumeration) */
			    int where, int size, u32 value)
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie, where);	/* PCI/NVMe: RP config aperture */

	switch (size) {
	case 1:
		writeb(value, addr);	/* PCI/NVMe: byte write */
		break;
	case 2:
		writew(value, addr);	/* PCI/NVMe: word write */
		break;
	default:
		writel(value, addr);	/* PCI/NVMe: dword write */
		break;
	}

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on root port
	 * and update local copy of root bus number accordingly.
	 */
	if (busno == pcie->root_bus_nr && where == PCI_PRIMARY_BUS)	/* PCI/NVMe: bus number update during NVMe scan */
		pcie->root_bus_nr = value & 0xff;	/* PCI/NVMe: cache new root bus number */

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: root-port config write ok */
}

static int aglx_rp_read_cfg(struct altera_pcie *pcie, int where,		/* PCI/NVMe: read Agilex root-port config for NVMe enumeration */
			    int size, u32 *value)
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie, where);	/* PCI/NVMe: direct HIP config access */

	switch (size) {
	case 1:
		*value = readb_relaxed(addr);	/* PCI/NVMe: byte access */
		break;
	case 2:
		*value = readw_relaxed(addr);	/* PCI/NVMe: word access */
		break;
	default:
		*value = readl_relaxed(addr);	/* PCI/NVMe: dword access */
		break;
	}

	/* Interrupt PIN not programmed in hardware, set to INTA. */
	if (where == PCI_INTERRUPT_PIN && size == 1 && !(*value))	/* PCI/NVMe: if no pin, force INTA for NVMe legacy fallback */
		*value = 0x01;
	else if (where == PCI_INTERRUPT_LINE && !(*value & 0xff00))	/* PCI/NVMe: if no interrupt line, encode INTA routing hint */
		*value |= 0x0100;

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: root-port config read ok */
}

static int aglx_rp_write_cfg(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: write Agilex root-port config */
			     int where, int size, u32 value)
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie, where);	/* PCI/NVMe: direct HIP config access */

	switch (size) {
	case 1:
		writeb_relaxed(value, addr);	/* PCI/NVMe: byte write */
		break;
	case 2:
		writew_relaxed(value, addr);	/* PCI/NVMe: word write */
		break;
	default:
		writel_relaxed(value, addr);	/* PCI/NVMe: dword write */
		break;
	}

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on Root Port
	 * and update local copy of root bus number accordingly.
	 */
	if (busno == pcie->root_bus_nr && where == PCI_PRIMARY_BUS)	/* PCI/NVMe: bus number change during NVMe enumeration */
		pcie->root_bus_nr = value & 0xff;	/* PCI/NVMe: cache new root bus number */

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: root-port config write ok */
}

static int aglx_ep_write_cfg(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: write downstream NVMe/EP config on Agilex */
			     unsigned int devfn, int where, int size, u32 value)
{
	cra_writel(pcie, ((busno << 8) | devfn), AGLX_BDF_REG);	/* PCI/NVMe: select target NVMe BDF in config window */
	if (busno > AGLX_RP_SECONDARY(pcie))	/* PCI/NVMe: Type1 routing for buses beyond secondary */
		where |= FIELD_PREP(AGLX_CFG_TARGET, AGLX_CFG_TARGET_TYPE1);

	switch (size) {
	case 1:
		cra_writeb(pcie, value, where);	/* PCI/NVMe: byte config write to NVMe */
		break;
	case 2:
		cra_writew(pcie, value, where);	/* PCI/NVMe: word config write to NVMe */
		break;
	default:
		cra_writel(pcie, value, where);	/* PCI/NVMe: dword config write to NVMe, e.g. BAR, MSI cap */
			break;
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: downstream config write ok */
}

static int aglx_ep_read_cfg(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: read downstream NVMe/EP config on Agilex */
			    unsigned int devfn, int where, int size, u32 *value)
{
	cra_writel(pcie, ((busno << 8) | devfn), AGLX_BDF_REG);	/* PCI/NVMe: select target NVMe BDF */
	if (busno > AGLX_RP_SECONDARY(pcie))	/* PCI/NVMe: Type1 for subordinate buses */
		where |= FIELD_PREP(AGLX_CFG_TARGET, AGLX_CFG_TARGET_TYPE1);

	switch (size) {
	case 1:
		*value = cra_readb(pcie, where);	/* PCI/NVMe: byte read, e.g. capabilities pointer */
		break;
	case 2:
		*value = cra_readw(pcie, where);	/* PCI/NVMe: word read, e.g. status/command */
		break;
	default:
		*value = cra_readl(pcie, where);	/* PCI/NVMe: dword read, e.g. BAR0, MSI/MSI-X caps */
		break;
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: downstream config read ok */
}

static int _altera_pcie_cfg_read(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: dispatch config read for NVMe/EP or root port */
				 unsigned int devfn, int where, int size,
				 u32 *value)
{
	int ret;				/* PCI/NVMe: result from lower layer */
	u32 data;				/* PCI/NVMe: temporary DWORD */
	u8 byte_en;				/* PCI/NVMe: byte-enable mask for sub-DWORD access */

	if (busno == pcie->root_bus_nr && pcie->pcie_data->ops->rp_read_cfg)	/* PCI/NVMe: root-port config read during NVMe bus scan */
		return pcie->pcie_data->ops->rp_read_cfg(pcie, where,
							 size, value);

	if (pcie->pcie_data->ops->ep_read_cfg)	/* PCI/NVMe: Agilex direct downstream read for NVMe */
		return pcie->pcie_data->ops->ep_read_cfg(pcie, busno, devfn,
							where, size, value);

	switch (size) {
	case 1:
		byte_en = 1 << (where & 3);	/* PCI/NVMe: enable one byte within DWORD */
		break;
	case 2:
		byte_en = 3 << (where & 3);	/* PCI/NVMe: enable two bytes within DWORD */
		break;
	default:
		byte_en = 0xf;			/* PCI/NVMe: full DWORD enable */
		break;
	}

	ret = tlp_cfg_dword_read(pcie, busno, devfn,
				 (where & ~DWORD_MASK), byte_en, &data);	/* PCI/NVMe: read full DWORD covering requested bytes */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* PCI/NVMe: NVMe/EP did not respond */

	switch (size) {
	case 1:
		*value = (data >> (8 * (where & 0x3))) & 0xff;	/* PCI/NVMe: extract requested byte for pci core */
		break;
	case 2:
		*value = (data >> (8 * (where & 0x2))) & 0xffff;	/* PCI/NVMe: extract requested word for pci core */
		break;
	default:
		*value = data;			/* PCI/NVMe: return full DWORD */
		break;
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: config read value returned to NVMe driver path */
}

static int _altera_pcie_cfg_write(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: dispatch config write for NVMe/EP or root port */
				  unsigned int devfn, int where, int size,
				  u32 value)
{
	u32 data32;				/* PCI/NVMe: shifted write data */
	u32 shift = 8 * (where & 3);		/* PCI/NVMe: byte shift within DWORD */
	u8 byte_en;				/* PCI/NVMe: byte-enable mask */

	if (busno == pcie->root_bus_nr && pcie->pcie_data->ops->rp_write_cfg)	/* PCI/NVMe: root-port config write */
		return pcie->pcie_data->ops->rp_write_cfg(pcie, busno,
							  where, size, value);

	if (pcie->pcie_data->ops->ep_write_cfg)	/* PCI/NVMe: Agilex direct downstream write for NVMe */
		return pcie->pcie_data->ops->ep_write_cfg(pcie, busno, devfn,
							 where, size, value);

	switch (size) {
	case 1:
		data32 = (value & 0xff) << shift;	/* PCI/NVMe: byte write aligned within DWORD */
		byte_en = 1 << (where & 3);		/* PCI/NVMe: enable only that byte */
		break;
	case 2:
		data32 = (value & 0xffff) << shift;	/* PCI/NVMe: word write aligned within DWORD */
		byte_en = 3 << (where & 3);		/* PCI/NVMe: enable those two bytes */
		break;
	default:
		data32 = value;				/* PCI/NVMe: full DWORD write */
		byte_en = 0xf;				/* PCI/NVMe: enable all bytes */
		break;
	}

	return tlp_cfg_dword_write(pcie, busno, devfn, (where & ~DWORD_MASK),		/* PCI/NVMe: send aligned DWORD config write TLP to NVMe/EP */
				   byte_en, data32);
}

static int altera_pcie_cfg_read(struct pci_bus *bus, unsigned int devfn,		/* PCI/NVMe: pci_ops.read called for every NVMe config access */
				int where, int size, u32 *value)
{
	struct altera_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host private data from bridge->sysdata */

	if (altera_pcie_hide_rc_bar(bus, devfn, where))	/* PCI/NVMe: skip RC BAR0 so inbound DMA/MSI region is not allocated as MMIO */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (!altera_pcie_valid_device(pcie, bus, PCI_SLOT(devfn)))	/* PCI/NVMe: if link down or invalid slot, report no NVMe */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return _altera_pcie_cfg_read(pcie, bus->number, devfn, where, size,
				     value);	/* PCI/NVMe: perform config read and return value to PCI core/NVMe driver */
}

static int altera_pcie_cfg_write(struct pci_bus *bus, unsigned int devfn,		/* PCI/NVMe: pci_ops.write called for every NVMe config write */
				 int where, int size, u32 value)
{
	struct altera_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host private data */

	if (altera_pcie_hide_rc_bar(bus, devfn, where))	/* PCI/NVMe: block writes to hidden RC BAR0 */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (!altera_pcie_valid_device(pcie, bus, PCI_SLOT(devfn)))	/* PCI/NVMe: no link or invalid slot */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return _altera_pcie_cfg_write(pcie, bus->number, devfn, where, size,
				      value);	/* PCI/NVMe: perform config write (BAR, command, MSI, etc.) for NVMe */
}

static struct pci_ops altera_pcie_ops = {
	.read = altera_pcie_cfg_read,	/* PCI/NVMe: PCI core -> NVMe config read path */
	.write = altera_pcie_cfg_write,	/* PCI/NVMe: PCI core -> NVMe config write path */
};

static int altera_read_cap_word(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: helper to read root-port PCIe cap word (link status) */
				unsigned int devfn, int offset, u16 *value)
{
	u32 data;				/* PCI/NVMe: temporary 32-bit read */
	int ret;

	ret = _altera_pcie_cfg_read(pcie, busno, devfn,		/* PCI/NVMe: read from root-port PCIe capability */
				    pcie->pcie_data->cap_offset + offset,
				    sizeof(*value),
				    &data);
	*value = data;	/* PCI/NVMe: truncate to u16 */
	return ret;	/* PCI/NVMe: return status to caller */
}

static int altera_write_cap_word(struct altera_pcie *pcie, u8 busno,		/* PCI/NVMe: helper to write root-port PCIe cap word */
				 unsigned int devfn, int offset, u16 value)
{
	return _altera_pcie_cfg_write(pcie, busno, devfn,		/* PCI/NVMe: write root-port PCIe capability */
				      pcie->pcie_data->cap_offset + offset,
				      sizeof(value),
				      value);
}

static void altera_wait_link_retrain(struct altera_pcie *pcie)		/* PCI/NVMe: wait for link retrain before NVMe can be used */
{
	struct device *dev = &pcie->pdev->dev;	/* PCI/NVMe: for timeout messages */
	u16 reg16;				/* PCI/NVMe: PCIe cap word */
	unsigned long start_jiffies;		/* PCI/NVMe: timeout reference */

	/* Wait for link training end. */
	start_jiffies = jiffies;			/* PCI/NVMe: start retrain timeout */
	for (;;) {
		altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,		/* PCI/NVMe: poll PCI_EXP_LNKSTA.LT */
				     PCI_EXP_LNKSTA, &reg16);
		if (!(reg16 & PCI_EXP_LNKSTA_LT))	/* PCI/NVMe: training complete */
			break;

		if (time_after(jiffies, start_jiffies + LINK_RETRAIN_TIMEOUT)) {	/* PCI/NVMe: abort if retrain stalls */
			dev_err(dev, "link retrain timeout\n");	/* PCI/NVMe: NVMe may still work at lower speed */
			break;
		}
		udelay(100);	/* PCI/NVMe: 100 us poll interval */
	}

	/* Wait for link is up */
	start_jiffies = jiffies;			/* PCI/NVMe: start link-up timeout */
	for (;;) {
		if (pcie->pcie_data->ops->get_link_status(pcie))	/* PCI/NVMe: DLLLA set -> NVMe reachable */
			break;

		if (time_after(jiffies, start_jiffies + LINK_UP_TIMEOUT)) {	/* PCI/NVMe: link still down after 1s */
			dev_err(dev, "link up timeout\n");	/* PCI/NVMe: NVMe enumeration will likely fail */
			break;
		}
		udelay(100);	/* PCI/NVMe: 100 us poll interval */
	}
}

static void altera_pcie_retrain(struct altera_pcie *pcie)			/* PCI/NVMe: attempt higher link speed before NVMe probe */
{
	u16 linkcap, linkstat, linkctl;		/* PCI/NVMe: PCIe link capability/status/control */

	if (!pcie->pcie_data->ops->get_link_status(pcie))	/* PCI/NVMe: no point retraining if link down */
		return;

	/*
	 * Set the retrain bit if the PCIe rootport support > 2.5GB/s, but
	 * current speed is 2.5 GB/s.
	 */
	altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN, PCI_EXP_LNKCAP,		/* PCI/NVMe: read supported link speeds */
			     &linkcap);
	if ((linkcap & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)	/* PCI/NVMe: only Gen1 supported, skip retrain */
		return;

	altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN, PCI_EXP_LNKSTA,		/* PCI/NVMe: read current link speed */
			     &linkstat);
	if ((linkstat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {	/* PCI/NVMe: currently at Gen1 but can go faster */
		altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,
				     PCI_EXP_LNKCTL, &linkctl);	/* PCI/NVMe: read link control */
		linkctl |= PCI_EXP_LNKCTL_RL;	/* PCI/NVMe: initiate retrain to Gen2/3 */
		altera_write_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,
				      PCI_EXP_LNKCTL, linkctl);	/* PCI/NVMe: kick retrain */

		altera_wait_link_retrain(pcie);	/* PCI/NVMe: wait until NVMe link is ready */
	}
}

static int altera_pcie_intx_map(struct irq_domain *domain, unsigned int irq,		/* PCI/NVMe: map legacy INTA..D for NVMe fallback interrupt */
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);	/* PCI/NVMe: simple legacy handler, no masking at chip level */
	irq_set_chip_data(irq, domain->host_data);	/* PCI/NVMe: point to pcie struct for chained dispatch */
	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = altera_pcie_intx_map,	/* PCI/NVMe: domain map callback for NVMe legacy IRQs */
	.xlate = pci_irqd_intx_xlate,	/* PCI/NVMe: translate PCI INTA..D to hwirq 0..3 */
};

static void altera_pcie_isr(struct irq_desc *desc)				/* PCI/NVMe: root-port ISR dispatching legacy INTx for NVMe */
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* PCI/NVMe: parent irqchip */
	struct altera_pcie *pcie;				/* PCI/NVMe: host private */
	struct device *dev;					/* PCI/NVMe: for error logging */
	unsigned long status;					/* PCI/NVMe: pending INTx bits */
	u32 bit;						/* PCI/NVMe: current INTx line */
	int ret;						/* PCI/NVMe: handle_domain_irq result */

	chained_irq_enter(chip, desc);	/* PCI/NVMe: ack/mask parent interrupt */
	pcie = irq_desc_get_handler_data(desc);	/* PCI/NVMe: pcie struct from chained handler data */
	dev = &pcie->pdev->dev;

	while ((status = cra_readl(pcie, P2A_INT_STATUS)	/* PCI/NVMe: read pending legacy INTx status */
		& P2A_INT_STS_ALL) != 0) {		/* PCI/NVMe: loop while any INTA..D pending */
		for_each_set_bit(bit, &status, PCI_NUM_INTX) {	/* PCI/NVMe: iterate INTA..D asserted by NVMe */
			/* clear interrupts */
			cra_writel(pcie, 1 << bit, P2A_INT_STATUS);	/* PCI/NVMe: clear this legacy interrupt at root port */

			ret = generic_handle_domain_irq(pcie->irq_domain, bit);	/* PCI/NVMe: dispatch to NVMe legacy INTA..D handler */
			if (ret)
				dev_err_ratelimited(dev, "unexpected IRQ, INT%d\n", bit);	/* PCI/NVMe: no NVMe handler mapped */
		}
	}
	chained_irq_exit(chip, desc);	/* PCI/NVMe: unmask parent interrupt */
}

static void aglx_isr(struct irq_desc *desc)					/* PCI/NVMe: Agilex root-port ISR, dispatches AER/legacy events for NVMe */
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* PCI/NVMe: parent irqchip */
	struct altera_pcie *pcie;				/* PCI/NVMe: host private */
	struct device *dev;					/* PCI/NVMe: for error logging */
	u32 status;						/* PCI/NVMe: port IRQ status */
	int ret;						/* PCI/NVMe: handle result */

	chained_irq_enter(chip, desc);	/* PCI/NVMe: ack/mask parent interrupt */
	pcie = irq_desc_get_handler_data(desc);	/* PCI/NVMe: pcie struct */
	dev = &pcie->pdev->dev;

	status = readl(pcie->hip_base + pcie->pcie_data->port_conf_offset +	/* PCI/NVMe: read Agilex port IRQ status */
		       pcie->pcie_data->port_irq_status_offset);

	if (status & CFG_AER) {	/* PCI/NVMe: AER event from NVMe/EP */
		writel(CFG_AER, (pcie->hip_base + pcie->pcie_data->port_conf_offset +	/* PCI/NVMe: clear AER status */
				 pcie->pcie_data->port_irq_status_offset));

		ret = generic_handle_domain_irq(pcie->irq_domain, 0);	/* PCI/NVMe: dispatch AER/legacy event (hwirq 0 = INTA/AER) */
		if (ret)
			dev_err_ratelimited(dev, "unexpected IRQ %d\n", pcie->irq);	/* PCI/NVMe: no handler registered */
	}
	chained_irq_exit(chip, desc);	/* PCI/NVMe: unmask parent interrupt */
}

static int altera_pcie_init_irq_domain(struct altera_pcie *pcie)		/* PCI/NVMe: create INTx domain used by NVMe when MSI/MSI-X unavailable */
{
	struct device *dev = &pcie->pdev->dev;

	/* Setup INTx */
	pcie->irq_domain = irq_domain_create_linear(dev_fwnode(dev), PCI_NUM_INTX,	/* PCI/NVMe: 4 legacy pins for NVMe INTA..D */
					&intx_domain_ops, pcie);
	if (!pcie->irq_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");	/* PCI/NVMe: cannot provide legacy fallback for NVMe */
		return -ENOMEM;
	}

	return 0;	/* PCI/NVMe: INTx domain ready for NVMe binding */
}

static void altera_pcie_irq_teardown(struct altera_pcie *pcie)		/* PCI/NVMe: release IRQ resources on driver removal */
{
	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);	/* PCI/NVMe: detach root-port ISR from parent IRQ */
	irq_domain_remove(pcie->irq_domain);	/* PCI/NVMe: free INTx domain used by NVMe */
	irq_dispose_mapping(pcie->irq);	/* PCI/NVMe: dispose parent IRQ mapping */
}

static int altera_pcie_parse_dt(struct altera_pcie *pcie)			/* PCI/NVMe: parse DT and map resources needed for NVMe host */
{
	struct platform_device *pdev = pcie->pdev;

	pcie->cra_base = devm_platform_ioremap_resource_byname(pdev, "Cra");	/* PCI/NVMe: map CRA registers used for TLP and legacy IRQ */
	if (IS_ERR(pcie->cra_base))
		return PTR_ERR(pcie->cra_base);	/* PCI/NVMe: cannot access controller -> NVMe init fails */

	if (pcie->pcie_data->version == ALTERA_PCIE_V2 ||	/* PCI/NVMe: S10/Agilex need HIP window */
	    pcie->pcie_data->version == ALTERA_PCIE_V3) {
		pcie->hip_base = devm_platform_ioremap_resource_byname(pdev, "Hip");	/* PCI/NVMe: map HIP for root-port config access */
		if (IS_ERR(pcie->hip_base))
			return PTR_ERR(pcie->hip_base);	/* PCI/NVMe: no HIP -> cannot configure root port for NVMe */
	}

	/* setup IRQ */
	pcie->irq = platform_get_irq(pdev, 0);	/* PCI/NVMe: obtain root-port interrupt line for legacy/AER events */
	if (pcie->irq < 0)
		return pcie->irq;	/* PCI/NVMe: no IRQ -> NVMe interrupts unavailable */

	irq_set_chained_handler_and_data(pcie->irq, pcie->pcie_data->ops->rp_isr, pcie);	/* PCI/NVMe: chain root-port ISR to parent IRQ */
	return 0;	/* PCI/NVMe: resources mapped, ready for bridge setup */
}

static void altera_pcie_host_init(struct altera_pcie *pcie)			/* PCI/NVMe: initialize host before scanning NVMe devices */
{
	altera_pcie_retrain(pcie);	/* PCI/NVMe: negotiate best link speed for NVMe DMA throughput */
}

static const struct altera_pcie_ops altera_pcie_ops_1_0 = {
	.tlp_read_pkt = tlp_read_packet,	/* PCI/NVMe: V1 TLP completion receive */
	.tlp_write_pkt = tlp_write_packet,	/* PCI/NVMe: V1 TLP request send */
	.get_link_status = altera_pcie_link_up,	/* PCI/NVMe: V1 LTSSM L0 check */
	.rp_isr = altera_pcie_isr,		/* PCI/NVMe: V1 legacy INTx ISR */
};

static const struct altera_pcie_ops altera_pcie_ops_2_0 = {
	.tlp_read_pkt = s10_tlp_read_packet,	/* PCI/NVMe: S10 TLP completion receive */
	.tlp_write_pkt = s10_tlp_write_packet,	/* PCI/NVMe: S10 TLP request send */
	.get_link_status = s10_altera_pcie_link_up,	/* PCI/NVMe: S10 DLLLA check */
	.rp_read_cfg = s10_rp_read_cfg,	/* PCI/NVMe: S10 direct root-port config read */
	.rp_write_cfg = s10_rp_write_cfg,	/* PCI/NVMe: S10 direct root-port config write */
	.rp_isr = altera_pcie_isr,		/* PCI/NVMe: S10 legacy INTx ISR */
};

static const struct altera_pcie_ops altera_pcie_ops_3_0 = {
	.rp_read_cfg = aglx_rp_read_cfg,	/* PCI/NVMe: Agilex direct root-port config read */
	.rp_write_cfg = aglx_rp_write_cfg,	/* PCI/NVMe: Agilex direct root-port config write */
	.get_link_status = aglx_altera_pcie_link_up,	/* PCI/NVMe: Agilex DLLLA check */
	.ep_read_cfg = aglx_ep_read_cfg,	/* PCI/NVMe: Agilex downstream config read for NVMe/EP */
	.ep_write_cfg = aglx_ep_write_cfg,	/* PCI/NVMe: Agilex downstream config write for NVMe/EP */
	.rp_isr = aglx_isr,			/* PCI/NVMe: Agilex AER/legacy ISR */
};

static const struct altera_pcie_data altera_pcie_1_0_data = {
	.ops = &altera_pcie_ops_1_0,	/* PCI/NVMe: V1 callbacks */
	.cap_offset = 0x80,		/* PCI/NVMe: PCIe capability offset in root-port config */
	.version = ALTERA_PCIE_V1,
	.cfgrd0 = TLP_FMTTYPE_CFGRD0,	/* PCI/NVMe: V1 Type0 read TLP fmt/type */
	.cfgrd1 = TLP_FMTTYPE_CFGRD1,	/* PCI/NVMe: V1 Type1 read TLP fmt/type */
	.cfgwr0 = TLP_FMTTYPE_CFGWR0,	/* PCI/NVMe: V1 Type0 write TLP fmt/type */
	.cfgwr1 = TLP_FMTTYPE_CFGWR1,	/* PCI/NVMe: V1 Type1 write TLP fmt/type */
};

static const struct altera_pcie_data altera_pcie_2_0_data = {
	.ops = &altera_pcie_ops_2_0,	/* PCI/NVMe: S10 callbacks */
	.version = ALTERA_PCIE_V2,
	.cap_offset = 0x70,		/* PCI/NVMe: S10 PCIe capability offset */
	.cfgrd0 = S10_TLP_FMTTYPE_CFGRD0,	/* PCI/NVMe: S10 Type0 read encoding */
	.cfgrd1 = S10_TLP_FMTTYPE_CFGRD1,	/* PCI/NVMe: S10 Type1 read encoding */
	.cfgwr0 = S10_TLP_FMTTYPE_CFGWR0,	/* PCI/NVMe: S10 Type0 write encoding */
	.cfgwr1 = S10_TLP_FMTTYPE_CFGWR1,	/* PCI/NVMe: S10 Type1 write encoding */
};

static const struct altera_pcie_data altera_pcie_3_0_f_tile_data = {
	.ops = &altera_pcie_ops_3_0,	/* PCI/NVMe: Agilex callbacks */
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,		/* PCI/NVMe: Agilex PCIe capability offset */
	.port_conf_offset = 0x14000,	/* PCI/NVMe: F-tile port config block */
	.port_irq_status_offset = AGLX_ROOT_PORT_IRQ_STATUS,	/* PCI/NVMe: F-tile IRQ status */
	.port_irq_enable_offset = AGLX_ROOT_PORT_IRQ_ENABLE,	/* PCI/NVMe: F-tile IRQ enable */
};

static const struct altera_pcie_data altera_pcie_3_0_p_tile_data = {
	.ops = &altera_pcie_ops_3_0,	/* PCI/NVMe: Agilex callbacks */
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,		/* PCI/NVMe: Agilex PCIe capability offset */
	.port_conf_offset = 0x104000,	/* PCI/NVMe: P-tile port config block */
	.port_irq_status_offset = AGLX_ROOT_PORT_IRQ_STATUS,	/* PCI/NVMe: P-tile IRQ status */
	.port_irq_enable_offset = AGLX_ROOT_PORT_IRQ_ENABLE,	/* PCI/NVMe: P-tile IRQ enable */
};

static const struct altera_pcie_data altera_pcie_3_0_r_tile_data = {
	.ops = &altera_pcie_ops_3_0,	/* PCI/NVMe: Agilex callbacks */
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,		/* PCI/NVMe: Agilex PCIe capability offset */
	.port_conf_offset = 0x1300,	/* PCI/NVMe: R-tile port config block */
	.port_irq_status_offset = 0x0,	/* PCI/NVMe: R-tile IRQ status offset */
	.port_irq_enable_offset = 0x4,	/* PCI/NVMe: R-tile IRQ enable offset */
};

static const struct of_device_id altera_pcie_of_match[] = {
	{.compatible = "altr,pcie-root-port-1.0",
	 .data = &altera_pcie_1_0_data },	/* PCI/NVMe: V1 root port, TLP-based config */
	{.compatible = "altr,pcie-root-port-2.0",
	 .data = &altera_pcie_2_0_data },	/* PCI/NVMe: Stratix10 root port */
	{.compatible = "altr,pcie-root-port-3.0-f-tile",
	 .data = &altera_pcie_3_0_f_tile_data },	/* PCI/NVMe: Agilex F-tile root port */
	{.compatible = "altr,pcie-root-port-3.0-p-tile",
	 .data = &altera_pcie_3_0_p_tile_data },	/* PCI/NVMe: Agilex P-tile root port */
	{.compatible = "altr,pcie-root-port-3.0-r-tile",
	 .data = &altera_pcie_3_0_r_tile_data },	/* PCI/NVMe: Agilex R-tile root port */
	{},
};

static int altera_pcie_probe(struct platform_device *pdev)			/* PCI/NVMe: platform probe sets up host bridge for NVMe enumeration */
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: device node for this root port */
	struct altera_pcie *pcie;		/* PCI/NVMe: host private data */
	struct pci_host_bridge *bridge;		/* PCI/NVMe: Linux PCI host bridge for NVMe */
	int ret;				/* PCI/NVMe: return value */
	const struct altera_pcie_data *data;	/* PCI/NVMe: matched variant data */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));	/* PCI/NVMe: allocate host bridge + private pcie struct */
	if (!bridge)
		return -ENOMEM;	/* PCI/NVMe: cannot allocate bridge -> NVMe init fails */

	pcie = pci_host_bridge_priv(bridge);	/* PCI/NVMe: private area becomes host controller state */
	pcie->pdev = pdev;			/* PCI/NVMe: remember platform device */
	platform_set_drvdata(pdev, pcie);	/* PCI/NVMe: retrieve pcie on remove */

	data = of_device_get_match_data(&pdev->dev);	/* PCI/NVMe: select V1/V2/V3 data by compatible string */
	if (!data)
		return -ENODEV;	/* PCI/NVMe: unknown DT compatible */

	pcie->pcie_data = data;			/* PCI/NVMe: bind variant ops/offsets */

	ret = altera_pcie_parse_dt(pcie);	/* PCI/NVMe: map CRA/HIP and IRQ */
	if (ret) {
		dev_err(dev, "Parsing DT failed\n");
		return ret;	/* PCI/NVMe: resource/IRQ failure propagated */
	}

	ret = altera_pcie_init_irq_domain(pcie);	/* PCI/NVMe: create INTx domain for NVMe legacy fallback */
	if (ret) {
		dev_err(dev, "Failed creating IRQ Domain\n");
		return ret;	/* PCI/NVMe: cannot support legacy interrupts for NVMe */
	}

	if (pcie->pcie_data->version == ALTERA_PCIE_V1 ||	/* PCI/NVMe: V1/V2 use CRA interrupt regs */
	    pcie->pcie_data->version == ALTERA_PCIE_V2) {
		/* clear all interrupts */
		cra_writel(pcie, P2A_INT_STS_ALL, P2A_INT_STATUS);	/* PCI/NVMe: clear pending legacy INTx before NVMe probe */
		/* enable all interrupts */
		cra_writel(pcie, P2A_INT_ENA_ALL, P2A_INT_ENABLE);	/* PCI/NVMe: enable INTA..INTD so NVMe legacy IRQ can fire */
		altera_pcie_host_init(pcie);	/* PCI/NVMe: link retrain for NVMe */
	} else if (pcie->pcie_data->version == ALTERA_PCIE_V3) {
		writel(CFG_AER,			/* PCI/NVMe: enable AER interrupt for NVMe error reporting */
		       pcie->hip_base + pcie->pcie_data->port_conf_offset +
		       pcie->pcie_data->port_irq_enable_offset);
	}

	bridge->sysdata = pcie;		/* PCI/NVMe: pci_ops will retrieve pcie via bus->sysdata */
	bridge->busnr = pcie->root_bus_nr;	/* PCI/NVMe: initial root bus number (0) for NVMe scan */
	bridge->ops = &altera_pcie_ops;	/* PCI/NVMe: register config access callbacks used during NVMe enumeration */

	return pci_host_probe(bridge);	/* PCI/NVMe: enumerate PCI bus, discover and bind NVMe device */
}

static void altera_pcie_remove(struct platform_device *pdev)			/* PCI/NVMe: remove host bridge and unbind NVMe */
{
	struct altera_pcie *pcie = platform_get_drvdata(pdev);	/* PCI/NVMe: retrieve host state */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);	/* PCI/NVMe: bridge from private */

	pci_stop_root_bus(bridge->bus);	/* PCI/NVMe: stop NVMe device and bus access */
	pci_remove_root_bus(bridge->bus);	/* PCI/NVMe: unbind/remove NVMe and child devices */
	altera_pcie_irq_teardown(pcie);	/* PCI/NVMe: release IRQ domain/mapping */
}

static struct platform_driver altera_pcie_driver = {
	.probe = altera_pcie_probe,	/* PCI/NVMe: bind Altera root port, start NVMe enumeration */
	.remove = altera_pcie_remove,	/* PCI/NVMe: unbind root port and NVMe */
	.driver = {
		.name = "altera-pcie",	/* PCI/NVMe: platform driver name */
		.of_match_table = altera_pcie_of_match,	/* PCI/NVMe: DT matching for Altera PCIe root ports */
	},
};

MODULE_DEVICE_TABLE(of, altera_pcie_of_match);	/* PCI/NVMe: export DT IDs for module loading */
module_platform_driver(altera_pcie_driver);	/* PCI/NVMe: register platform driver */
MODULE_DESCRIPTION("Altera PCIe host controller driver");	/* PCI/NVMe: module metadata */
MODULE_LICENSE("GPL v2");	/* PCI/NVMe: license */
