// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017-2018 Bartosz Golaszewski <brgl@bgdev.pl>
 * Copyright (C) 2020 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 */
/*
 * [한국어 설명] 소프트웨어로 인터럽트를 흉내 내는 시뮬레이터 (irq_sim.c)
 *
 * === 파일의 역할 ===
 * 실제 하드웨어 없이 인터럽트를 만들어 내는 가짜 인터럽트 컨트롤러다.
 * 사용자가 "이 인터럽트를 발생시켜라" 라고 요청하면, 그것이 진짜 인터럽트와
 * 구별되지 않는 경로로 드라이버 핸들러에 전달된다.
 *
 * 누가 쓰는가:
 *   gpio-sim, gpio-mockup — 가상 GPIO 칩의 인터럽트.
 *   각종 드라이버 시험 코드 — 인터럽트 처리 경로를 검증한다.
 *   iio 의 일부 트리거 — 소프트웨어로 샘플링을 촉발한다.
 *
 * 핵심 구조는 두 단계다. 인터럽트를 "발생" 시키면 대기 비트맵에 표시만
 * 하고 irq_work 를 예약한다. 실제 핸들러 호출은 그 irq_work 안에서 일어난다.
 *
 * 왜 곧바로 부르지 않는가: 발생을 요청하는 쪽은 임의의 문맥이다. 사용자
 * 공간에서 sysfs 를 통해 부를 수도 있고, 다른 드라이버가 프로세스 문맥에서
 * 부를 수도 있다. 그런 곳에서 인터럽트 핸들러를 직접 부르면 핸들러가
 * 기대하는 문맥(인터럽트가 꺼진 상태)과 어긋난다. irq_work 는 하드
 * 인터럽트 문맥에서 실행되므로 그 조건을 만족한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 가짜 컨트롤러로서 코어 아래에 붙는다:
 *
 *   설정:
 *     시험 코드나 gpio-sim
 *       ↓ irq_domain_create_sim()      ← **이 파일**
 *     irq_domain_create_linear() → 도메인 생성
 *       ↓ 드라이버가 request_irq()
 *     irq_sim_domain_map() 이 chip 과 흐름 제어를 꽂아 둔 상태
 *
 *   발생:
 *     시험 코드 → irq_set_irqchip_state(IRQCHIP_STATE_PENDING, true)
 *       ↓ chip->irq_set_irqchip_state
 *     irq_sim_set_irqchip_state()      ← **이 파일** — 비트를 세우고 irq_work 예약
 *       ↓ 나중에 하드 인터럽트 문맥에서
 *     irq_sim_handle_irq()             ← **이 파일**
 *       ↓ handle_simple_irq()
 *     드라이버 핸들러
 *
 * 실행 컨텍스트: 설정과 발생 요청은 프로세스 문맥, 실제 핸들러 호출은
 * irq_work 덕분에 하드 인터럽트 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   irqdomain.c — 도메인 생성과 hwirq↔virq 대응.
 *   chip.c 의 handle_simple_irq() — 가장 단순한 흐름 제어.
 *   irq_work 기반 — 임의 문맥에서 하드 인터럽트 문맥으로 넘어가는 수단.
 *   linux/cleanup.h 의 __free 속성 — 아래 create_sim_full 의 오류 처리에 쓴다.
 *
 * 이 파일에 의존하는 곳: gpio-sim, gpio-mockup, 여러 시험 모듈.
 *
 * === 주요 함수/구조체 요약 ===
 * struct irq_sim_work_ctx  — 시뮬레이터 하나 전체의 상태. 도메인, 대기 비트맵, irq_work.
 * struct irq_sim_irq_ctx   — 인터럽트 하나의 상태. 마스크 여부뿐이다.
 * irq_sim_irqmask/unmask() — 마스크 상태를 기록만 한다.
 * irq_sim_set_irqchip_state() — 인터럽트를 발생시키는 진입점.
 * irq_sim_handle_irq()     — irq_work 본체. 대기 비트를 훑어 핸들러를 부른다.
 * irq_sim_domain_map()     — 인터럽트 하나를 이 chip 에 연결한다.
 * irq_domain_create_sim_full() — 시뮬레이터를 만든다.
 * irq_domain_remove_sim()  — 없앤다.
 * devm_ 판들            — 위 둘의 장치 수명 관리 판.
 *
 * 이 파일에서 눈여겨볼 관용구가 하나 있다. create_sim_full() 이 커널의
 * __free/no_free_ptr 정리 기법을 써서, goto 사슬 없이 오류 처리를 한다.
 * 자세한 것은 그 함수의 주석에 있다.
 */

#include <linux/cleanup.h>	/* [한국어] __free(), no_free_ptr() — 아래 create_sim_full 의 자동 정리 기법 */
#include <linux/interrupt.h>	/* [한국어] 인터럽트 공개 API */
#include <linux/irq.h>	/* [한국어] struct irq_chip 과 irqd_* 접근자 */
#include <linux/irq_sim.h>	/* [한국어] 이 파일이 제공하는 API 의 선언과 struct irq_sim_ops */
#include <linux/irq_work.h>	/* [한국어] irq_work — 임의 문맥에서 하드 인터럽트 문맥으로 넘어가는 수단 */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree */

/* [한국어] 시뮬레이터 하나 전체의 상태.
 *
 * 도메인의 host_data 에 담겨, 이 도메인의 모든 인터럽트가 공유한다.
 * 아래 irq_sim_irq_ctx 가 인터럽트 하나씩의 상태라면 이쪽은 전체의 상태다. */
struct irq_sim_work_ctx {
	struct irq_work		work;
	/* [한국어] 실제 핸들러 호출을 하드 인터럽트 문맥으로 옮기는 수단.
	 * 설정자: create_sim_full 이 IRQ_WORK_INIT_HARD 로 초기화한다.
	 * 읽는 자: set_irqchip_state 가 irq_work_queue 로 예약하고, 커널이
	 *   나중에 irq_sim_handle_irq 를 부른다.
	 * 왜 HARD 판인가: 보통의 irq_work 는 softirq 문맥에서 실행될 수 있다.
	 *   인터럽트 핸들러는 하드 인터럽트 문맥을 기대하므로 그것으로는 부족하다.
	 *   HARD 판은 필요하면 자기 자신에게 IPI 를 보내서라도 그 문맥을 만든다.
	 * 동기화: irq_work 기반이 중복 예약을 알아서 걸러 낸다. */

