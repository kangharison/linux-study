// SPDX-License-Identifier: GPL-2.0
// Copyright 2017 Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
/*
 * [한국어 설명] 인터럽트 내부 상태를 debugfs 로 들여다보는 창 (debugfs.c)
 *
 * === 파일의 역할 ===
 * /sys/kernel/debug/irq/irqs/<번호> 파일을 통해 인터럽트 하나의 모든 내부
 * 상태를 사람이 읽을 수 있는 형태로 보여 준다. 그리고 그 파일에 쓰기로
 * 인터럽트를 인위 발생시킬 수도 있다.
 *
 * 왜 /proc/interrupts 로 부족한가: 그쪽은 인터럽트 횟수만 보여 준다.
 * "인터럽트가 오지 않는다" 는 문제를 진단하려면 그것으로는 아무것도 알 수
 * 없다. 마스크되어 있는가, activate 되었는가, 어느 도메인 계층에 어떻게
 * 매핑되었는가, 친화도는 어디를 가리키는가 — 그 모든 것을 이 파일이 보여 준다.
 *
 * 출력의 핵심은 비트 이름 풀이다. 상태 워드를 16진수로 찍으면 사람이
 * 해석할 수 없으므로, 비트마다 이름을 붙인 표를 두고 서 있는 것들의
 * 이름을 나열한다. 그 표가 이 파일의 절반을 차지한다.
 *
 * 세 개의 상태 워드를 각각 다른 표로 푼다는 점이 중요하다:
 *   status_use_accessors (_IRQ_*)  — 설정 비트. irqdesc_states 표.
 *   istate (IRQS_*)                — 코어 처리 상태. irqdesc_istates 표.
 *   irq_data 의 state (IRQD_*)     — 하드웨어 쪽 상태. irqdata_states 표.
 * 이 셋의 구분이 인터럽트 서브시스템을 읽을 때 가장 헷갈리는 부분이고,
 * 이 파일의 출력이 그 구분을 눈으로 보여 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진단 계층에 있으며, 인터럽트 처리 경로에는 관여하지 않는다:
 *
 *   부팅
 *     ↓ __initcall
 *   irq_debugfs_init()               ← **이 파일** — 디렉터리를 만들고
 *     ↓ 기존 인터럽트마다
 *   irq_add_debugfs_entry()          ← **이 파일**
 *     ↓ 이후 새로 만들어지는 인터럽트는
 *   irqdesc.c 의 서술자 생성 → irq_add_debugfs_entry()
 *
 *   사용자가 읽을 때:
 *     cat /sys/kernel/debug/irq/irqs/42
 *       ↓
 *     irq_debug_show()               ← **이 파일**
 *
 *   사용자가 "trigger" 를 쓸 때:
 *     echo trigger > .../irqs/42
 *       ↓
 *     irq_debug_write() → irq_inject_interrupt() (resend.c)
 *
 * 실행 컨텍스트: 전부 프로세스 문맥. 읽기는 서술자 락을 잡고, 쓰기는
 * 인터럽트를 실제로 발생시킨다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   internals.h — irq_bit_descr 구조와 BIT_MASK_DESCR 매크로, irqd_get().
 *   resend.c 의 irq_inject_interrupt() — 쓰기로 인터럽트를 발생시킨다.
 *   irqdomain.c 의 irq_domain_debugfs_init() — 도메인 쪽 항목.
 *
 * 이 파일에 의존하는 곳:
 *   irqdesc.c — 서술자를 만들 때 항목을 추가하고 없앨 때 지운다.
 *   manage.c — 요청할 때 장치 이름을 복사해 둔다.
 *
 * === 주요 함수/구조체 요약 ===
 * irq_dir                — /sys/kernel/debug/irq/irqs 디렉터리.
 * irq_debug_show_bits()  — 상태 워드에서 서 있는 비트의 이름을 찍는다.
 * irq_debug_show_masks() — 세 종류의 CPU 마스크를 찍는다.
 * irqchip_flags 등 네 표 — 비트와 이름의 대응.
 * irq_debug_show_chip()  — chip 정보를 찍는다.
 * irq_debug_show_data()  — irq_data 를 찍고, 계층이면 재귀한다.
 * irq_debug_show()       — 파일을 읽을 때의 본체.
 * irq_debug_write()      — "trigger" 쓰기로 인터럽트를 발생시킨다.
 * irq_debugfs_copy_devname() — 장치 이름을 복사해 둔다.
 * irq_add_debugfs_entry()— 인터럽트 하나의 파일을 만든다.
 * irq_debugfs_init()     — 부팅 때 디렉터리를 만들고 기존 것들을 등록한다.
 *
 * 들여쓰기(ind)를 인자로 전달하는 방식이 이 파일의 출력 형식을 만든다.
 * 계층형 도메인에서 부모로 올라갈 때마다 4칸씩 들여써, 계층 구조가
 * 눈으로 드러난다.
 */

#include <linux/irqdomain.h>	/* [한국어] struct irq_domain 과 그 ops->debug_show 콜백 */
#include <linux/irq.h>	/* [한국어] IRQCHIP_ 과 IRQD_ 계열 상수와 struct irq_data */
#include <linux/uaccess.h>	/* [한국어] copy_from_user — 아래 write 가 사용자 버퍼를 읽는다 */

#include "internals.h"	/* [한국어] irq_bit_descr, BIT_MASK_DESCR, irqd_get, IRQS_* 등 코어 내부 */

static struct dentry *irq_dir;	/* [한국어] /sys/kernel/debug/irq/irqs 디렉터리. 아래 init 이 만들고 add_debugfs_entry 가 그 아래에 파일을 만든다. NULL 이면 debugfs 가 아직 준비되지 않았다는 뜻이라 항목 추가를 건너뛴다 */

/*
 * [한국어]
 * irq_debug_show_bits - 상태 워드에서 서 있는 비트들의 이름을 출력한다
 *
 * @m:     출력할 seq_file.
 * @ind:   들여쓰기 칸 수. 계층 구조를 표현하는 데 쓴다.
 * @state: 검사할 상태 워드.
 * @sd:    비트와 이름을 짝지은 표.
 * @size:  그 표의 항목 수.
 *
 * 이 파일의 출력을 사람이 읽을 수 있게 만드는 핵심 함수다. 16진수 워드를
 * 그대로 찍으면 아무도 해석할 수 없지만, 서 있는 비트의 이름을 나열하면
 * 곧바로 읽힌다.
 *
 * ind + 12 로 들여쓰는 것이 출력 형식을 만든다. 앞줄에 "status:   0x..."
 * 같은 헤더가 나오고, 그 아래 비트 이름들이 더 들여써져 종속 관계가 드러난다.
 *
 * 표에 없는 비트는 조용히 무시된다. 새 상태 비트가 추가되었는데 표에
 * 넣지 않으면 그 비트는 출력에 나타나지 않는다 — 진단할 때 주의할 점이다.
 * 다만 헤더의 16진수 값에는 나타나므로 완전히 사라지지는 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 서술자 락을 쥐고 있다.
 *
 * 호출 체인:
 *   irq_debug_show()/irq_debug_show_chip() → [이 함수] → seq_printf()
 */
