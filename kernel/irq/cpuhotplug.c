// SPDX-License-Identifier: GPL-2.0
/*
 * Generic cpu hotunplug interrupt migration code copied from the
 * arch/arm implementation
 *
 * Copyright (C) Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/*
 * [한국어 설명] CPU 를 내리고 올릴 때 인터럽트를 옮기는 코드 (cpuhotplug.c)
 *
 * === 파일의 역할 ===
 * CPU 를 오프라인으로 내릴 때 그 CPU 로 향하던 인터럽트를 다른 CPU 로
 * 옮기고, 다시 온라인이 될 때 managed 인터럽트를 원래 자리로 되돌린다.
 *
 * 왜 필요한가: 인터럽트의 목적지가 사라진 CPU 를 가리킨 채 남으면 그
 * 인터럽트는 영원히 전달되지 않는다. 장치 쪽에서는 완료 통지를 기다리는
 * 요청이 끝나지 않는 형태로 나타나고, 시스템이 조용히 멈춘 것처럼 보인다.
 * 그래서 CPU 를 내리는 절차의 마지막 단계에서 반드시 이 정리를 거친다.
 *
 * 두 방향의 처리가 대칭이 아니라는 점이 이 파일의 특징이다. 내릴 때는
 * 모든 인터럽트를 옮겨야 하지만, 올릴 때는 managed 인터럽트만 되돌린다.
 * 보통의 인터럽트는 한 번 옮겨지면 그 자리에 남는다 — 사용자나 irqbalance
 * 가 정한 배치를 커널이 마음대로 되돌릴 수 없기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * CPU 핫플러그 상태 기계의 양 끝에 붙어 있다:
 *
 *   CPU 오프라인:
 *     cpu_down() → ... → CPUHP_TEARDOWN_CPU 단계
 *       ↓ 그 CPU 에서 실행
 *     irq_migrate_all_off_this_cpu()   ← **이 파일**
 *       ↓ 인터럽트마다
 *     migrate_one_irq() → irq_do_set_affinity()
 *
 *   CPU 온라인:
 *     cpu_up() → ... → CPUHP_AP_IRQ_AFFINITY_ONLINE 단계
 *       ↓
 *     irq_affinity_online_cpu()        ← **이 파일**
 *       ↓ managed 인터럽트마다
 *     irq_restore_affinity_of_irq() → irq_startup_managed()
 *
 * 실행 컨텍스트: 오프라인 쪽은 그 CPU 위에서, 인터럽트가 꺼진 상태로 돈다.
 * 온라인 쪽은 새로 올라온 CPU 위에서 프로세스 문맥으로 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — irq_do_set_affinity, irq_fixup_move_pending,
 *     irq_force_complete_move, irq_startup_managed 등.
 *   migration.c — 미뤄 둔 이동의 정리를 그쪽에 맡긴다.
 *   sched/isolation.h — housekeeping CPU 개념. 아래 hk_should_isolate 참고.
 *
 * 이 파일에 의존하는 곳: kernel/cpu.c 의 핫플러그 상태 기계.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_needs_fixup()             — 이 인터럽트가 내려가는 CPU 를 가리키는지 판정.
 * migrate_one_irq()             — 인터럽트 하나를 옮긴다. 이 파일의 본체.
 * irq_migrate_all_off_this_cpu()— 모든 인터럽트에 대해 위를 부른다.
 * hk_should_isolate()           — 새 CPU 가 housekeeping 이라 옮길 가치가 있는지.
 * irq_restore_affinity_of_irq() — managed 인터럽트를 새 CPU 로 되돌린다.
 * irq_affinity_online_cpu()     — 모든 인터럽트에 대해 위를 부른다.
 *
 * managed 인터럽트가 이 파일에서 특별 취급되는 이유: 그 인터럽트는 특정
 * CPU 집합에 묶인 장치 큐의 것이라, 옮기는 것이 의미가 없다. 대신 대상
 * CPU 가 모두 사라지면 끄고(managed shutdown), 하나라도 돌아오면 되살린다.
 */
#include <linux/interrupt.h>	/* [한국어] 인터럽트 공개 API */
#include <linux/ratelimit.h>	/* [한국어] pr_warn_ratelimited — 옮기기 실패가 폭주할 때 로그를 막는다 */
#include <linux/irq.h>	/* [한국어] struct irq_data, irqd_* 접근자 */
#include <linux/sched/isolation.h>	/* [한국어] housekeeping_cpumask — 실시간 격리 구성에서 인터럽트를 받아도 되는 CPU 집합 */

#include "internals.h"	/* [한국어] irq_do_set_affinity, irq_startup_managed 등 코어 내부 함수 */