	unsigned int		irq_count;
	/* [한국어] 이 시뮬레이터가 가진 인터럽트의 개수.
	 * 설정자: create_sim_full 이 요청받은 개수를 담는다.
	 * 읽는 자: handle_irq 가 비트맵을 훑을 범위로 쓴다.
	 * 값 범위: 1 이상. 생성 뒤에는 바뀌지 않는다.
	 * 왜 비트맵과 따로 두는가: bitmap 자료구조는 자기 크기를 기억하지
	 *   않는다. 훑을 때마다 상한을 함께 넘겨야 한다. */

	unsigned long		*pending;
	/* [한국어] 발생했지만 아직 핸들러가 불리지 않은 인터럽트들의 비트맵.
	 * 설정자: set_irqchip_state 가 assign_bit 으로 세우고, handle_irq 가
	 *   clear_bit 으로 지운다.
	 * 읽는 자: get_irqchip_state 가 상태를 물어볼 때, handle_irq 가 훑을 때.
	 * 값 범위: 비트 i 가 서 있으면 hwirq i 가 대기 중이다.
	 * 동기화: 원자적 비트 연산만 쓴다. 락이 없어도 되는 이유는 각 비트가
	 *   독립이고, 세우는 쪽과 지우는 쪽이 같은 비트를 두고 다투더라도
	 *   최악의 경우 인터럽트가 한 번 더 처리될 뿐이기 때문이다. */

	struct irq_domain	*domain;
	/* [한국어] 이 시뮬레이터의 인터럽트 도메인.
	 * 설정자: create_sim_full 이 만들어 담는다.
	 * 읽는 자: handle_irq 가 hwirq 를 리눅스 번호로 옮길 때, ops 콜백에
	 *   넘길 때.
	 * 값 범위: NULL 이 아니다. 생성에 실패하면 이 구조체 자체가 만들어지지 않는다.
	 * 순환 참조에 주의: 도메인의 host_data 가 이 구조체를 가리키고, 이
	 *   필드가 다시 도메인을 가리킨다. 해제 순서를 지켜야 한다. */

	struct irq_sim_ops	ops;
	/* [한국어] 사용자가 등록한 선택적 콜백들.
	 * 설정자: create_sim_full 이 memcpy 로 통째로 복사한다. 포인터가 아니라
	 *   구조체 자체를 담는 것에 주의 — 호출자가 준 ops 가 스택에 있어도 안전하다.
	 * 읽는 자: request/release_resources 가 콜백이 있는지 보고 부른다.
	 * 값 범위: 콜백을 등록하지 않았으면 전부 NULL 이다(kzalloc 덕분).
	 * 무엇에 쓰는가: 드라이버가 이 인터럽트를 요청하거나 해제할 때 알림을
	 *   받고 싶을 때다. gpio-sim 이 그것으로 가상 핀의 상태를 갱신한다. */

	void			*user_data;
	/* [한국어] 위 ops 콜백에 그대로 넘길 사용자 문맥.
	 * 설정자: create_sim_full 이 받은 값을 담는다.
	 * 읽는 자: 콜백을 부를 때 세 번째 인자로 넘긴다.
	 * 값 범위: 무엇이든. 커널은 뜻을 모르고 그대로 전달만 한다.
	 * 왜 필요한가: 콜백이 자기 문맥(어느 gpio-sim 칩인지 등)을 알아야 하는데,
	 *   전역 변수를 쓸 수는 없다. 등록 때 받아 두었다가 되돌려 주는 관용구다. */
};

/* [한국어] 인터럽트 하나의 상태.
 *
 * 위 work_ctx 가 시뮬레이터 전체의 상태라면 이쪽은 인터럽트 하나씩의
 * 상태다. 도메인의 map 콜백이 인터럽트마다 하나씩 만들어 chip_data 에 담는다.
 *
 * 필드가 둘뿐인 것이 이 시뮬레이터의 단순함을 보여 준다. 진짜 하드웨어라면
 * 레지스터 주소나 설정값이 들어갈 자리에, 여기서는 마스크 여부 하나만 있다. */
struct irq_sim_irq_ctx {
	bool			enabled;
	/* [한국어] 이 인터럽트가 언마스크되어 있는가.
	 * 설정자: irq_sim_irqmask/irqunmask 가 바꾼다.
	 * 읽는 자: set/get_irqchip_state 가 확인한다 — 마스크된 인터럽트는
	 *   발생시켜도 대기 비트를 세우지 않는다.
	 * 값 범위: 거짓이 초기값이다(kzalloc). 그래서 map 이 IRQ_NOAUTOEN 을
	 *   붙여 두는 것과 맞물려, 드라이버가 명시적으로 켜기 전에는 꺼져 있다.
	 * 동기화: 없다. 마스크와 발생이 동시에 일어나면 결과가 어느 쪽이든
	 *   될 수 있지만, 시뮬레이터라 그 경쟁이 문제가 되지 않는다. */

	struct irq_sim_work_ctx	*work_ctx;
	/* [한국어] 이 인터럽트가 속한 시뮬레이터의 전체 상태.
	 * 설정자: domain_map 이 도메인의 host_data 를 담는다.
	 * 읽는 자: 이 파일의 거의 모든 chip 콜백. 대기 비트맵과 irq_work,
	 *   그리고 ops 콜백에 닿는 통로다.
	 * 값 범위: NULL 이 아니다.
	 * 왜 필요한가: chip 콜백은 irq_data 만 받는다. 거기서 chip_data 로
	 *   이 구조체를 얻고, 다시 이 필드로 전체 상태에 닿는 두 단계 조회다. */
};

/*
 * [한국어]
 * irq_sim_irqmask - 이 가짜 인터럽트를 마스크한다
 *
 * @data: 그 인터럽트의 irq_data.
 *
 * 진짜 하드웨어라면 레지스터를 쓸 자리인데, 여기서는 플래그 하나를 내린다.
 *
 * 마스크된 인터럽트는 발생시켜도 대기 비트가 서지 않는다(아래
 * set_irqchip_state 참고). 즉 마스크 중에 온 인터럽트는 유실된다 —
 * 진짜 하드웨어가 대기 상태를 유지하는 것과 다르다.
 *
 * 그것이 시뮬레이터로서 문제인가: 대체로 아니다. 시험 코드는 마스크를
 * 풀어 둔 상태에서 인터럽트를 발생시키므로 그 차이가 드러나지 않는다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태 또는 프로세스 문맥.
 *
 * 호출 체인:
 *   disable_irq() → mask_irq() → chip->irq_mask → [이 함수]
 */
static void irq_sim_irqmask(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] map 이 담아 둔 인터럽트별 상태 */

	irq_ctx->enabled = false;	/* [한국어] 플래그만 내린다. 진짜 하드웨어라면 레지스터를 쓸 자리다 */
}

