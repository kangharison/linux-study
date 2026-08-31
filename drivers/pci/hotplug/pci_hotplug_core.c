// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI HotPlug Controller Core
 *
 * Copyright (C) 2001-2002 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001-2002 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 * Authors:
 *   Greg Kroah-Hartman <greg@kroah.com>
 *   Scott Murray <scottm@somanetworks.com>
 */

/*
 * [한국어 설명] 여러 핫플러그 방식이 공유하는 sysfs 계층 (pci_hotplug_core.c)
 *
 * === 파일의 역할 ===
 * PCI 핫플러그에는 방식이 여럿이다. PCIe 네이티브(pciehp), ACPI 기반
 * (acpiphp), 그리고 벤더 전용 컨트롤러들. 이 파일은 그것들이 공통으로
 * 쓰는 껍데기를 제공한다 — 슬롯을 /sys/bus/pci/slots/ 에 노출하고,
 * 사용자가 그 파일을 읽고 쓰면 해당 드라이버의 콜백으로 넘긴다.
 *
 * 핵심은 struct hotplug_slot_ops 다. 각 드라이버가 자기 하드웨어에 맞게
 * 이 함수 표를 채워 등록하면, 이 파일은 sysfs 접근을 그 표로 전달하기만
 * 한다. 공통 부분과 하드웨어 부분을 갈라 놓은 것이다.
 *
 * sysfs 에 나오는 항목이 다섯이다.
 *   power         — 슬롯 전원. 여기에 0 이나 1 을 쓰면 실제로 켜지고 꺼진다.
 *   attention     — Attention LED. 어느 드라이브가 문제인지 표시할 때 쓴다.
 *   latch         — 물리적 걸쇠가 열렸는지.
 *   adapter       — 카드가 꽂혀 있는지.
 *   test          — 드라이버별 시험용.
 * 모든 드라이버가 다섯을 다 지원하지는 않아서, ops 에 함수가 없으면
 * 그 파일 자체를 만들지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp / acpiphp / shpchp / 벤더 드라이버
 *   -> pci_hp_register() 또는 pci_hp_initialize() + pci_hp_add()
 *      -> [이 파일] 슬롯을 등록하고 sysfs 항목 생성
 *
 * 사용자가 /sys/bus/pci/slots/N/power 에 쓰기
 *   -> [이 파일] power_write_file()
 *      -> slot->ops->enable_slot() 또는 disable_slot()
 *         -> 예컨대 pciehp_sysfs_enable_slot() [pciehp_ctrl.c]
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. sysfs 쓰기는 사용자 프로세스에서
 * 시작되고, 그 안에서 열거·제거까지 동기적으로 진행될 수 있어 오래 걸린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 각 핫플러그 드라이버(hotplug/ 아래 전부).
 * 아래쪽: slot.c 의 struct pci_slot(슬롯의 PCI 코어 쪽 표현),
 *   그리고 커널 sysfs 계층.
 * 공유 상태: struct hotplug_slot 과 그것이 가리키는 struct pci_slot.
 *   등록·해제 시 두 구조체의 수명을 맞추는 것이 이 파일의 까다로운 부분이다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 운영 실무에서 NVMe 와 자주 엮인다. 서버에서 고장 난 드라이브를
 * 뽑기 전에 attention LED 를 켜서 어느 베이인지 표시하고, power 에 0 을
 * 써서 안전하게 내리는 절차가 전부 이 파일이 만든 sysfs 를 거친다.
 *   echo 1 > /sys/bus/pci/slots/3/attention
 *   echo 0 > /sys/bus/pci/slots/3/power
 * 두 번째 명령이 nvme_remove() 까지 이어진다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_hp_register()      : 슬롯을 등록한다. initialize + add 를 한 번에.
 * pci_hp_initialize()    : 등록 준비만 한다. 인터럽트를 켜기 전에 자료구조를
 *                          갖춰 두어야 하는 드라이버가 나눠 쓴다.
 * pci_hp_add()           : 실제로 sysfs 에 노출한다.
 * pci_hp_deregister() / pci_hp_del() / pci_hp_destroy() : 그 반대들.
 * power_write_file()     : 사용자의 전원 조작을 드라이버 콜백으로 넘긴다.
 * attention_write_file() : Attention LED 조작. 표시등 소유권을 확인한다.
 * has_power_file() 계열  : ops 에 해당 콜백이 있을 때만 sysfs 항목을 만든다.
 * hotplug_slot_attrs     : 위 항목들을 묶은 sysfs 속성 그룹.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include "../pci.h"
#include "cpci_hotplug.h"

#define MY_NAME	"pci_hotplug"

#define dbg(fmt, arg...) do { if (debug) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)

/* local variables */
static bool debug;

