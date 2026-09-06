// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Multiplex several virtual IPIs over a single HW IPI.
 *
 * Copyright The Asahi Linux Contributors
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */
/*
 * [한국어 설명] 하드웨어 IPI 하나 위에 여러 가상 IPI 를 얹는 다중화 계층 (ipi-mux.c)
 *
 * === 파일의 역할 ===
 * CPU 사이 인터럽트(IPI, Inter-Processor Interrupt)를 하드웨어가 한 종류만
 * 제공할 때, 그 위에 여러 개의 논리적 IPI 를 얹어 준다.
 *
 * 왜 필요한가: 리눅스는 여러 종류의 IPI 를 쓴다 — 스케줄러 깨우기, 함수
 * 원격 호출, TLB 무효화, 타이머 등이 각각 다른 IPI 번호를 쓴다. 그런데
 * 어떤 하드웨어(Apple M 시리즈, 일부 RISC-V)는 IPI 를 한 종류만 제공한다.
 * 그러면 인터럽트를 받은 CPU 가 "왜 깨웠는지"를 알 수 없다.
 *
 * 해결책: CPU 마다 비트맵을 두고, 보내는 쪽이 그 비트맵에 "무엇 때문인지"를
 * 표시한 뒤 하드웨어 IPI 를 하나 쏜다. 받는 쪽은 비트맵을 읽어 어느 가상
 * IPI 인지 알아내고 각각을 처리한다.
 *
 * 이 다중화의 핵심 비용은 락이 아니라 메모리 순서다. 비트맵 쓰기와 하드웨어
 * IPI 발행 사이의 순서가 어긋나면, 받는 쪽이 빈 비트맵을 읽거나(IPI 유실)
 * 이미 처리한 비트를 다시 본다. 그래서 이 파일의 코드 대부분이 원자 연산과
 * 메모리 배리어이며, 주석의 절반이 그 순서를 설명한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IPI 를 보내고 받는 두 방향의 한가운데에 있다:
 *
 *   보내기:
 *     smp_call_function() 등 → __ipi_send_mask()
 *       ↓ chip->ipi_send_mask
 *     ipi_mux_send_mask()          ← **이 파일** — 비트맵에 표시하고
 *       ↓ ipi_mux_send 콜백
 *     아키텍처의 하드웨어 IPI 발행
 *
 *   받기:
 *     하드웨어 IPI 도착 → 아키텍처의 핸들러
 *       ↓
 *     ipi_mux_process()            ← **이 파일** — 비트맵을 읽어
 *       ↓ generic_handle_domain_irq
 *     각 가상 IPI 의 핸들러 (스케줄러, 함수 호출 등)
 *
 * 실행 컨텍스트: 전부 인터럽트 문맥이거나 인터럽트가 꺼진 상태다. 잠들지
 * 않고 락도 잡지 않는다 — 원자 연산만으로 동기화한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   irq_domain — 가상 IPI 마다 인터럽트 번호를 발급받는다.
 *   percpu — CPU 마다의 비트맵을 둔다.
 *   원자 연산과 메모리 배리어 — 이 파일의 실질적인 동기화 수단.
 *
 * 이 파일에 의존하는 곳: 아키텍처의 IPI 초기화 코드가 ipi_mux_create() 로
 *   이 계층을 세우고, 하드웨어 IPI 핸들러에서 ipi_mux_process() 를 부른다.
 *   Apple Silicon 의 AIC 드라이버와 RISC-V 의 IPI 코드가 그렇다.
 *
 * 공유 상태: struct ipi_mux_cpu 의 두 비트맵. 자세한 것은 그 구조체 주석에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct ipi_mux_cpu   — CPU 하나의 "켜짐" 비트맵과 "대기 중" 비트맵.
 * ipi_mux_mask/unmask()— 가상 IPI 하나를 켜고 끈다. unmask 는 밀린 것을 곧바로 처리한다.
 * ipi_mux_send_mask()  — 대상 CPU 들의 비트맵에 표시하고 필요하면 하드웨어 IPI 를 쏜다.
 * ipi_mux_process()    — 받은 쪽에서 비트맵을 읽어 각 가상 IPI 를 처리한다.
 * ipi_mux_create()     — 이 계층을 세운다. 아키텍처 초기화가 한 번 부른다.
 *
 * 이 파일을 읽는 열쇠는 두 비트맵의 짝짓기다. send 쪽의
 * atomic_fetch_or_release 와 process 쪽의 atomic_fetch_andnot 이 짝을 이루고,
 * unmask 쪽의 or 와 send 쪽의 read 가 또 다른 짝을 이룬다. 각 주석이
 * 그 상대를 명시하고 있다.
 */

