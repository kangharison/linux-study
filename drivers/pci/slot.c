// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2006 Matthew Wilcox <matthew@wil.cx>
 * Copyright (C) 2006-2009 Hewlett-Packard Development Company, L.P.
 *	Alex Chiang <achiang@hp.com>
 */

/*
 * [한국어 설명] 물리적 슬롯을 sysfs 에 노출하는 계층 (slot.c)
 *
 * === 파일의 역할 ===
 * "슬롯" 은 카드를 꽂는 물리적 자리다. 장치(pci_dev)와는 다른 개념이라
 * 별도의 객체가 필요하다 — 슬롯은 비어 있을 수도 있고, 하나의 슬롯에
 * 여러 function 이 있는 장치가 꽂힐 수도 있다.
 *
 * 이 파일은 struct pci_slot 을 관리하고 /sys/bus/pci/slots/ 아래에
 * 노출한다. 슬롯 번호는 펌웨어(ACPI _SUN 메서드)나 핫플러그 컨트롤러가
 * 알려 주는 값이며, 그것이 섀시에 인쇄된 번호와 일치한다.
 *
 * 왜 필요한가. 데이터센터에서 드라이브 하나가 고장 났을 때, 관리자는
 * "0000:65:00.0" 이 아니라 "몇 번 베이" 인지를 알아야 한다. 이 파일이
 * 그 대응을 제공한다.
 *
 * 여러 주체가 같은 슬롯을 등록할 수 있다는 점이 설계의 요점이다. ACPI 와
 * pciehp 가 같은 슬롯을 각자 알고 있을 수 있으므로, 참조 카운트로
 * 관리하고 마지막 참조가 사라질 때 해제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 핫플러그 드라이버(pciehp/acpiphp) 또는 ACPI 코드
 *         -> [이 파일] pci_create_slot(parent, slot_nr, name, hotplug)
 *            -> kobject 를 만들어 /sys/bus/pci/slots/<번호>/ 생성
 *            -> 같은 버스의 장치들에서 dev->slot 을 이 슬롯으로 연결
 *
 * 사용: pci.c 의 pci_dev_reset_slot_function() 이 dev->slot->hotplug 로
 *       그 슬롯을 관리하는 컨트롤러를 찾아 리셋을 요청한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_slot_mutex 로 목록을 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: hotplug/ 의 각 컨트롤러 드라이버, pci-acpi.c.
 * 아래쪽: kobject/sysfs, bus.c 의 장치 목록.
 * 공유 상태: struct pci_bus 의 slots 목록, struct pci_dev 의 slot 포인터,
 *   그리고 전역 pci_slot_mutex.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 하지만 U.2/EDSFF 백플레인의 NVMe 드라이브는 반드시 슬롯을 갖는다.
 * 그것이 있어야 두 가지가 성립한다.
 *   - 핫스왑: pciehp 가 그 슬롯의 Presence Detect 를 감시한다.
 *   - 슬롯 리셋: pci.c 의 pci_dev_reset_slot_function() 이 dev->slot 이
 *     NULL 이 아닐 때만 성립한다. FLR 이 통하지 않는 드라이브에서
 *     쓸 수 있는 대안이다.
 *
 * 반대로 M.2 나 납땜된 NVMe 는 슬롯이 없어 dev->slot 이 NULL 이고,
 * 위 두 기능이 모두 해당되지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_create_slot()     : 슬롯 객체를 만들거나, 이미 있으면 참조를 늘린다.
 *                         같은 슬롯을 여러 주체가 등록할 수 있어 필요하다.
 * pci_destroy_slot()    : 참조를 줄이고, 0 이 되면 실제로 해제한다.
 * pci_slot_release()    : 실제 해제. 그 슬롯에 속한 장치들의 slot 포인터를
 *                         NULL 로 되돌린다.
 * make_slot_name()      : 이름 충돌을 피해 유일한 이름을 만든다.
 *                         같은 번호가 두 번 등록되면 "-1" 을 붙인다.
 * pci_slot_attrs        : sysfs 속성. address(장치 주소)와
 *                         function 별 정보를 노출한다.
 * pci_dev_assign_slot() : 새로 발견된 장치를 이미 등록된 슬롯에 이어 준다.
 * get_slot()            : 같은 번호의 슬롯이 이미 있는지 찾는다.
 * rename_slot()         : 이미 있는 슬롯의 이름을 바꾼다.
 * bus_speed_read() / max_speed_read_file() / cur_speed_read_file() :
 *                         버스 속도를 사람이 읽을 문자열로 내보내는 핸들러.
 */

#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/err.h>
#include "pci.h"

struct kset *pci_slots_kset;
EXPORT_SYMBOL_GPL(pci_slots_kset);

