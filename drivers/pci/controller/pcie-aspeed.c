// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Aspeed Technology Inc.
 */
#include <linux/bitfield.h>	/* PCI/NVMe: field helpers used for PCIe TLP headers targeting NVMe config space */
#include <linux/clk.h>		/* PCI/NVMe: clock gating for the PCIe root port that enumerates NVMe SSDs */
#include <linux/interrupt.h>	/* PCI/NVMe: interrupt handling for NVMe MSI/MSI-X/INTx completions */
#include <linux/irq.h>		/* PCI/NVMe: irq descriptor helpers; NVMe vectors are mapped through this RC IRQ domain */
#include <linux/irqdomain.h>	/* PCI/NVMe: INTx and MSI domains used when NVMe pci.c allocates irq vectors */
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip/irq-msi-lib.h>	/* PCI/NVMe: generic MSI library reused by this RC for NVMe MSI/MSI-X setup */
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>	/* PCI/NVMe: syscon access for SoC-level routing of upstream DMA from NVMe */
#include <linux/module.h>
#include <linux/msi.h>		/* PCI/NVMe: MSI flag definitions needed to expose MSI/MSI-X to NVMe devices */
#include <linux/mutex.h>		/* PCI/NVMe: protects NVMe MSI vector bitmap allocation */
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>		/* PCI/NVMe: OF PCI helpers to parse NVMe port devfn */
#include <linux/pci.h>		/* PCI/NVMe: core PCI structures; nvme_probe in drivers/nvme/host/pci.c runs after this RC enumerates the bus */
#include <linux/platform_device.h>
#include <linux/phy/pcie.h>
#include <linux/phy/phy.h>		/* PCI/NVMe: PHY configuration for the physical link to the NVMe SSD */
#include <linux/regmap.h>
#include <linux/reset.h>		/* PCI/NVMe: PERST and RC resets used during NVMe hotplug/reset sequences */

#include "../pci.h"		/* PCI/NVMe: internal PCI definitions shared with other host controller drivers */

#define MAX_MSI_HOST_IRQS	64	/* PCI/NVMe: max MSI vectors this RC can assign across all endpoints (e.g. NVMe queues) */
#define ASPEED_RESET_RC_WAIT_MS	10	/* PCI/NVMe: reset assertion time before the link can retrain for NVMe */

/* AST2600 AHBC Registers */
#define ASPEED_AHBC_KEY			0x00	/* PCI/NVMe: unlock register for AHBC memory decode needed by NVMe DMA */
#define  ASPEED_AHBC_UNLOCK_KEY			0xaeed1a03	/* PCI/NVMe: magic value to unlock upstream DMA path for NVMe traffic */
#define  ASPEED_AHBC_UNLOCK			0x01	/* PCI/NVMe: re-lock after enabling RC memory decode for NVMe */
#define ASPEED_AHBC_ADDR_MAPPING	0x8c	/* PCI/NVMe: controls whether upstream memory/DMA from NVMe is routed to DRAM */
#define  ASPEED_PCIE_RC_MEMORY_EN		BIT(5)	/* PCI/NVMe: enables RC memory decoding so NVMe BAR and DMA accesses reach host memory */

/* AST2600 H2X Controller Registers */
#define ASPEED_H2X_INT_STS		0x08	/* PCI/NVMe: interrupt/status bits; legacy INTx and config completion for NVMe */
#define  ASPEED_PCIE_TX_IDLE_CLEAR		BIT(0)	/* PCI/NVMe: clear config TLP transmit-done status after probing an NVMe device */
#define  ASPEED_PCIE_INTX_STS			GENMASK(3, 0)	/* PCI/NVMe: INTx A/B/C/D status from NVMe/endpoint if MSI not used */
#define ASPEED_H2X_HOST_RX_DESC_DATA	0x0c	/* PCI/NVMe: data returned from host-side config read (rarely used for NVMe) */
#define ASPEED_H2X_TX_DESC0		0x10	/* PCI/NVMe: config TLP DW0 (fmt/type) for NVMe config-space accesses */
#define ASPEED_H2X_TX_DESC1		0x14	/* PCI/NVMe: config TLP DW1 (requester ID, byte enables) for NVMe config access */
#define ASPEED_H2X_TX_DESC2		0x18	/* PCI/NVMe: config TLP DW2 (BDF + register offset) identifying the NVMe device */
#define ASPEED_H2X_TX_DESC3		0x1c	/* PCI/NVMe: config TLP DW3 reserved */
#define ASPEED_H2X_TX_DESC_DATA		0x20	/* PCI/NVMe: config write payload (e.g. NVMe BAR assignment) */
#define ASPEED_H2X_STS			0x24	/* PCI/NVMe: controller status; used to poll config TLP completion for NVMe */
#define  ASPEED_PCIE_TX_IDLE			BIT(31)	/* PCI/NVMe: set when config TLP engine is idle; NVMe config read/write waits for this */
#define  ASPEED_PCIE_STATUS_OF_TX		GENMASK(25, 24)	/* PCI/NVMe: outcome of the config TLP sent to NVMe */
#define	ASPEED_PCIE_RC_H_TX_COMPLETE		BIT(25)	/* PCI/NVMe: RC TLP completed; config read from NVMe returned */
#define  ASPEED_PCIE_TRIGGER_TX			BIT(0)	/* PCI/NVMe: kick the config TLP engine to access NVMe config space */
#define ASPEED_H2X_AHB_ADDR_CONFIG0	0x60	/* PCI/NVMe: AHB->PCIe outbound window low address for NVMe MMIO BAR mapping */
#define  ASPEED_AHB_REMAP_LO_ADDR(x)		(x & GENMASK(15, 4))	/* PCI/NVMe: low nibble of outbound address for NVMe BAR aperture */
#define  ASPEED_AHB_MASK_LO_ADDR(x)		FIELD_PREP(GENMASK(31, 20), x)	/* PCI/NVMe: mask selecting which AHB window maps to NVMe BAR space */
#define ASPEED_H2X_AHB_ADDR_CONFIG1	0x64	/* PCI/NVMe: outbound window high address for 64-bit NVMe BAR accesses */
#define  ASPEED_AHB_REMAP_HI_ADDR(x)		(x)	/* PCI/NVMe: upper 32 bits of PCIe address for NVMe BAR mapping */
#define ASPEED_H2X_AHB_ADDR_CONFIG2	0x68	/* PCI/NVMe: outbound window high mask for 64-bit NVMe BAR mapping */
#define  ASPEED_AHB_MASK_HI_ADDR(x)		(x)
#define ASPEED_H2X_DEV_CTRL		0xc0	/* PCI/NVMe: RC receive control; enables TLP reception from NVMe and MSI routing */
#define  ASPEED_PCIE_RX_DMA_EN			BIT(9)	/* PCI/NVMe: enables upstream DMA writes (NVMe read completions / MSI) into host memory */
#define  ASPEED_PCIE_RX_LINEAR			BIT(8)	/* PCI/NVMe: linear buffer mode for incoming TLPs from NVMe */
#define  ASPEED_PCIE_RX_MSI_SEL			BIT(7)	/* PCI/NVMe: route MSI TLPs from NVMe to dedicated MSI logic instead of memory write */
#define  ASPEED_PCIE_RX_MSI_EN			BIT(6)	/* PCI/NVMe: accept MSI TLPs from NVMe (used when nvme_probe enables MSI/MSI-X) */
#define  ASPEED_PCIE_UNLOCK_RX_BUFF		BIT(4)	/* PCI/NVMe: release RX buffer after a config read/write to NVMe */
#define  ASPEED_PCIE_WAIT_RX_TLP_CLR		BIT(2)	/* PCI/NVMe: wait for previous RX TLP to clear before next NVMe config access */
#define  ASPEED_PCIE_RC_RX_ENABLE		BIT(1)	/* PCI/NVMe: enable RC TLP reception (completion TLPs from NVMe config reads) */
#define  ASPEED_PCIE_RC_ENABLE			BIT(0)	/* PCI/NVMe: enable the RC; required before any NVMe enumeration/register access */
#define ASPEED_H2X_DEV_STS		0xc8	/* PCI/NVMe: RC status including RX-done interrupt for NVMe config completions */
#define  ASPEED_PCIE_RC_RX_DONE_ISR		BIT(4)	/* PCI/NVMe: RX completion TLP received after a config read from NVMe */
#define ASPEED_H2X_DEV_RX_DESC_DATA	0xcc	/* PCI/NVMe: completion data returned from NVMe config read */
#define ASPEED_H2X_DEV_RX_DESC1		0xd4	/* PCI/NVMe: completion header including status for NVMe config access */
#define ASPEED_H2X_DEV_TX_TAG		0xfc	/* PCI/NVMe: tag number used to match NVMe config completion TLPs */
#define  ASPEED_RC_TLP_TX_TAG_NUM		0x28	/* PCI/NVMe: number of outstanding config tags; impacts NVMe enumeration throughput */

