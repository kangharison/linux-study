// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains spurious interrupt handling.
 */
/*
 * [한국어 설명] 아무도 처리하지 않는 인터럽트를 감지하고 대응하는 코드 (spurious.c)
 *
 * === 파일의 역할 ===
 * 인터럽트가 들어왔는데 등록된 핸들러 중 아무도 "내 것" 이라고 하지 않는
 * 상황을 추적한다. 그것이 계속되면 그 인터럽트 선을 꺼 버린다.
 *
 * 왜 꺼야 하는가: 고장난 하드웨어나 잘못된 설정으로 인터럽트가 끊임없이
 * 들어오는데 아무도 원인을 지우지 못하면, CPU 가 그 인터럽트를 처리하는
 * 데만 시간을 다 쓰고 시스템이 사실상 멈춘다. 그 선 하나를 희생해 시스템을
 * 살리는 것이 이 파일의 목적이다.
 *
 * 판정 기준은 파일 중간의 영어 주석에 있다 — 최근 10만 번 중 99,900 번이
 * 처리되지 않았으면 고장으로 본다. 100 번의 여유를 두는 이유는 그 선을
 * 공유하는 다른 장치가 정상적으로 동작하고 있을 수 있기 때문이다.
 *
 * 두 번째 역할은 "잘못 배선된 인터럽트" 찾기다. 오래된 하드웨어에서는
 * 장치가 실제로 어느 선에 연결되었는지 펌웨어가 잘못 알려 주는 일이 있다.
 * irqfixup/irqpoll 부트 인자를 주면, 처리되지 않은 인터럽트가 있을 때
 * 다른 선들의 핸들러를 불러 보며 진짜 주인을 찾는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 처리의 맨 끝에 붙어 있다:
 *
 *   흐름 제어 핸들러 → handle_irq_event() → handle_irq_event_percpu()
 *     ↓ 모든 핸들러의 반환값을 OR 로 모아
 *   note_interrupt()                    ← **이 파일**의 진입점
 *     ↓ IRQ_NONE 이 쌓이면
 *   __report_bad_irq() → 로그 + 그 선을 끈다
 *     ↓ 끈 뒤에는
 *   poll_spurious_irqs() 타이머         ← **이 파일** — 주기적으로 되살려 본다
 *
 * 별개의 경로: irqfixup 이 켜져 있으면 note_interrupt 가 misrouted_irq() 를
 * 불러 다른 선들을 폴링한다.
 *
 * 실행 컨텍스트: note_interrupt 는 하드 인터럽트 문맥이며 서술자 락을
 * 쥐지 않은 상태다(INPROGRESS 가 대신 보호한다). 타이머 쪽은 softirq 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   handle.c 의 handle_irq_event() — 폴링할 때 핸들러를 직접 부른다.
 *   internals.h 의 IRQS_* 비트와 irq_settings_* 판정자.
 *   타이머 서브시스템 — 꺼 버린 인터럽트를 주기적으로 되살려 본다.
 *
 * 이 파일에 의존하는 곳:
 *   handle.c 의 handle_irq_event_percpu() 가 note_interrupt() 를 부른다.
 *   internals.h 가 noirqdebug 와 irq_poll_cpu 를 선언한다.
 *
 * === 주요 함수/구조체 요약 ===
 * irqfixup              — 0/1/2 로 잘못 배선 대응 수준을 정한다. 부트 인자로만 켠다.
 * poll_spurious_irq_timer — 꺼 버린 인터럽트를 주기적으로 되살려 보는 타이머.
 * try_one_irq()         — 인터럽트 하나의 핸들러를 강제로 불러 본다.
 * misrouted_irq()       — 모든 선을 훑으며 진짜 주인을 찾는다.
 * poll_spurious_irqs()  — 꺼진 선들만 골라 되살려 본다.
 * bad_action_ret()      — 핸들러가 규격 밖의 값을 돌려줬는지 판정.
 * __report_bad_irq()    — 문제를 로그에 자세히 남긴다.
 * note_interrupt()      — 이 파일의 진입점. 통계를 세고 임계값을 넘으면 끈다.
 *
 * 스레드 핸들러 때문에 판정이 한 번 미뤄진다는 것이 note_interrupt 의 가장
 * 어려운 부분이다. 그 이유는 그 함수의 긴 주석에 있다.
 */

#include <linux/jiffies.h>	/* [한국어] time_after() — 오탐 카운터를 언제 리셋할지 판정한다 */
#include <linux/irq.h>	/* [한국어] struct irq_desc 와 irqd_* 접근자 */
#include <linux/module.h>	/* [한국어] module_param — noirqdebug 와 irqfixup 을 실행 중에 바꿀 수 있게 한다 */
#include <linux/interrupt.h>	/* [한국어] irqreturn_t 와 IRQF_* 플래그 */
#include <linux/moduleparam.h>	/* [한국어] MODULE_PARM_DESC — 파라미터 설명 */
#include <linux/timer.h>	/* [한국어] DEFINE_TIMER, mod_timer — 되살리기 타이머 */

#include "internals.h"	/* [한국어] handle_irq_event, IRQS_*, irq_settings_* 등 코어 내부 */

/* [한국어] 잘못 배선된 인터럽트에 대응하는 수준.
 *
 * 세 값의 뜻:
 *   0 — 아무것도 하지 않는다 (기본값).
 *   1 — irqfixup 부트 인자. 처리되지 않은 인터럽트가 있으면 다른 선들을
 *       폴링해 진짜 주인을 찾는다.
 *   2 — irqpoll 부트 인자. 처리된 인터럽트에 대해서도 폴링한다. 훨씬
 *       공격적이라 성능에 뚜렷한 영향이 있다.
 *
 * __read_mostly 인 이유: 부팅 때 한 번 정해지고 이후 거의 읽기만 한다.
 * 그런 변수를 따로 모아 두면 자주 쓰이는 캐시 줄을 무효화하지 않는다.
 *
 * PREEMPT_RT 에서는 아예 켤 수 없다(아래 두 setup 함수 참고). 폴링이
 * 다른 CPU 의 핸들러를 강제로 부르는 방식이라 실시간 보장과 맞지 않는다. */
static int irqfixup __read_mostly;

#define POLL_SPURIOUS_IRQ_INTERVAL (HZ/10)	/* [한국어] 꺼 버린 인터럽트를 되살려 보는 주기 — 0.1초. 너무 짧으면 죽은 선을 계속 두드려 낭비고, 너무 길면 일시적 고장에서 복구가 늦다 */
static void poll_spurious_irqs(struct timer_list *unused);	/* [한국어] 아래 타이머 정의보다 먼저 선언해야 한다 — DEFINE_TIMER 가 이 이름을 참조하기 때문이다 */
static DEFINE_TIMER(poll_spurious_irq_timer, poll_spurious_irqs);	/* [한국어] 꺼 버린 인터럽트를 되살려 보는 타이머. 실제로 끈 순간에만 시작된다 */
int irq_poll_cpu;	/* [한국어] 지금 폴링을 수행 중인 CPU. static 이 아닌 이유는 internals.h 를 통해 다른 파일이 볼 수 있어야 하기 때문이다. 폴링이 자기 자신을 오탐으로 세지 않게 하는 데 쓴다 */
static atomic_t irq_poll_active;	/* [한국어] 폴링이 진행 중인지 나타내는 카운터. 여러 CPU 가 동시에 폴링하면 서로의 핸들러를 부르며 뒤엉키므로 하나만 허용한다 */