void irq_debug_show_bits(struct seq_file *m, int ind, unsigned int state,
			 const struct irq_bit_descr *sd, int size)
{
	int i;	/* [한국어] 표 순회 변수 */

	for (i = 0; i < size; i++, sd++) {	/* [한국어] 첨자와 포인터를 함께 진행시킨다 */
		if (state & sd->mask)	/* [한국어] 이 비트가 서 있는가 */
			seq_printf(m, "%*s%s\n", ind + 12, "", sd->name);	/* [한국어] %*s 로 폭만큼 공백을 찍어 들여쓰고 이름을 쓴다. 12 를 더하는 것은 헤더보다 안쪽으로 들여쓰기 위해서다 */
	}
}

/* [한국어] CPU 마스크 출력. 단일 프로세서 빌드에서는 보여 줄 것이 없다.
 *
 * 세 종류의 마스크를 각각 찍으며, 각각 다른 CONFIG 에 걸려 있다.
 * 그 셋의 차이가 친화도 문제를 진단할 때 결정적이다 — 아래 함수 주석 참고. */
#ifdef CONFIG_SMP	/* [한국어] 다중 프로세서 빌드 */
/*
 * [한국어]
 * irq_debug_show_masks - 이 인터럽트의 CPU 마스크들을 출력한다
 *
 * @m:    출력할 seq_file.
 * @desc: 대상 서술자.
 *
 * 세 종류의 마스크를 찍는데, 그 차이를 아는 것이 친화도 문제 진단의 핵심이다.
 *
 *   affinity  — 사용자나 커널이 "여기로 보내라" 고 요청한 집합.
 *   effectiv  — 하드웨어가 실제로 고른 CPU. 대개 하나뿐이다.
 *   pending   — 미뤄 둔 이동의 목표. 아직 적용되지 않은 요청이다.
 *
 * 왜 셋이 다를 수 있는가: 사용자가 CPU 0~3 을 요청해도(affinity) 컨트롤러가
 * 하나만 고를 수 있으면 실제로는 CPU 1 로만 간다(effectiv). 그리고 방금
 * CPU 2 로 옮기라고 요청했지만 아직 적용되지 않았다면 그것이 pending 에 있다.
 *
 * "친화도를 바꿨는데 인터럽트가 여전히 옛 CPU 로 온다" 는 문제를 만나면
 * 이 세 줄을 비교하는 것이 첫 단계다.
 *
 * %*pbl 형식이 cpumask 를 "0-3,7" 같은 사람이 읽는 형태로 찍어 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_debug_show() → [이 함수]
 */
static void irq_debug_show_masks(struct seq_file *m, struct irq_desc *desc)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);	/* [한국어] 마스크들이 담긴 곳 */
	const struct cpumask *msk;	/* [한국어] 찍을 마스크. 세 번 재사용한다 */

	msk = irq_data_get_affinity_mask(data);	/* [한국어] 요청된 목적지 집합 */
	seq_printf(m, "affinity: %*pbl\n", cpumask_pr_args(msk));	/* [한국어] %*pbl 이 "0-3,7" 형태로 찍는다. 매크로가 폭과 포인터 두 인자를 함께 넘겨 준다 */
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK	/* [한국어] 유효 친화도를 따로 추적하는 빌드 */
	msk = irq_data_get_effective_affinity_mask(data);	/* [한국어] 하드웨어가 실제로 고른 CPU */
	seq_printf(m, "effectiv: %*pbl\n", cpumask_pr_args(msk));	/* [한국어] 위 affinity 와 다르면 컨트롤러가 일부만 골랐다는 뜻이다 */
#endif
#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 친화도 변경을 미룰 수 있는 아키텍처 */
	msk = desc->pending_mask;	/* [한국어] 미뤄 둔 이동의 목표 */
	seq_printf(m, "pending:  %*pbl\n", cpumask_pr_args(msk));	/* [한국어] 비어 있지 않으면 아직 적용되지 않은 친화도 변경이 있다는 뜻이다 */
#endif
}
#else
/*
 * [한국어]
 * irq_debug_show_masks - (단일 프로세서) 보여 줄 마스크가 없다
 *
 * @m:    출력할 seq_file. 쓰이지 않는다.
 * @desc: 대상 서술자. 쓰이지 않는다.
 *
 * CPU 가 하나뿐이면 친화도라는 개념이 성립하지 않는다. 모든 인터럽트가
 * 그 하나의 CPU 로 갈 수밖에 없어 보여 줄 것이 없다.
 *
 * 빈 함수로 두어 호출부에 #ifdef 를 심지 않는다. 이 파일과 이 서브시스템
 * 전체에서 반복되는 관용구다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_debug_show() → [이 빈 구현]
 */
static void irq_debug_show_masks(struct seq_file *m, struct irq_desc *desc) { }
#endif

/* [한국어] irq_chip 의 성질 플래그와 그 이름의 대응표.
 *
 * BIT_MASK_DESCR 이 상수 이름을 문자열화해 담으므로, 이름과 출력이
 * 어긋날 수 없다. 새 플래그가 추가되면 여기에 한 줄만 더하면 된다.
 *
 * 이 표가 보여 주는 플래그들은 컨트롤러의 성질이라, "이 하드웨어가 왜
 * 이렇게 동작하는가" 를 설명해 준다. 예를 들어 MASK_ON_SUSPEND 가 있으면
 * 절전 때 실제로 마스크된다는 뜻이다. */
