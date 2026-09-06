// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006 Thomas Gleixner
 *
 * This file contains driver APIs to the irq subsystem.
 */

/*
 * [한국어 설명] 드라이버가 보는 인터럽트 API (manage.c)
 *
 * === 파일의 역할 ===
 * request_irq(), free_irq(), disable_irq(), enable_irq(),
 * irq_set_affinity() — 드라이버 작성자가 아는 인터럽트 함수는 거의
 * 전부 이 파일에 있다. 인터럽트 서브시스템의 얼굴이다.
 *
 * 그 뒤에 감춰진 일이 만만치 않다. request_irq() 하나가 하는 일을
 * 세어 보면 이렇다. 소유 모듈 참조를 잡고, 스레드 핸들러가 필요하면
 * 커널 스레드를 만들고, 공유 인터럽트라면 기존 등록자들과 플래그가
 * 맞는지 따지고, oneshot 스레드 마스크 비트를 배정하고, 트리거 방식을
 * 하드웨어에 설정하고, 도메인에 자원 배정을 요청하고, 인터럽트를
 * 시작하고, /proc 항목을 만든다. 그 사이 어디서 실패해도 앞의 것을
 * 정확히 되돌려야 한다.
 *
 * 두 번째 큰 덩어리가 스레드 인터럽트 기구다. 하드 인터럽트 문맥에서
 * 할 수 없는 일 — 잠들거나, 느린 버스를 기다리거나, 뮤텍스를 잡는 일 —
 * 을 위해 인터럽트마다 커널 스레드를 두는 방식이다. irq_thread() 와
 * 그 주변 함수들이 그 스레드의 생애를 관리한다.
 *
 * 세 번째는 친화도 관리다. 인터럽트를 어느 CPU 로 보낼지 정하는 일인데,
 * 관리형 인터럽트·격리 CPU·CPU 핫플러그·사용자 요청이 얽혀 이 파일에서
 * 가장 조건이 많은 부분이 됐다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 인터럽트 서브시스템의 가장 바깥 껍질이다. 위로는 드라이버가
 * 있고, 아래로는 코어의 나머지가 있다.
 *
 *   드라이버
 *     → request_irq() / free_irq() / disable_irq() ...   ← 이 파일
 *     → irq_startup() / irq_shutdown()                   (chip.c)
 *     → irq_domain_activate_irq()                        (irqdomain.c)
 *     → chip->irq_set_type() 등 칩 콜백                   (drivers/irqchip)
 *
 * 인터럽트가 실제로 발생할 때의 경로와는 거의 겹치지 않는다. 그쪽은
 * irqdesc.c → chip.c → handle.c 이고, 이 파일은 설정 경로다. 예외가
 * 스레드 인터럽트인데, irq_thread() 가 실행 경로의 일부다.
 *
 * 락 계층이 이 파일의 핵심 규약이다. __setup_irq() 위의 주석이 그것을
 * 명시한다.
 *
 *   desc->request_mutex   request_irq 와 free_irq 를 서로 배제
 *     chip_bus_lock       느린 버스(I2C/SPI) 접근을 직렬화
 *       desc->lock        하드 인터럽트와 경쟁을 막는다
 *
 * 안쪽으로 갈수록 짧게 잡아야 한다. desc->lock 은 인터럽트를 끈 채
 * 잡는 raw spinlock 이라 그 안에서 잠들 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 커널 전체의 드라이버가 이 파일의 API 를 부른다.
 *
 * 아래쪽:
 *   - kernel/irq/chip.c → irq_startup/shutdown/enable/disable,
 *     __irq_set_trigger 의 실제 하드웨어 조작
 *   - kernel/irq/irqdesc.c → 서술자 조회와 잠금 관용구
 *   - kernel/irq/irqdomain.c → 활성화·비활성화
 *   - kernel/irq/proc.c → /proc/irq 항목 생성
 *   - kernel/irq/pm.c → 서스펜드 시 인터럽트 처리
 *   - kernel/sched → 스레드 생성과 우선순위 설정
 *
 * 공유 상태: struct irqaction 이 이 파일이 만들고 관리하는 자료구조다.
 * 서술자의 action 목록에 매달려 공유 인터럽트의 여러 등록자를 잇는다.
 * desc->depth, desc->wake_depth 같은 중첩 카운터도 여기서 관리한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - __setup_irq(): 인터럽트 등록의 전부. 이 파일에서 가장 긴 함수이고
 *   request_irq 계열이 모두 여기로 모인다.
 * - __free_irq(): 그 반대. 순서와 락 해제 시점이 미묘하다.
 * - irq_thread(): 스레드 인터럽트의 본체. 깨어나 핸들러를 부르고
 *   oneshot 마스크를 정리하는 루프다.
 * - irq_do_set_affinity(): 친화도 설정의 중심. 격리 CPU 회피와
 *   온라인 CPU 검사가 여기 있다.
 * - synchronize_irq(): 진행 중인 처리가 끝나기를 기다린다. 해제
 *   경로의 안전을 떠받치는 함수다.
 * - __enable_irq() / __disable_irq(): 중첩 카운터(depth) 관리.
 */

#define pr_fmt(fmt) "genirq: " fmt	/* [한국어] 이 파일의 모든 pr_* 출력 앞에 "genirq: " 를 붙인다. 인터럽트 코어의 메시지를 로그에서 골라내기 쉽게 한다 */

#include <linux/irq.h>	/* [한국어] struct irq_desc, irq_chip, IRQD_ 상수 — 이 파일이 다루는 자료구조 */
#include <linux/kthread.h>	/* [한국어] kthread_create/stop — 스레드 인터럽트의 커널 스레드 생성 */
#include <linux/module.h>	/* [한국어] try_module_get/module_put — 소유 모듈이 내려가지 못하게 붙잡는다 */
#include <linux/random.h>	/* [한국어] 인터럽트를 엔트로피 원으로 쓰는 코드와 함께 포함된다 */
#include <linux/interrupt.h>	/* [한국어] struct irqaction, IRQF_ 플래그 — 드라이버가 보는 공개 인터페이스 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_activate_irq/deactivate_irq — 자원 배정과 반납 */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree — irqaction 할당 */
#include <linux/sched.h>	/* [한국어] task_struct, set_cpus_allowed_ptr — 스레드 관리 */
#include <linux/sched/rt.h>	/* [한국어] sched_set_fifo — 인터럽트 스레드를 실시간 우선순위로 올린다 */
#include <linux/sched/task.h>	/* [한국어] get_task_struct/put — 스레드 참조 관리 */
#include <linux/sched/isolation.h>	/* [한국어] housekeeping_cpumask — 격리된 CPU 에 인터럽트를 보내지 않으려는 검사 */
#include <uapi/linux/sched/types.h>	/* [한국어] 스케줄링 정책 상수 */
#include <linux/task_work.h>	/* [한국어] task_work_add — 스레드가 죽을 때 정리 콜백을 걸어 둔다 */

#include "internals.h"	/* [한국어] 코어 전용 선언. 이 파일이 서술자 내부와 다른 파일의 내부 함수를 직접 부른다 */

#if defined(CONFIG_IRQ_FORCED_THREADING) && !defined(CONFIG_PREEMPT_RT)	/* [한국어] 강제 스레드화를 부팅 옵션으로 켤 수 있는 빌드. PREEMPT_RT 에서는 항상 켜져 있어 스위치가 필요 없다 */
DEFINE_STATIC_KEY_FALSE(force_irqthreads_key);
/* [한국어] 모든 인터럽트를 스레드로 처리할지 정하는 정적 키.
 * 설정자: 아래 setup_forced_irqthreads() 가 "threadirqs" 부팅
 *   파라미터를 보고 켠다.
 * 읽는 자: force_irqthreads() 매크로를 통해 irq_setup_forced_threading()
 *   과 irq_thread() 가 본다.
 * 값 범위: 꺼짐(기본) 또는 켜짐.
 * 동기화: 정적 키는 코드 패칭으로 구현되어, 꺼져 있을 때 분기 비용이
 *   사실상 0 이다. 인터럽트 경로에서 매번 검사하므로 그 성질이 중요하다.
 *
 * 왜 이런 옵션이 있는가: 인터럽트 지연을 줄이려면 하드 인터럽트
 * 문맥에서 하는 일을 최소화해야 한다. 전부 스레드로 옮기면 지연이
 * 예측 가능해지지만 처리량은 떨어진다. 실시간 요구가 있는 시스템이
 * 그 맞바꿈을 택한다. */

/*
 * [한국어]
 * setup_forced_irqthreads - "threadirqs" 부팅 파라미터를 처리한다
 *
 * @arg: 파라미터 값 (쓰지 않는다)
 * @return: 항상 0 (early_param 규약상 성공)
 *
 * 부팅 명령줄에 threadirqs 가 있으면 모든 인터럽트를 스레드로
 * 처리하도록 정적 키를 켠다.
 *
 * early_param 이라 아주 이른 시점에 불린다 — 인터럽트가 요청되기
 * 훨씬 전이다. 그래야 이후의 모든 request_irq 가 이 설정을 본다.
 *
 * 값을 보지 않는 것에 주목: 파라미터가 있기만 하면 켠다. threadirqs=0
 * 같은 것으로 끌 수는 없다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드.
 *
 * 호출 체인:
 *   parse_early_param() → early_param 테이블 → [이 함수]
 */
static int __init setup_forced_irqthreads(char *arg)
{
	static_branch_enable(&force_irqthreads_key);	/* [한국어] 코드 패칭으로 분기를 켠다. 이 뒤로 force_irqthreads() 가 참이 된다 */
	return 0;	/* [한국어] early_param 규약상 0 이 성공. 값을 보지 않으므로 threadirqs=0 으로 끌 수 없다 */
}
early_param("threadirqs", setup_forced_irqthreads);	/* [한국어] 인터럽트가 요청되기 훨씬 전에 처리되어야 하므로 early_param 이다 */
#endif	/* [한국어] 강제 스레드화 옵션 분기의 끝 */

#ifdef CONFIG_SMP	/* [한국어] 다른 CPU 로 넘긴 작업을 기다리는 것은 SMP 에만 있는 개념이다 */
/*
 * [한국어]
 * synchronize_irqwork - 다른 CPU 로 넘긴 인터럽트 작업이 끝나기를 기다린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 디먹스 인터럽트가 친화도에 맞는 CPU 로 넘겨진 경우, 그 작업이
 * irq_work 큐에 들어 있거나 실행 중일 수 있다. 인터럽트를 해제하기
 * 전에 그것이 끝나야 한다.
 *
 * 그러지 않으면 어떻게 되는가: 해제된 서술자를 가리키는 작업 항목이
 * 큐에 남아, 나중에 실행되면서 해제된 메모리를 만진다.
 *
 * irq_work_sync 는 그 항목이 큐에서 나오고 실행까지 끝나기를 기다린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 기다리므로 잠들 수 있는 문맥이어야
 * 한다.
 *
 * 호출 체인:
 *   __synchronize_irq() → [이 함수] → irq_work_sync()
 */
static inline void synchronize_irqwork(struct irq_desc *desc)
{
	/* Synchronize pending or on the fly redirect work */
	irq_work_sync(&desc->redirect.work);	/* [한국어] (위 영어 주석) 큐에 들어 있거나 실행 중인 리다이렉트 작업을 기다린다. 남겨 두면 해제된 서술자를 만진다 */
}
#else	/* [한국어] 단일 프로세서 */
/*
 * [한국어]
 * synchronize_irqwork - 리다이렉트 작업 대기 (UP 판, 빈 함수)
 *
 * @desc: 무시
 * @return: 없음
 *
 * 넘길 다른 CPU 가 없으므로 리다이렉트 작업도 없다.
 *
 * 호출 체인:
 *   __synchronize_irq() → [이 함수]
 */
static inline void synchronize_irqwork(struct irq_desc *desc) { }	/* [한국어] 기다릴 작업이 없다 */
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

static int __irq_get_irqchip_state(struct irq_data *d, enum irqchip_irq_state which, bool *state);
/* [한국어] 칩의 내부 상태를 묻는 함수의 전방 선언.
 * 설정자: 파일 끝의 정의.
 * 읽는 자: 바로 아래 __synchronize_hardirq() 가 부른다.
 * 값 범위: 0 성공(그때만 *state 가 갱신됨), 음수 오류.
 * 동기화: 호출자가 desc->lock 을 쥐고 있어야 한다.
 *
 * 전방 선언이 필요한 이유: 정의는 파일 끝의 상태 조회 API 무리에
 * 있는데, 동기화 함수가 파일 앞머리에서 그것을 쓴다. */

/*
 * [한국어]
 * __synchronize_hardirq - 하드 인터럽트 처리가 끝나기를 기다린다
 *
 * @desc:      대상 서술자
 * @sync_chip: 하드웨어 수준의 대기 인터럽트까지 기다릴지
 * @return:    없음
 *
 * 다른 CPU 가 이 인터럽트의 핸들러를 실행 중이면 끝날 때까지 기다린다.
 * 인터럽트를 해제하거나 설정을 바꾸기 전에 반드시 거쳐야 하는 관문이다.
 *
 * 두 겹 루프의 구조가 이 함수의 핵심이다.
 *
 * 안쪽 while 은 락 없이 회전한다. 락을 잡고 검사하면 그 락을 필요로
 * 하는 처리 쪽이 진행하지 못해 영원히 끝나지 않는다. 대신 원본
 * 주석대로 메모리 장벽이 없어 답이 틀릴 수 있다.
 *
 * 그래서 바깥 do-while 이 락을 잡고 다시 확인한다. 락 획득이
 * 장벽 역할을 하므로 이번 답은 믿을 수 있다. 그런데 락을 잡는 사이에
 * 다시 시작됐을 수 있어 루프가 필요하다.
 *
 * sync_chip 이 여는 세 번째 검사: 소프트웨어적으로는 처리 중이 아닌데
 * 하드웨어에 아직 전달되지 않은 인터럽트가 걸려 있을 수 있다. 칩이
 * 그 상태를 알려 줄 수 있으면 그것까지 기다린다.
 *
 * 그 검사를 항상 하지 않는 이유가 synchronize_hardirq() 의 주석에
 * 있다 — 인터럽트를 끈 채로 부르면서 대상 CPU 가 자기 자신이면
 * 데드락이 난다.
 *
 * 실행 컨텍스트: 대개 프로세스 문맥. 회전 대기이므로 오래 걸리면 안
 * 된다.
 *
 * 호출 체인:
 *   synchronize_hardirq() / __synchronize_irq() → [이 함수] →
 *   __irq_get_irqchip_state()
 */
static void __synchronize_hardirq(struct irq_desc *desc, bool sync_chip)
{
	struct irq_data *irqd = irq_desc_get_irq_data(desc);	/* [한국어] 칩 상태 조회에 넘길 irq_data */
	bool inprogress;	/* [한국어] 아직 처리 중인가 */

	do {
		/*
		 * Wait until we're out of the critical section.  This might
		 * give the wrong answer due to the lack of memory barriers.
		 */
		while (irqd_irq_inprogress(&desc->irq_data))	/* [한국어] (위 영어 주석) 락 없이 회전한다. 락을 잡고 검사하면 그 락이 필요한 처리 쪽이 진행하지 못해 영원히 끝나지 않는다 */
			cpu_relax();	/* [한국어] 회전 대기 힌트 */

		/* Ok, that indicated we're done: double-check carefully. */
		guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] (위 영어 주석) 락 획득이 장벽 역할을 해 이번 답은 믿을 수 있다 */
		inprogress = irqd_irq_inprogress(&desc->irq_data);	/* [한국어] 다시 확인. 락을 잡는 사이에 새로 시작됐을 수 있다 */

		/*
		 * If requested and supported, check at the chip whether it
		 * is in flight at the hardware level, i.e. already pending
		 * in a CPU and waiting for service and acknowledge.
		 */
		if (!inprogress && sync_chip) {	/* [한국어] (위 영어 주석) 소프트웨어적으로는 끝났는데 하드웨어에 아직 전달 중인 인터럽트가 있을 수 있다 */
			/*
			 * Ignore the return code. inprogress is only updated
			 * when the chip supports it.
			 */
			__irq_get_irqchip_state(irqd, IRQCHIP_STATE_ACTIVE,	/* [한국어] (위 영어 주석) 반환값을 무시한다 — 칩이 지원하지 않으면 inprogress 가 그대로 거짓이라 루프가 끝난다 */
						&inprogress);
		}
		/* Oops, that failed? */
	} while (inprogress);	/* [한국어] (위 영어 주석) 아직 처리 중이면 다시 돈다 */
}

/**
 * synchronize_hardirq - wait for pending hard IRQ handlers (on other CPUs)
 * @irq: interrupt number to wait for
 *
 * This function waits for any pending hard IRQ handlers for this interrupt
 * to complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock. It does not take
 * associated threaded handlers into account.
 *
 * Do not use this for shutdown scenarios where you must be sure that all
 * parts (hardirq and threaded handler) have completed.
 *
 * Returns: false if a threaded handler is active.
 *
 * This function may be called - with care - from IRQ context.
 *
 * It does not check whether there is an interrupt in flight at the
 * hardware level, but not serviced yet, as this might deadlock when called
 * with interrupts disabled and the target CPU of the interrupt is the
 * current CPU.
 */
/*
 * [한국어]
 * synchronize_hardirq - 하드 인터럽트 핸들러만 기다린다
 *
 * @irq: 대상 인터럽트 번호
 * @return: true 스레드 핸들러도 없음, false 스레드가 아직 동작 중
 *
 * 아래 synchronize_irq() 와 두 가지가 다르다. 스레드 핸들러를 기다리지
 * 않고, 하드웨어 수준의 대기 인터럽트도 확인하지 않는다.
 *
 * 그 두 생략이 이 함수를 인터럽트 문맥에서도 (조심스럽게) 부를 수
 * 있게 만든다. 스레드를 기다리면 잠들어야 하고, 하드웨어 상태를
 * 확인하려다 대상 CPU 가 자기 자신이면 데드락이 나기 때문이다 —
 * 원본 주석의 마지막 문단이 그것을 설명한다.
 *
 * 반환값이 스레드 상태를 알려 주므로, 호출자가 그것을 보고 판단할 수
 * 있다. disable_hardirq() 가 그렇게 쓴다.
 *
 * 원본 주석의 경고: 완전한 정지가 필요한 해제 경로에는 쓰면 안 된다.
 * 그때는 synchronize_irq() 를 써야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 조심하면 인터럽트 문맥도 가능.
 *
 * 호출 체인:
 *   disable_hardirq() / 드라이버 → [이 함수] → __synchronize_hardirq()
 */
bool synchronize_hardirq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (desc) {	/* [한국어] 존재하는 인터럽트인가 */
		__synchronize_hardirq(desc, false);	/* [한국어] false — 하드웨어 상태를 확인하지 않는다. 인터럽트를 끈 채 불렸고 대상 CPU 가 자기 자신이면 데드락이 나기 때문이다 */
		return !atomic_read(&desc->threads_active);	/* [한국어] 스레드는 기다리지 않고 상태만 알려 준다. 호출자가 보고 판단한다 */
	}

	return true;	/* [한국어] 없는 인터럽트에는 기다릴 것도 없다 */
}
EXPORT_SYMBOL(synchronize_hardirq);	/* [한국어] 드라이버가 부른다 */

/*
 * [한국어]
 * __synchronize_irq - 모든 인터럽트 처리가 끝나기를 기다린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 세 가지를 순서대로 기다린다.
 *
 *   1. 다른 CPU 로 넘긴 리다이렉트 작업
 *   2. 하드 인터럽트 핸들러 (하드웨어 대기까지 포함)
 *   3. 스레드 핸들러
 *
 * 순서가 중요하다. 하드 인터럽트 핸들러가 스레드를 깨울 수 있으므로,
 * 2 를 먼저 끝내야 3 의 카운터가 더 이상 늘지 않는다. 반대로 하면
 * 스레드를 기다린 뒤에 새 스레드가 깨어날 수 있다.
 *
 * 1 이 가장 먼저인 이유도 같다. 리다이렉트 작업이 실행되면 그것이
 * 하드 인터럽트 처리를 시작한다.
 *
 * 3 에서 wait_event 를 쓰는 것에 주목: 회전하지 않고 잠든다. 스레드
 * 핸들러는 오래 걸릴 수 있어 회전 대기가 부적절하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   synchronize_irq() / __free_irq() → [이 함수]
 */
static void __synchronize_irq(struct irq_desc *desc)
{
	synchronize_irqwork(desc);	/* [한국어] 가장 먼저. 리다이렉트 작업이 실행되면 그것이 하드 인터럽트 처리를 시작한다 */
	__synchronize_hardirq(desc, true);	/* [한국어] true — 여기서는 하드웨어 대기 인터럽트까지 확인한다. 프로세스 문맥이라 데드락 위험이 없다 */

	/*
	 * We made sure that no hardirq handler is running. Now verify that no
	 * threaded handlers are active.
	 */
	wait_event(desc->wait_for_threads, !atomic_read(&desc->threads_active));	/* [한국어] (위 영어 주석) 잠들어 기다린다. 하드 인터럽트를 먼저 끝냈으므로 이 카운터가 더 이상 늘지 않는다 */
}

/**
 * synchronize_irq - wait for pending IRQ handlers (on other CPUs)
 * @irq: interrupt number to wait for
 *
 * This function waits for any pending IRQ handlers for this interrupt to
 * complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock.
 *
 * Can only be called from preemptible code as it might sleep when
 * an interrupt thread is associated to @irq.
 *
 * It optionally makes sure (when the irq chip supports that method)
 * that the interrupt is not pending in any CPU and waiting for
 * service.
 */
/*
 * [한국어]
 * synchronize_irq - 이 인터럽트의 모든 처리가 끝나기를 기다린다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 인터럽트 해제 경로의 안전을 떠받치는 함수다. 이 함수가 반환하면
 * 어느 CPU 에서도 이 인터럽트의 핸들러가 실행되고 있지 않다.
 *
 * 원본 주석의 데드락 경고가 중요하다. 핸들러가 필요로 하는 자원을
 * 쥔 채 부르면, 핸들러가 그 자원을 기다리고 이쪽은 핸들러를
 * 기다려 서로 멈춘다. 드라이버가 자기 락을 쥔 채 이것을 부르는 실수가
 * 흔하다.
 *
 * 잠들 수 있는 문맥이어야 한다 — 스레드 핸들러를 기다리기 때문이다.
 * 인터럽트 문맥에서 부르려면 위 synchronize_hardirq() 를 써야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 선점 가능 상태.
 *
 * 호출 체인:
 *   disable_irq() / irq_domain_disassociate() / 드라이버 → [이 함수]
 */
void synchronize_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (desc)	/* [한국어] 존재하는 인터럽트인가 */
		__synchronize_irq(desc);	/* [한국어] 세 가지를 순서대로 기다린다 */
}
EXPORT_SYMBOL(synchronize_irq);	/* [한국어] 드라이버와 코어 전반이 부른다 */

#ifdef CONFIG_SMP	/* [한국어] 여기부터 친화도 관리 전체가 SMP 전용이다. 대응하는 #endif 가 한참 아래에 있다 */
cpumask_var_t irq_default_affinity;
/* [한국어] 새 인터럽트가 물려받을 기본 친화도 마스크.
 * 설정자: kernel/irq/irqdesc.c 의 irq_affinity_setup()(부팅 파라미터)과
 *   init_irq_default_affinity(), 그리고 /proc/irq/default_smp_affinity
 *   쓰기(kernel/irq/proc.c).
 * 읽는 자: desc_smp_init() 이 새 서술자에 복사하고,
 *   irq_setup_affinity() 가 자동 선택의 출발점으로 쓴다.
 * 값 범위: 최소 한 CPU 는 켜져 있음이 보장된다 — 비면 인터럽트가
 *   갈 곳을 잃는다.
 * 동기화: 부팅 후에는 /proc 쓰기로만 바뀌고, 그 경로가 값을 통째로
 *   덮는다. 읽는 쪽이 낡은 값을 봐도 다음 인터럽트부터 반영되므로
 *   문제가 되지 않는다.
 *
 * static 이 아닌 것에 주목: proc.c 가 직접 읽고 쓴다. */

/*
 * [한국어]
 * __irq_can_set_affinity - 친화도를 바꿀 수 있는 인터럽트인지 본다
 *
 * @desc: 대상 서술자
 * @return: true 바꿀 수 있음, false 없음
 *
 * 세 가지를 확인한다. 서술자가 있는가, 부하 분산이 허용되는가
 * (IRQD_NO_BALANCING 이나 per-CPU 가 아닌가), 칩이 친화도 설정
 * 콜백을 제공하는가.
 *
 * irqd_can_balance 가 걸러 내는 것: per-CPU 인터럽트는 CPU 마다
 * 별개라 친화도라는 개념이 없고, NO_BALANCING 인터럽트는 드라이버가
 * 일부러 고정해 둔 것이다.
 *
 * 실행 컨텍스트: 제약 없음. 락을 잡지 않는데, 이 값들이 인터럽트
 * 설정 시점에 정해지고 거의 바뀌지 않기 때문이다.
 *
 * 호출 체인:
 *   irq_can_set_affinity() / irq_can_set_affinity_usr() /
 *   irq_setup_affinity() → [이 함수]
 */
static bool __irq_can_set_affinity(struct irq_desc *desc)
{
	if (!desc || !irqd_can_balance(&desc->irq_data) ||	/* [한국어] per-CPU 인터럽트는 친화도라는 개념이 없고, NO_BALANCING 은 드라이버가 일부러 고정한 것이다 */
	    !desc->irq_data.chip || !desc->irq_data.chip->irq_set_affinity)	/* [한국어] 칩이 친화도 설정을 지원하는가 */
		return false;
	return true;	/* [한국어] 세 조건을 모두 통과했다. 친화도를 바꿀 수 있는 인터럽트다 */
}

/**
 * irq_can_set_affinity - Check if the affinity of a given irq can be set
 * @irq:	Interrupt to check
 *
 */
/*
 * [한국어]
 * irq_can_set_affinity - 친화도를 바꿀 수 있는 인터럽트인지 묻는다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 0 이 아니면 바꿀 수 있음
 *
 * 위 내부 함수의 번호 버전이다. 반환 타입이 int 인 것이 아래
 * _usr 판의 bool 과 다른데, 오래된 API 라 그렇다.
 *
 * 드라이버가 친화도를 설정하기 전에 미리 물어볼 수 있게 한다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   드라이버 / kernel/irq/proc.c → [이 함수]
 */
int irq_can_set_affinity(unsigned int irq)
{
	return __irq_can_set_affinity(irq_to_desc(irq));	/* [한국어] 반환 타입이 int 인 것은 오래된 API 의 흔적이다 */
}

/**
 * irq_can_set_affinity_usr - Check if affinity of a irq can be set from user space
 * @irq:	Interrupt to check
 *
 * Like irq_can_set_affinity() above, but additionally checks for the
 * AFFINITY_MANAGED flag.
 */
/*
 * [한국어]
 * irq_can_set_affinity_usr - 사용자 공간이 친화도를 바꿀 수 있는지 묻는다
 *
 * @irq: 대상 인터럽트 번호
 * @return: true 사용자가 바꿀 수 있음
 *
 * 위 함수에 관리형 검사를 더한 것이다. 관리형 인터럽트는 커널이
 * 친화도를 자동으로 정하고, 사용자가 그것을 바꾸면 다중 큐 장치의
 * 큐-CPU 대응이 깨진다.
 *
 * 그래서 /proc/irq/N/smp_affinity 쓰기 경로가 이 함수를 쓴다. 그
 * 파일은 관리형 인터럽트에 대해서는 읽기만 되고 쓰기는 거절된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   kernel/irq/proc.c 의 write_irq_affinity() → [이 함수]
 */
bool irq_can_set_affinity_usr(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	return __irq_can_set_affinity(desc) &&	/* [한국어] 기본 조건 */
		!irqd_affinity_is_managed(&desc->irq_data);	/* [한국어] 관리형이면 거절한다. 사용자가 바꾸면 다중 큐 장치의 큐-CPU 대응이 깨진다 */
}

/**
 * irq_set_thread_affinity - Notify irq threads to adjust affinity
 * @desc:	irq descriptor which has affinity changed
 *
 * Just set IRQTF_AFFINITY and delegate the affinity setting to the
 * interrupt thread itself. We can not call set_cpus_allowed_ptr() here as
 * we hold desc->lock and this code can be called from hard interrupt
 * context.
 */
/*
 * [한국어]
 * irq_set_thread_affinity - 인터럽트 스레드에게 친화도 변경을 알린다
 *
 * @desc: 친화도가 바뀐 서술자
 * @return: 없음
 *
 * 인터럽트의 친화도가 바뀌면 그 스레드 핸들러도 같은 CPU 로 옮겨야
 * 한다. 그런데 여기서 직접 옮길 수 없다.
 *
 * 원본 주석이 이유를 짚는다. set_cpus_allowed_ptr() 는 잠들 수 있는데,
 * 이 함수는 desc->lock 을 쥔 채 불리고 하드 인터럽트 문맥일 수도 있다.
 *
 * 그래서 플래그만 세우고 스레드를 깨운다. 스레드가 깨어나
 * irq_thread_check_affinity() 에서 그 플래그를 보고 자기 문맥에서
 * 옮긴다. 잠들 수 있는 일을 잠들 수 있는 곳으로 미루는 방식이다.
 *
 * secondary 스레드까지 챙기는 것에 주목: 강제 스레드화로 만들어진
 * 보조 스레드도 같은 CPU 로 옮겨야 한다.
 *
 * 실행 컨텍스트: desc->lock 보유. 하드 인터럽트 문맥일 수 있다.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → [이 함수] → wake_up_process()
 */
static void irq_set_thread_affinity(struct irq_desc *desc)
{
	struct irqaction *action;	/* [한국어] 순회용 */

	for_each_action_of_desc(desc, action) {	/* [한국어] 공유 인터럽트면 여러 등록자가 있고 각자 스레드를 가질 수 있다 */
		if (action->thread) {	/* [한국어] 스레드 핸들러가 있는가 */
			set_bit(IRQTF_AFFINITY, &action->thread_flags);	/* [한국어] 직접 옮기지 않고 표시만 한다. set_cpus_allowed_ptr 은 잠들 수 있는데 여기는 락을 쥔 인터럽트 문맥일 수 있다 */
			wake_up_process(action->thread);	/* [한국어] 스레드가 깨어나 자기 문맥에서 옮긴다 */
		}
		if (action->secondary && action->secondary->thread) {	/* [한국어] 강제 스레드화로 만들어진 보조 스레드가 있는가 */
			set_bit(IRQTF_AFFINITY, &action->secondary->thread_flags);	/* [한국어] 그것도 같은 CPU 로 옮겨야 한다 */
			wake_up_process(action->secondary->thread);	/* [한국어] 보조 스레드도 깨워 자기 문맥에서 옮기게 한다 */
		}
	}
}

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 하드웨어가 실제로 고른 CPU 를 추적하는 빌드 */
/*
 * [한국어]
 * irq_validate_effective_affinity - 칩이 유효 친화도를 갱신했는지 확인한다
 *
 * @data: 대상 irq_data
 * @return: 없음 (문제가 있으면 경고만)
 *
 * 요청 친화도와 유효 친화도의 차이가 이 함수의 배경이다. 사용자가
 * "CPU 0~3 중 아무 데나" 라고 요청하면, 컨트롤러는 그중 하나를 골라
 * 실제로 그리로 보낸다. 그 고른 결과가 유효 친화도다.
 *
 * 그것을 채우는 것은 칩의 irq_set_affinity 콜백 몫이다. 안 채우면
 * 유효 친화도가 비어 있게 되고, 그 값을 쓰는 코드들이 오동작한다 —
 * 스레드 친화도 설정, 디먹스 리다이렉트 판단, /proc 표시가 그렇다.
 *
 * pr_warn_once 인 것에 주목: 드라이버 버그이므로 한 번만 알린다.
 * 매번 찍으면 그 인터럽트가 올 때마다 로그가 넘친다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → [이 함수]
 */
static void irq_validate_effective_affinity(struct irq_data *data)
{
	const struct cpumask *m = irq_data_get_effective_affinity_mask(data);	/* [한국어] 하드웨어가 실제로 고른 CPU 들 */
	struct irq_chip *chip = irq_data_get_irq_chip(data);	/* [한국어] 경고에 이름을 찍을 칩 */

	if (!cpumask_empty(m))	/* [한국어] 칩이 제대로 채웠는가 */
		return;
	pr_warn_once("irq_chip %s did not update eff. affinity mask of irq %u\n",	/* [한국어] 드라이버 버그다. 비어 있으면 스레드 친화도·리다이렉트 판단·/proc 표시가 모두 오동작한다 */
		     chip->name, data->irq);	/* [한국어] once 인 이유: 매번 찍으면 그 인터럽트가 올 때마다 로그가 넘친다 */
}
#else	/* [한국어] 유효 친화도를 추적하지 않는 빌드 */
/*
 * [한국어]
 * irq_validate_effective_affinity - 유효 친화도 검증 (빈 함수)
 *
 * @data: 무시
 * @return: 없음
 *
 * 유효 친화도를 추적하지 않는 빌드에는 검증할 대상이 없다.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → [이 함수]
 */
static inline void irq_validate_effective_affinity(struct irq_data *data) { }	/* [한국어] 검증할 대상이 없다 */
#endif	/* [한국어] 유효 친화도 분기의 끝 */

static DEFINE_PER_CPU(struct cpumask, __tmp_mask);
/* [한국어] 친화도 계산에 쓰는 CPU 별 임시 마스크.
 * 설정자·읽는 자: 아래 irq_do_set_affinity() 만 쓴다.
 * 값 범위: 계산 중간 결과. 함수를 벗어나면 의미가 없다.
 * 동기화: per-CPU 이고 그 함수가 인터럽트를 끈 채(desc->lock 보유)
 *   불리므로, 같은 CPU 에서 중첩되지 않는다.
 *
 * 왜 스택이 아니라 per-CPU 인가: cpumask 는 CPU 수에 비례해 커진다.
 * CPU 가 수천 개인 기계에서는 수백 바이트가 되어 인터럽트 문맥의
 * 좁은 스택에 두기 부담스럽다. 그렇다고 매번 할당하면 잠들 수 없는
 * 문맥에서 쓸 수 없다. per-CPU 정적 변수가 그 절충이다. */

/*
 * [한국어]
 * irq_do_set_affinity - 친화도를 실제로 하드웨어에 설정한다
 *
 * @data:  대상 irq_data
 * @mask:  요청된 CPU 마스크
 * @force: 온라인 검사를 무시할지
 * @return: 0 성공, 음수 오류
 *
 * 친화도 설정의 중심이다. 요청받은 마스크를 그대로 칩에 넘기지 않고
 * 두 단계로 걸러 낸다.
 *
 * 첫째, 격리 CPU 회피. 원본 주석이 길게 설명하는 부분이다. 관리형
 * 인터럽트가 격리된 CPU 로 가면, 그 CPU 에서 실행되는 실시간 작업이
 * 인터럽트에 방해받는다. 그런데 그 격리 CPU 가 직접 I/O 를 냈다면
 * 그 완료 인터럽트는 그리로 가는 것이 맞다. 그 구분을 하우스키핑
 * 마스크와의 교집합으로 표현한다 — 교집합에 온라인 CPU 가 있으면
 * 그쪽만 쓰고, 없으면 요청을 그대로 존중한다.
 *
 * 둘째, 온라인 CPU 검사. 꺼진 CPU 로 인터럽트를 보내면 아무도 받지
 * 않는다. force 는 CPU 핫플러그가 아직 온라인이 아닌 CPU 에 미리
 * 설정할 때 쓰는 예외다.
 *
 * 마지막 switch 가 칩의 응답을 해석한다. OK 계열은 요청 마스크를
 * 서술자에 기록하고, NOCOPY 는 칩이 이미 자기가 원하는 값을 넣었다는
 * 뜻이라 기록하지 않는다. 세 경우 모두 유효 친화도를 검증하고 스레드에
 * 알린다.
 *
 * 기록하는 것이 tmp_mask 가 아니라 원래 mask 인 것에 주목: 사용자가
 * 요청한 값을 보존해야 한다. 격리 회피로 좁혀진 값을 기록하면
 * 나중에 CPU 상태가 바뀌었을 때 원래 의도를 되살릴 수 없다.
 *
 * 실행 컨텍스트: desc->lock 보유, 인터럽트가 꺼진 상태.
 *
 * 호출 체인:
 *   irq_try_set_affinity() / irq_setup_affinity() / irq_startup() →
 *   [이 함수] → chip->irq_set_affinity()
 */
