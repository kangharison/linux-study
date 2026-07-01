// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2006 Matthew Wilcox <matthew@wil.cx>
 * Copyright (C) 2006-2009 Hewlett-Packard Development Company, L.P.
 *	Alex Chiang <achiang@hp.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/slot.c)은 PCI 물리 슬롯(struct pci_slot)을 생성,
 * 제거하고, sysfs(/sys/bus/pci/slots/<slot_name>/)를 통해 사용자 공간에
 * 노출하는 기능을 제공한다.
 * NVMe SSD는 PCIe endpoint로서 Root Port 아래 특정 slot(device 번호)에
 * 연결되므로, 이 슬롯 관리 코드는 NVMe 장치의 물리적 위치, 핫플러그,
 * 전원/EDR(Error Disconnect Recover), 속도 정보 등과 직접 맞닿아 있다.
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c)가 slot.c를 직접 호출하지는
 * 않지만, 다음과 같은 NVMe 동작들이 slot 정보에 의존한다.
 *   - NVMe 장치의 sysfs 경로 및 slot 이름(address, max_bus_speed,
 *     cur_bus_speed 속성) 노출
 *   - PCIe 핫플러그/사용자 공간 sysfs triggered remove 시 slot 단위로
 *     NVMe 장치가 분리됨
 *   - EDR(ACPI Error Disconnect Recover)이나 Surprise Remove 시 slot에
 *     속한 NVMe 디바이스의 link_down 처리
 *   - 전원 제어(power_control), D3cold, PERST# 등 slot 단위 전원 정책의
 *     사용자 공간 인터페이스
 *   - RCEC(Root Complex Event Collector)나 AER 등에서 slot 단위 이벤트
 *     전파
 * 주요 호출 경로(커널 부팅 및 NVMe probe 시):
 *   pci_scan_bus -> pci_scan_slot -> pci_create_slot
 *   nvme_probe(struct pci_dev *dev)에서 dev->slot이 이미 할당되어 있음
 *   사용자 공간: /sys/bus/pci/slots/<slot>/power -> slot 단위 전원 제어
 *              -> NVMe device reset/removal
 *   핫플러그: pciehp, acpi_pci_hp 등 -> pci_create_slot/pci_destroy_slot
 * ===================================================================
 */

#include <linux/kobject.h>	/* NVMe: sysfs kobject를 위한 헤더 */
#include <linux/slab.h>		/* NVMe: kzalloc/kfree 등 메모리 할당 */
#include <linux/pci.h>		/* NVMe: PCI 핵심 구조체 및 함수 */
#include <linux/err.h>		/* NVMe: ERR_PTR 등 오류 처리 매크로 */
#include "pci.h"		/* NVMe: PCI 서브시스템 내부 헤더 */

struct kset *pci_slots_kset;	/* NVMe: /sys/bus/pci/slots 디렉터리의 kset */
EXPORT_SYMBOL_GPL(pci_slots_kset);	/* NVMe: 핫플러그 드라이버 등 외부 모듈에서 slot kset 접근 허용 */

/*
 * pci_slot_attr_show:
 *   slot의 sysfs 속성(attribute) 읽기 콜백이다.
 *   NVMe 관점에서 /sys/bus/pci/slots/<slot>/address, max_bus_speed,
 *   cur_bus_speed 등을 읽을 때 이 함수가 호출된다.
 */
static ssize_t pci_slot_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct pci_slot *slot = to_pci_slot(kobj);	/* NVMe: kobject에서 pci_slot 구조체 획득 */
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);	/* NVMe: 일반 attribute를 pci_slot_attribute로 변환 */
	return attribute->show ? attribute->show(slot, buf) : -EIO;	/* NVMe: show 콜백이 있으면 호출, 없으면 I/O 오류 반환 */
}

