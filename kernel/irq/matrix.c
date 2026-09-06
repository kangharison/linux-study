// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2017 Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
/*
 * [한국어 설명] CPU 별 인터럽트 벡터를 분배하는 비트맵 할당기 (matrix.c)
 *
 * === 파일의 역할 ===
 * "CPU × 벡터 번호" 의 2차원 공간에서 인터럽트 벡터를 배정하는 자료구조와
 * 그 연산을 제공한다. 이름의 matrix 가 그 2차원을 뜻한다.
 *
 * 무엇을 위한 것인가: x86 에서 인터럽트가 CPU 에 도달하려면 그 CPU 의
 * 벡터 번호 하나를 차지해야 한다. 벡터는 CPU 마다 256개뿐이고 그중 상당수가
 * 시스템 용도로 예약되어 있어, 실제로 쓸 수 있는 것은 200개 남짓이다.
 * 장치가 많은 서버에서는 이 자원이 실제로 고갈되므로 신중한 배분이 필요하다.
 *
 * 이 파일이 푸는 문제 셋:
 *   어느 CPU 에 배정할 것인가 — 가장 여유 있는 CPU 를 고른다.
 *   managed 인터럽트를 어떻게 다룰 것인가 — 미리 자리를 잡아 두어야 한다.
 *   CPU 가 오르내릴 때 회계를 어떻게 맞출 것인가.
 *
 * 두 종류의 비트맵이 CPU 마다 있다는 것이 이 파일의 핵심 구조다:
 *   alloc_map   — 실제로 배정된 벡터들.
 *   managed_map — managed 인터럽트를 위해 잡아 둔 자리들.
 *
 * managed 를 따로 관리하는 이유: managed 인터럽트는 특정 CPU 집합에 묶인
 * 장치 큐의 것이라, 그 CPU 가 지금 오프라인이어도 자리를 미리 확보해
 * 두어야 한다. 나중에 CPU 가 켜졌을 때 벡터가 없어 실패하면 그 큐를
 * 영영 쓸 수 없기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * x86 의 벡터 관리 계층 아래에 있다:
 *
 *   request_irq() → irq_activate() → 도메인 계층
 *     ↓ x86 의 vector domain
 *   assign_vector_locked() (arch/x86/kernel/apic/vector.c)
 *     ↓
 *   irq_matrix_alloc()               ← **이 파일**
 *     ↓ 가장 여유 있는 CPU 를 고르고 비트를 세운다
 *   그 CPU 의 벡터 번호를 반환
 *
 *   CPU 핫플러그:
 *     lapic_online()/offline() → irq_matrix_online()/offline()  ← **이 파일**
 *
 * 실행 컨텍스트: 대부분 인터럽트가 꺼진 상태에서 벡터 락을 쥔 채 불린다.
 * 이 파일 자체는 락을 잡지 않는다 — 호출자(x86 벡터 관리)가 책임진다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   bitmap 기반 — 모든 연산이 비트맵 조작이다.
 *   percpu — CPU 마다의 맵.
 *   trace/events/irq_matrix.h — 모든 연산에 추적점이 붙어 있다.
 *
 * 이 파일에 의존하는 곳: arch/x86/kernel/apic/vector.c 가 사실상 유일한
 *   사용자다. 다른 아키텍처는 벡터라는 개념이 없거나 다르게 관리한다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct cpumap           — CPU 하나의 벡터 상태. 두 비트맵과 네 카운터.
 * struct irq_matrix       — 전체 상태. 전역 카운터와 시스템 비트맵.
 * irq_alloc_matrix()      — 행렬을 만든다. 부팅 때 한 번.
 * irq_matrix_online/offline() — CPU 를 회계에 넣고 뺀다.
 * matrix_alloc_area()     — 빈 자리를 찾아 비트를 세운다.
 * matrix_find_best_cpu()  — 가장 여유 있는 CPU 를 고른다.
 * matrix_find_best_cpu_managed() — managed 가 가장 적은 CPU 를 고른다.
 * irq_matrix_assign_system() — 시스템 벡터를 못 박는다.
 * irq_matrix_reserve_managed()/remove_managed() — managed 자리 확보와 반납.
 * irq_matrix_alloc_managed() — 확보해 둔 자리에서 실제 배정.
 * irq_matrix_alloc()      — 보통의 인터럽트 배정.
 * irq_matrix_free()       — 반납.
 * irq_matrix_debug_show() — debugfs 출력.
 *
 * 회계가 여러 겹이라는 점이 이 파일을 읽기 어렵게 만든다. available,
 * allocated, managed, managed_allocated 가 CPU 마다 있고, global_available,
 * global_reserved, total_allocated 가 전역에 있다. 각 함수가 그중 무엇을
 * 어떻게 바꾸는지가 정확해야 벡터 고갈 판정이 맞는다.
 */

#include <linux/spinlock.h>	/* [한국어] 락 타입 정의. 이 파일은 직접 잡지 않지만 헤더가 요구한다 */
#include <linux/seq_file.h>	/* [한국어] 아래 debug_show 의 출력 */
#include <linux/bitmap.h>	/* [한국어] bitmap_or/andnot/find_next_zero_area — 이 파일의 모든 연산이 비트맵 조작이다 */
#include <linux/percpu.h>	/* [한국어] __alloc_percpu — CPU 마다의 맵 */
#include <linux/cpu.h>	/* [한국어] for_each_possible_cpu, cpus_read_lock */
#include <linux/irq.h>	/* [한국어] struct irq_matrix 의 공개 선언 */

/* [한국어] CPU 하나의 벡터 할당 상태.
 *
 * 네 카운터와 두 비트맵의 관계를 아는 것이 이 파일을 읽는 열쇠다:
 *
 *   alloc_map 에 선 비트의 수  = allocated
 *   managed_map 에 선 비트의 수 = managed
 *   그중 alloc_map 에도 선 것    = managed_allocated
 *   available = 전체 자리 - managed - 시스템 - allocated 중 관리 외의 것
 *
 * 카운터를 따로 두는 이유: 비트맵을 세는 것은 O(비트 수)이고, 이 값들은
 * 벡터를 배정할 때마다 읽힌다. 매번 세면 비싸므로 갱신할 때마다 카운터를
 * 함께 조정한다. 그 조정이 정확하지 않으면 회계가 어긋난다. */
struct cpumap {
	unsigned int		available;
	/* [한국어] 이 CPU 에서 아직 배정할 수 있는 벡터의 수.
	 * 설정자: online 이 초기값을 계산하고, alloc/free/reserve_managed 등이 조정한다.
	 * 읽는 자: matrix_find_best_cpu() 가 가장 여유 있는 CPU 를 고를 때,
	 *   그리고 debugfs 출력.
	 * 값 범위: 0 이면 이 CPU 에 더 배정할 수 없다.
	 * 무엇이 빠져 있는가: managed 로 잡아 둔 자리와 시스템 벡터는 이미
	 *   제외되어 있다. 그래서 managed 인터럽트를 배정해도 이 값은 줄지
	 *   않는다 — 이미 빠져 있기 때문이다. */

	unsigned int		allocated;
	/* [한국어] 이 CPU 에 실제로 배정된 벡터의 총수(managed 포함).
	 * 설정자: alloc/alloc_managed/assign 이 올리고 free 가 내린다.
	 * 읽는 자: debugfs 출력과 irq_matrix_allocated().
	 * 값 범위: alloc_map 에 선 비트의 수와 언제나 같아야 한다.
	 * 아래 managed_allocated 를 포함한다는 점에 주의 — 보통의 인터럽트만
	 *   세려면 둘을 빼야 하고, 그것이 irq_matrix_allocated() 다. */

	unsigned int		managed;
	/* [한국어] managed 인터럽트를 위해 잡아 둔 자리의 수.
	 * 설정자: reserve_managed 가 올리고 remove_managed 가 내린다.
	 * 읽는 자: online 이 available 초기값을 계산할 때, debugfs 출력.
	 * 값 범위: managed_map 에 선 비트의 수와 같아야 한다.
	 * 왜 미리 잡아 두는가: managed 인터럽트는 특정 CPU 에 묶인 큐의
	 *   것이라, 나중에 그 CPU 가 켜졌을 때 벡터가 없으면 그 큐를 영영
	 *   쓸 수 없다. 오프라인 CPU 에도 미리 자리를 확보한다. */

	unsigned int		managed_allocated;
	/* [한국어] 잡아 둔 managed 자리 중 실제로 배정된 수.
	 * 설정자: alloc_managed 가 올리고 free 가(managed 인자가 참일 때) 내린다.
	 * 읽는 자: matrix_find_best_cpu_managed() 가 가장 한가한 CPU 를 고를 때.
	 * 값 범위: 0 이상 managed 이하.
	 * 위 managed 와의 차이: managed 는 "자리를 잡아 두었다", 이쪽은
	 *   "그 자리를 실제로 쓰고 있다" 이다. 둘의 차이가 아직 활성화되지
	 *   않은 managed 인터럽트의 수다. */

	bool			initialized;
	/* [한국어] 이 CPU 의 available 초기값이 계산되었는가.
	 * 설정자: irq_matrix_online() 이 처음 한 번 참으로 만든다.
	 * 읽는 자: 같은 함수가 다시 계산할지 정할 때.
	 * 왜 필요한가: CPU 는 여러 번 오르내릴 수 있다. 두 번째 online 에서
	 *   available 을 다시 계산하면, 그 사이에 배정된 벡터들이 회계에서
	 *   사라져 실제보다 많은 여유가 있다고 착각한다. */

