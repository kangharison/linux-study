// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */
/*
 * [한국어 설명] 드라이버 핸들러를 실제로 부르는 코어 (handle.c)
 *
 * === 파일의 역할 ===
 * 인터럽트가 도착해 흐름 제어까지 마친 뒤, 등록된 드라이버 핸들러들을
 * 차례로 부르고 그 결과를 집계한다. 인터럽트 처리 경로에서 드라이버 코드에
 * 가장 가까운 층이다.
 *
 * 하는 일이 셋이다:
 *   핸들러 호출  — action 목록을 순회하며 각 드라이버의 handler 를 부른다.
 *   결과 집계    — IRQ_HANDLED/IRQ_NONE/IRQ_WAKE_THREAD 를 OR 로 모은다.
 *   스레드 깨우기 — WAKE_THREAD 를 돌려준 핸들러의 스레드를 깨운다.
 *
 * 공유 인터럽트가 이 파일의 구조를 정한다. 한 선에 여러 드라이버가 붙어
 * 있으면 그중 누구의 장치가 울렸는지 알 수 없으므로, 전부 불러 보고 각자
 * "내 것이었다(IRQ_HANDLED)" 또는 "아니었다(IRQ_NONE)" 를 답하게 한다.
 * 아무도 자기 것이라 하지 않으면 오탐이며, spurious.c 가 그것을 센다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 처리의 한가운데에 있다:
 *
 *   하드웨어 인터럽트 → 아키텍처 진입점
 *     ↓ generic_handle_arch_irq()      ← **이 파일** (일부 아키텍처)
 *   generic_handle_irq() → desc->handle_irq
 *     ↓
 *   흐름 제어 핸들러 (handle_level_irq 등)  — chip.c
 *     ↓ 마스크·ack 를 방식에 맞게 처리한 뒤
 *   handle_irq_event()                 ← **이 파일**
 *     ↓ 서술자 락을 놓고
 *   handle_irq_event_percpu()          ← **이 파일**
 *     ↓
 *   __handle_irq_event_percpu()        ← **이 파일** — 실제 순회
 *     ↓
 *   드라이버의 handler(irq, dev_id)
 *     ↓ IRQ_WAKE_THREAD 를 돌려주면
 *   __irq_wake_thread()                ← **이 파일**
 *
 * 실행 컨텍스트: 전부 하드 인터럽트 문맥이다. 잠들 수 없고, 인터럽트가
 * 꺼진 상태를 유지해야 한다 — 아래 WARN_ONCE 가 그것을 감시한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — for_each_action_of_desc, IRQS_*, irq_settings_*.
 *   spurious.c 의 note_interrupt() — 결과를 넘겨 오탐 감지에 쓴다.
 *   random.c 의 add_interrupt_randomness() — 인터럽트 타이밍을 엔트로피로 쓴다.
 *   trace/events/irq.h — ftrace 이벤트.
 *
 * 이 파일에 의존하는 곳:
 *   chip.c 의 모든 흐름 제어 핸들러가 handle_irq_event() 를 부른다.
 *   manage.c 가 __irq_wake_thread() 의 짝인 스레드 쪽을 구현한다.
 *   아키텍처 진입 코드가 set_handle_irq()/generic_handle_arch_irq() 를 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * handle_arch_irq            — 아키텍처의 최상위 인터럽트 진입점을 담는 전역.
 * handle_bad_irq()           — 처리할 수 없는 인터럽트가 왔을 때의 흐름 제어.
 * no_action()               — 아무것도 하지 않는 핸들러. 선을 예약만 할 때 쓴다.
 * __irq_wake_thread()       — 스레드 핸들러를 깨운다. 이 파일에서 가장 미묘한 함수.
 * irqhandler_duration_check()— 핸들러가 너무 오래 돌면 경고한다.
 * __handle_irq_event_percpu()— action 목록을 순회하며 핸들러를 부른다.
 * handle_irq_event_percpu() — 거기에 엔트로피 수집과 오탐 감지를 더한다.
 * handle_irq_event()        — 거기에 락 해제·재획득과 상태 관리를 더한다.
 * generic_handle_arch_irq() — 아키텍처를 위한 최상위 래퍼.
 *
 * 세 겹의 handle_irq_event 계열이 있는 이유: per-CPU 인터럽트는 서술자 락을
 * 잡지 않으므로 가장 안쪽만 쓰고, 보통의 인터럽트는 바깥까지 쓴다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc 와 irqd_* 접근자 */
#include <linux/random.h>	/* [한국어] add_interrupt_randomness() — 인터럽트 타이밍을 난수 엔트로피로 쓴다 */
#include <linux/sched.h>	/* [한국어] wake_up_state(), PF_EXITING — 스레드 핸들러를 깨울 때 필요하다 */
#include <linux/interrupt.h>	/* [한국어] irqreturn_t 와 IRQ_* 반환값, IRQF_* 플래그 */
#include <linux/kernel_stat.h>	/* [한국어] 인터럽트 횟수 통계 */

#include <asm/irq_regs.h>	/* [한국어] set_irq_regs() — 인터럽트 시점의 레지스터를 저장해 프로파일러가 쓸 수 있게 한다 */

#include <trace/events/irq.h>	/* [한국어] trace_irq_handler_entry/exit — ftrace 로 핸들러 실행을 추적한다 */

#include "internals.h"	/* [한국어] for_each_action_of_desc, note_interrupt 등 코어 내부 */

/* [한국어] 아키텍처의 최상위 인터럽트 진입점.
 *
 * 무엇인가: 하드웨어 인터럽트가 발생하면 CPU 가 아키텍처의 예외 벡터로
 * 뛰고, 그 코드가 최종적으로 이 포인터를 따라간다. 여기서부터 인터럽트
 * 컨트롤러 드라이버가 어느 인터럽트인지 알아내 코어로 넘긴다.
 *
 * 왜 포인터인가: 하나의 커널 이미지가 여러 인터럽트 컨트롤러를 지원해야
 * 한다. 어느 것인지는 부팅 때 장치 트리를 보고 정해지므로, 컴파일 시점에
 * 함수를 못 박을 수 없다.
 *
 * __ro_after_init 이 붙어 있는 것이 중요하다. 부팅 중 한 번 채워진 뒤에는
 * 읽기 전용 페이지로 바뀌어, 이후 누구도 고칠 수 없다. 이 포인터를 바꿀
 * 수 있으면 모든 인터럽트를 가로챌 수 있어 커널 공격의 표적이 된다.
 *
 * MULTI_HANDLER 를 쓰지 않는 아키텍처(x86 등)는 자기 진입 코드에서 직접
 * 처리하므로 이 전역이 아예 없다. */
