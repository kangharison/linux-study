// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 미뤄 둔 인터럽트 친화도 변경을 안전한 순간에 완료하는 코드 (migration.c)
 *
 * === 파일의 역할 ===
 * 인터럽트의 목적지 CPU 를 바꾸는 일을, 바꿔도 안전한 순간까지 미뤘다가
 * 실제로 수행한다. 다섯 개의 함수가 모두 그 한 가지 일의 조각이다.
 *
 * 무엇이 문제인가: x86 의 IO-APIC 같은 컨트롤러는 인터럽트가 이미 CPU 를
 * 향해 날아가는 중에 목적지 레지스터를 고치면, 그 인터럽트가 유실되거나
 * 두 CPU 에 중복 전달된다. 그래서 아무 때나 친화도를 바꿀 수 없다.
 *
 * 어떻게 푸는가: 친화도 변경 요청이 오면 목표 CPU 마스크를 desc->pending_mask
 * 에 적어 두고 IRQD_SETAFFINITY_PENDING 을 세운다. 실제 변경은 다음 인터럽트를
 * 처리하고 ack 를 보낸 직후 — 곧 "지금은 확실히 날아오는 인터럽트가 없다"고
 * 말할 수 있는 유일한 순간 — 에 수행한다.
 *
 * 이 파일 전체가 CONFIG_GENERIC_PENDING_IRQ 를 켠 커널에서만 컴파일된다.
 * 그 기능이 없는 아키텍처(대부분의 ARM)는 언제든 안전하게 옮길 수 있어
 * internals.h 의 빈 구현이 대신 쓰인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 친화도 변경의 뒤쪽 절반을 맡는다:
 *
 *   사용자가 /proc/irq/N/smp_affinity 에 쓰기, 또는 커널의 자동 조정
 *     ↓
 *   irq_set_affinity() → irq_do_set_affinity()      (manage.c)
 *     ↓ irq_can_move_pcntxt() 가 거짓이면
 *   irqd_set_move_pending() + irq_copy_pending()    — 목표만 적어 두고 반환
 *     ↓ ... 시간이 흐르고, 그 인터럽트가 한 번 발생 ...
 *   흐름 제어 핸들러 (handle_edge_irq 등)            (chip.c)
 *     ↓ ack 를 보낸 직후
 *   irq_move_irq() → __irq_move_irq()               ← **이 파일**
 *     ↓ 마스크 → 실제 변경 → 언마스크
 *   irq_move_masked_irq() → irq_do_set_affinity()   ← **이 파일**
 *
 * 별개의 진입점이 하나 더 있다: CPU 가 오프라인이 될 때, 그 CPU 를 목표로
 * 미뤄 둔 이동을 정리해야 한다. irq_fixup_move_pending() 과
 * irq_force_complete_move() 가 그 경로다.
 *
 * 실행 컨텍스트: 대부분 인터럽트 문맥이며 서술자 락을 쥔 상태다. CPU
 * 핫플러그 경로의 둘만 프로세스 문맥에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h 의 irqd_set/clr_move_pending, irq_can_move_pcntxt,
 *     irqd_get_parent_data, irq_do_set_affinity.
 *   chip 드라이버의 irq_set_affinity 와 irq_force_complete_move 콜백.
 *
 * 이 파일에 의존하는 곳:
 *   chip.c — 흐름 제어 핸들러가 ack 직후 irq_move_irq() 를 부른다.
 *   cpuhotplug.c — CPU 를 내릴 때 두 정리 함수를 부른다.
 *
 * 공유 상태: desc->pending_mask 가 두 경로 사이의 유일한 통로다. 쓰는
 * 쪽(manage.c)과 읽는 쪽(이 파일)이 서술자 락으로 직렬화된다.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_fixup_move_pending()   — 죽는 CPU 를 목표 마스크에서 걸러 낸다.
 * irq_force_complete_move()  — 도메인 계층을 훑어 강제 완료 콜백을 찾아 부른다.
 * irq_move_masked_irq()      — 이미 마스크된 상태에서 실제 이동을 수행한다.
 * __irq_move_irq()           — 마스크·이동·언마스크를 감싼 판.
 * irq_can_move_in_process_context() — 계층 최상위 기준으로 즉시 이동 가능 여부를 답한다.
 *
 * 아래 세 함수가 반복해서 쓰는 관용구가 하나 있다:
 *   irq_desc_get_irq_data(irq_data_to_desc(data))
 * 이것은 "계층 어디에 있든 최상위 irq_data 를 얻는다"는 뜻이다. 자세한
 * 이유는 __irq_move_irq() 의 주석에 있다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_chip, irq_data 와 irqd_* 공개 접근자 */
