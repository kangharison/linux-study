// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Imagination Technologies Ltd
 * Author: Qais Yousef <qais.yousef@imgtec.com>
 *
 * This file contains driver APIs to the IPI subsystem.
 */
/*
 * [한국어 설명] CPU 사이 인터럽트(IPI)를 인터럽트 서브시스템으로 다루는 API (ipi.c)
 *
 * === 파일의 역할 ===
 * IPI(Inter-Processor Interrupt)를 보통의 인터럽트처럼 예약하고 보내는
 * 인터페이스를 제공한다. 아키텍처가 IPI 를 직접 다루는 대신 irq_domain
 * 을 통해 관리하게 해 준다.
 *
 * 왜 그렇게 하는가: IPI 도 결국 인터럽트 컨트롤러가 전달한다. 아키텍처마다
 * 자기 방식으로 다루면 컨트롤러 드라이버가 두 벌의 코드를 갖게 된다.
 * irq_domain 으로 통일하면 컨트롤러 드라이버가 하나의 방식만 구현하면 되고,
 * /proc/interrupts 같은 진단 수단도 그대로 쓸 수 있다.
 *
 * 이 파일 전체를 관통하는 구분이 하나 있다 — 하드웨어가 IPI 를 어떻게
 * 표현하느냐다.
 *
 *   IPI_SINGLE  — 모든 CPU 가 같은 hwirq 를 쓴다. 목적지는 보낼 때 지정한다.
 *                 리눅스 인터럽트 번호가 하나면 충분하다.
 *   IPI_PER_CPU — CPU 마다 다른 hwirq 를 쓴다. 목적지 CPU 수만큼의 리눅스
 *                 인터럽트 번호가 필요하고, 그것들이 연속이어야 한다.
 *
 * 후자의 "연속" 제약 때문에 ipi_offset 이라는 필드가 등장한다. 첫 번째
 * CPU 번호를 기억해 두고, "CPU n 의 인터럽트 번호 = base + n - offset"
 * 이라는 계산으로 대응시킨다. 이 계산이 파일 곳곳에 반복해서 나타난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 아키텍처와 인터럽트 코어 사이에 있다:
 *
 *   설정:
 *     아키텍처의 SMP 초기화
 *       ↓ irq_reserve_ipi(domain, dest)      ← **이 파일**
 *     irq_domain_alloc_descs() → __irq_domain_alloc_irqs()
 *       ↓ 컨트롤러 드라이버의 domain->ops->alloc
 *     hwirq 배정 완료
 *
 *   보내기:
 *     smp_call_function() 등 → 아키텍처의 IPI 발행
 *       ↓ __ipi_send_mask()/__ipi_send_single()  ← **이 파일**
 *     chip->ipi_send_mask 또는 ipi_send_single
 *       ↓
 *     실제 하드웨어 IPI
 *
 * 받는 쪽은 이 파일에 없다 — 보통의 인터럽트와 똑같이 흐름 제어 핸들러를
 * 거쳐 처리된다.
 *
 * 실행 컨텍스트: 예약은 부팅 중 프로세스 문맥이고, 보내기는 인터럽트가
 * 꺼진 임의의 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   irqdomain.c — 도메인 조회와 인터럽트 할당.
 *   chip 드라이버의 ipi_send_single/ipi_send_mask 콜백.
 *
 * 이 파일에 의존하는 곳:
 *   아키텍처의 SMP 코드(ARM, MIPS, RISC-V 등).
 *   ipi-mux.c 가 자기 chip 에 ipi_send_mask 를 제공해 이 계층 아래에 붙는다.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_reserve_ipi()    — IPI 용 인터럽트 번호를 예약한다.
 * irq_destroy_ipi()    — 그것을 반납한다.
 * ipi_get_hwirq()      — 특정 CPU 의 하드웨어 인터럽트 번호를 알아낸다.
 * ipi_send_verify()    — 보내기 전 인자를 검증한다.
 * __ipi_send_single()  — 한 CPU 에 보낸다. 검증을 건너뛰는 빠른 판.
 * __ipi_send_mask()    — 여러 CPU 에 보낸다. 역시 빠른 판.
 * ipi_send_single()    — 위 빠른 판에 검증을 더한 공개 API.
 * ipi_send_mask()      — 마찬가지.
 *
 * 밑줄 판과 공개 판을 나눈 이유: 아키텍처와 코어 코드는 신뢰할 수 있어
 * 검증을 생략해도 되지만, 그 밖의 호출자는 검증이 필요하다. 검증이
 * cpumask 비교를 포함해 싸지 않으므로 핫패스에서 빼는 것이다.
 */

#define pr_fmt(fmt) "genirq/ipi: " fmt	/* [한국어] 이 파일의 모든 pr_warn 앞에 "genirq/ipi: " 를 붙인다. 로그에서 출처를 바로 알 수 있게 하는 관용구다 */

#include <linux/irqdomain.h>	/* [한국어] irq_domain_is_ipi 계열 판정자와 인터럽트 할당 함수 */
#include <linux/irq.h>	/* [한국어] struct irq_data, irq_chip 과 irqd_to_hwirq() */

