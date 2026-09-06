// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner
 *
 * This file contains the IRQ-resend code
 *
 * If the interrupt is waiting to be processed, we try to re-run it.
 * We can't directly run it from here since the caller might be in an
 * interrupt-protected region. Not all irq controller chips can
 * retrigger interrupts at the hardware level, so in those cases
 * we allow the resending of IRQs via a tasklet.
 */
/*
 * [한국어 설명] 놓친 인터럽트를 다시 살려 내는 재전송 계층 (resend.c)
 *
 * === 파일의 역할 ===
 * 인터럽트가 비활성화된 동안 도착해 처리되지 못한 것을, 다시 켤 때 잃지
 * 않고 처리하도록 되살린다.
 *
 * 왜 필요한가: disable_irq() 로 꺼 둔 사이에 하드웨어가 인터럽트를 보내면
 * 커널은 그것을 처리하지 못하고 IRQS_PENDING 으로 표시만 해 둔다. 하드웨어는
 * 그 인터럽트를 다시 보내 주지 않으므로, 다시 켤 때 소프트웨어가 되살리지
 * 않으면 그 이벤트는 영원히 사라진다. 장치 쪽에서는 완료 통지를 기다리는
 * 요청이 영원히 끝나지 않는 형태로 나타난다.
 *
 * 두 가지 방법이 있고 이 파일이 둘을 모두 다룬다.
 *   하드웨어 재트리거 — 컨트롤러에게 "이 인터럽트를 다시 내라"고 명령한다.
 *     가능하면 이쪽이 항상 낫다. 실제 인터럽트와 완전히 같은 경로를 탄다.
 *   소프트웨어 재전송 — 컨트롤러가 그것을 못 하면, 커널이 tasklet 에서
 *     흐름 제어 핸들러를 직접 부른다. 위 영어 주석이 말하는 방식이다.
 *
 * 왜 그 자리에서 바로 부르지 않는가: 영어 주석의 설명대로 호출자가 인터럽트를
 * 끈 구간에 있을 수 있다. 그 상태에서 핸들러를 부르면 락 순서가 뒤엉키고,
 * 핸들러가 기대하는 문맥과도 어긋난다. 그래서 tasklet 으로 미룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트를 다시 켜는 경로의 끝에 있다:
 *
 *   enable_irq() / irq_startup()          (manage.c, chip.c)
 *     ↓ IRQS_PENDING 이 서 있으면
 *   check_irq_resend()                    ← **이 파일**
 *     ↓ 먼저 하드웨어에 시도
 *   try_retrigger() → chip->irq_retrigger 또는 도메인 계층
 *     ↓ 실패하면
 *   irq_sw_resend() → 목록에 넣고 tasklet 예약
 *     ↓ 나중에 softirq 문맥에서
 *   resend_irqs() → desc->handle_irq()    — 실제 인터럽트처럼 처리
 *
 * 별개의 진입점이 하나 더 있다: irq_inject_interrupt() 는 시험 목적으로
 * 인터럽트를 인위적으로 발생시킨다.
 *
 * 실행 컨텍스트: check_irq_resend 는 인터럽트가 꺼지고 서술자 락을 쥔
 * 상태에서 불린다. resend_irqs 는 softirq 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — IRQS_PENDING/IRQS_REPLAY, irq_settings_is_level 등.
 *   chip 드라이버의 irq_retrigger 콜백.
 *   tasklet(softirq) 기반 구조.
 *
 * 이 파일에 의존하는 곳:
 *   chip.c   — irq_startup 이 재전송을 요청한다.
 *   manage.c — __enable_irq 가 재전송을 요청한다.
 *   irqdesc.c — 서술자 생성·해제 시 irq_resend_init/clear_irq_resend.
 *
 * 공유 상태: irq_resend_list 전역 목록과 그것을 지키는 irq_resend_lock.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_resend_list / irq_resend_lock — 소프트웨어 재전송 대기 목록과 그 락.
 * resend_irqs()        — tasklet 본체. 목록을 비우며 핸들러를 부른다.
 * irq_sw_resend()      — 서술자를 목록에 넣고 tasklet 을 예약한다.
 * clear_irq_resend()   — 서술자를 목록에서 뺀다. 해제 전에 반드시 불러야 한다.
 * irq_resend_init()    — 목록 고리를 초기화한다.
 * try_retrigger()      — 하드웨어 재트리거를 시도한다.
 * check_irq_resend()   — 이 파일의 진입점. 조건을 따지고 두 방법을 순서대로 시도한다.
 * irq_inject_interrupt() — 시험용으로 인터럽트를 인위 발생시킨다.
 *
 * IRQS_PENDING 과 IRQS_REPLAY 의 구분이 이 파일을 읽는 열쇠다. PENDING 은
 * "재전송이 필요하다", REPLAY 는 "재전송을 이미 발행했다"이다. 후자가 없으면
 * 재전송이 재전송을 부르는 폭주가 생긴다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc, irqd_* 접근자, IRQCHIP_STATE_* */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL — irq_inject_interrupt 를 공개한다 */
