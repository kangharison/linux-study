// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the interrupt descriptor management code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */

/*
 * [한국어 설명] 인터럽트 서술자(irq_desc)의 생애 관리 (irqdesc.c)
 *
 * === 파일의 역할 ===
 * struct irq_desc 는 리눅스가 인터럽트 하나에 대해 아는 모든 것을 담은
 * 구조체다. 어떤 칩에 붙어 있는지, 어떤 흐름 처리기를 쓰는지, 어떤
 * 핸들러들이 등록돼 있는지, 몇 번이나 발생했는지가 전부 여기 들어간다.
 * 이 파일은 그 구조체를 만들고, 번호로 찾고, 없애는 일을 맡는다.
 *
 * 인터럽트 번호는 처음부터 끝까지 촘촘하지 않다. MSI 를 쓰는 PCI 장치가
 * 붙었다 떨어졌다 하면 번호가 듬성듬성 비게 된다. 그래서
 * CONFIG_SPARSE_IRQ 빌드에서는 서술자를 배열이 아니라 메이플 트리
 * (maple tree)에 담아, 쓰이는 번호만 메모리를 차지하게 한다. 이 파일의
 * 절반 가까이가 그 희소(sparse) 관리 코드이고, 나머지 절반은 옛
 * 고정 배열 방식을 위한 대칭 구현이다.
 *
 * 서술자를 해제할 때 곧바로 kfree 하지 않고 RCU 유예 기간을 거치는 것도
 * 이 파일의 중요한 설계다. /proc/interrupts 를 읽는 쪽이 락 없이
 * 서술자를 훑고 있을 수 있기 때문이다.
 *
 * 마지막으로 이 파일은 인터럽트가 실제로 발생했을 때 코어로 들어오는
 * 관문 — generic_handle_irq() 계열 — 도 제공한다. 아키텍처 코드가
 * 하드웨어 벡터에서 이 함수들을 부르면서 공용 코드가 시작된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 처리의 전체 흐름은 이렇다.
 *
 *   (하드웨어 인터럽트)
 *     → 아키텍처 벡터 (arch/<아키텍처>/kernel/irq.c 등)
 *     → generic_handle_irq() / generic_handle_domain_irq()   ← 이 파일
 *     → desc->handle_irq()  = handle_level_irq 등 (kernel/irq/chip.c)
 *     → handle_irq_event()  (kernel/irq/handle.c)
 *     → action->handler()   드라이버 핸들러
 *
 * 즉 이 파일은 아키텍처 고유 코드와 공용 인터럽트 코어가 만나는 첫
 * 지점이다. 여기서 하드웨어 번호가 서술자로 바뀌고, 그 뒤로는 아키텍처를
 * 몰라도 되는 코드만 실행된다.
 *
 * 생성 쪽으로 보면 반대편 끝에 있다. 드라이버가 인터럽트를 쓰려면 먼저
 * 서술자가 있어야 하고, 그것을 만드는 __irq_alloc_descs() 가 여기 있다.
 * 도메인(irqdomain.c)이 매핑을 만들 때 이 함수를 부른다.
 *
 * 실행 컨텍스트가 함수마다 크게 다르다. 할당·해제 계열은 프로세스
 * 문맥에서 sparse_irq_lock 뮤텍스를 잡고 동작하고, handle_irq_desc()
 * 계열은 인터럽트 문맥에서 락 없이 트리를 조회한다. 이 둘을 잇는 것이
 * RCU 다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 부르는 쪽):
 *   - 아키텍처 인터럽트 진입 코드 → generic_handle_irq() 계열
 *   - kernel/irq/irqdomain.c → __irq_alloc_descs(), irq_free_descs()
 *   - kernel/irq/manage.c, chip.c → __irq_get_desc_lock() 로 서술자를
 *     찾아 락을 잡는다. 이 파일이 그 조회·잠금 관용구를 제공한다.
 *   - fs/proc/interrupts.c → kstat_irqs_usr(), irq_get_next_irq()
 *
 * 아래쪽(이 파일이 부르는 쪽):
 *   - lib/maple_tree.c → 희소 서술자 저장소
 *   - kernel/irq/proc.c, debugfs.c → 서술자마다 항목을 만들고 지운다
 *   - kernel/irq/resend.c → irq_resend_init()
 *   - drivers/base 의 kobject → /sys/kernel/irq/N/ 항목
 *
 * 공유 자료구조: struct irq_desc 자체가 이 서브시스템 전체의 공유
 * 상태다. 특히 desc->lock 은 chip.c, manage.c, handle.c 가 모두 잡는
 * 락이고, desc->istate 는 코어만 만지는 상태 워드다.
 *
 * === 주요 함수/구조체 요약 ===
 * - irq_to_desc(): 인터럽트 번호로 서술자를 찾는다. 이 파일에서 가장
 *   많이 불리는 함수이며, 희소·비희소 두 판이 있다.
 * - __irq_alloc_descs(): 연속된 서술자 여러 개를 할당한다. MSI 처럼
 *   여러 벡터를 한꺼번에 쓰는 장치를 위해 구간 단위다.
 * - free_desc(): 서술자를 트리에서 빼고 RCU 유예 뒤 해제한다.
 * - handle_irq_desc(): 인터럽트 발생 시 공용 코어의 첫 관문.
 * - __irq_get_desc_lock() / __irq_put_desc_unlock(): 조회와 잠금을
 *   한 번에 하는 관용구. manage.c 와 chip.c 가 거의 모든 함수에서 쓴다.
 * - kstat_irqs_desc(): 발생 횟수 합계. 공유 인터럽트는 미리 합쳐 둔
 *   tot_count 를, per-CPU 인터럽트는 CPU 별 카운터의 합을 쓴다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc, irq_data, irq_chip — 이 파일이 만들고 관리하는 자료구조 전부 */
#include <linux/slab.h>	/* [한국어] kzalloc_node/kfree — 서술자를 NUMA 노드를 지정해 할당한다 */
#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL_GPL — 드라이버 모듈이 generic_handle_irq 등을 부른다 */
#include <linux/interrupt.h>	/* [한국어] irqaction, IRQ_ 플래그 등 인터럽트 API 의 공개 선언 */
#include <linux/kernel_stat.h>	/* [한국어] struct irqstat, kstat_incr_irqs_this_cpu — 발생 횟수 통계의 정의 */
#include <linux/maple_tree.h>	/* [한국어] 희소 서술자 저장소. 번호 구간을 효율적으로 검색·할당할 수 있어 배열을 대신한다 */
#include <linux/irqdomain.h>	/* [한국어] irq_resolve_mapping() — 도메인+하드웨어 번호를 서술자로 바꾼다 */
#include <linux/sysfs.h>	/* [한국어] sysfs_emit — /sys/kernel/irq/N/ 속성 파일 출력 */
#include <linux/string_choices.h>	/* [한국어] str_enabled_disabled() — 불린을 "enabled"/"disabled" 문자열로. sysfs 출력의 표현을 통일한다 */

#include "internals.h"	/* [한국어] 코어 전용 선언(desc->istate, irq_settings_*, unregister_irq_proc 등). 이 파일이 서술자 내부를 직접 만지므로 필수다 */

/*
 * lockdep: we want to handle all irq_desc locks as a single lock-class:
 */
static struct lock_class_key irq_desc_lock_class;
/* [한국어] 모든 서술자 락이 공유하는 lockdep 클래스 키.
 * 설정자: init_desc() 가 서술자를 만들 때마다 이 키를 건다.
 * 읽는 자: lockdep 검증기.
 * 값 범위: 내용 없는 표식 객체 — 주소 자체가 식별자다.
 * 동기화: 불필요.
 *
 * 왜 하나로 묶는가: 서술자마다 다른 클래스를 주면 lockdep 이 추적할
 * 클래스가 인터럽트 수만큼 늘어나 한계를 넘는다. 전부 한 클래스로
 * 보면 "서술자 락 안에서 다른 서술자 락" 을 재귀로 오해할 수 있지만,
 * 정상 경로에서는 그런 중첩이 없다. 예외인 중첩 컨트롤러는
 * generic-chip.c 가 별도 클래스를 따로 걸어 준다. */

#if defined(CONFIG_SMP)	/* [한국어] CPU 가 여럿일 때만 친화도(affinity) 개념이 있다 */
/*
 * [한국어]
 * irq_affinity_setup - 커널 파라미터 irqaffinity= 를 해석한다
 *
 * @str: "0-3,7" 형식의 CPU 목록 문자열
 * @return: 1 (__setup 규약상 "처리했음")
 *
 * 왜 필요한가: 인터럽트를 특정 CPU 에만 보내고 나머지는 지연에 민감한
 * 작업에 쓰고 싶을 때가 있다. 부팅 파라미터로 기본 친화도를 정해 두면
 * 이후 만들어지는 모든 인터럽트가 그 마스크로 시작한다.
 *
 * 부트 CPU 를 강제로 넣는 이유가 이 함수의 핵심이다. 사용자가 실수로
 * 존재하지 않는 CPU 만 지정하면 인터럽트가 갈 곳이 없어 부팅이 멈춘다.
 * 원본 주석대로 "무작위 커맨드라인 마스크 때문에 생기는 버그 신고" 를
 * 막기 위해 최소 한 CPU 는 보장한다.
 *
 * 실행 컨텍스트: 부팅 아주 초기 — 일반 할당자가 아직 없어
 * alloc_bootmem_cpumask_var 를 쓴다.
 *
 * 호출 체인:
 *   parse_early_param() (init/main.c) → __setup 테이블 → [이 함수]
 */
static int __init irq_affinity_setup(char *str)
{
	alloc_bootmem_cpumask_var(&irq_default_affinity);	/* [한국어] 부트 시점 전용 할당자. slab 이 아직 준비되지 않았다 */
	cpulist_parse(str, irq_default_affinity);	/* [한국어] "0-3,7" 같은 목록 표기를 비트마스크로 바꾼다 */
	/*
	 * Set at least the boot cpu. We don't want to end up with
	 * bugreports caused by random commandline masks
	 */
	cpumask_set_cpu(smp_processor_id(), irq_default_affinity);	/* [한국어] (위 영어 주석) 지금 실행 중인 CPU 를 무조건 넣는다. 이 한 줄이 잘못된 파라미터로 부팅이 멈추는 것을 막는다 */
	return 1;	/* [한국어] __setup 규약: 1 이면 이 파라미터를 소비했다는 뜻이다. 0 이면 init 에 인자로 넘어간다 */
}
__setup("irqaffinity=", irq_affinity_setup);	/* [한국어] 부팅 파라미터 테이블에 등록한다. 링커가 모아 두는 특수 섹션에 항목이 생긴다 */

/*
 * [한국어]
 * init_irq_default_affinity - 기본 친화도 마스크의 최종 초기화 (SMP 판)
 *
 * @return: 없음
 *
 * 위 irq_affinity_setup() 이 파라미터를 처리했을 수도, 안 했을 수도
 * 있다. 이 함수는 두 경우를 모두 정상 상태로 만든다. 파라미터가
 * 없었으면 마스크를 새로 잡고 모든 CPU 를 켠다.
 *
 * 두 번의 검사가 각각 다른 상황을 처리한다. cpumask_available 은
 * "할당된 적이 있는가" 를 보는데, CONFIG_CPUMASK_OFFSTACK 빌드에서는
 * cpumask_var_t 가 포인터라 이 검사가 의미를 갖는다. cpumask_empty 는
 * 할당은 됐지만 비어 있는 경우 — 이론상 파라미터가 빈 목록이었을 때 —
 * 를 잡는다.
 *
 * GFP_NOWAIT 인 이유: 이 시점에는 아직 회수(reclaim)를 돌릴 수 없다.
 * 부팅 초기라 실패할 일도 거의 없다.
 *
 * 실행 컨텍스트: early_irq_init() 안, 부팅 초기.
 *
 * 호출 체인:
 *   start_kernel() → early_irq_init() → [이 함수]
 */
static void __init init_irq_default_affinity(void)
{
	if (!cpumask_available(irq_default_affinity))	/* [한국어] 파라미터 파서가 잡아 두지 않았는가 */
		zalloc_cpumask_var(&irq_default_affinity, GFP_NOWAIT);	/* [한국어] 0 으로 채워 새로 잡는다. 부팅 초기라 대기 없는 할당을 쓴다 */
	if (cpumask_empty(irq_default_affinity))	/* [한국어] 비어 있는가 — 방금 잡았거나 파라미터가 빈 목록이었던 경우 */
		cpumask_setall(irq_default_affinity);	/* [한국어] 모든 CPU 를 켠다. 인터럽트가 아무 데도 못 가는 상태를 막는 기본값이다 */
}
#else	/* [한국어] 단일 프로세서 빌드 */
/*
 * [한국어]
 * init_irq_default_affinity - 기본 친화도 초기화 (UP 판, 빈 함수)
 *
 * @return: 없음
 *
 * CPU 가 하나뿐이면 친화도라는 개념 자체가 없다. 호출자
 * (early_irq_init)를 #ifdef 로 더럽히지 않으려고 빈 함수를 둔다.
 * 인라인되어 흔적도 남지 않는다.
 *
 * 실행 컨텍스트: early_irq_init() 안.
 *
 * 호출 체인:
 *   start_kernel() → early_irq_init() → [이 함수]
 */
static void __init init_irq_default_affinity(void)
{
}
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

#ifdef CONFIG_SMP	/* [한국어] 여기서부터 마스크 할당·해제도 SMP 전용이다 */
/*
 * [한국어]
 * alloc_masks - 서술자가 쓸 CPU 마스크들을 할당한다
 *
 * @desc: 대상 서술자 (아직 아무도 모르는 새 것)
 * @node: 할당할 NUMA 노드. 인터럽트가 주로 처리될 노드에 두면 캐시가 낫다.
 * @return: 0 성공, -ENOMEM 실패
 *
 * 서술자에는 최대 세 개의 CPU 마스크가 붙는다.
 *   affinity           — 사용자가 요청한 친화도. 항상 있다.
 *   effective_affinity — 하드웨어가 실제로 고른 CPU. 컨트롤러가 요청
 *                        마스크 중 하나만 고르는 경우가 많아, 그 결과를
 *                        따로 추적하는 빌드에서만 있다.
 *   pending_mask       — 인터럽트 처리 중에 들어온 친화도 변경 요청을
 *                        미뤄 두는 곳. 처리 중에 마스크를 바꾸면 위험한
 *                        컨트롤러를 위한 것이다.
 *
 * 계단식 에러 처리에 주목: 두 번째 할당이 실패하면 첫 번째를 풀고, 세
 * 번째가 실패하면 앞의 둘을 푼다. #ifdef 가 겹쳐 있어 읽기 어렵지만,
 * 각 해제가 자기 앞에 성공한 것만 정확히 되돌린다.
 *
 * 실행 컨텍스트: 서술자 생성, 프로세스 문맥. GFP_KERNEL 이라 잠들 수 있다.
 *
 * 호출 체인:
 *   init_desc() → [이 함수] → zalloc_cpumask_var_node()
 */
static int alloc_masks(struct irq_desc *desc, int node)
{
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.affinity,	/* [한국어] 요청 친화도 마스크. 세 개 중 유일하게 항상 필요하다 */
				     GFP_KERNEL, node))
		return -ENOMEM;	/* [한국어] 첫 할당 실패 — 아직 푼 것이 없다 */

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 하드웨어가 실제 고른 CPU 를 따로 추적하는 빌드 */
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.effective_affinity,	/* [한국어] 유효 친화도 마스크 */
				     GFP_KERNEL, node)) {
		free_cpumask_var(desc->irq_common_data.affinity);	/* [한국어] 앞서 성공한 것을 되돌린다 */
		return -ENOMEM;	/* [한국어] 유효 친화도 마스크 할당 실패. 바로 위에서 요청 친화도 마스크를 되돌렸으므로 남은 자원이 없다 */
	}
#endif

#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 친화도 변경을 미뤄 두어야 하는 아키텍처 (x86 등) */
	if (!zalloc_cpumask_var_node(&desc->pending_mask, GFP_KERNEL, node)) {	/* [한국어] 미뤄 둔 변경을 담을 마스크 */
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 그 마스크를 잡았던 빌드에서만 푼다 */
		free_cpumask_var(desc->irq_common_data.effective_affinity);	/* [한국어] 두 번째 것을 되돌린다 */
#endif
		free_cpumask_var(desc->irq_common_data.affinity);	/* [한국어] 첫 번째 것도 되돌린다 */
		return -ENOMEM;	/* [한국어] 미뤄 둔 변경 마스크 할당 실패. 앞의 두 마스크를 모두 되돌린 뒤라 새는 것이 없다 */
	}
#endif
	return 0;	/* [한국어] 이 빌드에 필요한 마스크가 모두 준비됐다 */
}

/*
 * [한국어]
 * irq_redirect_work - 다른 CPU 로 넘긴 인터럽트를 그 CPU 에서 처리한다
 *
 * @work: 서술자 안에 박혀 있는 irq_work 항목
 * @return: 없음
 *
 * 무엇을 위한 것인가: 여러 인터럽트를 한 선으로 묶어 올리는
 * 디먹스(demultiplex) 컨트롤러에서, 자식 인터럽트의 친화도가 지금
 * CPU 를 포함하지 않을 수 있다. 그럴 때 아래 demux_redirect_remote()
 * 가 이 작업 항목을 목표 CPU 에 던지고, 그 CPU 에서 이 함수가 실행된다.
 *
 * container_of 로 서술자를 되찾는다. irq_work 구조체가 서술자 안에
 * 박혀 있으므로, 작업 항목 주소에서 오프셋을 빼면 서술자가 나온다.
 * 별도의 참조 카운트가 없어도 되는 이유는, 작업이 큐에 있는 동안
 * 서술자가 해제되지 않도록 desc->action 검사가 막아 주기 때문이다.
 *
 * 실행 컨텍스트: 목표 CPU 의 하드 인터럽트 문맥
 * (IRQ_WORK_INIT_HARD 로 초기화했다). 그래야 인터럽트 처리기가
 * 기대하는 문맥과 맞는다.
 *
 * 호출 체인:
 *   demux_redirect_remote() → irq_work_queue_on() → (다른 CPU) →
 *   irq_work_run() → [이 함수] → handle_irq_desc()
 */
static void irq_redirect_work(struct irq_work *work)
{
	handle_irq_desc(container_of(work, struct irq_desc, redirect.work));	/* [한국어] 작업 항목 주소에서 서술자를 복원해 정상 처리 경로로 넣는다. 이 CPU 가 친화도 안에 있으므로 이번에는 그냥 처리된다 */
}

/*
 * [한국어]
 * desc_smp_init - 서술자의 SMP 관련 필드를 초기화한다
 *
 * @desc:     대상 서술자
 * @node:     이 서술자가 속한 NUMA 노드
 * @affinity: 초기 친화도. NULL 이면 시스템 기본값을 쓴다.
 * @return:   없음
 *
 * alloc_masks() 가 마스크의 "그릇" 을 만들었다면 이 함수는 그 안에
 * 값을 채운다. 두 함수가 나뉜 이유는 재초기화 때문이다. 비희소 빌드의
 * free_desc() 는 서술자를 해제하지 않고 기본값으로 되돌리는데, 그때
 * 마스크를 다시 할당하면 안 되고 값만 다시 채워야 한다.
 *
 * affinity 가 NULL 인 경우가 대부분이다. MSI 처럼 드라이버가 원하는
 * CPU 를 지정하는 경우에만 값이 들어온다.
 *
 * 실행 컨텍스트: 서술자 생성 또는 재설정. 생성 시에는 락이 필요 없고,
 * 재설정 시에는 호출자가 desc->lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   desc_set_defaults() → [이 함수]
 */
static void desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity)
{
	if (!affinity)	/* [한국어] 호출자가 원하는 CPU 를 지정하지 않았는가 */
		affinity = irq_default_affinity;	/* [한국어] 부팅 파라미터나 setall 로 정해진 시스템 기본값 */
	cpumask_copy(desc->irq_common_data.affinity, affinity);	/* [한국어] 값을 복사한다. 포인터를 저장하지 않는 이유는 호출자의 마스크가 스택에 있을 수 있어서다 */

#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 미뤄 둔 변경 마스크가 있는 빌드 */
	cpumask_clear(desc->pending_mask);	/* [한국어] 미뤄 둔 변경이 없는 상태로 시작한다 */
#endif
#ifdef CONFIG_NUMA	/* [한국어] 노드 개념이 있는 빌드에만 이 필드가 존재한다 */
	desc->irq_common_data.node = node;	/* [한국어] /proc/irq/N/node 로 노출되고, 이후 관련 할당의 노드 힌트로 쓰인다 */
#endif
	desc->redirect.work = IRQ_WORK_INIT_HARD(irq_redirect_work);	/* [한국어] 다른 CPU 로 넘길 때 쓸 작업 항목. HARD 판이라 목표 CPU 의 하드 인터럽트 문맥에서 실행된다 */
}

/*
 * [한국어]
 * free_masks - 서술자의 CPU 마스크들을 해제한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * alloc_masks() 의 반대다. 해제 순서가 할당 순서와 다른데(pending 을
 * 먼저), 각 마스크가 독립적이라 순서가 의미를 갖지 않기 때문이다.
 *
 * 언제 불리는가: irq_kobj_release() — 즉 RCU 유예와 kobject 참조가
 * 모두 끝난 뒤다. 그 전에 풀면 /proc 을 읽던 쪽이 해제된 마스크를
 * 만질 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥 (RCU 콜백 이후).
 *
 * 호출 체인:
 *   irq_kobj_release() / early_irq_init() 의 실패 경로 → [이 함수]
 */
