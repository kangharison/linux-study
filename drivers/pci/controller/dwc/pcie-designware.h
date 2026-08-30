/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synopsys DesignWare PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 */

#ifndef _PCIE_DESIGNWARE_H
#define _PCIE_DESIGNWARE_H

#include <linux/bitfield.h>	/* PCI/NVMe: bitfield helpers used for PCIe capability parsing relevant to NVMe BARs/MSI-X */
#include <linux/bitops.h>	/* PCI/NVMe: bitmap ops for tracking MSI vectors consumed by NVMe queues */
#include <linux/clk.h>		/* PCI/NVMe: PCIe ref/aux clocks must be stable before NVMe link training */
#include <linux/dma-mapping.h>	/* PCI/NVMe: DMA map/unmap for NVMe SQ/CQ PRPs/SGLs */
#include <linux/dma/edma.h>	/* PCI/NVMe: optional eDMA path for peer-to-peer NVMe traffic */
#include <linux/gpio/consumer.h>/* PCI/NVMe: PERST# GPIO used to reset the NVMe SSD during probe */
#include <linux/irq.h>		/* PCI/NVMe: IRQ domain wiring for NVMe MSI-X/MSI vectors */
#include <linux/msi.h>		/* PCI/NVMe: MSI core definitions; NVMe drives typically use MSI-X */
#include <linux/pci.h>		/* PCI/NVMe: core PCI defs; struct pci_dev of the NVMe controller lives here */
#include <linux/pci-ecam.h>	/* PCI/NVMe: ECAM config access used to enumerate NVMe device */
#include <linux/reset.h>	/* PCI/NVMe: bulk resets for controller init before NVMe link up */

#include <linux/pci-epc.h>	/* PCI/NVMe: endpoint controller glue if SoC exposes NVMe-like EP */
#include <linux/pci-epf.h>	/* PCI/NVMe: endpoint function helpers */

#include "../../pci.h"		/* PCI/NVMe: internal PCI core headers shared with NVMe host driver */

/* DWC PCIe IP-core versions (native support since v4.70a) */
#define DW_PCIE_VER_365A		0x3336352a	/* PCI/NVMe: v3.65a, older DWC core that may need quirks for NVMe MSI */
#define DW_PCIE_VER_460A		0x3436302a	/* PCI/NVMe: v4.60a, still indirect iATU viewport for NVMe BAR mapping */
#define DW_PCIE_VER_470A		0x3437302a	/* PCI/NVMe: v4.70a, first native host support; eDMA still in port logic */
#define DW_PCIE_VER_480A		0x3438302a	/* PCI/NVMe: v4.80a, unrolled iATU; simplifies NVMe outbound mapping */
#define DW_PCIE_VER_490A		0x3439302a	/* PCI/NVMe: v4.90a */
#define DW_PCIE_VER_500A		0x3530302a	/* PCI/NVMe: v5.00a */
#define DW_PCIE_VER_520A		0x3532302a	/* PCI/NVMe: v5.20a */
#define DW_PCIE_VER_540A		0x3534302a	/* PCI/NVMe: v5.40a */
#define DW_PCIE_VER_562A		0x3536322a	/* PCI/NVMe: v5.62a */