#include <linux/random.h>	/* [한국어] 역사적 잔재. 예전에는 인터럽트 타이밍을 엔트로피로 썼다 */
#include <linux/interrupt.h>	/* [한국어] tasklet 구조와 DECLARE_TASKLET */

#include "internals.h"	/* [한국어] IRQS_* 비트, irq_settings_is_level, scoped_irqdesc_get_and_buslock 등 */

/* [한국어] 소프트웨어 재전송을 지원하는 아키텍처인가.
 *
 * HARDIRQS_SW_RESEND 를 켠 아키텍처(ARM 계열 대부분)는 컨트롤러가 하드웨어
 * 재트리거를 지원하지 않는 경우가 많아, tasklet 으로 흉내 내는 경로가 필요하다.
 *
 * 끈 아키텍처(x86 등)는 컨트롤러가 재트리거를 지원하므로 이 경로가 아예
 * 컴파일되지 않는다. 아래 #else 쪽에서 irq_sw_resend 가 -EINVAL 을 돌려주는
 * 상수 구현이 되어, check_irq_resend 의 그 분기가 사라진다. */
#ifdef CONFIG_HARDIRQS_SW_RESEND	/* [한국어] 소프트웨어 재전송을 쓰는 빌드 */

/* hlist_head to handle software resend of interrupts: */
/* [한국어] (위 영어 주석) 재전송을 기다리는 서술자들의 목록.
 *
 * hlist(해시 리스트)를 쓰는 이유: 항목이 목록에 들어 있는지를 O(1) 로 알 수
 * 있다. 아래 hlist_unhashed() 가 그것이며, 같은 서술자를 두 번 넣지 않는
 * 검사에 쓴다. 보통의 list_head 로는 그 검사를 하려면 목록을 훑어야 한다. */
static HLIST_HEAD(irq_resend_list);
/* [한국어] 위 목록을 지키는 스핀락.
 *
 * raw 스핀락인 이유: PREEMPT_RT 커널에서도 잠들지 않아야 한다. 이 락을
 * 잡는 곳 중 하나가 인터럽트를 끈 상태의 check_irq_resend 경로라, 잠드는
 * 락으로는 쓸 수 없다. */
static DEFINE_RAW_SPINLOCK(irq_resend_lock);

/*
 * Run software resends of IRQ's
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * resend_irqs - 재전송 대기 목록을 비우며 각 인터럽트의 핸들러를 부른다
 *
 * @unused: tasklet API 가 요구하는 인자. 이 tasklet 은 전역 목록을 쓰므로
 *          문맥을 인자로 받을 필요가 없다.
 *
 * tasklet 본체다. 아래 irq_sw_resend() 가 목록에 서술자를 넣고 이 tasklet 을
 * 예약하면, softirq 처리 시점에 여기가 실행된다.
 *
 * 락을 놓았다 다시 잡는 것이 이 함수의 핵심이다. desc->handle_irq() 는
 * 흐름 제어 핸들러이고, 그 안에서 서술자 락을 잡고 드라이버 핸들러까지
 * 부른다. 그 동안 이 전역 락을 쥐고 있으면 다른 CPU 의 재전송이 전부 막히고,
 * 락 순서가 뒤엉켜 교착이 날 수도 있다.
 *
 * 그래서 목록에서 항목을 떼어 낸 뒤 락을 놓고, 핸들러를 부르고, 다시 잡는다.
 * 항목을 먼저 떼어 두었으므로 락을 놓은 사이에 다른 CPU 가 같은 서술자를
 * 다시 처리할 위험은 없다.
 *
 * while 로 도는 이유: 락을 놓은 사이에 새 항목이 추가될 수 있다. 매번
 * 목록이 비었는지 다시 확인해야 한다.
 *
 * 실행 컨텍스트: softirq(tasklet) 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   irq_sw_resend() → tasklet_schedule() → softirq → [resend_irqs]
 *     → desc->handle_irq() (흐름 제어 핸들러)
 */
