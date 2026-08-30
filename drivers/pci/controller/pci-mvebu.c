// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Marvell Armada 370 and Armada XP SoCs
 *
 * Author: Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 */

#include <linux/kernel.h>		/* PCI/NVMe: kernel core, used by PCIe host bridge for NVMe enumeration */
#include <linux/module.h>		/* PCI/NVMe: module init/exit, host controller driver registration for NVMe SSDs */
#include <linux/pci.h>			/* PCI/NVMe: core PCI definitions, config space access used by nvme-pci */
#include <linux/bitfield.h>		/* PCI/NVMe: FIELD_PREP for PCIe capability fields that nvme-pci later reads */
#include <linux/clk.h>			/* PCI/NVMe: PCIe refclk, must be stable before NVMe endpoint PERST release */
#include <linux/delay.h>		/* PCI/NVMe: PERST/uDelay timing required by PCIe CEM spec for NVMe link training */
#include <linux/gpio/consumer.h>	/* PCI/NVMe: PERST# GPIO control to reset NVMe SSD during probe */
#include <linux/init.h>			/* PCI/NVMe: __init/__exit annotations for host driver */
#include <linux/irqchip/chained_irq.h>	/* PCI/NVMe: chained INTx handler used when NVMe falls back from MSI-X */
#include <linux/irqdomain.h>		/* PCI/NVMe: INTx irqdomain mapping for legacy NVMe interrupts */
#include <linux/mbus.h>			/* PCI/NVMe: MBus windows translate NVMe DMA / MMIO to DRAM */
#include <linux/slab.h>			/* PCI/NVMe: kmalloc for per-port host controller data */
#include <linux/platform_device.h>	/* PCI/NVMe: platform bus registration of PCIe host controller */
#include <linux/of_address.h>		/* PCI/NVMe: DT register ranges for host controller MMIO */
#include <linux/of_irq.h>		/* PCI/NVMe: parse "intx" irq for legacy NVMe INTx support */
#include <linux/of_pci.h>		/* PCI/NVMe: DT PCI helpers, devfn parsing for NVMe root ports */
#include <linux/of_platform.h>		/* PCI/NVMe: OF platform device helpers */

#include "../pci.h"			/* PCI/NVMe: PCI core internal APIs used to register host bridge for NVMe */
#include "../pci-bridge-emul.h"		/* PCI/NVMe: emulated PCI bridge because mvebu RP lacks Type-1 config space for NVMe */

/*
 * PCIe unit register offsets.
 */
#define PCIE_DEV_ID_OFF		0x0000	/* PCI/NVMe: Device/Vendor ID, identifies mvebu root port to NVMe subsystem */
#define PCIE_CMD_OFF		0x0004	/* PCI/NVMe: PCI command register (IO/MEM/BusMaster) gates NVMe BAR/DMA access */
#define PCIE_DEV_REV_OFF	0x0008	/* PCI/NVMe: Class code/revision, later forced to PCI bridge for NVMe enumeration */
#define PCIE_BAR_LO_OFF(n)	(0x0010 + ((n) << 3))	/* PCI/NVMe: BAR low, maps internal registers needed for MSI to NVMe */
#define PCIE_BAR_HI_OFF(n)	(0x0014 + ((n) << 3))	/* PCI/NVMe: BAR high, 64-bit BAR support for NVMe memory windows */
#define PCIE_SSDEV_ID_OFF	0x002c	/* PCI/NVMe: Subsystem IDs visible to NVMe driver during enumeration */
#define PCIE_CAP_PCIEXP		0x0060	/* PCI/NVMe: PCIe capability offset; nvme-pci checks link status/capabilities here */
#define PCIE_CAP_PCIERR_OFF	0x0100	/* PCI/NVMe: Advanced Error Reporting (AER) capability base for NVMe error handling */
#define PCIE_BAR_CTRL_OFF(n)	(0x1804 + (((n) - 1) * 4))	/* PCI/NVMe: BAR1/2 control/size, used for NVMe memory aperture setup */
#define PCIE_WIN04_CTRL_OFF(n)	(0x1820 + ((n) << 4))	/* PCI/NVMe: MBus window 0-4 control, maps NVMe DMA to DRAM bank */
#define PCIE_WIN04_BASE_OFF(n)	(0x1824 + ((n) << 4))	/* PCI/NVMe: MBus window 0-4 base, CPU physical address for NVMe transactions */
#define PCIE_WIN04_REMAP_OFF(n)	(0x182c + ((n) << 4))	/* PCI/NVMe: MBus window 0-4 remap, address translation for IOMMU/DMA */
#define PCIE_WIN5_CTRL_OFF	0x1880	/* PCI/NVMe: MBus window 5 control, additional address translation for NVMe */
#define PCIE_WIN5_BASE_OFF	0x1884	/* PCI/NVMe: MBus window 5 base */
#define PCIE_WIN5_REMAP_OFF	0x188c	/* PCI/NVMe: MBus window 5 remap */
#define PCIE_CONF_ADDR_OFF	0x18f8	/* PCI/NVMe: config access address port for NVMe enumeration config cycles */
#define  PCIE_CONF_ADDR_EN		0x80000000	/* PCI/NVMe: enable bit for config cycle toward NVMe device */
#define  PCIE_CONF_REG(r)		((((r) & 0xf00) << 16) | ((r) & 0xfc))	/* PCI/NVMe: encode extended register address for NVMe config reads/writes */
#define  PCIE_CONF_BUS(b)		(((b) & 0xff) << 16)	/* PCI/NVMe: encode target bus for NVMe behind root port */
#define  PCIE_CONF_DEV(d)		(((d) & 0x1f) << 11)	/* PCI/NVMe: encode target device for NVMe SSD */
#define  PCIE_CONF_FUNC(f)		(((f) & 0x7) << 8)	/* PCI/NVMe: encode target function for NVMe controller */
#define  PCIE_CONF_ADDR(bus, devfn, where) \
	(PCIE_CONF_BUS(bus) | PCIE_CONF_DEV(PCI_SLOT(devfn))    | \
	 PCIE_CONF_FUNC(PCI_FUNC(devfn)) | PCIE_CONF_REG(where) | \
	 PCIE_CONF_ADDR_EN)	/* PCI/NVMe: full config address used to enumerate NVMe config space */
#define PCIE_CONF_DATA_OFF	0x18fc	/* PCI/NVMe: config data port, used by pci-mvebu child ops for NVMe */
#define PCIE_INT_CAUSE_OFF	0x1900	/* PCI/NVMe: interrupt cause register (PME/INTx) for NVMe events */
#define PCIE_INT_UNMASK_OFF	0x1910	/* PCI/NVMe: interrupt unmask register, controls INTx delivery to NVMe */
#define  PCIE_INT_INTX(i)		BIT(24+i)	/* PCI/NVMe: legacy INTx#A/B/C/D bit used when NVMe MSI-X is unavailable */
#define  PCIE_INT_PM_PME		BIT(28)	/* PCI/NVMe: PME interrupt used for NVMe link power management wakeup */
#define  PCIE_INT_ALL_MASK		GENMASK(31, 0)	/* PCI/NVMe: mask all interrupt sources during NVMe host init */
#define PCIE_CTRL_OFF		0x1a00	/* PCI/NVMe: root port control (RC mode, hot reset) for NVMe */
#define  PCIE_CTRL_X1_MODE		0x0001	/* PCI/NVMe: force x1 link width even if NVMe SSD supports wider */
#define  PCIE_CTRL_RC_MODE		BIT(1)	/* PCI/NVMe: enable Root Complex mode so NVMe SSD can be enumerated downstream */
#define  PCIE_CTRL_MASTER_HOT_RESET	BIT(24)	/* PCI/NVMe: secondary bus reset used to reset NVMe endpoint */
#define PCIE_STAT_OFF		0x1a04	/* PCI/NVMe: status (link up/down, local bus/dev) critical for NVMe probe */
#define  PCIE_STAT_BUS				0xff00	/* PCI/NVMe: local bus number used to route NVMe config cycles */
#define  PCIE_STAT_DEV				0x1f0000	/* PCI/NVMe: local device number, distinguishes root port from NVMe */
#define  PCIE_STAT_LINK_DOWN		BIT(0)	/* PCI/NVMe: link down means NVMe SSD not detected/trained */
#define PCIE_SSPL_OFF		0x1a0c	/* PCI/NVMe: Set Slot Power Limit message config for NVMe power budget */
#define  PCIE_SSPL_VALUE_SHIFT		0	/* PCI/NVMe: slot power limit value shift */
#define  PCIE_SSPL_VALUE_MASK		GENMASK(7, 0)	/* PCI/NVMe: slot power limit value mask */
#define  PCIE_SSPL_SCALE_SHIFT		8	/* PCI/NVMe: slot power limit scale shift */
#define  PCIE_SSPL_SCALE_MASK		GENMASK(9, 8)	/* PCI/NVMe: slot power limit scale mask */
#define  PCIE_SSPL_ENABLE		BIT(16)	/* PCI/NVMe: enable sending slot power limit message to NVMe */
#define PCIE_RC_RTSTA		0x1a14	/* PCI/NVMe: root status register, PME status from NVMe power state changes */
#define PCIE_DEBUG_CTRL         0x1a60	/* PCI/NVMe: debug control register used for soft reset */
#define  PCIE_DEBUG_SOFT_RESET		BIT(20)	/* PCI/NVMe: soft reset bit to recover NVMe link */

struct mvebu_pcie_port;	/* PCI/NVMe: forward declaration for per-root-port state used by NVMe host bridge */

/* Structure representing all PCIe interfaces */
struct mvebu_pcie {
	struct platform_device *pdev;	/* PCI/NVMe: platform device backing this host controller for NVMe */
	struct mvebu_pcie_port *ports;	/* PCI/NVMe: array of root ports each potentially connecting an NVMe SSD */
	struct resource io;		/* PCI/NVMe: host bridge I/O aperture, rarely used by NVMe but part of PCI bus */
	struct resource realio;		/* PCI/NVMe: remapped I/O resource exported to PCI bus for NVMe BAR allocation */
	struct resource mem;		/* PCI/NVMe: host bridge MEM aperture where NVMe BARs are allocated */
	int nports;			/* PCI/NVMe: number of probed root ports available for NVMe */
};

struct mvebu_pcie_window {
	phys_addr_t base;	/* PCI/NVMe: CPU-side base of MBus window for NVMe MMIO/DMA */
	phys_addr_t remap;	/* PCI/NVMe: bus-side remap address used for IOMMU/translation of NVMe accesses */
	size_t size;		/* PCI/NVMe: window size, power-of-two chunks for NVMe BAR alignment */
};

/* Structure representing one PCIe interface */
struct mvebu_pcie_port {
	char *name;			/* PCI/NVMe: human-readable port name, logged during NVMe host probe */
	void __iomem *base;		/* PCI/NVMe: iomapped controller registers for NVMe link/config access */
	u32 port;			/* PCI/NVMe: hardware port index for multi-port mvebu root complexes */
	u32 lane;			/* PCI/NVMe: SerDes lane index for this NVMe-capable port */
	bool is_x4;			/* PCI/NVMe: x4 link width flag; affects NVMe bandwidth and MLW capability */
	int devfn;			/* PCI/NVMe: BDF on virtual bus 0 where this NVMe root port appears */
	unsigned int mem_target;	/* PCI/NVMe: MBus target id for NVMe memory transactions toward DRAM */
	unsigned int mem_attr;		/* PCI/NVMe: MBus attribute for NVMe memory transactions */
	unsigned int io_target;		/* PCI/NVMe: MBus target id for NVMe I/O transactions */
	unsigned int io_attr;		/* PCI/NVMe: MBus attribute for NVMe I/O transactions */
	struct clk *clk;		/* PCI/NVMe: PCIe reference clock required before NVMe PERST release */
	struct gpio_desc *reset_gpio;	/* PCI/NVMe: PERST# GPIO for NVMe endpoint reset sequencing */
	char *reset_name;		/* PCI/NVMe: reset GPIO label for this NVMe port */
	struct pci_bridge_emul bridge;	/* PCI/NVMe: emulated Type-1 bridge seen by NVMe enumeration code */
	struct device_node *dn;		/* PCI/NVMe: DT node for this NVMe root port */
	struct mvebu_pcie *pcie;	/* PCI/NVMe: back pointer to global host controller state */
	struct mvebu_pcie_window memwin;	/* PCI/NVMe: current MEM window behind this NVMe root port */
	struct mvebu_pcie_window iowin;		/* PCI/NVMe: current I/O window behind this NVMe root port */
	u32 saved_pcie_stat;		/* PCI/NVMe: saved status for suspend/resume of NVMe link */
	struct resource regs;		/* PCI/NVMe: physical register resource for this NVMe root port */
	u8 slot_power_limit_value;	/* PCI/NVMe: slot power limit value advertised to NVMe */
	u8 slot_power_limit_scale;	/* PCI/NVMe: slot power limit scale advertised to NVMe */
	struct irq_domain *intx_irq_domain;	/* PCI/NVMe: irqdomain for legacy INTx when NVMe MSI-X unavailable */
	raw_spinlock_t irq_lock;	/* PCI/NVMe: protects INTx unmask register from concurrent NVMe IRQ changes */
	int intx_irq;			/* PCI/NVMe: GIC irq number for chained INTx handler serving NVMe */
};

