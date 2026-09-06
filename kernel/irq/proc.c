// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains the /proc/irq/ handling code.
 */
/*
 * [한국어 설명] /proc/irq/ 와 /proc/interrupts 를 만드는 코드 (proc.c)
 *
 * === 파일의 역할 ===
 * 사용자 공간이 인터럽트를 들여다보고 친화도를 바꾸는 통로를 만든다.
 * 두 가지를 제공한다.
 *
 *   /proc/irq/<번호>/ 디렉터리 — 인터럽트마다 하나씩. 친화도를 읽고 쓸 수
 *     있고, NUMA 노드와 오탐 통계도 보인다.
 *   /proc/interrupts — 모든 인터럽트의 CPU 별 발생 횟수를 표로 보여 준다.
 *     시스템 진단에서 가장 먼저 보는 파일이다.
 *
 * 이 파일에서 가장 중요한 것은 사용자가 인터럽트 친화도를 바꿀 수 있는
 * 유일한 표준 경로라는 점이다. irqbalance 데몬이 그것으로 부하를 분산하고,
 * 실시간 구성에서는 특정 CPU 를 인터럽트로부터 격리하는 데 쓴다.
 *
 * 파일 상단의 긴 영어 주석이 이 파일의 동기화 규칙을 설명한다. 두 경로가
 * 서로 다른 방식으로 보호된다:
 *   /proc/irq/N/ 읽기·쓰기 — procfs 자체가 보호한다. remove_proc_entry() 가
 *     새 접근을 막고 진행 중인 것을 기다리므로, 그 사이 서술자가 사라지지 않는다.
 *   /proc/interrupts 읽기 — 그런 보호가 없다. 그래서 조회와 접근을
 *     sparse_irq_lock 이나 RCU 로 직접 보호해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간과 인터럽트 코어 사이의 인터페이스다:
 *
 *   부팅
 *     ↓ init_irq_proc()               ← **이 파일**
 *   /proc/irq 디렉터리 생성, 기존 인터럽트 등록
 *     ↓ 이후 핸들러가 등록될 때마다
 *   register_irq_proc()/register_handler_proc()  ← **이 파일**
 *
 *   사용자가 친화도를 바꿀 때:
 *     echo 3 > /proc/irq/42/smp_affinity
 *       ↓
 *     write_irq_affinity()            ← **이 파일**
 *       ↓
 *     irq_set_affinity() (manage.c) → chip->irq_set_affinity
 *
 *   사용자가 통계를 볼 때:
 *     cat /proc/interrupts
 *       ↓ fs/proc/interrupts.c 의 seq_file 골격
 *     show_interrupts()               ← **이 파일**
 *
 * 실행 컨텍스트: 전부 프로세스 문맥. 할당과 사용자 공간 복사로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   manage.c 의 irq_set_affinity(), irq_can_set_affinity_usr().
 *   internals.h 의 irq_settings_is_hidden(), irq_desc_is_chained().
 *   procfs 기반과 seq_file 골격.
 *
 * 이 파일에 의존하는 곳:
 *   irqdesc.c — 서술자를 만들고 없앨 때 항목을 등록·해제한다.
 *   manage.c — 핸들러를 등록·해제할 때 핸들러 항목을 다룬다.
 *   fs/proc/interrupts.c — /proc/interrupts 의 seq_file 이 show_interrupts 를 부른다.
 *
 * === 주요 함수/구조체 요약 ===
 * root_irq_dir            — /proc/irq 디렉터리.
 * show_irq_affinity()     — 네 종류의 친화도 파일을 하나의 함수로 출력한다.
 * write_irq_affinity()    — 사용자가 쓴 마스크를 파싱해 적용한다.
 * irq_select_affinity_usr()— 빈 마스크를 썼을 때의 처리. 아키텍처마다 다르다.
 * default_affinity_*()    — /proc/irq/default_smp_affinity 의 읽기·쓰기.
 * name_unique()           — 핸들러 이름이 중복되지 않는지 확인한다.
 * register_irq_proc()     — 인터럽트 하나의 디렉터리와 파일들을 만든다.
 * unregister_irq_proc()   — 그것을 지운다.
 * init_irq_proc()         — 부팅 때 /proc/irq 를 만든다.
 * show_interrupts()       — /proc/interrupts 의 한 줄을 출력한다.
 *
 * 대부분의 코드가 CONFIG_SMP 안에 있다는 점이 이 파일의 성격을 말해 준다 —
 * 친화도는 CPU 가 여럿일 때만 의미가 있다.
 */

#include <linux/irq.h>	/* [한국어] struct irq_desc 와 irqd_* 접근자 */
#include <linux/gfp.h>	/* [한국어] GFP_KERNEL — cpumask 임시 할당 */
#include <linux/proc_fs.h>	/* [한국어] proc_mkdir, proc_create_data 등 procfs API */
#include <linux/seq_file.h>	/* [한국어] seq_printf 와 single_open 골격 */
#include <linux/interrupt.h>	/* [한국어] irq_set_affinity() 등 공개 API */
#include <linux/kernel_stat.h>	/* [한국어] 인터럽트 횟수 통계 */
#include <linux/mutex.h>	/* [한국어] register_irq_proc 의 중복 등록 방지 뮤텍스 */
#include <linux/string.h>	/* [한국어] strscpy, strcmp — 핸들러 이름 처리 */

#include "internals.h"	/* [한국어] irq_settings_is_hidden, irq_can_set_affinity_usr 등 코어 내부 */

/*
 * Access rules:
 *
 * procfs protects read/write of /proc/irq/N/ files against a
 * concurrent free of the interrupt descriptor. remove_proc_entry()
 * immediately prevents new read/writes to happen and waits for
 * already running read/write functions to complete.
 *
 * We remove the proc entries first and then delete the interrupt
 * descriptor from the radix tree and free it. So it is guaranteed
 * that irq_to_desc(N) is valid as long as the read/writes are
 * permitted by procfs.
 *
 * The read from /proc/interrupts is a different problem because there
 * is no protection. So the lookup and the access to irqdesc
 * information must be protected by sparse_irq_lock.
 */
/* [한국어] (위 영어 주석에 이어) 이 파일의 동기화 규칙.
 *
 * 문제: 사용자가 /proc/irq/42/smp_affinity 를 읽는 동안 42번 인터럽트가
 * 해제되어 서술자가 사라지면, 읽던 코드가 해제된 메모리를 만진다.
 *
 * 해법이 두 경로에서 다르다.
 *
 * /proc/irq/N/ 파일들은 procfs 가 알아서 보호한다. remove_proc_entry() 가
 * 새 접근을 즉시 막고 진행 중인 것이 끝나기를 기다린다. 그리고 커널은
 * proc 항목을 먼저 지운 뒤에야 서술자를 트리에서 빼고 해제한다. 그래서
 * procfs 가 접근을 허용하는 동안은 irq_to_desc(N) 이 반드시 유효하다.
 * 이 파일의 함수들이 서술자 널 검사를 하지 않는 것이 그 보장에 기댄 것이다.
 *
 * /proc/interrupts 는 그 보호를 받지 못한다. 특정 인터럽트의 파일이 아니라
 * 전체를 훑는 것이라, procfs 가 어느 서술자를 보호해야 할지 알 수 없다.
 * 그래서 아래 show_interrupts() 가 RCU 로 직접 보호한다. */
static struct proc_dir_entry *root_irq_dir;	/* [한국어] /proc/irq 디렉터리. NULL 이면 procfs 초기화 전이거나 생성에 실패한 상태라, 항목 등록이 조용히 건너뛰어진다 */

/* [한국어] 아래 대부분이 SMP 전용이다. 친화도는 CPU 가 여럿일 때만 의미가 있다. */
#ifdef CONFIG_SMP	/* [한국어] 다중 프로세서 빌드 */

/* [한국어] 친화도 파일 네 종류를 구분하는 값.
 *
 * 두 축의 조합이다:
 *   AFFINITY vs EFFECTIVE — 요청된 집합인가, 하드웨어가 실제로 고른 것인가.
 *   기본 vs _LIST         — 비트마스크 형식인가, "0-3,7" 목록 형식인가.
 *
 * 네 파일이 거의 같은 일을 하므로 하나의 함수로 처리하고, 이 값으로 갈래를
 * 나눈다. */
enum {
	AFFINITY,
	/* [한국어] /proc/irq/N/smp_affinity — 요청된 친화도를 비트마스크로.
	 * 설정자: 아래 irq_affinity_proc_show 가 넘긴다.
	 * 읽는 자: show_irq_affinity 의 두 switch.
	 * 출력 형식: "%*pb" — "00000003" 같은 16진 비트마스크. */

	AFFINITY_LIST,
	/* [한국어] /proc/irq/N/smp_affinity_list — 같은 값을 목록 형식으로.
	 * 설정자/읽는 자: 위와 같다.
	 * 출력 형식: "%*pbl" — "0-1" 같은 사람이 읽기 쉬운 형태.
	 * 왜 두 형식인가: 비트마스크는 CPU 가 수백 개인 기계에서 아주 길어진다.
	 *   목록 형식이 나중에 추가되었고, 옛 도구를 위해 둘 다 유지한다. */

	EFFECTIVE,
	/* [한국어] /proc/irq/N/effective_affinity — 하드웨어가 실제로 고른 CPU.
	 * 설정자: irq_effective_aff_proc_show.
	 * 읽는 자: show_irq_affinity.
	 * 위 AFFINITY 와의 차이: 사용자가 여러 CPU 를 요청해도 컨트롤러가
	 *   하나만 고를 수 있다. 그 실제 결과가 이것이며, 친화도 문제를
	 *   진단할 때 둘을 비교하는 것이 첫 단계다. */

