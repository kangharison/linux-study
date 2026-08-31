/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] 가짜 브리지 에뮬레이션의 자료구조 정의 (pci-bridge-emul.h)
 *
 * === 파일의 역할 ===
 * pci-bridge-emul.c 가 쓰는 자료구조와 상수를 정의한다. 세 가지가 핵심이다.
 *
 *   struct pci_bridge_emul_conf     — 가짜 PCI 브리지 헤더(타입 1)의
 *     레지스터들을 그대로 옮긴 구조체. vendor, device, command, status 부터
 *     프라이머리/세컨더리 버스 번호, I/O 와 메모리 윈도우까지 스펙 순서대로
 *     늘어놓았다. 이것이 메모리에 놓이면 그 자체가 config space 가 된다.
 *
 *   struct pci_bridge_emul_pcie_conf — PCIe capability 부분의 같은 것.
 *     선택적이며, 이것을 제공하면 커널이 이 가짜 브리지를 PCIe 브리지로
 *     인식한다.
 *
 *   struct pci_bridge_reg_behavior  — dword 하나의 성질. ro / rw / w1c
 *     세 마스크로 각 비트가 어떻게 동작해야 하는지 기술한다. 이것이
 *     "가짜지만 진짜처럼 보이게" 만드는 열쇠다.
 *
 * 구조체 필드가 스펙 레지스터 순서와 정확히 일치해야 한다는 점이 중요하다.
 * config 접근은 오프셋으로 이뤄지므로, 필드 순서가 어긋나면 엉뚱한 값이
 * 읽힌다. 그래서 __le16 / __le32 같은 엔디언 명시 타입을 쓰고 패딩을
 * 명시적으로 넣는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pci-bridge-emul.c 가 이 정의대로 메모리를 잡고 동작 마스크를 채운다.
 * controller/ 아래의 SoC 드라이버들이 이 헤더를 include 해
 * struct pci_bridge_emul 을 자기 구조체에 담고 ops 를 제공한다.
 *
 * === 타 모듈과의 연결 ===
 * 포함되는 곳: pci-bridge-emul.c, 그리고 브리지를 노출하지 않는
 *   SoC 컨트롤러 드라이버들(mvebu, aardvark, brcmstb, rockchip 등).
 * 의존하는 것: linux/types.h 의 엔디언 타입.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관련이 없다. 다만 이 헤더가 정의하는 윈도우 레지스터
 * (memory base/limit)가 그 아래 NVMe 의 BAR 주소를 결정하는 근거가 된다.
 * 자세한 것은 pci-bridge-emul.c 의 헤더 참고.
 *
 * === 주요 함수/구조체 요약 ===
 * struct pci_bridge_emul_conf      : 가짜 PCI 브리지 헤더 전체.
 * struct pci_bridge_emul_pcie_conf : 가짜 PCIe capability 전체.
 * struct pci_bridge_reg_behavior   : dword 하나의 ro/rw/w1c 마스크.
 * struct pci_bridge_emul_ops       : 컨트롤러 드라이버가 제공하는 훅.
 *                                    읽기 전(read_base/read_pcie)과
 *                                    쓰기 후(write_base/write_pcie)에
 *                                    실제 하드웨어와 동기화할 기회를 준다.
 * struct pci_bridge_emul           : 위 모두를 묶은 상태.
 * PCI_BRIDGE_EMUL_NO_PREFETCHABLE_BAR / _NO_IO_FORWARD /
 * _NO_PREFMEM_FORWARD              : 지원하지 않는 기능을 읽기 전용 0 으로
 *                                    고정하라는 플래그.
 * pci_bridge_emul_init() / _cleanup() / _conf_read() / _conf_write()
 *                                  : 구현부(.c)의 함수 선언.
 */

#ifndef __PCI_BRIDGE_EMUL_H__
#define __PCI_BRIDGE_EMUL_H__

#include <linux/kernel.h>