static void free_masks(struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 이 빌드에만 존재하는 마스크 */
	free_cpumask_var(desc->pending_mask);	/* [한국어] 미뤄 둔 변경 마스크 */
#endif
	free_cpumask_var(desc->irq_common_data.affinity);	/* [한국어] 요청 친화도. 항상 있다 */
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 추적하는 빌드 */
	free_cpumask_var(desc->irq_common_data.effective_affinity);	/* [한국어] 하드웨어가 고른 CPU 마스크 */
#endif
}

#else	/* [한국어] 단일 프로세서 빌드 — 아래 세 함수는 전부 빈 껍데기다 */
/*
 * [한국어]
 * alloc_masks - CPU 마스크 할당 (UP 판, 아무 일도 하지 않음)
 *
 * @desc: 무시
 * @node: 무시
 * @return: 항상 0 (성공)
 *
 * CPU 가 하나면 친화도 마스크가 필요 없다. 호출자인 init_desc() 를
 * #ifdef 로 나누지 않으려고 성공만 반환하는 인라인 함수를 둔다.
 *
 * 호출 체인:
 *   init_desc() → [이 함수]
 */
static inline int
alloc_masks(struct irq_desc *desc, int node) { return 0; }	/* [한국어] 잡을 것이 없으므로 성공만 알린다 */
/*
 * [한국어]
 * desc_smp_init - SMP 필드 초기화 (UP 판, 빈 함수)
 *
 * @desc:     무시
 * @node:     무시
 * @affinity: 무시
 * @return:   없음
 *
 * UP 빌드에는 채울 SMP 필드가 없다. 리다이렉트 작업 항목도 필요 없다 —
 * 넘길 다른 CPU 가 없기 때문이다.
 *
 * 호출 체인:
 *   desc_set_defaults() → [이 함수]
 */
static inline void
desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity) { }	/* [한국어] 채울 필드가 없다 */
/*
 * [한국어]
 * free_masks - CPU 마스크 해제 (UP 판, 빈 함수)
 *
 * @desc: 무시
 * @return: 없음
 *
 * alloc_masks 가 아무것도 잡지 않았으므로 풀 것도 없다.
 *
 * 호출 체인:
 *   irq_kobj_release() → [이 함수]
 */
static inline void free_masks(struct irq_desc *desc) { }	/* [한국어] 풀 것이 없다 */
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/*
 * [한국어]
 * desc_set_defaults - 서술자를 "아무것도 붙지 않은" 초기 상태로 만든다
 *
 * @desc:     대상 서술자
 * @irq:      이 서술자의 인터럽트 번호
 * @node:     NUMA 노드
 * @affinity: 초기 친화도 (NULL 이면 기본값)
 * @owner:    이 인터럽트를 소유한 모듈. 모듈이 내려갈 때 참조를 막는 데 쓴다.
 * @return:   없음
 *
 * 새로 만든 서술자와, 비희소 빌드에서 해제된 서술자가 모두 이 함수를
 * 거쳐 같은 상태가 된다. 그 "같은 상태" 의 핵심이 세 가지다.
 *
 * (1) 칩은 no_irq_chip — 아무 하드웨어에도 붙어 있지 않다. NULL 이
 *     아닌 이유는 코어가 chip 포인터를 검사 없이 역참조해서다.
 * (2) 처리기는 handle_bad_irq — 이 상태에서 인터럽트가 들어오면
 *     경고를 찍는다. 조용히 무시하지 않는 것이 중요하다.
 * (3) 비활성이면서 마스크됨, depth 는 1 — depth 는 비활성 중첩
 *     횟수라, 1 로 시작한다는 것은 누군가 enable_irq() 를 한 번
 *     불러야 열린다는 뜻이다.
 *
 * 상태 워드가 셋으로 나뉘어 있다는 점을 여기서 볼 수 있다.
 * irq_settings_clr_and_set 이 만지는 것은 설정 플래그(_IRQ_ 계열),
 * irqd_set 이 만지는 것은 하드웨어 상태(IRQD_ 계열)이며, istate
 * (IRQS_ 계열)는 여기서 건드리지 않는다.
 *
 * 실행 컨텍스트: 생성 시에는 락 없이, 재설정 시에는 desc->lock 보유.
 *
 * 호출 체인:
 *   init_desc() / free_desc()(비희소 판) → [이 함수] → desc_smp_init()
 */
static void desc_set_defaults(unsigned int irq, struct irq_desc *desc, int node,
			      const struct cpumask *affinity, struct module *owner)
{
	desc->irq_common_data.handler_data = NULL;	/* [한국어] 흐름 처리기가 쓰는 사설 데이터. 체인 처리기가 부모 정보를 여기 둔다 */
	desc->irq_common_data.msi_desc = NULL;	/* [한국어] MSI 서술자 연결. MSI 인터럽트에만 설정된다 */

	desc->irq_data.common = &desc->irq_common_data;	/* [한국어] 자기 안의 공통 데이터를 가리키는 자기 참조. 계층형 도메인에서는 여러 irq_data 가 이 하나를 공유한다 */
	desc->irq_data.irq = irq;	/* [한국어] 리눅스 인터럽트 번호. irq_data 만 들고 다니는 콜백들이 번호를 알아야 할 때 쓴다 */
	desc->irq_data.chip = &no_irq_chip;	/* [한국어] 더미 칩. NULL 로 두지 않는 이유는 코어 곳곳이 검사 없이 역참조하기 때문이다 */
	desc->irq_data.chip_data = NULL;	/* [한국어] 칩 드라이버의 사설 데이터. 아직 붙은 칩이 없다 */
	irq_settings_clr_and_set(desc, ~0, _IRQ_DEFAULT_INIT_FLAGS);	/* [한국어] 설정 플래그를 전부 지우고 기본값만 세운다. ~0 은 "전부 지움" 이다. 이것이 세 상태 워드 중 첫 번째(_IRQ_ 계열) */
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);	/* [한국어] 두 번째 상태 워드(IRQD_ 계열). 논리적으로 꺼져 있다는 뜻 */
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);	/* [한국어] 하드웨어적으로도 막혀 있다. DISABLED 와 MASKED 를 나눠 두는 것이 게으른 비활성(lazy disable)을 가능하게 한다 */
	desc->handle_irq = handle_bad_irq;	/* [한국어] 이 상태에서 인터럽트가 오면 경고를 찍는다. 조용히 무시하면 원인 모를 인터럽트 폭주를 놓친다 */
	desc->depth = 1;	/* [한국어] 비활성 중첩 깊이. 1 이므로 enable_irq() 한 번으로 열린다. 0 에서 시작하면 요청도 안 된 인터럽트가 열려 버린다 */
	desc->irq_count = 0;	/* [한국어] 오탐 검출용 누적 횟수. spurious.c 가 쓴다 */
	desc->irqs_unhandled = 0;	/* [한국어] 그중 아무 핸들러도 처리하지 못한 횟수 */
	desc->tot_count = 0;	/* [한국어] 공유 인터럽트의 전체 발생 횟수 합계. CPU 별 합산을 매번 하지 않으려는 최적화다 */
	desc->name = NULL;	/* [한국어] /proc/interrupts 에 보일 이름. 요청 시 설정된다 */
	desc->owner = owner;	/* [한국어] 소유 모듈. 모듈이 내려간 뒤 이 인터럽트를 만지는 것을 막는 데 쓴다 */
	desc_smp_init(desc, node, affinity);	/* [한국어] SMP 관련 필드는 별도 함수로. UP 빌드에서는 사라진다 */
}

static unsigned int nr_irqs = NR_IRQS;
/* [한국어] 시스템이 지원하는 인터럽트 번호의 상한.
 * 설정자: early_irq_init() 이 아키텍처가 알려 준 값으로 조정하고,
 *   irq_expand_nr_irqs() 가 희소 빌드에서 필요할 때 늘린다.
 * 읽는 자: 번호 유효성 검사가 필요한 모든 곳 — irq_free_descs(),
 *   irq_find_at_or_after(), /proc 순회 등.
 * 값 범위: NR_IRQS(컴파일 시 기본값) ~ MAX_SPARSE_IRQS.
 * 동기화: 늘리는 쪽은 sparse_irq_lock 아래에서만 한다. 읽는 쪽은 락
 *   없이 보는데, 값이 늘기만 하고 줄지 않아 낡은 값을 봐도 과하게
 *   보수적으로 판단할 뿐 안전하기 때문이다.
 *
 * static 인 것에 주목: 예전에는 전역이라 아무나 고칠 수 있었다. 아래
 * 두 접근자를 통해서만 만지게 막아, 누가 언제 바꾸는지 추적할 수
 * 있게 했다. */

/**
 * irq_get_nr_irqs() - Number of interrupts supported by the system.
 */
/*
 * [한국어]
 * irq_get_nr_irqs - 시스템이 지원하는 인터럽트 수를 돌려준다
 *
 * @return: 현재 nr_irqs 값
 *
 * 왜 함수인가: nr_irqs 를 static 으로 감췄기 때문이다. 예전에는 전역
 * 변수를 아무 데서나 직접 읽고 썼는데, 누가 언제 바꾸는지 추적할 수
 * 없어 접근자로 감쌌다.
 *
 * 락을 잡지 않는 이유: 이 값은 부팅 때 정해지고 이후로는 늘기만 한다.
 * 낡은(작은) 값을 읽으면 실제보다 보수적으로 판단할 뿐 위험하지 않다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   아키텍처 코드 / 드라이버 → [이 함수]
 */
unsigned int irq_get_nr_irqs(void)
{
	return nr_irqs;	/* [한국어] 단순 읽기. 원자성이 필요 없는 이유는 위 설명대로다 */
}
EXPORT_SYMBOL_GPL(irq_get_nr_irqs);	/* [한국어] 모듈로 빌드되는 아키텍처·드라이버 코드가 부른다 */

/**
 * irq_set_nr_irqs() - Set the number of interrupts supported by the system.
 * @nr: New number of interrupts.
 *
 * Return: @nr.
 */
/*
 * [한국어]
 * irq_set_nr_irqs - 지원 인터럽트 수를 설정한다
 *
 * @nr: 새 값
 * @return: 방금 설정한 @nr 그대로
 *
 * 누가 부르는가: 아키텍처의 arch_probe_nr_irqs() 구현이다. 하드웨어를
 * 살펴본 뒤 "이 기계는 인터럽트가 몇 개까지 있다" 를 알려 준다.
 *
 * 반환값이 인자 그대로인 이유: 호출자가 `return irq_set_nr_irqs(n);`
 * 처럼 한 줄로 쓸 수 있게 하려는 편의다.
 *
 * 주의: 아무 때나 부르면 안 된다. 부팅 초기, 서술자가 만들어지기
 * 전이어야 한다. 이미 만들어진 서술자보다 작은 값을 넣으면 그 서술자들이
 * 범위 검사에서 걸러져 접근 불가가 된다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드. 그래서 락이 없다.
 *
 * 호출 체인:
 *   early_irq_init() → arch_probe_nr_irqs() (아키텍처 구현) → [이 함수]
 */
unsigned int irq_set_nr_irqs(unsigned int nr)
{
	nr_irqs = nr;	/* [한국어] 부팅 초기라 경쟁이 없어 그냥 대입한다 */

	return nr;	/* [한국어] 호출자가 한 줄로 쓸 수 있게 인자를 그대로 돌려준다 */
}
EXPORT_SYMBOL_GPL(irq_set_nr_irqs);	/* [한국어] 모듈로 빌드되는 아키텍처 코드용 */

static DEFINE_MUTEX(sparse_irq_lock);
/* [한국어] 서술자의 할당·해제를 직렬화하는 뮤텍스.
 * 설정자·읽는 자: __irq_alloc_descs(), irq_free_descs(),
 *   irq_sysfs_init(), irq_mark_irq(), 그리고 외부에 노출된
 *   irq_lock_sparse()/irq_unlock_sparse().
 * 값 범위: 뮤텍스 — 잠들 수 있으므로 인터럽트 문맥에서 쓸 수 없다.
 * 동기화: 이 락이 지키는 것은 아래 메이플 트리의 구조 변경이다.
 *   조회(irq_to_desc)는 이 락 없이 RCU 로 한다. 그래서 인터럽트
 *   문맥에서도 서술자를 찾을 수 있다.
 *
 * 아래 트리 초기화에 이 락을 넘기는 것에 주목 — 메이플 트리가
 * MT_FLAGS_LOCK_EXTERN 으로 "외부 락을 쓴다" 고 선언하고, 그 락이
 * 무엇인지 여기서 알려 준다. lockdep 이 그 규약을 검증한다. */
static struct maple_tree sparse_irqs = MTREE_INIT_EXT(sparse_irqs,
					MT_FLAGS_ALLOC_RANGE |
					MT_FLAGS_LOCK_EXTERN |
					MT_FLAGS_USE_RCU,
					sparse_irq_lock);
/* [한국어] 인터럽트 번호 → 서술자 포인터 매핑을 담는 메이플 트리.
 * 설정자: irq_insert_desc(), delete_irq_desc() — 둘 다
 *   sparse_irq_lock 아래에서 불린다.
 * 읽는 자: irq_to_desc() 가 인터럽트 문맥에서도 락 없이 조회한다.
 * 값 범위: 0 ~ nr_irqs-1 번호에 대응하는 struct irq_desc 포인터.
 * 동기화: 쓰기는 뮤텍스, 읽기는 RCU. 해제된 서술자를 읽는 쪽이
 *   보지 않도록 free_desc() 가 call_rcu 로 유예를 둔다.
 *
 * 세 플래그의 뜻:
 *   MT_FLAGS_ALLOC_RANGE — 빈 구간을 찾아 주는 기능을 쓴다.
 *     __irq_alloc_descs() 가 "연속된 n 개가 비어 있는 곳" 을 찾을 때
 *     필요하다. 배열 시절에는 이것을 직접 훑어야 했다.
 *   MT_FLAGS_LOCK_EXTERN — 트리가 자기 락을 쓰지 않고 위 뮤텍스를
 *     쓴다. 트리 조작과 그 주변 작업(sysfs 등록 등)을 한 임계
 *     구역으로 묶기 위해서다.
 *   MT_FLAGS_USE_RCU — 조회를 락 없이 할 수 있게 한다. 인터럽트
 *     문맥에서 뮤텍스를 잡을 수 없으므로 필수다. */

/*
 * [한국어]
 * irq_find_free_area - 연속된 빈 인터럽트 번호 구간을 찾는다
 *
 * @from: 검색을 시작할 번호
 * @cnt:  필요한 연속 개수
 * @return: 찾은 구간의 시작 번호, 없으면 -ENOSPC
 *
 * 왜 구간인가: MSI-X 를 쓰는 장치는 벡터를 수십 개 쓰는데, 번호가
 * 연속이어야 관리가 단순하다. 그래서 하나씩이 아니라 구간 단위로
 * 할당한다.
 *
 * 메이플 트리의 mas_empty_area() 가 이 검색을 로그 시간에 해 준다.
 * 배열 시절에는 비트맵을 선형으로 훑어야 했다. MA_STATE 는 트리를
 * 순회하는 커서를 스택에 만드는 매크로다.
 *
 * 실행 컨텍스트: __irq_alloc_descs() 안, sparse_irq_lock 보유 상태.
 * 락 없이 부르면 찾은 구간이 반환 전에 남에게 넘어갈 수 있다.
 *
 * 호출 체인:
 *   __irq_alloc_descs() → [이 함수] → mas_empty_area()
 */
static int irq_find_free_area(unsigned int from, unsigned int cnt)
{
	MA_STATE(mas, &sparse_irqs, 0, 0);	/* [한국어] 트리 순회 커서를 스택에 만든다. 시작·끝을 0 으로 두는 것은 아래 검색이 범위를 다시 지정하기 때문이다 */

	if (mas_empty_area(&mas, from, MAX_SPARSE_IRQS, cnt))	/* [한국어] from 부터 MAX_SPARSE_IRQS 사이에서 cnt 개가 연속으로 빈 곳을 찾는다. 0 이 아니면 실패 */
		return -ENOSPC;	/* [한국어] 그만한 연속 구간이 없다. 호출자가 -ENOMEM 등으로 바꿔 올린다 */
	return mas.index;	/* [한국어] 커서에 남은 시작 번호. 아직 예약된 것은 아니라, 락을 놓기 전에 실제로 채워 넣어야 한다 */
}

/*
 * [한국어]
 * irq_find_at_or_after - 주어진 번호 이상에서 처음 존재하는 서술자를 찾는다
 *
 * @offset: 검색 시작 번호
 * @return: 찾은 인터럽트 번호, 없으면 nr_irqs
 *
 * 무엇을 위한 것인가: /proc/interrupts 처럼 존재하는 인터럽트를 순서대로
 * 훑어야 하는 곳이 있다. 희소 배치라 번호가 듬성듬성하므로, 다음 번호를
 * 하나씩 시도하는 대신 트리에게 "이 번호 이상 중 첫 항목" 을 묻는다.
 *
 * 반환값이 nr_irqs 인 것이 "끝" 을 뜻한다. 호출자의 순회 루프가
 * `irq < nr_irqs` 조건이라 자연스럽게 멈춘다. -1 같은 오류 값을 쓰지
 * 않는 이유가 이것이다.
 *
 * RCU 로 보호하는 이유: 인터럽트 문맥이나 락 없는 순회에서도 불릴 수
 * 있어야 한다. guard(rcu)() 가 이 함수를 벗어날 때 자동으로 푼다.
 * 다만 반환된 번호가 호출자에게 도달할 즈음 그 서술자가 사라졌을 수
 * 있으므로, 호출자는 다시 조회해 확인해야 한다.
 *
 * 실행 컨텍스트: 제약 없음. RCU 읽기 구역만 잡는다.
 *
 * 호출 체인:
 *   irq_get_next_irq() → [이 함수] → mt_find()
 */
static unsigned int irq_find_at_or_after(unsigned int offset)
{
	unsigned long index = offset;	/* [한국어] mt_find 가 이 변수를 읽고 쓰므로 지역 복사본이 필요하다 */
	struct irq_desc *desc;	/* [한국어] 찾은 서술자 */

	guard(rcu)();	/* [한국어] 트리 조회 동안 서술자가 해제되지 않게 한다. 함수를 벗어나면 자동으로 풀린다 */
	desc = mt_find(&sparse_irqs, &index, nr_irqs);	/* [한국어] index 이상 nr_irqs 미만에서 첫 항목. index 는 찾은 위치로 갱신된다 */

	return desc ? irq_desc_get_irq(desc) : nr_irqs;	/* [한국어] 찾았으면 그 번호, 못 찾았으면 nr_irqs. 후자가 호출자의 순회 루프를 자연스럽게 끝낸다 */
}

/*
 * [한국어]
 * irq_insert_desc - 서술자를 번호에 등록한다
 *
 * @irq:  인터럽트 번호
 * @desc: 등록할 서술자
 * @return: 없음
 *
 * 이 한 줄이 서술자를 "존재하게" 만든다. 이 함수가 끝나는 순간부터
 * irq_to_desc(irq) 가 성공하고, 다른 CPU 가 그 서술자를 볼 수 있다.
 * 그래서 서술자가 완전히 초기화된 뒤에 불려야 한다.
 *
 * WARN_ON 으로 실패를 잡는 이유: 호출자들이 반환값을 처리할 구조가
 * 아니다. 여기서 실패하는 것은 메모리 부족뿐인데, 그 시점이면 이미
 * 서술자 할당에서 걸렸어야 정상이다. 즉 여기 실패는 거의 일어나지 않는
 * 이례적 상황이라 경고로 남기고 넘어간다.
 *
 * 실행 컨텍스트: sparse_irq_lock 보유, 프로세스 문맥. GFP_KERNEL 이라
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   alloc_descs() / early_irq_init() / irq_mark_irq() → [이 함수]
 */
static void irq_insert_desc(unsigned int irq, struct irq_desc *desc)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);	/* [한국어] 이 번호 하나만 가리키는 커서 (시작=끝=irq) */
	WARN_ON(mas_store_gfp(&mas, desc, GFP_KERNEL) != 0);	/* [한국어] 트리에 넣는다. 이 순간부터 다른 CPU 가 이 서술자를 볼 수 있다. 실패는 메모리 부족뿐이고 호출자가 처리할 구조가 아니라 경고만 남긴다 */
}

/*
 * [한국어]
 * delete_irq_desc - 번호에서 서술자를 지운다
 *
 * @irq: 인터럽트 번호
 * @return: 없음
 *
 * irq_insert_desc() 의 반대다. 이 함수가 끝나면 irq_to_desc(irq) 가
 * NULL 을 돌려주기 시작한다. 다만 이미 조회를 마치고 서술자 포인터를
 * 들고 있는 쪽이 있을 수 있으므로, 실제 메모리 해제는 RCU 유예 뒤로
 * 미뤄야 한다 — 그 일은 호출자인 free_desc() 가 한다.
 *
 * 실행 컨텍스트: sparse_irq_lock 보유, 프로세스 문맥.
 *
 * 호출 체인:
 *   free_desc() → [이 함수] → mas_erase()
 */
static void delete_irq_desc(unsigned int irq)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);	/* [한국어] 지울 번호를 가리키는 커서 */
	mas_erase(&mas);	/* [한국어] 트리에서 뺀다. 이 뒤로 새 조회는 실패하지만, 이미 진행 중인 조회는 여전히 유효한 포인터를 들고 있다 */
}