static const struct irq_bit_descr irqchip_flags[] = {
	BIT_MASK_DESCR(IRQCHIP_SET_TYPE_MASKED),	/* [한국어] 트리거 방식을 바꾸려면 먼저 마스크해야 하는 하드웨어 */
	BIT_MASK_DESCR(IRQCHIP_EOI_IF_HANDLED),	/* [한국어] 실제로 처리했을 때만 EOI 를 보내야 하는 하드웨어 */
	BIT_MASK_DESCR(IRQCHIP_MASK_ON_SUSPEND),	/* [한국어] 절전 때 깨우기가 아닌 인터럽트를 실제로 마스크해야 한다. pm.c 가 이것을 본다 */
	BIT_MASK_DESCR(IRQCHIP_ONOFFLINE_ENABLED),	/* [한국어] CPU 핫플러그 때 enable/disable 콜백을 불러야 하는 하드웨어 */
	BIT_MASK_DESCR(IRQCHIP_SKIP_SET_WAKE),	/* [한국어] irq_set_wake 콜백이 없어도 오류로 다루지 말라. dummychip 등이 쓴다 */
	BIT_MASK_DESCR(IRQCHIP_ONESHOT_SAFE),	/* [한국어] ONESHOT 스레드 인터럽트에서 별도 마스킹이 필요 없는 하드웨어 */
	BIT_MASK_DESCR(IRQCHIP_EOI_THREADED),	/* [한국어] EOI 를 스레드 핸들러가 끝난 뒤에 보내야 하는 하드웨어 */
	BIT_MASK_DESCR(IRQCHIP_SUPPORTS_LEVEL_MSI),	/* [한국어] 레벨 트리거 MSI 를 지원한다. 보통의 MSI 는 엣지다 */
	BIT_MASK_DESCR(IRQCHIP_SUPPORTS_NMI),	/* [한국어] 이 컨트롤러의 인터럽트를 NMI 로 쓸 수 있다 */
	BIT_MASK_DESCR(IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND),	/* [한국어] 깨우기 인터럽트가 켜져 있어야만 깨울 수 있다. pm.c 가 그래서 일부러 켜 준다 */
	BIT_MASK_DESCR(IRQCHIP_IMMUTABLE),	/* [한국어] 이 chip 구조체를 실행 중에 고치면 안 된다. 여러 인터럽트가 공유하는 정적 자료라는 선언이다 */
	BIT_MASK_DESCR(IRQCHIP_MOVE_DEFERRED),	/* [한국어] 친화도 변경을 미뤄야 하는 하드웨어. migration.c 의 전체 구조가 이 플래그 하나 때문에 존재한다 */
};

/*
 * [한국어]
 * irq_debug_show_chip - 이 인터럽트의 컨트롤러 정보를 출력한다
 *
 * @m:    출력할 seq_file.
 * @data: 그 계층의 irq_data.
 * @ind:  들여쓰기 칸 수. 계층이 깊을수록 커진다.
 *
 * chip 의 이름과 플래그를 찍는다. 계층형 도메인에서는 각 단계마다 다른
 * chip 이 있으므로, 아래 show_data 가 재귀하며 이 함수를 여러 번 부른다.
 *
 * irq_print_chip 콜백이 있으면 그것에 맡기는 것이 요점이다. 어떤 컨트롤러는
 * 이름만으로 부족하다 — 예를 들어 GIC 는 어느 인스턴스인지, MSI 도메인은
 * 어느 장치의 것인지를 함께 보여 줘야 진단에 쓸모가 있다.
 *
 * chip 이 NULL 인 경우를 따로 다루는 이유: 매핑만 되고 컨트롤러가 아직
 * 꽂히지 않은 인터럽트가 있을 수 있다. 그때 널 역참조 대신 "None" 을 찍는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_debug_show_data() → [이 함수] → chip->irq_print_chip 또는 seq_printf
 */
static void
irq_debug_show_chip(struct seq_file *m, struct irq_data *data, int ind)
{
	struct irq_chip *chip = data->chip;	/* [한국어] 이 계층의 컨트롤러 */

	if (!chip) {	/* [한국어] 컨트롤러가 꽂히지 않은 인터럽트인가 */
		seq_printf(m, "chip: None\n");	/* [한국어] 널 역참조 대신 명시적으로 알린다. 들여쓰기를 하지 않는 것은 사소한 비일관이다 */
		return;
	}
	seq_printf(m, "%*schip:    ", ind, "");	/* [한국어] 헤더를 찍되 줄바꿈은 하지 않는다 — 아래에서 이름을 이어 붙인다 */
	if (chip->irq_print_chip)	/* [한국어] 컨트롤러가 자체 출력을 제공하는가 */
		chip->irq_print_chip(data, m);	/* [한국어] 이름만으로 부족한 경우다. GIC 라면 어느 인스턴스인지, MSI 라면 어느 장치의 것인지를 함께 찍는다 */
	else
		seq_printf(m, "%s", chip->name);	/* [한국어] 없으면 이름만 */
	seq_printf(m, "\n%*sflags:   0x%lx\n", ind + 1, "", chip->flags);	/* [한국어] 위 줄을 마치고 플래그를 16진수로. 아래 이름 풀이와 함께 보면 완전해진다 */
	irq_debug_show_bits(m, ind, chip->flags, irqchip_flags,	/* [한국어] 서 있는 플래그의 이름을 나열한다 */
			    ARRAY_SIZE(irqchip_flags));	/* [한국어] 표의 크기. 표와 크기를 함께 넘기는 것이 이 파일의 관례다 */
}

/*
 * [한국어]
 * irq_debug_show_data - irq_data 하나를 출력하고, 계층이면 부모까지 재귀한다
 *
 * @m:    출력할 seq_file.
 * @data: 출력할 irq_data.
 * @ind:  들여쓰기 칸 수.
 *
 * 계층형 도메인의 구조를 눈으로 보여 주는 함수다. 한 단계를 찍고 부모가
 * 있으면 들여쓰기를 4칸 늘려 재귀한다. 그 결과 출력이 이렇게 된다:
 *
 *   domain:  MSI
 *   hwirq:   0x0
 *   chip:    PCI-MSI
 *       parent:
 *       domain:  INTEL-IR
 *       hwirq:   0x1
 *       chip:    IR-PCI-MSI
 *           parent:
 *           domain:  VECTOR
 *           ...
 *
 * PCI MSI 인터럽트 하나가 세 단계로 겹쳐 있다는 것이 한눈에 드러난다.
 * 어느 단계에서 매핑이 잘못되었는지 찾을 때 이 출력이 결정적이다.
 *
 * 도메인의 debug_show 콜백을 부르는 이유: 도메인마다 보여 줄 것이 다르다.
 * MSI 도메인이라면 어느 장치의 몇 번째 벡터인지 같은 정보를 더한다.
 *
 * 계층이 없는 빌드에서는 재귀 부분이 통째로 컴파일되지 않아, 한 단계만
 * 찍고 끝난다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 서술자 락을 쥔 상태.
 *
 * 호출 체인:
 *   irq_debug_show() → [이 함수] → irq_debug_show_chip()
 *     → (재귀) [이 함수]
 */
