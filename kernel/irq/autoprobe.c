// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains the interrupt probing code and driver APIs.
 */
/*
 * [한국어 설명] 인터럽트 번호 자동 탐지 (autoprobe.c)
 *
 * === 파일의 역할 ===
 * 자기가 어느 인터럽트 번호를 쓰는지 스스로 알려 주지 못하는 오래된 장치를
 * 위해, 커널이 실험으로 그 번호를 알아내는 코드다.
 *
 * 방법은 단순하다. 쓰이지 않는 인터럽트 선을 전부 열어 두고 "아직 울리지
 * 않음"이라고 표시해 둔 뒤, 드라이버가 장치를 자극한다. 그러면 그 장치가
 * 인터럽트를 내고, 그 선의 표시가 지워진다. 표시가 지워진 선이 그 장치의
 * 인터럽트 번호다.
 *
 * 그 표시가 IRQS_WAITING 이고, 그것을 지우는 것은 흐름 제어 핸들러다.
 * 파일 상단의 영어 주석이 그 원리를 요약하고 있다 — "핸들러가 배정되지
 * 않은 선에 인터럽트가 들어오면 IRQS_WAITING 이 지워진 채 비활성화된다".
 *
 * 오늘날에는 거의 쓰이지 않는다. PCI 는 설정 공간이 인터럽트 번호를 알려
 * 주고, 장치 트리와 ACPI 도 마찬가지다. ISA 시절의 사운드카드, 병렬 포트,
 * 플로피 컨트롤러 같은 장치만이 이 기능을 필요로 했다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 프로브 경로에서 세 함수를 순서대로 부른다:
 *
 *   드라이버 프로브 시작
 *     ↓
 *   mask = probe_irq_on()        — 후보 선을 모두 열고 표시를 남긴다
 *     ↓
 *   장치를 자극 (드라이버가 하드웨어에 인터럽트를 내게 시킨다)
 *     ↓
 *   irq = probe_irq_off(mask)    — 표시가 지워진 선을 찾아 돌려준다
 *     ↓ 또는
 *   bits = probe_irq_mask(mask)  — 여러 선을 비트맵으로 받고 싶을 때
 *
 * on 과 off/mask 는 반드시 짝을 이뤄야 한다. on 이 뮤텍스를 잡고 off/mask 가
 * 푸는 구조라, 짝을 빠뜨리면 이후의 모든 탐지가 영원히 막힌다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥. msleep 으로 잠든다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — irq_settings_can_probe(), irq_activate_and_startup(),
 *     irq_shutdown_and_deactivate(), 그리고 istate 의 IRQS_* 비트들.
 *   chip 드라이버의 irq_set_type 콜백 — 탐지 중임을 알리는 데 쓴다.
 *   async_synchronize_full() — 비동기 프로브를 먼저 끝내게 한다.
 *
 * 이 파일에 의존하는 곳: 오래된 ISA 드라이버들 (sound/oss, drivers/parport,
 *   drivers/net 의 몇몇 구형 카드). 셋 다 EXPORT_SYMBOL 되어 있다.
 *
 * 공유 상태: probing_active 뮤텍스와 서술자의 IRQS_AUTODETECT/IRQS_WAITING
 *   비트가 세 함수 사이의 유일한 통로다.
 *
 * === 주요 함수/구조체 요약 ===
 * probing_active   — 한 번에 하나의 탐지만 허용하는 뮤텍스. on 이 잡고 off 가 푼다.
 * probe_irq_on()   — 후보 선을 열고 표시를 남긴 뒤, 명백한 오탐을 걸러 낸다.
 * probe_irq_mask() — 울린 선들을 16비트 비트맵으로 돌려주고 상태를 되돌린다.
 * probe_irq_off()  — 울린 선이 하나면 그 번호를, 여럿이면 음수를 돌려준다.
 *
 * probe_irq_on 이 두 번의 sleep 과 세 번의 순회로 이루어진 것이 이 파일의
 * 핵심이다. 그 세 단계가 각각 무엇을 걸러 내는지는 함수 주석에 있다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc 와 IRQ_TYPE_PROBE 상수 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL — 세 함수를 모듈 드라이버에 공개한다 */