/*
 * Recovery handler for misrouted interrupts.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * try_one_irq - 인터럽트 하나의 핸들러를 강제로 불러 본다
 *
 * @desc:   대상 서술자.
 * @force:  참이면 비활성화된 인터럽트에 대해서도 시도한다.
 * @return: 핸들러 중 하나가 IRQ_HANDLED 를 돌려주었으면 참.
 *
 * 실제 인터럽트가 오지 않았는데 핸들러를 부른다는 점이 이 함수의 성격이다.
 * 두 가지 목적에 쓰인다:
 *   잘못 배선된 인터럽트 찾기 — 다른 선의 장치가 울린 것인지 확인한다.
 *   꺼 버린 인터럽트 되살리기 — 이제 정상 동작하는지 확인한다.
 *
 * 여러 조건으로 걸러 내는데, 각각 폴링이 위험하거나 무의미한 경우다.
 * 자세한 이유는 아래 각 조건의 주석에 있다.
 *
 * IRQF_SHARED 를 요구하는 것이 핵심 제약이다. 공유되지 않는 선은 그 장치
 * 하나만 쓰므로 잘못 배선되었을 여지가 없고, 핸들러를 함부로 부르면
 * 장치 상태만 어지럽힌다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태. 서술자 락을 직접 잡는다.
 *
 * 호출 체인:
 *   misrouted_irq() 또는 poll_spurious_irqs() → [이 함수]
 *     → handle_irq_event() (kernel/irq/handle.c)
 */
static bool try_one_irq(struct irq_desc *desc, bool force)
{
	struct irqaction *action;	/* [한국어] 첫 핸들러. 플래그 검사와 목록 유효성 확인에 쓴다 */
	bool ret = false;	/* [한국어] 누군가 처리했는가 */

	guard(raw_spinlock)(&desc->lock);	/* [한국어] 서술자 락. guard 라 어느 return 으로 나가든 풀린다 */

	/*
	 * PER_CPU, nested thread interrupts and interrupts explicitly
	 * marked polled are excluded from polling.
	 */
	/* [한국어] (위 영어 주석에 이어) 세 종류는 폴링 대상이 아니다.
	 *
	 *   per-CPU  — CPU 마다 별개의 선이라 다른 CPU 의 장치가 울렸을 여지가
	 *     없다. 그리고 이 함수는 지금 CPU 의 문맥에서 도는데, 그 인터럽트는
	 *     다른 CPU 의 것일 수 있다.
	 *   중첩 스레드 — 자체 흐름 제어 핸들러가 없어 handle_irq_event() 를
	 *     부를 수 없다. 부모가 대신 불러 주는 구조다.
	 *   폴링 표시  — 드라이버가 이미 폴링을 병행한다고 선언한 선이다.
	 *     커널이 또 폴링하면 중복이고 장치 상태가 어지러워진다. */
	if (irq_settings_is_per_cpu(desc) || irq_settings_is_nested_thread(desc) ||	/* [한국어] CPU 별 선이거나 부모가 대신 부르는 선인가 */
	    irq_settings_is_polled(desc))	/* [한국어] 드라이버가 이미 폴링을 병행하는 선인가 */
		return false;	/* [한국어] 손대지 않는다 */

	/*
	 * Do not poll disabled interrupts unless the spurious
	 * disabled poller asks explicitly.
	 */
	/* [한국어] (위 영어 주석에 이어) 비활성화된 인터럽트는 원칙적으로 건드리지 않는다.
	 *
	 * 왜인가: 드라이버가 일부러 꺼 둔 선이다. 그 드라이버는 지금 핸들러가
	 * 불리지 않으리라 믿고 장치를 재설정하는 중일 수 있다.
	 *
	 * force 로 예외를 두는 이유: 아래 poll_spurious_irqs() 는 "우리가
	 * 오탐 때문에 꺼 버린" 선을 되살려 보는 것이라, 꺼져 있는 것이 당연하다.
	 * 그 경우에는 시도해야 한다. */
	if (irqd_irq_disabled(&desc->irq_data) && !force)	/* [한국어] 꺼져 있는데 강제 요청도 아닌가 */
		return false;	/* [한국어] 드라이버가 일부러 꺼 둔 선일 수 있어 건드리지 않는다 */

	/*
	 * All handlers must agree on IRQF_SHARED, so we test just the
	 * first.
	 */
	/* [한국어] (위 영어 주석에 이어) 공유 선만 폴링한다.
	 *
	 * 왜 첫 핸들러만 보는가: 커널은 한 선에 공유 핸들러와 비공유 핸들러가
	 * 섞이는 것을 허용하지 않는다. request_irq 가 그 조합을 거절하므로,
	 * 첫 번째만 보면 전체를 안 것이 된다.
	 *
	 * 왜 공유 선만인가: 공유되지 않는 선은 그 장치 하나만 쓴다. 다른
	 * 장치가 그 선에 잘못 연결되었을 여지가 없어 폴링할 이유가 없고,
	 * 핸들러를 헛되이 부르면 장치 상태만 어지럽힌다.
	 *
	 * __IRQF_TIMER 를 제외하는 이유: 타이머 인터럽트는 시스템의 시간
	 * 기준이다. 핸들러를 임의로 부르면 시간이 앞으로 튀거나 스케줄러가
	 * 혼란스러워진다. */
	action = desc->action;	/* [한국어] 첫 핸들러 */
	if (!action || !(action->flags & IRQF_SHARED) || (action->flags & __IRQF_TIMER))	/* [한국어] 핸들러가 없거나, 공유가 아니거나, 타이머인가 */
		return false;	/* [한국어] 폴링 대상이 아니다 */

	/* Already running on another processor */
	/* [한국어] (위 영어 주석) 다른 CPU 에서 이미 처리 중이다. */
	if (irqd_irq_inprogress(&desc->irq_data)) {	/* [한국어] 이 인터럽트가 지금 다른 CPU 에서 처리되고 있는가 */
		/*
		 * Already running: If it is shared get the other
		 * CPU to go looking for our mystery interrupt too
		 */
		/* [한국어] (위 영어 주석에 이어) 직접 부르는 대신 그쪽에 부탁한다.
		 *
		 * IRQS_PENDING 을 세우면 지금 처리 중인 쪽이 핸들러를 마친 뒤
		 * 한 번 더 돈다(아래 do-while 과 흐름 제어의 재실행 루프가 그렇다).
		 * 그래서 우리가 원하는 "핸들러를 한 번 더 불러 보기" 가 그쪽에서 일어난다.
		 *
		 * 왜 직접 부르지 않는가: 같은 인터럽트를 두 CPU 에서 동시에
		 * 처리하면 드라이버 핸들러가 재진입하게 되어, 대부분의 드라이버가
		 * 감당하지 못한다. */
		desc->istate |= IRQS_PENDING;	/* [한국어] 처리 중인 쪽이 한 번 더 돌게 만든다 — 그것이 우리가 원한 폴링을 대신해 준다 */
		return false;	/* [한국어] 우리는 아무것도 처리하지 않았다 */
	}

	/* Mark it poll in progress */
	/* [한국어] (위 영어 주석) 폴링 중임을 표시한다.
	 *
	 * 무엇을 막는가: 아래 handle_irq_event() 안에서 note_interrupt() 가
	 * 불린다. 그것이 이 폴링을 진짜 인터럽트로 세면, 처리되지 않은 폴링이
	 * 오탐 통계에 쌓여 멀쩡한 선이 꺼진다. note_interrupt 의 첫 검사가
	 * 이 비트를 보고 물러난다. */
	desc->istate |= IRQS_POLL_INPROGRESS;	/* [한국어] note_interrupt 가 이 호출을 통계에 세지 않게 한다 */
	do {
		if (handle_irq_event(desc) == IRQ_HANDLED)	/* [한국어] 핸들러들을 실제로 부른다. 진짜 인터럽트와 같은 경로다 */
			ret = true;	/* [한국어] 누군가 자기 것이라고 답했다 — 이 선의 장치가 정말 울린 것이다 */
		/* Make sure that there is still a valid action */
		/* [한국어] (위 영어 주석) 핸들러 목록이 아직 유효한지 다시 확인한다.
		 *
		 * 왜인가: handle_irq_event() 는 안에서 서술자 락을 놓았다 잡는다.
		 * 그 사이에 free_irq() 가 마지막 핸들러를 떼어 냈을 수 있다.
		 * 확인하지 않고 다시 돌면 빈 목록을 순회하게 된다. */
		action = desc->action;	/* [한국어] 락을 놓았다 잡은 사이에 목록이 바뀌었을 수 있다 */
	} while ((desc->istate & IRQS_PENDING) && action);	/* [한국어] 처리 중에 새 인터럽트가 밀렸고 핸들러가 아직 있으면 한 번 더 */
	desc->istate &= ~IRQS_POLL_INPROGRESS;	/* [한국어] 폴링 표시를 지운다. 이제부터의 인터럽트는 정상 집계된다 */
	return ret;	/* [한국어] 이 선의 장치가 실제로 울렸는가 */
}