/*
 * pci_slot_attr_store:
 *   slot의 sysfs 속성 쓰기 콜백이다.
 *   NVMe 관점에서 /sys/bus/pci/slots/<slot>/power 또는 attention 등
 *   쓰기 가능 속성에 값을 쓸 때 사용될 수 있다.
 */
static ssize_t pci_slot_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t len)
{
	struct pci_slot *slot = to_pci_slot(kobj);	/* NVMe: kobject로부터 pci_slot 획득 */
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);	/* NVMe: slot 속성 구조체로 변환 */
	return attribute->store ? attribute->store(slot, buf, len) : -EIO;	/* NVMe: store 콜백이 있으면 호출, 없으면 I/O 오류 반환 */
}

static const struct sysfs_ops pci_slot_sysfs_ops = {
	.show = pci_slot_attr_show,	/* NVMe: sysfs 읽기 시 pci_slot_attr_show 사용 */
	.store = pci_slot_attr_store,	/* NVMe: sysfs 쓰기 시 pci_slot_attr_store 사용 */
};

/*
 * address_read_file:
 *   slot의 sysfs 'address' 속성값을 생성한다.
 *   출력 형식은 dddd:bb 또는 dddd:bb:dd이며, NVMe 장치가 연결된
 *   PCIe slot의 domain/bus/device 주소를 식별하는 데 사용된다.
 */
static ssize_t address_read_file(struct pci_slot *slot, char *buf)
{
	if (slot->number == 0xff)	/* NVMe: placeholder slot(-1이 저장된 경우)인지 확인 */
		return sysfs_emit(buf, "%04x:%02x\n",
				  pci_domain_nr(slot->bus),	/* NVMe: slot이 속한 PCI domain 번호 출력 */
				  slot->bus->number);	/* NVMe: slot이 속한 bus 번호 출력 */

	/*
	 * Preserve legacy ABI expectations that hotplug drivers that manage
	 * multiple devices per slot emit 0 for the device number.
	 */
	if (slot->number == PCI_SLOT_ALL_DEVICES)	/* NVMe: PCIe 핫플러그용 bus-wide slot인지 확인(ARI 등 다중 function 포함) */
		return sysfs_emit(buf, "%04x:%02x:00\n",
				  pci_domain_nr(slot->bus),	/* NVMe: NVMe slot의 domain 번호 */
				  slot->bus->number);	/* NVMe: NVMe slot의 bus 번호, device는 00으로 고정(레거시 ABI) */

	return sysfs_emit(buf, "%04x:%02x:%02x\n",
			  pci_domain_nr(slot->bus),	/* NVMe: NVMe 장치가 연결된 PCI domain */
			  slot->bus->number,	/* NVMe: NVMe 장치가 연결된 bus 번호 */
			  slot->number);	/* NVMe: NVMe endpoint의 device 번호(slot 번호) */
}

/*
 * bus_speed_read:
 *   주어진 PCIe bus 속도(enum pci_bus_speed)를 문자열로 변환하여
 *   sysfs 버퍼에 기록한다.
 *   NVMe 장치의 성능 디버깅이나 링크 협상 상태 확인에 사용된다.
 */
static ssize_t bus_speed_read(enum pci_bus_speed speed, char *buf)
{
	return sysfs_emit(buf, "%s\n", pci_speed_string(speed));	/* NVMe: 속도 enum을 사람이 읽을 수 있는 문자열로 출력 */
}

/*
 * max_speed_read_file:
 *   slot이 지원하는 최대 PCIe 링크 속도(max_bus_speed)를 sysfs에 노출한다.
 *   NVMe SSD의 최대 성능을 제한하는 물리적 링크 속도를 확인할 수 있다.
 */
static ssize_t max_speed_read_file(struct pci_slot *slot, char *buf)
{
	return bus_speed_read(slot->bus->max_bus_speed, buf);	/* NVMe: slot bus의 최대 링크 속도 출력 */
}

