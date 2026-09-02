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
/* [한국어] 폴링 모드 여부. 인터럽트가 동작하지 않는 하드웨어에서 이벤트를 주기적으로
 * 읽어 확인하게 한다.
 * 설정자: 부팅 인자 또는 sysfs 쓰기(0644).
 * 읽는 자: pciehp_request() 가 IRQ 스레드를 깨울지 말지 정하는 데 쓰고,
 * pcie_init_notification() 이 폴링 스레드를 띄울지 정하는 데 쓴다.
 * 값 범위: false = 인터럽트, true = 폴링.
 * 동기화: 없다. 런타임에 바꾸면 이미 초기화된 컨트롤러에는 반영되지 않는다. */
bool pciehp_poll_mode;
/* [한국어] 폴링 주기(초).
 * 설정자: 부팅 인자 또는 sysfs 쓰기(0644).
 * 읽는 자: 폴링 스레드가 다음 확인까지 얼마나 잘지 정하는 데 쓴다.
 * 값 범위: 유효 범위 밖의 값은 폴링 쪽에서 기본값으로 보정한다.
 * 동기화: 없다. 폴링 스레드가 매 주기 읽으므로 변경이 다음 주기에 반영된다. */
int pciehp_poll_time;

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param(pciehp_poll_mode, bool, 0644);
module_param(pciehp_poll_time, int, 0644);
/* [한국어] 부팅 인자와 sysfs 로 폴링 모드를 설명한다. 인터럽트가 동작하지 않는
 * 하드웨어에서 쓰는 우회 수단이다. */
MODULE_PARM_DESC(pciehp_poll_mode, "Using polling mechanism for hot-plug events or not");
MODULE_PARM_DESC(pciehp_poll_time, "Polling mechanism frequency, in seconds");

static int set_attention_status(struct hotplug_slot *slot, u8 value);
/* [한국어] 전원 상태 조회 콜백 선언. */
static int get_power_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] 걸쇠 상태 조회 콜백 선언. */
static int get_latch_status(struct hotplug_slot *slot, u8 *value);
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);

/* [한국어]
 * init_slot - 이 컨트롤러가 지원하는 기능만 담은 ops 표를 만들어 슬롯을 준비한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 = 성공, -ENOMEM / -EBUSY 등.
 *
 * ops 표를 **슬롯마다 따로 할당** 하는 것이 이 함수의 핵심이다. 정적 표
 * 하나를 공유하지 않는 이유는 슬롯마다 지원 기능이 다르기 때문이다 —
 * pci_hotplug_core.c 의 has_ 계열이 이 표를 보고 sysfs 파일을 만들지 말지
 * 정하므로, 지원하지 않는 기능의 자리는 비워 두어야 그 파일이 생기지 않는다.
 *
 * 기본 넷(enable, disable, get_power_status, get_adapter_status, reset)은
 * 언제나 채운다. 나머지는 하드웨어에 따라 갈린다.
 * - 걸쇠 센서(MRL)가 있으면 걸쇠 조회를 채운다.
 * - 주의 표시등이 있으면 그 조회·설정을 채운다.
 * - 주의 표시등은 없지만 사용자 표시등을 허용한 포트라면, 표시등 레지스터를
 *   **날것 그대로** 다루는 함수 쌍을 채운다. 그 경우 사용자가 쓰는 값이
 *   규격의 표시등 인코딩 그대로라 의미가 다르다.
 *
 * 슬롯 이름을 Physical Slot Number 로 짓는다. 그것이 하드웨어가 알려 주는
 * 물리적 슬롯 번호라, 사용자가 실제 섀시의 슬롯과 짝지을 수 있다.
 *
 * PCI_SLOT_ALL_DEVICES 를 넘기는 것은 이 슬롯이 그 버스의 모든 장치 번호를
 * 아우른다는 뜻이다. 핫플러그 슬롯 아래에는 다중 기능 카드가 올 수 있다.
 *
 * 실행 컨텍스트: probe. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM. 등록이 실패하면 방금 만든 표를 해제하고
 * 그 오류를 올려보낸다. -EBUSY 는 다른 핫플러그 드라이버가 같은 슬롯을 이미
 * 등록했다는 뜻으로, 호출자가 그 경우만 따로 알린다.
 *
 * 호출 체인:
 *   pciehp_probe() → [이 함수] → kzalloc_obj() → pci_hp_initialize()
 */
