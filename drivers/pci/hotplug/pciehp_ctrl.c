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
 */

/*
 * [한국어 설명] 핫플러그 이벤트를 해석하는 상태 기계 (pciehp_ctrl.c)
 *
 * === 파일의 역할 ===
 * 하드웨어가 알려 준 이벤트를 받아 "무엇을 할지" 를 정한다. pciehp 네 파일
 * 중 판단을 담당하는 부분이며, 실제 하드웨어 조작은 hpc.c 에, 열거와
 * 제거는 pci.c 에 맡긴다.
 *
 * 상태 기계가 필요한 이유는 같은 이벤트가 상황에 따라 다른 뜻을 갖기
 * 때문이다. 예를 들어 Presence Detect Changed 는 "카드가 꽂혔다" 일 수도
 * "빠졌다" 일 수도 있고, 그 판단은 현재 슬롯 상태와 함께 봐야 한다.
 *
 * 상태는 여섯이다.
 *   OFF_STATE        — 슬롯 전원이 꺼져 있고 아무것도 없다.
 *   BLINKINGON_STATE — Attention 버튼이 눌려 "곧 켤 것" 을 LED 로 알리는 중.
 *                      5초 안에 다시 누르면 취소된다.
 *   BLINKINGOFF_STATE— 반대로 "곧 끌 것" 을 알리는 중.
 *   POWERON_STATE    — 전원을 넣고 링크와 열거를 진행하는 중.
 *   POWEROFF_STATE   — 제거 절차를 진행하는 중.
 *   ON_STATE         — 정상 동작 중.
 *
 * 중간 상태(POWERON/POWEROFF)가 있는 이유는 그 작업이 오래 걸리기
 * 때문이다. 그 사이에 들어온 새 이벤트는 대개 무시하거나 미뤄야 한다 —
 * 열거하는 도중에 또 열거를 시작하면 안 되기 때문이다.
 *
 * 사람이 버튼을 누르는 흐름과 소프트웨어가 sysfs 로 요청하는 흐름이
 * 같은 상태 기계로 모인다는 점도 이 파일의 설계다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 하드웨어 이벤트 -> pciehp_hpc.c 의 인터럽트 핸들러
 *   -> pciehp_request() 로 이벤트를 큐에 넣고 스레드를 깨운다
 *      -> [이 파일] pciehp_handle_*() 로 상태 기계 진입
 *         -> pciehp_enable_slot() / pciehp_disable_slot()
 *            -> hpc.c 로 전원과 LED 조작
 *            -> pci.c 로 열거 또는 제거
 *
 * 실행 컨텍스트: 대부분 pciehp 의 IRQ 스레드. 열거와 제거가 오래 걸리고
 * 잠들 수 있어 하드 IRQ 에서 처리할 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pciehp_hpc.c 의 인터럽트 처리, pciehp_core.c 의 sysfs 콜백.
 * 아래쪽: pciehp_hpc.c 의 하드웨어 조작, pciehp_pci.c 의 열거·제거.
 * 공유 상태: struct controller 의 state 와 그것을 보호하는 state_lock,
 *   그리고 이벤트 대기열.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * U.2 백플레인에서 NVMe 드라이브를 뽑았을 때 실제로 일어나는 일이
 * 이 파일의 상태 전이다:
 *   Presence Detect Changed 인터럽트
 *     -> ON_STATE 에서 pciehp_handle_presence_or_link_change()
 *        -> 존재하지 않음을 확인 -> POWEROFF_STATE 로
 *           -> pciehp_disable_slot() -> pciehp_unconfigure_device()
 *              -> pci_stop_and_remove_bus_device() -> nvme_remove()
 *           -> 슬롯 전원 차단 -> OFF_STATE
 *
 * === 주요 함수/구조체 요약 ===
 * pciehp_request()            : 이벤트를 큐에 넣고 IRQ 스레드를 깨운다.
 * pciehp_handle_button_press(): Attention 버튼. 깜빡임 상태로 들어가거나,
 *                               이미 깜빡이는 중이면 취소한다.
 * pciehp_handle_disable_request() : sysfs 를 통한 명시적 끄기 요청.
 * pciehp_handle_presence_or_link_change() : 삽입/제거의 실제 판정.
 *                               Presence 와 Link 두 신호를 함께 본다.
 * pciehp_enable_slot()        : 전원을 넣고 링크를 기다린 뒤 열거한다.
 * pciehp_disable_slot()       : 제거하고 전원을 끈다.
 * __pciehp_enable_slot() / __pciehp_disable_slot() : 잠금 없는 내부 판.
 * pciehp_sync_bus_speed()     : 링크 속도를 다시 읽어 반영한다.
 */

#define dev_fmt(fmt) "pciehp: " fmt
/* NVMe: 커널 메시지 앞에 "pciehp: " 접두사를 붙여 로그를 구분한다. */

