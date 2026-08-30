/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */

#ifndef _PCIE_RCAR_H
#define _PCIE_RCAR_H

/* PCI/NVMe: PCI Express Configuration Address Register (PCIECAR).       */
/* NVMe: Used by the host driver to form Type0/Type1 config access      */
/*       addresses when reading/writing NVMe endpoint config space.     */
#define PCIECAR			0x000010

/* PCI/NVMe: PCI Express Configuration Control Register (PCIECCTLR).    */
/* NVMe: Controls config cycle type and completion interrupt enable;    */
/*       set before kicking config read/write for the NVMe function.    */
#define PCIECCTLR		0x000018
#define  PCIECCTLR_CCIE		BIT(31) /* NVMe: completion IRQ enable     */
#define  TYPE0			(0 << 8) /* NVMe: Type0 cfg = endpoint      */
#define  TYPE1			BIT(8)   /* NVMe: Type1 cfg = bridge/RP     */

/* PCI/NVMe: PCI Express Configuration Data Register (PCIECDR).         */
/* NVMe: Data port for config accesses to NVMe VID/DID, BARs, CAPs.    */
#define PCIECDR			0x000020

/* PCI/NVMe: PCI Express Monitor Status Register (PCIEMSR).             */
/* NVMe: Reports config transaction status (done/error) after access.   */
#define PCIEMSR			0x000028

/* PCI/NVMe: PCIe INTx interrupt register.                              */
/* NVMe: Asserts/de-asserts legacy INTA for NVMe when MSI/MSI-X off.    */
#define PCIEINTXR		0x000400
#define  ASTINTX		BIT(16) /* NVMe: trigger legacy INTx msg   */

/* PCI/NVMe: PCIe PHY status register.                                  */
/* NVMe: PHYRDY must be set before any NVMe link training/config ops.   */
#define PCIEPHYSR		0x0007f0
#define  PHYRDY			BIT(0)  /* NVMe: PHY ready flag             */

/* PCI/NVMe: MSI transmit register for legacy MSI doorbell emulation.   */
/* NVMe: Host writes here to send an MSI TLP to the RC when MSI is on.  */
#define PCIEMSITXR		0x000840

/* Transfer control */
/* PCI/NVMe: PCIe transfer control register.                            */
/* NVMe: DL_DOWN tells us the data link to the NVMe device is down;     */
/*       CFINIT starts link training so NVMe can enumerate.             */
#define PCIETCTLR		0x02000
#define  DL_DOWN		BIT(3)  /* NVMe: data link down, NVMe lost */
#define  CFINIT			BIT(0)  /* NVMe: init/config cycle start   */

/* PCI/NVMe: PCIe transfer status register.                             */
/* NVMe: DATA_LINK_ACTIVE is polled before accessing NVMe config/BARs.  */
#define PCIETSTR		0x02004
#define  DATA_LINK_ACTIVE	BIT(0)  /* NVMe: link up, safe for NVMe    */

/* PCI/NVMe: PCIe error flag register.                                  */
/* NVMe: UNSUPPORTED_REQUEST may appear during NVMe config probing;     */
/*       driver must clear it to avoid stalls.                          */
#define PCIEERRFR		0x02020
#define  UNSUPPORTED_REQUEST	BIT(4)  /* NVMe: UR from missing device    */

/* PCI/NVMe: MSI flag register.                                         */
/* NVMe: Holds MSI-related event flags forwarded from NVMe interrupts.  */
#define PCIEMSIFR		0x02044

/* PCI/NVMe: MSI address lower register.                                */
/* NVMe: Lower 32 bits of the MSI target address for NVMe completions.  */
#define PCIEMSIALR		0x02048
#define  MSIFE			BIT(0)  /* NVMe: MSI address valid enable  */

/* PCI/NVMe: MSI address upper register.                                */
/* NVMe: Upper 32 bits of MSI target address; used for 64-bit MSI.      */
#define PCIEMSIAUR		0x0204c

/* PCI/NVMe: MSI interrupt enable register.                             */
/* NVMe: Masks/unmasks MSI vector reception from the NVMe device.       */
#define PCIEMSIIER		0x02050

/* root port address */
/* PCI/NVMe: Root Port address registers (indexed).                     */
/* NVMe: Stores the bus/dev/func of the R-Car root port so config       */
/*       cycles can target NVMe behind it.                              */
#define PCIEPRAR(x)		(0x02080 + ((x) * 0x4))

