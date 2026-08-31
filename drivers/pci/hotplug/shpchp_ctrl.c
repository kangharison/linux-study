// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 */
/* [한국어] 설명 SHPC(Standard Hot-Plug Controller) 핫플러그 드라이버의 슬롯 상태 기계 (shpchp_ctrl.c)
 * 
 * === 파일의 역할 ===
 * shpchp 드라이버에서 "언제 슬롯을 켜고 끌 것인가"를 결정하는 정책 계층이다. 레지스터를
 * 직접 만지는 코드는 여기에 하나도 없고, 모든 하드웨어 조작은 shpchp_hpc.c 의 함수를
 * 불러 위임한다. 하는 일은 네 가지다. (1) shpc_isr() 이 넘겨준 하드웨어 이벤트를 네 개의
 * shpchp_handle_ 함수가 받아 이벤트 종류를 판정하고, 인터럽트 컨텍스트에서 잠들 수 없으니
 * struct event_info 를 GFP_ATOMIC 으로 할당해 슬롯 워크큐에 던진다. (2) 워크큐에서 깨어난
 * interrupt_event_handler() 가 이벤트를 상태 기계에 먹인다. (3) 다섯 상태
 * (STATIC / BLINKINGON / BLINKINGOFF / POWERON / POWEROFF)를 오가는 전이 규칙과, Attention
 * 버튼을 누른 뒤 5초간 취소 기회를 주는 유예 로직을 구현한다. (4) 실제 활성화/비활성화
 * 절차인 board_added() 와 remove_board() 를 담아, 전원 인가 → 버스 속도 협상 → 슬롯 연결
 * → 커널 장치 등록이라는 순서를 강제한다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * shpchp 는 세 조각이다 — shpchp_core.c 가 pci_driver 등록과 sysfs 노출을,
 * shpchp_hpc.c 가 MMIO 레지스터와 명령 큐와 인터럽트를, 이 파일이 상태 기계를 맡는다.
 * pciehp 의 pciehp_ctrl.c 와 정확히 같은 자리이며 발상도 같다. 다만 상태 개수가 다르다.
 * pciehp 는 OFF_STATE / ON_STATE / BLINKINGON / BLINKINGOFF / POWERON / POWEROFF 여섯
 * 상태를 쓰지만, shpchp 는 "작업 중이 아님"을 켜짐/꺼짐으로 나누지 않고 STATIC_STATE
 * 하나로 합쳐 다섯 상태만 쓴다(shpchp.h 에서 확인한 사실이다). 대신 지금 켜져 있는지는
 * 그때그때 shpchp_get_power_status() 로 하드웨어에 물어본다. 5초 유예와 버튼 재입력
 * 취소라는 사용자 경험은 두 드라이버가 같다. 실행 컨텍스트는 세 갈래다 —
 * shpchp_handle_ 네 함수는 shpc_isr() 안에서 실행되므로 인터럽트 컨텍스트(잠들 수 없음),
 * interrupt_event_handler 와 shpchp_queue_pushbutton_work 와 shpchp_pushbutton_thread 는
 * 슬롯 전용 워크큐(프로세스 컨텍스트), shpchp_sysfs_enable_slot / shpchp_sysfs_disable_slot
 * 은 sysfs 에 쓰기를 한 사용자 프로세스 컨텍스트다.
 * 
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽: shpchp_hpc.c 의 상태 읽기 6종(shpchp_get_power_status /
 * get_attention_status / get_latch_status / get_adapter_status / get_adapter_speed /
 * query_power_fault)과 제어 5종(shpchp_power_on_slot / shpchp_slot_enable /
 * shpchp_slot_disable / shpchp_set_attention_status / shpchp_green_led_ 계열 /
 * shpchp_set_bus_speed_mode), shpchp_pci.c 의 shpchp_configure_device /
 * shpchp_unconfigure_device, shpchp.h 의 shpchp_find_slot 과 AMD POGO 에라타 인라인 함수
 * 두 개, 그리고 커널의 워크큐/뮤텍스/slab 계층이다. 이 파일에 의존하는 쪽: shpchp_hpc.c 의
 * shpc_isr() 이 shpchp_handle_ 네 함수를 부르고, shpchp_core.c 의 sysfs 콜백
 * enable_slot/disable_slot 이 shpchp_sysfs_enable_slot/shpchp_sysfs_disable_slot 을 부르며,
 * 같은 파일의 init_slots() 가 INIT_DELAYED_WORK 으로 shpchp_queue_pushbutton_work 를
 * 슬롯의 지연 작업 핸들러로 걸어 둔다. 데이터 흐름은 "하드웨어 이벤트 → shpc_isr →
 * shpchp_handle_ 계열 → event_info 를 워크큐에 적재 → interrupt_event_handler → 상태 전이
 * → (5초 뒤) shpchp_queue_pushbutton_work → shpchp_pushbutton_thread →
 * shpchp_enable_slot/disable_slot → board_added/remove_board → shpchp_hpc.c 명령 발행"
 * 이라는 한 줄기와, 그와 나란히 놓인 "sysfs 쓰기 → shpchp_sysfs_ 계열 → 같은
 * shpchp_enable_slot/disable_slot" 이라는 지름길 두 갈래다. 공유 상태는 struct slot 의
 * state(다섯 상태), status(0x00 정상 / 0x01 종료 중 / 0xFF 전원 결함), is_a_board,
 * pwr_save / presence_save / latch_save / attention_save 캐시 네 개이며, 슬롯 단위
 * 직렬화는 p_slot->lock 뮤텍스가, 컨트롤러 단위 직렬화는 ctrl->crit_sect 뮤텍스가 맡는다.
 * 
 * NVMe 와의 관계를 분명히 해 둔다. 이 파일은 물론 shpchp 전체가 drivers/nvme 의 함수를
 * 한 번도 부르지 않는다(이 트리에서 확인했다). 관계는 한 방향뿐이다 —
 * board_added() 안의 shpchp_configure_device() 가 PCI 버스를 재스캔하면 PCI 코어가
 * 드라이버를 매칭하고, 꽂힌 카드가 NVMe 라면 그 결과로 nvme_probe() 가 불린다. 반대로
 * remove_board() 안의 shpchp_unconfigure_device() 가 장치를 떼면 nvme_remove() 가 불린다.
 * 더구나 SHPC 는 PCI/PCI-X 시절 규격이고 NVMe 는 PCIe 전용이므로 실제 시스템에서 이 둘이
 * 만나는 경우 자체가 거의 없다.
 * 
 * === 주요 함수/구조체 요약 ===
 * - shpchp_handle_attention_button / switch_change / presence_change / power_fault:
 *   인터럽트 컨텍스트에서 이벤트 종류를 판정해 워크큐에 넘기는 네 진입점.
 * - queue_interrupt_event(): event_info 를 GFP_ATOMIC 으로 할당해 슬롯 워크큐에 적재.
 * - interrupt_event_handler(): 워크큐에서 이벤트를 꺼내 상태 기계에 먹이는 분배기.
 *   실제로 특별 취급하는 것은 버튼 눌림과 전원 결함 둘뿐이고 나머지는 캐시 갱신만 한다.
 * - handle_button_press_event(): 다섯 상태 전이의 중심. 5초 유예를 걸거나 취소한다.
 * - shpchp_queue_pushbutton_work() / shpchp_pushbutton_thread(): 5초 만료 후 상태를
 *   승격시키고 실제 켜기/끄기를 수행하는 2단 워크 구조.
 * - board_added() / remove_board(): 전원·속도·연결·장치등록의 실제 절차와 그 역순.
 * - fix_bus_speed() / change_bus_speed(): 카드 속도와 버스 속도를 맞추는 정책.
 * - shpchp_enable_slot() / shpchp_disable_slot(): 전제 조건 검사 + crit_sect 직렬화.
 * - shpchp_sysfs_enable_slot() / shpchp_sysfs_disable_slot(): sysfs 요청을 상태 기계에
 *   안전하게 태우는 바깥 껍데기.
 * 구조체는 pushbutton_work_info 하나만 새로 정의하고, 나머지 struct slot / struct
 * controller / struct event_info 는 shpchp.h 의 것을 그대로 쓴다. */

/* [한국어] 모듈 빌드 시 shpchp.h 의 MY_NAME 이 THIS_MODULE 을 참조하므로 필요하다 */
#include <linux/module.h>
/* [한국어] container_of 등 커널 공용 매크로를 얻기 위해 포함한다 — 워크 핸들러가 work 포인터에서 바깥 구조체를 역산할 때 쓴다 */
#include <linux/kernel.h>
/* [한국어] u8/u32 같은 고정 폭 정수 타입 */
#include <linux/types.h>
/* [한국어] kmalloc_obj()/kfree() — 이벤트 객체와 푸시버튼 워크 객체를 힙에 할당하기 위해 필요하다 */
#include <linux/slab.h>
/* [한국어] struct pci_dev/struct pci_bus, list_empty 로 자식 장치 목록을 보는 일, pci_domain_nr(), PCI_VENDOR_ID_AMD 상수를 얻는다 */
#include <linux/pci.h>
/* [한국어] PCI 코어 내부 헤더. 다만 이 파일이 이 헤더에서만 정의되는 심볼을 쓰는 곳은 이 트리에서 찾지 못했다 — 과거 코드의 잔재로 보이며 확인된 근거가 없으므로 단정하지 않는다 */
#include "../pci.h"
/* [한국어] 이 드라이버의 공용 헤더. struct slot/struct controller/struct event_info, 다섯 상태 상수와 INT_ 이벤트 상수, shpchp_find_slot(), AMD POGO 에라타 인라인 함수, ctrl_dbg 계열 로그 매크로를 모두 여기서 얻는다 */
#include "shpchp.h"

/* [한국어] interrupt_event_handler 전방 선언 — 아래 queue_interrupt_event() 가 정의보다 먼저 이 함수를 워크 핸들러로 연결하기 때문이다 */
static void interrupt_event_handler(struct work_struct *work);
/* [한국어] shpchp_enable_slot 전방 선언 — shpchp_pushbutton_thread() 가 정의보다 먼저 부른다 */
static int shpchp_enable_slot(struct slot *p_slot);
/* [한국어] shpchp_disable_slot 전방 선언 — 같은 이유다 */
static int shpchp_disable_slot(struct slot *p_slot);

/* [한국어] queue_interrupt_event - 인터럽트가 감지한 이벤트를 워크큐에 실어 프로세스 컨텍스트로 넘긴다
 * 
 * @p_slot: 이벤트가 발생한 슬롯.
 * @event_type: shpchp.h 가 정의한 INT_ 상수(INT_PRESENCE_ON/OFF, INT_SWITCH_CLOSE/OPEN,
 *              INT_POWER_FAULT, INT_POWER_FAULT_CLEAR, INT_BUTTON_PRESS).
 * @return: 0 이면 적재 성공, -ENOMEM 이면 메모리 부족으로 이벤트를 버렸다. 다만 네
 *          호출자 모두 이 반환값을 검사하지 않으므로, 메모리 부족 시 이벤트는 조용히
 *          사라진다 — 이 트리의 코드가 실제로 그렇게 되어 있다.
 * 
 * 인터럽트 컨텍스트와 프로세스 컨텍스트를 잇는 다리다. shpc_isr() 은 잠들 수 없는데
 * 이벤트 처리는 SHPC 명령을 내며 최대 1초씩 잠들어야 하므로, 인터럽트 쪽에서는 이벤트
 * 종류만 기록해 큐에 넣고 실제 처리는 워크큐에 맡긴다.
 * 
 * 동작 단계: (1) event_info 를 GFP_ATOMIC 으로 할당한다 — 인터럽트 컨텍스트라 잠들 수
 * 있는 GFP_KERNEL 을 쓸 수 없다. (2) 이벤트 종류와 슬롯을 담는다. (3) 핸들러를
 * interrupt_event_handler 로 연결한다. (4) 슬롯 전용 워크큐에 넣는다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트(shpc_isr 안). 잠들지 않는다. 폴링 모드에서는 타이머
 * 소프트IRQ 컨텍스트이며 그때도 잠들 수 없는 것은 마찬가지다.
 * 에러 경로: 할당 실패 시 -ENOMEM 을 돌려주지만 호출자가 무시한다.
 * 
 * 호출 체인:
 *   shpc_isr() → shpchp_handle_attention_button / switch_change / presence_change /
 *   power_fault → [queue_interrupt_event] → queue_work(슬롯 워크큐)
 *   → interrupt_event_handler() */
static int queue_interrupt_event(struct slot *p_slot, u32 event_type)
{
	/* [한국어] 워크큐에 실어 보낼 이벤트 서술자 */
	struct event_info *info;

	/* [한국어] GFP_ATOMIC 으로 할당한다 — 인터럽트 컨텍스트라 잠들 수 없기 때문이며, 그래서 메모리 압박 상황에서는 실패할 수 있다 */
	info = kmalloc_obj(*info, GFP_ATOMIC);
	/* [한국어] 할당에 실패하면 */
	if (!info)
		/* [한국어] 이벤트를 버리고 -ENOMEM 을 알린다(호출자는 이 값을 보지 않는다) */
		return -ENOMEM;

	/* [한국어] 어떤 이벤트인지 기록한다. interrupt_event_handler() 가 이 값으로 분기한다 */
	info->event_type = event_type;
	/* [한국어] 어느 슬롯의 이벤트인지 기록한다 */
	info->p_slot = p_slot;
	/* [한국어] 워크 항목을 초기화하고 핸들러를 interrupt_event_handler 로 연결한다 */
	INIT_WORK(&info->work, interrupt_event_handler);

	/* [한국어] 슬롯 전용 워크큐에 넣는다. 슬롯마다 큐가 따로라 한 슬롯의 느린 처리가 다른 슬롯을 막지 않는다 */
	queue_work(p_slot->wq, &info->work);

	/* [한국어] 적재 성공을 알린다 */
	return 0;
}