#include <linux/interrupt.h>	/* [한국어] 인터럽트 공개 API */

#include "internals.h"	/* [한국어] irqd_clr_move_pending, irq_do_set_affinity 등 코어 내부 함수 */

/**
 * irq_fixup_move_pending - Cleanup irq move pending from a dying CPU
 * @desc:		Interrupt descriptor to clean up
 * @force_clear:	If set clear the move pending bit unconditionally.
 *			If not set, clear it only when the dying CPU is the
 *			last one in the pending mask.
 *
 * Returns true if the pending bit was set and the pending mask contains an
 * online CPU other than the dying CPU.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_fixup_move_pending - 내려가는 CPU 때문에 미뤄 둔 이동을 정리한다
 *
 * @desc:        정리할 서술자.
 * @force_clear: 참이면 조건 없이 대기 비트를 지운다. 거짓이면 목표 마스크에
 *               온라인 CPU 가 하나도 남지 않을 때만 지운다.
 * @return:      대기 비트가 서 있었고, 목표 마스크에 (내려가는 CPU 말고)
 *               쓸 수 있는 온라인 CPU 가 남아 있으면 참.
 *
 * 무슨 상황인가: CPU 를 오프라인으로 내리는 중인데, 그 CPU 를 목표로 삼아
 * 미뤄 둔 친화도 변경이 남아 있을 수 있다. 그대로 두면 사라질 CPU 로
 * 인터럽트를 보내려는 시도가 남는다.
 *
 * 두 갈래로 나뉜다:
 *   목표 마스크에 온라인 CPU 가 하나도 없다 — 이동 자체가 무의미하므로
 *     대기 비트를 지우고 거짓을 돌려준다. 호출자는 다른 방법으로 이 인터럽트의
 *     갈 곳을 찾아야 한다.
 *   온라인 CPU 가 남아 있다 — 이동은 여전히 유효하다. 참을 돌려주어
 *     호출자가 그 목표로 옮기게 한다.
 *
 * force_clear 가 필요한 이유: 호출자에 따라 이 함수 뒤에 자기가 직접
 * 이동을 수행하는 경우가 있다. 그때는 대기 비트가 남아 있으면 나중에
 * 또 이동하려 하므로 미리 지워 달라고 요청한다.
 *
 * 실행 컨텍스트: CPU 핫플러그 경로, 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_migrate_all_off_this_cpu() → migrate_one_irq()/irq_needs_fixup()
 *     (kernel/irq/cpuhotplug.c) → [이 함수]
 */
bool irq_fixup_move_pending(struct irq_desc *desc, bool force_clear)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);	/* [한국어] 서술자의 최상위 irq_data. 대기 상태는 여기에 기록되어 있다 */

	if (!irqd_is_setaffinity_pending(data))	/* [한국어] 미뤄 둔 이동이 아예 없으면 */
		return false;	/* [한국어] 정리할 것도 없고, 호출자에게 알릴 목표도 없다 */

	/*
	 * The outgoing CPU might be the last online target in a pending
	 * interrupt move. If that's the case clear the pending move bit.
	 */
	/* [한국어] (위 영어 주석에 이어) 목표 마스크에 온라인 CPU 가 남아 있는지 본다.
	 *
	 * cpu_online_mask 는 이 시점에 이미 내려가는 CPU 가 빠진 상태다. CPU
	 * 핫플러그가 그 CPU 를 온라인 마스크에서 먼저 제거한 뒤 이 경로를
	 * 부르기 때문이다. 그래서 교집합이 비었다는 것은 곧 "내려가는 CPU 가
	 * 유일한 목표였다"는 뜻이 된다. */
	if (!cpumask_intersects(desc->pending_mask, cpu_online_mask)) {	/* [한국어] 목표 중 살아 있는 CPU 가 하나도 없는가 */
		irqd_clr_move_pending(data);	/* [한국어] 갈 곳이 없는 이동이라 포기한다 */
		return false;	/* [한국어] 호출자는 이 인터럽트의 갈 곳을 다른 방법으로 찾는다 */
	}
	if (force_clear)	/* [한국어] 호출자가 직접 이동을 수행할 예정이면 */
		irqd_clr_move_pending(data);	/* [한국어] 미리 지워 둔다. 남겨 두면 나중에 또 옮기려 한다 */
	return true;	/* [한국어] 쓸 수 있는 목표가 남아 있다 — 호출자가 desc->pending_mask 를 그대로 쓰면 된다 */
}

