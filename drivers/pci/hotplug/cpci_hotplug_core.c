// SPDX-License-Identifier: GPL-2.0+
/*
 * CompactPCI Hot Plug Driver
 *
 * Copyright (C) 2002,2005 SOMA Networks, Inc.
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/*
 * [한국어 설명] CompactPCI 핫플러그 공용 코어 (cpci_hotplug_core.c)
 *
 * === 파일의 역할 ===
 * CompactPCI(cPCI) 새시에서 보드를 살아 있는 채로 꽂고 뽑는 절차를 구현한다.
 * PICMG 2.1 규격이 정한 상태 기계가 이 파일의 전부라고 해도 된다.
 * 담당하는 것은 넷이다. (1) 슬롯 등록 — 보드별 드라이버가 알려 준 슬롯 번호
 * 범위를 받아 슬롯마다 struct slot 을 만들고 PCI 핫플러그 공용 코어에 등록해
 * sysfs 에 노출한다. (2) 컨트롤러 등록 — 보드별 드라이버가 채운 콜백 표를 받아
 * 두고, #ENUM 신호를 인터럽트로 받을지 폴링으로 볼지 정한다. (3) 감시 스레드 —
 * 이벤트가 오면 등록된 슬롯을 모두 훑어 각 슬롯의 HS(Hot Swap) CSR 을 읽고,
 * 삽입(INS)인지 추출 요청(EXT)인지 판별해 PCI 열거나 정리를 진행한다.
 * (4) sysfs 콜백 — 공용 코어가 요구하는 일곱 개의 슬롯 연산을 채운다.
 * 하드웨어 레지스터는 이 파일이 직접 만지지 않는다. HS CSR 접근과 PCI 열거는
 * 옆 파일 cpci_hotplug_pci.c 가, #ENUM 신호 읽기는 보드별 드라이버가 맡는다.
 * 이 파일은 그 둘 사이에서 '언제 무엇을 할지' 만 결정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * cPCI 핫플러그는 세 층이다. 최상단은 sysfs 슬롯 인터페이스를 제공하는 공용
 * 코어 pci_hotplug_core.c, 중간이 이 파일과 cpci_hotplug_pci.c, 최하단이
 * 보드별 하드웨어 접근 계층(cpcihp_generic.c, cpcihp_zt5550.c)이다.
 * 이 파일은 두 방향에서 불린다. 아래에서 위로는 보드 드라이버가
 * cpci_hp_register_controller() → cpci_hp_register_bus() → cpci_hp_start() 순으로
 * 부르며 시동을 건다. 위에서 아래로는 사용자가 sysfs 를 건드릴 때 공용 코어가
 * 이 파일의 콜백 일곱 개를 되부른다.
 * 그리고 세 번째 진입점이 있다 — 이 파일이 직접 띄우는 감시 스레드다. 그것이
 * 이 파일의 중심이고, 사실상 모든 상태 변화가 그 스레드 안에서 일어난다.
 * 초기화는 모듈 단위가 아니라 함수 하나로 들어온다. Makefile 이 이 파일을
 * pci_hotplug 모듈에 함께 링크하고(drivers/pci/hotplug/Makefile:33~34),
 * pci_hotplug_core.c 의 초기화 함수가 cpci_hotplug_init() 을 부른다
 * (pci_hotplug_core.c:1338). 그래서 이 파일에는 module_init 도 MODULE_ 계열
 * 매크로도 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci_hotplug.h 의 struct hotplug_slot 과 pci_hp_register() /
 * pci_hp_deregister(). 슬롯 이름 문자열의 소유권도 그쪽에 있어, 이 파일은
 * slot_name() 으로 조회만 한다.
 * 옆쪽: cpci_hotplug_pci.c 가 HS CSR 접근(cpci_get_hs_csr,
 * cpci_check_and_clear_ins, cpci_check_ext, cpci_clear_ext), LED 제어
 * (cpci_led_on, cpci_led_off), attention 상태(cpci_get_attention_status,
 * cpci_set_attention_status), 그리고 실제 PCI 열거/제거(cpci_configure_slot,
 * cpci_unconfigure_slot)를 모두 구현한다. 이 파일은 그 함수들을 부르기만 한다.
 * 아래쪽: struct cpci_hp_controller_ops 의 네 콜백(query_enum, enable_irq,
 * disable_irq, check_irq)이 보드별 하드웨어와의 유일한 접점이다.
 * 공유 상태: 이 파일이 전역 슬롯 리스트와 그 읽기·쓰기 세마포어, 슬롯 개수,
 * 추출 진행 카운터, 컨트롤러 포인터, 스레드 포인터를 모두 소유한다. cpci_debug
 * 만 헤더로 공개해 cpci_hotplug_pci.c 가 자기 디버그 매크로에서 함께 쓴다.
 * 데이터 흐름: #ENUM 신호 → (인터럽트 또는 폴링) → 감시 스레드 →
 * check_slots() 가 슬롯마다 HS CSR 을 읽음 → INS 면 cpci_configure_slot() 으로
 * 장치를 열거하고 파란 LED 를 끔, EXT 면 추출 중으로 표시하고 사용자 응답을
 * 기다림 → 사용자가 sysfs 로 disable 하면 cpci_unconfigure_slot() 후 LED 를 켬.
 *
 * === 주요 함수/구조체 요약 ===
 * - check_slots(): 이 파일의 심장. 슬롯을 모두 훑어 삽입·추출을 판별하고
 *   그에 맞는 처리를 한다. 반환값 규약이 특이하다 — 양수는 추출 요청 개수,
 *   0 은 처리할 것이 없지만 추출이 진행 중, 음수는 더 볼 수 없으니 멈추라는 뜻이다.
 * - event_thread() / poll_thread(): 감시 스레드의 두 가지 몸통. 전자는 잠들어
 *   있다가 인터럽트가 깨우면 돌고, 후자는 100ms 마다 스스로 #ENUM 을 물어본다.
 *   둘 다 안쪽에 '추출이 끝날 때까지 반복' 하는 루프를 갖는다.
 * - cpci_hp_intr(): 인터럽트 방식일 때의 1차 핸들러. 인터럽트를 끄고 스레드를
 *   깨우기만 한다 — 실제 처리는 전부 스레드가 한다.
 * - cpci_hp_register_bus() / cpci_hp_unregister_bus(): 슬롯 범위를 등록·해제한다.
 * - cpci_hp_register_controller() / cpci_hp_unregister_controller(): 보드 드라이버의
 *   콜백 표를 받아 두고 인터럽트를 잡는다. 컨트롤러는 시스템에 하나뿐이다.
 * - cpci_hp_start() / cpci_hp_stop(): 감시를 켜고 끈다.
 * - disable_slot(): sysfs 로 들어오는 유일한 '진짜 동작' 콜백. 장치를 떼어 내고
 *   EXT 를 지우고 파란 LED 를 켜 "뽑아도 좋다" 를 사용자에게 알린다.
 *
 * === 이 파일의 상태 기계 ===
 * cPCI 의 삽입과 추출은 대칭이 아니다. 삽입은 소프트웨어가 끝까지 처리하지만,
 * 추출은 사람이 손잡이를 여는 것으로 '요청' 이 시작되고 소프트웨어가 정리를
 * 마친 뒤 파란 LED 로 답해 주어야 비로소 뽑을 수 있다.
 *  - 삽입: 보드가 꽂히면 HS CSR 의 INS 비트가 선다 → 스레드가 그것을 보고
 *    지운 뒤 cpci_configure_slot() 으로 PCI 열거 → 상태를 '꽂힘' 으로 갱신하고
 *    파란 LED 를 끈다(= 사용 중).
 *  - 추출: 사람이 이젝터를 열면 EXT 비트가 선다 → 스레드가 그것을 보고 슬롯을
 *    '추출 중' 으로 표시하고 전역 카운터를 올린다 → 여기서 멈춘다. 실제 제거는
 *    사용자 공간이 sysfs 로 disable 을 걸어야 일어나고, 그때 disable_slot() 이
 *    장치를 떼고 EXT 를 지우고 파란 LED 를 켠다(= 뽑아도 안전).
 *  - 그래서 스레드는 추출이 진행 중인 동안 계속 돌면서 사용자 응답을 기다리고,
 *    그 사이 500ms 씩 잠들어 사용자 공간에 처리할 틈을 준다.
 *  - 예외 처리도 하나 있다. 추출 중인 슬롯의 HS CSR 이 통째로 0xffff 로 읽히면
 *    사용자가 절차를 무시하고 보드를 그냥 뽑았다는 뜻이라, 오류를 남기고 상태만
 *    되돌린다.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있다.
 * 따라서 kthread 의 정확한 수명 규약(kernel/kthread.c), 세마포어와 atomic 의
 * 구현, PICMG 2.1 규격 문서 자체는 확인 대상 밖이다. 아래 주석에서 그 바깥을
 * 단정한 곳은 없다. NVMe 와의 접점도 없다 — CompactPCI 는 PCIe 이전 세대의
 * 새시 규격이고, 이 파일에는 장치 종류에 관한 코드가 전혀 없다.
 */

/* [한국어] 이 파일은 module.h 를 포함하지만 MODULE_ 계열 매크로도 모듈 진입점도
 * 갖지 않는다. Makefile 이 이 파일을 pci_hotplug 모듈에 함께 링크하기 때문이며
 * (drivers/pci/hotplug/Makefile:33~34), 모듈 메타데이터는 pci_hotplug_core.c 가
 * 갖는다. 즉 이 include 가 이 파일에서 무엇을 위해 필요한지는 코드만으로
 * 특정되지 않는다. */
#include <linux/module.h>
/* [한국어] 범용 커널 매크로와 printk 계열 선언을 위해 포함한다. 아래 로그
 * 매크로들이 printk 를 직접 부른다. */
#include <linux/kernel.h>
/* [한국어] signal_pending() 을 위해 포함한다. 폴링 스레드가 시그널을 받으면
 * 스스로 빠져나가는 데 쓴다. */
#include <linux/sched/signal.h>
/* [한국어] kzalloc_obj() 와 kfree() 를 위해 포함한다. 슬롯 구조체를 슬롯마다
 * 하나씩 할당한다. */
#include <linux/slab.h>
/* [한국어] struct pci_bus / pci_dev, pci_get_slot(), PCI_DEVFN() 을 위해
 * 포함한다. 슬롯 번호에서 devfn 을 만들고 그 자리에 장치가 있는지 조회한다. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 과 pci_hp_register() / pci_hp_deregister() 를
 * 위해 포함한다. PCI 핫플러그 공용 코어와의 접점이 이 헤더다. */
#include <linux/pci_hotplug.h>
/* [한국어] __init 표시를 위해 포함한다. 이 파일에서는 cpci_hotplug_init() 하나에만
 * 붙는다 — 부팅이 끝나면 그 코드를 버릴 수 있다는 표시다. */
#include <linux/init.h>
/* [한국어] request_irq() / free_irq(), irqreturn_t, IRQF_SHARED 를 위해 포함한다.
 * 인터럽트 방식일 때 #ENUM 신호를 이 API 로 잡는다. */
#include <linux/interrupt.h>
/* [한국어] atomic_t 와 atomic_inc/dec/read 를 위해 포함한다. 추출 진행 중인 슬롯
 * 개수를 세는 전역 카운터가 원자 변수인데, 그 값을 스레드와 sysfs 콜백이
 * 서로 다른 락 아래에서 만지기 때문이다. */
#include <linux/atomic.h>
/* [한국어] msleep() 을 위해 포함한다. 폴링 주기(100ms)와 사용자 공간에 틈을
 * 주는 대기(500ms)에 쓴다. */
#include <linux/delay.h>
/* [한국어] kthread_run() / kthread_stop() / kthread_should_stop() 을 위해
 * 포함한다. 이 파일의 감시 스레드가 커널 스레드다. */
#include <linux/kthread.h>
/* [한국어] 이 드라이버 묶음의 공용 헤더. struct slot, struct cpci_hp_controller,
 * HS CSR 비트 정의, to_slot() / slot_name(), 그리고 cpci_hotplug_pci.c 가
 * 구현하는 함수들의 원형이 모두 여기 있다. 상대 경로 따옴표인 것은 커널 전역
 * 헤더가 아니라 이 디렉터리 안에서만 쓰이기 때문이다. */
#include "cpci_hotplug.h"

/* [한국어] 작성자 문자열. [상류 코드 관찰] 이 파일에서 참조하지 않는다 —
 * MODULE_AUTHOR 를 부르는 곳이 없기 때문이며, 그것은 이 파일이 독립 모듈이
 * 아니라 pci_hotplug 모듈의 일부이기 때문이다. */
#define DRIVER_AUTHOR	"Scott Murray <scottm@somanetworks.com>"
/* [한국어] 드라이버 설명 문자열. 위와 같은 이유로 참조되지 않는다. */
#define DRIVER_DESC	"CompactPCI Hot Plug Core"

/* [한국어] 아래 로그 매크로들이 모든 메시지 앞에 붙일 이름표. request_irq 의
 * 이름 인자로도 쓰여 /proc/interrupts 에 이 이름이 나타난다. */
