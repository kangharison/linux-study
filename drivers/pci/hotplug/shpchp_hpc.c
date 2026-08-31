// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard PCI Hot Plug Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>,<kristen.c.accardi@intel.com>
 *
 */
/* [한국어] 설명 SHPC(Standard Hot-Plug Controller) 핫플러그 드라이버의 하드웨어 접근 계층 (shpchp_hpc.c)
 * 
 * === 파일의 역할 ===
 * shpchp 드라이버에서 SHPC 하드웨어 레지스터를 직접 읽고 쓰는 유일한 파일이다. SHPC 는
 * PCI SIG 의 "PCI Standard Hot-Plug Controller and Subsystem Specification" 이 정의한,
 * PCIe 이전 세대의 PCI/PCI-X 브리지에 얹히는 표준 핫플러그 컨트롤러다. 이 파일이 하는
 * 일은 네 가지다. (1) shpc_init() 이 브리지의 SHPC capability(ID 0x0C)를 찾아 Working
 * Register Set 을 ioremap 하고, 컨트롤러 능력(슬롯 개수, 첫 device 번호, 물리 슬롯 번호,
 * 번호 증감 방향)을 읽어 struct controller 를 채우고, MSI(실패 시 INTx) 인터럽트 또는
 * 폴링 타이머를 설치한다. (2) shpc_write_cmd() 가 Command 레지스터에 명령을 넣고 완료를
 * 기다리는 SHPC 명령 큐 프로토콜을 구현하며, 그 위에 전원/LED/속도 제어 함수들이 얹힌다.
 * (3) shpchp_get_ 계열이 Logical Slot Register 의 비트 필드를 슬롯 상태(전원, LED, MRL,
 * 카드 존재, 어댑터 속도)로 번역해 준다. (4) shpc_isr() 이 Interrupt Locator 레지스터를
 * 읽어 어느 슬롯의 어떤 이벤트인지 가려내고 shpchp_ctrl.c 의 핸들러로 넘긴다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * shpchp 는 세 조각으로 나뉜다 — shpchp_core.c 가 pci_driver 등록과 슬롯의 sysfs 노출을
 * 맡고, 이 파일이 하드웨어를 맡고, shpchp_ctrl.c 가 슬롯 상태 기계를 맡는다. pciehp 의
 * pciehp_hpc.c / pciehp_ctrl.c 분업과 정확히 같은 구도이며, 이 파일이 pciehp_hpc.c 에
 * 대응한다. 다만 두 규격의 하드웨어 인터페이스는 결정적으로 다르다. pciehp 는 PCIe
 * capability 안의 Slot Control/Slot Status 레지스터를 config 공간 접근 한 번으로 읽고
 * 쓰지만, SHPC 는 브리지 BAR0 안에 놓인 MMIO Working Register Set 을 매핑해서 다루고,
 * 슬롯 제어를 레지스터 직접 쓰기가 아니라 "Command 레지스터에 명령 코드를 써 넣고
 * Controller Busy 비트가 내려가기를(또는 Command Completion 인터럽트를) 기다리는" 명령 큐
 * 방식으로 한다. 컨트롤러가 한 번에 한 명령만 받으므로 cmd_lock 뮤텍스로 직렬화하고 1초
 * 타임아웃을 두는 코드가 이 파일에만 있는 이유가 그것이다. 레지스터 배치는 shpchp.h 의
 * struct ctrl_reg 가 그대로 보여 준다 — 고정부가 오프셋 0x00~0x20 의 9 dword 이고 그
 * 뒤 0x24 부터 슬롯당 1 dword 의 Logical Slot Register 가 이어지므로, 매핑 크기가
 * 0x24 + 4 * 슬롯수 로 계산된다. 실행 컨텍스트는 두 갈래다. shpc_init(),
 * shpchp_release_ctlr(), 모든 명령 발행 경로는 프로세스 컨텍스트(잠들 수 있음)이고,
 * shpc_isr() 만 인터럽트 컨텍스트다(폴링 모드에서는 타이머 콜백 컨텍스트).
 * 
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽: linux/pci.h(pci_find_capability, pci_enable_device,
 * pci_enable_msi, pci_resource_start 계열, pci_ 설정 접근 함수들), linux/interrupt.h
 * (request_irq/free_irq, irqreturn_t, IRQF_SHARED), 커널 io 계층(readb/readw/readl,
 * writew/writel, ioremap/iounmap, request_mem_region/release_mem_region), 타이머 계층
 * (timer_setup/add_timer/timer_delete), 그리고 shpchp.h 가 주는 struct controller /
 * struct slot / ctrl_offsets enum / ctrl_dbg 계열 로그 매크로다. 이 파일에 의존하는 쪽:
 * shpchp_core.c 가 probe 에서 shpc_init() 을 부르고 remove 에서 shpchp_release_ctlr() 을
 * 부르며 sysfs 콜백에서 shpchp_get_ 계열과 shpchp_set_attention_status() 를 쓴다.
 * shpchp_ctrl.c 는 슬롯을 켜고 끌 때 shpchp_power_on_slot / shpchp_slot_enable /
 * shpchp_slot_disable / shpchp_green_led_ 계열 / shpchp_set_bus_speed_mode /
 * shpchp_get_adapter_speed / shpchp_query_power_fault 를 부른다. 반대 방향으로는 이
 * 파일의 shpc_isr() 이 shpchp_ctrl.c 의 shpchp_handle_switch_change /
 * shpchp_handle_attention_button / shpchp_handle_presence_change /
 * shpchp_handle_power_fault 네 함수를 부르고, shpchp_release_ctlr() 이 shpchp_core.c 의
 * cleanup_slots() 를 부른다. 데이터 흐름은 "하드웨어 → INTR_LOC/SLOT_REG 읽기 →
 * shpc_isr → shpchp_handle_ 계열 → 워크큐 → shpchp_ctrl.c 상태 기계 → 다시 이 파일의
 * 명령 발행 → CMD 레지스터 → 하드웨어" 라는 한 바퀴다. 공유 상태는 struct controller
 * (creg 매핑 주소, cmd_lock, 명령 완료 대기열 queue, num_slots, slot_device_offset)와
 * struct slot(hp_slot 논리 슬롯 번호)이다.
 * 
 * NVMe 와의 관계에 대해 분명히 해 둔다. SHPC 는 PCI/PCI-X 시절 규격이고 NVMe 는 PCIe
 * 전용이므로 둘은 사실상 무관하다. 이 트리를 확인한 결과 drivers/nvme 가 shpchp_ 계열
 * 심볼을 부르는 곳은 한 군데도 없다. 관계가 있다면 방향은 하나뿐이다 — 핫플러그가 버스
 * 열거나 제거를 일으키면 그 결과로 PCI 코어가 드라이버 probe/remove 를 부르고, 그
 * 끝에서 nvme_probe() / nvme_remove() 가 불릴 수 있다는 간접적 관계다.
 * 
 * === 주요 함수/구조체 요약 ===
 * - shpc_init(): capability 탐색 → 간접 읽기로 base offset 획득 → MMIO 매핑 → 컨트롤러
 *   능력 읽기 → 인터럽트/폴링 설치 → 슬롯 이벤트 인터럽트 개방. 초기화 전 과정.
 * - shpc_write_cmd(): cmd_lock 을 쥐고 CMD 레지스터에 명령을 발행한 뒤 완료를 기다리는
 *   이 파일의 심장. 모든 슬롯 제어가 이 함수를 통과한다.
 * - shpc_wait_cmd() / shpc_poll_ctrl_busy() / is_ctrl_busy(): 완료 대기의 세 층.
 *   인터럽트 모드는 대기열에서 자고, 폴링 모드는 0.1초 간격으로 Busy 비트를 본다.
 * - shpc_isr(): Interrupt Locator 를 읽어 명령 완료와 슬롯 이벤트를 갈라 보내고,
 *   RW1C 레지스터를 되써서 이벤트 래치를 지운다.
 * - shpchp_check_cmd_status(): Command Status 의 [3:1] 오류 코드를 상수로 번역.
 * - shpchp_get_ 계열 6개: Logical Slot Register 의 비트 필드를 sysfs 표현으로 번역.
 * - shpc_get_max_bus_speed() / shpc_get_cur_bus_speed(): Slot Available I/II 와
 *   Secondary Bus Configuration 을 읽어 pci_bus 의 max/cur 속도를 채운다.
 * - shpchp_release_ctlr(): 마스크 → 슬롯 정리 → 핸들러/타이머 해제 → 매핑 반납 순서.
 * 이 파일은 구조체를 새로 정의하지 않고 shpchp.h 의 struct controller / struct slot /
 * struct ctrl_reg 와 ctrl_offsets enum 을 그대로 쓴다. */

/* [한국어] container_of, ARRAY_SIZE 등 커널 공용 매크로와 기본 타입 정의를 얻기 위해 포함한다 */
#include <linux/kernel.h>
/* [한국어] 모듈로 빌드될 때 shpchp.h 의 MY_NAME 매크로가 THIS_MODULE 을 참조하므로 필요하다. MY_NAME 은 request_mem_region 과 request_irq 의 소유자 이름으로 쓰인다 */
#include <linux/module.h>
/* [한국어] u8/u16/u32 같은 고정 폭 정수 타입 — SHPC 레지스터 폭을 정확히 표현해야 하므로 필수다 */
#include <linux/types.h>
/* [한국어] struct pci_dev, pci_find_capability(), pci_enable_device(), pci_enable_msi(), pci_resource_start 계열, PCI_VENDOR_ID_AMD 같은 ID 상수, 그리고 readl/writel 과 ioremap 을 끌어오는 io 계층까지 이 헤더를 통해 들어온다 */
#include <linux/pci.h>
/* [한국어] request_irq()/free_irq(), irqreturn_t, IRQ_HANDLED/IRQ_NONE, IRQF_SHARED — shpc_isr 을 등록하고 공유 IRQ 로 동작하기 위해 필요하다 */
#include <linux/interrupt.h>

/* [한국어] 이 드라이버의 공용 헤더. struct controller/struct slot 정의, ctrl_offsets 레지스터 오프셋 enum, ctrl_dbg 계열 로그 매크로, shpchp_poll_mode 전역 선언, MY_NAME 을 모두 여기서 얻는다 */
#include "shpchp.h"

/* Slot Available Register I field definition */
/* [한국어] Slot Available Register I(오프셋 0x04)의 통상 PCI 33MHz 슬롯 개수 필드[4:0]. shpc_get_max_bus_speed() 가 최저 속도 후보로 검사한다 */
#define SLOT_33MHZ		0x0000001f
/* [한국어] 같은 레지스터의 PCI-X 66MHz 슬롯 개수 필드[12:8] */
#define SLOT_66MHZ_PCIX		0x00001f00
/* [한국어] 같은 레지스터의 PCI-X 100MHz 슬롯 개수 필드[20:16] */
#define SLOT_100MHZ_PCIX	0x001f0000
/* [한국어] 같은 레지스터의 PCI-X 133MHz 슬롯 개수 필드[28:24]. PI==1 컨트롤러가 광고할 수 있는 최고 속도다 */
#define SLOT_133MHZ_PCIX	0x1f000000

/* Slot Available Register II field definition */
/* [한국어] Slot Available Register II(오프셋 0x08)의 통상 PCI 66MHz 슬롯 개수 필드[4:0]. 66MHz 만 Register I 이 아니라 II 에 있다는 점이 규격의 함정이다 */
#define SLOT_66MHZ		0x0000001f
/* [한국어] PCI-X 266 모드에서 66MHz 클럭 슬롯 개수 필드[11:8] */
#define SLOT_66MHZ_PCIX_266	0x00000f00
/* [한국어] PCI-X 266 모드에서 100MHz 클럭 슬롯 개수 필드[15:12] */
#define SLOT_100MHZ_PCIX_266	0x0000f000
/* [한국어] PCI-X 266 모드에서 133MHz 클럭 슬롯 개수 필드[19:16] */
#define SLOT_133MHZ_PCIX_266	0x000f0000
/* [한국어] PCI-X 533 모드에서 66MHz 클럭 슬롯 개수 필드[23:20] */
#define SLOT_66MHZ_PCIX_533	0x00f00000
/* [한국어] PCI-X 533 모드에서 100MHz 클럭 슬롯 개수 필드[27:24] */
#define SLOT_100MHZ_PCIX_533	0x0f000000
/* [한국어] PCI-X 533 모드에서 133MHz 클럭 슬롯 개수 필드[31:28]. 이 컨트롤러가 낼 수 있는 최고 속도 조합이다 */
#define SLOT_133MHZ_PCIX_533	0xf0000000

/* Slot Configuration */
/* [한국어] Slot Configuration 레지스터(오프셋 0x0C)의 Slots Implemented 필드[4:0]. 이 컨트롤러가 관장하는 물리 슬롯 개수이며 MMIO 매핑 크기 계산의 근거가 된다 */
#define SLOT_NUM		0x0000001F
/* [한국어] 같은 레지스터의 First Device Number 필드[12:8]. 첫 핫플러그 슬롯이 쓰는 PCI device 번호로, 논리 슬롯 번호를 device 번호로 바꿀 때 더한다 */
#define	FIRST_DEV_NUM		0x00001F00
/* [한국어] 같은 레지스터의 Physical Slot Number 필드[26:16]. 섀시에 인쇄된 첫 슬롯 번호이며 sysfs 슬롯 이름의 시작값이 된다 */
#define PSN			0x07FF0000
/* [한국어] PSN Up/Down 비트(bit29). 1 이면 슬롯 번호가 증가 방향, 0 이면 감소 방향이라는 뜻이다 */
#define	UPDOWN			0x20000000
/* [한국어] MRL Sensor Implemented 비트(bit30). 래치 센서 유무를 광고한다. 이 파일에서는 정의만 두고 실제로 참조하지 않는다 */
#define	MRLSENSOR		0x40000000
/* [한국어] Attention Button Implemented 비트(bit31). 버튼 유무를 광고한다. 역시 정의만 두고 참조하지 않는다 */
#define ATTN_BUTTON		0x80000000

/*
 * Interrupt Locator Register definitions
 */
/* [한국어] Interrupt Locator 레지스터(오프셋 0x18)의 bit0 — 명령 완료 인터럽트가 대기 중임을 뜻한다 */
#define CMD_INTR_PENDING	(1 << 0)
/* [한국어] 같은 레지스터에서 i 번 논리 슬롯의 이벤트 대기 비트. bit0 이 명령 완료용으로 예약돼 있어 슬롯 비트가 1 부터 시작하므로 i+1 로 밀어 준다 */
#define SLOT_INTR_PENDING(i)	(1 << (i + 1))

/*
 * Controller SERR-INT Register
 */
/* [한국어] Controller SERR-INT 레지스터(오프셋 0x20)의 Global Interrupt Mask(bit0). 1 이면 컨트롤러가 어떤 인터럽트도 올리지 않는다. shpc_isr 진입 시 중첩을 막는 데도 쓴다 */
#define GLOBAL_INTR_MASK	(1 << 0)
/* [한국어] 같은 레지스터의 Global SERR Mask(bit1). 1 이면 SERR# 신호를 올리지 않는다. 이 드라이버는 SERR 를 끝까지 막아 둔 채로 둔다 */
#define GLOBAL_SERR_MASK	(1 << 1)
/* [한국어] Command Completion Interrupt Mask(bit2). 1 이면 명령 완료를 인터럽트로 알리지 않는다 */
#define COMMAND_INTR_MASK	(1 << 2)
/* [한국어] Arbiter SERR Mask(bit3). 1 이면 버스 중재기 타임아웃을 SERR 로 알리지 않는다 */
#define ARBITER_SERR_MASK	(1 << 3)
/* [한국어] Command Completion Detected(bit16). RW1C 상태 비트이며, 1 을 되쓰면 지워진다. shpc_isr 이 레지스터를 그대로 되쓰는 것만으로 이 비트가 클리어되는 이유다. 이 파일에서는 상수 정의만 두고 이름으로 참조하지는 않는다 */
#define COMMAND_DETECTED	(1 << 16)
/* [한국어] Arbiter Timeout Detected(bit17). 역시 RW1C 이며 정의만 두고 참조하지 않는다 */
#define ARBITER_DETECTED	(1 << 17)
/* [한국어] 예약 비트 [31:18] 마스크. 규격상 반드시 0 으로 써야 하는(RsvdZ) 구간이라, 이 레지스터를 되쓸 때마다 이 마스크로 떨어뜨린다 */
#define SERR_INTR_RSVDZ_MASK	0xfffc0000

/*
 * Logical Slot Register definitions
 */
/* [한국어] i 번 논리 슬롯의 Logical Slot Register 오프셋. 고정부가 0x24 바이트이고 슬롯당 1 dword 이므로 SLOT1(0x24) + 4*i 가 된다 */
#define SLOT_REG(i)		(SLOT1 + (4 * i))

/* [한국어] Slot State 필드[1:0]의 시프트량. 0 이지만 다른 필드와 추출 형태를 통일하려고 명시해 두었다 */
#define SLOT_STATE_SHIFT	(0)
/* [한국어] Slot State 필드 마스크[1:0] */
#define SLOT_STATE_MASK		(3 << 0)
/* [한국어] Slot State 인코딩 1 = Power Only(전원만 들어가고 버스에는 미연결) */
#define SLOT_STATE_PWRONLY	(1)
/* [한국어] Slot State 인코딩 2 = Enabled(전원이 들어가고 버스에도 연결) */
#define SLOT_STATE_ENABLED	(2)
/* [한국어] Slot State 인코딩 3 = Disabled(전원 차단). 인코딩 0 은 예약값이다 */
#define SLOT_STATE_DISABLED	(3)
/* [한국어] Power Indicator(녹색 LED) 상태 필드[3:2]의 시프트량. 이 파일에서 실제로 쓰이지는 않고 필드 표를 완성하려고 둔 정의다 */
#define PWR_LED_STATE_SHIFT	(2)
/* [한국어] Power Indicator 상태 필드 마스크[3:2]. 역시 참조되지 않는다 — 녹색 LED 는 읽지 않고 쓰기만 하기 때문이다 */
#define PWR_LED_STATE_MASK	(3 << 2)
/* [한국어] Attention Indicator(황색 LED) 상태 필드[5:4]의 시프트량 */
#define ATN_LED_STATE_SHIFT	(4)
/* [한국어] Attention Indicator 상태 필드 마스크[5:4] */
#define ATN_LED_STATE_MASK	(3 << 4)
/* [한국어] Attention Indicator 인코딩 1 = 켜짐 */
#define ATN_LED_STATE_ON	(1)
/* [한국어] Attention Indicator 인코딩 2 = 깜빡임 */
#define ATN_LED_STATE_BLINK	(2)
/* [한국어] Attention Indicator 인코딩 3 = 꺼짐. 인코딩 0 은 예약값이다 */
#define ATN_LED_STATE_OFF	(3)
/* [한국어] Power Fault 비트(bit6). 액티브 로우라 0 이 결함을 뜻하므로 읽는 쪽에서 반드시 뒤집어야 한다 */
#define POWER_FAULT		(1 << 6)
/* [한국어] Attention Button 현재 눌림 상태 비트(bit7). 이 파일은 눌림 순간(bit18 래치)만 보므로 이 비트는 정의만 두고 참조하지 않는다 */
#define ATN_BUTTON		(1 << 7)
/* [한국어] MRL Sensor 비트(bit8). 1 이면 래치가 열려 있다 */
#define MRL_SENSOR		(1 << 8)
/* [한국어] 66MHz Capable 비트(bit9). 꽂힌 카드가 66MHz 를 지원하는지 알려 준다 */
#define MHZ66_CAP		(1 << 9)
/* [한국어] Card Present 필드[11:10]의 시프트량 */
#define PRSNT_SHIFT		(10)
/* [한국어] Card Present 필드 마스크[11:10]. PRSNT1#/PRSNT2# 두 신호를 그대로 반영하며 0x3 이 빈 슬롯이다 */
#define PRSNT_MASK		(3 << 10)
/* [한국어] PCI-X Capability 필드의 시프트량(12). 필드 폭은 PI 값에 따라 달라진다 */
#define PCIX_CAP_SHIFT		(12)
/* [한국어] PI==1 일 때의 PCI-X Capability 마스크 — 2비트[13:12] */
#define PCIX_CAP_MASK_PI1	(3 << 12)
/* [한국어] PI==2 일 때의 PCI-X Capability 마스크 — 3비트[14:12]. 266/533 을 표현하려고 한 비트가 늘었다 */
#define PCIX_CAP_MASK_PI2	(7 << 12)
/* [한국어] Presence Detect Changed 래치(bit16). RW1C 이며 카드 삽입/제거를 알린다 */
#define PRSNT_CHANGE_DETECTED	(1 << 16)
/* [한국어] Isolated Power Fault Detected 래치(bit17). RW1C. 슬롯이 버스에서 분리된 상태에서의 전원 결함이다 */
#define ISO_PFAULT_DETECTED	(1 << 17)
/* [한국어] Attention Button Pressed 래치(bit18). RW1C. 버튼이 눌린 순간을 잡는다 */
#define BUTTON_PRESS_DETECTED	(1 << 18)
/* [한국어] MRL Sensor Changed 래치(bit19). RW1C. 래치 개폐가 바뀐 순간을 잡는다 */
#define MRL_CHANGE_DETECTED	(1 << 19)
/* [한국어] Connected Power Fault Detected 래치(bit20). RW1C. 슬롯이 버스에 연결된 상태에서의 전원 결함이다 */
#define CON_PFAULT_DETECTED	(1 << 20)
/* [한국어] Presence Detect 인터럽트 마스크(bit24). 1 이면 그 이벤트로 인터럽트를 올리지 않는다 */
#define PRSNT_CHANGE_INTR_MASK	(1 << 24)
/* [한국어] Isolated Power Fault 인터럽트 마스크(bit25) */
#define ISO_PFAULT_INTR_MASK	(1 << 25)
/* [한국어] Attention Button 인터럽트 마스크(bit26) */
#define BUTTON_PRESS_INTR_MASK	(1 << 26)
/* [한국어] MRL Change 인터럽트 마스크(bit27) */
#define MRL_CHANGE_INTR_MASK	(1 << 27)
/* [한국어] Connected Power Fault 인터럽트 마스크(bit28) */
#define CON_PFAULT_INTR_MASK	(1 << 28)
/* [한국어] MRL Change SERR 마스크(bit29). 이 드라이버는 이 비트를 끝까지 세워 둬 SERR 를 막는다 */
#define MRL_CHANGE_SERR_MASK	(1 << 29)
/* [한국어] Connected Power Fault SERR 마스크(bit30). 역시 계속 세워 둔다 */
#define CON_PFAULT_SERR_MASK	(1 << 30)
/* [한국어] 이 레지스터의 RsvdZ 구간 — bit15 와 bit23:21. 되쓸 때마다 반드시 0 으로 떨어뜨려야 하는 비트들이다 */
#define SLOT_REG_RSVDZ_MASK	((1 << 15) | (7 << 21))

