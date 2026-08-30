// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm PCIe root complex driver
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * Copyright 2015 Linaro Limited.
 *
 * Author: Stanimir Varbanov <svarbanov@mm-sol.com>
 */

#include <linux/clk.h>          /* PCI/NVMe: clock framework for PCIe/NVMe controller clocks */
#include <linux/crc8.h>         /* PCI/NVMe: CRC8 for BDF-to-SID hash used by IOMMU/SMMU */
#include <linux/debugfs.h>      /* PCI/NVMe: debugfs for exposing ASPM link transition counters */
#include <linux/delay.h>        /* PCI/NVMe: udelay/msleep during link training and PERST timing */
#include <linux/gpio/consumer.h> /* PCI/NVMe: PERST# GPIO control for NVMe endpoint reset */
#include <linux/interconnect.h> /* PCI/NVMe: interconnect bandwidth voting for NVMe DMA traffic */
#include <linux/interrupt.h>    /* PCI/NVMe: MSI/MSI-X/legacy interrupt handling for NVMe queues */
#include <linux/io.h>           /* PCI/NVMe: MMIO read/write to DBI/ELBI/PARF config spaces */
#include <linux/iopoll.h>       /* PCI/NVMe: polled register waits during link bring-up */
#include <linux/kernel.h>       /* PCI/NVMe: core kernel helpers */
#include <linux/limits.h>       /* PCI/NVMe: ULONG_MAX for max OPP frequency search */
#include <linux/init.h>         /* PCI/NVMe: module init macros */
#include <linux/of.h>           /* PCI/NVMe: device-tree parsing for NVMe endpoint ports/PERST */
#include <linux/of_pci.h>       /* PCI/NVMe: OF PCI helpers for bridge/bus setup */
#include <linux/pci.h>          /* PCI/NVMe: core PCI definitions seen by drivers/nvme/host/pci.c */
#include <linux/pci-ecam.h>     /* PCI/NVMe: ECAM ops for firmware-managed root complex */
#include <linux/pci-pwrctrl.h>  /* PCI/NVMe: power control for NVMe endpoint devices */
#include <linux/pm_opp.h>       /* PCI/NVMe: OPP scaling linked to NVMe link speed/bandwidth */
#include <linux/pm_runtime.h>   /* PCI/NVMe: runtime PM for PCIe host during NVMe idle/active */
#include <linux/platform_device.h> /* PCI/NVMe: platform driver binding for Qualcomm PCIe RC */
#include <linux/phy/pcie.h>     /* PCI/NVMe: PCIe PHY mode selection for RC operation */
#include <linux/phy/phy.h>      /* PCI/NVMe: PHY init/power for SerDes lanes to NVMe SSD */
#include <linux/regulator/consumer.h> /* PCI/NVMe: voltage regulators for PCIe/NVMe analog domains */
#include <linux/reset.h>        /* PCI/NVMe: reset assertion for controller bring-up */
#include <linux/slab.h>         /* PCI/NVMe: kmalloc/kzalloc for qcom_pcie and helpers */
#include <linux/types.h>        /* PCI/NVMe: fixed-width types for register values */
#include <linux/units.h>        /* PCI/NVMe: units for bandwidth/OPP calculations */

#include "../../pci.h"          /* PCI/NVMe: internal PCI core headers used during enumeration */
#include "../pci-host-common.h" /* PCI/NVMe: shared host bridge init for NVMe root ports */
#include "pcie-designware.h"    /* PCI/NVMe: DesignWare core that underpins NVMe host binding */
#include "pcie-qcom-common.h"   /* PCI/NVMe: Qualcomm/DesignWare common helpers */

/* PARF registers */
#define PARF_SYS_CTRL				0x00 /* PCI/NVMe: system control; clocks/PM/L1/L23 for NVMe link */
#define PARF_PM_CTRL				0x20 /* PCI/NVMe: PM control; gates entry to L1 used by NVMe ASPM */
#define PARF_PCS_DEEMPH				0x34 /* PCI/NVMe: PCIe PHY de-emphasis tuning for NVMe signal integrity */
#define PARF_PCS_SWING				0x38 /* PCI/NVMe: transmit swing settings for NVMe endpoint link */
#define PARF_PHY_CTRL				0x40 /* PCI/NVMe: PHY power-down and TX termination control */
#define PARF_PHY_REFCLK				0x4c /* PCI/NVMe: reference clock selection for NVMe PHY */
#define PARF_CONFIG_BITS			0x50 /* PCI/NVMe: RX equalization config for reliable NVMe training */
#define PARF_DBI_BASE_ADDR			0x168 /* PCI/NVMe: CPU phys addr of DW DBI; NVMe config space window */
#define PARF_SLV_ADDR_SPACE_SIZE		0x16c /* PCI/NVMe: size of downstream memory window for NVMe BARs/DMA */
#define PARF_MHI_CLOCK_RESET_CTRL		0x174 /* PCI/NVMe: MHI clock/reset bypass; affects power/clock gating */
#define PARF_AXI_MSTR_WR_ADDR_HALT		0x178 /* PCI/NVMe: halt AXI master writes for MSI doorbell ordering */
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2		0x1a8 /* PCI/NVMe: v2 AXI master write halt for MSI/MSI-X ordering */
#define PARF_Q2A_FLUSH				0x1ac /* PCI/NVMe: flush queue to avoid pending NVMe transactions */
#define PARF_LTSSM				0x1b0 /* PCI/NVMe: link training state machine enable for NVMe link up */
#define PARF_SID_OFFSET				0x234 /* PCI/NVMe: stream ID offset for IOMMU context of NVMe DMA */
#define PARF_BDF_TRANSLATE_CFG			0x24c /* PCI/NVMe: BDF-to-SID translation config for SMMU */
#define PARF_DBI_BASE_ADDR_V2			0x350 /* PCI/NVMe: v2 DBI base low for NVMe config access */
#define PARF_DBI_BASE_ADDR_V2_HI		0x354 /* PCI/NVMe: v2 DBI base high for 64-bit NVMe config window */
#define PARF_SLV_ADDR_SPACE_SIZE_V2		0x358 /* PCI/NVMe: v2 slave address size low */
#define PARF_SLV_ADDR_SPACE_SIZE_V2_HI		0x35c /* PCI/NVMe: v2 slave address size high for large NVMe BARs */
#define PARF_NO_SNOOP_OVERRIDE			0x3d4 /* PCI/NVMe: override No-Snoop attribute for NVMe DMA cache coherency */
#define PARF_ATU_BASE_ADDR			0x634 /* PCI/NVMe: ATU (Address Translation Unit) base low for NVMe MMIO/DMA mapping */
#define PARF_ATU_BASE_ADDR_HI			0x638 /* PCI/NVMe: ATU base high for 64-bit NVMe address maps */
#define PARF_DEVICE_TYPE			0x1000 /* PCI/NVMe: configure controller as Root Complex for NVMe enumeration */
#define PARF_BDF_TO_SID_TABLE_N			0x2000 /* PCI/NVMe: BDF-to-SID lookup table for IOMMU/SMMU of NVMe devices */
#define PARF_BDF_TO_SID_CFG			0x2c00 /* PCI/NVMe: BDF-to-SID bypass/enable for SMMU translation */

/* ELBI registers */
#define ELBI_SYS_CTRL				0x04 /* PCI/NVMe: ELBI system control; LTSSM enable on older IPs */

/* DBI registers */
#define AXI_MSTR_RESP_COMP_CTRL0		0x818 /* PCI/NVMe: AXI master remote read completion size for NVMe config/MMIO */
#define AXI_MSTR_RESP_COMP_CTRL1		0x81c /* PCI/NVMe: AXI master response bridge init for NVMe TLP completion */

/* MHI registers */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L2		0xc04 /* PCI/NVMe: debug counter for L2 link state used in NVMe suspend/resume */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L1		0xc0c /* PCI/NVMe: debug counter for L1 ASPM transitions of NVMe link */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L0S		0xc10 /* PCI/NVMe: debug counter for L0s ASPM transitions */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1	0xc84 /* PCI/NVMe: debug counter for L1.1 substate used by NVMe low-power */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2	0xc88 /* PCI/NVMe: debug counter for L1.2 substate; affects NVMe resume latency */

/* PARF_SYS_CTRL register fields */
#define MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN	BIT(29) /* PCI/NVMe: gate PHY powerdown mux during NVMe P2 transitions */
#define MST_WAKEUP_EN				BIT(13) /* PCI/NVMe: enable master wake for NVMe DMA resume from low power */
#define SLV_WAKEUP_EN				BIT(12) /* PCI/NVMe: enable slave wake for NVMe register access resume */
#define MSTR_ACLK_CGC_DIS			BIT(10) /* PCI/NVMe: disable master AXI clock gating to avoid NVMe DMA stalls */
#define SLV_ACLK_CGC_DIS			BIT(9) /* PCI/NVMe: disable slave AXI clock gating for reliable NVMe config access */
#define CORE_CLK_CGC_DIS			BIT(6) /* PCI/NVMe: disable core clock gating during NVMe active transfers */
#define AUX_PWR_DET				BIT(4) /* PCI/NVMe: auxiliary power detect for NVMe ASPM/PM support */
#define L23_CLK_RMV_DIS				BIT(2) /* PCI/NVMe: disallow L2/L3 clock removal to protect NVMe context */
#define L1_CLK_RMV_DIS				BIT(1) /* PCI/NVMe: disallow L1 clock removal to keep NVMe link responsive */

/* PARF_PM_CTRL register fields */
#define REQ_NOT_ENTR_L1				BIT(5) /* PCI/NVMe: block link entry to L1; cleared to allow NVMe ASPM L1 */

/* PARF_PCS_DEEMPH register fields */
#define PCS_DEEMPH_TX_DEEMPH_GEN1(x)		FIELD_PREP(GENMASK(21, 16), x) /* PCI/NVMe: Gen1 TX de-emphasis for NVMe SSD signal integrity */
#define PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(x)	FIELD_PREP(GENMASK(13, 8), x) /* PCI/NVMe: Gen2 3.5dB de-emphasis for NVMe link */
#define PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(x)	FIELD_PREP(GENMASK(5, 0), x) /* PCI/NVMe: Gen2 6dB de-emphasis for NVMe link */

/* PARF_PCS_SWING register fields */
#define PCS_SWING_TX_SWING_FULL(x)		FIELD_PREP(GENMASK(14, 8), x) /* PCI/NVMe: full TX swing for NVMe far-end SSD */
#define PCS_SWING_TX_SWING_LOW(x)		FIELD_PREP(GENMASK(6, 0), x) /* PCI/NVMe: low TX swing for power saving during NVMe idle */

/* PARF_PHY_CTRL register fields */
#define PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK	GENMASK(20, 16) /* PCI/NVMe: mask for lane 0 TX termination offset */
#define PHY_CTRL_PHY_TX0_TERM_OFFSET(x)		FIELD_PREP(PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK, x) /* PCI/NVMe: set lane 0 TX termination for NVMe PHY */
#define PHY_TEST_PWR_DOWN			BIT(0) /* PCI/NVMe: PHY test power-down; cleared to release NVMe PHY */

/* PARF_PHY_REFCLK register fields */
#define PHY_REFCLK_SSP_EN			BIT(16) /* PCI/NVMe: enable spread-spectrum clocking for NVMe PHY */
#define PHY_REFCLK_USE_PAD			BIT(12) /* PCI/NVMe: select pad reference clock source for NVMe PHY */

/* PARF_CONFIG_BITS register fields */
#define PHY_RX0_EQ(x)				FIELD_PREP(GENMASK(26, 24), x) /* PCI/NVMe: lane 0 RX equalization for NVMe training robustness */

/* PARF_SLV_ADDR_SPACE_SIZE register value */
#define SLV_ADDR_SPACE_SZ			0x80000000 /* PCI/NVMe: 2GB downstream address space for NVMe BAR/DMA windows */

/* PARF_MHI_CLOCK_RESET_CTRL register fields */
#define AHB_CLK_EN				BIT(0) /* PCI/NVMe: enable AHB clock for MHI/DBI register access */
#define MSTR_AXI_CLK_EN				BIT(1) /* PCI/NVMe: enable AXI master clock for NVMe DMA */
#define BYPASS					BIT(4) /* PCI/NVMe: bypass MHI reset/clock gating for stable NVMe operation */

/* PARF_AXI_MSTR_WR_ADDR_HALT register fields */
#define EN					BIT(31) /* PCI/NVMe: enable AXI master write halt for MSI ordering */

/* PARF_LTSSM register fields */
#define LTSSM_EN				BIT(8) /* PCI/NVMe: start LTSSM to train link toward NVMe endpoint */

/* PARF_NO_SNOOP_OVERRIDE register fields */
#define WR_NO_SNOOP_OVERRIDE_EN			BIT(1) /* PCI/NVMe: force writes snooped for NVMe DMA coherency */
#define RD_NO_SNOOP_OVERRIDE_EN			BIT(3) /* PCI/NVMe: force reads snooped for NVMe DMA coherency */

/* PARF_DEVICE_TYPE register fields */
#define DEVICE_TYPE_RC				0x4 /* PCI/NVMe: device type Root Complex for NVMe host enumeration */

/* PARF_BDF_TO_SID_CFG fields */
#define BDF_TO_SID_BYPASS			BIT(0) /* PCI/NVMe: bypass BDF-to-SID translation; clear when IOMMU used for NVMe */

/* ELBI_SYS_CTRL register fields */
#define ELBI_SYS_CTRL_LT_ENABLE			BIT(0) /* PCI/NVMe: enable link training via ELBI on legacy Qcom IPs */

/* AXI_MSTR_RESP_COMP_CTRL0 register fields */
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K	0x4 /* PCI/NVMe: limit remote read bridge to 2KB; affects NVMe config/MMIO reads */
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_4K	0x5 /* PCI/NVMe: 4KB remote read bridge option for NVMe TLPs */

/* AXI_MSTR_RESP_COMP_CTRL1 register fields */
#define CFG_BRIDGE_SB_INIT			BIT(0) /* PCI/NVMe: initialize response bridge sideband for NVMe completions */

/* PCI_EXP_SLTCAP register fields */
#define PCIE_CAP_SLOT_POWER_LIMIT_VAL		FIELD_PREP(PCI_EXP_SLTCAP_SPLV, 250) /* PCI/NVMe: 250x power limit for NVMe slot */
#define PCIE_CAP_SLOT_POWER_LIMIT_SCALE		FIELD_PREP(PCI_EXP_SLTCAP_SPLS, 1) /* PCI/NVMe: 0.1x scale for NVMe slot power limit */
/* [한국어] NVMe SSD 핫플러그를 지원하기 위한 Slot Capabilities 비트 묶음.
 * 주의: 아래 각 줄 끝의 백슬래시는 매크로 연속을 뜻하며 **줄의 마지막 문자여야 한다.**
 * 뒤에 주석이나 공백이 오면 연속이 끊겨 매크로가 첫 줄에서 끝나 버린다. */