/*
 * cur_speed_read_file:
 *   slot의 현재 PCIe 링크 속도(cur_bus_speed)를 sysfs에 노출한다.
 *   NVMe 장치가 현재 실제로 협상한 링크 속도를 확인할 때 사용된다.
 */
static ssize_t cur_speed_read_file(struct pci_slot *slot, char *buf)
{
	return bus_speed_read(slot->bus->cur_bus_speed, buf);	/* NVMe: slot bus의 현재 링크 속도 출력 */
}

/*
 * pci_slot_release:
 *   slot의 참조 카운트가 0이 되어 해제될 때 호출되는 release 콜백이다.
 *   NVMe 장치가 slot에 연결되어 있었다면 dev->slot을 NULL로 지우고,
 *   slot 구조체와 bus 참조를 정리한다.
 */
static void pci_slot_release(struct kobject *kobj)
{
	struct pci_dev *dev;	/* NVMe: slot에 속한 NVMe 등 PCI 장치를 순회할 포인터 */
	struct pci_slot *slot = to_pci_slot(kobj);	/* NVMe: kobject에서 pci_slot 구조체 복원 */

	dev_dbg(&slot->bus->dev, "dev %02x, released physical slot %s\n",
		slot->number, pci_slot_name(slot));	/* NVMe: slot 해제 디버그 메시지 출력 */

	down_read(&pci_bus_sem);	/* NVMe: bus 장치 리스트 읽기 잠금 획득 */
	list_for_each_entry(dev, &slot->bus->devices, bus_list)	/* NVMe: slot bus에 연결된 모든 PCI 장치 순회 */
		if (slot->number == PCI_SLOT_ALL_DEVICES ||	/* NVMe: bus-wide slot이면 모든 장치 대상 */
		    PCI_SLOT(dev->devfn) == slot->number)	/* NVMe: 동일 slot(device 번호)에 속한 장치만 선택 */
			dev->slot = NULL;	/* NVMe: NVMe 장치가 더 이상 이 slot을 가리키지 않도록 설정 */
	up_read(&pci_bus_sem);	/* NVMe: bus 장치 리스트 읽기 잠금 해제 */

	list_del(&slot->list);	/* NVMe: slot을 bus의 slot 리스트에서 제거 */
	pci_bus_put(slot->bus);	/* NVMe: slot이 참조하던 bus의 참조 카운트 감소 */

	kfree(slot);	/* NVMe: slot 구조체 메모리 해제 */
}

static struct pci_slot_attribute pci_slot_attr_address =
	__ATTR(address, S_IRUGO, address_read_file, NULL);	/* NVMe: 'address' 읽기 전용 속성 정의 */
static struct pci_slot_attribute pci_slot_attr_max_speed =
	__ATTR(max_bus_speed, S_IRUGO, max_speed_read_file, NULL);	/* NVMe: 'max_bus_speed' 읽기 전용 속성 정의 */
static struct pci_slot_attribute pci_slot_attr_cur_speed =
	__ATTR(cur_bus_speed, S_IRUGO, cur_speed_read_file, NULL);	/* NVMe: 'cur_bus_speed' 읽기 전용 속성 정의 */

static struct attribute *pci_slot_default_attrs[] = {
	&pci_slot_attr_address.attr,	/* NVMe: address 속성을 기본 그룹에 등록 */
	&pci_slot_attr_max_speed.attr,	/* NVMe: max_bus_speed 속성을 기본 그룹에 등록 */
	&pci_slot_attr_cur_speed.attr,	/* NVMe: cur_bus_speed 속성을 기본 그룹에 등록 */
	NULL,	/* NVMe: 속성 배열 종료 표시 */
};
ATTRIBUTE_GROUPS(pci_slot_default);	/* NVMe: pci_slot_default_groups 생성 */