/* AST2700 H2X */
#define ASPEED_H2X_CTRL			0x00	/* PCI/NVMe: bridge control for AST2700 RC that connects to NVMe */
#define  ASPEED_H2X_BRIDGE_EN			BIT(0)	/* PCI/NVMe: enable H2X bridge; must be set before NVMe config accesses work */
#define  ASPEED_H2X_BRIDGE_DIRECT_EN		BIT(1)	/* PCI/NVMe: direct bridge mode used on AST2700 for lower-latency NVMe access */
#define ASPEED_H2X_CFGE_INT_STS		0x08	/* PCI/NVMe: config engine status for AST2700 NVMe enumeration */
#define  ASPEED_CFGE_TX_IDLE			BIT(0)	/* PCI/NVMe: AST2700 config TLP engine idle; NVMe config read/write done */
#define  ASPEED_CFGE_RX_BUSY			BIT(1)	/* PCI/NVMe: AST2700 config completion received from NVMe/endpoint */
#define ASPEED_H2X_CFGI_TLP		0x20	/* PCI/NVMe: local RC config TLP register for AST2700 (root port internal config) */
#define  ASPEED_CFGI_BYTE_EN_MASK		GENMASK(19, 16)	/* PCI/NVMe: byte-enable mask for AST2700 config read/write */
#define  ASPEED_CFGI_BYTE_EN(x) \
			FIELD_PREP(ASPEED_CFGI_BYTE_EN_MASK, (x))	/* PCI/NVMe: encode byte enables for NVMe config access */
#define ASPEED_H2X_CFGI_WR_DATA		0x24	/* PCI/NVMe: local root port config write data (not NVMe endpoint) */
#define  ASPEED_CFGI_WRITE			BIT(20)	/* PCI/NVMe: local root port config write direction flag */
#define ASPEED_H2X_CFGI_CTRL		0x28	/* PCI/NVMe: local config engine control for AST2700 root port */
#define  ASPEED_CFGI_TLP_FIRE			BIT(0)	/* PCI/NVMe: trigger local root port config access */
#define ASPEED_H2X_CFGI_RET_DATA	0x2c	/* PCI/NVMe: returned local root port config data */
#define ASPEED_H2X_CFGE_TLP_1ST		0x30	/* PCI/NVMe: external config TLP DW0 for AST2700 NVMe enumeration */
#define ASPEED_H2X_CFGE_TLP_NEXT	0x34	/* PCI/NVMe: external config TLP DW1..n for AST2700 NVMe config access */
#define ASPEED_H2X_CFGE_CTRL		0x38	/* PCI/NVMe: external config engine control for AST2700 */
#define  ASPEED_CFGE_TLP_FIRE			BIT(0)	/* PCI/NVMe: trigger external config TLP to NVMe endpoint */
#define ASPEED_H2X_CFGE_RET_DATA	0x3c	/* PCI/NVMe: data returned from external NVMe config read on AST2700 */
#define ASPEED_H2X_REMAP_PREF_ADDR	0x70	/* PCI/NVMe: prefetchable 64-bit BAR remap high address for NVMe devices */
#define  ASPEED_REMAP_PREF_ADDR_63_32(x)	(x)	/* PCI/NVMe: upper 32 bits of prefetchable memory aperture for NVMe BARs */
#define ASPEED_H2X_REMAP_PCI_ADDR_HI	0x74	/* PCI/NVMe: outbound memory window upper 32 bits for NVMe BAR accesses */
#define  ASPEED_REMAP_PCI_ADDR_63_32(x)		(((x) >> 32) & GENMASK(31, 0))	/* PCI/NVMe: extract bits [63:32] for 64-bit NVMe BAR mapping */
#define ASPEED_H2X_REMAP_PCI_ADDR_LO	0x78	/* PCI/NVMe: outbound memory window lower 32 bits for NVMe BAR accesses */
#define  ASPEED_REMAP_PCI_ADDR_31_12(x)		((x) & GENMASK(31, 12))	/* PCI/NVMe: extract bits [31:12] of PCIe address mapped to NVMe BAR */

/* AST2700 SCU */
#define ASPEED_SCU_60			0x60	/* PCI/NVMe: SoC routing control enabling upstream/downstream paths for NVMe */
#define  ASPEED_RC_E2M_PATH_EN			BIT(0)	/* PCI/NVMe: enable CPU -> memory path used by NVMe DMA and MSI writes */
#define  ASPEED_RC_H2XS_PATH_EN			BIT(16)	/* PCI/NVMe: enable H2X slave path for NVMe config completions */
#define  ASPEED_RC_H2XD_PATH_EN			BIT(17)	/* PCI/NVMe: enable H2X debug path (unused for normal NVMe operation) */
#define  ASPEED_RC_H2XX_PATH_EN			BIT(18)	/* PCI/NVMe: enable H2X cross path for NVMe traffic */
#define  ASPEED_RC_UPSTREAM_MEM_EN		BIT(19)	/* PCI/NVMe: enable upstream memory writes from NVMe (DMA completions/MSI) */
#define ASPEED_SCU_64			0x64	/* PCI/NVMe: decode DMA base/limit windows for NVMe upstream traffic */
#define  ASPEED_RC0_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(7, 0), x)	/* PCI/NVMe: start of DMA decode window for RC0 (used by NVMe DMA) */
#define  ASPEED_RC0_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(15, 8), x)	/* PCI/NVMe: end of DMA decode window for RC0 (restricts NVMe DMA) */
#define  ASPEED_RC1_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(23, 16), x)	/* PCI/NVMe: start of DMA decode window for RC1 (used by NVMe DMA) */
#define  ASPEED_RC1_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(31, 24), x)	/* PCI/NVMe: end of DMA decode window for RC1 (restricts NVMe DMA) */
#define ASPEED_SCU_70			0x70	/* PCI/NVMe: disable endpoint function mode; RC mode required for NVMe enumeration */
#define  ASPEED_DISABLE_EP_FUNC			0	/* PCI/NVMe: force RC mode so the port can enumerate an NVMe SSD */

/* Macro to combine Fmt and Type into the 8-bit field */
#define ASPEED_TLP_FMT_TYPE(fmt, type)	((((fmt) & 0x7) << 5) | ((type) & 0x1f))	/* PCI/NVMe: build PCIe TLP fmt/type for NVMe config requests */
#define ASPEED_TLP_COMMON_FIELDS	GENMASK(31, 24)	/* PCI/NVMe: byte in DW0 holding fmt/type for NVMe config TLPs */

/* Completion status */
#define CPL_STS(x)	FIELD_GET(GENMASK(15, 13), (x))	/* PCI/NVMe: extract completion status from NVMe config-read completion TLP */
/* TLP configuration type 0 and type 1 */
#define CFG0_READ_FMTTYPE                                        \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                     \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_NO_DATA, \
				       PCIE_TLP_TYPE_CFG0_RD))	/* PCI/NVMe: TLP to read config of NVMe on the root bus (root port) */
#define CFG0_WRITE_FMTTYPE                                    \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                  \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_DATA, \
				       PCIE_TLP_TYPE_CFG0_WR))	/* PCI/NVMe: TLP to write config of NVMe on the root bus (root port) */
#define CFG1_READ_FMTTYPE                                        \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                     \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_NO_DATA, \
				       PCIE_TLP_TYPE_CFG1_RD))	/* PCI/NVMe: TLP to read config of NVMe behind a switch/bridge (downstream) */
#define CFG1_WRITE_FMTTYPE                                    \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                  \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_DATA, \
				       PCIE_TLP_TYPE_CFG1_WR))	/* PCI/NVMe: TLP to write config of NVMe behind a switch/bridge (downstream) */
#define CFG_PAYLOAD_SIZE		0x01 /* 1 DWORD */	/* PCI/NVMe: config TLP payload is one DWORD (matches NVMe config register size) */
#define TLP_HEADER_BYTE_EN(x, y)	((GENMASK((x) - 1, 0) << ((y) % 4)))	/* PCI/NVMe: byte-enable field for unaligned NVMe config access */
#define TLP_GET_VALUE(x, y, z)	\
	(((x) >> ((((z) % 4)) * 8)) & GENMASK((8 * (y)) - 1, 0))	/* PCI/NVMe: extract requested bytes from NVMe config-read completion DWORD */
#define TLP_SET_VALUE(x, y, z)	\
	((((x) & GENMASK((8 * (y)) - 1, 0)) << ((((z) % 4)) * 8)))	/* PCI/NVMe: insert requested bytes into NVMe config-write payload DWORD */
#define AST2600_TX_DESC1_VALUE		0x00002000	/* PCI/NVMe: fixed requester ID bits for AST2600 config TLPs to NVMe */
#define AST2700_TX_DESC1_VALUE		0x00401000	/* PCI/NVMe: fixed requester ID bits for AST2700 config TLPs to NVMe */

/**
 * struct aspeed_pcie_port - PCIe port information
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @clk: pointer to the port clock gate
 * @phy: pointer to PCIe PHY
 * @perst: pointer to port reset control
 * @slot: port slot
 */