static void resend_irqs(struct tasklet_struct *unused)
{
	guard(raw_spinlock_irq)(&irq_resend_lock);	/* [한국어] 목록을 지키는 락. guard 라 함수를 벗어날 때 자동으로 풀린다 */
	while (!hlist_empty(&irq_resend_list)) {	/* [한국어] 아래에서 락을 놓는 사이 새 항목이 들어올 수 있어 매번 다시 확인한다 */
		struct irq_desc *desc;	/* [한국어] 이번에 처리할 서술자 */

		desc = hlist_entry(irq_resend_list.first, struct irq_desc,  resend_node);	/* [한국어] 목록의 첫 항목에서 그것을 품은 서술자로 되짚는다 */
		hlist_del_init(&desc->resend_node);	/* [한국어] 목록에서 뗀다. _init 판이라 고리가 unhashed 상태가 되어, 아래 irq_sw_resend 의 중복 검사가 올바르게 동작한다 */

		raw_spin_unlock(&irq_resend_lock);	/* [한국어] 핸들러를 부르기 전에 전역 락을 놓는다. 쥔 채로 부르면 다른 CPU 의 재전송이 막히고 락 순서가 뒤엉킨다 */
		desc->handle_irq(desc);	/* [한국어] 흐름 제어 핸들러를 직접 부른다 — 실제 인터럽트가 온 것과 같은 경로를 탄다 */
		raw_spin_lock(&irq_resend_lock);	/* [한국어] 다시 잡는다. guard 가 함수 끝에서 풀 것이므로 균형이 맞는다 */
	}
}

/* Tasklet to handle resend: */
/* [한국어] (위 영어 주석) 위 resend_irqs 를 실행할 tasklet.
 *
 * 전역에 하나만 두는 이유: 재전송은 드문 일이고, 여러 인터럽트가 동시에
 * 재전송을 요청해도 하나의 tasklet 이 목록을 돌며 전부 처리하면 된다.
 * tasklet 은 이미 예약되어 있으면 중복 예약이 무시되므로, 아래
 * tasklet_schedule 을 여러 번 불러도 안전하다.
 *
 * tasklet 은 softirq 위에서 도는 오래된 지연 실행 수단이다. workqueue 와
 * 달리 잠들 수 없지만 그만큼 가볍고, 인터럽트 처리 직후에 실행된다. */
static DECLARE_TASKLET(resend_tasklet, resend_irqs);

/*
 * [한국어]
 * irq_sw_resend - 소프트웨어 재전송을 예약한다
 *
 * @desc:   재전송할 인터럽트의 서술자.
 * @return: 0 이면 예약 성공, -EINVAL 이면 이 인터럽트는 소프트웨어로
 *          재전송할 수 없다.
 *
 * 하드웨어 재트리거가 불가능할 때의 대안이다. 서술자를 전역 목록에 넣고
 * tasklet 을 예약하면, 나중에 softirq 문맥에서 위 resend_irqs 가 처리한다.
 *
 * 두 가지 거부 조건이 있다.
 *
 *   enforce_irqctx — 이 인터럽트의 핸들러가 진짜 하드 인터럽트 문맥을
 *     요구한다는 표시다. tasklet 은 softirq 문맥이라 그 요구를 만족할 수
 *     없어 재전송을 포기한다.
 *
 *   중첩 스레드 인터럽트 — 이 인터럽트는 자체 흐름 제어 핸들러가 없다.
 *     부모의 스레드 핸들러가 대신 불러 주는 구조라, desc->handle_irq() 를
 *     직접 부를 수 없다. 대신 부모를 재전송한다.
 *
 * 두 번째 경우에 desc 를 부모의 것으로 바꿔치기한다는 점이 중요하다. 그
 * 뒤의 목록 삽입은 부모 서술자에 대해 일어난다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   check_irq_resend() → [irq_sw_resend] → tasklet_schedule()
 */
