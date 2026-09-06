// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file contains power management functions related to interrupts.
 */
/*
 * [한국어 설명] 시스템 절전과 복귀에서 인터럽트를 다루는 코드 (pm.c)
 *
 * === 파일의 역할 ===
 * 시스템 전체가 잠들 때(suspend/hibernate) 인터럽트를 멈추고, 깨어날 때
 * 되살린다. 그런데 전부 멈출 수는 없다 — 시스템을 깨울 인터럽트는 살아
 * 있어야 하기 때문이다. 그 구분이 이 파일의 핵심이다.
 *
 * 인터럽트는 절전 시점에 네 부류로 갈린다:
 *
 *   깨우기 원천(wakeup)      — 살려 둔다. 이것이 들어오면 시스템이 깨어난다.
 *   NO_SUSPEND              — 살려 둔다. 절전 중에도 동작해야 하는 것들
 *                              (타이머, IPI, 일부 컨트롤러의 부모 인터럽트).
 *   쓰이지 않거나 연쇄       — 건드리지 않는다. 멈출 것이 없다.
 *   나머지 전부              — 멈춘다. 대부분의 장치 인터럽트가 여기 해당한다.
 *
 * 멈춘 인터럽트에 들어온 것을 잃지 않는 방법도 이 파일이 다룬다. 처리는
 * 미루되 IRQS_PENDING 으로 기록해 두고, 깨어날 때 재전송한다.
 *
 * 복귀 쪽도 두 단계로 나뉜다. 일부 인터럽트(IRQF_EARLY_RESUME)는 다른
 * 장치가 깨어나기 전에 먼저 살아나야 한다 — 그 장치들의 부모 컨트롤러가
 * 그렇다. 그래서 syscore 단계에서 한 번, 보통의 장치 복귀 단계에서 또 한 번
 * 되살린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 시스템 절전 절차의 양 끝에 있다:
 *
 *   절전:
 *     suspend_devices_and_enter() → dpm_suspend_noirq()
 *       ↓
 *     suspend_device_irqs()             ← **이 파일**
 *       ↓ 인터럽트마다
 *     suspend_device_irq() → __disable_irq() 또는 wakeup 무장
 *
 *   깨어남:
 *     syscore_resume()
 *       ↓
 *     irq_pm_syscore_resume() → resume_irqs(true)   ← **이 파일** (EARLY 만)
 *       ↓ 그 뒤 장치 복귀 단계에서
 *     resume_device_irqs() → resume_irqs(false)     ← **이 파일** (나머지)
 *
 *   절전 중 깨우기 인터럽트 도착:
 *     흐름 제어 핸들러가 IRQD_WAKEUP_ARMED 를 보고
 *       ↓
 *     irq_pm_handle_wakeup()            ← **이 파일**
 *       ↓ 그 인터럽트를 끄고 PM 코어에 알린다
 *     pm_system_irq_wakeup() → 절전 절차 중단
 *
 * 실행 컨텍스트: 절전과 복귀 경로는 프로세스 문맥이며 다른 CPU 가 이미
 * 멈춘 상태다. handle_wakeup 만 인터럽트 문맥에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — __disable_irq/__enable_irq, mask_irq, IRQS_* 비트들.
 *   linux/suspend.h — pm_system_irq_wakeup(), PM 코어에 깨우기를 알린다.
 *   linux/syscore_ops.h — 장치 복귀보다 이른 단계에 끼어들기 위한 기반.
 *
 * 이 파일에 의존하는 곳:
 *   drivers/base/power/main.c 의 절전·복귀 절차.
 *   chip.c 의 흐름 제어 핸들러 — 깨우기 인터럽트가 오면 handle_wakeup 을 부른다.
 *   manage.c — 핸들러를 등록·해제할 때 install/remove_action 을 부른다.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_pm_handle_wakeup()   — 절전 중 깨우기 인터럽트가 도착했을 때의 처리.
 * irq_pm_install_action()  — 핸들러 등록 시 절전 정책 카운터를 갱신한다.
 * irq_pm_remove_action()   — 해제 시 그것을 되돌린다.
 * suspend_device_irq()     — 인터럽트 하나를 절전 상태로 만든다.
 * suspend_device_irqs()    — 모든 인터럽트에 대해 위를 부른다.
 * resume_irq()             — 인터럽트 하나를 되살린다.
 * resume_irqs()            — EARLY 여부로 걸러 위를 부른다.
 * rearm_wake_irq()         — 깨우기를 알린 인터럽트를 다시 무장시킨다.
 * irq_pm_syscore_resume()  — 이른 복귀 단계의 진입점.
 * resume_device_irqs()     — 보통의 복귀 단계의 진입점.
 *
 * 세 개의 깊이 카운터(no_suspend_depth, cond_suspend_depth,
 * force_resume_depth)가 이 파일의 정책을 담는다. 공유 인터럽트에서는
 * 여러 드라이버가 서로 다른 요구를 하므로 개수를 세어야 한다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc 와 irqd_* 접근자 */
#include <linux/module.h>	/* [한국어] 모듈 관련 기본 정의 */
#include <linux/interrupt.h>	/* [한국어] IRQF_* 플래그와 synchronize_irq() */
#include <linux/suspend.h>	/* [한국어] pm_system_irq_wakeup() — PM 코어에 깨우기 원인을 알린다 */
#include <linux/syscore_ops.h>	/* [한국어] syscore 단계 등록. 장치 복귀보다 이른 시점에 끼어들기 위한 것이다 */

#include "internals.h"	/* [한국어] __enable_irq, mask_irq, IRQS_* 등 코어 내부 */

