// SPDX-License-Identifier: GPL-2.0+
/*
 * APM X-Gene MSI Driver
 *
 * Copyright (c) 2014, Applied Micro Circuits Corporation
 * Author: Tanmay Inamdar <tinamdar@apm.com>
 *	   Duc Dang <dhdang@apm.com>
 */

/*
 * [한국어 설명] APM X-Gene v1 의 MSI 종단(termination) 드라이버 (pci-xgene-msi.c)
 *
 * === 파일의 역할 ===
 * PCIe 장치가 보내는 MSI 를 받아 CPU 인터럽트로 바꾸는 하드웨어 블록의
 * 드라이버다. 같은 SoC 의 호스트 브리지 드라이버(pci-xgene.c)와는 별개의
 * 디바이스 트리 노드이며, 서로를 직접 부르지 않는다 — MSI 컨트롤러가
 * 부모 인터럽트 도메인으로 등록되고, 브리지 쪽은 그 도메인을 이름으로
 * 찾아 쓴다.
 *
 * 이 하드웨어의 핵심은 **MSI 주소 자체가 목적지를 인코딩한다** 는 점이다.
 * 장치가 어느 주소에 쓰느냐로 어느 레지스터가 울릴지가 정해지고,
 * 데이터 필드가 그 레지스터의 어느 비트인지를 고른다. 8MB 짜리 주소
 * 영역 전체가 사실상 "인터럽트 번호를 주소로 표현한 표" 인 셈이다.
 *
 * 규모가 이렇다 — 프레임 16개 × 프레임당 레지스터 8개 × 레지스터당 16비트
 * = 2048개의 MSI 자리. 프레임마다 GIC 인터럽트 선이 하나씩 붙어 총 16선이다.
 *
 * 그런데 이 드라이버가 실제로 쓰는 것은 그중 **256개뿐** 이다. 아래
 * "친화도 문제" 를 보라.
 *
 * === 친화도(affinity) 문제와 그 해법 ===
 * MSI 의 목적지 CPU 를 바꾸려면 원칙적으로 주소와 데이터를 함께 다시
 * 써야 하는데, 그 둘을 원자적으로 바꿀 방법이 없다. 중간에 인터럽트가
 * 오면 엉뚱한 곳으로 간다.
 *
 * 이 드라이버의 해법은 **CPU 마다 프레임을 미리 나눠 주는 것** 이다.
 * 프레임 번호의 아래 3비트를 CPU 번호로 쓰기로 정하면, 목적지를 옮길 때
 * 주소만 고치면 되고 데이터는 그대로 둘 수 있다.
 *
 * 대가는 용량이다. 한 논리적 MSI 가 모든 CPU 의 대응 프레임 자리를
 * 통째로 예약해 버리므로, 쓸 수 있는 자리가 2048 에서 256 으로 준다.
 * 상류 주석(compute_hwirq 위)이 이 설계를 그대로 밝히고 있다.
 *
 * 그래서 hwirq 8비트의 뜻이 이렇게 된다:
 *   비트 7   : 프레임 번호의 최상위 비트
 *   비트 6~4 : 프레임 안의 레지스터 번호(0~7)
 *   비트 3~0 : MSI 데이터(0~15)
 * **프레임의 아래 3비트(=CPU)는 hwirq 에 들어가지 않는다.** 그래서 같은
 * hwirq 가 CPU 를 옮겨도 그대로 유지된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> xgene_msi_probe()
 *     -> 레지스터 창 매핑 -> 비트맵 할당 -> MSI 부모 도메인 생성
 *       -> xgene_msi_handler_setup()
 *          -> 프레임 16개 각각: 잔여 인터럽트 청소 -> GIC 인터럽트 확보
 *             -> CPU 고정 -> 연쇄(chained) 핸들러 연결
 *
 * 장치가 MSI 를 요청할 때:
 *   PCI 코어 -> MSI 상위 도메인 -> [이 파일] xgene_irq_domain_alloc()
 *     -> 비트맵에서 빈 hwirq 하나 -> irq_domain_set_info()
 *   이어서 코어가 메시지를 만든다:
 *     -> [이 파일] xgene_compose_msi_msg() -> 주소·데이터 계산
 *
 * 인터럽트가 올 때:
 *   장치의 MSI 쓰기 -> 종단 블록이 비트를 세움 -> GIC 선이 울림
 *     -> [이 파일] xgene_msi_isr() (연쇄 핸들러)
 *        -> MSIINTn 으로 어느 레지스터가 울렸는지 -> MSInIRx 로 어느 비트인지
 *           -> compute_hwirq() -> generic_handle_domain_irq()
 *              -> 장치 드라이버의 핸들러
 *
 * 실행 컨텍스트: probe 와 도메인 alloc/free 는 프로세스 컨텍스트(뮤텍스를
 * 쓴다), xgene_msi_isr() 는 하드 인터럽트 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 커널 irqdomain/MSI 계층. msi_create_parent_irq_domain() 으로
 *   부모 도메인을 만들고, msi_parent_ops 로 어떤 MSI 기능을 지원하는지
 *   알린다. 상위 도메인의 기본 구현(msi-lib)이 나머지를 채운다.
 * 아래쪽: GIC. 프레임 16개가 각각 GIC 선 하나를 차지하며, 이 파일은
 *   그 선에 연쇄 핸들러를 건다. GIC 드라이버 자체는 이 트리에 없다.
 * 옆쪽: pci-xgene.c(같은 SoC 의 호스트 브리지). 코드상의 호출 관계는
 *   없고, 디바이스 트리의 msi-parent 참조로만 이어진다.
 *
 * 데이터 흐름:
 *   디바이스 트리(레지스터 창, GIC 인터럽트 16개) -> probe
 *   장치의 MSI 쓰기 -> 주소가 프레임·레지스터를, 데이터가 비트를 고름
 *     -> 읽어서 지우기 -> hwirq 로 환산 -> 도메인 -> 장치 핸들러
 *
 * 공유 상태: **전역 포인터 하나(xgene_msi_ctrl)** 에 모든 것이 매달려 있다.
 *   이 SoC 에 MSI 블록이 하나뿐이라는 전제다. 비트맵만 뮤텍스로 지키고
 *   나머지 필드는 probe 이후 불변이다.
 *
 * === NVMe 관점 ===
 * MSI 자리가 256개로 제한되므로, 이 SoC 에서 여러 NVMe 컨트롤러가 큐마다
 * 벡터를 요구하면 금방 바닥난다. xgene_irq_domain_alloc() 이 한 번에
 * 하나씩만 잡는 구조라 MSI-X 여러 개를 붙이려면 그만큼 호출이 반복된다.
 *
 * === 주요 함수/구조체 요약 ===
 * compute_hwirq()            : 프레임·레지스터·데이터 셋을 hwirq 하나로 접는다.
 *                              CPU 비트를 버리는 것이 이 함수의 요점이다.
 * xgene_compose_msi_msg()    : 그 역방향. hwirq 와 현재 CPU 로 주소를 만든다.
 * xgene_msi_set_affinity()   : 하드웨어를 건드리지 않고 목표 CPU 만 기록한다.
 * xgene_irq_domain_alloc()   : 비트맵에서 hwirq 를 하나 잡는다.
 * xgene_msi_isr()            : 연쇄 핸들러. 두 겹 루프로 대기 비트를 훑는다.
 * xgene_msi_handler_setup()  : 프레임 16개를 청소하고 CPU 에 고정한다.
 * struct xgene_msi           : 이 드라이버의 상태 전부.
 */
