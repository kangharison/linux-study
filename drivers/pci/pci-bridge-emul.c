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

#include <linux/pci.h>		/* NVMe: PCI core 헤더: pci.h는 PCI 설정 공간 오프셋, capability ID,
				 * MSI/MSI-X, AER, VC, NPEM 등 NVMe 호스트가 의존하는
				 * 모든 PCI/PCIe 상수와 구조체를 정의한다. */
#include "pci-bridge-emul.h"	/* NVMe: bridge emulation 전용 헤더:
				 * struct pci_bridge_emul, ops, conf 공간 정의 포함 */

#define PCI_BRIDGE_CONF_END	PCI_STD_HEADER_SIZEOF
	/* NVMe: 가짜 브리지의 일반 PCI 헤더 설정 공간 끝 오프셋(64바이트)을
	 * PCI 표준 헤더 크기로 고정한다. */
#define PCI_CAP_SSID_SIZEOF	(PCI_SSVID_DEVICE_ID + 2)
	/* NVMe: Subsystem Vendor ID capability의 총 크기를 SSVID 레지스터
	 * 끝까지로 계산한다. NVMe 컨트롤러의 서브시스템 식별에 쓰일 수 있다. */
#define PCI_CAP_PCIE_SIZEOF	(PCI_EXP_SLTSTA2 + 2)
	/* NVMe: PCIe capability 블록 전체 크기를 Slot Status 2 끝까지로
	 * 정의한다. Link Cap/Status, Root Cap/Control/Status, AER 관련
	 * 비트 등 NVMe 장치가 상위 링크 상태를 파악하는 데 필요하다. */

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
/* NVMe: 각 4바이트 레지스터 별로 어떤 비트가 읽기전용(RO), 읽기쓰기(RW),
 * 쓰기-1-클리어(W1C)인지를 기술한다. NVMe 엔드포인트 또는 PCI core가
 * 브리지 설정 공간을 읽을 때 예약된 비트가 0으로 마스크되도록 보장하여
 * 잘못된 capability/상태 해석을 방지한다. */
struct pci_bridge_reg_behavior {
	/* Read-only bits */
	u32 ro;			/* NVMe: 읽기 전용 비트 마스크 */

	/* Read-write bits */
	u32 rw;			/* NVMe: 읽기/쓰기 가능 비트 마스크 */

	/* Write-1-to-clear bits */
	u32 w1c;		/* NVMe: 1을 쓰면 클리어되는 상태 비트 마스크,
				 * 예: 링크 상태, 슬롯 상태, AER 관련 상태 비트 */
};

static const
struct pci_bridge_reg_behavior pci_regs_behavior[PCI_STD_HEADER_SIZEOF / 4] = {
	/* NVMe: 표준 PCI 헤더의 각 4바이트 레지스터별 접근 특성을 정의하는
	 * 상수 배열이다. 인덱스는 레지스터 오프셋을 4로 나눈 값이다. */

	[PCI_VENDOR_ID / 4] = { .ro = ~0 },
	/* NVMe: Vendor/Device ID 워드는 전체 32비트가 읽기 전용이다.
	 * NVMe 엔드포인트가 상위 브리지의 ID를 읽거나 PCI core가 열거할
	 * 때 변경 불가능해야 한다. */

	[PCI_COMMAND / 4] = {
		/* NVMe: Command 레지스터의 RW 비트를 설정한다. IO/MEM 공간
		 * 디코드, 버스 마스터, 패리티/SERR 등 NVMe 트래픽이 브리지를
		 * 통과하기 위해 필수적인 비트들이다. */
		.rw = (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		       PCI_COMMAND_MASTER | PCI_COMMAND_PARITY |
		       PCI_COMMAND_SERR),

		/* NVMe: Command/Status 레지스터에서 읽기 전용인 비트들을
		 * 마스크한다. 66MHz, fast back-to-back, DEVSEL 타이밍 등은
		 * 브리지가 지원하지 않거나 변경할 수 없다. */
		.ro = ((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			PCI_COMMAND_FAST_BACK) |
		       (PCI_STATUS_CAP_LIST | PCI_STATUS_66MHZ |
			PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MASK) << 16),

		/* NVMe: Status 레지스터의 에러 비트는 Write-1-to-Clear
		 * 방식으로, NVMe 장치나 상위 버스에서 발생한 에러가 보고된
		 * 후 소프트웨어가 1을 써서 클리어한다. */
		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_CLASS_REVISION / 4] = { .ro = ~0 },
	/* NVMe: Class Code(브리지 클래스)와 Revision은 전체 RO이다. */

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
	/* NVMe: Cache Line Size/Latency Timer/Header Type/BIST를 4바이트
	 * 단위로 모두 RO로 설정한다. NVMe 성능에 영향을 주는 Cache Line
	 * Size도 에뮬레이션에서는 하드웨어 고정값으로 처리한다. */