int irq_do_set_affinity(struct irq_data *data, const struct cpumask *mask, bool force)
{
	struct cpumask *tmp_mask = this_cpu_ptr(&__tmp_mask);	/* [한국어] 계산용 임시 마스크. 인터럽트가 꺼진 상태라 같은 CPU 에서 중첩되지 않는다 */
	struct irq_desc *desc = irq_data_to_desc(data);	/* [한국어] 결과를 기록할 서술자 */
	struct irq_chip *chip = irq_data_get_irq_chip(data);	/* [한국어] 설정을 넘길 칩 */
	const struct cpumask  *prog_mask;	/* [한국어] 첫 단계를 거친 마스크 */
	int ret;	/* [한국어] 칩의 응답 */

	if (!chip || !chip->irq_set_affinity)	/* [한국어] 친화도 설정을 지원하는 칩인가 */
		return -EINVAL;

	/*
	 * If this is a managed interrupt and housekeeping is enabled on
	 * it check whether the requested affinity mask intersects with
	 * a housekeeping CPU. If so, then remove the isolated CPUs from
	 * the mask and just keep the housekeeping CPU(s). This prevents
	 * the affinity setter from routing the interrupt to an isolated
	 * CPU to avoid that I/O submitted from a housekeeping CPU causes
	 * interrupts on an isolated one.
	 *
	 * If the masks do not intersect or include online CPU(s) then
	 * keep the requested mask. The isolated target CPUs are only
	 * receiving interrupts when the I/O operation was submitted
	 * directly from them.
	 *
	 * If all housekeeping CPUs in the affinity mask are offline, the
	 * interrupt will be migrated by the CPU hotplug code once a
	 * housekeeping CPU which belongs to the affinity mask comes
	 * online.
	 */
	if (irqd_affinity_is_managed(data) &&	/* [한국어] (위 영어 주석) 커널이 친화도를 관리하는 인터럽트이고 */
	    housekeeping_enabled(HK_TYPE_MANAGED_IRQ)) {	/* [한국어] 격리 설정이 켜져 있는가 */
		const struct cpumask *hk_mask;	/* [한국어] 격리되지 않은 CPU 들 */

		hk_mask = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ);	/* [한국어] 일반 작업을 맡는 CPU 집합 */

		cpumask_and(tmp_mask, mask, hk_mask);	/* [한국어] 요청과의 교집합 */
		if (!cpumask_intersects(tmp_mask, cpu_online_mask))	/* [한국어] 그중 살아 있는 CPU 가 있는가 */
			prog_mask = mask;	/* [한국어] 없으면 요청을 그대로 존중한다. 격리 CPU 가 직접 낸 I/O 의 완료 인터럽트는 그리로 가는 것이 맞다 */
		else	/* [한국어] 있으면 */
			prog_mask = tmp_mask;	/* [한국어] 격리 CPU 를 빼고 하우스키핑 CPU 로만 보낸다 */
	} else {	/* [한국어] 관리형이 아니거나 격리 설정이 꺼져 있는 경우 */
		prog_mask = mask;	/* [한국어] 요청 그대로 */
	}

	/*
	 * Make sure we only provide online CPUs to the irqchip,
	 * unless we are being asked to force the affinity (in which
	 * case we do as we are told).
	 */
	cpumask_and(tmp_mask, prog_mask, cpu_online_mask);	/* [한국어] (위 영어 주석) 살아 있는 CPU 만 남긴다 */
	if (!force && !cpumask_empty(tmp_mask))	/* [한국어] 강제가 아니고 살아 있는 대상이 있는가 */
		ret = chip->irq_set_affinity(data, tmp_mask, force);	/* [한국어] 정상 경로. 꺼진 CPU 로 보내면 아무도 받지 않는다 */
	else if (force)	/* [한국어] 강제 설정인가 */
		ret = chip->irq_set_affinity(data, mask, force);	/* [한국어] CPU 핫플러그가 아직 온라인이 아닌 CPU 에 미리 설정하는 경우다. 요청 마스크를 그대로 넘긴다 */
	else	/* [한국어] 강제도 아닌데 살아 있는 대상이 없다 */
		ret = -EINVAL;	/* [한국어] 갈 곳이 없는 설정이다 */

	switch (ret) {	/* [한국어] 칩의 응답을 해석한다 */
	case IRQ_SET_MASK_OK:	/* [한국어] 설정했고 코어가 마스크를 기록해야 한다 */
	case IRQ_SET_MASK_OK_DONE:	/* [한국어] 설정했고 코어가 더 할 일이 없다 */
		cpumask_copy(desc->irq_common_data.affinity, mask);	/* [한국어] tmp_mask 가 아니라 원래 요청을 기록한다. 격리 회피로 좁힌 값을 저장하면 CPU 상태가 바뀌었을 때 원래 의도를 되살릴 수 없다 */
		fallthrough;	/* [한국어] 아래 공통 처리로 */

	case IRQ_SET_MASK_OK_NOCOPY:	/* [한국어] 칩이 이미 자기가 원하는 값을 넣었다는 뜻이라 기록하지 않는다 */
		irq_validate_effective_affinity(data);	/* [한국어] 칩이 유효 친화도를 채웠는지 확인 */
		irq_set_thread_affinity(desc);	/* [한국어] 스레드 핸들러도 같은 CPU 로 옮기라고 알린다 */
		ret = 0;	/* [한국어] 세 응답 모두 성공으로 통일한다. 호출자가 IRQ_SET_MASK_ 상수를 몰라도 되게 한다 */
	}

	return ret;	/* [한국어] 0 또는 칩이 낸 오류 */
}

#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 친화도 변경을 미뤄 두는 기구가 있는 빌드. x86 처럼 인터럽트 처리 중에 벡터를 바꾸면 위험한 아키텍처가 쓴다 */
/*
 * [한국어]
 * irq_set_affinity_pending - 친화도 변경을 미뤄 둔다
 *
 * @data: 대상 irq_data
 * @dest: 원하는 CPU 마스크
 * @return: 항상 0
 *
 * 지금 당장 친화도를 바꿀 수 없을 때, 원하는 값을 서술자에 적어 두고
 * 나중에 적용되게 한다.
 *
 * 왜 미뤄야 하는가: x86 에서 인터럽트가 CPU 에 전달되는 중에 벡터를
 * 바꾸면 그 인터럽트를 잃거나 엉뚱한 벡터로 받는다. 안전한 시점은
 * 그 인터럽트의 처리가 끝난 직후인데, 그것은 인터럽트 문맥이다.
 *
 * 그래서 여기서는 표시만 남기고, 실제 적용은 인터럽트 처리 끝에
 * irq_move_irq() (kernel/irq/migration.c)가 한다.
 *
 * 호출자에게 성공을 알리는 것에 주목: 사용자 입장에서는 요청이
 * 받아들여진 것이 맞다. 반영이 조금 늦을 뿐이다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_try_set_affinity() / irq_set_affinity_locked() → [이 함수]
 */
static inline int irq_set_affinity_pending(struct irq_data *data,
					   const struct cpumask *dest)
{
	struct irq_desc *desc = irq_data_to_desc(data);	/* [한국어] 미뤄 둔 마스크를 담을 서술자 */

	irqd_set_move_pending(data);	/* [한국어] "적용할 변경이 있다" 는 표시. 인터럽트 처리 끝에 irq_move_irq 가 이것을 본다 */
	irq_copy_pending(desc, dest);	/* [한국어] 원하는 마스크를 pending_mask 에 적어 둔다 */
	return 0;	/* [한국어] 사용자 입장에서는 요청이 받아들여진 것이 맞다. 반영이 조금 늦을 뿐이다 */
}
#else	/* [한국어] 미루기 기구가 없는 빌드 */
/*
 * [한국어]
 * irq_set_affinity_pending - 친화도 변경 미루기 (지원 없음)
 *
 * @data: 무시
 * @dest: 무시
 * @return: 항상 -EBUSY
 *
 * 미룰 수 없으므로 호출자에게 실패를 알린다. 그 값이 사용자 공간까지
 * 올라가 "지금은 바꿀 수 없다" 가 된다.
 *
 * 호출 체인:
 *   irq_try_set_affinity() → [이 함수]
 */
static inline int irq_set_affinity_pending(struct irq_data *data,
					   const struct cpumask *dest)
{
	return -EBUSY;	/* [한국어] 미룰 수 없다. 이 값이 사용자 공간까지 올라간다 */
}
#endif	/* [한국어] CONFIG_GENERIC_PENDING_IRQ 분기의 끝 */

/*
 * [한국어]
 * irq_try_set_affinity - 친화도 설정을 시도하고 실패하면 미뤄 둔다
 *
 * @data:  대상 irq_data
 * @dest:  원하는 CPU 마스크
 * @force: 강제 설정인가
 * @return: 0 성공 또는 미뤄 둠, 음수 오류
 *
 * 벡터 관리가 바빠 지금 바꿀 수 없는 경우를 구제한다.
 *
 * -EBUSY 가 무엇을 뜻하는가: x86 의 벡터 관리 코드가 이 인터럽트의
 * 이전 이동이 아직 끝나지 않았을 때 그것을 돌려준다. 그 상태에서
 * 또 옮기면 벡터 회수가 꼬인다.
 *
 * 원본 주석대로 그럴 때 사용자에게 오류를 돌려주는 대신 미뤄 둔다.
 * 사용자가 echo 로 친화도를 바꾸다 이유 없이 실패하는 것보다 낫다.
 *
 * force 일 때는 미루지 않는 것에 주목: 강제 설정은 CPU 핫플러그처럼
 * 지금 반드시 반영돼야 하는 경우다. 미루면 그 CPU 가 내려간 뒤에
 * 적용되어 의미가 없다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_affinity_locked() → [이 함수] → irq_do_set_affinity()
 */
static int irq_try_set_affinity(struct irq_data *data,
				const struct cpumask *dest, bool force)
{
	int ret = irq_do_set_affinity(data, dest, force);	/* [한국어] 먼저 지금 설정을 시도한다 */

	/*
	 * In case that the underlying vector management is busy and the
	 * architecture supports the generic pending mechanism then utilize
	 * this to avoid returning an error to user space.
	 */
	if (ret == -EBUSY && !force)	/* [한국어] (위 영어 주석) 벡터 관리가 바쁘고 강제가 아닌가. force 일 때 미루지 않는 이유는 CPU 핫플러그처럼 지금 반영돼야 하는 경우이기 때문이다 */
		ret = irq_set_affinity_pending(data, dest);	/* [한국어] 미뤄 둔다. 사용자가 이유 없이 실패를 보는 것보다 낫다 */
	return ret;	/* [한국어] 0, 칩의 오류, 또는 콜백이 없다는 뜻의 -ENXIO */
}

/*
 * [한국어]
 * irq_set_affinity_deactivated - 활성화 전이면 마스크만 저장한다
 *
 * @data: 대상 irq_data
 * @mask: 원하는 CPU 마스크
 * @return: true 저장만 하고 끝냈음, false 정상 경로로 진행할 것
 *
 * 어떤 칩은 활성화된 상태에서만 친화도를 다룰 수 있다. 아직 자원이
 * 배정되지 않았는데 친화도를 설정하려 하면 실패하거나 엉뚱한 동작을
 * 한다.
 *
 * 그런 칩(IRQD_AFFINITY_ON_ACTIVATE 표시)에 대해서는 마스크만
 * 저장하고 칩을 부르지 않는다. 원본 주석대로, 나중에 활성화할 때
 * 드라이버가 어차피 인터럽트를 쓸 수 있는 상태로 만들어야 하므로
 * 그때 이 마스크가 반영된다.
 *
 * 유효 친화도까지 함께 채우는 것에 주목: 칩을 부르지 않았으니
 * 아무도 채워 주지 않는다. 그 값을 읽는 코드가 빈 마스크를 보지
 * 않도록 요청값을 그대로 넣어 둔다.
 *
 * IRQD_AFFINITY_SET 을 세우는 이유: 이후 자동 선택
 * (irq_setup_affinity)이 이 값을 존중하게 만든다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_affinity_locked() → [이 함수]
 */
static bool irq_set_affinity_deactivated(struct irq_data *data,
					 const struct cpumask *mask)
{
	struct irq_desc *desc = irq_data_to_desc(data);	/* [한국어] 마스크를 저장할 서술자 */

	/*
	 * Handle irq chips which can handle affinity only in activated
	 * state correctly
	 *
	 * If the interrupt is not yet activated, just store the affinity
	 * mask and do not call the chip driver at all. On activation the
	 * driver has to make sure anyway that the interrupt is in a
	 * usable state so startup works.
	 */
	if (!IS_ENABLED(CONFIG_IRQ_DOMAIN_HIERARCHY) ||	/* [한국어] (위 영어 주석) 계층형이 없거나 */
	    irqd_is_activated(data) || !irqd_affinity_on_activate(data))	/* [한국어] 이미 활성화됐거나, 활성화 후에만 친화도를 다루는 칩이 아닌가 */
		return false;	/* [한국어] 정상 경로로 진행하라 */

	cpumask_copy(desc->irq_common_data.affinity, mask);	/* [한국어] 요청 마스크만 저장한다. 활성화 때 드라이버가 이것을 반영한다 */
	irq_data_update_effective_affinity(data, mask);	/* [한국어] 칩을 부르지 않았으니 아무도 유효 친화도를 채워 주지 않는다. 그 값을 읽는 코드가 빈 마스크를 보지 않게 요청값을 그대로 넣는다 */
	irqd_set(data, IRQD_AFFINITY_SET);	/* [한국어] 이후 자동 선택이 이 값을 존중하게 만든다 */
	return true;	/* [한국어] 여기서 끝났다 */
}

/**
 * irq_affinity_schedule_notify_work - Schedule work to notify about affinity change
 * @desc:  Interrupt descriptor whose affinity changed
 */
/*
 * [한국어]
 * irq_affinity_schedule_notify_work - 친화도 변경 알림 작업을 예약한다
 *
 * @desc: 친화도가 바뀐 서술자
 * @return: 없음
 *
 * 드라이버가 친화도 변경을 알고 싶어 할 수 있다 — 다중 큐 장치가
 * 큐-CPU 대응을 다시 계산하려는 경우다. 그 알림은 잠들 수 있는
 * 작업이라 여기서 직접 부를 수 없고, 워크큐에 넘긴다.
 *
 * 참조 카운트 관리가 이 짧은 함수의 핵심이다. 작업이 큐에 있는 동안
 * 알림 구조체가 사라지면 안 되므로 참조를 하나 잡는다. 그런데
 * schedule_work 가 거짓을 돌려주면 이미 큐에 있다는 뜻이고, 그때는
 * 앞서 잡은 참조가 유효하므로 방금 잡은 것을 놓는다.
 *
 * 그 처리가 없으면 친화도를 여러 번 바꿀 때마다 참조가 쌓여, 알림
 * 구조체가 영영 해제되지 않는다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_affinity_locked() / kernel/irq/migration.c → [이 함수]
 */
void irq_affinity_schedule_notify_work(struct irq_desc *desc)
{
	lockdep_assert_held(&desc->lock);	/* [한국어] affinity_notify 포인터가 이 락 아래에서 바뀐다 */

	kref_get(&desc->affinity_notify->kref);	/* [한국어] 작업이 큐에 있는 동안 알림 구조체가 사라지면 안 된다 */
	if (!schedule_work(&desc->affinity_notify->work)) {	/* [한국어] 거짓이면 이미 큐에 있다는 뜻이다 */
		/* Work was already scheduled, drop our extra ref */
		kref_put(&desc->affinity_notify->kref, desc->affinity_notify->release);	/* [한국어] (위 영어 주석) 앞서 잡은 참조가 유효하므로 방금 것을 놓는다. 이 처리가 없으면 친화도를 바꿀 때마다 참조가 쌓여 영영 해제되지 않는다 */
	}
}

/*
 * [한국어]
 * irq_set_affinity_locked - 친화도 설정의 공통 절차 (락 보유)
 *
 * @data:  대상 irq_data
 * @mask:  원하는 CPU 마스크
 * @force: 강제 설정인가
 * @return: 0 성공, 음수 오류
 *
 * 친화도 설정 경로가 모두 여기로 모인다. 세 갈래를 거친다.
 *
 * (1) 활성화 전이고 그런 칩이면 마스크만 저장하고 끝.
 * (2) 인터럽트 문맥에서 옮길 수 있고 미뤄 둔 변경이 없으면 지금 설정.
 * (3) 아니면 미뤄 둔다.
 *
 * (2) 의 두 조건이 각각 다른 이유다. irq_can_move_pcntxt 는 "프로세스
 * 문맥에서 벡터를 바꿔도 안전한가" 를 뜻한다 — x86 은 아니고 대부분의
 * 다른 아키텍처는 그렇다. 두 번째 조건은 이미 미뤄 둔 변경이 있으면
 * 그것을 덮지 않고 새 값으로 갱신만 하겠다는 뜻이다.
 *
 * IRQD_AFFINITY_SET 을 마지막에 무조건 세우는 것에 주목: 설정이
 * 실패했어도 세운다. 사용자가 값을 지정했다는 사실 자체가 기록돼야,
 * 이후 자동 선택이 기본값으로 덮어쓰지 않는다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   __irq_set_affinity() / kernel/irq/proc.c → [이 함수]
 */
int irq_set_affinity_locked(struct irq_data *data, const struct cpumask *mask,
			    bool force)
{
	struct irq_chip *chip = irq_data_get_irq_chip(data);	/* [한국어] 설정을 넘길 칩 */
	struct irq_desc *desc = irq_data_to_desc(data);	/* [한국어] 알림과 미루기에 쓸 서술자 */
	int ret = 0;	/* [한국어] 결과 */

	if (!chip || !chip->irq_set_affinity)	/* [한국어] 친화도 설정을 지원하는가 */
		return -EINVAL;

	if (irq_set_affinity_deactivated(data, mask))	/* [한국어] 활성화 전이고 그런 칩인가 */
		return 0;	/* [한국어] 마스크만 저장하고 끝냈다 */

	if (irq_can_move_pcntxt(data) && !irqd_is_setaffinity_pending(data)) {	/* [한국어] 프로세스 문맥에서 벡터를 바꿔도 안전하고, 이미 미뤄 둔 변경이 없는가 */
		ret = irq_try_set_affinity(data, mask, force);	/* [한국어] 지금 설정한다 */
	} else {	/* [한국어] 지금 바꿀 수 없거나 이미 미뤄 둔 변경이 있는 경우 */
		irqd_set_move_pending(data);	/* [한국어] 미룸 표시 */
		irq_copy_pending(desc, mask);	/* [한국어] 새 값으로 갱신한다. 미뤄 둔 것이 있었다면 덮어쓰는 셈인데, 마지막 요청이 이기는 것이 맞다 */
	}

	if (desc->affinity_notify)	/* [한국어] 변경을 알고 싶어 하는 드라이버가 있는가 */
		irq_affinity_schedule_notify_work(desc);	/* [한국어] 잠들 수 있는 알림이라 워크큐로 넘긴다 */

	irqd_set(data, IRQD_AFFINITY_SET);	/* [한국어] 실패했어도 세운다. 사용자가 값을 지정했다는 사실이 기록돼야 이후 자동 선택이 기본값으로 덮어쓰지 않는다 */

	return ret;	/* [한국어] 0 이면 트리거가 하드웨어에 반영됐다 */
}

/**
 * irq_update_affinity_desc - Update affinity management for an interrupt
 * @irq:	The interrupt number to update
 * @affinity:	Pointer to the affinity descriptor
 *
 * This interface can be used to configure the affinity management of
 * interrupts which have been allocated already.
 *
 * There are certain limitations on when it may be used - attempts to use it
 * for when the kernel is configured for generic IRQ reservation mode (in
 * config GENERIC_IRQ_RESERVATION_MODE) will fail, as it may conflict with
 * managed/non-managed interrupt accounting. In addition, attempts to use it on
 * an interrupt which is already started or which has already been configured
 * as managed will also fail, as these mean invalid init state or double init.
 */
/*
 * [한국어]
 * irq_update_affinity_desc - 이미 할당된 인터럽트를 관리형으로 바꾼다
 *
 * @irq:      대상 인터럽트 번호
 * @affinity: 새 친화도 설명자 (관리형 여부와 마스크)
 * @return:   0 성공, -EOPNOTSUPP 예약 모드 빌드, -EBUSY 상태가 맞지 않음
 *
 * 보통 관리형 여부는 인터럽트를 할당할 때 정해진다. 이 함수는 그것을
 * 나중에 바꾼다 — 플랫폼 장치가 디바이스 트리를 보고 뒤늦게
 * 관리형으로 만들어야 하는 경우다.
 *
 * 제약이 많은 것이 원본 주석의 요지다.
 *
 * 예약 모드 빌드에서 거절하는 이유: 그 모드는 관리형·비관리형 벡터를
 * 따로 세는데, 중간에 종류가 바뀌면 그 계산이 어긋난다.
 *
 * 이미 시작된 인터럽트를 거절하는 이유: 동작 중에 관리 정책을 바꾸면
 * 그 사이의 상태가 정의되지 않는다.
 *
 * 이미 관리형인 것을 거절하는 이유: 이중 초기화다.
 *
 * 비활성화했다 다시 활성화하는 것에 주목: 관리형 여부가 자원 배정
 * 방식을 바꾸므로, 앞선 활성화가 만든 것을 되돌리고 새 설정으로 다시
 * 해야 한다. 원래 활성화돼 있던 경우에만 복원하는 것이 중요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 장치 드라이버 → [이 함수] → irq_domain_deactivate_irq() /
 *   irq_domain_activate_irq()
 */
int irq_update_affinity_desc(unsigned int irq, struct irq_affinity_desc *affinity)
{
	/*
	 * Supporting this with the reservation scheme used by x86 needs
	 * some more thought. Fail it for now.
	 */
	if (IS_ENABLED(CONFIG_GENERIC_IRQ_RESERVATION_MODE))	/* [한국어] (위 영어 주석) 예약 모드는 관리형·비관리형 벡터를 따로 센다. 중간에 종류가 바뀌면 그 계산이 어긋난다 */
		return -EOPNOTSUPP;

	scoped_irqdesc_get_and_buslock(irq, 0) {	/* [한국어] 서술자 조회 + 버스 락 + 스핀락. 아래 활성화 경로가 잠들 수 있다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */
		bool activated;	/* [한국어] 원래 활성화돼 있었는가 */

		/* Requires the interrupt to be shut down */
		if (irqd_is_started(&desc->irq_data))	/* [한국어] (위 영어 주석) 이미 동작 중인가 */
			return -EBUSY;	/* [한국어] 동작 중에 관리 정책을 바꾸면 그 사이의 상태가 정의되지 않는다 */

		/* Interrupts which are already managed cannot be modified */
		if (irqd_affinity_is_managed(&desc->irq_data))	/* [한국어] (위 영어 주석) 이미 관리형인가 */
			return -EBUSY;	/* [한국어] 이중 초기화다 */
		/*
		 * Deactivate the interrupt. That's required to undo
		 * anything an earlier activation has established.
		 */
		activated = irqd_is_activated(&desc->irq_data);	/* [한국어] (위 영어 주석) 원래 상태를 기억해 둔다 */
		if (activated)	/* [한국어] 활성화돼 있었다면 */
			irq_domain_deactivate_irq(&desc->irq_data);	/* [한국어] 되돌린다. 관리형 여부가 자원 배정 방식을 바꾸므로 앞선 배정을 무효화해야 한다 */

		if (affinity->is_managed) {	/* [한국어] 관리형으로 만드는가 */
			irqd_set(&desc->irq_data, IRQD_AFFINITY_MANAGED);	/* [한국어] 사용자가 친화도를 바꾸지 못하게 된다 */
			irqd_set(&desc->irq_data, IRQD_MANAGED_SHUTDOWN);	/* [한국어] 담당 CPU 가 없으면 꺼진 상태로 시작한다 */
		}

		cpumask_copy(desc->irq_common_data.affinity, &affinity->mask);	/* [한국어] 새 친화도 마스크 */

		/* Restore the activation state */
		if (activated)	/* [한국어] (위 영어 주석) 원래 활성화돼 있었다면 */
			irq_domain_activate_irq(&desc->irq_data, false);	/* [한국어] 새 설정으로 다시 활성화한다. 원래 아니었다면 그대로 둔다 */
		return 0;	/* [한국어] 설정을 저장했다. 실제 반영은 활성화 때 드라이버가 한다 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}

/*
 * [한국어]
 * __irq_set_affinity - 번호로 서술자를 찾아 친화도를 설정한다
 *
 * @irq:   대상 인터럽트 번호
 * @mask:  원하는 CPU 마스크
 * @force: 강제 설정인가
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음, 그 외 설정 오류
 *
 * 공개 API 인 irq_set_affinity() 와 irq_force_affinity() 가 force 만
 * 달리해 부르는 공통 구현이다.
 *
 * irqsave 판을 쓰는 것에 주목: 이 함수가 인터럽트 문맥에서도 불릴 수
 * 있다. CPU 핫플러그 경로가 그렇다.
 *
 * 버스 락을 잡지 않는 것도 눈에 띈다. 친화도 설정은 대개 벡터 관리
 * 계층에서 처리되고 느린 버스 뒤의 칩을 건드리지 않는다.
 *
 * 실행 컨텍스트: 제약이 비교적 적다. 인터럽트를 끈 채 스핀락만 잡는다.
 *
 * 호출 체인:
 *   irq_set_affinity() / irq_force_affinity() / __irq_apply_affinity_hint()
 *   → [이 함수] → irq_set_affinity_locked()
 */
static int __irq_set_affinity(unsigned int irq, const struct cpumask *mask,
			      bool force)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return -EINVAL;

	guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] irqsave 판 — 이 함수가 CPU 핫플러그 경로에서 인터럽트 문맥으로도 불린다 */
	return irq_set_affinity_locked(irq_desc_get_irq_data(desc), mask, force);	/* [한국어] 공통 절차에 위임 */
}

/**
 * irq_set_affinity - Set the irq affinity of a given irq
 * @irq:	Interrupt to set affinity
 * @cpumask:	cpumask
 *
 * Fails if cpumask does not contain an online CPU
 */
/*
 * [한국어]
 * irq_set_affinity - 인터럽트의 친화도를 설정한다
 *
 * @irq:     대상 인터럽트 번호
 * @cpumask: 원하는 CPU 마스크
 * @return:  0 성공, 음수 오류
 *
 * 드라이버가 쓰는 표준 친화도 설정 API 다. 원본 주석대로 마스크에
 * 살아 있는 CPU 가 하나도 없으면 실패한다 — 갈 곳 없는 인터럽트를
 * 만들지 않기 위해서다.
 *
 * 다중 큐 장치가 큐마다 다른 CPU 를 지정할 때 쓴다. 다만 관리형
 * 인터럽트를 쓰는 요즘 장치는 커널이 알아서 배정하므로 이 함수를
 * 부를 일이 줄었다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __irq_set_affinity()
 */
int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	return __irq_set_affinity(irq, cpumask, false);	/* [한국어] false — 온라인 CPU 검사를 한다. 살아 있는 대상이 없으면 실패한다 */
}
EXPORT_SYMBOL_GPL(irq_set_affinity);	/* [한국어] 드라이버가 부른다 */

/**
 * irq_force_affinity - Force the irq affinity of a given irq
 * @irq:	Interrupt to set affinity
 * @cpumask:	cpumask
 *
 * Same as irq_set_affinity, but without checking the mask against
 * online cpus.
 *
 * Solely for low level cpu hotplug code, where we need to make per
 * cpu interrupts affine before the cpu becomes online.
 */
/*
 * [한국어]
 * irq_force_affinity - 온라인 검사 없이 친화도를 강제한다
 *
 * @irq:     대상 인터럽트 번호
 * @cpumask: 원하는 CPU 마스크
 * @return:  0 성공, 음수 오류
 *
 * 위 함수와 하나만 다르다 — 대상 CPU 가 아직 온라인이 아니어도
 * 설정한다.
 *
 * 원본 주석이 용도를 못 박는다. CPU 핫플러그 코드가 CPU 를 올리는
 * 도중에 그 CPU 의 per-CPU 인터럽트를 미리 설정해야 한다. 그 시점에는
 * 아직 온라인 표시가 되지 않아 일반 경로가 거절한다.
 *
 * 드라이버가 이것을 쓰면 안 된다. 꺼진 CPU 로 인터럽트를 보내면
 * 아무도 받지 않는다.
 *
 * 실행 컨텍스트: 제약 없음. 대개 CPU 핫플러그 콜백 안이다.
 *
 * 호출 체인:
 *   CPU 핫플러그 코드 → [이 함수] → __irq_set_affinity()
 */
int irq_force_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	return __irq_set_affinity(irq, cpumask, true);	/* [한국어] true — 온라인 검사를 건너뛴다. CPU 를 올리는 도중이라 아직 온라인 표시가 되지 않았다 */
}
EXPORT_SYMBOL_GPL(irq_force_affinity);	/* [한국어] 아키텍처 핫플러그 코드가 모듈일 수 있다 */

/*
 * [한국어]
 * __irq_apply_affinity_hint - 친화도 힌트를 기록하고 선택적으로 적용한다
 *
 * @irq:         대상 인터럽트 번호
 * @m:           힌트 마스크. NULL 이면 힌트를 지운다.
 * @setaffinity: 힌트를 실제 친화도로도 적용할지
 * @return:      0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * 힌트가 무엇인가: 드라이버가 "이 인터럽트는 이 CPU 들에서 처리되면
 * 좋겠다" 고 사용자 공간에 알리는 값이다. /proc/irq/N/affinity_hint 로
 * 노출되고, irqbalance 같은 데몬이 그것을 읽어 참고한다.
 *
 * 즉 힌트 자체는 강제력이 없다. setaffinity 가 참이면 힌트를 실제
 * 친화도로도 설정하지만, 그것은 별개의 동작이다.
 *
 * 두 동작을 락 밖에서 나눠 하는 것에 주목: 힌트 기록은 락 안에서,
 * 친화도 설정은 락 밖에서 한다. 그 설정 함수가 자기 락을 다시 잡기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_set_affinity_hint() 매크로 / 드라이버 → [이 함수]
 */
int __irq_apply_affinity_hint(unsigned int irq, const struct cpumask *m, bool setaffinity)
{
	int ret = -EINVAL;	/* [한국어] 서술자를 못 찾았을 때의 기본값 */

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] per-CPU 인터럽트는 힌트라는 개념이 없어 걸러 낸다 */
		scoped_irqdesc->affinity_hint = m;	/* [한국어] 포인터를 그대로 저장한다. 드라이버가 그 마스크를 계속 유지해야 한다 */
		ret = 0;	/* [한국어] 세 응답을 성공으로 통일해 호출자가 IRQ_SET_MASK_ 상수를 몰라도 되게 한다 */
	}

	if (!ret && m && setaffinity)	/* [한국어] 힌트를 실제 친화도로도 적용할지 */
		__irq_set_affinity(irq, m, false);	/* [한국어] 락 밖에서 부른다. 그 함수가 자기 락을 다시 잡기 때문이다 */
	return ret;	/* [한국어] 0 또는 칩이 낸 오류 */
}
EXPORT_SYMBOL_GPL(__irq_apply_affinity_hint);	/* [한국어] 드라이버가 매크로를 통해 부른다 */

/*
 * [한국어]
 * irq_affinity_notify - 친화도 변경을 드라이버에 알린다 (워크큐 콜백)
 *
 * @work: 알림 구조체 안의 작업 항목
 * @return: 없음
 *
 * irq_affinity_schedule_notify_work() 가 예약한 작업의 본체다.
 * 프로세스 문맥에서 실행되므로 드라이버가 잠들 수 있는 일을 해도 된다.
 *
 * 현재 친화도를 읽는 부분이 미묘하다. 미뤄 둔 변경이 있으면 그것을,
 * 없으면 현재 값을 읽는다. 사용자가 방금 요청한 값을 보여 주는 것이
 * 의도에 맞기 때문이다 — 아직 하드웨어에 반영되지 않았더라도.
 *
 * 마스크를 락 안에서 복사하고 락 밖에서 콜백을 부르는 것에 주목:
 * 콜백이 잠들 수 있으므로 스핀락을 쥔 채 부를 수 없다.
 *
 * 마스크 할당이 실패하면 알림을 건너뛰는 것도 눈에 띈다. 진단용
 * 편의라 실패해도 인터럽트 동작에 영향이 없다. 다만 참조는 반드시
 * 놓아야 하므로 out 레이블로 간다.
 *
 * 실행 컨텍스트: 워크큐, 프로세스 문맥.
 *
 * 호출 체인:
 *   워크큐 → [이 함수] → notify->notify() (드라이버 콜백)
 */
static void irq_affinity_notify(struct work_struct *work)
{
	struct irq_affinity_notify *notify = container_of(work, struct irq_affinity_notify, work);	/* [한국어] 작업 항목에서 알림 구조체를 되찾는다 */
	struct irq_desc *desc = irq_to_desc(notify->irq);	/* [한국어] 대상 서술자. 알림 구조체가 번호를 기억하고 있다 */
	cpumask_var_t cpumask;	/* [한국어] 콜백에 넘길 마스크 사본 */

	if (!desc || !alloc_cpumask_var(&cpumask, GFP_KERNEL))	/* [한국어] 서술자가 사라졌거나 메모리가 부족한가 */
		goto out;	/* [한국어] 알림을 건너뛰되 참조는 반드시 놓아야 한다 */

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {	/* [한국어] 마스크를 읽는 동안만 락을 잡는다 */
		if (irq_move_pending(&desc->irq_data))	/* [한국어] 미뤄 둔 변경이 있는가 */
			irq_get_pending(cpumask, desc);	/* [한국어] 사용자가 방금 요청한 값을 보여 준다. 아직 하드웨어에 반영되지 않았더라도 그것이 의도에 맞다 */
		else	/* [한국어] 없으면 */
			cpumask_copy(cpumask, desc->irq_common_data.affinity);	/* [한국어] 현재 값 */
	}

	notify->notify(notify, cpumask);	/* [한국어] 락 밖에서 부른다. 드라이버 콜백이 잠들 수 있어 스핀락을 쥔 채 부를 수 없다 */

	free_cpumask_var(cpumask);	/* [한국어] 사본 반납 */
out:	/* [한국어] 성공과 실패가 합류한다 */
	kref_put(&notify->kref, notify->release);	/* [한국어] 예약할 때 잡은 참조를 놓는다. 0 이 되면 드라이버의 release 가 구조체를 해제한다 */
}

/**
 * irq_set_affinity_notifier - control notification of IRQ affinity changes
 * @irq:	Interrupt for which to enable/disable notification
 * @notify:	Context for notification, or %NULL to disable
 *		notification.  Function pointers must be initialised;
 *		the other fields will be initialised by this function.
 *
 * Must be called in process context.  Notification may only be enabled
 * after the IRQ is allocated and must be disabled before the IRQ is freed
 * using free_irq().
 */
/*
 * [한국어]
 * irq_set_affinity_notifier - 친화도 변경 알림을 등록하거나 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @notify: 알림 설명자, NULL 이면 해제
 * @return: 0 성공, -EINVAL 인터럽트가 없거나 NMI 임
 *
 * 드라이버가 친화도 변경을 알고 싶을 때 등록한다. 다중 큐 네트워크
 * 카드가 대표적으로, 큐-CPU 대응이 바뀌면 수신 큐 설정을 다시 한다.
 *
 * 옛 알림을 정리하는 순서가 이 함수에서 가장 조심스러운 부분이다.
 *
 *   1. 락 안에서 포인터를 교체한다 — 이 뒤로 새 변경은 새 알림으로 간다.
 *   2. 락 밖에서 옛 작업이 끝나기를 기다린다.
 *   3. 참조를 놓는다.
 *
 * 2 에서 cancel_work_sync 가 참을 돌려주면 큐에 있던 작업을 취소한
 * 것이다. 그 작업이 참조를 하나 들고 있었으므로 그것도 놓아야 한다.
 * 그래서 kref_put 이 두 번 나온다 — 취소한 작업 몫과 등록 몫이다.
 *
 * NMI 를 거절하는 이유: NMI 는 친화도를 바꿀 수 없고, 알림 기구가
 * 잠들 수 있는 작업을 쓰는데 NMI 문맥에서는 그것을 예약할 수 없다.
 *
 * 원본 주석의 조건: free_irq 전에 해제해야 한다. 그러지 않으면
 * free_irq 가 경고한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. cancel_work_sync 가 잠들 수 있다.
 *
 * 호출 체인:
 *   다중 큐 네트워크 드라이버 등 → [이 함수]
 */
int irq_set_affinity_notifier(unsigned int irq, struct irq_affinity_notify *notify)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irq_affinity_notify *old_notify;	/* [한국어] 교체 전의 알림 */

	/* The release function is promised process context */
	might_sleep();	/* [한국어] (위 영어 주석) 아래 cancel_work_sync 가 잠들 수 있다. 잘못된 문맥에서 부르면 여기서 걸린다 */

	if (!desc || irq_is_nmi(desc))	/* [한국어] 없거나 NMI 인가 — NMI 는 친화도를 바꿀 수 없고 워크큐도 쓸 수 없다 */
		return -EINVAL;

	/* Complete initialisation of *notify */
	if (notify) {	/* [한국어] (위 영어 주석) 등록하는 경우 */
		notify->irq = irq;	/* [한국어] 콜백이 서술자를 되찾을 통로 */
		kref_init(&notify->kref);	/* [한국어] 참조 카운트 1 로 시작 */
		INIT_WORK(&notify->work, irq_affinity_notify);	/* [한국어] 워크큐 항목 초기화 */
	}

	scoped_guard(raw_spinlock_irq, &desc->lock) {	/* [한국어] 포인터 교체만 락 안에서 */
		old_notify = desc->affinity_notify;	/* [한국어] 옛 것을 챙긴다 */
		desc->affinity_notify = notify;	/* [한국어] 이 대입 뒤로 새 변경은 새 알림으로 간다 */
	}

	if (old_notify) {	/* [한국어] 교체된 옛 알림이 있는가 */
		if (cancel_work_sync(&old_notify->work)) {	/* [한국어] 큐에 있던 작업을 취소했는가. 락 밖에서 하는 것은 이 함수가 잠들 수 있어서다 */
			/* Pending work had a ref, put that one too */
			kref_put(&old_notify->kref, old_notify->release);	/* [한국어] (위 영어 주석) 취소된 작업이 들고 있던 참조 */
		}
		kref_put(&old_notify->kref, old_notify->release);	/* [한국어] 등록 시점의 참조. 두 번 나오는 이유가 이 둘의 구분이다 */
	}

	return 0;	/* [한국어] 관리 정책 전환이 끝났다 */
}
EXPORT_SYMBOL_GPL(irq_set_affinity_notifier);	/* [한국어] 다중 큐 드라이버가 부른다 */