#include <linux/interrupt.h>	/* [한국어] probe_irq_* 의 공개 선언 */
#include <linux/delay.h>	/* [한국어] msleep — 인터럽트가 도착하기를 기다린다 */
#include <linux/async.h>	/* [한국어] async_synchronize_full — 비동기 프로브를 먼저 끝내게 한다 */

#include "internals.h"	/* [한국어] irq_settings_can_probe, irq_activate_and_startup 등 코어 내부 함수 */

/*
 * Autodetection depends on the fact that any interrupt that
 * comes in on to an unassigned handler will get stuck with
 * "IRQS_WAITING" cleared and the interrupt disabled.
 */
/* [한국어] (위 영어 주석에 이어) 한 번에 하나의 탐지만 허용하는 뮤텍스.
 *
 * 왜 필요한가: 탐지는 시스템의 모든 빈 인터럽트 선을 열어 두는 전역적인
 * 조작이다. 두 드라이버가 동시에 하면 서로의 인터럽트를 자기 것으로
 * 오인해 둘 다 틀린 답을 얻는다.
 *
 * 잠금 규칙이 이 파일에서 가장 주의할 점이다. probe_irq_on() 이 잡고,
 * probe_irq_mask() 나 probe_irq_off() 가 푼다. 즉 락의 획득과 해제가
 * 서로 다른 함수에 있어, 드라이버가 짝을 맞춰 부르지 않으면 뮤텍스가
 * 영원히 잡힌 채 남는다.
 *
 * 아래 probe_irq_off 의 kernel-doc 이 지적하는 "모듈에서 쓰면 두 호출자가
 * 겹치는 것을 막을 방법이 없다"는 이 뮤텍스의 한계를 말한 것이 아니라,
 * 짝을 맞추지 않는 잘못된 사용을 막을 수 없다는 뜻이다. */
static DEFINE_MUTEX(probing_active);

/**
 *	probe_irq_on	- begin an interrupt autodetect
 *
 *	Commence probing for an interrupt. The interrupts are scanned
 *	and a mask of potential interrupt lines is returned.
 *
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * probe_irq_on - 인터럽트 자동 탐지를 시작한다
 *
 * @return: 후보가 될 만한 인터럽트 선의 비트맵 (0~31번만). 드라이버는 이
 *          값을 보관했다가 probe_irq_off/mask 에 그대로 넘긴다.
 *
 * 세 단계로 이루어져 있고, 각 단계가 서로 다른 것을 걸러 낸다.
 *
 *   1단계 (첫 순회 + 20ms): 오래 밀려 있던 인터럽트를 흘려보낸다.
 *      부팅 전부터 어서션 상태인 선이 있을 수 있는데, 그것을 탐지 결과로
 *      세면 안 된다. 먼저 열어서 한 번 울리게 하고 지나가게 둔다.
 *      이 단계에서는 아직 AUTODETECT/WAITING 표시를 하지 않는다.
 *
 *   2단계 (둘째 순회 + 100ms): 진짜 탐지 표시를 남긴다.
 *      AUTODETECT 와 WAITING 을 세우고 다시 시작한다. 다시 시작하는 이유는
 *      영어 주석대로 1단계에서 인터럽트가 들어와 그 선이 스스로 마스크했을
 *      수 있기 때문이다.
 *
 *   3단계 (셋째 순회): 명백한 오탐을 걸러 낸다.
 *      드라이버가 아직 장치를 자극하지도 않았는데 울린 선은 다른 원인으로
 *      울린 것이다. 그런 선은 탐지 대상에서 빼고 다시 끈다.
 *
 * 남은 선들(WAITING 이 아직 서 있는 것들)의 번호가 반환 비트맵이 된다.
 *
 * 왜 32번까지만 돌려주는가: unsigned long 에 담아 돌려주는 오래된 API 라
 * 32비트 시스템에서 32개가 한계다. 자동 탐지를 쓰던 ISA 장치는 모두
 * 0~15번을 썼으므로 실용상 문제가 없었다.
 *
 * 실행 컨텍스트: 프로세스 문맥. msleep 으로 120ms 이상 잠든다. 그래서
 * 드라이버 프로브에서만 부를 수 있다.
 *
 * 잠금: probing_active 뮤텍스를 잡고 돌아온다 — 푸는 것은 off/mask 의 몫이다.
 *
 * 호출 체인:
 *   구형 ISA 드라이버의 프로브 → [probe_irq_on] → irq_activate_and_startup()
 */
