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
 * pci_hp_create_module_link() / _remove_module_link() : sysfs 에서 슬롯과
 *                         그것을 관리하는 모듈을 잇는 심볼릭 링크.
 */

#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/err.h>
#include "pci.h"

struct kset *pci_slots_kset;
EXPORT_SYMBOL_GPL(pci_slots_kset);

/*
 * pci_slot_attr_show:
 *   slot의 sysfs 속성(attribute) 읽기 콜백이다.
 *   NVMe 관점에서 /sys/bus/pci/slots/<slot>/address, max_bus_speed,
 *   cur_bus_speed 등을 읽을 때 이 함수가 호출된다.
 */
static ssize_t pci_slot_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct pci_slot *slot = to_pci_slot(kobj);
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);
	return attribute->show ? attribute->show(slot, buf) : -EIO;
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
	struct pci_slot *slot = to_pci_slot(kobj);
	struct pci_slot_attribute *attribute = to_pci_slot_attr(attr);
	return attribute->store ? attribute->store(slot, buf, len) : -EIO;
}

static const struct sysfs_ops pci_slot_sysfs_ops = {
	.show = pci_slot_attr_show,
	.store = pci_slot_attr_store,
};

/*
 * address_read_file:
 *   slot의 sysfs 'address' 속성값을 생성한다.
 *   출력 형식은 dddd:bb 또는 dddd:bb:dd이며, NVMe 장치가 연결된
 *   PCIe slot의 domain/bus/device 주소를 식별하는 데 사용된다.
 */
static ssize_t address_read_file(struct pci_slot *slot, char *buf)
{
	if (slot->number == 0xff)
		return sysfs_emit(buf, "%04x:%02x\n",
				  pci_domain_nr(slot->bus),
				  slot->bus->number);

	/*
	 * Preserve legacy ABI expectations that hotplug drivers that manage
	 * multiple devices per slot emit 0 for the device number.
	 */
	if (slot->number == PCI_SLOT_ALL_DEVICES)
		return sysfs_emit(buf, "%04x:%02x:00\n",
				  pci_domain_nr(slot->bus),
				  slot->bus->number);

	return sysfs_emit(buf, "%04x:%02x:%02x\n",
			  pci_domain_nr(slot->bus),
			  slot->bus->number,
			  slot->number);
}

/*
 * bus_speed_read:
 *   주어진 PCIe bus 속도(enum pci_bus_speed)를 문자열로 변환하여
 *   sysfs 버퍼에 기록한다.
 *   NVMe 장치의 성능 디버깅이나 링크 협상 상태 확인에 사용된다.
 */
static ssize_t bus_speed_read(enum pci_bus_speed speed, char *buf)
{
	return sysfs_emit(buf, "%s\n", pci_speed_string(speed));
}

/*
 * max_speed_read_file:
 *   slot이 지원하는 최대 PCIe 링크 속도(max_bus_speed)를 sysfs에 노출한다.
 *   NVMe SSD의 최대 성능을 제한하는 물리적 링크 속도를 확인할 수 있다.
 */
static ssize_t max_speed_read_file(struct pci_slot *slot, char *buf)
{
	return bus_speed_read(slot->bus->max_bus_speed, buf);
}

/*
 * cur_speed_read_file:
 *   slot의 현재 PCIe 링크 속도(cur_bus_speed)를 sysfs에 노출한다.
 *   NVMe 장치가 현재 실제로 협상한 링크 속도를 확인할 때 사용된다.
 */
static ssize_t cur_speed_read_file(struct pci_slot *slot, char *buf)
{
	return bus_speed_read(slot->bus->cur_bus_speed, buf);
}

/*
 * pci_slot_release:
 *   slot의 참조 카운트가 0이 되어 해제될 때 호출되는 release 콜백이다.
 *   NVMe 장치가 slot에 연결되어 있었다면 dev->slot을 NULL로 지우고,
 *   slot 구조체와 bus 참조를 정리한다.
 */