#include <linux/kernel.h>
/* NVMe: 커널 기본 자료형과 매크로 사용을 위해 포함. */
#include <linux/types.h>
/* NVMe: u8/u32 등 고정폭 타입 정의. */
#include <linux/pm_runtime.h>
/* NVMe: PCIe 포트의 런타임 전원 관리(get_sync/put)에 사용. */
#include <linux/pci.h>
/* NVMe: PCI/PCIe 구조체, 레지스터 비트, 함수 선언. */
#include <trace/events/pci.h>
/* NVMe: PCI 핫플러그 추적 이벤트(trace_pci_hp_event) 정의. */

#include "../pci.h"
/* NVMe: PCI 서브시스템 낸부 헤더(컨트롤러 구조체, 매크로 등). */
#include "pciehp.h"
/* NVMe: pciehp 전용 헤더(상태, 함수 프로토타입, 레지스터 비트). */

/* The following routines constitute the bulk of the
   hotplug controller logic
 */
/* NVMe: 이어지는 루틴들이 PCIe 핫플러그 컨트롤러 상태 머신의 핵심 로직이다. */

#define SAFE_REMOVAL	 true
/* NVMe: 안전 제거(safe removal): 정상적으로 장치를 비활성화한 뒤 슬롯 전원을 내림. */
#define SURPRISE_REMOVAL false
/* NVMe: 서프라이즈 제거(surprise removal): 링크 다운/갑작스러운 제거 시 긴급 처리. */

static void set_slot_off(struct controller *ctrl)
/* NVMe: 슬롯을 끄는 공통 루틴. NVMe 제거 후 전원과 인디케이터를 OFF 상태로 만든다. */
{
	/*
	 * Turn off slot, turn on attention indicator, turn off power
	 * indicator
	 */
	/* NVMe: 슬롯 전원을 끄고 Attention LED를 켜며 Power LED를 끈다. */
	if (POWER_CTRL(ctrl)) {
	/* NVMe: 이 포트/슬롯이 PCIe Slot Control의 Power Indicator Control과
	 *       Power Control 비트를 통해 전원을 제어할 수 있는 경우에만 수행.
	 *       NVMe가 장착된 슬롯이라면 전원 off가 실제로 NVMe에 공급되지 않음을 의미. */
		pciehp_power_off_slot(ctrl);
		/* NVMe: Slot Control 레지스터의 Power Control 비트를 0으로 설정해
		 *       슬롯 전원을 차단. NVMe 장치는 더 이상 ECAM/BAR 응답 불가. */

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		/* NVMe: 슬롯 전원이 실제로 낮아지고 NVMe 남아 있는 에너지가 소모될
		 *       시간을 확보. 1초 미만 대기 없이 다음 동작을 하면 하드웨어 상태가
		 *       불안정해질 수 있다. */
		msleep(1000);
		/* NVMe: 1000ms 동안 현재 컨텍스트를 슬립. 핫플러그 이벤트 핸들러 경로. */
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      PCI_EXP_SLTCTL_ATTN_IND_ON);
	/* NVMe: Power LED OFF, Attention LED ON.
	 *       NVMe 제거/오류 시 사용자에게 Attention 상태를 시각적으로 알림. */
}

/**
 * board_added - Called after a board has been added to the system.
 * @ctrl: PCIe hotplug controller where board is added
 *
 * Turns power on for the board.
 * Configures board.
 */
