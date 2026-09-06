// SPDX-License-Identifier: GPL-2.0
/*
 * Library implementing the most common irq chip callback functions
 *
 * Copyright (C) 2011, Thomas Gleixner
 */

/*
 * [한국어 설명] 범용(generic) 인터럽트 칩 라이브러리 (generic-chip.c)
 *
 * === 파일의 역할 ===
 * 임베디드 SoC 의 인터럽트 컨트롤러는 대부분 똑같이 생겼다. MMIO 레지스터
 * 몇 개(enable / disable / mask / ack / eoi)가 있고, 각 비트가 인터럽트
 * 하나에 대응하며, 32 개를 한 묶음(뱅크)으로 다룬다. 그런 컨트롤러마다
 * irq_chip 콜백 열 몇 개를 새로 쓰면 거의 같은 코드가 계속 복제된다.
 * 이 파일은 그 반복을 한 번만 구현해 둔 라이브러리다. 드라이버는 자기
 * 레지스터의 오프셋만 struct irq_chip_regs 에 채워 넣고, 여기 있는
 * irq_gc_mask_set_bit() 같은 함수를 콜백 자리에 그대로 꽂으면 된다.
 *
 * 핵심 자료구조는 struct irq_chip_generic (include/linux/irq.h) 이다.
 * 그 안에 레지스터 베이스 주소, 이 칩이 담당하는 인터럽트 번호의 시작,
 * 마스크 레지스터의 소프트웨어 사본(mask_cache), 그리고 이 모두를 지키는
 * gc->lock 이 들어 있다. 마스크 값을 소프트웨어에 캐시해 두는 것이 이
 * 라이브러리의 가장 중요한 설계 결정인데, 많은 컨트롤러의 마스크
 * 레지스터가 쓰기 전용이거나 읽으면 다른 뜻이 되기 때문이다.
 *
 * 한 칩이 여러 종류(type)의 흐름 처리를 지원하는 경우 — 예를 들어 같은
 * 선을 레벨 트리거로도 에지 트리거로도 쓸 수 있는 경우 — struct
 * irq_chip_type 을 여러 개 두고 irq_setup_alt_chip() 으로 갈아탄다.
 *
 * 마지막으로 이 파일은 전체 칩 목록(gc_list)을 들고 있다가 시스템
 * 서스펜드·리줌·셧다운 때 syscore 콜백으로 각 칩의 저장·복원 훅을
 * 불러 준다. 개별 드라이버가 PM 코드를 또 쓰지 않아도 되게 하려는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 서브시스템은 세 층으로 나뉜다.
 *
 *   (1) 흐름 처리 계층 — kernel/irq/chip.c 의 handle_level_irq(),
 *       handle_edge_irq() 등. "이 종류의 인터럽트는 언제 마스크하고
 *       언제 ack 하는가" 를 정한다.
 *   (2) 칩 계층 — struct irq_chip 의 콜백들. "이 하드웨어에서 마스크는
 *       어느 레지스터의 몇 번 비트인가" 를 정한다. 이 파일이 여기다.
 *   (3) 하드웨어 — MMIO 레지스터.
 *
 * (1) 은 자기가 부르는 irq_mask() 가 어떤 하드웨어를 건드리는지 모르고,
 * (2) 는 자기를 부른 흐름 처리기가 무엇인지 모른다. 이 파일은 (2) 의
 * 가장 흔한 구현을 모아 놓은 것이므로, 위로는 chip.c 의 흐름 처리기가
 * 부르고 아래로는 io.h 의 readl/writel 을 부른다.
 *
 * 실행 컨텍스트는 두 가지로 갈린다. irq_gc_mask_set_bit() 류의 콜백은
 * 인터럽트 문맥에서 서술자 락을 쥔 채 불리므로 raw_spinlock 만 쓸 수
 * 있고 잠들 수 없다. 반대로 irq_alloc_generic_chip() 류의 설정 함수는
 * 드라이버 초기화 시점의 프로세스 문맥에서 불리므로 GFP_KERNEL 할당이
 * 가능하다. 두 부류를 섞어 쓰면 안 된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/irqchip/ 아래의 수십 개 SoC 인터럽트 컨트롤러 드라이버가
 * 이 파일의 소비자다. 이들은 irq_alloc_generic_chip() 또는
 * irq_domain_alloc_generic_chips() 로 칩을 만들고, 콜백 자리에 여기
 * 함수들을 꽂는다.
 *
 * 아래쪽: irq_reg_writel()/irq_reg_readl() (include/linux/irq.h 의 인라인)
 * 을 거쳐 실제 MMIO 접근으로 내려간다. 그 인라인은 gc->reg_writel 이
 * 설정돼 있으면 그것을, 아니면 기본 writel 을 쓴다 — 빅엔디언 컨트롤러를
 * 위한 갈래다.
 *
 * 옆쪽: kernel/irq/irqdomain.c 와 짝을 이룬다. irq_generic_chip_ops 가
 * irq_domain_ops 로 등록되면, 도메인이 가상 인터럽트 번호를 매핑할 때마다
 * irq_map_generic_chip() 이 불려 그 번호를 담당 범용 칩에 연결한다.
 * kernel/irq/chip.c 의 irq_set_chip_and_handler(), irq_modify_status()
 * 를 호출해 서술자를 설정한다.
 *
 * 데이터 흐름: 드라이버가 채운 레지스터 오프셋 → irq_chip_generic →
 * (매핑 시점) irq_data->chip_data 에 저장 → (인터럽트 발생 시) 흐름
 * 처리기가 d->chip->irq_mask(d) 호출 → 이 파일의 함수가 chip_data 에서
 * gc 를 꺼내 레지스터에 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * - irq_gc_mask_set_bit() / irq_gc_mask_clr_bit(): 마스크 레지스터가
 *   하나뿐인 칩용. 소프트웨어 캐시를 고쳐 통째로 다시 쓴다.
 * - irq_gc_mask_disable_reg() / irq_gc_unmask_enable_reg(): enable 과
 *   disable 레지스터가 따로인 칩용. 해당 비트만 써 넣으면 되므로
 *   읽기·수정·쓰기가 필요 없지만, 캐시는 그래도 갱신해 둔다.
 * - irq_alloc_generic_chip(): 칩 하나를 할당해 초기화한다. 도메인을
 *   쓰지 않는 옛 드라이버용.
 * - irq_domain_alloc_generic_chips(): 도메인 크기에 맞춰 필요한 만큼의
 *   칩을 한 번에 할당한다. 요즘 드라이버가 쓰는 경로.
 * - irq_map_generic_chip(): 도메인의 map 콜백. 가상 인터럽트 번호를
 *   담당 칩과 비트 위치에 연결한다.
 * - struct irq_chip_generic: 칩 하나의 모든 상태 (레지스터 베이스,
 *   마스크 캐시, 락, 설치된 비트맵).
 * - struct irq_chip_type: 한 칩 안의 한 가지 흐름 종류 (irq_chip 콜백
 *   묶음 + 레지스터 오프셋 + 흐름 처리기).
 */

#include <linux/io.h>	/* [한국어] readl/writel, ioread32be/iowrite32be — 이 파일의 최종 목적지인 MMIO 접근 */
#include <linux/irq.h>	/* [한국어] struct irq_chip_generic, irq_chip_type, irq_reg_readl/writel — 이 파일이 다루는 자료구조 전부 */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree — 칩 구조체를 힙에 잡는다. 크기가 num_ct 에 따라 달라져 정적 배열로는 못 둔다 */
#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL_GPL — 소비자가 drivers/irqchip 의 모듈들이라 심볼을 내보내야 한다 */
#include <linux/irqdomain.h>	/* [한국어] struct irq_domain, irq_domain_ops, irq_find_mapping — 도메인 기반 경로에 필요 */
#include <linux/interrupt.h>	/* [한국어] irq_flow_handler_t 등 인터럽트 API 의 공개 선언 */
#include <linux/kernel_stat.h>	/* [한국어] 인터럽트 통계 헬퍼. 이 파일이 직접 쓰지는 않지만 관례상 함께 포함된다 */
#include <linux/syscore_ops.h>	/* [한국어] struct syscore_ops, register_syscore — 파일 끝의 서스펜드·리줌 훅 등록에 필요 */

#include "internals.h"	/* [한국어] irq_desc 내부 접근(irq_data_to_desc 등). 이 파일이 서술자의 handle_irq 를 직접 바꾸므로 코어 내부 헤더가 필요하다 */

static LIST_HEAD(gc_list);
/* [한국어] 시스템에 만들어진 모든 범용 칩의 연결 리스트.
 * 설정자: irq_setup_generic_chip() 과 irq_domain_alloc_generic_chips() 가
 *   칩을 만들 때 꼬리에 붙이고, irq_remove_generic_chip() 이 뺀다.
 * 읽는 자: 파일 끝의 irq_gc_suspend/resume/shutdown 이 전체를 훑는다.
 *   개별 드라이버가 PM 코드를 쓰지 않아도 되게 하려는 것이 이 리스트의
 *   유일한 존재 이유다.
 * 값 범위: 빈 리스트 ~ 등록된 칩 수만큼.
 * 동기화: 아래 gc_lock 이 지킨다. 다만 PM 콜백들은 락을 잡지 않는데,
 *   syscore 단계는 다른 CPU 가 모두 멈춘 뒤라 경쟁이 없기 때문이다. */
static DEFINE_RAW_SPINLOCK(gc_lock);
/* [한국어] gc_list 를 지키는 락.
 * 설정자·읽는 자: 리스트에 넣고 빼는 세 곳에서만 쓴다.
 * 값 범위: raw_spinlock — 일반 spinlock 이 아닌 이유는 이 리스트 조작이
 *   인터럽트를 끈 문맥에서도 일어날 수 있고, PREEMPT_RT 에서 잠들 수 있는
 *   spinlock 으로 바뀌면 안 되기 때문이다.
 * 동기화: 이 락은 개별 칩의 gc->lock 과 완전히 별개다. 둘을 함께 잡는
 *   곳은 없어 순서 문제가 생기지 않는다. */

/**
 * irq_gc_noop - NOOP function
 * @d: irq_data
 */
/*
 * [한국어]
 * irq_gc_noop - 아무 일도 하지 않는 콜백
 *
 * @d: 대상 인터럽트의 irq_data (쓰지 않는다)
 * @return: 없음
 *
 * 왜 빈 함수가 필요한가: irq_chip 의 콜백 포인터 중 일부는 코어가 NULL
 * 검사 없이 그냥 부른다. 그런 자리에 "할 일 없음" 을 표현하려면 NULL 이
 * 아니라 아무것도 안 하는 함수를 넣어야 한다. 예를 들어 ack 가 필요
 * 없는 컨트롤러의 irq_ack 자리에 이것을 꽂는다.
 * 실행 컨텍스트: 호출자를 그대로 따른다 — 대개 인터럽트 문맥.
 *
 * 호출 체인:
 *   handle_level_irq() 등 흐름 처리기 → d->chip->irq_ack() → [이 함수]
 */
void irq_gc_noop(struct irq_data *d)
{
}
EXPORT_SYMBOL_GPL(irq_gc_noop);	/* [한국어] 모듈로 빌드되는 irqchip 드라이버가 콜백 자리에 쓴다 */

/**
 * irq_gc_mask_disable_reg - Mask chip via disable register
 * @d: irq_data
 *
 * Chip has separate enable/disable registers instead of a single mask
 * register.
 */
/*
 * [한국어]
 * irq_gc_mask_disable_reg - disable 레지스터에 써서 인터럽트를 막는다
 *
 * @d: 막을 인터럽트의 irq_data. 흐름 처리기가 넘긴다.
 * @return: 없음 (irq_chip::irq_mask 의 규약)
 *
 * 어떤 하드웨어를 위한 것인가: enable 레지스터와 disable 레지스터가 따로
 * 있는 컨트롤러다. 이런 칩에서는 "1 을 쓴 비트만 끈다" 는 뜻이라
 * 읽기·수정·쓰기가 필요 없다. 다른 비트를 건드리지 않으므로 여러 CPU 가
 * 서로 다른 인터럽트를 동시에 막아도 하드웨어 차원에서는 안전하다.
 *
 * 그런데도 락을 잡는 이유: 아래에서 mask_cache 라는 소프트웨어 사본을
 * 읽고 고치기 때문이다. 이 사본은 읽기·수정·쓰기라서 보호가 필요하다.
 * 캐시를 유지하는 이유는 서스펜드에서 돌아왔을 때 마스크 상태를 복원해야
 * 하는데 disable 레지스터는 대개 현재 상태를 되읽을 수 없기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, 서술자 락(desc->lock)을 이미 쥔 상태.
 * 그래서 raw_spinlock 만 쓸 수 있고 잠들 수 없다.
 *
 * 호출 체인:
 *   handle_level_irq() / mask_irq() (kernel/irq/chip.c) →
 *   d->chip->irq_mask() → [이 함수] → irq_reg_writel() → writel()
 */
void irq_gc_mask_disable_reg(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 매핑 때 저장해 둔 칩 구조체를 꺼낸다. irq_data->chip_data 필드다 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 지금 쓰이는 흐름 종류. 종류마다 레지스터 오프셋이 다를 수 있다 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트 하나에 해당하는 비트. 매핑 때 1 << idx 로 계산해 두었다 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 아래 캐시 갱신이 읽기·수정·쓰기라 필요하다. guard 라서 함수를 벗어나면 자동으로 풀린다 */
	irq_reg_writel(gc, mask, ct->regs.disable);	/* [한국어] disable 레지스터에 이 비트만 1 로 쓴다. 다른 비트는 0 이라 영향이 없다 */
	*ct->mask_cache &= ~mask;	/* [한국어] 소프트웨어 사본에서도 이 비트를 내린다. 포인터인 이유는 여러 chip_type 이 한 캐시를 공유할 수도, 각자 가질 수도 있어서다 */
}
EXPORT_SYMBOL_GPL(irq_gc_mask_disable_reg);	/* [한국어] irq_chip::irq_mask 자리에 꽂으라고 내보낸다 */

