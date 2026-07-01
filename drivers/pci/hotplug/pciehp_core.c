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
 * ===================================================================
 * NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c) 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/hotplug/pciehp_core.c)은 PCI Express 핫플러그 컨트롤러
 * 드라이버의 핵심 부분으로, PCIe 슬롯의 장치 삽입/제거(Insertion/Removal),
 * 전원 제어(Power Control), Attention Indicator, Presence Detect Changed(PDC),
 * Link 상태 변화 등을 처리한다.
 *
 * NVMe SSD는 종종 서버/워크스테이션의 핫플러그 가능한 PCIe 슬롯에 연결되므로
 * 본 드라이버의 동작은 NVMe 장치의 생명주기와 직접 연결된다.
 *
 * NVMe 관련 주요 영향 및 호출 경로:
 *   - NVMe SSD가 슬롯에 삽입되면 pciehp는 Presence Detect Changed(PDC) 이벤트를
 *     처리하여 슬롯 전원을 켜고, 링크를 트레이닝(link training)시킨 뒤 PCI bus를
 *     다시 스캔한다. 이후 nvme_probe()가 호출되어 NVMe 컨트롤러가 초기화된다.
 *   - NVMe SSD가 물리적으로 제거(또는 surprise removal)되면 pciehp는 PDC를 받아
 *     pci_dev_set_disconnected() 등을 통해 NVMe 드라이버의 메모리/IO 접근을
 *     막고, nvme_remove()가 안전하게 수행될 수 있도록 한다.
 *   - 핫플러그로 인한 전원 OFF/ON은 NVMe의 DOE(Data Object Exchange), PTM
 *     (Precision Time Measurement), ROM(Option ROM), 대역폭 제어 등과 연관된
 *     PCIe 기능들이 임시로 사용 불가능해짐을 의미한다. 특히 NVMe 장치가 링크
 *     재학습(link retrain)을 거치면 현재 링크 속도/폭(带宽)이 바뀔 수 있다.
 *   - procfs(/sys/bus/pci/slots/...) 및 sysfs를 통해 사용자공간에서 슬롯 상태를
 *     확인하거나 수동으로 enable/disable할 수 있으며, NVMe SSD가 탑재된 슬롯은
 *     이 인터페이스로 제어된다.
 *
 * 핵심 진입 함수:
 *   pciehp_probe()    : PCIe 포트 서비스 드라이버로 등록되며 슬롯 초기화
 *   pciehp_remove()   : 드라이버 해제 시 슬롯 자원 정리
 *   pciehp_check_presence() : 현재 슬롯 점유 상태를 확인하고 이벤트를 합성
 *   pciehp_suspend/resume() : 전원 관리 시 핫플러그 인터럽트 및 상태 복원
 *
 * NVMe 드라이버는 본 파일을 직접 호출하지 않지만, NVMe SSD가 연결된 PCIe 슬롯의
 * 전원/링크/ Presence 상태 변화를 관리하는 관문(gatekeeper) 역할을 한다.
 * ===================================================================
 */

#define pr_fmt(fmt) "pciehp: " fmt
#define dev_fmt pr_fmt

#include <linux/bitfield.h> /* NVMe: PCIe Slot Control/Status 레지스터의 비트 필드 매크로 사용. */
#include <linux/moduleparam.h> /* NVMe: pciehp_poll_mode/pciehp_poll_time 같은 모듈 파라미터 정의용. */
#include <linux/kernel.h> /* NVMe: 커널 기본 타입 및 printk 관련 헤더. */
#include <linux/slab.h> /* NVMe: kzalloc_obj() 등 메모리 할당 함수 제공. */
#include <linux/types.h> /* NVMe: u8 등 기본 자료형 정의. */
#include <linux/pci.h> /* NVMe: PCI 핵심 구조체(pci_dev, pci_bus)와 함수 선언. */
#include "pciehp.h" /* NVMe: pciehp 전용 구조체, 매크로, 함수 원형. */

#include "../pci.h" /* NVMe: PCI 서브시스템 날부 헤더(PCI_EXP_* 정의 등). */