#ifdef CONFIG_SPARSE_IRQ	/* [한국어] 아래 kobj 타입은 희소 빌드에서만 정의된다 */
static const struct kobj_type irq_kobj_type;
/* [한국어] 아래에서 정의될 kobject 타입의 전방 선언.
 * 설정자: 파일 아래쪽의 정적 초기화 (CONFIG_SYSFS 여부에 따라 두 판).
 * 읽는 자: init_desc() 가 kobject_init() 에 넘긴다.
 * 값 범위: release 콜백과 sysfs 속성 그룹을 담은 상수 구조체.
 * 동기화: const 라 변경되지 않는다.
 *
 * 왜 전방 선언이 필요한가: init_desc() 가 이 타입을 쓰는데, 실제
 * 정의는 sysfs 관련 함수들 뒤에 와야 한다(그 함수들을 참조하므로).
 * 순환을 끊기 위해 선언을 앞으로 뺐다. */
#endif	/* [한국어] CONFIG_SPARSE_IRQ 분기의 끝 */

/*
 * [한국어]
 * init_desc - 서술자 하나를 쓸 수 있는 상태로 만든다
 *
 * @desc:     초기화할 서술자 (메모리는 호출자가 이미 확보했다)
 * @irq:      인터럽트 번호
 * @node:     NUMA 노드
 * @flags:    초기 IRQD_ 상태 플래그 (관리형 친화도 등)
 * @affinity: 초기 친화도
 * @owner:    소유 모듈
 * @return:   0 성공, -ENOMEM 통계 영역이나 마스크 할당 실패
 *
 * 왜 할당과 분리돼 있는가: 서술자 메모리를 얻는 방법이 두 가지다.
 * 희소 빌드는 kzalloc_node 로 하나씩 잡고, 비희소 빌드는 정적 배열의
 * 원소를 쓴다. 초기화 논리는 같으므로 여기 한 번만 둔다.
 *
 * 순서가 중요하다. 실패할 수 있는 할당(통계, 마스크)을 먼저 하고,
 * 실패할 수 없는 초기화(락, 뮤텍스, 기본값)를 나중에 한다. 그래야
 * 에러 경로에서 되돌릴 것이 적다.
 *
 * kstat_irqs 를 per-CPU 로 잡는 이유: 인터럽트마다 발생 횟수를 세는데,
 * 여러 CPU 가 동시에 같은 인터럽트를 처리할 수 있다. 공유 카운터를
 * 원자 연산으로 올리면 캐시 라인이 CPU 사이를 오가며 성능이 무너진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   alloc_desc() (희소) / early_irq_init() (비희소) → [이 함수] →
 *   alloc_percpu() / alloc_masks() / desc_set_defaults() / irq_resend_init()
 */
static int init_desc(struct irq_desc *desc, int irq, int node,
		     unsigned int flags,
		     const struct cpumask *affinity,
		     struct module *owner)
{
	desc->kstat_irqs = alloc_percpu(struct irqstat);	/* [한국어] CPU 마다 하나씩 있는 발생 횟수 카운터. 공유 카운터의 캐시 라인 다툼을 피하려는 것이다 */
	if (!desc->kstat_irqs)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 아무것도 잡지 않았으므로 되돌릴 것이 없다 */

	if (alloc_masks(desc, node)) {	/* [한국어] CPU 마스크들. UP 빌드에서는 항상 성공한다 */
		free_percpu(desc->kstat_irqs);	/* [한국어] 방금 잡은 통계 영역을 되돌린다 */
		return -ENOMEM;	/* [한국어] /sys/kernel/irq 디렉터리를 만들지 못했다. initcall 이 음수를 반환하면 부팅 로그에 경고가 남지만 부팅 자체는 계속된다 */
	}

	raw_spin_lock_init(&desc->lock);	/* [한국어] 이 인터럽트의 모든 상태를 지키는 락. 인터럽트 문맥에서 잡히므로 raw 여야 한다 */
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);	/* [한국어] 모든 서술자 락을 한 클래스로 묶는다. 안 그러면 lockdep 클래스가 인터럽트 수만큼 늘어난다 */
	mutex_init(&desc->request_mutex);	/* [한국어] request_irq/free_irq 를 직렬화하는 뮤텍스. 잠들 수 있는 칩 버스 접근을 포함하므로 스핀락일 수 없다 */
	init_waitqueue_head(&desc->wait_for_threads);	/* [한국어] synchronize_irq() 가 스레드 핸들러의 종료를 기다리는 큐 */
	desc_set_defaults(irq, desc, node, affinity, owner);	/* [한국어] 나머지 필드를 "아무것도 안 붙은" 상태로 채운다 */
	irqd_set(&desc->irq_data, flags);	/* [한국어] 호출자가 지정한 추가 상태. 관리형 친화도(IRQD_AFFINITY_MANAGED) 같은 것이 여기로 들어온다 */
	irq_resend_init(desc);	/* [한국어] 소프트웨어 재전송 기구 초기화 (kernel/irq/resend.c). 하드웨어가 재전송을 못 할 때 코어가 대신 한다 */
#ifdef CONFIG_SPARSE_IRQ	/* [한국어] 아래 둘은 서술자를 동적으로 만들고 없애는 희소 빌드에만 필요하다 */
	kobject_init(&desc->kobj, &irq_kobj_type);	/* [한국어] /sys/kernel/irq/N/ 항목의 뿌리. 참조 카운트가 1 로 시작한다 */
	init_rcu_head(&desc->rcu);	/* [한국어] 해제를 RCU 유예 뒤로 미룰 때 쓸 콜백 헤드 */
#endif

	return 0;	/* [한국어] 이 서술자는 이제 트리에 넣어도 되는 상태다 */
}

#ifdef CONFIG_SPARSE_IRQ	/* [한국어] 여기부터 파일의 절반은 희소 서술자 관리다. 대응하는 #else 가 아래에 있다 */

static void irq_kobj_release(struct kobject *kobj);
/* [한국어] kobject 참조가 0 이 될 때 불릴 해제 콜백의 전방 선언.
 * 설정자: 아래 irq_kobj_type 정적 초기화가 이 주소를 담는다.
 * 읽는 자: kobject_put() 이 참조를 0 으로 만들 때 코어가 부른다.
 * 값 범위: 이 파일 아래쪽의 정의.
 * 동기화: 함수 포인터라 변경되지 않는다.
 *
 * 전방 선언이 필요한 이유: irq_kobj_type 정의가 이 함수보다 앞에
 * 오는데, 그 정의가 이 함수의 주소를 필요로 한다. */

#ifdef CONFIG_SYSFS	/* [한국어] sysfs 를 쓰는 빌드에만 /sys/kernel/irq/ 항목이 있다 */
static struct kobject *irq_kobj_base;
/* [한국어] /sys/kernel/irq 디렉터리의 kobject.
 * 설정자: irq_sysfs_init() 이 postcore 단계에서 만든다.
 * 읽는 자: irq_sysfs_add() 가 서술자마다 항목을 붙일 부모로 쓴다.
 * 값 범위: NULL(아직 안 만들어짐) 또는 유효한 kobject.
 * 동기화: sparse_irq_lock 아래에서 설정된다. 읽는 쪽도 그 락을
 *   쥐고 있거나(alloc_descs 경로), 초기화 이후라 값이 바뀌지 않는다.
 *
 * NULL 검사가 중요한 이유: 부팅 초기에 만들어지는 인터럽트들은 이
 * 디렉터리가 생기기 전에 서술자를 얻는다. 그때는 sysfs 항목을 건너뛰고,
 * 나중에 irq_sysfs_init() 이 이미 있는 것들을 한꺼번에 등록한다. */

/* [한국어] 읽기 전용 sysfs 속성을 선언하는 축약 매크로.
 * <이름>_show 함수가 이미 정의돼 있다고 가정하고, 그것을 가리키는
 * <이름>_attr 구조체를 만든다. 아래 일곱 개 속성이 전부 이 매크로를
 * 쓴다 — 같은 두 줄을 일곱 번 반복하지 않으려는 것이다.
 * ## 은 토큰 이어붙이기 연산자로, per_cpu_count 를 넘기면
 * per_cpu_count_attr 라는 이름이 만들어진다.
 * 주석을 매크로 본문 안이 아니라 위에 두는 이유: 줄 잇기(\)가
 * 주석 제거보다 먼저 일어나므로, 본문 중간의 주석 줄은 매크로를
 * 그 자리에서 끊어 버릴 수 있다. */
#define IRQ_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)	/* [한국어] __ATTR_RO 가 0444 권한과 <이름>_show 를 묶어 준다. store 가 없어 쓰기는 커널이 거절한다 */

/*
 * [한국어]
 * per_cpu_count_show - /sys/kernel/irq/N/per_cpu_count 를 출력한다
 *
 * @kobj: 이 속성이 속한 kobject. 서술자 안에 박혀 있다.
 * @attr: 어떤 속성인지 (이 함수는 하나뿐이라 쓰지 않는다)
 * @buf:  출력 버퍼 (PAGE_SIZE)
 * @return: 쓴 바이트 수
 *
 * CPU 마다 이 인터럽트가 몇 번 발생했는지를 쉼표로 이어 출력한다.
 * /proc/interrupts 가 같은 정보를 표로 보여 주지만, 인터럽트 하나만
 * 보려면 그 큰 표를 파싱해야 한다. 이 파일은 그 수고를 던다.
 *
 * container_of 관용구: sysfs 콜백은 kobject 만 받는다. kobject 가
 * 서술자 안에 필드로 박혀 있으므로, 그 주소에서 필드 오프셋을 빼면
 * 서술자가 나온다. 이 파일의 모든 show 함수가 같은 첫 줄로 시작한다.
 *
 * 락을 잡지 않는 이유: per-CPU 카운터를 읽을 뿐이고, 값이 조금 낡아도
 * 통계로서 의미가 있다. 아래 다른 show 함수들은 서술자 필드를 보므로
 * 락을 잡는다.
 *
 * 실행 컨텍스트: 프로세스 문맥 (사용자가 파일을 읽을 때).
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/per_cpu_count → kobj_sysfs_ops.show →
 *   [이 함수] → irq_desc_kstat_cpu()
 */
static ssize_t per_cpu_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] kobject 주소에서 서술자를 복원한다. 이 파일 모든 show 함수의 공통 첫 줄 */
	ssize_t ret = 0;	/* [한국어] 지금까지 버퍼에 쓴 바이트 수 */
	char *p = "";	/* [한국어] 구분자. 첫 항목 앞에는 아무것도, 그 뒤로는 쉼표를 붙이려는 관용구다 */
	int cpu;	/* [한국어] 순회용 */

	for_each_possible_cpu(cpu) {	/* [한국어] online 이 아니라 possible 을 쓴다 — 잠시 오프라인인 CPU 의 누적값도 보여야 한다 */
		unsigned int c = irq_desc_kstat_cpu(desc, cpu);	/* [한국어] 그 CPU 의 발생 횟수 */

		ret += sysfs_emit_at(buf, ret, "%s%u", p, c);	/* [한국어] 오프셋 ret 부터 이어 쓴다. sysfs_emit_at 이 PAGE_SIZE 넘침을 대신 막아 준다 */
		p = ",";	/* [한국어] 다음부터는 쉼표를 앞에 붙인다 */
	}

	ret += sysfs_emit_at(buf, ret, "\n");	/* [한국어] sysfs 규약상 값 끝에 개행을 둔다 */
	return ret;	/* [한국어] 총 바이트 수. 커널이 이만큼을 사용자에게 복사한다 */
}
IRQ_ATTR_RO(per_cpu_count);	/* [한국어] 위 함수를 가리키는 읽기 전용 속성 구조체를 만든다 */

/*
 * [한국어]
 * chip_name_show - /sys/kernel/irq/N/chip_name 을 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수 (칩이 없으면 0 — 빈 파일)
 *
 * 이 인터럽트를 담당하는 컨트롤러의 이름이다. "GICv3", "IO-APIC" 같은
 * 값이 나온다. 어느 컨트롤러에 붙었는지 알면 인터럽트 문제를 좁히기
 * 쉽다.
 *
 * 락이 필요한 이유: chip 포인터가 실행 중에 바뀔 수 있다 —
 * irq_setup_alt_chip() 이 트리거 방식 변경 때 통째로 갈아 끼운다.
 * 락 없이 읽으면 포인터를 읽은 뒤 이름을 읽기 전에 칩이 사라질 수 있다.
 *
 * guard 안에서 return 하는 것에 주목: guard 매크로가 함수를 벗어날 때
 * 자동으로 락을 푸는 정리 속성을 쓰므로, 중간 return 이 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 인터럽트를 끈 채 스핀락을 잡는다.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/chip_name → [이 함수]
 */
static ssize_t chip_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 칩 포인터가 바뀌는 것을 막는다. irq 판이라 이 CPU 의 인터럽트도 끈다 */
	if (desc->irq_data.chip && desc->irq_data.chip->name)	/* [한국어] 칩이 붙어 있고 이름을 가졌는가. no_irq_chip 은 이름이 없다 */
		return sysfs_emit(buf, "%s\n", desc->irq_data.chip->name);	/* [한국어] 이름 한 줄 */
	return 0;	/* [한국어] 빈 파일을 돌려준다. 오류가 아니라 "해당 없음" 의 표현이다 */
}
IRQ_ATTR_RO(chip_name);	/* [한국어] 속성 구조체 생성 */

/*
 * [한국어]
 * hwirq_show - /sys/kernel/irq/N/hwirq 를 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수 (도메인이 없으면 0)
 *
 * 리눅스 인터럽트 번호(N)에 대응하는 하드웨어 번호를 보여 준다. 둘은
 * 전혀 다른 값이다. 리눅스 번호는 커널이 임의로 배정한 것이고,
 * 하드웨어 번호는 컨트롤러의 몇 번 입력인가를 뜻한다. 디바이스 트리에
 * 적힌 번호는 후자라, 이 파일이 둘을 잇는 다리다.
 *
 * 도메인이 없으면 빈 파일인 이유: 도메인을 쓰지 않는 옛 방식에서는
 * 두 번호가 같아서 따로 보여 줄 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/hwirq → [이 함수]
 */
static ssize_t hwirq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 도메인 포인터와 hwirq 를 한 시점의 값으로 함께 읽는다 */
	if (desc->irq_data.domain)	/* [한국어] 도메인 기반 인터럽트인가 */
		return sysfs_emit(buf, "%lu\n", desc->irq_data.hwirq);	/* [한국어] 컨트롤러 입장에서의 번호 */
	return 0;	/* [한국어] 도메인이 없으면 하드웨어 번호라는 개념이 따로 없다 */
}
IRQ_ATTR_RO(hwirq);	/* [한국어] 속성 구조체 생성 */

/*
 * [한국어]
 * type_show - /sys/kernel/irq/N/type 을 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수
 *
 * "level" 또는 "edge" 를 출력한다. 이 구분이 실제로 무엇을 바꾸는가:
 * 레벨 트리거는 신호가 계속 유지되므로 핸들러가 원인을 없앨 때까지
 * 마스크해 두어야 하고, 에지 트리거는 순간 신호라 래치를 ack 로
 * 지워야 한다. 흐름 처리기 자체가 달라진다.
 *
 * 두 가지밖에 없는 것처럼 보이지만, 실제로는 상승·하강·양쪽 에지와
 * 높음·낮음 레벨이 있다. 여기서는 그 세부를 접고 큰 갈래만 보여 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/type → [이 함수] → irqd_is_level_type()
 */
static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 트리거 방식은 실행 중에 바뀔 수 있어 락이 필요하다 */
	return sysfs_emit(buf, "%s\n", irqd_is_level_type(&desc->irq_data) ? "level" : "edge");	/* [한국어] IRQD_LEVEL 비트 하나로 두 갈래를 나눈다 */

}
IRQ_ATTR_RO(type);	/* [한국어] 속성 구조체 생성 */

/*
 * [한국어]
 * wakeup_show - /sys/kernel/irq/N/wakeup 을 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수
 *
 * 이 인터럽트가 시스템을 절전에서 깨울 수 있는지를 "enabled" 또는
 * "disabled" 로 보여 준다. 서스펜드가 안 되거나 원치 않게 깨어나는
 * 문제를 진단할 때 가장 먼저 보는 값이다.
 *
 * str_enabled_disabled() 를 쓰는 이유: 이런 불린 출력의 표현이 커널
 * 곳곳에서 제각각이었다("1/0", "yes/no", "on/off"). 공용 헬퍼로 통일해
 * 사용자 공간 도구가 예측할 수 있게 했다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/wakeup → [이 함수] → irqd_is_wakeup_set()
 */
static ssize_t wakeup_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] wakeup 설정은 enable_irq_wake 로 실행 중에 바뀐다 */
	return sysfs_emit(buf, "%s\n", str_enabled_disabled(irqd_is_wakeup_set(&desc->irq_data)));	/* [한국어] IRQD_WAKEUP_STATE 비트를 사람이 읽을 문자열로 */
}
IRQ_ATTR_RO(wakeup);	/* [한국어] 속성 구조체 생성 */

/*
 * [한국어]
 * name_show - /sys/kernel/irq/N/name 을 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수 (이름이 없으면 0)
 *
 * 서술자 자체의 이름이다. 아래 actions_show 가 보여 주는 핸들러 이름과
 * 다르다. 이쪽은 인터럽트 컨트롤러 계층이 붙인 이름 — 예를 들어
 * 체인 처리기가 자기 자식 인터럽트에 붙인 이름 — 이고, 저쪽은 드라이버가
 * request_irq 에 넘긴 이름이다. 대부분의 인터럽트는 이 이름이 없어
 * 빈 파일이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/name → [이 함수]
 */
static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 이름 포인터가 바뀌는 것을 막는다 */
	if (desc->name)	/* [한국어] 이름이 붙어 있는가. 대부분 NULL 이다 */
		return sysfs_emit(buf, "%s\n", desc->name);	/* [한국어] 이름 한 줄 */
	return 0;	/* [한국어] 빈 파일 */
}
IRQ_ATTR_RO(name);	/* [한국어] 속성 구조체 생성 */

/*
 * [한국어]
 * actions_show - /sys/kernel/irq/N/actions 를 출력한다
 *
 * @kobj: 서술자 안의 kobject
 * @attr: 쓰지 않는다
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수 (핸들러가 없으면 0)
 *
 * 이 인터럽트에 등록된 모든 핸들러의 이름을 쉼표로 이어 출력한다.
 * 공유 인터럽트에서는 여러 이름이 나온다 — 그 자체가 유용한 정보다.
 * 어떤 드라이버들이 한 선을 나눠 쓰는지 알 수 있다.
 *
 * scoped_guard 를 쓰는 이유가 여기 있다. 위 함수들처럼 guard 를 쓰면
 * 함수 끝까지 락을 쥐게 되는데, 마지막 sysfs_emit_at 은 락 밖에서
 * 해도 되고 그 편이 임계 구역을 짧게 만든다. 특히 핸들러가 많으면
 * 루프가 길어지므로 개행 하나까지 락 안에 둘 이유가 없다.
 *
 * ret 이 0 인지 확인하고 개행을 붙이는 것도 의미가 있다. 핸들러가
 * 하나도 없으면 개행만 있는 파일이 아니라 완전히 빈 파일이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /sys/kernel/irq/N/actions → [이 함수] → for_each_action_of_desc()
 */
static ssize_t actions_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] 서술자 복원 */
	struct irqaction *action;	/* [한국어] 핸들러 목록 순회용 */
	ssize_t ret = 0;	/* [한국어] 지금까지 쓴 바이트 수 */
	char *p = "";	/* [한국어] 첫 항목 앞에는 구분자 없이 */

	scoped_guard(raw_spinlock_irq, &desc->lock) {	/* [한국어] 목록을 훑는 동안만 락을 잡는다. 아래 개행 출력은 락 밖에서 해도 된다 */
		for_each_action_of_desc(desc, action) {	/* [한국어] 공유 인터럽트면 여러 개가 연결돼 있다 */
			ret += sysfs_emit_at(buf, ret, "%s%s", p, action->name);	/* [한국어] 드라이버가 request_irq 에 넘긴 이름 */
			p = ",";	/* [한국어] 다음부터 쉼표 */
		}
	}

	if (ret)	/* [한국어] 하나라도 썼는가 */
		ret += sysfs_emit_at(buf, ret, "\n");	/* [한국어] 그때만 개행을 붙인다. 핸들러가 없으면 완전히 빈 파일이 되게 하려는 것이다 */
	return ret;	/* [한국어] 총 바이트 수 */
}
IRQ_ATTR_RO(actions);	/* [한국어] 속성 구조체 생성 */