#define pr_fmt(fmt) "ipi-mux: " fmt	/* [한국어] 이 파일의 모든 pr_err/pr_info 앞에 "ipi-mux: " 를 붙인다. 로그에서 출처를 바로 알 수 있게 하는 관용구다 */
#include <linux/cpu.h>	/* [한국어] CPU 관련 기본 정의 */
#include <linux/init.h>	/* [한국어] 초기화 섹션 표시 매크로 */
#include <linux/irq.h>	/* [한국어] struct irq_chip, irqd_to_hwirq() */
#include <linux/irqchip.h>	/* [한국어] 인터럽트 컨트롤러 등록 기반 */
#include <linux/irqchip/chained_irq.h>	/* [한국어] 연쇄 인터럽트 보조 함수들 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain 생성과 인터럽트 할당 */
#include <linux/jump_label.h>	/* [한국어] 정적 분기 기반 */
#include <linux/percpu.h>	/* [한국어] alloc_percpu — CPU 마다의 비트맵을 둔다 */
#include <linux/smp.h>	/* [한국어] smp_processor_id(), for_each_cpu() */

/* [한국어] CPU 하나가 갖는 가상 IPI 상태.
 *
 * 두 비트맵의 관계가 이 파일 전체의 토대다:
 *   enable — 이 CPU 에서 켜져 있는 가상 IPI 들. 그 CPU 자신만 고친다.
 *   bits   — 이 CPU 로 보내졌지만 아직 처리되지 않은 가상 IPI 들. 다른
 *            CPU 들이 고친다.
 *
 * 둘을 나눈 이유: 꺼진 IPI 에 대해서는 하드웨어 IPI 를 쏘지 않아야 한다.
 * 그러면 받은 쪽이 처리할 것 없이 깨어나는 헛수고가 생긴다. 그래서 보내는
 * 쪽이 bits 에 표시한 뒤 enable 을 확인하고, 켜져 있을 때만 실제로 쏜다.
 *
 * 두 필드 모두 atomic_t 인 것이 요점이다. 여러 CPU 가 동시에 이 구조체를
 * 건드리므로 락이 필요할 법하지만, 비트 단위 연산이라 원자 연산만으로
 * 충분하고 그편이 IPI 핫패스에 훨씬 싸다. */
struct ipi_mux_cpu {
	atomic_t			enable;
	/* [한국어] 이 CPU 에서 켜져 있는 가상 IPI 의 비트맵.
	 * 설정자: 이 CPU 자신의 ipi_mux_mask()/unmask() 뿐이다.
	 * 읽는 자: 다른 CPU 의 ipi_mux_send_mask() (쏠지 말지 판정), 그리고
	 *   이 CPU 의 ipi_mux_process() (어느 비트를 처리할지).
	 * 값 범위: 비트 i 가 서 있으면 i 번 가상 IPI 가 켜져 있다.
	 * 동기화: 원자 연산. 소유 CPU 만 쓰고 남들은 읽기만 하므로, process()
	 *   가 이 값을 읽을 때는 순서 보장이 필요 없다(그 함수 주석 참고).
	 *   다만 send_mask() 가 읽을 때는 unmask() 와 경쟁하므로 배리어가 필요하다. */

	atomic_t			bits;
	/* [한국어] 이 CPU 로 보내졌지만 아직 처리되지 않은 가상 IPI 의 비트맵.
	 * 설정자: 다른 CPU(또는 자신)의 ipi_mux_send_mask() 가 OR 로 표시한다.
	 * 읽는 자/지우는 자: 이 CPU 의 ipi_mux_process() 가 처리하며 AND-NOT 으로 지운다.
	 * 값 범위: 비트 i 가 서 있으면 i 번 가상 IPI 가 대기 중이다.
	 * 동기화: 이 필드가 이 파일에서 가장 경쟁이 심하다. 여러 CPU 가 동시에
	 *   OR 하고, 소유 CPU 가 동시에 AND-NOT 한다. 그래서 단순한 원자 연산이
	 *   아니라 release/acquire 의미가 붙은 판을 쓴다 — 비트맵에 표시하기
	 *   전에 쓴 공유 데이터가 받는 쪽에 보이도록 보장해야 하기 때문이다. */
};

static struct ipi_mux_cpu __percpu *ipi_mux_pcpu;	/* [한국어] CPU 마다의 위 구조체. ipi_mux_create() 가 할당하고 이후 바뀌지 않는다 */
static struct irq_domain *ipi_mux_domain;	/* [한국어] 가상 IPI 들의 인터럽트 도메인. NULL 이 아니면 이 계층이 이미 세워졌다는 뜻이라, create() 의 중복 호출 검사에도 쓰인다 */
static void (*ipi_mux_send)(unsigned int cpu);	/* [한국어] 실제 하드웨어 IPI 를 쏘는 아키텍처 콜백. create() 가 받아 보관한다 */

/*
 * [한국어]
 * ipi_mux_mask - 가상 IPI 하나를 이 CPU 에서 끈다
 *
 * @d: 그 가상 IPI 의 irq_data. hwirq 가 비트 번호다.
 *
 * enable 비트맵에서 해당 비트를 내린다. 그러면 다른 CPU 가 이 IPI 를 보내도
 * 하드웨어 IPI 를 쏘지 않는다 — 표시만 bits 에 남고, 나중에 unmask 될 때
 * 처리된다.
 *
 * this_cpu_ptr 을 쓴다는 점이 중요하다. 인터럽트를 마스크하는 것은 언제나
 * 그것을 받는 CPU 자신이므로, 다른 CPU 의 구조체를 건드릴 일이 없다.
 * per-CPU 인터럽트의 마스킹이 그 CPU 에서만 유효한 것과 같은 이치다.
 *
 * 배리어가 없는 것도 의도적이다. 끄는 방향은 순서 문제가 없다 — 늦게
 * 반영되어 IPI 를 한 번 더 받더라도 그것은 낭비일 뿐 오류가 아니다.
 * 켜는 방향(아래 unmask)이 배리어를 요구하는 것과 대비된다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태 또는 인터럽트 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   disable_percpu_irq() → mask_irq() → chip->irq_mask → [이 함수]
 */