#define PCIE_CAP_SLOT_VAL			(PCI_EXP_SLTCAP_ABP | \
						 PCI_EXP_SLTCAP_PCP | \
						 PCI_EXP_SLTCAP_MRLSP | \
						 PCI_EXP_SLTCAP_AIP | \
						 PCI_EXP_SLTCAP_PIP | \
						 PCI_EXP_SLTCAP_HPS | \
						 PCI_EXP_SLTCAP_EIP | \
						 PCIE_CAP_SLOT_POWER_LIMIT_VAL | \
						 PCIE_CAP_SLOT_POWER_LIMIT_SCALE)

#define PERST_DELAY_US				1000 /* PCI/NVMe: 1ms delay after PERST# assert/deassert for NVMe reset timing */

#define QCOM_PCIE_CRC8_POLYNOMIAL		(BIT(2) | BIT(1) | BIT(0)) /* PCI/NVMe: CRC8 poly (x^8+x^2+x+1) for BDF-to-SID hash used by IOMMU for NVMe */

#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
		Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed))) /* PCI/NVMe: convert PCIe speed to interconnect bw for NVMe DMA */

struct qcom_pcie_resources_1_0_0 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks required before NVMe link training */
	int num_clks;				/* PCI/NVMe: number of clocks in clks array */
	struct reset_control *core;		/* PCI/NVMe: core reset for controller bring-up */
	struct regulator *vdda;			/* PCI/NVMe: analog supply for NVMe PHY and controller */
};

#define QCOM_PCIE_2_1_0_MAX_RESETS		6 /* PCI/NVMe: maximum resets for v2.1.0 IP before NVMe enumeration */
#define QCOM_PCIE_2_1_0_MAX_SUPPLY		3 /* PCI/NVMe: maximum regulators for v2.1.0 IP */
struct qcom_pcie_resources_2_1_0 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.1.0 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct reset_control_bulk_data resets[QCOM_PCIE_2_1_0_MAX_RESETS]; /* PCI/NVMe: reset array for v2.1.0 */
	int num_resets;				/* PCI/NVMe: active number of resets */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_1_0_MAX_SUPPLY]; /* PCI/NVMe: regulator array for NVMe power */
};

#define QCOM_PCIE_2_3_2_MAX_SUPPLY		2 /* PCI/NVMe: maximum regulators for v2.3.2 IP */
struct qcom_pcie_resources_2_3_2 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.3.2 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_3_2_MAX_SUPPLY]; /* PCI/NVMe: regulators for v2.3.2 */
};

#define QCOM_PCIE_2_3_3_MAX_RESETS		7 /* PCI/NVMe: maximum resets for v2.3.3 IP */
struct qcom_pcie_resources_2_3_3 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.3.3 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct reset_control_bulk_data rst[QCOM_PCIE_2_3_3_MAX_RESETS]; /* PCI/NVMe: reset array for v2.3.3 */
};

#define QCOM_PCIE_2_4_0_MAX_RESETS		12 /* PCI/NVMe: maximum resets for v2.4.0 IP */
struct qcom_pcie_resources_2_4_0 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.4.0 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct reset_control_bulk_data resets[QCOM_PCIE_2_4_0_MAX_RESETS]; /* PCI/NVMe: reset array for v2.4.0 */
	int num_resets;				/* PCI/NVMe: active number of resets */
};

#define QCOM_PCIE_2_7_0_MAX_SUPPLIES		2 /* PCI/NVMe: maximum regulators for v2.7.0 IP */
struct qcom_pcie_resources_2_7_0 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.7.0 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct regulator_bulk_data supplies[QCOM_PCIE_2_7_0_MAX_SUPPLIES]; /* PCI/NVMe: regulators for v2.7.0 */
	struct reset_control *rst;		/* PCI/NVMe: aggregated reset control for v2.7.0 */
};

struct qcom_pcie_resources_2_9_0 {
	struct clk_bulk_data *clks;		/* PCI/NVMe: controller clocks for v2.9.0 */
	int num_clks;				/* PCI/NVMe: number of clocks */
	struct reset_control *rst;		/* PCI/NVMe: aggregated reset control for v2.9.0 */
};

union qcom_pcie_resources {
	struct qcom_pcie_resources_1_0_0 v1_0_0; /* PCI/NVMe: resources for v1.0.0 */
	struct qcom_pcie_resources_2_1_0 v2_1_0; /* PCI/NVMe: resources for v2.1.0 */
	struct qcom_pcie_resources_2_3_2 v2_3_2; /* PCI/NVMe: resources for v2.3.2 */
	struct qcom_pcie_resources_2_3_3 v2_3_3; /* PCI/NVMe: resources for v2.3.3 */
	struct qcom_pcie_resources_2_4_0 v2_4_0; /* PCI/NVMe: resources for v2.4.0 */
	struct qcom_pcie_resources_2_7_0 v2_7_0; /* PCI/NVMe: resources for v2.7.0 */
	struct qcom_pcie_resources_2_9_0 v2_9_0; /* PCI/NVMe: resources for v2.9.0 */
};

struct qcom_pcie;

struct qcom_pcie_ops {
	int (*get_resources)(struct qcom_pcie *pcie); /* PCI/NVMe: parse clocks/resets/regulators for NVMe RC */
	int (*init)(struct qcom_pcie *pcie);	      /* PCI/NVMe: power/reset/clocks before NVMe link training */
	int (*post_init)(struct qcom_pcie *pcie);     /* PCI/NVMe: PHY/DBI/ATU config after NVMe PERST deassert */
	void (*host_post_init)(struct qcom_pcie *pcie); /* PCI/NVMe: late host init, e.g. ASPM enable for NVMe devices */
	void (*deinit)(struct qcom_pcie *pcie);       /* PCI/NVMe: power down NVMe RC and release resources */
	void (*ltssm_enable)(struct qcom_pcie *pcie); /* PCI/NVMe: start link training for NVMe endpoint */
	int (*config_sid)(struct qcom_pcie *pcie);    /* PCI/NVMe: configure BDF-to-SID for IOMMU/SMMU of NVMe DMA */
};

 /**
  * struct qcom_pcie_cfg - Per SoC config struct
  * @ops: qcom PCIe ops structure
  * @override_no_snoop: Override NO_SNOOP attribute in TLP to enable cache
  * snooping
  * @firmware_managed: Set if the Root Complex is firmware managed
  */
struct qcom_pcie_cfg {
	const struct qcom_pcie_ops *ops;	/* PCI/NVMe: SoC-specific operation table for NVMe RC */
	bool override_no_snoop;			/* PCI/NVMe: force snoop for NVMe DMA coherency when true */
	bool firmware_managed;			/* PCI/NVMe: firmware owns link training; kernel uses ECAM for NVMe */
	bool no_l0s;				/* PCI/NVMe: disable L0s ASPM to avoid NVMe latency/bugs */
};

struct qcom_pcie_perst {
	struct list_head list;			/* PCI/NVMe: list node for PERST# descriptors */
	struct gpio_desc *desc;			/* PCI/NVMe: PERST# GPIO descriptor to reset NVMe endpoint */
};

struct qcom_pcie_port {
	struct list_head list;			/* PCI/NVMe: list node for PCIe ports */
	struct phy *phy;			/* PCI/NVMe: PHY for this NVMe root port lane(s) */
	struct list_head perst;			/* PCI/NVMe: PERST# GPIO list for this port */
};

struct qcom_pcie {
	struct dw_pcie *pci;			/* PCI/NVMe: DesignWare PCIe core wrapping NVMe host controller */
	void __iomem *parf;			/* DT parf */ /* PCI/NVMe: PARF register base for Qualcomm-specific config */
	void __iomem *mhi;			/* PCI/NVMe: MHI/debug register base for ASPM counters */
	union qcom_pcie_resources res;		/* PCI/NVMe: SoC-specific power/clock/reset resources */
	struct icc_path *icc_mem;		/* PCI/NVMe: interconnect path for NVMe DMA memory traffic */
	struct icc_path *icc_cpu;		/* PCI/NVMe: interconnect path for CPU config/MMIO access to NVMe */
	const struct qcom_pcie_cfg *cfg;	/* PCI/NVMe: pointer to SoC config for NVMe RC */
	struct dentry *debugfs;			/* PCI/NVMe: debugfs directory for link transition counters */
	struct list_head ports;			/* PCI/NVMe: list of PCIe ports/PERSTs/PHYs */
	bool suspended;				/* PCI/NVMe: true when RC was powered off during NVMe system suspend */
	bool use_pm_opp;			/* PCI/NVMe: true when OPP used instead of ICC for NVMe bandwidth */
};

#define to_qcom_pcie(x)		dev_get_drvdata((x)->dev) /* PCI/NVMe: retrieve qcom_pcie from dw_pcie for NVMe host */

static void __qcom_pcie_perst_assert(struct qcom_pcie *pcie, bool assert)
{
	struct qcom_pcie_perst *perst;		/* PCI/NVMe: iterator over PERST# entries */
	struct qcom_pcie_port *port;		/* PCI/NVMe: iterator over PCIe ports */
	int val = assert ? 1 : 0;		/* PCI/NVMe: GPIO value to assert (1) or deassert (0) PERST# */

	list_for_each_entry(port, &pcie->ports, list) { /* PCI/NVMe: walk every NVMe root port */
		list_for_each_entry(perst, &port->perst, list) /* PCI/NVMe: walk every PERST# of the port */
			gpiod_set_value_cansleep(perst->desc, val); /* PCI/NVMe: drive PERST# to reset/release NVMe SSD */
	}

	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500); /* PCI/NVMe: wait for NVMe endpoint to sample PERST# */
}

static void qcom_pcie_perst_assert(struct qcom_pcie *pcie)
{
	__qcom_pcie_perst_assert(pcie, true); /* PCI/NVMe: assert PERST# to hold NVMe endpoint in reset */
}

static void qcom_pcie_perst_deassert(struct qcom_pcie *pcie)
{
	/* Ensure that PERST# has been asserted for at least 100 ms */
	msleep(PCIE_T_PVPERL_MS); /* PCI/NVMe: meet PCIe PERST# active time before releasing NVMe SSD */
	__qcom_pcie_perst_assert(pcie, false); /* PCI/NVMe: deassert PERST# to start NVMe link training */
}

static int qcom_pcie_start_link(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* PCI/NVMe: get Qualcomm wrapper around DW core */

	qcom_pcie_common_set_equalization(pci); /* PCI/NVMe: configure equalization for NVMe link stability */

	if (pcie_get_link_speed(pci->max_link_speed) == PCIE_SPEED_16_0GT) /* PCI/NVMe: check if Gen4 NVMe SSD requested */
		qcom_pcie_common_set_16gt_lane_margining(pci); /* PCI/NVMe: enable 16GT lane margining for Gen4 NVMe */

	/* Enable Link Training state machine */
	if (pcie->cfg->ops->ltssm_enable) /* PCI/NVMe: only start LTSSM if SoC provides the callback */
		pcie->cfg->ops->ltssm_enable(pcie); /* PCI/NVMe: start LTSSM to bring NVMe link to L0 */

	return 0; /* PCI/NVMe: link training initiated; actual up state checked later by NVMe host */
}

static void qcom_pcie_clear_aspm_l0s(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* PCI/NVMe: get Qualcomm wrapper */
	u16 offset;				/* PCI/NVMe: PCIe capability offset in NVMe root port config */
	u32 val;				/* PCI/NVMe: register value for ASPM L0S modification */

	if (!pcie->cfg->no_l0s) /* PCI/NVMe: skip if this SoC allows L0s for NVMe ASPM */
		return;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: locate PCIe capability block of NVMe RP */

	dw_pcie_dbi_ro_wr_en(pci); /* PCI/NVMe: enable write to read-only DBI for NVMe RP ASPM caps */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: read Link Capabilities of NVMe RP */
	val &= ~PCI_EXP_LNKCAP_ASPM_L0S; /* PCI/NVMe: clear L0s support to prevent NVMe ASPM L0s negotiation */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: write back modified Link Capabilities */

	dw_pcie_dbi_ro_wr_dis(pci); /* PCI/NVMe: restore DBI read-only protection */
}

static void qcom_pcie_set_slot_nccs(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: PCIe cap offset for NVMe RP slot regs */
	u32 val;				/* PCI/NVMe: slot capabilities value with NCCS set */

	dw_pcie_dbi_ro_wr_en(pci); /* PCI/NVMe: allow RO slot cap write for NVMe hot-plug tuning */

	/*
	 * Qcom PCIe Root Ports do not support generating command completion
	 * notifications for the Hot-Plug commands. So set the NCCS field to
	 * avoid waiting for the completions.
	 */
	val = readl(pci->dbi_base + offset + PCI_EXP_SLTCAP); /* PCI/NVMe: read Slot Capabilities for NVMe hot-plug */
	val |= PCI_EXP_SLTCAP_NCCS; /* PCI/NVMe: set No Command Complete Support for NVMe hotplug */
	writel(val, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* PCI/NVMe: write updated Slot Capabilities */

	dw_pcie_dbi_ro_wr_dis(pci); /* PCI/NVMe: restore DBI RO protection */
}

static void qcom_pcie_configure_dbi_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DesignWare core used by NVMe host */

	if (pci->dbi_phys_addr) { /* PCI/NVMe: only program if a physical DBI address is known */
		/*
		 * PARF_DBI_BASE_ADDR register is in CPU domain and require to
		 * be programmed with CPU physical address.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR); /* PCI/NVMe: set low 32b of DBI base for NVMe config access */
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
							PARF_SLV_ADDR_SPACE_SIZE); /* PCI/NVMe: set downstream window size for NVMe BARs/DMA */
	}
}

static void qcom_pcie_configure_dbi_atu_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DesignWare core for NVMe host */

	if (pci->dbi_phys_addr) { /* PCI/NVMe: proceed only with valid DBI physical address */
		/*
		 * PARF_DBI_BASE_ADDR_V2 and PARF_ATU_BASE_ADDR registers are
		 * in CPU domain and require to be programmed with CPU
		 * physical addresses.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR_V2); /* PCI/NVMe: v2 DBI base low for NVMe config window */
		writel(upper_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR_V2_HI); /* PCI/NVMe: v2 DBI base high for 64-bit NVMe config */

		if (pci->atu_phys_addr) { /* PCI/NVMe: ATU may be separate from DBI on newer IPs */
			writel(lower_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR); /* PCI/NVMe: ATU base low for NVMe MMIO/DMA translation */
			writel(upper_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR_HI); /* PCI/NVMe: ATU base high for 64-bit NVMe maps */
		}

		writel(0x0, pcie->parf + PARF_SLV_ADDR_SPACE_SIZE_V2); /* PCI/NVMe: clear low slave size */
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
						PARF_SLV_ADDR_SPACE_SIZE_V2_HI); /* PCI/NVMe: set high slave size for large NVMe BARs */
	}
}

