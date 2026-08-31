// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Marvell
 *
 * Author: Thomas Petazzoni <thomas.petazzoni@bootlin.com>
 *
 * This file helps PCI controller drivers implement a fake root port
 * PCI bridge when the HW doesn't provide such a root port PCI
 * bridge.
 *
 * It emulates a PCI bridge by providing a fake PCI configuration
 * space (and optionally a PCIe capability configuration space) in
 * memory. By default the read/write operations simply read and update
 * this fake configuration space in memory. However, PCI controller
 * drivers can provide through the 'struct pci_sw_bridge_ops'
 * structure a set of operations to override or complement this
 * default behavior.
 */

/*
 * [한국어 설명] 하드웨어에 없는 루트 포트를 소프트웨어로 흉내 낸다 (pci-bridge-emul.c)
 *
 * === 파일의 역할 ===
 * 위 원문 주석이 목적을 밝힌다 — 하드웨어가 루트 포트 브리지를 제공하지
 * 않을 때, 그 브리지의 config space 를 메모리에 만들어 있는 척한다.
 *
 * 왜 필요한가. Linux PCI 코어는 계층 구조를 전제로 만들어져 있다.
 * 엔드포인트는 반드시 어떤 브리지 아래에 있어야 하고, 그 브리지의
 * 윈도우 레지스터로 주소가 라우팅된다고 가정한다. 그런데 일부 SoC 의
 * PCIe 컨트롤러는 그런 브리지를 config space 로 노출하지 않는다 —
 * 하드웨어 안에서 알아서 처리하고 소프트웨어에게는 보여 주지 않는다.
 *
 * 그러면 커널이 곤란해진다. 브리지가 없으니 엔드포인트를 매달 곳이 없고,
 * 윈도우가 없으니 자원 배치도 할 수 없다. 이 파일은 가짜 브리지를 만들어
 * 그 간극을 메운다. 커널은 평범한 브리지를 다루듯 동작하고, 컨트롤러
 * 드라이버는 그 가짜 레지스터에 대한 읽기·쓰기를 실제 하드웨어 동작으로
 * 번역한다.
 *
 * 핵심 자료구조가 pci_regs_behavior 배열이다. config space 의 dword 마다
 * 세 개의 마스크를 둔다.
 *   ro  — 읽기 전용 비트. 쓰기가 무시된다.
 *   rw  — 읽고 쓸 수 있는 비트.
 *   w1c — 1 을 쓰면 지워지는 비트(오류 상태 등).
 * 이 세 마스크로 각 비트의 성질을 정확히 흉내 낸다. 예컨대 Status
 * 레지스터의 오류 비트는 w1c 여야 하고, Vendor ID 는 ro 여야 한다.
 * 이것을 제대로 하지 않으면 커널이 이상한 동작을 한다 — 예를 들어
 * 지워지지 않는 오류 비트를 보고 무한히 복구를 시도한다.
 *
 * PCIe capability 도 선택적으로 흉내 낸다. 그래야 커널이 이 브리지를
 * PCIe 브리지로 인식하고 링크 상태나 슬롯 제어를 시도한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 초기화: SoC 의 PCIe 컨트롤러 드라이버(controller/ 아래)
 *           -> [이 파일] pci_bridge_emul_init(bridge, flags)
 *              -> 가짜 config space 를 메모리에 잡고
 *              -> 각 dword 의 ro/rw/w1c 마스크를 채운다
 *              -> flags 로 지원하지 않는 기능을 읽기 전용 0 으로 고정
 *
 * 접근:  커널이 루트 포트의 config 를 읽거나 쓸 때
 *           -> 컨트롤러 드라이버의 config ops
 *              -> [이 파일] pci_bridge_emul_conf_read/write()
 *                 -> 기본은 메모리의 가짜 값을 읽고 쓴다
 *                 -> 드라이버가 ops 를 제공했으면 그것을 먼저 부른다
 *                    (실제 하드웨어와 동기화가 필요한 레지스터용)
 *
 * 실행 컨텍스트: config 접근 경로이므로 pci_lock 을 쥔 상태에서 불린다.
 * 잠들 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: controller/ 아래의 여러 SoC 드라이버 — mvebu, aardvark,
 *   brcmstb, rockchip 등 브리지를 노출하지 않는 컨트롤러들.
 * 아래쪽: 없다. 메모리 조작과 콜백 호출뿐이다.
 * 공유 상태: struct pci_bridge_emul — 가짜 config space(conf, pcie_conf),
 *   동작 마스크(pci_regs_behavior, pcie_cap_regs_behavior),
 *   그리고 드라이버가 제공한 ops 와 사설 데이터.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 직접 관련이 없다(전수 확인).
 *
 * 하지만 브리지를 노출하지 않는 SoC 에 NVMe 를 붙이면 이 에뮬레이션이
 * 그 사이에 놓인다. NVMe 컨트롤러가 열거되려면 그것을 담을 버스가
 * 있어야 하고, 그 버스를 만드는 것이 이 가짜 브리지다.
 *
 * 특히 자원 배치가 이것에 달려 있다. NVMe 의 BAR0 주소를 정하려면
 * 상위 브리지의 메모리 윈도우가 있어야 하는데(setup-bus.c),
 * 그 윈도우 레지스터가 바로 이 파일이 흉내 내는 것이다. 에뮬레이션이
 * 윈도우를 제대로 보고하지 않으면 NVMe 의 BAR 가 배정되지 않아
 * probe 가 실패한다.
 *
 * (기존 주석은 이 에뮬레이션이 "MSI/MSI-X, AER, Virtual Channel, NPEM,
 *  port driver 가 정상 동작하려면" 필요하다고 적었으나, 그 기능들은
 *  엔드포인트나 실제 포트의 capability 에 달린 것이고 이 가짜 브리지와
 *  직접 관계가 없다. 이 파일이 실제로 좌우하는 것은 열거와 자원 배치다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_bridge_emul_init()        : 가짜 브리지를 만든다. flags 로 지원하지
 *                                 않는 기능(I/O 포워딩, prefetchable 메모리)을
 *                                 읽기 전용 0 으로 고정할 수 있다.
 * pci_bridge_emul_cleanup()     : 할당한 마스크 배열을 해제한다.
 * pci_bridge_emul_conf_read()   : 가짜 config 를 읽는다. 드라이버 ops 가
 *                                 있으면 먼저 불러 값을 갱신할 기회를 준다.
 * pci_bridge_emul_conf_write()  : 쓴다. ro/rw/w1c 마스크에 따라 각 비트를
 *                                 다르게 처리하는 것이 이 함수의 핵심이다.
 * struct pci_bridge_emul        : 가짜 브리지 하나의 전부.
 * struct pci_bridge_emul_ops    : 컨트롤러 드라이버가 제공하는 훅.
 *                                 읽기 전/쓰기 후에 실제 하드웨어와
 *                                 동기화할 기회를 준다.
 * struct pci_bridge_reg_behavior: dword 하나의 ro/rw/w1c 마스크 세 개.
 */