/* NVMe: 시스템에 카드(NVMe 등)가 삽입된 후 호출. 전원을 켜고 장치를 구성. */
static int board_added(struct controller *ctrl)
/* NVMe: NVMe SSD 삽입 시 슬롯 전원/링크/장치 구성을 수행하는 함수. */
{
	int retval = 0;
	/* NVMe: 반환값을 0(성공)으로 초기화. */
	struct pci_bus *parent = ctrl->pcie->port->subordinate;
	/* NVMe: 핫플러그 포트 아래 서브ordinate 버스를 가리킴.
	 *       NVMe 장치는 이 버스의 0번 디바이스/0번 함수에 탐지될 것이다. */

	if (POWER_CTRL(ctrl)) {
	/* NVMe: 슬롯 전원 제어가 지원되면 전원을 켠다. */
		/* Power on slot */
		/* NVMe: 슬롯에 전원을 공급. */
		retval = pciehp_power_on_slot(ctrl);
		/* NVMe: Slot Control의 Power Control 비트를 1로 설정해 슬롯 전원 ON.
		 *       NVMe가 깨어나기 위한 첫 단계. */
		if (retval)
		/* NVMe: 전원 켜기에 실패하면 즉시 반환. */
			return retval;
		/* NVMe: 실패 코드를 호출자에게 전달. */
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
			      INDICATOR_NOOP);
	/* NVMe: Power LED를 깜빡임(blink) 상태로 설정. 장치 구성 중임을 표시.
	 *       Attention LED는 변경 없음(INDICATOR_NOOP). */

	/* Check link training status */
	/* NVMe: PCIe 링크 트레이닝 상태를 확인. */
	retval = pciehp_check_link_status(ctrl);
	/* NVMe: PCIe 링크가 L0로 up되었는지, training에 실패하지는 않았는지 점검.
	 *       NVMe와 포트 간 링크가 정상이어야 이후 PCI enumeration이 가능. */
	if (retval)
	/* NVMe: 링크가 정상적으로 트레이닝되지 않았으면 오류 처리로 이동. */
		goto err_exit;
	/* NVMe: err_exit 레이블로 점프해 슬롯을 off로 전환. */

	/* Check for a power fault */
	/* NVMe: 전원 이상(파워 폴트) 여부 확인. */
	if (ctrl->power_fault_detected || pciehp_query_power_fault(ctrl)) {
	/* NVMe: 이전에 파워 폴트가 검출되었거나, 지금 Slot Status의 Power Fault
	 *       Detected 비트가 1이면 NVMe 슬롯에 전원 문제가 있음. */
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		/* NVMe: 커널 로그에 파워 폴트 메시지 출력. */
		retval = -EIO;
		/* NVMe: I/O 오류 코드 설정. */
		goto err_exit;
		/* NVMe: 슬롯 off 처리. */
	}

	retval = pciehp_configure_device(ctrl);
	/* NVMe: 하위 버스의 PCI 장치(NVMe)를 탐색/구성/바인딩.
	 *       이 함수 낶에서 pci_scan_slot(), pci_bus_add_devices(),
	 *       NVMe 드라이버의 probe() 등이 연쇄적으로 호출될 수 있다. */
	if (retval) {
	/* NVMe: 장치 구성 중 오류 발생. */
		if (retval != -EEXIST) {
		/* NVMe: 이미 존재하는 장치가 아닌 다른 실패인 경우에만 오류 로깅. */
			ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
				 pci_domain_nr(parent), parent->number);
			/* NVMe: 지정된 버스 번호에 장치를 추가할 수 없음을 기록. */
			goto err_exit;
			/* NVMe: 슬롯 off 처리. */
		}
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
			      PCI_EXP_SLTCTL_ATTN_IND_OFF);
	/* NVMe: Power LED ON(정상 동작), Attention LED OFF.
	 *       NVMe가 정상적으로 탐지/구성되었음을 표시. */
	return 0;
	/* NVMe: 성공 반환. */

err_exit:
	set_slot_off(ctrl);
	/* NVMe: 오류 발생 시 슬롯 전원을 끄고 Attention LED ON. */
	return retval;
	/* NVMe: 오류 코드 반환. */
}

/**
 * remove_board - Turn off slot and Power Indicator
 * @ctrl: PCIe hotplug controller where board is being removed
 * @safe_removal: whether the board is safely removed (versus surprise removed)
 */
/* NVMe: NVMe 등 카드 제거 시 장치 해제 후 슬롯 전원/인디케이터를 끈다. */
static void remove_board(struct controller *ctrl, bool safe_removal)
/* NVMe: safe_removal이 true면 정상 제거, false면 서프라이즈 제거. */
{
	pciehp_unconfigure_device(ctrl, safe_removal);
	/* NVMe: 하위 버스의 장치(NVMe)를 해제.
	 *       safe_removal=true면 NVMe 드라이버 remove, MSI/MSI-X 해제, BAR/리소스
	 *       반납, dma_unmap 등을 정상 수행.
	 *       false면 surprise removal 경로로 빠르게 처리. */

	if (POWER_CTRL(ctrl)) {
	/* NVMe: 전원 제어 가능하면 슬롯 전원을 내린다. */
		pciehp_power_off_slot(ctrl);
		/* NVMe: Slot Control Power Control=0. NVMe에 공급되던 전원 차단. */

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		/* NVMe: NVMe 장치 내 잔류 전원/커패시터가 방전될 시간 확보. */
		msleep(1000);
		/* NVMe: 1초 대기. */

		/* Ignore link or presence changes caused by power off */
		/* NVMe: 전원 off로 인해 발생한 링크/프레즌스 변화 이벤트는 무시. */
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC),
			   &ctrl->pending_events);
		/* NVMe: pending_events 원자 변수에서 DLLSC(Data Link Layer State Changed)
		 *       와 PDC(Presence Detect Changed) 비트를 클리어.
		 *       이 비트들은 전원 off 자체로 인한 가짜 이벤트이므로 처리하지 않는다. */
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      INDICATOR_NOOP);
	/* NVMe: Power LED OFF. Attention LED는 변경 없음. */

	/* Don't carry LBMS indications across */
	/* NVMe: 전원 사이클을 넘어 Lock Button Managed Slot(LBMS) 표시가 남아있지
	 *       않도록 초기화. */
	pcie_reset_lbms(ctrl->pcie->port);
	/* NVMe: 포트의 Link Control 2 또는 Slot Status 관련 LBMS 비트를 리셋. */
}