static void ipi_mux_mask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);	/* [한국어] 자기 CPU 의 상태. 마스킹은 언제나 받는 CPU 자신이 한다 */

	atomic_andnot(BIT(irqd_to_hwirq(d)), &icpu->enable);	/* [한국어] 해당 비트를 내린다. hwirq 가 곧 비트 번호다. 배리어가 없는 이유는 위 주석 참고 */
}

/*
 * [한국어]
 * ipi_mux_unmask - 가상 IPI 하나를 이 CPU 에서 켜고, 밀린 것이 있으면 곧바로 처리한다
 *
 * @d: 그 가상 IPI 의 irq_data.
 *
 * 위 mask 의 짝이지만 훨씬 복잡하다. 단순히 비트를 세우는 것으로 끝나지
 * 않고, 꺼져 있는 동안 밀린 IPI 가 있는지 확인해 있으면 자기 자신에게
 * 하드웨어 IPI 를 쏜다.
 *
 * 왜 그래야 하는가: 꺼져 있는 동안 다른 CPU 가 이 IPI 를 보냈다면, 그쪽은
 * bits 에 표시만 하고 하드웨어 IPI 는 쏘지 않았다(enable 이 꺼져 있었으므로).
 * 켜는 시점에 이쪽이 확인하지 않으면 그 IPI 는 영원히 처리되지 않는다.
 *
 * 메모리 배리어가 필요한 이유가 여기 있다. "enable 을 세운다"와 "bits 를
 * 읽는다" 사이에 순서가 없으면, 보내는 쪽과 이쪽이 서로 상대의 갱신을
 * 보지 못해 둘 다 IPI 를 쏘지 않는 창이 생긴다. 아래 send_mask() 가
 * 정확히 거울상의 순서를 쓰며, 두 배리어가 짝을 이뤄 그 창을 없앤다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태 또는 인터럽트 문맥.
 *
 * 호출 체인:
 *   enable_percpu_irq() → unmask_irq() → chip->irq_unmask → [이 함수]
 *     → ipi_mux_send (아키텍처의 하드웨어 IPI 발행)
 */
static void ipi_mux_unmask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);	/* [한국어] 자기 CPU 의 상태 */
	u32 ibit = BIT(irqd_to_hwirq(d));	/* [한국어] 이 가상 IPI 의 비트 마스크. 아래에서 두 번 쓰므로 미리 계산해 둔다 */

	atomic_or(ibit, &icpu->enable);	/* [한국어] 켠다. 이제부터 다른 CPU 가 이 IPI 를 보내면 하드웨어 IPI 를 쏠 것이다 */

	/*
	 * The atomic_or() above must complete before the atomic_read()
	 * below to avoid racing ipi_mux_send_mask().
	 */
	/* [한국어] (위 영어 주석에 이어) 켜기와 확인 사이에 순서를 강제한다.
	 *
	 * 순서가 없으면 어떤 일이 생기는가: 보내는 CPU 가 bits 에 표시하고
	 * enable 을 읽는데 아직 꺼진 값을 보아 IPI 를 쏘지 않는다. 동시에
	 * 이쪽은 enable 을 켜고 bits 를 읽는데 아직 표시 전 값을 보아 역시
	 * 쏘지 않는다. 둘 다 상대를 놓쳐 IPI 가 통째로 유실된다.
	 *
	 * 두 곳에 배리어를 넣어 그 순서 뒤집힘을 막는다. 아래 send_mask() 의
	 * 같은 자리 배리어가 이것의 짝이다. */
	smp_mb__after_atomic();	/* [한국어] 위 원자 연산 뒤에 완전한 메모리 배리어. atomic_or 자체는 순서를 보장하지 않는다 */

	/* If a pending IPI was unmasked, raise a parent IPI immediately. */
	/* [한국어] (위 영어 주석) 꺼져 있는 동안 밀린 IPI 가 있으면 곧바로 쏜다.
	 *
	 * 자기 자신에게 쏜다는 점이 눈에 띈다. 하드웨어 IPI 가 도착하면
	 * ipi_mux_process() 가 돌면서 bits 를 읽어 처리하므로, 밀린 것을
	 * 직접 처리하지 않고 정상 경로를 타게 하는 것이다. 코드가 단순해지고
	 * 처리 경로가 하나로 유지된다. */
	if (atomic_read(&icpu->bits) & ibit)	/* [한국어] 이 IPI 가 대기 중인가 */
		ipi_mux_send(smp_processor_id());	/* [한국어] 자기 자신에게 하드웨어 IPI 를 쏜다. 곧 process() 가 돌아 밀린 것을 처리한다 */
}

