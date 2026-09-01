// SPDX-License-Identifier: GPL-2.0+
/*
 * RPA Virtual I/O device functions
 * Copyright (C) 2004 Linda Xie <lxie@us.ibm.com>
 *
 * All rights reserved.
 *
 * Send feedback to <lxie@us.ibm.com>
 *
 */
/* [한국어] 커널 공통 정의(-EAGAIN 등 errno). */
/*
 * [한국어 설명] RPA 핫플러그 슬롯 객체의 생성·등록·해제 (rpaphp_slot.c)
 *
 * === 파일의 역할 ===
 * struct slot 하나의 일생을 담당하는 파일이다. 디바이스 트리에서 얻은 DRC
 * 정보로 슬롯 구조체를 만들고(alloc_slot_struct), PCI 핫플러그 공용 코어와
 * 전역 리스트에 등록하고(rpaphp_register_slot), 그 역순으로 떼어 내
 * 해제한다(rpaphp_deregister_slot, dealloc_slot_struct). 하드웨어도 펌웨어도
 * 건드리지 않고 오직 객체 관리만 한다는 점에서, 같은 드라이버의
 * rpaphp_pci.c(RTAS 호출과 PCI 열거)와 역할이 뚜렷이 갈린다.
 * 이 파일의 주제는 "수명 관리"다. DRC 이름을 kstrdup 으로 복사하고 DT 노드를
 * of_node_get 으로 잡아 두는 것은 외부 자원의 수명에 의존하지 않기 위해서이고,
 * 해제 시 그 짝을 정확히 맞춘다. 등록·해제 순서에도 이유가 있다 — 등록은
 * 공용 코어에 성공한 뒤에야 전역 리스트에 넣고, 해제는 리스트에서 먼저 뗀 뒤
 * 공용 코어에서 지우고 마지막에 메모리를 푼다.
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
 * - alloc_slot_struct(): kzalloc 으로 구조체를 만들고 DRC 이름을 복사, DT 노드
 *   참조를 올리고 ops 를 건다. type 과 bus 는 채우지 않아 호출자의 몫으로 남긴다.
 *   실패 되감기가 두 단계(error_slot / error_nomem)인 것은 DT 참조를 올리기
 *   전과 후를 구분하기 위해서다.
 * - dealloc_slot_struct(): of_node_put → kfree(name) → kfree(slot) 순서.
 *   구조체를 마지막에 해제해야 그 안의 포인터를 읽을 수 있다.
 * - is_registered(): 전역 리스트를 선형 탐색해 같은 DRC 이름이 있는지 본다.
 *   인덱스가 아니라 이름으로 비교하는 이유는 이름이 곧 sysfs 디렉토리이기 때문이다.
 * - rpaphp_register_slot(): 중복 검사 → DT 자식에서 슬롯 번호 탐색 →
 *   pci_hp_register() → 전역 리스트 추가.
 * - rpaphp_deregister_slot(): list_del → pci_hp_deregister → dealloc.
 *   언제나 0 을 돌려준다(retval 이 한 번도 바뀌지 않는다).
 * - [상류 코드 관찰, 수정하지 않음] rpaphp_register_slot() 이
 *   of_property_read_u32() 반환값을 retval 에 담아 두고도 검사하지 않는다.
 *   ibm,my-drc-index 속성이 없는 자식 노드를 만나면 my_index 가 갱신되지 않은
 *   채 비교에 쓰이며, 첫 반복에서 그랬다면 초기화되지 않은 스택 값과 비교된다.
 */

#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL 매크로. */
#include <linux/module.h>
/* [한국어] sysfs 관련 정의. 슬롯이 sysfs 에 노출되므로 포함한다. */
#include <linux/sysfs.h>
/* [한국어] of_node_get/of_node_put 과 of_property_read_u32,
 * for_each_child_of_node_scoped 매크로 — 이 플랫폼의 슬롯 정보가 모두
 * 디바이스 트리에 있어 필수다. */