static inline void mvebu_writel(struct mvebu_pcie_port *port, u32 val, u32 reg)	/* PCI/NVMe: helper to write 32-bit register on NVMe host controller */
{
	writel(val, port->base + reg);	/* PCI/NVMe: post MMIO write to controller register affecting NVMe link/config */
}

static inline u32 mvebu_readl(struct mvebu_pcie_port *port, u32 reg)	/* PCI/NVMe: helper to read 32-bit register from NVMe host controller */
{
	return readl(port->base + reg);	/* PCI/NVMe: read MMIO register state used to detect NVMe link/IRQ events */
}

static inline bool mvebu_has_ioport(struct mvebu_pcie_port *port)	/* PCI/NVMe: checks if this NVMe root port has I/O translation support */
{
	return port->io_target != -1 && port->io_attr != -1;	/* PCI/NVMe: valid MBus I/O target/attribute exist for NVMe I/O BARs */
}

static bool mvebu_pcie_link_up(struct mvebu_pcie_port *port)	/* PCI/NVMe: returns true when NVMe SSD link is trained and up */
{
	return !(mvebu_readl(port, PCIE_STAT_OFF) & PCIE_STAT_LINK_DOWN);	/* PCI/NVMe: link down bit set means NVMe not reachable */
}

static u8 mvebu_pcie_get_local_bus_nr(struct mvebu_pcie_port *port)	/* PCI/NVMe: reads local bus number used for NVMe config cycle routing */
{
	return (mvebu_readl(port, PCIE_STAT_OFF) & PCIE_STAT_BUS) >> 8;	/* PCI/NVMe: bits [15:8] are secondary bus for NVMe behind RP */
}

static void mvebu_pcie_set_local_bus_nr(struct mvebu_pcie_port *port, int nr)	/* PCI/NVMe: programs local bus number for NVMe config routing */
{
	u32 stat;	/* PCI/NVMe: temporary copy of status register */

	stat = mvebu_readl(port, PCIE_STAT_OFF);	/* PCI/NVMe: read current status including NVMe link state */
	stat &= ~PCIE_STAT_BUS;	/* PCI/NVMe: clear previous bus number before programming NVMe secondary bus */
	stat |= nr << 8;	/* PCI/NVMe: set new local bus number so NVMe config cycles route correctly */
	mvebu_writel(port, stat, PCIE_STAT_OFF);	/* PCI/NVMe: write back status with new NVMe bus number */
}

static void mvebu_pcie_set_local_dev_nr(struct mvebu_pcie_port *port, int nr)	/* PCI/NVMe: sets local device number to distinguish root port from NVMe */
{
	u32 stat;	/* PCI/NVMe: temporary copy of status register */

	stat = mvebu_readl(port, PCIE_STAT_OFF);	/* PCI/NVMe: read current status */
	stat &= ~PCIE_STAT_DEV;	/* PCI/NVMe: clear old device number */
	stat |= nr << 16;	/* PCI/NVMe: program device number for this NVMe root port */
	mvebu_writel(port, stat, PCIE_STAT_OFF);	/* PCI/NVMe: write back so config cycles target correct NVMe BDF */
}

static void mvebu_pcie_disable_wins(struct mvebu_pcie_port *port)	/* PCI/NVMe: disable all BARs and MBus windows before reconfiguring NVMe apertures */
{
	int i;	/* PCI/NVMe: loop index for BAR/window iteration */

	mvebu_writel(port, 0, PCIE_BAR_LO_OFF(0));	/* PCI/NVMe: clear BAR0 low (internal registers/MSI target for NVMe) */
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(0));	/* PCI/NVMe: clear BAR0 high */

	for (i = 1; i < 3; i++) {	/* PCI/NVMe: iterate BAR1 and BAR2 used for NVMe memory windows */
		mvebu_writel(port, 0, PCIE_BAR_CTRL_OFF(i));	/* PCI/NVMe: disable BAR control for this NVMe aperture */
		mvebu_writel(port, 0, PCIE_BAR_LO_OFF(i));	/* PCI/NVMe: clear BAR low address */
		mvebu_writel(port, 0, PCIE_BAR_HI_OFF(i));	/* PCI/NVMe: clear BAR high address */
	}

	for (i = 0; i < 5; i++) {	/* PCI/NVMe: iterate MBus windows 0-4 mapping NVMe DMA to DRAM banks */
		mvebu_writel(port, 0, PCIE_WIN04_CTRL_OFF(i));	/* PCI/NVMe: disable MBus window control */
		mvebu_writel(port, 0, PCIE_WIN04_BASE_OFF(i));	/* PCI/NVMe: clear window base */
		mvebu_writel(port, 0, PCIE_WIN04_REMAP_OFF(i));	/* PCI/NVMe: clear window remap */
	}

	mvebu_writel(port, 0, PCIE_WIN5_CTRL_OFF);	/* PCI/NVMe: disable MBus window 5 control */
	mvebu_writel(port, 0, PCIE_WIN5_BASE_OFF);	/* PCI/NVMe: clear window 5 base */
	mvebu_writel(port, 0, PCIE_WIN5_REMAP_OFF);	/* PCI/NVMe: clear window 5 remap */
}

/*
 * Setup PCIE BARs and Address Decode Wins:
 * BAR[0] -> internal registers (needed for MSI)
 * BAR[1] -> covers all DRAM banks
 * BAR[2] -> Disabled
 * WIN[0-3] -> DRAM bank[0-3]
 */
static void mvebu_pcie_setup_wins(struct mvebu_pcie_port *port)	/* PCI/NVMe: configures DRAM windows so NVMe DMA/MMIO can reach system memory */
{
	const struct mbus_dram_target_info *dram;	/* PCI/NVMe: MBus DRAM geometry from SoC, used to map NVMe DMA */
	u32 size;	/* PCI/NVMe: total DRAM size covered by BAR1 for NVMe memory space */
	int i;	/* PCI/NVMe: loop index for DRAM chip-selects */

	dram = mv_mbus_dram_info();	/* PCI/NVMe: retrieve SoC DRAM bank layout for NVMe address translation */

	/* First, disable and clear BARs and windows. */
	mvebu_pcie_disable_wins(port);	/* PCI/NVMe: start from clean state before NVMememory map setup */

	/* Setup windows for DDR banks.  Count total DDR size on the fly. */
	size = 0;	/* PCI/NVMe: accumulate total DRAM size visible to NVMe */
	for (i = 0; i < dram->num_cs; i++) {	/* PCI/NVMe: one MBus window per DRAM chip-select for NVMe access */
		const struct mbus_dram_window *cs = dram->cs + i;	/* PCI/NVMe: current DRAM bank descriptor for NVMe mapping */

		mvebu_writel(port, cs->base & 0xffff0000,
			     PCIE_WIN04_BASE_OFF(i));	/* PCI/NVMe: program DRAM bank base for NVMe inbound transactions */
		mvebu_writel(port, 0, PCIE_WIN04_REMAP_OFF(i));	/* PCI/NVMe: identity remap, DRAM physical addresses preserved for NVMe DMA */
		mvebu_writel(port,
			     ((cs->size - 1) & 0xffff0000) |
			     (cs->mbus_attr << 8) |
			     (dram->mbus_dram_target_id << 4) | 1,
			     PCIE_WIN04_CTRL_OFF(i));	/* PCI/NVMe: enable window, set size/target/attr so NVMe DMA reaches DRAM */

		size += cs->size;	/* PCI/NVMe: sum bank sizes to size BAR1 aperture for NVMe */
	}

	/* Round up 'size' to the nearest power of two. */
	if ((size & (size - 1)) != 0)
		size = 1 << fls(size);	/* PCI/NVMe: BAR size must be power-of-two for NVMe BAR allocation */

	/* Setup BAR[1] to all DRAM banks. */
	mvebu_writel(port, dram->cs[0].base, PCIE_BAR_LO_OFF(1));	/* PCI/NVMe: BAR1 base at first DRAM bank, used for NVMe memory space */
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(1));	/* PCI/NVMe: BAR1 high 32 bits zero (32-bit aperture) */
	mvebu_writel(port, ((size - 1) & 0xffff0000) | 1,
		     PCIE_BAR_CTRL_OFF(1));	/* PCI/NVMe: enable BAR1 with computed size for NVMe MMIO/DMA */

	/*
	 * Point BAR[0] to the device's internal registers.
	 */
	mvebu_writel(port, round_down(port->regs.start, SZ_1M), PCIE_BAR_LO_OFF(0));	/* PCI/NVMe: BAR0 points to controller regs; MSI doorbell logic in NVMe may reach here */
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(0));	/* PCI/NVMe: BAR0 high 32 bits zero */
}

static void mvebu_pcie_setup_hw(struct mvebu_pcie_port *port)	/* PCI/NVMe: initialize controller hardware for NVMe enumeration */
{
	u32 ctrl, lnkcap, cmd, dev_rev, unmask, sspl;	/* PCI/NVMe: local copies of registers to program NVMe link/bridge */

	/* Setup PCIe controller to Root Complex mode. */
	ctrl = mvebu_readl(port, PCIE_CTRL_OFF);	/* PCI/NVMe: read current control state before NVMe RC setup */
	ctrl |= PCIE_CTRL_RC_MODE;	/* PCI/NVMe: enable Root Complex mode so NVMe SSD is downstream */
	mvebu_writel(port, ctrl, PCIE_CTRL_OFF);	/* PCI/NVMe: commit RC mode for NVMe enumeration */

	/*
	 * Set Maximum Link Width to X1 or X4 in Root Port's PCIe Link
	 * Capability register. This register is defined by PCIe specification
	 * as read-only but this mvebu controller has it as read-write and must
	 * be set to number of SerDes PCIe lanes (1 or 4). If this register is
	 * not set correctly then link with endpoint card is not established.
	 */
	lnkcap = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP);	/* PCI/NVMe: read link capability visible to NVMe driver */
	lnkcap &= ~PCI_EXP_LNKCAP_MLW;	/* PCI/NVMe: clear max link width field */
	lnkcap |= FIELD_PREP(PCI_EXP_LNKCAP_MLW, port->is_x4 ? 4 : 1);	/* PCI/NVMe: program x1 or x4 width matching NVMe SSD connector */
	mvebu_writel(port, lnkcap, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP);	/* PCI/NVMe: commit link width so NVMe training succeeds */

	/* Disable Root Bridge I/O space, memory space and bus mastering. */
	cmd = mvebu_readl(port, PCIE_CMD_OFF);	/* PCI/NVMe: read PCI command before reconfiguring NVMe access enables */
	cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);	/* PCI/NVMe: disable IO/MEM/busmaster until NVMe BARs assigned */
	mvebu_writel(port, cmd, PCIE_CMD_OFF);	/* PCI/NVMe: commit command, preventing premature NVMe DMA access */

	/*
	 * Change Class Code of PCI Bridge device to PCI Bridge (0x6004)
	 * because default value is Memory controller (0x5080).
	 *
	 * Note that this mvebu PCI Bridge does not have compliant Type 1
	 * Configuration Space. Header Type is reported as Type 0 and it
	 * has format of Type 0 config space.
	 *
	 * Moreover Type 0 BAR registers (ranges 0x10 - 0x28 and 0x30 - 0x34)
	 * have the same format in Marvell's specification as in PCIe
	 * specification, but their meaning is totally different and they do
	 * different things: they are aliased into internal mvebu registers
	 * (e.g. PCIE_BAR_LO_OFF) and these should not be changed or
	 * reconfigured by pci device drivers.
	 *
	 * Therefore driver uses emulation of PCI Bridge which emulates
	 * access to configuration space via internal mvebu registers or
	 * emulated configuration buffer. Driver access these PCI Bridge
	 * directly for simplification, but these registers can be accessed
	 * also via standard mvebu way for accessing PCI config space.
	 */
	dev_rev = mvebu_readl(port, PCIE_DEV_REV_OFF);	/* PCI/NVMe: read class/revision register */
	dev_rev &= ~0xffffff00;	/* PCI/NVMe: clear class code field */
	dev_rev |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;	/* PCI/NVMe: set class to PCI bridge so NVMe subsystem recognizes root port */
	mvebu_writel(port, dev_rev, PCIE_DEV_REV_OFF);	/* PCI/NVMe: commit class code for NVMe enumeration */

	/* Point PCIe unit MBUS decode windows to DRAM space. */
	mvebu_pcie_setup_wins(port);	/* PCI/NVMe: set up inbound windows for NVMe DMA to DRAM */

	/*
	 * Program Root Port to automatically send Set_Slot_Power_Limit
	 * PCIe Message when changing status from Dl_Down to Dl_Up and valid
	 * slot power limit was specified.
	 */
	sspl = mvebu_readl(port, PCIE_SSPL_OFF);	/* PCI/NVMe: read slot power limit register */
	sspl &= ~(PCIE_SSPL_VALUE_MASK | PCIE_SSPL_SCALE_MASK | PCIE_SSPL_ENABLE);	/* PCI/NVMe: clear previous NVMe power limit */
	if (port->slot_power_limit_value) {	/* PCI/NVMe: DT specified a power limit for this NVMe slot */
		sspl |= port->slot_power_limit_value << PCIE_SSPL_VALUE_SHIFT;	/* PCI/NVMe: program power limit value for NVMe */
		sspl |= port->slot_power_limit_scale << PCIE_SSPL_SCALE_SHIFT;	/* PCI/NVMe: program power limit scale for NVMe */
		sspl |= PCIE_SSPL_ENABLE;	/* PCI/NVMe: enable slot power limit message to NVMe */
	}
	mvebu_writel(port, sspl, PCIE_SSPL_OFF);	/* PCI/NVMe: commit slot power limit settings for NVMe */

	/* Mask all interrupt sources. */
	mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: mask everything until NVMe IRQ domain is ready */

	/* Clear all interrupt causes. */
	mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_CAUSE_OFF);	/* PCI/NVMe: clear stale PME/INTx causes before NVMe probe */

	/* Check if "intx" interrupt was specified in DT. */
	if (port->intx_irq > 0)
		return;	/* PCI/NVMe: per-INTx masking available, leave masked until NVMe driver requests IRQ */

	/*
	 * Fallback code when "intx" interrupt was not specified in DT:
	 * Unmask all legacy INTx interrupts as driver does not provide a way
	 * for masking and unmasking of individual legacy INTx interrupts.
	 * Legacy INTx are reported via one shared GIC source and therefore
	 * kernel cannot distinguish which individual legacy INTx was triggered.
	 * These interrupts are shared, so it should not cause any issue. Just
	 * performance penalty as every PCIe interrupt handler needs to be
	 * called when some interrupt is triggered.
	 */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: read unmask register for shared legacy NVMe INTx */
	unmask |= PCIE_INT_INTX(0) | PCIE_INT_INTX(1) |
		  PCIE_INT_INTX(2) | PCIE_INT_INTX(3);	/* PCI/NVMe: unmask all INTx lines so legacy NVMe interrupts can fire */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: commit unmask for legacy NVMe INTx fallback */
}