	EFFECTIVE_LIST,
	/* [한국어] 위 EFFECTIVE 의 목록 형식 판.
	 * 설정자: irq_effective_aff_list_proc_show.
	 * 읽는 자: show_irq_affinity.
	 * 네 값이 (요청/실제) × (마스크/목록) 의 조합을 이룬다. */
};

/*
 * [한국어]
 * show_irq_affinity - 네 종류의 친화도 파일을 하나의 함수로 출력한다
 *
 * @type:   위 enum 중 하나. 어느 파일인지 가른다.
 * @m:      출력할 seq_file. private 에 인터럽트 번호가 들어 있다.
 * @return: 0 이면 성공, -EINVAL 이면 지원하지 않는 종류다.
 *
 * switch 를 두 번 쓰는 구조가 특이하다. 첫 번째로 어느 마스크를 읽을지
 * 정하고, 두 번째로 어떤 형식으로 찍을지 정한다. 네 조합을 두 축으로
 * 나눠 처리하는 것이다.
 *
 * AFFINITY 쪽에서 pending 마스크를 대신 쓰는 것이 중요하다. 미뤄 둔
 * 친화도 변경이 있으면 사용자가 방금 요청한 값이 그쪽에 있고, 현재
 * affinity 는 아직 옛 값이다. 사용자에게는 요청한 값을 보여 주는 편이
 * 자연스러우므로 pending 을 우선한다.
 *
 * EFFECTIVE 쪽의 #ifdef 배치가 미묘하다. break 가 #ifdef 안에 있어서,
 * 그 기능을 뺀 빌드에서는 case 가 아래 default 로 흘러가 -EINVAL 이 된다.
 * 그런 빌드에서는 애초에 그 파일이 만들어지지 않으므로 도달하지 않는다.
 *
 * 서술자 널 검사가 없는 것에 주의: 위 동기화 규칙 덕분에 procfs 가
 * 이 함수를 부르는 동안 서술자가 반드시 유효하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 서술자 락을 잡는다.
 *
 * 호출 체인:
 *   cat /proc/irq/N/smp_affinity → single_open 골격
 *     → irq_affinity_proc_show() → [이 함수]
 */
static int show_irq_affinity(int type, struct seq_file *m)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);	/* [한국어] 파일 생성 때 담아 둔 인터럽트 번호. 위 동기화 규칙 덕에 널 검사가 필요 없다 */
	const struct cpumask *mask;	/* [한국어] 출력할 마스크. 아래 첫 switch 가 정한다 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 마스크를 읽는 동안 바뀌지 않게 한다 */

	switch (type) {	/* [한국어] 첫 번째 switch — 어느 마스크를 읽을 것인가 */
	case AFFINITY:	/* [한국어] 폴백(fall-through) — 비트마스크 형식과 목록 형식이 같은 마스크를 읽으므로 아래 case 로 그대로 흘려보낸다 */
	case AFFINITY_LIST:	/* [한국어] 두 형식이 같은 마스크를 쓴다 */
		mask = desc->irq_common_data.affinity;	/* [한국어] 요청된 친화도 */
		if (irq_move_pending(&desc->irq_data))	/* [한국어] 미뤄 둔 변경이 있는가 */
			mask = irq_desc_get_pending_mask(desc);	/* [한국어] 사용자가 방금 요청한 값을 우선해 보여 준다. 현재 affinity 는 아직 옛 값이다 */
		break;
	case EFFECTIVE:	/* [한국어] 폴백 — 유효 친화도도 두 형식이 같은 마스크를 공유한다 */
	case EFFECTIVE_LIST:	/* [한국어] 하드웨어가 실제로 고른 CPU */
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 따로 추적하는 빌드 */
		mask = irq_data_get_effective_affinity_mask(&desc->irq_data);	/* [한국어] 실제 결과 */
		break;	/* [한국어] break 가 #ifdef 안에 있다 — 그 기능이 없으면 아래 default 로 흘러 -EINVAL 이 된다. 그런 빌드에서는 이 파일이 만들어지지 않아 도달하지 않는다 */
#endif
	default:	/* [한국어] 알 수 없는 종류이거나 위 #ifdef 를 통과한 경우 */
		return -EINVAL;	/* [한국어] 알 수 없는 종류를 요청받았다. seq_file 의 show 콜백이므로 이 값이 read(2) 의 반환값으로 사용자 공간까지 그대로 올라간다 */
	}

	switch (type) {	/* [한국어] 두 번째 switch — 어떤 형식으로 찍을 것인가 */
	case AFFINITY_LIST:	/* [한국어] 폴백 — 요청 친화도와 유효 친화도가 같은 출력 형식을 쓴다 */
	case EFFECTIVE_LIST:	/* [한국어] 목록 형식 */
		seq_printf(m, "%*pbl\n", cpumask_pr_args(mask));	/* [한국어] "0-3,7" 처럼. CPU 가 많은 기계에서 읽기 좋다 */
		break;
	case AFFINITY:	/* [한국어] 폴백 — 위와 같은 이유로 두 종류가 한 형식을 공유한다 */
	case EFFECTIVE:	/* [한국어] 비트마스크 형식 */
		seq_printf(m, "%*pb\n", cpumask_pr_args(mask));	/* [한국어] "00000003" 처럼. 옛 도구가 이 형식을 기대한다 */
		break;
	}
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_affinity_hint_proc_show - /proc/irq/N/affinity_hint 를 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      seq_file 순회 위치. 쓰이지 않는다.
 * @return: 0 이면 성공, -ENOMEM 이면 임시 마스크 할당 실패.
 *
 * affinity_hint 가 무엇인가: 드라이버가 "이 인터럽트는 이 CPU 들에 두는
 * 것이 좋다" 고 알려 주는 값이다. 커널은 그것을 강제하지 않고, irqbalance
 * 같은 사용자 공간 도구가 참고한다.
 *
 * 왜 힌트인가: 드라이버는 자기 큐가 어느 CPU 의 것인지 안다. 그 정보를
 * 커널이 강제하면 사용자의 정책과 충돌하므로, 알려 주기만 하고 결정은
 * 사용자에게 맡긴다.
 *
 * 임시 마스크를 할당해 복사하는 것이 위 show_irq_affinity 와 다른 점이다.
 * 힌트가 NULL 일 수 있어(대부분의 드라이버는 설정하지 않는다) 그 경우
 * 빈 마스크를 보여 줘야 하는데, 직접 출력하면 널 역참조가 된다.
 *
 * 락 밖에서 출력하는 것에도 이유가 있다. seq_printf 는 잠들 수 있어
 * raw_spinlock 을 쥔 채 부를 수 없다. 그래서 락 안에서는 복사만 하고
 * 출력은 밖에서 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   cat /proc/irq/N/affinity_hint → proc_create_single_data 골격 → [이 함수]
 */
static int irq_affinity_hint_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);	/* [한국어] 대상 서술자 */
	cpumask_var_t mask;	/* [한국어] 임시 마스크. 큰 시스템에서는 힙에, 작은 시스템에서는 스택에 놓인다 */

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))	/* [한국어] 0 으로 채워 잡는다 — 힌트가 없으면 그 빈 마스크가 그대로 출력된다 */
		return -ENOMEM;

	scoped_guard(raw_spinlock_irq, &desc->lock) {	/* [한국어] 복사하는 동안만 락을 잡는다 */
		if (desc->affinity_hint)	/* [한국어] 드라이버가 힌트를 설정했는가. 대부분은 NULL 이다 */
			cpumask_copy(mask, desc->affinity_hint);	/* [한국어] 복사만 한다. 출력은 락 밖에서 */
	}

	seq_printf(m, "%*pb\n", cpumask_pr_args(mask));	/* [한국어] 락 밖에서 출력한다 — seq_printf 가 잠들 수 있어 raw_spinlock 안에서 부를 수 없다 */
	free_cpumask_var(mask);	/* [한국어] 임시 마스크 반납 */
	return 0;	/* [한국어] seq_file 규약상 0 이 성공이다. 위 두 switch 가 이미 버퍼에 다 찍었고, 실제 사용자 공간 복사는 seq_read() 가 이 함수가 끝난 뒤에 한다 */
}

int no_irq_affinity;	/* [한국어] 친화도 변경을 시스템 전체에서 막는 스위치. 아키텍처 코드가 설정하며, 참이면 아래 write_irq_affinity 가 모든 쓰기를 -EPERM 으로 거절한다 */
/*
 * [한국어]
 * irq_affinity_proc_show - smp_affinity 파일의 출력 진입점
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: show_irq_affinity 의 결과.
 *
 * 위 공용 함수에 종류만 알려 주는 한 줄짜리 래퍼다. seq_file 골격이
 * 종류를 넘길 방법이 없어 파일마다 래퍼를 하나씩 두어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/N/smp_affinity → single_open 골격 → [이 함수]
 */
static int irq_affinity_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY, m);	/* [한국어] 요청된 친화도를 비트마스크 형식으로 */
}

/*
 * [한국어]
 * irq_affinity_list_proc_show - smp_affinity_list 파일의 출력 진입점
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: show_irq_affinity 의 결과.
 *
 * 위와 같은 마스크를 목록 형식으로 찍는다. 두 함수가 종류 상수만 다르다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/N/smp_affinity_list → single_open 골격 → [이 함수]
 */
static int irq_affinity_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY_LIST, m);	/* [한국어] 같은 마스크를 "0-3,7" 형식으로 */
}

/* [한국어] 사용자가 빈 마스크를 썼을 때의 처리. 아키텍처마다 다르다.
 *
 * 왜 빈 마스크를 특별 취급하는가: 사용자가 "아무 CPU 도 아님" 을 요청하면
 * 그 인터럽트는 갈 곳이 없어진다. 그것을 그대로 받아들이면 시스템이
 * 망가지므로, 대신 "커널이 알아서 고르라" 는 뜻으로 해석한다.
 *
 * 그 해석을 어떻게 구현할지가 아키텍처마다 다르며, 아래 두 판이 그 차이다. */