#include <linux/of.h>
/* [한국어] PCI_SLOT() 매크로와 PCI 코어 정의. */
#include <linux/pci.h>
/* [한국어] kstrdup/strcmp 선언. */
#include <linux/string.h>
/* [한국어] kzalloc_obj/kfree 선언. */
#include <linux/slab.h>

/* [한국어] IBM POWER 의 RTAS 펌웨어 인터페이스 헤더. 이 파일에서 직접 RTAS 를 부르지는
 * 않지만 PCI_DN() 매크로 경로가 이 헤더 계열에 의존한다. */
#include <asm/rtas.h>
/* [한국어] 이 드라이버의 자체 헤더 — struct slot, 로그 매크로, 함수 선언. */
#include "rpaphp.h"

/* free up the memory used by a slot */
/* [한국어]
 * dealloc_slot_struct - 슬롯 구조체와 그 안의 동적 자원을 해제한다
 *
 * @slot: 해제할 슬롯. 이미 공용 코어에서 등록 해제되어 있어야 한다.
 *
 * 해제 순서가 핵심이다 — DT 노드 참조를 내리고, 이름 문자열을 free 하고,
 * 마지막에 구조체 자체를 free 한다. 구조체를 먼저 해제하면 그 안의 포인터를
 * 읽을 수 없게 되므로 반대 순서는 성립하지 않는다.
 *
 * of_node_put() 은 alloc_slot_struct() 의 of_node_get() 과 짝이다. 그 짝이
 * 맞지 않으면 디바이스 트리 노드가 영영 해제되지 않는다.
 *
 * 이 함수는 두 곳에서 불린다 — 정상 해제 경로인 rpaphp_deregister_slot(),
 * 그리고 rpaphp_core.c 의 슬롯 추가 실패 되감기.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출 전에 pci_hp_deregister() 가 끝나
 * 있어야 sysfs 접근과 겹치지 않는다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rpaphp_deregister_slot() 또는 rpaphp_add_slot() 실패 경로
 *     → [dealloc_slot_struct] → of_node_put() / kfree() ×2
 */
void dealloc_slot_struct(struct slot *slot)
{
	/* [한국어] DT 노드 참조를 내린다. alloc 에서 of_node_get() 으로 올린 것의 짝이다.
	 * 이 짝이 맞지 않으면 DT 노드가 영영 해제되지 않는다. */
	of_node_put(slot->dn);
	/* [한국어] kstrdup 으로 복사해 둔 이름 문자열을 해제한다. */
	kfree(slot->name);
	/* [한국어] 슬롯 구조체 자체를 해제한다. 해제 순서가 중요하다 — 구조체를 먼저 free 하면
	 * 그 안의 포인터를 읽을 수 없게 된다. */
	kfree(slot);
}

/* [한국어]
 * alloc_slot_struct - DRC 정보로 슬롯 구조체를 만든다
 *
 * @dn: 이 슬롯에 대응하는 디바이스 트리 노드.
 * @drc_index: 펌웨어가 부여한 DRC 인덱스 — 슬롯을 유일하게 식별한다.
 * @drc_name: DRC 이름. 슬롯의 sysfs 이름이 되며, 이 함수가 복사본을 만든다.
 * @power_domain: RTAS 전원 도메인 번호. 슬롯 전원 제어에 쓴다.
 * @return: 완성된 슬롯 포인터, 또는 NULL(메모리 부족).
 *
 * 왜 이름을 복사하는가: 원본 문자열이 디바이스 트리나 호출자 스택에 있을 수
 * 있어 수명을 신뢰할 수 없다. kstrdup 으로 복사해 두면 슬롯의 수명과
 * 문자열의 수명이 일치하게 되고, dealloc 이 그것을 해제한다.
 * 같은 이유로 DT 노드도 of_node_get() 으로 참조를 올려 잡아 둔다.
 *
 * kzalloc 을 쓰는 것도 의도적이다. 이 함수가 채우지 않는 필드
 * (state, type, bus, pci_devs, attention_status)가 모두 0 으로 시작하고,
 * 그중 state 의 0 은 마침 EMPTY 라는 올바른 초기 상태다.
 *
 * 주의: type 과 bus 는 이 함수가 채우지 않는다. 호출자인 rpaphp_core.c 가
 * 슬롯 종류를 판별하고 버스를 찾은 뒤 직접 채워야 등록이 올바르게 동작한다.
 *
 * 실행 컨텍스트: 슬롯 추가 경로, 프로세스 컨텍스트. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 에러 경로: 두 단계 되감기다. 이름 복사가 실패하면 구조체만 해제하고(error_slot),
 * 구조체 할당이 실패하면 그대로 NULL 을 돌려준다(error_nomem).
 * 이름 복사 실패 시점에는 아직 of_node_get() 을 하지 않았으므로
 * DT 참조를 내릴 필요가 없다 — 라벨이 두 개인 이유가 그것이다.
 *
 * 호출 체인:
 *   rpaphp_add_slot() (rpaphp_core.c) → [alloc_slot_struct]
 *     → kzalloc_obj() / kstrdup() / of_node_get()
 */
