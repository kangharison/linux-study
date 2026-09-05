// SPDX-License-Identifier: GPL-2.0-only

/*
 * [한국어 설명] 인터럽트 재매핑의 공통 진입점과 부팅 인자 처리 (irq_remapping.c)
 *
 * === 파일의 역할 ===
 * Intel VT-d, AMD-Vi, Hyper-V 세 인터럽트 재매핑 구현 위에 얹히는 얇은
 * 공통층이다. 하는 일이 셋이다 — 커널 명령줄 인자를 해석하고, 세 구현
 * 중 어느 것을 쓸지 고르고, 아키텍처 코드가 부르는 진입점을 갈고리표로
 * 넘긴다.
 *
 * 인터럽트 재매핑이란 장치가 인터럽트를 낼 때 실어 보내는 벡터와 대상
 * CPU 를 하드웨어가 표를 거쳐 바꿔치기하는 기능이다. 두 가지가 목적이다.
 * 하나는 안전 — 장치가 임의의 벡터를 쏘아 다른 CPU 의 중요한 인터럽트를
 * 흉내 내지 못하게 막는다. 다른 하나는 확장 — 8비트 벡터 공간의 한계를
 * 넘고, x2APIC 처럼 넓은 CPU 번호를 쓸 수 있게 한다.
 *
 * x86 전용 파일이다. 다른 아키텍처는 인터럽트 컨트롤러가 그 일을 직접
 * 맡거나 그런 개념이 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 순서에서 아주 이른 자리에 있다:
 *
 *   커널 명령줄 해석 → setup_nointremap()/setup_irqremap()  ← 이 파일
 *   APIC 초기화 준비 → irq_remapping_prepare()              ← 이 파일
 *     → 세 구현의 prepare() 를 차례로 시도해 하나를 고른다
 *   APIC 켜기        → irq_remapping_enable()               ← 이 파일
 *     → 고른 구현의 enable()
 *   CPU 가 올라올 때 → irq_remap_enable_fault_handling()     ← 이 파일
 *
 * 재매핑이 켜지면 x86 의 APIC 코드가 그 사실을 보고 x2APIC 사용 여부를
 * 정하고, 절전에서 깨어날 때 다시 켜는 일도 여기를 거친다.
 *
 * 실행 컨텍스트: 거의 모두 부팅 초기이거나 절전 복귀 — 다른 CPU 가
 * 멈춰 있거나 아직 올라오지 않은 상태다.
 *
 * === 타 모듈과의 연결 ===
 * - irq_remapping.h: 여기서 쓰는 갈고리표와 전역 변수의 선언.
 * - drivers/iommu/intel/irq_remapping.c: VT-d 구현.
 * - drivers/iommu/amd/: AMD-Vi 구현.
 * - drivers/iommu/hyperv-iommu.c: Hyper-V 게스트용 구현.
 * - arch/x86 의 APIC·부팅 코드: 이 파일의 진입점을 부르고,
 *   irq_remapping_enabled 를 보고 x2APIC 를 정한다.
 * - x86_apic_ops: 크래시 덤프로 넘어갈 때의 복구 갈고리를 이 파일이 바꿔 건다.
 *
 * === 주요 함수/구조체 요약 ===
 * - irq_remapping_prepare(): 세 구현을 차례로 시도해 하나를 고른다.
 *   이 함수 하나가 "어느 IOMMU 가 인터럽트를 재매핑할 것인가"를 정한다.
 * - irq_remapping_enable()/disable()/reenable(): 고른 구현의 갈고리로 넘긴다.
 * - setup_irqremap(): 쉼표로 이어진 부팅 인자를 하나씩 해석한다.
 * - irq_remapping_restore_boot_irq_mode(): 크래시 덤프 커널로 넘어갈 때,
 *   재매핑 표가 사라져도 인터럽트가 오도록 APIC 를 옛 방식으로 되돌린다.
 * - panic_if_irq_remap(): 재매핑이 켜진 상태에서는 성립할 수 없는 경로에
 *   놓아 두는 안전장치.
 */