/*
 * [한국어]
 * ipi_mux_send_mask - 대상 CPU 들에게 가상 IPI 를 보낸다
 *
 * @d:    보낼 가상 IPI 의 irq_data.
 * @mask: 받을 CPU 들의 집합.
 *
 * 각 대상 CPU 에 대해 두 단계를 밟는다. 그 CPU 의 bits 비트맵에 표시하고,
 * 필요하면 하드웨어 IPI 를 쏜다.
 *
 * "필요하면" 의 두 조건이 이 함수의 최적화다:
 *   그 비트가 이미 서 있었으면 쏘지 않는다 — 이미 보낸 IPI 가 처리되기를
 *     기다리는 중이므로, 또 쏘는 것은 낭비다.
 *   그 IPI 가 꺼져 있으면 쏘지 않는다 — 받아도 처리하지 않을 것이므로
 *     헛되이 깨우지 않는다. 나중에 unmask 될 때 그쪽이 처리한다.
 *
 * 세 개의 메모리 순서 요구가 얽혀 있고, 원 주석들이 각각의 짝을 명시한다.
 * 그 셋이 이 함수의 실질적인 내용이라, 아래 각 주석에 자세히 적었다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태. 잠들지 않는다.
 *
 * 호출 체인:
 *   smp_call_function()/scheduler_ipi() 등 → __ipi_send_mask()
 *     → chip->ipi_send_mask → [이 함수] → ipi_mux_send (아키텍처)
 */
static void ipi_mux_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);	/* [한국어] 초기값. 아래 루프에서 곧바로 대상 CPU 의 것으로 덮어쓴다 — 이 초기화는 사실상 쓰이지 않는다 */
	u32 ibit = BIT(irqd_to_hwirq(d));	/* [한국어] 보낼 가상 IPI 의 비트 마스크 */
	unsigned long pending;	/* [한국어] 표시하기 직전의 bits 값. 이미 서 있었는지 판정하는 데 쓴다 */
	int cpu;	/* [한국어] 순회 중인 대상 CPU */

	for_each_cpu(cpu, mask) {	/* [한국어] 대상 CPU 를 하나씩 */
		icpu = per_cpu_ptr(ipi_mux_pcpu, cpu);	/* [한국어] 그 CPU 의 상태. this_cpu_ptr 이 아니라 남의 것을 건드린다 — 그래서 원자 연산이 필요하다 */

		/*
		 * This sequence is the mirror of the one in ipi_mux_unmask();
		 * see the comment there. Additionally, release semantics
		 * ensure that the vIPI flag set is ordered after any shared
		 * memory accesses that precede it. This therefore also pairs
		 * with the atomic_fetch_andnot in ipi_mux_process().
		 */
		/* [한국어] (위 영어 주석에 이어) 이 한 줄이 두 가지 순서를 동시에 만든다.
		 *
		 * 첫째, unmask() 의 거울상이다. 그쪽이 "enable 을 켜고 bits 를 읽는"
		 * 반면 이쪽은 "bits 를 세우고 enable 을 읽는다". 두 순서가 서로
		 * 뒤집혀 있어야 어느 쪽도 상대를 놓치지 않는다.
		 *
		 * 둘째, release 의미가 붙어 있다. 이 IPI 를 보내기 전에 쓴 공유
		 * 데이터(예: smp_call_function 의 인자 구조체)가, 받는 쪽이 이
		 * 비트를 본 시점에 반드시 보여야 한다. release 가 그것을 보장하며,
		 * process() 의 andnot 과 짝을 이룬다.
		 *
		 * fetch 판을 쓰는 이유: 표시 직전의 값이 필요하다. 이미 서 있었다면
		 * 하드웨어 IPI 를 다시 쏠 이유가 없기 때문이다. */
		pending = atomic_fetch_or_release(ibit, &icpu->bits);	/* [한국어] 표시하고 이전 값을 받는다. release 로 앞선 공유 데이터 쓰기가 먼저 보이게 한다 */

		/*
		 * The atomic_fetch_or_release() above must complete
		 * before the atomic_read() below to avoid racing with
		 * ipi_mux_unmask().
		 */
		/* [한국어] (위 영어 주석) unmask() 와의 경쟁을 막는 배리어.
		 *
		 * 위 unmask() 의 같은 자리 배리어와 짝이다. 둘 중 하나라도 없으면
		 * 두 CPU 가 서로의 갱신을 보지 못해 IPI 가 유실될 수 있다. */
		smp_mb__after_atomic();	/* [한국어] bits 표시가 아래 enable 읽기보다 먼저 완료되게 한다 */

		/*
		 * The flag writes must complete before the physical IPI is
		 * issued to another CPU. This is implied by the control
		 * dependency on the result of atomic_read() below, which is
		 * itself already ordered after the vIPI flag write.
		 */
		/* [한국어] (위 영어 주석에 이어) 세 번째 순서 요구와, 그것이 이미
		 * 만족된다는 설명이다.
		 *
		 * 요구: 비트맵 쓰기가 하드웨어 IPI 발행보다 먼저 완료되어야 한다.
		 * 그러지 않으면 받는 CPU 가 깨어나 비트맵을 읽었을 때 아직 표시가
		 * 없어 아무것도 처리하지 않고 돌아간다.
		 *
		 * 왜 추가 배리어가 필요 없는가: 아래 if 가 atomic_read 의 결과에
		 * 의존하는 제어 의존성(control dependency)을 만든다. 그리고 그
		 * atomic_read 자체가 위 배리어에 의해 비트맵 쓰기 뒤로 정렬되어
		 * 있다. 따라서 ipi_mux_send() 호출은 자연히 비트맵 쓰기 뒤에 온다.
		 *
		 * 제어 의존성은 쓰기에 대해서만 순서를 보장하는 약한 성질이지만,
		 * 여기서 순서를 지켜야 할 것이 ipi_mux_send() 안의 MMIO 쓰기라
		 * 그것으로 충분하다. */
		if (!(pending & ibit) && (atomic_read(&icpu->enable) & ibit))	/* [한국어] 아직 대기 중이 아니었고, 그 CPU 에서 이 IPI 가 켜져 있을 때만 */
			ipi_mux_send(cpu);	/* [한국어] 실제 하드웨어 IPI 를 쏜다. 두 조건이 헛된 깨우기를 걸러 낸다 */
	}
}