#ifndef CONFIG_AUTO_IRQ_AFFINITY	/* [한국어] 자동 선택 기능이 없는 아키텍처 — 오늘날 거의 전부 */
/*
 * [한국어]
 * irq_select_affinity_usr - (자동 선택 없음) 빈 마스크 요청을 거절한다
 *
 * @irq:    대상 인터럽트 번호. 쓰이지 않는다.
 * @return: 항상 -EINVAL.
 *
 * 아래 영어 주석이 거절하는 두 가지 이유를 설명한다.
 *
 * 이미 시작된 인터럽트라면: 그것은 이미 온라인 CPU 에 배정되어 있다.
 * 무작위로 옮길 이유가 없으므로, 사용자에게 "그 마스크는 무의미하다" 고
 * 알린다.
 *
 * 아직 시작되지 않았다면: 시작할 때 irq_setup_affinity() 가 어차피 온라인
 * CPU 를 고른다. 지금 무엇을 하든 그때 덮어써지므로 의미가 없다.
 *
 * 어느 쪽이든 할 일이 없어 -EINVAL 로 답한다. 아래 ALPHA 판이 실제로
 * 무언가를 하는 것과 대조된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   write_irq_affinity() → [이 함수]
 */
static inline int irq_select_affinity_usr(unsigned int irq)
{
	/*
	 * If the interrupt is started up already then this fails. The
	 * interrupt is assigned to an online CPU already. There is no
	 * point to move it around randomly. Tell user space that the
	 * selected mask is bogus.
	 *
	 * If not then any change to the affinity is pointless because the
	 * startup code invokes irq_setup_affinity() which will select
	 * a online CPU anyway.
	 */
	/* [한국어] (위 영어 주석) 두 경우 모두 할 일이 없다. 자세한 설명은
	 * 위 함수 주석에 정리했다. */
	return -EINVAL;	/* [한국어] 호출자가 이것을 보고 사용자에게 -EINVAL 을 돌려준다 */
}
#else
/* ALPHA magic affinity auto selector. Keep it for historical reasons. */
/*
 * [한국어] (위 영어 주석에 이어)
 * irq_select_affinity_usr - (ALPHA) 커널이 CPU 를 골라 준다
 *
 * @irq:    대상 인터럽트 번호.
 * @return: irq_select_affinity() 의 결과. 0 이면 성공.
 *
 * 원 주석이 "역사적 이유로 유지한다" 고 밝힌다. AUTO_IRQ_AFFINITY 를
 * 켜는 아키텍처는 사실상 ALPHA 뿐이며, 그 하드웨어는 인터럽트를 어느
 * CPU 에 배정할지 고르는 자체 규칙을 갖는다.
 *
 * 위 판이 거절하는 요청을 이쪽은 받아들여 실제로 CPU 를 고른다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   write_irq_affinity() → [이 함수] → irq_select_affinity() (manage.c)
 */
static inline int irq_select_affinity_usr(unsigned int irq)
{
	return irq_select_affinity(irq);	/* [한국어] 아키텍처의 규칙으로 CPU 를 고른다 */
}
#endif

/*
 * [한국어]
 * write_irq_affinity - 사용자가 쓴 친화도 마스크를 파싱해 적용한다
 *
 * @type:   0 이면 비트마스크 형식, 0 이 아니면 목록 형식으로 파싱한다.
 * @file:   쓰기 대상 파일. inode 에 인터럽트 번호가 들어 있다.
 * @buffer: 사용자가 쓴 문자열.
 * @count:  그 길이.
 * @pos:    파일 위치. 쓰이지 않는다.
 * @return: 소비한 바이트 수(성공), 또는 음수 오류.
 *
 * 이 파일에서 가장 중요한 함수다. 사용자가 인터럽트 친화도를 바꾸는
 * 유일한 표준 경로이며, irqbalance 데몬이 이것으로 부하를 분산한다.
 *
 * 세 단계를 밟는다:
 *   권한 확인 — 이 인터럽트의 친화도를 바꿔도 되는가.
 *   파싱 — 형식에 맞춰 문자열을 마스크로 옮긴다.
 *   적용 — 온라인 CPU 가 하나라도 있으면 설정하고, 없으면 특수 처리.
 *
 * 빈 마스크(온라인 CPU 가 하나도 없는 마스크)를 특별 취급하는 것이
 * 이 함수의 미묘한 부분이다. 아래 영어 주석이 그 이유를 밝힌다 — 인터럽트를
 * 완전히 꺼 버리는 것은 시스템을 못 쓰게 만드는 너무 쉬운 방법이다.
 *
 * 성공했을 때 count 를 돌려주는 것이 write 규약이다. 그보다 작은 값을
 * 돌려주면 사용자 공간이 나머지를 다시 쓰려 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 할당과 사용자 공간 복사로 잠들 수 있다.
 *
 * 호출 체인:
 *   echo 3 > /proc/irq/N/smp_affinity → proc_ops->proc_write
 *     → irq_affinity_proc_write() → [이 함수] → irq_set_affinity() (manage.c)
 */
static ssize_t write_irq_affinity(int type, struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int irq = (int)(long)pde_data(file_inode(file));	/* [한국어] 파일 생성 때 담아 둔 인터럽트 번호. 포인터로 저장되어 있어 두 번 형변환한다 */
	cpumask_var_t new_value;	/* [한국어] 파싱한 결과를 담을 마스크 */
	int err;	/* [한국어] 파싱과 적용의 결과 */

	if (!irq_can_set_affinity_usr(irq) || no_irq_affinity)	/* [한국어] 이 인터럽트가 변경을 허용하는가, 그리고 시스템 전체 스위치가 켜져 있지 않은가 */
		return -EPERM;	/* [한국어] 권한 없음. -EINVAL 이 아닌 것은 "값이 잘못" 이 아니라 "허용되지 않음" 이기 때문이다 */

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))	/* [한국어] 파싱 결과를 담을 마스크. 0 으로 채워 잡는다 */
		return -ENOMEM;

	if (type)	/* [한국어] 목록 형식인가 */
		err = cpumask_parselist_user(buffer, count, new_value);	/* [한국어] "0-3,7" 형태를 파싱한다 */
	else
		err = cpumask_parse_user(buffer, count, new_value);	/* [한국어] "00000003" 같은 16진 비트마스크를 파싱한다 */
	if (err)	/* [한국어] 파싱 실패 — 사용자가 잘못된 형식을 썼다 */
		goto free_cpumask;	/* [한국어] 마스크를 반납하고 그 오류를 돌려준다 */

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	/* [한국어] (위 영어 주석에 이어) 인터럽트를 완전히 꺼 버리는 것을 막는다.
	 *
	 * 왜 위험한가: 목적지가 하나도 없는 인터럽트는 영영 전달되지 않는다.
	 * 그것이 디스크 컨트롤러의 인터럽트라면 시스템이 그 자리에서 멈춘다.
	 * 원 주석의 표정 기호가 붙은 문장이 그 사고의 흔함을 말해 준다.
	 *
	 * 그렇다고 오류로 처리하지는 않는다. 빈 마스크를 "커널이 알아서
	 * 고르라" 는 요청으로 해석하는 것이 아래 특수 처리다. */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {	/* [한국어] 요청한 CPU 중 살아 있는 것이 하나도 없는가 */
		/*
		 * Special case for empty set - allow the architecture code
		 * to set default SMP affinity.
		 */
		/* [한국어] (위 영어 주석) 빈 집합은 "아키텍처가 기본값을 정하라" 는 뜻으로 읽는다.
		 *
		 * 대부분의 아키텍처에서 그 요청은 -EINVAL 로 거절된다(위
		 * irq_select_affinity_usr 의 기본 판). ALPHA 만 실제로 고른다. */
		err = irq_select_affinity_usr(irq) ? -EINVAL : count;	/* [한국어] 0 이 아니면 -EINVAL, 0 이면 성공으로 count 를 돌려준다 */
	} else {
		err = irq_set_affinity(irq, new_value);	/* [한국어] 실제로 적용한다. manage.c 가 chip 까지 전달한다 */
		if (!err)	/* [한국어] 성공했으면 */
			err = count;	/* [한국어] write 규약대로 소비한 바이트 수를 돌려준다. 더 작은 값을 주면 사용자 공간이 나머지를 다시 쓴다 */
	}

free_cpumask:	/* [한국어] 파싱 실패 시의 진입점. 성공 경로도 여기를 지난다 */
	free_cpumask_var(new_value);	/* [한국어] 임시 마스크 반납 */
	return err;	/* [한국어] 성공이면 count, 실패면 음수 */
}

/*
 * [한국어]
 * irq_affinity_proc_write - smp_affinity 파일의 쓰기 진입점
 *
 * @file/@buffer/@count/@pos: 위 공용 함수에 그대로 전달한다.
 * @return: 그 결과.
 *
 * type 에 0 을 넘겨 비트마스크 형식으로 파싱하게 한다. 아래 list 판과
 * 그 인자만 다르다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   echo > /proc/irq/N/smp_affinity → proc_ops->proc_write → [이 함수]
 */
static ssize_t irq_affinity_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(0, file, buffer, count, pos);	/* [한국어] 0 이라 비트마스크 형식으로 파싱한다 */
}

/*
 * [한국어]
 * irq_affinity_list_proc_write - smp_affinity_list 파일의 쓰기 진입점
 *
 * @file/@buffer/@count/@pos: 위 공용 함수에 그대로 전달한다.
 * @return: 그 결과.
 *
 * type 에 1 을 넘겨 목록 형식으로 파싱하게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   echo > /proc/irq/N/smp_affinity_list → proc_ops->proc_write → [이 함수]
 */
static ssize_t irq_affinity_list_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(1, file, buffer, count, pos);	/* [한국어] 1 이라 "0-3,7" 형식으로 파싱한다 */
}