/*
 * [한국어]
 * irq_pm_handle_wakeup - 절전 중 깨우기 인터럽트가 도착했을 때의 처리
 *
 * @desc: 그 인터럽트의 서술자.
 *
 * 절전 중에 살려 둔 깨우기 인터럽트가 실제로 들어오면 흐름 제어 핸들러가
 * IRQD_WAKEUP_ARMED 를 보고 이 함수를 부른다.
 *
 * 다섯 가지를 순서대로 한다:
 *   1. 무장을 해제한다 — 한 번 깨웠으면 그것으로 임무를 다했다.
 *   2. SUSPENDED 와 PENDING 을 세운다 — 이 인터럽트는 아직 처리되지 않았고
 *      복귀 후에 처리되어야 한다.
 *   3. depth 를 올리고 실제로 끈다 — 깨어나는 동안 또 들어오면 안 된다.
 *   4. PM 코어에 알린다 — 절전 절차를 중단하고 깨어나게 한다.
 *
 * 왜 인터럽트를 끄는가: 이 시점에 시스템은 아직 절전 중이라 드라이버가
 * 동작할 수 없다. 핸들러를 부를 수 없으므로 인터럽트를 멈춰 두고, 깨어난
 * 뒤 PENDING 을 보고 재전송해 정상 경로로 처리한다.
 *
 * depth++ 와 irq_disable() 을 함께 하는 것에 주의한다. depth 는 비활성
 * 중첩 깊이이고, 복귀 때 __enable_irq() 가 그것을 되돌린다. 짝이 맞아야
 * 인터럽트가 다시 열린다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   흐름 제어 핸들러 (kernel/irq/chip.c) → [이 함수]
 *     → pm_system_irq_wakeup() → PM 코어의 절전 중단
 */
void irq_pm_handle_wakeup(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_WAKEUP_ARMED);	/* [한국어] 무장을 푼다. 한 번 깨웠으면 임무를 다한 것이라, 다시 무장하려면 rearm_wake_irq() 를 불러야 한다 */
	desc->istate |= IRQS_SUSPENDED | IRQS_PENDING;	/* [한국어] 절전 상태이며 처리되지 않은 인터럽트가 밀려 있음을 기록한다. PENDING 이 있어야 복귀 때 재전송된다 */
	desc->depth++;	/* [한국어] 비활성 깊이를 올린다. 복귀 때 __enable_irq() 가 이것을 되돌려야 인터럽트가 다시 열린다 */
	irq_disable(desc);	/* [한국어] 실제로 끈다. 아직 절전 중이라 드라이버 핸들러를 부를 수 없으므로 더 들어오지 못하게 막는다 */
	pm_system_irq_wakeup(irq_desc_get_irq(desc));	/* [한국어] PM 코어에 이 번호가 깨움 원인임을 알린다. 절전 절차가 중단되고 시스템이 깨어난다 */
}

/*
 * Called from __setup_irq() with desc->lock held after @action has
 * been installed in the action chain.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * irq_pm_install_action - 핸들러 등록에 맞춰 절전 정책 카운터를 갱신한다
 *
 * @desc:   대상 서술자.
 * @action: 방금 목록에 추가된 핸들러.
 *
 * 왜 카운터를 세는가: 하나의 인터럽트 선을 여러 드라이버가 공유할 수 있고,
 * 각자 절전에 대한 요구가 다르다. 한 드라이버가 "절전 중에도 살려 달라"
 * (IRQF_NO_SUSPEND)고 하면 그 선 전체가 살아 있어야 하므로, 그런 요구를
 * 한 핸들러가 몇 개인지 세어야 한다.
 *
 * 세 카운터의 뜻:
 *   nr_actions          — 등록된 핸들러의 총 개수.
 *   force_resume_depth  — IRQF_FORCE_RESUME 을 요구한 핸들러 수.
 *   no_suspend_depth    — IRQF_NO_SUSPEND 를 요구한 핸들러 수.
 *   cond_suspend_depth  — IRQF_COND_SUSPEND 를 요구한 핸들러 수.
 *
 * 두 개의 WARN 이 정합성을 지킨다. 각각의 이유는 아래 주석에 있으며,
 * 요약하면 "일부만 요구하는 상태는 성립하지 않는다"는 것이다.
 *
 * 실행 컨텍스트: 요청 경로, 서술자 락을 쥔 상태(위 영어 주석 참고).
 *
 * 호출 체인:
 *   request_threaded_irq() → __setup_irq() (kernel/irq/manage.c) → [이 함수]
 */
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions++;	/* [한국어] 이 선에 붙은 핸들러 총 개수. 아래 두 WARN 의 기준이 된다 */

	if (action->flags & IRQF_FORCE_RESUME)	/* [한국어] 복귀 때 무조건 다시 켜 달라는 요구인가 */
		desc->force_resume_depth++;	/* [한국어] 그런 핸들러의 수를 센다 */

	/* [한국어] FORCE_RESUME 은 전부이거나 전무여야 한다.
	 *
	 * 왜인가: 이 플래그는 "절전으로 꺼졌든 아니든 복귀 때 무조건 켜라"는
	 * 뜻이다. 같은 선에 그것을 요구하지 않는 핸들러가 섞여 있으면, 그
	 * 드라이버는 아직 준비되지 않았는데 인터럽트가 열린다.
	 *
	 * ONCE 판인 이유: 이 조건이 깨지면 그 선에 핸들러를 붙일 때마다
	 * 경고가 나 로그를 채운다. 한 번만 알려도 충분하다. */
	WARN_ON_ONCE(desc->force_resume_depth &&	/* [한국어] 요구한 핸들러가 하나라도 있는데 */
		     desc->force_resume_depth != desc->nr_actions);	/* [한국어] 전부가 아니면 모순이다 */

	/* [한국어] NO_SUSPEND 와 COND_SUSPEND 는 배타적이다.
	 *
	 *   NO_SUSPEND   — "절전 중에도 이 선을 살려 달라". 타이머나 부모
	 *     컨트롤러처럼 절전 절차 자체가 의존하는 인터럽트가 쓴다.
	 *   COND_SUSPEND — "다른 핸들러가 NO_SUSPEND 를 요구하면 나도 따르겠다".
	 *     그 선을 공유하는 처지라 어쩔 수 없이 깨어 있게 되는 드라이버가 쓴다.
	 *
	 * 한 핸들러가 둘 다 요구할 수는 없으므로 else if 다. */
	if (action->flags & IRQF_NO_SUSPEND)	/* [한국어] 절전 중에도 살려 달라는 요구인가 */
		desc->no_suspend_depth++;	/* [한국어] 그 수를 센다. 하나라도 있으면 이 선은 절전되지 않는다 */
	else if (action->flags & IRQF_COND_SUSPEND)	/* [한국어] 조건부로 따르겠다는 요구인가 */
		desc->cond_suspend_depth++;	/* [한국어] 그 수를 따로 센다 */

	/* [한국어] NO_SUSPEND 를 요구한 핸들러가 있으면, 나머지는 모두
	 * COND_SUSPEND 여야 한다.
	 *
	 * 왜인가: NO_SUSPEND 때문에 그 선은 절전 중에도 살아 있게 된다. 그
	 * 사실을 모르는 드라이버가 같은 선에 붙어 있으면, 자기는 잠들었다고
	 * 믿는데 인터럽트가 들어와 준비되지 않은 상태를 만진다.
	 *
	 * COND_SUSPEND 는 "그럴 수 있음을 알고 있다"는 선언이다. 그것을 붙이지
	 * 않은 핸들러가 섞여 있으면 위험한 조합이므로 경고한다. */
	WARN_ON_ONCE(desc->no_suspend_depth &&	/* [한국어] 절전 면제를 요구한 핸들러가 있는데 */
		     (desc->no_suspend_depth + desc->cond_suspend_depth) != desc->nr_actions);	/* [한국어] 그 사실을 아는(둘 중 하나를 붙인) 핸들러가 전부가 아니면 위험하다 */
}

