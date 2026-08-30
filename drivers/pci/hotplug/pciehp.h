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
 * NVMe 관점 요약:
 * 이 헤더는 PCIe 핫플러그 컨트롤러 드라이버(pciehp)의 남은 핫플러그 슬롯을 다룬다.
 * NVMe SSD가 PCIe 슬롯에 장착된 경우, 이 드라이버가 장치의 물리적 삽입/제거,
 * 전원 켜기/끄기, Attention Button 처리, Presence Detect 변경, Link Active 감시,
 * 그리고 PCI 핫플러그 코어로의 등록을 담당한다.
 *
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c)가 보는 관점에서 pciehp는
 * NVMe 장치가 연결된 다운스트림 포트의 상태 변화를 처리하는 하위 레이어다.
 * NVMe 드라이버는 PCI 버스 탐색 중 이 슬롯 아래에 노출된 nvmeX 네임스페이스를
 * 생성하며, 슬롯이 꺼지거나 장치가 제거되면 pciehp_unconfigure_device()를 통해
 * 하위 PCI 디바이스가 제거되고 NVMe 드라이버의 remove 콜백이 호출된다.
 *
 * 주요 NVMe 관련 호출 경로:
 * 1. PCIe 포트 서비스 드라이버(probe) -> pcie_init() -> controller 생성
 * 2. IRQ/폴 스레드 -> pciehp_handle_presence_or_link_change()
 *    -> pciehp_power_on_slot() -> pciehp_configure_device() -> pci_bus_add_devices()
 *       -> NVMe probe(nvme_probe)
 * 3. Attention Button / sysfs disable -> pciehp_handle_disable_request()
 *    -> pciehp_unconfigure_device() -> pci_stop_and_remove_bus_device()
 *       -> NVMe remove(nvme_remove)
 * 4. Link down / surprise removal -> 같은 unconfigure 경로로 NVMe 제거
 * 5. Slot reset -> pciehp_reset_slot() -> pciehp_slot_reset()
 *    -> PCI bus reset -> NVMe controller reset 및 재초기화
 */

#ifndef _PCIEHP_H
#define _PCIEHP_H				/* NVMe: pciehp.h 중복 include 방지 */

#include <linux/types.h>			/* NVMe: u8/u16/u32/u64 등 고정폭 타입 정의 */
#include <linux/pci.h>				/* NVMe: PCI 장치/버스 구조체, cfg access, capability 등 */
#include <linux/pci_hotplug.h>			/* NVMe: struct hotplug_slot, 핫플러그 코어 인터페이스 */
#include <linux/delay.h>			/* NVMe: 슬롯 전환 시 msleep()/udelay() 지원 */
#include <linux/mutex.h>			/* NVMe: Slot Control 레지스터 동기화용 뮤텍스 */
#include <linux/rwsem.h>			/* NVMe: reset_lock, reset 동안 Presence/Link 읽기 보호 */
#include <linux/workqueue.h>			/* NVMe: Attention Button 5초 지연 workqueue */

#include "../pcie/portdrv.h"			/* NVMe: struct pcie_device, PCIe 포트 서비스 드라이버 헤더 */

extern bool pciehp_poll_mode;			/* NVMe: IRQ 대신 폴링 모드 사용 여부 모듈 파라미터 */
extern int pciehp_poll_time;			/* NVMe: 폴링 주기(ms), IRQ 없는 플랫폼용 */

/*
 * Set CONFIG_DYNAMIC_DEBUG=y and boot with 'dyndbg="file pciehp* +p"' to
 * enable debug messages.
 */