static void
irq_debug_show_data(struct seq_file *m, struct irq_data *data, int ind)
{
	seq_printf(m, "%*sdomain:  %s\n", ind, "",	/* [한국어] 이 단계의 도메인 이름 */
		   data->domain ? data->domain->name : "");	/* [한국어] 도메인 없이 매핑된 인터럽트도 있어 널 검사를 한다 */
	seq_printf(m, "%*shwirq:   0x%lx\n", ind + 1, "", data->hwirq);	/* [한국어] 이 단계에서의 하드웨어 번호. 계층마다 다르다는 것이 이 출력의 요점이다 */
	irq_debug_show_chip(m, data, ind + 1);	/* [한국어] 이 단계의 컨트롤러 정보 */
	if (data->domain && data->domain->ops && data->domain->ops->debug_show)	/* [한국어] 도메인이 자체 출력을 제공하는가. 세 단계의 널 검사가 필요하다 */
		data->domain->ops->debug_show(m, NULL, data, ind + 1);	/* [한국어] 도메인별 정보. MSI 라면 어느 장치의 몇 번째 벡터인지 등을 더한다 */
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형 도메인을 쓰는 빌드 */
	if (!data->parent_data)	/* [한국어] 최상위 단계인가 */
		return;	/* [한국어] 더 올라갈 곳이 없다 */
	seq_printf(m, "%*sparent:\n", ind + 1, "");	/* [한국어] 부모 단계가 시작됨을 알리는 머리말 */
	irq_debug_show_data(m, data->parent_data, ind + 4);	/* [한국어] 4칸 더 들여써 재귀한다. 그 들여쓰기가 계층 구조를 눈으로 보여 준다 */
#endif
}

/* [한국어] irq_data 의 상태 비트(IRQD_*)와 이름의 대응표.
 *
 * 이 파일의 세 상태 표 중 가장 크고, 인터럽트의 "지금 상태" 를 가장 많이
 * 말해 준다. 빈 줄로 나뉜 묶음마다 성격이 다르다:
 *
 *   트리거 종류 — 엣지인지 레벨인지, 극성은 무엇인지.
 *   수명 주기   — activated, started, disabled, masked, inprogress.
 *   성질        — per-CPU 인지, 친화도를 바꿀 수 있는지.
 *   친화도 관련 — 설정 여부, 대기 중인 변경, managed 여부, shutdown 상태.
 *   가상화      — 게스트 vCPU 로 직접 전달되는지.
 *   깨우기      — 깨우기 원천으로 지정되었는지, 무장되었는지.
 *   기타        — 기본 트리거 설정 여부, 문맥 강제, 절전 중 활성화 등.
 *
 * 진단할 때 가장 자주 보는 것이 두 번째 묶음이다. activated 가 없으면
 * 하드웨어 자원이 배정되지 않은 것이고, started 가 없으면 켜지지 않은
 * 것이며, masked 가 있으면 하드웨어가 실제로 막고 있다는 뜻이다. */
static const struct irq_bit_descr irqdata_states[] = {
	BIT_MASK_DESCR(IRQ_TYPE_EDGE_RISING),	/* [한국어] 상승 엣지 트리거 */
	BIT_MASK_DESCR(IRQ_TYPE_EDGE_FALLING),	/* [한국어] 하강 엣지 트리거 */
	BIT_MASK_DESCR(IRQ_TYPE_LEVEL_HIGH),	/* [한국어] High 레벨 트리거 */
	BIT_MASK_DESCR(IRQ_TYPE_LEVEL_LOW),	/* [한국어] Low 레벨 트리거 */
	BIT_MASK_DESCR(IRQD_LEVEL),	/* [한국어] 레벨 트리거임을 요약한 비트. 위 네 개가 극성까지 담는 반면 이것은 흐름 제어 선택의 근거다 */

	BIT_MASK_DESCR(IRQD_ACTIVATED),	/* [한국어] 하드웨어 자원(벡터 등)이 배정되었다. 없으면 인터럽트가 도달할 곳이 없다 */
	BIT_MASK_DESCR(IRQD_IRQ_STARTED),	/* [한국어] 인터럽트가 시작되었다. activate 다음 단계다 */
	BIT_MASK_DESCR(IRQD_IRQ_DISABLED),	/* [한국어] 논리적으로 꺼져 있다. 아래 MASKED 와 구분되며, 그 구분이 게으른 비활성화의 토대다 */
	BIT_MASK_DESCR(IRQD_IRQ_MASKED),	/* [한국어] 하드웨어가 실제로 마스크되어 있다 */
	BIT_MASK_DESCR(IRQD_IRQ_INPROGRESS),	/* [한국어] 지금 처리 중이다. handle_irq_event 가 락을 놓고도 안전한 것이 이 비트 덕분이다 */

	BIT_MASK_DESCR(IRQD_PER_CPU),	/* [한국어] CPU 마다 따로 존재하는 인터럽트다 */
	BIT_MASK_DESCR(IRQD_NO_BALANCING),	/* [한국어] 친화도를 바꿀 수 없다 */

	BIT_MASK_DESCR(IRQD_SINGLE_TARGET),	/* [한국어] 한 CPU 에만 보낼 수 있는 하드웨어다. cpuhotplug.c 가 옮길지 정할 때 본다 */
	BIT_MASK_DESCR(IRQD_AFFINITY_SET),	/* [한국어] 사용자가 명시적으로 친화도를 지정했다. 기본값과 구분한다 */
	BIT_MASK_DESCR(IRQD_SETAFFINITY_PENDING),	/* [한국어] 미뤄 둔 친화도 변경이 있다. migration.c 가 다음 인터럽트에서 마무리한다 */
	BIT_MASK_DESCR(IRQD_AFFINITY_MANAGED),	/* [한국어] 커널이 친화도를 관리한다. 사용자가 바꿀 수 없고 CPU 핫플러그에 자동 대응한다 */
	BIT_MASK_DESCR(IRQD_AFFINITY_ON_ACTIVATE),	/* [한국어] activate 시점에 친화도를 적용해야 하는 인터럽트 */
	BIT_MASK_DESCR(IRQD_MANAGED_SHUTDOWN),	/* [한국어] 대상 CPU 가 모두 사라져 꺼 둔 상태. CPU 가 돌아오면 자동으로 되살아난다 */
	BIT_MASK_DESCR(IRQD_CAN_RESERVE),	/* [한국어] 실제 자원 배정 없이 예약만 해 둘 수 있다. MSI 벡터를 아끼는 데 쓴다 */

	BIT_MASK_DESCR(IRQD_FORWARDED_TO_VCPU),	/* [한국어] 게스트 vCPU 로 직접 전달되는 인터럽트. 호스트가 EOI 를 보낼 수 없어 kexec 경로가 특별히 다룬다 */

	BIT_MASK_DESCR(IRQD_WAKEUP_STATE),	/* [한국어] 깨우기 원천으로 지정되었다 */
	BIT_MASK_DESCR(IRQD_WAKEUP_ARMED),	/* [한국어] 절전 중이며 실제로 무장되어 있다. pm.c 가 절전 진입 때 세운다 */

	BIT_MASK_DESCR(IRQD_DEFAULT_TRIGGER_SET),	/* [한국어] 기본 트리거 방식이 설정되었다 */

	BIT_MASK_DESCR(IRQD_HANDLE_ENFORCE_IRQCTX),	/* [한국어] 반드시 하드 인터럽트 문맥에서 처리해야 한다. resend.c 가 소프트웨어 재전송을 거부하는 근거다 */

	BIT_MASK_DESCR(IRQD_IRQ_ENABLED_ON_SUSPEND),	/* [한국어] 절전 진입 때 커널이 일부러 켠 인터럽트. 복귀 때 되돌려야 한다 */

	BIT_MASK_DESCR(IRQD_RESEND_WHEN_IN_PROGRESS),	/* [한국어] 처리 중에 들어온 인터럽트를 재전송해야 하는 하드웨어 */
};