#define MY_NAME	"cpci_hotplug"

/* [한국어] 디버그 로그 매크로. 전역 cpci_debug 가 켜졌을 때만 실제로 찍는다.
 * do-while(0) 로 감싼 것은 if 문 뒤에 세미콜론과 함께 놓여도 문법이 깨지지
 * 않게 하는 표준 관용이다. ## arg 는 가변 인자가 비었을 때 앞의 쉼표까지
 * 지워 주는 GNU 확장으로, dbg("문자열") 처럼 인자 없이 부를 수 있게 해 준다.
 * 줄 끝 백슬래시로 이어지는 매크로라 그 안쪽 줄에는 주석을 붙일 수 없어
 * 여기 한 번에 설명한다: 조건 검사 → 커널 로그 레벨 KERN_DEBUG 로 이름표와
 * 함께 출력 → 줄바꿈은 매크로가 붙여 주므로 호출부에서 쓰지 않는다.
 * 이 파일은 로그가 매우 촘촘한데, 핫플러그가 사람의 손동작에 반응하는 기능이라
 * 재현이 어렵고 사후 추적에 의존하기 때문이다. */
#define dbg(format, arg...)					\
	do {							\
		if (cpci_debug)					\
			printk(KERN_DEBUG "%s: " format "\n",	\
				MY_NAME, ## arg);		\
	} while (0)
/* [한국어] 오류 로그. 조건 없이 언제나 찍는다 — 디버그와 달리 놓치면 안 되는
 * 정보이기 때문이다. */
#define err(format, arg...) printk(KERN_ERR "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 정보 로그. [상류 코드 관찰] 정의만 있고 이 파일에서 참조하지 않는다. */
#define info(format, arg...) printk(KERN_INFO "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 경고 로그. 이 파일에서는 '이미 꽂혀 있다고 아는 슬롯에 또 삽입
 * 신호가 왔다' 는 한 곳에서만 쓴다. */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format "\n", MY_NAME, ## arg)

/* local variables */
/* [한국어] 아래 전역들이 이 파일의 공유 상태 전부다(옆의 상류 주석).
 * 전역 슬롯 리스트를 지키는 읽기·쓰기 세마포어. 읽기 쪽은 여러 문맥이 동시에
 * 들어갈 수 있고 쓰기 쪽은 배타적이다. 순회만 하는 곳(init_slots, check_slots,
 * cpci_hp_start)은 읽기로, 목록을 바꾸는 곳(등록·해제·정리)과 슬롯 상태를
 * 바꾸는 disable_slot 은 쓰기로 잡는다. 세마포어라 잠들 수 있으므로 인터럽트
 * 문맥에서는 잡을 수 없고, 실제로 인터럽트 핸들러는 이 락을 건드리지 않는다. */
static DECLARE_RWSEM(list_rwsem);
/* [한국어] 등록된 모든 슬롯의 리스트 머리. 버스를 가리지 않고 한 줄로 이어
 * 두므로, 특정 버스의 슬롯만 걷어낼 때는 순회하며 골라내야 한다.
 * 설정자·읽는 자: 이 파일 전체. 동기화: 위 세마포어. */
static LIST_HEAD(slot_list);
/* [한국어] 등록된 슬롯 개수. 리스트가 비었는지 빠르게 판정하는 데 쓰인다.
 * 설정자: cpci_hp_register_bus() 가 늘리고 cpci_hp_unregister_bus() 가 줄인다.
 * 읽는 자: init_slots(), check_slots(), cleanup_slots() 의 '슬롯 없음' 검사.
 * 값 범위: 0 이상.
 * 동기화: 위 세마포어 아래에서만 만진다.
 * [상류 코드 관찰] cleanup_slots() 는 리스트에서 슬롯을 지우면서 이 카운터를
 *   줄이지 않아, 정리 후에도 값이 남는다. 다만 그 뒤 컨트롤러가 NULL 이 되어
 *   다시 시작할 수 없으므로 실제로 읽히지는 않는다. */
static int slots;
/* [한국어] 지금 추출 절차가 진행 중인 슬롯의 개수.
 * 설정자: check_slots() 가 EXT 를 처음 보면 올리고, disable_slot() 이나
 *   '보드가 그냥 뽑힘' 판정에서 내린다.
 * 읽는 자: 두 스레드의 안쪽 루프 조건 — 이 값이 0 이 아니면 사용자 응답을
 *   기다리며 계속 돈다. check_slots() 의 '#ENUM 원인을 못 찾겠다' 판정도 본다.
 * 값 범위: 0 이상.
 * 동기화: 원자 변수인 이유가 여기 있다 — 올리는 쪽은 리스트 읽기 락 아래이고
 *   내리는 쪽은 쓰기 락 아래여서, 읽기 락만으로는 배타가 보장되지 않는다.
 *   [상류 코드 관찰] 초기화 코드가 없다. 전역이라 0 으로 시작하므로 결과는
 *   맞지만 atomic_set 을 부르는 곳은 없다. */
static atomic_t extracting;
/* [한국어] 디버그 로그를 켤지. static 이 아닌 것은 옆 파일 cpci_hotplug_pci.c 가
 * 자기 디버그 매크로에서 같은 변수를 보기 때문이다(헤더에 extern 선언이 있다).
 * 설정자: cpci_hotplug_init() 이 공용 코어의 모듈 파라미터 값을 받아 넣는다.
 * 읽는 자: 이 파일과 cpci_hotplug_pci.c 의 dbg 매크로.
 * 값 범위: 0 또는 0 이 아닌 값.
 * 동기화: 없다. 진단용이라 경쟁이 문제가 되지 않는다. */
int cpci_debug;
/* [한국어] 등록된 보드별 컨트롤러. 시스템에 하나뿐이라는 것이 이 드라이버의
 * 전제이고, 그래서 포인터가 배열이 아니라 단일 변수다.
 * 설정자: cpci_hp_register_controller() 가 채우고 unregister 가 NULL 로 되돌린다.
 * 읽는 자: 인터럽트 핸들러, 두 스레드, 시작·정지 함수.
 * 값 범위: 유효 포인터 또는 NULL(등록 전/해제 후).
 * 동기화: 없다. 등록이 한 번뿐이고 그 뒤로는 읽기만 하는 구조에 기대고 있다. */
static struct cpci_hp_controller *controller;
/* [한국어] 감시 스레드의 태스크 구조체.
 * 설정자: cpci_start_thread() 가 kthread_run 의 결과를 담는다.
 * 읽는 자: 인터럽트 핸들러가 wake_up_process 로 깨울 때, cpci_stop_thread() 가
 *   멈출 때.
 * 값 범위: 유효 포인터, 또는 시작 실패 시 ERR_PTR.
 * 동기화: 없다.
 * [상류 코드 관찰] 시작 전에는 NULL 인데, 아래 thread_finished 설명에 적은
 *   경로로 그 상태에서 kthread_stop 이 불릴 수 있다. */
static struct task_struct *cpci_thread;
/* [한국어] 스레드가 이미 끝났는지 표시하는 플래그. 두 번 멈추려 하는 것을 막는다.
 * 설정자: cpci_start_thread() 가 0 으로 두고, 스레드가 오류로 빠져나갈 때와
 *   cpci_stop_thread() 가 1 로 만든다.
 * 읽는 자: cpci_hp_unregister_controller() 의 '아직 안 끝났으면 멈춰라' 검사.
 * 값 범위: 0 또는 1.
 * 동기화: 없다.
 * [상류 코드 관찰] 전역이라 0 으로 시작한다. 그래서 컨트롤러만 등록하고
 *   cpci_hp_start() 를 한 번도 부르지 않은 채 해제하면, 이 값이 0 이므로
 *   cpci_stop_thread() 가 불리고 그 안에서 아직 NULL 인 스레드 포인터로
 *   kthread_stop 이 호출된다. */
static int thread_finished;

/* [한국어] 아래 일곱 개는 전방 선언이다. 바로 다음에 오는 연산 표가 이 함수들을
 * 이름으로 참조하는데 정의는 그보다 아래에 있으므로, 컴파일러에게 먼저
 * 알려 두어야 한다. 표를 함수들 뒤로 옮기면 없앨 수 있는 선언이지만, 표를
 * 파일 앞에 두어 '이 드라이버가 공용 코어에 무엇을 제공하는가' 를 먼저 보이게
 * 하는 편을 택한 것으로 읽힌다.
 * enable_slot — 사용자가 sysfs 로 슬롯을 켤 때. */
static int enable_slot(struct hotplug_slot *slot);
/* [한국어] disable_slot — 사용자가 sysfs 로 슬롯을 끌 때. 이 파일에서 실제 동작이
 * 있는 유일한 sysfs 콜백이다. */
static int disable_slot(struct hotplug_slot *slot);
/* [한국어] set_attention_status — attention LED 를 켜고 끈다. */
static int set_attention_status(struct hotplug_slot *slot, u8 value);
/* [한국어] get_power_status — 슬롯 전원 상태를 묻는다. */
static int get_power_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] get_attention_status — attention LED 상태를 묻는다. */
static int get_attention_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] get_adapter_status — 보드가 꽂혀 있는지 묻는다. */
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] get_latch_status — 이젝터 래치가 닫혀 있는지 묻는다. */
static int get_latch_status(struct hotplug_slot *slot, u8 *value);

/* [한국어] PCI 핫플러그 공용 코어에 넘길 연산 표. 사용자가 슬롯의 sysfs 파일을
 * 읽거나 쓰면 공용 코어가 이 표의 함수를 되부른다. 슬롯마다 이 같은 표를
 * 가리키므로, 콜백은 받은 hotplug_slot 포인터에서 to_slot() 으로 자기 슬롯을
 * 되찾아야 한다. */
static const struct hotplug_slot_ops cpci_hotplug_slot_ops = {
	/* [한국어] 슬롯 켜기. */
	.enable_slot = enable_slot,
	/* [한국어] 슬롯 끄기 — 추출 절차의 후반부를 여기서 수행한다. */
	.disable_slot = disable_slot,
	/* [한국어] attention LED 설정. 실제 레지스터 조작은 옆 파일이 한다. */
	.set_attention_status = set_attention_status,
	/* [한국어] 전원 상태 조회. */
	.get_power_status = get_power_status,
	/* [한국어] attention LED 상태 조회. */
	.get_attention_status = get_attention_status,
	/* [한국어] 어댑터(보드) 유무 조회. */
	.get_adapter_status = get_adapter_status,
	/* [한국어] 래치 상태 조회. */
	.get_latch_status = get_latch_status,
};

/* [한국어]
 * enable_slot - 사용자가 슬롯을 켰을 때 불리는 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @return: 언제나 0(성공).
 *
 * [상류 코드 관찰] 로그만 남기고 아무것도 하지 않는다. cPCI 에서 삽입은
 * 사용자의 sysfs 조작이 아니라 하드웨어 이벤트(HS CSR 의 INS 비트)로 시작되고,
 * 그 처리를 감시 스레드가 전부 끝내기 때문이다. 즉 이 콜백이 할 일이 남아 있지
 * 않다. 그래도 표에 채워 두는 것은 공용 코어가 이 연산을 요구하기 때문이며,
 * 삽입과 추출이 대칭이 아니라는 이 규격의 성질이 여기서 드러난다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [enable_slot]
 */
static int
enable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 공용 코어가 준 포인터에서 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청이었는지만 남긴다. */
	dbg("%s - physical_slot = %s", __func__, slot_name(slot));

	/* [한국어] 할 일이 없으므로 성공을 알린다. */
	return 0;
}

/* [한국어]
 * disable_slot - 사용자가 슬롯을 껐을 때 추출 절차의 후반부를 수행한다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @return: 0 성공, 음수 실패(장치 제거 실패 또는 EXT 지우기 실패).
 *
 * cPCI 추출은 두 단계로 나뉜다. 사람이 이젝터 손잡이를 열면 HS CSR 의 EXT
 * 비트가 서고, 감시 스레드가 그것을 보고 슬롯을 '추출 중' 으로 표시한 뒤
 * 멈춘다 — 거기까지가 전반부다. 후반부가 이 함수이며, 사용자 공간이 정리를
 * 마치고 sysfs 로 disable 을 걸어야 비로소 시작된다.
 * 여기서 하는 일은 셋이다. (1) PCI 장치를 커널에서 떼어 낸다. (2) HS CSR 의
 * EXT 비트를 지워 하드웨어에 처리 완료를 알린다 — 이 규격에서는 그 비트에
 * 1 을 써서 지운다(옆의 상류 주석이 "by setting it" 이라고 밝힌다).
 * (3) 파란 LED 를 켠다. cPCI 에서 파란 LED 는 "이 보드를 뽑아도 안전하다" 는
 * 신호이므로, 이것이 사람에게 보내는 최종 응답이다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). 리스트 쓰기 락을
 * 잡으므로 잠들 수 있어야 한다.
 * 에러 경로: 어느 단계가 실패하든 같은 라벨로 모여 락을 풀고 오류를 돌려준다.
 * 실패했을 때 이미 한 일을 되돌리지는 않는다.
 *
 * [상류 코드 관찰] 슬롯 하나만 만지는데 전역 리스트의 '쓰기' 락을 잡는다.
 * 그래야 하는 이유는 아래 추출 카운터를 내리는 부분에 있는 것으로 읽힌다 —
 * 감시 스레드가 읽기 락 아래에서 그 카운터를 올리므로, 내리는 쪽이 쓰기 락을
 * 잡아야 두 동작이 겹치지 않는다. 다만 그 의도를 밝힌 주석은 코드에 없다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [disable_slot]
 *     → cpci_unconfigure_slot(), cpci_clear_ext(), cpci_led_on()
 */