/* Weee, fun with macros... */
#define GET_STATUS(name, type)	\
static int get_##name(struct hotplug_slot *slot, type *value)		\
{									\
	const struct hotplug_slot_ops *ops = slot->ops;			\
	int retval = 0;							\
	if (ops->get_##name)						\
		retval = ops->get_##name(slot, value);			\
	return retval;							\
}

GET_STATUS(power_status, u8)
GET_STATUS(attention_status, u8)
GET_STATUS(latch_status, u8)
GET_STATUS(adapter_status, u8)

static ssize_t power_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_power_status(pci_slot->hotplug, &value);
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t power_write_file(struct pci_slot *pci_slot, const char *buf,
				size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	unsigned long lpower;
	u8 power;
	int retval = 0;

	lpower = simple_strtoul(buf, NULL, 10);
	power = (u8)(lpower & 0xff);
	dbg("power = %d\n", power);

	switch (power) {
	case 0:
		if (slot->ops->disable_slot)
			retval = slot->ops->disable_slot(slot);
		break;

	case 1:
		if (slot->ops->enable_slot)
			retval = slot->ops->enable_slot(slot);
		break;

	default:
		err("Illegal value specified for power\n");
		retval = -EINVAL;
	}

	if (retval)
		return retval;
	return count;
}

static struct pci_slot_attribute hotplug_slot_attr_power = {
	.attr = {.name = "power", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	.show = power_read_file,
	.store = power_write_file
};

static ssize_t attention_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_attention_status(pci_slot->hotplug, &value);
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t attention_write_file(struct pci_slot *pci_slot, const char *buf,
				    size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	const struct hotplug_slot_ops *ops = slot->ops;
	unsigned long lattention;
	u8 attention;
	int retval = 0;

	lattention = simple_strtoul(buf, NULL, 10);
	attention = (u8)(lattention & 0xff);
	dbg(" - attention = %d\n", attention);

	if (ops->set_attention_status)
		retval = ops->set_attention_status(slot, attention);

	if (retval)
		return retval;
	return count;
}

static struct pci_slot_attribute hotplug_slot_attr_attention = {
	.attr = {.name = "attention", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	.show = attention_read_file,
	.store = attention_write_file
};

static ssize_t latch_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_latch_status(pci_slot->hotplug, &value);
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

static struct pci_slot_attribute hotplug_slot_attr_latch = {
	.attr = {.name = "latch", .mode = S_IFREG | S_IRUGO},
	.show = latch_read_file,
};

static ssize_t presence_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_adapter_status(pci_slot->hotplug, &value);
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

static struct pci_slot_attribute hotplug_slot_attr_presence = {
	.attr = {.name = "adapter", .mode = S_IFREG | S_IRUGO},
	.show = presence_read_file,
};

static ssize_t test_write_file(struct pci_slot *pci_slot, const char *buf,
			       size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	unsigned long ltest;
	u32 test;
	int retval = 0;

	ltest = simple_strtoul(buf, NULL, 10);
	test = (u32)(ltest & 0xffffffff);
	dbg("test = %d\n", test);

	if (slot->ops->hardware_test)
		retval = slot->ops->hardware_test(slot, test);

	if (retval)
		return retval;
	return count;
}

static struct pci_slot_attribute hotplug_slot_attr_test = {
	.attr = {.name = "test", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	.store = test_write_file
};

static bool has_power_file(struct hotplug_slot *slot)
{
	if ((slot->ops->enable_slot) ||
	    (slot->ops->disable_slot) ||
	    (slot->ops->get_power_status))
		return true;
	return false;
}

static bool has_attention_file(struct hotplug_slot *slot)
{
	if ((slot->ops->set_attention_status) ||
	    (slot->ops->get_attention_status))
		return true;
	return false;
}

static bool has_latch_file(struct hotplug_slot *slot)
{
	if (slot->ops->get_latch_status)
		return true;
	return false;
}

static bool has_adapter_file(struct hotplug_slot *slot)
{
	if (slot->ops->get_adapter_status)
		return true;
	return false;
}

static bool has_test_file(struct hotplug_slot *slot)
{
	if (slot->ops->hardware_test)
		return true;
	return false;
}

static int fs_add_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)
{
	struct kobject *kobj;
	int retval = 0;

	/* Create symbolic link to the hotplug driver module */
	kobj = kset_find_obj(module_kset, slot->mod_name);
	if (kobj) {
		retval = sysfs_create_link(&pci_slot->kobj, kobj, "module");
		if (retval)
			dev_err(&pci_slot->bus->dev,
				"Error creating sysfs link (%d)\n", retval);
		kobject_put(kobj);
	}

	if (has_power_file(slot)) {
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_power.attr);
		if (retval)
			goto exit_power;
	}

	if (has_attention_file(slot)) {
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_attention.attr);
		if (retval)
			goto exit_attention;
	}

	if (has_latch_file(slot)) {
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_latch.attr);
		if (retval)
			goto exit_latch;
	}

	if (has_adapter_file(slot)) {
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_presence.attr);
		if (retval)
			goto exit_adapter;
	}

	if (has_test_file(slot)) {
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_test.attr);
		if (retval)
			goto exit_test;
	}

	goto exit;