/* [한국어] shpchp_handle_attention_button - Attention 버튼 눌림 인터럽트를 이벤트로 바꿔 큐에 넣는다
 * 
 * @hp_slot: SHPC 가 세는 0 기반 논리 슬롯 번호. shpc_isr() 이 Interrupt Locator 의
 *           비트 위치에서 뽑아 넘긴다.
 * @ctrl: 이벤트가 올라온 컨트롤러.
 * @return: 항상 0. 나머지 세 형제 함수는 1 을 돌려주지만 이 함수만 0 이며, shpc_isr()
 *          은 어느 쪽도 검사하지 않으므로 실질적인 차이는 없다(상류 코드 그대로다).
 * 
 * 버튼은 눌린 순간에만 알 수 있는 정보라 인터럽트로 받아야 하지만, 눌림의 의미(켜라인지
 * 꺼라인지 취소하라인지)는 현재 슬롯 상태에 따라 달라지고 그 판정은 잠들 수 있는
 * 컨텍스트에서 해야 한다. 그래서 이 함수는 판정하지 않고 INT_BUTTON_PRESS 라는 사실만
 * 큐에 넣는다.
 * 
 * 동작 단계: (1) 논리 슬롯 번호에 첫 slot device 번호를 더해 struct slot 을 찾는다.
 * (2) 카드 존재 여부를 캐시에 갱신한다. (3) 버튼이 눌렸다고 로그를 남기고 이벤트를
 * 큐에 넣는다.
 * 실행 컨텍스트: 인터럽트 컨텍스트(shpc_isr 안). 잠들지 않는다.
 * 에러 경로: shpchp_find_slot() 은 슬롯을 못 찾으면 오류를 로그로 남기고 NULL 을
 * 돌려주는데, 이 함수는 그 NULL 을 검사하지 않고 곧바로 역참조한다 — 네 형제 함수가
 * 모두 같은 모양이다. 정상 동작에서는 shpc_isr 이 num_slots 범위 안의 번호만 넘기므로
 * 발생하지 않지만, 이 트리의 코드에 그런 검사가 없다는 사실은 그대로 적어 둔다.
 * 
 * 호출 체인:
 *   shpc_isr() (Logical Slot Register 의 bit18 감지)
 *   → [shpchp_handle_attention_button] → shpchp_find_slot / shpchp_get_adapter_status
 *   → queue_interrupt_event(INT_BUTTON_PRESS) */
u8 shpchp_handle_attention_button(u8 hp_slot, struct controller *ctrl)
{
	/* [한국어] 이벤트가 발생한 슬롯 객체 */
	struct slot *p_slot;
	/* [한국어] 큐에 실을 이벤트 종류 */
	u32 event_type;

	/* Attention Button Change */
	/* [한국어] 버튼 인터럽트를 받았다고 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Attention button interrupt received\n");

	/* [한국어] 0 기반 논리 슬롯 번호에 첫 slot device 번호를 더해 PCI device 번호를 만들고, 그 번호로 슬롯 목록에서 struct slot 을 찾는다 */
	p_slot = shpchp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);
	/* [한국어] 카드 존재 여부 캐시를 지금 값으로 갱신한다. 워크큐가 나중에 처리할 때 이 값이 판단 재료가 된다 */
	shpchp_get_adapter_status(p_slot, &p_slot->presence_save);

	/*
	 *  Button pressed - See if need to TAKE ACTION!!!
	 */
	/* [한국어] 어느 슬롯의 버튼이 눌렸는지 사용자에게 알린다 */
	ctrl_info(ctrl, "Button pressed on Slot(%s)\n", slot_name(p_slot));
	/* [한국어] 이벤트 종류를 버튼 눌림으로 정한다. 실제 해석은 handle_button_press_event() 가 상태를 보고 한다 */
	event_type = INT_BUTTON_PRESS;

	/* [한국어] 워크큐에 적재한다. 반환값은 검사하지 않으므로 메모리 부족 시 이벤트가 조용히 사라진다 */
	queue_interrupt_event(p_slot, event_type);

	/* [한국어] 형제 함수들과 달리 0 을 돌려준다. shpc_isr() 이 반환값을 쓰지 않아 동작에는 영향이 없다 */
	return 0;

}

/* [한국어] shpchp_handle_switch_change - MRL(래치) 개폐 인터럽트를 이벤트로 바꾸고 서프라이즈 제거를 가려낸다
 * 
 * @hp_slot: 0 기반 논리 슬롯 번호.
 * @ctrl: 이벤트가 올라온 컨트롤러.
 * @return: 항상 1. shpc_isr() 은 이 값을 검사하지 않는다.
 * 
 * 이 함수에는 판정이 하나 들어 있다. 래치가 열렸을 때, 그 슬롯에 전원이 들어와 있고
 * 카드도 꽂혀 있는 상태라면 사용자가 절차를 밟지 않고 카드를 뽑으려는 것이므로 단순한
 * 래치 열림이 아니라 전원 결함으로 취급한다("Surprise Removal"). 그래야
 * interrupt_event_handler() 가 황색 LED 를 켜고 녹색을 꺼서 위험을 알린다. 그냥
 * INT_SWITCH_OPEN 으로 넘기면 상태 캐시만 갱신되고 아무 경고도 나가지 않는다.
 * 
 * 동작 단계: (1) 슬롯을 찾고 카드 존재 여부 캐시를 갱신한다. (2) 래치 상태를 읽는다.
 * (3) 열렸으면 INT_SWITCH_OPEN, 단 전원과 카드가 모두 있으면 INT_POWER_FAULT 로 승격.
 * (4) 닫혔으면 INT_SWITCH_CLOSE. (5) 큐에 적재한다.
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들지 않는다.
 * 에러 경로: shpchp_find_slot() 의 NULL 반환을 검사하지 않는 것은 형제 함수와 같다.
 * INT_SWITCH_OPEN 과 INT_SWITCH_CLOSE 는 interrupt_event_handler 의 default 로 떨어져
 * 상태 캐시 갱신만 일으킨다 — 래치를 닫았다고 슬롯이 자동으로 켜지지는 않는다.
 * 
 * 호출 체인:
 *   shpc_isr() (Logical Slot Register 의 bit19 감지)
 *   → [shpchp_handle_switch_change] → shpchp_get_adapter_status /
 *     shpchp_get_latch_status → queue_interrupt_event() */
u8 shpchp_handle_switch_change(u8 hp_slot, struct controller *ctrl)
{
	/* [한국어] 이벤트가 발생한 슬롯 객체 */
	struct slot *p_slot;
	/* [한국어] 래치 상태를 받을 임시 변수 */
	u8 getstatus;
	/* [한국어] 큐에 실을 이벤트 종류 */
	u32 event_type;

	/* Switch Change */
	/* [한국어] 래치 인터럽트를 받았다고 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Switch interrupt received\n");

	/* [한국어] 논리 슬롯 번호를 device 번호로 바꿔 슬롯을 찾는다 */
	p_slot = shpchp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);
	/* [한국어] 카드 존재 여부 캐시를 갱신한다. 바로 아래 서프라이즈 제거 판정에 이 값을 쓴다 */
	shpchp_get_adapter_status(p_slot, &p_slot->presence_save);
	/* [한국어] 래치가 열렸는지 닫혔는지 하드웨어에서 읽는다. 1 이면 열림이다 */
	shpchp_get_latch_status(p_slot, &getstatus);
	/* [한국어] 카드 존재와 전원 상태를 함께 남긴다 — 서프라이즈 제거 판정의 근거를 로그로 확인할 수 있게 한다 */
	ctrl_dbg(ctrl, "Card present %x Power status %x\n",
		 p_slot->presence_save, p_slot->pwr_save);

	/* [한국어] 래치가 열린 경우 */
	if (getstatus) {
		/*
		 * Switch opened
		 */
		/* [한국어] 래치가 열렸음을 사용자에게 알린다 */
		ctrl_info(ctrl, "Latch open on Slot(%s)\n", slot_name(p_slot));
		/* [한국어] 기본적으로는 단순 래치 열림 이벤트다 */
		event_type = INT_SWITCH_OPEN;
		/* [한국어] 그런데 전원이 들어와 있고(pwr_save) 카드도 꽂혀 있으면(presence_save) 절차 없이 카드를 뽑으려는 상황이다 */
		if (p_slot->pwr_save && p_slot->presence_save) {
			/* [한국어] 전원 결함으로 승격한다. 그래야 interrupt_event_handler 가 황색 LED 를 켜 위험을 알린다 */
			event_type = INT_POWER_FAULT;
			/* [한국어] 서프라이즈 제거임을 오류 수준으로 기록한다 */
			ctrl_err(ctrl, "Surprise Removal of card\n");
		}
	/* [한국어] 래치가 닫힌 경우 */
	} else {
		/*
		 *  Switch closed
		 */
		/* [한국어] 래치가 닫혔음을 알린다 */
		ctrl_info(ctrl, "Latch close on Slot(%s)\n", slot_name(p_slot));
		/* [한국어] 단순 래치 닫힘 이벤트로 정한다. 이 이벤트만으로 슬롯이 켜지지는 않는다 */
		event_type = INT_SWITCH_CLOSE;
	}

	/* [한국어] 결정된 이벤트를 워크큐에 적재한다 */
	queue_interrupt_event(p_slot, event_type);

	/* [한국어] shpc_isr() 은 이 값을 쓰지 않는다 */
	return 1;
}

/* [한국어] shpchp_handle_presence_change - 카드 삽입/제거 인터럽트를 이벤트로 바꿔 큐에 넣는다
 * 
 * @hp_slot: 0 기반 논리 슬롯 번호.
 * @ctrl: 이벤트가 올라온 컨트롤러.
 * @return: 항상 1. shpc_isr() 은 이 값을 검사하지 않는다.
 * 
 * 카드를 꽂거나 뺐다는 사실 자체를 알리는 함수다. 중요한 점은 이 이벤트가
 * interrupt_event_handler() 의 default 분기로 떨어져 상태 캐시 갱신만 일으킨다는 것이다.
 * 즉 shpchp 는 카드를 꽂았다고 자동으로 전원을 넣지 않는다. 사용자가 Attention 버튼을
 * 누르거나 sysfs 의 power 에 1 을 써야 비로소 슬롯이 켜진다. 이 정책 덕분에 카드가
 * 제대로 자리 잡기 전에 전원이 들어가는 사고를 막을 수 있다.
 * 
 * 동작 단계: (1) 슬롯을 찾는다. (2) 카드 존재 여부를 읽어 캐시에 저장한다. (3) 있으면
 * INT_PRESENCE_ON, 없으면 INT_PRESENCE_OFF 로 정해 큐에 적재한다.
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들지 않는다.
 * 에러 경로: shpchp_find_slot() 의 NULL 반환을 검사하지 않는 것은 형제 함수와 같다.
 * 
 * 호출 체인:
 *   shpc_isr() (Logical Slot Register 의 bit16 감지)
 *   → [shpchp_handle_presence_change] → shpchp_get_adapter_status
 *   → queue_interrupt_event(INT_PRESENCE_ON 또는 INT_PRESENCE_OFF) */