struct aspeed_pcie_port {
	struct list_head list;		/* NVMe: links all RC ports that may connect NVMe SSDs */
	struct aspeed_pcie *pcie;	/* NVMe: back pointer to the host bridge used during NVMe enumeration */
	struct clk *clk;		/* NVMe: per-port clock; must be enabled before NVMe link training */
	struct phy *phy;		/* NVMe: PHY handle; configures the SerDes for the NVMe SSD */
	struct reset_control *perst;	/* NVMe: PERST# for this slot; asserted/deasserted during NVMe reset/hotplug */
	u32 slot;			/* NVMe: physical slot number reported in DT; used for logging during NVMe probe */
};

/**
 * struct aspeed_pcie - PCIe RC information
 * @host: pointer to PCIe host bridge
 * @dev: pointer to device structure
 * @reg: PCIe host register base address
 * @ahbc: pointer to AHHC register map
 * @cfg: pointer to Aspeed PCIe configuration register map
 * @platform: platform specific information
 * @ports: list of PCIe ports
 * @tx_tag: current TX tag for the port
 * @root_bus_nr: bus number of the host bridge
 * @h2xrst: pointer to H2X reset control
 * @intx_domain: IRQ domain for INTx interrupts
 * @msi_domain: IRQ domain for MSI interrupts
 * @lock: mutex to protect MSI bitmap variable
 * @msi_irq_in_use: bitmap to track used MSI host IRQs
 * @clear_msi_twice: AST2700 workaround to clear MSI status twice
 */
struct aspeed_pcie {
	struct pci_host_bridge *host;	/* NVMe: core host bridge; pci_host_probe() walks this to discover NVMe SSDs */
	struct device *dev;		/* NVMe: device pointer used for devm_* allocations and DMA/IOMMU configuration */
	void __iomem *reg;		/* NVMe: MMIO base of the H2X/PCIe controller; used for config TLPs and MSI setup */
	struct regmap *ahbc;		/* NVMe: AHBC syscon for upstream DMA decode (AST2600 NVMe DMA path) */
	struct regmap *cfg;		/* NVMe: SCU syscon for SoC-level routing (AST2700 NVMe DMA path) */
	const struct aspeed_pcie_rc_platform *platform;	/* NVMe: per-SoC callbacks and register offsets for NVMe host setup */
	struct list_head ports;		/* NVMe: head of aspeed_pcie_port list; each port can host an NVMe SSD */

	u8 tx_tag;			/* NVMe: rotating tag for config TLPs so completions from NVMe can be matched */
	u8 root_bus_nr;			/* NVMe: root bus number; CFG0 vs CFG1 selection depends on NVMe being on root bus or downstream */

	struct reset_control *h2xrst;	/* NVMe: H2X block reset; asserted before enabling NVMe link */

	struct irq_domain *intx_domain;	/* NVMe: maps legacy INTx#A-D from NVMe to Linux IRQs when MSI is unavailable */
	struct irq_domain *msi_domain;	/* NVMe: maps MSI/MSI-X vectors requested by nvme_probe to Linux IRQs */
	struct mutex lock;		/* NVMe: serializes allocation of MSI vectors among multiple NVMe devices/queues */
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_HOST_IRQS);	/* NVMe: tracks which MSI host IRQs are assigned to NVMe queues */

	bool clear_msi_twice;		/* AST2700 workaround */	/* NVMe: on AST2700 MSI status must be cleared twice to avoid losing NVMe interrupts */
};

/**
 * struct aspeed_pcie_rc_platform - Platform information
 * @setup: initialization function
 * @pcie_map_ranges: function to map PCIe address ranges
 * @reg_intx_en: INTx enable register offset
 * @reg_intx_sts: INTx status register offset
 * @reg_msi_en: MSI enable register offset
 * @reg_msi_sts: MSI enable register offset
 * @msi_address: HW fixed MSI address
 */
struct aspeed_pcie_rc_platform {
	int (*setup)(struct platform_device *pdev);	/* NVMe: SoC-specific RC init; sets up config ops that pci.c uses for NVMe */
	void (*pcie_map_ranges)(struct aspeed_pcie *pcie, u64 pci_addr);	/* NVMe: programs outbound window so NVMe BAR0/1 map to host memory */
	int reg_intx_en;	/* NVMe: offset of INTx enable register used to mask/unmask legacy NVMe interrupts */
	int reg_intx_sts;	/* NVMe: offset of INTx status register for legacy NVMe interrupt ack */
	int reg_msi_en;		/* NVMe: offset of MSI enable register controlling which MSI vectors NVMe may raise */
	int reg_msi_sts;	/* NVMe: offset of MSI status register showing pending NVMe MSI vectors */
	u32 msi_address;	/* NVMe: fixed MSI target address that NVMe device writes to signal an interrupt */
};

static void aspeed_pcie_intx_irq_ack(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);	/* NVMe: retrieve RC instance for this legacy NVMe INTx line */
	int intx_en = pcie->platform->reg_intx_en;	/* NVMe: cache INTx enable register offset for this SoC */
	u32 en;

	en = readl(pcie->reg + intx_en);	/* NVMe: read current INTx mask for legacy NVMe interrupts */
	en |= BIT(d->hwirq);			/* NVMe: set the bit corresponding to the NVMe INTx line being acked */
	writel(en, pcie->reg + intx_en);	/* NVMe: re-enable INTx so NVMe can assert the next completion interrupt */
}

static void aspeed_pcie_intx_irq_mask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);	/* NVMe: RC instance owning this legacy NVMe INTx */
	int intx_en = pcie->platform->reg_intx_en;	/* NVMe: INTx enable register offset for masking NVMe interrupts */
	u32 en;

	en = readl(pcie->reg + intx_en);	/* NVMe: read current INTx enable/mask state for NVMe device */
	en &= ~BIT(d->hwirq);			/* NVMe: clear enable bit to mask this INTx from the NVMe SSD */
	writel(en, pcie->reg + intx_en);	/* NVMe: apply mask; NVMe legacy interrupts are now blocked */
}

static void aspeed_pcie_intx_irq_unmask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);	/* NVMe: RC instance owning this legacy NVMe INTx */
	int intx_en = pcie->platform->reg_intx_en;	/* NVMe: INTx enable register offset for unmasking NVMe interrupts */
	u32 en;

	en = readl(pcie->reg + intx_en);	/* NVMe: read current INTx enable state for NVMe device */
	en |= BIT(d->hwirq);			/* NVMe: set enable bit to allow this INTx from the NVMe SSD */
	writel(en, pcie->reg + intx_en);	/* NVMe: apply unmask; NVMe legacy interrupts can now fire */
}

static struct irq_chip aspeed_intx_irq_chip = {
	.name = "INTx",				/* NVMe: human-readable name for legacy NVMe INTx chip */
	.irq_ack = aspeed_pcie_intx_irq_ack,	/* NVMe: re-enable INTx after handling a legacy NVMe completion interrupt */
	.irq_mask = aspeed_pcie_intx_irq_mask,	/* NVMe: disable legacy NVMe INTx when driver masks the IRQ */
	.irq_unmask = aspeed_pcie_intx_irq_unmask,	/* NVMe: enable legacy NVMe INTx when driver unmasks the IRQ */
};

static int aspeed_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &aspeed_intx_irq_chip, handle_level_irq);	/* NVMe: bind Linux IRQ to INTx chip and level handler for NVMe */
	irq_set_chip_data(irq, domain->host_data);	/* NVMe: store aspeed_pcie pointer so INTx callbacks know which NVMe RC */
	irq_set_status_flags(irq, IRQ_LEVEL);		/* NVMe: mark IRQ as level-triggered, typical for PCIe INTx from NVMe */

	return 0;
}

static const struct irq_domain_ops aspeed_intx_domain_ops = {
	.map = aspeed_pcie_intx_map,	/* NVMe: maps a Linux virq to an INTx hwirq for a legacy NVMe interrupt */
};

static irqreturn_t aspeed_pcie_intr_handler(int irq, void *dev_id)
{
	struct aspeed_pcie *pcie = dev_id;	/* NVMe: RC instance receiving the combined interrupt from NVMe endpoints */
	const struct aspeed_pcie_rc_platform *platform = pcie->platform;	/* NVMe: SoC-specific register offsets for NVMe interrupts */
	unsigned long status;	/* NVMe: pending MSI status bits, one per possible NVMe MSI vector */
	unsigned long intx;	/* NVMe: pending INTx status bits from NVMe or other endpoints */
	u32 bit;
	int i;

	intx = FIELD_GET(ASPEED_PCIE_INTX_STS,
				 readl(pcie->reg + platform->reg_intx_sts));	/* NVMe: read INTx status (legacy interrupts from NVMe) */
	for_each_set_bit(bit, &intx, PCI_NUM_INTX)
		generic_handle_domain_irq(pcie->intx_domain, bit);	/* NVMe: dispatch legacy INTx A/B/C/D to NVMe MSI handler fallback path */

	for (i = 0; i < 2; i++) {
		int msi_sts_reg = platform->reg_msi_sts + (i * 4);	/* NVMe: each 32-bit MSI status register covers 32 NVMe MSI vectors */

		status = readl(pcie->reg + msi_sts_reg);	/* NVMe: read pending MSI vectors from NVMe device */
		writel(status, pcie->reg + msi_sts_reg);	/* NVMe: clear pending MSI status so NVMe can raise the next interrupt */

		/*
		 * AST2700 workaround:
		 * The MSI status needs to clear one more time.
		 */
		if (pcie->clear_msi_twice)
			writel(status, pcie->reg + msi_sts_reg);	/* NVMe: AST2700 silicon quirk; without this NVMe MSI can stick */

		for_each_set_bit(bit, &status, 32) {
			bit += (i * 32);	/* NVMe: compute global MSI vector number used by NVMe queue interrupt */
			generic_handle_domain_irq(pcie->msi_domain, bit);	/* NVMe: route MSI to NVMe driver's per-queue interrupt handler */
		}
	}

	return IRQ_HANDLED;	/* NVMe: tell Linux the combined NVMe/PCIe interrupt was handled */
}