	/*
	 * Base Address registers not used must be implemented as
	 * read-only registers that return 0 when read.
	 */
	[PCI_BASE_ADDRESS_0 / 4] = { .ro = ~0 },
	/* NVMe: 브리지는 자체 BAR를 사용하지 않으므로 BAR0은 RO 0이다. */
	[PCI_BASE_ADDRESS_1 / 4] = { .ro = ~0 },
	/* NVMe: BAR1 역시 사용하지 않으므로 RO 0이다. */

	[PCI_PRIMARY_BUS / 4] = {
		/* Primary, secondary and subordinate bus are RW */
		/* NVMe: 버스 번호(primary/secondary/subordinate)는 PCI core가
		 * 열거하면서 NVMe 장치가 연결될 하위 버스 번호를 할당하므로
		 * 반드시 RW여야 한다. */
		.rw = GENMASK(24, 0),

		/* Secondary latency is read-only */
		/* NVMe: secondary latency timer 필드(31:24)는 RO로 고정한다. */
		.ro = GENMASK(31, 24),
	},

	[PCI_IO_BASE / 4] = {
		/* The high four bits of I/O base/limit are RW */
		/* NVMe: I/O base/limit의 상위 4비트만 RW로 하여 하위 버스의
		 * NVMe 엔드포인트가 사용할 I/O 공간을 구성할 수 있게 한다. */
		.rw = (GENMASK(15, 12) | GENMASK(7, 4)),

		/* The low four bits of I/O base/limit are RO */
		/* NVMe: I/O base/limit 하위 4비트는 16바이트 정렬을 위해
		 * 하드와이어드 0이며, secondary status의 일부 비트는 RO/W1C
		 * 로 처리한다. */
		.ro = (((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			 PCI_STATUS_DEVSEL_MASK) << 16) |
		       GENMASK(11, 8) | GENMASK(3, 0)),

		/* NVMe: secondary status의 에러 비트는 W1C로 클리어한다. */
		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_MEMORY_BASE / 4] = {
		/* The high 12-bits of mem base/limit are RW */
		/* NVMe: non-prefetchable 메모리 창의 base/limit 상위 12비트를
		 * RW로 설정하여 NVMe BAR 매핑에 사용할 메모리 범위를
		 * 구성한다. */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of mem base/limit are RO */
		/* NVMe: 메모리 범위 하위 4비트는 1MB 정렬을 위해 RO 0이다. */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_MEMORY_BASE / 4] = {
		/* The high 12-bits of pref mem base/limit are RW */
		/* NVMe: NVMe 장치의 64비트 prefetchable BAR를 매핑하기 위한
		 * 상위 범위 비트를 RW로 설정한다. */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of pref mem base/limit are RO */
		/* NVMe: prefetchable 메모리 범위 하위 4비트는 RO 0이다. */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_BASE_UPPER32 / 4] = {
		/* NVMe: 64비트 prefetchable 메모리 범위의 상위 32비트 base를
		 * RW로 설정한다. 4GB 이상의 NVMe BAR 매핑에 필수적이다. */
		.rw = ~0,
	},

	[PCI_PREF_LIMIT_UPPER32 / 4] = {
		/* NVMe: 64비트 prefetchable 메모리 범위의 상위 32비트 limit를
		 * RW로 설정한다. */
		.rw = ~0,
	},

	[PCI_IO_BASE_UPPER16 / 4] = {
		/* NVMe: 32비트 I/O 공간의 상위 16비트 base/limit을 RW로
		 * 설정한다. NVMe는 주로 MMIO를 사용하지만 호환성을 위해
		 * 제공한다. */
		.rw = ~0,
	},

	[PCI_CAPABILITY_LIST / 4] = {
		/* NVMe: capability list 포인터(7:0)만 RO로 노출한다. PCI core가
		 * PCIe capability, SSID capability 등을 순회할 수 있게 한다. */
		.ro = GENMASK(7, 0),
	},

	/*
	 * If expansion ROM is unsupported then ROM Base Address register must
	 * be implemented as read-only register that return 0 when read, same
	 * as for unused Base Address registers.
	 */
	[PCI_ROM_ADDRESS1 / 4] = {
		/* NVMe: Expansion ROM은 지원하지 않으므로 RO 0으로 처리한다. */
		.ro = ~0,
	},