/**
 * irq_gc_mask_set_bit - Mask chip via setting bit in mask register
 * @d: irq_data
 *
 * Chip has a single mask register. Values of this register are cached
 * and protected by gc->lock
 */
/*
 * [한국어]
 * irq_gc_mask_set_bit - 마스크 레지스터의 비트를 세워 인터럽트를 막는다
 *
 * @d: 막을 인터럽트의 irq_data
 * @return: 없음
 *
 * 어떤 하드웨어를 위한 것인가: 마스크 레지스터가 하나뿐이고 "1 인 비트가
 * 막힌 것" 인 컨트롤러다. 위 disable 레지스터 방식과 결정적으로 다른
 * 점은, 여기서는 레지스터 전체를 한 번에 써야 한다는 것이다. 이 비트만
 * 고치려면 나머지 비트의 현재 값을 알아야 한다.
 *
 * 그래서 mask_cache 가 필수다. 하드웨어에서 되읽지 않는 이유는 두
 * 가지다. 첫째, 많은 컨트롤러의 마스크 레지스터가 쓰기 전용이거나
 * 읽으면 "현재 대기 중" 같은 다른 뜻이 된다. 둘째, MMIO 읽기는 쓰기와
 * 달리 버스를 왕복해야 해서 인터럽트 경로에서 비싸다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 을 쥔 상태. 캐시 갱신과
 * 레지스터 쓰기가 원자적으로 묶여야 하므로 gc->lock 이 반드시 필요하다.
 * 락이 없으면 두 CPU 가 각자 읽은 옛 캐시로 덮어써 한쪽 변경이 사라진다.
 *
 * 호출 체인:
 *   mask_irq() (kernel/irq/chip.c) → d->chip->irq_mask() → [이 함수]
 */
void irq_gc_mask_set_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류의 레지스터 묶음 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 캐시 읽기·수정과 레지스터 쓰기를 하나로 묶는다 */
	*ct->mask_cache |= mask;	/* [한국어] 사본에 비트를 세운다. 1 이 "막힘" 인 하드웨어다 */
	irq_reg_writel(gc, *ct->mask_cache, ct->regs.mask);	/* [한국어] 사본 전체를 레지스터에 통째로 쓴다. 부분 쓰기가 불가능하므로 이 방식뿐이다 */
}
EXPORT_SYMBOL_GPL(irq_gc_mask_set_bit);	/* [한국어] 마스크 레지스터가 하나인 칩의 irq_mask 자리 */

/**
 * irq_gc_mask_clr_bit - Mask chip via clearing bit in mask register
 * @d: irq_data
 *
 * Chip has a single mask register. Values of this register are cached
 * and protected by gc->lock
 */
/*
 * [한국어]
 * irq_gc_mask_clr_bit - 마스크 레지스터의 비트를 내려 인터럽트를 막는다
 *
 * @d: 막을 인터럽트의 irq_data
 * @return: 없음
 *
 * 위 irq_gc_mask_set_bit() 과 하는 일은 같지만 비트의 의미가 반대인
 * 하드웨어를 위한 것이다. 여기서는 레지스터가 사실상 "enable 마스크"
 * 라서 1 이 허용, 0 이 차단을 뜻한다. 이름이 mask_clr_bit 인데 실제로는
 * 인터럽트를 막는 함수인 이유가 이것이다 — 막으려면 허용 비트를 내려야
 * 한다. 이 헷갈리는 이름은 커널 전체가 "mask = 막는다" 로 부르는 관례와
 * 하드웨어의 비트 의미가 어긋나서 생겼다.
 *
 * 짝이 되는 unmask 는 irq_gc_unmask_set_bit() 이 아니라, 이런 칩에서는
 * 대개 irq_gc_mask_set_bit() 을 unmask 자리에 꽂아 쓴다.
 *
 * 실행 컨텍스트·동기화는 irq_gc_mask_set_bit() 과 같다.
 *
 * 호출 체인:
 *   mask_irq() → d->chip->irq_mask() → [이 함수]
 */
void irq_gc_mask_clr_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 읽기·수정·쓰기 보호 */
	*ct->mask_cache &= ~mask;	/* [한국어] 허용 비트를 내린다 — 이 하드웨어에서는 0 이 차단이다 */
	irq_reg_writel(gc, *ct->mask_cache, ct->regs.mask);	/* [한국어] 사본 전체를 다시 쓴다 */
}
EXPORT_SYMBOL_GPL(irq_gc_mask_clr_bit);	/* [한국어] 비트 의미가 반대인 칩용 */

/**
 * irq_gc_unmask_enable_reg - Unmask chip via enable register
 * @d: irq_data
 *
 * Chip has separate enable/disable registers instead of a single mask
 * register.
 */
/*
 * [한국어]
 * irq_gc_unmask_enable_reg - enable 레지스터에 써서 인터럽트를 다시 연다
 *
 * @d: 열 인터럽트의 irq_data
 * @return: 없음 (irq_chip::irq_unmask 규약)
 *
 * irq_gc_mask_disable_reg() 의 정확한 반대다. enable 과 disable 이
 * 따로인 칩에서 이 비트만 1 로 쓰면 그 선이 다시 열린다.
 *
 * 언제 불리는가: 레벨 트리거 인터럽트에서 핸들러가 끝난 뒤
 * (handle_level_irq 의 마지막), 또는 disable 했던 인터럽트를 다시
 * 켤 때(enable_irq → irq_enable). 게으른 비활성(lazy disable) 때문에
 * 실제로 하드웨어를 건드리지 않고 넘어가는 경우도 많다 — 코어가
 * IRQD_IRQ_MASKED 를 보고 이미 열려 있으면 부르지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   unmask_irq() (kernel/irq/chip.c) → d->chip->irq_unmask() → [이 함수]
 */
void irq_gc_unmask_enable_reg(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 아래 캐시 갱신 때문에 필요하다 */
	irq_reg_writel(gc, mask, ct->regs.enable);	/* [한국어] enable 레지스터에 이 비트만 쓴다. 다른 선은 건드리지 않는다 */
	*ct->mask_cache |= mask;	/* [한국어] 사본에서도 "열림" 으로 표시한다. 여기서 캐시는 enable 마스크 의미다 — mask_disable_reg 와 같은 캐시를 반대 방향으로 쓴다 */
}
EXPORT_SYMBOL_GPL(irq_gc_unmask_enable_reg);	/* [한국어] enable/disable 이 분리된 칩의 irq_unmask 자리 */

/**
 * irq_gc_ack_set_bit - Ack pending interrupt via setting bit
 * @d: irq_data
 */
/*
 * [한국어]
 * irq_gc_ack_set_bit - 비트를 세워 대기 중인 인터럽트를 확인 처리한다
 *
 * @d: 확인할 인터럽트의 irq_data
 * @return: 없음 (irq_chip::irq_ack 규약)
 *
 * ack 가 무엇인가: 에지 트리거 인터럽트에서 컨트롤러는 "이 선에 신호가
 * 왔었다" 는 사실을 래치(latch)에 붙들어 둔다. 그 래치를 지워 주지
 * 않으면 같은 인터럽트가 끝없이 다시 올라온다. ack 는 그 래치를 지우는
 * 동작이다.
 *
 * 대부분의 컨트롤러에서 status 레지스터는 "쓰기로 지우기(write-1-to-
 * clear)" 규약을 따른다. 즉 지우고 싶은 비트에 1 을 쓴다. 그래서 여기서는
 * 읽기·수정·쓰기가 필요 없고 캐시도 갱신하지 않는다.
 *
 * 그런데도 gc->lock 을 잡는 이유: 하드웨어에 따라 ack 레지스터와 mask
 * 레지스터 접근 사이에 순서 보장이 필요하고, 같은 칩에 대한 MMIO 접근을
 * 직렬화해 두는 편이 안전하기 때문이다. 아래 irq_gc_eoi 도 같은 이유다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. handle_edge_irq() 가 핸들러를 부르기
 * 전에 호출한다 — 핸들러 실행 중에 온 새 신호를 놓치지 않으려면 먼저
 * 지워야 하기 때문이다.
 *
 * 호출 체인:
 *   handle_edge_irq() (kernel/irq/chip.c) → d->chip->irq_ack() → [이 함수]
 */
void irq_gc_ack_set_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 같은 칩에 대한 MMIO 접근을 직렬화한다 */
	irq_reg_writel(gc, mask, ct->regs.ack);	/* [한국어] write-1-to-clear — 이 비트에 1 을 써서 래치를 지운다. 캐시는 갱신하지 않는다: ack 는 상태가 아니라 일회성 동작이다 */
}
EXPORT_SYMBOL_GPL(irq_gc_ack_set_bit);	/* [한국어] 흔한 write-1-to-clear 칩의 irq_ack 자리 */

/**
 * irq_gc_ack_clr_bit - Ack pending interrupt via clearing bit
 * @d: irq_data
 */
/*
 * [한국어]
 * irq_gc_ack_clr_bit - 비트를 내려 대기 중인 인터럽트를 확인 처리한다
 *
 * @d: 확인할 인터럽트의 irq_data
 * @return: 없음
 *
 * 위 irq_gc_ack_set_bit() 과 반대 규약인 컨트롤러용이다. 여기서는
 * "0 을 쓴 비트를 지운다" 이므로 지우려는 비트만 0 이고 나머지는 1 인
 * 값을 써야 한다. 그래서 mask 를 ~d->mask 로 뒤집는다.
 *
 * 주의할 점: 이 방식은 나머지 비트에 1 을 쓰는 셈이므로, 하드웨어가
 * "1 은 아무 영향 없음" 으로 해석해야만 안전하다. 그렇지 않은 칩에서는
 * 다른 인터럽트의 래치까지 건드리게 된다.
 *
 * EXPORT_SYMBOL 이 없는 것에 주목: 위 형제 함수들과 달리 이 함수는
 * 내보내지지 않는다. 이 규약을 쓰는 칩이 커널에 내장 드라이버로만
 * 있기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   handle_edge_irq() → d->chip->irq_ack() → [이 함수]
 */
void irq_gc_ack_clr_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = ~d->mask;	/* [한국어] 비트를 뒤집는다 — 지울 비트만 0, 나머지는 1 이다. 이 한 줄이 위 형제 함수와의 유일한 차이다 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] MMIO 접근 직렬화 */
	irq_reg_writel(gc, mask, ct->regs.ack);	/* [한국어] 반전된 값을 쓴다. write-0-to-clear 규약 */
}

/**
 * irq_gc_mask_disable_and_ack_set - Mask and ack pending interrupt
 * @d: irq_data
 *
 * This generic implementation of the irq_mask_ack method is for chips
 * with separate enable/disable registers instead of a single mask
 * register and where a pending interrupt is acknowledged by setting a
 * bit.
 *
 * Note: This is the only permutation currently used.  Similar generic
 * functions should be added here if other permutations are required.
 */
/*
 * [한국어]
 * irq_gc_mask_disable_and_ack_set - 막기와 확인 처리를 한 번에 한다
 *
 * @d: 대상 인터럽트의 irq_data
 * @return: 없음 (irq_chip::irq_mask_ack 규약)
 *
 * 왜 별도의 콜백이 있는가: 에지 트리거 처리기는 핸들러를 부르기 전에
 * "막고 나서 ack" 를 해야 한다. 이 둘을 각각의 콜백으로 부르면 락을 두 번
 * 잡고 놓게 되고, 그 사이에 다른 CPU 가 끼어들 수 있다. irq_mask_ack 이
 * 있으면 코어가 그 하나만 부르고, 여기서 락을 한 번만 잡아 두 동작을
 * 원자적으로 묶는다.
 *
 * 조합의 문제: mask 방식이 두 가지(disable 레지스터 / 마스크 비트),
 * ack 방식도 두 가지(비트 세우기 / 비트 내리기)라 이론상 네 가지 조합이
 * 나온다. 원본 주석이 말하듯 실제로 쓰이는 것은 이 하나뿐이라 나머지는
 * 구현하지 않았다. 필요해지면 그때 추가하라는 뜻이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, desc->lock 보유.
 *
 * 호출 체인:
 *   handle_edge_irq() (kernel/irq/chip.c) → d->chip->irq_mask_ack() →
 *   [이 함수] → irq_reg_writel() 세 번
 */
void irq_gc_mask_disable_and_ack_set(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] 락을 한 번만 잡아 아래 세 동작을 원자적으로 묶는다 — 이 함수가 존재하는 이유다 */
	irq_reg_writel(gc, mask, ct->regs.disable);	/* [한국어] 먼저 막는다. 순서가 중요하다 — ack 를 먼저 하면 지운 직후 새 신호가 들어와 핸들러 실행 중에 재진입할 수 있다 */
	*ct->mask_cache &= ~mask;	/* [한국어] 소프트웨어 사본도 맞춘다 */
	irq_reg_writel(gc, mask, ct->regs.ack);	/* [한국어] 막은 뒤에 래치를 지운다. write-1-to-clear 규약 */
}
EXPORT_SYMBOL_GPL(irq_gc_mask_disable_and_ack_set);	/* [한국어] irq_chip::irq_mask_ack 자리에 꽂는다 */

/**
 * irq_gc_eoi - EOI interrupt
 * @d: irq_data
 */
/*
 * [한국어]
 * irq_gc_eoi - 인터럽트 처리 완료를 컨트롤러에 알린다
 *
 * @d: 대상 인터럽트의 irq_data
 * @return: 없음 (irq_chip::irq_eoi 규약)
 *
 * EOI(End Of Interrupt)가 무엇인가: 우선순위를 관리하는 컨트롤러 —
 * ARM GIC 나 x86 APIC 계열 — 는 인터럽트를 CPU 에 올릴 때 그 우선순위를
 * "진행 중" 으로 걸어 둔다. 그 상태에서는 같거나 낮은 우선순위의
 * 인터럽트가 올라오지 못한다. EOI 는 그 걸쇠를 푸는 신호다.
 *
 * ack 와 무엇이 다른가: ack 는 핸들러 실행 *전에* 래치를 지우는 것이고,
 * EOI 는 핸들러 실행 *후에* 우선순위 걸쇠를 푸는 것이다. 그래서 흐름
 * 처리기도 다르다 — handle_fasteoi_irq() 가 EOI 방식을 쓴다. 이름이
 * fasteoi 인 이유는 마스크·언마스크 없이 EOI 하나로 끝나 빠르기 때문이다.
 *
 * EXPORT_SYMBOL 이 없다: 이 함수를 쓰는 칩은 커널 내장 드라이버뿐이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥, 핸들러가 끝난 직후.
 *
 * 호출 체인:
 *   handle_fasteoi_irq() (kernel/irq/chip.c) → d->chip->irq_eoi() → [이 함수]
 */