static int init_slot(struct controller *ctrl)
{
	struct hotplug_slot_ops *ops;
	char name[SLOT_NAME_SIZE];
	/* [한국어] 슬롯 등록의 결과. */
	int retval;

	/* Setup hotplug slot ops */
	ops = kzalloc_obj(*ops);
	if (!ops)
		/* [한국어] 표를 잡지 못하면 이 슬롯은 등록할 수 없다. */
		return -ENOMEM;

	ops->enable_slot = pciehp_sysfs_enable_slot;
	/* [한국어] sysfs 의 끄기도 마찬가지로 요청 큐를 거친다. */
	ops->disable_slot = pciehp_sysfs_disable_slot;
	/* [한국어] 전원 상태 조회. 모든 슬롯이 지원한다. */
	ops->get_power_status = get_power_status;
	/* [한국어] 카드 존재 조회. 역시 모든 슬롯이 지원한다. */
	ops->get_adapter_status = get_adapter_status;
	/* [한국어] 슬롯 리셋. Secondary Bus Reset 을 거는 경로다. */
	ops->reset_slot = pciehp_reset_slot;
	/* [한국어] 걸쇠 센서(MRL, Manually-operated Retention Latch)가 있는 슬롯만, */
	if (MRL_SENS(ctrl))
		/* [한국어] 걸쇠 조회를 채운다. 없는 슬롯에서는 이 자리가 비어 있어
		 * 공용 코어가 latch 파일 자체를 만들지 않는다. */
		ops->get_latch_status = get_latch_status;
	/* [한국어] 주의 표시등이 있으면, */
	if (ATTN_LED(ctrl)) {
		/* [한국어] 규격의 표시등 인코딩을 해석하는 조회 함수를 쓴다. */
		ops->get_attention_status = pciehp_get_attention_status;
		/* [한국어] 설정 쪽은 이 파일의 함수를 쓴다 — 사용자 값을 규격 필드 자리로 옮겨야 하기 때문이다. */
		ops->set_attention_status = set_attention_status;
	/* [한국어] 주의 표시등은 없지만 포트가 사용자 표시등 조작을 허용하면, */
	} else if (ctrl->pcie->port->hotplug_user_indicators) {
		/* [한국어] 표시등 레지스터를 날것 그대로 읽는 함수를 쓴다. */
		ops->get_attention_status = pciehp_get_raw_indicator_status;
		/* [한국어] 쓰기도 날것 그대로다. 이 경우 사용자가 쓰는 값이 규격 인코딩 자체라
		 * 위 갈래와 값의 의미가 다르다. */
		ops->set_attention_status = pciehp_set_raw_indicator_status;
	/* [한국어] 둘 다 아니면 주의 표시등 관련 자리가 비어, attention 파일이 만들어지지 않는다. */
	}

	/* register this slot with the hotplug pci core */
	ctrl->hotplug_slot.ops = ops;
	snprintf(name, SLOT_NAME_SIZE, "%u", PSN(ctrl));

	retval = pci_hp_initialize(&ctrl->hotplug_slot,
				   /* [한국어] 슬롯이 붙을 버스는 이 포트의 하위 버스다. */
				   ctrl->pcie->port->subordinate,
				   PCI_SLOT_ALL_DEVICES, name);
	if (retval) {
		/* [한국어] 어느 단계에서 실패했는지 남기고, */
		ctrl_err(ctrl, "pci_hp_initialize failed: error %d\n", retval);
		/* [한국어] 방금 만든 표를 해제한다. 등록에 실패했으므로 이 표를 가리킬 주체가 없다. */
		kfree(ops);
	}
	return retval;
}

/* [한국어]
 * cleanup_slot - 슬롯 등록을 되돌리고 ops 표를 해제한다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * init_slot() 의 짝이다.
 *
 * 순서가 중요하다. 먼저 슬롯을 없애고 그 다음에 표를 해제한다. 반대로 하면
 * 슬롯이 아직 살아 있는 동안 그것이 가리키는 표가 사라져, 그 사이에 들어온
 * sysfs 접근이 해제된 메모리를 부른다.
 *
 * 실행 컨텍스트: remove, 또는 probe 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_remove() / pciehp_probe() 의 되감기 → [이 함수]
 *     → pci_hp_destroy() → kfree()
 */
static void cleanup_slot(struct controller *ctrl)
{
	struct hotplug_slot *hotplug_slot = &ctrl->hotplug_slot;

	pci_hp_destroy(hotplug_slot);
	kfree(hotplug_slot->ops);
}

/*
 * set_attention_status - Turns the Attention Indicator on, off or blinking
 */