	bool			online;
	/* [한국어] 이 CPU 가 지금 온라인인가.
	 * 설정자: irq_matrix_online/offline().
	 * 읽는 자: 두 find_best_cpu 가 후보에서 제외할 때, alloc/free 가
	 *   전역 카운터를 조정할지 정할 때.
	 * 값 범위: 오프라인 CPU 에도 managed 자리를 확보할 수 있으므로,
	 *   거짓이라고 해서 이 맵이 비어 있는 것은 아니다. */

	unsigned long		*managed_map;
	/* [한국어] managed 인터럽트를 위해 잡아 둔 자리의 비트맵.
	 * 설정자: matrix_alloc_area(managed=true) 가 세우고 remove_managed 가 지운다.
	 * 읽는 자: alloc_managed 가 빈 자리를 찾을 때, matrix_alloc_area 가
	 *   보통의 인터럽트를 그 자리에 배정하지 않도록 피할 때.
	 * 값 범위: alloc_end 비트까지.
	 * 메모리 배치: 아래 alloc_map 의 가변 길이 배열 뒤쪽 절반을 가리킨다.
	 *   irq_alloc_matrix() 가 두 배 크기로 잡고 나눠 쓴다 — 할당을 한 번만
	 *   하려는 것이다. */

	unsigned long		alloc_map[];
	/* [한국어] 실제로 배정된 벡터의 비트맵. 구조체 끝의 가변 길이 배열이다.
	 * 설정자: matrix_alloc_area/assign 이 세우고 free 가 지운다.
	 * 읽는 자: 빈 자리를 찾는 모든 경로, 그리고 debugfs 출력.
	 * 값 범위: matrix_bits 비트.
	 * 크기가 가변인 이유: 벡터 수가 아키텍처마다 다르다. 구조체 끝에
	 *   두면 한 번의 할당으로 구조체와 비트맵을 함께 잡을 수 있다.
	 * 실제로는 두 배 크기로 잡혀, 뒤쪽 절반을 위 managed_map 이 쓴다. */
};

/* [한국어] 행렬 전체의 상태.
 *
 * 위 cpumap 이 CPU 하나씩의 상태라면 이쪽은 전역 상태와 설정이다.
 * 전역 카운터 셋이 벡터 고갈 판정의 근거가 된다. */
struct irq_matrix {
	unsigned int		matrix_bits;
	/* [한국어] 벡터 번호 공간의 전체 크기(비트 수).
	 * 설정자: irq_alloc_matrix() 가 받은 값.
	 * 읽는 자: 비트맵 크기 계산과 debugfs 출력.
	 * 값 범위: x86 이면 256(NR_VECTORS). 그중 일부만 아래 alloc 범위다. */

	unsigned int		alloc_start;
	/* [한국어] 동적 배정에 쓸 수 있는 첫 벡터 번호.
	 * 설정자: irq_alloc_matrix() 가 받은 값.
	 * 읽는 자: matrix_alloc_area 가 탐색을 시작할 위치.
	 * 왜 0 이 아닌가: 낮은 벡터 번호는 CPU 예외와 시스템 인터럽트가
	 *   쓴다. x86 이면 0~31 이 예외라 그 뒤부터 시작한다. */

	unsigned int		alloc_end;
	/* [한국어] 동적 배정 범위의 끝(포함하지 않음).
	 * 설정자: irq_alloc_matrix() 가 받은 값.
	 * 읽는 자: 탐색의 상한. 이 값 이상이 나오면 자리가 없다는 뜻이다.
	 * 왜 matrix_bits 가 아닌가: 높은 벡터 번호도 시스템이 쓴다. x86 이면
	 *   IPI 와 APIC 오류 벡터가 위쪽을 차지한다. */

	unsigned int		alloc_size;
	/* [한국어] 동적 배정 범위의 크기. alloc_end - alloc_start 다.
	 * 설정자: irq_alloc_matrix() 가 계산해 담는다.
	 * 읽는 자: irq_matrix_online() 이 available 초기값을 계산할 때.
	 * 왜 미리 계산해 두는가: 매번 빼는 것보다 낫고, 그 계산이 회계의
	 *   기준이라 한 곳에 못 박아 두는 편이 안전하다. */

	unsigned int		global_available;
	/* [한국어] 온라인 CPU 들의 available 을 모두 더한 값.
	 * 설정자: online/offline 이 그 CPU 몫을 더하고 빼며, alloc/free 가 조정한다.
	 * 읽는 자: irq_matrix_available() 과 debugfs 출력, 그리고 아래
	 *   global_reserved 와 비교해 고갈을 경고할 때.
	 * 값 범위: 0 이면 시스템 전체에 벡터가 남지 않았다.
	 * 왜 전역 합계를 따로 두는가: 매번 모든 CPU 를 더하는 것은 CPU 가
	 *   수백 개인 기계에서 비싸다. */

	unsigned int		global_reserved;
	/* [한국어] 예약만 되고 아직 실제 벡터를 받지 않은 인터럽트의 수.
	 * 설정자: irq_matrix_reserve() 가 올리고, remove_reserved 와
	 *   alloc(reserved=true) 이 내린다.
	 * 읽는 자: irq_matrix_reserved() 와 debugfs, 그리고 reserve 가
	 *   고갈을 경고할 때.
	 * 무엇을 위한 것인가: 인터럽트를 만들어 두되 실제 벡터는 활성화
	 *   시점에 배정하는 방식이 있다. 그 사이의 인터럽트를 세어, 벡터가
	 *   모자랄 상황을 미리 경고한다. */

	unsigned int		systembits_inalloc;
	/* [한국어] 동적 배정 범위 안에 있는 시스템 벡터의 수.
	 * 설정자: irq_matrix_assign_system() 이 그 범위에 드는 비트마다 올린다.
	 * 읽는 자: irq_matrix_online() 이 available 초기값에서 뺄 때.
	 * 왜 필요한가: 시스템 벡터가 alloc 범위 밖에 있으면 신경 쓸 필요가
	 *   없지만, 안에 있으면 그만큼 쓸 수 있는 자리가 줄어든다. 그 수를
	 *   세어 두어야 available 초기값이 맞는다. */

	unsigned int		total_allocated;
	/* [한국어] 온라인 CPU 들에 배정된 벡터의 총수.
	 * 설정자: 모든 alloc 계열이 올리고 free 가 내린다.
	 * 읽는 자: debugfs 출력.
	 * free 가 online 인 CPU 에 대해서만 내리는 것에 주의 — 오프라인
	 *   CPU 의 벡터는 이 합계에 들어 있지 않기 때문이다. 그 비대칭이
	 *   아래 free 함수의 조건문에 나타난다. */

	unsigned int		online_maps;
	/* [한국어] 지금 온라인인 CPU 맵의 수.
	 * 설정자: online 이 올리고 offline 이 내린다.
	 * 읽는 자: assign_system 이 정합성을 검사할 때, debugfs 출력.
	 * assign_system 의 BUG_ON 이 이 값을 쓰는 이유: 시스템 벡터를 못
	 *   박는 것은 부팅 초기, CPU 가 하나뿐일 때만 안전하다. */

	struct cpumap __percpu	*maps;
	/* [한국어] CPU 마다의 위 cpumap.
	 * 설정자: irq_alloc_matrix() 가 __alloc_percpu 로 잡는다.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 왜 percpu 인가: 각 CPU 의 벡터 공간은 독립이고, 대부분의 연산이
	 *   자기 CPU 것만 건드린다. percpu 로 두면 캐시 줄 경쟁이 없다. */

	unsigned long		*system_map;
	/* [한국어] 시스템이 못 박은 벡터들의 비트맵. 모든 CPU 에 공통이다.
	 * 설정자: irq_matrix_assign_system().
	 * 읽는 자: matrix_alloc_area 가 그 자리를 피할 때, debugfs 출력.
	 * 왜 CPU 별이 아닌가: 시스템 벡터(예외, IPI, APIC 타이머)는 모든
	 *   CPU 에서 같은 번호를 쓴다. CPU 마다 따로 둘 이유가 없다.
	 * 메모리 배치: 아래 scratch_map 의 뒤쪽 절반을 가리킨다. */

	unsigned long		scratch_map[];
	/* [한국어] 임시 계산용 비트맵. 구조체 끝의 가변 길이 배열이다.
	 * 설정자/읽는 자: matrix_alloc_area 가 세 비트맵을 합칠 때,
	 *   remove_managed/alloc_managed 가 차집합을 구할 때.
	 * 왜 공용 임시 버퍼를 두는가: 그 계산에 비트맵 하나가 필요한데,
	 *   호출마다 할당하면 비싸고 스택에 두기에는 크다. 호출자가 락으로
	 *   직렬화되어 있어 하나를 공유해도 안전하다.
	 * 동기화: 락이 없다. 이 파일의 함수들이 벡터 락 아래에서만 불린다는
	 *   전제에 기대며, 그 전제가 깨지면 계산이 뒤섞인다.
	 * 실제로는 두 배 크기로 잡혀 뒤쪽 절반을 위 system_map 이 쓴다. */
};

#define CREATE_TRACE_POINTS	/* [한국어] 아래 헤더의 추적점을 이 파일에서 실제로 정의하게 한다. 여러 파일이 같은 헤더를 포함해도 정의는 한 곳에서만 이루어져야 한다 */
#include <trace/events/irq_matrix.h>	/* [한국어] trace_irq_matrix_* 추적점들. 이 파일의 거의 모든 함수가 끝에 하나씩 부른다 — 벡터 회계는 어긋나면 진단이 매우 어려워 추적이 촘촘하다 */