#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER	/* [한국어] 진입점을 실행 중에 정하는 아키텍처인가 */
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init;	/* [한국어] 부팅 뒤 읽기 전용이 된다 — 이 포인터를 고칠 수 있으면 모든 인터럽트를 가로챌 수 있다 */
#endif

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * handle_bad_irq - 처리할 수 없는 인터럽트가 왔을 때의 흐름 제어 핸들러
 *
 * @desc: 그 인터럽트의 서술자.
 *
 * 다른 흐름 제어 핸들러(handle_level_irq 등)와 같은 자리에 꽂히지만, 하는
 * 일은 정반대다 — 처리하는 대신 이것이 잘못된 인터럽트임을 알린다.
 *
 * 언제 꽂히는가: 서술자가 만들어질 때의 기본값이다. 컨트롤러 드라이버가
 * 제대로 된 흐름 제어를 꽂아 주기 전에는 이것이 남아 있고, 그 상태에서
 * 인터럽트가 오면 무언가 잘못된 것이다.
 *
 * 세 가지를 한다:
 *   서술자를 덤프해 진단 정보를 남긴다 (자체 ratelimit 이 있다).
 *   통계를 올린다 — /proc/interrupts 에 나타나야 사용자가 알아챈다.
 *   아키텍처에 알린다 — x86 이면 APIC 에 EOI 를 보낸다.
 *
 * 마지막이 중요하다. 잘못된 인터럽트라고 무시하면 컨트롤러가 그것을
 * "처리 중" 으로 여겨 같은 우선순위의 인터럽트가 전부 막힌다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   generic_handle_irq() → desc->handle_irq → [이 함수]
 *     → print_irq_desc() (kernel/irq/debug.h) → ack_bad_irq() (아키텍처)
 */
void handle_bad_irq(struct irq_desc *desc)
{
	unsigned int irq = irq_desc_get_irq(desc);	/* [한국어] 서술자에서 번호를 꺼낸다. 아래 두 곳에서 쓴다 */

	print_irq_desc(irq, desc);	/* [한국어] 서술자를 통째로 콘솔에 찍는다. 그 함수에 ratelimit 이 있어 폭주해도 안전하다 */
	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 통계를 올린다. /proc/interrupts 에 나타나야 사용자가 이상을 알아챈다 */
	ack_bad_irq(irq);	/* [한국어] 아키텍처에 알린다. x86 은 APIC 에 EOI 를 보내며, 빠뜨리면 같은 우선순위의 인터럽트가 전부 막힌다 */
}
EXPORT_SYMBOL_GPL(handle_bad_irq);	/* [한국어] 컨트롤러 드라이버가 모듈일 수 있어 공개한다. 흐름 제어를 임시로 이것으로 되돌리는 경우가 있다 */

/*
 * Special, empty irq handler:
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * no_action - 아무 일도 하지 않는 인터럽트 핸들러
 *
 * @cpl:    인터럽트 번호. 이름이 cpl 인 것은 아주 오래된 흔적이다.
 * @dev_id: 등록 때 준 장치 문맥. 쓰이지 않는다.
 * @return: 항상 IRQ_NONE.
 *
 * 왜 이런 것이 필요한가: 인터럽트 선을 "예약" 하고 싶을 때가 있다. 실제로
 * 처리할 것은 없지만 그 번호를 다른 드라이버가 가져가지 못하게 막거나,
 * 공유 인터럽트에서 자기 몫의 자리를 잡아 두는 경우다.
 *
 * request_irq() 는 핸들러가 NULL 이면(스레드 핸들러도 없으면) 거절하므로,
 * 형식상 핸들러가 필요하다. 이 함수가 그 자리를 채운다.
 *
 * IRQ_NONE 을 돌려주는 것에 주의: "내 것이 아니다" 라는 뜻이라, 이 핸들러만
 * 붙은 선에 인터럽트가 계속 들어오면 오탐으로 집계되어 "nobody cared" 로
 * 꺼질 수 있다. 그래서 실제로 인터럽트가 오는 선에는 쓰면 안 된다.
 *
 * 인자 이름 cpl 은 "current privilege level" 의 줄임으로, 1990년대 초의
 * 인터럽트 API 에서 온 이름이다. 지금은 인터럽트 번호가 들어온다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   __handle_irq_event_percpu() → action->handler → [이 함수]
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;	/* [한국어] "내 것이 아니다". 실제로 인터럽트가 오는 선에 이것만 붙이면 오탐으로 집계된다 */
}
EXPORT_SYMBOL_GPL(no_action);	/* [한국어] 드라이버가 선을 예약할 때 쓸 수 있게 공개한다 */

/*
 * [한국어]
 * warn_no_thread - 스레드 핸들러 없이 IRQ_WAKE_THREAD 를 돌려준 드라이버를 경고한다
 *
 * @irq:    그 인터럽트 번호.
 * @action: 잘못 답한 핸들러.
 *
 * 드라이버 버그다. 1차 핸들러가 "스레드에서 마저 처리하겠다" 고 답했는데
 * 등록된 스레드 함수가 없으면, 그 인터럽트는 아무도 처리하지 않은 채 끝난다.
 *
 * 한 번만 경고하는 이유가 이 함수의 존재 이유다. 그 인터럽트가 초당 수천
 * 번 들어오면 경고가 로그를 가득 채워 시스템을 마비시킨다 — 진단하려던
 * 문제보다 큰 문제가 된다.
 *
 * test_and_set_bit 하나로 "이미 찍었는가" 확인과 "찍었다고 표시" 를 함께
 * 한다. 원자 연산이라 여러 CPU 가 동시에 들어와도 정확히 한 번만 찍힌다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   __handle_irq_event_percpu() → [이 함수]
 */
static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))	/* [한국어] 이미 찍었으면 참을 돌려주고, 아니면 표시하며 거짓을 준다 — 확인과 표시를 한 원자 연산으로 */
		return;	/* [한국어] 이미 알린 문제다. 폭주하는 인터럽트에서 로그가 넘치는 것을 막는다 */

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "	/* [한국어] 어느 인터럽트의 어느 장치가 잘못 답했는지 */
	       "but no thread function available.", irq, action->name);	/* [한국어] action->name 은 request_irq 에 준 이름이라 드라이버를 곧바로 특정할 수 있다 */
}