/* [한국어] 설정 비트(_IRQ_*)와 이름의 대응표.
 *
 * settings.h 가 봉인한 그 비트들이다. 밑줄 붙은 이름을 쓰는 것에 주의 —
 * 공개 이름은 봉인되어 컴파일되지 않으므로, 사본 enum 의 이름을 써야 한다.
 *
 * 위 irqdata_states 가 "지금 상태" 라면 이쪽은 "이 인터럽트의 성질" 이다.
 * 실행 중에 거의 바뀌지 않고, 대부분 컨트롤러 드라이버가 초기화 때 정한다.
 *
 * _IRQ_LEVEL 과 _IRQ_PER_CPU 가 이 표에 없는 것이 눈에 띄는데, 그 둘은
 * 위 irqdata_states 의 IRQD_LEVEL/IRQD_PER_CPU 로 함께 표현되기 때문이다. */
static const struct irq_bit_descr irqdesc_states[] = {
	BIT_MASK_DESCR(_IRQ_NOPROBE),	/* [한국어] 자동 탐지 대상이 아니다. 대부분의 인터럽트가 여기 해당한다 */
	BIT_MASK_DESCR(_IRQ_NOREQUEST),	/* [한국어] request_irq 가 거절된다. 이것이 서 있는데 드라이버가 실패한다면 원인이 명확해진다 */
	BIT_MASK_DESCR(_IRQ_NOTHREAD),	/* [한국어] 강제 스레드화 예외다. 타이머나 IPI 가 그렇다 */
	BIT_MASK_DESCR(_IRQ_NOAUTOEN),	/* [한국어] 요청 뒤 자동으로 켜지지 않는다. 드라이버가 직접 enable_irq 를 불러야 한다 */
	BIT_MASK_DESCR(_IRQ_NESTED_THREAD),	/* [한국어] 부모 스레드 안에서 중첩 실행된다. I2C 뒤의 GPIO 확장기 등 */
	BIT_MASK_DESCR(_IRQ_PER_CPU_DEVID),	/* [한국어] CPU 마다 다른 dev_id 를 쓴다. request_percpu_irq 로만 요청할 수 있다 */
	BIT_MASK_DESCR(_IRQ_IS_POLLED),	/* [한국어] 드라이버가 폴링을 병행한다. 오탐 감지에서 제외된다 */
	BIT_MASK_DESCR(_IRQ_DISABLE_UNLAZY),	/* [한국어] disable 시점에 곧바로 마스크해야 한다. 게으른 비활성화의 예외다 */
	BIT_MASK_DESCR(_IRQ_HIDDEN),	/* [한국어] /proc/interrupts 에 보이지 않는다. 계층형 도메인의 중간 단계가 그렇다 */
};

/* [한국어] 코어 내부 상태(IRQS_*)와 이름의 대응표.
 *
 * 세 표 중 가장 짧고, 인터럽트 처리 중의 순간적인 상태를 담는다.
 *
 * internals.h 의 IRQS_* enum 에는 있지만 이 표에 없는 것이 둘 있다 —
 * IRQS_TIMINGS 와 IRQS_SYSFS 다. 진단에 쓸모가 적어 뺀 것으로 보이며,
 * 그 비트들은 헤더 줄의 16진수 값에만 나타난다. */
static const struct irq_bit_descr irqdesc_istates[] = {
	BIT_MASK_DESCR(IRQS_AUTODETECT),	/* [한국어] 자동 탐지가 진행 중이다 */
	BIT_MASK_DESCR(IRQS_SPURIOUS_DISABLED),	/* [한국어] 오탐이 너무 많아 커널이 강제로 껐다. "nobody cared" 메시지와 짝이다 */
	BIT_MASK_DESCR(IRQS_POLL_INPROGRESS),	/* [한국어] 오탐 감지의 폴링이 진행 중이다 */
	BIT_MASK_DESCR(IRQS_ONESHOT),	/* [한국어] 스레드 핸들러가 끝날 때까지 언마스크하지 않는다 */
	BIT_MASK_DESCR(IRQS_REPLAY),	/* [한국어] 소프트웨어 재전송을 발행해 두었다. 중복 재전송을 막는 표시다 */
	BIT_MASK_DESCR(IRQS_WAITING),	/* [한국어] 자동 탐지에서 아직 울리지 않았다 */
	BIT_MASK_DESCR(IRQS_PENDING),	/* [한국어] 처리하지 못한 인터럽트가 밀려 있다. 다시 켤 때 재전송된다 */
	BIT_MASK_DESCR(IRQS_SUSPENDED),	/* [한국어] 절전으로 중단된 상태다 */
	BIT_MASK_DESCR(IRQS_NMI),	/* [한국어] 이 선이 NMI 전달에 쓰인다 */
};