struct slot *alloc_slot_struct(struct device_node *dn,
		int drc_index, char *drc_name, int power_domain)
{
	/* [한국어] 할당할 슬롯 구조체. */
	struct slot *slot;

	/* [한국어] 구조체를 0 으로 초기화해 할당한다. kzalloc 인 덕분에 state 가 EMPTY(0)로,
	 * 채우지 않는 필드(type, bus, pci_devs)가 모두 0 으로 시작한다. */
	slot = kzalloc_obj(struct slot);
	/* [한국어] 메모리 부족. */
	if (!slot)
		/* [한국어] 아직 아무것도 잡지 않았으므로 곧장 NULL 반환 구간으로. */
		goto error_nomem;
	/* [한국어] DRC 이름의 복사본을 만든다. 원본이 DT 나 호출자 스택에 있을 수 있어
	 * 수명을 이 구조체에 종속시키기 위해 복사한다. */
	slot->name = kstrdup(drc_name, GFP_KERNEL);
	/* [한국어] 복사 실패. */
	if (!slot->name)
		/* [한국어] 이미 할당한 구조체를 해제하는 구간으로. */
		goto error_slot;
	/* [한국어] DT 노드 참조를 올려 저장한다. 이 참조 덕분에 노드가 우리보다 먼저 사라지지 않는다. */
	slot->dn = of_node_get(dn);
	/* [한국어] 펌웨어가 준 DRC 인덱스를 기록한다. */
	slot->index = drc_index;
	/* [한국어] RTAS 전원 도메인 번호를 기록한다. */
	slot->power_domain = power_domain;
	/* [한국어] 공용 코어가 부를 콜백 테이블을 건다. 이 대입이 있어야 pci_hp_register() 가
	 * 슬롯을 받아들인다.
	 * 주의: type 과 bus 는 여기서 채우지 않는다 — 호출자(rpaphp_core.c)가
	 * 나중에 채워야 등록이 올바르게 동작한다. */
	slot->hotplug_slot.ops = &rpaphp_hotplug_slot_ops;

	/* [한국어] 완성된 슬롯을 돌려준다. 괄호는 불필요하지만 상류 그대로 둔다. */
	return (slot);

/* [한국어] 이름 복사 실패 전용 정리 라벨. */
error_slot:
	/* [한국어] 구조체만 해제한다. dn 참조는 아직 올리지 않았으므로 내릴 것이 없다. */
	kfree(slot);
/* [한국어] 구조체 할당 실패가 곧장 도달하는 라벨. */
error_nomem:
	/* [한국어] 실패를 NULL 로 알린다. */
	return NULL;
}