/**
 * irq_alloc_matrix - Allocate a irq_matrix structure and initialize it
 * @matrix_bits:	Number of matrix bits
 * @alloc_start:	From which bit the allocation search starts
 * @alloc_end:		At which bit the allocation search ends, i.e first
 *			invalid bit
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_alloc_matrix - 행렬을 만들고 초기화한다
 *
 * @matrix_bits: 벡터 번호 공간의 전체 크기.
 * @alloc_start: 동적 배정에 쓸 첫 번호.
 * @alloc_end:   그 범위의 끝(포함하지 않음).
 * @return:      만들어진 행렬, 또는 할당 실패 시 NULL.
 *
 * 부팅 때 한 번만 불린다(__init 이 그것을 말한다). 그 뒤로 시스템이
 * 살아 있는 동안 이 행렬이 유지되며, 해제하는 함수가 없다.
 *
 * 두 번의 "두 배 할당" 이 이 함수의 특징이다. 비트맵을 두 개 잡아야 하는데
 * 각각 따로 할당하는 대신 두 배 크기로 한 번 잡고 절반씩 나눠 쓴다.
 *
 *   scratch_map 을 두 배로 → 뒤쪽 절반이 system_map.
 *   alloc_map 을 두 배로   → 뒤쪽 절반이 managed_map.
 *
 * 왜 그렇게 하는가: 할당 횟수가 줄고, 두 비트맵이 같은 캐시 줄에 들어올
 * 가능성이 높아진다. 대신 코드를 읽을 때 그 배치를 알아야 한다.
 *
 * 실행 컨텍스트: 부팅 초기, 프로세스 문맥.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 초기화 → [이 함수]
 */
__init struct irq_matrix *irq_alloc_matrix(unsigned int matrix_bits,
					   unsigned int alloc_start,
					   unsigned int alloc_end)
{
	unsigned int cpu, matrix_size = BITS_TO_LONGS(matrix_bits);	/* [한국어] 비트 수를 unsigned long 개수로 바꾼다. 비트맵의 실제 크기다 */
	struct irq_matrix *m;	/* [한국어] 만들 행렬 */

	m = kzalloc_flex(*m, scratch_map, matrix_size * 2);	/* [한국어] 두 배로 잡는다 — 앞쪽 절반이 scratch_map, 뒤쪽이 system_map 이 된다 */
	if (!m)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 호출자가 확인해야 한다. 부팅 초기라 실패하면 사실상 부팅이 불가능하다 */

	m->system_map = &m->scratch_map[matrix_size];	/* [한국어] 뒤쪽 절반을 가리키게 한다. 두 비트맵이 한 할당 안에 나란히 놓인다 */

	m->matrix_bits = matrix_bits;	/* [한국어] 전체 번호 공간의 크기 */
	m->alloc_start = alloc_start;	/* [한국어] 동적 배정의 시작 */
	m->alloc_end = alloc_end;	/* [한국어] 그 끝 */
	m->alloc_size = alloc_end - alloc_start;	/* [한국어] 미리 계산해 둔다. available 초기값 계산의 기준이 된다 */
	m->maps = __alloc_percpu(struct_size(m->maps, alloc_map, matrix_size * 2),	/* [한국어] CPU 마다의 맵. 역시 두 배로 잡아 뒤쪽 절반을 managed_map 이 쓴다 */
				 __alignof__(*m->maps));	/* [한국어] 구조체의 정렬 요구. percpu 할당은 정렬을 명시해야 한다 */
	if (!m->maps) {	/* [한국어] 할당 실패 */
		kfree(m);	/* [한국어] 위에서 잡은 행렬을 되돌린다 */
		return NULL;	/* [한국어] percpu 맵을 잡지 못했다. 위에서 잡은 행렬은 이미 반납했다 */
	}

	for_each_possible_cpu(cpu) {	/* [한국어] online 이 아니라 possible 이다 — 나중에 켜질 CPU 의 맵도 지금 준비해야 한다 */
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */

		cm->managed_map = &cm->alloc_map[matrix_size];	/* [한국어] 뒤쪽 절반을 가리키게 한다. 위 system_map 과 같은 방식이다 */
	}

	return m;	/* [한국어] 이 행렬은 해제되지 않고 시스템 수명 동안 유지된다 */
}

/**
 * irq_matrix_online - Bring the local CPU matrix online
 * @m:		Matrix pointer
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_online - 이 CPU 의 맵을 회계에 넣는다
 *
 * @m: 행렬.
 *
 * CPU 가 온라인이 될 때 불린다. 그 CPU 의 available 을 전역 합계에 더하고
 * 온라인으로 표시한다.
 *
 * initialized 검사가 이 함수의 핵심이다. CPU 는 여러 번 오르내릴 수 있는데,
 * available 을 매번 다시 계산하면 그 사이에 배정된 벡터들이 회계에서
 * 사라진다. 처음 한 번만 계산하고 그 뒤로는 alloc/free 가 유지한 값을 쓴다.
 *
 * 초기값 계산: 동적 배정 범위 전체에서 managed 로 잡아 둔 자리와 그 범위
 * 안의 시스템 벡터를 뺀다. 그 둘은 이미 임자가 있는 자리다.
 *
 * managed 를 빼는 것이 중요하다 — 그래서 나중에 managed 인터럽트를 실제로
 * 배정해도 available 이 줄지 않는다. 이미 빠져 있기 때문이다.
 *
 * this_cpu_ptr 을 쓰므로 반드시 그 CPU 위에서 불려야 한다.
 *
 * 실행 컨텍스트: CPU 온라인 경로, 그 CPU 위에서. 인터럽트가 꺼진 상태.
 *
 * 호출 체인:
 *   lapic_online() (arch/x86/kernel/apic/vector.c) → [이 함수]
 */
void irq_matrix_online(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵. 이 함수는 그 CPU 위에서 불려야 한다 */

	BUG_ON(cm->online);	/* [한국어] 이미 온라인인데 또 부르면 회계가 두 배로 어긋난다. 부팅 경로의 버그이므로 곧바로 멈춘다 */

	if (!cm->initialized) {	/* [한국어] 처음 온라인이 되는가 */
		cm->available = m->alloc_size;	/* [한국어] 동적 배정 범위 전체에서 시작한다 */
		cm->available -= cm->managed + m->systembits_inalloc;	/* [한국어] 이미 임자가 있는 자리를 뺀다 — managed 로 잡아 둔 것과 그 범위 안의 시스템 벡터 */
		cm->initialized = true;	/* [한국어] 다시 계산하지 않도록 표시한다. 두 번째 online 에서 재계산하면 그 사이 배정된 벡터가 회계에서 사라진다 */
	}
	m->global_available += cm->available;	/* [한국어] 전역 합계에 이 CPU 몫을 더한다 */
	cm->online = true;	/* [한국어] 이제 배정 후보가 된다 */
	m->online_maps++;	/* [한국어] 온라인 맵의 수 */
	trace_irq_matrix_online(m);	/* [한국어] 추적점. 회계가 어긋났을 때 언제 무엇이 바뀌었는지 되짚는 유일한 수단이다 */
}

/**
 * irq_matrix_offline - Bring the local CPU matrix offline
 * @m:		Matrix pointer
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_offline - 이 CPU 의 맵을 회계에서 뺀다
 *
 * @m: 행렬.
 *
 * 위 online 의 짝이지만 대칭이 아니다. available 을 전역 합계에서 빼고
 * 온라인 표시를 지울 뿐, cm->available 자체는 건드리지 않는다.
 *
 * 왜 그런가: 그 CPU 에 배정된 벡터들은 여전히 남아 있다. 나중에 다시
 * 온라인이 되면 그 상태를 그대로 이어받아야 하므로, cm 의 내용은 보존한다.
 * 위 initialized 검사가 그 보존을 완성한다.
 *
 * BUG_ON 이 없는 것도 online 과 다르다. 이미 오프라인인데 또 부르면
 * global_available 이 두 번 줄어 회계가 어긋나지만, 그 검사가 없다.
 * 호출자(x86 벡터 관리)가 짝을 맞춘다고 신뢰하는 것이다.
 *
 * 실행 컨텍스트: CPU 오프라인 경로, 그 CPU 위에서. 인터럽트가 꺼진 상태.
 *
 * 호출 체인:
 *   lapic_offline() (arch/x86/kernel/apic/vector.c) → [이 함수]
 */
void irq_matrix_offline(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵 */

	/* Update the global available size */
	/* [한국어] (위 영어 주석) 전역 합계만 조정한다.
	 * cm->available 은 그대로 둔다 — 다시 온라인이 될 때 이어받아야 한다. */
	m->global_available -= cm->available;	/* [한국어] 이 CPU 몫을 전역에서 뺀다 */
	cm->online = false;	/* [한국어] 배정 후보에서 빠진다. 두 find_best_cpu 가 이것을 보고 건너뛴다 */
	m->online_maps--;	/* [한국어] 온라인 맵의 수 */
	trace_irq_matrix_offline(m);	/* [한국어] 추적점 */
}