/*
 * [한국어]
 * irq_debug_show - debugfs 파일을 읽을 때 인터럽트의 모든 상태를 출력한다
 *
 * @m:      출력할 seq_file. private 에 서술자가 들어 있다.
 * @p:      seq_file 순회 위치. 이 구현은 쓰지 않는다 — single_open 이라
 *          한 번에 전부 출력하기 때문이다.
 * @return: 항상 0.
 *
 * 이 파일의 본체다. 출력 순서가 곧 진단의 순서라 할 만하다:
 *
 *   handler  — 어느 흐름 제어를 쓰는가. level/edge/fasteoi/percpu 중 하나.
 *   device   — 어느 장치가 요청했는가.
 *   status   — 설정 비트(_IRQ_*)와 그 이름 풀이.
 *   istate   — 코어 처리 상태(IRQS_*)와 이름 풀이.
 *   ddepth   — 비활성 중첩 깊이. 0 이 아니면 꺼져 있다.
 *   wdepth   — 깨우기 요청 중첩 깊이.
 *   dstate   — 하드웨어 쪽 상태(IRQD_*)와 이름 풀이.
 *   node     — 서술자가 놓인 NUMA 노드.
 *   masks    — 세 종류의 CPU 마스크.
 *   data     — 도메인 계층 전체.
 *
 * 세 상태 워드를 각각 16진수와 이름 풀이로 두 번씩 찍는 것이 이 출력의
 * 특징이다. 16진수는 표에 없는 비트까지 담고, 이름 풀이는 사람이 읽기
 * 좋다 — 둘 다 있어야 완전하다.
 *
 * 서술자 락을 잡는 이유: 출력하는 동안 상태가 바뀌면 앞뒤가 맞지 않는
 * 그림이 나온다. 예를 들어 istate 는 처리 중인데 dstate 는 끝난 것으로
 * 보이는 식이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. seq_file 출력이 잠들 수 있지만, 락을
 * 쥔 구간에서는 버퍼에 쓰기만 하므로 안전하다.
 *
 * 호출 체인:
 *   cat /sys/kernel/debug/irq/irqs/N → single_open 골격 → [이 함수]
 */
static int irq_debug_show(struct seq_file *m, void *p)
{
	struct irq_desc *desc = m->private;	/* [한국어] open 이 inode 에서 꺼내 담아 둔 서술자 */
	struct irq_data *data;	/* [한국어] 상태 비트와 도메인 계층을 읽을 통로 */

	guard(raw_spinlock_irq)(&desc->lock);	/* [한국어] 출력 중에 상태가 바뀌면 앞뒤가 맞지 않는 그림이 나온다. guard 라 함수를 벗어날 때 풀린다 */
	data = irq_desc_get_irq_data(desc);	/* [한국어] 최상위 irq_data */
	seq_printf(m, "handler:  %ps\n", desc->handle_irq);	/* [한국어] 흐름 제어 핸들러. handle_level_irq 인지 handle_edge_irq 인지가 여기서 드러난다 */
	seq_printf(m, "device:   %s\n", desc->dev_name);	/* [한국어] 아래 copy_devname 이 복사해 둔 이름. 요청한 적이 없으면 NULL 이라 "(null)" 로 찍힌다 */
	seq_printf(m, "status:   0x%08x\n", desc->status_use_accessors);	/* [한국어] 설정 비트를 16진수로. 아래 이름 풀이가 표에 있는 것만 보여 주므로 원본도 함께 찍는다 */
	irq_debug_show_bits(m, 0, desc->status_use_accessors, irqdesc_states,	/* [한국어] 그 비트들의 이름 */
			    ARRAY_SIZE(irqdesc_states));	/* [한국어] 표의 크기 */
	seq_printf(m, "istate:   0x%08x\n", desc->istate);	/* [한국어] 코어 처리 상태. internals.h 의 #define 이 긴 필드 이름을 istate 로 줄여 준다 */
	irq_debug_show_bits(m, 0, desc->istate, irqdesc_istates,	/* [한국어] 그 비트들의 이름 */
			    ARRAY_SIZE(irqdesc_istates));
	seq_printf(m, "ddepth:   %u\n", desc->depth);	/* [한국어] 비활성 중첩 깊이. 0 이 아니면 누군가 꺼 둔 것이며, 인터럽트가 오지 않는 문제의 가장 흔한 원인이다 */
	seq_printf(m, "wdepth:   %u\n", desc->wake_depth);	/* [한국어] 깨우기 요청의 중첩 깊이. 여러 드라이버가 같은 선을 깨우기 원천으로 요청할 수 있다 */
	seq_printf(m, "dstate:   0x%08x\n", irqd_get(data));	/* [한국어] 하드웨어 쪽 상태. internals.h 의 접근자를 통해서만 읽을 수 있다 */
	irq_debug_show_bits(m, 0, irqd_get(data), irqdata_states,	/* [한국어] 그 비트들의 이름. 세 표 중 가장 크다 */
			    ARRAY_SIZE(irqdata_states));
	seq_printf(m, "node:     %d\n", irq_data_get_node(data));	/* [한국어] 서술자 메모리가 놓인 NUMA 노드. 장치와 다른 노드면 성능 문제의 단서가 된다 */
	irq_debug_show_masks(m, desc);	/* [한국어] 세 종류의 CPU 마스크. 단일 프로세서 빌드에서는 아무것도 찍지 않는다 */
	irq_debug_show_data(m, data, 0);	/* [한국어] 도메인 계층 전체. 들여쓰기 0 에서 시작해 부모마다 4칸씩 깊어진다 */
	return 0;	/* [한국어] seq_file 은 0 이 아닌 값을 오류로 다룬다 */
}

/*
 * [한국어]
 * irq_debug_open - debugfs 파일을 열 때의 처리
 *
 * @inode:  그 파일의 inode. i_private 에 서술자가 들어 있다.
 * @file:   열린 파일.
 * @return: single_open 의 결과. 0 이면 성공.
 *
 * single_open 을 쓰는 것이 이 파일의 출력 방식을 정한다. 이것은 "한 번에
 * 전부 출력하고 끝" 인 seq_file 이며, 커널이 버퍼를 잡아 위 show 함수를
 * 한 번만 부른다.
 *
 * 왜 그것으로 충분한가: 인터럽트 하나의 상태는 길어야 수십 줄이다. 여러
 * 번에 나눠 출력할 이유가 없다.
 *
 * inode->i_private 에서 서술자를 꺼내 넘기는 것이 요점이다. 아래
 * add_debugfs_entry 가 파일을 만들 때 그 자리에 서술자를 담아 두었다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   open("/sys/kernel/debug/irq/irqs/N") → file_operations->open → [이 함수]
 */
static int irq_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_debug_show, inode->i_private);	/* [한국어] 한 번에 전부 출력하는 seq_file. i_private 의 서술자가 show 의 m->private 로 전달된다 */
}