static void qcom_pcie_2_1_0_ltssm_enable(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DesignWare core for NVMe host */
	u32 val;				/* PCI/NVMe: ELBI sys ctrl value with LT enable */

	if (!pci->elbi_base) { /* PCI/NVMe: ELBI only present on older Qcom IPs for NVMe link training */
		dev_err(pci->dev, "ELBI is not present\n"); /* PCI/NVMe: error if ELBI missing */
		return;
	}
	/* enable link training */
	val = readl(pci->elbi_base + ELBI_SYS_CTRL); /* PCI/NVMe: read ELBI system control */
	val |= ELBI_SYS_CTRL_LT_ENABLE; /* PCI/NVMe: set LT enable bit to train NVMe link */
	writel(val, pci->elbi_base + ELBI_SYS_CTRL); /* PCI/NVMe: start link training for NVMe endpoint */
}

static int qcom_pcie_get_resources_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* PCI/NVMe: v2.1.0 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device used for devm allocations */
	bool is_apq = of_device_is_compatible(dev->of_node, "qcom,pcie-apq8064"); /* PCI/NVMe: APQ variant has optional ext reset */
	int ret;				/* PCI/NVMe: return value from resource get */

	res->supplies[0].supply = "vdda"; /* PCI/NVMe: analog supply for NVMe controller */
	res->supplies[1].supply = "vdda_phy"; /* PCI/NVMe: PHY analog supply for NVMe SerDes */
	res->supplies[2].supply = "vdda_refclk"; /* PCI/NVMe: refclk supply for NVMe PHY clock */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* PCI/NVMe: request all regulators for NVMe RC */
	if (ret) /* PCI/NVMe: fail probe if regulators unavailable */
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe link bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->resets[0].id = "pci"; /* PCI/NVMe: PCI core reset for NVMe controller */
	res->resets[1].id = "axi"; /* PCI/NVMe: AXI reset for NVMe DMA master/slave */
	res->resets[2].id = "ahb"; /* PCI/NVMe: AHB reset for NVMe register interface */
	res->resets[3].id = "por"; /* PCI/NVMe: power-on reset for NVMe RC */
	res->resets[4].id = "phy"; /* PCI/NVMe: PHY reset for NVMe SerDes lanes */
	res->resets[5].id = "ext"; /* PCI/NVMe: external reset (optional on APQ8016) */

	/* ext is optional on APQ8016 */
	res->num_resets = is_apq ? 5 : 6; /* PCI/NVMe: APQ8016 omits ext reset for NVMe */
	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets); /* PCI/NVMe: acquire resets for NVMe RC */
	if (ret < 0) /* PCI/NVMe: reset acquisition failure prevents NVMe probe */
		return ret;

	return 0; /* PCI/NVMe: v2.1.0 resources acquired for NVMe host */
}

static void qcom_pcie_deinit_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* PCI/NVMe: v2.1.0 resources */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: stop clocks to NVMe controller */
	reset_control_bulk_assert(res->num_resets, res->resets); /* PCI/NVMe: assert resets to reset NVMe RC */

	writel(1, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: power down PHY after NVMe link down */

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: disable NVMe RC regulators */
}

static int qcom_pcie_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* PCI/NVMe: v2.1.0 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	/* reset the PCIe interface as uboot can leave it undefined state */
	ret = reset_control_bulk_assert(res->num_resets, res->resets); /* PCI/NVMe: assert all resets to clean NVMe RC state */
	if (ret < 0) { /* PCI/NVMe: reset assert failure aborts NVMe bring-up */
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: enable regulators for NVMe RC/PHY */
	if (ret < 0) { /* PCI/NVMe: regulator failure aborts NVMe bring-up */
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = reset_control_bulk_deassert(res->num_resets, res->resets); /* PCI/NVMe: release resets to start NVMe RC */
	if (ret < 0) { /* PCI/NVMe: deassert failure rolls back regulator enable */
		dev_err(dev, "cannot deassert resets\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
		return ret;
	}

	return 0; /* PCI/NVMe: v2.1.0 controller clocks still off; PHY enabled later */
}

static int qcom_pcie_post_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0; /* PCI/NVMe: v2.1.0 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for DT compat checks */
	struct device_node *node = dev->of_node; /* PCI/NVMe: DT node for IPQ compat checks */
	u32 val;				/* PCI/NVMe: scratch register value */
	int ret;				/* PCI/NVMe: result of post-init steps */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: read PHY control before releasing PHY from powerdown */
	val &= ~PHY_TEST_PWR_DOWN; /* PCI/NVMe: clear test powerdown to release NVMe PHY */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: release NVMe PHY */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe link */
	if (ret) /* PCI/NVMe: clock failure prevents NVMe link training */
		return ret;

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064") ||
	    of_device_is_compatible(node, "qcom,pcie-ipq8064-v2")) { /* PCI/NVMe: IPQ8064-specific PHY tuning for NVMe SSDs */
		writel(PCS_DEEMPH_TX_DEEMPH_GEN1(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(34),
		       pcie->parf + PARF_PCS_DEEMPH); /* PCI/NVMe: set TX de-emphasis for Gen1/Gen2 NVMe links */
		writel(PCS_SWING_TX_SWING_FULL(120) |
			       PCS_SWING_TX_SWING_LOW(120),
		       pcie->parf + PARF_PCS_SWING); /* PCI/NVMe: set TX swing for IPQ8064 NVMe links */
		writel(PHY_RX0_EQ(4), pcie->parf + PARF_CONFIG_BITS); /* PCI/NVMe: set RX equalization for IPQ8064 NVMe */
	}

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064")) { /* PCI/NVMe: IPQ8064-specific TX termination */
		/* set TX termination offset */
		val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: re-read PHY control */
		val &= ~PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK; /* PCI/NVMe: clear lane 0 TX termination offset */
		val |= PHY_CTRL_PHY_TX0_TERM_OFFSET(7); /* PCI/NVMe: set offset to 7 for IPQ8064 NVMe PHY */
		writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: apply TX termination offset */
	}

	/* enable external reference clock */
	val = readl(pcie->parf + PARF_PHY_REFCLK); /* PCI/NVMe: read refclk config for NVMe PHY */
	/* USE_PAD is required only for ipq806x */
	if (!of_device_is_compatible(node, "qcom,pcie-apq8064")) /* PCI/NVMe: non-APQ uses internal refclk for NVMe PHY */
		val &= ~PHY_REFCLK_USE_PAD;
	val |= PHY_REFCLK_SSP_EN; /* PCI/NVMe: enable spread-spectrum clocking for NVMe PHY */
	writel(val, pcie->parf + PARF_PHY_REFCLK); /* PCI/NVMe: apply refclk settings */

	/* wait for clock acquisition */
	usleep_range(1000, 1500); /* PCI/NVMe: allow NVMe PHY refclk to stabilize */

	/* Set the Max TLP size to 2K, instead of using default of 4K */
	writel(CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL0); /* PCI/NVMe: limit read completion bridge to 2KB for NVMe TLPs */
	writel(CFG_BRIDGE_SB_INIT,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL1); /* PCI/NVMe: initialize response bridge sideband for NVMe */

	qcom_pcie_set_slot_nccs(pcie->pci); /* PCI/NVMe: set NCCS for NVMe hot-plug slot */

	return 0; /* PCI/NVMe: v2.1.0 post-init complete; link training follows PERST deassert */
}

static int qcom_pcie_get_resources_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* PCI/NVMe: v1.0.0 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm resource allocation */

	res->vdda = devm_regulator_get(dev, "vdda"); /* PCI/NVMe: get analog regulator for NVMe RC */
	if (IS_ERR(res->vdda)) /* PCI/NVMe: regulator failure blocks NVMe probe */
		return PTR_ERR(res->vdda);

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: get all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe link bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->core = devm_reset_control_get_exclusive(dev, "core"); /* PCI/NVMe: get core reset for NVMe RC */
	return PTR_ERR_OR_ZERO(res->core); /* PCI/NVMe: return reset error or success */
}

static void qcom_pcie_deinit_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* PCI/NVMe: v1.0.0 resources */

	reset_control_assert(res->core); /* PCI/NVMe: assert core reset to reset NVMe RC */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable clocks to NVMe controller */
	regulator_disable(res->vdda); /* PCI/NVMe: disable analog supply for NVMe RC */
}

static int qcom_pcie_init_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0; /* PCI/NVMe: v1.0.0 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = reset_control_deassert(res->core); /* PCI/NVMe: release core reset to start NVMe RC */
	if (ret) { /* PCI/NVMe: reset release failure aborts NVMe bring-up */
		dev_err(dev, "cannot deassert core reset\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe */
	if (ret) { /* PCI/NVMe: clock failure triggers reset rollback */
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_assert_reset;
	}

	ret = regulator_enable(res->vdda); /* PCI/NVMe: enable analog regulator for NVMe RC/PHY */
	if (ret) { /* PCI/NVMe: regulator failure triggers clock rollback */
		dev_err(dev, "cannot enable vdda regulator\n");
		goto err_disable_clks;
	}

	return 0; /* PCI/NVMe: v1.0.0 init complete */

err_disable_clks:
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: roll back clocks on regulator failure */
err_assert_reset:
	reset_control_assert(res->core); /* PCI/NVMe: roll back reset on clock failure */

	return ret; /* PCI/NVMe: return original init error */
}

static int qcom_pcie_post_init_1_0_0(struct qcom_pcie *pcie)
{
	qcom_pcie_configure_dbi_base(pcie); /* PCI/NVMe: program DBI/slave window for NVMe config and BARs */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* PCI/NVMe: only configure MSI halt if MSI support is built in */
		u32 val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* PCI/NVMe: read AXI master write halt register */

		val |= EN; /* PCI/NVMe: enable write halt to order MSI doorbells from NVMe host */
		writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT); /* PCI/NVMe: apply MSI write ordering setting */
	}

	qcom_pcie_set_slot_nccs(pcie->pci); /* PCI/NVMe: set NCCS for NVMe hot-plug on v1.0.0 */

	return 0; /* PCI/NVMe: v1.0.0 post-init complete */
}

static void qcom_pcie_2_3_2_ltssm_enable(struct qcom_pcie *pcie)
{
	u32 val;				/* PCI/NVMe: LTSSM register value */

	/* enable link training */
	val = readl(pcie->parf + PARF_LTSSM); /* PCI/NVMe: read LTSSM register */
	val |= LTSSM_EN; /* PCI/NVMe: set LTSSM enable bit for NVMe link training */
	writel(val, pcie->parf + PARF_LTSSM); /* PCI/NVMe: start NVMe link training */
}

static int qcom_pcie_get_resources_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* PCI/NVMe: v2.3.2 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm allocations */
	int ret;				/* PCI/NVMe: result of resource get */

	res->supplies[0].supply = "vdda"; /* PCI/NVMe: analog supply for NVMe RC */
	res->supplies[1].supply = "vddpe-3v3"; /* PCI/NVMe: 3.3V supply for NVMe endpoint power */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* PCI/NVMe: request regulators for NVMe RC/SSD */
	if (ret) /* PCI/NVMe: regulator failure blocks NVMe probe */
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	return 0; /* PCI/NVMe: v2.3.2 resources acquired */
}

static void qcom_pcie_deinit_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* PCI/NVMe: v2.3.2 resources */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable controller clocks */
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: disable NVMe RC/SSD regulators */
}

static int qcom_pcie_init_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2; /* PCI/NVMe: v2.3.2 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: enable regulators for NVMe */
	if (ret < 0) { /* PCI/NVMe: regulator failure aborts NVMe bring-up */
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe */
	if (ret) { /* PCI/NVMe: clock failure rolls back regulators */
		dev_err(dev, "cannot prepare/enable clocks\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
		return ret;
	}

	return 0; /* PCI/NVMe: v2.3.2 init complete */
}

static int qcom_pcie_post_init_2_3_2(struct qcom_pcie *pcie)
{
	u32 val;				/* PCI/NVMe: scratch register value */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: read PHY control */
	val &= ~PHY_TEST_PWR_DOWN; /* PCI/NVMe: release NVMe PHY from test powerdown */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: apply PHY release */

	qcom_pcie_configure_dbi_base(pcie); /* PCI/NVMe: program DBI base for NVMe config access */

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: read system control */
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN; /* PCI/NVMe: disable PHY powerdown mux for stable NVMe P2 */
	writel(val, pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: apply system control */

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* PCI/NVMe: read MHI clock/reset control */
	val |= BYPASS; /* PCI/NVMe: bypass MHI reset/clock gating for NVMe host operation */
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* PCI/NVMe: apply MHI bypass */

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* PCI/NVMe: read v2 AXI master write halt */
	val |= EN; /* PCI/NVMe: enable write halt for MSI/MSI-X ordering */
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* PCI/NVMe: apply MSI write ordering */

	qcom_pcie_set_slot_nccs(pcie->pci); /* PCI/NVMe: set NCCS for NVMe hot-plug on v2.3.2 */

	return 0; /* PCI/NVMe: v2.3.2 post-init complete */
}

static int qcom_pcie_get_resources_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* PCI/NVMe: v2.4.0 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm allocations */
	bool is_ipq = of_device_is_compatible(dev->of_node, "qcom,pcie-ipq4019"); /* PCI/NVMe: IPQ4019 uses all 12 resets */
	int ret;				/* PCI/NVMe: result of resource get */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->resets[0].id = "axi_m"; /* PCI/NVMe: AXI master reset for NVMe DMA */
	res->resets[1].id = "axi_s"; /* PCI/NVMe: AXI slave reset for NVMe config access */
	res->resets[2].id = "axi_m_sticky"; /* PCI/NVMe: AXI master sticky reset */
	res->resets[3].id = "pipe_sticky"; /* PCI/NVMe: PIPE sticky reset */
	res->resets[4].id = "pwr"; /* PCI/NVMe: power domain reset */
	res->resets[5].id = "ahb"; /* PCI/NVMe: AHB reset for NVMe register interface */
	res->resets[6].id = "pipe"; /* PCI/NVMe: PIPE reset for NVMe PHY interface */
	res->resets[7].id = "axi_m_vmid"; /* PCI/NVMe: VMID reset for AXI master */
	res->resets[8].id = "axi_s_xpu"; /* PCI/NVMe: XPU reset for AXI slave */
	res->resets[9].id = "parf"; /* PCI/NVMe: PARF reset */
	res->resets[10].id = "phy"; /* PCI/NVMe: PHY reset */
	res->resets[11].id = "phy_ahb"; /* PCI/NVMe: PHY AHB reset */

	res->num_resets = is_ipq ? 12 : 6; /* PCI/NVMe: IPQ4019 uses all resets; others use first 6 */

	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets); /* PCI/NVMe: acquire resets for NVMe RC */
	if (ret < 0) /* PCI/NVMe: reset acquisition failure blocks NVMe probe */
		return ret;

	return 0; /* PCI/NVMe: v2.4.0 resources acquired */
}

static void qcom_pcie_deinit_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* PCI/NVMe: v2.4.0 resources */

	reset_control_bulk_assert(res->num_resets, res->resets); /* PCI/NVMe: assert resets to reset NVMe RC */
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable controller clocks */
}