/*
 * [한국어]
 * matrix_alloc_area - 연속된 빈 자리를 찾아 비트를 세운다
 *
 * @m:       행렬.
 * @cm:      대상 CPU 의 맵.
 * @num:     필요한 연속 비트의 수. 실제로는 언제나 1 이다.
 * @managed: 참이면 managed_map 에, 거짓이면 alloc_map 에 표시한다.
 * @return:  찾은 시작 비트, 또는 자리가 없으면 alloc_end 이상.
 *
 * 이 파일의 실제 할당 알고리즘이다. 세 비트맵을 합쳐 "쓸 수 없는 자리"
 * 전체를 만든 뒤, 거기서 빈 곳을 찾는다.
 *
 * 합치는 셋:
 *   managed_map — managed 로 잡아 둔 자리.
 *   system_map  — 시스템이 못 박은 자리.
 *   alloc_map   — 실제로 배정된 자리.
 *
 * scratch_map 을 두 번에 걸쳐 채우는 이유: bitmap_or 이 두 입력만 받으므로
 * 세 개를 합치려면 두 번 불러야 한다. 두 번째는 결과를 자기 자신에게
 * 다시 OR 하는 형태다.
 *
 * 반환값의 규약에 주의: 실패를 음수가 아니라 "범위를 벗어난 값" 으로
 * 나타낸다. bitmap_find_next_zero_area 가 그렇게 동작하기 때문이며,
 * 호출자는 alloc_end 와 비교해 판정한다.
 *
 * 동기화: scratch_map 을 락 없이 쓴다. 호출자가 벡터 락으로 직렬화한다는
 * 전제이며, 그 전제가 깨지면 두 CPU 의 계산이 뒤섞인다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼지고 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_matrix_reserve_managed()/irq_matrix_alloc() → [이 함수]
 */
static unsigned int matrix_alloc_area(struct irq_matrix *m, struct cpumap *cm,
				      unsigned int num, bool managed)
{
	unsigned int area, start = m->alloc_start;	/* [한국어] 찾은 자리와 탐색 시작 위치 */
	unsigned int end = m->alloc_end;	/* [한국어] 탐색 상한 */

	bitmap_or(m->scratch_map, cm->managed_map, m->system_map, end);	/* [한국어] 먼저 두 개를 합친다 — managed 로 잡아 둔 자리와 시스템 벡터 */
	bitmap_or(m->scratch_map, m->scratch_map, cm->alloc_map, end);	/* [한국어] 거기에 실제 배정된 것까지 더한다. 결과가 "쓸 수 없는 자리" 전체다 */
	area = bitmap_find_next_zero_area(m->scratch_map, end, start, num, 0);	/* [한국어] 그 밖의 연속된 빈 자리를 찾는다. 마지막 0 은 정렬 요구 없음 */
	if (area >= end)	/* [한국어] 실패를 음수가 아니라 범위 밖 값으로 나타낸다 */
		return area;	/* [한국어] 호출자가 alloc_end 와 비교해 판정한다 */
	if (managed)	/* [한국어] 어느 비트맵에 표시할 것인가 */
		bitmap_set(cm->managed_map, area, num);	/* [한국어] 자리를 잡아 두기만 한다 — 실제 배정은 나중에 alloc_managed 가 한다 */
	else
		bitmap_set(cm->alloc_map, area, num);	/* [한국어] 실제로 배정한다 */
	return area;	/* [한국어] 찾은 비트 번호 */
}

/* Find the best CPU which has the lowest vector allocation count */
/*
 * [한국어] (위 영어 주석에 이어)
 * matrix_find_best_cpu - 가장 여유 있는 CPU 를 고른다
 *
 * @m:      행렬.
 * @msk:    후보 CPU 들.
 * @return: 고른 CPU, 또는 후보가 없으면 UINT_MAX.
 *
 * 보통의 인터럽트를 배정할 때 쓴다. available 이 가장 큰 CPU 를 고르는데,
 * 그것이 곧 "벡터를 가장 적게 쓰고 있는" CPU 다.
 *
 * 왜 그 기준인가: 벡터를 고르게 나누면 특정 CPU 만 먼저 고갈되는 일이
 * 줄어든다. 그리고 인터럽트가 여러 CPU 에 퍼지면 처리 부하도 분산된다.
 *
 * maxavl 이 0 에서 시작하고 비교가 <= 인 것에 주의: available 이 0 인
 * CPU 는 후보가 되지 않는다. 그런 CPU 에 배정하면 곧바로 실패하기 때문이다.
 *
 * 실패를 UINT_MAX 로 나타내는 것도 이 파일의 관용구다. 음수를 쓸 수 없는
 * unsigned 반환형이라 그렇게 한다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_matrix_alloc() → [이 함수]
 */
static unsigned int matrix_find_best_cpu(struct irq_matrix *m,
					const struct cpumask *msk)
{
	unsigned int cpu, best_cpu, maxavl = 0;	/* [한국어] 순회 변수, 지금까지 최선, 그 CPU 의 여유. 0 에서 시작해 여유 없는 CPU 를 자연히 걸러 낸다 */
	struct cpumap *cm;	/* [한국어] 검사 중인 CPU 의 맵 */

	best_cpu = UINT_MAX;	/* [한국어] 실패값으로 시작한다. 후보를 하나도 못 찾으면 이대로 반환된다 */

	for_each_cpu(cpu, msk) {	/* [한국어] 후보 CPU 를 하나씩 */
		cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */

		if (!cm->online || cm->available <= maxavl)	/* [한국어] 오프라인이거나 지금까지 최선보다 여유가 적으면 건너뛴다. maxavl 이 0 에서 시작하므로 available 이 0 인 CPU 도 여기서 걸러진다 */
			continue;

		best_cpu = cpu;	/* [한국어] 새 최선 */
		maxavl = cm->available;	/* [한국어] 그 여유를 기준으로 갱신 */
	}
	return best_cpu;	/* [한국어] UINT_MAX 면 배정할 수 있는 CPU 가 없다 */
}

/* Find the best CPU which has the lowest number of managed IRQs allocated */
/*
 * [한국어] (위 영어 주석에 이어)
 * matrix_find_best_cpu_managed - managed 인터럽트가 가장 적은 CPU 를 고른다
 *
 * @m:      행렬.
 * @msk:    후보 CPU 들.
 * @return: 고른 CPU, 또는 후보가 없으면 UINT_MAX.
 *
 * 위 find_best_cpu 와 기준이 다르다. available 이 아니라 managed_allocated
 * 를 보고, 큰 것이 아니라 작은 것을 고른다.
 *
 * 왜 다른 기준인가: managed 인터럽트는 이미 자리를 잡아 두었으므로
 * available 을 보아도 소용이 없다 — 그 값에서 이미 빠져 있기 때문이다.
 * 대신 실제로 몇 개를 쓰고 있는지를 보고 가장 한가한 CPU 를 고른다.
 *
 * allocated 가 UINT_MAX 에서 시작하고 비교가 > 인 것에 주의: 위 함수와
 * 방향이 반대이며, 첫 온라인 CPU 가 반드시 후보가 되게 한다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_matrix_alloc_managed() → [이 함수]
 */
static unsigned int matrix_find_best_cpu_managed(struct irq_matrix *m,
						const struct cpumask *msk)
{
	unsigned int cpu, best_cpu, allocated = UINT_MAX;	/* [한국어] 위 함수와 반대로 최대값에서 시작한다 — 작은 것을 찾기 때문이다 */
	struct cpumap *cm;	/* [한국어] 검사 중인 CPU 의 맵 */

	best_cpu = UINT_MAX;	/* [한국어] 실패값 */

	for_each_cpu(cpu, msk) {	/* [한국어] 후보 CPU 를 하나씩 */
		cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */

		if (!cm->online || cm->managed_allocated > allocated)	/* [한국어] 오프라인이거나 지금까지 최선보다 많이 쓰고 있으면 건너뛴다 */
			continue;

		best_cpu = cpu;	/* [한국어] 새 최선 */
		allocated = cm->managed_allocated;	/* [한국어] 그 사용량을 기준으로 갱신 */
	}
	return best_cpu;	/* [한국어] UINT_MAX 면 온라인 후보가 없다 */
}

/**
 * irq_matrix_assign_system - Assign system wide entry in the matrix
 * @m:		Matrix pointer
 * @bit:	Which bit to reserve
 * @replace:	Replace an already allocated vector with a system
 *		vector at the same bit position.
 *
 * The BUG_ON()s below are on purpose. If this goes wrong in the
 * early boot process, then the chance to survive is about zero.
 * If this happens when the system is life, it's not much better.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_assign_system - 시스템 벡터를 못 박는다
 *
 * @m:       행렬.
 * @bit:     못 박을 벡터 번호.
 * @replace: 이미 배정된 벡터를 시스템 벡터로 바꿀 것인가.
 *
 * 시스템 벡터란: CPU 예외, IPI, APIC 타이머처럼 커널이 고정된 번호로
 * 쓰는 벡터다. 모든 CPU 에서 같은 번호를 쓰므로 CPU 별 맵이 아니라
 * 전역 system_map 에 표시한다.
 *
 * BUG_ON 세 개가 이 함수의 성격을 말해 준다. 원 주석이 그 의도를 밝힌다 —
 * 부팅 초기에 이것이 잘못되면 살아남을 가능성이 사실상 0 이고, 시스템이
 * 살아 있는 중이라도 크게 낫지 않다. 그래서 오류를 돌려주는 대신 곧바로 멈춘다.
 *
 * 두 번째 BUG_ON 이 특히 미묘하다. "온라인 맵이 하나를 넘거나, 하나
 * 있는데 replace 가 아니면" 멈춘다. 뜻을 풀면:
 *   CPU 가 여럿 온라인인 상태에서는 시스템 벡터를 못 박을 수 없다 —
 *     다른 CPU 의 회계를 이 함수가 맞출 수 없기 때문이다.
 *   CPU 하나가 온라인이면, 이미 배정된 것을 교체하는 경우에만 허용한다.
 *   CPU 가 하나도 온라인이 아니면(부팅 아주 초기) 자유롭게 허용한다.
 *
 * 실행 컨텍스트: 부팅 초기 또는 CPU 하나만 온라인인 상태.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 시스템 벡터 초기화 → [이 함수]
 */