/* Global variables */
bool pciehp_poll_mode; /* NVMe: 핫플러그 이벤트를 인터럽트 대신 폧링으로 처리할지 여부. */
int pciehp_poll_time; /* NVMe: 폧링 방식일 때 이벤트 확인 주기(초 단위). */

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param(pciehp_poll_mode, bool, 0644); /* NVMe: 커널 부트 파라미터로 폧링 모드를 제어할 수 있게 노출. */
module_param(pciehp_poll_time, int, 0644); /* NVMe: 커널 부트 파라미터로 폧링 주기를 제어할 수 있게 노출. */
MODULE_PARM_DESC(pciehp_poll_mode, "Using polling mechanism for hot-plug events or not"); /* NVMe: 폧링 모드 파라미터 설명. */
MODULE_PARM_DESC(pciehp_poll_time, "Polling mechanism frequency, in seconds"); /* NVMe: 폧링 주기 파라미터 설명. */

/*
 * pciehp 핫플러그 슬롯 operation 함수들의 전방 선언.
 * 이 함수들은 NVMe SSD가 장착된 슬롯의 전원, 래치, 어댑터 상태를 sysfs/procfs에
 * 노출하거나 사용자공간의 enable/disable 요청을 처리한다.
 */
static int set_attention_status(struct hotplug_slot *slot, u8 value); /* NVMe: Attention LED(주의 표시등) 설정 함수. */
static int get_power_status(struct hotplug_slot *slot, u8 *value); /* NVMe: 슬롯 전원 상태를 읽어오는 함수. */
static int get_latch_status(struct hotplug_slot *slot, u8 *value); /* NVMe: 슬롯 Mechanical Release Latch 상태 읽기. */
static int get_adapter_status(struct hotplug_slot *slot, u8 *value); /* NVMe: 슬롯에 NVMe 카드가 실제로 존재하는지 확인. */

/*
 * init_slot:
 *   PCIe 핫플러그 슬롯 하나를 초기화하고 PCI 핫플러그 코어에 등록한다.
 *   NVMe SSD가 연결될 수 있는 슬롯마다 이 함수가 호출되어 sysfs/procfs 상의
 *   슬롯 인터페이스를 생성한다.
 */
static int init_slot(struct controller *ctrl)
{
	struct hotplug_slot_ops *ops; /* NVMe: 이 슬롯에서 지원할 operation 테이블 포인터. */
	char name[SLOT_NAME_SIZE]; /* NVMe: sysfs/procfs에 표시될 슬롯 이름 버퍼. */
	int retval; /* NVMe: 각 단계의 반환값을 저장. */

	/* Setup hotplug slot ops */
	ops = kzalloc_obj(*ops); /* NVMe: hotplug_slot_ops 구조체를 0으로 초기화하며 할당. */
	if (!ops) /* NVMe: 메모리 할당 실패 시. */
		return -ENOMEM; /* NVMe: NVMe 슬롯 초기화 실패를 알리는 -ENOMEM 반환. */

	ops->enable_slot = pciehp_sysfs_enable_slot; /* NVMe: 사용자공간 "echo 1 > /sys/bus/pci/slots/.../enable" 시 호출. */
	ops->disable_slot = pciehp_sysfs_disable_slot; /* NVMe: 사용자공간 "echo 0 > .../enable" 시 호출되어 NVMe 전원 차단. */
	ops->get_power_status = get_power_status; /* NVMe: 현재 슬롯 전원 상태를 sysfs/procfs로 노출. */
	ops->get_adapter_status = get_adapter_status; /* NVMe: NVMe 장치 presence(삽입 여부) 상태를 노출. */
	ops->reset_slot = pciehp_reset_slot; /* NVMe: 슬롯 리셋 요청 시 링크 리트레인 등을 수행. */
	if (MRL_SENS(ctrl)) /* NVMe: 이 슬롯이 MRL(Manual Retention Latch) 센서를 가지면. */
		ops->get_latch_status = get_latch_status; /* NVMe: 래치 상태를 읽어오는 operation을 등록. */
	if (ATTN_LED(ctrl)) { /* NVMe: Attention LED 하드웨어가 존재하는 경우. */
		ops->get_attention_status = pciehp_get_attention_status; /* NVMe: Attention LED 상태 읽기. */
		ops->set_attention_status = set_attention_status; /* NVMe: Attention LED 켜기/끄기/깜빡이기. */
	} else if (ctrl->pcie->port->hotplug_user_indicators) { /* NVMe: 플랫폼이 사용자 정의 LED를 사용하는 경우. */
		ops->get_attention_status = pciehp_get_raw_indicator_status; /* NVMe: 사용자 정의 indicator 상태 읽기. */
		ops->set_attention_status = pciehp_set_raw_indicator_status; /* NVMe: 사용자 정의 indicator 상태 설정. */
	}

	/* register this slot with the hotplug pci core */
	ctrl->hotplug_slot.ops = ops; /* NVMe: 컨트롤러의 hotplug_slot에 operation 테이블을 연결. */
	snprintf(name, SLOT_NAME_SIZE, "%u", PSN(ctrl)); /* NVMe: 슬롯 번호를 문자열로 변환하여 이름 생성. */

	retval = pci_hp_initialize(&ctrl->hotplug_slot, /* NVMe: PCI 핫플러그 코어에 슬롯 구조체 등록. */
				   ctrl->pcie->port->subordinate, /* NVMe: 이 슬롯이 속한 하위 PCI bus(포트 아래 bus). */
				   PCI_SLOT_ALL_DEVICES, name); /* NVMe: 모든 슬롯 장치 번호에 대해 이름으로 등록. */
	if (retval) { /* NVMe: 등록에 실패하면. */
		ctrl_err(ctrl, "pci_hp_initialize failed: error %d\n", retval); /* NVMe: NVMe 슬롯 등록 실패 로그. */
		kfree(ops); /* NVMe: 할당한 operation 테이블을 반납. */
	}
	return retval; /* NVMe: 성공(0) 또는 오류 코드 반환. */
}