/*
 * SHPC Command Code definitions
 *
 *     Slot Operation				00h - 3Fh
 *     Set Bus Segment Speed/Mode A		40h - 47h
 *     Power-Only All Slots			48h
 *     Enable All Slots				49h
 *     Set Bus Segment Speed/Mode B (PI=2)	50h - 5Fh
 *     Reserved Command Codes			60h - BFh
 *     Vendor Specific Commands			C0h - FFh
 */
/* [한국어] Slot Operation 명령의 Slot State 필드 값 1 = Power Only. 카드에 전원만 넣고 버스에는 연결하지 않는다 */
#define SET_SLOT_PWR		0x01	/* Slot Operation */
/* [한국어] Slot State 필드 값 2 = Enable. 전원을 넣고 버스에도 연결한다 */
#define SET_SLOT_ENABLE		0x02
/* [한국어] Slot State 필드 값 3 = Disable. 버스에서 떼고 전원을 내린다 */
#define SET_SLOT_DISABLE	0x03
/* [한국어] Power Indicator 필드 값 = ON(비트 [3:2] 에 01) */
#define SET_PWR_ON		0x04
/* [한국어] Power Indicator 필드 값 = BLINK(비트 [3:2] 에 10) */
#define SET_PWR_BLINK		0x08
/* [한국어] Power Indicator 필드 값 = OFF(비트 [3:2] 에 11) */
#define SET_PWR_OFF		0x0c
/* [한국어] Attention Indicator 필드 값 = ON(비트 [5:4] 에 01) */
#define SET_ATTN_ON		0x10
/* [한국어] Attention Indicator 필드 값 = BLINK(비트 [5:4] 에 10) */
#define SET_ATTN_BLINK		0x20
/* [한국어] Attention Indicator 필드 값 = OFF(비트 [5:4] 에 11). 세 필드가 겹치지 않으므로 OR 로 합쳐 한 명령에 실을 수 있다 */
#define SET_ATTN_OFF		0x30
/* [한국어] Set Bus Segment Speed/Mode A 계열 시작 — 통상 PCI 33MHz. PI 값과 무관하게 모든 SHPC 가 이해한다 */
#define SETA_PCI_33MHZ		0x40	/* Set Bus Segment Speed/Mode A */
/* [한국어] A 계열 통상 PCI 66MHz */
#define SETA_PCI_66MHZ		0x41
/* [한국어] A 계열 PCI-X 66MHz */
#define SETA_PCIX_66MHZ		0x42
/* [한국어] A 계열 PCI-X 100MHz */
#define SETA_PCIX_100MHZ	0x43
/* [한국어] A 계열 PCI-X 133MHz. PI==1 컨트롤러가 갈 수 있는 최고 속도다 */
#define SETA_PCIX_133MHZ	0x44
/* [한국어] A 계열의 예약 코드 0x45. 정의만 두고 쓰지 않는다 */
#define SETA_RESERVED1		0x45
/* [한국어] A 계열의 예약 코드 0x46. 정의만 두고 쓰지 않는다 */
#define SETA_RESERVED2		0x46
/* [한국어] A 계열의 예약 코드 0x47. 정의만 두고 쓰지 않는다 */
#define SETA_RESERVED3		0x47
/* [한국어] 모든 슬롯을 한꺼번에 Power Only 로 만드는 명령. 이 드라이버는 슬롯을 하나씩 다루므로 정의만 두고 쓰지 않는다 */
#define SET_PWR_ONLY_ALL	0x48	/* Power-Only All Slots */
/* [한국어] 모든 슬롯을 한꺼번에 Enable 하는 명령. 역시 정의만 두고 쓰지 않는다 */
#define SET_ENABLE_ALL		0x49	/* Enable All Slots */
/* [한국어] Set Bus Segment Speed/Mode B 계열 시작 — 통상 PCI 33MHz. PI==2 컨트롤러에서만 유효하며, 이 드라이버는 33/66MHz 는 A 계열로 처리하므로 이 코드를 쓰지 않는다 */
#define	SETB_PCI_33MHZ		0x50	/* Set Bus Segment Speed/Mode B */
/* [한국어] B 계열 통상 PCI 66MHz. 위와 같은 이유로 쓰이지 않는다 */
#define SETB_PCI_66MHZ		0x51
/* [한국어] B 계열 PCI-X 66MHz Parity Mode. 이 드라이버는 pci_bus_speed 에 PM 을 구분하는 값이 없어 쓰지 않는다 */
#define SETB_PCIX_66MHZ_PM	0x52
/* [한국어] B 계열 PCI-X 100MHz Parity Mode. 쓰이지 않는다 */
#define SETB_PCIX_100MHZ_PM	0x53
/* [한국어] B 계열 PCI-X 133MHz Parity Mode. 쓰이지 않는다 */
#define SETB_PCIX_133MHZ_PM	0x54
/* [한국어] B 계열 PCI-X 66MHz Error-checking Mode. PCI_SPEED_66MHz_PCIX_ECC 요청이 이 코드로 번역된다 */
#define SETB_PCIX_66MHZ_EM	0x55
/* [한국어] B 계열 PCI-X 100MHz Error-checking Mode */
#define SETB_PCIX_100MHZ_EM	0x56
/* [한국어] B 계열 PCI-X 133MHz Error-checking Mode */
#define SETB_PCIX_133MHZ_EM	0x57
/* [한국어] B 계열 PCI-X 266 의 66MHz 클럭 */
#define SETB_PCIX_66MHZ_266	0x58
/* [한국어] B 계열 PCI-X 266 의 100MHz 클럭 */
#define SETB_PCIX_100MHZ_266	0x59
/* [한국어] B 계열 PCI-X 266 의 133MHz 클럭 */
#define SETB_PCIX_133MHZ_266	0x5a
/* [한국어] B 계열 PCI-X 533 의 66MHz 클럭 */
#define SETB_PCIX_66MHZ_533	0x5b
/* [한국어] B 계열 PCI-X 533 의 100MHz 클럭 */
#define SETB_PCIX_100MHZ_533	0x5c
/* [한국어] B 계열 PCI-X 533 의 133MHz 클럭. 이 드라이버가 낼 수 있는 최고 속도 명령이다 */
#define SETB_PCIX_133MHZ_533	0x5d
/* [한국어] B 계열의 예약 코드 0x5e. 정의만 두고 쓰지 않는다 */
#define SETB_RESERVED1		0x5e
/* [한국어] B 계열의 예약 코드 0x5f. 정의만 두고 쓰지 않는다 */
#define SETB_RESERVED2		0x5f

/*
 * SHPC controller command error code
 */
/* [한국어] Command Status[3:1] 오류 코드 1 — MRL(래치)이 열려 있어 명령을 거부했다 */
#define SWITCH_OPEN		0x1
/* [한국어] 오류 코드 2 — 컨트롤러가 이해하지 못하는 명령 코드였다 */
#define INVALID_CMD		0x2
/* [한국어] 오류 코드 4 — 요구한 버스 속도/모드를 이 세그먼트가 낼 수 없다. 코드 3/5/6/7 은 규격에 정의가 없다 */
#define INVALID_SPEED_MODE	0x4

/*
 * For accessing SHPC Working Register Set via PCI Configuration Space
 */
/* [한국어] SHPC capability 기준 +2 바이트에 있는 DWORD Select 레지스터. MMIO 를 아직 매핑하지 못한 초기화 단계에서 Working Register Set 을 config 공간으로 간접 접근하기 위한 인덱스 창이다 */
#define DWORD_SELECT		0x2
/* [한국어] SHPC capability 기준 +4 바이트에 있는 DWORD Data 레지스터. Select 에 쓴 인덱스가 가리키는 dword 값을 여기서 읽는다 */
#define DWORD_DATA		0x4

/* Field Offset in Logical Slot Register - byte boundary */
/* [한국어] Logical Slot Register 안에서 이벤트 래치(bit16~20)가 시작되는 바이트 오프셋 2. 워드 단위가 아닌 바이트 단위 접근용 상수인데, 이 파일에서는 정의만 두고 실제로 쓰지 않는다 */
#define SLOT_EVENT_LATCH	0x2
/* [한국어] Logical Slot Register 안에서 SERR/인터럽트 마스크(bit24~30)가 있는 바이트 오프셋 3. 역시 정의만 두고 쓰지 않는다 */
#define SLOT_SERR_INT_MASK	0x3

/* [한국어] shpc_isr 전방 선언 — 아래 int_poll_timeout() 이 정의보다 먼저 이 함수를 부르기 때문이다 */
static irqreturn_t shpc_isr(int irq, void *dev_id);
/* [한국어] start_int_poll_timer 전방 선언 — int_poll_timeout() 이 자기 자신을 다시 무장하려고 이 함수를 부르는데 정의가 뒤에 있기 때문이다 */
static void start_int_poll_timer(struct controller *ctrl, int sec);

/* [한국어] shpc_readb - SHPC Working Register Set 에서 1바이트를 읽는다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러. ctrl->creg 가 ioremap 이 돌려준 기준 주소다.
 * @reg: ctrl_offsets enum 의 바이트 오프셋(예: PROG_INTERFACE = 0x13).
 * @return: 읽은 8비트 값.
 * 
 * 레지스터 접근을 이 얇은 래퍼로 감싸 두면 호출부가 "creg + 오프셋" 계산을 반복하지
 * 않아도 되고, 나중에 접근 방식을 바꿔야 할 때 한 곳만 고치면 된다. readb() 는 MMIO
 * 읽기이므로 컴파일러 최적화로 사라지지 않고 순서도 보장된다.
 * 실행 컨텍스트: 프로세스/인터럽트 양쪽 모두. 잠들지 않으며 락도 잡지 않는다.
 * 에러 경로: 없다. 장치가 사라졌다면 0xff 가 읽힌다.
 * 
 * 호출 체인:
 *   shpc_get_cur_bus_speed / shpc_get_max_bus_speed / shpchp_get_prog_int /
 *   shpchp_set_bus_speed_mode → [shpc_readb] → readb() */
static inline u8 shpc_readb(struct controller *ctrl, int reg)
{
	/* [한국어] 기준 가상 주소에 오프셋을 더해 MMIO 1바이트를 읽는다. creg 는 void __iomem 포인터라 바이트 단위 산술이 그대로 성립한다 */
	return readb(ctrl->creg + reg);
}

/* [한국어] shpc_readw - SHPC Working Register Set 에서 2바이트를 읽는다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @reg: ctrl_offsets enum 의 바이트 오프셋(예: CMD_STATUS = 0x16, SEC_BUS_CONFIG = 0x10).
 * @return: 읽은 16비트 값(호스트 바이트 순서로 변환된 값).
 * 
 * Command Status 와 Secondary Bus Configuration 이 16비트 레지스터라 워드 접근이 필요하다.
 * 특히 is_ctrl_busy() 가 이 함수로 Controller Busy 비트를 폴링하므로 매 0.1초마다 불린다.
 * 실행 컨텍스트: 프로세스/인터럽트 양쪽 모두. 잠들지 않는다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   is_ctrl_busy / shpchp_check_cmd_status / shpc_get_cur_bus_speed
 *   → [shpc_readw] → readw() */
static inline u16 shpc_readw(struct controller *ctrl, int reg)
{
	/* [한국어] 기준 가상 주소에 오프셋을 더해 MMIO 2바이트를 읽는다 */
	return readw(ctrl->creg + reg);
}

/* [한국어] shpc_writew - SHPC Working Register Set 에 2바이트를 쓴다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @reg: ctrl_offsets enum 의 바이트 오프셋. 실제로는 CMD(0x14) 하나에만 쓰인다.
 * @val: 쓸 16비트 값. Command 레지스터의 경우 [12:8] 대상 슬롯 + [7:0] 명령 코드다.
 * @return: 없음.
 * 
 * 이 파일에서 유일하게 명령을 발행하는 통로다. 이 쓰기가 일어나는 순간 컨트롤러는
 * Controller Busy 를 세우고 명령 실행을 시작하므로, 호출 전에 반드시 Busy 가 0 임을
 * 확인해야 한다(그 확인은 shpc_write_cmd 가 한다).
 * 실행 컨텍스트: 프로세스 컨텍스트(cmd_lock 을 쥔 상태). 잠들지 않는다.
 * 에러 경로: 없다. 쓰기 성공 여부는 이후 Command Status 로만 알 수 있다.
 * 
 * 호출 체인:
 *   shpc_write_cmd() → [shpc_writew] → writew() */
static inline void shpc_writew(struct controller *ctrl, int reg, u16 val)
{
	/* [한국어] MMIO 2바이트 쓰기. writew 는 값이 먼저, 주소가 나중이라는 커널 관례를 따르므로 인자 순서가 뒤집혀 보인다 */
	writew(val, ctrl->creg + reg);
}

/* [한국어] shpc_readl - SHPC Working Register Set 에서 4바이트를 읽는다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @reg: ctrl_offsets enum 의 바이트 오프셋(예: INTR_LOC = 0x18, SLOT_REG(i) = 0x24+4i).
 * @return: 읽은 32비트 값.
 * 
 * Logical Slot Register 와 Controller SERR-INT, Interrupt Locator, Slot Available,
 * Slot Configuration 이 모두 32비트라 이 파일에서 가장 자주 쓰이는 접근자다.
 * 실행 컨텍스트: 프로세스/인터럽트 양쪽 모두. 잠들지 않는다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   shpc_isr / shpc_init / shpchp_release_ctlr / shpchp_get_ 계열 /
 *   shpc_get_max_bus_speed → [shpc_readl] → readl() */
static inline u32 shpc_readl(struct controller *ctrl, int reg)
{
	/* [한국어] 기준 가상 주소에 오프셋을 더해 MMIO 4바이트를 읽는다 */
	return readl(ctrl->creg + reg);
}

/* [한국어] shpc_writel - SHPC Working Register Set 에 4바이트를 쓴다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @reg: ctrl_offsets enum 의 바이트 오프셋(SERR_INTR_ENABLE 또는 SLOT_REG(i)).
 * @val: 쓸 32비트 값. 마스크 비트와 RW1C 상태 비트가 한 워드에 섞여 있으므로,
 *       호출부는 반드시 읽은 값을 고쳐 되쓰는 방식(read-modify-write)을 쓴다.
 * @return: 없음.
 * 
 * 이 함수로 쓰는 두 레지스터 모두 RW1C 비트를 품고 있다는 점이 중요하다. 읽은 값을
 * 그대로 되쓰면 1 로 읽힌 상태 비트가 지워지므로, 마스크 갱신과 이벤트 클리어가 한 번의
 * 쓰기로 동시에 일어난다. 그래서 이 파일에는 별도의 "이벤트 지우기" 코드가 없다.
 * 실행 컨텍스트: 프로세스/인터럽트 양쪽 모두. 잠들지 않는다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   shpc_isr / shpc_init / shpchp_release_ctlr → [shpc_writel] → writel() */
static inline void shpc_writel(struct controller *ctrl, int reg, u32 val)
{
	/* [한국어] MMIO 4바이트 쓰기. RsvdZ 비트를 떨어뜨리는 책임은 호출부에 있다 */
	writel(val, ctrl->creg + reg);
}

/* [한국어] shpc_indirect_read - MMIO 매핑 전에 config 공간의 간접 창으로 Working Register Set 을 읽는다
 * 
 * @ctrl: cap_offset 은 채워져 있고 creg 는 아직 없는 초기화 중의 컨트롤러.
 * @index: 읽을 dword 인덱스. 0 = Base Offset, 3 = Slot Configuration 이며,
 *         0~8 이 고정부 9 dword, 9 이상이 슬롯별 레지스터에 해당한다.
 * @value: 읽은 32비트 값을 담을 곳.
 * @return: 0 이면 성공. config 접근 자체가 실패하면 PCI 오류 코드가 그대로 올라온다.
 * 
 * 닭과 달걀 문제를 푸는 함수다. Working Register Set 이 BAR0 안 어디에 있는지는
 * Base Offset 레지스터를 읽어야 알 수 있는데, 그 레지스터 자체가 Working Register Set
 * 안에 있다. SHPC 는 이를 위해 config 공간의 capability 안에 DWORD Select(+2)와
 * DWORD Data(+4) 두 레지스터로 된 간접 창을 두었다. 인덱스를 Select 에 쓰고 Data 를
 * 읽으면 해당 dword 가 나온다.
 * 
 * 동작 단계: (1) capability + 2 에 인덱스를 1바이트로 쓴다. (2) capability + 4 에서
 * 4바이트를 읽는다. 두 접근 사이에 다른 코드가 끼어들면 인덱스가 어긋나지만, 이 함수는
 * shpc_init() 안에서만 순차적으로 불리므로 별도 락을 두지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로)에서만. 재진입은 고려하지 않는다.
 * 에러 경로: 쓰기가 실패하면 읽기를 시도하지 않고 즉시 오류를 돌려준다 — 인덱스가
 * 설정되지 않은 상태에서 읽으면 엉뚱한 값을 얻기 때문이다.
 * 
 * 호출 체인:
 *   shpc_init() → [shpc_indirect_read]
 *   → pci_write_config_byte() / pci_read_config_dword() */
static inline int shpc_indirect_read(struct controller *ctrl, int index,
				     u32 *value)
{
	/* [한국어] config 접근 결과 코드 */
	int rc;
	/* [한국어] SHPC capability 의 config 공간 오프셋 — 간접 창의 기준점이다 */
	u32 cap_offset = ctrl->cap_offset;
	/* [한국어] SHPC 를 품은 브리지의 pci_dev */
	struct pci_dev *pdev = ctrl->pci_dev;

	/* [한국어] capability + 2 의 DWORD Select 레지스터에 인덱스를 쓴다. 이 쓰기가 다음 읽기의 대상을 결정한다 */
	rc = pci_write_config_byte(pdev, cap_offset + DWORD_SELECT, index);
	/* [한국어] config 쓰기가 실패했다면 인덱스가 설정되지 않았다 */
	if (rc)
		/* [한국어] 읽기를 시도하지 않고 오류를 그대로 올린다 */
		return rc;
	/* [한국어] capability + 4 의 DWORD Data 레지스터에서 선택된 dword 를 읽어 value 에 채우고, 그 결과 코드를 그대로 반환한다 */
	return pci_read_config_dword(pdev, cap_offset + DWORD_DATA, value);
}