/* For !GENERIC_IRQ_EFFECTIVE_AFF_MASK this looks at general affinity mask */
/*
 * [한국어] (위 영어 주석에 이어)
 * irq_needs_fixup - 이 인터럽트가 지금 내려가는 CPU 를 가리키고 있는지 판정한다
 *
 * @d:      대상 irq_data.
 * @return: 이 CPU 를 가리키고 있어 옮겨야 하면 참.
 *
 * 두 종류의 친화도 마스크가 있다는 것이 이 함수를 이해하는 열쇠다.
 *
 *   일반 친화도(affinity)      — 사용자나 커널이 "여기로 보내라"고 요청한 집합.
 *   유효 친화도(effective)     — 하드웨어가 실제로 고른 CPU. 대개 하나뿐이다.
 *
 * 예를 들어 사용자가 CPU 0~3 을 요청해도, 컨트롤러가 하나만 고를 수 있으면
 * 유효 친화도는 CPU 1 하나가 된다. 옮겨야 하는지 판정할 때는 유효 친화도를
 * 봐야 정확하다 — 요청 집합에 이 CPU 가 들어 있어도 실제로는 다른 CPU 로
 * 가고 있다면 옮길 이유가 없다.
 *
 * 위 영어 주석이 말하듯, 유효 친화도 기능이 없는 빌드에서는 두 마스크가
 * 같은 것을 가리켜 자동으로 일반 친화도를 보게 된다.
 *
 * #ifdef 안의 두 블록은 각각 다른 문제를 다룬다. 첫째는 기능을 켰지만
 * 구현하지 않은 chip 에 대한 우회이고, 둘째는 이전에 정리를 놓친 흔적을
 * 잡아내는 검사다. 자세한 것은 각 주석에 있다.
 *
 * 실행 컨텍스트: 내려가는 CPU 위에서, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_migrate_all_off_this_cpu() → migrate_one_irq() → [이 함수]
 */
static inline bool irq_needs_fixup(struct irq_data *d)
{
	const struct cpumask *m = irq_data_get_effective_affinity_mask(d);	/* [한국어] 하드웨어가 실제로 고른 CPU 집합. 기능이 없는 빌드에서는 일반 친화도와 같다 */
	unsigned int cpu = smp_processor_id();	/* [한국어] 지금 내려가는 CPU. 이 함수는 그 CPU 위에서 돈다 */

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 따로 추적하는 빌드 */
	/*
	 * The cpumask_empty() check is a workaround for interrupt chips,
	 * which do not implement effective affinity, but the architecture has
	 * enabled the config switch. Use the general affinity mask instead.
	 */
	/* [한국어] (위 영어 주석에 이어) 유효 친화도가 비어 있으면 일반 친화도를 쓴다.
	 *
	 * 왜 비어 있을 수 있는가: CONFIG 는 아키텍처 단위로 켜지는데, 그
	 * 아키텍처의 모든 chip 드라이버가 유효 친화도를 채워 주는 것은 아니다.
	 * 채우지 않은 chip 의 인터럽트는 이 마스크가 빈 채로 남는다.
	 *
	 * 빈 마스크로 아래 판정을 하면 언제나 거짓이 나와, 그 인터럽트는
	 * 영원히 옮겨지지 않는다. 그래서 일반 친화도로 되돌아간다. */
	if (cpumask_empty(m))	/* [한국어] chip 이 유효 친화도를 채우지 않았는가 */
		m = irq_data_get_affinity_mask(d);	/* [한국어] 요청 집합으로 대신한다 — 부정확하지만 아예 옮기지 않는 것보다 낫다 */

	/*
	 * Sanity check. If the mask is not empty when excluding the outgoing
	 * CPU then it must contain at least one online CPU. The outgoing CPU
	 * has been removed from the online mask already.
	 */
	/* [한국어] (위 영어 주석에 이어) 정합성 검사.
	 *
	 * 무엇을 확인하는가: 내려가는 CPU 를 뺀 마스크가 비어 있지 않은데,
	 * 그 안에 온라인 CPU 가 하나도 없는 상태다. 즉 이 인터럽트가 이미
	 * 오프라인인 CPU 들만 가리키고 있다.
	 *
	 * 왜 그런 일이 생기는가: 이전에 어떤 CPU 를 내릴 때 이 인터럽트의
	 * 정리를 놓친 것이다. 그 CPU 는 이미 없는데 이 인터럽트는 여전히
	 * 그쪽을 가리킨다.
	 *
	 * 이 시점에 cpu_online_mask 에서 지금 내려가는 CPU 가 이미 빠져 있다는
	 * 점이 중요하다(원 주석의 마지막 문장). 그래서 "온라인 CPU 가 없다"는
	 * 판정이 곧 "살아 있는 목적지가 없다"가 된다.
	 *
	 * 참을 돌려주어 강제로 옮기게 한다 — 그러지 않으면 이 인터럽트는
	 * 영원히 갈 곳 없는 상태로 남는다. */
	if (cpumask_any_but(m, cpu) < nr_cpu_ids &&	/* [한국어] 내려가는 CPU 말고도 다른 CPU 가 마스크에 있는가 */
	    !cpumask_intersects(m, cpu_online_mask)) {	/* [한국어] 그런데 그중 살아 있는 것이 하나도 없는가 */
		/*
		 * If this happens then there was a missed IRQ fixup at some
		 * point. Warn about it and enforce fixup.
		 */
		/* [한국어] (위 영어 주석) 이전에 정리를 놓친 것이다. 경고하고 강제로 고친다. */
		pr_warn("Eff. affinity %*pbl of IRQ %u contains only offline CPUs after offlining CPU %u\n",	/* [한국어] %*pbl 은 cpumask 를 "0-3,7" 같은 사람이 읽는 형태로 찍는다 */
			cpumask_pr_args(m), d->irq, cpu);	/* [한국어] 이 매크로가 %*pbl 이 요구하는 두 인자(비트 수와 포인터)를 함께 넘겨 준다 */
		return true;	/* [한국어] 강제로 옮기게 한다. 그러지 않으면 이 인터럽트는 갈 곳 없이 남는다 */
	}
#endif
	return cpumask_test_cpu(cpu, m);	/* [한국어] 보통의 판정 — 이 인터럽트가 지금 내려가는 CPU 를 가리키고 있는가 */
}

