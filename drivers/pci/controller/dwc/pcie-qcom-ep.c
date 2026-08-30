// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm PCIe Endpoint controller driver
 *
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Author: Siddartha Mohanadoss <smohanad@codeaurora.org
 *
 * Copyright (c) 2021, Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org
 */

#include <linux/clk.h>          /* NVMe: clocks gate PCIe controller; host sees link down if clocks off */
#include <linux/debugfs.h>      /* PCI/NVMe: exposes ASPM L0s/L1/L1.1/L1.2/L2 transition counters for debug */
#include <linux/delay.h>        /* NVMe: reset/wake timing affects when host can enumerate the device */
#include <linux/gpio/consumer.h> /* PCI/NVMe: PERST# from host resets EP; WAKE# alerts host for re-enumeration */
#include <linux/interconnect.h> /* NVMe: sets bus bandwidth for DMA traffic between EP and host memory */
#include <linux/mfd/syscon.h>   /* NVMe: TCSR syscon controls PERST separation so host reboot does not lose EP */
#include <linux/phy/pcie.h>     /* PCI/NVMe: PHY mode configures EP role seen by host during link training */
#include <linux/phy/phy.h>      /* NVMe: PHY init/power-on must succeed before host can detect link up */
#include <linux/platform_device.h> /* NVMe: platform device represents this endpoint to the SoC */
#include <linux/pm_domain.h>    /* NVMe: power domain resume needed before host PERST deassert handling */
#include <linux/regmap.h>       /* NVMe: regmap writes PERST separation bits for host-friendly hotplug */
#include <linux/reset.h>        /* NVMe: core reset asserts then deasserts so host sees a clean EP */
#include <linux/module.h>       /* NVMe: module registration; builtin because NVMe root complex needs it early */

#include "../../pci.h"          /* NVMe: shared PCI host/endpoint definitions, capability IDs, etc. */
#include "pcie-designware.h"    /* NVMe: DesignWare core; host drivers rely on DWC EP to expose BARs/MSI */
#include "pcie-qcom-common.h"   /* NVMe: Qualcomm common equalization settings for reliable link training */

/* PARF registers */
#define PARF_SYS_CTRL				0x00    /* PCI/NVMe: system control; affects AUX power reporting to host */
#define PARF_DB_CTRL				0x10    /* NVMe: debounce control for PERST/link signals from host */
#define PARF_PM_CTRL				0x20    /* PCI/NVMe: PM control; handles host PM_TURNOFF and L1/L23 entry */
#define PARF_MHI_CLOCK_RESET_CTRL		0x174   /* NVMe: MHI bus clock gating; MHI is often used for NVMe-oF/modem */
#define PARF_MHI_BASE_ADDR_LOWER		0x178   /* NVMe: low 32 bits of BAR/MMIO base exposed to host */
#define PARF_MHI_BASE_ADDR_UPPER		0x17c   /* NVMe: high 32 bits of BAR/MMIO base for 64-bit host access */
#define PARF_DEBUG_INT_EN			0x190   /* NVMe: enables BME/PM/D-state events host triggers on EP */
#define PARF_AXI_MSTR_RD_HALT_NO_WRITES		0x1a4   /* NVMe: DMA read ordering relative to writes from host */
#define PARF_AXI_MSTR_WR_ADDR_HALT		0x1a8   /* NVMe: write-after-write ordering for EP DMA to host memory */
#define PARF_Q2A_FLUSH				0x1ac   /* NVMe: flush control for DMA coherency before host access */
#define PARF_LTSSM				0x1b0   /* PCI/NVMe: link training state machine; bit 8 enables link so host enumerates */
#define PARF_CFG_BITS				0x210   /* PCI/NVMe: exit L1SS on MSI/LTR messages so host IRQs/LTR work */
#define PARF_INT_ALL_STATUS			0x224   /* NVMe: aggregated interrupt status; host actions generate these */
#define PARF_INT_ALL_CLEAR			0x228   /* NVMe: write 1s to clear status after handling host events */
#define PARF_INT_ALL_MASK			0x22c   /* NVMe: mask/unmask link up/down/BME/PM events from host */
#define PARF_SLV_ADDR_MSB_CTRL			0x2c0   /* NVMe: controls MSB of slave address for host BAR access */
#define PARF_DBI_BASE_ADDR			0x350   /* NVMe: DBI aperture base low; host config space is via DBI */
#define PARF_DBI_BASE_ADDR_HI			0x354   /* NVMe: DBI aperture base high for 64-bit host config access */
#define PARF_SLV_ADDR_SPACE_SIZE		0x358   /* NVMe: size of address space host sees through BARs */
#define PARF_SLV_ADDR_SPACE_SIZE_HI		0x35c   /* NVMe: high size for 64-bit BARs exposed to host */
#define PARF_NO_SNOOP_OVERRIDE			0x3d4   /* NVMe: overrides TLP no-snoop attribute; affects host DMA coherency with IOMMU */
#define PARF_ATU_BASE_ADDR			0x634   /* NVMe: ATU (Address Translation Unit) base; maps EP DMA to host memory */
#define PARF_ATU_BASE_ADDR_HI			0x638   /* NVMe: high ATU base for 64-bit host DMA addressing */
#define PARF_SRIS_MODE				0x644   /* NVMe: Separate Refclk Independent SSC mode for high-speed host link */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L2		0xc04   /* NVMe: debug counter for L2 entry (host-initiated power down) */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L1		0xc0c   /* NVMe: debug counter for ASPM L1 entry negotiated with host */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L0S		0xc10   /* NVMe: debug counter for ASPM L0s entry seen from host */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1	0xc84   /* NVMe: debug counter for L1.1 ASPM substate with host */
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2	0xc88   /* NVMe: debug counter for L1.2 ASPM substate with host */
#define PARF_DEVICE_TYPE			0x1000  /* NVMe: selects endpoint mode so host enumerates this as a device */
#define PARF_BDF_TO_SID_CFG			0x2c00  /* NVMe: BDF to stream-ID; bypass lets host BDF reach EP directly */
#define PARF_INT_ALL_5_MASK			0x2dcc  /* NVMe: extended interrupt mask 5; MHI RAM parity related */
#define PARF_INT_ALL_3_MASK			0x2e18  /* NVMe: extended interrupt mask 3; PTM updating related */

/* PARF_INT_ALL_{STATUS/CLEAR/MASK} register fields */
#define PARF_INT_ALL_LINK_DOWN			BIT(1)  /* NVMe: host saw link down; EP must notify function drivers */
#define PARF_INT_ALL_BME			BIT(2)  /* NVMe: host set PCI_COMMAND_MASTER (Bus Master Enable); DMA allowed */
#define PARF_INT_ALL_PM_TURNOFF			BIT(3)  /* NVMe: host sent PM_Turn_Off; EP enters low power, host may remove power */
#define PARF_INT_ALL_DEBUG			BIT(4)  /* NVMe: debug interrupt aggregator status */
#define PARF_INT_ALL_LTR			BIT(5)  /* NVMe: Latency Tolerance Reporting message from host */
#define PARF_INT_ALL_MHI_Q6			BIT(6)  /* NVMe: MHI Q6 interrupt (platform specific) */
#define PARF_INT_ALL_MHI_A7			BIT(7)  /* NVMe: MHI A7 interrupt (platform specific) */
#define PARF_INT_ALL_DSTATE_CHANGE		BIT(8)  /* NVMe: host changed PCI power state (D-state) */
#define PARF_INT_ALL_L1SUB_TIMEOUT		BIT(9)  /* NVMe: L1SS timeout during host-EP ASPM negotiation */
#define PARF_INT_ALL_MMIO_WRITE			BIT(10) /* NVMe: host wrote EP MMIO space */
#define PARF_INT_ALL_CFG_WRITE			BIT(11) /* NVMe: host wrote EP config space (e.g., COMMAND/MSI/PCIE_CAP) */
#define PARF_INT_ALL_BRIDGE_FLUSH_N		BIT(12) /* NVMe: bridge flush notification from host */
#define PARF_INT_ALL_LINK_UP			BIT(13) /* NVMe: link training complete; host can now enumerate/config the EP */
#define PARF_INT_ALL_AER_LEGACY			BIT(14) /* NVMe: AER legacy interrupt forwarded from host/RC */
#define PARF_INT_ALL_PLS_ERR			BIT(15) /* NVMe: physical layer error; may trigger AER on host side */
#define PARF_INT_ALL_PME_LEGACY			BIT(16) /* NVMe: PME legacy interrupt to wake host */
#define PARF_INT_ALL_PLS_PME			BIT(17) /* NVMe: physical layer PME event to wake host */
#define PARF_INT_ALL_EDMA			BIT(22) /* NVMe: eDMA interrupt; used for NVMe data movement offload */