exit_test:
	if (has_adapter_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);
exit_adapter:
	if (has_latch_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);
exit_latch:
	if (has_attention_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);
exit_attention:
	if (has_power_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);
exit_power:
	sysfs_remove_link(&pci_slot->kobj, "module");
exit:
	return retval;
}

static void fs_remove_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)
{
	if (has_power_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);

	if (has_attention_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);

	if (has_latch_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);

	if (has_adapter_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);

	if (has_test_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_test.attr);

	sysfs_remove_link(&pci_slot->kobj, "module");
}

/**
 * __pci_hp_register - register a hotplug_slot with the PCI hotplug subsystem
 * @slot: pointer to the &struct hotplug_slot to register
 * @bus: bus this slot is on
 * @devnr: device number
 * @name: name registered with kobject core
 * @owner: caller module owner
 * @mod_name: caller module name
 *
 * Prepares a hotplug slot for in-kernel use and immediately publishes it to
 * user space in one go.  Drivers may alternatively carry out the two steps
 * separately by invoking pci_hp_initialize() and pci_hp_add().
 *
 * Returns 0 if successful, anything else for an error.
 */
int __pci_hp_register(struct hotplug_slot *slot, struct pci_bus *bus,
		      int devnr, const char *name,
		      struct module *owner, const char *mod_name)
{
	int result;

	result = __pci_hp_initialize(slot, bus, devnr, name, owner, mod_name);
	if (result)
		return result;

	result = pci_hp_add(slot);
	if (result)
		pci_hp_destroy(slot);

	return result;
}
EXPORT_SYMBOL_GPL(__pci_hp_register);