/*
 * cleanup_slot:
 *   init_slot()에서 등록한 핫플러그 슬롯을 해제한다.
 *   NVMe SSD가 탑재된 슬롯의 sysfs/procfs 노드를 제거할 때 사용된다.
 */
static void cleanup_slot(struct controller *ctrl)
{
	struct hotplug_slot *hotplug_slot = &ctrl->hotplug_slot; /* NVMe: 컨트롤러 낸부의 hotplug_slot 구조체를 가리킴. */

	pci_hp_destroy(hotplug_slot); /* NVMe: PCI 핫플러그 코어에서 슬롯 등록을 제거. */
	kfree(hotplug_slot->ops); /* NVMe: init_slot()에서 할당한 operation 테이블 메모리 해제. */
}

/*
 * set_attention_status - Turns the Attention Indicator on, off or blinking
 */
/*
 * set_attention_status:
 *   Attention Indicator(주의 표시등)를 켜거나 끄거나 깜빡이게 한다.
 *   NVMe SSD가 탑재된 슬롯의 장애/유지보수 상태를 외부 LED로 알릴 때 사용된다.
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: hotplug_slot에서 pciehp 컨트롤러를 얻음. */
	struct pci_dev *pdev = ctrl->pcie->port; /* NVMe: 이 슬롯을 관리하는 PCIe 포트(Upstream switch/Root Port). */

	if (status) /* NVMe: 사용자가 0이 아닌 LED 상태를 요청한 경우. */
		status = FIELD_PREP(PCI_EXP_SLTCTL_AIC, status); /* NVMe: Slot Control Attention Indicator Control 필드에 값을 채움. */
	else /* NVMe: 0이면 Attention LED를 끔. */
		status = PCI_EXP_SLTCTL_ATTN_IND_OFF; /* NVMe: Attention Indicator Off 비트를 설정. */

	pci_config_pm_runtime_get(pdev); /* NVMe: PCIe 포트의 런타임 PM(전원 관리) 참조를 획득해 config 접근이 가능하도록 함. */
	pciehp_set_indicators(ctrl, INDICATOR_NOOP, status); /* NVMe: Attention indicator만 새 상태로 갱신(Power indicator는 변경 없음). */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM 참조를 반납. */
	return 0; /* NVMe: Attention LED 설정 완료. */
}