/* [한국어] FIELD_PREP/FIELD_GET. 이 파일은 레지스터 오프셋조차 비트 필드로 조립한다. */
#include <linux/bitfield.h>
/* [한국어] num_possible_cpus(). NR_MSI_VEC 매크로가 이것을 품고 있다. */
#include <linux/cpu.h>
/* [한국어] irqreturn_t 와 인터럽트 플래그 정의. */
#include <linux/interrupt.h>
/* [한국어] 인터럽트 도메인 — 이 드라이버의 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] MODULE_ 계열 선언용. 다만 이 파일은 builtin 으로만 등록된다. */
#include <linux/module.h>
/* [한국어] struct msi_msg 와 MSI 계층 정의. */
#include <linux/msi.h>
/* [한국어] chained_irq_enter/exit. 연쇄 핸들러를 쓰기 때문에 필요하다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info 등 상위 도메인 기본 구현. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] PCI 관련 정의. MSI 가 PCI 장치의 것이기 때문이다. */
#include <linux/pci.h>
/* [한국어] 플랫폼 드라이버 등록과 자원 확보. */
#include <linux/platform_device.h>
/* [한국어] of_fwnode_handle 등 디바이스 트리 연동. */
#include <linux/of_pci.h>

/* [한국어] MSInIRx 영역의 시작 오프셋. 0 이라 사실상 창의 맨 앞이지만, 아래
 * MSI_INT0 과 짝을 이뤄 두 영역을 이름으로 구분하려고 정의해 두었다. */
#define MSI_IR0			0x000000
/* [한국어] MSIINTn 영역의 시작 오프셋. 비트 23 하나가 IR 영역과 INT 영역을
 * 가른다 — 즉 앞 8MB 가 IR, 그 뒤가 INT 다. */
#define MSI_INT0		0x800000
/* [한국어] 프레임 하나에 든 MSInIRx 레지스터 수. ISR 바깥 루프의 상한이다. */
#define IDX_PER_GROUP		8
/* [한국어] 레지스터 하나가 담는 MSI 비트 수. ISR 안쪽 루프의 상한이다. */
#define IRQS_PER_IDX		16
/* [한국어] 프레임 수이자 이 블록이 쓰는 GIC 인터럽트 선 수다. */
#define NR_HW_IRQS		16
/* [한국어] 곱하면 2048 — 하드웨어가 가진 MSI 자리의 총수다. */
#define NR_MSI_BITS		(IDX_PER_GROUP * IRQS_PER_IDX * NR_HW_IRQS)
/* [한국어] 실제로 쓸 수 있는 자리 수. **대문자 이름이지만 컴파일 상수가 아니다** —
 * num_possible_cpus() 가 실행 시점에 평가된다. CPU 가 8개면 256 이 되어,
 * compute_hwirq() 가 만들 수 있는 hwirq 8비트와 정확히 맞아떨어진다. */
#define NR_MSI_VEC		(NR_MSI_BITS / num_possible_cpus())

/* [한국어] MSInIRx 주소에서 프레임 번호가 놓이는 자리(4비트). 간격이 512kB 다. */
#define MSI_GROUP_MASK		GENMASK(22, 19)
/* [한국어] 같은 주소에서 레지스터 번호가 놓이는 자리(3비트). 간격이 64kB 다. */
#define MSI_INDEX_MASK		GENMASK(18, 16)
/* [한국어] MSIINTn 주소에서 프레임 번호가 놓이는 자리(4비트).
 * 위 GROUP 마스크와 **비트 19 가 겹치는데**, 이 마스크에 들어가는 값이
 * 0~7 이라 실제로는 18~16비트만 쓰여 충돌하지 않는다. */
#define MSI_INTR_MASK		GENMASK(19, 16)

/* [한국어] hwirq 안에서 레지스터 번호가 놓이는 자리(비트 6~4). */
#define MSInRx_HWIRQ_MASK	GENMASK(6, 4)
/* [한국어] hwirq 안에서 MSI 데이터가 놓이는 자리(비트 3~0). */
#define DATA_HWIRQ_MASK		GENMASK(3, 0)

struct xgene_msi {
	/* [한국어] 이 컨트롤러가 만든 부모 인터럽트 도메인.
	 * 설정자: xgene_allocate_domains().
	 * 읽는 자: ISR 이 hwirq 를 이 도메인으로 올리고, remove 가 없앤다.
	 * 값 범위: 유효한 도메인 포인터, 또는 만들기 전·후의 NULL.
	 * NULL 검사가 remove 에 있는 것은 probe 실패 경로에서도 불리기 때문이다.
	 * 동기화: probe 이후 불변이며, ISR 은 읽기만 한다. */
	struct irq_domain	*inner_domain;
	/* [한국어] 레지스터 창의 **물리** 시작 주소.
	 * 설정자: probe 가 자원의 시작 주소를 그대로 넣는다.
	 * 읽는 자: xgene_compose_msi_msg() 하나뿐. 장치가 MSI 를 보낼 주소를
	 * 계산하는 기준이다.
	 * 값 범위: 8MB+1MB 크기 창의 물리 시작 주소.
	 * 동기화: probe 이후 불변.
	 * **아래 msi_regs 와 짝** 이며 같은 창의 물리·가상 두 얼굴이다. CPU 는
	 * 가상 주소로 읽고, 장치는 물리 주소로 쓴다. */
	u64			msi_addr;
	/* [한국어] 레지스터 창의 **가상** 주소.
	 * 설정자: probe 의 devm_platform_get_and_ioremap_resource().
	 * 읽는 자: xgene_msi_ir_read() 와 xgene_msi_int_read().
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 이후 불변. */
	void __iomem		*msi_regs;
	/* [한국어] hwirq 배정 상태 비트맵.
	 * 설정자: xgene_msi_init_allocator() 가 0 으로 채워 할당하고,
	 * xgene_irq_domain_alloc()/free() 가 비트를 세우고 지운다.
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: 비트 수는 NR_MSI_VEC — 실행 시점에 정해진다.
	 * 동기화: 아래 bitmap_lock 이 지킨다. ISR 은 이 비트맵을 건드리지 않는다. */
	unsigned long		*bitmap;
	/* [한국어] 위 비트맵을 지키는 뮤텍스.
	 * 설정자: xgene_msi_init_allocator() 가 초기화한다.
	 * 읽는 자: alloc/free 두 곳뿐.
	 * 값 범위: 뮤텍스.
	 * 동기화: **뮤텍스라 인터럽트 문맥에서 쓸 수 없다.** ISR 이 비트맵을
	 * 읽지 않는 설계라 성립한다 — ISR 은 하드웨어가 준 비트를 그대로
	 * hwirq 로 접을 뿐이라 배정 상태를 알 필요가 없다. */
	struct mutex		bitmap_lock;
	/* [한국어] 프레임별 GIC 인터럽트 번호 16개.
	 * 설정자: xgene_msi_handler_setup().
	 * 읽는 자: remove 가 핸들러를 뗄 때 쓰고, **ISR 은 원소의 주소를 받아
	 * 그 주소에서 배열 시작을 빼 프레임 번호를 되찾는다** — 값이 아니라
	 * 위치가 정보인 셈이다.
	 * 값 범위: 유효한 가상 인터럽트 번호, 또는 아직 설정 전인 0.
	 * 0 검사가 remove 에 있는 것이 부분 초기화 상태를 견디게 한다.
	 * 동기화: probe 이후 불변. */
	unsigned int		gic_irq[NR_HW_IRQS];
};