static struct mvebu_pcie_port *mvebu_pcie_find_port(struct mvebu_pcie *pcie,
						    struct pci_bus *bus,
						    int devfn);	/* PCI/NVMe: forward declaration for config cycle routing to NVMe root port */

static int mvebu_pcie_child_rd_conf(struct pci_bus *bus, u32 devfn, int where,
				    int size, u32 *val)	/* PCI/NVMe: config read for NVMe devices behind root port (bus != 0) */
{
	struct mvebu_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host bridge private data for NVMe controller */
	struct mvebu_pcie_port *port;	/* PCI/NVMe: target root port for this NVMe config access */
	void __iomem *conf_data;	/* PCI/NVMe: data port MMIO address for NVMe config cycle */

	port = mvebu_pcie_find_port(pcie, bus, devfn);	/* PCI/NVMe: locate root port owning this NVMe BDF */
	if (!port)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: no root port for this NVMe bus/device */

	if (!mvebu_pcie_link_up(port))
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: NVMe link down, no config response possible */

	conf_data = port->base + PCIE_CONF_DATA_OFF;	/* PCI/NVMe: compute data register address for NVMe config read */

	mvebu_writel(port, PCIE_CONF_ADDR(bus->number, devfn, where),
		     PCIE_CONF_ADDR_OFF);	/* PCI/NVMe: write config address to initiate NVMe config read cycle */

	switch (size) {	/* PCI/NVMe: match config access width used by NVMe driver enumeration */
	case 1:
		*val = readb_relaxed(conf_data + (where & 3));	/* PCI/NVMe: byte read from NVMe config space */
		break;
	case 2:
		*val = readw_relaxed(conf_data + (where & 2));	/* PCI/NVMe: word read from NVMe config space */
		break;
	case 4:
		*val = readl_relaxed(conf_data);	/* PCI/NVMe: dword read from NVMe config space */
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;	/* PCI/NVMe: unsupported config access size for NVMe */
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: NVMe config read completed */
}

static int mvebu_pcie_child_wr_conf(struct pci_bus *bus, u32 devfn,
				    int where, int size, u32 val)	/* PCI/NVMe: config write for NVMe devices behind root port */
{
	struct mvebu_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host bridge private data */
	struct mvebu_pcie_port *port;	/* PCI/NVMe: target root port for NVMe config write */
	void __iomem *conf_data;	/* PCI/NVMe: data port MMIO address */

	port = mvebu_pcie_find_port(pcie, bus, devfn);	/* PCI/NVMe: locate root port for this NVMe BDF */
	if (!port)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: no root port owns this NVMe device */

	if (!mvebu_pcie_link_up(port))
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: NVMe link down, drop config write */

	conf_data = port->base + PCIE_CONF_DATA_OFF;	/* PCI/NVMe: compute data register address */

	mvebu_writel(port, PCIE_CONF_ADDR(bus->number, devfn, where),
		     PCIE_CONF_ADDR_OFF);	/* PCI/NVMe: write config address to initiate NVMe config write cycle */

	switch (size) {	/* PCI/NVMe: match config write width for NVMe driver */
	case 1:
		writeb(val, conf_data + (where & 3));	/* PCI/NVMe: byte write to NVMe config space */
		break;
	case 2:
		writew(val, conf_data + (where & 2));	/* PCI/NVMe: word write to NVMe config space */
		break;
	case 4:
		writel(val, conf_data);	/* PCI/NVMe: dword write to NVMe config space */
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;	/* PCI/NVMe: unsupported config write size */
	}

	return PCIBIOS_SUCCESSFUL;	/* PCI/NVMe: NVMe config write completed */
}

static struct pci_ops mvebu_pcie_child_ops = {
	.read = mvebu_pcie_child_rd_conf,	/* PCI/NVMe: child bus config read for NVMe endpoint enumeration */
	.write = mvebu_pcie_child_wr_conf,	/* PCI/NVMe: child bus config write for NVMe BAR/command setup */
};

/*
 * Remove windows, starting from the largest ones to the smallest
 * ones.
 */
static void mvebu_pcie_del_windows(struct mvebu_pcie_port *port,
				   phys_addr_t base, size_t size)	/* PCI/NVMe: tear down MBus windows when NVMe BAR aperture shrinks */
{
	while (size) {	/* PCI/NVMe: loop until all NVMe window chunks removed */
		size_t sz = 1 << (fls(size) - 1);	/* PCI/NVMe: largest power-of-two chunk covering remaining NVMe aperture */

		mvebu_mbus_del_window(base, sz);	/* PCI/NVMe: remove one MBus translation window for NVMe */
		base += sz;	/* PCI/NVMe: advance to next NVMe window chunk */
		size -= sz;	/* PCI/NVMe: reduce remaining NVMe aperture size */
	}
}

/*
 * MBus windows can only have a power of two size, but PCI BARs do not
 * have this constraint. Therefore, we have to split the PCI BAR into
 * areas each having a power of two size. We start from the largest
 * one (i.e highest order bit set in the size).
 */
static int mvebu_pcie_add_windows(struct mvebu_pcie_port *port,
				   unsigned int target, unsigned int attribute,
				   phys_addr_t base, size_t size,
				   phys_addr_t remap)	/* PCI/NVMe: create MBus windows mapping NVMe BAR aperture to DRAM */
{
	size_t size_mapped = 0;	/* PCI/NVMe: amount of NVMe aperture successfully mapped so far */

	while (size) {	/* PCI/NVMe: split NVMe BAR size into power-of-two MBus windows */
		size_t sz = 1 << (fls(size) - 1);	/* PCI/NVMe: largest power-of-two chunk for current NVMe window */
		int ret;	/* PCI/NVMe: return value from MBus window creation */

		ret = mvebu_mbus_add_window_remap_by_id(target, attribute, base,
							sz, remap);	/* PCI/NVMe: add one MBus translation window for NVMe DMA/MMIO */
		if (ret) {
			phys_addr_t end = base + sz - 1;

			dev_err(&port->pcie->pdev->dev,
				"Could not create MBus window at [mem %pa-%pa]: %d\n",
				&base, &end, ret);	/* PCI/NVMe: warn that NVMe aperture mapping failed */
			mvebu_pcie_del_windows(port, base - size_mapped,
					       size_mapped);	/* PCI/NVMe: rollback partially created NVMe windows on error */
			return ret;	/* PCI/NVMe: propagate mapping failure to NVMe BAR setup */
		}

		size -= sz;	/* PCI/NVMe: reduce remaining NVMe aperture */
		size_mapped += sz;	/* PCI/NVMe: track successfully mapped NVMe aperture */
		base += sz;	/* PCI/NVMe: next chunk base for NVMe window */
		if (remap != MVEBU_MBUS_NO_REMAP)
			remap += sz;	/* PCI/NVMe: advance remap address for translated NVMe windows */
	}

	return 0;	/* PCI/NVMe: all MBus windows for NVMe aperture created */
}

static int mvebu_pcie_set_window(struct mvebu_pcie_port *port,
				  unsigned int target, unsigned int attribute,
				  const struct mvebu_pcie_window *desired,
				  struct mvebu_pcie_window *cur)	/* PCI/NVMe: update MBus window for NVMe bridge I/O or MEM range */
{
	int ret;	/* PCI/NVMe: result of window update for NVMe */

	if (desired->base == cur->base && desired->remap == cur->remap &&
	    desired->size == cur->size)
		return 0;	/* PCI/NVMe: no change in NVMe window, skip reprogramming */

	if (cur->size != 0) {
		mvebu_pcie_del_windows(port, cur->base, cur->size);	/* PCI/NVMe: remove old NVMe window before installing new one */
		cur->size = 0;	/* PCI/NVMe: mark current NVMe window disabled */
		cur->base = 0;	/* PCI/NVMe: clear current base */

		/*
		 * If something tries to change the window while it is enabled
		 * the change will not be done atomically. That would be
		 * difficult to do in the general case.
		 */
	}

	if (desired->size == 0)
		return 0;	/* PCI/NVMe: NVMe window disabled intentionally */

	ret = mvebu_pcie_add_windows(port, target, attribute, desired->base,
				     desired->size, desired->remap);	/* PCI/NVMe: create new MBus window for updated NVMe range */
	if (ret) {
		cur->size = 0;	/* PCI/NVMe: new NVMe window failed, leave disabled */
		cur->base = 0;	/* PCI/NVMe: clear base on failure */
		return ret;	/* PCI/NVMe: return error to NVMe bridge emulation layer */
	}

	*cur = *desired;	/* PCI/NVMe: record newly active NVMe window */
	return 0;	/* PCI/NVMe: NVMe window updated successfully */
}

static int mvebu_pcie_handle_iobase_change(struct mvebu_pcie_port *port)	/* PCI/NVMe: reprogram I/O window when bridge I/O base/limit changes (rarely used by NVMe) */
{
	struct mvebu_pcie_window desired = {};	/* PCI/NVMe: desired I/O window for NVMe bridge */
	struct pci_bridge_emul_conf *conf = &port->bridge.conf;	/* PCI/NVMe: emulated bridge config for NVMe */

	/* Are the new iobase/iolimit values invalid? */
	if (conf->iolimit < conf->iobase ||
	    le16_to_cpu(conf->iolimitupper) < le16_to_cpu(conf->iobaseupper))
		return mvebu_pcie_set_window(port, port->io_target, port->io_attr,
					     &desired, &port->iowin);	/* PCI/NVMe: disable I/O window for invalid NVMe bridge I/O range */

	/*
	 * We read the PCI-to-PCI bridge emulated registers, and
	 * calculate the base address and size of the address decoding
	 * window to setup, according to the PCI-to-PCI bridge
	 * specifications. iobase is the bus address, port->iowin_base
	 * is the CPU address.
	 */
	desired.remap = ((conf->iobase & 0xF0) << 8) |
			(le16_to_cpu(conf->iobaseupper) << 16);	/* PCI/NVMe: compute bus-side I/O base for NVMe */
	desired.base = port->pcie->io.start + desired.remap;	/* PCI/NVMe: CPU-side I/O base for NVMe transactions */
	desired.size = ((0xFFF | ((conf->iolimit & 0xF0) << 8) |
			 (le16_to_cpu(conf->iolimitupper) << 16)) -
			desired.remap) +
		       1;	/* PCI/NVMe: compute I/O aperture size for NVMe */

	return mvebu_pcie_set_window(port, port->io_target, port->io_attr, &desired,
				     &port->iowin);	/* PCI/NVMe: apply computed I/O window for NVMe bridge */
}

static int mvebu_pcie_handle_membase_change(struct mvebu_pcie_port *port)	/* PCI/NVMe: reprogram MEM window when bridge membase/limit changes for NVMe BARs */
{
	struct mvebu_pcie_window desired = {.remap = MVEBU_MBUS_NO_REMAP};	/* PCI/NVMe: desired MEM window, identity remap for NVMe MMIO/DMA */
	struct pci_bridge_emul_conf *conf = &port->bridge.conf;	/* PCI/NVMe: emulated bridge config */

	/* Are the new membase/memlimit values invalid? */
	if (le16_to_cpu(conf->memlimit) < le16_to_cpu(conf->membase))
		return mvebu_pcie_set_window(port, port->mem_target, port->mem_attr,
					     &desired, &port->memwin);	/* PCI/NVMe: disable MEM window for invalid NVMe range */

	/*
	 * We read the PCI-to-PCI bridge emulated registers, and
	 * calculate the base address and size of the address decoding
	 * window to setup, according to the PCI-to-PCI bridge
	 * specifications.
	 */
	desired.base = ((le16_to_cpu(conf->membase) & 0xFFF0) << 16);	/* PCI/NVMe: compute 32-bit MEM base from NVMe bridge registers */
	desired.size = (((le16_to_cpu(conf->memlimit) & 0xFFF0) << 16) | 0xFFFFF) -
		       desired.base + 1;	/* PCI/NVMe: compute MEM aperture size for NVMe BARs */

	return mvebu_pcie_set_window(port, port->mem_target, port->mem_attr, &desired,
				     &port->memwin);	/* PCI/NVMe: apply MEM window so NVMe MMIO/DMA reaches DRAM */
}

static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_base_conf_read(struct pci_bridge_emul *bridge,
				     int reg, u32 *value)	/* PCI/NVMe: emulated Type-0/1 config read for root port seen by NVMe enumeration */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: retrieve mvebu port from emulated bridge */

	switch (reg) {	/* PCI/NVMe: dispatch base config register read for NVMe */
	case PCI_COMMAND:
		*value = mvebu_readl(port, PCIE_CMD_OFF);	/* PCI/NVMe: return command register used to gate NVMe IO/MEM/BM */
		break;

	case PCI_PRIMARY_BUS: {
		/*
		 * From the whole 32bit register we support reading from HW only
		 * secondary bus number which is mvebu local bus number.
		 * Other bits are retrieved only from emulated config buffer.
		 */
		__le32 *cfgspace = (__le32 *)&bridge->conf;	/* PCI/NVMe: emulated config space buffer for NVMe bridge */
		u32 val = le32_to_cpu(cfgspace[PCI_PRIMARY_BUS / 4]);	/* PCI/NVMe: read primary/subordinate bus bytes for NVMe */
		val &= ~0xff00;	/* PCI/NVMe: clear secondary bus field, will fill from HW for NVMe */
		val |= mvebu_pcie_get_local_bus_nr(port) << 8;	/* PCI/NVMe: expose HW secondary bus to NVMe enumeration */
		*value = val;	/* PCI/NVMe: return combined value to NVMe config read */
		break;
	}

	case PCI_INTERRUPT_LINE: {
		/*
		 * From the whole 32bit register we support reading from HW only
		 * one bit: PCI_BRIDGE_CTL_BUS_RESET.
		 * Other bits are retrieved only from emulated config buffer.
		 */
		__le32 *cfgspace = (__le32 *)&bridge->conf;	/* PCI/NVMe: emulated config buffer for NVMe bridge */
		u32 val = le32_to_cpu(cfgspace[PCI_INTERRUPT_LINE / 4]);	/* PCI/NVMe: read bridge control / interrupt line for NVMe */
		if (mvebu_readl(port, PCIE_CTRL_OFF) & PCIE_CTRL_MASTER_HOT_RESET)
			val |= PCI_BRIDGE_CTL_BUS_RESET << 16;	/* PCI/NVMe: reflect secondary bus reset state to NVMe */
		else
			val &= ~(PCI_BRIDGE_CTL_BUS_RESET << 16);	/* PCI/NVMe: bus reset not asserted for NVMe */
		*value = val;	/* PCI/NVMe: return bridge control to NVMe config read */
		break;
	}

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* PCI/NVMe: let generic emulation handle other NVMe config reads */
	}

	return PCI_BRIDGE_EMUL_HANDLED;	/* PCI/NVMe: this register read handled for NVMe bridge */
}