/* [한국어] 가상 IPI 들이 쓰는 인터럽트 컨트롤러.
 *
 * 콜백이 셋뿐인 것이 이 chip 의 성격을 말해 준다. IPI 는 마스크·언마스크와
 * 보내기만 있으면 되고, ack 나 eoi 는 하드웨어 IPI 를 다루는 부모 컨트롤러의
 * 몫이라 여기서는 필요 없다.
 *
 * const 인 것에 주의 — 최근 커널은 irq_chip 을 읽기 전용으로 두는 방향으로
 * 바뀌고 있다. chip 은 여러 인터럽트가 공유하는 정적 자료이므로 수정될
 * 이유가 없고, 읽기 전용 섹션에 두면 실수로 고치는 것을 막을 수 있다. */
static const struct irq_chip ipi_mux_chip = {
	.name		= "IPI Mux",
	/* [한국어] /proc/interrupts 에 표시될 이름.
	 * 설정자: 이 정적 초기화. 읽는 자: proc.c 의 출력.
	 * 이 이름이 보이면 그 인터럽트가 하드웨어 IPI 하나 위에 얹힌 가상
	 *   IPI 라는 뜻이다. */

	.irq_mask	= ipi_mux_mask,
	/* [한국어] 이 가상 IPI 를 끄는 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: mask_irq() (kernel/irq/chip.c).
	 * 끄면 enable 비트맵에서 빠져, 보내는 쪽이 하드웨어 IPI 를 쏘지 않는다. */

	.irq_unmask	= ipi_mux_unmask,
	/* [한국어] 이 가상 IPI 를 켜는 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: unmask_irq().
	 * 단순히 켜는 것이 아니라 꺼진 동안 밀린 IPI 를 되살리는 일까지 한다. */

	.ipi_send_mask	= ipi_mux_send_mask,
	/* [한국어] 여러 CPU 에게 이 IPI 를 보내는 콜백 — 이 chip 의 핵심이다.
	 * 설정자: 이 정적 초기화.
	 * 읽는 자: __ipi_send_mask() (kernel/irq/ipi.c).
	 * ack/eoi 콜백이 없는 이유: 하드웨어 IPI 의 완료 처리는 부모 컨트롤러가
	 *   하고, 이 계층은 비트맵만 다룬다. */
};

/*
 * [한국어]
 * ipi_mux_domain_alloc - 가상 IPI 들에 인터럽트 번호를 배정한다
 *
 * @d:       이 도메인.
 * @virq:    배정을 시작할 리눅스 인터럽트 번호.
 * @nr_irqs: 배정할 개수.
 * @arg:     도메인별 인자. 이 도메인은 쓰지 않는다 — 가상 IPI 는 하드웨어
 *           정보 없이 번호만으로 정해지기 때문이다.
 * @return:  항상 0. 이 배정은 실패할 수 없다.
 *
 * irq_domain 이 인터럽트를 할당할 때 부르는 콜백이다. 각 가상 IPI 에 대해
 * 두 가지를 설정한다.
 *
 *   per-CPU devid 로 표시 — IPI 는 CPU 마다 따로 존재하고, 핸들러에 넘길
 *     문맥도 CPU 마다 다르다. 이 표시가 있어야 request_percpu_irq() 로
 *     핸들러를 등록할 수 있다.
 *   chip 과 흐름 제어 핸들러 연결 — 위 ipi_mux_chip 과 handle_percpu_devid_irq
 *     를 꽂는다. 후자는 마스크나 ack 없이 곧바로 핸들러를 부르는 가장 단순한
 *     흐름 제어인데, per-CPU 인터럽트는 다른 CPU 와 경쟁하지 않아 그 처리가
 *     필요 없기 때문이다.
 *
 * hwirq 로 i(0 부터 시작하는 순번)를 쓴다는 점이 중요하다. 그 값이 위
 * 비트맵의 비트 번호가 되므로, 도메인이 배정하는 순서가 곧 비트 배치다.
 *
 * 실행 컨텍스트: 초기화 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   ipi_mux_create() → irq_domain_alloc_irqs() → domain->ops->alloc
 *     → [이 함수] → irq_domain_set_info()
 */