/*
 * get_power_status:
 *   현재 핫플러그 슬롯의 전원 상태(Power Controller Control)를 읽는다.
 *   NVMe SSD가 전원을 공급받고 있는지 확인할 때 사용된다.
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: sysfs/procfs를 통해 전달된 슬롯에서 컨트롤러 획득. */
	struct pci_dev *pdev = ctrl->pcie->port; /* NVMe: 이 슬롯을 제어하는 PCIe 포트. */

	pci_config_pm_runtime_get(pdev); /* NVMe: 런타임 PM을 깨워 config space에 접근 가능하게 함. */
	pciehp_get_power_status(ctrl, value); /* NVMe: Slot Status/Control 레지스터에서 전원 상태를 읽어 value에 저장. */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM 참조 반납. */
	return 0; /* NVMe: 전원 상태 읽기 완료. */
}

/*
 * get_latch_status:
 *   MRL(Manual Retention Latch) 상태를 읽는다.
 *   NVMe SSD가 물리적으로 고정/해제되었는지 확인하는 데 사용된다.
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: 슬롯 구조체에서 pciehp 컨트롤러를 추출. */
	struct pci_dev *pdev = ctrl->pcie->port; /* NVMe: 상위 PCIe 포트(슬롯을 관리). */

	pci_config_pm_runtime_get(pdev); /* NVMe: PCIe 포트 런타임 전원을 활성화. */
	pciehp_get_latch_status(ctrl, value); /* NVMe: MRL 센서 상태를 읽어 value에 기록. */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 전원 참조를 반납. */
	return 0; /* NVMe: 래치 상태 읽기 완료. */
}

/*
 * get_adapter_status:
 *   슬롯에 NVMe 어댑터(카드)가 실제로 삽입되어 있거나 링크가 활성화되어
 *   있는지를 확인한다. NVMe 드라이버가 probe/remove를 결정하는 핵심 상태다.
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: 슬롯 객체로부터 컨트롤러 포인터 획득. */
	struct pci_dev *pdev = ctrl->pcie->port; /* NVMe: 슬롯을 제어하는 PCIe 포트. */
	int ret; /* NVMe: presence 또는 link active 조사 결과. */

	pci_config_pm_runtime_get(pdev); /* NVMe: 런타임 PM 활성화로 config space 접근 허용. */
	ret = pciehp_card_present_or_link_active(ctrl); /* NVMe: Presence Detect 핀 또는 Link Active 상태로 카드 존재 여부 확인. */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM 참조 반납. */
	if (ret < 0) /* NVMe: 하드웨어 상태 읽기에 실패한 경우. */
		return ret; /* NVMe: 오류 코드를 그대로 상위로 전달. */

	*value = ret; /* NVMe: 0/1 형태의 presence 상태를 호출자가 제공한 value에 기록. */
	return 0; /* NVMe: 어댑터 상태 읽기 성공. */
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
/*
 * pciehp_check_presence:
 *   probe나 resume 시점에 현재 슬롯 상태를 명시적으로 확인하고, 변화가 있으면
 *   Presence Detect Changed(PDC) 이벤트를 합성한다.
 *   NVMe SSD가 시스템 부팅 중이나 절전 복귀 후 이미 삽입/제거된 상태를
 *   올바르게 반영할 수 있도록 하는 핵심 함수다.
 */
static void pciehp_check_presence(struct controller *ctrl)
{
	int occupied; /* NVMe: 슬롯에 NVMe 카드가 존재하면 1, 비어 있으면 0, 오류는 음수. */

	down_read_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: 재귀적 reset 락을 읽기 모드로 획득(동시성 제어). */
	mutex_lock(&ctrl->state_lock); /* NVMe: 슬롯 상태 변경을 보호하기 위해 mutex 획득. */

	occupied = pciehp_card_present_or_link_active(ctrl); /* NVMe: 현재 슬롯의 카드 존재 여부를 다시 확인. */
	if ((occupied > 0 && (ctrl->state == OFF_STATE || /* NVMe: 카드가 있는데 상태가 꺼져 있거나 켜지는 중이 아니면. */
			  ctrl->state == BLINKINGON_STATE)) ||
	    (!occupied && (ctrl->state == ON_STATE || /* NVMe: 카드가 없는데 상태가 켜져 있거나 꺼지는 중이면. */
			   ctrl->state == BLINKINGOFF_STATE)))
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC); /* NVMe: Presence Detect Changed 이벤트를 워커에 요청하여 NVMe probe/remove를 유도. */

	mutex_unlock(&ctrl->state_lock); /* NVMe: 슬롯 상태 락을 해제. */
	up_read(&ctrl->reset_lock); /* NVMe: reset 락 읽기 모드 해제. */
}