static u32 aspeed_pcie_get_bdf_offset(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	return ((bus->number) << 24) | (PCI_SLOT(devfn) << 19) |
		(PCI_FUNC(devfn) << 16) | (where & ~3);	/* PCI/NVMe: encode bus/dev/fn/reg for config TLP targeting NVMe or switch */
}

static int aspeed_ast2600_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val, u32 fmt_type,
			       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;	/* NVMe: RC instance behind this pci_bus; used for config access to NVMe */
	u32 bdf_offset, cfg_val, isr;
	int ret;

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);	/* NVMe: build config-space address of NVMe device */

	/* Driver may set unlock RX buffer before triggering next TX config */
	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_CTRL);	/* NVMe: read RC control to unlock RX buffer before NVMe config TLP */
	writel(ASPEED_PCIE_UNLOCK_RX_BUFF | cfg_val,
	       pcie->reg + ASPEED_H2X_DEV_CTRL);	/* NVMe: unlock RX buffer so the previous NVMe completion can be discarded */

	cfg_val = fmt_type | CFG_PAYLOAD_SIZE;
	writel(cfg_val, pcie->reg + ASPEED_H2X_TX_DESC0);	/* NVMe: write TLP DW0 (cfg read/write type, 1-DWORD payload) for NVMe */

	cfg_val = AST2600_TX_DESC1_VALUE |
		  FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  TLP_HEADER_BYTE_EN(size, where);	/* NVMe: build TLP DW1 with tag and byte-enables for this NVMe config access */
	writel(cfg_val, pcie->reg + ASPEED_H2X_TX_DESC1);	/* NVMe: write TLP DW1 for NVMe config transaction */

	writel(bdf_offset, pcie->reg + ASPEED_H2X_TX_DESC2);	/* NVMe: write TLP DW2 with NVMe BDF and register offset */
	writel(0, pcie->reg + ASPEED_H2X_TX_DESC3);		/* NVMe: TLP DW3 reserved/zero for 3-DW config header */
	if (write)
		writel(TLP_SET_VALUE(*val, size, where),
		       pcie->reg + ASPEED_H2X_TX_DESC_DATA);	/* NVMe: write config payload (e.g. NVMe BAR or command register) */

	cfg_val = readl(pcie->reg + ASPEED_H2X_STS);
	cfg_val |= ASPEED_PCIE_TRIGGER_TX;
	writel(cfg_val, pcie->reg + ASPEED_H2X_STS);	/* NVMe: fire config TLP to read/write NVMe config space */

	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_STS, cfg_val,
				 (cfg_val & ASPEED_PCIE_TX_IDLE), 0, 50);	/* NVMe: wait for NVMe config TLP to complete (50us timeout) */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), cfg_val);
		ret = PCIBIOS_SET_FAILED;	/* NVMe: tell pci.c the config access to NVMe failed */
		PCI_SET_ERROR_RESPONSE(val);	/* NVMe: pci.c expects ~0 on failure when reading NVMe config space */
		goto out;
	}

	cfg_val = readl(pcie->reg + ASPEED_H2X_INT_STS);
	cfg_val |= ASPEED_PCIE_TX_IDLE_CLEAR;
	writel(cfg_val, pcie->reg + ASPEED_H2X_INT_STS);	/* NVMe: clear transmit-done status before reading NVMe completion */

	cfg_val = readl(pcie->reg + ASPEED_H2X_STS);
	switch (cfg_val & ASPEED_PCIE_STATUS_OF_TX) {
	case ASPEED_PCIE_RC_H_TX_COMPLETE:
		ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_DEV_STS, isr,
					 (isr & ASPEED_PCIE_RC_RX_DONE_ISR), 0,
					 50);	/* NVMe: wait for completion TLP from NVMe config read (50us timeout) */
		if (ret) {
			dev_err(pcie->dev,
				"%02x:%02x.%d CR rx timeout sts: 0x%08x\n",
				bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), isr);
			ret = PCIBIOS_SET_FAILED;	/* NVMe: NVMe completion never arrived; report failure to pci.c */
			PCI_SET_ERROR_RESPONSE(val);	/* NVMe: return error response to NVMe driver/config read */
			goto out;
		}
		if (!write) {
			cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_RX_DESC1);	/* NVMe: read completion header for NVMe config read */
			if (CPL_STS(cfg_val) != PCIE_CPL_STS_SUCCESS) {
				ret = PCIBIOS_SET_FAILED;	/* NVMe: completion status indicates NVMe did not respond; fail the read */
				PCI_SET_ERROR_RESPONSE(val);	/* NVMe: signal error response so pci.c treats NVMe as absent */
				goto out;
			} else {
				*val = readl(pcie->reg +
					     ASPEED_H2X_DEV_RX_DESC_DATA);	/* NVMe: extract data returned from NVMe config space */
			}
		}
		break;
	case ASPEED_PCIE_STATUS_OF_TX:
		ret = PCIBIOS_SET_FAILED;	/* NVMe: TX status error; config access to NVMe failed */
		PCI_SET_ERROR_RESPONSE(val);	/* NVMe: return error response for NVMe config read */
		goto out;
	default:
		*val = readl(pcie->reg + ASPEED_H2X_HOST_RX_DESC_DATA);	/* NVMe: fallback data path (rare) for NVMe config read */
		break;
	}

	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_CTRL);
	cfg_val |= ASPEED_PCIE_UNLOCK_RX_BUFF;
	writel(cfg_val, pcie->reg + ASPEED_H2X_DEV_CTRL);	/* NVMe: release RX buffer now that NVMe completion is consumed */

	*val = TLP_GET_VALUE(*val, size, where);	/* NVMe: shift/mask completion DWORD to the exact bytes requested by pci.c */

	ret = PCIBIOS_SUCCESSFUL;	/* NVMe: config access to NVMe succeeded */
out:
	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_STS);
	writel(cfg_val, pcie->reg + ASPEED_H2X_DEV_STS);	/* NVMe: clear RC status for next NVMe config transaction */
	pcie->tx_tag = (pcie->tx_tag + 1) % 0x8;	/* NVMe: advance tag so the next NVMe config TLP has a new transaction ID */
	return ret;	/* NVMe: return pci.c status: PCIBIOS_SUCCESSFUL or error */
}

static int aspeed_ast2600_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	/*
	 * AST2600 has only one Root Port on the root bus.
	 */
	if (PCI_SLOT(devfn) != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: root bus only has the built-in root port at slot 8; no NVMe device here yet */

	return aspeed_ast2600_conf(bus, devfn, where, size, val,
				   CFG0_READ_FMTTYPE, false);	/* NVMe: issue type-0 config read to the root port itself during enumeration */
}

static int aspeed_ast2600_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, val,
				   CFG1_READ_FMTTYPE, false);	/* NVMe: type-1 config read targeting NVMe SSD or switch behind the root port */
}

static int aspeed_ast2600_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	/*
	 * AST2600 has only one Root Port on the root bus.
	 */
	if (PCI_SLOT(devfn) != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: root bus slot != 8 is invalid on AST2600 root port */

	return aspeed_ast2600_conf(bus, devfn, where, size, &val,
				   CFG0_WRITE_FMTTYPE, true);	/* NVMe: type-0 config write to root port (e.g. bridge control) before NVMe is set up */
}

static int aspeed_ast2600_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, &val,
				   CFG1_WRITE_FMTTYPE, true);	/* NVMe: type-1 config write to NVMe SSD config space (e.g. BAR, command, MSI) */
}