static int qcom_pcie_init_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0; /* PCI/NVMe: v2.4.0 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = reset_control_bulk_assert(res->num_resets, res->resets); /* PCI/NVMe: assert all resets to clean state */
	if (ret < 0) { /* PCI/NVMe: reset assert failure aborts NVMe bring-up */
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	usleep_range(10000, 12000); /* PCI/NVMe: hold resets for NVMe RC/PHY stabilization */

	ret = reset_control_bulk_deassert(res->num_resets, res->resets); /* PCI/NVMe: release resets to start NVMe RC */
	if (ret < 0) { /* PCI/NVMe: deassert failure aborts NVMe bring-up */
		dev_err(dev, "cannot deassert resets\n");
		return ret;
	}

	usleep_range(10000, 12000); /* PCI/NVMe: wait for NVMe RC out of reset */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe */
	if (ret) { /* PCI/NVMe: clock failure rolls back resets */
		reset_control_bulk_assert(res->num_resets, res->resets);
		return ret;
	}

	return 0; /* PCI/NVMe: v2.4.0 init complete */
}

static int qcom_pcie_get_resources_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* PCI/NVMe: v2.3.3 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm allocations */
	int ret;				/* PCI/NVMe: result of resource get */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->rst[0].id = "axi_m"; /* PCI/NVMe: AXI master reset for NVMe DMA */
	res->rst[1].id = "axi_s"; /* PCI/NVMe: AXI slave reset for NVMe config access */
	res->rst[2].id = "pipe"; /* PCI/NVMe: PIPE reset for NVMe PHY interface */
	res->rst[3].id = "axi_m_sticky"; /* PCI/NVMe: AXI master sticky reset */
	res->rst[4].id = "sticky"; /* PCI/NVMe: sticky reset */
	res->rst[5].id = "ahb"; /* PCI/NVMe: AHB reset */
	res->rst[6].id = "sleep"; /* PCI/NVMe: sleep reset for NVMe low-power domains */

	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(res->rst), res->rst); /* PCI/NVMe: acquire resets for NVMe RC */
	if (ret < 0) /* PCI/NVMe: reset acquisition failure blocks NVMe probe */
		return ret;

	return 0; /* PCI/NVMe: v2.3.3 resources acquired */
}

static void qcom_pcie_deinit_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* PCI/NVMe: v2.3.3 resources */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable controller clocks */
}

static int qcom_pcie_init_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3; /* PCI/NVMe: v2.3.3 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst); /* PCI/NVMe: assert all resets to clean state */
	if (ret < 0) { /* PCI/NVMe: reset assert failure aborts NVMe bring-up */
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	usleep_range(2000, 2500); /* PCI/NVMe: hold resets for NVMe RC/PHY stabilization */

	ret = reset_control_bulk_deassert(ARRAY_SIZE(res->rst), res->rst); /* PCI/NVMe: release resets to start NVMe RC */
	if (ret < 0) { /* PCI/NVMe: deassert failure aborts NVMe bring-up */
		dev_err(dev, "cannot deassert resets\n");
		return ret;
	}

	/*
	 * Don't have a way to see if the reset has completed.
	 * Wait for some time.
	 */
	usleep_range(2000, 2500); /* PCI/NVMe: wait for NVMe RC out of reset */

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe */
	if (ret) { /* PCI/NVMe: clock failure triggers reset rollback */
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_assert_resets;
	}

	return 0; /* PCI/NVMe: v2.3.3 init complete */

err_assert_resets:
	/*
	 * Not checking for failure, will anyway return
	 * the original failure in 'ret'.
	 */
	reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst); /* PCI/NVMe: roll back resets on clock failure */

	return ret; /* PCI/NVMe: return original init error */
}

static int qcom_pcie_post_init_2_3_3(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: PCIe capability offset in NVMe RP */
	u32 val;				/* PCI/NVMe: scratch register value */

	val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: read PHY control */
	val &= ~PHY_TEST_PWR_DOWN; /* PCI/NVMe: release NVMe PHY from test powerdown */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: apply PHY release */

	qcom_pcie_configure_dbi_atu_base(pcie); /* PCI/NVMe: program DBI/ATU bases for NVMe config/MMIO/DMA */

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS
		| SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: enable wakeups and disable clock removal for NVMe stability */
	writel(0, pcie->parf + PARF_Q2A_FLUSH); /* PCI/NVMe: clear queue-to-AXI flush state */

	writel(PCI_COMMAND_MASTER, pci->dbi_base + PCI_COMMAND); /* PCI/NVMe: enable bus mastering so NVMe DMA can occur */

	dw_pcie_dbi_ro_wr_en(pci); /* PCI/NVMe: enable writes to read-only config regs of NVMe RP */

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* PCI/NVMe: program slot caps for NVMe hot-plug/power */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: read Link Capabilities of NVMe RP */
	val &= ~PCI_EXP_LNKCAP_ASPMS; /* PCI/NVMe: clear ASPM support bits; OS will re-enable desired states for NVMe */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: write modified Link Capabilities */

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
		PCI_EXP_DEVCTL2); /* PCI/NVMe: disable completion timeout for NVMe RP */

	dw_pcie_dbi_ro_wr_dis(pci); /* PCI/NVMe: restore DBI read-only protection */

	return 0; /* PCI/NVMe: v2.3.3 post-init complete */
}

static int qcom_pcie_get_resources_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* PCI/NVMe: v2.7.0 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm allocations */
	int ret;				/* PCI/NVMe: result of resource get */

	res->rst = devm_reset_control_array_get_exclusive(dev); /* PCI/NVMe: get aggregated reset array for NVMe RC */
	if (IS_ERR(res->rst)) /* PCI/NVMe: reset failure blocks NVMe probe */
		return PTR_ERR(res->rst);

	res->supplies[0].supply = "vdda"; /* PCI/NVMe: analog supply for NVMe RC */
	res->supplies[1].supply = "vddpe-3v3"; /* PCI/NVMe: 3.3V endpoint supply for NVMe SSD */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies); /* PCI/NVMe: request regulators for NVMe RC/SSD */
	if (ret) /* PCI/NVMe: regulator failure blocks NVMe probe */
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	return 0; /* PCI/NVMe: v2.7.0 resources acquired */
}

static int qcom_pcie_init_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* PCI/NVMe: v2.7.0 resources */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for error messages */
	u32 val;				/* PCI/NVMe: scratch register value */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: enable regulators for NVMe */
	if (ret < 0) { /* PCI/NVMe: regulator failure aborts NVMe bring-up */
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks for NVMe */
	if (ret < 0)
		goto err_disable_regulators; /* PCI/NVMe: clock failure jumps to regulator rollback */

	ret = reset_control_assert(res->rst); /* PCI/NVMe: assert aggregated reset to clean NVMe RC */
	if (ret) { /* PCI/NVMe: reset assert failure rolls back clocks/regulators */
		dev_err(dev, "reset assert failed (%d)\n", ret);
		goto err_disable_clocks;
	}

	usleep_range(1000, 1500); /* PCI/NVMe: hold reset for NVMe RC stabilization */

	ret = reset_control_deassert(res->rst); /* PCI/NVMe: release aggregated reset to start NVMe RC */
	if (ret) { /* PCI/NVMe: reset deassert failure rolls back clocks/regulators */
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		goto err_disable_clocks;
	}

	/* Wait for reset to complete, required on SM8450 */
	usleep_range(1000, 1500); /* PCI/NVMe: wait for NVMe RC out of reset on SM8450 */

	/* configure PCIe to RC mode */
	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE); /* PCI/NVMe: set device type to Root Complex for NVMe host */

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: read PHY control */
	val &= ~PHY_TEST_PWR_DOWN; /* PCI/NVMe: release NVMe PHY from test powerdown */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: apply PHY release */

	qcom_pcie_configure_dbi_atu_base(pcie); /* PCI/NVMe: program DBI/ATU bases for NVMe config/MMIO/DMA */

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: read system control */
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN; /* PCI/NVMe: disable PHY powerdown mux for stable NVMe P2 */
	writel(val, pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: apply system control */

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* PCI/NVMe: read MHI clock/reset control */
	val |= BYPASS; /* PCI/NVMe: bypass MHI reset/clock gating for NVMe host */
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* PCI/NVMe: apply MHI bypass */

	/* Enable L1 and L1SS */
	val = readl(pcie->parf + PARF_PM_CTRL); /* PCI/NVMe: read PM control for NVMe ASPM */
	val &= ~REQ_NOT_ENTR_L1; /* PCI/NVMe: allow link entry to L1 for NVMe ASPM */
	writel(val, pcie->parf + PARF_PM_CTRL); /* PCI/NVMe: apply L1 enable */

	pci->l1ss_support = true; /* PCI/NVMe: advertise L1SS support for NVMe low-power substates */

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* PCI/NVMe: read v2 AXI master write halt */
	val |= EN; /* PCI/NVMe: enable write halt for MSI/MSI-X ordering */
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2); /* PCI/NVMe: apply MSI write ordering */

	return 0; /* PCI/NVMe: v2.7.0 init complete */
err_disable_clocks:
	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: roll back clocks on reset failure */
err_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: roll back regulators on clock/reset failure */

	return ret; /* PCI/NVMe: return original init error */
}

static int qcom_pcie_post_init_2_7_0(struct qcom_pcie *pcie)
{
	const struct qcom_pcie_cfg *pcie_cfg = pcie->cfg; /* PCI/NVMe: SoC config for NVMe RC */

	if (pcie_cfg->override_no_snoop) /* PCI/NVMe: some SoCs need forced snoop for NVMe DMA coherency */
		writel(WR_NO_SNOOP_OVERRIDE_EN | RD_NO_SNOOP_OVERRIDE_EN,
				pcie->parf + PARF_NO_SNOOP_OVERRIDE); /* PCI/NVMe: force snoop on read/write TLPs for NVMe DMA */

	qcom_pcie_set_slot_nccs(pcie->pci); /* PCI/NVMe: set NCCS for NVMe hot-plug on v2.7.0 */

	return 0; /* PCI/NVMe: v2.7.0 post-init complete */
}

static int qcom_pcie_enable_aspm(struct pci_dev *pdev, void *userdata)
{
	/*
	 * Downstream devices need to be in D0 state before enabling PCI PM
	 * substates.
	 */
	pci_set_power_state_locked(pdev, PCI_D0); /* PCI/NVMe: ensure NVMe device is in D0 before ASPM enable */
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL); /* PCI/NVMe: enable all ASPM states for NVMe endpoint */

	return 0; /* PCI/NVMe: continue walking NVMe PCIe bus */
}

static void qcom_pcie_host_post_init_2_7_0(struct qcom_pcie *pcie)
{
	struct dw_pcie_rp *pp = &pcie->pci->pp; /* PCI/NVMe: DesignWare root port struct for NVMe host */

	pci_walk_bus(pp->bridge->bus, qcom_pcie_enable_aspm, NULL); /* PCI/NVMe: enable ASPM on all NVMe devices after enumeration */
}

static void qcom_pcie_deinit_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0; /* PCI/NVMe: v2.7.0 resources */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable controller clocks */

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies); /* PCI/NVMe: disable NVMe RC/SSD regulators */
}

static int qcom_pcie_config_sid_1_9_0(struct qcom_pcie *pcie)
{
	/* iommu map structure */
	struct {
		u32 bdf;			/* PCI/NVMe: bus/dev/func of NVMe endpoint for SMMU lookup */
		u32 phandle;			/* PCI/NVMe: phandle to SMMU/IOMMU for NVMe device */
		u32 smmu_sid;			/* PCI/NVMe: stream ID assigned to NVMe device */
		u32 smmu_sid_len;		/* PCI/NVMe: number of consecutive SIDs for NVMe */
	} *map;					/* PCI/NVMe: parsed iommu-map entries for NVMe devices */
	void __iomem *bdf_to_sid_base = pcie->parf + PARF_BDF_TO_SID_TABLE_N; /* PCI/NVMe: base of BDF-to-SID table for IOMMU */
	struct device *dev = pcie->pci->dev; /* PCI/NVMe: device for DT parsing and errors */
	u8 qcom_pcie_crc8_table[CRC8_TABLE_SIZE]; /* PCI/NVMe: CRC8 lookup table for BDF hashing */
	int i, nr_map, size = 0;		/* PCI/NVMe: loop index, map count, iommu-map byte size */
	u32 smmu_sid_base;			/* PCI/NVMe: base stream ID from first iommu-map entry */
	u32 val;				/* PCI/NVMe: BDF-to-SID register value */

	of_get_property(dev->of_node, "iommu-map", &size); /* PCI/NVMe: read iommu-map size for NVMe SMMU config */
	if (!size) /* PCI/NVMe: no iommu-map means no SMMU translation needed for NVMe */
		return 0;

	/* Enable BDF to SID translation by disabling bypass mode (default) */
	val = readl(pcie->parf + PARF_BDF_TO_SID_CFG); /* PCI/NVMe: read BDF-to-SID config */
	val &= ~BDF_TO_SID_BYPASS; /* PCI/NVMe: enable BDF-to-SID translation for NVMe IOMMU isolation */
	writel(val, pcie->parf + PARF_BDF_TO_SID_CFG); /* PCI/NVMe: apply BDF-to-SID enable */

	map = kzalloc(size, GFP_KERNEL); /* PCI/NVMe: allocate parsed iommu-map for NVMe SMMU */
	if (!map) /* PCI/NVMe: allocation failure blocks NVMe IOMMU setup */
		return -ENOMEM;

	of_property_read_u32_array(dev->of_node, "iommu-map", (u32 *)map,
				   size / sizeof(u32)); /* PCI/NVMe: parse iommu-map for NVMe BDF/SID entries */

	nr_map = size / (sizeof(*map)); /* PCI/NVMe: number of BDF-to-SID mappings for NVMe */

	crc8_populate_msb(qcom_pcie_crc8_table, QCOM_PCIE_CRC8_POLYNOMIAL); /* PCI/NVMe: populate CRC8 table for BDF hash */

	/* Registers need to be zero out first */
	memset_io(bdf_to_sid_base, 0, CRC8_TABLE_SIZE * sizeof(u32)); /* PCI/NVMe: clear BDF-to-SID table before programming NVMe entries */

	/* Extract the SMMU SID base from the first entry of iommu-map */
	smmu_sid_base = map[0].smmu_sid; /* PCI/NVMe: use first entry SID as base for NVMe translations */

	/* Look for an available entry to hold the mapping */
	for (i = 0; i < nr_map; i++) { /* PCI/NVMe: iterate over NVMe BDF-to-SID mappings */
		__be16 bdf_be = cpu_to_be16(map[i].bdf); /* PCI/NVMe: BDF in big-endian for CRC8 hash */
		u32 val;			/* PCI/NVMe: current BDF-to-SID register value */
		u8 hash;			/* PCI/NVMe: CRC8 hash indexing BDF-to-SID table */

		hash = crc8(qcom_pcie_crc8_table, (u8 *)&bdf_be, sizeof(bdf_be), 0); /* PCI/NVMe: hash NVMe BDF to table index */

		val = readl(bdf_to_sid_base + hash * sizeof(u32)); /* PCI/NVMe: read current entry for this NVMe BDF hash */

		/* If the register is already populated, look for next available entry */
		while (val) { /* PCI/NVMe: resolve hash collisions for NVMe BDFs */
			u8 current_hash = hash++; /* PCI/NVMe: remember current entry while probing next */
			u8 next_mask = 0xff;	/* PCI/NVMe: lower 8 bits hold next hash pointer */

			/* If NEXT field is NULL then update it with next hash */
			if (!(val & next_mask)) { /* PCI/NVMe: link current entry to next free slot for NVMe BDF */
				val |= (u32)hash;
				writel(val, bdf_to_sid_base + current_hash * sizeof(u32));
			}

			val = readl(bdf_to_sid_base + hash * sizeof(u32)); /* PCI/NVMe: read next candidate entry */
		}

		/* BDF [31:16] | SID [15:8] | NEXT [7:0] */
		val = map[i].bdf << 16 | (map[i].smmu_sid - smmu_sid_base) << 8 | 0; /* PCI/NVMe: encode NVMe BDF and relative SID */
		writel(val, bdf_to_sid_base + hash * sizeof(u32)); /* PCI/NVMe: store NVMe BDF-to-SID mapping in hardware table */
	}

	kfree(map); /* PCI/NVMe: free parsed iommu-map after programming NVMe SMMU table */

	return 0; /* PCI/NVMe: BDF-to-SID configuration complete for NVMe IOMMU */
}

