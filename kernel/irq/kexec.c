// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] kexec 직전 모든 인터럽트를 잠재우는 코드 (kexec.c)
 *
 * === 파일의 역할 ===
 * kexec 로 새 커널을 띄우기 직전, 현재 커널이 켜 둔 모든 인터럽트를 끄고
 * 하드웨어를 조용한 상태로 되돌린다. 함수가 하나뿐인 아주 작은 파일이지만,
 * 이것이 없으면 새 커널이 부팅하는 도중 옛 설정의 인터럽트가 날아들어
 * 아직 핸들러가 없는 상태에서 처리되지 못한 인터럽트가 쌓인다.
 *
 * kexec 가 보통의 재부팅과 다른 점이 이 파일의 존재 이유다. 전원을 껐다
 * 켜면 하드웨어가 리셋되어 모든 인터럽트 설정이 초기화되지만, kexec 는
 * 하드웨어를 그대로 둔 채 커널 이미지만 바꿔치기한다. 그래서 옛 커널이
 * 남긴 인터럽트 컨트롤러 상태가 그대로 새 커널에 넘어간다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * kexec 의 마지막 단계에 있다:
 *
 *   kexec_load(2) 로 새 커널 이미지를 미리 메모리에 올려 둠
 *     ↓ reboot(LINUX_REBOOT_CMD_KEXEC)
 *   kernel_kexec()
 *     ↓ 장치 드라이버 종료, CPU 하나만 남기고 정지
 *   machine_shutdown() (아키텍처별)
 *     ↓
 *   machine_kexec_mask_interrupts()  ← **이 파일**
 *     ↓ 모든 인터럽트를 EOI 하고 shutdown
 *   machine_kexec() — 새 커널로 점프
 *
 * 실행 컨텍스트: 아주 특수하다. 다른 CPU 는 이미 멈췄고, 스케줄러도 장치
 * 드라이버도 더는 동작하지 않는다. 이 함수 뒤에는 새 커널로 점프하는 것
 * 말고 남은 일이 없다.
 *
 * === 타 모듈과의 연결 ===
 * 부르는 쪽: 아키텍처의 machine_kexec 구현(arm64, riscv 등)이 부른다.
 *   x86 은 자체 경로를 써서 이 함수를 쓰지 않는다 — 그래서 이 파일이
 *   CONFIG_GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD 등 일부 아키텍처에서만
 *   의미가 있는 구성을 다룬다.
 *
 * 부르는 곳: irq 코어의 두 함수뿐이다.
 *   irq_set_irqchip_state() — 가상화로 전달 중인 인터럽트의 활성 상태를 지운다.
 *   irq_shutdown()          — 인터럽트를 실제로 끈다.
 *
 * 데이터 흐름: 전역 인터럽트 서술자 목록을 훑으며 각각을 정리한다. 새로
 * 만들거나 넘기는 데이터는 없다 — 오직 하드웨어 상태를 되돌린다.
 *
 * === 주요 함수/구조체 요약 ===
 * machine_kexec_mask_interrupts() — 이 파일의 유일한 함수. 모든 서술자를
 *   훑으며 (1) 가상 머신에 전달 중인 활성 상태를 지우고, (2) 처리 중인
 *   인터럽트에 EOI 를 보내고, (3) 인터럽트를 끈다.
 *
 * 세 단계의 순서가 중요하다. EOI 를 보내지 않고 끄면 컨트롤러가 그 인터럽트를
 * 영원히 "처리 중"으로 여겨, 같은 우선순위의 인터럽트가 새 커널에서 막힌다.
 */

#include <linux/interrupt.h>	/* [한국어] 인터럽트 공개 API. irq_set_irqchip_state() 가 여기 선언되어 있다 */
#include <linux/irq.h>	/* [한국어] struct irq_chip 과 irqd_* 상태 접근자 */
#include <linux/irqdesc.h>	/* [한국어] struct irq_desc 와 irq_desc_get_chip() */
#include <linux/irqnr.h>	/* [한국어] for_each_irq_desc 순회 매크로 */

#include "internals.h"	/* [한국어] irq_shutdown() — 코어 내부 함수라 공개 헤더에 없다 */