static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_pcie_conf_read(struct pci_bridge_emul *bridge,
				     int reg, u32 *value)	/* PCI/NVMe: emulated PCIe capability read for root port seen by NVMe driver */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: mvebu port context for NVMe capability access */

	switch (reg) {	/* PCI/NVMe: dispatch PCIe capability register read for NVMe */
	case PCI_EXP_DEVCAP:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCAP);	/* PCI/NVMe: device capabilities visible to NVMe */
		break;

	case PCI_EXP_DEVCTL:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL);	/* PCI/NVMe: device control visible to NVMe */
		break;

	case PCI_EXP_LNKCAP:
		/*
		 * PCIe requires that the Clock Power Management capability bit
		 * is hard-wired to zero for downstream ports but HW returns 1.
		 * Additionally enable Data Link Layer Link Active Reporting
		 * Capable bit as DL_Active indication is provided too.
		 */
		*value = (mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP) &
			  ~PCI_EXP_LNKCAP_CLKPM) | PCI_EXP_LNKCAP_DLLLARC;	/* PCI/NVMe: sanitize link caps reported to NVMe driver */
		break;

	case PCI_EXP_LNKCTL:
		/* DL_Active indication is provided via PCIE_STAT_OFF */
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL) |
			 (mvebu_pcie_link_up(port) ?
			  (PCI_EXP_LNKSTA_DLLLA << 16) : 0);	/* PCI/NVMe: inject DLLLA into link status for NVMe driver */
		break;

	case PCI_EXP_SLTCTL: {
		u16 slotctl = le16_to_cpu(bridge->pcie_conf.slotctl);	/* PCI/NVMe: emulated slot control for NVMe */
		u16 slotsta = le16_to_cpu(bridge->pcie_conf.slotsta);	/* PCI/NVMe: emulated slot status for NVMe */
		u32 val = 0;
		/*
		 * When slot power limit was not specified in DT then
		 * ASPL_DISABLE bit is stored only in emulated config space.
		 * Otherwise reflect status of PCIE_SSPL_ENABLE bit in HW.
		 */
		if (!port->slot_power_limit_value)
			val |= slotctl & PCI_EXP_SLTCTL_ASPL_DISABLE;	/* PCI/NVMe: no HW power limit, return emulated ASPL for NVMe */
		else if (!(mvebu_readl(port, PCIE_SSPL_OFF) & PCIE_SSPL_ENABLE))
			val |= PCI_EXP_SLTCTL_ASPL_DISABLE;	/* PCI/NVMe: HW slot power limit disabled, report to NVMe */
		/* This callback is 32-bit and in high bits is slot status. */
		val |= slotsta << 16;	/* PCI/NVMe: combine slot status with control for NVMe config read */
		*value = val;
		break;
	}

	case PCI_EXP_RTSTA:
		*value = mvebu_readl(port, PCIE_RC_RTSTA);	/* PCI/NVMe: root status (PME) for NVMe power management events */
		break;

	case PCI_EXP_DEVCAP2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCAP2);	/* PCI/NVMe: device capabilities 2 for NVMe */
		break;

	case PCI_EXP_DEVCTL2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL2);	/* PCI/NVMe: device control 2 for NVMe */
		break;

	case PCI_EXP_LNKCTL2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL2);	/* PCI/NVMe: link control 2 for NVMe */
		break;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* PCI/NVMe: let generic emulation handle unknown NVMe capability reads */
	}

	return PCI_BRIDGE_EMUL_HANDLED;	/* PCI/NVMe: PCIe capability read handled for NVMe */
}

static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_ext_conf_read(struct pci_bridge_emul *bridge,
				    int reg, u32 *value)	/* PCI/NVMe: emulated AER extended capability read for NVMe error handling */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: mvebu port context for AER access */

	switch (reg) {	/* PCI/NVMe: dispatch AER register read for NVMe error reporting */
	case 0:
	case PCI_ERR_UNCOR_STATUS:
	case PCI_ERR_UNCOR_MASK:
	case PCI_ERR_UNCOR_SEVER:
	case PCI_ERR_COR_STATUS:
	case PCI_ERR_COR_MASK:
	case PCI_ERR_CAP:
	case PCI_ERR_HEADER_LOG+0:
	case PCI_ERR_HEADER_LOG+4:
	case PCI_ERR_HEADER_LOG+8:
	case PCI_ERR_HEADER_LOG+12:
	case PCI_ERR_ROOT_COMMAND:
	case PCI_ERR_ROOT_STATUS:
	case PCI_ERR_ROOT_ERR_SRC:
		*value = mvebu_readl(port, PCIE_CAP_PCIERR_OFF + reg);	/* PCI/NVMe: read AER register used for NVMe error recovery */
		break;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* PCI/NVMe: unknown AER register for NVMe */
	}

	return PCI_BRIDGE_EMUL_HANDLED;	/* PCI/NVMe: AER read handled for NVMe */
}

static void
mvebu_pci_bridge_emul_base_conf_write(struct pci_bridge_emul *bridge,
				      int reg, u32 old, u32 new, u32 mask)	/* PCI/NVMe: emulated Type-0/1 config write from NVMe enumeration path */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: mvebu port context */
	struct pci_bridge_emul_conf *conf = &bridge->conf;	/* PCI/NVMe: emulated bridge config written by NVMe core */

	switch (reg) {	/* PCI/NVMe: dispatch base config write from NVMe driver */
	case PCI_COMMAND:
		mvebu_writel(port, new, PCIE_CMD_OFF);	/* PCI/NVMe: apply NVMe-commanded IO/MEM/BusMaster enables to hardware */
		break;

	case PCI_IO_BASE:
		if ((mask & 0xffff) && mvebu_has_ioport(port) &&
		    mvebu_pcie_handle_iobase_change(port)) {	/* PCI/NVMe: NVMe wrote I/O base/limit, reprogram window */
			/* On error disable IO range */
			conf->iobase &= ~0xf0;	/* PCI/NVMe: clear valid I/O base nibble for NVMe bridge */
			conf->iolimit &= ~0xf0;	/* PCI/NVMe: clear valid I/O limit nibble for NVMe bridge */
			conf->iobase |= 0xf0;	/* PCI/NVMe: mark I/O window disabled for NVMe */
			conf->iobaseupper = cpu_to_le16(0x0000);	/* PCI/NVMe: clear upper I/O base for NVMe */
			conf->iolimitupper = cpu_to_le16(0x0000);	/* PCI/NVMe: clear upper I/O limit for NVMe */
		}
		break;

	case PCI_MEMORY_BASE:
		if (mvebu_pcie_handle_membase_change(port)) {	/* PCI/NVMe: NVMe wrote memory base/limit, reprogram MEM window */
			/* On error disable mem range */
			conf->membase = cpu_to_le16(le16_to_cpu(conf->membase) & ~0xfff0);	/* PCI/NVMe: clear membase for NVMe */
			conf->memlimit = cpu_to_le16(le16_to_cpu(conf->memlimit) & ~0xfff0);	/* PCI/NVMe: clear memlimit for NVMe */
			conf->membase = cpu_to_le16(le16_to_cpu(conf->membase) | 0xfff0);	/* PCI/NVMe: mark mem window disabled for NVMe */
		}
		break;

	case PCI_IO_BASE_UPPER16:
		if (mvebu_has_ioport(port) &&
		    mvebu_pcie_handle_iobase_change(port)) {	/* PCI/NVMe: NVMe wrote upper I/O base/limit, update I/O window */
			/* On error disable IO range */
			conf->iobase &= ~0xf0;
			conf->iolimit &= ~0xf0;
			conf->iobase |= 0xf0;
			conf->iobaseupper = cpu_to_le16(0x0000);
			conf->iolimitupper = cpu_to_le16(0x0000);
		}
		break;

	case PCI_PRIMARY_BUS:
		if (mask & 0xff00)
			mvebu_pcie_set_local_bus_nr(port, conf->secondary_bus);	/* PCI/NVMe: NVMe enumeration programmed secondary bus, update HW */
		break;

	case PCI_INTERRUPT_LINE:
		if (mask & (PCI_BRIDGE_CTL_BUS_RESET << 16)) {
			u32 ctrl = mvebu_readl(port, PCIE_CTRL_OFF);	/* PCI/NVMe: read control to perform NVMe secondary bus reset */
			if (new & (PCI_BRIDGE_CTL_BUS_RESET << 16))
				ctrl |= PCIE_CTRL_MASTER_HOT_RESET;	/* PCI/NVMe: assert hot reset to NVMe endpoint */
			else
				ctrl &= ~PCIE_CTRL_MASTER_HOT_RESET;	/* PCI/NVMe: deassert hot reset, allow NVMe link retrain */
			mvebu_writel(port, ctrl, PCIE_CTRL_OFF);	/* PCI/NVMe: commit bus reset state for NVMe */
		}
		break;

	default:
		break;	/* PCI/NVMe: other NVMe config writes ignored by hardware-specific handler */
	}
}