/*
 * [한국어]
 * misrouted_irq - 모든 선을 훑으며 인터럽트의 진짜 주인을 찾는다
 *
 * @irq:    방금 처리되지 않은 인터럽트의 번호. 이 선은 건너뛴다.
 * @return: 어느 선에서든 핸들러가 처리했으면 1, 아니면 0.
 *
 * 잘못 배선된 인터럽트에 대응하는 핵심이다. 어떤 장치가 실제로는 A 선에
 * 연결되어 있는데 펌웨어가 B 선이라고 알려 주면, B 로 인터럽트가 오지 않고
 * A 로 온다. A 에 붙은 드라이버들은 자기 것이 아니라며 IRQ_NONE 을 돌려주고,
 * 그 장치는 영원히 응답받지 못한다.
 *
 * 이 함수는 그럴 때 다른 모든 선의 핸들러를 불러 본다. 그중 하나가
 * IRQ_HANDLED 를 주면 그 장치가 원인이었던 것이고, 인터럽트가 처리된 셈이다.
 *
 * 두 가지를 건너뛴다:
 *   0 번 — 전통적으로 타이머 인터럽트다. 건드리면 시간이 어긋난다.
 *   방금 처리 실패한 선 — 이미 시도했으므로 또 부를 이유가 없다.
 *
 * 폴링을 하나로 제한하는 것이 중요하다. 여러 CPU 가 동시에 하면 서로의
 * 핸들러를 부르며 뒤엉키고, 재진입 문제까지 생긴다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 매우 비싼 연산이라 irqfixup 을
 * 켠 시스템에서만 실행된다.
 *
 * 호출 체인:
 *   note_interrupt() → try_misrouted_irq() 가 참이면 → [이 함수]
 *     → try_one_irq()
 */
static int misrouted_irq(int irq)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	int i, ok = 0;	/* [한국어] 인터럽트 번호와, 누군가 처리했는지 */

	if (atomic_inc_return(&irq_poll_active) != 1)	/* [한국어] 올린 결과가 1 이 아니면 이미 다른 CPU 가 폴링 중이다 */
		goto out;	/* [한국어] 물러난다. 올린 값을 아래에서 반드시 내려야 하므로 goto 로 간다 */

	irq_poll_cpu = smp_processor_id();	/* [한국어] 폴링 중인 CPU 를 기록한다. 다른 코드가 이 폴링을 오탐으로 세지 않게 하는 단서다 */

	for_each_irq_desc(i, desc) {	/* [한국어] 모든 인터럽트 선을 훑는다. 매우 비싼 연산이다 */
		if (!i)	/* [한국어] 0 번은 전통적으로 타이머 인터럽트다 */
			 continue;	/* [한국어] 건드리면 시스템 시간이 어긋난다 */

		if (i == irq)	/* Already tried */	/* [한국어] (원 주석) 방금 실패한 그 선이다 */
			continue;	/* [한국어] 이미 시도했으므로 또 부를 이유가 없다 */

		if (try_one_irq(desc, false))	/* [한국어] 이 선의 핸들러를 불러 본다. force 가 거짓이라 꺼진 선은 건너뛴다 */
			ok = 1;	/* [한국어] 이 선의 장치가 원인이었다 — 인터럽트가 처리된 셈이다 */
	}
out:	/* [한국어] 폴링 카운터를 반드시 내려야 하므로 조기 반환도 여기를 지난다 */
	atomic_dec(&irq_poll_active);	/* [한국어] 위에서 올린 것을 반드시 되돌린다. 조기 반환 경로도 여기를 지난다 */
	/* So the caller can adjust the irq error counts */
	return ok;	/* [한국어] (원 주석) 호출자가 오탐 카운터를 깎는 데 쓴다 */
}

/*
 * [한국어]
 * poll_spurious_irqs - 오탐으로 꺼 버린 인터럽트들을 주기적으로 되살려 본다
 *
 * @unused: 타이머 API 가 요구하는 인자. 전역 상태만 쓰므로 필요 없다.
 *
 * 오탐 감지가 인터럽트를 꺼 버린 뒤, 그것이 영구적인 사망 선고가 되지
 * 않게 하는 장치다. 0.1초마다 깨어나 꺼진 선들의 핸들러를 불러 보고,
 * 정상 동작하는 것이 확인되면 그 선이 되살아난다.
 *
 * 왜 필요한가: 오탐의 원인이 일시적일 수 있다. 장치가 초기화 중이었거나,
 * 공유 선의 다른 드라이버가 아직 준비되지 않았을 수 있다. 한 번 꺼졌다고
 * 영원히 꺼 두면 그런 시스템은 그 장치를 영영 못 쓴다.
 *
 * 위 misrouted_irq() 와 구조가 거의 같지만 세 가지가 다르다:
 *   IRQS_SPURIOUS_DISABLED 인 선만 고른다 — 우리가 꺼 버린 것들이다.
 *   try_one_irq 에 force=true 를 준다 — 꺼져 있는 것이 당연하기 때문이다.
 *   마지막에 타이머를 다시 건다 — 주기적으로 반복해야 한다.
 *
 * 인터럽트를 직접 끄고 켜는 이유: 타이머는 softirq 문맥이라 인터럽트가
 * 켜져 있다. try_one_irq() 는 인터럽트가 꺼진 상태를 전제하므로 직접 꺼 준다.
 *
 * 실행 컨텍스트: 타이머 softirq 문맥.
 *
 * 호출 체인:
 *   타이머 만료 → [이 함수] → try_one_irq(force=true)
 */
static void poll_spurious_irqs(struct timer_list *unused)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	int i;	/* [한국어] 인터럽트 번호 */

	if (atomic_inc_return(&irq_poll_active) != 1)	/* [한국어] 다른 CPU 가 이미 폴링 중이면 */
		goto out;	/* [한국어] 물러난다. 아래에서 반드시 카운터를 내려야 하므로 goto 다 */
	irq_poll_cpu = smp_processor_id();	/* [한국어] 폴링 중인 CPU 를 기록한다 */

	for_each_irq_desc(i, desc) {	/* [한국어] 모든 선을 훑되 아래에서 꺼진 것만 고른다 */
		unsigned int state;	/* [한국어] istate 의 사본 */

		if (!i)	/* [한국어] 0 번 타이머는 건너뛴다 */
			 continue;

		/* Racy but it doesn't matter */
		/* [한국어] (위 영어 주석) 락 없이 읽어 값이 낡을 수 있지만 상관없다.
		 *
		 * 왜 상관없는가: 여기서 판정을 놓쳐도 0.1초 뒤에 다시 시도한다.
		 * 반대로 이미 되살아난 선을 잘못 고르더라도, 아래 try_one_irq() 가
		 * 서술자 락을 잡고 다시 검사하므로 안전하다.
		 *
		 * 모든 선에 대해 락을 잡으면 인터럽트 처리가 그만큼 막힌다. 이
		 * 함수는 0.1초마다 도는 배경 작업이라 정확도보다 비용이 중요하다.
		 *
		 * READ_ONCE 를 쓰는 이유: 컴파일러가 이 읽기를 나누거나 합치지
		 * 못하게 한다. 락 없이 읽는 값은 반드시 한 번에 읽어야 반쯤
		 * 갱신된 값을 보지 않는다. */
		state = READ_ONCE(desc->istate);	/* [한국어] 락 없이 한 번에 읽는다. 낡아도 다음 주기에 다시 본다 */
		if (!(state & IRQS_SPURIOUS_DISABLED))	/* [한국어] 우리가 오탐 때문에 꺼 버린 선인가 */
			continue;	/* [한국어] 아니면 되살릴 대상이 아니다 */

		local_irq_disable();	/* [한국어] 타이머는 softirq 문맥이라 인터럽트가 켜져 있다. try_one_irq 는 꺼진 상태를 전제한다 */
		try_one_irq(desc, true);	/* [한국어] force 로 부른다 — 이 선이 꺼져 있는 것이 당연하기 때문이다 */
		local_irq_enable();	/* [한국어] 다시 켠다. 한 선씩 짧게 끄고 켜, 다른 인터럽트의 지연을 줄인다 */
	}