/*
 * [한국어]
 * migrate_one_irq - 인터럽트 하나를 내려가는 CPU 에서 다른 곳으로 옮긴다
 *
 * @desc:   대상 서술자.
 * @return: 요청된 친화도를 지키지 못하고 임의의 온라인 CPU 로 옮겼으면 참
 *          (친화도를 "깼다"는 뜻). 정상적으로 옮겼거나 옮길 필요가 없었으면 거짓.
 *
 * 이 파일의 본체이며, 여러 경우를 차례로 걸러 낸다.
 *
 *   1. chip 이 없거나 친화도 설정을 지원하지 않으면 아무것도 할 수 없다.
 *   2. 미뤄 둔 이동이 있으면 먼저 강제로 완료한다 — CPU 가 사라지기 전에.
 *   3. 옮길 필요가 없는 경우(per-CPU, 시작 안 됨, 이 CPU 가 대상 아님)는 넘어간다.
 *   4. 옮길 목적지를 정한다 — 미뤄 둔 목표가 있으면 그것을, 없으면 현재 친화도를.
 *   5. 목적지에 온라인 CPU 가 없으면: managed 면 끄고, 아니면 친화도를 깬다.
 *   6. 실제로 옮긴다. 벡터가 없어 실패하면 온라인 전체로 다시 시도한다.
 *
 * 반환값이 "친화도를 깼는가" 라는 점에 주의한다. 성공 여부가 아니다.
 * 호출자는 이 값이 참일 때 사용자에게 알린다 — 요청한 배치가 지켜지지
 * 않았음을 드라이버나 관리자가 알아야 하기 때문이다.
 *
 * maskchip 변수가 이 함수의 미묘한 부분이다. 아래 그 선언 주석에 설명이 있다.
 *
 * 실행 컨텍스트: 내려가는 CPU 위에서, 서술자 락을 쥔 상태. 인터럽트가 꺼져 있다.
 *
 * 호출 체인:
 *   irq_migrate_all_off_this_cpu() → [이 함수]
 *     → irq_force_complete_move()/irq_do_set_affinity()
 */
