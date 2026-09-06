// SPDX-License-Identifier: LGPL-2.1+
/*
 * [한국어 설명] 인터럽트 코어의 비활성 깊이(depth) 회계를 검증하는 KUnit 시험 (irq_test.c)
 *
 * === 파일의 역할 ===
 * 인터럽트 서브시스템의 depth 관리가 여러 경로에서 어긋나지 않는지 확인하는
 * 단위 시험이다. 가짜 chip 을 만들어 실제 인터럽트 번호를 배정받고, 그것에
 * disable/enable/free/shutdown/CPU 핫플러그를 걸어 보며 depth 값을 확인한다.
 *
 * depth 가 무엇인가: 인터럽트가 몇 겹으로 비활성화되어 있는지 세는 카운터다.
 * disable_irq() 가 올리고 enable_irq() 가 내리며, 0 이 될 때만 실제로 켜진다.
 * 여러 곳에서 독립적으로 끄고 켤 수 있게 하는 장치다.
 *
 * 왜 시험이 필요한가: depth 를 건드리는 경로가 여럿이다 — 사용자의
 * disable_irq, 절전, CPU 핫플러그로 인한 managed shutdown, 오탐 감지의
 * 강제 비활성화, free_irq 의 정리가 모두 그렇다. 이들이 겹칠 때 depth 가
 * 어긋나면 인터럽트가 영원히 꺼진 채 남거나(depth 가 0 으로 내려가지 않음)
 * 예상보다 일찍 켜진다. 어느 쪽이든 재현하기 어려운 버그가 된다.
 *
 * 네 가지 시나리오를 시험한다:
 *   기본 disable/enable 왕복.
 *   비활성 상태에서 free 한 뒤 다시 request 했을 때의 초기화.
 *   managed 인터럽트의 shutdown 과 복원.
 *   CPU 핫플러그로 인한 managed shutdown 과 복원.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 시험 대상은 kernel/irq 전체다:
 *
 *   이 파일의 시험 함수
 *     ↓ irq_domain_alloc_descs()      — irqdesc.c
 *   가짜 인터럽트 번호 확보
 *     ↓ request_irq()                 — manage.c
 *   핸들러 등록, depth 가 0 이 됨
 *     ↓ disable_irq()/enable_irq()    — manage.c
 *   depth 증감 확인
 *     ↓ irq_shutdown_and_deactivate() — chip.c
 *   managed shutdown 상태 확인
 *     ↓ remove_cpu()/add_cpu()        — cpuhotplug.c 를 거쳐
 *   자동 복원 확인
 *
 * 실행 컨텍스트: KUnit 시험 실행, 프로세스 문맥. CPU 핫플러그 시험은
 * 실제로 CPU 를 내리고 올린다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — irq_settings_clr_norequest, irq_shutdown_and_deactivate,
 *     irq_activate, irq_startup_managed. 코어 내부 함수를 직접 부르는 것이
 *     이 시험의 성격을 말해 준다 — 공개 API 만으로는 도달할 수 없는 상태를
 *     만들어야 하기 때문이다.
 *   kunit — 시험 골격과 단언 매크로.
 *   cpu.h — remove_cpu/add_cpu. 실제로 CPU 를 내리고 올린다.
 *
 * 이 파일에 의존하는 곳: 없다. 시험 전용 모듈이다.
 *
 * === 주요 함수/구조체 요약 ===
 * noop_handler()          — 언제나 IRQ_HANDLED 를 돌려주는 시험용 핸들러.
 * fake_irq_chip           — 아무 일도 하지 않는 시험용 컨트롤러.
 * irq_test_setup_fake_irq()— 가짜 인터럽트 하나를 준비한다.
 * irq_disable_depth_test()— disable/enable 왕복.
 * irq_free_disabled_test()— 비활성 상태에서 free 한 뒤의 재요청.
 * irq_shutdown_depth_test()— managed shutdown 과 복원.
 * irq_cpuhotplug_test()   — CPU 를 실제로 내리고 올려 본다.
 *
 * KUNIT_ASSERT 와 KUNIT_EXPECT 의 차이가 이 파일을 읽는 데 중요하다.
 * ASSERT 는 실패하면 그 시험을 곧바로 중단하고, EXPECT 는 실패를 기록하되
 * 계속 진행한다. 이후 단계가 그 값에 의존하면 ASSERT 를, 독립적이면
 * EXPECT 를 쓴다.
 */