static int aspeed_ast2700_config(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val, bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;	/* NVMe: RC instance used for local root port config access on AST2700 */
	u32 cfg_val;

	cfg_val = ASPEED_CFGI_BYTE_EN(TLP_HEADER_BYTE_EN(size, where)) |
		  (where & ~3);	/* NVMe: build byte-enable and register offset for local root port config access */
	if (write)
		cfg_val |= ASPEED_CFGI_WRITE;	/* NVMe: mark as write when programming local root port registers */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGI_TLP);	/* NVMe: issue local config TLP to the AST2700 root port itself */

	writel(TLP_SET_VALUE(*val, size, where),
	       pcie->reg + ASPEED_H2X_CFGI_WR_DATA);	/* NVMe: write local root port config payload */
	writel(ASPEED_CFGI_TLP_FIRE, pcie->reg + ASPEED_H2X_CFGI_CTRL);	/* NVMe: trigger local root port config access */
	*val = readl(pcie->reg + ASPEED_H2X_CFGI_RET_DATA);	/* NVMe: read back local root port config data */
	*val = TLP_GET_VALUE(*val, size, where);	/* NVMe: align return value to requested byte offset */

	return PCIBIOS_SUCCESSFUL;	/* NVMe: local root port config access completed (used before NVMe endpoint enumeration) */
}

static int aspeed_ast2700_child_config(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 *val,
				       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;	/* NVMe: RC instance behind this bus for AST2700 */
	u32 bdf_offset, status, cfg_val;
	int ret;

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);	/* NVMe: BDF+offset identifying the NVMe endpoint on AST2700 */

	cfg_val = CFG_PAYLOAD_SIZE;
	if (write)
		cfg_val |= (bus->number == (pcie->root_bus_nr + 1)) ?
				   CFG0_WRITE_FMTTYPE :
				   CFG1_WRITE_FMTTYPE;	/* NVMe: choose type-0 for first downstream bus or type-1 for deeper NVMe hierarchy */
	else
		cfg_val |= (bus->number == (pcie->root_bus_nr + 1)) ?
				   CFG0_READ_FMTTYPE :
				   CFG1_READ_FMTTYPE;	/* NVMe: choose type-0/type-1 config read for NVMe endpoint or switch */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGE_TLP_1ST);	/* NVMe: write first DW of external config TLP for NVMe */

	cfg_val = AST2700_TX_DESC1_VALUE |
		  FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  TLP_HEADER_BYTE_EN(size, where);	/* NVMe: build second DW with tag and byte enables for NVMe config access */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);	/* NVMe: write second DW of external config TLP */

	writel(bdf_offset, pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);	/* NVMe: write third DW with NVMe BDF and register offset */
	if (write)
		writel(TLP_SET_VALUE(*val, size, where),
		       pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);	/* NVMe: write config payload for NVMe endpoint write */
	writel(ASPEED_CFGE_TX_IDLE | ASPEED_CFGE_RX_BUSY,
	       pcie->reg + ASPEED_H2X_CFGE_INT_STS);	/* NVMe: clear status bits before firing NVMe config TLP */
	writel(ASPEED_CFGE_TLP_FIRE, pcie->reg + ASPEED_H2X_CFGE_CTRL);	/* NVMe: fire external config TLP to access NVMe config space */

	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_CFGE_INT_STS, status,
				 (status & ASPEED_CFGE_TX_IDLE), 0, 50);	/* NVMe: wait for NVMe config TLP transmit idle (50us) */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;	/* NVMe: config TLP to NVMe timed out */
		PCI_SET_ERROR_RESPONSE(val);	/* NVMe: error response to pci.c NVMe enumeration */
		goto out;
	}

	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_CFGE_INT_STS, status,
				 (status & ASPEED_CFGE_RX_BUSY), 0, 50);	/* NVMe: wait for NVMe config completion TLP arrival (50us) */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR rx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;	/* NVMe: NVMe completion never arrived */
		PCI_SET_ERROR_RESPONSE(val);	/* NVMe: error response for NVMe config read */
		goto out;
	}
	*val = readl(pcie->reg + ASPEED_H2X_CFGE_RET_DATA);	/* NVMe: read completion data from NVMe config space */
	*val = TLP_GET_VALUE(*val, size, where);	/* NVMe: adjust completion data to requested byte offset */

	ret = PCIBIOS_SUCCESSFUL;	/* NVMe: AST2700 config access to NVMe succeeded */
out:
	writel(status, pcie->reg + ASPEED_H2X_CFGE_INT_STS);	/* NVMe: clear config engine status for next NVMe access */
	pcie->tx_tag = (pcie->tx_tag + 1) % 0xf;	/* NVMe: advance transaction tag for next NVMe config TLP */
	return ret;	/* NVMe: return result to pci.c config accessor */
}

static int aspeed_ast2700_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	/*
	 * AST2700 has only one Root Port on the root bus.
	 */
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: AST2700 root port is function 0 on root bus; other devfn invalid */

	return aspeed_ast2700_config(bus, devfn, where, size, val, false);	/* NVMe: local root port config read before NVMe enumeration */
}

static int aspeed_ast2700_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, val, false);	/* NVMe: external config read to NVMe endpoint or switch on AST2700 */
}

static int aspeed_ast2700_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	/*
	 * AST2700 has only one Root Port on the root bus.
	 */
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: only root port function 0 is valid on AST2700 root bus */

	return aspeed_ast2700_config(bus, devfn, where, size, &val, true);	/* NVMe: local root port config write before NVMe enumeration */
}

static int aspeed_ast2700_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, &val, true);	/* NVMe: external config write to NVMe endpoint/switch on AST2700 */
}

static struct pci_ops aspeed_ast2600_pcie_ops = {
	.read = aspeed_ast2600_rd_conf,	/* NVMe: root-bus config read used during initial NVMe root port enumeration */
	.write = aspeed_ast2600_wr_conf,	/* NVMe: root-bus config write used during initial NVMe root port setup */
};

static struct pci_ops aspeed_ast2600_pcie_child_ops = {
	.read = aspeed_ast2600_child_rd_conf,	/* NVMe: downstream config read used to discover NVMe SSD and read its BARs */
	.write = aspeed_ast2600_child_wr_conf,	/* NVMe: downstream config write used to assign NVMe BARs and enable MSI */
};

static struct pci_ops aspeed_ast2700_pcie_ops = {
	.read = aspeed_ast2700_rd_conf,	/* NVMe: AST2700 root-bus config read for root port before NVMe enumeration */
	.write = aspeed_ast2700_wr_conf,	/* NVMe: AST2700 root-bus config write for root port before NVMe enumeration */
};

static struct pci_ops aspeed_ast2700_pcie_child_ops = {
	.read = aspeed_ast2700_child_rd_conf,	/* NVMe: AST2700 downstream config read for NVMe SSD/switch discovery */
	.write = aspeed_ast2700_child_wr_conf,	/* NVMe: AST2700 downstream config write for NVMe BAR assignment and MSI setup */
};

static void aspeed_irq_compose_msi_msg(struct irq_data *data,
				       struct msi_msg *msg)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);	/* NVMe: RC instance owning the MSI vector assigned to an NVMe queue */

	msg->address_hi = 0;	/* NVMe: fixed MSI target address is 32-bit on Aspeed; upper half zero */
	msg->address_lo = pcie->platform->msi_address;	/* NVMe: hardware-fixed address the NVMe SSD writes to raise an MSI */
	msg->data = data->hwirq;	/* NVMe: vector number written by NVMe device; maps to Linux IRQ for that queue */
}

static struct irq_chip aspeed_msi_bottom_irq_chip = {
	.name = "ASPEED MSI",				/* NVMe: name shown in /proc/interrupts for NVMe MSI vectors */
	.irq_compose_msi_msg = aspeed_irq_compose_msi_msg,	/* NVMe: builds MSI message programmed into NVMe device's MSI capability */
};

static int aspeed_irq_msi_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct aspeed_pcie *pcie = domain->host_data;	/* NVMe: RC instance from which NVMe MSI vectors are allocated */
	int bit;
	int i;

	guard(mutex)(&pcie->lock);	/* NVMe: serialize MSI bitmap access across multiple NVMe devices/queues */

	bit = bitmap_find_free_region(pcie->msi_irq_in_use, MAX_MSI_HOST_IRQS,
				      get_count_order(nr_irqs));	/* NVMe: find contiguous free MSI host IRQs for NVMe's requested vector count */

	if (bit < 0)
		return -ENOSPC;	/* NVMe: not enough host MSI vectors for all NVMe queues; pci_alloc_irq_vectors fails */

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, bit + i,
				    &aspeed_msi_bottom_irq_chip,
				    domain->host_data, handle_simple_irq, NULL,
				    NULL);	/* NVMe: bind each Linux virq to a hardware MSI bit and simple handler for NVMe queue interrupts */
	}

	return 0;	/* NVMe: MSI vector allocation succeeded for NVMe */
}

static void aspeed_irq_msi_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);	/* NVMe: get hardware bit for this NVMe MSI vector */
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);	/* NVMe: RC instance that owns the MSI bitmap */

	guard(mutex)(&pcie->lock);	/* NVMe: protect bitmap while releasing MSI vectors used by NVMe */

	bitmap_release_region(pcie->msi_irq_in_use, data->hwirq,
			      get_count_order(nr_irqs));	/* NVMe: free MSI host IRQs so another NVMe device or re-probe can reuse them */
}