static struct attribute *irq_attrs[] = {
	/* [한국어] 서술자마다 만들어질 sysfs 파일들의 목록.
	 * 위에서 IRQ_ATTR_RO 로 만든 일곱 개 속성을 모아, 아래
	 * ATTRIBUTE_GROUPS 가 이것을 그룹으로 감싼다. 순서는 파일 생성
	 * 순서일 뿐 의미가 없다. */
	&per_cpu_count_attr.attr,
	/* [한국어] CPU 별 발생 횟수 파일.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: sysfs 코어가 디렉터리를 만들 때 순회한다.
	 * 값 범위: 위 IRQ_ATTR_RO(per_cpu_count) 가 만든 구조체의 주소.
	 * 동기화: 배열 자체는 초기화 후 바뀌지 않는다. */
	&chip_name_attr.attr,
	/* [한국어] 담당 컨트롤러 이름 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: chip_name_attr 의 주소.
	 * 동기화: 위와 같다. */
	&hwirq_attr.attr,
	/* [한국어] 하드웨어 인터럽트 번호 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: hwirq_attr 의 주소.
	 * 동기화: 위와 같다. */
	&type_attr.attr,
	/* [한국어] 레벨/에지 구분 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: type_attr 의 주소.
	 * 동기화: 위와 같다. */
	&wakeup_attr.attr,
	/* [한국어] 절전 해제 가능 여부 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: wakeup_attr 의 주소.
	 * 동기화: 위와 같다. */
	&name_attr.attr,
	/* [한국어] 서술자 이름 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: name_attr 의 주소.
	 * 동기화: 위와 같다. */
	&actions_attr.attr,
	/* [한국어] 등록된 핸들러 이름 목록 파일.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: actions_attr 의 주소.
	 * 동기화: 위와 같다. */
	NULL
	/* [한국어] 배열의 끝 표식.
	 * 설정자: 정적 초기화.
	 * 읽는 자: sysfs 코어의 순회 루프가 이것을 보고 멈춘다.
	 * 값 범위: 항상 NULL. 개수를 따로 넘기지 않는 관례라 필수다.
	 * 동기화: 불필요. */
};
ATTRIBUTE_GROUPS(irq);	/* [한국어] 위 배열을 감싸 irq_groups 라는 그룹 배열을 만든다. sysfs 는 개별 속성이 아니라 그룹 단위로 등록받는다 */

static const struct kobj_type irq_kobj_type = {
	/* [한국어] 서술자 kobject 의 동작을 정의하는 타입 (sysfs 있는 판).
	 * init_desc() 가 kobject_init() 에 이것을 넘기면, 그 kobject 는
	 * 아래 세 가지 성질을 갖게 된다. */
	.release	= irq_kobj_release,
	/* [한국어] 참조 카운트가 0 이 될 때 불릴 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: kobject_put() 이 마지막 참조를 놓을 때 코어가 부른다.
	 * 값 범위: 이 파일의 irq_kobj_release — 마스크와 통계를 풀고
	 *   서술자 자체를 kfree 한다.
	 * 동기화: const 라 변경되지 않는다. 이 콜백이 서술자 해제의
	 *   진짜 마지막 지점이라는 점이 중요하다. */
	.sysfs_ops	= &kobj_sysfs_ops,
	/* [한국어] 속성 파일을 읽고 쓸 때 쓸 연산표.
	 * 설정자: 정적 초기화.
	 * 읽는 자: sysfs 가 파일 접근을 이 표로 넘긴다.
	 * 값 범위: 커널 공용 kobj_sysfs_ops — kobj_attribute 의
	 *   show/store 를 그대로 부르는 표준 구현이다.
	 * 동기화: 위와 같다. */
	.default_groups = irq_groups,
	/* [한국어] 디렉터리를 만들 때 자동으로 생성할 속성 그룹.
	 * 설정자: 정적 초기화 (위 ATTRIBUTE_GROUPS 가 만든 배열).
	 * 읽는 자: kobject_add() 가 이 그룹의 파일들을 만든다.
	 * 값 범위: irq_groups — 위 일곱 속성을 담은 그룹 하나.
	 * 동기화: 위와 같다. */
};

/*
 * [한국어]
 * irq_sysfs_add - 서술자 하나의 /sys/kernel/irq/N/ 디렉터리를 만든다
 *
 * @irq:  인터럽트 번호 (디렉터리 이름이 된다)
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 두 시점에서 불린다. 하나는 새 서술자를 만들 때(alloc_descs), 다른
 * 하나는 부팅 후 sysfs 가 준비됐을 때 이미 있는 것들을 한꺼번에
 * 등록할 때(irq_sysfs_init)다. irq_kobj_base 가 NULL 인지 검사하는
 * 것이 그 두 시점을 가르는 장치다 — 아직 디렉터리가 없으면 조용히
 * 건너뛰고, 나중에 초기화 함수가 몰아서 처리한다.
 *
 * 실패해도 계속 진행하는 이유는 원본 주석에 있다. sysfs 항목은 진단용
 * 편의이지 동작에 필요한 것이 아니고, 늦은 initcall 에서 일어난
 * 실패는 되돌릴 방법도 마땅치 않다. 경고만 남기고 인터럽트는 정상
 * 동작하게 둔다.
 *
 * IRQS_SYSFS 비트를 세우는 것이 중요하다. 아래 irq_sysfs_del() 이
 * "kobject_add 가 성공했던 것만" 지우기 위해 이 비트를 본다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   alloc_descs() / irq_sysfs_init() → [이 함수] → kobject_add()
 */
static void irq_sysfs_add(int irq, struct irq_desc *desc)
{
	if (irq_kobj_base) {	/* [한국어] /sys/kernel/irq 디렉터리가 이미 만들어졌는가. 부팅 초기에는 아직 없다 */
		/*
		 * Continue even in case of failure as this is nothing
		 * crucial and failures in the late irq_sysfs_init()
		 * cannot be rolled back.
		 */
		if (kobject_add(&desc->kobj, irq_kobj_base, "%d", irq))	/* [한국어] (위 영어 주석) 번호를 이름으로 하위 디렉터리를 만들고 속성 파일들을 생성한다 */
			pr_warn("Failed to add kobject for irq %d\n", irq);	/* [한국어] 실패해도 인터럽트는 정상 동작한다. 진단용 파일이 없을 뿐이다 */
		else	/* [한국어] 성공한 경우 */
			desc->istate |= IRQS_SYSFS;	/* [한국어] 아래 del 이 "성공했던 것만" 지우도록 표시해 둔다. 이 비트가 없으면 만들어진 적 없는 kobject 에 kobject_del 을 불러 경고가 난다 */
	}
}

/*
 * [한국어]
 * irq_sysfs_del - 서술자의 sysfs 디렉터리를 지운다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 add 의 반대인데, 대칭이 완전하지 않다. add 는 실패할 수 있고
 * 아예 건너뛸 수도 있으므로, 지울 때는 실제로 만들어졌는지 확인해야
 * 한다. IRQS_SYSFS 비트가 그 기록이다.
 *
 * 원본 주석이 두 경우를 짚는다. 부팅 초기라 sysfs 가 없었던 경우와,
 * kobject_add 가 실패한 경우다. 둘 다 이 비트가 꺼져 있어 걸러진다.
 *
 * kobject_del 과 kobject_put 의 차이: 여기서는 del 만 한다 —
 * 디렉터리를 파일 시스템에서 없앨 뿐 kobject 참조는 유지된다. 참조를
 * 놓는 것은 나중에 RCU 유예 뒤 delayed_free_desc() 가 한다. 이 분리가
 * "사용자 공간에서는 즉시 사라지지만 커널 안에서는 조금 더 살아 있는"
 * 상태를 만든다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   free_desc() → [이 함수] → kobject_del()
 */
static void irq_sysfs_del(struct irq_desc *desc)
{
	/*
	 * Only invoke kobject_del() when kobject_add() was successfully
	 * invoked for the descriptor. This covers both early boot, where
	 * sysfs is not initialized yet, and the case of a failed
	 * kobject_add() invocation.
	 */
	if (desc->istate & IRQS_SYSFS)	/* [한국어] (위 영어 주석) add 가 실제로 성공했던 서술자인가 */
		kobject_del(&desc->kobj);	/* [한국어] 디렉터리만 없앤다. 참조를 놓는 것은 RCU 유예 뒤 delayed_free_desc 가 한다 */
}

/*
 * [한국어]
 * irq_sysfs_init - /sys/kernel/irq 디렉터리를 만들고 기존 항목을 등록한다
 *
 * @return: 0 성공, -ENOMEM 디렉터리 생성 실패
 *
 * 닭과 달걀 문제를 푸는 함수다. 부팅 초기 인터럽트들은 sysfs 가
 * 준비되기 전에 서술자를 얻는다. 그때는 항목을 만들 수 없으므로,
 * sysfs 가 준비된 뒤 이 함수가 뒤늦게 몰아서 등록한다.
 *
 * 뮤텍스를 잡는 이유: 이 함수가 도는 동안 다른 CPU 가 새 인터럽트를
 * 할당할 수 있다. 락이 없으면 그 인터럽트가 두 번 등록되거나(순회
 * 중에 추가돼 새 경로와 이 루프가 모두 처리) 아예 빠질 수 있다. 락을
 * 잡으면 새 할당은 이 함수가 끝난 뒤 진행되고, 그때는 irq_kobj_base
 * 가 이미 설정돼 있어 정상 경로로 등록된다.
 *
 * postcore_initcall 인 이유: kernel_kobj (즉 /sys/kernel)가 만들어진
 * 뒤여야 하고, 대부분의 드라이버가 인터럽트를 요청하기 전이면 좋다.
 * postcore 가 그 사이에 있다.
 *
 * 실행 컨텍스트: 부팅 중 initcall, 프로세스 문맥.
 *
 * 호출 체인:
 *   do_initcalls() → [이 함수] → kobject_create_and_add() / irq_sysfs_add()
 */
static int __init irq_sysfs_init(void)
{
	struct irq_desc *desc;	/* [한국어] 순회용 */
	int irq;	/* [한국어] 순회용 번호 */

	/* Prevent concurrent irq alloc/free */
	guard(mutex)(&sparse_irq_lock);	/* [한국어] (위 영어 주석) 이 함수가 도는 동안 새 인터럽트가 생기거나 사라지지 않게 한다 */
	irq_kobj_base = kobject_create_and_add("irq", kernel_kobj);	/* [한국어] /sys/kernel/irq 디렉터리. 이 대입 뒤로는 새 서술자가 정상 경로로 등록된다 */
	if (!irq_kobj_base)	/* [한국어] 만들지 못했는가 */
		return -ENOMEM;	/* [한국어] sysfs 항목 없이 동작한다. 인터럽트 자체는 문제없다 */

	/* Add the already allocated interrupts */
	for_each_irq_desc(irq, desc)	/* [한국어] (위 영어 주석) 디렉터리가 없던 시절에 만들어진 것들을 훑는다 */
		irq_sysfs_add(irq, desc);	/* [한국어] 이제 irq_kobj_base 가 있으므로 실제로 등록된다 */
	return 0;	/* [한국어] initcall 성공 */
}
postcore_initcall(irq_sysfs_init);	/* [한국어] /sys/kernel 이 준비된 뒤, 드라이버들이 인터럽트를 요청하기 전 단계에 실행한다 */

#else /* !CONFIG_SYSFS */	/* [한국어] sysfs 를 뺀 빌드 — 임베디드 등 */

static const struct kobj_type irq_kobj_type = {
	/* [한국어] sysfs 없는 빌드의 kobject 타입. 속성도 sysfs_ops 도
	 * 없고 해제 콜백만 있다. kobject 자체를 없애지 않는 이유는,
	 * 서술자 해제가 그 참조 카운트 위에 얹혀 있기 때문이다 — RCU
	 * 유예 뒤 kobject_put 이 release 를 부르는 구조를 그대로 쓴다. */
	.release	= irq_kobj_release,
	/* [한국어] 참조가 0 이 될 때 불릴 해제 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: kobject_put().
	 * 값 범위: sysfs 판과 같은 함수 — 해제 경로는 빌드와 무관하다.
	 * 동기화: const 라 변경되지 않는다. */
};

/*
 * [한국어]
 * irq_sysfs_add - sysfs 항목 등록 (sysfs 없는 빌드, 빈 함수)
 *
 * @irq:  무시
 * @desc: 무시
 * @return: 없음
 *
 * 만들 파일 시스템이 없다. 호출자인 alloc_descs() 를 #ifdef 로
 * 나누지 않으려고 빈 함수를 둔다.
 *
 * 호출 체인:
 *   alloc_descs() → [이 함수]
 */
static void irq_sysfs_add(int irq, struct irq_desc *desc) {}	/* [한국어] 만들 것이 없다 */
/*
 * [한국어]
 * irq_sysfs_del - sysfs 항목 제거 (sysfs 없는 빌드, 빈 함수)
 *
 * @desc: 무시
 * @return: 없음
 *
 * add 가 아무것도 만들지 않았으므로 지울 것도 없다.
 *
 * 호출 체인:
 *   free_desc() → [이 함수]
 */
static void irq_sysfs_del(struct irq_desc *desc) {}	/* [한국어] 지울 것이 없다 */

#endif /* CONFIG_SYSFS */	/* [한국어] sysfs 분기의 끝 */

/*
 * [한국어]
 * irq_to_desc - 인터럽트 번호로 서술자를 찾는다 (희소 판)
 *
 * @irq: 리눅스 인터럽트 번호
 * @return: 서술자 포인터, 없으면 NULL
 *
 * 이 서브시스템에서 가장 많이 불리는 함수다. 인터럽트가 발생할 때마다,
 * 그리고 인터럽트 관련 API 를 부를 때마다 여기를 거친다.
 *
 * 락을 잡지 않는 이유: 메이플 트리가 MT_FLAGS_USE_RCU 로 만들어져
 * 조회가 락 없이 안전하다. 인터럽트 문맥에서 뮤텍스를 잡을 수 없으므로
 * 이것이 필수 조건이다. 다만 "안전하다" 는 것은 트리 구조를 따라가는
 * 동안 크래시가 나지 않는다는 뜻이지, 반환된 서술자가 계속 유효하다는
 * 뜻은 아니다. 호출자는 RCU 읽기 구역 안에 있거나(인터럽트 문맥은
 * 자동으로 그렇다) 다른 방법으로 수명을 보장해야 한다.
 *
 * 실행 컨텍스트: 제약 없음 — 인터럽트 문맥 포함.
 *
 * 호출 체인:
 *   generic_handle_irq() / __irq_get_desc_lock() / 그 외 수많은 곳 →
 *   [이 함수] → mtree_load()
 */
struct irq_desc *irq_to_desc(unsigned int irq)
{
	return mtree_load(&sparse_irqs, irq);	/* [한국어] 트리 조회 한 번. 없는 번호면 NULL 이 나온다 */
}
#ifdef CONFIG_KVM_BOOK3S_64_HV_MODULE	/* [한국어] PowerPC KVM 이 모듈로 빌드되는 경우에만 */
EXPORT_SYMBOL_GPL(irq_to_desc);	/* [한국어] 그 모듈이 게스트 인터럽트를 다루느라 서술자를 직접 봐야 한다. 다른 드라이버에는 일부러 열지 않는다 — 서술자 내부를 만지는 것은 코어의 몫이다 */
#endif

/*
 * [한국어]
 * irq_lock_sparse - 서술자 할당·해제를 막는다 (외부 공개)
 *
 * @return: 없음
 *
 * 왜 락을 밖으로 내보내는가: /proc/interrupts 를 만드는 코드
 * (fs/proc/interrupts.c)가 서술자 목록을 훑는 동안 그것이 바뀌면
 * 안 된다. 그 코드가 sparse_irq_lock 을 직접 만질 수는 없으므로
 * (static 이다) 이 두 함수로 감싸 내보낸다.
 *
 * 뮤텍스이므로 잠들 수 있다 — 인터럽트 문맥에서 부르면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   fs/proc/interrupts.c 의 seq_file 시작 → [이 함수]
 */
void irq_lock_sparse(void)
{
	mutex_lock(&sparse_irq_lock);	/* [한국어] 이 뒤로 서술자 할당·해제가 막힌다 */
}

/*
 * [한국어]
 * irq_unlock_sparse - irq_lock_sparse() 로 잡은 락을 푼다
 *
 * @return: 없음
 *
 * 위 함수의 짝이다. 반드시 같은 스레드가 불러야 한다 — 뮤텍스는
 * 소유자 개념이 있어 다른 스레드가 풀면 lockdep 이 경고한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   fs/proc/interrupts.c 의 seq_file 종료 → [이 함수]
 */
void irq_unlock_sparse(void)
{
	mutex_unlock(&sparse_irq_lock);	/* [한국어] 다시 할당·해제가 가능해진다 */
}

/*
 * [한국어]
 * alloc_desc - 서술자 하나를 할당하고 초기화한다 (희소 판)
 *
 * @irq:      인터럽트 번호
 * @node:     할당할 NUMA 노드
 * @flags:    초기 IRQD_ 상태 플래그
 * @affinity: 초기 친화도
 * @owner:    소유 모듈
 * @return:   서술자 포인터, 실패 시 NULL
 *
 * 메모리를 잡고 init_desc() 에 넘기는 얇은 함수다. 아직 트리에 넣지
 * 않으므로, 반환된 서술자는 호출자만 아는 상태다. 트리에 넣는 것은
 * 호출자인 alloc_descs() 가 별도로 한다 — 그래야 초기화가 끝난 뒤에만
 * 다른 CPU 에 보인다.
 *
 * kzalloc_node 로 노드를 지정하는 이유: 인터럽트가 주로 처리될 CPU 와
 * 같은 노드에 서술자를 두면 인터럽트 경로의 캐시 미스가 줄어든다.
 * 인터럽트 처리는 지연에 민감해서 이 정도 차이도 의미가 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   alloc_descs() / early_irq_init() → [이 함수] → kzalloc_node() / init_desc()
 */
static struct irq_desc *alloc_desc(int irq, int node, unsigned int flags,
				   const struct cpumask *affinity,
				   struct module *owner)
{
	struct irq_desc *desc;	/* [한국어] 할당 결과 */
	int ret;	/* [한국어] 초기화 결과 */

	desc = kzalloc_node(sizeof(*desc), GFP_KERNEL, node);	/* [한국어] 인터럽트가 처리될 노드에 둔다. 0 초기화라 init_desc 가 안 건드리는 필드는 전부 0 이다 */
	if (!desc)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 호출자가 NULL 검사로 처리한다 */

	ret = init_desc(desc, irq, node, flags, affinity, owner);	/* [한국어] 락·마스크·통계 등 실제 초기화 */
	if (unlikely(ret)) {	/* [한국어] 초기화 실패 — 메모리 부족뿐이다. unlikely 로 정상 경로를 빠르게 유지한다 */
		kfree(desc);	/* [한국어] 방금 잡은 것을 되돌린다. init_desc 가 자기가 잡은 것은 이미 풀었다 */
		return NULL;	/* [한국어] 초기화 실패. 방금 kfree 했으므로 호출자에게 넘길 것이 없다 */
	}

	return desc;	/* [한국어] 초기화 완료. 아직 트리에는 없으므로 호출자만 안다 */
}

/*
 * [한국어]
 * irq_kobj_release - kobject 참조가 0 이 될 때 서술자를 최종 해제한다
 *
 * @kobj: 서술자 안에 박힌 kobject
 * @return: 없음
 *
 * 서술자 해제 사슬의 마지막 고리다. 전체 사슬은 이렇다.
 *
 *   free_desc()           트리에서 빼고 call_rcu 예약
 *   → (RCU 유예 기간)      락 없이 조회 중이던 쪽이 모두 빠져나감
 *   → delayed_free_desc()  kobject_put 으로 참조를 놓음
 *   → [이 함수]            참조가 0 이면 실제 메모리 해제
 *
 * 왜 이렇게 긴가: 두 가지 다른 참조를 모두 기다려야 하기 때문이다.
 * RCU 는 락 없이 트리를 조회하던 쪽을, kobject 참조는 sysfs 파일을
 * 열어 둔 사용자 공간을 각각 기다린다. 사용자가 파일을 열어 둔 채로
 * 인터럽트가 해제되면 이 함수가 파일이 닫힐 때까지 미뤄진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 마지막 kobject_put 을 부른 쪽의
 * 문맥에서 동기적으로 실행된다.
 *
 * 호출 체인:
 *   kobject_put() (마지막 참조) → kobj_type.release → [이 함수]
 */
static void irq_kobj_release(struct kobject *kobj)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);	/* [한국어] kobject 주소에서 서술자를 복원한다 */

	free_masks(desc);	/* [한국어] CPU 마스크들 */
	free_percpu(desc->kstat_irqs);	/* [한국어] CPU 별 통계 영역 */
	kfree(desc);	/* [한국어] 서술자 본체. 이 줄 이후로 이 메모리를 가리키는 유효한 참조는 하나도 없어야 한다 */
}

/*
 * [한국어]
 * delayed_free_desc - RCU 유예가 끝난 뒤 kobject 참조를 놓는다
 *
 * @rhp: 서술자 안에 박힌 rcu_head
 * @return: 없음
 *
 * free_desc() 가 call_rcu 로 예약한 콜백이다. 이 함수가 불린다는 것은
 * "이 서술자를 락 없이 조회하던 쪽이 모두 빠져나갔다" 는 뜻이다.
 * 이제 kobject 참조만 남았으므로 그것을 놓는다.
 *
 * 참조가 0 이 되면 위 irq_kobj_release 가 곧바로 불려 메모리가
 * 해제된다. 사용자 공간이 sysfs 파일을 열어 두었다면 그때까지 미뤄진다.
 *
 * 실행 컨텍스트: RCU 콜백 — 소프트IRQ 문맥이거나 RCU 커널 스레드다.
 * 잠들 수 없으므로 무거운 일을 하면 안 된다. kobject_put 은 가볍다.
 *
 * 호출 체인:
 *   free_desc() → call_rcu() → (유예 기간) → [이 함수] → kobject_put()
 */
static void delayed_free_desc(struct rcu_head *rhp)
{
	struct irq_desc *desc = container_of(rhp, struct irq_desc, rcu);	/* [한국어] rcu_head 주소에서 서술자를 복원한다 */

	kobject_put(&desc->kobj);	/* [한국어] 참조를 하나 놓는다. 0 이 되면 irq_kobj_release 가 불려 실제로 해제된다 */
}