/*
 * [한국어]
 * irq_force_complete_move - 미뤄 둔 이동을 하드웨어 수준에서 강제로 끝낸다
 *
 * @desc: 대상 서술자.
 *
 * 위 fixup 이 소프트웨어 쪽 상태를 정리하는 것이라면, 이쪽은 하드웨어에
 * 남아 있는 이동의 흔적을 지운다.
 *
 * 무엇이 남는가: x86 의 벡터 관리에서는 친화도를 바꿔도 옛 CPU 의 벡터를
 * 곧바로 반납하지 않는다. 아직 그 벡터로 날아오는 인터럽트가 있을 수 있어,
 * 다음 인터럽트가 새 CPU 에 도착한 것을 확인한 뒤에 정리한다. CPU 가
 * 내려가면 그 확인을 기다릴 수 없으므로 강제로 마무리해야 한다.
 *
 * 왜 계층을 훑는가: 계층형 도메인에서 이 정리를 할 줄 아는 것은 실제 벡터를
 * 관리하는 단계(x86 이면 vector domain)뿐이다. 그 단계가 계층의 어디에
 * 있는지는 구성마다 다르므로, 위에서부터 내려가며 콜백을 가진 첫 단계를 찾는다.
 *
 * 찾으면 곧바로 반환한다 — 벡터 관리는 한 단계에서만 하므로 여러 번 부를
 * 이유가 없고, 부르면 오히려 이중 정리가 된다.
 *
 * 콜백을 가진 단계가 하나도 없으면 아무 일도 하지 않는다. 그런 아키텍처는
 * 정리할 하드웨어 상태가 없다는 뜻이다.
 *
 * 실행 컨텍스트: CPU 오프라인 경로, 프로세스 문맥. 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_migrate_all_off_this_cpu() (kernel/irq/cpuhotplug.c) → [이 함수]
 *     → chip->irq_force_complete_move (예: x86 의 apic_chip)
 */
void irq_force_complete_move(struct irq_desc *desc)
{
	for (struct irq_data *d = irq_desc_get_irq_data(desc); d; d = irqd_get_parent_data(d)) {	/* [한국어] 최상위에서 시작해 부모 방향으로 계층을 훑는다. 계층이 없는 빌드에서는 parent 가 NULL 이라 한 바퀴만 돈다 */
		if (d->chip && d->chip->irq_force_complete_move) {	/* [한국어] 이 단계가 강제 완료를 할 줄 아는가 */
			d->chip->irq_force_complete_move(d);	/* [한국어] 그 단계에 맡긴다 — 벡터 반납 같은 하드웨어 정리를 수행한다 */
			return;	/* [한국어] 한 단계만 처리한다. 벡터 관리는 한 곳에서만 하므로 계속 훑으면 이중 정리가 된다 */
		}
	}
}

/*
 * [한국어]
 * irq_move_masked_irq - 이미 마스크된 인터럽트의 미뤄 둔 이동을 수행한다
 *
 * @idata: 대상 irq_data. 계층 어디의 것이든 받는다 — 아래에서 서술자를
 *         거쳐 최상위로 되짚는다.
 *
 * 이름의 masked 가 계약이다. 이 함수는 호출자가 이미 인터럽트를 마스크해
 * 두었다고 전제하며, 스스로 마스크하지 않는다. 아래 __irq_move_irq() 가
 * 그 마스킹까지 감싼 판이다.
 *
 * 동작 순서:
 *   1. 미뤄 둔 이동이 없으면 곧바로 반환 (가장 흔한 경우).
 *   2. 대기 비트를 먼저 지운다 — 아래에서 실패해 다시 세울 수 있다.
 *   3. per-CPU 인터럽트면 경고 후 반환 (오면 안 되는 경우다).
 *   4. 목표 마스크가 비었거나 chip 이 친화도 설정을 지원하지 않으면 반환.
 *   5. 목표에 온라인 CPU 가 있으면 실제로 옮긴다.
 *   6. 목표 마스크를 비운다.
 *
 * -EBUSY 처리가 이 함수의 미묘한 부분이다. 하부 벡터 관리가 아직 이전
 * 이동의 정리를 끝내지 못했다는 뜻이며, 그때는 대기 비트를 다시 세우고
 * pending_mask 를 그대로 남겨 다음 인터럽트에서 재시도한다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 서술자 락을 쥔 상태여야 한다 —
 * 아래 assert_raw_spin_locked 가 그것을 확인한다.
 *
 * 호출 체인:
 *   handle_edge_irq()/handle_fasteoi_irq() → irq_move_irq() → [이 함수]
 *   또는 __irq_move_irq() → [이 함수]
 */