static int
disable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 공용 코어가 준 포인터에서 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 각 단계의 결과. 성공 경로에서도 이 값이 그대로 반환된다. */
	int retval = 0;

	/* [한국어] 어느 슬롯인지 남긴다. */
	dbg("%s - physical_slot = %s", __func__, slot_name(slot));

	/* [한국어] 슬롯 상태와 추출 카운터를 바꾸므로 쓰기 락을 잡는다(위 관찰 참조). */
	down_write(&list_rwsem);

	/* Unconfigure device */
	/* [한국어] 옆의 상류 주석대로 장치를 떼어 내기 시작한다는 로그. */
	dbg("%s - unconfiguring slot %s", __func__, slot_name(slot));
	/* [한국어] PCI 장치를 커널에서 제거한다. 실제 제거는 옆 파일이 한다. */
	retval = cpci_unconfigure_slot(slot);
	/* [한국어] 제거에 실패하면 EXT 를 지워서는 안 된다 — 그러면 아직 장치가 살아
	 * 있는데 사람에게 뽑아도 된다고 알리는 셈이 되기 때문이다. */
	if (retval) {
		/* [한국어] 어느 슬롯에서 실패했는지 남기고 */
		err("%s - could not unconfigure slot %s",
		    __func__, slot_name(slot));
		/* [한국어] 락을 푸는 공통 출구로 간다. */
		goto disable_error;
	}
	/* [한국어] 제거가 끝났다는 로그. */
	dbg("%s - finished unconfiguring slot %s", __func__, slot_name(slot));

	/* Clear EXT (by setting it) */
	/* [한국어] 옆의 상류 주석대로 EXT 비트를 지운다 — 이 규격에서는 그 비트에 1 을
	 * 써서 지우므로 주석이 "by setting it" 이라고 적혀 있다. 이것이 하드웨어에
	 * '추출 요청을 처리했다' 고 알리는 신호다. */
	if (cpci_clear_ext(slot)) {
		/* [한국어] 지우지 못했으면 하드웨어가 계속 추출 요청을 올릴 것이므로 */
		err("%s - could not clear EXT for slot %s",
		    __func__, slot_name(slot));
		/* [한국어] 장치가 없다는 뜻의 오류로 바꿔 담고 */
		retval = -ENODEV;
		/* [한국어] 공통 출구로 간다. */
		goto disable_error;
	}
	/* [한국어] 파란 LED 를 켠다. cPCI 에서 이 LED 는 "뽑아도 안전" 을 뜻하므로,
	 * 이 한 줄이 사람에게 보내는 최종 응답이다. */
	cpci_led_on(slot);

	/* [한국어] 이제 이 슬롯에는 보드가 없는 것으로 기록한다. sysfs 의 adapter 상태가
	 * 이 값을 그대로 보여 준다. */
	slot->adapter_status = 0;

	/* [한국어] 이 슬롯이 추출 절차 중이었다면 */
	if (slot->extracting) {
		/* [한국어] 그 표시를 지우고 */
		slot->extracting = 0;
		/* [한국어] 전역 추출 카운터도 내린다. 이 값이 0 이 되어야 감시 스레드의 안쪽
		 * 루프가 끝나고 스레드가 다시 잠들 수 있다 — 이 한 줄이 스레드를 풀어 주는
		 * 열쇠다. 원자 연산인 이유는 올리는 쪽이 읽기 락 아래에 있기 때문이다. */
		atomic_dec(&extracting);
	}
/* [한국어] 성공과 실패가 함께 모이는 출구. 라벨 이름이 error 지만 성공 경로도
 * 그대로 흘러 들어온다. */
disable_error:
	/* [한국어] 락을 푼다. */
	up_write(&list_rwsem);
	/* [한국어] 0 이면 성공, 음수면 실패로 사용자에게 전달된다. */
	return retval;
}

/* [한국어]
 * cpci_get_power_status - 슬롯 전원 상태를 돌려준다 (언제나 켜짐)
 *
 * @slot: 대상 슬롯. [상류 코드 관찰] 인자로 받지만 쓰지 않는다.
 * @return: 언제나 1.
 *
 * cPCI 새시에서 슬롯 전원은 소프트웨어가 켜고 끄는 대상이 아니라 백플레인이
 * 언제나 공급하는 것이다 — 보드를 꽂으면 바로 전원이 들어가고, 뽑을 때까지
 * 유지된다. 그래서 물어볼 하드웨어가 없고 상수를 돌려준다.
 * 이름이 이 파일 안의 sysfs 콜백 get_power_status 와 겹쳐 헷갈리기 쉬운데,
 * 그쪽이 이 함수를 부르는 얇은 껍데기다.
 * 실행 컨텍스트: sysfs 읽기(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → get_power_status() → [cpci_get_power_status]
 */
static u8
cpci_get_power_status(struct slot *slot)
{
	/* [한국어] 언제나 '전원 켜짐' 을 뜻하는 1 을 돌려준다. */
	return 1;
}

/* [한국어]
 * get_power_status - 전원 상태 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 언제나 0(성공).
 *
 * 공용 코어의 콜백 규약(포인터로 결과를 돌려주고 반환값은 성공/실패)에
 * 맞추기 위한 껍데기다. 실제 값은 위 함수가 정한다.
 * 실행 컨텍스트: sysfs 읽기(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_power_status]
 *     → to_slot(), cpci_get_power_status()
 */
static int
get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 상수 1 을 담는다. */
	*value = cpci_get_power_status(slot);
	/* [한국어] 조회는 실패할 수 없다. */
	return 0;
}

/* [한국어]
 * get_attention_status - attention LED 상태 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 언제나 0(성공).
 *
 * 전원 상태와 달리 이쪽은 실제 하드웨어를 읽는다 — 다만 그 읽기는 옆 파일
 * cpci_hotplug_pci.c 가 HS CSR 접근으로 수행하고, 이 함수는 값을 옮겨 담기만
 * 한다. 이 파일이 레지스터를 직접 만지지 않는다는 원칙이 여기서도 지켜진다.
 * 실행 컨텍스트: sysfs 읽기(프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 아래 함수가 실패를 알릴 방법이 없어(반환형이 u8) 언제나
 * 0 을 돌려준다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_attention_status]
 *     → to_slot(), cpci_get_attention_status()
 */
static int
get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 옆 파일이 하드웨어에서 읽어 온 값을 그대로 담는다. */
	*value = cpci_get_attention_status(slot);
	/* [한국어] 성공을 알린다. */
	return 0;
}

/* [한국어]
 * set_attention_status - attention LED 설정 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @status: 켤지 끌지.
 * @return: 옆 파일의 설정 결과를 그대로 전달한다.
 *
 * 조회 쪽과 달리 이쪽은 결과를 반환값으로 돌려줄 수 있어, 중간 변수 없이
 * 한 줄로 그대로 넘긴다. 이 파일의 sysfs 콜백 중 가장 얇다.
 * attention LED 는 관리자가 "이 슬롯을 보라" 고 표시하는 용도이며, 파란
 * LED(뽑아도 안전)와는 다른 물건이다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [set_attention_status]
 *     → to_slot(), cpci_set_attention_status()
 */
static int
set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	/* [한국어] 슬롯 객체로 바꿔 옆 파일에 그대로 넘기고 그 결과를 반환한다. */
	return cpci_set_attention_status(to_slot(hotplug_slot), status);
}

/* [한국어]
 * get_adapter_status - 보드가 꽂혀 있는지 알려 주는 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 언제나 0(성공).
 *
 * 하드웨어를 다시 읽지 않고 소프트웨어가 기억해 둔 상태를 그대로 돌려준다.
 * 그 값은 감시 스레드가 삽입을 처리할 때 1 로, disable_slot() 이 제거를 마칠 때
 * 0 으로 바꾼다. 즉 이 sysfs 파일은 '지금 전기적으로 무엇이 꽂혀 있는가' 가
 * 아니라 '커널이 이 슬롯을 사용 중으로 알고 있는가' 를 비춘다.
 * 실행 컨텍스트: sysfs 읽기(프로세스 컨텍스트). 락을 잡지 않는다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_adapter_status] → to_slot()
 */
static int
get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 기억해 둔 상태를 그대로 담는다. 락 없이 읽지만 1비트 값이라
	 * 찢어진 값을 볼 위험이 없고, 진단 목적의 조회라 순간적인 불일치도 문제되지 않는다. */
	*value = slot->adapter_status;
	/* [한국어] 성공을 알린다. */
	return 0;
}

/* [한국어]
 * get_latch_status - 이젝터 래치가 닫혀 있는지 알려 주는 sysfs 콜백
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 언제나 0(성공).
 *
 * 어댑터 상태와 마찬가지로 기억해 둔 값을 돌려준다. cPCI 에서 래치(이젝터
 * 손잡이)는 추출 절차의 출발 신호라, 감시 스레드가 EXT 를 볼 때 0 으로,
 * 삽입을 처리할 때 1 로 바꾼다.
 * 실행 컨텍스트: sysfs 읽기(프로세스 컨텍스트). 락을 잡지 않는다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_latch_status] → to_slot()
 */
static int
get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 기억해 둔 래치 상태를 담는다. */
	*value = slot->latch_status;
	/* [한국어] 성공을 알린다. */
	return 0;
}

/* [한국어]
 * release_slot - 슬롯 객체와 그것이 잡고 있던 장치 참조를 놓는다
 *
 * @slot: 해제할 슬롯.
 * @return: 없음.
 *
 * 슬롯을 리스트에서 떼고 공용 코어에서 등록 해제한 뒤 마지막에 부른다.
 * pci_dev_put 이 먼저인 이유는, 슬롯이 꽂힌 장치의 참조를 하나 들고 있기
 * 때문이다 — init_slots() 나 열거 경로가 pci_get_slot() 으로 올려 둔 것이다.
 * 구조체를 먼저 풀어 버리면 그 참조를 놓을 방법이 사라진다.
 * dev 가 NULL 이어도 안전하다(빈 슬롯인 경우).
 * 실행 컨텍스트: 등록 해제 경로(프로세스 컨텍스트). 호출자가 리스트 쓰기 락을
 * 쥔 상태에서 부른다.
 *
 * 호출 체인:
 *   cpci_hp_unregister_bus() / cleanup_slots() → [release_slot]
 *     → pci_dev_put(), kfree()
 */
static void release_slot(struct slot *slot)
{
	/* [한국어] 슬롯이 들고 있던 장치 참조를 놓는다. 빈 슬롯이면 NULL 이고 그래도
	 * 안전하다. 구조체를 풀기 전에 해야 한다. */
	pci_dev_put(slot->dev);
	/* [한국어] 슬롯 구조체를 반납한다. */
	kfree(slot);
}

/* [한국어] 슬롯 이름 버퍼의 크기. 아래에서 "버스번호:슬롯번호" 를 각각 두 자리
 * 16진수로 만들므로 "xx:xx" 다섯 글자에 널 종료 한 자리를 더해 6 이면 딱 맞는다.
 * 버스 번호가 0xff 를 넘을 수 없으므로 이 크기로 충분하다. */
#define SLOT_NAME_SIZE	6

/* [한국어]
 * cpci_hp_register_bus - 한 PCI 버스의 슬롯 범위를 등록해 sysfs 에 노출한다
 *
 * @bus: 슬롯들이 놓인 PCI 버스.
 * @first: 첫 슬롯 번호.
 * @last: 마지막 슬롯 번호(포함).
 * @return: 0 성공, -ENODEV(컨트롤러 미등록 또는 버스 없음), -ENOMEM,
 *          또는 공용 코어 등록 실패 코드.
 *
 * 보드별 드라이버가 컨트롤러를 등록한 뒤 부르는 두 번째 단계다. 새시에 슬롯이
 * 몇 번부터 몇 번까지 있는지는 보드마다 다르고 하드웨어로 알아낼 수도 없어,
 * 보드 드라이버가 알려 준 범위를 그대로 믿고 슬롯 객체를 만든다.
 * 슬롯 하나마다 (1) 구조체 할당, (2) 버스·번호·devfn 기록, (3) 이름 생성,
 * (4) 연산 표 연결, (5) 공용 코어 등록, (6) 전역 리스트 추가를 한다.
 * 컨트롤러가 먼저 등록되어 있어야 하는 이유: 슬롯만 있고 #ENUM 을 읽을 방법이
 * 없으면 아무 일도 일어나지 않기 때문이다.
 * 실행 컨텍스트: 보드 드라이버의 probe(프로세스 컨텍스트). 할당과 세마포어가
 * 있어 잠들 수 있다.
 *
 * [상류 코드 관찰] 두 가지. 첫째, 중간 슬롯에서 공용 코어 등록이 실패하면 그
 * 슬롯만 풀고 오류를 돌려주는데, 앞서 성공적으로 등록한 슬롯들은 그대로 남는다
 * — 되감기가 없다. 둘째, 리스트에 새 슬롯을 꼬리가 아니라 머리에 넣으므로
 * (list_add), 리스트 순서가 슬롯 번호의 역순이 된다. 이 파일의 순회는 순서에
 * 기대지 않아 결과에 영향은 없다.
 *
 * 호출 체인:
 *   보드별 드라이버(cpcihp_generic.c 등) → [cpci_hp_register_bus]
 *     → pci_hp_register()
 */