#define ctrl_dbg(ctrl, format, arg...)				\
	pci_dbg(ctrl->pcie->port, format, ## arg)		/* NVMe: pciehp 디버그 메시지를 NVMe가 탑재된 포트로 출력 */
#define ctrl_err(ctrl, format, arg...)				\
	pci_err(ctrl->pcie->port, format, ## arg)		/* NVMe: pciehp 에러 메시지를 NVMe가 탑재된 포트로 출력 */
#define ctrl_info(ctrl, format, arg...)				\
	pci_info(ctrl->pcie->port, format, ## arg)		/* NVMe: pciehp 정보 메시지를 NVMe가 탑재된 포트로 출력 */
#define ctrl_warn(ctrl, format, arg...)				\
	pci_warn(ctrl->pcie->port, format, ## arg)		/* NVMe: pciehp 경고 메시지를 NVMe가 탑재된 포트로 출력 */

#define SLOT_NAME_SIZE 10			/* NVMe: 슬롯 이름 버퍼 크기, 예: "1-1" */

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
 * @queue: wait queue to wake queue on reception of a Command Completed event,
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
 * @requester: wait queue to wake queue on completion of user request,
 *	used for synchronous slot enable/disable request via sysfs
 *
 * PCIe hotplug has a 1:1 relationship between controller and slot, hence
 * unlike other drivers, the two aren't represented by separate structures.
 */
struct controller {
	struct pcie_device *pcie;		/* NVMe: 이 슬롯을 관리하는 PCIe 포트 서비스 디바이스 포인터 */
	u64 dsn;				/* NVMe: 슬롯 내 Function 0의 Device Serial Number 캐시; sleep 중 NVMe 교체 여부 판별 */

	u32 slot_cap;				/* NVMe: Slot Capabilities 레지스터 캐시; Attention Button, Power Controller 등 능력 파악 */
	unsigned int inband_presence_disabled:1;	/* NVMe: In-Band Presence Detect Disable 지원 및 비활성화 여부 */

	u16 slot_ctrl;				/* NVMe: Slot Control 레지스터 캐시; Attention/PME/Link 등 인터럽트 마스크 포함 */
	struct mutex ctrl_lock;			/* NVMe: Slot Control 쓰기 직렬화, NVMe 장치 상태 변경 명령 충돌 방지 */
	unsigned long cmd_started;		/* NVMe: Slot Control 마지막 쓰기 시각(jiffies); 1초 최소 간격 규격 준수 */
	unsigned int cmd_busy:1;		/* NVMe: Slot Control 명령 진행 중 플래그, Command Completed IRQ로 클리어 */
	wait_queue_head_t queue;		/* NVMe: Command Completed 대기 큐, 동기적 Slot Control 쓰기용 */

	atomic_t pending_events;		/* NVMe: IRQ 핸들러가 읽은 Slot Status/사용자 요청 이벤트를 IRQ 스레드에 넘기는 버퍼 */
	unsigned int notification_enabled:1;	/* NVMe: MSI/INTx 등 인터럽트 등록 성공 여부 */
	unsigned int power_fault_detected;	/* NVMe: 하드웨어가 감지한 전원 결함이 사용자 해제 전까지 유지되는 플래그 */
	struct task_struct *poll_thread;	/* NVMe: IRQ 없는 환경에서 슬롯 이벤트를 폴링하는 커널 스레드 */

	u8 state;				/* NVMe: 슬롯 상태 머신 현재 상태, OFF/ON/POWERON 등 */
	struct mutex state_lock;		/* NVMe: @state 및 button_work 스케줄링/취소 보호 */
	struct delayed_work button_work;	/* NVMe: Attention Button 누름 후 5초 지연 슬롯 on/off work */

	struct hotplug_slot hotplug_slot;	/* NVMe: PCI 핫플러그 코어에 등록되는 슬롯 인터페이스; sysfs enable/disable 진입점 */
	struct rw_semaphore reset_lock;		/* NVMe: 슬롯 reset 중 Presence Detect/Link Active 비트 진동으로 인한 오판 방지 */
	unsigned int depth;			/* NVMe: 루트 버스까지 경로상 추가 핫플러그 포트 수, reset_lock lockdep subclass */
	unsigned int ist_running;		/* NVMe: IRQ 스레드 실행 중 사용자 요청 대기 플래그 */
	int request_result;			/* NVMe: 마지막 sysfs/버튼 요청의 처리 결과 */
	wait_queue_head_t requester;		/* NVMe: 사용자 요청 완료 대기 큐, sysfs 동기 enable/disable용 */
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
#define OFF_STATE			0	/* NVMe: 슬롯 전원 OFF, NVMe 미탐색 상태 */
#define BLINKINGON_STATE		1	/* NVMe: 5초 후 전원 ON 예약, Power LED 깜빡임 */
#define BLINKINGOFF_STATE		2	/* NVMe: 5초 후 전원 OFF 예약, Power LED 깜빡임 */
#define POWERON_STATE			3	/* NVMe: 슬롯 전원 ON 진행 중, NVMe 링크 트레이닝 대기 */
#define POWEROFF_STATE			4	/* NVMe: 슬롯 전원 OFF 진행 중, NVMe 제거 직전 */
#define ON_STATE			5	/* NVMe: 슬롯 전원 ON, NVMe 열거 완료 및 운영 중 */

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
#define DISABLE_SLOT		(1 << 16)	/* NVMe: sysfs/Attention Button로부터의 슬롯 disable 요청 플래그 */
#define RERUN_ISR		(1 << 17)	/* NVMe: 부모 포트가 런타임 슬립 중이어서 ISR을 IRQ 스레드가 재실행해야 함을 표시 */

#define ATTN_BUTTN(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_ABP)		/* NVMe: Attention Button Press 감지 기능 지원 여부, NVMe 수동 제거 버튼 */
#define POWER_CTRL(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_PCP)		/* NVMe: Power Control 지원 여부, NVMe 슬롯 전원 제어 가능 여부 */
#define MRL_SENS(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_MRLSP)	/* NVMe: MRL(Manual Retention Latch) 센서 지원 여부 */
#define ATTN_LED(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_AIP)		/* NVMe: Attention LED 제어 지원 여부 */
#define PWR_LED(ctrl)		((ctrl)->slot_cap & PCI_EXP_SLTCAP_PIP)		/* NVMe: Power LED 제어 지원 여부, NVMe 슬롯 상태 시각화 */
#define NO_CMD_CMPL(ctrl)	((ctrl)->slot_cap & PCI_EXP_SLTCAP_NCCS)		/* NVMe: Command Completed 지원 안 함, 폴리ング으로 완료 대기 */
#define PSN(ctrl)		(((ctrl)->slot_cap & PCI_EXP_SLTCAP_PSN) >> 19)	/* NVMe: Physical Slot Number 추출, /sys/bus/pci/slots/N 이름에 사용 */

void pciehp_request(struct controller *ctrl, int action);			/* NVMe: IRQ 핸들러/사용자 요청을 IRQ 스레드에 큐잉, NVMe 상태 변경 트리거 */
void pciehp_handle_button_press(struct controller *ctrl);			/* NVMe: Attention Button 누름 처리, 5초 지연 후 슬롯 on/off 결정 */
void pciehp_handle_disable_request(struct controller *ctrl);			/* NVMe: 슬롯 disable 요청 처리, NVMe 제거 및 전원 OFF */
void pciehp_handle_presence_or_link_change(struct controller *ctrl, u32 events);	/* NVMe: Presence Detect/Link Active 변경 처리, NVMe 삽입/제거/ surprise removal */
int pciehp_configure_device(struct controller *ctrl);				/* NVMe: 슬롯 아래 PCI 버스 탐색 및 NVMe probe 유발 */
void pciehp_unconfigure_device(struct controller *ctrl, bool presence);		/* NVMe: 슬롯 아래 PCI 장치 제거, NVMe remove 콜백 호출 */
void pciehp_queue_pushbutton_work(struct work_struct *work);			/* NVMe: Attention Button workqueue 핸들러, 상태 머신 전이 수행 */
struct controller *pcie_init(struct pcie_device *dev);				/* NVMe: pciehp probe, controller 및 hotplug_slot 할당/초기화 */
int pcie_init_notification(struct controller *ctrl);				/* NVMe: Slot Status 인터럽트 등록, NVMe 삽입/제거 이벤트 수신 */
void pcie_shutdown_notification(struct controller *ctrl);			/* NVMe: 인터럽트 해제 및 폴 스레드 중지, 모듈 제거/새로고침 시 */
void pcie_clear_hotplug_events(struct controller *ctrl);			/* NVMe: Slot Status 레지스터의 기존 핫플러그 이벤트 클리어 */
void pcie_enable_interrupt(struct controller *ctrl);				/* NVMe: Slot Control에서 핫플러그 인터럽트 언마스크 */
void pcie_disable_interrupt(struct controller *ctrl);				/* NVMe: Slot Control에서 핫플러그 인터럽트 마스크, NVMe 제거 중 사용 */
int pciehp_power_on_slot(struct controller *ctrl);				/* NVMe: 슬롯 전원 ON 및 PERST 해제, NVMe 링크 업 유도 */
void pciehp_power_off_slot(struct controller *ctrl);				/* NVMe: 슬롯 전원 OFF, NVMe 완전히 정지 */
void pciehp_get_power_status(struct controller *ctrl, u8 *status);		/* NVMe: 슬롯 전원 상태 읽기, sysfs power 상태 노출 */

#define INDICATOR_NOOP -1						/* NVMe: LED 상태를 변경하지 않음을 의미 */
void pciehp_set_indicators(struct controller *ctrl, int pwr, int attn);		/* NVMe: Power/Attention LED 상태 설정, NVMe 슬롯 시각적 상태 표시 */

void pciehp_get_latch_status(struct controller *ctrl, u8 *status);		/* NVMe: MRL latch 상태 읽기, NVMe 물리적 고정 여부 */
int pciehp_query_power_fault(struct controller *ctrl);				/* NVMe: 전원 결함 상태 질의, NVMe 과전류/결함 보호 */
int pciehp_card_present(struct controller *ctrl);				/* NVMe: Presence Detect로 NVMe 물리 장착 여부 확인 */
int pciehp_card_present_or_link_active(struct controller *ctrl);			/* NVMe: Presence Detect 또는 Link Active로 NVMe 존재/동작 여부 확인 */
int pciehp_check_link_status(struct controller *ctrl);				/* NVMe: Link Training 완료 및 상태 확인, NVMe PCIe 링크 품질 점검 */
int pciehp_check_link_active(struct controller *ctrl);				/* NVMe: Data Link Layer Link Active 비트로 NVMe 링크 활성 확인 */
bool pciehp_device_replaced(struct controller *ctrl);				/* NVMe: sleep 전후 DSN 비교로 NVMe 교체 여부 확인 */
void pciehp_release_ctrl(struct controller *ctrl);				/* NVMe: controller 및 hotplug_slot 자원 해제 */

int pciehp_sysfs_enable_slot(struct hotplug_slot *hotplug_slot);		/* NVMe: /sys/bus/pci/slots/N/power 쓰기 시 슬롯 enable 처리 */
int pciehp_sysfs_disable_slot(struct hotplug_slot *hotplug_slot);		/* NVMe: /sys/bus/pci/slots/N/power 쓰기 시 슬롯 disable 처리 */
int pciehp_reset_slot(struct hotplug_slot *hotplug_slot, bool probe);		/* NVMe: PCI 핫플러그 코어의 reset_slot 콜백, NVMe bus reset 수행 */
int pciehp_get_attention_status(struct hotplug_slot *hotplug_slot, u8 *status);	/* NVMe: Attention LED 상태 sysfs 노출 */
int pciehp_set_raw_indicator_status(struct hotplug_slot *h_slot, u8 status);	/* NVMe: 원시 LED 상태 설정, 테스트/디버그용 */
int pciehp_get_raw_indicator_status(struct hotplug_slot *h_slot, u8 *status);	/* NVMe: 원시 LED 상태 읽기, 테스트/디버그용 */

int pciehp_slot_reset(struct pcie_device *dev);					/* NVMe: PCIe 포트 서비스 레벨 슬롯 reset 진입점, NVMe FLR/복구 유발 가능 */

static inline const char *slot_name(struct controller *ctrl)
{
	return hotplug_slot_name(&ctrl->hotplug_slot);				/* NVMe: 이 슬롯의 sysfs 이름 반환, NVMe 로그/이벤트 식별에 사용 */
}

static inline struct controller *to_ctrl(struct hotplug_slot *hotplug_slot)
{
	return container_of(hotplug_slot, struct controller, hotplug_slot);	/* NVMe: hotplug_slot로부터 embedding된 controller 구조체 역참조 */
}

#endif									/* NVMe: pciehp.h 헤더 끝, NVMe PCIe 핫플러그 관련 선언 종료 */
