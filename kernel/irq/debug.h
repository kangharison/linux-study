/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Debugging printout:
 */
/*
 * [한국어 설명] 인터럽트 서술자 덤프 매크로 (debug.h)
 *
 * === 파일의 역할 ===
 * 인터럽트에 문제가 생겼을 때 서술자의 내용을 통째로 콘솔에 찍는 진단
 * 코드다. 함수 하나와 그것을 짧게 쓰기 위한 매크로 셋으로 이루어져 있다.
 *
 * 언제 쓰이는가: 이 파일의 print_irq_desc() 를 부르는 곳은 dummychip.c 의
 * ack_bad() 하나뿐이다. 존재하지 않는 벡터로 인터럽트가 들어왔을 때,
 * 즉 "일어나서는 안 되는 일"이 일어났을 때의 사후 분석용이다.
 *
 * 그래서 이 코드는 정상 동작 중에는 한 번도 실행되지 않는다. 성능을 신경
 * 쓸 이유가 없고, 대신 사람이 읽을 수 있는 형태로 최대한 많은 정보를 찍는 것이
 * 목적이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진단 계층의 가장 아래에 있다:
 *
 *   존재하지 않는 벡터로 인터럽트 도착
 *     ↓
 *   no_irq_chip 의 .irq_ack = ack_bad   (dummychip.c)
 *     ↓
 *   print_irq_desc()                    ← **이 파일**
 *     ↓
 *   ack_bad_irq() (아키텍처별 — 대개 경고를 찍고 넘어간다)
 *
 * 실행 컨텍스트: 인터럽트 문맥. printk 를 쓰지만 그것은 인터럽트 문맥에서도
 * 안전하다. 다만 아래 ratelimit 이 있는 이유가 바로 이 문맥 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 포함되는 곳: internals.h 가 settings.h 보다 먼저 이 파일을 포함한다.
 *   순서가 중요한데, 이 파일의 매크로들이 IRQ_LEVEL 같은 공개 이름을 쓰기
 *   때문이다 — settings.h 가 그 이름들을 봉인한 뒤에는 쓸 수 없다.
 *
 * 의존하는 것: printk, ratelimit, 그리고 struct irq_desc 의 필드들.
 *   include 를 하나도 하지 않는데, internals.h 가 이미 필요한 것을 들여온
 *   상태에서 포함되기 때문이다. 단독으로는 컴파일되지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * ___P(f)   — status_use_accessors 의 비트 f 가 서 있으면 그 이름을 찍는다.
 * ___PS(f)  — istate 의 비트 f 에 대해 같은 일을 한다.
 * ___PD(f)  — 아무 일도 하지 않는 자리 표시. 위 영어 주석의 FIXME 참고.
 * print_irq_desc() — 서술자 하나를 통째로 덤프한다. 위 세 매크로를 쓴다.
 *
 * 파일 끝에서 세 매크로를 모두 #undef 하는 것이 이 파일의 규율이다 —
 * ___P 같은 짧고 일반적인 이름이 다른 파일로 새 나가지 않게 한다.
 */

/* [한국어] 설정 비트 하나를 검사해 서 있으면 이름을 찍는 매크로.
 *
 * #f 가 전처리기의 문자열화다. ___P(IRQ_LEVEL) 이라고 쓰면 비트를 검사하는
 * 코드와 "IRQ_LEVEL" 이라는 문자열이 함께 만들어져, 이름과 출력이 어긋날 수 없다.
 *
 * %14s 로 폭을 맞추는 이유: 여러 줄이 이어질 때 이름 오른쪽의 "set" 이
 * 세로로 정렬되어 눈으로 훑기 좋다.
 *
 * 중괄호가 없는 if 라는 점에 주의한다. 아래 호출부처럼 한 줄에 하나씩
 * 쓰는 한 문제가 없지만, else 를 붙이거나 다른 if 안에 넣으면 의도치 않게
 * 결합한다. 이 파일 안에서만 쓰고 곧바로 #undef 하는 것이 그 위험을 막는다. */
#define ___P(f) if (desc->status_use_accessors & f) printk("%14s set\n", #f)
/* [한국어] 위와 같지만 istate(코어 내부 상태)를 검사하는 판.
 *
 * 두 매크로가 따로 있는 이유: 두 상태가 서로 다른 워드에 있기 때문이다.
 * status_use_accessors 는 설정(_IRQ_*)이고 istate 는 처리 상태(IRQS_*)다.
 * 하나의 매크로에 워드를 인자로 받게 할 수도 있었겠지만, 호출부가 길어져
 * 아래의 목록이 읽기 어려워진다. */