static int ipi_mux_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i;	/* [한국어] 순번. 그대로 hwirq 가 되어 비트맵의 비트 번호가 된다 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 요청받은 개수만큼 */
		irq_set_percpu_devid(virq + i);	/* [한국어] CPU 마다 다른 dev_id 를 갖는 per-CPU 인터럽트로 표시한다. 이것이 있어야 request_percpu_irq 로 등록할 수 있다 */
		irq_domain_set_info(d, virq + i, i, &ipi_mux_chip, NULL,	/* [한국어] hwirq 로 i 를 준다 — 이 값이 곧 비트맵의 비트 번호다. chip_data 는 NULL 이다 */
				    handle_percpu_devid_irq, NULL, NULL);	/* [한국어] 흐름 제어는 가장 단순한 판. per-CPU 라 마스크나 ack 없이 곧바로 핸들러를 부른다 */
	}

	return 0;	/* [한국어] 실패할 수 있는 단계가 없다 — 자원을 새로 잡지 않고 표시만 하기 때문이다 */
}

/* [한국어] 이 도메인의 연산표.
 *
 * 둘뿐인 것이 이 도메인의 단순함을 보여 준다. 하드웨어 자원을 잡지 않으므로
 * 매핑이나 변환 콜백이 필요 없고, 해제는 코어의 기본 구현으로 충분하다. */
static const struct irq_domain_ops ipi_mux_domain_ops = {
	.alloc		= ipi_mux_domain_alloc,
	/* [한국어] 인터럽트를 배정할 때 부르는 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_domain_alloc_irqs().
	 * 위 함수가 per-CPU 표시와 chip 연결을 한다. */

	.free		= irq_domain_free_irqs_top,
	/* [한국어] 해제할 때 부르는 콜백. 코어의 기본 구현을 그대로 쓴다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_domain_free_irqs().
	 * 왜 자체 구현이 필요 없는가: alloc 이 잡은 것은 표시뿐이고 자원이
	 *   아니다. 코어의 기본 해제가 서술자를 정리해 주면 그것으로 끝난다.
	 * 실제로 이 도메인의 IPI 는 부팅 때 만들어져 끝까지 유지되므로 이
	 *   콜백이 불릴 일이 사실상 없다. */
};

/**
 * ipi_mux_process - Process multiplexed virtual IPIs
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * ipi_mux_process - 도착한 하드웨어 IPI 를 풀어 가상 IPI 들을 처리한다
 *
 * 인자도 반환값도 없다. 현재 CPU 의 비트맵을 읽어 처리한다.
 *
 * 받는 쪽의 진입점이다. 아키텍처의 하드웨어 IPI 핸들러가 이 함수를 부르면,
 * 비트맵을 읽어 어느 가상 IPI 들이 대기 중인지 알아내고 각각의 핸들러를 부른다.
 *
 * 세 단계다:
 *   1. enable 을 읽는다 — 켜져 있는 IPI 만 처리한다.
 *   2. bits 에서 그것들을 한 번에 지우고 지운 값을 받는다 (fetch_andnot).
 *   3. 지운 비트마다 해당 인터럽트를 처리한다.
 *
 * 2 에서 "지우면서 읽는" 원자 연산을 쓰는 것이 핵심이다. 읽고 나서 따로
 * 지우면 그 사이에 도착한 IPI 가 함께 지워져 유실된다.
 *
 * enable 로 걸러 낸 뒤 그것만 지운다는 점도 중요하다. 꺼진 IPI 의 비트는
 * bits 에 그대로 남아, 나중에 unmask 될 때 처리된다.
 *
 * 실행 컨텍스트: 인터럽트 문맥(하드웨어 IPI 핸들러 안). 잠들지 않는다.
 *
 * 호출 체인:
 *   하드웨어 IPI 도착 → 아키텍처의 IPI 핸들러 → [이 함수]
 *     → generic_handle_domain_irq() → 각 가상 IPI 의 핸들러
 */
