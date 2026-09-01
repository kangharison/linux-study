/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * CompactPCI Hot Plug Core Functions
 *
 * Copyright (C) 2002 SOMA Networks, Inc.
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/* [한국어] include 가드 시작. */
/*
 * [한국어 설명] CompactPCI 핫플러그 코어의 공용 정의 (cpci_hotplug.h)
 *
 * === 파일의 역할 ===
 * CompactPCI(cPCI) 핫플러그 스택에서 "새시 공통 코어"와 "보드별 드라이버"가
 * 주고받는 계약을 정의하는 헤더다. 구현은 없고(인라인 둘 제외) 상수·구조체·
 * 함수 선언만 담는다.
 * 담고 있는 것은 네 묶음이다. (1) PICMG 2.1 R2.0 규격의 HS(Hot Swap) CSR
 * 비트 정의 — 삽입(INS)·추출(EXT) 이벤트와 블루 LED 제어(LOO)가 여기 있다.
 * cPCI 핫플러그의 상태 기계가 전부 이 레지스터 하나 위에서 돌아간다.
 * (2) struct slot — 물리 슬롯 하나를 나타내며, 공용 코어가 요구하는
 * hotplug_slot 을 값으로 내장한다. (3) struct cpci_hp_controller 와 그
 * ops 테이블 — 보드별 드라이버가 "내 하드웨어에서 #ENUM 은 이렇게 읽는다"를
 * 채워 넣는 곳이다. (4) 코어가 보드 드라이버에게 공개하는 함수와, 반대로
 * 보드 드라이버가 쓰면 안 되는 내부 함수의 구분(상류 주석이 명시한다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * cPCI 핫플러그는 세 층이다. 최상단은 sysfs 슬롯 인터페이스를 제공하는
 * 공용 코어 pci_hotplug_core.c, 중간은 cPCI 규격의 상태 기계를 구현한
 * cpci_hotplug_core.c 와 PCI 열거를 맡는 cpci_hotplug_pci.c, 최하단이
 * 보드별 하드웨어 접근 계층(cpcihp_zt5550.c 등)이다.
 * 이 헤더는 중간층과 최하단층 사이의 인터페이스를 정의한다. 보드 드라이버는
 * struct cpci_hp_controller 를 채워 cpci_hp_register_controller() 로 등록하고,
 * 슬롯 범위를 cpci_hp_register_bus() 로 알린 뒤 cpci_hp_start() 를 부른다.
 * 그러면 코어가 인터럽트를 걸거나 폴링 스레드를 띄워 #ENUM 신호를 감시하고,
 * 이벤트가 오면 HS CSR 을 읽어 삽입인지 추출인지 판별한 뒤 PCI 열거나 제거를
 * 진행한다.
 * 실행 컨텍스트는 두 갈래다. 등록·해제와 슬롯 구성은 프로세스 컨텍스트이고,
 * check_irq 콜백만 인터럽트 판정 경로에서 불린다 — 그래서 그 콜백은
 * 잠들지 않는 단순 레지스터 읽기여야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci_hotplug.h 의 struct hotplug_slot 과 hotplug_slot_name().
 * struct slot 이 hotplug_slot 을 값으로 내장해 to_slot() 이 container_of 로
 * 역변환하며, 이는 rpaphp.h / shpchp.h 와 같은 관용이다.
 * 아래쪽: 보드별 드라이버가 struct cpci_hp_controller_ops 의 네 콜백
 * (query_enum / enable_irq / disable_irq / check_irq)을 구현한다. 그것이
 * 하드웨어 접근의 전부이며, 코어는 레지스터를 직접 만지지 않는다.
 * 옆쪽: cpci_hotplug_core.c 가 전역 슬롯 리스트와 cpci_debug 플래그를 소유하고,
 * cpci_hotplug_pci.c 가 HS CSR 접근과 PCI 열거·제거를 구현한다.
 * 데이터 흐름: #ENUM 신호 → 보드 드라이버의 query_enum 또는 인터럽트 →
 * 코어가 슬롯을 순회하며 HS CSR 읽기 → INS/EXT 판별 → PCI 열거 또는 제거 →
 * struct slot 의 adapter_status / latch_status 갱신 → sysfs 로 사용자에게 노출.
 * 공유 상태: struct slot 의 slot_list 로 이어지는 전역 리스트와 cpci_debug.
 * 리스트 보호는 cpci_hotplug_core.c 가 자체 뮤텍스로 처리한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - HS_CSR_ 계열 상수 일곱 개: PICMG 2.1 규격의 Hot Swap Control/Status
 *   레지스터 비트다. INS(bit 7)와 EXT(bit 6)가 이벤트를 나르고,
 *   LOO(bit 3)가 "뽑아도 안전"을 뜻하는 블루 LED 를 제어한다.
 * - struct slot: 슬롯 번호와 devfn 을 따로 두는 것이 특징이다 — 새시 배선에
 *   따라 슬롯 1번이 devfn 0 이 아닐 수 있기 때문이다. hotplug_slot 을 값으로
 *   내장하고, latch/adapter 상태를 1비트 비트필드로 둔다.
 * - struct cpci_hp_controller_ops: 보드별 하드웨어 접근의 전부.
 *   query_enum 은 폴링에, enable/disable_irq 와 check_irq 는 인터럽트 방식에 쓰인다.
 * - struct cpci_hp_controller: 인터럽트 정보와 위 ops 를 묶는다.
 *   irq 가 0 이면 코어가 인터럽트 대신 폴링을 택한다.
 * - to_slot() / slot_name(): 두 인라인. 전자는 공용 코어에서 이 드라이버로
 *   돌아오는 통로이고, 후자는 이름 관리를 코어에 맡겼음을 보여 준다.
 * - 공개 API 여섯 개(register/unregister_controller, register/unregister_bus,
 *   start, stop)와 내부 함수 열 개. 상류 주석이 후자를 "board/chassis 드라이버가
 *   쓰면 안 된다"고 명시한다.
 * - cpci_hotplug_init(): CONFIG_HOTPLUG_PCI_CPCI 가 꺼지면 0 을 돌려주는
 *   인라인 더미로 대체되어, 호출자가 #ifdef 를 쓰지 않아도 된다.
 */