static void
mvebu_pci_bridge_emul_pcie_conf_write(struct pci_bridge_emul *bridge,
				      int reg, u32 old, u32 new, u32 mask)	/* PCI/NVMe: emulated PCIe capability write from NVMe driver path */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: mvebu port context */

	switch (reg) {	/* PCI/NVMe: dispatch PCIe capability write from NVMe core */
	case PCI_EXP_DEVCTL:
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL);	/* PCI/NVMe: apply NVMe-requested device control settings */
		break;

	case PCI_EXP_LNKCTL:
		/*
		 * PCIe requires that the Enable Clock Power Management bit
		 * is hard-wired to zero for downstream ports but HW allows
		 * to change it.
		 */
		new &= ~PCI_EXP_LNKCTL_CLKREQ_EN;	/* PCI/NVMe: mask CLKREQ_EN for NVMe downstream port compliance */

		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL);	/* PCI/NVMe: apply link control for NVMe ASPM/link settings */
		break;

	case PCI_EXP_SLTCTL:
		/*
		 * Allow to change PCIE_SSPL_ENABLE bit only when slot power
		 * limit was specified in DT and configured into HW.
		 */
		if ((mask & PCI_EXP_SLTCTL_ASPL_DISABLE) &&
		    port->slot_power_limit_value) {	/* PCI/NVMe: NVMe driver toggles automatic slot power limit */
			u32 sspl = mvebu_readl(port, PCIE_SSPL_OFF);	/* PCI/NVMe: read slot power limit register */
			if (new & PCI_EXP_SLTCTL_ASPL_DISABLE)
				sspl &= ~PCIE_SSPL_ENABLE;	/* PCI/NVMe: disable slot power limit message to NVMe */
			else
				sspl |= PCIE_SSPL_ENABLE;	/* PCI/NVMe: enable slot power limit message to NVMe */
			mvebu_writel(port, sspl, PCIE_SSPL_OFF);	/* PCI/NVMe: commit ASPL change for NVMe */
		}
		break;

	case PCI_EXP_RTSTA:
		/*
		 * PME Status bit in Root Status Register (PCIE_RC_RTSTA)
		 * is read-only and can be cleared only by writing 0b to the
		 * Interrupt Cause RW0C register (PCIE_INT_CAUSE_OFF). So
		 * clear PME via Interrupt Cause.
		 */
		if (new & PCI_EXP_RTSTA_PME)
			mvebu_writel(port, ~PCIE_INT_PM_PME, PCIE_INT_CAUSE_OFF);	/* PCI/NVMe: clear PME status caused by NVMe power state change */
		break;

	case PCI_EXP_DEVCTL2:
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL2);	/* PCI/NVMe: apply device control 2 for NVMe */
		break;

	case PCI_EXP_LNKCTL2:
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL2);	/* PCI/NVMe: apply link control 2 for NVMe */
		break;

	default:
		break;	/* PCI/NVMe: unknown PCIe capability write from NVMe ignored */
	}
}

static void
mvebu_pci_bridge_emul_ext_conf_write(struct pci_bridge_emul *bridge,
				     int reg, u32 old, u32 new, u32 mask)	/* PCI/NVMe: emulated AER extended capability write from NVMe error path */
{
	struct mvebu_pcie_port *port = bridge->data;	/* PCI/NVMe: mvebu port context */

	switch (reg) {	/* PCI/NVMe: dispatch AER register write from NVMe error handler */
	/* These are W1C registers, so clear other bits */
	case PCI_ERR_UNCOR_STATUS:
	case PCI_ERR_COR_STATUS:
	case PCI_ERR_ROOT_STATUS:
		new &= mask;	/* PCI/NVMe: keep only bits NVMe driver intended to clear (W1C) */
		fallthrough;

	case PCI_ERR_UNCOR_MASK:
	case PCI_ERR_UNCOR_SEVER:
	case PCI_ERR_COR_MASK:
	case PCI_ERR_CAP:
	case PCI_ERR_HEADER_LOG+0:
	case PCI_ERR_HEADER_LOG+4:
	case PCI_ERR_HEADER_LOG+8:
	case PCI_ERR_HEADER_LOG+12:
	case PCI_ERR_ROOT_COMMAND:
	case PCI_ERR_ROOT_ERR_SRC:
		mvebu_writel(port, new, PCIE_CAP_PCIERR_OFF + reg);	/* PCI/NVMe: write AER register for NVMe error recovery/masking */
		break;

	default:
		break;	/* PCI/NVMe: unknown AER write ignored */
	}
}

static const struct pci_bridge_emul_ops mvebu_pci_bridge_emul_ops = {
	.read_base = mvebu_pci_bridge_emul_base_conf_read,	/* PCI/NVMe: base config read callback for NVMe enumeration */
	.write_base = mvebu_pci_bridge_emul_base_conf_write,	/* PCI/NVMe: base config write callback for NVMe enumeration */
	.read_pcie = mvebu_pci_bridge_emul_pcie_conf_read,	/* PCI/NVMe: PCIe capability read callback for NVMe driver */
	.write_pcie = mvebu_pci_bridge_emul_pcie_conf_write,	/* PCI/NVMe: PCIe capability write callback for NVMe driver */
	.read_ext = mvebu_pci_bridge_emul_ext_conf_read,	/* PCI/NVMe: AER read callback for NVMe error handling */
	.write_ext = mvebu_pci_bridge_emul_ext_conf_write,	/* PCI/NVMe: AER write callback for NVMe error handling */
};

/*
 * Initialize the configuration space of the PCI-to-PCI bridge
 * associated with the given PCIe interface.
 */
static int mvebu_pci_bridge_emul_init(struct mvebu_pcie_port *port)	/* PCI/NVMe: set up emulated bridge for NVMe root port enumeration */
{
	unsigned int bridge_flags = PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD;	/* PCI/NVMe: no prefetchable memory forwarding for NVMe on this controller */
	struct pci_bridge_emul *bridge = &port->bridge;	/* PCI/NVMe: emulated bridge instance for this NVMe root port */
	u32 dev_id = mvebu_readl(port, PCIE_DEV_ID_OFF);	/* PCI/NVMe: read vendor/device ID for NVMe bridge header */
	u32 dev_rev = mvebu_readl(port, PCIE_DEV_REV_OFF);	/* PCI/NVMe: read class/revision for NVMe bridge */
	u32 ssdev_id = mvebu_readl(port, PCIE_SSDEV_ID_OFF);	/* PCI/NVMe: read subsystem IDs for NVMe bridge */
	u32 pcie_cap = mvebu_readl(port, PCIE_CAP_PCIEXP);	/* PCI/NVMe: read PCIe capability header for NVMe bridge */
	u8 pcie_cap_ver = ((pcie_cap >> 16) & PCI_EXP_FLAGS_VERS);	/* PCI/NVMe: extract PCIe capability version for NVMe */

	bridge->conf.vendor = cpu_to_le16(dev_id & 0xffff);	/* PCI/NVMe: set vendor ID visible to NVMe driver */
	bridge->conf.device = cpu_to_le16(dev_id >> 16);	/* PCI/NVMe: set device ID visible to NVMe driver */
	bridge->conf.class_revision = cpu_to_le32(dev_rev & 0xff);	/* PCI/NVMe: set revision visible to NVMe driver */

	if (mvebu_has_ioport(port)) {
		/* We support 32 bits I/O addressing */
		bridge->conf.iobase = PCI_IO_RANGE_TYPE_32;	/* PCI/NVMe: advertise 32-bit I/O decoding for NVMe bridge */
		bridge->conf.iolimit = PCI_IO_RANGE_TYPE_32;	/* PCI/NVMe: 32-bit I/O limit for NVMe bridge */
	} else {
		bridge_flags |= PCI_BRIDGE_EMUL_NO_IO_FORWARD;	/* PCI/NVMe: no I/O forwarding for NVMe on this port */
	}

	/*
	 * Older mvebu hardware provides PCIe Capability structure only in
	 * version 1. New hardware provides it in version 2.
	 * Enable slot support which is emulated.
	 */
	bridge->pcie_conf.cap = cpu_to_le16(pcie_cap_ver | PCI_EXP_FLAGS_SLOT);	/* PCI/NVMe: expose PCIe cap version and slot capability to NVMe */

	/*
	 * Set Presence Detect State bit permanently as there is no support for
	 * unplugging PCIe card from the slot. Assume that PCIe card is always
	 * connected in slot.
	 *
	 * Set physical slot number to port+1 as mvebu ports are indexed from
	 * zero and zero value is reserved for ports within the same silicon
	 * as Root Port which is not mvebu case.
	 *
	 * Also set correct slot power limit.
	 */
	bridge->pcie_conf.slotcap = cpu_to_le32(
		FIELD_PREP(PCI_EXP_SLTCAP_SPLV, port->slot_power_limit_value) |
		FIELD_PREP(PCI_EXP_SLTCAP_SPLS, port->slot_power_limit_scale) |
		FIELD_PREP(PCI_EXP_SLTCAP_PSN, port->port+1));	/* PCI/NVMe: build slot capability register for NVMe */
	bridge->pcie_conf.slotsta = cpu_to_le16(PCI_EXP_SLTSTA_PDS);	/* PCI/NVMe: assert presence detect so NVMe SSD is seen as inserted */

	bridge->subsystem_vendor_id = ssdev_id & 0xffff;	/* PCI/NVMe: subsystem vendor for NVMe bridge */
	bridge->subsystem_id = ssdev_id >> 16;	/* PCI/NVMe: subsystem ID for NVMe bridge */
	bridge->has_pcie = true;	/* PCI/NVMe: this emulated bridge is PCIe, required by NVMe capability parsing */
	bridge->pcie_start = PCIE_CAP_PCIEXP;	/* PCI/NVMe: PCIe capability offset used by NVMe link queries */
	bridge->data = port;	/* PCI/NVMe: callback private data points to mvebu NVMe root port */
	bridge->ops = &mvebu_pci_bridge_emul_ops;	/* PCI/NVMe: register read/write callbacks for NVMe config access */

	return pci_bridge_emul_init(bridge, bridge_flags);	/* PCI/NVMe: initialize emulation, errors prevent NVMe enumeration */
}

static inline struct mvebu_pcie *sys_to_pcie(struct pci_sys_data *sys)	/* PCI/NVMe: helper to retrieve mvebu host state from PCI sysdata for NVMe */
{
	return sys->private_data;	/* PCI/NVMe: mvebu host stored in PCI host private data used during NVMe scan */
}

static struct mvebu_pcie_port *mvebu_pcie_find_port(struct mvebu_pcie *pcie,
						    struct pci_bus *bus,
						    int devfn)	/* PCI/NVMe: find root port responsible for this NVMe bus/device */
{
	int i;	/* PCI/NVMe: port array index */

	for (i = 0; i < pcie->nports; i++) {	/* PCI/NVMe: scan all NVMe root ports */
		struct mvebu_pcie_port *port = &pcie->ports[i];	/* PCI/NVMe: candidate root port for NVMe */

		if (!port->base)
			continue;	/* PCI/NVMe: skip unprobed/failed NVMe root ports */

		if (bus->number == 0 && port->devfn == devfn)
			return port;	/* PCI/NVMe: access to virtual bus 0 root port itself */
		if (bus->number != 0 &&
		    bus->number >= port->bridge.conf.secondary_bus &&
		    bus->number <= port->bridge.conf.subordinate_bus)
			return port;	/* PCI/NVMe: access to NVMe device behind this root port */
	}

	return NULL;	/* PCI/NVMe: no root port owns this NVMe bus/device */
}

/* PCI configuration space write function */
static int mvebu_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
			      int where, int size, u32 val)	/* PCI/NVMe: top-level config write on virtual bus 0 for NVMe root ports */
{
	struct mvebu_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host bridge private data */
	struct mvebu_pcie_port *port;	/* PCI/NVMe: target root port for NVMe config write */

	port = mvebu_pcie_find_port(pcie, bus, devfn);	/* PCI/NVMe: find root port for this NVMe BDF */
	if (!port)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: no NVMe root port at this BDF */

	return pci_bridge_emul_conf_write(&port->bridge, where, size, val);	/* PCI/NVMe: route config write through emulated bridge for NVMe */
}

/* PCI configuration space read function */
static int mvebu_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
			      int size, u32 *val)	/* PCI/NVMe: top-level config read on virtual bus 0 for NVMe root ports */
{
	struct mvebu_pcie *pcie = bus->sysdata;	/* PCI/NVMe: host bridge private data */
	struct mvebu_pcie_port *port;	/* PCI/NVMe: target root port for NVMe config read */

	port = mvebu_pcie_find_port(pcie, bus, devfn);	/* PCI/NVMe: find root port for this NVMe BDF */
	if (!port)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* PCI/NVMe: no NVMe root port at this BDF */

	return pci_bridge_emul_conf_read(&port->bridge, where, size, val);	/* PCI/NVMe: route config read through emulated bridge for NVMe */
}

static struct pci_ops mvebu_pcie_ops = {
	.read = mvebu_pcie_rd_conf,	/* PCI/NVMe: root bus config read for NVMe root port enumeration */
	.write = mvebu_pcie_wr_conf,	/* PCI/NVMe: root bus config write for NVMe root port setup */
};

static void mvebu_pcie_intx_irq_mask(struct irq_data *d)	/* PCI/NVMe: mask one legacy INTx line for NVMe when driver disables IRQ */
{
	struct mvebu_pcie_port *port = d->domain->host_data;	/* PCI/NVMe: root port owning this NVMe INTx line */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* PCI/NVMe: INTx number (0=A .. 3=D) for NVMe interrupt */
	unsigned long flags;	/* PCI/NVMe: IRQ flags for spinlock */
	u32 unmask;	/* PCI/NVMe: interrupt unmask register shadow */

	raw_spin_lock_irqsave(&port->irq_lock, flags);	/* PCI/NVMe: serialize INTx mask changes with NVMe IRQ handler */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: read current INTx unmask state for NVMe */
	unmask &= ~PCIE_INT_INTX(hwirq);	/* PCI/NVMe: clear bit to mask this NVMe INTx */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: write unmask register, stopping NVMe INTx delivery */
	raw_spin_unlock_irqrestore(&port->irq_lock, flags);	/* PCI/NVMe: release lock after NVMe INTx mask update */
}