#define ___PS(f) if (desc->istate & f) printk("%14s set\n", #f)
/* FIXME */
/* [한국어] (위 영어 주석의 FIXME) 아무 일도 하지 않는 자리 표시.
 *
 * 무슨 사연인가: 아래 호출부에 IRQS_INPROGRESS, IRQS_DISABLED, IRQS_MASKED
 * 세 개가 남아 있는데, 그 상태들은 오래전에 istate 에서 irq_data 의
 * IRQD_* 로 옮겨 갔다. 그래서 지금은 존재하지 않는 이름이고, 그대로 두면
 * 컴파일이 되지 않는다.
 *
 * 호출부를 지우는 대신 매크로를 빈 것으로 만들어 둔 것은, 그 상태들도
 * 찍어야 한다는 사실을 잊지 않기 위해서다. FIXME 가 그 의도를 말한다.
 *
 * do { } while (0) 형태인 이유: 위 두 매크로처럼 문장 자리에 쓰이므로,
 * 세미콜론을 붙였을 때 문법이 맞아야 한다. 빈 매크로로 두면 if 문 뒤에
 * 왔을 때 문제가 생길 수 있다. */
#define ___PD(f) do { } while (0)

/*
 * [한국어]
 * print_irq_desc - 인터럽트 서술자의 내용을 콘솔에 통째로 찍는다
 *
 * @irq:  그 인터럽트 번호. 서술자에서도 얻을 수 있지만 호출부가 이미
 *        들고 있어 그대로 받는다.
 * @desc: 덤프할 서술자.
 *
 * 언제 불리는가: 존재하지 않는 벡터로 인터럽트가 들어왔을 때뿐이다
 * (dummychip.c 의 ack_bad). 정상 동작에서는 실행되지 않는다.
 *
 * 무엇을 찍는가, 순서대로:
 *   1. 번호, 서술자 주소, 비활성 깊이, 인터럽트 횟수, 처리되지 않은 횟수
 *   2. 흐름 제어 핸들러의 주소와 심볼 이름
 *   3. 담당 chip 의 주소와 심볼 이름
 *   4. action 목록의 첫 항목, 그리고 그 핸들러
 *   5. 설정 비트들 (___P)
 *   6. 코어 상태 비트들 (___PS)
 *
 * %pS 형식이 중요하다. 주소를 심볼 이름으로 바꿔 찍어 주므로, 어느 드라이버의
 * 어느 함수인지 곧바로 알 수 있다. 주소(%p)도 함께 찍는 것은 심볼이 없는
 * 모듈 함수일 때를 위해서다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   존재하지 않는 벡터로 인터럽트 도착 → ack_bad() (kernel/irq/dummychip.c)
 *     → [print_irq_desc] → ack_bad_irq() (아키텍처별)
 */