#ifndef CONFIG_AUTO_IRQ_AFFINITY	/* [한국어] 아키텍처가 자기 친화도 선택기를 제공하지 않는 경우 — 거의 모든 아키텍처가 여기다 */
/*
 * Generic version of the affinity autoselector.
 */
/*
 * [한국어]
 * irq_setup_affinity - 인터럽트를 시작할 때 친화도를 자동으로 고른다
 *
 * @desc: 대상 서술자
 * @return: 0 성공 또는 설정할 필요 없음, 음수 오류
 *
 * request_irq 로 인터럽트를 시작할 때, 사용자가 친화도를 지정하지
 * 않았다면 여기서 정한다. 세 단계로 후보를 좁힌다.
 *
 * (1) 출발점 정하기: 기본은 시스템 기본 친화도다. 다만 관리형이거나
 *     사용자가 이미 지정한 인터럽트라면 그 값을 존중한다 — 단, 그
 *     마스크에 살아 있는 CPU 가 있을 때만이다. 없으면 지정 표시를
 *     지우고 기본값으로 돌아간다.
 * (2) 온라인 CPU 로 좁히기: 그 결과가 비면 온라인 CPU 전체로 되돌린다.
 *     갈 곳 없는 설정을 만들지 않으려는 것이다.
 * (3) NUMA 노드로 좁히기: 이 인터럽트가 속한 노드의 CPU 가 후보에
 *     있으면 그쪽만 남긴다. 장치와 가까운 CPU 에서 처리하면 메모리
 *     접근이 빨라진다.
 *
 * 정적 마스크와 그 전용 락을 쓰는 것에 주목: 위 __tmp_mask 와 같은
 * 이유로 스택에 두기 부담스럽다. 다만 이쪽은 per-CPU 가 아니라
 * 진짜 공유 변수라 락이 필요하다. 이 경로가 인터럽트 시작 시점에만
 * 불려 경쟁이 드물기 때문에 그 선택이 가능하다.
 *
 * (3) 에서 교집합이 비면 노드 제한을 포기하는 것도 눈에 띈다. 노드
 * 지역성은 최적화이지 필수 조건이 아니다.
 *
 * 실행 컨텍스트: desc->lock 보유, 인터럽트 시작 경로.
 *
 * 호출 체인:
 *   irq_startup() (kernel/irq/chip.c) → [이 함수] → irq_do_set_affinity()
 */
int irq_setup_affinity(struct irq_desc *desc)
{
	struct cpumask *set = irq_default_affinity;	/* [한국어] 출발점. 아래에서 사용자 지정 값으로 바뀔 수 있다 */
	int node = irq_desc_get_node(desc);	/* [한국어] 이 인터럽트가 속한 NUMA 노드 */

	static DEFINE_RAW_SPINLOCK(mask_lock);	/* [한국어] 아래 정적 마스크를 지키는 전용 락. 이 경로가 인터럽트 시작 시점에만 불려 경쟁이 드물다 */
	static struct cpumask mask;	/* [한국어] 계산용 공유 마스크. CPU 가 많으면 커서 스택에 두기 부담스럽다 */

	/* Excludes PER_CPU and NO_BALANCE interrupts */
	if (!__irq_can_set_affinity(desc))	/* [한국어] (위 영어 주석) 친화도라는 개념이 있는 인터럽트인가 */
		return 0;	/* [한국어] 오류가 아니다. 설정할 것이 없다는 뜻이다 */

	guard(raw_spinlock)(&mask_lock);	/* [한국어] 정적 마스크 보호 */
	/*
	 * Preserve the managed affinity setting and a userspace affinity
	 * setup, but make sure that one of the targets is online.
	 */
	if (irqd_affinity_is_managed(&desc->irq_data) ||	/* [한국어] (위 영어 주석) 커널이 관리하는 인터럽트이거나 */
	    irqd_has_set(&desc->irq_data, IRQD_AFFINITY_SET)) {	/* [한국어] 사용자가 이미 지정했는가 */
		if (cpumask_intersects(desc->irq_common_data.affinity,	/* [한국어] 그 마스크에 살아 있는 CPU 가 있는가 */
				       cpu_online_mask))
			set = desc->irq_common_data.affinity;	/* [한국어] 있으면 그 값을 존중한다 */
		else	/* [한국어] 없으면 */
			irqd_clear(&desc->irq_data, IRQD_AFFINITY_SET);	/* [한국어] 지정 표시를 지우고 기본값으로 돌아간다. 살아 있는 CPU 가 없는 지정은 지킬 수 없다 */
	}

	cpumask_and(&mask, cpu_online_mask, set);	/* [한국어] 출발점을 온라인 CPU 로 좁힌다 */
	if (cpumask_empty(&mask))	/* [한국어] 결과가 비었는가 */
		cpumask_copy(&mask, cpu_online_mask);	/* [한국어] 온라인 CPU 전체로 되돌린다. 갈 곳 없는 설정을 만들지 않는다 */

	if (node != NUMA_NO_NODE) {	/* [한국어] 노드가 정해진 인터럽트인가 */
		const struct cpumask *nodemask = cpumask_of_node(node);	/* [한국어] 그 노드의 CPU 들 */

		/* make sure at least one of the cpus in nodemask is online */
		if (cpumask_intersects(&mask, nodemask))	/* [한국어] (위 영어 주석) 후보 중에 그 노드의 CPU 가 있는가 */
			cpumask_and(&mask, &mask, nodemask);	/* [한국어] 있으면 그쪽만 남긴다. 장치와 가까운 CPU 에서 처리하면 메모리 접근이 빠르다. 없으면 노드 제한을 포기한다 — 지역성은 최적화이지 필수가 아니다 */
	}
	return irq_do_set_affinity(&desc->irq_data, &mask, false);	/* [한국어] 고른 마스크로 실제 설정 */
}
#else	/* [한국어] 아키텍처가 자기 선택기를 제공하는 경우 */
/* Wrapper for ALPHA specific affinity selector magic */
/*
 * [한국어]
 * irq_setup_affinity - 아키텍처 고유 친화도 선택기로 넘긴다
 *
 * @desc: 대상 서술자
 * @return: 아키텍처 선택기의 반환값
 *
 * 원본 주석이 밝히듯 Alpha 전용이다. 그 아키텍처는 인터럽트 라우팅
 * 하드웨어가 특이해 공용 선택 논리를 쓸 수 없다.
 *
 * 서술자가 아니라 번호를 넘기는 것에 주목: 아키텍처 인터페이스가
 * 그렇게 정의돼 있다.
 *
 * 실행 컨텍스트: desc->lock 보유, 인터럽트 시작 경로.
 *
 * 호출 체인:
 *   irq_startup() → [이 함수] → irq_select_affinity() (아키텍처 구현)
 */
int irq_setup_affinity(struct irq_desc *desc)
{
	return irq_select_affinity(irq_desc_get_irq(desc));	/* [한국어] 아키텍처 인터페이스가 번호를 받게 되어 있다 */
}
#endif /* CONFIG_AUTO_IRQ_AFFINITY */	/* [한국어] 자동 선택기 분기의 끝 */
#endif /* CONFIG_SMP */	/* [한국어] 친화도 관리 전체가 여기서 끝난다. 아래는 UP 에서도 쓰이는 코드다 */


/**
 * irq_set_vcpu_affinity - Set vcpu affinity for the interrupt
 * @irq:	interrupt number to set affinity
 * @vcpu_info:	vCPU specific data or pointer to a percpu array of vCPU
 *		specific data for percpu_devid interrupts
 *
 * This function uses the vCPU specific data to set the vCPU affinity for
 * an irq. The vCPU specific data is passed from outside, such as KVM. One
 * example code path is as below: KVM -> IOMMU -> irq_set_vcpu_affinity().
 */
/*
 * [한국어]
 * irq_set_vcpu_affinity - 인터럽트를 게스트 vCPU 로 직접 전달하게 한다
 *
 * @irq:       대상 인터럽트 번호
 * @vcpu_info: 가상 CPU 정보 (구조는 아키텍처마다 다르다)
 * @return:    0 성공, -ENOSYS 지원하는 층이 없음, -EINVAL 인터럽트 없음
 *
 * 가상화 성능의 핵심 기능이다. 보통 게스트에 인터럽트를 주려면
 * 호스트가 먼저 받아 게스트에 다시 주입해야 한다. 그 왕복이 비싸다.
 *
 * 하드웨어가 지원하면 인터럽트를 게스트 vCPU 로 직접 보낼 수 있다.
 * x86 의 posted interrupt 나 ARM 의 GICv4 가 그것이다. 그러면 호스트
 * 개입 없이 게스트가 인터럽트를 받는다.
 *
 * 계층을 훑는 것에 주목: 어느 층이 그 기능을 갖는지 미리 알 수 없다.
 * IOMMU 리매핑 층일 수도, 그 아래일 수도 있다. 위에서부터 내려가며
 * 처음 찾은 층이 처리한다.
 *
 * 원본 주석의 호출 경로가 흥미롭다 — KVM → IOMMU → 이 함수다. KVM 이
 * 게스트 정보를 알고, IOMMU 계층이 그것을 인터럽트 설정으로 옮긴다.
 *
 * 루프가 break 로 끝났는지 data 가 NULL 인지로 성공을 판별하는 것에
 * 주목: 찾았으면 data 가 그 층을 가리키고, 못 찾았으면 NULL 이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   KVM → IOMMU 드라이버 → [이 함수] → chip->irq_set_vcpu_affinity()
 */
int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info)
{
	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */
		struct irq_data *data;	/* [한국어] 계층을 훑을 커서 */
		struct irq_chip *chip;	/* [한국어] 각 층의 칩 */

		data = irq_desc_get_irq_data(desc);	/* [한국어] 가장 바깥 층부터 */
		do {
			chip = irq_data_get_irq_chip(data);	/* [한국어] 이 층의 칩 */
			if (chip && chip->irq_set_vcpu_affinity)	/* [한국어] vCPU 직접 전달을 지원하는가 */
				break;	/* [한국어] 찾았다. data 가 그 층을 가리킨 채 루프를 벗어난다 */

			data = irqd_get_parent_data(data);	/* [한국어] 한 층 안으로. 어느 층이 그 기능을 갖는지 미리 알 수 없다 */
		} while (data);

		if (!data)	/* [한국어] 끝까지 못 찾았는가 */
			return -ENOSYS;	/* [한국어] 이 계층은 vCPU 직접 전달을 지원하지 않는다. KVM 이 일반 주입 경로로 대체한다 */
		return chip->irq_set_vcpu_affinity(data, vcpu_info);	/* [한국어] 찾은 층에 위임한다 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}
EXPORT_SYMBOL_GPL(irq_set_vcpu_affinity);	/* [한국어] IOMMU 드라이버가 부른다 */

/*
 * [한국어]
 * __disable_irq - 비활성 중첩 깊이를 하나 올린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 비활성화의 중첩을 세는 것이 이 두 줄의 전부다. depth 가 0 에서
 * 1 이 될 때만 실제로 하드웨어를 건드린다.
 *
 * 왜 세는가: 여러 곳에서 같은 인터럽트를 끌 수 있다. 드라이버가
 * 자기 이유로 끄고, 그 사이에 서스펜드가 또 끄면, 둘 다 켜야 실제로
 * 열려야 한다. 카운터가 그 짝을 맞춘다.
 *
 * 후치 증가를 조건식 안에 쓰는 관용구에 주목: `!desc->depth++` 는
 * "증가 전 값이 0 이었는가" 를 묻는다. 한 줄로 검사와 증가를 함께
 * 한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   __disable_irq_nosync() / kernel/irq/pm.c → [이 함수] → irq_disable()
 */
void __disable_irq(struct irq_desc *desc)
{
	if (!desc->depth++)	/* [한국어] 증가 전 값이 0 이었는가 — 즉 이번이 첫 비활성화인가. 검사와 증가를 한 줄로 한다 */
		irq_disable(desc);	/* [한국어] 그때만 실제로 하드웨어를 건드린다. 여러 곳에서 끈 것을 카운터가 짝지어 준다 */
}

/*
 * [한국어]
 * __disable_irq_nosync - 번호로 찾아 비활성화한다 (기다리지 않음)
 *
 * @irq: 대상 인터럽트 번호
 * @return: 0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * disable_irq_nosync(), disable_irq(), disable_hardirq() 가 공유하는
 * 구현이다. 세 함수의 차이는 이 함수 뒤에 무엇을 기다리느냐뿐이다.
 *
 * buslock 판을 쓰는 이유: 게으른 비활성이 아닌 칩(IRQ_DISABLE_UNLAZY)
 * 이나 전용 irq_disable 콜백을 가진 칩은 여기서 하드웨어를 건드리는데,
 * 그것이 느린 버스 뒤에 있을 수 있다.
 *
 * 반환값을 두는 이유: 호출자가 성공했을 때만 기다리게 하려는 것이다.
 * 없는 인터럽트를 기다리는 것은 무의미하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 버스 락이 잠들 수 있다.
 *
 * 호출 체인:
 *   disable_irq_nosync() / disable_irq() / disable_hardirq() → [이 함수]
 */
static int __disable_irq_nosync(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] per-CPU 인터럽트는 disable_percpu_irq 를 써야 한다. buslock 은 하드웨어를 건드릴 수 있어서다 */
		__disable_irq(scoped_irqdesc);	/* [한국어] 중첩 깊이를 올리고 필요하면 하드웨어를 끈다 */
		return 0;	/* [한국어] 힌트만 기록했거나 친화도까지 적용했다 */
	}
	return -EINVAL;	/* [한국어] 호출자가 이 값을 보고 기다릴지 정한다. 없는 인터럽트를 기다리는 것은 무의미하다 */
}

/**
 * disable_irq_nosync - disable an irq without waiting
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Disables and Enables are
 * nested.
 * Unlike disable_irq(), this function does not ensure existing
 * instances of the IRQ handler have completed before returning.
 *
 * This function may be called from IRQ context.
 */
/*
 * [한국어]
 * disable_irq_nosync - 인터럽트를 끄되 기다리지 않는다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 세 가지 disable 중 가장 가벼운 것이다. 표시만 하고 곧바로 돌아온다.
 *
 * 반환 시점에 다른 CPU 가 아직 이 인터럽트의 핸들러를 실행 중일 수
 * 있다. 그래서 핸들러가 만지는 자료구조를 해제하려는 목적으로는 쓸
 * 수 없다.
 *
 * 그 대신 인터럽트 문맥에서도 부를 수 있다. 기다리지 않으므로
 * 데드락 위험이 없다. 인터럽트 핸들러 안에서 자기 인터럽트를 끄는
 * 흔한 용법이 이것이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 포함.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __disable_irq_nosync()
 */
void disable_irq_nosync(unsigned int irq)
{
	__disable_irq_nosync(irq);	/* [한국어] 반환값을 무시한다. 기다리지 않으므로 성패를 알 필요가 없다 */
}
EXPORT_SYMBOL(disable_irq_nosync);	/* [한국어] 드라이버가 가장 많이 쓰는 disable 이다 */

/**
 * disable_irq - disable an irq and wait for completion
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Enables and Disables are nested.
 *
 * This function waits for any pending IRQ handlers for this interrupt to
 * complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock.
 *
 * Can only be called from preemptible code as it might sleep when an
 * interrupt thread is associated to @irq.
 *
 */
/*
 * [한국어]
 * disable_irq - 인터럽트를 끄고 진행 중인 처리가 끝나기를 기다린다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 이 함수가 반환하면 어느 CPU 에서도 이 인터럽트의 핸들러가 실행되고
 * 있지 않다. 그래서 핸들러가 만지는 자료구조를 안전하게 해제하거나
 * 재설정할 수 있다.
 *
 * 대가가 두 가지다. 잠들 수 있어 인터럽트 문맥에서 부를 수 없고,
 * 원본 주석의 데드락 경고가 적용된다 — 핸들러가 필요로 하는 락을
 * 쥔 채 부르면 서로 멈춘다.
 *
 * 실패했으면 기다리지 않는 것에 주목: __disable_irq_nosync 가 0 이
 * 아닌 값을 돌려주면 그런 인터럽트가 없다는 뜻이라 기다릴 대상도
 * 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 선점 가능 상태.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __disable_irq_nosync() → synchronize_irq()
 */
void disable_irq(unsigned int irq)
{
	might_sleep();	/* [한국어] 아래 synchronize_irq 가 스레드 핸들러를 기다리며 잠들 수 있다 */
	if (!__disable_irq_nosync(irq))	/* [한국어] 성공했을 때만 */
		synchronize_irq(irq);	/* [한국어] 진행 중인 처리가 끝나기를 기다린다. 이 뒤로 핸들러가 만지는 자료구조를 안전하게 해제할 수 있다 */
}
EXPORT_SYMBOL(disable_irq);	/* [한국어] 드라이버 정리 경로가 부른다 */

/**
 * disable_hardirq - disables an irq and waits for hardirq completion
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Enables and Disables are nested.
 *
 * This function waits for any pending hard IRQ handlers for this interrupt
 * to complete before returning. If you use this function while holding a
 * resource the hard IRQ handler may need you will deadlock.
 *
 * When used to optimistically disable an interrupt from atomic context the
 * return value must be checked.
 *
 * Returns: false if a threaded handler is active.
 *
 * This function may be called - with care - from IRQ context.
 */
/*
 * [한국어]
 * disable_hardirq - 인터럽트를 끄고 하드 인터럽트 처리만 기다린다
 *
 * @irq: 대상 인터럽트 번호
 * @return: true 완전히 멈췄음, false 스레드 핸들러가 아직 동작 중
 *
 * 위 두 함수의 중간이다. 하드 인터럽트 핸들러는 기다리지만 스레드
 * 핸들러는 기다리지 않는다.
 *
 * 그 절충이 원자적 문맥에서 쓸 수 있게 만든다. 스레드를 기다리려면
 * 잠들어야 하지만, 하드 인터럽트 핸들러는 회전 대기로 기다릴 수 있다.
 *
 * 반환값 검사가 필수인 이유가 원본 주석에 있다. false 를 받았다면
 * 스레드가 아직 자료구조를 만지고 있으므로, 호출자가 그것을 해제하면
 * 안 된다. 그 경우 대개 인터럽트를 다시 켜고 잠들 수 있는 문맥에서
 * 다시 시도한다.
 *
 * 실패 시 false 인 것에 주목: 인터럽트가 없어 끄지도 못했으면
 * "완전히 멈췄다" 고 말할 수 없다.
 *
 * 실행 컨텍스트: 조심하면 인터럽트 문맥도 가능.
 *
 * 호출 체인:
 *   드라이버의 원자적 정리 경로 → [이 함수] → synchronize_hardirq()
 */
bool disable_hardirq(unsigned int irq)
{
	if (!__disable_irq_nosync(irq))	/* [한국어] 끄기에 성공했는가 */
		return synchronize_hardirq(irq);	/* [한국어] 하드 인터럽트만 기다리고, 스레드 상태를 반환값으로 알린다. false 면 호출자가 자료구조를 해제하면 안 된다 */
	return false;	/* [한국어] 끄지도 못했으니 "완전히 멈췄다" 고 말할 수 없다 */
}
EXPORT_SYMBOL_GPL(disable_hardirq);	/* [한국어] 원자적 문맥에서 정리해야 하는 드라이버가 부른다 */

/**
 * disable_nmi_nosync - disable an nmi without waiting
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line. Disables and enables are nested.
 *
 * The interrupt to disable must have been requested through request_nmi.
 * Unlike disable_nmi(), this function does not ensure existing
 * instances of the IRQ handler have completed before returning.
 */
/*
 * [한국어]
 * disable_nmi_nosync - NMI 를 끄되 기다리지 않는다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 구현이 disable_irq_nosync() 를 그대로 부르는 것뿐이다. 별도 함수가
 * 있는 이유는 이름이다 — NMI 를 다루는 코드가 NMI 전용 API 를 쓰는
 * 편이 의도가 분명하고, 나중에 동작이 달라져야 할 때 호출자를 고치지
 * 않아도 된다.
 *
 * 기다리는 판(disable_nmi)이 없는 것에 주목: NMI 는 마스크할 수
 * 없으므로 "진행 중인 처리가 끝나기를 기다린다" 는 개념이 성립하기
 * 어렵다. 실제로 그 함수는 커널에 없다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   free_nmi() / NMI 를 쓰는 드라이버 → [이 함수]
 */
void disable_nmi_nosync(unsigned int irq)
{
	disable_irq_nosync(irq);	/* [한국어] 같은 구현이지만 이름이 다르다. NMI 코드가 전용 API 를 쓰는 편이 의도가 분명하다 */
}

/*
 * [한국어]
 * __enable_irq - 비활성 중첩 깊이를 하나 내린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * __disable_irq() 의 반대인데 훨씬 조심스럽다. switch 로 세 경우를
 * 나눈다.
 *
 * depth 0 — 끄지 않았는데 켜려 한다. 짝이 맞지 않는 호출이고,
 * 그대로 두면 카운터가 음수가 되어 이후 모든 짝이 어긋난다. 경고하고
 * 아무것도 하지 않는다.
 *
 * depth 1 — 마지막 켜기다. 실제로 인터럽트를 시작한다. 다만 서스펜드
 * 중이면 그것도 짝이 맞지 않는 호출이라 err_out 으로 뛴다.
 *
 * 그 외 — 아직 다른 이유로 꺼져 있다. 카운터만 내린다.
 *
 * irq_enable() 이 아니라 irq_startup() 을 부르는 이유가 원본 주석에
 * 자세하다. IRQ_NOAUTOEN 으로 요청된 인터럽트는 request_irq 때 시작되지
 * 않았으므로, 첫 enable_irq 가 시작 절차 전체를 밟아야 한다. 이미
 * 시작된 인터럽트라면 irq_startup 이 내부에서 irq_enable 만 부른다.
 *
 * noprobe 를 세우는 이유: 명시적으로 켜진 인터럽트는 자동 탐색의
 * 대상이 아니다. 탐색이 그 선을 건드리면 드라이버의 동작을 방해한다.
 *
 * goto 레이블이 case 안에 있는 것에 주목: C 에서 레이블은 어디에나
 * 둘 수 있고, 여기서는 두 오류 경우가 같은 경고를 공유하게 한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   enable_irq() / __setup_irq() / kernel/irq/pm.c → [이 함수]
 */
void __enable_irq(struct irq_desc *desc)
{
	switch (desc->depth) {	/* [한국어] 중첩 깊이로 세 경우를 나눈다. 0 은 짝이 맞지 않는 호출, 1 은 마지막 켜기, 그 외는 카운터만 내린다 */
	case 0:	/* [한국어] 끄지 않았는데 켜려 한다 */
 err_out:	/* [한국어] 아래 서스펜드 경우도 여기로 온다. C 에서 레이블은 case 안에도 둘 수 있다 */
		WARN(1, KERN_WARNING "Unbalanced enable for IRQ %d\n",	/* [한국어] 그대로 두면 카운터가 음수가 되어 이후 모든 짝이 어긋난다 */
		     irq_desc_get_irq(desc));
		break;
	case 1: {	/* [한국어] 마지막 켜기 — 실제로 인터럽트를 시작한다 */
		if (desc->istate & IRQS_SUSPENDED)	/* [한국어] 서스펜드 중인가 */
			goto err_out;	/* [한국어] 서스펜드가 끈 것을 드라이버가 켜려 하는 것도 짝이 맞지 않는 호출이다 */
		/* Prevent probing on this irq: */
		irq_settings_set_noprobe(desc);	/* [한국어] (위 영어 주석) 명시적으로 켜진 인터럽트는 자동 탐색 대상이 아니다. 탐색이 그 선을 건드리면 드라이버 동작을 방해한다 */
		/*
		 * Call irq_startup() not irq_enable() here because the
		 * interrupt might be marked NOAUTOEN so irq_startup()
		 * needs to be invoked when it gets enabled the first time.
		 * This is also required when __enable_irq() is invoked for
		 * a managed and shutdown interrupt from the S3 resume
		 * path.
		 *
		 * If it was already started up, then irq_startup() will
		 * invoke irq_enable() under the hood.
		 */
		irq_startup(desc, IRQ_RESEND, IRQ_START_FORCE);	/* [한국어] (위 영어 주석) NOAUTOEN 으로 요청된 인터럽트는 아직 시작되지 않았으므로 첫 enable 이 시작 절차 전체를 밟아야 한다 */
		break;
	}
	default:	/* [한국어] 아직 다른 이유로 꺼져 있다 */
		desc->depth--;	/* [한국어] 카운터만 내린다. 하드웨어는 그대로 */
	}
}

/**
 * enable_irq - enable handling of an irq
 * @irq: Interrupt to enable
 *
 * Undoes the effect of one call to disable_irq().  If this matches the
 * last disable, processing of interrupts on this IRQ line is re-enabled.
 *
 * This function may be called from IRQ context only when
 * desc->irq_data.chip->bus_lock and desc->chip->bus_sync_unlock are NULL !
 */
/*
 * [한국어]
 * enable_irq - 인터럽트를 다시 켠다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * disable_irq() 한 번을 되돌린다. 중첩된 만큼 켜야 실제로 열린다.
 *
 * 칩이 없는데 켜려는 것을 경고하는 이유: request_irq 전에 enable_irq
 * 를 부르는 실수가 흔하다. 그러면 no_irq_chip 상태라 아무 일도
 * 일어나지 않는데, 드라이버는 켰다고 믿고 인터럽트를 기다린다.
 *
 * 원본 주석의 문맥 조건: 버스 락 콜백이 있는 칩에서는 인터럽트
 * 문맥에서 부를 수 없다. 그 콜백이 잠들 수 있기 때문이다. 그런 칩이
 * 아니면 인터럽트 문맥에서도 안전하다.
 *
 * 실행 컨텍스트: 대개 프로세스 문맥. 버스 락이 없는 칩이면 인터럽트
 * 문맥도 가능하다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __enable_irq()
 */
void enable_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] per-CPU 는 enable_percpu_irq 를 써야 한다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */

		if (WARN(!desc->irq_data.chip, "enable_irq before setup/request_irq: irq %u\n", irq))	/* [한국어] request_irq 전에 부르는 흔한 실수. 그대로 두면 아무 일도 안 일어나는데 드라이버는 켰다고 믿는다 */
			return;
		__enable_irq(desc);	/* [한국어] 중첩 깊이를 내리고 0 이 되면 시작한다 */
	}
}
EXPORT_SYMBOL(enable_irq);	/* [한국어] 드라이버가 부른다 */

/**
 * enable_nmi - enable handling of an nmi
 * @irq: Interrupt to enable
 *
 * The interrupt to enable must have been requested through request_nmi.
 * Undoes the effect of one call to disable_nmi(). If this matches the last
 * disable, processing of interrupts on this IRQ line is re-enabled.
 */
/*
 * [한국어]
 * enable_nmi - NMI 를 다시 켠다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 위 disable_nmi_nosync 와 같은 이유로 존재하는 이름만의 함수다.
 * 구현은 enable_irq() 그대로다.
 *
 * NMI 를 쓰는 코드가 NMI 전용 API 짝을 쓰면 의도가 분명하고, 나중에
 * NMI 만의 처리가 필요해져도 호출자를 고치지 않아도 된다.
 *
 * 실행 컨텍스트: enable_irq() 와 같다.
 *
 * 호출 체인:
 *   NMI 를 쓰는 드라이버 → [이 함수] → enable_irq()
 */
void enable_nmi(unsigned int irq)
{
	enable_irq(irq);	/* [한국어] 이름만 다른 같은 구현. NMI 코드가 전용 API 짝을 쓰게 한다 */
}

/*
 * [한국어]
 * set_irq_wake_real - 절전 해제 설정을 칩에 전달한다
 *
 * @irq: 대상 인터럽트 번호
 * @on:  1 이면 wakeup 원으로, 0 이면 해제
 * @return: 0 성공 또는 설정 불필요, 음수 칩이 낸 오류 (-ENXIO 지원 없음)
 *
 * 두 갈래를 나눈다.
 *
 * IRQCHIP_SKIP_SET_WAKE 를 세운 칩은 wakeup 설정 자체가 필요 없다 —
 * 서스펜드해도 전원이 유지되는 컨트롤러다. 아무것도 하지 않고
 * 성공을 돌려준다.
 *
 * 그 플래그가 없는데 콜백도 없으면 -ENXIO 다. 초기값이 그 값인 것이
 * 그 처리다. "필요한데 못 한다" 를 알려, 사용자가 안 깨어나는
 * 시스템을 만들지 않게 한다.
 *
 * 이 두 경우의 구분은 kernel/irq/chip.c 의 irq_chip_set_wake_parent()
 * 와 같은 논리다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_irq_wake() → [이 함수] → chip->irq_set_wake()
 */
static int set_irq_wake_real(unsigned int irq, unsigned int on)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	int ret = -ENXIO;	/* [한국어] 콜백이 없을 때의 값. "필요한데 못 한다" 를 알려 안 깨어나는 시스템을 막는다 */

	if (irq_desc_get_chip(desc)->flags &  IRQCHIP_SKIP_SET_WAKE)	/* [한국어] 서스펜드해도 전원이 유지되어 설정이 필요 없는 칩인가 */
		return 0;	/* [한국어] 할 일 없이 성공. 아래 -ENXIO 와 구분해야 한다 */

	if (desc->irq_data.chip->irq_set_wake)	/* [한국어] 설정 콜백이 있는가 */
		ret = desc->irq_data.chip->irq_set_wake(&desc->irq_data, on);	/* [한국어] 칩에 위임. 대개 하드웨어 레지스터를 건드리거나 비트맵에 기록한다 */

	return ret;	/* [한국어] 0, 칩의 오류, 또는 콜백 없음(-ENXIO) */
}

/**
 * irq_set_irq_wake - control irq power management wakeup
 * @irq:	interrupt to control
 * @on:	enable/disable power management wakeup
 *
 * Enable/disable power management wakeup mode, which is disabled by
 * default.  Enables and disables must match, just as they match for
 * non-wakeup mode support.
 *
 * Wakeup mode lets this IRQ wake the system from sleep states like
 * "suspend to RAM".
 *
 * Note: irq enable/disable state is completely orthogonal to the
 * enable/disable state of irq wake. An irq can be disabled with
 * disable_irq() and still wake the system as long as the irq has wake
 * enabled. If this does not hold, then the underlying irq chip and the
 * related driver need to be investigated.
 */
/*
 * [한국어]
 * irq_set_irq_wake - 이 인터럽트로 시스템을 깨울 수 있게 한다
 *
 * @irq: 대상 인터럽트 번호
 * @on:  1 이면 wakeup 원으로 설정, 0 이면 해제
 * @return: 0 성공, -EINVAL NMI 이거나 인터럽트 없음, 그 외 칩의 오류
 *
 * 원본 주석의 마지막 문단이 이 기능의 핵심 개념을 짚는다. 인터럽트가
 * 켜져 있는지와 wakeup 원인지는 완전히 별개다. disable_irq() 로 끈
 * 인터럽트도 wakeup 이 설정돼 있으면 시스템을 깨운다.
 *
 * 왜 그런가: 서스펜드 상태에서는 커널이 인터럽트를 처리하지 않는다.
 * wakeup 설정은 "이 신호가 오면 시스템을 깨우라" 를 하드웨어에
 * 알리는 것이지 인터럽트 처리와는 다른 층이다.
 *
 * wake_depth 로 중첩을 세는 이유는 원본 주석에 있다. wakeup 가능한
 * 선을 여러 드라이버가 공유할 수 있고, 각자 다른 절전 요구를 가질
 * 수 있다. 하나라도 wakeup 을 원하면 켜져야 한다.
 *
 * 실패 시 카운터를 되돌리는 것에 주목: 하드웨어 설정이 실패했으면
 * 카운터도 원래대로 돌려야 짝이 맞는다. 그러지 않으면 다음 해제가
 * 엉뚱한 시점에 하드웨어를 건드린다.
 *
 * IRQD_WAKEUP_STATE 플래그는 sysfs 의 wakeup 파일과 서스펜드 경로가
 * 읽는다. 실제 설정이 성공했을 때만 세운다.
 *
 * NMI 를 거절하는 이유: NMI 는 마스크할 수 없어 서스펜드 중에도
 * 그대로 전달된다. wakeup 이라는 개념이 따로 필요 없고, 그 기구를
 * 적용하면 오히려 상태가 꼬인다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 버스 락이 잠들 수 있다.
 *
 * 호출 체인:
 *   enable_irq_wake() / disable_irq_wake() 매크로 → [이 함수]
 */
int irq_set_irq_wake(unsigned int irq, unsigned int on)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] 칩 설정이 느린 버스 뒤에 있을 수 있어 buslock 판이다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */
		int ret = 0;	/* [한국어] 결과 */

		/* Don't use NMIs as wake up interrupts please */
		if (irq_is_nmi(desc))	/* [한국어] (위 영어 주석) NMI 인가 */
			return -EINVAL;	/* [한국어] NMI 는 마스크할 수 없어 서스펜드 중에도 전달된다. wakeup 기구를 적용하면 상태만 꼬인다 */

		/*
		 * wakeup-capable irqs can be shared between drivers that
		 * don't need to have the same sleep mode behaviors.
		 */
		if (on) {	/* [한국어] (위 영어 주석) wakeup 원으로 설정하는가 */
			if (desc->wake_depth++ == 0) {	/* [한국어] 첫 요청인가. 하나라도 원하면 켜져야 하므로 0 에서 1 이 될 때만 하드웨어를 건드린다 */
				ret = set_irq_wake_real(irq, on);	/* [한국어] 칩에 전달 */
				if (ret)	/* [한국어] 실패 */
					desc->wake_depth = 0;	/* [한국어] 카운터를 되돌린다. 그러지 않으면 다음 해제가 엉뚱한 시점에 하드웨어를 건드린다 */
				else	/* [한국어] 성공 */
					irqd_set(&desc->irq_data, IRQD_WAKEUP_STATE);	/* [한국어] sysfs 의 wakeup 파일과 서스펜드 경로가 읽는 표시 */
			}
		} else {	/* [한국어] 해제하는 경우 */
			if (desc->wake_depth == 0) {	/* [한국어] 설정한 적이 없는가 */
				WARN(1, "Unbalanced IRQ %d wake disable\n", irq);	/* [한국어] 짝이 맞지 않는 호출. 카운터가 음수가 되는 것을 막는다 */
			} else if (--desc->wake_depth == 0) {	/* [한국어] 마지막 해제인가 */
				ret = set_irq_wake_real(irq, on);	/* [한국어] 이제 아무도 wakeup 을 원하지 않으므로 끈다 */
				if (ret)	/* [한국어] 실패 */
					desc->wake_depth = 1;	/* [한국어] 카운터를 되돌린다 */
				else	/* [한국어] 성공 */
					irqd_clear(&desc->irq_data, IRQD_WAKEUP_STATE);	/* [한국어] 표시를 지운다 */
			}
		}
		return ret;	/* [한국어] 0 성공, 칩의 오류, 또는 콜백 없음 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없거나 per-CPU 였다 */
}
EXPORT_SYMBOL(irq_set_irq_wake);	/* [한국어] 드라이버가 매크로를 통해 부른다 */

/*
 * Internal function that tells the architecture code whether a
 * particular irq has been exclusively allocated or is available
 * for driver use.
 */
/*
 * [한국어]
 * can_request_irq - 이 인터럽트를 요청할 수 있는지 미리 확인한다
 *
 * @irq:      대상 인터럽트 번호
 * @irqflags: 요청하려는 플래그
 * @return:   true 요청 가능, false 불가
 *
 * request_irq 를 실제로 부르지 않고 가능성만 묻는다. 인터럽트 자동
 * 탐색(kernel/irq/autoprobe.c)이나 아키텍처 코드가 후보를 추릴 때
 * 쓴다.
 *
 * 두 조건을 본다. 요청이 허용된 인터럽트인가(체인 처리기가 걸린
 * 선은 아니다), 그리고 비어 있거나 공유 가능한가.
 *
 * 공유 조건이 흥미롭다. `irqflags & desc->action->flags & IRQF_SHARED`
 * 는 세 값의 교집합이다 — 요청하려는 쪽과 기존 등록자가 모두
 * IRQF_SHARED 를 세웠을 때만 참이다. 한쪽만 공유를 허용하면 안 된다.
 *
 * 이 확인이 통과해도 실제 요청은 실패할 수 있다. 트리거 방식이나
 * oneshot 설정이 맞지 않으면 __setup_irq 가 거절한다. 그래서 이름이
 * can_ 이다 — 가능성이지 보장이 아니다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   probe_irq_on() (kernel/irq/autoprobe.c) / 아키텍처 코드 → [이 함수]
 */
