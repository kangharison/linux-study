/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 *
 * This header file contains stuff that is shared between different interrupt
 * remapping drivers but with no need to be visible outside of the IOMMU layer.
 */

/*
 * [한국어 설명] 인터럽트 재매핑 드라이버들이 나눠 쓰는 내부 헤더 (irq_remapping.h)
 *
 * === 파일의 역할 ===
 * (위 영어 주석 참고) Intel VT-d, AMD-Vi, Hyper-V 세 인터럽트 재매핑
 * 구현이 공통으로 쓰는 선언을 모아 둔 헤더다. IOMMU 계층 밖으로는
 * 나가지 않아야 할 것들이라 include/linux 가 아니라 여기 있다.
 *
 * 인터럽트 재매핑이란 무엇인가. 장치가 인터럽트를 낼 때 실어 보내는
 * 벡터 번호와 대상 CPU 를 하드웨어가 표를 거쳐 바꿔치기하는 기능이다.
 * 두 가지 이유로 필요하다. 하나는 안전 — 장치가 임의의 벡터를 쏘아
 * 다른 CPU 의 중요한 인터럽트를 흉내 내지 못하게 막는다. 다른 하나는
 * 확장 — 8비트 벡터 공간의 한계를 넘어 더 많은 인터럽트를 다루고,
 * x2APIC 처럼 넓은 CPU 번호 공간을 쓸 수 있게 한다.
 *
 * 이 헤더의 핵심은 struct irq_remap_ops 하나다. 세 구현이 그 표를 각각
 * 채우고, 공통 코드(irq_remapping.c)가 그 표만 보고 하드웨어를 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 흐름에서 이렇게 놓인다:
 *
 *   커널 부팅 (아키텍처 초기화)
 *     → irq_remapping_prepare()   [irq_remapping.c]
 *     → ops->prepare()            ← 구현이 하드웨어를 찾아 준비
 *     → irq_remapping_enable()
 *     → ops->enable()             ← 재매핑을 실제로 켠다
 *
 * 그 뒤 장치가 인터럽트를 요청하면 irq_domain 계층이 재매핑 도메인을
 * 거쳐 표 항목을 잡고, 장치에게는 그 항목을 가리키는 주소·데이터를 준다.
 * 실행 컨텍스트는 대부분 부팅 초기다 — 인터럽트 재매핑은 아주 이른
 * 단계에 켜져야 하므로, 일반 드라이버 모델보다 앞서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/iommu/irq_remapping.c: 이 헤더의 선언을 실제로 쓰는 공통 코드.
 *   커널 명령줄 인자를 해석하고, 어느 구현을 쓸지 고르고, 그 표를 부른다.
 * - drivers/iommu/intel/irq_remapping.c: VT-d 구현.
 * - drivers/iommu/amd/iommu.c 계열: AMD-Vi 구현.
 * - drivers/iommu/hyperv-iommu.c: Hyper-V 게스트용 구현.
 * - arch/x86 의 APIC 코드: 재매핑이 켜졌는지에 따라 x2APIC 를 쓸지 정한다 —
 *   그래서 irq_remapping_enabled 가 여기서 밖으로 보인다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct irq_remap_ops: 세 구현이 채우는 갈고리표. 준비·켜기·끄기·
 *   다시 켜기·폴트 처리 켜기 다섯 갈고리와 능력 비트 하나로 되어 있다.
 * - irq_remapping_enabled: 재매핑이 실제로 켜졌는가. 아키텍처 코드가 이
 *   값을 보고 x2APIC 사용 여부를 정한다.
 * - irq_remap_broken: 펌웨어가 재매핑이 망가졌다고 알려 온 경우 —
 *   그때는 켜지 않는다.
 * - CONFIG_IRQ_REMAP 를 끄고 빌드하면 아래 #else 의 상수들이 대신 들어가,
 *   호출부가 조건문 없이 그대로 컴파일된다.
 */

#ifndef __IRQ_REMAPPING_H	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define __IRQ_REMAPPING_H	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#ifdef CONFIG_IRQ_REMAP	/* [한국어] 인터럽트 재매핑을 켜고 빌드했을 때만 아래 선언들이 살아난다. */

struct irq_data;	/* [한국어] 아래 갈고리 원형에 쓰이는 형들 — 전체 정의가 필요 없어 전방 선언만 둔다. */
struct msi_msg;	/* [한국어] MSI 의 주소와 데이터. */
struct irq_domain;	/* [한국어] 인터럽트 번호를 나눠 주는 계층. */
struct irq_alloc_info;	/* [한국어] 인터럽트를 요청할 때의 부가 정보. */

extern int irq_remap_broken;	/* [한국어] 펌웨어가 "이 시스템의 재매핑은 망가졌다"고 알려 온 경우 참 — 그때는 켜지 않는다. */
extern int disable_sourceid_checking;	/* [한국어] 인터럽트를 낸 장치가 그 항목의 주인이 맞는지 검사할지. 끄면 안전이 약해지지만, 그 검사를 못 견디는 하드웨어가 있다. */
extern int no_x2apic_optout;	/* [한국어] 펌웨어가 x2APIC 를 쓰지 말라고 해도 무시할지 — 그 선언이 틀린 플랫폼이 있다. */
extern int irq_remapping_enabled;	/* [한국어] 재매핑이 실제로 켜졌는가. 아키텍처 코드가 이 값을 보고 x2APIC 사용 여부를 정한다. */