static int irq_sw_resend(struct irq_desc *desc)
{
	/*
	 * Validate whether this interrupt can be safely injected from
	 * non interrupt context
	 */
	/* [한국어] (위 영어 주석) 인터럽트 문맥이 아닌 곳에서 넣어도 안전한지 확인한다.
	 *
	 * enforce_irqctx 는 "이 핸들러는 진짜 하드 인터럽트 문맥에서만 돌아야
	 * 한다"는 표시다. tasklet 은 softirq 문맥이라 그 조건을 만족하지 못한다.
	 * 어기면 핸들러가 기대하는 in_hardirq() 검사나 문맥 가정이 깨진다. */
	if (irqd_is_handle_enforce_irqctx(&desc->irq_data))	/* [한국어] 하드 인터럽트 문맥을 강제하는 인터럽트인가 */
		return -EINVAL;	/* [한국어] softirq 에서 부를 수 없어 재전송을 포기한다 */

	/*
	 * If the interrupt is running in the thread context of the parent
	 * irq we need to be careful, because we cannot trigger it
	 * directly.
	 */
	/* [한국어] (위 영어 주석에 이어) 중첩 스레드 인터럽트는 직접 부를 수 없다.
	 *
	 * 구조: I2C 나 SPI 뒤의 GPIO 확장기 같은 칩의 자식 인터럽트는 자체
	 * 흐름 제어 핸들러가 없다. 부모의 스레드 핸들러가 칩 상태를 읽고
	 * handle_nested_irq() 로 불러 주는 방식이라, desc->handle_irq() 가 NULL 이거나
	 * 직접 부르기에 적절하지 않다.
	 *
	 * 그래서 부모를 재전송한다. 부모가 다시 돌면 칩 상태를 읽고 자식을
	 * 자연스럽게 불러 준다. */
	if (irq_settings_is_nested_thread(desc)) {	/* [한국어] 부모 스레드 안에서 도는 중첩 인터럽트인가 */
		/*
		 * If the parent_irq is valid, we retrigger the parent,
		 * otherwise we do nothing.
		 */
		/* [한국어] (위 영어 주석) 부모가 유효하면 부모를 재전송한다. */
		if (!desc->parent_irq)	/* [한국어] 부모 번호가 기록되어 있지 않으면 */
			return -EINVAL;	/* [한국어] 되살릴 방법이 없다 */

		desc = irq_to_desc(desc->parent_irq);	/* [한국어] 대상을 부모로 바꿔치기한다 — 아래의 목록 삽입은 부모에 대해 일어난다 */
		if (!desc)	/* [한국어] 부모 번호가 유효하지 않으면 */
			return -EINVAL;	/* [한국어] 포기한다 */
	}

	/* Add to resend_list and activate the softirq: */
	/* [한국어] (위 영어 주석) 목록에 넣고 softirq 를 깨운다.
	 *
	 * scoped_guard 로 락 구간을 블록에 가둔 이유: 아래 tasklet_schedule 은
	 * 락 밖에서 불러야 한다. 락 안에서 부르면 그 함수가 잡는 다른 락과
	 * 순서가 얽힐 수 있고, 어차피 목록에 넣은 뒤라면 순서가 중요하지 않다. */
	scoped_guard(raw_spinlock, &irq_resend_lock) {	/* [한국어] 목록을 고치는 동안만 락을 잡는다 */
		if (hlist_unhashed(&desc->resend_node))	/* [한국어] 아직 목록에 없을 때만 넣는다. hlist 를 쓴 이유가 이 O(1) 검사다 */
			hlist_add_head(&desc->resend_node, &irq_resend_list);	/* [한국어] 앞에 넣는다. 순서는 중요하지 않다 — 어차피 전부 처리한다 */
	}
	tasklet_schedule(&resend_tasklet);	/* [한국어] 락 밖에서 예약한다. 이미 예약되어 있으면 중복 예약은 무시되므로 여러 번 불러도 안전하다 */
	return 0;	/* [한국어] 예약 성공. 실제 처리는 나중에 softirq 문맥에서 일어난다 */
}

/*
 * [한국어]
 * clear_irq_resend - 서술자를 재전송 대기 목록에서 뺀다
 *
 * @desc: 대상 서술자.
 *
 * 왜 반드시 필요한가: 서술자가 해제되는데 재전송 목록에 아직 들어 있으면,
 * tasklet 이 나중에 그 항목을 꺼내 이미 반납된 메모리를 역참조한다.
 * use-after-free 이며 재현하기 어려운 형태로 나타난다.
 *
 * 그래서 서술자를 없애는 경로는 반드시 이 함수를 먼저 불러야 한다.
 *
 * hlist_del_init 을 쓰는 이유: 목록에 들어 있지 않은 항목에 대해서도
 * 안전하다. 호출자가 "지금 목록에 있는가"를 확인할 필요가 없다.
 *
 * 실행 컨텍스트: 서술자 해제 경로. 이 함수 자체는 어디서든 안전하다.
 *
 * 호출 체인:
 *   free_desc() (kernel/irq/irqdesc.c) → [이 함수]
 */
void clear_irq_resend(struct irq_desc *desc)
{
	guard(raw_spinlock)(&irq_resend_lock);	/* [한국어] 목록을 지키는 락 */
	hlist_del_init(&desc->resend_node);	/* [한국어] 목록에 없어도 안전하다 — _init 판이 고리를 다시 unhashed 로 만든다 */
}