bool can_request_irq(unsigned int irq, unsigned long irqflags)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {	/* [한국어] per-CPU 인터럽트는 이 API 의 대상이 아니다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */

		if (irq_settings_can_request(desc)) {	/* [한국어] 요청이 허용된 선인가. 체인 처리기가 걸린 선은 아니다 */
			if (!desc->action || irqflags & desc->action->flags & IRQF_SHARED)	/* [한국어] 비어 있거나, 요청하는 쪽과 기존 등록자가 모두 공유를 허용하는가. 세 값의 교집합이라 한쪽만 허용해서는 안 된다 */
				return true;
		}
	}
	return false;	/* [한국어] 가능성이지 보장이 아니다. 실제 요청은 트리거나 oneshot 이 맞지 않아 실패할 수 있다 */
}

/*
 * [한국어]
 * __irq_set_trigger - 트리거 방식을 하드웨어에 설정한다
 *
 * @desc:  대상 서술자
 * @flags: 요청된 트리거 방식 (IRQF_TRIGGER_ 계열)
 * @return: 0 성공, 음수 칩이 낸 오류
 *
 * 레벨이냐 에지냐, 상승이냐 하강이냐를 하드웨어에 알린다. 그 설정이
 * 흐름 처리기까지 바꿀 수 있어 조심스러운 함수다.
 *
 * 마스크 처리가 첫 번째 관문이다. IRQCHIP_SET_TYPE_MASKED 를 세운
 * 칩은 트리거를 바꾸는 동안 인터럽트가 오면 안 된다 — 옛 설정과 새
 * 설정이 섞인 상태로 신호를 받게 된다. 그래서 막았다가 나중에 연다.
 *
 * unmask 변수가 미묘하다. 원래 열려 있던 경우에만 나중에 다시 연다.
 * 원래 닫혀 있었다면 이 함수가 열어서는 안 된다.
 *
 * 콜백 결과 처리가 세 갈래다. OK 계열은 요청한 값을 상태에 기록하고,
 * NOCOPY 는 칩이 이미 자기 값을 넣었다는 뜻이라 기록하지 않는다.
 * 세 경우 모두 설정 워드와 IRQD_LEVEL 을 실제 값에 맞춰 다시 계산한다.
 *
 * 그 재계산이 필요한 이유: 레벨이냐 에지냐는 흐름 처리기 선택과
 * sysfs 표시에 쓰이는데, 그 정보가 두 곳(설정 워드와 IRQD 플래그)에
 * 사본으로 있다. 원본이 바뀌었으니 사본도 맞춰야 한다.
 *
 * 칩이 set_type 을 제공하지 않을 때 성공으로 반환하는 것에 주목:
 * 트리거가 하드웨어적으로 고정인 컨트롤러가 많다. 그것은 오류가
 * 아니라 그냥 설정할 것이 없다는 뜻이다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_irq_type() / __setup_irq() / __irq_do_set_handler() →
 *   [이 함수] → chip->irq_set_type()
 */
int __irq_set_trigger(struct irq_desc *desc, unsigned long flags)
{
	struct irq_chip *chip = desc->irq_data.chip;	/* [한국어] 설정을 넘길 칩 */
	int ret, unmask = 0;	/* [한국어] 결과와, 나중에 다시 열어야 하는지 */

	if (!chip || !chip->irq_set_type) {	/* [한국어] 트리거 설정을 지원하는가 */
		/*
		 * IRQF_TRIGGER_* but the PIC does not support multiple
		 * flow-types?
		 */
		pr_debug("No set_type function for IRQ %d (%s)\n",	/* [한국어] (위 영어 주석) 트리거가 하드웨어적으로 고정인 컨트롤러가 많다 */
			 irq_desc_get_irq(desc),
			 chip ? (chip->name ? : "unknown") : "unknown");	/* [한국어] 칩이나 이름이 없어도 안전하게 찍는다 */
		return 0;	/* [한국어] 오류가 아니다. 설정할 것이 없다는 뜻이다 */
	}

	if (chip->flags & IRQCHIP_SET_TYPE_MASKED) {	/* [한국어] 트리거를 바꾸는 동안 인터럽트가 오면 안 되는 칩인가 */
		if (!irqd_irq_masked(&desc->irq_data))	/* [한국어] 아직 열려 있는가 */
			mask_irq(desc);	/* [한국어] 막는다. 옛 설정과 새 설정이 섞인 상태로 신호를 받는 것을 막는다 */
		if (!irqd_irq_disabled(&desc->irq_data))	/* [한국어] 논리적으로 켜져 있던 인터럽트인가 */
			unmask = 1;	/* [한국어] 그때만 나중에 다시 연다. 원래 닫혀 있었다면 이 함수가 열어서는 안 된다 */
	}

	/* Mask all flags except trigger mode */
	flags &= IRQ_TYPE_SENSE_MASK;	/* [한국어] (위 영어 주석) IRQF_ 플래그에는 트리거 외의 비트도 많다. 칩에는 트리거만 넘긴다 */
	ret = chip->irq_set_type(&desc->irq_data, flags);	/* [한국어] 하드웨어 설정. 칩이 흐름 처리기까지 바꿀 수 있다 */

	switch (ret) {	/* [한국어] 칩의 응답 */
	case IRQ_SET_MASK_OK:	/* [한국어] 설정했고 코어가 값을 기록해야 한다 */
	case IRQ_SET_MASK_OK_DONE:	/* [한국어] 설정했고 코어가 더 할 일이 없다 */
		irqd_clear(&desc->irq_data, IRQD_TRIGGER_MASK);	/* [한국어] 옛 트리거 비트를 지우고 */
		irqd_set(&desc->irq_data, flags);	/* [한국어] 요청한 값을 기록한다 */
		fallthrough;	/* [한국어] 아래 공통 재계산으로 */

	case IRQ_SET_MASK_OK_NOCOPY:	/* [한국어] 칩이 이미 자기 값을 넣었다는 뜻이라 위 기록을 건너뛴다 */
		flags = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 실제로 설정된 값을 읽는다. NOCOPY 경로에서는 칩이 넣은 값이다 */
		irq_settings_set_trigger_mask(desc, flags);	/* [한국어] 설정 워드에도 사본을 둔다. 두 곳에 있는 것을 원본 변경에 맞춰 갱신한다 */
		irqd_clear(&desc->irq_data, IRQD_LEVEL);	/* [한국어] 레벨 표시를 일단 지우고 */
		irq_settings_clr_level(desc);	/* [한국어] 설정 워드 쪽도 */
		if (flags & IRQ_TYPE_LEVEL_MASK) {	/* [한국어] 실제로 레벨 트리거인가 */
			irq_settings_set_level(desc);	/* [한국어] 두 곳 모두 세운다. 이 정보가 흐름 처리기 선택과 sysfs 표시에 쓰인다 */
			irqd_set(&desc->irq_data, IRQD_LEVEL);	/* [한국어] 빠른 경로가 보는 사본. 설정 워드와 짝을 이룬다 */
		}

		ret = 0;	/* [한국어] 세 응답을 성공으로 통일한다 */
		break;
	default:	/* [한국어] 칩이 오류를 냈다 */
		pr_err("Setting trigger mode %lu for irq %u failed (%pS)\n",	/* [한국어] %pS 로 콜백 함수 이름을 찍어 어느 드라이버인지 알 수 있게 한다 */
		       flags, irq_desc_get_irq(desc), chip->irq_set_type);
	}
	if (unmask)	/* [한국어] 원래 열려 있었는가 */
		unmask_irq(desc);	/* [한국어] 다시 연다. 실패했어도 원래 상태로 되돌린다 */
	return ret;	/* [한국어] 0 이면 트리거가 반영됐고 설정 워드와 IRQD 사본도 갱신됐다 */
}

#ifdef CONFIG_HARDIRQS_SW_RESEND	/* [한국어] 소프트웨어 재전송을 쓰는 빌드에만 부모 인터럽트 개념이 필요하다 */
/*
 * [한국어]
 * irq_set_parent - 재전송에 쓸 부모 인터럽트를 지정한다
 *
 * @irq:        대상 인터럽트 번호
 * @parent_irq: 부모 인터럽트 번호
 * @return:     0 성공, -EINVAL 그런 인터럽트가 없음
 *
 * 소프트웨어 재전송이 필요한 상황을 위한 설정이다. 디먹스 컨트롤러의
 * 자식 인터럽트를 다시 올려야 할 때, 하드웨어적으로는 부모 선을
 * 흔들어야 한다. 이 함수가 그 부모를 알려 준다.
 *
 * kernel/irq/resend.c 가 이 값을 읽어 재전송 대상을 정한다.
 *
 * 검증이 없는 것에 주목: 부모 번호가 유효한지 확인하지 않는다.
 * 호출자가 컨트롤러 드라이버라 자기 계층을 안다는 전제다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   디먹스 컨트롤러 드라이버 → [이 함수]
 */
int irq_set_parent(int irq, int parent_irq)
{
	scoped_irqdesc_get_and_lock(irq, 0) {	/* [한국어] 서술자 조회와 잠금 */
		scoped_irqdesc->parent_irq = parent_irq;	/* [한국어] kernel/irq/resend.c 가 이 값을 읽어 재전송 대상을 정한다. 유효성을 검증하지 않는 것은 호출자가 자기 계층을 안다는 전제다 */
		return 0;	/* [한국어] 재전송 대상이 기록됐다 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}
EXPORT_SYMBOL_GPL(irq_set_parent);	/* [한국어] 디먹스 컨트롤러 드라이버가 부른다 */
#endif	/* [한국어] CONFIG_HARDIRQS_SW_RESEND 분기의 끝 */

/*
 * Default primary interrupt handler for threaded interrupts. Is
 * assigned as primary handler when request_threaded_irq is called
 * with handler == NULL. Useful for oneshot interrupts.
 */
/*
 * [한국어]
 * irq_default_primary_handler - 스레드를 깨우기만 하는 기본 1 차 핸들러
 *
 * @irq:    인터럽트 번호 (쓰지 않는다)
 * @dev_id: 장치 식별자 (쓰지 않는다)
 * @return: 항상 IRQ_WAKE_THREAD
 *
 * request_threaded_irq 에 1 차 핸들러를 NULL 로 넘기면 이것이 꽂힌다.
 * 하드 인터럽트 문맥에서 아무 일도 하지 않고 스레드를 깨운다.
 *
 * 왜 유용한가: 인터럽트 원인을 확인하는 데 잠들 수 있는 접근이
 * 필요한 장치가 있다. I2C 로 붙은 센서가 그렇다. 그런 장치는 하드
 * 인터럽트 문맥에서 할 수 있는 일이 없으므로 곧바로 스레드로 넘긴다.
 *
 * 그 대신 oneshot 이 필수가 된다. 원인을 확인하지 않았으니 선이
 * 여전히 활성일 수 있고, 그대로 열어 두면 인터럽트가 폭주한다.
 * __setup_irq 가 그 조합을 강제한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 *
 * 호출 체인:
 *   흐름 처리기 → handle_irq_event() → [이 함수]
 */
static irqreturn_t irq_default_primary_handler(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;	/* [한국어] 아무 일도 하지 않고 스레드를 깨운다. 원인을 확인하지 않았으므로 oneshot 으로 선을 막아 두어야 폭주하지 않는다 */
}

/*
 * Primary handler for nested threaded interrupts. Should never be
 * called.
 */
/*
 * [한국어]
 * irq_nested_primary_handler - 중첩 스레드 인터럽트의 함정 핸들러
 *
 * @irq:    인터럽트 번호
 * @dev_id: 장치 식별자 (쓰지 않는다)
 * @return: 항상 IRQ_NONE
 *
 * 중첩 스레드 인터럽트란: 부모 인터럽트의 스레드 안에서 처리되는
 * 자식 인터럽트다. GPIO 확장 칩의 각 핀이 그렇다.
 *
 * 그런 인터럽트는 하드 인터럽트 문맥으로 오지 않는다 —
 * handle_nested_irq() 가 스레드 문맥에서 thread_fn 을 직접 부른다.
 * 그러니 1 차 핸들러가 불릴 일이 없다.
 *
 * 그런데도 함수를 꽂는 이유: 코어 곳곳이 action->handler 를 검사
 * 없이 부른다. NULL 을 두면 그런 경로에서 터진다. 불리면 버그라는
 * 것을 경고로 알린다.
 *
 * 실행 컨텍스트: 불리지 않는 것이 정상.
 *
 * 호출 체인:
 *   (버그가 있을 때만) handle_irq_event_percpu() → [이 함수]
 */
static irqreturn_t irq_nested_primary_handler(int irq, void *dev_id)
{
	WARN(1, "Primary handler called for nested irq %d\n", irq);	/* [한국어] 중첩 인터럽트는 스레드 문맥에서만 처리된다. 여기 오면 코어 논리에 구멍이 있다 */
	return IRQ_NONE;	/* [한국어] "내 것이 아니다" */
}

/*
 * [한국어]
 * irq_forced_secondary_handler - 강제 스레드화 보조 action 의 함정 핸들러
 *
 * @irq:    인터럽트 번호
 * @dev_id: 장치 식별자 (쓰지 않는다)
 * @return: 항상 IRQ_NONE
 *
 * 강제 스레드화(threadirqs)에서 1 차 핸들러와 스레드 핸들러를 모두
 * 가진 드라이버는 특별한 처리가 필요하다. 둘 다 스레드로 옮겨야
 * 하는데 action 하나에 스레드는 하나뿐이다.
 *
 * 그래서 보조 action 을 하나 더 만든다. 원래 1 차 핸들러는 주
 * action 의 스레드가 되고, 원래 스레드 핸들러는 보조 action 의
 * 스레드가 된다.
 *
 * 그 보조 action 의 1 차 핸들러 자리에 이것이 꽂힌다. 보조 action 은
 * 인터럽트 목록에 등록되지 않아 하드 인터럽트 문맥으로 불릴 일이
 * 없다 — 주 action 의 스레드가 끝나면서 깨워 줄 뿐이다.
 *
 * 이 함수의 주소가 표식으로도 쓰인다. irq_thread() 가 이 주소를
 * 비교해 보조 스레드인지 판별하고 우선순위를 다르게 준다.
 *
 * 실행 컨텍스트: 불리지 않는 것이 정상.
 *
 * 호출 체인:
 *   (버그가 있을 때만) → [이 함수]
 */
static irqreturn_t irq_forced_secondary_handler(int irq, void *dev_id)
{
	WARN(1, "Secondary action handler called for irq %d\n", irq);	/* [한국어] 보조 action 은 인터럽트 목록에 등록되지 않아 하드 인터럽트 문맥으로 불릴 일이 없다 */
	return IRQ_NONE;	/* [한국어] 이 함수의 주소가 표식으로도 쓰인다 — irq_thread 가 비교해 보조 스레드를 판별한다 */
}

#ifdef CONFIG_SMP	/* [한국어] 스레드 친화도 조정은 SMP 에만 있다 */
/*
 * Check whether we need to change the affinity of the interrupt thread.
 */
/*
 * [한국어]
 * irq_thread_check_affinity - 스레드가 자기 CPU 친화도를 갱신한다
 *
 * @desc:   대상 서술자
 * @action: 이 스레드의 action
 * @return: 없음
 *
 * irq_set_thread_affinity() 가 표시만 남기고 미뤄 둔 일을, 스레드가
 * 자기 문맥에서 실제로 한다. 그 짝이 이 함수의 존재 이유다.
 *
 * 유효 친화도를 쓰는 것에 주목: 요청 친화도가 아니다. 하드웨어가
 * 실제로 고른 CPU 에서 스레드가 돌아야 캐시 지역성이 살아난다.
 *
 * 마스크를 락 안에서 복사하고 락 밖에서 적용하는 이유:
 * set_cpus_allowed_ptr 이 잠들 수 있어 스핀락을 쥔 채 부를 수 없다.
 *
 * 메모리 부족 시 플래그를 다시 세우는 처리가 흥미롭다. 원본 주석대로
 * 다음번에 다시 시도한다. 스레드는 인터럽트마다 깨어나므로 곧 기회가
 * 온다.
 *
 * __set_current_state(TASK_RUNNING) 이 앞에 있는 이유: 호출자인
 * irq_wait_for_interrupt 가 이미 TASK_INTERRUPTIBLE 로 바꿔 두었다.
 * 그 상태로 잠들 수 있는 함수를 부르면 경고가 난다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥.
 *
 * 호출 체인:
 *   irq_wait_for_interrupt() → [이 함수] → set_cpus_allowed_ptr()
 */
static void irq_thread_check_affinity(struct irq_desc *desc, struct irqaction *action)
{
	cpumask_var_t mask;	/* [한국어] 적용할 마스크의 사본 */

	if (!test_and_clear_bit(IRQTF_AFFINITY, &action->thread_flags))	/* [한국어] 친화도가 바뀌었다는 표시가 있는가. 읽으면서 지운다 */
		return;	/* [한국어] 대부분의 깨어남에서는 표시가 없다 */

	__set_current_state(TASK_RUNNING);	/* [한국어] 호출자가 TASK_INTERRUPTIBLE 로 바꿔 두었다. 그 상태로 잠들 수 있는 함수를 부르면 경고가 난다 */

	/*
	 * In case we are out of memory we set IRQTF_AFFINITY again and
	 * try again next time
	 */
	if (!alloc_cpumask_var(&mask, GFP_KERNEL)) {	/* [한국어] (위 영어 주석) 마스크 할당 실패 */
		set_bit(IRQTF_AFFINITY, &action->thread_flags);	/* [한국어] 표시를 되돌려 다음번에 다시 시도한다. 스레드는 인터럽트마다 깨어나므로 곧 기회가 온다 */
		return;
	}

	scoped_guard(raw_spinlock_irq, &desc->lock) {	/* [한국어] 마스크를 읽는 동안만 락 */
		const struct cpumask *m;	/* [한국어] 서술자 안의 마스크 */

		m = irq_data_get_effective_affinity_mask(&desc->irq_data);	/* [한국어] 요청이 아니라 하드웨어가 실제로 고른 CPU. 그곳에서 스레드가 돌아야 캐시 지역성이 산다 */
		cpumask_copy(mask, m);	/* [한국어] 사본을 만든다 */
	}

	set_cpus_allowed_ptr(current, mask);	/* [한국어] 락 밖에서. 이 함수가 잠들 수 있어 스핀락을 쥔 채 부를 수 없다 */
	free_cpumask_var(mask);	/* [한국어] 사본 반납 */
}
#else	/* [한국어] 단일 프로세서 */
/*
 * [한국어]
 * irq_thread_check_affinity - 스레드 친화도 갱신 (UP 판, 빈 함수)
 *
 * @desc:   무시
 * @action: 무시
 * @return: 없음
 *
 * CPU 가 하나면 옮길 곳이 없다.
 *
 * 호출 체인:
 *   irq_wait_for_interrupt() → [이 함수]
 */
static inline void irq_thread_check_affinity(struct irq_desc *desc, struct irqaction *action) { }	/* [한국어] 옮길 CPU 가 없다 */
#endif	/* [한국어] CONFIG_SMP 분기의 끝 */

/*
 * [한국어]
 * irq_wait_for_interrupt - 인터럽트 스레드가 깨어날 때까지 잠든다
 *
 * @desc:   대상 서술자
 * @action: 이 스레드의 action
 * @return: 0 처리할 인터럽트가 있음, -1 스레드를 끝내야 함
 *
 * 인터럽트 스레드의 대기 루프다. 잠들었다가 깨어나 두 가지를 확인한다 —
 * 종료 요청인가, 처리할 인터럽트가 있는가.
 *
 * 상태 전이 순서가 이 함수에서 가장 미묘하다.
 * set_current_state(TASK_INTERRUPTIBLE) 을 검사보다 *먼저* 한다.
 * 반대로 하면 검사와 잠들기 사이에 깨우기 신호가 와서 그것을 놓친다 —
 * 고전적인 잃어버린 깨우기(lost wakeup) 문제다.
 *
 * 그 순서 덕분에, 검사 사이에 온 wake_up_process 는 상태를
 * TASK_RUNNING 으로 바꿔 두어 아래 schedule() 이 곧바로 반환한다.
 *
 * 종료 처리가 두 갈래인 이유: 스레드를 멈추라는 요청을 받았어도
 * 아직 처리하지 않은 인터럽트가 남아 있을 수 있다. 그러면 한 번 더
 * 돌게 0 을 돌려준다. 원본 주석의 "may need to run one last time" 이
 * 그것이다.
 *
 * test_and_clear_bit 을 쓰는 이유: 읽으면서 지워야 다음 루프에서
 * 같은 인터럽트를 두 번 처리하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥.
 *
 * 호출 체인:
 *   irq_thread() → [이 함수]
 */
static int irq_wait_for_interrupt(struct irq_desc *desc,
				  struct irqaction *action)
{
	for (;;) {	/* [한국어] 무한 루프. 처리할 것이 생기거나 종료 요청이 올 때까지 잠들고 깨기를 반복한다 */
		set_current_state(TASK_INTERRUPTIBLE);	/* [한국어] 검사보다 먼저 상태를 바꾼다. 반대로 하면 검사와 잠들기 사이의 깨우기를 놓친다 — 고전적인 잃어버린 깨우기 문제다 */
		irq_thread_check_affinity(desc, action);	/* [한국어] 친화도가 바뀌었으면 여기서 옮긴다. 그 안에서 상태를 RUNNING 으로 되돌린다 */

		if (kthread_should_stop()) {	/* [한국어] 스레드를 멈추라는 요청이 왔는가 */
			/* may need to run one last time */
			if (test_and_clear_bit(IRQTF_RUNTHREAD,	/* [한국어] (위 영어 주석) 아직 처리하지 않은 인터럽트가 남아 있는가 */
					       &action->thread_flags)) {
				__set_current_state(TASK_RUNNING);	/* [한국어] 잠들지 않는다 */
				return 0;	/* [한국어] 한 번 더 돌게 한다 */
			}
			__set_current_state(TASK_RUNNING);		/* [한국어] 잠들지 않고 나간다 */
			return -1;	/* [한국어] 이제 정말 끝낸다 */
		}

		if (test_and_clear_bit(IRQTF_RUNTHREAD,	/* [한국어] 처리할 인터럽트가 있는가. 읽으면서 지워야 다음 루프에서 두 번 처리하지 않는다 */
				       &action->thread_flags)) {
			__set_current_state(TASK_RUNNING);		/* [한국어] 잠들지 않고 나간다 */
			return 0;	/* [한국어] 처리하러 나간다 */
		}
		schedule();	/* [한국어] 잠든다. 위에서 상태를 미리 바꿔 두었으므로 그 사이에 온 깨우기는 놓치지 않는다 */
	}
}

/*
 * Oneshot interrupts keep the irq line masked until the threaded
 * handler finished. unmask if the interrupt has not been disabled and
 * is marked MASKED.
 */
/*
 * [한국어]
 * irq_finalize_oneshot - 스레드가 끝난 뒤 oneshot 마스크를 정리한다
 *
 * @desc:   대상 서술자
 * @action: 방금 끝난 스레드의 action
 * @return: 없음
 *
 * oneshot 인터럽트는 스레드 핸들러가 끝날 때까지 선을 막아 둔다.
 * 이 함수가 그 마스크를 푸는 곳이다.
 *
 * 여러 등록자가 한 선을 공유할 수 있으므로 비트마스크를 쓴다.
 * 각 action 이 자기 비트를 갖고, 스레드가 깨어날 때 세워졌다가
 * 여기서 지워진다. 모든 비트가 0 이 되어야 선을 연다.
 *
 * INPROGRESS 재시도 루프가 이 함수에서 가장 미묘한 부분이다. 원본
 * 주석이 시나리오를 설명한다 — 스레드가 다른 CPU 의 하드 인터럽트
 * 핸들러보다 먼저 끝나면, 여기서 선을 열자마자 그 인터럽트가 다시
 * 와서 선을 막고, INPROGRESS 때문에 처리를 건너뛴 채 물러난다.
 * 그러면 선이 영영 막힌 채 남는다.
 *
 * 그래서 INPROGRESS 가 내려갈 때까지 락을 놓았다 다시 잡으며 기다린다.
 * 버스 락까지 놓는 이유는 그것을 쥔 채 회전하면 다른 CPU 의 하드
 * 인터럽트 처리가 진행하지 못하기 때문이다.
 *
 * 두 번째 검사도 중요하다. 기다리는 사이에 스레드가 다시 깨어날 수
 * 있는데, 그때 자기 비트를 지우면 그 새 요청이 사라진다.
 *
 * 보조 action 을 걸러 내는 첫 조건: 강제 스레드화의 보조 action 은
 * 주 action 과 마스크 비트를 공유하지 않으므로 정리할 것이 없다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥.
 *
 * 호출 체인:
 *   irq_thread_fn() / irq_thread_dtor() → [이 함수] →
 *   unmask_threaded_irq()
 */
static void irq_finalize_oneshot(struct irq_desc *desc,
				 struct irqaction *action)
{
	if (!(desc->istate & IRQS_ONESHOT) ||	/* [한국어] oneshot 인터럽트가 아니면 막아 둔 것도 없다 */
	    action->handler == irq_forced_secondary_handler)	/* [한국어] 강제 스레드화의 보조 action 은 주 action 과 마스크 비트를 공유하지 않아 정리할 것이 없다 */
		return;
again:	/* [한국어] INPROGRESS 가 내려갈 때까지 재시도하는 지점 */
	chip_bus_lock(desc);	/* [한국어] 아래 언마스크가 느린 버스 뒤의 칩을 건드릴 수 있다 */
	raw_spin_lock_irq(&desc->lock);	/* [한국어] 상태 검사와 갱신 */

	/*
	 * Implausible though it may be we need to protect us against
	 * the following scenario:
	 *
	 * The thread is faster done than the hard interrupt handler
	 * on the other CPU. If we unmask the irq line then the
	 * interrupt can come in again and masks the line, leaves due
	 * to IRQS_INPROGRESS and the irq line is masked forever.
	 *
	 * This also serializes the state of shared oneshot handlers
	 * versus "desc->threads_oneshot |= action->thread_mask;" in
	 * irq_wake_thread(). See the comment there which explains the
	 * serialization.
	 */
	if (unlikely(irqd_irq_inprogress(&desc->irq_data))) {	/* [한국어] (위 영어 주석) 다른 CPU 가 아직 하드 인터럽트를 처리 중인가 */
		raw_spin_unlock_irq(&desc->lock);	/* [한국어] 락을 놓는다. 쥔 채 회전하면 그쪽이 진행하지 못한다 */
		chip_bus_sync_unlock(desc);	/* [한국어] 버스 락도 놓는다. 같은 이유다 */
		cpu_relax();	/* [한국어] 회전 대기 힌트 */
		goto again;	/* [한국어] 다시 시도. 지금 열면 그 인터럽트가 다시 와서 막고 INPROGRESS 때문에 물러나, 선이 영영 막힌다 */
	}

	/*
	 * Now check again, whether the thread should run. Otherwise
	 * we would clear the threads_oneshot bit of this thread which
	 * was just set.
	 */
	if (test_bit(IRQTF_RUNTHREAD, &action->thread_flags))	/* [한국어] (위 영어 주석) 기다리는 사이에 스레드가 다시 깨어났는가 */
		goto out_unlock;	/* [한국어] 자기 비트를 지우면 그 새 요청이 사라진다 */

	desc->threads_oneshot &= ~action->thread_mask;	/* [한국어] 자기 비트를 내린다. 여러 등록자가 각자 비트를 갖는다 */

	if (!desc->threads_oneshot && !irqd_irq_disabled(&desc->irq_data) &&	/* [한국어] 모든 스레드가 끝났고 인터럽트가 꺼지지 않았고 */
	    irqd_irq_masked(&desc->irq_data))	/* [한국어] 실제로 막혀 있는가 */
		unmask_threaded_irq(desc);	/* [한국어] 선을 연다. EOI_THREADED 칩이면 그 안에서 EOI 도 보낸다 */

out_unlock:	/* [한국어] 정상 종료와 재깨움 경로가 합류한다 */
	raw_spin_unlock_irq(&desc->lock);	/* [한국어] 스핀락을 먼저 푼다. 아래 버스 락은 잠들 수 있어 스핀락 안에서 풀 수 없다 */
	chip_bus_sync_unlock(desc);	/* [한국어] 지연된 레지스터 쓰기가 여기서 하드웨어로 나간다 */
}

/*
 * Interrupts explicitly requested as threaded interrupts want to be
 * preemptible - many of them need to sleep and wait for slow busses to
 * complete.
 */
/*
 * [한국어]
 * irq_thread_fn - 스레드 핸들러를 부르고 뒤처리한다
 *
 * @desc:   대상 서술자
 * @action: 이 스레드의 action
 * @return: 핸들러의 반환값
 *
 * 명시적으로 스레드로 요청된 인터럽트의 실행 경로다. 선점 가능한
 * 상태에서 핸들러를 부르므로, 원본 주석대로 잠들거나 느린 버스를
 * 기다릴 수 있다.
 *
 * threads_handled 카운터를 올리는 것에 주목: 오탐 검출기
 * (kernel/irq/spurious.c)가 이 값을 본다. 스레드가 인터럽트를
 * 처리했다면 그 선이 오탐이 아니라는 증거이기 때문이다.
 *
 * 하드 인터럽트 핸들러가 IRQ_WAKE_THREAD 만 돌려주는 경우, 오탐
 * 검출기는 그것만으로는 처리 여부를 알 수 없다. 그래서 스레드의
 * 결과를 이 카운터로 전달받는다.
 *
 * 마지막의 oneshot 정리가 필수다. 이것이 없으면 선이 막힌 채 남는다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥, 선점 가능.
 *
 * 호출 체인:
 *   irq_thread() → [이 함수] → action->thread_fn()
 */
static irqreturn_t irq_thread_fn(struct irq_desc *desc,	struct irqaction *action)
{
	irqreturn_t ret = action->thread_fn(action->irq, action->dev_id);	/* [한국어] 드라이버의 스레드 핸들러. 선점 가능한 상태라 잠들 수 있다 */

	if (ret == IRQ_HANDLED)	/* [한국어] 실제로 처리했는가 */
		atomic_inc(&desc->threads_handled);	/* [한국어] 오탐 검출기가 이 값을 본다. 1 차 핸들러가 WAKE_THREAD 만 돌려주면 그것만으로는 처리 여부를 알 수 없어, 스레드의 결과를 이 카운터로 전달한다 */

	irq_finalize_oneshot(desc, action);	/* [한국어] 막아 둔 선을 정리한다. 이것이 없으면 선이 막힌 채 남는다 */
	return ret;	/* [한국어] 드라이버 스레드 핸들러의 반환값. 호출자가 WAKE_THREAD 인지 본다 */
}

/*
 * Interrupts which are not explicitly requested as threaded
 * interrupts rely on the implicit bh/preempt disable of the hard irq
 * context. So we need to disable bh here to avoid deadlocks and other
 * side effects.
 */
/*
 * [한국어]
 * irq_forced_thread_fn - 강제 스레드화된 핸들러를 원래 문맥처럼 부른다
 *
 * @desc:   대상 서술자
 * @action: 이 스레드의 action
 * @return: 핸들러의 반환값
 *
 * threadirqs 로 강제 스레드화된 핸들러는 원래 하드 인터럽트 문맥을
 * 전제하고 작성됐다. 그 문맥의 성질을 흉내 내지 않으면 오동작한다.
 *
 * 원본 주석이 무엇을 흉내 내는지 짚는다. 하드 인터럽트 문맥에서는
 * 소프트IRQ 가 실행되지 않고 선점도 꺼져 있다. 핸들러가 그 전제로
 * 락을 잡으면, 스레드 문맥에서는 소프트IRQ 가 끼어들어 같은 락을
 * 잡으려다 데드락이 난다.
 *
 * local_irq_disable 을 PREEMPT_RT 에서 건너뛰는 이유: 그 설정에서는
 * 인터럽트를 끄는 것이 실시간 응답성을 해친다. 대신 그쪽 커널은
 * 스핀락 자체가 잠들 수 있는 뮤텍스라 이 흉내가 필요 없다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥. 이 함수 안에서는 소프트IRQ 와
 * (RT 가 아니면) 인터럽트가 꺼져 있다.
 *
 * 호출 체인:
 *   irq_thread() → [이 함수] → irq_thread_fn()
 */
static irqreturn_t irq_forced_thread_fn(struct irq_desc *desc, struct irqaction *action)
{
	irqreturn_t ret;	/* [한국어] 핸들러 결과 */

	local_bh_disable();	/* [한국어] 하드 인터럽트 문맥에서는 소프트IRQ 가 실행되지 않는다. 핸들러가 그 전제로 락을 잡으면 여기서 소프트IRQ 가 끼어들어 데드락이 난다 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))	/* [한국어] 실시간 커널이 아닌가 */
		local_irq_disable();	/* [한국어] RT 에서는 인터럽트를 끄는 것이 응답성을 해치고, 그쪽은 스핀락 자체가 뮤텍스라 이 흉내가 필요 없다 */
	ret = irq_thread_fn(desc, action);	/* [한국어] 흉내 낸 문맥 안에서 핸들러를 부른다 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))	/* [한국어] 위와 대칭 */
		local_irq_enable();
	local_bh_enable();	/* [한국어] 흉내 낸 문맥을 되돌린다. 인터럽트 켜기와 역순이다 */
	return ret;	/* [한국어] 핸들러 결과를 그대로 올린다 */
}

/*
 * [한국어]
 * wake_threads_waitq - 활성 스레드 수를 줄이고 0 이면 대기자를 깨운다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * synchronize_irq() 가 기다리는 카운터를 관리한다. 스레드가 처리를
 * 마칠 때마다 하나 줄이고, 0 이 되면 기다리던 쪽을 깨운다.
 *
 * atomic_dec_and_test 를 쓰는 이유: 감소와 0 검사가 원자적이어야
 * 한다. 나눠 하면 두 스레드가 동시에 끝날 때 둘 다 0 을 못 보거나
 * 둘 다 깨우는 일이 생긴다.
 *
 * 카운터를 올리는 곳은 흩어져 있다 — handle_irq_event_percpu() 와
 * handle_nested_irq() 다. 내리는 곳이 여기 하나로 모인 것이
 * 대칭적이지 않아 보이지만, 올리는 시점이 상황마다 다르기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥 또는 스레드 문맥.
 *
 * 호출 체인:
 *   irq_thread() / handle_nested_irq() / irq_thread_dtor() → [이 함수]
 */
void wake_threads_waitq(struct irq_desc *desc)
{
	if (atomic_dec_and_test(&desc->threads_active))	/* [한국어] 감소와 0 검사가 원자적이어야 한다. 나눠 하면 두 스레드가 동시에 끝날 때 둘 다 0 을 못 보거나 둘 다 깨운다 */
		wake_up(&desc->wait_for_threads);	/* [한국어] synchronize_irq 가 기다리던 큐 */
}

/*
 * [한국어]
 * irq_thread_dtor - 인터럽트 스레드가 비정상 종료할 때 정리한다
 *
 * @unused: task_work 규약상의 인자 (쓰지 않는다)
 * @return: 없음
 *
 * 인터럽트 스레드가 죽을 때 불리도록 task_work 로 걸어 둔 콜백이다.
 * 정상 경로에서는 irq_thread() 가 이것을 취소하므로, 여기 도달하는
 * 것은 비정상 상황이다 — 누군가 스레드를 kill 했거나 OOM 킬러가
 * 골랐거나.
 *
 * 그런 일이 생기면 두 가지를 정리해야 한다. 활성 카운터가 남아 있으면
 * synchronize_irq 가 영원히 기다리고, oneshot 마스크 비트가 남아
 * 있으면 선이 영영 막힌다.
 *
 * PF_EXITING 확인이 안전장치다. 종료 중이 아닌데 이것이 불렸다면
 * task_work 기구가 잘못 동작한 것이다.
 *
 * kthread_data 로 action 을 얻는 것에 주목: 커널 스레드를 만들 때
 * 넘긴 데이터가 그것이다. 스레드 구조체에서 자기 action 을 되찾는
 * 통로다.
 *
 * 실행 컨텍스트: 죽어 가는 스레드의 문맥.
 *
 * 호출 체인:
 *   do_exit() → task_work_run() → [이 함수]
 */
static void irq_thread_dtor(struct callback_head *unused)
{
	struct task_struct *tsk = current;	/* [한국어] 죽어 가는 스레드 */
	struct irq_desc *desc;	/* [한국어] 정리할 서술자 */
	struct irqaction *action;	/* [한국어] 이 스레드의 action */

	if (WARN_ON_ONCE(!(current->flags & PF_EXITING)))	/* [한국어] 종료 중이 아닌데 불렸는가 — task_work 기구가 잘못 동작한 것이다 */
		return;

	action = kthread_data(tsk);	/* [한국어] 스레드를 만들 때 넘긴 데이터. 스레드 구조체에서 자기 action 을 되찾는 통로다 */

	pr_err("exiting task \"%s\" (%d) is an active IRQ thread (irq %d)\n",	/* [한국어] 정상 경로에서는 irq_thread 가 이 콜백을 취소한다. 여기 오면 누군가 스레드를 죽인 것이다 */
	       tsk->comm, tsk->pid, action->irq);


	desc = irq_to_desc(action->irq);	/* [한국어] 대상 서술자 */
	/*
	 * If IRQTF_RUNTHREAD is set, we need to decrement
	 * desc->threads_active and wake possible waiters.
	 */
	if (test_and_clear_bit(IRQTF_RUNTHREAD, &action->thread_flags))	/* [한국어] (위 영어 주석) 처리할 인터럽트를 받아 둔 상태였는가 */
		wake_threads_waitq(desc);	/* [한국어] 카운터가 남으면 synchronize_irq 가 영원히 기다린다 */

	/* Prevent a stale desc->threads_oneshot */
	irq_finalize_oneshot(desc, action);	/* [한국어] (위 영어 주석) 마스크 비트가 남으면 선이 영영 막힌다 */
}

/*
 * [한국어]
 * irq_wake_secondary - 보조 action 의 스레드를 깨운다
 *
 * @desc:   대상 서술자
 * @action: 주 action
 * @return: 없음
 *
 * 강제 스레드화에서 1 차 핸들러와 스레드 핸들러를 모두 가진 드라이버는
 * action 이 둘로 나뉜다. 주 action 의 스레드(원래 1 차 핸들러)가
 * IRQ_WAKE_THREAD 를 돌려주면, 보조 action 의 스레드(원래 스레드
 * 핸들러)를 깨워야 한다.
 *
 * 원래 구조에서 1 차 핸들러가 WAKE_THREAD 를 돌려주면 코어가 스레드를
 * 깨웠다. 강제 스레드화에서는 그 1 차 핸들러 자체가 스레드가 됐으므로,
 * 그 스레드가 끝나면서 다음 단계를 깨워야 한다. 이 함수가 그 다리다.
 *
 * WARN_ON_ONCE 로 보조 action 유무를 확인하는 이유: WAKE_THREAD 를
 * 돌려줬는데 깨울 대상이 없다면 설정이 잘못된 것이다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥.
 *
 * 호출 체인:
 *   irq_thread() → [이 함수] → __irq_wake_thread()
 */
static void irq_wake_secondary(struct irq_desc *desc, struct irqaction *action)
{
	struct irqaction *secondary = action->secondary;	/* [한국어] 원래 스레드 핸들러를 담은 보조 action */

	if (WARN_ON_ONCE(!secondary))	/* [한국어] WAKE_THREAD 를 돌려줬는데 깨울 대상이 없다면 설정이 잘못됐다 */
		return;

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 깨우기 기구가 서술자 상태를 만진다 */
	__irq_wake_thread(desc, secondary);	/* [한국어] 표준 깨우기 경로. oneshot 비트 설정도 그 안에서 한다 */
}

/*
 * Internal function to notify that a interrupt thread is ready.
 */
/*
 * [한국어]
 * irq_thread_set_ready - 스레드가 준비됐음을 알린다
 *
 * @desc:   대상 서술자
 * @action: 이 스레드의 action
 * @return: 없음
 *
 * 인터럽트 스레드가 실행을 시작하자마자 부른다. request_irq 가
 * 이것을 기다린다.
 *
 * 왜 기다려야 하는가: request_irq 가 반환한 직후 인터럽트가 올 수
 * 있는데, 그때 스레드가 아직 스케줄되지 않았다면 깨우기를 놓칠 수
 * 있다. 스레드가 확실히 대기 상태에 들어간 뒤에 인터럽트를 여는
 * 편이 안전하다.
 *
 * wait_for_threads 큐를 재활용하는 것에 주목: 그 큐는 원래
 * synchronize_irq 가 스레드 종료를 기다리는 데 쓴다. 대기 조건이
 * 달라 섞여도 문제가 없어 하나를 공유한다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥, 시작 직후.
 *
 * 호출 체인:
 *   irq_thread() 의 첫 줄 → [이 함수]
 */
static void irq_thread_set_ready(struct irq_desc *desc,
				 struct irqaction *action)
{
	set_bit(IRQTF_READY, &action->thread_flags);	/* [한국어] 준비 표시 */
	wake_up(&desc->wait_for_threads);	/* [한국어] 종료 대기용 큐를 재활용한다. 대기 조건이 달라 섞여도 문제가 없다 */
}

/*
 * Internal function to wake up a interrupt thread and wait until it is
 * ready.
 */
/*
 * [한국어]
 * wake_up_and_wait_for_irq_thread_ready - 스레드를 깨우고 준비를 기다린다
 *
 * @desc:   대상 서술자
 * @action: 대상 action (NULL 이면 아무것도 하지 않는다)
 * @return: 없음
 *
 * __setup_irq 의 마지막 단계다. 만들어 둔 스레드를 실제로 시작시키고
 * 그것이 대기 상태에 들어갈 때까지 기다린다.
 *
 * 왜 기다리는가: request_irq 가 반환하면 인터럽트가 열린 상태다.
 * 그때 스레드가 아직 시작되지 않았다면 첫 인터럽트의 깨우기를 놓칠
 * 수 있다. 미묘한 경쟁이지만 실제로 관찰되어 이 대기가 추가됐다.
 *
 * NULL 을 조용히 넘기는 것에 주목: 스레드가 없는 인터럽트와 보조
 * action 이 없는 경우 모두 그냥 넘어가야 한다. 호출자가 조건 없이
 * 두 번 부를 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 락을 모두 놓은 뒤.
 *
 * 호출 체인:
 *   __setup_irq() → [이 함수]
 */
static void wake_up_and_wait_for_irq_thread_ready(struct irq_desc *desc,
						  struct irqaction *action)
{
	if (!action || !action->thread)	/* [한국어] 스레드가 없는 인터럽트이거나 보조 action 이 없는 경우. 호출자가 조건 없이 두 번 부를 수 있게 한다 */
		return;

	wake_up_process(action->thread);	/* [한국어] 만들어 둔 스레드를 실제로 시작시킨다 */
	wait_event(desc->wait_for_threads,	/* [한국어] 대기 상태에 들어갈 때까지 기다린다. 그러지 않으면 첫 인터럽트의 깨우기를 놓칠 수 있다 */
		   test_bit(IRQTF_READY, &action->thread_flags));
}

/*
 * Interrupt handler thread
 */
/*
 * [한국어]
 * irq_thread - 인터럽트 스레드의 본체
 *
 * @data: 이 스레드가 담당할 irqaction
 * @return: 항상 0 (kthread 규약)
 *
 * 스레드 인터럽트의 실행 루프다. 한 번 시작되면 free_irq 로 멈출
 * 때까지 "잠들었다 깨어나 핸들러를 부르는" 것을 반복한다.
 *
 * 시작 부분의 우선순위 설정이 중요하다. 인터럽트 스레드는 실시간
 * FIFO 우선순위를 받는다 — 일반 작업보다 먼저 스케줄되어야 인터럽트
 * 지연이 예측 가능해진다.
 *
 * 보조 스레드에 다른 우선순위를 주는 것에 주목: 주 스레드(원래
 * 1 차 핸들러)가 보조 스레드(원래 스레드 핸들러)보다 먼저 돌아야
 * 원래 순서가 유지된다. sched_set_fifo_secondary 가 그 조금 낮은
 * 우선순위를 준다. 판별은 핸들러 주소 비교로 한다.
 *
 * handler_fn 선택이 두 번째 갈래다. 강제 스레드화된 핸들러는 원래
 * 하드 인터럽트 문맥을 전제하므로 그 문맥을 흉내 내는 판을 쓴다.
 *
 * task_work 로 정리 콜백을 거는 이유: 스레드가 비정상 종료해도
 * 카운터와 마스크가 정리되게 하려는 것이다. 정상 종료에서는 마지막에
 * 취소한다.
 *
 * 루프 조건이 !irq_wait_for_interrupt(...) 인 것에 주목: 그 함수가
 * -1 을 돌려주면 종료다. 0 이면 처리할 것이 있다는 뜻이다.
 *
 * 원본 주석의 마지막 문단이 정상 종료 경로를 설명한다. __free_irq 가
 * synchronize_hardirq 를 먼저 부르고 kthread_stop 을 하므로, 그
 * 시점에는 새 인터럽트도 없고 oneshot 비트도 세워지지 않는다. 그래서
 * 정리 콜백이 필요 없어 취소한다.
 *
 * 실행 컨텍스트: 자기 커널 스레드 문맥.
 *
 * 호출 체인:
 *   kthread_create() → (스케줄러) → [이 함수] → handler_fn()
 */
static int irq_thread(void *data)
{
	struct callback_head on_exit_work;	/* [한국어] 비정상 종료 시 불릴 정리 콜백. 스택에 두는 것은 스레드가 사는 동안 이 프레임이 유지되기 때문이다 */
	struct irqaction *action = data;	/* [한국어] 이 스레드가 담당할 action */
	struct irq_desc *desc = irq_to_desc(action->irq);	/* [한국어] 대상 서술자 */
	irqreturn_t (*handler_fn)(struct irq_desc *desc,	/* [한국어] 실행할 핸들러 판. 아래에서 두 갈래 중 하나로 정해진다 */
			struct irqaction *action);

	irq_thread_set_ready(desc, action);	/* [한국어] request_irq 가 이것을 기다린다. 첫 인터럽트의 깨우기를 놓치지 않기 위해서다 */

	if (action->handler == irq_forced_secondary_handler)	/* [한국어] 강제 스레드화의 보조 스레드인가. 핸들러 주소가 표식이다 */
		sched_set_fifo_secondary(current);	/* [한국어] 조금 낮은 우선순위. 주 스레드(원래 1 차 핸들러)가 먼저 돌아야 원래 순서가 유지된다 */
	else	/* [한국어] 주 스레드 */
		sched_set_fifo(current);	/* [한국어] 실시간 FIFO. 일반 작업보다 먼저 스케줄되어야 인터럽트 지연이 예측 가능해진다 */

	if (force_irqthreads() && test_bit(IRQTF_FORCED_THREAD,	/* [한국어] 강제 스레드화로 만들어진 스레드인가 */
					   &action->thread_flags))
		handler_fn = irq_forced_thread_fn;	/* [한국어] 원래 하드 인터럽트 문맥을 전제하는 핸들러라 그 문맥을 흉내 내야 한다 */
	else	/* [한국어] 명시적으로 스레드로 요청된 경우 */
		handler_fn = irq_thread_fn;	/* [한국어] 선점 가능한 상태에서 그대로 부른다 */

	init_task_work(&on_exit_work, irq_thread_dtor);	/* [한국어] 비정상 종료 시 카운터와 마스크를 정리할 콜백 */
	task_work_add(current, &on_exit_work, TWA_NONE);	/* [한국어] 이 스레드가 죽으면 불리도록 건다 */

	while (!irq_wait_for_interrupt(desc, action)) {	/* [한국어] 0 이면 처리할 것이 있고 -1 이면 종료다 */
		irqreturn_t action_ret;	/* [한국어] 핸들러 결과 */

		action_ret = handler_fn(desc, action);	/* [한국어] 위에서 고른 판으로 핸들러를 부른다 */
		if (action_ret == IRQ_WAKE_THREAD)	/* [한국어] 강제 스레드화에서 원래 1 차 핸들러가 스레드를 깨우려는 경우 */
			irq_wake_secondary(desc, action);	/* [한국어] 보조 스레드를 깨운다. 원래 구조의 다음 단계다 */

		wake_threads_waitq(desc);	/* [한국어] 활성 카운터를 내린다. 0 이 되면 synchronize_irq 가 깨어난다 */
	}

	/*
	 * This is the regular exit path. __free_irq() is stopping the
	 * thread via kthread_stop() after calling
	 * synchronize_hardirq(). So neither IRQTF_RUNTHREAD nor the
	 * oneshot mask bit can be set.
	 */
	task_work_cancel_func(current, irq_thread_dtor);	/* [한국어] (위 영어 주석) 정상 종료에서는 정리할 것이 없다. __free_irq 가 synchronize_hardirq 를 먼저 불러 새 인터럽트도 없고 oneshot 비트도 세워지지 않았다 */
	return 0;	/* [한국어] kthread 규약 */
}

/**
 * irq_wake_thread - wake the irq thread for the action identified by dev_id
 * @irq:	Interrupt line
 * @dev_id:	Device identity for which the thread should be woken
 */
/*
 * [한국어]
 * irq_wake_thread - 드라이버가 직접 인터럽트 스레드를 깨운다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 어느 등록자의 스레드를 깨울지
 * @return: 없음
 *
 * 보통 스레드는 1 차 핸들러가 IRQ_WAKE_THREAD 를 돌려줄 때 깨어난다.
 * 이 함수는 그 경로를 거치지 않고 드라이버가 직접 깨우게 한다.
 *
 * 언제 쓰는가: 인터럽트가 아닌 다른 이유로 스레드 핸들러의 작업이
 * 필요한 경우다. 예를 들어 드라이버가 폴링으로 상태 변화를
 * 발견했는데, 그 처리 코드가 스레드 핸들러에 있는 경우다.
 *
 * dev_id 로 등록자를 찾는 것에 주목: 공유 인터럽트에는 여러 스레드가
 * 있을 수 있고, 그중 자기 것만 깨워야 한다.
 *
 * per-CPU 인터럽트를 거절하는 이유: 그쪽은 스레드를 쓸 수 없다
 * (IRQF_NO_THREAD 가 강제된다).
 *
 * 실행 컨텍스트: 제약이 적다. 인터럽트를 끈 채 스핀락만 잡는다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __irq_wake_thread()
 */
void irq_wake_thread(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irqaction *action;	/* [한국어] 순회용 */

	if (!desc || WARN_ON(irq_settings_is_per_cpu_devid(desc)))	/* [한국어] per-CPU 인터럽트는 스레드를 쓸 수 없다 */
		return;

	guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 목록 순회와 깨우기 보호 */
	for_each_action_of_desc(desc, action) {	/* [한국어] 공유 인터럽트에는 여러 등록자가 있다 */
		if (action->dev_id == dev_id) {	/* [한국어] 자기 것인가. 남의 스레드를 깨우면 안 된다 */
			if (action->thread)	/* [한국어] 스레드가 있는 등록자인가 */
				__irq_wake_thread(desc, action);	/* [한국어] 표준 깨우기 경로. oneshot 비트 설정도 그 안에서 한다 */
			break;	/* [한국어] dev_id 는 유일하므로 더 볼 필요가 없다 */
		}
	}
}
EXPORT_SYMBOL_GPL(irq_wake_thread);	/* [한국어] 드라이버가 부른다 */

/*
 * [한국어]
 * irq_setup_forced_threading - 강제 스레드화를 위해 action 을 개조한다
 *
 * @new: 등록하려는 action
 * @return: 0 성공, -ENOMEM 보조 action 할당 실패
 *
 * threadirqs 부팅 옵션이 켜진 시스템에서, 스레드로 요청되지 않은
 * 인터럽트를 스레드로 바꾼다.
 *
 * 세 가지를 건너뛴다. IRQF_NO_THREAD 는 드라이버가 명시적으로
 * 거부한 것이고, IRQF_PERCPU 는 스레드화가 의미 없으며, IRQF_ONESHOT
 * 은 이미 스레드 기구를 쓰고 있다는 뜻이다.
 *
 * 개조 방식이 이 함수의 핵심이다. 1 차 핸들러를 스레드 핸들러 자리로
 * 옮기고, 1 차 자리에는 "스레드를 깨우기만 하는" 기본 핸들러를
 * 넣는다. 그러면 원래 1 차 핸들러가 스레드 문맥에서 실행된다.
 *
 * 그런데 드라이버가 1 차와 스레드 핸들러를 모두 가진 경우 자리가
 * 부족하다. 그래서 보조 action 을 만들어 원래 스레드 핸들러를
 * 그쪽으로 옮긴다. 결과적으로 스레드가 둘이 되고, 주 스레드가
 * 끝나면서 보조를 깨운다.
 *
 * IRQF_ONESHOT 을 강제로 세우는 이유: 1 차 핸들러가 스레드로 옮겨졌으니
 * 하드 인터럽트 문맥에서는 아무도 원인을 해소하지 않는다. 선을 막아
 * 두지 않으면 폭주한다.
 *
 * 이미 기본 핸들러인 경우를 건너뛰는 이유: 그것은 애초에 스레드로
 * 요청된 인터럽트라 개조할 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, request_irq 경로.
 *
 * 호출 체인:
 *   __setup_irq() → [이 함수]
 */
static int irq_setup_forced_threading(struct irqaction *new)
{
	if (!force_irqthreads())	/* [한국어] threadirqs 가 켜져 있는가 */
		return 0;
	if (new->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT))	/* [한국어] 드라이버가 거부했거나, per-CPU 라 의미가 없거나, 이미 스레드 기구를 쓰는가 */
		return 0;

	/*
	 * No further action required for interrupts which are requested as
	 * threaded interrupts already
	 */
	if (new->handler == irq_default_primary_handler)	/* [한국어] (위 영어 주석) 애초에 스레드로 요청된 인터럽트인가 */
		return 0;	/* [한국어] 개조할 것이 없다 */

	new->flags |= IRQF_ONESHOT;	/* [한국어] 1 차 핸들러가 스레드로 옮겨지면 하드 인터럽트 문맥에서 아무도 원인을 해소하지 않는다. 선을 막지 않으면 폭주한다 */

	/*
	 * Handle the case where we have a real primary handler and a
	 * thread handler. We force thread them as well by creating a
	 * secondary action.
	 */
	if (new->handler && new->thread_fn) {	/* [한국어] (위 영어 주석) 둘 다 가진 드라이버인가 — 자리가 부족하다 */
		/* Allocate the secondary action */
		new->secondary = kzalloc_obj(struct irqaction);	/* [한국어] (위 영어 주석) 원래 스레드 핸들러를 담을 곳 */
		if (!new->secondary)	/* [한국어] 메모리 부족 */
			return -ENOMEM;
		new->secondary->handler = irq_forced_secondary_handler;	/* [한국어] 불리면 안 되는 함정. 이 주소가 보조 스레드 판별의 표식이기도 하다 */
		new->secondary->thread_fn = new->thread_fn;	/* [한국어] 원래 스레드 핸들러를 여기로 옮긴다 */
		new->secondary->dev_id = new->dev_id;	/* [한국어] 아래 세 필드는 주 action 에서 복사한다 */
		new->secondary->irq = new->irq;		/* [한국어] 인터럽트 번호도 같다 */
		new->secondary->name = new->name;		/* [한국어] 이름도 공유한다. /proc 에는 "-s-" 가 붙은 스레드 이름으로 구분된다 */
	}
	/* Deal with the primary handler */
	set_bit(IRQTF_FORCED_THREAD, &new->thread_flags);	/* [한국어] (위 영어 주석) irq_thread 가 이 표시를 보고 문맥 흉내 판을 고른다 */
	new->thread_fn = new->handler;	/* [한국어] 1 차 핸들러를 스레드 자리로 옮긴다. 이것이 개조의 핵심이다 */
	new->handler = irq_default_primary_handler;	/* [한국어] 1 차 자리에는 스레드를 깨우기만 하는 함수를 넣는다 */
	return 0;	/* [한국어] 개조 완료. 이제 두 핸들러가 모두 스레드 문맥에서 실행된다 */
}

/*
 * [한국어]
 * irq_request_resources - 칩이 필요로 하는 자원을 잡는다
 *
 * @desc: 대상 서술자
 * @return: 0 성공 또는 콜백 없음, 음수 칩이 낸 오류
 *
 * 인터럽트를 쓰기 전에 필요한 것 — 클록, 전원, GPIO 핀 — 을 칩
 * 드라이버가 잡게 한다.
 *
 * 실패할 수 있는 준비 작업을 한곳에 모아 두면, 실패 시 되돌리기가
 * 쉬워진다. 인터럽트를 시작한 뒤에 자원 획득이 실패하면 이미 열린
 * 선을 다시 닫아야 하는데, 그 사이에 인터럽트가 올 수 있다.
 *
 * 첫 등록자만 부르는 것이 호출자의 책임이다. __setup_irq 가
 * desc->action 이 비었을 때만 이것을 부른다 — 자원은 선 하나에
 * 대한 것이지 등록자마다가 아니다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 버스 락 보유. 잠들 수 있다.
 *
 * 호출 체인:
 *   __setup_irq() → [이 함수] → chip->irq_request_resources()
 */
static int irq_request_resources(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;	/* [한국어] 칩 콜백에 넘길 데이터 */
	struct irq_chip *c = d->chip;	/* [한국어] 담당 칩 */

	return c->irq_request_resources ? c->irq_request_resources(d) : 0;	/* [한국어] 선택적 콜백이라 없으면 성공이다. 잡을 자원이 없다는 뜻이다 */
}

/*
 * [한국어]
 * irq_release_resources - 칩이 잡은 자원을 반납한다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 request 의 반대다. 마지막 등록자가 떠날 때 불린다.
 *
 * 반환값이 없는 것이 자연스럽다 — 해제는 실패할 수 없고, 실패해도
 * 호출자가 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 버스 락 보유. 잠들 수 있다.
 *
 * 호출 체인:
 *   __free_irq() / __setup_irq() 의 실패 경로 → [이 함수]
 */
static void irq_release_resources(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;	/* [한국어] 칩 콜백에 넘길 데이터 */
	struct irq_chip *c = d->chip;	/* [한국어] 담당 칩 */

	if (c->irq_release_resources)	/* [한국어] 반납할 자원이 있는 칩인가 */
		c->irq_release_resources(d);	/* [한국어] 반환값이 없다. 해제는 실패할 수 없고 실패해도 할 일이 없다 */
}

/*
 * [한국어]
 * irq_supports_nmi - 이 인터럽트를 NMI 로 쓸 수 있는지 본다
 *
 * @desc: 대상 서술자
 * @return: true 가능, false 불가
 *
 * NMI 는 제약이 많다. 세 조건을 모두 만족해야 한다.
 *
 * 계층형이면 안 된다. 원본 주석대로 가장 안쪽 컨트롤러가 직접
 * 관리하는 인터럽트만 NMI 가 될 수 있다. 계층을 거치면 각 층이
 * 자기 처리를 하는데, 그 코드가 NMI 안전하지 않다.
 *
 * 느린 버스 뒤에 있으면 안 된다. NMI 처리에서 I2C 전송을 기다릴 수
 * 없다 — 잠들 수 없는 문맥이기 때문이다.
 *
 * 칩이 NMI 지원을 선언해야 한다. 하드웨어가 그 선을 마스크 불가로
 * 설정할 수 있어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   request_nmi() / request_percpu_nmi() → [이 함수]
 */
static bool irq_supports_nmi(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 계층과 칩을 볼 데이터 */

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형이 있는 빌드에만 이 검사가 의미가 있다 */
	/* Only IRQs directly managed by the root irqchip can be set as NMI */
	if (d->parent_data)	/* [한국어] (위 영어 주석) 계층에 속해 있는가 */
		return false;	/* [한국어] 각 층의 처리 코드가 NMI 안전하지 않다 */
#endif
	/* Don't support NMIs for chips behind a slow bus */
	if (d->chip->irq_bus_lock || d->chip->irq_bus_sync_unlock)	/* [한국어] (위 영어 주석) 느린 버스 뒤의 칩인가 */
		return false;	/* [한국어] NMI 문맥에서 I2C 전송을 기다릴 수 없다 — 잠들 수 없기 때문이다 */

	return d->chip->flags & IRQCHIP_SUPPORTS_NMI;	/* [한국어] 하드웨어가 그 선을 마스크 불가로 설정할 수 있어야 한다 */
}

/*
 * [한국어]
 * irq_nmi_setup - 칩에 NMI 설정을 요청한다
 *
 * @desc: 대상 서술자
 * @return: 0 성공, -EINVAL 콜백 없음, 그 외 칩의 오류
 *
 * 하드웨어에 "이 선을 마스크 불가로 만들라" 고 알린다. ARM GIC 라면
 * 그 인터럽트의 우선순위를 특별한 값으로 올려 마스크를 우회하게 한다.
 *
 * 콜백이 없으면 -EINVAL 인 것에 주목: 위 irq_supports_nmi 가 이미
 * 걸러 냈어야 하는 상황이다. 방어적으로 남아 있는 검사다.
 *
 * 실행 컨텍스트: desc->lock 보유. NMI 설정 자체는 하드웨어 레지스터
 * 조작이라 빠르다.
 *
 * 호출 체인:
 *   request_nmi() / prepare_percpu_nmi() → [이 함수] →
 *   chip->irq_nmi_setup()
 */
static int irq_nmi_setup(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 칩 콜백에 넘길 데이터 */
	struct irq_chip *c = d->chip;	/* [한국어] 담당 칩 */

	return c->irq_nmi_setup ? c->irq_nmi_setup(d) : -EINVAL;	/* [한국어] 위 irq_supports_nmi 가 이미 걸러 냈어야 한다. 방어적으로 남은 검사다 */
}

/*
 * [한국어]
 * irq_nmi_teardown - 칩의 NMI 설정을 되돌린다
 *
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 위 setup 의 반대다. 그 선을 다시 보통 인터럽트로 되돌린다.
 *
 * 반환값이 없는 것과 콜백이 없어도 조용히 넘어가는 것이 setup 과
 * 다르다. 해제 경로의 일반적인 관례다 — 실패를 처리할 방법이 없다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   free_nmi() / teardown_percpu_nmi() → [이 함수] →
 *   chip->irq_nmi_teardown()
 */
static void irq_nmi_teardown(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);	/* [한국어] 칩 콜백에 넘길 데이터 */
	struct irq_chip *c = d->chip;	/* [한국어] 담당 칩 */

	if (c->irq_nmi_teardown)	/* [한국어] 되돌릴 것이 있는 칩인가 */
		c->irq_nmi_teardown(d);	/* [한국어] 반환값이 없다. 해제 경로에서 실패를 처리할 방법이 없다 */
}