#define __dw_pcie_ver_cmp(_pci, _ver, _op) \
	((_pci)->version _op DW_PCIE_VER_ ## _ver)	/* PCI/NVMe: expand version macro and compare; used to pick NVMe-safe workarounds */

#define dw_pcie_ver_is(_pci, _ver) __dw_pcie_ver_cmp(_pci, _ver, ==)	/* PCI/NVMe: exact IP version match, e.g., to enable NVMe MSI-X doorbell quirk */

#define dw_pcie_ver_is_ge(_pci, _ver) __dw_pcie_ver_cmp(_pci, _ver, >=)	/* PCI/NVMe: version >= _ver, e.g., unrolled iATU needed for fast NVMemap */

#define dw_pcie_ver_type_is(_pci, _ver, _type) \
	(__dw_pcie_ver_cmp(_pci, _ver, ==) && \
	 __dw_pcie_ver_cmp(_pci, TYPE_ ## _type, ==))	/* PCI/NVMe: match both IP version and controller type (RC/EP) for NVMe host path */

#define dw_pcie_ver_type_is_ge(_pci, _ver, _type) \
	(__dw_pcie_ver_cmp(_pci, _ver, ==) && \
	 __dw_pcie_ver_cmp(_pci, TYPE_ ## _type, >=))	/* PCI/NVMe: version == _ver and type >= _type; guards NVMe feature enablement */

/* DWC PCIe controller capabilities */
#define DW_PCIE_CAP_REQ_RES		0	/* PCI/NVMe: requester resource reservation; affects NVMe upstream posted writes */
#define DW_PCIE_CAP_IATU_UNROLL		1	/* PCI/NVMe: unrolled iATU windows speed up NVMe BAR/MMIO setup */
#define DW_PCIE_CAP_CDM_CHECK		2	/* PCI/NVMe: CDM register check; used during NVMe enumeration sanity checks */

#define dw_pcie_cap_is(_pci, _cap) \
	test_bit(DW_PCIE_CAP_ ## _cap, &(_pci)->caps)	/* PCI/NVMe: query a capability bit; e.g., skip viewport dance if IATU_UNROLL set for NVMe */

#define dw_pcie_cap_set(_pci, _cap) \
	set_bit(DW_PCIE_CAP_ ## _cap, &(_pci)->caps)	/* PCI/NVMe: record detected capability; informs NVMe BAR/MSI init path */

/* Parameters for the waiting for iATU enabled routine */
#define LINK_WAIT_MAX_IATU_RETRIES	5	/* PCI/NVMe: retry count when programming iATU for NVMe config/MMIO accesses */
#define LINK_WAIT_IATU			9	/* PCI/NVMe: udelay per iATU poll; NVMe boot latency accumulates here */

/* Synopsys-specific PCIe configuration registers */
#define PCIE_PORT_FORCE			0x708	/* PCI/NVMe: port force register; may be programmed before NVMe link comes up */
#define PORT_FORCE_DO_DESKEW_FOR_SRIS	BIT(23)	/* PCI/NVMe: SRIS deskew; affects NVMe Gen3/Gen4 signal integrity */

#define PCIE_PORT_AFR			0x70C	/* PCI/NVMe: Ack/Nak FTS and ASPM latency settings seen by NVMe device */
#define PORT_AFR_N_FTS_MASK		GENMASK(15, 8)	/* PCI/NVMe: FTS count mask; impacts NVMe L0s exit latency */
#define PORT_AFR_N_FTS(n)		FIELD_PREP(PORT_AFR_N_FTS_MASK, n)	/* PCI/NVMe: set FTS for NVMe L0s recovery */
#define PORT_AFR_CC_N_FTS_MASK		GENMASK(23, 16)
#define PORT_AFR_CC_N_FTS(n)		FIELD_PREP(PORT_AFR_CC_N_FTS_MASK, n)
#define PORT_AFR_ENTER_ASPM		BIT(30)	/* PCI/NVMe: allow ASPM L1 entry; NVMe ASPM policy consults this */
#define PORT_AFR_L0S_ENTRANCE_LAT_SHIFT	24	/* PCI/NVMe: L0s latency encoded here affects NVMe active-state PM choice */
#define PORT_AFR_L0S_ENTRANCE_LAT_MASK	GENMASK(26, 24)
#define PORT_AFR_L1_ENTRANCE_LAT_SHIFT	27	/* PCI/NVMe: L1 latency; NVMe driver reads ASPM capability derived from these */
#define PORT_AFR_L1_ENTRANCE_LAT_MASK	GENMASK(29, 27)

#define PCIE_PORT_LINK_CONTROL		0x710	/* PCI/NVMe: link control; sets lane count/speed for NVMe link training */
#define PORT_LINK_DLL_LINK_EN		BIT(5)	/* PCI/NVMe: enable data link layer; required before NVMe TLP exchange */
#define PORT_LINK_FAST_LINK_MODE	BIT(7)	/* PCI/NVMe: fast link mode; speeds up NVMe bring-up but relaxes timing */
#define PORT_LINK_MODE_MASK		GENMASK(21, 16)	/* PCI/NVMe: lane mode mask; decides NVMe x1/x4/x8 link width */
#define PORT_LINK_MODE(n)		FIELD_PREP(PORT_LINK_MODE_MASK, n)
#define PORT_LINK_MODE_1_LANES		PORT_LINK_MODE(0x1)	/* PCI/NVMe: x1 link, common for low-end NVMe */
#define PORT_LINK_MODE_2_LANES		PORT_LINK_MODE(0x3)	/* PCI/NVMe: x2 link */
#define PORT_LINK_MODE_4_LANES		PORT_LINK_MODE(0x7)	/* PCI/NVMe: x4 link, typical consumer NVMe */
#define PORT_LINK_MODE_8_LANES		PORT_LINK_MODE(0xf)	/* PCI/NVMe: x8 link, enterprise NVMe SSDs */
#define PORT_LINK_MODE_16_LANES		PORT_LINK_MODE(0x1f)	/* PCI/NVMe: x16 link, high-performance NVMe */

#define PCIE_PORT_LANE_SKEW		0x714	/* PCI/NVMe: lane skew insertion control; rarely touched for NVMe */
#define PORT_LANE_SKEW_INSERT_MASK	GENMASK(23, 0)

#define PCIE_PORT_DEBUG0		0x728	/* PCI/NVMe: LTSSM state debug; NVMe probe waits for L0 via this */
#define PORT_LOGIC_LTSSM_STATE_MASK	0x3f	/* PCI/NVMe: bits 0:5 hold LTSSM state relevant to NVMe link status */
#define PORT_LOGIC_LTSSM_STATE_L0	0x11	/* PCI/NVMe: L0 state; once reached NVMe config cycles can proceed */
#define PCIE_PORT_DEBUG1		0x72C	/* PCI/NVMe: link-up and training status for NVMe presence detection */
#define PCIE_PORT_DEBUG1_LINK_UP		BIT(4)	/* PCI/NVMe: LINK_UP bit; NVMe enumeration starts after this is set */
#define PCIE_PORT_DEBUG1_LINK_IN_TRAINING	BIT(29)	/* PCI/NVMe: still training; NVMe driver must wait before config read */

#define PCIE_LINK_WIDTH_SPEED_CONTROL	0x80C	/* PCI/NVMe: retrain link width/speed; used when NVMe negotiates lower speed */
#define PORT_LOGIC_N_FTS_MASK		GENMASK(7, 0)	/* PCI/NVMe: number of FTS; tuned for NVMe Gen3/Gen4 stability */
#define PORT_LOGIC_SPEED_CHANGE		BIT(17)	/* PCI/NVMe: initiate speed change; NVMe link speed switch happens here */
#define PORT_LOGIC_LINK_WIDTH_MASK	GENMASK(12, 8)	/* PCI/NVMe: current link width mask; logged when NVMe negotiates width */
#define PORT_LOGIC_LINK_WIDTH(n)	FIELD_PREP(PORT_LOGIC_LINK_WIDTH_MASK, n)
#define PORT_LOGIC_LINK_WIDTH_1_LANES	PORT_LOGIC_LINK_WIDTH(0x1)	/* PCI/NVMe: x1 width */
#define PORT_LOGIC_LINK_WIDTH_2_LANES	PORT_LOGIC_LINK_WIDTH(0x2)	/* PCI/NVMe: x2 width */
#define PORT_LOGIC_LINK_WIDTH_4_LANES	PORT_LOGIC_LINK_WIDTH(0x4)	/* PCI/NVMe: x4 width */
#define PORT_LOGIC_LINK_WIDTH_8_LANES	PORT_LOGIC_LINK_WIDTH(0x8)	/* PCI/NVMe: x8 width */

#define PCIE_MSI_ADDR_LO		0x820	/* PCI/NVMe: low 32 bits of MSI target address; NVMe MSI messages land here */
#define PCIE_MSI_ADDR_HI		0x824	/* PCI/NVMe: high 32 bits of MSI target address for 64-bit NVMe systems */
#define PCIE_MSI_INTR0_ENABLE		0x828	/* PCI/NVMe: MSI group 0 enable; gates NVMe queue interrupts */
#define PCIE_MSI_INTR0_MASK		0x82C	/* PCI/NVMe: MSI group 0 mask; masks NVMe MSI vectors */
#define PCIE_MSI_INTR0_STATUS		0x830	/* PCI/NVMe: MSI group 0 status; shows pending NVMe MSI interrupts */

#define GEN3_RELATED_OFF			0x890	/* PCI/NVMe: Gen3 equalization; affects NVMe Gen3 link quality */
#define GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL	BIT(0)
#define GEN3_RELATED_OFF_EQ_PHASE_2_3		BIT(9)	/* PCI/NVMe: enable Gen3 EQ phase 2/3 for stable NVMe Gen3 link */
#define GEN3_RELATED_OFF_RXEQ_RGRDLESS_RXTS	BIT(13)
#define GEN3_RELATED_OFF_GEN3_EQ_DISABLE	BIT(16)	/* PCI/NVMe: disable Gen3 EQ; sometimes needed for quirky NVMe SSDs */
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL_SHIFT	24
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK	GENMASK(25, 24)

#define GEN3_EQ_CONTROL_OFF			0x8A8	/* PCI/NVMe: Gen3 equalization feedback; NVMe high-speed signal tuning */
#define GEN3_EQ_CONTROL_OFF_FB_MODE		GENMASK(3, 0)
#define GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE	BIT(4)
#define GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC	GENMASK(23, 8)
#define GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL	BIT(24)

#define GEN3_EQ_FB_MODE_DIR_CHANGE_OFF		0x8AC	/* PCI/NVMe: Gen3 EQ feedback direction change */
#define GEN3_EQ_FMDC_T_MIN_PHASE23		GENMASK(4, 0)
#define GEN3_EQ_FMDC_N_EVALS			GENMASK(9, 5)
#define GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA	GENMASK(13, 10)
#define GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA	GENMASK(17, 14)

#define COHERENCY_CONTROL_1_OFF			0x8E0	/* PCI/NVMe: coherency boundary; ensures NVMe DMA sees consistent cache state */
#define CFG_MEMTYPE_BOUNDARY_LOW_ADDR_MASK	GENMASK(31, 2)
#define CFG_MEMTYPE_VALUE			BIT(0)

#define COHERENCY_CONTROL_2_OFF			0x8E4	/* PCI/NVMe: additional coherency settings for NVMe DMA */
#define COHERENCY_CONTROL_3_OFF			0x8E8	/* PCI/NVMe: additional coherency settings for NVMe DMA */

#define PCIE_PORT_MULTI_LANE_CTRL	0x8C0	/* PCI/NVMe: multi-lane control; up-config support for NVMe width changes */
#define PORT_MLTI_UPCFG_SUPPORT		BIT(7)	/* PCI/NVMe: permit NVMe link width up-config after initial training */

#define PCIE_VERSION_NUMBER		0x8F8	/* PCI/NVMe: DWC IP version number register; read before NVMe init */
#define PCIE_VERSION_TYPE		0x8FC	/* PCI/NVMe: DWC IP type register; distinguishes RC/EP for NVMe host */

/*
 * iATU inbound and outbound windows CSRs. Before the IP-core v4.80a each
 * iATU region CSRs had been indirectly accessible by means of the dedicated
 * viewport selector. The iATU/eDMA CSRs space was re-designed in DWC PCIe
 * v4.80a in a way so the viewport was unrolled into the directly accessible
 * iATU/eDMA CSRs space.
 */
#define PCIE_ATU_VIEWPORT		0x900	/* PCI/NVMe: indirect iATU viewport selector on pre-v4.80a; used to map NVMe config/MMIO */
#define PCIE_ATU_REGION_DIR_IB		BIT(31)	/* PCI/NVMe: inbound direction; NVMe P2P or host RAM mapping */
#define PCIE_ATU_REGION_DIR_OB		0	/* PCI/NVMe: outbound direction; CPU to NVMe config/MMIO/IO space */
#define PCIE_ATU_VIEWPORT_BASE		0x904	/* PCI/NVMe: base of indirect iATU register set */
#define PCIE_ATU_UNROLL_BASE(dir, index) \
	(((index) << 9) | ((dir == PCIE_ATU_REGION_DIR_IB) ? BIT(8) : 0))	/* PCI/NVMe: compute unrolled iATU offset for NVMe region index/direction */
#define PCIE_ATU_VIEWPORT_SIZE		0x2C	/* PCI/NVMe: size of one indirect iATU region context */
#define PCIE_ATU_REGION_CTRL1		0x000	/* PCI/NVMe: iATU control 1: TLP type for NVMe config/mem/IO requests */
#define PCIE_ATU_INCREASE_REGION_SIZE	BIT(13)	/* PCI/NVMe: allow 4 GB+ outbound region for large NVMe BAR mapping */
#define PCIE_ATU_TYPE_MEM		0x0	/* PCI/NVMe: memory TLP type; used for NVMe BAR memory mapping */
#define PCIE_ATU_TYPE_IO		0x2	/* PCI/NVMe: IO TLP type; NVMe rarely uses IO BARs */
#define PCIE_ATU_TYPE_CFG0		0x4	/* PCI/NVMe: type 0 config TLP; for NVMe device config cycles on bus 0 */
#define PCIE_ATU_TYPE_CFG1		0x5	/* PCI/NVMe: type 1 config TLP; for NVMe device config cycles behind bridges */
#define PCIE_ATU_TYPE_MSG		0x10	/* PCI/NVMe: message TLP type; used for NVMe PME/AER messages */
#define PCIE_ATU_TD			BIT(8)	/* PCI/NVMe: traffic digest (TLP digest) enable; affects NVMe ECRC */
#define PCIE_ATU_FUNC_NUM(pf)		   ((pf) << 20)	/* PCI/NVMe: target physical function for SR-IOV NVMe VF mapping */
#define PCIE_ATU_REGION_CTRL2		0x004	/* PCI/NVMe: iATU control 2: enable, BAR match, CFG shift for NVMe access */
#define PCIE_ATU_ENABLE			BIT(31)	/* PCI/NVMe: enable this iATU region so NVMe config/MMIO passes through */
#define PCIE_ATU_BAR_MODE_ENABLE	BIT(30)	/* PCI/NVMe: inbound BAR match mode; used when NVMe EP exposes BARs */
#define PCIE_ATU_CFG_SHIFT_MODE_ENABLE	BIT(28)	/* PCI/NVMe: shift mode for ECAM config access during NVMe enumeration */
#define PCIE_ATU_INHIBIT_PAYLOAD	BIT(22)	/* PCI/NVMe: inhibit payload; rarely set for NVMe */
#define PCIE_ATU_FUNC_NUM_MATCH_EN	BIT(19)	/* PCI/NVMe: match target function number for NVMe SR-IOV VF iATU */
#define PCIE_ATU_LOWER_BASE		0x008	/* PCI/NVMe: outbound region low CPU base for NVMe MMIO/config window */
#define PCIE_ATU_UPPER_BASE		0x00C	/* PCI/NVMe: outbound region high CPU base for 64-bit NVMe systems */
#define PCIE_ATU_LIMIT			0x010	/* PCI/NVMe: outbound region limit; bounds NVMe BAR aperture */
#define PCIE_ATU_LOWER_TARGET		0x014	/* PCI/NVMe: low PCIe target bus address for NVMe config/MMIO TLPs */
#define PCIE_ATU_BUS(x)			FIELD_PREP(GENMASK(31, 24), x)	/* PCI/NVMe: target bus number for NVMe device enumeration */
#define PCIE_ATU_DEV(x)			FIELD_PREP(GENMASK(23, 19), x)	/* PCI/NVMe: target device number for NVMe slot */
#define PCIE_ATU_FUNC(x)		FIELD_PREP(GENMASK(18, 16), x)	/* PCI/NVMe: target function number for NVMe PF/VF */
#define PCIE_ATU_UPPER_TARGET		0x018	/* PCI/NVMe: high PCIe target address for 64-bit NVMe apertures */
#define PCIE_ATU_UPPER_LIMIT		0x020	/* PCI/NVMe: high limit for large NVMe BARs (>4 GB) */

#define PCIE_MISC_CONTROL_1_OFF		0x8BC	/* PCI/NVMe: misc control; DBI read-only write enable used during NVMe init */
#define PCIE_DBI_RO_WR_EN		BIT(0)	/* PCI/NVMe: allow writes to read-only config fields; used to init NVMe link caps */

#define PCIE_MSIX_DOORBELL		0x948	/* PCI/NVMe: MSI-X doorbell register; NVMe MSI-X interrupts injected from EP side */
#define PCIE_MSIX_DOORBELL_PF_SHIFT	24	/* PCI/NVMe: PF number shift in MSI-X doorbell for NVMe SR-IOV */

/*
 * eDMA CSRs. DW PCIe IP-core v4.70a and older had the eDMA registers accessible
 * over the Port Logic registers space. Afterwards the unrolled mapping was
 * introduced so eDMA and iATU could be accessed via a dedicated registers
 * space.
 */
#define PCIE_DMA_VIEWPORT_BASE		0x970	/* PCI/NVMe: legacy eDMA viewport base; optional NVMe offload path */
#define PCIE_DMA_UNROLL_BASE		0x80000	/* PCI/NVMe: unrolled eDMA register space base for NVMe peer DMA */
#define PCIE_DMA_CTRL			0x008	/* PCI/NVMe: eDMA control; number of read/write channels */
#define PCIE_DMA_NUM_WR_CHAN		GENMASK(3, 0)	/* PCI/NVMe: number of eDMA write channels; can move NVMe data */
#define PCIE_DMA_NUM_RD_CHAN		GENMASK(19, 16)	/* PCI/NVMe: number of eDMA read channels; can move NVMe data */

#define PCIE_PL_CHK_REG_CONTROL_STATUS			0xB20	/* PCI/NVMe: CDM checker control; detects config corruption affecting NVMe */
#define PCIE_PL_CHK_REG_CHK_REG_START			BIT(0)	/* PCI/NVMe: start CDM register check */
#define PCIE_PL_CHK_REG_CHK_REG_CONTINUOUS		BIT(1)	/* PCI/NVMe: continuous CDM check while NVMe operates */
#define PCIE_PL_CHK_REG_CHK_REG_COMPARISON_ERROR	BIT(16)	/* PCI/NVMe: comparison error may indicate NVMe config corruption */
#define PCIE_PL_CHK_REG_CHK_REG_LOGIC_ERROR		BIT(17)	/* PCI/NVMe: logic error in CDM check */
#define PCIE_PL_CHK_REG_CHK_REG_COMPLETE		BIT(18)	/* PCI/NVMe: CDM check complete */

#define PCIE_PL_CHK_REG_ERR_ADDR			0xB28	/* PCI/NVMe: failing CDM register address; useful for NVMe AER debug */

/*
 * 16.0 GT/s (Gen 4) lane margining register definitions
 */
#define GEN4_LANE_MARGINING_1_OFF		0xB80	/* PCI/NVMe: Gen4 lane margining; NVMe Gen4 debug/diagnostics */
#define MARGINING_MAX_VOLTAGE_OFFSET		GENMASK(29, 24)
#define MARGINING_NUM_VOLTAGE_STEPS		GENMASK(22, 16)
#define MARGINING_MAX_TIMING_OFFSET		GENMASK(13, 8)
#define MARGINING_NUM_TIMING_STEPS		GENMASK(5, 0)

#define GEN4_LANE_MARGINING_2_OFF		0xB84	/* PCI/NVMe: Gen4 margining capabilities */
#define MARGINING_IND_ERROR_SAMPLER		BIT(28)
#define MARGINING_SAMPLE_REPORTING_METHOD	BIT(27)
#define MARGINING_IND_LEFT_RIGHT_TIMING		BIT(26)
#define MARGINING_IND_UP_DOWN_VOLTAGE		BIT(25)
#define MARGINING_VOLTAGE_SUPPORTED		BIT(24)
#define MARGINING_MAXLANES			GENMASK(20, 16)	/* PCI/NVMe: max lanes supported for NVMe width margining */
#define MARGINING_SAMPLE_RATE_TIMING		GENMASK(13, 8)
#define MARGINING_SAMPLE_RATE_VOLTAGE		GENMASK(5, 0)
/*
 * iATU Unroll-specific register definitions
 * From 4.80 core version the address translation will be made by unroll
 */
#define PCIE_ATU_UNR_REGION_CTRL1	0x00	/* PCI/NVMe: unrolled iATU ctrl1; sets TLP type for NVMe outbound/inbound */
#define PCIE_ATU_UNR_REGION_CTRL2	0x04	/* PCI/NVMe: unrolled iATU ctrl2; enable/match bits for NVMe region */
#define PCIE_ATU_UNR_LOWER_BASE		0x08	/* PCI/NVMe: unrolled outbound CPU low base for NVMe MMIO/config */
#define PCIE_ATU_UNR_UPPER_BASE		0x0C	/* PCI/NVMe: unrolled outbound CPU high base for NVMe MMIO/config */
#define PCIE_ATU_UNR_LOWER_LIMIT	0x10	/* PCI/NVMe: unrolled outbound limit low for NVMe aperture */
#define PCIE_ATU_UNR_LOWER_TARGET	0x14	/* PCI/NVMe: unrolled PCIe target low for NVMe TLP routing */
#define PCIE_ATU_UNR_UPPER_TARGET	0x18	/* PCI/NVMe: unrolled PCIe target high for NVMe 64-bit routing */
#define PCIE_ATU_UNR_UPPER_LIMIT	0x20	/* PCI/NVMe: unrolled outbound limit high for large NVMe BARs */

/*
 * RAS-DES register definitions
 */
#define PCIE_RAS_DES_EVENT_COUNTER_CONTROL	0x8	/* PCI/NVMe: RAS event counter control; used for NVMe link quality stats */
#define EVENT_COUNTER_ALL_CLEAR		0x3	/* PCI/NVMe: clear all RAS counters before collecting NVMe stats */
#define EVENT_COUNTER_ENABLE_ALL	0x7	/* PCI/NVMe: enable all RAS event counters */
#define EVENT_COUNTER_ENABLE_SHIFT	2
#define EVENT_COUNTER_EVENT_SEL_MASK	GENMASK(7, 0)
#define EVENT_COUNTER_EVENT_SEL_SHIFT	16
#define EVENT_COUNTER_EVENT_Tx_L0S	0x2	/* PCI/NVMe: count TX L0s entries; relevant to NVMe ASPM profiling */
#define EVENT_COUNTER_EVENT_Rx_L0S	0x3	/* PCI/NVMe: count RX L0s entries; relevant to NVMe ASPM profiling */
#define EVENT_COUNTER_EVENT_L1		0x5	/* PCI/NVMe: count L1 entries; impacts NVMe resume latency */
#define EVENT_COUNTER_EVENT_L1_1	0x7	/* PCI/NVMe: count L1.1 entries; NVMe L1SS profiling */
#define EVENT_COUNTER_EVENT_L1_2	0x8	/* PCI/NVMe: count L1.2 entries; NVMe L1SS profiling */
#define EVENT_COUNTER_GROUP_SEL_SHIFT	24
#define EVENT_COUNTER_GROUP_5		0x5

#define PCIE_RAS_DES_EVENT_COUNTER_DATA		0xc	/* PCI/NVMe: RAS counter data; exported via debugfs for NVMe diagnostics */

/* PTM register definitions */
#define PTM_RES_REQ_CTRL		0x8	/* PCI/NVMe: PTM resource request control; NVMe PTM timestamping */
#define PTM_RES_CCONTEXT_VALID		BIT(0)
#define PTM_REQ_AUTO_UPDATE_ENABLED	BIT(0)	/* PCI/NVMe: auto-update PTM context; useful for NVMe time sync */
#define PTM_REQ_START_UPDATE		BIT(1)	/* PCI/NVMe: trigger PTM update; NVMe precision time protocol */

#define PTM_LOCAL_LSB			0x10	/* PCI/NVMe: local PTM clock LSB */
#define PTM_LOCAL_MSB			0x14	/* PCI/NVMe: local PTM clock MSB */
#define PTM_T1_T2_LSB			0x18	/* PCI/NVMe: PTM t1/t2 LSB */
#define PTM_T1_T2_MSB			0x1c	/* PCI/NVMe: PTM t1/t2 MSB */
#define PTM_T3_T4_LSB			0x28	/* PCI/NVMe: PTM t3/t4 LSB */
#define PTM_T3_T4_MSB			0x2c	/* PCI/NVMe: PTM t3/t4 MSB */
#define PTM_MASTER_LSB			0x38	/* PCI/NVMe: PTM master time LSB */
#define PTM_MASTER_MSB			0x3c	/* PCI/NVMe: PTM master time MSB */

/*
 * The default address offset between dbi_base and atu_base. Root controller
 * drivers are not required to initialize atu_base if the offset matches this
 * default; the driver core automatically derives atu_base from dbi_base using
 * this offset, if atu_base not set.
 */
#define DEFAULT_DBI_ATU_OFFSET (0x3 << 20)	/* PCI/NVMe: default 3 MB DBI->iATU offset; used to set up NVMe outbound mapping */
#define DEFAULT_DBI_DMA_OFFSET PCIE_DMA_UNROLL_BASE	/* PCI/NVMe: default DBI->eDMA offset; optional NVMe DMA offload */

#define MAX_MSI_IRQS			256	/* PCI/NVMe: total MSI vectors DWC can signal; enough for multi-queue NVMe */
#define MAX_MSI_IRQS_PER_CTRL		32	/* PCI/NVMe: vectors per MSI control register; one bit per NVMe vector */
#define MAX_MSI_CTRLS			(MAX_MSI_IRQS / MAX_MSI_IRQS_PER_CTRL)	/* PCI/NVMe: number of MSI control blocks for NVMe MSI */
#define MSI_REG_CTRL_BLOCK_SIZE		12	/* PCI/NVMe: bytes per MSI ctrl block (enable/mask/status) for NVMe vectors */
#define MSI_DEF_NUM_VECTORS		32	/* PCI/NVMe: default MSI vectors allocated; NVMe often requests more */

/* Maximum number of inbound/outbound iATUs */
#define MAX_IATU_IN			256	/* PCI/NVMe: max inbound windows; supports many NVMe P2P or VF mappings */
#define MAX_IATU_OUT			256	/* PCI/NVMe: max outbound windows; supports NVMe config/MMIO/IO mappings */

/* Default eDMA LLP memory size */
#define DMA_LLP_MEM_SIZE		PAGE_SIZE	/* PCI/NVMe: link-list pointer memory for eDMA; used in NVMe offload paths */

/* Common struct pci_epc_feature bits among DWC EP glue drivers */
#define DWC_EPC_COMMON_FEATURES		.dynamic_inbound_mapping = true, \
					.subrange_mapping = true	/* PCI/NVMe: inbound mapping features if SoC acts as NVMe-like endpoint */

struct dw_pcie;		/* PCI/NVMe: forward declaration of DesignWare PCIe controller, parent of NVMe root port */
struct dw_pcie_rp;	/* PCI/NVMe: forward declaration of root port state; bridges to NVMe device */
struct dw_pcie_ep;	/* PCI/NVMe: forward declaration of endpoint state */

enum dw_pcie_device_mode {
	DW_PCIE_UNKNOWN_TYPE,	/* PCI/NVMe: mode not yet detected; NVMe host cannot proceed */
	DW_PCIE_EP_TYPE,	/* PCI/NVMe: endpoint mode; SoC behaves like an NVMe device */
	DW_PCIE_LEG_EP_TYPE,	/* PCI/NVMe: legacy endpoint mode */
	DW_PCIE_RC_TYPE,	/* PCI/NVMe: root complex mode; required to attach an NVMe SSD */
};

enum dw_pcie_app_clk {
	DW_PCIE_DBI_CLK,	/* PCI/NVMe: clock for DBI config interface; needed for NVMe config read/write */
	DW_PCIE_MSTR_CLK,	/* PCI/NVMe: master AXI clock; drives NVMe upstream DMA transactions */
	DW_PCIE_SLV_CLK,	/* PCI/NVMe: slave AXI clock; used when NVMe accesses host memory */
	DW_PCIE_NUM_APP_CLKS	/* PCI/NVMe: total application clocks that must be enabled for NVMe */
};

enum dw_pcie_core_clk {
	DW_PCIE_PIPE_CLK,	/* PCI/NVMe: PIPE clock from PHY; required for NVMe SerDes lanes */
	DW_PCIE_CORE_CLK,	/* PCI/NVMe: core digital clock; powers NVMe link layer logic */
	DW_PCIE_AUX_CLK,	/* PCI/NVMe: auxiliary clock; keeps NVMe link alive in low power */
	DW_PCIE_REF_CLK,	/* PCI/NVMe: reference clock; frequency affects NVMe link speed */
	DW_PCIE_NUM_CORE_CLKS	/* PCI/NVMe: total core clocks required before NVMe enumeration */
};

enum dw_pcie_app_rst {
	DW_PCIE_DBI_RST,	/* PCI/NVMe: DBI reset; released before first NVMe config cycle */
	DW_PCIE_MSTR_RST,	/* PCI/NVMe: master reset; released before NVMe DMA can start */
	DW_PCIE_SLV_RST,	/* PCI/NVMe: slave reset; released before NVMe can access host RAM */
	DW_PCIE_NUM_APP_RSTS	/* PCI/NVMe: total application resets */
};

enum dw_pcie_core_rst {
	DW_PCIE_NON_STICKY_RST,	/* PCI/NVMe: non-sticky config reset; retains some NVMe settings */
	DW_PCIE_STICKY_RST,	/* PCI/NVMe: sticky reset; clears persistent config for NVMe re-init */
	DW_PCIE_CORE_RST,	/* PCI/NVMe: full core reset; used during NVMe hotplug/resume */
	DW_PCIE_PIPE_RST,	/* PCI/NVMe: PIPE interface reset; re-trains NVMe PHY */
	DW_PCIE_PHY_RST,	/* PCI/NVMe: PHY reset; NVMe link drops and re-trains */
	DW_PCIE_HOT_RST,	/* PCI/NVMe: PCIe hot reset propagated to NVMe device */
	DW_PCIE_PWR_RST,	/* PCI/NVMe: power domain reset; full NVMe controller reset */
	DW_PCIE_NUM_CORE_RSTS	/* PCI/NVMe: total core resets */
};

enum dw_pcie_ltssm {
	/* Need to align with PCIE_PORT_DEBUG0 bits 0:5 */
	DW_PCIE_LTSSM_DETECT_QUIET = 0x0,	/* PCI/NVMe: initial detect quiet; NVMe not yet seen */
	DW_PCIE_LTSSM_DETECT_ACT = 0x1,	/* PCI/NVMe: detect active; searching for NVMe device */
	DW_PCIE_LTSSM_POLL_ACTIVE = 0x2,	/* PCI/NVMe: polling active; training with NVMe */
	DW_PCIE_LTSSM_POLL_COMPLIANCE = 0x3,	/* PCI/NVMe: polling compliance; NVMe compliance pattern */
	DW_PCIE_LTSSM_POLL_CONFIG = 0x4,	/* PCI/NVMe: polling config; checking NVMe lane polarity */
	DW_PCIE_LTSSM_PRE_DETECT_QUIET = 0x5,	/* PCI/NVMe: pre-detect quiet */
	DW_PCIE_LTSSM_DETECT_WAIT = 0x6,	/* PCI/NVMe: detect wait */
	DW_PCIE_LTSSM_CFG_LINKWD_START = 0x7,	/* PCI/NVMe: link width negotiation start for NVMe */
	DW_PCIE_LTSSM_CFG_LINKWD_ACEPT = 0x8,	/* PCI/NVMe: link width accepted for NVMe */
	DW_PCIE_LTSSM_CFG_LANENUM_WAI = 0x9,	/* PCI/NVMe: lane number wait */
	DW_PCIE_LTSSM_CFG_LANENUM_ACEPT = 0xa,	/* PCI/NVMe: lane number accepted for NVMe */
	DW_PCIE_LTSSM_CFG_COMPLETE = 0xb,	/* PCI/NVMe: config complete; NVMe config space accessible soon */
	DW_PCIE_LTSSM_CFG_IDLE = 0xc,	/* PCI/NVMe: config idle */
	DW_PCIE_LTSSM_RCVRY_LOCK = 0xd,	/* PCI/NVMe: recovery lock; NVMe link retraining */
	DW_PCIE_LTSSM_RCVRY_SPEED = 0xe,	/* PCI/NVMe: recovery speed; NVMe Gen switch */
	DW_PCIE_LTSSM_RCVRY_RCVRCFG = 0xf,	/* PCI/NVMe: recovery receiver config */
	DW_PCIE_LTSSM_RCVRY_IDLE = 0x10,	/* PCI/NVMe: recovery idle */
	DW_PCIE_LTSSM_L0 = 0x11,	/* PCI/NVMe: L0 active; NVMe enumeration and DMA can run */
	DW_PCIE_LTSSM_L0S = 0x12,	/* PCI/NVMe: L0s low power; NVMe ASPM L0s active */
	DW_PCIE_LTSSM_L123_SEND_EIDLE = 0x13,	/* PCI/NVMe: entering L1/L2/L3; NVMe power state change */
	DW_PCIE_LTSSM_L1_IDLE = 0x14,	/* PCI/NVMe: L1 idle; NVMe ASPM L1 active */
	DW_PCIE_LTSSM_L2_IDLE = 0x15,	/* PCI/NVMe: L2 idle; NVMe deep sleep */
	DW_PCIE_LTSSM_L2_WAKE = 0x16,	/* PCI/NVMe: L2 wake; resuming NVMe from sleep */
	DW_PCIE_LTSSM_DISABLED_ENTRY = 0x17,	/* PCI/NVMe: disabled entry */
	DW_PCIE_LTSSM_DISABLED_IDLE = 0x18,	/* PCI/NVMe: disabled idle */
	DW_PCIE_LTSSM_DISABLED = 0x19,	/* PCI/NVMe: disabled */
	DW_PCIE_LTSSM_LPBK_ENTRY = 0x1a,	/* PCI/NVMe: loopback entry */
	DW_PCIE_LTSSM_LPBK_ACTIVE = 0x1b,	/* PCI/NVMe: loopback active */
	DW_PCIE_LTSSM_LPBK_EXIT = 0x1c,	/* PCI/NVMe: loopback exit */
	DW_PCIE_LTSSM_LPBK_EXIT_TIMEOUT = 0x1d,	/* PCI/NVMe: loopback exit timeout */
	DW_PCIE_LTSSM_HOT_RESET_ENTRY = 0x1e,	/* PCI/NVMe: hot reset entry; NVMe device will reset */
	DW_PCIE_LTSSM_HOT_RESET = 0x1f,	/* PCI/NVMe: hot reset active; NVMe registers cleared */
	DW_PCIE_LTSSM_RCVRY_EQ0 = 0x20,	/* PCI/NVMe: recovery equalization phase 0 for NVMe Gen3+ */
	DW_PCIE_LTSSM_RCVRY_EQ1 = 0x21,	/* PCI/NVMe: recovery equalization phase 1 */
	DW_PCIE_LTSSM_RCVRY_EQ2 = 0x22,	/* PCI/NVMe: recovery equalization phase 2 */
	DW_PCIE_LTSSM_RCVRY_EQ3 = 0x23,	/* PCI/NVMe: recovery equalization phase 3 */

	/* Vendor glue drivers provide pseudo L1 substates from get_ltssm() */
	DW_PCIE_LTSSM_L1_1 = 0x141,	/* PCI/NVMe: pseudo L1.1 state; NVMe L1SS save */
	DW_PCIE_LTSSM_L1_2 = 0x142,	/* PCI/NVMe: pseudo L1.2 state; NVMe deeper L1SS save */

	DW_PCIE_LTSSM_UNKNOWN = 0xFFFFFFFF,	/* PCI/NVMe: unknown LTSSM; NVMe link debug stops here */
};

struct dw_pcie_ob_atu_cfg {
	int index;	/* PCI/NVMe: outbound iATU region index for this NVMe mapping */
	int type;	/* PCI/NVMe: TLP type: MEM/IO/CFG0/CFG1/MSG for NVMe access */
	u8 func_no;	/* PCI/NVMe: target physical function; SR-IOV NVMe VFs need this */
	u8 code;	/* PCI/NVMe: message code when type is MSG; used for NVMe PME/AER injection */
	u8 routing;	/* PCI/NVMe: message routing field for NVMe broadcast/ID routed messages */
	u32 ctrl2;	/* PCI/NVMe: extra region ctrl2 flags; e.g., CFG shift for NVMe ECAM */
	u64 parent_bus_addr;	/* PCI/NVMe: CPU-side base address of the NVMe window */
	u64 pci_addr;	/* PCI/NVMe: PCIe bus address seen by the NVMe device */
	u64 size;	/* PCI/NVMe: window size; must cover NVMe BAR apertures */
};

struct dw_pcie_host_ops {
	int (*init)(struct dw_pcie_rp *pp);	/* PCI/NVMe: platform-specific RC init; sets up clocks/phys before NVMe enumeration */
	void (*deinit)(struct dw_pcie_rp *pp);	/* PCI/NVMe: tear down RC resources after NVMe unbind */
	void (*post_init)(struct dw_pcie_rp *pp);	/* PCI/NVMe: late init after link up; may tune NVMe parameters */
	int (*msi_init)(struct dw_pcie_rp *pp);	/* PCI/NVMe: optional MSI controller init for NVMe interrupts */
	void (*pme_turn_off)(struct dw_pcie_rp *pp);	/* PCI/NVMe: send PME_Turn_Off before NVMe power down */
};

struct dw_pcie_rp {
	bool			use_imsi_rx:1;	/* PCI/NVMe: use inbound MSI receiver for NVMe MSI handling */
	bool			keep_rp_msi_en:1;	/* PCI/NVMe: keep root-port MSI enable across suspend for NVMe resume */
	bool			cfg0_io_shared:1;	/* PCI/NVMe: CFG0 and IO space share an iATU region on some NVMe platforms */
	u64			cfg0_base;	/* PCI/NVMe: CPU base of type-0 config window for NVMe device on bus 0 */
	void __iomem		*va_cfg0_base;	/* PCI/NVMe: virtual address of NVMe config window 0 */
	u32			cfg0_size;	/* PCI/NVMe: size of type-0 config window; covers NVMe config space */
	resource_size_t		io_base;	/* PCI/NVMe: CPU base of IO aperture; NVMe rarely uses IO BARs */
	phys_addr_t		io_bus_addr;	/* PCI/NVMe: PCIe bus address of IO aperture for NVMe IO BARs */
	u32			io_size;	/* PCI/NVMe: size of IO aperture */
	int			irq;	/* PCI/NVMe: wired interrupt line for legacy NVMe INTx or MSI aggregator */
	const struct dw_pcie_host_ops *ops;	/* PCI/NVMe: platform callback vector; controls NVMe-specific init */
	int			msi_irq[MAX_MSI_CTRLS];	/* PCI/NVMe: Linux IRQ numbers per MSI control block for NVMe vectors */
	struct irq_domain	*irq_domain;	/* PCI/NVMe: MSI IRQ domain; NVMe MSI/MSI-X vectors allocated from here */
	dma_addr_t		msi_data;	/* PCI/NVMe: DMA address written by NVMe MSI messages */
	struct irq_chip		*msi_irq_chip;	/* PCI/NVMe: irq_chip for NVMe MSI domain */
	u32			num_vectors;	/* PCI/NVMe: number of MSI vectors actually usable by NVMe */
	u32			irq_mask[MAX_MSI_CTRLS];	/* PCI/NVMe: software mask cache for NVMe MSI groups */
	struct pci_host_bridge  *bridge;	/* PCI/NVMe: Linux PCI host bridge; eventually probes drivers/nvme/host/pci.c */
	raw_spinlock_t		lock;	/* PCI/NVMe: protects MSI bitmap and iATU programming races with NVMe ISR */
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_IRQS);	/* PCI/NVMe: bitmap of MSI vectors currently assigned to NVMe queues/functions */
	bool			use_atu_msg;	/* PCI/NVMe: allocate iATU MSG region for NVMe PME/AER messages */
	int			msg_atu_index;	/* PCI/NVMe: iATU index reserved for NVMe message TLPs */
	struct resource		*msg_res;	/* PCI/NVMe: resource describing NVMe message region */
	struct pci_eq_presets	presets;	/* PCI/NVMe: equalization presets; affects NVMe Gen3/Gen4 eye */
	struct pci_config_window *cfg;	/* PCI/NVMe: ECAM config window; used to enumerate NVMe behind bridges */
	bool			ecam_enabled;	/* PCI/NVMe: true if ECAM path is used for NVMe config access */
	bool			native_ecam;	/* PCI/NVMe: true if hardware ECAM is native, else DWC DBI fallback */
	bool                    skip_l23_ready;	/* PCI/NVMe: skip L2/L3 ready handshake; speeds NVMe shutdown */
};

struct dw_pcie_ep_ops {
	void	(*pre_init)(struct dw_pcie_ep *ep);	/* PCI/NVMe: pre-initialization before EP registers are touched */
	void	(*init)(struct dw_pcie_ep *ep);	/* PCI/NVMe: EP init; when SoC acts as NVMe-like device */
	int	(*raise_irq)(struct dw_pcie_ep *ep, u8 func_no,
			     unsigned int type, u16 interrupt_num);	/* PCI/NVMe: raise IRQ to host; emulates NVMe interrupt injection */
	const struct pci_epc_features* (*get_features)(struct dw_pcie_ep *ep);	/* PCI/NVMe: report EP capabilities for NVMe function setup */
	/*
	 * Provide a method to implement the different func config space
	 * access for different platform, if different func have different
	 * offset, return the offset of func. if use write a register way
	 * return a 0, and implement code in callback function of platform
	 * driver.
	 */
	unsigned int (*get_dbi_offset)(struct dw_pcie_ep *ep, u8 func_no);	/* PCI/NVMe: per-function DBI offset for NVMe function config space */
	unsigned int (*get_dbi2_offset)(struct dw_pcie_ep *ep, u8 func_no);	/* PCI/NVMe: per-function DBI2 offset for NVMe function config space */
};

struct dw_pcie_ep_func {
	struct list_head	list;	/* PCI/NVMe: linked list of NVMe endpoint functions */
	u8			func_no;	/* PCI/NVMe: function number; corresponds to NVMe PF/VF */
	u8			msi_cap;	/* MSI capability offset */
	u8			msix_cap;	/* PCI/NVMe: MSI-X capability offset; NVMe drives use this */
	u8			bar_to_atu[PCI_STD_NUM_BARS];	/* PCI/NVMe: maps each NVMe BAR to an inbound iATU index */
	struct pci_epf_bar	*epf_bar[PCI_STD_NUM_BARS];	/* PCI/NVMe: BAR descriptors for NVMe function */

	/* Only for Address Match Mode inbound iATU */
	u32			*ib_atu_indexes[PCI_STD_NUM_BARS];	/* PCI/NVMe: inbound iATU indexes assigned to NVMe BARs */
	unsigned int		num_ib_atu_indexes[PCI_STD_NUM_BARS];	/* PCI/NVMe: count of inbound indexes per NVMe BAR */
};

struct dw_pcie_ep {
	struct pci_epc		*epc;	/* PCI/NVMe: endpoint controller; not used in NVMe host mode */
	struct list_head	func_list;	/* PCI/NVMe: list of NVMe-like endpoint functions */
	const struct dw_pcie_ep_ops *ops;	/* PCI/NVMe: EP callback vector */
	phys_addr_t		phys_base;	/* PCI/NVMe: physical base of controller registers */
	size_t			addr_size;	/* PCI/NVMe: addressable size for NVMe EP BARs */
	size_t			page_size;	/* PCI/NVMe: inbound page size for NVMe DMA */
	phys_addr_t		*outbound_addr;	/* PCI/NVMe: outbound CPU addresses for NVMe EP DMA */
	unsigned long		*ib_window_map;	/* PCI/NVMe: bitmap of inbound windows used by NVMe BARs */
	unsigned long		*ob_window_map;	/* PCI/NVMe: bitmap of outbound windows used by NVMe EP */
	void __iomem		*msi_mem;	/* PCI/NVMe: MSI target memory for host-triggered NVMe EP interrupts */
	phys_addr_t		msi_mem_phys;	/* PCI/NVMe: physical address of MSI target memory */

	/* MSI outbound iATU state */
	bool			msi_iatu_mapped;	/* PCI/NVMe: true if MSI outbound iATU is mapped for NVMe EP */
	u64			msi_msg_addr;	/* PCI/NVMe: PCIe address for NVMe EP MSI messages */
	size_t			msi_map_size;	/* PCI/NVMe: size of MSI outbound mapping */
};

struct dw_pcie_ops {
	u64	(*cpu_addr_fixup)(struct dw_pcie *pcie, u64 cpu_addr);	/* PCI/NVMe: convert CPU addr to iATU input addr; affects NVMe DMA aperture */
	u32	(*read_dbi)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			    size_t size);	/* PCI/NVMe: read DBI register; used for NVMe config space access */
	void	(*write_dbi)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			     size_t size, u32 val);	/* PCI/NVMe: write DBI register; programs NVMe link/config settings */
	void    (*write_dbi2)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			      size_t size, u32 val);	/* PCI/NVMe: write DBI2 register; updates NVMe shadow fields */
	bool	(*link_up)(struct dw_pcie *pcie);	/* PCI/NVMe: return true when NVMe link is up */
	enum dw_pcie_ltssm (*get_ltssm)(struct dw_pcie *pcie);	/* PCI/NVMe: read LTSSM state for NVMe link diagnostics */
	int	(*start_link)(struct dw_pcie *pcie);	/* PCI/NVMe: start NVMe link training */
	void	(*stop_link)(struct dw_pcie *pcie);	/* PCI/NVMe: stop NVMe link for suspend/hotplug */
};

struct debugfs_info {
	struct dentry		*debug_dir;	/* PCI/NVMe: debugfs directory; NVMe link/registers exposed here */
	void			*rasdes_info;	/* PCI/NVMe: RAS/DES debugfs data for NVMe error diagnostics */
};

struct dw_pcie {
	struct device		*dev;	/* PCI/NVMe: Linux device; parent of the NVMe root port */
	void __iomem		*dbi_base;	/* PCI/NVMe: DBI register base; used for NVMe config and link regs */
	resource_size_t		dbi_phys_addr;	/* PCI/NVMe: physical DBI base for iomem resources */
	void __iomem		*dbi_base2;	/* PCI/NVMe: secondary DBI base for some NVMe shadow registers */
	void __iomem		*atu_base;	/* PCI/NVMe: iATU register base; maps NVMe config/MMIO windows */
	void __iomem		*elbi_base;	/* PCI/NVMe: ELBI register base; platform-specific NVMe glue */
	resource_size_t		atu_phys_addr;	/* PCI/NVMe: physical iATU base */
	size_t			atu_size;	/* PCI/NVMe: iATU window size */
	resource_size_t		parent_bus_offset;	/* PCI/NVMe: offset applied to CPU addresses for NVMe outbound iATU */
	u32			num_ib_windows;	/* PCI/NVMe: number of inbound iATU windows available for NVMe P2P */
	u32			num_ob_windows;	/* PCI/NVMe: number of outbound iATU windows available for NVMe */
	u32			region_align;	/* PCI/NVMe: minimum iATU region alignment for NVMe BAR mapping */
	u64			region_limit;	/* PCI/NVMe: maximum addressable iATU region for NVMe aperture */
	struct dw_pcie_rp	pp;	/* PCI/NVMe: root port state; bridges to NVMe SSD */
	struct dw_pcie_ep	ep;	/* PCI/NVMe: endpoint state; unused in NVMe host mode */
	const struct dw_pcie_ops *ops;	/* PCI/NVMe: low-level ops; may be overridden for NVMe platform quirks */
	u32			version;	/* PCI/NVMe: detected DWC IP version; selects NVMe-safe code paths */
	u32			type;	/* PCI/NVMe: controller type; RC required for NVMe host */
	unsigned long		caps;	/* PCI/NVMe: capability bitmap; e.g., IATU_UNROLL speeds NVMe init */
	int			num_lanes;	/* PCI/NVMe: lane count; limits NVMe link width */
	int			max_link_speed;	/* PCI/NVMe: max link speed; caps NVMe Gen3/Gen4/Gen5 performance */
	u8			n_fts[2];	/* PCI/NVMe: FTS values for Gen1/Gen2 vs Gen3+; affects NVMe L0s exit */
	struct dw_edma_chip	edma;	/* PCI/NVMe: eDMA chip data; optional NVMe offload engine */
	bool			l1ss_support;	/* L1 PM Substates support */	/* PCI/NVMe: L1SS support; enables deeper NVMe power saving */
	struct clk_bulk_data	app_clks[DW_PCIE_NUM_APP_CLKS];	/* PCI/NVMe: application clock bulk data; enabled before NVMe probe */
	struct clk_bulk_data	core_clks[DW_PCIE_NUM_CORE_CLKS];	/* PCI/NVMe: core clock bulk data; enabled before NVMe link training */
	struct reset_control_bulk_data	app_rsts[DW_PCIE_NUM_APP_RSTS];	/* PCI/NVMe: application reset controls; deasserted for NVMe */
	struct reset_control_bulk_data	core_rsts[DW_PCIE_NUM_CORE_RSTS];	/* PCI/NVMe: core reset controls; deasserted for NVMe */
	struct gpio_desc		*pe_rst;	/* PCI/NVMe: PERST# GPIO; toggled to reset NVMe SSD */
	bool			suspended;	/* PCI/NVMe: suspend flag; prevents NVMe access in sleep */
	struct debugfs_info	*debugfs;	/* PCI/NVMe: debugfs state; exposes NVMe link/registers */
	enum			dw_pcie_device_mode mode;	/* PCI/NVMe: current mode; must be RC_TYPE for NVMe host */
	u16			ptm_vsec_offset;	/* PCI/NVMe: PTM VSEC offset for NVMe precision time measurements */
	struct pci_ptm_debugfs	*ptm_debugfs;	/* PCI/NVMe: PTM debugfs state for NVMe timing diagnostics */

	/*
	 * If iATU input addresses are offset from CPU physical addresses,
	 * we previously required .cpu_addr_fixup() to convert them.  We
	 * now rely on the devicetree instead.  If .cpu_addr_fixup()
	 * exists, we compare its results with devicetree.
	 *
	 * If .cpu_addr_fixup() does not exist, we assume the offset is
	 * zero and warn if devicetree claims otherwise.  If we know all
	 * devicetrees correctly describe the offset, set
	 * use_parent_dt_ranges to true to avoid this warning.
	 */
	bool			use_parent_dt_ranges;	/* PCI/NVMe: trust DT ranges for NVMe outbound iATU address translation */
};

#define to_dw_pcie_from_pp(port) container_of((port), struct dw_pcie, pp)	/* PCI/NVMe: retrieve dw_pcie from root port; used in NVMe host irq/map ops */

#define to_dw_pcie_from_ep(endpoint)   \
		container_of((endpoint), struct dw_pcie, ep)	/* PCI/NVMe: retrieve dw_pcie from EP state */

int dw_pcie_get_resources(struct dw_pcie *pci);	/* PCI/NVMe: parse DT/ACPI resources (clocks/resets/DBI/iATU) for NVMe host */

void dw_pcie_version_detect(struct dw_pcie *pci);	/* PCI/NVMe: read DWC version/type; determines NVMe init quirks */

u8 dw_pcie_find_capability(struct dw_pcie *pci, u8 cap);	/* PCI/NVMe: find PCI capability (e.g., MSI/MSI-X/PowerManagement) for NVMe */
u16 dw_pcie_find_ext_capability(struct dw_pcie *pci, u8 cap);	/* PCI/NVMe: find PCIe extended capability (e.g., AER/ACS/PTM) for NVMe */
void dw_pcie_remove_capability(struct dw_pcie *pci, u8 cap);	/* PCI/NVMe: remove a PCI capability from internal config space */
void dw_pcie_remove_ext_capability(struct dw_pcie *pci, u8 cap);	/* PCI/NVMe: remove an extended capability from internal config space */
u16 dw_pcie_find_rasdes_capability(struct dw_pcie *pci);	/* PCI/NVMe: find RAS/DES capability for NVMe error counters */
u16 dw_pcie_find_ptm_capability(struct dw_pcie *pci);	/* PCI/NVMe: find PTM capability for NVMe time sync */

int dw_pcie_read(void __iomem *addr, int size, u32 *val);	/* PCI/NVMe: safe MMIO read with size validation; used for NVMe register access */
int dw_pcie_write(void __iomem *addr, int size, u32 val);	/* PCI/NVMe: safe MMIO write with size validation; used for NVMe register access */

u32 dw_pcie_read_dbi(struct dw_pcie *pci, u32 reg, size_t size);	/* PCI/NVMe: read DBI register at given width; NVMe config reads go through this */
void dw_pcie_write_dbi(struct dw_pcie *pci, u32 reg, size_t size, u32 val);	/* PCI/NVMe: write DBI register; NVMe config/link settings */
void dw_pcie_write_dbi2(struct dw_pcie *pci, u32 reg, size_t size, u32 val);	/* PCI/NVMe: write DBI2 register; NVMe shadow config fields */
bool dw_pcie_link_up(struct dw_pcie *pci);	/* PCI/NVMe: return true if NVMe link is up */
void dw_pcie_upconfig_setup(struct dw_pcie *pci);	/* PCI/NVMe: enable link width up-config for NVMe */
int dw_pcie_wait_for_link(struct dw_pcie *pci);	/* PCI/NVMe: poll until NVMe link reaches L0 or timeout */
int dw_pcie_link_get_max_link_width(struct dw_pcie *pci);	/* PCI/NVMe: read negotiated link width for NVMe diagnostics */
int dw_pcie_prog_outbound_atu(struct dw_pcie *pci,
			      const struct dw_pcie_ob_atu_cfg *atu);	/* PCI/NVMe: program outbound iATU for NVMe config/MMIO/IO/msg window */
int dw_pcie_prog_inbound_atu(struct dw_pcie *pci, int index, int type,
			     u64 parent_bus_addr, u64 pci_addr, u64 size);	/* PCI/NVMe: program inbound iATU; maps NVMe DMA addresses to host RAM */
int dw_pcie_prog_ep_inbound_atu(struct dw_pcie *pci, u8 func_no, int index,
				int type, u64 parent_bus_addr,
				u8 bar, size_t size);	/* PCI/NVMe: program inbound iATU for NVMe endpoint BARs */
void dw_pcie_disable_atu(struct dw_pcie *pci, u32 dir, int index);	/* PCI/NVMe: disable an iATU region; used on NVMe unbind/suspend */
void dw_pcie_hide_unsupported_l1ss(struct dw_pcie *pci);	/* PCI/NVMe: hide L1SS bits if unsupported; avoids NVMe ASPM negotiation failures */
void dw_pcie_setup(struct dw_pcie *pci);	/* PCI/NVMe: common RC setup before NVMe enumeration (link, iATU, MSI) */
void dw_pcie_iatu_detect(struct dw_pcie *pci);	/* PCI/NVMe: detect iATU layout (viewport vs unrolled) for NVMe mapping */
int dw_pcie_edma_detect(struct dw_pcie *pci);	/* PCI/NVMe: detect eDMA engine; optional NVMe offload */
void dw_pcie_edma_remove(struct dw_pcie *pci);	/* PCI/NVMe: remove eDMA engine on NVMe unbind */
resource_size_t dw_pcie_parent_bus_offset(struct dw_pcie *pci,
					  const char *reg_name,
					  resource_size_t cpu_phy_addr);	/* PCI/NVMe: compute parent bus offset for NVMe outbound iATU CPU->bus translation */

static inline void dw_pcie_writel_dbi(struct dw_pcie *pci, u32 reg, u32 val)
{
	dw_pcie_write_dbi(pci, reg, 0x4, val);	/* PCI/NVMe: 32-bit DBI write; used for NVMe config dword writes */
}

static inline u32 dw_pcie_readl_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x4);	/* PCI/NVMe: 32-bit DBI read; NVMe config dword reads */
}

static inline void dw_pcie_writew_dbi(struct dw_pcie *pci, u32 reg, u16 val)
{
	dw_pcie_write_dbi(pci, reg, 0x2, val);	/* PCI/NVMe: 16-bit DBI write; NVMe config word writes */
}

static inline u16 dw_pcie_readw_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x2);	/* PCI/NVMe: 16-bit DBI read; NVMe config word reads */
}