/*
 * This is the interrupt polling timeout function.
 */
/* [한국어] int_poll_timeout - 폴링 모드에서 주기적으로 인터럽트 핸들러를 대신 부르는 타이머 콜백
 * 
 * @t: 만료된 타이머. struct controller 안에 박혀 있는 poll_timer 이므로 여기서
 *     컨트롤러 서술자를 역산해 낸다.
 * @return: 없음(타이머 콜백 규약).
 * 
 * MSI 도 INTx 도 신뢰할 수 없는 하드웨어나 인터럽트 관련 버그를 디버깅할 때를 위해
 * shpchp 는 shpchp_poll_mode 모듈 파라미터로 폴링 동작을 제공한다. 이 모드에서는
 * 인터럽트를 아예 등록하지 않고, 이 콜백이 주기적으로 shpc_isr() 을 직접 불러
 * Interrupt Locator 를 훑는다. 그래서 폴링 모드에서는 명령 완료 대기도 대기열이 아니라
 * 직접 Busy 비트 폴링으로 처리된다(shpc_wait_cmd 의 분기).
 * 
 * 동작 단계: (1) shpc_isr() 을 irq=0 으로 호출한다. (2) 폴링 주기가 설정돼 있지 않으면
 * 2초를 기본값으로 채운다. (3) 타이머를 다시 무장해 스스로를 반복시킨다.
 * 실행 컨텍스트: 타이머 소프트IRQ 컨텍스트 — 잠들 수 없다. 그래서 이 경로에서 불리는
 * shpc_isr() 과 그 아래 shpchp_handle_ 계열도 잠들지 않아야 한다.
 * 에러 경로: 없다. 실패를 보고할 상대가 없다.
 * 
 * 호출 체인:
 *   커널 타이머 코어 → [int_poll_timeout] → shpc_isr() → start_int_poll_timer() */
static void int_poll_timeout(struct timer_list *t)
{
	/* [한국어] 타이머 구조체 주소에서 그것을 품은 struct controller 를 역산한다. container_of 계열 매크로이며, poll_timer 필드가 controller 안에 박혀 있기에 가능하다 */
	struct controller *ctrl = timer_container_of(ctrl, t, poll_timer);

	/* Poll for interrupt events.  regs == NULL => polling */
	/* [한국어] 인터럽트 대신 직접 핸들러를 부른다. irq 인자에 0 을 넘기는 것이 폴링 경로라는 표시이며, shpc_isr 은 이 값을 쓰지 않는다 */
	shpc_isr(0, ctrl);

	/* [한국어] 모듈 파라미터가 설정되지 않았다면(0) 주기를 정할 수 없다 */
	if (!shpchp_poll_time)
		/* [한국어] 기본 폴링 주기를 2초로 채워 둔다. 이 대입은 전역 변수를 바꾸므로 이후 호출에도 계속 적용된다 */
		shpchp_poll_time = 2; /* default polling interval is 2 sec */

	/* [한국어] 다음 폴링을 예약해 스스로를 반복시킨다. 이 재무장이 없으면 폴링이 한 번만 돌고 끝난다 */
	start_int_poll_timer(ctrl, shpchp_poll_time);
}

/*
 * This function starts the interrupt polling timer.
 */
/* [한국어] start_int_poll_timer - 폴링 타이머를 sec 초 뒤로 무장한다
 * 
 * @ctrl: poll_timer 가 timer_setup() 으로 초기화된 컨트롤러.
 * @sec: 몇 초 뒤에 깨울지. 1~60 을 벗어나면 2초로 강제한다.
 * @return: 없음.
 * 
 * 모듈 파라미터 shpchp_poll_time 은 사용자가 sysfs 로 아무 값이나 쓸 수 있으므로
 * 그대로 믿으면 안 된다. 0 이나 음수면 타이머가 즉시 만료돼 CPU 를 태우고, 지나치게
 * 크면 이벤트를 몇 분씩 놓친다. 그래서 여기서 한 번 걸러 준다.
 * 
 * 동작 단계: (1) 범위를 벗어나면 2초로 고정한다. (2) 만료 시각을 현재 jiffies 기준으로
 * 계산해 넣는다. (3) 타이머를 커널 타이머 휠에 등록한다.
 * 실행 컨텍스트: shpc_init() 에서는 프로세스 컨텍스트, int_poll_timeout() 에서는 타이머
 * 소프트IRQ 컨텍스트. 둘 다 잠들지 않는 경로다.
 * 에러 경로: 없다. add_timer() 는 실패하지 않는다(이미 무장된 타이머를 다시 넣는 것은
 * 경고 대상이지만, 이 코드 흐름에서는 만료 후에만 다시 부르므로 문제되지 않는다).
 * 
 * 호출 체인:
 *   shpc_init() 또는 int_poll_timeout() → [start_int_poll_timer] → add_timer() */
static void start_int_poll_timer(struct controller *ctrl, int sec)
{
	/* Clamp to sane value */
	/* [한국어] 0 이하면 타이머가 즉시(혹은 과거 시각으로) 만료돼 폭주하고, 60초를 넘으면 핫플러그 반응이 지나치게 느려진다 — 양쪽 다 막는다 */
	if ((sec <= 0) || (sec > 60))
		/* [한국어] 둘 중 하나라도 걸리면 안전한 기본값 2초로 되돌린다 */
		sec = 2;

	/* [한국어] 만료 시각 = 현재 jiffies + sec 초. HZ 는 1초당 jiffies 수라 곱셈으로 초를 tick 으로 바꾼다 */
	ctrl->poll_timer.expires = jiffies + sec * HZ;
	/* [한국어] 타이머를 커널 타이머 휠에 등록한다. 만료되면 int_poll_timeout() 이 불린다 */
	add_timer(&ctrl->poll_timer);
}

/* [한국어] is_ctrl_busy - 컨트롤러가 지금 명령을 실행 중인지 한 번 확인한다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @return: 0 이 아니면 바쁨(Controller Busy 가 서 있음), 0 이면 명령을 받을 수 있다.
 * 
 * SHPC 명령 큐의 상태를 알려 주는 유일한 신호다. Command Status 레지스터(오프셋 0x16)의
 * bit0 이 Controller Busy 이며, CMD 레지스터에 명령을 쓰는 순간 하드웨어가 이 비트를
 * 세우고 명령이 끝나면 내린다. 명령을 쓰기 전에도, 완료를 기다릴 때도 이 비트를 본다.
 * 특히 wait_event_interruptible_timeout() 의 조건식으로 쓰이므로 잠들지 않아야 한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트와 대기열 조건 평가 컨텍스트. MMIO 읽기 하나뿐이라
 * 어디서 불려도 안전하다. 락은 잡지 않는다 — 상위 shpc_write_cmd 가 cmd_lock 을 쥔다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   shpc_poll_ctrl_busy / shpc_wait_cmd → [is_ctrl_busy] → shpc_readw(CMD_STATUS) */
static inline int is_ctrl_busy(struct controller *ctrl)
{
	/* [한국어] Command Status 레지스터를 워드로 읽는다. 상위 비트에는 오류 코드가 함께 들어 있다 */
	u16 cmd_status = shpc_readw(ctrl, CMD_STATUS);
	/* [한국어] bit0(Controller Busy)만 남긴다. 오류 코드 필드는 여기서 관심 밖이며 shpchp_check_cmd_status() 가 따로 본다 */
	return cmd_status & 0x1;
}

/*
 * Returns 1 if SHPC finishes executing a command within 1 sec,
 * otherwise returns 0.
 */
/* [한국어] shpc_poll_ctrl_busy - Controller Busy 가 내려갈 때까지 최대 1초 폴링한다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러.
 * @return: 1 이면 1초 안에 컨트롤러가 한가해졌다, 0 이면 1초가 지나도 여전히 바쁘다.
 * 
 * 두 곳에서 쓰인다. 첫째는 shpc_write_cmd() 가 명령을 쓰기 직전에 "앞선 명령이 정말
 * 끝났는지" 확인할 때이고, 둘째는 폴링 모드에서 명령 완료를 기다릴 때다. 인터럽트를
 * 쓸 수 없거나 믿을 수 없는 상황에서도 명령 큐 프로토콜이 성립하게 해 주는 안전망이다.
 * 1초라는 한도는 SHPC 명령이 그보다 오래 걸리지 않는다는 전제에서 나온 값이며, 아래
 * shpc_wait_cmd() 의 인터럽트 모드 타임아웃 1000ms 와 의도적으로 같게 맞춰 두었다.
 * 
 * 동작 단계: (1) 이미 한가하면 즉시 1 을 돌려준다(빠른 경로). (2) 아니면 100ms 쉬고
 * 다시 보기를 10번 반복한다. (3) 끝까지 바쁘면 0.
 * 실행 컨텍스트: 프로세스 컨텍스트 전용 — msleep() 이 잠들기 때문이다. 인터럽트
 * 컨텍스트에서 부르면 안 된다.
 * 에러 경로: 타임아웃을 0 으로만 알리며, -EBUSY 로 바꾸는 것은 호출자의 몫이다.
 * 
 * 호출 체인:
 *   shpc_write_cmd() 또는 shpc_wait_cmd()(폴링 모드) → [shpc_poll_ctrl_busy]
 *   → is_ctrl_busy() + msleep() */
static inline int shpc_poll_ctrl_busy(struct controller *ctrl)
{
	/* [한국어] 재시도 횟수 카운터 */
	int i;

	/* [한국어] 빠른 경로 — 대부분의 호출에서 컨트롤러는 이미 한가하다 */
	if (!is_ctrl_busy(ctrl))
		/* [한국어] 잠들 필요 없이 즉시 성공을 알린다 */
		return 1;

	/* Check every 0.1 sec for a total of 1 sec */
	/* [한국어] 0.1초 간격으로 10번 = 총 1초 동안 관찰한다 */
	for (i = 0; i < 10; i++) {
		/* [한국어] 100ms 잠든다. 이 대기 동안 CPU 를 다른 일에 내주므로, 바쁜 대기(busy loop)보다 낫다 */
		msleep(100);
		/* [한국어] 다시 Busy 비트를 확인한다 */
		if (!is_ctrl_busy(ctrl))
			/* [한국어] 내려갔으면 성공을 알린다 */
			return 1;
	}

	/* [한국어] 1초를 다 쓰고도 바쁘면 실패다. 호출자는 이를 -EBUSY 나 -EIO 로 바꾼다 */
	return 0;
}

/* [한국어] shpc_wait_cmd - 발행한 명령이 끝나기를 기다린다(인터럽트 모드와 폴링 모드 두 갈래)
 * 
 * @ctrl: 명령을 발행한 컨트롤러. cmd_lock 을 쥔 상태로 불린다.
 * @return: 0 이면 명령이 끝났다. -EIO 는 1초 타임아웃, -EINTR 은 대기 중 시그널을 받아
 *          중단됐다는 뜻이다.
 * 
 * SHPC 명령 큐의 대기 단계를 한곳에 모은 함수다. 인터럽트 모드에서는 ctrl->queue
 * 대기열에서 자다가 shpc_isr() 이 Command Completion 을 보고 깨워 준다. 폴링 모드에서는
 * 깨워 줄 인터럽트가 없으므로 직접 Busy 비트를 훑는다. 두 경로 모두 1초를 한도로 삼는다.
 * 
 * 주의할 점은 반환값 rc 의 의미가 두 경로에서 다르다는 것이다. 폴링 경로의 rc 는 1(성공)
 * 또는 0(타임아웃)이고, wait_event_interruptible_timeout() 의 rc 는 남은 jiffies(양수,
 * 성공), 0(타임아웃), 또는 -ERESTARTSYS(시그널)다. 아래 판정이 rc==0 과 rc<0 을 나눠 보는
 * 이유가 여기에 있다. rc 가 0 이어도 Busy 가 이미 내려갔다면(경계에서 명령이 막 끝난
 * 경우) 성공으로 처리하려고 is_ctrl_busy() 를 한 번 더 확인한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(잠든다). cmd_lock 을 쥔 채 잠들므로, 이 함수가
 * 느려지면 다른 슬롯의 명령도 모두 밀린다.
 * 에러 경로: 타임아웃과 시그널을 서로 다른 errno 로 구분해 올려 준다. 시그널 중단은
 * 사용자가 Ctrl-C 로 sysfs 쓰기를 끊은 경우에 해당한다.
 * 
 * 호출 체인:
 *   shpc_write_cmd() → [shpc_wait_cmd]
 *   → 인터럽트 모드: wait_event_interruptible_timeout(ctrl->queue) — shpc_isr 이 깨움
 *   → 폴링 모드: shpc_poll_ctrl_busy() */
static inline int shpc_wait_cmd(struct controller *ctrl)
{
	/* [한국어] 기본은 성공(0) */
	int retval = 0;
	/* [한국어] 1000ms 를 jiffies 로 환산한 대기 한도. 폴링 경로의 0.1초 x 10회와 같은 1초다 */
	unsigned long timeout = msecs_to_jiffies(1000);
	/* [한국어] 두 대기 방식의 결과를 함께 받는 변수. 의미가 경로마다 다르다는 점에 주의 */
	int rc;

	/* [한국어] 폴링 모드에서는 깨워 줄 인터럽트가 없다 */
	if (shpchp_poll_mode)
		/* [한국어] 직접 Busy 비트를 1초간 훑는다. 성공이면 1, 타임아웃이면 0 이 돌아온다 */
		rc = shpc_poll_ctrl_busy(ctrl);
	else
		/* [한국어] 인터럽트 모드에서는 대기열에서 잠들었다가 shpc_isr 의 wake_up_interruptible 로 깨어난다. 조건식 is_ctrl_busy 가 거짓이 되면 깨어나며, 시그널로도 깨어날 수 있다 */
		rc = wait_event_interruptible_timeout(ctrl->queue,
						!is_ctrl_busy(ctrl), timeout);
	/* [한국어] rc 가 0 이면 두 경로 모두 타임아웃을 뜻한다. 다만 경계에서 명령이 막 끝났을 수 있으므로 Busy 비트를 한 번 더 확인해, 실제로 아직 바쁠 때만 실패로 판정한다 */
	if (!rc && is_ctrl_busy(ctrl)) {
		/* [한국어] 하드웨어가 1초 안에 응답하지 않았으므로 입출력 오류로 본다 */
		retval = -EIO;
		/* [한국어] 타임아웃 사실을 로그로 남긴다 — 하드웨어 문제를 추적하는 첫 단서다 */
		ctrl_err(ctrl, "Command not completed in 1000 msec\n");
	/* [한국어] rc 가 음수인 경우는 인터럽트 모드에서만 나오며, 대기 중 시그널을 받았다는 뜻이다(-ERESTARTSYS) */
	} else if (rc < 0) {
		/* [한국어] 사용자 개입에 의한 중단이므로 -EINTR 로 구분해 올린다 */
		retval = -EINTR;
		/* [한국어] 오류가 아니라 정보 수준으로 로그를 남긴다 — 하드웨어 문제가 아니기 때문이다 */
		ctrl_info(ctrl, "Command was interrupted by a signal\n");
	}

	/* [한국어] 성공(0) 또는 -EIO/-EINTR 을 shpc_write_cmd() 에 돌려준다 */
	return retval;
}

/* [한국어] shpc_write_cmd - SHPC 명령 큐 프로토콜의 심장. 명령을 하나 발행하고 완료까지 기다린다
 * 
 * @slot: 명령 대상 슬롯(ctrl 을 얻는 통로이기도 하다).
 * @t_slot: 0 기반 논리 슬롯 번호. 버스 세그먼트 전체를 대상으로 하는 속도 변경 명령은
 *          여기에 0 을 넘긴다. 함수 안에서 1 을 더해 레지스터 인코딩(1 기반)으로 바꾼다.
 * @cmd: SHPC 명령 코드 한 바이트(Slot Operation 0x00~0x3F, Speed/Mode A 0x40~0x47,
 *       Power-Only All 0x48, Enable All 0x49, Speed/Mode B 0x50~0x5F).
 * @return: 0 이면 명령이 성공적으로 완료됐다. -EBUSY 는 명령을 내기도 전에 컨트롤러가
 *       1초 넘게 바빴다는 뜻, -EIO 는 완료를 1초 안에 못 봤거나 컨트롤러가 명령 오류를
 *       보고했다는 뜻, -EINTR 은 대기 중 시그널을 받았다는 뜻이다.
 * 
 * 이 함수가 SHPC 와 pciehp 를 가르는 지점이다. pciehp 는 PCIe capability 의 Slot
 * Control 레지스터에 config 쓰기를 한 번 하면 끝이지만, SHPC 는 Command 레지스터
 * (오프셋 0x14)에 [12:8] 대상 슬롯 + [7:0] 명령 코드를 실어 쓴 뒤, Command Status
 * 레지스터(오프셋 0x16)의 Controller Busy(bit0)가 내려가거나 Command Completion
 * 인터럽트가 올 때까지 기다려야 한다. 컨트롤러가 한 번에 한 명령만 받으므로
 * cmd_lock 뮤텍스로 함수 전체를 직렬화한다 — 이 락이 없으면 두 슬롯의 LED 명령이
 * 서로의 완료 대기를 훔쳐 간다.
 * 
 * 동작 단계: (1) cmd_lock 획득. (2) Controller Busy 가 이미 서 있으면 최대 1초 폴링해
 * 가라앉기를 기다리고, 그래도 바쁘면 -EBUSY. (3) 슬롯 번호를 1 기반으로 바꿔 명령
 * 워드를 조립하고 CMD 레지스터에 쓴다. (4) shpc_wait_cmd() 로 완료를 기다린다.
 * (5) Command Status 의 오류 코드를 확인한다. (6) 락 해제.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. msleep 과 wait_event_interruptible_timeout
 * 을 쓰므로 인터럽트 컨텍스트에서 부르면 안 된다. 여러 스레드가 동시에 불러도 cmd_lock
 * 으로 한 줄로 세워진다.
 * 에러 경로: 어느 단계에서 실패하든 out 라벨로 내려가 반드시 cmd_lock 을 푼다.
 * 
 * 호출 체인:
 *   shpchp_set_attention_status / shpchp_green_led_* / shpchp_power_on_slot /
 *   shpchp_slot_enable / shpchp_slot_disable / shpchp_set_bus_speed_mode
 *   → [shpc_write_cmd] → shpc_poll_ctrl_busy → shpc_writew(CMD)
 *   → shpc_wait_cmd (shpc_isr 가 깨움) → shpchp_check_cmd_status */