/*
 * [한국어]
 * setup_irq_thread - 인터럽트 스레드를 만든다
 *
 * @new:       스레드를 붙일 action
 * @irq:       인터럽트 번호 (스레드 이름에 쓴다)
 * @secondary: 보조 스레드인가
 * @return:    0 성공, 음수 스레드 생성 실패
 *
 * kthread_create 로 스레드를 만들고 action 에 연결한다. 아직
 * 시작시키지는 않는다 — 그것은 __setup_irq 가 모든 준비를 마친 뒤에 한다.
 *
 * 이름 짓기가 실용적이다. "irq/24-eth0" 처럼 번호와 장치 이름을
 * 붙여, ps 로 보면 어느 인터럽트의 스레드인지 알 수 있다. 보조
 * 스레드는 "-s-" 를 넣어 구분한다.
 *
 * 참조를 잡아 두는 이유가 원본 주석에 있다. 스레드가 죽어도 인터럽트
 * 코드가 해제된 task_struct 를 참조하지 않게 한다.
 *
 * kthread_bind_mask 로 모든 CPU 를 허용하는 것이 미묘하다. 원본
 * 주석대로 진짜 친화도는 인터럽트가 켜진 뒤에야 정해지므로, 지금은
 * 제한하지 않는다. 다만 그 호출이 스레드를 "바인딩됨" 으로 표시해,
 * cpuset 이나 하우스키핑이 마음대로 옮기지 못하게 한다 — 인터럽트
 * 스레드는 벡터와 함께 옮겨야지 따로 옮기면 안 되기 때문이다.
 *
 * IRQTF_AFFINITY 를 미리 세우는 이유: 스레드가 처음 실행될 때
 * 친화도를 한 번 맞추게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, request_irq 경로.
 *
 * 호출 체인:
 *   __setup_irq() → [이 함수] → kthread_create()
 */
static int
setup_irq_thread(struct irqaction *new, unsigned int irq, bool secondary)
{
	struct task_struct *t;	/* [한국어] 만든 스레드 */

	if (!secondary) {	/* [한국어] 주 스레드인가 */
		t = kthread_create(irq_thread, new, "irq/%d-%s", irq,	/* [한국어] "irq/24-eth0" 처럼. ps 로 보면 어느 인터럽트의 스레드인지 알 수 있다 */
				   new->name);
	} else {	/* [한국어] 보조 스레드 */
		t = kthread_create(irq_thread, new, "irq/%d-s-%s", irq,	/* [한국어] "-s-" 로 구분한다 */
				   new->name);
	}

	if (IS_ERR(t))	/* [한국어] 생성 실패 */
		return PTR_ERR(t);

	/*
	 * We keep the reference to the task struct even if
	 * the thread dies to avoid that the interrupt code
	 * references an already freed task_struct.
	 */
	new->thread = get_task_struct(t);	/* [한국어] (위 영어 주석) 참조를 잡는다. 스레드가 죽어도 인터럽트 코드가 해제된 구조체를 만지지 않는다 */

	/*
	 * The affinity can not be established yet, but it will be once the
	 * interrupt is enabled. Delay and defer the actual setting to the
	 * thread itself once it is ready to run. In the meantime, prevent
	 * it from ever being re-affined directly by cpuset or
	 * housekeeping. The proper way to do it is to re-affine the whole
	 * vector.
	 */
	kthread_bind_mask(t, cpu_possible_mask);	/* [한국어] (위 영어 주석) 모든 CPU 를 허용하되 "바인딩됨" 으로 표시한다. cpuset 이나 하우스키핑이 이 스레드만 따로 옮기면 안 된다 — 벡터와 함께 옮겨야 한다 */

	/*
	 * Ensure the thread adjusts the affinity once it reaches the
	 * thread function.
	 */
	set_bit(IRQTF_AFFINITY, &new->thread_flags);	/* [한국어] (위 영어 주석) 스레드가 처음 실행될 때 친화도를 한 번 맞추게 한다 */

	return 0;	/* [한국어] 아직 시작시키지 않았다. __setup_irq 가 모든 준비를 마친 뒤에 한다 */
}

/*
 * [한국어]
 * valid_percpu_irqaction - per-CPU 인터럽트를 공유해도 되는지 확인한다
 *
 * @old: 기존 등록자 목록의 시작
 * @new: 새로 등록하려는 action
 * @return: true 공유 가능, false 충돌
 *
 * per-CPU 인터럽트의 공유는 일반 공유와 뜻이 다르다. 일반 공유는
 * 같은 선에 여러 드라이버가 붙어 모두 불리는 것이고, per-CPU 공유는
 * CPU 를 나눠 갖는 것이다 — CPU 0~3 은 이 드라이버, 4~7 은 저
 * 드라이버 식이다.
 *
 * 그래서 친화도가 겹치면 안 된다. 겹치면 그 CPU 에서 어느 핸들러를
 * 부를지 정할 수 없다. handle_percpu_devid_irq() 가 친화도로 핸들러를
 * 고르므로 그 전제가 깨진다.
 *
 * dev_id 도 겹치면 안 된다. 그것이 해제할 때 등록자를 식별하는
 * 열쇠라, 같으면 어느 것을 해제할지 알 수 없다.
 *
 * do-while 로 쓴 것에 주목: 호출자가 old 가 NULL 이 아님을 이미
 * 확인했다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   __setup_irq() → [이 함수]
 */
static bool valid_percpu_irqaction(struct irqaction *old, struct irqaction *new)
{
	do {
		if (cpumask_intersects(old->affinity, new->affinity) ||	/* [한국어] 담당 CPU 가 겹치는가. 겹치면 그 CPU 에서 어느 핸들러를 부를지 정할 수 없다 */
		    old->percpu_dev_id == new->percpu_dev_id)	/* [한국어] 식별자가 같은가. 해제할 때 어느 것인지 알 수 없다 */
			return false;

		old = old->next;	/* [한국어] 기존 등록자를 전부 확인한다 */
	} while (old);

	return true;	/* [한국어] CPU 를 나눠 가지므로 공유해도 된다 */
}

/*
 * Internal function to register an irqaction - typically used to
 * allocate special interrupts that are part of the architecture.
 *
 * Locking rules:
 *
 * desc->request_mutex	Provides serialization against a concurrent free_irq()
 *   chip_bus_lock	Provides serialization for slow bus operations
 *     desc->lock	Provides serialization against hard interrupts
 *
 * chip_bus_lock and desc->lock are sufficient for all other management and
 * interrupt related functions. desc->request_mutex solely serializes
 * request/free_irq().
 */
/*
 * [한국어]
 * __setup_irq - 인터럽트 등록의 전부
 *
 * @irq:  대상 인터럽트 번호
 * @desc: 대상 서술자
 * @new:  등록할 action (호출자가 채워 놓았다)
 * @return: 0 성공, 음수 오류
 *
 * 이 파일에서 가장 긴 함수이고, request_irq 계열이 모두 여기로 모인다.
 * 크게 여섯 단계다.
 *
 *   1. 사전 검사와 스레드화 결정 (락 밖)
 *   2. 스레드 생성 (락 밖)
 *   3. 락 획득: request_mutex → bus_lock → desc->lock
 *   4. 첫 등록자면 자원 요청
 *   5. 공유 검사와 oneshot 마스크 비트 배정
 *   6. 첫 등록자면 트리거 설정·활성화·시작
 *
 * 위 원본 주석의 락 계층이 이 함수의 뼈대다. 세 락을 안쪽으로
 * 갈수록 짧게 잡는다. request_mutex 는 request_irq 와 free_irq 를
 * 서로 배제하고, bus_lock 은 느린 버스 접근을 직렬화하며, desc->lock
 * 은 하드 인터럽트와 경쟁을 막는다.
 *
 * 스레드를 락 밖에서 만드는 이유: kthread_create 가 잠들 수 있고
 * 시간도 오래 걸린다. 락 안에서 하면 그동안 이 인터럽트의 모든
 * 관리 작업이 막힌다.
 *
 * 에러 경로가 네 레이블로 나뉜 것도 그 계층을 반영한다 — 어디까지
 * 진행했느냐에 따라 되돌릴 것이 다르다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 여러 곳에서 잠들 수 있다.
 *
 * 호출 체인:
 *   request_threaded_irq() / request_nmi() / request_percpu_irq_affinity()
 *   → [이 함수]
 */