static inline void dw_pcie_writeb_dbi(struct dw_pcie *pci, u32 reg, u8 val)
{
	dw_pcie_write_dbi(pci, reg, 0x1, val);	/* PCI/NVMe: 8-bit DBI write; NVMe config byte writes */
}

static inline u8 dw_pcie_readb_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x1);	/* PCI/NVMe: 8-bit DBI read; NVMe config byte reads */
}

static inline void dw_pcie_writel_dbi2(struct dw_pcie *pci, u32 reg, u32 val)
{
	dw_pcie_write_dbi2(pci, reg, 0x4, val);	/* PCI/NVMe: 32-bit DBI2 write; updates NVMe shadow fields */
}

static inline int dw_pcie_read_cfg_byte(struct dw_pcie *pci, int where,
					u8 *val)
{
	*val = dw_pcie_readb_dbi(pci, where);	/* PCI/NVMe: emulate pci_read_config_byte for NVMe config space */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success code as expected by PCI core during NVMe enumeration */
}

static inline int dw_pcie_read_cfg_word(struct dw_pcie *pci, int where,
					u16 *val)
{
	*val = dw_pcie_readw_dbi(pci, where);	/* PCI/NVMe: emulate pci_read_config_word for NVMe config space */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success code for PCI core NVMe probe */
}