#include <linux/cpumask.h>	/* [한국어] CPU 집합 — 인터럽트 대상 CPU 를 다루는 코드가 쓴다. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */
#include <linux/string.h>	/* [한국어] 부팅 인자를 문자열로 견줄 때 쓴다. */
#include <linux/errno.h>	/* [한국어] 오류 코드. */
#include <linux/msi.h>	/* [한국어] MSI 관련 형들. */
#include <linux/irq.h>	/* [한국어] 인터럽트 서술자. */
#include <linux/pci.h>	/* [한국어] PCI 장치의 인터럽트. */
#include <linux/irqdomain.h>	/* [한국어] 인터럽트 번호를 나눠 주는 계층. */

#include <asm/hw_irq.h>	/* [한국어] x86 의 하드웨어 인터럽트 정의. */
#include <asm/irq_remapping.h>	/* [한국어] 아키텍처 쪽에 노출되는 이 기능의 인터페이스. */
#include <asm/processor.h>	/* [한국어] boot_cpu_has 같은 CPU 기능 조회. */
#include <asm/x86_init.h>	/* [한국어] 아래에서 갈고리를 바꿔 거는 x86_apic_ops. */
#include <asm/apic.h>	/* [한국어] APIC 조작 — 부팅 인터럽트 방식을 되돌릴 때 쓴다. */
#include <asm/hpet.h>	/* [한국어] HPET 타이머의 인터럽트도 재매핑 대상이다. */

#include "irq_remapping.h"	/* [한국어] 세 구현이 나눠 쓰는 갈고리표와 전역 변수 선언. */

int irq_remapping_enabled;	/* [한국어] 재매핑이 실제로 켜졌는가. x86 의 APIC 코드가 이 값을 보고 x2APIC 사용 여부를 정한다. */
int irq_remap_broken;	/* [한국어] 펌웨어가 "이 시스템의 재매핑은 망가졌다"고 알려 온 경우 — 그때는 켜지 않는다. */
int disable_sourceid_checking;	/* [한국어] 인터럽트를 낸 장치가 그 표 항목의 주인이 맞는지 검사할지. 끄면 안전이 약해지지만, 그 검사를 못 견디는 하드웨어가 있다. */
int no_x2apic_optout;	/* [한국어] 펌웨어가 x2APIC 를 쓰지 말라고 해도 무시할지 — 그 선언이 틀린 플랫폼이 있다. */

int disable_irq_post = 0;	/* [한국어] 인터럽트 게시를 끌지. 가상 머신에게 인터럽트를 CPU 를 거치지 않고 직접 넣어 주는 최적화다. */

bool enable_posted_msi __ro_after_init;	/* [한국어] MSI 도 게시 방식으로 받을지. 초기화 뒤에는 읽기 전용이 되어 실수로도 바뀌지 않는다. */

static int disable_irq_remap;	/* [한국어] 재매핑을 아예 켜지 말라고 요청받았는가 — 부팅 인자로 정한다. */
static struct irq_remap_ops *remap_ops;	/* [한국어] 고른 구현의 갈고리표. NULL 이면 아직 고르지 못했거나 쓸 수 있는 구현이 없다. */

/*
 * [한국어]
 * irq_remapping_restore_boot_irq_mode - APIC 를 부팅 초기 방식으로 되돌린다
 *
 * 크래시 덤프 커널로 넘어갈 때 불린다. 왜 필요한가 — 지금 커널이 죽으면서
 * 재매핑 표가 담긴 메모리도 함께 믿을 수 없게 되는데, 새 커널이 올라올
 * 때까지 인터럽트가 그 표를 거쳐 오면 아무 데로나 갈 수 있다. 그래서
 * 표를 거치지 않는 옛 방식으로 되돌려 둔다.
 *
 * 주석이 밝히듯 가상 배선 A 방식을 쓴다 — B 방식은 IOAPIC 항목과 재매핑
 * 표를 함께 설정해야 해서 복잡한데, 크래시 도중에는 단순한 편이 안전하다.
 *
 * 실행 컨텍스트: 크래시 덤프 진입. 다른 CPU 는 이미 멈춰 있다.
 *
 * 호출 체인:
 *   크래시 경로 → x86_apic_ops.restore = [이 함수] → disconnect_bsp_APIC()
 */