int
cpci_hp_register_bus(struct pci_bus *bus, u8 first, u8 last)
{
	/* [한국어] 이번에 만들 슬롯 객체. */
	struct slot *slot;
	/* [한국어] "버스:슬롯" 이름을 조립할 임시 버퍼. 공용 코어가 이 문자열을 복사해
	 * 가므로 지역 버퍼여도 안전하다. */
	char name[SLOT_NAME_SIZE];
	/* [한국어] 각 단계의 결과. */
	int status;
	/* [한국어] 슬롯 번호 순회 인덱스. */
	int i;

	/* [한국어] 컨트롤러가 먼저 등록되어 있어야 하고 버스도 있어야 한다.
	 * 컨트롤러가 없으면 #ENUM 을 읽을 방법이 없어 슬롯을 등록해도 아무 일도
	 * 일어나지 않기 때문이다. */
	if (!(controller && bus))
		/* [한국어] 둘 중 하나라도 없으면 등록을 거절한다. */
		return -ENODEV;

	/*
	 * Create a structure for each slot, and register that slot
	 * with the pci_hotplug subsystem.
	 */
	/* [한국어] 위 상류 주석대로 슬롯 하나씩 만들어 공용 코어에 등록한다.
	 * 범위가 닫힌 구간이라 last 도 포함한다. */
	for (i = first; i <= last; ++i) {
		/* [한국어] 슬롯 구조체를 0 으로 채워 할당한다. 0 초기화 덕분에 래치·어댑터
		 * 상태와 추출 표시가 모두 '비어 있음' 에서 시작한다. */
		slot = kzalloc_obj(struct slot);
		/* [한국어] 메모리 부족이면 */
		if (!slot) {
			/* [한국어] 오류를 담아 두고 */
			status = -ENOMEM;
			/* [한국어] 슬롯을 풀지 않는 출구로 간다 — 아직 할당되지 않았기 때문이다. */
			goto error;
		}

		/* [한국어] 이 슬롯이 속한 버스를 기록한다. 나중에 특정 버스의 슬롯만 걷어낼 때
		 * 이 값으로 골라낸다. */
		slot->bus = bus;
		/* [한국어] 사람이 보는 슬롯 번호. */
		slot->number = i;
		/* [한국어] 그 슬롯의 devfn. 함수 번호는 0 으로 고정한다 — cPCI 보드는 슬롯당
		 * 하나의 장치로 다뤄지기 때문이다. 슬롯 번호와 devfn 을 따로 두는 것은,
		 * 새시 배선에 따라 둘이 어긋날 수 있어서다(헤더의 구조체 주석 참조). */
		slot->devfn = PCI_DEVFN(i, 0);

		/* [한국어] "버스:슬롯" 형태의 이름을 두 자리 16진수로 조립한다. sysfs 에 이
		 * 이름으로 디렉터리가 생긴다. */
		snprintf(name, SLOT_NAME_SIZE, "%02x:%02x", bus->number, i);

		/* [한국어] 위에서 정의한 연산 표를 건다. 모든 슬롯이 같은 표를 가리키므로,
		 * 콜백들은 받은 포인터에서 자기 슬롯을 되찾아야 한다. */
		slot->hotplug_slot.ops = &cpci_hotplug_slot_ops;

		/* [한국어] 어떤 이름으로 등록하는지 남긴다. */
		dbg("registering slot %s", name);
		/* [한국어] 공용 코어에 등록한다. 이 호출이 sysfs 디렉터리와 파일들을 만들고,
		 * 이름 문자열의 소유권도 코어로 넘어간다. */
		status = pci_hp_register(&slot->hotplug_slot, bus, i, name);
		/* [한국어] 등록에 실패하면 */
		if (status) {
			/* [한국어] 원인을 남기고 */
			err("pci_hp_register failed with error %d", status);
			/* [한국어] 방금 할당한 슬롯만 푸는 출구로 간다(위 관찰 1 참조). */
			goto error_slot;
		}
		/* [한국어] 코어가 붙여 준 이름으로 다시 한 번 남긴다 — 조립한 이름과 실제
		 * 등록된 이름이 같은지 확인할 수 있다. */
		dbg("slot registered with name: %s", slot_name(slot));

		/* Add slot to our internal list */
		/* [한국어] 옆의 상류 주석대로 전역 리스트에 넣는다. 목록을 바꾸므로 쓰기 락이 필요하다. */
		down_write(&list_rwsem);
		/* [한국어] 머리에 넣는다 — 꼬리가 아니라 머리라 순서가 역순이 된다(위 관찰 2). */
		list_add(&slot->slot_list, &slot_list);
		/* [한국어] 슬롯 개수를 늘린다. */
		slots++;
		/* [한국어] 락을 푼다. 슬롯 하나마다 락을 잡았다 놓는 구조라, 등록이 오래 걸려도
		 * 다른 문맥이 리스트에 접근할 틈이 생긴다. */
		up_write(&list_rwsem);
	}
	/* [한국어] 범위의 모든 슬롯을 등록했다. */
	return 0;
/* [한국어] 공용 코어 등록에 실패했을 때의 출구 — 이미 할당된 슬롯을 풀어야 한다. */
error_slot:
	/* [한국어] 등록되지 않은 슬롯이므로 pci_hp_deregister 없이 그냥 반납한다. */
	kfree(slot);
/* [한국어] 할당 자체가 실패했을 때의 출구 — 풀 것이 없다. */
error:
	/* [한국어] 실패 코드를 보드 드라이버에 전달한다. */
	return status;
}
/* [한국어] 보드별 드라이버가 모듈로 빌드될 수 있으므로 심볼을 내보낸다.
 * GPL 전용 판이라 GPL 호환 모듈만 쓸 수 있다. */
EXPORT_SYMBOL_GPL(cpci_hp_register_bus);

/* [한국어]
 * cpci_hp_unregister_bus - 한 버스에 속한 슬롯을 모두 걷어낸다
 *
 * @bus: 대상 버스.
 * @return: 0 성공, -1(등록된 슬롯이 하나도 없음).
 *
 * 등록의 짝이다. 전역 리스트가 버스를 가리지 않고 한 줄로 이어져 있으므로,
 * 순회하며 버스가 일치하는 것만 골라낸다. 순회 중에 노드를 지우므로 안전한
 * 순회 판(_safe)을 써야 한다 — 지운 노드의 next 를 따라가면 안 되기 때문이다.
 * 슬롯 하나를 걷어내는 순서가 정해져 있다: 리스트에서 떼고 → 공용 코어에서
 * 등록 해제하고 → 마지막에 구조체를 푼다. 코어 해제가 먼저여야 sysfs 를 통해
 * 이미 해제된 슬롯에 접근하는 일이 없다.
 * 실행 컨텍스트: 보드 드라이버의 제거 경로(프로세스 컨텍스트). 쓰기 락을 잡으므로
 * 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] 반환값 변수를 0 으로 두고 한 번도 바꾸지 않아, 슬롯이
 * 하나라도 있으면 언제나 0 을 돌려준다. 또 대상 버스에 해당하는 슬롯이 하나도
 * 없어도 성공으로 보고한다.
 *
 * 호출 체인:
 *   보드별 드라이버 → [cpci_hp_unregister_bus]
 *     → pci_hp_deregister(), release_slot()
 */
int
cpci_hp_unregister_bus(struct pci_bus *bus)
{
	/* [한국어] 순회 커서. */
	struct slot *slot;
	/* [한국어] 안전한 순회를 위해 다음 노드를 미리 담아 둘 자리. */
	struct slot *tmp;
	/* [한국어] 반환값(위 관찰 참조). */
	int status = 0;

	/* [한국어] 리스트를 바꾸므로 쓰기 락을 잡는다. */
	down_write(&list_rwsem);
	/* [한국어] 등록된 슬롯이 하나도 없으면 걷어낼 것도 없다. */
	if (!slots) {
		/* [한국어] 락을 풀고 */
		up_write(&list_rwsem);
		/* [한국어] 실패를 알린다. errno 가 아니라 -1 을 그대로 쓰는 옛 관례다. */
		return -1;
	}
	/* [한국어] 지우면서 순회하므로 안전한 판을 쓴다 — 현재 노드를 지워도 다음
	 * 노드를 미리 잡아 두어 순회가 깨지지 않는다. */
	list_for_each_entry_safe(slot, tmp, &slot_list, slot_list) {
		/* [한국어] 이 슬롯이 대상 버스의 것인지 확인한다. 다른 버스의 슬롯은 건너뛴다. */
		if (slot->bus == bus) {
			/* [한국어] 먼저 리스트에서 뗀다. */
			list_del(&slot->slot_list);
			/* [한국어] 개수도 줄인다. */
			slots--;

			/* [한국어] 어느 슬롯을 걷어내는지 남긴다. */
			dbg("deregistering slot %s", slot_name(slot));
			/* [한국어] 공용 코어에서 등록을 해제한다. 이 호출이 sysfs 파일들을 없애므로,
			 * 이 뒤로는 사용자가 이 슬롯의 콜백을 부를 수 없다. */
			pci_hp_deregister(&slot->hotplug_slot);
			/* [한국어] 마지막으로 장치 참조와 구조체를 반납한다. 코어 해제 뒤여야 안전하다. */
			release_slot(slot);
		}
	}
	/* [한국어] 락을 푼다. */
	up_write(&list_rwsem);
	/* [한국어] 언제나 0 이다(위 관찰 참조). */
	return status;
}
/* [한국어] 등록 함수와 짝을 이루는 공개 심볼. */
EXPORT_SYMBOL_GPL(cpci_hp_unregister_bus);

/* [한국어]
 * cpci_hp_intr - #ENUM 인터럽트의 1차 핸들러. 인터럽트를 끄고 스레드를 깨운다
 *
 * @irq: 발생한 IRQ 번호(쓰이지 않는다).
 * @data: request_irq 에 넘겼던 보드 드라이버의 불투명 포인터(쓰이지 않는다).
 * @return: IRQ_HANDLED(우리 인터럽트) 또는 IRQ_NONE(다른 장치의 것).
 *
 * 하는 일이 아주 적은 것이 핵심이다. 슬롯 상태를 살피려면 설정공간을 읽어야
 * 하고 세마포어도 잡아야 하는데, 둘 다 인터럽트 문맥에서 할 수 없다. 그래서
 * 여기서는 인터럽트를 꺼서 폭주를 막고 스레드를 깨우기만 한다. 실제 처리는
 * 전부 event_thread() 가 하고, 껐던 인터럽트도 그 스레드가 되켠다.
 * #ENUM 은 레벨 신호라, 원인을 없애기 전까지 계속 어서트되어 있다 — 그래서
 * 끄지 않으면 같은 인터럽트가 끝없이 다시 들어온다.
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   (인터럽트 코어) → [cpci_hp_intr]
 *     → controller->ops->check_irq(), controller->ops->disable_irq(),
 *       wake_up_process()
 */