static int pciehp_enable_slot(struct controller *ctrl);
/* NVMe: 슬롯 활성화 함수의 전방 선언. */
static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal);
/* NVMe: 슬롯 비활성화 함수의 전방 선언. */

void pciehp_request(struct controller *ctrl, int action)
/* NVMe: 핫플러그 이벤트 처리를 IRQ 스레드에 요청. NVMe 삽입/제거/링크변화 시
 *       이 함수를 통해 비동기 처리가 시작된다. */
{
	atomic_or(action, &ctrl->pending_events);
	/* NVMe: ctrl->pending_events 원자 변수에 action 비트를 OR.
	 *       action으로는 PCI_EXP_SLTSTA_PDC, PCI_EXP_SLTSTA_DLLSC,
	 *       DISABLE_SLOT 등이 올 수 있다.
	 *       NVMe 장치의 삽입/링크업/링크다운/제거 요청이 여기 누적된다. */
	if (!pciehp_poll_mode)
	/* NVMe: 폴 모드가 아니면(즉, 인터럽트 기반 핫플러그) IRQ 스레드를 깨운다. */
		irq_wake_thread(ctrl->pcie->irq, ctrl);
	/* NVMe: pciehp의 IRQ 스레드(ist)를 깨워 pending_events를 처리하게 한다.
	 *       ctrl->pcie->irq는 PCIe 포트의 핫플러그 서비스용 MSI/MSI-X/INTx IRQ이며,
	 *       NVMe 장치 자체의 IRQ와는 별개이지만 동일 IRQ domain 내에 속할 수 있다. */
}

void pciehp_queue_pushbutton_work(struct work_struct *work)
/* NVMe: Attention 버튼 누름 후 5초 경과 시 실행되는 delayed work 핸들러. */
{
	struct controller *ctrl = container_of(work, struct controller,
					     button_work.work);
	/* NVMe: work 구조체에서 controller 구조체 포인터를 얻는다. */

	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호를 위해 뮤텍스 획득. */
	switch (ctrl->state) {
	/* NVMe: 현재 핫플러그 상태를 확인. */
	case BLINKINGOFF_STATE:
	/* NVMe: 사용자가 ON 상태에서 버튼을 눌러 5초 후 전원 off 예약 중. */
		pciehp_request(ctrl, DISABLE_SLOT);
		/* NVMe: 슬롯 비활성화(전원 off) 요청을 IRQ 스레드에 전달. */
		break;
		/* NVMe: switch 문의 이 case 종료. */
	case BLINKINGON_STATE:
	/* NVMe: 사용자가 OFF 상태에서 버튼을 눌러 5초 후 전원 on 예약 중. */
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		/* NVMe: Presence Detect Changed 이벤트로 처리해 슬롯 활성화 경로 탐. */
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: 예상치 못한 상태면 아무 것도 하지 않는다. */
		break;
		/* NVMe: default 종료. */
	}
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */
}