/* [한국어]
 * pci_slot_attr_show - sysfs 읽기를 슬롯 전용 show 콜백으로 중계한다
 *
 * @kobj: 대상 kobject.
 * @attr: 읽을 속성.
 * @buf: 출력 버퍼.
 * @return: show 콜백의 결과, 또는 -EIO(읽기 콜백이 없을 때).
 *
 * sysfs 코어는 kobject 와 attribute 라는 일반 타입만 다룬다. 이 파일의
 * 콜백들은 struct pci_slot 을 받고 싶어 하므로, 두 번의 container_of 로
 * 타입을 내려 주는 중계가 필요하다.
 *
 * show 가 NULL 이면 -EIO 를 돌려주는 것은 쓰기 전용 속성에 읽기를 시도한
 * 경우를 위한 것이다. 이 파일의 세 속성은 모두 읽기 전용이라 실제로는
 * 언제나 콜백이 있다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   /sys/bus/pci/slots 아래 속성 파일 읽기 → sysfs 코어 → [이 함수]
 *     → attribute->show()
 */
static ssize_t pci_slot_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	/* [한국어] kobject 에서 그것을 품은 슬롯을 되찾는다. */
	struct pci_slot *slot = to_pci_slot(kobj);
	/* [한국어] attribute 에서 그것을 품은 슬롯 전용 속성 구조체를 되찾는다. 두 번의
	 * container_of 로 sysfs 코어의 일반 타입에서 이 파일의 타입으로 내려온다. */
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);
	/* [한국어] show 콜백이 있으면 부르고, 없으면 입출력 오류로 답한다. 쓰기 전용 속성에
	 * 읽기를 시도한 경우가 여기 해당한다. */
	return attribute->show ? attribute->show(slot, buf) : -EIO;
}

/* [한국어]
 * pci_slot_attr_store - sysfs 쓰기를 슬롯 전용 store 콜백으로 중계한다
 *
 * @kobj: 대상 kobject.
 * @attr: 쓸 속성.
 * @buf: 입력 버퍼.
 * @len: 입력 길이.
 * @return: store 콜백의 결과, 또는 -EIO.
 *
 * pci_slot_attr_show() 의 짝이다. 이 파일이 정의하는 세 속성은 모두 읽기
 * 전용이라 store 가 NULL 이고, 따라서 이 함수는 언제나 -EIO 를 돌려준다.
 *
 * 그래도 두는 이유는 sysfs_ops 규약이 두 함수를 모두 요구하기 때문이며,
 * 바깥 모듈(핫플러그 드라이버)이 쓰기 가능한 슬롯 속성을 추가할 여지를
 * 남겨 두는 것이기도 하다.
 *
 * 실행 컨텍스트: sysfs 쓰기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sysfs 쓰기 → sysfs 코어 → [이 함수] → attribute->store()
 */
static ssize_t pci_slot_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t len)
{
	/* [한국어] kobject 에서 슬롯을, */
	struct pci_slot *slot = to_pci_slot(kobj);
	/* [한국어] attribute 에서 속성 구조체를 되찾는다. */
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);
	/* [한국어] store 콜백이 있으면 부르고, 없으면 오류. 이 파일의 세 속성은 모두
	 * 읽기 전용이라 실제로는 언제나 -EIO 다. */
	return attribute->store ? attribute->store(slot, buf, len) : -EIO;
}

/* [한국어] sysfs 코어가 이 kobject 종류의 속성에 접근할 때 쓸 두 함수. */
static const struct sysfs_ops pci_slot_sysfs_ops = {
	/* [한국어] 읽기. */
	.show = pci_slot_attr_show,
	.store = pci_slot_attr_store,
};

/* [한국어] 슬롯의 주소를 사람이 읽을 형식으로 내보낸다. */
/* [한국어]
 * address_read_file - 슬롯의 PCI 주소를 사람이 읽을 형식으로 내보낸다
 *
 * @slot: 대상 슬롯.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * 세 가지 형식을 슬롯 번호로 가른다.
 *
 * 0xff 이면 도메인:버스 두 자리만 찍는다. 어느 특정 장치도 가리키지 않는
 * 슬롯이라는 뜻이다.
 * PCI_SLOT_ALL_DEVICES 이면 도메인:버스:00 세 자리를 찍되 장치 자리를 00 으로
 * 고정한다. 위 경우와 달리 자리를 남겨 두는 것이 요점으로, 사용자 공간이
 * 필드 수만 세어 두 경우를 구분할 수 있다.
 * 그 밖에는 실제 장치 번호를 찍는다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../address → pci_slot_attr_show() → [이 함수] → sysfs_emit()
 */