void irq_gc_eoi(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = irq_data_get_chip_type(d);	/* [한국어] 현재 흐름 종류 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] MMIO 접근 직렬화 */
	irq_reg_writel(gc, mask, ct->regs.eoi);	/* [한국어] EOI 레지스터에 이 비트를 써서 우선순위 걸쇠를 푼다. 캐시와 무관한 일회성 동작이다 */
}

/**
 * irq_gc_set_wake - Set/clr wake bit for an interrupt
 * @d:  irq_data
 * @on: Indicates whether the wake bit should be set or cleared
 *
 * For chips where the wake from suspend functionality is not
 * configured in a separate register and the wakeup active state is
 * just stored in a bitmask.
 */
/*
 * [한국어]
 * irq_gc_set_wake - 이 인터럽트를 절전 해제(wakeup) 원으로 표시한다
 *
 * @d:  대상 인터럽트의 irq_data
 * @on: 1 이면 wakeup 원으로 설정, 0 이면 해제
 * @return: 0 성공, -EINVAL 이 인터럽트는 wakeup 을 지원하지 않음
 *
 * wakeup 인터럽트란: 시스템이 서스펜드된 동안에도 살려 두어 시스템을
 * 깨울 수 있는 인터럽트다. 사용자가 enable_irq_wake() 를 부르면 코어가
 * 이 콜백까지 내려온다.
 *
 * 이 구현이 하는 일은 비트 하나를 세우는 것뿐이다. 하드웨어를 건드리지
 * 않는 이유는, 이 함수가 대상으로 하는 칩들이 wakeup 을 위한 별도
 * 레지스터를 갖고 있지 않기 때문이다. 대신 서스펜드 시점에
 * gc->wake_active 를 보고 "이 비트들만 빼고 전부 막는" 처리를 드라이버의
 * suspend 훅이 한다. 즉 여기서 저장한 비트맵은 나중에 읽히는 의도 기록이다.
 *
 * gc->wake_enabled 검사: 하드웨어적으로 wakeup 이 가능한 선인지를
 * 드라이버가 미리 표시해 둔 것이다. 불가능한 선에 요청이 오면 조용히
 * 무시하지 않고 -EINVAL 로 거절해, 사용자가 깨어나지 않는 시스템을
 * 만들지 않게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥 (enable_irq_wake 경로), desc->lock 보유.
 *
 * 호출 체인:
 *   enable_irq_wake() (kernel/irq/manage.c) → set_irq_wake_real() →
 *   d->chip->irq_set_wake() → [이 함수]
 */
int irq_gc_set_wake(struct irq_data *d, unsigned int on)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	u32 mask = d->mask;	/* [한국어] 이 인터럽트의 비트. chip_type 은 쓰지 않는다 — wakeup 은 흐름 종류와 무관하다 */

	if (!(mask & gc->wake_enabled))	/* [한국어] 드라이버가 이 선을 wakeup 가능으로 표시했는가 */
		return -EINVAL;	/* [한국어] 하드웨어가 못 하는 일을 요청받았다. 조용히 성공을 반환하면 사용자가 안 깨어나는 시스템을 만들게 된다 */

	guard(raw_spinlock)(&gc->lock);	/* [한국어] wake_active 는 여러 인터럽트가 공유하는 비트맵이라 읽기·수정·쓰기 보호가 필요하다 */
	if (on)	/* [한국어] wakeup 원으로 설정하는가 */
		gc->wake_active |= mask;	/* [한국어] 비트를 세운다. 서스펜드 시점에 드라이버가 이 비트맵을 읽는다 */
	else	/* [한국어] 해제하는 경우 */
		gc->wake_active &= ~mask;	/* [한국어] 비트를 내린다 */
	return 0;	/* [한국어] 성공. 하드웨어를 건드리지 않았으므로 실패할 여지가 없다 */
}
EXPORT_SYMBOL_GPL(irq_gc_set_wake);	/* [한국어] irq_chip::irq_set_wake 자리 */

/*
 * [한국어]
 * irq_readl_be - 빅엔디언 레지스터 읽기 어댑터
 *
 * @addr: 읽을 MMIO 주소
 * @return: 호스트 바이트 순서로 변환된 32비트 값
 *
 * 왜 필요한가: 일부 SoC — 특히 PowerPC 계열 — 는 인터럽트 컨트롤러
 * 레지스터를 빅엔디언으로 배치한다. 리틀엔디언 CPU 에서 그대로 readl 하면
 * 바이트 순서가 뒤집힌 값이 나온다. ioread32be() 가 변환해 준다.
 *
 * 왜 ioread32be 를 직접 함수 포인터로 쓰지 않는가: 아키텍처에 따라
 * ioread32be 가 함수가 아니라 매크로거나 시그니처가 미묘하게 달라서,
 * gc->reg_readl 이 요구하는 정확한 타입에 대입할 수 없다. 이 얇은 래퍼가
 * 그 타입 차이를 흡수한다.
 *
 * 실행 컨텍스트: 호출자를 따른다 — 인터럽트 문맥일 수 있다.
 *
 * 호출 체인:
 *   irq_reg_readl() (include/linux/irq.h 인라인) → gc->reg_readl → [이 함수]
 */
static u32 irq_readl_be(void __iomem *addr)
{
	return ioread32be(addr);	/* [한국어] 빅엔디언으로 읽고 호스트 순서로 바꾼다 */
}

/*
 * [한국어]
 * irq_writel_be - 빅엔디언 레지스터 쓰기 어댑터
 *
 * @val:  쓸 값 (호스트 바이트 순서)
 * @addr: 쓸 MMIO 주소
 * @return: 없음
 *
 * 위 irq_readl_be() 의 쓰기 짝이다. 두 함수는 항상 함께 설정된다 —
 * 아래 irq_domain_alloc_generic_chips() 에서 IRQ_GC_BE_IO 플래그를 보고
 * gc->reg_readl 과 gc->reg_writel 에 나란히 대입한다.
 *
 * 실행 컨텍스트: 호출자를 따른다.
 *
 * 호출 체인:
 *   irq_reg_writel() (include/linux/irq.h 인라인) → gc->reg_writel → [이 함수]
 */
static void irq_writel_be(u32 val, void __iomem *addr)
{
	iowrite32be(val, addr);	/* [한국어] 호스트 순서 값을 빅엔디언으로 바꿔 쓴다 */
}

/*
 * [한국어]
 * irq_init_generic_chip - 이미 확보된 메모리 위에 범용 칩을 초기화한다
 *
 * @gc:       초기화할 칩 구조체. 호출자가 이미 0 으로 채워 놓았다고 가정한다.
 * @name:     /proc/interrupts 에 보일 칩 이름
 * @num_ct:   이 칩이 지원하는 흐름 종류의 수
 * @irq_base: 이 칩이 담당하는 첫 인터럽트 번호
 * @reg_base: 레지스터 블록의 가상 주소
 * @handler:  기본 흐름 처리기 (handle_level_irq 등)
 * @return:   없음
 *
 * 왜 할당과 분리돼 있는가: 칩 메모리를 잡는 방법이 두 가지이기 때문이다.
 * irq_alloc_generic_chip() 은 칩 하나를 따로 kzalloc 하고,
 * irq_domain_alloc_generic_chips() 는 여러 칩을 한 덩어리로 잡아 그 안을
 * 잘라 쓴다. 초기화 논리는 같으므로 여기에 한 번만 둔다.
 *
 * 주의: gc->lock 을 여기서 초기화한다. 이 함수를 부르기 전에는 그 락을
 * 쓸 수 없다. 또 num_ct 개의 chip_type 이 gc 뒤에 이어 붙어 있다고
 * 가정하므로(가변 길이 배열), 호출자가 그만큼의 공간을 확보해야 한다.
 *
 * 실행 컨텍스트: 드라이버 초기화, 프로세스 문맥. 아직 아무도 이 칩을
 * 모르는 시점이라 락이 필요 없다.
 *
 * 호출 체인:
 *   irq_alloc_generic_chip() / irq_domain_alloc_generic_chips() → [이 함수]
 */
void irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
			   int num_ct, unsigned int irq_base,
			   void __iomem *reg_base, irq_flow_handler_t handler)
{
	struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 구조체 뒤에 이어 붙은 가변 길이 배열의 시작 */
	int i;	/* [한국어] 흐름 종류 순회용 */

	raw_spin_lock_init(&gc->lock);	/* [한국어] 이 칩의 모든 레지스터 접근과 캐시 갱신을 지킬 락. 여기가 유일한 초기화 지점이다 */
	gc->num_ct = num_ct;	/* [한국어] 흐름 종류 개수. irq_setup_alt_chip 이 이 범위 안에서 찾는다 */
	gc->irq_base = irq_base;	/* [한국어] 담당 구간의 시작 번호. 도메인 기반이면 하드웨어 번호, 아니면 리눅스 인터럽트 번호다 — 이 이중 의미가 뒤에서 여러 번 갈림길을 만든다 */
	gc->reg_base = reg_base;	/* [한국어] 레지스터 블록 주소. 도메인 경로에서는 NULL 로 두고 드라이버의 init 콜백이 채운다 */
	for (i = 0; i < num_ct; i++)	/* [한국어] 모든 흐름 종류에 대해 */
		ct[i].chip.name = name;	/* [한국어] 같은 이름을 공유한다. /proc/interrupts 에는 종류와 무관하게 한 이름으로 보이는 편이 낫다 */
	gc->chip_types->handler = handler;	/* [한국어] 0 번 종류만 기본 처리기를 받는다. 나머지는 드라이버가 종류별로 채운다 */
}

/**
 * irq_alloc_generic_chip - Allocate a generic chip and initialize it
 * @name:	Name of the irq chip
 * @num_ct:	Number of irq_chip_type instances associated with this
 * @irq_base:	Interrupt base nr for this chip
 * @reg_base:	Register base address (virtual)
 * @handler:	Default flow handler associated with this chip
 *
 * Returns an initialized irq_chip_generic structure. The chip defaults
 * to the primary (index 0) irq_chip_type and @handler
 */
/*
 * [한국어]
 * irq_alloc_generic_chip - 범용 칩 하나를 할당해 초기화한다
 *
 * @name:     칩 이름
 * @num_ct:   흐름 종류의 수
 * @irq_base: 담당 구간의 첫 리눅스 인터럽트 번호
 * @reg_base: 레지스터 블록의 가상 주소 (ioremap 결과)
 * @handler:  기본 흐름 처리기
 * @return:   초기화된 칩 포인터, 할당 실패 시 NULL
 *
 * 언제 쓰는가: 인터럽트 도메인을 쓰지 않는 드라이버용 경로다. 이 경로는
 * 인터럽트 번호를 직접 지정해야 해서 요즘 방식은 아니지만, 도메인이
 * 도입되기 전부터 있던 드라이버들이 여전히 쓴다. 도메인을 쓰는
 * 드라이버는 아래 irq_domain_alloc_generic_chips() 를 쓴다.
 *
 * 짝이 되는 해제: 이 함수로 잡은 칩은 irq_remove_generic_chip() 으로
 * 리스트에서 뺀 뒤 드라이버가 직접 kfree 한다. 이 파일에 free 짝이 없는
 * 이유는, 대부분의 인터럽트 컨트롤러가 부팅 때 만들어져 끝까지 사는
 * 물건이라 해제 경로가 거의 안 쓰이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. kzalloc_flex 가 GFP_KERNEL 을 쓰므로
 * 잠들 수 있다 — 인터럽트 문맥에서 부르면 안 된다.
 *
 * 호출 체인:
 *   drivers/irqchip 의 각 드라이버 probe → [이 함수] →
 *   kzalloc_flex() → irq_init_generic_chip()
 */
struct irq_chip_generic *
irq_alloc_generic_chip(const char *name, int num_ct, unsigned int irq_base,
		       void __iomem *reg_base, irq_flow_handler_t handler)
{
	struct irq_chip_generic *gc;	/* [한국어] 할당 결과를 담을 포인터 */

	gc = kzalloc_flex(*gc, chip_types, num_ct);	/* [한국어] 구조체 본체 + chip_types 배열 num_ct 개를 한 번에 잡는다. 0 으로 채워지므로 아래 초기화가 안 건드리는 필드는 전부 0 이 된다 */
	if (gc) {	/* [한국어] 할당에 성공했을 때만 초기화한다 */
		irq_init_generic_chip(gc, name, num_ct, irq_base, reg_base,	/* [한국어] 공통 초기화 논리에 위임한다 */
				      handler);
	}
	return gc;	/* [한국어] 실패하면 NULL 이 그대로 나간다. 호출자가 검사해야 한다 */
}
EXPORT_SYMBOL_GPL(irq_alloc_generic_chip);	/* [한국어] 도메인을 쓰지 않는 옛 드라이버들이 부른다 */

