// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code, for irq-chip based
 * architectures. Detailed information is available in
 * Documentation/core-api/genericirq.rst
 */

/*
 * [한국어 설명] 인터럽트 흐름 처리기와 칩 계층 (chip.c)
 *
 * === 파일의 역할 ===
 * 인터럽트 하나를 처리하는 데는 하드웨어에 대한 약속이 필요하다. 언제
 * 마스크하고, 언제 ack 하고, 언제 EOI 를 보내고, 언제 다시 여는가.
 * 그 약속은 인터럽트의 종류에 따라 다르다. 레벨 트리거는 원인이
 * 사라질 때까지 신호가 유지되므로 핸들러가 끝날 때까지 막아 두어야
 * 하고, 에지 트리거는 순간 신호라 래치를 먼저 지우고 처리해야 한다.
 *
 * 이 파일은 그 약속들을 "흐름 처리기(flow handler)" 라는 함수로 구현한
 * 것이다. handle_level_irq(), handle_edge_irq(), handle_fasteoi_irq(),
 * handle_percpu_irq() 이 네 개가 거의 모든 경우를 덮는다. 서술자의
 * desc->handle_irq 에 그중 하나가 꽂혀 있고, 인터럽트가 오면 그것이
 * 불린다.
 *
 * 두 번째 역할은 인터럽트의 시작과 정지다. irq_startup() 과
 * irq_shutdown() 이 서술자를 "동작 가능" 과 "완전히 꺼짐" 사이에서
 * 옮긴다. 게으른 비활성(lazy disable)이라는 최적화가 여기 들어 있는데,
 * 논리적으로 끈 것과 하드웨어를 실제로 막은 것을 따로 추적해 대부분의
 * 경우 하드웨어 접근을 아낀다.
 *
 * 세 번째 역할은 계층형 도메인의 부모 칩으로 콜백을 넘겨주는
 * irq_chip_*_parent() 계열이다. 파일 뒤쪽 3 분의 1 이 전부 그것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트가 발생해 드라이버 핸들러에 닿기까지의 사슬에서 이 파일은
 * 정확히 가운데에 있다.
 *
 *   아키텍처 벡터
 *     → generic_handle_irq()        (kernel/irq/irqdesc.c)
 *     → desc->handle_irq()          ← 이 파일 (흐름 처리기)
 *     → handle_irq_event()          (kernel/irq/handle.c)
 *     → action->handler()           드라이버
 *
 * 아래로는 d->chip->irq_mask() 같은 칩 콜백을 부른다. 그 구현은
 * drivers/irqchip 이나 kernel/irq/generic-chip.c 에 있다.
 *
 * 흐름 처리기가 칩 콜백과 분리돼 있다는 것이 이 설계의 핵심이다.
 * "레벨 인터럽트는 이렇게 처리한다" 는 논리는 하드웨어와 무관하고,
 * "이 칩에서 마스크는 저 레지스터의 몇 번 비트다" 는 논리는 종류와
 * 무관하다. 둘을 나누면 N 개의 종류 × M 개의 칩 대신 N + M 개만
 * 구현하면 된다.
 *
 * 실행 컨텍스트는 함수마다 다르다. handle_* 계열은 하드 인터럽트
 * 문맥에서 desc->lock 을 잡고 실행되고, irq_set_* 계열은 프로세스
 * 문맥에서 불린다. handle_nested_irq() 만 예외로 스레드 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽:
 *   - kernel/irq/irqdesc.c → desc->handle_irq 로 이 파일의 흐름 처리기
 *   - kernel/irq/manage.c → irq_startup(), irq_shutdown(), irq_enable(),
 *     irq_disable() 를 부른다. request_irq/free_irq 의 실질적 동작이다.
 *   - drivers/irqchip → irq_set_chip_and_handler() 로 자기 칩과 처리기를
 *     서술자에 건다.
 *
 * 아래쪽:
 *   - struct irq_chip 의 콜백들 (drivers/irqchip, generic-chip.c)
 *   - kernel/irq/handle.c → handle_irq_event()
 *   - kernel/irq/resend.c → check_irq_resend()
 *   - kernel/irq/spurious.c → note_interrupt()
 *   - kernel/irq/irqdomain.c → irq_domain_activate_irq()
 *
 * 공유 상태: struct irq_desc 의 세 상태 워드를 이 파일이 가장 많이
 * 만진다. istate(IRQS_ 계열)는 코어 처리 상태, irq_data.state_use_
 * accessors(IRQD_ 계열)는 하드웨어 상태, settings(_IRQ_ 계열)는 설정
 * 플래그다. 특히 IRQD_IRQ_DISABLED 와 IRQD_IRQ_MASKED 의 구분이
 * 게으른 비활성의 근거다.
 *
 * === 주요 함수/구조체 요약 ===
 * - handle_level_irq(): 레벨 트리거. 먼저 막고 ack 한 뒤 처리하고,
 *   끝나면 조건부로 다시 연다.
 * - handle_edge_irq(): 에지 트리거. ack 로 래치를 먼저 지우고, 처리
 *   중에 새로 온 것을 루프로 이어 처리한다.
 * - handle_fasteoi_irq(): 우선순위를 하드웨어가 관리하는 컨트롤러용.
 *   EOI 한 번으로 끝나 가장 가볍다.
 * - handle_percpu_irq(): CPU 마다 별개인 인터럽트. 락도 통계 합산도
 *   없어 가장 짧다.
 * - irq_startup() / irq_shutdown(): 인터럽트를 동작 가능 상태로
 *   올리고 내린다. 관리형 인터럽트의 CPU 핫플러그 처리가 여기 얽힌다.
 * - irq_chip_*_parent(): 계층형 도메인에서 부모 칩으로 콜백을 넘긴다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc, irq_chip, IRQD_/IRQCHIP_ 상수 — 이 파일이 다루는 자료구조 전부 */
#include <linux/msi.h>	/* [한국어] struct msi_desc, msi_msg — MSI 서술자 연결과 메시지 조립에 필요 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL 계열. 이 파일의 대부분이 irqchip 드라이버 모듈에 노출된다 */
#include <linux/interrupt.h>	/* [한국어] struct irqaction, irqreturn_t — 핸들러 목록과 반환값 */
#include <linux/kernel_stat.h>	/* [한국어] kstat_incr_irqs_this_cpu — 흐름 처리기마다 발생 횟수를 올린다 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_activate_irq/deactivate_irq — 시작·정지 경로가 도메인에 자원 할당을 요청한다 */
#include <linux/random.h>	/* [한국어] add_interrupt_randomness — 인터럽트 도착 시각을 엔트로피 원으로 쓴다 */

#include <trace/events/irq.h>	/* [한국어] trace_irq_handler_entry/exit — ftrace 로 핸들러 실행 구간을 관찰한다 */

#include "internals.h"	/* [한국어] 코어 전용 선언 (desc->istate, irq_settings_*, handle_irq_event 등). 이 파일이 서술자 내부를 직접 만지므로 필수다 */

/*
 * [한국어]
 * bad_chained_irq - 체인 인터럽트에 핸들러가 불렸음을 알리는 함정
 *
 * @irq:    인터럽트 번호
 * @dev_id: 쓰지 않는다
 * @return: 항상 IRQ_NONE
 *
 * 체인(chained) 인터럽트란: 자식 인터럽트들을 묶어 올리는 부모 선이다.
 * 이 선의 처리는 컨트롤러 드라이버의 흐름 처리기가 직접 하고, 일반
 * 드라이버 핸들러가 등록되는 일은 없어야 한다.
 *
 * 그런데 코어 곳곳이 desc->action 이 NULL 인지로 "요청된 인터럽트인가"
 * 를 판단한다. 체인 인터럽트에 NULL 을 두면 그 검사들이 "요청 안 됨"
 * 으로 보고 엉뚱하게 동작한다. 그래서 실제로는 불리지 않을 가짜
 * action 을 하나 달아 둔다.
 *
 * 만약 정말 불린다면 코어의 논리에 구멍이 있다는 뜻이다. WARN_ONCE 가
 * 그것을 한 번 알린다.
 *
 * 실행 컨텍스트: 불리지 않는 것이 정상. 불린다면 인터럽트 문맥이다.
 *
 * 호출 체인:
 *   (버그가 있을 때만) handle_irq_event_percpu() → action->handler →
 *   [이 함수]
 */
static irqreturn_t bad_chained_irq(int irq, void *dev_id)
{
	WARN_ONCE(1, "Chained irq %d should not call an action\n", irq);	/* [한국어] 코어 논리의 구멍을 한 번만 알린다. 매번 찍으면 로그가 넘친다 */
	return IRQ_NONE;	/* [한국어] "내 것이 아니다". 오탐 검출기가 이것을 센다 */
}

/*
 * Chained handlers should never call action on their IRQ. This default
 * action will emit warning if such thing happens.
 */
struct irqaction chained_action = {
	/* [한국어] 체인 인터럽트의 서술자에 다는 자리 표시용 action.
	 * 설정자: __irq_do_set_handler() 가 체인으로 설정할 때 단다.
	 * 읽는 자: desc->action 이 NULL 인지 보는 코어의 모든 검사.
	 * 값 범위: 이 하나의 전역 인스턴스를 모든 체인 인터럽트가 공유한다.
	 *   각자 만들지 않는 이유는 내용이 없기 때문이다 — 존재 자체가
	 *   의미다.
	 * 동기화: 내용이 바뀌지 않으므로 공유해도 안전하다. */
	.handler = bad_chained_irq,
	/* [한국어] 실제로 불리면 경고를 내는 함정 함수.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: 정상 경로에서는 아무도 부르지 않는다. 불린다면 버그다.
	 * 값 범위: 위 bad_chained_irq.
	 * 동기화: 불필요.
	 * 나머지 필드가 전부 0 인 것도 의도적이다 — name 이 NULL,
	 *   next 가 NULL 이라 이 하나가 목록의 전부다. */
};

/**
 * irq_set_chip - set the irq chip for an irq
 * @irq:	irq number
 * @chip:	pointer to irq chip description structure
 */
/*
 * [한국어]
 * irq_set_chip - 인터럽트에 담당 컨트롤러를 붙인다
 *
 * @irq:  대상 인터럽트 번호
 * @chip: 담당할 irq_chip. NULL 이면 no_irq_chip 으로 되돌린다.
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * 인터럽트 컨트롤러 드라이버가 자기 칩을 서술자에 거는 첫 단계다.
 * 이것만으로는 인터럽트가 동작하지 않는다 — 흐름 처리기도 걸어야
 * 하는데, 그 둘을 함께 하는 irq_set_chip_and_handler_name() 이 아래
 * 있다.
 *
 * chip 이 NULL 이면 no_irq_chip 을 넣는 것에 주목: 코어가 chip
 * 포인터를 검사 없이 역참조하므로 NULL 을 그대로 두면 안 된다.
 * "떼어 낸다" 는 것은 더미 칩으로 바꾼다는 뜻이다.
 *
 * const 를 캐스팅으로 벗기는 것: 드라이버가 상수 테이블을 넘길 수
 * 있게 인자는 const 로 받지만, irq_data.chip 필드는 const 가 아니다.
 * 코어가 그 포인터로 콜백을 부를 뿐 내용을 바꾸지는 않아 안전하다.
 *
 * 마지막의 irq_mark_irq() 는 비희소 빌드 전용 뒷정리다. 그쪽에서는
 * 서술자가 정적 배열에 이미 있으므로, "이 번호는 쓰이고 있다" 는
 * 표시를 따로 해 주어야 /proc 순회에 나타난다.
 *
 * 실행 컨텍스트: 프로세스 문맥. scoped_irqdesc_get_and_lock 이
 * desc->lock 을 잡고 블록을 벗어나면 자동으로 푼다.
 *
 * 호출 체인:
 *   irqchip 드라이버 / irq_set_chip_and_handler_name() → [이 함수]
 */
int irq_set_chip(unsigned int irq, const struct irq_chip *chip)
{
	int ret = -EINVAL;	/* [한국어] 서술자를 못 찾았을 때의 기본값. 아래 블록에 진입하지 못하면 이 값이 나간다 */

	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 번호로 서술자를 찾아 락을 잡는다. 블록을 벗어나면 자동으로 풀린다 */
		scoped_irqdesc->irq_data.chip = (struct irq_chip *)(chip ?: &no_irq_chip);	/* [한국어] NULL 이면 더미 칩으로. const 를 벗기는 이유는 필드가 const 가 아니어서다 — 내용을 바꾸지는 않는다 */
		ret = 0;	/* [한국어] 성공 표시. 블록 밖에서 읽힌다 */
	}
	/* For !CONFIG_SPARSE_IRQ make the irq show up in allocated_irqs. */
	if (!ret)	/* [한국어] (위 영어 주석) 성공했을 때만 */
		irq_mark_irq(irq);	/* [한국어] 비희소 빌드에서 이 번호를 "사용 중" 으로 표시한다. 희소 빌드에서는 빈 함수다 */
	return ret;	/* [한국어] 0 또는 -EINVAL */
}
EXPORT_SYMBOL(irq_set_chip);	/* [한국어] GPL 판이 아닌 것은 역사적 이유다. 아주 오래된 API 라 예전 규약을 유지한다 */

/**
 * irq_set_irq_type - set the irq trigger type for an irq
 * @irq:	irq number
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 */
/*
 * [한국어]
 * irq_set_irq_type - 인터럽트의 트리거 방식을 바꾼다
 *
 * @irq:  대상 인터럽트 번호
 * @type: IRQ_TYPE_EDGE_RISING, IRQ_TYPE_LEVEL_HIGH 등
 * @return: 0 성공, 음수 오류 (-EINVAL 서술자 없음, 그 외 칩이 낸 값)
 *
 * 트리거 방식을 바꾸는 것은 단순한 설정 변경이 아니다. 칩의
 * irq_set_type 콜백이 하드웨어를 다시 설정하고, 그 결과에 따라 흐름
 * 처리기까지 바뀔 수 있다 — 레벨과 에지는 처리 절차가 다르기 때문이다.
 * 그 복잡한 일은 __irq_set_trigger() (kernel/irq/manage.c)가 한다.
 *
 * buslock 판을 쓰는 것에 주목: I2C/SPI 뒤에 있는 컨트롤러라면
 * 레지스터를 만지느라 잠들 수 있다. 그래서 스핀락 밖에 버스 락을
 * 한 겹 더 두르는 관용구가 필요하다.
 *
 * IRQ_GET_DESC_CHECK_GLOBAL 은 "per-CPU 인터럽트가 아닌지" 확인한다.
 * per-CPU 인터럽트의 트리거 방식은 CPU 마다 다를 수 없어 이 API 로
 * 다루지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   드라이버 (GPIO 등) → [이 함수] → __irq_set_trigger() →
 *   chip->irq_set_type()
 */
int irq_set_irq_type(unsigned int irq, unsigned int type)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL)	/* [한국어] 서술자 조회 + 버스 락 + 스핀락. per-CPU 인터럽트는 여기서 걸러진다 */
		return __irq_set_trigger(scoped_irqdesc, type);	/* [한국어] 하드웨어 설정과 흐름 처리기 교체까지 manage.c 에 위임한다 */
	return -EINVAL;	/* [한국어] 블록에 진입하지 못했다 — 그런 인터럽트가 없거나 per-CPU 였다 */
}
EXPORT_SYMBOL(irq_set_irq_type);	/* [한국어] GPIO 드라이버 등이 자주 부른다 */

/**
 * irq_set_handler_data - set irq handler data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to interrupt specific data
 *
 * Set the hardware irq controller data for an irq
 */
/*
 * [한국어]
 * irq_set_handler_data - 흐름 처리기가 쓸 사설 데이터를 건다
 *
 * @irq:  대상 인터럽트 번호
 * @data: 저장할 포인터
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * chip_data 와 헷갈리기 쉬운데 용도가 다르다. chip_data 는 칩 드라이버가
 * 자기 하드웨어 정보를 두는 곳이고, handler_data 는 흐름 처리기가
 * 쓰는 곳이다. 특히 체인 처리기가 "내 자식들이 속한 도메인" 같은
 * 정보를 여기 둔다.
 *
 * 그래서 체인 인터럽트를 설정할 때는 이 둘을 함께 해야 하고,
 * irq_set_chained_handler_and_data() 가 그 조합을 한 번에 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irqchip 드라이버 초기화 → [이 함수]
 */
int irq_set_handler_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		scoped_irqdesc->irq_common_data.handler_data = data;	/* [한국어] 흐름 처리기 전용 저장소. 계층형 도메인에서도 공통 데이터라 irq_common_data 에 있다 */
		return 0;	/* [한국어] 성공. guard 가 블록을 벗어나며 락을 푼다 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}
EXPORT_SYMBOL(irq_set_handler_data);	/* [한국어] 체인 처리기를 쓰는 드라이버가 부른다 */

/**
 * irq_set_msi_desc_off - set MSI descriptor data for an irq at offset
 * @irq_base:	Interrupt number base
 * @irq_offset:	Interrupt number offset
 * @entry:		Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq at offset
 */
/*
 * [한국어]
 * irq_set_msi_desc_off - 구간 안의 한 인터럽트에 MSI 서술자를 연결한다
 *
 * @irq_base:   MSI 구간의 첫 인터럽트 번호
 * @irq_offset: 그 안에서의 위치
 * @entry:      연결할 MSI 서술자
 * @return:     0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * MSI(Message Signaled Interrupt)는 선을 흔드는 대신 메모리 쓰기로
 * 인터럽트를 보내는 방식이다. 한 장치가 여러 벡터를 연속으로 받는
 * 경우가 많아, 구간의 시작과 오프셋으로 지정한다.
 *
 * entry->irq 를 첫 번째에만 채우는 것이 이 함수의 미묘한 지점이다.
 * MSI 서술자 하나가 여러 인터럽트를 대표할 수 있는데(MSI 는 벡터
 * 여러 개가 한 서술자에 속한다), 그 서술자가 기억하는 번호는 구간의
 * 시작이어야 한다. 오프셋이 0 이 아닌 인터럽트에서 덮어쓰면 그
 * 기준점이 어긋난다.
 *
 * 실행 컨텍스트: 프로세스 문맥, MSI 설정 경로.
 *
 * 호출 체인:
 *   msi_domain_populate_irqs() (kernel/irq/msi.c) → [이 함수]
 */
int irq_set_msi_desc_off(unsigned int irq_base, unsigned int irq_offset, struct msi_desc *entry)
{
	scoped_irqdesc_get_and_lock(irq_base + irq_offset, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] 구간 시작 + 오프셋이 실제 번호다 */
		scoped_irqdesc->irq_common_data.msi_desc = entry;	/* [한국어] 서술자에서 MSI 정보로 가는 연결. /sys 나 진단 코드가 이 경로로 장치를 찾는다 */
		if (entry && !irq_offset)	/* [한국어] 서술자를 다는 중이고, 구간의 첫 번째인가 */
			entry->irq = irq_base;	/* [한국어] 역방향 연결은 구간의 시작 번호만 기억한다. 뒤쪽 인터럽트에서 덮어쓰면 기준점이 어긋난다 */
		return 0;	/* [한국어] 성공 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없거나 per-CPU 였다 */
}

/**
 * irq_set_msi_desc - set MSI descriptor data for an irq
 * @irq:	Interrupt number
 * @entry:	Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq
 */
/*
 * [한국어]
 * irq_set_msi_desc - 인터럽트에 MSI 서술자를 연결한다
 *
 * @irq:   대상 인터럽트 번호
 * @entry: 연결할 MSI 서술자
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * 위 함수를 오프셋 0 으로 부르는 껍데기다. 벡터가 하나뿐인 MSI 나,
 * 구간을 신경 쓸 필요가 없는 호출자를 위한 편의 함수다.
 *
 * 오프셋 0 이므로 entry->irq 가 항상 채워진다는 점이 다르다 — 이
 * 인터럽트가 곧 구간의 시작이라는 뜻이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   MSI 설정 경로 → [이 함수] → irq_set_msi_desc_off()
 */
int irq_set_msi_desc(unsigned int irq, struct msi_desc *entry)
{
	return irq_set_msi_desc_off(irq, 0, entry);	/* [한국어] 오프셋 0 — 이 인터럽트가 구간의 시작이라는 뜻이다 */
}

/**
 * irq_set_chip_data - set irq chip data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to chip specific data
 *
 * Set the hardware irq chip data for an irq
 */
/*
 * [한국어]
 * irq_set_chip_data - 칩 드라이버의 사설 데이터를 건다
 *
 * @irq:  대상 인터럽트 번호
 * @data: 저장할 포인터
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * 칩 콜백들이 자기 하드웨어 정보를 되찾는 통로다. 예를 들어
 * generic-chip.c 의 모든 mask/ack 함수가 첫 줄에서
 * irq_data_get_irq_chip_data() 로 이 값을 꺼내 struct irq_chip_generic
 * 을 얻는다.
 *
 * 왜 이런 우회가 필요한가: irq_chip 의 콜백은 irq_data 하나만 받는다.
 * 콜백 시그니처에 드라이버별 인자를 넣을 수 없으므로, 서술자에 미리
 * 저장해 두고 콜백 안에서 꺼내는 방식을 쓴다.
 *
 * 위 handler_data 와 구분해야 한다. 그쪽은 흐름 처리기용, 이쪽은 칩
 * 콜백용이다. 한 인터럽트가 둘 다 가질 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irqchip 드라이버 초기화 / irq_setup_generic_chip() → [이 함수]
 */
int irq_set_chip_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		scoped_irqdesc->irq_data.chip_data = data;	/* [한국어] 칩 콜백이 되찾을 사설 데이터. 계층형 도메인에서는 층마다 다른 값을 가질 수 있어 irq_data 쪽에 있다 */
		return 0;	/* [한국어] 성공 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}
EXPORT_SYMBOL(irq_set_chip_data);	/* [한국어] 거의 모든 irqchip 드라이버가 부른다 */

/*
 * [한국어]
 * irq_get_irq_data - 인터럽트 번호로 irq_data 를 얻는다
 *
 * @irq: 대상 인터럽트 번호
 * @return: irq_data 포인터, 그런 인터럽트가 없으면 NULL
 *
 * 서술자 전체가 아니라 그 안의 irq_data 만 필요한 호출자를 위한
 * 편의 함수다. irq_data 는 칩 콜백들이 다루는 단위라, 드라이버가
 * 번호로 시작해 콜백 수준의 조작을 하려 할 때 쓴다.
 *
 * 락을 잡지 않는 것에 주목: 서술자 안의 필드 주소를 돌려줄 뿐이고,
 * 그 주소는 서술자가 사는 동안 바뀌지 않는다. 다만 반환된 포인터의
 * 내용을 읽거나 쓸 때의 보호는 호출자 몫이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   irqchip 드라이버 / irq_setup_generic_chip() → [이 함수] → irq_to_desc()
 */
struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 번호로 서술자 조회 */

	return desc ? &desc->irq_data : NULL;	/* [한국어] 서술자 안의 필드 주소. 서술자가 사는 동안 이 주소는 바뀌지 않는다 */
}
EXPORT_SYMBOL_GPL(irq_get_irq_data);	/* [한국어] 콜백 수준의 조작이 필요한 드라이버용 */

/*
 * [한국어]
 * irq_state_clr_disabled - "논리적으로 꺼짐" 표시를 내린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * IRQD_IRQ_DISABLED 는 세 상태 워드 중 IRQD_ 계열에 속하며, "코어가
 * 이 인터럽트를 꺼 두기로 했다" 를 뜻한다. 아래 IRQD_IRQ_MASKED 와
 * 반드시 구분해야 한다 — 그쪽은 "하드웨어가 실제로 막혀 있다" 다.
 *
 * 이 둘을 나눠 두는 것이 게으른 비활성(lazy disable)의 근거다.
 * disable_irq() 를 부르면 DISABLED 만 세우고 하드웨어는 건드리지
 * 않는다. 대부분의 경우 그 사이에 인터럽트가 오지 않아 하드웨어 접근을
 * 통째로 아낄 수 있다. 정말 인터럽트가 오면 그때 흐름 처리기가 막는다.
 *
 * 한 줄짜리 래퍼로 감싼 이유: 상태 조작 지점을 이름으로 표시해 두면
 * 어느 코드가 어떤 상태를 바꾸는지 추적하기 쉽다. 인라인되어 비용은
 * 없다.
 *
 * 실행 컨텍스트: desc->lock 보유 상태.
 *
 * 호출 체인:
 *   irq_enable() / __irq_startup() → [이 함수] → irqd_clear()
 */
static void irq_state_clr_disabled(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_DISABLED);	/* [한국어] 논리적 꺼짐 표시만 내린다. 하드웨어 마스크는 별개 상태다 */
}

/*
 * [한국어]
 * irq_state_clr_masked - "하드웨어가 막혀 있음" 표시를 내린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * IRQD_IRQ_MASKED 는 실제 하드웨어 상태를 반영한다. 이 비트를 내리는
 * 것은 "방금 언마스크했다" 는 기록이지 언마스크 동작 자체가 아니다.
 * 동작은 호출자가 칩 콜백으로 따로 한다.
 *
 * 왜 하드웨어를 매번 읽지 않고 소프트웨어로 추적하는가: 마스크
 * 레지스터를 읽는 것은 MMIO 왕복이라 비싸고, 애초에 읽을 수 없는
 * 컨트롤러도 많다. 그래서 코어가 자기가 시킨 것을 기억해 둔다.
 *
 * 실행 컨텍스트: desc->lock 보유 상태.
 *
 * 호출 체인:
 *   unmask_irq() / irq_enable() / __irq_startup() → [이 함수]
 */