static int shpc_write_cmd(struct slot *slot, u8 t_slot, u8 cmd)
{
	/* [한국어] 슬롯이 속한 컨트롤러 — 레지스터 접근과 로그의 기준이다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] shpchp_check_cmd_status() 가 돌려주는 명령 오류 코드를 담는다 */
	u16 cmd_status;
	/* [한국어] 반환값. 기본은 성공(0)이다 */
	int retval = 0;
	/* [한국어] CMD 레지스터에 실을 16비트 명령 워드 */
	u16 temp_word;

	/* [한국어] 명령 큐 직렬화 시작. SHPC 컨트롤러는 한 번에 한 명령만 처리하므로 이 락이 곧 하드웨어 자원의 소유권이다 */
	mutex_lock(&slot->ctrl->cmd_lock);

	/* [한국어] 명령을 쓰기 전에 Controller Busy(CMD_STATUS bit0)가 내려가 있어야 한다. 이 헬퍼가 최대 1초 동안 0.1초 간격으로 폴링한다 */
	if (!shpc_poll_ctrl_busy(ctrl)) {
		/* After 1 sec and the controller is still busy */
		/* [한국어] 1초를 기다려도 바쁘면 하드웨어가 멈췄거나 앞선 명령이 끝나지 않은 것이다 */
		ctrl_err(ctrl, "Controller is still busy after 1 sec\n");
		/* [한국어] 호출자가 재시도할 수 있도록 -EBUSY 를 준다 */
		retval = -EBUSY;
		/* [한국어] 락을 반드시 풀어야 하므로 반환 대신 out 으로 간다 */
		goto out;
	}

	/* [한국어] 레지스터의 Target Slot 필드는 1 기반이다(1 = 첫 슬롯). 드라이버 내부의 0 기반 hp_slot 을 여기서 한 칸 올려 인코딩을 맞춘다 */
	++t_slot;
	/* [한국어] 명령 워드 조립: [12:8] 에 대상 슬롯, [7:0] 에 명령 코드. cmd 를 0xFF 로 마스크해 상위 비트가 슬롯 필드를 침범하지 않게 한다 */
	temp_word =  (t_slot << 8) | (cmd & 0xFF);
	/* [한국어] 어떤 슬롯에 어떤 명령을 보내는지 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "%s: t_slot %x cmd %x\n", __func__, t_slot, cmd);

	/* To make sure the Controller Busy bit is 0 before we send out the
	 * command.
	 */
	/* [한국어] Command 레지스터(오프셋 0x14)에 명령 워드를 쓴다. 이 쓰기 자체가 명령 발행이며, 하드웨어는 즉시 Controller Busy 를 세운다 */
	shpc_writew(ctrl, CMD, temp_word);

	/*
	 * Wait for command completion.
	 */
	/* [한국어] 완료 대기. 인터럽트 모드면 ctrl->queue 에서 자다가 shpc_isr() 이 깨워 주고, 폴링 모드면 0.1초 간격으로 Controller Busy 를 직접 본다 */
	retval = shpc_wait_cmd(slot->ctrl);
	/* [한국어] 타임아웃(-EIO) 이나 시그널(-EINTR) 이면 오류 코드 확인 없이 빠져나간다 */
	if (retval)
		goto out;

	/* [한국어] Command Status 레지스터의 오류 코드 필드를 해석한다. 명령이 끝났어도 컨트롤러가 거절했을 수 있다 */
	cmd_status = shpchp_check_cmd_status(slot->ctrl);
	/* [한국어] 0 이 아니면 컨트롤러가 명령을 수행하지 못했다는 뜻이다 */
	if (cmd_status) {
		/* [한국어] 어떤 명령이 어떤 코드로 거절됐는지 남긴다 */
		ctrl_err(ctrl, "Failed to issued command 0x%x (error code = %d)\n",
			 cmd, cmd_status);
		/* [한국어] 하드웨어가 거절한 것이므로 입출력 오류로 보고한다 */
		retval = -EIO;
	}
 out:
	/* [한국어] 성공 경로든 실패 경로든 반드시 여기를 지나 락을 푼다 — goto out 을 쓰는 이유가 이것이다 */
	mutex_unlock(&slot->ctrl->cmd_lock);
	/* [한국어] 최종 결과를 호출자에게 돌려준다 */
	return retval;
}

/* [한국어] shpchp_check_cmd_status - Command Status 레지스터의 오류 코드를 드라이버 상수로 번역한다
 * 
 * @ctrl: 대상 컨트롤러.
 * @return: 0 이면 오류 없음. SWITCH_OPEN(0x1)은 MRL(래치)이 열려 있어 명령을 거부했다는
 *       뜻, INVALID_CMD(0x2)는 컨트롤러가 모르는 명령 코드, INVALID_SPEED_MODE(0x4)는
 *       이 세그먼트가 낼 수 없는 속도/모드 요구다. 정의되지 않은 코드는 읽은 값을 그대로
 *       돌려준다.
 * 
 * SHPC 는 명령을 거절할 때 인터럽트가 아니라 Command Status 레지스터(오프셋 0x16)의
 * [3:1] 필드에 이유를 남긴다. 그래서 명령 완료를 기다린 뒤에도 반드시 이 필드를 봐야
 * "완료됐지만 거절됐다"를 구분할 수 있다. 이 함수는 그 3비트를 읽어 shpchp.h 가 정의한
 * 상수로 바꾸고 사람이 읽을 로그까지 남긴다.
 * 
 * 실행 컨텍스트: shpc_write_cmd() 안, cmd_lock 을 쥔 프로세스 컨텍스트. shpchp.h 에
 * 선언이 있어 외부에도 노출돼 있지만 이 트리에서 실제 호출자는 shpc_write_cmd() 뿐이다.
 * 에러 경로: 자체 실패는 없다. 읽은 값을 해석해 돌려주는 순수 번역 함수다.
 * 
 * 호출 체인:
 *   shpc_write_cmd() → [shpchp_check_cmd_status] → shpc_readw(CMD_STATUS) */
int shpchp_check_cmd_status(struct controller *ctrl)
{
	/* [한국어] 기본은 오류 없음 */
	int retval = 0;
	/* [한국어] Command Status 레지스터를 읽고 하위 4비트만 남긴다 — bit0 은 Controller Busy, [3:1] 이 오류 코드 필드다 */
	u16 cmd_status = shpc_readw(ctrl, CMD_STATUS) & 0x000F;

	/* [한국어] 1비트 오른쪽으로 밀어 Controller Busy 를 떨어뜨리고 [3:1] 오류 코드만 값으로 만든다 */
	switch (cmd_status >> 1) {
	/* [한국어] 코드 0 = 명령이 정상적으로 수행됐다 */
	case 0:
		/* [한국어] 오류 없음 */
		retval = 0;
		break;
	/* [한국어] 코드 1 = MRL(래치)이 열려 있어 슬롯 조작을 거부했다 */
	case 1:
		/* [한국어] shpchp.h 의 SWITCH_OPEN 으로 번역 */
		retval = SWITCH_OPEN;
		/* [한국어] 사람이 볼 수 있게 래치가 열렸다고 알린다 */
		ctrl_err(ctrl, "Switch opened!\n");
		break;
	/* [한국어] 코드 2 = 컨트롤러가 이해하지 못하는 명령 코드였다 */
	case 2:
		/* [한국어] INVALID_CMD 로 번역 */
		retval = INVALID_CMD;
		/* [한국어] 잘못된 명령이었음을 알린다 */
		ctrl_err(ctrl, "Invalid HPC command!\n");
		break;
	/* [한국어] 코드 4 = 요구한 버스 속도/모드를 이 세그먼트가 낼 수 없다 */
	case 4:
		/* [한국어] INVALID_SPEED_MODE 로 번역 */
		retval = INVALID_SPEED_MODE;
		/* [한국어] 속도/모드가 잘못됐음을 알린다 */
		ctrl_err(ctrl, "Invalid bus speed/mode!\n");
		break;
	/* [한국어] 규격에 정의되지 않은 코드(3, 5, 6, 7) */
	default:
		/* [한국어] 해석하지 않고 읽은 값을 그대로 올려 보낸다 — 호출자는 0 이 아니라는 사실만으로 실패로 처리한다 */
		retval = cmd_status;
	}

	/* [한국어] 번역 결과를 돌려준다 */
	return retval;
}


/* [한국어] shpchp_get_attention_status - 슬롯의 Attention(황색) LED 현재 상태를 읽는다
 * 
 * @slot: 대상 슬롯.
 * @status: 결과를 담을 곳. 0 = 꺼짐, 1 = 켜짐, 2 = 깜빡임, 0xFF = 규격상 예약값.
 * @return: 항상 0. MMIO 읽기 하나뿐이라 실패할 경로가 없다.
 * 
 * LED 상태는 명령을 내지 않고 Logical Slot Register 를 직접 읽어 알 수 있다 — SHPC 에서
 * 읽기는 즉시 가능하고 쓰기만 명령 큐를 거친다는 비대칭이 여기서 드러난다. 레지스터의
 * Attention Indicator 필드[5:4]는 1=ON, 2=BLINK, 3=OFF 인코딩이라 sysfs 가 쓰는
 * 0/1/2 표현과 순서가 다르므로 이 함수가 번역해 준다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기나 워크큐). 락을 잡지 않으며 잠들지 않는다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   get_attention_status() (shpchp_core.c, sysfs) / update_slot_info() (shpchp_ctrl.c)
 *   → [shpchp_get_attention_status] → shpc_readl(SLOT_REG) */
int shpchp_get_attention_status(struct slot *slot, u8 *status)
{
	/* [한국어] 슬롯이 속한 컨트롤러 — creg 기준 주소를 얻기 위해서다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register(오프셋 0x24 + 4*hp_slot)를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));
	/* [한국어] Attention Indicator 필드[5:4]만 뽑아 오른쪽 정렬한다 */
	u8 state = (slot_reg & ATN_LED_STATE_MASK) >> ATN_LED_STATE_SHIFT;

	/* [한국어] 레지스터 인코딩을 sysfs 표현으로 번역한다 */
	switch (state) {
	/* [한국어] 인코딩 1 = 켜짐 */
	case ATN_LED_STATE_ON:
		/* [한국어] sysfs 표현으로는 1 */
		*status = 1;	/* On */
		break;
	/* [한국어] 인코딩 2 = 깜빡임 */
	case ATN_LED_STATE_BLINK:
		/* [한국어] sysfs 표현으로는 2 */
		*status = 2;	/* Blink */
		break;
	/* [한국어] 인코딩 3 = 꺼짐 */
	case ATN_LED_STATE_OFF:
		/* [한국어] sysfs 표현으로는 0 — 인코딩과 표현의 순서가 다른 지점이다 */
		*status = 0;	/* Off */
		break;
	/* [한국어] 인코딩 0 은 규격상 예약값이라 정의된 상태가 없다 */
	default:
		/* [한국어] 알 수 없음을 뜻하는 0xFF 를 준다 */
		*status = 0xFF;	/* Reserved */
		break;
	}

	/* [한국어] 읽기만 하므로 항상 성공이다 */
	return 0;
}

/* [한국어] shpchp_get_power_status - 슬롯의 전원/연결 상태를 읽는다
 * 
 * @slot: 대상 슬롯.
 * @status: 결과를 담을 곳. 0 = 꺼짐(Disabled), 1 = 켜지고 버스에 연결됨(Enabled),
 *          2 = 전원만 들어감(Power Only), 0xFF = 예약값.
 * @return: 항상 0.
 * 
 * SHPC 슬롯은 Disabled / Power Only / Enabled 3단계를 가지며 Logical Slot Register 의
 * Slot State 필드[1:0]에 그대로 실린다. 인코딩(1=Power Only, 2=Enabled, 3=Disabled)과
 * sysfs 표현(2/1/0)이 서로 다르므로 여기서 번역한다. shpchp_ctrl.c 의 상태 머신은 이
 * 함수의 결과가 0 인지 아닌지로 "지금 켜야 하는가 꺼야 하는가"를 판단한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음, 잠들지 않음.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   get_power_status() (shpchp_core.c, sysfs) / shpchp_enable_slot /
 *   shpchp_disable_slot / handle_button_press_event / update_slot_info (shpchp_ctrl.c)
 *   → [shpchp_get_power_status] → shpc_readl(SLOT_REG) */
int shpchp_get_power_status(struct slot *slot, u8 *status)
{
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register 를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));
	/* [한국어] Slot State 필드[1:0]만 뽑는다. SLOT_STATE_SHIFT 가 0 이라 시프트는 실질적으로 없지만 필드 추출 형태를 통일해 둔 것이다 */
	u8 state = (slot_reg & SLOT_STATE_MASK) >> SLOT_STATE_SHIFT;

	/* [한국어] 레지스터 인코딩을 sysfs 표현으로 번역한다 */
	switch (state) {
	/* [한국어] 인코딩 1 = 전원만 들어간 상태(버스 미연결) */
	case SLOT_STATE_PWRONLY:
		/* [한국어] sysfs 표현으로는 2 */
		*status = 2;	/* Powered only */
		break;
	/* [한국어] 인코딩 2 = 전원이 들어가고 버스에도 연결된 상태 */
	case SLOT_STATE_ENABLED:
		/* [한국어] sysfs 표현으로는 1 */
		*status = 1;	/* Enabled */
		break;
	/* [한국어] 인코딩 3 = 전원이 꺼진 상태 */
	case SLOT_STATE_DISABLED:
		/* [한국어] sysfs 표현으로는 0 */
		*status = 0;	/* Disabled */
		break;
	/* [한국어] 인코딩 0 은 예약값이다 */
	default:
		/* [한국어] 알 수 없음을 뜻하는 0xFF */
		*status = 0xFF;	/* Reserved */
		break;
	}

	/* [한국어] 읽기만 하므로 항상 성공이다 */
	return 0;
}


/* [한국어] shpchp_get_latch_status - 슬롯 래치(MRL, Manually-operated Retention Latch) 개폐 상태를 읽는다
 * 
 * @slot: 대상 슬롯.
 * @status: 결과를 담을 곳. 0 = 닫힘, 1 = 열림.
 * @return: 항상 0.
 * 
 * MRL 은 카드를 물리적으로 고정하는 레버다. 레버가 열려 있으면 사람이 카드를 뽑으려는
 * 중이므로 SHPC 는 슬롯 조작 명령을 거절하고(Command Status 의 MRL Open 오류), 드라이버
 * 상태 머신도 활성화를 거부한다. Logical Slot Register 의 MRL Sensor 비트(bit8) 하나를
 * 논리값으로 정규화해 돌려주는 것이 이 함수의 전부다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음, 잠들지 않음.
 * 에러 경로: 없다. MRL 센서가 없는 슬롯에서는 이 비트가 항상 0 으로 읽히므로 "닫힘"으로
 * 보이며, 센서 유무는 Slot Configuration 의 MRL Sensor Implemented 비트로 알 수 있지만
 * 이 함수는 그것을 구분하지 않는다.
 * 
 * 호출 체인:
 *   get_latch_status() (shpchp_core.c, sysfs) / shpchp_handle_switch_change /
 *   shpchp_enable_slot / shpchp_disable_slot / update_slot_info (shpchp_ctrl.c)
 *   → [shpchp_get_latch_status] → shpc_readl(SLOT_REG) */
int shpchp_get_latch_status(struct slot *slot, u8 *status)
{
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register 를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));

	/* [한국어] MRL Sensor 비트(bit8)를 이중 부정으로 0/1 로 정규화한다. 비트 위치가 8 이라 그냥 AND 만 하면 0x100 이 나오기 때문이다 */
	*status = !!(slot_reg & MRL_SENSOR);	/* 0 -> close; 1 -> open */

	/* [한국어] 읽기만 하므로 항상 성공이다 */
	return 0;
}

/* [한국어] shpchp_get_adapter_status - 슬롯에 카드가 꽂혀 있는지 읽는다
 * 
 * @slot: 대상 슬롯.
 * @status: 결과를 담을 곳. 1 = 카드 있음, 0 = 비어 있음.
 * @return: 항상 0.
 * 
 * Logical Slot Register 의 Card Present 필드[11:10]는 PRSNT1#/PRSNT2# 두 신호를 그대로
 * 반영한다. 두 신호 모두 하이(값 0x3)면 아무것도 안 꽂힌 것이고, 그 밖의 조합(0x0/0x1/
 * 0x2)은 카드가 있으면서 동시에 그 카드의 소비 전력 등급을 뜻한다. 이 함수는 등급은
 * 버리고 존재 여부만 돌려준다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음, 잠들지 않음.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   get_adapter_status() (shpchp_core.c, sysfs) / shpchp_handle_attention_button /
 *   shpchp_handle_switch_change / shpchp_handle_presence_change /
 *   shpchp_enable_slot / shpchp_disable_slot / update_slot_info (shpchp_ctrl.c)
 *   → [shpchp_get_adapter_status] → shpc_readl(SLOT_REG) */
int shpchp_get_adapter_status(struct slot *slot, u8 *status)
{
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register 를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));
	/* [한국어] Card Present 필드[11:10]를 뽑아 오른쪽 정렬한다 */
	u8 state = (slot_reg & PRSNT_MASK) >> PRSNT_SHIFT;

	/* [한국어] 0x3 은 두 PRSNT 신호가 모두 하이인 상태 = 빈 슬롯이다. 그 밖의 값은 전부 카드가 있다는 뜻이므로 1 로 접는다 */
	*status = (state != 0x3) ? 1 : 0;

	/* [한국어] 읽기만 하므로 항상 성공이다 */
	return 0;
}

/* [한국어] shpchp_get_prog_int - 컨트롤러의 Programming Interface 값을 읽는다
 * 
 * @slot: 대상 슬롯(ctrl 을 얻는 통로일 뿐, 슬롯별 값이 아니라 컨트롤러 단위 값이다).
 * @prog_int: 결과를 담을 곳. 1 이면 PCI-X 133MHz 까지, 2 면 PCI-X 266/533 까지 표현하는
 *            확장 인코딩이다.
 * @return: 항상 0.
 * 
 * 같은 SHPC 레지스터라도 PI 값에 따라 필드 폭과 의미가 달라진다. 특히 Logical Slot
 * Register 의 PCI-X Capability 필드가 PI==1 에서는 2비트[13:12], PI==2 에서는
 * 3비트[14:12]이고, Secondary Bus Configuration 의 Speed/Mode 필드도 3비트냐 4비트냐가
 * 갈린다. 그래서 속도를 해석하는 코드는 반드시 먼저 이 값을 읽어야 한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음, 잠들지 않음.
 * 에러 경로: 없다. shpchp.h 에 선언이 있어 외부에 노출돼 있지만 이 트리에서 실제
 * 호출자는 같은 파일의 shpchp_get_adapter_speed() 하나뿐이다.
 * 
 * 호출 체인:
 *   shpchp_get_adapter_speed() → [shpchp_get_prog_int] → shpc_readb(PROG_INTERFACE) */
int shpchp_get_prog_int(struct slot *slot, u8 *prog_int)
{
	/* [한국어] 슬롯이 속한 컨트롤러 — PI 는 슬롯이 아니라 컨트롤러의 속성이다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] Programming Interface 레지스터(오프셋 0x13)를 바이트로 읽는다 */
	*prog_int = shpc_readb(ctrl, PROG_INTERFACE);

	/* [한국어] 읽기만 하므로 항상 성공이다 */
	return 0;
}

/* [한국어] shpchp_get_adapter_speed - 꽂힌 카드가 감당할 수 있는 최대 속도를 읽는다
 * 
 * @slot: 대상 슬롯.
 * @value: 결과 pci_bus_speed 를 담을 곳.
 * @return: 0 이면 성공. -ENODEV 는 PI 값이 1/2 가 아니거나 PCI-X Capability 인코딩이
 *          규격에 정의되지 않은 경우다(이때 *value 는 PCI_SPEED_UNKNOWN 이 된다).
 * 
 * 카드를 꽂았을 때 버스 속도를 어디에 맞출지 결정하려면 카드의 능력을 알아야 한다.
 * SHPC 는 그 정보를 Logical Slot Register 안에 두 조각으로 넣어 두었다 — 통상 PCI 는
 * 66MHz Capable 비트(bit9) 하나로, PCI-X 는 PCI-X Capability 필드로 표현한다.
 * 그래서 PCI-X 인코딩이 0 일 때만 66MHz 비트를 봐서 33/66 을 가르는 구조가 된다.
 * 
 * 동작 단계: (1) 슬롯 레지스터와 66MHz Capable 비트를 읽는다. (2) PI 를 읽어 PCI-X
 * Capability 필드 폭을 정한다. (3) 그 필드를 pci_bus_speed 로 번역한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐). 락 없음, 잠들지 않음.
 * 에러 경로: -ENODEV 를 돌려주며, 호출자 board_added() 는 이를 WRONG_BUS_FREQUENCY 로
 * 바꿔 상위에 올려 슬롯 활성화를 포기한다.
 * 
 * 호출 체인:
 *   board_added() (shpchp_ctrl.c) → [shpchp_get_adapter_speed]
 *   → shpc_readl(SLOT_REG) + shpchp_get_prog_int() */