static bool migrate_one_irq(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 최상위 irq_data */
	struct irq_chip *chip = irq_data_get_irq_chip(d);	/* [한국어] 담당 컨트롤러 */
	/* [한국어] 옮기는 동안 chip 을 마스크해야 하는가.
	 *
	 * 두 조건이 모두 맞을 때만 참이다:
	 *   can_move_pcntxt 가 거짓 — 이 하드웨어는 인터럽트가 날아오는 중에
	 *     목적지를 바꾸면 안 된다(IO-APIC 등). 그래서 마스크가 필요하다.
	 *   아직 마스크되어 있지 않음 — 이미 마스크된 것을 또 마스크하면
	 *     아래에서 언마스크할 때 원래 상태를 잃는다.
	 *
	 * migration.c 의 __irq_move_irq() 가 같은 논리를 쓴다. 이미 마스크된
	 * 인터럽트를 함부로 언마스크하면 ONESHOT 스레드 인터럽트에서 폭풍이 난다. */
	bool maskchip = !irq_can_move_pcntxt(d) && !irqd_irq_masked(d);
	const struct cpumask *affinity;	/* [한국어] 옮길 목적지. 아래에서 세 후보 중 하나로 정해진다 */
	bool brokeaff = false;	/* [한국어] 요청된 친화도를 지키지 못했는가. 이 함수의 반환값이다 */
	int err;	/* [한국어] 친화도 설정 결과 */

	/*
	 * IRQ chip might be already torn down, but the irq descriptor is
	 * still in the radix tree. Also if the chip has no affinity setter,
	 * nothing can be done here.
	 */
	/* [한국어] (위 영어 주석에 이어) chip 이 없거나 친화도를 못 바꾸면 할 일이 없다.
	 *
	 * chip 이 NULL 일 수 있는 이유: 드라이버가 이미 정리되어 chip 을 뗐는데
	 * 서술자는 트리에 남아 있는 경우가 있다. 서술자의 수명이 chip 의 수명보다
	 * 길기 때문이다.
	 *
	 * 경고가 아니라 pr_debug 인 이유: 정상적으로 일어날 수 있는 상황이다.
	 * 옮길 수 없는 인터럽트가 있다는 것이 반드시 문제는 아니다. */
	if (!chip || !chip->irq_set_affinity) {	/* [한국어] chip 이 없거나 친화도 설정을 지원하지 않는가 */
		pr_debug("IRQ %u: Unable to migrate away\n", d->irq);	/* [한국어] 진단용으로만 남긴다 — 정상일 수 있는 상황이다 */
		return false;	/* [한국어] 친화도를 깬 것이 아니라 아예 손대지 못한 것이다 */
	}

	/*
	 * Complete an eventually pending irq move cleanup. If this
	 * interrupt was moved in hard irq context, then the vectors need
	 * to be cleaned up. It can't wait until this interrupt actually
	 * happens and this CPU was involved.
	 */
	/* [한국어] (위 영어 주석에 이어) 미뤄 둔 이동의 하드웨어 정리를 먼저 끝낸다.
	 *
	 * 왜 지금이어야 하는가: x86 의 벡터 관리는 이전 이동에서 반납한 벡터를
	 * "다음 인터럽트가 새 CPU 에 도착했다"는 확인 뒤에 정리한다. 그런데
	 * 그 확인에 이 CPU 가 관여해야 한다면, CPU 가 사라진 뒤에는 영원히
	 * 확인할 수 없다. 원 주석의 "이 인터럽트가 실제로 발생하기를 기다릴 수
	 * 없다"가 그 뜻이다.
	 *
	 * 그래서 사라지기 전에 강제로 마무리한다. */
	irq_force_complete_move(desc);	/* [한국어] migration.c 에 있다. 계층을 훑어 벡터 정리를 아는 단계에 맡긴다 */

	/*
	 * No move required, if:
	 * - Interrupt is per cpu
	 * - Interrupt is not started
	 * - Affinity mask does not include this CPU.
	 *
	 * Note: Do not check desc->action as this might be a chained
	 * interrupt.
	 */
	/* [한국어] (위 영어 주석에 이어) 옮길 필요가 없는 세 경우.
	 *
	 *   per-CPU — CPU 마다 별개의 선이라 옮긴다는 개념이 없다. 그 CPU 가
	 *     사라지면 그 선도 함께 사라진다.
	 *   시작되지 않음 — 아직 켜지지 않은 인터럽트는 어차피 오지 않는다.
	 *   이 CPU 가 대상이 아님 — 다른 CPU 로 가고 있으므로 손댈 이유가 없다.
	 *
	 * 원 주석의 Note 가 중요하다: desc->action 이 비어 있다고 건너뛰면 안 된다.
	 * 연쇄(chained) 인터럽트는 드라이버 핸들러 대신 표식용 더미 action 을
	 * 갖거나 아예 갖지 않는데, 그것도 실제로 CPU 를 가리키고 있어 옮겨야 한다.
	 * action 으로 판정하면 그런 인터럽트를 놓친다. */
	if (irqd_is_per_cpu(d) || !irqd_is_started(d) || !irq_needs_fixup(d)) {	/* [한국어] 세 경우 중 하나라도 해당하면 */
		/*
		 * If an irq move is pending, abort it if the dying CPU is
		 * the sole target.
		 */
		/* [한국어] (위 영어 주석) 옮기지는 않지만, 미뤄 둔 이동의 목표가
		 * 이 CPU 뿐이라면 그 이동은 포기시켜야 한다. false 를 넘겨
		 * "목표가 남아 있으면 그대로 두라"고 요청한다. */
		irq_fixup_move_pending(desc, false);	/* [한국어] migration.c. 목표에 온라인 CPU 가 없으면 대기 비트를 지운다 */
		return false;	/* [한국어] 친화도를 깨지 않았다 */
	}

	/*
	 * If there is a setaffinity pending, then try to reuse the pending
	 * mask, so the last change of the affinity does not get lost. If
	 * there is no move pending or the pending mask does not contain
	 * any online CPU, use the current affinity mask.
	 */
	/* [한국어] (위 영어 주석에 이어) 목적지를 정한다.
	 *
	 * 미뤄 둔 이동이 있으면 그 목표를 쓴다 — 사용자가 가장 최근에 요청한
	 * 배치이므로, 그것을 버리고 현재 친화도로 옮기면 그 요청이 유실된다.
	 *
	 * true 를 넘기는 것에 주의한다. 위에서는 false 였는데 여기서는 대기
	 * 비트를 지워 달라고 요청한다 — 이제 우리가 직접 옮길 것이라, 나중에
	 * 또 옮기려 하면 안 되기 때문이다. */
	if (irq_fixup_move_pending(desc, true))	/* [한국어] 쓸 만한 미뤄 둔 목표가 있는가. 있으면 대기 비트를 지우고 참을 준다 */
		affinity = irq_desc_get_pending_mask(desc);	/* [한국어] 사용자가 가장 최근에 요청한 배치 */
	else
		affinity = irq_data_get_affinity_mask(d);	/* [한국어] 현재 친화도 */

	/* Mask the chip for interrupts which cannot move in process context */
	/* [한국어] (위 영어 주석) 옮기는 동안 마스크가 필요한 하드웨어면 마스크한다.
	 * 위 maskchip 선언 주석에 두 조건의 이유가 있다. */
	if (maskchip && chip->irq_mask)	/* [한국어] 마스크가 필요하고, chip 이 마스크를 지원하는가 */
		chip->irq_mask(d);	/* [한국어] 인터럽트가 날아오는 중에 목적지를 바꾸는 것을 막는다 */

	if (!cpumask_intersects(affinity, cpu_online_mask)) {	/* [한국어] 목적지에 살아 있는 CPU 가 하나도 없는가 */
		/*
		 * If the interrupt is managed, then shut it down and leave
		 * the affinity untouched.
		 */
		/* [한국어] (위 영어 주석에 이어) managed 인터럽트는 끄되 친화도는 건드리지 않는다.
		 *
		 * 왜 다른가: managed 인터럽트는 특정 CPU 집합에 묶인 장치 큐의
		 * 것이다. 그 CPU 들이 모두 사라졌다면 그 큐를 쓸 일도 없으므로,
		 * 억지로 다른 CPU 로 옮기는 것보다 끄는 편이 맞다.
		 *
		 * 친화도를 그대로 두는 것이 핵심이다. 나중에 그 CPU 중 하나가
		 * 돌아오면 아래 irq_restore_affinity_of_irq() 가 이 친화도를 보고
		 * 원래 자리로 되살린다. 친화도를 고쳐 버리면 그 복원이 불가능해진다.
		 *
		 * 여기서 마스크를 되돌리지 않고 곧바로 반환하는 것에 주의 —
		 * shutdown 이 어차피 인터럽트를 끄므로 마스크 상태가 의미 없어진다. */
		if (irqd_affinity_is_managed(d)) {	/* [한국어] 커널이 친화도를 관리하는 인터럽트인가 */
			irqd_set_managed_shutdown(d);	/* [한국어] "고장이 아니라 CPU 가 없어서 껐다"고 표시한다. 복원의 근거가 된다 */
			irq_shutdown_and_deactivate(desc);	/* [한국어] 끄고 하드웨어 자원까지 반납한다 */
			return false;	/* [한국어] 친화도를 깬 것이 아니라 보존한 것이다 */
		}
		affinity = cpu_online_mask;	/* [한국어] managed 가 아니면 아무 온라인 CPU 로나 옮긴다 — 인터럽트를 잃는 것보다 낫다 */
		brokeaff = true;	/* [한국어] 요청된 배치를 지키지 못했음을 기록한다. 호출자가 이것을 사용자에게 알린다 */
	}
	/*
	 * Do not set the force argument of irq_do_set_affinity() as this
	 * disables the masking of offline CPUs from the supplied affinity
	 * mask and therefore might keep/reassign the irq to the outgoing
	 * CPU.
	 */
	/* [한국어] (위 영어 주석에 이어) force 를 참으로 주면 안 되는 이유.
	 *
	 * force 는 "주어진 마스크를 그대로 쓰라"는 뜻이라, 그 안의 오프라인
	 * CPU 를 걸러 내지 않는다. 지금 내려가는 CPU 가 마스크에 들어 있으면
	 * 그리로 다시 배정될 수 있다 — 옮기려던 목적과 정반대다.
	 *
	 * 거짓으로 주면 하부가 온라인 CPU 만 고르므로 안전하다. */
	err = irq_do_set_affinity(d, affinity, false);	/* [한국어] 실제로 옮긴다. force 는 반드시 거짓이어야 한다 */

	/*
	 * If there are online CPUs in the affinity mask, but they have no
	 * vectors left to make the migration work, try to break the
	 * affinity by migrating to any online CPU.
	 */
	/* [한국어] (위 영어 주석에 이어) 벡터 고갈로 실패한 경우의 재시도.
	 *
	 * -ENOSPC 는 "목적지 CPU 들에 남은 인터럽트 벡터가 없다"는 뜻이다.
	 * x86 처럼 CPU 마다 벡터가 유한한 아키텍처에서 일어난다.
	 *
	 * 세 조건이 모두 맞을 때만 재시도한다:
	 *   -ENOSPC 일 것 — 다른 오류는 재시도해도 같다.
	 *   managed 가 아닐 것 — managed 는 친화도를 깨면 안 된다. 위에서
	 *     이미 shutdown 으로 처리했으므로 여기 오지 않아야 하지만 방어한다.
	 *   이미 온라인 전체를 시도한 것이 아닐 것 — 그랬다면 더 넓힐 곳이 없다. */
	if (err == -ENOSPC && !irqd_affinity_is_managed(d) && affinity != cpu_online_mask) {	/* [한국어] 벡터가 없어 실패했고, 더 넓혀 볼 여지가 있는가 */
		pr_debug("IRQ%u: set affinity failed for %*pbl, re-try with online CPUs\n",	/* [한국어] 진단용. 정상적으로 일어날 수 있어 경고가 아니다 */
			 d->irq, cpumask_pr_args(affinity));	/* [한국어] 실패한 목적지 마스크를 사람이 읽는 형태로 */

		affinity = cpu_online_mask;	/* [한국어] 온라인 CPU 전체로 넓힌다 */
		brokeaff = true;	/* [한국어] 요청된 배치를 깼음을 기록한다 */

		err = irq_do_set_affinity(d, affinity, false);	/* [한국어] 다시 시도. 역시 force 는 거짓이다 */
	}

	if (err) {	/* [한국어] 재시도까지 실패했는가 */
		pr_warn_ratelimited("IRQ%u: set affinity failed(%d).\n",	/* [한국어] 이번에는 경고다 — 인터럽트가 갈 곳을 잃었을 수 있다. ratelimit 은 CPU 를 내릴 때 수백 개가 한꺼번에 실패하는 경우를 대비한 것이다 */
				    d->irq, err);	/* [한국어] 어느 인터럽트가 왜 실패했는지 */
		brokeaff = false;	/* [한국어] 옮기지 못했으므로 "깼다"고 할 수도 없다. 호출자가 사용자에게 잘못된 알림을 보내지 않게 한다 */
	}

	if (maskchip && chip->irq_unmask)	/* [한국어] 위에서 우리가 마스크했다면 */
		chip->irq_unmask(d);	/* [한국어] 되돌린다. 원래 마스크되어 있던 것은 maskchip 이 거짓이라 건드리지 않는다 */

	return brokeaff;	/* [한국어] 요청된 친화도를 지키지 못했으면 참. 성공 여부가 아니라는 점에 주의 */
}