static void mvebu_pcie_intx_irq_unmask(struct irq_data *d)	/* PCI/NVMe: unmask one legacy INTx line for NVMe when driver enables IRQ */
{
	struct mvebu_pcie_port *port = d->domain->host_data;	/* PCI/NVMe: root port owning this NVMe INTx line */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* PCI/NVMe: INTx number for NVMe interrupt */
	unsigned long flags;	/* PCI/NVMe: IRQ flags for spinlock */
	u32 unmask;	/* PCI/NVMe: interrupt unmask register shadow */

	raw_spin_lock_irqsave(&port->irq_lock, flags);	/* PCI/NVMe: serialize INTx unmask changes with NVMe IRQ handler */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: read current INTx unmask state for NVMe */
	unmask |= PCIE_INT_INTX(hwirq);	/* PCI/NVMe: set bit to unmask this NVMe INTx */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: write unmask register, allowing NVMe INTx delivery */
	raw_spin_unlock_irqrestore(&port->irq_lock, flags);	/* PCI/NVMe: release lock after NVMe INTx unmask update */
}

static struct irq_chip intx_irq_chip = {
	.name = "mvebu-INTx",	/* PCI/NVMe: IRQ chip name for legacy NVMe INTx */
	.irq_mask = mvebu_pcie_intx_irq_mask,	/* PCI/NVMe: mask callback used by NVMe when freeing/disabling IRQ */
	.irq_unmask = mvebu_pcie_intx_irq_unmask,	/* PCI/NVMe: unmask callback used by NVMe when requesting/enabling IRQ */
};

static int mvebu_pcie_intx_irq_map(struct irq_domain *h,
				   unsigned int virq, irq_hw_number_t hwirq)	/* PCI/NVMe: map hardware INTx to Linux virq for NVMe */
{
	struct mvebu_pcie_port *port = h->host_data;	/* PCI/NVMe: root port for this NVMe INTx domain */

	irq_set_status_flags(virq, IRQ_LEVEL);	/* PCI/NVMe: INTx is level-triggered, important for shared NVMe interrupts */
	irq_set_chip_and_handler(virq, &intx_irq_chip, handle_level_irq);	/* PCI/NVMe: bind level handler for NVMe INTx */
	irq_set_chip_data(virq, port);	/* PCI/NVMe: store port pointer for mask/unmask during NVMe IRQ handling */

	return 0;	/* PCI/NVMe: INTx mapped successfully for NVMe */
}

static const struct irq_domain_ops mvebu_pcie_intx_irq_domain_ops = {
	.map = mvebu_pcie_intx_irq_map,	/* PCI/NVMe: map operation for NVMe INTx domain */
	.xlate = irq_domain_xlate_onecell,	/* PCI/NVMe: one-cell DT translation for NVMe INTx */
};

static int mvebu_pcie_init_irq_domain(struct mvebu_pcie_port *port)	/* PCI/NVMe: create INTx irqdomain for legacy NVMe interrupts on this port */
{
	struct device *dev = &port->pcie->pdev->dev;	/* PCI/NVMe: device for log messages */
	struct device_node *pcie_intc_node;	/* PCI/NVMe: DT interrupt controller node for NVMe INTx */

	raw_spin_lock_init(&port->irq_lock);	/* PCI/NVMe: initialize lock protecting NVMe INTx unmask register */

	pcie_intc_node = of_get_next_child(port->dn, NULL);	/* PCI/NVMe: find interrupt-controller child node for NVMe */
	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found for %s\n", port->name);
		return -ENODEV;	/* PCI/NVMe: missing DT interrupt controller for NVMe INTx */
	}

	port->intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
							 PCI_NUM_INTX,
							 &mvebu_pcie_intx_irq_domain_ops, port);	/* PCI/NVMe: create linear irqdomain for 4 legacy NVMe INTx lines */
	of_node_put(pcie_intc_node);	/* PCI/NVMe: drop DT reference after domain creation for NVMe */
	if (!port->intx_irq_domain) {
		dev_err(dev, "Failed to get INTx IRQ domain for %s\n", port->name);
		return -ENOMEM;	/* PCI/NVMe: irqdomain creation failed, NVMe INTx unavailable */
	}

	return 0;	/* PCI/NVMe: INTx irqdomain ready for NVMe */
}

static void mvebu_pcie_irq_handler(struct irq_desc *desc)	/* PCI/NVMe: chained IRQ handler demultiplexing legacy INTx for NVMe */
{
	struct mvebu_pcie_port *port = irq_desc_get_handler_data(desc);	/* PCI/NVMe: root port associated with this GIC IRQ */
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* PCI/NVMe: parent GIC irqchip */
	struct device *dev = &port->pcie->pdev->dev;	/* PCI/NVMe: device for error logging */
	u32 cause, unmask, status;	/* PCI/NVMe: interrupt cause/unmask/active status for NVMe INTx */
	int i;	/* PCI/NVMe: INTx line index */

	chained_irq_enter(chip, desc);	/* PCI/NVMe: acknowledge parent GIC before handling NVMe INTx */

	cause = mvebu_readl(port, PCIE_INT_CAUSE_OFF);	/* PCI/NVMe: read raw interrupt causes from NVMe controller */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: read current unmask state for NVMe INTx */
	status = cause & unmask;	/* PCI/NVMe: compute active, unmasked NVMe interrupts */

	/* Process legacy INTx interrupts */
	for (i = 0; i < PCI_NUM_INTX; i++) {	/* PCI/NVMe: check each INTx A-D for NVMe */
		if (!(status & PCIE_INT_INTX(i)))
			continue;	/* PCI/NVMe: this NVMe INTx line not active */

		if (generic_handle_domain_irq(port->intx_irq_domain, i) == -EINVAL)
			dev_err_ratelimited(dev, "unexpected INT%c IRQ\n", (char)i+'A');	/* PCI/NVMe: demux failed for NVMe INTx */
	}

	chained_irq_exit(chip, desc);	/* PCI/NVMe: signal parent GIC after NVMe INTx processing */
}

static int mvebu_pcie_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)	/* PCI/NVMe: map PCI device interrupt pin to Linux IRQ for NVMe */
{
	/* Interrupt support on mvebu emulated bridges is not implemented yet */
	if (dev->bus->number == 0)
		return 0; /* Proper return code 0 == NO_IRQ */	/* PCI/NVMe: root port itself has no IRQ; NVMe downstream uses DT mapping */

	return of_irq_parse_and_map_pci(dev, slot, pin);	/* PCI/NVMe: parse DT interrupt-map for NVMe INTx routing */
}

static resource_size_t mvebu_pcie_align_resource(struct pci_dev *dev,
						 const struct resource *res,
						 resource_size_t start,
						 resource_size_t size,
						 resource_size_t align)	/* PCI/NVMe: alignment hook for NVMe BAR placement on root bus */
{
	if (dev->bus->number != 0)
		return start;	/* PCI/NVMe: only adjust alignment for NVMe root ports on bus 0 */

	/*
	 * On the PCI-to-PCI bridge side, the I/O windows must have at
	 * least a 64 KB size and the memory windows must have at
	 * least a 1 MB size. Moreover, MBus windows need to have a
	 * base address aligned on their size, and their size must be a
	 * power of two. This means that if the BAR doesn't have a
	 * power of two size, several MBus windows will actually be
	 * created. We need to ensure that the biggest MBus window
	 * (which will be the first one) is aligned on its size, which
	 * explains the rounddown_pow_of_two() being done here.
	 */
	if (res->flags & IORESOURCE_IO)
		return round_up(start, max_t(resource_size_t, SZ_64K,
					    rounddown_pow_of_two(size)));	/* PCI/NVMe: align NVMe I/O window to 64KB or power-of-two */
	else if (res->flags & IORESOURCE_MEM)
		return round_up(start, max_t(resource_size_t, SZ_1M,
					    rounddown_pow_of_two(size)));	/* PCI/NVMe: align NVMe MEM window to 1MB or power-of-two */
	else
		return start;	/* PCI/NVMe: no special alignment for other NVMe resource types */
}

static void __iomem *mvebu_pcie_map_registers(struct platform_device *pdev,
					      struct device_node *np,
					      struct mvebu_pcie_port *port)	/* PCI/NVMe: iomap controller registers for NVMe root port */
{
	int ret = 0;	/* PCI/NVMe: return code from resource parsing */

	ret = of_address_to_resource(np, 0, &port->regs);	/* PCI/NVMe: get register resource from DT for NVMe controller */
	if (ret)
		return (void __iomem *)ERR_PTR(ret);	/* PCI/NVMe: propagate error, this NVMe port cannot be used */

	return devm_ioremap_resource(&pdev->dev, &port->regs);	/* PCI/NVMe: iomap registers, returns pointer used for all NVMe MMIO */
}

static int mvebu_get_tgt_attr(struct device_node *np, int devfn,
			      unsigned long type,
			      unsigned int *tgt,
			      unsigned int *attr)	/* PCI/NVMe: parse DT ranges to find MBus target/attribute for NVMe windows */
{
	struct of_range range;	/* PCI/NVMe: one DT range entry for NVMe aperture */
	struct of_range_parser parser;	/* PCI/NVMe: parser for DT ranges property */

	*tgt = -1;	/* PCI/NVMe: default invalid MBus target for NVMe */
	*attr = -1;	/* PCI/NVMe: default invalid MBus attribute for NVMe */

	if (of_pci_range_parser_init(&parser, np))
		return -EINVAL;	/* PCI/NVMe: malformed ranges property for NVMe windows */

	for_each_of_range(&parser, &range) {	/* PCI/NVMe: iterate DT ranges to find matching NVMe slot/type */
		u32 slot = upper_32_bits(range.bus_addr);	/* PCI/NVMe: slot encoded in high 32 bits for NVMe BDF */

		if (slot == PCI_SLOT(devfn) &&
		    type == (range.flags & IORESOURCE_TYPE_BITS)) {
			*tgt = (range.parent_bus_addr >> 56) & 0xFF;	/* PCI/NVMe: MBus target ID for NVMe window */
			*attr = (range.parent_bus_addr >> 48) & 0xFF;	/* PCI/NVMe: MBus attribute ID for NVMe window */
			return 0;	/* PCI/NVMe: found matching range for NVMe */
		}
	}

	return -ENOENT;	/* PCI/NVMe: no DT range matches this NVMe port/type */
}

static int mvebu_pcie_suspend(struct device *dev)	/* PCI/NVMe: system sleep suspend callback saves NVMe root port state */
{
	struct mvebu_pcie *pcie;	/* PCI/NVMe: host controller state */
	int i;	/* PCI/NVMe: port index */

	pcie = dev_get_drvdata(dev);	/* PCI/NVMe: retrieve host state for NVMe suspend */
	for (i = 0; i < pcie->nports; i++) {
		struct mvebu_pcie_port *port = pcie->ports + i;	/* PCI/NVMe: each NVMe root port */
		if (!port->base)
			continue;	/* PCI/NVMe: skip failed/uninitialized NVMe ports */
		port->saved_pcie_stat = mvebu_readl(port, PCIE_STAT_OFF);	/* PCI/NVMe: save status for NVMe resume link recovery */
	}

	return 0;	/* PCI/NVMe: suspend state saved for NVMe */
}

static int mvebu_pcie_resume(struct device *dev)	/* PCI/NVMe: system sleep resume callback restores NVMe root port state */
{
	struct mvebu_pcie *pcie;	/* PCI/NVMe: host controller state */
	int i;	/* PCI/NVMe: port index */

	pcie = dev_get_drvdata(dev);	/* PCI/NVMe: retrieve host state for NVMe resume */
	for (i = 0; i < pcie->nports; i++) {
		struct mvebu_pcie_port *port = pcie->ports + i;	/* PCI/NVMe: each NVMe root port */
		if (!port->base)
			continue;	/* PCI/NVMe: skip failed/uninitialized NVMe ports */
		mvebu_writel(port, port->saved_pcie_stat, PCIE_STAT_OFF);	/* PCI/NVMe: restore saved status to reestablish NVMe bus/dev numbers */
		mvebu_pcie_setup_hw(port);	/* PCI/NVMe: reinitialize controller for NVMe after resume */
	}

	return 0;	/* PCI/NVMe: resume completed for NVMe */
}

static void mvebu_pcie_port_clk_put(void *data)	/* PCI/NVMe: devm action to release PCIe clock for NVMe port */
{
	struct mvebu_pcie_port *port = data;	/* PCI/NVMe: port whose clock should be released */

	clk_put(port->clk);	/* PCI/NVMe: drop clock reference when NVMe port removed */
}

static int mvebu_pcie_parse_port(struct mvebu_pcie *pcie,
	struct mvebu_pcie_port *port, struct device_node *child)	/* PCI/NVMe: parse one DT child describing an NVMe root port */
{
	struct device *dev = &pcie->pdev->dev;	/* PCI/NVMe: device for logging */
	u32 slot_power_limit;	/* PCI/NVMe: parsed slot power limit in milliwatts */
	int ret;	/* PCI/NVMe: return code from parsing */
	u32 num_lanes;	/* PCI/NVMe: number of lanes from DT for NVMe link width */

	port->pcie = pcie;	/* PCI/NVMe: link port back to global host state for NVMe */

	if (of_property_read_u32(child, "marvell,pcie-port", &port->port)) {
		dev_warn(dev, "ignoring %pOF, missing pcie-port property\n",
			 child);
		goto skip;	/* PCI/NVMe: cannot identify NVMe hardware port, skip */
	}

	if (of_property_read_u32(child, "marvell,pcie-lane", &port->lane))
		port->lane = 0;	/* PCI/NVMe: default lane 0 for this NVMe root port */

	if (!of_property_read_u32(child, "num-lanes", &num_lanes) && num_lanes == 4)
		port->is_x4 = true;	/* PCI/NVMe: mark x4 link for NVMe bandwidth if DT says 4 lanes */