static void irq_remapping_restore_boot_irq_mode(void)
{
	/*
	 * With interrupt-remapping, for now we will use virtual wire A
	 * mode, as virtual wire B is little complex (need to configure
	 * both IOAPIC RTE as well as interrupt-remapping table entry).
	 * As this gets called during crash dump, keep this simple for
	 * now.
	 */
	/* [한국어] (위 영어 주석 참고) 두 가지 되돌리기 방식 중 단순한 쪽을 쓴다.
	 * B 방식은 IOAPIC 항목과 재매핑 표를 함께 손봐야 하는데, 크래시 도중에
	 * 그 표를 믿을 수 없으므로 애초에 그것을 거치지 않는 A 방식이 낫다. */
	if (boot_cpu_has(X86_FEATURE_APIC) || apic_from_smp_config())	/* [한국어] APIC 가 있는 시스템일 때만 — 없으면 되돌릴 것도 없다. */
		disconnect_bsp_APIC(0);	/* [한국어] 부트 CPU 의 APIC 를 끊어 가상 배선 A 방식으로 되돌린다. 인자 0 이 A 방식을 뜻한다. */
}

/*
 * [한국어]
 * irq_remapping_modify_x86_ops - 크래시 복구 갈고리를 우리 것으로 바꿔 건다
 *
 * 재매핑이 실제로 켜진 뒤에만 부른다. 켜지지 않았다면 아키텍처의 기본
 * 복구 동작이 맞으므로 건드릴 이유가 없다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   irq_remapping_enable() → [이 함수]
 */
static void __init irq_remapping_modify_x86_ops(void)
{
	x86_apic_ops.restore = irq_remapping_restore_boot_irq_mode;	/* [한국어] 크래시 때 우리 방식으로 되돌리게 한다 — 재매핑 표를 믿을 수 없기 때문이다. */
}

/*
 * [한국어]
 * setup_nointremap - "nointremap" 부팅 인자를 처리한다
 *
 * @str: 인자 값 (이 인자는 값을 받지 않는다).
 * @return: 항상 0.
 *
 * 재매핑을 아예 켜지 말라는 요청이다. 재매핑이 문제를 일으키는지
 * 확인할 때나, 그 기능이 망가진 하드웨어에서 쓴다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들지 않는다.
 *
 * 호출 체인:
 *   커널 명령줄 해석 → [이 함수]
 */
static __init int setup_nointremap(char *str)
{
	disable_irq_remap = 1;	/* [한국어] 아래 prepare 가 이 값을 보고 곧바로 물러난다. */
	return 0;	/* [한국어] 해석을 끝냈다. */
}
early_param("nointremap", setup_nointremap);	/* [한국어] APIC 초기화보다 먼저 읽혀야 해서 이른 인자로 등록한다. */

/*
 * [한국어]
 * setup_irqremap - "intremap=" 부팅 인자를 처리한다
 *
 * @str: 쉼표로 이어진 값들.
 * @return: 0 성공, -EINVAL 값이 없음.
 *
 * 위 nointremap 보다 세밀한 조정을 한 인자에 모아 둔 것이다. 쉼표로
 * 이어진 여러 값을 앞에서부터 하나씩 견주며 해석한다.
 *
 * 모르는 값을 만나면 조용히 건너뛴다 — 아래 두 줄이 다음 쉼표까지
 * 건너뛰는 일을 하며, 일치하는 것이 없어도 그 동작은 같기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들지 않는다.
 *
 * 호출 체인:
 *   커널 명령줄 해석 → [이 함수]
 */