static void irq_state_clr_masked(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_MASKED);	/* [한국어] 하드웨어가 열렸다는 기록. 실제 언마스크는 호출자가 이미 했다 */
}

/*
 * [한국어]
 * irq_state_clr_started - "시작됨" 표시를 내린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * IRQD_IRQ_STARTED 는 irq_startup() 이 성공적으로 끝났는지를 기록한다.
 * 이 비트가 필요한 이유는 시작 절차가 한 번만 실행돼야 하기
 * 때문이다 — 칩의 irq_startup 콜백이 자원을 잡을 수 있어 두 번 부르면
 * 샌다.
 *
 * 반대로 irq_shutdown() 은 이 비트가 있을 때만 실제로 정지 작업을
 * 한다. 시작한 적 없는 인터럽트를 정지하려 하면 조용히 넘어간다.
 *
 * 실행 컨텍스트: desc->lock 보유 상태.
 *
 * 호출 체인:
 *   irq_shutdown() → [이 함수]
 */
static void irq_state_clr_started(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_STARTED);	/* [한국어] 이제 시작 절차를 다시 밟을 수 있는 상태가 된다 */
}

/*
 * [한국어]
 * irq_state_set_started - "시작됨" 표시를 세운다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 clr 의 짝이다. __irq_startup() 의 마지막 줄에서 불려, 그 뒤로
 * irq_startup() 을 다시 부르면 전체 절차 대신 irq_enable() 만 하도록
 * 만든다.
 *
 * 실행 컨텍스트: desc->lock 보유 상태.
 *
 * 호출 체인:
 *   __irq_startup() → [이 함수]
 */
static void irq_state_set_started(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_STARTED);	/* [한국어] 시작 절차가 끝났다는 기록. 두 번 실행되는 것을 막는다 */
}

enum {
	/* [한국어] irq_startup() 이 어떤 경로로 가야 하는지를 나타내는
	 * 내부 결정값. __irq_startup_managed() 가 관리형 인터럽트인지,
	 * 대상 CPU 가 온라인인지를 따져 이 셋 중 하나를 돌려준다.
	 * 이름 없는 enum 이라 타입이 아니라 상수 세 개일 뿐이다 — 파일
	 * 안에서만 쓰이므로 타입을 만들 필요가 없다. */
	IRQ_STARTUP_NORMAL,
	/* [한국어] 평범한 인터럽트 — 관리형이 아니다.
	 * 설정자: __irq_startup_managed() 의 첫 갈래.
	 * 읽는 자: irq_startup() 의 switch.
	 * 값 범위: 0 (enum 의 첫 값).
	 * 의미: 친화도를 코어가 정하고 정상적으로 시작한다. */
	IRQ_STARTUP_MANAGED,
	/* [한국어] 관리형 인터럽트이고 대상 CPU 가 온라인이다.
	 * 설정자: __irq_startup_managed() 의 마지막 갈래.
	 * 읽는 자: irq_startup() 의 switch.
	 * 값 범위: 1.
	 * 의미: 친화도를 사용자가 아니라 커널이 정해 둔 값으로 강제하고
	 *   시작한다. 다중 큐 장치의 큐-CPU 대응을 지키기 위해서다. */
	IRQ_STARTUP_ABORT,
	/* [한국어] 지금은 시작할 수 없다.
	 * 설정자: __irq_startup_managed() 의 중간 갈래.
	 * 읽는 자: irq_startup() 의 switch.
	 * 값 범위: 2.
	 * 의미: 관리형인데 친화도 안에 온라인 CPU 가 하나도 없다. 오류가
	 *   아니라 "CPU 가 올라오면 그때 시작" 이라는 유예 상태이고,
	 *   irq_startup() 은 0(성공)을 돌려준다. */
};

#ifdef CONFIG_SMP	/* [한국어] 관리형 친화도와 CPU 핫플러그는 SMP 에만 있는 개념이다 */
/*
 * [한국어]
 * __irq_startup_managed - 관리형 인터럽트의 시작 가능 여부를 판정한다
 *
 * @desc:  대상 서술자
 * @aff:   이 인터럽트의 친화도 마스크
 * @force: 무조건 시작해야 하는가 (IRQ_START_FORCE)
 * @return: IRQ_STARTUP_NORMAL / MANAGED / ABORT 중 하나
 *
 * 관리형(managed) 인터럽트란: 커널이 친화도를 자동으로 정하고 사용자가
 * 바꿀 수 없는 인터럽트다. 다중 큐 NVMe 나 네트워크 카드에서 큐 하나가
 * CPU 하나에 대응하도록 묶어 둔 것이다.
 *
 * 그 묶음 때문에 CPU 핫플러그와 얽힌다. 담당 CPU 가 오프라인이 되면
 * 그 큐의 인터럽트도 받을 데가 없다. 다른 CPU 로 옮기면 큐-CPU 대응이
 * 깨지므로, 옮기는 대신 인터럽트를 잠시 꺼 둔다. CPU 가 돌아오면
 * 다시 켠다. 그 "잠시 꺼 둠" 이 IRQD_MANAGED_SHUTDOWN 이다.
 *
 * force 검사가 잡는 것: 관리형 인터럽트에 enable_irq() 를 부르거나
 * 체인 처리기를 설치하거나 자동 탐색을 돌리는 코드다. 그런 경로는
 * 관리형의 전제를 무시하므로 경고하고 거절한다.
 *
 * irq_domain_activate_irq() 가 실패하면 안 되는 이유: 관리형
 * 인터럽트는 요청 시점에 이미 벡터 등 자원을 예약해 두었다. 그러니
 * 여기서 활성화가 실패하는 것은 자원 관리의 버그다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 핫플러그 콜백, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_startup() → [이 함수] → irq_domain_activate_irq()
 */
static int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 상태 플래그를 볼 irq_data */

	if (!irqd_affinity_is_managed(d))	/* [한국어] 커널이 친화도를 관리하는 인터럽트인가 */
		return IRQ_STARTUP_NORMAL;	/* [한국어] 아니면 평범한 경로. 아래 로직 전체가 관리형 전용이다 */

	irqd_clr_managed_shutdown(d);	/* [한국어] 유예 상태를 먼저 지운다. 아래에서 다시 세울 수도 있지만, 일단 지워야 상태가 겹치지 않는다 */

	if (!cpumask_intersects(aff, cpu_online_mask)) {	/* [한국어] 친화도 안에 살아 있는 CPU 가 하나도 없는가 */
		/*
		 * Catch code which fiddles with enable_irq() on a managed
		 * and potentially shutdown IRQ. Chained interrupt
		 * installment or irq auto probing should not happen on
		 * managed irqs either.
		 */
		if (WARN_ON_ONCE(force))	/* [한국어] (위 영어 주석) 무조건 시작하라는 요청인가 — 관리형에 그런 요청을 하는 것 자체가 버그다 */
			return IRQ_STARTUP_ABORT;	/* [한국어] 경고만 하고 거절한다. 강행하면 아무 CPU 도 받을 수 없는 인터럽트가 열린다 */
		/*
		 * The interrupt was requested, but there is no online CPU
		 * in it's affinity mask. Put it into managed shutdown
		 * state and let the cpu hotplug mechanism start it up once
		 * a CPU in the mask becomes available.
		 */
		return IRQ_STARTUP_ABORT;	/* [한국어] (위 영어 주석) 오류가 아니다. CPU 가 올라오면 핫플러그 경로가 irq_startup_managed() 로 시작해 준다 */
	}
	/*
	 * Managed interrupts have reserved resources, so this should not
	 * happen.
	 */
	if (WARN_ON(irq_domain_activate_irq(d, false)))	/* [한국어] (위 영어 주석) 자원을 예약해 둔 인터럽트라 활성화가 실패할 수 없다. 실패했다면 자원 관리의 버그다 */
		return IRQ_STARTUP_ABORT;	/* [한국어] 그래도 방어적으로 중단한다 */
	return IRQ_STARTUP_MANAGED;	/* [한국어] 친화도를 커널이 정한 값으로 강제하고 시작하라는 뜻 */
}

/*
 * [한국어]
 * irq_startup_managed - CPU 가 온라인이 될 때 관리형 인터럽트를 되살린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 함수가 IRQ_STARTUP_ABORT 로 유예해 둔 인터럽트를, 담당 CPU 가
 * 돌아왔을 때 다시 켜는 함수다. CPU 핫플러그 경로가 부른다.
 *
 * depth 를 다루는 방식이 이 함수의 핵심이다. depth 는 비활성 중첩
 * 깊이라, 0 이어야 인터럽트가 열린다. 유예 시점에 depth 를 1 로
 * 올려 두었으므로 여기서 하나 내린다.
 *
 * 왜 무조건 열지 않고 depth 를 보는가: 사용자가 disable_irq() 를 부른
 * 상태에서 CPU 를 뺐다 꽂았다고 하자. 그때 무조건 열면 사용자가 끈
 * 인터럽트가 저절로 켜진다. depth 를 통해 "유예 때문에 하나, 사용자
 * 요청으로 하나" 를 구분하면 사용자 요청이 살아남는다.
 *
 * managed_shutdown 표시를 먼저 지우는 것도 같은 맥락이다. 원본 주석이
 * 말하듯 여러 번 핫플러그해도 depth 가 어긋나지 않게 한다.
 *
 * 실행 컨텍스트: CPU 핫플러그 콜백, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_affinity_online_cpu() (kernel/irq/cpuhotplug.c) → [이 함수] →
 *   irq_startup()
 */
void irq_startup_managed(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 상태 플래그를 볼 irq_data */

	/*
	 * Clear managed-shutdown flag, so we don't repeat managed-startup for
	 * multiple hotplugs, and cause imbalanced disable depth.
	 */
	irqd_clr_managed_shutdown(d);	/* [한국어] (위 영어 주석) 유예 표시를 지운다. 이것이 없으면 여러 번 핫플러그할 때 depth 가 어긋난다 */

	/*
	 * Only start it up when the disable depth is 1, so that a disable,
	 * hotunplug, hotplug sequence does not end up enabling it during
	 * hotplug unconditionally.
	 */
	desc->depth--;	/* [한국어] (위 영어 주석) 유예 때 올린 것을 되돌린다 */
	if (!desc->depth)	/* [한국어] 0 이 되었는가 — 즉 다른 이유로 꺼진 것이 없는가 */
		irq_startup(desc, IRQ_RESEND, IRQ_START_COND);	/* [한국어] 시작한다. IRQ_START_COND 는 force 가 아니라는 뜻 — 조건이 안 맞으면 다시 유예된다 */
}

#else	/* [한국어] 단일 프로세서 — 관리형 친화도라는 개념이 없다 */
/*
 * [한국어]
 * __irq_startup_managed - 관리형 판정 (UP 판, 항상 평범한 경로)
 *
 * @desc:  무시
 * @aff:   무시
 * @force: 무시
 * @return: 항상 IRQ_STARTUP_NORMAL
 *
 * CPU 가 하나면 친화도를 관리할 대상이 없고 핫플러그로 사라질 CPU 도
 * 없다. 호출부의 switch 를 그대로 두기 위해 항상 평범한 경로를
 * 돌려준다.
 *
 * __always_inline 인 이유: 상수를 반환하므로 인라인되면 컴파일러가
 * switch 의 나머지 갈래를 통째로 지운다.
 *
 * 호출 체인:
 *   irq_startup() → [이 함수]
 */
static __always_inline int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	return IRQ_STARTUP_NORMAL;	/* [한국어] 상수 반환이라 인라인 후 switch 의 나머지 갈래가 사라진다 */
}
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/*
 * [한국어]
 * irq_enable - 인터럽트를 다시 연다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 게으른 비활성(lazy disable)의 양쪽 경우를 모두 처리한다.
 *
 * 첫 갈래 — DISABLED 가 아닌 경우: 논리적으로는 이미 켜져 있는데
 * 하드웨어만 막혀 있다. 이는 흐름 처리기가 인터럽트를 억제하려고
 * 임시로 막아 둔 상태다. 언마스크만 하면 된다.
 *
 * 둘째 갈래 — DISABLED 인 경우: 논리적으로도 꺼져 있었다. 표시를
 * 내리고 하드웨어를 연다. 이때 칩이 irq_enable 콜백을 제공하면 그것을
 * 쓰고, 없으면 언마스크로 대신한다. 두 콜백이 다른 이유는 칩에 따라
 * "enable" 과 "unmask" 가 실제로 다른 레지스터일 수 있기 때문이다 —
 * 예를 들어 enable 은 클록을 켜고 unmask 는 비트만 내리는 식이다.
 *
 * irq_enable 콜백을 쓴 뒤 MASKED 를 직접 내리는 것에 주목: unmask_irq()
 * 를 거치지 않았으므로 상태 기록을 손으로 맞춰야 한다.
 *
 * 실행 컨텍스트: desc->lock 보유. 인터럽트 문맥일 수도 프로세스
 * 문맥일 수도 있다.
 *
 * 호출 체인:
 *   irq_startup() / __irq_startup() / __enable_irq() (manage.c) →
 *   [이 함수] → chip->irq_enable() 또는 unmask_irq()
 */
static void irq_enable(struct irq_desc *desc)
{
	if (!irqd_irq_disabled(&desc->irq_data)) {	/* [한국어] 논리적으로는 이미 켜져 있는가 — 흐름 처리기가 임시로 막아 둔 상태다 */
		unmask_irq(desc);	/* [한국어] 하드웨어만 열면 된다. unmask_irq 안에서 MASKED 검사와 상태 갱신을 함께 한다 */
	} else {	/* [한국어] 논리적으로도 꺼져 있었던 경우 */
		irq_state_clr_disabled(desc);	/* [한국어] 먼저 논리 표시를 내린다 */
		if (desc->irq_data.chip->irq_enable) {	/* [한국어] 칩이 전용 enable 콜백을 갖는가. unmask 와 다른 레지스터일 수 있다 */
			desc->irq_data.chip->irq_enable(&desc->irq_data);	/* [한국어] 칩 고유의 켜기 동작. 클록을 켜는 등 unmask 보다 큰 일을 할 수 있다 */
			irq_state_clr_masked(desc);	/* [한국어] unmask_irq 를 거치지 않았으므로 상태 기록을 손으로 맞춘다 */
		} else {	/* [한국어] 전용 콜백이 없는 칩 */
			unmask_irq(desc);	/* [한국어] 언마스크로 대신한다. 대부분의 칩이 이쪽이다 */
		}
	}
}

/*
 * [한국어]
 * __irq_startup - 시작 절차의 실제 하드웨어 조작 부분
 *
 * @desc: 대상 서술자
 * @return: 0 또는 칩의 irq_startup 콜백이 낸 값
 *
 * irq_startup() 에서 판정과 친화도 설정을 뺀 나머지다. 칩이 전용
 * irq_startup 콜백을 갖고 있으면 그것을, 없으면 irq_enable() 을 쓴다.
 *
 * 두 경로에서 상태 갱신 방식이 다른 것에 주목: 콜백을 쓴 경우
 * DISABLED 와 MASKED 를 직접 내리는데, 그 콜백이 어떤 레지스터를
 * 만졌는지 코어가 알 수 없어 "이제 완전히 열렸다" 고 기록하는 수밖에
 * 없기 때문이다. irq_enable() 을 쓴 경우는 그 함수가 알아서 맞춘다.
 *
 * 활성화 확인 경고: 도메인이 자원(벡터 등)을 배정하지 않은 상태에서
 * 시작하려 하면 하드웨어가 갈 곳 없는 인터럽트를 보내게 된다. 그래도
 * 진행하는 것은, 계층형 도메인에서 바깥 칩만 준비된 어중간한 상태가
 * 정상적으로 존재하기 때문이다.
 *
 * 반환값이 칩 콜백에서 오지만 대부분 0 이다. 0 이 아닌 값을 돌려주는
 * 칩은 "시작은 했지만 대기 중인 인터럽트가 있다" 를 알리는 용도로 쓴다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_startup() → [이 함수] → chip->irq_startup() 또는 irq_enable()
 */
static int __irq_startup(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 칩 콜백에 넘길 irq_data */
	int ret = 0;	/* [한국어] 칩 콜백의 반환값. 콜백이 없으면 0 그대로 */

	/* Warn if this interrupt is not activated but try nevertheless */
	WARN_ON_ONCE(!irqd_is_activated(d));	/* [한국어] (위 영어 주석) 도메인이 자원을 배정하지 않았는데 시작하려 한다. 계층형에서 어중간한 상태가 정상적으로 존재해 거절하지는 않는다 */

	if (d->chip->irq_startup) {	/* [한국어] 칩이 전용 시작 콜백을 갖는가 */
		ret = d->chip->irq_startup(d);	/* [한국어] 칩 고유의 시작 절차. 자원을 잡거나 하드웨어를 초기화할 수 있다 */
		irq_state_clr_disabled(desc);	/* [한국어] 콜백이 무엇을 했는지 알 수 없으므로 "완전히 열림" 으로 기록한다 */
		irq_state_clr_masked(desc);	/* [한국어] 위와 같은 이유 */
	} else {	/* [한국어] 전용 콜백이 없는 칩 */
		irq_enable(desc);	/* [한국어] 일반 켜기 경로. 상태 갱신도 그 안에서 한다 */
	}
	irq_state_set_started(desc);	/* [한국어] 시작 절차가 끝났음을 기록한다. 다음 irq_startup 은 전체 절차 대신 irq_enable 만 한다 */
	return ret;	/* [한국어] 대부분 0. 칩이 "대기 중인 인터럽트 있음" 을 알릴 때만 다르다 */
}

/*
 * [한국어]
 * irq_startup - 인터럽트를 동작 가능 상태로 올린다
 *
 * @desc:   대상 서술자
 * @resend: 시작 후 놓친 인터럽트를 재전송할지 (IRQ_RESEND / IRQ_NORESEND)
 * @force:  관리형 검사를 무시하고 강제할지 (IRQ_START_FORCE / IRQ_START_COND)
 * @return: 0 성공 또는 유예, 그 외 칩 콜백의 오류
 *
 * request_irq() 의 마지막 단계이자, 인터럽트가 실제로 하드웨어에서
 * 올라오기 시작하는 지점이다.
 *
 * depth 를 0 으로 미는 것이 첫 줄인 이유: 시작한다는 것은 모든 비활성
 * 중첩을 무효화한다는 뜻이다. 요청 전에 disable_irq() 를 여러 번 불러
 * depth 가 쌓여 있었더라도, 새로 요청된 인터럽트는 열린 상태로
 * 시작해야 한다.
 *
 * 이미 시작된 경우 irq_enable() 만 하는 이유: 시작 절차는 자원을
 * 잡을 수 있어 한 번만 해야 한다. 두 번째부터는 여는 것만 필요하다.
 * 공유 인터럽트에서 두 번째 드라이버가 request_irq 할 때 이 경로로 온다.
 *
 * 친화도 설정 시점이 칩마다 다른 것이 흥미롭다. IRQCHIP_AFFINITY_PRE_
 * STARTUP 플래그가 있으면 시작 *전에* 친화도를 정한다. 시작하자마자
 * 인터럽트가 올라올 수 있는 칩에서, 그 첫 인터럽트가 엉뚱한 CPU 로
 * 가는 것을 막으려는 것이다. 그렇지 않은 칩은 시작 후에 정한다 —
 * 시작 전에는 하드웨어가 친화도 설정을 받아들이지 못할 수 있어서다.
 *
 * IRQ_STARTUP_ABORT 에서 0 을 돌려주는 것에 주목: 오류가 아니다.
 * depth 를 1 로 두고 유예 표시를 세운 뒤 정상 반환한다. 호출자
 * 입장에서 request_irq 는 성공한 것이고, 담당 CPU 가 올라오면
 * 그때 인터럽트가 열린다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 핫플러그 콜백, desc->lock 보유.
 *
 * 호출 체인:
 *   __setup_irq() (kernel/irq/manage.c) / irq_startup_managed() →
 *   [이 함수] → __irq_startup() / irq_setup_affinity() / check_irq_resend()
 */
int irq_startup(struct irq_desc *desc, bool resend, bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 상태와 칩 플래그를 볼 irq_data */
	const struct cpumask *aff = irq_data_get_affinity_mask(d);	/* [한국어] 요청 친화도. 관리형 판정과 강제 설정에 쓴다 */
	int ret = 0;	/* [한국어] 반환값 */

	desc->depth = 0;	/* [한국어] 시작은 모든 비활성 중첩을 무효화한다. 요청 전에 쌓인 depth 가 있어도 새로 요청된 인터럽트는 열린 상태로 시작한다 */

	if (irqd_is_started(d)) {	/* [한국어] 이미 시작된 인터럽트인가 — 공유 인터럽트의 두 번째 요청 등 */
		irq_enable(desc);	/* [한국어] 시작 절차는 자원을 잡을 수 있어 한 번만 한다. 여기서는 여는 것만 */
	} else {	/* [한국어] 처음 시작하는 경우 */
		switch (__irq_startup_managed(desc, aff, force)) {	/* [한국어] 관리형 여부와 CPU 가용성으로 경로를 정한다 */
		case IRQ_STARTUP_NORMAL:	/* [한국어] 평범한 인터럽트 */
			if (d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP)	/* [한국어] 시작 전에 친화도를 정해야 하는 칩인가 */
				irq_setup_affinity(desc);	/* [한국어] 시작하자마자 올라올 첫 인터럽트가 엉뚱한 CPU 로 가지 않게 미리 정한다 */
			ret = __irq_startup(desc);	/* [한국어] 실제 시작 */
			if (!(d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP))	/* [한국어] 그런 칩이 아니라면 */
				irq_setup_affinity(desc);	/* [한국어] 시작 후에 정한다. 시작 전에는 하드웨어가 친화도 설정을 받아들이지 못할 수 있다 */
			break;
		case IRQ_STARTUP_MANAGED:	/* [한국어] 관리형이고 담당 CPU 가 온라인이다 */
			irq_do_set_affinity(d, aff, false);	/* [한국어] 커널이 정해 둔 친화도를 그대로 강제한다. irq_setup_affinity 를 쓰지 않는 이유는 그쪽이 부하 분산으로 값을 고를 수 있어서다 */
			ret = __irq_startup(desc);	/* [한국어] 실제 시작 */
			break;
		case IRQ_STARTUP_ABORT:	/* [한국어] 관리형인데 담당 CPU 가 전부 오프라인이다 */
			desc->depth = 1;	/* [한국어] 꺼진 상태로 둔다. 위에서 0 으로 밀었던 것을 되돌리는 셈이다 */
			irqd_set_managed_shutdown(d);	/* [한국어] 유예 표시. CPU 가 올라오면 핫플러그 경로가 이 표시를 보고 되살린다 */
			return 0;	/* [한국어] 오류가 아니다. request_irq 는 성공한 것이고 시작만 미뤄졌다 */
		}
	}
	if (resend)	/* [한국어] 시작 전에 놓친 인터럽트를 되살릴지 */
		check_irq_resend(desc, false);	/* [한국어] IRQS_PENDING 이 세워져 있으면 소프트웨어로 다시 올린다. 요청 전에 도착해 버려진 인터럽트를 구제한다 */

	return ret;	/* [한국어] 칩 콜백의 결과 */
}

/*
 * [한국어]
 * irq_activate - 도메인에 이 인터럽트의 자원 배정을 요청한다
 *
 * @desc: 대상 서술자
 * @return: 0 성공 또는 할 일 없음, 음수 도메인이 낸 오류
 *
 * 활성화(activate)가 무엇인가: 계층형 도메인에서 인터럽트가 실제로
 * 동작하려면 각 층이 자원을 배정해야 한다. x86 이라면 CPU 벡터 번호,
 * MSI 라면 메시지 주소와 데이터 값 같은 것이다. 그 배정을 요청하는
 * 것이 활성화다.
 *
 * 할당(alloc)과 활성화(activate)를 나눠 둔 이유: 할당은 인터럽트 번호와
 * 자료구조를 만드는 것이고, 활성화는 실제 하드웨어 자원을 잡는 것이다.
 * 벡터 같은 자원은 희소하므로, 정말 쓸 때까지 미루는 편이 낫다.
 *
 * 관리형 인터럽트를 건너뛰는 이유: 그쪽은 요청 시점에 이미 자원을
 * 예약해 두었다. 예약해 두지 않으면 CPU 핫플러그로 되살릴 때 자원이
 * 없을 수 있고, 그 시점에는 실패를 처리할 방법이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_activate_and_startup() / __setup_irq() → [이 함수] →
 *   irq_domain_activate_irq()
 */
int irq_activate(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 도메인에 넘길 irq_data */

	if (!irqd_affinity_is_managed(d))	/* [한국어] 관리형이 아닌가 */
		return irq_domain_activate_irq(d, false);	/* [한국어] 지금 자원을 배정받는다. false 는 "예약만" 이 아니라 실제 활성화라는 뜻 */
	return 0;	/* [한국어] 관리형은 요청 시점에 이미 예약해 두었다. 여기서 또 하면 이중 배정이 된다 */
}