#include <linux/cleanup.h>	/* [한국어] scoped_guard — 아래 shutdown 시험이 서술자 락을 잡을 때 쓴다 */
#include <linux/cpu.h>	/* [한국어] remove_cpu/add_cpu, cpu_is_hotpluggable — CPU 핫플러그 시험 */
#include <linux/cpumask.h>	/* [한국어] CPU_MASK_ALL, cpumask_of — 친화도 마스크 구성 */
#include <linux/interrupt.h>	/* [한국어] request_irq/free_irq/disable_irq/enable_irq */
#include <linux/irq.h>	/* [한국어] struct irq_chip 과 irqd_* 상태 판정자 */
#include <linux/irqdesc.h>	/* [한국어] struct irq_desc 와 irq_to_desc() */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_alloc_descs() — 시험용 번호를 받아 온다 */
#include <linux/nodemask.h>	/* [한국어] NUMA_NO_NODE */
#include <kunit/test.h>	/* [한국어] KUnit 시험 골격과 단언 매크로 */

#include "internals.h"	/* [한국어] 코어 내부 함수를 직접 부른다. 공개 API 만으로는 만들 수 없는 상태를 시험해야 하기 때문이다 */

/*
 * [한국어]
 * noop_handler - 시험용 인터럽트 핸들러
 *
 * @irq:    인터럽트 번호. 쓰이지 않는다.
 * @data:   요청 때 준 dev_id. 쓰이지 않는다.
 * @return: 항상 IRQ_HANDLED.
 *
 * 실제로 아무 일도 하지 않지만 IRQ_HANDLED 를 돌려주는 것이 중요하다.
 * IRQ_NONE 을 돌려주면 오탐 감지가 이 인터럽트를 세기 시작하고, 시험이
 * 인터럽트를 여러 번 발생시키면 "nobody cared" 로 꺼질 수 있다.
 *
 * 이 시험들은 사실 인터럽트를 발생시키지 않는다 — depth 회계만 확인하므로
 * 핸들러가 불릴 일이 없다. 그래도 request_irq 가 핸들러를 요구하므로
 * 형식상 필요하다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥(불린다면).
 *
 * 호출 체인:
 *   (이 시험에서는 실제로 불리지 않는다)
 */
static irqreturn_t noop_handler(int irq, void *data)
{
	return IRQ_HANDLED;	/* [한국어] IRQ_NONE 을 주면 오탐 감지가 세기 시작한다. 아무 일도 하지 않지만 처리했다고 답한다 */
}

/*
 * [한국어]
 * noop - 아무 일도 하지 않는 chip 콜백
 *
 * @data: 대상 irq_data. 쓰이지 않는다.
 *
 * 아래 fake_irq_chip 의 반환값 없는 콜백들을 채운다. dummychip.c 의
 * 같은 이름 함수와 목적이 같지만, 그쪽은 코어의 널 객체이고 이쪽은
 * 시험 전용이라 따로 두었다.
 *
 * 왜 NULL 대신 이것을 꽂는가: chip 콜백이 NULL 이면 코어가 그 인터럽트를
 * 다르게 다룬다. 예를 들어 irq_mask 가 없으면 "마스크할 수 없는 인터럽트"
 * 로 판단해 흐름 제어 경로가 달라진다. 시험은 보통의 인터럽트와 같은
 * 경로를 타야 하므로 전부 채워 둔다.
 *
 * 실행 컨텍스트: 시험이 disable/enable 할 때 인터럽트가 꺼진 상태로 불린다.
 *
 * 호출 체인:
 *   mask_irq()/unmask_irq()/irq_enable() 등 → chip 콜백 → [이 함수]
 */
static void noop(struct irq_data *data) { }

/*
 * [한국어]
 * noop_ret - 아무 일도 하지 않고 0 을 돌려주는 chip 콜백
 *
 * @data:   대상 irq_data. 쓰이지 않는다.
 * @return: 항상 0.
 *
 * irq_startup 자리에 꽂는다. dummychip.c 의 같은 이름 함수와 마찬가지로,
 * 여기서 0 은 "성공" 이 아니라 "밀려 있던 인터럽트가 없다" 는 뜻이다.
 *
 * 0 이 아닌 값을 돌려주면 코어가 그 인터럽트를 곧바로 재전송하려 든다.
 * 시험은 인터럽트가 실제로 발생하는 것을 원하지 않으므로 0 이어야 한다.
 *
 * 실행 컨텍스트: 인터럽트를 켤 때, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_startup() → chip->irq_startup → [이 함수]
 */
static unsigned int noop_ret(struct irq_data *data) { return 0; }