/*
 * [한국어]
 * irq_sim_irqunmask - 이 가짜 인터럽트의 마스크를 푼다
 *
 * @data: 그 인터럽트의 irq_data.
 *
 * 위 mask 의 짝. 이때부터 발생 요청이 실제로 대기 비트를 세운다.
 *
 * 마스크 중에 요청된 인터럽트를 되살리지 않는다는 점에 주의한다. 진짜
 * 하드웨어의 언마스크는 그 사이 밀린 인터럽트를 곧바로 내지만, 이 구현은
 * 애초에 대기 비트를 세우지 않았으므로 되살릴 것이 없다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태 또는 프로세스 문맥.
 *
 * 호출 체인:
 *   enable_irq() → unmask_irq() → chip->irq_unmask → [이 함수]
 */
static void irq_sim_irqunmask(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] 인터럽트별 상태 */

	irq_ctx->enabled = true;	/* [한국어] 이때부터 발생 요청이 대기 비트를 세운다 */
}

/*
 * [한국어]
 * irq_sim_set_type - 트리거 방식을 설정한다
 *
 * @data:   그 인터럽트의 irq_data.
 * @type:   요청된 IRQ_TYPE_* 값.
 * @return: 0 이면 성공, -EINVAL 이면 지원하지 않는 방식이다.
 *
 * 엣지 트리거만 지원한다. 레벨을 거부하는 이유가 이 시뮬레이터의 구조에 있다.
 *
 * 레벨 트리거는 "원인이 사라질 때까지 계속 어서션" 이라는 의미다. 그것을
 * 흉내 내려면 원인이 언제 사라지는지 알아야 하는데, 이 시뮬레이터는 그
 * 개념이 없다 — 발생 요청 한 번이 인터럽트 한 번이다.
 *
 * 그래서 상승/하강 엣지만 받아들이고, 실제로는 그 구분조차 동작에
 * 영향을 주지 않는다. 기록만 해 둔다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_set_irq_type() → __irq_set_trigger() → chip->irq_set_type → [이 함수]
 */
static int irq_sim_set_type(struct irq_data *data, unsigned int type)
{
	/* We only support rising and falling edge trigger types. */
	/* [한국어] (위 영어 주석) 엣지만 지원한다.
	 *
	 * 레벨을 거부하는 이유: 레벨은 "원인이 남아 있는 동안 계속" 이라는
	 * 의미인데, 이 시뮬레이터에는 원인이라는 개념이 없다. 발생 요청
	 * 한 번이 인터럽트 한 번일 뿐이다. */
	if (type & ~IRQ_TYPE_EDGE_BOTH)	/* [한국어] 상승·하강 엣지 외의 비트가 섞여 있는가 */
		return -EINVAL;	/* [한국어] 레벨이나 다른 방식은 흉내 낼 수 없다 */

	irqd_set_trigger_type(data, type);	/* [한국어] 기록만 한다. 상승인지 하강인지는 동작에 영향을 주지 않는다 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_sim_get_irqchip_state - 이 인터럽트의 하드웨어 상태를 알려 준다
 *
 * @data:   그 인터럽트의 irq_data.
 * @which:  묻는 상태의 종류.
 * @state:  결과를 담을 곳.
 * @return: 0 이면 성공, -EINVAL 이면 지원하지 않는 종류다.
 *
 * PENDING 상태만 지원한다. ACTIVE 나 MASKED 같은 다른 종류는 이 시뮬레이터에
 * 대응하는 개념이 없어 거부한다.
 *
 * 마스크되어 있으면 state 를 건드리지 않는다는 점에 주의한다. 0 으로
 * 채우는 것도 아니고 그대로 두는데, 호출자가 초기화하지 않은 변수를
 * 넘겼다면 쓰레기 값을 읽게 된다. 실제로는 마스크된 인터럽트의 상태를
 * 묻는 일이 없어 드러나지 않는 문제다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_get_irqchip_state() → chip->irq_get_irqchip_state → [이 함수]
 */
static int irq_sim_get_irqchip_state(struct irq_data *data,
				     enum irqchip_irq_state which, bool *state)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] 인터럽트별 상태 */
	irq_hw_number_t hwirq = irqd_to_hwirq(data);	/* [한국어] 비트맵에서의 비트 번호 */

	switch (which) {	/* [한국어] 어떤 상태를 묻는가 */
	case IRQCHIP_STATE_PENDING:	/* [한국어] 대기 중인가 */
		if (irq_ctx->enabled)	/* [한국어] 마스크되어 있지 않을 때만 답한다 */
			*state = test_bit(hwirq, irq_ctx->work_ctx->pending);	/* [한국어] 대기 비트맵을 읽는다 */
		break;	/* [한국어] 마스크되어 있으면 state 를 건드리지 않는다 — 호출자가 초기화해 두어야 한다 */
	default:	/* [한국어] ACTIVE, MASKED 등 다른 종류 */
		return -EINVAL;	/* [한국어] 이 시뮬레이터에 대응하는 개념이 없다 */
	}

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_sim_set_irqchip_state - 인터럽트를 발생시킨다
 *
 * @data:   그 인터럽트의 irq_data.
 * @which:  설정할 상태의 종류. PENDING 만 지원한다.
 * @state:  참이면 발생, 거짓이면 대기 취소.
 * @return: 0 이면 성공, -EINVAL 이면 지원하지 않는 종류다.
 *
 * 이 파일에서 가장 중요한 함수다. 시험 코드가 인터럽트를 발생시키는 통로가
 * 바로 이것이며, irq_set_irqchip_state(irq, IRQCHIP_STATE_PENDING, true)
 * 라는 호출로 쓰인다.
 *
 * 두 단계로 나뉜다:
 *   대기 비트를 세운다 — 어느 인터럽트가 발생했는지 기록.
 *   irq_work 를 예약한다 — 실제 핸들러 호출을 하드 인터럽트 문맥으로 미룬다.
 *
 * 왜 미루는가: 이 함수를 부르는 쪽은 임의의 문맥이다. sysfs 쓰기라면
 * 프로세스 문맥이고, 잠들 수 있는 상태일 수도 있다. 인터럽트 핸들러는
 * 하드 인터럽트 문맥(인터럽트가 꺼지고 선점이 불가능한 상태)을 기대하므로,
 * 그 문맥을 만들어 주는 irq_work 를 거쳐야 한다.
 *
 * state 가 거짓일 때는 비트를 지우기만 하고 예약하지 않는다. 발생 요청을
 * 취소하는 셈이다.
 *
 * 실행 컨텍스트: 임의의 문맥. 그것이 irq_work 가 필요한 이유다.
 *
 * 호출 체인:
 *   시험 코드 → irq_set_irqchip_state() → chip->irq_set_irqchip_state
 *     → [이 함수] → irq_work_queue() → (나중에) irq_sim_handle_irq()
 */
