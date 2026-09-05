/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * omap iommu: pagetable definitions
 *
 * Copyright (C) 2008-2010 Nokia Corporation
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 */

/*
 * [한국어 설명] OMAP IOMMU 의 페이지 테이블 형식 정의 (omap-iopgtable.h)
 *
 * === 파일의 역할 ===
 * TI OMAP SoC 의 IOMMU 가 걷는 페이지 테이블의 형식을 매크로로 옮겨 놓은
 * 헤더다. 함수는 하나뿐이고 나머지는 모두 크기·마스크·첨자 계산 매크로다.
 *
 * 이 형식은 ARM v7 의 짧은 서술자(short descriptor)와 사실상 같다.
 * 2단계 구조이고, 각 단계에서 여러 크기를 쓸 수 있다:
 *
 *   1단계 항목(iopgd) — 4096개, 항목 하나가 1MB 를 담당
 *     · 표(table)로 쓰면 2단계 표를 가리킨다
 *     · 섹션(section)으로 쓰면 그 1MB 를 통째로 매핑한다
 *     · 슈퍼섹션(supersection)으로 쓰면 16MB 를 매핑한다 (항목 16개가
 *       같은 값을 갖는 방식이라 표를 아끼지는 못하고 TLB 만 아낀다)
 *
 *   2단계 항목(iopte) — 256개, 항목 하나가 4KB 를 담당
 *     · 작은 페이지(small)는 4KB
 *     · 큰 페이지(large)는 64KB (항목 16개가 같은 값을 갖는다)
 *
 * 큰 매핑을 쓰면 TLB 항목 하나가 넓은 범위를 덮어, 표를 걷는 횟수가
 * 줄고 성능이 좋아진다. 그래서 드라이버가 매핑을 걸 때 정렬과 크기를
 * 보고 가능한 한 큰 단위를 고른다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 낸 DMA 주소는 이렇게 걸어간다:
 *
 *   장치 주소(da)
 *     → iopgd_index(da) 로 1단계 항목을 짚는다
 *     → 섹션이면 여기서 끝, 표면 2단계로 내려간다
 *     → iopte_index(da) 로 2단계 항목을 짚는다
 *     → 물리 주소
 *
 * 그 계산을 하는 매크로들이 이 헤더의 절반을 차지한다. omap-iommu.c 가
 * 매핑을 걸고 풀 때 이 매크로들로 자리를 찾는다.
 *
 * === 타 모듈과의 연결 ===
 * - omap-iommu.c: 이 헤더의 매크로로 표를 걸어가며 매핑을 걸고 푼다.
 *   io-pgtable 을 쓰지 않고 직접 표를 다루는 옛 방식의 드라이버다.
 * - omap-iommu.h: 드라이버의 자료 구조 — 표의 뿌리(iopgd)가 거기 있다.
 * - omap-iommu-debug.c: debugfs 로 표를 덤프할 때도 이 매크로들을 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * - IOPGD_* / IOSECTION_* / IOSUPER_*: 1단계에서 쓸 수 있는 세 가지 크기.
 * - IOPTE_* / IOLARGE_*: 2단계에서 쓸 수 있는 두 가지 크기.
 * - iopgd_is_table/section/super, iopte_is_small/large: 항목의 아래 비트를
 *   보고 그것이 어떤 종류인지 판정한다.
 * - iopgd_index/offset, iopte_index/offset: 주소에서 표 안의 자리를 구한다.
 * - omap_iommu_translate(): 항목 값과 주소를 합쳐 최종 물리 주소를 만든다.
 */

#ifndef _OMAP_IOPGTABLE_H	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define _OMAP_IOPGTABLE_H	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#include <linux/bitops.h>	/* [한국어] 아래 크기 정의가 쓰는 BIT() 매크로. */

/*
 * "L2 table" address mask and size definitions.
 */
/* [한국어] (위 영어 주석 참고) 1단계 항목 하나가 담당하는 범위 — 곧 2단계 표
 * 하나가 덮는 크기다. 20비트라 1MB 이며, 그 위쪽 12비트가 1단계 첨자가 된다. */
#define IOPGD_SHIFT		20	/* [한국어] 1MB = 1 << 20. 주소에서 이만큼 오른쪽으로 밀면 1단계 첨자가 나온다. */
#define IOPGD_SIZE		BIT(IOPGD_SHIFT)	/* [한국어] 그 크기(바이트). */
#define IOPGD_MASK		(~(IOPGD_SIZE - 1))	/* [한국어] 주소를 그 경계로 내리는 마스크. */

/*
 * "section" address mask and size definitions.
 */
