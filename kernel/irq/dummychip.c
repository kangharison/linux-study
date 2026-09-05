// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the dummy interrupt chip implementation
 */
/*
 * [한국어 설명] 아무 일도 하지 않는 두 개의 인터럽트 컨트롤러 (dummychip.c)
 *
 * === 파일의 역할 ===
 * 실제 하드웨어가 없는 자리를 채우는 두 개의 struct irq_chip 을 제공한다.
 * 콜백이 모두 빈 함수이거나 오류 보고이며, 실제로 레지스터를 건드리는 코드는
 * 한 줄도 없다.
 *
 * 왜 이런 것이 필요한가: irq 코어는 모든 서술자에 chip 이 있다고 가정한다.
 * 흐름 제어 핸들러가 chip->irq_mask() 를 부르고, 시작·종료 경로가
 * chip->irq_startup() 을 부른다. chip 이 NULL 이면 그 모든 자리에 NULL
 * 검사를 넣어야 하는데, 그러면 핫패스가 지저분해지고 검사를 빠뜨린 곳에서
 * 널 역참조가 난다.
 *
 * 대신 "아무 일도 하지 않는 chip" 을 하나 두고 그것을 기본값으로 삼으면,
 * 코어는 chip 이 언제나 있다고 믿고 무조건 부를 수 있다. 널 객체 패턴이다.
 *
 * 두 개를 두는 이유는 성격이 다르기 때문이다:
 *   no_irq_chip    — "여기에는 컨트롤러가 없다". 인터럽트가 오면 오류다.
 *   dummy_irq_chip — "컨트롤러가 필요 없다". 인터럽트가 와도 정상이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 서술자가 만들어질 때의 기본 상태에 있다:
 *
 *   alloc_desc() (irqdesc.c)
 *     ↓ desc->irq_data.chip = &no_irq_chip
 *   서술자가 no_irq_chip 을 가리킨 채로 존재
 *     ↓ 컨트롤러 드라이버가 irq_set_chip() 으로 진짜 chip 을 꽂으면
 *   정상 동작
 *     ↓ 꽂히지 않은 채 인터럽트가 오면
 *   handle_bad_irq() → chip->irq_ack = ack_bad → print_irq_desc()
 *
 * dummy_irq_chip 은 그 흐름과 별개로, 소프트웨어만으로 인터럽트를 흉내 내는
 * 곳(irq_sim.c, 일부 GPIO 확장기)이 명시적으로 골라 쓴다.
 *
 * 실행 컨텍스트: 콜백들은 인터럽트 문맥에서 불린다. 모두 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h 의 print_irq_desc() — 잘못된 인터럽트를 덤프한다.
 *   아키텍처의 ack_bad_irq()        — 그 아키텍처의 방식으로 오류를 알린다.
 *
 * 이 파일에 의존하는 곳:
 *   irqdesc.c   — 서술자 초기값으로 no_irq_chip 을 쓴다.
 *   irq_sim.c   — 모의 인터럽트에 dummy_irq_chip 을 쓴다.
 *   여러 GPIO/MFD 드라이버 — 자기 인터럽트에 dummy_irq_chip 을 쓴다.
 *     그래서 dummy_irq_chip 만 EXPORT_SYMBOL_GPL 되어 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * ack_bad()        — 잘못된 인터럽트가 왔음을 덤프하고 아키텍처에 알린다.
 * noop()           — 아무 일도 하지 않는다. 반환값이 없는 콜백들에 쓴다.
 * noop_ret()       — 아무 일도 하지 않고 0 을 돌려준다. irq_startup 용이다.
 * no_irq_chip      — 컨트롤러가 없는 자리의 표식. ack 가 오류를 보고한다.
 * dummy_irq_chip   — 컨트롤러가 필요 없는 자리. ack 도 mask 도 조용히 성공한다.
 *
 * 두 chip 의 결정적 차이는 .irq_ack 와 mask/unmask 의 유무다. 그 차이가
 * 각 구조체 주석에 정리되어 있다.
 */
#include <linux/interrupt.h>	/* [한국어] 인터럽트 공개 API */
#include <linux/irq.h>	/* [한국어] struct irq_chip 정의와 IRQCHIP_* 플래그 */
#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL_GPL — dummy_irq_chip 을 모듈에 공개한다 */

#include "internals.h"	/* [한국어] print_irq_desc() 와 irq_data_to_desc(). 코어 내부라 공개 헤더에 없다 */