static const struct irq_domain_ops aspeed_msi_domain_ops = {
	.alloc = aspeed_irq_msi_domain_alloc,	/* NVMe: called when NVMe driver requests MSI/MSI-X irq vectors */
	.free = aspeed_irq_msi_domain_free,	/* NVMe: called when NVMe driver releases MSI/MSI-X irq vectors */
};

#define ASPEED_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				  MSI_FLAG_USE_DEF_CHIP_OPS	| \
				  MSI_FLAG_NO_AFFINITY)	/* NVMe: required MSI parent flags; no affinity support for NVMe vectors on this RC */

#define ASPEED_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				   MSI_FLAG_MULTI_PCI_MSI	| \
				   MSI_FLAG_PCI_MSIX)	/* NVMe: supports multi-vector MSI and MSI-X used by modern NVMe SSDs for per-CPU queues */

static const struct msi_parent_ops aspeed_msi_parent_ops = {
	.required_flags		= ASPEED_MSI_FLAGS_REQUIRED,	/* NVMe: flags that must be set by the NVMe MSI domain */
	.supported_flags	= ASPEED_MSI_FLAGS_SUPPORTED,	/* NVMe: flags advertised to NVMe driver for MSI/MSI-X capabilities */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,	/* NVMe: this domain serves PCI MSI interrupts for NVMe devices */
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,	/* NVMe: enable standard MSI chip ack behavior for NVMe interrupts */
	.prefix			= "ASPEED-",	/* NVMe: irq name prefix for NVMe MSI interrupts in /proc/interrupts */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* NVMe: generic helper to initialize NVMe MSI device info */
};

static int aspeed_pcie_msi_init(struct aspeed_pcie *pcie)
{
	writel(~0, pcie->reg + pcie->platform->reg_msi_en);	/* NVMe: enable all MSI vectors so NVMe device can raise any assigned vector */
	writel(~0, pcie->reg + pcie->platform->reg_msi_en + 0x04);	/* NVMe: enable upper 32 MSI vectors for NVMe */
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts);	/* NVMe: clear any pending lower MSI status before NVMe probe */
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts + 0x04);	/* NVMe: clear any pending upper MSI status before NVMe probe */

	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(pcie->dev),	/* NVMe: firmware node used to identify this RC's MSI domain to NVMe */
		.ops		= &aspeed_msi_domain_ops,	/* NVMe: alloc/free callbacks for NVMe MSI vectors */
		.host_data	= pcie,	/* NVMe: aspeed_pcie pointer passed to MSI ops so they know which NVMe RC */
		.size		= MAX_MSI_HOST_IRQS,	/* NVMe: total MSI vectors available to all NVMe endpoints */
	};

	pcie->msi_domain = msi_create_parent_irq_domain(&info,
							&aspeed_msi_parent_ops);	/* NVMe: create MSI parent domain; NVMe pci.c will create child domains under this */
	if (!pcie->msi_domain)
		return dev_err_probe(pcie->dev, -ENOMEM,
				     "failed to create MSI domain\n");	/* NVMe: without MSI domain NVMe cannot use MSI/MSI-X interrupts */

	return 0;	/* NVMe: MSI domain ready for NVMe vector allocation */
}

static void aspeed_pcie_msi_free(struct aspeed_pcie *pcie)
{
	if (pcie->msi_domain) {
		irq_domain_remove(pcie->msi_domain);	/* NVMe: tear down MSI domain so NVMe cannot raise further MSI interrupts */
		pcie->msi_domain = NULL;	/* NVMe: mark domain removed */
	}
}

static void aspeed_pcie_irq_domain_free(void *d)
{
	struct aspeed_pcie *pcie = d;	/* NVMe: RC instance whose IRQ domains are being torn down */

	if (pcie->intx_domain) {
		irq_domain_remove(pcie->intx_domain);	/* NVMe: remove INTx domain used by legacy NVMe interrupts */
		pcie->intx_domain = NULL;	/* NVMe: mark INTx domain removed */
	}
	aspeed_pcie_msi_free(pcie);	/* NVMe: also remove MSI domain so NVMe interrupts are fully disabled */
}

static int aspeed_pcie_init_irq_domain(struct aspeed_pcie *pcie)
{
	int ret;

	pcie->intx_domain = irq_domain_add_linear(pcie->dev->of_node,
						  PCI_NUM_INTX,
						  &aspeed_intx_domain_ops,
						  pcie);	/* NVMe: create linear IRQ domain for 4 legacy INTx lines from NVMe */
	if (!pcie->intx_domain) {
		ret = dev_err_probe(pcie->dev, -ENOMEM,
				    "failed to get INTx IRQ domain\n");
		goto err;	/* NVMe: legacy interrupt fallback unavailable; continue if MSI works, but fail here */
	}

	writel(0, pcie->reg + pcie->platform->reg_intx_en);	/* NVMe: mask all INTx at boot; unmasked later only if NVMe falls back to legacy interrupts */
	writel(~0, pcie->reg + pcie->platform->reg_intx_sts);	/* NVMe: clear stale INTx status before NVMe device is probed */

	ret = aspeed_pcie_msi_init(pcie);
	if (ret)
		goto err;	/* NVMe: MSI domain creation failed; NVMe MSI/MSI-X not usable */

	return 0;	/* NVMe: both INTx and MSI domains ready for NVMe interrupt setup */
err:
	aspeed_pcie_irq_domain_free(pcie);
	return ret;	/* NVMe: propagate IRQ domain init failure; NVMe probe will not proceed */
}

static int aspeed_pcie_port_init(struct aspeed_pcie_port *port)
{
	struct aspeed_pcie *pcie = port->pcie;	/* NVMe: RC instance owning this NVMe slot */
	struct device *dev = pcie->dev;		/* NVMe: device used for error reporting during NVMe port init */
	int ret;

	ret = clk_prepare_enable(port->clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set clock for slot (%d)\n",
				     port->slot);	/* NVMe: clock failure prevents the link from training and NVMe from being detected */

	ret = phy_init(port->phy);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to init phy pcie for slot (%d)\n",
				     port->slot);	/* NVMe: PHY init failure means no electrical link to the NVMe SSD */

	ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_RC);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set phy mode for slot (%d)\n",
				     port->slot);	/* NVMe: must configure PHY as Root Complex for NVMe enumeration to work */

	reset_control_deassert(port->perst);	/* NVMe: release PERST# so the NVMe SSD can exit reset and train the link */
	msleep(PCIE_RESET_CONFIG_WAIT_MS);	/* NVMe: wait for NVMe link to train and become ready for config accesses */

	return 0;	/* NVMe: port is powered, PHY configured, and NVMe may be visible on the bus */
}

static void aspeed_host_reset(struct aspeed_pcie *pcie)
{
	reset_control_assert(pcie->h2xrst);	/* NVMe: assert H2X block reset, dropping any NVMe link */
	mdelay(ASPEED_RESET_RC_WAIT_MS);	/* NVMe: hold reset long enough for NVMe SSD to see it */
	reset_control_deassert(pcie->h2xrst);	/* NVMe: release reset so the RC can retrain and later enumerate NVMe */
}

static void aspeed_pcie_map_ranges(struct aspeed_pcie *pcie)
{
	struct pci_host_bridge *bridge = pcie->host;	/* NVMe: host bridge whose windows describe NVMe BAR aperture */
	struct resource_entry *window;

	resource_list_for_each_entry(window, &bridge->windows) {
		u64 pci_addr;

		if (resource_type(window->res) != IORESOURCE_MEM)
			continue;	/* NVMe: skip IO and bus resources; only memory windows matter for NVMe BAR mapping */

		pci_addr = window->res->start - window->offset;	/* NVMe: compute PCIe-side start address of the NVMe memory aperture */
		pcie->platform->pcie_map_ranges(pcie, pci_addr);	/* NVMe: program outbound mapping so CPU accesses reach NVMe BARs */
		break;	/* NVMe: program only the first memory window; subsequent ones not supported here */
	}
}

static void aspeed_ast2600_pcie_map_ranges(struct aspeed_pcie *pcie,
					  u64 pci_addr)
{
	u32 pci_addr_lo = pci_addr & GENMASK(31, 0);	/* NVMe: lower 32 bits of PCIe address for NVMe BAR window */
	u32 pci_addr_hi = (pci_addr >> 32) & GENMASK(31, 0);	/* NVMe: upper 32 bits of PCIe address for 64-bit NVMe BAR window */

	pci_addr_lo >>= 16;
	writel(ASPEED_AHB_REMAP_LO_ADDR(pci_addr_lo) |
	       ASPEED_AHB_MASK_LO_ADDR(0xe00),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG0);	/* NVMe: program AST2600 outbound low address/mask so NVMe BAR0/1 are reachable */
	writel(ASPEED_AHB_REMAP_HI_ADDR(pci_addr_hi),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG1);	/* NVMe: program upper 32 bits for 64-bit NVMe BAR accesses */
	writel(ASPEED_AHB_MASK_HI_ADDR(~0),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG2);	/* NVMe: program upper mask to cover all 64-bit NVMe BAR space */
}