/* PARF_BDF_TO_SID_CFG register fields */
#define PARF_BDF_TO_SID_BYPASS			BIT(0)  /* NVMe: bypass BDF->SID translation so host BDF is preserved for IOMMU */

/* PARF_DEBUG_INT_EN register fields */
#define PARF_DEBUG_INT_PM_DSTATE_CHANGE		BIT(1)  /* NVMe: interrupt on host D-state change (D0/D3) */
#define PARF_DEBUG_INT_CFG_BUS_MASTER_EN	BIT(2)  /* NVMe: interrupt when host enables Bus Master in PCI_COMMAND */
#define PARF_DEBUG_INT_RADM_PM_TURNOFF		BIT(3)  /* NVMe: interrupt when host sends PM_Turn_Off message */

/* PARF_NO_SNOOP_OVERRIDE register fields */
#define WR_NO_SNOOP_OVERRIDE_EN			BIT(1)  /* NVMe: force snoop on EP writes to host memory (IOMMU/coherency) */
#define RD_NO_SNOOP_OVERRIDE_EN			BIT(3)  /* NVMe: force snoop on EP reads from host memory */

/* PARF_DEVICE_TYPE register fields */
#define PARF_DEVICE_TYPE_EP			0x0     /* NVMe: configure controller as PCIe Endpoint so host enumerates it */

/* PARF_PM_CTRL register fields */
#define PARF_PM_CTRL_REQ_EXIT_L1		BIT(1)  /* NVMe: request link exit from ASPM L1 when host wakes EP */
#define PARF_PM_CTRL_READY_ENTR_L23		BIT(2)  /* NVMe: acknowledge host PM_Turn_Off, ready to enter L2/L3-Ready */
#define PARF_PM_CTRL_REQ_NOT_ENTR_L1		BIT(5)  /* NVMe: block ASPM L1 entry if host tries too early */

/* PARF_MHI_CLOCK_RESET_CTRL fields */
#define PARF_MSTR_AXI_CLK_EN			BIT(1)  /* NVMe: enable MHI master AXI clock; gate in L1SS to save power */

/* PARF_AXI_MSTR_RD_HALT_NO_WRITES register fields */
#define PARF_AXI_MSTR_RD_HALT_NO_WRITE_EN	BIT(0)  /* NVMe: ordering rule for EP DMA reads vs writes to host memory */

/* PARF_AXI_MSTR_WR_ADDR_HALT register fields */
#define PARF_AXI_MSTR_WR_ADDR_HALT_EN		BIT(31) /* NVMe: ordering rule for consecutive EP DMA writes to host */

/* PARF_Q2A_FLUSH register fields */
#define PARF_Q2A_FLUSH_EN			BIT(16) /* NVMe: enable flush from queue to AXI; disabled for normal DMA flow */

/* PARF_SYS_CTRL register fields */
#define PARF_SYS_CTRL_AUX_PWR_DET		BIT(4)  /* NVMe: report Vaux present to host, required for PME wake from D3cold */
#define PARF_SYS_CTRL_CORE_CLK_CGC_DIS		BIT(6)  /* NVMe: disable core clock gating so PIPE clock reaches core for host link */
#define PARF_SYS_CTRL_MSTR_ACLK_CGC_DIS		BIT(10) /* NVMe: gate master AXI clock in idle; host DMA wakes it via L1 exit */
#define PARF_SYS_CTRL_SLV_DBI_WAKE_DISABLE	BIT(11) /* NVMe: do not wake link from DBI/slave access, host must use PERST/WAKE */

/* PARF_DB_CTRL register fields */
#define PARF_DB_CTRL_INSR_DBNCR_BLOCK		BIT(0)  /* NVMe: block insertion debouncer so host PERST is not filtered */
#define PARF_DB_CTRL_RMVL_DBNCR_BLOCK		BIT(1)  /* NVMe: block removal debouncer for host hotplug */
#define PARF_DB_CTRL_DBI_WKP_BLOCK		BIT(4)  /* NVMe: block DBI wake debounce; host config access does not wake link */
#define PARF_DB_CTRL_SLV_WKP_BLOCK		BIT(5)  /* NVMe: block slave wake debounce for host memory/MMIO access */
#define PARF_DB_CTRL_MST_WKP_BLOCK		BIT(6)  /* NVMe: block master wake debounce for EP DMA to host */

/* PARF_CFG_BITS register fields */
#define PARF_CFG_BITS_REQ_EXIT_L1SS_MSI_LTR_EN	BIT(1)  /* NVMe: exit ASPM L1SS to send MSI/MSI-X and LTR to host */

/* PARF_INT_ALL_5_MASK fields */
#define PARF_INT_ALL_5_MHI_RAM_DATA_PARITY_ERR	BIT(0)  /* NVMe: MHI RAM parity error; masked on platforms with unreliable MHI RAM */

/* PARF_INT_ALL_3_MASK fields */
#define PARF_INT_ALL_3_PTM_UPDATING		BIT(4)  /* NVMe: PTM (Precision Time Measurement) update interrupt from host */

/* ELBI registers */
#define ELBI_SYS_STTS				0x08    /* NVMe: ELBI system status; XMLH_LINK_UP tells if host link is up */
#define ELBI_CS2_ENABLE				0xa4    /* NVMe: DBI2 shadow register access enable for writing RO config fields */

/* DBI registers */
#define DBI_CON_STATUS				0x44    /* NVMe: DBI config status; holds current PCI power state (D-state) */

/* DBI register fields */
#define DBI_CON_STATUS_POWER_STATE_MASK		GENMASK(1, 0)   /* NVMe: masks D-state bits host wrote via PMCSR */

#define XMLH_LINK_UP				0x400   /* NVMe: bit in ELBI_SYS_STTS indicating host link is trained */
#define CORE_RESET_TIME_US_MIN			1000    /* NVMe: minimum reset assertion time before host can re-enumerate */
#define CORE_RESET_TIME_US_MAX			1005    /* NVMe: maximum reset assertion time */
#define WAKE_DELAY_US				2000 /* 2 ms */ /* NVMe: WAKE# pulse width host expects before enumeration */

#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
			Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed)))   /* NVMe: convert PCIe speed to interconnect bandwidth for DMA */

#define to_pcie_ep(x)				dev_get_drvdata((x)->dev)   /* NVMe: retrieve qcom_pcie_ep from dw_pcie dev drvdata */

enum qcom_pcie_ep_link_status {
	QCOM_PCIE_EP_LINK_DISABLED,     /* NVMe: link disabled, host cannot access device */
	QCOM_PCIE_EP_LINK_ENABLED,      /* NVMe: host enabled BME; DMA and config access possible */
	QCOM_PCIE_EP_LINK_UP,           /* NVMe: link trained, host enumeration/config can proceed */
	QCOM_PCIE_EP_LINK_DOWN,         /* NVMe: link lost; host will see device unavailable */
};

/**
 * struct qcom_pcie_ep_cfg - Per SoC config struct
 * @hdma_support: HDMA support on this SoC
 * @override_no_snoop: Override NO_SNOOP attribute in TLP to enable cache snooping
 * @disable_mhi_ram_parity_check: Disable MHI RAM data parity error check
 * @firmware_managed: Set if the controller is firmware managed
 */
struct qcom_pcie_ep_cfg {
	bool hdma_support;              /* NVMe: enables embedded DMA used for NVMe data path offload */
	bool override_no_snoop;         /* NVMe: force TLP snoop so host memory is coherent for NVMe DMA */
	bool disable_mhi_ram_parity_check; /* NVMe: mask MHI RAM parity IRQ that could spuriously interrupt host flow */
	bool firmware_managed;          /* NVMe: firmware owns PHY/clk/reset; host sees normal EP behavior */
};