static ssize_t address_read_file(struct pci_slot *slot, char *buf)
{
	/* [한국어] 0xff 는 어느 특정 장치도 가리키지 않는 값이다. */
	if (slot->number == 0xff)
		/* [한국어] 이때는 도메인:버스 까지만 찍는다. 장치 번호를 뺀 형식이 "이 슬롯은
		 * 버스 전체를 뜻한다" 는 표시가 된다. */
		return sysfs_emit(buf, "%04x:%02x\n",
				  pci_domain_nr(slot->bus),
				  slot->bus->number);

	/*
	 * Preserve legacy ABI expectations that hotplug drivers that manage
	 * multiple devices per slot emit 0 for the device number.
	 */
	if (slot->number == PCI_SLOT_ALL_DEVICES)
		/* [한국어] 모든 장치를 뜻하는 슬롯이면 장치 번호 자리에 00 을 고정으로 찍는다.
		 * 위 0xff 경우와 달리 자리를 남겨 두어, 사용자 공간이 형식을 파싱할 때
		 * 필드 수로 두 경우를 구분할 수 있다. */
		return sysfs_emit(buf, "%04x:%02x:00\n",
				  pci_domain_nr(slot->bus),
				  slot->bus->number);

	/* [한국어] 보통은 도메인:버스:장치 세 자리를 모두 찍는다. */
	return sysfs_emit(buf, "%04x:%02x:%02x\n",
			  pci_domain_nr(slot->bus),
			  slot->bus->number,
			  slot->number);
}

/* [한국어]
 * bus_speed_read - 속도 값을 문자열로 바꿔 내보낸다
 *
 * @speed: PCI 버스 속도 열거값.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * 아래 두 속성 함수가 공유하는 헬퍼다. pci_speed_string() 이
 * "8.0 GT/s PCIe" 같은 사람이 읽을 표현을 만들어 준다.
 *
 * 첫 인자가 struct pci_slot 이 아니라 속도 값 자체라는 점이 이 파일의 다른
 * show 함수들과 다르다. 그래서 속성 표에 직접 넣을 수 없고, 아래 두 래퍼가
 * 슬롯에서 필드를 꺼내 넘긴다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   max_speed_read_file() / cur_speed_read_file() → [이 함수]
 *     → pci_speed_string() → sysfs_emit()
 */
static ssize_t bus_speed_read(enum pci_bus_speed speed, char *buf)
{
	/* [한국어] 속도 값을 문자열로 바꿔 내보낸다. pci_speed_string() 이 "8.0 GT/s PCIe"
	 * 같은 사람이 읽을 표현을 준다. */
	return sysfs_emit(buf, "%s\n", pci_speed_string(speed));
}

/* [한국어]
 * max_speed_read_file - 이 슬롯이 낼 수 있는 최대 속도를 내보낸다
 *
 * @slot: 대상 슬롯.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * 슬롯이 속한 버스의 max_bus_speed 를 문자열로 내보낸다.
 *
 * 아래 cur_speed_read_file() 과 한 글자만 다르다. 두 값을 나란히 노출하는
 * 이유는 사용자가 "이 슬롯은 Gen4 를 낼 수 있는데 지금 Gen1 으로 협상되어
 * 있다" 같은 상태를 진단할 수 있게 하려는 것이다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../max_bus_speed → pci_slot_attr_show() → [이 함수] → bus_speed_read()
 */
static ssize_t max_speed_read_file(struct pci_slot *slot, char *buf)
{
	/* [한국어] 이 슬롯이 속한 버스가 낼 수 있는 최대 속도. */
	return bus_speed_read(slot->bus->max_bus_speed, buf);
}

/* [한국어]
 * cur_speed_read_file - 이 슬롯이 현재 협상한 속도를 내보낸다
 *
 * @slot: 대상 슬롯.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * max_speed_read_file() 의 짝이며 읽는 필드만 다르다. 링크가 신호 품질
 * 문제로 낮은 속도로 내려앉았을 때 그 사실이 여기 드러난다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../cur_bus_speed → pci_slot_attr_show() → [이 함수] → bus_speed_read()
 */
static ssize_t cur_speed_read_file(struct pci_slot *slot, char *buf)
{
	/* [한국어] 현재 협상된 속도. 두 함수가 같은 헬퍼를 쓰고 읽는 필드만 다르다. */
	return bus_speed_read(slot->bus->cur_bus_speed, buf);
}