/* Global data */
static struct xgene_msi *xgene_msi_ctrl;

/*
 * X-Gene v1 has 16 frames of MSI termination registers MSInIRx, where n is
 * frame number (0..15), x is index of registers in each frame (0..7).  Each
 * 32b register is at the beginning of a 64kB region, each frame occupying
 * 512kB (and the whole thing 8MB of PA space).
 *
 * Each register supports 16 MSI vectors (0..15) to generate interrupts. A
 * write to the MSInIRx from the PCI side generates an interrupt. A read
 * from the MSInRx on the CPU side returns a bitmap of the pending MSIs in
 * the lower 16 bits. A side effect of this read is that all pending
 * interrupts are acknowledged and cleared).
 *
 * Additionally, each MSI termination frame has 1 MSIINTn register (n is
 * 0..15) to indicate the MSI pending status caused by any of its 8
 * termination registers, reported as a bitmap in the lower 8 bits. Each 32b
 * register is at the beginning of a 64kB region (and overall occupying an
 * extra 1MB).
 *
 * There is one GIC IRQ assigned for each MSI termination frame, 16 in
 * total.
 *
 * The register layout is as follows:
 * MSI0IR0			base_addr
 * MSI0IR1			base_addr +  0x10000
 * ...				...
 * MSI0IR6			base_addr +  0x60000
 * MSI0IR7			base_addr +  0x70000
 * MSI1IR0			base_addr +  0x80000
 * MSI1IR1			base_addr +  0x90000
 * ...				...
 * MSI1IR7			base_addr +  0xF0000
 * MSI2IR0			base_addr + 0x100000
 * ...				...
 * MSIFIR0			base_addr + 0x780000
 * MSIFIR1			base_addr + 0x790000
 * ...				...
 * MSIFIR7			base_addr + 0x7F0000
 * MSIINT0			base_addr + 0x800000
 * MSIINT1			base_addr + 0x810000
 * ...				...
 * MSIINTF			base_addr + 0x8F0000
 */

/* MSInIRx read helper */
/* [한국어]
 * xgene_msi_ir_read - MSInIRx 레지스터를 읽어 대기 비트를 얻고 **동시에 지운다**
 *
 * @msi: 드라이버 상태.
 * @msi_grp: 프레임 번호(0~15).
 * @msir_idx: 그 프레임 안의 레지스터 번호(0~7).
 * @return: 아래 16비트에 대기 중인 MSI 비트맵.
 *
 * **읽기가 곧 확인응답(ack)이다.** 옆의 상류 주석이 밝히듯 이 레지스터를
 * 읽으면 대기 중이던 인터럽트가 모두 지워진다. 그래서 이 함수를 부르는
 * 쪽은 반환값을 반드시 처리해야 한다 — 버리면 그 인터럽트는 영영 사라진다.
 *
 * 주소 계산이 이 파일의 핵심 관용이다. 레지스터 번호를 오프셋 표로 두지
 * 않고 **비트 필드로 조립한다** — 프레임은 22~19비트, 레지스터 번호는
 * 18~16비트다. 즉 프레임 하나가 512kB, 레지스터 하나가 64kB 간격이다.
 *
 * relaxed 판을 쓴다. 이 읽기 전후로 다른 메모리 접근과의 순서를 강제할
 * 필요가 없기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥(ISR)과 초기화 청소. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xgene_msi_isr() / xgene_msi_handler_setup() → [이 함수] → readl_relaxed()
 */
static u32 xgene_msi_ir_read(struct xgene_msi *msi, u32 msi_grp, u32 msir_idx)
{
	/* [한국어] 창의 가상 주소에 IR 영역 오프셋과 조립한 필드를 더해 읽는다.
	 * **이 읽기 자체가 확인응답** 이므로 반환값을 반드시 처리해야 한다. */
	return readl_relaxed(msi->msi_regs + MSI_IR0 +
			     (FIELD_PREP(MSI_GROUP_MASK, msi_grp) |
			      FIELD_PREP(MSI_INDEX_MASK, msir_idx)));
}

/* MSIINTn read helper */
/* [한국어]
 * xgene_msi_int_read - MSIINTn 을 읽어 그 프레임의 어느 레지스터가 울렸는지 얻는다
 *
 * @msi: 드라이버 상태.
 * @msi_grp: 프레임 번호(0~15).
 * @return: 아래 8비트에 레지스터별 대기 여부.
 *
 * 위 함수의 한 단계 위 요약본이다. 프레임 하나에 레지스터가 8개 있는데,
 * 그중 어느 것에 대기 중인 MSI 가 있는지를 비트 8개로 알려 준다.
 *
 * 이것이 있어 ISR 이 레지스터 8개를 모두 읽지 않아도 된다 — 울린 것만
 * 골라 읽는다.
 *
 * **주소 필드가 위와 다르다.** 여기서는 프레임 번호를 19~16비트에 놓는다
 * (MSI_INTR_MASK). 기준 오프셋도 0x800000 으로, 비트 23 이 IR 영역과 INT
 * 영역을 가른다. 즉 INT 영역에서는 프레임 간격이 64kB 다.
 *
 * 이 읽기에는 지우는 부작용이 없다 — 아래 MSInIRx 를 읽어야 실제로 지워진다.
 *
 * 실행 컨텍스트: 인터럽트 문맥과 초기화 청소. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xgene_msi_isr() / xgene_msi_handler_setup() → [이 함수] → readl_relaxed()
 */
static u32 xgene_msi_int_read(struct xgene_msi *msi, u32 msi_grp)
{
	return readl_relaxed(msi->msi_regs + MSI_INT0 +
			     FIELD_PREP(MSI_INTR_MASK, msi_grp));
}

/*
 * In order to allow an MSI to be moved from one CPU to another without
 * having to repaint both the address and the data (which cannot be done
 * atomically), we statically partitions the MSI frames between CPUs. Given
 * that XGene-1 has 8 CPUs, each CPU gets two frames assigned to it
 *
 * We adopt the convention that when an MSI is moved, it is configured to
 * target the same register number in the congruent frame assigned to the
 * new target CPU. This reserves a given MSI across all CPUs, and reduces
 * the MSI capacity from 2048 to 256.
 *
 * Effectively, this amounts to:
 * - hwirq[7]::cpu[2:0] is the target frame number (n in MSInIRx)
 * - hwirq[6:4] is the register index in any given frame (x in MSInIRx)
 * - hwirq[3:0] is the MSI data
 */