static int irq_sim_set_irqchip_state(struct irq_data *data,
				     enum irqchip_irq_state which, bool state)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] 인터럽트별 상태 */
	irq_hw_number_t hwirq = irqd_to_hwirq(data);	/* [한국어] 비트맵에서의 비트 번호 */

	switch (which) {	/* [한국어] 어떤 상태를 설정하는가 */
	case IRQCHIP_STATE_PENDING:	/* [한국어] 대기 상태를 바꾼다 = 인터럽트를 발생시키거나 취소한다 */
		if (irq_ctx->enabled) {	/* [한국어] 마스크되어 있으면 아무 일도 하지 않는다 — 진짜 하드웨어와 달리 대기 상태를 유지하지 않는다 */
			assign_bit(hwirq, irq_ctx->work_ctx->pending, state);	/* [한국어] state 에 따라 세우거나 지운다. 원자적 비트 연산이라 락이 필요 없다 */
			if (state)	/* [한국어] 발생시키는 경우에만 */
				irq_work_queue(&irq_ctx->work_ctx->work);	/* [한국어] 하드 인터럽트 문맥으로 넘어가 실제 핸들러를 부르게 한다. 이미 예약되어 있으면 중복 예약은 무시된다 */
		}
		break;
	default:	/* [한국어] 다른 상태 종류 */
		return -EINVAL;	/* [한국어] 지원하지 않는다 */
	}

	return 0;	/* [한국어] 성공. 마스크되어 있어 아무 일도 하지 않았어도 성공이다 */
}

/*
 * [한국어]
 * irq_sim_request_resources - 드라이버가 이 인터럽트를 요청할 때 사용자에게 알린다
 *
 * @data:   그 인터럽트의 irq_data.
 * @return: 0 이면 요청을 허용한다, 음수면 거절한다.
 *
 * 사용자가 등록한 선택적 콜백을 부르는 자리다. 콜백이 없으면 조건 없이
 * 허용한다.
 *
 * 왜 이런 알림이 필요한가: gpio-sim 같은 사용자는 "지금 이 가상 핀에
 * 인터럽트 핸들러가 붙었다" 는 사실을 알아야 한다. 그래야 sysfs 로
 * 그 상태를 보여 주거나, 핀의 동작 방식을 바꿀 수 있다.
 *
 * 콜백이 음수를 돌려주면 request_irq() 가 실패한다. 사용자가 요청을
 * 거절할 수단이기도 하다.
 *
 * 실행 컨텍스트: 인터럽트 요청 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   request_irq() → __setup_irq() → chip->irq_request_resources → [이 함수]
 *     → ops.irq_sim_irq_requested (사용자 콜백)
 */
static int irq_sim_request_resources(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] 인터럽트별 상태 */
	struct irq_sim_work_ctx *work_ctx = irq_ctx->work_ctx;	/* [한국어] 그것을 통해 전체 상태로 — 콜백이 거기 있다 */
	irq_hw_number_t hwirq = irqd_to_hwirq(data);	/* [한국어] 콜백에 알려 줄 하드웨어 번호 */

	if (work_ctx->ops.irq_sim_irq_requested)	/* [한국어] 사용자가 콜백을 등록했는가 */
		return work_ctx->ops.irq_sim_irq_requested(work_ctx->domain,	/* [한국어] 도메인과 hwirq 로 어느 인터럽트인지 알려 준다 */
							   hwirq,	/* [한국어] 그 하드웨어 번호 */
							   work_ctx->user_data);	/* [한국어] 등록 때 받아 둔 문맥을 되돌려 준다 */

	return 0;	/* [한국어] 콜백이 없으면 조건 없이 허용한다 */
}

/*
 * [한국어]
 * irq_sim_release_resources - 드라이버가 이 인터럽트를 해제할 때 사용자에게 알린다
 *
 * @data: 그 인터럽트의 irq_data.
 *
 * 위 request 의 짝. 반환값이 없는 것이 차이인데, 해제는 거절할 수 없기
 * 때문이다 — 드라이버가 free_irq() 를 부른 이상 되돌릴 방법이 없다.
 *
 * 실행 컨텍스트: 인터럽트 해제 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   free_irq() → __free_irq() → chip->irq_release_resources → [이 함수]
 *     → ops.irq_sim_irq_released (사용자 콜백)
 */
static void irq_sim_release_resources(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);	/* [한국어] 인터럽트별 상태 */
	struct irq_sim_work_ctx *work_ctx = irq_ctx->work_ctx;	/* [한국어] 전체 상태로 */
	irq_hw_number_t hwirq = irqd_to_hwirq(data);	/* [한국어] 콜백에 알려 줄 하드웨어 번호 */

	if (work_ctx->ops.irq_sim_irq_released)	/* [한국어] 콜백이 등록되어 있는가 */
		work_ctx->ops.irq_sim_irq_released(work_ctx->domain, hwirq,	/* [한국어] request 와 같은 인자로 알린다 */
						   work_ctx->user_data);	/* [한국어] 사용자 문맥 */
}

/* [한국어] 이 시뮬레이터의 가짜 인터럽트 컨트롤러.
 *
 * 진짜 컨트롤러와 콜백 구성이 크게 다르지 않다는 점이 중요하다. 코어는
 * 이것을 보통의 chip 으로 다루므로, 시험 코드가 거치는 경로가 실제
 * 하드웨어의 경로와 거의 같다.
 *
 * ack 나 eoi 콜백이 없는 이유: 아래 map 이 흐름 제어로 handle_simple_irq
 * 를 꽂는데, 그것은 ack 도 eoi 도 부르지 않는 가장 단순한 판이다.
 *
 * const 가 아닌 것에 주의: 최근 커널은 irq_chip 을 읽기 전용으로 두는
 * 방향으로 바뀌고 있지만, 이 파일은 아직 그 변경이 적용되지 않았다. */
static struct irq_chip irq_sim_irqchip = {
	.name			= "irq_sim",
	/* [한국어] /proc/interrupts 에 표시될 이름.
	 * 설정자: 이 정적 초기화. 읽는 자: proc.c 의 출력.
	 * 이 이름이 보이면 그 인터럽트가 실제 하드웨어가 아니라 소프트웨어
	 *   시뮬레이터의 것이라는 뜻이다. */