/*
 * Called from __free_irq() with desc->lock held after @action has
 * been removed from the action chain.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * irq_pm_remove_action - 핸들러 해제에 맞춰 절전 정책 카운터를 되돌린다
 *
 * @desc:   대상 서술자.
 * @action: 방금 목록에서 빠진 핸들러.
 *
 * 위 install 의 짝이며, 정확히 반대로 센다. install 과 대칭이어야 카운터가
 * 어긋나지 않는다 — 어긋나면 절전 정책이 잘못 적용되어, 꺼야 할 인터럽트가
 * 살아 있거나 그 반대가 된다.
 *
 * install 에 있던 두 WARN 이 여기에는 없다는 점이 눈에 띈다. 해제 중에는
 * 카운터가 일시적으로 모순된 상태를 지날 수 있기 때문이다. 예를 들어
 * NO_SUSPEND 핸들러 하나와 COND_SUSPEND 핸들러 하나가 붙어 있을 때
 * 전자를 먼저 떼면, 잠시 "COND_SUSPEND 만 있는" 상태가 된다. 그것을
 * 경고하면 정상적인 해제 순서에서도 경고가 난다.
 *
 * 실행 컨텍스트: 해제 경로, 서술자 락을 쥔 상태(위 영어 주석 참고).
 *
 * 호출 체인:
 *   free_irq() → __free_irq() (kernel/irq/manage.c) → [이 함수]
 */
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions--;	/* [한국어] 총 개수를 되돌린다 */

	if (action->flags & IRQF_FORCE_RESUME)	/* [한국어] install 과 정확히 같은 조건으로 판정해야 카운터가 어긋나지 않는다 */
		desc->force_resume_depth--;	/* [한국어] 되돌린다 */

	if (action->flags & IRQF_NO_SUSPEND)	/* [한국어] install 의 else if 구조를 그대로 따라야 한다 */
		desc->no_suspend_depth--;	/* [한국어] 되돌린다 */
	else if (action->flags & IRQF_COND_SUSPEND)	/* [한국어] 배타적 관계도 그대로 */
		desc->cond_suspend_depth--;	/* [한국어] 되돌린다 */
}

/*
 * [한국어]
 * suspend_device_irq - 인터럽트 하나를 절전 상태로 만든다
 *
 * @desc:   대상 서술자.
 * @return: 호출자가 synchronize_irq() 를 불러야 하면 참. 건드리지 않았으면 거짓.
 *
 * 반환값의 뜻이 특이하다. "성공했는가"가 아니라 "동기화가 필요한가"이며,
 * 그 이유는 아래 wakeup 분기의 주석에 있다.
 *
 * 세 갈래로 나뉜다:
 *
 *   1. 건드리지 않는 경우 — 핸들러가 없거나, 연쇄 인터럽트이거나,
 *      NO_SUSPEND 를 요구한 핸들러가 붙어 있는 경우다. 거짓을 돌려준다.
 *
 *   2. 깨우기 원천인 경우 — 끄지 않고 IRQD_WAKEUP_ARMED 로 무장한다.
 *      이 인터럽트가 들어오면 위 irq_pm_handle_wakeup() 이 시스템을 깨운다.
 *      일부 하드웨어에서는 꺼져 있던 것을 일부러 켜기까지 한다.
 *
 *   3. 나머지 — SUSPENDED 를 세우고 끈다. 필요하면 chip 수준에서 마스크까지 한다.
 *
 * 연쇄 인터럽트를 건드리지 않는 이유: 그것은 하위 컨트롤러로 가는 통로다.
 * 통로를 막으면 그 아래의 깨우기 인터럽트까지 함께 막힌다. 대신 하위
 * 인터럽트들이 각자 절전되며, 그중 깨우기가 있으면 통로는 열려 있어야 한다.
 *
 * 실행 컨텍스트: 절전 경로, 서술자 락을 쥔 상태. 다른 CPU 는 이미 멈춰 있다.
 *
 * 호출 체인:
 *   suspend_device_irqs() → [이 함수] → __disable_irq()/mask_irq()
 */