static void pci_slot_release(struct kobject *kobj)
{
	struct pci_dev *dev;
	struct pci_slot *slot = to_pci_slot(kobj);

	dev_dbg(&slot->bus->dev, "dev %02x, released physical slot %s\n",
		slot->number, pci_slot_name(slot));

	down_read(&pci_bus_sem);
	list_for_each_entry(dev, &slot->bus->devices, bus_list)
		if (slot->number == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot->number)
			dev->slot = NULL;
	up_read(&pci_bus_sem);

	list_del(&slot->list);
	pci_bus_put(slot->bus);

	kfree(slot);
}

static struct pci_slot_attribute pci_slot_attr_address =
	__ATTR(address, S_IRUGO, address_read_file, NULL);
static struct pci_slot_attribute pci_slot_attr_max_speed =
	__ATTR(max_bus_speed, S_IRUGO, max_speed_read_file, NULL);
static struct pci_slot_attribute pci_slot_attr_cur_speed =
	__ATTR(cur_bus_speed, S_IRUGO, cur_speed_read_file, NULL);

static struct attribute *pci_slot_default_attrs[] = {
	&pci_slot_attr_address.attr,
	&pci_slot_attr_max_speed.attr,
	&pci_slot_attr_cur_speed.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pci_slot_default);

static const struct kobj_type pci_slot_ktype = {
	.sysfs_ops = &pci_slot_sysfs_ops,
	.release = &pci_slot_release,
	.default_groups = pci_slot_default_groups,
};

/*
 * make_slot_name:
 *   사용자가 요청한 slot 이름을 기반으로 /sys/bus/pci/slots 아래에서
 *   유일한 이름을 생성한다. 이름 충돌 시 "name-N" 형식으로 rename한다.
 *   NVMe SSD가 연결된 slot의 sysfs 이름이 결정되는 지점이다.
 */
static char *make_slot_name(const char *name)
{
	char *new_name;
	int len, max, dup;

	new_name = kstrdup(name, GFP_KERNEL);
	if (!new_name)
		return NULL;

	/*
	 * Make sure we hit the realloc case the first time through the
	 * loop.  'len' will be strlen(name) + 3 at that point which is
	 * enough space for "name-X" and the trailing NUL.
	 */
	len = strlen(name) + 2;
	max = 1;
	dup = 1;

	for (;;) {
		struct kobject *dup_slot;
		dup_slot = kset_find_obj(pci_slots_kset, new_name);
		if (!dup_slot)
			break;
		kobject_put(dup_slot);
		if (dup == max) {
			len++;
			max *= 10;
			kfree(new_name);
			new_name = kmalloc(len, GFP_KERNEL);
			if (!new_name)
				break;
		}
		sprintf(new_name, "%s-%d", name, dup++);
	}

	return new_name;
}

/*
 * rename_slot:
 *   기존 slot의 sysfs 이름을 변경한다.
 *   NVMe 장치가 연결된 slot의 이름이 핫플러그 드라이버에 의해
 *   업데이트될 때 사용될 수 있다.
 */
static int rename_slot(struct pci_slot *slot, const char *name)
{
	int result = 0;
	char *slot_name;

	if (strcmp(pci_slot_name(slot), name) == 0)
		return result;

	slot_name = make_slot_name(name);
	if (!slot_name)
		return -ENOMEM;

	result = kobject_rename(&slot->kobj, slot_name);
	kfree(slot_name);

	return result;
}

/*
 * pci_dev_assign_slot:
 *   NVMe 등 PCI 장치가 이미 생성된 slot에 연결되도록 dev->slot을 설정한다.
 *   NVMe probe 시 pci_dev 구조체가 초기화된 후 이 함수를 통해 해당 장치의
 *   slot 링크가 복원된다.
 */
void pci_dev_assign_slot(struct pci_dev *dev)
{
	struct pci_slot *slot;

	mutex_lock(&pci_slot_mutex);
	list_for_each_entry(slot, &dev->bus->slots, list)
		if (slot->number == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot->number)
			dev->slot = slot;
	mutex_unlock(&pci_slot_mutex);
}