/* [한국어] (위 영어 주석 참고) 섹션은 1단계 항목 하나로 1MB 를 통째로 매핑하는
 * 방식이다. 2단계 표를 만들지 않아도 되어 메모리와 표 순회를 함께 아낀다. */
#define IOSECTION_SHIFT		20	/* [한국어] 1단계 항목이 담당하는 범위와 같다 — 그 범위를 통째로 쓰기 때문이다. */
#define IOSECTION_SIZE		BIT(IOSECTION_SHIFT)	/* [한국어] 1MB. */
#define IOSECTION_MASK		(~(IOSECTION_SIZE - 1))	/* [한국어] 그 경계로 내리는 마스크 — 매핑이 이 경계에 맞아야 섹션을 쓸 수 있다. */

/*
 * "supersection" address mask and size definitions.
 */
/* [한국어] (위 영어 주석 참고) 슈퍼섹션은 16MB 를 한 매핑으로 덮는다. 다만
 * 1단계 항목 16개가 모두 같은 값을 가져야 해서 표를 아끼지는 못하고,
 * TLB 항목 하나로 16MB 를 덮는다는 이득만 있다. */
#define IOSUPER_SHIFT		24	/* [한국어] 16MB = 1 << 24. */
#define IOSUPER_SIZE		BIT(IOSUPER_SHIFT)	/* [한국어] 그 크기. */
#define IOSUPER_MASK		(~(IOSUPER_SIZE - 1))	/* [한국어] 그 경계로 내리는 마스크. */

#define PTRS_PER_IOPGD		(1UL << (32 - IOPGD_SHIFT))	/* [한국어] 32비트 주소 공간을 1MB 씩 나누면 1단계 항목이 4096개다. */
#define IOPGD_TABLE_SIZE	(PTRS_PER_IOPGD * sizeof(u32))	/* [한국어] 항목 하나가 4바이트이므로 표 전체가 16KB — 이 크기의 연속 메모리를 잡아야 한다. */

/*
 * "small page" address mask and size definitions.
 */
/* [한국어] (위 영어 주석 참고) 2단계의 기본 단위. 흔한 페이지 크기와 같다. */
#define IOPTE_SHIFT		12	/* [한국어] 4KB = 1 << 12. */
#define IOPTE_SIZE		BIT(IOPTE_SHIFT)	/* [한국어] 그 크기. */
#define IOPTE_MASK		(~(IOPTE_SIZE - 1))	/* [한국어] 그 경계로 내리는 마스크. */

/*
 * "large page" address mask and size definitions.
 */
/* [한국어] (위 영어 주석 참고) 큰 페이지는 64KB 를 덮는다. 슈퍼섹션과 마찬가지로
 * 2단계 항목 16개가 같은 값을 가져야 하며, TLB 를 아끼는 것이 목적이다. */
#define IOLARGE_SHIFT		16	/* [한국어] 64KB = 1 << 16. */
#define IOLARGE_SIZE		BIT(IOLARGE_SHIFT)	/* [한국어] 그 크기. */
#define IOLARGE_MASK		(~(IOLARGE_SIZE - 1))	/* [한국어] 그 경계로 내리는 마스크. */

#define PTRS_PER_IOPTE		(1UL << (IOPGD_SHIFT - IOPTE_SHIFT))	/* [한국어] 1MB 를 4KB 로 나누면 2단계 항목이 256개다. */
#define IOPTE_TABLE_SIZE	(PTRS_PER_IOPTE * sizeof(u32))	/* [한국어] 항목 하나가 4바이트라 표 하나가 1KB — 필요할 때마다 잡는다. */

#define IOPAGE_MASK		IOPTE_MASK	/* [한국어] "페이지 경계"를 뜻하는 일반 이름 — 기본 단위인 4KB 마스크와 같다. */

/**
 * omap_iommu_translate() - va to pa translation
 * @d:		omap iommu descriptor
 * @va:		virtual address
 * @mask:	omap iommu descriptor mask
 *
 * va to pa translation
 */
/*
 * [한국어]
 * omap_iommu_translate - 표 항목과 주소를 합쳐 물리 주소를 만든다
 *
 * @d: 표에서 읽은 항목 값 (위쪽에 물리 주소가, 아래쪽에 속성 비트가 있다).
 * @va: 번역할 장치 주소.
 * @mask: 그 항목이 덮는 크기의 마스크 (섹션이면 1MB, 작은 페이지면 4KB).
 * @return: 번역된 물리 주소.
 *
 * (위 영어 kernel-doc 참고) 계산 자체는 단순하다 — 항목에서 물리 주소
 * 부분만 남기고, 주소에서 그 범위 안의 오프셋만 남겨 합친다.
 *
 * 마스크를 인자로 받는 것이 요점이다. 이 형식은 한 표에서 여러 크기를
 * 쓸 수 있어, 항목이 섹션인지 페이지인지에 따라 "주소의 어디까지가
 * 오프셋인가"가 달라진다. 호출자가 항목 종류를 판정해 알맞은 마스크를
 * 넘겨야 한다.
 *
 * 실행 컨텍스트: 주소 변환 조회. 잠들지 않는다.
 *
 * 호출 체인:
 *   omap_iommu_iova_to_phys() → [이 함수]
 */