/* [한국어]
 * is_registered - 같은 이름의 슬롯이 이미 등록되어 있는지 확인한다
 *
 * @slot: 검사할 슬롯.
 * @return: 1 = 같은 이름이 이미 있음, 0 = 없음.
 *
 * 인덱스가 아니라 DRC 이름으로 비교하는 것이 핵심이다. 이름은 사용자에게
 * 보이는 식별자이자 sysfs 디렉토리 이름이므로, 같은 이름이 둘이면 sysfs
 * 등록 자체가 실패한다. 그 실패를 미리 걸러 내는 것이 이 함수의 목적이다.
 *
 * 전역 리스트를 선형 탐색한다. 슬롯 수가 수십 개 수준이라 성능이 문제되지 않는다.
 *
 * [상류 코드 관찰] 순회 중 락을 잡지 않는다. 등록과 해제가 직렬화된다는
 * 전제 위에 있으며, 실제로 두 경로 모두 슬롯 추가/제거라는 드문 사건이다.
 *
 * 실행 컨텍스트: rpaphp_register_slot() 안, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rpaphp_register_slot() → [is_registered] → list_for_each_entry()
 */
static int is_registered(struct slot *slot)
{
	/* [한국어] 순회용 임시 포인터. */
	struct slot *tmp_slot;

	/* [한국어] 이미 등록된 모든 슬롯을 순회한다. */
	list_for_each_entry(tmp_slot, &rpaphp_slot_head, rpaphp_slot_list) {
		/* [한국어] DRC 이름이 같으면 같은 슬롯이다. 인덱스가 아니라 이름으로 비교하는 이유는
		 * 이름이 사용자에게 보이는 식별자이고 sysfs 에서 충돌하면 안 되기 때문이다. */
		if (!strcmp(tmp_slot->name, slot->name))
			/* [한국어] 중복 발견. */
			return 1;
	}
	/* [한국어] 중복 없음. */
	return 0;
}

/* [한국어]
 * rpaphp_deregister_slot - 슬롯을 공용 코어와 전역 리스트에서 떼고 해제한다
 *
 * @slot: 제거할 슬롯.
 * @return: 언제나 0.
 *       [상류 코드 관찰] retval 을 0 으로 초기화한 뒤 한 번도 바꾸지 않는다.
 *       반환형이 int 인 것은 register 쪽과의 대칭을 위한 것으로 보이며,
 *       실제로 실패할 수 있는 단계가 없다.
 *
 * 순서가 이 함수의 전부다.
 *   1) 전역 리스트에서 뗀다 — 이후 is_registered() 가 이 슬롯을 보지 못한다.
 *   2) pci_hp_deregister() 로 공용 코어에서 등록을 해제한다. sysfs 엔트리가
 *      사라지고, 진행 중인 sysfs 접근이 끝날 때까지 코어가 기다려 준다.
 *   3) 그제야 구조체를 해제한다.
 * 2)와 3)의 순서를 바꾸면 사용자가 sysfs 파일을 읽는 도중 그 뒤의 슬롯
 * 구조체가 사라져 use-after-free 가 된다.
 *
 * 실행 컨텍스트: 슬롯 제거 경로, 프로세스 컨텍스트. pci_hp_deregister() 가
 * 진행 중인 접근을 기다리므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rpaphp_core.c 의 슬롯 제거 경로 → [rpaphp_deregister_slot]
 *     → list_del() → pci_hp_deregister() → dealloc_slot_struct()
 */
