/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PCI Hot Plug Controller Driver for RPA-compliant PPC64 platform.
 *
 * Copyright (C) 2003 Linda Xie <lxie@us.ibm.com>
 *
 * All rights reserved.
 *
 * Send feedback to <lxie@us.ibm.com>,
 *
 */

/* [한국어] 이 헤더가 한 번역 단위에 두 번 펼쳐지는 것을 막는 가드. 이름이 파일명과
 * 다른 것은 예전 파일명(ppc64php)의 흔적이다. */
/*
 * [한국어 설명] RPA 핫플러그 드라이버의 공용 정의 (rpaphp.h)
 *
 * === 파일의 역할 ===
 * IBM POWER 의 RPA(RS/6000 Platform Architecture) 규격을 따르는 PPC64
 * 플랫폼용 PCI 핫플러그 드라이버가 공유하는 정의를 모아 둔 헤더다. 함수 구현은
 * 없고(인라인 하나 제외) 상수·구조체·로그 매크로·함수 선언만 담는다.
 * 담고 있는 것은 네 묶음이다. (1) RTAS 토큰과 값 — DR_INDICATOR/DR_ENTITY_SENSE
 * 같은 펌웨어 서비스 토큰, POWER_ON/OFF, LED 상태 네 가지, 센서 값 EMPTY/PRESENT.
 * (2) 로그 매크로 dbg/err/info/warn — 모두 MY_NAME 접두사를 붙이고, dbg 만
 * 전역 플래그로 켜고 끈다. (3) struct slot — 물리 슬롯 하나를 나타내며 DRC
 * 정보, 상태, 그리고 공용 코어가 요구하는 hotplug_slot 을 값으로 내장한다.
 * (4) 세 소스 파일이 서로 부르는 함수들의 선언.
 * 눈에 띄는 점 하나: EMPTY 0 이 센서 값(:31)과 슬롯 상태(:51) 두 자리에
 * 정의되어 있다. 치환 내용이 같아 C 규격상 무해한 재정의이지만, 서로 다른 두
 * 값 공간이 한 이름을 공유한다는 사실은 읽을 때 주의가 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IBM POWER 의 PCI 핫플러그 스택은 세 층이다. 위에는 sysfs 슬롯 인터페이스를
 * 제공하는 공용 코어 pci_hotplug_core.c 가 있고, 아래에는 RTAS(Run-Time
 * Abstraction Services)라는 펌웨어 호출 인터페이스가 있다. rpaphp 는 그 사이에서
 * 공용 코어의 콜백을 RTAS 호출로 번역하는 어댑터다. 다른 플랫폼의 핫플러그
 * 드라이버가 MMIO 레지스터를 직접 두드리는 것과 달리, 이 드라이버는 하드웨어
 * 레지스터를 하나도 만지지 않는다 — 슬롯 전원도, LED 도, 카드 유무 감지도
 * 전부 펌웨어에 요청한다. 슬롯 정보의 출처도 config space 가 아니라 디바이스
 * 트리이며, 각 슬롯은 DRC(Dynamic Reconfiguration Connector)라는 논리
 * 식별자로 구분된다.
 * 드라이버는 네 파일로 나뉜다 — rpaphp_core.c(모듈 진입점, sysfs 콜백 구현,
 * DT 순회), rpaphp_slot.c(슬롯 구조체의 생성·등록·해제), rpaphp_pci.c(RTAS
 * 센서 조회와 PCI 장치 열거), 그리고 공용 정의를 담은 rpaphp.h.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. RTAS 호출과 PCI 열거가 잠들 수 있어
 * 인터럽트 문맥에서는 어느 함수도 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci_hotplug.h 의 struct hotplug_slot 과 pci_hp_register/deregister.
 * 이 드라이버의 struct slot 이 hotplug_slot 을 값으로 내장해 to_slot() 이
 * container_of 로 역변환한다.
 * 아래쪽: asm/rtas.h 의 rtas_token()/rtas_call()/rtas_get_sensor()/
 * rtas_get_power_level()/rtas_set_power_level(), asm/pci-bridge.h 의 PCI_DN()
 * 매크로와 pci_find_bus_by_node(), 그리고 EEH 서브시스템(eeh_dev_to_pe,
 * pseries_eeh_init_edev_recursive). 모두 PowerPC 전용이라 이 드라이버는
 * 아키텍처에 강하게 묶여 있다.
 * 옆쪽: drivers/pci/pci.h 의 pci_hp_add_devices() — 핫플러그 전용 열거 경로다.
 * 데이터 흐름: 디바이스 트리의 DRC 정보 → struct slot → 공용 코어의 sysfs 슬롯
 * → 사용자 조작 → RTAS 호출 → 펌웨어 → 하드웨어. 반대 방향으로는 RTAS 센서
 * 값이 slot->state 로, PCI 열거 결과가 slot->bus / pci_devs 로 들어온다.
 * 공유 상태: 전역 리스트 rpaphp_slot_head(정의는 rpaphp_core.c)와 디버그
 * 플래그 rpaphp_debug. 리스트 접근에 락이 없는데, 슬롯 추가·제거가 직렬화된다는
 * 전제 위에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct slot: 물리 슬롯 하나. rpaphp_slot_list(전역 리스트 노드),
 *   state(EMPTY/NOT_CONFIGURED/CONFIGURED/NOT_VALID), index(DRC 인덱스),
 *   type, power_domain(RTAS 전원 도메인), attention_status(LED),
 *   name(DRC 이름의 복사본), dn(DT 노드 참조), bus, pci_devs,
 *   그리고 값으로 내장된 hotplug_slot.
 * - to_slot(): 공용 코어가 주는 hotplug_slot 포인터에서 struct slot 을 되찾는
 *   유일한 인라인 함수. hotplug_slot 이 내장이라 container_of 가 성립한다.
 * - dbg/err/info/warn: 로그 매크로 네 개. dbg 만 rpaphp_debug 로 제어되며,
 *   do-while(0) 과 ## arg 라는 두 가지 커널 매크로 관용구를 쓴다.
 * - rpaphp_enable_slot() / rpaphp_get_sensor_state(): rpaphp_pci.c 구현.
 * - rpaphp_add_slot() / rpaphp_check_drc_props(): rpaphp_core.c 구현.
 * - alloc_slot_struct() / dealloc_slot_struct() / rpaphp_register_slot() /
 *   rpaphp_deregister_slot(): rpaphp_slot.c 구현.
 * - 상수 묶음: DR_* 토큰, POWER_*, LED_*, 센서 값, 슬롯 상태, MAX_DRC_NAME_LEN.
 */