/*
 * What should we do if we get a hw irq event on an illegal vector?
 * Each architecture has to answer this themselves.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * ack_bad - 컨트롤러가 없는 번호로 인터럽트가 왔을 때의 처리
 *
 * @data: 그 인터럽트의 irq_data.
 *
 * 무슨 상황인가: no_irq_chip 이 붙어 있다는 것은 그 번호에 실제 컨트롤러가
 * 꽂히지 않았다는 뜻이다. 그런데 인터럽트가 들어왔다. 원인은 대개 셋 중 하나다.
 *   - 펌웨어나 부트로더가 남긴 인터럽트 설정이 살아 있다.
 *   - 하드웨어가 잘못된 벡터를 보냈다.
 *   - 커널의 인터럽트 번호 매핑이 어긋났다.
 *
 * 무엇을 하는가: 두 단계다. 먼저 서술자 전체를 덤프해 진단 정보를 남기고,
 * 그 다음 아키텍처의 ack_bad_irq() 를 부른다.
 *
 * 왜 아키텍처에 넘기는가: 영어 주석이 말하듯 "각 아키텍처가 스스로 답해야
 * 할 문제"다. x86 은 APIC 에 EOI 를 보내야 다음 인터럽트가 막히지 않고,
 * 어떤 아키텍처는 단순히 경고만 찍는다. 공통 코드가 정할 수 없다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들지 않는다. print_irq_desc 의 ratelimit
 * 이 여기서 중요해진다 — 잘못된 인터럽트는 대개 폭주하기 때문이다.
 *
 * 호출 체인:
 *   handle_bad_irq() → desc->irq_data.chip->irq_ack → [ack_bad]
 *     → print_irq_desc() (kernel/irq/debug.h)
 *     → ack_bad_irq() (아키텍처별)
 */
static void ack_bad(struct irq_data *data)
{
	struct irq_desc *desc = irq_data_to_desc(data);	/* [한국어] irq_data 에서 그것을 품은 서술자로 되짚는다. 덤프에 서술자 전체가 필요하다 */

	print_irq_desc(data->irq, desc);	/* [한국어] 서술자를 통째로 콘솔에 찍는다. 자체 ratelimit 이 있어 폭주해도 안전하다 */
	ack_bad_irq(data->irq);	/* [한국어] 아키텍처마다 다른 뒷처리. x86 은 APIC 에 EOI 를 보내고, 그러지 않으면 같은 우선순위의 인터럽트가 영원히 막힌다 */
}

/*
 * NOP functions
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * noop - 아무 일도 하지 않는 콜백
 *
 * @data: 대상 irq_data. 쓰이지 않는다.
 *
 * 반환값이 없는 irq_chip 콜백들(mask, unmask, enable, disable, shutdown, ack)의
 * 자리를 채운다.
 *
 * 왜 NULL 대신 이것을 두는가: 코어의 어떤 자리는 콜백이 NULL 인지 확인하고
 * 부르지만, 어떤 자리는 확인 없이 부른다. 널 객체를 두면 그 구분을 신경 쓸
 * 필요가 없어진다. 또 "이 콜백이 없다"와 "이 콜백은 할 일이 없다"는 뜻이
 * 다른데, 후자를 표현하려면 실제 함수가 있어야 한다.
 *
 * 예를 들어 dummy_irq_chip 의 irq_mask 가 NULL 이면 코어는 "마스크할 수
 * 없는 인터럽트"로 보고 다르게 동작한다. noop 을 꽂아 두면 "마스크했다"고
 * 여기고 정상 경로로 간다 — 소프트웨어 인터럽트에서는 그것이 맞는 동작이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 또는 프로세스 문맥. 어디서든 안전하다.
 *
 * 호출 체인:
 *   mask_irq()/unmask_irq()/irq_enable() 등 → chip 콜백 → [noop]
 */
static void noop(struct irq_data *data) { }

/*
 * [한국어]
 * noop_ret - 아무 일도 하지 않고 성공을 돌려주는 콜백
 *
 * @data:   대상 irq_data. 쓰이지 않는다.
 * @return: 항상 0.
 *
 * 위 noop 의 반환값 있는 판이다. irq_chip 의 .irq_startup 이 unsigned int 를
 * 돌려주게 되어 있어, 그 자리에는 이것을 꽂는다.
 *
 * 0 이 무슨 뜻인가: irq_startup 콜백의 반환값은 오류 코드가 아니라 "이
 * 인터럽트가 이미 대기 중이었는가"를 뜻한다. 0 이면 밀린 것이 없다는 뜻이고,
 * 0 이 아니면 코어가 그 인터럽트를 곧바로 재전송한다.
 *
 * 실제 하드웨어가 없으니 밀려 있을 인터럽트도 없다. 그래서 0 이 맞는 답이다.
 *
 * 이 반환값의 뜻을 오해해 "성공"으로 읽으면, 0 이 아닌 값을 실패로 착각하게
 * 된다. irq_chip 콜백 중 이런 규약을 갖는 것은 startup 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(요청 경로). 잠들지 않는다.
 *
 * 호출 체인:
 *   irq_startup() → chip->irq_startup → [noop_ret]
 */