int shpchp_get_adapter_speed(struct slot *slot, enum pci_bus_speed *value)
{
	/* [한국어] 기본은 성공 */
	int retval = 0;
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register 를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));
	/* [한국어] 66MHz Capable 비트(bit9)를 0/1 로 정규화한다. PCI-X 가 아닌 통상 PCI 카드의 33/66 을 가르는 유일한 근거다 */
	u8 m66_cap  = !!(slot_reg & MHZ66_CAP);
	/* [한국어] pi: Programming Interface 값, pcix_cap: PCI-X Capability 필드에서 뽑아낸 값 */
	u8 pi, pcix_cap;

	/* [한국어] PI 를 읽어 아래에서 쓸 필드 폭을 결정한다 */
	retval = shpchp_get_prog_int(slot, &pi);
	/* [한국어] 이 헬퍼는 항상 0 을 돌려주지만 반환값 검사 관례를 지켜 둔 것이다 */
	if (retval)
		return retval;

	/* [한국어] PI 에 따라 PCI-X Capability 필드의 폭이 달라진다 */
	switch (pi) {
	/* [한국어] PI==1 이면 2비트 필드[13:12] */
	case 1:
		/* [한국어] 2비트 마스크로 뽑아 오른쪽 정렬 */
		pcix_cap = (slot_reg & PCIX_CAP_MASK_PI1) >> PCIX_CAP_SHIFT;
		break;
	/* [한국어] PI==2 면 3비트 필드[14:12] — 266/533 을 표현하려고 한 비트가 늘었다 */
	case 2:
		/* [한국어] 3비트 마스크로 뽑아 오른쪽 정렬 */
		pcix_cap = (slot_reg & PCIX_CAP_MASK_PI2) >> PCIX_CAP_SHIFT;
		break;
	/* [한국어] PI 가 1 도 2 도 아니면 해석 규칙을 모른다 */
	default:
		/* [한국어] 장치를 다룰 수 없다고 알린다 */
		return -ENODEV;
	}

	/* [한국어] 레지스터 원본과 뽑아낸 두 값을 디버그 로그로 남긴다 — 속도 협상 실패를 추적할 때 핵심 단서가 된다 */
	ctrl_dbg(ctrl, "%s: slot_reg = %x, pcix_cap = %x, m66_cap = %x\n",
		 __func__, slot_reg, pcix_cap, m66_cap);

	/* [한국어] PCI-X Capability 인코딩을 pci_bus_speed 로 번역한다 */
	switch (pcix_cap) {
	/* [한국어] 0x0 = PCI-X 아님(통상 PCI 카드) */
	case 0x0:
		/* [한국어] 이때만 66MHz Capable 비트를 봐서 66MHz 인지 33MHz 인지 가른다 */
		*value = m66_cap ? PCI_SPEED_66MHz : PCI_SPEED_33MHz;
		break;
	/* [한국어] 0x1 = PCI-X 66MHz */
	case 0x1:
		*value = PCI_SPEED_66MHz_PCIX;
		break;
	/* [한국어] 0x3 = PCI-X 133MHz */
	case 0x3:
		*value = PCI_SPEED_133MHz_PCIX;
		break;
	/* [한국어] 0x4 = PCI-X 266(133MHz 클럭 기준 표기) */
	case 0x4:
		*value = PCI_SPEED_133MHz_PCIX_266;
		break;
	/* [한국어] 0x5 = PCI-X 533(133MHz 클럭 기준 표기) */
	case 0x5:
		*value = PCI_SPEED_133MHz_PCIX_533;
		break;
	/* [한국어] 0x2 는 규격상 정의되지 않은 값이라 default 와 같이 묶어 처리한다 */
	case 0x2:
	default:
		/* [한국어] 속도를 알 수 없음으로 표시하고 */
		*value = PCI_SPEED_UNKNOWN;
		/* [한국어] 장치를 다룰 수 없다고 알린다 */
		retval = -ENODEV;
		break;
	}

	/* [한국어] 판정된 어댑터 속도를 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Adapter speed = %d\n", *value);
	/* [한국어] 성공/실패를 board_added() 에 돌려준다 */
	return retval;
}

/* [한국어] shpchp_query_power_fault - 이 슬롯에 전원 결함이 있는지 확인한다
 * 
 * @slot: 대상 슬롯.
 * @return: 1 이면 전원 결함 상태, 0 이면 정상. (음수 오류는 없다.)
 * 
 * Logical Slot Register 의 Power Fault 비트(bit6)는 액티브 로우다 — 논리 0 이 "결함
 * 있음"을 뜻한다. 그래서 이 함수가 부정 연산으로 뒤집어 준다. 이 뒤집기를 놓치면 전원
 * 정상인 슬롯을 결함으로 오인하게 되므로, 상류 코드도 굳이 주석으로 못 박아 두었다.
 * shpc_isr() 이 전원 결함 이벤트 비트를 보고 shpchp_handle_power_fault() 를 부르면,
 * 그 안에서 이 함수로 "결함이 새로 생긴 것인지 해제된 것인지"를 구분한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐가 아니라 인터럽트 핸들러가 부르는 경로에서
 * 호출된다 — shpchp_handle_power_fault() 는 shpc_isr() 안에서 실행되므로 인터럽트
 * 컨텍스트다). MMIO 읽기 하나뿐이라 잠들지 않아 그 컨텍스트에서도 안전하다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   shpc_isr() → shpchp_handle_power_fault() (shpchp_ctrl.c)
 *   → [shpchp_query_power_fault] → shpc_readl(SLOT_REG) */
int shpchp_query_power_fault(struct slot *slot)
{
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 Logical Slot Register 를 읽는다 */
	u32 slot_reg = shpc_readl(ctrl, SLOT_REG(slot->hp_slot));

	/* Note: Logic 0 => fault */
	/* [한국어] Power Fault 비트(bit6)는 액티브 로우다. 비트가 0 이면 결함이므로 부정해 1(결함 있음)로 만든다 */
	return !(slot_reg & POWER_FAULT);
}

/* [한국어] shpchp_set_attention_status - 슬롯의 Attention(황색) LED 상태를 SHPC 명령으로 바꾼다
 * 
 * @slot: 대상 슬롯. slot->hp_slot 이 SHPC 가 세는 0 기반 논리 슬롯 번호다.
 * @value: 0 = 끄기, 1 = 켜기, 2 = 깜빡임. 그 밖의 값은 거절한다.
 * @return: 0 이면 성공, -1 이면 잘못된 value, 그 밖의 음수는 shpc_write_cmd() 가
 *          돌려준 -EBUSY/-EIO/-EINTR 이다.
 * 
 * Attention LED 는 "이 슬롯에 문제가 있다"거나 "이 슬롯을 지금 만지고 있다"를 사람에게
 * 알리는 유일한 수단이다. pciehp 라면 Slot Control 레지스터에 비트를 직접 쓰면 되지만,
 * SHPC 는 반드시 Command 레지스터에 명령 코드를 넣고 완료를 기다려야 한다. 그래서 이
 * 함수는 value 를 SHPC 명령 코드(Slot Operation 계열)로 번역한 뒤 shpc_write_cmd() 에
 * 넘기는 얇은 번역 계층이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트에서만 불린다. shpc_write_cmd() 안에서 최대 1초
 * 잠들 수 있으므로 인터럽트 컨텍스트에서 부르면 안 된다. cmd_lock 이 직렬화를 맡는다.
 * 에러 경로: 잘못된 value 는 명령을 내기 전에 -1 로 튕긴다.
 * 
 * 호출 체인:
 *   set_attention_status() (shpchp_core.c, sysfs 쓰기) 또는
 *   remove_board()/handle_button_press_event()/interrupt_event_handler() (shpchp_ctrl.c)
 *   → [shpchp_set_attention_status] → shpc_write_cmd() → CMD 레지스터 쓰기 */
int shpchp_set_attention_status(struct slot *slot, u8 value)
{
	/* [한국어] 명령 코드를 담을 변수. 0 은 어떤 명령도 아니므로 아래 switch 가 반드시 덮어써야 한다 */
	u8 slot_cmd = 0;

	/* [한국어] sysfs/커널이 넘긴 논리적 LED 상태를 SHPC 명령 코드로 번역한다 */
	switch (value) {
		/* [한국어] 0 = LED 끄기 요청 */
		case 0:
			/* [한국어] Slot Operation 명령의 Attention Indicator 필드에 OFF(0x30)를 지정한다 */
			slot_cmd = SET_ATTN_OFF;	/* OFF */
			break;
		/* [한국어] 1 = LED 켜기 요청 */
		case 1:
			/* [한국어] Attention Indicator ON(0x10) */
			slot_cmd = SET_ATTN_ON;		/* ON */
			break;
		/* [한국어] 2 = LED 깜빡임 요청 */
		case 2:
			/* [한국어] Attention Indicator BLINK(0x20) */
			slot_cmd = SET_ATTN_BLINK;	/* BLINK */
			break;
		/* [한국어] 규격에 없는 값 — 번역할 명령 코드가 없다 */
		default:
			/* [한국어] 명령을 내지 않고 실패를 알린다. 호출자(sysfs 쓰기)는 이를 -EINVAL 성 오류로 다룬다 */
			return -1;
	}

	/* [한국어] 번역된 명령을 대상 슬롯에 발행한다. 완료까지 최대 1초 잠들 수 있다 */
	return shpc_write_cmd(slot, slot->hp_slot, slot_cmd);
}


/* [한국어] shpchp_green_led_on - 슬롯의 Power(녹색) LED 를 켠다
 * 
 * @slot: 대상 슬롯.
 * @return: 없음. shpc_write_cmd() 의 실패는 여기서 무시되며, 실패해도 로그만 남지 않고
 *          조용히 넘어간다(상류 코드가 그렇게 되어 있다).
 * 
 * 녹색 LED 는 "슬롯 전원이 들어와 있고 카드를 만지면 안 된다"를 뜻한다. board_added()
 * 가 카드 설정을 마친 뒤 이 함수로 LED 를 켜서 작업 완료를 사람에게 알린다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 내부에서 최대 1초 잠들 수 있다.
 * 
 * 호출 체인:
 *   board_added() / handle_button_press_event() (shpchp_ctrl.c)
 *   → [shpchp_green_led_on] → shpc_write_cmd() → CMD 레지스터 */
void shpchp_green_led_on(struct slot *slot)
{
	/* [한국어] Slot Operation 명령의 Power Indicator 필드에 ON(0x04)을 지정해 발행한다. 슬롯 상태 필드는 건드리지 않으므로 전원은 그대로다 */
	shpc_write_cmd(slot, slot->hp_slot, SET_PWR_ON);
}

/* [한국어] shpchp_green_led_off - 슬롯의 Power(녹색) LED 를 끈다
 * 
 * @slot: 대상 슬롯.
 * @return: 없음(실패는 무시된다).
 * 
 * 핫플러그 추가가 실패했거나 카드를 뺄 준비가 끝났을 때 녹색 LED 를 꺼서 "이제
 * 만져도 된다"를 알린다. 실행 컨텍스트는 프로세스 컨텍스트이며 최대 1초 잠들 수 있다.
 * 
 * 호출 체인:
 *   shpchp_pushbutton_thread() / handle_button_press_event() /
 *   interrupt_event_handler() (shpchp_ctrl.c)
 *   → [shpchp_green_led_off] → shpc_write_cmd() → CMD 레지스터 */
void shpchp_green_led_off(struct slot *slot)
{
	/* [한국어] Power Indicator 필드에 OFF(0x0c)를 지정해 발행한다 */
	shpc_write_cmd(slot, slot->hp_slot, SET_PWR_OFF);
}

/* [한국어] shpchp_green_led_blink - 슬롯의 Power(녹색) LED 를 깜빡이게 한다
 * 
 * @slot: 대상 슬롯.
 * @return: 없음(실패는 무시된다).
 * 
 * Attention 버튼을 눌렀을 때 5초 동안 녹색 LED 를 깜빡여 "지금 취소할 수 있다"를
 * 알리는 용도다. 이 깜빡임이 shpchp_ctrl.c 의 BLINKINGON/BLINKINGOFF 상태와 짝을 이룬다.
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐). 최대 1초 잠들 수 있다.
 * 
 * 호출 체인:
 *   handle_button_press_event() (shpchp_ctrl.c)
 *   → [shpchp_green_led_blink] → shpc_write_cmd() → CMD 레지스터 */
void shpchp_green_led_blink(struct slot *slot)
{
	/* [한국어] Power Indicator 필드에 BLINK(0x08)를 지정해 발행한다 */
	shpc_write_cmd(slot, slot->hp_slot, SET_PWR_BLINK);
}

/* [한국어] shpchp_release_ctlr - 컨트롤러가 잡고 있던 인터럽트·타이머·MMIO 를 되돌린다
 * 
 * @ctrl: 해제할 컨트롤러 서술자. shpc_init() 이 채워 둔 상태 그대로여야 한다.
 * @return: 없음. 실패해도 되돌릴 방법이 없는 해제 경로라 반환값을 두지 않는다.
 * 
 * 해제 순서가 중요하다. 먼저 하드웨어가 더는 인터럽트를 올리지 못하게 슬롯별 마스크와
 * 컨트롤러 전역 마스크를 세우고, 그다음에 핸들러/타이머를 떼고, 마지막에 매핑과 리소스
 * 예약을 반납한다. 순서를 뒤집으면 핸들러를 뗀 뒤 올라온 인터럽트가 갈 곳을 잃는다.
 * 중간에 cleanup_slots() 를 불러 슬롯 워크큐를 먼저 비우는 것도 같은 이유다 — 워크가
 * 남은 채로 매핑을 풀면 이미 해제된 creg 에 접근하게 된다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 remove 또는 probe 실패 정리). 재진입 없음.
 * 에러 경로: 없음. 모든 단계를 조건 없이 수행한다.
 * 
 * 호출 체인:
 *   shpc_probe() 실패 정리 / shpc_remove() (shpchp_core.c)
 *   → [shpchp_release_ctlr] → cleanup_slots() (shpchp_core.c)
 *   → free_irq / pci_disable_msi / timer_delete / iounmap / release_mem_region */
void shpchp_release_ctlr(struct controller *ctrl)
{
	/* [한국어] 슬롯 순회 인덱스 */
	int i;
	/* [한국어] slot_reg: Logical Slot Register 임시값, serr_int: Controller SERR-INT 레지스터 임시값 */
	u32 slot_reg, serr_int;

	/*
	 * Mask event interrupts and SERRs of all slots
	 */
	/* [한국어] 모든 슬롯의 이벤트 인터럽트와 SERR 를 막는 루프. 핸들러를 떼기 전에 먼저 입을 막는다 */
	for (i = 0; i < ctrl->num_slots; i++) {
		/* [한국어] 슬롯 i 의 Logical Slot Register 를 읽는다 */
		slot_reg = shpc_readl(ctrl, SLOT_REG(i));
		/* [한국어] Presence Change(bit24), Isolated Power Fault(bit25), Button Press(bit26), MRL Change(bit27), Connected Power Fault(bit28) 인터럽트 마스크와 MRL Change SERR(bit29), Connected Power Fault SERR(bit30) 마스크를 모두 세운다 — shpc_init() 초반과 정확히 같은 조합이다 */
		slot_reg |= (PRSNT_CHANGE_INTR_MASK | ISO_PFAULT_INTR_MASK |
			     BUTTON_PRESS_INTR_MASK | MRL_CHANGE_INTR_MASK |
			     CON_PFAULT_INTR_MASK   | MRL_CHANGE_SERR_MASK |
			     CON_PFAULT_SERR_MASK);
		/* [한국어] RsvdZ 비트(bit15, bit23:21)는 0 으로 써야 하므로 떨어뜨린다 */
		slot_reg &= ~SLOT_REG_RSVDZ_MASK;
		/* [한국어] 되쓴다. 동시에 bit16~20 의 RW1C 이벤트 래치도 지워진다 */
		shpc_writel(ctrl, SLOT_REG(i), slot_reg);
	}

	/* [한국어] 슬롯 객체 정리 — 핫플러그 코어에서 슬롯을 등록 해제하고 슬롯별 워크큐를 파괴한다(shpchp_core.c). 매핑을 풀기 전에 해야 남은 워크가 creg 를 건드리지 않는다 */
	cleanup_slots(ctrl);

	/*
	 * Mask SERR and System Interrupt generation
	 */
	/* [한국어] 컨트롤러 전역 마스크 차례 — Controller SERR-INT 레지스터를 읽는다 */
	serr_int = shpc_readl(ctrl, SERR_INTR_ENABLE);
	/* [한국어] Global Interrupt Mask(bit0), Global SERR Mask(bit1), Command Completion Interrupt Mask(bit2), Arbiter SERR Mask(bit3) 를 전부 세운다 */
	serr_int |= (GLOBAL_INTR_MASK  | GLOBAL_SERR_MASK |
		     COMMAND_INTR_MASK | ARBITER_SERR_MASK);
	/* [한국어] RsvdZ 비트를 떨어뜨리고 */
	serr_int &= ~SERR_INTR_RSVDZ_MASK;
	/* [한국어] 되쓴다. 이 시점부터 컨트롤러는 어떤 인터럽트도 올리지 않는다 */
	shpc_writel(ctrl, SERR_INTR_ENABLE, serr_int);

	/* [한국어] 폴링 모드였다면 걸린 것은 인터럽트가 아니라 타이머다 */
	if (shpchp_poll_mode)
		/* [한국어] 폴링 타이머를 제거한다. 콜백이 도는 중이면 끝날 때까지 기다리는 계열의 API 다 */
		timer_delete(&ctrl->poll_timer);
	else {
		/* [한국어] 인터럽트 모드였다면 핸들러를 뗀다. IRQF_SHARED 로 걸었으므로 dev_id 로 ctrl 을 넘겨 정확히 이 핸들러만 떼어 낸다 */
		free_irq(ctrl->pci_dev->irq, ctrl);
		/* [한국어] MSI 를 켰다면 끈다. MSI 를 못 얻어 INTx 로 떨어졌던 경우에도 안전하게 호출할 수 있다 */
		pci_disable_msi(ctrl->pci_dev);
	}

	/* [한국어] 커널 가상 매핑 해제 — 이 시점 이후 creg 접근은 모두 잘못된 접근이다 */
	iounmap(ctrl->creg);
	/* [한국어] 리소스 트리에서 MMIO 구간 예약을 반납한다. request_mem_region() 과 짝을 이룬다 */
	release_mem_region(ctrl->mmio_base, ctrl->mmio_size);
}

/* [한국어] shpchp_power_on_slot - 슬롯에 전원만 넣는다(버스에는 아직 붙이지 않는다)
 * 
 * @slot: 대상 슬롯.
 * @return: 0 이면 성공, 음수면 명령 발행 실패(-EBUSY/-EIO/-EINTR).
 * 
 * SHPC 의 슬롯 상태는 Disabled → Power Only → Enabled 3단계다. 카드를 꽂고 곧바로
 * 버스에 연결하면 전원이 안정되기 전에 신호가 오가 문제가 되므로, 먼저 Power Only 로
 * 전원만 올린 뒤 속도 협상을 마치고 나서 Enable 로 넘어간다. 이 함수가 그 첫 단계다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐 또는 sysfs 쓰기). 최대 1초 잠들 수 있다.
 * 에러 경로: 실패하면 로그를 남기고 그대로 반환하며, 호출자 board_added() 가 -1 을
 * 돌려주어 상위에서 슬롯을 다시 끈다.
 * 
 * 호출 체인:
 *   board_added() (shpchp_ctrl.c) → [shpchp_power_on_slot]
 *   → shpc_write_cmd(SET_SLOT_PWR) → CMD 레지스터 */
int shpchp_power_on_slot(struct slot *slot)
{
	/* [한국어] 명령 발행 결과를 담을 변수 */
	int retval;

	/* [한국어] Slot Operation 명령의 Slot State 필드에 Power Only(0x01)를 지정해 발행한다. LED 필드는 0 이라 지시 없음 = 변경하지 않는다 */
	retval = shpc_write_cmd(slot, slot->hp_slot, SET_SLOT_PWR);
	/* [한국어] 명령이 실패했다면 */
	if (retval)
		/* [한국어] 함수 이름과 함께 실패를 기록한다 — 어느 단계에서 넘어졌는지 로그로 구분하기 위해서다 */
		ctrl_err(slot->ctrl, "%s: Write command failed!\n", __func__);

	/* [한국어] 성공/실패를 그대로 상위 상태 머신에 전달한다 */
	return retval;
}

/* [한국어] shpchp_slot_enable - 슬롯을 버스에 연결하고 LED 를 진행 중 표시로 바꾼다
 * 
 * @slot: 대상 슬롯.
 * @return: 0 이면 성공, 음수면 명령 발행 실패.
 * 
 * 세 가지 지시를 하나의 Slot Operation 명령에 합쳐 보낸다는 점이 핵심이다. SHPC 의
 * Slot Operation 명령 코드는 [1:0] Slot State, [3:2] Power Indicator, [5:4] Attention
 * Indicator 세 필드를 한 바이트에 담기 때문에, OR 로 합치면 한 번의 명령으로 세 가지가
 * 동시에 바뀐다. 명령 한 번당 최대 1초를 기다려야 하므로 이렇게 합치는 것이 이득이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 최대 1초 잠들 수 있다.
 * 에러 경로: 실패 시 로그를 남기고 반환하며, board_added() 가 그대로 상위로 올린다.
 * 
 * 호출 체인:
 *   board_added() (shpchp_ctrl.c) → [shpchp_slot_enable]
 *   → shpc_write_cmd(SET_SLOT_ENABLE | SET_PWR_BLINK | SET_ATTN_OFF) */
