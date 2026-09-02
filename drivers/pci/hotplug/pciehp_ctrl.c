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
 * pciehp_sysfs_enable_slot() / pciehp_sysfs_disable_slot() : sysfs 요청용
 *                               바깥 진입점. 위 두 함수를 잠금과 함께 감싼다.
 * set_slot_off()              : 전원과 표시등을 끄는 공통 뒤처리.
 * board_added() / remove_board() : 열거와 해제의 실제 몸통.
 */

#define dev_fmt(fmt) "pciehp: " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pm_runtime.h>
#include <linux/pci.h>
#include <trace/events/pci.h>

#include "../pci.h"
#include "pciehp.h"

/* The following routines constitute the bulk of the
   hotplug controller logic
 */

#define SAFE_REMOVAL	 true
#define SURPRISE_REMOVAL false

/* [한국어]
 * set_slot_off - 슬롯을 끄고 오류 표시를 켠다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * board_added() 의 오류 경로 전용이다. 카드를 켜려다 실패했을 때 슬롯을
 * 안전한 상태로 되돌린다.
 *
 * 전원 제어를 지원하는 슬롯만 실제로 전원을 끊는다. 지원하지 않는 슬롯에서는
 * 표시등만 바꾸는 셈인데, 그것이 이 함수가 할 수 있는 전부다.
 *
 * 1초를 기다리는 것이 중요하다. 전원을 끊자마자 다시 켜면 커패시터에 남은
 * 전하 때문에 카드가 리셋되지 않은 채 살아 있을 수 있다.
 *
 * 표시등 두 개를 함께 바꾼다 — 전원 표시는 끄고 주의 표시는 켠다. 사람이
 * 슬롯을 보고 "꺼져 있고, 문제가 있었다" 를 알 수 있게 하는 조합이다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. msleep 이 있어 잠들 수 있는
 * 문맥이어야 한다.
 *
 * 에러 경로: 없다. 이 함수 자체가 오류 처리다.
 *
 * 호출 체인:
 *   board_added() 의 err_exit → [이 함수]
 *     → pciehp_power_off_slot() → pciehp_set_indicators()
 */
static void set_slot_off(struct controller *ctrl)
{
	/*
	 * Turn off slot, turn on attention indicator, turn off power
	 * indicator
	 */
	if (POWER_CTRL(ctrl)) {
		pciehp_power_off_slot(ctrl);

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		msleep(1000);
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      /* [한국어] 주의 표시등은 켠다. 두 표시등을 함께 바꿔 '꺼져 있고, 문제가 있었다' 를 사람에게 알린다. */
			      PCI_EXP_SLTCTL_ATTN_IND_ON);
}

/**
 * board_added - Called after a board has been added to the system.
 * @ctrl: PCIe hotplug controller where board is added
 *
 * Turns power on for the board.
 * Configures board.
 */
/* [한국어]
 * board_added - 슬롯에 전원을 넣고 카드를 열거해 드라이버까지 붙인다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 = 성공, 음수 오류.
 *
 * 카드가 꽂힌 뒤 실제로 쓸 수 있게 되기까지의 전 과정이다.
 *
 * 순서가 정해져 있고 각 단계가 앞 단계를 전제한다.
 * 1. 전원을 넣는다. 지원하는 슬롯만 해당한다.
 * 2. 전원 표시등을 깜빡인다 — 준비 중이라는 표시다.
 * 3. 링크가 올라올 때까지 기다리고 상태를 확인한다. 링크가 서지 않으면
 *    그 아래로 config 접근이 되지 않아 더 진행할 수 없다.
 * 4. 전원 결함을 확인한다. 카드가 규격을 넘는 전류를 끌면 슬롯이 그것을
 *    보고하는데, 그 상태로 진행하면 시스템 전체가 위험하다. 인터럽트가
 *    이미 잡아 둔 플래그와 지금 읽은 상태를 **둘 다** 보는 것이 요점으로,
 *    결함이 인터럽트로 왔을 수도 레지스터에만 남아 있을 수도 있다.
 * 5. 장치를 열거하고 드라이버를 붙인다.
 * 6. 표시등을 정상으로 바꾼다 — 전원 켜짐, 주의 꺼짐.
 *
 * -EEXIST 를 오류로 다루지 않는 것이 이 함수의 특이한 점이다. 그 자리에
 * 이미 장치가 있다는 뜻인데, 다른 경로가 먼저 열거를 마친 상황이라
 * 결과적으로 원하는 상태에 도달해 있다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. 링크 대기와 열거가 있어
 * 오래 걸리며 잠들 수 있는 문맥이어야 한다.
 *
 * 에러 경로: 어느 단계에서 실패하든 set_slot_off() 로 슬롯을 끄고 주의
 * 표시를 켠 뒤 오류를 돌려준다.
 *
 * 호출 체인:
 *   __pciehp_enable_slot() → [이 함수]
 *     → pciehp_power_on_slot() → pciehp_check_link_status()
 *     → pciehp_query_power_fault() → pciehp_configure_device()
 */