/* [한국어]
 * set_attention_status - 주의 표시등을 설정한다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @status: 사용자가 쓴 값.
 * @return: 언제나 0.
 *
 * 값 변환이 이 함수의 일이다. 사용자가 쓴 값을 규격의 Attention Indicator
 * Control 필드 자리로 옮기는데, 0 만은 예외로 "꺼짐" 인코딩을 직접 쓴다.
 * 그 두 경로가 다른 이유는 필드에 0 을 넣는 것이 "변경 없음" 을 뜻해,
 * 그대로 쓰면 표시등이 꺼지지 않기 때문이다.
 *
 * 런타임 PM 참조를 잡는 것이 이 파일의 sysfs 콜백 넷에 공통이다. config
 * 접근을 하려면 포트가 깨어 있어야 하고, 그러지 않으면 읽기가 모두 1 로
 * 돌아온다.
 *
 * 전원 표시등에는 INDICATOR_NOOP 을 넘겨 건드리지 않는다. 두 표시등이 한
 * 레지스터에 있어 함께 쓰이는데, 이 함수는 주의 쪽만 바꾸려 하기 때문이다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 쓰기. PM 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 없다. 아래 함수들이 실패를 알리지 않는다.
 *
 * 호출 체인:
 *   echo N > .../attention → pci_hotplug_core.c → ops->set_attention_status
 *   == [이 함수] → pci_config_pm_runtime_get() → pciehp_set_indicators()
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;

	if (status)
		/* [한국어] 0 이 아닌 값은 Attention Indicator Control 필드 자리로 옮긴다. */
		status = FIELD_PREP(PCI_EXP_SLTCTL_AIC, status);
	/* [한국어] 0 이면 — */
	else
		status = PCI_EXP_SLTCTL_ATTN_IND_OFF;

	pci_config_pm_runtime_get(pdev);
	pciehp_set_indicators(ctrl, INDICATOR_NOOP, status);
	/* [한국어] 런타임 PM 참조를 놓는다. 이제 포트가 다시 절전에 들어가도 된다. */
	pci_config_pm_runtime_put(pdev);
	return 0;
}

/* [한국어]
 * get_power_status - 슬롯 전원 상태를 읽는다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @value: 결과를 담을 자리.
 * @return: 언제나 0.
 *
 * 이 파일의 조회 콜백 셋(power, latch, adapter)이 같은 형태다 — PM 참조를
 * 잡고, 읽고, 놓는다.
 *
 * PM 참조가 필요한 이유는 위 set_attention_status() 와 같다. 포트가 자고
 * 있으면 config 읽기가 모두 1 로 돌아와 "전원이 켜져 있다" 로 잘못 읽힌다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 읽기. PM 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../power → pci_hotplug_core.c → ops->get_power_status == [이 함수]
 *     → pciehp_get_power_status()
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 공용 코어가 준 포인터에서 이 드라이버의 컨트롤러를 되찾는다. */
	struct controller *ctrl = to_ctrl(hotplug_slot);
	/* [한국어] config 접근의 대상이 되는 포트. 아래 PM 참조도 이 장치에 건다. */
	struct pci_dev *pdev = ctrl->pcie->port;

	pci_config_pm_runtime_get(pdev);
	pciehp_get_power_status(ctrl, value);
	/* [한국어] 참조를 놓는다. */
	pci_config_pm_runtime_put(pdev);
	return 0;
}

/* [한국어]
 * get_latch_status - 슬롯 걸쇠 상태를 읽는다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @value: 결과를 담을 자리.
 * @return: 언제나 0.
 *
 * get_power_status() 와 같은 형태이며 대상만 다르다.
 *
 * 이 콜백은 걸쇠 센서(MRL)가 있는 슬롯에만 등록된다. init_slot() 이 그
 * 조건을 확인하므로, 여기까지 왔다면 센서가 있다는 것이 보장된다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 읽기. PM 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../latch → pci_hotplug_core.c → ops->get_latch_status == [이 함수]
 *     → pciehp_get_latch_status()
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 공용 코어가 준 포인터에서 컨트롤러를 되찾는다. */
	struct controller *ctrl = to_ctrl(hotplug_slot);
	/* [한국어] PM 참조를 걸 포트. 이 콜백은 걸쇠 센서가 있는 슬롯에만 등록되므로,
	 * 여기까지 왔다면 센서가 있다는 것이 보장된다. */
	struct pci_dev *pdev = ctrl->pcie->port;

	pci_config_pm_runtime_get(pdev);
	pciehp_get_latch_status(ctrl, value);
	/* [한국어] 참조를 놓는다. */
	pci_config_pm_runtime_put(pdev);
	return 0;
}