int shpchp_slot_enable(struct slot *slot)
{
	/* [한국어] 명령 발행 결과를 담을 변수 */
	int retval;

	/* Slot - Enable, Power Indicator - Blink, Attention Indicator - Off */
	/* [한국어] Slot State = Enable(0x02), Power Indicator = Blink(0x08), Attention Indicator = Off(0x30) 를 OR 로 합쳐 0x3a 한 바이트로 만든다. 세 필드가 겹치지 않아 OR 가 성립한다 */
	retval = shpc_write_cmd(slot, slot->hp_slot,
			SET_SLOT_ENABLE | SET_PWR_BLINK | SET_ATTN_OFF);
	/* [한국어] 명령이 실패했다면 */
	if (retval)
		/* [한국어] 실패를 기록하고 */
		ctrl_err(slot->ctrl, "%s: Write command failed!\n", __func__);

	/* [한국어] 결과를 그대로 상위로 올린다 */
	return retval;
}

/* [한국어] shpchp_slot_disable - 슬롯을 버스에서 떼고 전원을 내리며 LED 를 경고 표시로 바꾼다
 * 
 * @slot: 대상 슬롯.
 * @return: 0 이면 성공, 음수면 명령 발행 실패.
 * 
 * shpchp_slot_enable() 의 정확한 반대다. 역시 세 필드를 한 명령에 합쳐 보낸다.
 * Attention LED 를 켜는 이유는 "이 슬롯은 지금 비활성 상태다"를 사람에게 알리기
 * 위해서이며, remove_board() 는 정상 제거를 마친 뒤 곧바로
 * shpchp_set_attention_status(slot, 0) 으로 이 LED 를 다시 끈다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 최대 1초 잠들 수 있다.
 * 에러 경로: 실패 시 로그를 남기고 반환한다. board_added() 의 err_exit 정리 경로와
 * remove_board() 두 곳에서 쓰이며, 여기서 실패하면 상위가 그대로 실패로 처리한다.
 * 
 * 호출 체인:
 *   board_added() 의 err_exit / remove_board() (shpchp_ctrl.c)
 *   → [shpchp_slot_disable]
 *   → shpc_write_cmd(SET_SLOT_DISABLE | SET_PWR_OFF | SET_ATTN_ON) */
int shpchp_slot_disable(struct slot *slot)
{
	/* [한국어] 명령 발행 결과를 담을 변수 */
	int retval;

	/* Slot - Disable, Power Indicator - Off, Attention Indicator - On */
	/* [한국어] Slot State = Disable(0x03), Power Indicator = Off(0x0c), Attention Indicator = On(0x10) 를 OR 로 합쳐 0x1f 한 바이트로 만든다 */
	retval = shpc_write_cmd(slot, slot->hp_slot,
			SET_SLOT_DISABLE | SET_PWR_OFF | SET_ATTN_ON);
	/* [한국어] 명령이 실패했다면 */
	if (retval)
		/* [한국어] 실패를 기록하고 */
		ctrl_err(slot->ctrl, "%s: Write command failed!\n", __func__);

	/* [한국어] 결과를 그대로 상위로 올린다 */
	return retval;
}

/* [한국어] shpc_get_cur_bus_speed - 세컨더리 버스가 지금 실제로 돌고 있는 속도를 읽어 pci_bus 에 기록한다
 * 
 * @ctrl: creg 매핑이 끝난 컨트롤러 서술자.
 * @return: 0 이면 성공. 인코딩이 해석 불가면 -ENODEV(이때 cur_bus_speed 는
 *          PCI_SPEED_UNKNOWN 으로 남는다).
 * 
 * shpc_get_max_bus_speed() 가 "낼 수 있는 최대 속도"를 보는 것과 달리 이 함수는
 * Secondary Bus Configuration 레지스터(오프셋 0x10)의 Speed/Mode 필드를 읽어 "지금
 * 설정된 속도"를 본다. 속도를 바꾸는 명령을 낸 직후에도 이 함수를 불러 실제 반영값을
 * 갱신한다. Programming Interface 가 1 이면 필드가 3비트(0~7), 2 면 4비트(0~13)라
 * 마스크와 유효 범위가 달라진다 — PI 에 따라 인코딩 폭이 바뀌는 것이 SHPC 의 특징이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(shpc_init 또는 속도 변경 명령 직후). 락은 잡지 않는다.
 * 에러 경로: -ENODEV 를 돌려주지만 두 호출자 모두 반환값을 검사하지 않는다.
 * 
 * 호출 체인:
 *   shpc_init() / shpchp_set_bus_speed_mode() → [shpc_get_cur_bus_speed]
 *   → shpc_readw/shpc_readb (MMIO 읽기) → bus->cur_bus_speed 갱신 */
static int shpc_get_cur_bus_speed(struct controller *ctrl)
{
	/* [한국어] 기본값 0(성공) */
	int retval = 0;
	/* [한국어] SHPC 가 관장하는 세컨더리 버스 객체 — 결과를 여기에 기록한다 */
	struct pci_bus *bus = ctrl->pci_dev->subordinate;
	/* [한국어] 판정 전 초깃값 */
	enum pci_bus_speed bus_speed = PCI_SPEED_UNKNOWN;
	/* [한국어] Secondary Bus Configuration 레지스터(오프셋 0x10) 읽기 */
	u16 sec_bus_reg = shpc_readw(ctrl, SEC_BUS_CONFIG);
	/* [한국어] Programming Interface(오프셋 0x13) — 인코딩 폭을 결정한다 */
	u8 pi = shpc_readb(ctrl, PROG_INTERFACE);
	/* [한국어] PI==2 면 하위 4비트(0x0~0xd 까지 정의), PI==1 이면 하위 3비트만 유효하다 */
	u8 speed_mode = (pi == 2) ? (sec_bus_reg & 0xF) : (sec_bus_reg & 0x7);

	/* [한국어] PI==1 인데 값이 4 를 넘으면 규격에 정의되지 않은 인코딩이다 */
	if ((pi == 1) && (speed_mode > 4)) {
		/* [한국어] 해석 불가로 표시하고 */
		retval = -ENODEV;
		/* [한국어] 판정을 건너뛰어 out 으로 간다 — bus_speed 는 PCI_SPEED_UNKNOWN 그대로다 */
		goto out;
	}

	/* [한국어] Speed/Mode 인코딩을 pci_bus_speed 열거값으로 번역한다 */
	switch (speed_mode) {
	/* [한국어] 0x0 = 통상 PCI 33MHz */
	case 0x0:
		bus_speed = PCI_SPEED_33MHz;
		break;
	/* [한국어] 0x1 = 통상 PCI 66MHz */
	case 0x1:
		bus_speed = PCI_SPEED_66MHz;
		break;
	/* [한국어] 0x2 = PCI-X 66MHz */
	case 0x2:
		bus_speed = PCI_SPEED_66MHz_PCIX;
		break;
	/* [한국어] 0x3 = PCI-X 100MHz */
	case 0x3:
		bus_speed = PCI_SPEED_100MHz_PCIX;
		break;
	/* [한국어] 0x4 = PCI-X 133MHz — PI==1 에서 표현 가능한 최대값이다 */
	case 0x4:
		bus_speed = PCI_SPEED_133MHz_PCIX;
		break;
	/* [한국어] 0x5 = PCI-X 66MHz ECC 모드 */
	case 0x5:
		bus_speed = PCI_SPEED_66MHz_PCIX_ECC;
		break;
	/* [한국어] 0x6 = PCI-X 100MHz ECC 모드 */
	case 0x6:
		bus_speed = PCI_SPEED_100MHz_PCIX_ECC;
		break;
	/* [한국어] 0x7 = PCI-X 133MHz ECC 모드. PI==1 에서는 여기까지도 나올 수 있다 */
	case 0x7:
		bus_speed = PCI_SPEED_133MHz_PCIX_ECC;
		break;
	/* [한국어] 0x8 = PCI-X 266 의 66MHz — 여기서부터는 PI==2 에서만 나온다 */
	case 0x8:
		bus_speed = PCI_SPEED_66MHz_PCIX_266;
		break;
	/* [한국어] 0x9 = PCI-X 266 의 100MHz */
	case 0x9:
		bus_speed = PCI_SPEED_100MHz_PCIX_266;
		break;
	/* [한국어] 0xa = PCI-X 266 의 133MHz */
	case 0xa:
		bus_speed = PCI_SPEED_133MHz_PCIX_266;
		break;
	/* [한국어] 0xb = PCI-X 533 의 66MHz */
	case 0xb:
		bus_speed = PCI_SPEED_66MHz_PCIX_533;
		break;
	/* [한국어] 0xc = PCI-X 533 의 100MHz */
	case 0xc:
		bus_speed = PCI_SPEED_100MHz_PCIX_533;
		break;
	/* [한국어] 0xd = PCI-X 533 의 133MHz — 이 인코딩이 표현할 수 있는 최고 속도다 */
	case 0xd:
		bus_speed = PCI_SPEED_133MHz_PCIX_533;
		break;
	/* [한국어] 규격에 정의되지 않은 인코딩 */
	default:
		/* [한국어] 해석 불가를 알린다 */
		retval = -ENODEV;
		break;
	}

 out:
	/* [한국어] 판정 결과(또는 PCI_SPEED_UNKNOWN)를 PCI 코어의 버스 객체에 기록한다 */
	bus->cur_bus_speed = bus_speed;
	/* [한국어] 결정된 현재 속도를 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Current bus speed = %d\n", bus_speed);
	/* [한국어] 성공/실패를 돌려준다 — 다만 두 호출자 모두 이 값을 검사하지 않는다 */
	return retval;
}


/* [한국어] shpchp_set_bus_speed_mode - 세컨더리 버스의 속도/모드를 SHPC 명령으로 바꾼다
 * 
 * @slot: 대상 슬롯. 다만 이 명령은 슬롯이 아니라 버스 세그먼트 전체에 걸리므로,
 *        아래에서 target slot 인자로 0 을 넘긴다(slot 은 ctrl 을 얻는 통로일 뿐이다).
 * @value: 원하는 pci_bus_speed 열거값.
 * @return: 0 이면 성공. -EINVAL 은 이 컨트롤러가 표현할 수 없는 속도이거나 규격에 없는
 *          값이고, 그 밖의 음수는 shpc_write_cmd() 의 실패다.
 * 
 * 핫플러그로 새 카드가 꽂히면 그 카드가 감당할 수 있는 속도로 버스 전체를 맞춰야 한다.
 * 버스는 가장 느린 참가자에 맞춰 돌아야 하기 때문이다. SHPC 는 이를 위해
 * Set Bus Segment Speed/Mode A(0x40~) 와 B(0x50~) 두 계열의 명령 코드를 두었고,
 * PI==1 장치는 A 계열만, PI==2 장치는 B 계열까지 이해한다.
 * 
 * 동작 단계: (1) PI 를 읽어 PI==1 인데 PCI-X 133MHz 를 넘는 속도를 요구하면 거절한다.
 * (2) pci_bus_speed 를 명령 코드로 번역한다. (3) target slot 0 으로 명령을 발행한다.
 * (4) 성공하면 shpc_get_cur_bus_speed() 로 실제 반영값을 다시 읽어 pci_bus 를 갱신한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐). 최대 1초 잠들 수 있다.
 * 에러 경로: 번역 실패는 명령을 내기 전에 -EINVAL 로 튕기고, 명령 실패는 로그 후 반환.
 * 호출자 board_added()/change_bus_speed() 는 실패를 WRONG_BUS_FREQUENCY 로 바꿔 올린다.
 * 
 * 호출 체인:
 *   board_added() / change_bus_speed() (shpchp_ctrl.c)
 *   → [shpchp_set_bus_speed_mode] → shpc_write_cmd() → CMD 레지스터
 *   → 성공 시 shpc_get_cur_bus_speed() */
int shpchp_set_bus_speed_mode(struct slot *slot, enum pci_bus_speed value)
{
	/* [한국어] 명령 발행 결과 */
	int retval;
	/* [한국어] 슬롯이 속한 컨트롤러 — 로그와 레지스터 접근에 쓴다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] pi: Programming Interface 값, cmd: 번역된 SHPC 명령 코드 */
	u8 pi, cmd;

	/* [한국어] Programming Interface 레지스터(오프셋 0x13)를 읽어 이 컨트롤러가 어느 명령 계열까지 이해하는지 확인한다 */
	pi = shpc_readb(ctrl, PROG_INTERFACE);
	/* [한국어] PI==1 컨트롤러는 Set Bus Segment Speed/Mode A 계열(33MHz~PCI-X 133MHz)만 가지므로 그보다 빠른 요구는 표현할 방법이 없다. 열거형 pci_bus_speed 의 정의 순서가 느린 것부터라는 전제에 기대는 비교다 */
	if ((pi == 1) && (value > PCI_SPEED_133MHz_PCIX))
		return -EINVAL;

	/* [한국어] 요청한 속도를 SHPC 명령 코드로 번역한다 */
	switch (value) {
	/* [한국어] 통상 PCI 33MHz → A 계열 0x40 */
	case PCI_SPEED_33MHz:
		cmd = SETA_PCI_33MHZ;
		break;
	/* [한국어] 통상 PCI 66MHz → A 계열 0x41 */
	case PCI_SPEED_66MHz:
		cmd = SETA_PCI_66MHZ;
		break;
	/* [한국어] PCI-X 66MHz → A 계열 0x42 */
	case PCI_SPEED_66MHz_PCIX:
		cmd = SETA_PCIX_66MHZ;
		break;
	/* [한국어] PCI-X 100MHz → A 계열 0x43 */
	case PCI_SPEED_100MHz_PCIX:
		cmd = SETA_PCIX_100MHZ;
		break;
	/* [한국어] PCI-X 133MHz → A 계열 0x44. PI==1 에서 갈 수 있는 마지막 지점이다 */
	case PCI_SPEED_133MHz_PCIX:
		cmd = SETA_PCIX_133MHZ;
		break;
	/* [한국어] PCI-X 66MHz ECC → B 계열의 Error-checking Mode 0x55 */
	case PCI_SPEED_66MHz_PCIX_ECC:
		cmd = SETB_PCIX_66MHZ_EM;
		break;
	/* [한국어] PCI-X 100MHz ECC → B 계열 0x56 */
	case PCI_SPEED_100MHz_PCIX_ECC:
		cmd = SETB_PCIX_100MHZ_EM;
		break;
	/* [한국어] PCI-X 133MHz ECC → B 계열 0x57 */
	case PCI_SPEED_133MHz_PCIX_ECC:
		cmd = SETB_PCIX_133MHZ_EM;
		break;
	/* [한국어] PCI-X 266 의 66MHz → B 계열 0x58 */
	case PCI_SPEED_66MHz_PCIX_266:
		cmd = SETB_PCIX_66MHZ_266;
		break;
	/* [한국어] PCI-X 266 의 100MHz → B 계열 0x59 */
	case PCI_SPEED_100MHz_PCIX_266:
		cmd = SETB_PCIX_100MHZ_266;
		break;
	/* [한국어] PCI-X 266 의 133MHz → B 계열 0x5a */
	case PCI_SPEED_133MHz_PCIX_266:
		cmd = SETB_PCIX_133MHZ_266;
		break;
	/* [한국어] PCI-X 533 의 66MHz → B 계열 0x5b */
	case PCI_SPEED_66MHz_PCIX_533:
		cmd = SETB_PCIX_66MHZ_533;
		break;
	/* [한국어] PCI-X 533 의 100MHz → B 계열 0x5c */
	case PCI_SPEED_100MHz_PCIX_533:
		cmd = SETB_PCIX_100MHZ_533;
		break;
	/* [한국어] PCI-X 533 의 133MHz → B 계열 0x5d */
	case PCI_SPEED_133MHz_PCIX_533:
		cmd = SETB_PCIX_133MHZ_533;
		break;
	/* [한국어] 위 표에 없는 pci_bus_speed 값은 SHPC 명령으로 표현할 수 없다 */
	default:
		/* [한국어] 명령을 내지 않고 거절한다 */
		return -EINVAL;
	}

	/* [한국어] target slot 에 0 을 넘긴다 — 이 명령은 특정 슬롯이 아니라 버스 세그먼트 전체를 대상으로 하기 때문이다. shpc_write_cmd() 안에서 ++t_slot 이 적용돼 레지스터에는 1 이 실린다 */
	retval = shpc_write_cmd(slot, 0, cmd);
	/* [한국어] 명령이 실패했다면 */
	if (retval)
		/* [한국어] 실패를 기록한다. 상위는 이를 WRONG_BUS_FREQUENCY 로 바꿔 올린다 */
		ctrl_err(ctrl, "%s: Write command failed!\n", __func__);
	/* [한국어] 성공했다면 */
	else
		/* [한국어] 실제로 반영된 속도를 다시 읽어 bus->cur_bus_speed 를 갱신한다 — 요청값과 반영값이 다를 수 있으므로 하드웨어를 다시 믿는다 */
		shpc_get_cur_bus_speed(ctrl);

	/* [한국어] 성공/실패를 상위 상태 머신에 돌려준다 */
	return retval;
}

/* [한국어] shpc_isr - SHPC 컨트롤러의 인터럽트 서비스 루틴(폴링 모드에서는 타이머 콜백이 대신 부른다)
 * 
 * @irq: 실제 인터럽트에서는 request_irq() 로 등록한 IRQ 번호. int_poll_timeout() 이
 *       부를 때는 0 을 넘긴다(폴링 경로라는 뜻이며 이 함수는 값을 쓰지 않는다).
 * @dev_id: request_irq() 의 마지막 인자로 넘긴 struct controller 포인터. IRQF_SHARED 로
 *       공유 IRQ 에 붙기 때문에, 이 값으로 어느 컨트롤러의 인터럽트인지 구분한다.
 * @return: IRQ_NONE 이면 내 인터럽트가 아니라는 뜻이라 커널이 다음 공유 핸들러로 넘긴다.
 *       IRQ_HANDLED 면 이 핸들러가 처리했다는 뜻이다.
 * 
 * SHPC 는 이벤트를 두 종류로 올린다. 하나는 Command 레지스터에 넣은 명령이 끝났다는
 * Command Completion 인터럽트이고, 다른 하나는 슬롯별 이벤트(MRL 개폐, Attention 버튼,
 * 카드 삽입/제거, 전원 결함)다. Interrupt Locator 레지스터 한 개가 두 종류를 모두
 * 비트로 알려 주기 때문에, 이 함수가 그 비트를 풀어 각각의 처리 경로로 나눠 보낸다.
 * 
 * 동작 단계: (1) INTR_LOC 을 읽어 0 이면 내 인터럽트가 아니므로 즉시 IRQ_NONE.
 * (2) 인터럽트 모드면 Global Interrupt Mask 를 세워 처리 중 중첩 인터럽트를 막는다
 * (SHPC 규격 rev 1.0 p.139 의 구현 노트). (3) 명령 완료 비트가 서 있으면 Controller
 * SERR-INT 레지스터의 Command Completion Detect 를 RW1C 로 지우고 ctrl->queue 에서
 * 자는 shpc_wait_cmd() 를 깨운다. (4) 슬롯 비트를 순회하며 Logical Slot Register 를
 * 읽어 이벤트별 핸들러를 부르고, 읽은 값을 그대로 되써서 RW1C 이벤트 래치를 지운다.
 * (5) 마지막에 Global Interrupt Mask 를 풀어 인터럽트를 다시 연다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트(잠들 수 없음). 폴링 모드에서는 타이머 콜백
 * 컨텍스트다. 이 안에서 부르는 shpchp_handle_* 는 kmalloc(GFP_ATOMIC) 후 워크큐에
 * 넣기만 하므로 잠들지 않는다. 같은 컨트롤러에 대해 재진입하지 않는다고 가정한다.
 * 에러 경로: 별도 실패 반환이 없다. 내 것이 아닌 인터럽트만 IRQ_NONE 으로 넘긴다.
 * 
 * 호출 체인:
 *   커널 IRQ 코어(request_irq 등록) 또는 int_poll_timeout() → [shpc_isr]
 *   → shpchp_handle_switch_change / shpchp_handle_attention_button /
 *     shpchp_handle_presence_change / shpchp_handle_power_fault (shpchp_ctrl.c)
 *   → wake_up_interruptible(&ctrl->queue) 로 shpc_wait_cmd() 기상 */