/*
 * [한국어]
 * noop_affinity - 친화도 설정을 성공한 것으로 처리하는 chip 콜백
 *
 * @data:   대상 irq_data.
 * @dest:   요청된 목적지 CPU 집합.
 * @force:  온라인 검사를 건너뛸지. 이 구현은 무시한다.
 * @return: 항상 0(성공).
 *
 * 위 두 noop 과 달리 실제로 무언가를 한다 — 유효 친화도를 갱신한다.
 *
 * 왜 그것이 필요한가: cpuhotplug.c 의 irq_needs_fixup() 이 유효 친화도를
 * 보고 "이 인터럽트가 지금 내려가는 CPU 를 가리키는가" 를 판정한다. 그
 * 값을 갱신하지 않으면 CPU 핫플러그 시험에서 인터럽트가 옮겨지지 않아
 * 시험이 무의미해진다.
 *
 * 진짜 하드웨어라면 여기서 레지스터를 쓰고 하드웨어가 실제로 고른 CPU 를
 * 기록할 것이다. 시험용 chip 은 요청된 것을 그대로 받아들인다.
 *
 * 실행 컨텍스트: 친화도 설정 경로. 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → chip->irq_set_affinity → [이 함수]
 */
static int noop_affinity(struct irq_data *data, const struct cpumask *dest,
			 bool force)
{
	irq_data_update_effective_affinity(data, dest);	/* [한국어] 요청된 것을 그대로 받아들인다. 이 갱신이 없으면 CPU 핫플러그 시험에서 인터럽트가 옮겨지지 않는다 */

	return 0;	/* [한국어] 언제나 성공 */
}

/* [한국어] 시험용 가짜 인터럽트 컨트롤러.
 *
 * dummychip.c 의 dummy_irq_chip 과 비슷하지만 두 가지가 다르다:
 *   irq_set_affinity 가 있다 — CPU 핫플러그 시험에 필요하다.
 *   시험 파일 안에 따로 두었다 — 시험이 chip 의 동작을 바꿔야 할 때
 *     코어의 것을 건드리지 않기 위해서다.
 *
 * 콜백을 빠짐없이 채우는 것이 요점이다. 하나라도 NULL 이면 코어가 그
 * 인터럽트를 특수한 경우로 다뤄, 시험이 보통의 인터럽트와 다른 경로를 탄다. */
static struct irq_chip fake_irq_chip = {
	.name           = "fake",
	/* [한국어] /proc/interrupts 에 표시될 이름.
	 * 설정자: 이 정적 초기화. 읽는 자: proc.c 의 출력.
	 * 시험 중에만 나타나며, 시험이 끝나면 인터럽트가 해제되어 사라진다. */

	.irq_startup    = noop_ret,
	/* [한국어] 시작 콜백. 0 은 "밀려 있던 인터럽트 없음" 이다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_startup() (chip.c). */

	.irq_shutdown   = noop,
	/* [한국어] 종료 콜백. 아래 shutdown 시험이 이 경로를 탄다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_shutdown() (chip.c). */

	.irq_enable     = noop,
	/* [한국어] 활성화 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_enable() (chip.c). */

	.irq_disable    = noop,
	/* [한국어] 비활성화 콜백. 시험이 disable_irq 를 부를 때 지나간다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_disable() (chip.c). */

	.irq_ack        = noop,
	/* [한국어] 인터럽트 도착 알림. 이 시험은 인터럽트를 발생시키지 않아
	 *   실제로 불리지 않지만, 없으면 코어가 다르게 동작한다.
	 * 설정자: 이 정적 초기화. 읽는 자: 흐름 제어 핸들러. */

	.irq_mask       = noop,
	/* [한국어] 마스크 콜백.
	 * 설정자: 이 정적 초기화. 읽는 자: mask_irq() (chip.c).
	 * 이것이 NULL 이면 코어가 "마스크할 수 없는 인터럽트" 로 보고
	 *   흐름 제어 경로를 달리한다. 그래서 반드시 채운다. */

	.irq_unmask     = noop,
	/* [한국어] 언마스크 콜백. 위 mask 의 짝.
	 * 설정자: 이 정적 초기화. 읽는 자: unmask_irq() (chip.c). */

	.irq_set_affinity = noop_affinity,
	/* [한국어] 친화도 설정 — 이 chip 이 dummy_irq_chip 과 갈리는 지점이다.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_do_set_affinity() (manage.c).
	 * 왜 필요한가: 이것이 없으면 cpuhotplug.c 의 migrate_one_irq() 가
	 *   "옮길 수 없다" 며 곧바로 물러나, CPU 핫플러그 시험이 아무것도
	 *   검증하지 못한다. */

	.flags          = IRQCHIP_SKIP_SET_WAKE,
	/* [한국어] irq_set_wake 콜백이 없어도 오류로 다루지 말라는 표시.
	 * 설정자: 이 정적 초기화. 읽는 자: irq_set_irq_wake().
	 * 시험은 깨우기를 쓰지 않지만, 코어의 다른 경로가 이 설정을 시도할
	 *   수 있어 미리 열어 둔다. */
};

