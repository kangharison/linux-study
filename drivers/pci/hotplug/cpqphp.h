/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/*
 * [한국어 설명] Compaq 핫플러그 드라이버의 공용 헤더 (cpqphp.h)
 *
 * === 파일의 역할 ===
 * 이 드라이버를 이루는 파일 다섯이 공유하는 것을 전부 담는다. 크게 넷이다.
 *
 * 첫째, **자료구조 정의.** struct controller(컨트롤러 하나),
 * struct slot(사용자에게 보이는 물리 슬롯), struct pci_func(PCI 함수 하나),
 * struct pci_resource(자유 목록의 노드), struct resource_lists(그 목록
 * 네 개의 꾸러미)가 여기 있다. 이 다섯의 관계가 곧 드라이버의 뼈대다.
 *
 * 둘째, **하드웨어 표의 형태.** 컨트롤러 MMIO 레지스터(struct ctrl_reg),
 * ROM 안의 Hot Plug Resource Table(struct hrt, struct slot_rt),
 * SMBIOS 표(struct smbios_entry_point 등)를 C 구조체로 적어 두고,
 * 각각에 대해 **offsetof 로 오프셋 enum 을 만든다.** 코드가 그 enum 을
 * readb/readl 의 오프셋으로 쓰므로, 구조체 정의를 고치면 오프셋이 따라
 * 바뀐다. 매직 넘버를 쓰지 않으면서 packed 구조체의 배치를 그대로
 * 오프셋으로 삼는 방식이다.
 *
 * 셋째, **레지스터를 두드리는 인라인 함수 스물.** LED 를 켜고 끄고,
 * 슬롯에 전원을 넣고 빼고, 명령을 밀어내고(set_SOGO), 그 완료를
 * 기다린다(wait_for_ctrl_irq). **cpqphp_ctrl.c 와 cpqphp_core.c 가
 * 레지스터 오프셋을 거의 직접 쓰지 않는 것은 이 함수들 덕이다** --
 * 두 파일은 amber_LED_on(ctrl, slot) 같은 이름만 부른다.
 *
 * 넷째, **로그 매크로와 상수.** dbg/err/info/warn, 이벤트 종류,
 * 슬롯 상태, 오류 코드, 사용자에게 보여 줄 메시지 문자열이 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 파일 다섯이 모두 이 헤더를 include 한다. 즉 이 파일은
 * 계층의 어느 한 층이 아니라 **모든 층이 딛고 선 바닥** 이다.
 *
 *   cpqphp_core.c  : 모듈 진입점, 컨트롤러 탐지, 슬롯 등록.
 *                    여기서 struct controller 를 만들고 채운다.
 *   cpqphp_ctrl.c  : 이벤트 상태 기계와 자원 할당기.
 *                    인라인 함수를 가장 많이 부르는 파일이다.
 *   cpqphp_pci.c   : 설정공간 저장·복원, HRT 읽기.
 *                    struct hrt 와 struct slot_rt 의 오프셋 enum 을 쓴다.
 *   cpqphp_nvram.c : NVRAM 접근. **이미 주석된 cpqphp_nvram.h 가
 *                    따로 있다.**
 *   cpqphp_sysfs.c : debugfs 파일. 이 헤더가 선언한 네 함수를 구현한다.
 *
 * **이 헤더 자체는 아무것도 실행하지 않는다.** 컴파일 단위마다 인라인
 * 함수의 사본이 생기고, 그것이 호출자 안으로 펼쳐진다.
 *
 * 아래쪽의 `#include <asm/pci_x86.h>` 가 특이하다. 헤더 맨 위가 아니라
 * cpqhp_routing_table_length() 바로 앞, 파일 끝 근처에 있다.
 * 그 함수가 struct irq_routing_table 과 struct irq_info 의 크기를 알아야
 * 하기 때문인데, **그 헤더는 이 스파스 체크아웃에 없어**(arch/ 와
 * include/asm 부재) 두 구조체의 정의를 확인하지 못했다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 **의존하는** 쪽:
 *   linux/interrupt.h : irqreturn_t. cpqhp_ctrl_intr 의 선언에 쓴다.
 *   linux/io.h        : readb/readw/readl 과 writeb/writew/writel.
 *                       **인라인 함수 전부가 이것에 기댄다.**
 *                       오른쪽 원문 주석이 그 사실을 밝힌다.
 *   linux/delay.h     : msleep_interruptible. wait_for_ctrl_irq 가 쓴다.
 *   linux/mutex.h     : struct mutex. controller 의 crit_sect 필드다.
 *   linux/sched/signal.h : signal_pending. wait_for_ctrl_irq 가 쓴다.
 *   asm/pci_x86.h     : struct irq_routing_table. **이 트리에 없다.**
 *   그 밖에 struct hotplug_slot, struct timer_list, struct pci_dev,
 *   struct work_struct, wait_queue_head_t 를 쓰는데, 그 정의를 담은
 *   헤더를 이 파일이 직접 include 하지 않는다 -- 포함하는 .c 파일이
 *   먼저 linux/pci.h 와 linux/pci_hotplug.h 를 include 하는 데 기대는
 *   구조다. 지금 기준으로는 헤더가 자기 의존을 스스로 갖추지 않은 것이다.
 *
 * 이 파일이 **선언만 하고 정의는 다른 곳에 있는** 것:
 *   함수 선언 스물여섯 개가 네 묶음으로 나뉘어 있다 -- debugfs(4),
 *     controller(9), resource(1), pci(12). 각 묶음 위의 원문 주석이
 *     어느 파일에 구현이 있는지를 사실상 알려 준다.
 *   전역 일곱 개(cpqhp_debug, cpqhp_legacy_mode, cpqhp_ctrl_list,
 *     cpqhp_slot_list[256], cpqhp_routing_table, cpqhp_nic_irq,
 *     cpqhp_disk_irq). 앞의 다섯은 cpqphp_core.c 가, 뒤의 둘은
 *     cpqphp_pci.c 가 정의한다.
 *
 * 데이터 흐름 -- 세 구조체가 같은 슬롯을 세 관점으로 본다:
 *   struct slot     : "사용자가 보는 슬롯". sysfs 에 등록되고, LED 상태와
 *     5초 타이머와 상태 기계의 state 를 든다. ctrl->slot 리스트에 달린다.
 *   struct pci_func : "PCI 버스 위의 함수". 설정공간 사본과 자원 목록을
 *     든다. 전역 cpqhp_slot_list[bus] 리스트에 달린다.
 *   struct controller : 둘을 모두 소유하고, MMIO 와 자유 목록과 이벤트
 *     큐를 든다.
 * 슬롯 하나에 struct slot 은 하나지만 struct pci_func 은 여럿일 수
 * 있다(다중 함수 카드). 두 리스트가 따로 관리되는 이유가 그것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체:
 *   struct ctrl_reg     : 컨트롤러 MMIO 레지스터의 배치. packed 이며,
 *     오른쪽 원문 주석이 각 필드의 오프셋을 적어 두었다.
 *     **다만 그 주석의 숫자와 실제 offsetof 가 어긋나는 자리가 있다**
 *     -- 아래 해당 필드에서 짚는다.
 *   struct hrt / struct slot_rt : ROM 안 자원 표의 헤더와 항목.
 *     cpqphp_pci.c 의 cpqhp_find_available_resources 가 읽는다.
 *   struct pci_func     : PCI 함수 하나. config_space[0x20] 이 설정공간
 *     128바이트 사본이고, 자원 목록 넷이 매달린다.
 *   struct slot         : 물리 슬롯 하나. state 가 상태 기계의 현재
 *     위치이고, task_event 가 버튼 취소를 위한 5초 타이머다.
 *   struct controller   : 컨트롤러 하나. **event_queue[10] 고정 링 버퍼와
 *     next_event 가 이 드라이버 이벤트 처리의 뼈대다.**
 *   struct pci_resource : 자유 목록 노드. base 와 length 와 next 뿐이다.
 *   struct irq_mapping  : IRQ 배정 상태. barber_pole 이 인터럽트 스위즐링
 *     위치를 든다.
 *
 * 인라인 함수(스물):
 *   set_SOGO            : "지금 설정한 값을 슬롯으로 밀어내라" 는 명령.
 *   wait_for_ctrl_irq   : 그 명령의 완료를 기다린다. 아래 부가 절 참조.
 *   amber_LED_on/off, read_amber_LED, green_LED_on/off/blink : LED 제어.
 *   slot_enable/disable, is_slot_enabled, read_slot_enable : 슬롯을
 *     버스에 붙이고 뗀다.
 *   enable_slot_power/disable_slot_power : **버스에 붙이지 않고 전원만**
 *     넣고 뺀다. 위의 슬롯 활성화와 구별되며, 속도 협상 전에 카드를
 *     살짝 켜 보는 데 쓴다.
 *   get_controller_speed / get_adapter_speed : 컨트롤러와 카드의 속도.
 *   cpq_get_attention_status, get_slot_enabled, cpq_get_latch_status,
 *     get_presence_status : sysfs 콜백이 쓰는 상태 조회.
 *   return_resource     : 자유 목록 머리에 노드를 도로 매단다.
 *   slot_name, to_slot  : 핫플러그 코어의 struct hotplug_slot 과
 *     이 드라이버의 struct slot 사이를 오간다.
 *   cpqhp_routing_table_length : IRQ 라우팅 표의 항목 수.
 *
 * === offsetof 로 오프셋 enum 을 만드는 방식 ===
 * 이 헤더의 절반 가까이가 이 관용구다. 표마다 두 벌이 짝을 이룬다 --
 * packed 구조체 하나와, 그 필드마다 offsetof 를 취한 enum 하나다.
 *
 * 얻는 것은 분명하다. 코드가 `readb(ctrl->hpc_reg + SLOT_MASK)` 라고
 * 쓸 수 있고, 그 SLOT_MASK 는 매직 넘버가 아니라 구조체 배치에서
 * 자동으로 계산된 값이다. 표의 형태가 바뀌면 오프셋이 따라 바뀐다.
 *
 * 대가도 있다. **구조체가 반드시 packed 여야 하고**, 컴파일러가 필드를
 * 재배치하지 않는다는 전제에 기댄다. 그리고 오프셋 이름이 전역
 * 이름공간을 차지한다 -- SLOT_MASK, MISC, IO_BASE 같은 흔한 이름이
 * enum 상수로 풀려 있어 다른 헤더와 충돌할 여지가 있다.
 *
 * === 명령과 대기 -- set_SOGO 와 wait_for_ctrl_irq ===
 * 이 컨트롤러는 LED 나 슬롯 전원을 바꿀 때 레지스터에 값을 쓰는 것만으로
 * 끝나지 않는다. 값을 써 둔 뒤 **set_SOGO 로 "지금 밀어내라" 고 지시**
 * 해야 실제로 슬롯에 반영된다. 그 함수는 MISC 레지스터의 비트 0 을
 * 세우고 비트 2 를 지운다.
 *
 * 그다음 wait_for_ctrl_irq 로 완료를 기다린다. **그 함수가 코드로
 * 말하는 것은 이렇다** -- ctrl->queue 대기 큐에 자기를 걸고,
 * msleep_interruptible(1000)을 부르고, 큐에서 빠지고, 시그널이 있었으면
 * -EINTR 을 돌려준다. 즉 **wait_event 계열이 아니라 무조건 1초를 잔다.**
 *
 * 한편 cpqphp_ctrl.c 의 cpqhp_ctrl_intr 은 Serial Output 인터럽트를
 * 받으면 wake_up_interruptible 로 그 큐를 깨운다. 대기 큐 등록과 깨움이
 * 짝을 이루고 있는 것은 분명하다.
 *
 * **다만 그 깨움이 실제로 1초를 줄여 주는지는 이 트리에서 확인할 수
 * 없다.** msleep_interruptible 의 구현이 이 스파스 체크아웃에 없기
 * 때문이다(kernel/ 부재). 코드가 말하는 사실은 "대기 큐에 등록만 하고
 * 무조건 1초를 잔다" 까지이고, 그 너머는 확인 대상 밖이다.
 *
 * 실용적 결과는 뚜렷하다. cpqphp_ctrl.c 가 LED 하나 바꿀 때마다 이
 * 대기를 하므로, 카드 하나를 켜고 끄는 데 초 단위가 걸린다.
 * cpqhp_hardware_test 의 LED 시험이 수십 초 걸리는 이유도 이것이다.
 *
 * === 2000년대 초 관용구에 대하여 ===
 *   자체 로그 매크로 : dbg/err/info/warn 이 printk 로 직접 정의되어
 *     있다. dbg 는 전역 cpqhp_debug 가 참일 때만 찍는다. dev_dbg 계열이나
 *     pr_fmt 관용구가 자리 잡기 전이며, MY_NAME 문자열을 손으로 붙인다.
 *   **`err` 라는 이름의 매크로** : 지역 변수 이름으로 흔한 err 를
 *     매크로로 잡아 두었다. cpqphp_core.c 의 cpqhpc_probe 가 `int err;`
 *     를 선언하면서도 err(msg_...) 를 부르는데, 매크로가 인자 있는
 *     형태라 인자 없는 `err` 는 변수로 남아 둘이 공존한다.
 *   헤더가 자기 의존을 갖추지 않음 : 위의 타 모듈 절에 적은 대로,
 *     struct pci_dev 등의 정의를 포함하는 쪽에 맡긴다.
 *   include 가 파일 중간에 : asm/pci_x86.h 가 파일 끝 근처에 있다.
 *   **CTRL_RESERVED2 의 값이 CTRL_RESERVED1 과 같다** :
 *     offsetof(struct ctrl_reg, reserved1) 을 두 번 쓴다. 아래에서 짚는다.
 *
 * === 값의 근거에 대하여 ===
 *   레지스터 비트의 의미는 인라인 함수가 그것을 어떻게 쓰는지가 유일한
 *     근거다. 공개 문서가 트리에 없으므로, 각 비트를 "어느 함수가
 *     세우고 지우는지" 로만 설명한다.
 *   PCI_HPC_ID 계열, PCISLOT_ 계열, 오류 코드 값의 출처도 이 파일뿐이다.
 *   asm/pci_x86.h 와 include/linux/pci_hotplug.h 가 이 스파스 체크아웃에
 *     없어, struct irq_routing_table, struct irq_info,
 *     struct hotplug_slot, hotplug_slot_name 의 정의를 확인하지 못했다.
 *   SMBIOS 표의 형식은 SMBIOS 규격이 정하는데 그 문서도 트리에 없다.
 *     구조체 필드 이름과 cpqphp_core.c 의 쓰임으로만 설명한다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 헤더의 어떤 심볼도 쓰지 않는다(이 트리에서 전수
 * 확인: 0건). 2001년 PCI/PCI-X 서버용 하드웨어라 시대가 겹치지 않는다.
 *
 * 다만 struct slot 과 struct pci_func 을 나눈 발상 -- "사용자가 보는
 * 슬롯" 과 "PCI 버스 위의 함수" 를 따로 관리하는 것 -- 은 지금 NVMe
 * 드라이브를 핫스왑할 때 pci_hotplug 코어가 struct hotplug_slot 과
 * struct pci_dev 를 나누는 것과 같은 구분이다. 이 파일의 to_slot()
 * 이 그 두 세계를 잇는 다리이며, 그 관용구는 지금도 그대로 쓰인다.
 */

#ifndef _CPQPHP_H
#define _CPQPHP_H

#include <linux/interrupt.h>
#include <linux/io.h>		/* for read? and write? functions */
#include <linux/delay.h>	/* for delays */
#include <linux/mutex.h>
#include <linux/sched/signal.h>	/* for signal_pending() */

#define MY_NAME	"cpqphp"

#define dbg(fmt, arg...) do { if (cpqhp_debug) printk(KERN_DEBUG "%s: " fmt, MY_NAME, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)



struct smbios_system_slot {
	/* [한국어] SMBIOS 구조체 종류. **슬롯은 9번** 이며, get_SMBIOS_entry 가 그 값으로
	 * 표를 훑는다(cpqphp_core.c 의 ctrl_slot_setup 이 9 를 넘긴다).
	 * 설정자: 시스템 펌웨어가 ROM 에 써 둔다. 커널은 읽기만 한다.
	 * 읽는 자: get_SMBIOS_entry 가 SMBIOS_GENERIC_TYPE 오프셋으로 읽는다.
	 * 동기화: ROM 영역이라 불변 */
	u8 type;
	/* [한국어] 이 항목의 길이(바이트). **가변 길이 표를 훑는 근거다** --
	 * get_subsequent_smbios_entry 가 이 값을 더해 다음 항목으로 넘어간다.
	 * 설정자: 펌웨어.
	 * 읽는 자: get_subsequent_smbios_entry 가 SMBIOS_GENERIC_LENGTH 로 읽는다.
	 * 동기화: 불변 */
	u8 length;
	/* [한국어] 이 항목의 핸들(식별자). **이 드라이버는 쓰지 않는다** --
	 * 오프셋 enum 에 SMBIOS_SLOT_GENERIC_HANDLE 이 정의되어 있으나
	 * 참조하는 코드가 없다.
	 * 동기화: 불변 */
	u16 handle;
	/* [한국어] 슬롯 이름 문자열의 번호. **이 드라이버는 쓰지 않는다** --
	 * 슬롯 이름은 SMBIOS 가 아니라 슬롯 번호를 문자열로 만들어 쓴다
	 * (ctrl_slot_setup 의 snprintf).
	 * 동기화: 불변 */
	u8 name_string_num;
	/* [한국어] 슬롯의 물리적 종류. **is_slot66mhz(cpqphp_core.c)가 이 값이 0x0E 인지
	 * 본다** -- 그러면 66MHz 슬롯으로 판정해 capabilities 에
	 * PCISLOT_66_MHZ_SUPPORTED 를 넣는다.
	 * 값 범위: SMBIOS 규격이 정하는 코드. **그 규격 문서가 이 트리에 없어**
	 *   0x0E 가 무엇을 뜻하는지는 그 함수의 쓰임으로만 알 수 있다.
	 * 동기화: 불변 */
	u8 slot_type;
	/* [한국어] 슬롯의 데이터 폭. **is_slot64bit(cpqphp_core.c)가 이 값이 0x06 인지
	 * 본다** -- 그러면 64비트 슬롯으로 판정한다.
	 * 값 범위: 역시 SMBIOS 규격의 코드이며 이 트리에서 확인할 수 없다.
	 * 동기화: 불변 */
	u8 slot_width;
	/* [한국어] 슬롯이 현재 쓰이고 있는지. **이 드라이버는 쓰지 않는다** --
	 * 카드 존재 여부는 SMBIOS 가 아니라 컨트롤러 레지스터에서 읽는다
	 * (get_presence_status).
	 * 동기화: 불변 */
	u8 slot_current_usage;
	/* [한국어] 슬롯의 물리적 길이. **이 드라이버는 쓰지 않는다.**
	 * 동기화: 불변 */
	u8 slot_length;
	/* [한국어] **이 표에서 이 드라이버가 가장 중요하게 쓰는 필드다.**
	 * ctrl_slot_setup 이 자기가 만든 슬롯의 number 와 이 값이 맞는 항목을
	 * 찾을 때까지 표를 훑고, 찾으면 그 항목의 주소를 slot->p_sm_slot 에
	 * 저장한다. 그 뒤 is_slot64bit / is_slot66mhz 가 그 주소로 위의
	 * 두 필드를 읽는다.
	 * 설정자: 펌웨어.
	 * 읽는 자: ctrl_slot_setup 이 SMBIOS_SLOT_NUMBER 오프셋으로 읽는다.
	 * 동기화: 불변 */
	u16 slot_number;
	/* [한국어] 슬롯 속성 바이트 1. **이 드라이버는 쓰지 않는다.**
	 * 동기화: 불변 */
	u8 properties1;
	/* [한국어] 슬롯 속성 바이트 2. **이 드라이버는 쓰지 않는다.**
	 * 오프셋 enum 에는 정의되어 있으나 참조하는 코드가 없다.
	 * 동기화: 불변 */
	u8 properties2;
/* [한국어] **packed 가 필수다.** 아래 enum 이 offsetof 로 오프셋을 뽑아
 * readb/readw 의 인자로 쓰므로, 컴파일러가 정렬 패딩을 넣으면
 * 펌웨어가 써 둔 실제 배치와 어긋난다 */
} __attribute__ ((packed));

/* offsets to the smbios generic type based on the above structure layout */
/* [한국어] 위 구조체의 필드마다 offsetof 를 취해 만든 오프셋 상수 모음.
 * **이 헤더에서 되풀이되는 관용구의 첫 번째 예다** --
 * 표 구조체 하나와 오프셋 enum 하나가 짝을 이룬다.
 * 오른쪽 원문 주석이 그 방식을 밝힌다 */
enum smbios_system_slot_offsets {
	SMBIOS_SLOT_GENERIC_TYPE =	offsetof(struct smbios_system_slot, type),
	SMBIOS_SLOT_GENERIC_LENGTH =	offsetof(struct smbios_system_slot, length),
	SMBIOS_SLOT_GENERIC_HANDLE =	offsetof(struct smbios_system_slot, handle),
	SMBIOS_SLOT_NAME_STRING_NUM =	offsetof(struct smbios_system_slot, name_string_num),
	SMBIOS_SLOT_TYPE =		offsetof(struct smbios_system_slot, slot_type),
	SMBIOS_SLOT_WIDTH =		offsetof(struct smbios_system_slot, slot_width),
	SMBIOS_SLOT_CURRENT_USAGE =	offsetof(struct smbios_system_slot, slot_current_usage),
	SMBIOS_SLOT_LENGTH =		offsetof(struct smbios_system_slot, slot_length),
	SMBIOS_SLOT_NUMBER =		offsetof(struct smbios_system_slot, slot_number),
	SMBIOS_SLOT_PROPERTIES1 =	offsetof(struct smbios_system_slot, properties1),
	SMBIOS_SLOT_PROPERTIES2 =	offsetof(struct smbios_system_slot, properties2),
};

struct smbios_generic {
	/* [한국어] 구조체 종류. **모든 SMBIOS 항목이 공통으로 갖는 머리 세 필드 중
	 * 첫째다.** get_SMBIOS_entry 가 원하는 종류를 찾을 때 이것을 본다.
	 * 설정자: 펌웨어. 읽는 자: get_SMBIOS_entry.
	 * 동기화: 불변 */
	u8 type;
	/* [한국어] 항목 길이. **표를 훑는 데 쓰이는 필드다** --
	 * get_subsequent_smbios_entry 가 현재 항목 주소에 이 값을 더해
	 * 다음 항목의 시작으로 간다.
	 * 설정자: 펌웨어. 읽는 자: get_subsequent_smbios_entry.
	 * 동기화: 불변 */
	u8 length;
	/* [한국어] 항목 핸들. **이 드라이버는 쓰지 않는다.**
	 * 동기화: 불변 */
	u16 handle;
/* [한국어] **packed 가 필수다** -- 아래 enum 의 offsetof 가 펌웨어의 실제
 * 배치와 일치해야 한다 */
} __attribute__ ((packed));

/* offsets to the smbios generic type based on the above structure layout */
/* [한국어] 공통 머리의 오프셋 모음. **종류가 무엇이든 이 세 오프셋은 같으므로**,
 * get_SMBIOS_entry 가 항목의 실제 종류를 모른 채 종류와 길이를 읽을 수 있다.
 * 그것이 가변 길이 표를 훑는 열쇠다 */
enum smbios_generic_offsets {
	SMBIOS_GENERIC_TYPE =	offsetof(struct smbios_generic, type),
	SMBIOS_GENERIC_LENGTH =	offsetof(struct smbios_generic, length),
	SMBIOS_GENERIC_HANDLE =	offsetof(struct smbios_generic, handle),
};