#include <linux/pci.h>
#include "pci-bridge-emul.h"
#define PCI_BRIDGE_CONF_END	PCI_STD_HEADER_SIZEOF

#define PCI_CAP_SSID_SIZEOF	(PCI_SSVID_DEVICE_ID + 2)
#define PCI_CAP_PCIE_SIZEOF	(PCI_EXP_SLTSTA2 + 2)
/**
 * struct pci_bridge_reg_behavior - register bits behaviors
 * @ro:		Read-Only bits
 * @rw:		Read-Write bits
 * @w1c:	Write-1-to-Clear bits
 *
 * Reads and Writes will be filtered by specified behavior. All other bits not
 * declared are assumed 'Reserved' and will return 0 on reads, per PCIe 5.0:
 * "Reserved register fields must be read only and must return 0 (all 0's for
 * multi-bit fields) when read".
 */
struct pci_bridge_reg_behavior {
	/* Read-only bits */
	u32 ro;
	/* Read-write bits */
	u32 rw;

	/* Write-1-to-clear bits */
	u32 w1c;
};

static const
struct pci_bridge_reg_behavior pci_regs_behavior[PCI_STD_HEADER_SIZEOF / 4] = {

	[PCI_VENDOR_ID / 4] = { .ro = ~0 },

	[PCI_COMMAND / 4] = {
		.rw = (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		       PCI_COMMAND_MASTER | PCI_COMMAND_PARITY |
		       PCI_COMMAND_SERR),

		.ro = ((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			PCI_COMMAND_FAST_BACK) |
		       (PCI_STATUS_CAP_LIST | PCI_STATUS_66MHZ |
			PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MASK) << 16),

		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_CLASS_REVISION / 4] = { .ro = ~0 },

