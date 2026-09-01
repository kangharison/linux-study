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

/* [한국어] PCI 브리지 표준 헤더(0x00~0x3f)의 레지스터별 동작 규칙 표.
 * 인덱스가 [오프셋 / 4] 인 것은 config 접근이 32비트 워드 단위이기 때문이며,
 * 그래서 한 항목이 인접한 두 개의 16비트 레지스터를 함께 서술하는 경우가 많다
 * (예: PCI_COMMAND 항목이 하위 16비트에 Command, 상위 16비트에 Status 를 담는다).
 * 위 kernel-doc 이 밝히듯, 여기 선언되지 않은 비트는 모두 예약으로 간주되어
 * 읽으면 0 이 나온다 — PCIe 5.0 규격이 요구하는 동작이다. */
static const
struct pci_bridge_reg_behavior pci_regs_behavior[PCI_STD_HEADER_SIZEOF / 4] = {

	/* [한국어] Vendor ID / Device ID(0x00). 장치 식별자는 소프트웨어가 바꿀 수 없으므로 전부 읽기 전용이다. */
	[PCI_VENDOR_ID / 4] = { .ro = ~0 },

	[PCI_COMMAND / 4] = {
		/* [한국어] Command 레지스터에서 쓰기 가능한 비트들 — I/O·메모리 디코딩, 버스 마스터,
		 * 패리티 오류 응답, SERR 활성화. 브리지가 실제로 동작을 바꿀 수 있는 것들이다. */
		.rw = (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		       PCI_COMMAND_MASTER | PCI_COMMAND_PARITY |
		       PCI_COMMAND_SERR),

		/* [한국어] Command 의 나머지 비트는 읽기 전용이다. Special Cycles, Memory Write and
		 * Invalidate, VGA Palette Snoop, Wait Cycle, Fast Back-to-Back 은
		 * 이 흉내 브리지가 지원하지 않으므로 쓰기를 받아들이지 않는다. */
		.ro = ((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			PCI_COMMAND_FAST_BACK) |
		       /* [한국어] 상위 16비트인 Status 레지스터에서 읽기 전용인 능력 표시 비트들.
		        * 16비트 시프트가 Command 와 Status 를 한 워드에 담는 배치를 반영한다. */
		       (PCI_STATUS_CAP_LIST | PCI_STATUS_66MHZ |
			PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MASK) << 16),

		/* [한국어] Status 의 오류 비트들은 W1C — 1 을 써야 지워진다. 소프트웨어가 오류를
		 * 확인한 뒤 명시적으로 지우는 PCI 의 표준 관용이다. */
		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	/* [한국어] Class Code / Revision ID(0x08). 전부 읽기 전용이며,
	 * pci_bridge_emul_init() 이 여기에 브리지 class 를 채워 넣는다. */
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
	/* [한국어] Cache Line Size / Latency Timer / Header Type / BIST(0x0c) 를 한 워드로 다룬다.
	 * 위 영어 주석이 네 레지스터를 모두 읽기 전용으로 두는 근거를 규격 인용과 함께 밝힌다 —
	 * Memory Write and Invalidate 를 지원하지 않고, 버스트 전송이 두 단계를 넘지 않으며,
	 * Header Type 은 언제나 읽기 전용이고, BIST 미지원 브리지는 0 을 돌려줘야 한다. */
	[PCI_CACHE_LINE_SIZE / 4] = { .ro = ~0 },

	/*
	 * Base Address registers not used must be implemented as
	 * read-only registers that return 0 when read.
	 */
	/* [한국어] BAR0(0x10). 위 영어 주석대로 쓰지 않는 BAR 는 읽으면 0 을 돌려주는
	 * 읽기 전용으로 구현해야 한다. */
	[PCI_BASE_ADDRESS_0 / 4] = { .ro = ~0 },
	/* [한국어] BAR1(0x14). 같은 이유로 읽기 전용이다. */
	[PCI_BASE_ADDRESS_1 / 4] = { .ro = ~0 },

	[PCI_PRIMARY_BUS / 4] = {
		/* Primary, secondary and subordinate bus are RW */
		/* [한국어] Primary / Secondary / Subordinate Bus Number 는 쓰기 가능하다.
		 * PCI 코어가 버스를 열거하며 이 번호들을 배정하기 때문에 반드시 써져야 한다.
		 * GENMASK(24, 0) 이 세 바이트를 덮는다(옆의 상류 주석). */
		.rw = GENMASK(24, 0),

		/* Secondary latency is read-only */
		/* [한국어] Secondary Latency Timer 는 읽기 전용이다(옆의 상류 주석).
		 * GENMASK(31, 24) 와 위 rw 마스크가 비트 24 에서 겹치는데,
		 * conf_write 가 rw 를 먼저 적용하므로 실질적으로 쓰기 가능으로 동작한다. */
		.ro = GENMASK(31, 24),
	},

	[PCI_IO_BASE / 4] = {
		/* The high four bits of I/O base/limit are RW */
		/* [한국어] I/O Base 와 I/O Limit 의 상위 4비트만 쓰기 가능하다(옆의 상류 주석).
		 * I/O 창은 4KB 단위로 정렬되므로 하위 12비트를 표현할 필요가 없고,
		 * 그래서 니블 하나가 주소의 [15:12] 를 담는다. */
		.rw = (GENMASK(15, 12) | GENMASK(7, 4)),

		/* The low four bits of I/O base/limit are RO */
		/* [한국어] 하위 4비트는 읽기 전용이다 — 32비트 I/O 주소 지원 여부를 나타내는
		 * 능력 표시이지 주소가 아니기 때문이다. 여기에 상위 16비트의 Secondary
		 * Status 읽기 전용 비트도 함께 담긴다. */
		.ro = (((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			 PCI_STATUS_DEVSEL_MASK) << 16) |
		       GENMASK(11, 8) | GENMASK(3, 0)),

		/* [한국어] Secondary Status 의 오류 비트들도 W1C 다. */
		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_MEMORY_BASE / 4] = {
		/* The high 12-bits of mem base/limit are RW */
		/* [한국어] Memory Base / Limit 는 상위 12비트가 쓰기 가능하다(옆의 상류 주석).
		 * 메모리 창이 1MB 단위로 정렬되므로 [31:20] 만 의미가 있다. */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of mem base/limit are RO */
		/* [한국어] 하위 4비트는 읽기 전용 — 이 자리는 예약이며 규격상 0 이어야 한다. */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_MEMORY_BASE / 4] = {
		/* The high 12-bits of pref mem base/limit are RW */
		/* [한국어] Prefetchable Memory Base / Limit 도 같은 배치다. */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of pref mem base/limit are RO */
		/* [한국어] 하위 4비트는 읽기 전용이지만 의미가 다르다 — 여기서는 64비트 주소 지원
		 * 여부를 나타내는 능력 표시다. */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_BASE_UPPER32 / 4] = {
		/* [한국어] Prefetchable Base Upper 32(0x28)는 전부 쓰기 가능하다.
		 * 64비트 프리페치 창의 상위 절반이라 정렬 제약이 없다. */
		.rw = ~0,
	},

	[PCI_PREF_LIMIT_UPPER32 / 4] = {
		/* [한국어] Prefetchable Limit Upper 32(0x2c)도 마찬가지. */
		.rw = ~0,
	},

	[PCI_IO_BASE_UPPER16 / 4] = {
		/* [한국어] I/O Base/Limit Upper 16(0x30). 32비트 I/O 주소를 쓸 때의 상위 절반이며
		 * 역시 전부 쓰기 가능하다. */
		.rw = ~0,
	},

	[PCI_CAPABILITY_LIST / 4] = {
		/* [한국어] Capabilities Pointer(0x34)의 하위 8비트만 읽기 전용으로 노출한다.
		 * 그 값은 init 이 pcie_start 또는 ssid_start 로 채우며, 소프트웨어가
		 * capability 목록의 시작을 바꿀 수는 없다. */
		.ro = GENMASK(7, 0),
	},

	/*
	 * If expansion ROM is unsupported then ROM Base Address register must
	 * be implemented as read-only register that return 0 when read, same
	 * as for unused Base Address registers.
	 */
	[PCI_ROM_ADDRESS1 / 4] = {
		/* [한국어] Expansion ROM Base Address(0x38). 위 영어 주석대로 ROM 을 지원하지 않으면
		 * 쓰지 않는 BAR 와 같이 읽으면 0 인 읽기 전용이어야 한다. */
		.ro = ~0,
	},

	/*
	 * Interrupt line (bits 7:0) are RW, interrupt pin (bits 15:8)
	 * are RO, and bridge control (31:16) are a mix of RW, RO,
	 * reserved and W1C bits
	 */
	[PCI_INTERRUPT_LINE / 4] = {
		/* Interrupt line is RW */
		/* [한국어] Interrupt Line(비트 7:0)은 쓰기 가능하다 — 펌웨어나 OS 가 배정한 IRQ 번호를
		 * 적어 두는 소프트웨어 전용 필드이기 때문이다(옆의 상류 주석).
		 * 함께 상위 16비트의 Bridge Control 쓰기 가능 비트들도 담는다 —
		 * 패리티, SERR, ISA, VGA, Master Abort, 그리고 Secondary Bus Reset. */
		.rw = (GENMASK(7, 0) |
		       ((PCI_BRIDGE_CTL_PARITY |
			 PCI_BRIDGE_CTL_SERR |
			 PCI_BRIDGE_CTL_ISA |
			 PCI_BRIDGE_CTL_VGA |
			 PCI_BRIDGE_CTL_MASTER_ABORT |
			 PCI_BRIDGE_CTL_BUS_RESET |
			 BIT(8) | BIT(9) | BIT(11)) << 16)),

		/* Interrupt pin is RO */
		/* [한국어] Interrupt Pin(비트 15:8)은 읽기 전용이다 — 하드웨어가 어느 INTx 선에
		 * 연결되어 있는지는 소프트웨어가 바꿀 수 없다. Bridge Control 의
		 * Fast Back-to-Back 도 읽기 전용이다. */
		.ro = (GENMASK(15, 8) | ((PCI_BRIDGE_CTL_FAST_BACK) << 16)),

		/* [한국어] Bridge Control 의 비트 10(Discard Timer Status)은 W1C 다. */
		.w1c = BIT(10) << 16,
	},
};

/* [한국어] PCIe capability 영역(0x00~PCI_CAP_PCIE_SIZEOF)의 동작 규칙 표.
 * 위 표와 같은 [오프셋 / 4] 색인 방식을 쓰며, 이쪽은 init 이 has_pcie 일 때만
 * 복사해 쓴다. */
static const
struct pci_bridge_reg_behavior pcie_cap_regs_behavior[PCI_CAP_PCIE_SIZEOF / 4] = {