void irq_move_masked_irq(struct irq_data *idata)
{
	struct irq_desc *desc = irq_data_to_desc(idata);	/* [한국어] 어느 계층의 irq_data 든 그것을 품은 서술자로 되짚는다 */
	struct irq_data *data = &desc->irq_data;	/* [한국어] 서술자의 최상위 irq_data. 대기 상태와 pending_mask 는 여기에만 있다 */
	struct irq_chip *chip = data->chip;	/* [한국어] 최상위 단계의 컨트롤러 */

	if (likely(!irqd_is_setaffinity_pending(data)))	/* [한국어] 미뤄 둔 이동이 없는 것이 압도적으로 흔하다 — likely 로 분기 예측을 그쪽에 맞춘다 */
		return;	/* [한국어] 인터럽트 핫패스에서 불리므로 이 조기 반환이 성능에 중요하다 */

	irqd_clr_move_pending(data);	/* [한국어] 먼저 지운다. 아래에서 -EBUSY 를 받으면 다시 세운다 */

	/*
	 * Paranoia: cpu-local interrupts shouldn't be calling in here anyway.
	 */
	/* [한국어] (위 영어 주석) per-CPU 인터럽트는 여기 오면 안 된다.
	 *
	 * 왜인가: per-CPU 인터럽트는 CPU 마다 별개의 선이라 "목적지를 옮긴다"는
	 * 개념이 성립하지 않는다. 애초에 친화도 설정이 거부되므로 대기 비트가
	 * 설 수 없고, 여기 도달했다면 어딘가에서 규칙이 깨진 것이다.
	 *
	 * WARN 만 하고 진행하지 않는 이유: 옮길 곳이 없으므로 할 수 있는 일이
	 * 없다. 위에서 대기 비트는 이미 지웠으므로 다음에 또 오지는 않는다. */
	if (irqd_is_per_cpu(data)) {	/* [한국어] per-CPU 인터럽트인가 */
		WARN_ON(1);	/* [한국어] 코어의 버그다. 스택 트레이스로 어디서 왔는지 남긴다 */
		return;	/* [한국어] 옮길 곳이 없으니 그대로 물러난다 */
	}

	if (unlikely(cpumask_empty(desc->pending_mask)))	/* [한국어] 목표 마스크가 비어 있는가 — 대기 비트는 섰는데 목표가 없는 모순된 상태 */
		return;	/* [한국어] 옮길 곳을 모르므로 포기한다. 위에서 대기 비트는 이미 지워졌다 */

	if (!chip->irq_set_affinity)	/* [한국어] 이 컨트롤러가 친화도 설정을 지원하는가 */
		return;	/* [한국어] 지원하지 않으면 옮길 방법이 없다 */

	assert_raw_spin_locked(&desc->lock);	/* [한국어] 서술자 락을 쥐고 있어야 한다. pending_mask 와 상태 비트를 다루므로 필수이며, 디버그 빌드에서 계약 위반을 잡아낸다 */

	/*
	 * If there was a valid mask to work with, please
	 * do the disable, re-program, enable sequence.
	 * This is *not* particularly important for level triggered
	 * but in a edge trigger case, we might be setting rte
	 * when an active trigger is coming in. This could
	 * cause some ioapics to mal-function.
	 * Being paranoid i guess!
	 *
	 * For correct operation this depends on the caller
	 * masking the irqs.
	 */
	/* [한국어] (위 영어 주석에 이어) 왜 마스크한 상태에서만 옮겨야 하는가.
	 *
	 * 원 주석이 말하는 것: 레벨 트리거에서는 크게 문제가 안 되지만, 엣지
	 * 트리거에서는 인터럽트가 날아오는 중에 IO-APIC 의 라우팅 항목(rte)을
	 * 고치면 일부 IO-APIC 이 오동작한다. 그래서 "끄고, 다시 프로그램하고,
	 * 켠다"는 순서를 지킨다.
	 *
	 * 그 마스킹을 이 함수가 하지 않는다는 점이 중요하다. 원 주석의 마지막
	 * 문장대로 "올바른 동작은 호출자가 마스크해 두는 것에 달려 있다".
	 * 흐름 제어 핸들러는 이미 마스크한 상태이거나 ack 를 보낸 직후라
	 * 안전한 시점이고, 그렇지 않은 호출자를 위해 아래 __irq_move_irq() 가 있다. */
	if (cpumask_intersects(desc->pending_mask, cpu_online_mask)) {	/* [한국어] 목표 중에 살아 있는 CPU 가 있는가. 없으면 아래 clear 로 목표만 비우고 끝낸다 */
		int ret;	/* [한국어] 실제 설정의 결과 */

		ret = irq_do_set_affinity(data, desc->pending_mask, false);	/* [한국어] 실제로 옮긴다. force 가 거짓이라 온라인 CPU 검사를 그쪽에서도 한다 */
		/*
		 * If the there is a cleanup pending in the underlying
		 * vector management, reschedule the move for the next
		 * interrupt. Leave desc->pending_mask intact.
		 */
		/* [한국어] (위 영어 주석에 이어) -EBUSY 는 실패가 아니라 "아직"이다.
		 *
		 * 무슨 상황인가: x86 의 벡터 관리는 이전 이동에서 반납한 벡터를
		 * 곧바로 재사용하지 않는다. 그 벡터로 날아오던 인터럽트가 모두
		 * 도착했음을 확인해야 하는데, 그 확인이 끝나기 전에 또 옮기려 하면
		 * -EBUSY 를 준다.
		 *
		 * 그때는 대기 비트를 되살리고 목표 마스크를 그대로 두어, 다음
		 * 인터럽트에서 자연스럽게 재시도되게 한다. 원 주석의 "pending_mask
		 * 를 건드리지 말라"가 그 뜻이며, 아래 cpumask_clear 를 건너뛰기
		 * 위해 여기서 곧바로 반환한다. */
		if (ret == -EBUSY) {	/* [한국어] 하부 벡터 관리가 아직 준비되지 않았다 */
			irqd_set_move_pending(data);	/* [한국어] 위에서 지운 대기 비트를 되살린다 */
			return;	/* [한국어] 아래 cpumask_clear 를 건너뛴다 — 목표를 남겨 두어야 다음에 재시도할 수 있다 */
		}
	}
	cpumask_clear(desc->pending_mask);	/* [한국어] 이동을 마쳤거나 포기했다. 목표를 비워 두어야 다음 대기 비트가 옛 목표를 쓰지 않는다 */
}