/*
 * [한국어]
 * irq_activate_and_startup - 활성화와 시작을 이어서 한다
 *
 * @desc:   대상 서술자
 * @resend: 시작 후 놓친 인터럽트를 재전송할지
 * @return: irq_startup() 의 반환값, 활성화 실패 시 0
 *
 * request_irq() 경로가 부르는 조합 함수다. 두 단계가 항상 함께
 * 일어나므로 묶어 두었다.
 *
 * 활성화 실패에 0 을 돌려주는 것이 이상해 보이는데, 이 함수의 반환값을
 * 실제로 쓰는 곳이 거의 없기 때문이다. 진짜 신호는 WARN_ON 이다 —
 * 활성화 실패는 자원 고갈 같은 심각한 상황이라 로그에 남겨야 하고,
 * 그 상태에서 시작을 강행하면 안 된다.
 *
 * IRQ_START_FORCE 를 넘기는 것에 주목: 이 경로는 명시적인 요청이므로
 * 관리형 검사에 걸리면 경고가 나야 한다. 반대로 핫플러그 경로는
 * IRQ_START_COND 를 써서 조용히 유예한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   __setup_irq() / __irq_do_set_handler() → [이 함수] →
 *   irq_activate() → irq_startup()
 */
int irq_activate_and_startup(struct irq_desc *desc, bool resend)
{
	if (WARN_ON(irq_activate(desc)))	/* [한국어] 자원 배정 실패는 심각한 상황이라 로그에 남긴다 */
		return 0;	/* [한국어] 시작을 강행하지 않는다. 반환값을 쓰는 곳이 거의 없어 0 으로 둔다 */
	return irq_startup(desc, resend, IRQ_START_FORCE);	/* [한국어] 명시적 요청이므로 FORCE. 관리형 검사에 걸리면 경고가 나야 한다 */
}

static void __irq_disable(struct irq_desc *desc, bool mask);
/* [한국어] 아래에서 정의되는 비활성 함수의 전방 선언.
 * 설정자: 파일 아래쪽의 정의.
 * 읽는 자: 바로 아래 irq_shutdown() 이 부른다.
 * 값 범위: 두 번째 인자가 true 면 하드웨어까지 막고, false 면
 *   논리 표시만 내린다(게으른 비활성).
 * 동기화: 호출자가 desc->lock 을 쥐고 있어야 한다.
 *
 * 전방 선언이 필요한 이유: irq_shutdown() 이 이 함수를 부르는데,
 * 정의는 irq_shutdown 뒤에 온다. 순서를 바꾸지 않고 선언만 앞으로
 * 뺐다. */

/*
 * [한국어]
 * irq_shutdown - 인터럽트를 완전히 정지한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * free_irq() 의 마지막 단계다. irq_startup() 의 반대이지만 완전한
 * 대칭은 아니다 — 활성화 해제는 별도의 irq_shutdown_and_deactivate()
 * 가 한다.
 *
 * 시작된 적 없으면 아무것도 하지 않는다. 그래서 두 번 불러도 안전하고,
 * 요청되지 않은 인터럽트에 불러도 문제가 없다.
 *
 * depth 를 올리는 이유가 원본 주석에 있다. CPU 핫플러그로 관리형
 * 인터럽트를 내릴 때 이 함수를 거치는데, 그때 depth 를 올려 두어야
 * CPU 가 돌아왔을 때 irq_startup_managed() 의 depth-- 와 짝이 맞는다.
 * 그 짝 덕분에 "사용자가 끈 것" 과 "핫플러그로 꺼진 것" 이 구분된다.
 *
 * clear_irq_resend() 를 먼저 부르는 이유: 재전송 타이머가 걸려 있으면
 * 정지한 뒤에 인터럽트가 올라온다. 정지 전에 취소해야 한다.
 *
 * 칩의 irq_shutdown 콜백이 있으면 그것을 쓰고, 없으면 __irq_disable 로
 * 하드웨어까지 막는다. 여기서는 게으른 비활성을 쓰지 않는다 — 정지는
 * "확실히 안 온다" 를 보장해야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 핫플러그 콜백, desc->lock 보유.
 *
 * 호출 체인:
 *   __free_irq() / irq_shutdown_and_deactivate() / cpuhotplug →
 *   [이 함수] → chip->irq_shutdown() 또는 __irq_disable()
 */
void irq_shutdown(struct irq_desc *desc)
{
	if (irqd_is_started(&desc->irq_data)) {	/* [한국어] 시작된 적이 있는가. 없으면 정지할 것도 없어 두 번 불러도 안전하다 */
		clear_irq_resend(desc);	/* [한국어] 재전송 타이머를 취소한다. 정지 후에 인터럽트가 올라오는 것을 막는다 */
		/*
		 * Increment disable depth, so that a managed shutdown on
		 * CPU hotunplug preserves the actual disabled state when the
		 * CPU comes back online. See irq_startup_managed().
		 */
		desc->depth++;	/* [한국어] (위 영어 주석) 핫플러그로 되살릴 때의 depth-- 와 짝을 이룬다. 사용자가 끈 것과 핫플러그로 꺼진 것을 구분하는 장치다 */

		if (desc->irq_data.chip->irq_shutdown) {	/* [한국어] 칩이 전용 정지 콜백을 갖는가 */
			desc->irq_data.chip->irq_shutdown(&desc->irq_data);	/* [한국어] 칩 고유의 정지. 자원을 풀거나 클록을 끌 수 있다 */
			irq_state_set_disabled(desc);	/* [한국어] 콜백이 무엇을 했는지 알 수 없으므로 "완전히 꺼짐" 으로 기록한다 */
			irq_state_set_masked(desc);	/* [한국어] 위와 같은 이유 */
		} else {	/* [한국어] 전용 콜백이 없는 칩 */
			__irq_disable(desc, true);	/* [한국어] mask=true — 게으른 비활성을 쓰지 않는다. 정지는 "확실히 안 온다" 를 보장해야 한다 */
		}
		irq_state_clr_started(desc);	/* [한국어] 다음 irq_startup 이 전체 절차를 다시 밟도록 표시를 내린다 */
	}
}


/*
 * [한국어]
 * irq_shutdown_and_deactivate - 정지하고 도메인 자원까지 반납한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * irq_activate_and_startup() 의 반대다. 위 irq_shutdown() 에
 * irq_domain_deactivate_irq() 를 더한 것뿐이지만, 그 순서가 중요하다 —
 * 먼저 인터럽트를 막고 나서 자원을 반납해야 한다. 반대로 하면 벡터가
 * 회수된 뒤에 인터럽트가 올라와 갈 곳을 잃는다.
 *
 * 원본 주석이 짚는 조건부 없음의 이유: 활성화는 시작보다 먼저 일어날
 * 수 있다. 인터럽트를 할당하고 활성화까지 했지만 아직 요청되지 않은
 * 상태가 정상적으로 존재한다. 그런 경우 irq_shutdown() 은 아무것도
 * 하지 않지만 비활성화는 해야 한다. 그래서 조건 없이 부른다.
 *
 * 그것이 안전한 이유는 irq_domain_deactivate_irq() 가 자기 상태
 * (IRQD_ACTIVATED)를 따로 추적해, 활성화되지 않은 것에 불러도 조용히
 * 넘어가기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   __free_irq() / irq_domain_free_irqs() → [이 함수] →
 *   irq_shutdown() → irq_domain_deactivate_irq()
 */
void irq_shutdown_and_deactivate(struct irq_desc *desc)
{
	irq_shutdown(desc);	/* [한국어] 먼저 인터럽트를 막는다. 순서가 반대면 벡터 회수 뒤에 인터럽트가 올라온다 */
	/*
	 * This must be called even if the interrupt was never started up,
	 * because the activation can happen before the interrupt is
	 * available for request/startup. It has it's own state tracking so
	 * it's safe to call it unconditionally.
	 */
	irq_domain_deactivate_irq(&desc->irq_data);	/* [한국어] (위 영어 주석) 조건 없이 부른다. 도메인이 자기 활성화 상태를 따로 추적해 중복 호출이 안전하다 */
}

/*
 * [한국어]
 * __irq_disable - 인터럽트를 끈다 (마스크 여부를 인자로 받는다)
 *
 * @desc: 대상 서술자
 * @mask: true 면 하드웨어까지 막고, false 면 논리 표시만 내린다
 * @return: 없음
 *
 * 게으른 비활성의 구현 중심이다. mask 인자 하나로 두 가지 정책을
 * 모두 담는다.
 *
 * mask=false (게으른 비활성): DISABLED 표시만 세우고 하드웨어는
 * 그대로 둔다. 그 뒤 인터럽트가 오면 흐름 처리기가 irq_can_handle()
 * 에서 DISABLED 를 보고 처리를 건너뛰면서 그때 하드웨어를 막는다.
 * 대부분의 경우 인터럽트가 오지 않으므로 MMIO 접근을 통째로 아낀다.
 *
 * mask=true: 하드웨어까지 확실히 막는다. 정지 경로와, 드라이버가
 * IRQ_DISABLE_UNLAZY 를 요청한 경우다.
 *
 * 첫 갈래가 흥미롭다 — 이미 DISABLED 인데 mask 만 요청받은 경우다.
 * 게으른 비활성으로 논리만 껐던 인터럽트를 나중에 하드웨어까지 막을
 * 때 이 경로로 온다. 예를 들어 irq_shutdown() 이 그렇다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_disable() / irq_shutdown() → [이 함수] →
 *   chip->irq_disable() 또는 mask_irq()
 */
static void __irq_disable(struct irq_desc *desc, bool mask)
{
	if (irqd_irq_disabled(&desc->irq_data)) {	/* [한국어] 논리적으로는 이미 꺼져 있는가 — 게으른 비활성 상태다 */
		if (mask)	/* [한국어] 이번에는 하드웨어까지 막으라는 요청인가 */
			mask_irq(desc);	/* [한국어] 논리만 껐던 것을 하드웨어까지 확실히 막는다. 정지 경로가 이 길로 온다 */
	} else {	/* [한국어] 아직 켜져 있던 경우 */
		irq_state_set_disabled(desc);	/* [한국어] 논리 표시를 먼저 세운다. 이 한 줄만으로 흐름 처리기가 처리를 건너뛰게 된다 */
		if (desc->irq_data.chip->irq_disable) {	/* [한국어] 칩이 전용 끄기 콜백을 갖는가 */
			desc->irq_data.chip->irq_disable(&desc->irq_data);	/* [한국어] 칩 고유의 끄기. 이 콜백이 있으면 게으른 비활성을 쓰지 않는다 — 칩이 확실히 끌 수 있다는 뜻이기 때문이다 */
			irq_state_set_masked(desc);	/* [한국어] 하드웨어가 막혔다고 기록 */
		} else if (mask) {	/* [한국어] 전용 콜백은 없지만 하드웨어까지 막으라는 요청인가 */
			mask_irq(desc);	/* [한국어] 마스크로 대신한다. mask 가 false 면 여기까지 오지 않는다 — 그것이 게으른 비활성이다 */
		}
	}
}

/**
 * irq_disable - Mark interrupt disabled
 * @desc:	irq descriptor which should be disabled
 *
 * If the chip does not implement the irq_disable callback, we
 * use a lazy disable approach. That means we mark the interrupt
 * disabled, but leave the hardware unmasked. That's an
 * optimization because we avoid the hardware access for the
 * common case where no interrupt happens after we marked it
 * disabled. If an interrupt happens, then the interrupt flow
 * handler masks the line at the hardware level and marks it
 * pending.
 *
 * If the interrupt chip does not implement the irq_disable callback,
 * a driver can disable the lazy approach for a particular irq line by
 * calling 'irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY)'. This can
 * be used for devices which cannot disable the interrupt at the
 * device level under certain circumstances and have to use
 * disable_irq[_nosync] instead.
 */
/*
 * [한국어]
 * irq_disable - 인터럽트를 끈다 (정책은 설정에서 읽는다)
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * disable_irq() 의 실질적 구현이다. 위 __irq_disable 을 부르되 mask
 * 인자를 설정에서 읽어 온다는 점이 전부다.
 *
 * 원본 주석이 게으른 비활성의 이유와 예외를 자세히 설명한다. 요지는
 * 두 가지다.
 *
 * 첫째, 왜 게으르게 하는가: disable_irq() 를 부른 뒤 실제로 인터럽트가
 * 오는 경우는 드물다. 그런데 하드웨어 마스크는 MMIO 접근이라 비싸다.
 * 오지 않을 인터럽트를 위해 매번 비용을 치를 이유가 없다. 정말 오면
 * 그때 흐름 처리기가 막고 IRQS_PENDING 으로 표시해 두어, 나중에
 * enable_irq() 할 때 재전송된다.
 *
 * 둘째, 왜 예외가 필요한가: 어떤 장치는 자기 쪽에서 인터럽트를 끌 수
 * 없어서 컨트롤러 마스크에 의존한다. 그런 장치에 게으른 비활성을
 * 쓰면 disable_irq() 이후에도 인터럽트가 CPU 에 도달해, 짧더라도
 * 원치 않는 처리가 일어난다. IRQ_DISABLE_UNLAZY 가 그 예외 표시다.
 *
 * 칩이 irq_disable 콜백을 제공하면 이 정책과 무관하게 그것이 쓰인다.
 * 콜백이 있다는 것 자체가 "확실히 끌 수 있다" 는 뜻이기 때문이다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   __disable_irq() (kernel/irq/manage.c) → [이 함수] → __irq_disable()
 */
void irq_disable(struct irq_desc *desc)
{
	__irq_disable(desc, irq_settings_disable_unlazy(desc));	/* [한국어] IRQ_DISABLE_UNLAZY 설정을 mask 인자로 넘긴다. 세워져 있으면 하드웨어까지 막고, 아니면 논리만 끈다 */
}

/*
 * [한국어]
 * irq_percpu_enable - per-CPU 인터럽트를 이 CPU 에서 연다
 *
 * @desc: 대상 서술자
 * @cpu:  여는 CPU 번호
 * @return: 없음
 *
 * per-CPU 인터럽트는 번호 하나를 여러 CPU 가 공유하지만 실제로는 CPU
 * 마다 별개의 인터럽트다. ARM 의 지역 타이머가 대표적이다. 그래서 켜고
 * 끄는 것도 CPU 단위여야 한다.
 *
 * 일반 인터럽트의 irq_enable() 과 결정적으로 다른 점: 상태 플래그
 * (IRQD_IRQ_DISABLED/MASKED)를 전혀 건드리지 않는다. 그 플래그들은
 * 인터럽트 하나에 대한 단일 상태라, CPU 마다 다를 수 있는 per-CPU
 * 인터럽트를 표현할 수 없다. 대신 desc->percpu_enabled 비트맵이
 * CPU 별 상태를 기록한다.
 *
 * 칩 콜백을 부를 때 CPU 를 넘기지 않는 것에 주목: per-CPU 컨트롤러의
 * 레지스터는 CPU 마다 자기 것만 보이도록 배치돼 있어, 지금 실행 중인
 * CPU 의 것이 자동으로 선택된다. 그래서 이 함수는 반드시 대상 CPU
 * 위에서 실행돼야 한다.
 *
 * 실행 컨텍스트: 대상 CPU 위, desc->lock 보유.
 *
 * 호출 체인:
 *   enable_percpu_irq() (kernel/irq/manage.c) → [이 함수] →
 *   chip->irq_enable() 또는 chip->irq_unmask()
 */
void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_enable)	/* [한국어] 전용 켜기 콜백이 있는가 */
		desc->irq_data.chip->irq_enable(&desc->irq_data);	/* [한국어] CPU 를 넘기지 않는다 — per-CPU 컨트롤러는 지금 CPU 의 레지스터가 자동으로 선택된다 */
	else	/* [한국어] 없으면 */
		desc->irq_data.chip->irq_unmask(&desc->irq_data);	/* [한국어] 언마스크로 대신한다. NULL 검사가 없는데, per-CPU 칩은 둘 중 하나를 반드시 제공한다 */
	cpumask_set_cpu(cpu, desc->percpu_enabled);	/* [한국어] CPU 별 상태 비트맵. IRQD_ 플래그는 단일 상태라 per-CPU 를 표현할 수 없어 이 비트맵이 따로 있다 */
}

/*
 * [한국어]
 * irq_percpu_disable - per-CPU 인터럽트를 이 CPU 에서 끈다
 *
 * @desc: 대상 서술자
 * @cpu:  끄는 CPU 번호
 * @return: 없음
 *
 * 위 enable 의 반대다. 게으른 비활성을 쓰지 않는 것에 주목 — 일반
 * 인터럽트와 달리 여기서는 항상 하드웨어를 건드린다.
 *
 * 왜 게으르게 할 수 없는가: 게으른 비활성은 "인터럽트가 오면 흐름
 * 처리기가 그때 막는다" 를 전제한다. 그런데 per-CPU 인터럽트의 흐름
 * 처리기(handle_percpu_irq)는 락도 상태 검사도 하지 않는 최소 경로라,
 * 그런 뒤처리를 할 자리가 없다. 그래서 끌 때 확실히 끈다.
 *
 * handle_percpu_devid_irq() 에서도 이 함수를 부르는데, 그쪽은 이 CPU 에
 * 맞는 핸들러가 없는 오탐 상황에서 인터럽트를 막으려는 용도다.
 *
 * 실행 컨텍스트: 대상 CPU 위, desc->lock 보유 (또는 오탐 경로).
 *
 * 호출 체인:
 *   disable_percpu_irq() / handle_percpu_devid_irq() → [이 함수]
 */
void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_disable)	/* [한국어] 전용 끄기 콜백이 있는가 */
		desc->irq_data.chip->irq_disable(&desc->irq_data);	/* [한국어] 지금 CPU 의 레지스터가 대상이다 */
	else	/* [한국어] 없으면 */
		desc->irq_data.chip->irq_mask(&desc->irq_data);	/* [한국어] 마스크로 대신한다. 게으른 비활성이 없는 이유는 per-CPU 흐름 처리기에 뒤처리할 자리가 없어서다 */
	cpumask_clear_cpu(cpu, desc->percpu_enabled);	/* [한국어] CPU 별 상태 비트맵에서 내린다 */
}

/*
 * [한국어]
 * mask_ack_irq - 막기와 확인 처리를 한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 에지·레벨 흐름 처리기가 인터럽트를 받자마자 부르는 함수다. 두
 * 동작이 필요한 이유가 각각 다르다. 막는 것은 핸들러 실행 중에 같은
 * 인터럽트가 또 들어오는 것을 막기 위해서고, ack 는 컨트롤러의 래치를
 * 지워 다음 신호를 받을 수 있게 하기 위해서다.
 *
 * 칩이 irq_mask_ack 를 제공하면 그것을 쓴다. 두 동작을 락 한 번에
 * 원자적으로 묶을 수 있고, MMIO 접근 횟수도 줄기 때문이다.
 *
 * 없으면 mask_irq() 와 irq_ack 를 따로 부른다. 순서가 중요하다 —
 * 막고 나서 ack 해야 한다. 반대로 하면 ack 로 래치를 지운 직후,
 * 아직 막기 전인 그 짧은 틈에 새 신호가 들어와 래치가 다시 서고
 * 인터럽트가 중복 처리된다.
 *
 * mask_irq() 를 쓰는 것과 chip->irq_mask 를 직접 부르는 것의 차이:
 * mask_irq() 는 이미 막혀 있으면 건너뛰고 상태도 갱신한다. 반면
 * irq_mask_ack 경로에서는 상태 갱신을 손으로 해야 한다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_level_irq() / handle_edge_irq() / handle_fasteoi_mask_irq() →
 *   [이 함수]
 */
static inline void mask_ack_irq(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_mask_ack) {	/* [한국어] 칩이 둘을 묶은 콜백을 갖는가 */
		desc->irq_data.chip->irq_mask_ack(&desc->irq_data);	/* [한국어] 락 한 번에 원자적으로. MMIO 접근도 줄어든다 */
		irq_state_set_masked(desc);	/* [한국어] mask_irq 를 거치지 않았으므로 상태를 손으로 맞춘다 */
	} else {	/* [한국어] 따로 해야 하는 칩 */
		mask_irq(desc);	/* [한국어] 먼저 막는다. 이 순서가 중요하다 */
		if (desc->irq_data.chip->irq_ack)	/* [한국어] ack 가 필요한 칩인가. 레벨 트리거 전용 칩은 없을 수 있다 */
			desc->irq_data.chip->irq_ack(&desc->irq_data);	/* [한국어] 막은 뒤에 래치를 지운다. 순서가 반대면 그 틈에 새 신호가 들어와 중복 처리된다 */
	}
}

/*
 * [한국어]
 * mask_irq - 인터럽트를 하드웨어에서 막는다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 이 파일 전체가 하드웨어를 막을 때 거치는 단일 지점이다. 두 가지를
 * 함께 해 주기 때문에 chip->irq_mask 를 직접 부르는 것보다 낫다.
 *
 * 첫째, 이미 막혀 있으면 건너뛴다. MMIO 쓰기는 비싸고, 흐름 처리기가
 * 같은 인터럽트를 여러 번 막으려 하는 경로가 실제로 있다.
 *
 * 둘째, IRQD_IRQ_MASKED 상태를 갱신한다. 이 기록이 있어야 나중에
 * unmask_irq() 가 "정말 열어야 하는가" 를 판단할 수 있다.
 *
 * irq_mask 콜백이 없으면 아무것도 하지 않고 상태도 바꾸지 않는다.
 * 마스크 기능이 없는 컨트롤러가 실제로 있는데, 그런 칩에서는 상태를
 * 세워 두면 unmask 가 영원히 열리지 않은 것으로 착각한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   mask_ack_irq() / __irq_disable() / handle_fasteoi_irq() 등 →
 *   [이 함수] → chip->irq_mask()
 */
void mask_irq(struct irq_desc *desc)
{
	if (irqd_irq_masked(&desc->irq_data))	/* [한국어] 이미 막혀 있는가 */
		return;	/* [한국어] MMIO 쓰기를 아낀다. 흐름 처리기가 중복해서 막으려는 경로가 실제로 있다 */

	if (desc->irq_data.chip->irq_mask) {	/* [한국어] 마스크 기능이 있는 칩인가 */
		desc->irq_data.chip->irq_mask(&desc->irq_data);	/* [한국어] 하드웨어를 막는다 */
		irq_state_set_masked(desc);	/* [한국어] 콜백이 있을 때만 상태를 세운다. 없는 칩에서 세우면 unmask 가 영원히 열지 못한다 */
	}
}

/*
 * [한국어]
 * unmask_irq - 인터럽트를 하드웨어에서 다시 연다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * mask_irq() 의 정확한 반대다. 이미 열려 있으면 건너뛰고, 열었을
 * 때만 상태를 갱신한다.
 *
 * 이 대칭이 중요한 이유: 두 함수가 IRQD_IRQ_MASKED 하나를 두고 서로의
 * 조건을 이룬다. 한쪽만 상태를 갱신하면 곧 어긋나 인터럽트가 영영
 * 막히거나 이중으로 열린다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_enable() / cond_unmask_irq() / handle_edge_irq() 등 →
 *   [이 함수] → chip->irq_unmask()
 */
void unmask_irq(struct irq_desc *desc)
{
	if (!irqd_irq_masked(&desc->irq_data))	/* [한국어] 이미 열려 있는가 */
		return;	/* [한국어] 불필요한 MMIO 쓰기를 아낀다 */

	if (desc->irq_data.chip->irq_unmask) {	/* [한국어] 언마스크 기능이 있는 칩인가 */
		desc->irq_data.chip->irq_unmask(&desc->irq_data);	/* [한국어] 하드웨어를 연다 */
		irq_state_clr_masked(desc);	/* [한국어] mask_irq 와 대칭. 한쪽만 상태를 갱신하면 곧 어긋난다 */
	}
}

/*
 * [한국어]
 * unmask_threaded_irq - 스레드 핸들러가 끝난 뒤 인터럽트를 다시 연다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 스레드 인터럽트(IRQF_ONESHOT)에서 쓰인다. 그 방식은 하드 인터럽트
 * 문맥에서 선을 막아 두고, 스레드가 실제 작업을 마친 뒤에야 다시
 * 여는 것이다. 이 함수가 그 "다시 여는" 지점이다.
 *
 * IRQCHIP_EOI_THREADED 플래그가 이 함수의 존재 이유다. 보통 EOI 는
 * 흐름 처리기가 인터럽트 문맥에서 보내지만, 이 플래그를 세운 칩은
 * 스레드가 끝날 때까지 EOI 를 미뤄야 한다. 왜냐하면 EOI 를 보내는
 * 순간 컨트롤러가 같은 우선순위의 다음 인터럽트를 올릴 수 있는데,
 * 스레드가 아직 장치를 다루고 있으면 곤란하기 때문이다.
 *
 * EOI 를 먼저 보내고 언마스크하는 순서에 주목: 반대로 하면 언마스크
 * 직후 EOI 전에 인터럽트가 올라와, 우선순위가 걸린 채로 처리가 시작된다.
 *
 * 실행 컨텍스트: 스레드 문맥 (irq 스레드), desc->lock 보유.
 *
 * 호출 체인:
 *   irq_finalize_oneshot() (kernel/irq/manage.c) → [이 함수]
 */
void unmask_threaded_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;	/* [한국어] 플래그와 콜백을 볼 칩 */

	if (chip->flags & IRQCHIP_EOI_THREADED)	/* [한국어] EOI 를 스레드 종료까지 미루는 칩인가 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] 이제 보낸다. 언마스크보다 먼저 — 반대면 EOI 전에 인터럽트가 올라온다 */

	unmask_irq(desc);	/* [한국어] 선을 다시 연다. 이 시점부터 새 인터럽트를 받는다 */
}