/* This is the interrupt mode interrupt handler */
static irqreturn_t
cpci_hp_intr(int irq, void *data)
{
	/* [한국어] 진입 흔적을 남긴다. */
	dbg("entered cpci_hp_intr");

	/* Check to see if it was our interrupt */
	/* [한국어] 옆의 상류 주석대로 우리 인터럽트인지 확인한다. 공유 IRQ 로 등록했을
	 * 때만 검사하며, 그 판정은 보드별 드라이버만 할 수 있으므로 콜백에 맡긴다.
	 * 단독 IRQ 라면 검사 없이 우리 것으로 본다. */
	if ((controller->irq_flags & IRQF_SHARED) &&
	    !controller->ops->check_irq(controller->dev_id)) {
		/* [한국어] 다른 장치의 인터럽트였다는 흔적을 남기고 */
		dbg("exited cpci_hp_intr, not our interrupt");
		/* [한국어] 처리하지 않았다고 알려 다음 핸들러로 넘긴다. */
		return IRQ_NONE;
	}

	/* Disable ENUM interrupt */
	/* [한국어] 옆의 상류 주석대로 #ENUM 인터럽트를 끈다. 이 신호는 원인이 사라질
	 * 때까지 계속 어서트되므로, 끄지 않으면 같은 인터럽트가 끝없이 다시 들어와
	 * 스레드가 돌 틈이 없다. 스레드가 처리를 마친 뒤 다시 켠다. */
	controller->ops->disable_irq();

	/* Trigger processing by the event thread */
	/* [한국어] 옆의 상류 주석대로 스레드를 깨운다. 스레드는
	 * TASK_INTERRUPTIBLE 로 자신을 재우고 schedule() 에 들어가 있으므로,
	 * 이 한 줄이 그것을 실행 가능 상태로 되돌린다. 인터럽트 문맥에서 할 수 있는
	 * 몇 안 되는 동작이다. */
	wake_up_process(cpci_thread);
	/* [한국어] 우리가 처리했다고 알린다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * init_slots - 등록된 슬롯의 초기 상태를 하드웨어에서 읽어 맞춘다
 *
 * @clear_ins: 참이면 각 슬롯의 INS 비트를 지운다.
 * @return: 0 성공, -1(등록된 슬롯이 없음).
 *
 * 감시를 시작하기 직전에 한 번 불려, 소프트웨어가 기억하는 상태와 실제 하드웨어
 * 상태를 일치시킨다. 이것이 없으면 이미 꽂혀 있던 보드를 새로 꽂힌 것으로
 * 오해하게 된다.
 * INS 비트를 지우는 이유는 위 상류 주석이 인용한 규격 조항에 있다 — 부팅
 * 시점에 이미 꽂혀 있던(cold-inserted) 보드들도 INS 비트를 세워 두므로,
 * 시스템 드라이버가 그것을 지워 주어야 이후의 INS 가 진짜 새 삽입을 뜻하게 된다.
 * clear_ins 를 인자로 받는 것은 그 정리가 첫 시작에서만 필요하기 때문이다
 * (cpci_hp_start 의 static 변수가 그것을 관리한다).
 * 그 다음 슬롯마다 실제로 장치가 있는지 조회해, 있으면 어댑터·래치 상태를
 * '꽂힘' 으로 맞추고 장치 참조를 잡아 둔다.
 * 실행 컨텍스트: cpci_hp_start() 안(프로세스 컨텍스트). 읽기 락을 잡는다 —
 * 리스트 구조는 바꾸지 않고 각 슬롯의 필드만 고치기 때문이다.
 *
 * 호출 체인:
 *   cpci_hp_start() → [init_slots]
 *     → cpci_check_and_clear_ins(), pci_get_slot()
 */
/*
 * According to PICMG 2.1 R2.0, section 6.3.2, upon
 * initialization, the system driver shall clear the
 * INS bits of the cold-inserted devices.
 */
static int
init_slots(int clear_ins)
{
	/* [한국어] 순회 커서. */
	struct slot *slot;
	/* [한국어] 슬롯 자리에서 찾은 PCI 장치. */
	struct pci_dev *dev;

	/* [한국어] 진입 흔적. */
	dbg("%s - enter", __func__);
	/* [한국어] 리스트 구조는 바꾸지 않으므로 읽기 락이면 충분하다. */
	down_read(&list_rwsem);
	/* [한국어] 슬롯이 하나도 등록되지 않았다면 할 일이 없다. */
	if (!slots) {
		/* [한국어] 락을 풀고 */
		up_read(&list_rwsem);
		/* [한국어] 실패를 알린다. 호출자가 이 값을 그대로 올려 보내 시작이 중단된다. */
		return -1;
	}
	/* [한국어] 등록된 모든 슬롯을 훑는다. */
	list_for_each_entry(slot, &slot_list, slot_list) {
		/* [한국어] 어느 슬롯을 보는 중인지 남긴다. */
		dbg("%s - looking at slot %s", __func__, slot_name(slot));
		/* [한국어] 첫 시작이라면 이미 서 있던 INS 비트를 지운다. 검사와 지우기가 한
		 * 함수 안에서 일어나며, 실제로 지웠을 때만 참을 돌려준다. */
		if (clear_ins && cpci_check_and_clear_ins(slot))
			/* [한국어] 지웠다는 사실을 남긴다 — 부팅 시 이미 꽂혀 있던 보드를 알 수 있다. */
			dbg("%s - cleared INS for slot %s",
			    __func__, slot_name(slot));
		/* [한국어] 이 슬롯 자리에 실제로 장치가 있는지 조회한다. 함수 번호 0 으로 고정한
		 * devfn 을 쓰는 것은 등록 때와 같은 규칙이다. 이 호출은 찾은 장치의 참조를
		 * 하나 올려 주며, 그 참조는 release_slot() 이 놓는다. */
		dev = pci_get_slot(slot->bus, PCI_DEVFN(slot->number, 0));
		/* [한국어] 장치가 있다면 이미 꽂혀 있던 보드다. */
		if (dev) {
			/* [한국어] 어댑터가 있는 것으로 기록하고 */
			slot->adapter_status = 1;
			/* [한국어] 래치도 닫힌 것으로 본다 — 정상적으로 꽂혀 동작 중이라는 뜻이다. */
			slot->latch_status = 1;
			/* [한국어] 장치 포인터를 보관한다. 이 값이 NULL 이 아니라는 사실이 나중에
			 * check_slots() 에서 '이미 꽂혀 있다' 는 판정 근거가 된다. */
			slot->dev = dev;
		}
	}
	/* [한국어] 락을 푼다. */
	up_read(&list_rwsem);
	/* [한국어] 종료 흔적. */
	dbg("%s - exit", __func__);
	/* [한국어] 초기 상태 맞추기 완료. */
	return 0;
}

/* [한국어]
 * check_slots - 모든 슬롯의 HS CSR 을 읽어 삽입·추출을 판별하고 처리한다
 *
 * @return: 양수면 이번에 감지한 추출 요청 개수, 0 이면 처리할 것이 없지만
 *          추출이 아직 진행 중, 음수(-1)면 더 볼 수 없으니 감시를 멈추라는 뜻.
 *
 * 이 파일의 심장이다. 두 스레드 모두 이 함수를 부르는 것 말고는 하는 일이 없다.
 * 반환값 규약이 특이해서 호출자 쪽 코드가 그것에 맞춰 짜여 있다 —
 * 양수면 사용자 공간이 반응할 시간을 주려고 500ms 자고, 음수면 스레드를
 * 끝내고, 0 이면 추출 카운터를 보고 계속 돌지 정한다.
 *
 * 슬롯마다 세 갈래로 갈린다.
 *  1) INS 비트가 서 있다 → 새로 꽂혔다. 다만 이미 장치를 알고 있는 슬롯이면
 *     옆의 상류 주석이 말하는 고장난 하드웨어(같은 신호를 두 번 내는 경우)라
 *     경고만 남기고 넘어간다. 정상이면 PCI 열거를 하고 상태를 갱신한 뒤
 *     파란 LED 를 꺼서 '사용 중' 을 알린다.
 *  2) EXT 비트가 서 있다 → 사람이 이젝터를 열었다. 여기서는 표시만 하고
 *     실제 제거는 하지 않는다. 사용자 공간이 sysfs 로 disable 을 걸어야
 *     disable_slot() 이 마무리한다.
 *  3) 둘 다 아닌데 이 슬롯이 추출 중이었다 → HS CSR 이 0xffff 로 읽히면
 *     설정공간 접근이 응답하지 않는다는 뜻이고, 그것은 사람이 절차를 무시하고
 *     보드를 그냥 뽑아 버렸다는 신호다. 오류를 남기고 상태만 되돌린다.
 *
 * 마지막 판정도 중요하다. 삽입도 추출도 하나 없고 추출 진행 중인 슬롯도 없다면,
 * #ENUM 이 어서트되었는데 원인을 찾지 못한 것이다. 그대로 두면 인터럽트 방식에서
 * 무한 루프가 되므로 감시를 접는다.
 *
 * 실행 컨텍스트: 감시 스레드(프로세스 컨텍스트). 읽기 락 아래에서 설정공간
 * 접근과 PCI 열거를 수행하므로 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] 읽기 락 아래에서 슬롯의 상태 필드와 전역 추출 카운터를
 * 바꾼다. 읽기 락은 여러 문맥이 동시에 들어올 수 있어 배타를 보장하지 않는데,
 * 카운터가 원자 변수인 것이 그 부분을 메워 준다. 상태 필드 쪽은 감시 스레드가
 * 하나뿐이라는 사실에 기대고 있는 것으로 읽힌다.
 *
 * 호출 체인:
 *   event_thread() / poll_thread() → [check_slots]
 *     → cpci_check_and_clear_ins(), cpci_get_hs_csr(), cpci_configure_slot(),
 *       cpci_led_off(), cpci_check_ext()
 */
static int
check_slots(void)
{
	/* [한국어] 순회 커서. */
	struct slot *slot;
	/* [한국어] 이번에 감지한 추출 요청 개수. 반환값이 된다. */
	int extracted;
	/* [한국어] 이번에 처리한 삽입 개수. 반환값 판정에만 쓰인다. */
	int inserted;
	/* [한국어] 읽어 온 HS CSR 값. 디버그 로그와 '그냥 뽑힘' 판정에 쓴다. */
	u16 hs_csr;

	/* [한국어] 리스트 구조는 바꾸지 않으므로 읽기 락을 잡는다(위 관찰 참조). */
	down_read(&list_rwsem);
	/* [한국어] 슬롯이 하나도 없으면 감시할 대상이 없다. */
	if (!slots) {
		/* [한국어] 락을 풀고 */
		up_read(&list_rwsem);
		/* [한국어] 원인을 남긴 뒤 */
		err("no slots registered, shutting down");
		/* [한국어] 감시를 멈추라는 뜻의 음수를 돌려준다. */
		return -1;
	}
	/* [한국어] 두 카운터를 0 에서 시작한다. 한 줄에 이어 대입하는 옛 스타일이다. */
	extracted = inserted = 0;
	/* [한국어] 등록된 모든 슬롯을 훑는다. */
	list_for_each_entry(slot, &slot_list, slot_list) {
		/* [한국어] 어느 슬롯을 보는 중인지 남긴다. */
		dbg("%s - looking at slot %s", __func__, slot_name(slot));
		/* [한국어] INS 비트가 서 있는지 보고, 서 있으면 지운다. 검사와 지우기가 한
		 * 동작인 것은 그 사이에 새 삽입이 끼어들어 놓치는 일을 막기 위해서다. */
		if (cpci_check_and_clear_ins(slot)) {
			/*
			 * Some broken hardware (e.g. PLX 9054AB) asserts
			 * ENUM# twice...
			 */
			/* [한국어] 옆의 상류 주석대로, 이미 장치를 알고 있는 슬롯인데 또 삽입 신호가
			 * 왔다면 고장난 하드웨어가 신호를 두 번 낸 것이다. */
			if (slot->dev) {
				/* [한국어] 경고를 남기고 */
				warn("slot %s already inserted",
				     slot_name(slot));
				/* [한국어] 삽입으로는 세어 둔다 — 그래야 아래 '원인을 못 찾겠다' 판정에
				 * 걸리지 않아 감시가 계속된다. */
				inserted++;
				/* [한국어] 다음 슬롯으로 넘어간다. 다시 열거하지는 않는다. */
				continue;
			}

			/* Process insertion */
			/* [한국어] 옆의 상류 주석대로 여기부터 삽입 처리다. */
			dbg("%s - slot %s inserted", __func__, slot_name(slot));

			/* GSM, debug */
			/* [한국어] 옆의 상류 디버그 주석대로, 열거 전의 HS CSR 값을 찍어 둔다. */
			hs_csr = cpci_get_hs_csr(slot);
			/* [한국어] 값을 남긴다. 아래에서 같은 일을 두 번 더 하는데, 열거 전/후/LED 조작
			 * 후의 세 시점을 비교하려는 진단이다. */
			dbg("%s - slot %s HS_CSR (1) = %04x",
			    __func__, slot_name(slot), hs_csr);

			/* Configure device */
			/* [한국어] 옆의 상류 주석대로 이제 실제 PCI 열거를 시작한다는 로그. */
			dbg("%s - configuring slot %s",
			    __func__, slot_name(slot));
			/* [한국어] 이 슬롯의 장치를 PCI 서브시스템에 등록한다. 버스 스캔, 자원 배정,
			 * 드라이버 바인딩이 모두 이 안에서 일어나며 구현은 옆 파일에 있다. */
			if (cpci_configure_slot(slot)) {
				/* [한국어] 실패하면 원인을 남기고 */
				err("%s - could not configure slot %s",
				    __func__, slot_name(slot));
				/* [한국어] 이 슬롯을 포기한다. 상태를 갱신하지 않으므로 다음 이벤트에서
				 * 다시 시도될 수 있다. */
				continue;
			}
			/* [한국어] 열거가 끝났다는 로그. */
			dbg("%s - finished configuring slot %s",
			    __func__, slot_name(slot));

			/* GSM, debug */
			/* [한국어] 옆의 상류 디버그 주석대로, 열거 후의 HS CSR 값. */
			hs_csr = cpci_get_hs_csr(slot);
			/* [한국어] 두 번째 시점의 값을 남긴다. */
			dbg("%s - slot %s HS_CSR (2) = %04x",
			    __func__, slot_name(slot), hs_csr);

			/* [한국어] 정상적으로 꽂혀 동작 중이므로 래치가 닫힌 것으로 기록한다. */
			slot->latch_status = 1;
			/* [한국어] 어댑터가 있는 것으로도 기록한다. sysfs 가 이 값을 보여 준다. */
			slot->adapter_status = 1;

			/* [한국어] 파란 LED 를 끈다. 이 LED 는 '뽑아도 안전' 을 뜻하므로, 끄는 것이
			 * 곧 '이제 사용 중이니 뽑지 말라' 는 신호다. */
			cpci_led_off(slot);

			/* GSM, debug */
			/* [한국어] 옆의 상류 디버그 주석대로, LED 조작 후의 HS CSR 값. */
			hs_csr = cpci_get_hs_csr(slot);
			/* [한국어] 세 번째 시점의 값을 남긴다. LED 비트가 실제로 바뀌었는지 확인할 수 있다. */
			dbg("%s - slot %s HS_CSR (3) = %04x",
			    __func__, slot_name(slot), hs_csr);

			/* [한국어] 삽입 하나를 처리했다. */
			inserted++;
		/* [한국어] INS 가 아니라면 EXT(추출 요청)인지 본다. 이쪽은 지우지 않는다 —
		 * 정리를 마친 뒤 disable_slot() 이 지워야 하기 때문이다. */
		} else if (cpci_check_ext(slot)) {
			/* Process extraction request */
			/* [한국어] 옆의 상류 주석대로 추출 요청 처리다. 실제 제거는 여기서 하지 않는다. */
			dbg("%s - slot %s extracted",
			    __func__, slot_name(slot));

			/* GSM, debug */
			/* [한국어] 옆의 상류 디버그 주석대로 HS CSR 값을 남겨 둔다. */
			hs_csr = cpci_get_hs_csr(slot);
			/* [한국어] 추출 요청 시점의 값을 기록한다. */
			dbg("%s - slot %s HS_CSR = %04x",
			    __func__, slot_name(slot), hs_csr);

			/* [한국어] 아직 추출 중으로 표시되지 않은 슬롯일 때만 상태를 바꾼다 — EXT 는
			 * 사람이 손잡이를 닫을 때까지 계속 서 있으므로, 이 검사가 없으면 스레드가
			 * 돌 때마다 카운터가 계속 올라간다. */
			if (!slot->extracting) {
				/* [한국어] 래치가 열렸다고 기록한다. sysfs 를 보는 사용자 공간이 이 변화를 본다. */
				slot->latch_status = 0;
				/* [한국어] 이 슬롯을 추출 중으로 표시한다. */
				slot->extracting = 1;
				/* [한국어] 전역 추출 카운터를 올린다. 이 값이 0 이 아닌 동안 스레드의 안쪽
				 * 루프가 계속 돌며 사용자 응답을 기다린다. 원자 연산인 이유는 내리는 쪽이
				 * 쓰기 락 아래에 있어 이쪽 읽기 락과 배타가 보장되지 않기 때문이다. */
				atomic_inc(&extracting);
			}
			/* [한국어] 추출 요청 하나를 감지했다. 이 값이 그대로 반환값이 되어 호출자가
			 * 500ms 자게 만든다. */
			extracted++;
		/* [한국어] INS 도 EXT 도 아닌데 이 슬롯이 추출 중이라면, 사용자 응답을 기다리는
		 * 중이거나 보드가 이미 사라졌거나 둘 중 하나다. */
		} else if (slot->extracting) {
			/* [한국어] 어느 쪽인지 알아보려고 HS CSR 을 읽는다. */
			hs_csr = cpci_get_hs_csr(slot);
			/* [한국어] 모든 비트가 1 로 읽힌다는 것은 설정공간 접근에 아무도 응답하지 않는
			 * 다는 뜻이고, 곧 보드가 이미 물리적으로 빠졌다는 신호다. */
			if (hs_csr == 0xffff) {
				/*
				 * Hmmm, we're likely hosed at this point, should we
				 * bother trying to tell the driver or not?
				 */
				/* [한국어] 옆의 상류 주석이 말하듯 이 시점에는 이미 손쓸 수 없다 — 드라이버에
				 * 알릴 방법도 마땅치 않다. 오류만 남긴다. */
				err("card in slot %s was improperly removed",
				    slot_name(slot));
				/* [한국어] 어댑터가 없는 것으로 되돌린다. 장치 자체를 제거하지는 못한다. */
				slot->adapter_status = 0;
				/* [한국어] 추출 표시를 지우고 */
				slot->extracting = 0;
				/* [한국어] 카운터도 내린다. 이것이 없으면 스레드의 안쪽 루프가 영원히 끝나지 않는다. */
				atomic_dec(&extracting);
			}
		}
	}
	/* [한국어] 락을 푼다. */
	up_read(&list_rwsem);
	/* [한국어] 이번 순회의 결과를 한 줄로 남긴다. 세 숫자를 함께 보면 상태 기계가
	 * 어디에 있는지 알 수 있다. */
	dbg("inserted=%d, extracted=%d, extracting=%d",
	    inserted, extracted, atomic_read(&extracting));
	/* [한국어] 삽입이든 추출이든 무언가 처리했다면 */
	if (inserted || extracted)
		/* [한국어] 추출 개수를 돌려준다. 양수면 호출자가 사용자 공간에 틈을 주려고
		 * 500ms 자고, 0 이면(삽입만 있었다면) 자지 않고 계속 간다. */
		return extracted;
	/* [한국어] 아무것도 처리하지 못했고, 추출 중인 슬롯도 없다면 */
	else if (!atomic_read(&extracting)) {
		/* [한국어] #ENUM 이 어서트되었는데 그 원인을 찾지 못한 것이다. */
		err("cannot find ENUM# source, shutting down");
		/* [한국어] 감시를 멈추라고 알린다. 그대로 두면 인터럽트 방식에서 같은 인터럽트가
		 * 끝없이 되돌아와 무한 루프가 되기 때문이다. */
		return -1;
	}
	/* [한국어] 처리한 것은 없지만 추출이 진행 중이다 — 사용자 응답을 기다리며
	 * 계속 돌라는 뜻으로 0 을 돌려준다. */
	return 0;
}

/* [한국어]
 * event_thread - 인터럽트 방식일 때의 감시 스레드 몸통
 *
 * @data: kthread_run 에 넘긴 인자(NULL). 쓰이지 않는다.
 * @return: 언제나 0.
 *
 * 인터럽트가 깨워 줄 때까지 잠들어 있다가, 깨어나면 상태 변화가 모두 정리될
 * 때까지 슬롯을 훑고, 다시 인터럽트를 켠 뒤 잠든다.
 * 잠드는 방식이 표준 관용이다 — 자신을 TASK_INTERRUPTIBLE 로 표시한 뒤
 * schedule() 을 부르면, 인터럽트 핸들러의 wake_up_process() 가 그것을 되돌린다.
 * 안쪽 do-while 루프가 이 함수의 핵심이다. 추출이 진행 중인 동안 계속 돌면서
 * 사용자 공간의 응답을 기다리고, 한 바퀴마다 500ms 자서 사용자 공간에 처리할
 * 틈을 준다. 추출 카운터가 0 이 되어야 — 즉 disable_slot() 이 불려야 — 이
 * 루프가 끝난다.
 * 인터럽트를 다시 켜는 것이 마지막인 이유: 처리 중에 켜면 아직 어서트되어 있는
 * #ENUM 이 곧바로 다시 인터럽트를 일으킨다.
 * 실행 컨텍스트: 커널 스레드(프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 정지 요청 검사가 세 곳에 흩어져 있다(잠에서 깬 직후,
 * 안쪽 루프 조건, 안쪽 루프 직후). 안쪽 루프 안에서 오래 머물 수 있어
 * 빠져나갈 지점을 여러 곳에 둔 것으로 읽힌다.
 *
 * 호출 체인:
 *   cpci_start_thread() → kthread_run → [event_thread]
 *     → check_slots(), controller->ops->enable_irq()
 */
/* This is the interrupt mode worker thread body */
static int
event_thread(void *data)
{
	/* [한국어] check_slots() 의 반환값. 세 갈래 판정에 쓰인다. */
	int rc;

	/* [한국어] 스레드가 떴다는 흔적. */
	dbg("%s - event thread started", __func__);
	/* [한국어] 정지 요청을 받을 때까지 무한히 돈다. */
	while (1) {
		/* [한국어] 이제 잠든다는 흔적. */
		dbg("event thread sleeping");
		/* [한국어] 자신을 '깨울 수 있는 잠' 상태로 표시한다. schedule() 을 부르기 전에
		 * 표시해야 하는 것이 중요하다 — 순서가 반대면 그 사이에 도착한 깨우기를
		 * 놓쳐 영원히 잠들 수 있다. */
		set_current_state(TASK_INTERRUPTIBLE);
		/* [한국어] CPU 를 놓고 잠든다. 인터럽트 핸들러의 wake_up_process() 또는
		 * kthread_stop() 이 여기서 깨운다. */
		schedule();
		/* [한국어] 깨어난 이유가 정지 요청이었다면 */
		if (kthread_should_stop())
			/* [한국어] 바깥 루프를 빠져나가 스레드를 끝낸다. */
			break;
		/* [한국어] 여기부터가 이 함수의 핵심 루프다. 상태 변화가 모두 정리될 때까지 돈다. */
		do {
			/* [한국어] 슬롯을 모두 훑어 삽입·추출을 처리한다. */
			rc = check_slots();
			/* [한국어] 추출 요청이 있었다면 */
			if (rc > 0) {
				/* Give userspace a chance to handle extraction */
				/* [한국어] 옆의 상류 주석대로 사용자 공간이 반응할 시간을 준다. 사용자 공간이
				 * sysfs 로 disable 을 걸어야 추출이 끝나므로, 여기서 쉬지 않으면 이 스레드가
				 * CPU 를 붙잡고 같은 상태를 계속 다시 읽게 된다. */
				msleep(500);
			/* [한국어] 음수는 더 볼 수 없다는 뜻이다. */
			} else if (rc < 0) {
				/* [한국어] 원인을 남기고 */
				dbg("%s - error checking slots", __func__);
				/* [한국어] 스스로 끝났음을 표시한다 — 이 표시가 있어야 나중에 해제 경로가
				 * 이미 끝난 스레드를 다시 멈추려 하지 않는다. */
				thread_finished = 1;
				/* [한국어] 두 루프를 한 번에 빠져나간다. */
				goto out;
			}
		/* [한국어] 추출이 아직 진행 중이고 정지 요청도 없으면 계속 돈다. 카운터가
		 * 0 이 되는 시점은 disable_slot() 이 결정한다. */
		} while (atomic_read(&extracting) && !kthread_should_stop());
		/* [한국어] 안쪽 루프를 빠져나온 이유가 정지 요청이었다면 */
		if (kthread_should_stop())
			/* [한국어] 바깥 루프도 빠져나간다. */
			break;

		/* Re-enable ENUM# interrupt */
		/* [한국어] 옆의 상류 주석대로 이제서야 인터럽트를 다시 켠다는 로그. */
		dbg("%s - re-enabling irq", __func__);
		/* [한국어] 처리를 모두 마친 뒤에 켜야 한다 — 처리 중에 켜면 아직 어서트되어
		 * 있는 #ENUM 이 곧바로 다시 인터럽트를 일으킨다. */
		controller->ops->enable_irq();
	}
 /* [한국어] 오류로 빠져나온 경로가 모이는 곳. 라벨 앞에 공백이 하나 있는 것은
  * 편집기가 라벨을 열 0 으로 정렬하지 않게 하려는 커널의 흔한 관례다. */
 out:
	/* [한국어] 커널 스레드의 반환값은 쓰이지 않으므로 0 으로 둔다. */
	return 0;
}

/* [한국어]
 * poll_thread - 폴링 방식일 때의 감시 스레드 몸통
 *
 * @data: kthread_run 에 넘긴 인자(NULL). 쓰이지 않는다.
 * @return: 언제나 0.
 *
 * 인터럽트가 없는 보드에서 쓴다. 잠들었다 깨는 대신 100ms 마다 스스로
 * #ENUM 이 어서트되었는지 물어보고, 그렇다면 인터럽트 방식과 똑같은 안쪽
 * 루프를 돈다. 두 스레드의 차이는 '이벤트를 어떻게 알아채는가' 뿐이고,
 * 알아챈 뒤의 처리는 완전히 같다.
 * 폴링이라 인터럽트를 껐다 켤 필요가 없어 그 코드가 없다.
 * 정지 조건에 시그널 검사가 하나 더 있는 것도 다른 점이다.
 * 실행 컨텍스트: 커널 스레드(프로세스 컨텍스트). msleep 으로 잠든다.
 *
 * [상류 코드 관찰] 100ms 주기는 상수로 박혀 있고 조정 수단이 없다. 사람이
 * 보드를 꽂고 뽑는 속도에 견주면 충분히 짧다는 판단으로 읽히지만, 그 근거를
 * 밝힌 주석은 없다.
 *
 * 호출 체인:
 *   cpci_start_thread() → kthread_run → [poll_thread]
 *     → controller->ops->query_enum(), check_slots()
 */
/* This is the polling mode worker thread body */
static int
poll_thread(void *data)
{
	/* [한국어] check_slots() 의 반환값. */
	int rc;

	/* [한국어] 정지 요청이나 시그널을 받을 때까지 돈다. */
	while (1) {
		/* [한국어] 정지 요청이 왔거나 시그널이 밀려 있으면 — 후자는 인터럽트 방식
		 * 스레드에는 없는 검사다. */
		if (kthread_should_stop() || signal_pending(current))
			/* [한국어] 루프를 빠져나가 스레드를 끝낸다. */
			break;
		/* [한국어] 보드 드라이버에게 #ENUM 이 어서트되었는지 물어본다. 이 한 줄이
		 * 인터럽트 방식의 '깨어남' 을 대신한다. */
		if (controller->ops->query_enum()) {
			/* [한국어] 이벤트가 있다면, 인터럽트 방식과 똑같은 안쪽 루프를 돈다. */
			do {
				/* [한국어] 슬롯을 모두 훑어 삽입·추출을 처리한다. */
				rc = check_slots();
				/* [한국어] 추출 요청이 있었다면 */
				if (rc > 0) {
					/* Give userspace a chance to handle extraction */
					/* [한국어] 옆의 상류 주석대로 사용자 공간에 처리할 틈을 준다. */
					msleep(500);
				/* [한국어] 음수는 더 볼 수 없다는 뜻이다. */
				} else if (rc < 0) {
					/* [한국어] 원인을 남기고 */
					dbg("%s - error checking slots", __func__);
					/* [한국어] 스스로 끝났음을 표시한 뒤 */
					thread_finished = 1;
					/* [한국어] 두 루프를 한 번에 빠져나간다. */
					goto out;
				}
			/* [한국어] 추출이 진행 중이고 정지 요청도 없으면 계속 돈다. */
			} while (atomic_read(&extracting) && !kthread_should_stop());
		}
		/* [한국어] 다음 폴링까지 100ms 잔다. 이벤트가 없을 때도 이 만큼은 반드시 자므로,
		 * 이 스레드가 CPU 를 계속 쓰지는 않는다. */
		msleep(100);
	}
 /* [한국어] 오류로 빠져나온 경로가 모이는 곳. */
 out:
	/* [한국어] 커널 스레드의 반환값은 쓰이지 않으므로 0 으로 둔다. */
	return 0;
}

/* [한국어]
 * cpci_start_thread - 컨트롤러의 방식에 맞는 감시 스레드를 띄운다
 *
 * @return: 0 성공, 스레드 생성 실패 시 그 오류.
 *
 * 인터럽트 번호가 있으면 인터럽트 방식 스레드를, 없으면 폴링 방식 스레드를
 * 띄운다. 그 선택 하나로 두 방식이 갈리며, 이후 코드는 어느 쪽인지 신경 쓰지
 * 않는다. 스레드 이름도 달라서(eventd / polld) ps 만 봐도 어느 방식인지 알 수 있다.
 * 실행 컨텍스트: cpci_hp_start() 안(프로세스 컨텍스트). 스레드 생성이 잠들 수 있다.
 *
 * [상류 코드 관찰] 실패하면 cpci_thread 에 ERR_PTR 값이 남는다. 호출자가
 * 오류를 그대로 올려 보내 시작이 중단되므로 그 값을 쓰는 경로는 없지만,
 * thread_finished 는 0 인 채로 남는다.
 *
 * 호출 체인:
 *   cpci_hp_start() → [cpci_start_thread] → kthread_run()
 */
static int
cpci_start_thread(void)
{
	/* [한국어] 인터럽트 번호가 있으면 인터럽트 방식이다. 이 한 줄이 두 방식을 가른다. */
	if (controller->irq)
		/* [한국어] 잠들었다 인터럽트에 깨어나는 스레드를 띄운다. */
		cpci_thread = kthread_run(event_thread, NULL, "cpci_hp_eventd");
	/* [한국어] 인터럽트가 없으면 폴링 방식이다. */
	else
		/* [한국어] 100ms 마다 스스로 물어보는 스레드를 띄운다. 이름이 달라 ps 로
		 * 구분할 수 있다. */
		cpci_thread = kthread_run(poll_thread, NULL, "cpci_hp_polld");
	/* [한국어] 생성 실패는 포인터에 오류가 실려 온다. */
	if (IS_ERR(cpci_thread)) {
		/* [한국어] 원인을 남기고 */
		err("Can't start up our thread");
		/* [한국어] 실린 오류를 그대로 올려 보낸다. */
		return PTR_ERR(cpci_thread);
	}
	/* [한국어] 스레드가 살아 있음을 표시한다. 이 값이 0 이어야 나중에 정지 경로가
	 * 스레드를 멈추려 시도한다. */
	thread_finished = 0;
	/* [한국어] 감시 시작 성공. */
	return 0;
}

/* [한국어]
 * cpci_stop_thread - 감시 스레드를 멈춘다
 *
 * @return: 없음.
 *
 * kthread_stop 은 스레드에 정지 요청을 걸고 그것이 실제로 끝날 때까지 기다린다.
 * 두 스레드 모두 루프 안에서 kthread_should_stop() 을 확인하므로 그 요청에
 * 응답한다. 인터럽트 방식 스레드가 schedule() 에서 자고 있어도 kthread_stop 이
 * 깨워 준다.
 * 실행 컨텍스트: 정지·해제 경로(프로세스 컨텍스트). 스레드가 끝날 때까지
 * 기다리므로 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] cpci_hp_stop() 은 이 함수를 조건 없이 부르고,
 * cpci_hp_unregister_controller() 는 thread_finished 를 확인한 뒤 부른다.
 * 두 경로의 방어 수준이 다르며, 스레드를 한 번도 띄우지 않은 상태에서는
 * cpci_thread 가 아직 NULL 이다. 그 상태로 kthread_stop 을 부르면 어떻게
 * 되는지는 kernel/kthread.c 가 이 트리에 없어 확인 못 함.
 *
 * 호출 체인:
 *   cpci_hp_stop() / cpci_hp_unregister_controller() → [cpci_stop_thread]
 *     → kthread_stop()
 */
static void
cpci_stop_thread(void)
{
	/* [한국어] 정지를 요청하고 스레드가 끝날 때까지 기다린다. */
	kthread_stop(cpci_thread);
	/* [한국어] 끝났음을 표시해 두 번 멈추려는 시도를 막는다. */
	thread_finished = 1;
}

/* [한국어]
 * cpci_hp_register_controller - 보드별 컨트롤러를 등록하고 인터럽트를 잡는다
 *
 * @new_controller: 보드 드라이버가 채운 컨트롤러 서술(콜백 표 포함).
 * @return: 0 성공, -1(이미 등록됨), -EINVAL(인자 부족), -ENODEV(IRQ 획득 실패).
 *
 * 보드별 드라이버가 가장 먼저 부르는 함수다. 이 코어는 시스템에 컨트롤러가
 * 하나뿐이라고 가정하므로(전역 포인터 하나), 두 번째 등록은 거절한다.
 * 인터럽트 번호가 있으면 여기서 핸들러까지 걸어 둔다. 다만 실제로 인터럽트를
 * 허용하는 것은 cpci_hp_start() 이므로, 등록만으로 이벤트가 들어오지는 않는다
 * — 슬롯이 아직 등록되기 전이라 이벤트가 오면 처리할 대상이 없기 때문이다.
 * 실행 컨텍스트: 보드 드라이버의 probe(프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 세 가지.
 *  1. enable_irq / disable_irq 콜백이 없으면 status 를 -EINVAL 로 두지만 곧바로
 *     돌아가지 않고 그대로 request_irq 까지 진행한다. 그 요청이 성공하면
 *     status 는 -EINVAL 인 채로 남아 컨트롤러가 등록되지 않는데, 잡아 둔 IRQ 를
 *     놓는 코드는 없다. 해제 함수도 컨트롤러가 NULL 이면 곧장 돌아가므로
 *     그 IRQ 는 반납되지 않는다.
 *  2. "acquired controller irq" 로그가 실패 분기 밖에 있어, request_irq 가
 *     실패해도 찍힌다.
 *  3. 이미 등록된 경우의 반환값이 errno 가 아니라 -1 이다.
 *
 * 호출 체인:
 *   보드별 드라이버(cpcihp_generic.c 등) → [cpci_hp_register_controller]
 *     → request_irq()
 */
int
cpci_hp_register_controller(struct cpci_hp_controller *new_controller)
{
	/* [한국어] 각 단계의 결과. 0 이어야 등록이 성사된다. */
	int status = 0;

	/* [한국어] 이미 컨트롤러가 등록되어 있으면 */
	if (controller)
		/* [한국어] 거절한다. 이 코어는 컨트롤러를 하나만 다룬다(위 관찰 3 참조). */
		return -1;
	/* [한국어] 서술 자체가 없거나 콜백 표가 없으면 아무것도 할 수 없다. */
	if (!(new_controller && new_controller->ops))
		/* [한국어] 잘못된 인자로 거절한다. */
		return -EINVAL;
	/* [한국어] 인터럽트 번호가 있으면 인터럽트 방식이다. 0 이면 폴링 방식이라
	 * 이 블록을 통째로 건너뛴다. */
	if (new_controller->irq) {
		/* [한국어] 인터럽트 방식이라면 인터럽트를 켜고 끄는 콜백이 반드시 있어야 한다
		 * — 핸들러가 인터럽트를 끄고 스레드가 다시 켜는 구조이기 때문이다. */
		if (!(new_controller->ops->enable_irq &&
		     new_controller->ops->disable_irq))
			/* [한국어] 없으면 잘못된 인자로 표시한다. 다만 여기서 돌아가지 않고 아래
			 * request_irq 까지 진행한다(위 관찰 1 참조). */
			status = -EINVAL;
		/* [한국어] 핸들러를 건다. 플래그는 보드 드라이버가 정한 값을 그대로 쓰므로,
		 * 공유 IRQ 여부도 보드 쪽이 결정한다. 마지막 인자가 핸들러의 data 이자
		 * check_irq 콜백이 소유권을 가릴 때 비교하는 값이 된다. */
		if (request_irq(new_controller->irq,
			       cpci_hp_intr,
			       new_controller->irq_flags,
			       MY_NAME,
			       new_controller->dev_id)) {
			/* [한국어] 실패하면 어느 IRQ 에서 실패했는지 남기고 */
			err("Can't get irq %d for the hotplug cPCI controller",
			    new_controller->irq);
			/* [한국어] 장치가 없다는 뜻으로 표시한다. */
			status = -ENODEV;
		}
		/* [한국어] IRQ 를 잡았다는 로그. 실패 분기 밖에 있어 실패해도 찍힌다(위 관찰 2). */
		dbg("%s - acquired controller irq %d",
		    __func__, new_controller->irq);
	}
	/* [한국어] 모든 검사를 통과했을 때만 */
	if (!status)
		/* [한국어] 전역 컨트롤러 포인터에 심는다. 이 대입 이후부터 슬롯 등록과 감시
		 * 시작이 가능해진다. */
		controller = new_controller;
	/* [한국어] 0 이면 성공, 음수면 실패로 보드 드라이버에 전달된다. */
	return status;
}
/* [한국어] 보드별 드라이버가 모듈로 빌드될 수 있으므로 심볼을 내보낸다. */
EXPORT_SYMBOL_GPL(cpci_hp_register_controller);

/* [한국어]
 * cleanup_slots - 남아 있는 슬롯을 버스와 관계없이 모두 걷어낸다
 *
 * @return: 없음.
 *
 * cpci_hp_unregister_bus() 가 특정 버스의 슬롯만 골라내는 것과 달리, 이쪽은
 * 컨트롤러가 사라질 때 남은 것을 전부 정리한다. 보드 드라이버가 버스 해제를
 * 빠뜨렸을 때의 마지막 그물이다.
 * 슬롯 하나를 걷어내는 순서(리스트에서 떼기 → 공용 코어 해제 → 구조체 반납)는
 * 버스 단위 해제와 같다.
 * 실행 컨텍스트: 컨트롤러 해제 경로(프로세스 컨텍스트). 쓰기 락을 잡는다.
 *
 * [상류 코드 관찰] 슬롯을 지우면서 전역 슬롯 개수를 줄이지 않아, 정리 후에도
 * 그 값이 남는다. 다만 이 함수가 불린 뒤에는 컨트롤러가 NULL 이 되어 다시
 * 시작할 수 없으므로, 그 값이 다시 읽히는 경로는 없다.
 *
 * 호출 체인:
 *   cpci_hp_unregister_controller() → [cleanup_slots]
 *     → pci_hp_deregister(), release_slot()
 */
static void
cleanup_slots(void)
{
	/* [한국어] 순회 커서. */
	struct slot *slot;
	/* [한국어] 안전한 순회를 위해 다음 노드를 미리 담아 둘 자리. */
	struct slot *tmp;

	/*
	 * Unregister all of our slots with the pci_hotplug subsystem,
	 * and free up all memory that we had allocated.
	 */
	/* [한국어] 위 상류 주석대로 남은 슬롯을 모두 정리한다. 리스트를 바꾸므로 쓰기 락. */
	down_write(&list_rwsem);
	/* [한국어] 이미 비어 있으면 */
	if (!slots)
		/* [한국어] 락만 풀고 나가는 출구로 간다. */
		goto cleanup_null;
	/* [한국어] 지우면서 순회하므로 안전한 판을 쓴다. */
	list_for_each_entry_safe(slot, tmp, &slot_list, slot_list) {
		/* [한국어] 리스트에서 뗀다(위 관찰대로 개수는 줄이지 않는다). */
		list_del(&slot->slot_list);
		/* [한국어] 공용 코어에서 등록을 해제해 sysfs 파일을 없앤다. */
		pci_hp_deregister(&slot->hotplug_slot);
		/* [한국어] 장치 참조와 구조체를 반납한다. */
		release_slot(slot);
	}
/* [한국어] 비어 있을 때 뛰어오는 출구. 이름의 null 은 '리스트가 비었다' 는 뜻이다. */
cleanup_null:
	/* [한국어] 락을 푼다. 두 경로가 여기서 만난다. */
	up_write(&list_rwsem);
}

/* [한국어]
 * cpci_hp_unregister_controller - 컨트롤러를 해제하고 남은 것을 모두 정리한다
 *
 * @old_controller: 해제할 컨트롤러. [상류 코드 관찰] 인자로 받지만 쓰지 않는다
 *                  — 전역 포인터 하나만 다루므로 무엇을 해제할지 물을 필요가 없다.
 * @return: 0 성공, -ENODEV(등록된 컨트롤러가 없음).
 *
 * 등록의 짝이며 순서가 정해져 있다. 먼저 감시 스레드를 멈춘다 — 그래야 아래에서
 * 슬롯을 걷어내는 동안 스레드가 그 슬롯을 만지지 않는다. 그 다음 인터럽트를
 * 반납하고, 전역 포인터를 지우고, 마지막에 남은 슬롯을 정리한다.
 * 실행 컨텍스트: 보드 드라이버의 제거 경로(프로세스 컨텍스트). 스레드 종료를
 * 기다리므로 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] thread_finished 를 확인한 뒤에만 스레드를 멈추는데, 그 값은
 * 전역이라 0 으로 시작한다. 그래서 컨트롤러만 등록하고 cpci_hp_start() 를 한
 * 번도 부르지 않은 채 이 함수를 부르면, 아직 NULL 인 스레드 포인터로
 * kthread_stop 이 불린다.
 *
 * 호출 체인:
 *   보드별 드라이버 → [cpci_hp_unregister_controller]
 *     → cpci_stop_thread(), free_irq(), cleanup_slots()
 */
int
cpci_hp_unregister_controller(struct cpci_hp_controller *old_controller)
{
	/* [한국어] 반환값. */
	int status = 0;

	/* [한국어] 등록된 컨트롤러가 있을 때만 정리한다. */
	if (controller) {
		/* [한국어] 스레드가 아직 살아 있다면(위 관찰의 초기값 문제 참조) */
		if (!thread_finished)
			/* [한국어] 먼저 멈춘다. 슬롯을 걷어내기 전에 해야 스레드가 사라진 슬롯을
			 * 만지는 일이 없다. */
			cpci_stop_thread();
		/* [한국어] 인터럽트 방식이었다면 */
		if (controller->irq)
			/* [한국어] 잡아 두었던 IRQ 를 반납한다. 등록 때와 같은 dev_id 를 넘겨야
			 * 공유 IRQ 에서 올바른 핸들러가 떨어진다. */
			free_irq(controller->irq, controller->dev_id);
		/* [한국어] 전역 포인터를 지운다. 이 시점부터 슬롯 등록과 감시 시작이 모두 거절된다. */
		controller = NULL;
		/* [한국어] 마지막으로 남아 있는 슬롯을 전부 걷어낸다. */
		cleanup_slots();
	/* [한국어] 등록된 컨트롤러가 없었다면 */
	} else
		/* [한국어] 해제할 대상이 없다고 알린다. */
		status = -ENODEV;
	/* [한국어] 결과를 보드 드라이버에 전달한다. */
	return status;
}
/* [한국어] 등록 함수와 짝을 이루는 공개 심볼. */
EXPORT_SYMBOL_GPL(cpci_hp_unregister_controller);

/* [한국어]
 * cpci_hp_start - 슬롯 초기 상태를 맞추고 감시를 시작한다
 *
 * @return: 0 성공, -ENODEV(컨트롤러 미등록 또는 슬롯 없음), 하위 단계의 오류.
 *
 * 보드별 드라이버가 컨트롤러와 슬롯을 모두 등록한 뒤 마지막으로 부르는 함수다.
 * 순서가 의미를 갖는다. (1) 준비가 되었는지 확인한다 — 컨트롤러가 있어야 하고
 * 슬롯도 하나는 있어야 한다. (2) 슬롯의 초기 상태를 하드웨어에서 읽어 맞춘다.
 * (3) 감시 스레드를 띄운다. (4) 인터럽트 방식이면 이제서야 인터럽트를 허용한다.
 * 인터럽트를 마지막에 켜는 이유: 스레드가 뜨기 전에 이벤트가 들어오면 깨울
 * 대상이 없기 때문이다.
 * 첫 시작에서만 INS 비트를 지우도록 static 변수로 기억해 둔다. 규격이 요구하는
 * '부팅 시 이미 꽂혀 있던 보드의 INS 정리' 는 한 번이면 충분하고, 정지 후
 * 재시작에서 다시 지우면 그 사이에 들어온 진짜 삽입을 놓치기 때문이다.
 * 실행 컨텍스트: 보드 드라이버의 probe 끝(프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 슬롯 유무 검사에서 읽기 락을 잡았다 곧바로 놓고, 그 뒤
 * init_slots() 가 다시 잡는다. 두 구간 사이에 슬롯이 모두 해제될 수 있는
 * 틈이 생기지만, init_slots() 가 다시 같은 검사를 하므로 결과는 안전하다.
 *
 * 호출 체인:
 *   보드별 드라이버 → [cpci_hp_start]
 *     → init_slots(), cpci_start_thread(), controller->ops->enable_irq()
 */
int
cpci_hp_start(void)
{
	/* [한국어] 이번이 첫 시작인지 기억하는 static 변수. 함수를 벗어나도 값이 남아,
	 * 정지 후 재시작에서는 0 이 된다 — 그래야 INS 정리를 두 번 하지 않는다. */
	static int first = 1;
	/* [한국어] 각 단계의 결과. */
	int status;

	/* [한국어] 진입 흔적. */
	dbg("%s - enter", __func__);
	/* [한국어] 컨트롤러가 등록되어 있지 않으면 */
	if (!controller)
		/* [한국어] 감시를 시작할 수 없다. */
		return -ENODEV;

	/* [한국어] 슬롯이 하나라도 있는지 보려고 읽기 락을 잡는다. */
	down_read(&list_rwsem);
	/* [한국어] 리스트가 비어 있으면 */
	if (list_empty(&slot_list)) {
		/* [한국어] 락을 풀고 */
		up_read(&list_rwsem);
		/* [한국어] 감시할 대상이 없다고 알린다. */
		return -ENODEV;
	}
	/* [한국어] 검사만 하고 곧바로 놓는다(위 관찰 참조). */
	up_read(&list_rwsem);

	/* [한국어] 슬롯의 초기 상태를 하드웨어에서 읽어 맞춘다. 첫 시작이면 INS 비트도
	 * 함께 지운다. */
	status = init_slots(first);
	/* [한국어] 첫 시작이었다면 */
	if (first)
		/* [한국어] 표시를 내려, 다음 시작부터는 INS 를 지우지 않게 한다. 결과를 확인하기
		 * 전에 내리므로, init_slots() 가 실패해도 '첫 시작' 은 소진된다. */
		first = 0;
	/* [한국어] 초기 상태 맞추기가 실패했으면 */
	if (status)
		/* [한국어] 그 오류를 그대로 올려 보낸다. */
		return status;

	/* [한국어] 감시 스레드를 띄운다. 방식(인터럽트/폴링)은 그 안에서 정해진다. */
	status = cpci_start_thread();
	/* [한국어] 스레드를 띄우지 못하면 */
	if (status)
		/* [한국어] 그 오류를 올려 보낸다. */
		return status;
	/* [한국어] 스레드가 떴다는 로그. */
	dbg("%s - thread started", __func__);

	/* [한국어] 인터럽트 방식이라면 */
	if (controller->irq) {
		/* Start enum interrupt processing */
		/* [한국어] 옆의 상류 주석대로 이제 #ENUM 인터럽트를 허용한다는 로그. */
		dbg("%s - enabling irq", __func__);
		/* [한국어] 스레드가 뜬 뒤에 켜야 한다 — 먼저 켜면 이벤트가 들어와도 깨울
		 * 대상이 없다. 이 한 줄로 감시가 실제로 살아난다. */
		controller->ops->enable_irq();
	}
	/* [한국어] 종료 흔적. */
	dbg("%s - exit", __func__);
	/* [한국어] 감시 시작 성공. */
	return 0;
}
/* [한국어] 보드별 드라이버가 부르는 공개 심볼. */
EXPORT_SYMBOL_GPL(cpci_hp_start);

/* [한국어]
 * cpci_hp_stop - 감시를 멈춘다
 *
 * @return: 0 성공, -ENODEV(등록된 컨트롤러가 없음).
 *
 * 시작의 짝이며 순서가 반대다. 먼저 인터럽트를 끄고 그 다음 스레드를 멈춘다
 * — 반대로 하면 스레드가 사라진 뒤에 인터럽트가 들어와 깨울 대상이 없어진다.
 * 슬롯은 그대로 두므로, 다시 cpci_hp_start() 를 불러 감시를 재개할 수 있다.
 * 실행 컨텍스트: 보드 드라이버의 제거 경로(프로세스 컨텍스트). 스레드 종료를
 * 기다리므로 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] thread_finished 를 확인하지 않고 스레드 정지를 조건 없이
 * 부른다. 컨트롤러 해제 함수가 그 값을 확인하는 것과 다른 점이다.
 *
 * 호출 체인:
 *   보드별 드라이버 → [cpci_hp_stop]
 *     → controller->ops->disable_irq(), cpci_stop_thread()
 */
int
cpci_hp_stop(void)
{
	/* [한국어] 컨트롤러가 없으면 멈출 것도 없다. */
	if (!controller)
		/* [한국어] 장치가 없다고 알린다. */
		return -ENODEV;
	/* [한국어] 인터럽트 방식이었다면 */
	if (controller->irq) {
		/* Stop enum interrupt processing */
		/* [한국어] 옆의 상류 주석대로 인터럽트를 먼저 끈다는 로그. */
		dbg("%s - disabling irq", __func__);
		/* [한국어] 스레드를 멈추기 전에 꺼야 한다 — 순서가 반대면 스레드가 사라진 뒤
		 * 인터럽트가 들어와 깨울 대상이 없어진다. */
		controller->ops->disable_irq();
	}
	/* [한국어] 감시 스레드를 멈춘다(위 관찰 참조). */
	cpci_stop_thread();
	/* [한국어] 감시 정지 성공. 슬롯은 그대로 남아 재시작할 수 있다. */
	return 0;
}
/* [한국어] 보드별 드라이버가 부르는 공개 심볼. */
EXPORT_SYMBOL_GPL(cpci_hp_stop);

/* [한국어]
 * cpci_hotplug_init - 이 코어의 초기화. 디버그 플래그를 받아 두는 것이 전부다
 *
 * @debug: PCI 핫플러그 공용 코어의 모듈 파라미터 값.
 * @return: 언제나 0.
 *
 * 이 파일은 독립 모듈이 아니라 pci_hotplug 모듈의 일부로 링크되므로
 * (drivers/pci/hotplug/Makefile:33~34), 자기 module_init 을 갖지 않는다.
 * 대신 공용 코어의 초기화 함수가 이 함수를 불러 준다
 * (pci_hotplug_core.c:1338). 그 덕분에 사용자는 pci_hotplug 모듈의 debug
 * 파라미터 하나로 두 계층의 디버그 로그를 함께 켤 수 있다.
 * 여기서 세팅한 전역은 옆 파일 cpci_hotplug_pci.c 의 디버그 매크로도 함께 본다.
 * __init 이 붙어 있어 부팅이 끝나면 이 코드는 버려진다.
 * 실행 컨텍스트: 부팅 중 initcall(프로세스 컨텍스트).
 * 에러 경로: 없다 — 실패할 수 있는 동작이 없다.
 *
 * 호출 체인:
 *   device_initcall → pci_hotplug_init() → [cpci_hotplug_init]
 */
int __init
cpci_hotplug_init(int debug)
{
	/* [한국어] 공용 코어의 모듈 파라미터 값을 전역에 옮겨 담는다. 이 값 하나가
	 * 이 파일과 cpci_hotplug_pci.c 의 dbg 매크로를 함께 켜고 끈다. */
	cpci_debug = debug;
	/* [한국어] 실패할 수 있는 동작이 없으므로 언제나 성공이다. 그래도 반환값을
	 * 두는 것은, 이 함수가 CONFIG_HOTPLUG_PCI_CPCI 가 꺼졌을 때 헤더의 인라인
	 * 더미로 대체되어야 하고 호출자가 그 결과를 확인하기 때문이다. */
	return 0;
}