	[PCI_CAP_LIST_ID / 4] = {
		/*
		 * Capability ID, Next Capability Pointer and
		 * bits [14:0] of Capabilities register are all read-only.
		 * Bit 15 of Capabilities register is reserved.
		 */
		/* [한국어] Capability ID, Next Pointer, 그리고 Capabilities 레지스터의 [14:0] 이
		 * 모두 읽기 전용이다(위 영어 주석). 비트 31 을 제외한 GENMASK(30, 0) 인 것은
		 * Capabilities 의 비트 15 가 예약이기 때문이다. */
		.ro = GENMASK(30, 0),
	},

	[PCI_EXP_DEVCAP / 4] = {
		/*
		 * Bits [31:29] and [17:16] are reserved.
		 * Bits [27:18] are reserved for non-upstream ports.
		 * Bits 28 and [14:6] are reserved for non-endpoint devices.
		 * Other bits are read-only.
		 */
		/* [한국어] Device Capabilities 에서 실제로 노출하는 읽기 전용 비트만 남긴다.
		 * 위 영어 주석이 어느 비트가 예약이고 어느 것이 엔드포인트나 업스트림
		 * 포트 전용인지 밝히며, 브리지에는 해당하지 않는 것을 전부 제외했다. */
		.ro = BIT(15) | GENMASK(5, 0),
	},

