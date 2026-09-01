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
	/* [한국어] Device ID(오프셋 0x02). __le16/__le32 로 선언한 이유: config space 는 규격상 리틀엔디안이고,
	 * 이 구조체가 그 바이트 배치를 그대로 흉내 내기 때문에 빅엔디안 CPU 에서도
	 * 같은 결과가 나와야 한다. 접근할 때마다 le16_to_cpu 계열로 변환한다. */
	__le16 device;
	/* [한국어] Command 레지스터(0x04). 메모리·I/O 디코딩과 버스 마스터 활성화 비트를 담는다. */
	__le16 command;
	/* [한국어] Status 레지스터(0x06). capability 목록 존재 여부 비트가 여기 있어,
	 * has_pcie 가 참이면 이 비트가 서고 capabilities_pointer 가 유효해진다. */
	__le16 status;
	/* [한국어] Class Code 3바이트와 Revision ID 1바이트를 합친 워드(0x08).
	 * 브리지 흉내를 내려면 상위 3바이트가 0x060400 이어야 한다. */
	__le32 class_revision;
	/* [한국어] Cache Line Size(0x0c). */
	u8 cache_line_size;
	/* [한국어] Latency Timer(0x0d). */
	u8 latency_timer;
	/* [한국어] Header Type(0x0e). 브리지는 0x01 이며, 그 값이 아래 필드 배치를 결정한다. */
	u8 header_type;
	/* [한국어] BIST(0x0f). 내장 자체 검사 레지스터로, 흉내 브리지는 지원하지 않는다. */
	u8 bist;
	/* [한국어] BAR 0~1(0x10, 0x14). 브리지 헤더는 BAR 가 둘뿐이다 —
	 * 일반 장치 헤더의 여섯 개와 다른 점이며, 나머지 자리를 아래 버스 번호와
	 * 창 레지스터가 차지한다. */
	__le32 bar[2];
	/* [한국어] Primary Bus Number(0x18). 이 브리지의 위쪽 버스 번호다. */
	u8 primary_bus;
	/* [한국어] Secondary Bus Number(0x19). 아래쪽 버스 번호. PCI 코어가 열거 중
	 * 여기에 값을 쓰면 그 아래를 스캔하기 시작한다. */
	u8 secondary_bus;
	/* [한국어] Subordinate Bus Number(0x1a). 이 브리지 아래에 존재하는 가장 큰 버스 번호. */
	u8 subordinate_bus;
	/* [한국어] Secondary Latency Timer(0x1b). */
	u8 secondary_latency_timer;
	/* [한국어] I/O Base 상위 니블(0x1c). I/O 창의 시작 주소 [15:12] 를 담고,
	 * 하위 니블은 32비트 주소 지원 여부를 나타낸다. */
	u8 iobase;
	/* [한국어] I/O Limit(0x1d). 창의 끝을 같은 방식으로 담는다. */
	u8 iolimit;
	/* [한국어] Secondary Status(0x1e). 아래쪽 버스에서 발생한 오류를 보고한다. */
	__le16 secondary_status;
	/* [한국어] Memory Base(0x20). 메모리 창의 시작 [31:20]. */
	__le16 membase;
	/* [한국어] Memory Limit(0x22). 그 끝. */
	__le16 memlimit;
	/* [한국어] Prefetchable Memory Base(0x24). 프리페치 가능 메모리 창의 시작.
	 * 일반 메모리와 별도의 창을 갖는 것이 PCI 브리지 규격이다. */
	__le16 pref_mem_base;
	/* [한국어] Prefetchable Memory Limit(0x26). */
	__le16 pref_mem_limit;
	/* [한국어] Prefetchable Base Upper 32 Bits(0x28). 64비트 프리페치 창의 상위 절반. */
	__le32 prefbaseupper;
	/* [한국어] Prefetchable Limit Upper 32 Bits(0x2c). */
	__le32 preflimitupper;
	/* [한국어] I/O Base Upper 16 Bits(0x30). 32비트 I/O 주소를 쓸 때의 상위 절반. */
	__le16 iobaseupper;
	/* [한국어] I/O Limit Upper 16 Bits(0x32). */
	__le16 iolimitupper;
	/* [한국어] Capabilities Pointer(0x34). capability 연결 리스트의 첫 항목 오프셋이며,
	 * pci_bridge_emul_init() 이 has_pcie 여부에 따라 pcie_start 를 여기에 넣는다. */
	u8 capabilities_pointer;
	/* [한국어] 0x35~0x37 예약 영역. 규격이 정의하지 않은 자리라 0 으로 읽힌다. */
	u8 reserve[3];
	/* [한국어] Expansion ROM Base Address(0x38). 흉내 브리지에는 ROM 이 없다. */
	__le32 romaddr;
	/* [한국어] Interrupt Line(0x3c). */
	u8 intline;
	/* [한국어] Interrupt Pin(0x3d). */
	u8 intpin;
	/* [한국어] Bridge Control(0x3e). 세컨더리 버스 리셋 비트가 여기 있어,
	 * PCI 코어가 하위 버스를 리셋할 때 이 레지스터에 쓴다. */
	__le16 bridgectrl;
};