/**
 * irq_migrate_all_off_this_cpu - Migrate irqs away from offline cpu
 *
 * The current CPU has been marked offline.  Migrate IRQs off this CPU.
 * If the affinity settings do not allow other CPUs, force them onto any
 * available CPU.
 *
 * Note: we must iterate over all IRQs, whether they have an attached
 * action structure or not, as we need to get chained interrupts too.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_migrate_all_off_this_cpu - 이 CPU 의 모든 인터럽트를 다른 곳으로 옮긴다
 *
 * 인자도 반환값도 없다. 현재 CPU 를 대상으로 삼는다.
 *
 * CPU 오프라인 절차의 마지막 단계에서 불린다. 이 시점에 그 CPU 는 이미
 * cpu_online_mask 에서 빠져 있어, 위 함수들이 "온라인 CPU" 를 물을 때
 * 자연히 자기 자신이 제외된다.
 *
 * kernel-doc 의 Note 가 중요하다: action 이 붙어 있든 아니든 모든 인터럽트를
 * 훑어야 한다. 연쇄(chained) 인터럽트는 드라이버 핸들러가 없지만 실제로
 * CPU 를 가리키고 있어, action 으로 걸러 내면 그것들이 남는다.
 *
 * 락 구조에 주의: 서술자 락은 scoped_guard 로 블록 안에서만 잡는다. 그
 * 블록 밖에서 pr_debug 를 부르는 이유는 로그 출력이 다른 락을 잡을 수 있어,
 * 서술자 락을 쥔 채로 부르면 락 순서가 얽히기 때문이다.
 *
 * 실행 컨텍스트: 내려가는 CPU 위에서, 인터럽트가 꺼진 상태.
 *
 * 호출 체인:
 *   cpu_down() → CPU 핫플러그 상태 기계 → [이 함수] → migrate_one_irq()
 */