struct smbios_entry_point {
	/* [한국어] **"_SM_" 네 글자 서명.** detect_SMBIOS_pointer 가 ROM 을 16바이트씩
	 * 훑으며 이 문자열을 찾아 표의 시작을 알아낸다.
	 * 설정자: 펌웨어. 읽는 자: detect_SMBIOS_pointer 는 오프셋 enum 을 쓰지 않고
	 *   fp, fp+1, fp+2, fp+3 을 직접 읽는다 -- ANCHOR 상수가 정의되어
	 *   있는데도 쓰이지 않는다.
	 * 동기화: 불변 */
	char anchor[4];
	/* [한국어] 진입점 구조체의 체크섬. **이 드라이버는 검증하지 않는다** --
	 * 서명만 보고 표를 믿는다.
	 * 동기화: 불변 */
	u8 ep_checksum;
	/* [한국어] 진입점 구조체의 길이. **이 드라이버는 쓰지 않는다.**
	 * 동기화: 불변 */
	u8 ep_length;
	/* [한국어] SMBIOS 규격의 주 버전. **이 드라이버는 쓰지 않는다** --
	 * 표 형식이 버전에 따라 달라질 수 있는데도 확인하지 않는다.
	 * 동기화: 불변 */
	u8 major_version;
	/* [한국어] 부 버전. **역시 쓰지 않는다.**
	 * 동기화: 불변 */
	u8 minor_version;
	/* [한국어] 가장 큰 항목의 크기. **쓰지 않는다.**
	 * 동기화: 불변 */
	u16 max_size_entry;
	/* [한국어] 진입점 구조체의 리비전. **쓰지 않는다.**
	 * 동기화: 불변 */
	u8 ep_rev;
	/* [한국어] 예약 영역 5바이트. **오프셋 enum 에도 없다** -- 건너뛰기 위한
	 * 자리 채우기이며, 그래야 뒤 필드들의 offsetof 가 맞는다.
	 * 동기화: 불변 */
	u8 reserved[5];
	/* [한국어] 중간 진입점의 "_DMI_" 서명. **이 드라이버는 쓰지 않는다.**
	 * 동기화: 불변 */
	char int_anchor[5];
	/* [한국어] 중간 진입점의 체크섬. **쓰지 않는다.**
	 * 동기화: 불변 */
	u8 int_checksum;
	/* [한국어] **구조체 표 전체의 바이트 길이.** 두 곳에서 쓰인다 --
	 * get_subsequent_smbios_entry 가 표의 끝(p_max)을 계산할 때,
	 * 그리고 one_time_init 이 ioremap 할 크기를 정할 때다.
	 * 설정자: 펌웨어. 읽는 자: 그 둘이 ST_LENGTH 오프셋으로 읽는다.
	 * 동기화: 불변 */
	u16 st_length;
	/* [한국어] **구조체 표의 물리 주소.** one_time_init 이 이 값을 ioremap 해
	 * smbios_start 를 얻는다. 즉 진입점은 ROM 안에 있고, 실제 표는
	 * 다른 곳에 있을 수 있다.
	 * 설정자: 펌웨어. 읽는 자: one_time_init 이 ST_ADDRESS 오프셋으로 읽는다.
	 * 동기화: 불변 */
	u32 st_address;
	/* [한국어] 표에 든 항목의 개수. **이 드라이버는 쓰지 않는다** --
	 * 개수 대신 표의 끝 주소까지 훑는 방식을 쓴다.
	 * 동기화: 불변 */
	u16 number_of_entrys;
	/* [한국어] BCD 형식의 리비전. **쓰지 않는다.**
	 * 동기화: 불변 */
	u8 bcd_rev;
/* [한국어] **packed 가 필수다.** 특히 char 배열과 u16/u32 가 섞여 있어
 * 패딩이 들어가면 st_address 와 st_length 의 오프셋이 어긋난다 */
} __attribute__ ((packed));

/* offsets to the smbios entry point based on the above structure layout */
/* [한국어] 진입점 구조체의 오프셋 모음.
 * **이 중 실제로 쓰이는 것은 ST_LENGTH 와 ST_ADDRESS 둘뿐이다.**
 * 나머지 열한 개는 표의 형태를 기록해 둔 문서 역할만 한다 */
enum smbios_entry_point_offsets {
	ANCHOR =		offsetof(struct smbios_entry_point, anchor[0]),
	EP_CHECKSUM =		offsetof(struct smbios_entry_point, ep_checksum),
	EP_LENGTH =		offsetof(struct smbios_entry_point, ep_length),
	MAJOR_VERSION =		offsetof(struct smbios_entry_point, major_version),
	MINOR_VERSION =		offsetof(struct smbios_entry_point, minor_version),
	MAX_SIZE_ENTRY =	offsetof(struct smbios_entry_point, max_size_entry),
	EP_REV =		offsetof(struct smbios_entry_point, ep_rev),
	INT_ANCHOR =		offsetof(struct smbios_entry_point, int_anchor[0]),
	INT_CHECKSUM =		offsetof(struct smbios_entry_point, int_checksum),
	ST_LENGTH =		offsetof(struct smbios_entry_point, st_length),
	ST_ADDRESS =		offsetof(struct smbios_entry_point, st_address),
	NUMBER_OF_ENTRYS =	offsetof(struct smbios_entry_point, number_of_entrys),
	BCD_REV =		offsetof(struct smbios_entry_point, bcd_rev),
};

struct ctrl_reg {			/* offset */
	/* [한국어] 슬롯 리셋 제어 레지스터(오프셋 0x00).
	 * **이 드라이버는 이 레지스터를 쓰지 않는다** -- 오프셋 enum 에
	 * SLOT_RST 가 정의되어 있으나 참조하는 코드가 없다. 슬롯 리셋은
	 * AFI 쪽이 아니라 PERST 신호로 처리되기 때문으로 보인다.
	 * 동기화: 해당 없음 */
	u8	slot_RST;		/* 0x00 */
	/* [한국어] 슬롯을 버스에 붙이는 활성화 비트 여덟 개(0x01). 슬롯당 비트 하나다.
	 * 설정자: slot_enable(), slot_disable() 이 읽고-고쳐-쓴다.
	 *   cpqphp_ctrl.c 의 set_controller_speed 가 주파수를 바꾸는 동안
	 *   0 으로 통째로 지웠다가 되돌린다.
	 * 읽는 자: is_slot_enabled(), read_slot_enable().
	 * **전원(slot_power)과 별개다** -- 이쪽은 버스 연결이다.
	 * 동기화: 호출자가 ctrl->crit_sect 를 쥔다 */
	u8	slot_enable;		/* 0x01 */
	/* [한국어] 기타 제어·상태 레지스터(0x02). **여러 용도가 섞여 있다.**
	 *   비트 0    : set_SOGO() 가 세우는 "밀어내기 시작" 지시.
	 *   비트 2    : Serial Output 완료. cpqhp_ctrl_intr 이 검사하고 지운다.
	 *               set_SOGO 가 지시를 내리며 함께 지운다.
	 *   비트 3    : 범용 입력 변화. cpqhp_ctrl_intr 이 검사한다.
	 *   비트 11   : 66MHz 동작 여부. get_controller_speed 가 PCI-X 를
	 *               지원하지 않는 컨트롤러에서 읽는다.
	 *   0x4006    : cpqhpc_probe 가 세우는 조합(비트 1, 2, 14).
	 *               Shift Out 인터럽트와 전원 결함 SERR 을 켜는 것으로
	 *               보이나 **개별 비트의 근거 문서는 이 트리에 없다.**
	 *   0xFFFD    : unload_cpqphpd 가 비트 1 을 지울 때 쓰는 마스크.
	 * 동기화: crit_sect 또는 인터럽트 컨텍스트 */
	u16	misc;			/* 0x02 */
	/* [한국어] LED 제어 레지스터(0x04). **32비트가 슬롯 여섯 개의 황색·녹색을
	 * 나눠 담는다.**
	 *   하위 16비트 : 녹색. green_LED_ 계열이 `0x0101L << slot` 으로 다룬다.
	 *   상위 16비트 : 황색. amber_LED_ 계열이 `0x01010000L << slot` 을 쓴다.
	 *   LED 하나가 비트 두 개이고, 그 조합이 꺼짐/깜빡임/켜짐 세 상태를
	 *   나타내는 것으로 읽힌다(green_LED_blink 가 하나만 세운다).
	 * 설정자: LED 인라인 함수 여섯, cpqhp_hardware_test 가 시험 패턴을 쓸 때.
	 * 읽는 자: read_amber_LED(), set_controller_speed 가 저장·복원할 때.
	 * **그 비트 배치의 근거 문서는 이 트리에 없다** -- 인라인 함수들이
	 *   쓰는 마스크에서 읽어 낸 것이다.
	 * 동기화: 호출자가 crit_sect 를 쥔다 */
	u32	led_control;		/* 0x04 */
	/* [한국어] **인터럽트 입력 상태이자 지우기 레지스터(0x08).**
	 * 이 드라이버에서 가장 여러 뜻으로 읽히는 레지스터다.
	 *   하위 8비트    : 슬롯 레버 상태. 비트가 서면 **열린** 것이다.
	 *                   cpq_get_latch_status 가 뒤집어 돌려주고,
	 *                   cpqhp_process_SI 가 열려 있으면 켜기를 거절한다.
	 *   비트 8~15     : 전원 결함. handle_power_fault 가 본다.
	 *   상위 16비트   : 카드 존재. **감지 핀이 두 벌이라** 비트 24 쪽과
	 *                   16 쪽을 OR 로 합쳐 읽는다(get_presence_status).
	 *                   0 이 "있음" 인 음논리다.
	 * 설정자: cpqhp_ctrl_intr 이 변화한 비트를 되써서 지운다.
	 *   cpqhpc_probe 가 시작할 때 0xFFFFFFFF 로 전부 지운다.
	 * 읽는 자: cpqhp_ctrl_intr, cpq_get_latch_status, get_presence_status,
	 *   board_replaced, cpqhp_process_SI, ctrl_slot_setup.
	 * 동기화: 인터럽트와 프로세스 컨텍스트가 함께 읽는다 */
	u32	int_input_clear;	/* 0x08 */
	/* [한국어] 인터럽트 마스크 레지스터.
	 * **오른쪽 원문 주석이 0x0a 라고 적었으나 실제 offsetof 는 0x0c 다** --
	 * 앞의 int_input_clear 가 u32 이므로 0x08 + 4 = 0x0c 이다.
	 * 코드는 이 주석이 아니라 enum 의 offsetof 를 쓰므로 동작은 정상이며,
	 * 장식 주석만 어긋나 있다. 코드는 손대지 않고 사실만 적어 둔다.
	 * 설정자: cpqhpc_probe 가 0xFFFFFFFF 로 전부 막았다가 설정을 마친 뒤
	 *   0 으로 열고, unload_cpqphpd 가 꺼진 슬롯만 다시 막는다.
	 * 읽는 자: 없다 -- 쓰기 전용으로만 쓰인다.
	 * 동기화: crit_sect 밖에서도 쓰인다(probe 경로) */
	u32	int_mask;		/* 0x0a */
	/* [한국어] 예약 필드들(0x10~0x13 구간의 앞 세 개).
	 * **하드웨어가 쓰지 않는 자리이거나 이 드라이버가 다루지 않는 기능이다.**
	 * 오프셋 enum 에 CTRL_RESERVED0~2 가 정의되어 있으나 참조하는 코드가 없다.
	 * 구조체에 자리를 잡아 두어야 뒤 필드들의 offsetof 가 맞는다.
	 * 동기화: 해당 없음 */
	u8	reserved0;		/* 0x10 */
	/* [한국어] 위 예약 묶음의 두 번째. **CTRL_RESERVED1 과 CTRL_RESERVED2 가
	 * 둘 다 이 필드의 offsetof 를 쓴다** -- 아래 enum 의 복사·붙여넣기
	 * 실수로 보이며, 두 상수의 값이 같아진다. 다만 둘 다 참조되지 않아
	 * 실제 동작에는 영향이 없다 */
	u8	reserved1;		/* 0x11 */
	/* [한국어] 위 예약 묶음의 세 번째. CTRL_RESERVED2 는 이 필드가 아니라
	 * reserved1 을 가리키므로, 이 필드의 오프셋을 나타내는 상수는 없다 */
	u8	reserved2;		/* 0x12 */
	/* [한국어] 범용 출력 레지스터(0x13). **이 드라이버는 쓰지 않는다** --
	 * GEN_OUTPUT_AB 상수가 정의되어 있으나 참조하는 코드가 없다.
	 * 동기화: 해당 없음 */
	u8	gen_output_AB;		/* 0x13 */
	/* [한국어] **인터럽트를 일으키지 않는 입력 레지스터(0x14).**
	 * 카드가 자기 속도 능력을 알리는 신호(PCIXCAP 등)가 여기 모인다.
	 *   비트 16+slot : PCI-X 133MHz 가능
	 *   비트  8+slot : PCI-X 66MHz 가능
	 *   비트  0+slot : PCI 66MHz 가능
	 * 설정자: 하드웨어. 소프트웨어는 쓰지 않는다.
	 * 읽는 자: get_adapter_speed() 하나뿐이다.
	 * **이름대로 인터럽트를 만들지 않으므로** 상태가 바뀌어도 알림이 없고,
	 * 필요할 때 읽어 보는 방식이다.
	 * 동기화: 읽기 전용 */
	u32	non_int_input;		/* 0x14 */
	/* [한국어] 예약 필드(0x18). 참조하는 코드가 없다 */
	u32	reserved3;		/* 0x18 */
	/* [한국어] 예약 필드.
	 * **오른쪽 원문 주석이 0x1a 라고 적었으나 실제 offsetof 는 0x1c 다** --
	 * 앞의 reserved3 이 u32 이므로 0x18 + 4 = 0x1c 이다.
	 * int_mask 와 함께 이 구조체에서 장식 주석이 어긋난 두 자리다.
	 * 참조하는 코드가 없어 실제 영향은 없다 */
	u32	reserved4;		/* 0x1a */
	/* [한국어] 예약 필드(0x20). 참조하는 코드가 없다 */
	u32	reserved5;		/* 0x20 */
	/* [한국어] 예약 필드(0x24). 참조하는 코드가 없다 */
	u8	reserved6;		/* 0x24 */
	/* [한국어] 예약 필드(0x25). 참조하는 코드가 없다 */
	u8	reserved7;		/* 0x25 */
	/* [한국어] 예약 필드(0x26). u16 이라 0x27 까지 차지한다. 참조하는 코드가 없다 */
	u16	reserved8;		/* 0x26 */
	/* [한국어] **슬롯 구성 정보(0x28). 한 바이트에 두 정보가 들어 있다.**
	 *   하위 니블 : 이 컨트롤러의 슬롯 **개수**.
	 *   상위 니블 : 첫 슬롯의 **장치 번호**.
	 * 그래서 코드에 `& 0x0F` 와 `>> 4` 가 짝으로 되풀이된다 --
	 * ctrl_slot_setup, cpqhpc_probe, init_SERR, cpqhp_hardware_test,
	 * handle_presence_change 가 모두 그렇게 읽는다.
	 * 설정자: 하드웨어(보드 배선). 소프트웨어는 쓰지 않는다.
	 * 읽는 자: 위의 다섯 곳. cpqhpc_probe 는 상위 니블을
	 *   ctrl->slot_device_offset 에 저장해 두고 이후 그것을 쓴다.
	 * 동기화: 읽기 전용 */
	u8	slot_mask;		/* 0x28 */
	/* [한국어] 예약 필드(0x29). 참조하는 코드가 없다 */
	u8	reserved9;		/* 0x29 */
	/* [한국어] 예약 필드(0x2a). 참조하는 코드가 없다 */
	u8	reserved10;		/* 0x2a */
	/* [한국어] 예약 필드(0x2b). 참조하는 코드가 없다 */
	u8	reserved11;		/* 0x2b */
	/* [한국어] 슬롯별 SERR 생성 제어(0x2c). 슬롯당 비트 하나다.
	 * 설정자: init_SERR(cpqphp_core.c)이 부팅 때 0 을 쓰고,
	 *   remove_board 가 카드를 뺄 때 해당 비트를 지운다 --
	 *   **카드가 사라진 슬롯에서 오류 신호가 올라오지 않게 하려는 것이다.**
	 *   unload_cpqphpd 가 모듈을 내릴 때 0 으로 지운다.
	 * 읽는 자: remove_board 가 읽고-고쳐-쓸 때만.
	 * **init_SERR 이 루프를 돌면서도 매번 같은 자리에 0 을 쓴다** --
	 *   슬롯 번호를 반영하지 않아 사실상 한 번 쓰는 것과 같다.
	 * 동기화: crit_sect */
	u8	slot_SERR;		/* 0x2c */
	/* [한국어] 슬롯 전원 제어(0x2d). 슬롯당 비트 하나다.
	 * 설정자: enable_slot_power(), disable_slot_power().
	 *   board_added 와 board_replaced 가 **타이머 버그 우회** 로
	 *   값을 0 으로 썼다가 되돌리기도 한다 -- 값을 바꿔야 하드웨어가
	 *   한 번 더 시프트아웃을 하기 때문이라고 그쪽 원문 주석이 밝힌다.
	 *   set_controller_speed 가 주파수 변경 전후로 저장·복원한다.
	 * 읽는 자: 위 함수들이 읽고-고쳐-쓸 때.
	 * **slot_enable 과 별개다** -- 이쪽은 전기, 그쪽은 버스 연결이다.
	 * 동기화: crit_sect */
	u8	slot_power;		/* 0x2d */
	/* [한국어] 예약 필드(0x2e). **오프셋 enum 에도 없다** -- 자리 채우기 전용이다 */
	u8	reserved12;		/* 0x2e */
	/* [한국어] 예약 필드(0x2f). 역시 오프셋 enum 에 없다 */
	u8	reserved13;		/* 0x2f */
	/* [한국어] 현재·다음 버스 주파수(0x30).
	 * 읽을 때 : get_controller_speed 가 상위 니블로 현재 속도를 판정한다
	 *   (0xB0=133MHz PCI-X, 0xA0=100MHz, 0x90=66MHz PCI-X, 0x10=66MHz PCI).
	 *   **마스크가 겹치므로 높은 속도부터 차례로 검사해야 한다.**
	 * 쓸 때 : set_controller_speed 가 하위 4비트에 목표 속도 코드를 넣고
	 *   비트 15:12 에 0xB 를 넣는다. **그 두 필드의 의미와 코드 값의 근거
	 *   문서는 이 트리에 없다.**
	 * 동기화: crit_sect */
	u8	next_curr_freq;		/* 0x30 */
	/* [한국어] 리셋·주파수 모드 상태(0x31).
	 * 읽는 자: cpqhp_ctrl_intr 이 **비트 6 을 보고 버스 리셋이 끝났는지**
	 *   판단한다. 끝났으면 그 비트를 지우고(`&= 0xCF`) 대기 큐를 깨운다.
	 *   0xCF 는 비트 4 와 5 를 함께 지우는 마스크라, 비트 6 만이 아니라
	 *   세 비트를 건드리는 셈이다.
	 * 설정자: 같은 곳에서 지울 때만.
	 * **세 비트 각각의 의미는 이 트리에서 확인할 수 없다.**
	 * 동기화: 인터럽트 컨텍스트 */
	u8	reset_freq_mode;	/* 0x31 */
} __attribute__ ((packed));

/* offsets to the controller registers based on the above structure layout */
/* [한국어] **컨트롤러 MMIO 레지스터 오프셋 모음.**
 * 위 struct ctrl_reg 의 필드마다 offsetof 를 취해 만든 상수들이며,
 * 실제 접근은 모두 `ctrl->hpc_reg + <이 상수>` 꼴이다.
 * **왜 구조체를 직접 역참조하지 않는가**: MMIO 는 readb/readw/readl 로만
 *   접근해야 하므로(캐시·재정렬·바이트 폭 때문에) 구조체 포인터로
 *   필드를 읽을 수 없다. 그래서 배치는 구조체로 적어 두고 주소는
 *   오프셋으로 뽑아 쓰는 두 겹 구조가 된다.
 * **이 방식의 이점**: 필드를 하나 끼워 넣으면 뒤따르는 오프셋이 전부
 *   자동으로 밀린다. 숫자를 손으로 적어 둔 것보다 안전하다.
 * 원문 주석이 이 방식을 그대로 밝히고 있다 */
enum ctrl_offsets {
	/* [한국어] 슬롯 리셋 제어(0x00). set_SOGO 로 명령을 내리기 전에
	 * 어느 슬롯을 리셋할지 비트로 고른다.
	 * 읽는 자: cpqphp_ctrl.c 의 board_replaced 계열 */
	SLOT_RST =		offsetof(struct ctrl_reg, slot_RST),
	/* [한국어] 슬롯 전원·활성 제어(0x02).
	 * 이 파일의 enable_slot / disable_slot / is_slot_enabled 세 인라인 함수가
	 * `0x01 << slot` 비트를 세우고 지우고 읽는다.
	 * **슬롯당 한 비트** 이므로 컨트롤러 하나가 최대 8슬롯이다 */
	SLOT_ENABLE =		offsetof(struct ctrl_reg, slot_enable),
	/* [한국어] 잡다 제어·상태(0x04, u16).
	 * **이 헤더에서 가장 많이 쓰이는 오프셋이다** -- set_SOGO 가 명령 개시
	 * 비트를, get_controller_speed 가 66MHz 여부 비트를 여기서 다룬다 */
	MISC =			offsetof(struct ctrl_reg, misc),
	/* [한국어] LED 제어(0x08, u32).
	 * 녹색(전원) LED 는 `0x0101L << slot`, 황색(주의) LED 는
	 * `0x01010000L << slot` 자리를 쓴다. 이 파일의 여섯 개 LED 인라인 함수가
	 * 모두 이 오프셋을 본다 */
	LED_CONTROL =		offsetof(struct ctrl_reg, led_control),
	/* [한국어] 인터럽트 상태 읽기 겸 쓰기로 지우기(0x0c, u32).
	 * **읽으면 어떤 슬롯에서 무슨 일이 있었는지 알려 주고, 그 값을 그대로
	 * 다시 쓰면 지워진다**(write-1-to-clear).
	 * 읽는 자: cpqhp_ctrl_intr, 그리고 get_presence_status 가 감지선 비트를
	 *   뽑아낼 때 */
	INT_INPUT_CLEAR =	offsetof(struct ctrl_reg, int_input_clear),
	/* [한국어] 인터럽트 마스크(실제 0x0c 다음, 구조체의 장식 주석은 0x0a 라 적혀
	 * 있으나 앞 필드들의 크기를 더하면 0x0c 다 -- 코드는 이 offsetof 를
	 * 쓰므로 동작에는 영향이 없다).
	 * 각 사건 종류를 켜고 끄는 비트 모음이며, cpqhpc_probe 가 초기화할 때와
	 * 인터럽트를 막아야 할 때 쓴다 */
	INT_MASK =		offsetof(struct ctrl_reg, int_mask),
	/* [한국어] 예약 구간 0. 배치를 맞추기 위한 자리이며 참조되지 않는다 */
	CTRL_RESERVED0 =	offsetof(struct ctrl_reg, reserved0),
	/* [한국어] 예약 구간 1. 참조되지 않는다 */
	CTRL_RESERVED1 =	offsetof(struct ctrl_reg, reserved1),
	/* [한국어] **예약 구간 2인데 reserved1 의 오프셋을 가리킨다.**
	 * CTRL_RESERVED1 과 값이 같아 사실상 중복이다. 다만 둘 다 이 드라이버
	 * 안에서 참조되지 않으므로 동작에 영향이 없다. 코드는 손대지 않는다 */
	CTRL_RESERVED2 =	offsetof(struct ctrl_reg, reserved1),
	/* [한국어] 범용 출력 A/B(u16). 컨트롤러 밖으로 내보내는 신호선이며,
	 * 이 드라이버 안에서 참조되지 않는다 */
	GEN_OUTPUT_AB =		offsetof(struct ctrl_reg, gen_output_AB),
	/* [한국어] 인터럽트를 일으키지 않는 입력 상태(u32).
	 * **폴링 경로가 보는 자리다** -- 인터럽트를 걸지 않고 현재 상태만
	 * 알고 싶을 때 여기를 읽는다.
	 * 읽는 자: get_presence_status, get_slot_enabled 계열의 상태 조회 */
	NON_INT_INPUT =		offsetof(struct ctrl_reg, non_int_input),
	/* [한국어] 예약 구간 3. 참조되지 않는다 */
	CTRL_RESERVED3 =	offsetof(struct ctrl_reg, reserved3),
	/* [한국어] 예약 구간 4. 참조되지 않는다.
	 * 구조체 쪽 장식 주석은 0x1a 라 적혀 있으나 실제 계산값은 0x1c 다.
	 * 코드가 이 offsetof 를 쓰므로 동작에는 영향이 없다 */
	CTRL_RESERVED4 =	offsetof(struct ctrl_reg, reserved4),
	/* [한국어] 예약 구간 5. 참조되지 않는다 */
	CTRL_RESERVED5 =	offsetof(struct ctrl_reg, reserved5),
	/* [한국어] 예약 구간 6. 참조되지 않는다 */
	CTRL_RESERVED6 =	offsetof(struct ctrl_reg, reserved6),
	/* [한국어] 예약 구간 7. 참조되지 않는다 */
	CTRL_RESERVED7 =	offsetof(struct ctrl_reg, reserved7),
	/* [한국어] 예약 구간 8. 참조되지 않는다 */
	CTRL_RESERVED8 =	offsetof(struct ctrl_reg, reserved8),
	/* [한국어] **슬롯 배치 정보(u8).**
	 * 상위 니블이 첫 슬롯의 PCI 장치 번호, 하위 니블이 슬롯 개수다.
	 * 읽는 자: cpqhpc_probe 가 ctrl->slot_device_offset 과 슬롯 수를
	 *   여기서 얻는다. 그 값이 이 드라이버의 두 번호 체계를 잇는다 */
	SLOT_MASK =		offsetof(struct ctrl_reg, slot_mask),
	/* [한국어] 예약 구간 9. 참조되지 않는다 */
	CTRL_RESERVED9 =	offsetof(struct ctrl_reg, reserved9),
	/* [한국어] 예약 구간 10. 참조되지 않는다 */
	CTRL_RESERVED10 =	offsetof(struct ctrl_reg, reserved10),
	/* [한국어] 예약 구간 11. 참조되지 않는다 */
	CTRL_RESERVED11 =	offsetof(struct ctrl_reg, reserved11),
	/* [한국어] 슬롯별 SERR(시스템 오류) 보고 활성 비트.
	 * 설정자: cpqphp_core.c 의 init_SERR 가 슬롯마다 켠다.
	 * **왜 켜는가**: 빈 슬롯이나 고장난 카드가 버스에 오류를 낼 때
	 *   시스템 전체가 멈추지 않고 이 컨트롤러가 잡아내도록 하기 위함이다 */
	SLOT_SERR =		offsetof(struct ctrl_reg, slot_SERR),
	/* [한국어] 슬롯별 전원 제어 비트.
	 * 이 파일의 slot_enable 과 별개이며, amber_LED 계열과 마찬가지로
	 * `0x01 << slot` 자리를 쓴다.
	 * 읽는 자·설정자: 이 파일의 전원 인라인 함수들 */
	SLOT_POWER =		offsetof(struct ctrl_reg, slot_power),
	/* [한국어] 현재/다음 버스 주파수(u8).
	 * **하위 4비트가 현재 속도, 상위 4비트가 다음에 적용할 속도다.**
	 * 읽는 자: get_controller_speed 가 PCI-X 속도 등급을 판별할 때.
	 * 설정자: set_controller_speed 가 목표 속도를 적어 넣을 때 */
	NEXT_CURR_FREQ =	offsetof(struct ctrl_reg, next_curr_freq),
	/* [한국어] 리셋·주파수 모드 상태(u8).
	 * 읽는 자: cpqhp_ctrl_intr 이 버스 리셋 완료 비트를 확인하고 지운다 */
	RESET_FREQ_MODE =	offsetof(struct ctrl_reg, reset_freq_mode),
};