static inline void print_irq_desc(unsigned int irq, struct irq_desc *desc)
{
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 5);	/* [한국어] 5초에 5번으로 출력을 제한한다. static 이라 호출 사이에 상태가 유지된다 */

	if (!__ratelimit(&ratelimit))	/* [한국어] 한도를 넘었으면 이번에는 찍지 않는다 */
		return;	/* [한국어] 이 제한이 없으면 잘못된 인터럽트가 초당 수천 번 들어올 때 printk 가 시스템을 멈춘다 — 진단하려던 문제보다 더 큰 문제가 된다 */

	printk("irq %d, desc: %p, depth: %d, count: %d, unhandled: %d\n",	/* [한국어] 첫 줄: 번호와 서술자 주소, 그리고 세 개의 카운터 */
		irq, desc, desc->depth, desc->irq_count, desc->irqs_unhandled);	/* [한국어] depth 는 비활성 중첩 깊이(0 이면 켜짐), irq_count 는 오탐 감지용 누적 횟수, irqs_unhandled 는 그중 아무도 처리하지 않은 횟수 */
	printk("->handle_irq():  %p, %pS\n",	/* [한국어] 흐름 제어 핸들러. handle_level_irq 인지 handle_edge_irq 인지가 여기서 드러난다 */
		desc->handle_irq, desc->handle_irq);	/* [한국어] 같은 값을 두 번 넘긴다 — %p 는 주소로, %pS 는 심볼 이름으로 찍는다 */
	printk("->irq_data.chip(): %p, %pS\n",	/* [한국어] 담당 컨트롤러. 여기가 no_irq_chip 이면 그 번호에 컨트롤러가 붙지 않았다는 뜻이다 */
		desc->irq_data.chip, desc->irq_data.chip);	/* [한국어] 역시 주소와 심볼 이름을 함께 */
	printk("->action(): %p\n", desc->action);	/* [한국어] 등록된 핸들러 목록의 첫 항목. NULL 이면 아무도 이 인터럽트를 요청하지 않았다는 뜻이라, 잘못된 인터럽트의 흔한 원인이다 */
	if (desc->action) {	/* [한국어] 핸들러가 있을 때만 그 안을 들여다본다 */
		printk("->action->handler(): %p, %pS\n",	/* [한국어] 첫 핸들러 함수. 어느 드라이버인지 심볼로 알 수 있다 */
			desc->action->handler, desc->action->handler);	/* [한국어] 공유 인터럽트면 next 로 더 있을 수 있지만 첫 항목만 찍는다 */
	}

	/* [한국어] 아래 여섯 줄이 설정 비트 덤프다. IRQ_* 공개 이름을 쓰는데,
	 * 이것이 이 헤더가 settings.h 보다 먼저 포함되어야 하는 이유다 —
	 * 그 파일이 이 이름들을 컴파일 불가 토큰으로 봉인하기 때문이다. */
	___P(IRQ_LEVEL);	/* [한국어] 레벨 트리거인가. 흐름 제어 핸들러와 일치하는지 대조할 수 있다 */
	___P(IRQ_PER_CPU);	/* [한국어] CPU 마다 따로인 인터럽트인가 */
	___P(IRQ_NOPROBE);	/* [한국어] 자동 탐지 제외 대상인가 */
	___P(IRQ_NOREQUEST);	/* [한국어] 요청이 막혀 있는가. 이것이 서 있는데 인터럽트가 왔다면 하드웨어나 펌웨어 설정이 잘못된 것이다 */
	___P(IRQ_NOTHREAD);	/* [한국어] 강제 스레드화 예외인가 */
	___P(IRQ_NOAUTOEN);	/* [한국어] 요청 뒤 자동으로 켜지 않는 인터럽트인가 */

	/* [한국어] 여기부터는 코어 처리 상태(istate) 덤프다. 위와 워드가 다르다. */
	___PS(IRQS_AUTODETECT);	/* [한국어] 자동 탐지 진행 중인가 */
	___PS(IRQS_REPLAY);	/* [한국어] 소프트웨어 재전송을 발행해 둔 상태인가 */
	___PS(IRQS_WAITING);	/* [한국어] 자동 탐지에서 아직 울리지 않은 상태인가 */
	___PS(IRQS_PENDING);	/* [한국어] 처리하지 못해 재전송이 필요한 인터럽트가 밀려 있는가 */

	/* [한국어] 아래 셋은 위 ___PD 가 빈 매크로라 아무것도 찍지 않는다.
	 * 그 상태들이 istate 에서 irq_data 의 IRQD_IRQ_INPROGRESS/DISABLED/MASKED
	 * 로 옮겨 갔기 때문이다. 호출을 남겨 둔 것은 언젠가 새 이름으로 되살리라는
	 * 표시이며, 위 ___PD 의 FIXME 가 그 뜻이다. */
	___PD(IRQS_INPROGRESS);	/* [한국어] 지금은 IRQD_IRQ_INPROGRESS 로 옮겨 갔다 */
	___PD(IRQS_DISABLED);	/* [한국어] 지금은 IRQD_IRQ_DISABLED */
	___PD(IRQS_MASKED);	/* [한국어] 지금은 IRQD_IRQ_MASKED */
}

/* [한국어] 세 매크로를 치운다.
 *
 * ___P 나 ___PS 같은 짧고 일반적인 이름이 이 헤더를 포함한 파일 전체로
 * 새 나가면, 다른 곳에서 우연히 같은 이름을 쓸 때 충돌한다. 쓰고 나서
 * 곧바로 없애는 것이 헤더에서 매크로를 정의할 때의 규율이다.
 *
 * internals.h 가 __irqd_to_state 를 쓰고 #undef 하는 것과 같은 방식이다. */
#undef ___P
#undef ___PS
#undef ___PD