out:	/* [한국어] 폴링 카운터를 반드시 내려야 하므로 조기 반환도 여기를 지난다 */
	atomic_dec(&irq_poll_active);	/* [한국어] 위에서 올린 것을 되돌린다. 조기 반환도 여기를 지난다 */
	mod_timer(&poll_spurious_irq_timer, jiffies + POLL_SPURIOUS_IRQ_INTERVAL);	/* [한국어] 0.1초 뒤에 다시 돈다. 꺼진 선이 하나도 없어도 계속 도는데, 언제 새로 꺼질지 알 수 없기 때문이다 */
}

/*
 * [한국어]
 * bad_action_ret - 핸들러가 규격 밖의 값을 돌려줬는지 판정한다
 *
 * @action_ret: 핸들러들의 반환값을 OR 로 모은 것.
 * @return:     규격 밖이면 1, 정상이면 0.
 *
 * 유효한 반환값은 IRQ_NONE(0), IRQ_HANDLED, IRQ_WAKE_THREAD 셋이며,
 * OR 로 합쳐도 (IRQ_HANDLED | IRQ_WAKE_THREAD) 를 넘을 수 없다.
 *
 * 그보다 큰 값이 나왔다는 것은 드라이버가 엉뚱한 것을 돌려줬다는 뜻이다.
 * 흔한 원인은 핸들러가 실수로 0/1 대신 다른 정수를 돌려주거나, 초기화되지
 * 않은 변수를 반환하는 경우다.
 *
 * unsigned 로 변환해 비교하는 것에 주의: irqreturn_t 는 enum 이라 부호가
 * 구현에 달려 있다. 음수가 들어오면 부호 있는 비교에서는 통과해 버리지만,
 * unsigned 로 보면 아주 큰 값이 되어 걸러진다.
 *
 * likely 를 쓰는 이유: 정상이 압도적으로 흔하다. 이 함수는 모든 인터럽트마다
 * 불리는 핫패스다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   note_interrupt() 와 __report_bad_irq() → [이 함수]
 */
static inline int bad_action_ret(irqreturn_t action_ret)
{
	unsigned int r = action_ret;	/* [한국어] unsigned 로 본다. 음수가 들어오면 아주 큰 값이 되어 아래 비교에서 걸러진다 */

	if (likely(r <= (IRQ_HANDLED | IRQ_WAKE_THREAD)))	/* [한국어] 유효한 값들을 OR 로 합친 최대치. 정상이 압도적으로 흔하다 */
		return 0;	/* [한국어] 정상 */
	return 1;	/* [한국어] 드라이버가 규격 밖의 값을 돌려줬다 */
}

/*
 * If 99,900 of the previous 100,000 interrupts have not been handled
 * then assume that the IRQ is stuck in some manner. Drop a diagnostic
 * and try to turn the IRQ off.
 *
 * (The other 100-of-100,000 interrupts may have been a correctly
 *  functioning device sharing an IRQ with the failing one)
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __report_bad_irq - 문제 있는 인터럽트의 상세 정보를 로그에 남긴다
 *
 * @desc:       문제의 서술자.
 * @action_ret: 핸들러들의 반환값. 두 가지 문제 유형을 가르는 데 쓴다.
 *
 * 원 주석이 판정 기준을 말한다: 최근 10만 번 중 99,900 번이 처리되지 않았으면
 * 그 선이 "막혔다" 고 본다. 100 번의 여유를 두는 이유도 함께 적혀 있는데,
 * 그 선을 공유하는 다른 장치가 정상 동작하고 있을 수 있기 때문이다.
 *
 * 두 가지 문제를 구분해 알린다:
 *   규격 밖 반환값 — 드라이버가 잘못된 값을 돌려준다. 코드 버그다.
 *   nobody cared  — 아무도 처리하지 않는다. 배선이나 하드웨어 문제일 수 있어
 *     irqpoll 부트 옵션을 권한다.
 *
 * 등록된 모든 핸들러의 이름을 찍는 것이 진단의 핵심이다. 공유 선에서
 * 어느 드라이버가 관련되어 있는지 알아야 원인을 좁힐 수 있다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 락에 대한 설명은 아래 주석에 있다.
 *
 * 호출 체인:
 *   report_bad_irq() 또는 note_interrupt() 의 임계 초과 경로 → [이 함수]
 */
static void __report_bad_irq(struct irq_desc *desc, irqreturn_t action_ret)
{
	unsigned int irq = irq_desc_get_irq(desc);	/* [한국어] 메시지에 찍을 인터럽트 번호 */
	struct irqaction *action;	/* [한국어] 핸들러 목록을 훑을 커서 */

	if (bad_action_ret(action_ret))	/* [한국어] 규격 밖의 값을 돌려준 경우인가 */
		pr_err("irq event %d: bogus return value %x\n", irq, action_ret);	/* [한국어] 드라이버 코드 버그다. 실제 값을 찍어 무엇을 돌려줬는지 보여 준다 */
	else
		pr_err("irq %d: nobody cared (try booting with the \"irqpoll\" option)\n", irq);	/* [한국어] 아무도 처리하지 않는다. irqpoll 을 권하는 이유는 잘못 배선된 경우 그것으로 진짜 주인을 찾을 수 있기 때문이다 */
	dump_stack();	/* [한국어] 스택 트레이스. 어느 경로에서 이 인터럽트가 처리되었는지 보여 준다 */
	pr_err("handlers:\n");	/* [한국어] 아래 목록의 머리말 */

	/*
	 * We need to take desc->lock here. note_interrupt() is called
	 * w/o desc->lock held, but IRQ_PROGRESS set. We might race
	 * with something else removing an action. It's ok to take
	 * desc->lock here. See synchronize_irq().
	 */
	/* [한국어] (위 영어 주석에 이어) 여기서 서술자 락을 잡아야 하는 이유.
	 *
	 * note_interrupt() 는 락 없이 불린다 — handle_irq_event() 가 락을 놓고
	 * 핸들러를 부르기 때문이다. 대신 IRQD_IRQ_INPROGRESS 가 서 있어 다른
	 * 경로가 물러난다.
	 *
	 * 그런데 그 보호는 완전하지 않다. free_irq() 가 핸들러를 떼는 중일 수
	 * 있고, 목록을 훑는 도중에 항목이 사라지면 해제된 메모리를 읽는다.
	 * 그래서 목록을 읽는 이 구간만 락을 잡는다.
	 *
	 * 원 주석의 "It's ok to take desc->lock here" 가 중요하다 — 이미
	 * INPROGRESS 가 서 있는데 락을 또 잡아도 교착하지 않는다는 뜻이다.
	 * synchronize_irq() 가 그 두 보호 수단의 관계를 보여 준다. */
	guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 목록을 읽는 동안만 잡는다. guard 라 함수를 벗어날 때 풀린다 */
	for_each_action_of_desc(desc, action) {	/* [한국어] 등록된 모든 핸들러. 공유 선이면 여럿이다 */
		pr_err("[<%p>] %ps", action->handler, action->handler);	/* [한국어] 주소와 심볼 이름을 함께. 심볼이 없는 모듈 함수를 위해 주소도 찍는다 */
		if (action->thread_fn)	/* [한국어] 스레드 핸들러도 등록되어 있으면 */
			pr_cont(" threaded [<%p>] %ps", action->thread_fn, action->thread_fn);	/* [한국어] 같은 줄에 이어 붙인다. pr_cont 가 줄바꿈 없이 계속 쓴다 */
		pr_cont("\n");	/* [한국어] 여기서 줄을 마친다 */
	}
}