#ifndef _PPC64PHP_H
/* [한국어] 가드 매크로 정의. */
#define _PPC64PHP_H

/* [한국어] struct pci_bus, PCI_SLOT() 등 PCI 코어 정의. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 과 hotplug_slot_ops, pci_hp_register/deregister 선언.
 * 이 드라이버가 PCI 핫플러그 공용 코어에 붙는 통로다. */
#include <linux/pci_hotplug.h>

/* [한국어] RTAS(Run-Time Abstraction Services) 인디케이터 토큰. RTAS 는 IBM POWER
 * 펌웨어가 제공하는 호출 인터페이스로, 슬롯 LED 같은 하드웨어 제어를
 * OS 가 직접 하지 않고 펌웨어에 요청한다. 9002 는 "DR(Dynamic Reconfiguration)
 * 인디케이터"를 가리키는 규격 정의 토큰이다. */
#define DR_INDICATOR 9002
/* [한국어] DR 엔티티 감지 센서 토큰(9003). 슬롯에 카드가 꽂혀 있는지를 이 센서로 묻는다.
 * MMIO 레지스터가 아니라 펌웨어 호출로 상태를 얻는 것이 이 플랫폼의 특징이다. */
#define DR_ENTITY_SENSE 9003

/* [한국어] 슬롯 전원 인가를 뜻하는 값. RTAS 전원 도메인 호출에 넘긴다. */
#define POWER_ON	100
/* [한국어] 슬롯 전원 차단. */
#define POWER_OFF	0