void pciehp_handle_button_press(struct controller *ctrl)
/* NVMe: Attention 버튼 물리적 누름 이벤트 처리. 사용자가 NVMe 슬롯의 버튼을
 *       눌렀을 때 5초 타이머를 시작하거나 취소. */
{
	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	switch (ctrl->state) {
	/* NVMe: 현재 상태에 따라 분기. */
	case OFF_STATE:
	/* NVMe: 슬롯이 꺼져 있음. */
	case ON_STATE:
	/* NVMe: 슬롯이 켜져 있음. */
		if (ctrl->state == ON_STATE) {
		/* NVMe: 현재 ON 상태이면 전원 off 예약. */
			ctrl->state = BLINKINGOFF_STATE;
			/* NVMe: BLINKINGOFF 상태로 전환. 5초 뒤 off 예정. */
			ctrl_info(ctrl, "Slot(%s): Button press: will power off in 5 sec\n",
				  slot_name(ctrl));
			/* NVMe: 사용자에게 5초 후 전원 off 예고. */
		} else {
		/* NVMe: OFF 상태이면 전원 on 예약. */
			ctrl->state = BLINKINGON_STATE;
			/* NVMe: BLINKINGON 상태로 전환. 5초 뒤 on 예정. */
			ctrl_info(ctrl, "Slot(%s): Button press: will power on in 5 sec\n",
				  slot_name(ctrl));
			/* NVMe: 사용자에게 5초 후 전원 on 예고. */
		}
		/* blink power indicator and turn off attention */
		/* NVMe: Power LED를 깜빡이게 하고 Attention LED를 끈다. */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
				      PCI_EXP_SLTCTL_ATTN_IND_OFF);
		/* NVMe: LED 상태 변경. */
		schedule_delayed_work(&ctrl->button_work, 5 * HZ);
		/* NVMe: 5초 후 pciehp_queue_pushbutton_work()가 실행되도록 예약. */
		break;
		/* NVMe: case 종료. */
	case BLINKINGOFF_STATE:
	/* NVMe: 이미 off 예약 중. */
	case BLINKINGON_STATE:
	/* NVMe: 이미 on 예약 중. */
		/*
		 * Cancel if we are still blinking; this means that we
		 * press the attention again before the 5 sec. limit
		 * expires to cancel hot-add or hot-remove
		 */
		/* NVMe: 5초 만료 전에 다시 버튼을 누륶면 예약을 취소. */
		cancel_delayed_work(&ctrl->button_work);
		/* NVMe: 예약된 delayed work를 취소. */
		if (ctrl->state == BLINKINGOFF_STATE) {
		/* NVMe: off 예약을 취소하고 ON 상태로 복귀. */
			ctrl->state = ON_STATE;
			/* NVMe: 상태를 ON으로 복원. */
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			/* NVMe: Power LED ON, Attention LED OFF. */
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power off\n",
				  slot_name(ctrl));
			/* NVMe: 취소 메시지 출력. */
		} else {
		/* NVMe: on 예약을 취소하고 OFF 상태로 복귀. */
			ctrl->state = OFF_STATE;
			/* NVMe: 상태를 OFF로 복원. */
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			/* NVMe: Power LED OFF, Attention LED OFF. */
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power on\n",
				  slot_name(ctrl));
			/* NVMe: 취소 메시지 출력. */
		}
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: 정의되지 않은 상태. */
		ctrl_err(ctrl, "Slot(%s): Button press: ignoring invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		/* NVMe: 잘못된 상태임을 경고. */
		break;
		/* NVMe: default 종료. */
	}
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */
}

void pciehp_handle_disable_request(struct controller *ctrl)
/* NVMe: sysfs나 사용자 요청으로 슬롯 비활성화 시 호출. */
{
	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	switch (ctrl->state) {
	/* NVMe: 버튼 blinking 중이면 우선 취소. */
	case BLINKINGON_STATE:
	/* NVMe: on 예약 중이던 것 취소. */
	case BLINKINGOFF_STATE:
	/* NVMe: off 예약 중이던 것 취소. */
		cancel_delayed_work(&ctrl->button_work);
		/* NVMe: 예약된 버튼 work 취소. */
		break;
		/* NVMe: case 종료. */
	}
	ctrl->state = POWEROFF_STATE;
	/* NVMe: 전원 off 진행 중 상태로 설정. */
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */

	ctrl->request_result = pciehp_disable_slot(ctrl, SAFE_REMOVAL);
	/* NVMe: 안전 제거 방식으로 슬롯을 비활성화.
	 *       NVMe 장치의 리소스, MSI/MSI-X, BAR 등이 정리된다. */
}