/* [한국어]
 * pci_slot_release - 마지막 참조가 사라진 슬롯을 실제로 해제한다
 *
 * @kobj: 해제되는 kobject.
 *
 * kobject 참조 카운트가 0 이 되면 kobject 코어가 부르는 소멸자다.
 *
 * 핵심은 이 슬롯을 가리키던 장치들의 포인터를 지우는 것이다. 그러지 않으면
 * 해제된 메모리를 가리키는 떠도는 포인터가 남는다. 순회 조건이
 * pci_create_slot() 의 연결 조건과 정확히 대칭이라, 심어 준 것을 그대로 거둔다.
 *
 * [상류 코드 관찰] 이 순회는 pci_bus_sem 을 잡지 않는다. 같은 목록을 훑는
 * pci_create_slot() 은 down_read/up_read 로 감싸는데 여기서는 그러지 않으며,
 * 그 차이가 의도적인지는 이 트리에서 확인할 수 없다.
 *
 * 버스 참조를 놓는 것과 구조체를 해제하는 것도 여기서 한다 — pci_create_slot()
 * 이 pci_bus_get() 으로 올린 것의 짝이다.
 *
 * 실행 컨텍스트: kobject 참조 해제 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_destroy_slot() → kobject_put() → [이 함수]
 *     → list_for_each_entry() → pci_bus_put() → kfree()
 */
static void pci_slot_release(struct kobject *kobj)
{
	/* [한국어] 슬롯 포인터를 지울 장치들을 순회할 커서. */
	struct pci_dev *dev;
	/* [한국어] kobject 에서 슬롯을 되찾는다. */
	struct pci_slot *slot = to_pci_slot(kobj);

	/* [한국어] 해제 사실을 디버그 로그에 남긴다. */
	dev_dbg(&slot->bus->dev, "dev %02x, released physical slot %s\n",
		slot->number, pci_slot_name(slot));

	down_read(&pci_bus_sem);
	/* [한국어] 이 버스의 장치들을 훑으며, */
	list_for_each_entry(dev, &slot->bus->devices, bus_list)
		/* [한국어] 이 슬롯에 속한 장치를 찾아, */
		if (slot->number == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot->number)
			/* [한국어] 슬롯 포인터를 지운다. 이렇게 하지 않으면 해제된 슬롯을 가리키는
			 * 떠도는 포인터가 남는다.
			 * [상류 코드 관찰] 이 순회는 pci_bus_sem 을 잡지 않는다. 슬롯 생성 쪽
			 * (pci_create_slot)이 같은 목록을 읽기 잠금 아래에서 훑는 것과 대비된다. */
			dev->slot = NULL;
	up_read(&pci_bus_sem);

	list_del(&slot->list);
	pci_bus_put(slot->bus);

	kfree(slot);
}

/* [한국어] 주소 속성. __ATTR 로 이름과 모드, 콜백을 한 번에 정한다. */
static struct pci_slot_attribute pci_slot_attr_address =
	__ATTR(address, S_IRUGO, address_read_file, NULL);
/* [한국어] 최대 속도 속성. */
static struct pci_slot_attribute pci_slot_attr_max_speed =
	__ATTR(max_bus_speed, S_IRUGO, max_speed_read_file, NULL);
/* [한국어] 현재 속도 속성. */
static struct pci_slot_attribute pci_slot_attr_cur_speed =
	__ATTR(cur_bus_speed, S_IRUGO, cur_speed_read_file, NULL);

/* [한국어] 위 셋을 묶은 기본 속성 목록. 슬롯 kobject 를 만들면 이 셋이 함께 생긴다. */
static struct attribute *pci_slot_default_attrs[] = {
	&pci_slot_attr_address.attr,
	&pci_slot_attr_max_speed.attr,
	&pci_slot_attr_cur_speed.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pci_slot_default);

/* [한국어] 슬롯 kobject 의 종류를 정의한다. */
static const struct kobj_type pci_slot_ktype = {
	/* [한국어] 위에서 만든 show/store 중계 함수. */
	.sysfs_ops = &pci_slot_sysfs_ops,
	.release = &pci_slot_release,
	.default_groups = pci_slot_default_groups,
};

/* [한국어] 이름 충돌을 피해 유일한 이름을 만든다. */
/* [한국어]
 * make_slot_name - 이미 있는 이름과 겹치지 않는 슬롯 이름을 만든다
 *
 * @name: 요청받은 이름.
 * @return: 새로 할당한 유일한 이름, 또는 NULL(할당 실패).
 *
 * 여러 주체가 같은 번호의 슬롯을 등록하려 할 때 sysfs 이름이 겹치는 것을
 * 막는다. 겹치면 "이름-1", "이름-2" 처럼 번호를 붙인다.
 *
 * 버퍼 크기 관리가 이 함수의 요령이다. 처음에는 원래 이름 + "-N" 을 담을
 * 만큼(len = strlen + 2)만 잡아 두고, 번호가 자릿수 경계(max)에 닿을 때마다
 * 한 글자 늘려 다시 할당한다. 9 다음 10 에서 한 글자가 더 필요해지는 것을
 * `if (dup == max) { len++; max *= 10; }` 세 줄이 처리한다.
 *
 * kset_find_obj() 가 찾은 kobject 의 참조를 올려 주므로 곧바로 놓아야 한다.
 *
 * [상류 코드 관찰] 재할당 실패 시 break 로 루프를 벗어나는데, 그때 new_name 은
 * 이미 NULL 이라 NULL 이 반환된다. 호출자가 그것을 실패로 다루므로 결과는
 * 맞지만, 앞서 kfree 한 이름을 잃는 셈이다.
 *
 * 실행 컨텍스트: 슬롯 생성 경로. pci_slot_mutex 아래이며 잠들 수 있다.
 *
 * 에러 경로: 할당 실패 시 NULL. 호출자가 -ENOMEM 으로 바꾼다.
 *
 * 호출 체인:
 *   pci_create_slot() / rename_slot() → [이 함수]
 *     → kstrdup() → kset_find_obj() → kmalloc() → sprintf()
 */