static int qcom_pcie_get_resources_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* PCI/NVMe: v2.9.0 resource bundle */
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for devm allocations */

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks); /* PCI/NVMe: request all clocks for NVMe RC */
	if (res->num_clks < 0) { /* PCI/NVMe: clock failure blocks NVMe bring-up */
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->rst = devm_reset_control_array_get_exclusive(dev); /* PCI/NVMe: get aggregated reset array for NVMe RC */
	if (IS_ERR(res->rst)) /* PCI/NVMe: reset failure blocks NVMe probe */
		return PTR_ERR(res->rst);

	return 0; /* PCI/NVMe: v2.9.0 resources acquired */
}

static void qcom_pcie_deinit_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* PCI/NVMe: v2.9.0 resources */

	clk_bulk_disable_unprepare(res->num_clks, res->clks); /* PCI/NVMe: disable controller clocks */
}

static int qcom_pcie_init_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0; /* PCI/NVMe: v2.9.0 resources */
	struct device *dev = pcie->pci->dev; /* PCI/NVMe: device for error messages */
	int ret;				/* PCI/NVMe: result of init steps */

	ret = reset_control_assert(res->rst); /* PCI/NVMe: assert aggregated reset to clean NVMe RC */
	if (ret) { /* PCI/NVMe: reset assert failure aborts NVMe bring-up */
		dev_err(dev, "reset assert failed (%d)\n", ret);
		return ret;
	}

	/*
	 * Delay periods before and after reset deassert are working values
	 * from downstream Codeaurora kernel
	 */
	usleep_range(2000, 2500); /* PCI/NVMe: hold reset before releasing NVMe RC */

	ret = reset_control_deassert(res->rst); /* PCI/NVMe: release aggregated reset to start NVMe RC */
	if (ret) { /* PCI/NVMe: reset deassert failure aborts NVMe bring-up */
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		return ret;
	}

	usleep_range(2000, 2500); /* PCI/NVMe: wait for NVMe RC out of reset */

	return clk_bulk_prepare_enable(res->num_clks, res->clks); /* PCI/NVMe: enable controller clocks and return result */
}

static int qcom_pcie_post_init_2_9_0(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: PCIe capability offset in NVMe RP */
	u32 val;				/* PCI/NVMe: scratch register value */
	int i;					/* PCI/NVMe: loop index for BDF-to-SID table clear */

	val = readl(pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: read PHY control */
	val &= ~PHY_TEST_PWR_DOWN; /* PCI/NVMe: release NVMe PHY from test powerdown */
	writel(val, pcie->parf + PARF_PHY_CTRL); /* PCI/NVMe: apply PHY release */

	qcom_pcie_configure_dbi_atu_base(pcie); /* PCI/NVMe: program DBI/ATU bases for NVMe config/MMIO/DMA */

	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE); /* PCI/NVMe: set device type to Root Complex for NVMe host */
	writel(BYPASS | MSTR_AXI_CLK_EN | AHB_CLK_EN,
		pcie->parf + PARF_MHI_CLOCK_RESET_CTRL); /* PCI/NVMe: bypass MHI and enable AHB/AXI master clocks for NVMe */
	writel(GEN3_RELATED_OFF_RXEQ_RGRDLESS_RXTS |
		GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL,
		pci->dbi_base + GEN3_RELATED_OFF); /* PCI/NVMe: Gen3 RX equalization and compliance settings for NVMe SSDs */

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS |
		SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL); /* PCI/NVMe: enable wakeups and disable clock removal for NVMe stability */

	writel(0, pcie->parf + PARF_Q2A_FLUSH); /* PCI/NVMe: clear queue-to-AXI flush state */

	dw_pcie_dbi_ro_wr_en(pci); /* PCI/NVMe: enable writes to read-only config regs of NVMe RP */

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP); /* PCI/NVMe: program slot caps for NVMe hot-plug/power */

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: read Link Capabilities of NVMe RP */
	val &= ~PCI_EXP_LNKCAP_ASPMS; /* PCI/NVMe: clear ASPM support bits for later NVMe ASPM enable */
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP); /* PCI/NVMe: write modified Link Capabilities */

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
			PCI_EXP_DEVCTL2); /* PCI/NVMe: disable completion timeout for NVMe RP */

	dw_pcie_dbi_ro_wr_dis(pci); /* PCI/NVMe: restore DBI read-only protection */

	for (i = 0; i < 256; i++) /* PCI/NVMe: clear entire BDF-to-SID hash table for NVMe IOMMU */
		writel(0, pcie->parf + PARF_BDF_TO_SID_TABLE_N + (4 * i)); /* PCI/NVMe: zero one BDF-to-SID entry */

	return 0; /* PCI/NVMe: v2.9.0 post-init complete */
}

static bool qcom_pcie_link_up(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: PCIe cap offset in NVMe RP */
	u16 val = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* PCI/NVMe: read Link Status of NVMe RP */

	return val & PCI_EXP_LNKSTA_DLLLA; /* PCI/NVMe: Data Link Layer Link Active means NVMe link is up */
}

static void qcom_pcie_phy_power_off(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port;		/* PCI/NVMe: iterator over PCIe ports */

	list_for_each_entry(port, &pcie->ports, list) /* PCI/NVMe: walk every NVMe root port */
		phy_power_off(port->phy); /* PCI/NVMe: power down PHY lane for NVMe */
}

static int qcom_pcie_phy_power_on(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port;		/* PCI/NVMe: iterator over PCIe ports */
	int ret;				/* PCI/NVMe: result of PHY power on */

	list_for_each_entry(port, &pcie->ports, list) { /* PCI/NVMe: walk every NVMe root port */
		ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_RC); /* PCI/NVMe: configure PHY as PCIe Root Complex for NVMe */
		if (ret) /* PCI/NVMe: PHY mode failure blocks NVMe link */
			return ret;

		ret = phy_power_on(port->phy); /* PCI/NVMe: power on PHY to train NVMe link */
		if (ret) { /* PCI/NVMe: PHY power-on failure rolls back previously powered PHYs */
			qcom_pcie_phy_power_off(pcie);
			return ret;
		}
	}

	return 0; /* PCI/NVMe: all NVMe PHYs powered on */
}

static int qcom_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* PCI/NVMe: get DW core from root port for NVMe host */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* PCI/NVMe: get Qualcomm wrapper for NVMe RC */
	int ret;				/* PCI/NVMe: result of host init steps */

	qcom_pcie_perst_assert(pcie); /* PCI/NVMe: assert PERST# to hold NVMe endpoint in reset during setup */

	ret = pcie->cfg->ops->init(pcie); /* PCI/NVMe: power/clock/reset init for NVMe RC */
	if (ret) /* PCI/NVMe: init failure aborts NVMe host bring-up */
		return ret;

	ret = qcom_pcie_phy_power_on(pcie); /* PCI/NVMe: power on PHY for NVMe link training */
	if (ret) /* PCI/NVMe: PHY failure rolls back controller init */
		goto err_deinit;

	ret = pci_pwrctrl_create_devices(pci->dev); /* PCI/NVMe: create power-control devices for NVMe endpoints */
	if (ret) /* PCI/NVMe: power control creation failure rolls back PHY */
		goto err_disable_phy;

	ret = pci_pwrctrl_power_on_devices(pci->dev); /* PCI/NVMe: power on NVMe endpoint devices */
	if (ret) /* PCI/NVMe: endpoint power-on failure rolls back power control */
		goto err_pwrctrl_destroy;

	if (pcie->cfg->ops->post_init) { /* PCI/NVMe: SoC-specific post-init may configure DBI/ATU for NVMe */
		ret = pcie->cfg->ops->post_init(pcie);
		if (ret) /* PCI/NVMe: post-init failure rolls back endpoint power */
			goto err_pwrctrl_power_off;
	}

	qcom_pcie_clear_aspm_l0s(pcie->pci); /* PCI/NVMe: disable L0s ASPM if configured for NVMe */
	dw_pcie_remove_capability(pcie->pci, PCI_CAP_ID_MSIX); /* PCI/NVMe: remove MSI-X cap from root port; NVMe endpoint still uses MSI-X */
	dw_pcie_remove_ext_capability(pcie->pci, PCI_EXT_CAP_ID_DPC); /* PCI/NVMe: remove DPC ext cap from root port for NVMe compatibility */

	qcom_pcie_perst_deassert(pcie); /* PCI/NVMe: release PERST# so NVMe endpoint can train link */

	if (pcie->cfg->ops->config_sid) { /* PCI/NVMe: configure BDF-to-SID for IOMMU/SMMU of NVMe DMA */
		ret = pcie->cfg->ops->config_sid(pcie);
		if (ret) /* PCI/NVMe: SID config failure reasserts PERST# */
			goto err_assert_reset;
	}

	return 0; /* PCI/NVMe: host init complete; DesignWare core will enumerate NVMe devices */

err_assert_reset:
	qcom_pcie_perst_assert(pcie); /* PCI/NVMe: reassert PERST# to reset NVMe endpoint on SID failure */
err_pwrctrl_power_off:
	pci_pwrctrl_power_off_devices(pci->dev); /* PCI/NVMe: power off NVMe endpoints on post-init failure */
err_pwrctrl_destroy:
	if (ret != -EPROBE_DEFER) /* PCI/NVMe: preserve devices if probe deferred for NVMe */
		pci_pwrctrl_destroy_devices(pci->dev); /* PCI/NVMe: destroy power control devices on failure */
err_disable_phy:
	qcom_pcie_phy_power_off(pcie); /* PCI/NVMe: power off PHY on failure */
err_deinit:
	pcie->cfg->ops->deinit(pcie); /* PCI/NVMe: deinit controller clocks/resets on failure */

	return ret; /* PCI/NVMe: return original host init error */
}

static void qcom_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* PCI/NVMe: get DW core from root port */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* PCI/NVMe: get Qualcomm wrapper */

	qcom_pcie_perst_assert(pcie); /* PCI/NVMe: assert PERST# to reset NVMe endpoint during deinit */

	/*
	 * No need to destroy pwrctrl devices as this function only gets called
	 * during system suspend as of now.
	 */
	pci_pwrctrl_power_off_devices(pci->dev); /* PCI/NVMe: power off NVMe endpoints in suspend */
	qcom_pcie_phy_power_off(pcie); /* PCI/NVMe: power off PHY during NVMe suspend */
	pcie->cfg->ops->deinit(pcie); /* PCI/NVMe: disable clocks/resets of NVMe RC */
}

static void qcom_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp); /* PCI/NVMe: get DW core from root port */
	struct qcom_pcie *pcie = to_qcom_pcie(pci); /* PCI/NVMe: get Qualcomm wrapper */

	if (pcie->cfg->ops->host_post_init) /* PCI/NVMe: late host init callback, e.g. ASPM for NVMe */
		pcie->cfg->ops->host_post_init(pcie); /* PCI/NVMe: run SoC-specific post-enumeration setup for NVMe */
}

static const struct dw_pcie_host_ops qcom_pcie_dw_ops = {
	.init		= qcom_pcie_host_init, /* PCI/NVMe: DesignWare core calls this to init NVMe host */
	.deinit		= qcom_pcie_host_deinit, /* PCI/NVMe: DesignWare core calls this to deinit NVMe host */
	.post_init	= qcom_pcie_host_post_init, /* PCI/NVMe: DesignWare core calls this after NVMe enumeration */
};

/* Qcom IP rev.: 2.1.0	Synopsys IP rev.: 4.01a */
static const struct qcom_pcie_ops ops_2_1_0 = {
	.get_resources = qcom_pcie_get_resources_2_1_0, /* PCI/NVMe: parse clocks/resets/regulators for v2.1.0 NVMe RC */
	.init = qcom_pcie_init_2_1_0, /* PCI/NVMe: power/reset init for v2.1.0 NVMe RC */
	.post_init = qcom_pcie_post_init_2_1_0, /* PCI/NVMe: PHY/DBI config for v2.1.0 NVMe RC */
	.deinit = qcom_pcie_deinit_2_1_0, /* PCI/NVMe: power down v2.1.0 NVMe RC */
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable, /* PCI/NVMe: start link training for v2.1.0 NVMe link */
};

/* Qcom IP rev.: 1.0.0	Synopsys IP rev.: 4.11a */
static const struct qcom_pcie_ops ops_1_0_0 = {
	.get_resources = qcom_pcie_get_resources_1_0_0, /* PCI/NVMe: parse resources for v1.0.0 NVMe RC */
	.init = qcom_pcie_init_1_0_0, /* PCI/NVMe: power/reset init for v1.0.0 NVMe RC */
	.post_init = qcom_pcie_post_init_1_0_0, /* PCI/NVMe: DBI/MSI config for v1.0.0 NVMe RC */
	.deinit = qcom_pcie_deinit_1_0_0, /* PCI/NVMe: power down v1.0.0 NVMe RC */
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable, /* PCI/NVMe: reuse ELBI LTSSM enable for v1.0.0 NVMe link */
};

/* Qcom IP rev.: 2.3.2	Synopsys IP rev.: 4.21a */
static const struct qcom_pcie_ops ops_2_3_2 = {
	.get_resources = qcom_pcie_get_resources_2_3_2, /* PCI/NVMe: parse resources for v2.3.2 NVMe RC */
	.init = qcom_pcie_init_2_3_2, /* PCI/NVMe: power/clock init for v2.3.2 NVMe RC */
	.post_init = qcom_pcie_post_init_2_3_2, /* PCI/NVMe: PHY/DBI config for v2.3.2 NVMe RC */
	.deinit = qcom_pcie_deinit_2_3_2, /* PCI/NVMe: power down v2.3.2 NVMe RC */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v2.3.2 NVMe link */
};