/* PCI configuration space of a PCI-to-PCI bridge. */
struct pci_bridge_emul_conf {
	__le16 vendor;
	__le16 device;
	__le16 command;
	__le16 status;
	__le32 class_revision;
	u8 cache_line_size;
	u8 latency_timer;
	u8 header_type;
	u8 bist;
	__le32 bar[2];
	u8 primary_bus;
	u8 secondary_bus;
	u8 subordinate_bus;
	u8 secondary_latency_timer;
	u8 iobase;
	u8 iolimit;
	__le16 secondary_status;
	__le16 membase;
	__le16 memlimit;
	__le16 pref_mem_base;
	__le16 pref_mem_limit;
	__le32 prefbaseupper;
	__le32 preflimitupper;
	__le16 iobaseupper;
	__le16 iolimitupper;
	u8 capabilities_pointer;
	u8 reserve[3];
	__le32 romaddr;
	u8 intline;
	u8 intpin;
	__le16 bridgectrl;
};

/* PCI configuration space of the PCIe capabilities */
struct pci_bridge_emul_pcie_conf {
	u8 cap_id;
	u8 next;
	__le16 cap;
	__le32 devcap;
	__le16 devctl;
	__le16 devsta;
	__le32 lnkcap;
	__le16 lnkctl;
	__le16 lnksta;
	__le32 slotcap;
	__le16 slotctl;
	__le16 slotsta;
	__le16 rootctl;
	__le16 rootcap;
	__le32 rootsta;
	__le32 devcap2;
	__le16 devctl2;
	__le16 devsta2;
	__le32 lnkcap2;
	__le16 lnkctl2;
	__le16 lnksta2;
	__le32 slotcap2;
	__le16 slotctl2;
	__le16 slotsta2;
};

struct pci_bridge_emul;

typedef enum { PCI_BRIDGE_EMUL_HANDLED,
	       PCI_BRIDGE_EMUL_NOT_HANDLED } pci_bridge_emul_read_status_t;

struct pci_bridge_emul_ops {
	/*
	 * Called when reading from the regular PCI bridge
	 * configuration space. Return PCI_BRIDGE_EMUL_HANDLED when the
	 * operation has handled the read operation and filled in the
	 * *value, or PCI_BRIDGE_EMUL_NOT_HANDLED when the read should
	 * be emulated by the common code by reading from the
	 * in-memory copy of the configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_base)(struct pci_bridge_emul *bridge,
						   int reg, u32 *value);

	/*
	 * Same as ->read_base(), except it is for reading from the
	 * PCIe capability configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_pcie)(struct pci_bridge_emul *bridge,
						   int reg, u32 *value);

	/*
	 * Same as ->read_base(), except it is for reading from the
	 * PCIe extended capability configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_ext)(struct pci_bridge_emul *bridge,
						  int reg, u32 *value);

	/*
	 * Called when writing to the regular PCI bridge configuration
	 * space. old is the current value, new is the new value being
	 * written, and mask indicates which parts of the value are
	 * being changed.
	 */
	void (*write_base)(struct pci_bridge_emul *bridge, int reg,
			   u32 old, u32 new, u32 mask);

	/*
	 * Same as ->write_base(), except it is for writing from the
	 * PCIe capability configuration space.
	 */
	void (*write_pcie)(struct pci_bridge_emul *bridge, int reg,
			   u32 old, u32 new, u32 mask);

	/*
	 * Same as ->write_base(), except it is for writing from the
	 * PCIe extended capability configuration space.
	 */
	void (*write_ext)(struct pci_bridge_emul *bridge, int reg,
			  u32 old, u32 new, u32 mask);
};

struct pci_bridge_reg_behavior;

struct pci_bridge_emul {
	struct pci_bridge_emul_conf conf;
	struct pci_bridge_emul_pcie_conf pcie_conf;
	const struct pci_bridge_emul_ops *ops;
	struct pci_bridge_reg_behavior *pci_regs_behavior;
	struct pci_bridge_reg_behavior *pcie_cap_regs_behavior;
	void *data;
	u8 pcie_start;
	u8 ssid_start;
	bool has_pcie;
	u16 subsystem_vendor_id;
	u16 subsystem_id;
};

enum {
	/*
	 * PCI bridge does not support forwarding of prefetchable memory
	 * requests between primary and secondary buses.
	 */
	PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD = BIT(0),

	/*
	 * PCI bridge does not support forwarding of IO requests between
	 * primary and secondary buses.
	 */
	PCI_BRIDGE_EMUL_NO_IO_FORWARD = BIT(1),
};

int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags);
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge);

int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value);
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value);

#endif /* __PCI_BRIDGE_EMUL_H__ */