/**
 * struct qcom_pcie_ep - Qualcomm PCIe Endpoint Controller
 * @pci: Designware PCIe controller struct
 * @parf: Qualcomm PCIe specific PARF register base
 * @mmio: MMIO register base
 * @perst_map: PERST regmap
 * @mmio_res: MMIO region resource
 * @core_reset: PCIe Endpoint core reset
 * @reset: PERST# GPIO
 * @wake: WAKE# GPIO
 * @phy: PHY controller block
 * @debugfs: PCIe Endpoint Debugfs directory
 * @icc_mem: Handle to an interconnect path between PCIe and MEM
 * @clks: PCIe clocks
 * @num_clks: PCIe clocks count
 * @perst_en: Flag for PERST enable
 * @perst_sep_en: Flag for PERST separation enable
 * @cfg: PCIe EP config struct
 * @link_status: PCIe Link status
 * @global_irq: Qualcomm PCIe specific Global IRQ
 * @perst_irq: PERST# IRQ
 */
struct qcom_pcie_ep {
	struct dw_pcie pci;             /* NVMe: DWC core; host config space/BARs/MSI are handled here */

	void __iomem *parf;             /* NVMe: Qualcomm PARF registers for link/PM/interrupt control from host */
	void __iomem *mmio;             /* NVMe: MMIO region mapped to host BAR space for NVMe register access */
	struct regmap *perst_map;       /* NVMe: TCSR regmap to delatch PERST so host reboot keeps device alive */
	struct resource *mmio_res;      /* NVMe: physical MMIO resource backing BAR exposed to host */

	struct reset_control *core_reset; /* NVMe: resets EP core so host sees a fresh PCIe device */
	struct gpio_desc *reset;        /* NVMe: PERST# input from host; asserted during host reset/hotplug */
	struct gpio_desc *wake;         /* NVMe: WAKE# output to host; used for PME and re-enumeration */
	struct phy *phy;                /* NVMe: PCIe PHY; must be on for host to detect link up */
	struct dentry *debugfs;         /* NVMe: debugfs directory with ASPM transition counters */

	struct icc_path *icc_mem;       /* NVMe: interconnect bandwidth for EP DMA to host DRAM */

	struct clk_bulk_data *clks;     /* NVMe: bulk clock handles; clocks must be on for host link */
	int num_clks;                   /* NVMe: number of clocks required for link/DMA */

	u32 perst_en;                   /* NVMe: TCSR offset enabling PERST propagation to EP from host */
	u32 perst_sep_en;               /* NVMe: TCSR offset enabling PERST separation on host reboot/hibernate */

	const struct qcom_pcie_ep_cfg *cfg; /* NVMe: SoC-specific config (HDMA, no-snoop, firmware-managed) */
	enum qcom_pcie_ep_link_status link_status; /* NVMe: current link state visible to host */
	int global_irq;                 /* NVMe: IRQ for aggregated link/PM/BME events triggered by host */
	int perst_irq;                  /* NVMe: IRQ for host PERST# assertion/deassertion (hotplug/reset) */
};

static int qcom_pcie_ep_core_reset(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: get DWC controller for logging */
	struct device *dev = pci->dev;          /* NVMe: device used for error messages */
	int ret;                                /* NVMe: return value tracking */

	ret = reset_control_assert(pcie_ep->core_reset);    /* NVMe: put EP in reset so host loses link temporarily */
	if (ret) {                          /* NVMe: if assert fails, host may see a stuck device */
		dev_err(dev, "Cannot assert core reset\n");
		return ret;
	}

	usleep_range(CORE_RESET_TIME_US_MIN, CORE_RESET_TIME_US_MAX);   /* NVMe: hold reset long enough for host RC to detect link down */

	ret = reset_control_deassert(pcie_ep->core_reset);  /* NVMe: release reset; host can retrain and enumerate EP */
	if (ret) {                          /* NVMe: deassert failure leaves device invisible to host */
		dev_err(dev, "Cannot de-assert core reset\n");
		return ret;
	}

	usleep_range(CORE_RESET_TIME_US_MIN, CORE_RESET_TIME_US_MAX);   /* NVMe: wait for EP to come out of reset before host access */

	return 0;                           /* NVMe: core reset completed; device ready for host enumeration */
}

/*
 * Delatch PERST_EN and PERST_SEPARATION_ENABLE with TCSR to avoid
 * device reset during host reboot and hibernation. The driver is
 * expected to handle this situation.
 */
static void qcom_pcie_ep_configure_tcsr(struct qcom_pcie_ep *pcie_ep)
{
	if (pcie_ep->perst_map) {       /* NVMe: only if TCSR syscon is present in DT */
		regmap_write(pcie_ep->perst_map, pcie_ep->perst_en, 0);     /* NVMe: disable hardware PERST propagation so host reboot does not reset EP */
		regmap_write(pcie_ep->perst_map, pcie_ep->perst_sep_en, 0); /* NVMe: disable PERST separation so driver manages hotplug */
	}
}

static bool qcom_pcie_dw_link_up(struct dw_pcie *pci)
{
	u32 reg;                        /* NVMe: holds ELBI status read */

	reg = readl_relaxed(pci->elbi_base + ELBI_SYS_STTS);    /* NVMe: read ELBI system status from host link */

	return reg & XMLH_LINK_UP;      /* NVMe: true if PHY/link layer reports link up to host */
}

static int qcom_pcie_dw_start_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* NVMe: back pointer to qcom EP state */

	enable_irq(pcie_ep->perst_irq); /* NVMe: allow host PERST# to trigger EP link start/reset handling */

	return 0;                       /* NVMe: link start ready; actual training happens on PERST deassert */
}

static void qcom_pcie_dw_stop_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* NVMe: back pointer to qcom EP state */

	disable_irq(pcie_ep->perst_irq); /* NVMe: ignore host PERST# while stopping link to avoid races */
}

static void qcom_pcie_dw_write_dbi2(struct dw_pcie *pci, void __iomem *base,
				    u32 reg, size_t size, u32 val)
{
	int ret;                        /* NVMe: write return code */

	writel(1, pci->elbi_base + ELBI_CS2_ENABLE);    /* NVMe: enable DBI2 shadow register write path for RO config fields */

	ret = dw_pcie_write(pci->dbi_base2 + reg, size, val);   /* NVMe: write config space field host will later read */
	if (ret)                        /* NVMe: if write fails, host may read stale/invalid capability values */
		dev_err(pci->dev, "Failed to write DBI2 register (0x%x): %d\n", reg, ret);

	writel(0, pci->elbi_base + ELBI_CS2_ENABLE);    /* NVMe: disable DBI2 shadow write; normal config access from host resumes */
}

static void qcom_pcie_ep_icc_update(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller pointer */
	u32 offset, status;             /* NVMe: capability offset and PCI_EXP_LNKSTA register value */
	int speed, width;               /* NVMe: negotiated link speed and width from host */
	int ret;                        /* NVMe: icc_set_bw return value */

	if (!pcie_ep->icc_mem)          /* NVMe: skip if no interconnect path defined for this SoC */
		return;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);  /* NVMe: find PCIe capability offset in EP config space */
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA); /* NVMe: read link status written by host enumeration/training */

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status);  /* NVMe: current link speed negotiated with host */
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status);  /* NVMe: current link width negotiated with host */

	ret = icc_set_bw(pcie_ep->icc_mem, 0, width * QCOM_PCIE_LINK_SPEED_TO_BW(speed)); /* NVMe: scale SoC interconnect bandwidth to match host link for DMA */
	if (ret)                        /* NVMe: bandwidth request failure may throttle NVMe DMA throughput */
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret);
}