/* [한국어]
 * compute_hwirq - 프레임·레지스터·데이터 셋을 hwirq 하나로 접는다
 *
 * @frame: 프레임 번호(0~15).
 * @index: 프레임 안의 레지스터 번호(0~7).
 * @data: MSI 데이터 값(0~15).
 * @return: 8비트 hwirq.
 *
 * **이 파일에서 가장 중요한 함수다.** 위 상류 주석이 설계 근거를 밝히고
 * 있으며, 요점은 *프레임 번호의 아래 3비트를 버린다* 는 것이다.
 *
 * 접는 방식이 이렇다.
 *   frame 의 비트 3     → hwirq 의 비트 7
 *   index(3비트)        → hwirq 의 비트 6~4
 *   data(4비트)         → hwirq 의 비트 3~0
 *
 * frame 의 비트 2~0 은 어디에도 들어가지 않는다. 그 자리가 목표 CPU 번호이며,
 * hwirq 에서 빼 두어야 CPU 를 옮겨도 hwirq 가 변하지 않는다.
 *
 * 그 대가로 표현 가능한 hwirq 가 2^8 = 256 개로 줄어든다. 상류 주석이
 * "2048 에서 256 으로" 라고 적은 것이 이 뜻이다.
 *
 * 반대 방향은 xgene_compose_msi_msg() 가 맡는다. 그쪽은 hwirq 의 비트 7 과
 * 현재 CPU 를 합쳐 프레임을 되살린다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 순수 계산이며 부작용이 없다.
 *
 * 에러 경로: 없다. 인자 범위를 검사하지 않는다 — 호출부가 이미 비트 개수로
 * 제한된 값만 넘긴다.
 *
 * 호출 체인:
 *   xgene_msi_isr() → [이 함수]
 */
static irq_hw_number_t compute_hwirq(u8 frame, u8 index, u8 data)
{
	return (FIELD_PREP(BIT(7), FIELD_GET(BIT(3), frame))	|
		FIELD_PREP(MSInRx_HWIRQ_MASK, index)		|
		/* [한국어] 데이터 4비트를 아래에 놓는다. 세 조각이 겹치지 않아 OR 로 합쳐진다. */
		FIELD_PREP(DATA_HWIRQ_MASK, data));
}

/* [한국어]
 * xgene_compose_msi_msg - hwirq 와 현재 목표 CPU 로 MSI 주소·데이터를 만든다
 *
 * @data: 이 인터럽트의 irq_data. hwirq 와 유효 친화도를 담고 있다.
 * @msg: 채워 넣을 MSI 메시지.
 *
 * compute_hwirq() 의 역방향이며, 장치가 어디에 무엇을 쓸지를 정한다.
 *
 * 세 단계다.
 * 1. 유효 친화도에서 목표 CPU 를 꺼낸다. 이 값이 프레임의 아래 3비트가 된다.
 * 2. hwirq 에서 레지스터 번호를 꺼내고, hwirq 의 비트 7 과 CPU 를 합쳐
 *    프레임 번호를 되살린다.
 * 3. 창의 기준 주소에 프레임·레지스터를 비트 필드로 얹어 목표 주소를 만든다.
 *
 * **친화도가 바뀌면 이 함수만 다시 불리면 된다.** 주소만 달라지고 데이터는
 * 그대로이므로, 상류 주석이 말한 "주소·데이터를 원자적으로 함께 바꿀 수
 * 없다" 는 문제를 피한다.
 *
 * 주소 조립에서 프레임은 22~19비트, 레지스터 번호는 19~16비트 자리를 쓴다.
 * 두 마스크가 **비트 19 에서 겹치는데**, 레지스터 번호가 0~7 이라 실제로는
 * 18~16비트만 채워져 충돌하지 않는다. 마스크 정의만 보면 위험해 보이지만
 * 값의 범위가 이를 막고 있는 구조다.
 *
 * 실행 컨텍스트: MSI 설정과 친화도 변경. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 코어(설정/재설정) → irq_chip.irq_compose_msi_msg == [이 함수]
 */
static void xgene_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct xgene_msi *msi = irq_data_get_irq_chip_data(data);
	u64 target_addr;
	/* [한국어] 프레임 번호와 레지스터 번호를 담을 자리. */
	u32 frame, msir;
	/* [한국어] 목표 CPU 번호. 프레임의 아래 3비트가 된다. */
	int cpu;

	cpu	= cpumask_first(irq_data_get_effective_affinity_mask(data));
	/* [한국어] hwirq 에서 레지스터 번호(비트 6~4)를 꺼낸다. */
	msir	= FIELD_GET(MSInRx_HWIRQ_MASK, data->hwirq);
	/* [한국어] 프레임 번호를 되살린다 — hwirq 의 비트 7 을 프레임의 비트 3 자리로
	 * 옮기고, 아래 3비트에 CPU 번호를 채운다. compute_hwirq() 가 버린
	 * 정보를 여기서 현재 친화도로 되채우는 것이다. */
	frame	= FIELD_PREP(BIT(3), FIELD_GET(BIT(7), data->hwirq)) | cpu;

	target_addr = msi->msi_addr;
	/* [한국어] 프레임과 레지스터 번호를 주소 비트에 얹는다. 이 덧셈의 결과가 곧
	 * 장치가 쓸 주소이며, 그 주소만으로 목적지가 결정된다. */
	target_addr += (FIELD_PREP(MSI_GROUP_MASK, frame) |
			/* [한국어] 레지스터 번호는 18~16비트에 들어간다(값이 0~7 이므로). */
			FIELD_PREP(MSI_INTR_MASK, msir));

	msg->address_hi = upper_32_bits(target_addr);
	/* [한국어] 주소를 32비트 둘로 나눠 담는다. PCIe MSI 메시지의 형식이다. */
	msg->address_lo = lower_32_bits(target_addr);
	/* [한국어] 데이터에는 hwirq 의 아래 4비트를 그대로 쓴다. **친화도가 바뀌어도
	 * 이 값은 변하지 않는다** — 그것이 이 설계의 요점이다. */
	msg->data = FIELD_GET(DATA_HWIRQ_MASK, data->hwirq);
}

/* [한국어]
 * xgene_msi_set_affinity - 목표 CPU 를 기록만 하고 하드웨어는 건드리지 않는다
 *
 * @irqdata: 이 인터럽트의 irq_data.
 * @mask: 요청된 CPU 집합.
 * @force: 강제 여부. 쓰지 않는다.
 * @return: 언제나 IRQ_SET_MASK_OK.
 *
 * **레지스터를 하나도 쓰지 않는다.** 요청된 집합의 첫 CPU 를 유효 친화도로
 * 기록해 둘 뿐이다.
 *
 * 그것으로 충분한 이유는 반환값에 있다. IRQ_SET_MASK_OK 를 돌려주면 MSI
 * 상위 계층이 **메시지를 다시 만들어 장치에 쓴다** — 즉 실제 목적지 변경은
 * 그 뒤에 불리는 xgene_compose_msi_msg() 가 새 주소를 계산해 처리한다.
 * IRQ_SET_MASK_OK_DONE 이었다면 그 재작성이 생략된다.
 *
 * 집합에 CPU 가 여럿 있어도 첫 번째만 쓴다. 이 하드웨어가 한 MSI 를 여러
 * CPU 로 분산시킬 수 없기 때문이다.
 *
 * 실행 컨텍스트: 친화도 변경. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 요청된 집합이 비어 있는 경우를 검사하지 않으나,
 * 그런 호출은 상위 계층이 거른다.
 *
 * 호출 체인:
 *   irq 코어 → irq_chip.irq_set_affinity == [이 함수]
 *     → irq_data_update_effective_affinity()
 *     → (이어서 코어가) xgene_compose_msi_msg()
 */