static __init int setup_irqremap(char *str)
{
	if (!str)	/* [한국어] 값 없이 "intremap" 만 적었다면. */
		return -EINVAL;	/* [한국어] 무엇을 하라는 것인지 알 수 없다. */

	while (*str) {	/* [한국어] 쉼표로 이어진 값들을 앞에서부터. */
		if (!strncmp(str, "on", 2)) {	/* [한국어] 명시적으로 켜라는 요청. */
			disable_irq_remap = 0;	/* [한국어] 재매핑을 켠다. */
			disable_irq_post = 0;	/* [한국어] 인터럽트 게시도 함께 살린다. */
		} else if (!strncmp(str, "off", 3)) {	/* [한국어] 끄라는 요청. */
			disable_irq_remap = 1;	/* [한국어] 재매핑을 끈다. */
			disable_irq_post = 1;	/* [한국어] 게시는 재매핑 위에 얹히는 기능이라 함께 꺼야 한다. */
		} else if (!strncmp(str, "nosid", 5))	/* [한국어] 출처 검사를 끄라는 요청. */
			disable_sourceid_checking = 1;	/* [한국어] 안전이 약해지지만, 그 검사를 못 견디는 하드웨어가 있다. */
		else if (!strncmp(str, "no_x2apic_optout", 16))	/* [한국어] 펌웨어의 x2APIC 거부를 무시하라는 요청. */
			no_x2apic_optout = 1;	/* [한국어] 그 선언이 틀린 플랫폼에서 쓴다. */
		else if (!strncmp(str, "nopost", 6))	/* [한국어] 게시만 끄라는 요청. */
			disable_irq_post = 1;	/* [한국어] 재매핑은 그대로 두고 게시만 끈다. */
		else if (IS_ENABLED(CONFIG_X86_POSTED_MSI) && !strncmp(str, "posted_msi", 10))	/* [한국어] 커널이 그 기능을 갖고 있을 때만. */
			enable_posted_msi = true;	/* [한국어] MSI 도 게시 방식으로 받는다 — 인터럽트 처리 부담이 줄어든다. */
		str += strcspn(str, ",");	/* [한국어] 다음 쉼표까지 건너뛴다 — 모르는 값도 이 줄 덕분에 조용히 넘어간다. */
		while (*str == ',')	/* [한국어] 쉼표가 이어져 있으면. */
			str++;	/* [한국어] 모두 건너뛴다 — 빈 항목을 허용한다. */
	}

	return 0;	/* [한국어] 표시를 세웠다. */
}
early_param("intremap", setup_irqremap);	/* [한국어] APIC 초기화보다 먼저 읽혀야 해서 이른 인자로 등록한다. */

/*
 * [한국어]
 * set_irq_remapping_broken - 이 시스템의 재매핑이 망가졌다고 표시한다
 *
 * 펌웨어가 알려 온 정보를 보고 아키텍처 코드가 부른다. 이 표시가 서면
 * 각 구현의 prepare 가 그것을 보고 물러난다.
 *
 * 실행 컨텍스트: 부팅 초기. 잠들지 않는다.
 *
 * 호출 체인:
 *   ACPI/DMI 검사 코드 → [이 함수]
 */
void set_irq_remapping_broken(void)
{
	irq_remap_broken = 1;	/* [한국어] 각 구현이 이 값을 보고 자기를 쓰지 않게 한다. */
}

/*
 * [한국어]
 * irq_remapping_cap - 고른 구현이 그 능력을 갖고 있는가
 *
 * @cap: 물어볼 능력.
 * @return: 지원하면 참.
 *
 * 지금은 인터럽트 게시 능력을 묻는 데 주로 쓰인다. KVM 이 게스트에게
 * 인터럽트를 직접 넣어 줄 수 있는지 판단할 때 이 답을 본다.
 *
 * 게시를 끄라고 했으면 능력과 무관하게 거짓을 돌려주는 것이 요점이다 —
 * 사용자 요청이 하드웨어 능력보다 우선한다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   KVM 등 → [이 함수]
 */
bool irq_remapping_cap(enum irq_remap_cap cap)
{
	if (!remap_ops || disable_irq_post)	/* [한국어] 쓸 구현이 없거나, 사용자가 게시를 끄라고 했다면. */
		return false;	/* [한국어] 능력과 무관하게 없다고 답한다. */

	return (remap_ops->capability & (1 << cap));	/* [한국어] 고른 구현이 그 비트를 세워 두었는지 본다. */
}
EXPORT_SYMBOL_GPL(irq_remapping_cap);	/* [한국어] KVM 이 모듈로 빌드될 수 있어 내보낸다. */