/* Busy wait until INPROGRESS is cleared */
/*
 * [한국어]
 * irq_wait_on_inprogress - 다른 CPU 의 처리가 끝날 때까지 바쁘게 기다린다
 *
 * @desc: 대상 서술자
 * @return: true 기다린 뒤에도 처리 가능한 상태, false 처리하지 말 것
 *
 * 무엇을 기다리는가: IRQD_IRQ_INPROGRESS 는 "어느 CPU 가 이 인터럽트의
 * 핸들러를 실행 중" 을 뜻한다. 그 처리가 끝나기를 기다린다.
 *
 * 락을 놓았다 다시 잡는 구조가 핵심이다. INPROGRESS 를 내리는 쪽도
 * desc->lock 이 필요하므로, 락을 쥔 채 기다리면 영원히 끝나지 않는다.
 * 그래서 락을 놓고 cpu_relax() 로 회전하다가, 비트가 내려가면 다시
 * 잡는다. 다시 잡은 뒤 한 번 더 확인하는 이유는 그 사이에 또 다른
 * CPU 가 처리를 시작했을 수 있어서다 — 바깥 do-while 이 그것을 잡는다.
 *
 * 락을 놓은 동안 상태가 바뀔 수 있으므로 마지막에 다시 확인한다.
 * 기다리는 사이에 인터럽트가 꺼지거나 해제됐다면 처리하면 안 된다.
 *
 * UP 에서 false 인 이유: CPU 가 하나면 INPROGRESS 를 세운 것도 나
 * 자신이다. 기다리면 데드락이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유 상태로 들어와
 * 보유 상태로 나간다. 바쁜 대기라 오래 걸리면 안 된다.
 *
 * 호출 체인:
 *   irq_can_handle_pm() → [이 함수]
 */
static bool irq_wait_on_inprogress(struct irq_desc *desc)
{
	if (IS_ENABLED(CONFIG_SMP)) {	/* [한국어] UP 에서는 INPROGRESS 를 세운 것이 나 자신이라 기다리면 데드락이다 */
		do {
			raw_spin_unlock(&desc->lock);	/* [한국어] 락을 놓는다. INPROGRESS 를 내리는 쪽도 이 락이 필요해서다 */
			while (irqd_irq_inprogress(&desc->irq_data))	/* [한국어] 비트가 내려갈 때까지 */
				cpu_relax();	/* [한국어] 회전 대기 힌트. 하이퍼스레딩 형제 코어에 자원을 양보하고 전력도 아낀다 */
			raw_spin_lock(&desc->lock);	/* [한국어] 다시 잡는다 */
		} while (irqd_irq_inprogress(&desc->irq_data));	/* [한국어] 락을 놓은 사이에 또 다른 CPU 가 시작했을 수 있어 다시 확인한다 */

		/* Might have been disabled in meantime */
		return !irqd_irq_disabled(&desc->irq_data) && desc->action;	/* [한국어] (위 영어 주석) 기다리는 동안 꺼지거나 해제됐을 수 있다. 둘 다 아니어야 처리한다 */
	}
	return false;	/* [한국어] UP 판 — 기다릴 수 없으므로 처리하지 않는다 */
}

/*
 * [한국어]
 * irq_can_handle_pm - 전원 관리·경쟁 상황 관점에서 처리 가능한지 판정한다
 *
 * @desc: 대상 서술자
 * @return: true 처리해도 좋음, false 지금은 처리하지 말 것
 *
 * 흐름 처리기들이 가장 먼저 부르는 관문이다. 이름에 pm 이 들어가지만
 * 실제로는 세 가지 다른 상황을 함께 처리한다.
 *
 * (1) 절전 해제 인터럽트: 서스펜드 중에 wakeup 원이 울렸다. 처리하지
 *     말고 PM 코어에 알려 시스템을 깨워야 한다. 핸들러를 지금 부르면
 *     장치들이 아직 서스펜드된 상태라 위험하다.
 *
 * (2) 폴링 중: /proc 오탐 검출이 다른 CPU 에서 이 인터럽트를 폴링하고
 *     있다. 같은 CPU 라면 재귀라 경고하고, 다른 CPU 라면 끝나기를
 *     기다린다.
 *
 * (3) 친화도 이동 경쟁: 이것이 가장 미묘하다. 원본 주석의 그림이
 *     보여 주듯, 친화도가 CPU 1 로 옮겨진 직후 CPU 0 이 아직 이전
 *     인터럽트를 처리 중이면 무한 루프가 생길 수 있다. CPU 1 이
 *     PENDING 을 세우고 물러나면 CPU 0 이 그것을 보고 다시 처리하는데,
 *     장치가 인터럽트를 빠르게 올리면 CPU 0 이 영원히 빠져나가지
 *     못한다. 그래서 새 대상 CPU 가 잠깐 기다렸다가 자기가 처리한다.
 *     가상 머신에서 실제로 관찰된 문제다.
 *
 * 세 조건이 모두 아닌 흔한 경우는 첫 검사에서 곧바로 true 로 나간다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_simple_irq() / handle_fasteoi_irq() / irq_can_handle() 등 →
 *   [이 함수] → irq_pm_handle_wakeup() 또는 irq_wait_on_inprogress()
 */
static bool irq_can_handle_pm(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;	/* [한국어] 상태 플래그를 볼 irq_data */
	const struct cpumask *aff;	/* [한국어] 유효 친화도 (아래 세 번째 상황에서만 쓴다) */

	/*
	 * If the interrupt is not in progress and is not an armed
	 * wakeup interrupt, proceed.
	 */
	if (!irqd_has_set(irqd, IRQD_IRQ_INPROGRESS | IRQD_WAKEUP_ARMED))	/* [한국어] (위 영어 주석) 두 비트가 모두 없는가 — 흔한 경우다 */
		return true;	/* [한국어] 빠른 경로. 아래 복잡한 판정을 전부 건너뛴다 */

	/*
	 * If the interrupt is an armed wakeup source, mark it pending
	 * and suspended, disable it and notify the pm core about the
	 * event.
	 */
	if (unlikely(irqd_has_set(irqd, IRQD_WAKEUP_ARMED))) {	/* [한국어] (위 영어 주석) 서스펜드 중에 절전 해제 원이 울렸는가 */
		irq_pm_handle_wakeup(desc);	/* [한국어] 핸들러 대신 PM 코어에 알린다. 장치들이 서스펜드된 상태라 지금 핸들러를 부르면 위험하다 */
		return false;	/* [한국어] 처리하지 않는다. 시스템이 깨어난 뒤 재전송된다 */
	}

	/* Check whether the interrupt is polled on another CPU */
	if (unlikely(desc->istate & IRQS_POLL_INPROGRESS)) {	/* [한국어] (위 영어 주석) 오탐 검출기가 이 인터럽트를 폴링 중인가 */
		if (WARN_ONCE(irq_poll_cpu == smp_processor_id(),	/* [한국어] 폴링하는 CPU 가 나 자신인가 — 재귀 상황이라 기다리면 데드락이다 */
			      "irq poll in progress on cpu %d for irq %d\n",
			      smp_processor_id(), desc->irq_data.irq))
			return false;	/* [한국어] 경고하고 처리하지 않는다 */
		return irq_wait_on_inprogress(desc);	/* [한국어] 다른 CPU 라면 끝나기를 기다렸다가 처리한다 */
	}

	/* The below works only for single target interrupts */
	if (!IS_ENABLED(CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK) ||	/* [한국어] (위 영어 주석) 유효 친화도를 추적하지 않는 빌드인가 */
	    !irqd_is_single_target(irqd) || desc->handle_irq != handle_edge_irq)	/* [한국어] 대상 CPU 가 하나가 아니거나 에지 트리거가 아닌가. 아래 문제는 이 세 조건이 다 맞을 때만 생긴다 */
		return false;	/* [한국어] 해당 없으면 그냥 처리하지 않는다 — INPROGRESS 인 상태이므로 */

	/*
	 * If the interrupt affinity was moved to this CPU and the
	 * interrupt is currently handled on the previous target CPU, then
	 * busy wait for INPROGRESS to be cleared. Otherwise for edge type
	 * interrupts the handler might get stuck on the previous target:
	 *
	 * CPU 0			CPU 1 (new target)
	 * handle_edge_irq()
	 * repeat:
	 *	handle_event()		handle_edge_irq()
	 *			        if (INPROGESS) {
	 *				  set(PENDING);
	 *				  mask();
	 *				  return;
	 *				}
	 *	if (PENDING) {
	 *	  clear(PENDING);
	 *	  unmask();
	 *	  goto repeat;
	 *	}
	 *
	 * This happens when the device raises interrupts with a high rate
	 * and always before handle_event() completes and the CPU0 handler
	 * can clear INPROGRESS. This has been observed in virtual machines.
	 */
	aff = irq_data_get_effective_affinity_mask(irqd);	/* [한국어] (위 영어 주석) 하드웨어가 실제로 고른 대상 CPU */
	if (cpumask_first(aff) != smp_processor_id())	/* [한국어] 내가 새 대상 CPU 인가. 아니면 위 무한 루프의 당사자가 아니다 */
		return false;	/* [한국어] 이전 대상 CPU 가 계속 처리하게 둔다 */
	return irq_wait_on_inprogress(desc);	/* [한국어] 새 대상인 내가 잠깐 기다렸다가 처리한다. 이전 CPU 를 루프에서 풀어 주는 것이 목적이다 */
}

/*
 * [한국어]
 * irq_can_handle_actions - 등록된 핸들러가 있고 켜져 있는지 확인한다
 *
 * @desc: 대상 서술자
 * @return: true 처리 가능, false 처리 불가 (PENDING 으로 표시해 둠)
 *
 * 위 irq_can_handle_pm() 이 경쟁 상황을 본다면, 이쪽은 "처리할 대상이
 * 있는가" 를 본다. 두 가지가 아니면 처리할 수 없다 — 핸들러가 하나도
 * 없거나, 인터럽트가 논리적으로 꺼져 있는 경우다.
 *
 * 후자가 게으른 비활성의 나머지 절반이다. disable_irq() 는 하드웨어를
 * 건드리지 않고 DISABLED 표시만 세웠다. 그래서 인터럽트가 실제로
 * 올라오는데, 여기서 걸러진다.
 *
 * IRQS_PENDING 을 세우는 것이 핵심이다. "이 인터럽트가 왔었지만 지금은
 * 처리할 수 없었다" 는 기록이고, 나중에 enable_irq() 할 때
 * check_irq_resend() 가 이것을 보고 소프트웨어로 다시 올린다. 이 기록이
 * 없으면 disable 구간에 온 인터럽트가 영영 사라진다.
 *
 * 두 비트를 미리 지우는 것도 중요하다. IRQS_REPLAY 는 "이것이 재전송된
 * 인터럽트다" 는 표시이고 IRQS_WAITING 은 자동 탐색용 표시인데, 둘 다
 * 인터럽트가 도착한 순간 소임을 다한다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 또는 스레드 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_can_handle() / handle_nested_irq() / handle_simple_irq() 등 →
 *   [이 함수]
 */
static inline bool irq_can_handle_actions(struct irq_desc *desc)
{
	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);	/* [한국어] REPLAY 는 "재전송된 것" 표시, WAITING 은 자동 탐색용 표시. 인터럽트가 도착한 순간 둘 다 소임을 다한다 */

	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {	/* [한국어] 핸들러가 없거나 논리적으로 꺼져 있는가 — 후자가 게으른 비활성의 결과다 */
		desc->istate |= IRQS_PENDING;	/* [한국어] "왔었다" 는 기록. enable_irq 할 때 check_irq_resend 가 이것을 보고 다시 올린다. 없으면 인터럽트가 영영 사라진다 */
		return false;	/* [한국어] 처리하지 않는다. 호출자가 하드웨어 마스크 등 뒤처리를 한다 */
	}
	return true;	/* [한국어] 핸들러가 있고 켜져 있다 */
}

/*
 * [한국어]
 * irq_can_handle - 두 관문을 모두 통과하는지 확인한다
 *
 * @desc: 대상 서술자
 * @return: true 처리 가능, false 처리 불가
 *
 * 위 두 판정 함수를 순서대로 부르는 조합이다. 순서가 정해져 있다 —
 * PM·경쟁 상황을 먼저 보고, 그 다음에 핸들러 유무를 본다.
 *
 * 왜 그 순서인가: irq_can_handle_actions() 는 부작용이 있다. 실패하면
 * IRQS_PENDING 을 세운다. 절전 해제 인터럽트처럼 아예 다른 방식으로
 * 처리돼야 하는 경우에 그 표시를 세우면 나중에 엉뚱하게 재전송된다.
 *
 * 모든 흐름 처리기가 이것을 쓰지는 않는다. handle_simple_irq() 와
 * handle_fasteoi_irq() 는 두 판정 사이에 자기만의 뒤처리(EOI, PENDING
 * 설정)를 끼워 넣어야 해서 따로 부른다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_level_irq() / handle_edge_irq() / handle_untracked_irq() →
 *   [이 함수]
 */
static inline bool irq_can_handle(struct irq_desc *desc)
{
	if (!irq_can_handle_pm(desc))	/* [한국어] PM·경쟁 상황을 먼저 본다. 이쪽이 먼저인 이유는 아래가 부작용(PENDING 설정)을 갖기 때문이다 */
		return false;	/* [한국어] 절전 해제나 경쟁 상황 — 호출자가 각자의 방식으로 처리한다 */

	return irq_can_handle_actions(desc);	/* [한국어] 핸들러 유무와 활성 여부. 실패하면 PENDING 을 세운다 */
}

/**
 * handle_nested_irq - Handle a nested irq from a irq thread
 * @irq:	the interrupt number
 *
 * Handle interrupts which are nested into a threaded interrupt
 * handler. The handler function is called inside the calling threads
 * context.
 */
/*
 * [한국어]
 * handle_nested_irq - 스레드 문맥에서 중첩된 인터럽트를 처리한다
 *
 * @irq: 처리할 인터럽트 번호
 * @return: 없음
 *
 * 다른 흐름 처리기들과 근본적으로 다른 하나다. 나머지는 전부 하드
 * 인터럽트 문맥에서 실행되지만, 이것은 부모 인터럽트의 스레드 핸들러
 * 안에서 실행된다.
 *
 * 왜 그런 것이 필요한가: I2C 나 SPI 로 붙은 GPIO 확장 칩을 생각하자.
 * 그 칩의 인터럽트 상태를 읽으려면 버스 전송을 해야 하고, 그것은
 * 잠들 수 있다. 하드 인터럽트 문맥에서는 불가능하다. 그래서 부모
 * 인터럽트를 스레드로 처리하고, 그 스레드 안에서 자식 인터럽트를
 * 이 함수로 처리한다.
 *
 * 그 결과 몇 가지가 달라진다. might_sleep() 으로 문맥을 확인하고,
 * handler 가 아니라 thread_fn 을 부르며(자식 인터럽트의 핸들러는
 * 처음부터 스레드용으로 등록된다), 락을 짧게만 잡는다.
 *
 * 락 구조에 주목: 상태 검사와 카운터 증가는 락 안에서, 실제 핸들러
 * 호출은 락 밖에서 한다. 핸들러가 잠들 수 있으므로 스핀락을 쥔 채로
 * 부를 수 없기 때문이다. threads_active 를 미리 올려 두는 것이
 * 그 사이의 안전을 보장한다 — synchronize_irq() 가 이 카운터를 보고
 * 기다린다.
 *
 * for_each_action_of_desc 를 락 밖에서 도는 것이 위험해 보이지만,
 * 이 경로의 인터럽트는 공유되지 않아 목록이 바뀌지 않는다.
 *
 * 실행 컨텍스트: 부모 인터럽트의 스레드 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   GPIO 확장 칩 드라이버의 스레드 핸들러 → [이 함수] →
 *   action->thread_fn() → note_interrupt() → wake_threads_waitq()
 */
void handle_nested_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irqaction *action;	/* [한국어] 핸들러 목록 순회용 */
	irqreturn_t action_ret;	/* [한국어] 핸들러들의 반환값을 OR 로 모은다 */

	might_sleep();	/* [한국어] 잠들 수 있는 문맥인지 확인한다. 하드 인터럽트 문맥에서 부르면 여기서 걸린다 */

	scoped_guard(raw_spinlock_irq, &desc->lock) {	/* [한국어] 상태 검사와 카운터 증가만 락 안에서. 핸들러는 잠들 수 있어 락 밖에서 불러야 한다 */
		if (!irq_can_handle_actions(desc))	/* [한국어] 핸들러가 있고 켜져 있는가. PM 판정은 하지 않는다 — 이 경로는 하드 인터럽트가 아니라 경쟁 상황이 다르다 */
			return;	/* [한국어] guard 가 락을 풀어 준다 */

		action = desc->action;	/* [한국어] 목록의 시작. 아래 루프가 이 값을 다시 읽지만 여기서 한 번 잡아 둔다 */
		kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수. 락 안이라 tot_count 합산도 안전하다 */
		atomic_inc(&desc->threads_active);	/* [한국어] 락을 놓기 전에 올린다. synchronize_irq() 가 이 카운터를 보고 처리 종료를 기다리므로, 락 밖에서 핸들러를 부르는 동안의 안전을 이것이 보장한다 */
	}

	action_ret = IRQ_NONE;	/* [한국어] "아무도 처리 안 함" 에서 시작해 OR 로 모은다 */
	for_each_action_of_desc(desc, action)	/* [한국어] 락 밖에서 순회한다. 이 경로의 인터럽트는 공유되지 않아 목록이 바뀌지 않는다 */
		action_ret |= action->thread_fn(action->irq, action->dev_id);	/* [한국어] handler 가 아니라 thread_fn 이다 — 이 인터럽트의 핸들러는 처음부터 스레드용으로 등록됐다 */

	if (!irq_settings_no_debug(desc))	/* [한국어] 오탐 검출을 끄지 않은 인터럽트인가 */
		note_interrupt(desc, action_ret);	/* [한국어] 아무도 처리하지 못한 인터럽트가 계속되면 선을 꺼 버린다 (kernel/irq/spurious.c) */

	wake_threads_waitq(desc);	/* [한국어] threads_active 를 내리고, 0 이 되면 synchronize_irq() 로 기다리던 쪽을 깨운다 */
}
EXPORT_SYMBOL_GPL(handle_nested_irq);	/* [한국어] GPIO 확장 칩 드라이버가 모듈인 경우가 많다 */

/**
 * handle_simple_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Simple interrupts are either sent from a demultiplexing interrupt
 * handler or come from hardware, where no interrupt hardware control is
 * necessary.
 *
 * Note: The caller is expected to handle the ack, clear, mask and unmask
 * issues if necessary.
 */
/*
 * [한국어]
 * handle_simple_irq - 하드웨어 제어가 필요 없는 인터럽트를 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 가장 단순한 흐름 처리기다. 마스크도 ack 도 EOI 도 하지 않고 그냥
 * 핸들러를 부른다.
 *
 * 언제 쓰는가: 두 경우다. 하나는 디먹스 컨트롤러의 자식 인터럽트로,
 * 하드웨어 제어를 부모 처리기가 이미 다 해 주는 경우다. 다른 하나는
 * 애초에 마스크할 하드웨어가 없는 가상 인터럽트다.
 *
 * 원본 주석이 경고하듯, 하드웨어 제어가 필요하다면 호출자가 알아서
 * 해야 한다. 이 처리기는 아무것도 해 주지 않는다.
 *
 * PM 판정 실패 시 IRQS_PENDING 을 세우는 조건이 미묘하다.
 * irqd_needs_resend_when_in_progress() 는 "친화도 이동 중에 놓친
 * 인터럽트를 재전송해야 하는 칩" 을 뜻한다. 그런 칩에서만 표시를
 * 남기는 이유는, 대부분의 칩은 그런 경우 하드웨어가 다시 올려 주기
 * 때문이다. 불필요하게 표시하면 중복 처리가 된다.
 *
 * guard 를 쓰는 것에 주목: 함수 전체가 락 안이다. 핸들러가 잠들 수
 * 없는 하드 인터럽트 문맥이므로 가능하다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 (또는 디먹스 처리기의 문맥).
 *
 * 호출 체인:
 *   generic_handle_irq() → desc->handle_irq → [이 함수] → handle_irq_event()
 */
void handle_simple_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안이다. 이 경로의 핸들러는 잠들지 않으므로 가능하다 */

	if (!irq_can_handle_pm(desc)) {	/* [한국어] 절전 해제나 경쟁 상황인가 */
		if (irqd_needs_resend_when_in_progress(&desc->irq_data))	/* [한국어] 친화도 이동 중 놓친 것을 소프트웨어로 재전송해야 하는 칩인가 */
			desc->istate |= IRQS_PENDING;	/* [한국어] 그런 칩에서만 표시한다. 대부분의 칩은 하드웨어가 다시 올려 주므로 표시하면 중복 처리가 된다 */
		return;
	}

	if (!irq_can_handle_actions(desc))	/* [한국어] 핸들러가 있고 켜져 있는가 */
		return;	/* [한국어] 실패했다면 그 안에서 PENDING 을 이미 세웠다 */

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수 */
	handle_irq_event(desc);	/* [한국어] 핸들러 실행. 이 안에서 INPROGRESS 를 세우고 락을 잠시 놓는다 */
}
EXPORT_SYMBOL_GPL(handle_simple_irq);	/* [한국어] 디먹스 컨트롤러 드라이버가 자식 인터럽트에 건다 */

/**
 * handle_untracked_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Untracked interrupts are sent from a demultiplexing interrupt handler
 * when the demultiplexer does not know which device it its multiplexed irq
 * domain generated the interrupt. IRQ's handled through here are not
 * subjected to stats tracking, randomness, or spurious interrupt
 * detection.
 *
 * Note: Like handle_simple_irq, the caller is expected to handle the ack,
 * clear, mask and unmask issues if necessary.
 */
/*
 * [한국어]
 * handle_untracked_irq - 통계·엔트로피·오탐 검출 없이 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 어떤 상황을 위한 것인가: 디먹스 컨트롤러가 "무언가 울렸는데 어느
 * 자식인지 모르겠다" 는 경우다. 그러면 등록된 모든 핸들러에게 물어볼
 * 수밖에 없다.
 *
 * 그때 통계를 세면 안 된다. 인터럽트 하나가 왔는데 여러 자식의 카운터를
 * 올리면 /proc/interrupts 가 거짓말을 하게 된다. 오탐 검출도 마찬가지다 —
 * 대부분의 핸들러가 IRQ_NONE 을 돌려주는 것이 정상인 상황이라, 검출기가
 * 멀쩡한 선을 꺼 버린다. 엔트로피 수집도 의미가 없다.
 *
 * 그래서 handle_irq_event() 대신 __handle_irq_event_percpu() 를 직접
 * 부른다. 전자는 통계·엔트로피·오탐 검출을 모두 포함하지만 후자는
 * 핸들러 호출만 한다.
 *
 * INPROGRESS 를 손으로 세우고 내리는 것도 그 때문이다. handle_irq_event()
 * 가 해 주던 일을 직접 해야 한다. 락을 놓았다 다시 잡는 구조인데,
 * INPROGRESS 가 세워져 있어 그 사이에 다른 CPU 가 같은 인터럽트를
 * 처리하려 하면 irq_can_handle_pm() 에서 걸린다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   디먹스 컨트롤러 드라이버 → desc->handle_irq → [이 함수] →
 *   __handle_irq_event_percpu()
 */
void handle_untracked_irq(struct irq_desc *desc)
{
	scoped_guard(raw_spinlock, &desc->lock) {	/* [한국어] 상태 검사와 표시만 락 안에서 */
		if (!irq_can_handle(desc))	/* [한국어] 두 관문을 모두 통과하는가 */
			return;

		desc->istate &= ~IRQS_PENDING;	/* [한국어] 처리를 시작하므로 대기 표시를 지운다 */
		irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);	/* [한국어] 손으로 세운다. handle_irq_event 를 쓰지 않으므로 그 안에서 해 주던 일을 직접 해야 한다 */
	}

	__handle_irq_event_percpu(desc);	/* [한국어] 락 밖에서 핸들러만 부른다. handle_irq_event 와 달리 통계·엔트로피·오탐 검출을 하지 않는다 — 어느 자식이 울렸는지 모르므로 그 정보가 거짓이 된다 */

	scoped_guard(raw_spinlock, &desc->lock)	/* [한국어] 다시 잡아 표시를 내린다 */
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);	/* [한국어] 처리 완료. 이 비트가 있는 동안 다른 CPU 는 irq_can_handle_pm 에서 걸린다 */
}
EXPORT_SYMBOL_GPL(handle_untracked_irq);	/* [한국어] 디먹스 컨트롤러 드라이버용 */

/*
 * Called unconditionally from handle_level_irq() and only for oneshot
 * interrupts from handle_fasteoi_irq()
 */
/*
 * [한국어]
 * cond_unmask_irq - 조건이 맞을 때만 인터럽트를 다시 연다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 레벨 트리거 처리의 마지막 단계다. 처리를 시작할 때 막았으므로
 * 끝나면 열어야 하는데, 열면 안 되는 경우가 두 가지 있다.
 *
 * (1) 인터럽트가 논리적으로 꺼졌다: 핸들러 실행 중에 disable_irq() 가
 *     불렸을 수 있다. 그러면 열면 안 된다.
 * (2) 스레드가 깨어났다 (threads_oneshot 이 0 이 아니다): IRQF_ONESHOT
 *     인터럽트는 스레드 작업이 끝날 때까지 막혀 있어야 한다. 여는 것은
 *     나중에 unmask_threaded_irq() 가 한다.
 *
 * 원본 주석이 세 가지 여는 경우를 든다. 일반 레벨 인터럽트, 그리고
 * oneshot 이지만 스레드를 깨우지 않은 두 경우 — 오탐이었거나 1 차
 * 핸들러가 혼자 다 처리한 경우다.
 *
 * MASKED 를 확인하는 이유: 마스크 기능이 없는 칩에서는 애초에 막히지
 * 않았으므로 열 것도 없다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_level_irq() → [이 함수] → unmask_irq()
 */