	port->name = devm_kasprintf(dev, GFP_KERNEL, "pcie%d.%d", port->port,
				    port->lane);	/* PCI/NVMe: allocate port name shown in NVMe host probe logs */
	if (!port->name) {
		ret = -ENOMEM;
		goto err;	/* PCI/NVMe: out of memory naming NVMe port */
	}

	port->devfn = of_pci_get_devfn(child);	/* PCI/NVMe: parse BDF for this NVMe root port on virtual bus 0 */
	if (port->devfn < 0)
		goto skip;	/* PCI/NVMe: invalid BDF, skip this NVMe port */
	if (PCI_FUNC(port->devfn) != 0) {
		dev_err(dev, "%s: invalid function number, must be zero\n",
			port->name);
		goto skip;	/* PCI/NVMe: mvebu only supports function 0 for NVMe root ports */
	}

	ret = mvebu_get_tgt_attr(dev->of_node, port->devfn, IORESOURCE_MEM,
				 &port->mem_target, &port->mem_attr);	/* PCI/NVMe: find MBus target/attr for NVMe MEM window */
	if (ret < 0) {
		dev_err(dev, "%s: cannot get tgt/attr for mem window\n",
			port->name);
		goto skip;	/* PCI/NVMe: cannot map NVMe MEM transactions, skip port */
	}

	if (resource_size(&pcie->io) != 0) {
		mvebu_get_tgt_attr(dev->of_node, port->devfn, IORESOURCE_IO,
				   &port->io_target, &port->io_attr);	/* PCI/NVMe: find MBus target/attr for NVMe I/O window if I/O exists */
	} else {
		port->io_target = -1;	/* PCI/NVMe: no I/O aperture, mark I/O target invalid for NVMe */
		port->io_attr = -1;	/* PCI/NVMe: no I/O attribute for NVMe */
	}

	/*
	 * Old DT bindings do not contain "intx" interrupt
	 * so do not fail probing driver when interrupt does not exist.
	 */
	port->intx_irq = of_irq_get_byname(child, "intx");	/* PCI/NVMe: get "intx" IRQ for legacy NVMe interrupts */
	if (port->intx_irq == -EPROBE_DEFER) {
		ret = port->intx_irq;
		goto err;	/* PCI/NVMe: deferred probe for NVMe INTx */
	}
	if (port->intx_irq <= 0) {
		dev_warn(dev, "%s: legacy INTx interrupts cannot be masked individually, "
			      "%pOF does not contain intx interrupt\n",
			 port->name, child);	/* PCI/NVMe: shared INTx fallback, per-line mask unavailable for NVMe */
	}

	port->reset_name = devm_kasprintf(dev, GFP_KERNEL, "%s-reset",
					  port->name);	/* PCI/NVMe: allocate reset GPIO label for NVMe */
	if (!port->reset_name) {
		ret = -ENOMEM;
		goto err;	/* PCI/NVMe: out of memory for NVMe reset name */
	}

	port->reset_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(child),
						 "reset", GPIOD_OUT_HIGH,
						 port->name);	/* PCI/NVMe: obtain PERST# GPIO, initially high (NVMe held in reset) */
	ret = PTR_ERR_OR_ZERO(port->reset_gpio);	/* PCI/NVMe: check GPIO acquisition result for NVMe PERST */
	if (ret) {
		if (ret != -ENOENT)
			goto err;	/* PCI/NVMe: real GPIO error for NVMe PERST */
		/* reset gpio is optional */
		port->reset_gpio = NULL;	/* PCI/NVMe: no PERST GPIO, rely on other reset means for NVMe */
		devm_kfree(dev, port->reset_name);	/* PCI/NVMe: free unused reset name for NVMe */
		port->reset_name = NULL;	/* PCI/NVMe: clear reset name */
	}

	slot_power_limit = of_pci_get_slot_power_limit(child,
							&port->slot_power_limit_value,
							&port->slot_power_limit_scale);	/* PCI/NVMe: parse DT slot power limit for NVMe power budget */
	if (slot_power_limit)
		dev_info(dev, "%s: Slot power limit %u.%uW\n",
			 port->name,
			 slot_power_limit / 1000,
			 (slot_power_limit / 100) % 10);	/* PCI/NVMe: log power limit advertised to NVMe */

	port->clk = of_clk_get_by_name(child, NULL);	/* PCI/NVMe: get PCIe reference clock for NVMe link */
	if (IS_ERR(port->clk)) {
		dev_err(dev, "%s: cannot get clock\n", port->name);
		goto skip;	/* PCI/NVMe: missing clock, cannot train NVMe link */
	}

	ret = devm_add_action_or_reset(dev, mvebu_pcie_port_clk_put, port);	/* PCI/NVMe: register clock release action for NVMe port */
	if (ret < 0)
		goto err;	/* PCI/NVMe: failed to register clock cleanup for NVMe */

	return 1;	/* PCI/NVMe: port parsed successfully, ready for NVMe probe */

skip:
	ret = 0;

	/* In the case of skipping, we need to free these */
	devm_kfree(dev, port->reset_name);	/* PCI/NVMe: free reset name if NVMe port skipped */
	port->reset_name = NULL;	/* PCI/NVMe: clear pointer */
	devm_kfree(dev, port->name);	/* PCI/NVMe: free port name if NVMe port skipped */
	port->name = NULL;	/* PCI/NVMe: clear pointer */

err:
	return ret;	/* PCI/NVMe: return 0 (skip), negative (error), or 1 (success) */
}

/*
 * Power up a PCIe port.  PCIe requires the refclk to be stable for 100µs
 * prior to releasing PERST.  See table 2-4 in section 2.6.2 AC Specifications
 * of the PCI Express Card Electromechanical Specification, 1.1.
 */
static int mvebu_pcie_powerup(struct mvebu_pcie_port *port)	/* PCI/NVMe: sequence clock and PERST# to bring NVMe SSD out of reset */
{
	int ret;	/* PCI/NVMe: result of clock enable */

	ret = clk_prepare_enable(port->clk);	/* PCI/NVMe: enable PCIe refclk for NVMe */
	if (ret < 0)
		return ret;	/* PCI/NVMe: clock enable failed, NVMe cannot train */

	if (port->reset_gpio) {
		u32 reset_udelay = PCI_PM_D3COLD_WAIT * 1000;	/* PCI/NVMe: default PERST active time for NVMe */

		of_property_read_u32(port->dn, "reset-delay-us",
				     &reset_udelay);	/* PCI/NVMe: optional DT override for NVMe reset pulse width */

		udelay(100);	/* PCI/NVMe: refclk stable 100us before PERST release per PCIe CEM */

		gpiod_set_value_cansleep(port->reset_gpio, 0);	/* PCI/NVMe: release PERST#, allowing NVMe SSD to train */
		msleep(reset_udelay / 1000);	/* PCI/NVMe: wait after PERST release for NVMe link training */
	}

	return 0;	/* PCI/NVMe: NVMe power-up sequence completed */
}

/*
 * Power down a PCIe port.  Strictly, PCIe requires us to place the card
 * in D3hot state before asserting PERST#.
 */
static void mvebu_pcie_powerdown(struct mvebu_pcie_port *port)	/* PCI/NVMe: assert PERST# and disable clock to power off NVMe SSD */
{
	gpiod_set_value_cansleep(port->reset_gpio, 1);	/* PCI/NVMe: assert PERST#, resetting NVMe endpoint */

	clk_disable_unprepare(port->clk);	/* PCI/NVMe: disable refclk to NVMe */
}

/*
 * devm_of_pci_get_host_bridge_resources() only sets up translatable resources,
 * so we need extra resource setup parsing our special DT properties encoding
 * the MEM and IO apertures.
 */
static int mvebu_pcie_parse_request_resources(struct mvebu_pcie *pcie)	/* PCI/NVMe: parse host bridge MEM/IO apertures used for NVMe BAR allocation */
{
	struct device *dev = &pcie->pdev->dev;	/* PCI/NVMe: device for resource requests */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);	/* PCI/NVMe: host bridge for NVMe resources */
	int ret;	/* PCI/NVMe: return code from resource setup */

	/* Get the PCIe memory aperture */
	mvebu_mbus_get_pcie_mem_aperture(&pcie->mem);	/* PCI/NVMe: fetch global PCIe memory aperture for NVMe BARs */
	if (resource_size(&pcie->mem) == 0) {
		dev_err(dev, "invalid memory aperture size\n");
		return -EINVAL;	/* PCI/NVMe: no memory aperture, NVMe BAR allocation impossible */
	}

	pcie->mem.name = "PCI MEM";	/* PCI/NVMe: name memory aperture for NVMe */
	pci_add_resource(&bridge->windows, &pcie->mem);	/* PCI/NVMe: add memory aperture to host bridge windows for NVMe BARs */
	ret = devm_request_resource(dev, &iomem_resource, &pcie->mem);	/* PCI/NVMe: reserve memory aperture in iomem_resource for NVMe */
	if (ret)
		return ret;	/* PCI/NVMe: memory aperture reservation failed for NVMe */

	/* Get the PCIe IO aperture */
	mvebu_mbus_get_pcie_io_aperture(&pcie->io);	/* PCI/NVMe: fetch global PCIe I/O aperture (may be empty for NVMe) */

	if (resource_size(&pcie->io) != 0) {
		pcie->realio.flags = pcie->io.flags;	/* PCI/NVMe: copy I/O flags for NVMe */
		pcie->realio.start = PCIBIOS_MIN_IO;	/* PCI/NVMe: start of real I/O space for NVMe */
		pcie->realio.end = min_t(resource_size_t,
					 IO_SPACE_LIMIT - SZ_64K,
					 resource_size(&pcie->io) - 1);	/* PCI/NVMe: limit real I/O space for NVMe */
		pcie->realio.name = "PCI I/O";

		ret = devm_pci_remap_iospace(dev, &pcie->realio, pcie->io.start);	/* PCI/NVMe: create CPU I/O mapping for NVMe I/O BARs */
		if (ret)
			return ret;	/* PCI/NVMe: I/O space remap failed for NVMe */

		pci_add_resource(&bridge->windows, &pcie->realio);	/* PCI/NVMe: add I/O aperture to host bridge windows for NVMe */
		ret = devm_request_resource(dev, &ioport_resource, &pcie->realio);	/* PCI/NVMe: reserve I/O aperture for NVMe */
		if (ret)
			return ret;	/* PCI/NVMe: I/O aperture reservation failed for NVMe */
	}

	return 0;	/* PCI/NVMe: host bridge resources parsed for NVMe */
}