struct hrt {
	/* [한국어] HRT 서명 네 글자 중 첫째. 넷을 합치면 "$HRT" 다.
	 * 설정자: 시스템 펌웨어가 ROM 에 써 둔다.
	 * 읽는 자: detect_HRT_floating_pointer(cpqphp_pci.c)가 ROM 을 16바이트씩
	 *   훑으며 SIG0~SIG3 오프셋으로 네 글자를 읽어 표의 시작을 찾는다.
	 * **char 인 것에 주의** -- 다른 필드는 u8/u16/u32 인데 서명만 char 다.
	 * 동기화: ROM 이라 불변 */
	char sig0;
	/* [한국어] 서명 둘째 글자 'H'.
	 * 동기화: 불변 */
	char sig1;
	/* [한국어] 서명 셋째 글자 'R'.
	 * 동기화: 불변 */
	char sig2;
	/* [한국어] 서명 넷째 글자 'T'. 넷이 모두 맞아야 표로 인정한다.
	 * **체크섬은 없다** -- 서명만 보고 믿는다.
	 * 동기화: 불변 */
	char sig3;
	/* [한국어] **시스템이 쓰지 않는 IRQ 의 비트맵.**
	 * cpqhp_find_available_resources 가 이 값을 오른쪽으로 밀어 가며
	 * 1 인 비트를 찾아, **첫 번째를 cpqhp_disk_irq, 두 번째를
	 * cpqhp_nic_irq 로 삼는다.** 저장장치와 그 밖의 카드에 서로 다른
	 * IRQ 를 주어 인터럽트 부하를 나누려는 것으로 보인다.
	 * 설정자: 펌웨어.
	 * 읽는 자: cpqhp_find_available_resources 하나뿐이다.
	 * **같은 이름의 static 전역이 cpqphp_pci.c 에 따로 있다** --
	 *   그쪽이 이 값을 읽어 담는 임시 변수다.
	 * 동기화: 불변 */
	u16 unused_IRQ;
	/* [한국어] PCI IRQ 관련 비트맵으로 보인다.
	 * **cpqhp_find_available_resources 가 읽어 unused_IRQ 변수에 덮어쓰지만,
	 * 그 뒤로 그 값을 쓰는 코드가 없다.** 과거에 쓰였다가 남은 것으로
	 * 보이며, 이름이 대문자인 것도 다른 필드와 어긋난다.
	 * 동기화: 불변 */
	u16 PCIIRQ;
	/* [한국어] **표에 든 slot_rt 항목의 개수.**
	 * cpqhp_find_available_resources 가 이 값을 읽어 순회 횟수로 삼는다.
	 * 설정자: 펌웨어. 읽는 자: 그 함수 하나.
	 * 주의: 그 함수는 이 값과 "보조 버스가 0 이 아님" 두 조건을 함께 보며
	 *   루프를 돈다 -- 개수만 믿지 않는 이중 방어다.
	 * 동기화: 불변 */
	u8 number_of_entries;
	/* [한국어] 표의 리비전. **이 드라이버는 쓰지 않는다** --
	 * REVISION 상수가 정의되어 있으나 참조하는 코드가 없다.
	 * 표 형식이 리비전에 따라 달라질 수 있는데도 확인하지 않는다.
	 * 동기화: 불변 */
	u8 revision;
	/* [한국어] 예약 필드. 자리 채우기이며 참조하는 코드가 없다.
	 * 동기화: 불변 */
	u16 reserved1;
	/* [한국어] 예약 필드. 이것까지 합쳐 헤더가 16바이트가 된다 --
	 * cpqhp_find_available_resources 가 `rom_resource_table + sizeof(struct hrt)`
	 * 로 첫 항목의 시작을 구하므로 그 크기가 정확해야 한다.
	 * 동기화: 불변 */
	u32 reserved2;
} __attribute__ ((packed));

/* offsets to the hotplug resource table registers based on the above
 * structure layout
 */
/* [한국어] **HRT(Hot Plug Resource Table) 헤더의 오프셋 모음.**
 * HRT 는 시스템 ROM 안에 펌웨어가 남겨 둔 표로, 핫플러그 슬롯에
 * 나눠 줄 수 있는 IO·메모리·버스 번호 여유 구간을 알려 준다.
 * **이 드라이버가 PCI 코어의 자원 할당기를 쓰지 않는 이유가 여기 있다** --
 *   코어는 부팅 때 이미 배치를 끝냈고 동작 중에 여유분을 되찾을 수단이
 *   없었다. 그래서 펌웨어가 미리 떼어 둔 이 구간을 받아 스스로 굴린다.
 * 읽는 자: detect_HRT_floating_pointer 와 cpqhp_find_available_resources
 *   (둘 다 cpqphp_pci.c)가 ROM 을 훑을 때 이 오프셋들로 읽는다.
 * **구조체를 역참조하지 않는 이유**: ROM 은 __iomem 영역이라
 *   readb/readw/readl 로만 접근해야 하기 때문이다 */
enum hrt_offsets {
	/* [한국어] 서명 첫 글자 위치. 네 글자를 모으면 "$HRT" 다 */
	SIG0 =			offsetof(struct hrt, sig0),
	/* [한국어] 서명 둘째 글자 위치 */
	SIG1 =			offsetof(struct hrt, sig1),
	/* [한국어] 서명 셋째 글자 위치 */
	SIG2 =			offsetof(struct hrt, sig2),
	/* [한국어] 서명 넷째 글자 위치.
	 * **네 글자가 모두 맞아야 표로 인정한다. 체크섬 검사는 없다** */
	SIG3 =			offsetof(struct hrt, sig3),
	/* [한국어] 시스템이 쓰지 않는 IRQ 비트맵의 위치.
	 * cpqhp_find_available_resources 가 여기서 1인 비트를 찾아
	 * 디스크·네트워크용 IRQ 두 개를 골라 둔다 */
	UNUSED_IRQ =		offsetof(struct hrt, unused_IRQ),
	/* [한국어] PCI 인터럽트로 쓸 수 있는 IRQ 비트맵의 위치 */
	PCIIRQ =		offsetof(struct hrt, PCIIRQ),
	/* [한국어] 뒤따르는 슬롯 항목의 개수.
	 * **표를 훑는 반복 횟수가 된다** -- 헤더 바로 뒤부터
	 *  struct slot_rt 크기만큼 이 횟수만큼 나아간다 */
	NUMBER_OF_ENTRIES =	offsetof(struct hrt, number_of_entries),
	/* [한국어] HRT 형식의 판 번호. 이 드라이버는 값을 읽지만 갈래를 나누지는 않는다 */
	REVISION =		offsetof(struct hrt, revision),
	/* [한국어] 예약 구간 1. 헤더 크기를 채운다 */
	HRT_RESERVED1 =		offsetof(struct hrt, reserved1),
	/* [한국어] 예약 구간 2.
	 * **이것까지 합쳐 헤더가 16바이트가 되어야 한다** --
	 * 첫 슬롯 항목의 위치를 `표 시작 + sizeof(struct hrt)` 로 구하기 때문이다 */
	HRT_RESERVED2 =		offsetof(struct hrt, reserved2),
};

struct slot_rt {
	/* [한국어] 이 항목이 가리키는 장치·함수 번호(devfn 형식).
	 * **상위 5비트가 장치, 하위 3비트가 함수다.**
	 * cpqhp_find_available_resources 가 `dev_func >> 3` 로 장치 번호를,
	 * `dev_func & 0x07` 로 함수 번호를 뽑아 노드를 찾는다.
	 * 설정자: 펌웨어. 읽는 자: 그 함수.
	 * 동기화: 불변 */
	u8 dev_func;
	/* [한국어] 이 슬롯이 붙어 있는 버스 번호.
	 * **cpqhp_find_available_resources 가 이 값이 ctrl->bus 와 다르면
	 * 항목을 통째로 건너뛴다** -- 컨트롤러가 여럿일 때 남의 몫을 가져가지
	 * 않기 위해서다.
	 * 동기화: 불변 */
	u8 primary_bus;
	/* [한국어] 이 슬롯 아래의 보조 버스 번호.
	 * 두 가지로 쓰인다 -- 항목이 유효한지 판단하는 조건(0 이면 표의 끝),
	 * 그리고 primary_bus 와 다르면 버스 번호 자원으로 등록하는 근거다.
	 * **bridged_slot 판정에도 쓰이지만 그 변수는 읽히지 않는다** --
	 *   그쪽 원문 주석이 "동작하지 않을 수 있고 쓰면 안 된다" 고 적었다.
	 * 동기화: 불변 */
	u8 secondary_bus;
	/* [한국어] 이 슬롯 아래로 쓸 수 있는 마지막 버스 번호.
	 * cpqhp_find_available_resources 가 `max_bus - secondary_bus + 1` 로
	 * 버스 개수를 계산해 자유 목록에 넣는다.
	 * 동기화: 불변 */
	u8 max_bus;
	/* [한국어] 이 슬롯에 배정된 IO 공간의 시작.
	 * **바이트 단위 그대로 쓴다** -- 메모리 쪽이 16비트 시프트하는 것과
	 * 다르다.
	 * 읽는 자: cpqhp_find_available_resources 가 노드를 만들 때.
	 * 주의: 0 이면 IO 자원이 없는 것으로 보고 건너뛴다.
	 * 동기화: 불변 */
	u16 io_base;
	/* [한국어] IO 공간의 길이.
	 * **시작 + 길이가 0x10000 을 넘으면 항목을 무시한다** --
	 * 필드가 16비트라 그 이상을 표현할 수 없기 때문으로 보인다.
	 * 동기화: 불변 */
	u16 io_length;
	/* [한국어] 메모리 공간의 시작. **64KiB 단위** 라
	 * cpqhp_find_available_resources 가 16비트 왼쪽으로 밀어 실제 주소로
	 * 만든다.
	 * 동기화: 불변 */
	u16 mem_base;
	/* [한국어] 메모리 공간의 길이. 역시 64KiB 단위라 16비트 왼쪽으로 민다.
	 * 동기화: 불변 */
	u16 mem_length;
	/* [한국어] prefetchable 메모리 공간의 시작. 메모리와 같은 방식이다.
	 * 동기화: 불변 */
	u16 pre_mem_base;
	/* [한국어] prefetchable 메모리 공간의 길이.
	 * **이 필드까지 열 개가 항목 하나를 이루며, 그 크기가 16바이트다** --
	 * cpqhp_find_available_resources 가 `one_slot += sizeof(struct slot_rt)`
	 * 로 다음 항목으로 넘어가므로 그 크기가 정확해야 한다.
	 * 동기화: 불변 */
	u16 pre_mem_length;
} __attribute__ ((packed));

/* offsets to the hotplug slot resource table registers based on the above
 * structure layout
 */
/* [한국어] **HRT 안의 슬롯 항목 하나에 대한 오프셋 모음.**
 * 헤더(struct hrt) 뒤에 이 형식의 항목이 number_of_entries 개 이어진다.
 * 항목 하나가 슬롯 하나에 배정된 IO·메모리·prefetchable 메모리 구간과
 * 버스 번호 범위를 알려 준다.
 * 읽는 자: cpqhp_find_available_resources 가 항목마다 이 오프셋들로 읽어
 *   struct pci_resource 노드를 만들고 컨트롤러의 자유 목록에 매단다.
 * **여기서 만들어진 목록이 이 드라이버 자원 관리의 출발점이다** */
enum slot_rt_offsets {
	/* [한국어] 이 항목이 가리키는 슬롯의 장치·함수 번호(상위 5비트 장치, 하위 3비트 함수).
	 * 어느 슬롯의 몫인지 가려내는 열쇠다 */
	DEV_FUNC =		offsetof(struct slot_rt, dev_func),
	/* [한국어] 이 슬롯이 붙어 있는 버스 번호.
	 * cpqhp_find_available_resources 가 컨트롤러의 버스와 맞는 항목만 취한다 */
	PRIMARY_BUS =		offsetof(struct slot_rt, primary_bus),
	/* [한국어] 이 슬롯 뒤에 브리지가 꽂힐 경우 쓸 수 있는 첫 버스 번호 */
	SECONDARY_BUS =		offsetof(struct slot_rt, secondary_bus),
	/* [한국어] 쓸 수 있는 마지막 버스 번호.
	 * **secondary~max 구간이 버스 번호 자유 목록의 한 노드가 된다** */
	MAX_BUS =		offsetof(struct slot_rt, max_bus),
	/* [한국어] 이 슬롯에 배정된 IO 공간의 시작 주소 */
	IO_BASE =		offsetof(struct slot_rt, io_base),
	/* [한국어] 그 IO 공간의 크기. base 와 함께 io_head 목록의 노드가 된다 */
	IO_LENGTH =		offsetof(struct slot_rt, io_length),
	/* [한국어] 일반(non-prefetchable) 메모리 구간의 시작 주소 */
	MEM_BASE =		offsetof(struct slot_rt, mem_base),
	/* [한국어] 그 메모리 구간의 크기. mem_head 목록의 노드가 된다 */
	MEM_LENGTH =		offsetof(struct slot_rt, mem_length),
	/* [한국어] prefetchable 메모리 구간의 시작 주소.
	 * **따로 두는 이유**: PCI 브리지의 창 레지스터가 둘을 나누므로
	 * 할당도 따로 해야 한다 */
	PRE_MEM_BASE =		offsetof(struct slot_rt, pre_mem_base),
	/* [한국어] 그 prefetchable 구간의 크기. p_mem_head 목록의 노드가 된다.
	 * **이 필드까지가 항목 하나이며, 그 크기만큼 나아가면 다음 항목이다** */
	PRE_MEM_LENGTH =	offsetof(struct slot_rt, pre_mem_length),
};

struct pci_func {
	/* [한국어] 전역 cpqhp_slot_list[bus] 리스트의 다음 노드.
	 * 설정자: cpqhp_slot_create 가 리스트 **끝에** 매달 때,
	 *   slot_remove 와 bridge_slot_remove 가 뺄 때.
	 * 읽는 자: cpqhp_slot_find 가 훑을 때, cpqphp_pci.c 의 재귀 함수들이
	 *   하위 버스의 노드를 순회할 때.
	 * **락이 없다** -- 전역 리스트인데도 보호가 없으며, 접근이 전역 스레드
	 *   하나이거나 probe 경로라 직렬화된다는 전제에 기댄다.
	 * 동기화: 없음 */
	struct pci_func *next;
	/* [한국어] 이 함수가 있는 PCI 버스 번호.
	 * 설정자: cpqhp_slot_create 로 노드를 만든 뒤 호출자가 채운다
	 *   (cpqhp_save_config, board_added, cpqhp_process_SI 등).
	 * 읽는 자: 설정공간 접근 전에 `pci_bus->number = func->bus` 로
	 *   임시 pci_bus 에 옮겨 담는 관용구가 파일 전체에 되풀이된다.
	 * 값 범위: 0~255. 전역 cpqhp_slot_list 배열의 색인과 같은 값이다.
	 * 동기화: 설정 후 사실상 불변 */
	u8 bus;
	/* [한국어] PCI 장치 번호(0~31).
	 * 읽는 자: cpqhp_slot_find 의 검색 키이자 PCI_DEVFN 의 첫 인자.
	 * **컨트롤러 안의 슬롯 번호를 구할 때도 쓴다** --
	 *   `func->device - ctrl->slot_device_offset` 이 그 계산이며,
	 *   이 파일의 인라인 함수들이 받는 slot 인자가 그 값이다.
	 * 동기화: 설정 후 불변 */
	u8 device;
	/* [한국어] PCI 함수 번호(0~7).
	 * 설정자: 위와 같다. 다중 함수 카드는 함수마다 노드가 따로 있다.
	 * 읽는 자: PCI_DEVFN 의 둘째 인자, cpqhp_save_config 가 원하는 함수의
	 *   노드를 고를 때.
	 * **cpqhp_slot_find 는 이 값으로 찾지 않는다** -- 그 함수는 index 로
	 *   "같은 device 의 몇 번째" 를 세므로, 호출자가 function 을 따로
	 *   비교해야 한다(cpqhp_save_config 의 while 루프).
	 * 동기화: 설정 후 불변 */
	u8 function;
	/* [한국어] 이 노드가 실제 카드인가, 빈 슬롯 자리표인가.
	 * 설정자: 카드를 설정하면 1(board_added), 뺀 뒤 자리표를 만들면 0
	 *   (remove_board, cpqhp_process_SI 의 실패 경로,
	 *   cpqhp_save_config 가 빈 슬롯에 노드를 만들 때).
	 * 읽는 자: cpqhp_process_SI 가 교체 경로로 갈지 추가 경로로 갈지 정하고,
	 *   cpqhp_valid_replace 가 0 이면 ADD_NOT_SUPPORTED 로 거절하며,
	 *   cpqhp_save_used_resources 가 보드인 함수만 훑는다.
	 * **빈 자리표를 두는 이유**: 그래야 카드가 새로 꽂혔을 때
	 *   cpqhp_slot_find 가 그 슬롯을 찾을 수 있다.
	 * 동기화: 없음 */
	u8 is_a_board;
	/* [한국어] **인터럽트와 스레드가 주고받는 통로.**
	 * 설정자: handle_power_fault 가 인터럽트 컨텍스트에서 전원 결함이 나면
	 *   0xFF 를, 해제되면 0x00 을 쓴다. remove_board 가 종료 시 0x01 을 쓴다.
	 *   board_added 와 board_replaced 가 확인한 뒤 0 으로 되돌린다.
	 * 읽는 자: board_added 와 board_replaced 가 카드를 켜고 1초 뒤
	 *   `func->status == 0xFF` 인지 보고 POWER_FAILURE 로 처리한다.
	 * **락이 없다** -- 인터럽트가 쓰고 스레드가 읽는데 보호가 없다.
	 *   u16 인데 0xFF 와 0x01 만 쓰이는 것도 어긋난다.
	 * 동기화: 없음 */
	u16 status;
	/* [한국어] 설정공간을 채워 넣었는가.
	 * 설정자: cpqhp_slot_create 가 1 로 초기화하고,
	 *   configure_new_function 과 cpqhp_configure_board 가 성공하면 1 로,
	 *   remove_board 와 새 노드를 만드는 곳들이 0 으로 둔다.
	 * 읽는 자: **이 드라이버 안에서 이 값을 읽어 분기하는 코드를 찾을 수 없다.**
	 *   cpqphp_core.c 의 process_SI 가 0 으로 밀어 두기는 하나,
	 *   그 값을 보고 무언가 하는 곳은 없다.
	 * 동기화: 없음 */
	u8 configured;
	/* [한국어] 슬롯 레버의 마지막 상태.
	 * 설정자: handle_switch_change 가 레버가 열리면 0, 닫히면 0x10 을 쓴다.
	 *   cpqhp_process_SI, remove_board, cpqhp_save_config,
	 *   cpqphp_core.c 의 cpqhpc_probe 도 노드를 만들며 채운다.
	 * 읽는 자: **handle_presence_change 가 이 값으로 갈래를 정한다** --
	 *   레버가 닫혀 있고(0x10) 컨트롤러가 버튼 모드이면 버튼 누름으로,
	 *   아니면 카드 존재 변화로 해석한다. 하드웨어가 같은 신호선을
	 *   두 용도로 쓰기 때문이다.
	 * 값 범위: 0(열림) 또는 0x10(닫힘).
	 *   **1 이 아니라 0x10 인 이유는 이 트리에서 확인할 수 없다.**
	 * 동기화: 없음. 인터럽트 컨텍스트에서 쓰인다 */
	u8 switch_save;
	/* [한국어] 카드 존재의 마지막 상태.
	 * 설정자: handle_switch_change, handle_presence_change,
	 *   cpqhp_process_SI, cpqhpc_probe 가 **같은 계산식으로** 채운다 --
	 *   상태 레지스터 상위 16비트에서 hp_slot 자리와 hp_slot+7 자리를 뽑아
	 *   0x01 과 0x02 자리에 놓는다. 그 식이 네 곳에 복사되어 있다.
	 * 읽는 자: **handle_presence_change 가 버튼을 누른 것인지 뗀 것인지
	 *   가릴 때 지금 값과 견준다** -- 같으면 뗀 것, 다르면 누른 것이다.
	 * 값 범위: 0~3. 감지 핀이 둘이라 두 비트다.
	 * 동기화: 없음 */
	u8 presence_save;
	/* [한국어] BAR 여섯 개가 각각 요구하는 크기.
	 * 설정자: cpqhp_save_base_addr_length(cpqphp_pci.c)가 BAR 에
	 *   0xFFFFFFFF 를 써 보고 되읽어 계산한 값을 담는다.
	 * 읽는 자: cpqhp_valid_replace 가 새로 꽂힌 카드의 BAR 크기와 견줘
	 *   같은 카드인지 판정한다.
	 * 색인: `(cloop - 0x10) >> 2` -- 설정공간 오프셋 0x10 이 0번,
	 *   0x14 가 1번 하는 식이다.
	 * **브리지는 BAR 가 둘뿐이라 앞의 두 칸만 채워진다.**
	 * 동기화: 없음 */
	u32 base_length[0x06];
	/* [한국어] 각 BAR 가 IO 인가 메모리인가.
	 * 설정자: base_length 와 같은 곳에서 함께 채운다. 1 이 IO, 0 이 메모리다.
	 * 읽는 자: cpqhp_valid_replace 가 크기와 함께 견준다 --
	 *   **크기가 같아도 종류가 바뀌었으면 다른 카드다.**
	 * 동기화: 없음 */
	u8 base_type[0x06];
	/* [한국어] 예약 필드. **참조하는 코드가 없다.**
	 * 앞의 base_type[6] 이 6바이트라 여기에 2바이트를 두면 8의 배수가
	 * 되는데, 그 정렬을 노린 것인지는 코드에 적혀 있지 않다.
	 * 동기화: 해당 없음 */
	u16 reserved2;
	/* [한국어] **설정공간 128바이트의 사본.** dword 서른두 개다.
	 * 설정자: cpqhp_save_config 와 cpqhp_save_slot_config 가
	 *   `pci_bus_read_config_dword(..., cloop << 2, &config_space[cloop])`
	 *   로 통째로 뜬다.
	 * 읽는 자: cpqhp_configure_board 가 **높은 오프셋부터 거꾸로** 되쓰고
	 *   (제어 레지스터를 마지막에 쓰려는 것),
	 *   cpqhp_valid_replace 가 벤더 ID 와 클래스 코드를 견주며,
	 *   bridge_slot_remove 와 is_bridge 가 헤더 타입과 버스 번호를 읽는다.
	 * 색인: 바이트 오프셋을 4로 나눈 값. 그래서 코드에
	 *   `config_space[0x06]`(오프셋 0x18)이나 `config_space[0x18 >> 2]`
	 *   같은 표현이 섞여 나온다.
	 * **이 배열이 교체 경로의 전부다** -- 같은 카드가 다시 꽂히면
	 *   이것을 되쓰는 것으로 설정이 끝난다.
	 * 동기화: 없음 */
	u32 config_space[0x20];
	/* [한국어] 이 함수가 **쓰고 있는** 메모리 자원 목록의 머리.
	 * 설정자: configure_new_function 이 자유 목록에서 떼어 온 노드를 매달고,
	 *   cpqhp_save_used_resources 가 이미 쓰던 범위를 기록할 때 매단다.
	 *   cpqhp_return_board_resources 가 반납하며 NULL 로 비운다.
	 * 읽는 자: 반납·해제 경로와, remove_board 가 "이미 기록되어 있는지"
	 *   확인할 때.
	 * **컨트롤러의 같은 이름 필드와 방향이 반대다** -- 그쪽은 나눠 줄 것,
	 *   이쪽은 쓰고 있는 것이다. 카드를 뽑을 때 이쪽에서 그쪽으로 옮겨 간다.
	 * 동기화: 없음 */
	struct pci_resource *mem_head;
	/* [한국어] prefetchable 메모리 자원 목록의 머리. mem_head 와 같은 방식이다.
	 * **둘을 나누는 이유**: PCI 브리지의 창 레지스터가 prefetchable 과
	 *   일반 메모리를 따로 두므로 할당도 따로 해야 한다.
	 * 동기화: 없음 */
	struct pci_resource *p_mem_head;
	/* [한국어] IO 자원 목록의 머리.
	 * **할당 함수가 다르다** -- 메모리는 get_resource 를 쓰지만 IO 는
	 *   get_io_resource 를 쓴다. ISA 별칭 구간을 피해야 하기 때문이다.
	 * 동기화: 없음 */
	struct pci_resource *io_head;
	/* [한국어] 버스 번호 자원 목록의 머리.
	 * **브리지에서만 채워진다** -- 그 뒤에 매달릴 버스 번호 구간을 받아
	 *   두는 자리이며, 일반 카드는 버스 번호를 쓰지 않는다.
	 * 설정자: configure_new_function 의 브리지 경로가 hold_bus_node 를 매달고,
	 *   cpqhp_save_used_resources 가 보조~종속 버스 범위를 기록한다.
	 * 동기화: 없음 */
	struct pci_resource *bus_head;
	/* [한국어] **포인터이며, 이 드라이버는 쓰지 않는다.**
	 * 설정자: remove_board 가 빈 슬롯 자리표를 만들 때 NULL 로 밀어 둔다.
	 * 읽는 자: 없다.
	 * 실제 5초 타이머는 struct slot 의 task_event 필드이며 **포인터가 아니라
	 *   값** 이다. 이 필드는 그 이전 설계의 잔재로 보이나, 코드가 그 사실을
	 *   적어 두지는 않았다.
	 * 동기화: 해당 없음 */
	struct timer_list *p_task_event;
	/* [한국어] 리눅스 쪽 struct pci_dev 포인터.
	 * 설정자: cpqhp_save_config 와 cpqhp_configure_device 가
	 *   pci_get_domain_bus_and_slot 으로 찾아 넣는다.
	 * 읽는 자: board_added 가 `!new_slot->pci_dev` 로 아직 등록되지 않은
	 *   함수만 골라 cpqhp_configure_device 를 부른다.
	 * **참조 계수를 쥐지 않는다** -- 두 설정자 모두 곧바로 pci_dev_put 을
	 *   부르므로 포인터만 남는다. NULL 인지만 보는 용도라 실질적 문제는 없다.
	 * 동기화: 없음 */
	struct pci_dev *pci_dev;
};