/**
 * irq_reserve_ipi() - Setup an IPI to destination cpumask
 * @domain:	IPI domain
 * @dest:	cpumask of CPUs which can receive the IPI
 *
 * Allocate a virq that can be used to send IPI to any CPU in dest mask.
 *
 * Return: Linux IRQ number on success or error code on failure
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_reserve_ipi - IPI 를 보낼 수 있는 리눅스 인터럽트 번호를 예약한다
 *
 * @domain: IPI 용 인터럽트 도메인. 컨트롤러 드라이버가 만들어 둔 것이다.
 * @dest:   이 IPI 를 받을 수 있는 CPU 들의 집합.
 * @return: 성공하면 첫 리눅스 인터럽트 번호(양수), 실패하면 음수.
 *
 * 하드웨어 표현 방식에 따라 필요한 번호의 개수가 달라지는 것이 이 함수의
 * 핵심이다.
 *
 *   IPI_SINGLE  — 번호 하나면 된다. 목적지는 보낼 때 지정하므로 dest 에
 *                 구멍이 있어도 상관없다.
 *   IPI_PER_CPU — CPU 마다 번호가 필요하다. 그리고 그 번호들이 연속이어야
 *                 하므로, dest 도 연속이어야 한다.
 *
 * 후자에서 "구멍" 을 거부하는 이유: 번호를 연속으로 잡아 두고 "CPU n 의
 * 번호 = base + n - offset" 으로 계산하기 때문이다. dest 가 {0,1,3} 처럼
 * 구멍이 있으면 그 계산이 성립하지 않는다. 원 주석대로, 구멍이 필요하면
 * 여러 번 나눠 예약하면 된다.
 *
 * 예약이 끝나면 각 인터럽트에 두 가지를 설정한다: 목적지 마스크(친화도
 * 자리에 담는다)와 IRQ_NO_BALANCING(IPI 는 옮길 수 없다).
 *
 * 실행 컨텍스트: 부팅 중 SMP 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   아키텍처의 SMP 초기화 → [이 함수] → irq_domain_alloc_descs()
 *     → __irq_domain_alloc_irqs() → 컨트롤러의 domain->ops->alloc
 */