/*
 * [한국어]
 * irq_affinity_proc_open - smp_affinity 파일을 열 때의 처리
 *
 * @inode:  그 파일의 inode. pde_data 에 인터럽트 번호가 들어 있다.
 * @file:   열린 파일.
 * @return: single_open 의 결과.
 *
 * 인터럽트 번호를 seq_file 의 private 로 넘긴다. 위 show 함수가 그것을
 * 꺼내 서술자를 찾는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   open("/proc/irq/N/smp_affinity") → proc_ops->proc_open → [이 함수]
 */
static int irq_affinity_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_proc_show, pde_data(inode));	/* [한국어] pde_data 의 인터럽트 번호가 show 의 m->private 로 전달된다 */
}

/*
 * [한국어]
 * irq_affinity_list_proc_open - smp_affinity_list 파일을 열 때의 처리
 *
 * @inode:  그 파일의 inode.
 * @file:   열린 파일.
 * @return: single_open 의 결과.
 *
 * 위와 같고 show 함수만 다르다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   open("/proc/irq/N/smp_affinity_list") → proc_ops->proc_open → [이 함수]
 */
static int irq_affinity_list_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_list_proc_show, pde_data(inode));	/* [한국어] 목록 형식 show 함수를 꽂는다 */
}

/* [한국어] smp_affinity 파일의 연산표.
 *
 * read/lseek/release 는 seq_file 골격의 기본 구현을 그대로 쓴다. open 이
 * single_open 으로 그 골격을 준비하므로 가능한 일이다.
 *
 * proc_ops 는 file_operations 와 별개의 타입이다. procfs 전용으로 나뉜
 * 것이며, 필요 없는 필드가 빠져 구조체가 작다. */
static const struct proc_ops irq_affinity_proc_ops = {
	.proc_open	= irq_affinity_proc_open,
	/* [한국어] 열 때. single_open 으로 seq_file 골격을 준비한다.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_read	= seq_read,
	/* [한국어] 읽기. seq_file 골격의 기본 구현.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_lseek	= seq_lseek,
	/* [한국어] 위치 이동. 역시 골격의 기본 구현.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_release	= single_release,
	/* [한국어] 닫을 때. single_open 이 잡은 버퍼를 반납한다.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs.
	 * single_open 과 짝이 맞아야 한다 — seq_release 를 쓰면 누수가 난다. */

	.proc_write	= irq_affinity_proc_write,
	/* [한국어] 쓰기 — 친화도를 바꾸는 통로다.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs.
	 * 이 항목이 있어 이 파일이 읽기 전용이 아니게 된다. 아래
	 *   register_irq_proc 이 권한을 0644 로 주는 것과 짝이다. */
};

/* [한국어] smp_affinity_list 파일의 연산표.
 *
 * 위와 완전히 같은 구조이며 open/write 만 목록 형식 판을 가리킨다. */
static const struct proc_ops irq_affinity_list_proc_ops = {
	.proc_open	= irq_affinity_list_proc_open,
	/* [한국어] 목록 형식의 open.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_read	= seq_read,
	/* [한국어] 골격의 기본 읽기.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_lseek	= seq_lseek,
	/* [한국어] 골격의 기본 위치 이동.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_release	= single_release,
	/* [한국어] 골격의 기본 해제.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_write	= irq_affinity_list_proc_write,
	/* [한국어] 목록 형식의 쓰기.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */
};

/* [한국어] 유효 친화도 파일들. 그 기능을 켠 빌드에만 있다.
 *
 * 읽기 전용이라 write 콜백이 없고, 그래서 proc_ops 구조체도 필요 없다 —
 * 아래 register_irq_proc 이 proc_create_single_data 로 간단히 만든다. */
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 추적하는 빌드 */
/*
 * [한국어]
 * irq_effective_aff_proc_show - effective_affinity 파일을 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: show_irq_affinity 의 결과.
 *
 * 하드웨어가 실제로 고른 CPU 를 비트마스크로 보여 준다. 사용자가 요청한
 * smp_affinity 와 다를 수 있으며, 그 차이가 친화도 문제 진단의 출발점이다.
 *
 * 읽기 전용인 이유: 이 값은 하드웨어가 정한 결과다. 사용자가 바꾸려면
 * smp_affinity 를 써야 하고, 그러면 컨트롤러가 다시 고른다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/N/effective_affinity → proc_create_single_data 골격 → [이 함수]
 */
static int irq_effective_aff_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE, m);	/* [한국어] 실제 결과를 비트마스크 형식으로 */
}

/*
 * [한국어]
 * irq_effective_aff_list_proc_show - effective_affinity_list 파일을 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: show_irq_affinity 의 결과.
 *
 * 위와 같은 값을 목록 형식으로 찍는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/N/effective_affinity_list → 골격 → [이 함수]
 */
static int irq_effective_aff_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE_LIST, m);	/* [한국어] 실제 결과를 "0-3,7" 형식으로 */
}
#endif

/*
 * [한국어]
 * default_affinity_show - /proc/irq/default_smp_affinity 를 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: 항상 0.
 *
 * 기본 친화도가 무엇인가: 새로 만들어지는 인터럽트에 적용할 초기 마스크다.
 * 개별 인터럽트의 파일이 아니라 시스템 전체의 설정이라, /proc/irq 바로
 * 아래에 있다.
 *
 * 언제 쓰는가: 실시간 구성에서 특정 CPU 를 인터럽트로부터 격리할 때다.
 * 부팅 초기에 이 값을 좁혀 두면 이후 만들어지는 모든 인터럽트가 그 범위
 * 안에 배정된다.
 *
 * 서술자를 찾지 않는 것이 위 함수들과 다르다 — 전역 변수 하나만 읽는다.
 * 그래서 락도 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/default_smp_affinity → single_open 골격 → [이 함수]
 */
static int default_affinity_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%*pb\n", cpumask_pr_args(irq_default_affinity));	/* [한국어] 전역 기본 마스크. 개별 서술자를 찾지 않으므로 락도 필요 없다 */
	return 0;	/* [한국어] 성공. 전역 마스크 한 줄만 찍으면 끝이다 */
}

/*
 * [한국어]
 * default_affinity_write - 기본 친화도를 바꾼다
 *
 * @file:   쓰기 대상 파일. 이 구현은 쓰지 않는다 — 전역 설정이라 대상이 하나뿐이다.
 * @buffer: 사용자가 쓴 문자열.
 * @count:  그 길이.
 * @ppos:   파일 위치. 쓰이지 않는다.
 * @return: 소비한 바이트 수(성공), 또는 음수 오류.
 *
 * 위 write_irq_affinity 와 구조가 비슷하지만 두 가지가 다르다.
 *
 *   권한 검사가 없다 — 개별 인터럽트가 아니라 전역 설정이라, 파일 권한
 *     (0644)만으로 root 만 쓸 수 있게 통제한다.
 *   빈 마스크를 특수 처리하지 않고 거절한다 — 기본값이 비어 있으면 이후
 *     만들어지는 모든 인터럽트가 갈 곳을 잃는다. 개별 인터럽트보다 파급이
 *     훨씬 커서 예외를 두지 않는다.
 *
 * 목록 형식 판이 없는 것도 다르다. 이 파일은 비트마스크 형식만 받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 할당과 사용자 공간 복사로 잠들 수 있다.
 *
 * 호출 체인:
 *   echo > /proc/irq/default_smp_affinity → proc_ops->proc_write → [이 함수]
 */
static ssize_t default_affinity_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	cpumask_var_t new_value;	/* [한국어] 파싱한 결과 */
	int err;	/* [한국어] 파싱 결과 */

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))	/* [한국어] 임시 마스크 */
		return -ENOMEM;

	err = cpumask_parse_user(buffer, count, new_value);	/* [한국어] 비트마스크 형식만 받는다. 목록 형식 판이 없다 */
	if (err)	/* [한국어] 파싱 실패 */
		goto out;

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	/* [한국어] (위 영어 주석) 위 write_irq_affinity 와 같은 방어인데,
	 * 여기서는 예외 없이 거절한다.
	 *
	 * 왜 더 엄격한가: 개별 인터럽트의 빈 마스크는 그 하나만 못 쓰게
	 * 되지만, 기본값이 비면 이후 만들어지는 모든 인터럽트가 갈 곳을 잃는다.
	 * 파급이 훨씬 커서 "아키텍처가 알아서 고르라" 는 해석을 두지 않는다. */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {	/* [한국어] 살아 있는 CPU 가 하나도 없는가 */
		err = -EINVAL;	/* [한국어] 예외 없이 거절한다 */
		goto out;	/* [한국어] 아래 out: 으로 뛰어 임시 마스크를 반납하고 err 를 반환한다. 여기서 바로 return 하면 new_value 가 샌다 */
	}

	cpumask_copy(irq_default_affinity, new_value);	/* [한국어] 전역 기본값을 갱신한다. 이미 만들어진 인터럽트에는 영향이 없고, 이후 만들어지는 것들에만 적용된다 */
	err = count;	/* [한국어] write 규약대로 소비한 바이트 수 */

out:	/* [한국어] 성공과 실패 경로가 여기서 합류한다 */
	free_cpumask_var(new_value);	/* [한국어] 임시 마스크 반납 */
	return err;	/* [한국어] 성공이면 소비한 바이트 수(count), 실패면 음수 errno. procfs write 규약 그대로다 */
}

/*
 * [한국어]
 * default_affinity_open - 기본 친화도 파일을 열 때의 처리
 *
 * @inode:  그 파일의 inode.
 * @file:   열린 파일.
 * @return: single_open 의 결과.
 *
 * pde_data 를 넘기지만 위 show 함수는 그것을 쓰지 않는다 — 전역 변수를
 * 읽으므로 문맥이 필요 없다. 형식을 맞춘 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   open("/proc/irq/default_smp_affinity") → proc_ops->proc_open → [이 함수]
 */