void ipi_mux_process(void)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);	/* [한국어] 자기 CPU 의 상태. IPI 는 언제나 받은 CPU 에서 처리된다 */
	irq_hw_number_t hwirq;	/* [한국어] 처리 중인 가상 IPI 의 비트 번호 */
	unsigned long ipis;	/* [한국어] 이번에 처리할 비트들. for_each_set_bit 이 unsigned long 을 요구해 이 타입이다 */
	unsigned int en;	/* [한국어] 켜져 있는 IPI 들의 마스크 */

	/*
	 * Reading enable mask does not need to be ordered as long as
	 * this function is called from interrupt handler because only
	 * the CPU itself can change it's own enable mask.
	 */
	/* [한국어] (위 영어 주석에 이어) 이 읽기에는 순서 보장이 필요 없다.
	 *
	 * 왜인가: enable 은 그 CPU 자신만 고친다(mask/unmask 가 this_cpu_ptr 을
	 * 쓴다). 그리고 이 함수는 인터럽트 핸들러 안에서 돈다. 같은 CPU 의
	 * 인터럽트 문맥과 프로세스 문맥이 동시에 실행될 수 없으므로, 여기서
	 * 읽는 값은 언제나 일관된 최신 값이다.
	 *
	 * 위 send_mask() 가 남의 enable 을 읽을 때 배리어가 필요했던 것과
	 * 대조된다 — 그쪽은 진짜로 다른 CPU 와 경쟁한다. */
	en = atomic_read(&icpu->enable);	/* [한국어] 켜져 있는 IPI 들. 자기 것이라 배리어 없이 읽어도 안전하다 */

	/*
	 * Clear the IPIs we are about to handle. This pairs with the
	 * atomic_fetch_or_release() in ipi_mux_send_mask().
	 */
	/* [한국어] (위 영어 주석에 이어) 처리할 비트들을 지우면서 동시에 읽는다.
	 *
	 * 왜 한 연산이어야 하는가: 읽고 나서 따로 지우면, 그 사이에 다른 CPU 가
	 * 보낸 IPI 가 함께 지워져 통째로 유실된다.
	 *
	 * send 쪽의 release 와 짝을 이룬다는 것이 원 주석의 요점이다. 보내는
	 * 쪽이 비트를 세우기 전에 쓴 공유 데이터가, 이 연산으로 비트를 본
	 * 시점에 반드시 보인다. 그 보장이 없으면 IPI 핸들러가 아직 준비되지
	 * 않은 데이터를 읽는다.
	 *
	 * & en 으로 한 번 더 거르는 이유: fetch_andnot 은 en 에 든 비트만
	 * 지우지만, 돌려주는 것은 지우기 전의 전체 값이다. 꺼진 IPI 의 비트도
	 * 섞여 있으므로 그것을 걸러 내야 한다. 그 비트들은 bits 에 그대로
	 * 남아 나중에 unmask 될 때 처리된다. */
	ipis = atomic_fetch_andnot(en, &icpu->bits) & en;	/* [한국어] 켜진 것만 지우고, 지우기 전 값에서 켜진 것만 남긴다 */

	for_each_set_bit(hwirq, &ipis, BITS_PER_TYPE(int))	/* [한국어] 서 있는 비트마다. 상한이 int 의 비트 수인 것은 비트맵이 atomic_t 라 32비트이기 때문이다 */
		generic_handle_domain_irq(ipi_mux_domain, hwirq);	/* [한국어] hwirq 로 도메인을 조회해 해당 가상 IPI 의 핸들러를 부른다. 실제 인터럽트와 같은 경로다 */
}

/**
 * ipi_mux_create - Create virtual IPIs multiplexed on top of a single
 * parent IPI.
 * @nr_ipi:		number of virtual IPIs to create. This should
 *			be <= BITS_PER_TYPE(int)
 * @mux_send:		callback to trigger parent IPI for a particular CPU
 *
 * Returns first virq of the newly created virtual IPIs upon success
 * or <=0 upon failure
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * ipi_mux_create - 이 다중화 계층을 세운다
 *
 * @nr_ipi:   만들 가상 IPI 의 개수. 비트맵이 atomic_t 라 32 이하여야 한다.
 * @mux_send: 실제 하드웨어 IPI 를 쏘는 아키텍처 콜백.
 * @return:   성공하면 첫 가상 IPI 의 리눅스 인터럽트 번호(양수). 실패하면
 *            0 이하. 호출자는 그 번호부터 nr_ipi 개를 자기 IPI 로 쓴다.
 *
 * 아키텍처의 IPI 초기화가 한 번만 부른다. 그 뒤 시스템이 살아 있는 동안
 * 이 계층이 유지되며, 해제하는 함수가 없다 — 부팅에 만들어져 끝까지 간다.
 *
 * 다섯 단계로 자원을 잡고, 각 단계의 실패마다 그때까지 잡은 것을 되돌리는
 * goto 사슬이 아래에 있다. 커널에서 흔한 오류 처리 관용구다.
 *
 * 마지막 두 대입의 순서가 중요하다. ipi_mux_domain 과 ipi_mux_send 를
 * 모든 자원이 준비된 뒤에야 채운다 — 그 둘이 채워진 순간부터 다른 CPU 가
 * 이 계층을 쓸 수 있게 되기 때문이다.
 *
 * 실행 컨텍스트: 부팅 중 초기화, 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   아키텍처의 IPI 초기화 (Apple AIC, RISC-V IPI 등) → [이 함수]
 *     → irq_domain_create_linear() → irq_domain_alloc_irqs()
 */