static int qcom_pcie_enable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller for error logging */
	int ret;                        /* NVMe: return code */

	ret = clk_bulk_prepare_enable(pcie_ep->num_clks, pcie_ep->clks);    /* NVMe: enable PCIe clocks so host can detect EP */
	if (ret)                        /* NVMe: clocks off means host sees no link/device */
		return ret;

	ret = qcom_pcie_ep_core_reset(pcie_ep); /* NVMe: reset then release EP core for clean host enumeration */
	if (ret)                        /* NVMe: reset failure prevents host from seeing device */
		goto err_disable_clk;

	ret = phy_init(pcie_ep->phy);   /* NVMe: initialize PCIe PHY for host link training */
	if (ret)                        /* NVMe: PHY init failure means no link to host */
		goto err_disable_clk;

	ret = phy_set_mode_ext(pcie_ep->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_EP);  /* NVMe: configure PHY as endpoint so host enumerates correctly */
	if (ret)                        /* NVMe: wrong PHY mode can cause host enumeration failure */
		goto err_phy_exit;

	ret = phy_power_on(pcie_ep->phy);   /* NVMe: power on PHY so TX/RX signals reach host */
	if (ret)                        /* NVMe: PHY off means host cannot detect link up */
		goto err_phy_exit;

	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(pcie_ep->icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1)); /* NVMe: initial Gen1 x1 bandwidth for early host config/DMA */
	if (ret) {                      /* NVMe: interconnect failure blocks DMA path to host memory */
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret);
		goto err_phy_off;
	}

	return 0;                       /* NVMe: all resources enabled; EP ready for host link training */

err_phy_off:
	phy_power_off(pcie_ep->phy);    /* NVMe: power down PHY on error; host loses link */
err_phy_exit:
	phy_exit(pcie_ep->phy);         /* NVMe: exit PHY state machine */
err_disable_clk:
	clk_bulk_disable_unprepare(pcie_ep->num_clks, pcie_ep->clks);  /* NVMe: disable clocks; host cannot access EP */

	return ret;                     /* NVMe: propagate first error to caller */
}

static void qcom_pcie_disable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = pcie_ep->pci.dev;  /* NVMe: device for pm_runtime */

	pm_runtime_put(dev);            /* NVMe: allow SoC to suspend EP power domain after host removes device */

	/* Skip resource disablement if controller is firmware-managed */
	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* NVMe: firmware owns PHY/clk on these SoCs; host still sees link down */
		return;

	icc_set_bw(pcie_ep->icc_mem, 0, 0); /* NVMe: tear down interconnect bandwidth for NVMe DMA to host */
	phy_power_off(pcie_ep->phy);    /* NVMe: turn off PHY; host detects link down */
	phy_exit(pcie_ep->phy);         /* NVMe: release PHY resources */
	clk_bulk_disable_unprepare(pcie_ep->num_clks, pcie_ep->clks);  /* NVMe: gate clocks; EP inaccessible to host */
}