struct slot {
	/* [한국어] 컨트롤러의 슬롯 리스트(ctrl->slot)의 다음 노드.
	 * 설정자: ctrl_slot_setup(cpqphp_core.c)이 슬롯을 만들며 **머리에** 매단다.
	 *   ctrl_slot_cleanup 이 모듈을 내릴 때 훑으며 지운다.
	 * 읽는 자: cpqhp_find_slot 이 장치 번호로 찾을 때,
	 *   set_controller_speed 가 다른 슬롯에 카드가 있는지 확인할 때.
	 * **pci_func 리스트와 별개다** -- 이쪽은 컨트롤러마다, 그쪽은 버스마다다.
	 * 동기화: 없음. probe 때 만들어진 뒤 바뀌지 않는다 */
	struct slot *next;
	/* [한국어] 이 슬롯이 붙어 있는 버스 번호.
	 * 설정자: ctrl_slot_setup 이 ctrl->bus 를 복사한다.
	 * 읽는 자: cpqhp_pushbutton_thread 가 `cpqhp_slot_find(p_slot->bus, ...)`
	 *   로 함수 노드를 찾을 때.
	 * 동기화: 설정 후 불변 */
	u8 bus;
	/* [한국어] 이 슬롯의 PCI 장치 번호.
	 * 설정자: ctrl_slot_setup 이 SLOT_MASK 의 상위 니블에서 시작해
	 *   슬롯마다 하나씩 늘려 가며 채운다.
	 * 읽는 자: cpqhp_find_slot 의 검색 키이고,
	 *   **네 조회 인라인 함수가 `slot->device - ctrl->slot_device_offset`
	 *   으로 컨트롤러 안의 슬롯 번호를 구할 때** 쓴다.
	 * 동기화: 설정 후 불변 */
	u8 device;
	/* [한국어] **사용자에게 보이는 물리 슬롯 번호.**
	 * 설정자: ctrl_slot_setup 이 ctrl->first_slot 에서 시작해 늘려 간다.
	 *   그 first_slot 은 get_slot_mapping 이 BIOS IRQ 라우팅 표에서 찾은 값이다.
	 * 읽는 자: sysfs 이름을 만들 때(snprintf), SMBIOS 표에서 이 슬롯의
	 *   항목을 찾을 때, cpqhp_get_bus_dev 에 넘겨 버스·장치 번호를 얻을 때,
	 *   그리고 사용자에게 보여 줄 로그 메시지(msg_button_on 등)에 쓸 때.
	 * **device 와 다르다** -- 이쪽은 섀시에 적힌 번호, 그쪽은 PCI 번호다.
	 * 동기화: 설정 후 불변 */
	u8 number;
	/* [한국어] **이 구조체에서는 쓰이지 않는다.**
	 * ctrl_slot_setup 이 kzalloc 으로 0 을 채운 뒤 건드리지 않으며,
	 * 읽는 코드도 없다. 같은 이름의 pci_func 필드가 실제로 쓰이는 것과
	 * 대비되며, 두 구조체를 비슷하게 맞추려다 남은 것으로 보인다.
	 * 동기화: 해당 없음 */
	u8 is_a_board;
	/* [한국어] **이 구조체에서는 쓰이지 않는다.** 위와 같은 이유로 보인다.
	 * 동기화: 해당 없음 */
	u8 configured;
	/* [한국어] **상태 기계의 현재 위치.** 이 드라이버 이벤트 처리의 핵심 필드다.
	 * 값 범위: STATIC_STATE(0), BLINKINGON_STATE(1), BLINKINGOFF_STATE(2),
	 *   POWERON_STATE(3), POWEROFF_STATE(4).
	 * 설정자: interrupt_event_handler 가 버튼을 뗐을 때 BLINKINGON 이나
	 *   BLINKINGOFF 로 옮기고, 취소되면 STATIC 으로 되돌린다.
	 *   cpqhp_pushbutton_thread 가 실제 작업 중에 POWERON/POWEROFF 로 두고
	 *   끝나면 STATIC 으로 되돌린다.
	 * 읽는 자: **handle_presence_change 가 인터럽트 컨텍스트에서 읽는다** --
	 *   깜빡이는 중이면 버튼을 INT_BUTTON_CANCEL 로, 전원 조작 중이면
	 *   INT_BUTTON_IGNORE 로 해석한다.
	 * **락이 없다** -- 스레드가 쓰고 인터럽트가 읽는데 보호가 없다.
	 * 동기화: 없음 */
	u8 state;
	/* [한국어] **이 구조체에서는 쓰이지 않는다.** 레버 상태는 pci_func 쪽 같은
	 * 이름 필드가 관리한다.
	 * 동기화: 해당 없음 */
	u8 switch_save;
	/* [한국어] **이 구조체에서는 쓰이지 않는다.** 존재 상태도 pci_func 쪽이 관리한다.
	 * 동기화: 해당 없음 */
	u8 presence_save;
	/* [한국어] 이 슬롯이 지원하는 기능과 현재 상태의 비트 모음.
	 * 설정자: ctrl_slot_setup 이 등록할 때 채운다 --
	 *   PCISLOT_REPLACE_SUPPORTED 와 PCISLOT_INTERLOCK_SUPPORTED 를
	 *   무조건 넣고(위의 원문 FIXME 주석이 "이 기능들은 쓰이지 않지만
	 *   쓰려면 제대로 구현해야 한다" 고 적었다), SMBIOS 로 64비트·66MHz
	 *   지원 여부를, 레지스터로 존재·레버·전원 상태를 넣는다.
	 * 읽는 자: **이 드라이버 안에 없다.** 위의 FIXME 가 밝힌 그대로다.
	 * 동기화: 설정 후 불변 */
	u32 capabilities;
	/* [한국어] 예약 필드. 참조하는 코드가 없다.
	 * 동기화: 해당 없음 */
	u16 reserved2;
	/* [한국어] **버튼을 뗀 뒤 5초를 세는 타이머. 값으로 박혀 있다.**
	 * 설정자: ctrl_slot_setup 이 timer_setup 으로 핸들러를
	 *   cpqhp_pushbutton_thread 로 걸어 두고,
	 *   interrupt_event_handler 가 버튼을 뗄 때마다 다시 걸며
	 *   expires 를 jiffies + 5*HZ 로 놓고 add_timer 한다.
	 *   INT_BUTTON_CANCEL 이면 timer_delete 로 지운다.
	 * 읽는 자: cpqhp_pushbutton_thread 가 timer_container_of 로 이 필드에서
	 *   **바깥 struct slot 을 되찾는다** -- 값으로 박혀 있어야 가능한 방식이다.
	 * **5초를 두는 이유**: 사용자가 마음을 바꿔 버튼을 다시 누르면
	 *   취소할 수 있게 하는 것이다.
	 * 주의: ctrl_slot_setup 이 expires 를 미리 넣어 두지만 add_timer 는
	 *   하지 않으므로, 그 초기값은 실제로 쓰이지 않는다.
	 * 동기화: 타이머 코어가 관리 */
	struct timer_list task_event;
	/* [한국어] **컨트롤러 안의 슬롯 번호(0부터).**
	 * 설정자: interrupt_event_handler 가 타이머를 걸기 직전에 채운다.
	 * 읽는 자: cpqhp_pushbutton_thread 가 LED 를 조작할 때 쓴다.
	 * **왜 여기 저장하는가**: 타이머 콜백은 struct slot 만 받으므로,
	 *   그때 다시 `slot->device - ctrl->slot_device_offset` 을 계산해도
	 *   되지만 미리 넣어 두는 방식을 택했다.
	 * 동기화: 없음 */
	u8 hp_slot;
	/* [한국어] 이 슬롯을 소유한 컨트롤러.
	 * 설정자: ctrl_slot_setup 이 슬롯을 만들며 채우고,
	 *   interrupt_event_handler 가 타이머를 걸기 전에 다시 채운다.
	 * 읽는 자: **cpqphp_core.c 의 모든 sysfs 콜백이 to_slot 으로 슬롯을
	 *   얻은 뒤 곧바로 이 필드로 컨트롤러까지 간다.**
	 *   cpqhp_pushbutton_thread 도 타이머에서 슬롯을 되찾은 뒤 이것을 쓴다.
	 * 동기화: 설정 후 불변 */
	struct controller *ctrl;
	/* [한국어] 이 슬롯에 해당하는 SMBIOS Type 9 항목의 주소.
	 * 설정자: ctrl_slot_setup 이 슬롯 번호가 맞는 항목을 찾아 넣는다.
	 * 읽는 자: is_slot64bit 과 is_slot66mhz(cpqphp_core.c)가 이 주소에
	 *   SMBIOS_SLOT_WIDTH 와 SMBIOS_SLOT_TYPE 오프셋을 더해 읽는다.
	 * 값 범위: **NULL 일 수 있다** -- 맞는 항목이 없으면 그렇게 남는데,
	 *   두 함수가 NULL 검사 없이 readb 하므로 그때 어떻게 되는지는
	 *   이 트리에서 알 수 없다. 코드는 손대지 않고 사실만 적어 둔다.
	 * 동기화: 설정 후 불변 */
	void __iomem *p_sm_slot;
	/* [한국어] **핫플러그 코어에 등록하는 구조체. 값으로 박혀 있다.**
	 * 설정자: ctrl_slot_setup 이 ops 를 채우고 pci_hp_register 로 등록한다.
	 *   ctrl_slot_cleanup 이 pci_hp_deregister 로 뺀다.
	 * 읽는 자: 코어가 sysfs 요청을 받으면 이 구조체의 ops 를 통해
	 *   이 드라이버의 콜백을 부르고, 그 콜백은 to_slot 으로 바깥의
	 *   struct slot 을 되찾는다.
	 * **값으로 박혀 있어야 to_slot 의 container_of 가 성립한다.**
	 *   이 필드 하나가 드라이버와 핫플러그 코어를 잇는 접합점이다.
	 * **struct hotplug_slot 의 정의는 이 스파스 체크아웃에 없다**
	 *   (include/linux/pci_hotplug.h 부재).
	 * 동기화: 코어가 관리 */
	struct hotplug_slot hotplug_slot;
};

struct pci_resource {
	/* [한국어] 단일 연결 리스트의 다음 노드.
	 * **이 드라이버가 PCI 코어의 자원 할당기를 쓰지 않고 직접 굴리는
	 *   자유 목록(free list)의 고리다.** cpqphp_pci.c 의 get_resource,
	 *   get_io_resource, cpqhp_resource_sort_and_combine 이 모두 이 고리를
	 *   따라 걷는다.
	 * 설정자: 리스트에 넣고 빼는 모든 곳. 특히 정렬·병합 함수가
	 *   주소 오름차순이 되도록 다시 엮는다.
	 * 읽는 자: 같은 함수들.
	 * **왜 코어를 안 쓰는가**: 2001년 당시 PCI 코어에는 동작 중인 시스템에
	 *   자원을 되돌려주고 다시 나눠 주는 수단이 없었다. 그래서 BIOS 가
	 *   넘겨 준 여유 구간(HRT)을 이 목록으로 받아 스스로 관리한다.
	 * 동기화: 없음. ctrl->crit_sect 밖에서 다뤄지는 곳이 있다 */
	struct pci_resource *next;
	/* [한국어] 이 자원 구간의 시작 주소(또는 버스 번호 목록이면 시작 버스 번호).
	 * 설정자: HRT 를 읽어 목록을 만들 때, 그리고 큰 노드를 잘라
	 *   일부만 내줄 때 남는 쪽의 시작을 다시 계산할 때.
	 * 읽는 자: get_resource 가 정렬 기준으로 삼고,
	 *   cpqhp_resource_sort_and_combine 이 인접 여부를 판단하며,
	 *   configure_new_function 이 BAR 에 써 넣을 값으로 쓴다.
	 * 값 범위: 32비트 물리 주소. **64비트 BAR 는 상위 dword 에 0 을 쓴다**
	 *   -- 이 드라이버는 4GB 아래만 다룬다.
	 * 동기화: 없음 */
	u32 base;
	/* [한국어] 이 구간의 크기(바이트, 또는 버스 번호 개수).
	 * 설정자: base 와 함께.
	 * 읽는 자: get_resource 가 요청 크기와 견주고, 정렬·병합 함수가
	 *   `base + length` 로 다음 구간과 맞닿는지 본다.
	 * **정렬 규칙**: get_resource 는 요청한 크기를 2의 거듭제곱으로 보고
	 *   그 경계에 맞는 위치를 찾는다. PCI BAR 가 자기 크기 경계에
	 *   정렬되어야 하기 때문이다.
	 * 동기화: 없음 */
	u32 length;
};

struct event_info {
	/* [한국어] 무슨 일이 일어났는가.
	 * 값 범위: INT_BUTTON_IGNORE(0), INT_PRESENCE_ON(1), INT_PRESENCE_OFF(2),
	 *   INT_SWITCH_CLOSE(3), INT_SWITCH_OPEN(4), INT_POWER_FAULT(5),
	 *   INT_POWER_FAULT_CLEAR(6), INT_BUTTON_PRESS(7), INT_BUTTON_RELEASE(8),
	 *   INT_BUTTON_CANCEL(9).
	 * 설정자: **인터럽트 컨텍스트** 의 handle_switch_change,
	 *   handle_presence_change, handle_power_fault 가 채운다.
	 * 읽는 자: 스레드 컨텍스트의 interrupt_event_handler 가 꺼내 갈래를 탄다.
	 * **u32 인 이유는 코드에 없다** -- 값이 0~9뿐이라 u8 로도 충분하다.
	 * 동기화: 없음. 아래 event_queue 설명 참조 */
	u32 event_type;
	/* [한국어] 이 일이 일어난 슬롯의 컨트롤러 안 번호(0부터).
	 * 설정자: 위 세 인터럽트 처리 함수가 레지스터 비트 자리에서 얻은 값.
	 * 읽는 자: interrupt_event_handler 가 이 번호로
	 *   `cpqhp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset)` 을 불러
	 *   슬롯을 되찾는다.
	 * **물리 슬롯 번호가 아니다** -- 사용자에게 보이는 번호는
	 *   slot->number 이고, 둘 사이는 first_slot 만큼 차이가 난다.
	 * 동기화: 없음 */
	u8 hp_slot;
};