static bool suspend_device_irq(struct irq_desc *desc)
{
	unsigned long chipflags = irq_desc_get_chip(desc)->flags;	/* [한국어] chip 의 성질 플래그. 아래 두 곳에서 절전 동작을 가른다 */
	struct irq_data *irqd = &desc->irq_data;	/* [한국어] 상태 비트를 다룰 대상 */

	/* [한국어] 건드리지 말아야 할 세 경우.
	 *
	 *   action 이 없음 — 아무도 쓰지 않는 인터럽트다. 끌 것도 없다.
	 *   연쇄 인터럽트  — 하위 컨트롤러로 가는 통로다. 막으면 그 아래의
	 *     깨우기 인터럽트까지 함께 막힌다.
	 *   no_suspend_depth 가 0 이 아님 — 절전 중에도 살려 달라는 핸들러가
	 *     붙어 있다. 타이머나 부모 컨트롤러가 그렇다. */
	if (!desc->action || irq_desc_is_chained(desc) ||	/* [한국어] 쓰이지 않거나 하위 컨트롤러로 가는 통로인가 */
	    desc->no_suspend_depth)	/* [한국어] 절전 면제를 요구한 핸들러가 있는가 */
		return false;	/* [한국어] 손대지 않았으므로 동기화도 필요 없다 */

	if (irqd_is_wakeup_set(irqd)) {	/* [한국어] 사용자나 드라이버가 이 인터럽트를 깨우기 원천으로 지정했는가 */
		irqd_set(irqd, IRQD_WAKEUP_ARMED);	/* [한국어] 무장한다. 이 비트를 본 흐름 제어 핸들러가 irq_pm_handle_wakeup() 을 부른다 */

		/* [한국어] 일부 하드웨어는 깨우기 인터럽트가 켜져 있어야만 깨울 수 있다.
		 *
		 * 무슨 상황인가: 드라이버가 disable_irq() 로 꺼 둔 채 절전에 들어갈
		 * 수 있다. 보통의 컨트롤러는 그래도 깨우기 회로가 따로 있어 상관없지만,
		 * IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND 를 선언한 chip 은 인터럽트가
		 * 실제로 언마스크되어 있어야 깨울 수 있다.
		 *
		 * 그래서 일부러 켜 주고, 켰다는 사실을 IRQD_IRQ_ENABLED_ON_SUSPEND
		 * 로 기록해 둔다. 복귀 때 resume_irq() 가 그 기록을 보고 원래 상태로
		 * 되돌린다 — 그러지 않으면 드라이버가 꺼 둔 인터럽트가 깨어난 뒤
		 * 열린 채로 남는다. */
		if ((chipflags & IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND) &&	/* [한국어] 켜져 있어야 깨울 수 있는 하드웨어인가 */
		     irqd_irq_disabled(irqd)) {	/* [한국어] 그런데 지금 꺼져 있는가 */
			/*
			 * Interrupt marked for wakeup is in disabled state.
			 * Enable interrupt here to unmask/enable in irqchip
			 * to be able to resume with such interrupts.
			 */
			/* [한국어] (위 영어 주석) 깨우기용으로 표시된 인터럽트가 꺼져 있다.
			 * 깨어날 수 있도록 여기서 켜 준다. */
			__enable_irq(desc);	/* [한국어] 켠다. depth 를 내리고 하드웨어를 언마스크한다 */
			irqd_set(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);	/* [한국어] "우리가 켰다"고 기록한다. 복귀 때 이 기록을 보고 되돌린다 */
		}
		/*
		 * We return true here to force the caller to issue
		 * synchronize_irq(). We need to make sure that the
		 * IRQD_WAKEUP_ARMED is visible before we return from
		 * suspend_device_irqs().
		 */
		/* [한국어] (위 영어 주석에 이어) 참을 돌려주는 진짜 이유.
		 *
		 * 이 인터럽트는 끄지 않았으므로 "동기화"가 필요 없어 보인다.
		 * 그런데도 참을 돌려주는 것은 메모리 가시성 때문이다.
		 *
		 * 지금 다른 CPU 에서 이 인터럽트의 핸들러가 돌고 있을 수 있다.
		 * 그 핸들러는 IRQD_WAKEUP_ARMED 를 세우기 전의 상태를 보고 있어,
		 * 깨우기 처리를 하지 않고 그냥 지나간다. suspend_device_irqs() 가
		 * 돌아온 뒤에도 그 상황이 남아 있으면 깨우기 신호를 놓친다.
		 *
		 * synchronize_irq() 는 진행 중인 핸들러가 끝나기를 기다린다. 그
		 * 뒤에 시작하는 핸들러는 반드시 무장된 상태를 본다. */
		return true;	/* [한국어] 끄지는 않았지만 동기화는 필요하다 — 무장 상태가 모든 CPU 에 보여야 한다 */
	}

	desc->istate |= IRQS_SUSPENDED;	/* [한국어] 절전 상태로 표시한다. 이 비트가 서 있으면 복귀 때 되살릴 대상이 된다 */
	__disable_irq(desc);	/* [한국어] 끈다. depth 를 올리고, DISABLE_UNLAZY 가 아니면 하드웨어는 건드리지 않는다 */

	/*
	 * Hardware which has no wakeup source configuration facility
	 * requires that the non wakeup interrupts are masked at the
	 * chip level. The chip implementation indicates that with
	 * IRQCHIP_MASK_ON_SUSPEND.
	 */
	/* [한국어] (위 영어 주석에 이어) 깨우기 설정 기능이 없는 하드웨어를 위한 처리.
	 *
	 * 무슨 문제인가: 위 __disable_irq() 는 게으른 비활성화라 하드웨어를
	 * 건드리지 않는다. 소프트웨어 표시만 남기고, 인터럽트가 실제로 들어왔을
	 * 때 그때 마스크한다.
	 *
	 * 그런데 어떤 컨트롤러는 "어느 인터럽트가 깨우기인가"를 하드웨어에
	 * 설정할 수단이 없다. 그런 하드웨어에서는 마스크되지 않은 인터럽트가
	 * 들어오면 그것이 무엇이든 시스템을 깨운다. 깨우기가 아닌 인터럽트가
	 * 시스템을 깨우면 절전이 무의미해진다.
	 *
	 * 그래서 그런 chip 은 IRQCHIP_MASK_ON_SUSPEND 를 선언하고, 여기서
	 * 실제로 마스크해 준다. */
	if (chipflags & IRQCHIP_MASK_ON_SUSPEND)	/* [한국어] 깨우기 설정 기능이 없어 실제 마스크가 필요한 chip 인가 */
		mask_irq(desc);	/* [한국어] 하드웨어를 실제로 마스크한다. 게으른 비활성화만으로는 부족하다 */
	return true;	/* [한국어] 껐으므로 진행 중인 핸들러가 끝나기를 기다려야 한다 */
}