static int xgene_msi_set_affinity(struct irq_data *irqdata,
				  const struct cpumask *mask, bool force)
{
	int target_cpu = cpumask_first(mask);

	/* [한국어] 요청된 첫 CPU 를 유효 친화도로 기록한다. 하드웨어는 건드리지 않는다 —
	 * 실제 목적지 변경은 옆의 상류 주석대로 코어가 메시지를 다시 만들 때
	 * xgene_compose_msi_msg() 가 처리한다. */
	irq_data_update_effective_affinity(irqdata, cpumask_of(target_cpu));

	/* Force the core code to regenerate the message */
	/* [한국어] 메시지 재작성을 유발하는 반환값이다. _DONE 이었다면 생략된다. */
	return IRQ_SET_MASK_OK;
}

static struct irq_chip xgene_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 에 보일 이름. */
	.name			= "MSI",
	/* [한국어] 친화도 변경 요청을 받는다. */
	.irq_set_affinity       = xgene_msi_set_affinity,
	.irq_compose_msi_msg	= xgene_compose_msi_msg,
};

/* [한국어]
 * xgene_irq_domain_alloc - 비트맵에서 빈 hwirq 를 하나 잡아 인터럽트에 묶는다
 *
 * @domain: 이 파일이 만든 부모 도메인.
 * @virq: 배정할 가상 인터럽트 번호.
 * @nr_irqs: 요청된 개수.
 * @args: 상위 계층이 넘긴 인자. 쓰지 않는다.
 * @return: 0 = 성공, -ENOSPC = 자리 없음.
 *
 * [상류 코드 관찰] **nr_irqs 를 무시하고 언제나 하나만 잡는다.** 여럿을
 * 요청받아도 첫 하나만 설정하므로, 연속 배정이 필요한 MSI(다중 벡터 MSI)는
 * 이 도메인에서 성립하지 않는다. 이 파일이 msi_parent_ops 에 MSI_FLAG_PCI_MSIX
 * 만 더해 두는 것과 맞물린 선택으로 보인다. 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다.
 *
 * 비트맵 잠금 구간이 최소화돼 있다 — 자리를 찾아 세우는 것까지만 뮤텍스
 * 안에서 하고, 실패 판정과 도메인 등록은 밖에서 한다.
 *
 * irqd_set_resend_when_in_progress() 가 마지막에 붙는 것이 중요하다. 이
 * 하드웨어는 MSI 를 마스크할 수단이 없어, 핸들러가 도는 중에 같은 MSI 가
 * 또 오면 그대로 유실될 수 있다. 이 표시가 있으면 irq 코어가 그 경우를
 * 기억했다가 나중에 다시 올려 준다.
 *
 * handle_simple_irq 를 쓰는 것도 같은 맥락이다 — 마스크·언마스크가 없는
 * 흐름이다.
 *
 * 실행 컨텍스트: MSI 배정. 프로세스 컨텍스트이며 뮤텍스를 잡는다.
 *
 * 에러 경로: 비트맵이 가득 찼으면 -ENOSPC 를 돌려주고, 상위 계층이 장치의
 * MSI 요청을 실패로 처리한다.
 *
 * 호출 체인:
 *   MSI 코어 → irq_domain_ops.alloc == [이 함수]
 *     → find_first_zero_bit() → set_bit() → irq_domain_set_info()
 */
static int xgene_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct xgene_msi *msi = domain->host_data;
	irq_hw_number_t hwirq;
/* [한국어] 잡은 hwirq 를 담을 자리. */

	mutex_lock(&msi->bitmap_lock);

	hwirq = find_first_zero_bit(msi->bitmap, NR_MSI_VEC);
	/* [한국어] 빈 자리를 찾았는지 확인한다. 못 찾으면 비트맵 크기가 그대로 돌아온다. */
	if (hwirq < NR_MSI_VEC)
		/* [한국어] 찾은 자리를 점유 표시한다. 잠금 안에서 해야 두 스레드가 같은 자리를 잡지 않는다. */
		set_bit(hwirq, msi->bitmap);

	mutex_unlock(&msi->bitmap_lock);

	if (hwirq >= NR_MSI_VEC)
		/* [한국어] 자리가 없으면 -ENOSPC. 상위 계층이 이 장치의 MSI 요청을 실패로
		 * 처리한다. 256개뿐이라 장치가 많으면 실제로 닿을 수 있는 한계다. */
		return -ENOSPC;

	irq_domain_set_info(domain, virq, hwirq,
			    /* [한국어] 이 파일의 하위 irq_chip 과 드라이버 상태를 함께 심는다. 뒤에서
			     * irq_data_get_irq_chip_data() 로 되찾는 것이 이 상태다. */
			    &xgene_msi_bottom_irq_chip, domain->host_data,
			    handle_simple_irq, NULL, NULL);
	irqd_set_resend_when_in_progress(irq_get_irq_data(virq));

	return 0;
}

/* [한국어]
 * xgene_irq_domain_free - 잡아 둔 hwirq 를 비트맵에 되돌린다
 *
 * @domain: 이 파일이 만든 부모 도메인.
 * @virq: 반납할 가상 인터럽트 번호.
 * @nr_irqs: 반납 개수.
 *
 * xgene_irq_domain_alloc() 의 짝이다.
 *
 * virq 에서 irq_data 를 거쳐 hwirq 를 되찾아 그 비트를 지운다. 배정 때
 * irq_domain_set_info() 가 심어 둔 chip_data 를 통해 드라이버 상태에도 닿는다.
 *
 * alloc 쪽처럼 여기서도 nr_irqs 를 비트맵 정리에 쓰지 않는다 — 하나만
 * 잡았으니 하나만 지우는 것으로 짝이 맞는다. 다만 마지막의 부모 해제에는
 * nr_irqs 를 그대로 넘긴다.
 *
 * 실행 컨텍스트: MSI 해제. 프로세스 컨텍스트이며 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다. irq_domain_get_irq_data() 가 NULL 을 돌려주는 경우를
 * 검사하지 않으나, 코어가 배정된 virq 로만 부른다.
 *
 * 호출 체인:
 *   MSI 코어 → irq_domain_ops.free == [이 함수]
 *     → clear_bit() → irq_domain_free_irqs_parent()
 */