	.irq_mask		= irq_sim_irqmask,
	/* [한국어] 마스크 콜백. 플래그만 내린다.
	 * 설정자: 이 정적 초기화. 읽는 자: mask_irq() (kernel/irq/chip.c). */

	.irq_unmask		= irq_sim_irqunmask,
	/* [한국어] 언마스크 콜백. 위 mask 의 짝이다.
	 * 설정자: 이 정적 초기화. 읽는 자: unmask_irq(). */

	.irq_set_type		= irq_sim_set_type,
	/* [한국어] 트리거 방식 설정. 엣지만 허용한다.
	 * 설정자: 이 정적 초기화. 읽는 자: __irq_set_trigger() (manage.c).
	 * 레벨을 거부하는 이유는 그 함수의 주석에 있다. */

	.irq_get_irqchip_state	= irq_sim_get_irqchip_state,
	/* [한국어] 하드웨어 상태 조회. PENDING 만 답한다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_get_irqchip_state(). */

	.irq_set_irqchip_state	= irq_sim_set_irqchip_state,
	/* [한국어] 하드웨어 상태 설정 — 이 시뮬레이터의 핵심 콜백이다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_set_irqchip_state().
	 * 시험 코드가 인터럽트를 발생시키는 통로가 바로 이것이다. 다른
	 *   chip 에서는 진단이나 오류 주입에 쓰이는 부수적인 콜백이지만,
	 *   여기서는 주된 인터페이스다. */

	.irq_request_resources	= irq_sim_request_resources,
	/* [한국어] 드라이버가 요청할 때 사용자 콜백을 부른다.
	 * 설정자: 이 정적 초기화. 읽는 자: __setup_irq() (manage.c).
	 * 진짜 chip 에서는 클럭이나 전원 같은 자원을 잡는 자리인데,
	 *   여기서는 알림 용도로 쓴다. */

	.irq_release_resources	= irq_sim_release_resources,
	/* [한국어] 해제할 때의 알림. 위 request 의 짝이다.
	 * 설정자: 이 정적 초기화. 읽는 자: __free_irq() (manage.c). */
};

/*
 * [한국어]
 * irq_sim_handle_irq - irq_work 본체. 대기 중인 인터럽트들의 핸들러를 부른다
 *
 * @work: 예약된 irq_work. container_of 로 전체 상태를 되짚는다.
 *
 * 위 set_irqchip_state 가 예약한 작업이 여기서 실행된다. 이 시점의 문맥이
 * 하드 인터럽트 문맥이라는 것이 이 함수의 존재 이유다 — 드라이버 핸들러가
 * 그 문맥을 기대하기 때문이다.
 *
 * 비트맵이 빌 때까지 반복한다. 한 번의 irq_work 실행이 여러 인터럽트를
 * 처리할 수 있고, 처리 중에 새로 발생한 것도 함께 처리한다.
 *
 * offset 을 루프 밖에 두고 초기화하지 않는 것이 미묘하다. find_next_bit 이
 * 그 위치부터 찾으므로, 처리한 자리 다음부터 이어 찾는 셈이다. 그런데
 * 비트를 지운 뒤에도 offset 을 갱신하지 않아, 다음 반복은 방금 처리한
 * 자리부터 다시 찾는다. 결과적으로 낮은 번호부터 순서대로 처리된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥(IRQ_WORK_INIT_HARD 덕분). 잠들지 않는다.
 *
 * 호출 체인:
 *   irq_sim_set_irqchip_state() → irq_work_queue() → (커널) → [이 함수]
 *     → handle_simple_irq() (kernel/irq/chip.c) → 드라이버 핸들러
 */
static void irq_sim_handle_irq(struct irq_work *work)
{
	struct irq_sim_work_ctx *work_ctx;	/* [한국어] 시뮬레이터 전체 상태 */
	unsigned int offset = 0;	/* [한국어] 비트를 찾기 시작할 위치. 0 부터라 낮은 번호가 먼저 처리된다 */
	int irqnum;	/* [한국어] hwirq 를 옮긴 리눅스 인터럽트 번호 */

	work_ctx = container_of(work, struct irq_sim_work_ctx, work);	/* [한국어] irq_work 를 품은 구조체로 되짚는다. work 가 첫 필드지만 container_of 는 오프셋을 계산하므로 위치와 무관하다 */

	while (!bitmap_empty(work_ctx->pending, work_ctx->irq_count)) {	/* [한국어] 대기 중인 것이 남아 있는 동안. 처리 중에 새로 발생한 것도 함께 처리된다 */
		offset = find_next_bit(work_ctx->pending,	/* [한국어] 다음 선 비트를 찾는다 */
				       work_ctx->irq_count, offset);	/* [한국어] 비트맵이 자기 크기를 모르므로 상한을 함께 넘긴다 */
		clear_bit(offset, work_ctx->pending);	/* [한국어] 처리하기 전에 먼저 지운다. 핸들러가 도는 동안 같은 인터럽트가 또 발생하면 다시 세워져, 다음 반복에서 처리된다 */
		irqnum = irq_find_mapping(work_ctx->domain, offset);	/* [한국어] hwirq 를 리눅스 인터럽트 번호로 옮긴다 */
		handle_simple_irq(irq_to_desc(irqnum));	/* [한국어] 가장 단순한 흐름 제어. 마스크도 ack 도 없이 곧바로 핸들러를 부른다 — 가짜 하드웨어라 그 절차가 필요 없다 */
	}
}

/*
 * [한국어]
 * irq_sim_domain_map - 인터럽트 하나를 이 시뮬레이터에 연결한다
 *
 * @domain: 이 시뮬레이터의 도메인.
 * @virq:   배정된 리눅스 인터럽트 번호.
 * @hw:     대응하는 하드웨어 번호. 이 시뮬레이터에서는 비트맵의 비트 번호다.
 * @return: 0 이면 성공, -ENOMEM 이면 할당 실패.
 *
 * 도메인이 인터럽트를 처음 매핑할 때 부르는 콜백이다. 인터럽트별 상태를
 * 만들고, chip 과 흐름 제어를 꽂고, 초기 설정 플래그를 정한다.
 *
 * 마지막 irq_modify_status() 가 이 함수에서 가장 중요하다. 세 플래그를
 * 다루는데 각각의 이유가 다르며, 그 자리의 주석에 적어 두었다.
 *
 * 실행 컨텍스트: 인터럽트 매핑 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_mapping()/irq_domain_associate() → domain->ops->map → [이 함수]
 */