static int board_added(struct controller *ctrl)
{
	/* [한국어] 각 단계의 결과. 0 으로 시작해, 아무 단계도 실패하지 않으면 그대로 성공이 된다. */
	int retval = 0;
	/* [한국어] 이 슬롯 아래의 버스. 열거가 실패했을 때 어느 위치였는지 찍는 데만 쓴다. */
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	if (POWER_CTRL(ctrl)) {
		/* Power on slot */
		retval = pciehp_power_on_slot(ctrl);
		if (retval)
			/* [한국어] 전원을 넣지 못했으면 더 진행할 수 없으므로 그대로 물러난다. */
			return retval;
	/* [한국어] 전원 제어를 지원하지 않는 슬롯은 이 단계를 건너뛰고 곧장 링크 확인으로 간다. */
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
			      /* [한국어] 주의 표시등은 그대로 둔다 — 이 단계에서 판단할 것이 없다. */
			      INDICATOR_NOOP);

	/* Check link training status */
	retval = pciehp_check_link_status(ctrl);
	if (retval)
		/* [한국어] 링크가 서지 않으면 그 아래로 config 접근이 되지 않아 열거를 시작할 수 없다. */
		goto err_exit;

	/* Check for a power fault */
	if (ctrl->power_fault_detected || pciehp_query_power_fault(ctrl)) {
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		/* [한국어] 전원 결함은 시스템 전체를 위협하므로 입출력 오류로 다룬다. */
		retval = -EIO;
		/* [한국어] 슬롯을 끄는 오류 경로로 간다. */
		goto err_exit;
	}

	retval = pciehp_configure_device(ctrl);
	/* [한국어] 열거나 드라이버 바인딩이 실패했으면, */
	if (retval) {
		/* [한국어] -EEXIST 만은 예외다 — 그 자리에 이미 장치가 있다는 뜻이라, 다른 경로가 먼저
		 * 열거를 마친 것이고 결과적으로 원하는 상태에 도달해 있다. */
		if (retval != -EEXIST) {
			/* [한국어] 그 밖의 오류는 어느 위치의 장치였는지 남기고, */
			ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
				 /* [한국어] 도메인과 버스 번호로 위치를 특정한다. 함수 번호를 00 으로 고정해 찍는 것은
				  * 핫플러그 슬롯의 첫 장치가 언제나 그 자리이기 때문이다. */
				 pci_domain_nr(parent), parent->number);
			goto err_exit;
		}
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
			      /* [한국어] 주의 표시등도 끈다 — 정상적으로 켜졌다는 표시다. */
			      PCI_EXP_SLTCTL_ATTN_IND_OFF);
	return 0;

err_exit:
	set_slot_off(ctrl);
	return retval;
/* [한국어] 여기까지 오면 카드가 열거되고 드라이버까지 붙은 상태다. */
}

/**
 * remove_board - Turn off slot and Power Indicator
 * @ctrl: PCIe hotplug controller where board is being removed
 * @safe_removal: whether the board is safely removed (versus surprise removed)
 */
/* [한국어]
 * remove_board - 카드의 장치들을 제거하고 슬롯 전원을 끊는다
 *
 * @ctrl: 대상 컨트롤러.
 * @safe_removal: 카드가 아직 꽂혀 있는 상태의 계획된 제거인지.
 *
 * board_added() 의 짝이다.
 *
 * safe_removal 이 아래로 전달되어, 카드가 이미 뽑힌 뒤라면 응답하지 않는
 * 장치에 config 접근을 시도하지 않게 한다.
 *
 * 대기 이벤트를 지우는 것이 이 함수의 미묘한 부분이다. 전원을 끊는 행위
 * 자체가 링크를 내리고 카드 없음을 만들어 내므로, 그 결과로 올라온 DLLSC 와
 * PDC 이벤트가 대기열에 남는다. 그것을 지우지 않으면 방금 우리가 만든
 * 변화를 새 이벤트로 오인해 다시 처리하려 든다.
 *
 * 지우는 시점이 1초 대기 **뒤** 인 것도 그래서다. 전원이 완전히 내려가
 * 더는 새 이벤트가 생기지 않게 된 뒤에 지워야 한다.
 *
 * 마지막의 LBMS 초기화는 링크 대역폭 변화 표시를 지우는 것이다. 전원을
 * 끊으면서 생긴 대역폭 변화가 다음 링크 판단을 흐리지 않게 한다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. msleep 과 장치 제거가 있어
 * 잠들 수 있는 문맥이어야 한다.
 *
 * 에러 경로: 반환값이 없다. 제거는 실패해도 되돌릴 수 없다.
 *
 * 호출 체인:
 *   __pciehp_disable_slot() → [이 함수]
 *     → pciehp_unconfigure_device() → pciehp_power_off_slot()
 *     → pcie_reset_lbms()
 */
static void remove_board(struct controller *ctrl, bool safe_removal)
{
	pciehp_unconfigure_device(ctrl, safe_removal);

	if (POWER_CTRL(ctrl)) {
		/* [한국어] 전원을 끊는다. 이 행위 자체가 링크를 내리고 카드 없음을 만들어 내,
		 * 아래에서 지울 이벤트들을 생성한다. */
		pciehp_power_off_slot(ctrl);

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		msleep(1000);

		/* Ignore link or presence changes caused by power off */
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC),
			   &ctrl->pending_events);
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      /* [한국어] 주의 표시등은 건드리지 않는다. 정상적인 제거라면 켤 이유가 없고,
			       * 오류로 인한 제거라면 그 표시를 낸 쪽이 이미 켜 두었다. */
			      INDICATOR_NOOP);

	/* Don't carry LBMS indications across */
	pcie_reset_lbms(ctrl->pcie->port);
}

static int pciehp_enable_slot(struct controller *ctrl);
static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal);