static void cond_unmask_irq(struct irq_desc *desc)
{
	/*
	 * We need to unmask in the following cases:
	 * - Standard level irq (IRQF_ONESHOT is not set)
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&	/* [한국어] (위 영어 주석) 핸들러 실행 중에 disable_irq 가 불리지 않았는가 */
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot)	/* [한국어] 실제로 막혀 있고, oneshot 스레드가 깨어나지 않았는가. 깨어났다면 그 스레드가 끝날 때 열린다 */
		unmask_irq(desc);	/* [한국어] 다시 연다. 다음 인터럽트를 받을 준비 */
}

/**
 * handle_level_irq - Level type irq handler
 * @desc:	the interrupt description structure for this irq
 *
 * Level type interrupts are active as long as the hardware line has the
 * active level. This may require to mask the interrupt and unmask it after
 * the associated handler has acknowledged the device, so the interrupt
 * line is back to inactive.
 */
/*
 * [한국어]
 * handle_level_irq - 레벨 트리거 인터럽트를 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 레벨 트리거란: 장치가 인터럽트 선을 활성 레벨로 계속 유지하는
 * 방식이다. 원인이 해소될 때까지 신호가 사라지지 않는다.
 *
 * 그래서 처리 순서가 정해진다. 먼저 막지 않으면 핸들러가 실행되는
 * 동안 같은 인터럽트가 CPU 를 계속 때린다 — 선이 여전히 활성이기
 * 때문이다. 그래서 첫 줄이 mask_ack_irq() 다.
 *
 * 핸들러가 장치의 원인을 해소하면 선이 비활성으로 돌아간다. 그때
 * 다시 열면 된다. 그것이 마지막 cond_unmask_irq() 다.
 *
 * 이 처리기가 에지 트리거보다 단순한 이유: 막혀 있는 동안 온 인터럽트를
 * 따로 챙길 필요가 없다. 선이 계속 활성이므로 열자마자 다시 올라온다.
 * 에지 트리거는 그 사이의 신호가 사라지므로 루프로 챙겨야 한다.
 *
 * 마스크를 먼저 하고 나서 처리 가능 여부를 확인하는 순서에 주목:
 * 처리하지 못하는 경우에도 막아 두어야 인터럽트 폭주를 막는다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   generic_handle_irq() → desc->handle_irq → [이 함수] →
 *   mask_ack_irq() → handle_irq_event() → cond_unmask_irq()
 */
void handle_level_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안 */
	mask_ack_irq(desc);	/* [한국어] 무조건 먼저 막는다. 레벨 신호는 사라지지 않아, 막지 않으면 핸들러 실행 중에 CPU 를 계속 때린다 */

	if (!irq_can_handle(desc))	/* [한국어] 처리 가능한가. 막은 뒤에 확인하는 것이 중요하다 — 처리 못 해도 막혀 있어야 폭주하지 않는다 */
		return;

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수 */
	handle_irq_event(desc);	/* [한국어] 핸들러 실행. 이 안에서 장치의 인터럽트 원인이 해소되어 선이 비활성으로 돌아간다 */

	cond_unmask_irq(desc);	/* [한국어] 조건이 맞으면 다시 연다. oneshot 스레드가 깨어났다면 그 스레드가 끝날 때 열린다 */
}
EXPORT_SYMBOL_GPL(handle_level_irq);	/* [한국어] 레벨 트리거 컨트롤러 드라이버가 건다 */

/*
 * [한국어]
 * cond_unmask_eoi_irq - EOI 를 보내고 조건에 따라 언마스크한다
 *
 * @desc: 대상 서술자
 * @chip: 이 인터럽트의 칩
 * @return: 없음
 *
 * fasteoi 처리기의 마무리다. 위 cond_unmask_irq() 에 EOI 를 언제
 * 보낼지가 얽혀 세 갈래가 된다.
 *
 * (1) oneshot 이 아닌 평범한 경우: EOI 만 보내고 끝. 마스크한 적이
 *     없으므로 열 것도 없다. fasteoi 의 원래 모습이다.
 * (2) oneshot 인데 스레드가 깨어나지 않은 경우: EOI 를 보내고 언마스크도
 *     한다. 처리를 시작할 때 막아 두었기 때문이다.
 * (3) oneshot 이고 스레드가 깨어난 경우: 아직 열면 안 된다. EOI 는
 *     보내되, IRQCHIP_EOI_THREADED 칩이면 그것마저 미룬다 —
 *     unmask_threaded_irq() 가 스레드 종료 시 보낸다.
 *
 * (3) 에서 EOI 를 미루는 이유: EOI 를 보내면 컨트롤러가 같은 우선순위의
 * 다음 인터럽트를 올릴 수 있다. 스레드가 아직 장치를 다루는 중인데
 * 그러면 곤란한 칩이 있다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_fasteoi_irq() / handle_fasteoi_ack_irq() / handle_fasteoi_mask_irq()
 *   → [이 함수] → chip->irq_eoi() / unmask_irq()
 */
static void cond_unmask_eoi_irq(struct irq_desc *desc, struct irq_chip *chip)
{
	if (!(desc->istate & IRQS_ONESHOT)) {	/* [한국어] 평범한 fasteoi 인가 — 처리 중 마스크한 적이 없다 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] EOI 만 보내면 끝. 이것이 fasteoi 의 원래 모습이다 */
		return;
	}
	/*
	 * We need to unmask in the following cases:
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&	/* [한국어] (위 영어 주석) 처리 중에 꺼지지 않았고 */
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot) {	/* [한국어] 막혀 있으며 스레드가 깨어나지 않았는가 — 오탐이거나 1 차 핸들러가 다 처리한 경우다 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] EOI 를 보내고 */
		unmask_irq(desc);	/* [한국어] 선도 다시 연다 */
	} else if (!(chip->flags & IRQCHIP_EOI_THREADED)) {	/* [한국어] 스레드가 깨어났다 — 아직 열면 안 된다. EOI 를 스레드까지 미루는 칩인가 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] 미루지 않는 칩이면 지금 보낸다. 미루는 칩이면 unmask_threaded_irq 가 나중에 보낸다 */
	}
}

/*
 * [한국어]
 * cond_eoi_irq - 처리하지 못한 인터럽트에 EOI 를 보낼지 결정한다
 *
 * @chip: 이 인터럽트의 칩
 * @data: irq_data
 * @return: 없음
 *
 * 인터럽트를 처리하지 못하고 물러날 때 부른다. 이때 EOI 를 보내야
 * 하는가가 칩마다 다르다.
 *
 * IRQCHIP_EOI_IF_HANDLED 를 세운 칩은 "처리한 경우에만 EOI" 를
 * 요구한다. 처리하지 않았는데 EOI 를 보내면 컨트롤러가 그 인터럽트를
 * 완료된 것으로 보고 잊어버려, 다시 올라오지 않는다. 그런 칩에서는
 * EOI 를 보내지 않는 것이 곧 "나중에 다시 올려 달라" 는 뜻이 된다.
 *
 * 그 플래그가 없는 칩은 EOI 를 보내야 우선순위 걸쇠가 풀린다. 안 보내면
 * 같거나 낮은 우선순위의 인터럽트가 영영 올라오지 못한다.
 *
 * 즉 두 갈래가 정반대의 위험을 피한다 — 한쪽은 인터럽트 유실, 다른
 * 쪽은 인터럽트 정지다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_fasteoi_irq() / handle_fasteoi_ack_irq() / handle_fasteoi_mask_irq()
 *   의 조기 반환 경로 → [이 함수]
 */
static inline void cond_eoi_irq(struct irq_chip *chip, struct irq_data *data)
{
	if (!(chip->flags & IRQCHIP_EOI_IF_HANDLED))	/* [한국어] "처리했을 때만 EOI" 를 요구하는 칩이 아닌가 */
		chip->irq_eoi(data);	/* [한국어] 보낸다. 안 보내면 우선순위 걸쇠가 풀리지 않아 인터럽트가 정지한다. 반대로 IF_HANDLED 칩에 보내면 그 인터럽트를 영영 잃는다 */
}

/**
 * handle_fasteoi_irq - irq handler for transparent controllers
 * @desc:	the interrupt description structure for this irq
 *
 * Only a single callback will be issued to the chip: an ->eoi() call when
 * the interrupt has been serviced. This enables support for modern forms
 * of interrupt handlers, which handle the flow details in hardware,
 * transparently.
 */
/*
 * [한국어]
 * handle_fasteoi_irq - 흐름을 하드웨어가 관리하는 컨트롤러용 처리기
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * ARM GIC 나 x86 APIC 처럼 흐름 제어를 하드웨어가 알아서 하는 현대적
 * 컨트롤러를 위한 처리기다. 요즘 시스템에서 가장 많이 쓰인다.
 *
 * 왜 마스크·언마스크가 필요 없는가: 이런 컨트롤러는 인터럽트를 CPU 에
 * 올릴 때 그 우선순위를 "진행 중" 으로 걸어 둔다. 그동안 같은
 * 인터럽트는 다시 올라오지 않는다. 그러니 소프트웨어가 막을 이유가
 * 없고, 처리가 끝났다는 신호(EOI) 하나만 보내면 된다. 그래서 fast 다.
 *
 * 그런데 실제 코드는 그리 짧지 않은데, 세 가지 예외가 얽혀서다.
 *
 * (1) IRQS_ONESHOT: 스레드 핸들러를 쓰는 인터럽트는 스레드가 끝날
 *     때까지 막아 두어야 한다. 하드웨어의 우선순위 걸쇠는 EOI 로
 *     풀리는데, EOI 는 인터럽트 문맥에서 나가기 때문이다.
 * (2) 처리 불가 시 마스크: 핸들러가 없거나 꺼진 인터럽트가 계속
 *     올라오는 것을 막는다.
 * (3) 친화도 이동 경쟁: 위 원본 주석이 말하는 상황이다. 친화도가
 *     새 CPU 로 옮겨졌는데 옛 CPU 가 아직 처리 중이면, 새로 온
 *     인터럽트를 놓칠 수 있다. IRQS_PENDING 으로 표시해 두었다가
 *     마지막에 소프트웨어로 재전송한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() → desc->handle_irq → [이 함수] →
 *   handle_irq_event() → cond_unmask_eoi_irq()
 */
void handle_fasteoi_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;	/* [한국어] EOI 콜백과 플래그를 볼 칩. 아래에서 여러 번 쓰므로 미리 잡는다 */

	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안 */

	/*
	 * When an affinity change races with IRQ handling, the next interrupt
	 * can arrive on the new CPU before the original CPU has completed
	 * handling the previous one - it may need to be resent.
	 */
	if (!irq_can_handle_pm(desc)) {	/* [한국어] (위 영어 주석) 절전 해제이거나 다른 CPU 가 처리 중인가 */
		if (irqd_needs_resend_when_in_progress(&desc->irq_data))	/* [한국어] 놓친 인터럽트를 소프트웨어로 살려야 하는 칩인가 */
			desc->istate |= IRQS_PENDING;	/* [한국어] 표시해 두었다가 아래 마지막 줄에서 재전송한다 */
		cond_eoi_irq(chip, &desc->irq_data);	/* [한국어] 처리하지 않았지만 칩에 따라 EOI 는 보내야 할 수 있다 */
		return;
	}

	if (!irq_can_handle_actions(desc)) {	/* [한국어] 핸들러가 없거나 꺼져 있는가 */
		mask_irq(desc);	/* [한국어] 막는다. 처리할 수 없는 인터럽트가 계속 올라오면 CPU 를 잡아먹는다 */
		cond_eoi_irq(chip, &desc->irq_data);	/* [한국어] 칩에 따라 EOI */
		return;
	}

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수 */
	if (desc->istate & IRQS_ONESHOT)	/* [한국어] 스레드 핸들러를 쓰는 인터럽트인가 */
		mask_irq(desc);	/* [한국어] 스레드가 끝날 때까지 막아 둔다. EOI 는 인터럽트 문맥에서 나가므로 하드웨어 걸쇠만으로는 부족하다 */

	handle_irq_event(desc);	/* [한국어] 핸들러 실행. oneshot 이면 여기서 스레드를 깨우고 돌아온다 */

	cond_unmask_eoi_irq(desc, chip);	/* [한국어] EOI 와 언마스크를 세 갈래로 나눠 처리한다 */

	/*
	 * When the race described above happens this will resend the interrupt.
	 */
	if (unlikely(desc->istate & IRQS_PENDING))	/* [한국어] (위 영어 주석) 위에서 표시해 둔 놓친 인터럽트가 있는가 */
		check_irq_resend(desc, false);	/* [한국어] 소프트웨어로 다시 올린다. 하드웨어가 다시 올려 주지 않는 경쟁 상황을 구제한다 */
}
EXPORT_SYMBOL_GPL(handle_fasteoi_irq);	/* [한국어] GIC 등 현대적 컨트롤러 드라이버가 건다 */

/**
 *	handle_fasteoi_nmi - irq handler for NMI interrupt lines
 *	@desc:	the interrupt description structure for this irq
 *
 *	A simple NMI-safe handler, considering the restrictions
 *	from request_nmi.
 *
 *	Only a single callback will be issued to the chip: an ->eoi()
 *	call when the interrupt has been serviced. This enables support
 *	for modern forms of interrupt handlers, which handle the flow
 *	details in hardware, transparently.
 */
/*
 * [한국어]
 * handle_fasteoi_nmi - NMI 선을 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * NMI(마스크 불가 인터럽트)는 워치독처럼 시스템이 멈춰도 살아 있어야
 * 하는 감시 기능에 쓰인다. 그래서 처리 경로에 락이 있으면 안 된다 —
 * 그 락을 쥔 채 멈춘 CPU 를 감시하려는 것인데, 같은 락을 기다리면
 * NMI 도 함께 멈춘다.
 *
 * 그래서 이 함수에는 desc->lock 이 없다. 대신 request_nmi() 가 여러
 * 제약을 강제한다 — 공유 불가(action 이 하나뿐), 스레드 불가, 자동
 * 활성화 불가 등이다. 그 제약 덕분에 락 없이도 안전하다.
 *
 * 그 제약이 코드에 그대로 드러난다. action 목록을 순회하지 않고
 * desc->action 하나만 쓴다. 원본 주석이 그 이유를 짚는다.
 *
 * __kstat_incr_irqs_this_cpu 를 쓰는 것에 주목: 밑줄 없는 판은
 * desc->tot_count 까지 올리는데, 그것은 여러 CPU 가 공유하는 변수라
 * NMI 문맥에서 만지면 위험하다. 밑줄 있는 판은 per-CPU 카운터만 올린다.
 *
 * 오탐 검출도 엔트로피 수집도 하지 않는다. 둘 다 락이나 공유 상태를
 * 건드리기 때문이다.
 *
 * 실행 컨텍스트: NMI 문맥. 락도 잠도 불가능하다.
 *
 * 호출 체인:
 *   generic_handle_domain_nmi() → desc->handle_irq → [이 함수]
 */
void handle_fasteoi_nmi(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] EOI 를 보낼 칩 */
	struct irqaction *action = desc->action;	/* [한국어] NMI 는 공유되지 않아 이 하나가 전부다 */
	unsigned int irq = irq_desc_get_irq(desc);	/* [한국어] 트레이스와 핸들러에 넘길 번호 */
	irqreturn_t res;	/* [한국어] 핸들러 반환값. 트레이스에만 쓴다 — 오탐 검출을 하지 않으므로 */

	__kstat_incr_irqs_this_cpu(desc);	/* [한국어] 밑줄 있는 판이다. 밑줄 없는 판은 공유 변수 tot_count 까지 올리는데 NMI 문맥에서는 위험하다 */

	trace_irq_handler_entry(irq, action);	/* [한국어] ftrace 진입 표시. NMI 안전한 링 버퍼를 쓴다 */
	/*
	 * NMIs cannot be shared, there is only one action.
	 */
	res = action->handler(irq, action->dev_id);	/* [한국어] (위 영어 주석) 목록을 순회하지 않는다. request_nmi 가 공유를 막아 하나뿐임이 보장된다 */
	trace_irq_handler_exit(irq, action, res);	/* [한국어] ftrace 종료 표시 */

	if (chip->irq_eoi)	/* [한국어] EOI 가 필요한 칩인가 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] 우선순위 걸쇠를 푼다. 락 없이 부르는 것이라 이 콜백도 NMI 안전해야 한다 */
}
EXPORT_SYMBOL_GPL(handle_fasteoi_nmi);	/* [한국어] NMI 를 지원하는 컨트롤러 드라이버가 건다 */

/**
 * handle_edge_irq - edge type IRQ handler
 * @desc:	the interrupt description structure for this irq
 *
 * Interrupt occurs on the falling and/or rising edge of a hardware
 * signal. The occurrence is latched into the irq controller hardware and
 * must be acked in order to be reenabled. After the ack another interrupt
 * can happen on the same source even before the first one is handled by
 * the associated event handler. If this happens it might be necessary to
 * disable (mask) the interrupt depending on the controller hardware. This
 * requires to reenable the interrupt inside of the loop which handles the
 * interrupts which have arrived while the handler was running. If all
 * pending interrupts are handled, the loop is left.
 */
/*
 * [한국어]
 * handle_edge_irq - 에지 트리거 인터럽트를 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 에지 트리거란: 신호의 상승 또는 하강 순간에만 인터럽트가 발생하는
 * 방식이다. 그 "순간" 을 컨트롤러가 래치에 붙들어 두고, ack 로 지워야
 * 다음 신호를 받을 수 있다.
 *
 * 이 방식의 근본적인 어려움은 신호가 사라진다는 것이다. 레벨 트리거는
 * 막아 두었다가 나중에 열면 인터럽트가 다시 올라오지만, 에지는 그
 * 사이의 신호가 영영 사라진다. 그래서 처리 중에 들어온 것을 놓치지
 * 않으려는 장치가 필요하다.
 *
 * 그 장치가 do-while 루프와 IRQS_PENDING 이다. 흐름은 이렇다.
 *
 *   1. ack 로 래치를 먼저 지운다 — 처리 중에 오는 새 신호를 받으려면
 *      래치가 비어 있어야 한다.
 *   2. 핸들러를 부른다. 이때 락이 잠시 풀린다.
 *   3. 그 사이에 새 인터럽트가 오면, 다른 CPU 의 이 함수가 INPROGRESS 를
 *      보고 IRQS_PENDING 만 세우고 물러난다 (그리고 선을 막는다).
 *   4. 루프 조건에서 PENDING 을 보고 다시 돈다. 막힌 선을 다시 열고
 *      핸들러를 또 부른다.
 *
 * 3 에서 선을 막는 이유: 그러지 않으면 인터럽트가 계속 올라와 CPU 가
 * 처리에 매몰된다. 4 에서 다시 여는 것이 짝이다.
 *
 * 루프 중간의 !desc->action 검사: 핸들러 실행 중에 free_irq() 가
 * 불렸을 수 있다. 그러면 막고 나간다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   generic_handle_irq() → desc->handle_irq → [이 함수] →
 *   chip->irq_ack() → handle_irq_event() (루프)
 */
void handle_edge_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안. 다만 handle_irq_event 안에서 잠시 풀린다 */

	if (!irq_can_handle(desc)) {	/* [한국어] 처리할 수 없는 상태인가 */
		desc->istate |= IRQS_PENDING;	/* [한국어] 다른 CPU 가 처리 중이라면 그쪽 루프가 이 표시를 보고 이어서 처리한다. 이것이 위 3 번 단계다 */
		mask_ack_irq(desc);	/* [한국어] 막고 래치도 지운다. 막지 않으면 인터럽트가 계속 올라와 CPU 가 매몰된다 */
		return;
	}

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수 */

	/* Start handling the irq */
	desc->irq_data.chip->irq_ack(&desc->irq_data);	/* [한국어] (위 영어 주석) 래치를 먼저 지운다. 처리 중에 오는 새 신호를 받으려면 래치가 비어 있어야 한다 */

	do {
		if (unlikely(!desc->action)) {	/* [한국어] 핸들러 실행 중에 free_irq 가 불렸는가 */
			mask_irq(desc);	/* [한국어] 받을 사람이 없으니 막고 나간다 */
			return;
		}

		/*
		 * When another irq arrived while we were handling
		 * one, we could have masked the irq.
		 * Reenable it, if it was not disabled in meantime.
		 */
		if (unlikely(desc->istate & IRQS_PENDING)) {	/* [한국어] (위 영어 주석) 처리 중에 새 인터럽트가 왔는가 — 그쪽이 선을 막아 두었다 */
			if (!irqd_irq_disabled(&desc->irq_data) &&	/* [한국어] 그 사이에 꺼지지 않았고 */
			    irqd_irq_masked(&desc->irq_data))	/* [한국어] 실제로 막혀 있는가 */
				unmask_irq(desc);	/* [한국어] 다시 연다. 위 3 번에서 막은 것의 짝이다 */
		}

		handle_irq_event(desc);	/* [한국어] 핸들러 실행. 이 안에서 PENDING 이 지워지고, 락이 잠시 풀리는 동안 다시 세워질 수 있다 */

	} while ((desc->istate & IRQS_PENDING) && !irqd_irq_disabled(&desc->irq_data));	/* [한국어] 처리 중에 또 왔고 아직 켜져 있으면 다시 돈다. 에지 신호는 사라지므로 이 루프가 없으면 인터럽트를 잃는다 */
}
EXPORT_SYMBOL(handle_edge_irq);	/* [한국어] GPL 판이 아닌 것은 역사적 이유다 */

/**
 *	handle_percpu_irq - Per CPU local irq handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Per CPU interrupts on SMP machines without locking requirements
 */
/*
 * [한국어]
 * handle_percpu_irq - CPU 지역 인터럽트를 처리한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 이 파일에서 가장 짧은 흐름 처리기다. 락이 없고, 상태 검사도 없고,
 * 마스크·언마스크도 없다.
 *
 * 왜 그럴 수 있는가: per-CPU 인터럽트는 번호를 공유하지만 실제로는
 * CPU 마다 별개의 인터럽트다. ARM 의 지역 타이머처럼 각 CPU 가 자기
 * 것만 받는다. 그러니 두 CPU 가 같은 인터럽트를 동시에 처리하는 일이
 * 없고, 직렬화할 이유가 없다.
 *
 * 락이 없다는 것이 이 처리기의 존재 이유이기도 하다. 타이머 인터럽트는
 * 매우 자주 발생하므로 락 획득 비용이 그대로 시스템 부담이 된다.
 *
 * 원본 주석이 tot_count 를 만지지 말라고 경고한다. 그것은 여러 CPU 가
 * 공유하는 변수라, 락 없이 올리면 값이 어긋난다. 그래서 밑줄 있는
 * __kstat_incr_irqs_this_cpu 를 쓴다 — per-CPU 카운터만 올린다.
 * kstat_irqs_desc() 가 per-CPU 인터럽트에 대해 CPU 별 카운터를 직접
 * 더하는 이유가 바로 이것이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() → desc->handle_irq → [이 함수] →
 *   handle_irq_event_percpu()
 */
void handle_percpu_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] ack/eoi 콜백을 볼 칩 */

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);	/* [한국어] (위 영어 주석) 밑줄 있는 판. 공유 변수 tot_count 를 건드리지 않아 락 없이 안전하다 */

	if (chip->irq_ack)	/* [한국어] 래치를 지워야 하는 칩인가 */
		chip->irq_ack(&desc->irq_data);	/* [한국어] 지운다. 이 CPU 의 레지스터가 자동으로 선택된다 */

	handle_irq_event_percpu(desc);	/* [한국어] 핸들러 실행. INPROGRESS 를 세우지 않는 판이다 — 직렬화가 필요 없으므로 */

	if (chip->irq_eoi)	/* [한국어] EOI 가 필요한 칩인가 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] 우선순위 걸쇠를 푼다 */
}

/**
 * handle_percpu_devid_irq - Per CPU local irq handler with per cpu dev ids
 * @desc:	the interrupt description structure for this irq
 *
 * Per CPU interrupts on SMP machines without locking requirements. Same as
 * handle_percpu_irq() above but with the following extras:
 *
 * action->percpu_dev_id is a pointer to percpu variables which
 * contain the real device id for the cpu on which this handler is
 * called
 */
/*
 * [한국어]
 * handle_percpu_devid_irq - CPU 별 장치 ID 를 쓰는 per-CPU 인터럽트 처리기
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 handle_percpu_irq() 와 같지만 핸들러에 넘기는 dev_id 가 다르다.
 *
 * 무엇이 문제인가: 지역 타이머 같은 per-CPU 장치는 CPU 마다 자기
 * 상태 구조체를 갖는다. 핸들러가 "지금 이 CPU 의 타이머" 를 찾아야
 * 하는데, 일반 인터럽트처럼 dev_id 하나를 넘기면 CPU 를 구분할 수
 * 없다. 그래서 percpu_dev_id 라는 per-CPU 포인터를 두고, 실행 중인
 * CPU 의 것을 raw_cpu_ptr 로 뽑아 넘긴다.
 *
 * 핸들러를 고르는 루프가 또 하나의 차이다. 한 번호에 여러 action 이
 * 붙을 수 있는데, 각자 담당 CPU 가 정해져 있다. 그래서 지금 CPU 를
 * 담당하는 것을 찾는다. 이것은 공유 인터럽트가 아니다 — 한 CPU 에
 * 대해서는 여전히 하나뿐이다.
 *
 * 담당 핸들러를 못 찾은 경우가 오탐이다. 그 CPU 에서는 아무도 이
 * 인터럽트를 기다리지 않는데 올라온 것이다. 그러면 그 CPU 에서만
 * 인터럽트를 끄고 한 번 경고한다. 시스템 전체를 끄지 않는 것이
 * per-CPU 인터럽트다운 처리다.
 *
 * add_interrupt_randomness() 를 부르는 것에 주목: 위 handle_percpu_irq
 * 는 이것을 하지 않는다. handle_irq_event_percpu() 안에 들어 있기
 * 때문인데, 여기서는 그 함수를 쓰지 않아 직접 부른다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() → desc->handle_irq → [이 함수] →
 *   action->handler()
 */