/*
 * [한국어]
 * irq_remapping_prepare - 세 구현 중 쓸 수 있는 것을 고른다
 *
 * @return: 0 하나를 골랐다, -ENOSYS 쓸 수 있는 구현이 없다.
 *
 * 이 파일에서 가장 중요한 함수다. Intel, AMD, Hyper-V 순으로 각 구현의
 * prepare 를 불러 보고, 처음으로 성공한 것을 쓴다. 각 구현이 자기
 * 하드웨어가 있는지 스스로 확인하므로, 여기서는 순서만 정해 주면 된다.
 *
 * 빌드에 포함된 구현만 시도하는 것도 요점이다 — IS_ENABLED 로 감싸
 * 없는 구현의 심볼을 참조하지 않게 한다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   x86 부팅 코드 → [이 함수] → 각 구현의 prepare()
 */
int __init irq_remapping_prepare(void)
{
	if (disable_irq_remap)	/* [한국어] 부팅 인자로 끄라고 했다면. */
		return -ENOSYS;	/* [한국어] 아무것도 시도하지 않는다. */

	if (IS_ENABLED(CONFIG_INTEL_IOMMU) &&	/* [한국어] 그 구현이 빌드에 있고. */
	    intel_irq_remap_ops.prepare() == 0)	/* [한국어] 자기 하드웨어를 찾았다면. */
		remap_ops = &intel_irq_remap_ops;	/* [한국어] 그것을 쓴다. */
	else if (IS_ENABLED(CONFIG_AMD_IOMMU) &&	/* [한국어] AMD 구현이 빌드에 있고 자기 하드웨어를 찾았다면. */
		 amd_iommu_irq_ops.prepare() == 0)
		remap_ops = &amd_iommu_irq_ops;	/* [한국어] AMD 하드웨어. */
	else if (IS_ENABLED(CONFIG_HYPERV_IOMMU) &&	/* [한국어] Hyper-V 구현이 빌드에 있고 하이퍼바이저를 찾았다면. */
		 hyperv_irq_remap_ops.prepare() == 0)
		remap_ops = &hyperv_irq_remap_ops;	/* [한국어] Hyper-V 게스트 — 하이퍼바이저가 재매핑을 대신한다. */
	else
		return -ENOSYS;	/* [한국어] 쓸 수 있는 구현이 없다 — 재매핑 없이 부팅한다. */

	return 0;	/* [한국어] 하나를 골랐다. */
}

/*
 * [한국어]
 * irq_remapping_enable - 고른 구현의 재매핑을 켠다
 *
 * @return: 그 구현이 돌려준 값, 또는 -ENODEV.
 *
 * 갈고리를 부르고, 실제로 켜졌으면 크래시 복구 갈고리도 바꿔 건다.
 * 켜졌는지는 반환값이 아니라 전역 변수로 판단하는데, 구현이 부분적으로만
 * 켜는 경우가 있어 그 사실을 자기가 표시하기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   x86 부팅 코드 → [이 함수] → remap_ops->enable()
 */
int __init irq_remapping_enable(void)
{
	int ret;	/* [한국어] 구현이 돌려준 값. */

	if (!remap_ops->enable)	/* [한국어] 고른 구현이 켜기 갈고리를 제공하지 않는다면. */
		return -ENODEV;

	ret = remap_ops->enable();	/* [한국어] 실제로 켠다 — 구현이 켜졌음을 전역 변수에 표시한다. */

	if (irq_remapping_enabled)	/* [한국어] 정말 켜졌다면. */
		irq_remapping_modify_x86_ops();	/* [한국어] 크래시 때 재매핑 표를 거치지 않게 복구 갈고리를 바꿔 건다. */

	return ret;	/* [한국어] 구현이 돌려준 값을 그대로 올린다. */
}

/*
 * [한국어]
 * irq_remapping_disable - 재매핑을 끈다
 *
 * 시스템을 재우거나 kexec 로 다음 커널을 띄우기 전에 불린다. 켜지지
 * 않았거나 구현이 끄기 갈고리를 제공하지 않으면 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 절전 진입·종료. 다른 CPU 는 멈춰 있다.
 *
 * 호출 체인:
 *   x86 절전·종료 코드 → [이 함수] → remap_ops->disable()
 */