/* [한국어]
 * pciehp_request - 처리할 일을 대기열에 얹고 IRQ 스레드를 깨운다
 *
 * @ctrl: 대상 컨트롤러.
 * @action: 요청할 동작 비트.
 *
 * 이 드라이버의 모든 요청이 이 한 지점으로 모인다 — 인터럽트가 알아낸
 * 이벤트도, 사용자의 sysfs 조작도, 버튼 타이머가 만든 요청도 전부 여기를
 * 거쳐 IRQ 스레드로 간다.
 *
 * 한 곳으로 모으는 이유는 직렬화다. 실제 처리를 IRQ 스레드 하나가 도맡으면
 * 여러 요청이 동시에 슬롯을 조작하는 일이 없다.
 *
 * atomic_or 로 비트를 얹는 것이 그 대기열의 구현이다. 같은 요청이 여러 번
 * 와도 비트 하나로 합쳐지고, 서로 다른 요청은 함께 남는다.
 *
 * 폴링 모드에서는 깨우지 않는다. 그때는 IRQ 스레드가 없고 주기적으로 도는
 * 폴링 스레드가 대기열을 확인하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, 워크큐, sysfs 쓰기 어디서든. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_isr() / pciehp_queue_pushbutton_work() / sysfs 조작
 *     → [이 함수] → atomic_or() → irq_wake_thread()
 */
void pciehp_request(struct controller *ctrl, int action)
{
	atomic_or(action, &ctrl->pending_events);
	if (!pciehp_poll_mode)
		/* [한국어] IRQ 스레드를 깨워 대기열을 처리하게 한다. 실제 작업을 그 스레드 하나가
		 * 도맡으므로 여러 요청이 동시에 슬롯을 조작하는 일이 없다. */
		irq_wake_thread(ctrl->pcie->irq, ctrl);
}

/* [한국어]
 * pciehp_queue_pushbutton_work - 버튼 5초 대기가 끝났을 때 실제 요청을 낸다
 *
 * @work: 지연 워크. 여기서 컨트롤러를 되찾는다.
 *
 * 주의 버튼을 누르면 곧바로 동작하지 않고 5초를 기다린다. 그동안 다시 누르면
 * 취소되는데, 사람이 실수로 누른 것을 되돌릴 기회를 주기 위해서다.
 *
 * 이 함수는 그 5초가 지났을 때 불린다. 즉 사용자가 취소하지 않았다는 뜻이므로
 * 실제 요청을 낸다.
 *
 * 지금 상태에 따라 무엇을 요청할지 갈린다. BLINKINGOFF 였다면 끄기를,
 * BLINKINGON 이었다면 켜기를 요청한다.
 *
 * 켜기 요청에 PDC(Presence Detect Change)를 쓰는 것이 눈에 띈다. "카드가
 * 꽂혔다" 는 이벤트와 같은 경로로 흘려보내는 것으로, 켜기 처리가 그 이벤트를
 * 다루는 코드와 하나이기 때문이다.
 *
 * 그 밖의 상태면 아무것도 하지 않는다. 5초 사이에 다른 경로가 상태를 바꿨다는
 * 뜻이라, 그때의 요청은 이미 낡았다.
 *
 * 상태를 읽고 판단하는 동안 뮤텍스를 쥔다. 그 사이에 상태가 바뀌면 잘못된
 * 요청을 내게 된다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   워크큐(5초 지연) → [이 함수] → pciehp_request()
 */
void pciehp_queue_pushbutton_work(struct work_struct *work)
{
	struct controller *ctrl = container_of(work, struct controller,
					     button_work.work);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 5초 대기가 끄기였다면, */
	case BLINKINGOFF_STATE:
		/* [한국어] 끄기 요청을 낸다. */
		pciehp_request(ctrl, DISABLE_SLOT);
		break;
	case BLINKINGON_STATE:
		/* [한국어] 켜기 요청을 낸다. PDC(Presence Detect Change)를 쓰는 것은 '카드가 꽂혔다'
		 * 이벤트와 같은 경로로 흘려보내기 위해서다 — 켜기 처리가 그 이벤트를 다루는
		 * 코드와 하나이기 때문이다. */
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		break;
	default:
		break;
	}
	mutex_unlock(&ctrl->state_lock);
}

/* [한국어]
 * pciehp_handle_button_press - 주의 버튼 눌림을 상태 기계에 반영한다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * 버튼 한 번이 두 가지 의미를 갖는다 — 아무 일도 없을 때 누르면 요청이고,
 * 5초 대기 중에 누르면 취소다.
 *
 * 그래서 상태가 넷으로 갈린다.
 * - ON 이면 끄기 요청 → BLINKINGOFF 로 가고 5초 타이머를 건다.
 * - OFF 면 켜기 요청 → BLINKINGON 으로 가고 5초 타이머를 건다.
 * - BLINKINGOFF 면 취소 → 타이머를 죽이고 ON 으로 되돌린다.
 * - BLINKINGON 이면 취소 → 타이머를 죽이고 OFF 로 되돌린다.
 *
 * 깜빡이는 전원 표시등이 "5초 안에 다시 누르면 취소된다" 를 사람에게 알리는
 * 신호다. 취소하면 표시등이 다시 고정된다.
 *
 * 그 밖의 상태(POWERON, POWEROFF)에서는 무시하고 기록만 남긴다. 이미 처리가
 * 진행 중이라 버튼을 받을 수 없는 시점이다.
 *
 * 전 구간에서 뮤텍스를 쥔다. 상태를 읽고 바꾸는 것이 하나의 원자적 동작이어야
 * 한다.
 *
 * 실행 컨텍스트: IRQ 스레드. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 잘못된 상태는 로그로만 남는다.
 *
 * 호출 체인:
 *   pciehp_ist() → [이 함수]
 *     → pciehp_set_indicators() → schedule_delayed_work() / cancel_delayed_work()
 */