static int irq_sim_domain_map(struct irq_domain *domain,
			      unsigned int virq, irq_hw_number_t hw)
{
	struct irq_sim_work_ctx *work_ctx = domain->host_data;	/* [한국어] create_sim_full 이 도메인에 담아 둔 전체 상태 */
	struct irq_sim_irq_ctx *irq_ctx;	/* [한국어] 이 인터럽트 하나의 상태 */

	irq_ctx = kzalloc_obj(*irq_ctx);	/* [한국어] 0 으로 채워 잡는다 — enabled 가 거짓으로 시작하는 것이 아래 IRQ_NOAUTOEN 과 짝이 된다 */
	if (!irq_ctx)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 매핑이 실패하고 호출자가 그것을 전달한다 */

	irq_set_chip(virq, &irq_sim_irqchip);	/* [한국어] 위 가짜 컨트롤러를 꽂는다 */
	irq_set_chip_data(virq, irq_ctx);	/* [한국어] chip 콜백들이 irq_data 에서 꺼내 쓸 인터럽트별 상태 */
	irq_set_handler(virq, handle_simple_irq);	/* [한국어] 흐름 제어. 마스크·ack 절차가 없는 가장 단순한 판이다 */
	/* [한국어] 세 플래그의 초기 설정. 각각 이유가 다르다.
	 *
	 *   IRQ_NOREQUEST 를 지운다 — 기본값이 "요청 금지" 이므로, 명시적으로
	 *     열어 주어야 드라이버가 request_irq 를 할 수 있다.
	 *   IRQ_NOAUTOEN 을 지운다 — 요청 직후 자동으로 켜지게 한다. 위
	 *     irq_ctx->enabled 가 거짓으로 시작하므로, 그 자동 활성화가
	 *     unmask 를 불러 참으로 만들어 준다.
	 *   IRQ_NOPROBE 를 세운다 — 자동 탐지 대상에서 뺀다. 가짜 인터럽트를
	 *     탐지 후보로 삼는 것은 의미가 없다.
	 *
	 * 인자 순서에 주의: 첫 번째가 지울 것, 두 번째가 세울 것이다. */
	irq_modify_status(virq, IRQ_NOREQUEST | IRQ_NOAUTOEN, IRQ_NOPROBE);	/* [한국어] 요청과 자동 활성화를 열고, 자동 탐지는 막는다 */
	irq_ctx->work_ctx = work_ctx;	/* [한국어] chip 콜백들이 전체 상태에 닿을 통로 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_sim_domain_unmap - 인터럽트 하나의 매핑을 해제한다
 *
 * @domain: 이 시뮬레이터의 도메인.
 * @virq:   해제할 리눅스 인터럽트 번호.
 *
 * 위 map 의 짝. 순서가 중요하다 — 핸들러를 먼저 떼고, irq_data 를
 * 초기화하고, 마지막에 메모리를 반납한다.
 *
 * 왜 그 순서인가: 핸들러를 떼기 전에 chip_data 를 반납하면, 그 사이에
 * 인터럽트가 들어와 해제된 메모리를 만진다. 핸들러를 먼저 NULL 로 만들면
 * 그 창이 닫힌다.
 *
 * 실행 컨텍스트: 매핑 해제 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_dispose_mapping()/irq_domain_remove() → domain->ops->unmap → [이 함수]
 */
static void irq_sim_domain_unmap(struct irq_domain *domain, unsigned int virq)
{
	struct irq_sim_irq_ctx *irq_ctx;	/* [한국어] 반납할 인터럽트별 상태 */
	struct irq_data *irqd;	/* [한국어] 그것을 꺼낼 통로 */

	irqd = irq_domain_get_irq_data(domain, virq);	/* [한국어] 이 도메인 계층의 irq_data */
	irq_ctx = irq_data_get_irq_chip_data(irqd);	/* [한국어] map 이 담아 둔 상태를 꺼낸다. 반납하기 전에 미리 꺼내 두어야 한다 */

	irq_set_handler(virq, NULL);	/* [한국어] 흐름 제어를 먼저 뗀다. 이 뒤로는 인터럽트가 들어와도 처리되지 않는다 */
	irq_domain_reset_irq_data(irqd);	/* [한국어] chip 과 chip_data 포인터를 지운다 */
	kfree(irq_ctx);	/* [한국어] 마지막에 메모리를 반납한다. 앞의 두 단계가 끝나야 아무도 이것을 참조하지 않는다 */
}

/* [한국어] 이 시뮬레이터 도메인의 연산표.
 *
 * map/unmap 만 있고 alloc/free 가 없는 것이 이 도메인의 성격을 말해 준다.
 * 계층형이 아닌 단순한 선형 도메인이라, 하위 도메인에 자원을 요청할
 * 일이 없다. */
static const struct irq_domain_ops irq_sim_domain_ops = {
	.map		= irq_sim_domain_map,
	/* [한국어] 인터럽트를 처음 매핑할 때의 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_domain_associate().
	 * chip 과 흐름 제어를 꽂고 인터럽트별 상태를 만든다. */

	.unmap		= irq_sim_domain_unmap,
	/* [한국어] 매핑을 해제할 때의 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_domain_disassociate().
	 * map 이 만든 것을 순서대로 되돌린다. */
};

/**
 * irq_domain_create_sim - Create a new interrupt simulator irq_domain and
 *                         allocate a range of dummy interrupts.
 *
 * @fwnode:     struct fwnode_handle to be associated with this domain.
 * @num_irqs:   Number of interrupts to allocate.
 *
 * On success: return a new irq_domain object.
 * On failure: a negative errno wrapped with ERR_PTR().
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_domain_create_sim - 시뮬레이터를 만든다 (콜백 없는 간단한 판)
 *
 * @fwnode:   이 도메인을 식별할 펌웨어 노드. NULL 이어도 된다.
 * @num_irqs: 만들 인터럽트의 개수.
 * @return:   만들어진 도메인, 또는 ERR_PTR 로 감싼 오류.
 *
 * 아래 full 판에 NULL 두 개를 넘기는 한 줄짜리 래퍼다. 콜백도 사용자
 * 문맥도 필요 없는 대부분의 시험 코드가 이것을 쓴다.
 *
 * 이름에 create 가 두 번 나타나지 않는 것에 주의 — 이 파일의 함수 이름은
 * irq_domain_ 접두사에 _sim 접미사를 붙이는 규칙을 따르고, 그래서
 * irq_sim_ 접두사를 쓰는 내부 함수들과 구분된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   시험 모듈 → [이 함수] → irq_domain_create_sim_full()
 */
struct irq_domain *irq_domain_create_sim(struct fwnode_handle *fwnode,
					 unsigned int num_irqs)
{
	return irq_domain_create_sim_full(fwnode, num_irqs, NULL, NULL);	/* [한국어] 콜백과 사용자 문맥 없이 부른다 */
}
EXPORT_SYMBOL_GPL(irq_domain_create_sim);	/* [한국어] 시험 모듈이 쓸 수 있게 공개 */

/*
 * [한국어]
 * irq_domain_create_sim_full - 시뮬레이터를 만든다 (콜백까지 받는 완전한 판)
 *
 * @fwnode:   이 도메인을 식별할 펌웨어 노드.
 * @num_irqs: 만들 인터럽트의 개수.
 * @ops:      선택적 콜백들. NULL 이면 알림을 받지 않는다.
 * @data:     콜백에 되돌려 줄 사용자 문맥.
 * @return:   만들어진 도메인, 또는 ERR_PTR 로 감싼 오류.
 *
 * 이 파일에서 눈여겨볼 관용구가 여기 있다 — 커널의 __free/no_free_ptr
 * 자동 정리 기법이다.
 *
 * 어떻게 동작하는가:
 *   __free(kfree) 가 붙은 변수는 범위를 벗어날 때 자동으로 kfree 된다.
 *     그래서 중간에 return 해도 그때까지 잡은 것이 자동으로 반납된다.
 *   no_free_ptr() 은 그 자동 반납을 취소하고 포인터를 꺼낸다. 성공해서
 *     소유권을 넘길 때 쓴다.
 *
 * 그 결과 goto 사슬이 통째로 사라진다. 이 파일의 devres.c 가 쓰는
 * 전통적인 goto 방식과 대비되는데, 커널이 최근 이쪽으로 옮겨 가고 있다.
 *
 * 마지막 줄의 no_free_ptr(work_ctx)->domain 이 그 기법의 요점을 보여
 * 준다 — 소유권을 놓으면서 동시에 필요한 값을 꺼낸다.
 *
 * 선언이 함수 중간에 나타나는 것도 이 기법 때문이다. pending 을 위쪽에
 * 선언하면 work_ctx 할당 실패 시에도 그 정리가 실행되는데, 아직 잡지
 * 않았으므로 의미가 없다. 쓰기 직전에 선언하면 그 문제가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   gpio-sim 등 → [이 함수] → irq_domain_create_linear()
 */
struct irq_domain *irq_domain_create_sim_full(struct fwnode_handle *fwnode,
					      unsigned int num_irqs,
					      const struct irq_sim_ops *ops,
					      void *data)
{
	struct irq_sim_work_ctx *work_ctx __free(kfree) =	/* [한국어] 범위를 벗어나면 자동으로 kfree 된다. 아래 어느 return 으로 나가든 반납이 보장된다 */
				kzalloc_obj(*work_ctx);	/* [한국어] 0 으로 채워 잡는다 — ops 가 전부 NULL 로 시작해야 콜백 검사가 성립한다 */

	if (!work_ctx)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 포인터를 돌려주는 API 라 오류도 ERR_PTR 로 감싼다 */

	unsigned long *pending __free(bitmap) = bitmap_zalloc(num_irqs, GFP_KERNEL);	/* [한국어] 대기 비트맵. 여기에 선언하는 이유는 위 블록 주석 참고 — 쓰기 직전에 선언해야 불필요한 정리를 피한다 */
	if (!pending)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] work_ctx 는 __free 가 알아서 반납한다 — goto 가 필요 없는 이유다 */

	work_ctx->domain = irq_domain_create_linear(fwnode, num_irqs,	/* [한국어] 선형 도메인 — hwirq 가 0..num_irqs-1 로 조밀하다 */
						    &irq_sim_domain_ops,	/* [한국어] 위 map/unmap 표 */
						    work_ctx);	/* [한국어] host_data 로 담긴다. map 콜백이 여기서 꺼내 쓴다 */
	if (!work_ctx->domain)	/* [한국어] 도메인 생성 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] work_ctx 와 pending 모두 __free 가 반납한다 */

	work_ctx->irq_count = num_irqs;	/* [한국어] 비트맵을 훑을 상한 */
	work_ctx->work = IRQ_WORK_INIT_HARD(irq_sim_handle_irq);	/* [한국어] HARD 판이라 반드시 하드 인터럽트 문맥에서 실행된다. 드라이버 핸들러가 그 문맥을 기대하기 때문이다 */
	work_ctx->pending = no_free_ptr(pending);	/* [한국어] 소유권을 넘긴다. 이 뒤로 pending 의 자동 반납은 취소된다 */
	work_ctx->user_data = data;	/* [한국어] 콜백에 되돌려 줄 문맥 */

	if (ops)	/* [한국어] 콜백을 등록했는가 */
		memcpy(&work_ctx->ops, ops, sizeof(*ops));	/* [한국어] 포인터가 아니라 내용을 복사한다 — 호출자가 준 구조체가 스택에 있어도 안전하다 */

	return no_free_ptr(work_ctx)->domain;	/* [한국어] 소유권을 넘기면서 동시에 도메인을 꺼낸다. 이 한 줄이 __free 기법의 요점이다 */
}
EXPORT_SYMBOL_GPL(irq_domain_create_sim_full);	/* [한국어] gpio-sim 등이 쓸 수 있게 공개 */