static int qcom_pcie_perst_deassert(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* NVMe: qcom EP state */
	struct device *dev = pci->dev;  /* NVMe: device for power management */
	u32 val, offset;                /* NVMe: scratch register values and capability offset */
	int ret;                        /* NVMe: return code */

	ret = pm_runtime_resume_and_get(dev);   /* NVMe: ensure power domain is active before host access */
	if (ret < 0) {                  /* NVMe: power domain failure means host cannot enumerate EP */
		dev_err(dev, "Failed to enable device: %d\n", ret);
		return ret;
	}

	/* Skip resource enablement if controller is firmware-managed */
	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* NVMe: firmware already enabled PHY/clk; jump to EP config */
		goto skip_resources_enable;

	ret = qcom_pcie_enable_resources(pcie_ep);  /* NVMe: enable clocks/PHY/interconnect for host link */
	if (ret) {                      /* NVMe: resource failure prevents host enumeration */
		dev_err(dev, "Failed to enable resources: %d\n", ret);
		pm_runtime_put(dev);
		return ret;
	}

skip_resources_enable:
	/* Perform cleanup that requires refclk */
	pci_epc_deinit_notify(pci->ep.epc); /* NVMe: notify function drivers that previous host session ended */
	dw_pcie_ep_cleanup(&pci->ep);   /* NVMe: clear previous EP state (BARs, MSI) before new host enumeration */

	/* Assert WAKE# to RC to indicate device is ready */
	gpiod_set_value_cansleep(pcie_ep->wake, 1); /* NVMe: pulse WAKE# to get host RC attention for enumeration/PME */
	usleep_range(WAKE_DELAY_US, WAKE_DELAY_US + 500);   /* NVMe: hold WAKE# long enough for host to sample */
	gpiod_set_value_cansleep(pcie_ep->wake, 0); /* NVMe: deassert WAKE# after host is alerted */

	qcom_pcie_ep_configure_tcsr(pcie_ep);   /* NVMe: delatch PERST from TCSR so host reboot does not reset EP */

	/* Disable BDF to SID mapping */
	val = readl_relaxed(pcie_ep->parf + PARF_BDF_TO_SID_CFG);   /* NVMe: read current BDF->SID config */
	val |= PARF_BDF_TO_SID_BYPASS;  /* NVMe: bypass translation so host BDF reaches EP and IOMMU sees original RID */
	writel_relaxed(val, pcie_ep->parf + PARF_BDF_TO_SID_CFG);   /* NVMe: apply bypass for host enumeration */

	/* Enable debug IRQ */
	val = readl_relaxed(pcie_ep->parf + PARF_DEBUG_INT_EN); /* NVMe: read debug interrupt enable */
	val |= PARF_DEBUG_INT_RADM_PM_TURNOFF |     /* NVMe: notify EP when host sends PM_Turn_Off */
	       PARF_DEBUG_INT_CFG_BUS_MASTER_EN |   /* NVMe: notify EP when host enables Bus Master */
	       PARF_DEBUG_INT_PM_DSTATE_CHANGE;      /* NVMe: notify EP when host changes D-state */
	writel_relaxed(val, pcie_ep->parf + PARF_DEBUG_INT_EN); /* NVMe: enable host-driven PM/BME events */

	/* Configure PCIe to endpoint mode */
	writel_relaxed(PARF_DEVICE_TYPE_EP, pcie_ep->parf + PARF_DEVICE_TYPE);  /* NVMe: set device type=EP so host enumerates as function */

	/* Allow entering L1 state */
	val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: read PM control */
	val &= ~PARF_PM_CTRL_REQ_NOT_ENTR_L1;   /* NVMe: clear L1-block so host can negotiate ASPM L1 */
	writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: allow ASPM L1 power saving with host */

	/* Read halts write */
	val = readl_relaxed(pcie_ep->parf + PARF_AXI_MSTR_RD_HALT_NO_WRITES);   /* NVMe: read AXI ordering config */
	val &= ~PARF_AXI_MSTR_RD_HALT_NO_WRITE_EN;  /* NVMe: disable read-halt so EP DMA flows to host memory */
	writel_relaxed(val, pcie_ep->parf + PARF_AXI_MSTR_RD_HALT_NO_WRITES);   /* NVMe: apply ordering for NVMe DMA */

	/* Write after write halt */
	val = readl_relaxed(pcie_ep->parf + PARF_AXI_MSTR_WR_ADDR_HALT);    /* NVMe: read write-halt config */
	val |= PARF_AXI_MSTR_WR_ADDR_HALT_EN;   /* NVMe: enable write address halt for ordering of EP DMA writes */
	writel_relaxed(val, pcie_ep->parf + PARF_AXI_MSTR_WR_ADDR_HALT);    /* NVMe: apply write ordering */

	/* Q2A flush disable */
	val = readl_relaxed(pcie_ep->parf + PARF_Q2A_FLUSH);    /* NVMe: read Q2A flush control */
	val &= ~PARF_Q2A_FLUSH_EN;      /* NVMe: disable flush for normal DMA operation */
	writel_relaxed(val, pcie_ep->parf + PARF_Q2A_FLUSH);    /* NVMe: apply Q2A flush setting */

	/*
	 * Disable Master AXI clock during idle.  Do not allow DBI access
	 * to take the core out of L1.  Disable core clock gating that
	 * gates PIPE clock from propagating to core clock.  Report to the
	 * host that Vaux is present.
	 */
	val = readl_relaxed(pcie_ep->parf + PARF_SYS_CTRL); /* NVMe: read system control register */
	val &= ~PARF_SYS_CTRL_MSTR_ACLK_CGC_DIS;    /* NVMe: allow master AXI clock gating in idle to save power */
	val |= PARF_SYS_CTRL_SLV_DBI_WAKE_DISABLE | /* NVMe: host config access does not wake link from L1 */
	       PARF_SYS_CTRL_CORE_CLK_CGC_DIS |     /* NVMe: keep core clock running so PIPE clock reaches core for host link */
	       PARF_SYS_CTRL_AUX_PWR_DET;            /* NVMe: tell host Vaux present so D3cold/PME works */
	writel_relaxed(val, pcie_ep->parf + PARF_SYS_CTRL); /* NVMe: apply system control for host PM/clock behavior */

	/* Disable the debouncers */
	val = readl_relaxed(pcie_ep->parf + PARF_DB_CTRL);  /* NVMe: read debounce control */
	val |= PARF_DB_CTRL_INSR_DBNCR_BLOCK | PARF_DB_CTRL_RMVL_DBNCR_BLOCK |
	       PARF_DB_CTRL_DBI_WKP_BLOCK | PARF_DB_CTRL_SLV_WKP_BLOCK |
	       PARF_DB_CTRL_MST_WKP_BLOCK;          /* NVMe: block all debouncers so host events are not filtered */
	writel_relaxed(val, pcie_ep->parf + PARF_DB_CTRL);  /* NVMe: apply debounce settings for reliable hotplug */

	/* Request to exit from L1SS for MSI and LTR MSG */
	val = readl_relaxed(pcie_ep->parf + PARF_CFG_BITS); /* NVMe: read config bits */
	val |= PARF_CFG_BITS_REQ_EXIT_L1SS_MSI_LTR_EN;  /* NVMe: auto-exit ASPM L1SS when sending MSI/MSI-X or LTR to host */
	writel_relaxed(val, pcie_ep->parf + PARF_CFG_BITS); /* NVMe: ensure MSI-X interrupts and LTR reach host */

	dw_pcie_dbi_ro_wr_en(pci);      /* NVMe: enable writing read-only config fields so host reads correct values */

	/* Set the L0s Exit Latency to 2us-4us = 0x6 */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);  /* NVMe: locate PCIe capability in config space */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);  /* NVMe: read link capability host will use for ASPM */
	val &= ~PCI_EXP_LNKCAP_L0SEL;   /* NVMe: clear L0s exit latency field */
	val |= FIELD_PREP(PCI_EXP_LNKCAP_L0SEL, 0x6);   /* NVMe: set L0s exit latency 2-4us for host ASPM policy */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, val);  /* NVMe: write link capability for host enumeration */

	/* Set the L1 Exit Latency to be 32us-64 us = 0x6 */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);  /* NVMe: locate PCIe capability again */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);  /* NVMe: read link capability */
	val &= ~PCI_EXP_LNKCAP_L1EL;    /* NVMe: clear L1 exit latency field */
	val |= FIELD_PREP(PCI_EXP_LNKCAP_L1EL, 0x6);    /* NVMe: set L1 exit latency 32-64us for host ASPM policy */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, val);  /* NVMe: write link capability for host ASPM decisions */

	dw_pcie_dbi_ro_wr_dis(pci);     /* NVMe: lock read-only config fields; host reads stable values */

	writel_relaxed(0, pcie_ep->parf + PARF_INT_ALL_MASK);   /* NVMe: clear all interrupt masks momentarily */
	val = PARF_INT_ALL_LINK_DOWN | PARF_INT_ALL_BME |
	      PARF_INT_ALL_PM_TURNOFF | PARF_INT_ALL_DSTATE_CHANGE |
	      PARF_INT_ALL_LINK_UP | PARF_INT_ALL_EDMA; /* NVMe: enable link/BME/PM/D-state/link-up/eDMA interrupts from host */
	writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_MASK); /* NVMe: unmask host-driven events EP must handle */

	if (pcie_ep->cfg && pcie_ep->cfg->disable_mhi_ram_parity_check) {
		val = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_5_MASK);   /* NVMe: read extended mask 5 */
		val &= ~PARF_INT_ALL_5_MHI_RAM_DATA_PARITY_ERR; /* NVMe: unmask then mask? actually clear bit to mask parity IRQ */
		writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_5_MASK);   /* NVMe: mask MHI RAM parity to avoid spurious host-side issues */
	}

	val = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_3_MASK);   /* NVMe: read extended mask 3 */
	val &= ~PARF_INT_ALL_3_PTM_UPDATING;    /* NVMe: clear/mask PTM updating interrupt */
	writel_relaxed(val, pcie_ep->parf + PARF_INT_ALL_3_MASK);   /* NVMe: apply PTM mask */

	ret = dw_pcie_ep_init_registers(&pcie_ep->pci.ep);  /* NVMe: initialize EP config/BAR/MSI registers host will enumerate */
	if (ret) {                      /* NVMe: init failure means host cannot see valid PCIe device */
		dev_err(dev, "Failed to complete initialization: %d\n", ret);
		goto err_disable_resources;
	}

	qcom_pcie_common_set_equalization(pci); /* NVMe: set TX equalization for reliable high-speed host link */

	if (pcie_get_link_speed(pci->max_link_speed) == PCIE_SPEED_16_0GT)  /* NVMe: if host supports PCIe 4.0 x16? actually 16GT */
		qcom_pcie_common_set_16gt_lane_margining(pci);  /* NVMe: configure 16GT lane margining for host signal integrity */

	/*
	 * The physical address of the MMIO region which is exposed as the BAR
	 * should be written to MHI BASE registers.
	 */
	writel_relaxed(pcie_ep->mmio_res->start,
		       pcie_ep->parf + PARF_MHI_BASE_ADDR_LOWER);    /* NVMe: write low 32 bits of BAR backing address for host MMIO */
	writel_relaxed(0, pcie_ep->parf + PARF_MHI_BASE_ADDR_UPPER);    /* NVMe: high 32 bits (assume <4GB) for host BAR */

	/* Gate Master AXI clock to MHI bus during L1SS */
	val = readl_relaxed(pcie_ep->parf + PARF_MHI_CLOCK_RESET_CTRL); /* NVMe: read MHI clock reset control */
	val &= ~PARF_MSTR_AXI_CLK_EN;   /* NVMe: allow MHI AXI clock gating in L1SS to save power */
	writel_relaxed(val, pcie_ep->parf + PARF_MHI_CLOCK_RESET_CTRL); /* NVMe: apply MHI clock gating for host ASPM L1SS */

	pci_epc_init_notify(pcie_ep->pci.ep.epc);   /* NVMe: notify function drivers EP is ready; host may now bind NVMe driver */

	/* Enable LTSSM */
	val = readl_relaxed(pcie_ep->parf + PARF_LTSSM);    /* NVMe: read LTSSM control */
	val |= BIT(8);                  /* NVMe: set LTSSM enable bit so link trains with host */
	writel_relaxed(val, pcie_ep->parf + PARF_LTSSM);    /* NVMe: start link training; host will detect link up */

	if (pcie_ep->cfg && pcie_ep->cfg->override_no_snoop)
		writel_relaxed(WR_NO_SNOOP_OVERRIDE_EN | RD_NO_SNOOP_OVERRIDE_EN,
				pcie_ep->parf + PARF_NO_SNOOP_OVERRIDE);  /* NVMe: force snoop on all TLPs for coherent NVMe DMA with host IOMMU */

	return 0;                       /* NVMe: PERST deassert handling complete; host can enumerate EP */

err_disable_resources:
	qcom_pcie_disable_resources(pcie_ep);   /* NVMe: roll back resources on error; host sees no device */

	return ret;                     /* NVMe: return initialization error */
}

static void qcom_pcie_perst_assert(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci); /* NVMe: qcom EP state */

	qcom_pcie_disable_resources(pcie_ep); /* NVMe: host asserted PERST#; shut down PHY/clocks/link */
	pcie_ep->link_status = QCOM_PCIE_EP_LINK_DISABLED;  /* NVMe: mark link disabled so host driver knows device gone */
}

/* Common DWC controller ops */
static const struct dw_pcie_ops pci_ops = {
	.link_up = qcom_pcie_dw_link_up,        /* NVMe: host stack queries this to check if EP link is trained */
	.start_link = qcom_pcie_dw_start_link,  /* NVMe: called when host stack wants to allow link training on PERST */
	.stop_link = qcom_pcie_dw_stop_link,    /* NVMe: called when host stack wants to block PERST handling */
	.write_dbi2 = qcom_pcie_dw_write_dbi2,  /* NVMe: used to write RO config fields host will read */
};