#ifndef _CPCI_HOTPLUG_H
/* [한국어] 가드 매크로 정의. */
#define _CPCI_HOTPLUG_H

/* [한국어] u8/u16 등 고정폭 정수 타입. */
#include <linux/types.h>
/* [한국어] struct pci_bus / pci_dev 정의. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 과 hotplug_slot_name() — 공용 코어와의 접점이다. */
#include <linux/pci_hotplug.h>

/* PICMG 2.1 R2.0 HS CSR bits: */
/* [한국어] PICMG 2.1 R2.0 규격의 HS(Hot Swap) CSR 비트들(위 상류 주석).
 * bit 7 — INS(Insertion). 보드가 삽입되었음을 나타내며, 1 을 써서 지운다. */
#define HS_CSR_INS	0x0080
/* [한국어] bit 6 — EXT(Extraction). 보드를 뽑으려 한다는 표시. 사용자가 이젝터 손잡이를
 * 열면 서면, 소프트웨어가 정리를 마친 뒤 지운다. */
#define HS_CSR_EXT	0x0040
/* [한국어] bit [5:4] — PI(Programming Interface). 이 슬롯의 프로그래밍 인터페이스
 * 종류를 나타내는 2비트 필드다. */
#define HS_CSR_PI	0x0030
/* [한국어] bit 3 — LOO(LED On/Off). 블루 LED 를 켜고 끄는 비트다. CompactPCI 에서
 * 파란 LED 는 "이 보드를 뽑아도 안전하다"는 표시다. */
#define HS_CSR_LOO	0x0008
/* [한국어] bit 2 — PIE(Programming Interface Enable). */
#define HS_CSR_PIE	0x0004
/* [한국어] bit 1 — EIM(Extraction Interrupt Mask). 추출 인터럽트를 차단한다. */
#define HS_CSR_EIM	0x0002
/* [한국어] bit 0 — DHA(Device Hiding Arm). 장치를 버스에서 숨기도록 무장한다. */
#define HS_CSR_DHA	0x0001