static char *make_slot_name(const char *name)
{
	/* [한국어] 만들어 낼 이름. */
	char *new_name;
	/* [한국어] 버퍼 길이, 자릿수 경계, 시도 중인 번호. */
	int len, max, dup;

	/* [한국어] 먼저 요청받은 이름 그대로 복사해 본다. 충돌이 없으면 이것이 그대로 쓰인다. */
	new_name = kstrdup(name, GFP_KERNEL);
	/* [한국어] 할당 실패면, */
	if (!new_name)
		return NULL;

	/*
	 * Make sure we hit the realloc case the first time through the
	 * loop.  'len' will be strlen(name) + 3 at that point which is
	 * enough space for "name-X" and the trailing NUL.
	 */
	len = strlen(name) + 2;
	/* [한국어] 자릿수가 늘어날 때마다 버퍼를 다시 잡을 기준. */
	max = 1;
	/* [한국어] 붙일 번호. 1 부터 시작한다. */
	dup = 1;

	/* [한국어] 충돌이 없어질 때까지 반복한다. */
	for (;;) {
		/* [한국어] 같은 이름의 kobject 를 담을 곳. */
		struct kobject *dup_slot;
		/* [한국어] 이미 그 이름이 있는지 찾는다. 찾으면 참조가 올라간다. */
		dup_slot = kset_find_obj(pci_slots_kset, new_name);
		/* [한국어] 없으면 이 이름을 쓸 수 있다. */
		if (!dup_slot)
			break;
		kobject_put(dup_slot);
		/* [한국어] 번호가 자릿수 경계에 닿았으면, */
		if (dup == max) {
			/* [한국어] 버퍼를 한 글자 늘리고, */
			len++;
			/* [한국어] 다음 경계를 열 배로 올린다. 9 다음 10 에서 한 글자가 더 필요해지는 것을
			 * 이 두 줄이 처리한다. */
			max *= 10;
			kfree(new_name);
			/* [한국어] 늘린 크기로 다시 할당한다. */
			new_name = kmalloc(len, GFP_KERNEL);
			/* [한국어] 실패하면 루프를 벗어나 NULL 을 반환하게 된다. */
			if (!new_name)
				break;
		}
		/* [한국어] "이름-번호" 형식으로 만들고 번호를 하나 올린다. 다음 반복에서 다시
		 * 충돌을 확인한다. */
		sprintf(new_name, "%s-%d", name, dup++);
	}

	/* [한국어] 충돌 없는 이름(또는 할당 실패 시 NULL)을 돌려준다. */
	return new_name;
}

/* [한국어]
 * rename_slot - 이미 있는 슬롯의 sysfs 이름을 바꾼다
 *
 * @slot: 대상 슬롯.
 * @name: 새 이름.
 * @return: 0 = 성공(또는 이미 같은 이름), -ENOMEM, kobject_rename() 의 오류.
 *
 * 핫플러그 드라이버가 나중에 붙을 때 쓰인다. 먼저 만들어진 슬롯은 임시
 * 이름을 갖고 있을 수 있고, 핫플러그 드라이버가 보드에 적힌 제대로 된
 * 이름을 알려 주는 것이다.
 *
 * 이름이 이미 같으면 곧바로 성공으로 답한다. 그 검사가 없으면
 * make_slot_name() 이 자기 자신을 충돌로 보고 "-1" 을 붙여 버린다.
 *
 * 실행 컨텍스트: 슬롯 생성 경로. pci_slot_mutex 아래.
 *
 * 에러 경로: 이름 생성 실패는 -ENOMEM, 그 밖은 kobject_rename() 의 결과.
 * 어느 쪽이든 호출자가 슬롯 생성을 중단한다.
 *
 * 호출 체인:
 *   pci_create_slot() → [이 함수] → make_slot_name() → kobject_rename()
 */