/* Qcom IP rev.: 2.4.0	Synopsys IP rev.: 4.20a */
static const struct qcom_pcie_ops ops_2_4_0 = {
	.get_resources = qcom_pcie_get_resources_2_4_0, /* PCI/NVMe: parse resources for v2.4.0 NVMe RC */
	.init = qcom_pcie_init_2_4_0, /* PCI/NVMe: reset/clock init for v2.4.0 NVMe RC */
	.post_init = qcom_pcie_post_init_2_3_2, /* PCI/NVMe: reuse v2.3.2 PHY/DBI config for v2.4.0 NVMe RC */
	.deinit = qcom_pcie_deinit_2_4_0, /* PCI/NVMe: power down v2.4.0 NVMe RC */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v2.4.0 NVMe link */
};

/* Qcom IP rev.: 2.3.3	Synopsys IP rev.: 4.30a */
static const struct qcom_pcie_ops ops_2_3_3 = {
	.get_resources = qcom_pcie_get_resources_2_3_3, /* PCI/NVMe: parse resources for v2.3.3 NVMe RC */
	.init = qcom_pcie_init_2_3_3, /* PCI/NVMe: reset/clock init for v2.3.3 NVMe RC */
	.post_init = qcom_pcie_post_init_2_3_3, /* PCI/NVMe: PHY/DBI/ATU/slot cap config for v2.3.3 NVMe RC */
	.deinit = qcom_pcie_deinit_2_3_3, /* PCI/NVMe: power down v2.3.3 NVMe RC */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v2.3.3 NVMe link */
};

/* Qcom IP rev.: 2.7.0	Synopsys IP rev.: 4.30a */
static const struct qcom_pcie_ops ops_2_7_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* PCI/NVMe: parse resources for v2.7.0 NVMe RC */
	.init = qcom_pcie_init_2_7_0, /* PCI/NVMe: regulator/clock/reset init for v2.7.0 NVMe RC */
	.post_init = qcom_pcie_post_init_2_7_0, /* PCI/NVMe: no-snoop override and slot cap for v2.7.0 NVMe RC */
	.deinit = qcom_pcie_deinit_2_7_0, /* PCI/NVMe: power down v2.7.0 NVMe RC */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v2.7.0 NVMe link */
};

/* Qcom IP rev.: 1.9.0 */
static const struct qcom_pcie_ops ops_1_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* PCI/NVMe: v1.9.0 reuses v2.7.0 resource parsing for NVMe RC */
	.init = qcom_pcie_init_2_7_0, /* PCI/NVMe: v1.9.0 reuses v2.7.0 init for NVMe RC */
	.post_init = qcom_pcie_post_init_2_7_0, /* PCI/NVMe: v1.9.0 reuses v2.7.0 post-init for NVMe RC */
	.host_post_init = qcom_pcie_host_post_init_2_7_0, /* PCI/NVMe: enable ASPM on NVMe devices after enumeration */
	.deinit = qcom_pcie_deinit_2_7_0, /* PCI/NVMe: v1.9.0 reuses v2.7.0 deinit */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v1.9.0 NVMe link */
	.config_sid = qcom_pcie_config_sid_1_9_0, /* PCI/NVMe: configure IOMMU BDF-to-SID for NVMe DMA isolation */
};

/* Qcom IP rev.: 1.21.0  Synopsys IP rev.: 5.60a */
static const struct qcom_pcie_ops ops_1_21_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0, /* PCI/NVMe: v1.21.0 reuses v2.7.0 resource parsing for NVMe RC */
	.init = qcom_pcie_init_2_7_0, /* PCI/NVMe: v1.21.0 reuses v2.7.0 init for NVMe RC */
	.post_init = qcom_pcie_post_init_2_7_0, /* PCI/NVMe: v1.21.0 reuses v2.7.0 post-init for NVMe RC */
	.host_post_init = qcom_pcie_host_post_init_2_7_0, /* PCI/NVMe: enable ASPM on NVMe devices after enumeration */
	.deinit = qcom_pcie_deinit_2_7_0, /* PCI/NVMe: v1.21.0 reuses v2.7.0 deinit */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v1.21.0 NVMe link */
};

/* Qcom IP rev.: 2.9.0  Synopsys IP rev.: 5.00a */
static const struct qcom_pcie_ops ops_2_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_9_0, /* PCI/NVMe: parse resources for v2.9.0 NVMe RC */
	.init = qcom_pcie_init_2_9_0, /* PCI/NVMe: reset/clock init for v2.9.0 NVMe RC */
	.post_init = qcom_pcie_post_init_2_9_0, /* PCI/NVMe: PHY/DBI/ATU/slot cap config for v2.9.0 NVMe RC */
	.deinit = qcom_pcie_deinit_2_9_0, /* PCI/NVMe: power down v2.9.0 NVMe RC */
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable, /* PCI/NVMe: start link training for v2.9.0 NVMe link */
};

static const struct qcom_pcie_cfg cfg_1_0_0 = {
	.ops = &ops_1_0_0, /* PCI/NVMe: config for v1.0.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_1_9_0 = {
	.ops = &ops_1_9_0, /* PCI/NVMe: config for v1.9.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_1_34_0 = {
	.ops = &ops_1_9_0, /* PCI/NVMe: config for v1.34.0 NVMe RC reuses v1.9.0 ops */
	.override_no_snoop = true, /* PCI/NVMe: force snoop for NVMe DMA coherency on this SoC */
};

static const struct qcom_pcie_cfg cfg_2_1_0 = {
	.ops = &ops_2_1_0, /* PCI/NVMe: config for v2.1.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_2_3_2 = {
	.ops = &ops_2_3_2, /* PCI/NVMe: config for v2.3.2 NVMe RC */
	.no_l0s = true, /* PCI/NVMe: disable L0s ASPM for NVMe on this SoC */
};

static const struct qcom_pcie_cfg cfg_2_3_3 = {
	.ops = &ops_2_3_3, /* PCI/NVMe: config for v2.3.3 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_2_4_0 = {
	.ops = &ops_2_4_0, /* PCI/NVMe: config for v2.4.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_2_7_0 = {
	.ops = &ops_2_7_0, /* PCI/NVMe: config for v2.7.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_2_9_0 = {
	.ops = &ops_2_9_0, /* PCI/NVMe: config for v2.9.0 NVMe RC */
};

static const struct qcom_pcie_cfg cfg_sc8280xp = {
	.ops = &ops_1_21_0, /* PCI/NVMe: config for SC8280XP NVMe RC */
	.no_l0s = true, /* PCI/NVMe: disable L0s ASPM for NVMe on SC8280XP */
};

static const struct qcom_pcie_cfg cfg_fw_managed = {
	.firmware_managed = true, /* PCI/NVMe: firmware manages link training; kernel enumerates NVMe via ECAM */
};

static const struct dw_pcie_ops dw_pcie_ops = {
	.link_up = qcom_pcie_link_up, /* PCI/NVMe: DW core queries this to know if NVMe link is up */
	.start_link = qcom_pcie_start_link, /* PCI/NVMe: DW core calls this to start NVMe link training */
};

static int qcom_pcie_icc_init(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	int ret;				/* PCI/NVMe: result of ICC setup */

	pcie->icc_mem = devm_of_icc_get(pci->dev, "pcie-mem"); /* PCI/NVMe: get interconnect path for NVMe DMA traffic */
	if (IS_ERR(pcie->icc_mem)) /* PCI/NVMe: ICC get failure blocks NVMe bandwidth setup */
		return PTR_ERR(pcie->icc_mem);

	pcie->icc_cpu = devm_of_icc_get(pci->dev, "cpu-pcie"); /* PCI/NVMe: get interconnect path for CPU-to-NVMe config/MMIO */
	if (IS_ERR(pcie->icc_cpu)) /* PCI/NVMe: ICC get failure blocks NVMe bandwidth setup */
		return PTR_ERR(pcie->icc_cpu);
	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(pcie->icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1)); /* PCI/NVMe: vote minimal Gen1 x1 bandwidth for NVMe DMA path */
	if (ret) { /* PCI/NVMe: bandwidth vote failure aborts NVMe ICC init */
		dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
			ret);
		return ret;
	}

	/*
	 * Since the CPU-PCIe path is only used for activities like register
	 * access of the host controller and endpoint Config/BAR space access,
	 * HW team has recommended to use a minimal bandwidth of 1KBps just to
	 * keep the path active.
	 */
	ret = icc_set_bw(pcie->icc_cpu, 0, kBps_to_icc(1)); /* PCI/NVMe: vote 1KBps to keep CPU-NVMe config path active */
	if (ret) { /* PCI/NVMe: CPU path bandwidth vote failure rolls back mem path */
		dev_err(pci->dev, "Failed to set bandwidth for CPU-PCIe interconnect path: %d\n",
			ret);
		icc_set_bw(pcie->icc_mem, 0, 0); /* PCI/NVMe: clear mem path bandwidth on CPU path failure */
		return ret;
	}

	return 0; /* PCI/NVMe: ICC initialized with initial NVMe bandwidth votes */
}

static void qcom_pcie_icc_opp_update(struct qcom_pcie *pcie)
{
	u32 offset, status, width, speed;	/* PCI/NVMe: PCIe cap offset and Link Status fields for NVMe */
	struct dw_pcie *pci = pcie->pci;	/* PCI/NVMe: DW core for NVMe host */
	struct dev_pm_opp_key key = {};		/* PCI/NVMe: OPP search key for NVMe bandwidth */
	unsigned long freq_kbps;		/* PCI/NVMe: calculated NVMe bandwidth in kbps */
	struct dev_pm_opp *opp;			/* PCI/NVMe: OPP handle for NVMe power/perf */
	int ret, freq_mbps;			/* PCI/NVMe: return value and link speed in Mbps */

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP); /* PCI/NVMe: locate PCIe cap of NVMe RP */
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* PCI/NVMe: read Link Status of NVMe link */

	/* Only update constraints if link is up. */
	if (!(status & PCI_EXP_LNKSTA_DLLLA)) /* PCI/NVMe: skip bandwidth update if NVMe link is down */
		return;

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status); /* PCI/NVMe: negotiated link speed for NVMe */
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status); /* PCI/NVMe: negotiated link width for NVMe */

	if (pcie->icc_mem) { /* PCI/NVMe: update interconnect bandwidth based on NVMe link speed/width */
		ret = icc_set_bw(pcie->icc_mem, 0,
				 width * QCOM_PCIE_LINK_SPEED_TO_BW(speed)); /* PCI/NVMe: vote NVMe DMA bandwidth = width * speed */
		if (ret) { /* PCI/NVMe: bandwidth update failure is non-fatal but logged */
			dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret);
		}
	} else if (pcie->use_pm_opp) { /* PCI/NVMe: use OPP framework instead of ICC for NVMe power/perf */
		freq_mbps = pcie_dev_speed_mbps(pcie_get_link_speed(speed)); /* PCI/NVMe: convert NVMe link speed to Mbps */
		if (freq_mbps < 0) /* PCI/NVMe: invalid speed aborts OPP update */
			return;

		freq_kbps = freq_mbps * KILO; /* PCI/NVMe: convert to kbps for OPP */
		opp = dev_pm_opp_find_level_exact(pci->dev, speed); /* PCI/NVMe: search OPP by PCIe speed level for NVMe */
		if (IS_ERR(opp)) { /* PCI/NVMe: no level-based OPP; search by frequency */
			 /* opp-level is not defined use only frequency */
			opp = dev_pm_opp_find_freq_exact(pci->dev, freq_kbps * width,
							 true); /* PCI/NVMe: search OPP by NVMe bandwidth frequency */
		} else {
			/* put opp-level OPP */
			dev_pm_opp_put(opp); /* PCI/NVMe: release level-based OPP reference */

			key.freq = freq_kbps * width; /* PCI/NVMe: OPP key frequency for NVMe bandwidth */
			key.level = speed; /* PCI/NVMe: OPP key level for NVMe link speed */
			key.bw = 0; /* PCI/NVMe: OPP key bandwidth placeholder */
			opp = dev_pm_opp_find_key_exact(pci->dev, &key, true); /* PCI/NVMe: search exact OPP key for NVMe */
		}
		if (!IS_ERR(opp)) { /* PCI/NVMe: valid OPP found; apply it for NVMe power/perf */
			ret = dev_pm_opp_set_opp(pci->dev, opp);
			if (ret) /* PCI/NVMe: OPP apply failure is non-fatal but logged */
				dev_err(pci->dev, "Failed to set OPP for freq (%lu): %d\n",
					freq_kbps * width, ret);
			dev_pm_opp_put(opp); /* PCI/NVMe: release OPP reference */
		}
	}
}