struct slot {
	/* [한국어] 슬롯 번호(사용자에게 보이는 번호).
	 * 설정자: cpci_hp_register_bus() 가 first~last 범위를 순회하며 채운다.
	 * 읽는 자: 슬롯 이름 생성과 로그.
	 * 값 범위: 등록 시 지정한 first 이상 last 이하.
	 * 동기화: 등록 후 읽기 전용. */
	u8 number;
	/* [한국어] 이 슬롯의 devfn. 슬롯 번호와 devfn 이 별개인 것은, 새시 배선에 따라
	 * 슬롯 1번이 devfn 0 이 아닐 수 있기 때문이다.
	 * 설정자: cpci_hp_register_bus().
	 * 읽는 자: config 접근으로 HS CSR 을 읽고 쓰는 모든 함수.
	 * 값 범위: 유효한 devfn.
	 * 동기화: 등록 후 읽기 전용. */
	unsigned int devfn;
	/* [한국어] 이 슬롯이 속한 PCI 버스.
	 * 설정자: cpci_hp_register_bus().
	 * 읽는 자: config 접근과 장치 열거 경로.
	 * 값 범위: 유효 포인터.
	 * 동기화: 등록 후 읽기 전용. */
	struct pci_bus *bus;
	/* [한국어] 슬롯에 꽂힌 장치. 비어 있으면 NULL 이다.
	 * 설정자: cpci_configure_slot() 이 열거 후 채우고, unconfigure 가 비운다.
	 * 읽는 자: HS CSR 접근 경로가 이 포인터로 config 에 접근한다.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: 핫플러그 상태 기계가 직렬화한다. */
	struct pci_dev *dev;
	/* [한국어] 이젝터 래치가 닫혀 있는지(1비트 비트필드).
	 * 설정자: 슬롯 상태 갱신 경로.
	 * 읽는 자: 공용 코어의 get_latch_status 콜백.
	 * 값 범위: 0 또는 1.
	 * 동기화: 별도 락 없음. */
	unsigned int latch_status:1;
	/* [한국어] 어댑터(보드)가 꽂혀 있는지(1비트 비트필드).
	 * 설정자: 슬롯 상태 갱신 경로.
	 * 읽는 자: 공용 코어의 get_adapter_status 콜백.
	 * 값 범위: 0 또는 1.
	 * 동기화: 별도 락 없음. */
	unsigned int adapter_status:1;
	/* [한국어] 추출 절차가 진행 중인지. 위 둘과 달리 비트필드가 아니라 온전한
	 * unsigned int 인데, 이 트리의 코드만으로는 그 이유를 확인할 수 없다.
	 * 설정자: EXT 비트를 감지한 폴링 스레드.
	 * 읽는 자: 같은 스레드가 중복 처리를 막는 데 쓴다.
	 * 값 범위: 0 또는 0 이 아닌 값.
	 * 동기화: 별도 락 없음. */
	unsigned int extracting;
	/* [한국어] PCI 핫플러그 공용 코어가 요구하는 슬롯 객체. 포인터가 아니라 값으로
	 * 내장되어 있어 아래 to_slot() 이 container_of 로 역변환할 수 있다.
	 * 설정자: 슬롯 등록 경로가 ops 를 걸고 pci_hp_register() 가 나머지를 채운다.
	 * 읽는 자: 공용 코어의 sysfs 처리 전부.
	 * 값 범위: 구조체 내장.
	 * 동기화: 공용 코어가 관리한다. */
	struct hotplug_slot hotplug_slot;
	/* [한국어] 전역 슬롯 리스트에 이 슬롯을 매다는 노드.
	 * 설정자·읽는 자: cpci_hotplug_core.c 의 등록·해제·순회.
	 * 값 범위: 항상 유효한 리스트 노드.
	 * 동기화: cpci_hotplug_core.c 가 자체 뮤텍스로 보호한다. */
	struct list_head slot_list;
};

struct cpci_hp_controller_ops {
	/* [한국어] #ENUM 신호가 어서트되었는지 묻는다.
	 * 설정자: 보드별 드라이버(cpcihp_zt5550.c 등)가 자기 구현을 채운다.
	 * 읽는 자: 폴링 스레드가 주기적으로 부른다.
	 * 값 범위: 0 이 아니면 이벤트 발생.
	 * 동기화: 보드 드라이버가 책임진다. */
	int (*query_enum)(void);
	/* [한국어] 컨트롤러의 #ENUM 인터럽트를 허용한다.
	 * 설정자: 보드별 드라이버.
	 * 읽는 자: 코어가 폴링 대신 인터럽트를 쓸 때.
	 * 값 범위: 0 = 성공.
	 * 동기화: 보드 드라이버가 책임진다. */
	int (*enable_irq)(void);
	/* [한국어] 그 인터럽트를 차단한다. enable 과 짝이다. */
	int (*disable_irq)(void);
	/* [한국어] 발생한 인터럽트가 이 컨트롤러의 것인지 판정한다.
	 * 공유 IRQ 라인에서 소유권을 가리는 용도이며, dev_id 로 자기 것인지 확인한다. */
	int (*check_irq)(void *dev_id);
};