/*
 * [한국어]
 * report_bad_irq - 문제를 알리되 전체 100 번까지만
 *
 * @desc:       문제의 서술자.
 * @action_ret: 핸들러들의 반환값.
 *
 * 위 __report_bad_irq() 를 감싸며 횟수 제한을 더한다.
 *
 * static 지역 변수 하나로 세는 것이 특이하다. 인터럽트별이 아니라 시스템
 * 전체에서 100 번이며, 그 뒤로는 어떤 인터럽트의 문제도 보고하지 않는다.
 *
 * 왜 그렇게 거친가: 이 함수를 부르는 경우는 드라이버가 규격 밖의 값을
 * 돌려주는 것뿐이고, 그런 드라이버는 매 인터럽트마다 그렇게 한다. 각
 * 보고가 dump_stack() 을 포함해 수십 줄이라, 제한이 없으면 콘솔이 곧바로
 * 마비된다. 100 번이면 원인을 찾기에 충분하다.
 *
 * 락이 없어 카운터가 경쟁하지만 상관없다 — 정확히 100 번일 필요는 없고,
 * "언젠가 멈춘다" 는 것만 보장되면 된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   note_interrupt() → [이 함수] → __report_bad_irq()
 */
static void report_bad_irq(struct irq_desc *desc, irqreturn_t action_ret)
{
	static int count = 100;	/* [한국어] 시스템 전체에서 100 번. 인터럽트별이 아니다 — 각 보고가 스택 트레이스를 포함해 수십 줄이기 때문이다 */

	if (count > 0) {	/* [한국어] 아직 여유가 있는가 */
		count--;	/* [한국어] 락 없이 줄인다. 경쟁해도 상관없다 — 정확한 횟수가 목적이 아니다 */
		__report_bad_irq(desc, action_ret);	/* [한국어] 실제 보고 */
	}
}

/*
 * [한국어]
 * try_misrouted_irq - 잘못 배선된 인터럽트 찾기를 시도할지 판정한다
 *
 * @irq:        그 인터럽트 번호.
 * @desc:       그 서술자.
 * @action_ret: 핸들러들의 반환값.
 * @return:     폴링을 시도해야 하면 참.
 *
 * 폴링은 매우 비싸다(모든 선의 핸들러를 부른다). 그래서 정말 필요할 때만
 * 하도록 이 함수가 걸러 낸다.
 *
 * irqfixup 값에 따라 기준이 달라진다:
 *   0 — 아무것도 하지 않는다. 대부분의 시스템이 여기다.
 *   1 — 처리되지 않은 인터럽트(IRQ_NONE)에 대해서만 폴링한다.
 *   2 — 처리된 인터럽트에 대해서도 폴링한다. 단, 0 번이거나 드라이버가
 *       IRQF_IRQPOLL 로 요청한 선에 한한다.
 *
 * 2 가 왜 처리된 것까지 보는가: 공유 선에서 A 장치가 처리했더라도 B 장치도
 * 함께 울렸을 수 있다. B 는 다른 선에 잘못 배선되어 있어 응답받지 못한다.
 * 그런 경우를 잡으려면 처리 여부와 무관하게 폴링해야 한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 서술자 락을 쥐지 않은 상태다.
 *
 * 호출 체인:
 *   note_interrupt() → [이 함수]
 */
static inline bool try_misrouted_irq(unsigned int irq, struct irq_desc *desc,
				     irqreturn_t action_ret)
{
	struct irqaction *action;	/* [한국어] 첫 핸들러. IRQF_IRQPOLL 검사에 쓴다 */

	if (!irqfixup)	/* [한국어] 부트 인자로 켜지 않았으면 */
		return false;	/* [한국어] 대부분의 시스템이 여기서 끝난다 */

	/* We didn't actually handle the IRQ - see if it was misrouted? */
	/* [한국어] (위 영어 주석) 처리되지 않았으면 잘못 배선을 의심할 만하다.
	 * irqfixup 이 1 이든 2 든 이 경우는 항상 폴링한다. */
	if (action_ret == IRQ_NONE)	/* [한국어] 아무도 자기 것이라 하지 않았는가 */
		return true;	/* [한국어] 다른 선에 진짜 주인이 있을 수 있다 */

	/*
	 * But for 'irqfixup == 2' we also do it for handled interrupts if
	 * they are marked as IRQF_IRQPOLL (or for irq zero, which is the
	 * traditional PC timer interrupt.. Legacy)
	 */
	/* [한국어] (위 영어 주석에 이어) irqfixup 이 2 면 처리된 것도 폴링한다.
	 *
	 * 왜인가: 공유 선에서 A 가 처리했더라도 B 도 함께 울렸을 수 있다.
	 * B 가 다른 선에 잘못 배선되어 있으면 영원히 응답받지 못한다.
	 *
	 * 0 번을 특별 취급하는 이유: 전통적인 PC 타이머 인터럽트다. 옛 PC
	 * 에서는 이 선이 가장 자주 울려, 잘못 배선된 장치를 발견할 기회가
	 * 많았다. 원 주석의 "Legacy" 가 그 사연을 말한다. */
	if (irqfixup < 2)	/* [한국어] irqpoll 수준이 아니면 */
		return false;	/* [한국어] 처리된 인터럽트는 건드리지 않는다 */

	if (!irq)	/* [한국어] 전통적인 PC 타이머 선인가 */
		return true;	/* [한국어] 가장 자주 울리는 선이라 폴링 기회로 삼는다 */

	/*
	 * Since we don't get the descriptor lock, "action" can
	 * change under us.
	 */
	/* [한국어] (위 영어 주석에 이어) 락 없이 읽으므로 값이 바뀔 수 있다.
	 *
	 * READ_ONCE 로 한 번에 읽는 이유: 컴파일러가 이 읽기를 나누면 반쯤
	 * 갱신된 포인터를 볼 수 있다. 한 번에 읽으면 옛 값이거나 새 값이지
	 * 그 사이의 쓰레기는 아니다.
	 *
	 * 그래도 그 포인터가 가리키는 action 이 곧바로 해제될 수 있지 않은가?
	 * 이 시점에 IRQD_IRQ_INPROGRESS 가 서 있어 free_irq() 가
	 * synchronize_irq() 에서 기다린다. 그래서 지금 보이는 action 은
	 * 이 처리가 끝날 때까지 유효하다. */
	action = READ_ONCE(desc->action);	/* [한국어] 락 없이 한 번에 읽는다. INPROGRESS 가 해제를 막고 있다 */
	return action && (action->flags & IRQF_IRQPOLL);	/* [한국어] 드라이버가 폴링 대상으로 표시한 선인가 */
}

#define SPURIOUS_DEFERRED	0x80000000	/* [한국어] threads_handled_last 의 최상위 비트를 "지연 판정 진행 중" 표시로 빌려 쓴다. 아래 note_interrupt 의 스레드 처리에서 쓰이며, 그 비트를 쓰는 이유는 그곳 주석에 있다 */

/*
 * [한국어]
 * note_interrupt - 인터럽트 처리 결과를 집계하고 필요하면 그 선을 끈다
 *
 * @desc:       방금 처리한 인터럽트의 서술자.
 * @action_ret: 모든 핸들러의 반환값을 OR 로 모은 것.
 *
 * 이 파일의 진입점이며, 인터럽트마다 한 번씩 불린다. 하는 일은 셋이다.
 *
 *   1. 규격 밖 반환값을 잡아낸다.
 *   2. 처리되지 않은 인터럽트의 수를 센다.
 *   3. 그 수가 임계값을 넘으면 선을 끄고 되살리기 타이머를 건다.
 *
 * 스레드 핸들러 때문에 판정이 한 번 미뤄지는 것이 이 함수의 가장 어려운
 * 부분이며, 그 구간의 긴 영어 주석이 이유를 설명한다. 요약하면 1차 핸들러가
 * IRQ_WAKE_THREAD 를 돌려준 시점에는 스레드가 아직 돌지 않아 "처리되었는가"
 * 를 알 수 없다는 것이다.
 *
 * 오탐 카운터의 리셋 규칙도 중요하다. 0.1초 넘게 처리되지 않은 인터럽트가
 * 없었으면 카운터를 1 로 되돌린다 — 그러지 않으면 버스 비동기성 때문에
 * 아주 드물게 생기는 오탐이 몇 달에 걸쳐 쌓여, 멀쩡한 시스템의 인터럽트가
 * 어느 날 갑자기 꺼진다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 서술자 락을 쥐지 않은 상태이며
 * IRQD_IRQ_INPROGRESS 가 대신 보호한다.
 *
 * 호출 체인:
 *   handle_irq_event_percpu() (kernel/irq/handle.c) → [이 함수]
 *     → report_bad_irq()/misrouted_irq()/irq_disable()
 */