/*
 * [한국어]
 * free_desc - 서술자를 없앤다 (희소 판)
 *
 * @irq: 없앨 인터럽트 번호
 * @return: 없음
 *
 * 해제의 순서가 이 함수의 전부다. 잘못된 순서는 곧 use-after-free 다.
 *
 *   1. debugfs, procfs 항목 제거 — 이 두 함수는 진행 중인 접근이
 *      끝날 때까지 기다린다. 그래서 가장 먼저 해야 한다.
 *   2. sysfs 디렉터리 제거 — 마찬가지로 사용자 공간의 새 접근을 막는다.
 *   3. 트리에서 제거 — 이 뒤로 irq_to_desc 는 NULL 을 돌려준다.
 *   4. RCU 유예 뒤 해제 — 이미 포인터를 들고 있던 쪽을 기다린다.
 *
 * 원본 주석이 짚는 두 가지: sparse_irq_lock 이 /proc 순회도 함께
 * 지키므로 트리에서 빼고 나면 그쪽 조회가 실패하게 된다는 것, 그리고
 * RCU 해제가 디먹스 인터럽트의 자식 관리와 kstat_irqs_usr() 를
 * 가능하게 한다는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   irq_free_descs() / alloc_descs() 의 에러 경로 → [이 함수] →
 *   unregister_irq_proc() / delete_irq_desc() / call_rcu()
 */
static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 없앨 서술자. 호출자가 유효성을 이미 확인했다 */

	irq_remove_debugfs_entry(desc);	/* [한국어] debugfs 항목. 진행 중인 읽기가 끝날 때까지 기다린다 */
	unregister_irq_proc(irq, desc);	/* [한국어] /proc/irq/N/ 항목. 마찬가지로 기다린다 */

	/*
	 * sparse_irq_lock protects also show_interrupts() and
	 * kstat_irq_usr(). Once we deleted the descriptor from the
	 * sparse tree we can free it. Access in proc will fail to
	 * lookup the descriptor.
	 *
	 * The sysfs entry must be serialized against a concurrent
	 * irq_sysfs_init() as well.
	 */
	irq_sysfs_del(desc);	/* [한국어] (위 영어 주석) sysfs 디렉터리. irq_sysfs_init 과의 경쟁은 sparse_irq_lock 이 막는다 */
	delete_irq_desc(irq);	/* [한국어] 트리에서 뺀다. 이 줄 이후로 새 조회는 실패한다 */

	/*
	 * We free the descriptor, masks and stat fields via RCU. That
	 * allows demultiplex interrupts to do rcu based management of
	 * the child interrupts.
	 * This also allows us to use rcu in kstat_irqs_usr().
	 */
	call_rcu(&desc->rcu, delayed_free_desc);	/* [한국어] (위 영어 주석) 이미 포인터를 들고 있던 쪽이 빠져나갈 때까지 기다린 뒤 해제한다. 여기서 바로 kfree 하면 락 없이 조회 중이던 쪽이 해제된 메모리를 읽는다 */
}

/*
 * [한국어]
 * alloc_descs - 연속된 서술자 여러 개를 만들어 트리에 등록한다
 *
 * @start:    시작 번호 (이미 비어 있음이 확인된 구간)
 * @cnt:      개수
 * @node:     기본 NUMA 노드
 * @affinity: 인터럽트별 친화도 배열 (cnt 개) 또는 NULL
 * @owner:    소유 모듈
 * @return:   성공 시 @start, 실패 시 -EINVAL 또는 -ENOMEM
 *
 * MSI-X 처럼 벡터를 여러 개 쓰는 장치를 위해 구간 단위로 만든다.
 *
 * affinity 배열이 주어지면 각 인터럽트가 서로 다른 CPU 를 향하게
 * 된다 — 다중 큐 네트워크 카드나 NVMe 가 큐마다 다른 CPU 에 인터럽트를
 * 보내는 구조다. 이때 node 를 그 CPU 의 노드로 덮어쓰는 것에 주목:
 * 서술자를 그 인터럽트가 실제로 처리될 노드에 두려는 것이다.
 *
 * is_managed 가 뜻하는 것: 커널이 친화도를 자동 관리하는 인터럽트다.
 * 사용자가 /proc 으로 바꿀 수 없고, CPU 가 오프라인이 되면 인터럽트도
 * 함께 잠시 꺼진다(IRQD_MANAGED_SHUTDOWN). 다중 큐 장치에서 큐와
 * CPU 를 1:1 로 묶어 두려는 설계다.
 *
 * 검증을 먼저 다 하는 이유: 중간에 빈 마스크를 발견하면 이미 만든
 * 것들을 되돌려야 한다. 미리 전부 검사하면 그 경로가 아예 없어진다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   __irq_alloc_descs() → [이 함수] → alloc_desc() / irq_insert_desc() /
 *   irq_sysfs_add() / irq_add_debugfs_entry()
 */
static int alloc_descs(unsigned int start, unsigned int cnt, int node,
		       const struct irq_affinity_desc *affinity,
		       struct module *owner)
{
	struct irq_desc *desc;	/* [한국어] 만든 서술자 */
	int i;	/* [한국어] 순회 인덱스. 에러 경로의 되돌릴 범위로도 쓰인다 */

	/* Validate affinity mask(s) */
	if (affinity) {	/* [한국어] (위 영어 주석) 호출자가 친화도를 지정했는가 */
		for (i = 0; i < cnt; i++) {	/* [한국어] 하나라도 잘못됐으면 아무것도 만들지 않는다 */
			if (cpumask_empty(&affinity[i].mask))	/* [한국어] 빈 마스크는 갈 CPU 가 없다는 뜻이라 무의미하다 */
				return -EINVAL;	/* [한국어] 아직 아무것도 만들지 않았으므로 되돌릴 것이 없다 — 미리 검사하는 이유다 */
		}
	}

	for (i = 0; i < cnt; i++) {	/* [한국어] 서술자를 하나씩 만든다 */
		const struct cpumask *mask = NULL;	/* [한국어] 이 인터럽트의 친화도. 기본은 지정 없음 */
		unsigned int flags = 0;	/* [한국어] 이 인터럽트의 초기 IRQD_ 상태 */

		if (affinity) {	/* [한국어] 친화도가 지정된 경우 */
			if (affinity->is_managed) {	/* [한국어] 커널이 자동 관리하는 인터럽트인가 */
				flags = IRQD_AFFINITY_MANAGED |	/* [한국어] 사용자가 /proc 으로 바꿀 수 없게 한다 */
					IRQD_MANAGED_SHUTDOWN;	/* [한국어] 대상 CPU 가 없으면 인터럽트를 잠시 끈 상태로 시작한다 */
			}
			flags |= IRQD_AFFINITY_SET;	/* [한국어] 기본값이 아니라 명시적으로 정해진 친화도임을 표시한다. 이후 자동 재분배가 이 값을 존중한다 */
			mask = &affinity->mask;	/* [한국어] 이 인터럽트가 향할 CPU 들 */
			node = cpu_to_node(cpumask_first(mask));	/* [한국어] 서술자를 그 CPU 의 노드에 둔다. 인터럽트 처리 경로의 캐시 미스를 줄인다 */
			affinity++;	/* [한국어] 배열의 다음 항목으로. 인터럽트마다 다른 친화도를 갖는다 */
		}

		desc = alloc_desc(start + i, node, flags, mask, owner);	/* [한국어] 실제 할당과 초기화 */
		if (!desc)	/* [한국어] 메모리 부족 */
			goto err;	/* [한국어] 이미 만든 것들을 되돌려야 한다 */
		irq_insert_desc(start + i, desc);	/* [한국어] 트리에 넣는다. 이 줄부터 다른 CPU 가 이 서술자를 볼 수 있다 */
		irq_sysfs_add(start + i, desc);	/* [한국어] /sys/kernel/irq/N/ 항목. 부팅 초기라면 조용히 건너뛴다 */
		irq_add_debugfs_entry(start + i, desc);	/* [한국어] debugfs 항목. 진단용이라 실패해도 무시한다 */
	}
	return start;	/* [한국어] 성공 — 구간의 첫 번호를 돌려준다. 호출자가 이것을 인터럽트 번호로 쓴다 */

err:	/* [한국어] 중간에 실패했을 때 */
	for (i--; i >= 0; i--)	/* [한국어] 실패한 i 번은 만들어지지 않았으므로 그 앞부터 역순으로 */
		free_desc(start + i);	/* [한국어] 트리에서 빼고 RCU 유예 뒤 해제한다 */
	return -ENOMEM;	/* [한국어] 유일한 실패 원인이 메모리 부족이다 */
}

/*
 * [한국어]
 * irq_expand_nr_irqs - 지원 인터럽트 상한을 늘린다 (희소 판)
 *
 * @nr: 새 상한
 * @return: true 늘렸음, false 절대 한계를 넘어 늘릴 수 없음
 *
 * 희소 배치의 장점이 여기서 드러난다. 번호 공간을 늘려도 그만큼
 * 메모리를 쓰지 않는다 — 실제 서술자가 만들어진 번호만 트리에
 * 자리를 차지하기 때문이다. 그래서 요청이 오면 대체로 그냥 늘려 준다.
 *
 * MAX_SPARSE_IRQS 라는 절대 한계는 있다. 번호 자체가 무한할 수는
 * 없고, 트리 인덱스의 범위와 아키텍처의 가정이 걸려 있다.
 *
 * 실행 컨텍스트: __irq_alloc_descs() 안, sparse_irq_lock 보유.
 * 락이 있어야 nr_irqs 갱신이 다른 할당과 엇갈리지 않는다.
 *
 * 호출 체인:
 *   __irq_alloc_descs() → [이 함수]
 */
static bool irq_expand_nr_irqs(unsigned int nr)
{
	if (nr > MAX_SPARSE_IRQS)	/* [한국어] 절대 한계를 넘는가 */
		return false;	/* [한국어] 호출자가 -ENOMEM 으로 바꿔 올린다 */
	nr_irqs = nr;	/* [한국어] 상한을 늘린다. 희소 배치라 메모리를 미리 잡지 않는다 */
	return true;	/* [한국어] 이제 이 번호까지 서술자를 만들 수 있다 */
}

/*
 * [한국어]
 * early_irq_init - 부팅 초기 인터럽트 서술자 기반을 세운다 (희소 판)
 *
 * @return: arch_early_irq_init() 의 반환값 (대개 0)
 *
 * 인터럽트 서브시스템이 처음으로 살아나는 지점이다. 이 함수가 끝나면
 * 서술자를 만들고 찾을 수 있게 된다.
 *
 * arch_probe_nr_irqs() 가 두 가지를 한다. 아키텍처가 아는 인터럽트
 * 수로 nr_irqs 를 조정하고, "미리 만들어 두어야 하는 개수" 를 돌려준다.
 * 후자가 필요한 이유: 레거시 인터럽트(x86 의 ISA 0~15 번 같은)는
 * 도메인 매핑을 거치지 않고 번호가 고정이라, 부팅 초기부터 서술자가
 * 있어야 한다.
 *
 * 세 번의 상한 검사가 각각 다른 잘못을 잡는다. 아키텍처가 nr_irqs 를
 * 너무 크게 잡았을 때, 미리 만들 개수를 너무 크게 잡았을 때, 그리고
 * 미리 만들 개수가 상한보다 클 때다. 마지막 경우는 상한을 올려 맞춘다.
 *
 * alloc_desc 의 실패를 검사하지 않는 것에 주목: 부팅 극초기에 이만한
 * 할당이 실패하면 시스템이 살아날 가망이 없다. 뒤이은 irq_insert_desc
 * 가 NULL 을 넣게 되지만, 그 상황은 어차피 곧 패닉이다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드. 그래서 sparse_irq_lock 을
 * 잡지 않는다.
 *
 * 호출 체인:
 *   start_kernel() → early_irq_init() → [이 함수] →
 *   init_irq_default_affinity() / arch_probe_nr_irqs() / alloc_desc()
 */
int __init early_irq_init(void)
{
	int i, initcnt, node = first_online_node;	/* [한국어] 미리 만들 개수와, 그것들을 둘 노드 */
	struct irq_desc *desc;	/* [한국어] 만든 서술자 */

	init_irq_default_affinity();	/* [한국어] 기본 친화도 마스크를 준비한다. 아래에서 만드는 서술자들이 이 값을 쓴다 */

	/* Let arch update nr_irqs and return the nr of preallocated irqs */
	initcnt = arch_probe_nr_irqs();	/* [한국어] (위 영어 주석) 아키텍처가 nr_irqs 를 조정하고 미리 만들 개수를 돌려준다 */
	printk(KERN_INFO "NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d\n",	/* [한국어] 부팅 로그에 남긴다. 인터럽트 문제 진단의 출발점이 되는 줄이다 */
	       NR_IRQS, nr_irqs, initcnt);

	if (WARN_ON(nr_irqs > MAX_SPARSE_IRQS))	/* [한국어] 아키텍처가 상한을 넘겨 잡았는가 — 버그다 */
		nr_irqs = MAX_SPARSE_IRQS;	/* [한국어] 잘라서 계속 진행한다. 부팅을 멈추는 것보다 낫다 */

	if (WARN_ON(initcnt > MAX_SPARSE_IRQS))	/* [한국어] 미리 만들 개수가 상한을 넘는가 */
		initcnt = MAX_SPARSE_IRQS;	/* [한국어] 마찬가지로 자른다 */

	if (initcnt > nr_irqs)	/* [한국어] 미리 만들 개수가 상한보다 큰가 — 모순이다 */
		nr_irqs = initcnt;	/* [한국어] 상한을 올려 맞춘다. 이쪽은 경고 없이 조용히 조정한다 */

	for (i = 0; i < initcnt; i++) {	/* [한국어] 레거시 인터럽트처럼 번호가 고정인 것들을 미리 만든다 */
		desc = alloc_desc(i, node, 0, NULL, NULL);	/* [한국어] 기본 친화도, 플래그 없음, 소유 모듈 없음 */
		irq_insert_desc(i, desc);	/* [한국어] 트리에 넣는다. 실패 검사가 없는데, 이 시점의 할당 실패는 곧 패닉이라 되돌릴 의미가 없다 */
	}
	return arch_early_irq_init();	/* [한국어] 아키텍처가 자기 초기화를 마저 한다. 그 결과가 이 함수의 반환값이 된다 */
}

#else /* !CONFIG_SPARSE_IRQ */	/* [한국어] 여기부터는 서술자를 정적 배열에 두는 옛 방식이다. 번호 공간이 작고 고정인 임베디드 시스템용 */

struct irq_desc irq_desc[NR_IRQS] __cacheline_aligned_in_smp = {
	/* [한국어] 모든 인터럽트 서술자를 담은 정적 배열.
	 * 희소 빌드의 메이플 트리를 대신한다. 번호가 곧 인덱스라 조회가
	 * 배열 접근 한 번으로 끝나는 대신, 실제로 쓰지 않는 번호의
	 * 서술자도 메모리를 차지한다.
	 * __cacheline_aligned_in_smp: 배열 시작을 캐시 라인에 맞춘다.
	 * 서로 다른 CPU 가 서로 다른 인터럽트를 동시에 처리할 때 한
	 * 라인을 나눠 쓰지 않게 하려는 것이다(거짓 공유 방지).
	 * 아래 지정 초기화는 배열의 모든 원소에 같은 초기값을 준다. */
	[0 ... NR_IRQS-1] = {
		/* [한국어] GCC 확장인 범위 지정 초기화. 0 번부터 마지막까지
		 * 전부 아래 세 필드로 시작한다. 나머지 필드는 0 이 되고,
		 * early_irq_init() 이 init_desc 로 제대로 채운다. */
		.handle_irq	= handle_bad_irq,
		/* [한국어] 초기 흐름 처리기.
		 * 설정자: 여기 정적 초기화, 이후 desc_set_defaults().
		 * 읽는 자: 인터럽트가 발생하면 코어가 이것을 부른다.
		 * 값 범위: handle_bad_irq — 아무도 담당하지 않는 인터럽트가
		 *   들어왔을 때 경고를 찍는 함수. 정적 초기화 시점에 이
		 *   값이어야 하는 이유는, early_irq_init() 전에 인터럽트가
		 *   들어오더라도 NULL 포인터 호출이 되지 않게 하기 위해서다.
		 * 동기화: desc->lock 아래에서 바뀐다. */
		.depth		= 1,
		/* [한국어] 비활성 중첩 깊이의 초기값.
		 * 설정자: 정적 초기화, 이후 enable_irq/disable_irq 가 증감.
		 * 읽는 자: __enable_irq() 가 0 이 될 때만 실제로 연다.
		 * 값 범위: 0(열림) 이상. 1 로 시작한다는 것은 누군가
		 *   명시적으로 열어야 동작한다는 뜻이다.
		 * 동기화: desc->lock 아래. */
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(irq_desc->lock),
		/* [한국어] 이 서술자의 모든 상태를 지키는 락.
		 * 설정자: 여기 정적 초기화 (풀린 상태), 이후 init_desc 가
		 *   raw_spin_lock_init 으로 다시 초기화한다.
		 * 읽는 자: chip.c, manage.c, handle.c 등 코어 전체.
		 * 값 범위: raw_spinlock — 인터럽트 문맥에서 잡히고
		 *   PREEMPT_RT 에서도 잠들면 안 되므로 raw 여야 한다.
		 * 동기화: 이것이 동기화 수단 자체다.
		 * 정적으로도 초기화해 두는 이유: early_irq_init() 이 돌기
		 *   전에 이 락을 잡는 경로가 있을 수 있어서다. 인자로 넘긴
		 *   irq_desc->lock 은 lockdep 이 쓸 이름일 뿐이다. */
	}
};

/*
 * [한국어]
 * early_irq_init - 부팅 초기 서술자 배열을 초기화한다 (비희소 판)
 *
 * @return: 0 성공, 음수 초기화 실패
 *
 * 희소 판과 하는 일은 같지만 방식이 정반대다. 희소 판은 필요한 것만
 * 만들지만, 여기서는 배열 전체를 한 번에 초기화한다. 메모리는 이미
 * 정적으로 잡혀 있으므로 할당이 아니라 초기화만 하면 된다.
 *
 * 그래도 실패할 수 있는 이유: 각 서술자의 per-CPU 통계 영역과 CPU
 * 마스크는 동적 할당이다. 구조체 자체는 정적이어도 그 안의 포인터가
 * 가리킬 곳은 따로 잡아야 한다.
 *
 * 에러 경로가 while (--i >= 0) 인 것에 주목: 실패한 i 번은 init_desc
 * 가 자기 것을 이미 풀었으므로 그 앞부터 되돌린다. 여기서 free_masks 와
 * free_percpu 를 직접 부르는 이유는, 서술자 자체가 정적 배열의 일부라
 * kfree 하면 안 되기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드.
 *
 * 호출 체인:
 *   start_kernel() → early_irq_init() → [이 함수] → init_desc()
 */
int __init early_irq_init(void)
{
	int count, i, node = first_online_node;	/* [한국어] 배열 크기와, 서술자를 둘 노드 */
	int ret;	/* [한국어] init_desc 의 결과 */

	init_irq_default_affinity();	/* [한국어] 기본 친화도 마스크 준비 */

	printk(KERN_INFO "NR_IRQS: %d\n", NR_IRQS);	/* [한국어] 희소 판보다 짧다 — 여기서는 상한이 컴파일 시 고정이라 보고할 것이 하나뿐이다 */

	count = ARRAY_SIZE(irq_desc);	/* [한국어] 배열 원소 수. NR_IRQS 와 같지만 배열에서 직접 얻어 어긋남을 막는다 */

	for (i = 0; i < count; i++) {	/* [한국어] 모든 서술자를 초기화한다. 쓰지 않을 번호까지 전부 — 정적 배열의 대가다 */
		ret = init_desc(irq_desc + i, i, node, 0, NULL, NULL);	/* [한국어] 락·통계·마스크를 준비한다. 메모리 자체는 이미 있다 */
		if (unlikely(ret))	/* [한국어] 통계 영역이나 마스크 할당이 실패했는가 */
			goto __free_desc_res;	/* [한국어] 이미 초기화한 것들의 동적 자원을 되돌려야 한다 */
	}

	return arch_early_irq_init();	/* [한국어] 아키텍처 초기화를 마저 하고 그 결과를 돌려준다 */

__free_desc_res:	/* [한국어] 초기화 중간에 실패했을 때 */
	while (--i >= 0) {	/* [한국어] 실패한 i 번은 init_desc 가 이미 자기 것을 풀었으므로 그 앞부터 */
		free_masks(irq_desc + i);	/* [한국어] CPU 마스크들 */
		free_percpu(irq_desc[i].kstat_irqs);	/* [한국어] 통계 영역. 서술자 자체는 정적 배열이라 kfree 하지 않는다 */
	}

	return ret;	/* [한국어] 실패 원인을 그대로 올린다. start_kernel 이 이 값을 보고 패닉한다 */
}

/*
 * [한국어]
 * irq_to_desc - 인터럽트 번호로 서술자를 찾는다 (비희소 판)
 *
 * @irq: 리눅스 인터럽트 번호
 * @return: 서술자 포인터, 범위를 벗어나면 NULL
 *
 * 배열 인덱싱 한 번이 전부다. 희소 판의 트리 조회보다 훨씬 빠르지만,
 * 대신 쓰지 않는 번호도 메모리를 차지한다. 이 맞바꿈이 두 빌드를
 * 나누는 이유다 — 인터럽트가 수십 개뿐인 임베디드에서는 배열이 낫고,
 * MSI 로 수천 개가 오가는 서버에서는 트리가 낫다.
 *
 * EXPORT_SYMBOL 이 GPL 판이 아닌 것에 주목: 희소 판은
 * EXPORT_SYMBOL_GPL 이고 그것도 특정 설정에서만인데, 여기서는 그냥
 * EXPORT_SYMBOL 이다. 이 방식을 쓰는 오래된 아키텍처의 드라이버들이
 * 예전부터 이 심볼에 의존해 온 역사적 결과다.
 *
 * 실행 컨텍스트: 제약 없음. RCU 도 락도 필요 없다 — 배열은 사라지지
 * 않는다.
 *
 * 호출 체인:
 *   인터럽트 코어 전반 → [이 함수]
 */
struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;	/* [한국어] 범위 검사 후 배열 인덱싱. unsigned 라 음수 검사는 필요 없다 */
}
EXPORT_SYMBOL(irq_to_desc);	/* [한국어] 이 방식을 쓰는 오래된 아키텍처의 드라이버들이 예전부터 의존해 왔다 */

/*
 * [한국어]
 * free_desc - 서술자를 해제한다 (비희소 판)
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 희소 판과 근본적으로 다르다. 서술자가 정적 배열의 원소라 메모리를
 * 반납할 수 없으므로, "해제" 가 곧 "기본값으로 되돌리기" 다. RCU 도
 * 필요 없다 — 누가 이 포인터를 들고 있어도 메모리 자체는 계속
 * 유효하기 때문이다.
 *
 * 통계 카운터를 손으로 0 으로 미는 것에 주목: 희소 판에서는 서술자를
 * 통째로 버리므로 이 작업이 필요 없다. 여기서는 다음에 이 번호를 쓰는
 * 쪽이 옛 카운터를 물려받지 않게 지워야 한다.
 *
 * 락을 잡는 이유: 배열 원소는 사라지지 않으므로 다른 CPU 가 이 서술자를
 * 만지고 있을 수 있다. 희소 판에서는 트리에서 뺀 뒤라 새 접근이
 * 없었지만, 여기서는 irq_to_desc 가 계속 성공한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   irq_free_descs() → [이 함수] → desc_set_defaults() / delete_irq_desc()
 */
static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 배열 원소. 절대 NULL 이 아니다 — 호출자가 범위를 확인했다 */
	int cpu;	/* [한국어] 통계 초기화 순회용 */

	scoped_guard(raw_spinlock_irqsave, &desc->lock)	/* [한국어] 배열 원소는 사라지지 않아 다른 CPU 가 만지고 있을 수 있다. 희소 판과 달리 락이 반드시 필요하다 */
		desc_set_defaults(irq, desc, irq_desc_get_node(desc), NULL, NULL);	/* [한국어] 메모리를 반납하는 대신 기본값으로 되돌린다. 노드는 원래 값을 유지한다 */

	for_each_possible_cpu(cpu)	/* [한국어] 모든 CPU 의 카운터를 */
		*per_cpu_ptr(desc->kstat_irqs, cpu) = (struct irqstat) { };	/* [한국어] 0 으로 민다. 다음에 이 번호를 쓰는 쪽이 옛 통계를 물려받지 않게 한다 */

	delete_irq_desc(irq);	/* [한국어] 트리에서도 뺀다. 비희소 빌드에도 메이플 트리가 있는 이유는 /proc 순회가 "존재하는 인터럽트" 만 보여야 하기 때문이다 */
}

/*
 * [한국어]
 * alloc_descs - 서술자 구간을 사용 중으로 표시한다 (비희소 판)
 *
 * @start:    시작 번호
 * @cnt:      개수
 * @node:     쓰지 않는다 (배열이 이미 자리를 정했다)
 * @affinity: 쓰지 않는다
 * @owner:    소유 모듈
 * @return:   항상 @start (실패할 수 없다)
 *
 * 희소 판과 이름만 같고 하는 일이 훨씬 적다. 서술자는 이미 존재하므로
 * 만들 것이 없고, 소유 모듈을 기록하고 트리에 등록하기만 하면 된다.
 *
 * affinity 인자를 무시하는 것이 눈에 띈다. 이 방식을 쓰는 시스템은
 * 대개 UP 이거나 CPU 가 몇 개 안 되어 인터럽트별 친화도 지정이 의미가
 * 적다. 인터페이스만 희소 판과 맞춰 두고 값은 버린다.
 *
 * 실행 컨텍스트: 프로세스 문맥, sparse_irq_lock 보유.
 *
 * 호출 체인:
 *   __irq_alloc_descs() → [이 함수] → irq_insert_desc()
 */
static inline int alloc_descs(unsigned int start, unsigned int cnt, int node,
			      const struct irq_affinity_desc *affinity,
			      struct module *owner)
{
	u32 i;	/* [한국어] 순회용 */

	for (i = 0; i < cnt; i++) {	/* [한국어] 구간의 각 번호에 대해 */
		struct irq_desc *desc = irq_to_desc(start + i);	/* [한국어] 이미 존재하는 배열 원소 */

		desc->owner = owner;	/* [한국어] 소유 모듈 기록. 모듈이 내려간 뒤의 접근을 막는 데 쓴다 */
		irq_insert_desc(start + i, desc);	/* [한국어] 트리에 등록해 "사용 중" 으로 표시한다. 이 번호가 /proc 순회에 나타나기 시작한다 */
	}
	return start;	/* [한국어] 실패할 여지가 없어 항상 성공이다 */
}

/*
 * [한국어]
 * irq_expand_nr_irqs - 지원 인터럽트 상한을 늘린다 (비희소 판, 항상 실패)
 *
 * @nr: 무시
 * @return: 항상 false
 *
 * 배열 크기가 컴파일 시 NR_IRQS 로 고정이라 늘릴 방법이 없다. 늘리려면
 * 커널을 다시 빌드해야 한다. 호출자인 __irq_alloc_descs() 는 이 false 를
 * 보고 -ENOMEM 을 돌려준다.
 *
 * 이 한계가 CONFIG_SPARSE_IRQ 가 만들어진 이유이기도 하다. MSI 를 쓰는
 * 장치를 꽂을 때마다 인터럽트 번호가 필요한데, 배열로는 최악의 경우를
 * 미리 잡아 두는 수밖에 없어 낭비가 크다.
 *
 * 실행 컨텍스트: __irq_alloc_descs() 안.
 *
 * 호출 체인:
 *   __irq_alloc_descs() → [이 함수]
 */
static inline bool irq_expand_nr_irqs(unsigned int nr)
{
	return false;	/* [한국어] 정적 배열은 늘어나지 않는다 */
}

/*
 * [한국어]
 * irq_mark_irq - 이미 존재하는 서술자를 사용 중으로 등록한다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 아키텍처 코드가 "이 번호는 내가 쓴다" 고 선언하는 통로다. 정상
 * 할당 경로(__irq_alloc_descs)를 거치지 않고 특정 번호를 곧바로
 * 점유해야 하는 경우 — 아키텍처가 하드웨어 배선상 고정된 번호를 쓸
 * 때 — 를 위한 것이다.
 *
 * 희소 빌드에는 이 함수가 없다. 그쪽에서는 서술자가 없으면 만들어야
 * 하므로 "표시만" 하는 것이 의미가 없고, __irq_alloc_descs 에 원하는
 * 번호를 지정해 부르면 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   아키텍처 인터럽트 초기화 코드 → [이 함수] → irq_insert_desc()
 */
void irq_mark_irq(unsigned int irq)
{
	guard(mutex)(&sparse_irq_lock);	/* [한국어] 트리 조작이므로 다른 할당·해제와 직렬화한다 */
	irq_insert_desc(irq, irq_desc + irq);	/* [한국어] 배열 원소를 트리에 등록한다. 범위 검사가 없는데, 아키텍처 코드가 유효한 번호만 넘긴다고 신뢰한다 */
}

#endif /* !CONFIG_SPARSE_IRQ */	/* [한국어] 희소/비희소 분기의 끝. 아래는 두 빌드가 공유하는 코드다 */

/*
 * [한국어]
 * handle_irq_desc - 서술자를 받아 공용 인터럽트 처리를 시작한다
 *
 * @desc: 처리할 인터럽트의 서술자. NULL 일 수 있다.
 * @return: 0 처리 시작함, -EINVAL 서술자가 없음, -EPERM 문맥 규칙 위반
 *
 * 아키텍처 고유 코드와 공용 인터럽트 코어가 만나는 지점이다. 아래
 * generic_handle_* 계열이 모두 이 함수로 모인다 — 그들의 차이는 어떻게
 * 서술자를 찾느냐뿐이고, 찾은 뒤의 처리는 전부 여기서 하나로 합쳐진다.
 *
 * 두 가지 검사만 한 뒤 넘긴다.
 *
 * NULL 검사: 존재하지 않는 번호나 매핑되지 않은 하드웨어 인터럽트가
 * 올라온 경우다. 하드웨어 오류이거나 아직 설정이 안 끝난 상태이므로
 * 조용히 -EINVAL 을 돌려준다. 호출자(아키텍처 코드)가 대개 그 벡터를
 * 오탐으로 세거나 로그를 남긴다.
 *
 * 문맥 검사: 일부 인터럽트는 반드시 하드 인터럽트 문맥에서 처리돼야
 * 한다(IRQD_HANDLE_ENFORCE_IRQCTX). 그 이유는 흐름 처리기나 칩 콜백이
 * in_hardirq() 를 전제로 동작하기 때문이다. 프로세스 문맥에서 부르면
 * per-CPU 변수 접근이나 선점 가정이 깨진다. WARN_ON_ONCE 라서 한 번만
 * 시끄럽게 알리고 이후는 조용히 거절한다.
 *
 * 실행 컨텍스트: 보통 하드 인터럽트 문맥. generic_handle_irq_safe()
 * 경로로 오면 인터럽트를 끈 프로세스 문맥일 수도 있다.
 *
 * 호출 체인:
 *   generic_handle_irq() / generic_handle_domain_irq() / irq_redirect_work()
 *   → [이 함수] → generic_handle_irq_desc() → desc->handle_irq()
 */
int handle_irq_desc(struct irq_desc *desc)
{
	struct irq_data *data;	/* [한국어] 문맥 검사에 쓸 irq_data */

	if (!desc)	/* [한국어] 매핑되지 않은 인터럽트가 올라왔는가 */
		return -EINVAL;	/* [한국어] 아키텍처 코드가 이 값을 보고 오탐으로 처리한다 */

	data = irq_desc_get_irq_data(desc);	/* [한국어] 서술자 안의 irq_data. 상태 플래그가 여기 있다 */
	if (WARN_ON_ONCE(!in_hardirq() && irqd_is_handle_enforce_irqctx(data)))	/* [한국어] 하드 인터럽트 문맥을 요구하는 인터럽트를 다른 문맥에서 부르려 하는가 */
		return -EPERM;	/* [한국어] 흐름 처리기가 in_hardirq 를 전제하므로 그냥 진행하면 per-CPU 접근이나 선점 가정이 깨진다. ONCE 라 한 번만 시끄럽다 */

	generic_handle_irq_desc(desc);	/* [한국어] 통계를 올리고 desc->handle_irq 를 부른다. 여기서부터 흐름 처리기의 세계다 */
	return 0;	/* [한국어] 처리를 시작했다는 뜻. 핸들러의 성공 여부와는 무관하다 */
}

/**
 * generic_handle_irq - Invoke the handler for a particular irq
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
  */
/*
 * [한국어]
 * generic_handle_irq - 리눅스 인터럽트 번호로 처리를 시작한다
 *
 * @irq: 리눅스 인터럽트 번호
 * @return: 0 성공, -EINVAL 그런 번호의 서술자가 없음
 *
 * 가장 단순한 진입점이다. 아키텍처가 하드웨어 벡터에서 리눅스 번호를
 * 직접 알 수 있는 경우 — 옛 방식이나 고정 배선 인터럽트 — 에 쓴다.
 * 도메인을 쓰는 요즘 컨트롤러는 아래 generic_handle_domain_irq() 를
 * 쓴다.
 *
 * "irq regs 가 초기화된 인터럽트 문맥" 이라는 원본 주석의 조건이
 * 중요하다. 인터럽트 처리 중에 프로파일러나 오탐 검출기가
 * get_irq_regs() 로 인터럽트 당시의 레지스터를 본다. 아키텍처
 * 진입 코드가 set_irq_regs() 를 먼저 해 두어야 한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   아키텍처 인터럽트 진입 → [이 함수] → irq_to_desc() → handle_irq_desc()
 */
int generic_handle_irq(unsigned int irq)
{
	return handle_irq_desc(irq_to_desc(irq));	/* [한국어] 번호를 서술자로 바꿔 공통 경로에 넘긴다. NULL 처리는 handle_irq_desc 가 한다 */
}
EXPORT_SYMBOL_GPL(generic_handle_irq);	/* [한국어] 모듈로 빌드되는 인터럽트 컨트롤러 드라이버가 부른다 */

/**
 * generic_handle_irq_safe - Invoke the handler for a particular irq from any
 *			     context.
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process context). It
 * will report an error if not invoked from IRQ context and the irq has been
 * marked to enforce IRQ-context only.
 */
/*
 * [한국어]
 * generic_handle_irq_safe - 어떤 문맥에서든 인터럽트 처리를 시작한다
 *
 * @irq: 리눅스 인터럽트 번호
 * @return: 0 성공, -EINVAL 서술자 없음, -EPERM 하드 인터럽트 문맥 필수인데 아님
 *
 * 위 generic_handle_irq() 와 하는 일은 같지만 인터럽트를 끄고 부른다.
 * 왜 그래야 하는가: 흐름 처리기와 그 아래 코드는 인터럽트가 꺼진
 * 상태를 전제한다. 하드 인터럽트 문맥이면 CPU 가 이미 꺼 놓았지만,
 * 프로세스 문맥에서 부르면 그 전제가 없다.
 *
 * 언제 프로세스 문맥에서 인터럽트를 처리하는가: I2C 나 SPI 로 붙은
 * GPIO 확장 칩 같은 중첩 컨트롤러다. 그런 칩의 상태를 읽으려면 버스
 * 전송을 해야 하고 그것은 잠들 수 있는 작업이라, 부모 인터럽트의
 * 스레드 핸들러 안에서 자식 인터럽트를 처리하게 된다.
 *
 * 그래도 -EPERM 검사가 남아 있는 이유: 자식 인터럽트 중에도 하드
 * 인터럽트 문맥을 요구하는 것이 섞여 있을 수 있다. local_irq_save 는
 * 인터럽트만 끄지 in_hardirq() 를 참으로 만들지는 않는다.
 *
 * 실행 컨텍스트: 제약 없음 — 이 함수의 존재 이유다.
 *
 * 호출 체인:
 *   중첩 인터럽트 컨트롤러 드라이버의 스레드 핸들러 → [이 함수] →
 *   handle_irq_desc()
 */
int generic_handle_irq_safe(unsigned int irq)
{
	unsigned long flags;	/* [한국어] 원래 인터럽트 상태를 담아 둘 곳 */
	int ret;	/* [한국어] 처리 결과 */

	local_irq_save(flags);	/* [한국어] 인터럽트를 끄고 원래 상태를 저장한다. 하드 인터럽트 문맥이면 이미 꺼져 있어 사실상 무해하다 */
	ret = handle_irq_desc(irq_to_desc(irq));	/* [한국어] 공통 경로. 인터럽트가 꺼진 상태를 전제하는 코드가 안전해진다 */
	local_irq_restore(flags);	/* [한국어] 원래 상태로 되돌린다. 무조건 켜지 않는 것이 중요하다 — 원래 꺼져 있었을 수 있다 */
	return ret;	/* [한국어] handle_irq_desc 의 결과를 그대로 전달한다 */
}
EXPORT_SYMBOL_GPL(generic_handle_irq_safe);	/* [한국어] 중첩 컨트롤러 드라이버가 모듈인 경우가 많다 */

#ifdef CONFIG_IRQ_DOMAIN	/* [한국어] 도메인 기반 진입점들. 도메인 없이 빌드하는 아키텍처에서는 통째로 사라진다 */
/**
 * generic_handle_domain_irq - Invoke the handler for a HW irq belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
 */
/*
 * [한국어]
 * generic_handle_domain_irq - 도메인과 하드웨어 번호로 처리를 시작한다
 *
 * @domain: 이 컨트롤러의 인터럽트 도메인
 * @hwirq:  컨트롤러 입장에서의 인터럽트 번호
 * @return: 0 성공, -EINVAL 매핑이 없음
 *
 * 요즘 인터럽트 컨트롤러 드라이버의 표준 진입점이다. 드라이버는
 * 하드웨어 레지스터에서 "몇 번 입력이 울렸는지" 만 알면 되고, 그것을
 * 리눅스 번호로 바꾸는 일은 도메인이 한다.
 *
 * 이 분리가 중요한 이유: 컨트롤러 드라이버는 리눅스 번호를 몰라도
 * 된다. 같은 하드웨어 번호라도 커널이 배정하는 리눅스 번호는 부팅
 * 때마다 다를 수 있고, 드라이버가 그것을 추적할 필요가 없다.
 *
 * irq_resolve_mapping() 은 도메인의 역방향 맵을 찾는다. 도메인 종류에
 * 따라 배열 인덱싱이거나 해시 조회다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, irq regs 설정된 상태.
 *
 * 호출 체인:
 *   컨트롤러 드라이버의 체인 처리기 → [이 함수] →
 *   irq_resolve_mapping() → handle_irq_desc()
 */
int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));	/* [한국어] 도메인의 역방향 맵으로 서술자를 찾아 공통 경로에 넘긴다 */
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq);	/* [한국어] 거의 모든 irqchip 드라이버가 부른다 */

 /**
 * generic_handle_irq_safe - Invoke the handler for a HW irq belonging
 *			     to a domain from any context.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process
 * context). If the interrupt is marked as 'enforce IRQ-context only' then
 * the function must be invoked from hard interrupt context.
 */
/*
 * [한국어]
 * generic_handle_domain_irq_safe - 어떤 문맥에서든 도메인 인터럽트를 처리한다
 *
 * @domain: 이 컨트롤러의 인터럽트 도메인
 * @hwirq:  컨트롤러 입장에서의 인터럽트 번호
 * @return: 0 성공, -EINVAL 매핑 없음, -EPERM 문맥 규칙 위반
 *
 * generic_handle_domain_irq() 에 인터럽트 끄기를 더한 판이다.
 * generic_handle_irq_safe() 가 generic_handle_irq() 에 대해 갖는 관계와
 * 같다. 쓰이는 곳도 같다 — I2C/SPI 로 붙은 중첩 컨트롤러의 스레드
 * 핸들러다.
 *
 * 위 원본 주석의 이름이 generic_handle_irq_safe 로 잘못 적혀 있다.
 * 커널 문서 생성기가 함수 이름을 이 첫 줄에서 가져오므로, 이 오타
 * 때문에 문서에서 두 함수가 겹쳐 보인다. 코드 동작에는 영향이 없다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   중첩 컨트롤러 드라이버의 스레드 핸들러 → [이 함수] →
 *   irq_resolve_mapping() → handle_irq_desc()
 */
int generic_handle_domain_irq_safe(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	unsigned long flags;	/* [한국어] 원래 인터럽트 상태 */
	int ret;	/* [한국어] 처리 결과 */

	local_irq_save(flags);	/* [한국어] 인터럽트를 끈다. 아래 처리 경로가 그것을 전제한다 */
	ret = handle_irq_desc(irq_resolve_mapping(domain, hwirq));	/* [한국어] 매핑 조회와 처리 */
	local_irq_restore(flags);	/* [한국어] 원래 상태 복원 */
	return ret;	/* [한국어] 결과를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq_safe);	/* [한국어] 중첩 컨트롤러 드라이버용 */

/**
 * generic_handle_domain_nmi - Invoke the handler for a HW nmi belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an NMI context with irq regs
 * 		initialized.
 **/
/*
 * [한국어]
 * generic_handle_domain_nmi - NMI 를 도메인 경로로 처리한다
 *
 * @domain: 이 컨트롤러의 인터럽트 도메인
 * @hwirq:  컨트롤러 입장에서의 번호
 * @return: 0 성공, -EINVAL 매핑 없음
 *
 * NMI(Non-Maskable Interrupt)는 마스크할 수 없는 인터럽트다. 워치독이나
 * 프로파일러가 쓴다. 처리 경로는 일반 인터럽트와 같지만 제약이 훨씬
 * 크다 — 락을 잡을 수 없고, 잠들 수 없고, 대부분의 커널 함수를 부를 수
 * 없다. NMI 로 등록된 핸들러는 그 제약을 지키도록 따로 작성돼 있다.
 *
 * 이 함수가 하는 유일한 추가 작업이 in_nmi() 확인이다. NMI 로 표시된
 * 인터럽트를 일반 문맥에서 처리하면 그 핸들러의 전제가 깨진다.
 * WARN_ON_ONCE 로 알리기만 하고 계속 진행하는 이유는, NMI 경로에서
 * 처리를 거부하면 워치독이 멈추는 등 더 큰 문제가 생길 수 있어서다.
 *
 * EXPORT_SYMBOL 이 없는 것에 주목: NMI 를 다루는 컨트롤러는 코어에
 * 내장된 것뿐이다.
 *
 * 실행 컨텍스트: NMI 문맥.
 *
 * 호출 체인:
 *   아키텍처 NMI 진입 → 컨트롤러 드라이버 → [이 함수] → handle_irq_desc()
 */
int generic_handle_domain_nmi(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	WARN_ON_ONCE(!in_nmi());	/* [한국어] NMI 문맥이 맞는지 확인만 한다. 거부하지 않는 이유는 워치독이 멈추는 것이 더 위험해서다 */
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));	/* [한국어] 일반 경로와 같다. NMI 용 제약은 핸들러 쪽이 지킨다 */
}