void handle_percpu_devid_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* [한국어] ack/eoi 콜백을 볼 칩 */
	unsigned int irq = irq_desc_get_irq(desc);	/* [한국어] 핸들러와 트레이스에 넘길 번호 */
	unsigned int cpu = smp_processor_id();	/* [한국어] 지금 실행 중인 CPU. 담당 핸들러를 고르는 기준이다 */
	struct irqaction *action;	/* [한국어] 이 CPU 를 담당하는 핸들러 */
	irqreturn_t res;	/* [한국어] 핸들러 반환값. 트레이스에만 쓴다 */

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);	/* [한국어] (위 영어 주석) 공유 변수를 건드리지 않는 판 */

	if (chip->irq_ack)	/* [한국어] 래치를 지워야 하는 칩인가 */
		chip->irq_ack(&desc->irq_data);	/* [한국어] 지운다 */

	for (action = desc->action; action; action = action->next)	/* [한국어] 등록된 핸들러들을 훑는다 */
		if (cpumask_test_cpu(cpu, action->affinity))	/* [한국어] 이 CPU 를 담당하는 것인가. 공유 인터럽트가 아니라 CPU 별 분담이다 */
			break;	/* [한국어] 찾았다. 한 CPU 에 대해서는 하나뿐이다 */

	if (likely(action)) {	/* [한국어] 담당 핸들러를 찾았는가 */
		trace_irq_handler_entry(irq, action);	/* [한국어] ftrace 진입 */
		res = action->handler(irq, raw_cpu_ptr(action->percpu_dev_id));	/* [한국어] 이 CPU 의 장치 상태 포인터를 뽑아 넘긴다. 이것이 이 처리기가 따로 있는 이유다 */
		trace_irq_handler_exit(irq, action, res);	/* [한국어] ftrace 종료 */
	} else {	/* [한국어] 이 CPU 에서는 아무도 기다리지 않는 인터럽트가 왔다 — 오탐이다 */
		bool enabled = cpumask_test_cpu(cpu, desc->percpu_enabled);	/* [한국어] 이 CPU 에서 켜져 있던 인터럽트인가. 경고 문구를 정확히 하려는 것이다 */

		if (enabled)	/* [한국어] 켜져 있었다면 */
			irq_percpu_disable(desc, cpu);	/* [한국어] 이 CPU 에서만 끈다. 다른 CPU 는 정상 동작을 계속한다 — per-CPU 인터럽트다운 처리다 */

		pr_err_once("Spurious%s percpu IRQ%u on CPU%u\n",	/* [한국어] 한 번만 알린다. 오탐이 반복되면 로그가 넘친다 */
			    enabled ? " and unmasked" : "", irq, cpu);
	}

	add_interrupt_randomness(irq);	/* [한국어] 도착 시각을 엔트로피 원으로 쓴다. handle_percpu_irq 는 handle_irq_event_percpu 안에서 하지만 여기서는 그 함수를 쓰지 않아 직접 부른다 */

	if (chip->irq_eoi)	/* [한국어] EOI 가 필요한 칩인가 */
		chip->irq_eoi(&desc->irq_data);	/* [한국어] 우선순위 걸쇠를 푼다 */
}

/*
 * [한국어]
 * __irq_do_set_handler - 서술자에 흐름 처리기를 설치하거나 떼어 낸다
 *
 * @desc:       대상 서술자
 * @handle:     설치할 흐름 처리기. NULL 이면 handle_bad_irq 로 바뀐다.
 * @is_chained: 체인 인터럽트인가 (설치 후 곧바로 시작한다)
 * @name:       /proc/interrupts 에 보일 이름
 * @return:     없음
 *
 * 이 파일에서 가장 갈래가 많은 함수다. 설치·제거·체인 설정이라는 세
 * 가지 일을 하나에 담고 있다.
 *
 * 첫 블록 — 칩 확인: 흐름 처리기는 칩 콜백을 부르므로 칩이 있어야
 * 한다. 계층형 도메인에서는 바깥 칩이 아직 준비되지 않았는데 안쪽은
 * 준비된 어중간한 상태가 있을 수 있다. 원본 주석대로 그 경우에는
 * 설치는 하되 시작하지 않는다 — 부모를 거슬러 올라가며 하나라도
 * 실제 칩이 있으면 통과시킨다.
 *
 * 다만 체인이라면 곧바로 시작해야 하므로 어중간한 상태를 허용할 수
 * 없다. 그래서 WARN_ON(is_chained) 로 거절한다.
 *
 * 둘째 블록 — 제거: handle_bad_irq 를 설치한다는 것은 사실상 제거다.
 * 하드웨어를 막고 꺼진 상태로 되돌린다. 체인이었다면 가짜 action 을
 * 떼고 PM 참조도 놓는다.
 *
 * 셋째 블록 — 체인 설정: 체인 인터럽트는 즉시 동작해야 하므로 여기서
 * 시작까지 한다. 그 전에 세 가지 설정 플래그를 세우는데, 전부
 * "일반 드라이버가 이 인터럽트를 건드리지 못하게" 하는 것이다.
 *
 * 트리거 재설정의 미묘함: __irq_set_trigger() 안에서 칩의 irq_set_type
 * 콜백이 흐름 처리기를 자기 마음대로 바꿀 수 있다. 그 콜백은 체인
 * 상황을 모르므로, 바꾼 것을 여기서 되돌린다. 원본 주석의 "we do know
 * better" 가 그 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 과 버스 락 보유.
 *
 * 호출 체인:
 *   __irq_set_handler() / irq_set_chained_handler_and_data() →
 *   [이 함수] → irq_activate_and_startup()
 */
static void
__irq_do_set_handler(struct irq_desc *desc, irq_flow_handler_t handle,
		     int is_chained, const char *name)
{
	if (!handle) {	/* [한국어] 처리기를 떼어 내라는 요청인가 */
		handle = handle_bad_irq;	/* [한국어] NULL 을 두면 인터럽트가 올 때 NULL 호출이 된다. 경고를 찍는 함수로 대신한다 */
	} else {	/* [한국어] 실제 처리기를 설치하는 경우 — 칩이 준비됐는지 확인해야 한다 */
		struct irq_data *irq_data = &desc->irq_data;	/* [한국어] 계층을 거슬러 올라갈 커서 */
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형 도메인이 있는 빌드에서만 부모를 따라갈 수 있다 */
		/*
		 * With hierarchical domains we might run into a
		 * situation where the outermost chip is not yet set
		 * up, but the inner chips are there.  Instead of
		 * bailing we install the handler, but obviously we
		 * cannot enable/startup the interrupt at this point.
		 */
		while (irq_data) {	/* [한국어] (위 영어 주석) 계층을 거슬러 올라가며 실제 칩을 찾는다 */
			if (irq_data->chip != &no_irq_chip)	/* [한국어] 더미가 아닌 진짜 칩인가 */
				break;	/* [한국어] 찾았다. 설치해도 된다 */
			/*
			 * Bail out if the outer chip is not set up
			 * and the interrupt supposed to be started
			 * right away.
			 */
			if (WARN_ON(is_chained))	/* [한국어] (위 영어 주석) 체인은 즉시 시작해야 하므로 어중간한 상태를 허용할 수 없다 */
				return;	/* [한국어] 거절한다 */
			/* Try the parent */
			irq_data = irq_data->parent_data;	/* [한국어] (위 영어 주석) 한 층 위로. 안쪽 칩만 준비된 경우를 구제한다 */
		}
#endif
		if (WARN_ON(!irq_data || irq_data->chip == &no_irq_chip))	/* [한국어] 계층 전체에 진짜 칩이 하나도 없는가 — 설치해도 동작할 수 없다 */
			return;	/* [한국어] 거절한다 */
	}

	/* Uninstall? */
	if (handle == handle_bad_irq) {	/* [한국어] (위 영어 주석) 떼어 내는 경우 */
		if (desc->irq_data.chip != &no_irq_chip)	/* [한국어] 칩이 아직 붙어 있는가 */
			mask_ack_irq(desc);	/* [한국어] 막고 래치도 지운다. 처리기가 없는 상태에서 인터럽트가 올라오면 안 된다 */
		irq_state_set_disabled(desc);	/* [한국어] 꺼진 상태로 기록 */
		if (is_chained) {	/* [한국어] 체인이었던 인터럽트인가 */
			desc->action = NULL;	/* [한국어] 가짜 action(chained_action)을 뗀다 */
			irq_chip_pm_put(irq_desc_get_irq_data(desc));	/* [한국어] 체인 설정 때 잡은 PM 참조를 놓는다. 짝이 맞아야 컨트롤러가 절전에 들어갈 수 있다 */
		}
		desc->depth = 1;	/* [한국어] 다시 쓰려면 명시적으로 열어야 하는 상태로 */
	}
	desc->handle_irq = handle;	/* [한국어] 실제 설치. 이 줄 이후로 인터럽트가 새 처리기로 들어간다 */
	desc->name = name;	/* [한국어] /proc/interrupts 와 /sys 에 보일 이름 */

	if (handle != handle_bad_irq && is_chained) {	/* [한국어] 체인 처리기를 설치하는 경우 — 즉시 시작까지 해야 한다 */
		unsigned int type = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 설정된 트리거 방식 */

		/*
		 * We're about to start this interrupt immediately,
		 * hence the need to set the trigger configuration.
		 * But the .set_type callback may have overridden the
		 * flow handler, ignoring that we're dealing with a
		 * chained interrupt. Reset it immediately because we
		 * do know better.
		 */
		if (type != IRQ_TYPE_NONE) {	/* [한국어] (위 영어 주석) 트리거 방식이 정해져 있는가 */
			__irq_set_trigger(desc, type);	/* [한국어] 하드웨어에 반영한다. 즉시 시작할 것이므로 미룰 수 없다 */
			desc->handle_irq = handle;	/* [한국어] 칩의 set_type 콜백이 처리기를 바꿨을 수 있다. 그 콜백은 체인 상황을 모르므로 여기서 되돌린다 */
		}

		irq_settings_set_noprobe(desc);	/* [한국어] 자동 탐색 대상에서 뺀다. 체인 선을 탐색하면 자식들이 엉망이 된다 */
		irq_settings_set_norequest(desc);	/* [한국어] 일반 드라이버가 request_irq 하지 못하게 막는다 */
		irq_settings_set_nothread(desc);	/* [한국어] 스레드 처리를 막는다. 체인 처리기는 하드 인터럽트 문맥이어야 한다 */
		desc->action = &chained_action;	/* [한국어] 가짜 action. desc->action 이 NULL 인지로 판단하는 코어 검사들을 통과시키기 위한 것이다 */
		WARN_ON(irq_chip_pm_get(irq_desc_get_irq_data(desc)));	/* [한국어] 컨트롤러의 전원을 켠 채로 유지한다. 체인 선은 항상 살아 있어야 자식들이 동작한다 */
		irq_activate_and_startup(desc, IRQ_RESEND);	/* [한국어] 즉시 시작. 일반 인터럽트는 request_irq 때 시작하지만 체인은 여기서 한다 */
	}
}

/*
 * [한국어]
 * __irq_set_handler - 번호로 서술자를 찾아 흐름 처리기를 설치한다
 *
 * @irq:        대상 인터럽트 번호
 * @handle:     설치할 흐름 처리기
 * @is_chained: 체인 인터럽트인가
 * @name:       표시 이름
 * @return:     없음
 *
 * 위 __irq_do_set_handler() 에 서술자 조회와 잠금을 두른 껍데기다.
 * irq_set_handler() 와 irq_set_chained_handler() 매크로가 이 함수를
 * is_chained 만 달리해 부른다.
 *
 * buslock 판을 쓰는 이유: 체인 설정 경로가 __irq_set_trigger() 를
 * 부르고, 그것이 I2C/SPI 뒤의 칩에서 잠들 수 있다.
 *
 * 실패를 알리지 않는다 — 반환 타입이 void 다. 그런 인터럽트가 없으면
 * 조용히 넘어간다. 이 API 는 초기화 경로에서 불리고, 그 시점에 번호가
 * 틀렸다면 다른 곳에서 이미 문제가 드러났을 것이라는 판단이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_set_handler() / irq_set_chained_handler() 매크로 → [이 함수] →
 *   __irq_do_set_handler()
 */
void __irq_set_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		       const char *name)
{
	scoped_irqdesc_get_and_buslock(irq, 0)	/* [한국어] 서술자 조회 + 버스 락 + 스핀락. 체인 경로가 잠들 수 있어 buslock 판이 필요하다 */
		__irq_do_set_handler(scoped_irqdesc, handle, is_chained, name);	/* [한국어] 실제 설치 논리에 위임한다 */
}
EXPORT_SYMBOL_GPL(__irq_set_handler);	/* [한국어] irqchip 드라이버가 매크로를 통해 부른다 */

/*
 * [한국어]
 * irq_set_chained_handler_and_data - 체인 처리기와 사설 데이터를 함께 건다
 *
 * @irq:    대상 인터럽트 번호
 * @handle: 설치할 체인 흐름 처리기
 * @data:   그 처리기가 쓸 사설 데이터
 * @return: 없음
 *
 * 왜 둘을 묶어야 하는가: 체인 설정은 즉시 인터럽트를 시작한다. 그런데
 * 체인 처리기는 대부분 handler_data 에서 자식 도메인 정보를 꺼내
 * 쓴다. 데이터를 나중에 설정하면, 시작 직후 첫 인터럽트가 NULL 을
 * 참조하게 된다.
 *
 * 그래서 같은 락 구역 안에서 데이터를 먼저 넣고 처리기를 설치한다.
 * 순서가 이 함수의 전부라고 해도 된다.
 *
 * is_chained 에 1 을, name 에 NULL 을 넘기는 것이 고정이다. 체인
 * 전용 API 이므로 다른 값이 올 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   디먹스 컨트롤러 드라이버 probe → [이 함수] → __irq_do_set_handler()
 */
void irq_set_chained_handler_and_data(unsigned int irq, irq_flow_handler_t handle,
				      void *data)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 매크로가 만든 변수에 짧은 이름을 붙인다 */

		desc->irq_common_data.handler_data = data;	/* [한국어] 먼저 데이터를 넣는다. 아래 설치가 곧바로 인터럽트를 시작하므로 순서가 중요하다 */
		__irq_do_set_handler(desc, handle, 1, NULL);	/* [한국어] 체인으로 설치하고 즉시 시작한다. 첫 인터럽트가 위 데이터를 이미 볼 수 있다 */
	}
}
EXPORT_SYMBOL_GPL(irq_set_chained_handler_and_data);	/* [한국어] 디먹스 컨트롤러 드라이버가 부른다 */

/*
 * [한국어]
 * irq_set_chip_and_handler_name - 칩과 흐름 처리기를 함께 건다
 *
 * @irq:    대상 인터럽트 번호
 * @chip:   담당 컨트롤러
 * @handle: 흐름 처리기
 * @name:   표시 이름
 * @return: 없음
 *
 * 인터럽트를 동작 가능하게 만드는 데 필요한 두 가지를 한 번에 한다.
 * 칩만 있으면 어떻게 처리할지 모르고, 처리기만 있으면 무엇을 만질지
 * 모른다.
 *
 * 순서가 중요하다: 칩을 먼저 걸어야 한다. __irq_do_set_handler() 가
 * "칩이 준비됐는가" 를 확인하는데, 순서가 반대면 그 검사에 걸려
 * 설치가 거절된다.
 *
 * 락을 두 번 잡았다 놓는 것이 비효율적으로 보이지만, 이 함수는
 * 초기화 경로에서만 불려 문제가 되지 않는다. 그 사이에 인터럽트가
 * 올라오더라도 아직 처리기가 없어 handle_bad_irq 가 경고를 찍을 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_set_chip_and_handler() 매크로 / irq_setup_generic_chip() →
 *   [이 함수] → irq_set_chip() → __irq_set_handler()
 */
void
irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name)
{
	irq_set_chip(irq, chip);	/* [한국어] 칩을 먼저. 아래 처리기 설치가 칩 존재를 확인하므로 순서가 반대면 거절된다 */
	__irq_set_handler(irq, handle, 0, name);	/* [한국어] 체인이 아닌 일반 설치. 시작은 request_irq 때 한다 */
}
EXPORT_SYMBOL_GPL(irq_set_chip_and_handler_name);	/* [한국어] 거의 모든 irqchip 드라이버가 부른다 */

/*
 * [한국어]
 * irq_modify_status - 설정 플래그를 바꾸고 하드웨어 상태에 반영한다
 *
 * @irq: 대상 인터럽트 번호
 * @clr: 지울 IRQ_ 플래그 비트
 * @set: 세울 IRQ_ 플래그 비트
 * @return: 없음
 *
 * 세 상태 워드가 어떻게 연결되는지를 가장 잘 보여 주는 함수다.
 *
 * 설정 워드(_IRQ_ 계열)는 "이 인터럽트는 어떤 성질인가" 를 담는다.
 * 하드웨어 상태 워드(IRQD_ 계열)는 코어의 빠른 경로가 읽는다. 성질
 * 중 일부는 두 곳 모두에 나타나는데, 설정이 원본이고 IRQD 는 그것을
 * 반영한 사본이다.
 *
 * 왜 사본을 두는가: 빠른 경로가 irq_data 하나만 들고 다니기 때문이다.
 * 흐름 처리기와 칩 콜백은 서술자 전체가 아니라 irq_data 를 받으므로,
 * 자주 보는 성질은 그쪽에도 있어야 한다.
 *
 * 그래서 이 함수는 설정을 바꾼 뒤 세 가지 사본(NO_BALANCING, PER_CPU,
 * LEVEL)을 다시 계산한다. 먼저 통째로 지우고 설정에 따라 다시 세우는
 * 방식이라, clr/set 이 무엇이었든 결과가 일관된다.
 *
 * 트리거 방식 처리가 미묘하다. 현재 값을 먼저 읽어 두고, 설정 워드에
 * 트리거 정보가 있으면 그것으로 덮는다. 설정에 없으면 원래 값을
 * 유지한다 — IRQD_TRIGGER_MASK 를 지웠으므로 다시 세워 주지 않으면
 * 정보가 사라진다.
 *
 * 첫 줄의 경고: 이미 동작 중인(depth 0) 인터럽트에 "자동 활성화 금지"
 * 를 세우는 것은 앞뒤가 맞지 않는다. 이미 켜져 있는데 켜지 말라는
 * 뜻이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_status_flags() / irq_clear_status_flags() 매크로 /
 *   irq_map_generic_chip() → [이 함수]
 */
void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */
		unsigned long trigger, tmp;	/* [한국어] 트리거 방식의 현재 값과 설정 워드에서 읽은 값 */
		/*
		 * Warn when a driver sets the no autoenable flag on an already
		 * active interrupt.
		 */
		WARN_ON_ONCE(!desc->depth && (set & _IRQ_NOAUTOEN));	/* [한국어] (위 영어 주석) 이미 켜져 있는(depth 0) 인터럽트에 "자동으로 켜지 마라" 를 거는 것은 앞뒤가 맞지 않는다 */

		irq_settings_clr_and_set(desc, clr, set);	/* [한국어] 설정 워드를 갱신한다. 이것이 원본이고 아래 IRQD 는 사본이다 */

		trigger = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 현재 트리거 방식을 먼저 읽어 둔다. 아래에서 IRQD_TRIGGER_MASK 를 지우므로 미리 챙겨야 한다 */

		irqd_clear(&desc->irq_data, IRQD_NO_BALANCING | IRQD_PER_CPU |	/* [한국어] 사본 비트를 통째로 지운다. 하나씩 조건부로 고치는 것보다 결과가 일관된다 */
			   IRQD_TRIGGER_MASK | IRQD_LEVEL);
		if (irq_settings_has_no_balance_set(desc))	/* [한국어] 설정 워드에 "부하 분산 금지" 가 있는가 */
			irqd_set(&desc->irq_data, IRQD_NO_BALANCING);	/* [한국어] 사본에 반영. 친화도 자동 조정 코드가 이 비트를 본다 */
		if (irq_settings_is_per_cpu(desc))	/* [한국어] per-CPU 인터럽트인가 */
			irqd_set(&desc->irq_data, IRQD_PER_CPU);	/* [한국어] 사본에 반영 */
		if (irq_settings_is_level(desc))	/* [한국어] 레벨 트리거인가 */
			irqd_set(&desc->irq_data, IRQD_LEVEL);	/* [한국어] 사본에 반영. sysfs 의 type 파일이 이 비트를 읽는다 */

		tmp = irq_settings_get_trigger_mask(desc);	/* [한국어] 설정 워드에 트리거 정보가 있는가 */
		if (tmp != IRQ_TYPE_NONE)	/* [한국어] 있으면 */
			trigger = tmp;	/* [한국어] 그것으로 덮는다. 없으면 위에서 챙겨 둔 원래 값을 유지한다 */

		irqd_set(&desc->irq_data, trigger);	/* [한국어] 트리거 사본을 다시 세운다. 이 줄이 없으면 위에서 지운 정보가 사라진다 */
	}
}
EXPORT_SYMBOL_GPL(irq_modify_status);	/* [한국어] 드라이버가 매크로를 통해 부른다 */

#ifdef CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE	/* [한국어] 폐기 예정 기능. 아래 두 함수는 새 코드가 쓰면 안 된다 */
/**
 *	irq_cpu_online - Invoke all irq_cpu_online functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_online()
 *	for each.
 */
/*
 * [한국어]
 * irq_cpu_online - CPU 가 온라인이 될 때 모든 칩에 알린다
 *
 * @return: 없음
 *
 * 왜 폐기 예정인가: 이 방식은 CPU 하나가 올라올 때마다 시스템의 모든
 * 인터럽트를 훑는다. 인터럽트가 수천 개인 서버에서 CPU 를 여럿 올리면
 * 그 곱만큼의 시간이 든다.
 *
 * 요즘 방식은 kernel/irq/cpuhotplug.c 의 irq_affinity_online_cpu() 로,
 * 실제로 그 CPU 와 관련된 인터럽트만 다룬다. 새 드라이버는 그쪽
 * 기구를 쓰고 irq_cpu_online 콜백을 두지 않는다.
 *
 * IRQCHIP_ONOFFLINE_ENABLED 플래그의 뜻: "꺼진 인터럽트에는 이 콜백을
 * 부르지 마라" 이다. 플래그가 없으면 상태와 무관하게 부른다. 조건식이
 * 헷갈리는데, "플래그가 없거나(무조건 부름) 인터럽트가 켜져 있으면"
 * 부른다는 뜻이다.
 *
 * 실행 컨텍스트: CPU 핫플러그 콜백, 프로세스 문맥.
 *
 * 호출 체인:
 *   아키텍처 CPU 시작 코드 → [이 함수] → chip->irq_cpu_online()
 */
void irq_cpu_online(void)
{
	unsigned int irq;	/* [한국어] 순회용 번호 */

	for_each_active_irq(irq) {	/* [한국어] 시스템의 모든 인터럽트를 훑는다. 이 전수 조사가 폐기 이유다 */
		struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 서술자 조회 */
		struct irq_chip *chip;	/* [한국어] 콜백을 볼 칩 */

		if (!desc)	/* [한국어] 순회 중에 사라졌는가 */
			continue;	/* [한국어] 건너뛴다 */

		guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 인터럽트마다 락을 잡았다 놓는다. 전체를 한 번에 잡으면 지연이 너무 길어진다 */
		chip = irq_data_get_irq_chip(&desc->irq_data);	/* [한국어] 담당 칩 */
		if (chip && chip->irq_cpu_online &&	/* [한국어] 칩이 있고 이 콜백을 제공하는가 */
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||	/* [한국어] 상태를 따지지 않는 칩이거나 */
		     !irqd_irq_disabled(&desc->irq_data)))	/* [한국어] 인터럽트가 켜져 있는가 */
			chip->irq_cpu_online(&desc->irq_data);	/* [한국어] 새 CPU 에 맞춰 하드웨어를 조정하게 한다 */
	}
}

/**
 *	irq_cpu_offline - Invoke all irq_cpu_offline functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_offline()
 *	for each.
 */
/*
 * [한국어]
 * irq_cpu_offline - CPU 가 오프라인이 될 때 모든 칩에 알린다
 *
 * @return: 없음
 *
 * 위 irq_cpu_online() 의 정확한 반대이고, 폐기 예정인 이유도 같다.
 * 코드가 콜백 이름 하나만 빼고 동일하다.
 *
 * 두 함수를 하나로 합치지 않은 이유는 아마 역사적인 것이다. 폐기
 * 예정 코드라 정리할 동기가 없다.
 *
 * 실행 컨텍스트: CPU 핫플러그 콜백, 프로세스 문맥.
 *
 * 호출 체인:
 *   아키텍처 CPU 정지 코드 → [이 함수] → chip->irq_cpu_offline()
 */