/*
 * [한국어]
 * irq_test_setup_fake_irq - 시험용 가짜 인터럽트 하나를 준비한다
 *
 * @test: KUnit 시험 문맥. 단언이 실패하면 이것을 통해 시험을 중단한다.
 * @affd: 친화도 요구. NULL 이면 기본값, managed 시험에서는 채워서 넘긴다.
 * @return: 배정받은 리눅스 인터럽트 번호.
 *
 * 네 시험이 모두 이 함수로 시작한다. 하는 일은 셋이다.
 *
 *   1. 서술자를 하나 배정받는다. 도메인 없이 번호만 잡는 것이라,
 *      실제 하드웨어와 연결되지 않은 순수한 시험용 번호가 된다.
 *   2. 위 가짜 chip 과 가장 단순한 흐름 제어를 꽂는다.
 *   3. IRQ_NOREQUEST 를 지워 request_irq 가 가능하게 만든다.
 *
 * 3 이 필요한 이유가 원 주석에 있다. 아키텍처에 따라 새 서술자의 기본
 * 플래그가 NOREQUEST | NOPROBE 인데, 그러면 request_irq 가 -EINVAL 로
 * 거절된다. 보통은 컨트롤러 드라이버가 열어 주지만 여기서는 그런 드라이버가
 * 없으므로 시험이 직접 연다.
 *
 * ASSERT 를 쓰는 것에 주의: 여기서 실패하면 이후 단계가 전부 무의미하므로
 * 곧바로 중단해야 한다.
 *
 * 실행 컨텍스트: KUnit 시험, 프로세스 문맥.
 *
 * 호출 체인:
 *   네 시험 함수 → [이 함수] → irq_domain_alloc_descs() (irqdesc.c)
 */
static int irq_test_setup_fake_irq(struct kunit *test, struct irq_affinity_desc *affd)
{
	struct irq_desc *desc;	/* [한국어] 배정받은 서술자 */
	int virq;	/* [한국어] 그 인터럽트 번호 */

	virq = irq_domain_alloc_descs(-1, 1, 0, NUMA_NO_NODE, affd);	/* [한국어] 번호를 가리지 않고(-1) 하나만, NUMA 노드도 가리지 않고 잡는다. affd 가 있으면 managed 인터럽트가 된다 */
	KUNIT_ASSERT_GE(test, virq, 0);	/* [한국어] 0 이상이어야 한다. 실패하면 이후가 전부 무의미하므로 ASSERT 로 곧바로 중단한다 */

	irq_set_chip_and_handler(virq, &fake_irq_chip, handle_simple_irq);	/* [한국어] 가짜 컨트롤러와 가장 단순한 흐름 제어를 함께 꽂는다 */

	desc = irq_to_desc(virq);	/* [한국어] 번호로 서술자를 찾는다 */
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);	/* [한국어] 방금 잡은 번호라 있어야 한다 */

	/* On some architectures, IRQs are NOREQUEST | NOPROBE by default. */
	/* [한국어] (위 영어 주석에 이어) 요청 금지를 풀어 준다.
	 *
	 * 새 서술자의 기본 플래그가 아키텍처마다 다르다. NOREQUEST 가 서 있으면
	 * request_irq 가 -EINVAL 로 거절되는데, 보통은 컨트롤러 드라이버가
	 * 열어 준다. 이 시험에는 그런 드라이버가 없으므로 직접 연다. */
	irq_settings_clr_norequest(desc);	/* [한국어] settings.h 의 접근자. 코어 내부라 internals.h 를 포함해야 쓸 수 있다 */

	return virq;	/* [한국어] 시험이 이 번호로 request_irq 등을 부른다 */
}

/*
 * [한국어]
 * irq_disable_depth_test - disable/enable 한 왕복에서 depth 가 맞는지 확인한다
 *
 * @test: KUnit 시험 문맥.
 *
 * 네 시험 중 가장 단순하며, 나머지의 기준선이 된다.
 *
 * 확인하는 것:
 *   request_irq 직후 depth 가 0 이다 — 자동으로 켜졌다는 뜻이다.
 *   disable_irq 뒤 1 이 된다.
 *   enable_irq 뒤 다시 0 이 된다.
 *
 * 왜 이런 당연해 보이는 것을 시험하는가: depth 를 건드리는 경로가 여럿이라,
 * 어느 하나를 고치다 이 기본 왕복이 깨지는 일이 실제로 있었다. 가장 흔한
 * 경로를 고정해 두면 그런 회귀를 곧바로 잡는다.
 *
 * 실행 컨텍스트: KUnit 시험, 프로세스 문맥.
 *
 * 호출 체인:
 *   kunit 실행기 → [이 함수] → request_irq()/disable_irq()/enable_irq()
 */