static int qcom_pcie_ep_get_io_resources(struct platform_device *pdev,
					 struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev;        /* NVMe: platform device representing this EP */
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller pointer */
	struct device_node *syscon;             /* NVMe: DT node for TCSR syscon */
	struct resource *res;                   /* NVMe: DT memory resource descriptor */
	int ret;                                /* NVMe: return code */

	pcie_ep->parf = devm_platform_ioremap_resource_byname(pdev, "parf");    /* NVMe: ioremap PARF registers used for host link/PM control */
	if (IS_ERR(pcie_ep->parf))      /* NVMe: PARF missing means cannot control link for host */
		return PTR_ERR(pcie_ep->parf);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");    /* NVMe: get DBI memory resource for PCIe config space */
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, res);  /* NVMe: ioremap DBI so host config space can be programmed */
	if (IS_ERR(pci->dbi_base))      /* NVMe: DBI missing means no PCIe config space for host to enumerate */
		return PTR_ERR(pci->dbi_base);
	pci->dbi_base2 = pci->dbi_base; /* NVMe: DBI2 alias for shadow writes of RO config fields */

	pcie_ep->mmio_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							 "mmio");    /* NVMe: get MMIO resource backing BAR exposed to host */
	if (!pcie_ep->mmio_res) {       /* NVMe: MMIO missing means host has no device registers to access */
		dev_err(dev, "Failed to get mmio resource\n");
		return -EINVAL;
	}

	pcie_ep->mmio = devm_pci_remap_cfg_resource(dev, pcie_ep->mmio_res);  /* NVMe: ioremap BAR/MMIO region for local and host access */
	if (IS_ERR(pcie_ep->mmio))      /* NVMe: MMIO remap failure means host cannot read NVMe registers */
		return PTR_ERR(pcie_ep->mmio);

	syscon = of_parse_phandle(dev->of_node, "qcom,perst-regs", 0);  /* NVMe: locate TCSR syscon for PERST separation */
	if (!syscon) {                  /* NVMe: optional; if absent, PERST separation not supported */
		dev_dbg(dev, "PERST separation not available\n");
		return 0;
	}

	pcie_ep->perst_map = syscon_node_to_regmap(syscon); /* NVMe: convert syscon node to regmap for PERST control */
	of_node_put(syscon);            /* NVMe: drop reference after regmap conversion */
	if (IS_ERR(pcie_ep->perst_map)) /* NVMe: regmap failure blocks PERST separation config */
		return PTR_ERR(pcie_ep->perst_map);

	ret = of_property_read_u32_index(dev->of_node, "qcom,perst-regs",
					 1, &pcie_ep->perst_en); /* NVMe: read PERST enable register offset from DT */
	if (ret < 0) {                  /* NVMe: missing offset means cannot delatch PERST from host */
		dev_err(dev, "No Perst Enable offset in syscon\n");
		return ret;
	}

	ret = of_property_read_u32_index(dev->of_node, "qcom,perst-regs",
					 2, &pcie_ep->perst_sep_en); /* NVMe: read PERST separation enable offset from DT */
	if (ret < 0) {                  /* NVMe: missing offset means cannot separate PERST on host reboot */
		dev_err(dev, "No Perst Separation Enable offset in syscon\n");
		return ret;
	}

	return 0;                       /* NVMe: I/O resources parsed successfully */
}

static int qcom_pcie_ep_get_resources(struct platform_device *pdev,
				      struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev;        /* NVMe: platform device */
	int ret;                                /* NVMe: return code */

	ret = qcom_pcie_ep_get_io_resources(pdev, pcie_ep); /* NVMe: parse and ioremap MMIO/DBI/PARF/TCSR */
	if (ret) {                      /* NVMe: resource parse failure blocks host enumeration */
		dev_err(dev, "Failed to get io resources %d\n", ret);
		return ret;
	}

	pcie_ep->reset = devm_gpiod_get(dev, "reset", GPIOD_IN);  /* NVMe: PERST# GPIO input from host */
	if (IS_ERR(pcie_ep->reset))     /* NVMe: PERST GPIO missing means no host reset/hotplug detection */
		return PTR_ERR(pcie_ep->reset);

	pcie_ep->wake = devm_gpiod_get_optional(dev, "wake", GPIOD_OUT_LOW);  /* NVMe: optional WAKE# output to host for PME/enumeration */
	if (IS_ERR(pcie_ep->wake))      /* NVMe: WAKE GPIO missing means no PME to host */
		return PTR_ERR(pcie_ep->wake);

	if (pcie_ep->cfg && pcie_ep->cfg->firmware_managed) /* NVMe: firmware-managed: skip clk/reset/phy/interconnect get */
		return 0;

	pcie_ep->num_clks = devm_clk_bulk_get_all(dev, &pcie_ep->clks); /* NVMe: get all PCIe clocks needed for host link */
	if (pcie_ep->num_clks < 0) {    /* NVMe: clock get failure prevents enabling link for host */
		dev_err(dev, "Failed to get clocks\n");
		return pcie_ep->num_clks;
	}

	pcie_ep->core_reset = devm_reset_control_get_exclusive(dev, "core");  /* NVMe: get EP core reset for host-initiated re-enumeration */
	if (IS_ERR(pcie_ep->core_reset))    /* NVMe: reset missing means cannot cleanly reset for host */
		return PTR_ERR(pcie_ep->core_reset);

	pcie_ep->phy = devm_phy_optional_get(dev, "pciephy"); /* NVMe: get optional PCIe PHY for host link training */
	if (IS_ERR(pcie_ep->phy))       /* NVMe: propagate PHY get error */
		ret = PTR_ERR(pcie_ep->phy);

	pcie_ep->icc_mem = devm_of_icc_get(dev, "pcie-mem");  /* NVMe: get interconnect path for NVMe DMA to host memory */
	if (IS_ERR(pcie_ep->icc_mem))   /* NVMe: propagate interconnect get error */
		ret = PTR_ERR(pcie_ep->icc_mem);

	return ret;                     /* NVMe: return last error if any resource failed */
}