void irq_matrix_assign_system(struct irq_matrix *m, unsigned int bit,
			      bool replace)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵. replace 일 때만 쓴다 */

	BUG_ON(bit > m->matrix_bits);	/* [한국어] 번호 공간을 벗어났다. 비트맵 밖을 쓰게 되므로 곧바로 멈춘다 */
	BUG_ON(m->online_maps > 1 || (m->online_maps && !replace));	/* [한국어] 온라인 CPU 가 여럿이면 다른 CPU 의 회계를 맞출 수 없고, 하나뿐이어도 교체가 아니면 허용하지 않는다. 자세한 뜻은 위 블록 주석 참고 */

	set_bit(bit, m->system_map);	/* [한국어] 전역 시스템 맵에 표시한다. 모든 CPU 가 이 번호를 쓸 수 없게 된다 */
	if (replace) {	/* [한국어] 이미 배정된 벡터를 시스템 벡터로 바꾸는가 */
		BUG_ON(!test_and_clear_bit(bit, cm->alloc_map));	/* [한국어] 실제로 배정되어 있어야 한다. 아니면 호출자의 가정이 틀린 것이다 */
		cm->allocated--;	/* [한국어] 그 배정을 회계에서 뺀다 */
		m->total_allocated--;	/* [한국어] 전역 합계도 */
	}
	if (bit >= m->alloc_start && bit < m->alloc_end)	/* [한국어] 이 벡터가 동적 배정 범위 안에 있는가 */
		m->systembits_inalloc++;	/* [한국어] 그렇다면 쓸 수 있는 자리가 하나 줄었다. online 이 available 초기값을 계산할 때 이 수를 뺀다 */

	trace_irq_matrix_assign_system(bit, m);	/* [한국어] 추적점 */
}

/**
 * irq_matrix_reserve_managed - Reserve a managed interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	On which CPUs the bits should be reserved.
 *
 * Can be called for offline CPUs. Note, this will only reserve one bit
 * on all CPUs in @msk, but it's not guaranteed that the bits are at the
 * same offset on all CPUs
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_reserve_managed - managed 인터럽트를 위한 자리를 모든 대상 CPU 에 잡아 둔다
 *
 * @m:      행렬.
 * @msk:    자리를 잡아 둘 CPU 들.
 * @return: 0 이면 성공, -ENOSPC 면 어느 CPU 에서 자리를 찾지 못했다.
 *
 * managed 인터럽트의 준비 단계다. 실제 배정은 나중에 alloc_managed 가
 * 그 CPU 중 하나에서 하지만, 자리는 모든 대상 CPU 에 미리 잡아 둔다.
 *
 * 왜 전부에 잡아 두는가: managed 인터럽트는 CPU 핫플러그에 따라 옮겨
 * 다닌다. 지금 CPU 3 에 배정했더라도 그것이 내려가면 CPU 5 로 옮겨야
 * 하는데, 그때 CPU 5 에 자리가 없으면 그 큐를 쓸 수 없게 된다. 미리
 * 잡아 두면 옮기기가 반드시 성공한다.
 *
 * 오프라인 CPU 에도 잡아 두는 것이 원 주석의 요점이다. 나중에 켜질
 * 수 있고, 그때를 대비해야 한다.
 *
 * 비트 위치가 CPU 마다 다를 수 있다는 것도 원 주석에 있다. 각 CPU 의
 * 비트맵 상태가 다르므로 빈 자리도 다른 곳에 있다.
 *
 * 실패하면 그때까지 잡은 것을 되돌린다. 부분적으로 잡힌 상태를 남기면
 * 회계가 어긋난다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   x86 벡터 관리의 managed 인터럽트 준비 → [이 함수] → matrix_alloc_area()
 */
int irq_matrix_reserve_managed(struct irq_matrix *m, const struct cpumask *msk)
{
	unsigned int cpu, failed_cpu;	/* [한국어] 순회 변수와, 실패한 지점을 기억할 자리 */

	for_each_cpu(cpu, msk) {	/* [한국어] 대상 CPU 를 하나씩. 오프라인도 포함한다 */
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */
		unsigned int bit;	/* [한국어] 잡은 자리 */

		bit = matrix_alloc_area(m, cm, 1, true);	/* [한국어] managed=true 라 managed_map 에 표시한다. 아직 실제 배정은 아니다 */
		if (bit >= m->alloc_end)	/* [한국어] 자리를 찾지 못했다 */
			goto cleanup;	/* [한국어] 그때까지 잡은 것을 되돌려야 한다 */
		cm->managed++;	/* [한국어] 잡아 둔 자리의 수 */
		if (cm->online) {	/* [한국어] 온라인 CPU 에서만 available 회계를 조정한다 */
			cm->available--;	/* [한국어] 쓸 수 있는 자리가 하나 줄었다 */
			m->global_available--;	/* [한국어] 전역 합계도. 오프라인 CPU 는 전역에 들어 있지 않아 건드리지 않는다 */
		}
		trace_irq_matrix_reserve_managed(bit, cpu, m, cm);	/* [한국어] 추적점 */
	}
	return 0;	/* [한국어] 모든 대상 CPU 에 자리를 잡았다 */
cleanup:	/* [한국어] 실패 시 되돌리기 */
	failed_cpu = cpu;	/* [한국어] 실패한 CPU 를 기억한다. 그 앞까지만 되돌려야 한다 */
	for_each_cpu(cpu, msk) {	/* [한국어] 처음부터 다시 훑으며 */
		if (cpu == failed_cpu)	/* [한국어] 실패 지점에 도달하면 */
			break;	/* [한국어] 그 CPU 부터는 잡은 적이 없으므로 멈춘다 */
		irq_matrix_remove_managed(m, cpumask_of(cpu));	/* [한국어] 한 CPU 씩 되돌린다. 마스크 하나만 든 임시 마스크를 넘긴다 */
	}
	return -ENOSPC;	/* [한국어] 벡터가 부족하다. 호출자는 이 managed 인터럽트를 만들 수 없다 */
}

/**
 * irq_matrix_remove_managed - Remove managed interrupts in a CPU map
 * @m:		Matrix pointer
 * @msk:	On which CPUs the bits should be removed
 *
 * Can be called for offline CPUs
 *
 * This removes not allocated managed interrupts from the map. It does
 * not matter which one because the managed interrupts free their
 * allocation when they shut down. If not, the accounting is screwed,
 * but all what can be done at this point is warn about it.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_remove_managed - 잡아 두었던 managed 자리를 반납한다
 *
 * @m:   행렬.
 * @msk: 반납할 CPU 들.
 *
 * 위 reserve_managed 의 짝이다. 원 주석의 설명이 이 함수의 미묘한 점을
 * 짚는다 — "어느 비트를 지우든 상관없다".
 *
 * 왜 상관없는가: managed 인터럽트는 종료할 때 자기 배정을 스스로 반납한다.
 * 그래서 이 시점에 남아 있는 managed 자리는 전부 "잡아 두었지만 쓰지 않는"
 * 것들이고, 그중 어느 것을 지우든 결과가 같다.
 *
 * 그 전제가 깨지면(managed 인터럽트가 반납하지 않고 사라지면) 회계가
 * 망가지는데, 원 주석대로 이 시점에 할 수 있는 일은 경고뿐이다.
 *
 * 그래서 "잡아 두었지만 배정되지 않은" 비트를 찾는다 — managed_map 에서
 * alloc_map 을 뺀 차집합이다. 그것이 비어 있으면 전제가 깨진 것이라 경고한다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_matrix_reserve_managed() 의 실패 경로 → [이 함수]
 *   x86 벡터 관리의 managed 인터럽트 해제 → [이 함수]
 */
void irq_matrix_remove_managed(struct irq_matrix *m, const struct cpumask *msk)
{
	unsigned int cpu;	/* [한국어] 순회 변수 */

	for_each_cpu(cpu, msk) {	/* [한국어] 대상 CPU 를 하나씩 */
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */
		unsigned int bit, end = m->alloc_end;	/* [한국어] 지울 비트와 탐색 상한 */

		if (WARN_ON_ONCE(!cm->managed))	/* [한국어] 잡아 둔 자리가 없는데 반납하려 한다. 호출자의 회계가 어긋난 것이다 */
			continue;	/* [한국어] 이 CPU 는 건너뛴다 */

		/* Get managed bit which are not allocated */
		/* [한국어] (위 영어 주석) 잡아 두었지만 아직 배정되지 않은 비트를 찾는다.
		 * managed_map 에서 alloc_map 을 뺀 차집합이다. */
		bitmap_andnot(m->scratch_map, cm->managed_map, cm->alloc_map, end);	/* [한국어] 임시 버퍼에 차집합을 만든다 */

		bit = find_first_bit(m->scratch_map, end);	/* [한국어] 그중 아무거나 하나. 원 주석대로 어느 것이든 상관없다 */
		if (WARN_ON_ONCE(bit >= end))	/* [한국어] 차집합이 비었다 — 잡아 둔 자리가 전부 배정되어 있다는 뜻이라, managed 인터럽트가 반납하지 않고 사라진 것이다 */
			continue;	/* [한국어] 회계가 이미 망가졌지만 여기서 할 수 있는 일이 없다 */

		clear_bit(bit, cm->managed_map);	/* [한국어] 잡아 둔 자리를 푼다 */

		cm->managed--;	/* [한국어] 잡아 둔 수를 줄인다 */
		if (cm->online) {	/* [한국어] 온라인 CPU 에서만 available 회계를 조정한다 */
			cm->available++;	/* [한국어] 쓸 수 있는 자리가 하나 늘었다 */
			m->global_available++;	/* [한국어] 전역 합계도 */
		}
		trace_irq_matrix_remove_managed(bit, cpu, m, cm);	/* [한국어] 추적점 */
	}
}

