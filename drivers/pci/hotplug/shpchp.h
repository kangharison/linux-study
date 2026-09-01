/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Standard Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>,<kristen.c.accardi@intel.com>
 *
 */
/* [한국어] 이중 포함 방지. 이 헤더를 shpchp_core.c / shpchp_ctrl.c / shpchp_hpc.c /
 * shpchp_pci.c / shpchp_sysfs.c 다섯 파일이 포함한다. */
/*
 * [한국어 설명] SHPC(Standard Hot-Plug Controller) 드라이버의 공용 헤더 (shpchp.h)
 *
 * === 파일의 역할 ===
 * SHPC 는 PCI(및 PCI-X) 시절의 표준 핫플러그 컨트롤러 규격이다. PCIe 의
 * pciehp 가 링크 상태와 Slot Control 레지스터로 슬롯을 다루는 것과 달리,
 * SHPC 는 브리지 안에 별도의 레지스터 창을 두고 그곳에 **명령을 써서**
 * 슬롯을 조작한다. 그 명령·응답 모델이 이 헤더의 구조를 결정한다.
 * 이 헤더는 shpchp 를 이루는 다섯 파일 — shpchp_core.c(모듈과 sysfs 등록),
 * shpchp_ctrl.c(이벤트 상태 기계), shpchp_hpc.c(하드웨어 조작),
 * shpchp_pci.c(장치 열거), shpchp_sysfs.c(추가 속성) — 이 공유하는 자료구조,
 * 상수, 함수 원형을 모아 둔다.
 * 코드는 static inline 다섯 개뿐인데, 그중 둘이 특정 AMD 칩의 하드웨어
 * 결함 대응이라는 점이 이 파일의 성격을 말해 준다. 2000년대 초 하드웨어를
 * 상대하던 코드가 그대로 남아 있다.
 * 자료구조는 셋이다. struct controller(브리지 하나), struct slot(슬롯 하나),
 * struct event_info(이벤트 하나). 그리고 struct ctrl_reg 가 하드웨어
 * 레지스터 창을 그대로 옮긴 것이며, 그 옆의 enum ctrl_offsets 가 offsetof 로
 * 오프셋을 뽑아낸다 — 상수를 손으로 적지 않아 구조체를 고치면 오프셋도
 * 따라오는 방식이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이벤트 흐름이 이 드라이버의 뼈대다.
 *   하드웨어 인터럽트(또는 shpchp_poll_mode 일 때 poll_timer)
 *     → shpchp_hpc.c 가 INTR_LOC 레지스터로 어느 슬롯인지 알아낸다
 *     → shpchp_handle_{attention_button,switch_change,presence_change,
 *        power_fault}() 중 하나
 *     → shpchp_find_slot() 으로 struct slot 을 찾고
 *     → struct event_info 를 만들어 slot->wq 워크큐에 넣는다
 *     → 워크 함수가 slot->state 를 진행시킨다
 * 버튼 이벤트에는 한 단계가 더 있다. 버튼을 누르면 바로 동작하지 않고
 * BLINKINGON/BLINKINGOFF 상태로 5초를 기다리는데(slot->work 지연 작업),
 * 그 사이 다시 누르면 INT_BUTTON_CANCEL 로 취소된다. 다섯 상태 상수
 * (STATIC / BLINKINGON / BLINKINGOFF / POWERON / POWEROFF)가 그 흐름이다.
 * 반대 방향은 sysfs 다. 사용자가 power 파일을 건드리면 핫플러그 코어가
 * shpchp_core.c 의 콜백을 부르고, get_slot() 으로 struct slot 을 되찾아
 * shpchp_sysfs_enable_slot() / _disable_slot() 으로 이어진다.
 * 실행 컨텍스트가 나뉜다. 인터럽트 핸들러와 shpchp_find_slot() 은 인터럽트
 * 문맥이라 잠들 수 없고, 워크 함수와 sysfs 경로는 프로세스 컨텍스트라
 * 뮤텍스를 잡을 수 있다. 그래서 slot->lock 과 controller 의 두 뮤텍스가
 * 모두 워크·sysfs 쪽에서만 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/hotplug/pci_hotplug_core.c 의 pci_hp_register() 와
 * struct hotplug_slot 규약. struct slot 이 hotplug_slot 을 내장하고
 * get_slot() 이 container_of 로 되돌린다.
 * 아래쪽: PCI config 접근(pci_read/write_config_dword — AMD 쿼크 인라인이
 * 직접 쓴다), ioremap 된 컨트롤러 레지스터 창, 워크큐, 타이머.
 * 옆쪽: 같은 디렉터리의 pciehp 와 대비된다. 둘 다 "슬롯 상태를 읽는 콜백을
 * 코어에 제공한다" 는 큰 틀은 같지만, pciehp 는 컨트롤러 하나에 슬롯 하나라
 * 두 구조체를 합쳐 두었고 SHPC 는 컨트롤러 하나에 여러 슬롯이라 나뉘어 있다.
 * 이름 충돌 주의: struct slot 과 slot_name() 이 acpiphp.h 와 cpci_hotplug.h
 * 에도 있다. 서로 다른 구조체이며, 각 구현 파일이 포함한 헤더가 어느 것을
 * 보게 할지 정한다.
 * 공유 상태: 세 구조체와 전역 셋(shpchp_poll_mode, shpchp_poll_time,
 * shpchp_debug)이다. 세 전역은 shpchp_core.c 의 모듈 파라미터다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct controller: 브리지 하나. 뮤텍스를 **둘** 갖는 것이 특징으로,
 *   crit_sect 는 슬롯 조작 전체를, cmd_lock 은 명령 레지스터 왕복을 지킨다.
 *   slot_num_inc 가 1 또는 -1 인 이유는 보드에 따라 물리 슬롯 번호가
 *   거꾸로 매겨지기 때문이다.
 * - struct slot: 슬롯 하나. 네 개의 _save 필드(attention/presence/latch/pwr)는
 *   인터럽트가 알려 준 상태를 워크 함수가 나중에 쓰기 위해 기억해 두는 것이다.
 *   슬롯마다 워크큐를 따로 두는데, 한 슬롯의 느린 작업이 다른 슬롯의 이벤트를
 *   막지 않게 하려는 것이다. device(PCI 번호)와 hp_slot(컨트롤러 기준 순번)이
 *   다른 값이라는 점에 주의해야 한다.
 * - struct event_info: 이벤트 하나. work 를 구조체 안에 두어 워크 함수가
 *   container_of 로 이벤트 정보를 되찾게 한다.
 * - struct ctrl_reg + enum ctrl_offsets: 레지스터 창을 구조체로 옮기고
 *   offsetof 로 오프셋 표를 만든다. __attribute__((packed)) 가 필수인데,
 *   u16 과 u8 이 섞여 있어 컴파일러가 정렬 빈틈을 넣으면 오프셋이 하드웨어와
 *   어긋나기 때문이다.
 * - get_slot() / slot_name(): 핫플러그 코어와의 경계에서 쓰는 두 래퍼.
 * - shpchp_find_slot(): PCI device 번호로 슬롯을 선형 탐색한다. 못 찾으면
 *   조용히 넘어가지 않고 ctrl_err 로 기록하는데, 하드웨어가 알려 준 슬롯이
 *   목록에 없다는 것 자체가 비정상이기 때문이다.
 * - amd_pogo_errata_save/restore_misc_reg(): AMD Pogo(0x7458) PCI-X 브리지의
 *   결함 대응. 슬롯 조작 중 발생하는 일시적 오류가 시스템 오류로 보고되어
 *   커널을 세우는 것을 막으려고, MiscII 의 다섯 활성화 비트를 끄고 나중에
 *   되살린다. 복원 쪽은 되살리기 **전에** 쌓인 오류 표시(PERR_OBSERVED, RSE)를
 *   W1C 로 지우며, 그 순서가 어긋나면 대응 자체가 무의미해진다.
 *   또 저장값을 통째로 되쓰지 않고 현재 값을 다시 읽어 다섯 비트만 맞추는데,
 *   그 사이 다른 비트가 바뀌었을 수 있기 때문이다.
 * - INT_ 계열 열 개(이벤트 종류), STATIC_STATE 계열 다섯 개(상태 기계),
 *   INTERLOCK_OPEN 계열 아홉 개(SHPC 명령 상태 코드).
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - MY_NAME 매크로는 정의되어 있지만 이 트리의 shpchp 파일들 어디에서도
 *   쓰이지 않는다. 로그가 ctrl_ 매크로로 옮겨 간 흔적으로 보인다.
 * - ctrl_dbg 는 shpchp_debug 전역을 보지 않고 pci_dbg 로 곧장 넘긴다.
 *   즉 디버그 출력은 dynamic debug 로 제어되며 그 모듈 파라미터와 무관하다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. SHPC 는 PCI/PCI-X 시절 규격이고 NVMe 는 PCIe 전용이라,
 * NVMe SSD 가 SHPC 슬롯에 꽂힐 일은 없다. NVMe 의 핫플러그는 pciehp 나
 * acpiphp 가 담당한다.
 * 다만 구조를 비교해 볼 값어치는 있다. SHPC 가 "명령 레지스터에 명령을 쓰고
 * 상태 레지스터로 결과를 받는" 모델인 것은 NVMe 가 제출 큐에 명령을 넣고
 * 완료 큐로 결과를 받는 것과 발상이 같다. 규모와 병렬성이 다를 뿐,
 * 명령·응답을 레지스터가 아닌 메모리로 옮긴 것이 NVMe 의 도약이었다.
 */