/*
 * [한국어]
 * __irq_wake_thread - 이 action 의 스레드 핸들러를 깨운다
 *
 * @desc:   대상 서술자.
 * @action: 1차 핸들러가 IRQ_WAKE_THREAD 를 돌려준 그 action.
 *
 * 이 파일에서 가장 미묘한 함수이며, 아래 긴 영어 주석 전체가 그 이유를
 * 설명한다. 요약하면 "락 없이 threads_oneshot 을 고쳐도 안전한 이유" 다.
 *
 * 네 단계로 이루어진다:
 *   1. 스레드가 죽는 중이면 물러난다.
 *   2. RUNTHREAD 를 세운다. 이미 서 있었으면 이미 깨웠다는 뜻이라 물러난다.
 *   3. threads_oneshot 에 이 스레드의 비트를 얹고 활성 카운터를 올린다.
 *   4. 스레드를 깨운다.
 *
 * threads_oneshot 이 무엇인가: ONESHOT 인터럽트에서 "아직 돌고 있는 스레드"
 * 들의 비트맵이다. 이것이 0 이 되어야 인터럽트를 다시 언마스크할 수 있다 —
 * 스레드가 장치의 원인을 지우기 전에 언마스크하면 폭풍이 나기 때문이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 서술자 락을 쥐지 않은 상태다
 * (handle_irq_event 가 놓고 들어왔다). 그 사실이 아래 주석의 전제다.
 *
 * 호출 체인:
 *   __handle_irq_event_percpu() → [이 함수] → wake_up_state()
 *     → 깨어난 irq_thread() (kernel/irq/manage.c)
 */
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action)
{
	/*
	 * In case the thread crashed and was killed we just pretend that
	 * we handled the interrupt. The hardirq handler has disabled the
	 * device interrupt, so no irq storm is lurking.
	 */
	/* [한국어] (위 영어 주석에 이어) 스레드가 죽는 중이면 처리한 척하고 넘어간다.
	 *
	 * 왜 안전한가: 1차 핸들러가 이미 장치의 인터럽트를 껐다(그것이 WAKE_THREAD
	 * 를 돌려주기 전에 해야 할 일이다). 그래서 스레드가 사라져도 인터럽트가
	 * 폭주하지는 않는다.
	 *
	 * 대신 그 인터럽트는 처리되지 않은 채 남는다. 하지만 스레드가 죽었다는
	 * 것 자체가 이미 비정상이고, 여기서 할 수 있는 일이 없다. */
	if (action->thread->flags & PF_EXITING)	/* [한국어] 스레드가 종료 중인가 */
		return;	/* [한국어] 깨울 대상이 없다. 1차 핸들러가 이미 장치를 껐으므로 폭주하지는 않는다 */

	/*
	 * Wake up the handler thread for this action. If the
	 * RUNTHREAD bit is already set, nothing to do.
	 */
	/* [한국어] (위 영어 주석) RUNTHREAD 가 이미 서 있으면 할 일이 없다.
	 *
	 * 무슨 뜻인가: 이전 인터럽트로 이미 깨워 두었고 스레드가 아직 그것을
	 * 처리하지 않았다. 스레드는 한 번 돌면서 쌓인 일을 모두 처리하므로,
	 * 또 깨울 필요가 없다.
	 *
	 * 이 검사가 아래 카운터 증가를 정확히 한 번만 일어나게 만든다 —
	 * 그것이 없으면 threads_active 가 실제 스레드 수보다 커져
	 * synchronize_irq() 가 영원히 기다린다. */
	if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))	/* [한국어] 확인과 표시를 한 원자 연산으로. 이미 서 있었으면 참 */
		return;	/* [한국어] 이미 깨워 두었다. 아래 카운터 증가를 건너뛰는 것이 핵심이다 */

	/*
	 * It's safe to OR the mask lockless here. We have only two
	 * places which write to threads_oneshot: This code and the
	 * irq thread.
	 *
	 * This code is the hard irq context and can never run on two
	 * cpus in parallel. If it ever does we have more serious
	 * problems than this bitmask.
	 *
	 * The irq threads of this irq which clear their "running" bit
	 * in threads_oneshot are serialized via desc->lock against
	 * each other and they are serialized against this code by
	 * IRQS_INPROGRESS.
	 *
	 * Hard irq handler:
	 *
	 *	spin_lock(desc->lock);
	 *	desc->state |= IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *	set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
	 *	desc->threads_oneshot |= mask;
	 *	spin_lock(desc->lock);
	 *	desc->state &= ~IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *
	 * irq thread:
	 *
	 * again:
	 *	spin_lock(desc->lock);
	 *	if (desc->state & IRQS_INPROGRESS) {
	 *		spin_unlock(desc->lock);
	 *		while(desc->state & IRQS_INPROGRESS)
	 *			cpu_relax();
	 *		goto again;
	 *	}
	 *	if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
	 *		desc->threads_oneshot &= ~mask;
	 *	spin_unlock(desc->lock);
	 *
	 * So either the thread waits for us to clear IRQS_INPROGRESS
	 * or we are waiting in the flow handler for desc->lock to be
	 * released before we reach this point. The thread also checks
	 * IRQTF_RUNTHREAD under desc->lock. If set it leaves
	 * threads_oneshot untouched and runs the thread another time.
	 */
	/* [한국어] (위 긴 영어 주석에 이어) 락 없이 이 비트맵을 고쳐도 되는 이유.
	 *
	 * 원 주석의 논증을 정리하면 이렇다.
	 *
	 * 이 비트맵을 쓰는 곳은 둘뿐이다 — 여기(하드 인터럽트 문맥)와 irq 스레드.
	 *
	 * 여기가 두 CPU 에서 동시에 돌 수는 없다. 하나의 인터럽트는 한 번에
	 * 한 CPU 에서만 처리되기 때문이다. 그렇지 않다면 이 비트맵보다 훨씬
	 * 심각한 문제가 있는 것이다(원 주석의 표현).
	 *
	 * 스레드들끼리는 desc->lock 으로 직렬화된다.
	 *
	 * 남는 것은 "여기 vs 스레드" 의 경쟁이고, 그것을 IRQS_INPROGRESS 가
	 * 막는다. 원 주석의 두 의사 코드가 그 순서를 보여 준다. 하드 인터럽트
	 * 쪽은 INPROGRESS 를 세운 뒤에야 이 비트맵을 만지고, 스레드는 락 아래에서
	 * INPROGRESS 를 확인해 서 있으면 기다린다.
	 *
	 * 마지막 문단이 또 하나의 안전장치다: 스레드가 락 아래에서 RUNTHREAD 를
	 * 확인해 서 있으면 비트맵을 건드리지 않고 한 번 더 돈다. 그래서 우리가
	 * 방금 세운 비트가 스레드에 의해 지워지는 일이 없다. */
	desc->threads_oneshot |= action->thread_mask;	/* [한국어] "이 스레드가 아직 돌고 있다" 는 표시. 이 비트맵이 0 이 되어야 ONESHOT 인터럽트를 언마스크할 수 있다 */

	/*
	 * We increment the threads_active counter in case we wake up
	 * the irq thread. The irq thread decrements the counter when
	 * it returns from the handler or in the exit path and wakes
	 * up waiters which are stuck in synchronize_irq() when the
	 * active count becomes zero. synchronize_irq() is serialized
	 * against this code (hard irq handler) via IRQS_INPROGRESS
	 * like the finalize_oneshot() code. See comment above.
	 */
	/* [한국어] (위 영어 주석에 이어) 돌고 있는 스레드의 수를 센다.
	 *
	 * 무엇을 위한 카운터인가: synchronize_irq() 가 "이 인터럽트의 모든
	 * 처리가 끝났다" 를 판정하는 근거다. 1차 핸들러뿐 아니라 스레드까지
	 * 끝나야 진짜로 끝난 것이므로, 스레드 수를 따로 세야 한다.
	 *
	 * 왜 중요한가: free_irq() 는 핸들러를 떼기 전에 synchronize_irq() 로
	 * 진행 중인 처리를 기다린다. 이 카운터가 정확하지 않으면 아직 도는
	 * 스레드가 있는데 action 을 반납해, 스레드가 해제된 메모리를 만진다.
	 *
	 * 위 RUNTHREAD 검사가 이 증가를 정확히 한 번만 일어나게 한다. 스레드는
	 * 핸들러에서 돌아올 때 한 번 내리므로, 둘의 짝이 맞는다. */
	atomic_inc(&desc->threads_active);	/* [한국어] synchronize_irq() 가 기다릴 대상의 수. 스레드가 끝나면서 내리고, 0 이 되면 대기자를 깨운다 */

	/*
	 * This might be a premature wakeup before the thread reached the
	 * thread function and set the IRQTF_READY bit. It's waiting in
	 * kthread code with state UNINTERRUPTIBLE. Once it reaches the
	 * thread function it waits with INTERRUPTIBLE. The wakeup is not
	 * lost in that case because the thread is guaranteed to observe
	 * the RUN flag before it goes to sleep in wait_for_interrupt().
	 */
	/* [한국어] (위 영어 주석에 이어) 아직 준비되지 않은 스레드를 깨워도 안전한 이유.
	 *
	 * 무슨 상황인가: request_threaded_irq() 가 스레드를 만들자마자 인터럽트가
	 * 들어올 수 있다. 그때 스레드는 아직 kthread 골격 안에서 시작을 기다리는
	 * 중이라, 이 깨우기가 그냥 사라지는 것처럼 보인다.
	 *
	 * 왜 유실되지 않는가: 그 시점의 스레드는 UNINTERRUPTIBLE 상태라 이
	 * 깨우기(TASK_INTERRUPTIBLE 대상)에 반응하지 않는다. 그러나 스레드가
	 * 자기 함수에 도달해 잠들기 전에 반드시 RUNTHREAD 비트를 확인하고,
	 * 우리가 이미 세워 두었으므로 잠들지 않고 곧바로 처리한다.
	 *
	 * 즉 "깨우기" 가 아니라 "비트" 가 실제 신호이며, 깨우기는 이미 잠든
	 * 스레드를 위한 보조 수단이다.
	 *
	 * TASK_INTERRUPTIBLE 을 지정하는 이유: 인터럽트 스레드는 그 상태로
	 * 잠들며, 그 상태의 태스크만 깨우면 다른 이유로 잠든 것을 잘못 깨우지 않는다. */
	wake_up_state(action->thread, TASK_INTERRUPTIBLE);	/* [한국어] 스레드를 깨운다. 실제 신호는 위 RUNTHREAD 비트이고 이것은 잠든 스레드를 위한 보조다 */
}