	[PCI_EXP_DEVCTL / 4] = {
		/*
		 * Device control register is RW, except bit 15 which is
		 * reserved for non-endpoints or non-PCIe-to-PCI/X bridges.
		 */
		/* [한국어] Device Control 은 [14:0] 이 쓰기 가능하다. 비트 15 는 엔드포인트나
		 * PCIe-to-PCI/X 브리지 전용이라 제외한다(위 영어 주석). */
		.rw = GENMASK(14, 0),

		/*
		 * Device status register has bits 6 and [3:0] W1C, [5:4] RO,
		 * the rest is reserved. Also bit 6 is reserved for non-upstream
		 * ports.
		 */
		/* [한국어] Device Status 의 오류 비트 [3:0] 은 W1C 다. */
		.w1c = GENMASK(3, 0) << 16,
		/* [한국어] [5:4] 는 읽기 전용 상태 비트다. */
		.ro = GENMASK(5, 4) << 16,
	},

	[PCI_EXP_LNKCAP / 4] = {
		/*
		 * All bits are RO, except bit 23 which is reserved and
		 * bit 18 which is reserved for non-upstream ports.
		 */
		/* [한국어] Link Capabilities 는 거의 전부 읽기 전용이다. 비트 23(예약)과
		 * CLKPM(업스트림 포트 전용)만 제외한다. lower_32_bits() 로 감싼 것은
		 * ~ 연산 결과의 타입 폭을 32비트로 확정하기 위해서다. */
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
		/* [한국어] Link Control 의 쓰기 가능 비트들 — ASPM 설정, 링크 재훈련,
		 * 공통 클럭 설정 등. 위 영어 주석이 어느 구간이 예약인지 밝힌다. */
		.rw = GENMASK(15, 14) | GENMASK(11, 9) | GENMASK(7, 3) | GENMASK(1, 0),
		/* [한국어] Link Status 의 [13:0] 은 읽기 전용 — 협상된 속도와 폭, 링크 훈련 상태다. */
		.ro = GENMASK(13, 0) << 16,
		/* [한국어] Link Status 의 [15:14] 는 W1C — 대역폭 관리 이벤트 비트다. */
		.w1c = GENMASK(15, 14) << 16,
	},