void pciehp_handle_presence_or_link_change(struct controller *ctrl, u32 events)
/* NVMe: NVMe 장치 삽입/제거 또는 PCIe 링크 up/down 이벤트 처리.
 *       events는 Slot Status 레지스터의 PDC/DLLSC 비트 조합. */
{
	int present, link_active;
	/* NVMe: 카드 프레즌스 상태와 링크 활성 상태를 저장할 변수. */

	/*
	 * If the slot is on and presence or link has changed, turn it off.
	 * Even if it's occupied again, we cannot assume the card is the same.
	 */
	/* NVMe: 슬롯이 ON 상태에서 프레즌스 또는 링크 상태가 변하면 우선 off 처리.
	 *       NVMe가 갑자기 빠졌다가 다른 NVMe로 교첼 수 있으므로 기존 장치를
	 *       해제하고 다시 enumerate 해야 한다. */
	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	switch (ctrl->state) {
	/* NVMe: 현재 상태 검사. */
	case BLINKINGOFF_STATE:
	/* NVMe: off 예약 중에 이벤트 발생. */
		cancel_delayed_work(&ctrl->button_work);
		/* NVMe: 예약된 버튼 work 취소. */
		fallthrough;
		/* NVMe: 아래 ON_STATE 처리로 이어진다. */
	case ON_STATE:
	/* NVMe: 슬롯이 켜져 있을 때. */
		ctrl->state = POWEROFF_STATE;
		/* NVMe: 전원 off 진행 상태로 변경. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 로그 출력과 장치 해제 전 뮤텍스 해제. */
		if (events & PCI_EXP_SLTSTA_DLLSC) {
		/* NVMe: Data Link Layer State Changed 이벤트, 즉 링크 다운/업. */
			ctrl_info(ctrl, "Slot(%s): Link Down\n",
				  slot_name(ctrl));
			/* NVMe: 링크 다운 메시지. NVMe와 포트 간 링크가 끊김. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_LINK_DOWN);
			/* NVMe: ftrace 추적 이벤트 기록. */
		}
		if (events & PCI_EXP_SLTSTA_PDC) {
		/* NVMe: Presence Detect Changed 이벤트. */
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  slot_name(ctrl));
			/* NVMe: 카드가 감지되지 않음. NVMe가 물리적으로 제거됨. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_NOT_PRESENT);
			/* NVMe: ftrace 추적 이벤트 기록. */
		}
		pciehp_disable_slot(ctrl, SURPRISE_REMOVAL);
		/* NVMe: 서프라이즈 제거 방식으로 슬롯 비활성화.
		 *       NVMe 장치를 긴급 해제. */
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: ON/BLINKINGOFF 외 상태면 아무 것도 하지 않는다. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 뮤텍스 해제. */
		break;
		/* NVMe: default 종료. */
	}

	/* Turn the slot on if it's occupied or link is up */
	/* NVMe: 슬롯에 카드가 있거나 링크가 up이면 슬롯을 켠다. */
	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	present = pciehp_card_present(ctrl);
	/* NVMe: Presence Detect 핀 또는 EMULA션 상태를 읽어 카드 존재 여부 확인. */
	link_active = pciehp_check_link_active(ctrl);
	/* NVMe: PCIe 링크가 L0 활성 상태인지 확인. */
	if (present <= 0 && link_active <= 0) {
	/* NVMe: 카드도 없고 링크도 up 아니면 슬롯을 끈다/유지한다. */
		if (ctrl->state == BLINKINGON_STATE) {
		/* NVMe: on 예약 중이었는데 실제로 카드/링크가 없으면 예약 취소. */
			ctrl->state = OFF_STATE;
			/* NVMe: OFF 상태로 복귀. */
			cancel_delayed_work(&ctrl->button_work);
			/* NVMe: 예약된 버튼 work 취소. */
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      INDICATOR_NOOP);
			/* NVMe: Power LED OFF. */
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  slot_name(ctrl));
			/* NVMe: 카드 없음 메시지. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_NOT_PRESENT);
			/* NVMe: ftrace 추적 이벤트. */
		}
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 뮤텍스 해제. */
		return;
		/* NVMe: 더 이상 처리할 것 없음. */
	}

	switch (ctrl->state) {
	/* NVMe: 카드/링크가 있으므로 슬롯 on 처리. */
	case BLINKINGON_STATE:
	/* NVMe: on 예약 중이었으면 예약 취소 후 즉시 on. */
		cancel_delayed_work(&ctrl->button_work);
		/* NVMe: 예약 work 취소. */
		fallthrough;
		/* NVMe: 아래 OFF_STATE 처리로 이어진다. */
	case OFF_STATE:
	/* NVMe: 슬롯이 꺼져 있으면 켠다. */
		ctrl->state = POWERON_STATE;
		/* NVMe: 전원 on 진행 상태로 변경. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 로그 출력과 장치 구성 전 뮤텍스 해제. */
		if (present) {
		/* NVMe: 카드가 물리적으로 감지됨. */
			ctrl_info(ctrl, "Slot(%s): Card present\n",
				  slot_name(ctrl));
			/* NVMe: NVMe 카드 삽입 메시지. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_PRESENT);
			/* NVMe: ftrace 추적 이벤트. */
		}
		if (link_active) {
		/* NVMe: PCIe 링크가 up됨. */
			ctrl_info(ctrl, "Slot(%s): Link Up\n", slot_name(ctrl));
			/* NVMe: 링크 업 메시지. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_LINK_UP);
			/* NVMe: ftrace 추적 이벤트. */
		}
		ctrl->request_result = pciehp_enable_slot(ctrl);
		/* NVMe: 슬롯을 켜고 NVMe 장치를 탐색/구성.
		 *       성공하면 ctrl->state가 ON_STATE가 된다. */
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: 이미 켜져 있거나 다른 상태면 아무 것도 안 한다. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 뮤텍스 해제. */
		break;
		/* NVMe: default 종료. */
	}
}