	/*
	 * Cache Line Size register: implement as read-only, we do not
	 * pretend implementing "Memory Write and Invalidate"
	 * transactions"
	 *
	 * Latency Timer Register: implemented as read-only, as "A
	 * bridge that is not capable of a burst transfer of more than
	 * two data phases on its primary interface is permitted to
	 * hardwire the Latency Timer to a value of 16 or less"
	 *
	 * Header Type: always read-only
	 *
	 * BIST register: implemented as read-only, as "A bridge that
	 * does not support BIST must implement this register as a
	 * read-only register that returns 0 when read"
	 */
	[PCI_CACHE_LINE_SIZE / 4] = { .ro = ~0 },

	/*
	 * Base Address registers not used must be implemented as
	 * read-only registers that return 0 when read.
	 */
	[PCI_BASE_ADDRESS_0 / 4] = { .ro = ~0 },
	[PCI_BASE_ADDRESS_1 / 4] = { .ro = ~0 },

	[PCI_PRIMARY_BUS / 4] = {
		/* Primary, secondary and subordinate bus are RW */
		.rw = GENMASK(24, 0),

		/* Secondary latency is read-only */
		.ro = GENMASK(31, 24),
	},

	[PCI_IO_BASE / 4] = {
		/* The high four bits of I/O base/limit are RW */
		.rw = (GENMASK(15, 12) | GENMASK(7, 4)),

		/* The low four bits of I/O base/limit are RO */
		.ro = (((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			 PCI_STATUS_DEVSEL_MASK) << 16) |
		       GENMASK(11, 8) | GENMASK(3, 0)),

		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_MEMORY_BASE / 4] = {
		/* The high 12-bits of mem base/limit are RW */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of mem base/limit are RO */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_MEMORY_BASE / 4] = {
		/* The high 12-bits of pref mem base/limit are RW */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of pref mem base/limit are RO */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_BASE_UPPER32 / 4] = {
		.rw = ~0,
	},

	[PCI_PREF_LIMIT_UPPER32 / 4] = {
		.rw = ~0,
	},

	[PCI_IO_BASE_UPPER16 / 4] = {
		.rw = ~0,
	},

	[PCI_CAPABILITY_LIST / 4] = {
		.ro = GENMASK(7, 0),
	},

	/*
	 * If expansion ROM is unsupported then ROM Base Address register must
	 * be implemented as read-only register that return 0 when read, same
	 * as for unused Base Address registers.
	 */
	[PCI_ROM_ADDRESS1 / 4] = {
		.ro = ~0,
	},

	/*
	 * Interrupt line (bits 7:0) are RW, interrupt pin (bits 15:8)
	 * are RO, and bridge control (31:16) are a mix of RW, RO,
	 * reserved and W1C bits
	 */
	[PCI_INTERRUPT_LINE / 4] = {
		/* Interrupt line is RW */
		.rw = (GENMASK(7, 0) |
		       ((PCI_BRIDGE_CTL_PARITY |
			 PCI_BRIDGE_CTL_SERR |
			 PCI_BRIDGE_CTL_ISA |
			 PCI_BRIDGE_CTL_VGA |
			 PCI_BRIDGE_CTL_MASTER_ABORT |
			 PCI_BRIDGE_CTL_BUS_RESET |
			 BIT(8) | BIT(9) | BIT(11)) << 16)),

		/* Interrupt pin is RO */
		.ro = (GENMASK(15, 8) | ((PCI_BRIDGE_CTL_FAST_BACK) << 16)),

		.w1c = BIT(10) << 16,
	},
};