void pciehp_handle_button_press(struct controller *ctrl)
{
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 꺼져 있거나, */
	case OFF_STATE:
	/* [한국어] 켜져 있는 상태에서 누른 것이면 새 요청이다. */
	case ON_STATE:
		if (ctrl->state == ON_STATE) {
			ctrl->state = BLINKINGOFF_STATE;
			/* [한국어] 켜져 있었으니 끄기 요청이다. 5초 뒤에 꺼진다고 알린다. */
			ctrl_info(ctrl, "Slot(%s): Button press: will power off in 5 sec\n",
				  /* [한국어] 슬롯 이름을 함께 남겨 여러 슬롯 중 어느 것인지 구분되게 한다. */
				  slot_name(ctrl));
		} else {
			ctrl->state = BLINKINGON_STATE;
			/* [한국어] 꺼져 있었으니 켜기 요청이다. */
			ctrl_info(ctrl, "Slot(%s): Button press: will power on in 5 sec\n",
				  /* [한국어] 여기서도 슬롯 이름을 남긴다. */
				  slot_name(ctrl));
		}
		/* blink power indicator and turn off attention */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
				      PCI_EXP_SLTCTL_ATTN_IND_OFF);
		schedule_delayed_work(&ctrl->button_work, 5 * HZ);
		/* [한국어] 타이머를 걸었으니 이 갈래는 끝이다. */
		break;
	case BLINKINGOFF_STATE:
	/* [한국어] 켜기를 기다리던 중이면 — 아래 갈래는 두 대기 상태를 함께 다룬다. */
	case BLINKINGON_STATE:
		/*
		 * Cancel if we are still blinking; this means that we
		 * press the attention again before the 5 sec. limit
		 * expires to cancel hot-add or hot-remove
		 */
		cancel_delayed_work(&ctrl->button_work);
		if (ctrl->state == BLINKINGOFF_STATE) {
			/* [한국어] 끄기를 기다리던 중이었으니 원래대로 켜진 상태로 되돌린다. */
			ctrl->state = ON_STATE;
			/* [한국어] 전원 표시등을 다시 고정으로 바꾼다 — 깜빡임이 멎으면 사람이 취소됐음을 안다. */
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
					      /* [한국어] 주의 표시등은 꺼 둔다. */
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power off\n",
				  /* [한국어] 취소됐음을 기록에 남긴다. */
				  slot_name(ctrl));
		} else {
			ctrl->state = OFF_STATE;
			/* [한국어] 켜기를 기다리던 중이었으니 꺼진 상태로 되돌리고 표시등도 끈다. */
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      /* [한국어] 주의 표시등도 끈다. */
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power on\n",
				  /* [한국어] 이쪽도 취소를 기록한다. */
				  slot_name(ctrl));
		}
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Button press: ignoring invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);
}

/* [한국어]
 * pciehp_handle_disable_request - 끄기 요청을 처리한다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * sysfs 의 끄기 조작과 버튼 타이머가 낸 끄기 요청이 여기로 온다.
 *
 * 대기 중인 버튼 타이머를 먼저 죽인다. 지금 끄기가 확정됐으므로 5초 뒤에
 * 같은 일을 또 하려는 타이머는 낡은 것이다.
 *
 * 상태를 POWEROFF 로 옮기고 **뮤텍스를 놓은 뒤** 실제 작업을 한다. 그 순서가
 * 중요한데, 아래 작업이 장치 제거까지 이어져 오래 걸리기 때문이다. 그동안
 * 뮤텍스를 쥐고 있으면 상태를 읽기만 하려는 다른 경로까지 막힌다.
 *
 * POWEROFF 라는 중간 상태가 그 사이를 지킨다. 뮤텍스가 풀려 있어도 다른
 * 경로가 그 상태를 보고 "처리 중" 임을 알 수 있다.
 *
 * 결과를 request_result 에 남긴다. sysfs 쪽에서 기다리던 쪽이 그 값을 읽어
 * 사용자에게 돌려준다.
 *
 * 실행 컨텍스트: IRQ 스레드. 프로세스 컨텍스트.
 *
 * 에러 경로: 아래 결과를 request_result 에 담아 요청자에게 전한다.
 *
 * 호출 체인:
 *   pciehp_ist() → [이 함수]
 *     → cancel_delayed_work() → pciehp_disable_slot(SAFE_REMOVAL)
 */
void pciehp_handle_disable_request(struct controller *ctrl)
{
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 켜기를 기다리던 중이거나, */
	case BLINKINGON_STATE:
	/* [한국어] 끄기를 기다리던 중이면 — 어느 쪽이든 그 타이머는 이제 낡았다. */
	case BLINKINGOFF_STATE:
		cancel_delayed_work(&ctrl->button_work);
		break;
	}
	ctrl->state = POWEROFF_STATE;
	/* [한국어] 상태를 옮겼으니 잠금을 놓는다. 아래 작업이 장치 제거까지 이어져 오래 걸리는데,
	 * 그동안 잠금을 쥐면 상태를 읽기만 하려는 다른 경로까지 막힌다.
	 * POWEROFF 라는 중간 상태가 그 사이를 지킨다. */
	mutex_unlock(&ctrl->state_lock);

	ctrl->request_result = pciehp_disable_slot(ctrl, SAFE_REMOVAL);
}