/* local address reg & mask */
/* PCI/NVMe: Local (CPU) address registers for inbound windows.         */
/* NVMe: Defines CPU DRAM region where NVMe DMA/PMR accesses land.      */
#define PCIELAR(x)		(0x02200 + ((x) * 0x20))

/* PCI/NVMe: Local address mask registers for inbound windows.          */
/* NVMe: Controls size, prefetch, 64-bit width, and enable of inbound   */
/*       mappings for NVMe queues/PRP buffers.                          */
#define PCIELAMR(x)		(0x02208 + ((x) * 0x20))
#define  LAM_PREFETCH		BIT(3)  /* NVMe: prefetchable inbound win  */
#define  LAM_64BIT		BIT(2)  /* NVMe: 64-bit inbound mapping    */
#define  LAR_ENABLE		BIT(1)  /* NVMe: enable inbound mapping    */

/* PCIe address reg & mask */
/* PCI/NVMe: PCIe address lower register for outbound windows.          */
/* NVMe: Lower 32 bits of PCIe bus address used to reach NVMe BARs      */
/*       and config space during nvme_probe()/BAR mapping.              */
#define PCIEPALR(x)		(0x03400 + ((x) * 0x20))

/* PCI/NVMe: PCIe address upper register for outbound windows.          */
/* NVMe: Upper 32 bits for 64-bit PCIe MMIO access to NVMe BARs.        */
#define PCIEPAUR(x)		(0x03404 + ((x) * 0x20))

/* PCI/NVMe: PCIe address mask register for outbound windows.           */
/* NVMe: Sets the size of the outbound aperture covering NVMe MMIO.     */
#define PCIEPAMR(x)		(0x03408 + ((x) * 0x20))

/* PCI/NVMe: PCIe transfer control register for outbound windows.       */
/* NVMe: Enables the window and selects MMIO/IO space for NVMe BARs.    */
#define PCIEPTCTLR(x)		(0x0340c + ((x) * 0x20))
#define  PAR_ENABLE		BIT(31) /* NVMe: enable outbound window    */
#define  IO_SPACE		BIT(8)  /* NVMe: IO space vs MEM space     */

/* Configuration */
/* PCI/NVMe: Root Port type0 config space emulation array.              */
/* NVMe: Mirrors the RC's own config header seen by the PCI core.       */
#define PCICONF(x)		(0x010000 + ((x) * 0x4))
#define  INTDIS			BIT(10) /* NVMe: disable legacy INTx       */

/* PCI/NVMe: Power Management Capability shadow region.                 */
/* NVMe: ASPM settings here affect NVMe link power and command latency. */
#define PMCAP(x)		(0x010040 + ((x) * 0x4))

/* PCI/NVMe: MSI Capability shadow region.                              */
/* NVMe: Mirrors MSI cap of the RC; NVMe driver may use MSI if enabled. */
#define MSICAP(x)		(0x010050 + ((x) * 0x4))
#define  MSICAP0_MSIE		BIT(16) /* NVMe: MSI enable bit            */
#define  MSICAP0_MMESCAP_OFFSET	17      /* NVMe: multiple msg cap offset   */
#define  MSICAP0_MMESE_OFFSET	20      /* NVMe: multiple msg enable offset*/
#define  MSICAP0_MMESE_MASK	GENMASK(22, 20) /* NVMe: MME field mask    */

/* PCI/NVMe: PCIe Express Capability shadow region.                     */
/* NVMe: Device/link capabilities visible to NVMe driver for AER/LnkCtl.*/
#define EXPCAP(x)		(0x010070 + ((x) * 0x4))

/* PCI/NVMe: Virtual Channel Capability shadow region.                  */
/* NVMe: QoS/VC arbitration used by NVMe traffic classes if enabled.    */
#define VCCAP(x)		(0x010100 + ((x) * 0x4))

/* link layer */
/* PCI/NVMe: Vendor ID / Device ID register for the root port.          */
/* NVMe: Identifies the upstream RC seen by the PCI bus during scan.    */
#define IDSETR0			0x011000

/* PCI/NVMe: Class code / revision ID register for the root port.       */
/* NVMe: Classifies the RC as a bridge; used by pci_bus_add_devices.    */
#define IDSETR1			0x011004

/* PCI/NVMe: Subsystem ID register for the root port.                   */
/* NVMe: Subsystem identification for the R-Car RC itself.              */
#define SUBIDSETR		0x011024

/* PCI/NVMe: Transaction layer control register.                        */
/* NVMe: Tuning of TL credits impacting NVMe throughput.                */
#define TLCTLR			0x011048