static inline int dw_pcie_read_cfg_dword(struct dw_pcie *pci, int where,
					 u32 *val)
{
	*val = dw_pcie_readl_dbi(pci, where);	/* PCI/NVMe: emulate pci_read_config_dword; reads NVMe BAR/MSI-X caps */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success code for PCI core NVMe probe */
}

static inline unsigned int dw_pcie_ep_get_dbi_offset(struct dw_pcie_ep *ep,
						     u8 func_no)
{
	unsigned int dbi_offset = 0;	/* PCI/NVMe: default no offset for NVMe function config access */

	if (ep->ops->get_dbi_offset)	/* PCI/NVMe: platform may relocate per-function DBI for NVMe SR-IOV */
		dbi_offset = ep->ops->get_dbi_offset(ep, func_no);	/* PCI/NVMe: apply per-function offset for NVMe config space */

	return dbi_offset;	/* PCI/NVMe: offset added to DBI address for this NVMe function */
}

static inline u32 dw_pcie_ep_read_dbi(struct dw_pcie_ep *ep, u8 func_no,
				      u32 reg, size_t size)
{
	unsigned int offset = dw_pcie_ep_get_dbi_offset(ep, func_no);	/* PCI/NVMe: compute NVMe function DBI offset */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* PCI/NVMe: retrieve parent controller */

	return dw_pcie_read_dbi(pci, offset + reg, size);	/* PCI/NVMe: read NVMe function config register */
}