static int rename_slot(struct pci_slot *slot, const char *name)
{
	/* [한국어] 결과. 이름이 같으면 0 으로 남는다. */
	int result = 0;
	/* [한국어] 새로 만든 이름. */
	char *slot_name;

	/* [한국어] 이미 같은 이름이면, */
	if (strcmp(pci_slot_name(slot), name) == 0)
		/* [한국어] 할 일이 없다. */
		return result;

	/* [한국어] 충돌을 피한 새 이름을 만든다. */
	slot_name = make_slot_name(name);
	/* [한국어] 실패하면, */
	if (!slot_name)
		return -ENOMEM;

	/* [한국어] kobject 이름을 바꾼다. sysfs 항목의 이름도 함께 바뀐다. */
	result = kobject_rename(&slot->kobj, slot_name);
	kfree(slot_name);

	/* [한국어] 결과를 돌려준다. */
	return result;
}

/* [한국어]
 * pci_dev_assign_slot - 새로 나타난 장치를 이미 등록된 슬롯에 이어 준다
 *
 * @dev: 방금 열거된 장치.
 *
 * 슬롯과 장치는 서로 다른 시점에 나타날 수 있다. 슬롯이 먼저면
 * pci_create_slot() 이 그때 있던 장치들을 이어 주고, 장치가 나중이면 이
 * 함수가 이어 준다. 두 함수의 순회 조건이 같은 것이 그 대칭이다.
 *
 * break 가 없어 마지막으로 맞는 슬롯이 남는데, 한 장치가 여러 슬롯에 속할
 * 수 없으므로 실제로는 하나만 맞는다.
 *
 * 실행 컨텍스트: 장치 열거 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 맞는 슬롯이 없으면 dev->slot 이 NULL 로 남고, 그것이
 * 정상적인 상태다 — 슬롯이 없는 장치가 훨씬 많다.
 *
 * 호출 체인:
 *   pci_device_add() 계열 → [이 함수]
 */
void pci_dev_assign_slot(struct pci_dev *dev)
{
	/* [한국어] 이 장치가 속할 슬롯을 찾을 커서. */
	struct pci_slot *slot;

	mutex_lock(&pci_slot_mutex);
	/* [한국어] 이 버스에 등록된 슬롯들을 훑으며, */
	list_for_each_entry(slot, &dev->bus->slots, list)
		/* [한국어] 모든 장치를 뜻하는 슬롯이거나, */
		if (slot->number == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot->number)
			/* [한국어] 장치 번호가 맞는 슬롯을 찾으면 연결한다. break 가 없어 마지막으로 맞는
			 * 슬롯이 남는데, 한 장치가 여러 슬롯에 속할 수 없으므로 실제로는 하나만
			 * 맞는다. */
			dev->slot = slot;
	mutex_unlock(&pci_slot_mutex);
}

/* [한국어]
 * get_slot - 이 버스에서 같은 번호의 슬롯을 찾아 참조를 올려 돌려준다
 *
 * @parent: 검색할 버스.
 * @slot_nr: 찾는 슬롯 번호.
 * @return: 찾은 슬롯(참조가 올라간 상태), 없으면 NULL.
 *
 * pci_create_slot() 이 "이미 있는 슬롯인가" 를 판단하는 데 쓴다.
 *
 * 참조를 올려 반환하는 것이 규약이다. 호출자는 그 슬롯을 그대로 돌려주거나
 * (이미 있는 경우) 오류 경로에서 놓는다.
 *
 * 호출자가 pci_slot_mutex 를 쥔 상태에서만 불리므로 이 함수 자체는 잠금을
 * 하지 않는다.
 *
 * 실행 컨텍스트: 슬롯 생성 경로. pci_slot_mutex 아래.
 *
 * 에러 경로: 없다. 못 찾으면 NULL 이다.
 *
 * 호출 체인:
 *   pci_create_slot() → [이 함수] → kobject_get()
 */
static struct pci_slot *get_slot(struct pci_bus *parent, int slot_nr)
{
	/* [한국어] 찾을 커서. */
	struct pci_slot *slot;

	/* We already hold pci_slot_mutex */
	list_for_each_entry(slot, &parent->slots, list)
		/* [한국어] 슬롯 번호가 맞으면, */
		if (slot->number == slot_nr) {
			kobject_get(&slot->kobj);
			/* [한국어] 참조를 올린 채 돌려준다. 호출자가 놓을 책임을 진다. */
			return slot;
		}

	return NULL;
}