u8 shpchp_handle_presence_change(u8 hp_slot, struct controller *ctrl)
{
	/* [한국어] 이벤트가 발생한 슬롯 객체 */
	struct slot *p_slot;
	/* [한국어] 큐에 실을 이벤트 종류 */
	u32 event_type;

	/* Presence Change */
	/* [한국어] 카드 존재 변화 인터럽트를 받았다고 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Presence/Notify input change\n");

	/* [한국어] 논리 슬롯 번호를 device 번호로 바꿔 슬롯을 찾는다 */
	p_slot = shpchp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	/*
	 * Save the presence state
	 */
	/* [한국어] 카드가 지금 있는지 읽어 캐시에 저장한다. 이 한 번의 읽기가 아래 분기의 유일한 근거다 */
	shpchp_get_adapter_status(p_slot, &p_slot->presence_save);
	/* [한국어] 카드가 있으면 삽입 이벤트다 */
	if (p_slot->presence_save) {
		/*
		 * Card Present
		 */
		/* [한국어] 카드가 꽂혔음을 사용자에게 알린다 */
		ctrl_info(ctrl, "Card present on Slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 삽입 이벤트로 정한다. 다만 이 이벤트만으로 슬롯이 자동으로 켜지지는 않는다 */
		event_type = INT_PRESENCE_ON;
	/* [한국어] 카드가 없으면 제거 이벤트다 */
	} else {
		/*
		 * Not Present
		 */
		/* [한국어] 카드가 빠졌음을 알린다 */
		ctrl_info(ctrl, "Card not present on Slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 제거 이벤트로 정한다. 역시 상태 캐시 갱신만 일으킨다 */
		event_type = INT_PRESENCE_OFF;
	}

	/* [한국어] 결정된 이벤트를 워크큐에 적재한다 */
	queue_interrupt_event(p_slot, event_type);

	/* [한국어] shpc_isr() 은 이 값을 쓰지 않는다 */
	return 1;
}

/* [한국어] shpchp_handle_power_fault - 전원 결함 인터럽트를 이벤트로 바꾸고 슬롯 상태 플래그를 남긴다
 * 
 * @hp_slot: 0 기반 논리 슬롯 번호.
 * @ctrl: 이벤트가 올라온 컨트롤러.
 * @return: 항상 1. shpc_isr() 은 이 값을 검사하지 않는다.
 * 
 * 전원 결함 이벤트는 발생과 해제가 같은 인터럽트 비트로 올라오므로, 지금이 어느 쪽인지
 * Logical Slot Register 의 Power Fault 비트를 다시 읽어 판정해야 한다. 그 비트는 액티브
 * 로우라 shpchp_query_power_fault() 가 뒤집어 돌려준다.
 * 
 * 여기서 p_slot->status 에 0xFF 를 쓰는 것이 중요하다. board_added() 는 슬롯을 켠 뒤
 * msleep(1000)으로 1초를 기다렸다가 이 값이 0xFF 인지 본다. 즉 인터럽트가 남긴 이
 * 플래그가 워크큐를 거치지 않고 활성화 절차에 직접 전달되는 유일한 통로다.
 * 
 * 동작 단계: (1) 슬롯을 찾는다. (2) Power Fault 비트를 읽어 결함 여부를 판정한다.
 * (3) 해제면 status 를 0 으로 지우고 INT_POWER_FAULT_CLEAR, 발생이면 status 를 0xFF 로
 * 세우고 INT_POWER_FAULT 로 정한다. (4) 큐에 적재한다.
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들지 않는다.
 * 에러 경로: shpchp_find_slot() 의 NULL 반환을 검사하지 않는 것은 형제 함수와 같다.
 * p_slot->status 를 락 없이 쓰지만, 읽는 쪽인 board_added() 가 이 슬롯의 활성화를
 * 진행 중인 한 값을 덮어쓰는 다른 경로가 없어 실질적인 경쟁은 일어나지 않는다.
 * 
 * 호출 체인:
 *   shpc_isr() (Logical Slot Register 의 bit17 또는 bit20 감지)
 *   → [shpchp_handle_power_fault] → shpchp_query_power_fault
 *   → queue_interrupt_event(INT_POWER_FAULT 또는 INT_POWER_FAULT_CLEAR) */
u8 shpchp_handle_power_fault(u8 hp_slot, struct controller *ctrl)
{
	/* [한국어] 이벤트가 발생한 슬롯 객체 */
	struct slot *p_slot;
	/* [한국어] 큐에 실을 이벤트 종류 */
	u32 event_type;

	/* Power fault */
	/* [한국어] 전원 결함 인터럽트를 받았다고 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "Power fault interrupt received\n");

	/* [한국어] 논리 슬롯 번호를 device 번호로 바꿔 슬롯을 찾는다 */
	p_slot = shpchp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	/* [한국어] Power Fault 비트를 다시 읽는다. 이 헬퍼는 액티브 로우를 뒤집어 1 이면 결함을 뜻하므로, 부정하면 '결함 없음 = 해제됨'이 된다 */
	if (!(shpchp_query_power_fault(p_slot))) {
		/*
		 * Power fault Cleared
		 */
		/* [한국어] 결함이 해제됐음을 사용자에게 알린다 */
		ctrl_info(ctrl, "Power fault cleared on Slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 슬롯 상태 플래그를 정상(0x00)으로 되돌린다 */
		p_slot->status = 0x00;
		/* [한국어] 해제 이벤트로 정한다. 이 이벤트는 interrupt_event_handler 의 default 로 떨어져 상태 캐시만 갱신한다 */
		event_type = INT_POWER_FAULT_CLEAR;
	/* [한국어] 결함이 새로 발생한 경우 */
	} else {
		/*
		 *   Power fault
		 */
		/* [한국어] 결함 발생을 사용자에게 알린다 */
		ctrl_info(ctrl, "Power fault on Slot(%s)\n", slot_name(p_slot));
		/* [한국어] 결함 이벤트로 정한다. interrupt_event_handler 가 이 이벤트만은 특별 취급해 황색 LED 를 켠다 */
		event_type = INT_POWER_FAULT;
		/* set power fault status for this board */
		/* [한국어] 슬롯 상태 플래그를 0xFF 로 세운다. board_added() 가 활성화 1초 뒤 이 값을 읽어 결함 여부를 판단하는, 인터럽트와 활성화 절차 사이의 직통 신호다 */
		p_slot->status = 0xFF;
		/* [한국어] 어느 슬롯에 결함 비트가 섰는지 남긴다 */
		ctrl_info(ctrl, "Power fault bit %x set\n", hp_slot);
	}

	/* [한국어] 결정된 이벤트를 워크큐에 적재한다 */
	queue_interrupt_event(p_slot, event_type);

	/* [한국어] shpc_isr() 은 이 값을 쓰지 않는다 */
	return 1;
}

/* The following routines constitute the bulk of the
   hotplug controller logic
 */
/* [한국어] change_bus_speed - 버스 세그먼트의 속도를 바꾸고 실패를 shpchp 에러 코드로 번역한다
 * 
 * @ctrl: 로그 출력용 컨트롤러(레지스터 접근은 p_slot 을 통해 이뤄진다).
 * @p_slot: 속도 변경 명령을 낼 통로가 되는 슬롯. 명령 자체는 세그먼트 전체에 걸린다.
 * @speed: 맞추려는 pci_bus_speed 값.
 * @return: 0 이면 성공, WRONG_BUS_FREQUENCY(0x0D)면 실패.
 * 
 * fix_bus_speed() 가 결정한 속도를 실제로 적용하는 한 겹의 래퍼다. 존재 이유는 오로지
 * 에러 코드 번역에 있다 — shpchp_set_bus_speed_mode() 는 -EINVAL 이나 -EIO 같은 errno 를
 * 돌려주는데, 상위 board_added() 는 속도 관련 실패를 WRONG_BUS_FREQUENCY 라는 shpchp
 * 자체 코드로 다루기 때문이다. 이 함수가 그 경계를 맡는다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐 또는 sysfs). 내부에서 SHPC 명령을 내므로
 * 최대 1초 잠들 수 있다. 호출자가 ctrl->crit_sect 를 쥐고 있다.
 * 에러 경로: 실패를 로그로 남기고 WRONG_BUS_FREQUENCY 를 돌려준다.
 * 
 * 호출 체인:
 *   board_added() → fix_bus_speed() → [change_bus_speed]
 *   → shpchp_set_bus_speed_mode() (shpchp_hpc.c) → shpc_write_cmd() */
static int change_bus_speed(struct controller *ctrl, struct slot *p_slot,
		enum pci_bus_speed speed)
{
	/* [한국어] 명령 발행 결과 */
	int rc = 0;

	/* [한국어] 어떤 속도로 바꾸려는지 디버그로 남긴다 */
	ctrl_dbg(ctrl, "Change speed to %d\n", speed);
	/* [한국어] 실제 속도 변경 명령을 발행한다. 성공하면 내부에서 shpc_get_cur_bus_speed() 로 반영값까지 갱신한다 */
	rc = shpchp_set_bus_speed_mode(p_slot, speed);
	/* [한국어] 명령이 실패했다면 */
	if (rc) {
		/* [한국어] 실패를 기록하고 */
		ctrl_err(ctrl, "%s: Issue of set bus speed mode command failed\n",
			 __func__);
		/* [한국어] errno 를 shpchp 자체 코드로 바꿔 올린다 — 상위 board_added() 가 기대하는 형태다 */
		return WRONG_BUS_FREQUENCY;
	}
	/* [한국어] 성공이면 rc 가 0 이므로 그대로 돌려준다 */
	return rc;
}

/* [한국어] fix_bus_speed - 카드 속도와 버스 속도가 어긋났을 때 어떻게 할지 결정한다
 * 
 * @ctrl: 로그 출력용 컨트롤러.
 * @pslot: 속도 변경 명령을 낼 통로가 되는 슬롯.
 * @flag: 같은 버스에 이미 다른 장치가 있으면 1. board_added() 가 자식 장치 목록이
 *        비어 있지 않은지 보고 채운다.
 * @asp: 새로 꽂힌 카드가 감당할 수 있는 최대 속도(adapter speed).
 * @bsp: 지금 버스가 돌고 있는 속도(bus speed).
 * @msp: 이 버스가 낼 수 있는 최대 속도(max speed).
 * @return: 0 이면 조치 완료(또는 조치 불필요), WRONG_BUS_FREQUENCY 면 맞출 수 없다.
 * 
 * PCI/PCI-X 버스는 참가한 모든 장치가 같은 속도로 돌아야 하고, 이미 동작 중인 장치가
 * 있으면 그 장치를 멈추지 않고는 속도를 바꿀 수 없다. 그래서 판단이 두 갈래로 갈린다.
 * 다른 장치가 이미 있으면(flag=1) 속도를 바꿀 수 없으므로, 새 카드가 현재 버스 속도를
 * 감당할 수 있는지만 확인하고 못 하면 거절한다. 슬롯이 비어 있었다면(flag=0) 자유롭게
 * 바꿀 수 있으므로, 카드 속도와 버스 최대 속도 중 느린 쪽에 맞춘다.
 * 
 * 여기서 asp < bsp 같은 비교가 성립하는 것은 enum pci_bus_speed 의 값이 느린 것부터
 * 차례로 정의돼 있다는 전제 덕분이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. change_bus_speed() 를 통해 최대 1초 잠들 수 있다.
 * 호출자가 ctrl->crit_sect 를 쥐고 있다.
 * 에러 경로: 속도 불일치는 WRONG_BUS_FREQUENCY 로 올려 board_added() 가 활성화를
 * 포기하게 만든다.
 * 
 * 호출 체인:
 *   board_added() → [fix_bus_speed] → change_bus_speed()
 *   → shpchp_set_bus_speed_mode() (shpchp_hpc.c) */
static int fix_bus_speed(struct controller *ctrl, struct slot *pslot,
		u8 flag, enum pci_bus_speed asp, enum pci_bus_speed bsp,
		enum pci_bus_speed msp)
{
	/* [한국어] 판정 결과. 기본은 조치 불필요(0) */
	int rc = 0;

	/*
	 * If other slots on the same bus are occupied, we cannot
	 * change the bus speed.
	 */
	/* [한국어] 같은 버스에 이미 다른 장치가 있는 경우 — 속도를 바꾸면 그 장치들이 오동작하므로 바꿀 수 없다 */
	if (flag) {
		/* [한국어] 새 카드가 현재 버스 속도를 감당하지 못하면(카드가 더 느리면) */
		if (asp < bsp) {
			/* [한국어] 버스 속도와 카드 속도가 맞지 않는다고 기록하고 */
			ctrl_err(ctrl, "Speed of bus %x and adapter %x mismatch\n",
				 bsp, asp);
			/* [한국어] 활성화를 거절한다. 카드를 켤 수 없다는 뜻이다 */
			rc = WRONG_BUS_FREQUENCY;
		}
		/* [한국어] 카드가 현재 속도를 감당할 수 있으면 아무것도 하지 않고 0 을 돌려준다. 이 경로는 반드시 여기서 끝나므로 아래 속도 변경 코드에 닿지 않는다 */
		return rc;
	}

	/* [한국어] 여기부터는 버스가 비어 있어 속도를 자유롭게 바꿀 수 있는 경우다. 카드가 버스 최대 속도보다 느리면 */
	if (asp < msp) {
		/* [한국어] 현재 버스 속도가 이미 카드 속도와 같다면 바꿀 필요가 없다 */
		if (bsp != asp)
			/* [한국어] 다르면 카드 속도에 맞춘다 — 버스는 가장 느린 참가자에 맞춰야 하기 때문이다 */
			rc = change_bus_speed(ctrl, pslot, asp);
	/* [한국어] 카드가 버스 최대 속도 이상을 감당할 수 있는 경우 */
	} else {
		/* [한국어] 현재 버스 속도가 이미 최대치라면 바꿀 필요가 없다 */
		if (bsp != msp)
			/* [한국어] 아니면 버스가 낼 수 있는 최대 속도까지 올린다 — 카드가 감당할 수 있으니 성능을 최대로 쓴다 */
			rc = change_bus_speed(ctrl, pslot, msp);
	}
	/* [한국어] 0(조치 완료 또는 불필요) 또는 WRONG_BUS_FREQUENCY 를 board_added() 에 돌려준다 */
	return rc;
}

/**
 * board_added - Called after a board has been added to the system.
 * @p_slot: target &slot
 *
 * Turns power on for the board.
 * Configures board.
 */
/* [한국어] board_added - 슬롯에 전원을 넣고 속도를 맞춘 뒤 커널에 장치를 등록하는 활성화 본체
 * 
 * @p_slot: 활성화할 슬롯. 호출자가 카드 존재/래치 닫힘/전원 꺼짐을 이미 확인했다.
 * @return: 0 이면 성공. -1 은 전원 인가 실패, WRONG_BUS_FREQUENCY(0x0D)는 속도 협상
 *          실패, 그 밖의 값은 shpchp_slot_enable/shpchp_slot_disable 이 돌려준 명령
 *          발행 실패다. errno 와 shpchp 자체 코드가 한 반환값에 섞여 나온다는 점에
 *          주의해야 한다.
 * 
 * 핫플러그로 카드를 켜는 일은 전원만 넣으면 끝나는 것이 아니다. PCI/PCI-X 버스는 참가한
 * 장치 가운데 가장 느린 쪽에 속도를 맞춰야 하므로, 전원을 넣되 버스에는 아직 붙이지 않은
 * Power Only 상태에서 카드의 능력을 읽고 버스 속도를 조정한 다음에야 버스에 연결한다.
 * 이 3단계(Power Only → 속도 조정 → Enable)가 SHPC 활성화 절차의 핵심이며 이 함수의
 * 골격이다.
 * 
 * 동작 단계: (1) shpchp_power_on_slot() 으로 전원만 인가한다. (2) Intel 0x8086:0x0332
 * 브리지는 예외라 먼저 33MHz 로 고정하고 곧바로 Enable 한다. (3) 카드가 낼 수 있는
 * 속도(asp), 현재 버스 속도(bsp), 버스 최대 속도(msp)를 모은다. (4) 같은 버스에 다른
 * 장치가 있는지 확인해 fix_bus_speed() 에 넘긴다. (5) shpchp_slot_enable() 로 버스에
 * 연결한다. (6) 1초 기다린 뒤 전원 결함 플래그를 확인한다. (7) 문제가 없으면
 * shpchp_configure_device() 로 버스를 재스캔해 커널에 장치를 등록한다.
 * 
 * 여기서 중요한 사실 하나 — 이 파일과 shpchp 전체는 NVMe 드라이버를 직접 부르지 않는다.
 * (7) 단계의 버스 재스캔이 PCI 코어의 드라이버 매칭을 일으키고, 꽂힌 카드가 NVMe 라면
 * 그 결과로 nvme_probe() 가 불릴 뿐이다. 관계는 이 한 방향뿐이며, 애초에 SHPC 는
 * PCI/PCI-X 시절 규격이라 PCIe 전용인 NVMe 와는 사실상 무관하다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 ctrl->crit_sect 를 쥔 상태이며, 이 함수는
 * msleep(1000)과 여러 번의 SHPC 명령으로 수 초를 소비한다.
 * 에러 경로: 전원 결함이나 장치 설정 실패는 err_exit 로 내려가 슬롯을 다시 끄고
 * 황색 LED 를 켠다. 속도 협상 실패는 슬롯을 끄지 않고 바로 반환한다 — 경로마다 정리
 * 수준이 다르다는 점이 이 함수의 특징이다.
 * 
 * 호출 체인:
 *   shpchp_enable_slot() → [board_added]
 *   → shpchp_power_on_slot / shpchp_get_adapter_speed / fix_bus_speed /
 *     shpchp_slot_enable (shpchp_hpc.c)
 *   → shpchp_configure_device() (shpchp_pci.c) → pci_scan_slot / pci_bus_add_devices */
static int board_added(struct slot *p_slot)
{
	/* [한국어] 논리 슬롯 번호(0 기반). 아래 디버그 로그에만 쓰인다 */
	u8 hp_slot;
	/* [한국어] 같은 버스에 이미 다른 장치가 있는지 나타내는 플래그. 있으면 버스 속도를 바꿀 수 없다 */
	u8 slots_not_empty = 0;
	/* [한국어] 각 단계의 반환값을 받는 변수 */
	int rc = 0;
	/* [한국어] asp: 꽂힌 카드가 낼 수 있는 속도, bsp: 현재 버스 속도, msp: 버스가 낼 수 있는 최대 속도 */
	enum pci_bus_speed asp, bsp, msp;
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = p_slot->ctrl;
	/* [한국어] 이 브리지의 세컨더리 버스 — 장치 등록 실패 로그에서 도메인 번호를 뽑는 데 쓴다 */
	struct pci_bus *parent = ctrl->pci_dev->subordinate;

	/* [한국어] PCI device 번호에서 첫 슬롯 device 번호를 빼 0 기반 논리 슬롯 번호를 만든다. shpc_init() 이 Slot Configuration 의 First Device Number 로 채워 둔 값이 기준이다 */
	hp_slot = p_slot->device - ctrl->slot_device_offset;

	/* [한국어] device 번호, 오프셋, 논리 슬롯 번호 세 값을 함께 남겨 슬롯 번호 매핑을 추적할 수 있게 한다 */
	ctrl_dbg(ctrl, "%s: p_slot->device, slot_offset, hp_slot = %d, %d ,%d\n",
		 __func__, p_slot->device, ctrl->slot_device_offset, hp_slot);

	/* Power on slot without connecting to bus */
	/* [한국어] 1단계 — Power Only 명령으로 전원만 인가한다. 아직 버스에는 붙이지 않으므로 신호가 오가지 않는다 */
	rc = shpchp_power_on_slot(p_slot);
	/* [한국어] 전원 인가에 실패하면 더 진행할 수 없다 */
	if (rc) {
		/* [한국어] 실패를 기록하고 */
		ctrl_err(ctrl, "Failed to power on slot\n");
		/* [한국어] -1 로 실패를 알린다. 정리(슬롯 끄기)는 하지 않는다 — 애초에 켜지지 않았기 때문이다 */
		return -1;
	}

	/* [한국어] Intel 0x8086:0x0332 브리지 예외 — 이 컨트롤러는 속도를 먼저 33MHz 로 고정하고 Enable 까지 해 두어야 아래의 어댑터 속도 읽기가 정상 동작한다 */
	if ((ctrl->pci_dev->vendor == 0x8086) && (ctrl->pci_dev->device == 0x0332)) {
		/* [한국어] 버스 세그먼트를 통상 PCI 33MHz 로 강제한다 */
		rc = shpchp_set_bus_speed_mode(p_slot, PCI_SPEED_33MHz);
		/* [한국어] 속도 설정 명령이 실패하면 */
		if (rc) {
			/* [한국어] 실패를 기록하고 */
			ctrl_err(ctrl, "%s: Issue of set bus speed mode command failed\n",
				 __func__);
			/* [한국어] 속도 문제로 실패했음을 shpchp 고유 코드로 알린다 */
			return WRONG_BUS_FREQUENCY;
		}

		/* turn on board, blink green LED, turn off Amber LED */
		/* [한국어] 이 예외 경로에서는 여기서 미리 버스 연결까지 해 둔다(Slot Enable + 녹색 깜빡임 + 황색 끄기) */
		rc = shpchp_slot_enable(p_slot);
		/* [한국어] Enable 명령이 실패하면 */
		if (rc) {
			/* [한국어] 실패를 기록하고 */
			ctrl_err(ctrl, "Issue of Slot Enable command failed\n");
			/* [한국어] 명령 발행 실패 코드를 그대로 올린다 */
			return rc;
		}
	}

	/* [한국어] 2단계 — 꽂힌 카드가 감당할 수 있는 최대 속도를 Logical Slot Register 에서 읽는다 */
	rc = shpchp_get_adapter_speed(p_slot, &asp);
	/* [한국어] PI 값이 이상하거나 PCI-X Capability 인코딩이 규격에 없으면 실패한다 */
	if (rc) {
		/* [한국어] 카드 속도를 알 수 없거나 버스 모드가 맞지 않는다고 기록하고 */
		ctrl_err(ctrl, "Can't get adapter speed or bus mode mismatch\n");
		/* [한국어] 속도 문제로 처리한다. 이 경로도 슬롯을 끄지 않고 반환한다 */
		return WRONG_BUS_FREQUENCY;
	}

	/* [한국어] 현재 버스가 돌고 있는 속도. shpc_get_cur_bus_speed() 가 채워 둔 값이다 */
	bsp = ctrl->pci_dev->subordinate->cur_bus_speed;
	/* [한국어] 이 버스가 낼 수 있는 최대 속도. shpc_get_max_bus_speed() 가 채워 둔 값이다 */
	msp = ctrl->pci_dev->subordinate->max_bus_speed;

	/* Check if there are other slots or devices on the same bus */
	/* [한국어] 이 버스에 이미 다른 장치가 매달려 있는지 확인한다. 있다면 속도를 바꾸는 순간 그 장치들이 오동작하므로 바꿀 수 없다 */
	if (!list_empty(&ctrl->pci_dev->subordinate->devices))
		/* [한국어] 다른 장치가 있다는 플래그를 세운다 */
		slots_not_empty = 1;

	/* [한국어] 네 가지 판단 재료를 한 줄에 모아 남긴다 — 속도 협상 실패를 추적하는 핵심 로그다 */
	ctrl_dbg(ctrl, "%s: slots_not_empty %d, adapter_speed %d, bus_speed %d, max_bus_speed %d\n",
		 __func__, slots_not_empty, asp,
		 bsp, msp);

	/* [한국어] 3단계 — 세 속도와 점유 여부를 놓고 버스 속도를 어떻게 할지 결정하고 필요하면 바꾼다 */
	rc = fix_bus_speed(ctrl, p_slot, slots_not_empty, asp, bsp, msp);
	/* [한국어] 속도를 맞출 수 없으면(WRONG_BUS_FREQUENCY) */
	if (rc)
		/* [한국어] 그대로 실패를 올린다. 이 경로도 슬롯을 끄지 않는다 */
		return rc;

	/* turn on board, blink green LED, turn off Amber LED */
	/* [한국어] 4단계 — 이제 버스에 연결한다. Slot Enable + 녹색 LED 깜빡임 + 황색 LED 끄기를 한 명령에 담아 보낸다 */
	rc = shpchp_slot_enable(p_slot);
	/* [한국어] Enable 명령이 실패하면 */
	if (rc) {
		/* [한국어] 실패를 기록하고 */
		ctrl_err(ctrl, "Issue of Slot Enable command failed\n");
		/* [한국어] 명령 발행 실패 코드를 그대로 올린다 */
		return rc;
	}

	/* Wait for ~1 second */
	/* [한국어] 카드 전원과 버스 신호가 안정될 때까지 1초 기다린다. 이 사이에 전원 결함이 생기면 shpc_isr → shpchp_handle_power_fault 가 p_slot->status 를 0xFF 로 만든다 — 아래 검사가 그 값을 본다 */
	msleep(1000);

	/* [한국어] 1초 뒤의 슬롯 상태 플래그를 남긴다 */
	ctrl_dbg(ctrl, "%s: slot status = %x\n", __func__, p_slot->status);
	/* Check for a power fault */
	/* [한국어] 0xFF 는 방금 1초 사이에 전원 결함 이벤트가 왔다는 뜻이다. 인터럽트 핸들러가 남긴 흔적을 여기서 읽는 구조다 */
	if (p_slot->status == 0xFF) {
		/* power fault occurred, but it was benign */
		/* [한국어] 전원 결함이 있었음을 디버그로 남기고 */
		ctrl_dbg(ctrl, "%s: Power fault\n", __func__);
		/* [한국어] 플래그를 지운 뒤 */
		p_slot->status = 0;
		/* [한국어] 슬롯을 다시 끄는 정리 경로로 간다 */
		goto err_exit;
	}

	/* [한국어] 5단계 — PCI 버스를 재스캔해 새 장치를 커널에 등록한다. 이 안에서 pci_scan_slot 과 pci_bus_add_devices 가 불리며, 그 결과로 해당 장치의 드라이버 probe 가 일어난다 */
	if (shpchp_configure_device(p_slot)) {
		/* [한국어] 등록에 실패하면 어느 도메인/버스/device 였는지 남기고 */
		ctrl_err(ctrl, "Cannot add device at %04x:%02x:%02x\n",
			 pci_domain_nr(parent), p_slot->bus, p_slot->device);
		/* [한국어] 슬롯을 다시 끄는 정리 경로로 간다 */
		goto err_exit;
	}

	/* [한국어] 여기까지 왔으면 정상이다 — 상태 플래그를 깨끗이 지운다 */
	p_slot->status = 0;
	/* [한국어] 이 슬롯에 보드가 들어 있다고 표시한다. remove_board() 가 이 플래그를 본다 */
	p_slot->is_a_board = 0x01;
	/* [한국어] 전원이 들어와 있다고 캐시에 기록한다. shpchp_handle_switch_change() 가 이 값으로 서프라이즈 제거를 판정한다 */
	p_slot->pwr_save = 1;

	/* [한국어] 녹색 LED 를 깜빡임에서 상시 점등으로 바꿔 작업 완료를 사람에게 알린다 */
	shpchp_green_led_on(p_slot);

	/* [한국어] 성공을 알린다 */
	return 0;

err_exit:
	/* turn off slot, turn on Amber LED, turn off Green LED */
	/* [한국어] 정리 경로 — Slot Disable + 녹색 LED 끄기 + 황색 LED 켜기를 한 명령으로 보낸다 */
	rc = shpchp_slot_disable(p_slot);
	/* [한국어] 정리 명령마저 실패하면 */
	if (rc) {
		/* [한국어] 그 사실을 기록하고 */
		ctrl_err(ctrl, "%s: Issue of Slot Disable command failed\n",
			 __func__);
		/* [한국어] 정리 실패 코드를 올린다 */
		return rc;
	}

	/* [한국어] 정리에 성공한 경우다. 이때 rc 는 shpchp_slot_disable() 이 돌려준 0 이므로, 활성화가 실패했는데도 0(성공)이 반환된다 — 상류 코드가 그렇게 되어 있으며 여기서 고치지 않는다 */
	return(rc);
}


/**
 * remove_board - Turns off slot and LEDs
 * @p_slot: target &slot
 */
/* [한국어] remove_board - 슬롯에서 장치를 걷어내고 전원을 내리는 비활성화 본체
 * 
 * @p_slot: 비활성화할 슬롯. 호출자가 카드 존재/래치 닫힘/전원 켜짐을 이미 확인했다.
 * @return: 0 이면 성공, 그 밖의 값은 shpchp_slot_disable() 또는
 *          shpchp_set_attention_status() 가 돌려준 명령 발행 실패다.
 * 
 * board_added() 의 역순이다. 순서가 중요한데, 반드시 커널에서 장치를 먼저 제거한 뒤에
 * 전원을 내려야 한다. 순서를 뒤집으면 드라이버가 아직 살아 있는 상태에서 장치가 응답을
 * 멈춰 MMIO 읽기가 전부 0xffffffff 로 돌아오고, 그 값을 정상 값으로 오해한 드라이버가
 * 오동작한다.
 * 
 * 동작 단계: (1) shpchp_unconfigure_device() 로 커널에서 자식 장치를 제거한다(그 안에서
 * 각 드라이버의 remove 콜백이 불린다). (2) 보드가 있었다면 상태 플래그를 종료 중으로
 * 바꾼다. (3) Slot Disable + 녹색 끄기 + 황색 켜기 명령으로 전원을 내린다. (4) 곧바로
 * 황색 LED 를 다시 꺼서 "정상적으로 제거됨"표시로 만든다. (5) 캐시 플래그를 정리한다.
 * 
 * 여기서도 NVMe 와의 관계는 한 방향이다 — (1) 단계의 장치 제거가 PCI 코어를 통해
 * 드라이버 remove 를 부르고, 꽂혀 있던 것이 NVMe 라면 그 결과로 nvme_remove() 가
 * 불릴 뿐이다. shpchp 가 NVMe 를 직접 부르는 코드는 없다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 ctrl->crit_sect 를 쥐고 있다. SHPC 명령
 * 두 번으로 최대 2초가 걸릴 수 있다.
 * 에러 경로: 어느 명령이든 실패하면 즉시 반환한다. 중간에 실패하면 황색 LED 가 켜진
 * 채로 남아 사람에게 문제를 알린다.
 * 
 * 호출 체인:
 *   shpchp_disable_slot() → [remove_board]
 *   → shpchp_unconfigure_device() (shpchp_pci.c) → pci_stop_and_remove_bus_device
 *   → shpchp_slot_disable / shpchp_set_attention_status (shpchp_hpc.c) */
static int remove_board(struct slot *p_slot)
{
	/* [한국어] 슬롯이 속한 컨트롤러 */
	struct controller *ctrl = p_slot->ctrl;
	/* [한국어] 논리 슬롯 번호 */
	u8 hp_slot;
	/* [한국어] 명령 발행 결과 */
	int rc;

	/* [한국어] 커널에서 이 슬롯 아래의 자식 장치들을 먼저 제거한다. 각 장치 드라이버의 remove 콜백이 여기서 불리며, 전원을 내리기 전에 끝나야 한다 */
	shpchp_unconfigure_device(p_slot);

	/* [한국어] PCI device 번호에서 첫 슬롯 device 번호를 빼 0 기반 논리 슬롯 번호를 만든다 */
	hp_slot = p_slot->device - ctrl->slot_device_offset;
	/* [한국어] 다시 그 논리 번호에 오프셋을 더해 슬롯을 찾는다 — 방금 뺀 값을 도로 더하는 셈이라 결과는 인자로 받은 p_slot 과 같다. 상류 코드에 남아 있는 불필요한 재조회이며 여기서는 그대로 둔다 */
	p_slot = shpchp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	/* [한국어] 논리 슬롯 번호를 디버그로 남긴다 */
	ctrl_dbg(ctrl, "%s: hp_slot = %d\n", __func__, hp_slot);

	/* Change status to shutdown */
	/* [한국어] 이 슬롯에 보드가 있었다면 */
	if (p_slot->is_a_board)
		/* [한국어] 상태를 종료 중(0x01)으로 표시한다. 이 값은 board_added() 가 보는 0xFF(전원 결함)와 구분된다 */
		p_slot->status = 0x01;

	/* turn off slot, turn on Amber LED, turn off Green LED */
	/* [한국어] Slot Disable + 녹색 LED 끄기 + 황색 LED 켜기를 한 명령으로 보낸다. 이 시점에 슬롯 전원이 끊긴다 */
	rc = shpchp_slot_disable(p_slot);
	/* [한국어] 명령이 실패하면 */
	if (rc) {
		/* [한국어] 실패를 기록하고 */
		ctrl_err(ctrl, "%s: Issue of Slot Disable command failed\n",
			 __func__);
		/* [한국어] 그대로 반환한다. 이 경우 황색 LED 상태는 명령 성공 여부에 달려 있다 */
		return rc;
	}

	/* [한국어] 정상적으로 껐으므로 황색 LED 를 다시 끈다. 위 Disable 명령이 켜 둔 경고 표시를 지워 '문제 없이 제거됨'상태로 만든다 */
	rc = shpchp_set_attention_status(p_slot, 0);
	/* [한국어] 이 명령이 실패하면 */
	if (rc) {
		/* [한국어] 실패를 기록하고 */
		ctrl_err(ctrl, "Issue of Set Attention command failed\n");
		/* [한국어] 그대로 반환한다. LED 는 켜진 채 남는다 */
		return rc;
	}

	/* [한국어] 전원이 꺼졌다고 캐시에 기록한다 */
	p_slot->pwr_save = 0;
	/* [한국어] 이 슬롯에 보드가 없다고 표시한다 */
	p_slot->is_a_board = 0;

	/* [한국어] 정상 제거 완료를 알린다 */
	return 0;
}


/* [한국어] 푸시버튼 지연 작업이 만료된 뒤 실제 켜기/끄기를 수행할 워크 항목. struct slot 안의 delayed_work 와 달리 매번 새로 할당해 쓰는 일회용 객체다 — 슬롯의 지연 작업 항목을 재사용하면 5초 타이머와 실제 작업이 같은 항목을 두고 다투게 되기 때문이다 */
struct pushbutton_work_info {
	/* [한국어] 대상 슬롯 포인터.
	 * 설정자: shpchp_queue_pushbutton_work() 가 할당 직후 채운다.
	 * 읽는 자: shpchp_pushbutton_thread() 가 워크 실행 시점에 읽어 켜기/끄기 대상을 정한다.
	 * 값 범위: 유효한 struct slot 포인터(NULL 불가). 슬롯 객체는 cleanup_slots() 전까지 산다.
	 * 동기화: 이 필드는 할당 직후 한 번만 쓰이고 이후 읽기만 하므로 별도 락이 필요 없다.
	 *         슬롯 상태 자체는 shpchp_pushbutton_thread() 가 p_slot->lock 으로 보호한다. */
	struct slot *p_slot;
	/* [한국어] 워크큐 항목 본체.
	 * 설정자: shpchp_queue_pushbutton_work() 가 INIT_WORK 으로 shpchp_pushbutton_thread 를
	 *         핸들러로 연결한 뒤 queue_work 로 슬롯 워크큐에 넣는다.
	 * 읽는 자: 워크큐 코어가 실행하며, 핸들러는 container_of 로 이 필드에서 바깥
	 *         pushbutton_work_info 를 되찾는다.
	 * 값 범위: 워크큐 코어가 관리하는 불투명 구조체 — 내부 필드를 직접 만지지 않는다.
	 * 동기화: 슬롯마다 전용 워크큐를 쓴다(shpchp_core.c 의 init_slots() 가
	 *         alloc_workqueue("shpchp-%d", WQ_PERCPU, 0, ...) 로 만든다). max_active 가
	 *         0(기본값)이라 항목들이 반드시 한 줄로 실행된다는 보장은 없으므로, 슬롯
	 *         상태의 보호는 핸들러가 잡는 p_slot->lock 이 맡는다. 실행이 끝나면
	 *         핸들러가 바깥 객체째 kfree 한다. */
	struct work_struct work;
};

/**
 * shpchp_pushbutton_thread - handle pushbutton events
 * @work: &struct work_struct to be handled
 *
 * Scheduled procedure to handle blocking stuff for the pushbuttons.
 * Handles all pending events and exits.
 */
/* [한국어] shpchp_pushbutton_thread - 5초 유예가 끝난 뒤 실제로 슬롯을 켜거나 끄는 워크 핸들러
 * 
 * @work: shpchp_queue_pushbutton_work() 가 할당한 pushbutton_work_info 안의 work 멤버.
 * @return: 없음(워크 핸들러 규약).
 * 
 * 버튼을 눌러 시작된 작업의 마지막 단계다. 여기까지 왔다는 것은 5초 유예 동안 취소가
 * 없었고 상태가 POWERON_STATE 또는 POWEROFF_STATE 로 승격됐다는 뜻이다. 실제 켜기/끄기는
 * 수 초가 걸리므로 인터럽트나 타이머가 아니라 워크큐에서 해야 한다.
 * 
 * 주목할 점은 락을 잡았다 놓았다 다시 잡는 패턴이다. p_slot->lock 을 잡고 상태를 읽은
 * 뒤, 실제 작업 전에 락을 놓고, 작업이 끝나면 다시 잡아 STATIC_STATE 로 되돌린다.
 * shpchp_enable_slot/disable_slot 이 ctrl->crit_sect 를 잡고 오래 걸리기 때문이며,
 * 그동안 상태값 POWERON/POWEROFF 자체가 다른 경로의 끼어들기를 막는 표지 역할을 한다.
 * 
 * 실행 컨텍스트: 슬롯 전용 워크큐(프로세스 컨텍스트). 수 초 동안 잠들 수 있다.
 * 에러 경로: 켜기에 실패하면 녹색 LED 를 꺼서 사용자에게 알리는 것이 전부다. 끄기
 * 실패는 별도 처리가 없다. 어느 경우든 상태는 STATIC_STATE 로 되돌아간다.
 * 
 * 호출 체인:
 *   handle_button_press_event() 가 건 5초 지연 작업 만료
 *   → shpchp_queue_pushbutton_work() → 워크큐 → [shpchp_pushbutton_thread]
 *   → shpchp_disable_slot() 또는 shpchp_enable_slot() */
static void shpchp_pushbutton_thread(struct work_struct *work)
{
	/* [한국어] work 포인터에서 그것을 품은 pushbutton_work_info 를 역산한다 */
	struct pushbutton_work_info *info =
		container_of(work, struct pushbutton_work_info, work);
	/* [한국어] 작업 대상 슬롯 */
	struct slot *p_slot = info->p_slot;

	/* [한국어] 상태를 읽기 위해 슬롯 락을 잡는다 */
	mutex_lock(&p_slot->lock);
	/* [한국어] 5초 유예 뒤 승격된 상태에 따라 켜기인지 끄기인지 갈린다 */
	switch (p_slot->state) {
	/* [한국어] 끄기로 승격된 상태 */
	case POWEROFF_STATE:
		/* [한국어] 긴 작업 전에 락을 놓는다. 상태값 POWEROFF_STATE 가 다른 경로의 끼어들기를 막는다 */
		mutex_unlock(&p_slot->lock);
		/* [한국어] 실제 비활성화 — 장치 제거, 전원 차단, LED 정리가 여기서 일어난다 */
		shpchp_disable_slot(p_slot);
		/* [한국어] 작업이 끝났으니 상태를 되돌리기 위해 다시 락을 잡는다 */
		mutex_lock(&p_slot->lock);
		/* [한국어] 평시 상태로 복귀 */
		p_slot->state = STATIC_STATE;
		break;
	/* [한국어] 켜기로 승격된 상태 */
	case POWERON_STATE:
		/* [한국어] 긴 작업 전에 락을 놓는다 */
		mutex_unlock(&p_slot->lock);
		/* [한국어] 실제 활성화. 실패하면 0 이 아닌 값이 돌아온다 */
		if (shpchp_enable_slot(p_slot))
			/* [한국어] 활성화에 실패했으면 깜빡이던 녹색 LED 를 꺼서 '켜지지 않았다'를 사람에게 알린다 */
			shpchp_green_led_off(p_slot);
		/* [한국어] 다시 락을 잡고 */
		mutex_lock(&p_slot->lock);
		/* [한국어] 평시 상태로 복귀 */
		p_slot->state = STATIC_STATE;
		break;
	/* [한국어] 그 밖의 상태 — 5초 사이에 취소되어 STATIC_STATE 로 돌아갔거나 이미 다른 작업이 진행 중인 경우다 */
	default:
		/* [한국어] 아무것도 하지 않는다. 취소가 정상 동작한 결과이므로 오류가 아니다 */
		break;
	}
	/* [한국어] 슬롯 락 해제 */
	mutex_unlock(&p_slot->lock);

	/* [한국어] shpchp_queue_pushbutton_work() 가 kmalloc 으로 만든 일회용 워크 객체를 해제한다 */
	kfree(info);
}

/* [한국어] shpchp_queue_pushbutton_work - 5초 유예가 만료됐을 때 BLINKING 상태를 POWER 상태로 승격시킨다
 * 
 * @work: struct slot 안에 박혀 있는 delayed_work 의 work 멤버. shpchp_core.c 의
 *        init_slots() 가 INIT_DELAYED_WORK 으로 이 함수를 핸들러로 걸어 두었다.
 * @return: 없음(지연 작업 핸들러 규약).
 * 
 * 버튼을 눌러 시작된 5초 유예의 종료 지점이다. 이 함수 자체는 슬롯을 켜거나 끄지 않고,
 * 상태를 승격시킨 뒤 실제 작업을 또 다른 워크 항목(shpchp_pushbutton_thread)으로 넘긴다.
 * 왜 한 단계를 더 두는가 — 지연 작업 항목은 슬롯에 하나뿐이라 재사용되는데, 실제 작업이
 * 수 초 걸리는 동안 그 항목을 붙잡고 있으면 다음 버튼 입력을 위한 지연 작업을 걸 수
 * 없기 때문이다. 그래서 일회용 워크 객체를 새로 할당해 넘긴다.
 * 
 * 동작 단계: (1) 새 pushbutton_work_info 를 GFP_KERNEL 로 할당한다(지연 작업은 프로세스
 * 컨텍스트라 잠들 수 있는 할당을 써도 된다). (2) 슬롯 락을 잡고 상태를 확인한다.
 * (3) BLINKINGOFF → POWEROFF, BLINKINGON → POWERON 으로 승격한다. (4) 그 밖의 상태면
 * 5초 안에 취소된 것이므로 방금 할당한 객체를 버리고 빠져나간다. (5) 승격했으면
 * 슬롯 워크큐에 실제 작업을 건다.
 * 
 * 실행 컨텍스트: 슬롯 전용 워크큐(프로세스 컨텍스트). handle_button_press_event() 가
 * 건 5초 타이머가 만료되면 워크큐 코어가 부른다.
 * 에러 경로: 메모리 할당 실패는 로그만 남기고 그냥 돌아간다 — 이 경우 슬롯 상태는
 * BLINKING 인 채로 남아, 사용자가 버튼을 다시 눌러야 STATIC_STATE 로 돌아온다.
 * 
 * 호출 체인:
 *   handle_button_press_event() → queue_delayed_work(5초) → 만료
 *   → 워크큐 코어 → [shpchp_queue_pushbutton_work]
 *   → queue_work → shpchp_pushbutton_thread() */
void shpchp_queue_pushbutton_work(struct work_struct *work)
{
	/* [한국어] work 포인터에서 struct slot 을 역산한다. 지연 작업이 slot 안에 박혀 있으므로 work.work 경로로 찾아 올라간다 */
	struct slot *p_slot = container_of(work, struct slot, work.work);
	/* [한국어] 실제 작업을 담을 일회용 워크 객체 */
	struct pushbutton_work_info *info;

	/* [한국어] GFP 플래그를 생략했으므로 kmalloc_obj 매크로의 기본값인 GFP_KERNEL 로 할당된다. 프로세스 컨텍스트(워크큐)라 잠들 수 있는 할당이 허용된다 */
	info = kmalloc_obj(*info);
	/* [한국어] 할당에 실패하면 */
	if (!info) {
		/* [한국어] 메모리 부족을 기록하고 */
		ctrl_err(p_slot->ctrl, "%s: Cannot allocate memory\n",
			 __func__);
		/* [한국어] 상태를 승격시키지 않고 돌아간다. 슬롯은 BLINKING 인 채로 남는다 */
		return;
	}
	/* [한국어] 작업 대상 슬롯을 채운다 */
	info->p_slot = p_slot;
	/* [한국어] 실제 켜기/끄기를 수행할 핸들러를 연결한다 */
	INIT_WORK(&info->work, shpchp_pushbutton_thread);

	/* [한국어] 상태를 읽고 바꾸므로 슬롯 락을 잡는다 */
	mutex_lock(&p_slot->lock);
	/* [한국어] 5초 사이에 취소됐는지, 아니면 그대로 진행해야 하는지 상태로 판별한다 */
	switch (p_slot->state) {
	/* [한국어] 끄기 예약이 유지된 상태 */
	case BLINKINGOFF_STATE:
		/* [한국어] 끄는 중으로 승격한다. 이 시점부터 버튼을 눌러도 취소되지 않는다 */
		p_slot->state = POWEROFF_STATE;
		break;
	/* [한국어] 켜기 예약이 유지된 상태 */
	case BLINKINGON_STATE:
		/* [한국어] 켜는 중으로 승격한다 */
		p_slot->state = POWERON_STATE;
		break;
	/* [한국어] 그 밖의 상태 — 5초 안에 버튼이 다시 눌려 취소됐다는 뜻이다(handle_button_press_event 가 STATIC_STATE 로 되돌렸다) */
	default:
		/* [한국어] 방금 할당한 워크 객체를 버린다. 누수를 막는 유일한 지점이다 */
		kfree(info);
		/* [한국어] 락을 풀기 위해 out 으로 간다 */
		goto out;
	}
	/* [한국어] 승격했으니 실제 작업을 슬롯 전용 워크큐에 건다. 슬롯마다 큐가 따로라 다른 슬롯 작업과 서로를 막지 않는다 */
	queue_work(p_slot->wq, &info->work);
 out:
	/* [한국어] 슬롯 락 해제 — 승격 경로와 취소 경로 모두 이 줄을 지난다 */
	mutex_unlock(&p_slot->lock);
}

/* [한국어] update_slot_info - 하드웨어의 현재 슬롯 상태 네 가지를 struct slot 의 캐시 필드에 새로 읽어 담는다
 * 
 * @slot: 갱신할 슬롯.
 * @return: 없음. 네 읽기 함수 모두 실패 경로가 없다.
 * 
 * 핫플러그 이벤트를 처리하고 나면 소프트웨어가 기억하는 상태와 하드웨어의 실제 상태가
 * 어긋날 수 있다. 특히 shpchp_handle_switch_change() 같은 이벤트 판정은 이전 값
 * (presence_save, pwr_save)과 새 값을 비교해 "서프라이즈 제거"인지 가르므로, 캐시가
 * 낡으면 오판이 난다. 그래서 상태가 바뀔 만한 지점마다 이 함수로 캐시를 새로 채운다.
 * 
 * 동작 단계: 전원 상태 → Attention LED 상태 → 래치(MRL) 상태 → 카드 존재 여부 순으로
 * shpchp_hpc.c 의 읽기 함수를 불러 각각 pwr_save/attention_save/latch_save/presence_save
 * 에 담는다. 넷 다 MMIO 읽기 한 번씩이라 명령 큐를 거치지 않아 빠르다.
 * 실행 컨텍스트: 프로세스 컨텍스트(워크큐 또는 sysfs 쓰기). 호출자가 p_slot->lock 또는
 * ctrl->crit_sect 를 쥔 상태에서 부른다.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   handle_button_press_event / interrupt_event_handler / shpchp_enable_slot /
 *   shpchp_disable_slot → [update_slot_info]
 *   → shpchp_get_power_status / shpchp_get_attention_status /
 *     shpchp_get_latch_status / shpchp_get_adapter_status (shpchp_hpc.c) */
static void update_slot_info(struct slot *slot)
{
	/* [한국어] Slot State 필드를 읽어 pwr_save 에 담는다. 0=꺼짐, 1=켜지고 연결됨, 2=전원만 */
	shpchp_get_power_status(slot, &slot->pwr_save);
	/* [한국어] Attention LED 상태를 읽어 attention_save 에 담는다. 0=꺼짐, 1=켜짐, 2=깜빡임 */
	shpchp_get_attention_status(slot, &slot->attention_save);
	/* [한국어] MRL 래치 상태를 읽어 latch_save 에 담는다. 0=닫힘, 1=열림 */
	shpchp_get_latch_status(slot, &slot->latch_save);
	/* [한국어] 카드 존재 여부를 읽어 presence_save 에 담는다. 이 값이 다음 이벤트 판정의 기준이 된다 */
	shpchp_get_adapter_status(slot, &slot->presence_save);
}

/*
 * Note: This function must be called with slot->lock held
 */
/* [한국어] handle_button_press_event - Attention 버튼 눌림을 슬롯 상태 기계에 반영한다(핵심 상태 전이)
 * 
 * @p_slot: 버튼이 눌린 슬롯. 호출자가 반드시 p_slot->lock 을 쥔 채로 불러야 한다
 *          (상류 주석이 그렇게 못 박아 두었고, 유일한 호출자가 그 조건을 지킨다).
 * @return: 없음.
 * 
 * 이 함수가 shpchp 상태 기계의 중심이다. SHPC 슬롯은 다섯 상태를 가진다 —
 * STATIC_STATE(0, 아무 작업도 진행 중이 아님), BLINKINGON_STATE(1, 5초 뒤 켜기 예정),
 * BLINKINGOFF_STATE(2, 5초 뒤 끄기 예정), POWERON_STATE(3, 켜는 중),
 * POWEROFF_STATE(4, 끄는 중). pciehp 는 여기에 OFF_STATE/ON_STATE 를 따로 두어 여섯
 * 상태를 쓰지만 shpchp 는 켜져 있든 꺼져 있든 "작업 중이 아님"을 STATIC_STATE 하나로
 * 합쳐 다섯 상태만 쓴다 — 이 파일에서 확인한 사실이다.
 * 
 * 버튼을 누르면 즉시 동작하지 않고 5초 동안 녹색 LED 를 깜빡여 취소할 기회를 준다는
 * 설계는 pciehp 와 같다. 그 5초는 아래 queue_delayed_work(..., 5*HZ) 가 만들고,
 * 만료되면 shpchp_queue_pushbutton_work() 가 BLINKING 상태를 POWER 상태로 승격시킨다.
 * 5초가 지나기 전에 버튼을 다시 누르면 이 함수의 두 번째 case 가 지연 작업을 취소하고
 * LED 를 원래대로 되돌린다.
 * 
 * 동작 단계: (1) STATIC_STATE 면 현재 전원 상태를 읽어 켜져 있으면 끄기 예약
 * (BLINKINGOFF), 꺼져 있으면 켜기 예약(BLINKINGON)으로 간다. (2) 이미 BLINKING 중이면
 * 취소로 해석해 원래 LED 상태로 되돌리고 STATIC_STATE 로 복귀한다. (3) 이미 POWERON/
 * POWEROFF 진행 중이면 무시한다 — 되돌릴 수 없는 구간이기 때문이다.
 * 
 * 실행 컨텍스트: 워크큐(프로세스 컨텍스트). p_slot->lock 을 쥔 채 shpchp_hpc.c 의 명령
 * 발행 함수를 부르므로 최대 1초씩 잠들 수 있다. 뮤텍스라 잠들어도 안전하다.
 * 에러 경로: LED 명령 실패는 검사하지 않는다. 상태 전이는 그대로 진행된다.
 * 
 * 호출 체인:
 *   shpc_isr() → shpchp_handle_attention_button() → queue_interrupt_event()
 *   → 워크큐 → interrupt_event_handler() → [handle_button_press_event]
 *   → shpchp_green_led_blink / shpchp_set_attention_status /
 *     queue_delayed_work(5초) → 만료 시 shpchp_queue_pushbutton_work() */
static void handle_button_press_event(struct slot *p_slot)
{
	/* [한국어] 전원 상태를 임시로 받을 변수 */
	u8 getstatus;
	/* [한국어] 로그 출력을 위한 컨트롤러 포인터 */
	struct controller *ctrl = p_slot->ctrl;

	/* [한국어] 현재 상태에 따라 버튼의 의미가 완전히 달라진다 — 시작인지, 취소인지, 무시인지 */
	switch (p_slot->state) {
	/* [한국어] 아무 작업도 진행 중이 아닌 평시 상태에서 버튼이 눌렸다 = 새 작업의 시작이다 */
	case STATIC_STATE:
		/* [한국어] 지금 전원이 들어와 있는지 하드웨어에서 직접 읽는다. 캐시가 아니라 실제 값을 봐야 방향을 정할 수 있다 */
		shpchp_get_power_status(p_slot, &getstatus);
		/* [한국어] 전원이 들어와 있으면 사용자의 의도는 끄기다 */
		if (getstatus) {
			/* [한국어] 5초 뒤 끄기 예정 상태로 전이 */
			p_slot->state = BLINKINGOFF_STATE;
			/* [한국어] 어떤 슬롯이 왜 꺼지려 하는지 사용자에게 알린다 */
			ctrl_info(ctrl, "PCI slot #%s - powering off due to button press\n",
				  slot_name(p_slot));
		/* [한국어] 전원이 꺼져 있으면 사용자의 의도는 켜기다 */
		} else {
			/* [한국어] 5초 뒤 켜기 예정 상태로 전이 */
			p_slot->state = BLINKINGON_STATE;
			/* [한국어] 어떤 슬롯이 왜 켜지려 하는지 알린다 */
			ctrl_info(ctrl, "PCI slot #%s - powering on due to button press\n",
				  slot_name(p_slot));
		}
		/* blink green LED and turn off amber */
		/* [한국어] 녹색 LED 를 깜빡이게 해 '지금 취소할 수 있다'를 사람에게 알린다. BLINKING 상태의 시각적 표현이다 */
		shpchp_green_led_blink(p_slot);
		/* [한국어] 황색 LED 를 끈다. 이전 작업에서 켜져 있었을 수 있는 경고 표시를 지운다 */
		shpchp_set_attention_status(p_slot, 0);

		/* [한국어] 5초 뒤 실행될 지연 작업을 슬롯 전용 워크큐에 건다. 이 5초가 곧 취소 유예 시간이며, 만료되면 shpchp_queue_pushbutton_work() 가 불린다(핸들러는 shpchp_core.c 의 INIT_DELAYED_WORK 로 연결돼 있다) */
		queue_delayed_work(p_slot->wq, &p_slot->work, 5*HZ);
		break;
	/* [한국어] 5초 유예 중에 버튼이 다시 눌렸다 — 끄기 예약을 취소하려는 것이다 */
	case BLINKINGOFF_STATE:
	/* [한국어] 켜기 예약도 마찬가지로 취소 대상이다 */
	case BLINKINGON_STATE:
		/*
		 * Cancel if we are still blinking; this means that we
		 * press the attention again before the 5 sec. limit
		 * expires to cancel hot-add or hot-remove
		 */
		/* [한국어] 취소되었음을 사용자에게 알린다 */
		ctrl_info(ctrl, "Button cancel on Slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 예약해 둔 지연 작업을 취소한다. 이미 실행이 시작됐다면 취소가 실패할 수 있지만, 그 경우에도 아래에서 상태를 STATIC_STATE 로 되돌리므로 shpchp_queue_pushbutton_work() 의 default 분기가 아무 일도 하지 않고 빠져나간다 */
		cancel_delayed_work(&p_slot->work);
		/* [한국어] 끄기를 취소한 것이라면 원래 상태는 켜져 있던 것이다 */
		if (p_slot->state == BLINKINGOFF_STATE)
			/* [한국어] 녹색 LED 를 다시 켜서 '전원 들어옴'표시로 되돌린다 */
			shpchp_green_led_on(p_slot);
		/* [한국어] 켜기를 취소한 것이라면 원래 상태는 꺼져 있던 것이다 */
		else
			/* [한국어] 녹색 LED 를 꺼서 '전원 없음'표시로 되돌린다 */
			shpchp_green_led_off(p_slot);
		/* [한국어] 황색 LED 도 꺼서 깨끗한 상태로 만든다 */
		shpchp_set_attention_status(p_slot, 0);
		/* [한국어] 취소가 완료됐음을 알린다 */
		ctrl_info(ctrl, "PCI slot #%s - action canceled due to button press\n",
			  slot_name(p_slot));
		/* [한국어] 평시 상태로 복귀 — 이제 다시 버튼을 누르면 새 작업이 시작된다 */
		p_slot->state = STATIC_STATE;
		break;
	/* [한국어] 이미 끄는 작업이 진행 중이다 */
	case POWEROFF_STATE:
	/* [한국어] 이미 켜는 작업이 진행 중이다 */
	case POWERON_STATE:
		/*
		 * Ignore if the slot is on power-on or power-off state;
		 * this means that the previous attention button action
		 * to hot-add or hot-remove is undergoing
		 */
		/* [한국어] 진행 중인 작업은 되돌릴 수 없으므로 버튼을 무시한다고 알린다 */
		ctrl_info(ctrl, "Button ignore on Slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 무시하더라도 상태 캐시는 최신으로 맞춰 둔다 */
		update_slot_info(p_slot);
		break;
	/* [한국어] 다섯 상태 어디에도 해당하지 않는 값 — 메모리 손상 같은 비정상 상황이다 */
	default:
		/* [한국어] 경고만 남기고 아무 상태 전이도 하지 않는다 */
		ctrl_warn(ctrl, "Not a valid state\n");
		break;
	}
}

/* [한국어] interrupt_event_handler - 인터럽트가 큐에 넣은 이벤트를 프로세스 컨텍스트에서 꺼내 처리한다
 * 
 * @work: queue_interrupt_event() 가 할당한 struct event_info 안의 work 멤버.
 *        container_of 로 원래 event_info 를 되찾는다.
 * @return: 없음(워크 핸들러 규약).
 * 
 * shpc_isr() 은 인터럽트 컨텍스트라 잠들 수 없는데, 이벤트 처리는 SHPC 명령을 내며
 * 최대 1초씩 잠들어야 한다. 그 간극을 워크큐가 메운다 — 인터럽트는 이벤트 종류만
 * 기록해 큐에 넣고, 실제 처리는 이 함수가 프로세스 컨텍스트에서 한다.
 * 
 * 이 switch 가 실제로 특별 취급하는 이벤트는 둘뿐이다. INT_BUTTON_PRESS 는 상태 기계로
 * 넘기고, INT_POWER_FAULT 는 황색 LED 를 켜고 녹색을 끈다. 나머지 이벤트
 * (INT_PRESENCE_ON/OFF, INT_SWITCH_OPEN/CLOSE, INT_POWER_FAULT_CLEAR)는 모두 default 로
 * 떨어져 상태 캐시만 갱신한다. 즉 shpchp 는 카드를 꽂았다고 자동으로 전원을 넣지 않고,
 * 사용자가 버튼을 누르거나 sysfs 에 쓰기를 해야 비로소 슬롯이 켜진다. shpchp.h 에
 * 정의된 INT_BUTTON_IGNORE / INT_BUTTON_RELEASE / INT_BUTTON_CANCEL 은 이 트리에서
 * 어디에서도 생성되지 않는다(정의만 남아 있다).
 * 
 * 실행 컨텍스트: 슬롯 전용 워크큐(프로세스 컨텍스트). p_slot->lock 을 잡아 같은 슬롯의
 * 이벤트 처리와 sysfs 경로가 겹치지 않게 한다.
 * 에러 경로: 없다. 처리 후 반드시 event_info 를 해제해 누수를 막는다.
 * 
 * 호출 체인:
 *   워크큐 코어 → [interrupt_event_handler]
 *   → handle_button_press_event() 또는 shpchp_set_attention_status /
 *     shpchp_green_led_off 또는 update_slot_info() */
static void interrupt_event_handler(struct work_struct *work)
{
	/* [한국어] work 포인터에서 그것을 품은 struct event_info 를 역산한다. queue_interrupt_event() 가 kmalloc 으로 만든 그 객체다 */
	struct event_info *info = container_of(work, struct event_info, work);
	/* [한국어] 이벤트가 발생한 슬롯 */
	struct slot *p_slot = info->p_slot;

	/* [한국어] 슬롯 단위 직렬화. 같은 슬롯에 대한 sysfs 요청과 이벤트 처리가 동시에 상태를 바꾸는 것을 막는다 */
	mutex_lock(&p_slot->lock);
	/* [한국어] 인터럽트가 기록해 둔 이벤트 종류로 분기한다 */
	switch (info->event_type) {
	/* [한국어] Attention 버튼 눌림 — 유일하게 상태 기계를 움직이는 이벤트다 */
	case INT_BUTTON_PRESS:
		/* [한국어] 5초 블링킹/취소 로직으로 넘긴다. 이미 p_slot->lock 을 쥔 상태이므로 그 함수의 전제 조건을 만족한다 */
		handle_button_press_event(p_slot);
		break;
	/* [한국어] 전원 결함 — 하드웨어가 슬롯 전원에 문제가 있다고 알렸다 */
	case INT_POWER_FAULT:
		/* [한국어] 결함 발생 사실을 디버그 로그로 남긴다 */
		ctrl_dbg(p_slot->ctrl, "%s: Power fault\n", __func__);
		/* [한국어] 황색 LED 를 켜서 문제 있는 슬롯을 사람이 찾을 수 있게 한다 */
		shpchp_set_attention_status(p_slot, 1);
		/* [한국어] 녹색 LED 를 꺼서 전원이 정상이 아님을 표시한다. 상태 전이는 하지 않으므로 슬롯 상태 기계는 그대로다 */
		shpchp_green_led_off(p_slot);
		break;
	/* [한국어] 나머지 모든 이벤트 — 카드 삽입/제거, 래치 개폐, 전원 결함 해제가 여기로 온다 */
	default:
		/* [한국어] 상태 캐시만 새로 읽어 둔다. 자동으로 슬롯을 켜거나 끄지는 않는다 */
		update_slot_info(p_slot);
		break;
	}
	/* [한국어] 슬롯 락 해제 */
	mutex_unlock(&p_slot->lock);

	/* [한국어] queue_interrupt_event() 가 kmalloc 으로 만든 이벤트 객체를 해제한다. 이 해제가 빠지면 인터럽트마다 메모리가 샌다 */
	kfree(info);
}


/* [한국어] shpchp_enable_slot - 슬롯을 실제로 켜는 본체(전제 조건 검사 + AMD 에라타 + board_added)
 * 
 * @p_slot: 켤 슬롯.
 * @return: 0 이면 성공. -ENODEV 는 전제 조건(카드 있음, 래치 닫힘, 전원 꺼짐)을 만족하지
 *          못해 아무것도 하지 않았다는 뜻이다. 그 밖의 값은 board_added() 가 돌려준
 *          것으로, -1 이거나 WRONG_BUS_FREQUENCY(0x0D) 같은 shpchp 고유 에러 코드일 수
 *          있다 — 이 드라이버는 errno 와 자체 코드를 한 반환값에 섞어 쓴다.
 * 
 * 켜기 전에 세 가지를 반드시 확인해야 한다. 카드가 꽂혀 있어야 하고, 래치(MRL)가 닫혀
 * 있어야 하며(열려 있으면 사람이 카드를 만지는 중이므로 전원을 넣으면 위험하다),
 * 이미 켜져 있으면 안 된다. 셋 중 하나라도 어긋나면 조용히 -ENODEV 로 물러난다.
 * 
 * AMD POGO 7458 브리지에 슬롯이 하나뿐인 구성에서는 카드 설정 중에 발생하는 가짜
 * PERR/SERR 때문에 시스템이 죽는 에라타가 있어, 활성화 전후로 브리지의 PCI-X MiscII
 * 레지스터에서 오류 보고 비트를 껐다 되돌린다(그 저장/복원 코드는 shpchp.h 의
 * amd_pogo_errata_save_misc_reg / amd_pogo_errata_restore_misc_reg 에 있다).
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. ctrl->crit_sect 를 잡아 같은 컨트롤러의 다른 슬롯
 * 작업과 직렬화한다 — SHPC 명령 큐가 컨트롤러당 하나뿐이기 때문이다. 이 함수는
 * p_slot->lock 을 잡지 않으며, 호출자가 그 락을 놓은 뒤 부르도록 되어 있다.
 * 에러 경로: 어느 검사에서 걸리든 out 라벨로 내려가 반드시 crit_sect 를 푼다.
 * 
 * 호출 체인:
 *   shpchp_pushbutton_thread() (버튼 경로) 또는 shpchp_sysfs_enable_slot() (sysfs 경로)
 *   → [shpchp_enable_slot] → board_added() → shpchp_power_on_slot /
 *     shpchp_set_bus_speed_mode / shpchp_slot_enable / shpchp_configure_device */
static int shpchp_enable_slot (struct slot *p_slot)
{
	/* [한국어] 상태 읽기 결과를 받는 임시 변수 */
	u8 getstatus = 0;
	/* [한국어] rc: 각 상태 읽기의 반환값, retval: 최종 반환값. 기본을 -ENODEV 로 두어 전제 조건에서 걸리면 그대로 반환되게 한다 */
	int rc, retval = -ENODEV;
	/* [한국어] 로그용 컨트롤러 포인터 */
	struct controller *ctrl = p_slot->ctrl;

	/* Check to see if (latch closed, card present, power off) */
	/* [한국어] 컨트롤러 단위 임계 구역 진입. SHPC 명령 큐가 하나뿐이라 두 슬롯을 동시에 켜고 끌 수 없다 */
	mutex_lock(&p_slot->ctrl->crit_sect);
	/* [한국어] 첫 번째 전제 조건 — 카드가 실제로 꽂혀 있는가 */
	rc = shpchp_get_adapter_status(p_slot, &getstatus);
	/* [한국어] 읽기 실패이거나 카드가 없으면 */
	if (rc || !getstatus) {
		/* [한국어] 빈 슬롯에 전원을 넣을 이유가 없다고 알리고 */
		ctrl_info(ctrl, "No adapter on slot(%s)\n", slot_name(p_slot));
		/* [한국어] -ENODEV 를 그대로 둔 채 정리 경로로 간다 */
		goto out;
	}
	/* [한국어] 두 번째 전제 조건 — 래치(MRL)가 닫혀 있는가 */
	rc = shpchp_get_latch_status(p_slot, &getstatus);
	/* [한국어] 읽기 실패이거나 래치가 열려 있으면(getstatus 가 1) */
	if (rc || getstatus) {
		/* [한국어] 사람이 카드를 만지는 중일 수 있으므로 전원을 넣지 않는다 */
		ctrl_info(ctrl, "Latch open on slot(%s)\n", slot_name(p_slot));
		/* [한국어] 정리 경로로 간다 */
		goto out;
	}
	/* [한국어] 세 번째 전제 조건 — 아직 꺼져 있는가 */
	rc = shpchp_get_power_status(p_slot, &getstatus);
	/* [한국어] 읽기 실패이거나 이미 켜져 있으면 */
	if (rc || getstatus) {
		/* [한국어] 중복 활성화를 막는다 */
		ctrl_info(ctrl, "Already enabled on slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 정리 경로로 간다 */
		goto out;
	}

	/* [한국어] 이 슬롯에 보드가 들어 있다고 표시한다. remove_board() 가 이 플래그를 보고 종료 처리 여부를 정한다 */
	p_slot->is_a_board = 1;

	/* We have to save the presence info for these slots */
	/* [한국어] 카드 존재 여부를 캐시에 저장한다. 이후 서프라이즈 제거 판정의 기준값이 된다 */
	shpchp_get_adapter_status(p_slot, &p_slot->presence_save);
	/* [한국어] 전원 상태를 캐시에 저장한다. shpchp_handle_switch_change() 가 이 값과 presence_save 를 함께 봐 서프라이즈 제거를 가려낸다 */
	shpchp_get_power_status(p_slot, &p_slot->pwr_save);
	/* [한국어] 저장된 전원 상태를 디버그 로그로 남긴다 */
	ctrl_dbg(ctrl, "%s: p_slot->pwr_save %x\n", __func__, p_slot->pwr_save);
	/* [한국어] 래치 상태도 한 번 읽어 둔다. 다만 이 값은 로컬 변수에만 담기고 이후 쓰이지 않는다(상류 코드 그대로다) */
	shpchp_get_latch_status(p_slot, &getstatus);

	/* [한국어] AMD POGO 7458 에라타 조건 검사 시작 — 벤더가 AMD 이고 */
	if ((p_slot->ctrl->pci_dev->vendor == PCI_VENDOR_ID_AMD &&
	     /* [한국어] 장치 ID 가 POGO 7458(0x7458)이며 */
	     p_slot->ctrl->pci_dev->device == PCI_DEVICE_ID_AMD_POGO_7458)
	     /* [한국어] 슬롯이 하나뿐인 구성일 때만 해당한다 */
	     && p_slot->ctrl->num_slots == 1) {
		/* handle AMD POGO errata; this must be done before enable  */
		/* [한국어] 활성화 전에 브리지의 PCI-X MiscII 레지스터를 저장하고 SERR/PERR 보고 비트를 모두 끈다. 카드 설정 중 발생하는 가짜 오류로 시스템이 죽는 것을 막기 위해서다 */
		amd_pogo_errata_save_misc_reg(p_slot);
		/* [한국어] 실제 활성화 — 전원 인가, 속도 협상, 슬롯 Enable, 버스 재스캔까지 여기서 일어난다 */
		retval = board_added(p_slot);
		/* handle AMD POGO errata; this must be done after enable  */
		/* [한국어] 활성화가 끝났으면 관찰된 PERR/RSE 를 write-one-to-clear 로 지우고 원래 MiscII 값을 복원한다 */
		amd_pogo_errata_restore_misc_reg(p_slot);
	} else
		/* [한국어] 에라타 대상이 아닌 일반 경로 — 곧바로 활성화한다 */
		retval = board_added(p_slot);

	/* [한국어] 활성화가 실패했다면 */
	if (retval) {
		/* [한국어] 실패 시점의 카드 존재 여부를 다시 읽어 캐시를 실제 상태에 맞춘다 */
		shpchp_get_adapter_status(p_slot, &p_slot->presence_save);
		/* [한국어] 래치 상태도 다시 읽는다. 역시 로컬 변수에만 담기고 쓰이지는 않는다 */
		shpchp_get_latch_status(p_slot, &getstatus);
	}

	/* [한국어] 성공이든 실패든 네 가지 상태 캐시를 하드웨어 실제 값으로 새로 채운다 */
	update_slot_info(p_slot);
 out:
	/* [한국어] 임계 구역 해제 — 모든 경로가 반드시 이 줄을 지난다 */
	mutex_unlock(&p_slot->ctrl->crit_sect);
	/* [한국어] 0(성공) 또는 -ENODEV / board_added() 의 실패 코드를 호출자에게 돌려준다 */
	return retval;
}


/* [한국어] shpchp_disable_slot - 슬롯을 실제로 끄는 본체(전제 조건 검사 + remove_board)
 * 
 * @p_slot: 끌 슬롯.
 * @return: 0 이면 성공. -ENODEV 는 전제 조건을 만족하지 못해 아무것도 하지 않았다는
 *          뜻이고, 그 밖의 값은 remove_board() 가 돌려준 명령 발행 실패다.
 * 
 * shpchp_enable_slot() 의 거울상이지만 세 번째 검사가 반대다 — 켤 때는 "꺼져 있어야"
 * 하고 끌 때는 "켜져 있어야" 한다. 래치가 열려 있으면 끄기도 거부하는데, 이는 SHPC
 * 컨트롤러 자체가 래치가 열린 상태에서 슬롯 조작 명령을 MRL Open 오류로 거절하기
 * 때문이다(shpchp_check_cmd_status 의 SWITCH_OPEN). 명령을 내 봐야 실패하므로 미리 막는다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. ctrl->crit_sect 로 컨트롤러 단위 직렬화.
 * 호출자가 p_slot->lock 을 놓은 뒤 부른다.
 * 에러 경로: 어느 검사에서 걸리든 out 으로 내려가 crit_sect 를 반드시 푼다.
 * 
 * 호출 체인:
 *   shpchp_pushbutton_thread() (버튼 경로) 또는 shpchp_sysfs_disable_slot() (sysfs 경로)
 *   → [shpchp_disable_slot] → remove_board() → shpchp_unconfigure_device /
 *     shpchp_slot_disable / shpchp_set_attention_status */
static int shpchp_disable_slot (struct slot *p_slot)
{
	/* [한국어] 상태 읽기 결과를 받는 임시 변수 */
	u8 getstatus = 0;
	/* [한국어] rc: 각 상태 읽기의 반환값, retval: 최종 반환값. 기본은 -ENODEV */
	int rc, retval = -ENODEV;
	/* [한국어] 로그용 컨트롤러 포인터 */
	struct controller *ctrl = p_slot->ctrl;

	/* [한국어] 슬롯에 컨트롤러가 연결돼 있지 않으면 레지스터에 접근할 방법이 없다 — 방어적 검사다 */
	if (!p_slot->ctrl)
		/* [한국어] 장치가 없다고 알린다 */
		return -ENODEV;

	/* Check to see if (latch closed, card present, power on) */
	/* [한국어] 컨트롤러 단위 임계 구역 진입 */
	mutex_lock(&p_slot->ctrl->crit_sect);

	/* [한국어] 첫 번째 전제 조건 — 카드가 꽂혀 있는가 */
	rc = shpchp_get_adapter_status(p_slot, &getstatus);
	/* [한국어] 읽기 실패이거나 카드가 없으면 끌 대상이 없다 */
	if (rc || !getstatus) {
		/* [한국어] 빈 슬롯임을 알리고 */
		ctrl_info(ctrl, "No adapter on slot(%s)\n", slot_name(p_slot));
		/* [한국어] 정리 경로로 간다 */
		goto out;
	}
	/* [한국어] 두 번째 전제 조건 — 래치가 닫혀 있는가 */
	rc = shpchp_get_latch_status(p_slot, &getstatus);
	/* [한국어] 래치가 열려 있으면 SHPC 가 슬롯 조작 명령 자체를 거절하므로 미리 막는다 */
	if (rc || getstatus) {
		/* [한국어] 래치가 열렸음을 알리고 */
		ctrl_info(ctrl, "Latch open on slot(%s)\n", slot_name(p_slot));
		/* [한국어] 정리 경로로 간다 */
		goto out;
	}
	/* [한국어] 세 번째 전제 조건 — 켜기와 반대로 이번엔 켜져 있어야 한다 */
	rc = shpchp_get_power_status(p_slot, &getstatus);
	/* [한국어] 읽기 실패이거나 이미 꺼져 있으면 */
	if (rc || !getstatus) {
		/* [한국어] 중복 비활성화를 막는다 */
		ctrl_info(ctrl, "Already disabled on slot(%s)\n",
			  slot_name(p_slot));
		/* [한국어] 정리 경로로 간다 */
		goto out;
	}

	/* [한국어] 실제 제거 — 커널에서 장치 제거, 슬롯 Disable 명령, LED 정리까지 여기서 일어난다 */
	retval = remove_board(p_slot);
	/* [한국어] 네 가지 상태 캐시를 하드웨어 실제 값으로 새로 채운다 */
	update_slot_info(p_slot);
 out:
	/* [한국어] 임계 구역 해제 — 모든 경로가 반드시 이 줄을 지난다 */
	mutex_unlock(&p_slot->ctrl->crit_sect);
	/* [한국어] 0(성공) 또는 -ENODEV / remove_board() 의 실패 코드를 돌려준다 */
	return retval;
}

/* [한국어] shpchp_sysfs_enable_slot - sysfs 의 슬롯 power 쓰기(1)를 상태 기계에 안전하게 태운다
 * 
 * @p_slot: 켤 슬롯.
 * @return: 0 이면 성공, 음수 errno 또는 shpchp 에러 코드면 실패. 상태 때문에 아무것도
 *          하지 않은 경우에는 초깃값 -ENODEV 가 그대로 나간다.
 * 
 * 사용자가 /sys/bus/pci/slots/<이름>/power 에 1 을 쓰면 shpchp_core.c 의 enable_slot()
 * 콜백을 거쳐 이 함수가 불린다. 버튼 경로와 sysfs 경로가 같은 슬롯을 동시에 건드릴 수
 * 있으므로, 여기서 상태를 확인해 충돌을 정리하는 것이 이 함수의 존재 이유다.
 * BLINKINGON_STATE(버튼으로 켜기 예약이 걸려 5초 대기 중)라면 지연 작업을 취소하고
 * 곧바로 켜기로 넘어간다 — 사용자가 명시적으로 요청했으니 5초를 더 기다릴 이유가 없다.
 * 
 * 동작 단계: (1) p_slot->lock 을 잡는다. (2) BLINKINGON 이면 지연 작업 취소 후 아래로
 * 흘려보낸다(fallthrough). (3) STATIC_STATE 면 POWERON_STATE 로 올리고 락을 놓은 뒤
 * shpchp_enable_slot() 을 부르고, 돌아와 다시 락을 잡고 STATIC_STATE 로 되돌린다.
 * (4) 이미 진행 중이거나 반대 방향 작업 중이면 로그만 남긴다.
 * 
 * 락을 놓았다 다시 잡는 이유는 shpchp_enable_slot() 이 ctrl->crit_sect 를 잡고 수 초씩
 * 걸리기 때문이다. 그동안 p_slot->lock 을 쥐고 있으면 같은 슬롯의 이벤트 처리가 모두
 * 막힌다. 상태를 POWERON_STATE 로 올려 두었으므로 그 사이에 버튼이 눌려도
 * handle_button_press_event() 가 "진행 중"으로 보고 무시한다 — 상태값 자체가 락을
 * 대신하는 구조다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기하는 사용자 프로세스). 잠들 수 있다.
 * 에러 경로: shpchp_enable_slot() 의 반환값을 그대로 올려 보낸다.
 * 
 * 호출 체인:
 *   sysfs write → enable_slot() (shpchp_core.c) → [shpchp_sysfs_enable_slot]
 *   → shpchp_enable_slot() → board_added() */
int shpchp_sysfs_enable_slot(struct slot *p_slot)
{
	/* [한국어] 기본 반환값. 아무 동작도 하지 않는 상태 분기에서는 이 값이 그대로 나간다 */
	int retval = -ENODEV;
	/* [한국어] 로그용 컨트롤러 포인터 */
	struct controller *ctrl = p_slot->ctrl;

	/* [한국어] 슬롯 상태를 읽고 바꾸므로 슬롯 락을 잡는다 */
	mutex_lock(&p_slot->lock);
	/* [한국어] 현재 상태에 따라 요청을 어떻게 처리할지 정한다 */
	switch (p_slot->state) {
	/* [한국어] 버튼으로 켜기 예약이 걸려 5초를 세는 중이다 */
	case BLINKINGON_STATE:
		/* [한국어] 사용자가 명시적으로 요청했으므로 남은 유예를 취소한다. 취소에 실패해도(이미 실행 중) 아래에서 상태를 바꾸므로 지연 작업 쪽이 무해하게 빠져나간다 */
		cancel_delayed_work(&p_slot->work);
		/* [한국어] 취소했으니 곧바로 아래 STATIC_STATE 처리로 흘려보낸다 — 의도된 fallthrough 임을 컴파일러에 알린다 */
		fallthrough;
	/* [한국어] 평시 상태 — 바로 켜기를 시작한다 */
	case STATIC_STATE:
		/* [한국어] 켜는 중으로 표시한다. 이 값이 다른 경로(버튼 이벤트)에게 '건드리지 마라'는 신호가 된다 */
		p_slot->state = POWERON_STATE;
		/* [한국어] 긴 작업 전에 슬롯 락을 놓는다. 상태값이 이미 보호 역할을 하므로 안전하다 */
		mutex_unlock(&p_slot->lock);
		/* [한국어] 실제 활성화. 내부에서 ctrl->crit_sect 를 잡고 수 초가 걸릴 수 있다 */
		retval = shpchp_enable_slot(p_slot);
		/* [한국어] 작업이 끝났으니 상태를 되돌리기 위해 다시 락을 잡는다 */
		mutex_lock(&p_slot->lock);
		/* [한국어] 평시 상태로 복귀 */
		p_slot->state = STATIC_STATE;
		break;
	/* [한국어] 이미 켜는 작업이 진행 중이다 */
	case POWERON_STATE:
		/* [한국어] 중복 요청임을 알린다. retval 은 -ENODEV 인 채로 나간다 */
		ctrl_info(ctrl, "Slot %s is already in powering on state\n",
			  slot_name(p_slot));
		break;
	/* [한국어] 버튼으로 끄기 예약이 걸린 상태 */
	case BLINKINGOFF_STATE:
	/* [한국어] 끄는 작업이 진행 중인 상태 */
	case POWEROFF_STATE:
		/* [한국어] 둘 다 켜기 요청과 방향이 충돌하므로 아무것도 하지 않고 알리기만 한다 */
		ctrl_info(ctrl, "Already enabled on slot %s\n",
			  slot_name(p_slot));
		break;
	/* [한국어] 다섯 상태 어디에도 없는 값 — 비정상이다 */
	default:
		/* [한국어] 오류 로그만 남긴다 */
		ctrl_err(ctrl, "Not a valid state on slot %s\n",
			 slot_name(p_slot));
		break;
	}
	/* [한국어] 슬롯 락 해제 — 모든 분기가 이 줄을 지난다 */
	mutex_unlock(&p_slot->lock);

	/* [한국어] 성공/실패를 sysfs 쓰기 경로로 돌려준다. 음수면 사용자에게 errno 로 보인다 */
	return retval;
}

/* [한국어] shpchp_sysfs_disable_slot - sysfs 의 슬롯 power 쓰기(0)를 상태 기계에 안전하게 태운다
 * 
 * @p_slot: 끌 슬롯.
 * @return: 0 이면 성공, 음수 errno 또는 shpchp 에러 코드면 실패. 상태 때문에 아무것도
 *          하지 않으면 초깃값 -ENODEV 가 그대로 나간다.
 * 
 * shpchp_sysfs_enable_slot() 의 정확한 거울상이다. BLINKINGOFF_STATE(버튼으로 끄기
 * 예약이 걸려 5초 대기 중)면 지연 작업을 취소하고 곧바로 끄기로 넘어가고, STATIC_STATE
 * 면 POWEROFF_STATE 로 올린 뒤 락을 놓고 shpchp_disable_slot() 을 부른다. 켜기 방향
 * 작업(BLINKINGON/POWERON)이 진행 중이면 충돌이므로 아무것도 하지 않는다.
 * 
 * 락을 놓았다 다시 잡는 이유와 상태값이 락을 대신하는 구조는 켜기 쪽과 동일하다.
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기하는 사용자 프로세스). 잠들 수 있다.
 * 에러 경로: shpchp_disable_slot() 의 반환값을 그대로 올려 보낸다.
 * 
 * 호출 체인:
 *   sysfs write → disable_slot() (shpchp_core.c) → [shpchp_sysfs_disable_slot]
 *   → shpchp_disable_slot() → remove_board() */
int shpchp_sysfs_disable_slot(struct slot *p_slot)
{
	/* [한국어] 기본 반환값 */
	int retval = -ENODEV;
	/* [한국어] 로그용 컨트롤러 포인터 */
	struct controller *ctrl = p_slot->ctrl;

	/* [한국어] 슬롯 상태를 읽고 바꾸므로 슬롯 락을 잡는다 */
	mutex_lock(&p_slot->lock);
	/* [한국어] 현재 상태에 따라 요청을 어떻게 처리할지 정한다 */
	switch (p_slot->state) {
	/* [한국어] 버튼으로 끄기 예약이 걸려 5초를 세는 중이다 */
	case BLINKINGOFF_STATE:
		/* [한국어] 사용자가 명시적으로 요청했으므로 남은 유예를 취소한다 */
		cancel_delayed_work(&p_slot->work);
		/* [한국어] 곧바로 아래 STATIC_STATE 처리로 흘려보낸다 — 의도된 fallthrough 다 */
		fallthrough;
	/* [한국어] 평시 상태 — 바로 끄기를 시작한다 */
	case STATIC_STATE:
		/* [한국어] 끄는 중으로 표시해 다른 경로가 끼어들지 못하게 한다 */
		p_slot->state = POWEROFF_STATE;
		/* [한국어] 긴 작업 전에 슬롯 락을 놓는다 */
		mutex_unlock(&p_slot->lock);
		/* [한국어] 실제 비활성화. 내부에서 ctrl->crit_sect 를 잡고 장치 제거까지 수행한다 */
		retval = shpchp_disable_slot(p_slot);
		/* [한국어] 작업이 끝났으니 상태를 되돌리기 위해 다시 락을 잡는다 */
		mutex_lock(&p_slot->lock);
		/* [한국어] 평시 상태로 복귀 */
		p_slot->state = STATIC_STATE;
		break;
	/* [한국어] 이미 끄는 작업이 진행 중이다 */
	case POWEROFF_STATE:
		/* [한국어] 중복 요청임을 알린다 */
		ctrl_info(ctrl, "Slot %s is already in powering off state\n",
			  slot_name(p_slot));
		break;
	/* [한국어] 버튼으로 켜기 예약이 걸린 상태 */
	case BLINKINGON_STATE:
	/* [한국어] 켜는 작업이 진행 중인 상태 */
	case POWERON_STATE:
		/* [한국어] 둘 다 끄기 요청과 방향이 충돌하므로 아무것도 하지 않고 알리기만 한다 */
		ctrl_info(ctrl, "Already disabled on slot %s\n",
			  slot_name(p_slot));
		break;
	/* [한국어] 다섯 상태 어디에도 없는 값 — 비정상이다 */
	default:
		/* [한국어] 오류 로그만 남긴다 */
		ctrl_err(ctrl, "Not a valid state on slot %s\n",
			 slot_name(p_slot));
		break;
	}
	/* [한국어] 슬롯 락 해제 — 모든 분기가 이 줄을 지난다 */
	mutex_unlock(&p_slot->lock);

	/* [한국어] 성공/실패를 sysfs 쓰기 경로로 돌려준다 */
	return retval;
}