/* TODO: Notify clients about PCIe state change */
static irqreturn_t qcom_pcie_ep_global_irq_thread(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data;    /* NVMe: EP controller instance */
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller pointer */
	struct device *dev = pci->dev;          /* NVMe: device for logging */
	u32 status = readl_relaxed(pcie_ep->parf + PARF_INT_ALL_STATUS);    /* NVMe: read aggregated host-driven interrupt status */
	u32 dstate, val;                        /* NVMe: D-state and scratch value */

	writel_relaxed(status, pcie_ep->parf + PARF_INT_ALL_CLEAR); /* NVMe: clear serviced interrupts to host */

	if (FIELD_GET(PARF_INT_ALL_LINK_DOWN, status)) {    /* NVMe: host RC detected link down */
		dev_dbg(dev, "Received Linkdown event\n");
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_DOWN;  /* NVMe: update status so NVMe host driver sees device unavailable */
		dw_pcie_ep_linkdown(&pci->ep);  /* NVMe: notify EP function drivers (e.g., NVMe target) link lost */
	} else if (FIELD_GET(PARF_INT_ALL_BME, status)) {   /* NVMe: host set PCI_COMMAND_MASTER (Bus Master Enable) */
		dev_dbg(dev, "Received Bus Master Enable event\n");
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_ENABLED;   /* NVMe: DMA now allowed; NVMe driver can start queues */
		qcom_pcie_ep_icc_update(pcie_ep);   /* NVMe: scale interconnect bandwidth to actual host link speed/width */
		pci_epc_bus_master_enable_notify(pci->ep.epc);  /* NVMe: notify function drivers host enabled bus mastering */
	} else if (FIELD_GET(PARF_INT_ALL_PM_TURNOFF, status)) {    /* NVMe: host sent PM_Turn_Off (sleep/hibernate) */
		dev_dbg(dev, "Received PM Turn-off event! Entering L23\n");
		val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: read PM control */
		val |= PARF_PM_CTRL_READY_ENTR_L23; /* NVMe: acknowledge ready to enter L23 so host can remove power */
		writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: tell host EP is in L23-ready */
	} else if (FIELD_GET(PARF_INT_ALL_DSTATE_CHANGE, status)) { /* NVMe: host changed PCI power management state */
		dstate = dw_pcie_readl_dbi(pci, DBI_CON_STATUS) &
				   DBI_CON_STATUS_POWER_STATE_MASK; /* NVMe: read current D-state host wrote via PMCSR */
		dev_dbg(dev, "Received D%d state event\n", dstate);
		if (dstate == 3) {          /* NVMe: D3cold/D3hot entry requested by host */
			val = readl_relaxed(pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: read PM control */
			val |= PARF_PM_CTRL_REQ_EXIT_L1;    /* NVMe: request exit from ASPM L1 before entering D3 */
			writel_relaxed(val, pcie_ep->parf + PARF_PM_CTRL);  /* NVMe: apply L1 exit request */
		}
	} else if (FIELD_GET(PARF_INT_ALL_LINK_UP, status)) {   /* NVMe: link training complete, host can enumerate */
		dev_dbg(dev, "Received Linkup event. Enumeration complete!\n");
		dw_pcie_ep_linkup(&pci->ep);    /* NVMe: notify EP function drivers that host enumeration is possible */
		pcie_ep->link_status = QCOM_PCIE_EP_LINK_UP;    /* NVMe: mark link up for host-visible state */
	} else {                        /* NVMe: unknown event from host/RC */
		dev_WARN_ONCE(dev, 1, "Received unknown event. INT_STATUS: 0x%08x\n",
			      status);
	}

	return IRQ_HANDLED;             /* NVMe: global IRQ handled */
}

static irqreturn_t qcom_pcie_ep_perst_irq_thread(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data;    /* NVMe: EP controller instance */
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller pointer */
	struct device *dev = pci->dev;          /* NVMe: device for logging */
	u32 perst;                              /* NVMe: current PERST# GPIO value */

	perst = gpiod_get_value(pcie_ep->reset);    /* NVMe: sample PERST# from host */
	if (perst) {                    /* NVMe: host asserted PERST# (reset or hot unplug) */
		dev_dbg(dev, "PERST asserted by host. Shutting down the PCIe link!\n");
		qcom_pcie_perst_assert(pci);    /* NVMe: disable resources; host will see link down */
	} else {                        /* NVMe: host deasserted PERST# (boot or hot plug) */
		dev_dbg(dev, "PERST de-asserted by host. Starting link training!\n");
		qcom_pcie_perst_deassert(pci);  /* NVMe: enable resources and start link training for host enumeration */
	}

	irq_set_irq_type(gpiod_to_irq(pcie_ep->reset),
			 (perst ? IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW));    /* NVMe: re-arm IRQ for opposite PERST edge */

	return IRQ_HANDLED;             /* NVMe: PERST IRQ handled */
}

static int qcom_pcie_ep_enable_irq_resources(struct platform_device *pdev,
					     struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = pcie_ep->pci.dev;  /* NVMe: device for IRQ naming/allocation */
	char *name;                             /* NVMe: IRQ name buffer */
	int ret;                                /* NVMe: return code */

	name = devm_kasprintf(dev, GFP_KERNEL, "qcom_pcie_ep_global_irq%d",
			      pcie_ep->pci.ep.epc->domain_nr);  /* NVMe: unique global IRQ name per host PCIe domain */
	if (!name)                      /* NVMe: name allocation failure */
		return -ENOMEM;

	pcie_ep->global_irq = platform_get_irq_byname(pdev, "global");  /* NVMe: get global IRQ line for host link/PM/BME events */
	if (pcie_ep->global_irq < 0)    /* NVMe: global IRQ missing means no host event handling */
		return pcie_ep->global_irq;

	ret = devm_request_threaded_irq(&pdev->dev, pcie_ep->global_irq, NULL,
					qcom_pcie_ep_global_irq_thread,
					IRQF_ONESHOT,
					name, pcie_ep); /* NVMe: register threaded handler for host-driven global events */
	if (ret) {                      /* NVMe: IRQ request failure means host events are lost */
		dev_err(&pdev->dev, "Failed to request Global IRQ\n");
		return ret;
	}

	name = devm_kasprintf(dev, GFP_KERNEL, "qcom_pcie_ep_perst_irq%d",
			      pcie_ep->pci.ep.epc->domain_nr);  /* NVMe: unique PERST IRQ name per host PCIe domain */
	if (!name)                      /* NVMe: name allocation failure */
		return -ENOMEM;

	pcie_ep->perst_irq = gpiod_to_irq(pcie_ep->reset);  /* NVMe: get IRQ number for host PERST# GPIO */
	irq_set_status_flags(pcie_ep->perst_irq, IRQ_NOAUTOEN); /* NVMe: do not auto-enable; link start enables it explicitly */
	ret = devm_request_threaded_irq(&pdev->dev, pcie_ep->perst_irq, NULL,
					qcom_pcie_ep_perst_irq_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					name, pcie_ep); /* NVMe: register threaded handler for host PERST# hotplug/reset */
	if (ret) {                      /* NVMe: PERST IRQ request failure means no host reset detection */
		dev_err(&pdev->dev, "Failed to request PERST IRQ\n");
		disable_irq(pcie_ep->global_irq);   /* NVMe: roll back global IRQ on PERST IRQ failure */
		return ret;
	}

	return 0;                       /* NVMe: IRQ resources enabled */
}

static int qcom_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				  unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);   /* NVMe: get DWC controller from EP */

	switch (type) {                 /* NVMe: dispatch interrupt type the host NVMe driver expects */
	case PCI_IRQ_INTX:              /* NVMe: legacy INTx interrupt to host (rare for NVMe but supported) */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:               /* NVMe: Message Signaled Interrupt to host; NVMe typically uses MSI-X but MSI fallback here */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	default:                        /* NVMe: unsupported interrupt type for host NVMe driver */
		dev_err(pci->dev, "Unknown IRQ type\n");
		return -EINVAL;
	}
}