/**
 * __pci_hp_initialize - prepare hotplug slot for in-kernel use
 * @slot: pointer to the &struct hotplug_slot to initialize
 * @bus: bus this slot is on
 * @devnr: slot number
 * @name: name registered with kobject core
 * @owner: caller module owner
 * @mod_name: caller module name
 *
 * Allocate and fill in a PCI slot for use by a hotplug driver.  Once this has
 * been called, the driver may invoke hotplug_slot_name() to get the slot's
 * unique name.  The driver must be prepared to handle a ->reset_slot callback
 * from this point on.
 *
 * Returns 0 on success or a negative int on error.
 */
int __pci_hp_initialize(struct hotplug_slot *slot, struct pci_bus *bus,
			int devnr, const char *name, struct module *owner,
			const char *mod_name)
{
	struct pci_slot *pci_slot;

	if (slot == NULL)
		return -ENODEV;
	if (slot->ops == NULL)
		return -EINVAL;

	slot->owner = owner;
	slot->mod_name = mod_name;

	/*
	 * No problems if we call this interface from both ACPI_PCI_SLOT
	 * driver and call it here again. If we've already created the
	 * pci_slot, the interface will simply bump the refcount.
	 */
	pci_slot = pci_create_slot(bus, devnr, name, slot);
	if (IS_ERR(pci_slot))
		return PTR_ERR(pci_slot);

	slot->pci_slot = pci_slot;
	pci_slot->hotplug = slot;
	return 0;
}
EXPORT_SYMBOL_GPL(__pci_hp_initialize);

/**
 * pci_hp_add - publish hotplug slot to user space
 * @slot: pointer to the &struct hotplug_slot to publish
 *
 * Make a hotplug slot's sysfs interface available and inform user space of its
 * addition by sending a uevent.  The hotplug driver must be prepared to handle
 * all &struct hotplug_slot_ops callbacks from this point on.
 *
 * Returns 0 on success or a negative int on error.
 */
int pci_hp_add(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot;
	int result;

	if (WARN_ON(!slot))
		return -EINVAL;

	pci_slot = slot->pci_slot;

	result = fs_add_slot(slot, pci_slot);
	if (result)
		return result;

	kobject_uevent(&pci_slot->kobj, KOBJ_ADD);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_hp_add);

/**
 * pci_hp_deregister - deregister a hotplug_slot with the PCI hotplug subsystem
 * @slot: pointer to the &struct hotplug_slot to deregister
 *
 * The @slot must have been registered with the pci hotplug subsystem
 * previously with a call to pci_hp_register().
 */
void pci_hp_deregister(struct hotplug_slot *slot)
{
	pci_hp_del(slot);
	pci_hp_destroy(slot);
}
EXPORT_SYMBOL_GPL(pci_hp_deregister);

/**
 * pci_hp_del - unpublish hotplug slot from user space
 * @slot: pointer to the &struct hotplug_slot to unpublish
 *
 * Remove a hotplug slot's sysfs interface.
 */
void pci_hp_del(struct hotplug_slot *slot)
{
	if (WARN_ON(!slot))
		return;

	fs_remove_slot(slot, slot->pci_slot);
}
EXPORT_SYMBOL_GPL(pci_hp_del);

/**
 * pci_hp_destroy - remove hotplug slot from in-kernel use
 * @slot: pointer to the &struct hotplug_slot to destroy
 *
 * Destroy a PCI slot used by a hotplug driver.  Once this has been called,
 * the driver may no longer invoke hotplug_slot_name() to get the slot's
 * unique name.  The driver no longer needs to handle a ->reset_slot callback
 * from this point on.
 */
void pci_hp_destroy(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot = slot->pci_slot;

	slot->pci_slot = NULL;
	pci_slot->hotplug = NULL;
	pci_destroy_slot(pci_slot);
}
EXPORT_SYMBOL_GPL(pci_hp_destroy);

static DECLARE_WAIT_QUEUE_HEAD(pci_hp_link_change_wq);