int irq_reserve_ipi(struct irq_domain *domain,
			     const struct cpumask *dest)
{
	unsigned int nr_irqs, offset;	/* [한국어] 잡을 번호의 개수와, per-CPU 방식에서 첫 CPU 번호 */
	struct irq_data *data;	/* [한국어] 잡은 인터럽트를 하나씩 설정할 때 쓴다 */
	int virq, i;	/* [한국어] 배정받은 첫 번호와 순회 변수 */

	if (!domain ||!irq_domain_is_ipi(domain)) {	/* [한국어] 도메인이 없거나 IPI 용이 아닌가 */
		pr_warn("Reservation on a non IPI domain\n");	/* [한국어] 호출자의 실수다. 보통의 도메인에 IPI 를 예약할 수 없다 */
		return -EINVAL;	/* [한국어] IPI 도메인이 아닌 곳에는 예약할 수 없다 */
	}

	if (!cpumask_subset(dest, cpu_possible_mask)) {	/* [한국어] 존재할 수 있는 CPU 들의 부분집합인가 */
		pr_warn("Reservation is not in possible_cpu_mask\n");	/* [한국어] online 이 아니라 possible 을 기준으로 한다 — 지금 꺼져 있어도 나중에 켜질 CPU 는 유효하다 */
		return -EINVAL;	/* [한국어] possible 이 아닌 CPU 는 존재할 수 없어 목적지가 될 수 없다 */
	}

	nr_irqs = cpumask_weight(dest);	/* [한국어] 목적지 CPU 의 개수. per-CPU 방식에서는 이만큼의 번호가 필요하다 */
	if (!nr_irqs) {	/* [한국어] 빈 마스크인가 */
		pr_warn("Reservation for empty destination mask\n");	/* [한국어] 보낼 곳이 없는 IPI 는 의미가 없다 */
		return -EINVAL;	/* [한국어] 보낼 곳이 없는 IPI 는 만들 이유가 없다 */
	}

	if (irq_domain_is_ipi_single(domain)) {	/* [한국어] 모든 CPU 가 같은 hwirq 를 쓰는 방식인가 */
		/*
		 * If the underlying implementation uses a single HW irq on
		 * all cpus then we only need a single Linux irq number for
		 * it. We have no restrictions vs. the destination mask. The
		 * underlying implementation can deal with holes nicely.
		 */
		/* [한국어] (위 영어 주석에 이어) 번호 하나면 충분하다.
		 *
		 * 왜 제약이 없는가: 목적지를 보낼 때 지정하므로, dest 에 구멍이
		 * 있어도 chip->ipi_send_mask 가 알아서 처리한다. 아래 per-CPU
		 * 방식이 연속을 요구하는 것과 대조된다. */
		nr_irqs = 1;	/* [한국어] 번호 하나로 모든 CPU 를 덮는다 */
		offset = 0;	/* [한국어] 번호 계산이 필요 없어 0 이다 */
	} else {
		unsigned int next;	/* [한국어] 구멍 검사에 쓸 임시 변수 */

		/*
		 * The IPI requires a separate HW irq on each CPU. We require
		 * that the destination mask is consecutive. If an
		 * implementation needs to support holes, it can reserve
		 * several IPI ranges.
		 */
		/* [한국어] (위 영어 주석에 이어) CPU 마다 별도의 hwirq 가 필요하다.
		 *
		 * 그래서 목적지 마스크가 연속이어야 한다. 번호를 연속으로 잡아 두고
		 * "CPU n 의 번호 = base + n - offset" 으로 계산하기 때문이다.
		 *
		 * 구멍이 필요하면 여러 번 나눠 예약하라는 것이 원 주석의 답이다. */
		offset = cpumask_first(dest);	/* [한국어] 첫 CPU 번호. 이후 모든 번호 계산의 기준이 된다 */
		/*
		 * Find a hole and if found look for another set bit after the
		 * hole. For now we don't support this scenario.
		 */
		/* [한국어] (위 영어 주석에 이어) 구멍 검사 방법.
		 *
		 * 두 단계다. 먼저 첫 CPU 이후의 첫 번째 빈 자리를 찾고, 그 뒤에
		 * 또 선 비트가 있는지 본다. 있으면 그 사이가 구멍이다.
		 *
		 * 예: {0,1,3} 이면 offset=0, 첫 빈 자리는 2, 그 뒤의 선 비트는 3
		 * → 구멍이 있다.
		 * 예: {0,1,2} 이면 offset=0, 첫 빈 자리는 3, 그 뒤에 선 비트가
		 * 없다 → 연속이다. */
		next = cpumask_next_zero(offset, dest);	/* [한국어] 첫 CPU 이후의 첫 빈 자리 */
		if (next < nr_cpu_ids)	/* [한국어] 빈 자리가 실제로 있으면 */
			next = cpumask_next(next, dest);	/* [한국어] 그 뒤에 또 선 비트가 있는지 본다 */
		if (next < nr_cpu_ids) {	/* [한국어] 있으면 그 사이가 구멍이다 */
			pr_warn("Destination mask has holes\n");	/* [한국어] 번호 계산이 성립하지 않는다. 나눠서 예약해야 한다 */
			return -EINVAL;	/* [한국어] 번호 계산이 성립하지 않으므로 예약을 거절한다 */
		}
	}

	virq = irq_domain_alloc_descs(-1, nr_irqs, 0, NUMA_NO_NODE, NULL);	/* [한국어] 서술자를 연속으로 잡는다. -1 은 번호를 가리지 않는다는 뜻이고, NUMA 노드도 가리지 않는다 — IPI 는 모든 CPU 의 것이다 */
	if (virq <= 0) {	/* [한국어] 0 이하는 실패다. IPI 에 0 번이 배정될 일은 없다 */
		pr_warn("Can't reserve IPI, failed to alloc descs\n");	/* [한국어] 서술자를 잡지 못했다 */
		return -ENOMEM;	/* [한국어] 아직 잡은 것이 없어 그대로 돌아간다 */
	}

	virq = __irq_domain_alloc_irqs(domain, virq, nr_irqs, NUMA_NO_NODE,	/* [한국어] 그 번호들을 도메인에 매핑한다. 컨트롤러의 alloc 콜백이 여기서 불려 hwirq 를 배정한다 */
				       (void *) dest, true, NULL);	/* [한국어] dest 를 도메인별 인자로 넘긴다 — 컨트롤러가 어느 CPU 들인지 알아야 한다. true 는 이미 서술자를 잡았다는 표시다 */

	if (virq <= 0) {	/* [한국어] 매핑 실패 */
		pr_warn("Can't reserve IPI, failed to alloc hw irqs\n");	/* [한국어] 매핑에 실패했다 — 아래에서 서술자를 되돌린다 */
		goto free_descs;	/* [한국어] 위에서 잡은 서술자를 반납해야 한다 */
	}

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 잡은 번호들을 하나씩 설정한다 */
		data = irq_get_irq_data(virq + i);	/* [한국어] 그 번호의 irq_data */
		cpumask_copy(data->common->affinity, dest);	/* [한국어] 목적지 마스크를 친화도 자리에 담는다. IPI 에는 친화도라는 개념이 없어, 그 자리를 "받을 수 있는 CPU 들" 로 빌려 쓴다 */
		data->common->ipi_offset = offset;	/* [한국어] 번호 계산의 기준. per-CPU 방식에서 "CPU n 의 번호 = base + n - offset" 에 쓰인다 */
		irq_set_status_flags(virq + i, IRQ_NO_BALANCING);	/* [한국어] 친화도를 바꿀 수 없게 막는다. 위에서 그 자리를 목적지 마스크로 쓰고 있어, 누가 고치면 IPI 가 깨진다 */
	}
	return virq;	/* [한국어] 첫 번호. 호출자는 여기서부터 nr_irqs 개를 쓴다 */

free_descs:	/* [한국어] 매핑에 실패했을 때 서술자를 되돌리는 자리 */
	irq_free_descs(virq, nr_irqs);	/* [한국어] 위에서 잡은 서술자 범위를 반납한다 */
	return -EBUSY;	/* [한국어] 원래 오류 코드가 아니라 -EBUSY 를 준다. __irq_domain_alloc_irqs 가 0 이하만 돌려주어 구체적인 코드를 잃었기 때문이다 */
}