void irq_migrate_all_off_this_cpu(void)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned int irq;	/* [한국어] 그 인터럽트 번호 */

	for_each_active_irq(irq) {	/* [한국어] 실제로 쓰이는 모든 인터럽트. kernel-doc 의 Note 대로 action 유무를 가리지 않는다 */
		bool affinity_broken;	/* [한국어] 이 인터럽트의 요청된 배치를 지키지 못했는가 */

		desc = irq_to_desc(irq);	/* [한국어] 번호로 서술자를 찾는다 */
		scoped_guard(raw_spinlock, &desc->lock) {	/* [한국어] 서술자 락. 블록을 벗어나면 자동으로 풀린다 */
			affinity_broken = migrate_one_irq(desc);	/* [한국어] 실제로 옮긴다. 반환값은 "친화도를 깼는가"이지 성공 여부가 아니다 */
			if (affinity_broken && desc->affinity_notify)	/* [한국어] 배치가 깨졌고, 그것을 알려 달라고 등록한 드라이버가 있으면 */
				irq_affinity_schedule_notify_work(desc);	/* [한국어] 워크큐로 알린다. 알림 콜백이 잠들 수 있어 여기서 직접 부를 수 없다 */
		}
		if (affinity_broken) {	/* [한국어] 락을 놓은 뒤에 로그를 찍는다 */
			pr_debug_ratelimited("IRQ %u: no longer affine to CPU%u\n",	/* [한국어] 락 밖에서 부르는 이유: printk 가 다른 락을 잡을 수 있어 락 순서가 얽힌다 */
					    irq, smp_processor_id());	/* [한국어] 어느 인터럽트가 어느 CPU 에서 떨어져 나갔는지 */
		}
	}
}