/* PCI/NVMe: MAC status register.                                       */
/* NVMe: Reports link speed/width negotiation result for NVMe link.     */
#define MACSR			0x011054
#define  SPCHGFIN		BIT(4)  /* NVMe: speed change finished     */
#define  SPCHGFAIL		BIT(6)  /* NVMe: speed change failed       */
#define  SPCHGSUC		BIT(7)  /* NVMe: speed change succeeded    */
#define  LINK_SPEED		(0xf << 16) /* NVMe: current link speed mask */
#define  LINK_SPEED_2_5GTS	(1 << 16)   /* NVMe: Gen1 speed             */
#define  LINK_SPEED_5_0GTS	(2 << 16)   /* NVMe: Gen2 speed             */

/* PCI/NVMe: MAC control register.                                      */
/* NVMe: Controls speed change, scrambling, and FTS for NVMe link.      */
#define MACCTLR			0x011058
#define  MACCTLR_NFTS_MASK	GENMASK(23, 16)	/* The name is from SH7786 */
#define  SPEED_CHANGE		BIT(24) /* NVMe: request link speed change */
#define  SCRAMBLE_DISABLE	BIT(27) /* NVMe: disable scrambling        */
#define  LTSMDIS		BIT(31) /* NVMe: disable link training SM  */
#define  MACCTLR_INIT_VAL	(LTSMDIS | MACCTLR_NFTS_MASK) /* NVMe: init */

/* PCI/NVMe: Power management status register.                          */
/* NVMe: Reports ASPM L1 entry/exit events from the NVMe link.          */
#define PMSR			0x01105c
#define  L1FAEG			BIT(31) /* NVMe: L1 fail entry/exit event  */
#define  PMEL1RX		BIT(23) /* NVMe: PM enter L1 received        */
#define  PMSTATE		GENMASK(18, 16) /* NVMe: current PM state     */
#define  PMSTATE_L1		(3 << 16)       /* NVMe: link in L1           */

/* PCI/NVMe: Power management control register.                         */
/* NVMe: Software can trigger L1 entry/exit for NVMe ASPM management.   */
#define PMCTLR			0x011060
#define  L1IATN			BIT(31) /* NVMe: L1 in attention/req       */

/* PCI/NVMe: MAC status 2 register.                                     */
/* NVMe: Additional link status used during NVMe link bring-up.         */
#define MACS2R			0x011078

/* PCI/NVMe: MAC change speed preset register.                          */
/* NVMe: Used to force or acknowledge a link speed change for NVMe.     */
#define MACCGSPSETR		0x011084
#define  SPCNGRSN		BIT(31) /* NVMe: speed change reason flag  */

/* R-Car H1 PHY */
/* PCI/NVMe: R-Car H1 PHY indirect address register.                    */
/* NVMe: Programs H1 PHY parameters required before NVMe link training. */
#define H1_PCIEPHYADRR		0x04000c
#define  WRITE_CMD		BIT(16) /* NVMe: PHY write command           */
#define  PHY_ACK		BIT(24) /* NVMe: PHY acknowledge             */
#define  RATE_POS		12      /* NVMe: rate field position         */
#define  LANE_POS		8       /* NVMe: lane field position         */
#define  ADR_POS		0       /* NVMe: address field position      */

/* PCI/NVMe: R-Car H1 PHY indirect data output register.                */
/* NVMe: Holds read-back PHY data after tuning for NVMe signal quality. */
#define H1_PCIEPHYDOUTR		0x040014

/* R-Car Gen2 PHY */
/* PCI/NVMe: R-Car Gen2 PHY indirect address register.                  */
/* NVMe: Used to tune Gen2 PHY equalization for reliable NVMe Gen2.     */
#define GEN2_PCIEPHYADDR	0x780

/* PCI/NVMe: R-Car Gen2 PHY data register.                              */
/* NVMe: Holds PHY read/write data affecting NVMe link margin.          */
#define GEN2_PCIEPHYDATA	0x784

/* PCI/NVMe: R-Car Gen2 PHY control register.                           */
/* NVMe: Controls PHY read/write handshakes during NVMe init.           */
#define GEN2_PCIEPHYCTRL	0x78c

/* PCI/NVMe: Number of MSI vectors the RC can deliver.                  */
/* NVMe: Caps vectors available to NVMe; drivers/nvme/host/pci.c tries  */
/*       to request enough vectors for per-queue MSIs.                  */
#define INT_PCI_MSI_NR		32

