/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ACPI PCI Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2002 Hiroshi Aono (h-aono@ap.jp.nec.com)
 * Copyright (C) 2002,2003 Takayoshi Kochi (t-kochi@bq.jp.nec.com)
 * Copyright (C) 2002,2003 NEC Corporation
 * Copyright (C) 2003-2005 Matthew Wilcox (willy@infradead.org)
 * Copyright (C) 2003-2005 Hewlett Packard
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>,
 *		    <t-kochi@bq.jp.nec.com>
 *
 */

/* [한국어] 이중 포함 방지. 이 헤더가 acpiphp_core.c / acpiphp_glue.c / acpiphp_ibm.c
 * 여러 곳에서 포함되기 때문에 필요하다. */
/*
 * [한국어 설명] ACPI PCI 핫플러그 드라이버의 공용 자료구조 헤더 (acpiphp.h)
 *
 * === 파일의 역할 ===
 * ACPI 기반 PCI 핫플러그를 구현하는 세 파일 — acpiphp_core.c(sysfs 표면),
 * acpiphp_glue.c(ACPI 네임스페이스 처리), acpiphp_ibm.c(IBM 전용 attention) —
 * 가 공유하는 구조체와 상수, 그리고 함수 원형을 모아 둔 헤더다.
 * 코드는 static inline 일곱 개뿐이고, 그 일곱 개 중 넷이 container_of 래퍼다.
 * 그 사실이 이 헤더의 성격을 말해 준다 — 여기 정의된 구조체들은 서로를
 * 품고 가리키는 그물이고, 이 헤더의 주된 일은 그 그물을 어느 방향으로든
 * 오갈 수 있게 만드는 것이다.
 * 구조체는 여섯이며 계층이 뚜렷하다. 위에서부터 acpiphp_bridge(브리지 하나)
 * → acpiphp_slot(물리 슬롯 하나) → acpiphp_func(PCI 함수 하나, 슬롯당 최대 8개)
 * 로 내려가고, 그와 별개로 struct slot 이 sysfs 표면을 맡으며,
 * acpiphp_context 와 acpiphp_root_context 가 ACPI 코어와의 접점을 맡는다.
 * 마지막으로 acpiphp_attention_info 는 attention LED 를 다루는 표준 방법이
 * ACPI 에 없어 플랫폼별 드라이버가 끼어들 자리를 만든 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ACPI 핫플러그의 데이터 흐름은 두 방향이며, 이 헤더의 구조체들이 그 두
 * 방향의 만남을 중개한다.
 * 아래에서 위로: 펌웨어가 ACPI 통지를 보내면 ACPI 코어가
 * acpi_hotplug_context 를 들고 acpiphp_glue.c 를 부른다. glue 는
 * to_acpiphp_context()(또는 루트면 to_acpiphp_root_context())로 자기 자료구조를
 * 되찾고, 거기서 bridge → slot → func 를 따라가며 _STA 나 _EJ0 를 평가한다.
 * 위에서 아래로: 사용자가 sysfs 의 power 를 건드리면 PCI 핫플러그 코어가
 * hotplug_slot 을 들고 acpiphp_core.c 를 부른다. core 는 to_slot() 으로
 * struct slot 을 되찾고, 그 안의 acpi_slot 포인터를 따라 ACPI 계층의
 * acpiphp_enable_slot() / acpiphp_disable_slot() 으로 넘긴다.
 * 즉 struct slot 은 sysfs 쪽 얼굴이고 struct acpiphp_slot 은 ACPI 쪽 얼굴이며,
 * 두 구조체가 서로를 가리키는 한 쌍(slot->acpi_slot, acpiphp_slot->slot)이
 * 두 세계를 잇는다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. static inline 들은 포인터 산술과
 * 필드 접근뿐이라 어디서 불려도 안전하다.
 *
 * === 타 모듈과의 연결 ===
 * acpiphp_core.c: 이 헤더의 struct slot 을 소유한다. 다섯 sysfs 콜백이 모두
 * to_slot() 으로 시작해 slot->acpi_slot 을 ACPI 계층에 넘기고(:420, :440,
 * :503, :564, :589), acpiphp_register_hotplug_slot() 이 acpi_slot 과 sun 을
 * 채운다(:650, :657). 전역 acpiphp_disabled 도 여기에 정의되어 있다(:92).
 * acpiphp_glue.c: 나머지 다섯 구조체를 모두 다룬다. 브리지 해체 시
 * is_going_away 를 세우고(:750), 슬롯에 SLOT_IS_GOING_AWAY 를 세우며(:747),
 * 켜기 요청은 그 두 플래그를 먼저 확인한다(:2032, :2035). _STA 는
 * FUNC_HAS_STA 가 켜진 함수만 평가하고(:1266), 없으면 ACPI_STA_ALL 을
 * 만들어 넣는다(:1271). _EJ0 는 FUNC_HAS_EJ0 가 붙은 첫 함수로 부른다(:2081).
 * acpiphp_ibm.c: hpslot_to_sun() 매크로(:44)로 struct slot 의 sun 필드만 쓴다.
 * acpiphp_ampere_altra.c: acpiphp_attention_info 를 채워
 * acpiphp_register_attention() 으로 등록한다(:365).
 * ACPI 코어(linux/acpi.h): acpi_hotplug_context 와 acpi_device, acpi_handle.
 * PCI 핫플러그 코어(linux/pci_hotplug.h): struct hotplug_slot 과
 * hotplug_slot_name().
 * 이름 충돌 주의: drivers/pci/hotplug/cpci_hotplug.h 에도 struct slot 과
 * slot_name(), to_slot() 이 있다. 서로 다른 구조체이며, 각 구현 파일이
 * 포함한 헤더에 따라 어느 쪽이 보이는지가 정해진다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct slot: sysfs 쪽 얼굴. hotplug_slot 을 첫 필드로 품어 to_slot() 의
 *   container_of 를 성립시키고, acpi_slot 으로 ACPI 계층을 가리킨다.
 * - struct acpiphp_bridge: 브리지 하나. kref 로 수명을 세고 is_going_away 로
 *   해체 중임을 알린다. nr_slots 는 개수이자, _SUN 이 없는 슬롯의 대체 번호다
 *   — 그래서 등록이 실패하면 다시 감소시킨다.
 * - struct acpiphp_slot: ACPI 쪽 얼굴. bus 와 device 로 PCI 위치를 지목하고,
 *   funcs 리스트에 함수들을 매달며, flags 로 SLOT_ENABLED /
 *   SLOT_IS_GOING_AWAY 상태를 기록한다.
 * - struct acpiphp_func: PCI 함수 하나. flags 의 FUNC_HAS_STA / FUNC_HAS_EJ0
 *   가 어떤 ACPI 메서드를 부를 수 있는지 말해 준다.
 * - struct acpiphp_context: ACPI 코어와의 접점. hp 를 첫 필드로,
 *   func 를 **값으로** 품는 배치가 to_acpiphp_context() 와 func_to_context()
 *   두 방향의 container_of 를 동시에 가능하게 한다. 수명은 kref 가 아니라
 *   평범한 unsigned int refcount 로 세므로 락 아래에서만 만져야 한다.
 * - struct acpiphp_root_context: 루트 브리지 전용. 루트에는 대응하는 PCI
 *   함수가 없어 acpiphp_context 를 쓸 수 없기 때문에 따로 있다. 같은
 *   acpi_hotplug_context 포인터를 두 변환 함수 중 어디에 넣느냐를 호출자가
 *   알고 골라야 한다.
 * - struct acpiphp_attention_info: ACPI 에 attention LED 표준이 없어 만든
 *   플랫폼 훅. set_attn / get_attn 쌍과 소유 모듈로 이루어진다.
 * - static inline 일곱: slot_name() 과 to_slot()(sysfs 쪽),
 *   to_acpiphp_context() / func_to_context() / func_to_acpi_device() /
 *   func_to_handle()(ACPI 쪽 세 단 사슬), to_acpiphp_root_context()(루트 전용).
 * - 상수: ACPI_STA_ALL(0x0f, _STA 에서 볼 하위 4비트), SLOT_ 계열 둘,
 *   FUNC_ 계열 둘.
 */