static int __pciehp_enable_slot(struct controller *ctrl)
/* NVMe: 실제 슬롯 활성화 로직. 전원 상태, MRL 상태를 점검 후 board_added() 호출. */
{
	u8 getstatus = 0;
	/* NVMe: 하드웨어 상태를 읽어 저장할 8비트 변수. */

	if (MRL_SENS(ctrl)) {
	/* NVMe: Mechanical Release Latch 센서가 있는 슬롯. */
		pciehp_get_latch_status(ctrl, &getstatus);
		/* NVMe: MRL 상태를 읽는다. */
		if (getstatus) {
		/* NVMe: 래치가 열린 상태면 NVMe가 물리적으로 고정되지 않았으므로
		 *       활성화하지 않는다. */
			ctrl_info(ctrl, "Slot(%s): Latch open\n",
				  slot_name(ctrl));
			/* NVMe: 래치 열림 메시지. */
			return -ENODEV;
			/* NVMe: 장치 없음 오류 반환. */
		}
	}

	if (POWER_CTRL(ctrl)) {
	/* NVMe: 전원 제어가 가능하면 현재 전원 상태 확인. */
		pciehp_get_power_status(ctrl, &getstatus);
		/* NVMe: Slot Status의 Power Indicator 상태를 읽는다. */
		if (getstatus) {
		/* NVMe: 이미 전원이 켜져 있으면. */
			ctrl_info(ctrl, "Slot(%s): Already enabled\n",
				  slot_name(ctrl));
			/* NVMe: 이미 활성화됨 메시지. */
			return 0;
			/* NVMe: 성공으로 반환. */
		}
	}

	return board_added(ctrl);
	/* NVMe: 전원/링크/장치 구성 수행. */
}

static int pciehp_enable_slot(struct controller *ctrl)
/* NVMe: 슬롯 활성화 래퍼. 런타임 전원과 상태 머신을 관리. */
{
	int ret;
	/* NVMe: 반환값 저장 변수. */

	pm_runtime_get_sync(&ctrl->pcie->port->dev);
	/* NVMe: PCIe 포트의 runtime PM 레퍼런스를 증가시키고 활성화를 동기 대기.
	 *       NVMe 슬롯 동작 중 포트가 저전력 상태로 들어가지 않도록 막는다. */
	ret = __pciehp_enable_slot(ctrl);
	/* NVMe: 실제 슬롯 활성화 수행. */
	if (ret && ATTN_BUTTN(ctrl))
	/* NVMe: 활성화 실패하고 Attention 버튼이 있는 슬롯이면. */
		/* may be blinking */
		/* NVMe: 버튼 누름으로 인해 Power LED가 깜빡이는 상태일 수 있으므로
		 *       OFF로 되돌려 사용자에게 실패를 알린다. */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
				      INDICATOR_NOOP);
		/* NVMe: Power LED OFF. */
	pm_runtime_put(&ctrl->pcie->port->dev);
	/* NVMe: PCIe 포트의 runtime PM 레퍼런스를 감소.
	 *       NVMe 사용 중이 아니면 포트가 다시 저전력 상태로 전환될 수 있다. */

	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	ctrl->state = ret ? OFF_STATE : ON_STATE;
	/* NVMe: 실패 시 OFF, 성공 시 ON 상태로 설정. */
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */

	return ret;
	/* NVMe: 활성화 결과 반환. */
}

static int __pciehp_disable_slot(struct controller *ctrl, bool safe_removal)
/* NVMe: 실제 슬롯 비활성화 로직. 전원 상태 확인 후 remove_board() 호출. */
{
	u8 getstatus = 0;
	/* NVMe: 하드웨어 상태를 읽어 저장할 8비트 변수. */

	if (POWER_CTRL(ctrl)) {
	/* NVMe: 전원 제어 가능하면. */
		pciehp_get_power_status(ctrl, &getstatus);
		/* NVMe: 현재 전원 상태 읽기. */
		if (!getstatus) {
		/* NVMe: 이미 전원이 꺼져 있으면. */
			ctrl_info(ctrl, "Slot(%s): Already disabled\n",
				  slot_name(ctrl));
			/* NVMe: 이미 비활성화됨 메시지. */
			return -EINVAL;
			/* NVMe: 잘못된 인수 오류 반환. */
		}
	}

	remove_board(ctrl, safe_removal);
	/* NVMe: NVMe 장치 해제 및 슬롯 전원 off. */
	return 0;
	/* NVMe: 성공 반환. */
}

static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal)
/* NVMe: 슬롯 비활성화 래퍼. 런타임 전원과 상태 머신을 관리. */
{
	int ret;
	/* NVMe: 반환값 저장 변수. */

	pm_runtime_get_sync(&ctrl->pcie->port->dev);
	/* NVMe: PCIe 포트의 runtime PM 레퍼런스를 증가시키고 활성화.
	 *       NVMe 제거 작업 중 포트가 저전력 상태로 들어가지 않도록 막는다. */
	ret = __pciehp_disable_slot(ctrl, safe_removal);
	/* NVMe: 실제 슬롯 비활성화 수행. */
	pm_runtime_put(&ctrl->pcie->port->dev);
	/* NVMe: PCIe 포트의 runtime PM 레퍼런스 감소. */

	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	ctrl->state = OFF_STATE;
	/* NVMe: 상태를 OFF로 설정. */
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */

	return ret;
	/* NVMe: 비활성화 결과 반환. */
}