static void xgene_irq_domain_free(struct irq_domain *domain,
				  unsigned int virq, unsigned int nr_irqs)
{
	/* [한국어] virq 로 irq_data 를 되찾는다. 여기에 hwirq 가 들어 있다. */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 배정 때 심어 둔 드라이버 상태를 chip_data 에서 꺼낸다. */
	struct xgene_msi *msi = irq_data_get_irq_chip_data(d);

	/* [한국어] 비트맵을 고치는 동안 잠근다. */
	mutex_lock(&msi->bitmap_lock);

	/* [한국어] 점유 표시를 지워 자리를 되돌린다. */
	clear_bit(d->hwirq, msi->bitmap);

	/* [한국어] 잠금을 푼다. */
	mutex_unlock(&msi->bitmap_lock);

	/* [한국어] 상위 계층이 잡아 둔 자원도 함께 해제하게 한다. */
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

static const struct irq_domain_ops xgene_msi_domain_ops = {
	/* [한국어] MSI 배정 요청이 오면 이 함수가 받는다. */
	.alloc  = xgene_irq_domain_alloc,
	/* [한국어] 해제 요청도 마찬가지다. */
	.free   = xgene_irq_domain_free,
};

static const struct msi_parent_ops xgene_msi_parent_ops = {
	/* [한국어] 일반 MSI 기능 전부에 더해 MSI-X 를 지원한다고 알린다.
	 * 다중 벡터 MSI(MSI_FLAG_MULTI_PCI_MSI)는 여기에 없다 — alloc 이 한 번에
	 * 하나만 잡는 구현과 맞아떨어진다. */
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   /* [한국어] MSI-X 지원을 명시한다. */
				   MSI_FLAG_PCI_MSIX),
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS),
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * xgene_allocate_domains - MSI 부모 인터럽트 도메인을 만든다
 *
 * @node: 이 컨트롤러의 디바이스 트리 노드. 도메인의 이름표가 된다.
 * @msi: 드라이버 상태. 도메인의 host_data 로 들어간다.
 * @return: 0 = 성공, -ENOMEM = 실패.
 *
 * 도메인 하나만 만든다. 상위(장치용) 도메인은 커널의 MSI 계층이 필요할 때
 * 자동으로 만들어 주며, 그 규칙을 xgene_msi_parent_ops 가 정한다.
 *
 * 정보 구조체에 넣는 것이 넷이다 — 노드 핸들, 이 파일의 alloc/free 표,
 * 자리 수(NR_MSI_VEC), 그리고 드라이버 상태다.
 *
 * **size 에 들어가는 NR_MSI_VEC 가 컴파일 상수가 아니다.** 그 매크로가
 * num_possible_cpus() 를 품고 있어 실행 시점에 계산된다. 이름과 대문자
 * 표기만 보면 상수로 오해하기 쉬운 자리다.
 *
 * 디바이스 트리 노드를 이름표로 쓰기 때문에, 브리지 쪽에서 msi-parent 로
 * 이 노드를 가리키면 그 도메인을 찾아 쓸 수 있다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 도메인을 못 만들면 -ENOMEM 을 돌려주고 probe 가 되감는다.
 *
 * 호출 체인:
 *   xgene_msi_probe() → [이 함수] → msi_create_parent_irq_domain()
 */
static int xgene_allocate_domains(struct device_node *node,
				  struct xgene_msi *msi)
{
	struct irq_domain_info info = {
		.fwnode		= of_fwnode_handle(node),
		/* [한국어] 이 파일의 alloc/free 표를 도메인에 건다. */
		.ops		= &xgene_msi_domain_ops,
		.size		= NR_MSI_VEC,
		.host_data	= msi,
	};

	msi->inner_domain = msi_create_parent_irq_domain(&info, &xgene_msi_parent_ops);
	/* [한국어] 도메인을 못 만들었으면 메모리 부족으로 본다. 삼항 연산으로 포인터를
	 * 0/-ENOMEM 으로 바꿔 돌려준다. */
	return msi->inner_domain ? 0 : -ENOMEM;
}

/* [한국어]
 * xgene_msi_init_allocator - hwirq 배정용 비트맵과 그 잠금을 준비한다
 *
 * @dev: 이 컨트롤러의 device. devm 관리 주체다.
 * @return: 0 = 성공, -ENOMEM = 실패.
 *
 * 비트가 NR_MSI_VEC 개인 비트맵을 0 으로 채워 할당하고, 그것을 지킬 뮤텍스를
 * 초기화한다.
 *
 * **인자로 받은 dev 만 쓰고 드라이버 상태는 전역에서 가져온다.** 이 파일이
 * 전역 포인터 하나에 의존하는 방식이 여기에도 드러난다.
 *
 * devm 판이라 해제 코드가 따로 없다.
 *
 * 크기가 실행 시점에 정해진다 — CPU 수가 8이면 256개, 그보다 적으면 더
 * 많아진다. 상류 주석이 밝힌 대로 이 설계는 CPU 8개를 전제한다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패 시 -ENOMEM. probe 가 기록을 남기고 되감는다.
 *
 * 호출 체인:
 *   xgene_msi_probe() → [이 함수] → devm_bitmap_zalloc() → mutex_init()
 */
static int xgene_msi_init_allocator(struct device *dev)
{
	xgene_msi_ctrl->bitmap = devm_bitmap_zalloc(dev, NR_MSI_VEC, GFP_KERNEL);
	if (!xgene_msi_ctrl->bitmap)
		/* [한국어] 비트맵을 못 잡으면 더 진행할 수 없다. */
		return -ENOMEM;

	mutex_init(&xgene_msi_ctrl->bitmap_lock);

	return 0;
}

/* [한국어]
 * xgene_msi_isr - 프레임 하나의 대기 MSI 를 모두 훑어 상위로 올린다
 *
 * @desc: 이 GIC 인터럽트의 서술자. 핸들러 데이터에 프레임 정보가 들어 있다.
 *
 * 이 파일의 인터럽트 처리 전부다. **연쇄(chained) 핸들러** 라서 자기 자신이
 * 정식 인터럽트로 등록되지 않고, GIC 핸들러가 직접 부른다.
 *
 * 프레임 번호를 되찾는 방식이 특이하다. 핸들러 데이터로 gic_irq 배열의
 * 원소 **주소** 를 받아 두고, 그 주소에서 배열 시작을 빼 첨자를 얻는다.
 * 번호를 따로 저장하는 대신 포인터 산술로 푸는 관용이다.
 *
 * 두 겹 루프다.
 *   바깥: MSIINTn 이 알려 준, 울린 레지스터들(최대 8개)
 *   안쪽: 각 레지스터가 알려 준, 대기 중인 비트들(최대 16개)
 * 안쪽 루프에 들어가기 위한 읽기 자체가 확인응답이므로, 읽은 뒤에는
 * 반드시 전부 처리해야 한다.
 *
 * 세 값(프레임, 레지스터 번호, 비트 번호)을 compute_hwirq() 로 접어
 * 도메인에 넘긴다.
 *
 * chained_irq_enter/exit 로 감싸는 이유는 부모 GIC 인터럽트를 처리 중에
 * 막아 두기 위해서다. 그러지 않으면 이 함수가 도는 중에 같은 선이 다시
 * 울려 중첩된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥. 잠들 수 없고, 그래서 여기서는
 * 비트맵 뮤텍스를 건드리지 않는다.
 *
 * 에러 경로: 도메인에 없는 hwirq 면 generic_handle_domain_irq() 가 오류를
 * 돌려주며, WARN_ON_ONCE 로 한 번만 알리고 계속 진행한다.
 *
 * 호출 체인:
 *   GIC 핸들러 → [이 함수]
 *     → chained_irq_enter() → xgene_msi_int_read() → xgene_msi_ir_read()
 *     → compute_hwirq() → generic_handle_domain_irq() → chained_irq_exit()
 */