/**
 * irq_destroy_ipi() - unreserve an IPI that was previously allocated
 * @irq:	Linux IRQ number to be destroyed
 * @dest:	cpumask of CPUs which should have the IPI removed
 *
 * The IPIs allocated with irq_reserve_ipi() are returned to the system
 * destroying all virqs associated with them.
 *
 * Return: %0 on success or error code on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_destroy_ipi - 예약했던 IPI 를 반납한다
 *
 * @irq:    위 reserve 가 돌려준 첫 번호.
 * @dest:   반납할 CPU 들. 예약 때 준 집합의 부분집합이어야 한다.
 * @return: 0 이면 성공, 음수면 오류.
 *
 * 부분 반납을 허용하는 것이 이 함수의 특징이다. 예약 때 {0,1,2,3} 을
 * 주었더라도 {2,3} 만 반납할 수 있다.
 *
 * per-CPU 방식에서 그 부분 반납이 어떻게 성립하는가: 번호가 CPU 마다
 * 하나씩 연속으로 잡혀 있으므로, dest 의 첫 CPU 에 해당하는 번호부터
 * dest 의 개수만큼 반납하면 된다. 그 계산이 아래에 있다.
 *
 * SINGLE 방식에서는 번호가 하나뿐이라 부분 반납이 의미가 없다. 어느
 * dest 를 주든 그 하나를 반납한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   아키텍처의 정리 코드 → [이 함수] → irq_domain_free_irqs()
 */
int irq_destroy_ipi(unsigned int irq, const struct cpumask *dest)
{
	struct irq_data *data = irq_get_irq_data(irq);	/* [한국어] 첫 번호의 irq_data. 여기에 도메인과 offset 이 들어 있다 */
	const struct cpumask *ipimask;	/* [한국어] 예약 때 저장해 둔 목적지 마스크 */
	struct irq_domain *domain;	/* [한국어] 이 IPI 가 속한 도메인 */
	unsigned int nr_irqs;	/* [한국어] 반납할 번호의 개수 */

	if (!irq || !data)	/* [한국어] 0 번이거나 서술자가 없으면 */
		return -EINVAL;	/* [한국어] IPI 에 0 번이 배정될 일은 없으므로 0 도 잘못된 입력이다 */

	domain = data->domain;	/* [한국어] 도메인을 꺼낸다 */
	if (WARN_ON(domain == NULL))	/* [한국어] 서술자는 있는데 도메인이 없다면 코어의 버그다 */
		return -EINVAL;

	if (!irq_domain_is_ipi(domain)) {	/* [한국어] IPI 도메인이 아니면 */
		pr_warn("Trying to destroy a non IPI domain!\n");	/* [한국어] 호출자가 엉뚱한 번호를 넘긴 것이다 */
		return -EINVAL;	/* [한국어] 예약 집합을 벗어난 반납 요청이다 */
	}

	ipimask = irq_data_get_affinity_mask(data);	/* [한국어] 예약 때 친화도 자리에 담아 둔 목적지 마스크 */
	if (!ipimask || WARN_ON(!cpumask_subset(dest, ipimask)))	/* [한국어] 마스크가 없거나, 반납하려는 집합이 예약 집합을 벗어나는가 */
		/*
		 * Must be destroying a subset of CPUs to which this IPI
		 * was set up to target
		 */
		/* [한국어] (위 영어 주석) 예약한 CPU 들의 부분집합만 반납할 수 있다.
		 * 벗어나면 잡지도 않은 번호를 반납하려는 것이라 위험하다. */
		return -EINVAL;

	if (irq_domain_is_ipi_per_cpu(domain)) {	/* [한국어] CPU 마다 번호가 따로인 방식인가 */
		irq = irq + cpumask_first(dest) - data->common->ipi_offset;	/* [한국어] 반납할 첫 번호를 계산한다. 이 파일 전체에 반복해서 나타나는 "base + n - offset" 계산이다 */
		nr_irqs = cpumask_weight(dest);	/* [한국어] 반납할 CPU 수만큼의 번호 */
	} else {
		nr_irqs = 1;	/* [한국어] SINGLE 방식은 번호가 하나뿐이라 부분 반납이 의미가 없다 */
	}

	irq_domain_free_irqs(irq, nr_irqs);	/* [한국어] 매핑과 서술자를 함께 반납한다 */
	return 0;	/* [한국어] 성공 */
}

/**
 * ipi_get_hwirq - Get the hwirq associated with an IPI to a CPU
 * @irq:	Linux IRQ number
 * @cpu:	the target CPU
 *
 * When dealing with coprocessors IPI, we need to inform the coprocessor of
 * the hwirq it needs to use to receive and send IPIs.
 *
 * Return: hwirq value on success or INVALID_HWIRQ on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * ipi_get_hwirq - 특정 CPU 로 가는 IPI 의 하드웨어 인터럽트 번호를 알아낸다
 *
 * @irq:    위 reserve 가 돌려준 리눅스 인터럽트 번호.
 * @cpu:    목적지 CPU.
 * @return: 그 hwirq, 또는 실패 시 INVALID_HWIRQ.
 *
 * 왜 하드웨어 번호를 밖에 알려 줘야 하는가: kernel-doc 이 답한다 — 보조
 * 프로세서(coprocessor)와 IPI 를 주고받을 때다. 그 프로세서는 리눅스가
 * 아니라 자체 펌웨어로 돌아, 리눅스 인터럽트 번호라는 개념을 모른다.
 * 하드웨어 번호를 직접 알려 줘야 그것으로 IPI 를 보내고 받는다.
 *
 * SINGLE 방식과 PER_CPU 방식에서 답이 달라진다:
 *   SINGLE  — 모든 CPU 가 같은 hwirq 를 쓰므로 cpu 인자와 무관하게 같은 값.
 *             목적지 구분은 보내는 쪽이 한다.
 *   PER_CPU — CPU 마다 다른 hwirq 라, 그 CPU 의 번호로 옮겨 가서 읽어야 한다.
 *
 * 그 옮겨 가기가 "base + cpu - offset" 계산이며, 이 파일에서 세 번째로
 * 나타나는 같은 관용구다.
 *
 * INVALID_HWIRQ 를 돌려주는 세 경우: 서술자가 없거나, CPU 번호가 범위를
 * 벗어나거나, 그 CPU 가 예약 때의 목적지 집합에 없는 경우다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 보조 프로세서 초기화에서 불린다.
 *
 * 호출 체인:
 *   보조 프로세서 드라이버(remoteproc 등) → [이 함수]
 */