struct controller {
	/* [한국어] 전역 컨트롤러 리스트 cpqhp_ctrl_list 의 다음 노드.
	 * 설정자: cpqhpc_probe 가 컨트롤러를 찾을 때마다 머리에 매단다.
	 * 읽는 자: cpqhpc_cleanup 이 모듈을 내릴 때 전부 훑고,
	 *   unload_cpqphpd 가 자원을 돌려줄 때 훑는다.
	 * **한 대에 컨트롤러가 여럿일 수 있다** -- Compaq 서버는 PCI 버스마다
	 *   핫플러그 컨트롤러를 따로 두는 구성이 있었다.
	 * 동기화: 없음. probe 와 module_exit 에서만 다뤄진다 */
	struct controller *next;
	/* [한국어] **인터럽트가 남기고 스레드가 가져가는 상태 레지스터 사본.**
	 * 설정자: cpqhp_ctrl_intr 이 인터럽트 컨텍스트에서 INT_INPUT_CLEAR 에
	 *   써 넣은 값(=처리한 비트들)을 그대로 남긴다.
	 * 읽는 자: cpqhp_ctrl_intr 자신이 다음 인터럽트에서 읽고,
	 *   wait_for_ctrl_irq 를 기다리던 쪽이 무엇이 일어났는지 볼 수 있다.
	 * **락이 없다** -- 인터럽트가 쓰고 다른 문맥이 읽는데 보호가 없으며,
	 *   u32 한 번의 쓰기가 쪼개지지 않는다는 데 기댄다.
	 * 동기화: 없음 */
	u32 ctrl_int_comp;
	/* [한국어] **컨트롤러 레지스터를 만지는 임계 구역 뮤텍스.**
	 * 설정자·읽는 자: board_added, board_replaced, remove_board,
	 *   cpqhp_pushbutton_thread, set_controller_speed,
	 *   그리고 cpqphp_core.c 의 sysfs 콜백들이 mutex_lock/unlock 으로 감싼다.
	 * **무엇을 지키는가**: 이 컨트롤러의 MMIO 레지스터 한 벌이다.
	 *   set_SOGO 로 명령을 내리고 wait_for_ctrl_irq 로 완료를 기다리는
	 *   한 쌍은 나뉘어서는 안 되므로, 그 둘을 함께 감싼다.
	 * **인터럽트 처리기는 이 락을 잡지 않는다** -- 뮤텍스는 잠들 수 있어
	 *   인터럽트 컨텍스트에서 쓸 수 없다. 그래서 위의 여러 필드가
	 *   보호 없이 인터럽트와 공유된다.
	 * 원문 주석: critical section mutex */
	struct mutex crit_sect;	/* critical section mutex */
	/* [한국어] **이 컨트롤러의 MMIO 레지스터 창 시작 주소.**
	 * 설정자: cpqhpc_probe 가 BAR 를 ioremap 한 결과.
	 *   cpqhpc_cleanup 이 iounmap 한다.
	 * 읽는 자: 이 파일의 인라인 함수 전부와 cpqphp_ctrl.c 의 레지스터 접근이
	 *   `ctrl->hpc_reg + <CTRL_ 계열 오프셋>` 형태로 쓴다.
	 *   그 오프셋들이 위의 enum ctrl_offsets 이고,
	 *   가리키는 배치가 struct ctrl_reg 다.
	 * **__iomem 표시** 는 sparse 정적 검사기에게 "이 포인터는 역참조하지 말고
	 *   readb/writel 로만 접근하라" 고 알리는 것이다.
	 * 원문 주석: cookie for our pci controller location
	 * 동기화: 접근 자체는 crit_sect 로, 포인터 값은 불변 */
	void __iomem *hpc_reg;	/* cookie for our pci controller location */
	/* [한국어] **나눠 줄 수 있는** 메모리 자원의 자유 목록.
	 * 설정자: cpqhpc_probe 가 HRT 를 읽어 처음 채우고,
	 *   cpqhp_return_board_resources 가 카드를 뽑을 때 되돌려 받아 매단다.
	 * 읽는 자: configure_new_function 이 여기서 떼어 쓰고,
	 *   unload_cpqphpd 가 모듈을 내리며 전부 해제한다.
	 * **pci_func 의 같은 이름 필드와 방향이 반대다** -- 그쪽은 쓰고 있는 것,
	 *   이쪽은 아직 아무도 안 쓰는 것이다.
	 * 동기화: 없음 */
	struct pci_resource *mem_head;
	/* [한국어] prefetchable 메모리의 자유 목록. mem_head 와 같은 방식이다.
	 * 브리지 창 레지스터가 둘을 나누므로 목록도 나눈다.
	 * 동기화: 없음 */
	struct pci_resource *p_mem_head;
	/* [한국어] IO 공간의 자유 목록.
	 * **IO 는 특히 귀하다** -- 한 시스템에 64KB뿐인 데다
	 *   ISA 별칭 때문에 실제 쓸 수 있는 구간은 더 좁다.
	 *   그래서 get_io_resource 라는 별도 할당 함수를 둔다.
	 * 동기화: 없음 */
	struct pci_resource *io_head;
	/* [한국어] 버스 번호의 자유 목록.
	 * **브리지가 꽂혔을 때만 쓴다** -- 브리지 뒤에 새 버스가 생기므로
	 *   그 번호를 여기서 떼어 준다.
	 * 동기화: 없음 */
	struct pci_resource *bus_head;
	/* [한국어] 이 핫플러그 **컨트롤러 자신** 의 pci_dev.
	 * 설정자: cpqhpc_probe 가 인자로 받은 것을 그대로 넣는다.
	 * 읽는 자: dev_err 계열 로그의 대상, 그리고 cpqhpc_probe 가
	 *   pci_resource_start 로 레지스터 주소를 얻을 때.
	 * **슬롯에 꽂힌 카드가 아니라 컨트롤러 자체다** -- 컨트롤러도 PCI
	 *   장치이며, 이 드라이버는 그것에 붙는 보통의 PCI 드라이버다.
	 * 동기화: 불변 */
	struct pci_dev *pci_dev;
	/* [한국어] **설정공간 접근용으로 빌려 쓰는 pci_bus 구조체.**
	 * 설정자: cpqhpc_probe 가 kmalloc 으로 하나 만들어
	 *   `*ctrl->pci_bus = *pdev->bus` 로 통째로 복사한다.
	 * 읽는 자: 설정공간을 읽고 쓰는 거의 모든 곳이
	 *   `ctrl->pci_bus->number = <원하는 버스>` 로 번호만 바꿔 놓고
	 *   pci_bus_read_config_ 계열을 부른다.
	 * **왜 복사본을 두는가**: 진짜 버스 구조체의 number 를 건드리면
	 *   커널 전체가 깨진다. ops 포인터만 같으면 되므로, 값을 복사해
	 *   번호만 갈아 끼우는 것이다. **지금 기준으로는 위험한 관용구이며**,
	 *   요즘은 pci_get_domain_bus_and_slot 이나 도메인 인지 API 를 쓴다.
	 * 동기화: 없음. crit_sect 밖에서 번호가 바뀌는 곳이 있다 */
	struct pci_bus *pci_bus;
	/* [한국어] **인터럽트가 넣고 스레드가 꺼내 가는 고정 크기 링 버퍼. 열 칸.**
	 * 설정자: 인터럽트 컨텍스트의 handle_switch_change,
	 *   handle_presence_change, handle_power_fault 가
	 *   `ctrl->event_queue[ctrl->next_event]` 에 채운 뒤
	 *   `next_event = (next_event + 1) % 10` 으로 넘긴다.
	 * 읽는 자: 스레드 컨텍스트의 interrupt_event_handler 가
	 *   0번부터 9번까지 훑으며 event_type 이 0 이 아닌 칸을 처리하고
	 *   0 으로 지운다.
	 * **꼬리 색인이 없다** -- next_event 하나뿐이라 소비자는 전체를 훑는다.
	 *   열 칸이 다 차면 **가장 오래된 것부터 조용히 덮어쓴다.**
	 *   이벤트가 유실될 수 있으나 그것을 알리는 수단이 없다.
	 * **락이 전혀 없다** -- 생산자는 인터럽트, 소비자는 워크큐인데
	 *   둘 사이에 어떤 보호도 없다.
	 * **세대 대비**: 같은 문제를 shpchp 는 슬롯마다 워크큐 항목을 만들어
	 *   풀고(shpchp_ctrl.c:153,156), pciehp 는 스레드 IRQ 와
	 *   atomic_or/atomic_xchg 로 푼다(pciehp_hpc.c:50,90,91).
	 *   세 세대가 같은 문제를 어떻게 다르게 풀었는지 보여 주는 자리다.
	 * 동기화: 없음 */
	struct event_info event_queue[10];
	/* [한국어] 이 컨트롤러가 거느린 슬롯 리스트의 머리.
	 * 설정자: ctrl_slot_setup 이 슬롯마다 **머리에** 매단다.
	 *   ctrl_slot_cleanup 이 훑으며 지운다.
	 * 읽는 자: cpqhp_find_slot 이 장치 번호로 찾고,
	 *   set_controller_speed 가 다른 슬롯의 카드 유무를 살필 때 훑는다.
	 * **머리에 매다는 순서 때문에 리스트는 슬롯 번호 역순이다.**
	 * 동기화: 없음. probe 뒤 바뀌지 않는다 */
	struct slot *slot;
	/* [한국어] event_queue 링의 **다음에 쓸 칸** 색인.
	 * 설정자: 인터럽트 처리 함수들이 이벤트를 넣은 뒤 1 늘리고 10 으로 나눈
	 *   나머지를 넣는다. cpqhpc_probe 가 0 으로 초기화한다.
	 * 읽는 자: 같은 인터럽트 처리 함수들.
	 * **소비자는 이 값을 쓰지 않는다** -- interrupt_event_handler 는
	 *   링 전체를 훑으므로 꺼내는 위치를 따로 기억하지 않는다.
	 * 값 범위: 0~9.
	 * 동기화: 없음 */
	u8 next_event;
	/* [한국어] 이 컨트롤러가 실제로 쓰는 IRQ 번호.
	 * 설정자: cpqhpc_probe 가 `ctrl->interrupt = pdev->irq` 로 넣고,
	 *   cfgspc_irq 를 보고 필요하면 설정공간의 값으로 덮어쓴다.
	 * 읽는 자: request_irq 와 free_irq 의 인자.
	 * **cfgspc_irq 와 나누는 이유** 는 아래 필드 설명을 보라.
	 * 동기화: 불변 */
	u8 interrupt;
	/* [한국어] **설정공간 PCI_INTERRUPT_LINE 에 적혀 있던 IRQ 번호.**
	 * 설정자: cpqhpc_probe 가 pci_read_config_byte 로 직접 읽는다.
	 * 읽는 자: cpqhpc_probe 자신이 pdev->irq 와 다르면 로그를 남기고
	 *   어느 쪽을 쓸지 정한다.
	 * **왜 둘이 다를 수 있는가**: 커널이 ACPI 나 MP 표를 보고 IRQ 를
	 *   다시 배정하면 pdev->irq 는 바뀌지만 설정공간 값은 BIOS 가 적어 둔
	 *   그대로 남는다. 2000년대 초에는 어느 쪽이 맞는지 기종마다 달라서
	 *   둘을 다 들고 있었던 것으로 보이나, **그 판단 근거가 코드에 적혀
	 *   있지는 않다.**
	 * 동기화: 불변 */
	u8 cfgspc_irq;
	/* [한국어] 이 핫플러그 컨트롤러가 관장하는 **슬롯들이 붙은** 버스 번호.
	 * 설정자: cpqhpc_probe 가 HRT 나 컨트롤러 레지스터에서 얻어 넣는다.
	 * 읽는 자: ctrl_slot_setup 이 슬롯마다 복사하고,
	 *   자원 목록을 만들 때 어느 버스의 자원인지 가릴 때 쓴다.
	 * **컨트롤러 자신이 있는 버스(pci_dev->bus->number)와 다를 수 있다.**
	 * 원문 주석: bus number for the pci hotplug controller
	 * 동기화: 불변 */
	u8 bus;			/* bus number for the pci hotplug controller */
	/* [한국어] 컨트롤러 칩의 리비전 번호.
	 * 설정자: cpqhpc_probe 가 pdev->revision 에서 가져온다.
	 * 읽는 자: cpqhpc_probe 자신이 리비전에 따라 기능 유무를 정한다
	 *   (예: 특정 리비전 아래는 PCI-X 속도 설정을 지원하지 않는다).
	 * 동기화: 불변 */
	u8 rev;
	/* [한국어] **컨트롤러 안 슬롯 번호(0부터)를 PCI 장치 번호로 바꾸는 더하기 값.**
	 * 설정자: cpqhpc_probe 가 SLOT_MASK 레지스터의 상위 니블에서 얻는다.
	 * 읽는 자: interrupt_event_handler 가
	 *   `cpqhp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset)` 으로 쓰고,
	 *   거꾸로 이 파일의 인라인 함수 호출자들이
	 *   `slot->device - ctrl->slot_device_offset` 으로 되돌린다.
	 * **이 값 하나가 두 번호 체계를 잇는다** -- 레지스터의 비트 자리(0부터)와
	 *   PCI 장치 번호(보통 4나 6부터) 사이다.
	 * 동기화: 불변 */
	u8 slot_device_offset;
	/* [한국어] **이 컨트롤러의 첫 슬롯에 적힌 물리 번호.**
	 * 설정자: cpqhpc_probe 가 get_slot_mapping 으로 BIOS IRQ 라우팅 표를
	 *   뒤져 얻는다.
	 * 읽는 자: ctrl_slot_setup 이 slot->number 를 매길 때 시작값으로 쓴다.
	 * **slot_device_offset 과 다르다** -- 그쪽은 PCI 장치 번호로 가는 값,
	 *   이쪽은 섀시에 인쇄된 번호로 가는 값이다. 하나는 하드웨어 주소,
	 *   다른 하나는 사람이 보는 이름이다.
	 * 동기화: 불변 */
	u8 first_slot;
	/* [한국어] 이 컨트롤러가 **빈 슬롯에 새 카드 꽂기** 를 지원하는가.
	 * 설정자: cpqhpc_probe 가 HRT 존재 여부로 정한다 -- 자원 목록을
	 *   BIOS 에서 받아 오지 못하면 새 카드에 자원을 줄 수 없기 때문이다.
	 * 읽는 자: **cpqhp_process_SI 가 0 이면 ADD_NOT_SUPPORTED 로 거절한다.**
	 *   같은 카드로 교체하는 것은 저장해 둔 설정공간을 되쓰면 되므로
	 *   이 값과 무관하게 된다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변 */
	u8 add_support;
	/* [한국어] **버튼 처리 중임을 나타내는 표시.**
	 * 설정자: cpqhp_pushbutton_thread 가 일을 시작하며 1 로,
	 *   마치며 0 으로 되돌린다.
	 * 읽는 자: **읽어서 분기하는 코드를 이 드라이버에서 찾을 수 없다.**
	 *   cpqphp_ctrl.c 의 전역 pushbutton_pending 이 실제 그 역할을 하고,
	 *   이 필드는 함께 갱신되기만 한다.
	 * 동기화: 없음 */
	u8 push_flag;
	/* [한국어] 이 컨트롤러에 **누름 버튼이 달려 있는가.**
	 * 설정자: cpqhpc_probe 가 컨트롤러 능력 레지스터에서 읽는다.
	 * 읽는 자: **handle_presence_change 가 갈래를 정할 때** --
	 *   버튼이 있고 레버가 닫혀 있으면 감지선 변화를 버튼 신호로 읽고,
	 *   버튼이 없으면 카드를 꽂거나 뺀 것으로 읽는다.
	 *   cpqphp_core.c 의 process_SI/process_SS 도 이 값에 따라
	 *   즉시 처리할지 5초 타이머로 갈지 정한다.
	 * 원문 주석: 0 = no pushbutton, 1 = pushbutton present
	 * 동기화: 불변 */
	u8 push_button;			/* 0 = no pushbutton, 1 = pushbutton present */
	/* [한국어] 슬롯에 **레버(스위치)가 달려 있는가.**
	 * 설정자: cpqhpc_probe 가 능력 레지스터에서 읽는다.
	 * 읽는 자: cpqhp_process_SI 와 board_added 가 레버가 닫혀 있는지
	 *   확인해야 하는지 정할 때. 레버가 없으면 그 검사를 건너뛴다.
	 * 원문 주석: 0 = no switch, 1 = switch present
	 * 동기화: 불변 */
	u8 slot_switch_type;		/* 0 = no switch, 1 = switch present */
	/* [한국어] 이 컨트롤러에서 **핫플러그 자체가 켜져 있는가.**
	 * 설정자: cpqhpc_probe 가 능력 레지스터에서 읽는다.
	 * 읽는 자: cpqhpc_probe 자신이 0 이면 이 컨트롤러를 등록하지 않고 뺀다.
	 * **이름이 뒤집혀 보인다** -- defeature(기능 해제)인데 1 이 지원이다.
	 *   원문 주석이 그렇게 밝히고 있으므로 그대로 따른다.
	 * 원문 주석: 0 = PHP not supported, 1 = PHP supported
	 * 동기화: 불변 */
	u8 defeature_PHP;		/* 0 = PHP not supported, 1 = PHP supported */
	/* [한국어] 레지스터를 **다른 BAR 로도 접근할 수 있는가.**
	 * 설정자: cpqhpc_probe 가 능력 레지스터에서 읽는다.
	 * 읽는 자: **이 드라이버 안에서 읽어 분기하는 곳을 찾을 수 없다.**
	 *   능력을 기록만 해 두는 필드다.
	 * 원문 주석: 0 = not supported, 1 = supported
	 * 동기화: 불변 */
	u8 alternate_base_address;	/* 0 = not supported, 1 = supported */
	/* [한국어] 레지스터를 **설정공간의 인덱스/데이터 창** 으로 접근할 수 있는가.
	 * 설정자: cpqhpc_probe 가 능력 레지스터에서 읽는다.
	 * 읽는 자: **이 드라이버 안에서 읽어 분기하는 곳을 찾을 수 없다.**
	 *   MMIO 로만 접근하기 때문이다.
	 * 원문 주석: Index/data access to working registers 0 = not supported, 1 = supported
	 * 동기화: 불변 */
	u8 pci_config_space;		/* Index/data access to working registers 0 = not supported, 1 = supported */
	/* [한국어] 이 컨트롤러가 **PCI-X 속도 설정을 지원하는가.**
	 * 설정자: cpqhpc_probe 가 리비전과 능력 레지스터를 보고 정한다.
	 * 읽는 자: set_controller_speed 가 0 이면 속도 조정을 건너뛴다.
	 * **PCI-X 란**: PCI 를 133MHz 까지 끌어올린 서버용 확장 규격으로,
	 *   같은 버스에 느린 카드가 하나 꽂히면 전체가 그 속도로 떨어진다.
	 *   그래서 카드를 꽂을 때마다 버스 속도를 다시 정해야 한다.
	 * 원문 주석: PCI-X
	 * 동기화: 불변 */
	u8 pcix_speed_capability;	/* PCI-X */
	/* [한국어] 이 컨트롤러가 **PCI-X 버스에 붙어 있는가.**
	 * 설정자: cpqhpc_probe 가 능력 레지스터에서 읽는다.
	 * 읽는 자: set_controller_speed 와 board_added 가 속도 협상 갈래를 정할 때.
	 * **pcix_speed_capability 와 나뉜 이유**: PCI-X 버스이기는 하나
	 *   속도를 바꿔 줄 수는 없는 리비전이 있었기 때문으로 보이나,
	 *   그 배경이 코드에 적혀 있지는 않다.
	 * 원문 주석: PCI-X
	 * 동기화: 불변 */
	u8 pcix_support;		/* PCI-X */
	/* [한국어] 컨트롤러 칩의 PCI 벤더 ID.
	 * 설정자: cpqhpc_probe 가 pdev->vendor 에서 가져온다.
	 * 읽는 자: **Compaq(0x0E11)인지 Intel(0x8086)인지에 따라 레지스터 배치가
	 *   달라지는 자리들이 갈린다** -- cpqhp_ctrl_intr 의 인터럽트 확인 방식과
	 *   cpqphp_core.c 의 초기화 갈래가 이 값을 본다.
	 *   Compaq 을 HP 가, 그 뒤 일부 제품을 Intel 이 이어받으면서
	 *   같은 드라이버가 두 벤더를 다루게 된 흔적이다.
	 * 동기화: 불변 */
	u16 vendor_id;
	/* [한국어] **인터럽트가 미룬 일을 처리할 워크큐 항목. 값으로 박혀 있다.**
	 * 설정자: cpqhpc_probe 가 INIT_WORK 으로 처리 함수를
	 *   interrupt_event_handler 로 걸어 둔다.
	 *   cpqhp_ctrl_intr 이 인터럽트 컨텍스트에서 schedule_work 로 올린다.
	 * 읽는 자: 워크큐 코어가 스레드 컨텍스트에서 부른다. 그 함수는
	 *   이 필드에서 container_of 로 바깥 controller 를 되찾는다.
	 * **왜 미루는가**: 카드에 전원을 넣고 설정공간을 쓰는 일은 잠들 수 있어
	 *   인터럽트 안에서 할 수 없다. 인터럽트는 event_queue 에 적어 두기만
	 *   하고 물러난다.
	 * **세대 대비**: 이 자리가 원래는 전역 커널 스레드와 세마포어였고,
	 *   뒤에 워크큐로 바뀌었다. 지금 세대인 pciehp 는 아예 스레드 IRQ 를
	 *   써서 인터럽트 처리기 자체가 잠들 수 있는 문맥에서 돈다.
	 * 동기화: 워크큐 코어가 관리 */
	struct work_struct int_task_event;
	/* [한국어] **wait_for_ctrl_irq 가 잠드는 대기 큐의 머리.**
	 * 설정자: cpqhpc_probe 가 init_waitqueue_head 로 초기화한다.
	 * 읽는 자: 이 파일의 wait_for_ctrl_irq 가 add_wait_queue 로 자신을
	 *   올리고 잔다. cpqhp_ctrl_intr 이 wake_up_interruptible 로 깨운다.
	 * **다만 그 함수는 조건을 확인하지 않는다** -- 대기 큐에 올린 뒤
	 *   msleep_interruptible(1000) 으로 무조건 1초를 잔다. 깨움이 그 잠을
	 *   줄이는지는 이 스파스 체크아웃에 msleep_interruptible 의 정의가 없어
	 *   **확인할 수 없다.**
	 * 원문 주석: sleep & wake process
	 * 동기화: 대기 큐 자체의 락은 커널이 관리 */
	wait_queue_head_t queue;	/* sleep & wake process */
	/* [한국어] 이 컨트롤러의 debugfs 항목.
	 * 설정자: cpqhp_create_debugfs_files 가 만들고,
	 *   cpqhp_remove_debugfs_files 가 지운다(둘 다 cpqphp_sysfs.c).
	 * 읽는 자: 같은 두 함수.
	 * **무엇이 보이는가**: 컨트롤러 레지스터와 슬롯 상태를 사람이 읽을 수
	 *   있게 찍어 주는 파일이다. sysfs 로는 드러내지 않는 내부 상태를
	 *   들여다보기 위한 것이다.
	 * 원문 주석: debugfs dentry
	 * 동기화: debugfs 코어가 관리 */
	struct dentry *dentry;		/* debugfs dentry */
};

struct irq_mapping {
	/* [한국어] **인터럽트 핀 회전 계수기.**
	 * 설정자: configure_new_function 이 브리지 뒤로 내려갈 때마다
	 *   `(barber_pole + 1) & 0x03` 으로 하나씩 돌린다.
	 * 읽는 자: 같은 함수가 카드의 INTA~INTD 를 실제 인터럽트 선으로
	 *   옮길 때 이 값을 더한다.
	 * **왜 돌리는가**: PCI 규격은 브리지 뒤 장치의 인터럽트 핀이
	 *   장치 번호에 따라 회전해 매핑되도록 정한다. 그래야 네 선에
	 *   부하가 고르게 퍼진다. 이 필드가 그 회전량을 들고 다닌다.
	 * 이름 barber pole 은 이발소 간판의 도는 줄무늬에서 왔다.
	 * 값 범위: 0~3.
	 * 동기화: 없음. 설정 경로 한 곳에서만 쓰인다 */
	u8 barber_pole;
	/* [한국어] **어느 인터럽트 선이 실제로 쓰이는지 나타내는 비트 모음.**
	 * 설정자: configure_new_function 이
	 *   `valid_INT |= 0x01 << (temp_byte + barber_pole - 1) & 0x03` 로 쌓는다.
	 *   **연산자 우선순위 때문에 `<<` 가 먼저 계산되고 그 결과에 & 0x03 이
	 *   걸린다** -- 즉 비트 자리를 0~3 으로 접으려던 것으로 보이는 자리에서
	 *   실제로는 시프트 결과가 마스크된다. 코드는 손대지 않고 사실만 적는다.
	 * 읽는 자: cpqphp_core.c 와 cpqphp_pci.c 의 자원 처리 경로.
	 * 동기화: 없음 */
	u8 valid_INT;
	/* [한국어] INTA, INTB, INTC, INTD 각각에 배정된 실제 인터럽트 선 번호.
	 * 설정자: cpqhp_save_slot_config 계열이 브리지를 타고 내려가며 채운다.
	 * 읽는 자: configure_new_function 이 카드의 PCI_INTERRUPT_LINE 에
	 *   써 넣을 값을 여기서 고른다.
	 * 색인: 0 이 INTA, 3 이 INTD. 설정공간의 PCI_INTERRUPT_PIN 값이
	 *   1~4 이므로 그 값에서 1 을 빼야 이 색인이 된다.
	 * 동기화: 없음 */
	u8 interrupt[4];
};

struct resource_lists {
	/* [한국어] 메모리 자유 목록의 머리.
	 * **이 구조체의 존재 이유**: 카드 하나를 설정하는 데 필요한 자원 목록
	 *   넷과 인터럽트 매핑을 한 덩어리로 묶어 함수 사이에 넘기기 위한 것이다.
	 *   configure_new_device, configure_new_function,
	 *   cpqhp_return_board_resources 가 모두 이 구조체 하나를 주고받는다.
	 * **컨트롤러의 목록을 옮겨 담은 사본이다** -- 호출자가
	 *   `res_lists.mem_head = ctrl->mem_head` 처럼 채워 넘기고,
	 *   일이 끝나면 남은 것을 다시 컨트롤러로 되돌린다.
	 * 동기화: 없음 */
	struct pci_resource *mem_head;
	/* [한국어] prefetchable 메모리 자유 목록의 머리. 위와 같은 방식이다.
	 * 동기화: 없음 */
	struct pci_resource *p_mem_head;
	/* [한국어] IO 자유 목록의 머리. 위와 같은 방식이다.
	 * 동기화: 없음 */
	struct pci_resource *io_head;
	/* [한국어] 버스 번호 자유 목록의 머리. 브리지를 설정할 때만 소모된다.
	 * 동기화: 없음 */
	struct pci_resource *bus_head;
	/* [한국어] 이 카드에 적용할 인터럽트 매핑. **포인터다.**
	 * 값 범위: **NULL 일 수 있다** -- 최상위 버스의 카드는 인터럽트 회전이
	 *   필요 없으므로 호출자가 NULL 로 넘긴다. configure_new_function 이
	 *   `if (resources->irqs)` 로 확인한 뒤에만 쓴다.
	 * **앞의 넷과 달리 포인터인 이유**: 앞의 넷은 리스트의 머리라
	 *   값으로 들고 다녀도 되지만, 이쪽은 브리지를 타고 내려가며
	 *   barber_pole 을 고쳐 나가야 해서 같은 실체를 가리켜야 한다.
	 * 동기화: 없음 */
	struct irq_mapping *irqs;
};

/* [한국어] **시스템 ROM 이 매핑되는 물리 주소(0x0F0000 = 960KB 지점).**
 * PC 구조에서 0xF0000~0xFFFFF 는 BIOS 가 자리 잡는 전통적인 구간이다.
 * 읽는 자: cpqhpc_probe 가 ioremap 으로 이 구간을 매핑해
 *   detect_HRT_floating_pointer 와 detect_SMBIOS_pointer 에게 넘긴다.
 * **왜 여기를 뒤지는가**: HRT 와 SMBIOS 표가 이 안에 있기 때문이다 */
#define ROM_PHY_ADDR			0x0F0000
/* [한국어] 매핑할 ROM 길이(0xffff = 64KB에서 1 모자란 값).
 * **0x10000 이 아니라 0xffff 인 것이 눈에 띄나** 그 이유가 코드에
 * 적혀 있지는 않다. 표 탐색은 16바이트 단위로 훑으므로 실질적 차이는 없다 */
#define ROM_PHY_LEN			0x00ffff

/* [한국어] **이 드라이버가 붙는 핫플러그 컨트롤러의 PCI 장치 ID(0xA0F7).**
 * 벤더는 Compaq(0x0E11)이다.
 * 읽는 자: 이 파일 밖의 pci_device_id 표(cpqphp_core.c)가 이 값으로
 *   장치를 고른다 */
#define PCI_HPC_ID			0xA0F7
/* [한국어] 서브시스템 ID 그 하나(0xA2F7).
 * **같은 장치 ID 아래 여러 기종이 있어 서브시스템 ID 로 갈라낸다** --
 * 기종마다 레지스터 배치나 지원 기능이 조금씩 달랐기 때문이다 */
#define PCI_SUB_HPC_ID			0xA2F7
/* [한국어] 서브시스템 ID 그 둘(0xA2F8) */
#define PCI_SUB_HPC_ID2			0xA2F8
/* [한국어] 서브시스템 ID 그 셋(0xA2F9) */
#define PCI_SUB_HPC_ID3			0xA2F9
/* [한국어] **Intel 이 이어받은 기종의 서브시스템 ID(0xA2FA).**
 * 이름 끝의 INTC 가 Intel 을 가리킨다. Compaq 을 HP 가 인수한 뒤
 * 일부 제품 계열이 Intel 로 넘어간 흔적이며,
 * controller 구조체의 vendor_id 필드가 이 갈래를 들고 다닌다 */
#define PCI_SUB_HPC_ID_INTC		0xA2FA
/* [한국어] 서브시스템 ID 그 넷(0xA2FD) */
#define PCI_SUB_HPC_ID4			0xA2FD

/* [한국어] **event_queue 에 실리는 사건 종류. 여기부터 열 가지다.**
 * 0 -- 버튼을 눌렀으나 무시했다. 이미 다른 작업이 진행 중일 때다.
 * **0 이 "사건 없음" 을 겸한다** -- interrupt_event_handler 가
 * 처리한 칸을 0 으로 지우므로, 이 값은 실제로 큐에 남지 않는다 */
#define INT_BUTTON_IGNORE		0
/* [한국어] 1 -- 카드가 꽂혔다(감지선이 닫혔다) */
#define INT_PRESENCE_ON			1
/* [한국어] 2 -- 카드가 빠졌다(감지선이 열렸다) */
#define INT_PRESENCE_OFF		2
/* [한국어] 3 -- 레버가 닫혔다. 카드를 넣고 잠갔다는 뜻이다 */
#define INT_SWITCH_CLOSE		3
/* [한국어] 4 -- 레버가 열렸다. **동작 중인 카드에서 이 사건이 나면
 * 전원을 끄고 카드를 내려야 한다** */
#define INT_SWITCH_OPEN			4
/* [한국어] 5 -- 슬롯 전원 이상. 카드가 과전류를 끌었을 때 컨트롤러가 알린다 */
#define INT_POWER_FAULT			5
/* [한국어] 6 -- 전원 이상이 풀렸다 */
#define INT_POWER_FAULT_CLEAR		6
/* [한국어] 7 -- 누름 버튼이 눌렸다. **5초 타이머를 걸고 취소를 기다리는 신호다** */
#define INT_BUTTON_PRESS		7
/* [한국어] 8 -- 버튼에서 손을 뗐다 */
#define INT_BUTTON_RELEASE		8
/* [한국어] 9 -- 5초 안에 버튼을 다시 눌러 취소했다.
 * **이 취소 창이 이 드라이버 사용자 경험의 핵심이다** --
 * 실수로 눌렀을 때 되돌릴 틈을 준다 */
#define INT_BUTTON_CANCEL		9

/* [한국어] **slot->state 가 가질 수 있는 다섯 값. 여기부터다.**
 * 0 -- 아무 일도 진행되지 않는 평상 상태 */
#define STATIC_STATE			0
/* [한국어] 1 -- 켜기 예약. LED 가 깜빡이며 5초 취소 창이 열려 있다 */
#define BLINKINGON_STATE		1
/* [한국어] 2 -- 끄기 예약. 마찬가지로 취소를 기다린다 */
#define BLINKINGOFF_STATE		2
/* [한국어] 3 -- 실제로 카드를 올리는 중. 되돌릴 수 없다 */
#define POWERON_STATE			3
/* [한국어] 4 -- 실제로 카드를 내리는 중. 되돌릴 수 없다.
 * **상태 기계가 이 다섯뿐이며 전이 표가 코드에 흩어져 있다** --
 * process_SI/process_SS(cpqphp_core.c)와 cpqhp_pushbutton_thread,
 * interrupt_event_handler(cpqphp_ctrl.c)가 나눠 들고 있다 */
#define POWEROFF_STATE			4

/* [한국어] **슬롯 상태를 사용자 공간에 보고할 때 쓰는 비트 모음. 여기부터다.**
 * 0x01 -- 레버가 닫혀 있다 */
#define PCISLOT_INTERLOCK_CLOSED	0x00000001
/* [한국어] 0x02 -- 카드가 꽂혀 있다 */
#define PCISLOT_ADAPTER_PRESENT		0x00000002
/* [한국어] 0x04 -- 슬롯에 전원이 들어와 있다 */
#define PCISLOT_POWERED			0x00000004
/* [한국어] 0x08 -- 지금 66MHz 로 돌고 있다 */
#define PCISLOT_66_MHZ_OPERATION	0x00000008
/* [한국어] 0x10 -- 지금 64비트 폭으로 돌고 있다 */
#define PCISLOT_64_BIT_OPERATION	0x00000010
/* [한국어] 0x20 -- 같은 카드로 교체하기를 지원한다 */
#define PCISLOT_REPLACE_SUPPORTED	0x00000020
/* [한국어] 0x40 -- 빈 슬롯에 새 카드 넣기를 지원한다.
 * controller 의 add_support 필드가 이 값의 근거다 */