static inline void dw_pcie_ep_write_dbi(struct dw_pcie_ep *ep, u8 func_no,
					u32 reg, size_t size, u32 val)
{
	unsigned int offset = dw_pcie_ep_get_dbi_offset(ep, func_no);	/* PCI/NVMe: compute NVMe function DBI offset */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* PCI/NVMe: retrieve parent controller */

	dw_pcie_write_dbi(pci, offset + reg, size, val);	/* PCI/NVMe: write NVMe function config register */
}

static inline void dw_pcie_ep_writel_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u32 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x4, val);	/* PCI/NVMe: 32-bit NVMe function DBI write */
}

static inline u32 dw_pcie_ep_readl_dbi(struct dw_pcie_ep *ep, u8 func_no,
				       u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x4);	/* PCI/NVMe: 32-bit NVMe function DBI read */
}

static inline void dw_pcie_ep_writew_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u16 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x2, val);	/* PCI/NVMe: 16-bit NVMe function DBI write */
}

static inline u16 dw_pcie_ep_readw_dbi(struct dw_pcie_ep *ep, u8 func_no,
				       u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x2);	/* PCI/NVMe: 16-bit NVMe function DBI read */
}

static inline void dw_pcie_ep_writeb_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u8 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x1, val);	/* PCI/NVMe: 8-bit NVMe function DBI write */
}