static void irq_disable_depth_test(struct kunit *test)
{
	struct irq_desc *desc;	/* [한국어] depth 를 직접 읽기 위한 서술자 */
	int virq, ret;	/* [한국어] 인터럽트 번호와 요청 결과 */

	virq = irq_test_setup_fake_irq(test, NULL);	/* [한국어] 친화도 요구 없이 보통의 인터럽트를 준비한다 */

	desc = irq_to_desc(virq);	/* [한국어] depth 는 공개 API 로 읽을 수 없어 서술자를 직접 본다 */
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);	/* [한국어] 없으면 이후가 무의미하다 */

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);	/* [한국어] 핸들러를 등록한다. 플래그 0 이라 공유도 스레드도 아니다 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 요청이 성공해야 이후 시험이 성립한다 */

	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 요청 직후 자동으로 켜져 depth 가 0 이어야 한다. IRQ_NOAUTOEN 이 없으므로 */

	disable_irq(virq);	/* [한국어] 한 겹 끈다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] depth 가 1 이 된다 */

	enable_irq(virq);	/* [한국어] 다시 켠다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 0 으로 돌아와야 짝이 맞은 것이다 */

	free_irq(virq, NULL);	/* [한국어] 정리. dev_id 가 NULL 인 것은 요청 때도 NULL 이었기 때문이다 */
}

/*
 * [한국어]
 * irq_free_disabled_test - 비활성 상태에서 해제한 뒤 다시 요청했을 때를 확인한다
 *
 * @test: KUnit 시험 문맥.
 *
 * 무엇이 문제가 될 수 있는가: 드라이버가 disable_irq() 로 꺼 둔 채
 * free_irq() 를 부르면 depth 가 1 인 상태로 서술자가 남는다. 그 뒤 다른
 * 드라이버가 같은 번호를 요청하면, 그 드라이버는 인터럽트가 켜져 있다고
 * 믿는데 실제로는 꺼져 있다.
 *
 * 그래서 코어는 요청 경로에서 depth 를 0 으로 되돌려야 한다. 이 시험이
 * 그것을 확인한다.
 *
 * free 직후에 EQ 가 아니라 GE 를 쓰는 것에 주의한다. free_irq 가 마지막
 * 핸들러를 떼면서 인터럽트를 끄므로 depth 가 1 보다 클 수도 있다 —
 * 정확한 값이 아니라 "적어도 꺼져 있다" 만 확인하는 것이다.
 *
 * 실행 컨텍스트: KUnit 시험, 프로세스 문맥.
 *
 * 호출 체인:
 *   kunit 실행기 → [이 함수] → free_irq()/request_irq()
 */
static void irq_free_disabled_test(struct kunit *test)
{
	struct irq_desc *desc;	/* [한국어] depth 를 읽을 서술자 */
	int virq, ret;	/* [한국어] 인터럽트 번호와 요청 결과 */

	virq = irq_test_setup_fake_irq(test, NULL);	/* [한국어] 보통의 인터럽트를 준비한다 */

	desc = irq_to_desc(virq);	/* [한국어] 서술자를 찾는다 */
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);	/* [한국어] 서술자가 없으면 이후가 무의미하다 */

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);	/* [한국어] 첫 요청 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 요청이 성공해야 이후 시험이 성립한다 */

	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 자동으로 켜졌다 */

	disable_irq(virq);	/* [한국어] 끈 상태로 만든다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] 한 겹 꺼졌다 */

	free_irq(virq, NULL);	/* [한국어] 꺼진 채로 해제한다. 이것이 이 시험이 만들려는 상황이다 */
	KUNIT_EXPECT_GE(test, desc->depth, 1);	/* [한국어] EQ 가 아니라 GE 다 — free_irq 가 마지막 핸들러를 떼며 또 끄므로 1 보다 클 수 있다 */

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);	/* [한국어] 같은 번호를 다시 요청한다. 새 드라이버가 물려받는 상황을 흉내 낸다 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 요청 자체는 성공해야 한다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 핵심 검증 — 이전 드라이버가 남긴 비활성 상태가 지워지고 0 으로 초기화되어야 한다 */

	free_irq(virq, NULL);	/* [한국어] 정리 */
}