/*
 * [한국어]
 * irq_gc_init_mask_cache - 마스크 캐시 포인터를 걸고 초기값을 읽어 온다
 *
 * @gc:    대상 범용 칩
 * @flags: IRQ_GC_MASK_CACHE_PER_TYPE, IRQ_GC_INIT_MASK_CACHE 조합
 * @return: 없음
 *
 * 이 함수가 정하는 것은 "여러 흐름 종류가 마스크 캐시를 공유하는가" 다.
 * 기본은 공유다 — 모든 chip_type 의 mask_cache 포인터가 gc->mask_cache
 * 하나를 가리킨다. 마스크 레지스터가 칩에 하나뿐인 흔한 경우다.
 *
 * IRQ_GC_MASK_CACHE_PER_TYPE 를 주면 종류마다 자기 캐시
 * (ct[i].mask_cache_priv)를 갖는다. 흐름 종류별로 마스크 레지스터가
 * 따로 있는 칩을 위한 것이다. 포인터를 한 겹 두른 설계 덕분에, 캐시를
 * 읽고 쓰는 위쪽 콜백들은 어느 쪽인지 몰라도 된다 — 항상
 * *ct->mask_cache 로 접근하면 된다.
 *
 * IRQ_GC_INIT_MASK_CACHE 를 주면 하드웨어의 현재 마스크 값을 읽어
 * 캐시의 출발점으로 삼는다. 부트로더가 설정해 둔 상태를 이어받아야
 * 하는 칩에서 필요하다. 이 플래그가 없으면 캐시는 0 에서 시작한다.
 *
 * 실행 컨텍스트: 두 갈래다. irq_setup_generic_chip() 에서는 락 없이,
 * irq_map_generic_chip() 에서는 gc->lock 을 쥔 채 불린다. 전자는 아직
 * 아무도 이 칩을 모르는 시점이라 안전하다.
 *
 * 호출 체인:
 *   irq_setup_generic_chip() / irq_map_generic_chip() → [이 함수] →
 *   irq_reg_readl()
 */
static void
irq_gc_init_mask_cache(struct irq_chip_generic *gc, enum irq_gc_flags flags)
{
	struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 흐름 종류 배열 */
	u32 *mskptr = &gc->mask_cache, mskreg = ct->regs.mask;	/* [한국어] 기본값: 칩 공용 캐시와 0 번 종류의 마스크 레지스터. 아래 루프에서 플래그에 따라 종류별로 바뀔 수 있다 */
	int i;	/* [한국어] 순회용 */

	for (i = 0; i < gc->num_ct; i++) {	/* [한국어] 모든 흐름 종류에 대해 */
		if (flags & IRQ_GC_MASK_CACHE_PER_TYPE) {	/* [한국어] 종류마다 자기 캐시를 갖는가 */
			mskptr = &ct[i].mask_cache_priv;	/* [한국어] 이 종류 전용 저장소로 바꾼다 */
			mskreg = ct[i].regs.mask;	/* [한국어] 레지스터 오프셋도 이 종류의 것으로 */
		}
		ct[i].mask_cache = mskptr;	/* [한국어] 포인터를 건다. 공유든 전용이든 위쪽 콜백은 이 포인터만 본다 — 한 겹 간접이 두 경우를 하나로 만든다 */
		if (flags & IRQ_GC_INIT_MASK_CACHE)	/* [한국어] 하드웨어 현재값을 이어받아야 하는가 */
			*mskptr = irq_reg_readl(gc, mskreg);	/* [한국어] 부트로더나 이전 상태가 설정해 둔 마스크를 읽어 캐시의 출발점으로 삼는다 */
	}
}

/**
 * irq_domain_alloc_generic_chips - Allocate generic chips for an irq domain
 * @d:		irq domain for which to allocate chips
 * @info:	Generic chip information
 *
 * Return: 0 on success, negative error code on failure
 */
/*
 * [한국어]
 * irq_domain_alloc_generic_chips - 도메인 크기에 맞춰 범용 칩들을 한꺼번에 만든다
 *
 * @d:    칩을 붙일 인터럽트 도메인. d->revmap_size 가 담당 인터럽트 수다.
 * @info: 칩 이름, 종류 수, 칩당 인터럽트 수, 플래그, 콜백을 담은 설명서
 * @return: 0 성공, -EBUSY 이미 붙어 있음, -EINVAL 계산 결과 칩이 0 개,
 *          -ENOMEM 할당 실패, 그 외 드라이버 init 콜백이 낸 오류
 *
 * 왜 여러 칩인가: 범용 칩 하나는 32 비트 레지스터 하나에 대응하므로
 * 최대 32 개의 인터럽트만 담당한다. 128 개를 다루는 컨트롤러라면 칩이
 * 네 개 필요하다. 이 함수가 그 나눗셈을 대신해 준다.
 *
 * 메모리 배치가 이 함수의 핵심이다. 세 종류의 물건을 한 번의 kzalloc 으로
 * 잡는다:
 *
 *   [ irq_domain_chip_generic 헤더 + gc 포인터 배열 numchips 개 ]
 *   [ 칩 0: irq_chip_generic + chip_types 배열 num_ct 개 ]
 *   [ 칩 1: irq_chip_generic + chip_types 배열 num_ct 개 ]
 *   ...
 *
 * 한 덩어리로 잡는 이유는 해제를 한 번의 kfree 로 끝내기 위해서다. 대신
 * 아래에서 tmp 포인터를 gc_sz 씩 밀어 가며 각 칩의 시작 위치를 손으로
 * 계산해야 한다. struct_size() 는 가변 길이 배열까지 포함한 크기를
 * 오버플로 검사와 함께 계산해 주는 매크로다.
 *
 * 에러 처리: 중간에 드라이버의 init 콜백이 실패하면 err: 로 뛰어
 * 이미 만든 칩들을 역순으로 되돌린다. i 가 실패한 인덱스를 가리키므로
 * while (i--) 가 정확히 성공한 것들만 훑는다.
 *
 * 실행 컨텍스트: 드라이버 probe, 프로세스 문맥. GFP_KERNEL 할당이 있어
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   drivers/irqchip 드라이버 probe → [이 함수] →
 *   irq_init_generic_chip() / info->init() / list_add_tail()
 */
int irq_domain_alloc_generic_chips(struct irq_domain *d,
				   const struct irq_domain_chip_generic_info *info)
{
	struct irq_domain_chip_generic *dgc;	/* [한국어] 도메인에 붙일 칩 묶음 헤더 */
	struct irq_chip_generic *gc;	/* [한국어] 루프 안에서 현재 칩을 가리킨다 */
	int numchips, i;	/* [한국어] 필요한 칩 수와 순회 인덱스. i 는 에러 경로에서 되돌릴 범위로도 쓰인다 */
	size_t dgc_sz;	/* [한국어] 헤더 + 포인터 배열의 크기 */
	size_t gc_sz;	/* [한국어] 칩 하나의 크기 (chip_types 포함) */
	size_t sz;	/* [한국어] 전체 할당 크기 */
	void *tmp;	/* [한국어] 덩어리 안을 걸어 다니며 각 칩의 시작 주소를 계산하는 커서 */
	int ret;	/* [한국어] 드라이버 init 콜백의 반환값 */

	if (d->gc)	/* [한국어] 이 도메인에 이미 범용 칩이 붙어 있는가 */
		return -EBUSY;	/* [한국어] 두 번 붙이면 먼저 것이 새면서 조용히 잘못된 매핑이 생긴다. 명시적으로 막는다 */

	numchips = DIV_ROUND_UP(d->revmap_size, info->irqs_per_chip);	/* [한국어] 올림 나눗셈 — 33 개 인터럽트에 칩당 32 개면 2 개가 필요하다. 남는 자리는 쓰이지 않는다 */
	if (!numchips)	/* [한국어] 도메인이 담당하는 인터럽트가 0 개인가 */
		return -EINVAL;	/* [한국어] 칩 0 개를 할당하면 아래 포인터 계산이 무의미해진다. 호출자의 설정 오류다 */

	/* Allocate a pointer, generic chip and chiptypes for each chip */
	gc_sz = struct_size(gc, chip_types, info->num_ct);	/* [한국어] (위 영어 주석) 칩 하나 + 흐름 종류 배열. struct_size 가 오버플로까지 검사한다 */
	dgc_sz = struct_size(dgc, gc, numchips);	/* [한국어] 헤더 + 칩 포인터 배열 */
	sz = dgc_sz + numchips * gc_sz;	/* [한국어] 전부 한 덩어리로 잡을 총 크기 */

	tmp = dgc = kzalloc(sz, GFP_KERNEL);	/* [한국어] 한 번에 잡는다. tmp 는 같은 주소에서 출발해 아래에서 칩 위치를 훑는 커서가 된다. 0 초기화라 설정하지 않는 필드는 전부 0 이다 */
	if (!dgc)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 아무것도 바꾸지 않았으므로 되돌릴 것이 없다 */
	dgc->irqs_per_chip = info->irqs_per_chip;	/* [한국어] 나중에 하드웨어 번호를 칩 인덱스와 비트 위치로 나눌 때 쓰는 제수(divisor) */
	dgc->num_chips = numchips;	/* [한국어] 범위 검사와 해제 루프의 상한 */
	dgc->irq_flags_to_set = info->irq_flags_to_set;	/* [한국어] 매핑 때 각 서술자에 세울 IRQ_ 플래그. IRQ_NOAUTOEN 처럼 */
	dgc->irq_flags_to_clear = info->irq_flags_to_clear;	/* [한국어] 매핑 때 지울 플래그. 기본값을 끄는 데 쓴다 */
	dgc->gc_flags = info->gc_flags;	/* [한국어] IRQ_GC_INIT_MASK_CACHE, IRQ_GC_BE_IO 등 칩 동작을 정하는 플래그 */
	dgc->exit = info->exit;	/* [한국어] 칩을 없앨 때 드라이버가 정리할 훅. NULL 이면 건너뛴다 */
	d->gc = dgc;	/* [한국어] 도메인에 걸어 둔다. 이 시점부터 매핑 콜백이 이 묶음을 찾을 수 있다 */

	/* Calc pointer to the first generic chip */
	tmp += dgc_sz;	/* [한국어] (위 영어 주석) 헤더와 포인터 배열을 건너뛰면 첫 칩의 자리다 */
	for (i = 0; i < numchips; i++) {	/* [한국어] 칩을 하나씩 초기화한다 */
		/* Store the pointer to the generic chip */
		dgc->gc[i] = gc = tmp;	/* [한국어] (위 영어 주석) 덩어리 안의 위치를 포인터 배열에 기록한다. gc 에도 같은 값을 담아 아래에서 쓴다 */
		irq_init_generic_chip(gc, info->name, info->num_ct,	/* [한국어] 공통 초기화. reg_base 를 NULL 로 넘기는 것에 주의 — 도메인 경로에서는 드라이버의 init 콜백이 채운다 */
				      i * dgc->irqs_per_chip, NULL,	/* [한국어] 이 칩이 담당하는 첫 하드웨어 번호. 도메인 경로에서 irq_base 는 리눅스 번호가 아니라 하드웨어 번호다 */
				      info->handler);

		gc->domain = d;	/* [한국어] 역참조. 이 필드가 NULL 이 아니라는 사실 자체가 "irq_base 는 하드웨어 번호" 라는 신호로 뒤에서 여러 번 쓰인다 */
		if (dgc->gc_flags & IRQ_GC_BE_IO) {	/* [한국어] 레지스터가 빅엔디언인가 */
			gc->reg_readl = &irq_readl_be;	/* [한국어] 위에서 정의한 어댑터를 건다. irq_reg_readl 인라인이 이 포인터를 우선한다 */
			gc->reg_writel = &irq_writel_be;	/* [한국어] 쓰기 짝. 둘은 항상 함께 설정된다 */
		}

		if (info->init) {	/* [한국어] 드라이버가 칩별 추가 설정 훅을 주었는가 */
			ret = info->init(gc);	/* [한국어] 대개 여기서 reg_base 를 채우고 자기 레지스터 오프셋을 설정한다 */
			if (ret)	/* [한국어] 실패하면 */
				goto err;	/* [한국어] 이미 만든 칩들을 되돌려야 한다. 여기서 그냥 반환하면 gc_list 에 반쯤 만들어진 칩들이 남는다 */
		}

		scoped_guard (raw_spinlock_irqsave, &gc_lock)	/* [한국어] 전역 리스트 조작. irqsave 인 이유는 이 리스트를 인터럽트가 꺼진 문맥에서도 만질 수 있어서다 */
			list_add_tail(&gc->list, &gc_list);	/* [한국어] PM 콜백이 훑을 목록에 등록한다. 꼬리에 붙여 생성 순서를 보존한다 — 리줌 때 같은 순서로 복원된다 */
		/* Calc pointer to the next generic chip */
		tmp += gc_sz;	/* [한국어] (위 영어 주석) 커서를 다음 칩 자리로 민다 */
	}
	return 0;	/* [한국어] 모든 칩이 준비됐다. 이제 도메인이 매핑을 받을 수 있다 */

err:	/* [한국어] 드라이버 init 콜백이 실패했을 때만 오는 경로 */
	while (i--) {	/* [한국어] 실패한 i 번은 빼고 성공한 것들만 역순으로 되돌린다. 후치 감소라 첫 반복에서 i-1 부터 시작한다 */
		if (dgc->exit)	/* [한국어] 드라이버가 정리 훅을 주었는가 */
			dgc->exit(dgc->gc[i]);	/* [한국어] init 이 잡은 자원을 드라이버가 풀게 한다 */
		irq_remove_generic_chip(dgc->gc[i], ~0U, 0, 0);	/* [한국어] gc_list 에서 뺀다. 마스크 ~0U 는 "이 칩의 모든 비트" 라는 뜻이지만, 아직 매핑된 인터럽트가 없어 실제 정리 대상은 리스트 항목뿐이다 */
	}
	d->gc = NULL;	/* [한국어] 도메인에서 떼어 낸다. 이 줄이 없으면 해제된 메모리를 가리키는 포인터가 남는다 */
	kfree(dgc);	/* [한국어] 한 덩어리로 잡았으므로 한 번의 kfree 로 칩 전부가 사라진다 */
	return ret;	/* [한국어] 드라이버 init 이 낸 오류를 그대로 올린다 */
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_generic_chips);	/* [한국어] 도메인을 쓰는 요즘 드라이버의 주 진입점 */

/**
 * irq_domain_remove_generic_chips - Remove generic chips from an irq domain
 * @d: irq domain for which generic chips are to be removed
 */