/**
 * irq_domain_remove_sim - Deinitialize the interrupt simulator domain: free
 *                         the interrupt descriptors and allocated memory.
 *
 * @domain:     The interrupt simulator domain to tear down.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_domain_remove_sim - 시뮬레이터를 없앤다
 *
 * @domain: 없앨 도메인.
 *
 * 위 create 의 짝이며, 순서가 결정적으로 중요하다.
 *
 *   1. irq_work 가 끝나기를 기다린다 — 지금 돌고 있을 수 있다.
 *   2. 비트맵을 반납한다.
 *   3. 전체 상태를 반납한다.
 *   4. 도메인을 없앤다.
 *
 * 1 을 빠뜨리면 어떤 일이 생기는가: irq_work 가 다른 CPU 에서 돌면서
 * work_ctx->pending 을 읽고 있는데, 여기서 그것을 반납한다. 해제된
 * 메모리를 읽는 것이며 재현하기 어려운 형태로 나타난다.
 *
 * 4 를 마지막에 하는 것도 의도적이다. 도메인을 먼저 없애면 unmap 콜백이
 * 불리는데, 그 콜백이 work_ctx 를 참조하지는 않으므로 순서를 바꿔도
 * 동작하기는 한다. 다만 자원을 잡은 역순으로 푸는 것이 읽기에 자연스럽다.
 *
 * 실행 컨텍스트: 프로세스 문맥. irq_work_sync 가 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   시험 모듈의 정리 또는 devm_irq_domain_remove_sim() → [이 함수]
 */