unsigned long probe_irq_on(void)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned long mask = 0;	/* [한국어] 돌려줄 후보 비트맵. 3단계에서 채운다 */
	int i;	/* [한국어] 인터럽트 번호 */

	/*
	 * quiesce the kernel, or at least the asynchronous portion
	 */
	/* [한국어] (위 영어 주석) 비동기 프로브가 끝나기를 기다린다.
	 *
	 * 왜인가: 커널은 부팅을 빠르게 하려고 일부 드라이버 프로브를 비동기로
	 * 돌린다. 그 프로브들이 인터럽트를 일으키는 중에 탐지를 시작하면, 그
	 * 인터럽트를 우리 장치의 것으로 오인한다. 먼저 조용해지기를 기다린다. */
	async_synchronize_full();	/* [한국어] 비동기 초기화가 모두 끝날 때까지 잠든다 */
	mutex_lock(&probing_active);	/* [한국어] 탐지를 독점한다. 푸는 것은 probe_irq_off/mask 의 몫이다 */
	/*
	 * something may have generated an irq long ago and we want to
	 * flush such a longstanding irq before considering it as spurious.
	 */
	/* [한국어] (위 영어 주석에 이어) 1단계 — 오래 밀려 있던 인터럽트를 흘려보낸다.
	 *
	 * 아직 AUTODETECT/WAITING 표시를 하지 않는다는 점이 2단계와의 차이다.
	 * 여기서 들어오는 인터럽트는 우리가 알고 싶은 것이 아니라 치워야 할
	 * 잡음이므로, 표시를 남기면 오히려 방해가 된다. */
	for_each_irq_desc_reverse(i, desc) {	/* [한국어] 높은 번호부터 훑는다. 낮은 번호(레거시 ISA)를 나중에 열어 그쪽 인터럽트가 늦게 오게 하려는 오래된 관행이다 */
		guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 서술자 락을 잡는다. guard 라 이 반복의 끝에서 자동으로 풀린다 */
		if (!desc->action && irq_settings_can_probe(desc)) {	/* [한국어] 핸들러가 없고(빈 선이고) 탐지가 허용된 선만 대상이다. 쓰이는 선을 열면 그 드라이버가 오동작한다 */
			/*
			 * Some chips need to know about probing in
			 * progress:
			 */
			/* [한국어] (위 영어 주석) 일부 컨트롤러는 탐지 중임을 알아야 한다.
			 *
			 * IRQ_TYPE_PROBE 는 실제 트리거 방식이 아니라 "지금 탐지 중이다"
			 * 라는 신호다. 그 사실을 아는 chip 은 평소보다 관대하게 동작해,
			 * 트리거 방식을 모르는 상태에서도 인터럽트를 받아들인다. */
			if (desc->irq_data.chip->irq_set_type)	/* [한국어] 트리거 설정 콜백이 있는 chip 에만 */
				desc->irq_data.chip->irq_set_type(&desc->irq_data, IRQ_TYPE_PROBE);	/* [한국어] 탐지 중임을 알린다. 실제 트리거 방식이 아니라 특수 신호값이다 */
			irq_activate_and_startup(desc, IRQ_NORESEND);	/* [한국어] 선을 연다. NORESEND 인 이유: 밀려 있던 것을 재전송하면 그것이 탐지 결과로 잡힌다 */
		}
	}

	/* Wait for longstanding interrupts to trigger. */
	/* [한국어] (위 영어 주석) 밀려 있던 인터럽트가 도착하기를 기다린다.
	 * 20ms 는 하드웨어가 어서션 상태를 실제 인터럽트로 올리기에 충분한 시간이다. */
	msleep(20);	/* [한국어] 잠든다. 그 사이 밀린 인터럽트들이 들어와 지나간다 */

	/*
	 * enable any unassigned irqs
	 * (we must startup again here because if a longstanding irq
	 * happened in the previous stage, it may have masked itself)
	 */
	/* [한국어] (위 영어 주석에 이어) 2단계 — 진짜 탐지 표시를 남기고 다시 연다.
	 *
	 * 왜 다시 startup 하는가: 원 주석대로 1단계에서 인터럽트가 들어왔다면
	 * 그 선은 핸들러가 없어 스스로 마스크했을 수 있다. 그대로 두면 이번
	 * 탐지에서 울릴 수 없다. */
	for_each_irq_desc_reverse(i, desc) {	/* [한국어] 다시 높은 번호부터 */
		guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 서술자 락 */
		if (!desc->action && irq_settings_can_probe(desc)) {	/* [한국어] 1단계와 같은 조건 */
			desc->istate |= IRQS_AUTODETECT | IRQS_WAITING;	/* [한국어] 탐지 대상이며 아직 울리지 않았음을 표시한다. 인터럽트가 들어오면 흐름 제어 핸들러가 WAITING 을 지운다 */
			if (irq_activate_and_startup(desc, IRQ_NORESEND))	/* [한국어] 다시 연다. 반환값이 0 이 아니면 "이미 밀려 있는 인터럽트가 있다"는 뜻이다 */
				desc->istate |= IRQS_PENDING;	/* [한국어] 그 밀린 것을 기록해 둔다. 탐지가 끝나고 선을 정상 상태로 되돌릴 때 처리된다 */
		}
	}

	/*
	 * Wait for spurious interrupts to trigger
	 */
	/* [한국어] (위 영어 주석) 오탐이 나타나기를 기다린다.
	 *
	 * 1단계의 20ms 보다 훨씬 긴 100ms 인 이유: 여기서 잡으려는 것은 주기가
	 * 긴 잡음이다. 드라이버가 아직 장치를 자극하지 않았는데 이 시간 안에
	 * 울리는 선은 다른 원인이 있는 것이고, 그것을 아래에서 걸러 낸다. */
	msleep(100);	/* [한국어] 잠든다. 스스로 울리는 선들이 이 사이에 정체를 드러낸다 */

	/*
	 * Now filter out any obviously spurious interrupts
	 */
	/* [한국어] (위 영어 주석) 3단계 — 스스로 울린 선을 탐지 대상에서 뺀다.
	 *
	 * 이번에는 정방향으로 훑는다. 위 두 순회가 역방향이었던 것과 다른데,
	 * 여기서는 여는 순서가 중요하지 않고 비트맵을 낮은 번호부터 채우는
	 * 편이 자연스럽기 때문이다. */
	for_each_irq_desc(i, desc) {	/* [한국어] 낮은 번호부터 */
		guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 서술자 락 */
		if (desc->istate & IRQS_AUTODETECT) {	/* [한국어] 2단계에서 탐지 대상으로 표시한 선만 본다 */
			/* It triggered already - consider it spurious. */
			/* [한국어] (위 영어 주석) WAITING 이 지워졌다는 것은 이미 울렸다는 뜻이다.
			 * 드라이버는 아직 장치를 자극하지 않았으므로, 이 선은 우리가 찾는
			 * 것이 아니라 다른 원인으로 울리는 선이다. */
			if (!(desc->istate & IRQS_WAITING)) {	/* [한국어] 벌써 울린 선인가 */
				desc->istate &= ~IRQS_AUTODETECT;	/* [한국어] 탐지 대상에서 뺀다 */
				irq_shutdown_and_deactivate(desc);	/* [한국어] 다시 닫는다. 열어 둔 채로 두면 계속 울려 시스템을 방해한다 */
			} else if (i < 32) {	/* [한국어] 아직 울리지 않은 선. 반환값이 unsigned long 이라 32번까지만 담을 수 있다 */
				mask |= 1 << i;	/* [한국어] 후보 비트맵에 넣는다 */
			}
		}
	}

	return mask;	/* [한국어] 드라이버는 이 값을 보관했다가 probe_irq_off/mask 에 넘긴다. 32번 이상의 선도 탐지 대상으로는 남아 있어 off 가 찾아낼 수 있다 */
}
EXPORT_SYMBOL(probe_irq_on);	/* [한국어] 구형 ISA 드라이버가 모듈로 빌드될 수 있어 공개한다. GPL 제한이 없는 것은 아주 오래된 API 이기 때문이다 */