/* [한국어]
 * pciehp_handle_presence_or_link_change - 카드 존재나 링크 상태의 변화를 처리한다
 *
 * @ctrl: 대상 컨트롤러.
 * @events: 어떤 이벤트가 왔는지(PDC, DLLSC).
 *
 * 이 파일에서 가장 복잡한 함수이며, 이유는 **끄기와 켜기를 한 번에 처리할
 * 수 있어야** 하기 때문이다. 카드를 뽑았다가 곧바로 다시 꽂으면 두 변화가
 * 한 번의 처리 안에 들어온다.
 *
 * 그래서 구조가 두 단계다.
 * 1. 먼저 끄기를 판단한다. ON 이나 BLINKINGOFF 였다면 무조건 내린다 —
 *    이벤트가 왔다는 것 자체가 이전 카드가 그대로 있지 않다는 뜻이므로,
 *    지금 카드가 있든 없든 일단 내리는 것이 안전하다.
 * 2. 그 다음 **현재** 상태를 다시 읽어 켜기를 판단한다. 이벤트 비트가 아니라
 *    실제 하드웨어 상태를 읽는 것이 요점으로, 뽑았다 꽂는 사이의 중간
 *    이벤트들은 이미 의미가 없고 지금 카드가 있느냐만 중요하다.
 *
 * 카드도 없고 링크도 없으면 거기서 끝난다. BLINKINGON 이었다면 그 대기를
 * 취소하는데, 켜려고 기다리던 카드가 없어졌기 때문이다.
 *
 * 각 갈래에서 뮤텍스를 놓는 시점이 다른 것에 주의할 만하다. 오래 걸리는
 * 작업(enable/disable) 전에 반드시 놓고, 상태 변수를 만지는 짧은 구간에서만
 * 쥔다.
 *
 * trace 이벤트를 곳곳에 남기는 것은 핫플러그 문제를 나중에 추적하기
 * 위해서다 — 이 경로는 재현이 어려워 로그가 유일한 단서인 경우가 많다.
 *
 * 실행 컨텍스트: IRQ 스레드. 프로세스 컨텍스트이며 열거·제거로 오래 걸린다.
 *
 * 에러 경로: 켜기 결과를 request_result 에 담는다. 끄기 결과는 버려지는데,
 * 서프라이즈 제거는 실패해도 할 수 있는 일이 없기 때문이다.
 *
 * 호출 체인:
 *   pciehp_ist() → [이 함수]
 *     → pciehp_disable_slot(SURPRISE_REMOVAL) → pciehp_card_present()
 *     → pciehp_check_link_active() → pciehp_enable_slot()
 */
void pciehp_handle_presence_or_link_change(struct controller *ctrl, u32 events)
{
	int present, link_active;

	/*
	 * If the slot is on and presence or link has changed, turn it off.
	 * Even if it's occupied again, we cannot assume the card is the same.
	 */
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 끄기를 기다리던 중이었다면, */
	case BLINKINGOFF_STATE:
		/* [한국어] 그 타이머를 죽인다. 이벤트가 왔으므로 5초를 더 기다릴 이유가 없다. */
		cancel_delayed_work(&ctrl->button_work);
		fallthrough;
	/* [한국어] 켜져 있던 경우와 같은 처리로 이어진다 — 어느 쪽이든 지금 카드가 그대로 있지 않다. */
	case ON_STATE:
		/* [한국어] 중간 상태로 옮긴다. */
		ctrl->state = POWEROFF_STATE;
		mutex_unlock(&ctrl->state_lock);
		if (events & PCI_EXP_SLTSTA_DLLSC) {
			/* [한국어] 링크가 내려갔음을 기록한다. */
			ctrl_info(ctrl, "Slot(%s): Link Down\n",
				  /* [한국어] 어느 슬롯인지 함께 남긴다. */
				  slot_name(ctrl));
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_LINK_DOWN);
		}
		if (events & PCI_EXP_SLTSTA_PDC) {
			/* [한국어] 카드가 없어졌음을 기록한다. */
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  /* [한국어] 어느 슬롯인지 함께 남긴다. */
				  slot_name(ctrl));
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_NOT_PRESENT);
		}
		pciehp_disable_slot(ctrl, SURPRISE_REMOVAL);
		/* [한국어] 끄기 처리는 여기까지다. 아래에서 현재 상태를 다시 읽어 켜기를 판단한다. */
		break;
	default:
		mutex_unlock(&ctrl->state_lock);
		break;
	}

	/* Turn the slot on if it's occupied or link is up */
	mutex_lock(&ctrl->state_lock);
	present = pciehp_card_present(ctrl);
	/* [한국어] 링크가 살아 있는지도 확인한다. 카드 감지와 링크 상태를 **둘 다** 보는 이유는
	 * 어느 한쪽만 보고되는 하드웨어가 있기 때문이다. */
	link_active = pciehp_check_link_active(ctrl);
	/* [한국어] 카드도 없고 링크도 없으면 켤 것이 없다. */
	if (present <= 0 && link_active <= 0) {
		/* [한국어] 켜기를 기다리던 중이었다면, */
		if (ctrl->state == BLINKINGON_STATE) {
			/* [한국어] 그 대기를 접고 꺼진 상태로 확정한다 — 기다리던 카드가 없어졌다. */
			ctrl->state = OFF_STATE;
			/* [한국어] 타이머도 죽인다. */
			cancel_delayed_work(&ctrl->button_work);
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      /* [한국어] 주의 표시등은 건드리지 않는다. */
					      INDICATOR_NOOP);
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  /* [한국어] 어느 슬롯인지 남긴다. */
				  slot_name(ctrl));
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_NOT_PRESENT);
		}
		mutex_unlock(&ctrl->state_lock);
		return;
	}

	switch (ctrl->state) {
	/* [한국어] 켜기를 기다리던 중이었다면, */
	case BLINKINGON_STATE:
		/* [한국어] 타이머를 죽인다 — 카드가 실제로 왔으므로 5초를 기다릴 이유가 없다. */
		cancel_delayed_work(&ctrl->button_work);
		fallthrough;
	/* [한국어] 꺼져 있던 경우와 같은 처리로 이어진다. */
	case OFF_STATE:
		/* [한국어] 중간 상태로 옮긴다. */
		ctrl->state = POWERON_STATE;
		mutex_unlock(&ctrl->state_lock);
		if (present) {
			/* [한국어] 카드가 감지됐음을 기록한다. */
			ctrl_info(ctrl, "Slot(%s): Card present\n",
				  /* [한국어] 어느 슬롯인지 남긴다. */
				  slot_name(ctrl));
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_CARD_PRESENT);
		}
		if (link_active) {
			/* [한국어] 링크가 올라왔음을 기록한다. */
			ctrl_info(ctrl, "Slot(%s): Link Up\n", slot_name(ctrl));
			/* [한국어] 추적 이벤트에도 남긴다. 핫플러그 문제는 재현이 어려워 이런 기록이
			 * 유일한 단서인 경우가 많다. */
			trace_pci_hp_event(pci_name(ctrl->pcie->port),
					   slot_name(ctrl),
					   PCI_HOTPLUG_LINK_UP);
		}
		ctrl->request_result = pciehp_enable_slot(ctrl);
		/* [한국어] 켜기 처리는 여기까지다. */
		break;
	default:
		mutex_unlock(&ctrl->state_lock);
		break;
	}
}