static void xgene_msi_isr(struct irq_desc *desc)
{
	unsigned int *irqp = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 전역에서 드라이버 상태를 가져온다. 핸들러 데이터로도 받을 수 있었겠지만,
	 * 그 자리는 프레임 번호를 전하는 데 이미 쓰였다. */
	struct xgene_msi *xgene_msi = xgene_msi_ctrl;
	/* [한국어] 이 프레임에서 울린 레지스터들의 비트맵. */
	unsigned long grp_pending;
	/* [한국어] 바깥 루프의 첨자 — 레지스터 번호. */
	int msir_idx;
	/* [한국어] 이 인터럽트가 맡은 프레임 번호. */
	u32 msi_grp;

	/* [한국어] 부모 GIC 인터럽트를 처리 중에 막는다. 그러지 않으면 이 함수가 도는
	 * 중에 같은 선이 다시 울려 중첩된다. */
	chained_irq_enter(chip, desc);

	/* [한국어] 핸들러 데이터로 받은 **원소 주소** 에서 배열 시작을 빼 프레임 번호를
	 * 얻는다. 번호를 따로 저장하는 대신 포인터 산술로 푸는 관용이다. */
	msi_grp = irqp - xgene_msi->gic_irq;

	/* [한국어] 이 프레임의 레지스터 8개 중 어느 것이 울렸는지 얻는다. */
	grp_pending = xgene_msi_int_read(xgene_msi, msi_grp);

	for_each_set_bit(msir_idx, &grp_pending, IDX_PER_GROUP) {
		/* [한국어] 이 레지스터에 대기 중인 MSI 비트맵. */
		unsigned long msir;
		/* [한국어] 안쪽 루프의 첨자 — 레지스터 안의 비트 번호. */
		int intr_idx;

		msir = xgene_msi_ir_read(xgene_msi, msi_grp, msir_idx);

		for_each_set_bit(intr_idx, &msir, IRQS_PER_IDX) {
			/* [한국어] 이 MSI 에 대응하는 hwirq. */
			irq_hw_number_t hwirq;
			/* [한국어] 도메인 전달 결과. */
			int ret;

			hwirq = compute_hwirq(msi_grp, msir_idx, intr_idx);
			/* [한국어] hwirq 를 도메인에 넘겨 장치 드라이버의 핸들러까지 가게 한다.
			 * 이 안에서 실제 장치 핸들러가 돈다. */
			ret = generic_handle_domain_irq(xgene_msi->inner_domain,
							hwirq);
			WARN_ON_ONCE(ret);
		}
	}

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * xgene_msi_remove - 연쇄 핸들러를 모두 떼고 도메인을 없앤다
 *
 * @pdev: 플랫폼 장치. 쓰지 않는다.
 *
 * **정식 remove 이면서 동시에 probe 의 되감기 함수다.** 그래서 아직 설정되지
 * 않은 상태에서도 안전해야 하고, 실제로 두 가지 검사가 그 역할을 한다 —
 * 인터럽트 번호가 0 이면 건너뛰고, 도메인이 NULL 이면 없애지 않는다.
 *
 * 핸들러를 NULL 로 다시 걸어 떼는 것이 첫 단계다. 도메인을 먼저 없애면
 * 그 사이에 온 인터럽트가 사라진 도메인을 참조하게 된다.
 *
 * [상류 코드 관찰] 전역 포인터 xgene_msi_ctrl 을 NULL 로 되돌리지 않는다.
 * 그 메모리는 devm 관리라 이 함수가 돌아간 뒤 해제되므로, 이후 전역을 읽는
 * 코드가 있다면 이미 해제된 곳을 가리킨다. 다만 이 드라이버는
 * builtin_platform_driver 로 등록돼 모듈 제거 경로가 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 비트맵은 여기서 풀지 않는다 — devm 판이다.
 *
 * 실행 컨텍스트: remove 와 probe 실패 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 코어 / xgene_msi_probe() 의 error 라벨 → [이 함수]
 *     → irq_set_chained_handler_and_data(NULL) → irq_domain_remove()
 */
static void xgene_msi_remove(struct platform_device *pdev)
{
	for (int i = 0; i < NR_HW_IRQS; i++) {
		unsigned int irq = xgene_msi_ctrl->gic_irq[i];
		/* [한국어] 아직 설정되지 않은 프레임이다. probe 가 중간에 실패한 경우 뒤쪽
		 * 원소들이 0 으로 남아 있다. */
		if (!irq)
			/* [한국어] 건너뛴다. */
			continue;
		irq_set_chained_handler_and_data(irq, NULL, NULL);
	}

	if (xgene_msi_ctrl->inner_domain)
		/* [한국어] 도메인을 없앤다. **핸들러를 모두 뗀 뒤** 라야 안전하다 — 순서가
		 * 반대면 그 사이의 인터럽트가 사라진 도메인을 참조한다. */
		irq_domain_remove(xgene_msi_ctrl->inner_domain);
}

/* [한국어]
 * xgene_msi_handler_setup - 프레임 16개를 청소하고 CPU 에 고정한 뒤 핸들러를 건다
 *
 * @pdev: 플랫폼 장치. 인터럽트를 얻는 출처다.
 * @return: 0 = 성공, 음수 오류.
 *
 * 프레임마다 네 가지를 한다.
 *
 * 1. **잔여 인터럽트 청소.** 레지스터 8개를 전부 읽는다. 읽기가 곧 지우기라
 *    반환값은 버린다 — 이 파일에서 반환값을 버리는 유일한 자리이며,
 *    여기서는 그것이 의도다.
 * 2. **청소 확인.** MSIINTn 이 0 이 아니면 지워지지 않은 것이 남았다는
 *    뜻이므로 실패로 본다. 부트로더가 남긴 상태나 하드웨어 이상을 걸러낸다.
 * 3. **CPU 고정.** 프레임 i 를 CPU (i % CPU수) 에 묶고 IRQ_NO_BALANCING 을
 *    세워 커널이 옮기지 못하게 한다. 이 고정이 곧 "프레임의 아래 3비트가
 *    CPU 번호" 라는 이 드라이버의 전제를 실제로 성립시킨다.
 * 4. 연쇄 핸들러를 건다. 핸들러 데이터로 배열 원소의 **주소** 를 넘겨,
 *    ISR 이 그것으로 프레임 번호를 되찾게 한다.
 *
 * 중간에 실패하면 앞서 건 핸들러들이 남지만, probe 의 error 경로가
 * xgene_msi_remove() 를 불러 전부 떼어낸다.
 *
 * [상류 코드 관찰] pr_err() 하나만 dev_err() 이 아니라 접두사 없는 판이고,
 * 문자열에 줄바꿈도 없다. 원본에서 확인했으며 고치지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 청소 실패는 -EINVAL, 인터럽트를 못 얻으면 그 오류,
 * 친화도 설정 실패는 그 오류를 각각 올려보낸다.
 *
 * 호출 체인:
 *   xgene_msi_probe() → [이 함수]
 *     → xgene_msi_ir_read() → xgene_msi_int_read() → platform_get_irq()
 *     → irq_set_status_flags() → irq_set_affinity()
 *     → irq_set_chained_handler_and_data()
 */
static int xgene_msi_handler_setup(struct platform_device *pdev)
{
	struct xgene_msi *xgene_msi = xgene_msi_ctrl;
	int i;

	for (i = 0; i < NR_HW_IRQS; i++) {
		/* [한국어] 청소 확인용으로 읽을 MSIINTn 값. */
		u32 msi_val;
		/* [한국어] 이 프레임의 GIC 인터럽트 번호와 각 단계의 오류 코드. */
		int irq, err;

		/*
		 * MSInIRx registers are read-to-clear; before registering
		 * interrupt handlers, read all of them to clear spurious
		 * interrupts that may occur before the driver is probed.
		 */
		for (int msi_idx = 0; msi_idx < IDX_PER_GROUP; msi_idx++)
			/* [한국어] 반환값을 버린다 — 여기서는 읽는 행위 자체가 목적이다. */
			xgene_msi_ir_read(xgene_msi, i, msi_idx);

		/* Read MSIINTn to confirm */
		/* [한국어] 정말 다 지워졌는지 요약 레지스터로 확인한다. */
		msi_val = xgene_msi_int_read(xgene_msi, i);
		if (msi_val) {
			/* [한국어] 지워지지 않은 대기 인터럽트가 남았다. 하드웨어 상태가 예상 밖이다. */
			dev_err(&pdev->dev, "Failed to clear spurious IRQ\n");
			/* [한국어] 설정을 중단한다. probe 의 error 경로가 앞서 건 핸들러들을 떼어낸다. */
			return -EINVAL;
		}

		irq = platform_get_irq(pdev, i);
		/* [한국어] 이 프레임에 해당하는 GIC 인터럽트를 못 얻었다. */
		if (irq < 0)
			/* [한국어] 그 오류를 올려보낸다. */
			return irq;

		xgene_msi->gic_irq[i] = irq;

		/*
		 * Statically allocate MSI GIC IRQs to each CPU core.
		 * With 8-core X-Gene v1, 2 MSI GIC IRQs are allocated
		 * to each core.
		 */
		irq_set_status_flags(irq, IRQ_NO_BALANCING);
		err = irq_set_affinity(irq, cpumask_of(i % num_possible_cpus()));
		/* [한국어] 친화도를 고정하지 못했다. */
		if (err) {
			/* [한국어] [상류 코드 관찰] dev_err 이 아닌 pr_err 이고 문자열에 줄바꿈도 없다.
			 * 원본에서 확인했으며 코드는 고치지 않았다. */
			pr_err("failed to set affinity for GIC IRQ");
			/* [한국어] 그 오류를 올려보낸다. */
			return err;
		}

		irq_set_chained_handler_and_data(irq, xgene_msi_isr,
						 /* [한국어] 핸들러 데이터로 **배열 원소의 주소** 를 넘긴다. ISR 이 이 주소에서
						  * 배열 시작을 빼 프레임 번호를 되찾는다 — 번호를 따로 저장하지 않는
						  * 대신 위치를 정보로 쓰는 관용이다. */
						 &xgene_msi_ctrl->gic_irq[i]);
	}

	return 0;
}

static const struct of_device_id xgene_msi_match_table[] = {
	/* [한국어] 이 드라이버가 지원하는 유일한 하드웨어. */
	{.compatible = "apm,xgene1-msi"},
	/* [한국어] 표의 끝 표시. */
	{},
};

/* [한국어]
 * xgene_msi_probe - 레지스터 창을 잡고 비트맵·도메인·핸들러를 차례로 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, 네 단계가 순서대로 이어진다.
 *
 * 1. 상태 구조체를 할당해 **전역 포인터** 에 매단다. 지역 변수는 그 전역의
 *    사본일 뿐이다.
 * 2. 레지스터 창을 매핑하고, 그 **물리 시작 주소** 를 따로 보관한다. 이것이
 *    중요하다 — 장치가 MSI 를 보낼 주소는 가상 주소가 아니라 물리 주소여야
 *    하므로, 매핑 결과와 자원의 시작 주소를 둘 다 쥐고 있어야 한다.
 * 3. 비트맵과 도메인을 만든다.
 * 4. 프레임 16개를 청소하고 핸들러를 건다.
 *
 * 되감기가 한 갈래다. 모든 실패가 같은 라벨로 모여 xgene_msi_remove() 를
 * 부른다. 그 함수가 부분 초기화 상태에서도 안전하도록 쓰여 있어 가능한
 * 구조다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 단계가 실패하든 error 라벨로 모여 되감고 그 오류를
 * 올려보낸다. 첫 할당 실패만 되감을 것이 없어 곧바로 돌아간다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_kzalloc() → devm_platform_get_and_ioremap_resource()
 *     → xgene_msi_init_allocator() → xgene_allocate_domains()
 *     → xgene_msi_handler_setup()
 */
static int xgene_msi_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct xgene_msi *xgene_msi;
	/* [한국어] 각 단계의 반환값을 받을 자리. */
	int rc;

	xgene_msi_ctrl = devm_kzalloc(&pdev->dev, sizeof(*xgene_msi_ctrl),
				      GFP_KERNEL);
	if (!xgene_msi_ctrl)
		/* [한국어] 메모리 부족이다. 아직 되감을 것이 없어 곧바로 돌아간다. */
		return -ENOMEM;

	xgene_msi = xgene_msi_ctrl;

	xgene_msi->msi_regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	/* [한국어] 레지스터 창을 못 잡았다. */
	if (IS_ERR(xgene_msi->msi_regs)) {
		/* [한국어] 오류 코드를 꺼낸다. */
		rc = PTR_ERR(xgene_msi->msi_regs);
		/* [한국어] 되감기 경로로 간다. */
		goto error;
	}
	xgene_msi->msi_addr = res->start;

	rc = xgene_msi_init_allocator(&pdev->dev);
	/* [한국어] 비트맵을 못 만들었다. */
	if (rc) {
		/* [한국어] 실패를 알린다. */
		dev_err(&pdev->dev, "Error allocating MSI bitmap\n");
		/* [한국어] 되감기 경로로 간다. */
		goto error;
	}

	rc = xgene_allocate_domains(dev_of_node(&pdev->dev), xgene_msi);
	/* [한국어] 도메인을 못 만들었다. */
	if (rc) {
		/* [한국어] 실패를 알린다. */
		dev_err(&pdev->dev, "Failed to allocate MSI domain\n");
		/* [한국어] 되감기 경로로 간다. */
		goto error;
	}

	rc = xgene_msi_handler_setup(pdev);
	/* [한국어] 프레임 청소나 핸들러 연결이 실패했다. */
	if (rc)
		/* [한국어] 여기는 이미 앞 단계에서 기록을 남겼으므로 바로 되감는다. */
		goto error;

	/* [한국어] 성공을 알린다. 이 줄이 부팅 로그에 남는 유일한 흔적이다. */
	dev_info(&pdev->dev, "APM X-Gene PCIe MSI driver loaded\n");

	/* [한국어] 여기까지 오면 MSI 컨트롤러가 완전히 동작한다. */
	return 0;
error:
	/* [한국어] 모든 실패가 한 라벨로 모인다. remove 함수가 부분 초기화 상태를
	 * 견디게 쓰여 있어 가능한 구조다. */
	xgene_msi_remove(pdev);
	/* [한국어] 실패 이유를 그대로 올려보낸다. */
	return rc;
}

static struct platform_driver xgene_msi_driver = {
	/* [한국어] 드라이버 이름과 매칭 표. */
	.driver = {
		/* [한국어] sysfs 에 보일 이름. */
		.name = "xgene-msi",
		/* [한국어] 위의 디바이스 트리 매칭 표를 건다. */
		.of_match_table = xgene_msi_match_table,
	},
	.probe = xgene_msi_probe,
	.remove = xgene_msi_remove,
};
builtin_platform_driver(xgene_msi_driver);