/**
 * suspend_device_irqs - disable all currently enabled interrupt lines
 *
 * During system-wide suspend or hibernation device drivers need to be
 * prevented from receiving interrupts and this function is provided
 * for this purpose.
 *
 * So we disable all interrupts and mark them IRQS_SUSPENDED except
 * for those which are unused, those which are marked as not
 * suspendable via an interrupt request with the flag IRQF_NO_SUSPEND
 * set and those which are marked as active wakeup sources.
 *
 * The active wakeup sources are handled by the flow handler entry
 * code which checks for the IRQD_WAKEUP_ARMED flag, suspends the
 * interrupt and notifies the pm core about the wakeup.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * suspend_device_irqs - 시스템 절전을 위해 모든 인터럽트를 멈춘다
 *
 * 인자도 반환값도 없다. 시스템의 모든 인터럽트를 대상으로 한다.
 *
 * 위 suspend_device_irq() 를 모든 서술자에 대해 부르고, 참을 돌려준 것에
 * 대해 synchronize_irq() 를 부른다.
 *
 * 중첩 스레드 인터럽트를 건너뛰는 이유: 그것들은 자체 하드웨어 선이 없고
 * 부모의 스레드 핸들러가 대신 불러 준다. 부모가 절전되면 자식도 자연히
 * 멈추므로, 따로 손댈 것이 없다. 오히려 손대면 부모와 상태가 어긋난다.
 *
 * 락 구조에 주의: synchronize_irq() 를 서술자 락 밖에서 부른다. 그 함수는
 * 진행 중인 핸들러가 끝나기를 기다리는데, 그 핸들러가 같은 서술자 락을
 * 잡으려 하므로 락을 쥔 채 부르면 교착한다.
 *
 * 실행 컨텍스트: 절전 경로, 프로세스 문맥. 이 시점에 다른 CPU 는 이미
 * 멈췄지만 인터럽트는 아직 들어올 수 있다.
 *
 * 호출 체인:
 *   dpm_suspend_noirq() (drivers/base/power/main.c) → [이 함수]
 *     → suspend_device_irq() / synchronize_irq()
 */
void suspend_device_irqs(void)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	int irq;	/* [한국어] 그 인터럽트 번호. synchronize_irq() 가 번호를 받는다 */

	for_each_irq_desc(irq, desc) {	/* [한국어] 모든 서술자를 훑는다 */
		bool sync;	/* [한국어] 이 인터럽트에 대해 동기화가 필요한가 */

		if (irq_settings_is_nested_thread(desc))	/* [한국어] 부모 스레드 안에서 도는 중첩 인터럽트인가 */
			continue;	/* [한국어] 부모가 절전되면 함께 멈춘다. 따로 손대면 상태가 어긋난다 */
		scoped_guard(raw_spinlock_irqsave, &desc->lock)	/* [한국어] 서술자 락. 블록이 한 문장이라 중괄호가 없다 */
			sync = suspend_device_irq(desc);	/* [한국어] 실제 절전 처리. 반환값은 "동기화가 필요한가"다 */

		if (sync)	/* [한국어] 락 밖에서 확인한다 */
			synchronize_irq(irq);	/* [한국어] 진행 중인 핸들러가 끝나기를 기다린다. 락을 쥔 채 부르면 그 핸들러가 같은 락을 기다려 교착한다 */
	}
}

/*
 * [한국어]
 * resume_irq - 인터럽트 하나를 절전에서 되살린다
 *
 * @desc: 대상 서술자.
 *
 * 위 suspend_device_irq() 의 짝이며, 그것이 남긴 세 가지 흔적을 각각 되돌린다.
 *
 *   IRQD_WAKEUP_ARMED         — 무조건 지운다. 절전이 끝났으므로 무장을 푼다.
 *   IRQD_IRQ_ENABLED_ON_SUSPEND — 우리가 일부러 켰던 것을 되돌린다.
 *   IRQS_SUSPENDED            — 절전으로 껐던 것을 다시 켠다.
 *
 * 여기에 더해 IRQF_FORCE_RESUME 처리가 있다. 절전으로 꺼진 것이 아닌데도
 * 복귀 때 무조건 켜 달라는 요구인데, 그것을 처리하는 방식이 이 함수에서
 * 가장 독특한 부분이다 — 아래 그 자리의 주석에 설명이 있다.
 *
 * goto resume 로 두 경로를 합치는 구조에 주의한다. SUSPENDED 였으면 곧바로
 * 되살리고, 아니면 FORCE_RESUME 조건을 따진 뒤 같은 자리로 합류한다.
 *
 * 실행 컨텍스트: 복귀 경로, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   resume_irqs() → [이 함수] → __enable_irq()/__disable_irq()
 */