static const struct kobj_type pci_slot_ktype = {
	.sysfs_ops = &pci_slot_sysfs_ops,	/* NVMe: slot sysfs read/write 콜백 등록 */
	.release = &pci_slot_release,	/* NVMe: slot 해제 시 pci_slot_release 호출 */
	.default_groups = pci_slot_default_groups,	/* NVMe: slot 생성 시 기본 속성 그룹 연결 */
};

/*
 * make_slot_name:
 *   사용자가 요청한 slot 이름을 기반으로 /sys/bus/pci/slots 아래에서
 *   유일한 이름을 생성한다. 이름 충돌 시 "name-N" 형식으로 rename한다.
 *   NVMe SSD가 연결된 slot의 sysfs 이름이 결정되는 지점이다.
 */
static char *make_slot_name(const char *name)
{
	char *new_name;		/* NVMe: 최종 생성될 slot 이름 문자열 */
	int len, max, dup;	/* NVMe: 문자열 길이, 최대 중복 허용치, 중복 카운터 */

	new_name = kstrdup(name, GFP_KERNEL);	/* NVMe: 입력 이름을 커널 메모리에 복사 */
	if (!new_name)	/* NVMe: 메모리 할당 실패 확인 */
		return NULL;	/* NVMe: 실패 시 NULL 반환 */

	/*
	 * Make sure we hit the realloc case the first time through the
	 * loop.  'len' will be strlen(name) + 3 at that point which is
	 * enough space for "name-X" and the trailing NUL.
	 */
	len = strlen(name) + 2;	/* NVMe: 첫 번째 realloc 직전 길이 설정 */
	max = 1;	/* NVMe: 한 자리 수 중복까지 허용하기 위한 임계값 */
	dup = 1;	/* NVMe: 이름 중복 시 붙일 번호 초기화 */

	for (;;) {	/* NVMe: 유일한 이름을 찾을 때까지 무한 루프 */
		struct kobject *dup_slot;	/* NVMe: 동일 이름의 기존 slot kobject */
		dup_slot = kset_find_obj(pci_slots_kset, new_name);	/* NVMe: slots kset에서 동일 이름 검색 */
		if (!dup_slot)	/* NVMe: 동일 이름이 없으면 유일한 이름 확보 */
			break;	/* NVMe: 루프 종료, new_name 반환 */
		kobject_put(dup_slot);	/* NVMe: 검색 과정에서 증가한 참조 카운트 감소 */
		if (dup == max) {	/* NVMe: 숫자 자리수가 부족해지면 메모리 재할당 */
			len++;	/* NVMe: 문자열 길이를 한 자리 늘림 */
			max *= 10;	/* NVMe: 다음 자리수 경계로 갱신(9->99->999...) */
			kfree(new_name);	/* NVMe: 기존 버퍼 해제 */
			new_name = kmalloc(len, GFP_KERNEL);	/* NVMe: 더 큰 버퍼 할당 */
			if (!new_name)	/* NVMe: 메모리 할당 실패 확인 */
				break;	/* NVMe: 루프 종료, NULL은 아래서 반환 */
		}
		sprintf(new_name, "%s-%d", name, dup++);	/* NVMe: "name-N" 형식으로 이름 생성 후 번호 증가 */
	}

	return new_name;	/* NVMe: 유일한 slot 이름 반환(실패 시 NULL) */
}

/*
 * rename_slot:
 *   기존 slot의 sysfs 이름을 변경한다.
 *   NVMe 장치가 연결된 slot의 이름이 핫플러그 드라이버에 의해
 *   업데이트될 때 사용될 수 있다.
 */