static int qcom_pcie_ep_link_transition_count(struct seq_file *s, void *data)
{
	struct qcom_pcie_ep *pcie_ep = (struct qcom_pcie_ep *)
				     dev_get_drvdata(s->private); /* NVMe: get EP instance from debugfs private data */

	seq_printf(s, "L0s transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L0S)); /* NVMe: ASPM L0s transitions seen with host */

	seq_printf(s, "L1 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L1)); /* NVMe: ASPM L1 transitions seen with host */

	seq_printf(s, "L1.1 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1)); /* NVMe: ASPM L1.1 transitions seen with host */

	seq_printf(s, "L1.2 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2)); /* NVMe: ASPM L1.2 transitions seen with host */

	seq_printf(s, "L2 transition count: %u\n",
		   readl_relaxed(pcie_ep->mmio + PARF_DEBUG_CNT_PM_LINKST_IN_L2)); /* NVMe: L2 (host power-off) transitions seen with host */

	return 0;                       /* NVMe: debugfs show completed */
}

static void qcom_pcie_ep_init_debugfs(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = &pcie_ep->pci;    /* NVMe: DWC controller pointer */

	debugfs_create_devm_seqfile(pci->dev, "link_transition_count", pcie_ep->debugfs,
				    qcom_pcie_ep_link_transition_count); /* NVMe: expose ASPM counters useful when host reports NVMe latency issues */
}

static const struct pci_epc_features qcom_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES,        /* NVMe: common DWC EP features (BAR config, MSI setup) */
	.linkup_notifier = true,        /* NVMe: notify function drivers when host link comes up */
	.msi_capable = true,            /* NVMe: EP supports MSI/MSI-X for host NVMe driver interrupt handling */
	.align = SZ_4K,                 /* NVMe: 4 KiB BAR alignment matches NVMe register page alignment */
	.bar[BAR_0] = { .only_64bit = true, }, /* NVMe: BAR0 is 64-bit only, typically maps NVMe controller registers for host */
	.bar[BAR_2] = { .only_64bit = true, }, /* NVMe: BAR2 is 64-bit only, may map additional NVMe queues/CMB for host */
};

static const struct pci_epc_features *
qcom_pcie_epc_get_features(struct dw_pcie_ep *pci_ep)
{
	return &qcom_pcie_epc_features; /* NVMe: return feature set the host NVMe driver will see during enumeration */
}

static const struct dw_pcie_ep_ops pci_ep_ops = {
	.raise_irq = qcom_pcie_ep_raise_irq,    /* NVMe: callback to send MSI/INTx to host NVMe driver */
	.get_features = qcom_pcie_epc_get_features, /* NVMe: callback to advertise BAR/MSI capabilities to host */
};

static int qcom_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;        /* NVMe: platform device for this EP */
	struct qcom_pcie_ep *pcie_ep;           /* NVMe: EP controller instance */
	char *name;                             /* NVMe: debugfs directory name */
	int ret;                                /* NVMe: return code */

	pcie_ep = devm_kzalloc(dev, sizeof(*pcie_ep), GFP_KERNEL);  /* NVMe: allocate EP controller memory */
	if (!pcie_ep)                   /* NVMe: allocation failure prevents registration of EP for host */
		return -ENOMEM;

	pcie_ep->pci.dev = dev;         /* NVMe: bind DWC controller to platform device */
	pcie_ep->pci.ops = &pci_ops;    /* NVMe: register DWC host-facing ops (link_up, start_link, etc.) */
	pcie_ep->pci.ep.ops = &pci_ep_ops;  /* NVMe: register EP ops for MSI/BAR features seen by host */

	pcie_ep->cfg = of_device_get_match_data(dev);   /* NVMe: load SoC-specific config from DT compatible */
	if (pcie_ep->cfg && pcie_ep->cfg->hdma_support) {
		pcie_ep->pci.edma.ll_wr_cnt = 8;    /* NVMe: eDMA linked-list write count for NVMe write offload */
		pcie_ep->pci.edma.ll_rd_cnt = 8;    /* NVMe: eDMA linked-list read count for NVMe read offload */
		pcie_ep->pci.edma.mf = EDMA_MF_HDMA_NATIVE; /* NVMe: native HDMA mode for high-throughput NVMe DMA */
	}

	platform_set_drvdata(pdev, pcie_ep);    /* NVMe: store EP instance for remove/shutdown and PERST IRQ */

	pm_runtime_get_noresume(dev);   /* NVMe: keep device initially active until probe is ready */
	pm_runtime_set_active(dev);     /* NVMe: mark runtime PM active before host can access */
	ret = devm_pm_runtime_enable(dev);  /* NVMe: enable runtime PM for host-initiated power transitions */
	if (ret)                        /* NVMe: runtime PM failure blocks proper host power management */
		return ret;

	ret = qcom_pcie_ep_get_resources(pdev, pcie_ep);    /* NVMe: parse DT resources (MMIO, DBI, IRQ, PHY, clocks) */
	if (ret)                        /* NVMe: resource failure means EP cannot appear on host bus */
		return ret;

	ret = dw_pcie_ep_init(&pcie_ep->pci.ep);    /* NVMe: initialize DWC endpoint framework; prepares config/BARs host will enumerate */
	if (ret) {                      /* NVMe: EP init failure means no valid PCIe device for host */
		dev_err(dev, "Failed to initialize endpoint: %d\n", ret);
		return ret;
	}

	ret = qcom_pcie_ep_enable_irq_resources(pdev, pcie_ep); /* NVMe: register global and PERST IRQ handlers for host events */
	if (ret)                        /* NVMe: IRQ failure means host link/PM events are not handled */
		goto err_ep_deinit;

	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node);  /* NVMe: debugfs directory named after DT node */
	if (!name) {                    /* NVMe: name allocation failure */
		ret = -ENOMEM;
		goto err_disable_irqs;
	}

	ret = pm_runtime_put_sync(dev); /* NVMe: allow runtime suspend now that probe is mostly done */
	if (ret < 0) {                  /* NVMe: suspend failure keeps device active; host may still enumerate but power is higher */
		dev_err(dev, "Failed to suspend device: %d\n", ret);
		goto err_disable_irqs;
	}

	pcie_ep->debugfs = debugfs_create_dir(name, NULL);  /* NVMe: create debugfs directory for ASPM counters */
	qcom_pcie_ep_init_debugfs(pcie_ep); /* NVMe: populate debugfs files useful for NVMe latency/power debug */

	return 0;                       /* NVMe: probe successful; EP ready to respond to host enumeration */

err_disable_irqs:
	disable_irq(pcie_ep->global_irq);   /* NVMe: disable global IRQ on error path */
	disable_irq(pcie_ep->perst_irq);    /* NVMe: disable PERST IRQ on error path */

err_ep_deinit:
	dw_pcie_ep_deinit(&pcie_ep->pci.ep);    /* NVMe: tear down DWC EP framework so host sees no device */

	return ret;                     /* NVMe: return probe error */
}

static void qcom_pcie_ep_remove(struct platform_device *pdev)
{
	struct qcom_pcie_ep *pcie_ep = platform_get_drvdata(pdev);  /* NVMe: retrieve EP instance */

	disable_irq(pcie_ep->global_irq);   /* NVMe: stop handling host link/PM/BME events */
	disable_irq(pcie_ep->perst_irq);    /* NVMe: stop handling host PERST# hotplug/reset */

	debugfs_remove_recursive(pcie_ep->debugfs); /* NVMe: remove debugfs ASPM counters */

	if (pcie_ep->link_status == QCOM_PCIE_EP_LINK_DISABLED) /* NVMe: if already disabled by host PERST, nothing more */
		return;

	qcom_pcie_disable_resources(pcie_ep);   /* NVMe: power off PHY/clocks so host loses link on driver remove */
}

static const struct qcom_pcie_ep_cfg cfg_1_34_0 = {
	.hdma_support = true,           /* NVMe: enable eDMA/HDMA for high-throughput NVMe data movement */
	.override_no_snoop = true,      /* NVMe: force snoop for coherent DMA with host IOMMU/SMMU */
	.disable_mhi_ram_parity_check = true,   /* NVMe: mask MHI RAM parity to avoid spurious interrupts during NVMe operation */
};

static const struct qcom_pcie_ep_cfg cfg_1_34_0_fw_managed = {
	.hdma_support = true,           /* NVMe: same HDMA support as non-firmware-managed variant */
	.override_no_snoop = true,      /* NVMe: same snoop override for host DMA coherency */
	.disable_mhi_ram_parity_check = true,   /* NVMe: same MHI RAM parity masking */
	.firmware_managed = true,       /* NVMe: firmware owns PHY/clk/reset; host still sees normal EP behavior */
};

static const struct of_device_id qcom_pcie_ep_match[] = {
	{ .compatible = "qcom,sa8255p-pcie-ep", .data = &cfg_1_34_0_fw_managed}, /* NVMe: SA8255P EP, firmware-managed resources */
	{ .compatible = "qcom,sa8775p-pcie-ep", .data = &cfg_1_34_0}, /* NVMe: SA8775P EP with HDMA and snoop override */
	{ .compatible = "qcom,sdx55-pcie-ep", }, /* NVMe: SDX55 EP (default config) */
	{ .compatible = "qcom,sm8450-pcie-ep", }, /* NVMe: SM8450 EP (default config) */
	{ .compatible = "qcom,sar2130p-pcie-ep", }, /* NVMe: SAR2130P EP (default config) */
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pcie_ep_match);    /* NVMe: register DT match table for module loading before host NVMe driver binds */

static struct platform_driver qcom_pcie_ep_driver = {
	.probe	= qcom_pcie_ep_probe,    /* NVMe: called when DT node probed; sets up EP for host enumeration */
	.remove = qcom_pcie_ep_remove,  /* NVMe: called on driver unload; tears down EP so host loses device */
	.driver	= {
		.name = "qcom-pcie-ep", /* NVMe: platform driver name */
		.of_match_table	= qcom_pcie_ep_match,    /* NVMe: DT compatibles for Qualcomm PCIe endpoint controllers */
	},
};
builtin_platform_driver(qcom_pcie_ep_driver);   /* NVMe: built-in driver so EP is ready before host PCI enumeration */

MODULE_AUTHOR("Siddartha Mohanadoss <smohanad@codeaurora.org>");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Qualcomm PCIe Endpoint controller driver"); /* NVMe: endpoint-side counterpart that host NVMe driver sees as a PCIe device */
MODULE_LICENSE("GPL v2");