/* [한국어] 인터럽트 핸들러가 너무 오래 도는지 감시하는 기능.
 *
 * 왜 필요한가: 하드 인터럽트 핸들러는 인터럽트가 꺼진 채로 돈다. 오래
 * 돌면 그 동안 다른 인터럽트가 지연되고, 실시간 응답성이 무너진다.
 * 그런데 어느 드라이버가 범인인지 알아내기가 어렵다.
 *
 * 정적 분기(static key)를 쓰는 것이 요점이다. 부트 인자로 켜기 전까지는
 * 아래 검사 코드가 분기조차 하지 않는다 — 커널이 그 자리를 nop 으로
 * 채워 두었다가, 켤 때 점프 명령으로 바꿔 넣는다. 그래서 쓰지 않는
 * 시스템에서는 인터럽트 핫패스에 아무 비용도 없다. */
static DEFINE_STATIC_KEY_FALSE(irqhandler_duration_check_enabled);	/* [한국어] 기본은 꺼짐. 부트 인자로만 켤 수 있다 */
static u64 irqhandler_duration_threshold_ns __ro_after_init;	/* [한국어] 이 시간을 넘으면 경고한다. 부팅 뒤 읽기 전용이 되어 실행 중 바뀌지 않는다 */

/*
 * [한국어]
 * irqhandler_duration_check_setup - 부트 인자를 파싱해 감시를 켠다
 *
 * @arg:    "irqhandler.duration_warn_us=" 뒤에 온 문자열.
 * @return: 1 이면 인자를 소비했다, 0 이면 처리하지 못했다.
 *
 * 부트 인자 처리기의 반환값 규약이 특이하다. 1 이 성공이고 0 이 실패인데,
 * 실패해도 부팅은 계속된다 — 그 인자를 알아보지 못한 것으로 처리될 뿐이다.
 *
 * 두 가지를 검증한다: 숫자로 파싱되는가, 그리고 0 이 아닌가. 0 을 허용하면
 * 모든 핸들러가 경고를 내 로그가 폭주한다.
 *
 * 단위 변환에 주의: 인자는 마이크로초로 받고 내부에는 나노초로 담는다.
 * 아래 검사가 local_clock() 의 나노초 값과 비교하기 때문이다.
 *
 * static_branch_enable 이 마지막에 오는 것이 중요하다. 임계값을 먼저 담고
 * 그 다음에 분기를 켜야, 켜진 직후의 검사가 0 인 임계값을 보지 않는다.
 *
 * 실행 컨텍스트: 부팅 초기의 부트 인자 파싱. 프로세스 문맥.
 *
 * 호출 체인:
 *   parse_early_options() → __setup 표 → [이 함수]
 */