static int rename_slot(struct pci_slot *slot, const char *name)
{
	int result = 0;	/* NVMe: 이름 변경 결과(0은 성공) */
	char *slot_name;	/* NVMe: 새로 할당된 유일한 slot 이름 */

	if (strcmp(pci_slot_name(slot), name) == 0)	/* NVMe: 현재 이름과 요청 이름이 같으면 */
		return result;	/* NVMe: 변경 없이 성공 반환 */

	slot_name = make_slot_name(name);	/* NVMe: 충돌 없는 새 이름 생성 */
	if (!slot_name)	/* NVMe: 메모리 할당 실패 확인 */
		return -ENOMEM;	/* NVMe: 메모리 부족 오류 반환 */

	result = kobject_rename(&slot->kobj, slot_name);	/* NVMe: sysfs 상 slot 디렉터리 이름 변경 */
	kfree(slot_name);	/* NVMe: 임시 이름 버퍼 해제 */

	return result;	/* NVMe: rename 결과 반환 */
}

/*
 * pci_dev_assign_slot:
 *   NVMe 등 PCI 장치가 이미 생성된 slot에 연결되도록 dev->slot을 설정한다.
 *   NVMe probe 시 pci_dev 구조체가 초기화된 후 이 함수를 통해 해당 장치의
 *   slot 링크가 복원된다.
 */
void pci_dev_assign_slot(struct pci_dev *dev)
{
	struct pci_slot *slot;	/* NVMe: 탐색 중인 pci_slot 포인터 */

	mutex_lock(&pci_slot_mutex);	/* NVMe: slot 리스트 보호 뮤텍스 획득 */
	list_for_each_entry(slot, &dev->bus->slots, list)	/* NVMe: NVMe 장치가 속한 bus의 모든 slot 순회 */
		if (slot->number == PCI_SLOT_ALL_DEVICES ||	/* NVMe: bus-wide slot이면 모든 장치 매칭 */
		    PCI_SLOT(dev->devfn) == slot->number)	/* NVMe: 장치의 device 번호와 slot 번호가 일치하면 */
			dev->slot = slot;	/* NVMe: NVMe 장치가 이 slot을 가리키도록 설정 */
	mutex_unlock(&pci_slot_mutex);	/* NVMe: slot 뮤텍스 해제 */
}

/*
 * get_slot:
 *   parent bus와 slot 번호로 기존 pci_slot을 찾아 참조 카운트를 증가시킨다.
 *   pci_create_slot() 내에서 재사용할 slot을 검색할 때 사용된다.
 *   호출자는 pci_slot_mutex를 이미 보유하고 있어야 한다.
 */
static struct pci_slot *get_slot(struct pci_bus *parent, int slot_nr)
{
	struct pci_slot *slot;	/* NVMe: 탐색 중인 slot 포인터 */

	/* We already hold pci_slot_mutex */
	list_for_each_entry(slot, &parent->slots, list)	/* NVMe: parent bus의 slot 리스트 순회 */
		if (slot->number == slot_nr) {	/* NVMe: 동일 slot 번호를 찾으면 */
			kobject_get(&slot->kobj);	/* NVMe: slot kobject 참조 카운트 증가 */
			return slot;	/* NVMe: 기존 slot 반환 */
		}

	return NULL;	/* NVMe: 해당 slot이 없으면 NULL 반환 */
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
/*
 * pci_create_slot:
 *   PCI 물리 슬롯을 새로 생성하거나, 이미 존재하면 참조 카운트를 증가시킨다.
 *   NVMe 장치가 연결될 PCIe slot이 이 함수를 통해 sysfs에 등록되며,
 *   NVMe probe 전에 slot 리소스가 준비되어 있어야 한다.
 */
struct pci_slot *pci_create_slot(struct pci_bus *parent, int slot_nr,
				 const char *name,
				 struct hotplug_slot *hotplug)
{
	struct pci_dev *dev;	/* NVMe: slot에 매칭할 bus 상의 PCI 장치 */
	struct pci_slot *slot;	/* NVMe: 생성 또는 재사용될 pci_slot */
	int err = 0;	/* NVMe: 오류 코드 초기화 */
	char *slot_name = NULL;	/* NVMe: 할당된 sysfs slot 이름 */

	mutex_lock(&pci_slot_mutex);	/* NVMe: slot 생성/제거 보호를 위한 뮤텍스 획득 */