/*
 * [한국어]
 * irq_domain_remove_generic_chips - 도메인에 붙은 범용 칩들을 전부 없앤다
 *
 * @d: 대상 인터럽트 도메인
 * @return: 없음
 *
 * 위 irq_domain_alloc_generic_chips() 의 정확한 반대다. 실제로 위 함수의
 * err: 경로와 몸통이 거의 같은데, 다른 점은 여기서는 모든 칩을 되돌리고
 * 순서도 앞에서부터라는 것뿐이다. 해제 순서가 중요하지 않은 이유는 칩들이
 * 서로를 참조하지 않기 때문이다.
 *
 * 언제 불리는가: 드라이버 모듈을 내리거나 probe 후반이 실패해 되돌릴 때.
 * 대부분의 인터럽트 컨트롤러는 부팅 때 만들어져 끝까지 살기 때문에 이
 * 경로는 거의 쓰이지 않는다.
 *
 * 주의: 이 함수를 부르기 전에 도메인의 모든 매핑이 이미 해제돼 있어야
 * 한다. 살아 있는 매핑이 있으면 그 irq_data->chip_data 가 방금 kfree 한
 * 메모리를 가리키게 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 remove / probe 실패 경로 → [이 함수] →
 *   dgc->exit() → irq_remove_generic_chip() → kfree()
 */
void irq_domain_remove_generic_chips(struct irq_domain *d)
{
	struct irq_domain_chip_generic *dgc = d->gc;	/* [한국어] 도메인에 걸린 칩 묶음 */
	unsigned int i;	/* [한국어] 순회용 */

	if (!dgc)	/* [한국어] 붙은 적이 없는가 */
		return;	/* [한국어] 할 일이 없다. 두 번 불러도 안전하게 만드는 검사다 */

	for (i = 0; i < dgc->num_chips; i++) {	/* [한국어] 모든 칩에 대해 */
		if (dgc->exit)	/* [한국어] 드라이버 정리 훅이 있는가 */
			dgc->exit(dgc->gc[i]);	/* [한국어] 드라이버가 init 에서 잡은 것을 푼다 */
		irq_remove_generic_chip(dgc->gc[i], ~0U, 0, 0);	/* [한국어] gc_list 에서 빼고 매핑된 인터럽트가 있으면 서술자를 no_irq_chip 으로 되돌린다 */
	}
	d->gc = NULL;	/* [한국어] 도메인에서 떼어 낸다. kfree 보다 먼저 해야 그 사이에 누가 참조하지 못한다 */
	kfree(dgc);	/* [한국어] 헤더와 모든 칩이 한 덩어리라 한 번이면 끝난다 */
}
EXPORT_SYMBOL_GPL(irq_domain_remove_generic_chips);	/* [한국어] 드라이버 remove 경로에서 부른다 */

/**
 * __irq_alloc_domain_generic_chips - Allocate generic chips for an irq domain
 * @d:			irq domain for which to allocate chips
 * @irqs_per_chip:	Number of interrupts each chip handles (max 32)
 * @num_ct:		Number of irq_chip_type instances associated with this
 * @name:		Name of the irq chip
 * @handler:		Default flow handler associated with these chips
 * @clr:		IRQ_* bits to clear in the mapping function
 * @set:		IRQ_* bits to set in the mapping function
 * @gcflags:		Generic chip specific setup flags
 */
/*
 * [한국어]
 * __irq_alloc_domain_generic_chips - 인자 나열 방식의 옛 진입점
 *
 * @d:             대상 도메인
 * @irqs_per_chip: 칩당 인터럽트 수 (최대 32 — 레지스터가 32비트라서)
 * @num_ct:        흐름 종류 수
 * @name:          칩 이름
 * @handler:       기본 흐름 처리기
 * @clr:           매핑 때 지울 IRQ_ 플래그 비트
 * @set:           매핑 때 세울 IRQ_ 플래그 비트
 * @gcflags:       IRQ_GC_ 계열 설정 플래그
 * @return:        irq_domain_alloc_generic_chips() 의 반환값 그대로
 *
 * 왜 두 진입점이 있는가: 원래는 이 함수처럼 인자를 여덟 개 늘어놓는
 * 방식뿐이었다. 인자를 하나 추가할 때마다 모든 호출자를 고쳐야 하는
 * 문제가 있어, 설명서 구조체를 넘기는 irq_domain_alloc_generic_chips()
 * 가 나중에 추가됐다. 이 함수는 기존 호출자를 위해 남은 얇은 껍데기로,
 * 인자들을 구조체에 채워 새 함수를 부르는 일만 한다.
 *
 * 새 방식에만 있는 필드(init, exit 콜백)는 여기서 채우지 않으므로,
 * 지역 구조체의 나머지가 0 으로 초기화되는 것이 중요하다. 지정 초기화
 * 문법이 그것을 보장한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   옛 drivers/irqchip 드라이버 → [이 함수] →
 *   irq_domain_alloc_generic_chips()
 */
int __irq_alloc_domain_generic_chips(struct irq_domain *d, int irqs_per_chip,
				     int num_ct, const char *name,
				     irq_flow_handler_t handler,
				     unsigned int clr, unsigned int set,
				     enum irq_gc_flags gcflags)
{
	struct irq_domain_chip_generic_info info = {	/* [한국어] 스택에 설명서를 만든다. 지정하지 않은 init/exit 는 0(NULL)이 되어 새 함수가 건너뛴다 */
		.irqs_per_chip		= irqs_per_chip,	/* [한국어] 칩당 인터럽트 수 */
		.num_ct			= num_ct,	/* [한국어] 흐름 종류 수 */
		.name			= name,	/* [한국어] 칩 이름 */
		.handler		= handler,	/* [한국어] 기본 흐름 처리기 */
		.irq_flags_to_clear	= clr,	/* [한국어] 인자 이름과 필드 이름이 다르다 — 옛 API 의 짧은 이름을 새 필드에 옮긴다 */
		.irq_flags_to_set	= set,	/* [한국어] 위와 같은 이유 */
		.gc_flags		= gcflags,	/* [한국어] 설정 플래그 */
	};

	return irq_domain_alloc_generic_chips(d, &info);	/* [한국어] 실제 일은 전부 새 함수가 한다. info 는 스택에 있지만 그 함수가 값을 복사해 가므로 안전하다 */
}
EXPORT_SYMBOL_GPL(__irq_alloc_domain_generic_chips);	/* [한국어] 옛 호출자를 위해 계속 내보낸다 */

/*
 * [한국어]
 * __irq_get_domain_generic_chip - 하드웨어 번호로 담당 칩을 찾는다 (오류 포인터 반환)
 *
 * @d:      대상 도메인
 * @hw_irq: 하드웨어 인터럽트 번호
 * @return: 담당 칩 포인터, 또는 ERR_PTR(-ENODEV) 범용 칩이 안 붙음,
 *          ERR_PTR(-EINVAL) 번호가 범위 밖
 *
 * 나눗셈 한 번이 전부다. 칩당 32 개씩 담당하므로 hw_irq / 32 가 칩
 * 인덱스이고 hw_irq % 32 가 그 칩 안의 비트 위치다. 나머지 연산은
 * 호출자가 따로 한다 — 이 함수는 칩만 돌려준다.
 *
 * 왜 ERR_PTR 인가: 아래 irq_get_domain_generic_chip() 은 NULL 만
 * 돌려주면 되지만, irq_map_generic_chip() 은 -ENODEV 와 -EINVAL 을
 * 구분해 도메인 코어에 올려야 한다. 그래서 안쪽 함수가 오류 코드를
 * 보존하고, 바깥 껍데기가 그것을 NULL 로 눌러 준다.
 *
 * 실행 컨텍스트: 매핑 경로(프로세스 문맥)와 조회 경로 양쪽. 락을 잡지
 * 않는데, 읽는 필드들이 칩 생성 이후 바뀌지 않기 때문이다.
 *
 * 호출 체인:
 *   irq_map_generic_chip() / irq_get_domain_generic_chip() → [이 함수]
 */
static struct irq_chip_generic *
__irq_get_domain_generic_chip(struct irq_domain *d, unsigned int hw_irq)
{
	struct irq_domain_chip_generic *dgc = d->gc;	/* [한국어] 도메인에 걸린 칩 묶음 */
	int idx;	/* [한국어] 칩 인덱스 */

	if (!dgc)	/* [한국어] 이 도메인은 범용 칩을 쓰지 않는가 */
		return ERR_PTR(-ENODEV);	/* [한국어] 장치가 없다는 뜻. 호출자가 IS_ERR 로 걸러 낸다 */
	idx = hw_irq / dgc->irqs_per_chip;	/* [한국어] 어느 칩이 담당하는가. 32 개씩 나눠 가지므로 단순 나눗셈이다 */
	if (idx >= dgc->num_chips)	/* [한국어] 계산된 인덱스가 배열을 벗어나는가 */
		return ERR_PTR(-EINVAL);	/* [한국어] 도메인 크기를 넘는 번호를 물었다. 배열 밖 접근을 막는 마지막 관문이다 */
	return dgc->gc[idx];	/* [한국어] 담당 칩. 비트 위치 계산은 호출자 몫이다 */
}

/**
 * irq_get_domain_generic_chip - Get a pointer to the generic chip of a hw_irq
 * @d:			irq domain pointer
 * @hw_irq:		Hardware interrupt number
 */
/*
 * [한국어]
 * irq_get_domain_generic_chip - 하드웨어 번호로 담당 칩을 찾는다 (NULL 반환)
 *
 * @d:      대상 도메인
 * @hw_irq: 하드웨어 인터럽트 번호
 * @return: 담당 칩 포인터, 없거나 범위 밖이면 NULL
 *
 * 위 안쪽 함수의 공개 껍데기다. 오류 포인터를 NULL 로 눌러 주는 일만
 * 한다. 드라이버들이 "있으면 쓰고 없으면 만다" 식으로 쓰기 때문에
 * 오류 코드를 구분할 필요가 없고, IS_ERR 를 드라이버마다 쓰게 하는 것보다
 * NULL 검사 하나가 실수가 적다.
 *
 * 실행 컨텍스트: 드라이버가 부르는 곳을 따른다. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   drivers/irqchip 드라이버 / irq_unmap_generic_chip() → [이 함수] →
 *   __irq_get_domain_generic_chip()
 */
struct irq_chip_generic *
irq_get_domain_generic_chip(struct irq_domain *d, unsigned int hw_irq)
{
	struct irq_chip_generic *gc = __irq_get_domain_generic_chip(d, hw_irq);	/* [한국어] 실제 조회 */

	return !IS_ERR(gc) ? gc : NULL;	/* [한국어] 오류 포인터를 NULL 로 바꾼다. IS_ERR 는 포인터가 커널의 마지막 페이지 안에 있는지로 오류를 판별한다 */
}
EXPORT_SYMBOL_GPL(irq_get_domain_generic_chip);	/* [한국어] 드라이버가 자기 칩을 되찾을 때 쓴다 */

/*
 * Separate lockdep classes for interrupt chip which can nest irq_desc
 * lock and request mutex.
 */
/*
 * [한국어]
 * 아래 두 lockdep 클래스 키는 "중첩된 인터럽트 컨트롤러" 문제를 푼다.
 *
 * 무슨 문제인가: I2C 나 SPI 로 붙은 GPIO 확장 칩처럼, 인터럽트를
 * 처리하려면 다른 인터럽트를 기다려야 하는 컨트롤러가 있다. 이런 칩의
 * 서술자 락은 부모 컨트롤러의 서술자 락 안에서 잡힌다. 즉 같은 종류의
 * 락이 자기 자신 안에서 중첩된다.
 *
 * lockdep 은 락의 "클래스" 단위로 순서를 추적하는데, 모든 irq_desc 락이
 * 한 클래스면 이 중첩을 재귀 데드락으로 오해해 경고를 낸다. 실제로는
 * 부모와 자식이 항상 같은 순서라 안전하다.
 *
 * 해결: 중첩되는 칩의 서술자에는 별도 클래스 키를 붙여 lockdep 이 둘을
 * 다른 락으로 보게 한다. 드라이버가 IRQ_GC_INIT_NESTED_LOCK 플래그를
 * 주면 아래 매핑·설정 함수가 이 키들을 건다.
 *
 * 두 개인 이유: 서술자 락(desc->lock)과 요청 뮤텍스(desc->request_mutex)가
 * 각각 따로 중첩되므로 클래스도 따로 필요하다.
 */
static struct lock_class_key irq_nested_lock_class;
/* [한국어] 중첩 가능한 칩의 desc->lock 에 붙일 lockdep 클래스 키.
 * 설정자: irq_set_lockdep_class() 를 통해 매핑·설정 시점에 붙는다.
 * 읽는 자: lockdep 검증기만 본다. CONFIG_LOCKDEP 이 꺼져 있으면
 *   이 변수는 크기 0 이 되고 관련 코드도 전부 사라진다.
 * 값 범위: 내용이 없는 표식 객체 — 주소 자체가 식별자다.
 * 동기화: 불필요. 주소만 쓰이고 내용을 읽고 쓰지 않는다. */
static struct lock_class_key irq_nested_request_class;
/* [한국어] 중첩 가능한 칩의 desc->request_mutex 에 붙일 클래스 키.
 * 설정자·읽는 자: 위와 같다.
 * 값 범위: 위와 같은 표식 객체.
 * 동기화: 불필요. 위 키와 반드시 별개여야 한다 — 하나로 합치면
 *   두 종류의 중첩이 뒤섞여 lockdep 이 다시 헛경고를 낸다. */

/*
 * irq_map_generic_chip - Map a generic chip for an irq domain
 */