void irq_remapping_disable(void)
{
	if (irq_remapping_enabled && remap_ops->disable)	/* [한국어] 켜져 있고 끄기 갈고리가 있을 때만. */
		remap_ops->disable();	/* [한국어] 하드웨어를 끈다. */
}

/*
 * [한국어]
 * irq_remapping_reenable - 절전에서 깨어난 뒤 재매핑을 다시 켠다
 *
 * @mode: 어떤 방식으로 되살릴지 (구현이 해석한다).
 * @return: 0 성공 또는 할 일 없음, 음수 실패.
 *
 * 절전 중에 하드웨어 상태가 사라졌을 수 있어, 깨어난 뒤 표와 설정을
 * 다시 세워야 한다. 그 일은 각 구현이 맡고 여기서는 넘기기만 한다.
 *
 * 실행 컨텍스트: 절전 복귀. 다른 CPU 는 아직 올라오지 않았다.
 *
 * 호출 체인:
 *   x86 절전 복귀 코드 → [이 함수] → remap_ops->reenable()
 */
int irq_remapping_reenable(int mode)
{
	if (irq_remapping_enabled && remap_ops->reenable)	/* [한국어] 켜져 있었고 되살리기 갈고리가 있을 때만. */
		return remap_ops->reenable(mode);

	return 0;	/* [한국어] 할 일이 없으면 성공으로 친다. */
}

/*
 * [한국어]
 * irq_remap_enable_fault_handling - 재매핑 오류를 받아 처리할 준비를 한다
 *
 * @return: 0 성공, -ENODEV 구현이 그 갈고리를 제공하지 않음.
 *
 * 오류 처리는 CPU 마다 준비해야 한다 — 오류 인터럽트를 그 CPU 에서 받을
 * 수 있어야 하기 때문이다. 그래서 CPU 핫플러그 콜백으로 등록해 두고,
 * 지금 도는 CPU 에 대해서는 곧바로 한 번 부른다.
 *
 * 재매핑이 켜지지 않았으면 성공으로 치는 것이 요점이다 — 처리할 오류가
 * 애초에 없으므로 실패로 볼 이유가 없다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   x86 부팅 코드 → [이 함수] → cpuhp_setup_state()
 */
int __init irq_remap_enable_fault_handling(void)
{
	if (!irq_remapping_enabled)	/* [한국어] 재매핑이 켜지지 않았다면. */
		return 0;	/* [한국어] 처리할 오류가 없으니 성공으로 친다. */

	if (!remap_ops->enable_faulting)	/* [한국어] 구현이 그 갈고리를 제공하지 않는다면. */
		return -ENODEV;

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "dmar:enable_fault_handling",	/* [한국어] 앞으로 올라올 CPU 마다 그 갈고리가 불리게 등록한다. */
			  remap_ops->enable_faulting, NULL);	/* [한국어] 내려갈 때의 콜백은 없다 — 오류 처리를 되돌릴 필요가 없다. */

	return remap_ops->enable_faulting(smp_processor_id());	/* [한국어] 지금 도는 CPU 는 등록으로 커버되지 않아 직접 부른다. */
}

/*
 * [한국어]
 * panic_if_irq_remap - 재매핑이 켜진 상태라면 커널을 멈춘다
 *
 * @msg: 멈추며 남길 메시지.
 *
 * 재매핑이 켜져 있으면 성립할 수 없는 코드 경로에 놓아 두는 안전장치다.
 * 그런 자리에 도달했다는 것은 가정이 깨졌다는 뜻이고, 그대로 진행하면
 * 인터럽트가 엉뚱한 곳으로 가 조용히 망가진다. 그럴 바에는 그 자리에서
 * 멈추는 편이 낫다는 판단이다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다.
 *
 * 호출 체인:
 *   x86 의 인터럽트 설정 경로 → [이 함수] → panic()
 */
void panic_if_irq_remap(const char *msg)
{
	if (irq_remapping_enabled)	/* [한국어] 재매핑이 켜진 상태라면 이 경로에 오면 안 된다. */
		panic(msg);	/* [한국어] 조용히 망가지는 것보다 그 자리에서 멈추는 편이 낫다. */
}
