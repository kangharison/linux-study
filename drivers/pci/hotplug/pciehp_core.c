// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Express Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 * Authors:
 *   Dan Zink <dan.zink@compaq.com>
 *   Greg Kroah-Hartman <greg@kroah.com>
 *   Dely Sy <dely.l.sy@intel.com>"
 */

/*
 * [한국어 설명] pciehp 드라이버의 등록과 슬롯 노출 (pciehp_core.c)
 *
 * === 파일의 역할 ===
 * pciehp 를 PCIe 포트 서비스 드라이버로 등록하고, 슬롯을 커널의 hotplug
 * 코어에 노출한다. 네 파일 중 "입구" 에 해당한다.
 *
 * probe 에서 하는 일이 순서대로 넷이다.
 *   1) 이 포트가 정말 핫플러그 슬롯을 갖는지 확인한다(Slot Implemented
 *      비트와 Slot Capabilities). 아니면 바인딩을 거절한다.
 *   2) struct controller 를 만들고 하드웨어를 초기화한다(pciehp_hpc.c).
 *   3) 슬롯을 hotplug 코어에 등록해 /sys/bus/pci/slots/ 에 노출한다.
 *   4) 인터럽트를 켜고, 이미 카드가 꽂혀 있으면 즉시 열거를 시작한다.
 *
 * 4번의 "이미 꽂혀 있으면" 이 중요하다. 부팅 시점에 이미 드라이브가
 * 꽂혀 있는 것이 보통이고, 그때는 삽입 이벤트가 발생하지 않는다.
 * 그래서 초기 상태를 직접 확인해 필요하면 열거를 걸어 준다.
 *
 * 전원 관리도 이 파일이 다룬다. 절전에서 복귀했을 때 그 사이에 카드가
 * 바뀌었을 수 있으므로, 현재 상태를 다시 읽어 기억하던 것과 다르면
 * 적절히 열거하거나 제거한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pcie/portdrv.c 가 HP 서비스 가상 장치를 만들면
 *   -> [이 파일] pciehp_probe()
 *      -> pcie_init() [hpc.c] 로 하드웨어 초기화
 *      -> pci_hp_initialize() [pci_hotplug_core.c] 로 슬롯 등록
 *      -> pcie_enable_notification() [hpc.c] 로 인터럽트 활성화
 *         -> 이후 이벤트는 hpc.c 의 인터럽트 핸들러가 받아
 *            ctrl.c 의 상태 기계로 넘긴다
 *
 * 실행 컨텍스트: probe/remove 와 PM 콜백 — 전부 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c(서비스 등록), hotplug/pci_hotplug_core.c(슬롯 등록).
 * 아래쪽: 같은 디렉터리의 pciehp_hpc.c / _ctrl.c / _pci.c.
 * 공유 상태: struct controller, 그리고 hotplug 코어에 등록한 struct hotplug_slot.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 * 관계는 한 방향이다 — 이 드라이버가 슬롯을 감시하다가 열거를 일으키면
 * 그 결과로 nvme_probe() 가 불린다.
 *
 * 서버에서 이 드라이버가 붙지 못하는 경우가 실제로 있다. 펌웨어가 _OSC
 * 협상에서 핫플러그 소유권을 넘겨주지 않으면(pci-acpi.c 참고) portdrv 가
 * HP 서비스를 만들지 않아 이 드라이버가 바인딩되지 않는다. 그러면
 * NVMe 드라이브를 뽑았다 꽂아도 커널이 알아채지 못한다.
 *
 * === 주요 함수/구조체 요약 ===
 * pciehp_probe()          : 슬롯을 확인하고 컨트롤러를 만들어 등록한다.
 * pciehp_remove()         : 그 반대.
 * pciehp_suspend() / _resume() / _resume_noirq() : 절전 전후 처리.
 *                           복귀 시 상태를 다시 읽어 변화를 반영한다.
 * pciehp_runtime_suspend() / _runtime_resume() : 런타임 절전판.
 * get_power_status() / get_latch_status() / get_adapter_status() /
 * set_attention_status()  : hotplug 코어가 부르는 콜백. 슬롯의 전원,
 *                           래치, 존재, 표시등 상태를 읽고 쓴다.
 * init_slot()             : 그 콜백들을 묶은 표를 **실행 시점에** 만든다.
 *                           정적인 표가 없는 이유는 컨트롤러가 가진 기능에
 *                           따라 채우는 항목이 달라지기 때문이다 —
 *                           MRL 센서가 있어야 get_latch_status 를 넣고,
 *                           attention LED 유무에 따라 attention 콜백 쌍이
 *                           pciehp_get/set_attention_status 가 되거나
 *                           pciehp_get/set_raw_indicator_status 가 된다.
 * pciehp_driver           : 포트 서비스 드라이버 정의.
 */

#define pr_fmt(fmt) "pciehp: " fmt
#define dev_fmt pr_fmt

#include <linux/bitfield.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/pci.h>
#include "pciehp.h"

#include "../pci.h"