static int __init irqhandler_duration_check_setup(char *arg)
{
	unsigned long val;	/* [한국어] 파싱한 마이크로초 값 */
	int ret;	/* [한국어] 파싱 결과 */

	ret = kstrtoul(arg, 0, &val);	/* [한국어] 진법 0 이라 0x 접두사가 있으면 16진수로도 읽는다 */
	if (ret) {	/* [한국어] 숫자가 아닌 문자열이면 */
		pr_err("Unable to parse irqhandler.duration_warn_us setting: ret=%d\n", ret);	/* [한국어] 사용자가 오타를 알아챌 수 있게 알린다 */
		return 0;	/* [한국어] 인자를 처리하지 못했음을 알린다. 부팅은 계속된다 */
	}

	if (!val) {	/* [한국어] 0 을 허용하면 모든 핸들러가 임계값을 넘어 경고가 폭주한다 */
		pr_err("Invalid irqhandler.duration_warn_us setting, must be > 0\n");	/* [한국어] 왜 거부했는지 알린다 */
		return 0;	/* [한국어] 감시를 켜지 않는다 */
	}

	irqhandler_duration_threshold_ns = val * 1000;	/* [한국어] 마이크로초를 나노초로. 아래 검사가 local_clock() 의 나노초와 비교한다 */
	static_branch_enable(&irqhandler_duration_check_enabled);	/* [한국어] 임계값을 담은 뒤에 켠다 — 순서를 바꾸면 켜진 직후의 검사가 0 을 본다 */

	return 1;	/* [한국어] 인자를 소비했음을 알린다 */
}
__setup("irqhandler.duration_warn_us=", irqhandler_duration_check_setup);	/* [한국어] 이 접두사로 시작하는 부트 인자를 위 함수에 넘기도록 등록한다 */

/*
 * [한국어]
 * irqhandler_duration_check - 핸들러 실행 시간을 재고 임계값을 넘으면 경고한다
 *
 * @ts_start: 핸들러를 부르기 직전의 시각(나노초).
 * @irq:      그 인터럽트 번호.
 * @action:   그 핸들러. 경고에 함수 이름을 찍는 데 쓴다.
 *
 * 감시가 켜졌을 때만 불린다. 아래 호출부가 정적 분기로 감싸고 있어,
 * 꺼져 있으면 이 함수도 시각 측정도 아예 실행되지 않는다.
 *
 * local_clock() 을 쓰는 이유: CPU 로컬 클럭이라 읽기가 매우 싸다. 정확도는
 * 낮지만 마이크로초 단위의 임계값을 판정하는 데는 충분하고, 인터럽트
 * 문맥에서 부를 수 있다.
 *
 * %ps 형식이 중요하다. 핸들러 주소를 심볼 이름으로 찍어 주므로 어느
 * 드라이버가 오래 도는지 곧바로 알 수 있다.
 *
 * ratelimit 을 쓰는 이유: 오래 도는 핸들러는 매번 오래 돌므로, 제한이
 * 없으면 경고 자체가 시스템을 더 느리게 만든다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   __handle_irq_event_percpu() → [이 함수]
 */
static inline void irqhandler_duration_check(u64 ts_start, unsigned int irq,
					     const struct irqaction *action)
{
	u64 delta_ns = local_clock() - ts_start;	/* [한국어] 걸린 시간. local_clock 은 CPU 로컬이라 싸다 */

	if (unlikely(delta_ns > irqhandler_duration_threshold_ns)) {	/* [한국어] 넘는 경우가 드물다고 보고 분기 예측을 그쪽에 맞춘다 */
		pr_warn_ratelimited("[CPU%u] long duration of IRQ[%u:%ps], took: %llu us\n",	/* [한국어] %ps 가 핸들러 주소를 심볼 이름으로 찍어, 어느 드라이버인지 바로 드러난다 */
				    smp_processor_id(), irq, action->handler,	/* [한국어] 어느 CPU 에서, 어느 인터럽트의, 어느 핸들러가 */
				    div_u64(delta_ns, NSEC_PER_USEC));	/* [한국어] 나노초를 마이크로초로. 64비트 나눗셈을 32비트 아키텍처에서도 안전하게 하는 헬퍼다 */
	}
}

/*
 * [한국어]
 * __handle_irq_event_percpu - action 목록을 순회하며 드라이버 핸들러를 부른다
 *
 * @desc:   대상 서술자.
 * @return: 모든 핸들러의 반환값을 OR 로 모은 것. IRQ_HANDLED 가 섞여 있으면
 *          누군가는 처리한 것이고, IRQ_NONE 만 나오면 아무도 자기 것이라
 *          하지 않은 것이다.
 *
 * 이 파일의 알맹이다. 공유 인터럽트에서는 여러 드라이버가 한 선에 붙어
 * 있고, 그중 누구의 장치가 울렸는지 커널은 모른다. 그래서 전부 불러 보고
 * 각자 답하게 한다.
 *
 * 반환값을 OR 로 모으는 이유: 하나라도 IRQ_HANDLED 를 주면 그 인터럽트는
 * 처리된 것이다. IRQ_NONE 은 0 이므로 OR 에 영향을 주지 않아, 자연스럽게
 * "하나라도 처리했는가" 가 된다.
 *
 * 세 가지 부수 작업이 순회 안에 섞여 있다:
 *   lockdep 에 스레드화 가능성 알리기 — 아래 그 자리의 주석 참고.
 *   ftrace 이벤트 — 핸들러 진입과 종료를 추적한다.
 *   인터럽트 재활성화 감시 — 핸들러가 인터럽트를 켜 놓고 나가면 잡아낸다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 인터럽트가 꺼진 상태여야 하며,
 * 아래 WARN_ONCE 가 그 계약을 감시한다.
 *
 * 호출 체인:
 *   handle_irq_event_percpu() 또는 per-CPU 흐름 제어 → [이 함수]
 *     → action->handler → 드라이버 코드
 */
irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc)
{
	irqreturn_t retval = IRQ_NONE;	/* [한국어] 아무도 처리하지 않았다는 값에서 시작해 OR 로 쌓는다. IRQ_NONE 은 0 이라 OR 의 항등원이다 */
	unsigned int irq = desc->irq_data.irq;	/* [한국어] 인터럽트 번호. 핸들러의 첫 인자이자 추적 이벤트에 쓰인다 */
	struct irqaction *action;	/* [한국어] 순회 중인 핸들러 */

	for_each_action_of_desc(desc, action) {	/* [한국어] 이 선에 붙은 핸들러를 전부. 공유 인터럽트면 여럿이다 */
		irqreturn_t res;	/* [한국어] 이 핸들러의 답 */

		/*
		 * If this IRQ would be threaded under force_irqthreads, mark it so.
		 */
		/* [한국어] (위 영어 주석에 이어) lockdep 에 "이것은 스레드화될 수 있는
		 * 핸들러다" 라고 알린다.
		 *
		 * 왜 필요한가: lockdep 은 하드 인터럽트 문맥에서 잡는 락과 프로세스
		 * 문맥에서 잡는 락의 순서를 따져 교착 가능성을 찾는다. 그런데 이
		 * 핸들러는 threadirqs 커널에서는 스레드 문맥에서 돌 수 있다.
		 *
		 * 그 사실을 알리지 않으면 lockdep 이 "이 락은 하드 인터럽트 문맥
		 * 전용" 이라고 학습하고, 나중에 같은 락을 스레드에서 잡을 때 잘못된
		 * 경고를 낸다. 반대로 진짜 문제를 놓칠 수도 있다.
		 *
		 * 세 플래그를 제외하는 이유: NO_THREAD 는 명시적 거부이고, PERCPU 는
		 * 스레드화할 수 없으며, ONESHOT 은 이미 스레드 핸들러가 따로 있다. */
		if (irq_settings_can_thread(desc) &&	/* [한국어] 이 인터럽트가 강제 스레드화 대상인가 */
		    !(action->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT)))	/* [한국어] 세 예외 중 어느 것도 아닌가 */
			lockdep_hardirq_threaded();	/* [한국어] lockdep 에 이 문맥이 스레드일 수도 있음을 알린다. 잘못된 락 순서 경고를 막는다 */

		trace_irq_handler_entry(irq, action);	/* [한국어] ftrace 이벤트. 핸들러 진입 시각과 이름을 남긴다 */

		/* [한국어] 아래 분기가 정적 분기(static key)로 갈린다.
		 *
		 * 감시가 꺼져 있으면 컴파일된 코드에서 이 분기가 nop 이 되어,
		 * 곧바로 else 쪽 한 줄만 실행된다. 시각 측정은커녕 조건 검사도
		 * 하지 않는다.
		 *
		 * 인터럽트 핸들러 호출은 시스템에서 가장 자주 실행되는 경로 중
		 * 하나라, 쓰지 않는 기능에 단 한 번의 분기도 남기지 않으려는 것이다. */
		if (static_branch_unlikely(&irqhandler_duration_check_enabled)) {	/* [한국어] 부트 인자로 켜지 않았으면 이 분기 자체가 없다 */
			u64 ts_start = local_clock();	/* [한국어] 핸들러를 부르기 직전의 시각 */

			res = action->handler(irq, action->dev_id);	/* [한국어] 드라이버 핸들러. dev_id 는 request_irq 에 준 문맥이다 */
			irqhandler_duration_check(ts_start, irq, action);	/* [한국어] 오래 걸렸으면 경고한다 */
		} else {
			res = action->handler(irq, action->dev_id);	/* [한국어] 감시가 꺼진 경우 — 측정 없이 곧바로 부른다 */
		}

		trace_irq_handler_exit(irq, action, res);	/* [한국어] ftrace 이벤트. 반환값까지 남겨 어느 핸들러가 처리했는지 추적할 수 있다 */

		/* [한국어] 핸들러가 인터럽트를 켜 놓고 나갔는지 감시한다.
		 *
		 * 왜 문제인가: 하드 인터럽트 핸들러는 인터럽트가 꺼진 채로 돌아야
		 * 한다. 핸들러가 local_irq_enable() 을 부르거나 그런 효과가 있는
		 * 함수를 쓰면, 그 뒤로 재진입이나 스택 오버플로가 일어날 수 있다.
		 *
		 * 오래된 드라이버가 잠들 수 있는 함수를 잘못 쓰는 것이 흔한 원인이다.
		 *
		 * ONCE 판인 이유: 그 드라이버의 인터럽트마다 경고가 나면 로그가
		 * 넘친다. 한 번만 알리고, 대신 매번 인터럽트를 다시 꺼 준다 —
		 * 경고만 하고 방치하면 뒤따르는 처리가 모두 위험해진다. */
		if (WARN_ONCE(!irqs_disabled(),"irq %u handler %pS enabled interrupts\n",	/* [한국어] 인터럽트가 켜져 있으면 핸들러가 규칙을 어긴 것이다 */
			      irq, action->handler))	/* [한국어] %pS 로 어느 함수인지 심볼 이름을 찍는다 */
			local_irq_disable();	/* [한국어] 다시 끈다. 경고만 하고 두면 뒤따르는 처리가 잘못된 문맥에서 돈다 */

		switch (res) {	/* [한국어] 핸들러의 답에 따라 후처리를 정한다 */
		case IRQ_WAKE_THREAD:	/* [한국어] "1차 처리는 했고 나머지는 스레드에서" */
			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */
			/* [한국어] (위 영어 주석) 스레드 함수 없이 이렇게 답한 드라이버를 잡는다.
			 *
			 * 드라이버 버그다. 깨울 스레드가 없으므로 그 인터럽트는 아무도
			 * 처리하지 않은 채 끝난다. 아래 warn_no_thread 가 한 번만 알린다.
			 *
			 * break 로 빠져나가면 아래 retval |= res 는 실행되므로,
			 * IRQ_WAKE_THREAD 가 결과에 섞인다. 그 값은 IRQ_NONE 이 아니라
			 * 오탐 감지에서 "처리됨" 으로 집계된다 — 처리되지 않았지만
			 * 오탐으로 몰아 인터럽트를 꺼 버리는 것보다는 낫다는 판단이다. */
			if (unlikely(!action->thread_fn)) {	/* [한국어] 스레드 함수가 등록되어 있지 않은가 */
				warn_no_thread(irq, action);	/* [한국어] 한 번만 경고한다 */
				break;	/* [한국어] 깨울 스레드가 없으니 아래 호출을 건너뛴다 */
			}

			__irq_wake_thread(desc, action);	/* [한국어] 스레드를 깨운다. threads_oneshot 과 threads_active 도 여기서 갱신된다 */
			break;

		default:	/* [한국어] IRQ_HANDLED 와 IRQ_NONE 은 후처리가 없다 */
			break;
		}

		retval |= res;	/* [한국어] OR 로 쌓는다. 하나라도 IRQ_HANDLED 면 결과에 그 비트가 남는다 */
	}

	return retval;	/* [한국어] 호출자가 이 값으로 오탐 여부를 판정한다 */
}