#ifndef _SHPCHP_H
#define _SHPCHP_H

/* [한국어] u8/u16/u32 등 고정 폭 정수. */
#include <linux/types.h>
/* [한국어] struct pci_dev 와 config 접근자. 아래 AMD 쿼크 인라인이 직접 쓴다. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 과 hotplug_slot_name(). 아래 struct slot 이
 * hotplug_slot 을 품는 구조라 반드시 필요하다. */
#include <linux/pci_hotplug.h>
/* [한국어] 이 헤더 자체는 쓰지 않지만 이것을 포함하는 구현 파일들이 쓰도록
 * 상류가 여기서 끌어온다. */
#include <linux/delay.h>
/* [한국어] 옆의 영어 주석대로 signal_pending() 과 struct timer_list 때문이다.
 * 아래 controller 의 poll_timer 가 그 타입을 쓴다. */
#include <linux/sched/signal.h>	/* signal_pending(), struct timer_list */
/* [한국어] struct mutex. slot 과 controller 가 각각 갖는다. */
#include <linux/mutex.h>
/* [한국어] struct work_struct / delayed_work / workqueue_struct. */
#include <linux/workqueue.h>

/* [한국어] 내장 빌드이면, */
#if !defined(MODULE)
	/* [한국어] 이름을 문자열 상수로 박는다. */
	#define MY_NAME	"shpchp"
#else
	/* [한국어] 모듈 빌드이면 실행 시점에 모듈 이름을 가져온다.
	 * [상류 코드 관찰] 정의는 남아 있지만 이 트리의 shpchp 파일들에서
	 * MY_NAME 을 쓰는 곳은 없다. 로그가 ctrl_ 매크로로 옮겨 간 흔적이다. */
	#define MY_NAME	THIS_MODULE->name
#endif

/* [한국어] 인터럽트 대신 폴링으로 슬롯 상태를 감시할지.
 * 설정자: shpchp_core.c 의 모듈 파라미터.
 * 읽는 자: shpchp_hpc.c 가 poll_timer 를 띄울지 결정할 때.
 * 값 범위: false = 인터럽트, true = 폴링. 인터럽트가 신뢰할 수 없는
 *   하드웨어를 위한 대비책이다.
 * 동기화: 부팅 시 정해지고 이후 읽기만 한다. */
extern bool shpchp_poll_mode;
/* [한국어] 폴링 간격(초).
 * 설정자: 모듈 파라미터.  읽는 자: poll_timer 를 다시 걸 때.
 * 값 범위: 0 이면 기본값이 쓰인다.
 * 동기화: 위와 같다. */
extern int shpchp_poll_time;
/* [한국어] 디버그 로그를 켤지.
 * 설정자: 모듈 파라미터.  읽는 자: shpchp_core.c.
 * 값 범위: true/false.
 * 동기화: 위와 같다.
 * [상류 코드 관찰] 아래 ctrl_dbg 는 이 값을 보지 않고 pci_dbg 로 곧장
 *   넘긴다. 즉 dynamic debug 로 제어되며 이 파라미터와 무관하다. */
extern bool shpchp_debug;

/* [한국어] 컨트롤러 문맥이 붙은 디버그 로그. ctrl->pci_dev 를 꺼내 pci_dbg 로 넘기므로
 * 메시지 앞에 장치 주소가 자동으로 붙는다. 네 매크로가 같은 형태라,
 * 구현 파일들이 로그 수준만 바꿔 가며 쓴다. */