/*
 * [한국어]
 * hk_should_isolate - 새로 올라온 CPU 가 housekeeping 이라 옮길 가치가 있는지 판정한다
 *
 * @data:   대상 irq_data.
 * @cpu:    새로 온라인이 된 CPU.
 * @return: 이 인터럽트를 그 CPU 로 옮기는 것이 격리 정책에 부합하면 참.
 *
 * housekeeping 이란: 실시간 구성에서 일부 CPU 를 "격리" 해 두고 인터럽트나
 * 커널 작업이 그리로 가지 않게 한다. 격리되지 않은 나머지 CPU 들이
 * housekeeping CPU 이며, 잡일을 그쪽으로 몰아 격리된 CPU 의 지연 시간을 지킨다.
 *
 * 이 함수가 푸는 문제: managed 인터럽트의 친화도 집합에 격리 CPU 와
 * housekeeping CPU 가 섞여 있을 수 있다. 지금 그 인터럽트가 격리 CPU 로
 * 가고 있는데 housekeeping CPU 하나가 새로 올라왔다면, 옮기는 편이 낫다.
 *
 * 세 단계로 판정한다:
 *   격리 기능이 꺼져 있으면 거짓 — 옮길 이유가 없다.
 *   현재 목적지가 전부 housekeeping 이면 거짓 — 이미 좋은 자리에 있다.
 *   새 CPU 가 housekeeping 이면 참 — 옮길 가치가 있다.
 *
 * 실행 컨텍스트: CPU 온라인 경로, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_affinity_online_cpu() → irq_restore_affinity_of_irq() → [이 함수]
 */
static bool hk_should_isolate(struct irq_data *data, unsigned int cpu)
{
	const struct cpumask *hk_mask;	/* [한국어] 인터럽트를 받아도 되는 CPU 집합 */

	if (!housekeeping_enabled(HK_TYPE_MANAGED_IRQ))	/* [한국어] managed 인터럽트에 대한 격리 정책이 켜져 있는가 */
		return false;	/* [한국어] 꺼져 있으면 모든 CPU 가 동등하므로 옮길 이유가 없다 */

	hk_mask = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ);	/* [한국어] 격리되지 않은 CPU 들 */
	if (cpumask_subset(irq_data_get_effective_affinity_mask(data), hk_mask))	/* [한국어] 지금 목적지가 이미 전부 housekeeping 안에 있는가 */
		return false;	/* [한국어] 이미 좋은 자리에 있으므로 옮기지 않는다 */

	return cpumask_test_cpu(cpu, hk_mask);	/* [한국어] 새 CPU 가 housekeeping 이면 옮길 가치가 있다 */
}

/*
 * [한국어]
 * irq_restore_affinity_of_irq - managed 인터럽트를 새로 올라온 CPU 로 되돌린다
 *
 * @desc: 대상 서술자.
 * @cpu:  새로 온라인이 된 CPU.
 *
 * CPU 를 올릴 때의 처리다. 내릴 때와 달리 모든 인터럽트를 손대지 않고,
 * managed 인터럽트만 되돌린다.
 *
 * 왜 managed 만인가: 보통의 인터럽트는 사용자나 irqbalance 가 배치를 정한다.
 * CPU 가 하나 늘었다고 커널이 마음대로 옮기면 그 결정을 뒤엎는 것이 된다.
 * managed 인터럽트는 커널 자신이 배치를 관리하므로 되돌릴 권한이 있다.
 *
 * 네 가지 조건을 모두 만족해야 손댄다. 아래 그 조건문 주석에 각각의 이유가 있다.
 *
 * 두 단계로 처리한다:
 *   1. managed shutdown 상태였으면 되살린다 — 대상 CPU 가 전부 사라져
 *      꺼 두었던 인터럽트다.
 *   2. 친화도를 다시 적용한다 — 다만 옮길 가치가 있을 때만.
 *
 * 실행 컨텍스트: 새로 올라온 CPU 위에서, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_affinity_online_cpu() → [이 함수]
 *     → irq_startup_managed()/irq_set_affinity_locked()
 */