int pciehp_sysfs_enable_slot(struct hotplug_slot *hotplug_slot)
/* NVMe: /sys/bus/pci/slots/.../power 등 sysfs 인터페이스에서 슬롯 활성화 요청 시
 *       호출. 사용자가 NVMe 슬롯을 켜려 할 때 사용. */
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	/* NVMe: hotplug_slot 구조체에서 controller 구조체 포인터 추출. */

	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	switch (ctrl->state) {
	/* NVMe: 현재 상태에 따라 처리. */
	case BLINKINGON_STATE:
	/* NVMe: on 예약 중. */
	case OFF_STATE:
	/* NVMe: 꺼져 있음. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 요청 처리 전 뮤텍스 해제. */
		/*
		 * The IRQ thread becomes a no-op if the user pulls out the
		 * card before the thread wakes up, so initialize to -ENODEV.
		 */
		/* NVMe: 사용자가 IRQ 스레드가 깨어나기 전 카드를 빼면 요청이 no-op이
		 *       되므로, 미리 -ENODEV로 초기화핑 결과를 안전하게 만든다. */
		ctrl->request_result = -ENODEV;
		/* NVMe: 결과를 -ENODEV로 초기화. */
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		/* NVMe: Presence Detect Changed 이벤트를 pending_events에 설정하고
		 *       IRQ 스레드 깨우기. */
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		/* NVMe: IRQ 스레드(ist)가 pending_events를 모두 처리하고 종료될 때까지
		 *       대기. NVMe 장치가 완전히 활성화/탐지될 때까지 블록. */
		return ctrl->request_result;
		/* NVMe: 활성화 결과를 sysfs 호출자에게 반환. */
	case POWERON_STATE:
	/* NVMe: 이미 전원 on 진행 중. */
		ctrl_info(ctrl, "Slot(%s): Already in powering on state\n",
			  slot_name(ctrl));
		/* NVMe: 이미 진행 중 메시지. */
		break;
		/* NVMe: case 종료. */
	case BLINKINGOFF_STATE:
	/* NVMe: off 예약 중. */
	case ON_STATE:
	/* NVMe: 이미 켜져 있음. */
	case POWEROFF_STATE:
	/* NVMe: 전원 off 진행 중. */
		ctrl_info(ctrl, "Slot(%s): Already enabled\n",
			  slot_name(ctrl));
		/* NVMe: 이미 활성화됨 메시지. */
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: 잘못된 상태. */
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		/* NVMe: 오류 메시지. */
		break;
		/* NVMe: default 종료. */
	}
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */

	return -ENODEV;
	/* NVMe: 잘못된 상태에서는 -ENODEV 반환. */
}

int pciehp_sysfs_disable_slot(struct hotplug_slot *hotplug_slot)
/* NVMe: sysfs 인터페이스에서 슬롯 비활성화 요청 시 호출. 사용자가 NVMe 슬롯을
 *       끄려 할 때 사용. */
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	/* NVMe: hotplug_slot에서 controller 포인터 추출. */

	mutex_lock(&ctrl->state_lock);
	/* NVMe: 상태 머신 보호. */
	switch (ctrl->state) {
	/* NVMe: 현재 상태에 따라 처리. */
	case BLINKINGOFF_STATE:
	/* NVMe: off 예약 중. */
	case ON_STATE:
	/* NVMe: 켜져 있음. */
		mutex_unlock(&ctrl->state_lock);
		/* NVMe: 요청 처리 전 뮤텍스 해제. */
		pciehp_request(ctrl, DISABLE_SLOT);
		/* NVMe: DISABLE_SLOT 이벤트를 pending_events에 설정하고 IRQ 스레드
		 *       깨우기. */
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		/* NVMe: IRQ 스레드가 NVMe 장치 해제와 슬롯 전원 off를 완료할 때까지
		 *       블록. */
		return ctrl->request_result;
		/* NVMe: 비활성화 결과 반환. */
	case POWEROFF_STATE:
	/* NVMe: 이미 전원 off 진행 중. */
		ctrl_info(ctrl, "Slot(%s): Already in powering off state\n",
			  slot_name(ctrl));
		/* NVMe: 이미 진행 중 메시지. */
		break;
		/* NVMe: case 종료. */
	case BLINKINGON_STATE:
	/* NVMe: on 예약 중. */
	case OFF_STATE:
	/* NVMe: 꺼져 있음. */
	case POWERON_STATE:
	/* NVMe: 전원 on 진행 중. */
		ctrl_info(ctrl, "Slot(%s): Already disabled\n",
			  slot_name(ctrl));
		/* NVMe: 이미 비활성화됨 메시지. */
		break;
		/* NVMe: case 종료. */
	default:
	/* NVMe: 잘못된 상태. */
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		/* NVMe: 오류 메시지. */
		break;
		/* NVMe: default 종료. */
	}
	mutex_unlock(&ctrl->state_lock);
	/* NVMe: 상태 뮤텍스 해제. */

	return -ENODEV;
	/* NVMe: 잘못된 상태에서는 -ENODEV 반환. */
}