/* [한국어]
 * get_adapter_status - 카드가 있는지 읽는다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @value: 결과를 담을 자리.
 * @return: 0 = 성공, 음수 오류.
 *
 * 앞의 두 조회와 달리 오류를 돌려줄 수 있다. 아래 함수가 카드 감지와 링크
 * 상태를 함께 보는데, 그 과정에서 실패할 수 있기 때문이다.
 *
 * 카드 감지만이 아니라 링크도 보는 이유가 있다. 카드 감지 핀이 없거나
 * 믿을 수 없는 하드웨어가 있어, 링크가 서 있다는 것이 카드가 있다는 더
 * 확실한 증거인 경우가 있다.
 *
 * 오류일 때는 value 를 건드리지 않는다. 그때 사용자에게 나가는 것은 값이
 * 아니라 오류이므로 채울 필요가 없다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 읽기. PM 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 아래 함수의 음수 결과를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../adapter → pci_hotplug_core.c → ops->get_adapter_status
 *   == [이 함수] → pciehp_card_present_or_link_active()
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl->pcie->port;
	/* [한국어] 아래 함수의 결과. 이 파일의 조회 콜백 중 유일하게 실패할 수 있어
	 * 결과를 따로 받는다. */
	int ret;

	pci_config_pm_runtime_get(pdev);
	ret = pciehp_card_present_or_link_active(ctrl);
	/* [한국어] 참조를 놓는다. 실패 여부를 보기 전에 놓는 것이 요점으로,
	 * 어느 갈래로 빠지든 참조가 새지 않는다. */
	pci_config_pm_runtime_put(pdev);
	if (ret < 0)
		/* [한국어] 실패했으면 value 를 건드리지 않고 그 오류를 올려보낸다. */
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
/* [한국어]
 * pciehp_check_presence - 지금 하드웨어 상태가 소프트웨어 상태와 어긋나면 바로잡는다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * 이 함수가 필요한 이유는 **인터럽트를 놓칠 수 있는 구간** 이 있기 때문이다.
 * probe 가 끝나기 전과 절전 중에는 인터럽트를 받지 못하므로, 그동안 카드가
 * 꽂히거나 뽑혔다면 그 변화를 아무도 모른다.
 *
 * 그래서 probe 마지막과 resume 에서 이 함수를 불러 실제 상태를 읽고, 소프트웨어
 * 상태와 어긋나면 이벤트를 하나 만들어 낸다.
 *
 * 어긋남의 판정이 두 갈래다 — 카드가 있는데 꺼진 상태로 알고 있거나, 카드가
 * 없는데 켜진 상태로 알고 있거나. 두 경우 모두 PDC 요청을 내면, 그 처리
 * 경로가 실제 상태를 다시 읽어 올바른 방향으로 처리한다.
 *
 * reset_lock 을 읽기로 잡는 것이 중요하다. 리셋 중에는 링크가 내려가므로,
 * 그때 읽은 상태로 판단하면 카드가 뽑힌 것으로 오인한다. _nested 판을 쓰는
 * 것은 브리지가 겹칠 때 같은 종류의 잠금을 여러 겹 잡게 되어, lockdep 에
 * 그 깊이를 알려 줘야 하기 때문이다.
 *
 * 실행 컨텍스트: probe 마지막, resume. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_probe() / pciehp_resume() → [이 함수]
 *     → pciehp_card_present_or_link_active() → pciehp_request()
 */
static void pciehp_check_presence(struct controller *ctrl)
{
	int occupied;

	down_read_nested(&ctrl->reset_lock, ctrl->depth);
	/* [한국어] 상태 변수를 읽고 요청을 내는 동안 잠금을 쥔다. */
	mutex_lock(&ctrl->state_lock);

	occupied = pciehp_card_present_or_link_active(ctrl);
	/* [한국어] 카드가 있는데 꺼진 상태로 알고 있거나, */
	if ((occupied > 0 && (ctrl->state == OFF_STATE ||
			  /* [한국어] 켜기를 기다리던 중이거나 — */
			  ctrl->state == BLINKINGON_STATE)) ||
	    (!occupied && (ctrl->state == ON_STATE ||
			   ctrl->state == BLINKINGOFF_STATE)))
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);

	mutex_unlock(&ctrl->state_lock);
	up_read(&ctrl->reset_lock);
}