/*
 * [한국어]
 * irq_resend_init - 서술자의 재전송 목록 고리를 초기화한다
 *
 * @desc: 새로 만든 서술자.
 *
 * 서술자가 만들어질 때 한 번 불린다. 초기화된 고리는 unhashed 상태이며,
 * 그것이 위 irq_sw_resend 의 중복 검사와 clear_irq_resend 의 안전한 삭제를
 * 모두 성립하게 한다.
 *
 * 이 호출을 빠뜨리면 고리에 쓰레기 값이 남아, hlist_unhashed 가 엉뚱한
 * 답을 주고 목록이 깨진다.
 *
 * 실행 컨텍스트: 서술자 생성 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   desc_set_defaults() (kernel/irq/irqdesc.c) → [이 함수]
 */
void irq_resend_init(struct irq_desc *desc)
{
	INIT_HLIST_NODE(&desc->resend_node);	/* [한국어] unhashed 상태로 만든다 — 이것이 중복 검사와 안전한 삭제의 전제다 */
}
#else
/*
 * [한국어]
 * clear_irq_resend - (소프트웨어 재전송 없음) 뺄 목록이 없다
 *
 * @desc: 대상 서술자. 쓰이지 않는다.
 *
 * 이 구성에는 재전송 대기 목록 자체가 없다. 서술자의 resend_node 필드도
 * 쓰이지 않으므로 정리할 것이 없다.
 *
 * 그래도 함수가 존재해야 irqdesc.c 의 해제 경로가 #ifdef 없이 컴파일된다.
 * 인라인이 아니라 실제 함수인 이유: 이 파일은 .c 이고 선언이 internals.h 에
 * 있어, 위쪽 판과 시그니처를 맞추려면 실제 심볼이어야 한다.
 *
 * 실행 컨텍스트: 서술자 해제 경로.
 *
 * 호출 체인:
 *   free_desc() (kernel/irq/irqdesc.c) → [이 빈 구현]
 */
void clear_irq_resend(struct irq_desc *desc) {}
/*
 * [한국어]
 * irq_resend_init - (소프트웨어 재전송 없음) 초기화할 고리가 없다
 *
 * @desc: 새 서술자. 쓰이지 않는다.
 *
 * 위 clear_irq_resend 와 짝을 이루는 빈 구현이다. 목록이 없으니 고리도 없다.
 *
 * 실행 컨텍스트: 서술자 생성 경로.
 *
 * 호출 체인:
 *   desc_set_defaults() (kernel/irq/irqdesc.c) → [이 빈 구현]
 */
void irq_resend_init(struct irq_desc *desc) {}

/*
 * [한국어]
 * irq_sw_resend - (소프트웨어 재전송 없음) 언제나 실패를 답한다
 *
 * @desc:   대상 서술자. 쓰이지 않는다.
 * @return: 항상 -EINVAL.
 *
 * 이 아키텍처는 컨트롤러가 하드웨어 재트리거를 지원한다고 전제한다. 그래서
 * 소프트웨어로 흉내 내는 경로가 아예 없다.
 *
 * -EINVAL 을 상수로 돌려주면 check_irq_resend 의 그 분기가 컴파일러에게
 * 뻔해져, 재전송 실패 처리만 남고 나머지는 사라진다.
 *
 * static 인 이유: 위쪽 판과 달리 이것은 이 파일 안에서만 쓰인다.
 * clear_irq_resend/irq_resend_init 과 달리 internals.h 에 선언이 없다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   check_irq_resend() → [이 빈 구현]
 */
static int irq_sw_resend(struct irq_desc *desc)
{
	return -EINVAL;	/* [한국어] 소프트웨어 재전송을 지원하지 않는다. 하드웨어 재트리거가 실패하면 그것으로 끝이다 */
}
#endif

/*
 * [한국어]
 * try_retrigger - 컨트롤러에게 인터럽트를 다시 내라고 요청한다
 *
 * @desc:   재전송할 인터럽트의 서술자.
 * @return: 0 이 아니면 성공(하드웨어가 다시 낼 것이다), 0 이면 실패.
 *
 * 반환값의 극성에 주의한다. 이 함수는 성공을 0 이 아닌 값으로, 실패를 0 으로
 * 나타낸다 — 커널의 흔한 관례(0 이 성공)와 반대다. chip->irq_retrigger
 * 콜백이 원래 그렇게 정의되어 있어 그것을 그대로 물려받은 것이다.
 *
 * 그래서 호출부인 check_irq_resend 가 `if (!try_retrigger(desc))` 로 검사한다 —
 * 0 이면(실패하면) 소프트웨어 재전송으로 넘어간다는 뜻이다.
 *
 * 두 단계로 시도한다:
 *   1. 이 인터럽트의 chip 이 직접 재트리거를 지원하면 그것을 쓴다.
 *   2. 지원하지 않으면 계층형 도메인의 부모들에게 물어본다. MSI 처럼
 *      여러 단계로 겹친 인터럽트는 아래쪽 단계(벡터 도메인)가 재트리거를
 *      할 줄 아는 경우가 많다.
 *
 * 계층이 없는 빌드에서는 2단계가 상수 0 이 되어, chip 이 지원하지 않으면
 * 곧바로 실패로 끝난다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   check_irq_resend() → [try_retrigger] → chip->irq_retrigger
 *     또는 irq_chip_retrigger_hierarchy() (kernel/irq/chip.c)
 */