static const
struct pci_bridge_reg_behavior pcie_cap_regs_behavior[PCI_CAP_PCIE_SIZEOF / 4] = {

	[PCI_CAP_LIST_ID / 4] = {
		/*
		 * Capability ID, Next Capability Pointer and
		 * bits [14:0] of Capabilities register are all read-only.
		 * Bit 15 of Capabilities register is reserved.
		 */
		.ro = GENMASK(30, 0),
	},

	[PCI_EXP_DEVCAP / 4] = {
		/*
		 * Bits [31:29] and [17:16] are reserved.
		 * Bits [27:18] are reserved for non-upstream ports.
		 * Bits 28 and [14:6] are reserved for non-endpoint devices.
		 * Other bits are read-only.
		 */
		.ro = BIT(15) | GENMASK(5, 0),
	},

	[PCI_EXP_DEVCTL / 4] = {
		/*
		 * Device control register is RW, except bit 15 which is
		 * reserved for non-endpoints or non-PCIe-to-PCI/X bridges.
		 */
		.rw = GENMASK(14, 0),

		/*
		 * Device status register has bits 6 and [3:0] W1C, [5:4] RO,
		 * the rest is reserved. Also bit 6 is reserved for non-upstream
		 * ports.
		 */
		.w1c = GENMASK(3, 0) << 16,
		.ro = GENMASK(5, 4) << 16,
	},

	[PCI_EXP_LNKCAP / 4] = {
		/*
		 * All bits are RO, except bit 23 which is reserved and
		 * bit 18 which is reserved for non-upstream ports.
		 */
		.ro = lower_32_bits(~(BIT(23) | PCI_EXP_LNKCAP_CLKPM)),
	},

	[PCI_EXP_LNKCTL / 4] = {
		/*
		 * Link control has bits [15:14], [11:3] and [1:0] RW, the
		 * rest is reserved. Bit 8 is reserved for non-upstream ports.
		 *
		 * Link status has bits [13:0] RO, and bits [15:14]
		 * W1C.
		 */
		.rw = GENMASK(15, 14) | GENMASK(11, 9) | GENMASK(7, 3) | GENMASK(1, 0),
		.ro = GENMASK(13, 0) << 16,
		.w1c = GENMASK(15, 14) << 16,
	},

	[PCI_EXP_SLTCAP / 4] = {
		.ro = ~0,
	},

	[PCI_EXP_SLTCTL / 4] = {
		/*
		 * Slot control has bits [14:0] RW, the rest is
		 * reserved.
		 *
		 * Slot status has bits 8 and [4:0] W1C, bits [7:5] RO, the
		 * rest is reserved.
		 */
		.rw = GENMASK(14, 0),
		.w1c = (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
			PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_PDC |
			PCI_EXP_SLTSTA_CC | PCI_EXP_SLTSTA_DLLSC) << 16,
		.ro = (PCI_EXP_SLTSTA_MRLSS | PCI_EXP_SLTSTA_PDS |
		       PCI_EXP_SLTSTA_EIS) << 16,
	},

	[PCI_EXP_RTCTL / 4] = {
		/*
		 * Root control has bits [4:0] RW, the rest is
		 * reserved.
		 *
		 * Root capabilities has bit 0 RO, the rest is reserved.
		 */
		.rw = (PCI_EXP_RTCTL_SECEE | PCI_EXP_RTCTL_SENFEE |
		       PCI_EXP_RTCTL_SEFEE | PCI_EXP_RTCTL_PMEIE |
		       PCI_EXP_RTCTL_RRS_SVE),
		.ro = PCI_EXP_RTCAP_RRS_SV << 16,
	},