/* [한국어]
 * pciehp_probe - 이 포트에 핫플러그 서비스를 붙인다
 *
 * @dev: 포트 서비스 장치.
 * @return: 0 = 성공, -ENODEV.
 *
 * 포트 드라이버가 핫플러그 서비스를 가진 포트마다 부른다.
 *
 * 앞의 두 검사가 방어적이다. 서비스 종류를 다시 확인하는 것은 이 함수가
 * 핫플러그 서비스에만 등록되어 있어 원래 필요 없지만, 하위 버스가 없는
 * 브리지를 걸러 내는 두 번째 검사는 실질적이다 — 그런 브리지는 아래에 아무것도
 * 올 수 없어 핫플러그 자체가 무의미하다.
 *
 * 네 단계를 순서대로 밟고, 각 단계마다 되감기 라벨이 있다.
 * 1. 컨트롤러 초기화 — 레지스터를 읽어 이 슬롯의 능력을 파악한다.
 * 2. 슬롯 준비 — ops 표를 만들고 커널 안에 등록한다.
 * 3. 알림 초기화 — 인터럽트를 걸고 이벤트를 받기 시작한다.
 * 4. 사용자 공간 노출 — sysfs 항목이 생긴다.
 *
 * 3번과 4번의 순서가 요점이다. 인터럽트를 먼저 켜는데, 그래야 sysfs 가
 * 열리는 순간 이미 이벤트를 처리할 준비가 되어 있다.
 *
 * 마지막의 존재 확인이 probe 중에 놓친 변화를 메운다. 3번 이전에 카드가
 * 꽂혔다면 그 이벤트는 아무도 받지 못했다.
 *
 * 되감기가 역순 계단이다. 각 라벨이 그 지점까지 성공한 것만 되돌린다.
 *
 * 실행 컨텍스트: 포트 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 단계에서 실패하든 -ENODEV 로 통일해 돌려준다. 원래
 * 오류 코드는 로그에만 남는다.
 *
 * 호출 체인:
 *   포트 드라이버 → [이 함수]
 *     → pcie_init() → init_slot() → pcie_init_notification()
 *     → pci_hp_add() → pciehp_check_presence()
 */