static int default_affinity_open(struct inode *inode, struct file *file)
{
	return single_open(file, default_affinity_show, pde_data(inode));	/* [한국어] show 가 쓰지 않지만 형식을 맞춰 넘긴다 */
}

/* [한국어] 기본 친화도 파일의 연산표.
 *
 * 위 두 표와 같은 구조다. 이 파일이 /proc/irq 바로 아래에 있다는 점만
 * 다르며, 아래 register_default_affinity_proc() 이 만든다. */
static const struct proc_ops default_affinity_proc_ops = {
	.proc_open	= default_affinity_open,
	/* [한국어] 열 때.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_read	= seq_read,
	/* [한국어] 골격의 기본 읽기.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_lseek	= seq_lseek,
	/* [한국어] 골격의 기본 위치 이동.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_release	= single_release,
	/* [한국어] 골격의 기본 해제.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs. */

	.proc_write	= default_affinity_write,
	/* [한국어] 기본 친화도를 바꾸는 통로.
	 * 설정자: 이 정적 초기화. 읽는 자: procfs.
	 * 이 파일도 0644 권한으로 만들어져 root 만 쓸 수 있다. */
};

/*
 * [한국어]
 * irq_node_proc_show - /proc/irq/N/node 를 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: 항상 0.
 *
 * 이 인터럽트의 서술자가 놓인 NUMA 노드를 보여 준다.
 *
 * 무엇에 쓰는가: 인터럽트를 처리하는 CPU 와 그 자료가 같은 노드에 있어야
 * 접근이 빠르다. 어긋나 있으면 성능 문제의 단서가 된다. 성능 조율 도구가
 * 이 값과 친화도를 함께 보고 배치를 판단한다.
 *
 * 락을 잡지 않는 것에 주의: 노드는 서술자를 만들 때 정해지고 이후 바뀌지
 * 않으므로 경쟁이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/irq/N/node → proc_create_single_data 골격 → [이 함수]
 */
static int irq_node_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);	/* [한국어] 대상 서술자 */

	seq_printf(m, "%d\n", irq_desc_get_node(desc));	/* [한국어] 노드는 생성 때 정해지고 바뀌지 않아 락이 필요 없다 */
	return 0;	/* [한국어] 성공. 노드 번호 한 줄이 전부다 */
}
#endif

/*
 * [한국어]
 * irq_spurious_proc_show - /proc/irq/N/spurious 를 출력한다
 *
 * @m:      출력할 seq_file.
 * @v:      순회 위치. 쓰이지 않는다.
 * @return: 항상 0.
 *
 * 오탐 감지의 통계를 보여 준다. spurious.c 가 유지하는 세 값이다:
 *   count          — 이 선에 들어온 인터럽트의 누적 수(미처리가 있을 때만 센다).
 *   unhandled      — 그중 아무도 처리하지 않은 수.
 *   last_unhandled — 마지막 미처리로부터 얼마나 지났는가.
 *
 * 언제 보는가: "인터럽트가 꺼졌다" 는 문제를 만났을 때다. unhandled 가
 * count 에 가까우면 그 선이 곧 "nobody cared" 로 꺼질 것이고, 이미
 * 꺼졌다면 왜 그랬는지 여기서 확인할 수 있다.
 *
 * 이 파일은 CONFIG_SMP 밖에 있다 — 위 친화도 파일들과 달리 단일
 * 프로세서에서도 의미가 있기 때문이다.
 *
 * last_unhandled 를 밀리초로 바꿔 찍는 것이 사용자를 위한 배려다.
 * jiffies 는 HZ 설정에 따라 뜻이 달라 사람이 해석하기 어렵다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다 — 통계라 순간의 값이면 충분하다.
 *
 * 호출 체인:
 *   cat /proc/irq/N/spurious → proc_create_single_data 골격 → [이 함수]
 */
static int irq_spurious_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);	/* [한국어] 대상 서술자 */

	seq_printf(m, "count %u\n" "unhandled %u\n" "last_unhandled %u ms\n",	/* [한국어] 문자열 리터럴 셋이 컴파일 시 하나로 이어진다. 세 줄로 나눠 쓴 것은 읽기 위해서다 */
		   desc->irq_count, desc->irqs_unhandled,	/* [한국어] 누적 횟수와 그중 미처리 수. 둘이 가까우면 곧 꺼질 선이다 */
		   jiffies_to_msecs(desc->last_unhandled));	/* [한국어] 밀리초로 바꿔 찍는다. jiffies 는 HZ 에 따라 뜻이 달라 사람이 해석하기 어렵다 */
	return 0;	/* [한국어] 성공. 세 통계값을 한 번의 seq_printf 로 찍었다 */
}

#define MAX_NAMELEN 128	/* [한국어] 핸들러 이름의 최대 길이. 아래 register_handler_proc 이 이 크기의 스택 버퍼를 쓴다. 파일 끝에서 #undef 하고 다른 값으로 다시 정의하므로 유효 범위가 좁다 */

/*
 * [한국어]
 * name_unique - 이 핸들러 이름이 같은 인터럽트 안에서 유일한지 확인한다
 *
 * @irq:        대상 인터럽트 번호.
 * @new_action: 새로 등록하려는 핸들러.
 * @return:     유일하면 참, 이미 같은 이름이 있으면 거짓.
 *
 * 왜 필요한가: 아래 register_handler_proc 이 핸들러 이름으로 디렉터리를
 * 만든다. 공유 인터럽트에 같은 이름의 드라이버가 둘 붙으면 디렉터리
 * 이름이 충돌한다.
 *
 * 실제로 그런 경우가 있는가: 있다. 같은 드라이버가 여러 장치를 다루면서
 * 같은 이름으로 요청하면(예: "eth0" 대신 "e1000" 을 쓰는 경우) 충돌한다.
 * 그때는 조용히 디렉터리를 만들지 않는다.
 *
 * new_action 자신을 비교에서 제외하는 것이 중요하다. 이 함수가 불릴 때
 * 그 action 은 이미 목록에 들어 있으므로, 제외하지 않으면 언제나 자기
 * 자신과 이름이 같아 거짓이 나온다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 서술자 락을 잡는다.
 *
 * 호출 체인:
 *   register_handler_proc() → [이 함수]
 */
static bool name_unique(unsigned int irq, struct irqaction *new_action)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irqaction *action;	/* [한국어] 목록을 훑을 커서 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 목록을 훑는 동안 바뀌지 않게 한다 */
	for_each_action_of_desc(desc, action) {	/* [한국어] 이 선에 붙은 모든 핸들러 */
		if ((action != new_action) && action->name &&	/* [한국어] 자기 자신은 제외한다 — 이미 목록에 들어 있어 비교하면 언제나 같다 */
		    !strcmp(new_action->name, action->name))	/* [한국어] 이름이 같은가 */
			return false;	/* [한국어] 충돌. guard 가 락을 풀어 준다 */
	}
	return true;	/* [한국어] 유일하다 */
}

/*
 * [한국어]
 * register_handler_proc - /proc/irq/N/<핸들러이름>/ 디렉터리를 만든다
 *
 * @irq:    대상 인터럽트 번호.
 * @action: 등록된 핸들러.
 *
 * 공유 인터럽트에서 누가 그 선을 쓰고 있는지 보여 주는 항목이다.
 * /proc/interrupts 의 마지막 열에도 이름이 나오지만, 이쪽은 디렉터리라
 * 나중에 핸들러별 항목을 더할 여지가 있다.
 *
 * 실제로 그 안에 파일이 만들어지지는 않는다 — 빈 디렉터리다. 역사적으로
 * 핸들러별 통계를 두려던 자리이며, 지금은 존재 자체가 정보다.
 *
 * 네 가지 조건 중 하나라도 어긋나면 조용히 물러난다:
 *   desc->dir 이 없다 — 인터럽트 디렉터리가 아직 없다.
 *   action->dir 이 이미 있다 — 중복 등록이다.
 *   이름이 없다 — 만들 디렉터리 이름이 없다.
 *   이름이 중복이다 — 위 name_unique 참고.
 *
 * 이름을 스택 버퍼에 복사하는 이유: proc_mkdir 이 이름을 복사해 두므로
 * 사실 원본을 그대로 넘겨도 된다. 길이를 자르는 안전장치로 남아 있다.
 *
 * 실행 컨텍스트: 인터럽트 요청 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   request_irq() → __setup_irq() (kernel/irq/manage.c) → [이 함수]
 */
void register_handler_proc(unsigned int irq, struct irqaction *action)
{
	char name[MAX_NAMELEN];	/* [한국어] 이름 사본. 128바이트로 잘라 넘긴다 */
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc->dir || action->dir || !action->name || !name_unique(irq, action))	/* [한국어] 네 조건 중 하나라도 어긋나면 만들지 않는다. 전부 조용히 넘어가는 것이 이 진단 기능의 성격이다 */
		return;

	strscpy(name, action->name);	/* [한국어] 길이를 자르며 복사한다. strcpy 와 달리 넘치지 않고 널 종료를 보장한다 */

	/* create /proc/irq/1234/handler/ */
	/* [한국어] (위 영어 주석) 핸들러 이름의 디렉터리를 만든다.
	 * 안에 파일은 없다 — 역사적으로 핸들러별 통계를 두려던 자리이며,
	 * 지금은 존재 자체가 "이 드라이버가 이 선을 쓰고 있다" 는 정보다. */
	action->dir = proc_mkdir(name, desc->dir);	/* [한국어] 실패하면 NULL 이 담기고, 아래 unregister 가 그것을 안전하게 처리한다 */
}

#undef MAX_NAMELEN	/* [한국어] 위 128 정의를 치운다. 아래에서 다른 값으로 다시 정의하기 위해서다 */