/* [한국어]
 * __pciehp_enable_slot - 켜도 되는 상태인지 확인하고 켠다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 = 성공(또는 이미 켜져 있음), 음수 오류.
 *
 * 두 가지 사전 확인을 하고 board_added() 로 넘긴다.
 *
 * 1. 걸쇠가 열려 있으면 -ENODEV. 카드가 물리적으로 고정되지 않은 상태에서
 *    전원을 넣으면 접촉이 불안정해 위험하다. 걸쇠 센서가 있는 슬롯만 해당한다.
 * 2. 이미 전원이 들어와 있으면 성공으로 답하고 물러난다. 오류가 아닌
 *    이유는 원하는 상태에 이미 도달해 있기 때문이다.
 *
 * 두 검사 모두 해당 기능을 지원하는 슬롯에서만 한다. 지원하지 않으면
 * 확인할 방법이 없어 그냥 진행한다.
 *
 * 이름 앞의 밑줄 두 개는 pciehp_enable_slot() 이 감싼다는 표시다. 그쪽이
 * 런타임 PM 참조와 상태 갱신을 맡고, 이쪽은 판단과 실행만 한다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. 프로세스 컨텍스트.
 *
 * 에러 경로: 걸쇠가 열렸으면 -ENODEV, 그 밖은 board_added() 의 오류.
 *
 * 호출 체인:
 *   pciehp_enable_slot() → [이 함수]
 *     → pciehp_get_latch_status() → pciehp_get_power_status()
 *     → board_added()
 */
static int __pciehp_enable_slot(struct controller *ctrl)
{
	u8 getstatus = 0;

	if (MRL_SENS(ctrl)) {
		/* [한국어] 걸쇠 상태를 읽는다. */
		pciehp_get_latch_status(ctrl, &getstatus);
		/* [한국어] 걸쇠가 열려 있으면, */
		if (getstatus) {
			/* [한국어] 그 사실을 남기고, */
			ctrl_info(ctrl, "Slot(%s): Latch open\n",
				  /* [한국어] 어느 슬롯인지 함께 남긴다. */
				  slot_name(ctrl));
			return -ENODEV;
		}
	}

	if (POWER_CTRL(ctrl)) {
		/* [한국어] 전원 상태를 읽는다. */
		pciehp_get_power_status(ctrl, &getstatus);
		/* [한국어] 이미 전원이 들어와 있으면, */
		if (getstatus) {
			/* [한국어] 기록만 남기고, */
			ctrl_info(ctrl, "Slot(%s): Already enabled\n",
				  /* [한국어] 어느 슬롯인지 함께 남긴다. */
				  slot_name(ctrl));
			return 0;
		}
	}

	return board_added(ctrl);
}

/* [한국어]
 * pciehp_enable_slot - 런타임 PM 을 붙잡고 슬롯을 켠 뒤 상태를 갱신한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 = 성공, 음수 오류.
 *
 * __pciehp_enable_slot() 을 감싸며 세 가지를 더한다.
 *
 * 1. 포트의 런타임 PM 참조를 잡는다. 이것이 없으면 작업 도중 포트가 절전에
 *    들어가 config 접근이 실패한다. _sync 판이라 포트가 이미 자고 있으면
 *    깨우고 기다린다.
 * 2. 실패했고 주의 버튼이 있는 슬롯이면 전원 표시등을 끈다. 버튼으로 켜기를
 *    시도했다가 실패한 사용자에게 결과를 보여 주는 것이다.
 * 3. 결과에 따라 상태를 ON 이나 OFF 로 확정한다. 그 전까지 POWERON 이라는
 *    중간 상태였다.
 *
 * 상태 갱신을 PM 참조를 놓은 뒤에 하는 것이 눈에 띈다. 순서를 바꿔도
 * 동작에 차이는 없지만, 뮤텍스를 쥔 구간을 최소로 두는 관용을 따른 것이다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. PM 동기 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 아래 오류를 그대로 올려보내며, 그때 상태는 OFF 로 확정된다.
 *
 * 호출 체인:
 *   pciehp_handle_presence_or_link_change() → [이 함수]
 *     → pm_runtime_get_sync() → __pciehp_enable_slot() → pm_runtime_put()
 */