#ifndef _ACPIPHP_H
#define _ACPIPHP_H

/* [한국어] struct acpi_hotplug_context, struct acpi_device, acpi_handle —
 * 아래 acpiphp_context 와 acpiphp_root_context 가 그것을 품고 있고,
 * func_to_handle() 이 handle 필드를 꺼낸다. */
#include <linux/acpi.h>
/* [한국어] 이 헤더 자체는 뮤텍스를 선언하지 않지만, 이 헤더를 포함하는 구현 파일들이
 * 쓰도록 상류가 여기서 끌어온다. */
#include <linux/mutex.h>
/* [한국어] struct hotplug_slot 과 hotplug_slot_name(). 아래 struct slot 이
 * hotplug_slot 을 첫 필드로 품는 구조라 반드시 필요하다. */
#include <linux/pci_hotplug.h>

/* [한국어] 아래에서 실제로 정의되지만, 서로를 포인터로 참조하는 순환 구조라
 * 미리 이름만 알려 둔다 — acpiphp_bridge 는 context 를 가리키고
 * context 는 bridge 를 가리킨다. */
struct acpiphp_context;
/* [한국어] 위와 같은 이유의 전방 선언. */
struct acpiphp_bridge;
/* [한국어] 위와 같은 이유의 전방 선언. acpiphp_slot 은 slot 을, slot 은 acpiphp_slot 을
 * 서로 가리킨다. */
struct acpiphp_slot;

/*
 * struct slot - slot information for each *physical* slot
 */