	[PCI_EXP_SLTCAP / 4] = {
		/* [한국어] Slot Capabilities 는 전부 읽기 전용이다. 슬롯의 물리적 능력은
		 * 소프트웨어가 바꿀 수 없다. */
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
		/* [한국어] Slot Control 은 [14:0] 이 쓰기 가능하다 — 핫플러그 인터럽트 허용과
		 * LED 제어 비트들이다(위 영어 주석). */
		.rw = GENMASK(14, 0),
		/* [한국어] Slot Status 의 이벤트 비트들은 W1C — attention 버튼 눌림, 전원 결함,
		 * MRL 센서 변화, 카드 존재 변화, 명령 완료, 데이터 링크 상태 변화. */
		.w1c = (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
			PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_PDC |
			PCI_EXP_SLTSTA_CC | PCI_EXP_SLTSTA_DLLSC) << 16,
		/* [한국어] Slot Status 의 현재 상태 비트들은 읽기 전용 — MRL 센서 상태,
		 * 카드 존재 상태, 전기 인터록 상태. 이벤트(W1C)와 상태(RO)를 구분하는
		 * 것이 핫플러그 레지스터 설계의 핵심이다. */
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
		/* [한국어] Root Control 의 쓰기 가능 비트들 — 정정 가능·비치명적·치명적 오류 보고
		 * 인터럽트 허용, PME 인터럽트 허용, 그리고 RRS 소프트웨어 가시성 활성화. */
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

static pci_bridge_emul_read_status_t
/* [한국어]
 * pci_bridge_emul_read_ssid - Subsystem ID capability 영역의 읽기를 처리한다
 *
 * @bridge: 흉내 브리지 객체.
 * @reg: 이 capability 안에서의 상대 오프셋(호출자가 이미 ssid_start 를 뺐다).
 * @value: 결과를 담을 출력 인자.
 * @return: PCI_BRIDGE_EMUL_HANDLED = 값을 채웠다.
 *       PCI_BRIDGE_EMUL_NOT_HANDLED = 이 capability 에 속하지 않는 오프셋이다.
 *
 * 다른 영역과 달리 드라이버 콜백이 아니라 이 파일의 전용 함수가 처리한다.
 * SSID capability 의 내용이 전적으로 struct pci_bridge_emul 의 두 필드
 * (subsystem_vendor_id, subsystem_id)에서 나오고, 하드웨어를 볼 일이 전혀
 * 없기 때문이다. 그래서 저장소(cfgspace)도 규칙 표(behavior)도 두지 않는다.
 *
 * capability 헤더 워드를 만드는 부분이 이 함수의 핵심이다. ID 는 SSVID 로
 * 고정이고, next 포인터는 PCIe capability 가 이보다 뒤에 있을 때만 그 오프셋을
 * 넣는다 — 앞에 있으면 0 을 넣어 여기가 목록의 끝이 된다. 두 capability 의
 * 배치가 init 에서 상황에 따라 달라지므로, 링크도 그에 맞춰 실행 시점에 정해진다.
 *
 * NOT_HANDLED 를 돌려주면 호출자가 저장된 값을 가져오려 하는데, SSID 영역은
 * cfgspace 가 NULL 이라 결과적으로 0 이 나온다 — 구현되지 않은 config 영역이
 * 0 을 반환해야 한다는 규격과 맞는다.
 *
 * 실행 컨텍스트: config 읽기 경로. PCI 코어의 pci_lock 아래에서 불린다.
 *
 * 에러 경로: 없다. 모르는 오프셋은 NOT_HANDLED 로 답한다.
 *
 * 호출 체인:
 *   pci_bridge_emul_conf_read() → read_op == [이 함수]
 */
pci_bridge_emul_read_ssid(struct pci_bridge_emul *bridge, int reg, u32 *value)
{
	/* [한국어] 오프셋에 따라 SSID capability 안의 어느 레지스터를 요청했는지 가른다.
	 * 이 capability 는 두 워드뿐이라 switch 로 충분하다. */
	switch (reg) {
	case PCI_CAP_LIST_ID:
		/* [한국어] capability 헤더 워드. ID 는 SSVID 이고, PCIe capability 가 이보다 뒤에
		 * 있으면 그 오프셋을 next 포인터로 넣어 목록을 잇는다.
		 * PCIe 가 앞에 있으면 0 을 넣어 여기가 목록의 끝이 된다. */
		*value = PCI_CAP_ID_SSVID |
			((bridge->pcie_start > bridge->ssid_start) ? (bridge->pcie_start << 8) : 0);
		return PCI_BRIDGE_EMUL_HANDLED;

	case PCI_SSVID_VENDOR_ID:
		/* [한국어] Subsystem Vendor ID(하위 16비트)와 Subsystem ID(상위 16비트)를 한 워드로 합친다.
		 * 드라이버가 init 전에 채워 둔 두 값을 그대로 노출한다. */
		*value = bridge->subsystem_vendor_id |
			(bridge->subsystem_id << 16);
		return PCI_BRIDGE_EMUL_HANDLED;

	/* [한국어] 그 밖의 오프셋은 이 capability 에 속하지 않는다. */
	default:
		/* [한국어] NOT_HANDLED 를 돌려주면 호출자가 저장된 값에서 가져오는데,
		 * SSID 는 cfgspace 가 NULL 이라 결과적으로 0 이 된다. */
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
int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags)
{
	/* [한국어] 컴파일 시점 검사 — struct pci_bridge_emul_conf 의 크기가 표준 헤더 크기와
	 * 정확히 같아야 한다. 필드를 하나라도 빠뜨리거나 패딩이 끼면 config 오프셋과
	 * 구조체 필드가 어긋나므로, 그 실수를 빌드에서 잡는다. */
	BUILD_BUG_ON(sizeof(bridge->conf) != PCI_BRIDGE_CONF_END);

	/*
	 * class_revision: Class is high 24 bits and revision is low 8 bit
	 * of this member, while class for PCI Bridge Normal Decode has the
	 * 24-bit value: PCI_CLASS_BRIDGE_PCI_NORMAL
	 */
	/* [한국어] class code 를 8비트 밀어 넣는다(위 영어 주석). 상위 24비트가 class 이고
	 * 하위 8비트가 revision 이라, 브리지 class 값을 그 자리에 맞추려면 시프트가 필요하다.
	 * |= 인 것은 드라이버가 미리 채워 둔 revision 을 보존하기 위해서다. */
	bridge->conf.class_revision |=
		cpu_to_le32(PCI_CLASS_BRIDGE_PCI_NORMAL << 8);
	/* [한국어] 헤더 타입을 브리지(0x01)로 고정한다. 이 값이 PCI 코어가 아래 필드 배치를
	 * 브리지 형식으로 해석하게 만든다. */
	bridge->conf.header_type = PCI_HEADER_TYPE_BRIDGE;
	/* [한국어] 캐시 라인 크기를 0x10(64바이트)으로 둔다. 읽기 전용이므로 이 값이 그대로 노출된다. */
	bridge->conf.cache_line_size = 0x10;
	/* [한국어] capability 목록이 있다고 Status 에 표시한다. 아래에서 실제 포인터가
	 * 0 이면 다시 검사해 세우므로, 여기서는 우선 켜 두는 셈이다. */
	bridge->conf.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
	/* [한국어] 동작 규칙 표를 복사한다. 원본은 const 정적 배열인데, 드라이버가 자기
	 * 하드웨어에 맞게 일부 규칙을 바꿀 수 있어야 하므로 인스턴스마다 사본이 필요하다. */
	bridge->pci_regs_behavior = kmemdup(pci_regs_behavior,
					    sizeof(pci_regs_behavior),
					    GFP_KERNEL);
	/* [한국어] 복사 실패. */
	if (!bridge->pci_regs_behavior)
		return -ENOMEM;

	/* If ssid_start and pcie_start were not specified then choose the lowest possible value. */
	/* [한국어] 두 capability 의 시작 오프셋을 드라이버가 지정하지 않았으면 가능한 가장
	 * 낮은 값을 고른다(옆의 상류 주석). 아래 세 갈래가 그 배치 계산이다. */
	if (!bridge->ssid_start && !bridge->pcie_start) {
		/* [한국어] 둘 다 미지정이고 SSID 를 쓴다면, */
		if (bridge->subsystem_vendor_id)
			/* [한국어] SSID 를 헤더 바로 뒤에 놓는다. */
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		/* [한국어] PCIe 도 쓴다면, */
		if (bridge->has_pcie)
			/* [한국어] SSID 다음에 이어 붙인다. ssid_start 가 0 이면(SSID 미사용) 결과적으로
			 * PCIe 가 헤더 바로 뒤에 오게 되어, 한 식으로 두 경우를 모두 처리한다. */
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	/* [한국어] PCIe 위치만 지정되어 있고 SSID 를 쓴다면, */
	} else if (!bridge->ssid_start && bridge->subsystem_vendor_id) {
		/* [한국어] 헤더와 PCIe 사이에 SSID 가 들어갈 자리가 있는지 본다. */
		if (bridge->pcie_start - PCI_BRIDGE_CONF_END >= PCI_CAP_SSID_SIZEOF)
			/* [한국어] 있으면 그 틈에 넣고, */
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		else
			/* [한국어] 없으면 PCIe 뒤에 놓는다. */
			bridge->ssid_start = bridge->pcie_start + PCI_CAP_PCIE_SIZEOF;
	/* [한국어] SSID 위치만 지정되어 있고 PCIe 를 쓴다면, */
	} else if (!bridge->pcie_start && bridge->has_pcie) {
		/* [한국어] 같은 방식으로 틈을 확인해, */
		if (bridge->ssid_start - PCI_BRIDGE_CONF_END >= PCI_CAP_PCIE_SIZEOF)
			/* [한국어] 헤더 바로 뒤에 넣거나, */
			bridge->pcie_start = PCI_BRIDGE_CONF_END;
		else
			/* [한국어] SSID 뒤에 놓는다. */
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	}

	/* [한국어] 둘 중 앞선 것이 capability 목록의 시작이다. 하나만 쓰면 다른 하나가 0 이라
	 * min 이 0 을 고를 것 같지만, 그 경우 목록 자체가 없다는 뜻이 되어 아래
	 * 조건문이 Status 비트를 세우지 않는다. */
	bridge->conf.capabilities_pointer = min(bridge->ssid_start, bridge->pcie_start);

	/* [한국어] 실제로 capability 가 하나라도 있으면, */
	if (bridge->conf.capabilities_pointer)
		/* [한국어] Status 의 CAP_LIST 비트를 세운다. */
		bridge->conf.status |= cpu_to_le16(PCI_STATUS_CAP_LIST);

	/* [한국어] PCIe capability 를 노출하는 경우. */
	if (bridge->has_pcie) {
		/* [한국어] capability ID 를 PCIe(0x10)로. */
		bridge->pcie_conf.cap_id = PCI_CAP_ID_EXP;
		/* [한국어] SSID 가 뒤에 있으면 그 오프셋을 next 로, 앞에 있으면 0(목록 끝)으로 둔다.
		 * 두 capability 의 순서가 배치에 따라 달라지므로 링크도 그에 맞춰 정해진다. */
		bridge->pcie_conf.next = (bridge->ssid_start > bridge->pcie_start) ?
					 bridge->ssid_start : 0;
		/* [한국어] 포트 종류를 루트 포트로 광고한다. 4비트 시프트가 그 필드의 위치다.
		 * 이 흉내 브리지를 쓰는 컨트롤러가 모두 루트 포트라는 전제다. */
		bridge->pcie_conf.cap |= cpu_to_le16(PCI_EXP_TYPE_ROOT_PORT << 4);
		/* [한국어] PCIe 규칙 표도 사본을 만든다. */
		bridge->pcie_cap_regs_behavior =
			kmemdup(pcie_cap_regs_behavior,
				sizeof(pcie_cap_regs_behavior),
				GFP_KERNEL);
		/* [한국어] 복사 실패. */
		if (!bridge->pcie_cap_regs_behavior) {
			/* [한국어] 앞서 성공한 PCI 규칙 표를 되돌린다 — 이 파일에서 유일한 되감기다. */
			kfree(bridge->pci_regs_behavior);
			return -ENOMEM;
		}
		/* These bits are applicable only for PCI and reserved on PCIe */
		/* [한국어] Latency Timer(비트 15:8). PCIe 에는 버스 중재라는
		 * 개념이 없어 이 필드가 의미를 잃었다. 읽기 전용 마스크에서
		 * 빼면 그 비트는 항상 0 으로 읽힌다. */
		/* [한국어] 이하 여섯 덩어리는 PCIe 에서 예약된 비트들을 규칙 표에서 지운다(옆의 상류 주석).
		 * 표는 PCI 브리지 기준으로 작성되어 있어, PCIe 로 노출할 때는 PCI 전용
		 * 비트를 읽기 전용 목록에서 빼야 규격대로 0 이 나온다.
		 * 먼저 Latency Timer 자리(비트 15:8)를 지운다. */
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
		/* [한국어] Command 의 PCI 전용 비트들과 Status 의 PCI 전용 능력 비트들을 지운다. */
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro &=
			~((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			   PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			   PCI_COMMAND_FAST_BACK) |
			  (PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		/* [한국어] 이 dword 의 최상위 바이트(31:24)는 Secondary Latency Timer 다.
		 * 위와 같은 이유로 PCIe 에서는 예약이다. */
		/* [한국어] Secondary Latency Timer 자리를 지운다. */
		bridge->pci_regs_behavior[PCI_PRIMARY_BUS / 4].ro &=
			~GENMASK(31, 24);
		/* [한국어] 이 dword 의 상위 16비트는 Secondary Status 다.
		 * Command/Status 쌍과 같은 이유로 버스 타이밍 비트들을 지운다. */
		/* [한국어] Secondary Status 의 PCI 전용 능력 비트들을 지운다. */
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro &=
			~((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		/* [한국어] 이 dword 의 상위 16비트는 Bridge Control 이다.
		 * Master Abort Mode 와 비트 8/9/11 은 PCIe 에서 예약이라
		 * 쓰기 가능 마스크에서 뺀다 — 쓰려고 해도 무시된다. */
		/* [한국어] Bridge Control 에서 PCIe 에 없는 쓰기 가능 비트들을 지운다. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].rw &=
			~((PCI_BRIDGE_CTL_MASTER_ABORT |
			   BIT(8) | BIT(9) | BIT(11)) << 16);
		/* [한국어] Fast Back-to-Back Enable 도 마찬가지로 예약. */
		/* [한국어] Fast Back-to-Back 읽기 전용 비트를 지운다. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].ro &=
			~((PCI_BRIDGE_CTL_FAST_BACK) << 16);
		/* [한국어] 비트 10(Discard Timer Status)은 원래 RW1C 였으나
		 * PCIe 에서는 예약이라 그 동작도 없앤다. */
		/* [한국어] Discard Timer Status W1C 비트를 지운다. */
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].w1c &=
			~(BIT(10) << 16);
	}

	/* [한국어] 드라이버가 프리페치 메모리 전달을 지원하지 않는다고 알렸으면, */
	if (flags & PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD) {
		/* [한국어] 그 레지스터를 통째로 읽기 전용으로 만들고, */
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].ro = ~0;
		/* [한국어] 쓰기 가능 비트를 모두 없앤다. 그러면 PCI 코어가 그 창에 값을 써도
		 * 반영되지 않아 0 으로 읽히고, 결국 창이 비활성으로 취급된다 —
		 * 하드웨어가 그 기능을 갖지 않은 것처럼 보이게 하는 방법이다. */
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].rw = 0;
	}

	/* [한국어] I/O 전달을 지원하지 않는 경우도 같은 방식이다. */
	if (flags & PCI_BRIDGE_EMUL_NO_IO_FORWARD) {
		/* [한국어] Command 의 I/O 디코딩 비트를 읽기 전용으로 옮기고, */
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro |= PCI_COMMAND_IO;
		/* [한국어] 쓰기 가능 목록에서 뺀다. */
		bridge->pci_regs_behavior[PCI_COMMAND / 4].rw &= ~PCI_COMMAND_IO;
		/* [한국어] I/O Base/Limit(하위 16비트)를 읽기 전용으로 고정한다.
		 * ro 에 넣고 rw 에서 빼는 두 동작이 한 쌍이다 — 읽으면 0 이 나오고
		 * 쓰기는 무시된다. 그러면 소프트웨어가 "이 브리지는 I/O 창이
		 * 없다" 고 판단한다(base > limit 이 되므로). */
		/* [한국어] I/O Base/Limit 도 읽기 전용으로, */
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro |= GENMASK(15, 0);
		/* [한국어] 쓰기 가능 목록에서 제거하고, */
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].rw &= ~GENMASK(15, 0);
		/* [한국어] 32비트 I/O 주소용 상위 16비트 레지스터도 통째로
		 * 읽기 전용 0 으로 만든다. 여기는 dword 전체가 그 용도라
		 * &= 가 아니라 = 로 덮어쓴다. */
		/* [한국어] 상위 16비트 레지스터도, */
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].ro = ~0;
		/* [한국어] 같은 처리를 한다. */
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].rw = 0;
	}

	/* [한국어] 초기화 성공. */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_init);

/*
 * Cleanup a pci_bridge_emul structure that was previously initialized
 * using pci_bridge_emul_init().
 */
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge)
{
	/* [한국어] PCIe 표는 has_pcie 일 때만 할당했으므로 그때만 해제한다. */
	if (bridge->has_pcie)
		/* [한국어] PCIe 규칙 표 해제. */
		kfree(bridge->pcie_cap_regs_behavior);
	/* [한국어] PCI 규칙 표 해제. kfree(NULL) 은 안전하므로 조건 없이 부른다. */
	kfree(bridge->pci_regs_behavior);
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_cleanup);

/*
 * Should be called by the PCI controller driver when reading the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->read_base or ->ops->read_pcie operations.
 */
int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value)
{
	/* [한국어] 드라이버 콜백의 결과. */
	int ret;
	/* [한국어] 오프셋을 4바이트 경계로 내린다. 이 흉내 계층은 언제나 워드 단위로
	 * 다루고, 요청한 폭으로 자르는 것은 마지막에 한다. */
	int reg = where & ~3;
	/* [한국어] 영역에 따라 달라질 읽기 콜백 포인터. */
	pci_bridge_emul_read_status_t (*read_op)(struct pci_bridge_emul *bridge,
						 int reg, u32 *value);
	/* [한국어] 저장된 config 값의 배열. SSID 와 확장 영역에는 저장소가 없어 NULL 이 된다. */
	__le32 *cfgspace;
	/* [한국어] 그 영역의 동작 규칙 표. 역시 없을 수 있다. */
	const struct pci_bridge_reg_behavior *behavior;

	/* [한국어] 표준 브리지 헤더 영역(옆의 상류 주석). */
	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		/* [한국어] 드라이버의 read_base 콜백, */
		read_op = bridge->ops->read_base;
		/* [한국어] 저장소는 conf 구조체, */
		cfgspace = (__le32 *) &bridge->conf;
		/* [한국어] 규칙 표는 PCI 쪽. */
		behavior = bridge->pci_regs_behavior;
	/* [한국어] SSID capability 영역인지 확인한다. 시작 오프셋과 크기, 그리고 드라이버가
	 * 실제로 SSID 를 쓰는지까지 모두 봐야 한다. */
	} else if (reg >= bridge->ssid_start && reg < bridge->ssid_start + PCI_CAP_SSID_SIZEOF &&
		   bridge->subsystem_vendor_id) {
		/* Emulated PCI Bridge Subsystem Vendor ID capability */
		/* [한국어] 영역 안에서의 상대 오프셋으로 바꾼다. 이후 콜백과 배열 색인이 모두
		 * 그 상대 오프셋을 쓴다. */
		reg -= bridge->ssid_start;
		/* [한국어] SSID 는 드라이버 콜백이 아니라 이 파일의 전용 함수가 처리한다 —
		 * 값이 전적으로 bridge 구조체 필드에서 나오기 때문이다. */
		read_op = pci_bridge_emul_read_ssid;
		/* [한국어] 저장소가 없다. */
		cfgspace = NULL;
		/* [한국어] 규칙 표도 없다 — 아래 예약 비트 마스킹을 건너뛰게 된다. */
		behavior = NULL;
	/* [한국어] PCIe capability 영역인지 확인한다(옆의 상류 주석). */
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		/* [한국어] 상대 오프셋으로. */
		reg -= bridge->pcie_start;
		/* [한국어] 드라이버의 read_pcie 콜백. */
		read_op = bridge->ops->read_pcie;
		/* [한국어] 저장소는 pcie_conf 구조체. */
		cfgspace = (__le32 *) &bridge->pcie_conf;
		/* [한국어] 규칙 표는 PCIe 쪽. */
		behavior = bridge->pcie_cap_regs_behavior;
	/* [한국어] 확장 config 영역(0x100 이상)인지 확인한다(옆의 상류 주석). */
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		/* [한국어] 상대 오프셋으로. */
		reg -= PCI_CFG_SPACE_SIZE;
		/* [한국어] 드라이버의 read_ext 콜백. */
		read_op = bridge->ops->read_ext;
		/* [한국어] 저장소 없음. */
		cfgspace = NULL;
		/* [한국어] 규칙 표 없음. */
		behavior = NULL;
	/* [한국어] 어느 영역에도 속하지 않으면(옆의 상류 주석), */
	} else {
		/* Not implemented */
		/* [한국어] 0 을 돌려준다. 오류가 아니라 성공으로 처리하는 것이 중요하다 —
		 * PCI 규격상 구현되지 않은 config 영역은 0 을 반환해야 하기 때문이다. */
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	/* [한국어] 드라이버가 그 영역의 콜백을 제공했으면, */
	if (read_op)
		/* [한국어] 먼저 물어본다. 드라이버가 하드웨어에서 값을 가져와야 하는 레지스터
		 * (예: 링크 상태)를 여기서 가로챈다. */
		ret = read_op(bridge, reg, value);
	else
		/* [한국어] 콜백이 없으면 처리되지 않은 것으로 취급한다. */
		ret = PCI_BRIDGE_EMUL_NOT_HANDLED;

	/* [한국어] 드라이버가 처리하지 않았으면 저장된 값을 쓴다. */
	if (ret == PCI_BRIDGE_EMUL_NOT_HANDLED) {
		/* [한국어] 저장소가 있으면, */
		if (cfgspace)
			/* [한국어] 리틀엔디안으로 저장된 워드를 CPU 바이트 순서로 바꿔 돌려준다. */
			*value = le32_to_cpu(cfgspace[reg / 4]);
		else
			/* [한국어] 저장소가 없으면 0. */
			*value = 0;
	}

	/*
	 * Make sure we never return any reserved bit with a value
	 * different from 0.
	 */
	/* [한국어] 규칙 표가 있으면(위 영어 주석대로 예약 비트가 0 이 아닌 값으로 나가지
	 * 않게 하려면), */
	if (behavior)
		/* [한국어] RO/RW/W1C 중 어디에도 속하지 않는 비트를 지운다. 세 마스크의 합집합
		 * 밖이 곧 예약 비트이며, PCIe 규격이 그것을 0 으로 읽도록 요구한다.
		 * 드라이버 콜백이 채운 값에도 이 마스킹이 적용된다는 점이 중요하다. */
		*value &= behavior[reg / 4].ro | behavior[reg / 4].rw |
			  behavior[reg / 4].w1c;

	/* [한국어] 1바이트 요청이면, */
	if (size == 1)
		/* [한국어] 워드 안에서 해당 바이트를 뽑는다. */
		*value = (*value >> (8 * (where & 3))) & 0xff;
	/* [한국어] 2바이트 요청이면, */
	else if (size == 2)
		/* [한국어] 해당 하프워드를 뽑는다. */
		*value = (*value >> (8 * (where & 3))) & 0xffff;
	/* [한국어] 4바이트도 아니면 잘못된 폭이다. */
	else if (size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_read);

/*
 * Should be called by the PCI controller driver when writing the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->write_base or ->ops->write_pcie operations.
 */
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value)
{
	/* [한국어] 오프셋을 워드 경계로 내린다. */
	int reg = where & ~3;
	/* [한국어] mask: 이번 쓰기가 건드릴 비트. old/new: 변경 전후 워드. shift: 바이트 위치. */
	int mask, ret, old, new, shift;
	/* [한국어] 영역에 따라 달라질 쓰기 콜백. */
	void (*write_op)(struct pci_bridge_emul *bridge, int reg,
			 u32 old, u32 new, u32 mask);
	/* [한국어] 저장소. */
	__le32 *cfgspace;
	/* [한국어] 동작 규칙 표. */
	const struct pci_bridge_reg_behavior *behavior;

	/* [한국어] 먼저 워드 전체를 읽는다. 부분 쓰기를 워드 단위로 처리하려면 기존 값이
	 * 필요하고, 드라이버 콜백에 "무엇이 어떻게 바뀌었는지"를 알려 주려면
	 * 이전 값이 있어야 한다. */
	ret = pci_bridge_emul_conf_read(bridge, reg, 4, &old);
	/* [한국어] 읽기 실패. */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/* [한국어] 표준 브리지 헤더 영역(옆의 상류 주석). */
	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		/* [한국어] 드라이버의 write_base 콜백. */
		write_op = bridge->ops->write_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	/* [한국어] PCIe capability 영역(옆의 상류 주석).
	 * [상류 코드 관찰] 읽기 경로에는 SSID 갈래가 있는데 쓰기 경로에는 없다.
	 * SSID capability 는 전부 읽기 전용이라 쓰기를 받을 이유가 없기 때문이며,
	 * 그 결과 SSID 영역에 쓰면 아래 default 갈래로 떨어져 조용히 무시된다. */
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		/* [한국어] 상대 오프셋으로. */
		reg -= bridge->pcie_start;
		write_op = bridge->ops->write_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	/* [한국어] 확장 config 영역(옆의 상류 주석). */
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		/* [한국어] 상대 오프셋으로. */
		reg -= PCI_CFG_SPACE_SIZE;
		write_op = bridge->ops->write_ext;
		cfgspace = NULL;
		behavior = NULL;
	/* [한국어] 어느 영역도 아니면(옆의 상류 주석), */
	} else {
		/* Not implemented */
		/* [한국어] 아무 일도 하지 않고 성공을 돌려준다. */
		return PCIBIOS_SUCCESSFUL;
	}

	/* [한국어] 요청 오프셋의 바이트 위치를 비트 시프트로 바꾼다. */
	shift = (where & 0x3) * 8;

	/* [한국어] 요청한 폭에 해당하는 비트만 1인 마스크를 만든다.
	 * shift 는 dword 안에서의 바이트 오프셋 x 8 이다.
	 * 예: size=2, where=0x02 이면 shift=16 이라 마스크가 0xffff0000 —
	 * 즉 그 dword 의 상위 워드만 대상이 된다.
	 * 이 마스크로 dword 단위 저장소에서 요청한 부분만 뽑아내거나
	 * 갈아끼운다. */
	/* [한국어] 4바이트 쓰기면, */
	if (size == 4)
		mask = 0xffffffff;	/* [한국어] dword 전체 */
	/* [한국어] 2바이트면, */
	else if (size == 2)
		mask = 0xffff << shift;	/* [한국어] 워드 하나를 그 자리로 */
	/* [한국어] 1바이트면, */
	else if (size == 1)
		mask = 0xff << shift;	/* [한국어] 바이트 하나를 그 자리로 */
	else
		/* [한국어] 그 밖의 폭은 오류. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 규칙 표가 있으면 비트별 동작을 적용한다. */
	if (behavior) {
		/* Keep all bits, except the RW bits */
		/* [한국어] 먼저 이번 쓰기가 건드리지 않는 비트와, 건드리더라도 쓰기 불가인 비트를
		 * 옛 값에서 보존한다(옆의 상류 주석). */
		new = old & (~mask | ~behavior[reg / 4].rw);

		/* Update the value of the RW bits */
		/* [한국어] 쓰기 가능하면서 이번 마스크에 든 비트만 새 값으로 갱신한다. */
		new |= (value << shift) & (behavior[reg / 4].rw & mask);

		/* Clear the W1C bits */
		/* [한국어] W1C 비트는 1 을 쓴 자리를 지운다 — 그것이 W1C 의 정의다.
		 * 위 두 줄과 달리 값을 넣는 것이 아니라 빼는 연산이라는 점에 주의한다. */
		new &= ~((value << shift) & (behavior[reg / 4].w1c & mask));
	/* [한국어] 규칙 표가 없는 영역(SSID·확장)에서는, */
	} else {
		/* [한국어] 마스크 자리를 비우고, */
		new = old & ~mask;
		/* [한국어] 새 값을 그대로 넣는다. 모든 비트가 쓰기 가능인 것처럼 다루는 셈이다. */
		new |= (value << shift) & mask;
	}

	/* [한국어] 저장소가 있으면, */
	if (cfgspace) {
		/* Save the new value with the cleared W1C bits into the cfgspace */
		/* [한국어] W1C 가 반영된 새 값을 저장한다(옆의 상류 주석). 이 시점의 new 는
		 * "소프트웨어가 보게 될 상태"다. */
		cfgspace[reg / 4] = cpu_to_le32(new);
	}

	/* [한국어] 규칙 표가 있으면 드라이버에 넘길 값을 한 번 더 가공한다. */
	if (behavior) {
		/*
		 * Clear the W1C bits not specified by the write mask, so that the
		 * write_op() does not clear them.
		 */
		/* [한국어] 이번 쓰기 마스크 밖의 W1C 비트를 지운다(위 영어 주석) — 드라이버가
		 * 그것까지 지우지 않게 하려는 것이다. 저장소에 넣은 값과 드라이버에 넘기는
		 * 값이 달라지는 지점이며, 이 두 단계 가공이 이 함수에서 가장 미묘한 부분이다. */
		new &= ~(behavior[reg / 4].w1c & ~mask);

		/*
		 * Set the W1C bits specified by the write mask, so that write_op()
		 * knows about that they are to be cleared.
		 */
		new |= (value << shift) & (behavior[reg / 4].w1c & mask);
	}

	/* [한국어] 드라이버가 그 영역의 쓰기 콜백을 제공했으면, */
	if (write_op)
		/* [한국어] 이전 값·새 값·쓰기 마스크 셋을 함께 넘긴다. 세 값이 모두 필요한 이유는
		 * 드라이버가 "어떤 비트가 어떻게 바뀌었는지"를 알아야 하기 때문이다 —
		 * 예컨대 Secondary Bus Reset 비트가 0 에서 1 로 바뀐 순간에만 실제 리셋을
		 * 수행하려면 old 와 new 를 비교해야 하고, mask 는 이번 쓰기가 건드리지 않은
		 * 비트의 변화를 무시하게 해 준다.
		 * 이 호출이 흉내 브리지와 실제 하드웨어를 잇는 지점이다. */
		write_op(bridge, reg, old, new, mask);

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_write);