static void resume_irq(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;	/* [한국어] 상태 비트를 다룰 대상 */

	irqd_clear(irqd, IRQD_WAKEUP_ARMED);	/* [한국어] 무장을 푼다. 절전이 끝났으므로 이제 깨우기 처리가 필요 없다 */

	if (irqd_is_enabled_on_suspend(irqd)) {	/* [한국어] 절전에 들어갈 때 우리가 일부러 켰던 인터럽트인가 */
		/*
		 * Interrupt marked for wakeup was enabled during suspend
		 * entry. Disable such interrupts to restore them back to
		 * original state.
		 */
		/* [한국어] (위 영어 주석에 이어) 원래 상태로 되돌린다.
		 *
		 * suspend_device_irq() 에서 IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND 인
		 * chip 의 꺼져 있던 깨우기 인터럽트를 일부러 켰다. 드라이버는
		 * 그것이 꺼져 있다고 믿고 있으므로, 복귀하면 다시 꺼야 한다.
		 *
		 * 이 처리를 빠뜨리면 드라이버가 disable_irq() 로 꺼 둔 인터럽트가
		 * 깨어난 뒤 열린 채로 남아, 준비되지 않은 핸들러가 불린다. */
		__disable_irq(desc);	/* [한국어] 우리가 올린 것을 되돌린다. depth 가 다시 오른다 */
		irqd_clear(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);	/* [한국어] 기록도 지운다 */
	}

	if (desc->istate & IRQS_SUSPENDED)	/* [한국어] 절전으로 꺼졌던 인터럽트인가 */
		goto resume;	/* [한국어] 되살리러 간다. 아래 FORCE_RESUME 조건을 따질 필요가 없다 */

	/* Force resume the interrupt? */
	/* [한국어] (위 영어 주석) 절전되지 않았는데도 강제로 켜야 하는가.
	 *
	 * 여기 도달했다는 것은 이 인터럽트가 절전 대상이 아니었다는 뜻이다
	 * (NO_SUSPEND 였거나 깨우기 원천이었거나 쓰이지 않았다).
	 *
	 * 그런데 IRQF_FORCE_RESUME 을 요구한 핸들러가 붙어 있으면, 그 요구는
	 * "절전 여부와 무관하게 복귀 때 반드시 켜라"는 뜻이다. 절전 절차 중에
	 * 다른 경로가 이 인터럽트를 껐을 수 있어, 그것까지 되돌리려는 것이다. */
	if (!desc->force_resume_depth)	/* [한국어] 강제 복귀를 요구한 핸들러가 없으면 */
		return;	/* [한국어] 손댈 이유가 없다 */

	/* Pretend that it got disabled ! */
	/* [한국어] (위 영어 주석) "꺼진 것처럼" 상태를 꾸며 낸다.
	 *
	 * 이것이 이 함수에서 가장 헷갈리는 부분이다. 왜 켜기 전에 끄는 흉내를 내는가?
	 *
	 * 아래 __enable_irq() 는 depth 를 하나 내리고, 0 이 되면 실제로 켠다.
	 * 그런데 이 인터럽트는 절전으로 꺼진 적이 없어 depth 가 이미 0 이다.
	 * 그대로 __enable_irq() 를 부르면 depth 가 음수가 되어 경고가 나고
	 * 상태가 망가진다.
	 *
	 * 그래서 depth 를 하나 올리고 상태 비트도 "꺼짐"으로 맞춰, 아래
	 * __enable_irq() 가 정상적인 짝을 이루게 한다. 결과적으로 하드웨어는
	 * 실제로 언마스크되고 depth 는 다시 0 이 된다.
	 *
	 * 상태 비트 둘을 함께 세우는 이유: __enable_irq() 가 그 비트들을 보고
	 * 실제로 무엇을 되돌릴지 정하기 때문이다. depth 만 올리면 하드웨어를
	 * 건드리지 않고 지나간다. */
	desc->depth++;	/* [한국어] 아래 __enable_irq() 가 내릴 몫을 미리 올려 둔다. 없으면 depth 가 음수가 된다 */
	irq_state_set_disabled(desc);	/* [한국어] 논리적으로 꺼진 상태로 표시한다 */
	irq_state_set_masked(desc);	/* [한국어] 하드웨어도 마스크된 것으로 표시한다. 그래야 __enable_irq 이 실제로 언마스크한다 */
resume:	/* [한국어] 두 경로(절전으로 꺼진 것 / 강제 복귀를 요구한 것)가 여기서 합류한다 */
	desc->istate &= ~IRQS_SUSPENDED;	/* [한국어] 절전 표시를 지운다. 두 경로가 여기서 합류한다 */
	__enable_irq(desc);	/* [한국어] 켠다. depth 를 내리고 0 이 되면 하드웨어를 언마스크하며, IRQS_PENDING 이 있으면 재전송까지 발행한다 */
}