#define PCISLOT_ADD_SUPPORTED		0x00000040
/* [한국어] 0x80 -- 이 슬롯에 레버가 달려 있다 */
#define PCISLOT_INTERLOCK_SUPPORTED	0x00000080
/* [한국어] 0x100 -- 66MHz 를 낼 수 있는 슬롯이다 */
#define PCISLOT_66_MHZ_SUPPORTED	0x00000100
/* [한국어] 0x200 -- 64비트 폭을 낼 수 있는 슬롯이다.
 * **앞의 두 쌍(OPERATION 과 SUPPORTED)을 나눠 둔 이유**: 슬롯이 낼 수
 *   있는 것과 지금 실제로 내고 있는 것이 다르기 때문이다. 같은 버스에
 *   느린 카드가 하나 꽂히면 전체가 33MHz 로 떨어진다 */
#define PCISLOT_64_BIT_SUPPORTED	0x00000200

/* [한국어] **PCI-to-PCI 브리지의 클래스 코드(0x060400).**
 * 상위 바이트 0x06 이 "브리지", 다음 0x04 가 "PCI-to-PCI",
 * 마지막 0x00 이 프로그래밍 인터페이스다.
 * 읽는 자: cpqphp_pci.c 의 설정 경로가 방금 꽂힌 것이 브리지인지
 *   가려낼 때. 브리지면 그 뒤로 버스 번호를 내려 주고 재귀로 훑어야 한다 */
#define PCI_TO_PCI_BRIDGE_CLASS		0x00060400

/* [한국어] **사용자 공간에 돌려주는 오류 코드. 여기부터다.**
 * 0x02 -- 레버가 열려 있어 작업을 진행할 수 없다.
 * **errno 가 아니라 이 드라이버 고유의 값이며** 핫플러그 코어를 거쳐
 *  sysfs 쓰기의 반환값이 된다 */
#define INTERLOCK_OPEN			0x00000002
/* [한국어] 0x03 -- 새 카드 넣기를 지원하지 않는다.
 * controller 의 add_support 가 0 일 때 cpqhp_process_SI 가 돌려준다 */
#define ADD_NOT_SUPPORTED		0x00000003
/* [한국어] 0x05 -- 카드가 이미 잘 돌고 있다. 다시 켤 필요가 없다는 뜻이다 */
#define CARD_FUNCTIONING		0x00000005
/* [한국어] 0x06 -- **꽂힌 카드가 원래 있던 것과 다르다.**
 * cpqhp_valid_replace 가 저장해 둔 설정공간과 비교해 낸 결론이며,
 * add_support 가 없는 컨트롤러에서는 여기서 멈춘다 */
#define ADAPTER_NOT_SAME		0x00000006
/* [한국어] 0x09 -- 슬롯이 비어 있다 */
#define NO_ADAPTER_PRESENT		0x00000009
/* [한국어] 0x0B -- **자유 목록에 남은 자원이 모자란다.**
 * HRT 가 떼어 준 구간을 다 써 버렸을 때 나온다.
 * 이 드라이버가 자원을 스스로 굴리기 때문에 생기는 고유한 실패다 */
#define NOT_ENOUGH_RESOURCES		0x0000000B
/* [한국어] 0x0C -- 이 드라이버가 다룰 수 없는 종류의 장치다 */
#define DEVICE_TYPE_NOT_SUPPORTED	0x0000000C
/* [한국어] 0x0E -- 전원을 넣었으나 이상이 발생했다 */
#define POWER_FAILURE			0x0000000E

/* [한국어] 0x03 -- 빼기를 지원하지 않는다.
 * **ADD_NOT_SUPPORTED 와 값이 같다(둘 다 0x03).**
 * 넣기와 빼기를 서로 다른 경로가 처리하므로 값이 겹쳐도 헷갈리지 않는다 */
#define REMOVE_NOT_SUPPORTED		0x00000003


/*
 * error Messages
 */
/* [한국어] **아래는 로그 문자열 상수들이다. 원문 주석이 error Messages 라 밝힌다.**
 * 초기화 실패. err() 매크로에 넘겨 쓴다.
 * **왜 문자열을 매크로로 빼 두는가**: 2000년대 초 관용구로,
 *   같은 문구를 여러 곳에서 쓰고 한자리에서 고치기 위함이다.
 *   지금 커널은 보통 호출 자리에 직접 적는다 */
#define msg_initialization_err	"Initialization failure, error=%d\n"
/* [한국어] 컨트롤러 리비전이 이 드라이버가 아는 범위를 벗어났다 */
#define msg_HPC_rev_error	"Unsupported revision of the PCI hot plug controller found.\n"
/* [한국어] Compaq 도 Intel 도 아닌 컨트롤러다. vendor_id 검사에서 걸린 경우다 */
#define msg_HPC_non_compaq_or_intel	"The PCI hot plug controller is not supported by this driver.\n"
/* [한국어] 이 시스템 자체를 이 판의 cpqphpd 가 지원하지 않는다.
 * **cpqphpd 는 이 드라이버의 사용자 공간 짝이던 데몬 이름이다** --
 * 초기에는 정책 판단을 사용자 공간이 맡는 구조였다 */
#define msg_HPC_not_supported	"this system is not supported by this version of cpqphpd. Upgrade to a newer version of cpqphpd\n"
/* [한국어] **설정공간을 저장하지 못했다는 치명적 경고.**
 * 저장이 없으면 카드를 뺐다 꽂았을 때 되돌릴 값이 없으므로,
 * 메시지는 재부팅 전에는 장치를 추가하지 말라고 말한다 */
#define msg_unable_to_save	"unable to store PCI hot plug add resource information. This system must be rebooted before adding any PCI devices.\n"
/* [한국어] 버튼을 눌러 전원을 켠다는 알림. 슬롯 번호를 찍는다 */
#define msg_button_on		"PCI slot #%d - powering on due to button press.\n"
/* [한국어] 버튼을 눌러 전원을 끈다는 알림 */
#define msg_button_off		"PCI slot #%d - powering off due to button press.\n"
/* [한국어] 5초 안에 다시 눌러 작업이 취소되었다는 알림 */
#define msg_button_cancel	"PCI slot #%d - action canceled due to button press.\n"
/* [한국어] 이미 작업이 진행 중이라 버튼을 무시했다는 알림.
 * **취소 창이 지난 뒤(POWERON/POWEROFF 상태)에 누른 경우다** */
#define msg_button_ignore	"PCI slot #%d - button press ignored.  (action in progress...)\n"


/* debugfs functions for the hotplug controller info */
/* [한국어] **아래 넷은 cpqphp_sysfs.c 에 있는 debugfs 함수들이다.**
 * 모듈이 올라올 때 debugfs 뿌리를 만든다.
 * 호출자: cpqhpc_init(cpqphp_core.c) */
void cpqhp_initialize_debugfs(void);
/* [한국어] 모듈이 내려갈 때 그 뿌리를 지운다.
 * 호출자: cpqhpc_cleanup */
void cpqhp_shutdown_debugfs(void);
/* [한국어] 컨트롤러 하나에 대한 debugfs 파일을 만든다.
 * 그 결과가 controller 의 dentry 필드에 담긴다.
 * 호출자: cpqhpc_probe */
void cpqhp_create_debugfs_files(struct controller *ctrl);
/* [한국어] 그 파일을 지운다.
 * 호출자: cpqhpc_cleanup 이 컨트롤러 목록을 훑으며 부른다 */
void cpqhp_remove_debugfs_files(struct controller *ctrl);

/* controller functions */
/* [한국어] **아래는 cpqphp_ctrl.c 의 컨트롤러 쪽 함수들이다.**
 * 5초 타이머가 만료되면 불리는 함수. 이름은 스레드지만
 * **실제로는 타이머 콜백이다** -- 예전 설계의 잔재다.
 * 버튼을 눌러 예약된 켜기/끄기를 실제로 수행한다 */
void cpqhp_pushbutton_thread(struct timer_list *t);
/* [한국어] 컨트롤러의 인터럽트 처리기.
 * **인터럽트 컨텍스트에서 돌며 잠들 수 없다** -- 상태 레지스터를 읽고
 * event_queue 에 사건을 적은 뒤 schedule_work 로 물러난다.
 * 호출자: 커널의 인터럽트 코어(cpqhpc_probe 가 request_irq 로 걸었다) */
irqreturn_t cpqhp_ctrl_intr(int IRQ, void *data);
/* [한국어] ROM 의 HRT 를 읽어 컨트롤러의 네 자유 목록을 채운다.
 * **이 드라이버 자원 관리의 출발점이다.**
 * @rom_start 는 cpqhpc_probe 가 ioremap 해 둔 ROM 매핑이다 */
int cpqhp_find_available_resources(struct controller *ctrl,
				   void __iomem *rom_start);
/* [한국어] 사건 처리 스레드를 띄운다.
 * **지금은 워크큐를 쓰므로 이름과 실제가 어긋나 있다** --
 * 초기에는 진짜 커널 스레드 하나가 모든 컨트롤러의 사건을 처리했다 */
int cpqhp_event_start_thread(void);
/* [한국어] 그 스레드를 세운다. 모듈을 내릴 때 부른다 */
void cpqhp_event_stop_thread(void);
/* [한국어] 버스 번호로 pci_func 노드를 새로 만들어
 * 전역 cpqhp_slot_list 의 해당 칸에 매단다 */
struct pci_func *cpqhp_slot_create(unsigned char busnumber);
/* [한국어] 버스·장치·함수 번호로 pci_func 노드를 찾는다.
 * **NULL 을 돌려줄 수 있으나 호출자 여럿이 검사하지 않는다** --
 * handle_switch_change, handle_presence_change, handle_power_fault 가
 * 결과를 바로 역참조한다. 코드는 손대지 않고 사실만 적는다 */
struct pci_func *cpqhp_slot_find(unsigned char bus, unsigned char device,
				 unsigned char index);
/* [한국어] Slot Insert -- 슬롯에 카드를 넣는 전체 절차를 수행한다.
 * add_support 가 없으면 ADD_NOT_SUPPORTED 로 거절한다 */
int cpqhp_process_SI(struct controller *ctrl, struct pci_func *func);
/* [한국어] Slot Standby -- 슬롯의 카드를 내리는 전체 절차를 수행한다 */
int cpqhp_process_SS(struct controller *ctrl, struct pci_func *func);
/* [한국어] 컨트롤러 하드웨어 시험. sysfs 로 시험 번호를 써서 부른다.
 * **LED 를 순서대로 켜 보는 정도의 진단 기능이다** */
int cpqhp_hardware_test(struct controller *ctrl, int test_num);

/* resource functions */
/* [한국어] **자유 목록을 주소 오름차순으로 정렬하고 맞닿은 구간을 합친다.**
 * 이 드라이버가 자원 조각화를 막는 유일한 수단이다.
 * 카드를 빼고 자원을 돌려받을 때마다 불러야 다음 할당이 가능해진다.
 * @head 가 이중 포인터인 것은 머리 자체가 바뀔 수 있기 때문이다.
 * 원문 주석이 밝히듯 이 하나가 resource functions 의 전부다 */
int	cpqhp_resource_sort_and_combine(struct pci_resource **head);

/* pci functions */
/* [한국어] **아래는 cpqphp_pci.c 의 PCI 설정 함수들이다.**
 * 장치의 인터럽트 핀을 실제 IRQ 선에 이어 준다.
 * BIOS 의 IRQ 라우팅 표를 참고해 PCI 인터럽트 라우터를 건드린다 */
int cpqhp_set_irq(u8 bus_num, u8 dev_num, u8 int_pin, u8 irq_num);
/* [한국어] 컨트롤러 안 슬롯 번호에서 버스·장치 번호를 얻는다.
 * 출력이 둘이라 포인터로 돌려준다 */
int cpqhp_get_bus_dev(struct controller *ctrl, u8 *bus_num, u8 *dev_num,
		      u8 slot);
/* [한국어] **버스 하나를 훑어 꽂혀 있는 장치들의 설정공간을 저장한다.**
 * 브리지를 만나면 그 뒤로 재귀한다.
 * @is_hot_plug 가 참이면 핫플러그 슬롯으로 표시해 둔다.
 * 부팅 때 한 번 불러 "원래 무엇이 꽂혀 있었는가" 를 기록한다 */
int cpqhp_save_config(struct controller *ctrl, int busnumber, int is_hot_plug);
/* [한국어] 각 BAR 에 0xFFFFFFFF 를 써 보고 되읽어 크기를 알아낸다.
 * **되읽은 값을 뒤집고 1 을 더하면 크기가 나온다** -- BAR 의 하위
 * 비트가 크기만큼 고정 0 이기 때문이다. PCI 규격이 정한 방식이다 */
int cpqhp_save_base_addr_length(struct controller *ctrl, struct pci_func *func);
/* [한국어] 현재 카드가 쓰고 있는 자원을 pci_func 의 목록에 기록한다.
 * **카드를 뺄 때 무엇을 돌려줘야 하는지 알기 위함이다** */
int cpqhp_save_used_resources(struct controller *ctrl, struct pci_func *func);
/* [한국어] 저장해 둔 설정공간 값을 카드에 되쓴다.
 * **cpqhp_valid_replace 가 BAR 를 0xFFFFFFFF 로 남겨 두므로
 * 이 함수가 반드시 뒤따라야 한다** */
int cpqhp_configure_board(struct controller *ctrl, struct pci_func *func);
/* [한국어] 새로 꽂힌 카드의 함수들을 훑어 pci_func 노드를 만든다.
 * 다기능 카드면 함수마다 노드가 생긴다 */
int cpqhp_save_slot_config(struct controller *ctrl, struct pci_func *new_slot);
/* [한국어] **꽂힌 카드가 원래 있던 것과 같은지 확인한다.**
 * 벤더·장치 ID 와 BAR 크기를 저장해 둔 값과 견준다.
 * 다르면 ADAPTER_NOT_SAME 이 된다 */
int cpqhp_valid_replace(struct controller *ctrl, struct pci_func *func);
/* [한국어] pci_func 이 들고 있던 자원 노드를 모두 해제한다.
 * **원문의 이름과 달리 목록으로 되돌리는 것이 아니라 free 한다** --
 * return_resource 에서 옮겨 온 주석이 남아 있으나 실제 동작은 해제다 */
void cpqhp_destroy_board_resources(struct pci_func *func);
/* [한국어] **카드가 쓰던 자원을 컨트롤러의 자유 목록으로 되돌린다.**
 * 되돌린 뒤 cpqhp_resource_sort_and_combine 으로 병합해야
 * 다음 카드가 큰 덩어리를 받을 수 있다 */
int cpqhp_return_board_resources(struct pci_func *func,
				 struct resource_lists *resources);
/* [한국어] resource_lists 가 들고 있던 네 목록을 모두 해제한다.
 * 이쪽도 원문 주석은 되돌린다고 적혀 있으나 실제 동작은 해제다 */
void cpqhp_destroy_resource_list(struct resource_lists *resources);
/* [한국어] **리눅스 PCI 코어에 이 장치를 등록한다.**
 * pci_scan_slot 과 pci_bus_add_devices 를 불러
 * 일반 드라이버가 이 카드에 붙을 수 있게 만든다.
 * 여기서부터 커널의 보통 PCI 세계로 넘어간다 */
int cpqhp_configure_device(struct controller *ctrl, struct pci_func *func);
/* [한국어] 거꾸로 코어에서 이 장치를 뗀다.
 * pci_stop_and_remove_bus_device 를 부른다 */
int cpqhp_unconfigure_device(struct pci_func *func);

/* Global variables */
/* [한국어] **아래는 cpqphp_core.c 에 정의된 전역 변수들이다.**
 * dbg() 매크로가 이 값이 참일 때만 출력하도록 감싼다.
 * 모듈 인자 debug 로 켠다 */
extern int cpqhp_debug;
/* [한국어] 레거시 모드 표시.
 * **설정자·읽는 자를 이 트리에서 찾아 확인하지 못했다** --
 * 이름과 선언만 남아 있다 */
extern int cpqhp_legacy_mode;
/* [한국어] **컨트롤러 전역 리스트의 머리.**
 * cpqhpc_probe 가 머리에 매달고 cpqhpc_cleanup 이 훑으며 지운다.
 * **락이 없다** -- probe 와 module_exit 은 겹치지 않는다고 본 것이다 */
extern struct controller *cpqhp_ctrl_list;
/* [한국어] **버스 번호로 색인하는 pci_func 리스트 배열. 256칸.**
 * 칸 하나가 그 버스의 함수 노드 연결 리스트 머리다.
 * **전역 배열 하나로 온 시스템의 PCI 함수를 들고 있는 구조이며**,
 * PCI 도메인 개념이 없던 시절의 설계다. 도메인이 여럿이면 버스 번호가
 * 겹칠 수 있으나 이 배열은 그것을 구분하지 못한다.
 * 설정자: cpqhp_slot_create. 읽는 자: cpqhp_slot_find.
 * **락이 없다** */
extern struct pci_func *cpqhp_slot_list[256];
/* [한국어] **BIOS 의 $PIR 인터럽트 라우팅 표 사본.**
 * 설정자: init_cpqhp_routing_table(cpqphp_core.c)이 pcibios_get_irq_routing_table
 *   로 받아 둔다.
 * 읽는 자: get_slot_mapping 이 슬롯의 물리 번호를 찾을 때,
 *   cpqhp_set_irq 가 어느 IRQ 선을 쓸지 정할 때 */
extern struct irq_routing_table *cpqhp_routing_table;

/* these can be gotten rid of, but for debugging they are purty */
/* [한국어] 네트워크 카드용으로 떼어 둔 IRQ 번호.
 * HRT 의 unused_IRQ 비트맵에서 고른 값이다.
 * 원문 주석이 밝히듯 **없어도 되는 값이며 디버깅용으로 남겨 둔 것이다** */
extern u8 cpqhp_nic_irq;
/* [한국어] 디스크 컨트롤러용으로 떼어 둔 IRQ 번호. 위와 같은 출처다.
 * 원문 주석: these can be gotten rid of, but for debugging they are purty */
extern u8 cpqhp_disk_irq;


/* inline functions */

/* [한국어]
 * slot_name - 슬롯의 이름 문자열을 얻는다
 *
 * @slot: 이 드라이버의 슬롯.
 * @return: 핫플러그 코어가 들고 있는 이름 문자열.
 *
 * **두 세계를 잇는 함수 중 하나다.** 이 드라이버의 struct slot 안에는
 * 핫플러그 코어의 struct hotplug_slot 이 값으로 박혀 있고, 이름은 그쪽이
 * 관리한다. 그래서 이름을 얻으려면 안쪽 구조체를 꺼내 코어에 물어야 한다.
 *
 * 이름은 ctrl_slot_setup(cpqphp_core.c)이 pci_hp_register 를 부를 때
 * 슬롯 번호를 문자열로 만들어 넘긴 것이다.
 *
 * **hotplug_slot_name 의 정의는 이 스파스 체크아웃에 없다**
 * (include/linux/pci_hotplug.h 부재). 이름과 쓰임으로만 설명한다.
 *
 * 쓰이는 곳은 cpqphp_core.c 의 sysfs 콜백들이 dbg 로 "어느 슬롯인가" 를
 * 찍을 때뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 접근 경로).
 *
 * 호출 체인:
 *   set_attention_status / process_SI / process_SS / hardware_test /
 *   get_power_status 계열 → [이 함수] → hotplug_slot_name
 */
static inline const char *slot_name(struct slot *slot)
{
	return hotplug_slot_name(&slot->hotplug_slot);
}

/* [한국어]
 * to_slot - 핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다
 *
 * @hotplug_slot: 코어가 넘겨 준 슬롯.
 * @return: 그것을 품고 있는 struct slot.
 *
 * **sysfs 콜백의 첫 줄에 반드시 나오는 함수다.** 핫플러그 코어는
 * struct hotplug_slot 포인터만 알고 있으므로, container_of 로 바깥의
 * struct slot 을 계산해 꺼내야 이 드라이버의 상태에 닿는다.
 *
 * 같은 관용구를 pcie-tegra194.c 의 to_tegra_pcie 나 cpqphp_ctrl.c 의
 * msi_to_pcie 가 쓴다 -- "남이 아는 부분에서 내 전체로 나오는" 변환이다.
 *
 * 이 다리를 건넌 뒤에는 slot->ctrl 로 컨트롤러까지, 거기서 다시
 * ctrl->hpc_reg 로 하드웨어까지 닿을 수 있다. cpqphp_core.c 의 콜백들이
 * 전부 `slot = to_slot(...); ctrl = slot->ctrl;` 두 줄로 시작하는 이유다.
 *
 * 실행 컨텍스트: 어디서나. 포인터 산술뿐이다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 sysfs 콜백 전부 → [이 함수]
 */