/*
 * [한국어]
 * handle_irq_event_percpu - 핸들러 호출에 엔트로피 수집과 오탐 감지를 더한다
 *
 * @desc:   대상 서술자.
 * @return: 위 __handle_irq_event_percpu() 의 결과 그대로.
 *
 * 알맹이는 위 함수가 하고, 여기서는 두 가지 부수 작업을 더한다.
 *
 *   난수 엔트로피 — 인터럽트가 도착한 시각은 예측하기 어려운 값이라
 *     난수 생성기의 엔트로피 원천으로 쓴다. 장치의 물리적 동작에서
 *     오는 것이라 소프트웨어만으로는 흉내 내기 어렵다.
 *   오탐 감지 — 결과를 spurious.c 에 넘겨, 아무도 처리하지 않는 인터럽트가
 *     계속되면 그 선을 꺼 버리게 한다.
 *
 * per-CPU 라는 이름이 붙은 이유: 이 함수는 서술자 락을 잡지 않는다.
 * per-CPU 인터럽트는 CPU 마다 별개라 다른 CPU 와 경쟁하지 않으므로
 * 락이 필요 없고, 그런 흐름 제어가 이 함수를 직접 부른다. 보통의
 * 인터럽트는 아래 handle_irq_event() 를 거쳐 락 관리까지 받는다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   handle_percpu_irq()/handle_percpu_devid_irq() (chip.c) → [이 함수]
 *   handle_irq_event() (아래) → [이 함수]
 */
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc)
{
	irqreturn_t retval;	/* [한국어] 핸들러들의 집계 결과 */

	retval = __handle_irq_event_percpu(desc);	/* [한국어] 실제 핸들러 호출은 알맹이에 맡긴다 */

	add_interrupt_randomness(desc->irq_data.irq);	/* [한국어] 도착 시각의 예측 불가능성을 난수 풀에 넣는다. 장치의 물리적 동작에서 오는 엔트로피다 */

	if (!irq_settings_no_debug(desc))	/* [한국어] 오탐 감지에서 제외된 인터럽트가 아니면 */
		note_interrupt(desc, retval);	/* [한국어] spurious.c 에 결과를 넘긴다. IRQ_NONE 이 쌓이면 그 선을 꺼 버린다 */
	return retval;	/* [한국어] 흐름 제어 핸들러가 이 값으로 다음 동작을 정한다 */
}

/*
 * [한국어]
 * handle_irq_event - 서술자 락을 놓고 핸들러를 부른 뒤 다시 잡는다
 *
 * @desc:   대상 서술자. 호출자가 락을 쥔 상태로 넘겨야 한다.
 * @return: 핸들러들의 집계 결과.
 *
 * 보통의 인터럽트가 쓰는 진입점이다. 위 두 함수에 락 관리와 상태 관리를 더한다.
 *
 * 왜 락을 놓는가: 드라이버 핸들러는 임의의 시간이 걸리고, 그 안에서
 * 인터럽트 코어의 함수를 부를 수도 있다. 서술자 락을 쥔 채 부르면 그런
 * 호출이 교착하고, 다른 CPU 의 인터럽트 관리 작업도 그 시간 동안 막힌다.
 *
 * 락을 놓아도 안전한 이유가 IRQD_IRQ_INPROGRESS 다. 그 비트가 서 있는 동안
 * 다른 경로는 이 인터럽트가 처리 중임을 알고 물러난다 — 같은 인터럽트가
 * 다른 CPU 에서 동시에 처리되지 않고, free_irq() 도 기다린다.
 *
 * 순서가 중요하다: PENDING 을 지우고, INPROGRESS 를 세우고, 그 다음에
 * 락을 놓는다. 셋 다 락 아래에서 일어나야 원자적으로 보인다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 들어올 때와 나갈 때 모두 락을 쥔 상태다.
 *
 * 호출 체인:
 *   handle_level_irq()/handle_edge_irq()/handle_fasteoi_irq() (chip.c)
 *     → [이 함수] → handle_irq_event_percpu()
 */
irqreturn_t handle_irq_event(struct irq_desc *desc)
{
	irqreturn_t ret;	/* [한국어] 핸들러들의 집계 결과 */

	desc->istate &= ~IRQS_PENDING;	/* [한국어] 지금 처리하므로 "밀려 있음" 표시를 지운다. 처리 중에 새 인터럽트가 오면 흐름 제어가 다시 세운다 */
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);	/* [한국어] 처리 중임을 표시한다. 락을 놓아도 안전한 것은 이 비트 덕분이다 */
	raw_spin_unlock(&desc->lock);	/* [한국어] 락을 놓는다. 드라이버 핸들러가 임의의 시간을 쓰고 코어 함수를 부를 수도 있어 쥔 채로 부를 수 없다 */

	ret = handle_irq_event_percpu(desc);	/* [한국어] 락 없이 핸들러들을 부른다. INPROGRESS 가 다른 경로를 막아 준다 */

	raw_spin_lock(&desc->lock);	/* [한국어] 다시 잡는다. 호출자가 락을 쥔 상태를 기대하기 때문이다 */
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);	/* [한국어] 처리가 끝났음을 알린다. 기다리던 free_irq/synchronize_irq 가 이제 진행할 수 있다 */
	return ret;	/* [한국어] 흐름 제어 핸들러가 이 값으로 재시도 여부 등을 정한다 */
}