/* [한국어] LED 끔. */
#define LED_OFF		0
/* [한국어] LED 계속 켬(옆의 상류 주석 근거). */
#define LED_ON		1	/* continuous on */
/* [한국어] LED 느린 깜빡임 — 슬롯 식별용(옆의 상류 주석). */
#define LED_ID		2	/* slow blinking */
/* [한국어] LED 빠른 깜빡임 — 작업 진행 중 표시(옆의 상류 주석). */
#define LED_ACTION	3	/* fast blinking */

/* Sensor values from rtas_get-sensor */
/* [한국어] DR_ENTITY_SENSE 센서가 돌려주는 값 — 슬롯이 비어 있음(옆의 상류 주석). */
#define EMPTY           0	/* No card in slot */
/* [한국어] 센서 값 — 카드가 꽂혀 있음(옆의 상류 주석). */
#define PRESENT         1	/* Card in slot */

/* [한국어] 모든 로그 메시지 앞에 붙일 드라이버 이름. */
#define MY_NAME "rpaphp"
/* [한국어] 디버그 로그를 켤지 정하는 전역 플래그. 정의는 rpaphp_core.c 에 있고
 * 모듈 파라미터로 노출된다. */
extern bool rpaphp_debug;
/* [한국어] 조건부 디버그 로그 매크로. do-while(0) 로 감싸는 것은 if 문 뒤에
 * 괄호 없이 써도 안전하게 하기 위한 커널 관용구다.
 * ## arg 는 가변 인자가 비었을 때 앞의 쉼표를 지우는 GNU 확장이다. */
#define dbg(format, arg...)					\
	do {							\
		if (rpaphp_debug)				\
			printk(KERN_DEBUG "%s: " format,	\
				MY_NAME, ## arg);		\
	} while (0)
/* [한국어] 오류 로그 — 조건 없이 항상 출력한다. */
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
/* [한국어] 정보 로그. */
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
/* [한국어] 경고 로그. 세 매크로 모두 MY_NAME 접두사를 붙여 dmesg 에서 구분되게 한다. */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)

/* slot states */

/* [한국어] 슬롯 상태 — 유효하지 않음(DRC 정보가 잘못됐거나 슬롯이 사라진 경우). */
#define	NOT_VALID	3
/* [한국어] 슬롯 상태 — 카드는 있으나 아직 PCI 구성이 되지 않음. */
#define	NOT_CONFIGURED	2
/* [한국어] 슬롯 상태 — 카드가 있고 구성까지 완료됨. */
#define	CONFIGURED	1
/* [한국어] 슬롯 상태 — 비어 있음.
 * [상류 코드 관찰] 위 :31 에서 센서 값으로 이미 EMPTY 0 을 정의했다.
 * 치환 내용이 완전히 같아 C 규격상 허용되는 무해한 재정의이고 경고도 나지 않지만,
 * "센서 값"과 "슬롯 상태"라는 서로 다른 두 값 공간이 같은 이름을 공유한다는
 * 점은 읽는 사람에게 혼동을 줄 수 있다. */
#define	EMPTY		0

/* DRC constants */

/* [한국어] DRC(Dynamic Reconfiguration Connector) 이름의 최대 길이.
 * DRC 는 펌웨어가 각 슬롯에 붙여 둔 논리 식별자로, 이 드라이버는 그것을
 * 슬롯 이름으로 그대로 쓴다. */
#define MAX_DRC_NAME_LEN 64

/*
 * struct slot - slot information for each *physical* slot
 */