/*
 * [한국어]
 * __irq_move_irq - 마스킹까지 스스로 처리하는 이동 수행
 *
 * @idata: 대상 irq_data. 계층 어디의 것이든 받는다.
 *
 * 위 irq_move_masked_irq() 가 "호출자가 마스크해 두었을 것"을 전제하는 반면,
 * 이쪽은 마스크되어 있지 않은 상태에서도 쓸 수 있게 마스킹을 감싼다.
 *
 * 이미 마스크되어 있는지 먼저 확인하는 것이 이 함수의 핵심이다. 확인 없이
 * 무조건 마스크하고 언마스크하면, 원래 마스크되어 있던 인터럽트가 언마스크되어
 * 버린다. 그 결과가 영어 주석이 경고하는 인터럽트 폭풍이다 — ONESHOT 스레드
 * 인터럽트는 스레드가 끝날 때까지 마스크를 유지해야 하는데, 여기서 풀리면
 * 같은 인터럽트가 곧바로 다시 들어와 무한히 반복된다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_move_irq() (include/linux/irq.h 의 인라인) → [이 함수]
 *     → irq_move_masked_irq()
 */
void __irq_move_irq(struct irq_data *idata)
{
	bool masked;	/* [한국어] 들어올 때 이미 마스크되어 있었는지. 나갈 때 원래대로 되돌리기 위해 기억한다 */

	/*
	 * Get top level irq_data when CONFIG_IRQ_DOMAIN_HIERARCHY is enabled,
	 * and it should be optimized away when CONFIG_IRQ_DOMAIN_HIERARCHY is
	 * disabled. So we avoid an "#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY" here.
	 */
	/* [한국어] (위 영어 주석에 이어) 계층 최상위의 irq_data 를 얻는 관용구.
	 *
	 * 왜 필요한가: 계층형 도메인에서는 이 함수가 어느 단계의 irq_data 로
	 * 불릴지 알 수 없다. 그런데 마스크·언마스크와 대기 상태는 최상위 단계의
	 * 것이어야 한다. 서술자를 거쳐 되짚으면 어디서 왔든 최상위가 나온다.
	 *
	 * 원 주석의 요점: 계층이 없는 빌드에서는 이 왕복이 항등 변환이라 컴파일러가
	 * 통째로 없앤다. 그래서 #ifdef 를 쓰지 않아도 비용이 붙지 않는다.
	 * 아래 irq_can_move_in_process_context() 도 같은 관용구를 쓴다. */
	idata = irq_desc_get_irq_data(irq_data_to_desc(idata));	/* [한국어] irq_data → 서술자 → 최상위 irq_data. 계층이 없으면 컴파일 후 사라진다 */

	if (unlikely(irqd_irq_disabled(idata)))	/* [한국어] 비활성 상태인 인터럽트는 */
		return;	/* [한국어] 지금 옮길 이유가 없다. 다시 켤 때 어차피 친화도가 다시 적용된다 */

	/*
	 * Be careful vs. already masked interrupts. If this is a
	 * threaded interrupt with ONESHOT set, we can end up with an
	 * interrupt storm.
	 */
	/* [한국어] (위 영어 주석에 이어) 이미 마스크된 인터럽트를 조심하라.
	 *
	 * 확인 없이 마스크·언마스크를 하면 ONESHOT 스레드 인터럽트의 마스크가
	 * 풀린다. ONESHOT 은 스레드 핸들러가 장치의 원인을 지울 때까지 마스크를
	 * 유지해야 하는데, 여기서 풀리면 원인이 그대로인 채 같은 인터럽트가
	 * 곧바로 다시 들어온다. 그것이 인터럽트 폭풍이며 시스템이 멈춘다.
	 *
	 * 그래서 원래 상태를 기억했다가 그대로 되돌린다 — 마스크되어 있었으면
	 * 마스크된 채로 나간다. */
	masked = irqd_irq_masked(idata);	/* [한국어] 들어올 때의 마스크 상태를 기억한다 */
	if (!masked)	/* [한국어] 마스크되어 있지 않았을 때만 */
		idata->chip->irq_mask(idata);	/* [한국어] 마스크한다. 이미 되어 있었다면 건드리지 않는다 */
	irq_move_masked_irq(idata);	/* [한국어] 마스크된 상태를 전제하는 본체를 부른다 */
	if (!masked)	/* [한국어] 우리가 마스크한 경우에만 */
		idata->chip->irq_unmask(idata);	/* [한국어] 되돌린다. 원래 마스크되어 있었다면 그대로 둔다 — 그것이 폭풍을 막는다 */
}