/*
 * [한국어]
 * irq_shutdown_depth_test - managed 인터럽트의 shutdown 과 복원을 확인한다
 *
 * @test: KUnit 시험 문맥.
 *
 * managed 인터럽트가 무엇인가: 커널이 친화도를 관리하는 인터럽트다. 대상
 * CPU 가 모두 오프라인이 되면 끄고(managed shutdown), 하나라도 돌아오면
 * 자동으로 되살린다.
 *
 * 이 시험은 CPU 를 실제로 내리지 않고 그 shutdown/복원 경로만 직접 부른다.
 * 아래 cpuhotplug 시험이 실제 CPU 로 같은 것을 확인하는 반면, 이쪽은
 * 코어 함수 수준에서 검증한다.
 *
 * 확인하는 것:
 *   요청 뒤 activated/started/managed 상태가 모두 참이다.
 *   shutdown 뒤 activated/started 가 거짓이 된다.
 *   activate + startup_managed 로 되살린 뒤 depth 가 1 로 남는다 —
 *     사용자가 disable_irq 로 꺼 둔 것은 복원되어도 유지되어야 한다.
 *   enable_irq 로 그것까지 풀면 0 이 된다.
 *
 * 세 번째가 이 시험의 핵심이다. managed 복원이 사용자의 disable 을
 * 무시하고 켜 버리면, 드라이버가 준비되지 않은 상태에서 인터럽트를 받는다.
 *
 * CONFIG_SMP 를 요구하는 이유: managed shutdown 은 CPU 가 여럿일 때만
 * 의미가 있고, irq_startup_managed() 자체가 SMP 빌드에만 있다.
 *
 * 실행 컨텍스트: KUnit 시험, 프로세스 문맥.
 *
 * 호출 체인:
 *   kunit 실행기 → [이 함수] → irq_shutdown_and_deactivate()/irq_activate()
 *     → irq_startup_managed() (kernel/irq/chip.c)
 */
static void irq_shutdown_depth_test(struct kunit *test)
{
	struct irq_desc *desc;	/* [한국어] depth 를 읽을 서술자 */
	struct irq_data *data;	/* [한국어] 상태 비트를 읽을 통로 */
	int virq, ret;	/* [한국어] 인터럽트 번호와 요청 결과 */
	/* [한국어] managed 인터럽트를 만들기 위한 친화도 요구.
	 *
	 * is_managed 가 1 이어야 커널이 친화도를 관리하는 인터럽트가 된다.
	 * mask 를 모든 CPU 로 두는 것은, 이 시험이 CPU 를 실제로 내리지 않아
	 * 어느 CPU 든 상관없기 때문이다. */
	struct irq_affinity_desc affinity = {
		.is_managed = 1,	/* [한국어] 커널이 친화도를 관리한다 — 사용자가 바꿀 수 없고, CPU 가 사라지면 자동으로 꺼진다 */
		.mask = CPU_MASK_ALL,	/* [한국어] 모든 CPU. 실제로 내리지 않으므로 범위는 중요하지 않다 */
	};

	if (!IS_ENABLED(CONFIG_SMP))	/* [한국어] 단일 프로세서 빌드인가 */
		kunit_skip(test, "requires CONFIG_SMP for managed shutdown");	/* [한국어] irq_startup_managed 자체가 SMP 빌드에만 있어 컴파일은 되지만 의미가 없다 */

	virq = irq_test_setup_fake_irq(test, &affinity);	/* [한국어] managed 인터럽트로 준비한다 */

	desc = irq_to_desc(virq);	/* [한국어] 서술자 */
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);	/* [한국어] 서술자가 없으면 이후가 무의미하다 */

	data = irq_desc_get_irq_data(desc);	/* [한국어] 상태 비트를 읽을 irq_data */
	KUNIT_ASSERT_PTR_NE(test, data, NULL);	/* [한국어] irq_data 가 없으면 상태를 읽을 수 없다 */

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);	/* [한국어] 핸들러 등록. 이 시점에 activate 와 startup 이 함께 일어난다 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 요청이 성공해야 이후 시험이 성립한다 */

	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));	/* [한국어] 하드웨어 자원이 배정되었다 */
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));	/* [한국어] 인터럽트가 시작되었다 */
	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));	/* [한국어] managed 로 표시되었다. 위 affinity.is_managed 가 여기까지 전달된 것이다 */

	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 자동으로 켜졌다 */

	disable_irq(virq);	/* [한국어] 사용자가 끈 상황을 만든다. 아래 복원이 이것을 존중해야 한다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] 한 겹 꺼졌다 */

	scoped_guard(raw_spinlock_irqsave, &desc->lock)	/* [한국어] 코어 내부 함수라 서술자 락을 직접 잡아야 한다. guard 라 블록을 벗어나면 풀린다 */
		irq_shutdown_and_deactivate(desc);	/* [한국어] CPU 가 모두 사라진 상황을 흉내 낸다. 끄고 하드웨어 자원까지 반납한다 */

	KUNIT_EXPECT_FALSE(test, irqd_is_activated(data));	/* [한국어] 자원이 반납되었다 */
	KUNIT_EXPECT_FALSE(test, irqd_is_started(data));	/* [한국어] 인터럽트가 멈췄다 */

	KUNIT_EXPECT_EQ(test, irq_activate(desc), 0);	/* [한국어] 자원을 다시 배정한다. CPU 가 돌아온 상황의 첫 단계다 */