static inline u8 dw_pcie_ep_readb_dbi(struct dw_pcie_ep *ep, u8 func_no,
				      u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x1);	/* PCI/NVMe: 8-bit NVMe function DBI read */
}

static inline int dw_pcie_ep_read_cfg_byte(struct dw_pcie_ep *ep, u8 func_no,
					   int where, u8 *val)
{
	*val = dw_pcie_ep_readb_dbi(ep, func_no, where);	/* PCI/NVMe: NVMe EP config byte read */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success for NVMe EP config access */
}

static inline int dw_pcie_ep_read_cfg_word(struct dw_pcie_ep *ep, u8 func_no,
					   int where, u16 *val)
{
	*val = dw_pcie_ep_readw_dbi(ep, func_no, where);	/* PCI/NVMe: NVMe EP config word read */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success for NVMe EP config access */
}

static inline int dw_pcie_ep_read_cfg_dword(struct dw_pcie_ep *ep, u8 func_no,
					    int where, u32 *val)
{
	*val = dw_pcie_ep_readl_dbi(ep, func_no, where);	/* PCI/NVMe: NVMe EP config dword read */
	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: success for NVMe EP config access */
}

static inline unsigned int dw_pcie_ep_get_dbi2_offset(struct dw_pcie_ep *ep,
						      u8 func_no)
{
	unsigned int dbi2_offset = 0;	/* PCI/NVMe: default no DBI2 offset for NVMe function */

	if (ep->ops->get_dbi2_offset)	/* PCI/NVMe: platform-specific DBI2 offset for NVMe function */
		dbi2_offset = ep->ops->get_dbi2_offset(ep, func_no);	/* PCI/NVMe: apply DBI2 offset */
	else if (ep->ops->get_dbi_offset)     /* for backward compatibility */	/* PCI/NVMe: fall back to DBI offset if DBI2 not provided for NVMe */
		dbi2_offset = ep->ops->get_dbi_offset(ep, func_no);	/* PCI/NVMe: backward-compatible offset for NVMe function */

	return dbi2_offset;	/* PCI/NVMe: offset added to DBI2 address for this NVMe function */
}