struct slot {
	/* [한국어] 전역 rpaphp_slot_head 리스트에 이 슬롯을 매다는 노드.
	 * 설정자: rpaphp_register_slot() 이 list_add 로 붙이고,
	 *   rpaphp_deregister_slot() 이 list_del 로 뗀다.
	 * 읽는 자: is_registered() 의 중복 검사 순회.
	 * 값 범위: 항상 유효한 리스트 노드.
	 * 동기화: 이 파일에는 락이 없다 — 등록/해제가 직렬화된다는 전제다. */
	struct list_head rpaphp_slot_list;
	/* [한국어] 슬롯 상태(EMPTY / CONFIGURED / NOT_CONFIGURED / NOT_VALID).
	 * 설정자: rpaphp_core.c 와 rpaphp_pci.c 의 상태 갱신 경로.
	 * 읽는 자: 슬롯 활성화/비활성화 판정.
	 * 값 범위: 위 네 상수 중 하나. alloc 시 kzalloc 이라 0(EMPTY)으로 시작한다.
	 * 동기화: 별도 락 없음. */
	int state;
	/* [한국어] 펌웨어가 부여한 DRC 인덱스. 슬롯을 유일하게 식별하는 번호다.
	 * 설정자: alloc_slot_struct() 가 인자로 받아 저장한다.
	 * 읽는 자: rpaphp_register_slot() 이 DT 자식 노드의 ibm,my-drc-index 와
	 *   비교해 슬롯 번호를 알아내는 데 쓴다.
	 * 값 범위: 펌웨어가 정한 값.
	 * 동기화: 설정 후 읽기 전용. */
	u32 index;
	/* [한국어] 슬롯 종류(PCI, PCI-X, PCIe 등을 구분하는 코드).
	 * 설정자: alloc_slot_struct() 가 아니라 호출자(rpaphp_core.c)가 나중에 채운다 —
	 *   할당 시점에는 아직 알 수 없기 때문이다.
	 * 읽는 자: 등록 로그와 슬롯 종류별 처리.
	 * 값 범위: 펌웨어가 정한 값. 채워지기 전에는 0.
	 * 동기화: 설정 후 읽기 전용. */
	u32 type;
	/* [한국어] RTAS 전원 도메인 번호. 슬롯 전원을 켜고 끌 때 펌웨어에 넘기는 식별자다.
	 * 설정자: alloc_slot_struct().
	 * 읽는 자: 전원 제어 경로.
	 * 값 범위: 펌웨어가 정한 값.
	 * 동기화: 설정 후 읽기 전용. */
	u32 power_domain;
	/* [한국어] attention LED 의 현재 상태(LED_OFF/ON/ID/ACTION).
	 * 설정자: 핫플러그 코어의 set_attention_status 콜백.
	 * 읽는 자: get_attention_status 콜백이 하드웨어를 다시 묻지 않고 이 값을 돌려준다.
	 * 값 범위: 위 LED_* 네 값.
	 * 동기화: 별도 락 없음. */
	u8 attention_status;
	/* [한국어] DRC 이름 문자열. 슬롯의 sysfs 이름이 된다.
	 * 설정자: alloc_slot_struct() 가 kstrdup 으로 복사본을 만든다 —
	 *   원본 DT 문자열의 수명에 의존하지 않기 위해서다.
	 * 읽는 자: 등록, 중복 검사, 모든 로그.
	 * 값 범위: NULL 이 아닌 문자열. dealloc 이 kfree 로 해제한다.
	 * 동기화: 설정 후 읽기 전용. */
	char *name;
	/* [한국어] 이 슬롯에 대응하는 디바이스 트리 노드.
	 * 설정자: alloc_slot_struct() 가 of_node_get() 으로 참조를 올려 저장한다.
	 * 읽는 자: 자식 노드 순회로 슬롯 번호를 찾는 경로.
	 * 값 범위: 유효한 노드 포인터.
	 * 동기화: DT 노드 참조 카운트로 관리되며, dealloc 이 of_node_put() 으로 내린다. */
	struct device_node *dn;
	/* [한국어] 이 슬롯 아래의 PCI 버스.
	 * 설정자: 호출자(rpaphp_core.c)가 채운다.
	 * 읽는 자: pci_hp_register() 에 넘겨 sysfs 슬롯을 버스에 연결한다.
	 * 값 범위: 유효 포인터 또는 NULL(아직 버스가 없는 빈 슬롯).
	 * 동기화: 설정 후 읽기 전용. */
	struct pci_bus *bus;
	/* [한국어] 이 슬롯에 속한 PCI 장치들의 리스트 머리를 가리키는 포인터.
	 * 설정자: rpaphp_pci.c 의 구성 경로.
	 * 읽는 자: 슬롯 비활성화 시 장치 제거 순회.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: 별도 락 없음. */
	struct list_head *pci_devs;
	/* [한국어] PCI 핫플러그 공용 코어가 요구하는 슬롯 객체. 포인터가 아니라 내장이라
	 * to_slot() 이 container_of 로 역변환할 수 있다.
	 * 설정자: alloc_slot_struct() 가 ops 를 걸고, pci_hp_register() 가 나머지를 채운다.
	 * 읽는 자: 공용 코어의 sysfs 처리 전부.
	 * 값 범위: 구조체 내장.
	 * 동기화: 공용 코어가 관리한다. */
	struct hotplug_slot hotplug_slot;
};