	[PCI_EXP_RTSTA / 4] = {
		/*
		 * Root status has bits 17 and [15:0] RO, bit 16 W1C, the rest
		 * is reserved.
		 */
		.ro = GENMASK(15, 0) | PCI_EXP_RTSTA_PENDING,
		.w1c = PCI_EXP_RTSTA_PME,
	},

	[PCI_EXP_DEVCAP2 / 4] = {
		/*
		 * Device capabilities 2 register has reserved bits [30:27].
		 * Also bits [26:24] are reserved for non-upstream ports.
		 */
		.ro = BIT(31) | GENMASK(23, 0),
	},

	[PCI_EXP_DEVCTL2 / 4] = {
		/*
		 * Device control 2 register is RW. Bit 11 is reserved for
		 * non-upstream ports.
		 *
		 * Device status 2 register is reserved.
		 */
		.rw = GENMASK(15, 12) | GENMASK(10, 0),
	},

	[PCI_EXP_LNKCAP2 / 4] = {
		/* Link capabilities 2 register has reserved bits [30:25] and 0. */
		.ro = BIT(31) | GENMASK(24, 1),
	},

	[PCI_EXP_LNKCTL2 / 4] = {
		/*
		 * Link control 2 register is RW.
		 *
		 * Link status 2 register has bits 5, 15 W1C;
		 * bits 10, 11 reserved and others are RO.
		 */
		.rw = GENMASK(15, 0),
		.w1c = (BIT(15) | BIT(5)) << 16,
		.ro = (GENMASK(14, 12) | GENMASK(9, 6) | GENMASK(4, 0)) << 16,
	},

	[PCI_EXP_SLTCAP2 / 4] = {
		/* Slot capabilities 2 register is reserved. */
	},

	[PCI_EXP_SLTCTL2 / 4] = {
		/* Both Slot control 2 and Slot status 2 registers are reserved. */
	},
};

/*
 * NVMe: Subsystem Vendor ID capability를 읽을 때 호출되는 헬퍼 함수.
 * PCI core나 NVMe 드라이버가 상위 브리지의 서브시스템 정보를 조회하면
 * capability chain을 따라 이 함수가 응답한다.
 */
static pci_bridge_emul_read_status_t
pci_bridge_emul_read_ssid(struct pci_bridge_emul *bridge, int reg, u32 *value)
{
	switch (reg) {
	case PCI_CAP_LIST_ID:
		*value = PCI_CAP_ID_SSVID |
			((bridge->pcie_start > bridge->ssid_start) ? (bridge->pcie_start << 8) : 0);
		return PCI_BRIDGE_EMUL_HANDLED;

	case PCI_SSVID_VENDOR_ID:
		*value = bridge->subsystem_vendor_id |
			(bridge->subsystem_id << 16);
		return PCI_BRIDGE_EMUL_HANDLED;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;
	}
}

/*
 * Initialize a pci_bridge_emul structure to represent a fake PCI
 * bridge configuration space. The caller needs to have initialized
 * the PCI configuration space with whatever values make sense
 * (typically at least vendor, device, revision), the ->ops pointer,
 * and optionally ->data and ->has_pcie.
 */
/*
 * NVMe: pci_bridge_emul 구조체를 초기화하여 가짜 PCI 브리지 설정 공간을
 * 구성한다. PCI 컨트롤러 드라이버는 먼저 vendor/device/revision, ops,
 * has_pcie, subsystem 정보 등을 채워둔 후 이 함수를 호출해야 한다.
 * 초기화가 완료되면 NVMe 장치가 연결될 하위 버스를 위한 논리적 Root
 * Port가 생성된다.
 */