static int try_retrigger(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_retrigger)	/* [한국어] 이 단계의 컨트롤러가 재트리거를 지원하는가 */
		return desc->irq_data.chip->irq_retrigger(&desc->irq_data);	/* [한국어] 그 결과를 그대로 돌려준다. 0 이 아니면 성공이라는 극성도 그대로 물려받는다 */

	/* [한국어] chip 이 직접 지원하지 않으면 계층을 거슬러 올라가며 찾아본다.
	 * MSI 처럼 여러 단계로 겹친 인터럽트는 최종 벡터 단계가 재트리거를
	 * 할 줄 아는 경우가 많다. */
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형 도메인을 쓰는 빌드 */
	return irq_chip_retrigger_hierarchy(&desc->irq_data);	/* [한국어] 부모 방향으로 훑으며 재트리거를 아는 단계를 찾는다 */
#else
	return 0;	/* [한국어] 계층이 없으면 물어볼 곳도 없다. 0 은 실패이므로 호출부가 소프트웨어 재전송으로 넘어간다 */
#endif
}

/*
 * IRQ resend
 *
 * Is called with interrupts disabled and desc->lock held.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * check_irq_resend - 밀린 인터럽트가 있으면 재전송한다
 *
 * @desc:   대상 서술자.
 * @inject: 참이면 IRQS_PENDING 이 없어도 강제로 재전송한다. 시험용
 *          irq_inject_interrupt() 만 참으로 부른다.
 * @return: 0 이면 재전송했거나 할 것이 없었다. 음수면 재전송에 실패했다.
 *
 * 이 파일의 진입점이다. 인터럽트를 다시 켜는 경로가 여기를 거쳐, 꺼져 있던
 * 사이에 놓친 인터럽트를 되살린다.
 *
 * 네 가지 관문을 차례로 통과해야 실제 재전송에 이른다.
 *
 *   1. 레벨 트리거가 아닐 것 — 레벨은 원인이 남아 있으면 하드웨어가 알아서
 *      다시 낸다. 소프트웨어가 개입할 이유가 없다.
 *   2. 이미 재전송 중이 아닐 것 (IRQS_REPLAY) — 중복 발행을 막는다.
 *   3. 밀린 것이 있을 것 (IRQS_PENDING), 또는 inject 가 참일 것.
 *   4. 하드웨어나 소프트웨어 재전송 중 하나가 성공할 것.
 *
 * 성공하면 IRQS_REPLAY 를 세운다. 그 비트는 핸들러가 실제로 돌 때 지워지며,
 * 그때까지 다음 재전송을 막는다. 이 방어가 없으면 재전송이 또 재전송을
 * 부르는 폭주가 생긴다.
 *
 * 실행 컨텍스트: 위 영어 주석대로 인터럽트가 꺼져 있고 desc->lock 을 쥔
 * 상태에서 불린다. 이 계약이 지켜지지 않으면 istate 갱신이 경쟁한다.
 *
 * 호출 체인:
 *   __enable_irq() (kernel/irq/manage.c) → [이 함수]
 *   irq_startup() (kernel/irq/chip.c) → [이 함수]
 *   irq_inject_interrupt() (아래) → [이 함수] (inject = true)
 */
