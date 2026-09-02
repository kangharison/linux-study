// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Microsoft Corporation.
 *
 * Author:
 *   Jake Oshins <jakeo@microsoft.com>
 *
 * This driver acts as a paravirtual front-end for PCI Express root buses.
 * When a PCI Express function (either an entire device or an SR-IOV
 * Virtual Function) is being passed through to the VM, this driver exposes
 * a new bus to the guest VM.  This is modeled as a root PCI bus because
 * no bridges are being exposed to the VM.  In fact, with a "Generation 2"
 * VM within Hyper-V, there may seem to be no PCI bus at all in the VM
 * until a device as been exposed using this driver.
 *
 * Each root PCI bus has its own PCI domain, which is called "Segment" in
 * the PCI Firmware Specifications.  Thus while each device passed through
 * to the VM using this front-end will appear at "device 0", the domain will
 * be unique.  Typically, each bus will have one PCI function on it, though
 * this driver does support more than one.
 *
 * In order to map the interrupts from the device through to the guest VM,
 * this driver also implements an IRQ Domain, which handles interrupts (either
 * MSI or MSI-X) associated with the functions on the bus.  As interrupts are
 * set up, torn down, or reaffined, this driver communicates with the
 * underlying hypervisor to adjust the mappings in the I/O MMU so that each
 * interrupt will be delivered to the correct virtual processor at the right
 * vector.  This driver does not support level-triggered (line-based)
 * interrupts, and will report that the Interrupt Line register in the
 * function's configuration space is zero.
 *
 * The rest of this driver mostly maps PCI concepts onto underlying Hyper-V
 * facilities.  For instance, the configuration space of a function exposed
 * by Hyper-V is mapped into a single page of memory space, and the
 * read and write handlers for config space must be aware of this mechanism.
 * Similarly, device setup and teardown involves messages sent to and from
 * the PCI back-end driver in Hyper-V.
 */

/*
 * [한국어 설명] Hyper-V 게스트에서 VMBus 패킷으로 PCI 를 흉내 내는 드라이버
 * (pci-hyperv.c)
 *
 * === 파일의 역할 ===
 * 이 파일에는 **실제 PCI 하드웨어 레지스터를 다루는 코드가 거의 없다.**
 * 게스트 VM 안에서 도는 반가상화(paravirtual) 프론트엔드이고,
 * 상대는 하드웨어가 아니라 **호스트(Hyper-V 부모 파티션)의 PCI 백엔드
 * 드라이버** 다. 둘은 VMBus 채널 위에서 패킷을 주고받는다.
 *
 * 그래서 이 파일의 뼈대는 레지스터 배치가 아니라 **패킷 프로토콜과 그
 * 상태 기계** 다. 크게 넷으로 나뉜다.
 *
 *   1. **프로토콜 버전 협상.** 게스트가 자기가 아는 가장 높은 판부터
 *      차례로 제안하고, 호스트가 받아들일 때까지 내려간다.
 *      pci_protocol_versions[] 배열이 그 순서이며,
 *      hv_pci_protocol_negotiation() 이 그 협상을 수행한다.
 *      **한 번 정해진 판이 이후 거의 모든 메시지의 모양을 좌우한다** --
 *      hv_msi_desc 가 세 판(v1/v2/v3)으로 갈리는 것이 그 예다.
 *
 *   2. **요청-응답 대응.** 게스트가 보낸 패킷과 호스트가 돌려준 응답을
 *      짝지어야 한다. 그 방법이 struct pci_packet 의 completion_func 와
 *      compl_ctxt 다 -- 요청을 보낼 때 그 포인터를 VMBus 에 넘겨 두면,
 *      응답이 올 때 hv_pci_onchannelcallback() 이 그것을 되찾아 부른다.
 *      대부분의 요청은 struct hv_pci_compl 의 completion 으로 잠들었다가
 *      wait_for_response() 에서 깨어난다.
 *
 *   3. **호스트가 먼저 보내는 알림.** 응답이 아니라 호스트가 스스로
 *      보내는 메시지가 넷 있다 -- 장치 목록이 바뀌었다(BUS_RELATIONS),
 *      장치를 뽑겠다(EJECT), 설정 블록이 무효가 됐다(INVALIDATE_BLOCK),
 *      그리고 각각의 2판. 이것들은 짝지을 요청이 없으므로
 *      hv_pci_onchannelcallback() 이 메시지 종류를 보고 갈래를 탄다.
 *      **모두 워크큐로 미뤄 처리한다** -- 콜백이 채널 문맥에서 돌기 때문이다.
 *
 *   4. **PCI 개념을 Hyper-V 시설로 옮기기.** 설정공간은 MMIO 한 페이지로
 *      매핑되고(_hv_pcifront_read_config), MSI 는 하이퍼콜과 패킷으로
 *      호스트에 등록되며(hv_compose_msi_msg), BAR 는 호스트에게 물어본
 *      값으로 게스트가 직접 배정한다(prepopulate_bars).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCI 컨트롤러 드라이버이면서 동시에 VMBus 장치 드라이버다.
 * 두 세계가 만나는 자리이며, 그래서 등록도 두 번 한다.
 *
 *   module_init -> init_hv_pci_drv()
 *     -> hv_pci_irqchip_init()      [아키텍처별 IRQ 도메인 준비]
 *     -> vmbus_driver_register()    [VMBus 쪽 등록]
 *        -> (호스트가 PCI 장치를 넘겨주면) hv_pci_probe()
 *           -> vmbus_open()                    [채널 열기]
 *           -> hv_pci_protocol_negotiation()   [버전 협상]
 *           -> hv_allocate_config_window()     [설정공간 창 잡기]
 *           -> hv_pci_enter_d0()               [버스를 D0 로]
 *           -> hv_pci_query_relations()        [장치 목록 요청]
 *           -> hv_pci_allocate_bridge_windows()[MMIO 창 잡기]
 *           -> hv_send_resources_allocated()   [BAR 배정 결과 통보]
 *           -> prepopulate_bars()              [BAR 값 채우기]
 *           -> create_root_hv_pci_bus()        [리눅스 PCI 버스 등록]
 *
 * 동작 중에는 두 방향으로 흐른다.
 *   게스트 -> 호스트 : 설정공간 접근, MSI 등록/해제, 전원 상태 변경.
 *   호스트 -> 게스트 : 장치 추가/제거 알림, eject 요청.
 *     -> hv_pci_onchannelcallback() 이 받아 워크큐로 미룬다
 *        -> pci_devices_present_work() 또는 hv_eject_device_work()
 *
 * 실행 컨텍스트: hv_pci_onchannelcallback() 은 VMBus 채널 콜백 문맥에서
 * 돈다 -- **그 문맥이 인터럽트인지 tasklet 인지는 drivers/hv 가 이 트리에
 * 없어 확인할 수 없다.** 다만 이 파일이 그 안에서 잠들 수 있는 일을 모두
 * 워크큐로 미루는 것으로 보아 잠들 수 없는 문맥으로 다루고 있다.
 * 나머지는 프로세스 문맥이며, 여러 곳에서 completion 으로 잠든다.
 *
 * === 타 모듈과의 연결 ===
 * **위쪽**: 리눅스 PCI 코어(pci_scan_child_bus, pci_bus_add_devices)와
 * IRQ 도메인 계층. 이 드라이버가 만든 버스에 보통의 PCI 드라이버가 붙는다.
 *
 * **아래쪽**: VMBus -- vmbus_open, vmbus_sendpacket, vmbus_recvpacket_raw,
 * vmbus_next_request_id 등. **drivers/hv 와 include/linux/hyperv.h 가 이
 * 스파스 체크아웃에 없어** 그 함수들의 내부는 확인할 수 없다.
 * 이 파일의 주석은 호출 자리의 쓰임새로만 그것들을 설명한다.
 * 하이퍼바이저 하이퍼콜(hv_do_hypercall, HVCALL_RETARGET_INTERRUPT 등)도
 * asm/mshyperv.h 에 있고 이 트리에 없다.
 *
 * **옆쪽**: 아키텍처마다 IRQ 처리가 크게 갈린다. x86 은 x86_vector_domain 을
 * 부모로 쓰고 하이퍼콜로 재타게팅하며, ARM64 는 GIC 위에 자기 IRQ 도메인을
 * 따로 만든다. 파일 앞쪽의 긴 #ifdef 블록이 그 차이다.
 *
 * **공유 상태**: struct hv_pcibus_device 하나가 버스 하나의 전부다.
 * 그 안의 children 목록(hv_pci_dev), dr_list(들어온 장치 목록 변경),
 * 그리고 그것들을 지키는 state_lock 뮤텍스와 두 스핀락이 핵심이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - hv_pci_probe() : 이 파일에서 가장 긴 함수. 채널 열기부터 리눅스 PCI
 *   버스 등록까지 열 단계가 넘는 절차를 순서대로 밟고, 실패하면 그
 *   역순으로 되돌린다.
 * - hv_pci_onchannelcallback() : 호스트가 보낸 패킷을 받는 유일한 입구.
 *   응답이면 요청 때 넘겨 둔 completion_func 를 부르고,
 *   호스트가 먼저 보낸 알림이면 종류별로 워크큐에 넘긴다.
 * - hv_pci_protocol_negotiation() : 버전을 높은 것부터 제안해 내려간다.
 * - hv_compose_msi_msg() : MSI 를 호스트에 등록해 실제 주소·데이터를 받는다.
 *   프로토콜 판에 따라 v1/v2/v3 세 가지 요청 형식을 쓴다.
 * - _hv_pcifront_read_config() / _write_config() : 설정공간 접근.
 *   MMIO 한 페이지에 인덱스와 데이터를 겹쳐 쓰는 방식이라 락이 필요하다.
 * - survey_child_resources() / prepopulate_bars() : 호스트에게 물어본 BAR
 *   크기로 게스트가 직접 주소를 배정한다.
 * - pci_devices_present_work() : 장치 목록 변경을 실제로 반영한다.
 *   새로 온 것을 만들고, 사라진 것을 표시하고, 리눅스 코어에 알린다.
 * - hv_eject_device_work() : 호스트의 뽑기 요청을 처리하고 완료를 통보한다.
 * - struct hv_pcibus_device : 버스 하나의 전부.
 * - struct hv_pci_dev : 장치 하나. refcount 로 수명을 관리한다.
 * - struct pci_packet : 요청-응답을 잇는 열쇠. completion_func 와 문맥.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 것 ===
 * **패킷 구조체가 프로토콜 판마다 갈린다.** hv_msi_desc / _desc2 / _desc3,
 * pci_function_description / _description2, pci_resources_assigned / 2 / 3.
 * 뒤 판이 앞 판의 확장이며, hbus->protocol_version 이 어느 것을 쓸지 정한다.
 *
 * **union win_slot_encoding 이 PCI 의 devfn 과 다르다.** Windows 쪽 표현이며
 * devfn_to_wslot() 과 wslot_to_devfn() 이 그 사이를 옮긴다.
 *
 * **모든 요청 구조체가 __packed 다.** 호스트와 바이트 배치를 맞춰야 하므로
 * 컴파일러가 정렬을 위해 빈틈을 넣어서는 안 된다.
 *
 * **CONFIG_X86 과 CONFIG_ARM64 갈래가 파일 앞쪽 400줄을 차지한다.**
 * 같은 이름의 함수가 두 벌 정의되어 있으니(hv_pci_irqchip_init,
 * hv_pci_get_root_domain, hv_msi_get_int_vector, hv_arch_irq_unmask),
 * 읽을 때 어느 갈래인지 먼저 확인해야 한다.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/msi.h>
#include <linux/hyperv.h>
#include <linux/refcount.h>
#include <linux/irqdomain.h>
#include <linux/acpi.h>
#include <linux/sizes.h>
#include <linux/of_irq.h>
#include <asm/mshyperv.h>

/*
 * Protocol versions. The low word is the minor version, the high word the
 * major version.
 */

#define PCI_MAKE_VERSION(major, minor) ((u32)(((major) << 16) | (minor)))
#define PCI_MAJOR_VERSION(version) ((u32)(version) >> 16)
#define PCI_MINOR_VERSION(version) ((u32)(version) & 0xff)

enum pci_protocol_version_t {
	PCI_PROTOCOL_VERSION_1_1 = PCI_MAKE_VERSION(1, 1),	/* Win10 */
	PCI_PROTOCOL_VERSION_1_2 = PCI_MAKE_VERSION(1, 2),	/* RS1 */
	PCI_PROTOCOL_VERSION_1_3 = PCI_MAKE_VERSION(1, 3),	/* Vibranium */
	PCI_PROTOCOL_VERSION_1_4 = PCI_MAKE_VERSION(1, 4),	/* WS2022 */
};

#define CPU_AFFINITY_ALL	-1ULL

/*
 * Supported protocol versions in the order of probing - highest go
 * first.
 */
static enum pci_protocol_version_t pci_protocol_versions[] = {
	PCI_PROTOCOL_VERSION_1_4,
	PCI_PROTOCOL_VERSION_1_3,
	PCI_PROTOCOL_VERSION_1_2,
	PCI_PROTOCOL_VERSION_1_1,
};

#define PCI_CONFIG_MMIO_LENGTH	0x2000
#define CFG_PAGE_OFFSET 0x1000
#define CFG_PAGE_SIZE (PCI_CONFIG_MMIO_LENGTH - CFG_PAGE_OFFSET)

#define MAX_SUPPORTED_MSI_MESSAGES 0x400

#define STATUS_REVISION_MISMATCH 0xC0000059

/* space for 32bit serial number as string */
#define SLOT_NAME_SIZE 11

/*
 * Size of requestor for VMbus; the value is based on the observation
 * that having more than one request outstanding is 'rare', and so 64
 * should be generous in ensuring that we don't ever run out.
 */
#define HV_PCI_RQSTOR_SIZE 64

/*
 * Message Types
 */

enum pci_message_type {
	/*
	 * Version 1.1
	 */
	PCI_MESSAGE_BASE                = 0x42490000,
	PCI_BUS_RELATIONS               = PCI_MESSAGE_BASE + 0,
	PCI_QUERY_BUS_RELATIONS         = PCI_MESSAGE_BASE + 1,
	PCI_POWER_STATE_CHANGE          = PCI_MESSAGE_BASE + 4,
	PCI_QUERY_RESOURCE_REQUIREMENTS = PCI_MESSAGE_BASE + 5,
	PCI_QUERY_RESOURCE_RESOURCES    = PCI_MESSAGE_BASE + 6,
	PCI_BUS_D0ENTRY                 = PCI_MESSAGE_BASE + 7,
	PCI_BUS_D0EXIT                  = PCI_MESSAGE_BASE + 8,
	PCI_READ_BLOCK                  = PCI_MESSAGE_BASE + 9,
	PCI_WRITE_BLOCK                 = PCI_MESSAGE_BASE + 0xA,
	PCI_EJECT                       = PCI_MESSAGE_BASE + 0xB,
	PCI_QUERY_STOP                  = PCI_MESSAGE_BASE + 0xC,
	PCI_REENABLE                    = PCI_MESSAGE_BASE + 0xD,
	PCI_QUERY_STOP_FAILED           = PCI_MESSAGE_BASE + 0xE,
	PCI_EJECTION_COMPLETE           = PCI_MESSAGE_BASE + 0xF,
	PCI_RESOURCES_ASSIGNED          = PCI_MESSAGE_BASE + 0x10,
	PCI_RESOURCES_RELEASED          = PCI_MESSAGE_BASE + 0x11,
	PCI_INVALIDATE_BLOCK            = PCI_MESSAGE_BASE + 0x12,
	PCI_QUERY_PROTOCOL_VERSION      = PCI_MESSAGE_BASE + 0x13,
	PCI_CREATE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x14,
	PCI_DELETE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x15,
	PCI_RESOURCES_ASSIGNED2		= PCI_MESSAGE_BASE + 0x16,
	PCI_CREATE_INTERRUPT_MESSAGE2	= PCI_MESSAGE_BASE + 0x17,
	PCI_DELETE_INTERRUPT_MESSAGE2	= PCI_MESSAGE_BASE + 0x18, /* unused */
	PCI_BUS_RELATIONS2		= PCI_MESSAGE_BASE + 0x19,
	PCI_RESOURCES_ASSIGNED3         = PCI_MESSAGE_BASE + 0x1A,
	PCI_CREATE_INTERRUPT_MESSAGE3   = PCI_MESSAGE_BASE + 0x1B,
	PCI_MESSAGE_MAXIMUM
};

/*
 * Structures defining the virtual PCI Express protocol.
 */

union pci_version {
	struct {
		u16 minor_version;
		u16 major_version;
	} parts;
	u32 version;
} __packed;

/*
 * Function numbers are 8-bits wide on Express, as interpreted through ARI,
 * which is all this driver does.  This representation is the one used in
 * Windows, which is what is expected when sending this back and forth with
 * the Hyper-V parent partition.
 */
union win_slot_encoding {
	struct {
		u32	dev:5;
		u32	func:3;
		u32	reserved:24;
	} bits;
	u32 slot;
} __packed;

/*
 * Pretty much as defined in the PCI Specifications.
 */
struct pci_function_description {
	u16	v_id;	/* vendor ID */
	u16	d_id;	/* device ID */
	u8	rev;
	u8	prog_intf;
	u8	subclass;
	u8	base_class;
	u32	subsystem_id;
	union win_slot_encoding win_slot;
	u32	ser;	/* serial number */
} __packed;

enum pci_device_description_flags {
	HV_PCI_DEVICE_FLAG_NONE			= 0x0,
	HV_PCI_DEVICE_FLAG_NUMA_AFFINITY	= 0x1,
};

struct pci_function_description2 {
	u16	v_id;	/* vendor ID */
	u16	d_id;	/* device ID */
	u8	rev;
	u8	prog_intf;
	u8	subclass;
	u8	base_class;
	u32	subsystem_id;
	union	win_slot_encoding win_slot;
	u32	ser;	/* serial number */
	u32	flags;
	u16	virtual_numa_node;
	u16	reserved;
} __packed;

/**
 * struct hv_msi_desc
 * @vector:		IDT entry
 * @delivery_mode:	As defined in Intel's Programmer's
 *			Reference Manual, Volume 3, Chapter 8.
 * @vector_count:	Number of contiguous entries in the
 *			Interrupt Descriptor Table that are
 *			occupied by this Message-Signaled
 *			Interrupt. For "MSI", as first defined
 *			in PCI 2.2, this can be between 1 and
 *			32. For "MSI-X," as first defined in PCI
 *			3.0, this must be 1, as each MSI-X table
 *			entry would have its own descriptor.
 * @reserved:		Empty space
 * @cpu_mask:		All the target virtual processors.
 */
struct hv_msi_desc {
	u8	vector;
	u8	delivery_mode;
	u16	vector_count;
	u32	reserved;
	u64	cpu_mask;
} __packed;

/**
 * struct hv_msi_desc2 - 1.2 version of hv_msi_desc
 * @vector:		IDT entry
 * @delivery_mode:	As defined in Intel's Programmer's
 *			Reference Manual, Volume 3, Chapter 8.
 * @vector_count:	Number of contiguous entries in the
 *			Interrupt Descriptor Table that are
 *			occupied by this Message-Signaled
 *			Interrupt. For "MSI", as first defined
 *			in PCI 2.2, this can be between 1 and
 *			32. For "MSI-X," as first defined in PCI
 *			3.0, this must be 1, as each MSI-X table
 *			entry would have its own descriptor.
 * @processor_count:	number of bits enabled in array.
 * @processor_array:	All the target virtual processors.
 */
struct hv_msi_desc2 {
	u8	vector;
	u8	delivery_mode;
	u16	vector_count;
	u16	processor_count;
	u16	processor_array[32];
} __packed;

/*
 * struct hv_msi_desc3 - 1.3 version of hv_msi_desc
 *	Everything is the same as in 'hv_msi_desc2' except that the size of the
 *	'vector' field is larger to support bigger vector values. For ex: LPI
 *	vectors on ARM.
 */
struct hv_msi_desc3 {
	u32	vector;
	u8	delivery_mode;
	u8	reserved;
	u16	vector_count;
	u16	processor_count;
	u16	processor_array[32];
} __packed;

/**
 * struct tran_int_desc
 * @reserved:		unused, padding
 * @vector_count:	same as in hv_msi_desc
 * @data:		This is the "data payload" value that is
 *			written by the device when it generates
 *			a message-signaled interrupt, either MSI
 *			or MSI-X.
 * @address:		This is the address to which the data
 *			payload is written on interrupt
 *			generation.
 */
struct tran_int_desc {
	u16	reserved;
	u16	vector_count;
	u32	data;
	u64	address;
} __packed;

/*
 * A generic message format for virtual PCI.
 * Specific message formats are defined later in the file.
 */

struct pci_message {
	u32 type;
} __packed;

struct pci_child_message {
	struct pci_message message_type;
	union win_slot_encoding wslot;
} __packed;

struct pci_incoming_message {
	struct vmpacket_descriptor hdr;
	struct pci_message message_type;
} __packed;

struct pci_response {
	struct vmpacket_descriptor hdr;
	s32 status;			/* negative values are failures */
} __packed;

struct pci_packet {
	void (*completion_func)(void *context, struct pci_response *resp,
				int resp_packet_size);
	void *compl_ctxt;
};

/*
 * Specific message types supporting the PCI protocol.
 */

/*
 * Version negotiation message. Sent from the guest to the host.
 * The guest is free to try different versions until the host
 * accepts the version.
 *
 * pci_version: The protocol version requested.
 * is_last_attempt: If TRUE, this is the last version guest will request.
 * reservedz: Reserved field, set to zero.
 */

struct pci_version_request {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 게스트가 시도하는 프로토콜 판본.
	 * 설정자: hv_pci_protocol_negotiation() 이 목록을 앞에서부터 하나씩 넣는다.
	 * 읽는 자: 호스트가 지원 여부를 판단해 응답한다.
	 * 값 범위: PCI_PROTOCOL_VERSION_ 계열 값.
	 * 동기화: 협상은 한 번에 하나만 진행된다. */
	u32 protocol_version;
/* [한국어] 판본 협상 요청 메시지. */
} __packed;

/*
 * Bus D0 Entry.  This is sent from the guest to the host when the virtual
 * bus (PCI Express port) is ready for action.
 */

struct pci_bus_d0_entry {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 예약 필드. 이 파일에서 값을 넣는 곳은 없다 — kzalloc 으로 0 이 된 채로 간다.
	 * 설정자: 없음.
	 * 읽는 자: 호스트(프로토콜상 무시).
	 * 값 범위: 0.
	 * 동기화: 해당 없음. */
	u32 reserved;
	/* [한국어] config 창의 시작 물리 주소.
	 * 설정자: hv_pci_enter_d0() 이 hbus->mem_config->start 를 넣는다.
	 * 읽는 자: 호스트가 이 주소부터의 접근을 config 요청으로 해석하기 시작한다.
	 * 값 범위: hv_allocate_config_window() 가 얻은 MMIO 창의 시작.
	 * 동기화: D0 진입은 state_lock 아래에서 한 번만 일어난다. */
	u64 mmio_base;
/* [한국어] D0 진입 요청 메시지. 위 상류 주석이 이 메시지의 의미를 밝힌다. */
} __packed;

struct pci_bus_relations {
	/* [한국어] 호스트가 먼저 보내는 알림의 공통 머리.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 그 안의 type 으로 갈래를 정한다.
	 * 값 범위: 프로토콜이 정한 알림 종류.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는 구조라, 그 버퍼를 소유한 콜백만 본다. */
	struct pci_incoming_message incoming;
	/* [한국어] 뒤따르는 배열의 항목 수.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 받은 바이트 수가 이 개수를 담기에
	 * 충분한지 먼저 확인한 뒤 hv_pci_devices_present() 에 넘긴다.
	 * 값 범위: 호스트가 정한 값이라 게스트는 신뢰하지 않고 크기 검사를 한다.
	 * 동기화: 수신 버퍼 위에서만 읽는다. */
	u32 device_count;
	/* [한국어] 장치 서술의 가변 길이 배열.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_devices_present() 가 하나씩 커널 쪽 서술로 옮긴다.
	 * 값 범위: 위 device_count 개.
	 * 동기화: 수신 버퍼 위에서만 읽는다. */
	struct pci_function_description func[];
/* [한국어] 장치 목록 알림(구판). 프로토콜 1.1 이하가 이 형식을 쓴다. */
} __packed;

struct pci_bus_relations2 {
	/* [한국어] 호스트가 먼저 보내는 알림의 공통 머리.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 그 안의 type 으로 갈래를 정한다.
	 * 값 범위: 프로토콜이 정한 알림 종류.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는 구조라, 그 버퍼를 소유한 콜백만 본다. */
	struct pci_incoming_message incoming;
	/* [한국어] 뒤따르는 배열의 항목 수. 구판과 같은 규약이다. */
	u32 device_count;
	/* [한국어] 장치 서술의 가변 길이 배열. **구판과 항목 타입이 다르다** —
	 * 확장판에는 가상 NUMA 노드 같은 필드가 더 들어 있다.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_devices_present2().
	 * 값 범위: 위 device_count 개.
	 * 동기화: 수신 버퍼 위에서만 읽는다. */
	struct pci_function_description2 func[];
/* [한국어] 장치 목록 알림(확장판). 프로토콜 1.2 이상이 이 형식을 쓴다. */
} __packed;

struct pci_q_res_req_response {
	/* [한국어] VMBus 패킷의 공통 머리.
	 * 설정자: 호스트(VMBus 계층).
	 * 읽는 자: 이 파일은 직접 읽지 않고, 뒤따르는 필드를 찾는 기준으로만 쓴다.
	 * 값 범위: VMBus 가 정의한 서술자.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는다. */
	struct vmpacket_descriptor hdr;
	/* [한국어] 요청 처리 결과(옆의 상류 주석대로 음수가 실패).
	 * 설정자: 호스트.
	 * 읽는 자: 자원 질의의 완료 콜백.
	 * 값 범위: 0 이상이 성공.
	 * 동기화: 완료 콜백 안에서만 읽는다. */
	s32 status;			/* negative values are failures */
	/* [한국어] 각 BAR 에 0xFFFFFFFF 를 쓰고 읽었을 때의 값.
	 * 설정자: 호스트가 실제 장치에서 얻은 값을 채워 준다.
	 * 읽는 자: 이 파일이 그 값을 hv_pci_dev::probed_bar 에 복사해 두었다가
	 * BAR 크기와 종류를 계산하는 데 쓴다.
	 * 값 범위: BAR 하나당 한 워드. 0 이면 그 BAR 이 없다는 뜻이다.
	 * 동기화: 완료 콜백에서 복사한 뒤로는 소유자만 읽는다. */
	u32 probed_bar[PCI_STD_NUM_BARS];
/* [한국어] 자원 질의 응답. 실제 하드웨어를 탐색할 수 없어 호스트가 대신 답해 준다. */
} __packed;

struct pci_set_power {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 요청하는 전원 상태(옆의 상류 주석대로 Windows 기준 값).
	 * 설정자: 전원 상태를 바꾸는 요청부.
	 * 읽는 자: 호스트.
	 * 값 범위: Windows 의 전원 상태 값 — 리눅스의 D0~D3 과 이름은 비슷하나
	 * 인코딩이 같은지는 이 트리에서 확인 못 함.
	 * 동기화: 패킷마다 독립이다. */
	u32 power_state;		/* In Windows terms */
	/* [한국어] 예약 필드. 값을 넣는 곳이 없다. */
	u32 reserved;
/* [한국어] 전원 상태 변경 요청 메시지. */
} __packed;

struct pci_set_power_response {
	/* [한국어] VMBus 패킷의 공통 머리.
	 * 설정자: 호스트(VMBus 계층).
	 * 읽는 자: 이 파일은 직접 읽지 않고, 뒤따르는 필드를 찾는 기준으로만 쓴다.
	 * 값 범위: VMBus 가 정의한 서술자.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는다. */
	struct vmpacket_descriptor hdr;
	/* [한국어] 요청 처리 결과(옆의 상류 주석대로 음수가 실패). */
	s32 status;			/* negative values are failures */
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 실제로 도달한 전원 상태(옆의 상류 주석대로 Windows 기준).
	 * 설정자: 호스트.
	 * 읽는 자: 완료 콜백.
	 * 값 범위: 요청한 상태와 다를 수 있다.
	 * 동기화: 완료 콜백 안에서만 읽는다. */
	u32 resultant_state;		/* In Windows terms */
	/* [한국어] 예약 필드. */
	u32 reserved;
/* [한국어] 전원 상태 변경 응답. */
} __packed;

struct pci_resources_assigned {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 메모리 범위 배열(옆의 상류 주석대로 여기서는 쓰지 않는다).
	 * 설정자: 없음 — memset 으로 0 이 된 채로 간다.
	 * 읽는 자: 호스트가 무시한다.
	 * 값 범위: 0.
	 * 동기화: 해당 없음.
	 * 리눅스는 BAR 주소를 config 공간에 직접 써서 알리므로 이 경로가 필요 없다. */
	u8 memory_range[0x14][6];	/* not used here */
	/* [한국어] MSI 서술자 개수.
	 * 설정자: 없음 — 이 파일은 0 인 채로 보낸다.
	 * 읽는 자: 호스트.
	 * 값 범위: 0.
	 * 동기화: 해당 없음. */
	u32 msi_descriptors;
	/* [한국어] 예약 필드. */
	u32 reserved[4];
/* [한국어] 자원 배정 통보(구판). 프로토콜 1.2 미만이 이 형식을 쓴다. */
} __packed;

struct pci_resources_assigned2 {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 메모리 범위 배열(옆의 상류 주석대로 여기서는 쓰지 않는다). */
	u8 memory_range[0x14][6];	/* not used here */
	/* [한국어] MSI 서술자 개수. 구판의 msi_descriptors 와 같은 자리이며 이름만 다르다.
	 * 설정자: 없음.
	 * 읽는 자: 호스트.
	 * 값 범위: 0.
	 * 동기화: 해당 없음. */
	u32 msi_descriptor_count;
	/* [한국어] 예약 필드. 구판보다 커져 구조체 전체 크기가 달라진다 —
	 * hv_send_resources_allocated() 가 판본에 따라 크기를 달리 계산하는 이유다. */
	u8 reserved[70];
/* [한국어] 자원 배정 통보(확장판). 프로토콜 1.2 이상이 이 형식을 쓴다. */
} __packed;

struct pci_create_interrupt {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 만들 인터럽트의 서술(초판 형식).
	 * 설정자: hv_compose_msi_msg() 계열이 벡터·CPU 정보를 채운다.
	 * 읽는 자: 호스트가 실제 인터럽트를 만들어 응답으로 돌려준다.
	 * 값 범위: 초판은 대상 CPU 를 8비트 마스크로 표현해 표현 범위가 좁다.
	 * 동기화: 패킷마다 독립이다. */
	struct hv_msi_desc int_desc;
/* [한국어] 인터럽트 생성 요청(초판). */
} __packed;

struct pci_create_int_response {
	/* [한국어] 응답 공통 머리.
	 * 설정자: 호스트.
	 * 읽는 자: 완료 콜백이 그 안의 status 를 본다.
	 * 값 범위: 프로토콜이 정한 응답 머리.
	 * 동기화: 수신 버퍼 위에서만 읽는다. */
	struct pci_response response;
	/* [한국어] 예약 필드. */
	u32 reserved;
	/* [한국어] 호스트가 만들어 준 인터럽트의 변환 서술.
	 * 설정자: 호스트.
	 * 읽는 자: 이 파일이 그 값으로 MSI 주소·데이터를 만들어 장치에 쓴다.
	 * 값 범위: 호스트가 정한 불투명 값.
	 * 동기화: 완료 콜백에서 복사한 뒤로는 소유자만 읽는다. */
	struct tran_int_desc int_desc;
/* [한국어] 인터럽트 생성 응답. 세 판본이 같은 응답 형식을 쓴다. */
} __packed;

struct pci_create_interrupt2 {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 만들 인터럽트의 서술(2판).
	 * 설정자: hv_compose_msi_msg() 계열.
	 * 읽는 자: 호스트.
	 * 값 범위: 2판은 CPU 를 프로세서 배열로 표현해 8개를 넘는 vCPU 를 다룰 수 있다.
	 * 동기화: 패킷마다 독립이다. */
	struct hv_msi_desc2 int_desc;
/* [한국어] 인터럽트 생성 요청(2판). */
} __packed;

struct pci_create_interrupt3 {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 만들 인터럽트의 서술(3판).
	 * 설정자: hv_compose_msi_msg() 계열.
	 * 읽는 자: 호스트.
	 * 값 범위: 3판은 벡터를 32비트로 넓혀 더 큰 벡터 번호를 표현한다.
	 * 동기화: 패킷마다 독립이다. */
	struct hv_msi_desc3 int_desc;
/* [한국어] 인터럽트 생성 요청(3판). 세 판본이 나뉜 이유가 vCPU 수와 벡터 범위의
 * 확장이며, 어느 것을 쓸지는 협상된 프로토콜 판본이 정한다. */
} __packed;

struct pci_delete_interrupt {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 지울 인터럽트의 변환 서술.
	 * 설정자: 생성 응답에서 받아 둔 값을 그대로 되돌려 보낸다.
	 * 읽는 자: 호스트가 이 값으로 어느 인터럽트를 지울지 가른다.
	 * 값 범위: 호스트가 준 불투명 값.
	 * 동기화: 패킷마다 독립이다. */
	struct tran_int_desc int_desc;
/* [한국어] 인터럽트 삭제 요청. */
} __packed;

/*
 * Note: the VM must pass a valid block id, wslot and bytes_requested.
 */
struct pci_read_block {
	struct pci_message message_type;
	/* [한국어] 읽을 블록의 번호.
	 * 설정자: hv_read_config_block().
	 * 읽는 자: 호스트.
	 * 값 범위: 장치가 정의한 블록 번호(위 상류 주석대로 유효한 값이어야 한다).
	 * 동기화: 패킷마다 독립이다. */
	u32 block_id;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 요청하는 바이트 수.
	 * 설정자: hv_read_config_block().
	 * 읽는 자: 호스트가 그만큼만 돌려준다.
	 * 값 범위: HV_CONFIG_BLOCK_SIZE_MAX 이하(위 상류 주석 참조).
	 * 동기화: 패킷마다 독립이다. */
	u32 bytes_requested;
/* [한국어] 블록 읽기 요청. */
} __packed;

struct pci_read_block_response {
	/* [한국어] VMBus 패킷의 공통 머리.
	 * 설정자: 호스트(VMBus 계층).
	 * 읽는 자: 이 파일은 직접 읽지 않고, 뒤따르는 필드를 찾는 기준으로만 쓴다.
	 * 값 범위: VMBus 가 정의한 서술자.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는다. */
	struct vmpacket_descriptor hdr;
	/* [한국어] 요청 처리 결과.
	 * 설정자: 호스트.
	 * 읽는 자: 읽기 완료 콜백.
	 * 값 범위: 0 이 성공. 위 응답들과 달리 부호 없는 타입이다.
	 * 동기화: 완료 콜백 안에서만 읽는다. */
	u32 status;
	/* [한국어] 읽어 온 블록 내용.
	 * 설정자: 호스트.
	 * 읽는 자: 완료 콜백이 호출자의 버퍼로 복사한다.
	 * 값 범위: 최대 크기로 고정된 배열이며, 실제 유효한 길이는 요청한 바이트 수다.
	 * 동기화: 완료 콜백에서 복사한 뒤로는 호출자만 본다. */
	u8 bytes[HV_CONFIG_BLOCK_SIZE_MAX];
/* [한국어] 블록 읽기 응답. */
} __packed;

/*
 * Note: the VM must pass a valid block id, wslot and byte_count.
 */
struct pci_write_block {
	struct pci_message message_type;
	/* [한국어] 쓸 블록의 번호.
	 * 설정자: hv_write_config_block().
	 * 읽는 자: 호스트.
	 * 값 범위: 장치가 정의한 블록 번호(위 상류 주석 참조).
	 * 동기화: 패킷마다 독립이다. */
	u32 block_id;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 쓸 바이트 수.
	 * 설정자: hv_write_config_block().
	 * 읽는 자: 호스트가 그만큼만 반영한다.
	 * 값 범위: HV_CONFIG_BLOCK_SIZE_MAX 이하(위 상류 주석 참조).
	 * 동기화: 패킷마다 독립이다. */
	u32 byte_count;
	/* [한국어] 쓸 내용.
	 * 설정자: hv_write_config_block() 이 호출자의 버퍼에서 복사해 담는다.
	 * 읽는 자: 호스트.
	 * 값 범위: 위 byte_count 만큼만 유효하다.
	 * 동기화: 패킷마다 독립이다. */
	u8 bytes[HV_CONFIG_BLOCK_SIZE_MAX];
/* [한국어] 블록 쓰기 요청. */
} __packed;

struct pci_dev_inval_block {
	/* [한국어] 호스트가 먼저 보내는 알림의 공통 머리.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 그 안의 type 으로 갈래를 정한다.
	 * 값 범위: 프로토콜이 정한 알림 종류.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는 구조라, 그 버퍼를 소유한 콜백만 본다. */
	struct pci_incoming_message incoming;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 무효화된 블록들의 비트마스크.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 등록된 block_invalidate 콜백에 넘긴다.
	 * 값 범위: 비트 n 이 서면 n 번 블록이 무효화됐다는 뜻이다.
	 * 동기화: 수신 버퍼 위에서 읽어 콜백에 값으로 넘긴다. */
	u64 block_mask;
/* [한국어] 블록 무효화 알림. 장치 쪽 설정이 바뀌었으니 다시 읽으라는 신호다. */
} __packed;

struct pci_dev_incoming {
	/* [한국어] 호스트가 먼저 보내는 알림의 공통 머리.
	 * 설정자: 호스트.
	 * 읽는 자: hv_pci_onchannelcallback() 이 그 안의 type 으로 갈래를 정한다.
	 * 값 범위: 프로토콜이 정한 알림 종류.
	 * 동기화: 수신 버퍼 위에 겹쳐 읽는 구조라, 그 버퍼를 소유한 콜백만 본다. */
	struct pci_incoming_message incoming;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
/* [한국어] 장치 제거 요청 알림. 슬롯 번호 외에 실을 것이 없어 이렇게 짧다. */
} __packed;

struct pci_eject_response {
	/* [한국어] 메시지 종류를 담은 공통 머리.
	 * 설정자: 요청을 만드는 함수가 PCI_ 계열 상수 중 하나를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 메시지를 해석한다.
	 * 값 범위: 프로토콜이 정한 메시지 종류 열거값.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	struct pci_message message_type;
	/* [한국어] 설정자: 요청을 만드는 쪽이 대상 장치의 슬롯 번호를 넣는다.
	 * 읽는 자: 호스트가 이 값으로 어느 장치의 요청인지 가른다.
	 * 값 범위: Windows 슬롯 인코딩. devfn 과 1:1 대응한다.
	 * 동기화: 패킷마다 독립이라 공유되지 않는다. */
	union win_slot_encoding wslot;
	/* [한국어] 제거 처리 결과.
	 * 설정자: 게스트가 제거를 마친 뒤 채워 보낸다.
	 * 읽는 자: 호스트.
	 * 값 범위: 0 이 성공.
	 * 동기화: 패킷마다 독립이다. */
	u32 status;
/* [한국어] 장치 제거 응답. 위 알림에 대한 게스트의 답이다. */
} __packed;

static int pci_ring_size = VMBUS_RING_SIZE(SZ_16K);

/*
 * Driver specific state.
 */

enum hv_pcibus_state {
	/* [한국어] 초기 상태. probe 가 구조체를 잡은 직후이며, 아직 호스트와 아무것도 합의하지 않았다. */
	hv_pcibus_init = 0,
	/* [한국어] 탐색 완료. D0 에 들어가고 자원까지 배정했지만 아직 리눅스 버스를 만들지 않은 상태다. */
	hv_pcibus_probed,
	hv_pcibus_installed,
	hv_pcibus_removing,
	hv_pcibus_maximum
};

struct hv_pcibus_device {
/* [한국어] x86 에서는 도메인 번호를 담는 pci_sysdata 를 쓴다. */
#ifdef CONFIG_X86
	struct pci_sysdata sysdata;
/* [한국어] ARM64 에서는 ECAM 창 서술을 쓴다. 두 아키텍처가 config 접근 방식이 달라
 * 같은 자리에 서로 다른 타입이 온다. */
#elif defined(CONFIG_ARM64)
	struct pci_config_window sysdata;
/* [한국어] 아키텍처 분기 끝. 이 필드가 **맨 앞** 이라, PCI 코어가 bus->sysdata 로 읽는 것이
 * 이 구조체의 시작과 같은 주소가 된다. */
#endif
	struct pci_host_bridge *bridge;
	/* [한국어] 인터럽트 도메인의 이름 노드.
	 * 설정자: hv_pci_probe() 가 VMBus 인스턴스 GUID 로 만든 이름으로 잡는다.
	 * 읽는 자: hv_pcie_init_irq_domain() 과 해제 경로.
	 * 값 범위: 유효한 fwnode 포인터.
	 * 동기화: probe 후 불변. */
	struct fwnode_handle *fwnode;
	/* Protocol version negotiated with the host */
	enum pci_protocol_version_t protocol_version;

	struct mutex state_lock;
	/* [한국어] 이 버스의 현재 상태.
	 * 설정자: probe/remove/suspend 가 바꾸며, 그때 tasklet 을 껐다 켜거나
	 * state_lock 을 쥔다.
	 * 읽는 자: 채널 콜백 경로가 이 값으로 알림을 처리할지 판단한다.
	 * 값 범위: 위 enum 의 다섯 값.
	 * 동기화: tasklet 을 껐다 켜는 사이에 바꾸는 관용을 쓴다 — 채널 콜백이
	 * tasklet 문맥에서 돌기 때문에, 그 사이에는 읽는 쪽이 실행되지 않는다. */
	enum hv_pcibus_state state;

	struct hv_device *hdev;
	/* [한국어] 4GB 아래 MMIO 로 필요한 총 크기.
	 * 설정자: BAR 탐색 단계가 자식 장치들의 요구를 합산해 채운다.
	 * 읽는 자: hv_pci_allocate_bridge_windows() 가 이만큼을 호스트에 요청한다.
	 * 값 범위: 0 이면 그 창이 필요 없다는 뜻이라 할당을 건너뛴다.
	 * 동기화: probe 안에서만 다룬다. */
	resource_size_t low_mmio_space;
	/* [한국어] 4GB 위 MMIO 로 필요한 총 크기. 위와 같은 규약이다. */
	resource_size_t high_mmio_space;
	/* [한국어] config 접근에 쓸 MMIO 창.
	 * 설정자: hv_allocate_config_window().
	 * 읽는 자: hv_pci_enter_d0() 이 시작 주소를 호스트에 알리고,
	 * hv_free_config_window() 가 돌려준다.
	 * 값 범위: 유효한 자원 포인터. IORESOURCE_BUSY 가 선 채로 유지된다.
	 * 동기화: probe 후 불변. */
	struct resource *mem_config;
	/* [한국어] 4GB 아래 브리지 창.
	 * 설정자: hv_pci_allocate_bridge_windows().
	 * 읽는 자: 해제 경로와 PCI 코어(브리지 창 목록에 등록된다).
	 * 값 범위: 유효한 자원 포인터 또는 NULL(크기가 0 이면 할당하지 않는다).
	 * 동기화: probe 후 불변. */
	struct resource *low_mmio_res;
	/* [한국어] 4GB 위 브리지 창. 위와 같은 규약이다. */
	struct resource *high_mmio_res;
	/* [한국어] 장치 목록 질의의 완료 객체.
	 * 설정자: hv_pci_query_relations() 가 cmpxchg 로 등록하고,
	 * 알림 처리 쪽이 다 쓰면 지운다.
	 * 읽는 자: 장치 목록 알림을 처리하는 쪽이 이것을 깨운다.
	 * 값 범위: 유효한 completion 포인터 또는 NULL.
	 * 동기화: cmpxchg 로 등록해 동시 질의를 막는다 — NULL 이 아니면 이미
	 * 진행 중이라는 뜻이라 -ENOTEMPTY 로 거절한다. */
	struct completion *survey_event;
	/* [한국어] config 접근을 직렬화하는 스핀락(옆의 상류 주석대로 두 스레드가 인덱스
	 * 페이지에 동시에 쓰는 것을 막는다).
	 * 설정자·읽는 자: config 읽기·쓰기 경로.
	 * 값 범위: 스핀락.
	 * 동기화: config 접근이 '슬롯 번호를 쓰고 데이터를 읽는' 두 단계라,
	 * 그 사이에 다른 스레드가 슬롯 번호를 덮으면 엉뚱한 장치를 읽게 된다. */
	spinlock_t config_lock;	/* Avoid two threads writing index page */
	/* [한국어] 아래 두 목록을 지키는 스핀락(옆의 상류 주석).
	 * 설정자·읽는 자: 목록을 훑거나 고치는 모든 경로.
	 * 값 범위: 스핀락.
	 * 동기화: 인터럽트 문맥에서도 잡으므로 irqsave 판으로 쓴다. */
	spinlock_t device_list_lock;	/* Protect lists below */
	/* [한국어] config 창의 가상 주소.
	 * 설정자: hv_pci_probe() 의 ioremap.
	 * 읽는 자: config 읽기·쓰기 경로.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. */
	void __iomem *cfg_addr;

	struct list_head children;
	/* [한국어] 장치 목록 변경 알림을 순서대로 처리하기 위한 대기열.
	 * 설정자: 알림 처리 쪽이 새 목록을 여기 매단다.
	 * 읽는 자: 워크 함수가 앞에서 꺼내 반영한다.
	 * 값 범위: 아직 반영되지 않은 목록 스냅샷들.
	 * 동기화: 위 device_list_lock 이 지킨다. */
	struct list_head dr_list;

	struct irq_domain *irq_domain;

	struct workqueue_struct *wq;

	/* Highest slot of child device with resources allocated */
	int wslot_res_allocated;
	bool use_calls; /* Use hypercalls to access mmio cfg space */
/* [한국어] 이 가상 버스의 상태 전부. 실제 하드웨어가 없으므로 이 구조체가
 * 곧 '버스' 다. */
};

/*
 * Tracks "Device Relations" messages from the host, which must be both
 * processed in order and deferred so that they don't run in the context
 * of the incoming packet callback.
 */
struct hv_dr_work {
	struct work_struct wrk;
	/* [한국어] 이 워크가 속한 버스.
	 * 설정자: 워크를 거는 쪽.
	 * 읽는 자: 워크 함수.
	 * 값 범위: 유효한 버스 포인터.
	 * 동기화: 워크 하나당 하나씩 할당되어 공유되지 않는다. */
	struct hv_pcibus_device *bus;
/* [한국어] 장치 목록 변경을 워크큐로 미루기 위한 포장(위 상류 주석이 그 이유를 밝힌다). */
};

struct hv_pcidev_description {
	/* [한국어] 벤더 ID(옆의 상류 주석).
	 * 설정자: 호스트가 보낸 장치 서술에서 복사한다.
	 * 읽는 자: config 읽기가 이 값으로 표준 config 헤더를 흉내 낸다.
	 * 값 범위: PCI 벤더 ID.
	 * 동기화: 장치 생성 후 불변. */
	u16	v_id;	/* vendor ID */
	/* [한국어] 장치 ID(옆의 상류 주석). 위와 같은 규약이다. */
	u16	d_id;	/* device ID */
	/* [한국어] 리비전.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: PCI 리비전 바이트.
	 * 동기화: 장치 생성 후 불변. */
	u8	rev;
	/* [한국어] 프로그래밍 인터페이스(클래스 코드의 최하위 바이트).
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: PCI 클래스 코드의 prog-if.
	 * 동기화: 장치 생성 후 불변. */
	u8	prog_intf;
	/* [한국어] 서브클래스. 위와 같다. */
	u8	subclass;
	/* [한국어] 기본 클래스. 위 셋을 합치면 24비트 클래스 코드가 된다. */
	u8	base_class;
	/* [한국어] 서브시스템 ID(벤더와 장치를 한 워드에 담는다).
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 상위 16비트가 서브시스템 장치, 하위가 서브시스템 벤더.
	 * 동기화: 장치 생성 후 불변. */
	u32	subsystem_id;
	/* [한국어] 이 장치의 Windows 슬롯 번호.
	 * 설정자: 호스트가 보낸 값.
	 * 읽는 자: get_pcichild_wslot() 의 비교 기준이자, devfn 으로의 변환 원본이다.
	 * 값 범위: 0~255.
	 * 동기화: 장치 생성 후 불변. */
	union	win_slot_encoding win_slot;
	/* [한국어] 일련번호(옆의 상류 주석).
	 * 설정자: 호스트.
	 * 읽는 자: 슬롯 이름을 짓는 데 쓴다.
	 * 값 범위: 호스트가 정한 값.
	 * 동기화: 장치 생성 후 불변. */
	u32	ser;	/* serial number */
	/* [한국어] 장치 속성 플래그.
	 * 설정자: 호스트. 확장판 서술에만 있고 구판에서는 0 으로 남는다.
	 * 읽는 자: 이 파일이 특정 비트를 보는 곳이 있는지는 사용처를 따라가야 한다.
	 * 값 범위: 호스트가 정한 비트 묶음.
	 * 동기화: 장치 생성 후 불변. */
	u32	flags;
	/* [한국어] 가상 NUMA 노드 번호.
	 * 설정자: 호스트. 확장판 서술에만 있다.
	 * 읽는 자: 장치를 등록할 때 NUMA 정보로 쓴다.
	 * 값 범위: 유효한 노드 번호. 구판에서는 0 으로 남는다.
	 * 동기화: 장치 생성 후 불변. */
	u16	virtual_numa_node;
/* [한국어] 장치 하나에 대해 호스트가 알려 준 정보. 실제 config 공간을 읽을 수 없어
 * 이 서술이 그 자리를 대신한다. */
};

struct hv_dr_state {
	/* [한국어] 대기열의 고리.
	 * 설정자: 알림 처리 쪽이 hbus->dr_list 에 매단다.
	 * 읽는 자: 워크 함수가 꺼내 반영한다.
	 * 값 범위: dr_list 를 머리로 하는 목록의 한 마디.
	 * 동기화: device_list_lock 이 지킨다. */
	struct list_head list_entry;
	/* [한국어] 이 스냅샷의 장치 개수.
	 * 설정자: 알림 처리 쪽.
	 * 읽는 자: 워크 함수가 아래 배열을 훑는 범위로 쓴다.
	 * 값 범위: 호스트가 보고한 장치 수.
	 * 동기화: 생성 후 불변. */
	u32 device_count;
	/* [한국어] 그만큼의 장치 서술 배열. __counted_by 가 위 필드를 길이로 지목해,
	 * 컴파일러와 런타임 검사가 범위를 확인할 수 있게 한다.
	 * 설정자: 알림 처리 쪽이 호스트 메시지에서 복사한다.
	 * 읽는 자: 워크 함수.
	 * 값 범위: device_count 개.
	 * 동기화: 생성 후 불변. */
	struct hv_pcidev_description func[] __counted_by(device_count);
/* [한국어] 장치 목록 한 벌의 스냅샷. 알림을 받은 순서대로 처리하려고 큐에 쌓는다. */
};

struct hv_pci_dev {
	/* List protected by pci_rescan_remove_lock */
	struct list_head list_entry;
	refcount_t refs;
	/* [한국어] 이 장치의 sysfs 슬롯 객체.
	 * 설정자: 장치를 등록할 때 만든다.
	 * 읽는 자: 제거 경로가 없앤다.
	 * 값 범위: 유효한 슬롯 포인터 또는 NULL.
	 * 동기화: pci_rescan_remove_lock 아래에서 다룬다. */
	struct pci_slot *pci_slot;
	/* [한국어] 호스트가 알려 준 이 장치의 서술.
	 * 설정자: 장치를 만들 때 복사해 담는다.
	 * 읽는 자: config 읽기가 이 값으로 표준 헤더를 흉내 낸다.
	 * 값 범위: 위 hv_pcidev_description 의 내용.
	 * 동기화: 장치 생성 후 불변. */
	struct hv_pcidev_description desc;
	/* [한국어] 이번 목록 갱신에서 사라진 것으로 표시됐는지.
	 * 설정자: 목록 갱신 워크가 새 목록에 없는 장치에 세운다.
	 * 읽는 자: 같은 워크가 그 표시를 보고 제거를 진행한다.
	 * 값 범위: true 면 제거 대상이다.
	 * 동기화: 목록 갱신 워크 안에서만 다룬다. */
	bool reported_missing;
	/* [한국어] 이 장치가 속한 버스.
	 * 설정자: 장치를 만들 때.
	 * 읽는 자: 워크 함수와 제거 경로가 버스를 되찾는 데 쓴다.
	 * 값 범위: 유효한 버스 포인터.
	 * 동기화: 장치 생성 후 불변. */
	struct hv_pcibus_device *hbus;
	/* [한국어] 이 장치에 대한 지연 작업(제거 등)을 담는 워크.
	 * 설정자: 그 작업을 거는 쪽.
	 * 읽는 자: 워크큐.
	 * 값 범위: 워크 구조체.
	 * 동기화: 워크큐가 관리한다. */
	struct work_struct wrk;

	void (*block_invalidate)(void *context, u64 block_mask);
	/* [한국어] 위 콜백에 함께 넘길 문맥.
	 * 설정자: hv_register_block_invalidate().
	 * 읽는 자: 무효화 알림 처리가 콜백에 그대로 넘긴다.
	 * 값 범위: 등록한 쪽이 정한 불투명 포인터.
	 * 동기화: 콜백과 같다. */
	void *invalidate_context;

	/*
	 * What would be observed if one wrote 0xFFFFFFFF to a BAR and then
	 * read it back, for each of the BAR offsets within config space.
	 */
	u32 probed_bar[PCI_STD_NUM_BARS];
};

struct hv_pci_compl {
	/* [한국어] 응답을 기다리는 완료 객체.
	 * 설정자: 요청을 보내는 쪽이 init_completion 으로 초기화한다.
	 * 읽는 자: hv_pci_generic_compl() 이 응답을 받으면 깨운다.
	 * 값 범위: completion 구조체.
	 * 동기화: completion 자체가 동기화 수단이다. */
	struct completion host_event;
	/* [한국어] 호스트가 돌려준 처리 결과.
	 * 설정자: hv_pci_generic_compl() 이 응답에서 복사한다.
	 * 읽는 자: 기다리던 쪽이 깨어난 뒤 확인한다.
	 * 값 범위: 0 이상이 성공, 음수가 실패. STATUS_REVISION_MISMATCH 는
	 * 판본 협상에서 '그 판본을 모른다' 는 특별한 값이다.
	 * 동기화: 완료 신호가 이 값의 가시성까지 보장한다. */
	s32 completion_status;
};

/* [한국어]
 * hv_pci_onchannelcallback - 호스트가 보낸 패킷을 받는 유일한 입구
 *
 * @context: 이 버스를 나타내는 struct hv_pcibus_device.
 * @return: 없음.
 *
 * **이 파일 프로토콜 처리의 중심이다.** 상류 주석대로 호스트가 이 채널에
 * 패킷을 보낼 때마다 불리며, 채널은 이 루트 PCI 버스 전용이다.
 *
 * **받은 패킷을 두 갈래로 나눈다.**
 *
 * **VM_PKT_COMP -- 응답이다.** 요청을 보낼 때 넘겨 둔 패킷 주소를
 * 요청 ID 로 되찾아, 거기 담긴 completion_func 를 부른다.
 * **__vmbus_request_addr_match 를 lock_requestor 안에서 부르는 것이 요점이다** --
 * 그 표에서 항목을 빼는 것과 그 주소를 쓰는 것 사이에 틈이 생기면
 * hv_compose_msi_msg() 의 정리 경로와 겹칠 수 있다.
 *
 * **VM_PKT_DATA_INBAND -- 호스트가 먼저 보낸 알림이다.** 넷을 다룬다.
 * - PCI_BUS_RELATIONS / _RELATIONS2 : 장치 목록 변경.
 * - PCI_EJECT : 장치 뽑기 요청.
 * - PCI_INVALIDATE_BLOCK : 설정 블록 무효화.
 * **모두 크기를 먼저 확인한다** -- 가변 길이 배열이 붙는 것은
 * struct_size 로 실제 필요한 크기를 계산해 견준다.
 * 호스트를 믿지 않는 방어다.
 *
 * **버퍼를 키우는 재시도가 루프 맨 앞에 있다.**
 * -ENOBUFS 면 받은 크기만큼 다시 잡아 이어서 시도한다 --
 * 장치 목록이 길면 처음 잡은 0x100 바이트로 모자라기 때문이다.
 *
 * **응답보다 작은 패킷은 건너뛴다** -- 원문 주석이 밝히듯
 * 들어오는 모든 패킷은 최소한 응답 크기여야 한다.
 *
 * **GFP_ATOMIC 으로 버퍼를 잡는다** -- 채널 콜백 문맥이라 잠들 수 없다.
 *
 * **hv_compose_msi_msg() 가 이 함수를 직접 부르기도 한다** --
 * 그쪽이 IRQ 락을 쥔 채 응답을 폴링해야 하기 때문이다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   VMBus 코어 / hv_compose_msi_msg -> [이 함수]
 *     -> comp_packet->completion_func(), hv_pci_devices_present(),
 *   hv_pci_eject_device(), hpdev->block_invalidate()
 */
static void hv_pci_onchannelcallback(void *context);

#ifdef CONFIG_X86
#define DELIVERY_MODE		APIC_DELIVERY_MODE_FIXED
#define HV_MSI_CHIP_FLAGS	MSI_CHIP_FLAG_SET_ACK

/* [한국어] 
 *  * hv_pci_irqchip_init - x86 갈래. IRQ 칩 초기화가 필요 없다
 *  *
 *  * @return: 늘 0.
 *  *
 *  * **x86 에서는 준비할 것이 없다.** MSI 를 커널의 x86_vector_domain 위에
 *  * 얹기만 하면 되고, 그 도메인은 부팅 때 이미 세워져 있기 때문이다.
 *  *
 *  * **ARM64 갈래에는 같은 이름의 다른 함수가 있다** -- 그쪽은 GIC 위에
 *  * 자기 IRQ 도메인을 새로 만들어야 해서 쉰 줄이 넘는다.
 *  * 이 파일에서 같은 이름이 두 벌 정의된 함수가 넷 있으며, 이것이 그중 하나다.
 *  *
 *  * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *  *
 *  * 호출 체인:
 *   *   init_hv_pci_drv -> [이 함수] */
static int hv_pci_irqchip_init(void)
{
	return 0;
}

/* [한국어] 
 *  * hv_pci_get_root_domain - x86 갈래. MSI 도메인의 부모를 돌려준다
 *  *
 *  * @return: x86_vector_domain.
 *  *
 *  * **커널이 이미 만들어 둔 벡터 도메인을 그대로 부모로 쓴다.**
 *  * 이 드라이버가 만드는 MSI 도메인이 그 아래에 매달리며,
 *  * 실제 벡터 배정은 부모가 한다.
 *  *
 *  * **ARM64 갈래는 이 파일이 직접 만든 hv_msi_gic_irq_domain 을 돌려준다** --
 *  * 그쪽에는 바로 쓸 수 있는 벡터 도메인이 없기 때문이다.
 *  *
 *  * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 도메인 생성 경로).
 *  *
 *  * 호출 체인:
 *   *   hv_pcie_init_irq_domain -> [이 함수] */
static struct irq_domain *hv_pci_get_root_domain(void)
{
	return x86_vector_domain;
}

/* [한국어] 
 *  * hv_msi_get_int_vector - x86 갈래. 이 인터럽트에 배정된 벡터 번호를 얻는다
 *  *
 *  * @data: 인터럽트를 나타내는 irq_data.
 *  * @return: IDT 벡터 번호.
 *  *
 *  * **부모 도메인(x86_vector_domain)이 배정해 둔 벡터를 꺼내 온다.**
 *  * irqd_cfg() 가 그 도메인의 설정 구조체를 되찾아 주며,
 *  * **그 함수의 정의는 arch/x86 에 있고 이 트리에 없다.**
 *  *
 *  * 이 값이 호스트에 보내는 MSI 등록 요청에 실린다 --
 *  * 호스트가 인터럽트를 어느 벡터로 넣어 줄지 알아야 하기 때문이다.
 *  *
 *  * **ARM64 갈래는 부모 irq_data 의 hwirq 를 그대로 돌려준다** --
 *  * GIC 에서는 SPI 번호가 곧 벡터다.
 *  *
 *  * 실행 컨텍스트: 어디서든. 포인터를 따라가는 계산뿐이다.
 *  *
 *  * 호출 체인:
 *   *   hv_irq_retarget_interrupt / hv_compose_msi_msg -> [이 함수] */
static unsigned int hv_msi_get_int_vector(struct irq_data *data)
{
	struct irq_cfg *cfg = irqd_cfg(data);

	return cfg->vector;
}

#define hv_msi_prepare		pci_msi_prepare

/**
 * hv_irq_retarget_interrupt() - "Unmask" the IRQ by setting its current
 * affinity.
 * @data:	Describes the IRQ
 *
 * Build new a destination for the MSI and make a hypercall to
 * update the Interrupt Redirection Table. "Device Logical ID"
 * is built out of this PCI bus's instance GUID and the function
 * number of the device.
 */
/* [한국어]
 * hv_irq_retarget_interrupt - 하이퍼콜로 인터럽트의 대상 가상 프로세서를 바꾼다
 *
 * @data: 인터럽트를 나타내는 irq_data.
 * @return: 없음.
 *
 * **게스트는 인터럽트 라우팅을 직접 바꿀 수 없다.** MSI 주소·데이터를 고쳐도
 * 그것을 실제로 해석하는 것은 하이퍼바이저의 인터럽트 리디렉션 테이블이므로,
 * 하이퍼콜로 그 테이블을 고쳐 달라고 부탁해야 한다. 상류 주석이 그 뜻을 밝힌다.
 *
 * **device_id 를 만드는 방식이 이 함수의 특징이다.** 상류 주석대로
 * **이 PCI 버스의 VMBus 인스턴스 GUID 와 장치의 함수 번호를 섞어** 만든다 --
 * GUID 의 바이트 5, 4, 7, 6 을 자리마다 밀어 넣고 마지막에 함수 번호를 OR 한다.
 * 호스트는 그것으로 어느 장치의 인터럽트인지 알아낸다.
 *
 * **프로토콜 판이 하이퍼콜의 모양을 가른다.**
 * - **1.2 이상이면** VP_SET 형식을 쓴다. 상류 주석이 밝히듯 그 형식이
 *   가상 프로세서 64개를 넘는 구성을 지원하며, cpumask 를 vpset 으로 옮긴다.
 *   **가변 길이 하이퍼콜이라 var_size 를 17비트 밀어 하이퍼콜 코드에 실는다.**
 * - **그 아래면** 64비트 vp_mask 에 비트를 하나씩 세운다. 곧 64개까지만 된다.
 *
 * **this_cpu_ptr 로 얻은 per-CPU 버퍼를 쓰므로 local_irq_save 가 필수다** --
 * 그 사이 선점되어 다른 CPU 로 옮겨 가면 남의 버퍼를 쓰게 된다.
 *
 * **버스를 내리는 중이면 실패를 로그로 남기지 않는다** -- 상류 주석이 밝히듯
 * 최대 절전에서 CPU 를 오프라인시킬 때 커널이 인터럽트를 옮기려 하는데,
 * 그때는 VMBus 채널이 이미 닫혀 있어 이 하이퍼콜이 늘 실패하기 때문이다.
 *
 * **정의가 arch/x86 이나 drivers/hv 에 있어 이 트리에서 확인할 수 없는
 * 것들**: hv_do_hypercall, hyperv_pcpu_input_arg, cpumask_to_vpset,
 * hv_cpu_number_to_vp_number, hv_result_success.
 *
 * 실행 컨텍스트: IRQ 코어의 unmask 경로. 인터럽트를 막고 돈다.
 *
 * 호출 체인:
 *   hv_arch_irq_unmask -> [이 함수] -> hv_do_hypercall()
 */
static void hv_irq_retarget_interrupt(struct irq_data *data)
{
	struct msi_desc *msi_desc = irq_data_get_msi_desc(data);
	struct hv_retarget_device_interrupt *params;
	struct tran_int_desc *int_desc;
	struct hv_pcibus_device *hbus;
	const struct cpumask *dest;
	cpumask_var_t tmp;
	struct pci_bus *pbus;
	struct pci_dev *pdev;
	unsigned long flags;
	u32 var_size = 0;
	int cpu, nr_bank;
	u64 res;

	dest = irq_data_get_effective_affinity_mask(data);
	pdev = msi_desc_to_pci_dev(msi_desc);
	pbus = pdev->bus;
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);
	int_desc = data->chip_data;
	if (!int_desc) {
		dev_warn(&hbus->hdev->device, "%s() can not unmask irq %u\n",
			 __func__, data->irq);
		return;
	}

	local_irq_save(flags);

	params = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(params, 0, sizeof(*params));
	params->partition_id = HV_PARTITION_ID_SELF;
	params->int_entry.source = HV_INTERRUPT_SOURCE_MSI;
	params->int_entry.msi_entry.address.as_uint32 = int_desc->address & 0xffffffff;
	params->int_entry.msi_entry.data.as_uint32 = int_desc->data;
	params->device_id = (hbus->hdev->dev_instance.b[5] << 24) |
			   (hbus->hdev->dev_instance.b[4] << 16) |
			   (hbus->hdev->dev_instance.b[7] << 8) |
			   (hbus->hdev->dev_instance.b[6] & 0xf8) |
			   PCI_FUNC(pdev->devfn);
	params->int_target.vector = hv_msi_get_int_vector(data);

	if (hbus->protocol_version >= PCI_PROTOCOL_VERSION_1_2) {
		/*
		 * PCI_PROTOCOL_VERSION_1_2 supports the VP_SET version of the
		 * HVCALL_RETARGET_INTERRUPT hypercall, which also coincides
		 * with >64 VP support.
		 * ms_hyperv.hints & HV_X64_EX_PROCESSOR_MASKS_RECOMMENDED
		 * is not sufficient for this hypercall.
		 */
		params->int_target.flags |=
			HV_DEVICE_INTERRUPT_TARGET_PROCESSOR_SET;

		if (!alloc_cpumask_var(&tmp, GFP_ATOMIC)) {
			res = 1;
			goto out;
		}

		cpumask_and(tmp, dest, cpu_online_mask);
		nr_bank = cpumask_to_vpset(&params->int_target.vp_set, tmp);
		free_cpumask_var(tmp);

		if (nr_bank <= 0) {
			res = 1;
			goto out;
		}

		/*
		 * var-sized hypercall, var-size starts after vp_mask (thus
		 * vp_set.format does not count, but vp_set.valid_bank_mask
		 * does).
		 */
		var_size = 1 + nr_bank;
	} else {
		for_each_cpu_and(cpu, dest, cpu_online_mask) {
			params->int_target.vp_mask |=
				(1ULL << hv_cpu_number_to_vp_number(cpu));
		}
	}

	res = hv_do_hypercall(HVCALL_RETARGET_INTERRUPT | (var_size << 17),
			      params, NULL);

out:
	local_irq_restore(flags);

	/*
	 * During hibernation, when a CPU is offlined, the kernel tries
	 * to move the interrupt to the remaining CPUs that haven't
	 * been offlined yet. In this case, the below hv_do_hypercall()
	 * always fails since the vmbus channel has been closed:
	 * refer to cpu_disable_common() -> fixup_irqs() ->
	 * irq_migrate_all_off_this_cpu() -> migrate_one_irq().
	 *
	 * Suppress the error message for hibernation because the failure
	 * during hibernation does not matter (at this time all the devices
	 * have been frozen). Note: the correct affinity info is still updated
	 * into the irqdata data structure in migrate_one_irq() ->
	 * irq_do_set_affinity(), so later when the VM resumes,
	 * hv_pci_restore_msi_state() is able to correctly restore the
	 * interrupt with the correct affinity.
	 */
	if (!hv_result_success(res) && hbus->state != hv_pcibus_removing)
		dev_err(&hbus->hdev->device,
			"%s() failed: %#llx", __func__, res);
}

/* [한국어] 
 *  * hv_arch_irq_unmask - x86 갈래. 인터럽트 마스크를 풀며 대상 CPU 를 갱신한다
 *  *
 *  * @data: 인터럽트를 나타내는 irq_data.
 *  * @return: 없음.
 *  *
 *  * **x86 에서 마스크 해제는 곧 "이 인터럽트를 어느 가상 프로세서로 보낼지
 *  * 하이퍼바이저에 다시 알리는 일" 이다.** 게스트가 직접 인터럽트 라우팅을
 *  * 바꿀 수 없으므로 하이퍼콜이나 호스트 매핑을 거쳐야 한다.
 *  *
 *  * 갈래가 둘이다.
 *  * - **루트 파티션이면** hv_map_msi_interrupt() 로 매핑을 다시 만든다.
 *  *   루트 파티션은 하이퍼바이저와 더 가까운 특권 위치라 다른 경로를 쓴다.
 *  * - **그 밖이면** hv_irq_retarget_interrupt() 로 재타게팅 하이퍼콜을 낸다.
 *  *
 *  * **hv_root_partition() 과 hv_map_msi_interrupt() 의 정의는 arch/x86 이나
 *  * drivers/hv 에 있고 이 트리에 없다.**
 *  *
 *  * **ARM64 갈래는 빈 함수다** -- GIC 가 마스크를 스스로 다루므로
 *  * 이 계층이 할 일이 없다.
 *  *
 *  * 실행 컨텍스트: IRQ 코어의 unmask 경로. 인터럽트가 막힌 채로 불린다.
 *  *
 *  * 호출 체인:
 *   *   hv_irq_unmask -> [이 함수]
 *   *     -> hv_map_msi_interrupt() 또는 hv_irq_retarget_interrupt() */
static void hv_arch_irq_unmask(struct irq_data *data)
{
	if (hv_root_partition())
		/*
		 * In case of the nested root partition, the nested hypervisor
		 * is taking care of interrupt remapping and thus the
		 * MAP_DEVICE_INTERRUPT hypercall is required instead of
		 * RETARGET_INTERRUPT.
		 */
		(void)hv_map_msi_interrupt(data, NULL);
	else
		hv_irq_retarget_interrupt(data);
}
#elif defined(CONFIG_ARM64)
/*
 * SPI vectors to use for vPCI; arch SPIs range is [32, 1019], but leaving a bit
 * of room at the start to allow for SPIs to be specified through ACPI and
 * starting with a power of two to satisfy power of 2 multi-MSI requirement.
 */
#define HV_PCI_MSI_SPI_START	64
#define HV_PCI_MSI_SPI_NR	(1020 - HV_PCI_MSI_SPI_START)
#define DELIVERY_MODE		0
#define HV_MSI_CHIP_FLAGS	MSI_CHIP_FLAG_SET_EOI
#define hv_msi_prepare		NULL

struct hv_pci_chip_data {
	DECLARE_BITMAP(spi_map, HV_PCI_MSI_SPI_NR);
	struct mutex	map_lock;
};

/* Hyper-V vPCI MSI GIC IRQ domain */
static struct irq_domain *hv_msi_gic_irq_domain;

/* Hyper-V PCI MSI IRQ chip */
static struct irq_chip hv_arm64_msi_irq_chip = {
	.name = "MSI",
	.irq_set_affinity = irq_chip_set_affinity_parent,
	.irq_eoi = irq_chip_eoi_parent,
	.irq_mask = irq_chip_mask_parent,
	.irq_unmask = irq_chip_unmask_parent
};

/* [한국어] 
 *  * hv_msi_get_int_vector - ARM64 갈래. 부모가 배정한 SPI 번호를 돌려준다
 *  *
 *  * @irqd: 인터럽트를 나타내는 irq_data.
 *  * @return: GIC 의 하드웨어 인터럽트 번호(SPI).
 *  *
 *  * **GIC 에서는 SPI 번호가 곧 벡터다.** 부모 도메인(GIC)이 배정한
 *  * hwirq 를 그대로 꺼내 쓰면 된다.
 *  *
 *  * **x86 갈래가 irqd_cfg() 로 벡터 설정 구조체를 거치는 것과 대비된다** --
 *  * x86 은 IDT 벡터와 IRQ 번호가 별개라 한 겹이 더 필요하다.
 *  *
 *  * 이 값이 호스트에 보내는 MSI 등록 요청에 실린다.
 *  *
 *  * 실행 컨텍스트: 어디서든. 포인터를 따라가는 계산뿐이다.
 *  *
 *  * 호출 체인:
 *   *   hv_compose_msi_msg -> [이 함수] */
static unsigned int hv_msi_get_int_vector(struct irq_data *irqd)
{
	return irqd->parent_data->hwirq;
}

/*
 * @nr_bm_irqs:		Indicates the number of IRQs that were allocated from
 *			the bitmap.
 * @nr_dom_irqs:	Indicates the number of IRQs that were allocated from
 *			the parent domain.
 */
/* [한국어]
 * hv_pci_vec_irq_free - ARM64 갈래. SPI 비트맵을 비우고 IRQ 를 놓는다
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @virq: 놓을 첫 리눅스 IRQ 번호.
 * @nr_bm_irqs: 비트맵에서 되돌릴 개수.
 * @nr_dom_irqs: 도메인에서 놓을 개수.
 * @return: 없음.
 *
 * **개수 인자가 둘인 것이 이 함수의 요점이다.** 보통은 같지만,
 * 할당이 도중에 실패했을 때는 다르다 -- 비트맵은 요청한 만큼 통째로
 * 잡았는데 도메인 쪽은 성공한 것까지만 만들어졌기 때문이다.
 * hv_pci_vec_irq_domain_alloc() 의 실패 경로가 그렇게 부른다.
 *
 * **bitmap_release_region 이 2의 거듭제곱 단위로 동작한다.**
 * 그래서 get_count_order() 로 개수를 지수로 바꿔 넘긴다 --
 * 할당 쪽의 bitmap_find_free_region 과 짝을 맞춘 것이다.
 *
 * SPI 번호에서 HV_PCI_MSI_SPI_START 를 빼 비트맵 색인으로 옮긴다 --
 * 이 도메인이 관리하는 것은 64번부터이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   hv_pci_vec_irq_domain_free / hv_pci_vec_irq_domain_alloc 의 실패 경로
 *     -> [이 함수] -> bitmap_release_region(), irq_domain_free_irqs_parent()
 */
static void hv_pci_vec_irq_free(struct irq_domain *domain,
				unsigned int virq,
				unsigned int nr_bm_irqs,
				unsigned int nr_dom_irqs)
{
	struct hv_pci_chip_data *chip_data = domain->host_data;
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	int first = d->hwirq - HV_PCI_MSI_SPI_START;
	int i;

	mutex_lock(&chip_data->map_lock);
	bitmap_release_region(chip_data->spi_map,
			      first,
			      get_count_order(nr_bm_irqs));
	mutex_unlock(&chip_data->map_lock);
	for (i = 0; i < nr_dom_irqs; i++) {
		if (i)
			d = irq_domain_get_irq_data(domain, virq + i);
		irq_domain_reset_irq_data(d);
	}

	irq_domain_free_irqs_parent(domain, virq, nr_dom_irqs);
}

/* [한국어]
 * hv_pci_vec_irq_domain_free - ARM64 갈래. IRQ 도메인의 free 콜백
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @virq: 놓을 첫 리눅스 IRQ 번호.
 * @nr_irqs: 놓을 개수.
 * @return: 없음.
 *
 * **한 줄짜리 래퍼다.** IRQ 도메인 규약이 요구하는 시그니처에 맞추어
 * hv_pci_vec_irq_free() 를 부르며, **두 개수 인자에 같은 값을 넘긴다** --
 * 정상적인 해제에서는 비트맵과 도메인이 늘 같은 수이기 때문이다.
 *
 * 따로 두는 이유는 그 함수가 실패 경로에서 다른 조합으로도 불려야 해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 코어의 해제 경로).
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> hv_pci_vec_irq_free()
 */
static void hv_pci_vec_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq,
				       unsigned int nr_irqs)
{
	hv_pci_vec_irq_free(domain, virq, nr_irqs, nr_irqs);
}

/* [한국어]
 * hv_pci_vec_alloc_device_irq - ARM64 갈래. 비트맵에서 SPI 번호를 잡는다
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @nr_irqs: 필요한 연속 개수.
 * @hwirq: 잡은 첫 SPI 번호를 담아 돌려줄 자리.
 * @return: 성공 0, 남은 자리가 없으면 -ENOSPC.
 *
 * **MSI 는 연속한 벡터를 요구하므로 비트맵에서 연속 구간을 찾아야 한다.**
 * bitmap_find_free_region() 이 그 일을 하며, **2의 거듭제곱 단위로만
 * 찾으므로** get_count_order() 로 개수를 지수로 바꿔 넘긴다.
 *
 * 찾은 색인에 HV_PCI_MSI_SPI_START 를 더해 실제 SPI 번호로 옮긴다 --
 * 이 도메인이 관리하는 범위가 64번부터이기 때문이다.
 *
 * **해제 쪽 hv_pci_vec_irq_free() 와 정확히 대칭이다** --
 * 그쪽은 같은 지수로 bitmap_release_region 을 부르고 START 를 뺀다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   hv_pci_vec_irq_domain_alloc -> [이 함수] -> bitmap_find_free_region()
 */
static int hv_pci_vec_alloc_device_irq(struct irq_domain *domain,
				       unsigned int nr_irqs,
				       irq_hw_number_t *hwirq)
{
	struct hv_pci_chip_data *chip_data = domain->host_data;
	int index;

	/* Find and allocate region from the SPI bitmap */
	mutex_lock(&chip_data->map_lock);
	index = bitmap_find_free_region(chip_data->spi_map,
					HV_PCI_MSI_SPI_NR,
					get_count_order(nr_irqs));
	mutex_unlock(&chip_data->map_lock);
	if (index < 0)
		return -ENOSPC;

	*hwirq = index + HV_PCI_MSI_SPI_START;

	return 0;
}

/* [한국어]
 * hv_pci_vec_irq_gic_domain_alloc - ARM64 갈래. 부모 GIC 도메인에 IRQ 하나를 요청한다
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @virq: 리눅스 IRQ 번호.
 * @hwirq: 요청할 SPI 번호.
 * @return: 성공 0, 실패면 음수.
 *
 * **계층 IRQ 도메인의 규약을 따르는 함수다.** 이 도메인이 SPI 번호를
 * 정했으면 부모(GIC)에게 그 번호로 실제 인터럽트를 만들어 달라고 해야 한다.
 *
 * **부모를 부르는 인자 형식이 펌웨어 종류에 따라 갈린다.**
 * - **device tree 라면** 인자가 셋이다 -- 종류(0=SPI), 번호, 트리거 방식.
 *   **번호에서 32 를 빼는 것이 요점이다** -- device tree 의 SPI 번호는
 *   GIC 의 전역 번호에서 32(SGI 16 + PPI 16)를 뺀 값이기 때문이다.
 * - **ACPI 라면** 인자가 둘이다 -- 번호와 트리거 방식. 빼기가 없다.
 *
 * 부모가 만들어 준 뒤 **트리거 방식을 다시 한 번 명시적으로 설정한다** --
 * fwspec 으로 넘긴 값이 반영되지 않는 경우를 대비한 것으로 보이나,
 * 그 사정이 코드에 적혀 있지는 않다.
 *
 * **MSI 는 늘 에지 트리거다** -- 메시지가 도착하는 순간이 곧 인터럽트이므로
 * 레벨이라는 개념이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 할당 경로).
 *
 * 호출 체인:
 *   hv_pci_vec_irq_domain_alloc -> [이 함수] -> irq_domain_alloc_irqs_parent()
 */
static int hv_pci_vec_irq_gic_domain_alloc(struct irq_domain *domain,
					   unsigned int virq,
					   irq_hw_number_t hwirq)
{
	struct irq_fwspec fwspec;
	struct irq_data *d;
	int ret;

	fwspec.fwnode = domain->parent->fwnode;
	if (is_of_node(fwspec.fwnode)) {
		/* SPI lines for OF translations start at offset 32 */
		fwspec.param_count = 3;
		fwspec.param[0] = 0;
		fwspec.param[1] = hwirq - 32;
		fwspec.param[2] = IRQ_TYPE_EDGE_RISING;
	} else {
		fwspec.param_count = 2;
		fwspec.param[0] = hwirq;
		fwspec.param[1] = IRQ_TYPE_EDGE_RISING;
	}

	ret = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (ret)
		return ret;

	/*
	 * Since the interrupt specifier is not coming from ACPI or DT, the
	 * trigger type will need to be set explicitly. Otherwise, it will be
	 * set to whatever is in the GIC configuration.
	 */
	d = irq_domain_get_irq_data(domain->parent, virq);

	return d->chip->irq_set_type(d, IRQ_TYPE_EDGE_RISING);
}

/* [한국어]
 * hv_pci_vec_irq_domain_alloc - ARM64 갈래. IRQ 도메인의 alloc 콜백
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @virq: 첫 리눅스 IRQ 번호.
 * @nr_irqs: 요청 개수.
 * @args: 쓰이지 않는다.
 * @return: 성공 0, 실패면 음수.
 *
 * **두 단계로 나뉜다** -- 먼저 비트맵에서 연속한 SPI 번호를 잡고,
 * 그다음 하나씩 부모 GIC 도메인에 만들어 달라고 요청한다.
 *
 * **실패 처리가 이 함수의 요점이다.** 중간에 실패하면
 * `hv_pci_vec_irq_free(domain, virq, nr_irqs, i)` 를 부르는데,
 * **비트맵은 nr_irqs 만큼 통째로 되돌리고 도메인은 성공한 i 개만 놓는다.**
 * 그래서 그 함수의 개수 인자가 둘로 나뉘어 있다.
 *
 * 만들 때마다 irq_domain_set_hwirq_and_chip() 으로 이 계층의 irq_chip 을
 * 붙인다 -- hv_arm64_msi_irq_chip 이며, 그 콜백들은 모두
 * irq_chip_..._parent 로 부모에 그대로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 할당 경로).
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수]
 *     -> hv_pci_vec_alloc_device_irq(), hv_pci_vec_irq_gic_domain_alloc()
 */
static int hv_pci_vec_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	irq_hw_number_t hwirq;
	unsigned int i;
	int ret;

	ret = hv_pci_vec_alloc_device_irq(domain, nr_irqs, &hwirq);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = hv_pci_vec_irq_gic_domain_alloc(domain, virq + i,
						      hwirq + i);
		if (ret) {
			hv_pci_vec_irq_free(domain, virq, nr_irqs, i);
			return ret;
		}

		irq_domain_set_hwirq_and_chip(domain, virq + i,
					      hwirq + i,
					      &hv_arm64_msi_irq_chip,
					      domain->host_data);
		pr_debug("pID:%d vID:%u\n", (int)(hwirq + i), virq + i);
	}

	return 0;
}

/*
 * Pick the first cpu as the irq affinity that can be temporarily used for
 * composing MSI from the hypervisor. GIC will eventually set the right
 * affinity for the irq and the 'unmask' will retarget the interrupt to that
 * cpu.
 */
/* [한국어]
 * hv_pci_vec_irq_domain_activate - ARM64 갈래. 인터럽트의 유효 친화도를 정한다
 *
 * @domain: 이 드라이버가 만든 MSI 도메인.
 * @irqd: 활성화할 인터럽트.
 * @reserve: 쓰이지 않는다.
 * @return: 늘 0.
 *
 * **현재 존재하는 첫 CPU 하나로 친화도를 고정한다.**
 * cpumask_first(cpu_present_mask) 가 그 CPU 이며, 보통 0번이다.
 *
 * **왜 하나로 고정하는가**: 이 도메인 아래의 MSI 는 GIC 의 SPI 로
 * 전달되는데, SPI 는 특정 CPU 로 라우팅되어야 하기 때문이다.
 * **그래서 이 갈래에서는 인터럽트 부하가 CPU 에 흩어지지 않는다** --
 * 그 결과가 코드에 적혀 있지는 않다.
 *
 * **online 이 아니라 present 마스크를 보는 것이 눈에 띈다** --
 * 아직 온라인이 아닌 CPU 도 후보가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 활성화 경로).
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> irq_data_update_effective_affinity()
 */
static int hv_pci_vec_irq_domain_activate(struct irq_domain *domain,
					  struct irq_data *irqd, bool reserve)
{
	int cpu = cpumask_first(cpu_present_mask);

	irq_data_update_effective_affinity(irqd, cpumask_of(cpu));

	return 0;
}

static const struct irq_domain_ops hv_pci_domain_ops = {
	.alloc	= hv_pci_vec_irq_domain_alloc,
	.free	= hv_pci_vec_irq_domain_free,
	.activate = hv_pci_vec_irq_domain_activate,
};

#ifdef CONFIG_OF

/* [한국어]
 * hv_pci_of_irq_domain_parent - ARM64 갈래. device tree 에서 부모 IRQ 도메인을 찾는다
 *
 * @return: 찾은 도메인, 없으면 NULL.
 *
 * **VMBus 루트 장치의 device tree 노드에서 위로 거슬러 인터럽트 부모를 찾는다.**
 * of_irq_find_parent() 가 그 노드의 interrupt-parent 속성을 따라가며,
 * 그 끝에 GIC 노드가 있다.
 *
 * 찾은 노드로 irq_find_host() 를 불러 그 노드에 등록된 IRQ 도메인을 얻는다.
 *
 * **of_node_put 을 반드시 부른다** -- of_irq_find_parent 가 참조를 올려
 * 돌려주기 때문이다. 도메인을 얻은 뒤에 놓으므로 순서가 맞다.
 *
 * **CONFIG_OF 안에서만 정의된다.** ACPI 로 부팅하는 시스템에는 device tree 가
 * 없으므로 아래의 ACPI 판이 쓰인다.
 *
 * **hv_get_vmbus_root_device() 의 정의는 drivers/hv 에 있고 이 트리에 없다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:
 *   hv_pci_irqchip_init -> [이 함수] -> of_irq_find_parent(), irq_find_host()
 */
static struct irq_domain *hv_pci_of_irq_domain_parent(void)
{
	struct device_node *parent;
	struct irq_domain *domain;

	parent = of_irq_find_parent(hv_get_vmbus_root_device()->of_node);
	if (!parent)
		return NULL;
	domain = irq_find_host(parent);
	of_node_put(parent);

	return domain;
}

#endif

#ifdef CONFIG_ACPI

/* [한국어]
 * hv_pci_acpi_irq_domain_parent - ARM64 갈래. ACPI 에서 부모 IRQ 도메인을 찾는다
 *
 * @return: 찾은 도메인, 없으면 NULL.
 *
 * **ACPI 로 부팅한 시스템에서 device tree 판을 대신한다.**
 * GSI(Global System Interrupt) 번호를 도메인으로 옮겨 주는 함수를
 * ACPI 계층에서 얻어, 0번 GSI 로 물어본 결과의 fwnode 를 쓴다.
 *
 * **0 을 넘기는 것이 요점이다** -- 특정 GSI 의 도메인이 필요한 것이 아니라
 * **아무 GSI 나 물어보아 그것을 담당하는 도메인(곧 GIC)을 찾는 것** 이다.
 *
 * irq_find_matching_fwnode(..., DOMAIN_BUS_ANY) 로 그 fwnode 에 등록된
 * 도메인을 얻는다.
 *
 * **디스패처가 없으면 NULL 을 돌려준다** -- ACPI 가 인터럽트 정보를 주지
 * 않는 구성이며, 그때는 호출자가 device tree 판을 시도한다.
 *
 * **CONFIG_ACPI 안에서만 정의된다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:
 *   hv_pci_irqchip_init -> [이 함수] -> acpi_get_gsi_dispatcher()
 */
static struct irq_domain *hv_pci_acpi_irq_domain_parent(void)
{
	acpi_gsi_domain_disp_fn gsi_domain_disp_fn;

	gsi_domain_disp_fn = acpi_get_gsi_dispatcher();
	if (!gsi_domain_disp_fn)
		return NULL;
	return irq_find_matching_fwnode(gsi_domain_disp_fn(0),
				     DOMAIN_BUS_ANY);
}

#endif

/* [한국어] 
 *  * hv_pci_irqchip_init - ARM64 갈래. GIC 위에 MSI IRQ 도메인을 만든다
 *  *
 *  * @return: 성공 0, 실패면 음수.
 *  *
 *  * **x86 과 달리 ARM64 에는 바로 쓸 수 있는 벡터 도메인이 없어**
 *  * 이 드라이버가 GIC 위에 계층 도메인을 직접 세운다.
 *  *
 *  * 절차가 넷이다.
 *  * 1. **SPI 비트맵을 담을 chip_data 를 만든다.** 이 도메인이 배정할 수 있는
 *  *    SPI(Shared Peripheral Interrupt) 번호를 비트맵으로 관리한다.
 *  * 2. fwnode 를 만들어 도메인에 이름을 준다.
 *  * 3. **부모 도메인을 찾는다.** ACPI 가 있으면 그쪽에서, 없으면 device tree
 *  *    에서 GIC 도메인을 찾는다. 둘 다 없으면 펌웨어 구성이 잘못된 것이므로
 *  *    WARN_ONCE 로 알리고 물러난다.
 *  * 4. irq_domain_create_hierarchy() 로 그 위에 계층 도메인을 만든다.
 *  *
 *  * **원문 주석이 밝히듯 한 번 만든 도메인은 없애지 않는다** --
 *  * 그것을 쓰는 장치가 모두 사라졌고 인터럽트가 더 오지 않는다는 것을
 *  * 보장할 방법이 없기 때문이다. 그래서 성공 경로에 해제가 없다.
 *  *
 *  * **chip_data 가 static 인 것이 눈에 띈다.** 함수 안 static 이라
 *  * 값이 다음 호출까지 남지만, 이 함수는 모듈 초기화 때 한 번만 불린다.
 *  *
 *  * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화). kzalloc 이 있어 잠들 수 있다.
 *  *
 *  * 호출 체인:
 *   *   init_hv_pci_drv -> [이 함수]
 *   *     -> hv_pci_acpi_irq_domain_parent() / hv_pci_of_irq_domain_parent(),
 *   *        irq_domain_create_hierarchy() */
static int hv_pci_irqchip_init(void)
{
	static struct hv_pci_chip_data *chip_data;
	struct fwnode_handle *fn = NULL;
	struct irq_domain *irq_domain_parent = NULL;
	int ret = -ENOMEM;

	chip_data = kzalloc_obj(*chip_data);
	if (!chip_data)
		return ret;

	mutex_init(&chip_data->map_lock);
	fn = irq_domain_alloc_named_fwnode("hv_vpci_arm64");
	if (!fn)
		goto free_chip;

	/*
	 * IRQ domain once enabled, should not be removed since there is no
	 * way to ensure that all the corresponding devices are also gone and
	 * no interrupts will be generated.
	 */
#ifdef CONFIG_ACPI
	if (!acpi_disabled)
		irq_domain_parent = hv_pci_acpi_irq_domain_parent();
#endif
#ifdef CONFIG_OF
	if (!irq_domain_parent)
		irq_domain_parent = hv_pci_of_irq_domain_parent();
#endif
	if (!irq_domain_parent) {
		WARN_ONCE(1, "Invalid firmware configuration for VMBus interrupts\n");
		ret = -EINVAL;
		goto free_chip;
	}

	hv_msi_gic_irq_domain = irq_domain_create_hierarchy(irq_domain_parent, 0,
		HV_PCI_MSI_SPI_NR,
		fn, &hv_pci_domain_ops,
		chip_data);

	if (!hv_msi_gic_irq_domain) {
		pr_err("Failed to create Hyper-V arm64 vPCI MSI IRQ domain\n");
		goto free_chip;
	}

	return 0;

free_chip:
	kfree(chip_data);
	if (fn)
		irq_domain_free_fwnode(fn);

	return ret;
}

/* [한국어] 
 *  * hv_pci_get_root_domain - ARM64 갈래. 이 파일이 만든 MSI 도메인을 돌려준다
 *  *
 *  * @return: hv_msi_gic_irq_domain.
 *  *
 *  * **hv_pci_irqchip_init() 이 만들어 전역에 담아 둔 도메인이다.**
 *  * x86 갈래가 커널의 x86_vector_domain 을 그대로 쓰는 것과 달리,
 *  * ARM64 에서는 이 드라이버가 세운 것을 쓴다.
 *  *
 *  * **초기화가 실패했으면 NULL 이 나간다** -- 그 경우 모듈 초기화 자체가
 *  * 앞서 물러나므로 실제로는 닿지 않는다.
 *  *
 *  * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 도메인 생성 경로).
 *  *
 *  * 호출 체인:
 *   *   hv_pcie_init_irq_domain -> [이 함수] */
static struct irq_domain *hv_pci_get_root_domain(void)
{
	return hv_msi_gic_irq_domain;
}

/*
 * SPIs are used for interrupts of PCI devices and SPIs is managed via GICD
 * registers which Hyper-V already supports, so no hypercall needed.
 */
/* [한국어] 
 *  * hv_arch_irq_unmask - ARM64 갈래. 할 일이 없다
 *  *
 *  * @data: 인터럽트를 나타내는 irq_data.
 *  * @return: 없음.
 *  *
 *  * **빈 함수다.** GIC 가 마스크와 대상 CPU 를 스스로 다루므로
 *  * 이 계층이 하이퍼바이저에 따로 알릴 것이 없다.
 *  *
 *  * **x86 갈래는 같은 이름으로 하이퍼콜을 내는 스무 줄짜리 함수다** --
 *  * 그쪽은 게스트가 인터럽트 라우팅을 직접 바꿀 수 없어
 *  * 마스크 해제 때마다 호스트에 대상을 다시 알려야 한다.
 *  *
 *  * **두 갈래를 같은 이름으로 둔 덕에 hv_irq_unmask() 는 아키텍처를 몰라도 된다** --
 *  * 조건부 컴파일을 호출자마다 두지 않으려는 설계다.
 *  *
 *  * 실행 컨텍스트: IRQ 코어의 unmask 경로.
 *  *
 *  * 호출 체인:
 *   *   hv_irq_unmask -> [이 함수] */
static void hv_arch_irq_unmask(struct irq_data *data) { }
#endif /* CONFIG_ARM64 */

/**
 * hv_pci_generic_compl() - Invoked for a completion packet
 * @context:		Set up by the sender of the packet.
 * @resp:		The response packet
 * @resp_packet_size:	Size in bytes of the packet
 *
 * This function is used to trigger an event and report status
 * for any message for which the completion packet contains a
 * status and nothing else.
 */
/* [한국어]
 * hv_pci_generic_compl - 상태만 담긴 응답을 받아 요청자를 깨운다
 *
 * @context: 요청을 보낼 때 넘겨 둔 struct hv_pci_compl.
 * @resp: 호스트가 돌려준 응답 패킷.
 * @resp_packet_size: 응답 크기. **이 함수는 쓰지 않는다.**
 * @return: 없음.
 *
 * **이 파일의 요청-응답 대응 구조에서 가장 단순한 완료 함수다.**
 * 상류 주석이 밝히듯 **응답에 상태 말고 아무것도 없는 메시지들이 이것을 쓴다.**
 *
 * 동작은 둘뿐이다 -- 상태를 문맥에 옮겨 담고, completion 을 깨운다.
 * 그러면 wait_for_response() 에서 잠들어 있던 요청자가 깨어나
 * comp_pkt->completion_status 로 결과를 확인한다.
 *
 * **요청자가 이 함수를 어떻게 지정하는가**: struct pci_packet 의
 * completion_func 에 이 포인터를, compl_ctxt 에 hv_pci_compl 주소를 넣어
 * vmbus_sendpacket 에 함께 넘긴다. 응답이 오면
 * hv_pci_onchannelcallback() 이 그 둘을 되찾아 부른다.
 *
 * **문맥이 스택 위에 있는 경우가 많다** -- 요청자가 깨어나기 전에
 * 이 함수가 그것을 건드리므로, 요청자는 completion 을 기다리는 동안
 * 그 변수를 살려 두어야 한다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> complete()
 */
static void hv_pci_generic_compl(void *context, struct pci_response *resp,
				 int resp_packet_size)
{
	struct hv_pci_compl *comp_pkt = context;

	comp_pkt->completion_status = resp->status;
	complete(&comp_pkt->host_event);
}

static struct hv_pci_dev *get_pcichild_wslot(struct hv_pcibus_device *hbus,
						u32 wslot);

/* [한국어]
 * get_pcichild - 장치 구조체의 참조를 하나 올린다
 *
 * @hpdev: 참조를 올릴 장치.
 * @return: 없음.
 *
 * **hv_pci_dev 는 여러 곳에서 동시에 쓰이므로 참조 계수로 수명을 관리한다** --
 * children 목록, 워크큐에 걸린 작업, 그리고 그 순간 그것을 다루는 코드가
 * 각각 참조를 쥔다.
 *
 * **refcount_t 를 쓰는 것이 요점이다.** atomic_t 와 달리 0 에서 올리거나
 * 넘침이 생기면 커널이 경고한다 -- use-after-free 를 막는 장치다.
 *
 * **짝이 되는 put_pcichild() 가 0 이 되면 해제한다.**
 *
 * 실행 컨텍스트: 어디서든. 원자적 연산 하나뿐이다.
 *
 * 호출 체인:
 *   get_pcichild_wslot / new_pcichild_device / pci_devices_present_work 등
 *     -> [이 함수] -> refcount_inc()
 */
static void get_pcichild(struct hv_pci_dev *hpdev)
{
	refcount_inc(&hpdev->refs);
}

/* [한국어]
 * put_pcichild - 장치 구조체의 참조를 놓고, 0 이 되면 해제한다
 *
 * @hpdev: 참조를 놓을 장치.
 * @return: 없음.
 *
 * **get_pcichild() 의 짝이다.** refcount_dec_and_test 가 하나 내린 뒤
 * 0 이 되었는지 원자적으로 알려 주므로, 그때만 kfree 한다.
 *
 * **두 연산을 나누면 안 되는 이유**: 내리는 것과 0 인지 보는 것 사이에
 * 다른 CPU 가 올렸다 내리면 두 번 해제하게 된다. 그래서 한 연산으로 묶은
 * 전용 함수가 있다.
 *
 * **hbus 는 놓지 않는다** -- hv_pci_dev 가 hbus 포인터를 들고 있지만
 * 참조를 쥐지는 않는다. 버스가 장치보다 오래 산다는 전제다.
 *
 * 실행 컨텍스트: 어디서든. 다만 해제가 일어나면 kfree 가 불린다.
 *
 * 호출 체인:
 *   이 파일에서 get_pcichild 로 참조를 올린 모든 자리의 짝
 *     -> [이 함수] -> refcount_dec_and_test(), kfree()
 */
static void put_pcichild(struct hv_pci_dev *hpdev)
{
	if (refcount_dec_and_test(&hpdev->refs))
		kfree(hpdev);
}

/*
 * There is no good way to get notified from vmbus_onoffer_rescind(),
 * so let's use polling here, since this is not a hot path.
 */
/* [한국어]
 * wait_for_response - 호스트의 응답을 기다리되 장치가 사라지면 물러난다
 *
 * @hdev: VMBus 장치. 채널이 살아 있는지 여기서 본다.
 * @comp: 기다릴 completion.
 * @return: 응답이 오면 0, 장치가 사라졌으면 -ENODEV.
 *
 * **단순히 completion 을 기다리지 않고 0.1초씩 끊어 기다리는 이유가
 * 상류 주석에 있다** -- VMBus 채널이 회수(rescind)되었다는 것을 알려 주는
 * 좋은 방법이 없어서 폴링으로 확인한다. 그 주석이 밝히듯
 * **이 경로는 자주 도는 곳이 아니므로** 폴링의 비용이 문제되지 않는다.
 *
 * **채널이 회수되면 응답이 영영 오지 않는다.** 호스트가 장치를 떼어 갔는데
 * 그것을 모른 채 기다리면 그 문맥이 영원히 잠든다. 그래서 매 회 깨어나
 * hdev->channel->rescind 를 확인한다.
 *
 * **dev_warn_once 를 쓴다** -- 여러 요청이 동시에 기다리다 함께 깨어나면
 * 같은 메시지가 쏟아지기 때문이다.
 *
 * **wait_for_completion_timeout 은 남은 시간을 돌려주므로 0 이 아니면
 * 응답이 온 것이다.** 0 이면 시간이 다 된 것이고, 그때 루프를 한 번 더 돈다.
 *
 * **이 파일의 거의 모든 동기 요청이 이 함수로 응답을 기다린다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   hv_pci_protocol_negotiation / hv_pci_enter_d0 / hv_pci_query_relations /
 *   hv_send_resources_allocated / hv_read_config_block 등
 *     -> [이 함수] -> wait_for_completion_timeout()
 */
static int wait_for_response(struct hv_device *hdev,
			     struct completion *comp)
{
	while (true) {
		if (hdev->channel->rescind) {
			dev_warn_once(&hdev->device, "The device is gone.\n");
			return -ENODEV;
		}

		if (wait_for_completion_timeout(comp, HZ / 10))
			break;
	}

	return 0;
}

/**
 * devfn_to_wslot() - Convert from Linux PCI slot to Windows
 * @devfn:	The Linux representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Windows representation
 */
/* [한국어]
 * devfn_to_wslot - 리눅스의 devfn 을 Windows 의 슬롯 표현으로 옮긴다
 *
 * @devfn: 리눅스 표현. 상위 5비트가 장치, 하위 3비트가 함수다.
 * @return: Windows 표현.
 *
 * **호스트가 Windows 쪽 표현을 쓰기 때문에 필요한 변환이다.**
 * 상류 주석이 그 사정을 밝힌다.
 *
 * **union win_slot_encoding 이 그 표현이다** -- 비트필드로 dev 5비트,
 * func 3비트, 나머지 24비트 예약을 두고, 그것을 u32 하나로 겹쳐 놓았다.
 * 곧 **리눅스 devfn 과 비트 배치가 같지만 32비트로 넓혀 놓은 것이며**,
 * 나머지를 0 으로 두어야 하므로 먼저 slot 을 0 으로 지운다.
 *
 * **union 을 쓰는 것이 요점이다** -- 비트필드로 채우고 u32 로 꺼내면
 * 바이트 배치가 호스트가 기대하는 대로 맞는다.
 *
 * **이 파일에서 호스트에 보내는 거의 모든 메시지가 이 값을 싣는다** --
 * 어느 장치에 대한 요청인지 알려야 하기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산이다.
 *
 * 호출 체인:
 *   hv_read_config_block / hv_compose_msi_msg / hv_eject_device_work 등
 *     -> [이 함수]
 */
static u32 devfn_to_wslot(int devfn)
{
	union win_slot_encoding wslot;

	wslot.slot = 0;
	wslot.bits.dev = PCI_SLOT(devfn);
	wslot.bits.func = PCI_FUNC(devfn);

	return wslot.slot;
}

/**
 * wslot_to_devfn() - Convert from Windows PCI slot to Linux
 * @wslot:	The Windows representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Linux representation
 */
/* [한국어]
 * wslot_to_devfn - Windows 의 슬롯 표현을 리눅스의 devfn 으로 옮긴다
 *
 * @wslot: Windows 표현.
 * @return: 리눅스 표현.
 *
 * **devfn_to_wslot() 의 반대 방향이다.** 호스트가 보낸 메시지에 실린
 * 슬롯 번호를 리눅스 PCI 코어가 아는 형태로 바꾼다.
 *
 * union 으로 u32 를 받아 비트필드로 읽고, PCI_DEVFN 매크로로 다시 합친다.
 *
 * **호스트가 먼저 보내는 알림을 처리할 때 늘 쓰인다** --
 * 장치 목록 변경, eject 요청, 설정 블록 무효화가 모두 wslot 을 실어 오므로
 * 그것으로 리눅스 쪽 장치를 찾아야 한다.
 *
 * **예약 24비트를 확인하지 않는다** -- 호스트가 0 이 아닌 값을 실어 보내도
 * 그대로 무시된다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산이다.
 *
 * 호출 체인:
 *   hv_pci_assign_slots / pci_devices_present_work / hv_eject_device_work 등
 *     -> [이 함수]
 */
static int wslot_to_devfn(u32 wslot)
{
	union win_slot_encoding slot_no;

	slot_no.slot = wslot;
	return PCI_DEVFN(slot_no.bits.dev, slot_no.bits.func);
}

/* [한국어]
 * hv_pci_read_mmio - 하이퍼콜로 MMIO 를 읽는다
 *
 * @dev: 오류 로그를 남길 장치.
 * @gpa: 읽을 게스트 물리 주소.
 * @size: 읽을 바이트 수(1, 2, 또는 4).
 * @val: 읽은 값을 담아 돌려줄 자리.
 * @return: 없음.
 *
 * **MMIO 를 직접 읽지 않고 하이퍼바이저에게 대신 읽어 달라고 한다.**
 * 설정공간 창이 게스트에게 직접 매핑되지 않는 구성에서 쓰이며,
 * hbus->use_calls 가 그 갈래를 정한다.
 *
 * **per-CPU 입력 페이지를 입력과 출력으로 나눠 쓴다** -- 입력 구조체 뒤에
 * 출력 구조체를 이어 붙이는 형태다. 상류 주석이 밝히듯
 * **인터럽트를 막은 채로 불려야 한다** -- 그 사이 다른 CPU 로 옮겨 가면
 * 남의 버퍼를 쓰게 되기 때문이다.
 *
 * **크기별로 다른 폭으로 꺼낸다** -- 1이면 u8, 2면 u16, 그 밖이면 u32 다.
 * default 갈래가 4를 겸하므로 3 같은 값도 4로 다뤄진다.
 *
 * **실패하면 val 을 건드리지 않는다** -- 호출자가 넘긴 변수의 값이 그대로
 * 남으며, 이 파일의 호출자들은 그 전에 0xFFFFFFFF 로 채워 두지 않는다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **HVCALL_MMIO_READ 와 hv_mmio_read_input 의 정의는 이 트리에 없다.**
 *
 * 실행 컨텍스트: 인터럽트를 막은 상태여야 한다.
 *
 * 호출 체인:
 *   _hv_pcifront_read_config -> [이 함수] -> hv_do_hypercall()
 */
static void hv_pci_read_mmio(struct device *dev, phys_addr_t gpa, int size, u32 *val)
{
	struct hv_mmio_read_input *in;
	struct hv_mmio_read_output *out;
	u64 ret;

	/*
	 * Must be called with interrupts disabled so it is safe
	 * to use the per-cpu input argument page.  Use it for
	 * both input and output.
	 */
	in = *this_cpu_ptr(hyperv_pcpu_input_arg);
	out = *this_cpu_ptr(hyperv_pcpu_input_arg) + sizeof(*in);
	in->gpa = gpa;
	in->size = size;

	ret = hv_do_hypercall(HVCALL_MMIO_READ, in, out);
	if (hv_result_success(ret)) {
		switch (size) {
		case 1:
			*val = *(u8 *)(out->data);
			break;
		case 2:
			*val = *(u16 *)(out->data);
			break;
		default:
			*val = *(u32 *)(out->data);
			break;
		}
	} else
		dev_err(dev, "MMIO read hypercall error %llx addr %llx size %d\n",
				ret, gpa, size);
}

/* [한국어]
 * hv_pci_write_mmio - 하이퍼콜로 MMIO 에 쓴다
 *
 * @dev: 오류 로그를 남길 장치.
 * @gpa: 쓸 게스트 물리 주소.
 * @size: 쓸 바이트 수(1, 2, 또는 4).
 * @val: 쓸 값.
 * @return: 없음.
 *
 * **hv_pci_read_mmio() 의 짝이며 구조가 같다.**
 * 다만 출력 구조체가 없어 per-CPU 페이지를 입력으로만 쓴다.
 *
 * **크기별로 다른 폭으로 입력 버퍼에 넣는다** -- 읽기 쪽이 꺼내는 것과
 * 정확히 대칭이다.
 *
 * **실패해도 알리지 않는다(void)** -- 로그만 남기고 호출자는 알 수 없다.
 * 설정공간 쓰기가 실패한 것을 위층에 전할 방법이 없는 셈이다.
 *
 * **여기서도 인터럽트를 막은 채로 불려야 한다** -- 상류 주석이 그것을 밝힌다.
 *
 * 실행 컨텍스트: 인터럽트를 막은 상태여야 한다.
 *
 * 호출 체인:
 *   _hv_pcifront_read_config / _hv_pcifront_write_config
 *     -> [이 함수] -> hv_do_hypercall()
 */
static void hv_pci_write_mmio(struct device *dev, phys_addr_t gpa, int size, u32 val)
{
	struct hv_mmio_write_input *in;
	u64 ret;

	/*
	 * Must be called with interrupts disabled so it is safe
	 * to use the per-cpu input argument memory.
	 */
	in = *this_cpu_ptr(hyperv_pcpu_input_arg);
	in->gpa = gpa;
	in->size = size;
	switch (size) {
	case 1:
		*(u8 *)(in->data) = val;
		break;
	case 2:
		*(u16 *)(in->data) = val;
		break;
	default:
		*(u32 *)(in->data) = val;
		break;
	}

	ret = hv_do_hypercall(HVCALL_MMIO_WRITE, in, NULL);
	if (!hv_result_success(ret))
		dev_err(dev, "MMIO write hypercall error %llx addr %llx size %d\n",
				ret, gpa, size);
}

/*
 * PCI Configuration Space for these root PCI buses is implemented as a pair
 * of pages in memory-mapped I/O space.  Writing to the first page chooses
 * the PCI function being written or read.  Once the first page has been
 * written to, the following page maps in the entire configuration space of
 * the function.
 */

/**
 * _hv_pcifront_read_config() - Internal PCI config read
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	Pointer to the buffer receiving the data
 */
/* [한국어]
 * _hv_pcifront_read_config - 설정공간을 읽되 일부 필드는 흉내로 답한다
 *
 * @hpdev: 이 드라이버가 아는 장치.
 * @where: 설정공간 안의 오프셋.
 * @size: 읽을 바이트 수.
 * @val: 읽은 값을 담아 돌려줄 자리.
 * @return: 없음.
 *
 * **설정공간 접근이 두 겹으로 나뉘는 것이 이 함수의 뼈대다** --
 * 어떤 필드는 호스트에 묻지 않고 **이 드라이버가 이미 아는 값으로 답하고**,
 * 나머지만 실제 MMIO 접근으로 내려간다.
 *
 * **흉내로 답하는 다섯 갈래.**
 * 1. **PCI_COMMAND 앞 -- 벤더/장치 ID.** 호스트가 장치 목록을 알려 줄 때
 *    함께 준 desc.v_id 에서 그대로 복사한다.
 * 2. **클래스 코드와 리비전.** desc.rev 부터의 바이트를 복사한다.
 * 3. **서브시스템 ID.** desc.subsystem_id 에서 복사한다.
 * 4. **ROM BAR -- 늘 0.** 원문 주석이 밝히듯 구현되지 않았다.
 * 5. **Interrupt Line 과 Interrupt PIN -- 늘 0.** 원문 주석이 그 이유를
 *    밝힌다 -- 이 프론트엔드는 메시지 신호 인터럽트만 지원하므로
 *    핀 기반 인터럽트를 광고해서는 안 된다.
 *
 * **나머지는 MMIO 로 내려가며, 거기서 다시 두 갈래다.**
 * - **use_calls 면** 하이퍼콜로 읽는다.
 * - **아니면** 매핑된 창에 직접 readb/readw/readl 한다.
 *
 * **설정공간 창이 두 페이지로 되어 있다.** 위쪽 원문 주석이 그 구조를
 * 밝힌다 -- **첫 페이지에 슬롯 번호를 쓰면 다음 페이지가 그 함수의
 * 설정공간으로 바뀐다.** 그래서 슬롯을 고르는 쓰기와 값을 읽는 읽기가
 * 반드시 짝으로 붙어 있어야 하고, **그 사이를 config_lock 스핀락이 지킨다.**
 *
 * **mb() 가 두 번 나온다.** 하나는 슬롯 선택이 읽기보다 먼저 하드웨어에
 * 닿게 하려는 것이고, 다른 하나는 읽기가 끝난 뒤에 락이 풀리게 하려는
 * 것이다 -- 원문 주석이 둘 다 그 뜻을 밝힌다.
 *
 * **창 밖을 읽으려 하면 오류를 남기고 val 을 건드리지 않는다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 irqsave 로 잡는다 --
 * 하이퍼콜 경로가 인터럽트를 막은 상태를 요구하기 때문이다.
 *
 * 호출 체인:
 *   hv_pcifront_read_config -> [이 함수]
 *     -> hv_pci_write_mmio()/hv_pci_read_mmio() 또는 writel()/readl()
 */
static void _hv_pcifront_read_config(struct hv_pci_dev *hpdev, int where,
				     int size, u32 *val)
{
	struct hv_pcibus_device *hbus = hpdev->hbus;
	struct device *dev = &hbus->hdev->device;
	int offset = where + CFG_PAGE_OFFSET;
	unsigned long flags;

	/*
	 * If the attempt is to read the IDs or the ROM BAR, simulate that.
	 */
	if (where + size <= PCI_COMMAND) {
		memcpy(val, ((u8 *)&hpdev->desc.v_id) + where, size);
	} else if (where >= PCI_CLASS_REVISION && where + size <=
		   PCI_CACHE_LINE_SIZE) {
		memcpy(val, ((u8 *)&hpdev->desc.rev) + where -
		       PCI_CLASS_REVISION, size);
	} else if (where >= PCI_SUBSYSTEM_VENDOR_ID && where + size <=
		   PCI_ROM_ADDRESS) {
		memcpy(val, (u8 *)&hpdev->desc.subsystem_id + where -
		       PCI_SUBSYSTEM_VENDOR_ID, size);
	} else if (where >= PCI_ROM_ADDRESS && where + size <=
		   PCI_CAPABILITY_LIST) {
		/* ROM BARs are unimplemented */
		*val = 0;
	} else if ((where >= PCI_INTERRUPT_LINE && where + size <= PCI_INTERRUPT_PIN) ||
		   (where >= PCI_INTERRUPT_PIN && where + size <= PCI_MIN_GNT)) {
		/*
		 * Interrupt Line and Interrupt PIN are hard-wired to zero
		 * because this front-end only supports message-signaled
		 * interrupts.
		 */
		*val = 0;
	} else if (where + size <= CFG_PAGE_SIZE) {

		spin_lock_irqsave(&hbus->config_lock, flags);
		if (hbus->use_calls) {
			phys_addr_t addr = hbus->mem_config->start + offset;

			hv_pci_write_mmio(dev, hbus->mem_config->start, 4,
						hpdev->desc.win_slot.slot);
			hv_pci_read_mmio(dev, addr, size, val);
		} else {
			void __iomem *addr = hbus->cfg_addr + offset;

			/* Choose the function to be read. (See comment above) */
			writel(hpdev->desc.win_slot.slot, hbus->cfg_addr);
			/* Make sure the function was chosen before reading. */
			mb();
			/* Read from that function's config space. */
			switch (size) {
			case 1:
				*val = readb(addr);
				break;
			case 2:
				*val = readw(addr);
				break;
			default:
				*val = readl(addr);
				break;
			}
			/*
			 * Make sure the read was done before we release the
			 * spinlock allowing consecutive reads/writes.
			 */
			mb();
		}
		spin_unlock_irqrestore(&hbus->config_lock, flags);
	} else {
		dev_err(dev, "Attempt to read beyond a function's config space.\n");
	}
}

/* [한국어]
 * hv_pcifront_get_vendor_id - 설정공간에서 벤더 ID 만 읽어 온다
 *
 * @hpdev: 이 드라이버가 아는 장치.
 * @return: 읽은 벤더 ID.
 *
 * **_hv_pcifront_read_config() 가 벤더 ID 를 흉내로 답하는 것과 달리,
 * 이 함수는 실제로 하드웨어(호스트)에 물어본다.**
 * 그 차이가 이 함수를 따로 둔 이유다 -- 장치가 아직 살아 있는지
 * 확인하려면 캐시된 값이 아니라 진짜 응답이 필요하다.
 *
 * **hv_eject_device_work() 가 이것을 쓴다** -- 뽑기 전에 장치가 아직
 * 응답하는지 확인하는 데 쓰는 것으로 보이나, 그 사정이 코드에 적혀 있지는 않다.
 *
 * 두 갈래는 읽기 쪽과 같다 -- use_calls 면 하이퍼콜, 아니면 직접 readw.
 *
 * **하이퍼콜 갈래에서 u32 로 받아 u16 에 넣는다.** 원문 주석이
 * "Truncates to 16 bits" 라 그것을 밝힌다.
 *
 * **직접 읽기 갈래에는 읽은 뒤의 mb() 가 없다.** 원문 주석이 그 이유를
 * 밝힌다 -- spin_unlock_irqrestore 자체가 장벽이기 때문이다.
 * **_hv_pcifront_read_config() 는 같은 자리에 mb() 를 두는데**,
 * 그쪽은 락 해제가 아직 남아 있는 구조라 차이가 생긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   hv_eject_device_work -> [이 함수]
 */
static u16 hv_pcifront_get_vendor_id(struct hv_pci_dev *hpdev)
{
	struct hv_pcibus_device *hbus = hpdev->hbus;
	struct device *dev = &hbus->hdev->device;
	u32 val;
	u16 ret;
	unsigned long flags;

	spin_lock_irqsave(&hbus->config_lock, flags);

	if (hbus->use_calls) {
		phys_addr_t addr = hbus->mem_config->start +
					 CFG_PAGE_OFFSET + PCI_VENDOR_ID;

		hv_pci_write_mmio(dev, hbus->mem_config->start, 4,
					hpdev->desc.win_slot.slot);
		hv_pci_read_mmio(dev, addr, 2, &val);
		ret = val;  /* Truncates to 16 bits */
	} else {
		void __iomem *addr = hbus->cfg_addr + CFG_PAGE_OFFSET +
					     PCI_VENDOR_ID;
		/* Choose the function to be read. (See comment above) */
		writel(hpdev->desc.win_slot.slot, hbus->cfg_addr);
		/* Make sure the function was chosen before we start reading. */
		mb();
		/* Read from that function's config space. */
		ret = readw(addr);
		/*
		 * mb() is not required here, because the
		 * spin_unlock_irqrestore() is a barrier.
		 */
	}

	spin_unlock_irqrestore(&hbus->config_lock, flags);

	return ret;
}

/**
 * _hv_pcifront_write_config() - Internal PCI config write
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	The data being transferred
 */
/* [한국어]
 * _hv_pcifront_write_config - 설정공간에 쓰되 읽기 전용 필드는 무시한다
 *
 * @hpdev: 이 드라이버가 아는 장치.
 * @where: 설정공간 안의 오프셋.
 * @size: 쓸 바이트 수.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * **_hv_pcifront_read_config() 의 짝이며 갈래가 더 단순하다.**
 *
 * **세 갈래로 나뉜다.**
 * 1. **서브시스템 ID 부터 Capability List 앞까지 -- 조용히 무시한다.**
 *    원문 주석이 밝히듯 SSID 와 ROM BAR 는 읽기 전용이다.
 *    **빈 블록이라 아무 일도 일어나지 않으며 오류도 알리지 않는다.**
 * 2. **PCI_COMMAND 부터 창 끝까지 -- 실제로 쓴다.**
 * 3. **그 밖 -- 오류를 남긴다.** 곧 PCI_COMMAND 앞쪽(벤더/장치 ID)에
 *    쓰려 해도 오류가 된다 -- 그 필드들은 읽기 전용이기 때문이다.
 *
 * **쓰기 갈래는 읽기와 같은 두 겹이다** -- use_calls 면 하이퍼콜,
 * 아니면 매핑된 창에 writeb/writew/writel.
 *
 * **장벽이 읽기 쪽과 다르다.** 슬롯 선택 뒤에 mb() 가 아니라 **wmb()** 를
 * 쓴다 -- 뒤따르는 것이 쓰기뿐이라 쓰기 순서만 보장하면 되기 때문이다.
 * 쓰기가 끝난 뒤에는 읽기 쪽과 같이 mb() 를 쓴다.
 *
 * **config_lock 이 여기서도 두 접근을 하나로 묶는다** -- 슬롯 선택과
 * 실제 쓰기 사이에 다른 문맥이 끼어들면 엉뚱한 함수에 쓰게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 irqsave 로 잡는다.
 *
 * 호출 체인:
 *   hv_pcifront_write_config -> [이 함수]
 *     -> hv_pci_write_mmio() 또는 writel()
 */
static void _hv_pcifront_write_config(struct hv_pci_dev *hpdev, int where,
				      int size, u32 val)
{
	struct hv_pcibus_device *hbus = hpdev->hbus;
	struct device *dev = &hbus->hdev->device;
	int offset = where + CFG_PAGE_OFFSET;
	unsigned long flags;

	if (where >= PCI_SUBSYSTEM_VENDOR_ID &&
	    where + size <= PCI_CAPABILITY_LIST) {
		/* SSIDs and ROM BARs are read-only */
	} else if (where >= PCI_COMMAND && where + size <= CFG_PAGE_SIZE) {
		spin_lock_irqsave(&hbus->config_lock, flags);

		if (hbus->use_calls) {
			phys_addr_t addr = hbus->mem_config->start + offset;

			hv_pci_write_mmio(dev, hbus->mem_config->start, 4,
						hpdev->desc.win_slot.slot);
			hv_pci_write_mmio(dev, addr, size, val);
		} else {
			void __iomem *addr = hbus->cfg_addr + offset;

			/* Choose the function to write. (See comment above) */
			writel(hpdev->desc.win_slot.slot, hbus->cfg_addr);
			/* Make sure the function was chosen before writing. */
			wmb();
			/* Write to that function's config space. */
			switch (size) {
			case 1:
				writeb(val, addr);
				break;
			case 2:
				writew(val, addr);
				break;
			default:
				writel(val, addr);
				break;
			}
			/*
			 * Make sure the write was done before we release the
			 * spinlock allowing consecutive reads/writes.
			 */
			mb();
		}
		spin_unlock_irqrestore(&hbus->config_lock, flags);
	} else {
		dev_err(dev, "Attempt to write beyond a function's config space.\n");
	}
}

/**
 * hv_pcifront_read_config() - Read configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be read
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
/* [한국어]
 * hv_pcifront_read_config - PCI 코어의 설정공간 읽기 콜백
 *
 * @bus: 리눅스 PCI 버스.
 * @devfn: 장치·함수 번호.
 * @where: 오프셋.
 * @size: 크기.
 * @val: 결과를 담을 자리.
 * @return: 성공 PCIBIOS_SUCCESSFUL, 장치가 없으면 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * **리눅스 PCI 코어가 이 버스의 설정공간을 읽을 때 부르는 함수다.**
 * hv_pcifront_ops 표에 등록되어 있다.
 *
 * 세 단계로 나뉜다.
 * 1. **버스에서 hbus 를 되찾는다.** container_of 로 sysdata 에서 계산한다 --
 *    struct hv_pcibus_device 의 첫 멤버가 sysdata 이기 때문이다.
 * 2. **devfn 을 Windows 표현으로 옮겨 장치를 찾는다.**
 *    찾으면 참조가 올라간 채 돌아오므로 반드시 놓아야 한다.
 * 3. 실제 읽기를 _hv_pcifront_read_config() 에 맡긴다.
 *
 * **장치를 못 찾으면 PCIBIOS_DEVICE_NOT_FOUND 를 돌려준다** --
 * 빈 슬롯을 읽으면 코어가 그것을 보고 장치가 없다고 판단한다.
 *
 * **val 을 채우지 않고 돌아가는 경로가 있다** -- 장치를 못 찾은 경우다.
 * PCI 코어가 그 반환값을 보고 val 을 쓰지 않으므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 열거·설정 경로).
 *
 * 호출 체인:
 *   PCI 코어 -> [이 함수]
 *     -> get_pcichild_wslot(), _hv_pcifront_read_config(), put_pcichild()
 */
static int hv_pcifront_read_config(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct hv_pcibus_device *hbus =
		container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_read_config(hpdev, where, size, val);

	put_pcichild(hpdev);
	return PCIBIOS_SUCCESSFUL;
}

/**
 * hv_pcifront_write_config() - Write configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be written to device
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
/* [한국어]
 * hv_pcifront_write_config - PCI 코어의 설정공간 쓰기 콜백
 *
 * @bus: 리눅스 PCI 버스.
 * @devfn: 장치·함수 번호.
 * @where: 오프셋.
 * @size: 크기.
 * @val: 쓸 값.
 * @return: 성공 PCIBIOS_SUCCESSFUL, 장치가 없으면 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * **hv_pcifront_read_config() 와 구조가 완전히 같고 방향만 다르다.**
 * 버스에서 hbus 를 되찾고, 장치를 찾아 참조를 쥐고, 실제 일을
 * _hv_pcifront_write_config() 에 맡긴 뒤 참조를 놓는다.
 *
 * **쓰기 실패를 알릴 방법이 없다** -- 아래층이 void 이므로 이 함수는
 * 장치를 찾았다는 사실만으로 PCIBIOS_SUCCESSFUL 을 돌려준다.
 * 읽기 전용 필드에 쓰려 해도 성공으로 보고된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 설정 경로).
 *
 * 호출 체인:
 *   PCI 코어 -> [이 함수]
 *     -> get_pcichild_wslot(), _hv_pcifront_write_config(), put_pcichild()
 */
static int hv_pcifront_write_config(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	struct hv_pcibus_device *hbus =
	    container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_write_config(hpdev, where, size, val);

	put_pcichild(hpdev);
	return PCIBIOS_SUCCESSFUL;
}

/* PCIe operations */
static struct pci_ops hv_pcifront_ops = {
	.read  = hv_pcifront_read_config,
	.write = hv_pcifront_write_config,
};

/*
 * Paravirtual backchannel
 *
 * Hyper-V SR-IOV provides a backchannel mechanism in software for
 * communication between a VF driver and a PF driver.  These
 * "configuration blocks" are similar in concept to PCI configuration space,
 * but instead of doing reads and writes in 32-bit chunks through a very slow
 * path, packets of up to 128 bytes can be sent or received asynchronously.
 *
 * Nearly every SR-IOV device contains just such a communications channel in
 * hardware, so using this one in software is usually optional.  Using the
 * software channel, however, allows driver implementers to leverage software
 * tools that fuzz the communications channel looking for vulnerabilities.
 *
 * The usage model for these packets puts the responsibility for reading or
 * writing on the VF driver.  The VF driver sends a read or a write packet,
 * indicating which "block" is being referred to by number.
 *
 * If the PF driver wishes to initiate communication, it can "invalidate" one or
 * more of the first 64 blocks.  This invalidation is delivered via a callback
 * supplied to the VF driver by this driver.
 *
 * No protocol is implied, except that supplied by the PF and VF drivers.
 */

struct hv_read_config_compl {
	struct hv_pci_compl comp_pkt;
	void *buf;
	unsigned int len;
	unsigned int bytes_returned;
};

/**
 * hv_pci_read_config_compl() - Invoked when a response packet
 * for a read config block operation arrives.
 * @context:		Identifies the read config operation
 * @resp:		The response packet itself
 * @resp_packet_size:	Size in bytes of the response packet
 */
/* [한국어]
 * hv_pci_read_config_compl - 설정 블록 읽기 응답을 받아 버퍼에 옮긴다
 *
 * @context: 요청 때 넘겨 둔 struct hv_read_config_compl.
 * @resp: 호스트가 돌려준 응답.
 * @resp_packet_size: 응답 패킷의 실제 크기.
 * @return: 없음.
 *
 * **hv_pci_generic_compl() 과 달리 응답에 데이터가 실려 오므로,
 * 그것을 요청자의 버퍼로 옮기는 일이 이 함수의 몫이다.**
 *
 * **resp_packet_size 를 실제로 쓰는 드문 완료 함수다.**
 * 응답 구조체의 고정 머리 크기를 offsetof 로 구한 뒤, 받은 크기가 그보다
 * 작으면 잘린 패킷이므로 -1 을 상태로 담고 물러난다.
 * 그렇지 않으면 나머지가 데이터 길이다.
 *
 * **요청자가 준 버퍼보다 많이 오면 잘라 넣는다** -- min(comp->len, data_len)
 * 이 그 처리다. 호스트를 믿지 않는 방어이며, 그 개수를 bytes_returned 로
 * 알려 준다.
 *
 * **상태가 0 이 아니거나 데이터가 없으면 0바이트를 받았다고 표시한다** --
 * 호출자 hv_read_config_block() 이 그 둘을 함께 보고 -EIO 를 낸다.
 *
 * **goto out 이 completion 을 반드시 부르게 만든다** -- 어느 경로로든
 * 요청자를 깨워야 그쪽이 영영 잠들지 않는다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> memcpy(), complete()
 */
static void hv_pci_read_config_compl(void *context, struct pci_response *resp,
				     int resp_packet_size)
{
	struct hv_read_config_compl *comp = context;
	struct pci_read_block_response *read_resp =
		(struct pci_read_block_response *)resp;
	unsigned int data_len, hdr_len;

	hdr_len = offsetof(struct pci_read_block_response, bytes);
	if (resp_packet_size < hdr_len) {
		comp->comp_pkt.completion_status = -1;
		goto out;
	}

	data_len = resp_packet_size - hdr_len;
	if (data_len > 0 && read_resp->status == 0) {
		comp->bytes_returned = min(comp->len, data_len);
		memcpy(comp->buf, read_resp->bytes, comp->bytes_returned);
	} else {
		comp->bytes_returned = 0;
	}

	comp->comp_pkt.completion_status = read_resp->status;
out:
	complete(&comp->comp_pkt.host_event);
}

/**
 * hv_read_config_block() - Sends a read config block request to
 * the back-end driver running in the Hyper-V parent partition.
 * @pdev:		The PCI driver's representation for this device.
 * @buf:		Buffer into which the config block will be copied.
 * @len:		Size in bytes of buf.
 * @block_id:		Identifies the config block which has been requested.
 * @bytes_returned:	Size which came back from the back-end driver.
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_read_config_block - 호스트에 설정 블록 읽기를 요청한다
 *
 * @pdev: 대상 PCI 장치.
 * @buf: 결과를 받을 버퍼.
 * @len: 버퍼 크기.
 * @block_id: 어느 설정 블록인지.
 * @bytes_returned: 실제로 받은 바이트 수를 담아 돌려줄 자리.
 * @return: 성공 0, 실패면 음수.
 *
 * **설정공간이 아니라 "설정 블록" 이라는 Hyper-V 고유 개념을 다룬다.**
 * 장치별 정보를 호스트가 블록 단위로 제공하며, 그것을 쓰는 것은
 * 이 파일이 아니라 위층 드라이버다(hv_pci_bus_ops 로 노출된다).
 *
 * **이 파일의 동기 요청 관용이 여기 그대로 나온다.**
 * 1. **패킷과 요청 본문을 한 구조체에 붙여 스택에 잡는다** --
 *    struct pci_packet 뒤에 요청 바이트가 오는 형태다.
 * 2. **completion 을 초기화하고 완료 함수와 문맥을 패킷에 담는다.**
 * 3. vmbus_sendpacket 으로 보내며 **패킷 주소를 요청 ID 로 함께 넘긴다** --
 *    응답이 올 때 그것으로 이 요청을 되찾는다.
 * 4. wait_for_response() 로 잠들었다 깨어난다.
 * 5. 상태를 확인한다.
 *
 * **VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED 가 응답을 요구하는 표시다.**
 * 이 플래그가 없으면 호스트가 답하지 않는다.
 *
 * **길이를 두 번 검사한다** -- 여기서 0 과 상한을 보고,
 * 완료 함수가 실제로 받은 크기를 다시 본다.
 *
 * **스택 위의 구조체를 호스트에 넘긴다.** 응답이 올 때까지 이 함수가
 * 잠들어 있으므로 그 스택이 살아 있다 -- wait_for_response() 가
 * 반드시 돌아와야 하는 이유이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   위층 드라이버(hv_pci_bus_ops 경유) -> [이 함수]
 *     -> vmbus_sendpacket(), wait_for_response()
 */
static int hv_read_config_block(struct pci_dev *pdev, void *buf,
				unsigned int len, unsigned int block_id,
				unsigned int *bytes_returned)
{
	struct hv_pcibus_device *hbus =
		container_of(pdev->bus->sysdata, struct hv_pcibus_device,
			     sysdata);
	struct {
		struct pci_packet pkt;
		char buf[sizeof(struct pci_read_block)];
	} pkt;
	struct hv_read_config_compl comp_pkt;
	struct pci_read_block *read_blk;
	int ret;

	if (len == 0 || len > HV_CONFIG_BLOCK_SIZE_MAX)
		return -EINVAL;

	init_completion(&comp_pkt.comp_pkt.host_event);
	comp_pkt.buf = buf;
	comp_pkt.len = len;

	memset(&pkt, 0, sizeof(pkt));
	pkt.pkt.completion_func = hv_pci_read_config_compl;
	pkt.pkt.compl_ctxt = &comp_pkt;
	read_blk = (struct pci_read_block *)pkt.buf;
	read_blk->message_type.type = PCI_READ_BLOCK;
	read_blk->wslot.slot = devfn_to_wslot(pdev->devfn);
	read_blk->block_id = block_id;
	read_blk->bytes_requested = len;

	ret = vmbus_sendpacket(hbus->hdev->channel, read_blk,
			       sizeof(*read_blk), (unsigned long)&pkt.pkt,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		return ret;

	ret = wait_for_response(hbus->hdev, &comp_pkt.comp_pkt.host_event);
	if (ret)
		return ret;

	if (comp_pkt.comp_pkt.completion_status != 0 ||
	    comp_pkt.bytes_returned == 0) {
		dev_err(&hbus->hdev->device,
			"Read Config Block failed: 0x%x, bytes_returned=%d\n",
			comp_pkt.comp_pkt.completion_status,
			comp_pkt.bytes_returned);
		return -EIO;
	}

	*bytes_returned = comp_pkt.bytes_returned;
	return 0;
}

/**
 * hv_pci_write_config_compl() - Invoked when a response packet for a write
 * config block operation arrives.
 * @context:		Identifies the write config operation
 * @resp:		The response packet itself
 * @resp_packet_size:	Size in bytes of the response packet
 */
/* [한국어]
 * hv_pci_write_config_compl - 설정 블록 쓰기 응답을 받아 요청자를 깨운다
 *
 * @context: 요청 때 넘겨 둔 struct hv_pci_compl.
 * @resp: 호스트가 돌려준 응답.
 * @resp_packet_size: 쓰이지 않는다.
 * @return: 없음.
 *
 * **hv_pci_generic_compl() 과 내용이 완전히 같다** -- 상태를 옮겨 담고
 * completion 을 깨운다. 쓰기 응답에는 데이터가 없기 때문이다.
 *
 * **그런데도 따로 둔 이유가 코드에 적혀 있지 않다.**
 * hv_pci_generic_compl 을 그대로 써도 동작이 같아 보이는 자리다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> complete()
 */
static void hv_pci_write_config_compl(void *context, struct pci_response *resp,
				      int resp_packet_size)
{
	struct hv_pci_compl *comp_pkt = context;

	comp_pkt->completion_status = resp->status;
	complete(&comp_pkt->host_event);
}

/**
 * hv_write_config_block() - Sends a write config block request to the
 * back-end driver running in the Hyper-V parent partition.
 * @pdev:		The PCI driver's representation for this device.
 * @buf:		Buffer from which the config block will	be copied.
 * @len:		Size in bytes of buf.
 * @block_id:		Identifies the config block which is being written.
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_write_config_block - 호스트에 설정 블록 쓰기를 요청한다
 *
 * @pdev: 대상 PCI 장치.
 * @buf: 쓸 데이터가 든 버퍼.
 * @len: 데이터 크기.
 * @block_id: 어느 설정 블록인지.
 * @return: 성공 0, 실패면 음수.
 *
 * **hv_read_config_block() 의 짝이며 흐름이 같다.**
 * 다만 데이터를 요청 패킷에 실어 보내므로 패킷 크기 계산이 필요하다.
 *
 * **패킷 크기를 offsetof 로 구한다** -- 고정 머리 뒤에 실제 데이터 길이만
 * 붙이므로, 최대 크기 배열 전체를 보내지 않는다.
 *
 * **그러고 나서 4바이트를 더 붙이는 것이 이 함수의 특이점이다.**
 * 상류 주석이 그 사정을 자세히 밝힌다 -- **2018년 무렵 출시된 일부 호스트가
 * 패킷 크기를 제대로 확인하지 않아** 생긴 우회이며, 2019년 초에 고쳐졌다.
 * 아주 오래된 호스트와 새 호스트 모두에서 안전한 이유도 밝혀 두었다 --
 * 실제로 중요한 것은 write_blk->byte_count 에 적힌 길이이기 때문이다.
 * **그래서 구조체에 쓰이지 않는 reserved 필드가 하나 있다** --
 * 크기를 4바이트 늘리기 위한 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   위층 드라이버(hv_pci_bus_ops 경유) -> [이 함수]
 *     -> vmbus_sendpacket(), wait_for_response()
 */
static int hv_write_config_block(struct pci_dev *pdev, void *buf,
				unsigned int len, unsigned int block_id)
{
	struct hv_pcibus_device *hbus =
		container_of(pdev->bus->sysdata, struct hv_pcibus_device,
			     sysdata);
	struct {
		struct pci_packet pkt;
		char buf[sizeof(struct pci_write_block)];
		u32 reserved;
	} pkt;
	struct hv_pci_compl comp_pkt;
	struct pci_write_block *write_blk;
	u32 pkt_size;
	int ret;

	if (len == 0 || len > HV_CONFIG_BLOCK_SIZE_MAX)
		return -EINVAL;

	init_completion(&comp_pkt.host_event);

	memset(&pkt, 0, sizeof(pkt));
	pkt.pkt.completion_func = hv_pci_write_config_compl;
	pkt.pkt.compl_ctxt = &comp_pkt;
	write_blk = (struct pci_write_block *)pkt.buf;
	write_blk->message_type.type = PCI_WRITE_BLOCK;
	write_blk->wslot.slot = devfn_to_wslot(pdev->devfn);
	write_blk->block_id = block_id;
	write_blk->byte_count = len;
	memcpy(write_blk->bytes, buf, len);
	pkt_size = offsetof(struct pci_write_block, bytes) + len;
	/*
	 * This quirk is required on some hosts shipped around 2018, because
	 * these hosts don't check the pkt_size correctly (new hosts have been
	 * fixed since early 2019). The quirk is also safe on very old hosts
	 * and new hosts, because, on them, what really matters is the length
	 * specified in write_blk->byte_count.
	 */
	pkt_size += sizeof(pkt.reserved);

	ret = vmbus_sendpacket(hbus->hdev->channel, write_blk, pkt_size,
			       (unsigned long)&pkt.pkt, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		return ret;

	ret = wait_for_response(hbus->hdev, &comp_pkt.host_event);
	if (ret)
		return ret;

	if (comp_pkt.completion_status != 0) {
		dev_err(&hbus->hdev->device,
			"Write Config Block failed: 0x%x\n",
			comp_pkt.completion_status);
		return -EIO;
	}

	return 0;
}

/**
 * hv_register_block_invalidate() - Invoked when a config block invalidation
 * arrives from the back-end driver.
 * @pdev:		The PCI driver's representation for this device.
 * @context:		Identifies the device.
 * @block_invalidate:	Identifies all of the blocks being invalidated.
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_register_block_invalidate - 설정 블록 무효화 알림을 받을 콜백을 등록한다
 *
 * @pdev: 대상 PCI 장치.
 * @context: 콜백에 함께 넘길 문맥.
 * @block_invalidate: 무효화가 왔을 때 부를 함수.
 * @return: 성공 0, 장치를 못 찾으면 -ENODEV.
 *
 * **호스트가 "이 장치의 설정 블록이 바뀌었으니 다시 읽어라" 고 알려 올 때
 * 그것을 받을 상대를 등록하는 함수다.**
 *
 * 호스트가 그 알림(PCI_INVALIDATE_BLOCK)을 보내면
 * hv_pci_onchannelcallback() 이 받아 여기 등록된 콜백을 부른다 --
 * **요청-응답이 아니라 호스트가 먼저 보내는 세 알림 중 하나다.**
 *
 * **콜백과 문맥을 hv_pci_dev 에 담아 둔다.** 그 구조체가 장치 하나에
 * 대응하므로, 알림에 실린 wslot 으로 그것을 찾아 콜백을 부를 수 있다.
 *
 * **해제하는 짝이 없다** -- 등록만 있고 등록 해제 함수가 이 파일에 없다.
 * 장치가 사라질 때 hv_pci_dev 가 통째로 해제되므로 콜백도 함께 사라진다.
 *
 * **참조를 쥐었다 놓는다** -- 포인터 둘을 채우는 동안만 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   위층 드라이버(hv_pci_bus_ops 경유) -> [이 함수]
 *     -> get_pcichild_wslot(), put_pcichild()
 */
static int hv_register_block_invalidate(struct pci_dev *pdev, void *context,
					void (*block_invalidate)(void *context,
								 u64 block_mask))
{
	struct hv_pcibus_device *hbus =
		container_of(pdev->bus->sysdata, struct hv_pcibus_device,
			     sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(pdev->devfn));
	if (!hpdev)
		return -ENODEV;

	hpdev->block_invalidate = block_invalidate;
	hpdev->invalidate_context = context;

	put_pcichild(hpdev);
	return 0;

}

/* Interrupt management hooks */
/* [한국어]
 * hv_int_desc_free - 호스트에 인터럽트 삭제를 알리고 서술자를 해제한다
 *
 * @hpdev: 이 인터럽트를 가진 장치.
 * @int_desc: 해제할 인터럽트 서술자. 호스트가 준 주소·데이터가 들어 있다.
 * @return: 없음.
 *
 * **호스트가 인터럽트 리디렉션 테이블을 관리하므로 게스트가 그냥 잊어서는
 * 안 된다** -- 삭제 메시지를 보내야 호스트가 그 항목을 지운다.
 *
 * **vector_count 가 0 이면 메시지를 보내지 않고 해제만 한다.**
 * 아직 호스트에 등록되지 않은 서술자라는 뜻이다 --
 * hv_compose_msi_msg() 가 실패했을 때 그런 상태가 남는다.
 *
 * **응답을 기다리지 않는다.** vmbus_sendpacket 의 요청 ID 자리에 0 을 넣고
 * 완료 요청 플래그도 주지 않는다 -- 곧 **보내고 잊는(fire-and-forget) 방식** 이다.
 * 그래서 이 함수는 잠들지 않으며, 반환값도 확인하지 않는다.
 *
 * **패킷과 본문을 한 구조체에 붙여 스택에 잡는 관용은 이 파일의 다른
 * 요청들과 같다.** 다만 응답을 기다리지 않으므로 completion 이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   hv_msi_free / hv_compose_msi_msg 의 재시도 경로
 *     -> [이 함수] -> vmbus_sendpacket(), kfree()
 */
static void hv_int_desc_free(struct hv_pci_dev *hpdev,
			     struct tran_int_desc *int_desc)
{
	struct pci_delete_interrupt *int_pkt;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_delete_interrupt)];
	} ctxt;

	if (!int_desc->vector_count) {
		kfree(int_desc);
		return;
	}
	memset(&ctxt, 0, sizeof(ctxt));
	int_pkt = (struct pci_delete_interrupt *)ctxt.buffer;
	int_pkt->message_type.type =
		PCI_DELETE_INTERRUPT_MESSAGE;
	int_pkt->wslot.slot = hpdev->desc.win_slot.slot;
	int_pkt->int_desc = *int_desc;
	vmbus_sendpacket(hpdev->hbus->hdev->channel, int_pkt, sizeof(*int_pkt),
			 0, VM_PKT_DATA_INBAND, 0);
	kfree(int_desc);
}

/**
 * hv_msi_free() - Free the MSI.
 * @domain:	The interrupt domain pointer
 * @irq:	Identifies the IRQ.
 *
 * The Hyper-V parent partition and hypervisor are tracking the
 * messages that are in use, keeping the interrupt redirection
 * table up to date.  This callback sends a message that frees
 * the IRT entry and related tracking nonsense.
 */
/* [한국어]
 * hv_msi_free - MSI 하나를 호스트에서 지운다
 *
 * @domain: MSI IRQ 도메인.
 * @irq: 지울 리눅스 IRQ 번호.
 * @return: 없음.
 *
 * **상류 주석이 그 이유를 밝힌다** -- 호스트와 하이퍼바이저가 쓰이고 있는
 * 메시지를 추적하며 인터럽트 리디렉션 테이블을 유지하므로,
 * 게스트가 인터럽트를 놓을 때 그 항목도 함께 지워 달라고 알려야 한다.
 *
 * 절차가 넷이다.
 * 1. irq_data 에서 msi_desc 를 거쳐 pci_dev 를 되찾는다.
 * 2. **chip_data 에 담아 둔 인터럽트 서술자를 꺼낸다.**
 *    그것이 hv_compose_msi_msg() 가 호스트에게 받아 저장해 둔 값이다.
 *    **없으면 아직 등록되지 않은 것이므로 그대로 물러난다.**
 * 3. **chip_data 를 NULL 로 먼저 민다** -- 이 뒤로 같은 서술자를 두 번
 *    놓지 않게 하는 장치다.
 * 4. 장치를 찾아 hv_int_desc_free() 로 넘긴다.
 *    **장치를 못 찾으면 메시지를 보내지 못하고 서술자만 해제한다** --
 *    장치가 이미 사라진 경우이며, 호스트 쪽 항목은 그때 함께 정리된다.
 *
 * **hbus 를 domain->host_data 에서 얻는다** -- 이 도메인을 만들 때
 * hv_pcie_init_irq_domain() 이 거기에 담아 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 해제 경로).
 *
 * 호출 체인:
 *   hv_pcie_domain_free -> [이 함수]
 *     -> get_pcichild_wslot(), hv_int_desc_free(), put_pcichild()
 */
static void hv_msi_free(struct irq_domain *domain, unsigned int irq)
{
	struct hv_pcibus_device *hbus;
	struct hv_pci_dev *hpdev;
	struct pci_dev *pdev;
	struct tran_int_desc *int_desc;
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, irq);
	struct msi_desc *msi = irq_data_get_msi_desc(irq_data);

	pdev = msi_desc_to_pci_dev(msi);
	hbus = domain->host_data;
	int_desc = irq_data_get_irq_chip_data(irq_data);
	if (!int_desc)
		return;

	irq_data->chip_data = NULL;
	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(pdev->devfn));
	if (!hpdev) {
		kfree(int_desc);
		return;
	}

	hv_int_desc_free(hpdev, int_desc);
	put_pcichild(hpdev);
}

/* [한국어]
 * hv_irq_mask - 인터럽트를 막는다. 부모가 할 수 있으면 부모에게 넘긴다
 *
 * @data: 인터럽트를 나타내는 irq_data.
 * @return: 없음.
 *
 * **이 계층은 마스크를 스스로 다루지 않는다.** 부모 도메인의 irq_chip 에
 * irq_mask 가 있으면 그쪽으로 넘기고, 없으면 아무것도 하지 않는다.
 *
 * **부모에 그 콜백이 없을 수 있는 이유**: 아키텍처와 구성에 따라 부모가
 * x86_vector_domain 이거나 GIC 도메인이며, 마스크를 지원하지 않는 조합이
 * 있기 때문으로 보인다 -- 그 사정이 코드에 적혀 있지는 않다.
 *
 * **hv_irq_unmask() 와 대칭이 아니다.** 그쪽은 부모에 넘기기 전에
 * hv_arch_irq_unmask() 로 하이퍼바이저에 대상 CPU 를 알리는 일을 먼저 한다.
 * **막을 때는 그런 통보가 필요 없기 때문이다** -- 대상이 바뀌는 것은
 * 푸는 순간이다.
 *
 * 실행 컨텍스트: IRQ 코어의 mask 경로. 인터럽트가 막힌 채로 불린다.
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> irq_chip_mask_parent()
 */
static void hv_irq_mask(struct irq_data *data)
{
	if (data->parent_data->chip->irq_mask)
		irq_chip_mask_parent(data);
}

/* [한국어]
 * hv_irq_unmask - 인터럽트를 풀며 하이퍼바이저에 대상 CPU 를 알린다
 *
 * @data: 인터럽트를 나타내는 irq_data.
 * @return: 없음.
 *
 * **순서가 이 함수의 요점이다** -- 먼저 아키텍처별 처리를 하고,
 * 그다음 부모에게 마스크 해제를 넘긴다.
 *
 * **hv_arch_irq_unmask() 가 아키텍처를 감춘다.**
 * x86 에서는 하이퍼콜로 인터럽트 대상을 다시 알리고,
 * ARM64 에서는 빈 함수라 아무 일도 하지 않는다.
 * **두 갈래를 같은 이름으로 둔 덕에 이 함수에는 조건부 컴파일이 없다.**
 *
 * **왜 푸는 순간에 대상을 알려야 하는가**: 게스트가 친화도를 바꿔도
 * 그것을 실제로 해석하는 것은 하이퍼바이저의 리디렉션 테이블이다.
 * 마스크가 풀려 인터럽트가 흐르기 시작하기 전에 그 테이블이 최신이어야 한다.
 * **그래서 아키텍처 처리가 부모 호출보다 앞에 온다.**
 *
 * 부모에 irq_unmask 가 없으면 그 단계는 건너뛴다 -- hv_irq_mask() 와 같은 방어다.
 *
 * 실행 컨텍스트: IRQ 코어의 unmask 경로.
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수]
 *     -> hv_arch_irq_unmask(), irq_chip_unmask_parent()
 */
static void hv_irq_unmask(struct irq_data *data)
{
	hv_arch_irq_unmask(data);

	if (data->parent_data->chip->irq_unmask)
		irq_chip_unmask_parent(data);
}

struct compose_comp_ctxt {
	struct hv_pci_compl comp_pkt;
	struct tran_int_desc int_desc;
};

/* [한국어]
 * hv_pci_compose_compl - MSI 등록 응답을 받아 주소·데이터를 챙긴다
 *
 * @context: 요청 때 넘겨 둔 struct compose_comp_ctxt.
 * @resp: 호스트가 돌려준 응답.
 * @resp_packet_size: 응답 크기.
 * @return: 없음.
 *
 * **응답에 실려 오는 tran_int_desc 가 이 요청의 목적 전부다** --
 * 그 안의 address 와 data 가 곧 장치가 인터럽트를 낼 때 쓸 MSI 주소와
 * 데이터이며, 호스트만이 그것을 알려 줄 수 있다.
 *
 * **크기를 먼저 확인한다.** 기대하는 응답 구조체보다 작으면 잘린 패킷이므로
 * -1 을 상태로 담고 물러난다 -- 그 뒤의 int_desc 복사를 건너뛰어야
 * 쓰레기 값을 쓰지 않는다.
 *
 * **상태와 서술자를 함께 담는다.** 호출자 hv_compose_msi_msg() 가
 * 그 둘을 보고 성공 여부와 실제 값을 얻는다.
 *
 * **goto out 이 어느 경로로든 completion 을 부르게 한다** --
 * 이 파일의 완료 함수들이 공유하는 형태다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> complete()
 */
static void hv_pci_compose_compl(void *context, struct pci_response *resp,
				 int resp_packet_size)
{
	struct compose_comp_ctxt *comp_pkt = context;
	struct pci_create_int_response *int_resp =
		(struct pci_create_int_response *)resp;

	if (resp_packet_size < sizeof(*int_resp)) {
		comp_pkt->comp_pkt.completion_status = -1;
		goto out;
	}
	comp_pkt->comp_pkt.completion_status = resp->status;
	comp_pkt->int_desc = int_resp->int_desc;
out:
	complete(&comp_pkt->comp_pkt.host_event);
}

/* [한국어]
 * hv_compose_msi_req_v1 - 프로토콜 1.1 형식의 MSI 등록 요청을 채운다
 *
 * @int_pkt: 채울 요청 구조체.
 * @slot: Windows 표현의 슬롯 번호.
 * @vector: 인터럽트 벡터. **1.1 에서는 8비트다.**
 * @vector_count: 연속한 벡터 개수.
 * @return: 채운 요청의 크기(바이트).
 *
 * **세 판(v1/v2/v3) 중 가장 오래된 형식이다.**
 * 프로토콜 버전 협상 결과가 1.1 이면 이것을 쓴다.
 *
 * **대상 CPU 를 지정하지 않고 CPU_AFFINITY_ALL 을 넣는다.**
 * 상류 주석이 그 사정을 밝힌다 -- 여기서는 더미 값을 넣고,
 * 실제 대상은 나중에 hv_irq_unmask() 가 재타게팅 하이퍼콜로 알린다.
 *
 * **v2/v3 와 결정적으로 다른 점이 그 필드다** -- 뒤 판은
 * processor_array 로 특정 가상 프로세서를 지정하는데,
 * 1.1 은 64비트 마스크 하나뿐이라 "전부" 를 뜻하는 값을 쓴다.
 *
 * **크기를 돌려주는 것이 세 판 공통의 규약이다** -- 호출자가 그 값으로
 * vmbus_sendpacket 에 넘길 길이를 정한다.
 *
 * 실행 컨텍스트: hv_compose_msi_msg 안. 스핀락을 쥔 상태일 수 있다.
 *
 * 호출 체인:
 *   hv_compose_msi_msg -> [이 함수]
 */
static u32 hv_compose_msi_req_v1(
	struct pci_create_interrupt *int_pkt,
	u32 slot, u8 vector, u16 vector_count)
{
	int_pkt->message_type.type = PCI_CREATE_INTERRUPT_MESSAGE;
	int_pkt->wslot.slot = slot;
	int_pkt->int_desc.vector = vector;
	int_pkt->int_desc.vector_count = vector_count;
	int_pkt->int_desc.delivery_mode = DELIVERY_MODE;

	/*
	 * Create MSI w/ dummy vCPU set, overwritten by subsequent retarget in
	 * hv_irq_unmask().
	 */
	int_pkt->int_desc.cpu_mask = CPU_AFFINITY_ALL;

	return sizeof(*int_pkt);
}

/*
 * The vCPU selected by hv_compose_multi_msi_req_get_cpu() and
 * hv_compose_msi_req_get_cpu() is a "dummy" vCPU because the final vCPU to be
 * interrupted is specified later in hv_irq_unmask() and communicated to Hyper-V
 * via the HVCALL_RETARGET_INTERRUPT hypercall. But the choice of dummy vCPU is
 * not irrelevant because Hyper-V chooses the physical CPU to handle the
 * interrupts based on the vCPU specified in message sent to the vPCI VSP in
 * hv_compose_msi_msg(). Hyper-V's choice of pCPU is not visible to the guest,
 * but assigning too many vPCI device interrupts to the same pCPU can cause a
 * performance bottleneck. So we spread out the dummy vCPUs to influence Hyper-V
 * to spread out the pCPUs that it selects.
 *
 * For the single-MSI and MSI-X cases, it's OK for hv_compose_msi_req_get_cpu()
 * to always return the same dummy vCPU, because a second call to
 * hv_compose_msi_msg() contains the "real" vCPU, causing Hyper-V to choose a
 * new pCPU for the interrupt. But for the multi-MSI case, the second call to
 * hv_compose_msi_msg() exits without sending a message to the vPCI VSP, so the
 * original dummy vCPU is used. This dummy vCPU must be round-robin'ed so that
 * the pCPUs are spread out. All interrupts for a multi-MSI device end up using
 * the same pCPU, even though the vCPUs will be spread out by later calls
 * to hv_irq_unmask(), but that is the best we can do now.
 *
 * With Hyper-V in Nov 2022, the HVCALL_RETARGET_INTERRUPT hypercall does *not*
 * cause Hyper-V to reselect the pCPU based on the specified vCPU. Such an
 * enhancement is planned for a future version. With that enhancement, the
 * dummy vCPU selection won't matter, and interrupts for the same multi-MSI
 * device will be spread across multiple pCPUs.
 */

/*
 * Create MSI w/ dummy vCPU set targeting just one vCPU, overwritten
 * by subsequent retarget in hv_irq_unmask().
 */
/* [한국어]
 * hv_compose_msi_req_get_cpu - 단일 MSI/MSI-X 용 더미 vCPU 를 고른다
 *
 * @affinity: 이 인터럽트가 갈 수 있는 CPU 집합.
 * @return: 고른 CPU 번호.
 *
 * **"더미" 라는 말이 왜 붙는지가 위쪽 긴 상류 주석에 있다.**
 * 실제 대상은 나중에 hv_irq_unmask() 의 재타게팅 하이퍼콜이 정하므로,
 * 여기서 고르는 값은 최종 대상이 아니다.
 *
 * **그런데도 아무 값이나 넣으면 안 된다.** 상류 주석이 밝히듯
 * **Hyper-V 는 이 메시지에 실린 vCPU 를 보고 물리 CPU 를 고르며,
 * 그 선택은 게스트에게 보이지 않는다.** 같은 물리 CPU 에 너무 많은
 * 인터럽트가 몰리면 병목이 되므로, 더미 vCPU 를 흩어 놓아
 * 물리 CPU 도 흩어지도록 유도한다.
 *
 * **단일 MSI 와 MSI-X 는 늘 같은 값을 돌려주어도 된다** --
 * 상류 주석대로 두 번째 호출에 진짜 vCPU 가 실려 가므로
 * Hyper-V 가 그때 물리 CPU 를 다시 고르기 때문이다.
 * **다중 MSI 만 다르며**, 그래서 아래에 별도 함수가 있다.
 *
 * cpumask_first_and 로 친화도와 온라인 CPU 의 교집합에서 첫 번째를 고른다.
 *
 * 실행 컨텍스트: hv_compose_msi_msg 안.
 *
 * 호출 체인:
 *   hv_compose_msi_msg -> [이 함수]
 */
static int hv_compose_msi_req_get_cpu(const struct cpumask *affinity)
{
	return cpumask_first_and(affinity, cpu_online_mask);
}

/*
 * Make sure the dummy vCPU values for multi-MSI don't all point to vCPU0.
 */
/* [한국어]
 * hv_compose_multi_msi_req_get_cpu - 다중 MSI 용 더미 vCPU 를 돌아가며 고른다
 *
 * @return: 고른 CPU 번호.
 *
 * **다중 MSI 만 별도 함수가 필요한 이유가 위쪽 상류 주석에 있다.**
 * 다중 MSI 는 두 번째 hv_compose_msi_msg() 호출이 메시지를 보내지 않고
 * 곧바로 빠져나가므로, **첫 호출의 더미 vCPU 가 그대로 최종 선택에 쓰인다.**
 * 그래서 그 값을 라운드로빈으로 돌려 물리 CPU 가 흩어지게 해야 한다.
 *
 * **정적 변수 둘을 쓴다** -- 다음에 고를 CPU 를 기억하는 cpu_next 와
 * 그것을 지키는 스핀락이다. 함수 안 static 이라 호출 사이에 값이 남는다.
 * **-1 로 시작하는 것은 cpumask_next_wrap 이 0번부터 내주게 하려는 것이며**,
 * 주석이 그 뜻을 밝힌다.
 *
 * **스핀락이 필요한 이유**: 여러 장치가 동시에 MSI 를 등록하면
 * 같은 CPU 를 두 번 내줄 수 있다. irqsave 로 잡는 것은 이 경로가
 * 인터럽트를 막은 문맥에서도 불릴 수 있기 때문으로 보인다.
 *
 * **상류 주석이 이 방식의 한계도 밝힌다** -- 한 다중 MSI 장치의 인터럽트는
 * 결국 같은 물리 CPU 를 쓰게 되며, 2022년 11월 기준 재타게팅 하이퍼콜이
 * 물리 CPU 를 다시 고르지 않기 때문이다. 그 개선이 계획되어 있다고 적어 두었다.
 *
 * 실행 컨텍스트: hv_compose_msi_msg 안. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   hv_compose_msi_msg -> [이 함수] -> cpumask_next_wrap()
 */
static int hv_compose_multi_msi_req_get_cpu(void)
{
	static DEFINE_SPINLOCK(multi_msi_cpu_lock);

	/* -1 means starting with CPU 0 */
	static int cpu_next = -1;

	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&multi_msi_cpu_lock, flags);

	cpu_next = cpumask_next_wrap(cpu_next, cpu_online_mask);
	cpu = cpu_next;

	spin_unlock_irqrestore(&multi_msi_cpu_lock, flags);

	return cpu;
}

/* [한국어]
 * hv_compose_msi_req_v2 - 프로토콜 1.2 형식의 MSI 등록 요청을 채운다
 *
 * @int_pkt: 채울 요청 구조체.
 * @cpu: 더미 대상 CPU.
 * @slot: Windows 표현의 슬롯 번호.
 * @vector: 인터럽트 벡터. **v1 과 같이 8비트다.**
 * @vector_count: 연속한 벡터 개수.
 * @return: 채운 요청의 크기(바이트).
 *
 * **v1 과 다른 점은 대상 지정 방식뿐이다.**
 * 64비트 cpu_mask 대신 **processor_array 배열과 개수** 를 쓴다 --
 * 그래야 가상 프로세서 64개를 넘는 구성을 다룰 수 있다.
 *
 * **배열에 하나만 넣고 개수를 1 로 둔다.** 여러 CPU 를 지정할 수 있는
 * 형식인데도 하나만 쓰는 것은, 위쪽 상류 주석이 밝히듯 이 값이 더미이고
 * 실제 대상은 재타게팅 하이퍼콜이 정하기 때문이다.
 *
 * **hv_cpu_number_to_vp_number() 로 리눅스 CPU 번호를 가상 프로세서
 * 번호로 옮긴다** -- 둘이 늘 같지는 않다.
 * **그 함수의 정의는 이 트리에 없다.**
 *
 * 실행 컨텍스트: hv_compose_msi_msg 안.
 *
 * 호출 체인:
 *   hv_compose_msi_msg -> [이 함수] -> hv_cpu_number_to_vp_number()
 */
static u32 hv_compose_msi_req_v2(
	struct pci_create_interrupt2 *int_pkt, int cpu,
	u32 slot, u8 vector, u16 vector_count)
{
	int_pkt->message_type.type = PCI_CREATE_INTERRUPT_MESSAGE2;
	int_pkt->wslot.slot = slot;
	int_pkt->int_desc.vector = vector;
	int_pkt->int_desc.vector_count = vector_count;
	int_pkt->int_desc.delivery_mode = DELIVERY_MODE;
	int_pkt->int_desc.processor_array[0] =
		hv_cpu_number_to_vp_number(cpu);
	int_pkt->int_desc.processor_count = 1;

	return sizeof(*int_pkt);
}

/* [한국어]
 * hv_compose_msi_req_v3 - 프로토콜 1.3 형식의 MSI 등록 요청을 채운다
 *
 * @int_pkt: 채울 요청 구조체.
 * @cpu: 더미 대상 CPU.
 * @slot: Windows 표현의 슬롯 번호.
 * @vector: 인터럽트 벡터. **v2 와 달리 32비트다.**
 * @vector_count: 연속한 벡터 개수.
 * @return: 채운 요청의 크기(바이트).
 *
 * **v2 와 다른 점은 벡터 필드의 폭 하나뿐이다.**
 * struct hv_msi_desc3 의 주석이 그 이유를 밝힌다 -- ARM 의 LPI 벡터처럼
 * 더 큰 값을 담아야 하기 때문이다.
 *
 * 폭이 넓어지면서 delivery_mode 뒤에 reserved 바이트가 하나 생겼고,
 * 이 함수가 그것을 0 으로 채운다 -- **호스트가 쓰레기 값을 보지 않게
 * 하려는 것이며, 나머지 필드는 호출자가 memset 으로 이미 0 으로 만들었다.**
 *
 * **세 판을 나눠 두었지만 채우는 내용은 거의 같다** --
 * 메시지 종류, 슬롯, 벡터, 개수, 전달 방식, 대상.
 * 프로토콜이 자라면서 필드 폭과 대상 표현만 바뀐 셈이다.
 *
 * 실행 컨텍스트: hv_compose_msi_msg 안.
 *
 * 호출 체인:
 *   hv_compose_msi_msg -> [이 함수] -> hv_cpu_number_to_vp_number()
 */
static u32 hv_compose_msi_req_v3(
	struct pci_create_interrupt3 *int_pkt, int cpu,
	u32 slot, u32 vector, u16 vector_count)
{
	int_pkt->message_type.type = PCI_CREATE_INTERRUPT_MESSAGE3;
	int_pkt->wslot.slot = slot;
	int_pkt->int_desc.vector = vector;
	int_pkt->int_desc.reserved = 0;
	int_pkt->int_desc.vector_count = vector_count;
	int_pkt->int_desc.delivery_mode = DELIVERY_MODE;
	int_pkt->int_desc.processor_array[0] =
		hv_cpu_number_to_vp_number(cpu);
	int_pkt->int_desc.processor_count = 1;

	return sizeof(*int_pkt);
}

/**
 * hv_compose_msi_msg() - Supplies a valid MSI address/data
 * @data:	Everything about this MSI
 * @msg:	Buffer that is filled in by this function
 *
 * This function unpacks the IRQ looking for target CPU set, IDT
 * vector and mode and sends a message to the parent partition
 * asking for a mapping for that tuple in this partition.  The
 * response supplies a data value and address to which that data
 * should be written to trigger that interrupt.
 */
/* [한국어]
 * hv_compose_msi_msg - 호스트에 MSI 를 등록해 실제 주소·데이터를 받아 온다
 *
 * @data: 이 MSI 에 관한 모든 것.
 * @msg: 채워 돌려줄 MSI 주소·데이터.
 * @return: 없음.
 *
 * **이 파일에서 가장 복잡한 함수이며, 프로토콜과 락이 한꺼번에 얽힌다.**
 * 상류 주석이 뜻을 밝힌다 -- 대상 CPU 집합과 벡터와 전달 방식을 꺼내
 * 부모 파티션에 매핑을 요청하고, 그 응답으로 받은 주소·데이터를 돌려준다.
 *
 * **다중 MSI 가 이 함수의 갈래를 넷으로 늘린다.**
 * 1. **이미 chip_data 가 있고 다중 MSI 면** 앞서 받은 값을 그대로 다시 준다.
 *    원문 주석의 "Reuse the previous allocation" 이 그것이다.
 * 2. **다중 MSI 인데 첫 번째가 아니면** 호스트에 묻지 않는다.
 *    첫 번째가 받은 주소는 그대로 쓰고 **데이터만 IRQ 번호 차이만큼 더한다** --
 *    다중 MSI 는 연속한 데이터 값을 쓰기 때문이다.
 * 3. **단일 MSI/MSI-X 인데 앞선 등록이 있으면** 먼저 그것을 지운다.
 * 4. 그 밖이면 호스트에 새로 요청한다.
 *
 * **더미 벡터를 고르는 방식이 갈린다.**
 * - 다중 MSI 는 **벡터를 32 로 고정한다.** 원문 주석이 그 근거를 밝힌다 --
 *   개수와 정렬이 맞아야 하고 0 이 아니어야 하는데, 다중 MSI 는 32까지의
 *   2의 거듭제곱이므로 32면 늘 통한다.
 * - 단일은 실제 벡터를 쓴다.
 *
 * **프로토콜 판이 요청 형식을 가른다** -- 1.1 은 v1, 1.2/1.3 은 v2,
 * 1.4 는 v3 다. 원문 주석이 밝히듯 v1/v2 는 x86 전용이라 벡터가 u8 을
 * 넘지 않으므로 명시적으로 형변환한다.
 * **default 갈래는 닿지 않는다** -- 아는 판만 협상하기 때문이며,
 * 원문 주석이 그것을 밝히면서도 앞으로의 갱신을 돕기 위해 메시지를 남긴다.
 *
 * **응답을 기다리는 방식이 이 파일에서 유일하게 다르다.**
 * 원문 주석이 그 이유를 밝힌다 -- **이 함수는 IRQ 락을 쥔 채로 불리므로
 * 잠들 수 없어**, wait_for_response() 대신 **폴링한다.**
 * 그 폴링 루프가 세 가지를 함께 한다.
 * - **tasklet 을 막아** hv_pci_onchannelcallback() 이 동시에 돌지 않게 한다.
 * - **벤더 ID 를 읽어 장치가 살아 있는지 확인한다** -- 0xFFFF 면 사라진 것이다.
 * - **채널 콜백을 직접 부른다.** 그래야 응답이 처리된다.
 *   원문 주석이 밝히듯 sched_lock 안에서 onchannel_callback 이 NULL 인지
 *   확인해야 링 버퍼가 해제되는 것과 겹치지 않는다.
 *
 * **실패 경로가 다섯 라벨로 나뉜다** -- enable_tasklet, free_int_desc,
 * drop_reference, return_null_message, 그리고 성공 경로.
 * **enable_tasklet 에서 vmbus_request_addr_match 를 부르는 이유가
 * 원문 주석에 있다** -- 스택 위의 완료 패킷이 return 뒤 무효가 되므로,
 * 그 식별자가 아직 이 패킷에 매여 있다면 VMBus 요청자 표에서 빼야 한다.
 *
 * **실패하면 주소와 데이터를 0 으로 채워 돌려준다** -- MSI 를 쓸 수 없다는
 * 뜻이 되며, 오류를 알릴 반환값이 없기 때문이다.
 *
 * 실행 컨텍스트: IRQ 락을 쥔 상태. **잠들 수 없어 GFP_ATOMIC 과 폴링을 쓴다.**
 *
 * 호출 체인:
 *   IRQ 코어(hv_msi_irq_chip.irq_compose_msi_msg) -> [이 함수]
 *     -> hv_compose_msi_req_v1/v2/v3(), vmbus_sendpacket_getid(),
 *   hv_pci_onchannelcallback(), hv_pcifront_get_vendor_id()
 */
static void hv_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct hv_pcibus_device *hbus;
	struct vmbus_channel *channel;
	/* [한국어] 이 인터럽트가 속한 장치의 게스트 쪽 표현. */
	struct hv_pci_dev *hpdev;
	/* [한국어] 그 장치가 붙은 PCI 버스. 아래에서 hbus 를 되찾는 다리다. */
	struct pci_bus *pbus;
	/* [한국어] MSI 서술자에서 되찾은 PCI 장치. */
	struct pci_dev *pdev;
	/* [한국어] 이 인터럽트를 받을 CPU 집합. 아래 대상 CPU 계산의 입력이다. */
	const struct cpumask *dest;
	/* [한국어] 호스트 응답을 기다릴 완료 문맥. 응답의 tran_int_desc 도 여기 담긴다. */
	struct compose_comp_ctxt comp;
	/* [한국어] 호스트가 만들어 준 인터럽트의 변환 서술을 담을 자리. */
	struct tran_int_desc *int_desc;
	/* [한국어] 이 인터럽트의 MSI 서술자. 아래 판단 대부분이 이 값에서 나온다. */
	struct msi_desc *msi_desc;
	/*
	 * vector_count should be u16: see hv_msi_desc, hv_msi_desc2
	 * and hv_msi_desc3. vector must be u32: see hv_msi_desc3.
	 */
	u16 vector_count;
	u32 vector;
	/* [한국어] 요청 패킷을 **스택에** 둔다. 완료를 이 함수 안에서 기다리므로
	 * 돌아가기 전에 응답이 끝나 있다는 전제다. */
	struct {
		/* [한국어] VMBus 요청의 공통 머리. */
		struct pci_packet pci_pkt;
		/* [한국어] 세 판본의 요청 구조체를 겹쳐 둔다 — 한 번에 하나만 쓰므로
		 * 가장 큰 것의 크기만 잡으면 된다. */
		union {
			/* [한국어] 프로토콜 1.1 형식. */
			struct pci_create_interrupt v1;
			/* [한국어] 1.2~1.3 형식. 대상 CPU 를 프로세서 배열로 지정한다. */
			struct pci_create_interrupt2 v2;
			/* [한국어] 1.4 형식. 벡터를 32비트로 넓혔다. */
			struct pci_create_interrupt3 v3;
		/* [한국어] 이 공용체의 어느 갈래를 쓸지는 협상된 판본이 정한다. */
		} int_pkts;
	/* [한국어] __packed 인 것이 중요하다 — 이 구조체 전체가 그대로 전선에 나가므로
	 * 컴파일러가 패딩을 끼워 넣으면 호스트가 필드를 잘못 읽는다. */
	} __packed ctxt;
	/* [한국어] 여러 벡터를 쓰는 MSI 인지. MSI-X 는 벡터마다 서술자가 따로라 해당하지 않는다. */
	bool multi_msi;
	/* [한국어] 보낸 요청의 ID. 시간 초과 시 이것으로 요청을 회수한다. */
	u64 trans_id;
	/* [한국어] 실제로 채워진 요청의 크기. 판본마다 달라 아래 switch 가 정한다. */
	u32 size;
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 이 인터럽트를 보낼 대상 CPU. */
	int cpu;

	msi_desc  = irq_data_get_msi_desc(data);
	/* [한국어] MSI-X 가 아니면서 벡터를 여러 개 쓰는 경우가 multi-MSI 다. */
	multi_msi = !msi_desc->pci.msi_attrib.is_msix &&
		    /* [한국어] 그 둘을 모두 만족해야 한다 — MSI-X 는 벡터마다 서술자가 따로 있어
		     * 이 경로가 필요 없다. */
		    msi_desc->nvec_used > 1;

	/* Reuse the previous allocation */
	if (data->chip_data && multi_msi) {
		int_desc = data->chip_data;
		/* [한국어] 이미 만들어 둔 서술에서 주소 상위 32비트를 꺼내고, */
		msg->address_hi = int_desc->address >> 32;
		/* [한국어] 하위 32비트도 꺼내고, */
		msg->address_lo = int_desc->address & 0xffffffff;
		/* [한국어] 데이터까지 꺼내 그대로 돌려준다. */
		msg->data = int_desc->data;
		/* [한국어] multi-MSI 는 벡터들이 한 요청으로 함께 만들어지므로,
		 * 두 번째 이후 벡터는 이미 있는 결과를 재사용한다(옆의 상류 주석). */
		return;
	}

	pdev = msi_desc_to_pci_dev(msi_desc);
	/* [한국어] 실제로 적용된 친화도 마스크를 읽는다 — 요청된 것이 아니라
	 * 커널이 확정한 값이라야 대상 CPU 계산이 맞는다. */
	dest = irq_data_get_effective_affinity_mask(data);
	/* [한국어] 이 장치가 붙은 버스. */
	pbus = pdev->bus;
	/* [한국어] sysdata 가 hbus 구조체의 **맨 앞** 필드라 이 변환이 성립한다. */
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);
	/* [한국어] 호스트와 주고받을 VMBus 채널. */
	channel = hbus->hdev->channel;
	/* [한국어] devfn 을 Windows 슬롯 번호로 바꿔 게스트 쪽 장치를 찾는다. 참조가 올라간다. */
	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(pdev->devfn));
	/* [한국어] 장치를 못 찾으면 — 그 사이에 제거됐다는 뜻이다. */
	if (!hpdev)
		/* [한국어] 빈 메시지를 돌려주는 경로로 간다. */
		goto return_null_message;

	/* Free any previous message that might have already been composed. */
	if (data->chip_data && !multi_msi) {
		int_desc = data->chip_data;
		/* [한국어] 칩 데이터를 먼저 비운 뒤 해제한다. 순서를 바꾸면 해제된 뒤에도
		 * 포인터가 남아 다른 경로가 그것을 볼 수 있다. */
		data->chip_data = NULL;
		/* [한국어] 이전 서술을 호스트에서도 지운다 — 게스트 메모리만 놓으면
		 * 호스트 쪽 인터럽트가 남는다. */
		hv_int_desc_free(hpdev, int_desc);
	/* [한국어] 이전 메시지 정리 끝(옆의 상류 주석). */
	}

	int_desc = kzalloc_obj(*int_desc, GFP_ATOMIC);
	/* [한국어] 서술을 잡지 못하면 — GFP_ATOMIC 이라 실패할 수 있다. */
	if (!int_desc)
		/* [한국어] 장치 참조를 놓고 물러나는 경로로 간다. */
		goto drop_reference;

	if (multi_msi) {
		/*
		 * If this is not the first MSI of Multi MSI, we already have
		 * a mapping.  Can exit early.
		 */
		if (msi_desc->irq != data->irq) {
			data->chip_data = int_desc;
			/* [한국어] 첫 벡터의 주소 하위 32비트와, */
			int_desc->address = msi_desc->msg.address_lo |
					    /* [한국어] 상위 32비트를 합쳐 64비트 주소를 만든다. */
					    (u64)msi_desc->msg.address_hi << 32;
			/* [한국어] 데이터는 첫 벡터의 값에 **번호 차이를 더한다** — multi-MSI 는
			 * 연속된 데이터 값이 연속된 벡터를 뜻하기 때문이다. */
			int_desc->data = msi_desc->msg.data +
					 (data->irq - msi_desc->irq);
			msg->address_hi = msi_desc->msg.address_hi;
			/* [한국어] 주소는 모든 벡터가 같으므로 첫 벡터의 값을 그대로 쓴다. */
			msg->address_lo = msi_desc->msg.address_lo;
			/* [한국어] 데이터만 위에서 계산한 값을 쓴다. */
			msg->data = int_desc->data;
			/* [한국어] 여기서 돌아가므로 장치 참조를 놓는다. */
			put_pcichild(hpdev);
			return;
		}
		/*
		 * The vector we select here is a dummy value.  The correct
		 * value gets sent to the hypervisor in unmask().  This needs
		 * to be aligned with the count, and also not zero.  Multi-msi
		 * is powers of 2 up to 32, so 32 will always work here.
		 */
		vector = 32;
		vector_count = msi_desc->nvec_used;
		/* [한국어] multi-MSI 는 모든 벡터가 한 CPU 로 가야 하므로 전용 계산을 쓴다. */
		cpu = hv_compose_multi_msi_req_get_cpu();
	/* [한국어] 단일 벡터면 — */
	} else {
		vector = hv_msi_get_int_vector(data);
		/* [한국어] 벡터 하나만 요청한다. */
		vector_count = 1;
		/* [한국어] 친화도 마스크에서 대상 CPU 를 고른다. */
		cpu = hv_compose_msi_req_get_cpu(dest);
	/* [한국어] 벡터 수와 대상 CPU 가 정해졌다. */
	}

	/*
	 * hv_compose_msi_req_v1 and v2 are for x86 only, meaning 'vector'
	 * can't exceed u8. Cast 'vector' down to u8 for v1/v2 explicitly
	 * for better readability.
	 */
	memset(&ctxt, 0, sizeof(ctxt));
	init_completion(&comp.comp_pkt.host_event);
	ctxt.pci_pkt.completion_func = hv_pci_compose_compl;
	/* [한국어] 완료 문맥을 패킷에 매단다. 응답이 오면 위에서 건 콜백이 이것을 깨운다. */
	ctxt.pci_pkt.compl_ctxt = &comp;

	switch (hbus->protocol_version) {
	/* [한국어] 1.1 이면, */
	case PCI_PROTOCOL_VERSION_1_1:
		/* [한국어] 가장 오래된 형식으로 채운다 — 벡터가 8비트라 캐스팅이 필요하다. */
		size = hv_compose_msi_req_v1(&ctxt.int_pkts.v1,
					hpdev->desc.win_slot.slot,
					(u8)vector,
					vector_count);
		break;

	case PCI_PROTOCOL_VERSION_1_2:
	/* [한국어] 1.3 도 같은 형식을 쓴다 — 두 판본이 MSI 요청에서는 차이가 없다. */
	case PCI_PROTOCOL_VERSION_1_3:
		size = hv_compose_msi_req_v2(&ctxt.int_pkts.v2,
					cpu,
					hpdev->desc.win_slot.slot,
					(u8)vector,
					vector_count);
		break;

	case PCI_PROTOCOL_VERSION_1_4:
		/* [한국어] 1.4 는 벡터를 32비트로 넓혀, 캐스팅 없이 그대로 넘긴다. */
		size = hv_compose_msi_req_v3(&ctxt.int_pkts.v3,
					cpu,
					hpdev->desc.win_slot.slot,
					vector,
					vector_count);
		break;

	default:
		/* As we only negotiate protocol versions known to this driver,
		 * this path should never hit. However, this is it not a hot
		 * path so we print a message to aid future updates.
		 */
		dev_err(&hbus->hdev->device,
			"Unexpected vPCI protocol, update driver.");
		goto free_int_desc;
	}

	ret = vmbus_sendpacket_getid(hpdev->hbus->hdev->channel, &ctxt.int_pkts,
				     size, (unsigned long)&ctxt.pci_pkt,
				     &trans_id, VM_PKT_DATA_INBAND,
				     VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret) {
		dev_err(&hbus->hdev->device,
			"Sending request for interrupt failed: 0x%x",
			comp.comp_pkt.completion_status);
		goto free_int_desc;
	}

	/*
	 * Prevents hv_pci_onchannelcallback() from running concurrently
	 * in the tasklet.
	 */
	tasklet_disable_in_atomic(&channel->callback_event);

	/*
	 * Since this function is called with IRQ locks held, can't
	 * do normal wait for completion; instead poll.
	 */
	while (!try_wait_for_completion(&comp.comp_pkt.host_event)) {
		unsigned long flags;

		/* 0xFFFF means an invalid PCI VENDOR ID. */
		if (hv_pcifront_get_vendor_id(hpdev) == 0xFFFF) {
			dev_err_once(&hbus->hdev->device,
				     "the device has gone\n");
			goto enable_tasklet;
		}

		/*
		 * Make sure that the ring buffer data structure doesn't get
		 * freed while we dereference the ring buffer pointer.  Test
		 * for the channel's onchannel_callback being NULL within a
		 * sched_lock critical section.  See also the inline comments
		 * in vmbus_reset_channel_cb().
		 */
		spin_lock_irqsave(&channel->sched_lock, flags);
		if (unlikely(channel->onchannel_callback == NULL)) {
			spin_unlock_irqrestore(&channel->sched_lock, flags);
			goto enable_tasklet;
		}
		hv_pci_onchannelcallback(hbus);
		spin_unlock_irqrestore(&channel->sched_lock, flags);

		udelay(100);
	}

	tasklet_enable(&channel->callback_event);

	if (comp.comp_pkt.completion_status < 0) {
		dev_err(&hbus->hdev->device,
			"Request for interrupt failed: 0x%x",
			comp.comp_pkt.completion_status);
		goto free_int_desc;
	}

	/*
	 * Record the assignment so that this can be unwound later. Using
	 * irq_set_chip_data() here would be appropriate, but the lock it takes
	 * is already held.
	 */
	*int_desc = comp.int_desc;
	data->chip_data = int_desc;

	/* Pass up the result. */
	msg->address_hi = comp.int_desc.address >> 32;
	msg->address_lo = comp.int_desc.address & 0xffffffff;
	msg->data = comp.int_desc.data;

	put_pcichild(hpdev);
	return;

enable_tasklet:
	tasklet_enable(&channel->callback_event);
	/*
	 * The completion packet on the stack becomes invalid after 'return';
	 * remove the ID from the VMbus requestor if the identifier is still
	 * mapped to/associated with the packet.  (The identifier could have
	 * been 're-used', i.e., already removed and (re-)mapped.)
	 *
	 * Cf. hv_pci_onchannelcallback().
	 */
	vmbus_request_addr_match(channel, trans_id, (unsigned long)&ctxt.pci_pkt);
free_int_desc:
	kfree(int_desc);
drop_reference:
	put_pcichild(hpdev);
return_null_message:
	msg->address_hi = 0;
	msg->address_lo = 0;
	msg->data = 0;
}

/* [한국어]
 * hv_pcie_init_dev_msi_info - 장치별 MSI 도메인 정보를 이 드라이버에 맞게 고친다
 *
 * @dev: 대상 장치.
 * @domain: 만들 MSI 도메인.
 * @real_parent: 실제 부모 도메인.
 * @info: 채울 msi_domain_info.
 * @return: 성공하면 true.
 *
 * **MSI 라이브러리가 기본값을 채운 뒤 이 드라이버가 세 가지를 덧쓴다.**
 * msi_lib_init_dev_msi_info() 가 먼저 공통 부분을 채우며, 실패하면
 * 그대로 false 를 올린다.
 *
 * 세 가지 수정은 이렇다.
 * 1. **msi_prepare 를 아키텍처별 함수로 바꾼다.** x86 은 pci_msi_prepare,
 *    ARM64 는 NULL 이며, 파일 앞쪽 #define 이 그 이름을 정한다.
 * 2. **친화도 설정을 부모에게 넘기도록 한다** -- 이 계층은 친화도를
 *    스스로 다루지 않는다.
 * 3. **x86 에서만 IRQCHIP_MOVE_DEFERRED 를 켠다.**
 *    인터럽트 대상 변경을 곧바로 하지 않고 다음 인터럽트 때로 미루라는
 *    뜻이며, x86 에서 재타게팅이 하이퍼콜을 거치기 때문으로 보이나
 *    그 사정이 코드에 적혀 있지는 않다.
 *
 * **IS_ENABLED 를 쓰므로 조건부 컴파일이 아니라 보통의 if 다** --
 * 컴파일러가 상수 조건을 접어 없애므로 결과는 같고, 양쪽 갈래가 모두
 * 문법 검사를 받는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(MSI 도메인 생성 경로).
 *
 * 호출 체인:
 *   MSI 코어(hv_pcie_msi_parent_ops.init_dev_msi_info) -> [이 함수]
 *     -> msi_lib_init_dev_msi_info()
 */
static bool hv_pcie_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				      struct irq_domain *real_parent, struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;

	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;

	info->ops->msi_prepare = hv_msi_prepare;

	chip->irq_set_affinity = irq_chip_set_affinity_parent;

	if (IS_ENABLED(CONFIG_X86))
		chip->flags |= IRQCHIP_MOVE_DEFERRED;

	return true;
}

#define HV_PCIE_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS		| \
				    MSI_FLAG_USE_DEF_CHIP_OPS		| \
				    MSI_FLAG_PCI_MSI_MASK_PARENT)
#define HV_PCIE_MSI_FLAGS_SUPPORTED (MSI_FLAG_MULTI_PCI_MSI		| \
				     MSI_FLAG_PCI_MSIX			| \
				     MSI_FLAG_PCI_MSIX_ALLOC_DYN	| \
				     MSI_GENERIC_FLAGS_MASK)

static const struct msi_parent_ops hv_pcie_msi_parent_ops = {
	.required_flags		= HV_PCIE_MSI_FLAGS_REQUIRED,
	.supported_flags	= HV_PCIE_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= HV_MSI_CHIP_FLAGS,
	.prefix			= "HV-",
	.init_dev_msi_info	= hv_pcie_init_dev_msi_info,
};

/* HW Interrupt Chip Descriptor */
static struct irq_chip hv_msi_irq_chip = {
	.name			= "Hyper-V PCIe MSI",
	.irq_compose_msi_msg	= hv_compose_msi_msg,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_ack		= irq_chip_ack_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_mask		= hv_irq_mask,
	.irq_unmask		= hv_irq_unmask,
};

/* [한국어]
 * hv_pcie_domain_alloc - MSI 도메인의 alloc 콜백
 *
 * @d: 이 드라이버의 MSI 도메인.
 * @virq: 첫 리눅스 IRQ 번호.
 * @nr_irqs: 요청 개수.
 * @arg: 부모에게 그대로 넘길 인자.
 * @return: 성공 0, 실패면 음수.
 *
 * **부모에게 먼저 만들어 달라고 한 뒤 이 계층의 irq_chip 을 붙인다.**
 * 계층 IRQ 도메인의 전형적인 형태다.
 *
 * **원문 주석이 TODO 를 남겨 두었다** -- tran_int_desc 를 만들고 채우는
 * 일이 hv_compose_msi_msg() 에 있는데 여기로 옮겨야 한다는 것이다.
 * 그렇게 하면 그 함수가 IRQ 락 아래에서 GFP_ATOMIC 할당을 하지 않아도 된다.
 *
 * **hwirq 를 0 으로 둔다** -- 이 계층에는 의미 있는 하드웨어 번호가 없고,
 * 실제 벡터는 부모가 관리하기 때문이다.
 *
 * **x86 에서만 에지 핸들러를 명시적으로 건다.**
 * MSI 는 늘 에지 트리거이지만, x86 갈래에서는 그것을 여기서 정해 주어야
 * 하는 것으로 보인다 -- 그 사정이 코드에 적혀 있지는 않다.
 *
 * **for 문 안에서 int i 를 선언한다** -- C99 이후 형태이며,
 * 이 파일의 다른 반복문들과 다른 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 할당 경로).
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> irq_domain_alloc_irqs_parent()
 */
static int hv_pcie_domain_alloc(struct irq_domain *d, unsigned int virq, unsigned int nr_irqs,
			       void *arg)
{
	/*
	 * TODO: Allocating and populating struct tran_int_desc in hv_compose_msi_msg()
	 * should be moved here.
	 */
	int ret;

	ret = irq_domain_alloc_irqs_parent(d, virq, nr_irqs, arg);
	if (ret < 0)
		return ret;

	for (int i = 0; i < nr_irqs; i++) {
		irq_domain_set_hwirq_and_chip(d, virq + i, 0, &hv_msi_irq_chip, NULL);
		if (IS_ENABLED(CONFIG_X86))
			__irq_set_handler(virq + i, handle_edge_irq, 0, "edge");
	}

	return 0;
}

/* [한국어]
 * hv_pcie_domain_free - MSI 도메인의 free 콜백
 *
 * @d: 이 드라이버의 MSI 도메인.
 * @virq: 첫 리눅스 IRQ 번호.
 * @nr_irqs: 놓을 개수.
 * @return: 없음.
 *
 * **하나씩 hv_msi_free() 로 호스트에서 지운 뒤 상위 계층을 정리한다.**
 * 호스트가 인터럽트 리디렉션 테이블을 관리하므로 그쪽 항목부터 지워야 한다.
 *
 * **irq_domain_free_irqs_top 을 쓰는 것이 요점이다** -- 이 도메인과
 * 그 위(부모)를 함께 정리한다. hv_msi_free() 가 호스트 쪽만 다루므로
 * 커널 쪽 자료구조는 이 호출이 맡는다.
 *
 * **순서가 중요하다** -- 먼저 호스트에 삭제를 알리고 그다음 커널 구조를
 * 놓는다. 반대로 하면 hv_msi_free() 가 irq_data 를 찾지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(IRQ 해제 경로).
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> hv_msi_free(), irq_domain_free_irqs_top()
 */
static void hv_pcie_domain_free(struct irq_domain *d, unsigned int virq, unsigned int nr_irqs)
{
	for (int i = 0; i < nr_irqs; i++)
		hv_msi_free(d, virq + i);

	irq_domain_free_irqs_top(d, virq, nr_irqs);
}

static const struct irq_domain_ops hv_pcie_domain_ops = {
	.alloc	= hv_pcie_domain_alloc,
	.free	= hv_pcie_domain_free,
};

/**
 * hv_pcie_init_irq_domain() - Initialize IRQ domain
 * @hbus:	The root PCI bus
 *
 * This function creates an IRQ domain which will be used for
 * interrupts from devices that have been passed through.  These
 * devices only support MSI and MSI-X, not line-based interrupts
 * or simulations of line-based interrupts through PCIe's
 * fabric-layer messages.  Because interrupts are remapped, we
 * can support multi-message MSI here.
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어]
 * hv_pcie_init_irq_domain - 이 버스를 위한 MSI IRQ 도메인을 만든다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 성공 0, 실패면 -ENODEV.
 *
 * **상류 주석이 이 도메인의 성격을 밝힌다** -- 통과된 장치들의 인터럽트를
 * 다루며, 그 장치들은 **MSI 와 MSI-X 만 지원하고 선 기반 인터럽트나
 * 그것을 흉내 내는 PCIe 패브릭 메시지는 지원하지 않는다.**
 * 그리고 인터럽트가 리맵되므로 다중 메시지 MSI 도 지원할 수 있다.
 *
 * **부모 도메인을 아키텍처별 함수에서 얻는다** -- x86 은
 * x86_vector_domain, ARM64 는 이 드라이버가 만든 GIC 위 도메인이다.
 *
 * **host_data 에 hbus 를 담아 두는 것이 요점이다.**
 * hv_msi_free() 같은 콜백이 domain->host_data 로 버스를 되찾는다.
 *
 * 만든 뒤 bridge 의 device 에 그 도메인을 붙인다 -- 그래야 그 아래
 * PCI 장치들이 MSI 를 요청할 때 이 도메인으로 온다.
 *
 * **해제하는 짝이 없다** -- 실패 경로에서만 정리되며, 정상 종료 시에는
 * hv_pci_remove() 가 irq_domain_remove 를 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   hv_pci_probe -> [이 함수]
 *     -> hv_pci_get_root_domain(), msi_create_parent_irq_domain()
 */
static int hv_pcie_init_irq_domain(struct hv_pcibus_device *hbus)
{
	struct irq_domain_info info = {
		.fwnode		= hbus->fwnode,
		.ops		= &hv_pcie_domain_ops,
		.host_data	= hbus,
		.parent		= hv_pci_get_root_domain(),
	};

	hbus->irq_domain = msi_create_parent_irq_domain(&info, &hv_pcie_msi_parent_ops);
	if (!hbus->irq_domain) {
		dev_err(&hbus->hdev->device,
			"Failed to build an MSI IRQ domain\n");
		return -ENODEV;
	}

	dev_set_msi_domain(&hbus->bridge->dev, hbus->irq_domain);

	return 0;
}

/**
 * get_bar_size() - Get the address space consumed by a BAR
 * @bar_val:	Value that a BAR returned after -1 was written
 *              to it.
 *
 * This function returns the size of the BAR, rounded up to 1
 * page.  It has to be rounded up because the hypervisor's page
 * table entry that maps the BAR into the VM can't specify an
 * offset within a page.  The invariant is that the hypervisor
 * must place any BARs of smaller than page length at the
 * beginning of a page.
 *
 * Return:	Size in bytes of the consumed MMIO space.
 */
/* [한국어]
 * get_bar_size - BAR 에 -1 을 쓴 뒤 읽은 값에서 크기를 구한다
 *
 * @bar_val: BAR 에 -1 을 쓰고 되읽은 값.
 * @return: 그 BAR 가 차지하는 MMIO 크기(바이트).
 *
 * **PCI 의 BAR 크기 알아내기 방식이 그대로 쓰인다** -- 되읽은 값에서
 * 주소 비트만 남기고 뒤집어 1 을 더하면 크기가 나온다.
 * `1 + ~(bar_val & MASK)` 가 그 계산이다.
 *
 * **그 결과를 페이지 크기로 올림하는 것이 이 함수의 요점이며,
 * 상류 주석이 그 이유를 밝힌다** -- 하이퍼바이저가 BAR 를 VM 에
 * 매핑하는 페이지 테이블 항목이 페이지 안의 오프셋을 지정할 수 없기
 * 때문이다. 그래서 **페이지보다 작은 BAR 는 반드시 페이지 시작에
 * 놓여야 하며**, 그 약속이 다른 코드에서 지켜진다고 주석이 밝힌다.
 *
 * **IO BAR 는 다루지 않는다** -- 마스크가 메모리용이며,
 * survey_child_resources() 가 IO BAR 를 만나면 오류를 남긴다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산이다.
 *
 * 호출 체인:
 *   survey_child_resources / prepopulate_bars -> [이 함수]
 */
static u64 get_bar_size(u64 bar_val)
{
	return round_up((1 + ~(bar_val & PCI_BASE_ADDRESS_MEM_MASK)),
			PAGE_SIZE);
}

/**
 * survey_child_resources() - Total all MMIO requirements
 * @hbus:	Root PCI bus, as understood by this driver
 */
/* [한국어]
 * survey_child_resources - 자식 장치들이 필요로 하는 MMIO 총량을 센다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 없음.
 *
 * **호스트에게 물어본 BAR 값들을 모두 더해 이 버스가 얼마나 큰 MMIO 창을
 * 잡아야 하는지 알아낸다.** 그 결과가 hbus->low_mmio_space 와
 * high_mmio_space 이며, hv_pci_allocate_bridge_windows() 가 그것으로
 * 실제 창을 잡는다.
 *
 * **맨 앞의 xchg 가 이 함수의 성격을 말해 준다.**
 * survey_event 를 원자적으로 꺼내 NULL 로 바꾸므로,
 * **기다리는 쪽이 없으면 계산조차 하지 않고 물러난다.**
 * 원문 주석이 그 뜻을 밝힌다. 이 함수는 장치 목록을 처리하는 워크에서
 * 불리는데, 매번 세는 것은 낭비이기 때문이다.
 *
 * **이미 센 적이 있으면 그대로 깨운다** -- 두 공간 중 하나라도 0 이
 * 아니면 앞서 계산한 값이 남아 있다는 뜻이다.
 *
 * **정렬을 따로 추적하지 않는 근거가 상류 주석에 있다** --
 * PCI 규격상 모든 메모리 영역이 2의 거듭제곱 크기이고 그 크기에 정렬되므로,
 * **그냥 더하기만 해도 충분하다.**
 *
 * **64비트 BAR 는 두 칸을 차지한다** -- 그래서 `++i` 로 다음 칸을 함께
 * 읽어 상위 32비트로 쓰고 반복 색인을 한 칸 더 밀어 준다.
 * 32비트 BAR 는 상위를 모두 1 로 채워 get_bar_size 가 올바로 계산하게 한다.
 *
 * **IO BAR 를 만나면 오류만 남기고 계속 진행한다** -- 이 드라이버는
 * 메모리 BAR 만 다루기 때문이다.
 *
 * 실행 컨텍스트: 워크 문맥(프로세스 컨텍스트). 스핀락을 잡는다.
 *
 * 호출 체인:
 *   pci_devices_present_work -> [이 함수] -> get_bar_size(), complete()
 */
static void survey_child_resources(struct hv_pcibus_device *hbus)
{
	struct hv_pci_dev *hpdev;
	resource_size_t bar_size = 0;
	unsigned long flags;
	struct completion *event;
	u64 bar_val;
	int i;

	/* If nobody is waiting on the answer, don't compute it. */
	event = xchg(&hbus->survey_event, NULL);
	if (!event)
		return;

	/* If the answer has already been computed, go with it. */
	if (hbus->low_mmio_space || hbus->high_mmio_space) {
		complete(event);
		return;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/*
	 * Due to an interesting quirk of the PCI spec, all memory regions
	 * for a child device are a power of 2 in size and aligned in memory,
	 * so it's sufficient to just add them up without tracking alignment.
	 */
	list_for_each_entry(hpdev, &hbus->children, list_entry) {
		for (i = 0; i < PCI_STD_NUM_BARS; i++) {
			if (hpdev->probed_bar[i] & PCI_BASE_ADDRESS_SPACE_IO)
				dev_err(&hbus->hdev->device,
					"There's an I/O BAR in this list!\n");

			if (hpdev->probed_bar[i] != 0) {
				/*
				 * A probed BAR has all the upper bits set that
				 * can be changed.
				 */

				bar_val = hpdev->probed_bar[i];
				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					bar_val |=
					((u64)hpdev->probed_bar[++i] << 32);
				else
					bar_val |= 0xffffffff00000000ULL;

				bar_size = get_bar_size(bar_val);

				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					hbus->high_mmio_space += bar_size;
				else
					hbus->low_mmio_space += bar_size;
			}
		}
	}

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	complete(event);
}

/**
 * prepopulate_bars() - Fill in BARs with defaults
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * The core PCI driver code seems much, much happier if the BARs
 * for a device have values upon first scan. So fill them in.
 * The algorithm below works down from large sizes to small,
 * attempting to pack the assignments optimally. The assumption,
 * enforced in other parts of the code, is that the beginning of
 * the memory-mapped I/O space will be aligned on the largest
 * BAR size.
 */
/* [한국어]
 * prepopulate_bars - BAR 에 기본 주소를 미리 채워 넣는다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 없음.
 *
 * **상류 주석이 이 함수가 필요한 이유를 밝힌다** -- 코어 PCI 드라이버가
 * 처음 훑을 때 BAR 에 값이 들어 있으면 훨씬 순조롭게 동작한다.
 * 그래서 게스트가 미리 주소를 배정해 둔다.
 *
 * **큰 것부터 작은 것으로 내려가며 채우는 것이 이 함수의 알고리즘이다.**
 * 상류 주석대로 그렇게 해야 빈틈 없이 채울 수 있으며,
 * **MMIO 공간의 시작이 가장 큰 BAR 크기에 정렬되어 있다는 전제** 를
 * 다른 코드가 지켜 준다.
 *
 * **시작 크기를 최상위 비트로 구한다** -- `1ULL << (63 - __builtin_clzll(x))`
 * 가 x 이하의 가장 큰 2의 거듭제곱이다. 그것을 바깥 루프가 매번 반으로
 * 줄이며 그 크기의 BAR 만 채운다.
 *
 * **맨 앞에서 메모리 활성 비트를 꺼야 하는 이유가 상류 주석에 자세히 있다.**
 * 최대 절전 경로에서 장치가 잠들었다 깨어났다 다시 잠드는 과정 때문에
 * 그 비트가 이미 켜져 있을 수 있는데, **켜져 있으면 Hyper-V 가 아래의
 * BAR 갱신을 조용히 무시하고**, 그러면 그 장치의 레지스터를 읽을 때
 * 늘 0xFFFFFFFF 가 나와 드라이버가 동작하지 못한다.
 *
 * **끝난 뒤에도 메모리 활성 비트를 켜지 않는 이유도 주석에 있다** --
 * 코어 PCI 드라이버가 그 비트를 요구하지 않으며, 오히려 꺼 두어야
 * BAR 탐색 과정에서 Hyper-V 가 가상 BAR 를 물리 BAR 에 붙였다 뗐다
 * 되풀이하지 않는다. **BAR 가 크면 그 차이가 VM 부팅 시간에 크게 나타난다.**
 *
 * **64비트 BAR 는 32비트씩 두 번 쓴다** -- 하위를 먼저 쓰고 색인을 밀어
 * 상위를 쓴다. 크기가 맞지 않으면 그 자리도 건너뛰어야 하므로
 * `i++; continue;` 가 함께 나온다.
 *
 * **하위 주소에 0xffffff00 마스크를 거는 것이 눈에 띈다** --
 * BAR 의 하위 비트는 속성 표시라 주소가 아니기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 쥔 채로 설정공간에 접근한다.
 *
 * 호출 체인:
 *   hv_pci_probe / hv_pci_resume -> [이 함수]
 *     -> _hv_pcifront_read_config(), _hv_pcifront_write_config(), get_bar_size()
 */
static void prepopulate_bars(struct hv_pcibus_device *hbus)
{
	resource_size_t high_size = 0;
	resource_size_t low_size = 0;
	resource_size_t high_base = 0;
	resource_size_t low_base = 0;
	resource_size_t bar_size;
	struct hv_pci_dev *hpdev;
	unsigned long flags;
	u64 bar_val;
	u32 command;
	bool high;
	int i;

	if (hbus->low_mmio_space) {
		low_size = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		low_base = hbus->low_mmio_res->start;
	}

	if (hbus->high_mmio_space) {
		high_size = 1ULL <<
			(63 - __builtin_clzll(hbus->high_mmio_space));
		high_base = hbus->high_mmio_res->start;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/*
	 * Clear the memory enable bit, in case it's already set. This occurs
	 * in the suspend path of hibernation, where the device is suspended,
	 * resumed and suspended again: see hibernation_snapshot() and
	 * hibernation_platform_enter().
	 *
	 * If the memory enable bit is already set, Hyper-V silently ignores
	 * the below BAR updates, and the related PCI device driver can not
	 * work, because reading from the device register(s) always returns
	 * 0xFFFFFFFF (PCI_ERROR_RESPONSE).
	 */
	list_for_each_entry(hpdev, &hbus->children, list_entry) {
		_hv_pcifront_read_config(hpdev, PCI_COMMAND, 2, &command);
		command &= ~PCI_COMMAND_MEMORY;
		_hv_pcifront_write_config(hpdev, PCI_COMMAND, 2, command);
	}

	/* Pick addresses for the BARs. */
	do {
		list_for_each_entry(hpdev, &hbus->children, list_entry) {
			for (i = 0; i < PCI_STD_NUM_BARS; i++) {
				bar_val = hpdev->probed_bar[i];
				if (bar_val == 0)
					continue;
				high = bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64;
				if (high) {
					bar_val |=
						((u64)hpdev->probed_bar[i + 1]
						 << 32);
				} else {
					bar_val |= 0xffffffffULL << 32;
				}
				bar_size = get_bar_size(bar_val);
				if (high) {
					if (high_size != bar_size) {
						i++;
						continue;
					}
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(high_base & 0xffffff00));
					i++;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4, (u32)(high_base >> 32));
					high_base += bar_size;
				} else {
					if (low_size != bar_size)
						continue;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(low_base & 0xffffff00));
					low_base += bar_size;
				}
			}
			if (high_size <= 1 && low_size <= 1) {
				/*
				 * No need to set the PCI_COMMAND_MEMORY bit as
				 * the core PCI driver doesn't require the bit
				 * to be pre-set. Actually here we intentionally
				 * keep the bit off so that the PCI BAR probing
				 * in the core PCI driver doesn't cause Hyper-V
				 * to unnecessarily unmap/map the virtual BARs
				 * from/to the physical BARs multiple times.
				 * This reduces the VM boot time significantly
				 * if the BAR sizes are huge.
				 */
				break;
			}
		}

		high_size >>= 1;
		low_size >>= 1;
	}  while (high_size || low_size);

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
}

/*
 * Assign entries in sysfs pci slot directory.
 *
 * Note that this function does not need to lock the children list
 * because it is called from pci_devices_present_work which
 * is serialized with hv_eject_device_work because they are on the
 * same ordered workqueue. Therefore hbus->children list will not change
 * even when pci_create_slot sleeps.
 */
/* [한국어]
 * hv_pci_assign_slots - sysfs 의 PCI 슬롯 항목을 만든다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 없음.
 *
 * **장치마다 sysfs 슬롯 디렉터리를 만들어 준다.**
 * 이름으로 **장치의 일련번호(desc.ser)를 쓰는 것이 특징이다** --
 * Hyper-V 가 장치마다 고유한 일련번호를 주므로, 그것이 사용자에게
 * 가장 뜻 있는 이름이 된다.
 *
 * **락을 잡지 않는 이유가 상류 주석에 자세히 있다** --
 * 이 함수는 pci_devices_present_work 에서 불리고,
 * 그것은 hv_eject_device_work 와 **같은 정렬 워크큐에 있어 직렬화된다.**
 * 그래서 children 목록이 바뀌지 않으며, pci_create_slot 이 잠들어도 안전하다.
 *
 * **이미 슬롯이 있으면 건너뛴다** -- 이 함수가 장치 목록이 바뀔 때마다
 * 불리므로 같은 장치를 두 번 만들지 않아야 한다.
 *
 * **실패해도 계속 진행한다** -- 경고만 남기고 pci_slot 을 NULL 로 되돌린다.
 * 슬롯 항목이 없어도 장치 자체는 동작하기 때문이다.
 *
 * 실행 컨텍스트: 워크 문맥. pci_create_slot 이 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_devices_present_work / hv_pci_probe -> [이 함수] -> pci_create_slot()
 */
static void hv_pci_assign_slots(struct hv_pcibus_device *hbus)
{
	struct hv_pci_dev *hpdev;
	char name[SLOT_NAME_SIZE];
	int slot_nr;

	list_for_each_entry(hpdev, &hbus->children, list_entry) {
		if (hpdev->pci_slot)
			continue;

		slot_nr = PCI_SLOT(wslot_to_devfn(hpdev->desc.win_slot.slot));
		snprintf(name, SLOT_NAME_SIZE, "%u", hpdev->desc.ser);
		hpdev->pci_slot = pci_create_slot(hbus->bridge->bus, slot_nr,
					  name, NULL);
		if (IS_ERR(hpdev->pci_slot)) {
			pr_warn("pci_create slot %s failed\n", name);
			hpdev->pci_slot = NULL;
		}
	}
}

/*
 * Remove entries in sysfs pci slot directory.
 */
/* [한국어]
 * hv_pci_remove_slots - sysfs 의 PCI 슬롯 항목을 지운다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 없음.
 *
 * **hv_pci_assign_slots() 의 짝이다.** 만들어 둔 슬롯을 모두 지우고
 * 포인터를 NULL 로 되돌린다.
 *
 * **포인터를 비우는 것이 중요하다** -- 그러지 않으면 다시 만들 때
 * 이미 있다고 판단해 건너뛰게 된다.
 *
 * **여기도 락을 잡지 않는다** -- assign 쪽과 같은 이유이며,
 * 버스를 내리는 경로에서 불리므로 목록이 바뀌지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 제거 경로).
 *
 * 호출 체인:
 *   hv_pci_bus_exit / hv_pci_remove -> [이 함수] -> pci_destroy_slot()
 */
static void hv_pci_remove_slots(struct hv_pcibus_device *hbus)
{
	struct hv_pci_dev *hpdev;

	list_for_each_entry(hpdev, &hbus->children, list_entry) {
		if (!hpdev->pci_slot)
			continue;
		pci_destroy_slot(hpdev->pci_slot);
		hpdev->pci_slot = NULL;
	}
}

/*
 * Set NUMA node for the devices on the bus
 */
/* [한국어]
 * hv_pci_assign_numa_node - 장치들의 NUMA 노드를 정한다
 *
 * @hbus: 대상 루트 PCI 버스.
 * @return: 없음.
 *
 * **두 단계로 정한다** -- 먼저 무조건 0번 노드로 두고,
 * 호스트가 알려 준 값이 있으면 그것으로 덮어쓴다.
 *
 * **0 을 기본값으로 두는 이유가 상류 주석에 있다** -- 호스트가 노드를
 * 알려 주지 않을 때 NUMA_NO_NODE 로 두면 커널이 여러 노드에 작업을
 * 흩뿌리는데, **Hyper-V 에서는 그것이 오히려 성능을 떨어뜨린다.**
 *
 * **호스트 값을 쓰려면 두 조건이 맞아야 한다** --
 * NUMA 친화도 플래그가 서 있어야 하고, 그 노드 번호가 이 커널이 아는
 * 노드 수보다 작아야 한다.
 *
 * **numa_map_to_online_node 로 한 번 더 옮기는 이유도 주석에 있다** --
 * KDUMP 커널이나 `numa=off` 로 부팅하면 일부 노드가 오프라인일 수 있어,
 * 호스트가 준 번호를 실제로 쓰이는 노드로 조정해야 한다.
 *
 * **참조를 쥐었다 놓는다** -- 장치마다 hv_pci_dev 를 찾아 flags 와
 * virtual_numa_node 를 읽어야 하기 때문이다.
 *
 * **리눅스 쪽 목록(bus->devices)을 걸으며 Hyper-V 쪽 목록에서 짝을 찾는다** --
 * 두 세계의 장치 목록이 따로 있어 wslot 으로 이어야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 또는 장치 추가 경로).
 *
 * 호출 체인:
 *   hv_pci_probe / pci_devices_present_work -> [이 함수]
 *     -> get_pcichild_wslot(), set_dev_node(), put_pcichild()
 */
static void hv_pci_assign_numa_node(struct hv_pcibus_device *hbus)
{
	struct pci_dev *dev;
	struct pci_bus *bus = hbus->bridge->bus;
	struct hv_pci_dev *hv_dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		hv_dev = get_pcichild_wslot(hbus, devfn_to_wslot(dev->devfn));
		if (!hv_dev)
			continue;

		/*
		 * If the Hyper-V host doesn't provide a NUMA node for the
		 * device, default to node 0. With NUMA_NO_NODE the kernel
		 * may spread work across NUMA nodes, which degrades
		 * performance on Hyper-V.
		 */
		set_dev_node(&dev->dev, 0);

		if (hv_dev->desc.flags & HV_PCI_DEVICE_FLAG_NUMA_AFFINITY &&
		    hv_dev->desc.virtual_numa_node < num_possible_nodes())
			/*
			 * The kernel may boot with some NUMA nodes offline
			 * (e.g. in a KDUMP kernel) or with NUMA disabled via
			 * "numa=off". In those cases, adjust the host provided
			 * NUMA node to a valid NUMA node used by the kernel.
			 */
			set_dev_node(&dev->dev,
				     numa_map_to_online_node(
					     hv_dev->desc.virtual_numa_node));

		put_pcichild(hv_dev);
	}
}

/**
 * create_root_hv_pci_bus() - Expose a new root PCI bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * create_root_hv_pci_bus - 리눅스에 새 루트 PCI 버스를 드러낸다
 *
 * @hbus: 이 드라이버가 아는 루트 PCI 버스.
 * @return: 성공 0, 실패면 음수.
 *
 * **게스트 커널이 이 버스를 보게 되는 순간이다.** 여기까지 오면
 * 호스트와의 협상, 장치 목록 수집, BAR 배정이 모두 끝나 있다.
 *
 * **bridge->sysdata 에 hbus->sysdata 를 넣는 것이 열쇠다** --
 * 설정공간 콜백들이 그 포인터에서 container_of 로 hbus 를 되찾는다.
 * 곧 이 한 줄이 리눅스 PCI 코어와 이 드라이버를 잇는다.
 *
 * **pci_scan_root_bus_bridge 뒤의 다섯 호출이 순서대로 의미가 있다.**
 * 1. NUMA 노드를 정한다 -- 장치가 붙기 전에 해야 드라이버가 올바른
 *    노드에서 메모리를 잡는다.
 * 2. 자원을 배정한다.
 * 3. sysfs 슬롯 항목을 만든다.
 * 4. **pci_bus_add_devices 로 드라이버를 붙인다** -- 이 순간부터
 *    보통의 PCI 드라이버가 장치를 잡는다.
 * 5. 상태를 installed 로 올린다.
 *
 * **pci_lock_rescan_remove 로 감싸는 이유**: 같은 시각 호스트가 보낸
 * 장치 목록 변경이 워크큐에서 처리되며 같은 버스를 훑을 수 있기 때문이다.
 *
 * **상태를 마지막에 바꾼다** -- pci_devices_present_work() 가 그 값을 보고
 * 갈래를 정하므로, 버스가 실제로 준비된 뒤에 올려야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로). 잠들 수 있다.
 *
 * 호출 체인:
 *   hv_pci_probe -> [이 함수]
 *     -> pci_scan_root_bus_bridge(), hv_pci_assign_numa_node(),
 *   hv_pci_assign_slots(), pci_bus_add_devices()
 */
static int create_root_hv_pci_bus(struct hv_pcibus_device *hbus)
{
	int error;
	struct pci_host_bridge *bridge = hbus->bridge;

	bridge->dev.parent = &hbus->hdev->device;
	bridge->sysdata = &hbus->sysdata;
	bridge->ops = &hv_pcifront_ops;

	error = pci_scan_root_bus_bridge(bridge);
	if (error)
		return error;

	pci_lock_rescan_remove();
	hv_pci_assign_numa_node(hbus);
	pci_bus_assign_resources(bridge->bus);
	hv_pci_assign_slots(hbus);
	pci_bus_add_devices(bridge->bus);
	pci_unlock_rescan_remove();
	hbus->state = hv_pcibus_installed;
	return 0;
}

struct q_res_req_compl {
	struct completion host_event;
	struct hv_pci_dev *hpdev;
};

/**
 * q_resource_requirements() - Query Resource Requirements
 * @context:		The completion context.
 * @resp:		The response that came from the host.
 * @resp_packet_size:	The size in bytes of resp.
 *
 * This function is invoked on completion of a Query Resource
 * Requirements packet.
 */
/* [한국어]
 * q_resource_requirements - 자원 요구 조회 응답에서 BAR 값을 챙긴다
 *
 * @context: 요청 때 넘겨 둔 struct q_res_req_compl.
 * @resp: 호스트가 돌려준 응답.
 * @resp_packet_size: 응답 크기.
 * @return: 없음.
 *
 * **호스트가 알려 주는 probed_bar 여섯 개가 이 요청의 목적 전부다.**
 * 그 값은 "BAR 에 0xFFFFFFFF 를 쓰고 되읽으면 무엇이 나오는가" 이며,
 * 게스트는 그것으로 각 BAR 의 크기를 알아낸다.
 *
 * **게스트가 직접 BAR 를 탐색하지 않는 이유**: 설정공간이 호스트를 거쳐
 * 매핑되므로, 탐색을 하려면 매번 호스트와 주고받아야 한다.
 * 그보다 **한 번에 여섯 값을 받아 두는 편이 빠르다.**
 *
 * **크기 검사와 상태 검사를 한 줄에 합쳤다** --
 * 받은 크기가 모자라면 -1 을, 아니면 응답의 상태를 status 에 담고
 * 그것이 음수인지로 갈래를 탄다.
 *
 * **실패해도 completion 은 반드시 부른다** -- if/else 밖에 있으므로
 * 어느 경로로든 요청자가 깨어난다.
 *
 * **실패하면 probed_bar 를 채우지 않는다** -- new_pcichild_device() 가
 * kzalloc 으로 만든 구조체이므로 0 으로 남으며,
 * survey_child_resources() 는 0 인 BAR 를 없는 것으로 다룬다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> complete()
 */
static void q_resource_requirements(void *context, struct pci_response *resp,
				    int resp_packet_size)
{
	struct q_res_req_compl *completion = context;
	struct pci_q_res_req_response *q_res_req =
		(struct pci_q_res_req_response *)resp;
	s32 status;
	int i;

	status = (resp_packet_size < sizeof(*q_res_req)) ? -1 : resp->status;
	if (status < 0) {
		dev_err(&completion->hpdev->hbus->hdev->device,
			"query resource requirements failed: %x\n",
			status);
	} else {
		for (i = 0; i < PCI_STD_NUM_BARS; i++) {
			completion->hpdev->probed_bar[i] =
				q_res_req->probed_bar[i];
		}
	}

	complete(&completion->host_event);
}

/**
 * new_pcichild_device() - Create a new child device
 * @hbus:	The internal struct tracking this root PCI bus.
 * @desc:	The information supplied so far from the host
 *              about the device.
 *
 * This function creates the tracking structure for a new child
 * device and kicks off the process of figuring out what it is.
 *
 * Return: Pointer to the new tracking struct
 */
/* [한국어]
 * new_pcichild_device - 새 자식 장치의 추적 구조체를 만든다
 *
 * @hbus: 이 루트 PCI 버스.
 * @desc: 호스트가 지금까지 알려 준 장치 정보.
 * @return: 만든 구조체, 실패하면 NULL.
 *
 * **호스트가 알려 준 장치 하나를 이 드라이버가 추적하기 시작하는 자리다.**
 * 상류 주석대로 구조체를 만들고 **그것이 무엇인지 알아내는 절차를 시작한다** --
 * 곧 자원 요구 조회를 보낸다.
 *
 * **참조가 2 가 되는 것이 이 함수의 특징이다.**
 * refcount_set 으로 1 을 세우고 get_pcichild 로 하나 더 올린다.
 * 하나는 children 목록이 쥔 것이고, 다른 하나는 호출자에게 돌려주는 것이다.
 * **hv_eject_device_work() 의 주석이 그 둘을 함께 놓는 것을 밝힌다.**
 *
 * **자원 요구 조회를 여기서 보내고 응답까지 기다린다** --
 * 그래야 probed_bar 가 채워진 채로 목록에 오른다.
 * 그 값이 없으면 survey_child_resources() 가 이 장치를 세지 못한다.
 *
 * **목록에 넣는 것이 마지막이다** -- 조회가 실패하면 목록에 오르지 않고
 * 구조체만 해제된다. 그래서 error 라벨이 kfree 하나뿐이다.
 *
 * **hpdev->hbus 를 먼저 채운다** -- 완료 함수가 오류를 남길 때
 * 그 고리로 device 를 찾기 때문이다.
 *
 * 실행 컨텍스트: 워크 문맥(프로세스 컨텍스트). 응답을 기다리며 잠든다.
 *
 * 호출 체인:
 *   pci_devices_present_work -> [이 함수]
 *     -> vmbus_sendpacket(), wait_for_response(), get_pcichild()
 */
static struct hv_pci_dev *new_pcichild_device(struct hv_pcibus_device *hbus,
		struct hv_pcidev_description *desc)
{
	struct hv_pci_dev *hpdev;
	struct pci_child_message *res_req;
	struct q_res_req_compl comp_pkt;
	struct {
		struct pci_packet init_packet;
		u8 buffer[sizeof(struct pci_child_message)];
	} pkt;
	unsigned long flags;
	int ret;

	hpdev = kzalloc_obj(*hpdev);
	if (!hpdev)
		return NULL;

	hpdev->hbus = hbus;

	memset(&pkt, 0, sizeof(pkt));
	init_completion(&comp_pkt.host_event);
	comp_pkt.hpdev = hpdev;
	pkt.init_packet.compl_ctxt = &comp_pkt;
	pkt.init_packet.completion_func = q_resource_requirements;
	res_req = (struct pci_child_message *)pkt.buffer;
	res_req->message_type.type = PCI_QUERY_RESOURCE_REQUIREMENTS;
	res_req->wslot.slot = desc->win_slot.slot;

	ret = vmbus_sendpacket(hbus->hdev->channel, res_req,
			       sizeof(struct pci_child_message),
			       (unsigned long)&pkt.init_packet,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto error;

	if (wait_for_response(hbus->hdev, &comp_pkt.host_event))
		goto error;

	hpdev->desc = *desc;
	refcount_set(&hpdev->refs, 1);
	get_pcichild(hpdev);
	spin_lock_irqsave(&hbus->device_list_lock, flags);

	list_add_tail(&hpdev->list_entry, &hbus->children);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	return hpdev;

error:
	kfree(hpdev);
	return NULL;
}

/**
 * get_pcichild_wslot() - Find device from slot
 * @hbus:	Root PCI bus, as understood by this driver
 * @wslot:	Location on the bus
 *
 * This function looks up a PCI device and returns the internal
 * representation of it.  It acquires a reference on it, so that
 * the device won't be deleted while somebody is using it.  The
 * caller is responsible for calling put_pcichild() to release
 * this reference.
 *
 * Return:	Internal representation of a PCI device
 */
/* [한국어] get_pcichild_wslot - 슬롯 번호로 자식 장치를 찾고 참조를 올린다
 * 
 * @hbus: 이 가상 버스.
 * @wslot: 찾을 Windows 슬롯 번호.
 * @return: 찾은 장치(참조가 올라간 상태), 없으면 NULL.
 * 
 * 호스트가 보내는 모든 장치별 메시지가 슬롯 번호로 대상을 지목하므로,
 * 그 번호를 커널 쪽 객체로 바꾸는 이 함수가 프로토콜 처리의 길목이 된다.
 * 
 * 찾자마자 참조를 올리는 것이 이 함수의 계약이다. 잠금을 놓은 뒤에도 그
 * 객체가 살아 있어야 하기 때문이며, 호출자는 다 쓴 뒤 반드시
 * put_pcichild() 로 돌려줘야 한다.
 * 
 * 목록 잠금이 irqsave 판인 것은 이 목록을 인터럽트 문맥에서도 다루기
 * 때문이다 — hv_pci_onchannelcallback() 이 그 문맥에서 이 함수를 부른다.
 * 
 * 선형 탐색이다. 슬롯이 최대 256개뿐이라 해시 없이도 충분하다.
 * 
 * 실행 컨텍스트: 인터럽트 문맥과 프로세스 컨텍스트 양쪽. 스핀락만 쓰므로
 * 잠들지 않는다.
 * 
 * 에러 경로: 없다. 못 찾으면 NULL 이며 호출자가 그것을 정상 상황으로 다룬다.
 * 
 * 호출 체인:
 *   hv_pci_onchannelcallback() / hv_send_resources_allocated() /
 *   hv_send_resources_released() 등 → [이 함수] → get_pcichild() */
static struct hv_pci_dev *get_pcichild_wslot(struct hv_pcibus_device *hbus,
					     u32 wslot)
{
	unsigned long flags;
	struct hv_pci_dev *iter, *hpdev = NULL;

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each_entry(iter, &hbus->children, list_entry) {
		if (iter->desc.win_slot.slot == wslot) {
			hpdev = iter;
			get_pcichild(hpdev);
			break;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	return hpdev;
}

/**
 * pci_devices_present_work() - Handle new list of child devices
 * @work:	Work struct embedded in struct hv_dr_work
 *
 * "Bus Relations" is the Windows term for "children of this
 * bus."  The terminology is preserved here for people trying to
 * debug the interaction between Hyper-V and Linux.  This
 * function is called when the parent partition reports a list
 * of functions that should be observed under this PCI Express
 * port (bus).
 *
 * This function updates the list, and must tolerate being
 * called multiple times with the same information.  The typical
 * number of child devices is one, with very atypical cases
 * involving three or four, so the algorithms used here can be
 * simple and inefficient.
 *
 * It must also treat the omission of a previously observed device as
 * notification that the device no longer exists.
 *
 * Note that this function is serialized with hv_eject_device_work(),
 * because both are pushed to the ordered workqueue hbus->wq.
 */
/* [한국어]
 * pci_devices_present_work - 호스트가 알려 준 새 장치 목록을 반영한다
 *
 * @work: struct hv_dr_work 안에 박힌 워크 구조체.
 * @return: 없음.
 *
 * **상류 주석이 배경을 자세히 밝힌다.** "Bus Relations" 는 Windows 용어로
 * "이 버스의 자식들" 을 뜻하며, Hyper-V 와 리눅스의 상호작용을 디버깅하는
 * 사람을 위해 그 용어를 그대로 남겨 두었다고 한다.
 *
 * **같은 정보로 여러 번 불려도 견뎌야 하고**, 앞서 보고되었던 장치가
 * 빠져 있으면 그것을 "사라졌다" 는 뜻으로 다뤄야 한다.
 * **hv_eject_device_work() 와 같은 정렬 워크큐에 있어 직렬화된다.**
 *
 * 절차가 다섯이다.
 * 1. **dr_list 에서 가장 마지막 것만 남기고 나머지를 버린다.**
 *    목록 변경이 잇달아 오면 마지막 것이 가장 최신이기 때문이다.
 *    그래서 while 루프가 앞의 것들을 kfree 하며 훑는다.
 * 2. **모든 자식을 "사라졌다" 로 표시한다.**
 * 3. **새 목록에 있는 것을 다시 살린다.** 슬롯·벤더·장치·일련번호 넷이
 *    모두 맞아야 같은 장치로 본다. 없으면 새로 만든다.
 * 4. **여전히 표시가 남은 것을 스택 위 목록으로 옮긴다.**
 *    **do-while 로 한 번에 하나씩 옮기는 것이 눈에 띈다** --
 *    list_move_tail 로 목록을 바꾸므로 순회를 계속할 수 없어,
 *    찾을 때마다 break 하고 처음부터 다시 돈다.
 * 5. **옮긴 것들을 실제로 지운다.** 슬롯 항목을 없애고 참조를 놓는다.
 *
 * **마지막 switch 가 버스 상태에 따라 갈린다.**
 * - **installed** 면 리눅스 코어에 다시 훑으라고 알린다.
 * - **init 이나 probed** 면 아직 버스가 서지 않았으므로
 *   survey_child_resources() 로 MMIO 총량만 센다.
 *
 * **state_lock 뮤텍스가 이 함수 전체를 감싼다** -- 버스 상태를 보고
 * 갈래를 타므로 그 사이에 상태가 바뀌면 안 된다.
 *
 * **dr 이 NULL 이면 곧바로 물러난다** -- 다른 워크가 이미 처리한 경우다.
 *
 * 실행 컨텍스트: 워크 문맥. 뮤텍스와 스핀락을 잡고, 잠들 수 있다.
 *
 * 호출 체인:
 *   워크큐 -> [이 함수]
 *     -> new_pcichild_device(), put_pcichild(), pci_scan_child_bus(),
 *   survey_child_resources()
 */
static void pci_devices_present_work(struct work_struct *work)
{
	u32 child_no;
	bool found;
	/* [한국어] 새 목록에서 꺼낸 장치 서술을 가리킬 포인터. */
	struct hv_pcidev_description *new_desc;
	/* [한국어] 기존 목록을 훑을 때 쓰는 포인터. */
	struct hv_pci_dev *hpdev;
	/* [한국어] 이 워크가 속한 버스. */
	struct hv_pcibus_device *hbus;
	/* [한국어] 사라진 장치를 잠시 옮겨 둘 스택 위 목록. */
	struct list_head removed;
	/* [한국어] 이 워크의 포장 구조체. */
	struct hv_dr_work *dr_wrk;
	/* [한국어] 처리할 목록 스냅샷. NULL 로 시작해, 큐가 비어 있으면 그대로 남는다. */
	struct hv_dr_state *dr = NULL;
	/* [한국어] 저장할 인터럽트 상태. 이 목록을 인터럽트 문맥에서도 다루므로 irqsave 판이 필요하다. */
	unsigned long flags;

	dr_wrk = container_of(work, struct hv_dr_work, wrk);
	/* [한국어] 워크가 속한 버스를 꺼내고, */
	hbus = dr_wrk->bus;
	/* [한국어] 포장은 곧바로 해제한다 — 더 필요한 정보가 없다. */
	kfree(dr_wrk);

	INIT_LIST_HEAD(&removed);

	/* Pull this off the queue and process it if it was the last one. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	while (!list_empty(&hbus->dr_list)) {
		/* [한국어] 큐 맨 앞의 스냅샷을 꺼낸다. */
		dr = list_first_entry(&hbus->dr_list, struct hv_dr_state,
				      /* [한국어] list_head 에서 바깥 구조체를 되찾는다. */
				      list_entry);
		list_del(&dr->list_entry);

		/* Throw this away if the list still has stuff in it. */
		if (!list_empty(&hbus->dr_list)) {
			kfree(dr);
			continue;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
/* [한국어] 루프를 다 돌면 **가장 마지막 스냅샷만** 남는다(옆의 상류 주석) —
 * 중간 것들은 이미 낡았으므로 버린다. 장치 목록은 누적이 아니라
 * 매번 전체를 알려 주는 형식이라 최신 것 하나면 충분하다. */

	if (!dr)
		/* [한국어] 큐가 비어 있었으면 처리할 것이 없다. */
		return;

	mutex_lock(&hbus->state_lock);

	/* First, mark all existing children as reported missing. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each_entry(hpdev, &hbus->children, list_entry) {
		/* [한국어] **먼저 전부 사라진 것으로 표시한다.** 아래에서 새 목록에 있는 것만
		 * 표시를 지우므로, 남은 것이 곧 사라진 장치가 된다. */
		hpdev->reported_missing = true;
	/* [한국어] 표시 끝. */
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
/* [한국어] 이제 새 목록의 장치를 하나씩 확인한다(옆의 상류 주석). */

	/* Next, add back any reported devices. */
	for (child_no = 0; child_no < dr->device_count; child_no++) {
		found = false;
		/* [한국어] 이번에 확인할 장치 서술. */
		new_desc = &dr->func[child_no];

		spin_lock_irqsave(&hbus->device_list_lock, flags);
		/* [한국어] 기존 목록에서 같은 장치를 찾는다. */
		list_for_each_entry(hpdev, &hbus->children, list_entry) {
			/* [한국어] 슬롯 번호와, */
			if ((hpdev->desc.win_slot.slot == new_desc->win_slot.slot) &&
			    /* [한국어] 벤더 ID 와 장치 ID, 일련번호까지 **넷을 모두** 비교한다 —
			     * 같은 슬롯에 다른 장치가 들어온 경우를 새 장치로 다뤄야 하기 때문이다. */
			    (hpdev->desc.v_id == new_desc->v_id) &&
			    (hpdev->desc.d_id == new_desc->d_id) &&
			    (hpdev->desc.ser == new_desc->ser)) {
				hpdev->reported_missing = false;
				/* [한국어] 이미 있는 장치이므로 새로 만들지 않는다. */
				found = true;
			/* [한국어] 일치 확인 끝. */
			}
		}
		spin_unlock_irqrestore(&hbus->device_list_lock, flags);
/* [한국어] 기존 목록에 없으면 — */

		if (!found) {
			/* [한국어] 새 장치를 만든다. */
			hpdev = new_pcichild_device(hbus, new_desc);
			/* [한국어] 만들지 못하면, */
			if (!hpdev)
				/* [한국어] 기록만 남기고 계속한다 — 나머지 장치라도 처리하는 편이 낫다. */
				dev_err(&hbus->hdev->device,
					"couldn't record a child device.\n");
		}
	}

	/* Move missing children to a list on the stack. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	do {
		found = false;
		/* [한국어] 사라진 것으로 남은 장치를 찾는다. */
		list_for_each_entry(hpdev, &hbus->children, list_entry) {
			/* [한국어] 표시가 지워지지 않았으면 이번 목록에 없는 장치다. */
			if (hpdev->reported_missing) {
				/* [한국어] 하나 찾았음을 표시하고, */
				found = true;
				/* [한국어] 목록이 들고 있던 참조를 놓는다. */
				put_pcichild(hpdev);
				list_move_tail(&hpdev->list_entry, &removed);
				/* [한국어] **한 번에 하나만 옮기고 루프를 다시 시작한다** — 순회 중에 목록을
				 * 고치므로, 이어서 훑으면 끊어진 포인터를 따라가게 된다. */
				break;
			}
		}
	} while (found);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
/* [한국어] 옮길 것이 없을 때까지 반복한다. */

	/* Delete everything that should no longer exist. */
	while (!list_empty(&removed)) {
		hpdev = list_first_entry(&removed, struct hv_pci_dev,
					 /* [한국어] 스택 목록에서 하나씩 꺼낸다. */
					 list_entry);
		list_del(&hpdev->list_entry);

		if (hpdev->pci_slot)
			/* [한국어] sysfs 슬롯이 있으면 없앤다 — 잠금 밖이라 잠들 수 있는 이 작업이 가능하다. */
			pci_destroy_slot(hpdev->pci_slot);

		put_pcichild(hpdev);
	}

	switch (hbus->state) {
	/* [한국어] 이미 설치된 버스면 — 새 장치를 곧바로 스캔해 올린다. */
	case hv_pcibus_installed:
		/*
		 * Tell the core to rescan bus
		 * because there may have been changes.
		 */
		pci_lock_rescan_remove();
		pci_scan_child_bus(hbus->bridge->bus);
		hv_pci_assign_numa_node(hbus);
		hv_pci_assign_slots(hbus);
		pci_unlock_rescan_remove();
		break;

	case hv_pcibus_init:
	case hv_pcibus_probed:
		survey_child_resources(hbus);
		break;

	default:
		break;
	}

	mutex_unlock(&hbus->state_lock);

	kfree(dr);
}

/**
 * hv_pci_start_relations_work() - Queue work to start device discovery
 * @hbus:	Root PCI bus, as understood by this driver
 * @dr:		The list of children returned from host
 *
 * Return:  0 on success, -errno on failure
 */
/* [한국어]
 * hv_pci_start_relations_work - 장치 목록 처리를 워크큐에 올린다
 *
 * @hbus: 이 루트 PCI 버스.
 * @dr: 호스트가 알려 준 자식 목록.
 * @return: 성공 0, 실패면 음수.
 *
 * **호스트 알림을 채널 콜백 문맥에서 처리할 수 없으므로 워크로 미룬다.**
 * 실제 처리는 pci_devices_present_work() 가 한다.
 *
 * **중복 워크를 피하는 방식이 이 함수의 요점이다.**
 * dr_list 가 이미 비어 있지 않으면 **앞서 올린 워크가 아직 돌지 않았다는
 * 뜻이므로**, 새 목록만 매달고 워크는 올리지 않는다.
 * 원문 주석이 그 판단을 밝힌다 -- 그 워크가 새 목록도 함께 볼 것이기 때문이다.
 *
 * **그 판단을 락 안에서 해야 한다** -- 목록을 매다는 것과 비어 있는지
 * 보는 것이 나뉘면 두 워크가 올라가거나 하나도 안 올라간다.
 *
 * **워크 구조체를 미리 잡아 두고 필요 없으면 버린다** --
 * 락 안에서 GFP_NOWAIT 할당을 하지 않으려는 것으로 보인다.
 *
 * **GFP_NOWAIT 를 쓰는 이유**: 이 함수가 채널 콜백 문맥에서 불리므로
 * 잠들 수 없다.
 *
 * **버스를 내리는 중이면 무시한다** -- 곧 사라질 버스에 장치를 추가할
 * 필요가 없다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   hv_pci_devices_present / hv_pci_devices_present2 -> [이 함수]
 *     -> queue_work()
 */
static int hv_pci_start_relations_work(struct hv_pcibus_device *hbus,
				       struct hv_dr_state *dr)
{
	struct hv_dr_work *dr_wrk;
	unsigned long flags;
	bool pending_dr;

	if (hbus->state == hv_pcibus_removing) {
		dev_info(&hbus->hdev->device,
			 "PCI VMBus BUS_RELATIONS: ignored\n");
		return -ENOENT;
	}

	dr_wrk = kzalloc_obj(*dr_wrk, GFP_NOWAIT);
	if (!dr_wrk)
		return -ENOMEM;

	INIT_WORK(&dr_wrk->wrk, pci_devices_present_work);
	dr_wrk->bus = hbus;

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	/*
	 * If pending_dr is true, we have already queued a work,
	 * which will see the new dr. Otherwise, we need to
	 * queue a new work.
	 */
	pending_dr = !list_empty(&hbus->dr_list);
	list_add_tail(&dr->list_entry, &hbus->dr_list);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	if (pending_dr)
		kfree(dr_wrk);
	else
		queue_work(hbus->wq, &dr_wrk->wrk);

	return 0;
}

/**
 * hv_pci_devices_present() - Handle list of new children
 * @hbus:      Root PCI bus, as understood by this driver
 * @relations: Packet from host listing children
 *
 * Process a new list of devices on the bus. The list of devices is
 * discovered by VSP and sent to us via VSP message PCI_BUS_RELATIONS,
 * whenever a new list of devices for this bus appears.
 */
/* [한국어]
 * hv_pci_devices_present - 프로토콜 1.1 형식의 장치 목록을 받아 옮긴다
 *
 * @hbus: 이 루트 PCI 버스.
 * @relations: 호스트가 보낸 자식 목록 패킷.
 * @return: 없음.
 *
 * **호스트가 먼저 보내는 알림 중 하나다.** 상류 주석이 밝히듯
 * 이 버스의 장치 목록이 바뀔 때마다 호스트 쪽 VSP 가 보낸다.
 *
 * **패킷의 배열을 이 드라이버의 배열로 한 필드씩 옮긴다.**
 * 왜 그대로 쓰지 않는가 -- **패킷 구조체는 프로토콜 판마다 다르지만
 * hv_pcidev_description 은 하나뿐이기 때문이다.**
 * 그래서 v1 과 v2 두 함수가 같은 목적지 구조체를 채운다.
 *
 * **kzalloc_flex 로 가변 길이 배열을 함께 잡는다** --
 * device_count 만큼의 func 배열이 구조체 끝에 붙는다.
 *
 * **GFP_NOWAIT 를 쓴다** -- 채널 콜백 문맥이라 잠들 수 없다.
 * 할당에 실패하면 조용히 물러나며, **그 목록 변경은 잃는다.**
 *
 * **워크 올리기에 실패하면 여기서 해제한다** -- 성공했다면 워크가
 * 처리한 뒤 해제하므로, 해제 책임이 결과에 따라 갈린다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> hv_pci_start_relations_work()
 */
static void hv_pci_devices_present(struct hv_pcibus_device *hbus,
				   struct pci_bus_relations *relations)
{
	struct hv_dr_state *dr;
	int i;

	dr = kzalloc_flex(*dr, func, relations->device_count, GFP_NOWAIT);
	if (!dr)
		return;

	dr->device_count = relations->device_count;
	for (i = 0; i < dr->device_count; i++) {
		dr->func[i].v_id = relations->func[i].v_id;
		dr->func[i].d_id = relations->func[i].d_id;
		dr->func[i].rev = relations->func[i].rev;
		dr->func[i].prog_intf = relations->func[i].prog_intf;
		dr->func[i].subclass = relations->func[i].subclass;
		dr->func[i].base_class = relations->func[i].base_class;
		dr->func[i].subsystem_id = relations->func[i].subsystem_id;
		dr->func[i].win_slot = relations->func[i].win_slot;
		dr->func[i].ser = relations->func[i].ser;
	}

	if (hv_pci_start_relations_work(hbus, dr))
		kfree(dr);
}

/**
 * hv_pci_devices_present2() - Handle list of new children
 * @hbus:	Root PCI bus, as understood by this driver
 * @relations:	Packet from host listing children
 *
 * This function is the v2 version of hv_pci_devices_present()
 */
/* [한국어]
 * hv_pci_devices_present2 - 프로토콜 1.2 이상 형식의 장치 목록을 받아 옮긴다
 *
 * @hbus: 이 루트 PCI 버스.
 * @relations: 호스트가 보낸 자식 목록 패킷(2판).
 * @return: 없음.
 *
 * **hv_pci_devices_present() 의 2판이며, 옮기는 필드가 둘 더 있다** --
 * flags 와 virtual_numa_node 다.
 *
 * **그 둘이 2판을 만든 이유다.** NUMA 친화도 정보를 호스트가 알려 줄 수
 * 있게 되었고, hv_pci_assign_numa_node() 가 그것을 쓴다.
 * flags 의 HV_PCI_DEVICE_FLAG_NUMA_AFFINITY 비트가 그 값이 유효한지 알려 준다.
 *
 * **나머지 아홉 필드를 옮기는 코드가 1판과 글자 그대로 같다** --
 * 두 함수를 합칠 수도 있었을 자리이나 상류가 나눠 두었다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> hv_pci_start_relations_work()
 */
static void hv_pci_devices_present2(struct hv_pcibus_device *hbus,
				    struct pci_bus_relations2 *relations)
{
	struct hv_dr_state *dr;
	int i;

	dr = kzalloc_flex(*dr, func, relations->device_count, GFP_NOWAIT);
	if (!dr)
		return;

	dr->device_count = relations->device_count;
	for (i = 0; i < dr->device_count; i++) {
		dr->func[i].v_id = relations->func[i].v_id;
		dr->func[i].d_id = relations->func[i].d_id;
		dr->func[i].rev = relations->func[i].rev;
		dr->func[i].prog_intf = relations->func[i].prog_intf;
		dr->func[i].subclass = relations->func[i].subclass;
		dr->func[i].base_class = relations->func[i].base_class;
		dr->func[i].subsystem_id = relations->func[i].subsystem_id;
		dr->func[i].win_slot = relations->func[i].win_slot;
		dr->func[i].ser = relations->func[i].ser;
		dr->func[i].flags = relations->func[i].flags;
		dr->func[i].virtual_numa_node =
			relations->func[i].virtual_numa_node;
	}

	if (hv_pci_start_relations_work(hbus, dr))
		kfree(dr);
}

/**
 * hv_eject_device_work() - Asynchronously handles ejection
 * @work:	Work struct embedded in internal device struct
 *
 * This function handles ejecting a device.  Windows will
 * attempt to gracefully eject a device, waiting 60 seconds to
 * hear back from the guest OS that this completed successfully.
 * If this timer expires, the device will be forcibly removed.
 */
/* [한국어]
 * hv_eject_device_work - 호스트의 장치 뽑기 요청을 실제로 처리한다
 *
 * @work: hv_pci_dev 안에 박힌 워크 구조체.
 * @return: 없음.
 *
 * **상류 주석이 시간 제약을 밝힌다** -- Windows 는 장치를 곱게 뽑으려
 * 시도하며 게스트의 완료 보고를 **60초** 기다린다.
 * 그 안에 답하지 않으면 강제로 제거한다.
 *
 * 절차가 다섯이다.
 * 1. **리눅스 쪽 PCI 장치를 찾아 뗀다.** 원문 주석이 밝히듯
 *    **뽑기 요청은 PCI 버스가 서기 전에도 올 수 있어**
 *    pci_domain_nr(hbus->bridge->bus) 같은 형태를 쓸 수 없다 --
 *    그 버스가 아직 없을 수 있기 때문이다. 그래서 bridge->domain_nr 을
 *    직접 쓴다.
 * 2. children 목록에서 뺀다.
 * 3. sysfs 슬롯 항목을 지운다.
 * 4. **호스트에 완료를 알린다.** 이 메시지를 보내야 호스트가 뽑기를 마친다.
 *    응답을 기다리지 않는 보내고 잊는 방식이다.
 * 5. **참조를 셋 놓는다.** 주석이 각각의 출처를 밝힌다 --
 *    하나는 hv_pci_eject_device() 가 올린 것,
 *    둘은 new_pcichild_device() 가 올린 것이다.
 *    **그 뒤로 hpdev 를 쓰면 안 된다** 고 주석이 못박는다.
 *
 * **state_lock 이 전체를 감싼다** -- pci_devices_present_work() 와
 * 같은 뮤텍스이며, 두 경로가 children 목록을 함께 다루기 때문이다.
 * **게다가 같은 정렬 워크큐에 있어 애초에 동시에 돌지 않는다.**
 *
 * 실행 컨텍스트: 워크 문맥. 뮤텍스를 잡고 잠들 수 있다.
 *
 * 호출 체인:
 *   워크큐 -> [이 함수]
 *     -> pci_stop_and_remove_bus_device(), vmbus_sendpacket(), put_pcichild()
 */
static void hv_eject_device_work(struct work_struct *work)
{
	struct pci_eject_response *ejct_pkt;
	struct hv_pcibus_device *hbus;
	struct hv_pci_dev *hpdev;
	struct pci_dev *pdev;
	unsigned long flags;
	int wslot;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_eject_response)];
	} ctxt;

	hpdev = container_of(work, struct hv_pci_dev, wrk);
	hbus = hpdev->hbus;

	mutex_lock(&hbus->state_lock);

	/*
	 * Ejection can come before or after the PCI bus has been set up, so
	 * attempt to find it and tear down the bus state, if it exists.  This
	 * must be done without constructs like pci_domain_nr(hbus->bridge->bus)
	 * because hbus->bridge->bus may not exist yet.
	 */
	wslot = wslot_to_devfn(hpdev->desc.win_slot.slot);
	pdev = pci_get_domain_bus_and_slot(hbus->bridge->domain_nr, 0, wslot);
	if (pdev) {
		pci_lock_rescan_remove();
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
		pci_unlock_rescan_remove();
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_del(&hpdev->list_entry);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	if (hpdev->pci_slot)
		pci_destroy_slot(hpdev->pci_slot);

	memset(&ctxt, 0, sizeof(ctxt));
	ejct_pkt = (struct pci_eject_response *)ctxt.buffer;
	ejct_pkt->message_type.type = PCI_EJECTION_COMPLETE;
	ejct_pkt->wslot.slot = hpdev->desc.win_slot.slot;
	vmbus_sendpacket(hbus->hdev->channel, ejct_pkt,
			 sizeof(*ejct_pkt), 0,
			 VM_PKT_DATA_INBAND, 0);

	/* For the get_pcichild() in hv_pci_eject_device() */
	put_pcichild(hpdev);
	/* For the two refs got in new_pcichild_device() */
	put_pcichild(hpdev);
	put_pcichild(hpdev);
	/* hpdev has been freed. Do not use it any more. */

	mutex_unlock(&hbus->state_lock);
}

/**
 * hv_pci_eject_device() - Handles device ejection
 * @hpdev:	Internal device tracking struct
 *
 * This function is invoked when an ejection packet arrives.  It
 * just schedules work so that we don't re-enter the packet
 * delivery code handling the ejection.
 */
/* [한국어]
 * hv_pci_eject_device - 뽑기 요청을 받아 워크로 미룬다
 *
 * @hpdev: 뽑을 장치.
 * @return: 없음.
 *
 * **상류 주석이 미루는 이유를 밝힌다** -- 패킷 전달 코드 안에서
 * 다시 그 코드로 들어가지 않기 위해서다. 뽑기 처리는 리눅스 PCI 코어를
 * 호출하고 잠들 수 있으므로 채널 콜백 문맥에서 할 수 없다.
 *
 * **참조를 먼저 올린다** -- 워크가 돌기 전에 장치가 사라지면 안 된다.
 * 그 참조를 hv_eject_device_work() 가 놓는다.
 *
 * **버스를 내리는 중이면 무시한다** -- 어차피 곧 모두 사라지므로
 * 따로 뽑을 필요가 없다.
 *
 * **INIT_WORK 을 매번 부른다** -- 워크 구조체가 hv_pci_dev 안에 박혀
 * 있으므로, 같은 장치가 두 번 뽑히지 않는다는 전제에서만 안전하다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 문맥.
 *
 * 호출 체인:
 *   hv_pci_onchannelcallback -> [이 함수] -> get_pcichild(), queue_work()
 */
static void hv_pci_eject_device(struct hv_pci_dev *hpdev)
{
	struct hv_pcibus_device *hbus = hpdev->hbus;
	struct hv_device *hdev = hbus->hdev;

	if (hbus->state == hv_pcibus_removing) {
		dev_info(&hdev->device, "PCI VMBus EJECT: ignored\n");
		return;
	}

	get_pcichild(hpdev);
	INIT_WORK(&hpdev->wrk, hv_eject_device_work);
	queue_work(hbus->wq, &hpdev->wrk);
}

/**
 * hv_pci_onchannelcallback() - Handles incoming packets
 * @context:	Internal bus tracking struct
 *
 * This function is invoked whenever the host sends a packet to
 * this channel (which is private to this root PCI bus).
 */
/* [한국어]
 * hv_pci_onchannelcallback - VMBus 채널에 도착한 호스트 메시지를 처리한다
 *
 * @context: 채널을 열 때 넘겨 둔 이 버스.
 *
 * 이 드라이버의 심장이다. 실제 PCI 하드웨어가 없으므로, 이 파일이 하는 모든
 * 일은 호스트와 주고받는 메시지로 이뤄지고 그 수신 쪽 전부가 이 함수다.
 *
 * 메시지가 두 종류다.
 * - **VM_PKT_COMP** — 우리가 보낸 요청의 응답이다. 요청 ID 로 원래 패킷을
 *   찾아 그 완료 콜백을 부른다. 이것이 이 파일 곳곳의 "보내고 기다리는"
 *   패턴을 완성하는 반쪽이다.
 * - **VM_PKT_DATA_INBAND** — 호스트가 먼저 보내는 알림이다. 장치 목록 변경,
 *   장치 제거 요청, 블록 무효화 세 가지가 여기 온다.
 *
 * 버퍼를 다시 잡는 루프가 이 함수의 특징이다. 받을 메시지 크기를 미리 알 수
 * 없어, 일단 256바이트로 시도하고 -ENOBUFS 가 오면 알려 준 크기로 다시
 * 잡아 재시도한다.
 *
 * GFP_ATOMIC 으로 잡는 이유는 인터럽트 문맥이기 때문이다. 그래서 할당이
 * 실패하면 그냥 돌아가는데, 그 경우 도착한 메시지를 처리하지 못한다 —
 * 그것이 요청 응답이었다면 그쪽에서 기다리던 코드가 시간 초과로 끝난다.
 *
 * 크기 검사가 각 갈래마다 있다. 호스트가 보낸 값을 믿고 배열을 훑기 전에
 * 받은 바이트 수가 그 구조체를 담기에 충분한지 확인하는데, 게스트가 호스트를
 * 신뢰할 수 없다는 전제에서 나온 방어다.
 *
 * 요청자 잠금(lock_requestor)을 완료 콜백을 부르는 **동안** 쥐고 있는 것에
 * 주의할 만하다. 그 사이에 같은 ID 가 재사용되지 않게 한다.
 *
 * 실행 컨텍스트: VMBus 채널 콜백 — 인터럽트 문맥(tasklet)이다. 잠들 수 없다.
 *
 * 에러 경로: 알 수 없는 메시지 종류는 경고만 남긴다. 크기가 모자란 메시지는
 * 기록하고 건너뛴다.
 *
 * 호출 체인:
 *   VMBus 채널 → [이 함수]
 *     → vmbus_recvpacket_raw() → completion_func()
 *   / hv_pci_devices_present() / hv_pci_eject_device()
 *   / block_invalidate 콜백
 */
static void hv_pci_onchannelcallback(void *context)
{
	const int packet_size = 0x100;
	int ret;
	struct hv_pcibus_device *hbus = context;
	struct vmbus_channel *chan = hbus->hdev->channel;
	u32 bytes_recvd;
	u64 req_id, req_addr;
	struct vmpacket_descriptor *desc;
	unsigned char *buffer;
	int bufferlen = packet_size;
	struct pci_packet *comp_packet;
	struct pci_response *response;
	struct pci_incoming_message *new_message;
	struct pci_bus_relations *bus_rel;
	struct pci_bus_relations2 *bus_rel2;
	struct pci_dev_inval_block *inval;
	struct pci_dev_incoming *dev_message;
	struct hv_pci_dev *hpdev;
	unsigned long flags;

	buffer = kmalloc(bufferlen, GFP_ATOMIC);
	if (!buffer)
		return;

	while (1) {
		ret = vmbus_recvpacket_raw(chan, buffer, bufferlen,
					   &bytes_recvd, &req_id);

		if (ret == -ENOBUFS) {
			kfree(buffer);
			/* Handle large packet */
			bufferlen = bytes_recvd;
			buffer = kmalloc(bytes_recvd, GFP_ATOMIC);
			if (!buffer)
				return;
			continue;
		}

		/* Zero length indicates there are no more packets. */
		if (ret || !bytes_recvd)
			break;

		/*
		 * All incoming packets must be at least as large as a
		 * response.
		 */
		if (bytes_recvd <= sizeof(struct pci_response))
			continue;
		desc = (struct vmpacket_descriptor *)buffer;

		switch (desc->type) {
		case VM_PKT_COMP:

			lock_requestor(chan, flags);
			req_addr = __vmbus_request_addr_match(chan, req_id,
							      VMBUS_RQST_ADDR_ANY);
			if (req_addr == VMBUS_RQST_ERROR) {
				unlock_requestor(chan, flags);
				dev_err(&hbus->hdev->device,
					"Invalid transaction ID %llx\n",
					req_id);
				break;
			}
			comp_packet = (struct pci_packet *)req_addr;
			response = (struct pci_response *)buffer;
			/*
			 * Call ->completion_func() within the critical section to make
			 * sure that the packet pointer is still valid during the call:
			 * here 'valid' means that there's a task still waiting for the
			 * completion, and that the packet data is still on the waiting
			 * task's stack.  Cf. hv_compose_msi_msg().
			 */
			comp_packet->completion_func(comp_packet->compl_ctxt,
						     response,
						     bytes_recvd);
			unlock_requestor(chan, flags);
			break;

		case VM_PKT_DATA_INBAND:

			new_message = (struct pci_incoming_message *)buffer;
			switch (new_message->message_type.type) {
			case PCI_BUS_RELATIONS:

				bus_rel = (struct pci_bus_relations *)buffer;
				if (bytes_recvd < sizeof(*bus_rel) ||
				    bytes_recvd <
					struct_size(bus_rel, func,
						    bus_rel->device_count)) {
					dev_err(&hbus->hdev->device,
						"bus relations too small\n");
					break;
				}

				hv_pci_devices_present(hbus, bus_rel);
				break;

			case PCI_BUS_RELATIONS2:

				bus_rel2 = (struct pci_bus_relations2 *)buffer;
				if (bytes_recvd < sizeof(*bus_rel2) ||
				    bytes_recvd <
					struct_size(bus_rel2, func,
						    bus_rel2->device_count)) {
					dev_err(&hbus->hdev->device,
						"bus relations v2 too small\n");
					break;
				}

				hv_pci_devices_present2(hbus, bus_rel2);
				break;

			case PCI_EJECT:

				dev_message = (struct pci_dev_incoming *)buffer;
				if (bytes_recvd < sizeof(*dev_message)) {
					dev_err(&hbus->hdev->device,
						"eject message too small\n");
					break;
				}
				hpdev = get_pcichild_wslot(hbus,
						      dev_message->wslot.slot);
				if (hpdev) {
					hv_pci_eject_device(hpdev);
					put_pcichild(hpdev);
				}
				break;

			case PCI_INVALIDATE_BLOCK:

				inval = (struct pci_dev_inval_block *)buffer;
				if (bytes_recvd < sizeof(*inval)) {
					dev_err(&hbus->hdev->device,
						"invalidate message too small\n");
					break;
				}
				hpdev = get_pcichild_wslot(hbus,
							   inval->wslot.slot);
				if (hpdev) {
					if (hpdev->block_invalidate) {
						hpdev->block_invalidate(
						    hpdev->invalidate_context,
						    inval->block_mask);
					}
					put_pcichild(hpdev);
				}
				break;

			default:
				dev_warn(&hbus->hdev->device,
					"Unimplemented protocol message %x\n",
					new_message->message_type.type);
				break;
			}
			break;

		default:
			dev_err(&hbus->hdev->device,
				"unhandled packet type %d, tid %llx len %d\n",
				desc->type, req_id, bytes_recvd);
			break;
		}
	}

	kfree(buffer);
}

/**
 * hv_pci_protocol_negotiation() - Set up protocol
 * @hdev:		VMBus's tracking struct for this root PCI bus.
 * @version:		Array of supported channel protocol versions in
 *			the order of probing - highest go first.
 * @num_version:	Number of elements in the version array.
 *
 * This driver is intended to support running on Windows 10
 * (server) and later versions. It will not run on earlier
 * versions, as they assume that many of the operations which
 * Linux needs accomplished with a spinlock held were done via
 * asynchronous messaging via VMBus.  Windows 10 increases the
 * surface area of PCI emulation so that these actions can take
 * place by suspending a virtual processor for their duration.
 *
 * This function negotiates the channel protocol version,
 * failing if the host doesn't support the necessary protocol
 * level.
 */
/* [한국어]
 * hv_pci_protocol_negotiation - 호스트와 쓸 프로토콜 판본을 합의한다
 *
 * @hdev: 이 VMBus 장치.
 * @version: 시도할 판본 목록. 새 것부터 옛 것 순으로 늘어서 있다.
 * @num_version: 그 개수.
 * @return: 0 = 합의 성공, -ENOMEM / -EPROTO 또는 전송 오류.
 *
 * 호스트와 게스트의 Hyper-V 판본이 다를 수 있어, 무엇을 주고받을지 먼저
 * 정해야 한다. 이 합의 결과가 이후 모든 메시지의 형식을 정한다 —
 * 예를 들어 hv_send_resources_allocated() 가 어느 구조체를 쓸지가 여기서
 * 정해진 protocol_version 에 달렸다.
 *
 * 목록을 앞에서부터 시도하는 것이 협상 방식이다. 호스트가 그 판본을 모르면
 * STATUS_REVISION_MISMATCH 로 답하고, 그때만 다음 판본으로 넘어간다.
 * 그 밖의 오류는 협상 자체가 깨진 것이라 즉시 물러난다.
 *
 * completion 객체를 재사용하므로 실패한 시도마다 reinit_completion() 이
 * 필요하다. 그것을 빠뜨리면 두 번째 시도가 첫 번째의 완료 신호를 보고
 * 곧바로 돌아온다.
 *
 * 목록을 다 써도 합의하지 못하면 -EPROTO 다. 게스트가 아는 판본을 호스트가
 * 하나도 지원하지 않는 경우이며, 이 장치를 쓸 수 없다.
 *
 * 패킷 하나를 재사용하며 반복하는 것도 눈에 띈다 — 판본 필드만 바꿔 다시
 * 보내므로 매번 할당할 이유가 없다.
 *
 * 실행 컨텍스트: probe 와 resume. 완료 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, 협상 실패는 -EPROTO, 전송 실패는 그 오류.
 * 어느 경로든 패킷을 해제한다.
 *
 * 호출 체인:
 *   hv_pci_probe() / hv_pci_resume() → [이 함수]
 *     → vmbus_sendpacket() → wait_for_response()
 */
static int hv_pci_protocol_negotiation(struct hv_device *hdev,
				       enum pci_protocol_version_t version[],
				       int num_version)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_version_request *version_req;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	int ret;
	int i;

	/*
	 * Initiate the handshake with the host and negotiate
	 * a version that the host can support. We start with the
	 * highest version number and go down if the host cannot
	 * support it.
	 */
	pkt = kzalloc(sizeof(*pkt) + sizeof(*version_req), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	version_req = (struct pci_version_request *)(pkt + 1);
	version_req->message_type.type = PCI_QUERY_PROTOCOL_VERSION;

	for (i = 0; i < num_version; i++) {
		version_req->protocol_version = version[i];
		ret = vmbus_sendpacket(hdev->channel, version_req,
				sizeof(struct pci_version_request),
				(unsigned long)pkt, VM_PKT_DATA_INBAND,
				VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
		if (!ret)
			ret = wait_for_response(hdev, &comp_pkt.host_event);

		if (ret) {
			dev_err(&hdev->device,
				"PCI Pass-through VSP failed to request version: %d",
				ret);
			goto exit;
		}

		if (comp_pkt.completion_status >= 0) {
			hbus->protocol_version = version[i];
			dev_info(&hdev->device,
				"PCI VMBus probing: Using version %#x\n",
				hbus->protocol_version);
			goto exit;
		}

		if (comp_pkt.completion_status != STATUS_REVISION_MISMATCH) {
			dev_err(&hdev->device,
				"PCI Pass-through VSP failed version request: %#x",
				comp_pkt.completion_status);
			ret = -EPROTO;
			goto exit;
		}

		reinit_completion(&comp_pkt.host_event);
	}

	dev_err(&hdev->device,
		"PCI pass-through VSP failed to find supported version");
	ret = -EPROTO;

exit:
	kfree(pkt);
	return ret;
}

/**
 * hv_pci_free_bridge_windows() - Release memory regions for the
 * bus
 * @hbus:	Root PCI bus, as understood by this driver
 */
/* [한국어]
 * hv_pci_free_bridge_windows - 호스트에서 얻은 MMIO 창을 돌려준다
 *
 * @hbus: 이 가상 버스.
 *
 * hv_pci_allocate_bridge_windows() 의 짝이다.
 *
 * **IORESOURCE_BUSY 를 다시 세우는 것** 이 이 함수에서 눈여겨볼 부분이다.
 * 할당 쪽이 그 비트를 지워 자식 장치들이 그 범위 안에서 자리를 얻을 수
 * 있게 해 두었는데, 돌려주기 전에 다시 세워 그 사이에 아무도 새로 가져가지
 * 못하게 한다.
 *
 * 두 창(4GB 아래·위)을 각각 크기와 자원 포인터가 모두 있을 때만 해제한다.
 * 크기가 0 이면 애초에 할당하지 않았다는 뜻이다.
 *
 * 실행 컨텍스트: probe 의 되감기와 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   hv_pci_probe() 의 되감기 / hv_pci_remove() → [이 함수]
 *     → vmbus_free_mmio()
 */
static void hv_pci_free_bridge_windows(struct hv_pcibus_device *hbus)
{
	/*
	 * Set the resources back to the way they looked when they
	 * were allocated by setting IORESOURCE_BUSY again.
	 */

	if (hbus->low_mmio_space && hbus->low_mmio_res) {
		hbus->low_mmio_res->flags |= IORESOURCE_BUSY;
		vmbus_free_mmio(hbus->low_mmio_res->start,
				resource_size(hbus->low_mmio_res));
	}

	if (hbus->high_mmio_space && hbus->high_mmio_res) {
		hbus->high_mmio_res->flags |= IORESOURCE_BUSY;
		vmbus_free_mmio(hbus->high_mmio_res->start,
				resource_size(hbus->high_mmio_res));
	}
}

/**
 * hv_pci_allocate_bridge_windows() - Allocate memory regions
 * for the bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * This function calls vmbus_allocate_mmio(), which is itself a
 * bit of a compromise.  Ideally, we might change the pnp layer
 * in the kernel such that it comprehends either PCI devices
 * which are "grandchildren of ACPI," with some intermediate bus
 * node (in this case, VMBus) or change it such that it
 * understands VMBus.  The pnp layer, however, has been declared
 * deprecated, and not subject to change.
 *
 * The workaround, implemented here, is to ask VMBus to allocate
 * MMIO space for this bus.  VMBus itself knows which ranges are
 * appropriate by looking at its own ACPI objects.  Then, after
 * these ranges are claimed, they're modified to look like they
 * would have looked if the ACPI and pnp code had allocated
 * bridge windows.  These descriptors have to exist in this form
 * in order to satisfy the code which will get invoked when the
 * endpoint PCI function driver calls request_mem_region() or
 * request_mem_region_exclusive().
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_pci_allocate_bridge_windows - 자식 장치들이 쓸 MMIO 창을 호스트에서 얻는다
 *
 * @hbus: 이 가상 버스.
 * @return: 0 = 성공, 음수 오류.
 *
 * 실제 하드웨어가 없으므로 주소 공간도 호스트에게 요청해서 받는다.
 * vmbus_allocate_mmio() 가 그 통로다.
 *
 * 창이 둘로 나뉜 이유는 32비트 BAR 때문이다. 4GB 아래 창은 32비트 BAR 만
 * 쓸 수 있는 장치를 위한 것이고, 위 창은 64비트 BAR 용이다. 각각 필요한
 * 크기가 미리 계산되어 있어(survey 단계), 그 크기가 0 이면 건너뛴다.
 *
 * 정렬 계산이 눈에 띈다 — 필요한 크기 이하의 가장 큰 2의 거듭제곱을
 * 정렬로 삼는데, `__builtin_clzll` 로 최상위 비트 위치를 구해 만든다.
 * PCI 창은 자기 크기에 정렬되어야 하기 때문이다.
 *
 * **IORESOURCE_BUSY 를 지우는 것** 이 이 함수의 핵심이다. 자원을 얻으면
 * 기본적으로 "사용 중" 으로 표시되는데, 그대로 두면 자식 장치의 BAR 이
 * 그 안에서 자리를 얻지 못한다. WINDOW 를 세우고 BUSY 를 지워야 PCI 코어가
 * 그것을 "나눠 줄 수 있는 창" 으로 본다.
 *
 * 되감기가 한 갈래다 — 위 창 할당이 실패하면 아래 창을 돌려준다.
 * 다만 그때 pci_add_resource() 로 목록에 넣은 것은 빼지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패는 그 오류를 올려보내며, 어느 창이 모자랐는지와
 * "VM 설정을 바꿔 보라" 는 안내를 기록에 남긴다.
 *
 * 호출 체인:
 *   hv_pci_probe() → [이 함수]
 *     → vmbus_allocate_mmio() → pci_add_resource()
 */
static int hv_pci_allocate_bridge_windows(struct hv_pcibus_device *hbus)
{
	resource_size_t align;
	int ret;

	if (hbus->low_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->low_mmio_res, hbus->hdev, 0,
					  (u64)(u32)0xffffffff,
					  hbus->low_mmio_space,
					  align, false);
		if (ret) {
			dev_err(&hbus->hdev->device,
				"Need %#llx of low MMIO space. Consider reconfiguring the VM.\n",
				hbus->low_mmio_space);
			return ret;
		}

		/* Modify this resource to become a bridge window. */
		hbus->low_mmio_res->flags |= IORESOURCE_WINDOW;
		hbus->low_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->bridge->windows, hbus->low_mmio_res);
	}

	if (hbus->high_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->high_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->high_mmio_res, hbus->hdev,
					  0x100000000, -1,
					  hbus->high_mmio_space, align,
					  false);
		if (ret) {
			dev_err(&hbus->hdev->device,
				"Need %#llx of high MMIO space. Consider reconfiguring the VM.\n",
				hbus->high_mmio_space);
			goto release_low_mmio;
		}

		/* Modify this resource to become a bridge window. */
		hbus->high_mmio_res->flags |= IORESOURCE_WINDOW;
		hbus->high_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->bridge->windows, hbus->high_mmio_res);
	}

	return 0;

release_low_mmio:
	if (hbus->low_mmio_res) {
		vmbus_free_mmio(hbus->low_mmio_res->start,
				resource_size(hbus->low_mmio_res));
	}

	return ret;
}

/**
 * hv_allocate_config_window() - Find MMIO space for PCI Config
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * This function claims memory-mapped I/O space for accessing
 * configuration space for the functions on this bus.
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_allocate_config_window - config 공간 접근에 쓸 MMIO 창을 얻는다
 *
 * @hbus: 이 가상 버스.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 창은 자식 장치에게 나눠 주는 것이 아니라 **게스트가 config 공간을
 * 읽고 쓰는 데** 쓴다. 호스트가 그 창의 주소를 보고 어느 장치의 config
 * 접근인지 판단한다.
 *
 * 그래서 IORESOURCE_BUSY 를 **세운다.** 위 브리지 창들이 그 비트를 지우는
 * 것과 정반대인데, 이 창은 누구에게도 나눠 주면 안 되기 때문이다.
 *
 * 주소 범위를 제한하지 않는다(0 부터 -1). 게스트만 쓰는 창이라 32비트
 * 장치의 제약과 무관하기 때문이다.
 *
 * 이 창의 시작 주소가 hv_pci_enter_d0() 을 통해 호스트에게 전달되어,
 * 그때부터 호스트가 그 범위의 접근을 config 요청으로 해석한다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   hv_pci_probe() → [이 함수] → vmbus_allocate_mmio()
 */
static int hv_allocate_config_window(struct hv_pcibus_device *hbus)
{
	int ret;

	/*
	 * Set up a region of MMIO space to use for accessing configuration
	 * space.
	 */
	ret = vmbus_allocate_mmio(&hbus->mem_config, hbus->hdev, 0, -1,
				  PCI_CONFIG_MMIO_LENGTH, 0x1000, false);
	if (ret)
		return ret;

	/*
	 * vmbus_allocate_mmio() gets used for allocating both device endpoint
	 * resource claims (those which cannot be overlapped) and the ranges
	 * which are valid for the children of this bus, which are intended
	 * to be overlapped by those children.  Set the flag on this claim
	 * meaning that this region can't be overlapped.
	 */

	hbus->mem_config->flags |= IORESOURCE_BUSY;

	return 0;
}

/* [한국어]
 * hv_free_config_window - config 창을 호스트에 돌려준다
 *
 * @hbus: 이 가상 버스.
 *
 * hv_allocate_config_window() 의 짝이며 한 줄이다.
 *
 * 브리지 창 해제와 달리 BUSY 비트를 손대지 않는다. 할당 쪽이 세워 둔 채로
 * 두었고 그 사이에 아무도 가져가지 않았으므로, 그대로 돌려주면 된다.
 *
 * 크기를 자원에서 읽지 않고 상수를 다시 쓰는 것에 주의할 만하다 —
 * 할당 때와 같은 값이라 결과는 같다.
 *
 * 실행 컨텍스트: probe 의 되감기와 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   hv_pci_probe() 의 되감기 / hv_pci_remove() → [이 함수]
 *     → vmbus_free_mmio()
 */
static void hv_free_config_window(struct hv_pcibus_device *hbus)
{
	vmbus_free_mmio(hbus->mem_config->start, PCI_CONFIG_MMIO_LENGTH);
}

static int hv_pci_bus_exit(struct hv_device *hdev, bool keep_devs);

/**
 * hv_pci_enter_d0() - Bring the "bus" into the D0 power state
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_pci_enter_d0 - 가상 버스를 D0(동작) 상태로 올린다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, -ENOMEM / -EPROTO 또는 전송 오류.
 *
 * 호스트에게 "이제 이 버스를 쓰겠다" 고 알리는 단계다. 이 메시지에 config
 * 창의 시작 주소를 함께 보내, 호스트가 그 범위의 접근을 config 요청으로
 * 해석하기 시작한다.
 *
 * **한 번 재시도하는 구조** 가 이 함수의 특징이다. 첫 시도가 실패하면
 * 호스트가 아직 이전 세션의 상태를 들고 있다고 보고, hv_pci_bus_exit() 으로
 * 그것을 정리한 뒤 처음부터 다시 시도한다.
 *
 * 그 정리 전에 wslot_res_allocated 를 255 로 두는 것이 요점이다.
 * hv_send_resources_released() 가 그 값부터 거꾸로 내려가며 해제 메시지를
 * 보내므로, 실제로 무엇이 배정됐는지 모르는 상황에서는 최대값을 넣어
 * 전부 훑게 하는 것이다.
 *
 * 재시도는 한 번뿐이다(retry 플래그). 그마저 실패하면 -EPROTO 다.
 *
 * 재시도 경로에서 패킷을 해제하고 라벨로 돌아가 다시 할당하는 것에 주의할
 * 만하다 — 같은 패킷을 재사용하지 않고 새로 잡는다.
 *
 * 실행 컨텍스트: probe 와 resume. 완료 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, 두 번째 시도까지 실패하면 -EPROTO.
 * 어느 경로든 패킷을 해제한다.
 *
 * 호출 체인:
 *   hv_pci_probe() / hv_pci_resume() → [이 함수]
 *     → vmbus_sendpacket() → wait_for_response() → hv_pci_bus_exit()
 */
static int hv_pci_enter_d0(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_bus_d0_entry *d0_entry;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	bool retry = true;
	int ret;

enter_d0_retry:
	/*
	 * Tell the host that the bus is ready to use, and moved into the
	 * powered-on state.  This includes telling the host which region
	 * of memory-mapped I/O space has been chosen for configuration space
	 * access.
	 */
	pkt = kzalloc(sizeof(*pkt) + sizeof(*d0_entry), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	d0_entry = (struct pci_bus_d0_entry *)(pkt + 1);
	d0_entry->message_type.type = PCI_BUS_D0ENTRY;
	d0_entry->mmio_base = hbus->mem_config->start;

	ret = vmbus_sendpacket(hdev->channel, d0_entry, sizeof(*d0_entry),
			       (unsigned long)pkt, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (!ret)
		ret = wait_for_response(hdev, &comp_pkt.host_event);

	if (ret)
		goto exit;

	/*
	 * In certain case (Kdump) the pci device of interest was
	 * not cleanly shut down and resource is still held on host
	 * side, the host could return invalid device status.
	 * We need to explicitly request host to release the resource
	 * and try to enter D0 again.
	 */
	if (comp_pkt.completion_status < 0 && retry) {
		retry = false;

		dev_err(&hdev->device, "Retrying D0 Entry\n");

		/*
		 * Hv_pci_bus_exit() calls hv_send_resource_released()
		 * to free up resources of its child devices.
		 * In the kdump kernel we need to set the
		 * wslot_res_allocated to 255 so it scans all child
		 * devices to release resources allocated in the
		 * normal kernel before panic happened.
		 */
		hbus->wslot_res_allocated = 255;

		ret = hv_pci_bus_exit(hdev, true);

		if (ret == 0) {
			kfree(pkt);
			goto enter_d0_retry;
		}
		dev_err(&hdev->device,
			"Retrying D0 failed with ret %d\n", ret);
	}

	if (comp_pkt.completion_status < 0) {
		dev_err(&hdev->device,
			"PCI Pass-through VSP failed D0 Entry with status %x\n",
			comp_pkt.completion_status);
		ret = -EPROTO;
		goto exit;
	}

	ret = 0;

exit:
	kfree(pkt);
	return ret;
}

/**
 * hv_pci_query_relations() - Ask host to send list of child
 * devices
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_pci_query_relations - 호스트에게 장치 목록을 물어보고 처리가 끝나기를 기다린다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, -ENOTEMPTY 또는 전송 오류.
 *
 * 이 가상 버스에 어떤 장치가 붙어 있는지는 호스트만 안다. 그것을 묻는 것이
 * 이 함수다.
 *
 * 기다림이 **두 겹** 인 것이 요점이다.
 * 1. 완료 객체 — 호스트가 응답을 보낼 때까지 기다린다. 다만 그 응답은
 *    VM_PKT_COMP 가 아니라 PCI_BUS_RELATIONS 알림으로 오고, 그 처리 함수가
 *    이 완료를 깨운다.
 * 2. 워크큐 flush — 알림 처리가 장치 목록 갱신을 워크로 미루므로,
 *    그것까지 끝나야 목록이 실제로 반영된다.
 *
 * cmpxchg 로 완료 객체를 등록하는 것이 동시 질의를 막는다. 이미 다른 질의가
 * 진행 중이면 -ENOTEMPTY 로 거절하는데, 두 질의가 겹치면 어느 응답이 어느
 * 질의의 것인지 알 수 없기 때문이다.
 *
 * **성공하든 실패하든 워크큐를 flush 한다.** 응답을 기다리다 실패했더라도
 * 그 사이에 알림이 도착해 워크가 걸렸을 수 있다.
 *
 * 실행 컨텍스트: probe 와 resume. 완료 대기와 flush 가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 질의가 겹치면 -ENOTEMPTY, 그 밖은 전송·대기의 오류.
 *
 * 호출 체인:
 *   hv_pci_probe() / hv_pci_resume() → [이 함수]
 *     → vmbus_sendpacket() → wait_for_response() → flush_workqueue()
 */
static int hv_pci_query_relations(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_message message;
	struct completion comp;
	int ret;

	/* Ask the host to send along the list of child devices */
	init_completion(&comp);
	if (cmpxchg(&hbus->survey_event, NULL, &comp))
		return -ENOTEMPTY;

	memset(&message, 0, sizeof(message));
	message.type = PCI_QUERY_BUS_RELATIONS;

	ret = vmbus_sendpacket(hdev->channel, &message, sizeof(message),
			       0, VM_PKT_DATA_INBAND, 0);
	if (!ret)
		ret = wait_for_response(hdev, &comp);

	/*
	 * In the case of fast device addition/removal, it's possible that
	 * vmbus_sendpacket() or wait_for_response() returns -ENODEV but we
	 * already got a PCI_BUS_RELATIONS* message from the host and the
	 * channel callback already scheduled a work to hbus->wq, which can be
	 * running pci_devices_present_work() -> survey_child_resources() ->
	 * complete(&hbus->survey_event), even after hv_pci_query_relations()
	 * exits and the stack variable 'comp' is no longer valid; as a result,
	 * a hang or a page fault may happen when the complete() calls
	 * raw_spin_lock_irqsave(). Flush hbus->wq before we exit from
	 * hv_pci_query_relations() to avoid the issues. Note: if 'ret' is
	 * -ENODEV, there can't be any more work item scheduled to hbus->wq
	 * after the flush_workqueue(): see vmbus_onoffer_rescind() ->
	 * vmbus_reset_channel_cb(), vmbus_rescind_cleanup() ->
	 * channel->rescind = true.
	 */
	flush_workqueue(hbus->wq);

	return ret;
}

/**
 * hv_send_resources_allocated() - Report local resource choices
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * The host OS is expecting to be sent a request as a message
 * which contains all the resources that the device will use.
 * The response contains those same resources, "translated"
 * which is to say, the values which should be used by the
 * hardware, when it delivers an interrupt.  (MMIO resources are
 * used in local terms.)  This is nice for Windows, and lines up
 * with the FDO/PDO split, which doesn't exist in Linux.  Linux
 * is deeply expecting to scan an emulated PCI configuration
 * space.  So this message is sent here only to drive the state
 * machine on the host forward.
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_send_resources_allocated - 배정된 자원을 슬롯마다 호스트에 알린다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, -ENOMEM / -EPROTO 또는 전송 오류.
 *
 * 커널이 BAR 주소를 정한 뒤, 그 결과를 호스트에게 알려야 호스트가 그
 * 주소로 오는 접근을 해당 장치로 보낸다.
 *
 * 메시지 형식이 프로토콜 판본으로 갈린다. 1.2 미만은 옛 구조체를,
 * 그 이상은 확장된 구조체를 쓰며, 크기 계산도 그에 맞춘다. 이것이
 * hv_pci_protocol_negotiation() 의 결과가 실제로 쓰이는 자리다.
 *
 * 슬롯 0~255 를 모두 훑되 없는 슬롯은 건너뛴다. 슬롯 번호가 곧 devfn 이라
 * 256개가 최대다.
 *
 * **wslot_res_allocated 를 갱신하는 것** 이 이 함수의 부수 효과이자 중요한
 * 계약이다. 어디까지 알렸는지를 기록해 두어야, 실패했을 때
 * hv_send_resources_released() 가 그 지점부터 거꾸로 되감을 수 있다.
 *
 * 패킷 하나를 재사용하며 매번 memset 으로 지운다. 슬롯마다 할당하면 256번
 * 할당하게 되므로 그편이 낫다.
 *
 * put_pcichild() 를 전송 **전** 에 부르는 것에 주의할 만하다. 필요한 값을
 * 이미 패킷에 복사해 두었으므로 더 붙잡고 있을 이유가 없다.
 *
 * 실행 컨텍스트: probe 와 resume. 완료 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, 호스트가 거절하면 -EPROTO. 어느 경우든
 * 그때까지의 wslot_res_allocated 가 남아 되감기의 기준이 된다.
 *
 * 호출 체인:
 *   hv_pci_probe() / hv_pci_resume() → [이 함수]
 *     → get_pcichild_wslot() → vmbus_sendpacket() → wait_for_response()
 */
static int hv_send_resources_allocated(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_resources_assigned *res_assigned;
	struct pci_resources_assigned2 *res_assigned2;
	struct hv_pci_compl comp_pkt;
	struct hv_pci_dev *hpdev;
	struct pci_packet *pkt;
	size_t size_res;
	int wslot;
	int ret;

	size_res = (hbus->protocol_version < PCI_PROTOCOL_VERSION_1_2)
			? sizeof(*res_assigned) : sizeof(*res_assigned2);

	pkt = kmalloc(sizeof(*pkt) + size_res, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	ret = 0;

	for (wslot = 0; wslot < 256; wslot++) {
		hpdev = get_pcichild_wslot(hbus, wslot);
		if (!hpdev)
			continue;

		memset(pkt, 0, sizeof(*pkt) + size_res);
		init_completion(&comp_pkt.host_event);
		pkt->completion_func = hv_pci_generic_compl;
		pkt->compl_ctxt = &comp_pkt;

		if (hbus->protocol_version < PCI_PROTOCOL_VERSION_1_2) {
			res_assigned =
				(struct pci_resources_assigned *)(pkt + 1);
			res_assigned->message_type.type =
				PCI_RESOURCES_ASSIGNED;
			res_assigned->wslot.slot = hpdev->desc.win_slot.slot;
		} else {
			res_assigned2 =
				(struct pci_resources_assigned2 *)(pkt + 1);
			res_assigned2->message_type.type =
				PCI_RESOURCES_ASSIGNED2;
			res_assigned2->wslot.slot = hpdev->desc.win_slot.slot;
		}
		put_pcichild(hpdev);

		ret = vmbus_sendpacket(hdev->channel, pkt + 1,
				size_res, (unsigned long)pkt,
				VM_PKT_DATA_INBAND,
				VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
		if (!ret)
			ret = wait_for_response(hdev, &comp_pkt.host_event);
		if (ret)
			break;

		if (comp_pkt.completion_status < 0) {
			ret = -EPROTO;
			dev_err(&hdev->device,
				"resource allocated returned 0x%x",
				comp_pkt.completion_status);
			break;
		}

		hbus->wslot_res_allocated = wslot;
	}

	kfree(pkt);
	return ret;
}

/**
 * hv_send_resources_released() - Report local resources
 * released
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_send_resources_released - 배정했던 자원을 슬롯마다 호스트에 반납한다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, 전송 오류.
 *
 * hv_send_resources_allocated() 의 짝이며 **역순으로** 훑는다.
 * wslot_res_allocated 가 "어디까지 알렸는가" 를 기억하고 있어, 그 지점부터
 * 0 까지 내려간다.
 *
 * 역순인 이유는 부분 실패를 정확히 되감기 위해서다. 배정이 슬롯 3 에서
 * 실패했다면 0~2 만 알린 상태이고, 그 셋만 반납해야 한다.
 *
 * 한 슬롯을 반납할 때마다 wslot_res_allocated 를 하나 줄인다. 그래서
 * 이 함수가 중간에 실패해도 다음 호출이 남은 것부터 이어서 반납한다.
 *
 * 끝나면 -1 로 둔다. "반납할 것이 없다" 는 표시이며, 다음 배정이 0 부터
 * 다시 세기 시작한다.
 *
 * 완료를 기다리지 않는 것이 배정 쪽과 다르다. 반납은 호스트의 확인이
 * 필요 없는 단방향 알림이다.
 *
 * 실행 컨텍스트: bus_exit 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 전송이 실패하면 즉시 그 오류를 올려보내며, 그때까지 줄어든
 * wslot_res_allocated 가 남는다.
 *
 * 호출 체인:
 *   hv_pci_bus_exit() → [이 함수]
 *     → get_pcichild_wslot() → vmbus_sendpacket()
 */
static int hv_send_resources_released(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_child_message pkt;
	struct hv_pci_dev *hpdev;
	int wslot;
	int ret;

	for (wslot = hbus->wslot_res_allocated; wslot >= 0; wslot--) {
		hpdev = get_pcichild_wslot(hbus, wslot);
		if (!hpdev)
			continue;

		memset(&pkt, 0, sizeof(pkt));
		pkt.message_type.type = PCI_RESOURCES_RELEASED;
		pkt.wslot.slot = hpdev->desc.win_slot.slot;

		put_pcichild(hpdev);

		ret = vmbus_sendpacket(hdev->channel, &pkt, sizeof(pkt), 0,
				       VM_PKT_DATA_INBAND, 0);
		if (ret)
			return ret;

		hbus->wslot_res_allocated = wslot - 1;
	}

	hbus->wslot_res_allocated = -1;

	return 0;
}

/**
 * hv_pci_probe() - New VMBus channel probe, for a root PCI bus
 * @hdev:	VMBus's tracking struct for this root PCI bus
 * @dev_id:	Identifies the device itself
 *
 * Return: 0 on success, -errno on failure
 */
/* [한국어]
 * hv_pci_probe - VMBus 장치를 가상 PCI 호스트 브리지로 세운다
 *
 * @hdev: 이 VMBus 장치.
 * @dev_id: 매칭된 장치 ID. 쓰지 않는다.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, 이 파일에서 가장 긴 함수다. 열두 단계가
 * 순서대로 이어지고 되감기 라벨이 아홉 개다.
 *
 * 도메인 번호를 VMBus 인스턴스 GUID 에서 뽑아내는 것이 첫 단계다. 그 값을
 * 힌트로 쓰되 충돌하면 다른 번호를 받는데, 게스트 안에서 이 브리지를
 * 가리키는 이름이 안정적이기를 바라기 때문이다. 배정받은 번호는 브리지가
 * 해제될 때 PCI 코어가 돌려준다(probe.c 의 pci_bus_release_emul_domain_nr).
 *
 * 채널을 여는 것이 통신의 시작이다. 그 전에 요청 ID 콜백 셋을 채워 두는데,
 * 이 드라이버가 여러 요청을 동시에 띄우고 응답을 ID 로 짝지어야 하기 때문이다.
 *
 * drvdata 를 채널을 연 **뒤** 에 매다는 순서가 미묘하다. 채널 콜백이
 * 그것을 읽으므로 그 전에 매달아야 할 것 같지만, 콜백에 hbus 를 인자로
 * 직접 넘기고 있어 문제가 되지 않는다.
 *
 * 이후 순서가 프로토콜의 순서다 — 판본 협상, config 창 확보, 인터럽트
 * 도메인 생성, 장치 목록 질의, D0 진입, 브리지 창 확보, 자원 배정 통보,
 * BAR 미리 채우기, 루트 버스 생성.
 *
 * state_lock 을 D0 진입부터 버스 생성까지 쥐는 것이 요점이다. 그 사이에
 * 호스트가 장치 목록 변경을 보내면 반쯤 세워진 버스를 건드리게 된다.
 *
 * 되감기 라벨이 계단으로 늘어서 있어, 각 진입점이 그 지점까지 성공한 것만
 * 정확히 되돌린다.
 *
 * 실행 컨텍스트: VMBus 드라이버 probe. 완료 대기가 여럿 있어 프로세스
 * 컨텍스트여야 하며 오래 걸린다.
 *
 * 에러 경로: 각 단계의 실패가 goto 로 해당 라벨에 들어가 역순으로 되감는다.
 *
 * 호출 체인:
 *   VMBus 드라이버 코어 → [이 함수]
 *     → vmbus_open() → hv_pci_protocol_negotiation()
 *     → hv_allocate_config_window() → hv_pcie_init_irq_domain()
 *     → hv_pci_query_relations() → hv_pci_enter_d0()
 *     → hv_pci_allocate_bridge_windows()
 *     → hv_send_resources_allocated() → create_root_hv_pci_bus()
 */
static int hv_pci_probe(struct hv_device *hdev,
			const struct hv_vmbus_device_id *dev_id)
{
	struct pci_host_bridge *bridge;
	struct hv_pcibus_device *hbus;
	int ret, dom;
	u16 dom_req;
	char *name;

	bridge = devm_pci_alloc_host_bridge(&hdev->device, 0);
	if (!bridge)
		return -ENOMEM;

	hbus = kzalloc_obj(*hbus);
	if (!hbus)
		return -ENOMEM;

	hbus->bridge = bridge;
	mutex_init(&hbus->state_lock);
	hbus->state = hv_pcibus_init;
	hbus->wslot_res_allocated = -1;

	/*
	 * The PCI bus "domain" is what is called "segment" in ACPI and other
	 * specs. Pull it from the instance ID, to get something usually
	 * unique. In rare cases of collision, we will find out another number
	 * not in use.
	 *
	 * Note that, since this code only runs in a Hyper-V VM, Hyper-V
	 * together with this guest driver can guarantee that (1) The only
	 * domain used by Gen1 VMs for something that looks like a physical
	 * PCI bus (which is actually emulated by the hypervisor) is domain 0.
	 * (2) There will be no overlap between domains (after fixing possible
	 * collisions) in the same VM.
	 *
	 * Because Gen1 VMs use domain 0, don't allow picking domain 0 here,
	 * even if bytes 4 and 5 of the instance GUID are both zero. For wider
	 * userspace compatibility, limit the domain ID to a 16-bit value.
	 */
	dom_req = hdev->dev_instance.b[5] << 8 | hdev->dev_instance.b[4];
	dom = pci_bus_find_emul_domain_nr(dom_req, 1, U16_MAX);
	/* [한국어] 도메인 번호를 얻지 못하면 — 힌트도 대안도 쓸 수 없다는 뜻이다. */
	if (dom < 0) {
		/* [한국어] 어느 번호를 시도했는지 남기고, */
		dev_err(&hdev->device,
			"Unable to use dom# 0x%x or other numbers", dom_req);
		ret = -EINVAL;
		goto free_bus;
	}

	if (dom != dom_req)
		/* [한국어] 힌트와 다른 번호를 받았으면 그 사실을 알린다 — 게스트 안에서
		 * 이 브리지를 가리키는 이름이 예상과 달라지므로 사용자가 알아야 한다. */
		dev_info(&hdev->device,
			 "PCI dom# 0x%x has collision, using 0x%x",
			 dom_req, dom);

	hbus->bridge->domain_nr = dom;
/* [한국어] x86 에서는 sysdata 에 도메인 번호를 따로 담는다. */
#ifdef CONFIG_X86
	hbus->sysdata.domain = dom;
	/* [한국어] 하이퍼바이저가 MMIO 하이퍼콜을 지원한다고 알리면 그것을 쓴다 —
	 * config 접근을 창 매핑 대신 하이퍼콜로 처리한다. */
	hbus->use_calls = !!(ms_hyperv.hints & HV_X64_USE_MMIO_HYPERCALLS);
/* [한국어] ARM64 에서는 — */
#elif defined(CONFIG_ARM64)
	/*
	 * Set the PCI bus parent to be the corresponding VMbus
	 * device. Then the VMbus device will be assigned as the
	 * ACPI companion in pcibios_root_bridge_prepare() and
	 * pci_dma_configure() will propagate device coherence
	 * information to devices created on the bus.
	 */
	hbus->sysdata.parent = hdev->device.parent;
	hbus->use_calls = false;
/* [한국어] 아키텍처 분기 끝. ARM64 는 하이퍼콜 경로를 쓰지 않는다. */
#endif

	hbus->hdev = hdev;
	/* [한국어] 자식 장치 목록과, */
	INIT_LIST_HEAD(&hbus->children);
	INIT_LIST_HEAD(&hbus->dr_list);
	spin_lock_init(&hbus->config_lock);
	spin_lock_init(&hbus->device_list_lock);
	hbus->wq = alloc_ordered_workqueue("hv_pci_%x", 0,
					   /* [한국어] 워크큐 이름에 도메인 번호를 넣어 여러 브리지를 구분한다.
					    * **ordered 판** 인 것이 중요한데, 장치 목록 갱신이 순서대로 처리돼야 하기 때문이다. */
					   hbus->bridge->domain_nr);
	if (!hbus->wq) {
		/* [한국어] 워크큐를 만들지 못하면 목록 갱신을 처리할 수 없다. */
		ret = -ENOMEM;
		goto free_bus;
	}

	hdev->channel->next_request_id_callback = vmbus_next_request_id;
	/* [한국어] 응답에서 원래 요청을 찾는 콜백. */
	hdev->channel->request_addr_callback = vmbus_request_addr;
	/* [한국어] 동시에 띄울 수 있는 요청 수. 이 드라이버가 여러 요청을 함께 보내므로
	 * 그 크기만큼 ID 를 관리한다. */
	hdev->channel->rqstor_size = HV_PCI_RQSTOR_SIZE;
/* [한국어] 요청 ID 관리를 채널에 맡길 준비가 끝났다. */

	ret = vmbus_open(hdev->channel, pci_ring_size, pci_ring_size, NULL, 0,
			 /* [한국어] 수신 콜백으로 이 파일의 핸들러를, 문맥으로 hbus 를 넘긴다. */
			 hv_pci_onchannelcallback, hbus);
	if (ret)
		/* [한국어] 채널을 열지 못하면 워크큐를 되돌린다. */
		goto destroy_wq;

	hv_set_drvdata(hdev, hbus);
/* [한국어] 이제부터 호스트와 대화할 수 있다. */

	ret = hv_pci_protocol_negotiation(hdev, pci_protocol_versions,
					  /* [한국어] 이 드라이버가 아는 판본 목록을 통째로 넘겨 협상시킨다. */
					  ARRAY_SIZE(pci_protocol_versions));
	if (ret)
		/* [한국어] 협상이 깨지면 채널부터 되돌린다. */
		goto close;

	ret = hv_allocate_config_window(hbus);
	/* [한국어] config 창을 얻지 못하면, */
	if (ret)
		/* [한국어] 채널을 되돌린다. */
		goto close;

	hbus->cfg_addr = ioremap(hbus->mem_config->start,
				 /* [한국어] config 창 전체를 매핑한다. 이 주소로 게스트가 config 공간을 읽고 쓴다. */
				 PCI_CONFIG_MMIO_LENGTH);
	if (!hbus->cfg_addr) {
		/* [한국어] 매핑에 실패하면 그 사실을 남긴다 — 다른 실패와 달리 이유가
		 * 주소 공간 부족이라 사용자가 알아야 한다. */
		dev_err(&hdev->device,
			"Unable to map a virtual address for config space\n");
		ret = -ENOMEM;
		goto free_config;
	}

	name = kasprintf(GFP_KERNEL, "%pUL", &hdev->dev_instance);
	/* [한국어] 이름을 만들지 못하면, */
	if (!name) {
		/* [한국어] 메모리 부족으로, */
		ret = -ENOMEM;
		goto unmap;
	}

	hbus->fwnode = irq_domain_alloc_named_fwnode(name);
	/* [한국어] fwnode 가 이름을 복사해 가므로 원본은 곧바로 놓는다. */
	kfree(name);
	if (!hbus->fwnode) {
		/* [한국어] fwnode 를 잡지 못하면 메모리 부족이다. */
		ret = -ENOMEM;
		goto unmap;
	}

	ret = hv_pcie_init_irq_domain(hbus);
	/* [한국어] 인터럽트 도메인 생성이 실패하면, */
	if (ret)
		/* [한국어] fwnode 부터 되돌린다. */
		goto free_fwnode;

	ret = hv_pci_query_relations(hdev);
	/* [한국어] 장치 목록 질의가 실패하면, */
	if (ret)
		/* [한국어] 인터럽트 도메인부터 되돌린다. */
		goto free_irq_domain;

	mutex_lock(&hbus->state_lock);

	ret = hv_pci_enter_d0(hdev);
	/* [한국어] D0 진입이 실패하면, */
	if (ret)
		/* [한국어] 잠금만 놓고 그 아래로 이어 되돌린다. */
		goto release_state_lock;

	ret = hv_pci_allocate_bridge_windows(hbus);
	/* [한국어] 브리지 창을 얻지 못하면, */
	if (ret)
		/* [한국어] D0 에서 빠져나오는 자리로 뛴다. */
		goto exit_d0;

	ret = hv_send_resources_allocated(hdev);
	/* [한국어] 자원 배정 통보가 실패하면, */
	if (ret)
		/* [한국어] 브리지 창부터 되돌린다. */
		goto free_windows;

	prepopulate_bars(hbus);

	hbus->state = hv_pcibus_probed;
/* [한국어] 여기까지 오면 호스트 쪽 준비가 끝났다. */

	ret = create_root_hv_pci_bus(hbus);
	/* [한국어] 루트 버스 생성이 실패하면, */
	if (ret)
		/* [한국어] 브리지 창부터 되돌린다. */
		goto free_windows;

	mutex_unlock(&hbus->state_lock);
	return 0;

free_windows:
	hv_pci_free_bridge_windows(hbus);
exit_d0:
	(void) hv_pci_bus_exit(hdev, true);
release_state_lock:
	mutex_unlock(&hbus->state_lock);
free_irq_domain:
	irq_domain_remove(hbus->irq_domain);
free_fwnode:
	irq_domain_free_fwnode(hbus->fwnode);
unmap:
	iounmap(hbus->cfg_addr);
free_config:
	hv_free_config_window(hbus);
close:
	vmbus_close(hdev->channel);
destroy_wq:
	destroy_workqueue(hbus->wq);
free_bus:
	kfree(hbus);
	return ret;
}

/* [한국어]
 * hv_pci_bus_exit - 호스트와의 세션을 정리하고 D0 에서 빠져나온다
 *
 * @hdev: 이 VMBus 장치.
 * @keep_devs: 자식 장치 목록을 남겨 둘지.
 * @return: 0 = 성공, -ETIMEDOUT 또는 전송 오류.
 *
 * remove, suspend, 그리고 D0 재시도가 모두 이 함수로 모인다. keep_devs 가
 * 그 셋을 가른다 — 재시도와 절전은 장치 목록을 남기고(다시 쓸 것이므로),
 * remove 만 지운다.
 *
 * 채널이 이미 rescind 됐으면 곧바로 성공을 답한다. 호스트가 이미 연결을
 * 끊었다는 뜻이라 보낼 곳이 없고, 그 상태에서 메시지를 보내면 시간만 낭비한다.
 *
 * 장치 목록을 지우는 방식이 눈에 띈다. 잠금 안에서 임시 목록으로 **옮기고**
 * 잠금 밖에서 하나씩 정리하는데, 정리 과정(pci_destroy_slot)이 잠들 수 있어
 * 스핀락 안에서 할 수 없기 때문이다.
 *
 * put_pcichild() 를 **두 번** 부르는 것이 중요하다. 하나는 목록이 들고 있던
 * 참조를, 다른 하나는 이 루프가 잠시 들고 있는 참조를 놓는 것이다.
 *
 * 자원 반납이 D0 종료보다 먼저다. 호스트 입장에서 자원이 배정된 채로 D0 에서
 * 나가면 그 자원이 새는 셈이 된다.
 *
 * 시간 초과 처리가 세밀하다. 10초 안에 응답이 없으면 요청 ID 를 명시적으로
 * 회수하는데, 그러지 않으면 이 함수가 돌아간 뒤 스택에 있던 패킷 주소로
 * 늦은 응답이 도착해 이미 사라진 메모리를 건드린다.
 *
 * 패킷을 스택에 두는 것도 그와 맞물린다 — 그래서 시간 초과 시 ID 회수가
 * 선택이 아니라 필수다.
 *
 * 실행 컨텍스트: remove, suspend, D0 재시도. 완료 대기가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 자원 반납 실패는 그 오류를, 응답이 없으면 -ETIMEDOUT 을
 * 올려보낸다.
 *
 * 호출 체인:
 *   hv_pci_remove() / hv_pci_suspend() / hv_pci_enter_d0() 의 재시도
 *     → [이 함수]
 *     → hv_send_resources_released() → vmbus_sendpacket_getid()
 *     → wait_for_completion_timeout()
 */
static int hv_pci_bus_exit(struct hv_device *hdev, bool keep_devs)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct vmbus_channel *chan = hdev->channel;
	struct {
		struct pci_packet teardown_packet;
		u8 buffer[sizeof(struct pci_message)];
	} pkt;
	struct pci_message *msg;
	struct hv_pci_compl comp_pkt;
	struct hv_pci_dev *hpdev, *tmp;
	unsigned long flags;
	u64 trans_id;
	int ret;

	/*
	 * After the host sends the RESCIND_CHANNEL message, it doesn't
	 * access the per-channel ringbuffer any longer.
	 */
	if (chan->rescind)
		return 0;

	if (!keep_devs) {
		struct list_head removed;

		/* Move all present children to the list on stack */
		INIT_LIST_HEAD(&removed);
		spin_lock_irqsave(&hbus->device_list_lock, flags);
		list_for_each_entry_safe(hpdev, tmp, &hbus->children, list_entry)
			list_move_tail(&hpdev->list_entry, &removed);
		spin_unlock_irqrestore(&hbus->device_list_lock, flags);

		/* Remove all children in the list */
		list_for_each_entry_safe(hpdev, tmp, &removed, list_entry) {
			list_del(&hpdev->list_entry);
			if (hpdev->pci_slot)
				pci_destroy_slot(hpdev->pci_slot);
			/* For the two refs got in new_pcichild_device() */
			put_pcichild(hpdev);
			put_pcichild(hpdev);
		}
	}

	ret = hv_send_resources_released(hdev);
	if (ret) {
		dev_err(&hdev->device,
			"Couldn't send resources released packet(s)\n");
		return ret;
	}

	memset(&pkt.teardown_packet, 0, sizeof(pkt.teardown_packet));
	init_completion(&comp_pkt.host_event);
	pkt.teardown_packet.completion_func = hv_pci_generic_compl;
	pkt.teardown_packet.compl_ctxt = &comp_pkt;
	msg = (struct pci_message *)pkt.buffer;
	msg->type = PCI_BUS_D0EXIT;

	ret = vmbus_sendpacket_getid(chan, msg, sizeof(*msg),
				     (unsigned long)&pkt.teardown_packet,
				     &trans_id, VM_PKT_DATA_INBAND,
				     VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		return ret;

	if (wait_for_completion_timeout(&comp_pkt.host_event, 10 * HZ) == 0) {
		/*
		 * The completion packet on the stack becomes invalid after
		 * 'return'; remove the ID from the VMbus requestor if the
		 * identifier is still mapped to/associated with the packet.
		 *
		 * Cf. hv_pci_onchannelcallback().
		 */
		vmbus_request_addr_match(chan, trans_id,
					 (unsigned long)&pkt.teardown_packet);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * hv_pci_remove() - Remove routine for this VMBus channel
 * @hdev:	VMBus's tracking struct for this root PCI bus
 */
/* [한국어]
 * hv_pci_remove - 가상 버스를 내리고 모든 자원을 놓는다
 *
 * @hdev: 이 VMBus 장치.
 *
 * hv_pci_probe() 의 짝이며 정확히 역순이다.
 *
 * 상태를 바꾸는 방식이 특이하다. tasklet 을 껐다 켜는 사이에 상태를
 * 바꾸는데, 채널 콜백이 tasklet 문맥에서 돌기 때문이다. 그 사이에는
 * 콜백이 실행되지 않으므로, 상태를 읽는 쪽과 바꾸는 쪽이 겹치지 않는다 —
 * 잠금 없이 같은 효과를 내는 방법이다.
 *
 * 워크큐를 파괴한 뒤 NULL 로 두는 것도 그와 맞물린다. 이후 경로가 그 값을
 * 보고 워크를 걸지 말지 판단한다.
 *
 * 설치된 상태일 때만 버스를 내린다. probe 가 중간에 실패했다면 버스가
 * 만들어지지 않았으므로 그 단계를 건너뛴다.
 *
 * pci_lock_rescan_remove() 로 감싸는 구간이 PCI 코어와의 접점이다.
 * 버스를 내리는 동안 다른 경로가 스캔하거나 제거하지 못하게 막는다.
 *
 * 도메인 번호를 여기서 돌려주지 않는 것에 주의할 만하다. 그것은 브리지가
 * 해제될 때 PCI 코어가 대신 한다.
 *
 * 실행 컨텍스트: VMBus 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 hv_pci_bus_exit() 의 실패도 무시된다.
 *
 * 호출 체인:
 *   VMBus 드라이버 코어 → [이 함수]
 *     → pci_stop_root_bus() → hv_pci_remove_slots() → pci_remove_root_bus()
 *     → hv_pci_bus_exit() → vmbus_close() → hv_free_config_window()
 *     → hv_pci_free_bridge_windows() → irq_domain_remove()
 */
static void hv_pci_remove(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus;

	hbus = hv_get_drvdata(hdev);
	if (hbus->state == hv_pcibus_installed) {
		tasklet_disable(&hdev->channel->callback_event);
		hbus->state = hv_pcibus_removing;
		tasklet_enable(&hdev->channel->callback_event);
		destroy_workqueue(hbus->wq);
		hbus->wq = NULL;
		/*
		 * At this point, no work is running or can be scheduled
		 * on hbus-wq. We can't race with hv_pci_devices_present()
		 * or hv_pci_eject_device(), it's safe to proceed.
		 */

		/* Remove the bus from PCI's point of view. */
		pci_lock_rescan_remove();
		pci_stop_root_bus(hbus->bridge->bus);
		hv_pci_remove_slots(hbus);
		pci_remove_root_bus(hbus->bridge->bus);
		pci_unlock_rescan_remove();
	}

	hv_pci_bus_exit(hdev, false);

	vmbus_close(hdev->channel);

	iounmap(hbus->cfg_addr);
	hv_free_config_window(hbus);
	hv_pci_free_bridge_windows(hbus);
	irq_domain_remove(hbus->irq_domain);
	irq_domain_free_fwnode(hbus->fwnode);

	kfree(hbus);
}

/* [한국어]
 * hv_pci_suspend - 절전 진입 시 세션을 정리하고 채널을 닫는다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, -EINVAL 또는 bus_exit 의 오류.
 *
 * 절전 중에는 호스트와의 채널이 끊기므로, 그 전에 세션을 정상적으로
 * 정리해야 한다.
 *
 * remove 와 달리 **장치 목록을 남긴다**(keep_devs = true). 복귀 후 같은
 * 장치들이 그대로 있을 것이므로 다시 열거할 필요가 없다.
 *
 * 상태 전환에 tasklet 을 껐다 켜는 관용이 여기서도 쓰인다. 다만 여기서는
 * 이전 상태를 기억해 두었다가, 설치된 상태가 아니었으면 -EINVAL 로 물러난다 —
 * 아직 세워지지 않았거나 이미 내려가는 중인 버스를 절전시킬 수는 없다.
 *
 * 워크큐를 flush 하되 파괴하지 않는 것이 remove 와 다르다. 복귀 후 다시
 * 쓸 것이기 때문이다.
 *
 * 실행 컨텍스트: 시스템 절전. 프로세스 컨텍스트.
 *
 * 에러 경로: 상태가 맞지 않으면 -EINVAL, 세션 정리가 실패하면 그 오류.
 * 어느 쪽이든 채널은 닫지 않는다.
 *
 * 호출 체인:
 *   VMBus PM 코어 → [이 함수]
 *     → flush_workqueue() → hv_pci_bus_exit(keep_devs=true) → vmbus_close()
 */
static int hv_pci_suspend(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	enum hv_pcibus_state old_state;
	int ret;

	/*
	 * hv_pci_suspend() must make sure there are no pending work items
	 * before calling vmbus_close(), since it runs in a process context
	 * as a callback in dpm_suspend().  When it starts to run, the channel
	 * callback hv_pci_onchannelcallback(), which runs in a tasklet
	 * context, can be still running concurrently and scheduling new work
	 * items onto hbus->wq in hv_pci_devices_present() and
	 * hv_pci_eject_device(), and the work item handlers can access the
	 * vmbus channel, which can be being closed by hv_pci_suspend(), e.g.
	 * the work item handler pci_devices_present_work() ->
	 * new_pcichild_device() writes to the vmbus channel.
	 *
	 * To eliminate the race, hv_pci_suspend() disables the channel
	 * callback tasklet, sets hbus->state to hv_pcibus_removing, and
	 * re-enables the tasklet. This way, when hv_pci_suspend() proceeds,
	 * it knows that no new work item can be scheduled, and then it flushes
	 * hbus->wq and safely closes the vmbus channel.
	 */
	tasklet_disable(&hdev->channel->callback_event);

	/* Change the hbus state to prevent new work items. */
	old_state = hbus->state;
	if (hbus->state == hv_pcibus_installed)
		hbus->state = hv_pcibus_removing;

	tasklet_enable(&hdev->channel->callback_event);

	if (old_state != hv_pcibus_installed)
		return -EINVAL;

	flush_workqueue(hbus->wq);

	ret = hv_pci_bus_exit(hdev, true);
	if (ret)
		return ret;

	vmbus_close(hdev->channel);

	return 0;
}

/* [한국어]
 * hv_pci_restore_msi_msg - 장치 하나의 MSI 메시지를 다시 만들어 쓴다
 *
 * @pdev: 대상 장치.
 * @arg: 쓰지 않는다.
 * @return: 0 = 성공, -EINVAL.
 *
 * 절전 복귀 후 호스트 쪽 MSI 매핑이 사라져 있으므로, 게스트가 알고 있는
 * 설정으로 다시 만들어야 한다.
 *
 * hv_compose_msi_msg() 를 직접 부르는 것이 요점이다. 그 함수가 호스트에게
 * "이 벡터를 만들어 달라" 는 메시지를 보내고 결과를 서술자에 채운다 —
 * 즉 복원이 아니라 **재생성** 이다.
 *
 * MSI 도 MSI-X 도 쓰지 않는 장치는 곧바로 성공을 답한다. 대부분의 장치가
 * 그렇다.
 *
 * guard(msi_descs_lock) 이 범위를 벗어날 때 자동으로 잠금을 놓는다.
 * 중간에 -EINVAL 로 나가는 경로가 있어 수동 해제로는 놓치기 쉽다.
 *
 * irq_data 가 없으면 WARN_ON_ONCE 로 잡는다. ASSOCIATED 서술자에는 반드시
 * 있어야 하는 것이라, 없다면 커널 내부의 모순이다.
 *
 * pci_walk_bus 의 콜백 규약을 따라 0 이 아닌 값은 순회를 멈춘다.
 *
 * 실행 컨텍스트: 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: irq_data 부재는 -EINVAL 이며 순회 전체가 멈춘다.
 *
 * 호출 체인:
 *   hv_pci_restore_msi_state() → pci_walk_bus() → [이 함수]
 *     → hv_compose_msi_msg()
 */
static int hv_pci_restore_msi_msg(struct pci_dev *pdev, void *arg)
{
	struct irq_data *irq_data;
	struct msi_desc *entry;

	if (!pdev->msi_enabled && !pdev->msix_enabled)
		return 0;

	guard(msi_descs_lock)(&pdev->dev);
	msi_for_each_desc(entry, &pdev->dev, MSI_DESC_ASSOCIATED) {
		irq_data = irq_get_irq_data(entry->irq);
		if (WARN_ON_ONCE(!irq_data))
			return -EINVAL;
		hv_compose_msi_msg(irq_data, &entry->msg);
	}
	return 0;
}

/*
 * Upon resume, pci_restore_msi_state() -> ... ->  __pci_write_msi_msg()
 * directly writes the MSI/MSI-X registers via MMIO, but since Hyper-V
 * doesn't trap and emulate the MMIO accesses, here hv_compose_msi_msg()
 * must be used to ask Hyper-V to re-create the IOMMU Interrupt Remapping
 * Table entries.
 */
/* [한국어]
 * hv_pci_restore_msi_state - 이 버스의 모든 장치에 MSI 재생성을 적용한다
 *
 * @hbus: 이 가상 버스.
 *
 * pci_walk_bus 로 위 콜백을 트리 전체에 적용하는 한 줄이다.
 *
 * 함수로 감싼 이유는 hv_pci_resume() 을 읽기 쉽게 하기 위해서다 —
 * 그쪽에서 이 이름 하나만 보면 무슨 일이 일어나는지 알 수 있다.
 *
 * pci_walk_bus 는 콜백의 오류를 버리므로, 위 콜백이 -EINVAL 을 돌려줘도
 * 여기서는 알 수 없다.
 *
 * 실행 컨텍스트: 절전 복귀. pci_bus_sem 을 잡으므로 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   hv_pci_resume() → [이 함수]
 *     → pci_walk_bus() → hv_pci_restore_msi_msg()
 */
static void hv_pci_restore_msi_state(struct hv_pcibus_device *hbus)
{
	pci_walk_bus(hbus->bridge->bus, hv_pci_restore_msi_msg, NULL);
}

/* [한국어]
 * hv_pci_resume - 절전에서 깨어나 호스트와의 세션을 처음부터 다시 세운다
 *
 * @hdev: 이 VMBus 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * hv_pci_suspend() 의 짝이지만 하는 일은 probe 에 가깝다. 채널을 다시 열고
 * 프로토콜부터 다시 합의하는데, 호스트 쪽에는 이전 세션의 상태가 남아 있지
 * 않기 때문이다.
 *
 * probe 와 다른 점이 셋이다.
 * 1. 판본 협상에서 **이미 합의한 판본 하나만** 시도한다. 다시 협상하다
 *    다른 판본으로 떨어지면 이미 열거된 장치들의 전제가 깨진다.
 * 2. 브리지 창을 다시 얻지 않는다. 그것은 게스트 쪽 자원이라 절전 중에도
 *    그대로 남아 있다.
 * 3. 루트 버스를 만들지 않는다. 이미 있는 버스를 그대로 쓴다.
 *
 * 대신 MSI 재생성이 추가된다. 호스트 쪽 벡터 매핑이 사라졌으므로,
 * 게스트가 알고 있는 설정으로 다시 만들어 달라고 요청해야 한다.
 *
 * BAR 미리 채우기가 여기에도 있는 것이 눈에 띈다. 호스트가 새 세션에서
 * BAR 값을 모르므로, 게스트가 알고 있는 값을 다시 써 준다.
 *
 * state_lock 을 D0 진입부터 상태 확정까지 쥐는 것이 probe 와 같다.
 *
 * 실행 컨텍스트: 시스템 복귀. 완료 대기가 여럿 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 두 갈래다 — 잠금을 잡기 전의 실패는 채널만 닫고,
 * 잡은 뒤의 실패는 잠금을 놓은 뒤 채널을 닫는다.
 *
 * 호출 체인:
 *   VMBus PM 코어 → [이 함수]
 *     → vmbus_open() → hv_pci_protocol_negotiation()
 *     → hv_pci_query_relations() → hv_pci_enter_d0()
 *     → hv_send_resources_allocated() → prepopulate_bars()
 *     → hv_pci_restore_msi_state()
 */
static int hv_pci_resume(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	enum pci_protocol_version_t version[1];
	int ret;

	hbus->state = hv_pcibus_init;

	hdev->channel->next_request_id_callback = vmbus_next_request_id;
	hdev->channel->request_addr_callback = vmbus_request_addr;
	hdev->channel->rqstor_size = HV_PCI_RQSTOR_SIZE;

	ret = vmbus_open(hdev->channel, pci_ring_size, pci_ring_size, NULL, 0,
			 hv_pci_onchannelcallback, hbus);
	if (ret)
		return ret;

	/* Only use the version that was in use before hibernation. */
	version[0] = hbus->protocol_version;
	ret = hv_pci_protocol_negotiation(hdev, version, 1);
	if (ret)
		goto out;

	ret = hv_pci_query_relations(hdev);
	if (ret)
		goto out;

	mutex_lock(&hbus->state_lock);

	ret = hv_pci_enter_d0(hdev);
	if (ret)
		goto release_state_lock;

	ret = hv_send_resources_allocated(hdev);
	if (ret)
		goto release_state_lock;

	prepopulate_bars(hbus);

	hv_pci_restore_msi_state(hbus);

	hbus->state = hv_pcibus_installed;
	mutex_unlock(&hbus->state_lock);
	return 0;

release_state_lock:
	mutex_unlock(&hbus->state_lock);
out:
	vmbus_close(hdev->channel);
	return ret;
}

static const struct hv_vmbus_device_id hv_pci_id_table[] = {
	/* PCI Pass-through Class ID */
	/* 44C4F61D-4444-4400-9D52-802E27EDE19F */
	{ HV_PCIE_GUID, },
	{ },
};

MODULE_DEVICE_TABLE(vmbus, hv_pci_id_table);

static struct hv_driver hv_pci_drv = {
	.name		= "hv_pci",
	.id_table	= hv_pci_id_table,
	.probe		= hv_pci_probe,
	.remove		= hv_pci_remove,
	.suspend	= hv_pci_suspend,
	.resume		= hv_pci_resume,
};

/* [한국어]
 * exit_hv_pci_drv - 드라이버 등록을 풀고 블록 접근 훅을 떼어 낸다
 *
 * @: 인자 없음.
 *
 * init_hv_pci_drv() 의 짝이다.
 *
 * 전역 훅 셋을 NULL 로 지우는 것이 이 함수의 실질이다. 그 훅은 다른
 * 서브시스템(Hyper-V 장치의 config 블록을 읽고 쓰는 쪽)이 이 드라이버의
 * 함수를 부르는 통로인데, 모듈이 내려간 뒤에도 남아 있으면 사라진 코드를
 * 가리키게 된다.
 *
 * 드라이버 등록을 **먼저** 푸는 순서가 중요하다. 그래야 남아 있는 장치들의
 * remove 가 끝난 뒤에 훅을 지운다.
 *
 * 실행 컨텍스트: 모듈 언로드. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → vmbus_driver_unregister()
 */
static void __exit exit_hv_pci_drv(void)
{
	vmbus_driver_unregister(&hv_pci_drv);

	hvpci_block_ops.read_block = NULL;
	hvpci_block_ops.write_block = NULL;
	hvpci_block_ops.reg_blk_invalidate = NULL;
}

/* [한국어]
 * init_hv_pci_drv - Hyper-V 게스트에서만 이 드라이버를 등록한다
 *
 * @return: 0 = 성공, -ENODEV 또는 등록 오류.
 *
 * 두 가지를 확인한 뒤에야 등록한다.
 *
 * 1. Hyper-V 게스트인지. 아니면 이 드라이버가 할 일이 없다.
 * 2. 루트 파티션이면서 중첩이 아닌지. 루트 파티션은 하이퍼바이저 자신을
 *    호스팅하는 특별한 파티션이라, 그쪽에서는 이 준가상화 경로가 아니라
 *    실제 하드웨어 드라이버가 PCI 를 다룬다. 중첩 환경에서는 그 예외가
 *    적용되지 않아 이 드라이버가 필요하다.
 *
 * 아키텍처별 인터럽트 칩 초기화가 그 다음이다. x86 과 ARM64 에서 하는
 * 일이 달라 따로 두었다.
 *
 * 블록 접근 훅 셋을 등록하는 것이 등록 **전** 인 것에 주의할 만하다.
 * 드라이버가 등록되면 곧바로 장치가 붙을 수 있고, 그 장치가 블록 접근을
 * 쓸 수 있기 때문이다.
 *
 * 실행 컨텍스트: 모듈 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 환경이 맞지 않으면 -ENODEV, 인터럽트 칩 초기화 실패는 그 오류.
 *
 * 호출 체인:
 *   module_init → [이 함수]
 *     → hv_is_hyperv_initialized() → hv_pci_irqchip_init()
 *     → vmbus_driver_register()
 */
static int __init init_hv_pci_drv(void)
{
	int ret;

	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	if (hv_root_partition() && !hv_nested)
		return -ENODEV;

	ret = hv_pci_irqchip_init();
	if (ret)
		return ret;

	/* Initialize PCI block r/w interface */
	hvpci_block_ops.read_block = hv_read_config_block;
	hvpci_block_ops.write_block = hv_write_config_block;
	hvpci_block_ops.reg_blk_invalidate = hv_register_block_invalidate;

	return vmbus_driver_register(&hv_pci_drv);
}

module_init(init_hv_pci_drv);
module_exit(exit_hv_pci_drv);

MODULE_DESCRIPTION("Hyper-V PCI");
MODULE_LICENSE("GPL v2");