irq_hw_number_t ipi_get_hwirq(unsigned int irq, unsigned int cpu)
{
	struct irq_data *data = irq_get_irq_data(irq);	/* [한국어] 첫 번호의 irq_data */
	const struct cpumask *ipimask;	/* [한국어] 예약 때 저장해 둔 목적지 마스크 */

	if (!data || cpu >= nr_cpu_ids)	/* [한국어] 서술자가 없거나 CPU 번호가 범위를 벗어나는가 */
		return INVALID_HWIRQ;	/* [한국어] 0 이나 음수가 아니라 전용 상수를 쓴다 — hwirq 0 이 유효한 값일 수 있기 때문이다 */

	ipimask = irq_data_get_affinity_mask(data);	/* [한국어] 예약 때 담아 둔 목적지 집합 */
	if (!ipimask || !cpumask_test_cpu(cpu, ipimask))	/* [한국어] 마스크가 없거나 그 CPU 가 목적지에 없는가 */
		return INVALID_HWIRQ;	/* [한국어] 예약하지 않은 CPU 의 번호를 물어본 것이다 */

	/*
	 * Get the real hardware irq number if the underlying implementation
	 * uses a separate irq per cpu. If the underlying implementation uses
	 * a single hardware irq for all cpus then the IPI send mechanism
	 * needs to take care of the cpu destinations.
	 */
	/* [한국어] (위 영어 주석에 이어) 방식에 따라 갈린다.
	 *
	 * PER_CPU 면 그 CPU 에 해당하는 번호로 옮겨 가서 hwirq 를 읽는다.
	 * SINGLE 이면 옮길 필요가 없다 — 모든 CPU 가 같은 hwirq 를 쓰고,
	 * 목적지 구분은 보내는 쪽(chip->ipi_send_mask)이 한다. */
	if (irq_domain_is_ipi_per_cpu(data->domain))	/* [한국어] CPU 마다 번호가 따로인가 */
		data = irq_get_irq_data(irq + cpu - data->common->ipi_offset);	/* [한국어] 그 CPU 의 번호로 옮겨 간다. 이 파일의 세 번째 "base + n - offset" 계산이다 */

	return data ? irqd_to_hwirq(data) : INVALID_HWIRQ;	/* [한국어] 옮겨 간 번호에 서술자가 없을 수 있어 다시 확인한다 */
}
EXPORT_SYMBOL_GPL(ipi_get_hwirq);	/* [한국어] 보조 프로세서 드라이버가 모듈일 수 있어 공개한다 */

/*
 * [한국어]
 * ipi_send_verify - IPI 를 보내기 전에 인자를 검증한다
 *
 * @chip:   그 인터럽트의 컨트롤러.
 * @data:   그 인터럽트의 irq_data.
 * @dest:   여러 CPU 에 보낼 때의 목적지 집합. 한 CPU 에 보낼 때는 NULL.
 * @cpu:    한 CPU 에 보낼 때의 목적지. dest 가 있으면 무시된다.
 * @return: 0 이면 보내도 좋다, -EINVAL 이면 잘못된 요청이다.
 *
 * dest 와 cpu 중 하나만 쓴다는 것이 이 함수의 인터페이스다. dest 가
 * NULL 이 아니면 그것을 검사하고, NULL 이면 cpu 를 검사한다. 두 호출
 * 형태(single/mask)를 하나의 검증 함수로 처리하려는 설계다.
 *
 * 네 가지를 확인한다:
 *   chip 과 data 가 있을 것.
 *   chip 이 보내기 콜백을 하나라도 제공할 것 — 둘 중 하나만 있어도 된다.
 *     없는 쪽은 아래 보내기 함수들이 있는 쪽으로 대신 처리한다.
 *   CPU 번호가 범위 안일 것.
 *   목적지가 예약 때의 집합 안일 것.
 *
 * 이 검증이 싸지 않다는 점이 아래 밑줄 판들의 존재 이유다. cpumask 비교가
 * CPU 수에 비례하는 비용이라, 매 IPI 마다 하면 부담이 된다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 임의의 문맥.
 *
 * 호출 체인:
 *   ipi_send_single()/ipi_send_mask() → [이 함수]
 *   __ipi_send_single()/__ipi_send_mask() → [이 함수] (DEBUG 빌드에서만)
 */