/* PCI/NVMe: Convenience offsets into the emulated root config space.   */
/* NVMe: Used by the RC driver when setting up the bridge seen by NVMe. */
#define RCONF(x)		(PCICONF(0) + (x))
#define RPMCAP(x)		(PMCAP(0) + (x))
#define REXPCAP(x)		(EXPCAP(0) + (x))
#define RVCCAP(x)		(VCCAP(0) + (x))

/* PCI/NVMe: Build a PCIe config address for a given bus.               */
/* NVMe: Encodes the bus number of the NVMe device or upstream bridge.  */
#define PCIE_CONF_BUS(b)	(((b) & 0xff) << 24)

/* PCI/NVMe: Build a PCIe config address for a given device.            */
/* NVMe: Encodes the device number where the NVMe SSD appears.          */
#define PCIE_CONF_DEV(d)	(((d) & 0x1f) << 19)

/* PCI/NVMe: Build a PCIe config address for a given function.          */
/* NVMe: Encodes the function number of the NVMe controller.            */
#define PCIE_CONF_FUNC(f)	(((f) & 0x7) << 16)

/* PCI/NVMe: Maximum number of PCI bridge windows supported.            */
/* NVMe: Limits BAR/MMIO resource assignment for the NVMe endpoint.     */
#define RCAR_PCI_MAX_RESOURCES	4

/* PCI/NVMe: Maximum number of inbound address translation windows.     */
/* NVMe: Enough to map DRAM regions for NVMe command queues and PRPs.   */
#define MAX_NR_INBOUND_MAPS	6

/* PCI/NVMe: Per-controller private data for the R-Car PCIe host.       */
/* NVMe: This is the bridge between the PCI core and the R-Car HW;      */
/*       NVMe enumeration, DMA, and IRQ all go through this struct.     */
struct rcar_pcie {
	struct device		*dev;  /* NVMe: device for DMA/IRQ alloc    */
	void __iomem		*base; /* NVMe: MMIO base for all RC regs   */
};

/* PCI/NVMe: Direction selector for rcar_pcie_config_read/write.        */
/* NVMe: Distinguishes config reads (e.g. reading NVMe BAR0) from       */
/*       config writes (e.g. writing NVMe command register).            */
enum {
	RCAR_PCI_ACCESS_READ,
	RCAR_PCI_ACCESS_WRITE,
};

/* PCI/NVMe: Write a 32-bit value to a R-Car PCIe register.             */
/* NVMe: Used for all host-side setup affecting NVMe link/windows/IRQ.  */
void rcar_pci_write_reg(struct rcar_pcie *pcie, u32 val, unsigned int reg);

/* PCI/NVMe: Read a 32-bit value from a R-Car PCIe register.            */
/* NVMe: Used to poll link status, error flags, and PHY state for NVMe. */
u32 rcar_pci_read_reg(struct rcar_pcie *pcie, unsigned int reg);

/* PCI/NVMe: Read-modify-write helper for config space fields.          */
/* NVMe: Applied to NVMe function config space via PCIECAR/PCIECDR.     */
void rcar_rmw32(struct rcar_pcie *pcie, int where, u32 mask, u32 data);

/* PCI/NVMe: Block until the PCIe PHY reports ready.                    */
/* NVMe: Must succeed before any NVMe device can be detected on the     */
/*       link; failure means NVMe will never probe.                     */
int rcar_pcie_wait_for_phyrdy(struct rcar_pcie *pcie);

/* PCI/NVMe: Block until the PCIe data link is active.                  */
/* NVMe: Confirms the NVMe device is physically present and link-trained.*/
int rcar_pcie_wait_for_dl(struct rcar_pcie *pcie);

/* PCI/NVMe: Program an outbound (CPU -> PCIe) address window.          */
/* NVMe: Maps the CPU address space to NVMe BARs and config space so    */
/*       the NVMe driver can access registers with ioremap()/readl().   */
void rcar_pcie_set_outbound(struct rcar_pcie *pcie, int win,
			    struct resource_entry *window);

/* PCI/NVMe: Program an inbound (PCIe -> CPU) address window.           */
/* NVMe: Maps PCIe bus addresses to CPU DRAM so NVMe DMA to queues,     */
/*       PRP lists, and SGLs reaches the correct physical memory.       */
void rcar_pcie_set_inbound(struct rcar_pcie *pcie, u64 cpu_addr,
			   u64 pci_addr, u64 flags, int idx, bool host);

#endif