#define ctrl_dbg(ctrl, format, arg...)					\
	pci_dbg(ctrl->pci_dev, format, ## arg)
/* [한국어] 오류 로그. */
#define ctrl_err(ctrl, format, arg...)					\
	pci_err(ctrl->pci_dev, format, ## arg)
/* [한국어] 정보 로그. */
#define ctrl_info(ctrl, format, arg...)					\
	pci_info(ctrl->pci_dev, format, ## arg)
/* [한국어] 경고 로그. */
#define ctrl_warn(ctrl, format, arg...)					\
	pci_warn(ctrl->pci_dev, format, ## arg)


/* [한국어] 슬롯 이름 버퍼 크기. 아래 struct slot 은 이름을 직접 갖지 않고
 * hotplug_slot 이 들고 있으므로, 이 상수는 shpchp_core.c 가 이름을 만들 때 쓴다. */
#define SLOT_NAME_SIZE 10
struct slot {
	/* [한국어] 이 슬롯이 속한 PCI 버스 번호.
	 * 설정자: shpchp_core.c 의 슬롯 초기화.  읽는 자: 장치 조회 경로.
	 * 값 범위: 0~255.  동기화: 초기화 후 불변. */
	u8 bus;
	/* [한국어] 이 슬롯의 PCI device 번호.
	 * 설정자: 슬롯 초기화.  읽는 자: shpchp_find_slot() 이 이 값으로 슬롯을 찾는다.
	 * 값 범위: 0~31.  동기화: 초기화 후 불변. */
	u8 device;
	/* [한국어] 슬롯 상태 캐시.
	 * 설정자/읽는 자: shpchp_ctrl.c 의 상태 기계.
	 * 값 범위: 드라이버 내부 표현.  동기화: lock 아래에서 다룬다. */
	u16 status;
	/* [한국어] 사용자에게 보일 물리 슬롯 번호.
	 * 설정자: 초기화 시 controller->first_slot 과 slot_num_inc 로 계산한다.
	 * 읽는 자: sysfs 이름 생성.
	 * 값 범위: 보드가 붙인 라벨과 맞춘 값.  동기화: 초기화 후 불변. */
	u32 number;
	/* [한국어] 이 슬롯에 보드가 꽂혀 있는지.
	 * 설정자/읽는 자: shpchp_ctrl.c 의 삽입·제거 처리.
	 * 값 범위: 0/1.  동기화: lock 아래. */
	u8 is_a_board;
	/* [한국어] 슬롯 상태 기계의 현재 상태. 아래 STATIC_STATE 계열 상수 중 하나다.
	 * 설정자/읽는 자: shpchp_ctrl.c 와 sysfs 경로.
	 * 값 범위: STATIC / BLINKINGON / BLINKINGOFF / POWERON / POWEROFF.
	 * 동기화: lock 아래에서만 읽고 쓴다. 버튼을 눌러 5초 대기 중인 상태
	 *   (BLINKING 계열)와 sysfs 요청이 경쟁하므로 이 보호가 필수다. */
	u8 state;
	/* [한국어] attention 표시등 상태의 소프트웨어 사본.
	 * 설정자/읽는 자: 인터럽트 처리와 sysfs.
	 * 값 범위: 0/1.  동기화: lock 아래.
	 * 네 개의 _save 필드는 모두 같은 목적이다 — 인터럽트가 알려 준 값을
	 *   기억해 두어, 나중에 워크큐에서 처리할 때 그 시점의 상태를 쓰려는 것이다. */
	u8 attention_save;
	/* [한국어] 슬롯에 보드가 있는지의 소프트웨어 사본.
	 * 설정자/읽는 자: 위와 같다.  값 범위: 0/1.  동기화: lock 아래. */
	u8 presence_save;
	/* [한국어] 래치(걸쇠) 상태의 소프트웨어 사본.
	 * 설정자/읽는 자: 위와 같다.  값 범위: 0/1.  동기화: lock 아래. */
	u8 latch_save;
	/* [한국어] 전원 상태의 소프트웨어 사본.
	 * 설정자/읽는 자: 위와 같다.  값 범위: 0/1.  동기화: lock 아래. */
	u8 pwr_save;
	/* [한국어] 이 슬롯을 소유한 컨트롤러. 위로 거슬러 올라가는 통로다.
	 * 설정자: 슬롯 초기화.  읽는 자: 거의 모든 함수(ctrl_ 로그 매크로 포함).
	 * 값 범위: 유효한 controller 포인터.  동기화: 초기화 후 불변. */
	struct controller *ctrl;
	/* [한국어] PCI 핫플러그 코어에 등록되는 슬롯. **내장** 이라 아래 get_slot() 의
	 *   container_of 가 성립한다.
	 * 설정자: shpchp_core.c 가 채워 pci_hp_register() 로 넘긴다.
	 * 읽는 자: 코어가 sysfs 접근마다 이 포인터로 콜백을 부른다.
	 * 값 범위: 유효한 hotplug_slot.  동기화: 코어가 등록·해제를 직렬화한다. */
	struct hotplug_slot hotplug_slot;
	/* [한국어] 컨트롤러의 slot_list 에 이 슬롯을 잇는 고리.
	 * 설정자/읽는 자: 슬롯 등록·해제와 shpchp_find_slot() 의 순회.
	 * 값 범위: 리스트에 들어 있거나 비어 있거나.
	 * 동기화: 슬롯 목록은 초기화 시 만들어지고 이후 바뀌지 않는다. */
	struct list_head	slot_list;
	/* [한국어] 옆의 영어 주석대로 버튼 이벤트용 지연 작업. 버튼을 누르면 5초를 기다린 뒤
	 *   실제 동작을 하는데, 그 사이 다시 누르면 취소할 수 있게 하기 위함이다.
	 * 설정자: shpchp_ctrl.c 의 버튼 처리.
	 * 읽는 자: 워크큐가 지연 후 실행한다.
	 * 값 범위: 유효한 delayed_work.  동기화: 워크큐 코어가 중복 실행을 막는다. */
	struct delayed_work work;	/* work for button event */
	/* [한국어] 이 슬롯의 상태를 보호하는 뮤텍스.
	 * 설정자/읽는 자: 상태를 만지는 모든 경로.
	 * 값 범위: 뮤텍스.  잠들 수 있는 락이므로 인터럽트 문맥에서는 쓸 수 없다.
	 * 동기화: controller->crit_sect 와 역할이 다르다 — 이쪽은 슬롯 하나의
	 *   상태를, 저쪽은 컨트롤러 전체의 임계 구역을 지킨다. */
	struct mutex lock;
	/* [한국어] 이 슬롯 전용 워크큐.
	 * 설정자: 슬롯 초기화가 만든다.  읽는 자: 이벤트 처리 경로.
	 * 값 범위: 유효한 workqueue_struct.
	 * 동기화: 슬롯마다 따로 두는 이유는 한 슬롯의 느린 작업이 다른 슬롯의
	 *   이벤트 처리를 막지 않게 하려는 것이다. */
	struct workqueue_struct *wq;
	/* [한국어] 컨트롤러 안에서의 슬롯 인덱스(0부터).
	 * 설정자: 슬롯 초기화.  읽는 자: 하드웨어 레지스터의 슬롯 번호로 쓰인다.
	 * 값 범위: 0 ~ num_slots-1.  device 필드와 달리 PCI 번호가 아니라
	 *   컨트롤러 기준 순번이다.
	 * 동기화: 초기화 후 불변. */
	u8 hp_slot;
};

struct event_info {
	/* [한국어] 어떤 이벤트인지. 아래 INT_ 계열 상수 중 하나다.
	 * 설정자: 인터럽트 처리.  읽는 자: 워크 함수.
	 * 값 범위: INT_BUTTON_IGNORE ~ INT_BUTTON_CANCEL.
	 * 동기화: 이벤트마다 새로 만들어지므로 공유되지 않는다. */
	u32 event_type;
	/* [한국어] 이벤트가 일어난 슬롯.
	 * 설정자/읽는 자: 위와 같다.
	 * 값 범위: 유효한 slot 포인터.  동기화: 위와 같다. */
	struct slot *p_slot;
	/* [한국어] 이 이벤트를 워크큐에 넣기 위한 작업 항목. 이벤트 정보를 구조체에 담고
	 *   work 를 그 안에 두는 것은, 워크 함수가 container_of 로 이벤트 정보를
	 *   되찾게 하려는 흔한 관용구다.
	 * 설정자: 인터럽트 처리가 초기화하고 큐에 넣는다.
	 * 읽는 자: 워크큐 스레드.
	 * 값 범위: 유효한 work_struct.  동기화: 워크큐 코어가 관리한다. */
	struct work_struct work;
};

struct controller {
	/* [한국어] 옆의 영어 주석대로 임계 구역 뮤텍스. 컨트롤러 전체에 걸친 조작을
	 *   직렬화한다.
	 * 설정자/읽는 자: shpchp_ctrl.c 의 슬롯 조작 경로.
	 * 값 범위: 뮤텍스.
	 * 동기화: slot->lock 과 역할이 다르다 — 이쪽은 컨트롤러 단위다. */
	struct mutex crit_sect;		/* critical section mutex */
	/* [한국어] 옆의 영어 주석대로 명령 락. SHPC 는 명령 레지스터에 한 번에 하나의 명령만
	 *   넣을 수 있고 완료를 기다려야 하므로, 그 왕복 전체를 이 락이 감싼다.
	 * 설정자/읽는 자: shpchp_hpc.c 의 명령 실행 경로.
	 * 값 범위: 뮤텍스.
	 * 동기화: crit_sect 와 따로 두는 이유는 잠금 범위가 다르기 때문이다. */
	struct mutex cmd_lock;		/* command lock */
	/* [한국어] 옆의 영어 주석대로 이 컨트롤러가 가진 슬롯 수.
	 * 설정자: shpc_init() 이 SLOT_CONFIG 레지스터에서 읽는다.
	 * 읽는 자: 슬롯 생성 루프.
	 * 값 범위: 하드웨어가 보고한 값.  동기화: 초기화 후 불변. */
	int num_slots;			/* Number of slots on ctlr */
	/* [한국어] 옆의 영어 주석대로 1 또는 -1. 물리 슬롯 번호가 증가하는 방향이다.
	 * 설정자: shpc_init() 이 SLOT_CONFIG 의 방향 비트에서 얻는다.
	 * 읽는 자: 슬롯마다 number 를 계산할 때 first_slot 에 이 값을 더해 나간다.
	 * 값 범위: 1 또는 -1. 보드에 따라 슬롯 번호가 거꾸로 매겨지기 때문이다.
	 * 동기화: 초기화 후 불변. */
	int slot_num_inc;		/* 1 or -1 */
	/* [한국어] 이 컨트롤러 자신인 PCI 장치(브리지).
	 * 설정자: shpc_init().  읽는 자: 모든 config 접근과 ctrl_ 로그 매크로.
	 * 값 범위: 유효한 pci_dev 포인터.  동기화: 초기화 후 불변. */
	struct pci_dev *pci_dev;
	/* [한국어] 이 컨트롤러의 슬롯들을 잇는 리스트 머리.
	 * 설정자: 슬롯 등록.  읽는 자: shpchp_find_slot() 과 cleanup_slots().
	 * 값 범위: num_slots 개의 항목.  동기화: 초기화 후 바뀌지 않는다. */
	struct list_head slot_list;
	/* [한국어] 옆의 영어 주석대로 잠들고 깨우는 대기 큐. 명령 완료를 기다리는 데 쓴다.
	 * 설정자: 인터럽트 처리가 깨운다.  읽는 자: 명령 실행 경로가 잠든다.
	 * 값 범위: 대기 큐.  동기화: 대기 큐 자체가 잠금을 갖는다. */
	wait_queue_head_t queue;	/* sleep & wake process */
	/* [한국어] 첫 슬롯의 PCI device 번호.
	 * 설정자: shpc_init() 이 하드웨어에서 읽는다.
	 * 읽는 자: 슬롯마다 device 번호를 계산할 때.
	 * 값 범위: 0~31.  동기화: 초기화 후 불변. */
	u8 slot_device_offset;
	/* [한국어] 옆의 영어 주석대로 AMD Pogo 칩 오류 대응용 저장 자리. 아래
	 *   amd_pogo_errata_save_misc_reg() 가 MiscII 레지스터를 여기 넣고,
	 *   restore 쪽이 그 값으로 복원한다.
	 * 설정자: amd_pogo_errata_save_misc_reg().
	 * 읽는 자: amd_pogo_errata_restore_misc_reg().
	 * 값 범위: MiscII 레지스터의 32비트 값.
	 * 동기화: 저장과 복원이 한 슬롯 조작 안에서 짝을 이루며, crit_sect 가 감싼다. */
	u32 pcix_misc2_reg;	/* for amd pogo errata */
	/* [한국어] 옆의 영어 주석대로 첫 물리 슬롯 번호. 사용자에게 보일 번호의 기준이다.
	 * 설정자: shpc_init().  읽는 자: 슬롯 number 계산.
	 * 값 범위: 보드 라벨과 맞춘 값.  동기화: 초기화 후 불변. */
	u32 first_slot;		/* First physical slot number */
	/* [한국어] SHPC capability 의 config 공간 오프셋.
	 * 설정자: shpc_init() 이 pci_find_capability() 로 찾는다.
	 * 읽는 자: 하드웨어 레지스터 창의 주소를 계산할 때.
	 * 값 범위: 0 이 아닌 유효한 오프셋.  동기화: 초기화 후 불변. */
	u32 cap_offset;
	/* [한국어] 컨트롤러 레지스터 창의 물리 주소.
	 * 설정자: shpc_init().  읽는 자: ioremap 과 해제.
	 * 값 범위: 하드웨어가 보고한 주소.  동기화: 초기화 후 불변. */
	unsigned long mmio_base;
	/* [한국어] 그 창의 크기.
	 * 설정자/읽는 자: 위와 같다.  동기화: 초기화 후 불변. */
	unsigned long mmio_size;
	/* [한국어] 매핑된 컨트롤러 레지스터 창. 아래 struct ctrl_reg 로 해석한다.
	 * 설정자: shpc_init() 의 ioremap.
	 * 읽는 자: shpchp_hpc.c 의 모든 레지스터 접근.
	 * 값 범위: 유효한 __iomem 포인터.  동기화: 초기화 후 불변. */
	void __iomem *creg;
	/* [한국어] 폴링 모드에서 쓰는 타이머.
	 * 설정자: shpchp_poll_mode 가 켜졌을 때 shpc_init() 이 건다.
	 * 읽는 자: 타이머 콜백이 다시 건다.
	 * 값 범위: 유효한 timer_list.  동기화: 타이머 코어가 관리한다. */
	struct timer_list poll_timer;
};

/* Define AMD SHPC ID  */
/* [한국어] 이 오류 대응이 필요한 AMD 칩의 장치 ID. shpchp_hpc.c 가 이 값과
 * 비교해 아래 두 인라인을 부를지 정한다. */
#define PCI_DEVICE_ID_AMD_POGO_7458	0x7458

/* AMD PCI-X bridge registers */
/* [한국어] AMD PCI-X 브리지의 메모리 베이스/리밋 레지스터 오프셋. */
#define PCIX_MEM_BASE_LIMIT_OFFSET	0x1C
/* [한국어] MiscII 레지스터 오프셋. 오류 보고 활성화 비트들이 여기 있다. */
#define PCIX_MISCII_OFFSET		0x48
/* [한국어] 브리지 오류 레지스터 오프셋. */
#define PCIX_MISC_BRIDGE_ERRORS_OFFSET	0x80

/* AMD PCIX_MISCII masks and offsets */
/* [한국어] 수정 가능한 패리티 오류 보고 활성화 비트. */
#define PERRNONFATALENABLE_MASK		0x00040000
/* [한국어] 치명적 패리티 오류 보고 활성화 비트. */
#define PERRFATALENABLE_MASK		0x00080000
/* [한국어] 패리티 오류 홍수 방지 활성화 비트. */
#define PERRFLOODENABLE_MASK		0x00100000
/* [한국어] 수정 가능한 시스템 오류 보고 활성화 비트. */
#define SERRNONFATALENABLE_MASK		0x00200000
/* [한국어] 치명적 시스템 오류 보고 활성화 비트. 이 다섯 비트를 슬롯 조작 전에
 * 모두 끄고 나중에 되살리는 것이 이 오류 대응의 전부다. */
#define SERRFATALENABLE_MASK		0x00400000

/* AMD PCIX_MISC_BRIDGE_ERRORS masks and offsets */
/* [한국어] 패리티 오류가 관측되었음을 나타내는 비트. W1C 방식이다. */
#define PERR_OBSERVED_MASK		0x00000001

/* AMD PCIX_MEM_BASE_LIMIT masks */
/* [한국어] 메모리 베이스/리밋의 RSE 비트. 역시 W1C 다. */
#define RSE_MASK			0x40000000

/* [한국어] 버튼 이벤트를 무시하라는 표시. 아래 상수들이 event_info.event_type 에 들어간다. */
#define INT_BUTTON_IGNORE		0
/* [한국어] 보드가 꽂혔다. */
#define INT_PRESENCE_ON			1
/* [한국어] 보드가 빠졌다. */
#define INT_PRESENCE_OFF		2
/* [한국어] 래치가 닫혔다. */
#define INT_SWITCH_CLOSE		3
/* [한국어] 래치가 열렸다. */
#define INT_SWITCH_OPEN			4
/* [한국어] 전원 오류가 발생했다. */
#define INT_POWER_FAULT			5
/* [한국어] 전원 오류가 해소되었다. */
#define INT_POWER_FAULT_CLEAR		6
/* [한국어] 버튼이 눌렸다. */
#define INT_BUTTON_PRESS		7
/* [한국어] 버튼에서 손을 뗐다. */
#define INT_BUTTON_RELEASE		8
/* [한국어] 버튼 동작이 취소되었다. 5초 대기 중에 버튼을 다시 누르면 이 이벤트가 된다. */
#define INT_BUTTON_CANCEL		9

/* [한국어] 아무 일도 진행 중이 아닌 상태. 아래 상수들이 slot.state 에 들어간다. */
#define STATIC_STATE			0
/* [한국어] 켜는 중이며 표시등이 깜박이는 상태. 버튼을 누른 뒤 5초 대기 구간이다. */
#define BLINKINGON_STATE		1
/* [한국어] 끄는 중이며 표시등이 깜박이는 상태. */
#define BLINKINGOFF_STATE		2
/* [한국어] 실제로 전원을 넣는 중. */
#define POWERON_STATE			3
/* [한국어] 실제로 전원을 끄는 중. 다섯 상태가 버튼 → 대기 → 실행의 흐름을 나타낸다. */
#define POWEROFF_STATE			4

/* Error messages */
/* [한국어] 래치가 열려 있어 진행할 수 없다. 아래 상수들은 SHPC 규격이 정한
 * 명령 상태 코드로, shpchp_check_cmd_status() 가 사용자에게 보일 메시지로
 * 바꾼다. */
#define INTERLOCK_OPEN			0x00000002
/* [한국어] 이 슬롯에는 추가를 지원하지 않는다. */
#define ADD_NOT_SUPPORTED		0x00000003
/* [한국어] 카드가 정상 동작 중이다. */
#define CARD_FUNCTIONING		0x00000005
/* [한국어] 같은 종류의 어댑터가 아니다. */
#define ADAPTER_NOT_SAME		0x00000006
/* [한국어] 어댑터가 없다. */
#define NO_ADAPTER_PRESENT		0x00000009
/* [한국어] 자원이 부족하다. */
#define NOT_ENOUGH_RESOURCES		0x0000000B
/* [한국어] 지원하지 않는 장치 종류다. */
#define DEVICE_TYPE_NOT_SUPPORTED	0x0000000C
/* [한국어] 버스 주파수가 맞지 않는다. */
#define WRONG_BUS_FREQUENCY		0x0000000D
/* [한국어] 전원 공급에 실패했다. */
#define POWER_FAILURE			0x0000000E

/* [한국어] sysfs 속성을 만든다. __must_check 라 호출자가 반환값을 반드시 확인해야 한다. */
int __must_check shpchp_create_ctrl_files(struct controller *ctrl);
/* [한국어] 그 반대. */
void shpchp_remove_ctrl_files(struct controller *ctrl);
/* [한국어] sysfs 에서 슬롯을 켜는 요청. */
int shpchp_sysfs_enable_slot(struct slot *slot);
/* [한국어] 끄는 요청. */
int shpchp_sysfs_disable_slot(struct slot *slot);
/* [한국어] attention 버튼 인터럽트 처리. 네 handle_ 함수 모두 hp_slot 번호와
 * 컨트롤러를 받아 해당 슬롯을 찾아 처리한다. */
u8 shpchp_handle_attention_button(u8 hp_slot, struct controller *ctrl);
/* [한국어] 래치 변화 인터럽트 처리. */
u8 shpchp_handle_switch_change(u8 hp_slot, struct controller *ctrl);
/* [한국어] 보드 착탈 인터럽트 처리. */
u8 shpchp_handle_presence_change(u8 hp_slot, struct controller *ctrl);
/* [한국어] 전원 오류 인터럽트 처리. */
u8 shpchp_handle_power_fault(u8 hp_slot, struct controller *ctrl);
/* [한국어] 슬롯에 꽂힌 장치를 열거해 커널에 등록한다. */
int shpchp_configure_device(struct slot *p_slot);
/* [한국어] 그 반대. */
void shpchp_unconfigure_device(struct slot *p_slot);
/* [한국어] 모든 슬롯을 정리한다. */
void cleanup_slots(struct controller *ctrl);
/* [한국어] 버튼 지연 작업의 워크 함수. */
void shpchp_queue_pushbutton_work(struct work_struct *work);
/* [한국어] 컨트롤러 하드웨어를 초기화한다. capability 를 찾고 레지스터 창을 매핑하며
 * 슬롯 수와 번호 체계를 읽어 온다. */
int shpc_init(struct controller *ctrl, struct pci_dev *pdev);

/* [한국어]
 * slot_name - 슬롯의 sysfs 이름을 얻는다
 *
 * @slot: shpchp 쪽 슬롯.
 * @return: 핫플러그 코어가 붙여 둔 이름 문자열.
 *
 * 내장된 hotplug_slot 에서 이름을 꺼내는 한 줄 래퍼다. 로그 메시지에서 자주
 * 쓰이는데, 매번 &slot->hotplug_slot 을 적는 것보다 읽기 좋기 때문이다.
 *
 * 이름이 같은 함수가 acpiphp.h 와 cpci_hotplug.h 에도 있다. 서로 다른
 * struct slot 을 다루며, 각 구현 파일이 포함한 헤더에 따라 어느 것이 보이는지가
 * 정해진다.
 *
 * 실행 컨텍스트: 어디서든. static inline 이고 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   shpchp_ctrl.c / shpchp_core.c 의 로그 출력 → [이 함수] → hotplug_slot_name()
 */
static inline const char *slot_name(struct slot *slot)
{
	return hotplug_slot_name(&slot->hotplug_slot);
}

struct ctrl_reg {
	/* [한국어] 레지스터 창의 기준 오프셋.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 base_offset;
	/* [한국어] 사용 가능한 슬롯 정보 1.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 slot_avail1;
	/* [한국어] 사용 가능한 슬롯 정보 2.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 slot_avail2;
	/* [한국어] 슬롯 구성. 슬롯 수, 첫 슬롯 번호, 번호 증가 방향이 여기 들어 있다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 slot_config;
	/* [한국어] 세컨더리 버스 구성. 버스 주파수 설정이 여기 있다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u16 sec_bus_config;
	/* [한국어] MSI 제어.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u8  msi_ctrl;
	/* [한국어] 프로그래밍 인터페이스 버전.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u8  prog_interface;
	/* [한국어] 명령 레지스터. 이 레지스터에 쓰는 것이 컨트롤러에 명령을 내리는 방법이며,
	 *   cmd_lock 이 그 왕복 전체를 감싼다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u16 cmd;
	/* [한국어] 명령 상태. 위 INTERLOCK_OPEN 계열 코드가 여기서 나온다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u16 cmd_status;
	/* [한국어] 인터럽트 발생 위치. 어느 슬롯에서 무슨 일이 있었는지 비트로 알려 준다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 intr_loc;
	/* [한국어] SERR 발생 위치.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 serr_loc;
	/* [한국어] SERR 와 인터럽트 활성화.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 serr_intr_enable;
	/* [한국어] 첫 슬롯의 레지스터. 슬롯마다 이 뒤로 이어지며, hp_slot 인덱스로 접근한다.
	 * 설정자: 하드웨어, 그리고 이 드라이버가 shpchp_writew/writel 로 쓴다.
	 * 읽는 자: shpchp_hpc.c 가 아래 enum ctrl_offsets 의 값으로 오프셋을 얻어 접근한다.
	 * 값 범위: SHPC 규격이 정한 비트 배치. 이 트리에 규격 문서가 없어 확인하지 못했다.
	 * 동기화: 명령 레지스터 접근은 controller->cmd_lock 이, 슬롯 상태 변경은
	 *   crit_sect 가 보호한다. */
	volatile u32 slot1;
/* [한국어] packed 속성이 필수다. 컴파일러가 정렬을 위해 빈틈을 넣으면 아래
 * offsetof 로 계산한 오프셋이 하드웨어의 실제 배치와 어긋난다.
 * u16 과 u8 이 섞여 있어 실제로 그럴 위험이 있다. */
} __attribute__ ((packed));

/* offsets to the controller registers based on the above structure layout */
/* [한국어] 위 영어 주석대로 구조체 배치에서 오프셋을 뽑아낸다. 상수를 손으로
 * 적지 않고 offsetof 로 계산하므로, 구조체를 고치면 오프셋도 함께 따라온다.
 * 구조체와 오프셋 표를 나란히 두는 이 방식은 pcie-spear13xx.c 가 구조체
 * 포인터로 직접 접근하는 방식과 대비된다. */
enum ctrl_offsets {
	/* [한국어] 기준 오프셋. */
	BASE_OFFSET	 = offsetof(struct ctrl_reg, base_offset),
	/* [한국어] 슬롯 정보 1. */
	SLOT_AVAIL1	 = offsetof(struct ctrl_reg, slot_avail1),
	/* [한국어] 슬롯 정보 2. */
	SLOT_AVAIL2	 = offsetof(struct ctrl_reg, slot_avail2),
	/* [한국어] 슬롯 구성. */
	SLOT_CONFIG	 = offsetof(struct ctrl_reg, slot_config),
	/* [한국어] 세컨더리 버스 구성. */
	SEC_BUS_CONFIG	 = offsetof(struct ctrl_reg, sec_bus_config),
	/* [한국어] MSI 제어. */
	MSI_CTRL	 = offsetof(struct ctrl_reg, msi_ctrl),
	/* [한국어] 프로그래밍 인터페이스. */
	PROG_INTERFACE	 = offsetof(struct ctrl_reg, prog_interface),
	/* [한국어] 명령. */
	CMD		 = offsetof(struct ctrl_reg, cmd),
	/* [한국어] 명령 상태. */
	CMD_STATUS	 = offsetof(struct ctrl_reg, cmd_status),
	/* [한국어] 인터럽트 위치. */
	INTR_LOC	 = offsetof(struct ctrl_reg, intr_loc),
	/* [한국어] SERR 위치. */
	SERR_LOC	 = offsetof(struct ctrl_reg, serr_loc),
	/* [한국어] SERR/인터럽트 활성화. */
	SERR_INTR_ENABLE = offsetof(struct ctrl_reg, serr_intr_enable),
	/* [한국어] 첫 슬롯 레지스터. */
	SLOT1		 = offsetof(struct ctrl_reg, slot1),
};

/* [한국어]
 * get_slot - 핫플러그 코어가 넘긴 hotplug_slot 에서 shpchp 슬롯을 되찾는다
 *
 * @hotplug_slot: 코어가 콜백에 넘긴 슬롯 포인터.
 * @return: 그것을 품고 있는 struct slot.
 *
 * PCI 핫플러그 코어는 shpchp 의 자료구조를 모른다. 그래서 struct slot 이
 * hotplug_slot 을 내장하고, 콜백에서 container_of 로 거슬러 올라간다.
 *
 * acpiphp.h 의 to_slot() 과 같은 역할이며 이름만 다르다.
 *
 * 실행 컨텍스트: sysfs 콜백. static inline 이라 호출 비용이 없다.
 *
 * 에러 경로: 없다. 포인터 산술일 뿐이다.
 *
 * 호출 체인:
 *   PCI 핫플러그 코어 → shpchp_core.c 의 sysfs 콜백 → [이 함수] → container_of()
 */
static inline struct slot *get_slot(struct hotplug_slot *hotplug_slot)
{
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/* [한국어]
 * shpchp_find_slot - PCI device 번호로 이 컨트롤러의 슬롯을 찾는다
 *
 * @ctrl: 검색할 컨트롤러.
 * @device: 찾는 슬롯의 PCI device 번호.
 * @return: 찾은 슬롯, 없으면 NULL.
 *
 * 인터럽트가 알려 주는 것은 하드웨어 쪽 슬롯 번호이고, 그것을 소프트웨어
 * 쪽 struct slot 으로 바꾸는 데 쓴다. 리스트를 선형 탐색하는데, 슬롯 수가
 * 많아야 수십 개라 자료구조를 더 얹을 값어치가 없기 때문이다.
 *
 * 못 찾았을 때 ctrl_err 로 기록하는 점이 눈에 띈다. 조용히 NULL 을
 * 돌려주지 않는 것은, 하드웨어가 알려 준 슬롯이 목록에 없다는 것 자체가
 * 정상적인 상황이 아니기 때문이다.
 *
 * slot->device 로 찾는다는 점에 주의할 만하다. slot 에는 hp_slot 이라는
 * 컨트롤러 기준 순번도 있는데, 그쪽은 하드웨어 레지스터 인덱스이고
 * 이 함수가 쓰는 device 는 PCI 번호다.
 *
 * 실행 컨텍스트: 인터럽트 처리와 이벤트 경로. 잠들지 않는다.
 *
 * 에러 경로: NULL 을 받은 호출자가 확인해야 한다.
 *
 * 호출 체인:
 *   shpchp_handle_* 계열 → [이 함수] → list_for_each_entry()
 */
static inline struct slot *shpchp_find_slot(struct controller *ctrl, u8 device)
{
	/* [한국어] 순회 커서. */
	struct slot *slot;

	/* [한국어] 컨트롤러의 슬롯 목록을 훑는다. */
	list_for_each_entry(slot, &ctrl->slot_list, slot_list) {
		/* [한국어] PCI device 번호가 맞으면, */
		if (slot->device == device)
			/* [한국어] 그 슬롯을 돌려준다. */
			return slot;
	}

	/* [한국어] 못 찾으면 오류로 기록한다. 하드웨어가 알려 준 슬롯이 목록에 없다는 뜻이라
	 * 정상적인 상황이 아니다. */
	ctrl_err(ctrl, "Slot (device=0x%02x) not found\n", device);
	/* [한국어] NULL 을 돌려준다. 호출자가 확인해야 한다. */
	return NULL;
}

/* [한국어]
 * amd_pogo_errata_save_misc_reg - AMD Pogo 칩의 오류 보고를 잠시 꺼 둔다
 *
 * @p_slot: 조작할 슬롯. 컨트롤러를 거쳐 브리지 config 에 접근한다.
 *
 * AMD Pogo(장치 ID 0x7458) PCI-X 브리지의 하드웨어 결함 대응이다.
 * 슬롯을 켜고 끄는 동안 일시적인 패리티·시스템 오류가 발생하는데, 그것이
 * 정상 보고 경로를 타면 커널이 심각한 오류로 판단해 시스템을 세울 수 있다.
 *
 * 그래서 조작 전에 MiscII 레지스터의 다섯 활성화 비트(SERR 치명/비치명,
 * PERR 치명/비치명, PERR 홍수 방지)를 모두 끄고, 조작이 끝나면
 * amd_pogo_errata_restore_misc_reg() 가 되살린다.
 *
 * 원래 값을 지역 변수가 아니라 controller->pcix_misc2_reg 에 보관하는 이유는,
 * 저장과 복원이 서로 다른 함수이고 그 사이에 슬롯 조작 전체가 들어가기
 * 때문이다.
 *
 * 실행 컨텍스트: 슬롯 조작 경로. controller->crit_sect 아래에서 불린다.
 *
 * 에러 경로: 없다. config 접근의 실패를 확인하지 않는다.
 *
 * 호출 체인:
 *   shpchp_ctrl.c 의 슬롯 켜기/끄기(AMD Pogo 일 때만) → [이 함수]
 *     → pci_read_config_dword() → pci_write_config_dword()
 */
static inline void amd_pogo_errata_save_misc_reg(struct slot *p_slot)
{
	/* [한국어] 읽어 둘 MiscII 값. */
	u32 pcix_misc2_temp;

	/* save MiscII register */
	/* [한국어] 옆의 영어 주석대로 MiscII 레지스터를 읽는다. */
	pci_read_config_dword(p_slot->ctrl->pci_dev, PCIX_MISCII_OFFSET, &pcix_misc2_temp);

	/* [한국어] 컨트롤러에 원래 값을 보관한다. 복원 쪽이 이 값을 본다. */
	p_slot->ctrl->pcix_misc2_reg = pcix_misc2_temp;

	/* clear SERR/PERR enable bits */
	/* [한국어] 옆의 영어 주석대로 SERR/PERR 활성화 비트를 모두 끈다. */
	pcix_misc2_temp &= ~SERRFATALENABLE_MASK;
	/* [한국어] 수정 가능한 SERR 도. */
	pcix_misc2_temp &= ~SERRNONFATALENABLE_MASK;
	/* [한국어] 패리티 오류 홍수 방지도. */
	pcix_misc2_temp &= ~PERRFLOODENABLE_MASK;
	/* [한국어] 치명적 PERR 도. */
	pcix_misc2_temp &= ~PERRFATALENABLE_MASK;
	/* [한국어] 수정 가능한 PERR 도. 다섯 비트를 모두 끄는 이유는, 슬롯 조작 중에
	 * 발생하는 일시적 오류가 시스템 오류로 보고되어 커널을 세우는 것을
	 * 막기 위해서다. */
	pcix_misc2_temp &= ~PERRNONFATALENABLE_MASK;
	/* [한국어] 끈 값을 되쓴다. */
	pci_write_config_dword(p_slot->ctrl->pci_dev, PCIX_MISCII_OFFSET, pcix_misc2_temp);
}

/* [한국어]
 * amd_pogo_errata_restore_misc_reg - 조작 중 쌓인 오류를 지우고 보고를 되살린다
 *
 * @p_slot: 조작이 끝난 슬롯.
 *
 * save 쪽의 짝이지만 하는 일이 하나 더 많다. 보고를 되살리기 **전에**,
 * 조작 중에 실제로 발생한 오류 표시를 먼저 지운다.
 *
 * 지우는 것은 둘이다. 브리지 오류 레지스터의 PERR_OBSERVED 비트와
 * 메모리 베이스/리밋의 RSE 비트. 둘 다 W1C(1을 쓰면 지워짐) 방식이라
 * 읽은 값을 그대로 되쓰며, 그렇게 하면 다른 비트를 건드리지 않는다.
 * 지우기 전에 각각 로그를 남겨, 실제로 오류가 있었다는 사실이 기록에 남는다.
 *
 * 이 순서가 중요하다. 오류 표시를 남겨 둔 채 보고를 켜면 조작 중의 일시적
 * 오류가 그제야 보고되어, 결함 대응 자체가 무의미해진다.
 *
 * 복원 방식도 세심하다. 저장해 둔 값을 통째로 되쓰지 않고, 현재 값을 다시
 * 읽어 다섯 비트만 하나씩 원래대로 맞춘다. 그 사이 다른 비트가 바뀌었을 수
 * 있기 때문이다. 그래서 다섯 개의 if/else 쌍이 늘어서 있다.
 *
 * 실행 컨텍스트: 슬롯 조작 경로. controller->crit_sect 아래에서 불린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   shpchp_ctrl.c 의 슬롯 켜기/끄기 마무리(AMD Pogo 일 때만) → [이 함수]
 *     → pci_read/write_config_dword()(브리지 오류, 메모리 베이스, MiscII)
 */
static inline void amd_pogo_errata_restore_misc_reg(struct slot *p_slot)
{
	/* [한국어] 읽어 올 MiscII 값. */
	u32 pcix_misc2_temp;
	/* [한국어] 브리지 오류 레지스터 값. */
	u32 pcix_bridge_errors_reg;
	/* [한국어] 메모리 베이스/리밋 값. */
	u32 pcix_mem_base_reg;
	/* [한국어] 패리티 오류가 관측되었는지. */
	u8  perr_set;
	/* [한국어] RSE 가 서 있는지. */
	u8  rse_set;

	/* write-one-to-clear Bridge_Errors[ PERR_OBSERVED ] */
	/* [한국어] 옆의 영어 주석대로 브리지 오류 레지스터를 읽는다. */
	pci_read_config_dword(p_slot->ctrl->pci_dev, PCIX_MISC_BRIDGE_ERRORS_OFFSET, &pcix_bridge_errors_reg);
	/* [한국어] 관측 비트만 추린다. */
	perr_set = pcix_bridge_errors_reg & PERR_OBSERVED_MASK;
	/* [한국어] 서 있으면, */
	if (perr_set) {
		/* [한국어] 어떤 값이었는지 디버그 로그에 남기고, */
		ctrl_dbg(p_slot->ctrl,
			 "Bridge_Errors[ PERR_OBSERVED = %08X] (W1C)\n",
			 perr_set);

		/* [한국어] 읽은 비트를 그대로 되써서 지운다. W1C(1을 쓰면 지워짐) 방식이라
		 * 다른 비트를 건드리지 않는다. */
		pci_write_config_dword(p_slot->ctrl->pci_dev, PCIX_MISC_BRIDGE_ERRORS_OFFSET, perr_set);
	}

	/* write-one-to-clear Memory_Base_Limit[ RSE ] */
	/* [한국어] 옆의 영어 주석대로 메모리 베이스/리밋을 읽는다. */
	pci_read_config_dword(p_slot->ctrl->pci_dev, PCIX_MEM_BASE_LIMIT_OFFSET, &pcix_mem_base_reg);
	/* [한국어] RSE 비트만 추린다. */
	rse_set = pcix_mem_base_reg & RSE_MASK;
	/* [한국어] 서 있으면, */
	if (rse_set) {
		/* [한국어] 기록하고, */
		ctrl_dbg(p_slot->ctrl, "Memory_Base_Limit[ RSE ] (W1C)\n");

		/* [한국어] W1C 로 지운다. */
		pci_write_config_dword(p_slot->ctrl->pci_dev, PCIX_MEM_BASE_LIMIT_OFFSET, rse_set);
	}
	/* restore MiscII register */
	/* [한국어] 옆의 영어 주석대로 MiscII 를 다시 읽는다. 저장해 둔 값을 통째로
	 * 되쓰지 않고 현재 값을 읽어 비트만 고치는 이유는, 그 사이 다른 비트가
	 * 바뀌었을 수 있기 때문이다. */
	pci_read_config_dword(p_slot->ctrl->pci_dev, PCIX_MISCII_OFFSET, &pcix_misc2_temp);

	/* [한국어] 저장해 둔 값에서 치명적 SERR 이 켜져 있었으면, */
	if (p_slot->ctrl->pcix_misc2_reg & SERRFATALENABLE_MASK)
		/* [한국어] 켜고, */
		pcix_misc2_temp |= SERRFATALENABLE_MASK;
	else
		/* [한국어] 아니면 끈다. 아래 네 쌍이 모두 같은 형태로, 다섯 비트를 하나씩
		 * 원래대로 되돌린다. */
		pcix_misc2_temp &= ~SERRFATALENABLE_MASK;

	/* [한국어] 수정 가능한 SERR. */
	if (p_slot->ctrl->pcix_misc2_reg & SERRNONFATALENABLE_MASK)
		/* [한국어] 켜기. */
		pcix_misc2_temp |= SERRNONFATALENABLE_MASK;
	else
		/* [한국어] 끄기. */
		pcix_misc2_temp &= ~SERRNONFATALENABLE_MASK;

	/* [한국어] 패리티 오류 홍수 방지. */
	if (p_slot->ctrl->pcix_misc2_reg & PERRFLOODENABLE_MASK)
		/* [한국어] 켜기. */
		pcix_misc2_temp |= PERRFLOODENABLE_MASK;
	else
		/* [한국어] 끄기. */
		pcix_misc2_temp &= ~PERRFLOODENABLE_MASK;

	/* [한국어] 치명적 PERR. */
	if (p_slot->ctrl->pcix_misc2_reg & PERRFATALENABLE_MASK)
		/* [한국어] 켜기. */
		pcix_misc2_temp |= PERRFATALENABLE_MASK;
	else
		/* [한국어] 끄기. */
		pcix_misc2_temp &= ~PERRFATALENABLE_MASK;

	/* [한국어] 수정 가능한 PERR. */
	if (p_slot->ctrl->pcix_misc2_reg & PERRNONFATALENABLE_MASK)
		/* [한국어] 켜기. */
		pcix_misc2_temp |= PERRNONFATALENABLE_MASK;
	else
		/* [한국어] 끄기. */
		pcix_misc2_temp &= ~PERRNONFATALENABLE_MASK;
	/* [한국어] 다섯 비트를 모두 복원한 값을 한 번에 되쓴다. */
	pci_write_config_dword(p_slot->ctrl->pci_dev, PCIX_MISCII_OFFSET, pcix_misc2_temp);
}

/* [한국어] 슬롯에 전원을 넣는다. 아래 선언들은 모두 shpchp_hpc.c 가 정의하는
 * 하드웨어 조작 함수다. */
int shpchp_power_on_slot(struct slot *slot);
/* [한국어] 슬롯을 활성화한다. */
int shpchp_slot_enable(struct slot *slot);
/* [한국어] 슬롯을 비활성화한다. */
int shpchp_slot_disable(struct slot *slot);
/* [한국어] 버스 속도를 설정한다. */
int shpchp_set_bus_speed_mode(struct slot *slot, enum pci_bus_speed speed);
/* [한국어] 전원 상태를 읽는다. */
int shpchp_get_power_status(struct slot *slot, u8 *status);
/* [한국어] attention 표시등 상태를 읽는다. */
int shpchp_get_attention_status(struct slot *slot, u8 *status);
/* [한국어] attention 표시등을 설정한다. */
int shpchp_set_attention_status(struct slot *slot, u8 status);
/* [한국어] 래치 상태를 읽는다. */
int shpchp_get_latch_status(struct slot *slot, u8 *status);
/* [한국어] 어댑터 존재 여부를 읽는다. */
int shpchp_get_adapter_status(struct slot *slot, u8 *status);
/* [한국어] 어댑터가 지원하는 속도를 읽는다. */
int shpchp_get_adapter_speed(struct slot *slot, enum pci_bus_speed *speed);
/* [한국어] 프로그래밍 인터페이스 버전을 읽는다. */
int shpchp_get_prog_int(struct slot *slot, u8 *prog_int);
/* [한국어] 전원 오류가 있었는지 확인한다. */
int shpchp_query_power_fault(struct slot *slot);
/* [한국어] 녹색 표시등을 켠다. */
void shpchp_green_led_on(struct slot *slot);
/* [한국어] 끈다. */
void shpchp_green_led_off(struct slot *slot);
/* [한국어] 깜박이게 한다. 세 함수가 나뉘어 있는 것은 상태 기계의 세 국면
 * (켜짐/꺼짐/대기 중)에 그대로 대응하기 때문이다. */
void shpchp_green_led_blink(struct slot *slot);
/* [한국어] 컨트롤러 자원을 해제한다. */
void shpchp_release_ctlr(struct controller *ctrl);
/* [한국어] 명령 상태 레지스터를 읽어 위 INTERLOCK_OPEN 계열 코드를 사람이 읽을
 * 메시지로 바꾼다. */
int shpchp_check_cmd_status(struct controller *ctrl);

/* [한국어] 이중 포함 방지 블록의 끝. */
#endif				/* _SHPCHP_H */