static inline void dw_pcie_ep_write_dbi2(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, size_t size, u32 val)
{
	unsigned int offset = dw_pcie_ep_get_dbi2_offset(ep, func_no);	/* PCI/NVMe: compute NVMe function DBI2 offset */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* PCI/NVMe: retrieve parent controller */

	dw_pcie_write_dbi2(pci, offset + reg, size, val);	/* PCI/NVMe: write NVMe function DBI2 register */
}

static inline void dw_pcie_ep_writel_dbi2(struct dw_pcie_ep *ep, u8 func_no,
					  u32 reg, u32 val)
{
	dw_pcie_ep_write_dbi2(ep, func_no, reg, 0x4, val);	/* PCI/NVMe: 32-bit NVMe function DBI2 write */
}

static inline void dw_pcie_dbi_ro_wr_en(struct dw_pcie *pci)
{
	u32 reg;
	u32 val;

	reg = PCIE_MISC_CONTROL_1_OFF;	/* PCI/NVMe: select DBI RO write enable register */
	val = dw_pcie_readl_dbi(pci, reg);	/* PCI/NVMe: read current value */
	val |= PCIE_DBI_RO_WR_EN;	/* PCI/NVMe: set RO write enable bit to modify NVMe link caps */
	dw_pcie_writel_dbi(pci, reg, val);	/* PCI/NVMe: write back; now RO config fields can be changed for NVMe init */
}