/*
 * get_slot:
 *   parent bus와 slot 번호로 기존 pci_slot을 찾아 참조 카운트를 증가시킨다.
 *   pci_create_slot() 내에서 재사용할 slot을 검색할 때 사용된다.
 *   호출자는 pci_slot_mutex를 이미 보유하고 있어야 한다.
 */
static struct pci_slot *get_slot(struct pci_bus *parent, int slot_nr)
{
	struct pci_slot *slot;

	/* We already hold pci_slot_mutex */
	list_for_each_entry(slot, &parent->slots, list)
		if (slot->number == slot_nr) {
			kobject_get(&slot->kobj);
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
	struct pci_dev *dev;
	struct pci_slot *slot;
	int err = 0;
	char *slot_name = NULL;

	mutex_lock(&pci_slot_mutex);

	if (slot_nr == -1)
		goto placeholder;

	/*
	 * Hotplug drivers are allowed to rename an existing slot,
	 * but only if not already claimed.
	 */
	slot = get_slot(parent, slot_nr);
	if (slot) {
		if (hotplug) {
			if (slot->hotplug) {
				err = -EBUSY;
				goto put_slot;
			}
			err = rename_slot(slot, name);
			if (err)
				goto put_slot;
		}
		goto out;
	}

placeholder:
	slot = kzalloc_obj(*slot);
	if (!slot) {
		err = -ENOMEM;
		goto err;
	}

	slot->bus = pci_bus_get(parent);
	slot->number = slot_nr;

	slot->kobj.kset = pci_slots_kset;

	slot_name = make_slot_name(name);
	if (!slot_name) {
		err = -ENOMEM;
		pci_bus_put(slot->bus);
		kfree(slot);
		goto err;
	}

	INIT_LIST_HEAD(&slot->list);
	list_add(&slot->list, &parent->slots);

	err = kobject_init_and_add(&slot->kobj, &pci_slot_ktype, NULL,
				   "%s", slot_name);
	if (err)
		goto put_slot;

	down_read(&pci_bus_sem);
	list_for_each_entry(dev, &parent->devices, bus_list)
		if (slot_nr == PCI_SLOT_ALL_DEVICES ||
		    PCI_SLOT(dev->devfn) == slot_nr)
			dev->slot = slot;
	up_read(&pci_bus_sem);

	dev_dbg(&parent->dev, "dev %02x, created physical slot %s\n",
		slot_nr, pci_slot_name(slot));

out:
	kfree(slot_name);
	mutex_unlock(&pci_slot_mutex);
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
/*
 * pci_destroy_slot:
 *   slot의 참조 카운트를 감소시킨다. 마지막 참조라면 pci_slot_release를
 *   통해 메모리가 해제된다. NVMe 장치 제거(hot-unplug, EDR, 사용자 공간
 *   remove) 시 연결된 slot이 정리되는 경로이다.
 */
void pci_destroy_slot(struct pci_slot *slot)
{
	dev_dbg(&slot->bus->dev, "dev %02x, dec refcount to %d\n",
		slot->number, kref_read(&slot->kobj.kref) - 1);

	mutex_lock(&pci_slot_mutex);
	kobject_put(&slot->kobj);
	mutex_unlock(&pci_slot_mutex);
}
EXPORT_SYMBOL_GPL(pci_destroy_slot);

/*
 * pci_slot_init:
 *   PCI slot 서브시스템 초기화 함수. 부팅 시 subsys_initcall로 호출되어
 *   /sys/bus/pci/slots 디렉터리를 생성한다. 이후 NVMe 장치의 slot이
 *   이 kset 아래에 등록된다.
 */
static int pci_slot_init(void)
{
	struct kset *pci_bus_kset;

	pci_bus_kset = bus_get_kset(&pci_bus_type);
	pci_slots_kset = kset_create_and_add("slots", NULL,
					    &pci_bus_kset->kobj);
	if (!pci_slots_kset) {
		pr_err("PCI: Slot initialization failure\n");
		return -ENOMEM;
	}
	return 0;
}

subsys_initcall(pci_slot_init);