/*
 * [한국어]
 * irq_debug_write - debugfs 파일에 쓰기로 인터럽트를 인위 발생시킨다
 *
 * @file:     쓰기 대상 파일. inode 에 서술자가 들어 있다.
 * @user_buf: 사용자가 쓴 내용.
 * @count:    그 길이.
 * @ppos:     파일 위치. 쓰이지 않는다 — 이 파일은 위치를 갖지 않는다.
 * @return:   소비한 바이트 수, 또는 음수 오류.
 *
 * "trigger" 를 쓰면 그 인터럽트가 실제로 발생한다. 오류 주입 시험이나
 * 인터럽트 처리 경로의 검증에 쓴다.
 *
 * 버퍼가 8바이트뿐인 것이 이 함수의 성격을 말해 준다. 지금은 "trigger"
 * 하나만 받으므로 그 이상이 필요 없다. 사용자가 더 긴 것을 쓰면 앞의
 * 7바이트만 읽고 나머지는 소비한 것으로 처리한다.
 *
 * 알 수 없는 문자열을 오류로 다루지 않는 것에 주의한다. count 를 그대로
 * 돌려주어 성공으로 보이게 한다 — 나중에 새 명령이 추가될 여지를 두고,
 * 잘못 쓴 것을 조용히 무시하는 관대한 처리다.
 *
 * 이 기능이 위험할 수 있다는 점: 인터럽트를 인위로 만들면 드라이버가
 * 예상하지 못한 시점에 핸들러가 돈다. resend.c 의 irq_inject_interrupt
 * kernel-doc 이 그 위험을 자세히 설명한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   echo trigger > /sys/kernel/debug/irq/irqs/N → file_operations->write
 *     → [이 함수] → irq_inject_interrupt() (kernel/irq/resend.c)
 */
static ssize_t irq_debug_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	struct irq_desc *desc = file_inode(file)->i_private;	/* [한국어] 파일을 만들 때 담아 둔 서술자 */
	char buf[8] = { 0, };	/* [한국어] "trigger" 하나만 받으므로 8바이트면 충분하다. 0 으로 초기화해 널 종료를 보장한다 */
	size_t size;	/* [한국어] 실제로 복사할 길이 */

	size = min(sizeof(buf) - 1, count);	/* [한국어] 버퍼를 넘지 않게 자른다. -1 은 널 종료 자리를 남기기 위해서다 */
	if (copy_from_user(buf, user_buf, size))	/* [한국어] 사용자 공간에서 복사한다. 실패는 잘못된 포인터를 뜻한다 */
		return -EFAULT;	/* [한국어] 사용자가 유효하지 않은 주소를 넘겼다 */

	if (!strncmp(buf, "trigger", size)) {	/* [한국어] size 만큼만 비교한다 — 사용자가 "trigger\n" 을 써도 앞부분이 맞으면 통과한다 */
		int err = irq_inject_interrupt(irq_desc_get_irq(desc));	/* [한국어] resend.c 가 실제 발생을 담당한다. 하드웨어 주입을 먼저 시도하고 안 되면 재전송 기구를 쓴다 */

		return err ? err : count;	/* [한국어] 실패하면 그 오류를, 성공하면 소비한 바이트 수를 돌려준다 */
	}

	return count;	/* [한국어] 알 수 없는 명령도 소비한 것으로 처리한다. 오류로 다루지 않는 것은 나중에 새 명령을 더할 여지를 두려는 것이다 */
}

/* [한국어] 이 debugfs 파일의 연산표.
 *
 * open 과 write 만 자체 구현이고 나머지 셋은 seq_file 골격의 기본 구현을
 * 그대로 쓴다. 위 single_open 이 그 골격을 준비해 주므로 가능한 일이다.
 *
 * write 가 있다는 점이 이 파일을 특별하게 만든다. 대부분의 debugfs 파일은
 * 읽기 전용인데, 여기서는 인터럽트를 발생시킬 수 있다. */
static const struct file_operations dfs_irq_ops = {
	.open		= irq_debug_open,
	/* [한국어] 파일을 열 때. single_open 으로 seq_file 골격을 준비한다.
	 * 설정자: 이 정적 초기화. 읽는 자: VFS. */

	.write		= irq_debug_write,
	/* [한국어] 쓰기 — "trigger" 로 인터럽트를 발생시킨다.
	 * 설정자: 이 정적 초기화. 읽는 자: VFS.
	 * 대부분의 debugfs 진단 파일이 읽기 전용인 것과 달리, 이 파일은
	 *   상태를 바꿀 수 있다. 그래서 아래 create_file 의 권한이 0644 다. */

	.read		= seq_read,
	/* [한국어] 읽기. seq_file 골격의 기본 구현이다.
	 * 설정자: 이 정적 초기화. 읽는 자: VFS.
	 * open 이 single_open 으로 준비해 두었으므로 그대로 쓸 수 있다. */

	.llseek		= seq_lseek,
	/* [한국어] 파일 위치 이동. 역시 골격의 기본 구현.
	 * 설정자: 이 정적 초기화. 읽는 자: VFS.
	 * single_open 은 내용을 통째로 버퍼에 담으므로 임의 위치 이동이 가능하다. */

	.release	= single_release,
	/* [한국어] 파일을 닫을 때. single_open 이 잡은 버퍼를 반납한다.
	 * 설정자: 이 정적 초기화. 읽는 자: VFS.
	 * single_open 과 짝이 맞아야 한다 — seq_release 를 쓰면 누수가 난다. */
};

/*
 * [한국어]
 * irq_debugfs_copy_devname - 요청한 장치의 이름을 서술자에 복사해 둔다
 *
 * @irq: 그 인터럽트 번호.
 * @dev: 요청한 장치.
 *
 * 위 show 함수가 "device:" 줄에 찍을 이름을 미리 복사해 두는 함수다.
 *
 * 왜 포인터가 아니라 복사인가: 장치가 인터럽트보다 먼저 사라질 수 있다.
 * 포인터만 들고 있으면 그때 해제된 메모리를 읽는다. 사본을 가지면 장치가
 * 없어져도 진단 출력이 유효하다.
 *
 * 할당 실패를 확인하지 않는 것에 주의: kstrdup 이 NULL 을 돌려주면
 * dev_name 이 NULL 로 남고, 출력에 "(null)" 이 찍힌다. 진단 기능이라
 * 실패해도 크게 문제되지 않으므로 검사를 생략한 것이다.
 *
 * 해제는 internals.h 의 irq_remove_debugfs_entry() 가 kfree 로 한다.
 *
 * 실행 컨텍스트: 인터럽트 요청 경로, 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   request_irq() → __setup_irq() (kernel/irq/manage.c) → [이 함수]
 */