/*
 * [한국어]
 * machine_kexec_mask_interrupts - kexec 직전 모든 인터럽트를 정리한다
 *
 * 인자도 반환값도 없다. 전역 인터럽트 서술자 전체를 대상으로 삼는다.
 *
 * 왜 필요한가: kexec 는 하드웨어를 리셋하지 않고 커널 이미지만 바꾼다.
 * 그래서 옛 커널이 켜 둔 인터럽트가 그대로 살아 있고, 새 커널이 자기
 * 인터럽트 컨트롤러를 초기화하기 전에 그것이 날아들면 처리할 핸들러가 없다.
 * 어떤 컨트롤러는 그런 인터럽트를 계속 재전송해 새 커널의 부팅을 막는다.
 *
 * 동작 과정:
 *   1. 모든 서술자를 훑는다.
 *   2. chip 이 없거나 아직 시작되지 않은 것은 건너뛴다 — 정리할 상태가 없다.
 *   3. (구성에 따라) 가상 머신으로 전달 중인 인터럽트의 활성 상태를 먼저 지운다.
 *   4. 그것이 안 되면 처리 중인 인터럽트에 EOI 를 보낸다.
 *   5. 인터럽트를 끈다.
 *
 * 3 과 4 가 배타적인 것이 이 함수의 핵심 논리다. 자세한 이유는 아래
 * check_eoi 관련 주석에 있다.
 *
 * 실행 컨텍스트: 다른 CPU 가 모두 멈춘 뒤의 단일 CPU 문맥. 스케줄러도
 * 드라이버도 동작하지 않는다. 서술자 락을 잡지 않는데, 경쟁할 상대가
 * 없기 때문이다.
 *
 * 에러 처리: 없다. 이 시점에서 물러설 곳이 없어, 실패하더라도 그대로 진행한다.
 *
 * 호출 체인:
 *   kernel_kexec() → machine_shutdown() (아키텍처별)
 *     → [machine_kexec_mask_interrupts] → irq_shutdown()
 */
void machine_kexec_mask_interrupts(void)
{
	struct irq_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned int i;	/* [한국어] 그 인터럽트 번호. 아래 irq_set_irqchip_state() 가 번호를 받는다 */

	for_each_irq_desc(i, desc) {	/* [한국어] 시스템의 모든 인터럽트 서술자를 훑는다. SPARSE_IRQ 면 트리를, 아니면 배열을 돈다 */
		struct irq_chip *chip;	/* [한국어] 이 인터럽트를 담당하는 컨트롤러 */
		int check_eoi = 1;	/* [한국어] EOI 를 보낼 필요가 있는지. 기본은 "보낸다"이고, 아래에서 VM 전달 해제에 성공하면 0 이 된다 */

		chip = irq_desc_get_chip(desc);	/* [한국어] 서술자에서 컨트롤러를 꺼낸다 */
		if (!chip || !irqd_is_started(&desc->irq_data))	/* [한국어] 컨트롤러가 없거나(매핑만 되고 쓰이지 않는 번호) 아직 시작된 적이 없으면 */
			continue;	/* [한국어] 정리할 하드웨어 상태가 없으므로 건너뛴다 */

		if (IS_ENABLED(CONFIG_GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD)) {	/* [한국어] 가상화 전달 인터럽트를 다뤄야 하는 구성인가. IS_ENABLED 라 컴파일 시 상수로 접힌다 */
			/*
			 * First try to remove the active state from an interrupt which is forwarded
			 * to a VM. If the interrupt is not forwarded, try to EOI the interrupt.
			 */
			/* [한국어] (위 영어 주석에 이어)
			 * 왜 이 순서인가: 가상 머신에 직접 전달되는 인터럽트(ARM GIC 의
			 * forwarded interrupt)는 호스트가 EOI 를 보낼 수 없다. 그 인터럽트의
			 * 완료는 게스트가 처리하기로 되어 있고, 지금 게스트는 이미 멈췄다.
			 *
			 * 그래서 그런 인터럽트는 EOI 대신 "활성 상태"를 직접 지워야 한다.
			 * 그것이 성공하면(반환값 0) EOI 는 필요 없어진다.
			 *
			 * 전달 중이 아닌 보통의 인터럽트에 대해서는 이 호출이 실패하고,
			 * check_eoi 가 0 이 아닌 값으로 남아 아래 EOI 경로로 넘어간다.
			 * 즉 반환값이 그대로 "EOI 가 필요한가"라는 판정이 된다. */
			check_eoi = irq_set_irqchip_state(i, IRQCHIP_STATE_ACTIVE, false);	/* [한국어] 활성 상태를 내려 본다. 성공하면 0 이 되어 아래 EOI 를 건너뛴다 */
		}

		if (check_eoi && chip->irq_eoi && irqd_irq_inprogress(&desc->irq_data))	/* [한국어] 세 조건이 모두 맞을 때만 EOI: 위에서 처리되지 않았고, 컨트롤러가 EOI 방식이고, 지금 처리 중인 인터럽트가 있다 */
			chip->irq_eoi(&desc->irq_data);	/* [한국어] 완료를 알린다. 이것을 빠뜨리면 컨트롤러가 그 인터럽트를 계속 처리 중으로 여겨, 새 커널에서 같은 우선순위가 막힌다 */

		irq_shutdown(desc);	/* [한국어] 인터럽트를 끈다. 여기서 chip->irq_shutdown 이나 mask 가 불려 하드웨어가 조용해진다 */
	}
}