/* [한국어] 이 드라이버가 공용 코어에 제공하는 콜백 테이블. 정의는 rpaphp_core.c 에 있다. */
extern const struct hotplug_slot_ops rpaphp_hotplug_slot_ops;
/* [한국어] 등록된 모든 슬롯의 전역 리스트 머리. 정의는 rpaphp_core.c 에 있다. */
extern struct list_head rpaphp_slot_head;

/* [한국어]
 * to_slot - 공용 코어의 hotplug_slot 에서 이 드라이버의 struct slot 을 되찾는다
 *
 * @hotplug_slot: PCI 핫플러그 공용 코어가 콜백에 넘겨 주는 포인터.
 * @return: 그것을 감싸고 있는 struct slot 의 주소.
 *
 * 왜 필요한가: 공용 코어(pci_hotplug_core.c)는 드라이버별 구조를 모르고
 * struct hotplug_slot 만 다룬다. 그래서 콜백을 받은 드라이버는 그 포인터에서
 * 자기 문맥으로 되돌아갈 방법이 필요하다. struct slot 이 hotplug_slot 을
 * 포인터가 아니라 값으로 내장하고 있기 때문에 container_of 가 성립한다 —
 * drvdata 같은 별도의 저장소가 필요 없다.
 *
 * 실행 컨텍스트: 공용 코어의 sysfs 콜백 경로. 순수 주소 계산이라 어디서든 안전하다.
 *
 * 에러 경로: 없다. NULL 을 넘기면 잘못된 주소가 나오므로 호출자가 보장해야 한다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → rpaphp_hotplug_slot_ops 의 콜백
 *     → [to_slot] → 이 드라이버의 struct slot */
static inline struct slot *to_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 공용 코어가 주는 hotplug_slot 포인터에서 이 드라이버의 struct slot 을 되찾는다.
	 * hotplug_slot 이 구조체에 내장되어 있어 container_of 가 성립한다. */
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/* function prototypes */

/* rpaphp_pci.c */
/* [한국어] 슬롯에 전원을 넣고 그 아래 PCI 장치를 구성한다. 정의는 rpaphp_pci.c. */
int rpaphp_enable_slot(struct slot *slot);
/* [한국어] DR 엔티티 감지 센서로 카드 유무를 묻는다(EMPTY/PRESENT). 정의는 rpaphp_pci.c. */
int rpaphp_get_sensor_state(struct slot *slot, int *state);

/* rpaphp_core.c */
/* [한국어] DT 노드 하나를 슬롯으로 만들어 등록한다. 정의는 rpaphp_core.c. */
int rpaphp_add_slot(struct device_node *dn);
/* [한국어] DT 노드의 DRC 속성이 기대한 이름·종류와 맞는지 확인한다. 정의는 rpaphp_core.c. */
int rpaphp_check_drc_props(struct device_node *dn, char *drc_name,
		char *drc_type);

/* rpaphp_slot.c */
/* [한국어] 슬롯 구조체와 그 안의 동적 자원(name, dn 참조)을 해제한다. */
void dealloc_slot_struct(struct slot *slot);
/* [한국어] 슬롯 구조체를 만들고 DRC 정보로 채운다. */
struct slot *alloc_slot_struct(struct device_node *dn, int drc_index, char *drc_name, int power_domain);
/* [한국어] 슬롯을 공용 코어와 전역 리스트에 등록한다. */
int rpaphp_register_slot(struct slot *slot);
/* [한국어] 등록을 되돌린다. */
int rpaphp_deregister_slot(struct slot *slot);

/* [한국어] include 가드의 끝. 뒤의 주석은 어떤 #ifndef 에 대응하는지 밝히는 관례다. */
#endif				/* _PPC64PHP_H */