static int
__setup_irq(unsigned int irq, struct irq_desc *desc, struct irqaction *new)
{
	struct irqaction *old, **old_ptr;	/* [한국어] 기존 등록자와 그것을 가리키는 포인터의 주소. 목록 끝에 매달 때 쓴다 */
	unsigned long flags, thread_mask = 0;	/* [한국어] 인터럽트 상태와, 이미 쓰인 oneshot 비트들의 합집합 */
	int ret, nested, shared = 0;	/* [한국어] 결과, 중첩 스레드 여부, 공유 등록 여부 */
	bool per_cpu_devid;	/* [한국어] per-CPU 장치 ID 를 쓰는 인터럽트인가 */

	if (!desc)	/* [한국어] 서술자가 없는가 */
		return -EINVAL;

	if (desc->irq_data.chip == &no_irq_chip)	/* [한국어] 아직 칩이 붙지 않았는가 */
		return -ENOSYS;	/* [한국어] 컨트롤러 드라이버가 아직 이 선을 설정하지 않았다 */
	if (!try_module_get(desc->owner))	/* [한국어] 소유 모듈을 붙잡는다. 인터럽트를 쓰는 동안 그 모듈이 내려가면 안 된다 */
		return -ENODEV;	/* [한국어] 모듈이 이미 내려가는 중이다 */

	per_cpu_devid = irq_settings_is_per_cpu_devid(desc);	/* [한국어] 아래 여러 검사에서 쓰므로 미리 읽어 둔다 */

	new->irq = irq;	/* [한국어] action 이 자기 번호를 기억한다. 핸들러와 스레드가 이 값을 쓴다 */

	/*
	 * If the trigger type is not specified by the caller,
	 * then use the default for this interrupt.
	 */
	if (!(new->flags & IRQF_TRIGGER_MASK))	/* [한국어] (위 영어 주석) 호출자가 트리거를 지정하지 않았는가 */
		new->flags |= irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 디바이스 트리나 이전 설정에서 온 값을 쓴다. 드라이버가 몰라도 되게 한다 */

	/*
	 * IRQF_ONESHOT means the interrupt source in the IRQ chip will be
	 * masked until the threaded handled is done. If there is no thread
	 * handler then it makes no sense to have IRQF_ONESHOT.
	 */
	WARN_ON_ONCE(new->flags & IRQF_ONESHOT && !new->thread_fn);	/* [한국어] (위 영어 주석) oneshot 은 "스레드가 끝날 때까지 막는다" 는 뜻이라 스레드가 없으면 뜻이 통하지 않는다 */

	/*
	 * Check whether the interrupt nests into another interrupt
	 * thread.
	 */
	nested = irq_settings_is_nested_thread(desc);	/* [한국어] (위 영어 주석) 부모 인터럽트의 스레드 안에서 처리되는 자식인가 */
	if (nested) {	/* [한국어] 중첩 인터럽트 */
		if (!new->thread_fn) {	/* [한국어] 스레드 핸들러가 없는가 */
			ret = -EINVAL;	/* [한국어] 중첩 인터럽트는 스레드 문맥에서만 처리되므로 스레드 핸들러가 필수다 */
			goto out_mput;			/* [한국어] 모듈 참조만 놓으면 된다. 아직 스레드도 락도 잡지 않았다 */
		}
		/*
		 * Replace the primary handler which was provided from
		 * the driver for non nested interrupt handling by the
		 * dummy function which warns when called.
		 */
		new->handler = irq_nested_primary_handler;	/* [한국어] (위 영어 주석) 1 차 핸들러는 불릴 일이 없다. 함정으로 바꿔 혹시 불리면 알 수 있게 한다 */
	} else {	/* [한국어] 보통의 인터럽트 */
		if (irq_settings_can_thread(desc)) {	/* [한국어] 스레드화가 허용된 선인가 */
			ret = irq_setup_forced_threading(new);	/* [한국어] threadirqs 가 켜져 있으면 action 을 개조한다 */
			if (ret)	/* [한국어] 보조 action 할당 실패 */
				goto out_mput;
		}
	}

	/*
	 * Create a handler thread when a thread function is supplied
	 * and the interrupt does not nest into another interrupt
	 * thread.
	 */
	if (new->thread_fn && !nested) {	/* [한국어] (위 영어 주석) 스레드 핸들러가 있고 중첩이 아닌가. 중첩은 부모의 스레드를 쓰므로 자기 스레드가 필요 없다 */
		ret = setup_irq_thread(new, irq, false);	/* [한국어] 락 밖에서 만든다. kthread_create 가 잠들 수 있고 오래 걸려, 락 안에서 하면 그동안 모든 관리 작업이 막힌다 */
		if (ret)	/* [한국어] 스레드 생성 실패 */
			goto out_mput;
		if (new->secondary) {	/* [한국어] 강제 스레드화로 보조 action 이 만들어졌는가 */
			ret = setup_irq_thread(new->secondary, irq, true);	/* [한국어] 보조 스레드도 만든다 */
			if (ret)	/* [한국어] 실패 */
				goto out_thread;	/* [한국어] 주 스레드까지 되돌려야 한다 */
		}
	}

	/*
	 * Drivers are often written to work w/o knowledge about the
	 * underlying irq chip implementation, so a request for a
	 * threaded irq without a primary hard irq context handler
	 * requires the ONESHOT flag to be set. Some irq chips like
	 * MSI based interrupts are per se one shot safe. Check the
	 * chip flags, so we can avoid the unmask dance at the end of
	 * the threaded handler for those.
	 */
	if (desc->irq_data.chip->flags & IRQCHIP_ONESHOT_SAFE)	/* [한국어] (위 영어 주석) MSI 처럼 본디 oneshot 이 필요 없는 칩인가 */
		new->flags &= ~IRQF_ONESHOT;	/* [한국어] 플래그를 지운다. 스레드 끝의 마스크·언마스크 춤을 통째로 아낀다 — MSI 는 에지 방식이라 선이 계속 활성일 일이 없다 */

	/*
	 * Protects against a concurrent __free_irq() call which might wait
	 * for synchronize_hardirq() to complete without holding the optional
	 * chip bus lock and desc->lock. Also protects against handing out
	 * a recycled oneshot thread_mask bit while it's still in use by
	 * its previous owner.
	 */
	mutex_lock(&desc->request_mutex);	/* [한국어] (위 영어 주석) 가장 바깥 락. free_irq 가 synchronize_hardirq 를 기다리는 동안 다른 두 락을 놓으므로, 이 뮤텍스가 그 구간을 지킨다 */

	/*
	 * Acquire bus lock as the irq_request_resources() callback below
	 * might rely on the serialization or the magic power management
	 * functions which are abusing the irq_bus_lock() callback,
	 */
	chip_bus_lock(desc);	/* [한국어] (위 영어 주석) 중간 락. 일부 드라이버가 이 콜백을 전원 관리 용도로 남용해, 자원 요청이 그 직렬화에 기댈 수 있다 */

	/* First installed action requests resources. */
	if (!desc->action) {	/* [한국어] (위 영어 주석) 첫 등록자인가 */
		ret = irq_request_resources(desc);	/* [한국어] 자원은 선 하나에 대한 것이지 등록자마다가 아니다 */
		if (ret) {	/* [한국어] 실패 */
			pr_err("Failed to request resources for %s (irq %d) on irqchip %s\n",			/* [한국어] 어느 드라이버가 어느 컨트롤러에서 실패했는지 함께 찍는다 */
			       new->name, irq, desc->irq_data.chip->name);
			goto out_bus_unlock;	/* [한국어] 아직 desc->lock 을 잡지 않았다 */
		}
	}

	/*
	 * The following block of code has to be executed atomically
	 * protected against a concurrent interrupt and any of the other
	 * management calls which are not serialized via
	 * desc->request_mutex or the optional bus lock.
	 */
	raw_spin_lock_irqsave(&desc->lock, flags);	/* [한국어] (위 영어 주석) 가장 안쪽 락. 여기서부터 하드 인터럽트와 경쟁하는 상태를 만진다 */
	old_ptr = &desc->action;	/* [한국어] 목록 머리의 주소. 아래에서 끝까지 따라가며 매달 자리를 찾는다 */
	old = *old_ptr;	/* [한국어] 첫 등록자 */
	if (old) {	/* [한국어] 이미 등록자가 있는가 — 공유 등록이다 */
		/*
		 * Can't share interrupts unless both agree to and are
		 * the same type (level, edge, polarity). So both flag
		 * fields must have IRQF_SHARED set and the bits which
		 * set the trigger type must match. Also all must
		 * agree on ONESHOT.
		 * Interrupt lines used for NMIs cannot be shared.
		 */
		unsigned int oldtype;	/* [한국어] 기존 트리거 방식 */

		if (irq_is_nmi(desc) && !per_cpu_devid) {	/* [한국어] (위 영어 주석) NMI 인가. per-CPU NMI 는 CPU 를 나눠 갖는 방식이라 예외다 */
			pr_err("Invalid attempt to share NMI for %s (irq %d) on irqchip %s.\n",	/* [한국어] NMI 는 공유할 수 없다. 여러 핸들러를 순회하는 것 자체가 NMI 문맥에서 위험하다 */
				new->name, irq, desc->irq_data.chip->name);
			ret = -EINVAL;			/* [한국어] NMI 는 공유할 수 없다 */
			goto out_unlock;			/* [한국어] desc->lock 을 잡은 상태라 그 경로로 나간다 */
		}

		if (per_cpu_devid && !valid_percpu_irqaction(old, new)) {	/* [한국어] per-CPU 인터럽트인데 담당 CPU 나 식별자가 겹치는가 */
			pr_err("Overlapping affinities for %s (irq %d) on irqchip %s.\n",	/* [한국어] 겹치면 그 CPU 에서 어느 핸들러를 부를지 정할 수 없다 */
				new->name, irq, desc->irq_data.chip->name);
			ret = -EINVAL;			/* [한국어] 담당 CPU 나 식별자가 겹친다 */
			goto out_unlock;			/* [한국어] 같은 정리 경로 */
		}

		/*
		 * If nobody did set the configuration before, inherit
		 * the one provided by the requester.
		 */
		if (irqd_trigger_type_was_set(&desc->irq_data)) {	/* [한국어] (위 영어 주석) 트리거가 이미 정해져 있는가 */
			oldtype = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 그 값과 비교한다 */
		} else {	/* [한국어] 아직 아무도 정하지 않았다 */
			oldtype = new->flags & IRQF_TRIGGER_MASK;	/* [한국어] 새 요청자의 값을 기준으로 삼고 */
			irqd_set_trigger_type(&desc->irq_data, oldtype);	/* [한국어] 그것을 정식 값으로 기록한다 */
		}

		if (!((old->flags & new->flags) & IRQF_SHARED) ||	/* [한국어] 양쪽 모두 공유를 허용했는가. 한쪽만으로는 안 된다 */
		    (oldtype != (new->flags & IRQF_TRIGGER_MASK)))	/* [한국어] 트리거 방식이 같은가. 한 선을 레벨과 에지로 동시에 쓸 수 없다 */
			goto mismatch;

		if ((old->flags & IRQF_ONESHOT) &&	/* [한국어] 기존이 oneshot 이고 */
		    (new->flags & IRQF_COND_ONESHOT))	/* [한국어] 새 요청자가 "필요하면 맞추겠다" 고 했는가 */
			new->flags |= IRQF_ONESHOT;	/* [한국어] 맞춰 준다. 유연한 드라이버를 위한 편의다 */
		else if ((old->flags ^ new->flags) & IRQF_ONESHOT)	/* [한국어] oneshot 여부가 다른가 */
			goto mismatch;	/* [한국어] 한쪽은 스레드가 끝날 때까지 막기를 원하고 다른 쪽은 아니다. 양립할 수 없다 */

		/* All handlers must agree on per-cpuness */
		if ((old->flags & IRQF_PERCPU) !=	/* [한국어] (위 영어 주석) per-CPU 여부가 같은가 */
		    (new->flags & IRQF_PERCPU))
			goto mismatch;	/* [한국어] 처리 방식 자체가 다르다 */

		/* add new interrupt at end of irq queue */
		do {
			/*
			 * Or all existing action->thread_mask bits,
			 * so we can find the next zero bit for this
			 * new action.
			 */
			thread_mask |= old->thread_mask;	/* [한국어] (위 영어 주석) 이미 쓰인 oneshot 비트를 모은다. 아래에서 빈 비트를 찾는 데 쓴다 */
			old_ptr = &old->next;	/* [한국어] 목록 끝까지 간다. 새 등록자는 뒤에 붙어 등록 순서대로 불린다 */
			old = *old_ptr;			/* [한국어] 다음 항목으로. NULL 이 되면 목록 끝이다 */
		} while (old);
		shared = 1;	/* [한국어] 공유 등록임을 기록. 아래에서 시작 절차를 건너뛴다 */
	}

	/*
	 * Setup the thread mask for this irqaction for ONESHOT. For
	 * !ONESHOT irqs the thread mask is 0 so we can avoid a
	 * conditional in irq_wake_thread().
	 */
	if (new->flags & IRQF_ONESHOT) {	/* [한국어] (위 영어 주석) oneshot 인터럽트인가. 아니면 마스크가 0 이라 irq_wake_thread 가 조건 없이 OR 할 수 있다 */
		/*
		 * Unlikely to have 32 resp 64 irqs sharing one line,
		 * but who knows.
		 */
		if (thread_mask == ~0UL) {	/* [한국어] (위 영어 주석) 비트를 모두 썼는가 — 한 선을 32 개 또는 64 개가 공유한다는 뜻이다 */
			ret = -EBUSY;	/* [한국어] 더 배정할 비트가 없다. 현실에서 보기 어렵지만 방어한다 */
			goto out_unlock;		/* [한국어] desc->lock 을 잡은 상태의 정리 경로 */
		}
		/*
		 * The thread_mask for the action is or'ed to
		 * desc->thread_active to indicate that the
		 * IRQF_ONESHOT thread handler has been woken, but not
		 * yet finished. The bit is cleared when a thread
		 * completes. When all threads of a shared interrupt
		 * line have completed desc->threads_active becomes
		 * zero and the interrupt line is unmasked. See
		 * handle.c:irq_wake_thread() for further information.
		 *
		 * If no thread is woken by primary (hard irq context)
		 * interrupt handlers, then desc->threads_active is
		 * also checked for zero to unmask the irq line in the
		 * affected hard irq flow handlers
		 * (handle_[fasteoi|level]_irq).
		 *
		 * The new action gets the first zero bit of
		 * thread_mask assigned. See the loop above which or's
		 * all existing action->thread_mask bits.
		 */
		new->thread_mask = 1UL << ffz(thread_mask);	/* [한국어] (위 영어 주석) 빈 첫 비트를 배정한다. ffz 는 "find first zero" — 위 루프가 모은 합집합에서 안 쓰인 자리를 찾는다 */

	} else if (new->handler == irq_default_primary_handler &&	/* [한국어] 스레드로 요청됐는데(1 차 핸들러가 기본값) */
		   !(desc->irq_data.chip->flags & IRQCHIP_ONESHOT_SAFE)) {	/* [한국어] 칩이 본디 oneshot 안전한 것도 아닌가 */
		/*
		 * The interrupt was requested with handler = NULL, so
		 * we use the default primary handler for it. But it
		 * does not have the oneshot flag set. In combination
		 * with level interrupts this is deadly, because the
		 * default primary handler just wakes the thread, then
		 * the irq lines is reenabled, but the device still
		 * has the level irq asserted. Rinse and repeat....
		 *
		 * While this works for edge type interrupts, we play
		 * it safe and reject unconditionally because we can't
		 * say for sure which type this interrupt really
		 * has. The type flags are unreliable as the
		 * underlying chip implementation can override them.
		 */
		pr_err("Threaded irq requested with handler=NULL and !ONESHOT for %s (irq %d)\n",	/* [한국어] (위 영어 주석) 레벨 트리거와 만나면 치명적이다. 기본 핸들러가 스레드만 깨우고 선을 다시 여는데 장치는 여전히 신호를 유지해 무한 반복한다 */
		       new->name, irq);
		ret = -EINVAL;	/* [한국어] 에지라면 괜찮지만 트리거 종류를 확실히 알 수 없어 무조건 거절한다. 칩 구현이 플래그를 덮을 수 있기 때문이다 */
		goto out_unlock;		/* [한국어] 트리거 설정이 실패했다. 같은 정리 경로 */
	}

	if (!shared) {	/* [한국어] 첫 등록자인가. 아래 전체가 선 하나에 대한 일회성 설정이다 */
		/* Setup the type (level, edge polarity) if configured: */
		if (new->flags & IRQF_TRIGGER_MASK) {	/* [한국어] (위 영어 주석) 트리거가 지정됐는가 */
			ret = __irq_set_trigger(desc,	/* [한국어] 하드웨어에 설정한다. 흐름 처리기까지 바뀔 수 있다 */
						new->flags & IRQF_TRIGGER_MASK);

			if (ret)	/* [한국어] 칩이 거절했는가 */
				goto out_unlock;
		}

		/*
		 * Activate the interrupt. That activation must happen
		 * independently of IRQ_NOAUTOEN. request_irq() can fail
		 * and the callers are supposed to handle
		 * that. enable_irq() of an interrupt requested with
		 * IRQ_NOAUTOEN is not supposed to fail. The activation
		 * keeps it in shutdown mode, it merily associates
		 * resources if necessary and if that's not possible it
		 * fails. Interrupts which are in managed shutdown mode
		 * will simply ignore that activation request.
		 */
		ret = irq_activate(desc);	/* [한국어] (위 영어 주석) 자원을 배정받는다. NOAUTOEN 과 무관하게 여기서 해야 하는 이유는, 나중에 enable_irq 가 실패하면 안 되기 때문이다 — 실패는 request_irq 가 감당한다 */
		if (ret)	/* [한국어] 벡터 고갈 등 */
			goto out_unlock;

		desc->istate &= ~(IRQS_AUTODETECT | IRQS_SPURIOUS_DISABLED | \
				  IRQS_ONESHOT | IRQS_WAITING);	/* [한국어] 자동 탐색 흔적과 오탐으로 꺼진 표시를 지운다. 새 드라이버에게 깨끗한 상태를 준다. 줄 잇기 백슬래시 뒤에는 주석을 둘 수 없어 아래 줄에 붙였다 */
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);	/* [한국어] 이전 사용자가 남긴 처리 중 표시도 지운다 */

		if (new->flags & IRQF_PERCPU) {	/* [한국어] per-CPU 인터럽트인가 */
			irqd_set(&desc->irq_data, IRQD_PER_CPU);	/* [한국어] 빠른 경로가 보는 사본 */
			irq_settings_set_per_cpu(desc);	/* [한국어] 설정 워드 쪽 원본 */
			if (new->flags & IRQF_NO_DEBUG)	/* [한국어] 오탐 검출을 끄라고 요청했는가 */
				irq_settings_set_no_debug(desc);	/* [한국어] per-CPU 인터럽트는 통계가 CPU 별이라 오탐 검출이 오판할 수 있다 */
		}

		if (noirqdebug)	/* [한국어] 부팅 옵션으로 오탐 검출 전체를 껐는가 */
			irq_settings_set_no_debug(desc);

		if (new->flags & IRQF_ONESHOT)	/* [한국어] oneshot 인터럽트인가 */
			desc->istate |= IRQS_ONESHOT;	/* [한국어] 흐름 처리기가 이 비트를 보고 스레드가 끝날 때까지 선을 막아 둔다 */

		/* Exclude IRQ from balancing if requested */
		if (new->flags & IRQF_NOBALANCING) {	/* [한국어] (위 영어 주석) 친화도 자동 조정에서 빼 달라는 요청인가 */
			irq_settings_set_no_balancing(desc);	/* [한국어] 설정 워드 */
			irqd_set(&desc->irq_data, IRQD_NO_BALANCING);	/* [한국어] 빠른 경로가 보는 사본 */
		}

		if (!(new->flags & IRQF_NO_AUTOEN) &&	/* [한국어] 자동 시작을 거부하지 않았고 */
		    irq_settings_can_autoenable(desc)) {	/* [한국어] 그 선이 자동 시작을 허용하는가 */
			irq_startup(desc, IRQ_RESEND, IRQ_START_COND);	/* [한국어] 인터럽트를 시작한다. 이 줄 이후로 하드웨어에서 인터럽트가 올라올 수 있다 */
		} else if (!per_cpu_devid) {	/* [한국어] 자동 시작하지 않는 경우. per-CPU 는 원래 각 CPU 에서 따로 켜므로 예외다 */
			/*
			 * Shared interrupts do not go well with disabling
			 * auto enable. The sharing interrupt might request
			 * it while it's still disabled and then wait for
			 * interrupts forever.
			 */
			WARN_ON_ONCE(new->flags & IRQF_SHARED);	/* [한국어] (위 영어 주석) 공유 인터럽트에서 자동 시작을 끄면, 뒤에 붙는 드라이버가 꺼진 선을 물려받아 영영 인터럽트를 기다린다 */
			/* Undo nested disables: */
			desc->depth = 1;	/* [한국어] (위 영어 주석) enable_irq 한 번으로 열리는 상태로 맞춘다 */
		}

	} else if (new->flags & IRQF_TRIGGER_MASK) {	/* [한국어] 공유 등록인데 트리거를 지정했는가 */
		unsigned int nmsk = new->flags & IRQF_TRIGGER_MASK;	/* [한국어] 새 요청자가 원하는 트리거 */
		unsigned int omsk = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 현재 설정된 트리거 */

		if (nmsk != omsk)	/* [한국어] 다른가 */
			/* hope the handler works with current  trigger mode */
			pr_warn("irq %d uses trigger mode %u; requested %u\n",	/* [한국어] (위 영어 주석) 거절하지 않고 경고만 한다. 위 공유 검사가 이미 통과했으므로 치명적이지 않다는 판단이다 */
				irq, omsk, nmsk);
	}

	*old_ptr = new;	/* [한국어] 목록 끝에 매단다. 이 한 줄이 등록의 실질이다 — 이 뒤로 인터럽트가 오면 이 핸들러가 불린다 */

	irq_pm_install_action(desc, new);	/* [한국어] 서스펜드 정책을 계산한다. IRQF_NO_SUSPEND 등을 보고 이 선을 서스펜드 중에도 살릴지 정한다 */

	/* Reset broken irq detection when installing new handler */
	desc->irq_count = 0;	/* [한국어] (위 영어 주석) 오탐 검출 카운터를 초기화한다. 새 드라이버에게 깨끗한 출발점을 준다 */
	desc->irqs_unhandled = 0;	/* [한국어] 미처리 횟수도 함께. 두 값이 짝이 되어 오탐을 판정한다 */

	/*
	 * Check whether we disabled the irq via the spurious handler
	 * before. Reenable it and give it another chance.
	 */
	if (shared && (desc->istate & IRQS_SPURIOUS_DISABLED)) {	/* [한국어] (위 영어 주석) 오탐 검출기가 꺼 버린 선인가 */
		desc->istate &= ~IRQS_SPURIOUS_DISABLED;	/* [한국어] 표시를 지우고 */
		__enable_irq(desc);	/* [한국어] 다시 켠다. 새 드라이버가 그 오탐의 원인을 처리할 수도 있으니 기회를 준다 */
	}

	raw_spin_unlock_irqrestore(&desc->lock, flags);	/* [한국어] 안쪽 락부터 역순으로 푼다 */
	chip_bus_sync_unlock(desc);	/* [한국어] 지연된 레지스터 쓰기가 여기서 하드웨어로 나간다 */
	mutex_unlock(&desc->request_mutex);	/* [한국어] 가장 바깥 락 */

	wake_up_and_wait_for_irq_thread_ready(desc, new);	/* [한국어] 락을 모두 놓은 뒤에 스레드를 시작시킨다. 그 대기가 잠들 수 있기 때문이다 */
	wake_up_and_wait_for_irq_thread_ready(desc, new->secondary);	/* [한국어] 보조 스레드도. NULL 이면 그 안에서 조용히 넘어간다 */

	register_irq_proc(irq, desc);	/* [한국어] /proc/irq/N/ 디렉터리. 여러 번 불려도 안전하다 */
	new->dir = NULL;	/* [한국어] 아래 함수가 채울 자리를 미리 비운다 */
	register_handler_proc(irq, new);	/* [한국어] /proc/irq/N/<이름>/ 항목 */
	return 0;	/* [한국어] 등록 완료 */

mismatch:	/* [한국어] 공유 조건이 맞지 않은 경우 */
	if (!(new->flags & IRQF_PROBE_SHARED)) {	/* [한국어] 자동 탐색이 시험 삼아 요청한 것인가 — 그러면 실패가 정상이라 조용히 넘어간다 */
		pr_err("Flags mismatch irq %d. %08x (%s) vs. %08x (%s)\n",	/* [한국어] 양쪽 플래그와 이름을 함께 찍어 어느 조건이 어긋났는지 알 수 있게 한다 */
		       irq, new->flags, new->name, old->flags, old->name);
#ifdef CONFIG_DEBUG_SHIRQ	/* [한국어] 공유 인터럽트 디버깅 빌드 */
		dump_stack();	/* [한국어] 호출 경로까지 찍는다. 어느 드라이버가 잘못 요청했는지 추적하기 위해서다 */
#endif
	}
	ret = -EBUSY;	/* [한국어] 공유할 수 없다 */

out_unlock:	/* [한국어] desc->lock 을 잡은 뒤 실패한 경로들 */
	raw_spin_unlock_irqrestore(&desc->lock, flags);	/* [한국어] 안쪽 락부터 역순으로 */

	if (!desc->action)	/* [한국어] 등록된 것이 하나도 없는가 — 우리가 첫 등록자였고 실패했다는 뜻이다 */
		irq_release_resources(desc);	/* [한국어] 위에서 잡은 자원을 되돌린다 */
out_bus_unlock:	/* [한국어] 버스 락만 잡은 상태에서 실패한 경로 */
	chip_bus_sync_unlock(desc);	/* [한국어] 중간 락 */
	mutex_unlock(&desc->request_mutex);	/* [한국어] 가장 바깥 락. 이제 다른 request_irq 나 free_irq 가 들어올 수 있다 */

out_thread:	/* [한국어] 스레드를 만든 뒤 실패한 경로 */
	if (new->thread) {	/* [한국어] 주 스레드를 만들었는가 */
		struct task_struct *t = new->thread;	/* [한국어] 지역 변수에 옮긴다 */

		new->thread = NULL;	/* [한국어] 먼저 끊는다. 아래 stop 이 진행되는 동안 다른 코드가 이 포인터를 보지 못하게 한다 */
		kthread_stop_put(t);	/* [한국어] 스레드를 멈추고 참조를 놓는다 */
	}
	if (new->secondary && new->secondary->thread) {	/* [한국어] 보조 스레드도 */
		struct task_struct *t = new->secondary->thread;		/* [한국어] 지역 변수에 옮긴다 */

		new->secondary->thread = NULL;		/* [한국어] 먼저 끊는다. stop 이 진행되는 동안 다른 코드가 이 포인터를 보지 못하게 한다 */
		kthread_stop_put(t);		/* [한국어] 보조 스레드를 멈추고 참조를 놓는다 */
	}
out_mput:	/* [한국어] 모든 실패 경로가 마지막에 지난다 */
	module_put(desc->owner);	/* [한국어] 함수 처음에 잡은 모듈 참조를 놓는다 */
	return ret;	/* [한국어] 실패 원인 */
}

/*
 * Internal function to unregister an irqaction - used to free
 * regular and special interrupts that are part of the architecture.
 */
/*
 * [한국어]
 * __free_irq - 인터럽트 등록을 해제한다
 *
 * @desc:   대상 서술자
 * @dev_id: 어느 등록자를 해제할지
 * @return: 해제된 action, 못 찾으면 NULL
 *
 * __setup_irq() 의 반대인데, 락을 다루는 방식이 더 까다롭다. 순서가
 * 이렇다.
 *
 *   1. 세 락을 모두 잡고 목록에서 뗀다
 *   2. 마지막 등록자면 선을 정지한다
 *   3. desc->lock 과 버스 락을 놓는다 (request_mutex 는 유지)
 *   4. 진행 중인 처리가 끝나기를 기다린다
 *   5. 스레드를 멈춘다
 *   6. 마지막 등록자면 자원을 반납한다
 *
 * 3 에서 버스 락을 놓는 이유가 원본 주석에 자세하다. 두 가지다.
 * 하나는 위에서 한 칩 조작이 느린 버스로 실제로 나가야 하기
 * 때문이고, 다른 하나는 데드락 회피다 — 스레드 핸들러가
 * irq_finalize_oneshot 에서 버스 락을 잡는데, 그 락을 쥔 채
 * kthread_stop 을 하면 스레드가 락을 기다리고 이쪽은 스레드를
 * 기다려 서로 멈춘다.
 *
 * request_mutex 를 유지하는 것이 그 사이의 안전을 보장한다. 동시에
 * request_irq 가 들어와 자원을 다시 잡거나 oneshot 비트를 재사용하는
 * 것을 막는다.
 *
 * DEBUG_SHIRQ 블록은 공유 인터럽트 드라이버를 시험한다. 원본 주석대로,
 * 해제 중에도 인터럽트가 올 수 있으므로 드라이버가 그것을 견뎌야
 * 한다. 일부러 한 번 더 불러 본다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 인터럽트 문맥에서 부르면 경고한다.
 *
 * 호출 체인:
 *   free_irq() → [이 함수]
 */
static struct irqaction *__free_irq(struct irq_desc *desc, void *dev_id)
{
	unsigned irq = desc->irq_data.irq;	/* [한국어] 로그와 /proc 정리에 쓸 번호 */
	struct irqaction *action, **action_ptr;	/* [한국어] 찾은 등록자와 그것을 가리키는 포인터의 주소 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	WARN(in_interrupt(), "Trying to free IRQ %d from IRQ context!\n", irq);	/* [한국어] 이 함수는 여러 곳에서 잠든다. 인터럽트 문맥에서 부르면 곧 문제가 된다 */

	mutex_lock(&desc->request_mutex);	/* [한국어] 가장 바깥 락. 아래에서 다른 두 락을 놓는 구간을 이것이 지킨다 */
	chip_bus_lock(desc);	/* [한국어] 중간 락 */
	raw_spin_lock_irqsave(&desc->lock, flags);	/* [한국어] 가장 안쪽 락 */

	/*
	 * There can be multiple actions per IRQ descriptor, find the right
	 * one based on the dev_id:
	 */
	action_ptr = &desc->action;	/* [한국어] (위 영어 주석) 목록 머리의 주소. 포인터의 주소를 쓰면 중간 항목을 뺄 때 앞 항목을 따로 기억하지 않아도 된다 */
	for (;;) {	/* [한국어] 목록을 훑어 dev_id 가 맞는 등록자를 찾는다. 못 찾으면 안에서 반환한다 */
		action = *action_ptr;	/* [한국어] 현재 항목 */

		if (!action) {	/* [한국어] 목록 끝까지 못 찾았는가 */
			WARN(1, "Trying to free already-free IRQ %d\n", irq);	/* [한국어] 두 번 해제하거나 엉뚱한 dev_id 를 넘긴 드라이버 버그다 */
			raw_spin_unlock_irqrestore(&desc->lock, flags);	/* [한국어] 잡은 락을 역순으로 푼다 */
			chip_bus_sync_unlock(desc);			/* [한국어] 중간 락 */
			mutex_unlock(&desc->request_mutex);			/* [한국어] 가장 바깥 락 */
			return NULL;	/* [한국어] 호출자가 NULL 검사로 처리한다 */
		}

		if (action->dev_id == dev_id)	/* [한국어] 찾는 등록자인가 */
			break;
		action_ptr = &action->next;	/* [한국어] 다음 항목의 포인터 주소로 */
	}

	/* Found it - now remove it from the list of entries: */
	*action_ptr = action->next;	/* [한국어] (위 영어 주석) 목록에서 뗀다. 포인터의 주소를 들고 있어 앞 항목이 무엇이든 이 한 줄로 끝난다 */

	irq_pm_remove_action(desc, action);	/* [한국어] 서스펜드 정책을 다시 계산한다. 남은 등록자들의 플래그로 결정된다 */

	/* If this was the last handler, shut down the IRQ line: */
	if (!desc->action) {	/* [한국어] (위 영어 주석) 마지막 등록자였는가 */
		irq_settings_clr_disable_unlazy(desc);	/* [한국어] 게으른 비활성 금지 설정을 지운다. 그것은 등록자의 요구였고 이제 그 등록자가 없다 */
		/* Only shutdown. Deactivate after synchronize_hardirq() */
		irq_shutdown(desc);	/* [한국어] (위 영어 주석) 정지만 한다. 비활성화는 진행 중인 처리가 끝난 뒤에 해야 자원이 회수된 뒤 인터럽트가 오는 일이 없다 */
	}

#ifdef CONFIG_SMP	/* [한국어] 친화도 힌트는 SMP 에만 있다 */
	/* make sure affinity_hint is cleaned up */
	if (WARN_ON_ONCE(desc->affinity_hint))	/* [한국어] (위 영어 주석) 드라이버가 힌트를 지우지 않고 해제했는가 */
		desc->affinity_hint = NULL;	/* [한국어] 그 포인터가 드라이버 메모리를 가리키므로 남겨 두면 해제된 것을 참조한다 */
#endif

	raw_spin_unlock_irqrestore(&desc->lock, flags);	/* [한국어] 안쪽 락을 푼다. 아래 대기가 오래 걸릴 수 있다 */
	/*
	 * Drop bus_lock here so the changes which were done in the chip
	 * callbacks above are synced out to the irq chips which hang
	 * behind a slow bus (I2C, SPI) before calling synchronize_hardirq().
	 *
	 * Aside of that the bus_lock can also be taken from the threaded
	 * handler in irq_finalize_oneshot() which results in a deadlock
	 * because kthread_stop() would wait forever for the thread to
	 * complete, which is blocked on the bus lock.
	 *
	 * The still held desc->request_mutex() protects against a
	 * concurrent request_irq() of this irq so the release of resources
	 * and timing data is properly serialized.
	 */
	chip_bus_sync_unlock(desc);	/* [한국어] (위 영어 주석) 두 이유로 놓는다. 위 칩 조작이 느린 버스로 나가야 하고, 스레드가 이 락을 잡으므로 쥔 채 kthread_stop 하면 데드락이다 */

	unregister_handler_proc(irq, action);	/* [한국어] /proc 항목을 지운다. 그 안에서 진행 중인 읽기를 기다린다 */

	/*
	 * Make sure it's not being used on another CPU and if the chip
	 * supports it also make sure that there is no (not yet serviced)
	 * interrupt in flight at the hardware level.
	 */
	__synchronize_irq(desc);	/* [한국어] (위 영어 주석) 진행 중인 처리가 모두 끝나기를 기다린다. 이 뒤로 핸들러가 만지는 자료구조를 안전하게 해제할 수 있다 */

#ifdef CONFIG_DEBUG_SHIRQ	/* [한국어] 공유 인터럽트 드라이버를 시험하는 빌드 */
	/*
	 * It's a shared IRQ -- the driver ought to be prepared for an IRQ
	 * event to happen even now it's being freed, so let's make sure that
	 * is so by doing an extra call to the handler ....
	 *
	 * ( We do this after actually deregistering it, to make sure that a
	 *   'real' IRQ doesn't run in parallel with our fake. )
	 */
	if (action->flags & IRQF_SHARED) {	/* [한국어] (위 영어 주석) 공유 인터럽트인가 */
		local_irq_save(flags);	/* [한국어] 진짜 인터럽트와 겹치지 않게 한다 */
		action->handler(irq, dev_id);	/* [한국어] 일부러 한 번 더 부른다. 해제 중에도 인터럽트가 올 수 있으므로 드라이버가 그것을 견뎌야 한다 */
		local_irq_restore(flags);		/* [한국어] 원래 인터럽트 상태로 되돌린다 */
	}
#endif

	/*
	 * The action has already been removed above, but the thread writes
	 * its oneshot mask bit when it completes. Though request_mutex is
	 * held across this which prevents __setup_irq() from handing out
	 * the same bit to a newly requested action.
	 */
	if (action->thread) {	/* [한국어] (위 영어 주석) 스레드가 있는 등록자인가 */
		kthread_stop_put(action->thread);	/* [한국어] 스레드를 멈추고 참조를 놓는다. 그 스레드가 끝나면서 oneshot 비트를 쓰는데, request_mutex 가 그 비트의 재사용을 막고 있다 */
		if (action->secondary && action->secondary->thread)	/* [한국어] 보조 스레드도 */
			kthread_stop_put(action->secondary->thread);
	}

	/* Last action releases resources */
	if (!desc->action) {	/* [한국어] (위 영어 주석) 마지막 등록자였는가 */
		/*
		 * Reacquire bus lock as irq_release_resources() might
		 * require it to deallocate resources over the slow bus.
		 */
		chip_bus_lock(desc);	/* [한국어] (위 영어 주석) 자원 반납이 느린 버스를 쓸 수 있어 다시 잡는다 */
		/*
		 * There is no interrupt on the fly anymore. Deactivate it
		 * completely.
		 */
		scoped_guard(raw_spinlock_irqsave, &desc->lock)	/* [한국어] 짧게 잡는다 */
			irq_domain_deactivate_irq(&desc->irq_data);	/* [한국어] (위 영어 주석) 이제야 자원을 회수한다. 진행 중인 인터럽트가 없음이 위에서 보장됐다 */

		irq_release_resources(desc);	/* [한국어] 클록·전원 등 칩이 잡았던 것을 반납한다 */
		chip_bus_sync_unlock(desc);	/* [한국어] 지연된 조작을 하드웨어로 내보낸다 */
	}

	mutex_unlock(&desc->request_mutex);	/* [한국어] 가장 바깥 락. 이제 다른 request_irq 가 들어와도 된다 */

	irq_chip_pm_put(&desc->irq_data);	/* [한국어] 컨트롤러의 전원 참조를 놓는다. request 때 잡은 것과 짝이다 */
	module_put(desc->owner);	/* [한국어] 소유 모듈 참조도 */
	kfree(action->secondary);	/* [한국어] 강제 스레드화로 만든 보조 action. 주 action 은 호출자가 해제한다 */
	return action;	/* [한국어] 호출자가 이름을 꺼내 쓰고 해제한다 */
}

/**
 * free_irq - free an interrupt allocated with request_irq
 * @irq:	Interrupt line to free
 * @dev_id:	Device identity to free
 *
 * Remove an interrupt handler. The handler is removed and if the interrupt
 * line is no longer in use by any driver it is disabled.  On a shared IRQ
 * the caller must ensure the interrupt is disabled on the card it drives
 * before calling this function. The function does not return until any
 * executing interrupts for this IRQ have completed.
 *
 * This function must not be called from interrupt context.
 *
 * Returns the devname argument passed to request_irq.
 */
/*
 * [한국어]
 * free_irq - request_irq 로 등록한 인터럽트를 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 해제할 등록자의 식별자
 * @return: request_irq 에 넘겼던 이름, 실패 시 NULL
 *
 * 드라이버가 쓰는 표준 해제 API 다.
 *
 * 원본 주석의 조건이 중요하다. 공유 인터럽트라면 호출자가 먼저
 * 장치 쪽에서 인터럽트를 꺼야 한다 — 코어는 선을 끄지 않기 때문이다
 * (다른 등록자가 아직 쓰고 있다). 그러지 않으면 해제 뒤에도 그 장치가
 * 인터럽트를 보내고, 아무도 그것을 처리하지 못해 오탐이 된다.
 *
 * 이름을 돌려주는 것이 편의다. 드라이버가 그 문자열을 힙에
 * 할당했다면 여기서 받아 해제할 수 있다.
 *
 * affinity_notify 검사가 안전장치다. 알림이 남아 있으면 그 워크큐
 * 항목이 해제된 서술자를 참조할 수 있다. 경고하고 지운다.
 *
 * per-CPU 인터럽트를 거절하는 이유: free_percpu_irq 를 써야 한다.
 * 자료구조 해석이 달라 잘못 해제하면 조용히 망가진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 인터럽트 문맥에서 부르면 안 된다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __free_irq()
 */
const void *free_irq(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irqaction *action;	/* [한국어] 해제된 등록자 */
	const char *devname;	/* [한국어] 돌려줄 이름 */

	if (!desc || WARN_ON(irq_settings_is_per_cpu_devid(desc)))	/* [한국어] per-CPU 는 free_percpu_irq 를 써야 한다. 자료구조 해석이 달라 잘못 해제하면 조용히 망가진다 */
		return NULL;

#ifdef CONFIG_SMP	/* [한국어] 알림 기구는 SMP 에만 있다 */
	if (WARN_ON(desc->affinity_notify))	/* [한국어] 드라이버가 알림을 해제하지 않았는가 */
		desc->affinity_notify = NULL;	/* [한국어] 남겨 두면 그 워크큐 항목이 해제된 서술자를 참조할 수 있다 */
#endif

	action = __free_irq(desc, dev_id);	/* [한국어] 실제 해제 절차 */

	if (!action)	/* [한국어] 못 찾았는가 */
		return NULL;

	devname = action->name;	/* [한국어] 해제 전에 이름을 챙긴다 */
	kfree(action);	/* [한국어] action 구조체. 보조 action 은 __free_irq 가 이미 해제했다 */
	return devname;	/* [한국어] 드라이버가 그 문자열을 힙에 할당했다면 여기서 받아 해제할 수 있다 */
}
EXPORT_SYMBOL(free_irq);	/* [한국어] 거의 모든 드라이버가 부른다 */