/*
 * [한국어]
 * resume_irqs - 인터럽트들을 되살린다. EARLY 여부로 걸러 낸다
 *
 * @want_early: 참이면 IRQF_EARLY_RESUME 인 것만, 거짓이면 전부 처리한다.
 *
 * 복귀가 두 단계로 나뉘는 이유가 이 인자에 담겨 있다.
 *
 * 어떤 인터럽트는 다른 장치가 깨어나기 전에 먼저 살아나야 한다. 대표적으로
 * 다른 컨트롤러의 부모가 되는 인터럽트다 — 그것이 죽어 있으면 그 아래의
 * 모든 장치가 인터럽트를 받지 못해 복귀가 진행되지 않는다.
 *
 * 그래서 IRQF_EARLY_RESUME 을 요구한 것들은 syscore 단계(장치 복귀보다
 * 훨씬 이른 시점)에 먼저 켠다. 나머지는 보통의 장치 복귀 단계에서 켠다.
 *
 * want_early 가 거짓일 때 EARLY 인 것도 함께 처리한다는 점에 주의한다.
 * 이미 켜진 것을 또 켜는 셈인데, resume_irq() 가 IRQS_SUSPENDED 를 보고
 * 판단하므로 두 번째 호출은 아무 일도 하지 않는다 — 첫 호출이 그 비트를
 * 지웠기 때문이다.
 *
 * 실행 컨텍스트: 복귀 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_pm_syscore_resume() → [이 함수] (want_early = true)
 *   resume_device_irqs() → [이 함수] (want_early = false)
 */
static void resume_irqs(bool want_early)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	int irq;	/* [한국어] 그 인터럽트 번호. 이 함수에서는 쓰이지 않지만 순회 매크로가 요구한다 */

	for_each_irq_desc(irq, desc) {	/* [한국어] 모든 서술자를 훑는다 */
		bool is_early = desc->action &&	desc->action->flags & IRQF_EARLY_RESUME;	/* [한국어] 첫 핸들러만 본다 — 공유 인터럽트에서 나머지가 다르게 요구하는 경우는 없다고 전제한다 */

		if (!is_early && want_early)	/* [한국어] 이른 복귀 단계인데 EARLY 가 아니면 */
			continue;	/* [한국어] 나중 단계로 미룬다 */
		if (irq_settings_is_nested_thread(desc))	/* [한국어] 중첩 인터럽트는 절전 때도 건너뛰었다 */
			continue;	/* [한국어] 부모가 살아나면 함께 살아난다 */

		guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 서술자 락. guard 라 이 반복의 끝에서 풀린다 */
		resume_irq(desc);	/* [한국어] 실제로 되살린다 */
	}
}

/**
 * rearm_wake_irq - rearm a wakeup interrupt line after signaling wakeup
 * @irq: Interrupt to rearm
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * rearm_wake_irq - 깨우기를 알린 인터럽트를 다시 무장시킨다
 *
 * @irq: 다시 무장할 인터럽트 번호.
 *
 * 무슨 상황인가: 절전 중에 깨우기 인터럽트가 들어와 위
 * irq_pm_handle_wakeup() 이 시스템을 깨우려 했다. 그런데 PM 코어가 그
 * 깨우기를 무시하기로 결정할 수 있다 — 예를 들어 절전 진입이 이미 너무
 * 진행되어 되돌릴 수 없거나, 그 깨우기가 유효하지 않다고 판단한 경우다.
 *
 * 그러면 시스템은 계속 잠들어 있는데, 그 인터럽트는 무장이 풀리고 꺼진
 * 상태로 남는다. 다음에 진짜 깨워야 할 일이 생겨도 깨우지 못한다.
 *
 * 이 함수가 그 상태를 되돌린다. 절전 표시를 지우고 다시 무장한 뒤 켠다 —
 * 곧 handle_wakeup() 이 한 일을 정확히 되감는다.
 *
 * 두 조건을 확인하는 이유: 절전 상태가 아니거나 애초에 깨우기 원천이
 * 아닌 인터럽트를 무장시키면 안 된다. 잘못된 호출에 대한 방어다.
 *
 * 실행 컨텍스트: PM 코어의 절전 경로, 프로세스 문맥. 버스 락 때문에
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   PM 코어의 깨우기 무시 결정 → [이 함수] → __enable_irq()
 */
void rearm_wake_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] 서술자를 찾아 락과 버스 락을 잡는다. I2C 뒤의 칩일 수 있어 버스 락이 필요하다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] guard 가 들고 있는 포인터. NULL 이면 블록이 실행되지 않는다 */

		if (!(desc->istate & IRQS_SUSPENDED) || !irqd_is_wakeup_set(&desc->irq_data))	/* [한국어] 절전 상태가 아니거나 깨우기 원천이 아니면 */
			return;	/* [한국어] 무장할 대상이 아니다. guard 가 락을 풀어 준다 */

		desc->istate &= ~IRQS_SUSPENDED;	/* [한국어] handle_wakeup 이 세운 절전 표시를 지운다 */
		irqd_set(&desc->irq_data, IRQD_WAKEUP_ARMED);	/* [한국어] 다시 무장한다. 이제 이 인터럽트가 들어오면 또 깨우기를 시도한다 */
		__enable_irq(desc);	/* [한국어] handle_wakeup 이 올린 depth 를 되돌리고 켠다. 셋이 합쳐 그 함수를 정확히 되감는다 */
	}
}

/**
 * irq_pm_syscore_resume - enable interrupt lines early
 * @data: syscore context
 *
 * Enable all interrupt lines with %IRQF_EARLY_RESUME set.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_pm_syscore_resume - 이른 복귀 단계에서 EARLY 인터럽트만 켠다
 *
 * @data: syscore 문맥. 이 구현은 쓰지 않는다 — 전역 상태만 다루기 때문이다.
 *
 * syscore 단계가 무엇인가: 시스템 복귀 절차에서 개별 장치가 깨어나기
 * 훨씬 전, CPU 와 핵심 서브시스템만 살아난 시점이다. 여기서 처리해야
 * 하는 것은 다른 모든 것이 의존하는 기반이다.
 *
 * 왜 인터럽트 일부를 여기서 켜는가: 다른 컨트롤러의 부모가 되는 인터럽트가
 * 죽어 있으면, 그 아래 장치들이 복귀하면서 내는 인터럽트가 전달되지 않는다.
 * 장치 복귀가 인터럽트를 기다리며 멈춘다.
 *
 * 실행 컨텍스트: syscore 복귀 단계. 인터럽트가 꺼진 단일 CPU 문맥이다.
 *
 * 호출 체인:
 *   syscore_resume() (kernel/power/) → ops->resume → [이 함수]
 *     → resume_irqs(true)
 */