/**
 * irq_matrix_alloc_managed - Allocate a managed interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	Which CPUs to search in
 * @mapped_cpu:	Pointer to store the CPU for which the irq was allocated
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_alloc_managed - 잡아 둔 managed 자리에서 실제로 배정한다
 *
 * @m:          행렬.
 * @msk:        후보 CPU 들. reserve 때의 집합과 같거나 그 부분집합이다.
 * @mapped_cpu: 어느 CPU 에 배정했는지 돌려줄 곳.
 * @return:     배정한 벡터 번호(0 이상), 또는 음수 오류.
 *
 * 위 reserve_managed 가 잡아 둔 자리 중 하나를 실제로 쓴다. 새로 자리를
 * 찾지 않고 이미 확보된 것에서 고르는 것이 아래 alloc() 과의 결정적 차이다.
 *
 * 그래서 available 을 건드리지 않는다 — reserve 때 이미 뺐기 때문이다.
 * managed_allocated 와 allocated 만 올린다. 이 비대칭이 이 파일의 회계에서
 * 가장 헷갈리는 부분이다.
 *
 * CPU 선택도 다르다. 가장 여유 있는 CPU 가 아니라 managed 를 가장 적게
 * 쓰고 있는 CPU 를 고른다 — 그 이유는 matrix_find_best_cpu_managed() 의
 * 주석에 있다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태. 인터럽트 활성화 경로에서 불린다.
 *
 * 호출 체인:
 *   irq_activate() → x86 의 assign_managed_vector() → [이 함수]
 */
int irq_matrix_alloc_managed(struct irq_matrix *m, const struct cpumask *msk,
			     unsigned int *mapped_cpu)
{
	unsigned int bit, cpu, end;	/* [한국어] 배정할 비트, 고른 CPU, 탐색 상한 */
	struct cpumap *cm;	/* [한국어] 그 CPU 의 맵 */

	if (cpumask_empty(msk))	/* [한국어] 후보가 없는가 */
		return -EINVAL;	/* [한국어] 호출자의 실수다. 아래 find_best 가 UINT_MAX 를 주는 것과 구분해 -EINVAL 로 답한다 */

	cpu = matrix_find_best_cpu_managed(m, msk);	/* [한국어] managed 를 가장 적게 쓰는 CPU. available 이 아니라 이 기준인 이유는 그 함수 주석 참고 */
	if (cpu == UINT_MAX)	/* [한국어] 온라인 후보가 하나도 없다 */
		return -ENOSPC;

	cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 고른 CPU 의 맵 */
	end = m->alloc_end;	/* [한국어] 탐색 상한 */
	/* Get managed bit which are not allocated */
	/* [한국어] (위 영어 주석) 잡아 두었지만 아직 쓰지 않는 자리를 찾는다.
	 * 위 remove_managed 와 같은 차집합 계산이다 — 그쪽은 반납할 것을,
	 * 이쪽은 쓸 것을 찾는다. */
	bitmap_andnot(m->scratch_map, cm->managed_map, cm->alloc_map, end);	/* [한국어] managed_map 에서 alloc_map 을 뺀다 */
	bit = find_first_bit(m->scratch_map, end);	/* [한국어] 그중 첫 번째 */
	if (bit >= end)	/* [한국어] 잡아 둔 자리를 전부 쓰고 있다 */
		return -ENOSPC;	/* [한국어] reserve 때 확보한 것보다 많이 배정하려는 것이다 */
	set_bit(bit, cm->alloc_map);	/* [한국어] 실제로 배정한다. managed_map 은 그대로 둔다 — 자리는 여전히 잡혀 있다 */
	cm->allocated++;	/* [한국어] 총 배정 수 */
	cm->managed_allocated++;	/* [한국어] 그중 managed 몫 */
	m->total_allocated++;	/* [한국어] 전역 합계 */
	/* [한국어] available 을 건드리지 않는 것에 주의 — reserve 때 이미 뺐다.
	 * 아래 alloc() 이 available 을 줄이는 것과 대조되며, 이것이 managed
	 * 회계의 핵심이다. */
	*mapped_cpu = cpu;	/* [한국어] 호출자에게 어느 CPU 인지 알린다 */
	trace_irq_matrix_alloc_managed(bit, cpu, m, cm);	/* [한국어] 추적점 */
	return bit;	/* [한국어] 배정한 벡터 번호. 0 일 수 있으므로 호출자는 음수만 실패로 봐야 한다 */
}

/**
 * irq_matrix_assign - Assign a preallocated interrupt in the local CPU map
 * @m:		Matrix pointer
 * @bit:	Which bit to mark
 *
 * This should only be used to mark preallocated vectors
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_assign - 이미 정해진 벡터 번호를 이 CPU 의 맵에 표시한다
 *
 * @m:   행렬.
 * @bit: 표시할 벡터 번호.
 *
 * 위 alloc 계열과 달리 번호를 고르지 않는다. 이미 정해진 번호를 회계에
 * 반영할 뿐이다.
 *
 * 언제 쓰는가: kernel-doc 이 말하는 "미리 배정된 벡터" 다. 부팅 초기에
 * 커널이 특정 번호를 쓰기로 정해 둔 인터럽트들이 있고, 그것들을 나중에
 * 행렬의 회계에 넣어야 한다.
 *
 * 두 WARN 이 잘못된 사용을 잡는다:
 *   범위 밖 번호 — 동적 배정 범위 안이어야 한다. 시스템 벡터라면 위
 *     assign_system 을 써야 한다.
 *   이미 배정된 번호 — test_and_set_bit 이 참을 주면 중복이다.
 *
 * 이 함수는 available 을 줄인다. 위 alloc_managed 가 줄이지 않는 것과
 * 대조되며, 그 이유는 이것이 미리 잡아 둔 자리가 아니기 때문이다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태, 그 CPU 위에서.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 미리 배정된 벡터 등록 → [이 함수]
 */
void irq_matrix_assign(struct irq_matrix *m, unsigned int bit)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵 */

	if (WARN_ON_ONCE(bit < m->alloc_start || bit >= m->alloc_end))	/* [한국어] 동적 배정 범위 안이어야 한다. 시스템 벡터라면 assign_system 을 써야 한다 */
		return;
	if (WARN_ON_ONCE(test_and_set_bit(bit, cm->alloc_map)))	/* [한국어] 확인과 표시를 한 원자 연산으로. 참이면 이미 배정된 번호라 중복이다 */
		return;
	cm->allocated++;	/* [한국어] 이 CPU 의 배정 수 */
	m->total_allocated++;	/* [한국어] 전역 합계 */
	cm->available--;	/* [한국어] 쓸 수 있는 자리가 줄었다. alloc_managed 가 이것을 건드리지 않는 것과 대조된다 */
	m->global_available--;	/* [한국어] 전역 여유도 */
	trace_irq_matrix_assign(bit, smp_processor_id(), m, cm);	/* [한국어] 추적점. this_cpu_ptr 을 썼으므로 현재 CPU 번호를 함께 남긴다 */
}

/**
 * irq_matrix_reserve - Reserve interrupts
 * @m:		Matrix pointer
 *
 * This is merely a book keeping call. It increments the number of globally
 * reserved interrupt bits w/o actually allocating them. This allows to
 * setup interrupt descriptors w/o assigning low level resources to it.
 * The actual allocation happens when the interrupt gets activated.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_reserve - 벡터를 실제로 잡지 않고 예약만 한다
 *
 * @m: 행렬.
 *
 * 원 주석이 말하듯 순수한 회계 호출이다. 비트를 세우지도, 어느 CPU 인지
 * 정하지도 않는다. 전역 카운터 하나만 올린다.
 *
 * 무엇을 위한 것인가: 인터럽트 서술자를 만들되 실제 벡터는 활성화 시점에
 * 배정하는 방식이 있다. MSI-X 를 수천 개 지원하는 장치가 그 벡터를 전부
 * 미리 잡으면 시스템의 벡터가 곧 고갈되지만, 실제로 쓰는 것은 몇 개뿐이다.
 *
 * 그렇다고 아예 세지 않으면 위험하다. 그 인터럽트들이 한꺼번에 활성화되면
 * 벡터가 모자랄 수 있는데, 그때는 이미 늦다. 그래서 "언젠가 필요할 수도
 * 있는" 수를 세어 두고, 그것이 여유를 넘어서면 경고한다.
 *
 * 경고만 하고 막지 않는 것에 주의: 예약이 여유를 넘어도 실패로 처리하지
 * 않는다. 실제로 전부 활성화되지 않을 가능성이 높고, 여기서 막으면
 * 정상적인 장치 초기화가 실패하기 때문이다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_domain 의 예약 경로 → x86 의 x86_vector_alloc_irqs() → [이 함수]
 */
void irq_matrix_reserve(struct irq_matrix *m)
{
	if (m->global_reserved == m->global_available)	/* [한국어] 예약이 여유와 같아졌는가. 한 번 더 예약하면 넘어선다 */
		pr_warn("Interrupt reservation exceeds available resources\n");	/* [한국어] 경고만 하고 막지 않는다 — 예약이 전부 실현되지는 않으므로, 여기서 실패시키면 정상적인 초기화가 깨진다 */

	m->global_reserved++;	/* [한국어] 비트는 세우지 않는다. 순수한 회계다 */
	trace_irq_matrix_reserve(m);	/* [한국어] 추적점 */
}