void irq_cpu_offline(void)
{
	unsigned int irq;	/* [한국어] 순회용 번호 */

	for_each_active_irq(irq) {	/* [한국어] 모든 인터럽트 전수 조사 */
		struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 서술자 조회 */
		struct irq_chip *chip;	/* [한국어] 담당 칩 */

		if (!desc)	/* [한국어] 순회 중에 사라졌는가 */
			continue;	/* [한국어] 건너뛴다 */

		guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 인터럽트마다 락 */
		chip = irq_data_get_irq_chip(&desc->irq_data);	/* [한국어] 담당 칩 */
		if (chip && chip->irq_cpu_offline &&	/* [한국어] 콜백을 제공하는가 */
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||	/* [한국어] 상태를 따지지 않는 칩이거나 */
		     !irqd_irq_disabled(&desc->irq_data)))	/* [한국어] 켜져 있는가 */
			chip->irq_cpu_offline(&desc->irq_data);	/* [한국어] 사라지는 CPU 에 맞춰 하드웨어를 조정하게 한다 */
	}
}
#endif	/* [한국어] CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE 분기의 끝 */

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 여기부터 파일 끝까지는 계층형 도메인 전용이다.
					 * 계층형이란 인터럽트 하나가 여러 컨트롤러를 거치는 구조다.
					 * 예: PCI 장치 → MSI 도메인 → 리매핑 도메인 → 벡터 도메인 → CPU.
					 * 각 층이 자기 irq_data 를 갖고 parent_data 로 이어진다.
					 * 아래 irq_chip_*_parent 함수들은 바깥 층 칩이 자기 콜백에서
					 * "이 일은 부모가 해야 한다" 고 위임할 때 쓰는 도구 모음이다. */

#ifdef CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS	/* [한국어] 계층형에서 fasteoi 변형이 필요한 아키텍처만 */
/**
 * handle_fasteoi_ack_irq - irq handler for edge hierarchy stacked on
 *			    transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_ack() function called.
 */
/*
 * [한국어]
 * handle_fasteoi_ack_irq - fasteoi 에 ack 를 더한 처리기 (에지 계층용)
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 어떤 상황을 위한 것인가: 바깥 층은 에지 트리거라 ack 가 필요한데
 * 안쪽(부모) 층은 fasteoi 방식인 계층이다. 예를 들어 SoC 의 에지
 * 트리거 GPIO 컨트롤러가 GIC 위에 얹힌 경우다.
 *
 * handle_fasteoi_irq() 와 딱 한 줄 다르다 — handle_irq_event() 앞에
 * irq_ack 호출이 있다. 그 한 줄 때문에 함수를 통째로 복제한 것이,
 * 인터럽트 경로에서 조건 분기 하나도 아깝다는 판단이다.
 *
 * ack 위치가 중요하다. oneshot 마스크 뒤, 핸들러 앞이다. 마스크보다
 * 먼저 ack 하면 그 틈에 새 신호가 래치에 걸리고, 핸들러 뒤로 미루면
 * 처리 중에 온 신호를 잃는다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() → desc->handle_irq → [이 함수]
 */
void handle_fasteoi_ack_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;	/* [한국어] EOI 콜백과 플래그를 볼 칩 */

	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안 */

	if (!irq_can_handle_pm(desc)) {	/* [한국어] 절전 해제이거나 경쟁 상황인가 */
		cond_eoi_irq(chip, &desc->irq_data);	/* [한국어] 칩에 따라 EOI. handle_fasteoi_irq 와 달리 PENDING 표시는 하지 않는다 — 에지 계층에서는 부모가 재전송을 맡는다 */
		return;
	}

	if (unlikely(!irq_can_handle_actions(desc))) {	/* [한국어] 핸들러가 없거나 꺼져 있는가 */
		mask_irq(desc);	/* [한국어] 처리할 수 없는 인터럽트가 계속 올라오는 것을 막는다 */
		cond_eoi_irq(chip, &desc->irq_data);	/* [한국어] 칩에 따라 EOI */
		return;
	}

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수 */
	if (desc->istate & IRQS_ONESHOT)	/* [한국어] 스레드 핸들러를 쓰는가 */
		mask_irq(desc);	/* [한국어] 스레드가 끝날 때까지 막아 둔다 */

	desc->irq_data.chip->irq_ack(&desc->irq_data);	/* [한국어] 이 한 줄이 handle_fasteoi_irq 와의 유일한 차이다. 마스크 뒤, 핸들러 앞이어야 한다 — 먼저 하면 틈이 생기고 나중에 하면 신호를 잃는다 */

	handle_irq_event(desc);	/* [한국어] 핸들러 실행 */

	cond_unmask_eoi_irq(desc, chip);	/* [한국어] EOI 와 언마스크를 조건에 따라 */
}
EXPORT_SYMBOL_GPL(handle_fasteoi_ack_irq);	/* [한국어] 계층형 에지 컨트롤러 드라이버가 건다 */

/**
 * handle_fasteoi_mask_irq - irq handler for level hierarchy stacked on
 *			     transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_mask_ack() function called.
 */
/*
 * [한국어]
 * handle_fasteoi_mask_irq - fasteoi 에 mask_ack 를 더한 처리기 (레벨 계층용)
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 ack 판의 레벨 트리거 짝이다. 바깥 층이 레벨 트리거라 처리 전에
 * 막아야 하고 안쪽은 fasteoi 인 계층에 쓴다.
 *
 * 구조가 handle_level_irq() 와 handle_fasteoi_irq() 를 섞은 모양이다.
 * 첫 줄에서 mask_ack_irq() 를 부르는 것은 레벨 처리기와 같고,
 * 마지막에 cond_unmask_eoi_irq() 를 부르는 것은 fasteoi 와 같다.
 *
 * mask_ack 를 판정보다 먼저 하는 것이 레벨 처리의 원칙이다. 처리하지
 * 못하는 경우에도 막혀 있어야 인터럽트가 폭주하지 않는다.
 *
 * ONESHOT 검사가 없는 것에 주목: 이미 무조건 막았으므로 따로 막을
 * 필요가 없다. cond_unmask_eoi_irq() 가 스레드 상태를 보고 열지 말지를
 * 결정한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() → desc->handle_irq → [이 함수]
 */
void handle_fasteoi_mask_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;	/* [한국어] EOI 콜백과 플래그를 볼 칩 */

	guard(raw_spinlock)(&desc->lock);	/* [한국어] 함수 전체가 락 안 */
	mask_ack_irq(desc);	/* [한국어] 레벨 처리의 원칙 — 판정보다 먼저 막는다. 처리 못 해도 막혀 있어야 폭주하지 않는다 */

	if (!irq_can_handle(desc)) {	/* [한국어] 두 관문을 통과하는가 */
		cond_eoi_irq(chip, &desc->irq_data);	/* [한국어] 칩에 따라 EOI */
		return;
	}

	kstat_incr_irqs_this_cpu(desc);	/* [한국어] 발생 횟수. ONESHOT 검사가 없는 것은 이미 무조건 막았기 때문이다 */

	handle_irq_event(desc);	/* [한국어] 핸들러 실행 */

	cond_unmask_eoi_irq(desc, chip);	/* [한국어] 스레드 상태를 보고 EOI 와 언마스크를 결정한다 */
}
EXPORT_SYMBOL_GPL(handle_fasteoi_mask_irq);	/* [한국어] 계층형 레벨 컨트롤러 드라이버가 건다 */

#endif /* CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS */	/* [한국어] 계층형 fasteoi 변형의 끝 */

#ifdef CONFIG_SMP	/* [한국어] 리다이렉트는 다른 CPU 가 있어야 의미가 있다 */
/*
 * [한국어]
 * irq_chip_pre_redirect_parent - 리다이렉트 준비를 부모 칩에 위임한다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 디먹스 인터럽트를 다른 CPU 로 넘기기 직전에 불리는 콜백의 위임
 * 판이다. 계층형에서 실제 하드웨어를 만지는 것은 대개 부모 층이므로,
 * 바깥 층 칩은 자기 irq_pre_redirect 자리에 이 함수를 꽂아 두면 된다.
 *
 * 무엇을 준비하는가: 대개 원인 비트를 지우는 일이다. 넘기는 동안
 * 같은 인터럽트가 또 올라오면 작업 항목이 중복 큐잉되거나 목표 CPU 가
 * 헛일을 하게 된다.
 *
 * NULL 검사가 없다: 부모가 이 콜백을 갖고 있다는 것을 확인한 쪽이
 * 이 함수를 꽂았을 것이라는 전제다. 이 파일의 위임 함수들이 대부분
 * 이런 성격인데, 없는 콜백을 부르면 곧바로 터지므로 개발 중에 드러난다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   demux_redirect_remote() (kernel/irq/irqdesc.c) →
 *   chip->irq_pre_redirect → [이 함수] → 부모 칩의 같은 콜백
 */
void irq_chip_pre_redirect_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로. 이 파일 위임 함수들의 공통 첫 줄이다 */
	data->chip->irq_pre_redirect(data);	/* [한국어] 부모의 같은 콜백. NULL 검사가 없는 것은 이 함수를 꽂은 쪽이 부모의 지원을 확인했다는 전제다 */
}
EXPORT_SYMBOL_GPL(irq_chip_pre_redirect_parent);	/* [한국어] 계층형 디먹스 컨트롤러 드라이버용 */
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/**
 * irq_chip_set_parent_state - set the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: State to be restored (one of IRQCHIP_STATE_*)
 * @val: Value corresponding to @which
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
/*
 * [한국어]
 * irq_chip_set_parent_state - 부모 인터럽트의 상태를 설정한다
 *
 * @data:  이 층의 irq_data
 * @which: 어떤 상태인가 (IRQCHIP_STATE_PENDING, _ACTIVE, _MASKED 등)
 * @val:   설정할 값
 * @return: 0 성공 또는 부모가 지원하지 않음, 그 외 부모가 낸 오류
 *
 * 부모 칩의 상태를 직접 조작하는 통로다. 주로 가상화에서 쓰인다 —
 * 게스트가 인터럽트를 처리하다 마이그레이션되면, 새 호스트에서
 * "대기 중" 같은 상태를 복원해야 한다.
 *
 * 부모가 콜백을 갖고 있지 않으면 0(성공)을 돌려주는 것이 이 함수의
 * 규약이다. 원본 주석의 "conditional success" 가 그 뜻이다. 왜 오류가
 * 아닌가: 상태 조작을 지원하지 않는 칩은 애초에 그런 상태를 갖지
 * 않으므로, 설정할 것이 없다는 뜻이지 실패가 아니다.
 *
 * parent_data 가 NULL 인 경우도 같이 처리한다 — 이 층이 계층의
 * 맨 안쪽이라 부모가 없는 경우다.
 *
 * 실행 컨텍스트: 호출자를 따른다.
 *
 * 호출 체인:
 *   KVM 등 가상화 코드 / 계층형 칩 드라이버 → [이 함수]
 */
int irq_chip_set_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool val)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */

	if (!data || !data->chip->irq_set_irqchip_state)	/* [한국어] 부모가 없거나 상태 설정을 지원하지 않는가 */
		return 0;	/* [한국어] 오류가 아니다 — 그런 상태를 갖지 않는 칩이라 설정할 것이 없다는 뜻이다 */

	return data->chip->irq_set_irqchip_state(data, which, val);	/* [한국어] 부모에게 위임. 결과를 그대로 올린다 */
}
EXPORT_SYMBOL_GPL(irq_chip_set_parent_state);	/* [한국어] 가상화 코드와 계층형 드라이버용 */

/**
 * irq_chip_get_parent_state - get the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: one of IRQCHIP_STATE_* the caller wants to know
 * @state: a pointer to a boolean where the state is to be stored
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
/*
 * [한국어]
 * irq_chip_get_parent_state - 부모 인터럽트의 상태를 읽는다
 *
 * @data:  이 층의 irq_data
 * @which: 어떤 상태를 묻는가
 * @state: 결과를 담을 곳 (출력)
 * @return: 0 성공 또는 부모가 지원하지 않음, 그 외 부모가 낸 오류
 *
 * 위 set 의 읽기 짝이다. 가상화에서 게스트를 저장할 때 인터럽트
 * 상태를 함께 저장하려면 이것으로 읽는다.
 *
 * 한 가지 함정: 부모가 지원하지 않으면 0 을 돌려주면서 @state 를
 * 건드리지 않는다. 호출자가 그 변수를 초기화하지 않았다면 쓰레기
 * 값을 성공으로 받게 된다. 호출자가 미리 false 로 초기화해 두어야
 * 한다.
 *
 * 실행 컨텍스트: 호출자를 따른다.
 *
 * 호출 체인:
 *   KVM 등 가상화 코드 / 계층형 칩 드라이버 → [이 함수]
 */
int irq_chip_get_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool *state)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */

	if (!data || !data->chip->irq_get_irqchip_state)	/* [한국어] 부모가 없거나 상태 읽기를 지원하지 않는가 */
		return 0;	/* [한국어] 성공으로 돌려주되 @state 는 건드리지 않는다. 호출자가 미리 초기화해 두어야 하는 이유다 */

	return data->chip->irq_get_irqchip_state(data, which, state);	/* [한국어] 부모에게 위임 */
}
EXPORT_SYMBOL_GPL(irq_chip_get_parent_state);	/* [한국어] 가상화 코드와 계층형 드라이버용 */

/**
 * irq_chip_shutdown_parent - Shutdown the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_shutdown() callback of the parent if available or falls
 * back to irq_chip_disable_parent().
 */
/*
 * [한국어]
 * irq_chip_shutdown_parent - 부모 인터럽트를 정지한다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 계층형 칩이 자기 irq_shutdown 자리에 꽂는 위임 함수다. 부모가
 * 전용 정지 콜백을 가지면 그것을, 없으면 비활성으로 대신한다.
 *
 * 이 대체가 성립하는 이유: 정지는 비활성보다 강한 개념이지만
 * (자원 반납까지 포함), 부모가 정지 콜백을 갖지 않는다는 것은 반납할
 * 자원이 없다는 뜻이다. 그러면 비활성만으로 충분하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_shutdown() → chip->irq_shutdown → [이 함수] →
 *   부모의 irq_shutdown 또는 irq_chip_disable_parent()
 */
void irq_chip_shutdown_parent(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;	/* [한국어] 부모 층. 아래에서 두 번 쓰므로 변수에 담는다 */

	if (parent->chip->irq_shutdown)	/* [한국어] 부모가 전용 정지 콜백을 갖는가 */
		parent->chip->irq_shutdown(parent);	/* [한국어] 자원 반납까지 포함한 정지 */
	else	/* [한국어] 없으면 */
		irq_chip_disable_parent(data);	/* [한국어] 비활성으로 대신한다. 정지 콜백이 없다는 것은 반납할 자원이 없다는 뜻이다. data 를 넘기는 것에 주의 — 그 함수가 다시 parent_data 를 따라간다 */
}
EXPORT_SYMBOL_GPL(irq_chip_shutdown_parent);	/* [한국어] 계층형 칩 드라이버가 콜백 자리에 꽂는다 */

/**
 * irq_chip_startup_parent - Startup the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_startup() callback of the parent if available or falls
 * back to irq_chip_enable_parent().
 */
/*
 * [한국어]
 * irq_chip_startup_parent - 부모 인터럽트를 시작한다
 *
 * @data: 이 층의 irq_data
 * @return: 부모의 irq_startup 반환값, 대체 경로면 0
 *
 * 위 shutdown 의 짝이다. 부모가 전용 시작 콜백을 가지면 그 반환값을
 * 그대로 올리고, 없으면 활성화로 대신하고 0 을 돌려준다.
 *
 * 반환값의 의미: irq_chip::irq_startup 은 unsigned int 를 돌려주는데,
 * 0 이 아닌 값은 "시작했고 대기 중인 인터럽트가 있다" 를 뜻한다.
 * 대체 경로에서 0 인 것은 활성화만으로는 그런 정보를 알 수 없어서다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   __irq_startup() → chip->irq_startup → [이 함수] →
 *   부모의 irq_startup 또는 irq_chip_enable_parent()
 */
unsigned int irq_chip_startup_parent(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;	/* [한국어] 부모 층 */

	if (parent->chip->irq_startup)	/* [한국어] 부모가 전용 시작 콜백을 갖는가 */
		return parent->chip->irq_startup(parent);	/* [한국어] 반환값을 그대로 올린다. 0 이 아니면 "대기 중인 인터럽트 있음" 이다 */

	irq_chip_enable_parent(data);	/* [한국어] 활성화로 대신한다 */
	return 0;	/* [한국어] 활성화만으로는 대기 인터럽트 여부를 알 수 없어 0 이다 */
}
EXPORT_SYMBOL_GPL(irq_chip_startup_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_enable_parent - Enable the parent interrupt (defaults to unmask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_enable_parent - 부모 인터럽트를 켠다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 부모가 전용 irq_enable 을 가지면 그것을, 없으면 언마스크로 대신한다.
 * 이 파일 위쪽의 irq_enable() 이 같은 판단을 하는 것과 같은 구조다 —
 * enable 과 unmask 가 다른 레지스터일 수 있는 칩을 위한 것이다.
 *
 * irq_unmask 에 NULL 검사가 없는 이유: 계층에 참여하는 칩은 최소한
 * mask/unmask 는 제공해야 한다는 것이 암묵적 규약이다. 그것마저 없으면
 * 그 층이 존재할 이유가 없다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   계층형 칩의 irq_enable 콜백 / irq_chip_startup_parent() → [이 함수]
 */
void irq_chip_enable_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	if (data->chip->irq_enable)	/* [한국어] 부모가 전용 켜기 콜백을 갖는가 */
		data->chip->irq_enable(data);	/* [한국어] enable 과 unmask 가 다른 레지스터일 수 있다 */
	else	/* [한국어] 없으면 */
		data->chip->irq_unmask(data);	/* [한국어] 언마스크로 대신한다. NULL 검사가 없는 것은 계층 참여 칩이 최소한 mask/unmask 는 제공한다는 규약 때문이다 */
}
EXPORT_SYMBOL_GPL(irq_chip_enable_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_disable_parent - Disable the parent interrupt (defaults to mask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_disable_parent - 부모 인터럽트를 끈다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 위 enable 의 정확한 반대다. 부모가 전용 irq_disable 을 가지면
 * 그것을, 없으면 마스크로 대신한다.
 *
 * 여기서는 게으른 비활성을 쓰지 않는 것에 주목: 부모 층에 대해서는
 * 항상 실제로 하드웨어를 건드린다. 게으른 비활성은 코어의 상태 추적과
 * 흐름 처리기의 협력이 있어야 성립하는데, 부모 층은 그 추적 대상이
 * 아니기 때문이다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   계층형 칩의 irq_disable 콜백 / irq_chip_shutdown_parent() → [이 함수]
 */
void irq_chip_disable_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	if (data->chip->irq_disable)	/* [한국어] 부모가 전용 끄기 콜백을 갖는가 */
		data->chip->irq_disable(data);	/* [한국어] 클록을 끄는 등 마스크보다 큰 일을 할 수 있다 */
	else	/* [한국어] 없으면 */
		data->chip->irq_mask(data);	/* [한국어] 마스크로 대신한다. 게으른 비활성을 쓰지 않는 이유는 부모 층이 코어의 상태 추적 대상이 아니어서다 */
}
EXPORT_SYMBOL_GPL(irq_chip_disable_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_ack_parent - Acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_ack_parent - 부모 인터럽트의 래치를 지운다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 위임 함수 중 가장 단순한 형태다. 대체 경로도 조건도 없다.
 *
 * 왜 대체가 없는가: ack 는 "래치를 지운다" 는 하나의 뜻뿐이라 다른
 * 콜백으로 흉내 낼 수 없다. 마스크로 대신하면 인터럽트가 사라지고,
 * 아무것도 하지 않으면 같은 인터럽트가 무한히 반복된다.
 *
 * 부모가 ack 를 제공하지 않으면 여기서 NULL 호출로 터진다. 그것이
 * 의도된 동작이다 — ack 가 필요한 계층을 잘못 구성했다는 뜻이고,
 * 조용히 넘어가면 인터럽트 폭주로 시스템이 멈춘 뒤에야 알게 된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   흐름 처리기 → 계층형 칩의 irq_ack 콜백 → [이 함수] → 부모의 irq_ack
 */
void irq_chip_ack_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	data->chip->irq_ack(data);	/* [한국어] 대체 경로가 없다. ack 는 다른 콜백으로 흉내 낼 수 없어, 없으면 여기서 터지는 것이 낫다 */
}
EXPORT_SYMBOL_GPL(irq_chip_ack_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_mask_parent - Mask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_mask_parent - 부모 인터럽트를 막는다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 가장 많이 쓰이는 위임 함수다. 계층형 칩 대부분이 자기 irq_mask 자리에
 * 그냥 이것을 꽂는다 — 바깥 층은 마스크 레지스터를 갖지 않고 부모에
 * 의존하는 경우가 흔하기 때문이다.
 *
 * 예를 들어 MSI 도메인의 바깥 층은 논리적 개념이라 막을 하드웨어가
 * 없다. 실제 마스크는 아래 벡터 도메인이나 리매핑 하드웨어가 한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   mask_irq() → 계층형 칩의 irq_mask 콜백 → [이 함수] → 부모의 irq_mask
 */
void irq_chip_mask_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	data->chip->irq_mask(data);	/* [한국어] 부모에게 그대로 넘긴다. 바깥 층이 막을 하드웨어를 갖지 않는 경우가 흔하다 */
}
EXPORT_SYMBOL_GPL(irq_chip_mask_parent);	/* [한국어] 계층형 칩 드라이버가 가장 많이 쓰는 위임 함수 */

/**
 * irq_chip_mask_ack_parent - Mask and acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_mask_ack_parent - 부모 인터럽트를 막고 래치도 지운다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * mask 와 ack 를 따로 위임하지 않고 부모의 irq_mask_ack 를 그대로
 * 부른다. 그래야 두 동작이 부모 안에서 원자적으로 묶인다.
 *
 * 만약 여기서 irq_chip_mask_parent() 와 irq_chip_ack_parent() 를
 * 이어 부르면 부모의 락을 두 번 잡게 되고, 그 사이에 다른 CPU 가
 * 끼어들 수 있다. 그것을 피하려고 별도 함수를 둔다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   mask_ack_irq() → 계층형 칩의 irq_mask_ack 콜백 → [이 함수]
 */
void irq_chip_mask_ack_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	data->chip->irq_mask_ack(data);	/* [한국어] 부모의 묶음 콜백을 쓴다. mask 와 ack 를 따로 위임하면 부모 락을 두 번 잡아 그 사이에 틈이 생긴다 */
}
EXPORT_SYMBOL_GPL(irq_chip_mask_ack_parent);	/* [한국어] 계층형 에지 컨트롤러 드라이버용 */

/**
 * irq_chip_unmask_parent - Unmask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_unmask_parent - 부모 인터럽트를 다시 연다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * irq_chip_mask_parent() 의 짝이다. 둘은 거의 항상 함께 꽂힌다 —
 * 마스크를 부모에 위임하는 칩은 언마스크도 위임해야 한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   unmask_irq() → 계층형 칩의 irq_unmask 콜백 → [이 함수]
 */
void irq_chip_unmask_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	data->chip->irq_unmask(data);	/* [한국어] 부모에게 그대로 넘긴다. mask 를 위임한 칩은 unmask 도 반드시 위임해야 짝이 맞는다 */
}
EXPORT_SYMBOL_GPL(irq_chip_unmask_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_eoi_parent - Invoke EOI on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_eoi_parent - 부모 인터럽트에 EOI 를 보낸다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 우선순위 걸쇠를 관리하는 것은 대개 계층의 가장 안쪽 — CPU 에 가장
 * 가까운 층 — 이다. 바깥 층들은 그저 이 함수를 꽂아 EOI 를 아래로
 * 흘려보낸다.
 *
 * ack 와 마찬가지로 대체 경로가 없다. EOI 를 보내지 않으면 우선순위
 * 걸쇠가 풀리지 않아 같거나 낮은 우선순위의 인터럽트가 전부 멈춘다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥 또는 스레드 문맥(EOI_THREADED),
 * desc->lock 보유.
 *
 * 호출 체인:
 *   handle_fasteoi_irq() → 계층형 칩의 irq_eoi 콜백 → [이 함수]
 */
void irq_chip_eoi_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	data->chip->irq_eoi(data);	/* [한국어] 우선순위 걸쇠를 관리하는 것은 대개 가장 안쪽 층이다. 바깥 층은 그저 흘려보낸다 */
}
EXPORT_SYMBOL_GPL(irq_chip_eoi_parent);	/* [한국어] 계층형 fasteoi 칩 드라이버용 */

/**
 * irq_chip_set_affinity_parent - Set affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @dest:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * [한국어]
 * irq_chip_set_affinity_parent - 친화도 설정을 부모에게 위임한다
 *
 * @data:  이 층의 irq_data
 * @dest:  설정할 CPU 마스크
 * @force: 온라인 검사를 무시하고 강제할지
 * @return: 부모가 낸 값, 부모가 지원하지 않으면 -ENOSYS
 *
 * 친화도를 실제로 결정하는 것은 대개 계층의 가장 안쪽 벡터 도메인이다.
 * 어느 CPU 의 어느 벡터 번호를 쓸지가 곧 친화도이기 때문이다. 바깥
 * 층들은 이 함수로 아래에 넘긴다.
 *
 * -ENOSYS 를 돌려주는 것이 위 set_parent_state 계열과 다른 점이다.
 * 그쪽은 0(성공)이었다. 차이의 이유: 상태 설정은 지원하지 않아도
 * 의미가 통하지만, 친화도 설정은 지원하지 않으면 사용자의 요청이
 * 이루어지지 않은 것이다. 호출자가 그것을 알아야 한다.
 *
 * force 인자: CPU 핫플러그처럼 대상 CPU 가 아직 온라인이 아닌 상태에서
 * 미리 설정해야 하는 경우를 위한 것이다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → 계층형 칩의 irq_set_affinity 콜백 → [이 함수]
 */