static int pciehp_enable_slot(struct controller *ctrl)
{
	int ret;

	pm_runtime_get_sync(&ctrl->pcie->port->dev);
	ret = __pciehp_enable_slot(ctrl);
	/* [한국어] 실패했고 주의 버튼이 있는 슬롯이면 — 버튼으로 켜기를 시도한 사용자에게
	 * 결과를 보여 줘야 한다. */
	if (ret && ATTN_BUTTN(ctrl))
		/* may be blinking */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
				      INDICATOR_NOOP);
	pm_runtime_put(&ctrl->pcie->port->dev);

	mutex_lock(&ctrl->state_lock);
	ctrl->state = ret ? OFF_STATE : ON_STATE;
	/* [한국어] 상태를 확정했으니 잠금을 놓는다. */
	mutex_unlock(&ctrl->state_lock);

	return ret;
}

/* [한국어]
 * __pciehp_disable_slot - 꺼도 되는 상태인지 확인하고 끈다
 *
 * @ctrl: 대상 컨트롤러.
 * @safe_removal: 계획된 제거인지.
 * @return: 0 = 성공, -EINVAL = 이미 꺼져 있음.
 *
 * __pciehp_enable_slot() 의 짝이며 확인이 하나뿐이다 — 이미 꺼져 있으면
 * -EINVAL 로 답한다.
 *
 * 켜기 쪽이 "이미 켜져 있음" 을 0 으로 답하는 것과 대비된다 — 대칭인 두
 * 상황을 상류 코드가 서로 다른 값으로 다루고 있다. sysfs 경로에서는 이
 * 차이가 드러나지 않는데, 그쪽은 상태 기계에서 먼저 걸러 여기까지 오지
 * 않기 때문이다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. 프로세스 컨텍스트.
 *
 * 에러 경로: 이미 꺼져 있으면 -EINVAL 이며 기록을 남긴다.
 *
 * 호출 체인:
 *   pciehp_disable_slot() → [이 함수]
 *     → pciehp_get_power_status() → remove_board()
 */
static int __pciehp_disable_slot(struct controller *ctrl, bool safe_removal)
{
	u8 getstatus = 0;

	if (POWER_CTRL(ctrl)) {
		/* [한국어] 전원 상태를 읽는다. */
		pciehp_get_power_status(ctrl, &getstatus);
		/* [한국어] 이미 꺼져 있으면, */
		if (!getstatus) {
			/* [한국어] 기록을 남기고, */
			ctrl_info(ctrl, "Slot(%s): Already disabled\n",
				  /* [한국어] 어느 슬롯인지 함께 남긴다. */
				  slot_name(ctrl));
			return -EINVAL;
		}
	}

	remove_board(ctrl, safe_removal);
	/* [한국어] 제거가 끝났다. remove_board() 는 반환값이 없어 여기서는 언제나 성공이다. */
	return 0;
}

/* [한국어]
 * pciehp_disable_slot - 런타임 PM 을 붙잡고 슬롯을 끈 뒤 상태를 갱신한다
 *
 * @ctrl: 대상 컨트롤러.
 * @safe_removal: 계획된 제거인지.
 * @return: 0 = 성공, 음수 오류.
 *
 * pciehp_enable_slot() 과 같은 구조이며 두 가지가 다르다.
 *
 * 첫째, 표시등을 손대지 않는다. 끄기는 remove_board() 가 이미 표시등을
 * 정리하기 때문이다.
 *
 * 둘째, 결과와 무관하게 상태를 OFF 로 확정한다. 켜기 쪽이 성공 여부로
 * 갈리는 것과 다른데, 끄기가 실패했다는 것은 "이미 꺼져 있었다" 는 뜻이라
 * 어느 쪽이든 결과는 꺼진 상태이기 때문이다.
 *
 * 실행 컨텍스트: IRQ 스레드 또는 sysfs 쓰기. PM 동기 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 아래 오류를 올려보내지만 상태는 OFF 로 확정된다.
 *
 * 호출 체인:
 *   pciehp_handle_disable_request() / pciehp_handle_presence_or_link_change()
 *     → [이 함수]
 *     → pm_runtime_get_sync() → __pciehp_disable_slot() → pm_runtime_put()
 */
static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal)
{
	int ret;

	pm_runtime_get_sync(&ctrl->pcie->port->dev);
	ret = __pciehp_disable_slot(ctrl, safe_removal);
	/* [한국어] 런타임 PM 참조를 놓는다. 이제 포트가 절전에 들어가도 된다. */
	pm_runtime_put(&ctrl->pcie->port->dev);

	mutex_lock(&ctrl->state_lock);
	ctrl->state = OFF_STATE;
	/* [한국어] 상태를 확정했으니 잠금을 놓는다. */
	mutex_unlock(&ctrl->state_lock);

	return ret;
}