/**
 * irq_matrix_remove_reserved - Remove interrupt reservation
 * @m:		Matrix pointer
 *
 * This is merely a book keeping call. It decrements the number of globally
 * reserved interrupt bits. This is used to undo irq_matrix_reserve() when the
 * interrupt was never in use and a real vector allocated, which undid the
 * reservation.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_remove_reserved - 예약을 취소한다
 *
 * @m: 행렬.
 *
 * 위 reserve 의 짝이다. 원 주석이 언제 쓰는지를 정확히 말한다 — 그
 * 인터럽트가 한 번도 쓰이지 않은 채 사라질 때다.
 *
 * 왜 그 조건이 붙는가: 실제로 벡터를 배정받으면(irq_matrix_alloc 에
 * reserved=true 로) 그쪽이 이미 예약을 소비한다. 그 뒤에 또 취소하면
 * 카운터가 두 번 줄어 회계가 어긋난다.
 *
 * 그래서 이 함수는 "예약했지만 끝내 쓰지 않았다" 는 경우에만 불려야 한다.
 * 검사가 없으므로 호출자가 그 규칙을 지켜야 한다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   x86 벡터 관리의 인터럽트 해제 경로 → [이 함수]
 */
void irq_matrix_remove_reserved(struct irq_matrix *m)
{
	m->global_reserved--;	/* [한국어] 검사가 없다 — 예약하지 않은 것을 취소하면 카운터가 음수로 감싸며 회계가 망가진다. 호출자가 규칙을 지켜야 한다 */
	trace_irq_matrix_remove_reserved(m);	/* [한국어] 추적점 */
}

/**
 * irq_matrix_alloc - Allocate a regular interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	Which CPUs to search in
 * @reserved:	Allocate previously reserved interrupts
 * @mapped_cpu: Pointer to store the CPU for which the irq was allocated
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_alloc - 보통의 인터럽트에 벡터를 배정한다
 *
 * @m:          행렬.
 * @msk:        후보 CPU 들.
 * @reserved:   위 reserve 로 미리 예약해 둔 것을 실현하는 경우인가.
 * @mapped_cpu: 어느 CPU 에 배정했는지 돌려줄 곳.
 * @return:     배정한 벡터 번호(0 이상), 또는 음수 오류.
 *
 * 이 파일에서 가장 자주 불리는 함수다. 위 alloc_managed 와 대비하면
 * 이 파일의 두 회계 방식이 드러난다:
 *
 *   alloc_managed — 미리 잡아 둔 자리를 쓴다. available 을 건드리지 않는다.
 *   alloc         — 새로 자리를 찾는다. available 을 줄인다.
 *
 * CPU 선택도 다르다. 가장 여유 있는 CPU 를 골라 벡터를 고르게 분산한다.
 *
 * reserved 인자가 예약 회계와 이어진다. 참이면 위 reserve 가 올려 둔
 * 카운터를 여기서 소비한다 — 예약이 실현되었으므로 더는 "예약 중" 이
 * 아니기 때문이다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_activate() → x86 의 assign_vector_locked() → [이 함수]
 */
int irq_matrix_alloc(struct irq_matrix *m, const struct cpumask *msk,
		     bool reserved, unsigned int *mapped_cpu)
{
	unsigned int cpu, bit;	/* [한국어] 고른 CPU 와 배정할 비트 */
	struct cpumap *cm;	/* [한국어] 그 CPU 의 맵 */

	/*
	 * Not required in theory, but matrix_find_best_cpu() uses
	 * for_each_cpu() which ignores the cpumask on UP .
	 */
	/* [한국어] (위 영어 주석에 이어) 단일 프로세서 빌드를 위한 방어.
	 *
	 * 무슨 문제인가: UP 커널에서 for_each_cpu 는 마스크를 무시하고 CPU 0
	 * 한 번을 돈다. 그래서 빈 마스크를 넘겨도 CPU 0 이 후보가 되어,
	 * 호출자가 의도하지 않은 배정이 일어난다.
	 *
	 * 이론적으로는 필요 없다는 원 주석의 뜻: SMP 에서는 빈 마스크면
	 * 루프가 돌지 않아 UINT_MAX 가 나오고 자연히 -ENOSPC 가 된다. */
	if (cpumask_empty(msk))	/* [한국어] 후보가 없는가 */
		return -EINVAL;	/* [한국어] UP 커널에서 for_each_cpu 가 빈 마스크를 무시하는 것에 대한 방어다 */

	cpu = matrix_find_best_cpu(m, msk);	/* [한국어] 가장 여유 있는 CPU. managed 판이 다른 기준을 쓰는 것과 대조된다 */
	if (cpu == UINT_MAX)	/* [한국어] 온라인이면서 여유가 있는 CPU 가 없다 */
		return -ENOSPC;

	cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 고른 CPU 의 맵 */
	bit = matrix_alloc_area(m, cm, 1, false);	/* [한국어] managed=false 라 alloc_map 에 직접 표시한다. 새로 자리를 찾는 것이 managed 판과의 차이다 */
	if (bit >= m->alloc_end)	/* [한국어] 자리를 못 찾았다. available 이 0 이 아니었는데도 실패할 수 있다 — 연속된 자리가 필요한 경우 등 */
		return -ENOSPC;
	cm->allocated++;	/* [한국어] 이 CPU 의 배정 수 */
	cm->available--;	/* [한국어] 여유를 줄인다. alloc_managed 가 건드리지 않는 것과 대조되는 지점이다 */
	m->total_allocated++;	/* [한국어] 전역 합계 */
	m->global_available--;	/* [한국어] 전역 여유 */
	if (reserved)	/* [한국어] 미리 예약해 둔 것을 실현하는 경우인가 */
		m->global_reserved--;	/* [한국어] 예약이 실현되었으므로 그 카운터를 소비한다. 이것이 remove_reserved 를 따로 부르면 안 되는 이유다 */
	*mapped_cpu = cpu;	/* [한국어] 호출자에게 어느 CPU 인지 알린다 */
	trace_irq_matrix_alloc(bit, cpu, m, cm);	/* [한국어] 추적점 */
	return bit;	/* [한국어] 배정한 벡터 번호. 0 일 수 있으므로 음수만 실패로 봐야 한다 */

}

/**
 * irq_matrix_free - Free allocated interrupt in the matrix
 * @m:		Matrix pointer
 * @cpu:	Which CPU map needs be updated
 * @bit:	The bit to remove
 * @managed:	If true, the interrupt is managed and not accounted
 *		as available.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_free - 배정된 벡터를 반납한다
 *
 * @m:       행렬.
 * @cpu:     그 벡터가 배정된 CPU. this_cpu 가 아니라 인자로 받는 것에 주의 —
 *           다른 CPU 의 벡터를 반납할 수 있어야 하기 때문이다.
 * @bit:     반납할 벡터 번호.
 * @managed: managed 인터럽트의 것인가. 회계 방식이 갈린다.
 *
 * 위 두 alloc 계열의 짝이며, managed 인자로 어느 쪽인지 구분한다.
 *
 * 세 갈래의 조건이 이 함수를 복잡하게 만든다:
 *
 *   managed 면 managed_allocated 도 줄인다 — 그 카운터는 managed 배정만 센다.
 *   online 인 CPU 에서만 total_allocated 를 줄인다 — 오프라인 CPU 의
 *     벡터는 애초에 그 합계에 들어 있지 않다.
 *   managed 가 아닐 때만 available 을 늘린다 — managed 자리는 반납해도
 *     여전히 잡혀 있으므로 여유가 늘지 않는다.
 *
 * 마지막이 이 파일의 회계에서 가장 미묘한 부분이다. managed 인터럽트를
 * 반납하면 alloc_map 의 비트는 지워지지만 managed_map 의 비트는 남는다.
 * 자리는 여전히 그 인터럽트의 것이고, 나중에 다시 활성화될 수 있다.
 *
 * 실행 컨텍스트: 벡터 락을 쥔 상태. 임의의 CPU 에서 불릴 수 있다.
 *
 * 호출 체인:
 *   irq_shutdown_and_deactivate() → x86 의 벡터 해제 → [이 함수]
 */
void irq_matrix_free(struct irq_matrix *m, unsigned int cpu,
		     unsigned int bit, bool managed)
{
	struct cpumap *cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] this_cpu_ptr 이 아니다 — 다른 CPU 의 벡터를 반납할 수 있어야 한다 */

	if (WARN_ON_ONCE(bit < m->alloc_start || bit >= m->alloc_end))	/* [한국어] 동적 배정 범위 밖의 번호를 반납하려 한다 */
		return;

	if (WARN_ON_ONCE(!test_and_clear_bit(bit, cm->alloc_map)))	/* [한국어] 배정되지 않은 번호를 반납하려 한다. 이중 해제이거나 호출자의 회계가 어긋난 것이다 */
		return;

	cm->allocated--;	/* [한국어] 이 CPU 의 배정 수 */
	if(managed)	/* [한국어] managed 인터럽트였는가 */
		cm->managed_allocated--;	/* [한국어] 그 몫도 줄인다. managed_map 의 비트는 그대로 둔다 — 자리는 여전히 잡혀 있다 */

	if (cm->online)	/* [한국어] 온라인 CPU 인가 */
		m->total_allocated--;	/* [한국어] 오프라인 CPU 의 벡터는 애초에 이 합계에 들어 있지 않아 줄이면 안 된다 */

	if (!managed) {	/* [한국어] 보통의 인터럽트였는가 */
		cm->available++;	/* [한국어] 여유가 늘어난다. managed 는 자리가 여전히 잡혀 있어 늘지 않는다 */
		if (cm->online)	/* [한국어] 전역 여유는 온라인 CPU 몫만 센다 */
			m->global_available++;
	}
	trace_irq_matrix_free(bit, cpu, m, cm);	/* [한국어] 추적점 */
}