	/*
	 * Interrupt line (bits 7:0) are RW, interrupt pin (bits 15:8)
	 * are RO, and bridge control (31:16) are a mix of RW, RO,
	 * reserved and W1C bits
	 */
	[PCI_INTERRUPT_LINE / 4] = {
		/* Interrupt line is RW */
		/* NVMe: 인터럽트 라인(7:0)과 bridge control의 일부 RW 비트를
		 * 설정한다. 브리지가 하위 버스의 NVMe 인터럽트를 상위로
		 * 전파하는 동작을 제어한다. */
		.rw = (GENMASK(7, 0) |
		       ((PCI_BRIDGE_CTL_PARITY |
			 PCI_BRIDGE_CTL_SERR |
			 PCI_BRIDGE_CTL_ISA |
			 PCI_BRIDGE_CTL_VGA |
			 PCI_BRIDGE_CTL_MASTER_ABORT |
			 PCI_BRIDGE_CTL_BUS_RESET |
			 BIT(8) | BIT(9) | BIT(11)) << 16)),

		/* Interrupt pin is RO */
		/* NVMe: 인터럽트 핀(15:8)과 bridge control의 일부 RO 비트를
		 * 마스크한다. */
		.ro = (GENMASK(15, 8) | ((PCI_BRIDGE_CTL_FAST_BACK) << 16)),

		/* NVMe: bridge control의 W1C 비트(10번)를 설정한다. */
		.w1c = BIT(10) << 16,
	},
};

static const
struct pci_bridge_reg_behavior pcie_cap_regs_behavior[PCI_CAP_PCIE_SIZEOF / 4] = {
	/* NVMe: PCIe capability 블록의 각 4바이트 레지스터별 접근 특성을
	 * 정의한다. NVMe 장치가 상위 Root Port의 링크 상태, 루트 상태,
	 * 슬롯 상태, AER/VC 등 확장 capability 탐색에 필요한 기초 정보를
	 * 제공한다. */

	[PCI_CAP_LIST_ID / 4] = {
		/*
		 * Capability ID, Next Capability Pointer and
		 * bits [14:0] of Capabilities register are all read-only.
		 * Bit 15 of Capabilities register is reserved.
		 */
		/* NVMe: PCIe capability의 ID, next pointer, cap 레지스터
		 * 하위 15비트는 모두 RO이다. NVMe가 capability chain을 따라
		 * 열거할 때 올바른 ID(0x10)와 다음 포인터를 제공해야 한다. */
		.ro = GENMASK(30, 0),
	},

	[PCI_EXP_DEVCAP / 4] = {
		/*
		 * Bits [31:29] and [17:16] are reserved.
		 * Bits [27:18] are reserved for non-upstream ports.
		 * Bits 28 and [14:6] are reserved for non-endpoint devices.
		 * Other bits are read-only.
		 */
		/* NVMe: Root Port의 device capabilities는 주로 RO이며,
		 * payload size, extended tag field 등 NVMe DMA 성능과 관련된
		 * 능력을 표현한다. */
		.ro = BIT(15) | GENMASK(5, 0),
	},

	[PCI_EXP_DEVCTL / 4] = {
		/*
		 * Device control register is RW, except bit 15 which is
		 * reserved for non-endpoints or non-PCIe-to-PCI/X bridges.
		 */
		/* NVMe: Root Port의 device control 레지스터는 14:0 비트가 RW.
		 * Max payload size, relaxed ordering, extended tag enable,
		 * unsupported request reporting 등 NVMe 트래픽 특성을
		 * 제어한다. */
		.rw = GENMASK(14, 0),

		/*
		 * Device status register has bits 6 and [3:0] W1C, [5:4] RO,
		 * the rest is reserved. Also bit 6 is reserved for non-upstream
		 * ports.
		 */
		/* NVMe: device status의 에러 비트들을 W1C/RO로 처리한다.
		 * Correctable/Non-fatal/Fatal/Unsupported request 상태는
		 * AER 처리와 연결된다. */
		.w1c = GENMASK(3, 0) << 16,
		.ro = GENMASK(5, 4) << 16,
	},

	[PCI_EXP_LNKCAP / 4] = {
		/*
		 * All bits are RO, except bit 23 which is reserved and
		 * bit 18 which is reserved for non-upstream ports.
		 */
		/* NVMe: Root Port의 링크 능력(최대 속도/폭, ASPM, L0s/L1
		 * 지원, 포트 번호 등)을 RO로 노출한다. NVMe SSD 링크
		 * 협상(예: Gen4 x4)의 상한을 결정하는 근거가 된다. */
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
		/* NVMe: 링크 제어(ASPM, Common Clock, Extended Sync, Clock
		 * Power Management 등)는 RW로, NVMe 장치 전력/성능 조정에
		 * 사용된다. */
		.rw = GENMASK(15, 14) | GENMASK(11, 9) | GENMASK(7, 3) | GENMASK(1, 0),
		/* NVMe: 링크 상태(현재 속도/폭, 링크 트레이닝, 스킵 등)는
		 * RO로 제공되며, NVMe 드라이버나 사용자가
		 * `lspci -vv` 등으로 확인한다. */
		.ro = GENMASK(13, 0) << 16,
		/* NVMe: 링크 상태의 W1C 비트(15:14)는 에러/변경 보고 후
		 * 소프트웨어가 클리어한다. */
		.w1c = GENMASK(15, 14) << 16,
	},