/*
 * [한국어]
 * irq_map_generic_chip - 도메인 매핑 콜백: 가상 번호를 범용 칩에 연결한다
 *
 * @d:      매핑을 요청한 도메인
 * @virq:   새로 배정된 리눅스(가상) 인터럽트 번호
 * @hw_irq: 그에 대응하는 하드웨어 인터럽트 번호
 * @return: 0 성공, -ENODEV 범용 칩 없음, -EINVAL 범위 밖,
 *          -ENOTSUPP 이 선은 쓰지 않기로 표시됨, -EBUSY 이미 매핑됨
 *
 * 언제 불리는가: 드라이버가 인터럽트를 요청해 도메인이 새 매핑을 만들
 * 때마다 코어가 이 콜백을 부른다. 이 함수가 하는 일은 그 하나의 번호를
 * 담당 칩·비트 위치·irq_chip 콜백 묶음에 연결하고, 서술자에 흐름 처리기를
 * 다는 것이다. 여기까지 끝나면 그 인터럽트는 실제로 동작할 수 있다.
 *
 * 마스크 캐시 초기화 시점이 미묘하다. 캐시는 칩당 한 번만 초기화해야
 * 하는데, 매핑은 인터럽트마다 일어난다. 그래서 gc->installed 가 아직
 * 0 인지 — 즉 이 칩의 첫 매핑인지 — 를 보고 그때만 초기화한다.
 * 두 번 초기화하면 이미 반영된 마스크 설정이 하드웨어 값으로 덮여
 * 사라진다.
 *
 * d->mask 계산: 대부분의 칩은 "칩 안에서 몇 번째인가" 를 그대로 비트
 * 위치로 쓴다(1 << idx). 그러지 않는 칩은 irq_calc_mask 콜백을 제공해
 * 자기 방식대로 계산한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 도메인 뮤텍스를 쥔 상태로 불린다.
 *
 * 호출 체인:
 *   irq_create_mapping() → irq_domain_associate() (kernel/irq/irqdomain.c) →
 *   d->ops->map() → [이 함수] → irq_domain_set_info() / irq_modify_status()
 */
int irq_map_generic_chip(struct irq_domain *d, unsigned int virq,
			 irq_hw_number_t hw_irq)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);	/* [한국어] 이 가상 번호의 irq_data. 아래에서 mask 를 채워 넣을 대상이다 */
	struct irq_domain_chip_generic *dgc = d->gc;	/* [한국어] 칩 묶음 헤더 */
	struct irq_chip_generic *gc;	/* [한국어] 이 번호를 담당할 칩 */
	struct irq_chip_type *ct;	/* [한국어] 그 칩의 0 번 흐름 종류 */
	struct irq_chip *chip;	/* [한국어] 그 종류의 콜백 묶음 */
	int idx;	/* [한국어] 칩 안에서의 비트 위치 */

	gc = __irq_get_domain_generic_chip(d, hw_irq);	/* [한국어] 담당 칩 조회. 오류 포인터가 올 수 있다 */
	if (IS_ERR(gc))	/* [한국어] 범용 칩이 없거나 번호가 범위 밖인가 */
		return PTR_ERR(gc);	/* [한국어] -ENODEV 또는 -EINVAL 을 그대로 도메인 코어에 올린다. 이 구분을 위해 안쪽 함수가 ERR_PTR 를 쓴다 */

	idx = hw_irq % dgc->irqs_per_chip;	/* [한국어] 칩 안에서 몇 번째인가. 위 나눗셈의 나머지다 */

	if (test_bit(idx, &gc->unused))	/* [한국어] 드라이버가 이 선을 "배선되지 않음" 으로 표시해 두었는가 */
		return -ENOTSUPP;	/* [한국어] 하드웨어에 실제로 없는 선이다. 매핑을 만들면 존재하지 않는 인터럽트를 기다리게 된다 */

	if (test_bit(idx, &gc->installed))	/* [한국어] 이미 매핑된 선인가 */
		return -EBUSY;	/* [한국어] 두 가상 번호가 한 하드웨어 선을 가리키면 마스크·언마스크가 서로를 덮는다 */

	ct = gc->chip_types;	/* [한국어] 0 번 흐름 종류를 기본으로 쓴다. 다른 종류로 바꾸는 것은 나중에 irq_setup_alt_chip 이 한다 */
	chip = &ct->chip;	/* [한국어] 그 종류의 irq_chip 콜백 묶음 */

	/* We only init the cache for the first mapping of a generic chip */
	if (!gc->installed) {	/* [한국어] (위 영어 주석) 이 칩의 첫 매핑인가. 비트맵 전체가 0 이면 아직 아무도 안 붙었다 */
		guard(raw_spinlock_irqsave)(&gc->lock);	/* [한국어] 캐시 초기화가 레지스터를 읽고 여러 필드를 고치므로 보호한다. irqsave 는 이 경로가 인터럽트 꺼진 문맥에서도 불릴 수 있어서다 */
		irq_gc_init_mask_cache(gc, dgc->gc_flags);	/* [한국어] 캐시 포인터를 걸고 필요하면 하드웨어 현재값을 읽어 온다. 두 번 부르면 이미 반영된 설정이 날아간다 */
	}

	/* Mark the interrupt as installed */
	set_bit(idx, &gc->installed);	/* [한국어] (위 영어 주석) 이 비트를 점유 표시한다. 위 -EBUSY 검사의 근거이자, PM 경로가 "이 칩에서 살아 있는 선" 을 찾는 근거이기도 하다 */

	if (dgc->gc_flags & IRQ_GC_INIT_NESTED_LOCK)	/* [한국어] 이 칩이 다른 인터럽트 안에서 중첩되는 종류인가 */
		irq_set_lockdep_class(virq, &irq_nested_lock_class,	/* [한국어] 위에서 정의한 별도 클래스를 걸어 lockdep 헛경고를 막는다 */
				      &irq_nested_request_class);

	if (chip->irq_calc_mask)	/* [한국어] 칩이 비트 위치를 자기 방식으로 계산하는가 */
		chip->irq_calc_mask(data);	/* [한국어] 드물다. 비트 배치가 단순 1:1 이 아닌 컨트롤러용 */
	else	/* [한국어] 대부분의 경우 */
		data->mask = 1 << idx;	/* [한국어] 칩 안 위치를 그대로 비트로 쓴다. 이 값이 앞의 모든 mask/ack/eoi 콜백이 쓰는 d->mask 다 */

	irq_domain_set_info(d, virq, hw_irq, chip, gc, ct->handler, NULL, NULL);	/* [한국어] 서술자에 칩·chip_data(gc)·흐름 처리기를 한 번에 설정한다. gc 를 chip_data 로 넣는 것이 앞의 irq_data_get_irq_chip_data() 가 칩을 되찾는 근거다 */
	irq_modify_status(virq, dgc->irq_flags_to_clear, dgc->irq_flags_to_set);	/* [한국어] 드라이버가 지정한 IRQ_ 플래그를 적용한다. IRQ_NOAUTOEN 처럼 이 컨트롤러 전체에 공통인 성질을 건다 */
	return 0;	/* [한국어] 이 인터럽트가 이제 동작 준비를 마쳤다 */
}

/*
 * [한국어]
 * irq_unmap_generic_chip - 도메인 unmap 콜백: 매핑을 끊는다
 *
 * @d:    대상 도메인
 * @virq: 끊을 리눅스 인터럽트 번호
 * @return: 없음 (irq_domain_ops::unmap 규약)
 *
 * 위 irq_map_generic_chip() 의 반대다. 점유 비트를 내리고 서술자의 칩을
 * no_irq_chip 으로 되돌린다. no_irq_chip 은 모든 콜백이 아무것도 하지
 * 않는 더미 칩으로, "이 번호는 이제 하드웨어와 연결돼 있지 않다" 는 뜻이다.
 * NULL 로 두지 않는 이유는 코어 곳곳이 chip 포인터를 검사 없이 역참조하기
 * 때문이다.
 *
 * 마스크 캐시는 건드리지 않는다. 같은 칩의 다른 선들이 여전히 그 캐시를
 * 쓰고 있어서다. 끊긴 선의 마스크 비트가 캐시에 남지만, 그 선은 이제
 * 하드웨어에서 막힌 상태이므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_dispose_mapping() → irq_domain_disassociate() → d->ops->unmap() →
 *   [이 함수] → irq_domain_set_info()
 */
void irq_unmap_generic_chip(struct irq_domain *d, unsigned int virq)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);	/* [한국어] 끊을 번호의 irq_data. 여기서 하드웨어 번호를 얻는다 */
	struct irq_domain_chip_generic *dgc = d->gc;	/* [한국어] 칩 묶음 헤더 */
	unsigned int hw_irq = data->hwirq;	/* [한국어] 매핑 때 저장된 하드웨어 번호. 이것으로 담당 칩을 되찾는다 */
	struct irq_chip_generic *gc;	/* [한국어] 담당 칩 */
	int irq_idx;	/* [한국어] 칩 안 비트 위치 */

	gc = irq_get_domain_generic_chip(d, hw_irq);	/* [한국어] NULL 반환 판을 쓴다 — 여기서는 오류 종류를 구분할 필요가 없다 */
	if (!gc)	/* [한국어] 담당 칩을 못 찾았는가 */
		return;	/* [한국어] unmap 은 반환값이 없어 오류를 알릴 길이 없다. 조용히 넘어간다 */

	irq_idx = hw_irq % dgc->irqs_per_chip;	/* [한국어] 매핑 때와 같은 계산 */

	clear_bit(irq_idx, &gc->installed);	/* [한국어] 점유 표시를 내린다. 이제 같은 하드웨어 번호를 다시 매핑할 수 있다 */
	irq_domain_set_info(d, virq, hw_irq, &no_irq_chip, NULL, NULL, NULL,	/* [한국어] 더미 칩으로 되돌리고 chip_data 를 NULL 로 지운다. 해제된 gc 를 가리키는 포인터가 남지 않게 하는 것이 핵심이다 */
			    NULL);

}

const struct irq_domain_ops irq_generic_chip_ops = {
	/* [한국어] 범용 칩을 쓰는 도메인이 그대로 가져다 쓰는 표준 연산표.
	 * 드라이버가 irq_domain_add_linear() 등에 이 주소를 넘기면 매핑
	 * 처리가 전부 이 파일로 들어온다. */
	.map	= irq_map_generic_chip,
	/* [한국어] 새 매핑을 만들 때 불리는 콜백.
	 * 설정자: 여기 정적 초기화가 유일하다.
	 * 읽는 자: irq_domain_associate() 가 매핑마다 부른다.
	 * 값 범위: 항상 이 파일의 irq_map_generic_chip.
	 * 동기화: 읽기 전용 상수 테이블 — const 라서 .rodata 에 놓인다. */
	.unmap  = irq_unmap_generic_chip,
	/* [한국어] 매핑을 끊을 때 불리는 콜백.
	 * 설정자·읽는 자: map 과 대칭이며 irq_domain_disassociate() 가 부른다.
	 * 값 범위: 항상 irq_unmap_generic_chip.
	 * 동기화: 위와 같다. */
	.xlate	= irq_domain_xlate_onetwocell,
	/* [한국어] 디바이스 트리의 interrupts 속성을 해석하는 콜백.
	 * 설정자: 정적 초기화.
	 * 읽는 자: irq_create_of_mapping() 이 DT 노드를 해석할 때.
	 * 값 범위: onetwocell 은 "셀이 하나면 번호만, 둘이면 번호와 플래그"
	 *   라는 뜻이다 (kernel/irq/irqdomain.c 제공). 대부분의 단순 SoC
	 *   컨트롤러가 이 형식을 쓴다. 다른 형식이 필요한 드라이버는 이
	 *   테이블을 쓰지 못하고 자기 ops 를 만들어야 한다.
	 * 동기화: 위와 같다. */
};
EXPORT_SYMBOL_GPL(irq_generic_chip_ops);	/* [한국어] 드라이버가 도메인 생성 시 그대로 넘긴다 */

/**
 * irq_setup_generic_chip - Setup a range of interrupts with a generic chip
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @flags:	Flags for initialization
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Set up max. 32 interrupts starting from gc->irq_base. Note, this
 * initializes all interrupts to the primary irq_chip_type and its
 * associated handler.
 */
/*
 * [한국어]
 * irq_setup_generic_chip - 도메인 없이 인터럽트 구간을 칩에 붙인다
 *
 * @gc:    irq_alloc_generic_chip() 으로 만든 칩
 * @msk:   gc->irq_base 부터 세어 어느 선을 설정할지 나타내는 비트마스크
 * @flags: IRQ_GC_ 계열 설정 플래그
 * @clr:   각 서술자에서 지울 IRQ_ 플래그
 * @set:   각 서술자에 세울 IRQ_ 플래그
 * @return: 없음
 *
 * 도메인 경로의 irq_map_generic_chip() 에 대응하는 옛 방식이다. 결정적
 * 차이는 시점이다. 도메인 방식은 인터럽트가 실제로 요청될 때 하나씩
 * 매핑하지만, 이 방식은 초기화 때 구간 전체를 한 번에 붙인다. 그래서
 * 쓰이지 않는 선도 서술자를 차지한다.
 *
 * msk 비트 순회 관용구에 주목: msk 를 오른쪽으로 밀면서 최하위 비트를
 * 보고, i 는 계속 증가한다. 루프가 끝나는 조건이 "msk 가 0" 이므로,
 * 마스크의 최상위 1 비트 위에 있는 선들은 아예 보지 않는다. 루프가 끝난
 * 뒤 i 는 마지막으로 본 위치의 다음이므로 그것이 담당 개수가 된다.
 *
 * gc->irq_base 의 의미: 이 경로에서는 리눅스 인터럽트 번호다. 도메인
 * 경로에서는 하드웨어 번호였다. 같은 필드의 이 이중 의미가
 * irq_remove_generic_chip() 에서 gc->domain 을 검사하는 이유다.
 *
 * 실행 컨텍스트: 드라이버 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/irqchip 의 옛 드라이버 init → [이 함수] →
 *   irq_gc_init_mask_cache() / irq_set_chip_and_handler() / irq_modify_status()
 */