/* PCI configuration space of the PCIe capabilities */
struct pci_bridge_emul_pcie_conf {
	/* [한국어] Capability ID(오프셋 +0x00). PCIe capability 이므로 0x10 이다. */
	u8 cap_id;
	/* [한국어] Next Capability Pointer(+0x01). 다음 capability 의 오프셋이며,
	 * 0 이면 목록의 끝이다. ssid_start 가 설정되면 그쪽을 가리키게 된다. */
	u8 next;
	/* [한국어] PCI Express Capabilities Register(+0x02). 포트 종류(루트 포트/다운스트림 등)를 담는다. */
	__le16 cap;
	/* [한국어] Device Capabilities(+0x04). */
	__le32 devcap;
	/* [한국어] Device Control(+0x08). */
	__le16 devctl;
	/* [한국어] Device Status(+0x0a). */
	__le16 devsta;
	/* [한국어] Link Capabilities(+0x0c). 최대 링크 속도와 폭을 광고한다. */
	__le32 lnkcap;
	/* [한국어] Link Control(+0x10). ASPM 설정과 링크 재훈련 비트가 여기 있다. */
	__le16 lnkctl;
	/* [한국어] Link Status(+0x12). 현재 협상된 속도와 폭, 링크 훈련 상태.
	 * 실제 컨트롤러 드라이버가 read_pcie 콜백에서 이 값을 하드웨어에서 가져와 채운다. */
	__le16 lnksta;
	/* [한국어] Slot Capabilities(+0x14). 핫플러그 지원 여부와 슬롯 번호. */
	__le32 slotcap;
	/* [한국어] Slot Control(+0x18). 핫플러그 인터럽트 허용과 LED 제어. */
	__le16 slotctl;
	/* [한국어] Slot Status(+0x1a). 카드 존재 여부와 핫플러그 이벤트. */
	__le16 slotsta;
	/* [한국어] Root Control(+0x1c). PME 와 오류 보고 인터럽트 설정. */
	__le16 rootctl;
	/* [한국어] Root Capabilities(+0x1e). */
	__le16 rootcap;
	/* [한국어] Root Status(+0x20). PME 요청자 정보. */
	__le32 rootsta;
	/* [한국어] Device Capabilities 2(+0x24). */
	__le32 devcap2;
	/* [한국어] Device Control 2(+0x28). 완료 타임아웃 설정 등. */
	__le16 devctl2;
	/* [한국어] Device Status 2(+0x2a). 규격상 예약이지만 배치를 맞추기 위해 자리를 둔다. */
	__le16 devsta2;
	/* [한국어] Link Capabilities 2(+0x2c). 지원하는 링크 속도 목록. */
	__le32 lnkcap2;
	/* [한국어] Link Control 2(+0x30). 목표 링크 속도 설정. */
	__le16 lnkctl2;
	/* [한국어] Link Status 2(+0x32). */
	__le16 lnksta2;
	/* [한국어] Slot Capabilities 2(+0x34). */
	__le32 slotcap2;
	/* [한국어] Slot Control 2(+0x38). */
	__le16 slotctl2;
	/* [한국어] Slot Status 2(+0x3a). 여기까지가 PCIe capability v2 의 전체 배치다. */
	__le16 slotsta2;
};

/* [한국어] 전방 선언. 아래 ops 의 콜백들이 struct pci_bridge_emul 포인터를 받는데
 * 그 정의는 뒤에 오므로 이름만 먼저 알려 준다. */
struct pci_bridge_emul;

/* [한국어] 읽기 콜백의 반환 타입. HANDLED 면 드라이버가 값을 채웠다는 뜻이고,
 * NOT_HANDLED 면 흉내 계층이 자기 conf 배열에서 값을 가져오라는 뜻이다.
 * 이 두 갈래가 이 프레임워크의 핵심 — 대부분의 레지스터는 소프트웨어가
 * 기억한 값으로 충분하고, 링크 상태처럼 실제 하드웨어를 봐야 하는 것만
 * 드라이버가 가로챈다. */
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

/* [한국어] 전방 선언. 레지스터별 동작(읽기 전용/쓰기 가능/RW1C 등)을 나타내는
 * 구조체이며, 정의는 pci-bridge-emul.c 에 있다. */
struct pci_bridge_reg_behavior;

/* [한국어] 흉내 브리지 하나의 전체 상태. 컨트롤러 드라이버가 자기 구조체 안에
 * 이것을 두고 pci_bridge_emul_init() 으로 초기화한다. */