static int pciehp_probe(struct pcie_device *dev)
{
	/* [한국어] 각 단계의 결과. */
	int rc;
	/* [한국어] 이 포트의 컨트롤러. 아래에서 만들어진다. */
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
	/* [한국어] 컨트롤러 초기화가 실패했으면, */
	if (!ctrl) {
		/* [한국어] 그 사실을 남기고, */
		pci_err(dev->port, "Controller initialization failed\n");
		/* [한국어] -ENODEV 로 물러난다. 되감을 것이 아직 없다. */
		return -ENODEV;
	}
	set_service_data(dev, ctrl);

	/* Setup the slot information structures */
	rc = init_slot(ctrl);
	if (rc) {
		/* [한국어] 다른 핫플러그 드라이버가 같은 슬롯을 이미 등록한 경우는, */
		if (rc == -EBUSY)
			/* [한국어] 경고 수준으로 남긴다. 오류라기보다 구성의 문제이기 때문이다. */
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n");
		/* [한국어] 그 밖의 실패는 — */
		else
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", rc);
		/* [한국어] 컨트롤러만 되돌리면 된다. */
		goto err_out_release_ctlr;
	}

	/* Enable events after we have setup the data structures */
	rc = pcie_init_notification(ctrl);
	if (rc) {
		/* [한국어] 알림 초기화가 실패했음을 남기고, */
		ctrl_err(ctrl, "Notification initialization failed (%d)\n", rc);
		/* [한국어] 슬롯과 컨트롤러를 되돌린다. */
		goto err_out_free_ctrl_slot;
	}

	/* Publish to user space */
	rc = pci_hp_add(&ctrl->hotplug_slot);
	if (rc) {
		/* [한국어] 사용자 공간 노출이 실패했음을 남기고, */
		ctrl_err(ctrl, "Publication to user space failed (%d)\n", rc);
		/* [한국어] 알림·슬롯·컨트롤러를 차례로 되돌린다. */
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

/* [한국어]
 * pciehp_remove - 이 포트에서 핫플러그 서비스를 뗀다
 *
 * @dev: 포트 서비스 장치.
 *
 * pciehp_probe() 의 짝이며 정확히 역순이다.
 *
 * 1. 사용자 공간에서 내린다 — 새 조작이 들어오지 못하게 먼저 막는다.
 * 2. 알림을 끈다 — 인터럽트가 더는 오지 않는다.
 * 3. 슬롯 등록을 되돌리고 ops 표를 해제한다.
 * 4. 컨트롤러를 해제한다.
 *
 * 이 순서여야 하는 이유는 각 단계가 뒤 단계의 자원을 쓰기 때문이다. sysfs 를
 * 먼저 닫지 않으면 그 뒤 단계 도중에 사용자 조작이 들어와 해제 중인 구조를
 * 건드린다.
 *
 * 실행 컨텍스트: 포트 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 드라이버 → [이 함수]
 *     → pci_hp_del() → pcie_shutdown_notification() → cleanup_slot()
 *     → pciehp_release_ctrl()
 */
static void pciehp_remove(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	pci_hp_del(&ctrl->hotplug_slot);
	pcie_shutdown_notification(ctrl);
	cleanup_slot(ctrl);
	pciehp_release_ctrl(ctrl);
}

#ifdef CONFIG_PM
/* [한국어]
 * pme_is_native - 이 포트의 PME 를 커널이 직접 다루는지 판단한다
 *
 * @dev: 포트 서비스 장치.
 * @return: true = 커널이 다룬다, false = 펌웨어가 다룬다.
 *
 * PME(Power Management Event)와 핫플러그 인터럽트는 같은 인터럽트 벡터를
 * 공유한다. 그래서 PME 를 펌웨어(ACPI)가 다루기로 한 시스템에서는 커널이
 * 그 인터럽트를 마음대로 끄면 안 된다 — 껐다가 펌웨어의 PME 알림까지 막게
 * 된다.
 *
 * 이 판단이 아래 절전 경로에서 쓰이며, 인터럽트를 꺼도 되는지를 정한다.
 *
 * 두 조건 중 하나면 참이다. 부팅 인자로 포트 서비스를 커널이 맡도록 강제했거나,
 * 펌웨어가 PME 제어권을 커널에 넘겼거나.
 *
 * CONFIG_PM 안에만 있는 것은 절전 경로에서만 쓰이기 때문이다.
 *
 * 실행 컨텍스트: 절전·복귀 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_disable_interrupt() / pciehp_resume() / pciehp_runtime_resume()
 *     → [이 함수] → pci_find_host_bridge()
 */
static bool pme_is_native(struct pcie_device *dev)
{
	const struct pci_host_bridge *host;

	host = pci_find_host_bridge(dev->port->bus);
	/* [한국어] 부팅 인자로 포트 서비스를 커널이 맡도록 강제했거나, 펌웨어가 PME 제어권을
	 * 커널에 넘겼으면 참이다. 둘 중 하나면 인터럽트를 끄고 켜도 된다. */
	return pcie_ports_native || host->native_pme;
}

/* [한국어]
 * pciehp_disable_interrupt - 커널이 PME 를 다룰 때만 인터럽트를 끈다
 *
 * @dev: 포트 서비스 장치.
 *
 * 절전에 들어갈 때 인터럽트를 끄는 이유는, 자고 있는 동안 온 핫플러그
 * 이벤트를 처리할 수 없기 때문이다. 켜 둔 채로 두면 깨어나지 못한 상태에서
 * 인터럽트가 반복해 올라온다.
 *
 * 그런데 무조건 끌 수는 없다. PME 를 펌웨어가 다루는 시스템에서는 같은
 * 벡터를 공유하므로, 끄면 펌웨어의 PME 알림까지 막힌다. pme_is_native() 가
 * 그 조건을 가른다.
 *
 * 실행 컨텍스트: 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_suspend() / pciehp_runtime_suspend() → [이 함수]
 *     → pme_is_native() → pcie_disable_interrupt()
 */
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
/* [한국어]
 * pciehp_suspend - 시스템 절전 진입 시 인터럽트를 끈다
 *
 * @dev: 포트 서비스 장치.
 * @return: 언제나 0.
 *
 * dev_pm_skip_suspend() 를 먼저 보는 것이 요점이다. 참이면 이 장치가
 * 런타임 절전 상태 그대로 시스템 절전에 들어간다는 뜻인데, 그때는 이미
 * pciehp_runtime_suspend() 가 인터럽트를 껐으므로 또 끌 필요가 없다.
 *
 * CONFIG_PM_SLEEP 안에만 있다. 런타임 절전만 쓰고 시스템 절전은 쓰지 않는
 * 구성에서는 이 함수가 아예 없다.
 *
 * 실행 컨텍스트: 시스템 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 드라이버의 절전 → [이 함수]
 *     → dev_pm_skip_suspend() → pciehp_disable_interrupt()
 */
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

/* [한국어]
 * pciehp_resume_noirq - 인터럽트가 아직 꺼진 단계에서 자는 동안의 변화를 확인한다
 *
 * @dev: 포트 서비스 장치.
 * @return: 언제나 0.
 *
 * 절전 중에 카드가 바뀌었을 수 있다. 이 함수가 그것을 확인하는 자리다.
 *
 * noirq 단계에서 하는 이유가 중요하다. 이 단계에서는 인터럽트가 아직 켜지지
 * 않아, 여기서 이벤트를 지워도 새 이벤트가 끼어들지 않는다. 인터럽트가 켜진
 * 뒤에 지우면 그 사이에 온 진짜 이벤트까지 지울 수 있다.
 *
 * 명령 타임스탬프를 지금으로 되돌리는 것이 첫 일이다. 자는 동안 시간이
 * 흘렀으므로, 그것을 갱신하지 않으면 컨트롤러가 이전 명령이 시간 초과된
 * 것으로 오판한다.
 *
 * 카드가 바뀌었으면 두 가지를 한다. 먼저 그 아래 장치를 모두 "연결 끊김" 으로
 * 표시하는데, 이미 없는 장치에 접근하려는 드라이버들을 막기 위해서다.
 * 그 다음 PDC 요청을 내 실제 처리를 맡긴다.
 *
 * 켜져 있던 슬롯에서만 확인한다. 꺼져 있던 슬롯은 절전 전에도 카드가 없었거나
 * 쓰이지 않던 상태라, 바뀌었더라도 인터럽트로 처리하면 된다.
 *
 * 실행 컨텍스트: 절전 복귀의 noirq 단계. 인터럽트가 꺼진 상태이며
 * 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 드라이버의 복귀 → [이 함수]
 *     → pcie_clear_hotplug_events() → pciehp_device_replaced()
 *     → pci_walk_bus(pci_dev_set_disconnected) → pciehp_request()
 */
static int pciehp_resume_noirq(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies;
	/* [한국어] 명령이 진행 중이라고 표시한다. 위 상류 주석이 그 근거를 밝힌다 —
	 * pci_restore_state() 가 방금 Slot Control 레지스터에 썼으므로,
	 * 컨트롤러 입장에서는 명령을 하나 낸 것과 같은 상태다. */
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
			/* [한국어] 그 아래 장치를 모두 '연결 끊김' 으로 표시한다. 이미 없는 장치에
			 * 접근하려는 드라이버들을 막는 것으로, 여기서 막지 않으면 그 드라이버들이
			 * 1 로 채워진 응답을 정상 값으로 오인한다. */
			pci_walk_bus(ctrl->pcie->port->subordinate,
				     pci_dev_set_disconnected, NULL);
			pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		/* [한국어] 교체 확인 끝. 실제 처리는 요청을 받은 IRQ 스레드가 한다. */
		}
	}

	return 0;
}
#endif