/*
 * pciehp_probe:
 *   PCIe 포트 서비스 드라이버의 probe 함수. 핫플러그 서비스를 지원하는
 *   PCIe 포트마다 호출되며, NVMe SSD가 연결될 수 있는 슬롯을 초기화하고
 *   사용자공간에 노출한다.
 */
static int pciehp_probe(struct pcie_device *dev)
{
	int rc; /* NVMe: 초기화 단계별 반환값. */
	struct controller *ctrl; /* NVMe: 이 슬롯의 pciehp 컨트롤러 인스턴스. */

	/* If this is not a "hotplug" service, we have no business here. */
	if (dev->service != PCIE_PORT_SERVICE_HP) /* NVMe: 포트 서비스가 핫플러그가 아니면 관여하지 않음. */
		return -ENODEV; /* NVMe: NVMe와 무관한 포트이므로 -ENODEV 반환. */

	if (!dev->port->subordinate) { /* NVMe: 하위 PCI bus가 없으면(예: bus 번호 부족). */
		/* Can happen if we run out of bus numbers during probe */
		pci_err(dev->port, /* NVMe: PCIe 포트에 대한 에러 메시지 출력. */
			"Hotplug bridge without secondary bus, ignoring\n"); /* NVMe: NVMe 장치를 탐색할 하위 bus가 없음을 알림. */
		return -ENODEV; /* NVMe: 이 포트에서는 핫플러그를 활성화할 수 없음. */
	}

	ctrl = pcie_init(dev); /* NVMe: pciehp 컨트롤러(IRQ, 워커, 상태 머신 등)를 초기화. */
	if (!ctrl) { /* NVMe: 컨트롤러 초기화 실패 시. */
		pci_err(dev->port, "Controller initialization failed\n"); /* NVMe: NVMe 슬롯 관리를 위한 컨트롤러 초기화 실패 로그. */
		return -ENODEV; /* NVMe: probe 실패. */
	}
	set_service_data(dev, ctrl); /* NVMe: pcie_device의 서비스 데이터로 컨트롤러를 저장(나중에 remove/resume 등에서 사용). */

	/* Setup the slot information structures */
	rc = init_slot(ctrl); /* NVMe: sysfs/procfs 핫플러그 슬롯 구조체를 생성하고 등록. */
	if (rc) { /* NVMe: 슬롯 등록에 실패하면. */
		if (rc == -EBUSY) /* NVMe: 이미 다른 핫플러그 드라이버가 이 슬롯을 사용 중인 경우. */
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n"); /* NVMe: 중복 등록 경고. */
		else /* NVMe: 그 외 초기화 오류. */
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", rc); /* NVMe: NVMe 슬롯 초기화 실패 에러. */
		goto err_out_release_ctlr; /* NVMe: 컨트롤러 해제로 이동. */
	}

	/* Enable events after we have setup the data structures */
	rc = pcie_init_notification(ctrl); /* NVMe: 핫플러그 이벤트(Attention Button, PDC, MRL, Link Event) 처리를 활성화. */
	if (rc) { /* NVMe: 이벤트 알림 초기화 실패 시. */
		ctrl_err(ctrl, "Notification initialization failed (%d)\n", rc); /* NVMe: NVMe 장치 삽입/제거 알림 활성화 실패. */
		goto err_out_free_ctrl_slot; /* NVMe: 슬롯 정리 후 컨트롤러 해제. */
	}

	/* Publish to user space */
	rc = pci_hp_add(&ctrl->hotplug_slot); /* NVMe: /sys/bus/pci/slots/... 아래에 슬롯을 노출하여 사용자공간이 NVMe 슬롯을 제어 가능하게 함. */
	if (rc) { /* NVMe: 사용자공간 노출 실패 시. */
		ctrl_err(ctrl, "Publication to user space failed (%d)\n", rc); /* NVMe: sysfs/procfs 등록 실패 에러. */
		goto err_out_shutdown_notification; /* NVMe: 알림을 끈 뒤 슬롯과 컨트롤러 정리. */
	}

	pciehp_check_presence(ctrl); /* NVMe: 초기 등록 시점에 이미 삽입된 NVMe SSD를 발견하면 PDC 이벤트 합성. */

	return 0; /* NVMe: pciehp probe 성공, NVMe 장치 준비 완료. */

err_out_shutdown_notification:
	pcie_shutdown_notification(ctrl); /* NVMe: 실패 시 핫플러그 인터럽트/이벤트 알림을 종료. */
err_out_free_ctrl_slot:
	cleanup_slot(ctrl); /* NVMe: 등록했던 sysfs/procfs 슬롯을 정리. */
err_out_release_ctlr:
	pciehp_release_ctrl(ctrl); /* NVMe: 컨트롤러 인스턴스 메모리와 자원을 해제. */
	return -ENODEV; /* NVMe: 최종 probe 실패 반환. */
}