int rpaphp_deregister_slot(struct slot *slot)
{
	/* [한국어] [상류 코드 관찰] 0 으로 초기화한 뒤 한 번도 바뀌지 않아 항상 0 을 돌려준다.
	 * 반환형이 int 인 것은 대칭을 위한 것으로 보이며, 실제로 실패할 수 있는
	 * 단계가 없다. */
	int retval = 0;
	/* [한국어] 공용 코어에 넘길 내장 hotplug_slot 의 주소. */
	struct hotplug_slot *php_slot = &slot->hotplug_slot;

	 /* [한국어] 진입 로그. 앞의 여분 공백은 상류 그대로다. */
	 dbg("%s - Entry: deregistering slot=%s\n",
		__func__, slot->name);

	/* [한국어] 먼저 전역 리스트에서 뗀다. 이후 is_registered() 가 이 슬롯을 보지 못한다. */
	list_del(&slot->rpaphp_slot_list);
	/* [한국어] 공용 코어에서 등록을 해제한다 — sysfs 엔트리가 사라지고, 진행 중인
	 * sysfs 접근이 끝날 때까지 기다려 준다. 그래서 이 호출 뒤에야 구조체를
	 * 해제해도 안전하다. */
	pci_hp_deregister(php_slot);
	/* [한국어] 구조체와 그 안의 자원을 해제한다. 순서가 핵심이다 — 등록 해제가 먼저다. */
	dealloc_slot_struct(slot);

	/* [한국어] 종료 로그. */
	dbg("%s - Exit: rc[%d]\n", __func__, retval);
	/* [한국어] 언제나 0 이다(위 관찰 참조). */
	return retval;
}
EXPORT_SYMBOL_GPL(rpaphp_deregister_slot);

/* [한국어]
 * rpaphp_register_slot - 슬롯을 공용 코어와 전역 리스트에 등록한다
 *
 * @slot: 등록할 슬롯. dn / index / name / bus 가 이미 채워져 있어야 한다.
 * @return: 0 = 성공. -EAGAIN = 같은 이름의 슬롯이 이미 등록됨.
 *       그 밖의 음수 = pci_hp_register() 실패.
 *
 * 동작 과정:
 *   1) 등록하려는 슬롯의 모든 정보를 디버그 로그로 남긴다. %pOF 는 디바이스
 *      트리 노드의 전체 경로를 찍는 커널 전용 서식이다.
 *   2) 같은 이름이 이미 등록되어 있으면 -EAGAIN 으로 거절한다.
 *   3) 슬롯 노드의 자식들을 순회하며 ibm,my-drc-index 가 이 슬롯의 인덱스와
 *      일치하는 노드를 찾아, 그 devfn 에서 슬롯 번호를 뽑는다.
 *      _scoped 판 매크로라 break 로 빠져나가도 of_node_put() 이 자동으로 불린다.
 *   4) pci_hp_register() 로 공용 코어에 등록한다. 이 시점부터 sysfs 에 슬롯
 *      디렉토리가 생기고 사용자가 power/attention 파일을 쓸 수 있다.
 *   5) 성공했을 때만 전역 리스트에 넣는다. 순서를 바꾸면 등록 실패 시
 *      리스트에 유령 항목이 남는다.
 *
 * [상류 코드 관찰, 수정하지 않음] 3)의 of_property_read_u32() 반환값을
 * retval 에 담아 두고도 검사하지 않는다. 속성이 없는 자식 노드를 만나면
 * my_index 가 갱신되지 않은 채 비교에 쓰이며, 첫 반복에서 그랬다면
 * 초기화되지 않은 스택 값과 비교하게 된다. 그 결과 엉뚱한 자식에서 슬롯
 * 번호를 가져오거나, 끝내 못 찾아 slotno 가 -1 인 채로 4)에 넘어간다.
 *
 * 실행 컨텍스트: 슬롯 추가 경로, 프로세스 컨텍스트. sysfs 생성이 있어 잠들 수 있다.
 *
 * 에러 경로: 두 갈래 모두 곧장 return 한다. 리스트 추가 전이라 되돌릴 것이 없고,
 * 슬롯 구조체 자체의 해제는 호출자의 몫이다.
 *
 * 호출 체인:
 *   rpaphp_add_slot() (rpaphp_core.c) → [rpaphp_register_slot]
 *     → is_registered() → of_property_read_u32() → pci_hp_register()
 *     → list_add()
 */