/**
 * pci_create_slot - create or increment refcount for physical PCI slot
 * @parent: struct pci_bus of parent bridge
 * @slot_nr: PCI_SLOT(pci_dev->devfn), -1 for placeholder, or
 *	PCI_SLOT_ALL_DEVICES
 * @name: user visible string presented in /sys/bus/pci/slots/<name>
 * @hotplug: set if caller is hotplug driver, NULL otherwise
 *
 * PCI slots have first class attributes such as address, speed, width,
 * and a &struct pci_slot is used to manage them. This interface will
 * either return a new &struct pci_slot to the caller, or if the pci_slot
 * already exists, its refcount will be incremented.
 *
 * Slots are uniquely identified by a @pci_bus, @slot_nr tuple.
 *
 * There are known platforms with broken firmware that assign the same
 * name to multiple slots. Workaround these broken platforms by renaming
 * the slots on behalf of the caller. If firmware assigns name N to
 * multiple slots:
 *
 * The first slot is assigned N
 * The second slot is assigned N-1
 * The third slot is assigned N-2
 * etc.
 *
 * Placeholder slots:
 * In most cases, @pci_bus, @slot_nr will be sufficient to uniquely identify
 * a slot. There is one notable exception - pSeries (rpaphp), where the
 * @slot_nr cannot be determined until a device is actually inserted into
 * the slot. In this scenario, the caller may pass -1 for @slot_nr.
 *
 * The following semantics are imposed when the caller passes @slot_nr ==
 * -1. First, we no longer check for an existing %struct pci_slot, as there
 * may be many slots with @slot_nr of -1.  The other change in semantics is
 * user-visible, which is the 'address' parameter presented in sysfs will
 * consist solely of a dddd:bb tuple, where dddd is the PCI domain of the
 * %struct pci_bus and bb is the bus number. In other words, the devfn of
 * the 'placeholder' slot will not be displayed.
 *
 * Bus-wide slots:
 * For PCIe hotplug, the physical slot encompasses the entire secondary
 * bus, not just a single device number. If the device supports ARI and ARI
 * Forwarding is enabled in the upstream bridge, a multi-function device
 * may include functions that appear to have several different device
 * numbers, i.e., PCI_SLOT() values.  Pass @slot_nr == PCI_SLOT_ALL_DEVICES
 * to create a slot that matches all devices on the bus. Unlike placeholder
 * slots, bus-wide slots go through normal slot lookup and reuse existing
 * slots if present.
 */
struct pci_slot *pci_create_slot(struct pci_bus *parent, int slot_nr,
				 const char *name,
				 struct hotplug_slot *hotplug)
{
	/* [한국어] 슬롯에 연결할 장치들을 순회할 커서. */
	struct pci_dev *dev;
	/* [한국어] 만들거나 찾은 슬롯. */
	struct pci_slot *slot;
	/* [한국어] 오류 코드. */
	int err = 0;
	/* [한국어] 만든 이름. out 라벨에서 해제한다. */
	char *slot_name = NULL;

	mutex_lock(&pci_slot_mutex);

	/* [한국어] -1 은 "자리만 만들어 달라" 는 뜻이다. 기존 슬롯을 찾지 않고 곧장
	 * 새로 만든다. */
	if (slot_nr == -1)
		goto placeholder;

	/*
	 * Hotplug drivers are allowed to rename an existing slot,
	 * but only if not already claimed.
	 */
	slot = get_slot(parent, slot_nr);
	/* [한국어] 같은 번호의 슬롯이 이미 있으면, */
	if (slot) {
		/* [한국어] 이번 호출이 핫플러그 등록이면, */
		if (hotplug) {
			/* [한국어] 이미 핫플러그가 등록되어 있는지 본다. 하나의 슬롯에 두 핫플러그
			 * 드라이버가 붙을 수는 없다. */
			if (slot->hotplug) {
				err = -EBUSY;
				goto put_slot;
			}
			/* [한국어] 이름을 바꿔 준다. 먼저 만들어진 슬롯은 임시 이름을 갖고 있을 수 있어,
			 * 핫플러그 드라이버가 제대로 된 이름을 알려 주는 것이다. */
			err = rename_slot(slot, name);
			/* [한국어] 실패하면, */
			if (err)
				goto put_slot;
		}
		goto out;
	}

placeholder:
	slot = kzalloc_obj(*slot);
	/* [한국어] 새로 만들 차례다. */
	if (!slot) {
		err = -ENOMEM;
		goto err;
	}

	/* [한국어] 버스 참조를 올린다. 슬롯이 살아 있는 동안 버스도 살아 있어야 한다. */
	slot->bus = pci_bus_get(parent);
	/* [한국어] 슬롯 번호를 기록한다. */
	slot->number = slot_nr;

	/* [한국어] 이 kobject 가 속할 집합을 지정한다. /sys/bus/pci/slots 아래에 나타나게 된다. */
	slot->kobj.kset = pci_slots_kset;

	/* [한국어] 충돌 없는 이름을 만든다. */
	slot_name = make_slot_name(name);
	/* [한국어] 실패하면, */
	if (!slot_name) {
		err = -ENOMEM;
		pci_bus_put(slot->bus);
		kfree(slot);
		goto err;
	}

	INIT_LIST_HEAD(&slot->list);
	/* [한국어] 버스의 슬롯 목록에 넣는다. kobject 등록 **전에** 넣는 순서인데,
	 * 아래 실패 경로가 kobject_put 으로 정리하면서 release 콜백이 목록에서
	 * 빼 주기를 기대하기 때문이다. */
	list_add(&slot->list, &parent->slots);