struct pci_bridge_emul {
	/* [한국어] 흉내 낼 PCI 브리지 헤더의 실제 저장소. config 읽기가 여기서 값을 가져오고,
	 * 쓰기가 여기에 값을 남긴다.
	 * 설정자: pci_bridge_emul_init() 이 기본값을 채우고, 이후 config 쓰기가 갱신한다.
	 * 읽는 자: pci_bridge_emul_conf_read() 와 드라이버의 write 콜백.
	 * 값 범위: 위 struct pci_bridge_emul_conf 의 배치 그대로.
	 * 동기화: 없다. config 접근이 PCI 코어의 pci_lock 으로 직렬화된다는 전제다. */
	struct pci_bridge_emul_conf conf;
	struct pci_bridge_emul_pcie_conf pcie_conf;
	const struct pci_bridge_emul_ops *ops;
	/* [한국어] PCI 브리지 헤더 각 레지스터의 동작 규칙 배열(읽기 전용 / 쓰기 가능 /
	 * RW1C 등). 정의는 pci-bridge-emul.c 에 있다.
	 * 설정자: pci_bridge_emul_init() 이 기본 규칙 표를 복사해 준다 —
	 *   복사본이라 드라이버가 자기 하드웨어에 맞게 일부를 수정할 수 있다.
	 * 읽는 자: conf_write() 가 쓰기 가능 비트만 골라 반영할 때.
	 * 값 범위: 유효 포인터. cleanup() 이 해제한다.
	 * 동기화: 초기화 후 읽기 전용이 원칙이다. */
	struct pci_bridge_reg_behavior *pci_regs_behavior;
	/* [한국어] PCIe capability 쪽의 같은 규칙 배열.
	 * 설정자·읽는 자·동기화는 위와 같다. has_pcie 가 거짓이면 쓰이지 않는다. */
	struct pci_bridge_reg_behavior *pcie_cap_regs_behavior;
	/* [한국어] 컨트롤러 드라이버가 자기 문맥을 담아 두는 불투명 포인터.
	 * 설정자: 드라이버가 init 전에 직접 대입한다.
	 * 읽는 자: 드라이버의 read/write 콜백이 자기 하드웨어 상태에 닿을 때.
	 * 값 범위: 드라이버가 정한 값.
	 * 동기화: 드라이버 소관. */
	void *data;
	/* [한국어] config space 안에서 PCIe capability 가 시작되는 오프셋.
	 * 설정자: 드라이버가 init 전에 정한다. 0 이면 init 이 기본값을 쓴다.
	 * 읽는 자: conf_read/write 가 접근 오프셋이 어느 영역인지 가를 때,
	 *   그리고 capabilities_pointer 에 넣을 값으로.
	 * 값 범위: 브리지 헤더 크기(0x40) 이상.
	 * 동기화: 초기화 후 읽기 전용. */
	u8 pcie_start;
	/* [한국어] Subsystem ID capability 가 시작되는 오프셋.
	 * 설정자: 드라이버가 init 전에 정한다. 0 이면 그 capability 를 노출하지 않는다.
	 * 읽는 자: conf_read 가 그 영역 접근을 처리할 때.
	 * 값 범위: 0(없음) 또는 유효 오프셋.
	 * 동기화: 초기화 후 읽기 전용. */
	u8 ssid_start;
	/* [한국어] 이 흉내 브리지가 PCIe capability 를 노출할지 여부.
	 * 설정자: 드라이버가 init 전에 정한다.
	 * 읽는 자: init 이 capabilities_pointer 와 Status 의 capability 비트를
	 *   세울지 결정하고, conf_read/write 가 PCIe 영역 접근을 처리할지 가른다.
	 * 값 범위: true/false.
	 * 동기화: 초기화 후 읽기 전용. */
	bool has_pcie;
	/* [한국어] Subsystem Vendor ID 로 보고할 값.
	 * 설정자: 드라이버가 init 전에 정한다.
	 * 읽는 자: ssid capability 영역의 읽기 처리.
	 * 값 범위: 16비트 값. ssid_start 가 0 이면 쓰이지 않는다.
	 * 동기화: 초기화 후 읽기 전용. */
	u16 subsystem_vendor_id;
	/* [한국어] Subsystem ID 로 보고할 값. 위와 짝이다. */
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

/* [한국어] 흉내 브리지를 초기화한다 — 헤더 기본값을 채우고 동작 규칙 표를 복사한다.
 * flags 로 프리페치 메모리나 I/O 전달을 지원하지 않는다고 알릴 수 있다.
 * 정의는 pci-bridge-emul.c. */
int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags);
/* [한국어] init 이 할당한 규칙 표를 해제한다. 드라이버의 remove 경로에서 부른다. */
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge);

/* [한국어] 흉내 브리지의 config 공간을 읽는다. 오프셋에 따라 헤더·PCIe·ssid 영역을
 * 가르고, 드라이버 콜백이 HANDLED 를 돌려주지 않으면 저장된 값을 쓴다. */
int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value);
/* [한국어] 흉내 브리지의 config 공간에 쓴다. 동작 규칙 표를 보고 쓰기 가능한 비트만
 * 반영한 뒤, 드라이버의 write 콜백에 이전 값·새 값·마스크를 함께 넘긴다 —
 * 그 세 값이 있어야 드라이버가 "무엇이 어떻게 바뀌었는지"를 알 수 있다. */
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value);

#endif /* __PCI_BRIDGE_EMUL_H__ */