/*
 * pciehp_remove:
 *   PCIe 포트 서비스 드라이버의 remove 함수. NVMe SSD가 연결된 슬롯의
 *   핫플러그 인프라를 제거한다.
 */
static void pciehp_remove(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev); /* NVMe: probe에서 저장한 pciehp 컨트롤러를 꺼냄. */

	pci_hp_del(&ctrl->hotplug_slot); /* NVMe: /sys/bus/pci/slots/... 에서 슬롯을 제거(NVMe 제어 인터페이스 삭제). */
	pcie_shutdown_notification(ctrl); /* NVMe: 핫플러그 이벤트 인터럽트를 비활성화. */
	cleanup_slot(ctrl); /* NVMe: 핫플러그 슬롯 구조체와 operation 메모리 해제. */
	pciehp_release_ctrl(ctrl); /* NVMe: 컨트롤러 자원을 모두 반납. */
}

#ifdef CONFIG_PM
/*
 * pme_is_native:
 *   현재 포트에서 PME(Power Management Event)를 네이티브하게 처리하는지
 *   확인한다. NVMe 장치가 절전/런타임 전원 이벤트를 보고할 때 핫플러그
 *   인터럽트 처리 방식에 영향을 준다.
 */
static bool pme_is_native(struct pcie_device *dev)
{
	const struct pci_host_bridge *host; /* NVMe: 이 PCIe 트리의 host bridge. */

	host = pci_find_host_bridge(dev->port->bus); /* NVMe: NVMe 포트가 연결된 host bridge를 검색. */
	return pcie_ports_native || host->native_pme; /* NVMe: 전역 설정 또는 host bridge의 native PME 플래그에 따라 true/false 반환. */
}

/*
 * pciehp_disable_interrupt:
 *   절전/런타임 정지 시 하류 링크가 낮아질 때 불필요한 핫플러그 인터럽트가
 *   발생하지 않도록 인터럽트를 끈다. NVMe SSD가 런타임 절전에 들어갈 때
 *   사용된다.
 */
static void pciehp_disable_interrupt(struct pcie_device *dev)
{
	/*
	 * Disable hotplug interrupt so that it does not trigger
	 * immediately when the downstream link goes down.
	 */
	if (pme_is_native(dev)) /* NVMe: native PME 처리가 활성화된 포트에서만. */
		pcie_disable_interrupt(get_service_data(dev)); /* NVMe: 컨트롤러의 핫플러그 인터럽트를 비활성화. */
}

#ifdef CONFIG_PM_SLEEP
/*
 * pciehp_suspend:
 *   시스템 수면(S3/S4) 진입 전 핫플러그 인터럽트를 비활성화한다.
 *   NVMe SSD도 이 시점에 suspend가 시작되며, 슬롯 이벤트가 깨우지 않도록
 *   막는다.
 */