/*
 * [한국어]
 * irq_can_move_in_process_context - 임의의 시점에 곧바로 옮겨도 되는지 답한다
 *
 * @data:   대상 irq_data. 계층 어디의 것이든 받는다.
 * @return: 즉시 옮겨도 되면 참, 미뤄야 하면 거짓.
 *
 * internals.h 의 irq_can_move_pcntxt() 를 감싼 함수인데, 두 가지가 다르다.
 *   - 계층 최상위로 되짚는 처리가 들어 있다.
 *   - 이쪽은 실제 함수라 다른 파일(주로 x86 의 벡터 관리)에서 부를 수 있다.
 *
 * 왜 최상위여야 하는가: 이동을 미룰지 말지는 최상위 컨트롤러의 성질이다.
 * 하위 단계의 chip 플래그를 보면 엉뚱한 답이 나온다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 파일의 다른 함수들과 달리 서술자 락을
 * 요구하지 않는다 — chip 플래그만 읽으므로 경쟁이 없다.
 *
 * 호출 체인:
 *   x86 의 벡터 관리 코드 → [이 함수] → irq_can_move_pcntxt() (internals.h)
 */
bool irq_can_move_in_process_context(struct irq_data *data)
{
	/*
	 * Get the top level irq_data in the hierarchy, which is optimized
	 * away when CONFIG_IRQ_DOMAIN_HIERARCHY is disabled.
	 */
	/* [한국어] (위 영어 주석) 위 __irq_move_irq() 와 같은 관용구다.
	 * 계층이 없는 빌드에서는 항등 변환이라 컴파일러가 없앤다. */
	data = irq_desc_get_irq_data(irq_data_to_desc(data));	/* [한국어] 계층 최상위로 되짚는다 — 이동 가능 여부는 최상위 컨트롤러의 성질이다 */
	return irq_can_move_pcntxt(data);	/* [한국어] chip 이 IRQCHIP_MOVE_DEFERRED 를 선언했는지 하나로 판정한다 */
}