void irq_setup_generic_chip(struct irq_chip_generic *gc, u32 msk,
			    enum irq_gc_flags flags, unsigned int clr,
			    unsigned int set)
{
	struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 0 번 흐름 종류. 이 경로는 항상 기본 종류로만 설정한다 */
	struct irq_chip *chip = &ct->chip;	/* [한국어] 그 종류의 콜백 묶음 */
	unsigned int i;	/* [한국어] 현재 보고 있는 리눅스 인터럽트 번호 */

	scoped_guard (raw_spinlock, &gc_lock)	/* [한국어] 전역 칩 목록 조작. 이 경로는 초기화 시점이라 인터럽트가 켜져 있어 irqsave 가 없어도 된다 */
		list_add_tail(&gc->list, &gc_list);	/* [한국어] PM 콜백이 훑을 목록에 등록한다 */

	irq_gc_init_mask_cache(gc, flags);	/* [한국어] 캐시 포인터를 걸고 필요하면 하드웨어 값을 읽는다. 여기서는 칩당 한 번이 자연스럽다 — 이 함수 자체가 칩당 한 번 불리기 때문이다 */

	for (i = gc->irq_base; msk; msk >>= 1, i++) {	/* [한국어] 마스크를 오른쪽으로 밀며 리눅스 번호를 하나씩 올린다. msk 가 0 이 되면 끝 */
		if (!(msk & 0x01))	/* [한국어] 이 위치가 설정 대상인가 */
			continue;	/* [한국어] 건너뛴다. 다만 i 는 이미 증가했으므로 마지막 개수 계산에는 포함된다 */

		if (flags & IRQ_GC_INIT_NESTED_LOCK)	/* [한국어] 중첩되는 컨트롤러인가 */
			irq_set_lockdep_class(i, &irq_nested_lock_class,	/* [한국어] 별도 lockdep 클래스를 걸어 헛경고를 막는다 */
					      &irq_nested_request_class);

		if (!(flags & IRQ_GC_NO_MASK)) {	/* [한국어] 이 칩이 마스크 기능을 갖는가. 없는 칩은 d->mask 가 의미 없다 */
			struct irq_data *d = irq_get_irq_data(i);	/* [한국어] 이 번호의 irq_data */

			if (chip->irq_calc_mask)	/* [한국어] 칩이 비트 위치를 직접 계산하는가 */
				chip->irq_calc_mask(d);	/* [한국어] 드문 배치의 컨트롤러용 */
			else	/* [한국어] 보통의 경우 */
				d->mask = 1 << (i - gc->irq_base);	/* [한국어] 구간 시작으로부터의 거리를 비트 위치로 쓴다. 도메인 경로의 hw_irq % irqs_per_chip 과 같은 값이 되도록 뺄셈으로 맞춘다 */
		}
		irq_set_chip_and_handler(i, chip, ct->handler);	/* [한국어] 서술자에 칩과 흐름 처리기를 건다. 이 줄부터 이 선이 동작할 수 있다 */
		irq_set_chip_data(i, gc);	/* [한국어] chip_data 에 칩을 저장한다. 앞의 모든 콜백이 이것으로 gc 를 되찾는다 */
		irq_modify_status(i, clr, set);	/* [한국어] 드라이버가 지정한 IRQ_ 플래그를 적용한다 */
	}
	gc->irq_cnt = i - gc->irq_base;	/* [한국어] 담당 개수. 루프가 msk 소진으로 끝나므로 마스크의 최상위 1 비트까지의 길이다 — 중간의 0 비트도 개수에 포함된다 */
}
EXPORT_SYMBOL_GPL(irq_setup_generic_chip);	/* [한국어] 도메인을 쓰지 않는 드라이버의 설정 진입점 */

/**
 * irq_setup_alt_chip - Switch to alternative chip
 * @d:		irq_data for this interrupt
 * @type:	Flow type to be initialized
 *
 * Only to be called from chip->irq_set_type() callbacks.
 */
/*
 * [한국어]
 * irq_setup_alt_chip - 요청된 트리거 방식에 맞는 흐름 종류로 갈아탄다
 *
 * @d:    대상 인터럽트의 irq_data
 * @type: IRQ_TYPE_EDGE_RISING, IRQ_TYPE_LEVEL_HIGH 등 요청된 트리거 방식
 * @return: 0 맞는 종류를 찾아 전환함, -EINVAL 이 칩은 그 방식을 지원 안 함
 *
 * 무엇을 푸는 문제인가: 같은 인터럽트 선을 레벨 트리거로도 에지
 * 트리거로도 쓸 수 있는 컨트롤러가 있다. 두 방식은 흐름 처리기가 다르고
 * (handle_level_irq vs handle_edge_irq) ack 규약도 다르다. 즉 콜백
 * 묶음 전체가 달라진다.
 *
 * 그래서 이런 칩은 struct irq_chip_type 을 여러 개 두고, 각 종류가
 * 자기가 담당하는 트리거 방식을 ct->type 비트마스크로 표시해 둔다. 이
 * 함수는 요청된 방식을 담당하는 종류를 찾아 서술자를 통째로 바꿔 끼운다.
 *
 * 바꾸는 것이 두 가지라는 점이 중요하다: irq_data 의 chip 포인터(마스크
 * ·ack 콜백)와 서술자의 handle_irq(흐름 처리기). 둘 중 하나만 바꾸면
 * 예컨대 에지 처리기가 레벨용 ack 를 부르게 되어 인터럽트 폭주가 난다.
 *
 * 왜 irq_set_type 안에서만 불러야 하는가: 그 경로가 desc->lock 을 쥔
 * 상태이기 때문이다. 락 없이 서술자의 handle_irq 를 바꾸면 그 순간
 * 인터럽트가 들어와 옛 처리기와 새 콜백이 섞인다.
 *
 * 실행 컨텍스트: 프로세스 문맥 (irq_set_irq_type 경로), desc->lock 보유.
 *
 * 호출 체인:
 *   irq_set_irq_type() (kernel/irq/manage.c) → __irq_set_trigger() →
 *   chip->irq_set_type() (드라이버 구현) → [이 함수]
 */
int irq_setup_alt_chip(struct irq_data *d, unsigned int type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);	/* [한국어] 담당 범용 칩 */
	struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 흐름 종류 배열의 처음. 아래 루프에서 하나씩 민다 */
	unsigned int i;	/* [한국어] 순회용 */

	for (i = 0; i < gc->num_ct; i++, ct++) {	/* [한국어] 이 칩이 가진 모든 흐름 종류를 훑는다 */
		if (ct->type & type) {	/* [한국어] 이 종류가 요청된 트리거 방식을 담당하는가. 비트마스크라 한 종류가 여러 방식을 맡을 수 있다 */
			d->chip = &ct->chip;	/* [한국어] 마스크·ack·eoi 콜백 묶음을 통째로 바꾼다 */
			irq_data_to_desc(d)->handle_irq = ct->handler;	/* [한국어] 흐름 처리기도 함께 바꾼다. 콜백만 바꾸면 에지 처리기가 레벨용 ack 를 부르는 식의 어긋남이 생긴다 */
			return 0;	/* [한국어] 전환 완료. 첫 번째로 맞는 종류를 쓴다 */
		}
	}
	return -EINVAL;	/* [한국어] 이 칩은 요청된 트리거 방식을 지원하지 않는다. 호출자가 요청을 거절하도록 올린다 */
}
EXPORT_SYMBOL_GPL(irq_setup_alt_chip);	/* [한국어] 드라이버의 irq_set_type 구현이 부른다 */

/**
 * irq_remove_generic_chip - Remove a chip
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Remove up to 32 interrupts starting from gc->irq_base.
 */
/*
 * [한국어]
 * irq_remove_generic_chip - 칩을 목록에서 빼고 담당 서술자들을 되돌린다
 *
 * @gc:  없앨 칩
 * @msk: 되돌릴 선들의 비트마스크 (gc->irq_base 기준 상대 위치)
 * @clr: 각 서술자에서 지울 IRQ_ 플래그
 * @set: 각 서술자에 세울 IRQ_ 플래그
 * @return: 없음
 *
 * irq_setup_generic_chip() 의 반대이면서, 도메인 경로의 해제에서도
 * 쓰인다. 두 경로가 같은 함수를 쓸 수 있는 것은 gc->domain 검사 덕분이다.
 *
 * gc->irq_base 의 이중 의미가 여기서 갈린다. 도메인 기반 칩에서는 그
 * 필드가 하드웨어 번호라 서술자를 찾으려면 irq_find_mapping() 으로
 * 리눅스 번호를 얻어야 한다. 도메인 없는 칩에서는 이미 리눅스 번호라
 * 그냥 더하면 된다. 이 갈림이 없으면 도메인 칩에서 엉뚱한 서술자를
 * 건드리게 된다.
 *
 * 정리 순서가 중요하다. 흐름 처리기를 먼저 NULL 로 만드는데,
 * irq_set_handler(virq, NULL) 은 내부적으로 그 선을 마스크한다. 칩
 * 포인터를 먼저 지우면 마스크할 방법이 사라져 선이 열린 채 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_remove_generic_chips() / 드라이버 remove → [이 함수] →
 *   irq_set_handler() / irq_set_chip() / irq_modify_status()
 */
void irq_remove_generic_chip(struct irq_chip_generic *gc, u32 msk,
			     unsigned int clr, unsigned int set)
{
	unsigned int i, virq;	/* [한국어] i 는 칩 안 상대 위치, virq 는 그에 대응하는 리눅스 번호 */

	scoped_guard (raw_spinlock, &gc_lock)	/* [한국어] 전역 목록 조작 보호 */
		list_del(&gc->list);	/* [한국어] PM 콜백이 이 칩을 더 이상 보지 않게 한다. 아래 서술자 정리보다 먼저 해야 그 사이에 서스펜드가 반쯤 정리된 칩을 만지지 않는다 */

	for (i = 0; msk; msk >>= 1, i++) {	/* [한국어] 위 setup 과 같은 관용구. 다만 여기서는 i 가 0 부터 시작하는 상대 위치다 */
		if (!(msk & 0x01))	/* [한국어] 이 위치가 대상인가 */
			continue;	/* [한국어] 건너뛴다 */

		/*
		 * Interrupt domain based chips store the base hardware
		 * interrupt number in gc::irq_base. Otherwise gc::irq_base
		 * contains the base Linux interrupt number.
		 */
		if (gc->domain) {	/* [한국어] (위 영어 주석) 도메인 기반 칩인가. 그렇다면 irq_base 는 하드웨어 번호다 */
			virq = irq_find_mapping(gc->domain, gc->irq_base + i);	/* [한국어] 하드웨어 번호로 리눅스 번호를 역조회한다 */
			if (!virq)	/* [한국어] 이 선은 매핑된 적이 없는가 */
				continue;	/* [한국어] 정리할 서술자가 없다. 0 은 유효한 인터럽트 번호가 아니라 "없음" 을 뜻한다 */
		} else {	/* [한국어] 도메인 없는 옛 방식 칩 */
			virq = gc->irq_base + i;	/* [한국어] irq_base 가 이미 리눅스 번호라 그냥 더한다 */
		}

		/* Remove handler first. That will mask the irq line */
		irq_set_handler(virq, NULL);	/* [한국어] (위 영어 주석) 처리기를 떼면 코어가 그 선을 마스크한다. 반드시 칩 포인터를 지우기 전에 해야 한다 — 순서가 바뀌면 마스크할 콜백이 사라져 선이 열린 채 남는다 */
		irq_set_chip(virq, &no_irq_chip);	/* [한국어] 더미 칩으로 바꾼다. NULL 이 아닌 이유는 코어가 chip 포인터를 검사 없이 역참조하는 곳이 있어서다 */
		irq_set_chip_data(virq, NULL);	/* [한국어] gc 포인터를 지운다. 곧 kfree 될 메모리를 가리키는 참조를 남기지 않는다 */
		irq_modify_status(virq, clr, set);	/* [한국어] 설정 때 걸었던 IRQ_ 플래그를 되돌린다 */
	}
}
EXPORT_SYMBOL_GPL(irq_remove_generic_chip);	/* [한국어] 드라이버 remove 와 도메인 해제 양쪽에서 부른다 */

/*
 * [한국어]
 * irq_gc_get_irq_data - 이 칩을 대표할 irq_data 하나를 고른다
 *
 * @gc: 대상 범용 칩
 * @return: 대표 irq_data, 고를 수 없으면 NULL
 *
 * 왜 이런 함수가 필요한가: 아래 PM 콜백들이 부르는
 * chip->irq_suspend/irq_resume 는 시그니처가 irq_data 를 받는다. 그런데
 * 이 훅들은 인터럽트 하나가 아니라 칩 전체를 저장·복원하는 용도다.
 * 드라이버는 넘겨받은 irq_data 에서 chip_data(즉 gc)만 꺼내 쓴다.
 * 그러니 아무 irq_data 나 하나면 되는데, 그 "아무 하나" 를 고르는 것이
 * 생각보다 까다롭다.
 *
 * 도메인 없는 칩은 쉽다 — irq_base 가 리눅스 번호라 그것을 그대로 쓴다.
 * 도메인 기반 칩은 어느 하드웨어 번호가 실제로 매핑됐는지 알 수 없다.
 * 그래서 gc->installed 비트맵에서 가장 낮은 1 비트를 __ffs 로 찾아
 * 그 선을 대표로 쓴다. 하나도 설치되지 않았으면 NULL 을 반환해 호출자가
 * 훅 호출 자체를 건너뛰게 한다.
 *
 * 실행 컨텍스트: syscore 서스펜드·리줌 단계 — 다른 CPU 가 모두 멈추고
 * 인터럽트가 꺼진 상태다. 그래서 락 없이 gc 를 읽어도 안전하다.
 *
 * 호출 체인:
 *   irq_gc_suspend() / irq_gc_resume() / irq_gc_shutdown() → [이 함수] →
 *   irq_find_mapping() / irq_get_irq_data()
 */
static struct irq_data *irq_gc_get_irq_data(struct irq_chip_generic *gc)
{
	unsigned int virq;	/* [한국어] 대표로 고른 선의 리눅스 번호 */