static irqreturn_t shpc_isr(int irq, void *dev_id)
{
	/* [한국어] request_irq() 에 넘겼던 dev_id 를 컨트롤러 서술자로 되돌린다. 공유 IRQ 이므로 이 포인터가 어느 SHPC 인지 구분하는 유일한 근거다 */
	struct controller *ctrl = (struct controller *)dev_id;
	/* [한국어] serr_int: Controller SERR-INT 레지스터 임시값, slot_reg: Logical Slot Register 임시값, intr_loc/intr_loc2: Interrupt Locator 레지스터 값(마스크 전후 두 번 읽는다) */
	u32 serr_int, slot_reg, intr_loc, intr_loc2;
	/* [한국어] 논리 슬롯 인덱스(0 기반). 물리 device 번호가 아니라 SHPC 가 세는 슬롯 번호다 */
	int hp_slot;

	/* Check to see if it was our interrupt */
	/* [한국어] Interrupt Locator 레지스터(오프셋 0x18) 읽기. bit0 = 명령 완료 대기, bit(i+1) = i 번 슬롯 이벤트 대기 */
	intr_loc = shpc_readl(ctrl, INTR_LOC);
	/* [한국어] 한 비트도 서 있지 않으면 이 SHPC 가 올린 인터럽트가 아니다 — 공유 IRQ 라 다른 장치일 수 있다 */
	if (!intr_loc)
		return IRQ_NONE;

	/* [한국어] 어떤 비트가 서 있었는지 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "%s: intr_loc = %x\n", __func__, intr_loc);

	/* [한국어] 폴링 모드가 아닐 때만 = 진짜 인터럽트 경로일 때만 마스크 조작이 필요하다. 폴링 모드에서는 애초에 인터럽트가 오지 않으므로 중첩을 막을 이유가 없다 */
	if (!shpchp_poll_mode) {
		/*
		 * Mask Global Interrupt Mask - see implementation
		 * note on p. 139 of SHPC spec rev 1.0
		 */
		/* [한국어] Controller SERR-INT 레지스터(오프셋 0x20) 를 읽어 현재 마스크 상태를 가져온다 */
		serr_int = shpc_readl(ctrl, SERR_INTR_ENABLE);
		/* [한국어] Global Interrupt Mask(bit0) 를 세워 이 핸들러가 도는 동안 새 인터럽트가 올라오지 않게 한다 */
		serr_int |= GLOBAL_INTR_MASK;
		serr_int &= ~SERR_INTR_RSVDZ_MASK;
		/* [한국어] 마스크를 세운 값을 되쓴다. 이 시점부터 컨트롤러는 인터럽트 신호를 올리지 않는다 */
		shpc_writel(ctrl, SERR_INTR_ENABLE, serr_int);

		/* [한국어] 마스크를 세운 뒤 Interrupt Locator 를 다시 읽는다. 마스크 직전에 새로 생긴 이벤트가 있는지 디버그로 확인하기 위한 것이며, 아래 처리 로직은 여전히 첫 번째로 읽은 intr_loc 을 쓴다 */
		intr_loc2 = shpc_readl(ctrl, INTR_LOC);
		/* [한국어] 두 번째로 읽은 값도 로그로 남긴다 */
		ctrl_dbg(ctrl, "%s: intr_loc2 = %x\n", __func__, intr_loc2);
	}

	/* [한국어] bit0(Command Completion Interrupt Pending)이 서 있으면 앞서 CMD 레지스터에 넣은 명령이 끝났다는 뜻이다 */
	if (intr_loc & CMD_INTR_PENDING) {
		/*
		 * Command Complete Interrupt Pending
		 * RO only - clear by writing 1 to the Command Completion
		 * Detect bit in Controller SERR-INT register
		 */
		/* [한국어] Controller SERR-INT 레지스터를 다시 읽는다. 이때 bit16(Command Completion Detect)은 1 로 읽힌다 */
		serr_int = shpc_readl(ctrl, SERR_INTR_ENABLE);
		/* [한국어] RsvdZ(예약, 반드시 0으로 써야 하는) 비트 0xfffc0000 를 떨어뜨린다. 규격상 이 비트에 1 을 쓰면 안 되기 때문이다 */
		serr_int &= ~SERR_INTR_RSVDZ_MASK;
		/* [한국어] 읽은 값을 그대로 되쓴다. bit16 은 RW1C(1 을 쓰면 지워짐)이므로 이 되쓰기 한 번으로 Command Completion Detect 가 지워진다 — 별도 클리어 코드가 없는 이유다 */
		shpc_writel(ctrl, SERR_INTR_ENABLE, serr_int);

		/* [한국어] ctrl->queue 대기열에서 자고 있던 shpc_wait_cmd() 를 깨운다. 명령을 낸 프로세스 컨텍스트가 여기서 비로소 진행한다 */
		wake_up_interruptible(&ctrl->queue);
	}

	/* [한국어] 명령 완료 비트를 빼고 나머지가 전부 0 이면 슬롯 이벤트는 없다는 뜻이라 슬롯 순회를 건너뛴다 */
	if (!(intr_loc & ~CMD_INTR_PENDING))
		goto out;

	/* [한국어] 컨트롤러가 가진 슬롯 수만큼 순회. num_slots 는 shpc_init() 이 Slot Configuration 레지스터의 Slots Implemented 필드에서 읽어 둔 값이다 */
	for (hp_slot = 0; hp_slot < ctrl->num_slots; hp_slot++) {
		/* To find out which slot has interrupt pending */
		/* [한국어] 이 슬롯의 인터럽트 대기 비트(bit hp_slot+1)가 서 있지 않으면 건너뛴다. bit0 이 명령 완료용이라 슬롯 비트가 1 부터 시작한다 */
		if (!(intr_loc & SLOT_INTR_PENDING(hp_slot)))
			continue;

		/* [한국어] 이 슬롯의 Logical Slot Register(오프셋 0x24 + 4*i) 를 읽는다. 상태 필드와 RW1C 이벤트 래치가 한 워드에 같이 들어 있다 */
		slot_reg = shpc_readl(ctrl, SLOT_REG(hp_slot));
		/* [한국어] 슬롯 번호와 레지스터 원본 값을 디버그 로그로 남긴다 */
		ctrl_dbg(ctrl, "Slot %x with intr, slot register = %x\n",
			 hp_slot, slot_reg);

		/* [한국어] bit19 MRL Sensor Changed — 래치(레버) 개폐가 감지됐다 */
		if (slot_reg & MRL_CHANGE_DETECTED)
			/* [한국어] shpchp_ctrl.c 로 넘겨 이벤트를 워크큐에 태운다. 열림이면 INT_SWITCH_OPEN, 카드가 전원 켜진 채 열렸으면 INT_POWER_FAULT 로 승격된다 */
			shpchp_handle_switch_change(hp_slot, ctrl);

		/* [한국어] bit18 Attention Button Pressed — 사용자가 슬롯 옆 버튼을 눌렀다 */
		if (slot_reg & BUTTON_PRESS_DETECTED)
			/* [한국어] shpchp_ctrl.c 로 넘겨 INT_BUTTON_PRESS 이벤트를 워크큐에 태운다. 5초 블링킹/취소 로직은 거기서 처리한다 */
			shpchp_handle_attention_button(hp_slot, ctrl);

		/* [한국어] bit16 Presence Detect Changed — 카드가 꽂히거나 빠졌다 */
		if (slot_reg & PRSNT_CHANGE_DETECTED)
			/* [한국어] shpchp_ctrl.c 로 넘겨 INT_PRESENCE_ON/OFF 이벤트를 워크큐에 태운다 */
			shpchp_handle_presence_change(hp_slot, ctrl);

		/* [한국어] bit17 Isolated Power Fault 또는 bit20 Connected Power Fault — 슬롯 전원 결함이 감지됐다. 둘 중 하나라도 서면 같은 핸들러로 보낸다 */
		if (slot_reg & (ISO_PFAULT_DETECTED | CON_PFAULT_DETECTED))
			/* [한국어] shpchp_ctrl.c 로 넘겨 전원 결함 발생/해제 이벤트를 워크큐에 태운다 */
			shpchp_handle_power_fault(hp_slot, ctrl);

		/* Clear all slot events */
		/* [한국어] 되쓰기 전에 RsvdZ 비트(bit15 와 bit23:21)를 떨어뜨린다 — 규격상 0 으로 써야 한다 */
		slot_reg &= ~SLOT_REG_RSVDZ_MASK;
		/* [한국어] 읽은 값을 그대로 되쓴다. bit16~20 의 이벤트 검출 비트가 RW1C 라 이 한 번의 쓰기로 이번에 처리한 이벤트 래치가 모두 지워진다 */
		shpc_writel(ctrl, SLOT_REG(hp_slot), slot_reg);
	}
 out:
	/* [한국어] 인터럽트 모드였다면 위에서 세워 둔 Global Interrupt Mask 를 다시 풀어야 한다 */
	if (!shpchp_poll_mode) {
		/* Unmask Global Interrupt Mask */
		/* [한국어] Controller SERR-INT 레지스터를 다시 읽고 */
		serr_int = shpc_readl(ctrl, SERR_INTR_ENABLE);
		/* [한국어] Global Interrupt Mask(bit0)와 RsvdZ 비트를 함께 떨어뜨린다 — 마스크 해제와 예약 비트 0 유지를 한 번에 */
		serr_int &= ~(GLOBAL_INTR_MASK | SERR_INTR_RSVDZ_MASK);
		/* [한국어] 되써서 인터럽트를 다시 연다 */
		shpc_writel(ctrl, SERR_INTR_ENABLE, serr_int);
	}

	/* [한국어] 여기까지 왔다는 것은 INTR_LOC 에 최소 한 비트가 서 있었다는 뜻이므로 이 핸들러가 처리했다고 알린다 */
	return IRQ_HANDLED;
}

/* [한국어] shpc_get_max_bus_speed - SHPC 가 광고하는 슬롯 능력에서 이 세그먼트의 최대 버스 속도를 구해 pci_bus 에 기록한다
 * 
 * @ctrl: shpc_init() 이 creg 매핑까지 끝낸 컨트롤러 서술자.
 * @return: 0 이면 성공. 어떤 속도 비트도 서 있지 않아 판정 불가면 -ENODEV.
 * 
 * SHPC 는 "이 세그먼트가 지원하는 속도별 슬롯 개수"를 Slot Available Register I/II
 * 두 워드에 비트 필드로 광고한다. 각 필드가 0 이 아니면 그 속도를 낼 수 있는 슬롯이
 * 존재한다는 뜻이므로, 빠른 쪽부터 훑어 처음으로 0 이 아닌 필드를 최대 속도로 삼는다.
 * Programming Interface(PI)가 2 일 때만 PCI-X 266/533 필드가 유효하므로 먼저 PI 를 본다.
 * 
 * 동작 단계: (1) PI 를 읽는다. (2) PI==2 면 Slot Available II 의 533 → 266 순으로
 * 검사. (3) 아직 못 정했으면 Slot Available I 의 133/100/66 PCI-X → 66MHz(II) →
 * 33MHz 순으로 검사. (4) 결과를 bus->max_bus_speed 에 기록한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(shpc_init 안에서 한 번만). 동시 호출은 없다.
 * 에러 경로: 아무 비트도 없으면 -ENODEV 를 돌려주지만 shpc_init() 은 이 반환값을
 * 검사하지 않으므로, 실패해도 max_bus_speed 가 PCI_SPEED_UNKNOWN 으로 남을 뿐이다.
 * 
 * 호출 체인:
 *   shpc_init() → [shpc_get_max_bus_speed] → shpc_readb/shpc_readl (MMIO 읽기) */
