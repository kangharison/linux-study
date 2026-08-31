/* SPDX-License-Identifier: GPL-2.0+ */
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
 */

/*
 * [한국어 설명] PCIe 네이티브 핫플러그 드라이버의 내부 자료구조 (pciehp.h)
 *
 * === 파일의 역할 ===
 * pciehp 드라이버 네 파일(core / ctrl / hpc / pci)이 공유하는 자료구조와
 * 상수를 정의한다. 중심은 struct controller — 슬롯 하나를 관리하는 모든
 * 상태가 여기 모여 있다.
 *
 * 이 헤더를 읽을 때 핵심은 상태 기계와 이벤트의 구분이다.
 *   이벤트 비트(PCI_EXP_SLTSTA_*) — 하드웨어가 알려 주는 사실.
 *     Presence Detect Changed(카드가 꽂히거나 빠짐),
 *     Data Link Layer State Changed(링크가 올라오거나 내려감),
 *     Attention Button Pressed, Power Fault 등.
 *   슬롯 상태(OFF_STATE / BLINKINGON_STATE / ON_STATE / ...) — 드라이버가
 *     기억하는 논리 상태. 같은 이벤트라도 현재 상태에 따라 다르게 반응한다.
 *
 * 이 둘을 나눈 이유는 사람의 개입 때문이다. Attention 버튼을 누르면
 * 5초 동안 LED 가 깜빡이며(BLINKINGON/BLINKINGOFF) 취소할 기회를 준다.
 * 그 사이에 다시 누르면 취소된다. 하드웨어 이벤트 하나에 대해 상태가
 * 여러 갈래로 갈리는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp_core.c : 포트 서비스로 등록되고 슬롯을 hotplug 코어에 노출
 * pciehp_hpc.c  : 하드웨어 접근 — Slot Control/Status 레지스터, 인터럽트
 * pciehp_ctrl.c : 상태 기계 — 이벤트를 받아 무엇을 할지 정한다
 * pciehp_pci.c  : 실제 열거/제거 — pci_scan_slot, pci_stop_and_remove
 * 이 헤더가 그 넷을 잇는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c 가 HP 서비스로 pciehp_core.c 를 바인딩한다.
 * 아래쪽: slot.c 의 struct pci_slot, hotplug/pci_hotplug_core.c 의
 *   struct hotplug_slot, 그리고 probe.c / remove.c 의 열거·제거 함수.
 * 공유 상태: struct controller 하나가 슬롯 하나에 대응한다.
 *
 * === NVMe 관점 ===
 * pciehp 는 U.2 / EDSFF 백플레인의 NVMe 핫스왑을 실제로 수행하는 드라이버다.
 * 드라이브를 꽂으면 Presence Detect Changed 인터럽트가 나고, 이 드라이버가
 * 슬롯 전원을 켜고 링크를 기다린 뒤 열거해 nvme_probe() 가 불린다.
 * 뽑으면 반대 경로로 nvme_remove() 가 불린다.
 *
 * 다만 NVMe 드라이버가 이 헤더를 include 하거나 여기 정의된 함수를
 * 부르지는 않는다(전수 확인). 관계는 전적으로 한 방향이다 —
 * pciehp 가 열거·제거를 일으키고 NVMe 는 그 결과로 붙거나 떨어진다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct controller     : 슬롯 하나의 모든 상태. capability 오프셋, 인터럽트,
 *                         현재 상태, 대기열, 그리고 여러 뮤텍스가 들어 있다.
 * 슬롯 상태 상수         : OFF_STATE / BLINKINGON_STATE / BLINKINGOFF_STATE /
 *                         POWERON_STATE / POWEROFF_STATE / ON_STATE.
 * pciehp_handle_*()     : ctrl.c 의 이벤트 처리 함수들.
 * pciehp_power_on_slot() / _power_off_slot() : hpc.c 의 하드웨어 조작.
 * pciehp_configure_device() / _unconfigure_device() : pci.c 의 열거·제거.
 * pciehp_query_power_fault() / pciehp_get_*_status() : 상태 조회.
 */

#ifndef _PCIEHP_H
#define _PCIEHP_H

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>

#include "../pcie/portdrv.h"

extern bool pciehp_poll_mode;
extern int pciehp_poll_time;

/*
 * Set CONFIG_DYNAMIC_DEBUG=y and boot with 'dyndbg="file pciehp* +p"' to
 * enable debug messages.
 */