/* [한국어] 아키텍처의 최상위 인터럽트 진입점을 실행 중에 정하는 기능.
 *
 * ARM64, RISC-V 등이 쓴다. 하나의 커널 이미지가 여러 인터럽트 컨트롤러를
 * 지원해야 하고, 어느 것인지는 부팅 때 장치 트리를 보고 정해지기 때문이다.
 *
 * x86 은 이 기능을 쓰지 않는다. 인터럽트 진입을 어셈블리로 직접 다루고
 * 그 안에서 여러 처리를 함께 하므로, 함수 포인터 하나로 대체할 수 없다. */
#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER	/* [한국어] 진입점을 실행 중에 정하는 아키텍처인가 */
/*
 * [한국어]
 * set_handle_irq - 아키텍처의 최상위 인터럽트 진입점을 등록한다
 *
 * @handle_irq: 그 진입점 함수.
 * @return:     0 이면 등록 성공, -EBUSY 면 이미 등록되어 있다.
 *
 * 인터럽트 컨트롤러 드라이버가 초기화하면서 한 번 부른다. 그 뒤로 모든
 * 하드웨어 인터럽트가 이 함수를 거친다.
 *
 * -EBUSY 방어가 중요하다. 두 컨트롤러 드라이버가 모두 최상위라고 주장하면
 * 나중 것이 앞의 것을 덮어써, 앞 컨트롤러의 인터럽트가 통째로 사라진다.
 * 조용히 덮어쓰는 대신 두 번째 시도를 거절해 문제를 드러낸다.
 *
 * __init 인 이유: 부팅 중에만 불린다. 그 뒤 이 함수의 코드는 메모리에서
 * 반납되고, 위 handle_arch_irq 는 __ro_after_init 으로 읽기 전용이 되어
 * 아무도 고칠 수 없게 된다.
 *
 * 실행 컨텍스트: 부팅 중 컨트롤러 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   GIC/PLIC 등 최상위 컨트롤러 드라이버의 초기화 → [이 함수]
 */
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq)	/* [한국어] 이미 다른 드라이버가 등록했는가 */
		return -EBUSY;	/* [한국어] 덮어쓰지 않고 거절한다. 조용히 덮으면 앞 컨트롤러의 인터럽트가 통째로 사라진다 */

	handle_arch_irq = handle_irq;	/* [한국어] 등록한다. 부팅이 끝나면 이 변수는 읽기 전용이 된다 */
	return 0;	/* [한국어] 성공 */
}

/**
 * generic_handle_arch_irq - root irq handler for architectures which do no
 *                           entry accounting themselves
 * @regs:	Register file coming from the low-level handling code
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * generic_handle_arch_irq - 인터럽트 진입·이탈 회계를 대신해 주는 최상위 래퍼
 *
 * @regs: 저수준 진입 코드가 저장한 레지스터 상태.
 *
 * kernel-doc 이 말하는 "진입 회계를 스스로 하지 않는 아키텍처" 를 위한
 * 것이다. 아키텍처의 어셈블리 진입 코드가 이 함수를 부르면, 인터럽트
 * 문맥 진입·이탈 처리를 대신 해 준다.
 *
 * 네 가지를 감싼다:
 *   irq_enter()   — 선점 카운트를 올려 인터럽트 문맥임을 알리고, RCU 와
 *                   틱 처리에 진입을 통보한다.
 *   set_irq_regs()— 인터럽트 시점의 레지스터를 per-CPU 자리에 저장한다.
 *                   프로파일러가 "인터럽트가 어느 코드에서 일어났는가" 를
 *                   알아내는 데 쓴다.
 *   handle_arch_irq()— 실제 컨트롤러 드라이버의 진입점.
 *   irq_exit()    — 위 enter 의 짝. 선점 카운트를 내리고, 이 시점에 밀린
 *                   softirq 를 처리한다.
 *
 * 옛 값을 저장했다 복원하는 이유: 인터럽트가 중첩될 수 있다. 바깥 인터럽트의
 * 레지스터를 덮어쓰면 그것이 돌아갈 때 잘못된 값을 본다.
 *
 * noinstr 이 붙은 이유: 이 함수는 RCU 가 아직 활성화되지 않은 구간을 지난다.
 * ftrace 나 kprobe 같은 계측이 끼어들면 그 도구들이 RCU 를 쓰다 문제가
 * 생기므로, 계측을 아예 금지한다.
 *
 * asmlinkage 는 어셈블리에서 부르는 함수임을 나타내며, 일부 아키텍처에서
 * 인자 전달 규약을 스택 기반으로 바꾼다.
 *
 * 실행 컨텍스트: 하드 인터럽트 진입 직후. 가장 이른 C 코드다.
 *
 * 호출 체인:
 *   아키텍처의 예외 벡터(어셈블리) → [이 함수] → handle_arch_irq()
 *     → 컨트롤러 드라이버 → generic_handle_domain_irq() → 흐름 제어
 */
asmlinkage void noinstr generic_handle_arch_irq(struct pt_regs *regs)
{
	struct pt_regs *old_regs;	/* [한국어] 중첩 인터럽트를 위해 옛 값을 보관한다 */

	irq_enter();	/* [한국어] 인터럽트 문맥 진입. 선점 카운트를 올리고 RCU 와 틱에 통보한다 */
	old_regs = set_irq_regs(regs);	/* [한국어] 프로파일러가 볼 레지스터를 갈아 끼우고 옛 것을 받는다 */
	handle_arch_irq(regs);	/* [한국어] 실제 컨트롤러 드라이버의 진입점. 여기서 어느 인터럽트인지 알아내 코어로 넘긴다 */
	set_irq_regs(old_regs);	/* [한국어] 옛 값을 되돌린다. 중첩 인터럽트에서 바깥쪽이 잘못된 값을 보지 않게 한다 */
	irq_exit();	/* [한국어] 인터럽트 문맥 이탈. 선점 카운트를 내리고, 이 시점에 밀린 softirq 를 처리한다 */
}
#endif