void note_interrupt(struct irq_desc *desc, irqreturn_t action_ret)
{
	unsigned int irq;	/* [한국어] 인터럽트 번호. 아래에서 두 번 쓰므로 미리 꺼내 둔다 */

	/* [한국어] 두 경우는 집계하지 않는다.
	 *
	 *   POLL_INPROGRESS — 지금 우리가 폴링으로 부른 호출이다. 진짜
	 *     인터럽트가 아니므로 세면 통계가 오염된다.
	 *   is_polled       — 드라이버가 폴링을 병행한다고 선언한 선이다.
	 *     정상 동작 중에도 IRQ_NONE 이 잦아, 세면 멀쩡한 선이 꺼진다. */
	if (desc->istate & IRQS_POLL_INPROGRESS || irq_settings_is_polled(desc))	/* [한국어] 폴링으로 부른 호출이거나 폴링 병행 선인가 */
		return;	/* [한국어] 집계 대상이 아니다 */

	if (bad_action_ret(action_ret)) {	/* [한국어] 드라이버가 규격 밖의 값을 돌려줬는가 */
		report_bad_irq(desc, action_ret);	/* [한국어] 코드 버그다. 전체 100 번까지만 알린다 */
		return;	/* [한국어] 값을 믿을 수 없으므로 아래 집계를 하지 않는다 */
	}

	/*
	 * We cannot call note_interrupt from the threaded handler
	 * because we need to look at the compound of all handlers
	 * (primary and threaded). Aside of that in the threaded
	 * shared case we have no serialization against an incoming
	 * hardware interrupt while we are dealing with a threaded
	 * result.
	 *
	 * So in case a thread is woken, we just note the fact and
	 * defer the analysis to the next hardware interrupt.
	 *
	 * The threaded handlers store whether they successfully
	 * handled an interrupt and we check whether that number
	 * changed versus the last invocation.
	 *
	 * We could handle all interrupts with the delayed by one
	 * mechanism, but for the non forced threaded case we'd just
	 * add pointless overhead to the straight hardirq interrupts
	 * for the sake of a few lines less code.
	 */
	/* [한국어] (위 긴 영어 주석에 이어) 스레드 핸들러가 만드는 문제와 그 해법.
	 *
	 * 문제: 1차 핸들러가 IRQ_WAKE_THREAD 를 돌려준 시점에 스레드는 아직
	 * 돌지 않았다. 그 인터럽트가 실제로 처리될지는 스레드가 끝나 봐야
	 * 안다. 그런데 이 함수는 지금 판정해야 한다.
	 *
	 * 스레드에서 부르면 안 되는 이유는 두 가지로 원 주석에 있다:
	 *   1차와 스레드 핸들러 전체의 결과를 함께 봐야 하는데, 스레드는
	 *     자기 것만 안다.
	 *   공유 선에서는 스레드 결과를 다루는 동안 새 하드웨어 인터럽트가
	 *     들어오는 것을 막을 수단이 없다.
	 *
	 * 해법: 판정을 다음 하드웨어 인터럽트로 미룬다. 스레드는 자기가
	 * 처리했는지를 threads_handled 카운터에 기록해 두고, 다음 인터럽트
	 * 때 그 값이 지난번과 달라졌는지 본다. 달라졌으면 스레드가 처리한
	 * 것이고, 그대로면 아무도 처리하지 않은 것이다.
	 *
	 * 마지막 문단이 설계 판단을 말한다: 모든 인터럽트를 이 "한 번 미루기"
	 * 방식으로 다룰 수도 있었지만, 스레드를 쓰지 않는 보통의 인터럽트에
	 * 불필요한 부담을 지우게 된다. 코드 몇 줄을 아끼려고 핫패스를 느리게
	 * 하지는 않겠다는 것이다. */
	if (action_ret & IRQ_WAKE_THREAD) {	/* [한국어] 스레드를 깨운 핸들러가 하나라도 있는가 */
		/*
		 * There is a thread woken. Check whether one of the
		 * shared primary handlers returned IRQ_HANDLED. If
		 * not we defer the spurious detection to the next
		 * interrupt.
		 */
		/* [한국어] (위 영어 주석) 1차 핸들러 중 처리한 것이 있는지 먼저 본다.
		 *
		 * action_ret == IRQ_WAKE_THREAD 는 "정확히 그 값만" 이라는 뜻이다.
		 * IRQ_HANDLED 가 섞여 있으면 OR 결과가 달라지므로 이 조건이 거짓이 된다.
		 * 즉 이 안쪽은 "스레드만 깨웠고 1차 중에는 처리한 것이 없다" 는 경우다. */
		if (action_ret == IRQ_WAKE_THREAD) {	/* [한국어] 오직 스레드 깨우기만 있었는가 */
			int handled;	/* [한국어] 스레드들이 지금까지 처리한 횟수 */
			/*
			 * We use bit 31 of thread_handled_last to
			 * denote the deferred spurious detection
			 * active. No locking necessary as
			 * thread_handled_last is only accessed here
			 * and we have the guarantee that hard
			 * interrupts are not reentrant.
			 */
			/* [한국어] (위 영어 주석에 이어) 최상위 비트를 표시로 빌려 쓴다.
			 *
			 * 왜 별도 변수를 두지 않는가: 서술자는 인터럽트마다 하나씩
			 * 있어 크기가 곧 메모리 비용이다. 한 비트를 위해 필드를
			 * 더하는 것보다 남는 자리를 빌리는 편이 낫고, 처리 횟수의
			 * 최상위 비트는 실질적으로 쓰이지 않는다.
			 *
			 * 락이 필요 없는 이유도 원 주석에 있다: 이 필드는 여기서만
			 * 접근하고, 하드 인터럽트는 같은 선에 대해 재진입하지 않는다.
			 *
			 * 첫 번째 방문이면 표시만 남기고 물러난다 — 비교할 이전
			 * 값이 없기 때문이다. 그것이 "한 번 미루기" 의 시작점이다. */
			if (!(desc->threads_handled_last & SPURIOUS_DEFERRED)) {	/* [한국어] 지연 판정이 아직 시작되지 않았는가 */
				desc->threads_handled_last |= SPURIOUS_DEFERRED;	/* [한국어] 표시만 남긴다 */
				return;	/* [한국어] 비교할 이전 값이 없어 이번에는 판정하지 않는다 */
			}
			/*
			 * Check whether one of the threaded handlers
			 * returned IRQ_HANDLED since the last
			 * interrupt happened.
			 *
			 * For simplicity we just set bit 31, as it is
			 * set in threads_handled_last as well. So we
			 * avoid extra masking. And we really do not
			 * care about the high bits of the handled
			 * count. We just care about the count being
			 * different than the one we saw before.
			 */
			/* [한국어] (위 영어 주석에 이어) 지난번 이후 스레드가 처리했는지 본다.
			 *
			 * 비교 방식이 영리하다. threads_handled 를 읽어 최상위 비트를
			 * 똑같이 세운 뒤 통째로 비교한다. 그러면 마스킹 없이 한 번에
			 * 비교할 수 있다 — 저장된 값에도 그 비트가 서 있기 때문이다.
			 *
			 * 처리 횟수의 상위 비트를 잃는 셈이지만 상관없다. 원 주석대로
			 * "값이 지난번과 다른가" 만 알면 되지 절대값은 의미가 없다. */
			handled = atomic_read(&desc->threads_handled);	/* [한국어] 스레드들이 처리한 누적 횟수 */
			handled |= SPURIOUS_DEFERRED;	/* [한국어] 저장된 값에도 이 비트가 있으므로 맞춰 준다. 마스킹을 피하는 요령이다 */
			if (handled != desc->threads_handled_last) {	/* [한국어] 지난번과 달라졌는가 — 스레드가 그 사이에 처리했다는 뜻이다 */
				action_ret = IRQ_HANDLED;	/* [한국어] 처리된 것으로 판정한다. 아래 오탐 집계를 건너뛰게 된다 */
				/*
				 * Note: We keep the SPURIOUS_DEFERRED
				 * bit set. We are handling the
				 * previous invocation right now.
				 * Keep it for the current one, so the
				 * next hardware interrupt will
				 * account for it.
				 */
				/* [한국어] (위 영어 주석) 표시 비트를 그대로 둔다.
				 *
				 * 왜인가: 지금 판정한 것은 "이전 인터럽트" 에 대한
				 * 것이다. 방금 들어온 이번 인터럽트도 스레드를 깨웠으므로,
				 * 그것에 대한 판정은 또 다음 번으로 미뤄야 한다.
				 * 비트를 유지하면 그 미루기가 계속 이어진다. */
				desc->threads_handled_last = handled;	/* [한국어] 다음 비교의 기준. SPURIOUS_DEFERRED 비트가 함께 저장된다 */
			} else {
				/*
				 * None of the threaded handlers felt
				 * responsible for the last interrupt
				 *
				 * We keep the SPURIOUS_DEFERRED bit
				 * set in threads_handled_last as we
				 * need to account for the current
				 * interrupt as well.
				 */
				/* [한국어] (위 영어 주석) 스레드도 처리하지 않았다.
				 *
				 * 값이 그대로라는 것은 지난 인터럽트 이후 어떤 스레드
				 * 핸들러도 IRQ_HANDLED 를 돌려주지 않았다는 뜻이다.
				 * 그러면 그 인터럽트는 아무도 처리하지 않은 것이다.
				 *
				 * threads_handled_last 를 갱신하지 않는 것에 주의 —
				 * 값이 같으므로 갱신할 것이 없고, 비트도 그대로 남아
				 * 다음 인터럽트의 판정이 이어진다. */
				action_ret = IRQ_NONE;	/* [한국어] 처리되지 않은 것으로 판정한다. 아래 오탐 카운터가 올라간다 */
			}
		} else {
			/*
			 * One of the primary handlers returned
			 * IRQ_HANDLED. So we don't care about the
			 * threaded handlers on the same line. Clear
			 * the deferred detection bit.
			 *
			 * In theory we could/should check whether the
			 * deferred bit is set and take the result of
			 * the previous run into account here as
			 * well. But it's really not worth the
			 * trouble. If every other interrupt is
			 * handled we never trigger the spurious
			 * detector. And if this is just the one out
			 * of 100k unhandled ones which is handled
			 * then we merily delay the spurious detection
			 * by one hard interrupt. Not a real problem.
			 */
			/* [한국어] (위 영어 주석에 이어) 1차 핸들러 중 하나가 처리했다.
			 *
			 * 그러면 이 인터럽트는 확실히 처리된 것이라, 스레드 결과를
			 * 기다릴 이유가 없다. 지연 판정 표시를 지운다.
			 *
			 * 원 주석의 후반부는 정밀도를 일부러 포기한 이유를 말한다.
			 * 이론적으로는 지연 표시가 서 있었다면 이전 인터럽트의 결과도
			 * 함께 따져야 하지만, 그럴 가치가 없다:
			 *   인터럽트가 자주 처리되는 정상 시스템에서는 오탐 감지가
			 *     애초에 발동하지 않는다.
			 *   10만 번 중 처리된 한 번이 하필 여기 걸리더라도, 감지가
			 *     하드웨어 인터럽트 한 번만큼 늦어질 뿐이다. */
			desc->threads_handled_last &= ~SPURIOUS_DEFERRED;	/* [한국어] 지연 판정을 끝낸다. 이 인터럽트는 확실히 처리되었다 */
		}
	}

	if (unlikely(action_ret == IRQ_NONE)) {	/* [한국어] 아무도 처리하지 않았는가. 정상 시스템에서는 드물다 */
		/*
		 * If we are seeing only the odd spurious IRQ caused by
		 * bus asynchronicity then don't eventually trigger an error,
		 * otherwise the counter becomes a doomsday timer for otherwise
		 * working systems
		 */
		/* [한국어] (위 영어 주석에 이어) 드문 오탐이 쌓여 멀쩡한 시스템을
		 * 죽이지 않게 하는 장치다.
		 *
		 * 무슨 문제인가: 버스의 비동기성 때문에 아주 드물게 원인 없는
		 * 인터럽트가 생긴다. 그것 자체는 무해하다. 그런데 카운터가 계속
		 * 쌓이기만 하면, 몇 달 동안 정상 동작한 시스템에서 어느 날 갑자기
		 * 10만 번에 도달해 인터럽트가 꺼진다. 원 주석의 "doomsday timer"
		 * 라는 표현이 그것이다.
		 *
		 * 그래서 0.1초 넘게 처리되지 않은 인터럽트가 없었으면 카운터를
		 * 1 로 되돌린다. 진짜로 막힌 선은 0.1초 안에 수천 번 들어오므로
		 * 리셋될 틈이 없고, 드문 오탐은 매번 리셋되어 쌓이지 않는다. */
		if (time_after(jiffies, desc->last_unhandled + HZ/10))	/* [한국어] 지난 미처리로부터 0.1초가 넘었는가 */
			desc->irqs_unhandled = 1;	/* [한국어] 되돌린다. 0 이 아니라 1 인 것은 지금 이 인터럽트를 세기 때문이다 */
		else
			desc->irqs_unhandled++;	/* [한국어] 연속된 미처리다. 계속 쌓는다 */
		desc->last_unhandled = jiffies;	/* [한국어] 다음 판정의 기준 시각 */
	}

	irq = irq_desc_get_irq(desc);	/* [한국어] 아래 두 곳에서 쓸 번호 */
	if (unlikely(try_misrouted_irq(irq, desc, action_ret))) {	/* [한국어] 잘못 배선 찾기를 할 조건인가. irqfixup 을 켜지 않았으면 언제나 거짓이다 */
		int ok = misrouted_irq(irq);	/* [한국어] 다른 선들을 폴링해 진짜 주인을 찾는다. 매우 비싸다 */
		if (action_ret == IRQ_NONE)	/* [한국어] 이번 인터럽트가 처리되지 않았는데 */
			desc->irqs_unhandled -= ok;	/* [한국어] 다른 선에서 처리되었다면 미처리로 세지 않는다. 잘못 배선된 것이 실제로 처리된 셈이기 때문이다 */
	}

	if (likely(!desc->irqs_unhandled))	/* [한국어] 미처리가 없으면 여기서 끝난다. 정상 시스템의 압도적 다수가 이 경로다 */
		return;

	/* Now getting into unhandled irq detection */
	/* [한국어] (위 영어 주석) 여기부터가 실제 감지 로직이다.
	 *
	 * 두 카운터가 함께 쓰인다:
	 *   irq_count       — 이 선에 들어온 인터럽트의 총 수.
	 *   irqs_unhandled  — 그중 처리되지 않은 수.
	 * 10만 번마다 한 번씩 둘의 비율을 본다. */
	desc->irq_count++;	/* [한국어] 총 횟수를 센다. 미처리가 하나라도 있을 때만 세므로 정확한 총계는 아니다 */
	if (likely(desc->irq_count < 100000))	/* [한국어] 아직 10만 번이 안 됐으면 */
		return;	/* [한국어] 판정을 미룬다 */

	desc->irq_count = 0;	/* [한국어] 다음 10만 번을 위해 리셋한다 */
	if (unlikely(desc->irqs_unhandled > 99900)) {	/* [한국어] 10만 번 중 99,900 번 이상이 미처리인가. 나머지 100 번의 여유는 공유 선의 정상 장치를 위한 것이다 */
		/*
		 * The interrupt is stuck
		 */
		/* [한국어] (위 영어 주석) 이 선은 막혔다. 사실상 고장 판정이다. */
		__report_bad_irq(desc, action_ret);	/* [한국어] 등록된 모든 핸들러의 이름과 스택을 남긴다. 사용자가 원인을 찾을 유일한 단서다 */
		/*
		 * Now kill the IRQ
		 */
		/* [한국어] (위 영어 주석) 그 선을 죽인다.
		 *
		 * pr_emerg 를 쓰는 이유: 이것은 시스템에 실질적인 영향을 주는
		 * 조치다. 그 선의 장치들이 이 순간부터 동작하지 않으므로,
		 * 가장 높은 수준으로 알려 사용자가 놓치지 않게 한다. */
		pr_emerg("Disabling IRQ #%d\n", irq);	/* [한국어] 최고 수준의 경고. 이 선의 장치들이 이제 동작하지 않는다 */
		desc->istate |= IRQS_SPURIOUS_DISABLED;	/* [한국어] "오탐 때문에 껐다" 고 표시한다. 아래 타이머가 이 표시를 보고 되살려 본다 */
		desc->depth++;	/* [한국어] 비활성 깊이를 올린다. 사용자가 enable_irq() 로 되살릴 때 짝이 맞아야 한다 */
		irq_disable(desc);	/* [한국어] 실제로 끈다 */

		mod_timer(&poll_spurious_irq_timer, jiffies + POLL_SPURIOUS_IRQ_INTERVAL);	/* [한국어] 되살리기 타이머를 건다. 0.1초마다 이 선을 폴링해 정상으로 돌아왔는지 본다 */
	}
	desc->irqs_unhandled = 0;	/* [한국어] 다음 10만 번을 위해 리셋한다. 임계값을 넘지 않은 경우에도 여기를 지난다 */
}