/* This function must be called with desc->lock held */
/*
 * [한국어]
 * __cleanup_nmi - NMI 등록을 정리한다
 *
 * @irq:  대상 인터럽트 번호
 * @desc: 대상 서술자
 * @return: 등록 때의 이름, action 이 없으면 NULL
 *
 * NMI 해제의 공통 부분이다. free_nmi() 와 request_nmi() 의 실패
 * 경로가 공유한다.
 *
 * 일반 free_irq 보다 훨씬 짧은 이유: NMI 는 공유할 수 없어 등록자가
 * 하나뿐이고, 스레드도 쓸 수 없다. 목록을 순회하거나 스레드를 멈출
 * 필요가 없다.
 *
 * synchronize_irq 를 부르지 않는 것도 눈에 띈다. NMI 는 마스크할 수
 * 없어 "진행 중인 처리를 기다린다" 는 개념이 성립하기 어렵고,
 * 그 대기 자체가 NMI 문맥과 얽히면 위험하다.
 *
 * IRQS_NMI 를 먼저 지우는 것에 주목: 이 뒤로 이 선은 보통 인터럽트로
 * 취급된다. 정리 도중에 NMI 로 오해되면 안 된다.
 *
 * 원본 주석의 조건: desc->lock 을 쥔 채 불러야 한다. 그 안에서
 * irq_shutdown_and_deactivate 등 락이 필요한 일을 한다.
 *
 * 실행 컨텍스트: desc->lock 보유.
 *
 * 호출 체인:
 *   free_nmi() / request_nmi() 의 실패 경로 → [이 함수]
 */
static const void *__cleanup_nmi(unsigned int irq, struct irq_desc *desc)
{
	const char *devname = NULL;	/* [한국어] 돌려줄 이름 */

	desc->istate &= ~IRQS_NMI;	/* [한국어] 먼저 지운다. 정리 도중에 이 선이 NMI 로 오해되면 안 된다 */

	if (!WARN_ON(desc->action == NULL)) {	/* [한국어] 등록자가 있는가. NMI 는 공유할 수 없어 하나뿐이다 */
		irq_pm_remove_action(desc, desc->action);	/* [한국어] 서스펜드 정책 재계산 */
		devname = desc->action->name;	/* [한국어] 해제 전에 이름을 챙긴다 */
		unregister_handler_proc(irq, desc->action);	/* [한국어] /proc 항목 제거 */

		kfree(desc->action);	/* [한국어] 여기서 해제한다. free_irq 와 달리 호출자에게 넘기지 않는다 */
		desc->action = NULL;	/* [한국어] 목록을 비운다. 하나뿐이라 순회가 필요 없다 */
	}

	irq_settings_clr_disable_unlazy(desc);	/* [한국어] 게으른 비활성 금지 설정을 지운다 */
	irq_shutdown_and_deactivate(desc);	/* [한국어] 정지와 자원 반납을 한 번에. synchronize_irq 를 부르지 않는 것은 NMI 가 마스크 불가라 그 대기가 성립하지 않아서다 */

	irq_release_resources(desc);	/* [한국어] 칩이 잡았던 자원 */

	irq_chip_pm_put(&desc->irq_data);	/* [한국어] 전원 참조 */
	module_put(desc->owner);	/* [한국어] 소유 모듈 참조 */

	return devname;	/* [한국어] 호출자가 그대로 돌려준다 */
}

/*
 * [한국어]
 * free_nmi - NMI 등록을 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 쓰지 않는다 (NMI 는 등록자가 하나뿐이다)
 * @return: 등록 때의 이름, 실패 시 NULL
 *
 * request_nmi() 의 반대다. 검사가 세 겹인 것이 특징이다.
 *
 * NMI 가 맞는가, per-CPU 가 아닌가, 그리고 이미 꺼져 있는가.
 *
 * 마지막 검사가 흥미롭다. NMI 를 끄지 않고 해제하려 하면 경고하고
 * 대신 꺼 준다. 원본 코드가 그것을 그냥 두지 않는 이유: NMI 는
 * 마스크할 수 없으므로, 켜진 채로 자료구조를 해제하면 그 사이에
 * 오는 NMI 가 해제된 메모리를 만진다.
 *
 * dev_id 를 쓰지 않는 것에 주목: NMI 는 공유할 수 없어 등록자를
 * 구분할 필요가 없다. 인터페이스 대칭을 위해 인자만 받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   NMI 를 쓰는 드라이버 → [이 함수] → __cleanup_nmi()
 */
const void *free_nmi(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || WARN_ON(!irq_is_nmi(desc)))	/* [한국어] NMI 로 등록된 선인가 */
		return NULL;

	if (WARN_ON(irq_settings_is_per_cpu_devid(desc)))	/* [한국어] per-CPU NMI 는 free_percpu_nmi 를 써야 한다 */
		return NULL;

	/* NMI still enabled */
	if (WARN_ON(desc->depth == 0))	/* [한국어] (위 영어 주석) 아직 켜져 있는가 */
		disable_nmi_nosync(irq);	/* [한국어] 대신 꺼 준다. NMI 는 마스크할 수 없어 켜진 채 해제하면 그 사이의 NMI 가 해제된 메모리를 만진다 */

	guard(raw_spinlock_irqsave)(&desc->lock);	/* [한국어] 아래 두 함수가 락을 요구한다 */
	irq_nmi_teardown(desc);	/* [한국어] 하드웨어의 NMI 설정을 되돌린다 */
	return __cleanup_nmi(irq, desc);	/* [한국어] 나머지 정리. dev_id 를 쓰지 않는 것은 NMI 가 공유되지 않아 등록자를 구분할 필요가 없어서다 */
}

/**
 * request_threaded_irq - allocate an interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Primary handler for threaded interrupts.
 *		If handler is NULL and thread_fn != NULL
 *		the default primary handler is installed.
 * @thread_fn:	Function called from the irq handler thread
 *		If NULL, no irq thread is created
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. From the point this call is made your handler function
 * may be invoked. Since your handler function must clear any interrupt the
 * board raises, you must take care both to initialise your hardware and to
 * set up the interrupt handler in the right order.
 *
 * If you want to set up a threaded irq handler for your device then you
 * need to supply @handler and @thread_fn. @handler is still called in hard
 * interrupt context and has to check whether the interrupt originates from
 * the device. If yes it needs to disable the interrupt on the device and
 * return IRQ_WAKE_THREAD which will wake up the handler thread and run
 * @thread_fn. This split handler design is necessary to support shared
 * interrupts.
 *
 * @dev_id must be globally unique. Normally the address of the device data
 * structure is used as the cookie. Since the handler receives this value
 * it makes sense to use it.
 *
 * If your interrupt is shared you must pass a non NULL dev_id as this is
 * required when freeing the interrupt.
 *
 * Flags:
 *
 *	IRQF_SHARED		Interrupt is shared
 *	IRQF_TRIGGER_*		Specify active edge(s) or level
 *	IRQF_ONESHOT		Run thread_fn with interrupt line masked
 */
/*
 * [한국어]
 * request_threaded_irq - 인터럽트를 등록한다 (스레드 핸들러 포함)
 *
 * @irq:       대상 인터럽트 번호
 * @handler:   하드 인터럽트 문맥에서 불릴 1 차 핸들러 (NULL 가능)
 * @thread_fn: 스레드 문맥에서 불릴 핸들러 (NULL 가능)
 * @irqflags:  IRQF_ 플래그 조합
 * @devname:   /proc 에 표시될 장치 이름
 * @dev_id:    핸들러에 넘겨질 식별자
 * @return:    0 성공, 음수 오류
 *
 * 드라이버가 인터럽트를 얻는 주 통로다. request_irq() 도 이것을
 * thread_fn 없이 부르는 매크로다.
 *
 * 원본 주석이 두 핸들러로 나눈 설계를 설명한다. 공유 인터럽트를
 * 지원하려면 1 차 핸들러가 하드 인터럽트 문맥에서 "이 인터럽트가
 * 내 장치의 것인가" 를 판별해야 한다. 그 판별을 스레드로 미루면,
 * 여러 드라이버의 스레드가 모두 깨어나 각자 확인하게 되어 비효율적이고
 * 선을 언제 열지도 정하기 어렵다.
 *
 * 네 조건 검사가 각각 다른 실수를 잡는다.
 *
 *   - 공유인데 dev_id 가 NULL: 해제할 때 어느 등록자인지 구분할 수 없다.
 *   - 공유인데 자동 시작 거부: 뒤에 붙는 드라이버가 꺼진 선을
 *     물려받아 영영 인터럽트를 기다린다.
 *   - 공유가 아닌데 COND_SUSPEND: 그 플래그는 공유 인터럽트에서
 *     "다른 등록자가 서스펜드를 허용하면 나도" 라는 뜻이라 단독
 *     등록에는 의미가 없다.
 *   - NO_SUSPEND 와 COND_SUSPEND 를 함께: 서로 모순이다.
 *
 * IRQ_NOTCONNECTED 처리가 흥미롭다. 디바이스 트리에서 "이 장치는
 * 인터럽트를 쓰지 않는다" 를 표현하는 특수 값이다. 드라이버가
 * 조건 없이 request_irq 를 부를 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 / request_irq() 매크로 → [이 함수] → __setup_irq()
 */
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
			 irq_handler_t thread_fn, unsigned long irqflags,
			 const char *devname, void *dev_id)
{
	struct irqaction *action;	/* [한국어] 만들 등록 정보 */
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	int retval;	/* [한국어] 결과 */

	if (irq == IRQ_NOTCONNECTED)	/* [한국어] 디바이스 트리가 "인터럽트를 쓰지 않는다" 고 표현한 값인가 */
		return -ENOTCONN;	/* [한국어] 드라이버가 조건 없이 부를 수 있게 하려는 특수 처리다 */

	/*
	 * Sanity-check: shared interrupts must pass in a real dev-ID,
	 * otherwise we'll have trouble later trying to figure out
	 * which interrupt is which (messes up the interrupt freeing
	 * logic etc).
	 *
	 * Also shared interrupts do not go well with disabling auto enable.
	 * The sharing interrupt might request it while it's still disabled
	 * and then wait for interrupts forever.
	 *
	 * Also IRQF_COND_SUSPEND only makes sense for shared interrupts and
	 * it cannot be set along with IRQF_NO_SUSPEND.
	 */
	if (((irqflags & IRQF_SHARED) && !dev_id) ||	/* [한국어] (위 영어 주석) 공유인데 식별자가 없는가 — 해제할 때 어느 등록자인지 구분할 수 없다 */
	    ((irqflags & IRQF_SHARED) && (irqflags & IRQF_NO_AUTOEN)) ||	/* [한국어] 공유인데 자동 시작을 거부하는가 — 뒤에 붙는 드라이버가 꺼진 선을 물려받아 영영 기다린다 */
	    (!(irqflags & IRQF_SHARED) && (irqflags & IRQF_COND_SUSPEND)) ||	/* [한국어] 단독인데 조건부 서스펜드인가 — 그 플래그는 다른 등록자와의 협의를 뜻해 단독에는 의미가 없다 */
	    ((irqflags & IRQF_NO_SUSPEND) && (irqflags & IRQF_COND_SUSPEND)))	/* [한국어] 서스펜드 거부와 조건부 서스펜드를 함께 — 모순이다 */
		return -EINVAL;

	desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return -EINVAL;

	if (!irq_settings_can_request(desc) ||	/* [한국어] 요청이 허용된 선인가. 체인 처리기가 걸린 선은 아니다 */
	    WARN_ON(irq_settings_is_per_cpu_devid(desc)))	/* [한국어] per-CPU 는 request_percpu_irq 를 써야 한다 */
		return -EINVAL;

	if (!handler) {	/* [한국어] 1 차 핸들러를 주지 않았는가 */
		if (!thread_fn)	/* [한국어] 스레드 핸들러도 없는가 */
			return -EINVAL;	/* [한국어] 처리할 함수가 하나도 없다 */
		handler = irq_default_primary_handler;	/* [한국어] 스레드를 깨우기만 하는 기본 핸들러. __setup_irq 가 oneshot 조합을 강제한다 */
	}

	action = kzalloc_obj(struct irqaction);	/* [한국어] 0 초기화. thread_mask, secondary 등이 0 으로 시작하는 것이 유효한 상태다 */
	if (!action)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	action->handler = handler;	/* [한국어] 1 차 핸들러 */
	action->thread_fn = thread_fn;	/* [한국어] 스레드 핸들러. NULL 이면 스레드를 만들지 않는다 */
	action->flags = irqflags;	/* [한국어] IRQF_ 플래그. __setup_irq 가 공유 조건 판별에 쓴다 */
	action->name = devname;	/* [한국어] /proc/interrupts 에 표시된다 */
	action->dev_id = dev_id;	/* [한국어] 핸들러에 넘겨지고 해제할 때 등록자를 식별한다 */

	retval = irq_chip_pm_get(&desc->irq_data);	/* [한국어] 컨트롤러의 전원을 잡는다. 인터럽트를 쓰는 동안 절전에 들어가면 안 된다 */
	if (retval < 0) {	/* [한국어] 전원을 켜지 못했는가 */
		kfree(action);	/* [한국어] 방금 잡은 것을 되돌린다 */
		return retval;		/* [한국어] 전원을 켜지 못했다. action 은 위에서 해제했다 */
	}

	retval = __setup_irq(irq, desc, action);	/* [한국어] 실제 등록. 이 파일에서 가장 긴 함수다 */

	if (retval) {	/* [한국어] 실패 */
		irq_chip_pm_put(&desc->irq_data);	/* [한국어] 전원 참조를 놓는다 */
		kfree(action->secondary);	/* [한국어] 강제 스레드화가 만들었을 수 있다. NULL 이어도 안전하다 */
		kfree(action);		/* [한국어] action 본체. 위에서 보조 action 도 해제했다 */
	}

#ifdef CONFIG_DEBUG_SHIRQ_FIXME	/* [한국어] 이름에 FIXME 가 붙어 사실상 꺼져 있는 코드다. 그 설정이 존재하지 않아 컴파일되지 않는다 */
	if (!retval && (irqflags & IRQF_SHARED)) {	/* [한국어] 공유 인터럽트를 성공적으로 등록했는가 */
		/*
		 * It's a shared IRQ -- the driver ought to be prepared for it
		 * to happen immediately, so let's make sure....
		 * We disable the irq to make sure that a 'real' IRQ doesn't
		 * run in parallel with our fake.
		 */
		unsigned long flags;	/* [한국어] 인터럽트 상태 */

		disable_irq(irq);	/* [한국어] (위 영어 주석) 진짜 인터럽트와 겹치지 않게 한다 */
		local_irq_save(flags);	/* [한국어] 더 확실히 */

		handler(irq, dev_id);	/* [한국어] 일부러 한 번 불러 본다. 등록 직후 인터럽트가 올 수 있으므로 드라이버가 그것을 견뎌야 한다 */

		local_irq_restore(flags);		/* [한국어] 인터럽트 상태 복원 */
		enable_irq(irq);		/* [한국어] 위에서 끈 것을 되돌린다 */
	}
#endif
	return retval;	/* [한국어] 0 이면 이 시점부터 핸들러가 불릴 수 있다 */
}
EXPORT_SYMBOL(request_threaded_irq);	/* [한국어] 거의 모든 드라이버가 직접 또는 매크로로 부른다 */

/**
 * request_any_context_irq - allocate an interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Threaded handler for threaded interrupts.
 * @flags:	Interrupt type flags
 * @name:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. It selects either a hardirq or threaded handling
 * method depending on the context.
 *
 * Returns: On failure, it returns a negative value. On success, it returns either
 * IRQC_IS_HARDIRQ or IRQC_IS_NESTED.
 */
/*
 * [한국어]
 * request_any_context_irq - 선에 맞는 방식으로 알아서 등록한다
 *
 * @irq:     대상 인터럽트 번호
 * @handler: 핸들러 (문맥은 선에 따라 정해진다)
 * @flags:   IRQF_ 플래그
 * @name:    장치 이름
 * @dev_id:  식별자
 * @return:  IRQC_IS_HARDIRQ 또는 IRQC_IS_NESTED, 실패 시 음수
 *
 * 같은 드라이버가 여러 플랫폼에서 쓰일 때를 위한 함수다. 어떤
 * 플랫폼에서는 그 인터럽트가 직접 배선이고, 다른 플랫폼에서는
 * GPIO 확장 칩 뒤에 있어 중첩 스레드로 처리해야 한다.
 *
 * 드라이버가 그 차이를 알 필요 없게, 이 함수가 선을 보고 정한다.
 * 중첩이면 핸들러를 thread_fn 자리에 넣고, 아니면 handler 자리에 넣는다.
 *
 * 반환값으로 어느 쪽이 선택됐는지 알려 준다. 드라이버가 그것을 보고
 * 자기 코드의 잠금 방식을 조정할 수 있다 — 스레드 문맥이면 뮤텍스를
 * 써도 되고, 하드 인터럽트 문맥이면 스핀락만 써야 한다.
 *
 * !ret ? A : ret 관용구에 주목: 성공(0)이면 상수를, 실패면 오류
 * 코드를 그대로 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   여러 플랫폼을 지원하는 드라이버 → [이 함수] →
 *   request_threaded_irq() 또는 request_irq()
 */
int request_any_context_irq(unsigned int irq, irq_handler_t handler,
			    unsigned long flags, const char *name, void *dev_id)
{
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	int ret;	/* [한국어] 등록 결과 */

	if (irq == IRQ_NOTCONNECTED)	/* [한국어] 인터럽트를 쓰지 않는 장치인가 */
		return -ENOTCONN;

	desc = irq_to_desc(irq);	/* [한국어] 선의 성질을 보기 위해 */
	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return -EINVAL;

	if (irq_settings_is_nested_thread(desc)) {	/* [한국어] 부모 인터럽트의 스레드 안에서 처리되는 선인가 — GPIO 확장 칩 뒤 등 */
		ret = request_threaded_irq(irq, NULL, handler,	/* [한국어] 핸들러를 thread_fn 자리에 넣는다. 중첩 인터럽트는 스레드 문맥에서만 처리된다 */
					   flags, name, dev_id);
		return !ret ? IRQC_IS_NESTED : ret;	/* [한국어] 드라이버가 이 값을 보고 자기 잠금 방식을 조정할 수 있다 — 스레드 문맥이면 뮤텍스도 쓸 수 있다 */
	}

	ret = request_irq(irq, handler, flags, name, dev_id);	/* [한국어] 직접 배선이면 하드 인터럽트 문맥에서 처리한다 */
	return !ret ? IRQC_IS_HARDIRQ : ret;	/* [한국어] 그 경우 드라이버는 스핀락만 써야 한다 */
}
EXPORT_SYMBOL_GPL(request_any_context_irq);	/* [한국어] 여러 플랫폼을 지원하는 드라이버가 부른다 */

/**
 * request_nmi - allocate an interrupt line for NMI delivery
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Threaded handler for threaded interrupts.
 * @irqflags:	Interrupt type flags
 * @name:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. It sets up the IRQ line to be handled as an NMI.
 *
 * An interrupt line delivering NMIs cannot be shared and IRQ handling
 * cannot be threaded.
 *
 * Interrupt lines requested for NMI delivering must produce per cpu
 * interrupts and have auto enabling setting disabled.
 *
 * @dev_id must be globally unique. Normally the address of the device data
 * structure is used as the cookie. Since the handler receives this value
 * it makes sense to use it.
 *
 * If the interrupt line cannot be used to deliver NMIs, function will fail
 * and return a negative value.
 */
/*
 * [한국어]
 * request_nmi - 인터럽트를 NMI 로 등록한다
 *
 * @irq:      대상 인터럽트 번호
 * @handler:  NMI 문맥에서 불릴 핸들러
 * @irqflags: IRQF_ 플래그 (IRQF_PERCPU 필수)
 * @name:     장치 이름
 * @dev_id:   식별자
 * @return:   0 성공, 음수 오류
 *
 * NMI 는 마스크할 수 없는 인터럽트다. 워치독처럼 시스템이 멈춰도
 * 살아 있어야 하는 기능이 쓴다.
 *
 * 그 대가로 제약이 많다. 공유할 수 없고, 스레드로 처리할 수 없고,
 * 폴링 대상이 될 수 없고, per-CPU 여야 하고, 자동 시작이 꺼져 있어야
 * 한다.
 *
 * per-CPU 를 요구하는 이유: NMI 핸들러는 락을 쓸 수 없다. per-CPU
 * 인터럽트는 CPU 마다 별개라 직렬화가 필요 없어, 그 제약과 맞는다.
 *
 * 자동 시작을 금지하는 이유: NMI 설정은 request 뒤에 별도로 해야
 * 한다. 그 전에 인터럽트가 켜지면 아직 NMI 가 아닌 상태로 처리된다.
 *
 * NO_THREAD 와 NOBALANCING 을 강제로 더하는 것에 주목: 드라이버가
 * 실수로 빠뜨려도 스레드화나 친화도 조정이 일어나지 않게 한다.
 *
 * scoped_guard 블록 안에서 return 하는 구조가 특이하다. 그 안의 두
 * return 이 모두 정상 종료 경로이고, 아래 err 레이블들은 그 블록에
 * 도달하기 전의 실패만 처리한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   워치독 / 프로파일러 드라이버 → [이 함수] → __setup_irq()
 */
int request_nmi(unsigned int irq, irq_handler_t handler,
		unsigned long irqflags, const char *name, void *dev_id)
{
	struct irqaction *action;	/* [한국어] 만들 등록 정보 */
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	int retval;	/* [한국어] 결과 */

	if (irq == IRQ_NOTCONNECTED)	/* [한국어] 인터럽트를 쓰지 않는 장치인가 */
		return -ENOTCONN;

	/* NMI cannot be shared, used for Polling */
	if (irqflags & (IRQF_SHARED | IRQF_COND_SUSPEND | IRQF_IRQPOLL))	/* [한국어] (위 영어 주석) 공유는 여러 핸들러 순회가 NMI 문맥에서 위험하고, 폴링은 NMI 를 일반 인터럽트처럼 다루는 기구라 맞지 않는다 */
		return -EINVAL;

	if (!(irqflags & IRQF_PERCPU))	/* [한국어] per-CPU 인터럽트인가 */
		return -EINVAL;	/* [한국어] NMI 핸들러는 락을 쓸 수 없다. per-CPU 는 직렬화가 필요 없어 그 제약과 맞는다 */

	if (!handler)	/* [한국어] 핸들러가 없는가 */
		return -EINVAL;	/* [한국어] NMI 는 스레드를 쓸 수 없어 1 차 핸들러가 필수다 */

	desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || (irq_settings_can_autoenable(desc) &&	/* [한국어] 자동 시작이 허용된 선인데 */
	    !(irqflags & IRQF_NO_AUTOEN)) ||	/* [한국어] 그것을 거부하지 않았는가 — NMI 설정은 request 뒤에 따로 해야 하는데 그 전에 켜지면 아직 NMI 가 아닌 상태로 처리된다 */
	    !irq_settings_can_request(desc) ||	/* [한국어] 요청이 허용된 선인가 */
	    WARN_ON(irq_settings_is_per_cpu_devid(desc)) ||	/* [한국어] per-CPU 장치 ID 를 쓰는 선은 request_percpu_nmi 를 써야 한다 */
	    !irq_supports_nmi(desc))	/* [한국어] 계층형이 아니고 느린 버스 뒤가 아니며 칩이 NMI 를 지원하는가 */
		return -EINVAL;

	action = kzalloc(sizeof(struct irqaction), GFP_KERNEL);	/* [한국어] 등록 정보 */
	if (!action)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	action->handler = handler;	/* [한국어] NMI 문맥에서 불릴 함수 */
	action->flags = irqflags | IRQF_NO_THREAD | IRQF_NOBALANCING;	/* [한국어] 드라이버가 빠뜨려도 스레드화나 친화도 조정이 일어나지 않게 강제로 더한다 */
	action->name = name;	/* [한국어] /proc/interrupts 표시 */
	action->dev_id = dev_id;	/* [한국어] 핸들러에 넘겨진다 */

	retval = irq_chip_pm_get(&desc->irq_data);	/* [한국어] 컨트롤러 전원을 잡는다 */
	if (retval < 0)	/* [한국어] 실패 */
		goto err_out;

	retval = __setup_irq(irq, desc, action);	/* [한국어] 일반 등록 절차. NMI 설정은 그 뒤에 별도로 한다 */
	if (retval)	/* [한국어] 실패 */
		goto err_irq_setup;

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {	/* [한국어] NMI 상태 전환과 하드웨어 설정을 원자적으로 */
		/* Setup NMI state */
		desc->istate |= IRQS_NMI;	/* [한국어] (위 영어 주석) 이 선을 NMI 로 표시한다. 흐름 처리기와 여러 검사가 이 비트를 본다 */
		retval = irq_nmi_setup(desc);	/* [한국어] 하드웨어에 마스크 불가 설정을 요청한다 */
		if (retval) {	/* [한국어] 칩이 거절했는가 */
			__cleanup_nmi(irq, desc);	/* [한국어] 위에서 한 등록을 통째로 되돌린다. 이 함수가 락을 요구하므로 여기서 부르는 것이 맞다 */
			return -EINVAL;	/* [한국어] 아래 err 레이블로 가지 않는다 — cleanup 이 이미 전원과 모듈 참조를 놓았다 */
		}
		return 0;	/* [한국어] 성공. 이제 이 선이 NMI 로 동작한다 */
	}

err_irq_setup:	/* [한국어] 등록이 실패한 경우 */
	irq_chip_pm_put(&desc->irq_data);	/* [한국어] 전원 참조를 놓는다 */
err_out:	/* [한국어] 전원 획득이 실패한 경우 */
	kfree(action);	/* [한국어] 등록 정보 해제 */

	return retval;	/* [한국어] 실패 원인 */
}

/*
 * [한국어]
 * enable_percpu_irq - per-CPU 인터럽트를 이 CPU 에서 켠다
 *
 * @irq:  대상 인터럽트 번호
 * @type: 트리거 방식, IRQ_TYPE_NONE 이면 기존 설정을 쓴다
 * @return: 없음
 *
 * per-CPU 인터럽트는 request 만으로 동작하지 않는다. 각 CPU 가
 * 자기 것을 따로 켜야 한다 — 그 선의 서술자는 하나지만 하드웨어는
 * CPU 마다 별개이기 때문이다.
 *
 * 그래서 대개 CPU 핫플러그 콜백에서 부른다. CPU 가 올라올 때마다
 * 그 CPU 의 타이머 인터럽트를 켜는 식이다.
 *
 * 트리거 설정이 여기 있는 것이 조금 이상해 보인다. 그것은 선 전체의
 * 성질이라 CPU 마다 다를 수 없다. 첫 CPU 가 설정하고 나머지는 같은
 * 값을 다시 쓰는 셈인데, 무해하고 코드가 단순해진다.
 *
 * 트리거 설정 실패 시 켜지 않고 반환하는 것에 주목: 잘못된 트리거로
 * 켜면 인터럽트가 폭주하거나 아예 오지 않는다.
 *
 * smp_processor_id() 를 쓰므로 마이그레이션이 꺼진 문맥이어야 한다.
 * 다른 CPU 로 옮겨 가면 엉뚱한 CPU 의 비트를 세운다.
 *
 * 실행 컨텍스트: 마이그레이션이 꺼진 문맥, 대개 CPU 핫플러그 콜백.
 *
 * 호출 체인:
 *   CPU 핫플러그 콜백 / per-CPU 장치 드라이버 → [이 함수] →
 *   irq_percpu_enable()
 */
void enable_percpu_irq(unsigned int irq, unsigned int type)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {	/* [한국어] per-CPU 인터럽트만 받는다. 일반 인터럽트는 enable_irq 를 써야 한다 */
		struct irq_desc *desc = scoped_irqdesc;	/* [한국어] 짧은 이름 */

		/*
		 * If the trigger type is not specified by the caller, then
		 * use the default for this interrupt.
		 */
		type &= IRQ_TYPE_SENSE_MASK;	/* [한국어] (위 영어 주석) 트리거 외의 비트를 자른다 */
		if (type == IRQ_TYPE_NONE)	/* [한국어] 지정하지 않았는가 */
			type = irqd_get_trigger_type(&desc->irq_data);	/* [한국어] 기존 설정을 쓴다. 두 번째 CPU 부터는 대개 이 경로다 */

		if (type != IRQ_TYPE_NONE) {	/* [한국어] 설정할 트리거가 있는가 */
			if (__irq_set_trigger(desc, type)) {	/* [한국어] 하드웨어에 설정한다. 선 전체의 성질이라 CPU 마다 다를 수 없고, 첫 CPU 가 정한 값을 나머지가 다시 쓰는 셈이다 */
				WARN(1, "failed to set type for IRQ%d\n", irq);	/* [한국어] 잘못된 트리거로 켜면 인터럽트가 폭주하거나 아예 오지 않는다 */
				return;	/* [한국어] 켜지 않고 물러난다 */
			}
		}
		irq_percpu_enable(desc, smp_processor_id());	/* [한국어] 이 CPU 에서만 켠다. 마이그레이션이 꺼진 문맥이어야 엉뚱한 CPU 의 비트를 세우지 않는다 */
	}
}
EXPORT_SYMBOL_GPL(enable_percpu_irq);	/* [한국어] per-CPU 장치 드라이버가 부른다 */

/*
 * [한국어]
 * enable_percpu_nmi - per-CPU NMI 를 이 CPU 에서 켠다
 *
 * @irq:  대상 인터럽트 번호
 * @type: 트리거 방식
 * @return: 없음
 *
 * 이름만 다른 같은 구현이다. NMI 를 쓰는 코드가 전용 API 짝을 쓰게
 * 하려는 것이다.
 *
 * 다만 순서가 중요하다. per-CPU NMI 는 prepare_percpu_nmi() 로 그
 * CPU 의 NMI 설정을 먼저 한 뒤에 이것을 불러야 한다. 순서가 반대면
 * 아직 NMI 가 아닌 상태로 인터럽트가 켜진다.
 *
 * 실행 컨텍스트: 마이그레이션이 꺼진 문맥.
 *
 * 호출 체인:
 *   워치독 등의 CPU 핫플러그 콜백 → [이 함수] → enable_percpu_irq()
 */
void enable_percpu_nmi(unsigned int irq, unsigned int type)
{
	enable_percpu_irq(irq, type);	/* [한국어] prepare_percpu_nmi 를 먼저 부른 뒤에 이것을 불러야 한다. 반대면 아직 NMI 가 아닌 상태로 켜진다 */
}

/**
 * irq_percpu_is_enabled - Check whether the per cpu irq is enabled
 * @irq:	Linux irq number to check for
 *
 * Must be called from a non migratable context. Returns the enable
 * state of a per cpu interrupt on the current cpu.
 */
/*
 * [한국어]
 * irq_percpu_is_enabled - 이 CPU 에서 per-CPU 인터럽트가 켜져 있는지 본다
 *
 * @irq: 대상 인터럽트 번호
 * @return: true 켜져 있음, false 꺼져 있거나 그런 인터럽트가 없음
 *
 * per-CPU 인터럽트는 CPU 마다 켜짐 상태가 다르므로, "켜져 있는가" 는
 * 항상 "지금 이 CPU 에서" 를 뜻한다.
 *
 * 원본 주석의 조건: 마이그레이션이 꺼진 문맥이어야 한다. 그러지
 * 않으면 검사하는 사이에 다른 CPU 로 옮겨 가 엉뚱한 답을 얻는다.
 *
 * percpu_enabled 비트맵을 보는 것에 주목: IRQD_ 플래그가 아니다.
 * 그 플래그는 인터럽트 하나에 대한 단일 상태라 per-CPU 를 표현할 수
 * 없다.
 *
 * 실행 컨텍스트: 마이그레이션이 꺼진 문맥.
 *
 * 호출 체인:
 *   per-CPU 장치 드라이버 → [이 함수]
 */
bool irq_percpu_is_enabled(unsigned int irq)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU)	/* [한국어] per-CPU 인터럽트만 */
		return cpumask_test_cpu(smp_processor_id(), scoped_irqdesc->percpu_enabled);	/* [한국어] IRQD_ 플래그가 아니라 비트맵을 본다. 그 플래그는 단일 상태라 CPU 별 상태를 표현할 수 없다 */
	return false;	/* [한국어] 없거나 per-CPU 가 아니다 */
}
EXPORT_SYMBOL_GPL(irq_percpu_is_enabled);	/* [한국어] per-CPU 장치 드라이버가 부른다 */

/*
 * [한국어]
 * disable_percpu_irq - per-CPU 인터럽트를 이 CPU 에서 끈다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * enable_percpu_irq() 의 반대다. 훨씬 짧은 것은 트리거 설정이
 * 필요 없어서다.
 *
 * 중첩 카운터가 없는 것에 주목: 일반 인터럽트의 depth 같은 것이
 * 없다. per-CPU 인터럽트는 CPU 핫플러그 경로가 관리하고 여러 곳에서
 * 겹쳐 끄는 일이 없다는 전제다.
 *
 * 실행 컨텍스트: 마이그레이션이 꺼진 문맥, 대개 CPU 핫플러그 콜백.
 *
 * 호출 체인:
 *   CPU 핫플러그 콜백 / per-CPU 장치 드라이버 → [이 함수] →
 *   irq_percpu_disable()
 */
void disable_percpu_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU)	/* [한국어] per-CPU 인터럽트만 */
		irq_percpu_disable(scoped_irqdesc, smp_processor_id());	/* [한국어] 중첩 카운터가 없다. CPU 핫플러그 경로가 관리하고 여러 곳에서 겹쳐 끄는 일이 없다는 전제다 */
}
EXPORT_SYMBOL_GPL(disable_percpu_irq);	/* [한국어] per-CPU 장치 드라이버가 부른다 */

/*
 * [한국어]
 * disable_percpu_nmi - per-CPU NMI 를 이 CPU 에서 끈다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * 이름만 다른 같은 구현이다. NMI 코드가 전용 API 짝을 쓰게 한다.
 *
 * teardown_percpu_nmi() 와 순서가 중요하다. 끄고 나서 NMI 설정을
 * 되돌려야 한다 — 반대면 NMI 가 아닌 상태로 켜진 구간이 생긴다.
 *
 * 실행 컨텍스트: 마이그레이션이 꺼진 문맥.
 *
 * 호출 체인:
 *   워치독 등의 CPU 핫플러그 콜백 → [이 함수] → disable_percpu_irq()
 */
void disable_percpu_nmi(unsigned int irq)
{
	disable_percpu_irq(irq);	/* [한국어] 끄고 나서 teardown_percpu_nmi 를 불러야 한다. 반대면 NMI 가 아닌 상태로 켜진 구간이 생긴다 */
}

/*
 * Internal function to unregister a percpu irqaction.
 */
/*
 * [한국어]
 * __free_percpu_irq - per-CPU 인터럽트 등록을 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 해제할 등록자의 per-CPU 식별자
 * @return: 해제된 action, 실패 시 NULL
 *
 * 일반 __free_irq() 보다 훨씬 짧다. per-CPU 인터럽트는 스레드를
 * 쓸 수 없고 자원 요청도 없어, 목록에서 빼고 참조를 놓는 것이
 * 전부다.
 *
 * 켜진 CPU 가 있으면 거절하는 것이 중요한 검사다. 원본 주석은 없지만
 * 이유는 명확하다 — 켜진 채로 핸들러를 떼면 그 CPU 에서 인터럽트가
 * 올 때 아무도 처리하지 못한다. per-CPU 인터럽트는 각 CPU 가 자기
 * 것을 꺼야 하므로 드라이버가 그 순회를 책임진다.
 *
 * 담당 CPU 와의 교집합을 보는 것에 주목: 이 등록자가 담당하지 않는
 * CPU 에서 켜져 있는 것은 다른 등록자의 몫이라 상관없다.
 *
 * NMI 강등이 마지막 등록자에서 일어난다. 그 선이 더 이상 NMI 로
 * 쓰이지 않는다는 표시다.
 *
 * synchronize_irq 를 부르지 않는 것에 주목: per-CPU 인터럽트는
 * 각 CPU 가 이미 자기 것을 껐음이 위 검사로 보장되므로, 진행 중인
 * 처리가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 인터럽트 문맥에서 부르면 경고한다.
 *
 * 호출 체인:
 *   free_percpu_irq() / free_percpu_nmi() → [이 함수]
 */