/**
 *	probe_irq_mask - scan a bitmap of interrupt lines
 *	@val:	mask of interrupts to consider
 *
 *	Scan the interrupt lines and return a bitmap of active
 *	autodetect interrupts. The interrupt probe logic state
 *	is then returned to its previous value.
 *
 *	Note: we need to scan all the irq's even though we will
 *	only return autodetect irq numbers - just so that we reset
 *	them all to a known state.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * probe_irq_mask - 울린 선들을 비트맵으로 돌려주고 탐지를 끝낸다
 *
 * @val:    probe_irq_on() 이 돌려준 후보 비트맵. 결과를 이것으로 걸러 낸다.
 * @return: 울린 선들의 비트맵 (0~15번만). val 과 AND 한 결과다.
 *
 * 아래 probe_irq_off() 와 하는 일이 거의 같고, 결과를 표현하는 방식만 다르다.
 * 이쪽은 여러 선이 울렸을 때 그것을 모두 비트맵으로 돌려주고, 저쪽은 하나만
 * 고르되 여럿이면 음수로 모호함을 알린다.
 *
 * 언제 이쪽을 쓰는가: 장치가 여러 인터럽트 선을 동시에 쓸 수 있어, 어느
 * 하나가 아니라 전부를 알아야 하는 경우다. 실제로는 거의 쓰이지 않는다.
 *
 * 16번까지만 돌려주는 것에 주의 — 위 probe_irq_on 이 32번까지 후보로 삼는
 * 것과 다르다. 반환형이 unsigned int 이고 이 API 가 ISA 의 16개 선만을
 * 염두에 두고 만들어졌기 때문이다.
 *
 * kernel-doc 의 Note 가 중요하다: 돌려줄 것은 일부뿐이지만 모든 선을 훑어야
 * 한다. 탐지로 열어 둔 선을 하나라도 닫지 않고 남기면 그 선이 계속 울리게 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. probing_active 뮤텍스를 푼다.
 *
 * 호출 체인:
 *   구형 드라이버의 프로브 → [probe_irq_mask] → irq_shutdown_and_deactivate()
 */