	if (slot_nr == -1)	/* NVMe: placeholder slot 요청인지 확인(pSeries 등) */
		goto placeholder;	/* NVMe: 기존 slot 검색 없이 새 placeholder 생성 */

	/*
	 * Hotplug drivers are allowed to rename an existing slot,
	 * but only if not already claimed.
	 */
	slot = get_slot(parent, slot_nr);	/* NVMe: 동일 bus+slot 조합의 기존 slot 검색 */
	if (slot) {	/* NVMe: 기존 slot이 존재하면 */
		if (hotplug) {	/* NVMe: 핫플러그 드라이버가 호출한 경우 */
			if (slot->hotplug) {	/* NVMe: 이미 다른 핫플러그 드라이버가 점유 중이면 */
				err = -EBUSY;	/* NVMe: busy 오류 설정 */
				goto put_slot;	/* NVMe: 참조 감소 후 오류 처리 */
			}
			err = rename_slot(slot, name);	/* NVMe: slot 이름을 새 핫플러그 이름으로 변경 */
			if (err)	/* NVMe: rename 실패 시 */
				goto put_slot;	/* NVMe: 참조 감소 후 오류 처리 */
		}
		goto out;	/* NVMe: 기존 slot 반환(참조 증가된 상태) */
	}

placeholder:
	slot = kzalloc_obj(*slot);	/* NVMe: pci_slot 구조체 0 초기화 후 할당 */
	if (!slot) {	/* NVMe: 메모리 할당 실패 확인 */
		err = -ENOMEM;	/* NVMe: 메모리 부족 오류 설정 */
		goto err;	/* NVMe: 오류 처리 경로로 이동 */
	}

	slot->bus = pci_bus_get(parent);	/* NVMe: parent bus 참조 카운트 증가 및 연결 */
	slot->number = slot_nr;	/* NVMe: slot 번호 저장(NVMe device 번호 또는 ALL_DEVICES) */

	slot->kobj.kset = pci_slots_kset;	/* NVMe: slot kobject가 속할 kset 지정(/sys/bus/pci/slots) */

	slot_name = make_slot_name(name);	/* NVMe: sysfs에 노출할 유일한 slot 이름 생성 */
	if (!slot_name) {	/* NVMe: 이름 생성 실패(메모리 부족) 확인 */
		err = -ENOMEM;	/* NVMe: 메모리 부족 오류 설정 */
		pci_bus_put(slot->bus);	/* NVMe: 이전에 증가시킨 bus 참조 감소 */
		kfree(slot);	/* NVMe: 할당한 slot 구조체 해제 */
		goto err;	/* NVMe: 오류 처리 경로로 이동 */
	}

	INIT_LIST_HEAD(&slot->list);	/* NVMe: slot 리스트 노드 초기화 */
	list_add(&slot->list, &parent->slots);	/* NVMe: parent bus의 slot 리스트에 추가 */

	err = kobject_init_and_add(&slot->kobj, &pci_slot_ktype, NULL,
				   "%s", slot_name);	/* NVMe: slot kobject 초기화 및 /sys/bus/pci/slots/<name> 등록 */
	if (err)	/* NVMe: kobject 등록 실패 확인 */
		goto put_slot;	/* NVMe: slot 참조를 감소시켜 해제 유도 */

	down_read(&pci_bus_sem);	/* NVMe: bus 장치 리스트 읽기 잠금 획득 */
	list_for_each_entry(dev, &parent->devices, bus_list)	/* NVMe: parent bus의 모든 PCI 장치 순회 */
		if (slot_nr == PCI_SLOT_ALL_DEVICES ||	/* NVMe: bus-wide slot이면 모든 장치 연결 */
		    PCI_SLOT(dev->devfn) == slot_nr)	/* NVMe: 동일 device 번호의 장치만 선택 */
			dev->slot = slot;	/* NVMe: NVMe 등 장치가 이 slot을 가리키도록 연결 */
	up_read(&pci_bus_sem);	/* NVMe: bus 장치 리스트 읽기 잠금 해제 */