void irq_debugfs_copy_devname(int irq, struct device *dev)
{
	struct irq_desc *desc = irq_to_desc(irq);	/* [한국어] 이름을 담아 둘 서술자 */
	const char *name = dev_name(dev);	/* [한국어] 장치 이름. 아직 이름이 정해지지 않은 장치는 NULL 을 준다 */

	if (name)	/* [한국어] 이름이 있을 때만 */
		desc->dev_name = kstrdup(name, GFP_KERNEL);	/* [한국어] 사본을 만든다. 장치가 먼저 사라져도 진단 출력이 유효하도록. 실패는 확인하지 않는다 — 진단 기능이라 NULL 이어도 큰 문제가 아니다 */
}

/*
 * [한국어]
 * irq_add_debugfs_entry - 인터럽트 하나의 debugfs 파일을 만든다
 *
 * @irq:  그 인터럽트 번호. 파일 이름이 된다.
 * @desc: 그 서술자. i_private 에 담겨 show/write 가 꺼내 쓴다.
 *
 * 두 경로에서 불린다. 부팅 때 아래 init 이 기존 인터럽트마다 부르고,
 * 그 뒤로는 irqdesc.c 가 새 서술자를 만들 때마다 부른다.
 *
 * 세 가지 조기 반환 조건이 각각 다른 상황을 다룬다:
 *   irq_dir 이 없다 — debugfs 가 아직 초기화되지 않았다. 부팅 초기에
 *     만들어지는 인터럽트가 여기 해당하며, 나중에 init 이 일괄 등록한다.
 *   desc 가 없다 — 잘못된 호출이다.
 *   이미 파일이 있다 — 중복 생성을 막는다. init 과 irqdesc.c 가 같은
 *     인터럽트에 대해 둘 다 부를 수 있기 때문이다.
 *
 * 버퍼가 12바이트인 것은 32비트 정수의 최대 자릿수(10)에 널 종료와 여유를
 * 더한 크기다. 인터럽트 번호가 그보다 클 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_debugfs_init() → [이 함수]
 *   alloc_desc() (kernel/irq/irqdesc.c) → [이 함수]
 */
void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *desc)
{
	char name [12];	/* [한국어] 인터럽트 번호를 담을 문자열. 32비트 정수의 최대 자릿수에 여유를 더한 크기다 */

	if (!irq_dir || !desc || desc->debugfs_file)	/* [한국어] debugfs 가 아직 없거나, 서술자가 없거나, 이미 만들어 두었는가 */
		return;	/* [한국어] 첫 조건이 중요하다 — 부팅 초기의 인터럽트는 여기서 물러나고 아래 init 이 나중에 일괄 등록한다 */

	sprintf(name, "%u", irq);	/* [한국어] 번호를 문자열로. 버퍼가 충분히 커서 넘칠 수 없다 */
	desc->debugfs_file = debugfs_create_file(name, 0644, irq_dir, desc,	/* [한국어] 0644 — 쓰기가 가능해야 "trigger" 를 받을 수 있다. root 만 쓸 수 있다 */
						 &dfs_irq_ops);	/* [한국어] 위 연산표. desc 는 i_private 에 담겨 show/write 가 꺼내 쓴다 */
}

/*
 * [한국어]
 * irq_debugfs_init - 부팅 때 debugfs 디렉터리를 만들고 기존 인터럽트를 등록한다
 *
 * @return: 항상 0. initcall 이 int 를 요구해 형식만 맞춘다.
 *
 * 디렉터리 구조를 만든다:
 *   /sys/kernel/debug/irq/          — 최상위
 *   /sys/kernel/debug/irq/domains/  — 도메인 정보 (irqdomain.c 가 만든다)
 *   /sys/kernel/debug/irq/irqs/     — 인터럽트별 파일 (이 파일이 만든다)
 *
 * 그리고 이미 만들어져 있는 인터럽트들을 일괄 등록한다. 부팅 초기에
 * 만들어진 인터럽트들은 위 add_debugfs_entry 가 irq_dir 이 없어 물러났으므로,
 * 여기서 한 번에 처리해야 한다.
 *
 * irq_lock_sparse 를 잡는 이유: 순회 중에 다른 CPU 가 새 인터럽트를 만들거나
 * 없앨 수 있다. 그러면 목록이 바뀌어 순회가 깨진다.
 *
 * 반환값을 확인하지 않는 것에 주의: debugfs_create_dir 이 실패하면 NULL 을
 * 돌려주고, 그것을 부모로 쓰는 다음 호출도 실패한다. 결국 irq_dir 이
 * NULL 로 남아 아무 파일도 만들어지지 않는다 — 진단 기능이라 조용히
 * 비활성화되는 것이 맞는 처리다.
 *
 * 실행 컨텍스트: 부팅 중 initcall, 프로세스 문맥.
 *
 * 호출 체인:
 *   do_initcalls() → [이 함수] → irq_domain_debugfs_init()/irq_add_debugfs_entry()
 */
static int __init irq_debugfs_init(void)
{
	struct dentry *root_dir;	/* [한국어] /sys/kernel/debug/irq */
	int irq;	/* [한국어] 순회 중인 인터럽트 번호 */

	root_dir = debugfs_create_dir("irq", NULL);	/* [한국어] 최상위 디렉터리. 실패하면 NULL 이지만 확인하지 않는다 — 아래 호출들이 자연히 실패해 조용히 비활성화된다 */

	irq_domain_debugfs_init(root_dir);	/* [한국어] 도메인 쪽 항목을 그 아래에 만든다. 도메인을 쓰지 않는 빌드에서는 빈 함수다 */

	irq_dir = debugfs_create_dir("irqs", root_dir);	/* [한국어] 인터럽트별 파일이 놓일 디렉터리. 이 전역이 채워진 뒤부터 add_debugfs_entry 가 실제로 파일을 만든다 */

	irq_lock_sparse();	/* [한국어] 번호 공간을 잠근다. 순회 중에 서술자가 생기거나 사라지지 않게 한다 */
	for_each_active_irq(irq)	/* [한국어] 이미 만들어져 있는 인터럽트들 */
		irq_add_debugfs_entry(irq, irq_to_desc(irq));	/* [한국어] 부팅 초기에 물러났던 것들을 여기서 일괄 등록한다 */
	irq_unlock_sparse();	/* [한국어] 잠금을 푼다 */

	return 0;	/* [한국어] initcall 의 형식. 실패해도 부팅을 막지 않는다 */
}
__initcall(irq_debugfs_init);	/* [한국어] 부팅 중 실행되도록 등록한다. debugfs 자체가 초기화된 뒤여야 하므로 이른 단계일 수 없다 */