static struct irqaction *__free_percpu_irq(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	struct irqaction *action, **action_ptr;	/* [한국어] 찾은 등록자와 그것을 가리키는 포인터의 주소 */

	WARN(in_interrupt(), "Trying to free IRQ %d from IRQ context!\n", irq);	/* [한국어] 아래 /proc 정리가 잠들 수 있다 */

	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return NULL;

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {	/* [한국어] 목록 조작만 락 안에서. 아래 /proc 정리는 밖에서 한다 */
		action_ptr = &desc->action;	/* [한국어] 목록 머리의 주소 */
		for (;;) {		/* [한국어] 목록을 훑어 percpu_dev_id 가 맞는 등록자를 찾는다 */
			action = *action_ptr;	/* [한국어] 현재 항목 */

			if (!action) {	/* [한국어] 못 찾았는가 */
				WARN(1, "Trying to free already-free IRQ %d\n", irq);	/* [한국어] 두 번 해제하거나 엉뚱한 식별자를 넘긴 드라이버 버그다 */
				return NULL;				/* [한국어] 못 찾았다. guard 가 락을 풀어 준다 */
			}

			if (action->percpu_dev_id == dev_id)	/* [한국어] 찾는 등록자인가 */
				break;

			action_ptr = &action->next;	/* [한국어] 다음 항목의 포인터 주소로 */
		}

		if (cpumask_intersects(desc->percpu_enabled, action->affinity)) {	/* [한국어] 이 등록자가 담당하는 CPU 중에 아직 켜진 것이 있는가. 다른 등록자의 CPU 는 상관없다 */
			WARN(1, "percpu IRQ %d still enabled on CPU%d!\n", irq,	/* [한국어] 켜진 채 핸들러를 떼면 그 CPU 에서 인터럽트가 올 때 아무도 처리하지 못한다 */
			     cpumask_first_and(desc->percpu_enabled, action->affinity));	/* [한국어] 어느 CPU 가 문제인지 찍어 준다 */
			return NULL;	/* [한국어] 해제를 거절한다. 드라이버가 각 CPU 에서 먼저 꺼야 한다 */
		}

		/* Found it - now remove it from the list of entries: */
		*action_ptr = action->next;	/* [한국어] (위 영어 주석) 목록에서 뗀다 */

		/* Demote from NMI if we killed the last action */
		if (!desc->action)	/* [한국어] (위 영어 주석) 마지막 등록자였는가 */
			desc->istate &= ~IRQS_NMI;	/* [한국어] 그 선이 더 이상 NMI 로 쓰이지 않는다는 표시 */
	}

	unregister_handler_proc(irq, action);	/* [한국어] 락 밖에서. 진행 중인 /proc 읽기를 기다린다 */
	irq_chip_pm_put(&desc->irq_data);	/* [한국어] 전원 참조. synchronize_irq 가 없는 것은 각 CPU 가 이미 꺼서 진행 중인 처리가 없기 때문이다 */
	module_put(desc->owner);	/* [한국어] 소유 모듈 참조 */
	return action;	/* [한국어] 호출자가 해제한다 */
}

/**
 * free_percpu_irq - free an interrupt allocated with request_percpu_irq
 * @irq:	Interrupt line to free
 * @dev_id:	Device identity to free
 *
 * Remove a percpu interrupt handler. The handler is removed, but the
 * interrupt line is not disabled. This must be done on each CPU before
 * calling this function. The function does not return until any executing
 * interrupts for this IRQ have completed.
 *
 * This function must not be called from interrupt context.
 */
/*
 * [한국어]
 * free_percpu_irq - per-CPU 인터럽트 등록을 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 해제할 등록자의 per-CPU 식별자
 * @return: 없음
 *
 * 원본 주석의 조건이 이 API 의 핵심 규약이다. 코어가 선을 끄지
 * 않으므로, 드라이버가 각 CPU 에서 disable_percpu_irq 를 먼저 불러야
 * 한다. 그러지 않으면 __free_percpu_irq 가 거절한다.
 *
 * 왜 코어가 끄지 않는가: per-CPU 인터럽트를 끄려면 각 CPU 에서
 * 실행돼야 하는데, 이 함수는 한 CPU 에서만 돈다. 다른 CPU 에 작업을
 * 보내는 기구를 쓸 수도 있지만, 드라이버가 이미 CPU 를 순회하는
 * 코드를 갖고 있는 것이 보통이라 그쪽에 맡긴다.
 *
 * 버스 락을 잡는 이유: 해제 과정에서 칩 조작이 일어날 수 있다.
 *
 * kfree 를 인자 안에서 부르는 관용구에 주목: NULL 이어도 안전하므로
 * 실패 검사가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   per-CPU 장치 드라이버 → [이 함수] → __free_percpu_irq()
 */
void free_percpu_irq(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || !irq_settings_is_per_cpu_devid(desc))	/* [한국어] per-CPU 인터럽트인가. 일반 인터럽트는 free_irq 를 써야 한다 */
		return;

	chip_bus_lock(desc);	/* [한국어] 해제 과정에서 칩 조작이 일어날 수 있다 */
	kfree(__free_percpu_irq(irq, dev_id));	/* [한국어] NULL 이어도 안전하므로 실패 검사가 필요 없다 */
	chip_bus_sync_unlock(desc);	/* [한국어] 지연된 조작을 하드웨어로 */
}
EXPORT_SYMBOL_GPL(free_percpu_irq);	/* [한국어] per-CPU 장치 드라이버가 부른다 */

/*
 * [한국어]
 * free_percpu_nmi - per-CPU NMI 등록을 해제한다
 *
 * @irq:    대상 인터럽트 번호
 * @dev_id: 해제할 등록자의 per-CPU 식별자
 * @return: 없음
 *
 * 위 함수와 두 가지가 다르다. NMI 인지 확인하고, 버스 락을 잡지
 * 않는다.
 *
 * 버스 락을 잡지 않는 이유: NMI 로 등록될 수 있는 선은 애초에 느린
 * 버스 뒤에 있을 수 없다. irq_supports_nmi() 가 그것을 강제한다.
 * 그러니 잡을 락이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   워치독 등의 드라이버 → [이 함수] → __free_percpu_irq()
 */
void free_percpu_nmi(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || !irq_settings_is_per_cpu_devid(desc))	/* [한국어] per-CPU 인터럽트인가 */
		return;

	if (WARN_ON(!irq_is_nmi(desc)))	/* [한국어] NMI 로 등록된 선인가 */
		return;

	kfree(__free_percpu_irq(irq, dev_id));	/* [한국어] 버스 락이 없다. NMI 로 등록될 수 있는 선은 애초에 느린 버스 뒤에 있을 수 없어 잡을 락이 없다 */
}

/*
 * [한국어]
 * create_percpu_irqaction - per-CPU 등록 정보를 만든다
 *
 * @handler:  핸들러
 * @flags:    추가 IRQF_ 플래그
 * @devname:  장치 이름
 * @affinity: 담당 CPU 마스크, NULL 이면 모든 CPU
 * @dev_id:   per-CPU 식별자
 * @return:   만든 action, 실패 시 NULL
 *
 * request_percpu_irq_affinity() 와 request_percpu_nmi() 가 공유하는
 * 생성 함수다.
 *
 * 두 플래그를 강제로 더한다. IRQF_PERCPU 는 당연하고,
 * IRQF_NO_SUSPEND 는 per-CPU 인터럽트가 대개 타이머나 IPI 처럼
 * 서스펜드 중에도 필요한 것이기 때문이다.
 *
 * 마지막 조건이 이 함수의 흥미로운 부분이다. 담당 CPU 가 전체가
 * 아니면 IRQF_SHARED 를 세운다.
 *
 * 왜 그런가: 원본 주석대로 담당 CPU 가 겹치지 않으면 여러 등록자가
 * 한 선을 나눠 쓸 수 있다. __setup_irq 의 공유 검사를 통과하려면
 * 그 플래그가 필요하다. 반대로 모든 CPU 를 담당하면 나눠 쓸 여지가
 * 없으므로 세우지 않는다 — 세우면 두 번째 등록자가 겹치는 CPU 로
 * 들어올 수 있게 되어 위험하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   request_percpu_irq_affinity() / request_percpu_nmi() → [이 함수]
 */
static
struct irqaction *create_percpu_irqaction(irq_handler_t handler, unsigned long flags,
					  const char *devname, const cpumask_t *affinity,
					  void __percpu *dev_id)
{
	struct irqaction *action;	/* [한국어] 만들 등록 정보 */

	if (!affinity)	/* [한국어] 담당 CPU 를 지정하지 않았는가 */
		affinity = cpu_possible_mask;	/* [한국어] 모든 CPU 를 담당한다 */

	action = kzalloc_obj(struct irqaction);	/* [한국어] 0 초기화 */
	if (!action)	/* [한국어] 메모리 부족 */
		return NULL;

	action->handler = handler;	/* [한국어] 핸들러 */
	action->flags = flags | IRQF_PERCPU | IRQF_NO_SUSPEND;	/* [한국어] PERCPU 는 당연하고, NO_SUSPEND 는 per-CPU 인터럽트가 대개 타이머나 IPI 처럼 서스펜드 중에도 필요해서다 */
	action->name = devname;	/* [한국어] /proc 표시 */
	action->percpu_dev_id = dev_id;	/* [한국어] per-CPU 변수의 포인터. 핸들러가 자기 CPU 것을 받는다 */
	action->affinity = affinity;	/* [한국어] 담당 CPU. handle_percpu_devid_irq 가 이것으로 핸들러를 고른다 */

	/*
	 * We allow some form of sharing for non-overlapping affinity
	 * masks. Obviously, covering all CPUs prevents any sharing in
	 * the first place.
	 */
	if (!cpumask_equal(affinity, cpu_possible_mask))	/* [한국어] (위 영어 주석) 일부 CPU 만 담당하는가 */
		action->flags |= IRQF_SHARED;	/* [한국어] 담당이 겹치지 않으면 나눠 쓸 수 있고, __setup_irq 의 공유 검사를 통과하려면 이 플래그가 필요하다. 전체를 담당하면 세우지 않는다 — 세우면 두 번째 등록자가 겹쳐 들어올 수 있다 */

	return action;	/* [한국어] 호출자가 kfree 한다. 이름을 돌려주지 않는 것은 per-CPU API 의 규약이 그래서다 */
}

/**
 * request_percpu_irq_affinity - allocate a percpu interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 * @devname:	An ascii name for the claiming device
 * @affinity:	A cpumask describing the target CPUs for this interrupt
 * @dev_id:	A percpu cookie passed back to the handler function
 *
 * This call allocates interrupt resources, but doesn't enable the interrupt
 * on any CPU, as all percpu-devid interrupts are flagged with IRQ_NOAUTOEN.
 * It has to be done on each CPU using enable_percpu_irq().
 *
 * @dev_id must be globally unique. It is a per-cpu variable, and
 * the handler gets called with the interrupted CPU's instance of
 * that variable.
 */
/*
 * [한국어]
 * request_percpu_irq_affinity - per-CPU 인터럽트를 등록한다
 *
 * @irq:      대상 인터럽트 번호
 * @handler:  핸들러
 * @devname:  장치 이름
 * @affinity: 담당 CPU 마스크
 * @dev_id:   per-CPU 식별자
 * @return:   0 성공, 음수 오류
 *
 * ARM 의 지역 타이머처럼 CPU 마다 별개인 인터럽트를 등록한다.
 *
 * 원본 주석의 조건이 중요하다. 이 함수는 인터럽트를 켜지 않는다.
 * per-CPU 인터럽트의 서술자는 IRQ_NOAUTOEN 으로 표시돼 있어 자동
 * 시작되지 않고, 각 CPU 에서 enable_percpu_irq 를 불러야 한다.
 *
 * dev_id 가 per-CPU 변수의 포인터인 것이 이 API 의 핵심이다.
 * 핸들러는 그 변수의 "자기 CPU 인스턴스" 를 받는다. 그래서 같은
 * 핸들러가 CPU 마다 다른 상태를 다룰 수 있다.
 *
 * dev_id 가 NULL 이면 거절하는 이유: 그 포인터가 없으면 핸들러가
 * 자기 CPU 의 상태를 찾을 수 없다. 일반 인터럽트와 달리 선택 사항이
 * 아니다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   ARM 아키텍처 타이머 등 → [이 함수] → __setup_irq()
 */
int request_percpu_irq_affinity(unsigned int irq, irq_handler_t handler, const char *devname,
				const cpumask_t *affinity, void __percpu *dev_id)
{
	struct irqaction *action;	/* [한국어] 만들 등록 정보 */
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	int retval;	/* [한국어] 결과 */

	if (!dev_id)	/* [한국어] per-CPU 식별자가 없는가 */
		return -EINVAL;	/* [한국어] 일반 인터럽트와 달리 선택 사항이 아니다. 없으면 핸들러가 자기 CPU 의 상태를 찾을 수 없다 */

	desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */
	if (!desc || !irq_settings_can_request(desc) ||	/* [한국어] 요청이 허용된 선인가 */
	    !irq_settings_is_per_cpu_devid(desc))	/* [한국어] per-CPU 장치 ID 를 쓰도록 설정된 선인가. 아키텍처가 irq_set_percpu_devid 로 미리 표시해 둔다 */
		return -EINVAL;

	action = create_percpu_irqaction(handler, 0, devname, affinity, dev_id);	/* [한국어] 추가 플래그 없이. NMI 판은 여기에 NO_THREAD 등을 더한다 */
	if (!action)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	retval = irq_chip_pm_get(&desc->irq_data);	/* [한국어] 컨트롤러 전원을 잡는다 */
	if (retval < 0) {	/* [한국어] 실패 */
		kfree(action);		/* [한국어] 전원을 켜지 못했다 */
		return retval;		/* [한국어] 실패 원인을 그대로 올린다 */
	}

	retval = __setup_irq(irq, desc, action);	/* [한국어] 일반 등록 절차와 같다. 자동 시작은 IRQ_NOAUTOEN 때문에 일어나지 않는다 */

	if (retval) {	/* [한국어] 실패 */
		irq_chip_pm_put(&desc->irq_data);		/* [한국어] 전원 참조를 놓는다 */
		kfree(action);		/* [한국어] 등록 정보 해제 */
	}

	return retval;	/* [한국어] 성공해도 아직 인터럽트가 켜지지 않았다. 각 CPU 에서 enable_percpu_irq 를 불러야 한다 */
}
EXPORT_SYMBOL_GPL(request_percpu_irq_affinity);	/* [한국어] per-CPU 장치 드라이버가 부른다 */

/**
 * request_percpu_nmi - allocate a percpu interrupt line for NMI delivery
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 * @name:	An ascii name for the claiming device
 * @affinity:	A cpumask describing the target CPUs for this interrupt
 * @dev_id:	A percpu cookie passed back to the handler function
 *
 * This call allocates interrupt resources for a per CPU NMI. Per CPU NMIs
 * have to be setup on each CPU by calling prepare_percpu_nmi() before
 * being enabled on the same CPU by using enable_percpu_nmi().
 *
 * @dev_id must be globally unique. It is a per-cpu variable, and the
 * handler gets called with the interrupted CPU's instance of that
 * variable.
 *
 * Interrupt lines requested for NMI delivering should have auto enabling
 * setting disabled.
 *
 * If the interrupt line cannot be used to deliver NMIs, function
 * will fail returning a negative value.
 */
/*
 * [한국어]
 * request_percpu_nmi - per-CPU NMI 를 등록한다
 *
 * @irq:      대상 인터럽트 번호
 * @handler:  NMI 문맥에서 불릴 핸들러
 * @name:     장치 이름
 * @affinity: 담당 CPU 마스크
 * @dev_id:   per-CPU 식별자
 * @return:   0 성공, 음수 오류
 *
 * 워치독이 쓰는 등록 경로다. CPU 마다 자기 NMI 를 받아 그 CPU 가
 * 멈췄는지 감시한다.
 *
 * 세 단계 설정이 필요한 것이 원본 주석의 요지다.
 *
 *   1. request_percpu_nmi()   — 등록 (이 함수)
 *   2. prepare_percpu_nmi()   — 각 CPU 에서 NMI 설정
 *   3. enable_percpu_nmi()    — 각 CPU 에서 켜기
 *
 * 2 가 따로 있는 이유: NMI 설정은 CPU 별 레지스터를 건드리므로 각
 * CPU 에서 실행돼야 한다. 등록 시점에는 한 CPU 에서만 돌고 있다.
 *
 * 마지막 검사가 미묘하다. 이미 NMI 인 선에 "모든 CPU 를 담당하는"
 * 등록을 하려 하면 거절한다. 그러면 기존 등록자와 담당이 반드시
 * 겹치기 때문이다. 일부 CPU 만 담당하는 등록은 허용된다 — 겹치지
 * 않으면 나눠 쓸 수 있다.
 *
 * request_nmi() 와 달리 IRQS_NMI 를 여기서 세우는 것에 주목: 그쪽은
 * 하드웨어 설정까지 함께 했지만, per-CPU 는 그것을 2 단계로 미룬다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   워치독 초기화 → [이 함수] → __setup_irq()
 */
int request_percpu_nmi(unsigned int irq, irq_handler_t handler, const char *name,
		       const struct cpumask *affinity, void __percpu *dev_id)
{
	struct irqaction *action;	/* [한국어] 만들 등록 정보 */
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	int retval;	/* [한국어] 결과 */

	if (!handler)	/* [한국어] 핸들러가 없는가 */
		return -EINVAL;	/* [한국어] NMI 는 스레드를 쓸 수 없어 필수다 */

	desc = irq_to_desc(irq);	/* [한국어] 대상 서술자 */

	if (!desc || !irq_settings_can_request(desc) ||	/* [한국어] 요청이 허용된 선인가 */
	    !irq_settings_is_per_cpu_devid(desc) ||	/* [한국어] per-CPU 장치 ID 를 쓰는 선인가 */
	    irq_settings_can_autoenable(desc) ||	/* [한국어] 자동 시작이 허용된 선인가 — NMI 설정 전에 켜지면 안 된다 */
	    !irq_supports_nmi(desc))	/* [한국어] 계층형이 아니고 느린 버스 뒤가 아니며 칩이 NMI 를 지원하는가 */
		return -EINVAL;

	/* The line cannot be NMI already if the new request covers all CPUs */
	if (irq_is_nmi(desc) &&	/* [한국어] (위 영어 주석) 이미 NMI 로 등록된 선인데 */
	    (!affinity || cpumask_equal(affinity, cpu_possible_mask)))	/* [한국어] 모든 CPU 를 담당하려 하는가 — 기존 등록자와 반드시 겹친다. 일부만 담당하면 허용된다 */
		return -EINVAL;

	action = create_percpu_irqaction(handler, IRQF_NO_THREAD | IRQF_NOBALANCING,	/* [한국어] NMI 는 스레드화와 친화도 조정을 허용하지 않는다 */
					 name, affinity, dev_id);
	if (!action)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	retval = irq_chip_pm_get(&desc->irq_data);	/* [한국어] 컨트롤러 전원 */
	if (retval < 0)	/* [한국어] 실패 */
		goto err_out;

	retval = __setup_irq(irq, desc, action);	/* [한국어] 일반 등록 절차 */
	if (retval)	/* [한국어] 실패 */
		goto err_irq_setup;

	scoped_guard(raw_spinlock_irqsave, &desc->lock)	/* [한국어] 상태 비트 설정 */
		desc->istate |= IRQS_NMI;	/* [한국어] 표시만 한다. 하드웨어 설정은 prepare_percpu_nmi 가 각 CPU 에서 따로 한다 — request_nmi 와 다른 점이다 */
	return 0;	/* [한국어] 성공. 아직 2 단계와 3 단계가 남았다 */

err_irq_setup:	/* [한국어] 등록 실패 */
	irq_chip_pm_put(&desc->irq_data);	/* [한국어] 전원 참조를 놓는다 */
err_out:	/* [한국어] 전원 획득 실패 */
	kfree(action);	/* [한국어] 등록 정보 해제 */

	return retval;	/* [한국어] 실패 원인 */
}

/**
 * prepare_percpu_nmi - performs CPU local setup for NMI delivery
 * @irq: Interrupt line to prepare for NMI delivery
 *
 * This call prepares an interrupt line to deliver NMI on the current CPU,
 * before that interrupt line gets enabled with enable_percpu_nmi().
 *
 * As a CPU local operation, this should be called from non-preemptible
 * context.
 *
 * If the interrupt line cannot be used to deliver NMIs, function will fail
 * returning a negative value.
 */
/*
 * [한국어]
 * prepare_percpu_nmi - 이 CPU 에서 NMI 전달 설정을 한다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 0 성공, -EINVAL NMI 가 아니거나 인터럽트 없음, 그 외 칩의 오류
 *
 * per-CPU NMI 설정의 2 단계다. request_percpu_nmi() 로 등록한 뒤,
 * 각 CPU 에서 이것을 부르고, 그 다음 enable_percpu_nmi() 를 부른다.
 *
 * 왜 CPU 마다 따로 해야 하는가: NMI 설정은 대개 CPU 별 레지스터를
 * 건드린다. ARM GIC 라면 그 CPU 의 우선순위 마스크 레지스터다.
 * 다른 CPU 에서 실행하면 엉뚱한 레지스터를 건드린다.
 *
 * preemptible() 검사가 그것을 강제한다. 선점 가능한 상태에서 부르면
 * 도중에 다른 CPU 로 옮겨 가 설정이 흩어진다.
 *
 * NMI 가 아닌 선에 부르는 것을 경고와 함께 거절하는 이유: 순서를
 * 잘못 이해한 드라이버 버그다. request_percpu_nmi 를 먼저 불러야 한다.
 *
 * 실패를 로그로 남기는 것에 주목: 반환값만으로는 어느 인터럽트가
 * 문제인지 알기 어렵다.
 *
 * 실행 컨텍스트: 선점이 꺼진 문맥, 대개 CPU 핫플러그 콜백.
 *
 * 호출 체인:
 *   워치독의 CPU 핫플러그 콜백 → [이 함수] → irq_nmi_setup()
 */
int prepare_percpu_nmi(unsigned int irq)
{
	int ret = -EINVAL;	/* [한국어] 서술자를 못 찾았을 때의 값 */

	WARN_ON(preemptible());	/* [한국어] 선점 가능한 상태면 도중에 다른 CPU 로 옮겨 가 설정이 흩어진다 */

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {	/* [한국어] per-CPU 인터럽트만 */
		if (WARN(!irq_is_nmi(scoped_irqdesc),	/* [한국어] NMI 로 등록된 선인가 */
			 "prepare_percpu_nmi called for a non-NMI interrupt: irq %u\n", irq))
			return -EINVAL;	/* [한국어] 순서를 잘못 이해한 드라이버 버그다. request_percpu_nmi 를 먼저 불러야 한다 */

		ret = irq_nmi_setup(scoped_irqdesc);	/* [한국어] 이 CPU 의 레지스터를 설정한다 */
		if (ret)	/* [한국어] 칩이 거절했는가 */
			pr_err("Failed to setup NMI delivery: irq %u\n", irq);	/* [한국어] 반환값만으로는 어느 인터럽트가 문제인지 알기 어렵다 */
	}
	return ret;	/* [한국어] 0 이면 이 CPU 에서 NMI 전달 준비가 끝났다. 이제 enable_percpu_nmi 를 부를 수 있다 */
}

/**
 * teardown_percpu_nmi - undoes NMI setup of IRQ line
 * @irq: Interrupt line from which CPU local NMI configuration should be removed
 *
 * This call undoes the setup done by prepare_percpu_nmi().
 *
 * IRQ line should not be enabled for the current CPU.
 * As a CPU local operation, this should be called from non-preemptible
 * context.
 */
/*
 * [한국어]
 * teardown_percpu_nmi - 이 CPU 의 NMI 전달 설정을 되돌린다
 *
 * @irq: 대상 인터럽트 번호
 * @return: 없음
 *
 * prepare_percpu_nmi() 의 반대다. 순서는 disable_percpu_nmi() 를
 * 먼저 부른 뒤 이것이다 — 원본 주석의 "이 CPU 에서 켜져 있으면
 * 안 된다" 가 그 뜻이다.
 *
 * 왜 그 순서인가: NMI 설정을 되돌리면 그 선이 보통 인터럽트가 된다.
 * 켜진 채로 그렇게 하면 그 사이에 오는 신호가 NMI 도 일반 인터럽트도
 * 아닌 어중간한 상태로 처리된다.
 *
 * 그 조건을 코드가 검사하지는 않는다. 검사하려면 percpu_enabled
 * 비트맵을 봐야 하는데, 그것만으로는 이 CPU 에서 방금 껐는지 원래
 * 안 켰는지 구분할 수 없어 유용한 경고가 되기 어렵다.
 *
 * 실행 컨텍스트: 선점이 꺼진 문맥.
 *
 * 호출 체인:
 *   워치독의 CPU 핫플러그 콜백 → [이 함수] → irq_nmi_teardown()
 */
void teardown_percpu_nmi(unsigned int irq)
{
	WARN_ON(preemptible());	/* [한국어] CPU 별 레지스터를 건드리므로 옮겨 가면 안 된다 */

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {	/* [한국어] per-CPU 인터럽트만 */
		if (WARN_ON(!irq_is_nmi(scoped_irqdesc)))	/* [한국어] NMI 로 등록된 선인가 */
			return;
		irq_nmi_teardown(scoped_irqdesc);	/* [한국어] 이 CPU 의 설정을 되돌린다. 켜진 채로 하면 그 사이의 신호가 어중간한 상태로 처리된다 */
	}
}

/*
 * [한국어]
 * __irq_get_irqchip_state - 계층을 훑어 칩의 내부 상태를 읽는다
 *
 * @data:  시작할 층의 irq_data
 * @which: 어떤 상태를 묻는가 (IRQCHIP_STATE_PENDING, _ACTIVE, _MASKED)
 * @state: 결과를 담을 곳 (출력)
 * @return: 0 성공, -EINVAL 지원하는 층 없음, -ENODEV 칩이 없음
 *
 * 컨트롤러가 이 인터럽트를 어떻게 보고 있는지 묻는다. 대기 중인가,
 * CPU 에 전달 중인가, 마스크돼 있는가.
 *
 * 누가 쓰는가: __synchronize_hardirq() 가 "하드웨어에 아직 전달되지
 * 않은 인터럽트가 있는가" 를 확인할 때, 그리고 가상화가 게스트
 * 상태를 저장할 때다.
 *
 * 계층을 훑는 것에 주목: 어느 층이 그 상태를 아는지 미리 알 수 없다.
 * 대개 가장 안쪽 벡터 도메인이 알지만, 리매핑 하드웨어가 알 수도 있다.
 *
 * #ifdef 로 parent_data 접근을 감싸는 이유: 계층형이 없는 빌드에는
 * 그 필드가 없다. data 를 NULL 로 만들어 루프를 끝낸다.
 *
 * ret 초기값이 -EINVAL 인 것과 마지막 검사가 짝을 이룬다. 루프가
 * break 로 끝났으면 data 가 유효하고, 끝까지 갔으면 NULL 이라 초기값이
 * 그대로 나간다.
 *
 * 실행 컨텍스트: 호출자를 따른다. desc->lock 을 쥔 채 불릴 수 있다.
 *
 * 호출 체인:
 *   __synchronize_hardirq() / irq_get_irqchip_state() → [이 함수]
 */
static int __irq_get_irqchip_state(struct irq_data *data, enum irqchip_irq_state which, bool *state)
{
	struct irq_chip *chip;	/* [한국어] 각 층의 칩 */
	int err = -EINVAL;	/* [한국어] 지원하는 층을 못 찾았을 때의 값 */

	do {
		chip = irq_data_get_irq_chip(data);	/* [한국어] 이 층의 칩 */
		if (WARN_ON_ONCE(!chip))	/* [한국어] 칩이 없는 층이 있는가 — 계층 구성이 잘못됐다 */
			return -ENODEV;
		if (chip->irq_get_irqchip_state)	/* [한국어] 상태를 알려 줄 수 있는 층인가 */
			break;	/* [한국어] 찾았다. data 가 그 층을 가리킨 채 루프를 벗어난다 */
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형이 없는 빌드에는 parent_data 필드가 없다 */
		data = data->parent_data;	/* [한국어] 한 층 안으로 */
#else
		data = NULL;	/* [한국어] 층이 하나뿐이라 루프를 끝낸다 */
#endif
	} while (data);

	if (data)	/* [한국어] 지원하는 층을 찾았는가 */
		err = chip->irq_get_irqchip_state(data, which, state);	/* [한국어] 그 층에 묻는다. 못 찾았으면 -EINVAL 이 그대로 나간다 */
	return err;	/* [한국어] 0 이면 *state 가 갱신됐고, 아니면 건드리지 않았다 */
}

/**
 * irq_get_irqchip_state - returns the irqchip state of a interrupt.
 * @irq:	Interrupt line that is forwarded to a VM
 * @which:	One of IRQCHIP_STATE_* the caller wants to know about
 * @state:	a pointer to a boolean where the state is to be stored
 *
 * This call snapshots the internal irqchip state of an interrupt,
 * returning into @state the bit corresponding to stage @which
 *
 * This function should be called with preemption disabled if the interrupt
 * controller has per-cpu registers.
 */
/*
 * [한국어]
 * irq_get_irqchip_state - 칩의 내부 상태를 읽는다 (공개 API)
 *
 * @irq:   대상 인터럽트 번호
 * @which: 어떤 상태를 묻는가
 * @state: 결과를 담을 곳 (출력)
 * @return: 0 성공, -EINVAL 인터럽트가 없거나 지원하는 층 없음
 *
 * 위 내부 함수에 서술자 조회와 잠금을 두른 껍데기다.
 *
 * 주 용도가 가상화다. 게스트를 저장하거나 마이그레이션할 때, 그
 * 게스트에 전달 중이던 인터럽트의 상태를 함께 저장해야 한다. 원본
 * 주석의 "VM 에 전달되는 인터럽트" 가 그 뜻이다.
 *
 * 원본 주석의 조건: CPU 별 레지스터를 가진 컨트롤러에서는 선점을
 * 꺼야 한다. 읽는 도중에 CPU 가 바뀌면 엉뚱한 레지스터를 읽는다.
 *
 * buslock 판을 쓰는 이유: 상태를 읽으려면 느린 버스 뒤의 칩과
 * 통신해야 할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. per-CPU 레지스터 칩이면 선점을 꺼야
 * 한다.
 *
 * 호출 체인:
 *   KVM / 가상화 코드 → [이 함수] → __irq_get_irqchip_state()
 */
int irq_get_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool *state)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {	/* [한국어] 상태를 읽으려면 느린 버스 뒤의 칩과 통신해야 할 수 있다 */
		struct irq_data *data = irq_desc_get_irq_data(scoped_irqdesc);	/* [한국어] 가장 바깥 층부터 */

		return __irq_get_irqchip_state(data, which, state);	/* [한국어] 계층을 훑어 아는 층에 묻는다 */
	}
	return -EINVAL;	/* [한국어] 그런 인터럽트가 없다 */
}
EXPORT_SYMBOL_GPL(irq_get_irqchip_state);	/* [한국어] KVM 이 부른다 */

/**
 * irq_set_irqchip_state - set the state of a forwarded interrupt.
 * @irq:	Interrupt line that is forwarded to a VM
 * @which:	State to be restored (one of IRQCHIP_STATE_*)
 * @val:	Value corresponding to @which
 *
 * This call sets the internal irqchip state of an interrupt, depending on
 * the value of @which.
 *
 * This function should be called with migration disabled if the interrupt
 * controller has per-cpu registers.
 */
/*
 * [한국어]
 * irq_set_irqchip_state - 칩의 내부 상태를 설정한다
 *
 * @irq:   대상 인터럽트 번호
 * @which: 어떤 상태를 설정하는가
 * @val:   설정할 값
 * @return: 0 성공, -EINVAL 지원하는 층 없음, -ENODEV 칩 없음
 *
 * 위 get 의 반대다. 게스트를 복원할 때 "이 인터럽트는 대기 중이었다"
 * 같은 상태를 되살린다.
 *
 * 계층 순회를 이 함수가 직접 하는 것에 주목: get 은 내부 함수로
 * 뺐는데 set 은 인라인이다. get 쪽은 __synchronize_hardirq() 도
 * 쓰므로 함수로 뺄 이유가 있었지만, set 은 여기서만 쓰인다.
 *
 * irqd_get_parent_data() 를 쓰는 것도 다르다. 그 인라인이 계층형
 * 여부를 안에서 처리하므로 #ifdef 가 필요 없다. get 쪽이 더 오래된
 * 코드라 그 인라인이 생기기 전의 방식이 남아 있다.
 *
 * 원본 주석의 조건이 get 과 미묘하게 다르다 — 선점이 아니라
 * 마이그레이션을 꺼야 한다고 말한다. 실질적으로 같은 요구다.
 *
 * 실행 컨텍스트: 프로세스 문맥. per-CPU 레지스터 칩이면 마이그레이션을
 * 꺼야 한다.
 *
 * 호출 체인:
 *   KVM / 가상화 코드 → [이 함수] → chip->irq_set_irqchip_state()
 */
int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool val)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {	/* [한국어] 느린 버스 뒤의 칩과 통신할 수 있다 */
		struct irq_data *data = irq_desc_get_irq_data(scoped_irqdesc);	/* [한국어] 가장 바깥 층부터 */
		struct irq_chip *chip;	/* [한국어] 각 층의 칩 */

		do {
			chip = irq_data_get_irq_chip(data);	/* [한국어] 이 층의 칩 */

			if (WARN_ON_ONCE(!chip))	/* [한국어] 칩이 없는 층 — 계층 구성이 잘못됐다 */
				return -ENODEV;

			if (chip->irq_set_irqchip_state)	/* [한국어] 상태 설정을 지원하는 층인가 */
				break;	/* [한국어] 찾았다 */

			data = irqd_get_parent_data(data);	/* [한국어] 이 인라인이 계층형 여부를 안에서 처리해 #ifdef 가 필요 없다. get 쪽은 더 오래된 코드라 옛 방식이 남아 있다 */
		} while (data);

		if (data)	/* [한국어] 지원하는 층을 찾았는가 */
			return chip->irq_set_irqchip_state(data, which, val);	/* [한국어] 그 층에 설정한다 */
	}
	return -EINVAL;	/* [한국어] 인터럽트가 없거나 지원하는 층이 없다 */
}
EXPORT_SYMBOL_GPL(irq_set_irqchip_state);	/* [한국어] KVM 이 게스트 복원 때 부른다 */

/**
 * irq_has_action - Check whether an interrupt is requested
 * @irq:	The linux irq number
 *
 * Returns: A snapshot of the current state
 */
/*
 * [한국어]
 * irq_has_action - 이 인터럽트가 요청된 상태인지 본다
 *
 * @irq: 대상 인터럽트 번호
 * @return: true 등록자가 하나 이상 있음, false 없음
 *
 * 진단 코드나 아키텍처 코드가 "이 선을 쓰고 있는 드라이버가 있는가"
 * 를 물을 때 쓴다.
 *
 * RCU 로 감싸는 이유: irq_to_desc 가 반환한 서술자를 곧바로 읽는데,
 * 그 사이에 다른 CPU 가 해제할 수 있다. RCU 읽기 구역 안에 있으면
 * 그 메모리가 해제되지 않는다.
 *
 * 원본 주석의 "현재 상태의 스냅숏" 이 중요하다. 반환 직후 값이
 * 바뀔 수 있으므로, 이 결과에 기대어 뭔가를 하면 안 된다. 진단
 * 목적으로만 쓰라는 뜻이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   아키텍처 코드 / 진단 코드 → [이 함수]
 */
bool irq_has_action(unsigned int irq)
{
	bool res;	/* [한국어] 결과 */

	rcu_read_lock();	/* [한국어] 서술자를 읽는 사이에 해제되지 않게 한다 */
	res = irq_desc_has_action(irq_to_desc(irq));	/* [한국어] NULL 서술자도 안전하게 처리하는 헬퍼다 */
	rcu_read_unlock();	/* [한국어] 결과를 읽은 뒤 구역을 벗어난다. res 는 값이라 안전하다 */
	return res;	/* [한국어] 반환 직후 바뀔 수 있는 스냅숏이다. 이 결과에 기대어 뭔가를 하면 안 된다 */
}
EXPORT_SYMBOL_GPL(irq_has_action);	/* [한국어] 아키텍처 코드가 부른다 */

/**
 * irq_check_status_bit - Check whether bits in the irq descriptor status are set
 * @irq:	The linux irq number
 * @bitmask:	The bitmask to evaluate
 *
 * Returns: True if one of the bits in @bitmask is set
 */
/*
 * [한국어]
 * irq_check_status_bit - 설정 워드의 비트를 검사한다
 *
 * @irq:     대상 인터럽트 번호
 * @bitmask: 검사할 비트 조합
 * @return:  true 하나라도 세워져 있음, false 없거나 인터럽트 없음
 *
 * 세 상태 워드 중 설정 워드(status_use_accessors)를 읽는다. 그 이름의
 * use_accessors 가 "직접 만지지 말고 접근자를 쓰라" 는 뜻이고, 이
 * 함수가 그 접근자 중 하나다.
 *
 * 여러 비트를 한 번에 검사할 수 있는 것에 주목: OR 조건이다.
 * "IRQ_NO_BALANCING 이거나 IRQ_PER_CPU 인가" 같은 물음에 답한다.
 *
 * RCU 보호는 위 irq_has_action 과 같은 이유다.
 *
 * 이중 부정으로 불린화하는 것에 주목: 비트 연산 결과는 0 또는 그
 * 비트값이라 그대로 bool 에 넣으면 컴파일러 경고가 날 수 있다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   irq_is_nmi() 등의 인라인 / 진단 코드 → [이 함수]
 */
bool irq_check_status_bit(unsigned int irq, unsigned int bitmask)
{
	struct irq_desc *desc;	/* [한국어] 대상 서술자 */
	bool res = false;	/* [한국어] 없는 인터럽트는 거짓 */

	rcu_read_lock();	/* [한국어] 읽는 사이에 해제되지 않게 한다 */
	desc = irq_to_desc(irq);	/* [한국어] 서술자 조회 */
	if (desc)	/* [한국어] 존재하는가 */
		res = !!(desc->status_use_accessors & bitmask);	/* [한국어] OR 조건이라 여러 비트를 한 번에 검사할 수 있다. 이중 부정은 비트값을 불린으로 바꾸는 관용구다 */
	rcu_read_unlock();	/* [한국어] 구역을 벗어난다 */
	return res;	/* [한국어] 스냅숏이다. 반환 직후 바뀔 수 있다 */
}
EXPORT_SYMBOL_GPL(irq_check_status_bit);	/* [한국어] 인라인 헬퍼들이 이것을 통해 설정 워드를 읽는다 */