	if (!gc->domain)	/* [한국어] 도메인 없는 옛 방식 칩인가 */
		return irq_get_irq_data(gc->irq_base);	/* [한국어] irq_base 가 곧 리눅스 번호다. 설치 여부를 따지지 않는데, 이 경로에서는 setup 때 구간 전체가 이미 붙었기 때문이다 */

	/*
	 * We don't know which of the irqs has been actually
	 * installed. Use the first one.
	 */
	if (!gc->installed)	/* [한국어] (위 영어 주석) 이 칩에 매핑된 선이 하나라도 있는가 */
		return NULL;	/* [한국어] 없으면 대표를 고를 수 없다. 호출자가 훅을 건너뛴다 — 아무 선도 안 쓰는 칩은 저장·복원할 것도 없다 */

	virq = irq_find_mapping(gc->domain, gc->irq_base + __ffs(gc->installed));	/* [한국어] __ffs 로 가장 낮은 설치 비트를 찾아 하드웨어 번호를 만들고, 그것을 리눅스 번호로 역조회한다. gc->installed 가 0 이 아님을 위에서 확인했으므로 __ffs 가 정의된다 */
	return virq ? irq_get_irq_data(virq) : NULL;	/* [한국어] 역조회가 실패하면 NULL. installed 비트와 도메인 매핑이 어긋나는 짧은 창이 있을 수 있어 방어한다 */
}

#ifdef CONFIG_PM	/* [한국어] 전원 관리를 쓰는 빌드에만 서스펜드·리줌 훅이 필요하다 */
/*
 * [한국어]
 * irq_gc_suspend - 시스템 서스펜드 시 모든 범용 칩의 상태를 저장한다
 *
 * @data: syscore 콜백 규약상 넘어오는 인자. 이 파일은 쓰지 않는다.
 * @return: 항상 0 (실패할 여지가 없다)
 *
 * 왜 여기 있는가: 서스펜드하면 인터럽트 컨트롤러의 전원이 끊기거나
 * 레지스터가 초기화될 수 있다. 마스크 설정, 트리거 방식 같은 것을
 * 저장해 두었다가 리줌 때 되돌려야 한다. 그 일을 드라이버마다 하지 않게
 * 이 라이브러리가 gc_list 를 훑어 각 칩의 훅을 대신 불러 준다.
 *
 * 훅이 두 종류다. ct->chip.irq_suspend 는 irq_data 를 받는 인터럽트
 * 단위 규약이고, gc->suspend 는 칩을 직접 받는 이 라이브러리 전용
 * 규약이다. 전자는 irq_chip 인터페이스를 그대로 쓸 수 있어 편하고,
 * 후자는 대표 선을 고를 필요가 없어 확실하다. 드라이버가 편한 쪽을
 * 고르면 된다.
 *
 * 실행 컨텍스트: syscore 단계 — 다른 CPU 는 모두 멈췄고 인터럽트도
 * 꺼져 있다. 그래서 gc_lock 을 잡지 않고 리스트를 훑어도 안전하다.
 *
 * 호출 체인:
 *   suspend_devices_and_enter() → syscore_suspend() →
 *   irq_gc_syscore_ops.suspend → [이 함수] → chip->irq_suspend() / gc->suspend()
 */
static int irq_gc_suspend(void *data)
{
	struct irq_chip_generic *gc;	/* [한국어] 리스트 순회용 */

	list_for_each_entry(gc, &gc_list, list) {	/* [한국어] 등록된 모든 범용 칩. 락 없이 훑는 것은 syscore 단계라 경쟁이 없어서다 */
		struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 0 번 흐름 종류만 본다. PM 훅은 종류별이 아니라 칩 단위 개념이라서다 */

		if (ct->chip.irq_suspend) {	/* [한국어] irq_chip 규약의 서스펜드 훅이 있는가 */
			struct irq_data *data = irq_gc_get_irq_data(gc);	/* [한국어] 대표 irq_data 를 고른다. 바깥 인자 data 를 가리지만, 그 인자는 어차피 쓰지 않는다 */

			if (data)	/* [한국어] 고를 수 있었는가 — 설치된 선이 하나도 없으면 NULL 이다 */
				ct->chip.irq_suspend(data);	/* [한국어] 드라이버가 chip_data 로 gc 를 꺼내 칩 전체를 저장한다 */
		}

		if (gc->suspend)	/* [한국어] 이 라이브러리 전용 훅도 있는가. 두 훅은 배타적이지 않아 둘 다 불릴 수 있다 */
			gc->suspend(gc);	/* [한국어] 칩을 직접 넘긴다 — 대표 선을 고를 필요가 없어 설치된 선이 없어도 동작한다 */
	}
	return 0;	/* [한국어] syscore_ops.suspend 는 int 를 반환하지만 여기서 실패할 일이 없다. 0 이 아니면 서스펜드 전체가 취소된다 */
}

/*
 * [한국어]
 * irq_gc_resume - 시스템 리줌 시 모든 범용 칩의 상태를 복원한다
 *
 * @data: syscore 콜백 규약상의 인자. 쓰지 않는다.
 * @return: 없음
 *
 * irq_gc_suspend() 의 반대인데, 훅을 부르는 순서가 뒤집혀 있다는 점이
 * 핵심이다. 서스펜드는 (1) chip.irq_suspend, (2) gc->suspend 순이지만
 * 리줌은 (1) gc->resume, (2) chip.irq_resume 순이다.
 *
 * 왜 뒤집는가: 자원 해제·획득의 일반 규칙이다. 나중에 저장한 것을 먼저
 * 복원해야 계층이 맞는다. 대개 gc->resume 이 레지스터 블록 자체를
 * 되살리는 낮은 층 작업이고, chip.irq_resume 이 개별 인터럽트 설정을
 * 되돌리는 높은 층 작업이라, 낮은 층을 먼저 세워야 한다.
 *
 * 실행 컨텍스트: syscore 리줌 단계. 여전히 다른 CPU 는 멈춰 있다.
 *
 * 호출 체인:
 *   syscore_resume() → irq_gc_syscore_ops.resume → [이 함수] →
 *   gc->resume() / chip->irq_resume()
 */
static void irq_gc_resume(void *data)
{
	struct irq_chip_generic *gc;	/* [한국어] 리스트 순회용 */

	list_for_each_entry(gc, &gc_list, list) {	/* [한국어] 등록 순서대로 훑는다. 꼬리에 붙였으므로 생성 순이다 */
		struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 0 번 흐름 종류 */

		if (gc->resume)	/* [한국어] 라이브러리 전용 훅을 먼저 부른다 — 서스펜드와 반대 순서다 */
			gc->resume(gc);	/* [한국어] 대개 레지스터 블록 자체를 되살리는 낮은 층 작업 */

		if (ct->chip.irq_resume) {	/* [한국어] irq_chip 규약의 리줌 훅 */
			struct irq_data *data = irq_gc_get_irq_data(gc);	/* [한국어] 대표 irq_data */

			if (data)	/* [한국어] 설치된 선이 있는가 */
				ct->chip.irq_resume(data);	/* [한국어] 개별 인터럽트 설정을 되돌리는 높은 층 작업 */
		}
	}
}
#else	/* [한국어] CONFIG_PM 이 꺼진 빌드 */
#define irq_gc_suspend NULL	/* [한국어] 아래 syscore_ops 초기화에서 참조되므로 이름 자체는 있어야 한다. NULL 이면 코어가 훅 호출을 건너뛴다 */
#define irq_gc_resume NULL	/* [한국어] 위와 같은 이유 */
#endif	/* [한국어] CONFIG_PM 분기의 끝 */

/*
 * [한국어]
 * irq_gc_shutdown - 시스템 종료·재부팅 직전에 컨트롤러를 정리한다
 *
 * @data: syscore 콜백 규약상의 인자. 쓰지 않는다.
 * @return: 없음
 *
 * 왜 필요한가: 킥스텍(kexec)이나 재부팅 직후, 새 커널이 인터럽트
 * 컨트롤러를 초기화하기 전에 옛 설정으로 인터럽트가 올라오면 아무도
 * 처리하지 못한다. 그것이 곧 부팅 실패로 이어진다. 이 훅에서 드라이버가
 * 자기 컨트롤러를 조용한 상태로 만들어 둔다.
 *
 * CONFIG_PM 밖에 있다는 점이 중요하다. 서스펜드·리줌은 전원 관리
 * 기능이지만 재부팅은 그것과 무관하게 항상 일어나므로, PM 을 끈 빌드에도
 * 이 훅은 있어야 한다.
 *
 * 훅이 한 종류뿐인 것도 위 두 함수와 다르다 — gc->shutdown 에 해당하는
 * 라이브러리 전용 훅은 없고 chip.irq_pm_shutdown 만 있다.
 *
 * 실행 컨텍스트: syscore 셧다운 단계. 다른 CPU 는 이미 멈췄다.
 *
 * 호출 체인:
 *   kernel_restart() / kernel_halt() → syscore_shutdown() →
 *   irq_gc_syscore_ops.shutdown → [이 함수] → chip->irq_pm_shutdown()
 */
static void irq_gc_shutdown(void *data)
{
	struct irq_chip_generic *gc;	/* [한국어] 리스트 순회용 */

	list_for_each_entry(gc, &gc_list, list) {	/* [한국어] 등록된 모든 칩 */
		struct irq_chip_type *ct = gc->chip_types;	/* [한국어] 0 번 흐름 종류 */

		if (ct->chip.irq_pm_shutdown) {	/* [한국어] 종료 훅이 있는가. 대부분의 드라이버는 제공하지 않는다 */
			struct irq_data *data = irq_gc_get_irq_data(gc);	/* [한국어] 대표 irq_data */

			if (data)	/* [한국어] 설치된 선이 있는가 */
				ct->chip.irq_pm_shutdown(data);	/* [한국어] 컨트롤러를 조용한 상태로 만든다. 다음 커널이 초기화하기 전에 인터럽트가 올라오지 않게 하려는 것이다 */
		}
	}
}

static const struct syscore_ops irq_gc_syscore_ops = {
	/* [한국어] 이 라이브러리가 코어 PM 에 등록하는 콜백 묶음.
	 * syscore 단계는 모든 장치 드라이버의 서스펜드가 끝나고 다른 CPU 가
	 * 멈춘 뒤, 시스템이 실제로 잠들기 직전에 실행된다. 인터럽트
	 * 컨트롤러는 다른 장치들이 살아 있는 동안 계속 필요하므로 이 늦은
	 * 시점이어야 한다. */
	.suspend = irq_gc_suspend,
	/* [한국어] 잠들기 직전에 불릴 저장 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: syscore_suspend() (drivers/base/syscore.c).
	 * 값 범위: CONFIG_PM 이 켜져 있으면 위 함수, 꺼져 있으면 NULL —
	 *   위쪽 #define 이 그 갈림을 만든다.
	 * 동기화: const 테이블이라 변경되지 않는다. */
	.resume = irq_gc_resume,
	/* [한국어] 깨어난 직후 불릴 복원 콜백.
	 * 설정자·읽는 자: suspend 와 대칭.
	 * 값 범위: CONFIG_PM 여부에 따라 함수 또는 NULL.
	 * 동기화: 위와 같다. */
	.shutdown = irq_gc_shutdown,
	/* [한국어] 종료·재부팅 직전에 불릴 정리 콜백.
	 * 설정자: 정적 초기화.
	 * 읽는 자: syscore_shutdown().
	 * 값 범위: 항상 위 함수 — CONFIG_PM 과 무관하게 존재한다. 재부팅은
	 *   전원 관리 기능이 아니라 언제나 일어나는 일이기 때문이다.
	 * 동기화: 위와 같다. */
};

static struct syscore irq_gc_syscore = {
	/* [한국어] 코어 PM 목록에 등록될 노드. 위 연산표를 감싸는 껍데기다.
	 * 등록 자체는 아래 initcall 이 한다. */
	.ops = &irq_gc_syscore_ops,
	/* [한국어] 이 노드가 가리키는 콜백 묶음.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: register_syscore() 가 목록에 넣은 뒤, PM 코어가 각
	 *   단계마다 이 포인터를 따라 콜백을 부른다.
	 * 값 범위: 항상 위 irq_gc_syscore_ops 의 주소.
	 * 동기화: 등록 후에는 바뀌지 않는다. 구조체 자체가 static 이라
	 *   수명이 커널 전체와 같아, 목록에 남은 포인터가 무효해질 일이 없다. */
};

/*
 * [한국어]
 * irq_gc_init_ops - 서스펜드·리줌·셧다운 훅을 코어 PM 에 등록한다
 *
 * @return: 항상 0 (initcall 규약상의 성공)
 *
 * 왜 device_initcall 인가: 이 등록은 인터럽트 컨트롤러 드라이버들이
 * 자기 칩을 gc_list 에 올린 뒤여도 되고 그 전이어도 된다 — 콜백은
 * 실행 시점에 리스트를 훑기 때문이다. 다만 첫 서스펜드가 일어나기
 * 전에는 반드시 끝나야 한다. device_initcall 은 그 조건을 넉넉히
 * 만족하는 늦은 단계다.
 *
 * 반환값이 항상 0 인 이유: register_syscore() 는 리스트에 노드를 넣는
 * 일만 하므로 실패할 수 없다. initcall 이 0 이 아닌 값을 반환하면 부팅
 * 로그에 경고가 남는다.
 *
 * 실행 컨텍스트: 부팅 중 initcall 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   do_initcalls() (init/main.c) → [이 함수] → register_syscore()
 */
static int __init irq_gc_init_ops(void)
{
	register_syscore(&irq_gc_syscore);	/* [한국어] PM 코어의 syscore 목록에 노드를 건다. 이 시점부터 서스펜드·리줌·셧다운 때 위 콜백들이 불린다 */
	return 0;	/* [한국어] initcall 규약상 성공 */
}
device_initcall(irq_gc_init_ops);	/* [한국어] 부팅 중 device 단계에 위 함수를 부르도록 등록한다. 첫 서스펜드보다 훨씬 이르다 */