static int shpc_get_max_bus_speed(struct controller *ctrl)
{
	/* [한국어] 기본값 0(성공). 아래에서 속도를 하나도 못 정했을 때만 -ENODEV 로 덮인다 */
	int retval = 0;
	/* [한국어] SHPC 가 관장하는 세컨더리 버스. 이 브리지 아래에 매달린 PCI 버스 객체다 */
	struct pci_bus *bus = ctrl->pci_dev->subordinate;
	/* [한국어] 판정 결과 초깃값 — 아직 모른다는 뜻의 PCI_SPEED_UNKNOWN 으로 시작한다 */
	enum pci_bus_speed bus_speed = PCI_SPEED_UNKNOWN;
	/* [한국어] Programming Interface 레지스터(오프셋 0x13). 1 이면 PCI-X 133 까지, 2 면 PCI-X 266/533 까지 표현하는 확장 인코딩이다 */
	u8 pi = shpc_readb(ctrl, PROG_INTERFACE);
	/* [한국어] Slot Available Register I(오프셋 0x04). 33MHz[4:0], PCI-X 66[12:8], 100[20:16], 133[28:24] 슬롯 개수 */
	u32 slot_avail1 = shpc_readl(ctrl, SLOT_AVAIL1);
	/* [한국어] Slot Available Register II(오프셋 0x08). 66MHz 통상[4:0], PCI-X 266 의 66/100/133[11:8]/[15:12]/[19:16], PCI-X 533 의 66/100/133[23:20]/[27:24]/[31:28] */
	u32 slot_avail2 = shpc_readl(ctrl, SLOT_AVAIL2);

	/* [한국어] PI 가 2 일 때만 Slot Available II 의 266/533 필드가 정의된다 — PI==1 컨트롤러에서 이 필드를 읽으면 의미 없는 값이다 */
	if (pi == 2) {
		/* [한국어] PCI-X 533MHz 를 낼 수 있는 133MHz 슬롯이 하나라도 있는가 — 가장 빠른 조합부터 본다 */
		if (slot_avail2 & SLOT_133MHZ_PCIX_533)
			bus_speed = PCI_SPEED_133MHz_PCIX_533;
		/* [한국어] 다음으로 빠른 PCI-X 533 의 100MHz 슬롯 */
		else if (slot_avail2 & SLOT_100MHZ_PCIX_533)
			bus_speed = PCI_SPEED_100MHz_PCIX_533;
		/* [한국어] 그다음 PCI-X 533 의 66MHz 슬롯 */
		else if (slot_avail2 & SLOT_66MHZ_PCIX_533)
			bus_speed = PCI_SPEED_66MHz_PCIX_533;
		/* [한국어] 533 이 없으면 PCI-X 266 의 133MHz 슬롯 */
		else if (slot_avail2 & SLOT_133MHZ_PCIX_266)
			bus_speed = PCI_SPEED_133MHz_PCIX_266;
		/* [한국어] PCI-X 266 의 100MHz 슬롯 */
		else if (slot_avail2 & SLOT_100MHZ_PCIX_266)
			bus_speed = PCI_SPEED_100MHz_PCIX_266;
		/* [한국어] PCI-X 266 의 66MHz 슬롯 */
		else if (slot_avail2 & SLOT_66MHZ_PCIX_266)
			bus_speed = PCI_SPEED_66MHz_PCIX_266;
	}

	/* [한국어] PI==1 이었거나 II 에서 아무것도 못 찾았으면 Slot Available I 로 내려간다 */
	if (bus_speed == PCI_SPEED_UNKNOWN) {
		/* [한국어] PCI-X 133MHz 슬롯 */
		if (slot_avail1 & SLOT_133MHZ_PCIX)
			bus_speed = PCI_SPEED_133MHz_PCIX;
		/* [한국어] PCI-X 100MHz 슬롯 */
		else if (slot_avail1 & SLOT_100MHZ_PCIX)
			bus_speed = PCI_SPEED_100MHz_PCIX;
		/* [한국어] PCI-X 66MHz 슬롯 */
		else if (slot_avail1 & SLOT_66MHZ_PCIX)
			bus_speed = PCI_SPEED_66MHz_PCIX;
		/* [한국어] 통상 PCI 66MHz 는 Register I 이 아니라 Register II 의 [4:0] 에 들어 있다 — 규격상 필드 위치가 그렇다 */
		else if (slot_avail2 & SLOT_66MHZ)
			bus_speed = PCI_SPEED_66MHz;
		/* [한국어] 마지막으로 통상 PCI 33MHz 슬롯 */
		else if (slot_avail1 & SLOT_33MHZ)
			bus_speed = PCI_SPEED_33MHz;
		else
			/* [한국어] 어떤 속도 비트도 서 있지 않다 — 능력 레지스터가 비어 있는 비정상 컨트롤러이므로 -ENODEV */
			retval = -ENODEV;
	}

	/* [한국어] 판정 결과를 PCI 코어의 버스 객체에 기록한다. /sys/bus/pci/... 의 속도 표시와 자식 장치 설정이 이 값을 참조한다 */
	bus->max_bus_speed = bus_speed;
	/* [한국어] 결정된 최대 속도를 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Max bus speed = %d\n", bus_speed);

	return retval;
}

/* [한국어] shpc_init - SHPC Working Register Set 을 찾아 매핑하고 컨트롤러를 인터럽트까지 살려 낸다
 * 
 * @ctrl: shpchp_core.c 의 shpc_probe() 가 kzalloc 으로 갓 할당한 빈 컨트롤러 서술자.
 *        이 함수가 pci_dev, cap_offset, mmio_base/size, creg, num_slots, first_slot,
 *        slot_device_offset, slot_num_inc, 뮤텍스, 대기열, 타이머를 모두 채워 준다.
 * @pdev: SHPC 를 품고 있는 PCI-to-PCI 브리지의 pci_dev. 슬롯이 아니라 브리지다.
 * @return: 0 이면 성공. 실패 시 음수 또는 -1(이 함수는 표준 errno 로 통일돼 있지 않다).
 * 
 * 이 함수가 없으면 드라이버는 SHPC 레지스터가 메모리 어디에 있는지조차 알 수 없다.
 * SHPC 는 레지스터 묶음을 브리지의 BAR0 기준 오프셋에 두고, 그 오프셋을 PCI capability
 * (ID 0x0C) 안의 간접 접근 창(DWORD Select/DWORD Data)으로 알려 준다. 그래서 먼저
 * config 공간으로 base offset 을 캐낸 뒤에야 MMIO 를 ioremap 할 수 있다.
 * 
 * 동작 단계: (1) AMD GOLAM 7450 은 base offset 을 쓰지 않는 예외라 BAR0 전체를 쓴다.
 * (2) 그 외에는 SHPC capability 를 찾아 간접 읽기로 index 0(base offset)과
 * index 3(Slot Configuration)을 읽고, 디버그용으로 9+슬롯수 개의 dword 를 훑는다
 * (Working Register Set 이 0x00~0x20 의 9 dword + 슬롯당 1 dword 이기 때문이다).
 * (3) pci_enable_device → request_mem_region → ioremap 으로 creg 를 얻는다.
 * (4) 뮤텍스 2개와 명령 완료 대기열을 초기화한다. (5) MMIO 로 Slot Configuration 을
 * 다시 읽어 슬롯 수/첫 device 번호/물리 슬롯 번호/번호 증감 방향을 채운다.
 * (6) 모든 인터럽트를 일단 막고, MSI(실패 시 INTx)로 핸들러를 걸거나 폴링 타이머를
 * 세운 뒤, 마지막에 슬롯 이벤트 인터럽트를 열어 준다 — 핸들러를 걸기 전에 인터럽트가
 * 올라오는 창을 없애기 위한 순서다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 probe 경로). 컨트롤러당 한 번만 불린다.
 * 에러 경로: abort 로 점프해 rc 를 그대로 반환하며, ioremap 이후 실패는
 * abort_iounmap 에서 매핑만 해제한다. 주의할 점은 abort_iounmap 경로가
 * release_mem_region() 을 부르지 않아 예약한 MMIO 영역이 남는다는 것이다 —
 * 이 트리의 코드가 실제로 그렇게 되어 있다(상류 코드 그대로이며 여기서 고치지 않는다).
 * 
 * 호출 체인:
 *   shpc_probe() (shpchp_core.c) → [shpc_init]
 *   → pci_find_capability / shpc_indirect_read / request_mem_region / ioremap
 *   → shpc_get_max_bus_speed / shpc_get_cur_bus_speed
 *   → request_irq(shpc_isr) 또는 timer_setup + start_int_poll_timer */
int shpc_init(struct controller *ctrl, struct pci_dev *pdev)
{
	/* [한국어] rc 를 -1 로 시작해 두어, 아래 어느 지점에서 abort 로 뛰든 실패값이 반환되게 한다. num_slots 는 간접 읽기로 알아낸 슬롯 수(MMIO 매핑 크기 계산용) */
	int rc = -1, num_slots = 0;
	/* [한국어] 슬롯 순회용 8비트 인덱스 */
	u8 hp_slot;
	/* [한국어] BAR0 기준 SHPC Working Register Set 의 시작 오프셋(간접 읽기 index 0) */
	u32 shpc_base_offset;
	/* [한국어] tempdword: 간접/MMIO 읽기 임시값, slot_reg: Logical Slot Register 값, slot_config: Slot Configuration 레지스터 값 */
	u32 tempdword, slot_reg, slot_config;
	/* [한국어] 간접 읽기 디버그 루프용 인덱스 */
	u8 i;

	/* [한국어] 컨트롤러 서술자에 브리지의 pci_dev 를 연결한다. 이후 모든 config 접근과 ctrl_dbg 계열 로그가 이 포인터를 쓴다 */
	ctrl->pci_dev = pdev;  /* pci_dev of the P2P bridge */
	/* [한국어] 초기화 시작을 알리는 디버그 로그 */
	ctrl_dbg(ctrl, "Hotplug Controller:\n");

	/* [한국어] AMD GOLAM 7450(0x1022:0x7450) 예외 처리 시작 — 이 칩의 SHPC 구현은 base offset 을 쓰지 않는다 */
	if (pdev->vendor == PCI_VENDOR_ID_AMD &&
	    pdev->device == PCI_DEVICE_ID_AMD_GOLAM_7450) {
		/* amd shpc driver doesn't use Base Offset; assume 0 */
		/* [한국어] 그래서 BAR0 의 시작 물리 주소를 그대로 레지스터 묶음의 시작으로 삼는다 */
		ctrl->mmio_base = pci_resource_start(pdev, 0);
		/* [한국어] 매핑 크기도 BAR0 전체 길이를 쓴다 — 슬롯 수로 계산하지 않는다 */
		ctrl->mmio_size = pci_resource_len(pdev, 0);
	} else {
		/* [한국어] 일반 경로: config 공간에서 SHPC capability(ID 0x0C)의 오프셋을 찾는다. 이것이 간접 접근 창의 기준점이 된다 */
		ctrl->cap_offset = pci_find_capability(pdev, PCI_CAP_ID_SHPC);
		/* [한국어] capability 가 없으면 이 브리지는 SHPC 를 갖고 있지 않다 — 더 진행할 수 없다 */
		if (!ctrl->cap_offset) {
			/* [한국어] 실패 사유를 남기고 */
			ctrl_err(ctrl, "Cannot find PCI capability\n");
			/* [한국어] rc 는 위에서 -1 로 초기화돼 있으므로 그대로 실패 반환된다 */
			goto abort;
		}
		/* [한국어] 찾은 capability 오프셋을 디버그 로그로 남긴다 */
		ctrl_dbg(ctrl, " cap_offset = %x\n", ctrl->cap_offset);

		/* [한국어] 간접 읽기 index 0 = Base Offset 레지스터. BAR0 안에서 Working Register Set 이 시작하는 바이트 오프셋이다 */
		rc = shpc_indirect_read(ctrl, 0, &shpc_base_offset);
		/* [한국어] config 쓰기/읽기 자체가 실패한 경우 — 버스가 끊겼거나 장치가 사라졌다 */
		if (rc) {
			ctrl_err(ctrl, "Cannot read base_offset\n");
			goto abort;
		}

		/* [한국어] 간접 읽기 index 3 = Slot Configuration 레지스터. 아직 MMIO 를 매핑하기 전이라 config 창으로 읽어야 한다 */
		rc = shpc_indirect_read(ctrl, 3, &tempdword);
		if (rc) {
			ctrl_err(ctrl, "Cannot read slot config\n");
			goto abort;
		}
		/* [한국어] Slots Implemented 필드[4:0] 만 뽑아 슬롯 개수를 얻는다. 매핑 크기를 계산하려면 이 값이 필요하다 */
		num_slots = tempdword & SLOT_NUM;
		/* [한국어] 간접 경로로 읽은 슬롯 수를 디버그 로그로 남긴다 */
		ctrl_dbg(ctrl, " num_slots (indirect) %x\n", num_slots);

		/* [한국어] 9 + num_slots 개의 dword 를 전부 훑는다. Working Register Set 의 고정부가 0x00~0x20 즉 9 dword 이고 그 뒤로 슬롯당 1 dword 가 이어지기 때문이다 — 값을 쓰지 않고 덤프만 하는 진단 루프다 */
		for (i = 0; i < 9 + num_slots; i++) {
			/* [한국어] index 를 하나씩 올려 가며 간접 읽기 */
			rc = shpc_indirect_read(ctrl, i, &tempdword);
			/* [한국어] 중간에 하나라도 실패하면 컨트롤러가 정상이 아니다 */
			if (rc) {
				ctrl_err(ctrl, "Cannot read creg (index = %d)\n",
					 i);
				goto abort;
			}
			/* [한국어] 각 오프셋의 값을 디버그 로그로 덤프한다 */
			ctrl_dbg(ctrl, " offset %d: value %x\n", i, tempdword);
		}

		/* [한국어] 매핑 시작 물리 주소 = BAR0 시작 + Base Offset. 이 덧셈이 SHPC 레지스터가 BAR 안에 얹혀 있다는 구조를 그대로 반영한다 */
		ctrl->mmio_base =
			pci_resource_start(pdev, 0) + shpc_base_offset;
		/* [한국어] 매핑 크기 = 고정부 0x24 바이트 + 슬롯당 4 바이트. shpchp.h 의 struct ctrl_reg 가 slot1 을 오프셋 0x24 에 두는 것과 정확히 맞아떨어진다 */
		ctrl->mmio_size = 0x24 + 0x4 * num_slots;
	}

	/* [한국어] 잡은 컨트롤러의 vendor/device/subsystem ID 를 정보 로그로 남긴다 — 버그 리포트에서 어떤 하드웨어인지 식별하는 근거가 된다 */
	ctrl_info(ctrl, "HPC vendor_id %x device_id %x ss_vid %x ss_did %x\n",
		  pdev->vendor, pdev->device, pdev->subsystem_vendor,
		  pdev->subsystem_device);

	/* [한국어] 브리지의 메모리 디코딩을 켠다. 이걸 켜지 않으면 아래 ioremap 으로 만든 매핑에 접근해도 응답이 오지 않는다 */
	rc = pci_enable_device(pdev);
	/* [한국어] 전원 상태 전이나 리소스 문제로 실패할 수 있다 */
	if (rc) {
		ctrl_err(ctrl, "pci_enable_device failed\n");
		goto abort;
	}

	/* [한국어] 커널의 리소스 트리에 이 MMIO 구간을 shpchp 이름으로 예약한다. 다른 드라이버가 같은 구간을 잡는 것을 막는다 */
	if (!request_mem_region(ctrl->mmio_base, ctrl->mmio_size, MY_NAME)) {
		/* [한국어] 이미 다른 드라이버가 잡고 있으면 실패한다 */
		ctrl_err(ctrl, "Cannot reserve MMIO region\n");
		/* [한국어] rc 를 -1 로 명시해 두고(위에서 pci_enable_device 가 0 을 돌려줘 rc 가 0 이 되었을 수 있다) */
		rc = -1;
		goto abort;
	}

	/* [한국어] 예약한 물리 구간을 커널 가상 주소로 매핑한다. 이후 모든 shpc_readX/shpc_writeX 가 이 creg 를 기준으로 오프셋을 더한다 */
	ctrl->creg = ioremap(ctrl->mmio_base, ctrl->mmio_size);
	/* [한국어] 매핑 실패 — 주소 공간이 부족하거나 구간이 잘못됐다 */
	if (!ctrl->creg) {
		ctrl_err(ctrl, "Cannot remap MMIO region %lx @ %lx\n",
			 ctrl->mmio_size, ctrl->mmio_base);
		/* [한국어] 매핑에 실패했으니 방금 잡은 리소스 예약을 되돌린다 — 여기서는 제대로 짝을 맞춘다 */
		release_mem_region(ctrl->mmio_base, ctrl->mmio_size);
		rc = -1;
		goto abort;
	}
	/* [한국어] 매핑된 가상 주소를 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "ctrl->creg %p\n", ctrl->creg);

	/* [한국어] crit_sect: 슬롯 활성/비활성 전체 구간을 감싸는 뮤텍스. shpchp_ctrl.c 의 shpchp_enable_slot/disable_slot 이 잡는다 */
	mutex_init(&ctrl->crit_sect);
	/* [한국어] cmd_lock: SHPC 명령 큐 직렬화용 뮤텍스. Command 레지스터는 한 번에 한 명령만 받으므로 shpc_write_cmd() 전체를 이 락으로 감싼다 */
	mutex_init(&ctrl->cmd_lock);

	/* Setup wait queue */
	/* [한국어] 명령 완료를 기다릴 대기열 초기화. shpc_wait_cmd() 가 여기서 자고 shpc_isr() 이 깨운다 */
	init_waitqueue_head(&ctrl->queue);

	/* Return PCI Controller Info */
	/* [한국어] 이제 MMIO 로 Slot Configuration 레지스터(오프셋 0x0C)를 읽는다. 위 간접 읽기와 같은 레지스터지만 이번엔 정식 경로다 */
	slot_config = shpc_readl(ctrl, SLOT_CONFIG);
	/* [한국어] First Device Number 필드[12:8] — 첫 번째 핫플러그 슬롯이 쓰는 PCI device 번호. 논리 슬롯 번호를 device 번호로 바꿀 때 이 값을 더한다 */
	ctrl->slot_device_offset = (slot_config & FIRST_DEV_NUM) >> 8;
	/* [한국어] Slots Implemented 필드[4:0] — 이 컨트롤러가 관장하는 물리 슬롯 개수 */
	ctrl->num_slots = slot_config & SLOT_NUM;
	/* [한국어] Physical Slot Number 필드[26:16] — 섀시에 인쇄된 첫 슬롯의 번호. sysfs 슬롯 이름을 만들 때 쓴다 */
	ctrl->first_slot = (slot_config & PSN) >> 16;
	/* [한국어] PSN Up/Down 비트(bit29) — 1 이면 슬롯 번호가 증가 방향, 0 이면 감소 방향이다. shpchp_core.c 의 init_slots() 가 이 증감으로 슬롯 이름을 만든다 */
	ctrl->slot_num_inc = ((slot_config & UPDOWN) >> 29) ? 1 : -1;

	/* Mask Global Interrupt Mask & Command Complete Interrupt Mask */
	/* [한국어] Controller SERR-INT 레지스터를 읽어 현재 마스크 상태를 확인한다 */
	tempdword = shpc_readl(ctrl, SERR_INTR_ENABLE);
	/* [한국어] 변경 전 값을 로그로 남긴다 */
	ctrl_dbg(ctrl, "SERR_INTR_ENABLE = %x\n", tempdword);
	/* [한국어] Global Interrupt Mask(bit0), Global SERR Mask(bit1), Command Completion Interrupt Mask(bit2), Arbiter SERR Mask(bit3) 를 모두 세워 인터럽트와 SERR 를 전부 막는다. 핸들러를 아직 걸지 않았으므로 이 순서가 중요하다 */
	tempdword |= (GLOBAL_INTR_MASK  | GLOBAL_SERR_MASK |
		      COMMAND_INTR_MASK | ARBITER_SERR_MASK);
	/* [한국어] RsvdZ 비트는 0 으로 써야 하므로 떨어뜨린다 */
	tempdword &= ~SERR_INTR_RSVDZ_MASK;
	/* [한국어] 마스크를 세운 값을 되쓴다 */
	shpc_writel(ctrl, SERR_INTR_ENABLE, tempdword);
	/* [한국어] 쓰기가 반영됐는지 다시 읽어 */
	tempdword = shpc_readl(ctrl, SERR_INTR_ENABLE);
	/* [한국어] 로그로 확인한다 */
	ctrl_dbg(ctrl, "SERR_INTR_ENABLE = %x\n", tempdword);

	/* Mask the MRL sensor SERR Mask of individual slot in
	 * Slot SERR-INT Mask & clear all the existing event if any
	 */
	/* [한국어] 모든 슬롯에 대해 이벤트 인터럽트와 SERR 를 막고, 남아 있던 이벤트 래치를 지운다 */
	for (hp_slot = 0; hp_slot < ctrl->num_slots; hp_slot++) {
		/* [한국어] 슬롯 i 의 Logical Slot Register 를 읽는다 */
		slot_reg = shpc_readl(ctrl, SLOT_REG(hp_slot));
		/* [한국어] 부팅 시점의 초깃값을 디버그로 남긴다 — 펌웨어가 남긴 상태를 확인하는 근거가 된다 */
		ctrl_dbg(ctrl, "Default Logical Slot Register %d value %x\n",
			 hp_slot, slot_reg);
		/* [한국어] Presence Change(bit24), Isolated Power Fault(bit25), Button Press(bit26), MRL Change(bit27), Connected Power Fault(bit28) 인터럽트 마스크와 MRL Change SERR(bit29), Connected Power Fault SERR(bit30) 마스크를 전부 세운다 */
		slot_reg |= (PRSNT_CHANGE_INTR_MASK | ISO_PFAULT_INTR_MASK |
			     BUTTON_PRESS_INTR_MASK | MRL_CHANGE_INTR_MASK |
			     CON_PFAULT_INTR_MASK   | MRL_CHANGE_SERR_MASK |
			     CON_PFAULT_SERR_MASK);
		/* [한국어] RsvdZ(bit15, bit23:21)를 떨어뜨린다 */
		slot_reg &= ~SLOT_REG_RSVDZ_MASK;
		/* [한국어] 되쓴다. 이때 bit16~20 의 이벤트 검출 비트가 읽은 값 그대로 1 이면 RW1C 로 지워지므로, 마스킹과 기존 이벤트 클리어가 한 번에 이루어진다 */
		shpc_writel(ctrl, SLOT_REG(hp_slot), slot_reg);
	}

	/* [한국어] 폴링 모드면 인터럽트 대신 타이머로 shpc_isr() 을 주기적으로 부른다 */
	if (shpchp_poll_mode) {
		/* Install interrupt polling timer. Start with 10 sec delay */
		/* [한국어] poll_timer 를 int_poll_timeout() 콜백으로 초기화한다 */
		timer_setup(&ctrl->poll_timer, int_poll_timeout, 0);
		/* [한국어] 첫 폴링은 10초 뒤로 잡는다 — 부팅 직후 초기화가 끝날 시간을 준다 */
		start_int_poll_timer(ctrl, 10);
	} else {
		/* Installs the interrupt handler */
		/* [한국어] 인터럽트 모드: MSI 를 먼저 시도한다. MSI 는 전용 인터럽트라 공유 INTx 보다 낫다 */
		rc = pci_enable_msi(pdev);
		/* [한국어] MSI 할당 실패는 치명적이지 않다 */
		if (rc) {
			/* [한국어] MSI 를 못 얻었다고 알리고 */
			ctrl_info(ctrl, "Can't get msi for the hotplug controller\n");
			/* [한국어] 레거시 INTx 로 대신한다고 알린다 — pdev->irq 가 그대로 INTx 번호를 담고 있다 */
			ctrl_info(ctrl, "Use INTx for the hotplug controller\n");
		} else {
			/* [한국어] MSI 성공 시에는 bus master 를 켠다. MSI 는 장치가 메모리 쓰기로 인터럽트를 보내는 방식이라 bus master 권한이 필요하다 */
			pci_set_master(pdev);
		}

		/* [한국어] 핸들러 등록. IRQF_SHARED 는 INTx 로 떨어졌을 때 다른 장치와 IRQ 선을 공유할 수 있기 때문이며, 그래서 shpc_isr() 이 맨 앞에서 INTR_LOC 을 보고 IRQ_NONE 을 돌려줄 수 있어야 한다 */
		rc = request_irq(ctrl->pci_dev->irq, shpc_isr, IRQF_SHARED,
				 MY_NAME, (void *)ctrl);
		/* [한국어] 등록 결과를 IRQ 번호와 함께 디버그로 남긴다 */
		ctrl_dbg(ctrl, "request_irq %d (returns %d)\n",
			 ctrl->pci_dev->irq, rc);
		/* [한국어] 핸들러 등록에 실패하면 이벤트를 받을 방법이 없으므로 초기화를 접는다 */
		if (rc) {
			ctrl_err(ctrl, "Can't get irq %d for the hotplug controller\n",
				 ctrl->pci_dev->irq);
			/* [한국어] ioremap 만 되돌리는 정리 경로로 뛴다 */
			goto abort_iounmap;
		}
	}
	/* [한국어] 여기까지 오면 컨트롤러가 살아 있다 — 장치 이름과 IRQ 번호를 남긴다 */
	ctrl_dbg(ctrl, "HPC at %s irq=%x\n", pci_name(pdev), pdev->irq);

	/* [한국어] Slot Available I/II 에서 이 세그먼트의 최대 속도를 구해 bus->max_bus_speed 에 기록 */
	shpc_get_max_bus_speed(ctrl);
	/* [한국어] Secondary Bus Configuration 에서 현재 동작 속도를 구해 bus->cur_bus_speed 에 기록 */
	shpc_get_cur_bus_speed(ctrl);

	/*
	 * Unmask all event interrupts of all slots
	 */
	/* [한국어] 이제 핸들러가 걸렸으므로 슬롯 이벤트 인터럽트를 열어 준다 — 앞서 막아 둔 것을 되돌리는 단계다 */
	for (hp_slot = 0; hp_slot < ctrl->num_slots; hp_slot++) {
		/* [한국어] 슬롯 i 의 Logical Slot Register 를 다시 읽고 */
		slot_reg = shpc_readl(ctrl, SLOT_REG(hp_slot));
		/* [한국어] 값을 로그로 남긴 뒤 */
		ctrl_dbg(ctrl, "Default Logical Slot Register %d value %x\n",
			 hp_slot, slot_reg);
		/* [한국어] Presence Change/Isolated Power Fault/Button Press/MRL Change/Connected Power Fault 인터럽트 마스크(bit24~28)를 모두 내린다. SERR 마스크(bit29,30)는 세워 둔 채로 남겨 두어 SERR 는 계속 막는다 */
		slot_reg &= ~(PRSNT_CHANGE_INTR_MASK | ISO_PFAULT_INTR_MASK |
			      BUTTON_PRESS_INTR_MASK | MRL_CHANGE_INTR_MASK |
			      CON_PFAULT_INTR_MASK | SLOT_REG_RSVDZ_MASK);
		/* [한국어] 되쓴다. 이 순간부터 이 슬롯의 이벤트가 shpc_isr() 로 올라온다 */
		shpc_writel(ctrl, SLOT_REG(hp_slot), slot_reg);
	}
	/* [한국어] 폴링 모드에서는 컨트롤러 단위 인터럽트를 열 필요가 없다 */
	if (!shpchp_poll_mode) {
		/* Unmask all general input interrupts and SERR */
		/* [한국어] Controller SERR-INT 레지스터를 읽어 */
		tempdword = shpc_readl(ctrl, SERR_INTR_ENABLE);
		/* [한국어] Global Interrupt Mask(bit0)와 Command Completion Interrupt Mask(bit2)를 내린다. Global SERR Mask(bit1)와 Arbiter SERR Mask(bit3)는 세워 둔 채 남아 SERR 는 계속 막힌다 */
		tempdword &= ~(GLOBAL_INTR_MASK | COMMAND_INTR_MASK |
			       SERR_INTR_RSVDZ_MASK);
		/* [한국어] 되써서 인터럽트를 최종적으로 연다 */
		shpc_writel(ctrl, SERR_INTR_ENABLE, tempdword);
		/* [한국어] 반영 결과를 다시 읽어 */
		tempdword = shpc_readl(ctrl, SERR_INTR_ENABLE);
		/* [한국어] 로그로 확인한다 */
		ctrl_dbg(ctrl, "SERR_INTR_ENABLE = %x\n", tempdword);
	}

	/* [한국어] 여기까지 왔으면 성공 — 호출자 shpc_probe() 는 이어서 init_slots() 로 슬롯을 등록한다 */
	return 0;

	/* We end up here for the many possible ways to fail this API.  */
/* [한국어] ioremap 이후 실패한 경로: 매핑만 해제한다. 위 request_mem_region 으로 잡은 예약은 이 경로에서 풀리지 않는다(상류 코드 그대로) */
abort_iounmap:
	/* [한국어] 커널 가상 매핑 해제 */
	iounmap(ctrl->creg);
/* [한국어] ioremap 이전 실패는 되돌릴 것이 없으므로 바로 여기로 온다 */
abort:
	/* [한국어] 실패 사유가 담긴 rc 를 그대로 돌려준다. 호출자는 0 이 아니면 컨트롤러를 해제한다 */
	return rc;
}