#define ctrl_dbg(ctrl, format, arg...)				\
	pci_dbg(ctrl->pcie->port, format, ## arg)
#define ctrl_err(ctrl, format, arg...)				\
	pci_err(ctrl->pcie->port, format, ## arg)
#define ctrl_info(ctrl, format, arg...)				\
	pci_info(ctrl->pcie->port, format, ## arg)
#define ctrl_warn(ctrl, format, arg...)				\
	pci_warn(ctrl->pcie->port, format, ## arg)

#define SLOT_NAME_SIZE 10

/**
 * struct controller - PCIe hotplug controller
 * @pcie: pointer to the controller's PCIe port service device
 * @dsn: cached copy of Device Serial Number of Function 0 in the hotplug slot
 *	(PCIe r6.2 sec 7.9.3); used to determine whether a hotplugged device
 *	was replaced with a different one during system sleep
 * @slot_cap: cached copy of the Slot Capabilities register
 * @inband_presence_disabled: In-Band Presence Detect Disable supported by
 *	controller and disabled per spec recommendation (PCIe r5.0, appendix I
 *	implementation note)
 * @slot_ctrl: cached copy of the Slot Control register
 * @ctrl_lock: serializes writes to the Slot Control register
 * @cmd_started: jiffies when the Slot Control register was last written;
 *	the next write is allowed 1 second later, absent a Command Completed
 *	interrupt (PCIe r4.0, sec 6.7.3.2)
 * @cmd_busy: flag set on Slot Control register write, cleared by IRQ handler
 *	on reception of a Command Completed event
 * @queue: wait queue to wake up on reception of a Command Completed event,
 *	used for synchronous writes to the Slot Control register
 * @pending_events: used by the IRQ handler to save events retrieved from the
 *	Slot Status register for later consumption by the IRQ thread
 * @notification_enabled: whether the IRQ was requested successfully
 * @power_fault_detected: whether a power fault was detected by the hardware
 *	that has not yet been cleared by the user
 * @poll_thread: thread to poll for slot events if no IRQ is available,
 *	enabled with pciehp_poll_mode module parameter
 * @state: current state machine position
 * @state_lock: protects reads and writes of @state;
 *	protects scheduling, execution and cancellation of @button_work
 * @button_work: work item to turn the slot on or off after 5 seconds
 *	in response to an Attention Button press
 * @hotplug_slot: structure registered with the PCI hotplug core
 * @reset_lock: prevents access to the Data Link Layer Link Active bit in the
 *	Link Status register and to the Presence Detect State bit in the Slot
 *	Status register during a slot reset which may cause them to flap
 * @depth: Number of additional hotplug ports in the path to the root bus,
 *	used as lock subclass for @reset_lock
 * @ist_running: flag to keep user request waiting while IRQ thread is running
 * @request_result: result of last user request submitted to the IRQ thread
 * @requester: wait queue to wake up on completion of user request,
 *	used for synchronous slot enable/disable request via sysfs
 *
 * PCIe hotplug has a 1:1 relationship between controller and slot, hence
 * unlike other drivers, the two aren't represented by separate structures.
 */
struct controller {
	struct pcie_device *pcie;
	u64 dsn;

	u32 slot_cap;				/* capabilities and quirks */
	/* [한국어] Slot Capabilities 레지스터의 사본.
	 * 이 슬롯이 무엇을 할 수 있는지가 여기 다 들어 있다 — Attention Button 이
	 * 있는가, Power Controller 가 있는가, Presence Detect 를 지원하는가,
	 * MRL(Manually Operated Retention Latch) 센서가 있는가.
	 * 설정자: pcie_init() [hpc.c] 이 probe 때 한 번 읽어 둔다. 이후 불변.
	 * 읽는 자: 거의 모든 곳. 없는 기능을 조작하려 들면 안 되므로 매번 확인한다.
	 * 값 범위: PCI_EXP_SLTCAP_* 비트 조합. 상류 주석의 quirks 는 일부 하드웨어의
	 *   잘못된 값을 pciehp_core.c 에서 보정해 넣기 때문에 붙은 말이다.
	 * 동기화: 읽기 전용이라 불필요. */
	unsigned int inband_presence_disabled:1;

	u16 slot_ctrl;				/* control register access */
	/* [한국어] Slot Control 레지스터에 마지막으로 쓴 값의 사본.
	 * 사본을 두는 이유가 둘이다. 하나는 레지스터를 읽으면 느리기 때문이고,
	 * 다른 하나는 이 레지스터가 여러 비트를 한꺼번에 담고 있어(전원, LED,
	 * 인터럽트 마스크) 한 비트만 바꾸려면 나머지를 알고 있어야 하기 때문이다.
	 * 설정자: pcie_write_cmd() [hpc.c] 가 쓸 때마다 갱신한다.
	 * 읽는 자: 같은 함수가 read-modify-write 의 기준값으로 쓴다.
	 * 값 범위: PCI_EXP_SLTCTL_* 비트 조합.
	 * 동기화: 바로 아래 ctrl_lock 이 이 필드와 실제 레지스터 쓰기를 함께 보호한다. */
	struct mutex ctrl_lock;
	unsigned long cmd_started;
	unsigned int cmd_busy:1;
	wait_queue_head_t queue;

	atomic_t pending_events;		/* event handling */
	/* [한국어] 하드 IRQ 핸들러가 읽은 이벤트를 IRQ 스레드에 넘기는 자리.
	 * 왜 넘겨야 하는가. Slot Status 레지스터의 이벤트 비트는 읽고 나서
	 * write-1-to-clear 로 지워야 다음 인터럽트가 온다. 그 작업은 하드 IRQ 에서
	 * 즉시 해야 하지만, 그 이벤트에 대응하는 일(열거, 제거)은 잠들 수 있어
	 * 스레드로 미뤄야 한다. 그래서 지운 이벤트를 여기 쌓아 둔다.
	 * 설정자: pciehp_isr() 가 atomic_or 로 비트를 더한다. 하드 IRQ 컨텍스트.
	 * 읽는 자: pciehp_ist() 가 atomic_xchg 로 통째로 가져가며 비운다.
	 * 값 범위: PCI_EXP_SLTSTA_* 이벤트 비트 + DISABLE_SLOT 같은 소프트웨어 요청.
	 * 동기화: atomic_t 인 이유가 이것이다 — 하드 IRQ 와 스레드가 동시에 건드리는데
	 *   락을 쓰면 하드 IRQ 쪽이 잠길 수 없으므로 원자 연산으로 처리한다. */
	unsigned int notification_enabled:1;
	unsigned int power_fault_detected;
	struct task_struct *poll_thread;

	u8 state;				/* state machine */
	/* [한국어] 이 슬롯의 논리 상태. 파일 상단에서 설명한 여섯 상태 중 하나다.
	 * 하드웨어에서 읽는 값이 아니라 드라이버가 기억하는 값이라는 점이 중요하다.
	 * 같은 이벤트라도 이 값에 따라 다르게 반응해야 하기 때문에 존재한다.
	 * 설정자: pciehp_ctrl.c 의 상태 기계 함수들.
	 * 읽는 자: 같은 함수들, 그리고 sysfs 로 상태를 보여 줄 때.
	 * 값 범위: OFF_STATE, BLINKINGON_STATE, BLINKINGOFF_STATE,
	 *   POWERON_STATE, POWEROFF_STATE, ON_STATE.
	 * 동기화: 바로 아래 state_lock 이 보호한다. IRQ 스레드와 sysfs 를 통한
	 *   사용자 요청이 동시에 이 값을 바꾸려 할 수 있다. */
	struct mutex state_lock;
	struct delayed_work button_work;

	struct hotplug_slot hotplug_slot;	/* hotplug core interface */
	/* [한국어] 핫플러그 코어(pci_hotplug_core.c)에 등록하는 슬롯 표현.
	 * 포인터가 아니라 통째로 박아 넣은 이유는, PCIe 핫플러그에서는 컨트롤러
	 * 하나가 슬롯 하나에 정확히 대응하기 때문이다(상류 주석 마지막 문단).
	 * 다른 핫플러그 방식은 컨트롤러 하나가 여러 슬롯을 갖기도 해서 둘을
	 * 분리하지만, 여기서는 그럴 필요가 없어 container_of 로 오갈 수 있게 했다.
	 * 설정자: pciehp_probe() 가 ops 를 채워 pci_hp_initialize() 로 등록.
	 * 읽는 자: 핫플러그 코어가 sysfs 접근을 이 구조체의 ops 로 전달한다.
	 * 값 범위: ops 가 pciehp_slot_ops 를 가리킨다.
	 * 동기화: 코어가 자체 잠금으로 등록·해제를 보호한다. */
	struct rw_semaphore reset_lock;
	unsigned int depth;
	unsigned int ist_running;
	int request_result;
	wait_queue_head_t requester;
};

/**
 * DOC: Slot state
 *
 * @OFF_STATE: slot is powered off, no subordinate devices are enumerated
 * @BLINKINGON_STATE: slot will be powered on after the 5 second delay,
 *	Power Indicator is blinking
 * @BLINKINGOFF_STATE: slot will be powered off after the 5 second delay,
 *	Power Indicator is blinking
 * @POWERON_STATE: slot is currently powering on
 * @POWEROFF_STATE: slot is currently powering off
 * @ON_STATE: slot is powered on, subordinate devices have been enumerated
 */
#define OFF_STATE			0
#define BLINKINGON_STATE		1
#define BLINKINGOFF_STATE		2
#define POWERON_STATE			3
#define POWEROFF_STATE			4
#define ON_STATE			5

/**
 * DOC: Flags to request an action from the IRQ thread
 *
 * These are stored together with events read from the Slot Status register,
 * hence must be greater than its 16-bit width.
 *
 * %DISABLE_SLOT: Disable the slot in response to a user request via sysfs or
 *	an Attention Button press after the 5 second delay
 * %RERUN_ISR: Used by the IRQ handler to inform the IRQ thread that the
 *	hotplug port was inaccessible when the interrupt occurred, requiring
 *	that the IRQ handler is rerun by the IRQ thread after it has made the
 *	hotplug port accessible by runtime resuming its parents to D0
 */
#define DISABLE_SLOT		(1 << 16)
#define RERUN_ISR		(1 << 17)

#define ATTN_BUTTN(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_ABP)
#define POWER_CTRL(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_PCP)
#define MRL_SENS(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_MRLSP)
#define ATTN_LED(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_AIP)
#define PWR_LED(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_PIP)
#define NO_CMD_CMPL(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_NCCS)
#define PSN(ctrl)		(((ctrl)->slot_cap & PCI_EXP_SLTCAP_PSN) >> 19)

void pciehp_request(struct controller *ctrl, int action);
void pciehp_handle_button_press(struct controller *ctrl);
void pciehp_handle_disable_request(struct controller *ctrl);
void pciehp_handle_presence_or_link_change(struct controller *ctrl, u32 events);
int pciehp_configure_device(struct controller *ctrl);
void pciehp_unconfigure_device(struct controller *ctrl, bool presence);
void pciehp_queue_pushbutton_work(struct work_struct *work);
struct controller *pcie_init(struct pcie_device *dev);
int pcie_init_notification(struct controller *ctrl);
void pcie_shutdown_notification(struct controller *ctrl);
void pcie_clear_hotplug_events(struct controller *ctrl);
void pcie_enable_interrupt(struct controller *ctrl);
void pcie_disable_interrupt(struct controller *ctrl);
int pciehp_power_on_slot(struct controller *ctrl);
void pciehp_power_off_slot(struct controller *ctrl);
void pciehp_get_power_status(struct controller *ctrl, u8 *status);

#define INDICATOR_NOOP -1	/* Leave indicator unchanged */
/* [한국어] pciehp_set_indicators() 는 전원 LED 와 Attention LED 를 한 번의
 * 레지스터 쓰기로 함께 설정한다. 하나만 바꾸고 싶을 때 나머지 인자에
 * 이 값을 넘기면 그 LED 는 건드리지 않는다.
 * 굳이 이런 장치를 둔 이유는 Slot Control 레지스터 쓰기가 비싸기 때문이다.
 * 쓴 뒤 Command Completed 를 기다려야 하고, 그것이 없는 하드웨어에서는
 * 1초를 그냥 기다린다(위 cmd_started 주석 참고). 두 LED 를 따로 쓰면
 * 그 대기를 두 번 하게 되므로 한 번에 처리한다.
 * -1 을 쓰는 이유는 유효한 LED 값(PCI_EXP_SLTCTL_*_IND)이 모두 양수라
 * 겹치지 않기 때문이다. */
void pciehp_set_indicators(struct controller *ctrl, int pwr, int attn);

void pciehp_get_latch_status(struct controller *ctrl, u8 *status);
int pciehp_query_power_fault(struct controller *ctrl);
int pciehp_card_present(struct controller *ctrl);
int pciehp_card_present_or_link_active(struct controller *ctrl);
int pciehp_check_link_status(struct controller *ctrl);
int pciehp_check_link_active(struct controller *ctrl);
bool pciehp_device_replaced(struct controller *ctrl);
void pciehp_release_ctrl(struct controller *ctrl);

int pciehp_sysfs_enable_slot(struct hotplug_slot *hotplug_slot);
int pciehp_sysfs_disable_slot(struct hotplug_slot *hotplug_slot);
int pciehp_reset_slot(struct hotplug_slot *hotplug_slot, bool probe);
int pciehp_get_attention_status(struct hotplug_slot *hotplug_slot, u8 *status);
int pciehp_set_raw_indicator_status(struct hotplug_slot *h_slot, u8 status);
int pciehp_get_raw_indicator_status(struct hotplug_slot *h_slot, u8 *status);

int pciehp_slot_reset(struct pcie_device *dev);

static inline const char *slot_name(struct controller *ctrl)
{
	return hotplug_slot_name(&ctrl->hotplug_slot);
}

static inline struct controller *to_ctrl(struct hotplug_slot *hotplug_slot)
{
	return container_of(hotplug_slot, struct controller, hotplug_slot);
}

#endif