bool noirqdebug __read_mostly;	/* [한국어] 오탐 감지 전체를 끄는 스위치. handle.c 가 이 값을 보고 note_interrupt 를 부를지 정한다. 실행 중에도 module_param 으로 바꿀 수 있다 */

/*
 * [한국어]
 * noirqdebug_setup - "noirqdebug" 부트 인자를 처리한다
 *
 * @str:    인자의 값 부분. 이 인자는 값을 받지 않아 쓰이지 않는다.
 * @return: 항상 1 (인자를 소비했음).
 *
 * 오탐 감지를 통째로 끈다. 언제 필요한가: 오탐 감지가 오작동해 멀쩡한
 * 인터럽트를 꺼 버리는 하드웨어가 있다. 그런 시스템에서 부팅을 살리는
 * 마지막 수단이다.
 *
 * 끄면 무슨 일이 생기는가: 진짜로 막힌 인터럽트가 있어도 커널이 개입하지
 * 않아, 그 선이 CPU 를 계속 점유한다. 그래서 원인을 아는 경우에만 써야 한다.
 *
 * __init 이 붙지 않은 것에 주의: 아래 module_param 으로 실행 중에도 이
 * 값을 바꿀 수 있어, 이 함수는 부팅 전용이지만 변수는 그렇지 않다.
 *
 * 실행 컨텍스트: 부팅 초기의 부트 인자 파싱.
 *
 * 호출 체인:
 *   parse_args() → __setup 표 → [이 함수]
 */