struct slot {
	/* [한국어] PCI 핫플러그 코어에 등록되는 슬롯 구조체.
	 * 설정자: acpiphp_register_hotplug_slot()(acpiphp_core.c) 이 채운 뒤
	 *   pci_hp_register() 로 코어에 넘긴다.
	 * 읽는 자: 코어가 sysfs 접근 때마다 이 포인터로 콜백을 부르고,
	 *   콜백은 아래 to_slot() 으로 바깥 struct slot 을 되찾는다.
	 * 값 범위: 이 구조체의 **첫 필드**여야 한다. to_slot() 의 container_of 가
	 *   성립하는 근거이며, 필드 순서를 바꾸면 오프셋 계산이 깨지지는 않지만
	 *   이 배치가 관례다.
	 * 동기화: 코어가 등록·해제 시점을 직렬화한다. */
	struct hotplug_slot	hotplug_slot;
	/* [한국어] 이 물리 슬롯에 대응하는 ACPI 쪽 슬롯 정보.
	 * 설정자: acpiphp_register_hotplug_slot()(acpiphp_core.c:650).
	 * 읽는 자: acpiphp_core.c 의 다섯 콜백이 모두 이 포인터를 꺼내
	 *   acpiphp_enable_slot() / disable_slot() / get_*_status() 에 넘긴다(:420, :440,
	 *   :503, :564, :589). 즉 sysfs 요청을 ACPI 계층으로 옮기는 다리다.
	 * 값 범위: 유효한 acpiphp_slot 포인터. 등록 시점에 정해지고 바뀌지 않는다.
	 * 동기화: 슬롯 수명 동안 불변이라 락이 필요 없다. */
	struct acpiphp_slot	*acpi_slot;
	/* [한국어] ACPI _SUN(Slot User Number) 값 — 사용자에게 보일 슬롯 번호.
	 * 설정자: acpiphp_register_hotplug_slot()(acpiphp_core.c:657) 이 인자로 받은
	 *   값을 그대로 넣는다. 그 값은 glue 쪽에서 _SUN 을 평가해 얻거나,
	 *   _SUN 이 없으면 브리지의 nr_slots 순번으로 대체한 것이다.
	 * 읽는 자: acpiphp_ibm.c 의 hpslot_to_sun() 매크로(:44)가 IBM 전용 attention
	 *   처리에서 이 번호로 슬롯을 식별한다.
	 * 값 범위: 펌웨어가 정하는 임의의 양수. 물리적 슬롯 라벨과 맞추는 것이 목적이다.
	 * 동기화: 등록 시 한 번 쓰고 이후 읽기만 한다. */
	unsigned int sun;	/* ACPI _SUN (Slot User Number) value */
};

/* [한국어]
 * slot_name - 슬롯의 sysfs 이름을 얻는다
 *
 * @slot: acpiphp 쪽 슬롯 구조체.
 * @return: 핫플러그 코어가 붙여 둔 슬롯 이름 문자열.
 *
 * 내장된 hotplug_slot 에서 이름을 꺼내는 한 줄 래퍼다. 굳이 두는 이유는
 * 로그 메시지에서 자주 쓰이는데, 매번 &slot->hotplug_slot 을 적는 것보다
 * 읽기 좋기 때문이다(acpiphp_core.c:417, :437, :499, :560, :586, :677, :722).
 *
 * 주의할 점이 있다. drivers/pci/hotplug 에는 이름이 같은 struct slot 과
 * slot_name() 이 cpci_hotplug.h 에도 있어, CompactPCI 쪽 코드의 동명 함수와
 * 혼동하기 쉽다. 서로 다른 구조체이며 이 헤더를 포함한 파일에서만 이것이 보인다.
 *
 * 실행 컨텍스트: 어디서든. static inline 이고 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_core.c 의 로그 출력 → [이 함수] → hotplug_slot_name()
 */
static inline const char *slot_name(struct slot *slot)
{
	return hotplug_slot_name(&slot->hotplug_slot);
}

/* [한국어]
 * to_slot - 핫플러그 코어가 넘긴 hotplug_slot 에서 acpiphp 슬롯을 되찾는다
 *
 * @hotplug_slot: 코어가 콜백에 넘긴 슬롯 포인터.
 * @return: 그것을 품고 있는 struct slot.
 *
 * PCI 핫플러그 코어는 acpiphp 의 자료구조를 모른다. 그래서 struct slot 이
 * hotplug_slot 을 첫 필드로 품고, 콜백에서 container_of 로 거슬러 올라간다 —
 * 이 헤더에 있는 네 개의 container_of 래퍼 중 첫 번째다.
 *
 * acpiphp_core.c 의 다섯 sysfs 콜백이 전부 이 함수로 시작한다
 * (:413, :433, :495, :556, :582). 되찾은 struct slot 에서 다시 acpi_slot 을
 * 꺼내 ACPI 계층 함수에 넘기는 것이 그 콜백들의 공통 형태다.
 *
 * 실행 컨텍스트: sysfs 콜백. static inline 이라 호출 비용이 없다.
 *
 * 에러 경로: 없다. 포인터 산술일 뿐이라 실패할 수 없다.
 *
 * 호출 체인:
 *   PCI 핫플러그 코어 → acpiphp_core.c 의 sysfs 콜백 → [이 함수] → container_of()
 */