extern int disable_irq_post;	/* [한국어] 인터럽트 게시(posted interrupt)를 끌지 — 가상 머신에게 인터럽트를 직접 넣어 주는 최적화다. */

/* [한국어] 인터럽트 재매핑 구현이 채우는 갈고리표.
 *
 * 세 구현(VT-d, AMD-Vi, Hyper-V)이 이 표를 각각 채우고, 공통 코드는
 * 표만 보고 하드웨어를 다룬다. 부팅 단계마다 하나씩 불리는 구조라
 * 갈고리 이름이 곧 부팅 순서를 보여 준다. */
struct irq_remap_ops {
	/* The supported capabilities */
	/* [한국어] (위 영어 주석 참고) 이 구현이 지원하는 능력 비트.
	 * 설정자: 각 구현이 자기 표에 상수로 적어 둔다.
	 * 읽는 자: 공통 코드가 x2APIC 같은 기능을 쓸 수 있는지 판단할 때.
	 * 값 범위: IRQ_REMAP_* 비트 조합.
	 * 동기화: 읽기 전용이다. */
	int capability;

	/* Initializes hardware and makes it ready for remapping interrupts */
	/* [한국어] (위 영어 주석 참고) 하드웨어를 찾아 표를 잡고 준비시킨다.
	 * 설정자: 각 구현.
	 * 읽는 자: irq_remapping_prepare() 가 부팅 초기에 부른다.
	 * 값 범위: 0 성공, 음수면 이 구현을 쓸 수 없다는 뜻이라 다음 구현을 시도한다.
	 * 동기화: 부팅 초기라 경합이 없다. */
	int  (*prepare)(void);

	/* Enables the remapping hardware */
	/* [한국어] (위 영어 주석 참고) 준비된 하드웨어에서 재매핑을 실제로 켠다.
	 * 설정자: 각 구현.
	 * 읽는 자: irq_remapping_enable().
	 * 값 범위: 0 성공, 음수 실패.
	 * 동기화: 부팅 초기. */
	int  (*enable)(void);

	/* Disables the remapping hardware */
	/* [한국어] (위 영어 주석 참고) 재매핑을 끈다 — 시스템을 재우거나 kexec 로
	 * 다음 커널을 띄우기 전에 부른다.
	 * 설정자: 각 구현.
	 * 읽는 자: 절전 진입과 종료 경로.
	 * 값 범위: 반환값이 없다 — 끄기는 실패할 수 없어야 한다.
	 * 동기화: 다른 CPU 가 멈춘 상태에서 불린다. */
	void (*disable)(void);

	/* Reenables the remapping hardware */
	/* [한국어] (위 영어 주석 참고) 절전에서 깨어난 뒤 다시 켠다. 인자는
	 * 어떤 방식으로 되살릴지 알려 준다.
	 * 설정자: 각 구현.
	 * 읽는 자: 절전 복귀 경로.
	 * 값 범위: 0 성공, 음수 실패.
	 * 동기화: 다른 CPU 가 멈춘 상태에서 불린다. */
	int  (*reenable)(int);

	/* Enable fault handling */
	/* [한국어] (위 영어 주석 참고) 재매핑 하드웨어가 낸 오류를 받아 처리할
	 * 준비를 한다 — 인터럽트를 걸어야 해서 켜기와 별도 단계로 나뉘어 있다.
	 * 설정자: 각 구현.
	 * 읽는 자: CPU 가 올라올 때마다 불린다 (인자가 그 CPU 번호다).
	 * 값 범위: 0 성공, 음수 실패.
	 * 동기화: CPU 핫플러그 경로. */
	int  (*enable_faulting)(unsigned int);
};

extern struct irq_remap_ops intel_irq_remap_ops;	/* [한국어] Intel VT-d 구현의 갈고리표. */
extern struct irq_remap_ops amd_iommu_irq_ops;	/* [한국어] AMD-Vi 구현의 갈고리표. */
extern struct irq_remap_ops hyperv_irq_remap_ops;	/* [한국어] Hyper-V 게스트용 구현 — 실제 하드웨어가 아니라 하이퍼바이저가 재매핑을 대신한다. */

#else  /* CONFIG_IRQ_REMAP */
/* [한국어] (위 영어 주석 참고) 재매핑을 끄고 빌드했을 때. 호출부가 조건문
 * 없이 그대로 컴파일되도록 상수로 정의해 둔다 — 컴파일러가 그 분기를
 * 통째로 걷어 낸다. */

#define irq_remapping_enabled 0	/* [한국어] 언제나 꺼져 있는 것으로 본다. */
#define irq_remap_broken      0	/* [한국어] 망가졌는지 따질 대상 자체가 없다. */
#define disable_irq_post      1	/* [한국어] 인터럽트 게시도 쓸 수 없다 — 재매핑 위에 얹히는 기능이기 때문이다. */

#endif /* CONFIG_IRQ_REMAP */

#endif /* __IRQ_REMAPPING_H */