static unsigned int noop_ret(struct irq_data *data)
{
	return 0;	/* [한국어] "밀려 있던 인터럽트 없음". 오류 코드가 아니라 재전송 필요 여부다 */
}

/*
 * Generic no controller implementation
 */
/* [한국어] (위 영어 주석에 이어) 컨트롤러가 없는 자리를 나타내는 chip.
 *
 * 서술자가 만들어질 때의 기본값이며, 그 번호에 아직 아무 컨트롤러도 꽂히지
 * 않았음을 뜻한다. 아래 dummy_irq_chip 과 결정적으로 다른 점이 둘 있다.
 *
 *   .irq_ack 가 ack_bad 다 — 인터럽트가 들어오면 오류로 보고한다. 컨트롤러가
 *     없는 번호로 인터럽트가 오는 것은 정상이 아니기 때문이다.
 *   mask/unmask 가 없다 — 마스크할 하드웨어가 없으므로 콜백도 두지 않는다.
 *     코어는 이것을 "마스크할 수 없는 인터럽트"로 보고 그에 맞게 동작한다.
 *
 * EXPORT 되지 않는다는 점도 다르다. 이것은 코어의 내부 기본값이지 드라이버가
 * 골라 쓸 물건이 아니다. */
struct irq_chip no_irq_chip = {
	.name		= "none",
	/* [한국어] /proc/interrupts 와 debugfs 에 표시될 이름.
	 * 설정자: 이 정적 초기화뿐.
	 * 읽는 자: proc.c 의 출력, debugfs 의 chip 정보.
	 * 값이 "none" 인 것이 진단에 중요하다 — /proc/interrupts 에서 chip 열이
	 *   none 으로 보이면 그 번호에 컨트롤러가 꽂히지 않았다는 뜻이다. */

	.irq_startup	= noop_ret,
	/* [한국어] 인터럽트를 시작할 때의 콜백. 할 일이 없어 0 을 돌려준다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: irq_startup() (kernel/irq/chip.c).
	 * 0 의 뜻: 밀려 있던 인터럽트가 없다 — 재전송할 필요가 없다는 뜻이다. */

	.irq_shutdown	= noop,
	/* [한국어] 인터럽트를 끌 때의 콜백. 끌 하드웨어가 없다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: irq_shutdown() (kernel/irq/chip.c).
	 * 이 콜백이 있어야 kexec 경로의 irq_shutdown() 이 널 검사 없이 돌 수 있다. */

	.irq_enable	= noop,
	/* [한국어] 인터럽트를 켤 때의 콜백. 할 일이 없다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: irq_enable() (kernel/irq/chip.c).
	 * enable 과 아래 disable 을 둘 다 두는 이유: 하나만 있으면 코어가
	 *   나머지를 mask/unmask 로 대신하려 하는데, 이 chip 에는 그것도 없다. */

	.irq_disable	= noop,
	/* [한국어] 인터럽트를 끌 때의 콜백. 위 enable 의 짝이다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: irq_disable() (kernel/irq/chip.c). */

	.irq_ack	= ack_bad,
	/* [한국어] 인터럽트를 받았을 때의 콜백 — 이 chip 의 핵심이다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: 흐름 제어 핸들러가 인터럽트 도착을 컨트롤러에 알릴 때.
	 * 왜 noop 이 아닌가: 컨트롤러가 꽂히지 않은 번호로 인터럽트가 오는 것은
	 *   설정 오류이거나 하드웨어 오동작이다. 조용히 넘기면 원인을 알 수 없어,
	 *   서술자를 덤프하고 아키텍처에 알린다.
	 * 아래 dummy_irq_chip 이 여기에 noop 을 두는 것과 대비되는 지점이다. */

	.flags		= IRQCHIP_SKIP_SET_WAKE,
	/* [한국어] 이 chip 의 성질 플래그.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: irq_set_irq_wake() 가 깨우기 설정을 시도할지 정할 때.
	 * SKIP_SET_WAKE 의 뜻: 이 chip 에는 irq_set_wake 콜백이 없지만, 그것을
	 *   오류로 다루지 말라는 표시다. 이 플래그가 없으면 깨우기를 요청한
	 *   드라이버가 -ENXIO 를 받는다.
	 * 왜 그것이 맞는가: 컨트롤러가 없으면 깨우기라는 개념도 없다. 요청을
	 *   거절하는 것보다 조용히 받아들이는 편이 상위 코드를 단순하게 한다. */
};

/*
 * Generic dummy implementation which can be used for
 * real dumb interrupt sources
 */