static int pciehp_suspend(struct pcie_device *dev)
{
	/*
	 * If the port is already runtime suspended we can keep it that
	 * way.
	 */
	if (dev_pm_skip_suspend(&dev->port->dev)) /* NVMe: 이미 런타임 suspended 상태면 추가 suspend를 건너뜀. */
		return 0; /* NVMe: NVMe 포트가 이미 절전 상태이므로 처리 완료. */

	pciehp_disable_interrupt(dev); /* NVMe: 핫플러그 인터럽트를 꺼서 수면 중 이벤트 억제. */
	return 0; /* NVMe: suspend 성공. */
}

/*
 * pciehp_resume_noirq:
 *   시스템 수면 복귀(noirq 단계) 시 슬롯 상태를 복원하고, 수면 중에 NVMe
 *   SSD가 교첵되었는지 확인한다. 교첵되었으면 기존 NVMe 디바이스를
 *   disconnected로 표시하고 새로운 PDC 이벤트를 합성한다.
 */
static int pciehp_resume_noirq(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev); /* NVMe: probe에서 저장한 컨트롤러 획득. */

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies; /* NVMe: Slot Control 레지스터 쓰기 시점을 기록(명령 완료 타임아웃 계산용). */
	ctrl->cmd_busy = true; /* NVMe: Slot Control 명령이 진행 중임을 표시. */

	/* clear spurious events from rediscovery of inserted card */
	if (ctrl->state == ON_STATE || ctrl->state == BLINKINGOFF_STATE) { /* NVMe: 슬롯이 켜진 상태에서만 이상 이벤트를 정리. */
		pcie_clear_hotplug_events(ctrl); /* NVMe: 깨어나며 발생한 가짜 핫플러그 이벤트를 클리어. */

		/*
		 * If hotplugged device was replaced with a different one
		 * during system sleep, mark the old device disconnected
		 * (to prevent its driver from accessing the new device)
		 * and synthesize a Presence Detect Changed event.
		 */
		if (pciehp_device_replaced(ctrl)) { /* NVMe: 수면 중 NVMe SSD가 다른 장치로 교첵되었는지 확인. */
			ctrl_dbg(ctrl, "device replaced during system sleep\n"); /* NVMe: 교첵 디버그 메시지. */
			pci_walk_bus(ctrl->pcie->port->subordinate, /* NVMe: 하위 PCI bus를 순회하며. */
				     pci_dev_set_disconnected, NULL); /* NVMe: 기존 pci_dev(NVMe 포함)를 disconnected로 표시해 접근 차단. */
			pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC); /* NVMe: 새 NVMe 장치를 탐색/초기화하도록 PDC 이벤트 합성. */
		}
	}

	return 0; /* NVMe: noirq resume 성공. */
}
#endif

/*
 * pciehp_resume:
 *   시스템/런타임 resume 시 핫플러그 인터럽트를 다시 활성화하고 현재 슬롯
 *   상태를 확인한다. NVMe SSD가 복귀 후 다시 인식되도록 한다.
 */
static int pciehp_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev); /* NVMe: 컨트롤러 인스턴스 획득. */

	if (pme_is_native(dev)) /* NVMe: native PME 처리 포트에서만. */
		pcie_enable_interrupt(ctrl); /* NVMe: 핫플러그 인터럽트를 다시 켜서 NVMe 삽입/제거 감지 재개. */

	pciehp_check_presence(ctrl); /* NVMe: resume 후 실제 슬롯 상태를 확인하고 필요하면 PDC 합성. */

	return 0; /* NVMe: resume 성공. */
}

/*
 * pciehp_runtime_suspend:
 *   런타임 절전 시 핫플러그 인터럽트를 끈다. NVMe SSD가 런타임 PM으로
 *   절전되면 링크가 낮아질 수 있으므로, 이때 발생하는 핫플러그 이벤트를
 *   억제한다.
 */
static int pciehp_runtime_suspend(struct pcie_device *dev)
{
	pciehp_disable_interrupt(dev); /* NVMe: 런타임 suspend 전 핫플러그 인터럽트 비활성화. */
	return 0; /* NVMe: 런타임 suspend 성공. */
}