static void irq_restore_affinity_of_irq(struct irq_desc *desc, unsigned int cpu)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);	/* [한국어] 최상위 irq_data */
	const struct cpumask *affinity = irq_data_get_affinity_mask(data);	/* [한국어] 원래 요청된 친화도. managed shutdown 때 보존해 둔 것이 여기 있다 */

	/* [한국어] 네 조건을 모두 만족해야 손댄다:
	 *   managed 일 것        — 커널이 배치를 관리하는 인터럽트만 되돌린다.
	 *   action 이 있을 것    — 쓰이지 않는 인터럽트를 켤 이유가 없다.
	 *   chip 이 있을 것      — 없으면 아무것도 설정할 수 없다.
	 *   이 CPU 가 대상일 것  — 친화도에 없는 CPU 가 올라온 것은 이 인터럽트와 무관하다.
	 *
	 * 위 내릴 때의 판정이 desc->action 을 보지 말라고 했던 것과 대조된다.
	 * 그쪽은 연쇄 인터럽트도 옮겨야 했지만, 이쪽은 managed 인터럽트만
	 * 다루고 연쇄 인터럽트는 managed 가 될 수 없어 문제가 없다. */
	if (!irqd_affinity_is_managed(data) || !desc->action ||	/* [한국어] managed 가 아니거나 쓰이지 않는 인터럽트면 */
	    !irq_data_get_irq_chip(data) || !cpumask_test_cpu(cpu, affinity))	/* [한국어] chip 이 없거나 이 CPU 가 대상이 아니면 */
		return;	/* [한국어] 손대지 않는다 */

	if (irqd_is_managed_and_shutdown(data))	/* [한국어] 대상 CPU 가 전부 사라져 꺼 두었던 인터럽트인가 */
		irq_startup_managed(desc);	/* [한국어] 되살린다. 이 CPU 가 돌아왔으므로 이제 보낼 곳이 있다 */

	/*
	 * If the interrupt can only be directed to a single target
	 * CPU then it is already assigned to a CPU in the affinity
	 * mask. No point in trying to move it around unless the
	 * isolation mechanism requests to move it to an upcoming
	 * housekeeping CPU.
	 */
	/* [한국어] (위 영어 주석에 이어) 옮길 가치가 있을 때만 친화도를 다시 적용한다.
	 *
	 * single target 이란: 이 인터럽트를 한 CPU 에만 보낼 수 있는 하드웨어다.
	 * 그런 인터럽트는 이미 친화도 집합 안의 어느 CPU 에 배정되어 있고,
	 * 그 배정이 유효하다면 옮겨 봐야 같은 곳에 다시 놓일 뿐이다.
	 *
	 * 예외가 격리다. 지금 격리된 CPU 로 가고 있는데 housekeeping CPU 가
	 * 새로 올라왔다면, 같은 친화도 안에서도 더 나은 자리로 옮길 수 있다.
	 * 그 판정이 위 hk_should_isolate() 다.
	 *
	 * single target 이 아니면(여러 CPU 로 보낼 수 있으면) 조건 없이 다시
	 * 적용한다 — 새 CPU 가 목적지 집합에 더해지는 것이 이득이기 때문이다. */
	if (!irqd_is_single_target(data) || hk_should_isolate(data, cpu))	/* [한국어] 여러 CPU 로 보낼 수 있거나, 격리 정책상 옮길 가치가 있는가 */
		irq_set_affinity_locked(data, affinity, false);	/* [한국어] 원래 친화도를 다시 적용한다. locked 판이라 서술자 락을 이미 쥔 상태에서 부를 수 있다 */
}

/**
 * irq_affinity_online_cpu - Restore affinity for managed interrupts
 * @cpu:	Upcoming CPU for which interrupts should be restored
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_affinity_online_cpu - 새로 올라온 CPU 를 위해 managed 인터럽트를 되돌린다
 *
 * @cpu:    새로 온라인이 된 CPU.
 * @return: 항상 0. 핫플러그 상태 기계가 오류 코드를 요구해 형식을 맞춘 것이며,
 *          실제로 실패할 수 있는 단계가 없다.
 *
 * 위 irq_migrate_all_off_this_cpu() 의 대칭이지만 두 가지가 다르다.
 *
 *   irq_lock_sparse() 를 잡는다 — 인터럽트 번호 공간 자체를 지키는 락이다.
 *     이 경로는 프로세스 문맥이라 그 사이 다른 CPU 가 새 인터럽트를 만들거나
 *     없앨 수 있어, for_each_active_irq 순회 중에 목록이 바뀌지 않게 막는다.
 *     내릴 때는 인터럽트가 꺼진 상태라 그 위험이 없어 이 락이 없다.
 *
 *   서술자 락을 raw_spinlock_irq 로 잡는다 — 내릴 때는 이미 인터럽트가
 *     꺼져 있어 보통의 raw_spinlock 으로 충분했지만, 이쪽은 직접 꺼야 한다.
 *
 * 실행 컨텍스트: 새로 올라온 CPU 위에서, 프로세스 문맥.
 *
 * 호출 체인:
 *   cpu_up() → CPU 핫플러그 상태 기계(CPUHP_AP_IRQ_AFFINITY_ONLINE)
 *     → [이 함수] → irq_restore_affinity_of_irq()
 */
int irq_affinity_online_cpu(unsigned int cpu)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned int irq;	/* [한국어] 그 인터럽트 번호 */

	irq_lock_sparse();	/* [한국어] 인터럽트 번호 공간을 잠근다. 순회 중에 서술자가 생기거나 사라지지 않게 한다 */
	for_each_active_irq(irq) {	/* [한국어] 실제로 쓰이는 모든 인터럽트 */
		desc = irq_to_desc(irq);	/* [한국어] 번호로 서술자를 찾는다 */
		scoped_guard(raw_spinlock_irq, &desc->lock)	/* [한국어] 서술자 락과 함께 인터럽트도 끈다. 내릴 때와 달리 이 경로는 프로세스 문맥이라 직접 꺼야 한다 */
			irq_restore_affinity_of_irq(desc, cpu);	/* [한국어] managed 인터럽트면 되살리고 친화도를 다시 적용한다 */
	}
	irq_unlock_sparse();	/* [한국어] 번호 공간 잠금을 푼다 */

	return 0;	/* [한국어] 핫플러그 상태 기계가 int 를 요구해 형식만 맞춘다. 실패할 수 있는 단계가 없다 */
}