int rpaphp_register_slot(struct slot *slot)
{
	/* [한국어] 공용 코어에 넘길 내장 hotplug_slot 의 주소. */
	struct hotplug_slot *php_slot = &slot->hotplug_slot;
	/* [한국어] DT 자식 노드에서 읽어 올 DRC 인덱스. */
	u32 my_index;
	/* [한국어] 각 단계의 반환값. */
	int retval;
	/* [한국어] 슬롯 번호. -1 로 시작하는 것은 "아직 못 찾음"을 뜻하며,
	 * 끝내 못 찾아도 그대로 pci_hp_register() 에 넘겨진다. */
	int slotno = -1;

	/* [한국어] 등록하려는 슬롯의 모든 정보를 로그로 남긴다. %pOF 는 디바이스 트리 노드의
	 * 전체 경로를 찍는 커널 전용 서식이다. */
	dbg("%s registering slot:path[%pOF] index[%x], name[%s] pdomain[%x] type[%d]\n",
		__func__, slot->dn, slot->index, slot->name,
		slot->power_domain, slot->type);

	/* should not try to register the same slot twice */
	/* [한국어] 같은 이름의 슬롯이 이미 등록되어 있으면, */
	if (is_registered(slot)) {
		/* [한국어] 오류 로그를 남기고, */
		err("rpaphp_register_slot: slot[%s] is already registered\n", slot->name);
		/* [한국어] -EAGAIN 으로 거절한다. 재시도를 뜻하는 코드지만 여기서는 사실상
		 * "중복이라 거부"의 의미로 쓰인다. */
		return -EAGAIN;
	}

	/* [한국어] 슬롯 노드의 자식들을 순회하며 이 슬롯에 대응하는 PCI 장치 노드를 찾는다.
	 * _scoped 판이라 루프를 어떻게 빠져나가도 of_node_put() 이 자동으로 불린다. */
	for_each_child_of_node_scoped(slot->dn, child) {
		/* [한국어] 자식 노드의 ibm,my-drc-index 속성을 읽는다.
		 * [상류 코드 관찰, 수정하지 않음] 반환값을 retval 에 담아 두고도 검사하지 않는다.
		 * 속성이 없는 자식 노드를 만나면 my_index 가 갱신되지 않은 채(이전 반복의 값,
		 * 또는 첫 반복이면 초기화되지 않은 스택 값) 다음 줄의 비교에 쓰인다. */
		retval = of_property_read_u32(child, "ibm,my-drc-index", &my_index);
		/* [한국어] 인덱스가 일치하면 그 자식이 이 슬롯의 장치다. */
		if (my_index == slot->index) {
			/* [한국어] PCI_DN() 으로 DT 노드에 붙어 있는 PCI 정보를 얻고, 그 devfn 에서
			 * 슬롯 번호를 뽑는다. */
			slotno = PCI_SLOT(PCI_DN(child)->devfn);
			/* [한국어] 찾았으므로 순회를 멈춘다. */
			break;
		}
	}

	/* [한국어] 공용 코어에 슬롯을 등록한다. 이 호출이 성공하면 sysfs 에 슬롯 디렉토리가
	 * 생기고 사용자가 power/attention 파일을 쓸 수 있게 된다.
	 * slotno 가 -1 이면 코어가 "번호 없는 슬롯"으로 처리한다. */
	retval = pci_hp_register(php_slot, slot->bus, slotno, slot->name);
	/* [한국어] 등록 실패 검사. */
	if (retval) {
		/* [한국어] 실패 로그. */
		err("pci_hp_register failed with error %d\n", retval);
		/* [한국어] 오류를 그대로 전달한다. 이 시점에는 리스트에 넣기 전이라 정리할 것이 없다. */
		return retval;
	}

	/* add slot to our internal list */
	/* [한국어] 전역 리스트에 추가한다. 등록이 성공한 뒤에 넣는 순서가 중요하다 —
	 * 먼저 넣으면 등록 실패 시 유령 항목이 남는다. */
	list_add(&slot->rpaphp_slot_list, &rpaphp_slot_head);
	/* [한국어] 등록 완료를 알린다. */
	info("Slot [%s] registered\n", slot->name);
	/* [한국어] 성공. */
	return 0;
}