/**
 * irq_matrix_available - Get the number of globally available irqs
 * @m:		Pointer to the matrix to query
 * @cpudown:	If true, the local CPU is about to go down, adjust
 *		the number of available irqs accordingly
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_available - 시스템 전체에서 쓸 수 있는 벡터의 수를 알려 준다
 *
 * @m:       행렬.
 * @cpudown: 지금 CPU 가 내려가는 중인가. 참이면 그 CPU 몫을 뺀 값을 준다.
 * @return:  쓸 수 있는 벡터의 수.
 *
 * cpudown 인자가 이 함수의 유일한 복잡함이다. CPU 를 내리기 전에 "그
 * CPU 가 없어도 벡터가 충분한가" 를 확인해야 하는데, 그때는 아직 이
 * CPU 가 온라인이라 global_available 에 그 몫이 들어 있다.
 *
 * 미리 빼 보고 판정하는 것이 이 인자의 목적이다. 부족하면 CPU 를 내리지
 * 않는다 — 내린 뒤에 벡터가 모자라면 되돌릴 방법이 없기 때문이다.
 *
 * this_cpu_ptr 을 쓰므로 cpudown 이 참일 때는 반드시 내려가는 CPU 위에서
 * 불려야 한다.
 *
 * 실행 컨텍스트: CPU 오프라인 준비 경로 또는 진단.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 lapic_can_unplug_cpu() → [이 함수]
 */
unsigned int irq_matrix_available(struct irq_matrix *m, bool cpudown)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵. cpudown 일 때만 쓴다 */

	if (!cpudown)	/* [한국어] 보통의 조회인가 */
		return m->global_available;	/* [한국어] 전역 합계를 그대로 */
	return m->global_available - cm->available;	/* [한국어] 이 CPU 가 빠진 뒤의 값. 내리기 전에 미리 확인해 보는 것이다 */
}

/**
 * irq_matrix_reserved - Get the number of globally reserved irqs
 * @m:		Pointer to the matrix to query
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_reserved - 예약만 되고 아직 실현되지 않은 인터럽트의 수
 *
 * @m:      행렬.
 * @return: global_reserved 값 그대로.
 *
 * 위 available 과 짝을 이루어 쓰인다. CPU 를 내려도 되는지 판정할 때
 * "남을 여유" 와 "실현될 수 있는 예약" 을 견주어야 하기 때문이다.
 *
 * 예약이 여유보다 많으면 CPU 를 내리는 것이 위험하다 — 그 예약들이
 * 나중에 실현될 때 벡터가 모자라기 때문이다.
 *
 * 한 줄짜리 접근자를 함수로 두는 이유: struct irq_matrix 의 정의가 이
 * 파일 안에만 있다. 다른 파일에서는 필드에 직접 닿을 수 없다.
 *
 * 실행 컨텍스트: 어디서든. 락 없이 읽는다.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 lapic_can_unplug_cpu() → [이 함수]
 */
unsigned int irq_matrix_reserved(struct irq_matrix *m)
{
	return m->global_reserved;	/* [한국어] 구조체 정의가 이 파일 안에만 있어, 다른 파일은 이 접근자를 거쳐야 한다 */
}

/**
 * irq_matrix_allocated - Get the number of allocated non-managed irqs on the local CPU
 * @m:		Pointer to the matrix to search
 *
 * This returns number of allocated non-managed interrupts.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_allocated - 이 CPU 에 배정된 보통의(managed 가 아닌) 벡터 수
 *
 * @m:      행렬.
 * @return: allocated 에서 managed_allocated 를 뺀 값.
 *
 * 왜 그 뺄셈인가: allocated 는 managed 를 포함한 총수이고, 여기서는
 * 보통의 인터럽트만 세고 싶다.
 *
 * 무엇에 쓰는가: CPU 를 내릴 때 옮겨야 할 인터럽트의 수를 미리 아는 데
 * 쓴다. managed 인터럽트는 옮기지 않고 그냥 끄므로 셀 필요가 없고,
 * 보통의 인터럽트만 다른 CPU 로 옮겨야 한다.
 *
 * 실행 컨텍스트: CPU 오프라인 준비 경로, 그 CPU 위에서.
 *
 * 호출 체인:
 *   arch/x86/kernel/apic/vector.c 의 lapic_can_unplug_cpu() → [이 함수]
 */
unsigned int irq_matrix_allocated(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);	/* [한국어] 자기 CPU 의 맵 */

	return cm->allocated - cm->managed_allocated;	/* [한국어] 총수에서 managed 몫을 뺀다. 옮겨야 할 인터럽트의 수가 된다 */
}

/* [한국어] debugfs 출력. 진단 기능을 켠 빌드에만 있다. */
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] irq debugfs 를 켠 빌드 */
/**
 * irq_matrix_debug_show - Show detailed allocation information
 * @sf:		Pointer to the seq_file to print to
 * @m:		Pointer to the matrix allocator
 * @ind:	Indentation for the print format
 *
 * Note, this is a lockless snapshot.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_matrix_debug_show - 벡터 배정 상황을 표로 출력한다
 *
 * @sf:  출력할 seq_file.
 * @m:   행렬.
 * @ind: 들여쓰기 칸 수. 호출자가 계층 구조를 표현하는 데 쓴다.
 *
 * 벡터 고갈을 진단할 때 보는 출력이다. 전역 요약 네 줄 뒤에 CPU 마다
 * 한 줄씩 나온다.
 *
 * CPU 별 줄의 다섯 숫자가 이 파일의 회계 그대로다:
 *   avl — available. 아직 쓸 수 있는 자리.
 *   man — managed. 잡아 둔 managed 자리.
 *   mac — managed_allocated. 그중 실제로 쓰는 것.
 *   act — allocated. 배정된 총수(managed 포함).
 *   vectors — alloc_map 을 사람이 읽는 형태로.
 *
 * kernel-doc 의 Note 가 중요하다 — 락 없는 스냅숏이다. 출력하는 동안
 * 다른 CPU 가 벡터를 배정하거나 반납할 수 있어, 숫자들이 서로 앞뒤가
 * 맞지 않을 수 있다.
 *
 * 왜 락을 잡지 않는가: 이 출력은 CPU 수에 비례해 길다. 그 동안 벡터
 * 락을 쥐면 인터럽트 설정이 전부 막힌다. 진단 목적이라 정확도보다
 * 방해하지 않는 것이 중요하다.
 *
 * cpus_read_lock 은 잡는다. 그것은 벡터 락이 아니라 CPU 핫플러그를 막는
 * 것이며, 순회 중에 CPU 가 사라져 맵이 해제되는 것을 방지한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. debugfs 읽기.
 *
 * 호출 체인:
 *   x86 의 벡터 도메인 debug_show → [이 함수]
 */
void irq_matrix_debug_show(struct seq_file *sf, struct irq_matrix *m, int ind)
{
	unsigned int nsys = bitmap_weight(m->system_map, m->matrix_bits);	/* [한국어] 시스템이 못 박은 벡터의 수. 카운터가 없어 그때그때 센다 — 진단 경로라 비용이 문제되지 않는다 */
	int cpu;	/* [한국어] 순회 중인 CPU */

	seq_printf(sf, "Online bitmaps:   %6u\n", m->online_maps);	/* [한국어] 온라인 CPU 의 수 */
	seq_printf(sf, "Global available: %6u\n", m->global_available);	/* [한국어] 시스템 전체의 여유. 0 에 가까우면 벡터 고갈이 임박한 것이다 */
	seq_printf(sf, "Global reserved:  %6u\n", m->global_reserved);	/* [한국어] 실현되지 않은 예약. 위 available 보다 크면 위험하다 */
	seq_printf(sf, "Total allocated:  %6u\n", m->total_allocated);	/* [한국어] 실제로 배정된 총수 */
	seq_printf(sf, "System: %u: %*pbl\n", nsys, m->matrix_bits,	/* [한국어] 시스템 벡터의 수와 그 목록 */
		   m->system_map);	/* [한국어] %*pbl 이 "0-31,48" 같은 형태로 찍는다 */
	seq_printf(sf, "%*s| CPU | avl | man | mac | act | vectors\n", ind, " ");	/* [한국어] CPU 별 표의 머리말. 다섯 숫자가 이 파일의 회계 그대로다 */
	cpus_read_lock();	/* [한국어] 핫플러그를 막는다. 벡터 락이 아니라 CPU 가 사라져 맵이 해제되는 것을 방지하는 것이다 */
	for_each_online_cpu(cpu) {	/* [한국어] 온라인 CPU 만. 오프라인 것은 보여 줄 의미가 적다 */
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);	/* [한국어] 그 CPU 의 맵 */

		seq_printf(sf, "%*s %4d  %4u  %4u  %4u %4u  %*pbl\n", ind, " ",	/* [한국어] 머리말과 폭을 맞춘 한 줄 */
			   cpu, cm->available, cm->managed,	/* [한국어] CPU 번호, 여유, 잡아 둔 managed 자리 */
			   cm->managed_allocated, cm->allocated,	/* [한국어] 그중 쓰는 것, 배정된 총수 */
			   m->matrix_bits, cm->alloc_map);	/* [한국어] 배정된 벡터의 목록. 어느 번호가 쓰이는지 눈으로 확인할 수 있다 */
	}
	cpus_read_unlock();	/* [한국어] 핫플러그 잠금을 푼다 */
}
#endif