static inline struct slot *to_slot(struct hotplug_slot *hotplug_slot)
{
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/*
 * return_resource
 *
 * Puts node back in the resource list pointed to by head
 */
/* [한국어]
 * return_resource - 자유 목록의 **머리에** 노드를 도로 매단다
 *
 * @head: 목록의 머리를 가리키는 포인터의 포인터.
 * @node: 반납할 노드.
 * @return: 없음.
 *
 * **이 드라이버 자원 반납의 최소 단위다.** 세 줄뿐인데, 정렬도 병합도
 * 하지 않고 그냥 머리에 붙인다.
 *
 * 그래서 반납이 반복되면 목록이 주소순이 아니게 되고, 인접한 조각이
 * 합쳐지지 않은 채 흩어진다. 그것을 정리하는 것은
 * cpqhp_resource_sort_and_combine(cpqphp_ctrl.c)의 일이며,
 * **호출자들이 이 함수를 여러 번 부른 뒤 반드시 그것을 부른다** --
 * cpqhp_return_board_resources 가 네 목록을 옮긴 뒤 네 번 정렬하는 것이
 * 그 예다.
 *
 * NULL 검사를 둘 다 하는 것에 주의 -- node 가 NULL 이거나 head 가
 * NULL 이면 조용히 돌아간다. configure_new_function 의 free_and_out
 * 경로가 아직 할당되지 않았을 수 있는 hold_ 사본을 넘기므로 필요한
 * 방어다.
 *
 * 위의 원문 주석이 "노드를 head 가 가리키는 목록에 도로 넣는다" 고
 * 정확히 적고 있다. **다만 cpqphp_pci.c 의 cpqhp_destroy_resource_list 와
 * cpqhp_destroy_board_resources 가 그 문장을 그대로 복사해 갔는데,
 * 그 둘의 실제 동작은 해제다** -- 이 함수의 주석만 사실과 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 반납 경로).
 *
 * 호출 체인:
 *   cpqhp_return_board_resources / configure_new_function → [이 함수]
 */
static inline void return_resource(struct pci_resource **head,
				   struct pci_resource *node)
{
	if (!node || !head)
		return;
	node->next = *head;
	*head = node;
}

/* [한국어]
 * set_SOGO - 설정한 값을 슬롯 하드웨어로 밀어내라고 지시한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * **이 컨트롤러를 다루는 모든 절차의 두 번째 단계다.** LED 나 슬롯 전원을
 * 바꾸려면 먼저 해당 레지스터에 값을 쓰고, 그다음 이 함수로 "지금
 * 밀어내라" 고 지시해야 실제로 반영된다. 그러고 나서
 * wait_for_ctrl_irq 로 완료를 기다린다.
 *
 * 그래서 cpqphp_ctrl.c 와 cpqphp_core.c 에 이 세 줄 묶음이 되풀이된다 --
 * LED 조작 → set_SOGO → wait_for_ctrl_irq.
 *
 * 하는 일은 MISC 레지스터의 **비트 0 을 세우고 비트 2 를 지우는** 것이다.
 * `(misc | 0x0001) & 0xFFFB` 가 그 계산이며, 0xFFFB 는 비트 2 만 0 인
 * 마스크다.
 *
 * 두 비트의 의미는 코드가 쓰는 방식에서만 읽을 수 있다.
 * 비트 0 은 이 함수가 세우는 "밀어내기 시작" 지시로 보이고,
 * 비트 2 는 cpqhp_ctrl_intr 이 인터럽트를 지울 때 세우는 자리
 * (`misc |= 0x0004`)와 같은 자리라 **상태 비트를 실수로 지우지 않으려고
 * 여기서 미리 지우는 것** 으로 읽힌다. 다만 **그 해석의 근거 문서는
 * 이 트리에 없다.**
 *
 * SOGO 라는 이름이 무엇의 줄임인지도 확인할 수 없다. 호출자 쪽 원문
 * 주석이 완료를 기다리는 것을 "SOBS 가 내려가기를 기다린다" 고 적으므로,
 * Serial Output 과 관련된 이름으로 보인다.
 *
 * **읽고-고쳐-쓰기인데 락이 없다.** 대신 호출자가 ctrl->crit_sect 뮤텍스를
 * 쥔 채 부른다 -- cpqphp_ctrl.c 의 board_added 나 cpqphp_core.c 의
 * cpqhp_set_attention_status 가 그렇게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   LED·전원을 바꾸는 거의 모든 함수 → [이 함수] → readw, writew
 */
static inline void set_SOGO(struct controller *ctrl)
{
	/* [한국어] MISC 레지스터는 16비트폭이라 u16 으로 받는다 */
	u16 misc;

	/* [한국어] 현재 MISC 값을 읽어 온다. **읽고-고쳐-쓰기의 첫 단계다** --
	 * 다른 비트를 보존해야 하므로 통째로 덮어쓸 수 없다 */
	misc = readw(ctrl->hpc_reg + MISC);
	/* [한국어] **비트 0 을 세우고(명령 개시) 비트 2 를 지운다.**
	 * 0xFFFB 는 비트 2 만 0 인 마스크다.
	 * 비트 0 을 세우는 것이 컨트롤러에게 "방금 써 넣은 LED·전원 값을
	 * 실제 하드웨어에 반영하라" 고 지시하는 신호다.
	 * **비트 2 를 왜 함께 지우는지는 이 트리에서 확인할 수 없다** --
	 * 컨트롤러 문서가 없으므로 쓰임으로만 적는다 */
	misc = (misc | 0x0001) & 0xFFFB;
	/* [한국어] 고친 값을 되쓴다. 이 쓰기가 실제 명령 개시다.
	 * **직후에 호출자가 wait_for_ctrl_irq 로 완료를 기다린다** --
	 * 둘이 한 쌍이며 그 사이가 나뉘지 않도록 crit_sect 로 감싼다 */
	writew(misc, ctrl->hpc_reg + MISC);
}


/* [한국어]
 * amber_LED_on - 슬롯의 황색(주의) LED 를 켠다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호(0부터).
 * @return: 없음.
 *
 * 황색 LED 는 **오류나 주의를 알리는 표시** 다. 전원 결함이 났을 때
 * (handle_power_fault), 카드 설정에 실패했을 때(board_added 의 오류 경로),
 * 사용자가 sysfs 로 켜라고 했을 때(cpqhp_set_attention_status) 켜진다.
 *
 * 비트 계산이 `0x01010000L << slot` 이다. **LED 제어 레지스터 32비트가
 * 슬롯 여섯 개의 황색·녹색을 나눠 담는 구조** 인데, 황색은 비트 16 과
 * 24 쪽에 두 벌로 놓여 있다. 두 비트를 함께 세우는 것으로 보아 LED
 * 하나를 두 비트로 표현하는 것으로 읽히나, **그 배치의 근거 문서는
 * 이 트리에 없다.**
 *
 * 녹색 쪽인 green_LED_on 이 `0x0101L << slot` 을 쓰는 것과 견주면,
 * 황색과 녹색이 같은 형태의 비트쌍을 16비트 떨어진 자리에 각각 갖고
 * 있음을 알 수 있다.
 *
 * **값을 쓰기만 하고 밀어내지 않는다.** 실제로 LED 가 바뀌려면 호출자가
 * set_SOGO 를 불러야 한다.
 *
 * 읽고-고쳐-쓰기라 호출자가 crit_sect 를 쥐고 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. **인터럽트 컨텍스트에서도 불린다** --
 * handle_power_fault 가 그렇게 한다.
 *
 * 호출 체인:
 *   handle_power_fault / board_added / board_replaced /
 *   cpqhp_pushbutton_thread / cpqhp_set_attention_status → [이 함수]
 */
static inline void amber_LED_on(struct controller *ctrl, u8 slot)
{
	/* [한국어] LED_CONTROL 은 32비트폭이다. 녹색과 황색이 한 레지스터에 들어 있다 */
	u32 led_control;

	/* [한국어] 현재 LED 상태를 통째로 읽는다. 다른 슬롯의 LED 를 보존해야 한다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **황색 LED 켜기 비트를 세운다.**
	 * 0x01010000 은 비트 16 과 비트 24 두 자리다. `<< slot` 으로 슬롯마다
	 * 한 칸씩 옮겨 가므로, 상위 16비트 안에 8슬롯 분의 두 비트 묶음이 들어간다.
	 * **두 비트인 이유는 하드웨어가 켜짐/깜빡임을 따로 두기 때문으로 보이나,
	 * 그 문서는 이 트리에 없다** -- green_LED_blink 가 두 비트 중 하나만
	 * 세우는 것이 그 방증이다 */
	led_control |= (0x01010000L << slot);
	/* [한국어] 되쓴다. **다만 이것만으로는 LED 가 바뀌지 않는다** --
	 * 호출자가 이어서 set_SOGO 를 불러야 하드웨어에 반영된다 */
	writel(led_control, ctrl->hpc_reg + LED_CONTROL);
}


/* [한국어]
 * amber_LED_off - 슬롯의 황색 LED 를 끈다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * amber_LED_on 과 비트 계산이 같고 연산만 `&= ~` 로 바뀐다.
 *
 * 꺼지는 때는 "이제 문제가 없다" 는 뜻이다 -- 카드를 켜기 시작할 때
 * (board_added, board_replaced), 버튼으로 동작을 예약할 때
 * (interrupt_event_handler 의 BUTTON_RELEASE 경로), 취소되었을 때
 * (BUTTON_CANCEL 경로)다.
 *
 * 여기서도 값만 바꾸고 밀어내지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced / interrupt_event_handler /
 *   cpqhp_set_attention_status → [이 함수]
 */
static inline void amber_LED_off(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같다 */
	u32 led_control;

	/* [한국어] 현재 값을 읽어 다른 비트를 보존한다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **같은 두 비트를 지운다.** ~ 로 마스크를 뒤집어 AND 한다.
	 * amber_LED_on 과 정확히 대칭이다 */
	led_control &= ~(0x01010000L << slot);
	/* [한국어] 되쓴다. 반영은 set_SOGO 가 한다 */
	writel(led_control, ctrl->hpc_reg + LED_CONTROL);
}


/* [한국어]
 * read_amber_LED - 슬롯의 황색 LED 가 켜져 있는지 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 켜져 있으면 1, 아니면 0.
 *
 * amber_LED_on 과 같은 마스크를 쓰되 **읽어서 검사만 한다.**
 * 두 비트 중 하나라도 서 있으면 1 을 돌려주므로, 엄밀히는 "완전히 켜짐"
 * 과 "반만 켜짐" 을 구별하지 않는다.
 *
 * 삼항 연산자로 0/1 로 눌러 주는 것에 주의 -- 마스크한 값 자체는
 * 0x01010000 처럼 큰 수라, 그대로 돌려주면 sysfs 가 기대하는 불리언이
 * 되지 않는다.
 *
 * **LED 상태를 하드웨어에서 되읽는 유일한 함수다.** 소프트웨어가 따로
 * 기억해 두지 않고 레지스터를 진실의 원천으로 삼는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 조회 경로).
 *
 * 호출 체인:
 *   cpq_get_attention_status → [이 함수] → readl
 */
static inline int read_amber_LED(struct controller *ctrl, u8 slot)
{
	/* [한국어] 읽기 전용이지만 레지스터 폭이 같으므로 u32 로 받는다 */
	u32 led_control;

	/* [한국어] 현재 LED 상태를 읽는다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **이 슬롯의 황색 비트 두 자리만 남긴다.** 나머지는 모두 0 이 된다 */
	led_control &= (0x01010000L << slot);

	/* [한국어] 남은 값이 0 이 아니면 켜져 있다는 뜻이다.
	 * **두 비트 중 하나만 서 있어도 1 을 돌려준다** */
	return led_control ? 1 : 0;
}


/* [한국어]
 * green_LED_on - 슬롯의 녹색(전원) LED 를 켠다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * 녹색 LED 는 **카드가 정상 동작 중임을 알리는 표시** 다.
 * board_added 가 설정을 모두 마친 뒤 마지막으로 켠다.
 *
 * 비트 계산이 `0x0101L << slot` 이라 황색(`0x01010000L << slot`)과
 * 정확히 16비트 떨어져 있다. 즉 **하위 16비트가 녹색, 상위 16비트가
 * 황색** 이며, 각각 슬롯당 두 비트씩 8비트 간격으로 놓인 것으로 읽힌다.
 * cpqhp_hardware_test 가 상하위 16비트를 맞바꾸며 시험 패턴을 만드는
 * 것도 이 배치와 맞아떨어진다.
 *
 * **"켜짐" 과 "깜빡임" 이 같은 두 비트로 표현된다** -- green_LED_blink 를
 * 보면 두 비트 중 하나만 세우는 것이 깜빡임이다. 그래서 이 함수가
 * 두 비트를 모두 세우는 것이 "완전히 켜짐" 이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트 컨텍스트에서도 불릴 수 있다.
 *
 * 호출 체인:
 *   board_added / interrupt_event_handler / cpqhp_pushbutton_thread
 *     → [이 함수]
 */
static inline void green_LED_on(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같은 레지스터를 다룬다 */
	u32 led_control;

	/* [한국어] 현재 값을 읽는다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **녹색(전원) LED 켜기.**
	 * 0x0101 은 비트 0 과 비트 8 이다. 황색이 상위 16비트를 쓰는 것과 짝을 이뤄
	 * 녹색은 하위 16비트를 쓴다. 한 레지스터에 두 색이 나뉘어 들어간 것이다 */
	led_control |= 0x0101L << slot;
	/* [한국어] 되쓴다. 반영은 set_SOGO 가 한다 */
	writel(led_control, ctrl->hpc_reg + LED_CONTROL);
}

/* [한국어]
 * green_LED_off - 슬롯의 녹색 LED 를 끈다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * green_LED_on 과 같은 마스크로 두 비트를 모두 지운다.
 *
 * 꺼지는 때는 카드를 끄거나 설정에 실패했을 때다 -- remove_board,
 * board_added 의 오류 경로, handle_power_fault(rev < 4), 그리고
 * cpqphp_core.c 의 cpqhpc_probe 가 부팅 시 빈 슬롯을 끌 때다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트 컨텍스트에서도 불린다.
 *
 * 호출 체인:
 *   remove_board / board_added / board_replaced / handle_power_fault /
 *   interrupt_event_handler / cpqhpc_probe → [이 함수]
 */
static inline void green_LED_off(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같다 */
	u32 led_control;

	/* [한국어] 현재 값을 읽는다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] 녹색 두 비트를 모두 지운다. green_LED_on 과 대칭이다 */
	led_control &= ~(0x0101L << slot);
	/* [한국어] 되쓴다 */
	writel(led_control, ctrl->hpc_reg + LED_CONTROL);
}


/* [한국어]
 * green_LED_blink - 슬롯의 녹색 LED 를 깜빡인다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * **두 비트 중 하나만 세우는 것이 깜빡임이다.** 먼저 두 비트를 모두
 * 지우고(`&= ~(0x0101L << slot)`) 아래쪽 하나만 세운다
 * (`|= (0x0001L << slot)`). green_LED_on 이 둘 다 세우는 것과 대비된다.
 *
 * 즉 이 하드웨어는 LED 하나를 두 비트로 표현하며, 그 조합이
 * 꺼짐(00) / 깜빡임(01) / 켜짐(11) 세 상태를 나타내는 것으로 읽힌다.
 * **그 인코딩의 근거 문서는 이 트리에 없고**, 세 함수가 쓰는 비트 조합에서
 * 읽어 낸 것이다.
 *
 * 깜빡임이 뜻하는 것은 "진행 중" 이다. 버튼을 눌러 5초 대기에 들어갔을 때
 * (interrupt_event_handler), 카드에 전원을 넣고 설정하는 동안
 * (board_added, board_replaced) 깜빡인다. 사용자에게 "지금 뭔가 하고
 * 있으니 기다리거나 취소하라" 를 알리는 신호다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced / interrupt_event_handler → [이 함수]
 */
static inline void green_LED_blink(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같다 */
	u32 led_control;

	/* [한국어] 현재 값을 읽는다 */
	led_control = readl(ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **먼저 녹색 두 비트를 모두 지운다.** 켜짐 상태를 확실히 없애기 위함이다 */
	led_control &= ~(0x0101L << slot);
	/* [한국어] **그다음 아래쪽 한 비트만 세운다.**
	 * 두 비트가 모두 서면 상시 점등, 아래 비트만 서면 깜빡임이다.
	 * 이 함수가 두 비트의 의미가 다르다는 것을 보여 주는 자리다.
	 * **깜빡임은 5초 취소 창이 열려 있다는 표시이며**,
	 * slot->state 의 BLINKINGON_STATE / BLINKINGOFF_STATE 와 짝을 이룬다 */
	led_control |= (0x0001L << slot);
	/* [한국어] 되쓴다. 반영은 set_SOGO 가 한다 */
	writel(led_control, ctrl->hpc_reg + LED_CONTROL);
}


/* [한국어]
 * slot_disable - 슬롯을 버스에서 뗀다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * SLOT_ENABLE 레지스터의 해당 비트를 내린다. 슬롯당 비트 하나이며
 * (`0x01 << slot`), LED 가 두 비트를 쓰는 것과 다르다.
 *
 * **disable_slot_power 와 구별해야 한다.**
 *   slot_disable       : 카드를 **버스에서 뗀다.** 전원은 그대로일 수 있다.
 *   disable_slot_power : **전원만** 끊는다. 버스 연결과는 별개다.
 * 두 단계를 나눈 덕에 board_added 가 "버스에 붙이지 않고 전원만 넣어
 * 속도를 확인한 뒤 다시 끄는" 절차를 밟을 수 있다 -- 속도가 맞지 않는
 * 카드를 버스에 붙이면 같은 세그먼트의 다른 카드가 오동작하기 때문이다.
 *
 * 읽고-고쳐-쓰기라 호출자가 crit_sect 를 쥔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   remove_board / board_added(오류) / board_replaced(오류) /
 *   cpqhpc_probe → [이 함수]
 */
static inline void slot_disable(struct controller *ctrl, u8 slot)
{
	/* [한국어] SLOT_ENABLE 은 8비트폭이다. **슬롯당 한 비트이므로 최대 8슬롯이다** */
	u8 slot_enable;

	/* [한국어] 현재 활성 비트 모음을 읽는다 */
	slot_enable = readb(ctrl->hpc_reg + SLOT_ENABLE);
	/* [한국어] **이 슬롯의 비트를 지워 버스에서 떼어 낸다.**
	 * 전원 자체는 SLOT_POWER 가 따로 관리하며, 이쪽은 버스 연결 쪽이다 */
	slot_enable &= ~(0x01 << slot);
	/* [한국어] 되쓴다. 반영은 set_SOGO 가 한다 */
	writeb(slot_enable, ctrl->hpc_reg + SLOT_ENABLE);
}


/* [한국어]
 * slot_enable - 슬롯을 버스에 붙인다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * slot_disable 의 반대로 SLOT_ENABLE 비트를 세운다. 이 순간부터 카드가
 * PCI 버스에 응답하기 시작한다.
 *
 * **호출 순서가 중요하다.** board_added 와 board_replaced 는
 *   enable_slot_power 로 전원만 넣고 → 속도를 확인하고 →
 *   disable_slot_power 로 껐다가 → **그다음에야** slot_enable 을 부른다.
 * 속도 협상이 끝나기 전에 버스에 붙이지 않으려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced → [이 함수]
 */
static inline void slot_enable(struct controller *ctrl, u8 slot)
{
	/* [한국어] **지역 변수 이름이 함수 이름과 같다.**
	 * C 에서 문제가 되지는 않으나 읽기에 혼란스러운 자리다. 코드는 손대지 않는다 */
	u8 slot_enable;

	/* [한국어] 현재 값을 읽는다 */
	slot_enable = readb(ctrl->hpc_reg + SLOT_ENABLE);
	/* [한국어] 이 슬롯의 비트를 세워 버스에 붙인다. slot_disable 과 대칭이다 */
	slot_enable |= (0x01 << slot);
	/* [한국어] 되쓴다 */
	writeb(slot_enable, ctrl->hpc_reg + SLOT_ENABLE);
}


/* [한국어]
 * is_slot_enabled - 슬롯이 버스에 붙어 있는지 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 붙어 있으면 1, 아니면 0.
 *
 * SLOT_ENABLE 레지스터의 해당 비트를 읽어 0/1 로 눌러 돌려준다.
 *
 * **이 드라이버의 상태 판단이 이 함수 하나에 크게 기댄다.**
 *   interrupt_event_handler 가 버튼을 뗐을 때 "켜려는 것인가 끄려는
 *     것인가" 를 이 값으로 정한다.
 *   cpqhp_pushbutton_thread 가 5초 뒤 실제로 켤지 끌지도 이 값으로 정한다.
 *   board_replaced 가 "이미 켜져 있으면 CARD_FUNCTIONING" 으로 거절하는
 *     것도 이 값이다.
 * 즉 소프트웨어가 따로 기억하지 않고 **하드웨어 레지스터를 진실의 원천으로
 * 삼는다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   interrupt_event_handler / cpqhp_pushbutton_thread / board_replaced /
 *   get_slot_enabled → [이 함수] → readb
 */
static inline u8 is_slot_enabled(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같은 레지스터를 읽기 전용으로 다룬다 */
	u8 slot_enable;

	/* [한국어] 현재 값을 읽는다 */
	slot_enable = readb(ctrl->hpc_reg + SLOT_ENABLE);
	/* [한국어] 이 슬롯의 비트만 남긴다 */
	slot_enable &= (0x01 << slot);
	/* [한국어] 비트 자리 값이 아니라 **0/1 로 정규화해** 돌려준다.
	 * 호출자가 비트 위치를 몰라도 되게 하려는 것이다 */
	return slot_enable ? 1 : 0;
}


/* [한국어]
 * read_slot_enable - SLOT_ENABLE 레지스터를 통째로 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 여덟 슬롯의 활성화 비트가 담긴 바이트.
 *
 * is_slot_enabled 가 비트 하나를 보는 데 비해 이쪽은 **전체를 그대로**
 * 돌려준다. 여러 슬롯의 상태를 한꺼번에 봐야 할 때 쓴다.
 *
 * 쓰이는 곳이 둘이다.
 *   ctrl_slot_setup(cpqphp_core.c) : 슬롯을 등록하며 capabilities 에
 *     "지금 켜져 있음" 비트를 넣을 때 쓴다. 읽은 값을 2비트 왼쪽으로
 *     민 뒤 슬롯 번호만큼 오른쪽으로 밀어 비트 2 자리에 맞춘다.
 *   unload_cpqphpd(cpqphp_core.c) : 모듈을 내릴 때 읽어,
 *     **꺼져 있는 슬롯의 인터럽트만 마스크한다**
 *     (`writel(0xFFFFFFC0L | ~rc, ...)`). 켜져 있는 슬롯은 계속
 *     인터럽트를 받게 두는 셈이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ctrl_slot_setup / unload_cpqphpd → [이 함수] → readb
 */
static inline u8 read_slot_enable(struct controller *ctrl)
{
	/* [한국어] **여덟 슬롯의 활성 비트를 한 번에 통째로 돌려준다.**
	 * 슬롯 하나만 보는 is_slot_enabled 와 달리 전체 그림이 필요한 곳,
	 * 예컨대 카드를 내리기 전에 다른 슬롯이 살아 있는지 볼 때 쓴다 */
	return readb(ctrl->hpc_reg + SLOT_ENABLE);
}


/**
 * get_controller_speed - find the current frequency/mode of controller.
 *
 * @ctrl: controller to get frequency/mode for.
 *
 * Returns controller speed.
 */
/* [한국어]
 * get_controller_speed - 컨트롤러가 지금 돌고 있는 주파수와 모드를 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: PCI_SPEED_ 계열 값.
 *
 * **PCI-X 를 지원하는지에 따라 읽는 레지스터가 다르다.**
 *
 * PCI-X 지원(ctrl->pcix_support)이면 NEXT_CURR_FREQ 레지스터를 읽어
 * 상위 니블로 판정한다. 검사가 **높은 속도부터 차례로** 이뤄지는데,
 * 마스크가 겹치기 때문이다 -- 0xB0 은 0xA0 과 0x90 의 비트를 포함하므로
 * 순서를 뒤집으면 133MHz 를 100MHz 로 잘못 읽는다.
 *   0xB0 → PCI-X 133MHz
 *   0xA0 → PCI-X 100MHz
 *   0x90 → PCI-X 66MHz
 *   0x10 → PCI 66MHz (이것만 단일 비트 검사다)
 *   그 밖 → PCI 33MHz
 *
 * PCI-X 를 지원하지 않으면 MISC 레지스터의 비트 11 하나만 본다 --
 * 66MHz 냐 33MHz 냐 둘 중 하나다.
 *
 * 위의 원문 kernel-doc 가 "컨트롤러 속도를 돌려준다" 고 요약한다.
 *
 * **이 값이 set_controller_speed 의 판단 근거가 된다.**
 * cpqphp_ctrl.c 가 새 카드의 속도와 이 값을 견줘 버스 주파수를 바꿀지
 * 정하는데, 다만 실제로는 bus->cur_bus_speed 에 저장된 소프트웨어
 * 사본을 쓰고 이 함수는 부팅 때 그 사본을 채우는 데 쓰인다
 * (cpqphp_core.c 의 cpqhpc_probe).
 *
 * **니블 값의 의미는 이 트리에서 확인할 수 없다.** 어떤 비트가 무엇을
 * 뜻하는지는 이 함수가 비교하는 방식으로만 읽힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqhpc_probe → [이 함수] → readb, readw
 */
static inline u8 get_controller_speed(struct controller *ctrl)
{
	/* [한국어] PCI-X 경로에서 NEXT_CURR_FREQ 레지스터 값을 담을 자리 */
	u8 curr_freq;
	/* [한국어] PCI 경로에서 MISC 레지스터 값을 담을 자리. 두 경로가 보는 곳이 다르다 */
	u16 misc;

	/* [한국어] **PCI-X 컨트롤러인가로 갈래가 갈린다.**
	 * PCI-X 는 속도 등급이 넷이라 전용 레지스터를 봐야 하고,
	 * 보통 PCI 는 66MHz 인지 아닌지 한 비트면 된다 */
	if (ctrl->pcix_support) {
		/* [한국어] 현재/다음 주파수 레지스터를 읽는다.
		 * **상위 니블이 현재 속도 코드다** -- 아래 비교가 모두 0xF0 자리를 본다 */
		curr_freq = readb(ctrl->hpc_reg + NEXT_CURR_FREQ);
		/* [한국어] 상위 니블이 0xB 면 **PCI-X 133MHz**.
		 * **정확히 같은지를 보는 것이 아니라 0xB0 비트가 모두 서 있는지 본다** --
		 * 0xF0 같은 값도 이 조건을 통과한다. 코드가 그렇게 쓰여 있다 */
		if ((curr_freq & 0xB0) == 0xB0)
			return PCI_SPEED_133MHz_PCIX;
		/* [한국어] 0xA 면 **PCI-X 100MHz**. 위 검사를 통과하지 못한 뒤이므로
		 * 0xB 는 이미 걸러졌다. 빠른 것부터 차례로 보는 구조다 */
		if ((curr_freq & 0xA0) == 0xA0)
			return PCI_SPEED_100MHz_PCIX;
		/* [한국어] 0x9 면 **PCI-X 66MHz** */
		if ((curr_freq & 0x90) == 0x90)
			return PCI_SPEED_66MHz_PCIX;
		/* [한국어] 위 셋에 걸리지 않았는데 비트 4 가 서 있으면 **보통 PCI 66MHz** 다.
		 * 같은 컨트롤러가 PCI-X 카드가 없을 때는 PCI 속도로 떨어지기 때문이다 */
		if (curr_freq & 0x10)
			return PCI_SPEED_66MHz;

		/* [한국어] 아무 비트도 서 있지 않으면 33MHz 다. PCI-X 경로의 마지막 갈래 */
		return PCI_SPEED_33MHz;
	}

	/* [한국어] 보통 PCI 컨트롤러 경로. MISC 레지스터 하나면 충분하다 */
	misc = readw(ctrl->hpc_reg + MISC);
	/* [한국어] **비트 11 이 66MHz 여부다.**
	 * 서 있으면 66MHz, 아니면 33MHz.
	 * **그 비트 번호의 근거 문서는 이 트리에 없다** -- 쓰임으로만 적는다 */
	return (misc & 0x0800) ? PCI_SPEED_66MHz : PCI_SPEED_33MHz;
}


/**
 * get_adapter_speed - find the max supported frequency/mode of adapter.
 *
 * @ctrl: hotplug controller.
 * @hp_slot: hotplug slot where adapter is installed.
 *
 * Returns adapter speed.
 */
/* [한국어]
 * get_adapter_speed - 슬롯에 꽂힌 카드가 낼 수 있는 최대 속도를 읽는다
 *
 * @ctrl:    대상 컨트롤러.
 * @hp_slot: 컨트롤러 안의 슬롯 번호.
 * @return: PCI_SPEED_ 계열 값.
 *
 * 카드가 자기 능력을 알리는 신호(PCIXCAP 등)를 컨트롤러가 모아 둔
 * NON_INT_INPUT 레지스터를 읽는다. **슬롯마다 비트가 세 자리에 흩어져
 * 있다.**
 *   비트 16+slot → PCI-X 133MHz
 *   비트  8+slot → PCI-X 66MHz
 *   비트  0+slot → PCI 66MHz
 *   아무것도 없으면 → PCI 33MHz
 *
 * 앞의 둘은 PCI-X 를 지원하는 컨트롤러에서만 본다 -- 지원하지 않으면
 * 그 비트가 의미 없기 때문이다.
 *
 * **여기서도 검사 순서가 높은 속도부터다.** 다만 이쪽은 마스크가 겹치지
 * 않는 단일 비트 검사라, get_controller_speed 와 달리 순서가 정확성에
 * 영향을 주지는 않는다.
 *
 * **PCI-X 100MHz 를 판정하는 경로가 없다** -- 133MHz, 66MHz PCI-X,
 * 66MHz PCI, 33MHz 넷만 돌려준다. 카드가 100MHz 를 광고할 방법이
 * 이 레지스터에 없기 때문으로 보이나, 그 근거는 이 트리에 없다.
 *
 * 맨 앞에서 읽은 값을 dbg 로 찍는다. 속도 협상이 어긋나면 원인을 찾기
 * 어려워 남긴 로그로 보인다.
 *
 * **이 값이 핫애드 절차의 첫 관문이다.** board_added 와 board_replaced 가
 * 버스에 붙이기 전에 전원만 넣고 이 값을 읽어, set_controller_speed 로
 * 버스 주파수를 맞출 수 있는지 판단한다. 맞출 수 없으면
 * WRONG_BUS_FREQUENCY 로 카드를 거절한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced → [이 함수] → readl
 */
static inline u8 get_adapter_speed(struct controller *ctrl, u8 hp_slot)
{
	/* [한국어] **NON_INT_INPUT 은 인터럽트를 일으키지 않는 상태 입력이다.**
	 * 카드가 스스로 알리는 PCIXCAP 신호선이 여기 실린다.
	 * 선언과 읽기를 한 줄에 합쳐 둔 것이 이 파일에서는 드문 형태다 */
	u32 temp_dword = readl(ctrl->hpc_reg + NON_INT_INPUT);
	/* [한국어] 디버그 출력. cpqhp_debug 가 참일 때만 찍힌다.
	 * **PCIXCAP 은 PCI-X 규격이 정한 카드 능력 표시 핀 이름이다** */
	dbg("slot: %d, PCIXCAP: %8x\n", hp_slot, temp_dword);
	/* [한국어] 컨트롤러가 PCI-X 를 못 하면 카드가 뭐라 하든 소용없으므로 건너뛴다 */
	if (ctrl->pcix_support) {
		/* [한국어] 비트 16 부터가 **133MHz 가능 표시** 자리다. 슬롯마다 한 비트 */
		if (temp_dword & (0x10000 << hp_slot))
			return PCI_SPEED_133MHz_PCIX;
		/* [한국어] 비트 8 부터가 **PCI-X 66MHz 가능 표시** 자리다 */
		if (temp_dword & (0x100 << hp_slot))
			return PCI_SPEED_66MHz_PCIX;
	}

	/* [한국어] 비트 0 부터가 **보통 PCI 66MHz 가능 표시** 자리다.
	 * PCI-X 갈래 밖이므로 컨트롤러 종류와 무관하게 본다 */
	if (temp_dword & (0x01 << hp_slot))
		return PCI_SPEED_66MHz;

	/* [한국어] 아무 비트도 없으면 33MHz 카드다.
	 * **호출자는 이 값과 get_controller_speed 의 값을 견주어
	 * 버스 전체를 느린 쪽에 맞춘다** -- 한 슬롯의 느린 카드가
	 * 버스 전체를 끌어내리는 것이 PCI 의 성질이다 */
	return PCI_SPEED_33MHz;
}

/* [한국어]
 * enable_slot_power - 슬롯에 전원만 넣는다 (버스에는 붙이지 않는다)
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * SLOT_POWER 레지스터의 해당 비트를 세운다.
 *
 * **slot_enable 과 반드시 구별해야 한다.** 이 함수는 카드에 전기만
 * 공급하고 버스에는 연결하지 않는다. 그래서 카드가 살아나 자기 속도
 * 능력 신호를 내보내기 시작하지만, 아직 PCI 트랜잭션에는 응답하지 않는다.
 *
 * 그 구분이 있어야 board_added 의 절차가 성립한다:
 *   enable_slot_power  → 전원만 넣는다
 *   get_adapter_speed  → 카드가 낼 수 있는 속도를 읽는다
 *   set_controller_speed → 버스 주파수를 맞춘다
 *   disable_slot_power → 다시 끈다
 *   slot_enable        → 이제 버스에 붙인다
 * 속도가 맞지 않는 카드를 버스에 붙이면 같은 세그먼트의 다른 카드가
 * 오동작하므로, 붙이기 전에 확인하는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced → [이 함수]
 */
static inline void enable_slot_power(struct controller *ctrl, u8 slot)
{
	/* [한국어] SLOT_POWER 도 8비트폭이며 슬롯당 한 비트다 */
	u8 slot_power;

	/* [한국어] 현재 전원 비트 모음을 읽는다 */
	slot_power = readb(ctrl->hpc_reg + SLOT_POWER);
	/* [한국어] **이 슬롯에 전원을 넣는다.**
	 * slot_enable 과 나뉜 이유는 순서 때문이다 -- 전원을 먼저 넣어
	 * 카드를 안정시킨 뒤 버스에 붙여야 한다 */
	slot_power |= (0x01 << slot);
	/* [한국어] 되쓴다. 반영은 set_SOGO 가 한다 */
	writeb(slot_power, ctrl->hpc_reg + SLOT_POWER);
}

/* [한국어]
 * disable_slot_power - 슬롯의 전원만 끊는다
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 컨트롤러 안의 슬롯 번호.
 * @return: 없음.
 *
 * enable_slot_power 의 반대다. 속도 확인이 끝난 뒤 카드를 잠시 꺼 두는
 * 데 쓴다.
 *
 * **이 함수를 부르는 곳이 board_added 와 board_replaced 둘뿐이고,
 * 둘 다 속도 확인 직후다.** 카드를 완전히 제거할 때는 이것이 아니라
 * slot_disable 을 쓴다 -- remove_board 가 그렇게 한다.
 *
 * 즉 이 함수는 "잠깐 켜 봤다가 다시 끄는" 용도 전용이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / board_replaced → [이 함수]
 */
static inline void disable_slot_power(struct controller *ctrl, u8 slot)
{
	/* [한국어] 위와 같다 */
	u8 slot_power;

	/* [한국어] 현재 값을 읽는다 */
	slot_power = readb(ctrl->hpc_reg + SLOT_POWER);
	/* [한국어] 이 슬롯의 전원을 끊는다.
	 * **내릴 때는 순서가 반대다** -- 버스에서 떼어 낸 뒤 전원을 끊는다 */
	slot_power &= ~(0x01 << slot);
	/* [한국어] 되쓴다 */
	writeb(slot_power, ctrl->hpc_reg + SLOT_POWER);
}


/* [한국어]
 * cpq_get_attention_status - sysfs 용 황색 LED 상태 조회
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 사용자에게 보이는 슬롯.
 * @return: 황색 LED 가 켜져 있으면 1, 아니면 0.
 *
 * read_amber_LED 를 감싸는 얇은 껍데기다. **두 가지 번호 체계를
 * 변환하는 것이 이 함수의 실질적 역할이다.**
 *
 *   slot->device            : PCI 버스 위의 장치 번호
 *   ctrl->slot_device_offset : 이 컨트롤러의 첫 슬롯이 쓰는 장치 번호
 *   둘의 차                  : 컨트롤러 안의 슬롯 번호(0부터)
 *
 * 인라인 함수들은 마지막 것을 받으므로, sysfs 쪽에서 온
 * struct slot 을 그 번호로 바꿔 줘야 한다. 그 변환이
 * `slot->device - ctrl->slot_device_offset` 한 줄이며,
 * 아래 세 함수(get_slot_enabled, cpq_get_latch_status,
 * get_presence_status)도 똑같이 시작한다.
 *
 * 이름에 cpq_ 접두사가 붙은 것은 핫플러그 코어의
 * get_attention_status 콜백과 이름이 겹치기 때문으로 보인다 --
 * cpqphp_core.c 에 같은 이름의 static 함수가 따로 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:
 *   cpqphp_core.c 의 get_attention_status → [이 함수] → read_amber_LED
 */
static inline int cpq_get_attention_status(struct controller *ctrl, struct slot *slot)
{
	/* [한국어] 컨트롤러 안 슬롯 번호(0부터)를 담을 자리 */
	u8 hp_slot;

	/* [한국어] **두 번호 체계를 잇는 계산이다.**
	 * slot->device 는 PCI 장치 번호, slot_device_offset 은 첫 슬롯의 장치 번호.
	 * 빼면 레지스터 비트 자리(0부터)가 나온다.
	 * **이 파일의 네 조회 함수가 모두 이 한 줄로 시작한다** */
	hp_slot = slot->device - ctrl->slot_device_offset;

	/* [한국어] 황색 LED 가 켜져 있는지가 곧 주의 상태다.
	 * **LED 자체를 상태 저장소로 쓰는 셈이다** -- 따로 변수를 두지 않는다 */
	return read_amber_LED(ctrl, hp_slot);
}


/* [한국어]
 * get_slot_enabled - sysfs 용 슬롯 전원 상태 조회
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 사용자에게 보이는 슬롯.
 * @return: 버스에 붙어 있으면 1, 아니면 0.
 *
 * is_slot_enabled 를 감싸며 슬롯 번호를 변환한다. 구조가
 * cpq_get_attention_status 와 같다.
 *
 * **이름이 헷갈리기 쉽다** -- "enabled" 는 전원이 아니라 **버스 연결**
 * 여부다. SLOT_ENABLE 레지스터를 보기 때문이며, 전원 자체를 담는
 * SLOT_POWER 레지스터를 읽는 함수는 이 헤더에 없다.
 *
 * sysfs 의 power 파일이 이 값을 보여 준다 -- cpqphp_core.c 의
 * get_power_status 가 이 함수를 그대로 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:
 *   cpqphp_core.c 의 get_power_status /
 *   cpqphp_ctrl.c 의 set_controller_speed → [이 함수] → is_slot_enabled
 */
static inline int get_slot_enabled(struct controller *ctrl, struct slot *slot)
{
	/* [한국어] 위와 같다 */
	u8 hp_slot;

	/* [한국어] 같은 번호 변환 */
	hp_slot = slot->device - ctrl->slot_device_offset;

	/* [한국어] 슬롯 활성 비트를 그대로 전원 상태로 보고한다 */
	return is_slot_enabled(ctrl, hp_slot);
}


/* [한국어]
 * cpq_get_latch_status - 슬롯 레버(래치)가 닫혀 있는지 조회
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 사용자에게 보이는 슬롯.
 * @return: 레버가 **닫혀** 있으면 1, 열려 있으면 0.
 *
 * INT_INPUT_CLEAR 레지스터의 하위 8비트가 슬롯별 레버 상태다.
 * 슬롯 번호에 해당하는 비트를 뽑아 본다.
 *
 * **논리가 뒤집혀 있다** -- 비트가 서 있으면 레버가 **열린** 것이고,
 * 이 함수는 `(status == 0) ? 1 : 0` 으로 그것을 뒤집어
 * "닫혀 있으면 1" 로 돌려준다. sysfs 의 latch 파일이 "닫힘=1" 을
 * 기대하기 때문으로 보인다.
 *
 * 같은 비트를 cpqphp_ctrl.c 가 반대 방향으로 읽는다 --
 * cpqhp_process_SI 가 `if (tempdword & (0x01 << hp_slot)) return 1;`
 * 로 "비트가 서 있으면(열려 있으면) 켜기를 거절" 하고,
 * handle_switch_change 도 같은 뜻으로 쓴다.
 *
 * dbg 로 두 번호를 찍는 것에 주의 -- 번호 변환이 어긋나면 엉뚱한 슬롯을
 * 보게 되므로, 그 계산을 추적할 수 있게 남긴 로그로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:
 *   cpqphp_core.c 의 get_latch_status → [이 함수] → readl
 */
static inline int cpq_get_latch_status(struct controller *ctrl,
				       struct slot *slot)
{
	/* [한국어] INT_INPUT_CLEAR 값에서 이 슬롯 비트만 남긴 결과를 담는다 */
	u32 status;
	/* [한국어] 컨트롤러 안 슬롯 번호 */
	u8 hp_slot;

	/* [한국어] 같은 번호 변환 */
	hp_slot = slot->device - ctrl->slot_device_offset;
	/* [한국어] **변환에 쓰인 두 값을 그대로 찍는다.**
	 * 번호 체계가 둘이라 어긋나기 쉬웠던 흔적으로 보인다 */
	dbg("%s: slot->device = %d, ctrl->slot_device_offset = %d\n",
	    __func__, slot->device, ctrl->slot_device_offset);

	/* [한국어] 인터럽트 상태 레지스터에서 이 슬롯의 레버 비트만 남긴다.
	 * **이 레지스터는 읽기로 상태를 알려 주고 쓰기로 지워지는 자리이며**,
	 * 여기서는 읽기만 하므로 지워지지 않는다 */
	status = (readl(ctrl->hpc_reg + INT_INPUT_CLEAR) & (0x01L << hp_slot));

	/* [한국어] **비트가 0 일 때 닫힘(1)을 돌려준다 -- 뜻이 뒤집혀 있다.**
	 * 하드웨어가 레버 열림을 1 로 알리기 때문이다 */
	return (status == 0) ? 1 : 0;
}


/* [한국어]
 * get_presence_status - 슬롯에 카드가 꽂혀 있는지 조회
 *
 * @ctrl: 대상 컨트롤러.
 * @slot: 사용자에게 보이는 슬롯.
 * @return: 카드가 있으면 2, 없으면 0.
 *
 * **이 헤더에서 비트 계산이 가장 까다로운 함수다.**
 *
 *   presence_save = (((~tempdword) >> 23) | ((~tempdword) >> 15)) >> hp_slot & 0x02
 *
 * 읽는 순서대로 보면:
 *   1) `~tempdword` -- **먼저 뒤집는다.** 이 하드웨어는 카드가 있을 때
 *      비트를 0 으로 두는 음논리이기 때문이다. cpqphp_ctrl.c 의
 *      handle_presence_change 가 `!(ctrl_int_comp & ...)` 로 같은 판단을
 *      하는 것과 대응한다.
 *   2) `>> 23` 과 `>> 15` -- **감지 핀 두 벌을 같은 자리로 모은다.**
 *      상위 16비트 안에 존재 감지가 두 벌 있는데(비트 24 쪽과 16 쪽),
 *      각각 23 과 15 만큼 밀면 둘 다 비트 1 자리로 온다.
 *   3) `|` -- **둘 중 하나라도 서 있으면 있는 것으로 본다.**
 *   4) `>> hp_slot` -- 원하는 슬롯 자리로 민다.
 *   5) `& 0x02` -- 비트 1 만 남긴다.
 *
 * 그래서 **반환값이 0 또는 2 이지 0/1 이 아니다.** 다른 조회 함수들이
 * 0/1 로 눌러 돌려주는 것과 다르며, sysfs 의 adapter 파일이 그 값을
 * 그대로 보여 준다.
 *
 * 같은 계산이 cpqphp_core.c 의 ctrl_slot_setup 에도 그대로 나온다 --
 * 슬롯을 등록하며 capabilities 에 존재 비트를 넣는 자리다.
 * **두 곳에 같은 식이 복사되어 있다.**
 *
 * **두 벌인 이유와 비트 배치의 근거 문서는 이 트리에 없다.** 코드가
 * 두 자리를 OR 로 합친다는 사실만 확인된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:
 *   cpqphp_core.c 의 get_adapter_status /
 *   cpqphp_ctrl.c 의 set_controller_speed → [이 함수] → readl
 */
static inline int get_presence_status(struct controller *ctrl,
				      struct slot *slot)
{
	/* [한국어] 결과를 담을 자리. **0 으로 초기화하지만 아래에서 통째로 덮어쓴다** */
	int presence_save = 0;
	/* [한국어] 컨트롤러 안 슬롯 번호 */
	u8 hp_slot;
	/* [한국어] INT_INPUT_CLEAR 원본 값을 담는다 */
	u32 tempdword;

	/* [한국어] 같은 번호 변환 */
	hp_slot = slot->device - ctrl->slot_device_offset;

	/* [한국어] 인터럽트 상태 레지스터를 통째로 읽는다.
	 * **감지선 신호가 두 자리에 나뉘어 실려 있다** -- 아래 식이 그 둘을 합친다 */
	tempdword = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);
	/* [한국어] **이 파일에서 가장 읽기 어려운 한 줄이다.**
	 * 먼저 ~ 로 전체를 뒤집는다 -- 하드웨어가 "없음" 을 1 로 알리므로
	 *   뒤집어야 "있음" 이 1 이 된다.
	 * 그다음 23비트와 15비트 오른쪽 시프트 결과를 OR 한다 --
	 *   카드 감지선이 두 벌(PRSNT1, PRSNT2)이고 각각 다른 자리에 실리기
	 *   때문이다. PCI 규격은 카드 폭을 알리려고 감지선을 두 개 둔다.
	 * 마지막으로 `>> hp_slot` 으로 이 슬롯 자리를 맨 아래로 내리고
	 *   `& 0x02` 로 한 비트만 남긴다.
	 * **결과는 0 또는 2 이며 0/1 이 아니다** -- 호출자가 참/거짓으로만 쓰므로
	 *   문제가 되지 않는다.
	 * **23 과 15 라는 자리의 근거 문서는 이 트리에 없다** -- 식이 하는 일로만 적는다 */
	presence_save = (int) ((((~tempdword) >> 23) | ((~tempdword) >> 15))
				>> hp_slot) & 0x02;

	/* [한국어] 카드가 꽂혀 있으면 0 이 아닌 값을 돌려준다 */
	return presence_save;
}

/* [한국어]
 * wait_for_ctrl_irq - set_SOGO 로 내린 명령이 끝나기를 기다린다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 정상, 시그널을 받았으면 -EINTR.
 *
 * **set_SOGO 의 짝이며, 이 드라이버가 느린 근본 이유다.**
 *
 * 코드가 하는 일은 넷이다.
 *   1) ctrl->queue 대기 큐에 자기를 건다(add_wait_queue).
 *   2) **msleep_interruptible(1000) 을 부른다.**
 *   3) 큐에서 빠진다(remove_wait_queue).
 *   4) 시그널이 왔었으면 -EINTR 을 돌려준다.
 *
 * 한편 cpqphp_ctrl.c 의 cpqhp_ctrl_intr 은 Serial Output 완료
 * 인터럽트를 받으면 wake_up_interruptible(&ctrl->queue) 로 이 큐를
 * 깨운다. 대기 큐 등록과 깨움이 짝을 이루고 있는 것은 분명하다.
 *
 * **다만 그 깨움이 이 1초를 실제로 줄여 주는지는 이 트리에서 확인할 수
 * 없다.** msleep_interruptible 의 구현이 이 스파스 체크아웃에 없기
 * 때문이다(kernel/ 부재). 코드가 말하는 사실은 "대기 큐에 등록만 하고
 * 무조건 1초를 잔다" 까지이며, 그 너머는 확인 대상 밖이다.
 * wait_event 계열이 아니라 조건 검사 없이 msleep 을 부르는 형태라는
 * 점도 코드에서 그대로 읽힌다.
 *
 * 실용적 결과는 뚜렷하다. cpqphp_ctrl.c 와 cpqphp_core.c 가 LED 하나
 * 바꿀 때마다 이 대기를 하므로 카드 하나를 켜고 끄는 데 초 단위가
 * 걸리고, cpqhp_hardware_test 의 LED 시험은 수십 초가 걸린다.
 *
 * **반환값을 검사하는 호출자가 없다.** 이 드라이버 전체에서 이 함수를
 * 부르는 스무 곳 남짓이 모두 값을 버리므로, -EINTR 은 사실상 쓰이지 않는다.
 *
 * 앞뒤로 dbg 를 찍는다 -- 어디서 멈췄는지 추적하려는 것으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. **잠들므로 인터럽트 컨텍스트에서
 * 부르면 안 된다.** 호출자가 crit_sect 뮤텍스를 쥔 채 부르는 경우가
 * 많아, 그 구간 전체가 초 단위로 길어진다.
 *
 * 호출 체인:
 *   set_SOGO 를 부른 거의 모든 함수 → [이 함수]
 *     → add_wait_queue, msleep_interruptible, remove_wait_queue
 */
static inline int wait_for_ctrl_irq(struct controller *ctrl)
{
	/* [한국어] 현재 태스크를 가리키는 대기 항목을 스택에 만든다.
	 * **커널 매크로이며 정의는 include/linux/wait.h 에 있으나
	 * 이 스파스 체크아웃에는 없다** -- 쓰임으로만 설명한다 */
	DECLARE_WAITQUEUE(wait, current);
	/* [한국어] 반환값. 신호로 깨어난 경우에만 바뀐다 */
	int retval = 0;

	/* [한국어] 들어온 것을 기록한다. 이 함수는 자주 불리므로 로그가 짝으로 남는다 */
	dbg("%s - start\n", __func__);
	/* [한국어] **컨트롤러의 대기 큐에 자신을 올린다.**
	 * cpqhp_ctrl_intr 이 wake_up_interruptible 로 이 큐를 깨운다 */
	add_wait_queue(&ctrl->queue, &wait);
	/* Sleep for up to 1 second to wait for the LED to change. */
	/* [한국어] **대기 큐에 올려 두고 무조건 1초를 잔다.**
	 * 조건 변수를 확인하는 wait_event 계열이 아니라 정해진 시간을 자는
	 * 형태다. 그래서 인터럽트가 일찍 깨워도 이 잠이 실제로 줄어드는지는
	 * **msleep_interruptible 의 정의가 이 트리에 없어 확인할 수 없다.**
	 * 원문 주석은 LED 가 바뀌기를 최대 1초 기다린다고 밝힌다.
	 * **지금 기준으로는 완료 조건을 확인하지 않는 대기이며**,
	 * 이후 세대인 shpchp 와 pciehp 는 완료 플래그를 두고
	 * wait_event_timeout 계열로 기다린다 */
	msleep_interruptible(1000);
	/* [한국어] 큐에서 자신을 뺀다. 스택에 있던 항목이므로 반드시 빼야 한다 */
	remove_wait_queue(&ctrl->queue, &wait);
	/* [한국어] **자는 동안 신호가 왔는지 본다.**
	 * sysfs 쓰기로 들어온 경로라면 사용자가 Ctrl-C 를 눌렀을 수 있다 */
	if (signal_pending(current))
		/* [한국어] 신호로 중단되었음을 알린다.
		 * **호출자 대부분이 이 반환값을 확인하지 않는다** --
		 * 확인 없이 다음 명령으로 넘어가는 자리가 여럿이다 */
		retval =  -EINTR;

	/* [한국어] 나가는 것을 기록한다 */
	dbg("%s - end\n", __func__);
	/* [한국어] 0 이면 정상, -EINTR 이면 신호로 깨어난 것이다 */
	return retval;
}

#include <asm/pci_x86.h>
/* [한국어]
 * cpqhp_routing_table_length - BIOS IRQ 라우팅 표의 항목 수를 계산한다
 *
 * @return: 표에 든 struct irq_info 항목의 개수.
 *
 * 전역 cpqhp_routing_table 이 가리키는 표의 전체 크기에서 헤더 크기를
 * 빼고 항목 하나의 크기로 나눈다. 가변 길이 표의 항목 수를 구하는
 * 표준적인 계산이다.
 *
 * **BUG_ON 으로 NULL 을 막는 것에 주의.** 표가 없으면 커널을 멈춘다 --
 * 지금 기준으로는 과한 대응이며, 오류를 돌려주거나 WARN_ON 을 쓸
 * 자리다. 다만 이 함수를 부르는 곳들이 모두 표가 이미 준비된 뒤이므로
 * 실제로 걸릴 일은 없다 -- init_cpqhp_routing_table 이 성공한 뒤에만
 * 쓰인다.
 *
 * 바로 위의 `#include <asm/pci_x86.h>` 가 이 함수 때문에 있다.
 * struct irq_routing_table 과 struct irq_info 의 크기를 알아야 하기
 * 때문인데, **헤더 맨 위가 아니라 파일 끝 근처에 include 를 둔 것이
 * 지금 기준으로는 낯선 배치다.**
 *
 * **그 헤더가 이 스파스 체크아웃에 없어**(arch/ 와 include/asm 부재)
 * 두 구조체의 정의를 확인하지 못했다. 계산 방식으로만 설명한다.
 *
 * 쓰이는 곳은 셋이다 -- init_cpqhp_routing_table 이 표가 비었는지
 * 확인할 때, pci_print_IRQ_route 와 get_slot_mapping 이 표를 순회할 때,
 * 그리고 cpqphp_pci.c 의 PCI_GetBusDevHelper 가 슬롯을 찾을 때다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   init_cpqhp_routing_table / pci_print_IRQ_route / get_slot_mapping /
 *   PCI_GetBusDevHelper → [이 함수]
 */
static inline int cpqhp_routing_table_length(void)
{
	/* [한국어] **표가 없으면 커널을 멈춘다.**
	 * 이 함수를 부르는 곳은 이미 표가 있다고 전제하므로,
	 * NULL 이면 그 전제가 깨진 것이라 계속 진행할 수 없다는 판단이다.
	 * **지금 커널은 BUG_ON 을 이렇게 쓰는 것을 권하지 않는다** --
	 * 복구 가능한 상황이면 오류를 돌려주는 쪽을 택한다 */
	BUG_ON(cpqhp_routing_table == NULL);
	/* [한국어] **$PIR 표에 든 항목 개수를 계산한다.**
	 * 표 전체 크기에서 고정 헤더(struct irq_routing_table) 크기를 빼면
	 * 항목들이 차지한 바이트가 남고, 그것을 항목 하나의 크기로 나눈다.
	 * **가변 길이 배열이 구조체 끝에 붙어 있는 형태의 전형적인 계산이다.**
	 * 읽는 자: get_slot_mapping 과 cpqhp_set_irq 가 이 값만큼 항목을 훑는다 */
	return ((cpqhp_routing_table->size - sizeof(struct irq_routing_table)) /
		sizeof(struct irq_info));
}

#endif