static int ipi_send_verify(struct irq_chip *chip, struct irq_data *data,
			   const struct cpumask *dest, unsigned int cpu)
{
	const struct cpumask *ipimask;	/* [한국어] 예약 때의 목적지 집합 */

	if (!chip || !data)	/* [한국어] 잘못된 인터럽트 번호를 넘겼거나 chip 이 붙지 않은 번호다 */
		return -EINVAL;

	if (!chip->ipi_send_single && !chip->ipi_send_mask)	/* [한국어] 둘 중 하나도 없으면 보낼 방법이 없다 */
		return -EINVAL;	/* [한국어] 하나만 있어도 된다 — 아래 보내기 함수들이 있는 쪽으로 대신 처리한다 */

	if (cpu >= nr_cpu_ids)	/* [한국어] CPU 번호가 범위를 벗어나는가. dest 를 쓸 때는 호출자가 0 을 넘겨 이 검사를 통과시킨다 */
		return -EINVAL;

	ipimask = irq_data_get_affinity_mask(data);	/* [한국어] 예약 때 담아 둔 목적지 집합 */
	if (!ipimask)	/* [한국어] 예약되지 않은 인터럽트다 */
		return -EINVAL;

	/* [한국어] dest 와 cpu 중 하나만 검사한다. 두 호출 형태를 하나의
	 * 검증 함수로 처리하기 위한 구조다. */
	if (dest) {	/* [한국어] 여러 CPU 에 보내는 경우 */
		if (!cpumask_subset(dest, ipimask))	/* [한국어] 목적지가 예약 집합 안에 모두 들어 있는가 */
			return -EINVAL;	/* [한국어] 예약하지 않은 CPU 에 보내려는 것이다 */
	} else {	/* [한국어] 한 CPU 에 보내는 경우 */
		if (!cpumask_test_cpu(cpu, ipimask))	/* [한국어] 그 CPU 가 예약 집합에 있는가 */
			return -EINVAL;
	}
	return 0;	/* [한국어] 보내도 좋다 */
}

/**
 * __ipi_send_single - send an IPI to a target Linux SMP CPU
 * @desc:	pointer to irq_desc of the IRQ
 * @cpu:	destination CPU, must in the destination mask passed to
 *		irq_reserve_ipi()
 *
 * This function is for architecture or core code to speed up IPI sending. Not
 * usable from driver code.
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * __ipi_send_single - 한 CPU 에 IPI 를 보낸다 (검증을 건너뛰는 빠른 판)
 *
 * @desc:   그 IPI 의 서술자. 번호가 아니라 서술자를 받는 것이 빠른 판의 표시다.
 * @cpu:    목적지 CPU.
 * @return: 항상 0. 검증을 하지 않으므로 실패할 수 있는 단계가 없다.
 *
 * kernel-doc 이 "아키텍처나 코어 코드용이며 드라이버에서 쓸 수 없다" 고
 * 못 박는다. 그 이유는 검증을 생략하기 때문이다 — 잘못된 인자를 주면
 * 조용히 엉뚱한 CPU 를 깨우거나 널 역참조가 난다.
 *
 * 서술자를 인자로 받는 것도 같은 취지다. 번호에서 서술자를 찾는 조회도
 * 비용이라, 호출자가 이미 들고 있으면 그대로 넘기게 한다.
 *
 * 두 가지 대비를 한다:
 *   ipi_send_single 콜백이 없으면 mask 판으로 대신한다 — 한 CPU 만 든
 *     마스크를 만들어 넘긴다.
 *   PER_CPU 방식이면 그 CPU 의 번호로 옮겨 간다.
 *
 * DEBUG 빌드에서만 검증을 켜는 것에 주의: 원 주석대로 호출자를 신뢰할 수
 * 있다고 보고 평소에는 생략하되, 개발 중에는 잡아낸다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 임의의 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   아키텍처의 SMP 코드 → [이 함수] → chip->ipi_send_single/mask
 */
int __ipi_send_single(struct irq_desc *desc, unsigned int cpu)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);	/* [한국어] 서술자에서 irq_data 를 꺼낸다 */
	struct irq_chip *chip = irq_data_get_irq_chip(data);	/* [한국어] 보내기 콜백을 가진 컨트롤러 */

#ifdef DEBUG	/* [한국어] 개발용 빌드에서만 검증한다 */
	/*
	 * Minimise the overhead by omitting the checks for Linux SMP IPIs.
	 * Since the callers should be arch or core code which is generally
	 * trusted, only check for errors when debugging.
	 */
	/* [한국어] (위 영어 주석) 호출자를 신뢰해 평소에는 검증을 생략한다.
	 *
	 * IPI 는 스케줄러와 TLB 무효화가 쓰는 가장 뜨거운 경로 중 하나다.
	 * cpumask 비교를 매번 하면 그만큼 지연이 붙는다. */
	if (WARN_ON_ONCE(ipi_send_verify(chip, data, NULL, cpu)))	/* [한국어] dest 에 NULL 을 넘겨 cpu 쪽을 검사하게 한다 */
		return -EINVAL;