int irq_chip_set_affinity_parent(struct irq_data *data,
				 const struct cpumask *dest, bool force)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	if (data->chip->irq_set_affinity)	/* [한국어] 부모가 친화도 설정을 지원하는가 */
		return data->chip->irq_set_affinity(data, dest, force);	/* [한국어] 위임. IRQ_SET_MASK_OK 계열의 값이 나온다 */

	return -ENOSYS;	/* [한국어] 상태 설정 계열과 달리 오류다. 사용자의 친화도 요청이 이루어지지 않았음을 호출자가 알아야 한다 */
}
EXPORT_SYMBOL_GPL(irq_chip_set_affinity_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_set_type_parent - Set IRQ type on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * [한국어]
 * irq_chip_set_type_parent - 트리거 방식 설정을 부모에게 위임한다
 *
 * @data: 이 층의 irq_data
 * @type: IRQ_TYPE_EDGE_RISING 등
 * @return: 부모가 낸 값, 지원하지 않으면 -ENOSYS
 *
 * 트리거 방식을 실제로 정하는 층은 하드웨어에 따라 다르다. GPIO
 * 컨트롤러라면 바깥 층이 직접 하고, 단순한 중계 층이라면 부모에게
 * 넘긴다.
 *
 * -ENOSYS 인 이유는 위 친화도와 같다. 사용자가 요청한 트리거 방식이
 * 반영되지 않았다면 알려야 한다 — 그대로 두면 에지 장치를 레벨로
 * 처리하는 등의 오동작이 생긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   __irq_set_trigger() → 계층형 칩의 irq_set_type 콜백 → [이 함수]
 */
int irq_chip_set_type_parent(struct irq_data *data, unsigned int type)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */

	if (data->chip->irq_set_type)	/* [한국어] 부모가 트리거 설정을 지원하는가 */
		return data->chip->irq_set_type(data, type);	/* [한국어] 위임 */

	return -ENOSYS;	/* [한국어] 요청이 반영되지 않았음을 알린다. 조용히 넘기면 에지 장치를 레벨로 처리하는 오동작이 생긴다 */
}
EXPORT_SYMBOL_GPL(irq_chip_set_type_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_retrigger_hierarchy - Retrigger an interrupt in hardware
 * @data:	Pointer to interrupt specific data
 *
 * Iterate through the domain hierarchy of the interrupt and check
 * whether a hw retrigger function exists. If yes, invoke it.
 */
/*
 * [한국어]
 * irq_chip_retrigger_hierarchy - 계층을 훑어 하드웨어 재전송을 시도한다
 *
 * @data: 이 층의 irq_data
 * @return: 재전송을 수행한 층의 반환값, 아무 층도 지원하지 않으면 0
 *
 * 다른 위임 함수들과 다르다. 부모 한 층이 아니라 계층 전체를 훑는다.
 *
 * 왜 그런가: 재전송은 "이 인터럽트를 다시 올려 달라" 는 요청인데,
 * 어느 층이 그것을 할 수 있는지 미리 알 수 없다. 벡터 도메인이 할
 * 수도 있고 중간의 리매핑 하드웨어가 할 수도 있다. 그래서 하나라도
 * 찾을 때까지 내려간다.
 *
 * 0 을 돌려주는 것의 의미가 중요하다. "하드웨어로 재전송하지
 * 못했다" 는 뜻이고, 호출자인 check_irq_resend() 가 그것을 보고
 * 소프트웨어 재전송(tasklet)으로 대신한다. 오류가 아니라 대체 경로로
 * 넘기는 신호다.
 *
 * data->parent_data 부터 시작하는 것에 주목: 자기 층은 건너뛴다.
 * 자기 층의 retrigger 를 부르는 것은 이 함수를 부른 그 콜백 자신이라
 * 무한 재귀가 된다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   check_irq_resend() (kernel/irq/resend.c) → chip->irq_retrigger →
 *   [이 함수] → 계층 어딘가의 irq_retrigger
 */
int irq_chip_retrigger_hierarchy(struct irq_data *data)
{
	for (data = data->parent_data; data; data = data->parent_data)	/* [한국어] 자기 층은 건너뛰고 부모부터. 자기 것을 부르면 무한 재귀다 */
		if (data->chip && data->chip->irq_retrigger)	/* [한국어] 이 층이 하드웨어 재전송을 할 수 있는가 */
			return data->chip->irq_retrigger(data);	/* [한국어] 처음 찾은 층이 한다. 어느 층이 할 수 있는지 미리 알 수 없어 훑는다 */

	return 0;	/* [한국어] 아무 층도 못 한다. 오류가 아니라 "소프트웨어 재전송으로 대신하라" 는 신호다 */
}
EXPORT_SYMBOL_GPL(irq_chip_retrigger_hierarchy);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_set_vcpu_affinity_parent - Set vcpu affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @vcpu_info:	The vcpu affinity information
 */
/*
 * [한국어]
 * irq_chip_set_vcpu_affinity_parent - vCPU 친화도 설정을 부모에게 위임한다
 *
 * @data:      이 층의 irq_data
 * @vcpu_info: 가상 CPU 정보 (구조는 아키텍처마다 다르다)
 * @return:    부모가 낸 값, 지원하지 않으면 -ENOSYS
 *
 * vCPU 친화도란: 가상화에서 인터럽트를 호스트 CPU 가 아니라 게스트의
 * 가상 CPU 로 직접 전달하는 기능이다. 그러면 호스트가 중간에서
 * 가로채 다시 주입하는 비용이 사라진다. x86 의 posted interrupt 나
 * ARM 의 GICv4 가 그런 기능이다.
 *
 * vcpu_info 가 void 포인터인 이유: 그 내용이 아키텍처마다 완전히
 * 달라 공통 타입을 정의할 수 없다. 이 함수는 그저 아래로 넘기기만
 * 하므로 내용을 알 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   KVM 의 인터럽트 전달 설정 → irq_set_vcpu_affinity() →
 *   계층형 칩의 콜백 → [이 함수]
 */
int irq_chip_set_vcpu_affinity_parent(struct irq_data *data, void *vcpu_info)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	if (data->chip->irq_set_vcpu_affinity)	/* [한국어] 부모가 vCPU 직접 전달을 지원하는가 */
		return data->chip->irq_set_vcpu_affinity(data, vcpu_info);	/* [한국어] 위임. vcpu_info 는 아키텍처마다 달라 void 포인터로 그냥 흘려보낸다 */

	return -ENOSYS;	/* [한국어] 지원하지 않으면 KVM 이 일반 주입 경로로 대체한다 */
}
EXPORT_SYMBOL_GPL(irq_chip_set_vcpu_affinity_parent);	/* [한국어] 계층형 칩 드라이버용 */
/**
 * irq_chip_set_wake_parent - Set/reset wake-up on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @on:		Whether to set or reset the wake-up capability of this irq
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * [한국어]
 * irq_chip_set_wake_parent - 절전 해제 설정을 부모에게 위임한다
 *
 * @data: 이 층의 irq_data
 * @on:   1 이면 wakeup 원으로, 0 이면 해제
 * @return: 부모가 낸 값, SKIP 플래그면 0, 지원하지 않으면 -ENOSYS
 *
 * 다른 위임 함수와 달리 검사가 두 겹이다.
 *
 * IRQCHIP_SKIP_SET_WAKE 는 "이 칩은 wakeup 설정이 필요 없다" 는 뜻이다.
 * 항상 깨어 있는 컨트롤러 — 서스펜드해도 전원이 유지되는 — 가
 * 그렇다. 그런 칩에서는 아무것도 하지 않고 성공을 돌려주는 것이 맞다.
 *
 * 그 플래그가 없는데 콜백도 없으면 -ENOSYS 다. 이 경우는 "필요한데
 * 못 한다" 이므로 오류다. 사용자가 enable_irq_wake() 를 불렀는데
 * 실제로 깨어나지 않는 시스템을 만들지 않으려는 것이다.
 *
 * 두 경우를 구분하지 않으면 어느 한쪽이 잘못된다 — 전부 0 이면
 * 안 깨어나는 시스템이 조용히 만들어지고, 전부 -ENOSYS 면 멀쩡한
 * 칩에서 wakeup 설정이 거절된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   enable_irq_wake() → set_irq_wake_real() → 계층형 칩의 콜백 → [이 함수]
 */
int irq_chip_set_wake_parent(struct irq_data *data, unsigned int on)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */

	if (data->chip->flags & IRQCHIP_SKIP_SET_WAKE)	/* [한국어] 이 칩은 wakeup 설정 자체가 불필요한가 — 서스펜드해도 전원이 유지되는 컨트롤러다 */
		return 0;	/* [한국어] 할 일 없이 성공. 아래 -ENOSYS 와 구분해야 한다 */

	if (data->chip->irq_set_wake)	/* [한국어] 부모가 wakeup 설정을 지원하는가 */
		return data->chip->irq_set_wake(data, on);	/* [한국어] 위임 */

	return -ENOSYS;	/* [한국어] 필요한데 못 한다 — 오류다. 조용히 성공하면 안 깨어나는 시스템이 만들어진다 */
}
EXPORT_SYMBOL_GPL(irq_chip_set_wake_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_request_resources_parent - Request resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_request_resources_parent - 자원 요청을 부모에게 위임한다
 *
 * @data: 이 층의 irq_data
 * @return: 부모가 낸 값, 부모가 이 콜백을 갖지 않으면 0
 *
 * irq_request_resources 콜백은 request_irq() 초기에 불려, 인터럽트를
 * 쓰기 전에 필요한 자원(클록, 전원, GPIO 핀 등)을 잡는다. 실패할 수
 * 있는 준비 작업을 여기 모아 두면, 실패 시 되돌리기가 쉬워진다.
 *
 * 없어도 0 인 이유가 원본 주석에 있다 — 이 콜백은 선택적이다. 잡을
 * 자원이 없는 칩이 대부분이고, 그것은 오류가 아니다.
 *
 * 위 set_wake 와 대조적이다. 그쪽은 "필요한데 못 한다" 를 구분해야
 * 했지만, 자원 요청은 "없으면 잡을 것도 없다" 로 뜻이 통한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다 — 클록이나 전원을
 * 켜는 작업이 포함될 수 있다.
 *
 * 호출 체인:
 *   __setup_irq() → chip->irq_request_resources → [이 함수]
 */
int irq_chip_request_resources_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */

	if (data->chip->irq_request_resources)	/* [한국어] 부모가 잡을 자원이 있는가 */
		return data->chip->irq_request_resources(data);	/* [한국어] 위임. 실패하면 request_irq 전체가 실패한다 */

	/* no error on missing optional irq_chip::irq_request_resources */
	return 0;	/* [한국어] (위 영어 주석) 선택적 콜백이라 없어도 오류가 아니다. 잡을 자원이 없다는 뜻이다 */
}
EXPORT_SYMBOL_GPL(irq_chip_request_resources_parent);	/* [한국어] 계층형 칩 드라이버용 */

/**
 * irq_chip_release_resources_parent - Release resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * [한국어]
 * irq_chip_release_resources_parent - 자원 반납을 부모에게 위임한다
 *
 * @data: 이 층의 irq_data
 * @return: 없음
 *
 * 위 request 의 짝이다. free_irq() 경로에서 불려 잡았던 자원을 푼다.
 *
 * 반환값이 없는 것이 자연스럽다 — 해제는 실패할 수 없고, 실패해도
 * 호출자가 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   __free_irq() → chip->irq_release_resources → [이 함수]
 */
void irq_chip_release_resources_parent(struct irq_data *data)
{
	data = data->parent_data;	/* [한국어] 한 층 위로 */
	if (data->chip->irq_release_resources)	/* [한국어] 반납할 자원이 있는 칩인가 */
		data->chip->irq_release_resources(data);	/* [한국어] 위임. 반환값이 없는 것은 해제가 실패할 수 없고 실패해도 할 일이 없어서다 */
}
EXPORT_SYMBOL_GPL(irq_chip_release_resources_parent);	/* [한국어] 계층형 칩 드라이버용 */
#endif /* CONFIG_IRQ_DOMAIN_HIERARCHY */	/* [한국어] 계층형 도메인 전용 구역의 끝 */

#ifdef CONFIG_SMP	/* [한국어] 리다이렉트는 다른 CPU 가 있어야 의미가 있다 */
/*
 * [한국어]
 * irq_chip_redirect_set_affinity - 친화도를 하드웨어 대신 소프트웨어로 구현한다
 *
 * @data:  대상 irq_data
 * @dest:  요청된 CPU 마스크
 * @force: 무시한다
 * @return: 항상 IRQ_SET_MASK_OK_DONE
 *
 * 어떤 문제를 푸는가: 디먹스 컨트롤러의 자식 인터럽트는 하드웨어적으로
 * 친화도를 가질 수 없다. 부모 선 하나로 올라오므로 부모가 도착한
 * CPU 에서 처리될 수밖에 없다.
 *
 * 그런데 사용자는 그 자식 인터럽트의 친화도를 설정하고 싶어 한다.
 * 하드웨어가 못 한다면 소프트웨어로 흉내 내면 된다 — 목표 CPU 를
 * 기억해 두었다가, 인터럽트가 엉뚱한 CPU 에 도착하면 irq_work 로
 * 목표 CPU 에 넘기는 것이다. 그 기억이 redirect.target_cpu 다.
 *
 * cpumask_first 를 쓰는 이유: 넘기는 것은 CPU 하나로만 할 수 있다.
 * 마스크에 여러 CPU 가 있어도 첫 번째를 고른다.
 *
 * WRITE_ONCE 를 쓰는 이유: 이 값을 읽는 demux_redirect_remote() 는
 * 다른 CPU 의 인터럽트 문맥에서 READ_ONCE 로 읽는다. 컴파일러가
 * 쓰기를 쪼개거나 미루지 못하게 한다.
 *
 * 유효 친화도를 갱신하는 것도 중요하다. 사용자가 /proc 으로 결과를
 * 확인할 때 실제로 어디로 가는지 보여야 하고, demux_redirect_remote()
 * 도 이 값으로 "지금 CPU 가 대상인가" 를 판단한다.
 *
 * IRQ_SET_MASK_OK_DONE 은 "설정했고 코어는 더 할 일이 없다" 는 뜻이다.
 * 그냥 OK 면 코어가 유효 친화도를 다시 계산하려 드는데, 여기서는
 * 이미 우리가 정확한 값을 넣었다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → chip->irq_set_affinity → [이 함수]
 */
int irq_chip_redirect_set_affinity(struct irq_data *data, const struct cpumask *dest, bool force)
{
	struct irq_redirect *redir = &irq_data_to_desc(data)->redirect;	/* [한국어] 서술자 안의 리다이렉트 상태. irq_work 항목도 여기 들어 있다 */

	WRITE_ONCE(redir->target_cpu, cpumask_first(dest));	/* [한국어] 넘길 목표 CPU 를 기억한다. 하나만 고르는 이유는 irq_work 가 CPU 하나에만 던질 수 있어서다. WRITE_ONCE 는 인터럽트 문맥의 READ_ONCE 와 짝이다 */
	irq_data_update_effective_affinity(data, dest);	/* [한국어] 유효 친화도를 요청값 그대로 반영한다. /proc 표시와 demux_redirect_remote 의 판단 근거가 된다 */

	return IRQ_SET_MASK_OK_DONE;	/* [한국어] "설정했고 코어는 더 할 일 없음". 그냥 OK 면 코어가 유효 친화도를 다시 계산해 우리 값을 덮는다 */
}
EXPORT_SYMBOL_GPL(irq_chip_redirect_set_affinity);	/* [한국어] 디먹스 컨트롤러 드라이버가 자식 칩의 콜백 자리에 꽂는다 */
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/**
 * irq_chip_compose_msi_msg - Compose msi message for a irq chip
 * @data:	Pointer to interrupt specific data
 * @msg:	Pointer to the MSI message
 *
 * For hierarchical domains we find the first chip in the hierarchy
 * which implements the irq_compose_msi_msg callback. For non
 * hierarchical we use the top level chip.
 */
/*
 * [한국어]
 * irq_chip_compose_msi_msg - MSI 메시지를 조립한다
 *
 * @data: 대상 irq_data
 * @msg:  조립 결과를 담을 곳 (출력)
 * @return: 0 성공, -ENOSYS 조립할 수 있는 층이 없음
 *
 * MSI 메시지란: 장치가 인터럽트를 보낼 때 쓰는 주소와 데이터 쌍이다.
 * 장치가 그 주소에 그 값을 쓰면 인터럽트 컨트롤러가 그것을 받아
 * 해당 CPU 에 인터럽트를 올린다. 즉 이 두 값이 "어느 CPU 의 어느
 * 벡터로 갈 것인가" 를 인코딩한다.
 *
 * 누가 그것을 아는가: 계층의 안쪽 층이다. 벡터 도메인이 CPU 와 벡터
 * 번호를 알고, 리매핑 하드웨어가 있으면 그것이 중간 주소를 안다.
 * 그래서 계층을 훑어 조립할 수 있는 층을 찾는다.
 *
 * 루프가 특이하다. pos 를 찾은 뒤에도 멈추지 않고 계속 내려가면서
 * 더 안쪽 층이 조립 콜백을 가지면 그것으로 덮는다. 즉 "가장 안쪽에서
 * 조립할 수 있는 층" 을 고른다. 원본 주석은 "첫 번째" 라고 하지만
 * 실제 동작은 마지막이다 — 계층의 안쪽일수록 실제 하드웨어에 가깝고
 * 정확한 값을 알기 때문이다.
 *
 * 루프 조건이 `!pos && data` 인데도 pos 가 갱신되는 것에 주목하면,
 * 사실 첫 번째를 찾으면 멈춘다. 바깥에서 안쪽으로 내려가므로 "가장
 * 바깥에서 조립 가능한 층" 이 선택된다. MSI 도메인 자신이 대개 그
 * 층이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, MSI 설정 경로.
 *
 * 호출 체인:
 *   msi_domain_activate() (kernel/irq/msi.c) → [이 함수] →
 *   chip->irq_compose_msi_msg()
 */
int irq_chip_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_data *pos;	/* [한국어] 조립을 담당할 층 */

	for (pos = NULL; !pos && data; data = irqd_get_parent_data(data)) {	/* [한국어] 바깥에서 안쪽으로 내려가며 찾는다. pos 가 정해지면 조건에서 멈춘다 */
		if (data->chip && data->chip->irq_compose_msi_msg)	/* [한국어] 이 층이 메시지를 조립할 수 있는가 */
			pos = data;	/* [한국어] 담당 층으로 기록. 루프 조건이 다음 반복을 막는다 */
	}

	if (!pos)	/* [한국어] 조립할 수 있는 층이 하나도 없는가 */
		return -ENOSYS;	/* [한국어] MSI 를 쓸 수 없는 계층이다. 호출자가 요청을 거절한다 */

	pos->chip->irq_compose_msi_msg(pos, msg);	/* [한국어] 주소와 데이터를 채운다. 이 값을 나중에 장치의 MSI 레지스터에 써 넣는다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_get_pm_device - 이 인터럽트의 전원 관리 대상 장치를 찾는다
 *
 * @data: 대상 irq_data
 * @return: 도메인이 등록해 둔 pm_dev, 없으면 NULL
 *
 * 인터럽트 컨트롤러도 장치이므로 런타임 전원 관리의 대상이다. 아무도
 * 쓰지 않으면 클록을 끄고 절전에 들어갈 수 있다. 그런데 그 인터럽트를
 * 쓰는 동안에는 깨어 있어야 한다.
 *
 * 그 연결을 도메인이 들고 있다. 컨트롤러 드라이버가 도메인을 만들 때
 * 자기 struct device 를 pm_dev 에 등록해 두면, 아래 두 함수가 그것을
 * 찾아 참조 카운트를 올리고 내린다.
 *
 * 도메인이 없으면 NULL 이다 — 도메인을 쓰지 않는 옛 방식 컨트롤러는
 * 이 기구의 대상이 아니다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   irq_chip_pm_get() / irq_chip_pm_put() → [이 함수]
 */
static struct device *irq_get_pm_device(struct irq_data *data)
{
	if (data->domain)	/* [한국어] 도메인 기반 인터럽트인가 */
		return data->domain->pm_dev;	/* [한국어] 컨트롤러 드라이버가 도메인 생성 때 등록해 둔 장치. NULL 일 수도 있다 */

	return NULL;	/* [한국어] 도메인을 쓰지 않는 옛 방식은 이 기구의 대상이 아니다 */
}

/**
 * irq_chip_pm_get - Enable power for an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Enable the power to the IRQ chip referenced by the interrupt data
 * structure.
 */
/*
 * [한국어]
 * irq_chip_pm_get - 인터럽트 컨트롤러의 전원을 켜고 참조를 잡는다
 *
 * @data: 대상 irq_data
 * @return: 0 성공 또는 대상 없음, 음수 전원을 켜지 못함
 *
 * 인터럽트를 쓰기 시작할 때 컨트롤러가 절전에 들어가지 않게 붙잡는다.
 * request_irq() 경로와 체인 처리기 설치 경로가 부른다.
 *
 * pm_runtime_resume_and_get() 은 두 가지를 함께 한다 — 참조 카운트를
 * 올리고, 절전 중이었다면 깨운다. 그 두 단계 사이에 경쟁이 없도록
 * 묶어 둔 함수다. 실패하면 참조도 원상 복구하므로 호출자가 따로
 * 되돌릴 필요가 없다.
 *
 * CONFIG_PM 검사를 IS_ENABLED 로 하는 것에 주목: #ifdef 가 아니라
 * 일반 if 문이다. 그래야 두 경우 모두 컴파일러의 문법 검사를 받는다.
 * 최적화 단계에서 상수 조건이 제거되므로 코드 크기는 같다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 전원을 켜는 동안 잠들 수 있다.
 *
 * 호출 체인:
 *   __setup_irq() / __irq_do_set_handler() → [이 함수] →
 *   pm_runtime_resume_and_get()
 */
int irq_chip_pm_get(struct irq_data *data)
{
	struct device *dev = irq_get_pm_device(data);	/* [한국어] 도메인이 등록해 둔 컨트롤러 장치 */
	int retval = 0;	/* [한국어] 대상이 없으면 성공으로 본다 */

	if (IS_ENABLED(CONFIG_PM) && dev)	/* [한국어] 런타임 PM 이 있는 빌드이고 대상 장치가 있는가. #ifdef 가 아니라 if 인 것은 두 경우 모두 문법 검사를 받게 하려는 것이다 */
		retval = pm_runtime_resume_and_get(dev);	/* [한국어] 참조를 올리고 절전 중이면 깨운다. 실패하면 참조도 원상 복구해 주므로 되돌릴 필요가 없다 */

	return retval;	/* [한국어] 0 이 아니면 호출자가 요청 전체를 실패시킨다 */
}

/**
 * irq_chip_pm_put - Drop a PM reference on an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Drop a power management reference, acquired via irq_chip_pm_get(), on the IRQ
 * chip represented by the interrupt data structure.
 *
 * Note that this will not disable power to the IRQ chip until this function
 * has been called for all IRQs that have called irq_chip_pm_get() and it may
 * not disable power at all (if user space prevents that, for example).
 */
/*
 * [한국어]
 * irq_chip_pm_put - 인터럽트 컨트롤러의 전원 참조를 놓는다
 *
 * @data: 대상 irq_data
 * @return: 없음
 *
 * 위 get 의 짝이다. free_irq() 경로와 체인 처리기 제거 경로가 부른다.
 *
 * 원본 주석이 짚는 두 가지가 중요하다. 첫째, 이 함수를 불렀다고
 * 곧바로 전원이 꺼지지는 않는다 — 같은 컨트롤러의 다른 인터럽트들이
 * 아직 참조를 잡고 있을 수 있다. 참조 카운트가 0 이 되어야 절전
 * 후보가 된다.
 *
 * 둘째, 0 이 되어도 꺼지지 않을 수 있다. 사용자가 sysfs 로 런타임
 * PM 을 막아 두었거나, 다른 이유로 그 장치가 깨어 있어야 할 수 있다.
 * 이 함수는 "이제 나는 필요 없다" 를 알릴 뿐 결정은 PM 코어가 한다.
 *
 * IS_ENABLED(CONFIG_PM) 검사가 없는 것에 주목: get 과 달리 여기서는
 * dev 검사만 한다. CONFIG_PM 이 꺼져 있으면 pm_runtime_put 자체가
 * 빈 인라인이 되어 아무 일도 하지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __free_irq() / __irq_do_set_handler() → [이 함수] → pm_runtime_put()
 */
void irq_chip_pm_put(struct irq_data *data)
{
	struct device *dev = irq_get_pm_device(data);	/* [한국어] 컨트롤러 장치 */

	if (dev)	/* [한국어] 대상이 있는가. CONFIG_PM 검사가 없는 것은 pm_runtime_put 이 그 빌드에서 빈 인라인이라서다 */
		pm_runtime_put(dev);	/* [한국어] 참조를 놓는다. 0 이 되어도 실제로 꺼질지는 PM 코어가 결정한다 */
}