#ifdef CONFIG_SMP	/* [한국어] 다른 CPU 로 넘긴다는 개념은 SMP 에만 있다 */
/*
 * [한국어]
 * demux_redirect_remote - 디먹스 인터럽트를 친화도에 맞는 CPU 로 넘긴다
 *
 * @desc: 처리하려는 자식 인터럽트의 서술자
 * @return: true 다른 CPU 로 넘겼으니 여기서는 처리하지 말 것,
 *          false 이 CPU 에서 그대로 처리할 것
 *
 * 어떤 문제를 푸는가: 여러 인터럽트를 한 선으로 묶어 올리는 디먹스
 * 컨트롤러에서, 부모 선의 친화도와 자식 인터럽트의 친화도가 다를 수
 * 있다. 부모가 CPU 0 에 오는데 자식은 CPU 3 에서 처리되기를 원하는
 * 경우다. 그대로 처리하면 사용자가 설정한 친화도가 무시된다.
 *
 * 해결: 지금 CPU 가 그 자식의 유효 친화도에 없으면, irq_work 로 목표
 * CPU 에 처리를 넘긴다. 그 CPU 에서 irq_redirect_work() 가 실행된다.
 *
 * desc->action 검사가 안전장치다. __free_irq() 가 desc->lock 을 쥔 채
 * action 을 NULL 로 만드는데, 이 함수도 같은 락을 쥐고 있다. 따라서
 * action 이 NULL 이 아니면 이 인터럽트는 아직 해제 중이 아니고,
 * 작업을 큐에 넣어도 서술자가 사라지지 않는다.
 *
 * 원본의 긴 주석은 CPU 언플러그와의 경쟁을 다룬다. 요지는 이렇다.
 * CPU 를 내릴 때 multi_cpu_stop() 이 장벽 역할을 해서, 하드 인터럽트
 * 문맥인 이 함수는 take_cpu_down() 보다 확실히 앞이거나 뒤다. 앞이면
 * 목표 CPU 가 아직 오프라인 표시가 안 되어 경고가 나지 않고 큐에 넣은
 * 작업도 나중에 비워진다. 뒤라면 친화도 마이그레이션이 이미 끝나
 * target_cpu 가 죽는 CPU 를 가리키지 않는다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, 부모 인터럽트 처리 중.
 * desc->lock 을 이 함수가 직접 잡는다.
 *
 * 호출 체인:
 *   generic_handle_demux_domain_irq() → [이 함수] → irq_work_queue_on()
 */
static bool demux_redirect_remote(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);	/* [한국어] 친화도와 action 을 한 시점의 값으로 읽고, __free_irq 와의 경쟁을 막는다. 이미 인터럽트가 꺼진 문맥이라 irqsave 가 필요 없다 */
	const struct cpumask *m = irq_data_get_effective_affinity_mask(&desc->irq_data);	/* [한국어] 요청 친화도가 아니라 유효 친화도 — 하드웨어가 실제로 고른 CPU 들이다 */
	unsigned int target_cpu = READ_ONCE(desc->redirect.target_cpu);	/* [한국어] 넘길 목표 CPU. 친화도 변경 경로가 락 밖에서 갱신할 수 있어 READ_ONCE 로 한 번만 읽는다 */

	if (desc->irq_data.chip->irq_pre_redirect)	/* [한국어] 넘기기 전에 할 일이 있는 칩인가 */
		desc->irq_data.chip->irq_pre_redirect(&desc->irq_data);	/* [한국어] 대개 원인 비트를 지워, 넘기는 동안 같은 인터럽트가 또 올라오지 않게 한다 */

	/*
	 * If the interrupt handler is already running on a CPU that's included
	 * in the interrupt's affinity mask, redirection is not necessary.
	 */
	if (cpumask_test_cpu(smp_processor_id(), m))	/* [한국어] (위 영어 주석) 지금 CPU 가 이미 친화도 안에 있는가 */
		return false;	/* [한국어] 넘길 이유가 없다. 호출자가 여기서 그대로 처리한다 */

	/*
	 * The desc->action check protects against IRQ shutdown: __free_irq() sets
	 * desc->action to NULL while holding desc->lock, which we also hold.
	 *
	 * Calling irq_work_queue_on() here is safe w.r.t. CPU unplugging:
	 *   - takedown_cpu() schedules multi_cpu_stop() on all active CPUs,
	 *     including the one that's taken down.
	 *   - multi_cpu_stop() acts like a barrier, which means all active
	 *     CPUs go through MULTI_STOP_DISABLE_IRQ and disable hard IRQs
	 *     *before* the dying CPU runs take_cpu_down() in MULTI_STOP_RUN.
	 *   - Hard IRQs are re-enabled at the end of multi_cpu_stop(), *after*
	 *     the dying CPU has run take_cpu_down() in MULTI_STOP_RUN.
	 *   - Since we run in hard IRQ context, we run either before or after
	 *     take_cpu_down() but never concurrently.
	 *   - If we run before take_cpu_down(), the dying CPU hasn't been marked
	 *     offline yet (it's marked via take_cpu_down() -> __cpu_disable()),
	 *     so the WARN in irq_work_queue_on() can't occur.
	 *   - Furthermore, the work item we queue will be flushed later via
	 *     take_cpu_down() -> cpuhp_invoke_callback_range_nofail() ->
	 *     smpcfd_dying_cpu() -> irq_work_run().
	 *   - If we run after take_cpu_down(), target_cpu has been already
	 *     updated via take_cpu_down() -> __cpu_disable(), which eventually
	 *     calls irq_do_set_affinity() during IRQ migration. So, target_cpu
	 *     no longer points to the dying CPU in this case.
	 */
	if (desc->action)	/* [한국어] (위 영어 주석) 아직 핸들러가 등록돼 있는가 — 해제 중이 아닌가. 같은 락을 쥐고 있어 __free_irq 와 엇갈리지 않는다 */
		irq_work_queue_on(&desc->redirect.work, target_cpu);	/* [한국어] 목표 CPU 에 처리를 넘긴다. 그쪽에서 irq_redirect_work 가 하드 인터럽트 문맥으로 실행된다 */

	return true;	/* [한국어] 넘겼으니 이 CPU 에서는 처리하지 말라고 알린다. action 이 NULL 이어서 안 넘긴 경우에도 true 다 — 그때는 처리할 핸들러 자체가 없다 */
}
#else /* CONFIG_SMP */	/* [한국어] 단일 프로세서 */
/*
 * [한국어]
 * demux_redirect_remote - 다른 CPU 로 넘기기 (UP 판, 항상 실패)
 *
 * @desc: 무시
 * @return: 항상 false
 *
 * 넘길 다른 CPU 가 없다. 호출자가 항상 이 CPU 에서 처리하게 된다.
 * 호출부를 #ifdef 로 나누지 않으려고 이 판을 둔다.
 *
 * 호출 체인:
 *   generic_handle_demux_domain_irq() → [이 함수]
 */
static bool demux_redirect_remote(struct irq_desc *desc)
{
	return false;	/* [한국어] 넘길 곳이 없으니 여기서 처리하라고 알린다 */
}
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/**
 * generic_handle_demux_domain_irq - Invoke the handler for a hardware interrupt
 *				     of a demultiplexing domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The hardware interrupt number to convert to a logical one
 *
 * Returns:	True on success, or false if lookup has failed
 */
/*
 * [한국어]
 * generic_handle_demux_domain_irq - 디먹스 컨트롤러의 자식 인터럽트를 처리한다
 *
 * @domain: 디먹스 컨트롤러의 도메인
 * @hwirq:  울린 자식 인터럽트의 하드웨어 번호
 * @return: true 처리했거나 다른 CPU 로 넘겼음, false 매핑이 없거나 처리 실패
 *
 * 디먹스(demultiplexing) 컨트롤러란: 자식 인터럽트 여러 개를 부모 선
 * 하나로 묶어 올리는 컨트롤러다. 부모 인터럽트가 오면 드라이버가 상태
 * 레지스터를 읽어 어느 자식이 울렸는지 알아내고, 각각에 대해 이 함수를
 * 부른다.
 *
 * 왜 generic_handle_domain_irq() 로 충분하지 않은가: 자식마다 친화도가
 * 다를 수 있는데 부모는 한 CPU 에만 온다. 그 어긋남을 위 리다이렉트
 * 기구가 푼다. 이 함수는 그 기구를 끼워 넣은 판이다.
 *
 * 반환 타입이 bool 인 것도 다르다. 디먹스 드라이버는 대개 여러 자식을
 * 순회하며 "하나라도 처리했는가" 를 세므로 불린이 편하다. 마지막 줄의
 * !handle_irq_desc(desc) 가 0(성공)을 true 로 뒤집는다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥, 부모 인터럽트 처리 중.
 *
 * 호출 체인:
 *   디먹스 컨트롤러 드라이버의 체인 처리기 → [이 함수] →
 *   demux_redirect_remote() 또는 handle_irq_desc()
 */
bool generic_handle_demux_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	struct irq_desc *desc = irq_resolve_mapping(domain, hwirq);	/* [한국어] 자식 인터럽트의 서술자 */

	if (unlikely(!desc))	/* [한국어] 매핑되지 않은 자식이 울렸는가 */
		return false;	/* [한국어] 드라이버가 이것을 오탐으로 센다 */

	if (demux_redirect_remote(desc))	/* [한국어] 이 CPU 가 자식의 친화도 밖인가 */
		return true;	/* [한국어] 넘겼으니 여기서는 끝. 드라이버 입장에서는 "처리됨" 이다 */

	return !handle_irq_desc(desc);	/* [한국어] 정상 처리. 0(성공)을 true 로 뒤집는다 — 이 함수만 불린을 쓰기 때문이다 */
}
EXPORT_SYMBOL_GPL(generic_handle_demux_domain_irq);	/* [한국어] 디먹스 컨트롤러 드라이버가 모듈일 수 있다 */

#endif	/* [한국어] CONFIG_IRQ_DOMAIN 분기의 끝 */

/* Dynamic interrupt handling */

/**
 * irq_free_descs - free irq descriptors
 * @from:	Start of descriptor range
 * @cnt:	Number of consecutive irqs to free
 */
/*
 * [한국어]
 * irq_free_descs - 인터럽트 서술자 구간을 해제한다
 *
 * @from: 해제할 구간의 첫 번호
 * @cnt:  개수
 * @return: 없음
 *
 * __irq_alloc_descs() 의 반대다. 구간 단위인 이유도 같다 — MSI-X 처럼
 * 벡터를 여러 개 잡은 장치가 한꺼번에 반납한다.
 *
 * 범위 검사가 조용히 반환하는 것에 주목: 잘못된 범위를 넘기면 오류를
 * 알리지 않고 아무것도 하지 않는다. 반환 타입이 void 라 알릴 방법이
 * 없기 때문이다. 이 API 는 대개 해제 경로에서 불리고, 그 경로에서
 * 오류를 처리할 방법도 마땅치 않다는 판단이다.
 *
 * from + cnt 의 오버플로를 따로 검사하지 않는 것은, 두 값 모두
 * nr_irqs 보다 훨씬 작은 범위에서만 쓰이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() / 드라이버 해제 경로 → [이 함수] → free_desc()
 */
void irq_free_descs(unsigned int from, unsigned int cnt)
{
	int i;	/* [한국어] 순회용 */

	if (from >= nr_irqs || (from + cnt) > nr_irqs)	/* [한국어] 구간이 유효 범위 안에 있는가 */
		return;	/* [한국어] 조용히 무시한다. void 라 오류를 알릴 방법이 없다 */

	guard(mutex)(&sparse_irq_lock);	/* [한국어] 트리 조작과 /proc 순회를 직렬화한다 */
	for (i = 0; i < cnt; i++)	/* [한국어] 구간의 각 번호를 */
		free_desc(from + i);	/* [한국어] 하나씩 해제한다. 빌드에 따라 실제 해제이거나 기본값 되돌리기다 */
}
EXPORT_SYMBOL_GPL(irq_free_descs);	/* [한국어] 인터럽트를 직접 할당하는 드라이버가 부른다 */

/**
 * __irq_alloc_descs - allocate and initialize a range of irq descriptors
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate.
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 * @affinity:	Optional pointer to an affinity mask array of size @cnt which
 *		hints where the irq descriptors should be allocated and which
 *		default affinities to use
 *
 * Returns the first irq number or error code
 */
/*
 * [한국어]
 * __irq_alloc_descs - 연속된 인터럽트 서술자 구간을 할당한다
 *
 * @irq:      특정 번호를 원하면 그 번호, 아무 데나 좋으면 음수
 * @from:     검색을 시작할 번호 (@irq 가 음수일 때만 의미가 있다)
 * @cnt:      필요한 연속 개수
 * @node:     서술자를 둘 선호 NUMA 노드
 * @owner:    소유 모듈 (NULL 가능)
 * @affinity: 인터럽트별 친화도 배열 (cnt 개) 또는 NULL
 * @return:   할당된 구간의 첫 번호, 또는 음수 오류
 *
 * 인터럽트 번호를 얻는 유일한 정문이다. 두 가지 방식으로 부를 수 있다.
 *
 *   irq >= 0 — "정확히 이 번호를 달라". 레거시 인터럽트처럼 번호가
 *              하드웨어적으로 고정된 경우다. 그 번호가 이미 쓰이고
 *              있으면 -EEXIST 다.
 *   irq < 0  — "아무 번호나 cnt 개 연속으로 달라". MSI 등 대부분의
 *              현대적 경우.
 *
 * arch_dynirq_lower_bound() 가 흥미로운 지점이다. 원본 주석이 x86 을
 * 예로 드는데, x86 에는 ACPI 가 정의하는 GSI(Global System Interrupt)
 * 공간이 있어 그 번호들은 나중에 특정 장치에 배정될 수 있다. 자유
 * 할당이 그 영역을 먹어 버리면 나중에 -EEXIST 가 난다. 그래서
 * 아키텍처가 하한을 밀어 올린다.
 *
 * __ref 표시: 이 함수가 __init 코드를 부를 수 있다는 뜻이다. 부팅
 * 초기에도 불리기 때문인데, 섹션 불일치 경고를 억제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스와 GFP_KERNEL 할당이 있어
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   irq_domain_alloc_descs() / 드라이버 → [이 함수] →
 *   irq_find_free_area() → alloc_descs()
 */
int __ref __irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node,
			    struct module *owner, const struct irq_affinity_desc *affinity)
{
	int start;	/* [한국어] 실제로 배정된 구간의 첫 번호 */

	if (!cnt)	/* [한국어] 0 개를 요청했는가 */
		return -EINVAL;	/* [한국어] 호출자의 버그다. 아래 검색이 0 개 구간을 어떻게 다룰지 정의되지 않았다 */

	if (irq >= 0) {	/* [한국어] 특정 번호를 요구하는 경우 */
		if (from > irq)	/* [한국어] 검색 시작점이 요구 번호보다 뒤인가 — 모순된 요청이다 */
			return -EINVAL;	/* [한국어] 그 번호를 절대 찾을 수 없는 조합이다 */
		from = irq;	/* [한국어] 검색을 그 번호에서 시작하게 맞춘다. 아래에서 start != irq 인지 확인한다 */
	} else {	/* [한국어] 아무 번호나 좋은 경우 */
		/*
		 * For interrupts which are freely allocated the
		 * architecture can force a lower bound to the @from
		 * argument. x86 uses this to exclude the GSI space.
		 */
		from = arch_dynirq_lower_bound(from);	/* [한국어] (위 영어 주석) 아키텍처가 예약해 둔 저번호 영역을 피한다. x86 은 ACPI 의 GSI 공간을 이렇게 지킨다 */
	}

	guard(mutex)(&sparse_irq_lock);	/* [한국어] 빈 구간을 찾고 실제로 채워 넣기까지가 한 임계 구역이어야 한다. 그 사이에 다른 CPU 가 같은 구간을 가져가면 안 된다 */

	start = irq_find_free_area(from, cnt);	/* [한국어] 연속으로 비어 있는 구간을 찾는다 */
	if (irq >=0 && start != irq)	/* [한국어] 특정 번호를 요구했는데 다른 곳이 나왔는가 — 그 번호가 이미 쓰이고 있다는 뜻이다 */
		return -EEXIST;	/* [한국어] 다른 번호로 대체하지 않는다. 요구한 쪽은 그 번호여야만 하는 이유가 있다 */

	if (start + cnt > nr_irqs) {	/* [한국어] 찾은 구간이 현재 상한을 넘는가 */
		if (!irq_expand_nr_irqs(start + cnt))	/* [한국어] 상한을 늘려 본다. 비희소 빌드에서는 항상 실패한다 */
			return -ENOMEM;	/* [한국어] 번호 공간이 부족하다 */
	}
	return alloc_descs(start, cnt, node, affinity, owner);	/* [한국어] 실제 생성. 성공하면 start 를, 실패하면 음수를 돌려준다 */
}
EXPORT_SYMBOL_GPL(__irq_alloc_descs);	/* [한국어] 인터럽트 번호를 직접 잡는 드라이버와 도메인 코드가 부른다 */

/**
 * irq_get_next_irq - get next allocated irq number
 * @offset:	where to start the search
 *
 * Returns next irq number after offset or nr_irqs if none is found.
 */
/*
 * [한국어]
 * irq_get_next_irq - 다음으로 존재하는 인터럽트 번호를 찾는다
 *
 * @offset: 검색을 시작할 번호
 * @return: 찾은 번호, 없으면 nr_irqs
 *
 * for_each_irq_desc 매크로가 쓰는 순회 기반이다. 희소 배치에서는
 * 번호가 듬성듬성해 offset+1 을 하나씩 시도하면 대부분 헛수고가 되므로,
 * 트리에게 직접 묻는다.
 *
 * 껍데기에 불과한데도 따로 두는 이유: irq_find_at_or_after() 는
 * static 이고, 이 파일 밖(내부 헤더를 통해)에서 쓸 이름이 필요하다.
 * 이름도 더 서술적이다.
 *
 * 반환값이 nr_irqs 라는 것이 "더 없음" 의 약속이다. 호출자의 루프
 * 조건이 그 값과 비교하도록 짜여 있다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   for_each_irq_desc 매크로 / /proc 순회 → [이 함수] →
 *   irq_find_at_or_after()
 */
unsigned int irq_get_next_irq(unsigned int offset)
{
	return irq_find_at_or_after(offset);	/* [한국어] RCU 로 보호된 트리 조회에 위임한다 */
}

/*
 * [한국어]
 * __irq_get_desc_lock - 서술자를 찾아 락까지 잡아 돌려준다
 *
 * @irq:   인터럽트 번호
 * @flags: 인터럽트 상태를 저장할 곳 (출력)
 * @bus:   true 면 칩 버스 락도 함께 잡는다
 * @check: _IRQ_DESC_CHECK, _IRQ_DESC_PERCPU 조합 — 종류 검증
 * @return: 락을 잡은 서술자, 조건에 맞지 않으면 NULL (그때는 락도 안 잡힌다)
 *
 * manage.c 와 chip.c 의 거의 모든 공개 함수가 이 관용구로 시작한다.
 * "번호로 찾고, 유효성 검사하고, 락을 잡는" 세 단계가 매번 반복되므로
 * 하나로 묶었다.
 *
 * check 인자가 푸는 문제: per-CPU 인터럽트와 일반 인터럽트는 API 가
 * 다르다. request_percpu_irq 로 잡은 것을 free_irq 로 풀면 자료구조
 * 해석이 어긋나 조용히 망가진다. 그래서 각 API 가 "내가 다루는 종류가
 * 맞는지" 를 여기서 확인한다. 두 검사가 서로의 부정이라는 점에
 * 주목 — 일반 API 는 per-CPU 를 거부하고, per-CPU API 는 일반을
 * 거부한다.
 *
 * bus 인자: I2C/SPI 로 붙은 컨트롤러는 레지스터를 만지려면 버스 전송이
 * 필요하고 그것은 잠들 수 있다. 그래서 스핀락을 잡기 *전에* 버스 락을
 * 먼저 잡아 둔다. 순서가 반대면 스핀락을 쥔 채 잠들게 된다.
 *
 * 실행 컨텍스트: 대개 프로세스 문맥. bus 가 true 면 반드시 잠들 수
 * 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   irq_set_type() / disable_irq() 등 수많은 공개 API → [이 함수]
 */
struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
				     unsigned int check)
{
	struct irq_desc *desc;	/* [한국어] 찾은 서술자 */

	desc = irq_to_desc(irq);	/* [한국어] 번호로 조회 */
	if (!desc)	/* [한국어] 그런 인터럽트가 없는가 */
		return NULL;	/* [한국어] 호출자가 -EINVAL 등으로 바꿔 올린다. 락을 잡지 않았으므로 풀 것도 없다 */

	if (check & _IRQ_DESC_CHECK) {	/* [한국어] 종류 검증을 요청했는가 */
		if ((check & _IRQ_DESC_PERCPU) && !irq_settings_is_per_cpu_devid(desc))	/* [한국어] per-CPU API 인데 일반 인터럽트인가 */
			return NULL;	/* [한국어] 자료구조 해석이 달라 그대로 진행하면 조용히 망가진다 */

		if (!(check & _IRQ_DESC_PERCPU) && irq_settings_is_per_cpu_devid(desc))	/* [한국어] 일반 API 인데 per-CPU 인터럽트인가 */
			return NULL;	/* [한국어] 위와 반대 방향의 같은 실수를 막는다 */
	}

	if (bus)	/* [한국어] 잠들 수 있는 버스 뒤의 컨트롤러인가 */
		chip_bus_lock(desc);	/* [한국어] 스핀락보다 먼저 잡는다. 순서가 반대면 스핀락을 쥔 채 잠들게 된다 */
	raw_spin_lock_irqsave(&desc->lock, *flags);	/* [한국어] 서술자 락. 원래 인터럽트 상태를 호출자의 flags 에 저장한다 */

	return desc;	/* [한국어] 락을 잡은 채로 돌려준다. 호출자는 반드시 __irq_put_desc_unlock 으로 풀어야 한다 */
}