static int aspeed_ast2600_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);	/* NVMe: RC instance for AST2600 */
	struct device *dev = pcie->dev;

	pcie->ahbc = syscon_regmap_lookup_by_phandle(dev->of_node,
						     "aspeed,ahbc");	/* NVMe: obtain AHBC syscon for upstream DMA decode used by NVMe */
	if (IS_ERR(pcie->ahbc))
		return dev_err_probe(dev, PTR_ERR(pcie->ahbc),
				     "failed to map ahbc base\n");	/* NVMe: without AHBC NVMe DMA cannot reach host memory */

	aspeed_host_reset(pcie);	/* NVMe: reset H2X block before enabling NVMe enumeration */

	regmap_write(pcie->ahbc, ASPEED_AHBC_KEY, ASPEED_AHBC_UNLOCK_KEY);	/* NVMe: unlock AHBC to program upstream DMA path for NVMe */
	regmap_update_bits(pcie->ahbc, ASPEED_AHBC_ADDR_MAPPING,
			   ASPEED_PCIE_RC_MEMORY_EN, ASPEED_PCIE_RC_MEMORY_EN);	/* NVMe: enable RC memory decode so NVMe DMA writes and MSI reach DRAM */
	regmap_write(pcie->ahbc, ASPEED_AHBC_KEY, ASPEED_AHBC_UNLOCK);	/* NVMe: re-lock AHBC after configuring NVMe DMA path */

	writel(ASPEED_H2X_BRIDGE_EN, pcie->reg + ASPEED_H2X_CTRL);	/* NVMe: enable H2X bridge so config/MEM TLPs can reach NVMe */

	writel(ASPEED_PCIE_RX_DMA_EN | ASPEED_PCIE_RX_LINEAR |
	       ASPEED_PCIE_RX_MSI_SEL | ASPEED_PCIE_RX_MSI_EN |
	       ASPEED_PCIE_WAIT_RX_TLP_CLR | ASPEED_PCIE_RC_RX_ENABLE |
	       ASPEED_PCIE_RC_ENABLE,
	       pcie->reg + ASPEED_H2X_DEV_CTRL);	/* NVMe: enable RC, TLP RX, MSI capture, and upstream DMA from NVMe */

	writel(ASPEED_RC_TLP_TX_TAG_NUM, pcie->reg + ASPEED_H2X_DEV_TX_TAG);	/* NVMe: set number of outstanding config TLP tags for NVMe enumeration */

	pcie->host->ops = &aspeed_ast2600_pcie_ops;	/* NVMe: root-bus config ops used by pci.c before NVMe is discovered */
	pcie->host->child_ops = &aspeed_ast2600_pcie_child_ops;	/* NVMe: downstream config ops used by pci.c to talk to NVMe SSD/switch */

	return 0;	/* NVMe: AST2600 RC ready; pci_host_probe will enumerate NVMe devices */
}

static void aspeed_ast2700_pcie_map_ranges(struct aspeed_pcie *pcie,
					  u64 pci_addr)
{
	writel(ASPEED_REMAP_PCI_ADDR_31_12(pci_addr),
		pcie->reg + ASPEED_H2X_REMAP_PCI_ADDR_LO);	/* NVMe: program AST2700 outbound memory window low bits for NVMe BAR accesses */
	writel(ASPEED_REMAP_PCI_ADDR_63_32(pci_addr),
		pcie->reg + ASPEED_H2X_REMAP_PCI_ADDR_HI);	/* NVMe: program AST2700 outbound memory window high bits for 64-bit NVMe BARs */
}

static int aspeed_ast2700_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);	/* NVMe: RC instance for AST2700 */
	struct device *dev = pcie->dev;

	pcie->cfg = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "aspeed,pciecfg");	/* NVMe: obtain SCU syscon for AST2700 SoC-level NVMe routing */
	if (IS_ERR(pcie->cfg))
		return dev_err_probe(dev, PTR_ERR(pcie->cfg),
				     "failed to map pciecfg base\n");	/* NVMe: without SCU config NVMe link/DMA paths stay disabled */

	regmap_update_bits(pcie->cfg, ASPEED_SCU_60,
			   ASPEED_RC_E2M_PATH_EN | ASPEED_RC_H2XS_PATH_EN |
			   ASPEED_RC_H2XD_PATH_EN | ASPEED_RC_H2XX_PATH_EN |
			   ASPEED_RC_UPSTREAM_MEM_EN,
			   ASPEED_RC_E2M_PATH_EN | ASPEED_RC_H2XS_PATH_EN |
			   ASPEED_RC_H2XD_PATH_EN | ASPEED_RC_H2XX_PATH_EN |
			   ASPEED_RC_UPSTREAM_MEM_EN);	/* NVMe: enable all NVMe upstream/downstream paths (DMA, config, MSI) on AST2700 */
	regmap_write(pcie->cfg, ASPEED_SCU_64,
		     ASPEED_RC0_DECODE_DMA_BASE(0) |
		     ASPEED_RC0_DECODE_DMA_LIMIT(0xff) |
		     ASPEED_RC1_DECODE_DMA_BASE(0) |
		     ASPEED_RC1_DECODE_DMA_LIMIT(0xff));	/* NVMe: configure full DMA decode windows so NVMe DMA is not restricted by base/limit */
	regmap_write(pcie->cfg, ASPEED_SCU_70, ASPEED_DISABLE_EP_FUNC);	/* NVMe: force RC mode; required to enumerate NVMe SSDs */

	aspeed_host_reset(pcie);	/* NVMe: reset H2X block before bringing up NVMe link */

	writel(0, pcie->reg + ASPEED_H2X_CTRL);	/* NVMe: clear bridge control before reconfiguring for NVMe */
	writel(ASPEED_H2X_BRIDGE_EN | ASPEED_H2X_BRIDGE_DIRECT_EN,
	       pcie->reg + ASPEED_H2X_CTRL);	/* NVMe: enable direct H2X bridge for lower-latency NVMe config/MEM accesses */

	/* Prepare for 64-bit BAR pref */
	writel(ASPEED_REMAP_PREF_ADDR_63_32(0x3),
	       pcie->reg + ASPEED_H2X_REMAP_PREF_ADDR);	/* NVMe: set prefetchable memory high address; used when NVMe exposes 64-bit BARs */

	pcie->host->ops = &aspeed_ast2700_pcie_ops;	/* NVMe: AST2700 root-bus config ops for pci.c NVMe enumeration */
	pcie->host->child_ops = &aspeed_ast2700_pcie_child_ops;	/* NVMe: AST2700 downstream config ops for NVMe SSD/switch access */
	pcie->clear_msi_twice = true;	/* NVMe: enable AST2700 MSI status double-clear workaround to avoid lost NVMe interrupts */

	return 0;	/* NVMe: AST2700 RC ready; pci_host_probe will enumerate NVMe devices */
}

static void aspeed_pcie_reset_release(void *d)
{
	struct reset_control *perst = d;	/* NVMe: PERST reset control obtained for an NVMe slot */

	if (!perst)
		return;	/* NVMe: nothing to release if reset was not acquired */

	reset_control_put(perst);	/* NVMe: release PERST reference when the RC is removed (NVMe already reset/off) */
}

static int aspeed_pcie_parse_port(struct aspeed_pcie *pcie,
				  struct device_node *node,
				  int slot)
{
	struct aspeed_pcie_port *port;	/* NVMe: per-slot port descriptor for a possible NVMe SSD */
	struct device *dev = pcie->dev;
	int ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;	/* NVMe: cannot allocate port metadata; NVMe on this slot unavailable */

	port->clk = devm_get_clk_from_child(dev, node, NULL);
	if (IS_ERR(port->clk))
		return dev_err_probe(dev, PTR_ERR(port->clk),
				     "failed to get pcie%d clock\n", slot);	/* NVMe: clock missing means no NVMe on this slot */

	port->phy = devm_of_phy_get(dev, node, NULL);
	if (IS_ERR(port->phy))
		return dev_err_probe(dev, PTR_ERR(port->phy),
				     "failed to get phy pcie%d\n", slot);	/* NVMe: PHY missing means no electrical link to NVMe SSD */

	port->perst = of_reset_control_get_exclusive(node, "perst");
	if (IS_ERR(port->perst))
		return dev_err_probe(dev, PTR_ERR(port->perst),
				     "failed to get pcie%d reset control\n",
				     slot);	/* NVMe: PERST missing means NVMe reset/hotplug cannot be controlled */
	ret = devm_add_action_or_reset(dev, aspeed_pcie_reset_release,
				       port->perst);
	if (ret)
		return ret;	/* NVMe: devm action registration failed; PERST would leak on remove */
	reset_control_assert(port->perst);	/* NVMe: keep NVMe SSD in reset while clocks/PHY are configured */

	port->slot = slot;	/* NVMe: record DT slot number for NVMe port logs */
	port->pcie = pcie;	/* NVMe: link port back to the RC that enumerates NVMe */

	INIT_LIST_HEAD(&port->list);	/* NVMe: initialize port list node before adding to RC port list */
	list_add_tail(&port->list, &pcie->ports);	/* NVMe: add port to list of NVMe-capable slots */

	ret = aspeed_pcie_port_init(port);
	if (ret)
		return ret;	/* NVMe: port init failed; NVMe on this slot will not be enumerated */

	return 0;	/* NVMe: port parsed and initialized, ready for NVMe link training */
}