/*
 * pciehp_runtime_resume:
 *   런타임 복귀 시 Slot Control 명령 타이밍을 초기화하고, 가짜 이벤트를
 *   정리한 뒤 pciehp_resume()를 호출한다. NVMe SSD가 런타임 절전에서 깨어날
 *   때 호출된다.
 */
static int pciehp_runtime_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev); /* NVMe: 컨트롤러 인스턴스 획득. */

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies; /* NVMe: Slot Control 쓰기 시점 기록. */
	ctrl->cmd_busy = true; /* NVMe: 명령 진행 중 플래그 설정. */

	/* clear spurious events from rediscovery of inserted card */
	if ((ctrl->state == ON_STATE || ctrl->state == BLINKINGOFF_STATE) && /* NVMe: 슬롯이 켜진 상태이면서. */
	     pme_is_native(dev)) /* NVMe: native PME 포트인 경우에만. */
		pcie_clear_hotplug_events(ctrl); /* NVMe: 런타임 복귀 중 발생한 가짜 핫플러그 이벤트 클리어. */

	return pciehp_resume(dev); /* NVMe: 인터럽트 재활성화 및 presence 확인을 수행. */
}
#endif /* PM */

/*
 * hpdriver_portdrv:
 *   PCIe 포트 서비스 드라이버 구조체. pciehp 드라이버를 PCIe 핫플러그
 *   서비스에 등록한다. NVMe SSD가 연결될 수 있는 모든 PCIe 포트에서
 *   .probe()가 호출될 수 있다.
 */
static struct pcie_port_service_driver hpdriver_portdrv = {
	.name		= "pciehp", /* NVMe: sysfs/driver/pciehp 등에 표시될 드라이버 이름. */
	.port_type	= PCIE_ANY_PORT, /* NVMe: Root Port, Switch Downstream Port 등 모든 PCIe 포트에 적용 가능. */
	.service	= PCIE_PORT_SERVICE_HP, /* NVMe: PCIe 포트 서비스 중 핫플러그 서비스에 연결. */

	.probe		= pciehp_probe, /* NVMe: 포트 발견 시 NVMe 슬롯 초기화를 위한 probe 콜백. */
	.remove		= pciehp_remove, /* NVMe: 포트 제거 시 NVMe 슬롯 정리를 위한 remove 콜백. */

#ifdef	CONFIG_PM
#ifdef	CONFIG_PM_SLEEP
	.suspend	= pciehp_suspend, /* NVMe: 시스템 수면 전 인터럽트 비활성화. */
	.resume_noirq	= pciehp_resume_noirq, /* NVMe: 시스템 복귀 noirq 단계에서 상태 복원. */
	.resume		= pciehp_resume, /* NVMe: 복귀 시 인터럽트 재활성화 및 presence 확인. */
#endif
	.runtime_suspend = pciehp_runtime_suspend, /* NVMe: 런타임 절전 시 인터럽트 비활성화. */
	runtime_resume	= pciehp_runtime_resume, /* NVMe: 런타임 복귀 시 상태 복원. */
#endif	/* PM */

	.slot_reset	= pciehp_slot_reset, /* NVMe: 슬롯 리셋 이벤트 처리(링크 리트레인 등). */
};

/*
 * pcie_hp_init:
 *   pciehp 모듈 초기화 함수. PCIe 포트 서비스 등록을 통해 핫플러그 드라이버를
 *   커널에 등록한다. 이후 NVMe SSD가 연결된 PCIe 슬롯에서 pciehp_probe()가
 *   호출된다.
 */
int __init pcie_hp_init(void)
{
	int retval = 0; /* NVMe: 등록 결과(0이면 성공). */

	retval = pcie_port_service_register(&hpdriver_portdrv); /* NVMe: pciehp 포트 서비스 드라이버를 PCIe core에 등록. */
	pr_debug("pcie_port_service_register = %d\n", retval); /* NVMe: 등록 결과를 디버그 로그로 출력. */
	if (retval) /* NVMe: 등록 실패 시. */
		pr_debug("Failure to register service\n"); /* NVMe: 등록 실패 디버그 메시지. */

	return retval; /* NVMe: 성공(0) 또는 오류 코드 반환. */
}