#ifdef CONFIG_SMP	/* [한국어] 위 IS_ENABLED 검사와 달리 여기는 진짜 #ifdef 다 — 이 함수가 SMP 빌드에만 존재하기 때문이다 */
	irq_startup_managed(desc);	/* [한국어] managed 인터럽트를 되살린다. cpuhotplug.c 가 CPU 온라인 때 부르는 것과 같은 함수다 */
#endif

	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] 이 시험의 핵심 — 복원해도 사용자의 disable 은 유지되어야 한다. 0 이 되면 드라이버가 준비되지 않은 채 인터럽트를 받는다 */

	enable_irq(virq);	/* [한국어] 사용자가 켠다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 이제야 0 이 된다 */

	free_irq(virq, NULL);	/* [한국어] 정리 */
}

/*
 * [한국어]
 * irq_cpuhotplug_test - CPU 를 실제로 내리고 올려 managed 복원을 확인한다
 *
 * @test: KUnit 시험 문맥.
 *
 * 위 shutdown 시험이 코어 함수를 직접 불렀다면, 이쪽은 CPU 를 진짜로
 * 내리고 올려 전체 경로를 검증한다. 그래서 이 파일에서 가장 무거운 시험이다.
 *
 * 네 가지 전제 조건을 확인하고 하나라도 어긋나면 건너뛴다:
 *   SMP 빌드일 것.
 *   CPU 1 이 존재할 것.
 *   CPU 1 이 핫플러그 가능할 것 — 어떤 시스템은 부트 CPU 외에는 내릴 수 없다.
 *   CPU 1 이 지금 온라인일 것.
 *
 * 시나리오: CPU 1 에만 묶인 managed 인터럽트를 만들고, 사용자가 disable
 * 한 뒤, CPU 1 을 내렸다가 올린다. 그러면 인터럽트는 managed shutdown 을
 * 거쳐 자동 복원되는데, 그 과정에서 사용자의 disable 이 유지되어야 한다.
 *
 * 위 shutdown 시험과 같은 것을 확인하되, 실제 핫플러그 경로가 그 일을
 * 제대로 하는지까지 본다.
 *
 * 실행 컨텍스트: KUnit 시험, 프로세스 문맥. 실제로 CPU 를 내리므로
 * 시스템 전체에 영향이 있고, 시간도 오래 걸린다.
 *
 * 호출 체인:
 *   kunit 실행기 → [이 함수] → remove_cpu()/add_cpu()
 *     → CPU 핫플러그 상태 기계 → irq_migrate_all_off_this_cpu()
 *     → irq_affinity_online_cpu() (kernel/irq/cpuhotplug.c)
 */
static void irq_cpuhotplug_test(struct kunit *test)
{
	struct irq_desc *desc;	/* [한국어] depth 를 읽을 서술자 */
	struct irq_data *data;	/* [한국어] 상태 비트를 읽을 통로 */
	int virq, ret;	/* [한국어] 인터럽트 번호와 요청 결과 */
	/* [한국어] managed 친화도 요구. 위 shutdown 시험과 달리 mask 를 여기서
	 * 채우지 않는다 — 아래에서 CPU 1 하나만 담는다. */
	struct irq_affinity_desc affinity = {
		.is_managed = 1,	/* [한국어] 커널이 관리하는 인터럽트로 만든다 */
	};

	if (!IS_ENABLED(CONFIG_SMP))	/* [한국어] 단일 프로세서면 내릴 CPU 가 없다 */
		kunit_skip(test, "requires CONFIG_SMP for CPU hotplug");
	if (!get_cpu_device(1))	/* [한국어] CPU 1 이 존재하는가 */
		kunit_skip(test, "requires more than 1 CPU for CPU hotplug");
	if (!cpu_is_hotpluggable(1))	/* [한국어] 내릴 수 있는 CPU 인가. 어떤 시스템은 부트 CPU 외에는 내릴 수 없다 */
		kunit_skip(test, "CPU 1 must be hotpluggable");
	if (!cpu_online(1))	/* [한국어] 지금 켜져 있는가. 이미 꺼져 있으면 내릴 수 없다 */
		kunit_skip(test, "CPU 1 must be online");

	cpumask_copy(&affinity.mask, cpumask_of(1));	/* [한국어] CPU 1 하나만 담는다. 그 CPU 가 내려가면 이 인터럽트는 갈 곳이 없어져 managed shutdown 대상이 된다 */

	virq = irq_test_setup_fake_irq(test, &affinity);	/* [한국어] CPU 1 에만 묶인 managed 인터럽트를 준비한다 */

	desc = irq_to_desc(virq);	/* [한국어] 서술자 */
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);	/* [한국어] 서술자가 없으면 이후가 무의미하다 */

	data = irq_desc_get_irq_data(desc);	/* [한국어] 상태 비트를 읽을 irq_data */
	KUNIT_ASSERT_PTR_NE(test, data, NULL);	/* [한국어] irq_data 가 없으면 상태를 읽을 수 없다 */

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);	/* [한국어] 핸들러 등록 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 요청이 성공해야 이후 시험이 성립한다 */

	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));	/* [한국어] 자원이 배정되었다 */
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));	/* [한국어] 시작되었다 */
	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));	/* [한국어] managed 로 표시되었다 */

	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 자동으로 켜졌다 */

	disable_irq(virq);	/* [한국어] 사용자가 끈다. 핫플러그를 거쳐도 이 상태가 유지되어야 한다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] 한 겹 꺼졌다. 아래 핫플러그를 거쳐도 이 상태가 유지되어야 한다 */

	KUNIT_EXPECT_EQ(test, remove_cpu(1), 0);	/* [한국어] CPU 1 을 실제로 내린다. 그 과정에서 cpuhotplug.c 가 이 인터럽트를 managed shutdown 한다 */
	KUNIT_EXPECT_GE(test, desc->depth, 1);	/* [한국어] EQ 가 아니라 GE — shutdown 이 depth 를 더 올릴 수 있어 정확한 값이 아니라 "여전히 꺼져 있다" 만 본다 */
	KUNIT_EXPECT_EQ(test, add_cpu(1), 0);	/* [한국어] 다시 올린다. cpuhotplug.c 가 이 인터럽트를 자동으로 되살린다 */

	KUNIT_EXPECT_EQ(test, desc->depth, 1);	/* [한국어] 이 시험의 핵심 — 자동 복원이 사용자의 disable 을 존중해 depth 가 1 로 남아야 한다 */

	enable_irq(virq);	/* [한국어] 사용자가 켠다 */
	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));	/* [한국어] 복원 과정에서 자원이 다시 배정되었다 */
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));	/* [한국어] 다시 시작되었다 */
	KUNIT_EXPECT_EQ(test, desc->depth, 0);	/* [한국어] 이제야 완전히 켜졌다 */

	free_irq(virq, NULL);	/* [한국어] 정리 */
}