	dev_dbg(&parent->dev, "dev %02x, created physical slot %s\n",
		slot_nr, pci_slot_name(slot));	/* NVMe: slot 생성 완료 디버그 로그 */

out:
	kfree(slot_name);	/* NVMe: 임시 slot 이름 버퍼 해제(성공/실패 모두) */
	mutex_unlock(&pci_slot_mutex);	/* NVMe: slot 뮤텍스 해제 */
	return slot;	/* NVMe: 생성되거나 참조 증가된 slot 반환 */

put_slot:
	kobject_put(&slot->kobj);	/* NVMe: kobject 참조 감소 -> release 또는 카운트 감소 */
err:
	slot = ERR_PTR(err);	/* NVMe: 오류 포인터로 변환 */
	goto out;	/* NVMe: 공통 반환 경로로 이동 */
}
EXPORT_SYMBOL_GPL(pci_create_slot);	/* NVMe: 핫플러그/EDR 등 외부 모듈에서 pci_create_slot 호출 가능 */

/**
 * pci_destroy_slot - decrement refcount for physical PCI slot
 * @slot: struct pci_slot to decrement
 *
 * %struct pci_slot is refcounted, so destroying them is really easy; we
 * just call kobject_put on its kobj and let our release methods do the
 * rest.
 */
/*
 * pci_destroy_slot:
 *   slot의 참조 카운트를 감소시킨다. 마지막 참조라면 pci_slot_release를
 *   통해 메모리가 해제된다. NVMe 장치 제거(hot-unplug, EDR, 사용자 공간
 *   remove) 시 연결된 slot이 정리되는 경로이다.
 */
void pci_destroy_slot(struct pci_slot *slot)
{
	dev_dbg(&slot->bus->dev, "dev %02x, dec refcount to %d\n",
		slot->number, kref_read(&slot->kobj.kref) - 1);	/* NVMe: 참조 카운트 감소 전 디버그 로그 */

	mutex_lock(&pci_slot_mutex);	/* NVMe: slot 제거 보호 뮤텍스 획득 */
	kobject_put(&slot->kobj);	/* NVMe: slot kobject 참조 감소(마지막이면 release 호출) */
	mutex_unlock(&pci_slot_mutex);	/* NVMe: slot 뮤텍스 해제 */
}
EXPORT_SYMBOL_GPL(pci_destroy_slot);	/* NVMe: 핫플러그/EDR 등 외부 모듈에서 pci_destroy_slot 호출 가능 */

/*
 * pci_slot_init:
 *   PCI slot 서브시스템 초기화 함수. 부팅 시 subsys_initcall로 호출되어
 *   /sys/bus/pci/slots 디렉터리를 생성한다. 이후 NVMe 장치의 slot이
 *   이 kset 아래에 등록된다.
 */
static int pci_slot_init(void)
{
	struct kset *pci_bus_kset;	/* NVMe: /sys/bus/pci의 kset */

	pci_bus_kset = bus_get_kset(&pci_bus_type);	/* NVMe: pci_bus_type의 kset 획득 */
	pci_slots_kset = kset_create_and_add("slots", NULL,
					    &pci_bus_kset->kobj);	/* NVMe: /sys/bus/pci/slots kset 생성 및 등록 */
	if (!pci_slots_kset) {	/* NVMe: kset 생성 실패 확인 */
		pr_err("PCI: Slot initialization failure\n");	/* NVMe: 초기화 실패 메시지 출력 */
		return -ENOMEM;	/* NVMe: 메모리 부족 오류 반환 */
	}
	return 0;	/* NVMe: slot 서브시스템 초기화 성공 */
}

subsys_initcall(pci_slot_init);	/* NVMe: 커널 서브시스템 초기화 단계에서 pci_slot_init 실행 */