static inline struct slot *to_slot(struct hotplug_slot *hotplug_slot)
{
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/*
 * struct acpiphp_bridge - PCI bridge information
 *
 * for each bridge device in ACPI namespace
 */
struct acpiphp_bridge {
	/* [한국어] 전역 bridge_list 에 이 브리지를 잇는 고리.
	 * 설정자/읽는 자: acpiphp_glue.c 가 브리지를 등록·해제할 때 다룬다.
	 * 값 범위: 리스트에 들어 있거나 비어 있거나 둘 중 하나.
	 * 동기화: acpiphp_glue.c 의 bridge_mutex 가 보호한다. */
	struct list_head list;
	/* [한국어] 이 브리지 아래 슬롯들(acpiphp_slot)의 리스트 머리.
	 * 설정자: 열거 과정에서 슬롯을 발견할 때마다 acpiphp_glue.c 가 잇는다.
	 * 읽는 자: 슬롯을 순회하는 모든 경로 — 켜기, 끄기, 해체.
	 * 값 범위: 비어 있을 수 있다(슬롯 없는 브리지).
	 * 동기화: 같은 bridge_mutex 계열 보호를 받는다. */
	struct list_head slots;
	/* [한국어] 브리지 자체의 참조 계수.
	 * 설정자/읽는 자: acpiphp_glue.c 가 브리지를 참조할 때마다 올리고 놓을 때 내린다.
	 * 값 범위: 0 이 되면 해제 콜백이 브리지를 반환한다.
	 * 동기화: kref 자체가 원자적이다. 아래 acpiphp_context 의 refcount 와는
	 *   별개의 수명 관리라는 점이 중요하다 — 브리지는 kref 로, context 는
	 *   정수 refcount 로 따로 센다(acpiphp_glue.c:77). */
	struct kref ref;

	/* [한국어] 이 브리지에 대응하는 ACPI 컨텍스트.
	 * 설정자: 브리지 등록 시 acpiphp_glue.c 가 연결한다.
	 * 읽는 자: ACPI 이벤트를 브리지로 옮길 때. 아래 acpiphp_context 의
	 *   bridge 필드와 서로를 가리키는 쌍을 이룬다.
	 * 값 범위: 유효한 context 포인터.
	 * 동기화: 브리지 수명 동안 불변. */
	struct acpiphp_context *context;

	/* [한국어] 이 브리지에서 지금까지 발견한 슬롯 수.
	 * 설정자/읽는 자: acpiphp_glue.c 의 슬롯 열거 경로(:701, :704, :712)에서만 쓴다.
	 * 값 범위: 0 이상. 단순한 개수 이상의 의미가 있는데, 슬롯에 _SUN 이 없을 때
	 *   이 값을 슬롯 번호 대신 쓴다(:704). 그래서 슬롯 등록이 실패하면
	 *   다시 감소시킨다(:712) — 번호가 어긋나지 않게 하기 위함이다.
	 * 동기화: 열거 경로가 직렬화되어 있다. */
	int nr_slots;

	/* This bus (host bridge) or Secondary bus (PCI-to-PCI bridge) */
	/* [한국어] 이 브리지 아래에 매달린 PCI 버스.
	 * 설정자: 브리지 등록 시. 위 영어 주석대로 호스트 브리지면 자기 버스,
	 *   PCI-PCI 브리지면 세컨더리 버스다.
	 * 읽는 자: 슬롯을 켤 때 이 버스를 스캔해 장치를 찾는다.
	 * 값 범위: 유효한 pci_bus 포인터.
	 * 동기화: 브리지 수명 동안 불변. */
	struct pci_bus *pci_bus;

	/* PCI-to-PCI bridge device */
	/* [한국어] PCI-PCI 브리지 장치. 호스트 브리지인 경우에는 없다.
	 * 설정자: 브리지 등록 시.
	 * 읽는 자: 브리지 자체의 설정을 만질 때.
	 * 값 범위: PCI-PCI 브리지면 유효한 포인터, 호스트 브리지면 NULL.
	 *   이 두 경우를 가르는 것이 위 pci_bus 필드의 의미도 함께 바꾼다.
	 * 동기화: 브리지 수명 동안 불변. */
	struct pci_dev *pci_dev;

	/* [한국어] 이 브리지가 해체 중인지 표시하는 깃발.
	 * 설정자: acpiphp_glue.c 가 브리지를 전역 리스트에서 떼어 낼 때 세운다(:750).
	 * 읽는 자: 새 작업을 시작하려는 경로들이 먼저 확인한다(:359, :1474).
	 * 값 범위: false = 정상, true = 해체 중.
	 * 동기화: 이 깃발이 필요한 이유 자체가 경쟁 때문이다 — 사용자가 sysfs 를
	 *   붙잡고 있는 사이에 브리지가 사라질 수 있어, 리스트에서 떼는 것만으로는
	 *   이미 진행 중인 작업을 막을 수 없다. */
	bool is_going_away;
};


/*
 * struct acpiphp_slot - PCI slot information
 *
 * PCI slot information for each *physical* PCI slot
 */
struct acpiphp_slot {
	/* [한국어] 부모 브리지의 slots 리스트에 이 슬롯을 잇는 고리.
	 * 설정자/읽는 자: acpiphp_glue.c 의 슬롯 열거와 순회.
	 * 값 범위: 리스트에 들어 있거나 비어 있거나.
	 * 동기화: 브리지 리스트와 같은 보호를 받는다. */
	struct list_head node;
	/* [한국어] 이 슬롯이 속한 PCI 버스.
	 * 설정자: 슬롯 생성 시.
	 * 읽는 자: 슬롯을 켤 때 이 버스에서 device 번호로 장치를 찾는다.
	 * 값 범위: 유효한 pci_bus 포인터.
	 * 동기화: 슬롯 수명 동안 불변. */
	struct pci_bus *bus;
	/* [한국어] 이 슬롯에 속한 함수들(acpiphp_func)의 리스트 머리.
	 * 설정자: ACPI 네임스페이스를 훑으며 함수 객체를 발견할 때마다 잇는다.
	 * 읽는 자: _STA 평가와 _EJ0 평가가 이 리스트를 순회한다
	 *   (acpiphp_glue.c:1266, :2081).
	 * 값 범위: 옆의 영어 주석대로 한 슬롯에 여러 함수가 달릴 수 있다.
	 *   PCI 함수 하나마다 ACPI 객체 하나가 대응하며 최대 8개다.
	 * 동기화: 슬롯 조작 경로가 직렬화되어 있다. */
	struct list_head funcs;		/* one slot may have different
					   objects (i.e. for each function) */
	/* [한국어] sysfs 에 노출된 슬롯 구조체. 노출되지 않은 슬롯이면 NULL 이다.
	 * 설정자: acpiphp_register_hotplug_slot() 성공 시 연결된다.
	 * 읽는 자: 슬롯을 해체할 때 sysfs 등록을 먼저 풀어야 하는지 판단한다
	 *   (acpiphp_glue.c:747 부근).
	 * 값 범위: NULL 또는 유효한 struct slot 포인터.
	 *   위 struct slot 의 acpi_slot 필드와 서로를 가리키는 쌍이다.
	 * 동기화: 등록·해제 경로가 직렬화한다. */
	struct slot *slot;

	/* [한국어] 이 슬롯의 PCI device 번호(0~31).
	 * 설정자: ACPI _ADR 의 상위 16비트에서 뽑아낸다.
	 * 읽는 자: bus 필드와 함께 pci_get_slot() 류 조회에 쓴다.
	 * 값 범위: 0~31. PCI 스펙상 버스당 장치 번호의 범위다.
	 * 동기화: 슬롯 수명 동안 불변. */
	u8		device;		/* pci device# */
	/* [한국어] 슬롯 상태 플래그. 아래 SLOT_ 계열 상수를 담는다.
	 * 설정자: 슬롯을 켤 때 SLOT_ENABLED 를(acpiphp_glue.c:594, :1070),
	 *   해체를 시작할 때 SLOT_IS_GOING_AWAY 를 세운다(:747).
	 * 읽는 자: 켜기 요청이 들어오면 두 플래그를 모두 확인한다(:2032, :2035) —
	 *   해체 중이면 물러나고, 이미 켜져 있으면 중복 작업을 피한다.
	 * 값 범위: SLOT_ENABLED(0x1) 와 SLOT_IS_GOING_AWAY(0x2) 의 조합.
	 * 동기화: 슬롯 조작 경로가 직렬화되어 있다. */
	u32		flags;		/* see below */
};


/*
 * struct acpiphp_func - PCI function information
 *
 * PCI function information for each object in ACPI namespace
 * typically 8 objects per slot (i.e. for each PCI function)
 */
struct acpiphp_func {
	/* [한국어] 이 함수가 속한 브리지.
	 * 설정자: 함수 객체를 발견해 등록할 때.
	 * 읽는 자: 함수에서 브리지 수준 정보로 거슬러 올라갈 때.
	 * 값 범위: 유효한 acpiphp_bridge 포인터.
	 * 동기화: 함수 수명 동안 불변. */
	struct acpiphp_bridge *parent;
	/* [한국어] 이 함수가 속한 슬롯. 한 슬롯에 최대 8개의 함수가 매달린다.
	 * 설정자: 함수 등록 시.
	 * 읽는 자: 함수 단위 이벤트를 슬롯 단위 동작으로 옮길 때.
	 * 값 범위: 유효한 acpiphp_slot 포인터.
	 * 동기화: 함수 수명 동안 불변. */
	struct acpiphp_slot *slot;

	/* [한국어] 같은 슬롯에 속한 형제 함수들을 잇는 고리. 슬롯의 funcs 리스트에 들어간다.
	 * 설정자/읽는 자: 슬롯의 함수 순회 경로.
	 * 값 범위: 리스트에 들어 있거나 비어 있거나.
	 * 동기화: 슬롯 조작 경로가 직렬화한다. */
	struct list_head sibling;

	/* [한국어] 이 객체가 대응하는 PCI 함수 번호(0~7).
	 * 설정자: ACPI _ADR 의 하위 16비트에서 뽑아낸다.
	 * 읽는 자: 슬롯의 device 번호와 합쳐 devfn 을 만든다.
	 * 값 범위: 0~7. PCI 스펙상 장치당 함수 번호의 범위다.
	 * 동기화: 함수 수명 동안 불변. */
	u8		function;	/* pci function# */
	/* [한국어] 이 함수가 어떤 ACPI 메서드를 갖고 있는지 나타내는 플래그.
	 *   아래 FUNC_ 계열 상수를 담는다.
	 * 설정자: ACPI 네임스페이스를 훑을 때 _STA 나 _EJ0 메서드의 존재를 확인해 세운다.
	 * 읽는 자: _STA 평가는 FUNC_HAS_STA 가 켜진 함수만 대상으로 하고
	 *   (acpiphp_glue.c:1266), 슬롯을 뺄 때는 FUNC_HAS_EJ0 가 붙은 첫 함수를
	 *   찾아 그것으로 _EJ0 를 부른다(:2081).
	 * 값 범위: FUNC_HAS_STA(0x1) 와 FUNC_HAS_EJ0(0x2) 의 조합.
	 * 동기화: 열거 시 정해지고 이후 읽기만 한다. */
	u32		flags;		/* see below */
};

struct acpiphp_context {
	/* [한국어] ACPI 코어가 다루는 핫플러그 컨텍스트. 이 구조체의 **첫 필드**라
	 *   아래 to_acpiphp_context() 의 container_of 가 성립한다.
	 * 설정자: ACPI 코어가 컨텍스트를 등록할 때 초기화한다. 그 등록 함수는
  이 트리(drivers 만 체크아웃)에는 없어 이름을 단정하지 않는다.
	 * 읽는 자: ACPI 이벤트가 도착하면 코어가 이 구조체를 넘겨 주고,
	 *   to_acpiphp_context() 로 acpiphp 쪽 정보를 되찾는다.
	 *   self 필드는 func_to_acpi_device() 가 꺼내 쓴다.
	 * 값 범위: 유효한 acpi_hotplug_context.
	 * 동기화: ACPI 코어의 규약을 따른다. */
	struct acpi_hotplug_context hp;
	/* [한국어] 이 컨텍스트가 대표하는 PCI 함수 정보. 포인터가 아니라 **내장** 이라는 점이
	 *   핵심이다 — 그래서 아래 func_to_context() 가 반대 방향의 container_of 로
	 *   성립한다.
	 * 설정자: 컨텍스트 생성 시 acpiphp_glue.c 가 채운다.
	 * 읽는 자: 함수 단위 동작 전부.
	 * 값 범위: 내장 구조체라 언제나 유효하다.
	 * 동기화: 컨텍스트 수명 동안 유지된다. */
	struct acpiphp_func func;
	/* [한국어] 이 컨텍스트가 브리지에 대응한다면 그 브리지. 아니면 NULL.
	 * 설정자: 브리지 등록 시 연결. 위 acpiphp_bridge 의 context 필드와
	 *   서로를 가리키는 쌍이다.
	 * 읽는 자: ACPI 이벤트가 브리지 수준 처리를 요구할 때.
	 * 값 범위: NULL(일반 함수) 또는 유효한 브리지 포인터.
	 * 동기화: 브리지 등록·해제 경로가 직렬화한다. */
	struct acpiphp_bridge *bridge;
	/* [한국어] 이 컨텍스트의 참조 계수.
	 * 설정자/읽는 자: acpiphp_glue.c 의 acpiphp_get_context()(:209 부근)가 올리고
	 *   대응하는 put 이 내린다.
	 * 값 범위: 0 이 되면 컨텍스트를 해제한다.
	 * 동기화: kref 가 아니라 평범한 unsigned int 라 원자적이지 않다.
	 *   따라서 이 값을 만지는 경로는 반드시 acpiphp_glue.c 의 락 아래에서
	 *   동작해야 한다. 브리지가 kref 를 쓰는 것과 대비되는 지점이다
	 *   (acpiphp_glue.c:77 의 설명 참조). */
	unsigned int refcount;
};

/* [한국어]
 * to_acpiphp_context - ACPI 핫플러그 컨텍스트에서 acpiphp 컨텍스트를 되찾는다
 *
 * @hp: ACPI 코어가 넘긴 핫플러그 컨텍스트.
 * @return: 그것을 첫 필드로 품고 있는 struct acpiphp_context.
 *
 * ACPI 코어와의 경계에서 쓰는 되돌리기다. 코어는 acpi_hotplug_context 만
 * 알고 있으므로, 이벤트가 도착하면 이 함수로 acpiphp 쪽 정보를 복원한다
 * (acpiphp_glue.c:209 부근의 acpiphp_get_context()).
 *
 * 중요한 함정이 있다. 루트 브리지에는 acpiphp_context 가 아니라
 * acpiphp_root_context 가 붙으며, 그쪽은 아래 to_acpiphp_root_context() 로
 * 따로 꺼내야 한다. 이 함수는 어느 종류인지 판별하지 않으므로,
 * 호출자가 미리 알고 맞는 것을 골라야 한다(acpiphp_glue.c:214 의 설명).
 *
 * 실행 컨텍스트: ACPI 이벤트 처리 경로. static inline.
 *
 * 에러 경로: 없다. 잘못된 종류에 쓰면 조용히 엉뚱한 포인터가 나오므로,
 * 그 판별은 호출자의 책임이다.
 *
 * 호출 체인:
 *   ACPI 코어의 핫플러그 통지 → acpiphp_glue.c → [이 함수] → container_of()
 */
static inline struct acpiphp_context *to_acpiphp_context(struct acpi_hotplug_context *hp)
{
	return container_of(hp, struct acpiphp_context, hp);
}

/* [한국어]
 * func_to_context - PCI 함수 정보에서 그것을 품은 컨텍스트를 되찾는다
 *
 * @func: acpiphp_context 안에 **내장된** 함수 정보.
 * @return: 그것을 품고 있는 struct acpiphp_context.
 *
 * 앞의 두 되돌리기와 방향이 다르다. to_slot() 과 to_acpiphp_context() 는
 * "코어가 넘긴 첫 필드에서 바깥으로" 였는데, 이것은 "가운데 필드에서
 * 바깥으로" 다. acpiphp_context 가 acpiphp_func 를 포인터가 아니라 값으로
 * 품고 있기 때문에 성립한다 — 그 배치가 이 함수의 전제다.
 *
 * 아래 func_to_acpi_device() 와 func_to_handle() 이 이 함수 위에 쌓여
 * 세 단짜리 사슬을 이룬다. func → context → hp.self → handle.
 *
 * 실행 컨텍스트: 함수 단위 ACPI 동작. static inline.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 함수 단위 처리 → [이 함수] → container_of()
 */
static inline struct acpiphp_context *func_to_context(struct acpiphp_func *func)
{
	return container_of(func, struct acpiphp_context, func);
}

/* [한국어]
 * func_to_acpi_device - PCI 함수 정보에서 대응하는 ACPI 장치를 얻는다
 *
 * @func: acpiphp 함수 정보.
 * @return: 그 함수에 대응하는 struct acpi_device.
 *
 * func_to_context() 로 컨텍스트를 되찾은 뒤 hp.self 를 꺼낸다. ACPI 코어가
 * 컨텍스트를 만들 때 그 필드에 자기 장치를 넣어 두기 때문에, 함수에서
 * ACPI 네임스페이스 쪽으로 건너가는 다리가 된다.
 *
 * 실행 컨텍스트: ACPI 메서드를 평가하려는 경로. static inline.
 *
 * 에러 경로: 없다. hp.self 가 NULL 인 컨텍스트는 만들어지지 않는다는 전제다.
 *
 * 호출 체인:
 *   acpiphp_glue.c → [이 함수] → func_to_context() → hp.self
 */
static inline struct acpi_device *func_to_acpi_device(struct acpiphp_func *func)
{
	return func_to_context(func)->hp.self;
}

/* [한국어]
 * func_to_handle - PCI 함수 정보에서 ACPI 네임스페이스 핸들을 얻는다
 *
 * @func: acpiphp 함수 정보.
 * @return: 그 함수의 acpi_handle.
 *
 * 세 단 사슬의 마지막이다. func → func_to_context() → func_to_acpi_device()
 * → handle. _STA 나 _EJ0 같은 ACPI 메서드를 평가하려면 결국 핸들이 필요하고,
 * acpiphp 의 함수 정보에서 거기까지 가는 길을 이 한 줄로 접어 둔 것이다.
 *
 * 세 함수를 따로 두어 각 단계를 이름으로 드러낸 것이 이 헤더의 방식이다 —
 * 필요한 쪽이 중간 단계(context 나 acpi_device)를 직접 쓸 수도 있다.
 *
 * 실행 컨텍스트: ACPI 메서드 평가 경로. static inline.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 _STA / _EJ0 평가 → [이 함수]
 *     → func_to_acpi_device() → func_to_context()
 */
static inline acpi_handle func_to_handle(struct acpiphp_func *func)
{
	return func_to_acpi_device(func)->handle;
}

struct acpiphp_root_context {
	/* [한국어] ACPI 핫플러그 컨텍스트. 역시 **첫 필드**라 아래
	 *   to_acpiphp_root_context() 의 container_of 가 성립한다.
	 * 설정자/읽는 자: 위 acpiphp_context 의 hp 필드와 같은 역할이다.
	 * 값 범위: 유효한 acpi_hotplug_context.
	 * 동기화: ACPI 코어 규약을 따른다. */
	struct acpi_hotplug_context hp;
	/* [한국어] 이 루트 브리지에 대응하는 acpiphp_bridge.
	 * 설정자: 루트 브리지 등록 시.
	 * 읽는 자: 루트 브리지에 온 ACPI 이벤트를 처리할 때.
	 * 값 범위: 유효한 브리지 포인터.
	 * 동기화: 루트 브리지 수명 동안 불변.
	 * 이 구조체가 acpiphp_context 와 따로 존재하는 이유는, 루트 브리지에는
	 *   대응하는 PCI 함수가 없어 func 필드가 의미를 갖지 않기 때문이다.
	 *   그래서 호출자가 어느 종류의 컨텍스트인지 알고 맞는 변환 함수를 골라야 한다
	 *   (acpiphp_glue.c:214 의 설명 참조). */
	struct acpiphp_bridge *root_bridge;
};

/* [한국어]
 * to_acpiphp_root_context - ACPI 컨텍스트에서 루트 브리지 컨텍스트를 되찾는다
 *
 * @hp: ACPI 코어가 넘긴 핫플러그 컨텍스트.
 * @return: 그것을 첫 필드로 품고 있는 struct acpiphp_root_context.
 *
 * to_acpiphp_context() 의 루트 브리지 판이다. 두 함수가 따로 있는 이유는
 * 루트 브리지에 대응하는 PCI 함수가 없기 때문이다 — acpiphp_context 의
 * func 필드가 루트에서는 의미를 갖지 않으므로, 아예 다른 구조체를 쓴다.
 *
 * 같은 acpi_hotplug_context 포인터를 두 함수 중 어느 쪽에 넣느냐로 결과가
 * 완전히 달라지고, 어느 쪽인지는 코드가 판별해 주지 않는다. 호출자가
 * 루트인지 아닌지 알고 골라야 한다(acpiphp_glue.c:214 의 설명).
 *
 * 실행 컨텍스트: 루트 브리지 관련 ACPI 처리. static inline.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ACPI 코어의 루트 브리지 통지 → acpiphp_glue.c → [이 함수] → container_of()
 */
static inline struct acpiphp_root_context *to_acpiphp_root_context(struct acpi_hotplug_context *hp)
{
	return container_of(hp, struct acpiphp_root_context, hp);
}

/*
 * struct acpiphp_attention_info - device specific attention registration
 *
 * ACPI has no generic method of setting/getting attention status
 * this allows for device specific driver registration
 */
struct acpiphp_attention_info {
	/* [한국어] attention LED 를 켜고 끄는 콜백.
	 * 설정자: 플랫폼별 드라이버가 자기 함수를 넣는다
	 *   (예: acpiphp_ampere_altra.c:365 가 acpiphp_register_attention() 으로 등록).
	 * 읽는 자: acpiphp_core.c 의 attention 관련 sysfs 콜백.
	 * 값 범위: 유효한 함수 포인터. 등록 자체가 선택적이다.
	 * 동기화: 등록·해제가 acpiphp_core.c 에서 직렬화된다. */
	int (*set_attn)(struct hotplug_slot *slot, u8 status);
	/* [한국어] attention LED 의 현재 상태를 읽는 콜백.
	 * 설정자: set_attn 과 짝으로 함께 등록된다.
	 * 읽는 자: attention sysfs 읽기 경로.
	 * 값 범위: 유효한 함수 포인터.
	 * 동기화: set_attn 과 같다. */
	int (*get_attn)(struct hotplug_slot *slot, u8 *status);
	/* [한국어] 이 콜백들을 제공한 모듈.
	 * 설정자: 등록하는 드라이버가 THIS_MODULE 을 넣는다.
	 * 읽는 자: 콜백을 부르는 동안 모듈이 내려가지 못하게 참조를 잡는 데 쓴다.
	 * 값 범위: 유효한 module 포인터.
	 * 동기화: 모듈 참조 계수 자체가 원자적이다. */
	struct module *owner;
};

/* ACPI _STA method value (ignore bit 4; battery present) */
/* [한국어] _STA 가 반환한 값에서 볼 비트들의 마스크. 옆의 영어 주석대로 4번 비트
 * (배터리 존재)는 무시하고 하위 4비트만 본다.
 * 쓰이는 곳: acpiphp_glue.c:1271 — _STA 메서드가 없는 함수는 존재한다고
 * 간주해 이 값(존재+활성+표시+동작)을 만들어 넣는다. */
#define ACPI_STA_ALL			(0x0000000f)

/* slot flags */

/* [한국어] 이 슬롯이 켜져 있다는 표시. 슬롯의 flags 필드에 들어간다. */
#define SLOT_ENABLED		(0x00000001)
/* [한국어] 이 슬롯이 해체 중이라는 표시. 켜기 요청이 들어오면 이것부터 확인한다. */
#define SLOT_IS_GOING_AWAY	(0x00000002)

/* function flags */

/* [한국어] 이 함수에 ACPI _STA 메서드가 있다는 표시. 함수의 flags 필드에 들어간다. */
#define FUNC_HAS_STA		(0x00000001)
/* [한국어] 이 함수에 ACPI _EJ0(꺼내기) 메서드가 있다는 표시.
 * 슬롯을 뺄 때 이 플래그가 붙은 첫 함수로 _EJ0 를 부른다. */
#define FUNC_HAS_EJ0		(0x00000002)

/* function prototypes */

/* acpiphp_core.c */
/* [한국어] 플랫폼별 attention 콜백 쌍을 등록한다. ACPI 에는 attention LED 를 다루는
 * 표준 방법이 없어, 위 영어 주석대로 장치별 드라이버가 끼어들 자리를 만든 것이다. */
int acpiphp_register_attention(struct acpiphp_attention_info *info);
/* [한국어] 등록했던 콜백 쌍을 해제한다. */
int acpiphp_unregister_attention(struct acpiphp_attention_info *info);
/* [한국어] ACPI 쪽 슬롯을 sysfs 에 노출한다. glue 가 슬롯을 발견하면 이것을 부른다. */
int acpiphp_register_hotplug_slot(struct acpiphp_slot *slot, unsigned int sun);
/* [한국어] sysfs 노출을 되돌린다. */
void acpiphp_unregister_hotplug_slot(struct acpiphp_slot *slot);

/* [한국어] 슬롯을 켠다. acpiphp_core.c 의 enable_slot 콜백이 이것으로 위임한다. */
int acpiphp_enable_slot(struct acpiphp_slot *slot);
/* [한국어] 슬롯을 끈다. */
int acpiphp_disable_slot(struct acpiphp_slot *slot);
/* [한국어] 전원 상태를 조회한다. */
u8 acpiphp_get_power_status(struct acpiphp_slot *slot);
/* [한국어] 래치(걸쇠) 상태를 조회한다. */
u8 acpiphp_get_latch_status(struct acpiphp_slot *slot);
/* [한국어] 어댑터 존재 여부를 조회한다. */
u8 acpiphp_get_adapter_status(struct acpiphp_slot *slot);

/* variables */
/* [한국어] acpiphp 를 사용자가 꺼 두었는지.
 * 설정자: acpiphp_core.c:92 에 정의되고 :107 의 module_param_named 로
 *   "disable" 파라미터에 묶인다(0444 라 부팅 시에만 지정 가능).
 * 읽는 자: acpiphp_glue.c:1805 의 브리지 등록 사전 조건 검사와
 *   acpiphp_core.c:758 의 초기화 메시지.
 * 값 범위: false = 정상 동작, true = 사용자가 꺼 둠.
 * 동기화: 부팅 시 한 번 정해지고 이후 읽기만 하므로 락이 없다. */
extern bool acpiphp_disabled;

/* [한국어] 이중 포함 방지 블록의 끝. */
#endif /* _ACPIPHP_H */