static void irq_pm_syscore_resume(void *data)
{
	resume_irqs(true);	/* [한국어] EARLY 로 표시된 것만 켠다. 나머지는 장치 복귀 단계로 미룬다 */
}

/* [한국어] syscore 단계에 끼어들기 위한 연산표.
 *
 * resume 만 있고 suspend 가 없는 것이 눈에 띈다. 절전 쪽은
 * suspend_device_irqs() 가 장치 절전 단계에서 이미 처리하므로 syscore
 * 단계에 할 일이 없다.
 *
 * 비대칭인 이유: 절전은 "늦게 꺼도 되지만" 복귀는 "일찍 켜야" 하기
 * 때문이다. 부모 인터럽트는 자식들이 모두 잠든 뒤에 꺼도 늦지 않지만,
 * 자식들이 깨어나기 전에는 켜져 있어야 한다. */
static const struct syscore_ops irq_pm_syscore_ops = {
	.resume		= irq_pm_syscore_resume,
	/* [한국어] 이른 복귀 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: syscore_resume().
	 * suspend 짝이 없는 이유는 위 블록 주석 참고. */
};

/* [한국어] 위 연산표를 담아 syscore 에 등록할 객체.
 *
 * ops 를 구조체로 한 번 더 감싸는 것은 최근 커널의 syscore 등록 방식이다.
 * 등록 목록의 고리 같은 내부 상태를 이 구조체가 들고 있어, ops 자체는
 * const 로 둘 수 있다. */
static struct syscore irq_pm_syscore = {
	.ops = &irq_pm_syscore_ops,
	/* [한국어] 위 연산표를 가리킨다.
	 * 설정자: 이 정적 초기화. 읽는 자: syscore 코어가 복귀 때 따라간다.
	 * 이 구조체는 const 가 아니다 — syscore 코어가 등록 목록의 고리를
	 *   여기에 넣기 때문이다. */
};

/*
 * [한국어]
 * irq_pm_init_ops - 위 syscore 객체를 등록한다
 *
 * @return: 항상 0. initcall 이 int 를 요구해 형식만 맞춘다.
 *
 * 부팅 중 한 번 불려, 이 파일의 이른 복귀 처리를 syscore 목록에 넣는다.
 * 그 뒤로는 시스템이 복귀할 때마다 위 irq_pm_syscore_resume() 이 불린다.
 *
 * device_initcall 로 등록하는 이유: 인터럽트 서브시스템 자체는 이보다
 * 훨씬 일찍 초기화되지만, syscore 등록은 그 기반이 준비된 뒤라면 언제든
 * 상관없다. 첫 절전이 일어나기 전이기만 하면 된다.
 *
 * 실행 컨텍스트: 부팅 중 initcall, 프로세스 문맥.
 *
 * 호출 체인:
 *   do_initcalls() → [이 함수] → register_syscore()
 */
static int __init irq_pm_init_ops(void)
{
	register_syscore(&irq_pm_syscore);	/* [한국어] syscore 복귀 목록에 등록한다. 해제하는 코드가 없다 — 커널이 살아 있는 동안 유지된다 */
	return 0;	/* [한국어] initcall 의 형식. 실패할 수 있는 단계가 없다 */
}

device_initcall(irq_pm_init_ops);	/* [한국어] 위 함수를 device 초기화 단계의 initcall 로 등록한다. 첫 절전 전에만 실행되면 되므로 이른 단계일 필요가 없다 */

/**
 * resume_device_irqs - enable interrupt lines disabled by suspend_device_irqs()
 *
 * Enable all non-%IRQF_EARLY_RESUME interrupt lines previously
 * disabled by suspend_device_irqs() that have the IRQS_SUSPENDED flag
 * set as well as those with %IRQF_FORCE_RESUME.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * resume_device_irqs - 절전으로 멈춘 인터럽트들을 되살린다
 *
 * 인자도 반환값도 없다. 위 suspend_device_irqs() 의 짝이다.
 *
 * resume_irqs(false) 한 줄이 전부인 얇은 껍데기다. 이름을 따로 두는 이유는
 * 이것이 PM 코어에 공개되는 API 이고, resume_irqs 는 내부 구현이기 때문이다.
 * 위 irq_pm_syscore_resume() 과 짝을 이루어, 같은 내부 함수를 다른 인자로 부른다.
 *
 * kernel-doc 이 말하는 두 대상:
 *   IRQS_SUSPENDED 가 서 있는 것 — 절전으로 꺼진 것들.
 *   IRQF_FORCE_RESUME 을 요구한 것 — 절전되지 않았어도 무조건 켜야 하는 것들.
 * 후자의 처리가 resume_irq() 의 "꺼진 척하기" 구간이다.
 *
 * EARLY 인 것도 함께 훑지만, 이미 syscore 단계에서 켜져 IRQS_SUSPENDED 가
 * 지워졌으므로 아무 일도 일어나지 않는다.
 *
 * 실행 컨텍스트: 장치 복귀 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   dpm_resume_noirq() (drivers/base/power/main.c) → [이 함수]
 *     → resume_irqs(false)
 */
void resume_device_irqs(void)
{
	resume_irqs(false);	/* [한국어] EARLY 를 가리지 않고 전부 훑는다. 이미 켜진 것은 IRQS_SUSPENDED 가 없어 그냥 지나간다 */
}