int ipi_mux_create(unsigned int nr_ipi, void (*mux_send)(unsigned int cpu))
{
	struct fwnode_handle *fwnode;	/* [한국어] 도메인을 식별할 가짜 펌웨어 노드. 실제 장치 트리 노드가 없으므로 이름만으로 만든다 */
	struct irq_domain *domain;	/* [한국어] 만들 도메인. 성공해야 전역에 대입한다 */
	int rc;	/* [한국어] 오류 코드 또는 성공 시 첫 인터럽트 번호 */

	if (ipi_mux_domain)	/* [한국어] 이미 세워져 있는가. 전역이 하나뿐이라 두 번 부를 수 없다 */
		return -EEXIST;	/* [한국어] 아키텍처 초기화가 한 번만 부르도록 보장하는 방어다 */

	if (BITS_PER_TYPE(int) < nr_ipi || !mux_send)	/* [한국어] 비트맵이 atomic_t(32비트)라 그보다 많은 IPI 는 담을 수 없다. 콜백도 필수다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	ipi_mux_pcpu = alloc_percpu(typeof(*ipi_mux_pcpu));	/* [한국어] CPU 마다의 비트맵을 잡는다. 0 으로 초기화되어 모든 IPI 가 꺼진 상태로 시작한다 */
	if (!ipi_mux_pcpu)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 잡은 것이 없어 그냥 돌아간다 */

	fwnode = irq_domain_alloc_named_fwnode("IPI-Mux");	/* [한국어] 이름만 가진 가짜 펌웨어 노드. 도메인은 fwnode 로 식별되는데 이 계층은 장치 트리에 없어 만들어 준다 */
	if (!fwnode) {	/* [한국어] 할당 실패 */
		pr_err("unable to create IPI Mux fwnode\n");	/* [한국어] 위 pr_fmt 덕에 "ipi-mux: " 가 앞에 붙는다 */
		rc = -ENOMEM;	/* [한국어] 오류 코드를 담고 */
		goto fail_free_cpu;	/* [한국어] percpu 만 되돌리면 된다 */
	}

	domain = irq_domain_create_linear(fwnode, nr_ipi,	/* [한국어] 선형 도메인 — hwirq 가 0..nr_ipi-1 로 조밀해 배열로 매핑하면 된다 */
					  &ipi_mux_domain_ops, NULL);	/* [한국어] 위 연산표를 꽂는다. host_data 는 쓰지 않는다 */
	if (!domain) {	/* [한국어] 도메인 생성 실패 */
		pr_err("unable to add IPI Mux domain\n");	/* [한국어] 진단 메시지 */
		rc = -ENOMEM;	/* [한국어] 오류 코드 */
		goto fail_free_fwnode;	/* [한국어] fwnode 와 percpu 를 되돌린다 */
	}

	domain->flags |= IRQ_DOMAIN_FLAG_IPI_SINGLE;	/* [한국어] "이 도메인의 IPI 는 hwirq 하나가 모든 CPU 를 뜻한다"는 표시. CPU 마다 별도 hwirq 를 쓰는 방식과 구분된다 */
	irq_domain_update_bus_token(domain, DOMAIN_BUS_IPI);	/* [한국어] 이 도메인이 IPI 용임을 표시한다. 도메인 조회 시 같은 fwnode 의 다른 도메인과 구별하는 데 쓰인다 */

	rc = irq_domain_alloc_irqs(domain, nr_ipi, NUMA_NO_NODE, NULL);	/* [한국어] 실제로 인터럽트 번호들을 배정한다. 위 domain_alloc 콜백이 여기서 불린다. NUMA 노드를 가리지 않는다 — IPI 는 모든 CPU 의 것이다 */
	if (rc <= 0) {	/* [한국어] 성공하면 첫 인터럽트 번호(양수)를 준다. 0 이하는 실패다 */
		pr_err("unable to alloc IRQs from IPI Mux domain\n");	/* [한국어] 진단 메시지 */
		goto fail_free_domain;	/* [한국어] rc 에 이미 오류 코드가 들어 있어 따로 담지 않는다 */
	}

	/* [한국어] 여기서부터 이 계층이 살아난다. 두 전역을 마지막에 채우는 것이
	 * 중요하다 — 채워지는 순간부터 다른 CPU 가 send/process 를 부를 수 있으므로,
	 * 그 전에 모든 자원이 준비되어 있어야 한다. */
	ipi_mux_domain = domain;	/* [한국어] 도메인을 공개한다. process() 가 이것으로 핸들러를 찾는다 */
	ipi_mux_send = mux_send;	/* [한국어] 하드웨어 IPI 발행 콜백을 공개한다. send_mask() 와 unmask() 가 부른다 */

	return rc;	/* [한국어] 첫 가상 IPI 의 인터럽트 번호. 호출자는 이 번호부터 nr_ipi 개를 쓴다 */

/* [한국어] 아래는 실패 시 되돌리기 사슬이다. 잡은 역순으로 풀도록 배치되어
 * 있어, 실패한 지점의 라벨로 뛰면 그 아래가 차례로 실행된다. 커널의 흔한
 * 오류 처리 관용구이며, 각 단계마다 조건문을 두는 것보다 짧고 빠뜨리기 어렵다. */
fail_free_domain:
	irq_domain_remove(domain);	/* [한국어] 도메인을 없앤다. 아래로 이어져 fwnode 와 percpu 도 정리된다 */
fail_free_fwnode:	/* [한국어] fwnode 를 만든 뒤 실패한 경우의 진입점 */
	irq_domain_free_fwnode(fwnode);	/* [한국어] 가짜 펌웨어 노드를 반납한다 */
fail_free_cpu:	/* [한국어] percpu 할당 뒤 어디서 실패해도 여기까지 내려온다 */
	free_percpu(ipi_mux_pcpu);	/* [한국어] CPU 마다의 비트맵을 반납한다. 전역 포인터를 NULL 로 되돌리지 않는 것은, 실패하면 이 계층을 쓰는 코드가 아예 동작하지 않기 때문이다 */
	return rc;	/* [한국어] 실패 원인을 그대로 돌려준다 */
}