	[PCI_EXP_SLTCAP / 4] = {
		/* NVMe: Slot capabilities(전원 제한, 핫플러그 등)는 전체
		 * RO로 처리한다. */
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
		/* NVMe: 슬롯 제어(전원/Attention Button/프레즌스 감지 등)는
		 * RW로, NVMe 핫플러그 시스템에서 슬롯 상태를 제어할 때
		 * 사용된다. */
		.rw = GENMASK(14, 0),
		/* NVMe: 슬롯 상태의 Attention Button/Present Detect/MRL
		 * Sensor/Command Complete/Data Link Layer State Changed 등의
		 * 비트를 W1C로 클리어한다. */
		.w1c = (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
			PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_PDC |
			PCI_EXP_SLTSTA_CC | PCI_EXP_SLTSTA_DLLSC) << 16,
		/* NVMe: 슬롯 상태의 MRL Sensor/Presence Detect/Electromechanical
		 * Interlock Status 등은 RO이다. */
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
		/* NVMe: Root Control는 Correctable/Non-fatal/Fatal/PME
		 * interrupt enable, CRS software visibility enable 등을
		 * 제어한다. AER 관련 인터럽트 마스크의 일부이다. */
		.rw = (PCI_EXP_RTCTL_SECEE | PCI_EXP_RTCTL_SENFEE |
		       PCI_EXP_RTCTL_SEFEE | PCI_EXP_RTCTL_PMEIE |
		       PCI_EXP_RTCTL_RRS_SVE),
		/* NVMe: Root Capabilities의 CRS Software Visibility bit 0은
		 * RO로 노출된다. */
		.ro = PCI_EXP_RTCAP_RRS_SV << 16,
	},

	[PCI_EXP_RTSTA / 4] = {
		/*
		 * Root status has bits 17 and [15:0] RO, bit 16 W1C, the rest
		 * is reserved.
		 */
		/* NVMe: Root Status의 PME Requester ID, PME Status, PME
		 * Pending 등을 노출한다. NVMe 장치의 PCIe 전원 관리 이벤트를
		 * Root Port가 수신하면 이 레지스터를 통해 보고된다. */
		.ro = GENMASK(15, 0) | PCI_EXP_RTSTA_PENDING,
		/* NVMe: PME Status 비트는 W1C로 클리어한다. */
		.w1c = PCI_EXP_RTSTA_PME,
	},

	[PCI_EXP_DEVCAP2 / 4] = {
		/*
		 * Device capabilities 2 register has reserved bits [30:27].
		 * Also bits [26:24] are reserved for non-upstream ports.
		 */
		/* NVMe: Device Capabilities 2(Completion Timeout ranges,
		 * ARI forwarding, AtomicOp routing, 10-bit Tag requester,
		 * OBFF 등)의 유효 비트를 RO로 노출한다. */
		.ro = BIT(31) | GENMASK(23, 0),
	},

	[PCI_EXP_DEVCTL2 / 4] = {
		/*
		 * Device control 2 register is RW. Bit 11 is reserved for
		 * non-upstream ports.
		 *
		 * Device status 2 register is reserved.
		 */
		/* NVMe: Device Control 2(Completion Timeout value, ARI
		 * forwarding enable, AtomicOp requester/complete enable,
		 * IDO enable 등)는 RW로, NVMe의 고급 기능 활성화에 사용된다. */
		.rw = GENMASK(15, 12) | GENMASK(10, 0),
	},

	[PCI_EXP_LNKCAP2 / 4] = {
		/* Link capabilities 2 register has reserved bits [30:25] and 0. */
		/* NVMe: Link Capabilities 2(지원하는 링크 속도 벡터, Crosslink,
		 * Retimer presence detect 등)의 유효 비트를 RO로 노출하여
		 * NVMe 링크 협상 상한을 제공한다. */
		.ro = BIT(31) | GENMASK(24, 1),
	},

	[PCI_EXP_LNKCTL2 / 4] = {
		/*
		 * Link control 2 register is RW.
		 *
		 * Link status 2 register has bits 5, 15 W1C;
		 * bits 10, 11 reserved and others are RO.
		 */
		/* NVMe: Link Control 2(목표 링크 속도, Compliance/De-emphasis
		 * 등)는 RW로, NVMe 장치와의 링크 재학습/속도 변경 시
		 * 사용된다. */
		.rw = GENMASK(15, 0),
		/* NVMe: Link Status 2의 Current De-emphasis Level 등은
		 * W1C로 클리어한다. */
		.w1c = (BIT(15) | BIT(5)) << 16,
		/* NVMe: Link Status 2의 나머지 상태 비트는 RO이다. */
		.ro = (GENMASK(14, 12) | GENMASK(9, 6) | GENMASK(4, 0)) << 16,
	},