/* [한국어] 이 파일이 제공하는 시험 목록.
 *
 * KUNIT_CASE 가 함수 포인터와 그 이름을 짝지어 담는다. 이름은 시험 결과
 * 출력에 그대로 나타나므로, 어느 시험이 실패했는지 바로 알 수 있다.
 *
 * 마지막의 빈 항목 {} 이 목록의 끝 표식이다. KUnit 이 개수를 따로 받지
 * 않고 이 표식으로 끝을 안다. */
static struct kunit_case irq_test_cases[] = {
	KUNIT_CASE(irq_disable_depth_test),	/* [한국어] 기본 disable/enable 왕복 */
	KUNIT_CASE(irq_free_disabled_test),	/* [한국어] 비활성 상태에서 해제한 뒤의 재요청 */
	KUNIT_CASE(irq_shutdown_depth_test),	/* [한국어] managed shutdown 과 복원 (코어 함수 직접 호출) */
	KUNIT_CASE(irq_cpuhotplug_test),	/* [한국어] 실제 CPU 핫플러그를 통한 같은 검증 */
	{}	/* [한국어] 끝 표식. KUnit 이 개수를 따로 받지 않고 이것으로 끝을 안다 */
};

/* [한국어] 위 시험들을 묶은 스위트.
 *
 * name 이 시험 결과 출력의 머리말이 되고, kunit.py 같은 도구가 이 이름으로
 * 특정 스위트만 골라 실행할 수 있다. */
static struct kunit_suite irq_test_suite = {
	.name = "irq_test_cases",
	/* [한국어] 이 스위트의 이름.
	 * 설정자: 이 정적 초기화. 읽는 자: KUnit 실행기와 결과 출력.
	 * 도구가 이 이름으로 특정 스위트만 골라 실행할 수 있다. */

	.test_cases = irq_test_cases,
	/* [한국어] 위에서 정의한 시험 목록.
	 * 설정자: 이 정적 초기화. 읽는 자: KUnit 실행기가 순서대로 실행한다.
	 * init/exit 콜백이 없는 것에 주의 — 각 시험이 자기 자원을 스스로
	 *   준비하고 정리하므로 스위트 수준의 준비가 필요 없다. */
};

kunit_test_suite(irq_test_suite);	/* [한국어] 이 스위트를 KUnit 에 등록한다. 모듈로 빌드하면 로드 시에, 내장하면 부팅 중에 실행된다 */
MODULE_DESCRIPTION("IRQ unit test suite");	/* [한국어] modinfo 에 보일 설명 */
MODULE_LICENSE("GPL");	/* [한국어] 라이선스 선언. 이것이 없으면 모듈 로드 시 커널이 오염되었다고 표시한다 */