/* Global variables */
bool pciehp_poll_mode;
int pciehp_poll_time;

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param(pciehp_poll_mode, bool, 0644);
module_param(pciehp_poll_time, int, 0644);
MODULE_PARM_DESC(pciehp_poll_mode, "Using polling mechanism for hot-plug events or not");
MODULE_PARM_DESC(pciehp_poll_time, "Polling mechanism frequency, in seconds");

static int set_attention_status(struct hotplug_slot *slot, u8 value);
static int get_power_status(struct hotplug_slot *slot, u8 *value);
static int get_latch_status(struct hotplug_slot *slot, u8 *value);
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);

static int init_slot(struct controller *ctrl)
{
	struct hotplug_slot_ops *ops;
	char name[SLOT_NAME_SIZE];
	int retval;

	/* Setup hotplug slot ops */
	ops = kzalloc_obj(*ops);
	if (!ops)
		return -ENOMEM;

	ops->enable_slot = pciehp_sysfs_enable_slot;
	ops->disable_slot = pciehp_sysfs_disable_slot;
	ops->get_power_status = get_power_status;
	ops->get_adapter_status = get_adapter_status;
	ops->reset_slot = pciehp_reset_slot;
	if (MRL_SENS(ctrl))
		ops->get_latch_status = get_latch_status;
	if (ATTN_LED(ctrl)) {
		ops->get_attention_status = pciehp_get_attention_status;
		ops->set_attention_status = set_attention_status;
	} else if (ctrl->pcie->port->hotplug_user_indicators) {
		ops->get_attention_status = pciehp_get_raw_indicator_status;
		ops->set_attention_status = pciehp_set_raw_indicator_status;
	}

	/* register this slot with the hotplug pci core */
	ctrl->hotplug_slot.ops = ops;
	snprintf(name, SLOT_NAME_SIZE, "%u", PSN(ctrl));

	retval = pci_hp_initialize(&ctrl->hotplug_slot,
				   ctrl->pcie->port->subordinate,
				   PCI_SLOT_ALL_DEVICES, name);
	if (retval) {
		ctrl_err(ctrl, "pci_hp_initialize failed: error %d\n", retval);
		kfree(ops);
	}
	return retval;
}

static void cleanup_slot(struct controller *ctrl)
{
	struct hotplug_slot *hotplug_slot = &ctrl->hotplug_slot;

	pci_hp_destroy(hotplug_slot);
	kfree(hotplug_slot->ops);
}

/*
 * set_attention_status - Turns the Attention Indicator on, off or blinking
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;

	if (status)
		status = FIELD_PREP(PCI_EXP_SLTCTL_AIC, status);
	else
		status = PCI_EXP_SLTCTL_ATTN_IND_OFF;

	pci_config_pm_runtime_get(pdev);
	pciehp_set_indicators(ctrl, INDICATOR_NOOP, status);
	pci_config_pm_runtime_put(pdev);
	return 0;
}

static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;

	pci_config_pm_runtime_get(pdev);
	pciehp_get_power_status(ctrl, value);
	pci_config_pm_runtime_put(pdev);
	return 0;
}

static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;

	pci_config_pm_runtime_get(pdev);
	pciehp_get_latch_status(ctrl, value);
	pci_config_pm_runtime_put(pdev);
	return 0;
}

static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;
	int ret;

	pci_config_pm_runtime_get(pdev);
	ret = pciehp_card_present_or_link_active(ctrl);
	pci_config_pm_runtime_put(pdev);
	if (ret < 0)
		return ret;

	*value = ret;
	return 0;
}

/**
 * pciehp_check_presence() - synthesize event if presence has changed
 * @ctrl: controller to check
 *
 * On probe and resume, an explicit presence check is necessary to bring up an
 * occupied slot or bring down an unoccupied slot.  This can't be triggered by
 * events in the Slot Status register, they may be stale and are therefore
 * cleared.  Secondly, sending an interrupt for "events that occur while
 * interrupt generation is disabled [when] interrupt generation is subsequently
 * enabled" is optional per PCIe r4.0, sec 6.7.3.4.
 */
static void pciehp_check_presence(struct controller *ctrl)
{
	int occupied;

	down_read_nested(&ctrl->reset_lock, ctrl->depth);
	mutex_lock(&ctrl->state_lock);

	occupied = pciehp_card_present_or_link_active(ctrl);
	if ((occupied > 0 && (ctrl->state == OFF_STATE ||
			  ctrl->state == BLINKINGON_STATE)) ||
	    (!occupied && (ctrl->state == ON_STATE ||
			   ctrl->state == BLINKINGOFF_STATE)))
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);

	mutex_unlock(&ctrl->state_lock);
	up_read(&ctrl->reset_lock);
}