/*
 * [한국어]
 * __irq_put_desc_unlock - __irq_get_desc_lock() 이 잡은 락들을 푼다
 *
 * @desc:  대상 서술자
 * @flags: get 이 저장해 둔 인터럽트 상태
 * @bus:   get 에 넘겼던 값과 같아야 한다
 * @return: 없음
 *
 * 푸는 순서가 잡은 순서의 역순이다. 스핀락을 먼저 풀고 버스 락을
 * 나중에 푼다. 버스 락은 잠들 수 있는 뮤텍스라, 스핀락을 쥔 채로
 * 풀려고 하면 그 안에서 잠들 수 있어 안 된다.
 *
 * __releases 주석은 sparse 정적 분석기를 위한 것이다. 이 함수가 락을
 * 푼다는 사실을 알려, 잡기와 풀기가 짝이 맞는지 컴파일 시점에 검사하게
 * 한다. 락을 잡는 쪽과 푸는 쪽이 다른 함수라 사람 눈으로는 놓치기 쉽다.
 *
 * bus 를 get 과 다르게 넘기면 조용히 망가진다 — 버스 락이 안 풀리거나
 * 잡지도 않은 것을 풀게 된다. 호출자들이 같은 지역 변수를 두 번 쓰는
 * 관례로 이것을 지킨다.
 *
 * 실행 컨텍스트: get 을 부른 곳과 같아야 한다.
 *
 * 호출 체인:
 *   __irq_get_desc_lock() 을 부른 모든 곳 → [이 함수]
 */
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus)
	__releases(&desc->lock)
{
	raw_spin_unlock_irqrestore(&desc->lock, flags);	/* [한국어] 서술자 락을 먼저 푼다. 저장해 둔 인터럽트 상태도 복원한다 */
	if (bus)	/* [한국어] 버스 락을 잡았던 경우 */
		chip_bus_sync_unlock(desc);	/* [한국어] 스핀락 밖에서 푼다 — 이 함수가 실제 버스 전송을 하며 잠들 수 있기 때문이다. 지연된 레지스터 쓰기가 여기서 하드웨어로 나간다 */
}

/*
 * [한국어]
 * irq_set_percpu_devid - 인터럽트를 per-CPU 종류로 바꾼다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 0 성공, -EINVAL 서술자가 없거나 이미 per-CPU 임, -ENOMEM 할당 실패
 *
 * per-CPU 인터럽트란: 같은 번호가 CPU 마다 별개의 인터럽트처럼 동작하는
 * 것이다. ARM 의 지역 타이머가 대표적이다 — 모든 CPU 가 자기 타이머
 * 인터럽트를 갖고, 번호는 하나를 공유한다. 그래서 CPU 마다 따로 켜고
 * 끌 수 있어야 하고, 그 상태를 담을 비트맵이 필요하다.
 *
 * 이 함수가 하는 일은 그 비트맵을 잡고 서술자에 종류 표시를 하는 것이다.
 * 그 뒤로는 request_percpu_irq / enable_percpu_irq 계열만 쓸 수 있고,
 * 일반 API 를 쓰면 위 __irq_get_desc_lock 의 검사가 막는다.
 *
 * 두 번 부를 수 없는 이유: percpu_enabled 가 이미 있으면 -EINVAL 이다.
 * 다시 잡으면 앞의 것이 새고, CPU 별 켜짐 상태도 잃는다.
 *
 * 락을 잡지 않는 것에 주목: 이 함수는 인터럽트를 아무도 쓰기 전,
 * 설정 단계에서만 불린다는 전제다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 초기화 단계.
 *
 * 호출 체인:
 *   ARM 아키텍처 타이머 등 per-CPU 장치 드라이버 초기화 → [이 함수]
 */
int irq_set_percpu_devid(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || desc->percpu_enabled)	/* [한국어] 없거나 이미 per-CPU 로 만들어졌는가 */
		return -EINVAL;	/* [한국어] 두 번 부르면 앞의 비트맵이 새고 CPU 별 상태도 잃는다 */

	desc->percpu_enabled = kzalloc_obj(*desc->percpu_enabled);	/* [한국어] CPU 별 켜짐 상태를 담을 비트맵. 0 으로 시작하므로 모든 CPU 에서 꺼진 상태다 */

	if (!desc->percpu_enabled)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 종류 표시를 하지 않았으므로 서술자는 일반 인터럽트로 남는다 */

	irq_set_percpu_devid_flags(irq);	/* [한국어] 설정 워드에 per-CPU 표시를 한다. 이 뒤로 일반 API 는 __irq_get_desc_lock 에서 거절된다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * kstat_incr_irq_this_cpu - 이 CPU 의 발생 횟수를 하나 올린다
 *
 * @irq: 인터럽트 번호
 * @return: 없음
 *
 * 번호를 서술자로 바꿔 실제 증가 함수에 넘기는 껍데기다. 정상 인터럽트
 * 경로는 이미 서술자를 들고 있어 kstat_incr_irqs_this_cpu 를 직접
 * 부르므로, 이 함수는 번호만 아는 예외적인 호출자 — 아키텍처 고유의
 * 인터럽트 처리 경로 — 를 위한 것이다.
 *
 * NULL 검사가 없는 것에 주목: irq_to_desc 가 NULL 을 돌려주면
 * kstat_incr_irqs_this_cpu 안에서 터진다. 이 함수를 부르는 쪽은 유효한
 * 번호만 넘긴다는 전제다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. per-CPU 변수를 만지므로 선점이 꺼져
 * 있어야 한다 — 인터럽트 문맥은 그 조건을 만족한다.
 *
 * 호출 체인:
 *   아키텍처 인터럽트 처리 경로 → [이 함수] → kstat_incr_irqs_this_cpu()
 */
void kstat_incr_irq_this_cpu(unsigned int irq)
{
	kstat_incr_irqs_this_cpu(irq_to_desc(irq));	/* [한국어] 번호를 서술자로 바꿔 넘긴다. 이 CPU 의 카운터만 올리므로 원자 연산이 필요 없다 */
}

/**
 * kstat_irqs_cpu - Get the statistics for an interrupt on a cpu
 * @irq:	The interrupt number
 * @cpu:	The cpu number
 *
 * Returns the sum of interrupt counts on @cpu since boot for
 * @irq. The caller must ensure that the interrupt is not removed
 * concurrently.
 */
/*
 * [한국어]
 * kstat_irqs_cpu - 특정 CPU 에서의 발생 횟수를 돌려준다
 *
 * @irq: 인터럽트 번호
 * @cpu: CPU 번호
 * @return: 부팅 이후 그 CPU 에서의 발생 횟수, 서술자가 없으면 0
 *
 * /proc/interrupts 의 각 칸에 들어가는 값이다. 인터럽트가 어느 CPU 에
 * 몰려 있는지 보는 것이 성능 진단의 출발점이다.
 *
 * 원본 주석의 조건이 중요하다 — "호출자가 인터럽트가 동시에 제거되지
 * 않도록 보장해야 한다". 이 함수는 RCU 도 락도 잡지 않으므로, 반환
 * 직후 서술자가 해제될 수 있다. /proc 쪽 호출자는 irq_lock_sparse() 로
 * 그것을 막는다.
 *
 * 없는 인터럽트에 0 을 돌려주는 것은 오류가 아니라 "발생한 적 없음" 과
 * 같은 표현이다. 호출자가 순회 중에 사라진 인터럽트를 만나도 자연스럽게
 * 처리된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   fs/proc/interrupts.c → [이 함수]
 */
unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	return desc && desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, cpu) : 0;	/* [한국어] 서술자와 통계 영역이 모두 있어야 읽는다. 없으면 0 — 오류가 아니라 "발생한 적 없음" 이다 */
}

/*
 * [한국어]
 * kstat_irqs_desc - 주어진 CPU 집합에서의 발생 횟수 합계
 *
 * @desc:    대상 서술자
 * @cpumask: 합산할 CPU 들
 * @return:  합계
 *
 * 이 함수의 핵심은 두 갈래로 나뉜다는 것이다.
 *
 * 일반 공유 인터럽트는 tot_count 라는 미리 합쳐 둔 값을 그대로 쓴다.
 * 인터럽트가 발생할 때마다 CPU 별 카운터와 함께 이 합계도 올려 두기
 * 때문이다. CPU 가 수백 개인 기계에서 /proc/interrupts 를 읽을 때마다
 * 수백 번 더하는 것을 피하려는 최적화다.
 *
 * 그런데 per-CPU 인터럽트와 NMI 는 그 합계를 쓸 수 없다. per-CPU
 * 인터럽트는 CPU 마다 사실상 다른 인터럽트라 하나의 합계가 의미가 없고,
 * NMI 는 발생 경로가 tot_count 를 올리지 않는다 — NMI 문맥에서 공유
 * 변수를 올리는 것 자체가 위험하기 때문이다. 이 경우에만 CPU 별
 * 카운터를 손으로 더한다.
 *
 * data_race() 표시: 이 읽기가 락 없이 이루어져 KCSAN(데이터 경쟁
 * 검출기)이 경고할 수 있는데, 통계값이 조금 낡거나 찢어져도 무해하다는
 * 것을 명시적으로 알려 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, RCU 읽기 구역 안일 수 있다.
 *
 * 호출 체인:
 *   kstat_irqs() → [이 함수]
 */
static unsigned int kstat_irqs_desc(struct irq_desc *desc, const struct cpumask *cpumask)
{
	unsigned int sum = 0;	/* [한국어] 합산 결과 */
	int cpu;	/* [한국어] 순회용 */

	if (!irq_settings_is_per_cpu_devid(desc) &&	/* [한국어] per-CPU 장치 ID 를 쓰는 인터럽트가 아니고 */
	    !irq_settings_is_per_cpu(desc) &&	/* [한국어] per-CPU 인터럽트도 아니고 */
	    !irq_is_nmi(desc))	/* [한국어] NMI 도 아닌가 — 즉 평범한 인터럽트인가 */
		return data_race(desc->tot_count);	/* [한국어] 미리 합쳐 둔 값을 그대로 쓴다. CPU 수백 개를 매번 훑지 않으려는 최적화다. data_race 는 락 없는 읽기를 KCSAN 에 허용한다는 표시 */

	for_each_cpu(cpu, cpumask)	/* [한국어] 위 셋 중 하나라면 합계를 신뢰할 수 없어 직접 더한다 */
		sum += data_race(per_cpu(desc->kstat_irqs->cnt, cpu));	/* [한국어] CPU 별 카운터. NMI 는 tot_count 를 올리지 않고, per-CPU 는 합계 자체가 무의미하다 */
	return sum;	/* [한국어] 합산 결과 */
}

/*
 * [한국어]
 * kstat_irqs - 모든 CPU 에서의 발생 횟수 합계
 *
 * @irq: 인터럽트 번호
 * @return: 합계, 서술자나 통계 영역이 없으면 0
 *
 * kstat_irqs_desc() 를 cpu_possible_mask 로 부르는 껍데기다.
 * online 이 아니라 possible 을 쓰는 것이 중요하다 — 잠시 오프라인인
 * CPU 에 쌓인 카운트도 세야 총계가 맞는다.
 *
 * static 이고 아래 kstat_irqs_usr() 만 부른다. 굳이 나눠 둔 이유는
 * RCU 잠금 책임을 분리하려는 것이다 — 이 함수는 조회와 합산만 하고,
 * 보호는 호출자가 한다.
 *
 * 실행 컨텍스트: RCU 읽기 구역 안이어야 한다.
 *
 * 호출 체인:
 *   kstat_irqs_usr() → [이 함수] → kstat_irqs_desc()
 */
static unsigned int kstat_irqs(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || !desc->kstat_irqs)	/* [한국어] 서술자나 통계 영역이 없는가 */
		return 0;	/* [한국어] "발생한 적 없음" 과 같은 표현 */
	return kstat_irqs_desc(desc, cpu_possible_mask);	/* [한국어] 가능한 모든 CPU. online 이 아닌 이유는 오프라인 CPU 에 쌓인 카운트도 총계에 들어가야 하기 때문이다 */
}

#ifdef CONFIG_GENERIC_IRQ_STAT_SNAPSHOT	/* [한국어] "특정 시점 이후 몇 번" 을 재는 기능. 인터럽트 폭주 검출기 등이 쓴다 */

/*
 * [한국어]
 * kstat_snapshot_irqs - 지금 이 CPU 의 모든 카운터를 기준점으로 기록한다
 *
 * @return: 없음
 *
 * 무엇을 위한 것인가: "지난 1 초 동안 이 인터럽트가 몇 번 왔는가" 를
 * 알고 싶을 때가 있다. 누적 카운터만으로는 두 시점의 값을 빼야 하는데,
 * 그 "이전 값" 을 어딘가 기록해야 한다. 이 함수가 그 기록을 남긴다.
 *
 * 각 카운터 옆에 ref 라는 짝 필드를 두고 현재 cnt 를 복사해 둔다.
 * 그러면 아래 kstat_get_irq_since_snapshot() 이 뺄셈 한 번으로 증가분을
 * 낸다. 호출자가 스냅숏 배열을 따로 관리하지 않아도 된다.
 *
 * this_cpu 계열만 쓰는 것에 주목: 이 CPU 의 카운터만 다룬다. 다른 CPU 의
 * 값을 읽으려면 동기화가 필요한데, 이 기능의 소비자는 자기 CPU 의
 * 인터럽트 빈도만 보면 되기 때문이다.
 *
 * 실행 컨텍스트: 선점이 꺼진 문맥. this_cpu 접근이 그것을 요구한다.
 *
 * 호출 체인:
 *   인터럽트 폭주 검출기 등 → [이 함수] → for_each_irq_desc()
 */
void kstat_snapshot_irqs(void)
{
	struct irq_desc *desc;	/* [한국어] 순회용 */
	unsigned int irq;	/* [한국어] 순회용 번호 */

	for_each_irq_desc(irq, desc) {	/* [한국어] 존재하는 모든 인터럽트 */
		if (!desc->kstat_irqs)	/* [한국어] 통계 영역이 없는가 */
			continue;	/* [한국어] 기록할 것이 없다 */
		this_cpu_write(desc->kstat_irqs->ref, this_cpu_read(desc->kstat_irqs->cnt));	/* [한국어] 현재 누적값을 기준점으로 복사한다. 이 CPU 것만 다루므로 동기화가 필요 없다 */
	}
}

/*
 * [한국어]
 * kstat_get_irq_since_snapshot - 스냅숏 이후의 증가분을 돌려준다
 *
 * @irq: 인터럽트 번호
 * @return: 마지막 kstat_snapshot_irqs() 이후 이 CPU 에서의 발생 횟수
 *
 * 위 스냅숏과 짝이다. 지금 값에서 기록해 둔 기준점을 뺀다.
 *
 * 뺄셈이 오버플로해도 괜찮다: 두 값 모두 unsigned int 이고, 카운터가
 * 한 바퀴 돌아 지금 값이 기준점보다 작아져도 unsigned 뺄셈의 모듈로
 * 성질 덕분에 올바른 증가분이 나온다. 증가분이 2^32 를 넘을 만큼
 * 오래 두지 않는다는 전제다.
 *
 * 실행 컨텍스트: 선점이 꺼진 문맥. 스냅숏을 찍은 CPU 와 같아야
 * 의미가 있다.
 *
 * 호출 체인:
 *   인터럽트 폭주 검출기 등 → [이 함수]
 */
unsigned int kstat_get_irq_since_snapshot(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || !desc->kstat_irqs)	/* [한국어] 서술자나 통계 영역이 없는가 */
		return 0;	/* [한국어] 증가분 0 으로 본다 */
	return this_cpu_read(desc->kstat_irqs->cnt) - this_cpu_read(desc->kstat_irqs->ref);	/* [한국어] 지금 값에서 기준점을 뺀다. unsigned 뺄셈이라 카운터가 한 바퀴 돌아도 올바른 증가분이 나온다 */
}

#endif	/* [한국어] CONFIG_GENERIC_IRQ_STAT_SNAPSHOT 분기의 끝 */

/**
 * kstat_irqs_usr - Get the statistics for an interrupt from thread context
 * @irq:	The interrupt number
 *
 * Returns the sum of interrupt counts on all cpus since boot for @irq.
 *
 * It uses rcu to protect the access since a concurrent removal of an
 * interrupt descriptor is observing an rcu grace period before
 * delayed_free_desc()/irq_kobj_release().
 */
/*
 * [한국어]
 * kstat_irqs_usr - 사용자 공간에 보여 줄 발생 횟수 합계
 *
 * @irq: 인터럽트 번호
 * @return: 부팅 이후 모든 CPU 에서의 발생 횟수 합계
 *
 * 이름의 usr 가 "사용자 공간 요청으로 프로세스 문맥에서 불린다" 는
 * 뜻이다. 그 문맥의 특징은 인터럽트가 언제든 끼어들 수 있고, 다른
 * CPU 가 이 인터럽트를 해제하고 있을 수도 있다는 것이다.
 *
 * RCU 로 감싸는 이유가 정확히 그것이다. free_desc() 가 서술자를
 * 트리에서 뺀 뒤 call_rcu 로 해제를 예약하므로, RCU 읽기 구역 안에
 * 있으면 그 사이에 서술자가 사라지지 않는다. 원본 주석이 그 사슬을
 * 짚는다 — 해제는 delayed_free_desc() 와 irq_kobj_release() 를 거치는데,
 * 그 앞에 유예 기간이 있다.
 *
 * 위 kstat_irqs_cpu() 와 대조적이다. 그쪽은 호출자에게 보호를 떠넘기지만
 * (irq_lock_sparse 로), 이쪽은 스스로 RCU 를 잡는다. 뮤텍스를 잡을 수
 * 없는 호출자도 쓸 수 있게 하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   fs/proc/stat.c (/proc/stat 의 intr 줄) → [이 함수] → kstat_irqs()
 */
unsigned int kstat_irqs_usr(unsigned int irq)
{
	unsigned int sum;	/* [한국어] 합산 결과 */

	rcu_read_lock();	/* [한국어] 이 구역 안에서는 서술자가 해제되지 않는다. free_desc 의 call_rcu 가 이 구역이 끝날 때까지 기다린다 */
	sum = kstat_irqs(irq);	/* [한국어] 조회와 합산 */
	rcu_read_unlock();	/* [한국어] 구역을 벗어난다. 이 뒤로 서술자가 해제될 수 있으므로 포인터를 들고 나오면 안 된다 — sum 은 값이라 안전하다 */
	return sum;	/* [한국어] 합계 */
}

#ifdef CONFIG_LOCKDEP	/* [한국어] lockdep 이 없는 빌드에서는 클래스라는 개념 자체가 없다 */
/*
 * [한국어]
 * __irq_set_lockdep_class - 서술자 락에 별도 lockdep 클래스를 건다
 *
 * @irq:           대상 인터럽트 번호
 * @lock_class:    desc->lock 에 붙일 클래스 키
 * @request_class: desc->request_mutex 에 붙일 클래스 키
 * @return:        없음
 *
 * 파일 위쪽에서 모든 서술자 락을 한 클래스(irq_desc_lock_class)로
 * 묶었는데, 그 예외를 만드는 함수다.
 *
 * 왜 예외가 필요한가: I2C 나 SPI 로 붙은 GPIO 확장 칩 같은 중첩
 * 컨트롤러에서, 자식 인터럽트의 서술자 락은 부모 인터럽트의 서술자 락
 * 안에서 잡힌다. 모두 한 클래스면 lockdep 이 이것을 자기 자신에 대한
 * 재귀 데드락으로 오해한다. 실제로는 부모와 자식의 순서가 항상 같아
 * 안전하다.
 *
 * 서술자가 없으면 조용히 아무것도 하지 않는다. lockdep 설정은
 * 진단용이므로 실패를 알릴 필요가 없다.
 *
 * CONFIG_LOCKDEP 이 꺼지면 이 함수 자체가 없어지고, 호출부인
 * irq_set_lockdep_class() 인라인도 빈 몸이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 매핑·설정 단계.
 *
 * 호출 체인:
 *   irq_map_generic_chip() / irq_setup_generic_chip()
 *   (kernel/irq/generic-chip.c) → irq_set_lockdep_class() → [이 함수]
 */
void __irq_set_lockdep_class(unsigned int irq, struct lock_class_key *lock_class,
			     struct lock_class_key *request_class)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (desc) {	/* [한국어] 존재하는 인터럽트인가. 없으면 조용히 넘어간다 — 진단용 설정이라 실패를 알릴 필요가 없다 */
		lockdep_set_class(&desc->lock, lock_class);	/* [한국어] 서술자 락을 공용 클래스에서 떼어 낸다. 중첩이 재귀로 오해되지 않게 한다 */
		lockdep_set_class(&desc->request_mutex, request_class);	/* [한국어] 요청 뮤텍스도 따로. 두 락이 각각 중첩되므로 클래스도 각각 필요하다 */
	}
}
EXPORT_SYMBOL_GPL(__irq_set_lockdep_class);	/* [한국어] 중첩 컨트롤러 드라이버가 모듈로 빌드될 수 있다 */
#endif	/* [한국어] CONFIG_LOCKDEP 분기의 끝 */