/* [한국어] (위 영어 주석에 이어) 컨트롤러가 필요 없는 자리를 위한 chip.
 *
 * 영어 주석의 "real dumb interrupt sources" 가 무엇인가: 마스크할 수도,
 * ack 를 보낼 수도 없는 아주 단순한 인터럽트 원천이다. 소프트웨어로 흉내
 * 낸 인터럽트(irq_sim.c), 또는 부모가 모든 처리를 대신해 주는 자식 인터럽트가
 * 여기 해당한다.
 *
 * 위 no_irq_chip 과 무엇이 다른가:
 *   .irq_ack 가 noop 이다 — 인터럽트가 오는 것이 정상이므로 오류로 보지 않는다.
 *   mask/unmask 가 있다 — 실제로 마스크하지는 않지만, 코어에게 "마스크할 수
 *     있는 인터럽트"로 보이게 한다. 그래야 흐름 제어가 정상 경로를 탄다.
 *
 * 두 번째 차이가 특히 중요하다. mask 콜백이 없으면 코어는 레벨 트리거
 * 인터럽트를 안전하게 다룰 수 없다고 판단해 다르게 동작한다. noop 을
 * 꽂아 두면 "마스크했다"고 여기고 보통의 흐름을 따른다.
 *
 * 아래 EXPORT_SYMBOL_GPL 이 붙는 것도 no_irq_chip 과 다른 점이다 —
 * 이쪽은 드라이버가 골라 쓰는 물건이다. */
struct irq_chip dummy_irq_chip = {
	.name		= "dummy",
	/* [한국어] /proc/interrupts 등에 표시될 이름.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: proc.c 의 출력.
	 * 진단할 때 chip 열이 dummy 로 보이면 그 인터럽트는 실제 하드웨어
	 *   컨트롤러 없이 소프트웨어로만 다뤄진다는 뜻이다. */

	.irq_startup	= noop_ret,
	/* [한국어] 시작 콜백. 위 no_irq_chip 과 같다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_startup().
	 * 0 은 "밀려 있던 인터럽트 없음"이다. */

	.irq_shutdown	= noop,
	/* [한국어] 종료 콜백. 끌 하드웨어가 없다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_shutdown(). */

	.irq_enable	= noop,
	/* [한국어] 활성화 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_enable(). */

	.irq_disable	= noop,
	/* [한국어] 비활성화 콜백. 위 enable 의 짝.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_disable(). */

	.irq_ack	= noop,
	/* [한국어] 인터럽트 도착 알림 콜백 — no_irq_chip 과 갈리는 첫 지점이다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: 흐름 제어 핸들러.
	 * 왜 ack_bad 가 아닌가: 이 chip 을 쓰는 인터럽트는 실제로 발생하는 것이
	 *   정상이다. 소프트웨어가 일부러 일으키거나 부모가 전달해 준다. 오류로
	 *   보고하면 정상 동작마다 콘솔이 넘친다. */

	.irq_mask	= noop,
	/* [한국어] 마스크 콜백 — no_irq_chip 에는 없는 것이다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: mask_irq() (kernel/irq/chip.c).
	 * 왜 있어야 하는가: 이 콜백이 NULL 이면 코어는 "마스크할 수 없는
	 *   인터럽트"로 판단해 레벨 트리거 흐름 제어를 안전하게 쓸 수 없다.
	 *   실제로 하는 일이 없더라도 존재 자체가 코어에게 신호가 된다.
	 * 실제 마스킹은 누가 하는가: 이 chip 을 쓰는 쪽이 소프트웨어적으로
	 *   인터럽트 발생 자체를 억제한다. 코어의 마스크 요청은 상태 기록으로만 남는다. */

	.irq_unmask	= noop,
	/* [한국어] 언마스크 콜백. 위 mask 의 짝이다.
	 * 설정자: 이 정적 초기화. 읽는 자: unmask_irq().
	 * 짝을 반드시 함께 두어야 한다 — 한쪽만 있으면 코어가 마스크한 뒤
	 *   되돌릴 방법이 없다고 판단한다. */

	.flags		= IRQCHIP_SKIP_SET_WAKE,
	/* [한국어] 성질 플래그. 위 no_irq_chip 과 같은 값이다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_set_irq_wake().
	 * 뜻: irq_set_wake 콜백이 없어도 깨우기 요청을 오류로 다루지 말라.
	 * 소프트웨어 인터럽트는 시스템을 깨울 수 없지만, 요청을 거절하면
	 *   그것을 쓰는 드라이버가 불필요하게 복잡해진다. */
};
EXPORT_SYMBOL_GPL(dummy_irq_chip);	/* [한국어] 모듈에 공개한다. GPIO 확장기나 MFD 드라이버가 자기 자식 인터럽트에 이 chip 을 쓴다. 위 no_irq_chip 은 코어 내부 기본값이라 공개하지 않는다 */