int check_irq_resend(struct irq_desc *desc, bool inject)
{
	int err = 0;	/* [한국어] 재전송 결과. 아무것도 하지 않아도 0(성공)이다 */

	/*
	 * We do not resend level type interrupts. Level type interrupts
	 * are resent by hardware when they are still active. Clear the
	 * pending bit so suspend/resume does not get confused.
	 */
	/* [한국어] (위 영어 주석에 이어) 레벨 트리거는 재전송하지 않는다.
	 *
	 * 왜인가: 레벨 인터럽트는 원인이 사라질 때까지 신호가 계속 어서션
	 * 상태다. 언마스크하는 순간 하드웨어가 알아서 다시 낸다. 소프트웨어가
	 * 또 내면 같은 인터럽트가 두 번 처리된다.
	 *
	 * PENDING 을 지우는 이유도 원 주석에 있다 — 절전과 복귀 경로가 그 비트를
	 * 보고 "처리하지 못한 인터럽트가 있다"고 판단하는데, 레벨에서는 그것이
	 * 사실이 아니므로 혼란을 준다.
	 *
	 * -EINVAL 을 돌려주지만 호출자에게는 오류가 아니다. "재전송할 성질의
	 * 인터럽트가 아니다"라는 뜻이며, 대부분의 호출자는 반환값을 무시한다. */
	if (irq_settings_is_level(desc)) {	/* [한국어] 레벨 트리거인가 */
		desc->istate &= ~IRQS_PENDING;	/* [한국어] 밀림 표시를 지운다. 하드웨어가 알아서 낼 것이므로 커널이 기억할 이유가 없다 */
		return -EINVAL;	/* [한국어] 재전송하지 않았음을 알린다 */
	}

	if (desc->istate & IRQS_REPLAY)	/* [한국어] 이미 재전송을 발행해 두었는가 */
		return -EBUSY;	/* [한국어] 그 재전송이 처리될 때까지 기다린다. 이 방어가 없으면 재전송이 재전송을 부르는 폭주가 생긴다 */

	if (!(desc->istate & IRQS_PENDING) && !inject)	/* [한국어] 밀린 것이 없고 강제 주입도 아니면 */
		return 0;	/* [한국어] 할 일이 없다. 성공으로 답한다 — 오류가 아니다 */

	desc->istate &= ~IRQS_PENDING;	/* [한국어] 밀림 표시를 지운다. 지금부터 재전송을 시도하므로 더는 "밀려 있는" 상태가 아니다 */

	if (!try_retrigger(desc))	/* [한국어] 먼저 하드웨어에 맡긴다. 극성에 주의 — 0 이 실패다 */
		err = irq_sw_resend(desc);	/* [한국어] 하드웨어가 못 하면 tasklet 으로 흉내 낸다. 그것도 안 되면 음수가 남는다 */

	/* If the retrigger was successful, mark it with the REPLAY bit */
	/* [한국어] (위 영어 주석) 재전송에 성공했으면 REPLAY 로 표시한다.
	 *
	 * 이 비트가 위 두 번째 관문을 막는다. 핸들러가 실제로 돌면서 지워질
	 * 때까지, 같은 인터럽트에 대한 다음 재전송이 -EBUSY 로 거절된다.
	 *
	 * 실패했다면(err 이 음수) 표시하지 않는다. 다음 기회에 다시 시도할 수
	 * 있어야 하기 때문이다. */
	if (!err)	/* [한국어] 하드웨어든 소프트웨어든 하나가 성공했는가 */
		desc->istate |= IRQS_REPLAY;	/* [한국어] 재전송이 진행 중임을 표시한다. 핸들러가 돌면서 지운다 */
	return err;	/* [한국어] 0 이면 성공, 음수면 어느 방법으로도 재전송할 수 없었다 */
}

/* [한국어] 인터럽트를 인위적으로 발생시키는 시험용 인터페이스.
 *
 * GENERIC_IRQ_INJECTION 을 켠 커널에서만 컴파일된다. 오류 주입 시험이나
 * 인터럽트 처리 경로의 검증에 쓰며, 아래 kernel-doc 이 강조하듯 디버그와
 * 시험 목적으로만 써야 한다. */