#endif
	if (!chip->ipi_send_single) {	/* [한국어] 한 CPU 전용 콜백이 없으면 */
		chip->ipi_send_mask(data, cpumask_of(cpu));	/* [한국어] 마스크 판으로 대신한다. cpumask_of 는 그 CPU 하나만 든 정적 마스크를 돌려준다 */
		return 0;	/* [한국어] 아래 PER_CPU 처리를 건너뛴다 — mask 판이 목적지를 알아서 다룬다 */
	}

	/* FIXME: Store this information in irqdata flags */
	/* [한국어] (위 영어 주석의 FIXME) 도메인 플래그를 매번 조회하는 대신
	 * irq_data 에 캐시해 두면 좋겠다는 뜻이다.
	 *
	 * 왜 아직 그렇게 하지 않았는가: irq_data 에 필드를 더하면 모든 인터럽트가
	 * 그만큼 커진다. IPI 는 시스템에 몇 개뿐이라 이득이 크지 않다. */
	if (irq_domain_is_ipi_per_cpu(data->domain) &&	/* [한국어] CPU 마다 번호가 따로인 방식인가 */
	    cpu != data->common->ipi_offset) {	/* [한국어] 첫 CPU 가 아니면 옮겨 가야 한다. 첫 CPU 면 지금 data 가 이미 그것이다 */
		/* use the correct data for that cpu */
		/* [한국어] (위 영어 주석) 그 CPU 에 해당하는 irq_data 로 바꾼다. */
		unsigned irq = data->irq + cpu - data->common->ipi_offset;	/* [한국어] 이 파일의 네 번째 "base + n - offset" 계산 */

		data = irq_get_irq_data(irq);	/* [한국어] 그 번호의 irq_data 로 갈아 끼운다 */
	}
	chip->ipi_send_single(data, cpu);	/* [한국어] 실제로 보낸다. PER_CPU 면 위에서 바꾼 data 가 그 CPU 의 hwirq 를 담고 있다 */
	return 0;	/* [한국어] 콜백이 void 라 실패를 알 방법이 없다 */
}

/**
 * __ipi_send_mask - send an IPI to target Linux SMP CPU(s)
 * @desc:	pointer to irq_desc of the IRQ
 * @dest:	dest CPU(s), must be a subset of the mask passed to
 *		irq_reserve_ipi()
 *
 * This function is for architecture or core code to speed up IPI sending. Not
 * usable from driver code.
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * __ipi_send_mask - 여러 CPU 에 IPI 를 보낸다 (검증을 건너뛰는 빠른 판)
 *
 * @desc:   그 IPI 의 서술자.
 * @dest:   목적지 CPU 집합. 예약 때의 집합의 부분집합이어야 한다.
 * @return: 항상 0.
 *
 * 위 single 판의 대응이며, 세 갈래로 나뉜다.
 *
 *   chip 이 mask 판을 제공하면 그것 한 번으로 끝난다. 가장 효율적이다 —
 *     하드웨어가 여러 CPU 에 한 번에 보낼 수 있는 경우다.
 *   없으면 single 판을 CPU 마다 반복한다. 그때 PER_CPU 방식이면 매번
 *     그 CPU 의 irq_data 로 옮겨 가야 한다.
 *   SINGLE 방식이면 옮길 필요 없이 같은 data 로 반복한다.
 *
 * 두 번째 갈래에서 data 를 루프 안에서 덮어쓰는 것에 주의한다. base 를
 * 미리 저장해 두는 이유가 그것이다 — data->irq 가 매 반복마다 바뀌므로
 * 기준으로 쓸 수 없다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 임의의 문맥.
 *
 * 호출 체인:
 *   아키텍처의 SMP 코드, smp_call_function() → [이 함수]
 *     → chip->ipi_send_mask 또는 반복적인 ipi_send_single
 */
int __ipi_send_mask(struct irq_desc *desc, const struct cpumask *dest)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);	/* [한국어] 첫 번호의 irq_data */
	struct irq_chip *chip = irq_data_get_irq_chip(data);	/* [한국어] 보내기 콜백을 가진 컨트롤러 */
	unsigned int cpu;	/* [한국어] 순회 중인 목적지 CPU */

#ifdef DEBUG	/* [한국어] 개발용 빌드에서만 검증 */
	/*
	 * Minimise the overhead by omitting the checks for Linux SMP IPIs.
	 * Since the callers should be arch or core code which is generally
	 * trusted, only check for errors when debugging.
	 */
	/* [한국어] (위 영어 주석) 위 single 판과 같은 이유로 평소에는 생략한다. */
	if (WARN_ON_ONCE(ipi_send_verify(chip, data, dest, 0)))	/* [한국어] dest 를 넘겨 그쪽을 검사하게 한다. cpu 자리의 0 은 쓰이지 않지만 범위 검사를 통과해야 해서 유효한 값이어야 한다 */
		return -EINVAL;
#endif
	if (chip->ipi_send_mask) {	/* [한국어] 여러 CPU 에 한 번에 보낼 수 있는가 */
		chip->ipi_send_mask(data, dest);	/* [한국어] 가장 효율적인 경로. 하드웨어가 한 번의 조작으로 여러 CPU 를 깨운다 */
		return 0;	/* [한국어] 콜백이 void 라 실패를 알 방법이 없다 */
	}

	/* [한국어] mask 콜백이 없으면 single 판을 반복한다. 아래 두 갈래는
	 * PER_CPU 여부에 따라 매번 irq_data 를 옮길지가 다르다. */
	if (irq_domain_is_ipi_per_cpu(data->domain)) {	/* [한국어] CPU 마다 번호가 따로인가 */
		unsigned int base = data->irq;	/* [한국어] 루프에서 data 를 덮어쓰므로 기준 번호를 미리 저장해 둔다 */

		for_each_cpu(cpu, dest) {	/* [한국어] 목적지를 하나씩 */
			unsigned irq = base + cpu - data->common->ipi_offset;	/* [한국어] 이 파일의 다섯 번째 "base + n - offset" 계산 */

			data = irq_get_irq_data(irq);	/* [한국어] 그 CPU 의 irq_data 로 갈아 끼운다. common 은 모든 번호가 공유하므로 offset 은 계속 유효하다 */
			chip->ipi_send_single(data, cpu);	/* [한국어] 그 CPU 에 보낸다 */
		}
	} else {
		for_each_cpu(cpu, dest)	/* [한국어] SINGLE 방식은 모든 CPU 가 같은 hwirq 를 쓴다 */
			chip->ipi_send_single(data, cpu);	/* [한국어] 같은 data 로 반복한다. 목적지 구분은 cpu 인자가 한다 */
	}
	return 0;	/* [한국어] 콜백이 void 라 실패를 알 방법이 없다 */
}