static int aspeed_pcie_parse_dt(struct aspeed_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *node = dev->of_node;
	int ret;

	for_each_available_child_of_node_scoped(node, child) {
		int slot;
		const char *type;

		ret = of_property_read_string(child, "device_type", &type);
		if (ret || strcmp(type, "pci"))
			continue;	/* NVMe: skip non-PCI child nodes; only PCIe ports matter for NVMe */

		ret = of_pci_get_devfn(child);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to parse devfn\n");	/* NVMe: invalid devfn means this DT port cannot host NVMe */

		slot = PCI_SLOT(ret);	/* NVMe: physical slot number for this NVMe port */

		ret = aspeed_pcie_parse_port(pcie, child, slot);
		if (ret)
			return ret;	/* NVMe: stop if a port cannot be initialized; NVMe probe will fail */
	}

	if (list_empty(&pcie->ports))
		return dev_err_probe(dev, -ENODEV,
				     "No PCIe port found in DT\n");	/* NVMe: no usable PCIe port means no NVMe can be enumerated */

	return 0;	/* NVMe: at least one PCIe port parsed and ready for NVMe enumeration */
}

static int aspeed_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* NVMe: device structure used for DMA/IOMMU and resource management */
	struct pci_host_bridge *host;	/* NVMe: host bridge that Linux PCI core will use to enumerate NVMe SSDs */
	struct aspeed_pcie *pcie;	/* NVMe: driver private data for this Root Complex */
	struct resource_entry *entry;
	const struct aspeed_pcie_rc_platform *md;
	int irq, ret;

	md = of_device_get_match_data(dev);
	if (!md)
		return -ENODEV;	/* NVMe: no matching SoC data means this driver cannot support NVMe on this board */

	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!host)
		return -ENOMEM;	/* NVMe: cannot allocate host bridge; NVMe enumeration impossible */

	pcie = pci_host_bridge_priv(host);	/* NVMe: carve driver private data out of host bridge allocation */
	pcie->dev = dev;	/* NVMe: store device pointer for DMA, IOMMU, and logging */
	pcie->tx_tag = 0;	/* NVMe: start config TLP tag counter at zero */
	platform_set_drvdata(pdev, pcie);	/* NVMe: allow platform callbacks (setup) to retrieve pcie */

	pcie->platform = md;	/* NVMe: attach AST2600/AST2700 specific ops/register offsets */
	pcie->host = host;	/* NVMe: link driver data to host bridge for pci_host_probe */
	INIT_LIST_HEAD(&pcie->ports);	/* NVMe: initialize list of NVMe-capable ports */

	/* Get root bus num for cfg command to decide tlp type 0 or type 1 */
	entry = resource_list_first_type(&host->windows, IORESOURCE_BUS);	/* NVMe: fetch bus-range resource to know root bus number for NVMe CFG0/CFG1 selection */
	if (entry)
		pcie->root_bus_nr = entry->res->start;	/* NVMe: root bus number used to decide type-0 vs type-1 config for NVMe */

	pcie->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pcie->reg))
		return PTR_ERR(pcie->reg);	/* NVMe: cannot map H2X registers; NVMe config/MEM/MSI access impossible */

	pcie->h2xrst = devm_reset_control_get_exclusive(dev, "h2x");
	if (IS_ERR(pcie->h2xrst))
		return dev_err_probe(dev, PTR_ERR(pcie->h2xrst),
				     "failed to get h2x reset\n");	/* NVMe: H2X reset missing; cannot reset RC before NVMe enumeration */

	ret = devm_mutex_init(dev, &pcie->lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init mutex\n");	/* NVMe: mutex init failed; MSI vector allocation would be unsafe for NVMe */

	ret = pcie->platform->setup(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to setup PCIe RC\n");	/* NVMe: SoC-specific RC setup failed; NVMe cannot be enumerated */

	aspeed_pcie_map_ranges(pcie);	/* NVMe: program outbound memory window so NVMe BAR0/1 map to CPU address space */

	ret = aspeed_pcie_parse_dt(pcie);
	if (ret)
		return ret;	/* NVMe: no usable PCIe port parsed; NVMe probe aborts */

	host->sysdata = pcie;	/* NVMe: give pci_bus sysdata pointer used by config read/write callbacks for NVMe */

	ret = aspeed_pcie_init_irq_domain(pcie);
	if (ret)
		return ret;	/* NVMe: IRQ domain init failed; NVMe interrupts unavailable */

	ret = devm_add_action_or_reset(dev, aspeed_pcie_irq_domain_free, pcie);
	if (ret)
		return ret;	/* NVMe: cannot register IRQ domain teardown; cleanup on remove would be incomplete */

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;	/* NVMe: no host IRQ; cannot receive NVMe MSI/INTx interrupts */

	ret = devm_request_irq(dev, irq, aspeed_pcie_intr_handler, IRQF_SHARED,
			       dev_name(dev), pcie);
	if (ret)
		return ret;	/* NVMe: host IRQ request failed; NVMe interrupts cannot be delivered */

	return pci_host_probe(host);	/* NVMe: enumerate PCI bus; if an NVMe SSD is present, nvme_probe in drivers/nvme/host/pci.c will be called */
}

static const struct aspeed_pcie_rc_platform pcie_rc_ast2600 = {
	.setup = aspeed_ast2600_setup,	/* NVMe: AST2600-specific RC setup before NVMe enumeration */
	.pcie_map_ranges = aspeed_ast2600_pcie_map_ranges,	/* NVMe: AST2600 outbound memory mapping for NVMe BARs */
	.reg_intx_en = 0xc4,	/* NVMe: INTx enable register offset for AST2600 legacy NVMe interrupts */
	.reg_intx_sts = 0xc8,	/* NVMe: INTx status register offset for AST2600 legacy NVMe interrupts */
	.reg_msi_en = 0xe0,	/* NVMe: MSI enable register offset for AST2600 NVMe MSI vectors */
	.reg_msi_sts = 0xe8,	/* NVMe: MSI status register offset for AST2600 NVMe MSI vectors */
	.msi_address = 0x1e77005c,	/* NVMe: fixed MSI target address for AST2600; NVMe SSD writes here to raise MSI */
};

static const struct aspeed_pcie_rc_platform pcie_rc_ast2700 = {
	.setup = aspeed_ast2700_setup,	/* NVMe: AST2700-specific RC setup before NVMe enumeration */
	.pcie_map_ranges = aspeed_ast2700_pcie_map_ranges,	/* NVMe: AST2700 outbound memory mapping for NVMe BARs */
	.reg_intx_en = 0x40,	/* NVMe: INTx enable register offset for AST2700 legacy NVMe interrupts */
	.reg_intx_sts = 0x48,	/* NVMe: INTx status register offset for AST2700 legacy NVMe interrupts */
	.reg_msi_en = 0x50,	/* NVMe: MSI enable register offset for AST2700 NVMe MSI vectors */
	.reg_msi_sts = 0x58,	/* NVMe: MSI status register offset for AST2700 NVMe MSI vectors */
	.msi_address = 0x000000f0,	/* NVMe: fixed MSI target address for AST2700; NVMe SSD writes here to raise MSI */
};

static const struct of_device_id aspeed_pcie_of_match[] = {
	{ .compatible = "aspeed,ast2600-pcie", .data = &pcie_rc_ast2600 },	/* NVMe: match AST2600 PCIe RC; enables NVMe enumeration on AST2600 */
	{ .compatible = "aspeed,ast2700-pcie", .data = &pcie_rc_ast2700 },	/* NVMe: match AST2700 PCIe RC; enables NVMe enumeration on AST2700 */
	{}
};

static struct platform_driver aspeed_pcie_driver = {
	.driver = {
		.name = "aspeed-pcie",	/* NVMe: platform driver name; bound when DT compatible matches */
		.of_match_table = aspeed_pcie_of_match,	/* NVMe: DT match table selecting AST2600/AST2700 NVMe RC support */
		.suppress_bind_attrs = true,	/* NVMe: disable manual bind/unbind to avoid breaking NVMe link while in use */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,	/* NVMe: defer probe so NVMe SSD can finish link training before enumeration */
	},
	.probe = aspeed_pcie_probe,	/* NVMe: entry point that sets up RC and triggers NVMe enumeration */
};

builtin_platform_driver(aspeed_pcie_driver);	/* NVMe: register driver at init time so NVMe SSDs can be discovered early */

MODULE_AUTHOR("Jacky Chou <jacky_chou@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED PCIe Root Complex");	/* NVMe: description of the RC driver that underpins NVMe SSD support */
MODULE_LICENSE("GPL");