	[PCI_EXP_SLTCAP2 / 4] = {
		/* Slot capabilities 2 register is reserved. */
		/* NVMe: Slot Capabilities 2는 현재 예약되어 있어 모든 비트
		 * 처리가 기본값(0)으로 남는다. */
	},

	[PCI_EXP_SLTCTL2 / 4] = {
		/* Both Slot control 2 and Slot status 2 registers are reserved. */
		/* NVMe: Slot Control/Status 2는 예약되어 있어 별도 RW/RO/W1C
		 * 비트가 없다. */
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
	/* NVMe: 읽으려는 레지스터 오프셋에 따라 SSID capability의 ID/Next
	 * 포인터 또는 Subsystem Vendor/Device ID를 반환한다. */
	switch (reg) {
	case PCI_CAP_LIST_ID:
		/* NVMe: capability ID는 0x0D(SSID), next 포인터는 PCIe
		 * capability가 뒤에 있으면 그 오프셋, 없으면 0을 반환한다. */
		*value = PCI_CAP_ID_SSVID |
			((bridge->pcie_start > bridge->ssid_start) ? (bridge->pcie_start << 8) : 0);
		return PCI_BRIDGE_EMUL_HANDLED;

	case PCI_SSVID_VENDOR_ID:
		/* NVMe: Subsystem Vendor ID(하위 16비트)와 Subsystem ID(상위
		 * 16비트)를 조합하여 반환한다. */
		*value = bridge->subsystem_vendor_id |
			(bridge->subsystem_id << 16);
		return PCI_BRIDGE_EMUL_HANDLED;

	default:
		/* NVMe: 처리할 수 없는 오프셋이면 일반 emulation 경로로
		 * 넘긴다. */
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
	/* NVMe: conf 배열 크기가 표준 PCI 헤더 크기와 정확히 일치하는지
	 * 컴파일 타임에 검사한다. 크기 불일치 시 빌드 오류를 발생시켜
	 * NVMe의 설정 공간 접근이 깨지는 것을 방지한다. */
	BUILD_BUG_ON(sizeof(bridge->conf) != PCI_BRIDGE_CONF_END);

	/*
	 * class_revision: Class is high 24 bits and revision is low 8 bit
	 * of this member, while class for PCI Bridge Normal Decode has the
	 * 24-bit value: PCI_CLASS_BRIDGE_PCI_NORMAL
	 */
	/* NVMe: Class Code 필드 상위 24비트에 PCI-to-PCI Bridge Normal
	 * Decode 클래스를 기록한다. PCI core가 이 장치를 브리지로 인식하여
	 * 하위 버스를 열거할 수 있게 한다. */
	bridge->conf.class_revision |=
		cpu_to_le32(PCI_CLASS_BRIDGE_PCI_NORMAL << 8);
	/* NVMe: Header Type을 PCI 브리지 유형(1)로 고정한다. */
	bridge->conf.header_type = PCI_HEADER_TYPE_BRIDGE;
	/* NVMe: Cache Line Size를 0x10(64바이트)로 초기화한다. */
	bridge->conf.cache_line_size = 0x10;
	/* NVMe: Status 레지스터에 Capability List 존재 비트를 설정한다.
	 * 이 비트가 있어야 PCI core가 PCIe capability, SSID capability 등을
	 * 탐색한다. */
	bridge->conf.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
	/* NVMe: 표준 PCI 헤더 레지스터별 RW/RO/W1C 특성을 동적 배열로
	 * 복제하여 runtime에 수정할 수 있게 한다. */
	bridge->pci_regs_behavior = kmemdup(pci_regs_behavior,
					    sizeof(pci_regs_behavior),
					    GFP_KERNEL);
	/* NVMe: 메모리 할당 실패 시 NVMe 장치 초기화를 중단하고 -ENOMEM을
	 * 반환한다. */
	if (!bridge->pci_regs_behavior)
		return -ENOMEM;

	/* If ssid_start and pcie_start were not specified then choose the lowest possible value. */
	/* NVMe: SSID capability와 PCIe capability의 시작 오프셋이 지정되지
	 * 않은 경우, 둘 다 표준 헤더 직후에 가능한 한 낮은 위치에 배치한다. */
	if (!bridge->ssid_start && !bridge->pcie_start) {
		/* NVMe: 서브시스템 식별 정보가 있으면 표준 헤더 바로 뒤에
		 * SSID capability를 배치한다. */
		if (bridge->subsystem_vendor_id)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		/* NVMe: PCIe capability가 필요하면 SSID 뒤에 배치한다. */
		if (bridge->has_pcie)
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	} else if (!bridge->ssid_start && bridge->subsystem_vendor_id) {
		/* NVMe: pcie_start만 지정된 상태에서 SSID를 추가해야 할 때,
		 * PCIe capability 앞에 공간이 충분하면 그곳에, 아니면 PCIe
		 * 뒤에 배치한다. */
		if (bridge->pcie_start - PCI_BRIDGE_CONF_END >= PCI_CAP_SSID_SIZEOF)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		else
			bridge->ssid_start = bridge->pcie_start + PCI_CAP_PCIE_SIZEOF;
	} else if (!bridge->pcie_start && bridge->has_pcie) {
		/* NVMe: ssid_start만 지정된 상태에서 PCIe capability를 추가할
		 * 때, SSID 앞에 공간이 충분하면 그곳에, 아니면 SSID 뒤에
		 * 배치한다. */
		if (bridge->ssid_start - PCI_BRIDGE_CONF_END >= PCI_CAP_PCIE_SIZEOF)
			bridge->pcie_start = PCI_BRIDGE_CONF_END;
		else
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	}

	/* NVMe: capability list의 첫 번째 포인터를 SSID와 PCIe capability
	 * 중 더 작은 오프셋으로 설정한다. */
	bridge->conf.capabilities_pointer = min(bridge->ssid_start, bridge->pcie_start);

	/* NVMe: capability가 하나라도 있으면 Status 레지스터에 Capability
	 * List 비트를 다시 설정한다. */
	if (bridge->conf.capabilities_pointer)
		bridge->conf.status |= cpu_to_le16(PCI_STATUS_CAP_LIST);

	/* NVMe: PCIe capability가 필요한 경우(대부분의 NVMe Root Port) */
	if (bridge->has_pcie) {
		/* NVMe: PCIe capability ID를 0x10(PCI Express)으로 설정한다. */
		bridge->pcie_conf.cap_id = PCI_CAP_ID_EXP;
		/* NVMe: capability chain의 next 포인터를 SSID capability
		 * 뒤에 있으면 그 오프셋으로, 없으면 0으로 설정한다. */
		bridge->pcie_conf.next = (bridge->ssid_start > bridge->pcie_start) ?
					 bridge->ssid_start : 0;
		/* NVMe: PCIe capability register의 Device/Port Type 필드를
		 * Root Port(0x4)로 설정한다. NVMe 장치의 상위가 Root Port임을
		 * 표시한다. */
		bridge->pcie_conf.cap |= cpu_to_le16(PCI_EXP_TYPE_ROOT_PORT << 4);
		/* NVMe: PCIe capability 레지스터별 RW/RO/W1C 특성을 동적
		 * 배열로 복제한다. */
		bridge->pcie_cap_regs_behavior =
			kmemdup(pcie_cap_regs_behavior,
				sizeof(pcie_cap_regs_behavior),
				GFP_KERNEL);
		/* NVMe: PCIe capability 메모리 할당 실패 시 이전에 할당한
		 * pci_regs_behavior를 해제하고 -ENOMEM을 반환한다. */
		if (!bridge->pcie_cap_regs_behavior) {
			kfree(bridge->pci_regs_behavior);
			return -ENOMEM;
		}
		/* These bits are applicable only for PCI and reserved on PCIe */
		/* NVMe: PCIe 모드에서는 PCI 전용으로 사용되던 비트들을 예약
		 * 처리하기 위해 마스크에서 제거한다. */
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

	/* NVMe: prefetchable 메모리 포워딩을 지원하지 않는 플래그가 설정된
	 * 경우, prefetchable 메모리 베이스/리미트 레지스터를 RO 0으로
	 * 고정한다. NVMe 장치의 prefetchable BAR가 없을 때 사용된다. */
	if (flags & PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD) {
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].ro = ~0;
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].rw = 0;
	}

	/* NVMe: I/O 포워딩을 지원하지 않는 플래그가 설정된 경우, I/O
	 * base/limit과 관련 command 비트를 RO 0으로 고정한다. NVMe는
	 * MMIO만 사용하는 경우가 많아 실무에서 자주 설정된다. */
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

	/* NVMe: 가짜 브리지 초기화 성공. 이후 PCI core가 이 브리지를 통해
	 * NVMe 엔드포인트를 열거할 수 있다. */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_init);
/* NVMe: GPL EXPORT하여 PCI 컨트롤러 모듈과 NVMe 관련 PCI core가 이
 * 초기화 함수를 사용할 수 있게 한다. */

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
	/* NVMe: PCIe capability 동작 배열이 할당되어 있으면 먼저 해제한다. */
	if (bridge->has_pcie)
		kfree(bridge->pcie_cap_regs_behavior);
	/* NVMe: 표준 PCI 헤더 동작 배열을 해제한다. */
	kfree(bridge->pci_regs_behavior);
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_cleanup);
/* NVMe: cleanup 함수도 GPL EXPORT한다. */

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
	/* NVMe: 함수 반환값과 4바이트 정렬된 레지스터 오프셋을 선언한다. */
	int ret;
	int reg = where & ~3;
	/* NVMe: 컨트롤러가 제공한 read 콜백 함수 포인터를 저장한다. */
	pci_bridge_emul_read_status_t (*read_op)(struct pci_bridge_emul *bridge,
						 int reg, u32 *value);
	/* NVMe: 읽을 설정 공간 버퍼 포인터이다. */
	__le32 *cfgspace;
	/* NVMe: 해당 영역의 RW/RO/W1C 동작 테이블 포인터이다. */
	const struct pci_bridge_reg_behavior *behavior;

	/* NVMe: 읽으려는 주소가 표준 PCI 헤더 영역이면 */
	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		/* NVMe: 기본 설정 공간 read 콜백을 사용하고, 메모리 내
		 * conf 배열과 pci_regs_behavior를 참조한다. */
		read_op = bridge->ops->read_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	/* NVMe: 읽으려는 주소가 SSID capability 영역이면 */
	} else if (reg >= bridge->ssid_start && reg < bridge->ssid_start + PCI_CAP_SSID_SIZEOF &&
		   bridge->subsystem_vendor_id) {
		/* Emulated PCI Bridge Subsystem Vendor ID capability */
		/* NVMe: SSID capability 시작점을 0 기준으로 재조정하고
		 * pci_bridge_emul_read_ssid()를 호출한다. */
		reg -= bridge->ssid_start;
		read_op = pci_bridge_emul_read_ssid;
		cfgspace = NULL;
		behavior = NULL;
	/* NVMe: 읽으려는 주소가 PCIe capability 영역이면 */
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		/* NVMe: PCIe capability 시작점을 0 기준으로 재조정하고
		 * pcie_conf 배열과 pcie_cap_regs_behavior를 참조한다. */
		reg -= bridge->pcie_start;
		read_op = bridge->ops->read_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	/* NVMe: 읽으려는 주소가 PCIe extended capability 공간(256~4095)이면 */
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		/* NVMe: AER, VC, NPEM, ACS, ATS 등 확장 capability를 처리하는
		 * read_ext 콜백으로 전달한다. 컨트롤러가 직접 구현하지 않으면
		 * 0을 반환한다. */
		reg -= PCI_CFG_SPACE_SIZE;
		read_op = bridge->ops->read_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		/* NVMe: 처리되지 않는 영역은 0을 반환한다. PCI 표준에 따라
		 * 예약 영역은 0을 읽어야 한다. */
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	/* NVMe: 컨트롤러 콜백이 등록되어 있으면 호출한다. */
	if (read_op)
		ret = read_op(bridge, reg, value);
	else
		/* NVMe: 콜백이 없으면 일반 emulation 경로로 처리한다. */
		ret = PCI_BRIDGE_EMUL_NOT_HANDLED;

	/* NVMe: 콜백이 처리하지 않았으면 메모리 내 설정 공간에서 값을
	 * 읽어온다. */
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
	/* NVMe: RO/RW/W1C로 선언되지 않은 예약 비트를 0으로 마스크하여
	 * NVMe 장치나 사용자 도구가 잘못된 값을 보지 않도록 한다. */
	if (behavior)
		*value &= behavior[reg / 4].ro | behavior[reg / 4].rw |
			  behavior[reg / 4].w1c;

	/* NVMe: 1바이트 읽기면 해당 바이트만 추출한다. */
	if (size == 1)
		*value = (*value >> (8 * (where & 3))) & 0xff;
	/* NVMe: 2바이트 읽기면 해당 워드만 추출한다. */
	else if (size == 2)
		*value = (*value >> (8 * (where & 3))) & 0xffff;
	/* NVMe: 4바이트가 아닌 다른 크기는 잘못된 레지스터 번호로 처리한다. */
	else if (size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* NVMe: 정상적으로 읽기를 완료하면 PCIBIOS_SUCCESSFUL을 반환한다. */
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_read);
/* NVMe: read 함수를 GPL EXPORT하여 PCI 컨트롤러 드라이버가 사용할 수
 * 있게 한다. */

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
	/* NVMe: 4바이트 정렬된 레지스터 오프셋을 계산한다. */
	int reg = where & ~3;
	/* NVMe: 마스크, 반환값, 이전/새 값, 바이트 시프트량 변수를
	 * 선언한다. */
	int mask, ret, old, new, shift;
	/* NVMe: 컨트롤러가 제공한 write 콜백 함수 포인터를 저장한다. */
	void (*write_op)(struct pci_bridge_emul *bridge, int reg,
			 u32 old, u32 new, u32 mask);
	/* NVMe: 쓸 설정 공간 버퍼 포인터이다. */
	__le32 *cfgspace;
	/* NVMe: 해당 영역의 RW/RO/W1C 동작 테이블 포인터이다. */
	const struct pci_bridge_reg_behavior *behavior;

	/* NVMe: 먼저 같은 레지스터를 4바이트로 읽어 현재 값(old)을
	 * 가져온다. */
	ret = pci_bridge_emul_conf_read(bridge, reg, 4, &old);
	/* NVMe: 읽기가 실패하면 쓰기도 실패로 처리한다. */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/* NVMe: 쓰려는 주소가 표준 PCI 헤더 영역이면 */
	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		/* NVMe: 기본 설정 공간 write 콜백과 conf 배열,
		 * pci_regs_behavior를 사용한다. */
		write_op = bridge->ops->write_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	/* NVMe: 쓰려는 주소가 PCIe capability 영역이면 */
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		/* NVMe: PCIe capability 시작점을 0 기준으로 재조정하고
		 * pcie_conf 배열과 pcie_cap_regs_behavior를 사용한다. */
		reg -= bridge->pcie_start;
		write_op = bridge->ops->write_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	/* NVMe: 쓰려는 주소가 PCIe extended capability 공간이면 */
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		/* NVMe: AER, VC, NPEM 등 확장 capability 쓰기를 write_ext
		 * 콜백으로 전달한다. */
		reg -= PCI_CFG_SPACE_SIZE;
		write_op = bridge->ops->write_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		/* NVMe: 처리되지 않는 영역에 대한 쓰기는 무시하고 성공으로
		 * 반환한다. */
		return PCIBIOS_SUCCESSFUL;
	}

	/* NVMe: where의 하위 2비트를 8배하여 바이트 시프트량을 계산한다. */
	shift = (where & 0x3) * 8;

	/* NVMe: 쓰기 크기에 따라 업데이트할 비트 마스크를 만든다. */
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
		/* NVMe: 1/2/4바이트 외 크기는 잘못된 레지스터 번호로 처리한다. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* NVMe: 동작 테이블이 있으면 RO/RW/W1C 규칙을 적용하여 새 값을
	 * 조합한다. */
	if (behavior) {
		/* Keep all bits, except the RW bits */
		/* NVMe: 마스크로 지정된 위치 중 RW가 아닌 비트는 old 값을
		 * 유지한다. */
		new = old & (~mask | ~behavior[reg / 4].rw);

		/* Update the value of the RW bits */
		/* NVMe: 마스크 내 RW 비트만 요청된 값으로 갱신한다. */
		new |= (value << shift) & (behavior[reg / 4].rw & mask);

		/* Clear the W1C bits */
		/* NVMe: 쓰기 값에서 W1C 비트가 1로 설정된 부분은 현재 값에서
		 * 클리어한다. */
		new &= ~((value << shift) & (behavior[reg / 4].w1c & mask));
	} else {
		/* NVMe: 동작 테이블이 없으면 마스크 영역만 단순 교체한다. */
		new = old & ~mask;
		new |= (value << shift) & mask;
	}

	/* NVMe: 메모리 내 설정 공간이 있으면 W1C 처리가 끝난 새 값을
	 * 저장한다. */
	if (cfgspace) {
		/* Save the new value with the cleared W1C bits into the cfgspace */
		cfgspace[reg / 4] = cpu_to_le32(new);
	}

	/* NVMe: 동작 테이블이 있으면 write_op에 전달할 값을 다시
	 * 조정한다. */
	if (behavior) {
		/*
		 * Clear the W1C bits not specified by the write mask, so that the
		 * write_op() does not clear them.
		 */
		/* NVMe: 쓰기 마스크 밖의 W1C 비트는 클리어하지 않도록 new에서
		 * 제거한다. */
		new &= ~(behavior[reg / 4].w1c & ~mask);

		/*
		 * Set the W1C bits specified by the write mask, so that write_op()
		 * knows about that they are to be cleared.
		 */
		/* NVMe: 쓰기 마스크 안의 W1C 비트를 new에 다시 설정하여
		 * 콜백이 클리어 사실을 인지할 수 있게 한다. */
		new |= (value << shift) & (behavior[reg / 4].w1c & mask);
	}

	/* NVMe: 컨트롤러 콜백이 등록되어 있으면 old/new/mask를 전달하여
	 * 호출한다. */
	if (write_op)
		write_op(bridge, reg, old, new, mask);

	/* NVMe: 쓰기를 완료하면 PCIBIOS_SUCCESSFUL을 반환한다. */
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_write);
/* NVMe: write 함수를 GPL EXPORT하여 PCI 컨트롤러 드라이버가 사용할 수
 * 있게 한다. NVMe 장치의 BAR, 버스 마스터, MSI/MSI-X, AER 등 설정이
 * 이 경로를 통해 브리지에 반영된다. */