int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags)
{
	BUILD_BUG_ON(sizeof(bridge->conf) != PCI_BRIDGE_CONF_END);

	/*
	 * class_revision: Class is high 24 bits and revision is low 8 bit
	 * of this member, while class for PCI Bridge Normal Decode has the
	 * 24-bit value: PCI_CLASS_BRIDGE_PCI_NORMAL
	 */
	bridge->conf.class_revision |=
		cpu_to_le32(PCI_CLASS_BRIDGE_PCI_NORMAL << 8);
	bridge->conf.header_type = PCI_HEADER_TYPE_BRIDGE;
	bridge->conf.cache_line_size = 0x10;
	bridge->conf.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
	bridge->pci_regs_behavior = kmemdup(pci_regs_behavior,
					    sizeof(pci_regs_behavior),
					    GFP_KERNEL);
	if (!bridge->pci_regs_behavior)
		return -ENOMEM;

	/* If ssid_start and pcie_start were not specified then choose the lowest possible value. */
	if (!bridge->ssid_start && !bridge->pcie_start) {
		if (bridge->subsystem_vendor_id)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		if (bridge->has_pcie)
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	} else if (!bridge->ssid_start && bridge->subsystem_vendor_id) {
		if (bridge->pcie_start - PCI_BRIDGE_CONF_END >= PCI_CAP_SSID_SIZEOF)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		else
			bridge->ssid_start = bridge->pcie_start + PCI_CAP_PCIE_SIZEOF;
	} else if (!bridge->pcie_start && bridge->has_pcie) {
		if (bridge->ssid_start - PCI_BRIDGE_CONF_END >= PCI_CAP_PCIE_SIZEOF)
			bridge->pcie_start = PCI_BRIDGE_CONF_END;
		else
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	}

	bridge->conf.capabilities_pointer = min(bridge->ssid_start, bridge->pcie_start);

	if (bridge->conf.capabilities_pointer)
		bridge->conf.status |= cpu_to_le16(PCI_STATUS_CAP_LIST);

	if (bridge->has_pcie) {
		bridge->pcie_conf.cap_id = PCI_CAP_ID_EXP;
		bridge->pcie_conf.next = (bridge->ssid_start > bridge->pcie_start) ?
					 bridge->ssid_start : 0;
		bridge->pcie_conf.cap |= cpu_to_le16(PCI_EXP_TYPE_ROOT_PORT << 4);
		bridge->pcie_cap_regs_behavior =
			kmemdup(pcie_cap_regs_behavior,
				sizeof(pcie_cap_regs_behavior),
				GFP_KERNEL);
		if (!bridge->pcie_cap_regs_behavior) {
			kfree(bridge->pci_regs_behavior);
			return -ENOMEM;
		}
		/* These bits are applicable only for PCI and reserved on PCIe */
		/* [한국어] Latency Timer(비트 15:8). PCIe 에는 버스 중재라는
		 * 개념이 없어 이 필드가 의미를 잃었다. 읽기 전용 마스크에서
		 * 빼면 그 비트는 항상 0 으로 읽힌다. */
		bridge->pci_regs_behavior[PCI_CACHE_LINE_SIZE / 4].ro &=
			~GENMASK(15, 8);
		/* [한국어] 이 dword 는 하위 16비트가 Command, 상위 16비트가 Status 다.
		 * 그래서 Status 쪽 비트에는 << 16 을 붙여 자리를 옮긴다.
		 * 지우는 것들:
		 *   Command 의 Special Cycles / Memory Write and Invalidate /
		 *   VGA Palette Snoop / Wait Cycle Control / Fast Back-to-Back —
		 *   전부 공유 버스 시절의 기능이라 점대점 링크인 PCIe 에는 없다.
		 *   Status 의 66MHz Capable / Fast Back-to-Back Capable /
		 *   DEVSEL Timing — 마찬가지로 PCI 버스 타이밍 개념이다. */
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro &=
			~((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			   PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			   PCI_COMMAND_FAST_BACK) |
			  (PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		/* [한국어] 이 dword 의 최상위 바이트(31:24)는 Secondary Latency Timer 다.
		 * 위와 같은 이유로 PCIe 에서는 예약이다. */
		bridge->pci_regs_behavior[PCI_PRIMARY_BUS / 4].ro &=
			~GENMASK(31, 24);
		/* [한국어] 이 dword 의 상위 16비트는 Secondary Status 다.
		 * Command/Status 쌍과 같은 이유로 버스 타이밍 비트들을 지운다. */
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro &=
			~((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		/* [한국어] 이 dword 의 상위 16비트는 Bridge Control 이다.
		 * Master Abort Mode 와 비트 8/9/11 은 PCIe 에서 예약이라
		 * 쓰기 가능 마스크에서 뺀다 — 쓰려고 해도 무시된다. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].rw &=
			~((PCI_BRIDGE_CTL_MASTER_ABORT |
			   BIT(8) | BIT(9) | BIT(11)) << 16);
		/* [한국어] Fast Back-to-Back Enable 도 마찬가지로 예약. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].ro &=
			~((PCI_BRIDGE_CTL_FAST_BACK) << 16);
		/* [한국어] 비트 10(Discard Timer Status)은 원래 RW1C 였으나
		 * PCIe 에서는 예약이라 그 동작도 없앤다. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].w1c &=
			~(BIT(10) << 16);
	}

	if (flags & PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD) {
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].ro = ~0;
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].rw = 0;
	}

	if (flags & PCI_BRIDGE_EMUL_NO_IO_FORWARD) {
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro |= PCI_COMMAND_IO;
		bridge->pci_regs_behavior[PCI_COMMAND / 4].rw &= ~PCI_COMMAND_IO;
		/* [한국어] I/O Base/Limit(하위 16비트)를 읽기 전용으로 고정한다.
		 * ro 에 넣고 rw 에서 빼는 두 동작이 한 쌍이다 — 읽으면 0 이 나오고
		 * 쓰기는 무시된다. 그러면 소프트웨어가 "이 브리지는 I/O 창이
		 * 없다" 고 판단한다(base > limit 이 되므로). */
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro |= GENMASK(15, 0);
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].rw &= ~GENMASK(15, 0);
		/* [한국어] 32비트 I/O 주소용 상위 16비트 레지스터도 통째로
		 * 읽기 전용 0 으로 만든다. 여기는 dword 전체가 그 용도라
		 * &= 가 아니라 = 로 덮어쓴다. */
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].ro = ~0;
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].rw = 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_init);

/*
 * Cleanup a pci_bridge_emul structure that was previously initialized
 * using pci_bridge_emul_init().
 */
/*
 * NVMe: pci_bridge_emul_init()에서 할당한 동적 메모리를 해제한다.
 * NVMe 장치가 제거되거나 컨트롤러 드라이버가 unload될 때 호출된다.
 */
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge)
{
	if (bridge->has_pcie)
		kfree(bridge->pcie_cap_regs_behavior);
	kfree(bridge->pci_regs_behavior);
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_cleanup);

/*
 * Should be called by the PCI controller driver when reading the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->read_base or ->ops->read_pcie operations.
 */
/*
 * NVMe: PCI 컨트롤러 드라이버가 가짜 브리지의 설정 공간을 읽을 때
 * 호출된다. where는 읽을 바이트 오프셋, size는 1/2/4바이트, value는
 * 결과 저장 포인터이다. NVMe 엔드포인트가 상위 Root Port 상태를 읽거나
 * lspci 등이 브리지 설정 공간을 조회할 때 이 함수가 사용된다.
 */
int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value)
{
	int ret;
	int reg = where & ~3;
	pci_bridge_emul_read_status_t (*read_op)(struct pci_bridge_emul *bridge,
						 int reg, u32 *value);
	__le32 *cfgspace;
	const struct pci_bridge_reg_behavior *behavior;

	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		read_op = bridge->ops->read_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	} else if (reg >= bridge->ssid_start && reg < bridge->ssid_start + PCI_CAP_SSID_SIZEOF &&
		   bridge->subsystem_vendor_id) {
		/* Emulated PCI Bridge Subsystem Vendor ID capability */
		reg -= bridge->ssid_start;
		read_op = pci_bridge_emul_read_ssid;
		cfgspace = NULL;
		behavior = NULL;
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		reg -= bridge->pcie_start;
		read_op = bridge->ops->read_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		reg -= PCI_CFG_SPACE_SIZE;
		read_op = bridge->ops->read_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	if (read_op)
		ret = read_op(bridge, reg, value);
	else
		ret = PCI_BRIDGE_EMUL_NOT_HANDLED;

	if (ret == PCI_BRIDGE_EMUL_NOT_HANDLED) {
		if (cfgspace)
			*value = le32_to_cpu(cfgspace[reg / 4]);
		else
			*value = 0;
	}

	/*
	 * Make sure we never return any reserved bit with a value
	 * different from 0.
	 */
	if (behavior)
		*value &= behavior[reg / 4].ro | behavior[reg / 4].rw |
			  behavior[reg / 4].w1c;

	if (size == 1)
		*value = (*value >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*value = (*value >> (8 * (where & 3))) & 0xffff;
	else if (size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_read);

/*
 * Should be called by the PCI controller driver when writing the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->write_base or ->ops->write_pcie operations.
 */
/*
 * NVMe: PCI 컨트롤러 드라이버가 가짜 브리지의 설정 공간에 쓸 때
 * 호출된다. PCI core가 NVMe 장치를 위해 버스 번호, 메모리 창, 인터럽트,
 * PCIe 제어/상태 등을 구성할 때 이 함수를 통해 브리지 설정 공간에
 * 기록된다.
 */
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value)
{
	int reg = where & ~3;
	int mask, ret, old, new, shift;
	void (*write_op)(struct pci_bridge_emul *bridge, int reg,
			 u32 old, u32 new, u32 mask);
	__le32 *cfgspace;
	const struct pci_bridge_reg_behavior *behavior;

	ret = pci_bridge_emul_conf_read(bridge, reg, 4, &old);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		write_op = bridge->ops->write_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		reg -= bridge->pcie_start;
		write_op = bridge->ops->write_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		reg -= PCI_CFG_SPACE_SIZE;
		write_op = bridge->ops->write_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		return PCIBIOS_SUCCESSFUL;
	}

	shift = (where & 0x3) * 8;

	/* [한국어] 요청한 폭에 해당하는 비트만 1인 마스크를 만든다.
	 * shift 는 dword 안에서의 바이트 오프셋 x 8 이다.
	 * 예: size=2, where=0x02 이면 shift=16 이라 마스크가 0xffff0000 —
	 * 즉 그 dword 의 상위 워드만 대상이 된다.
	 * 이 마스크로 dword 단위 저장소에서 요청한 부분만 뽑아내거나
	 * 갈아끼운다. */
	if (size == 4)
		mask = 0xffffffff;	/* [한국어] dword 전체 */
	else if (size == 2)
		mask = 0xffff << shift;	/* [한국어] 워드 하나를 그 자리로 */
	else if (size == 1)
		mask = 0xff << shift;	/* [한국어] 바이트 하나를 그 자리로 */
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (behavior) {
		/* Keep all bits, except the RW bits */
		new = old & (~mask | ~behavior[reg / 4].rw);

		/* Update the value of the RW bits */
		new |= (value << shift) & (behavior[reg / 4].rw & mask);

		/* Clear the W1C bits */
		new &= ~((value << shift) & (behavior[reg / 4].w1c & mask));
	} else {
		new = old & ~mask;
		new |= (value << shift) & mask;
	}

	if (cfgspace) {
		/* Save the new value with the cleared W1C bits into the cfgspace */
		cfgspace[reg / 4] = cpu_to_le32(new);
	}

	if (behavior) {
		/*
		 * Clear the W1C bits not specified by the write mask, so that the
		 * write_op() does not clear them.
		 */
		new &= ~(behavior[reg / 4].w1c & ~mask);

		/*
		 * Set the W1C bits specified by the write mask, so that write_op()
		 * knows about that they are to be cleared.
		 */
		new |= (value << shift) & (behavior[reg / 4].w1c & mask);
	}

	if (write_op)
		write_op(bridge, reg, old, new, mask);

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_write);