/**
 * ipi_send_single - Send an IPI to a single CPU
 * @virq:	Linux IRQ number from irq_reserve_ipi()
 * @cpu:	destination CPU, must in the destination mask passed to
 *		irq_reserve_ipi()
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * ipi_send_single - 한 CPU 에 IPI 를 보낸다 (검증을 하는 공개 API)
 *
 * @virq:   위 reserve 가 돌려준 리눅스 인터럽트 번호.
 * @cpu:    목적지 CPU.
 * @return: 0 이면 성공, -EINVAL 이면 잘못된 요청이다.
 *
 * 위 밑줄 판과 두 가지가 다르다:
 *   번호를 받아 서술자를 조회한다 — 호출자가 서술자를 들고 있지 않아도 된다.
 *   검증을 항상 한다 — DEBUG 빌드가 아니어도.
 *
 * 그래서 드라이버나 일반 커널 코드가 쓸 수 있다. 대신 매 호출마다
 * 서술자 조회와 cpumask 비교의 비용이 든다.
 *
 * 삼항 연산자가 이어지는 것에 주의: 서술자가 없으면 data 도 NULL 이고,
 * 그러면 chip 도 NULL 이 된다. 그 셋을 아래 verify 가 한 번에 걸러 낸다 —
 * 각 단계마다 if 를 두는 대신 널 전파로 처리한 것이다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 임의의 문맥.
 *
 * 호출 체인:
 *   커널 코드 → [이 함수] → ipi_send_verify() → __ipi_send_single()
 */
int ipi_send_single(unsigned int virq, unsigned int cpu)
{
	struct irq_desc *desc = irq_to_desc(virq);	/* [한국어] 번호로 서술자를 찾는다. 없으면 NULL */
	struct irq_data *data = desc ? irq_desc_get_irq_data(desc) : NULL;	/* [한국어] 널 전파. 서술자가 없으면 여기도 NULL 이다 */
	struct irq_chip *chip = data ? irq_data_get_irq_chip(data) : NULL;	/* [한국어] 계속 전파. 아래 verify 가 세 경우를 한 번에 걸러 낸다 */

	if (WARN_ON_ONCE(ipi_send_verify(chip, data, NULL, cpu)))	/* [한국어] 항상 검증한다. WARN 인 이유는 이 API 를 잘못 쓰는 것이 호출자의 버그이기 때문이다 */
		return -EINVAL;

	return __ipi_send_single(desc, cpu);	/* [한국어] 검증을 마쳤으니 빠른 판에 맡긴다. DEBUG 빌드에서는 검증이 두 번 일어나지만 개발용이라 상관없다 */
}
EXPORT_SYMBOL_GPL(ipi_send_single);	/* [한국어] 모듈에 공개. 보조 프로세서를 다루는 드라이버 등이 쓴다 */

/**
 * ipi_send_mask - Send an IPI to target CPU(s)
 * @virq:	Linux IRQ number from irq_reserve_ipi()
 * @dest:	dest CPU(s), must be a subset of the mask passed to
 *		irq_reserve_ipi()
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * ipi_send_mask - 여러 CPU 에 IPI 를 보낸다 (검증을 하는 공개 API)
 *
 * @virq:   위 reserve 가 돌려준 리눅스 인터럽트 번호.
 * @dest:   목적지 CPU 집합.
 * @return: 0 이면 성공, -EINVAL 이면 잘못된 요청이다.
 *
 * 위 ipi_send_single() 과 완전히 대칭인 구조다. 번호로 서술자를 찾고,
 * 널 전파로 세 포인터를 얻고, 검증한 뒤 빠른 판에 맡긴다.
 *
 * verify 에 넘기는 인자만 다르다 — dest 를 주고 cpu 자리에 0 을 넘겨,
 * 그쪽이 dest 검사 경로를 타게 한다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 임의의 문맥.
 *
 * 호출 체인:
 *   커널 코드 → [이 함수] → ipi_send_verify() → __ipi_send_mask()
 */
int ipi_send_mask(unsigned int virq, const struct cpumask *dest)
{
	struct irq_desc *desc = irq_to_desc(virq);	/* [한국어] 번호로 서술자를 찾는다 */
	struct irq_data *data = desc ? irq_desc_get_irq_data(desc) : NULL;	/* [한국어] 널 전파 */
	struct irq_chip *chip = data ? irq_data_get_irq_chip(data) : NULL;	/* [한국어] 계속 전파 */

	if (WARN_ON_ONCE(ipi_send_verify(chip, data, dest, 0)))	/* [한국어] dest 를 넘겨 그쪽 경로로 검사하게 한다. 0 은 쓰이지 않지만 범위 검사를 통과해야 한다 */
		return -EINVAL;

	return __ipi_send_mask(desc, dest);	/* [한국어] 검증을 마쳤으니 빠른 판에 맡긴다 */
}
EXPORT_SYMBOL_GPL(ipi_send_mask);	/* [한국어] 모듈에 공개 */