static int qcom_pcie_link_transition_count(struct seq_file *s, void *data)
{
	struct qcom_pcie *pcie = (struct qcom_pcie *)dev_get_drvdata(s->private); /* PCI/NVMe: get qcom_pcie from debugfs private data */

	seq_printf(s, "L0s transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L0S)); /* PCI/NVMe: show NVMe L0s ASPM transition count */

	seq_printf(s, "L1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L1)); /* PCI/NVMe: show NVMe L1 ASPM transition count */

	seq_printf(s, "L1.1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1)); /* PCI/NVMe: show NVMe L1.1 substate transition count */

	seq_printf(s, "L1.2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2)); /* PCI/NVMe: show NVMe L1.2 substate transition count */

	seq_printf(s, "L2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L2)); /* PCI/NVMe: show NVMe L2 link state transition count */

	return 0; /* PCI/NVMe: debugfs read complete */
}

static void qcom_pcie_init_debugfs(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci; /* PCI/NVMe: DW core for NVMe host */
	struct device *dev = pci->dev; /* PCI/NVMe: device for debugfs node naming */
	char *name;				/* PCI/NVMe: debugfs directory name */

	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node); /* PCI/NVMe: create DT-path name for NVMe RC debugfs */
	if (!name) /* PCI/NVMe: allocation failure skips debugfs creation */
		return;

	pcie->debugfs = debugfs_create_dir(name, NULL); /* PCI/NVMe: create per-NVMe-RC debugfs directory */
	debugfs_create_devm_seqfile(dev, "link_transition_count", pcie->debugfs,
				    qcom_pcie_link_transition_count); /* PCI/NVMe: expose NVMe ASPM transition counters */
}

static void qcom_pci_free_msi(void *ptr)
{
	struct dw_pcie_rp *pp = (struct dw_pcie_rp *)ptr; /* PCI/NVMe: DesignWare root port for NVMe MSI handling */

	if (pp && pp->use_imsi_rx) /* PCI/NVMe: only free if inbound MSI was allocated for NVMe */
		dw_pcie_free_msi(pp); /* PCI/NVMe: free MSI doorbell region used by NVMe endpoints */
}

static int qcom_pcie_ecam_host_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent; /* PCI/NVMe: parent device of firmware-managed ECAM window */
	struct dw_pcie_rp *pp;			/* PCI/NVMe: DesignWare root port for NVMe MSI */
	struct dw_pcie *pci;			/* PCI/NVMe: DesignWare core for NVMe host */
	int ret;					/* PCI/NVMe: result of ECAM host init */

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL); /* PCI/NVMe: allocate DW core for NVMe host */
	if (!pci) /* PCI/NVMe: allocation failure aborts firmware-managed NVMe init */
		return -ENOMEM;

	pci->dev = dev; /* PCI/NVMe: assign device to DW core for NVMe host */
	pp = &pci->pp; /* PCI/NVMe: root port used for NVMe MSI setup */
	pci->dbi_base = cfg->win; /* PCI/NVMe: DBI base mapped to ECAM window for NVMe config access */
	pp->num_vectors = MSI_DEF_NUM_VECTORS; /* PCI/NVMe: default number of MSI vectors for NVMe endpoints */

	ret = dw_pcie_msi_host_init(pp); /* PCI/NVMe: initialize MSI parent for firmware-managed NVMe RC */
	if (ret) /* PCI/NVMe: MSI init failure aborts firmware-managed NVMe init */
		return ret;

	pp->use_imsi_rx = true; /* PCI/NVMe: enable inbound MSI receiver for NVMe endpoints */
	dw_pcie_msi_init(pp); /* PCI/NVMe: program MSI hardware for NVMe MSI/MSI-X delivery */

	return devm_add_action_or_reset(dev, qcom_pci_free_msi, pp); /* PCI/NVMe: register cleanup for NVMe MSI resources */
}

static const struct pci_ecam_ops pci_qcom_ecam_ops = {
	.init		= qcom_pcie_ecam_host_init, /* PCI/NVMe: ECAM init for firmware-managed NVMe RC */
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus, /* PCI/NVMe: map ECAM bus/dev/func for NVMe config cycles */
		.read		= pci_generic_config_read, /* PCI/NVMe: read NVMe endpoint config space via ECAM */
		.write		= pci_generic_config_write, /* PCI/NVMe: write NVMe endpoint config space via ECAM */
	}
};

/* Parse PERST# from all nodes in depth first manner starting from @np */
static int qcom_pcie_parse_perst(struct qcom_pcie *pcie,
				 struct qcom_pcie_port *port,
				 struct device_node *np)
{
	struct device *dev = pcie->pci->dev; /* PCI/NVMe: device for GPIO allocation */
	struct qcom_pcie_perst *perst;		/* PCI/NVMe: new PERST# descriptor for NVMe */
	struct gpio_desc *reset;			/* PCI/NVMe: GPIO descriptor for NVMe PERST# */
	int ret;						/* PCI/NVMe: result of PERST# parse */

	if (!of_find_property(np, "reset-gpios", NULL)) /* PCI/NVMe: skip node if no reset GPIO for NVMe */
		goto parse_child_node;

	reset = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_HIGH, "PERST#"); /* PCI/NVMe: acquire PERST# GPIO for NVMe endpoint */
	if (IS_ERR(reset)) { /* PCI/NVMe: GPIO acquisition failure handling */
		/*
		 * FIXME: GPIOLIB currently supports exclusive GPIO access only.
		 * Non exclusive access is broken. But shared PERST# requires
		 * non-exclusive access. So once GPIOLIB properly supports it,
		 * implement it here.
		 */
		if (PTR_ERR(reset) == -EBUSY) /* PCI/NVMe: shared PERST# not supported for NVMe yet */
			dev_err(dev, "Shared PERST# is not supported\n");

		return PTR_ERR(reset); /* PCI/NVMe: return GPIO error to abort NVMe probe */
	}

	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL); /* PCI/NVMe: allocate PERST# tracking struct for NVMe */
	if (!perst) /* PCI/NVMe: allocation failure aborts NVMe probe */
		return -ENOMEM;

	INIT_LIST_HEAD(&perst->list); /* PCI/NVMe: initialize PERST# list node */
	perst->desc = reset; /* PCI/NVMe: store PERST# GPIO descriptor for NVMe reset control */
	list_add_tail(&perst->list, &port->perst); /* PCI/NVMe: add PERST# to port's list */

parse_child_node:
	for_each_available_child_of_node_scoped(np, child) { /* PCI/NVMe: recursively parse child nodes for NVMe PERST# */
		ret = qcom_pcie_parse_perst(pcie, port, child);
		if (ret) /* PCI/NVMe: child parse failure aborts NVMe PERST# setup */
			return ret;
	}

	return 0; /* PCI/NVMe: PERST# parse complete for this node */
}

static int qcom_pcie_parse_port(struct qcom_pcie *pcie, struct device_node *node)
{
	struct device *dev = pcie->pci->dev; /* PCI/NVMe: device for PHY and memory allocation */
	struct qcom_pcie_port *port;		/* PCI/NVMe: new PCIe port for NVMe */
	struct phy *phy;				/* PCI/NVMe: PHY handle for NVMe root port lanes */
	int ret;						/* PCI/NVMe: result of port parse */

	phy = devm_of_phy_get(dev, node, NULL); /* PCI/NVMe: get PHY for NVMe root port from DT */
	if (IS_ERR(phy)) /* PCI/NVMe: PHY acquisition failure aborts NVMe port setup */
		return PTR_ERR(phy);

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL); /* PCI/NVMe: allocate port struct for NVMe */
	if (!port) /* PCI/NVMe: allocation failure aborts NVMe port setup */
		return -ENOMEM;

	ret = phy_init(phy); /* PCI/NVMe: initialize PHY for NVMe link training */
	if (ret) /* PCI/NVMe: PHY init failure aborts NVMe port setup */
		return ret;

	INIT_LIST_HEAD(&port->perst); /* PCI/NVMe: initialize PERST# list for this NVMe port */

	ret = qcom_pcie_parse_perst(pcie, port, node); /* PCI/NVMe: parse PERST# GPIOs for NVMe port */
	if (ret) /* PCI/NVMe: PERST# parse failure aborts NVMe port setup */
		return ret;

	port->phy = phy; /* PCI/NVMe: store PHY handle for NVMe port */
	INIT_LIST_HEAD(&port->list); /* PCI/NVMe: initialize port list node */
	list_add_tail(&port->list, &pcie->ports); /* PCI/NVMe: add port to controller's port list */

	return 0; /* PCI/NVMe: port parse complete */
}

static int qcom_pcie_parse_ports(struct qcom_pcie *pcie)
{
	struct qcom_pcie_perst *perst, *tmp_perst;	/* PCI/NVMe: iterators for PERST# cleanup */
	struct qcom_pcie_port *port, *tmp_port;		/* PCI/NVMe: iterators for port cleanup */
	struct device *dev = pcie->pci->dev;		/* PCI/NVMe: device for DT child iteration */
	int ret = -ENODEV;						/* PCI/NVMe: default error if no pci port found for NVMe */

	for_each_available_child_of_node_scoped(dev->of_node, of_port) { /* PCI/NVMe: iterate DT child nodes for NVMe ports */
		if (!of_node_is_type(of_port, "pci")) /* PCI/NVMe: skip non-pci child nodes */
			continue;
		ret = qcom_pcie_parse_port(pcie, of_port); /* PCI/NVMe: parse one NVMe root port */
		if (ret) /* PCI/NVMe: port parse failure triggers cleanup */
			goto err_port_del;
	}

	return ret; /* PCI/NVMe: returns 0 if ports parsed, -ENODEV if none found */

err_port_del:
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) { /* PCI/NVMe: cleanup parsed ports on failure */
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list) /* PCI/NVMe: cleanup parsed PERST# entries */
			list_del(&perst->list);
		phy_exit(port->phy); /* PCI/NVMe: exit PHY for failed NVMe port */
		list_del(&port->list);
	}

	return ret; /* PCI/NVMe: return original parse error */
}

static int qcom_pcie_parse_legacy_binding(struct qcom_pcie *pcie)
{
	struct device *dev = pcie->pci->dev; /* PCI/NVMe: device for legacy PHY/GPIO allocation */
	struct qcom_pcie_perst *perst;		/* PCI/NVMe: PERST# descriptor for legacy NVMe binding */
	struct qcom_pcie_port *port;		/* PCI/NVMe: port descriptor for legacy NVMe binding */
	struct gpio_desc *reset;			/* PCI/NVMe: PERST# GPIO for legacy NVMe binding */
	struct phy *phy;					/* PCI/NVMe: PHY for legacy NVMe binding */
	int ret;						/* PCI/NVMe: result of legacy parse */

	phy = devm_phy_optional_get(dev, "pciephy"); /* PCI/NVMe: get optional PHY for legacy NVMe binding */
	if (IS_ERR(phy)) /* PCI/NVMe: PHY error aborts legacy NVMe setup */
		return PTR_ERR(phy);

	reset = devm_gpiod_get_optional(dev, "perst", GPIOD_OUT_HIGH); /* PCI/NVMe: get optional PERST# GPIO for legacy NVMe */
	if (IS_ERR(reset)) /* PCI/NVMe: GPIO error aborts legacy NVMe setup */
		return PTR_ERR(reset);

	ret = phy_init(phy); /* PCI/NVMe: initialize PHY for legacy NVMe link */
	if (ret) /* PCI/NVMe: PHY init failure aborts legacy NVMe setup */
		return ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL); /* PCI/NVMe: allocate port for legacy NVMe binding */
	if (!port) /* PCI/NVMe: allocation failure aborts legacy NVMe setup */
		return -ENOMEM;

	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL); /* PCI/NVMe: allocate PERST# for legacy NVMe binding */
	if (!perst) /* PCI/NVMe: allocation failure aborts legacy NVMe setup */
		return -ENOMEM;

	port->phy = phy; /* PCI/NVMe: store PHY for legacy NVMe port */
	INIT_LIST_HEAD(&port->list); /* PCI/NVMe: initialize legacy port list node */
	list_add_tail(&port->list, &pcie->ports); /* PCI/NVMe: add legacy port to controller list */

	perst->desc = reset; /* PCI/NVMe: store PERST# GPIO for legacy NVMe */
	INIT_LIST_HEAD(&port->perst); /* PCI/NVMe: initialize legacy PERST# list */
	INIT_LIST_HEAD(&perst->list); /* PCI/NVMe: initialize legacy PERST# node */
	list_add_tail(&perst->list, &port->perst); /* PCI/NVMe: add PERST# to legacy port */

	return 0; /* PCI/NVMe: legacy binding parse complete */
}

static int qcom_pcie_probe(struct platform_device *pdev)
{
	struct qcom_pcie_perst *perst, *tmp_perst;	/* PCI/NVMe: cleanup iterators for PERST# */
	struct qcom_pcie_port *port, *tmp_port;		/* PCI/NVMe: cleanup iterators for ports */
	const struct qcom_pcie_cfg *pcie_cfg;		/* PCI/NVMe: matched SoC config for NVMe RC */
	unsigned long max_freq = ULONG_MAX;			/* PCI/NVMe: max OPP frequency for NVMe link bring-up */
	struct device *dev = &pdev->dev;			/* PCI/NVMe: platform device for NVMe RC */
	struct dev_pm_opp *opp;						/* PCI/NVMe: max OPP for NVMe RC */
	struct qcom_pcie *pcie;						/* PCI/NVMe: Qualcomm PCIe wrapper for NVMe host */
	struct dw_pcie_rp *pp;						/* PCI/NVMe: DesignWare root port for NVMe */
	struct resource *res;						/* PCI/NVMe: MHI memory resource for NVMe debug/ASPM */
	struct dw_pcie *pci;						/* PCI/NVMe: DesignWare core for NVMe host */
	int ret;									/* PCI/NVMe: result of probe steps */

	pcie_cfg = of_device_get_match_data(dev); /* PCI/NVMe: lookup SoC-specific NVMe RC config */
	if (!pcie_cfg) { /* PCI/NVMe: missing match data aborts NVMe RC probe */
		dev_err(dev, "No platform data\n");
		return -ENODATA;
	}

	if (!pcie_cfg->firmware_managed && !pcie_cfg->ops) { /* PCI/NVMe: non-firmware-managed RC needs ops for NVMe */
		dev_err(dev, "No platform ops\n");
		return -ENODATA;
	}

	pm_runtime_enable(dev); /* PCI/NVMe: enable runtime PM for NVMe RC */
	ret = pm_runtime_get_sync(dev); /* PCI/NVMe: hold runtime PM reference during NVMe probe */
	if (ret < 0) /* PCI/NVMe: runtime PM failure aborts NVMe probe */
		goto err_pm_runtime_put;

	if (pcie_cfg->firmware_managed) { /* PCI/NVMe: firmware already trained link; kernel uses ECAM for NVMe */
		struct pci_host_bridge *bridge;
		struct pci_config_window *cfg;

		bridge = devm_pci_alloc_host_bridge(dev, 0); /* PCI/NVMe: allocate host bridge for firmware-managed NVMe RC */
		if (!bridge) { /* PCI/NVMe: bridge allocation failure aborts NVMe probe */
			ret = -ENOMEM;
			goto err_pm_runtime_put;
		}

		/* Parse and map our ECAM configuration space area */
		cfg = pci_host_common_ecam_create(dev, bridge,
				&pci_qcom_ecam_ops); /* PCI/NVMe: create ECAM window for NVMe config space */
		if (IS_ERR(cfg)) { /* PCI/NVMe: ECAM creation failure aborts NVMe probe */
			ret = PTR_ERR(cfg);
			goto err_pm_runtime_put;
		}

		bridge->sysdata = cfg; /* PCI/NVMe: store ECAM window in host bridge for NVMe enumeration */
		bridge->ops = (struct pci_ops *)&pci_qcom_ecam_ops.pci_ops; /* PCI/NVMe: use ECAM PCI ops for NVMe config access */
		bridge->msi_domain = true; /* PCI/NVMe: enable MSI domain for firmware-managed NVMe RC */

		ret = pci_host_probe(bridge); /* PCI/NVMe: probe host bridge and enumerate NVMe devices */
		if (ret) /* PCI/NVMe: host probe failure aborts NVMe enumeration */
			goto err_pm_runtime_put;

		return 0; /* PCI/NVMe: firmware-managed NVMe RC probe complete */
	}

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL); /* PCI/NVMe: allocate Qualcomm wrapper for NVMe RC */
	if (!pcie) { /* PCI/NVMe: allocation failure aborts NVMe probe */
		ret = -ENOMEM;
		goto err_pm_runtime_put;
	}

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL); /* PCI/NVMe: allocate DesignWare core for NVMe host */
	if (!pci) { /* PCI/NVMe: allocation failure aborts NVMe probe */
		ret = -ENOMEM;
		goto err_pm_runtime_put;
	}

	INIT_LIST_HEAD(&pcie->ports); /* PCI/NVMe: initialize NVMe root port list */

	pci->dev = dev; /* PCI/NVMe: assign device to DW core for NVMe host */
	pci->ops = &dw_pcie_ops; /* PCI/NVMe: register Qcom link_up/start_link ops for NVMe link */
	pp = &pci->pp; /* PCI/NVMe: root port for NVMe host initialization */

	pcie->pci = pci; /* PCI/NVMe: link Qualcomm wrapper to DW core */

	pcie->cfg = pcie_cfg; /* PCI/NVMe: store SoC config for NVMe RC */

	pcie->parf = devm_platform_ioremap_resource_byname(pdev, "parf"); /* PCI/NVMe: map PARF registers for NVMe RC config */
	if (IS_ERR(pcie->parf)) { /* PCI/NVMe: PARF map failure aborts NVMe probe */
		ret = PTR_ERR(pcie->parf);
		goto err_pm_runtime_put;
	}

	/* MHI region is optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mhi"); /* PCI/NVMe: get optional MHI memory resource for NVMe debug */
	if (res) { /* PCI/NVMe: MHI resource present; map it for ASPM counters */
		pcie->mhi = devm_ioremap_resource(dev, res); /* PCI/NVMe: map MHI registers for NVMe ASPM counters */
		if (IS_ERR(pcie->mhi)) { /* PCI/NVMe: MHI map failure aborts NVMe probe */
			ret = PTR_ERR(pcie->mhi);
			goto err_pm_runtime_put;
		}
	}

	/* OPP table is optional */
	ret = devm_pm_opp_of_add_table(dev); /* PCI/NVMe: parse optional OPP table for NVMe power/perf */
	if (ret && ret != -ENODEV) { /* PCI/NVMe: non-ENOENT OPP error aborts NVMe probe */
		dev_err_probe(dev, ret, "Failed to add OPP table\n");
		goto err_pm_runtime_put;
	}

	/*
	 * Before the PCIe link is initialized, vote for highest OPP in the OPP
	 * table, so that we are voting for maximum voltage corner for the
	 * link to come up in maximum supported speed. At the end of the
	 * probe(), OPP will be updated using qcom_pcie_icc_opp_update().
	 */
	if (!ret) { /* PCI/NVMe: OPP table present; vote max performance for NVMe link bring-up */
		opp = dev_pm_opp_find_freq_floor(dev, &max_freq); /* PCI/NVMe: find highest OPP for NVMe RC */
		if (IS_ERR(opp)) { /* PCI/NVMe: max OPP lookup failure aborts NVMe probe */
			ret = PTR_ERR(opp);
			dev_err_probe(pci->dev, ret,
				      "Unable to find max freq OPP\n");
			goto err_pm_runtime_put;
		} else {
			ret = dev_pm_opp_set_opp(dev, opp); /* PCI/NVMe: apply max OPP for NVMe link bring-up */
		}

		dev_pm_opp_put(opp); /* PCI/NVMe: release max OPP reference */
		if (ret) { /* PCI/NVMe: max OPP apply failure aborts NVMe probe */
			dev_err_probe(pci->dev, ret,
				      "Failed to set OPP for freq %lu\n",
				      max_freq);
			goto err_pm_runtime_put;
		}

		pcie->use_pm_opp = true; /* PCI/NVMe: remember that OPP is used for NVMe bandwidth/power */
	} else { /* PCI/NVMe: no OPP table; use ICC for NVMe bandwidth voting */
		/* Skip ICC init if OPP is supported as it is handled by OPP */
		ret = qcom_pcie_icc_init(pcie); /* PCI/NVMe: initialize ICC paths for NVMe DMA/config traffic */
		if (ret) /* PCI/NVMe: ICC init failure aborts NVMe probe */
			goto err_pm_runtime_put;
	}

	ret = pcie->cfg->ops->get_resources(pcie); /* PCI/NVMe: acquire clocks/resets/regulators for NVMe RC */
	if (ret) /* PCI/NVMe: resource acquisition failure aborts NVMe probe */
		goto err_pm_runtime_put;

	pp->ops = &qcom_pcie_dw_ops; /* PCI/NVMe: register Qcom host ops with DesignWare core for NVMe */

	ret = qcom_pcie_parse_ports(pcie); /* PCI/NVMe: parse DT ports/PERST/PHY for NVMe */
	if (ret) { /* PCI/NVMe: port parse failure may fall back to legacy binding */
		if (ret != -ENODEV) { /* PCI/NVMe: real parse error aborts NVMe probe */
			dev_err_probe(pci->dev, ret,
				      "Failed to parse Root Port: %d\n", ret);
			goto err_pm_runtime_put;
		}

		/*
		 * In the case of properties not populated in Root Port node,
		 * fallback to the legacy method of parsing the Host Bridge
		 * node. This is to maintain DT backwards compatibility.
		 */
		ret = qcom_pcie_parse_legacy_binding(pcie); /* PCI/NVMe: fallback to legacy DT binding for NVMe */
		if (ret) /* PCI/NVMe: legacy binding failure aborts NVMe probe */
			goto err_pm_runtime_put;
	}

	platform_set_drvdata(pdev, pcie); /* PCI/NVMe: store qcom_pcie in platform device for suspend/resume */

	ret = dw_pcie_host_init(pp); /* PCI/NVMe: initialize DesignWare host and enumerate NVMe devices */
	if (ret) { /* PCI/NVMe: host init failure triggers PHY cleanup */
		dev_err_probe(dev, ret, "cannot initialize host\n");
		goto err_phy_exit;
	}

	qcom_pcie_icc_opp_update(pcie); /* PCI/NVMe: update ICC/OPP bandwidth based on negotiated NVMe link speed/width */

	if (pcie->mhi) /* PCI/NVMe: create debugfs only if MHI region available for NVMe ASPM counters */
		qcom_pcie_init_debugfs(pcie); /* PCI/NVMe: expose NVMe link transition counters in debugfs */

	return 0; /* PCI/NVMe: NVMe RC probe and enumeration complete */