	/* [한국어] kobject 를 초기화하고 sysfs 에 등록한다. 이 호출이 성공하면
	 * /sys/bus/pci/slots/<이름> 디렉터리와 세 속성 파일이 생긴다. */
	err = kobject_init_and_add(&slot->kobj, &pci_slot_ktype, NULL,
				   "%s", slot_name);
	/* [한국어] 실패하면, */
	if (err)
		goto put_slot;

	down_read(&pci_bus_sem);
	/* [한국어] 이 버스의 장치들을 훑으며, */
	list_for_each_entry(dev, &parent->devices, bus_list)
		/* [한국어] 모든 장치를 뜻하는 슬롯이거나, */
		if (slot_nr == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot_nr)
			/* [한국어] 번호가 맞는 장치에 슬롯 포인터를 심는다. 슬롯이 생긴 시점에 이미
			 * 열거된 장치들을 이어 주는 것으로, 나중에 나타나는 장치는
			 * pci_dev_assign_slot() 이 이어 준다. */
			dev->slot = slot;
	up_read(&pci_bus_sem);

	/* [한국어] 생성 사실을 디버그 로그에 남긴다. */
	dev_dbg(&parent->dev, "dev %02x, created physical slot %s\n",
		slot_nr, pci_slot_name(slot));

out:
	kfree(slot_name);
	mutex_unlock(&pci_slot_mutex);
	/* [한국어] 만들었거나 찾은 슬롯을 돌려준다. */
	return slot;

put_slot:
	kobject_put(&slot->kobj);
err:
	slot = ERR_PTR(err);
	goto out;
}
EXPORT_SYMBOL_GPL(pci_create_slot);

/**
 * pci_destroy_slot - decrement refcount for physical PCI slot
 * @slot: struct pci_slot to decrement
 *
 * %struct pci_slot is refcounted, so destroying them is really easy; we
 * just call kobject_put on its kobj and let our release methods do the
 * rest.
 */
void pci_destroy_slot(struct pci_slot *slot)
{
	/* [한국어] 참조 카운트를 줄인 결과를 남긴다. 여러 주체가 같은 슬롯을 등록할 수 있어,
	 * 마지막 참조가 사라져야 실제로 해제된다. */
	dev_dbg(&slot->bus->dev, "dev %02x, dec refcount to %d\n",
		slot->number, kref_read(&slot->kobj.kref) - 1);

	mutex_lock(&pci_slot_mutex);
	kobject_put(&slot->kobj);
	mutex_unlock(&pci_slot_mutex);
}
EXPORT_SYMBOL_GPL(pci_destroy_slot);

/* [한국어]
 * pci_slot_init - /sys/bus/pci/slots 디렉터리를 만든다
 *
 * @return: 0 = 성공, -ENOMEM = kset 생성 실패.
 *
 * 부팅 시 한 번 불려 모든 슬롯이 나타날 자리를 마련한다.
 *
 * kset_create_and_add() 에 부모를 NULL 로 주고 kset 만 지정하는 것이
 * /sys/bus/pci/slots 경로를 만드는 방법이다. 부모를 NULL 로 두면 kset 이
 * 가리키는 곳 아래에 놓인다.
 *
 * 실패해도 시스템은 동작한다 — 슬롯 정보를 sysfs 로 볼 수 없을 뿐이므로,
 * 오류를 남기되 부팅을 막지는 않는다.
 *
 * 실행 컨텍스트: 서브시스템 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: -ENOMEM 을 반환하며, 그 뒤 모든 pci_create_slot() 이
 * pci_slots_kset 이 NULL 인 채로 동작하게 된다.
 *
 * 호출 체인:
 *   subsys_initcall → [이 함수] → bus_get_kset() → kset_create_and_add()
 */
static int pci_slot_init(void)
{
	/* [한국어] PCI 버스 타입의 kset. */
	struct kset *pci_bus_kset;

	/* [한국어] 그 kset 을 얻어, */
	pci_bus_kset = bus_get_kset(&pci_bus_type);
	/* [한국어] 그 아래 "slots" 집합을 만든다. 부모를 NULL 로 주고 kset 만 지정하는 것이
	 * /sys/bus/pci/slots 경로를 만드는 방법이다. */
	pci_slots_kset = kset_create_and_add("slots", NULL,
					    &pci_bus_kset->kobj);
	/* [한국어] 실패하면, */
	if (!pci_slots_kset) {
		/* [한국어] 오류를 남긴다. 슬롯 기능 없이도 시스템은 동작하므로 부팅을 막지는 않는다. */
		pr_err("PCI: Slot initialization failure\n");
		return -ENOMEM;
	}
	return 0;
}

subsys_initcall(pci_slot_init);