/* [한국어]
 * pciehp_resume - 인터럽트를 다시 켜고 현재 상태를 확인한다
 *
 * @dev: 포트 서비스 장치.
 * @return: 언제나 0.
 *
 * pciehp_suspend() 의 짝이다.
 *
 * 인터럽트를 켜는 조건이 끌 때와 같다 — 커널이 PME 를 다루는 경우에만
 * 건드린다. 펌웨어가 다루는 시스템에서는 애초에 끄지 않았으므로 켤 것도 없다.
 *
 * 인터럽트를 켠 **뒤** 에 존재를 확인하는 순서가 요점이다. 반대로 하면
 * 확인과 인터럽트 활성화 사이에 온 변화를 양쪽 다 놓친다.
 *
 * 이 함수는 시스템 복귀와 런타임 복귀 양쪽에서 쓰인다 —
 * pciehp_runtime_resume() 이 자기 일을 마친 뒤 이것을 부른다.
 *
 * 실행 컨텍스트: 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 드라이버의 복귀 / pciehp_runtime_resume() → [이 함수]
 *     → pme_is_native() → pcie_enable_interrupt() → pciehp_check_presence()
 */
static int pciehp_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	if (pme_is_native(dev))
		/* [한국어] 커널이 PME 를 다루는 경우에만 인터럽트를 다시 켠다. 끌 때와 같은 조건이라
		 * 펌웨어가 다루는 시스템에서는 애초에 끄지 않았으므로 켤 것도 없다. */
		pcie_enable_interrupt(ctrl);

	pciehp_check_presence(ctrl);

	return 0;
}

/* [한국어]
 * pciehp_runtime_suspend - 런타임 절전 진입 시 인터럽트를 끈다
 *
 * @dev: 포트 서비스 장치.
 * @return: 언제나 0.
 *
 * pciehp_suspend() 에서 dev_pm_skip_suspend() 검사만 뺀 것이다. 런타임 절전은
 * 그 검사가 뜻을 갖지 않는다 — 그 검사 자체가 "런타임 절전 상태로 시스템
 * 절전에 들어가는가" 를 묻는 것이기 때문이다.
 *
 * CONFIG_PM_SLEEP 이 아니라 CONFIG_PM 안에 있다. 런타임 절전만 쓰는 구성에서도
 * 필요하기 때문이다.
 *
 * 실행 컨텍스트: 런타임 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 드라이버의 런타임 절전 → [이 함수] → pciehp_disable_interrupt()
 */
static int pciehp_runtime_suspend(struct pcie_device *dev)
{
	pciehp_disable_interrupt(dev);
	return 0;
}