err_phy_exit:
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) { /* PCI/NVMe: cleanup parsed ports on probe failure */
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list) /* PCI/NVMe: cleanup PERST# entries */
			list_del(&perst->list);
		phy_exit(port->phy); /* PCI/NVMe: exit PHY for failed NVMe port */
		list_del(&port->list);
	}
err_pm_runtime_put:
	pm_runtime_put(dev); /* PCI/NVMe: release runtime PM reference on NVMe probe failure */
	pm_runtime_disable(dev); /* PCI/NVMe: disable runtime PM on NVMe probe failure */

	return ret; /* PCI/NVMe: return original probe error */
}

static int qcom_pcie_suspend_noirq(struct device *dev)
{
	struct qcom_pcie *pcie;				/* PCI/NVMe: Qualcomm wrapper for NVMe RC */
	int ret = 0;						/* PCI/NVMe: suspend result */

	pcie = dev_get_drvdata(dev); /* PCI/NVMe: retrieve qcom_pcie from device for NVMe suspend */
	if (!pcie) /* PCI/NVMe: no driver data means nothing to suspend for NVMe */
		return 0;

	/*
	 * Set minimum bandwidth required to keep data path functional during
	 * suspend.
	 */
	if (pcie->icc_mem) { /* PCI/NVMe: reduce NVMe DMA interconnect bandwidth during suspend */
		ret = icc_set_bw(pcie->icc_mem, 0, kBps_to_icc(1)); /* PCI/NVMe: vote 1KBps to keep NVMe path alive */
		if (ret) { /* PCI/NVMe: bandwidth reduction failure aborts NVMe suspend */
			dev_err(dev,
				"Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret);
			return ret;
		}
	}

	/*
	 * Turn OFF the resources only for controllers without active PCIe
	 * devices. For controllers with active devices, the resources are kept
	 * ON and the link is expected to be in L0/L1 (sub)states.
	 *
	 * Turning OFF the resources for controllers with active PCIe devices
	 * will trigger access violation during the end of the suspend cycle,
	 * as kernel tries to access the PCIe devices config space for masking
	 * MSIs.
	 *
	 * Also, it is not desirable to put the link into L2/L3 state as that
	 * implies VDD supply will be removed and the devices may go into
	 * powerdown state. This will affect the lifetime of the storage devices
	 * like NVMe.
	 */
	if (!dw_pcie_link_up(pcie->pci)) { /* PCI/NVMe: only power down NVMe RC if link is already down */
		qcom_pcie_host_deinit(&pcie->pci->pp); /* PCI/NVMe: deinit NVMe RC during suspend */
		pcie->suspended = true; /* PCI/NVMe: mark RC as suspended for NVMe resume */
	}

	/*
	 * Only disable CPU-PCIe interconnect path if the suspend is non-S2RAM.
	 * Because on some platforms, DBI access can happen very late during the
	 * S2RAM and a non-active CPU-PCIe interconnect path may lead to NoC
	 * error.
	 */
	if (pm_suspend_target_state != PM_SUSPEND_MEM) { /* PCI/NVMe: avoid CPU path disable during S2RAM for NVMe safety */
		ret = icc_disable(pcie->icc_cpu); /* PCI/NVMe: disable CPU-NVMe config path when not entering S2RAM */
		if (ret) /* PCI/NVMe: CPU path disable failure is logged but not fatal */
			dev_err(dev, "Failed to disable CPU-PCIe interconnect path: %d\n", ret);

		if (pcie->use_pm_opp) /* PCI/NVMe: drop OPP when not entering S2RAM for NVMe power saving */
			dev_pm_opp_set_opp(pcie->pci->dev, NULL);
	}
	return ret; /* PCI/NVMe: suspend complete */
}

static int qcom_pcie_resume_noirq(struct device *dev)
{
	struct qcom_pcie *pcie;				/* PCI/NVMe: Qualcomm wrapper for NVMe RC */
	int ret;						/* PCI/NVMe: resume result */

	pcie = dev_get_drvdata(dev); /* PCI/NVMe: retrieve qcom_pcie from device for NVMe resume */
	if (!pcie) /* PCI/NVMe: no driver data means nothing to resume for NVMe */
		return 0;

	if (pm_suspend_target_state != PM_SUSPEND_MEM) { /* PCI/NVMe: re-enable CPU path only for non-S2RAM NVMe resume */
		ret = icc_enable(pcie->icc_cpu); /* PCI/NVMe: enable CPU-NVMe config path */
		if (ret) { /* PCI/NVMe: CPU path enable failure aborts NVMe resume */
			dev_err(dev, "Failed to enable CPU-PCIe interconnect path: %d\n", ret);
			return ret;
		}
	}

	if (pcie->suspended) { /* PCI/NVMe: RC was powered down; re-init NVMe link */
		ret = qcom_pcie_host_init(&pcie->pci->pp); /* PCI/NVMe: reinitialize NVMe host during resume */
		if (ret) /* PCI/NVMe: host re-init failure aborts NVMe resume */
			return ret;

		pcie->suspended = false; /* PCI/NVMe: mark RC as resumed for NVMe */
	}

	qcom_pcie_icc_opp_update(pcie); /* PCI/NVMe: restore ICC/OPP bandwidth for resumed NVMe link */

	return 0; /* PCI/NVMe: resume complete */
}

static const struct of_device_id qcom_pcie_match[] = {
	{ .compatible = "qcom,pcie-apq8064", .data = &cfg_2_1_0 }, /* PCI/NVMe: APQ8064 NVMe RC config */
	{ .compatible = "qcom,pcie-apq8084", .data = &cfg_1_0_0 }, /* PCI/NVMe: APQ8084 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq4019", .data = &cfg_2_4_0 }, /* PCI/NVMe: IPQ4019 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq5018", .data = &cfg_2_9_0 }, /* PCI/NVMe: IPQ5018 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq6018", .data = &cfg_2_9_0 }, /* PCI/NVMe: IPQ6018 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq8064", .data = &cfg_2_1_0 }, /* PCI/NVMe: IPQ8064 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq8064-v2", .data = &cfg_2_1_0 }, /* PCI/NVMe: IPQ8064 v2 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq8074", .data = &cfg_2_3_3 }, /* PCI/NVMe: IPQ8074 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq8074-gen3", .data = &cfg_2_9_0 }, /* PCI/NVMe: IPQ8074 Gen3 NVMe RC config */
	{ .compatible = "qcom,pcie-ipq9574", .data = &cfg_2_9_0 }, /* PCI/NVMe: IPQ9574 NVMe RC config */
	{ .compatible = "qcom,pcie-msm8996", .data = &cfg_2_3_2 }, /* PCI/NVMe: MSM8996 NVMe RC config */
	{ .compatible = "qcom,pcie-qcs404", .data = &cfg_2_4_0 }, /* PCI/NVMe: QCS404 NVMe RC config */
	{ .compatible = "qcom,pcie-sa8255p", .data = &cfg_fw_managed }, /* PCI/NVMe: SA8255P firmware-managed NVMe RC */
	{ .compatible = "qcom,pcie-sa8540p", .data = &cfg_sc8280xp }, /* PCI/NVMe: SA8540P NVMe RC config */
	{ .compatible = "qcom,pcie-sa8775p", .data = &cfg_1_34_0}, /* PCI/NVMe: SA8775P NVMe RC config */
	{ .compatible = "qcom,pcie-sc7280", .data = &cfg_1_9_0 }, /* PCI/NVMe: SC7280 NVMe RC config */
	{ .compatible = "qcom,pcie-sc8180x", .data = &cfg_1_9_0 }, /* PCI/NVMe: SC8180X NVMe RC config */
	{ .compatible = "qcom,pcie-sc8280xp", .data = &cfg_sc8280xp }, /* PCI/NVMe: SC8280XP NVMe RC config */
	{ .compatible = "qcom,pcie-sdm845", .data = &cfg_2_7_0 }, /* PCI/NVMe: SDM845 NVMe RC config */
	{ .compatible = "qcom,pcie-sdx55", .data = &cfg_1_9_0 }, /* PCI/NVMe: SDX55 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8150", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8150 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8250", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8250 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8350", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8350 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8450-pcie0", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8450 pcie0 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8450-pcie1", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8450 pcie1 NVMe RC config */
	{ .compatible = "qcom,pcie-sm8550", .data = &cfg_1_9_0 }, /* PCI/NVMe: SM8550 NVMe RC config */
	{ .compatible = "qcom,pcie-x1e80100", .data = &cfg_sc8280xp }, /* PCI/NVMe: X1E80100 NVMe RC config */
	{ }
};

static void qcom_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL; /* PCI/NVMe: ensure Qcom root port is classified as PCI bridge for NVMe enumeration */
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0101, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x0101 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0104, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x0104 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0106, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x0106 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0107, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x0107 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0302, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x0302 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1000, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x1000 NVMe RP */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1001, qcom_fixup_class); /* PCI/NVMe: fixup class for QCOM 0x1001 NVMe RP */

static const struct dev_pm_ops qcom_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(qcom_pcie_suspend_noirq, qcom_pcie_resume_noirq) /* PCI/NVMe: noirq suspend/resume callbacks for NVMe RC */
};

static struct platform_driver qcom_pcie_driver = {
	.probe = qcom_pcie_probe, /* PCI/NVMe: probe Qualcomm PCIe RC for NVMe host */
	.driver = {
		.name = "qcom-pcie", /* PCI/NVMe: platform driver name for NVMe RC */
		.suppress_bind_attrs = true, /* PCI/NVMe: disable manual bind/unbind to protect NVMe link state */
		.of_match_table = qcom_pcie_match, /* PCI/NVMe: DT match table for NVMe-capable Qualcomm SoCs */
		.pm = &qcom_pcie_pm_ops, /* PCI/NVMe: power management ops for NVMe suspend/resume */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS, /* PCI/NVMe: async probe for faster NVMe host enumeration */
	},
};
builtin_platform_driver(qcom_pcie_driver); /* PCI/NVMe: register Qualcomm PCIe RC driver for NVMe hosts */