unsigned int probe_irq_mask(unsigned long val)
{
	unsigned int mask = 0;	/* [한국어] 울린 선들의 비트맵 */
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	int i;	/* [한국어] 인터럽트 번호 */

	for_each_irq_desc(i, desc) {	/* [한국어] kernel-doc 의 Note 대로 전부 훑는다 — 결과와 무관하게 모든 탐지 상태를 되돌려야 한다 */
		guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 서술자 락 */
		if (desc->istate & IRQS_AUTODETECT) {	/* [한국어] 탐지 대상으로 남아 있던 선만 */
			if (i < 16 && !(desc->istate & IRQS_WAITING))	/* [한국어] WAITING 이 지워졌으면 울린 것이다. 16번까지만 담는다 */
				mask |= 1 << i;	/* [한국어] 결과 비트맵에 넣는다 */

			desc->istate &= ~IRQS_AUTODETECT;	/* [한국어] 탐지 표시를 지운다 */
			irq_shutdown_and_deactivate(desc);	/* [한국어] 열어 두었던 선을 닫고 자원을 반납한다 */
		}
	}
	mutex_unlock(&probing_active);	/* [한국어] probe_irq_on 이 잡았던 뮤텍스를 여기서 푼다. 이 짝을 빠뜨리면 이후의 모든 탐지가 막힌다 */

	return mask & val;	/* [한국어] 호출자가 준 후보 목록으로 한 번 더 거른다 — on 과 off 사이에 새로 생긴 선을 결과에 넣지 않기 위해서다 */
}
EXPORT_SYMBOL(probe_irq_mask);	/* [한국어] 모듈 드라이버에 공개 */