int noirqdebug_setup(char *str)
{
	noirqdebug = 1;	/* [한국어] 감지를 끈다. handle.c 가 이 값을 보고 note_interrupt 호출을 건너뛴다 */
	pr_info("IRQ lockup detection disabled\n");	/* [한국어] 사용자가 이 상태를 잊지 않도록 알린다 */
	return 1;	/* [한국어] 인자를 소비했음을 알린다 */
}
__setup("noirqdebug", noirqdebug_setup);	/* [한국어] 부트 인자로 등록한다 */
module_param(noirqdebug, bool, 0644);	/* [한국어] /sys/module/kernel/parameters/noirqdebug 로 실행 중에도 바꿀 수 있게 한다. 0644 라 root 만 쓴다 */
MODULE_PARM_DESC(noirqdebug, "Disable irq lockup detection when true");	/* [한국어] modinfo 와 sysfs 에 보일 설명 */

/*
 * [한국어]
 * irqfixup_setup - "irqfixup" 부트 인자를 처리한다
 *
 * @str:    인자의 값 부분. 쓰이지 않는다.
 * @return: 항상 1.
 *
 * 잘못 배선된 인터럽트 대응을 1 수준으로 켠다. 처리되지 않은 인터럽트가
 * 있을 때 다른 선들을 폴링해 진짜 주인을 찾는다.
 *
 * PREEMPT_RT 에서 거부하는 이유: 폴링은 다른 선의 핸들러를 임의의 시점에
 * 강제로 부른다. 실시간 커널은 인터럽트 지연 시간을 보장해야 하는데,
 * 그 보장이 통째로 무너진다.
 *
 * 두 줄의 경고를 찍는 것에 주의: 이 옵션은 성능에 실질적인 영향을 준다.
 * 진단 목적으로만 써야 하며, 사용자가 그것을 알고 켰는지 확인할 방법이
 * 로그밖에 없다.
 *
 * 실행 컨텍스트: 부팅 초기의 부트 인자 파싱.
 *
 * 호출 체인:
 *   parse_args() → __setup 표 → [이 함수]
 */
static int __init irqfixup_setup(char *str)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {	/* [한국어] 실시간 커널인가. IS_ENABLED 라 컴파일 시 상수로 접힌다 */
		pr_warn("irqfixup boot option not supported with PREEMPT_RT\n");	/* [한국어] 폴링이 실시간 지연 보장을 무너뜨리므로 거부한다 */
		return 1;	/* [한국어] 인자는 소비했지만 켜지 않았다 */
	}
	irqfixup = 1;	/* [한국어] 처리되지 않은 인터럽트에 대해서만 폴링한다 */
	pr_warn("Misrouted IRQ fixup support enabled.\n");	/* [한국어] 켜졌음을 알린다 */
	pr_warn("This may impact system performance.\n");	/* [한국어] 성능 영향을 경고한다. 진단 목적으로만 써야 한다 */
	return 1;	/* [한국어] 인자를 소비했다 */
}
__setup("irqfixup", irqfixup_setup);	/* [한국어] 부트 인자로 등록 */
module_param(irqfixup, int, 0644);	/* [한국어] 실행 중에도 수준을 바꿀 수 있다. MODULE_PARM_DESC 가 없는 것은 오래된 코드의 누락이다 */

/*
 * [한국어]
 * irqpoll_setup - "irqpoll" 부트 인자를 처리한다
 *
 * @str:    인자의 값 부분. 쓰이지 않는다.
 * @return: 항상 1.
 *
 * 위 irqfixup 보다 한 단계 공격적인 2 수준을 켠다. 처리된 인터럽트에
 * 대해서도 폴링하므로, 공유 선에서 다른 장치가 함께 울린 경우까지 잡는다.
 *
 * 경고 문구가 위와 미묘하게 다르다 — "significantly impact" 다. 모든
 * 인터럽트마다 전체 선을 폴링할 수 있어 부담이 훨씬 크기 때문이다.
 *
 * 위 __report_bad_irq() 가 "nobody cared" 메시지에서 권하는 것이 바로
 * 이 옵션이다. 인터럽트가 꺼지는 문제를 만났을 때 원인을 찾는 수단이다.
 *
 * PREEMPT_RT 에서 거부하는 이유는 위와 같다.
 *
 * 실행 컨텍스트: 부팅 초기의 부트 인자 파싱.
 *
 * 호출 체인:
 *   parse_args() → __setup 표 → [이 함수]
 */
static int __init irqpoll_setup(char *str)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {	/* [한국어] 실시간 커널에서는 거부한다 */
		pr_warn("irqpoll boot option not supported with PREEMPT_RT\n");	/* [한국어] 이유는 irqfixup 과 같다 */
		return 1;	/* [한국어] 켜지 않고 물러난다 */
	}
	irqfixup = 2;	/* [한국어] 처리된 인터럽트에 대해서도 폴링한다 */
	pr_warn("Misrouted IRQ fixup and polling support enabled\n");	/* [한국어] 켜졌음을 알린다 */
	pr_warn("This may significantly impact system performance\n");	/* [한국어] irqfixup 보다 강한 표현 — 부담이 훨씬 크다 */
	return 1;	/* [한국어] 인자를 소비했다 */
}
__setup("irqpoll", irqpoll_setup);	/* [한국어] 부트 인자로 등록. 이쪽은 module_param 이 없어 실행 중에는 바꿀 수 없다 — irqfixup 변수를 통해 간접적으로만 가능하다 */