/* [한국어]
 * pciehp_runtime_resume - 런타임 복귀 시 낡은 이벤트를 지우고 정상 복귀를 이어 간다
 *
 * @dev: 포트 서비스 장치.
 * @return: pciehp_resume() 의 결과.
 *
 * 시스템 복귀와 다른 점이 하나다 — 런타임 절전에는 noirq 단계가 없어,
 * pciehp_resume_noirq() 가 하던 이벤트 정리를 여기서 겸한다.
 *
 * 명령 타임스탬프를 되돌리는 것도 같은 이유다.
 *
 * 이벤트를 지우는 조건에 pme_is_native() 가 더 붙는다. 펌웨어가 PME 를 다루는
 * 시스템에서는 인터럽트를 끄지 않았으므로 이벤트가 정상적으로 처리됐고,
 * 지울 낡은 이벤트가 없기 때문이다.
 *
 * 카드 교체 확인은 하지 않는다. 런타임 절전은 짧고 사용자가 그동안 카드를
 * 바꿀 가능성이 낮다고 보는 것이며, 바뀌었더라도 아래 pciehp_resume() 의
 * 존재 확인이 잡아낸다.
 *
 * 정리를 마친 뒤 pciehp_resume() 에 나머지를 맡긴다.
 *
 * 실행 컨텍스트: 런타임 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: pciehp_resume() 의 결과를 그대로 돌려준다.
 *
 * 호출 체인:
 *   포트 드라이버의 런타임 복귀 → [이 함수]
 *     → pme_is_native() → pcie_clear_hotplug_events() → pciehp_resume()
 */
static int pciehp_runtime_resume(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	/* pci_restore_state() just wrote to the Slot Control register */
	ctrl->cmd_started = jiffies;
	/* [한국어] 런타임 복귀에서도 같은 표시가 필요하다. 이유는 시스템 복귀 쪽과 같다. */
	ctrl->cmd_busy = true;

	/* clear spurious events from rediscovery of inserted card */
	if ((ctrl->state == ON_STATE || ctrl->state == BLINKINGOFF_STATE) &&
	     pme_is_native(dev))
		pcie_clear_hotplug_events(ctrl);

	return pciehp_resume(dev);
/* [한국어] 런타임 복귀 마무리. 나머지는 시스템 복귀와 같은 경로가 처리한다. */
}
#endif

static struct pcie_port_service_driver hpdriver_portdrv = {
	/* [한국어] sysfs 와 로그에 나오는 서비스 이름. */
	.name		= "pciehp",
	/* [한국어] 포트 종류를 가리지 않는다 — 루트 포트든 스위치 다운스트림 포트든
	 * 핫플러그 능력이 있으면 붙는다. */
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_HP,

	.probe		= pciehp_probe,
	/* [한국어] 서비스를 뗄 때 불린다. */
	.remove		= pciehp_remove,

#ifdef	CONFIG_PM
#ifdef	CONFIG_PM_SLEEP
	.suspend	= pciehp_suspend,
	/* [한국어] 복귀는 두 단계다. noirq 단계에서 자는 동안의 변화를 확인하고, */
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

/* [한국어]
 * pcie_hp_init - pciehp 를 포트 서비스 드라이버로 등록한다
 *
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 이 모듈의 초기화 진입점이다. 하는 일은 등록 하나뿐이며, 실제 작업은
 * 등록된 뒤 포트마다 불리는 pciehp_probe() 가 한다.
 *
 * 포트 서비스 드라이버라는 구조가 이 파일의 전제다. PCIe 포트 하나가
 * 핫플러그·AER·PME·대역폭 알림 같은 여러 서비스를 함께 가질 수 있어,
 * 포트 드라이버가 그것을 나눠 각 서비스 드라이버에 맡긴다.
 *
 * 실패해도 디버그 기록만 남기고 그 값을 그대로 돌려준다. 등록 실패는
 * 드물고, 실패하면 핫플러그가 동작하지 않을 뿐 시스템은 계속 돈다.
 *
 * 실행 컨텍스트: 부팅 중 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 등록 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_hotplug 초기화 → [이 함수] → pcie_port_service_register()
 */
int __init pcie_hp_init(void)
{
	int retval = 0;

	retval = pcie_port_service_register(&hpdriver_portdrv);
	/* [한국어] 등록 결과를 디버그 기록에 남긴다. */
	pr_debug("pcie_port_service_register = %d\n", retval);
	/* [한국어] 실패했으면, */
	if (retval)
		/* [한국어] 그 사실도 남긴다. 기록만 남기는 이유는 핫플러그가 없어도 시스템은
		 * 계속 돌기 때문이다. */
		pr_debug("Failure to register service\n");

	return retval;
/* [한국어] 등록 결과를 그대로 돌려준다. */
}