struct cpci_hp_controller {
	/* [한국어] #ENUM 인터럽트 번호. 0 이면 인터럽트를 쓰지 않고 폴링한다.
	 * 설정자: 보드별 드라이버가 등록 전에 채운다.
	 * 읽는 자: cpci_hp_start() 가 request_irq 여부를 결정한다.
	 * 값 범위: 유효 IRQ 번호 또는 0.
	 * 동기화: 등록 후 읽기 전용. */
	unsigned int irq;
	/* [한국어] request_irq 에 넘길 플래그(IRQF_SHARED 등).
	 * 설정자·읽는 자·동기화: irq 와 같다. */
	unsigned long irq_flags;
	/* [한국어] request_irq 에 넘길 이름. /proc/interrupts 에 표시된다.
	 * 설정자·읽는 자·동기화: irq 와 같다. */
	char *devname;
	/* [한국어] 인터럽트 핸들러에 넘길 불투명 포인터이자, check_irq 가 소유권을 가릴 때
	 * 비교하는 값이다.
	 * 설정자: 보드별 드라이버.
	 * 읽는 자: 코어의 인터럽트 등록과 check_irq 호출.
	 * 값 범위: 보드 드라이버가 정한 값.
	 * 동기화: 등록 후 읽기 전용. */
	void *dev_id;
	/* [한국어] 이 컨트롤러의 이름. 로그에 쓰인다.
	 * 설정자: 보드별 드라이버.
	 * 읽는 자: 코어의 로그.
	 * 값 범위: NULL 이 아닌 문자열.
	 * 동기화: 등록 후 읽기 전용. */
	char *name;
	/* [한국어] 위에서 정의한 콜백 테이블. 이 포인터가 보드별 하드웨어 접근과
	 * 공용 상태 기계를 잇는 유일한 통로다.
	 * 설정자: 보드별 드라이버.
	 * 읽는 자: cpci_hotplug_core.c 의 폴링 스레드와 인터럽트 경로.
	 * 값 범위: 유효 포인터(NULL 이면 코어가 등록을 거부한다).
	 * 동기화: 등록 후 읽기 전용. */
	struct cpci_hp_controller_ops *ops;
};

/* [한국어]
 * slot_name - 슬롯의 이름 문자열을 얻는다
 *
 * @slot: 이 드라이버의 슬롯 객체.
 * @return: 공용 코어가 관리하는 이름 문자열.
 *
 * 이름 저장을 공용 코어에 맡기고 여기서는 조회만 하는 구조다. 그 덕분에
 * 이 드라이버는 문자열의 수명이나 해제를 신경 쓸 필요가 없다 —
 * pci_hp_register() 가 만들고 pci_hp_deregister() 가 해제한다.
 *
 * 로그 매크로 여러 곳에서 쓰이므로 인라인으로 두어 호출 비용을 없앴다.
 *
 * 실행 컨텍스트: 어디서든 안전하다. 포인터 역참조 두 번이 전부다.
 *
 * 에러 경로: 없다. slot 이 NULL 이면 호출자의 버그다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 로그와 sysfs 경로 → [slot_name]
 *     → hotplug_slot_name()
 */
static inline const char *slot_name(struct slot *slot)
{
	/* [한국어] 공용 코어가 관리하는 슬롯 이름을 그대로 돌려준다. 이름 저장을
	 * 코어에 맡기고 여기서는 조회만 하므로, 이 드라이버는 이름 문자열의
	 * 수명을 신경 쓸 필요가 없다. */
	return hotplug_slot_name(&slot->hotplug_slot);
}

/* [한국어]
 * to_slot - 공용 코어의 hotplug_slot 에서 이 드라이버의 struct slot 을 되찾는다
 *
 * @hotplug_slot: PCI 핫플러그 공용 코어가 콜백에 넘겨 주는 포인터.
 * @return: 그것을 감싸고 있는 struct slot 의 주소.
 *
 * 공용 코어(pci_hotplug_core.c)는 드라이버별 구조를 모르고 struct hotplug_slot
 * 만 다룬다. 그래서 콜백을 받은 드라이버는 그 포인터에서 자기 문맥으로
 * 되돌아갈 방법이 필요하다. struct slot 이 hotplug_slot 을 포인터가 아니라
 * 값으로 내장하고 있기 때문에 container_of 가 성립하며, drvdata 같은 별도
 * 저장소가 필요 없다.
 *
 * 같은 트리의 rpaphp.h 와 shpchp.h 도 정확히 같은 관용을 쓴다 — 핫플러그
 * 드라이버들의 공통 패턴이다.
 *
 * 실행 컨텍스트: 공용 코어의 sysfs 콜백 경로. 순수 주소 계산이라 어디서든 안전하다.
 *
 * 에러 경로: 없다. NULL 을 넘기면 잘못된 주소가 나오므로 호출자가 보장해야 한다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → cpci_hotplug_core.c 의 콜백
 *     → [to_slot] → 이 드라이버의 struct slot
 */