/**
 *	probe_irq_off	- end an interrupt autodetect
 *	@val: mask of potential interrupts (unused)
 *
 *	Scans the unused interrupt lines and returns the line which
 *	appears to have triggered the interrupt. If no interrupt was
 *	found then zero is returned. If more than one interrupt is
 *	found then minus the first candidate is returned to indicate
 *	their is doubt.
 *
 *	The interrupt probe logic state is returned to its previous
 *	value.
 *
 *	BUGS: When used in a module (which arguably shouldn't happen)
 *	nothing prevents two IRQ probe callers from overlapping. The
 *	results of this are non-optimal.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * probe_irq_off - 탐지를 끝내고 울린 인터럽트 번호를 돌려준다
 *
 * @val:    probe_irq_on() 이 돌려준 후보 비트맵. 이 함수에서는 쓰이지 않는다 —
 *          kernel-doc 이 (unused) 라고 못 박고 있다. API 의 대칭을 위해
 *          인자만 남아 있으며, 위 probe_irq_mask 는 실제로 쓴다.
 * @return: 세 가지 경우로 나뉜다.
 *            0        — 아무 선도 울리지 않았다. 탐지 실패.
 *            양수 n   — n 번 선 하나만 울렸다. 확실한 답이다.
 *            음수 -n  — 여러 선이 울렸고, 그중 첫 번째가 n 이다. 모호하다.
 *
 * 음수로 모호함을 알리는 것이 이 API 의 특징이다. 드라이버는 음수를 받으면
 * 대개 탐지를 포기하거나, 절댓값을 취해 첫 후보를 그냥 써 본다.
 *
 * 왜 여러 선이 울릴 수 있는가: 인터럽트 선을 공유하는 ISA 구성에서는 우리
 * 장치의 인터럽트가 여러 선에 나타날 수 있고, 자극하는 동안 다른 장치가
 * 우연히 울릴 수도 있다.
 *
 * kernel-doc 의 BUGS 항목: 모듈에서 쓰면 두 호출자가 겹치는 것을 막을 수
 * 없다고 되어 있는데, 위 probing_active 뮤텍스가 그 겹침을 막는다. 이
 * 주석은 뮤텍스가 도입되기 전에 쓰인 것으로 보이며, 지금 남은 위험은
 * 드라이버가 on/off 짝을 맞추지 않는 경우다.
 *
 * 실행 컨텍스트: 프로세스 문맥. probing_active 뮤텍스를 푼다.
 *
 * 호출 체인:
 *   구형 드라이버의 프로브 → [probe_irq_off] → irq_shutdown_and_deactivate()
 */
int probe_irq_off(unsigned long val)
{
	int i, irq_found = 0, nr_of_irqs = 0;	/* [한국어] 차례로: 번호, 찾은 첫 번째 선(0 이면 못 찾음), 울린 선의 개수 */
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */

	for_each_irq_desc(i, desc) {	/* [한국어] 전부 훑는다. 결과와 무관하게 모든 탐지 상태를 되돌려야 한다 */
		guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 서술자 락 */
		if (desc->istate & IRQS_AUTODETECT) {	/* [한국어] 탐지 대상으로 남아 있던 선만 */
			if (!(desc->istate & IRQS_WAITING)) {	/* [한국어] WAITING 이 지워졌으면 이 선이 울린 것이다 */
				if (!nr_of_irqs)	/* [한국어] 첫 번째로 찾은 선인가 */
					irq_found = i;	/* [한국어] 그 번호를 기억한다. 정방향 순회라 가장 낮은 번호가 잡힌다 */
				nr_of_irqs++;	/* [한국어] 개수를 센다. 둘 이상이면 아래에서 음수로 만든다 */
			}
			desc->istate &= ~IRQS_AUTODETECT;	/* [한국어] 탐지 표시를 지운다 */
			irq_shutdown_and_deactivate(desc);	/* [한국어] 선을 닫고 자원을 반납한다 */
		}
	}
	mutex_unlock(&probing_active);	/* [한국어] probe_irq_on 이 잡았던 뮤텍스를 푼다 */

	if (nr_of_irqs > 1)	/* [한국어] 여러 선이 울렸으면 어느 것이 우리 것인지 확신할 수 없다 */
		irq_found = -irq_found;	/* [한국어] 음수로 만들어 모호함을 알린다. 0 번이 유일하게 울린 경우와 구분되지 않는 약점이 있지만, 0 번은 타이머라 탐지 대상이 되지 않는다 */

	return irq_found;	/* [한국어] 0 이면 실패, 양수면 확실, 음수면 모호 */
}
EXPORT_SYMBOL(probe_irq_off);	/* [한국어] 모듈 드라이버에 공개 */