static inline phys_addr_t omap_iommu_translate(unsigned long d, dma_addr_t va,
					       dma_addr_t mask)
{
	return (d & mask) | (va & (~mask));	/* [한국어] 항목에서 물리 주소 부분을, 장치 주소에서 그 안의 오프셋을 가져와 합친다. */
}

/*
 * some descriptor attributes.
 */
/* [한국어] (위 영어 주석 참고) 항목의 아래 비트가 그 항목의 종류를 말한다.
 * 값이 0 이면 매핑이 없다는 뜻이라, 표를 0 으로 채워 두면 모두 미매핑이 된다. */
#define IOPGD_TABLE		(1)	/* [한국어] 이 항목은 2단계 표를 가리킨다. */
#define IOPGD_SECTION		(2)	/* [한국어] 이 항목이 1MB 를 직접 매핑한다. */
#define IOPGD_SUPER		(BIT(18) | IOPGD_SECTION)	/* [한국어] 섹션이면서 18번 비트가 서면 16MB 슈퍼섹션 — 섹션의 변형이라 값이 그것을 포함한다. */

#define iopgd_is_table(x)	(((x) & 3) == IOPGD_TABLE)	/* [한국어] 아래 두 비트로 표인지 판정한다. */
#define iopgd_is_section(x)	(((x) & (1 << 18 | 3)) == IOPGD_SECTION)	/* [한국어] 섹션이려면 18번 비트가 꺼져 있어야 한다 — 켜져 있으면 슈퍼섹션이다. */
#define iopgd_is_super(x)	(((x) & (1 << 18 | 3)) == IOPGD_SUPER)	/* [한국어] 섹션 표시와 18번 비트가 함께 서야 슈퍼섹션이다. */

#define IOPTE_SMALL		(2)	/* [한국어] 2단계 항목이 4KB 를 매핑한다. */
#define IOPTE_LARGE		(1)	/* [한국어] 2단계 항목이 64KB 를 매핑한다. */

#define iopte_is_small(x)	(((x) & 2) == IOPTE_SMALL)	/* [한국어] 1번 비트만 보면 된다 — 작은 페이지는 0번 비트를 다른 뜻으로 쓴다. */
#define iopte_is_large(x)	(((x) & 3) == IOPTE_LARGE)	/* [한국어] 큰 페이지는 두 비트를 함께 봐야 구분된다. */

/* to find an entry in a page-table-directory */
/* [한국어] (위 영어 주석 참고) 장치 주소에서 1단계 표의 자리를 구하는 매크로들. */
#define iopgd_index(da)		(((da) >> IOPGD_SHIFT) & (PTRS_PER_IOPGD - 1))	/* [한국어] 위쪽 12비트가 1단계 첨자다. */
#define iopgd_offset(obj, da)	((obj)->iopgd + iopgd_index(da))	/* [한국어] 표의 뿌리에서 그 첨자만큼 옮긴 자리. */

#define iopgd_page_paddr(iopgd)	(*iopgd & ~((1 << 10) - 1))	/* [한국어] 표 항목에서 2단계 표의 물리 주소를 꺼낸다 — 2단계 표가 1KB 라 아래 10비트가 속성이다. */
#define iopgd_page_vaddr(iopgd)	((u32 *)phys_to_virt(iopgd_page_paddr(iopgd)))	/* [한국어] 그 물리 주소를 커널 주소로 바꾼다 — 표가 저지대 메모리에 있다는 전제다. */

/* to find an entry in the second-level page table. */
/* [한국어] (위 영어 주석 참고) 장치 주소에서 2단계 표의 자리를 구하는 매크로들. */
#define iopte_index(da)		(((da) >> IOPTE_SHIFT) & (PTRS_PER_IOPTE - 1))	/* [한국어] 12~19번 비트가 2단계 첨자다. */
#define iopte_offset(iopgd, da)	(iopgd_page_vaddr(iopgd) + iopte_index(da))	/* [한국어] 1단계 항목이 가리키는 표에서 그 첨자만큼 옮긴 자리. */

#endif /* _OMAP_IOPGTABLE_H */