#define MAX_NAMELEN 10	/* [한국어] 인터럽트 번호 문자열의 최대 길이. 32비트 정수의 자릿수에 널 종료를 더한 크기다. 위 128 과 달리 숫자만 담으므로 훨씬 작아도 된다 */

/*
 * [한국어]
 * register_irq_proc - 인터럽트 하나의 /proc/irq/N/ 디렉터리와 파일들을 만든다
 *
 * @irq:  그 인터럽트 번호. 디렉터리 이름이 된다.
 * @desc: 그 서술자.
 *
 * 만드는 것들:
 *   /proc/irq/N/                       — 디렉터리
 *   /proc/irq/N/smp_affinity           — 읽기·쓰기 (권한이 조건부다)
 *   /proc/irq/N/smp_affinity_list      — 같은 값의 목록 형식
 *   /proc/irq/N/affinity_hint          — 읽기 전용
 *   /proc/irq/N/node                   — 읽기 전용
 *   /proc/irq/N/effective_affinity     — 읽기 전용 (기능을 켠 빌드에만)
 *   /proc/irq/N/effective_affinity_list— 같은 값의 목록 형식
 *   /proc/irq/N/spurious               — 읽기 전용 (SMP 여부와 무관)
 *
 * 뮤텍스가 필요한 이유가 아래 영어 주석에 있다. 서술자를 만들 때가 아니라
 * 핸들러를 등록할 때 디렉터리를 만들므로, 여러 태스크가 같은 인터럽트에
 * 대해 동시에 이 함수를 부를 수 있다.
 *
 * no_irq_chip 을 걸러 내는 것도 중요하다. 컨트롤러가 꽂히지 않은 번호는
 * 실재하지 않는 인터럽트라, /proc 에 보여 줄 이유가 없다.
 *
 * smp_affinity 의 권한이 조건부인 것이 이 함수의 미묘한 부분이다.
 * 바꿀 수 없는 인터럽트에는 쓰기 권한을 주지 않아, 사용자가 시도조차
 * 하지 않게 한다.
 *
 * 실행 컨텍스트: 인터럽트 요청 경로 또는 부팅 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   __setup_irq() (kernel/irq/manage.c) → [이 함수]
 *   init_irq_proc() (아래) → [이 함수]
 */
void register_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	static DEFINE_MUTEX(register_lock);	/* [한국어] 함수 안의 static 이라 이 함수 전용이다. 모든 인터럽트가 공유하지만 등록이 드물어 경쟁이 없다 */
	void __maybe_unused *irqp = (void *)(unsigned long) irq;	/* [한국어] 인터럽트 번호를 포인터로 담는다 — proc_create_data 가 void * 만 받기 때문이다. SMP 가 아니면 쓰이지 않아 __maybe_unused 가 경고를 막는다 */
	char name [MAX_NAMELEN];	/* [한국어] 디렉터리 이름이 될 번호 문자열 */

	if (!root_irq_dir || (desc->irq_data.chip == &no_irq_chip))	/* [한국어] procfs 가 아직 없거나, 컨트롤러가 꽂히지 않은 번호인가 */
		return;	/* [한국어] 후자는 실재하지 않는 인터럽트라 보여 줄 이유가 없다 */

	/*
	 * irq directories are registered only when a handler is
	 * added, not when the descriptor is created, so multiple
	 * tasks might try to register at the same time.
	 */
	/* [한국어] (위 영어 주석에 이어) 뮤텍스가 필요한 이유.
	 *
	 * 디렉터리를 서술자 생성 시점이 아니라 핸들러 등록 시점에 만든다.
	 * 공유 인터럽트에 두 드라이버가 동시에 request_irq 를 하면 이 함수가
	 * 동시에 두 번 불릴 수 있고, 둘 다 디렉터리를 만들려 든다.
	 *
	 * 뮤텍스를 잡은 뒤 desc->dir 을 다시 확인하는 것이 그 방어의 나머지
	 * 절반이다 — 기다리는 동안 다른 쪽이 이미 만들었을 수 있다. */
	guard(mutex)(&register_lock);	/* [한국어] 여기서 잠들 수 있다. 이 함수가 프로세스 문맥에서만 불리는 이유다 */

	if (desc->dir)	/* [한국어] 기다리는 동안 다른 태스크가 만들었는가 */
		return;	/* [한국어] 이중 확인. 락 밖에서 한 번, 안에서 또 한 번 보는 흔한 관용구다 */

	/* create /proc/irq/1234 */
	sprintf(name, "%u", irq);	/* [한국어] (위 영어 주석) 번호를 문자열로. 버퍼가 충분히 커서 넘칠 수 없다 */
	desc->dir = proc_mkdir(name, root_irq_dir);	/* [한국어] 인터럽트 디렉터리 */
	if (!desc->dir)	/* [한국어] 생성 실패 */
		return;	/* [한국어] 아래 파일들을 만들 부모가 없다 */

#ifdef CONFIG_SMP	/* [한국어] 친화도 파일들은 CPU 가 여럿일 때만 만든다 */
	umode_t umode = S_IRUGO;	/* [한국어] 기본은 모두 읽기 가능 */

	if (irq_can_set_affinity_usr(desc->irq_data.irq))	/* [한국어] 사용자가 이 인터럽트의 친화도를 바꿀 수 있는가 */
		umode |= S_IWUSR;	/* [한국어] 그럴 때만 소유자(root) 쓰기 권한을 더한다. 바꿀 수 없는 인터럽트에는 시도조차 못 하게 한다 */

	/* create /proc/irq/<irq>/smp_affinity */
	proc_create_data("smp_affinity", umode, desc->dir, &irq_affinity_proc_ops, irqp);	/* [한국어] (위 영어 주석) 조건부 권한으로 만든다. irqp 가 show/write 에 전달될 인터럽트 번호다 */

	/* create /proc/irq/<irq>/affinity_hint */
	proc_create_single_data("affinity_hint", 0444, desc->dir,	/* [한국어] (위 영어 주석) 읽기 전용. 드라이버가 알려 주는 값이라 사용자가 바꿀 수 없다 */
				irq_affinity_hint_proc_show, irqp);	/* [한국어] single_data 판은 show 함수만 주면 되어 proc_ops 구조체가 필요 없다 */

	/* create /proc/irq/<irq>/smp_affinity_list */
	proc_create_data("smp_affinity_list", umode, desc->dir,	/* [한국어] (위 영어 주석) 위 smp_affinity 와 같은 권한 */
			 &irq_affinity_list_proc_ops, irqp);	/* [한국어] 목록 형식의 연산표 */

	proc_create_single_data("node", 0444, desc->dir, irq_node_proc_show, irqp);	/* [한국어] NUMA 노드. 읽기 전용이며 생성 때 정해진 값이라 바뀌지 않는다 */
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 추적하는 빌드에만 */
	proc_create_single_data("effective_affinity", 0444, desc->dir,	/* [한국어] 하드웨어가 실제로 고른 CPU. 읽기 전용 — 바꾸려면 smp_affinity 를 써야 한다 */
				irq_effective_aff_proc_show, irqp);
	proc_create_single_data("effective_affinity_list", 0444, desc->dir,	/* [한국어] 같은 값의 목록 형식 */
				irq_effective_aff_list_proc_show, irqp);
# endif	/* [한국어] CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK 분기의 끝. 이 기능이 없는 빌드에서는 위 두 show 함수가 아예 컴파일되지 않는다 */
#endif
	proc_create_single_data("spurious", 0444, desc->dir,	/* [한국어] CONFIG_SMP 밖에 있다 — 오탐 통계는 단일 프로세서에서도 의미가 있다 */
				irq_spurious_proc_show, (void *)(long)irq);	/* [한국어] irqp 를 쓰지 않고 다시 형변환한다. SMP 가 아니면 irqp 가 정의되지 않기 때문이다 */

}

/*
 * [한국어]
 * unregister_irq_proc - 인터럽트 하나의 /proc 항목들을 지운다
 *
 * @irq:  그 인터럽트 번호.
 * @desc: 그 서술자.
 *
 * 위 register 의 짝이다. 파일들을 먼저 지우고 마지막에 디렉터리를 지운다 —
 * 비어 있지 않은 디렉터리는 지울 수 없기 때문이다.
 *
 * 이 함수가 파일 상단 동기화 규칙의 핵심이다. remove_proc_entry() 가
 * 새 접근을 막고 진행 중인 것을 기다리므로, 이 함수가 돌아온 뒤에는
 * 아무도 이 서술자를 /proc 을 통해 만지지 않는다. 그래서 호출자가
 * 안심하고 서술자를 해제할 수 있다.
 *
 * #ifdef 구조가 위 register 와 정확히 대칭이어야 한다. 한쪽만 고치면
 * 만들었는데 지우지 않거나 그 반대가 되어, 지우지 못한 항목이 해제된
 * 서술자를 가리킨 채 남는다.
 *
 * 실행 컨텍스트: 서술자 해제 경로, 프로세스 문맥. remove_proc_entry 가
 * 진행 중인 접근을 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   free_desc() (kernel/irq/irqdesc.c) → [이 함수]
 */
void unregister_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	char name [MAX_NAMELEN];	/* [한국어] 디렉터리 이름이 될 번호 문자열 */

	if (!root_irq_dir || !desc->dir)	/* [한국어] procfs 가 없거나 디렉터리를 만든 적이 없는가 */
		return;