/**
 * pci_hp_ignore_link_change - begin code section causing spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Mark the beginning of a code section causing spurious link changes on the
 * Secondary Bus of @pdev, e.g. as a side effect of a Secondary Bus Reset,
 * D3cold transition, firmware update or FPGA reconfiguration.
 *
 * Hotplug drivers can thus check whether such a code section is executing
 * concurrently, await it with pci_hp_spurious_link_change() and ignore the
 * resulting link change events.
 *
 * Must be paired with pci_hp_unignore_link_change().  May be called both
 * from the PCI core and from Endpoint drivers.  May be called for bridges
 * which are not hotplug-capable, in which case it has no effect because
 * no hotplug driver is bound to the bridge.
 */
void pci_hp_ignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGING, &pdev->priv_flags);
	smp_mb__after_atomic(); /* pairs with implied barrier of wait_event() */
	/* [한국어] set_bit 자체는 원자적이지만 순서까지 보장하지는 않는다.
	 * 다른 CPU 에서 wait_event() 로 이 비트를 기다리는 쪽이 있는데, 그쪽이
	 * 비트 설정을 보기 전에 이후 코드의 효과를 먼저 보면 안 되므로 배리어를 둔다.
	 * wait_event() 안에 이미 대응하는 배리어가 있어서 짝이 맞는다.
	 * smp_mb__after_atomic() 을 쓰는 이유는 아키텍처에 따라 set_bit 이
	 * 이미 배리어를 포함하기도 해서, 그럴 때는 아무 명령도 내지 않기 위해서다. */
}

/**
 * pci_hp_unignore_link_change - end code section causing spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Mark the end of a code section causing spurious link changes on the
 * Secondary Bus of @pdev.  Must be paired with pci_hp_ignore_link_change().
 */
void pci_hp_unignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGED, &pdev->priv_flags);
	mb(); /* ensure pci_hp_spurious_link_change() sees either bit set */
	/* [한국어] 여기서 완전한 배리어가 필요한 이유가 분명하다. 바로 위에서
	 * CHANGED 를 세우고 바로 아래에서 CHANGING 을 지우는데, 그 둘이 뒤집혀
	 * 보이면 관측자가 두 비트 모두 꺼진 순간을 보게 된다.
	 * pci_hp_spurious_link_change() 는 "둘 중 하나는 켜져 있다" 를 전제로
	 * 판단하므로, 그 틈이 생기면 진짜 링크 변화로 오인해 불필요한 재열거가 일어난다.
	 * smp_mb 가 아니라 mb 인 것은 이 순서가 CPU 사이뿐 아니라 장치 접근과도
	 * 관계되기 때문이다. */
	clear_bit(PCI_LINK_CHANGING, &pdev->priv_flags);
	wake_up_all(&pci_hp_link_change_wq);
}

/**
 * pci_hp_spurious_link_change - check for spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Check whether a code section is executing concurrently which is causing
 * spurious link changes on the Secondary Bus of @pdev.  Await the end of the
 * code section if so.
 *
 * May be called by hotplug drivers to check whether a link change is spurious
 * and can be ignored.
 *
 * Because a genuine link change may have occurred in-between a spurious link
 * change and the invocation of this function, hotplug drivers should perform
 * sanity checks such as retrieving the current link state and bringing down
 * the slot if the link is down.
 *
 * Return: %true if such a code section has been executing concurrently,
 * otherwise %false.  Also return %true if such a code section has not been
 * executing concurrently, but at least once since the last invocation of this
 * function.
 */
bool pci_hp_spurious_link_change(struct pci_dev *pdev)
{
	wait_event(pci_hp_link_change_wq,
		   !test_bit(PCI_LINK_CHANGING, &pdev->priv_flags));

	return test_and_clear_bit(PCI_LINK_CHANGED, &pdev->priv_flags);
}

static int __init pci_hotplug_init(void)
{
	int result;

	result = cpci_hotplug_init(debug);
	if (result) {
		err("cpci_hotplug_init with error %d\n", result);
		return result;
	}

	return result;
}
device_initcall(pci_hotplug_init);

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");