static inline struct slot *to_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 공용 코어가 주는 hotplug_slot 포인터에서 이 드라이버의 struct slot 을
	 * 되찾는다. hotplug_slot 이 값으로 내장되어 있어 성립하는 관용이다. */
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/* [한국어] 보드별 드라이버가 자기 컨트롤러를 코어에 등록한다. 정의는 cpci_hotplug_core.c. */
int cpci_hp_register_controller(struct cpci_hp_controller *controller);
/* [한국어] 그 등록을 해제한다. */
int cpci_hp_unregister_controller(struct cpci_hp_controller *controller);
/* [한국어] 한 버스의 first~last 슬롯을 코어에 등록한다. 새시의 물리 슬롯 배치를
 * 코어에 알려 주는 함수다. */
int cpci_hp_register_bus(struct pci_bus *bus, u8 first, u8 last);
/* [한국어] 그 버스의 슬롯들을 모두 해제한다. */
int cpci_hp_unregister_bus(struct pci_bus *bus);
/* [한국어] 핫플러그 감시를 시작한다 — 인터럽트를 걸거나 폴링 스레드를 띄운다. */
int cpci_hp_start(void);
/* [한국어] 감시를 멈춘다. */
int cpci_hp_stop(void);

/* Global variables */
/* [한국어] 디버그 로그 활성화 플래그(위 상류 주석). 정의는 cpci_hotplug_core.c 이고
 * 모듈 파라미터로 노출된다. */
extern int cpci_debug;

/*
 * Internal function prototypes, these functions should not be used by
 * board/chassis drivers.
 */
/* [한국어] 슬롯의 attention LED 상태를 읽는다(위 상류 주석대로 보드 드라이버가
 * 쓰면 안 되는 내부 함수다). */
u8 cpci_get_attention_status(struct slot *slot);
/* [한국어] HS CSR 레지스터 전체를 읽는다. 위 HS_CSR_ 비트들이 이 값 안에 있다. */
u16 cpci_get_hs_csr(struct slot *slot);
/* [한국어] attention LED 상태를 설정한다. */
int cpci_set_attention_status(struct slot *slot, int status);
/* [한국어] INS 비트를 확인하고 지운다 — 삽입 이벤트를 소비하는 함수다. */
int cpci_check_and_clear_ins(struct slot *slot);
/* [한국어] EXT 비트를 확인한다. 추출 요청을 감지한다. */
int cpci_check_ext(struct slot *slot);
/* [한국어] EXT 비트를 지운다. 정리가 끝났음을 하드웨어에 알린다. */
int cpci_clear_ext(struct slot *slot);
/* [한국어] 블루 LED 를 켠다 — "뽑아도 안전" 표시다. */
int cpci_led_on(struct slot *slot);
/* [한국어] 블루 LED 를 끈다. */
int cpci_led_off(struct slot *slot);
/* [한국어] 슬롯의 PCI 장치를 열거해 커널에 등록한다. */
int cpci_configure_slot(struct slot *slot);
/* [한국어] 그 장치를 제거한다. */
int cpci_unconfigure_slot(struct slot *slot);

#ifdef CONFIG_HOTPLUG_PCI_CPCI
/* [한국어] CPCI 핫플러그가 빌드에 포함된 경우의 초기화 함수 선언. */
int cpci_hotplug_init(int debug);
#else
/* [한국어] 빌드에서 빠진 경우를 위한 인라인 더미. 호출자가 #ifdef 로 자기 코드를
 * 감싸지 않아도 되게 해 주며, inline 이라 컴파일러가 통째로 없애 버린다. */
static inline int cpci_hotplug_init(int debug) { return 0; }
#endif

/* [한국어] include 가드의 끝. 뒤의 주석은 어떤 #ifndef 에 대응하는지 밝히는 관례다. */
#endif	/* _CPCI_HOTPLUG_H */
