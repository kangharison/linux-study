// SPDX-License-Identifier: GPL-2.0+
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
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 */

#define pr_fmt(fmt) "acpiphp: " fmt

/*
 * [한국어 설명] ACPI 핫플러그의 sysfs 쪽 얼굴 (acpiphp_core.c)
 *
 * === 파일의 역할 ===
 * acpiphp 는 두 파일로 나뉜다. 이 파일은 핫플러그 코어와 맞닿는 부분이고,
 * 실제 ACPI 네임스페이스 처리와 이벤트 수신은 acpiphp_glue.c 가 한다.
 *
 * ACPI 핫플러그가 따로 필요한 이유는 PCIe 표준 핫플러그가 없는 환경 때문이다.
 *   - 구형 PCI/PCI-X 버스에는 표준 핫플러그 레지스터가 아예 없다.
 *   - 가상화 환경에서 장치를 붙이고 떼는 것도 대개 ACPI 이벤트로 온다.
 *   - PCIe 슬롯이라도 펌웨어가 _OSC 로 소유권을 안 넘기면 pciehp 가 못 붙는다.
 * 이런 경우 슬롯 조작은 ACPI 제어 메서드(_EJ0, _PS0, _STA 등)로 한다.
 *
 * 이 파일이 하는 일은 그 메서드들을 hotplug_slot_ops 모양으로 감싸는 것이다.
 * 사용자가 sysfs 의 power 에 0 을 쓰면 disable_slot() 이 불리고, 그것이
 * acpiphp_glue.c 를 거쳐 결국 ACPI _EJ0 메서드 평가로 이어진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ACPI 코어가 핫플러그 알림(Notify)을 받음
 *   -> acpiphp_glue.c 의 핸들러
 *      -> 슬롯 상태를 갱신하고 열거 또는 제거
 *
 * 사용자가 sysfs 조작
 *   -> pci_hotplug_core.c
 *      -> [이 파일] enable_slot() / disable_slot() / get_*_status()
 *         -> acpiphp_glue.c 의 실제 ACPI 메서드 평가
 *
 * 실행 컨텍스트: sysfs 경로는 프로세스 컨텍스트. ACPI 알림은 ACPI 코어의
 * 워크큐에서 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci_hotplug_core.c.
 * 아래쪽: acpiphp_glue.c, 그리고 그 아래 ACPI 코어(drivers/acpi/).
 * 옆쪽: pci-acpi.c 가 PCI 장치와 ACPI 노드를 잇는 매핑을 제공한다.
 * 공유 상태: struct acpiphp_slot(acpiphp.h) — ACPI 핸들과 슬롯 정보.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 실무에서 의미가 있는 경우는 두 가지다. 하나는 가상 머신에서 NVMe 를
 * 핫플러그로 붙일 때 — QEMU 등이 ACPI 이벤트로 알리므로 pciehp 가 아니라
 * 이 드라이버가 처리한다. 다른 하나는 펌웨어가 _OSC 협상에서 핫플러그
 * 소유권을 유지하는 서버로, 이때도 이쪽이 담당한다.
 *
 * === 주요 함수/구조체 요약 ===
 * init_acpi() / acpiphp_init() : 모듈 초기화. glue 계층을 준비한다.
 * enable_slot() / disable_slot(): sysfs 전원 조작을 ACPI 쪽으로 넘긴다.
 * set_attention_status() / get_attention_status() : Attention LED.
 *                          ACPI 에서는 _STA 등으로 표현된다.
 * get_power_status() / get_latch_status() / get_adapter_status() : 상태 조회.
 * acpiphp_register_hotplug_slot() : glue 가 슬롯을 발견하면 이 함수로
 *                          핫플러그 코어에 등록한다.
 * acpiphp_unregister_hotplug_slot() : 그 반대.
 * acpiphp_hotplug_slot_ops : 위 콜백들을 묶은 표.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci_hotplug.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include "acpiphp.h"
/* name size which is used for entries in pcihpfs */
#define SLOT_NAME_SIZE  21              /* {_SUN} */

bool acpiphp_disabled;

/* local variables */
static struct acpiphp_attention_info *attention_info;

#define DRIVER_VERSION	"0.5"

#define DRIVER_AUTHOR	"Greg Kroah-Hartman <gregkh@us.ibm.com>, Takayoshi Kochi <t-kochi@bq.jp.nec.com>, Matthew Wilcox <willy@infradead.org>"
#define DRIVER_DESC	"ACPI Hot Plug PCI Controller Driver"
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_PARM_DESC(disable, "disable acpiphp driver");
module_param_named(disable, acpiphp_disabled, bool, 0444);

static int enable_slot(struct hotplug_slot *slot);
static int disable_slot(struct hotplug_slot *slot);
static int set_attention_status(struct hotplug_slot *slot, u8 value);
static int get_power_status(struct hotplug_slot *slot, u8 *value);
static int get_attention_status(struct hotplug_slot *slot, u8 *value);
static int get_latch_status(struct hotplug_slot *slot, u8 *value);
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);