static int mvebu_pcie_probe(struct platform_device *pdev)	/* PCI/NVMe: probe host controller, enumerate NVMe root ports */
{
	struct device *dev = &pdev->dev;	/* PCI/NVMe: platform device for this host controller */
	struct mvebu_pcie *pcie;	/* PCI/NVMe: host controller state for NVMe */
	struct pci_host_bridge *bridge;	/* PCI/NVMe: Linux PCI host bridge for NVMe enumeration */
	struct device_node *np = dev->of_node;	/* PCI/NVMe: DT node for host controller */
	struct device_node *child;	/* PCI/NVMe: child DT node for each NVMe root port */
	int num, i, ret;	/* PCI/NVMe: loop indices and return code */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(struct mvebu_pcie));	/* PCI/NVMe: allocate host bridge with private NVMe state */
	if (!bridge)
		return -ENOMEM;	/* PCI/NVMe: host bridge allocation failed for NVMe */

	pcie = pci_host_bridge_priv(bridge);	/* PCI/NVMe: get private area for mvebu NVMe state */
	pcie->pdev = pdev;	/* PCI/NVMe: store platform device for NVMe port references */
	platform_set_drvdata(pdev, pcie);	/* PCI/NVMe: set driver private data for suspend/remove of NVMe host */

	ret = mvebu_pcie_parse_request_resources(pcie);	/* PCI/NVMe: parse MEM/IO apertures needed for NVMe BARs */
	if (ret)
		return ret;	/* PCI/NVMe: resource parsing failed, cannot enumerate NVMe */

	num = of_get_available_child_count(np);	/* PCI/NVMe: count available NVMe root port nodes in DT */

	pcie->ports = devm_kcalloc(dev, num, sizeof(*pcie->ports), GFP_KERNEL);	/* PCI/NVMe: allocate per-port state for all potential NVMe root ports */
	if (!pcie->ports)
		return -ENOMEM;	/* PCI/NVMe: port array allocation failed for NVMe */

	i = 0;
	for_each_available_child_of_node(np, child) {
		struct mvebu_pcie_port *port = &pcie->ports[i];	/* PCI/NVMe: next candidate NVMe root port */

		ret = mvebu_pcie_parse_port(pcie, port, child);	/* PCI/NVMe: parse DT properties for this NVMe root port */
		if (ret < 0) {
			of_node_put(child);
			return ret;	/* PCI/NVMe: fatal parse error for NVMe port */
		} else if (ret == 0) {
			continue;	/* PCI/NVMe: skip invalid/incomplete NVMe port */
		}

		port->dn = child;	/* PCI/NVMe: remember DT node for this NVMe root port */
		i++;
	}
	pcie->nports = i;	/* PCI/NVMe: number of successfully parsed NVMe root ports */

	for (i = 0; i < pcie->nports; i++) {
		struct mvebu_pcie_port *port = &pcie->ports[i];	/* PCI/NVMe: current NVMe root port to initialize */
		int irq = port->intx_irq;	/* PCI/NVMe: local copy of INTx IRQ for NVMe */

		child = port->dn;
		if (!child)
			continue;	/* PCI/NVMe: no DT node, skip NVMe port init */

		ret = mvebu_pcie_powerup(port);	/* PCI/NVMe: enable clock and release PERST# for NVMe SSD */
		if (ret < 0)
			continue;	/* PCI/NVMe: powerup failed, skip this NVMe port */

		port->base = mvebu_pcie_map_registers(pdev, child, port);	/* PCI/NVMe: iomap controller registers for NVMe access */
		if (IS_ERR(port->base)) {
			dev_err(dev, "%s: cannot map registers\n", port->name);
			port->base = NULL;	/* PCI/NVMe: mark port unusable for NVMe */
			mvebu_pcie_powerdown(port);	/* PCI/NVMe: power down failed NVMe port */
			continue;
		}

		ret = mvebu_pci_bridge_emul_init(port);	/* PCI/NVMe: initialize emulated bridge for NVMe enumeration */
		if (ret < 0) {
			dev_err(dev, "%s: cannot init emulated bridge\n",
				port->name);
			devm_iounmap(dev, port->base);	/* PCI/NVMe: unmap registers on emulated bridge init failure */
			port->base = NULL;	/* PCI/NVMe: mark port failed for NVMe */
			mvebu_pcie_powerdown(port);	/* PCI/NVMe: power down failed NVMe port */
			continue;
		}

		if (irq > 0) {
			ret = mvebu_pcie_init_irq_domain(port);	/* PCI/NVMe: create INTx domain for legacy NVMe interrupts */
			if (ret) {
				dev_err(dev, "%s: cannot init irq domain\n",
					port->name);
				pci_bridge_emul_cleanup(&port->bridge);	/* PCI/NVMe: cleanup emulated bridge on IRQ domain failure */
				devm_iounmap(dev, port->base);	/* PCI/NVMe: unmap registers */
				port->base = NULL;	/* PCI/NVMe: mark port failed */
				mvebu_pcie_powerdown(port);	/* PCI/NVMe: power down failed NVMe port */
				continue;
			}
			irq_set_chained_handler_and_data(irq,
							 mvebu_pcie_irq_handler,
							 port);	/* PCI/NVMe: register chained IRQ handler for NVMe INTx */
		}

		/*
		 * PCIe topology exported by mvebu hw is quite complicated. In
		 * reality has something like N fully independent host bridges
		 * where each host bridge has one PCIe Root Port (which acts as
		 * PCI Bridge device). Each host bridge has its own independent
		 * internal registers, independent access to PCI config space,
		 * independent interrupt lines, independent window and memory
		 * access configuration. But additionally there is some kind of
		 * peer-to-peer support between PCIe devices behind different
		 * host bridges limited just to forwarding of memory and I/O
		 * transactions (forwarding of error messages and config cycles
		 * is not supported). So we could say there are N independent
		 * PCIe Root Complexes.
		 *
		 * For this kind of setup DT should have been structured into
		 * N independent PCIe controllers / host bridges. But instead
		 * structure in past was defined to put PCIe Root Ports of all
		 * host bridges into one bus zero, like in classic multi-port
		 * Root Complex setup with just one host bridge.
		 *
		 * This means that pci-mvebu.c driver provides "virtual" bus 0
		 * on which registers all PCIe Root Ports (PCI Bridge devices)
		 * specified in DT by their BDF addresses and virtually routes
		 * PCI config access of each PCI bridge device to specific PCIe
		 * host bridge.
		 *
		 * Normally PCI Bridge should choose between Type 0 and Type 1
		 * config requests based on primary and secondary bus numbers
		 * configured on the bridge itself. But because mvebu PCI Bridge
		 * does not have registers for primary and secondary bus numbers
		 * in its config space, it determinates type of config requests
		 * via its own custom way.
		 *
		 * There are two options how mvebu determinate type of config
		 * request.
		 *
		 * 1. If Secondary Bus Number Enable bit is not set or is not
		 * available (applies for pre-XP PCIe controllers) then Type 0
		 * is used if target bus number equals Local Bus Number (bits
		 * [15:8] in register 0x1a04) and target device number differs
		 * from Local Device Number (bits [20:16] in register 0x1a04).
		 * Type 1 is used if target bus number differs from Local Bus
		 * Number. And when target bus number equals Local Bus Number
		 * and target device equals Local Device Number then request is
		 * routed to Local PCI Bridge (PCIe Root Port).
		 *
		 * 2. If Secondary Bus Number Enable bit is set (bit 7 in
		 * register 0x1a2c) then mvebu hw determinate type of config
		 * request like compliant PCI Bridge based on primary bus number
		 * which is configured via Local Bus Number (bits [15:8] in
		 * register 0x1a04) and secondary bus number which is configured
		 * via Secondary Bus Number (bits [7:0] in register 0x1a2c).
		 * Local PCI Bridge (PCIe Root Port) is available on primary bus
		 * as device with Local Device Number (bits [20:16] in register
		 * 0x1a04).
		 *
		 * Secondary Bus Number Enable bit is disabled by default and
		 * option 2. is not available on pre-XP PCIe controllers. Hence
		 * this driver always use option 1.
		 *
		 * Basically it means that primary and secondary buses shares
		 * one virtual number configured via Local Bus Number bits and
		 * Local Device Number bits determinates if accessing primary
		 * or secondary bus. Set Local Device Number to 1 and redirect
		 * all writes of PCI Bridge Secondary Bus Number register to
		 * Local Bus Number (bits [15:8] in register 0x1a04).
		 *
		 * So when accessing devices on buses behind secondary bus
		 * number it would work correctly. And also when accessing
		 * device 0 at secondary bus number via config space would be
		 * correctly routed to secondary bus. Due to issues described
		 * in mvebu_pcie_setup_hw(), PCI Bridges at primary bus (zero)
		 * are not accessed directly via PCI config space but rarher
		 * indirectly via kernel emulated PCI bridge driver.
		 */
		mvebu_pcie_setup_hw(port);	/* PCI/NVMe: finalize controller setup for NVMe enumeration */
		mvebu_pcie_set_local_dev_nr(port, 1);	/* PCI/NVMe: local device number 1, root port itself at dev 0 for NVMe */
		mvebu_pcie_set_local_bus_nr(port, 0);	/* PCI/NVMe: local bus number 0, used for NVMe config routing */
	}

	bridge->sysdata = pcie;	/* PCI/NVMe: host bridge sysdata points to mvebu state for NVMe */
	bridge->ops = &mvebu_pcie_ops;	/* PCI/NVMe: root bus config ops for NVMe root ports */
	bridge->child_ops = &mvebu_pcie_child_ops;	/* PCI/NVMe: child bus config ops for NVMe endpoints */
	bridge->align_resource = mvebu_pcie_align_resource;	/* PCI/NVMe: alignment hook for NVMe root port BARs */
	bridge->map_irq = mvebu_pcie_map_irq;	/* PCI/NVMe: IRQ mapping for NVMe INTx */

	return pci_host_probe(bridge);	/* PCI/NVMe: register host bridge and enumerate NVMe SSDs */
}

static void mvebu_pcie_remove(struct platform_device *pdev)	/* PCI/NVMe: remove host controller and unbind NVMe devices */
{
	struct mvebu_pcie *pcie = platform_get_drvdata(pdev);	/* PCI/NVMe: host controller state */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);	/* PCI/NVMe: host bridge holding NVMe bus */
	u32 cmd, sspl;	/* PCI/NVMe: temporary register values for NVMe shutdown */
	int i;	/* PCI/NVMe: port index */

	/* Remove PCI bus with all devices. */
	pci_lock_rescan_remove();	/* PCI/NVMe: lock PCI bus list while removing NVMe devices */
	pci_stop_root_bus(bridge->bus);	/* PCI/NVMe: stop root bus, quiesce NVMe driver activity */
	pci_remove_root_bus(bridge->bus);	/* PCI/NVMe: remove root bus and unbind NVMe SSDs */
	pci_unlock_rescan_remove();	/* PCI/NVMe: release PCI bus lock after NVMe removal */

	for (i = 0; i < pcie->nports; i++) {
		struct mvebu_pcie_port *port = &pcie->ports[i];	/* PCI/NVMe: each NVMe root port */
		int irq = port->intx_irq;	/* PCI/NVMe: INTx IRQ for this NVMe port */

		if (!port->base)
			continue;	/* PCI/NVMe: skip failed/uninitialized NVMe ports */

		/* Disable Root Bridge I/O space, memory space and bus mastering. */
		cmd = mvebu_readl(port, PCIE_CMD_OFF);	/* PCI/NVMe: read command register for NVMe shutdown */
		cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);	/* PCI/NVMe: disable IO/MEM/BM to stop NVMe access */
		mvebu_writel(port, cmd, PCIE_CMD_OFF);	/* PCI/NVMe: commit command for NVMe port shutdown */

		/* Mask all interrupt sources. */
		mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_UNMASK_OFF);	/* PCI/NVMe: mask all interrupts for NVMe port */

		/* Clear all interrupt causes. */
		mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_CAUSE_OFF);	/* PCI/NVMe: clear pending PME/INTx for NVMe */

		if (irq > 0)
			irq_set_chained_handler_and_data(irq, NULL, NULL);	/* PCI/NVMe: unregister chained handler for NVMe INTx */

		/* Remove IRQ domains. */
		if (port->intx_irq_domain)
			irq_domain_remove(port->intx_irq_domain);	/* PCI/NVMe: remove INTx irqdomain for NVMe */

		/* Free config space for emulated root bridge. */
		pci_bridge_emul_cleanup(&port->bridge);	/* PCI/NVMe: cleanup emulated bridge state for NVMe */

		/* Disable sending Set_Slot_Power_Limit PCIe Message. */
		sspl = mvebu_readl(port, PCIE_SSPL_OFF);	/* PCI/NVMe: read slot power limit register */
		sspl &= ~(PCIE_SSPL_VALUE_MASK | PCIE_SSPL_SCALE_MASK | PCIE_SSPL_ENABLE);	/* PCI/NVMe: disable slot power limit message to NVMe */
		mvebu_writel(port, sspl, PCIE_SSPL_OFF);	/* PCI/NVMe: commit SSPL disable for NVMe */

		/* Disable and clear BARs and windows. */
		mvebu_pcie_disable_wins(port);	/* PCI/NVMe: disable BARs and MBus windows for NVMe */

		/* Delete PCIe IO and MEM windows. */
		if (port->iowin.size)
			mvebu_pcie_del_windows(port, port->iowin.base, port->iowin.size);	/* PCI/NVMe: tear down NVMe I/O windows */
		if (port->memwin.size)
			mvebu_pcie_del_windows(port, port->memwin.base, port->memwin.size);	/* PCI/NVMe: tear down NVMe MEM windows */

		/* Power down card and disable clocks. Must be the last step. */
		mvebu_pcie_powerdown(port);	/* PCI/NVMe: assert PERST# and disable clock for NVMe SSD */
	}
}

static const struct of_device_id mvebu_pcie_of_match_table[] = {
	{ .compatible = "marvell,armada-xp-pcie", },	/* PCI/NVMe: match Armada XP PCIe host controller for NVMe */
	{ .compatible = "marvell,armada-370-pcie", },	/* PCI/NVMe: match Armada 370 PCIe host controller for NVMe */
	{ .compatible = "marvell,dove-pcie", },	/* PCI/NVMe: match Dove PCIe host controller for NVMe */
	{ .compatible = "marvell,kirkwood-pcie", },	/* PCI/NVMe: match Kirkwood PCIe host controller for NVMe */
	{},
};
MODULE_DEVICE_TABLE(of, mvebu_pcie_of_match_table);	/* PCI/NVMe: export OF match table for module autoload with NVMe */

static const struct dev_pm_ops mvebu_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mvebu_pcie_suspend, mvebu_pcie_resume)	/* PCI/NVMe: suspend/resume NVMe host controller across system sleep */
};

static struct platform_driver mvebu_pcie_driver = {
	.driver = {
		.name = "mvebu-pcie",	/* PCI/NVMe: platform driver name for host controller binding */
		.of_match_table = mvebu_pcie_of_match_table,	/* PCI/NVMe: DT compatibility list for NVMe-capable SoCs */
		.pm = &mvebu_pcie_pm_ops,	/* PCI/NVMe: power management ops for NVMe host */
	},
	.probe = mvebu_pcie_probe,	/* PCI/NVMe: probe callback enumerates NVMe root ports */
	.remove = mvebu_pcie_remove,	/* PCI/NVMe: remove callback tears down NVMe host */
};
module_platform_driver(mvebu_pcie_driver);	/* PCI/NVMe: register platform driver for mvebu PCIe host controller serving NVMe */

MODULE_AUTHOR("Thomas Petazzoni <thomas.petazzoni@bootlin.com>");
MODULE_AUTHOR("Pali Rohár <pali@kernel.org>");
MODULE_DESCRIPTION("Marvell EBU PCIe controller");	/* PCI/NVMe: module description; this controller provides PCIe bus for NVMe SSDs */
MODULE_LICENSE("GPL v2");	/* PCI/NVMe: GPL v2 license, compatible with NVMe host driver */