#ifdef CONFIG_SMP	/* [한국어] 위 register 와 정확히 대칭이어야 한다 */
	remove_proc_entry("smp_affinity", desc->dir);	/* [한국어] 각 호출이 진행 중인 접근을 기다린다 */
	remove_proc_entry("affinity_hint", desc->dir);	/* [한국어] 드라이버가 준 힌트 파일. 만든 순서와 무관하게 지울 수 있다 — 각 파일이 독립적이다 */
	remove_proc_entry("smp_affinity_list", desc->dir);	/* [한국어] 목록 형식 파일. smp_affinity 와 같은 마스크를 보지만 procfs 항목은 별개라 따로 지운다 */
	remove_proc_entry("node", desc->dir);	/* [한국어] NUMA 노드 파일 */
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 만들 때와 같은 조건 */
	remove_proc_entry("effective_affinity", desc->dir);	/* [한국어] 유효 친화도 파일. 만들 때와 같은 #ifdef 안에 있어야 짝이 맞는다 */
	remove_proc_entry("effective_affinity_list", desc->dir);	/* [한국어] 그 목록 형식 파일 */
# endif	/* [한국어] 유효 친화도 분기의 끝. register_irq_proc 의 같은 위치와 대칭이다 */
#endif
	remove_proc_entry("spurious", desc->dir);	/* [한국어] 이것도 만들 때와 같이 CONFIG_SMP 밖에 있다 */

	sprintf(name, "%u", irq);	/* [한국어] 디렉터리 이름 */
	remove_proc_entry(name, root_irq_dir);	/* [한국어] 마지막에 디렉터리를 지운다. 위에서 파일을 전부 지웠으므로 비어 있다 */
}

#undef MAX_NAMELEN	/* [한국어] 두 번째 정의를 치운다. 헤더가 아닌 .c 파일이지만 매크로를 쓰고 나서 없애는 규율을 지킨다 */

/*
 * [한국어]
 * unregister_handler_proc - 핸들러 디렉터리를 지운다
 *
 * @irq:    그 인터럽트 번호. 이 구현은 쓰지 않는다 — action 이 자기
 *          디렉터리 포인터를 들고 있기 때문이다.
 * @action: 해제되는 핸들러.
 *
 * 위 register_handler_proc 의 짝인데 훨씬 짧다. 이름으로 찾을 필요 없이
 * action->dir 포인터를 그대로 넘기면 된다.
 *
 * proc_remove 는 NULL 을 받아도 안전하다. 위 register 가 이름 충돌 등의
 * 이유로 디렉터리를 만들지 않았을 수 있어, 그 경우에도 그냥 부를 수 있다.
 *
 * irq 인자가 쓰이지 않는데도 남아 있는 것은 internals.h 의 선언과
 * 시그니처를 맞추기 위해서다. PROC_FS 를 뺀 빌드의 빈 구현도 같은
 * 시그니처를 갖는다.
 *
 * 실행 컨텍스트: 인터럽트 해제 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   free_irq() → __free_irq() (kernel/irq/manage.c) → [이 함수]
 */
void unregister_handler_proc(unsigned int irq, struct irqaction *action)
{
	proc_remove(action->dir);	/* [한국어] NULL 이어도 안전하다 — register 가 이름 충돌로 만들지 않았을 수 있다 */
}

/*
 * [한국어]
 * register_default_affinity_proc - /proc/irq/default_smp_affinity 를 만든다
 *
 * 인자도 반환값도 없다.
 *
 * 개별 인터럽트가 아니라 /proc/irq 바로 아래에 놓이는 전역 설정 파일이다.
 * 그래서 아래 init_irq_proc 이 한 번만 부른다.
 *
 * 함수 전체가 #ifdef 로 감싸여 있어 단일 프로세서 빌드에서는 빈 함수가
 * 된다. 파일이 만들어지지 않으므로 그 구성에서는 이 설정 자체가 없다.
 *
 * proc_create 를 쓰는 것에 주의: 위 인터럽트별 파일들이 쓰는
 * proc_create_data 와 달리 문맥 포인터를 넘기지 않는다. 전역 변수를
 * 다루므로 문맥이 필요 없기 때문이다.
 *
 * 실행 컨텍스트: 부팅 중 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   init_irq_proc() → [이 함수]
 */
static void register_default_affinity_proc(void)
{
#ifdef CONFIG_SMP	/* [한국어] 단일 프로세서에서는 기본 친화도라는 개념이 없다 */
	proc_create("irq/default_smp_affinity", 0644, NULL,	/* [한국어] 경로에 "irq/" 를 포함해 최상위 기준으로 만든다. 0644 라 root 만 쓸 수 있다 */
		    &default_affinity_proc_ops);	/* [한국어] 문맥 포인터를 넘기지 않는다 — 전역 변수를 다루므로 필요 없다 */
#endif
}

/*
 * [한국어]
 * init_irq_proc - 부팅 때 /proc/irq 를 만들고 기존 인터럽트를 등록한다
 *
 * 인자도 반환값도 없다.
 *
 * 세 단계다:
 *   /proc/irq 디렉터리를 만든다.
 *   전역 기본 친화도 파일을 만든다.
 *   이미 존재하는 인터럽트들의 항목을 만든다.
 *
 * 세 번째가 필요한 이유: 부팅 초기에 등록된 인터럽트들은 그때
 * root_irq_dir 이 NULL 이라 위 register_irq_proc 이 조용히 물러났다.
 * 여기서 일괄 등록해야 그것들도 /proc 에 나타난다.
 *
 * debugfs.c 의 irq_debugfs_init() 과 같은 구조다. 진단 기능이 인터럽트
 * 서브시스템보다 늦게 초기화되므로, 그 사이의 것들을 나중에 챙긴다.
 *
 * initcall 이 아니라 명시적으로 불리는 것에 주의: init/main.c 가 procfs
 * 초기화 뒤 적절한 시점에 직접 부른다. 순서를 정확히 통제해야 하기 때문이다.
 *
 * 실행 컨텍스트: 부팅 중, 프로세스 문맥.
 *
 * 호출 체인:
 *   start_kernel() → ... → [이 함수] → register_irq_proc()
 */
void init_irq_proc(void)
{
	unsigned int irq;	/* [한국어] 순회 중인 인터럽트 번호 */
	struct irq_desc *desc;	/* [한국어] 그 서술자 */

	/* create /proc/irq */
	root_irq_dir = proc_mkdir("irq", NULL);	/* [한국어] (위 영어 주석) 최상위 디렉터리. 이 전역이 채워진 뒤부터 register_irq_proc 이 실제로 항목을 만든다 */
	if (!root_irq_dir)	/* [한국어] 생성 실패 */
		return;	/* [한국어] 아무 항목도 만들어지지 않는다. 진단 기능이라 조용히 비활성화되는 것이 맞다 */

	register_default_affinity_proc();	/* [한국어] 전역 기본 친화도 파일. SMP 가 아니면 빈 함수다 */

	/*
	 * Create entries for all existing IRQs.
	 */
	/* [한국어] (위 영어 주석) 이미 만들어진 인터럽트들을 일괄 등록한다.
	 * 부팅 초기에 root_irq_dir 이 NULL 이라 물러났던 것들이다. */
	for_each_irq_desc(irq, desc)	/* [한국어] 모든 서술자 */
		register_irq_proc(irq, desc);	/* [한국어] 각각의 디렉터리와 파일을 만든다. 중복 등록은 그 함수가 걸러 낸다 */
}

/* [한국어] /proc/interrupts 의 출력. 공통 구현을 쓰는 아키텍처에만 있다.
 *
 * 왜 조건부인가: 몇몇 아키텍처는 이 파일의 형식을 자기 방식으로 만든다.
 * 그런 아키텍처는 GENERIC_IRQ_SHOW 를 켜지 않고 자체 show_interrupts 를
 * 제공한다. 오늘날 대부분은 이 공통 구현을 쓴다. */
#ifdef CONFIG_GENERIC_IRQ_SHOW	/* [한국어] 공통 /proc/interrupts 구현을 쓰는 아키텍처 */

/*
 * [한국어]
 * arch_show_interrupts - 아키텍처가 자기 고유 항목을 덧붙일 자리
 *
 * @p:      출력할 seq_file.
 * @prec:   첫 열의 폭. 아래 show_interrupts 가 계산해 둔 값이다.
 * @return: 0 이면 성공.
 *
 * __weak 이라 아키텍처가 같은 이름의 함수를 정의하면 그것이 대신 쓰인다.
 * 정의하지 않으면 이 빈 구현이 링크되어 아무것도 덧붙이지 않는다.
 *
 * 무엇을 덧붙이는가: /proc/interrupts 의 아래쪽에 나오는 NMI, LOC, RES
 * 같은 줄들이다. 그것들은 보통의 인터럽트 서술자를 갖지 않는 아키텍처
 * 고유의 인터럽트라, 이 훅으로 직접 찍는다.
 *
 * prec 를 넘기는 이유: 위쪽 줄들과 열 폭을 맞추기 위해서다. 인터럽트
 * 번호의 자릿수에 따라 첫 열의 폭이 달라지므로, 그 값을 알려 주어야
 * 아키텍처 줄들도 정렬된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /proc/interrupts → show_interrupts() → [이 함수 또는 아키텍처의 판]
 */
int __weak arch_show_interrupts(struct seq_file *p, int prec)
{
	return 0;	/* [한국어] 아키텍처가 재정의하지 않으면 아무것도 덧붙이지 않는다 */
}

/* [한국어] 순회의 상한. 아키텍처가 재정의할 수 있다.
 *
 * 기본은 실제 인터럽트 수인데, 어떤 아키텍처는 그보다 큰 값을 써서
 * 마지막에 여분의 줄을 넣을 자리를 만든다. 아래 show_interrupts 가
 * 이 값과 같을 때 arch_show_interrupts 를 부르는 구조라, 그 자리가
 * 아키텍처 고유 항목의 출력 지점이 된다. */
#ifndef ACTUAL_NR_IRQS	/* [한국어] 아키텍처가 정의하지 않았으면 */
# define ACTUAL_NR_IRQS irq_get_nr_irqs()	/* [한국어] 실제 인터럽트 수를 쓴다 */
#endif