static const struct hotplug_slot_ops acpi_hotplug_slot_ops = {
	.enable_slot		= enable_slot,
	.disable_slot		= disable_slot,
	.set_attention_status	= set_attention_status,
	.get_power_status	= get_power_status,
	.get_attention_status	= get_attention_status,
	.get_latch_status	= get_latch_status,
	.get_adapter_status	= get_adapter_status,
};

/**
 * acpiphp_register_attention - set attention LED callback
 * @info: must be completely filled with LED callbacks
 *
 * Description: This is used to register a hardware specific ACPI
 * driver that manipulates the attention LED.  All the fields in
 * info must be set.
 */
int acpiphp_register_attention(struct acpiphp_attention_info *info)
{
	int retval = -EINVAL;

	if (info && info->set_attn && info->get_attn && !attention_info) {
		retval = 0;
		attention_info = info;
	}
	return retval;
}
EXPORT_SYMBOL_GPL(acpiphp_register_attention);


/**
 * acpiphp_unregister_attention - unset attention LED callback
 * @info: must match the pointer used to register
 *
 * Description: This is used to un-register a hardware specific acpi
 * driver that manipulates the attention LED.  The pointer to the
 * info struct must be the same as the one used to set it.
 */
int acpiphp_unregister_attention(struct acpiphp_attention_info *info)
{
	int retval = -EINVAL;

	if (info && attention_info == info) {
		attention_info = NULL;
		retval = 0;
	}
	return retval;
}
EXPORT_SYMBOL_GPL(acpiphp_unregister_attention);


/**
 * enable_slot - power on and enable a slot
 * @hotplug_slot: slot to enable
 *
 * Actual tasks are done in acpiphp_enable_slot()
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* enable the specified slot */
	return acpiphp_enable_slot(slot->acpi_slot);
}


/**
 * disable_slot - disable and power off a slot
 * @hotplug_slot: slot to disable
 *
 * Actual tasks are done in acpiphp_disable_slot()
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* disable the specified slot */
	return acpiphp_disable_slot(slot->acpi_slot);
}


/**
 * set_attention_status - set attention LED
 * @hotplug_slot: slot to set attention LED on
 * @status: value to set attention LED to (0 or 1)
 *
 * attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to blink the light for us.
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	int retval = -ENODEV;

	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));

	if (attention_info && try_module_get(attention_info->owner)) {
		retval = attention_info->set_attn(hotplug_slot, status);
		module_put(attention_info->owner);
	} else
		attention_info = NULL;
	return retval;
}


/**
 * get_power_status - get power status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * Some platforms may not implement _STA method properly.
 * In that case, the value returned may not be reliable.
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	*value = acpiphp_get_power_status(slot->acpi_slot);

	return 0;
}


/**
 * get_attention_status - get attention LED status
 * @hotplug_slot: slot to get status from
 * @value: returns with value of attention LED
 *
 * ACPI doesn't have known method to determine the state
 * of the attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to determine its state.
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int retval = -EINVAL;

	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));

	if (attention_info && try_module_get(attention_info->owner)) {
		retval = attention_info->get_attn(hotplug_slot, value);
		module_put(attention_info->owner);
	} else
		attention_info = NULL;
	return retval;
}


/**
 * get_latch_status - get latch status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access latch status.
 * Instead, we fake latch status from _STA.
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	*value = acpiphp_get_latch_status(slot->acpi_slot);

	return 0;
}


/**
 * get_adapter_status - get adapter status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access adapter status.
 * Instead, we fake adapter status from _STA.
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	*value = acpiphp_get_adapter_status(slot->acpi_slot);

	return 0;
}

/* callback routine to initialize 'struct slot' for each slot */
int acpiphp_register_hotplug_slot(struct acpiphp_slot *acpiphp_slot,
				  unsigned int sun)
{
	struct slot *slot;
	int retval = -ENOMEM;
	char name[SLOT_NAME_SIZE];

	slot = kzalloc_obj(*slot);
	if (!slot)
		goto error;

	slot->hotplug_slot.ops = &acpi_hotplug_slot_ops;

	slot->acpi_slot = acpiphp_slot;

	acpiphp_slot->slot = slot;
	slot->sun = sun;
	snprintf(name, SLOT_NAME_SIZE, "%u", sun);

	retval = pci_hp_register(&slot->hotplug_slot, acpiphp_slot->bus,
				 acpiphp_slot->device, name);
	if (retval == -EBUSY)
		goto error_slot;
	if (retval) {
		pr_err("pci_hp_register failed with error %d\n", retval);
		goto error_slot;
	}

	pr_info("Slot [%s] registered\n", slot_name(slot));

	return 0;
error_slot:
	kfree(slot);
error:
	return retval;
}


void acpiphp_unregister_hotplug_slot(struct acpiphp_slot *acpiphp_slot)
{
	struct slot *slot = acpiphp_slot->slot;

	pr_info("Slot [%s] unregistered\n", slot_name(slot));

	pci_hp_deregister(&slot->hotplug_slot);
	kfree(slot);
}


void __init acpiphp_init(void)
{
	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "%s\n",
		acpiphp_disabled ? ", disabled by user; please report a bug"
				 : "");
}