/* [한국어]
 * pciehp_sysfs_enable_slot - sysfs 의 켜기 요청을 IRQ 스레드에 넘기고 기다린다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * sysfs 에서 직접 켜지 않고 요청만 낸 뒤 기다리는 것이 이 함수의 핵심이다.
 * 그렇게 해야 IRQ 스레드가 낸 요청과 사용자가 낸 요청이 한 줄로 직렬화된다.
 *
 * 기다림의 조건이 둘이다 — 대기 이벤트가 모두 비었고, IRQ 스레드가 돌고
 * 있지 않아야 한다. 앞의 조건만으로는 부족한데, 스레드가 이벤트를 이미
 * 가져가 처리 중이면 대기열은 비어 있어도 작업은 끝나지 않았기 때문이다.
 *
 * request_result 를 미리 -ENODEV 로 두는 것이 안전장치다. 처리하는 쪽이
 * 그 값을 덮어쓰지 못한 채로 끝나면 실패로 보고된다.
 *
 * BLINKINGON 이나 OFF 일 때만 요청을 낸다. 그 밖의 상태는 이미 켜져 있거나
 * 켜는 중이라 기록만 남기고 성공으로 답한다.
 *
 * 여기서도 켜기 요청에 PDC 를 쓴다. 처리 코드가 "카드가 꽂혔다" 이벤트와
 * 하나이기 때문이다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 쓰기. wait_event 가 있어 잠들 수 있다.
 *
 * 에러 경로: 처리 결과가 request_result 로 전달된다.
 *
 * 호출 체인:
 *   echo 1 > .../power → pci_hotplug_core.c 의 power_write_file()
 *     → ops->enable_slot == [이 함수]
 *     → pciehp_request() → wait_event()
 */
int pciehp_sysfs_enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 켜기를 기다리던 중이거나, */
	case BLINKINGON_STATE:
	/* [한국어] 꺼져 있으면 — 이 둘만이 켜기 요청을 낼 수 있는 상태다. */
	case OFF_STATE:
		mutex_unlock(&ctrl->state_lock);
		/*
		 * The IRQ thread becomes a no-op if the user pulls out the
		 * card before the thread wakes up, so initialize to -ENODEV.
		 */
		ctrl->request_result = -ENODEV;
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		/* [한국어] IRQ 스레드가 처리를 마칠 때까지 기다린다. */
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		return ctrl->request_result;
	/* [한국어] 이미 켜는 중이면, */
	case POWERON_STATE:
		/* [한국어] 기록만 남긴다. 같은 요청을 또 낼 이유가 없다. */
		ctrl_info(ctrl, "Slot(%s): Already in powering on state\n",
			  slot_name(ctrl));
		break;
	case BLINKINGOFF_STATE:
	/* [한국어] 이미 켜져 있는 경우도 마찬가지다. */
	case ON_STATE:
	case POWEROFF_STATE:
		ctrl_info(ctrl, "Slot(%s): Already enabled\n",
			  slot_name(ctrl));
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);

	return -ENODEV;
}

/* [한국어]
 * pciehp_sysfs_disable_slot - sysfs 의 끄기 요청을 IRQ 스레드에 넘기고 기다린다
 *
 * @hotplug_slot: 공용 코어가 준 슬롯.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * pciehp_sysfs_enable_slot() 의 짝이며 구조가 같다 — 직접 끄지 않고 요청만
 * 낸 뒤 IRQ 스레드가 처리하기를 기다린다.
 *
 * 켜기 쪽과 다른 점이 하나 있다. request_result 를 미리 채우지 않는다.
 * 켜기 쪽은 요청을 내기 직전에 -ENODEV 로 두는데, 옆의 상류 주석이 그
 * 이유를 밝힌다 — IRQ 스레드가 깨어나기 전에 사용자가 카드를 뽑아 버리면
 * 그 스레드가 아무 일도 하지 않고 끝나, 아무도 결과를 쓰지 않기 때문이다.
 * 여기에는 그 대비가 없어, 그런 경우 앞선 요청이 남긴 값이 그대로 읽힌다.
 *
 * 요청을 내지 못한 나머지 상태는 모두 -ENODEV 로 끝난다. "이미 꺼져 있다" 도,
 * "끄는 중이다" 도, 알 수 없는 상태도 같은 값이라 호출자가 구분하지 못한다.
 * 이 점은 켜기 쪽도 같다.
 *
 * 기다림의 조건은 켜기 쪽과 같다 — 대기 이벤트가 비었고 IRQ 스레드가 돌고
 * 있지 않아야 한다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 쓰기. wait_event 가 있어 잠들 수 있다.
 *
 * 에러 경로: 요청을 낸 경우 처리 결과가 request_result 로 전달되고,
 * 그 밖에는 -ENODEV 가 나간다.
 *
 * 호출 체인:
 *   echo 0 > .../power → pci_hotplug_core.c 의 power_write_file()
 *     → ops->disable_slot == [이 함수]
 *     → pciehp_request(DISABLE_SLOT) → wait_event()
 */
int pciehp_sysfs_disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	/* [한국어] 끄기를 기다리던 중이거나, */
	case BLINKINGOFF_STATE:
	/* [한국어] 켜져 있으면 — 이 둘만이 끄기 요청을 낼 수 있는 상태다. */
	case ON_STATE:
		mutex_unlock(&ctrl->state_lock);
		pciehp_request(ctrl, DISABLE_SLOT);
		/* [한국어] IRQ 스레드가 처리를 마칠 때까지 기다린다. 조건이 둘인 이유는 대기열이 비어도
		 * 스레드가 이미 가져간 일을 처리 중일 수 있기 때문이다. */
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		return ctrl->request_result;
	/* [한국어] 이미 끄는 중이면, */
	case POWEROFF_STATE:
		/* [한국어] 기록만 남긴다. */
		ctrl_info(ctrl, "Slot(%s): Already in powering off state\n",
			  slot_name(ctrl));
		break;
	case BLINKINGON_STATE:
	/* [한국어] 이미 꺼져 있는 경우도 마찬가지다. */
	case OFF_STATE:
	case POWERON_STATE:
		ctrl_info(ctrl, "Slot(%s): Already disabled\n",
			  slot_name(ctrl));
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);

	return -ENODEV;
}