static int pciehp_probe(struct pcie_device *dev)
{
	int rc;
	struct controller *ctrl;

	/* If this is not a "hotplug" service, we have no business here. */
	if (dev->service != PCIE_PORT_SERVICE_HP)
		return -ENODEV;

	if (!dev->port->subordinate) {
		/* Can happen if we run out of bus numbers during probe */
		pci_err(dev->port,
			"Hotplug bridge without secondary bus, ignoring\n");
		return -ENODEV;
	}

	ctrl = pcie_init(dev);
	if (!ctrl) {
		pci_err(dev->port, "Controller initialization failed\n");
		return -ENODEV;
	}
	set_service_data(dev, ctrl);

	/* Setup the slot information structures */
	rc = init_slot(ctrl);
	if (rc) {
		if (rc == -EBUSY)
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n");
		else
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", rc);
		goto err_out_release_ctlr;
	}

	/* Enable events after we have setup the data structures */
	rc = pcie_init_notification(ctrl);
	if (rc) {
		ctrl_err(ctrl, "Notification initialization failed (%d)\n", rc);
		goto err_out_free_ctrl_slot;
	}

	/* Publish to user space */
	rc = pci_hp_add(&ctrl->hotplug_slot);
	if (rc) {
		ctrl_err(ctrl, "Publication to user space failed (%d)\n", rc);
		goto err_out_shutdown_notification;
	}

	pciehp_check_presence(ctrl);

	return 0;

err_out_shutdown_notification:
	pcie_shutdown_notification(ctrl);
err_out_free_ctrl_slot:
	cleanup_slot(ctrl);
err_out_release_ctlr:
	pciehp_release_ctrl(ctrl);
	return -ENODEV;
}

static void pciehp_remove(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	pci_hp_del(&ctrl->hotplug_slot);
	pcie_shutdown_notification(ctrl);
	cleanup_slot(ctrl);
	pciehp_release_ctrl(ctrl);
}

#ifdef CONFIG_PM
static bool pme_is_native(struct pcie_device *dev)
{
	const struct pci_host_bridge *host;

	host = pci_find_host_bridge(dev->port->bus);
	return pcie_ports_native || host->native_pme;
}

static void pciehp_disable_interrupt(struct pcie_device *dev)
{
	/*
	 * Disable hotplug interrupt so that it does not trigger
	 * immediately when the downstream link goes down.
	 */
	if (pme_is_native(dev))
		pcie_disable_interrupt(get_service_data(dev));
}

#ifdef CONFIG_PM_SLEEP
static int pciehp_suspend(struct pcie_device *dev)
{
	/*
	 * If the port is already runtime suspended we can keep it that
	 * way.
	 */
	if (dev_pm_skip_suspend(&dev->port->dev))
		return 0;

	pciehp_disable_interrupt(dev);
	return 0;
}

static int pciehp_resume_noirq(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies;
	ctrl->cmd_busy = true;

	/* clear spurious events from rediscovery of inserted card */
	if (ctrl->state == ON_STATE || ctrl->state == BLINKINGOFF_STATE) {
		pcie_clear_hotplug_events(ctrl);

		/*
		 * If hotplugged device was replaced with a different one
		 * during system sleep, mark the old device disconnected
		 * (to prevent its driver from accessing the new device)
		 * and synthesize a Presence Detect Changed event.
		 */
		if (pciehp_device_replaced(ctrl)) {
			ctrl_dbg(ctrl, "device replaced during system sleep\n");
			pci_walk_bus(ctrl->pcie->port->subordinate,
				     pci_dev_set_disconnected, NULL);
			pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		}
	}

	return 0;
}
#endif

static int pciehp_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	if (pme_is_native(dev))
		pcie_enable_interrupt(ctrl);

	pciehp_check_presence(ctrl);

	return 0;
}

static int pciehp_runtime_suspend(struct pcie_device *dev)
{
	pciehp_disable_interrupt(dev);
	return 0;
}

static int pciehp_runtime_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies;
	ctrl->cmd_busy = true;

	/* clear spurious events from rediscovery of inserted card */
	if ((ctrl->state == ON_STATE || ctrl->state == BLINKINGOFF_STATE) &&
	     pme_is_native(dev))
		pcie_clear_hotplug_events(ctrl);

	return pciehp_resume(dev);
}
#endif

static struct pcie_port_service_driver hpdriver_portdrv = {
	.name		= "pciehp",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_HP,

	.probe		= pciehp_probe,
	.remove		= pciehp_remove,

#ifdef	CONFIG_PM
#ifdef	CONFIG_PM_SLEEP
	.suspend	= pciehp_suspend,
	.resume_noirq	= pciehp_resume_noirq,
	.resume		= pciehp_resume,
#endif
	.runtime_suspend = pciehp_runtime_suspend,
	/* [한국어] 런타임 복귀 시 슬롯 상태를 복원한다.
	 * 주의: 앞의 점(.)은 지정 초기화자(designated initializer) 문법의 일부이며
	 * 생략하면 컴파일되지 않는다 — 주석을 달다 지우기 쉬운 문자다. */
	.runtime_resume	= pciehp_runtime_resume,
#endif

	.slot_reset	= pciehp_slot_reset,
};

int __init pcie_hp_init(void)
{
	int retval = 0;

	retval = pcie_port_service_register(&hpdriver_portdrv);
	pr_debug("pcie_port_service_register = %d\n", retval);
	if (retval)
		pr_debug("Failure to register service\n");

	return retval;
}