void irq_domain_remove_sim(struct irq_domain *domain)
{
	struct irq_sim_work_ctx *work_ctx = domain->host_data;	/* [한국어] create 가 담아 둔 전체 상태 */

	irq_work_sync(&work_ctx->work);	/* [한국어] 돌고 있는 irq_work 가 끝나기를 기다린다. 이것을 빠뜨리면 아래에서 반납한 메모리를 그쪽이 계속 읽는다 */
	bitmap_free(work_ctx->pending);	/* [한국어] 대기 비트맵 반납 */
	kfree(work_ctx);	/* [한국어] 전체 상태 반납 */

	irq_domain_remove(domain);	/* [한국어] 도메인을 없앤다. 매핑된 인터럽트마다 unmap 콜백이 불린다 */
}
EXPORT_SYMBOL_GPL(irq_domain_remove_sim);	/* [한국어] 시험 모듈에 공개 */

/*
 * [한국어]
 * devm_irq_domain_remove_sim - devm 정리 콜백
 *
 * @data: 없앨 도메인. devm_add_action 이 void * 로 넘긴다.
 *
 * 아래 devm 판이 등록하는 정리 동작이다. 타입만 되돌려 위 remove 를 부른다.
 *
 * devres.c 의 devres_alloc 방식과 달리 devm_add_action_or_reset 을 쓴다.
 * 그쪽은 자원 정보를 담을 구조체가 필요할 때 쓰고, 이쪽은 포인터 하나만
 * 넘기면 될 때 쓰는 더 간단한 관용구다.
 *
 * 실행 컨텍스트: 장치 detach, 프로세스 문맥.
 *
 * 호출 체인:
 *   device_release_driver() → devres_release_all() → [이 함수]
 *     → irq_domain_remove_sim()
 */
static void devm_irq_domain_remove_sim(void *data)
{
	struct irq_domain *domain = data;	/* [한국어] void * 를 원래 타입으로 되돌린다 */

	irq_domain_remove_sim(domain);	/* [한국어] 실제 정리는 위 함수에 맡긴다 */
}

/**
 * devm_irq_domain_create_sim - Create a new interrupt simulator for
 *                              a managed device.
 *
 * @dev:        Device to initialize the simulator object for.
 * @fwnode:     struct fwnode_handle to be associated with this domain.
 * @num_irqs:   Number of interrupts to allocate
 *
 * On success: return a new irq_domain object.
 * On failure: a negative errno wrapped with ERR_PTR().
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_irq_domain_create_sim - 장치 수명에 묶인 시뮬레이터 (간단한 판)
 *
 * @dev:      소유할 장치. detach 때 자동으로 정리된다.
 * @fwnode:   도메인을 식별할 펌웨어 노드.
 * @num_irqs: 만들 인터럽트의 개수.
 * @return:   만들어진 도메인, 또는 ERR_PTR 로 감싼 오류.
 *
 * 아래 full 판에 NULL 두 개를 넘기는 한 줄짜리 래퍼다. 위
 * irq_domain_create_sim() 과 같은 관계이며, devm 여부만 다르다.
 *
 * 이 파일에 네 개의 create 함수가 있는 구조가 여기서 완성된다:
 *   (devm 여부) × (콜백 여부) 의 네 조합이다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 프로브 → [이 함수] → devm_irq_domain_create_sim_full()
 */
struct irq_domain *devm_irq_domain_create_sim(struct device *dev,
					      struct fwnode_handle *fwnode,
					      unsigned int num_irqs)
{
	return devm_irq_domain_create_sim_full(dev, fwnode, num_irqs,	/* [한국어] full 판에 맡긴다 */
					       NULL, NULL);	/* [한국어] 콜백과 사용자 문맥 없이 */
}
EXPORT_SYMBOL_GPL(devm_irq_domain_create_sim);	/* [한국어] 드라이버에 공개 */

/*
 * [한국어]
 * devm_irq_domain_create_sim_full - 장치 수명에 묶인 시뮬레이터 (완전한 판)
 *
 * @dev:      소유할 장치.
 * @fwnode:   도메인을 식별할 펌웨어 노드.
 * @num_irqs: 만들 인터럽트의 개수.
 * @ops:      선택적 콜백들.
 * @data:     콜백에 되돌려 줄 사용자 문맥.
 * @return:   만들어진 도메인, 또는 ERR_PTR 로 감싼 오류.
 *
 * 위 irq_domain_create_sim_full() 에 devm 등록을 더한 것이다.
 *
 * devm_add_action_or_reset 을 쓰는 것에 주의한다. 이름의 or_reset 이
 * 뜻하는 바가 중요하다 — 등록에 실패하면 그 자리에서 정리 동작을 한 번
 * 실행해 준다. 그래서 등록 실패 시 시뮬레이터를 손으로 없앨 필요가 없다.
 *
 * 그것이 없었다면 실패 경로에서 irq_domain_remove_sim() 을 직접 불러야
 * 했고, 그 호출을 빠뜨리면 누수가 된다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   gpio-sim 등의 프로브 → [이 함수] → irq_domain_create_sim_full()
 *     + devm_add_action_or_reset()
 */
struct irq_domain *
devm_irq_domain_create_sim_full(struct device *dev,
				struct fwnode_handle *fwnode,
				unsigned int num_irqs,
				const struct irq_sim_ops *ops,
				void *data)
{
	struct irq_domain *domain;	/* [한국어] 만들어진 도메인 */
	int ret;	/* [한국어] devm 등록 결과 */

	domain = irq_domain_create_sim_full(fwnode, num_irqs, ops, data);	/* [한국어] 먼저 시뮬레이터를 만든다 */
	if (IS_ERR(domain))	/* [한국어] 생성 실패 */
		return domain;	/* [한국어] ERR_PTR 를 그대로 전달한다 */

	ret = devm_add_action_or_reset(dev, devm_irq_domain_remove_sim, domain);	/* [한국어] 정리 동작을 등록한다. or_reset 이라 등록 실패 시 그 자리에서 정리까지 해 준다 */
	if (ret)	/* [한국어] 등록 실패 — 시뮬레이터는 이미 정리되었다 */
		return ERR_PTR(ret);	/* [한국어] 손으로 없앨 필요가 없는 것이 or_reset 판의 이점이다 */

	return domain;	/* [한국어] 성공. 이제 detach 때 자동으로 정리된다 */
}
EXPORT_SYMBOL_GPL(devm_irq_domain_create_sim_full);	/* [한국어] 드라이버에 공개 */