static inline void dw_pcie_dbi_ro_wr_dis(struct dw_pcie *pci)
{
	u32 reg;
	u32 val;

	reg = PCIE_MISC_CONTROL_1_OFF;	/* PCI/NVMe: select DBI RO write enable register */
	val = dw_pcie_readl_dbi(pci, reg);	/* PCI/NVMe: read current value */
	val &= ~PCIE_DBI_RO_WR_EN;	/* PCI/NVMe: clear RO write enable bit to protect NVMe config fields */
	dw_pcie_writel_dbi(pci, reg, val);	/* PCI/NVMe: write back; RO fields are read-only again for NVMe */
}

static inline int dw_pcie_start_link(struct dw_pcie *pci)
{
	if (pci->ops && pci->ops->start_link)	/* PCI/NVMe: platform-specific link start exists */
		return pci->ops->start_link(pci);	/* PCI/NVMe: initiate NVMe link training */

	return 0;	/* PCI/NVMe: no platform callback; assume link already started for NVMe */
}

static inline void dw_pcie_stop_link(struct dw_pcie *pci)
{
	if (pci->ops && pci->ops->stop_link)	/* PCI/NVMe: platform-specific link stop exists */
		pci->ops->stop_link(pci);	/* PCI/NVMe: stop NVMe link for suspend/hotplug */
}

static inline enum dw_pcie_ltssm dw_pcie_get_ltssm(struct dw_pcie *pci)
{
	u32 val;

	if (pci->ops && pci->ops->get_ltssm)	/* PCI/NVMe: platform overrides LTSSM read for NVMe diagnostics */
		return pci->ops->get_ltssm(pci);	/* PCI/NVMe: return platform-specific LTSSM state */

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG0);	/* PCI/NVMe: default: read LTSSM from debug register */

	return (enum dw_pcie_ltssm)FIELD_GET(PORT_LOGIC_LTSSM_STATE_MASK, val);	/* PCI/NVMe: extract LTSSM state for NVMe link status checks */
}

const char *dw_pcie_ltssm_status_string(enum dw_pcie_ltssm ltssm);	/* PCI/NVMe: human-readable LTSSM name for NVMe debugfs/logs */

#ifdef CONFIG_PCIE_DW_HOST
int dw_pcie_suspend_noirq(struct dw_pcie *pci);	/* PCI/NVMe: suspend callback before IRQs off; saves NVMe link state */
int dw_pcie_resume_noirq(struct dw_pcie *pci);	/* PCI/NVMe: resume callback before IRQs on; restores NVMe link state */
void dw_handle_msi_irq(struct dw_pcie_rp *pp);	/* PCI/NVMe: handle aggregated MSI IRQ from NVMe device */
void dw_pcie_msi_init(struct dw_pcie_rp *pp);	/* PCI/NVMe: initialize DWC MSI hardware for NVMe vectors */
int dw_pcie_msi_host_init(struct dw_pcie_rp *pp);	/* PCI/NVMe: host-side MSI domain init for NVMe interrupts */
void dw_pcie_free_msi(struct dw_pcie_rp *pp);	/* PCI/NVMe: free MSI domain and vectors when NVMe unbinds */
int dw_pcie_setup_rc(struct dw_pcie_rp *pp);	/* PCI/NVMe: configure root complex for NVMe enumeration */
int dw_pcie_host_init(struct dw_pcie_rp *pp);	/* PCI/NVMe: full host init; eventually calls pci_scan_root_bus for NVMe */
void dw_pcie_host_deinit(struct dw_pcie_rp *pp);	/* PCI/NVMe: tear down host bridge after NVMe removal */
int dw_pcie_allocate_domains(struct dw_pcie_rp *pp);	/* PCI/NVMe: allocate MSI/MSI-X IRQ domains for NVMe */
void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus, unsigned int devfn,
				       int where);	/* PCI/NVMe: map_bus callback for own config space; used during NVMe enumeration */
#else
static inline int dw_pcie_suspend_noirq(struct dw_pcie *pci)
{
	return 0;	/* PCI/NVMe: host support disabled; no NVMe suspend work */
}

static inline int dw_pcie_resume_noirq(struct dw_pcie *pci)
{
	return 0;	/* PCI/NVMe: host support disabled; no NVMe resume work */
}

static inline void dw_handle_msi_irq(struct dw_pcie_rp *pp) { }	/* PCI/NVMe: host support disabled; no NVMe MSI handling */

static inline void dw_pcie_msi_init(struct dw_pcie_rp *pp)
{ }	/* PCI/NVMe: host support disabled; MSI not initialized for NVMe */

static inline int dw_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	return -ENODEV;	/* PCI/NVMe: host support disabled; NVMe MSI unavailable */
}

static inline void dw_pcie_free_msi(struct dw_pcie_rp *pp)
{ }	/* PCI/NVMe: host support disabled; nothing to free */

static inline int dw_pcie_setup_rc(struct dw_pcie_rp *pp)
{
	return 0;	/* PCI/NVMe: host support disabled; RC setup is no-op */
}

static inline int dw_pcie_host_init(struct dw_pcie_rp *pp)
{
	return 0;	/* PCI/NVMe: host support disabled; no NVMe enumeration */
}

static inline void dw_pcie_host_deinit(struct dw_pcie_rp *pp)
{
}	/* PCI/NVMe: host support disabled; no deinit */

static inline int dw_pcie_allocate_domains(struct dw_pcie_rp *pp)
{
	return 0;	/* PCI/NVMe: host support disabled; no IRQ domain for NVMe */
}
static inline void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus,
					     unsigned int devfn,
					     int where)
{
	return NULL;	/* PCI/NVMe: host support disabled; no config access for NVMe */
}
#endif

#ifdef CONFIG_PCIE_DW_EP
void dw_pcie_ep_linkup(struct dw_pcie_ep *ep);	/* PCI/NVMe: notify EP layer that NVMe link is up */
void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep);	/* PCI/NVMe: notify EP layer that NVMe link is down */
int dw_pcie_ep_init(struct dw_pcie_ep *ep);	/* PCI/NVMe: initialize EP controller; SoC as NVMe-like device */
int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep);	/* PCI/NVMe: init EP config registers for NVMe function */
void dw_pcie_ep_deinit(struct dw_pcie_ep *ep);	/* PCI/NVMe: deinitialize EP controller */
void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep);	/* PCI/NVMe: cleanup EP resources */
int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no);	/* PCI/NVMe: raise legacy INTx IRQ to NVMe host */
int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u8 interrupt_num);	/* PCI/NVMe: raise MSI IRQ to NVMe host */
int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u16 interrupt_num);	/* PCI/NVMe: raise MSI-X IRQ to NVMe host */
int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep, u8 func_no,
				       u16 interrupt_num);	/* PCI/NVMe: raise MSI-X via doorbell; used by NVMe EP emulation */
void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar);	/* PCI/NVMe: reset a BAR for NVMe EP function */
struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no);	/* PCI/NVMe: lookup NVMe EP function by number */
#else
static inline void dw_pcie_ep_linkup(struct dw_pcie_ep *ep)
{
}	/* PCI/NVMe: EP support disabled; no NVMe link-up notification */

static inline void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep)
{
}	/* PCI/NVMe: EP support disabled; no NVMe link-down notification */

static inline int dw_pcie_ep_init(struct dw_pcie_ep *ep)
{
	return 0;	/* PCI/NVMe: EP support disabled; NVMe EP init no-op */
}

static inline int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep)
{
	return 0;	/* PCI/NVMe: EP support disabled; no NVMe EP registers to init */
}

static inline void dw_pcie_ep_deinit(struct dw_pcie_ep *ep)
{
}	/* PCI/NVMe: EP support disabled; no deinit */

static inline void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep)
{
}	/* PCI/NVMe: EP support disabled; no cleanup */

static inline int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no)
{
	return 0;	/* PCI/NVMe: EP support disabled; no NVMe INTx injection */
}

static inline int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
					   u8 interrupt_num)
{
	return 0;	/* PCI/NVMe: EP support disabled; no NVMe MSI injection */
}

static inline int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
					    u16 interrupt_num)
{
	return 0;	/* PCI/NVMe: EP support disabled; no NVMe MSI-X injection */
}

static inline int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep,
					     u8 func_no,
					     u16 interrupt_num)
{
	return 0;	/* PCI/NVMe: EP support disabled; no NVMe MSI-X doorbell */
}

static inline void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar)
{
}	/* PCI/NVMe: EP support disabled; no NVMe BAR reset */

static inline struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no)
{
	return NULL;	/* PCI/NVMe: EP support disabled; no NVMe EP function */
}
#endif

#ifdef CONFIG_PCIE_DW_DEBUGFS
void dwc_pcie_debugfs_init(struct dw_pcie *pci, enum dw_pcie_device_mode mode);	/* PCI/NVMe: create debugfs entries for NVMe link/registers */
void dwc_pcie_debugfs_deinit(struct dw_pcie *pci);	/* PCI/NVMe: remove debugfs entries when NVMe controller goes away */
#else
static inline void dwc_pcie_debugfs_init(struct dw_pcie *pci,
					 enum dw_pcie_device_mode mode)
{
}	/* PCI/NVMe: debugfs disabled; no NVMe diagnostics directory */
static inline void dwc_pcie_debugfs_deinit(struct dw_pcie *pci)
{
}	/* PCI/NVMe: debugfs disabled; no NVMe diagnostics to remove */
#endif

#endif /* _PCIE_DESIGNWARE_H */