#ifdef CONFIG_GENERIC_IRQ_INJECTION	/* [한국어] 인터럽트 주입 기능을 켠 빌드 */
/**
 * irq_inject_interrupt - Inject an interrupt for testing/error injection
 * @irq:	The interrupt number
 *
 * This function must only be used for debug and testing purposes!
 *
 * Especially on x86 this can cause a premature completion of an interrupt
 * affinity change causing the interrupt line to become stale. Very
 * unlikely, but possible.
 *
 * The injection can fail for various reasons:
 * - Interrupt is not activated
 * - Interrupt is NMI type or currently replaying
 * - Interrupt is level type
 * - Interrupt does not support hardware retrigger and software resend is
 *   either not enabled or not possible for the interrupt.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_inject_interrupt - 시험 목적으로 인터럽트를 인위 발생시킨다
 *
 * @irq:    발생시킬 인터럽트 번호.
 * @return: 0 이면 주입 성공. 음수면 여러 이유 중 하나로 실패했다.
 *
 * 원 주석이 경고하는 x86 의 위험: 인터럽트 친화도 변경은 "다음 인터럽트가
 * 새 CPU 에 도착했다"는 것을 확인해야 완료된다. 인위로 인터럽트를 만들면
 * 그 확인이 실제 인터럽트 없이 이루어져, 옛 벡터로 아직 날아오던 인터럽트가
 * 갈 곳을 잃을 수 있다. 드물지만 가능하다는 것이 원 주석의 뜻이다.
 *
 * 두 가지 방법을 순서대로 시도한다:
 *   1. chip 의 상태 주입 인터페이스 — IRQCHIP_STATE_PENDING 을 직접 세운다.
 *      가장 실제에 가까운 방법이라 먼저 시도한다.
 *   2. 위 check_irq_resend 의 재전송 기구 — 1 이 실패했을 때의 대안이다.
 *
 * 2 에서 두 가지를 확인한다. NMI 가 아닐 것(NMI 는 보통의 경로로 다룰 수
 * 없다)과, activate 되어 있을 것(하드웨어 자원이 배정되지 않은 인터럽트는
 * 발생시켜도 갈 곳이 없다).
 *
 * buslock 판의 scoped guard 를 쓰는 이유: 이 경로는 프로세스 문맥이고,
 * 대상이 I2C 뒤의 칩일 수 있어 버스 락까지 필요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 버스 락 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   debugfs 의 irq injection 인터페이스, 또는 시험 모듈 → [이 함수]
 *     → irq_set_irqchip_state() 또는 check_irq_resend()
 */
int irq_inject_interrupt(unsigned int irq)
{
	int err = -EINVAL;	/* [한국어] 기본값은 실패. 아래 두 경로 중 하나가 성공해야 바뀐다 */

	/* Try the state injection hardware interface first */
	/* [한국어] (위 영어 주석) 먼저 하드웨어의 상태 주입 인터페이스를 시도한다.
	 *
	 * 왜 이쪽이 먼저인가: 컨트롤러의 pending 비트를 직접 세우면 실제
	 * 인터럽트가 도착한 것과 구별되지 않는다. 재전송 기구보다 훨씬 실제에
	 * 가까워 시험의 신뢰도가 높다. */
	if (!irq_set_irqchip_state(irq, IRQCHIP_STATE_PENDING, true))	/* [한국어] 컨트롤러에 "이 인터럽트가 대기 중"이라고 직접 쓴다. 0 이면 성공이다 */
		return 0;	/* [한국어] 하드웨어가 곧 인터럽트를 낼 것이다 */

	/* That failed, try via the resend mechanism */
	/* [한국어] (위 영어 주석) 실패했으면 재전송 기구로 시도한다. */
	scoped_irqdesc_get_and_buslock(irq, 0) {	/* [한국어] 서술자를 찾아 락과 버스 락을 함께 잡는다. 블록을 벗어나면 자동으로 풀린다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] guard 가 들고 있는 포인터를 꺼낸다. NULL 일 수 있다 */

		/*
		 * Only try to inject when the interrupt is:
		 *  - not NMI type
		 *  - activated
		 */
		/* [한국어] (위 영어 주석) 두 조건을 만족할 때만 주입한다.
		 *
		 * NMI 를 제외하는 이유: NMI 는 마스크할 수 없고 보통의 흐름 제어
		 * 경로를 타지 않는다. 재전송 기구로 흉내 내면 그 인터럽트가 기대하는
		 * 문맥과 전혀 다른 곳에서 핸들러가 돈다.
		 *
		 * activate 를 요구하는 이유: 하드웨어 자원(벡터 등)이 아직 배정되지
		 * 않은 인터럽트는 발생시켜도 도달할 곳이 없다.
		 *
		 * desc 가 NULL 인 경우를 검사하지 않는 것처럼 보이지만, scoped guard
		 * 의 블록은 desc 가 NULL 이면 실행되지 않는다 — 그래서 err 이 기본값
		 * -EINVAL 인 채로 남는다. */
		if (!irq_is_nmi(desc) && irqd_is_activated(&desc->irq_data))	/* [한국어] NMI 가 아니고 자원이 배정되어 있는가 */
			err = check_irq_resend(desc, true);	/* [한국어] inject 를 참으로 넘겨, IRQS_PENDING 이 없어도 강제로 재전송하게 한다 */
	}
	return err;	/* [한국어] 0 이면 성공. 조건을 만족하지 못했으면 기본값 -EINVAL 이 그대로 남는다 */
}
EXPORT_SYMBOL_GPL(irq_inject_interrupt);	/* [한국어] 시험 모듈이 쓸 수 있게 공개한다. GPL 제한이 붙는 것은 최근에 추가된 API 이기 때문이다 */
#endif