/*
 * [한국어]
 * show_interrupts - /proc/interrupts 의 한 줄을 출력한다
 *
 * @p:      출력할 seq_file.
 * @v:      seq_file 이 넘기는 순회 위치. 인터럽트 번호가 들어 있다.
 * @return: 0 이면 성공(줄을 찍었든 건너뛰었든).
 *
 * seq_file 의 show 콜백이라 한 번에 한 줄씩 불린다. 사용자가 파일을
 * 읽으면 인터럽트 번호 0 부터 차례로 이 함수가 실행된다.
 *
 * 출력 형식:
 *              CPU0       CPU1       CPU2       CPU3
 *     16:      1234          0          0          0   IO-APIC   16-fasteoi   ehci_hcd
 *
 * 한 줄의 구성:
 *   번호 — 첫 열. 폭은 전체 인터럽트 수의 자릿수에 맞춘다.
 *   CPU 별 횟수 — 온라인 CPU 마다 하나씩.
 *   chip 이름 — 어느 컨트롤러가 담당하는가.
 *   hwirq — 그 컨트롤러에서의 하드웨어 번호.
 *   트리거 방식 — Level 인지 Edge 인지 (그 기능을 켠 빌드에만).
 *   서술자 이름 — 흐름 제어 방식 등.
 *   핸들러 이름들 — 공유 인터럽트면 쉼표로 이어진다.
 *
 * 세 가지 특수 처리가 있다:
 *   i == 0 이면 머리말을 찍고 첫 열 폭을 계산한다. 그 폭이 static 변수에
 *     담겨 이후 줄들이 공유한다.
 *   i == ACTUAL_NR_IRQS 면 아키텍처 고유 항목을 찍는다.
 *   그보다 크면 아무것도 하지 않는다 — 순회의 끝이다.
 *
 * 세 종류의 인터럽트를 걸러 낸다:
 *   hidden — 계층형 도메인의 중간 단계. 통계가 언제나 0 이라 무의미하다.
 *   핸들러 없음 — 아무도 쓰지 않는 번호.
 *   연쇄(chained) — 하위 컨트롤러로 가는 통로라 그 자체의 통계가 없다.
 *
 * 파일 상단의 동기화 규칙대로, 이 경로는 procfs 의 보호를 받지 못해
 * RCU 로 직접 서술자를 보호한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. RCU 읽기 구간과 서술자 락을 차례로 잡는다.
 *
 * 호출 체인:
 *   cat /proc/interrupts → fs/proc/interrupts.c 의 seq_file → [이 함수]
 */
int show_interrupts(struct seq_file *p, void *v)
{
	const unsigned int nr_irqs = irq_get_nr_irqs();	/* [한국어] 전체 인터럽트 수. 첫 열의 폭을 계산하는 데 쓴다 */
	static int prec;	/* [한국어] 첫 열의 폭. static 이라 첫 줄에서 계산한 값을 이후 줄들이 공유한다. 여러 프로세스가 동시에 읽으면 서로 덮어쓸 수 있지만, 값이 거의 같아 출력이 어긋나 보이지 않는다 */

	int i = *(loff_t *) v, j;	/* [한국어] seq_file 이 넘긴 위치가 곧 인터럽트 번호다. j 는 CPU 순회에 쓴다 */
	struct irqaction *action;	/* [한국어] 핸들러 목록을 훑을 커서 */
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */

	if (i > ACTUAL_NR_IRQS)	/* [한국어] 순회의 끝을 넘었는가 */
		return 0;	/* [한국어] 아무것도 찍지 않는다 */

	if (i == ACTUAL_NR_IRQS)	/* [한국어] 마지막 자리인가 */
		return arch_show_interrupts(p, prec);	/* [한국어] 아키텍처 고유 항목(NMI, LOC 등)을 찍을 자리다. 위에서 계산한 열 폭을 넘겨 정렬을 맞춘다 */

	/* print header and calculate the width of the first column */
	/* [한국어] (위 영어 주석) 첫 줄에서 머리말을 찍고 열 폭을 정한다. */
	if (i == 0) {	/* [한국어] 첫 호출인가 */
		for (prec = 3, j = 1000; prec < 10 && j <= nr_irqs; ++prec)	/* [한국어] 최소 3칸에서 시작해, 인터럽트 수의 자릿수만큼 늘린다. 상한 10 은 int 의 최대 자릿수다 */
			j *= 10;	/* [한국어] 1000, 10000, ... 과 견주며 자릿수를 센다 */

		seq_printf(p, "%*s", prec + 8, "");	/* [한국어] 머리말의 왼쪽 여백. 번호 열과 그 뒤 콜론·공백만큼 비운다 */
		for_each_online_cpu(j)	/* [한국어] 온라인 CPU 마다 */
			seq_printf(p, "CPU%-8d", j);	/* [한국어] "CPU0       " 처럼. 아래 숫자 열의 폭과 맞춘다 */
		seq_putc(p, '\n');	/* [한국어] 머리말을 마친다. 이 뒤로 인터럽트 0 번 줄이 이어진다 */
	}

	guard(rcu)();	/* [한국어] 파일 상단 동기화 규칙대로, 이 경로는 procfs 의 보호를 받지 못해 직접 서술자를 보호해야 한다 */
	desc = irq_to_desc(i);	/* [한국어] 번호로 서술자를 찾는다. RCU 아래라 찾는 동안 사라지지 않는다 */
	if (!desc || irq_settings_is_hidden(desc))	/* [한국어] 없는 번호이거나, 계층형 도메인의 중간 단계라 감춰야 하는가 */
		return 0;	/* [한국어] 그 줄을 건너뛴다. 중간 단계는 통계가 언제나 0 이라 보여 줄 의미가 없다 */

	if (!desc->action || irq_desc_is_chained(desc) || !desc->kstat_irqs)	/* [한국어] 핸들러가 없거나, 하위 컨트롤러로 가는 통로이거나, 통계 자체가 없는가 */
		return 0;	/* [한국어] 세 경우 모두 보여 줄 숫자가 없다 */

	seq_printf(p, "%*d:", prec, i);	/* [한국어] 번호 열. 위에서 계산한 폭으로 오른쪽 정렬한다 */
	for_each_online_cpu(j) {	/* [한국어] 온라인 CPU 마다 하나씩 */
		unsigned int cnt = desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, j) : 0;	/* [한국어] 위에서 이미 확인했지만 다시 검사한다 — RCU 아래라도 그 사이 바뀔 수 있다는 방어다 */

		seq_put_decimal_ull_width(p, " ", cnt, 10);	/* [한국어] 10칸 폭으로 찍는다. 머리말의 "CPU%-8d" 와 맞물려 열이 정렬된다 */
	}
	seq_putc(p, ' ');	/* [한국어] 숫자 열과 아래 chip 이름 사이의 구분 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 아래 chip 과 핸들러 목록을 읽는 동안 바뀌지 않게 한다. RCU 만으로는 목록의 일관성이 보장되지 않는다 */
	if (desc->irq_data.chip) {	/* [한국어] 컨트롤러가 꽂혀 있는가 */
		if (desc->irq_data.chip->irq_print_chip)	/* [한국어] 자체 출력을 제공하는가 */
			desc->irq_data.chip->irq_print_chip(&desc->irq_data, p);	/* [한국어] GIC 라면 어느 인스턴스인지까지 찍는다 */
		else if (desc->irq_data.chip->name)	/* [한국어] 이름이라도 있는가 */
			seq_printf(p, "%8s", desc->irq_data.chip->name);	/* [한국어] 8칸 폭으로 정렬 */
		else
			seq_printf(p, "%8s", "-");	/* [한국어] chip 은 있는데 이름이 없다. 드문 경우다 */
	} else {
		seq_printf(p, "%8s", "None");	/* [한국어] 컨트롤러가 꽂히지 않았다. 위 register_irq_proc 이 no_irq_chip 을 걸러 내므로 여기 오는 일은 드물다 */
	}
	if (desc->irq_data.domain)	/* [한국어] 도메인에 매핑된 인터럽트인가 */
		seq_printf(p, " %*lu", prec, desc->irq_data.hwirq);	/* [한국어] 하드웨어 번호. 리눅스 번호와 다르다는 것이 도메인의 존재 이유다 */
	else
		seq_printf(p, " %*s", prec, "");	/* [한국어] 도메인이 없으면 그 자리를 비운다. 폭은 맞춰 열이 어긋나지 않게 한다 */
#ifdef CONFIG_GENERIC_IRQ_SHOW_LEVEL	/* [한국어] 트리거 방식까지 보여 주는 빌드 */
	seq_printf(p, " %-8s", irqd_is_level_type(&desc->irq_data) ? "Level" : "Edge");	/* [한국어] 레벨인지 엣지인지. 흐름 제어 방식과 맞는지 대조할 수 있다 */
#endif
	if (desc->name)	/* [한국어] 서술자에 이름이 붙어 있는가 */
		seq_printf(p, "-%-8s", desc->name);	/* [한국어] 흐름 제어 방식 등이 담긴다. "16-fasteoi" 의 뒷부분이 이것이다 */

	action = desc->action;	/* [한국어] 첫 핸들러 */
	if (action) {	/* [한국어] 위에서 이미 확인했지만 락을 잡은 뒤 다시 읽는다 */
		seq_printf(p, "  %s", action->name);	/* [한국어] 첫 핸들러의 이름 */
		while ((action = action->next) != NULL)	/* [한국어] 공유 인터럽트면 여럿이다 */
			seq_printf(p, ", %s", action->name);	/* [한국어] 쉼표로 이어 붙인다. 이 열을 보면 누가 그 선을 공유하는지 알 수 있다 */
	}

	seq_putc(p, '\n');	/* [한국어] 한 줄을 마친다 */
	return 0;	/* [한국어] seq_file 은 0 이 아닌 값을 오류로 다룬다 */
}
#endif
