// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] ARM LPAE 페이지 테이블 구현 (drivers/iommu/io-pgtable-arm.c)
 *
 * === 파일의 역할 ===
 * ARM 의 긴 서술자(Long-descriptor, LPAE) 형식 페이지 테이블을 만들고 다루는
 * 코드다. IOMMU 하드웨어를 전혀 만지지 않고 메모리 상의 자료구조만 조작하며,
 * 그래서 SMMUv2·SMMUv3·Mediatek·Rockchip·Mali 등 서로 다른 하드웨어가 이 파일
 * 하나를 공유한다. 파일 첫 줄의 "CPU-agnostic" 이 그 뜻이다.
 *
 * 형식 자체는 ARM CPU 의 페이지 테이블과 같다. 64비트 서술자, 레벨당 고정 비트
 * 수, 블록/테이블/페이지 세 가지 항목 종류. 그래서 CPU 쪽 mm 코드를 아는 사람이면
 * 구조가 바로 읽힌다 — 다만 접근 권한과 캐시 속성의 해석이 stage 에 따라 다르다.
 *
 * stage-1 과 stage-2 를 함께 구현한다는 점이 중요하다. stage-1 은 OS 가 관리하는
 * 보통의 번역(IOVA → 물리)이고, stage-2 는 하이퍼바이저가 게스트의 출력을 다시
 * 번역하는 층이다. 두 stage 는 서술자 비트 배치가 달라(AP vs HAP, MAIR 인덱스 vs
 * 직접 인코딩) 권한 변환 함수가 갈린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: iommu.c iommu_map
 *         → 벤더 드라이버 map_pages
 *           → [이 파일] arm_lpae_map_pages
 *             → 레벨을 내려가며 필요하면 테이블을 만들고, 마지막에 PTE 기입
 *             → 비일관 IOMMU 면 그 캐시라인을 메모리로 밀어낸다
 *           → 드라이버가 cfg->tlb 콜백으로 무효화
 *
 * 이 파일이 직접 부르는 것은 두 가지뿐이다 — 페이지 할당(iommu-pages.c)과
 * TLB 무효화 콜백(cfg->tlb). 하드웨어 접근은 전부 후자를 통해 드라이버에 되돌린다.
 *
 * === 타 모듈과의 연결 ===
 * - io-pgtable.c: 이 구현을 포맷 표에 등록하고, alloc_io_pgtable_ops 로 진입시킨다.
 * - io_pgtable_cfg: 양방향 통신. 드라이버가 주소 폭·페이지 크기·일관성 여부를
 *   넣으면, 이 파일이 TCR/TTBR/MAIR 레지스터 값을 채워 돌려준다.
 * - iommu-pages.c: 테이블 페이지를 그쪽에서 얻는다. 비일관 하드웨어면 기입 후
 *   캐시 플러시까지 그쪽 헬퍼로 처리한다.
 * - 벤더 드라이버: 채워진 레지스터 값을 하드웨어에 쓰고, TLB 콜백을 구현한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct arm_lpae_io_pgtable : 이 테이블의 형상 (레벨 수, 레벨당 비트, PGD 크기).
 * - arm_lpae_map_pages()   : 레벨을 내려가며 매핑을 기입한다. 필요하면 테이블 생성.
 * - arm_lpae_unmap_pages() : 그 역. 블록을 쪼개야 하면 분할까지 한다.
 * - arm_lpae_iova_to_phys(): 테이블을 걸어 내려가 물리 주소를 얻는다.
 * - __arm_lpae_set_pte()   : PTE 하나를 쓰고, 비일관 하드웨어면 캐시를 민다.
 * - arm_lpae_install_table(): 블록을 테이블로 바꾸는 원자적 교체 (분할의 핵심).
 * - alloc_io_pgtable_ops 진입점들: 64/32비트 × stage-1/2, Mali 변형.
 */
#define pr_fmt(fmt)	"arm-lpae io-pgtable: " fmt	/* [한국어] 이 파일의 로그에 붙일 접두사 — 여러 드라이버가 공유하므로 어느 계층에서 난 메시지인지 구별이 필요하다 */

#include <linux/atomic.h>	/* [한국어] PTE 교체에 cmpxchg 를 쓴다 — 블록 분할이 다른 CPU 의 매핑과 경쟁할 수 있다 */
#include <linux/bitops.h>	/* [한국어] 비트 조작 */
#include <linux/io-pgtable.h>	/* [한국어] io_pgtable_cfg/ops 정의 */
#include <linux/sizes.h>	/* [한국어] SZ_4K 등 크기 상수 */
#include <linux/slab.h>	/* [한국어] 테이블 객체 할당 */
#include <linux/types.h>	/* [한국어] 기본 타입 */
#include <linux/dma-mapping.h>	/* [한국어] 비일관 하드웨어의 캐시 관리 */

#include <asm/barrier.h>	/* [한국어] PTE 기입 순서를 강제하는 배리어 */

#include "io-pgtable-arm.h"	/* [한국어] 이 파일과 SMMU 드라이버가 공유하는 서술자 정의 */
#include "iommu-pages.h"	/* [한국어] 테이블 페이지 할당자 */

#define ARM_LPAE_MAX_ADDR_BITS		52	/* [한국어] LPAE 가 표현할 수 있는 최대 주소 폭. ARMv8.2 의 52비트 확장까지 포함한다 */
#define ARM_LPAE_S2_MAX_CONCAT_PAGES	16	/* [한국어] stage-2 는 최상위 테이블을 여러 장 이어 붙여(concatenate) 레벨 하나를 줄일 수 있다. 게스트 주소 공간이 커도 워크 깊이를 얕게 유지하는 기법이며, 최대 16장까지 허용된다 */
#define ARM_LPAE_MAX_LEVELS		4	/* [한국어] 최대 4단계 워크 (레벨 0~3) */

/* Struct accessors */
#define io_pgtable_to_data(x)						\	/* [한국어] 공통 io_pgtable 객체에서 이 구현의 확장형으로 되짚는다 */
	container_of((x), struct arm_lpae_io_pgtable, iop)	/* [한국어] iop 이 확장형 안에 박혀 있으므로 성립한다 */

#define io_pgtable_ops_to_data(x)					\	/* [한국어] ops 포인터에서 곧바로 확장형으로 (두 단계 역산을 합친 것) */
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))	/* [한국어] ops → io_pgtable → arm_lpae_io_pgtable */

/*
 * Calculate the right shift amount to get to the portion describing level l
 * in a virtual address mapped by the pagetable in d.
 */
#define ARM_LPAE_LVL_SHIFT(l,d)						\	/* [한국어] 레벨 l 의 인덱스를 꺼내려면 주소를 몇 비트 오른쪽으로 밀어야 하는지 */
	(((ARM_LPAE_MAX_LEVELS - (l)) * (d)->bits_per_level) +		\	/* [한국어] 레벨이 낮을수록(상위 테이블일수록) 더 많이 민다. 레벨당 bits_per_level 씩 차이가 난다 */
	ilog2(sizeof(arm_lpae_iopte)))	/* [한국어] 서술자 하나가 8바이트이므로 최하위 3비트는 항목 내 오프셋이다 (위 영어 주석) */

#define ARM_LPAE_GRANULE(d)						\	/* [한국어] 테이블 한 장의 크기 = 항목 크기 × 항목 수. 이것이 곧 이 설정의 페이지 크기(4K/16K/64K)이기도 하다 */
	(sizeof(arm_lpae_iopte) << (d)->bits_per_level)	/* [한국어] 8 << bits_per_level */
#define ARM_LPAE_PGD_SIZE(d)						\	/* [한국어] 최상위 테이블의 크기. 보통 테이블 한 장과 같지만, stage-2 의 이어붙이기나 좁은 주소 공간에서는 다를 수 있다 */
	(sizeof(arm_lpae_iopte) << (d)->pgd_bits)	/* [한국어] 8 << pgd_bits */

#define ARM_LPAE_PTES_PER_TABLE(d)					\	/* [한국어] 테이블 한 장에 들어가는 항목 수 */
	(ARM_LPAE_GRANULE(d) >> ilog2(sizeof(arm_lpae_iopte)))	/* [한국어] 테이블 크기 / 항목 크기 */

/*
 * Calculate the index at level l used to map virtual address a using the
 * pagetable in d.
 */
#define ARM_LPAE_PGD_IDX(l,d)						\	/* [한국어] 최상위 레벨에서만 인덱스 폭이 다를 수 있다 — 이어붙이기나 축소된 주소 공간 때문 */
	((l) == (d)->start_level ? (d)->pgd_bits - (d)->bits_per_level : 0)	/* [한국어] 최상위면 그 차이를, 아니면 0 */

#define ARM_LPAE_LVL_IDX(a,l,d)						\	/* [한국어] 주소 a 에서 레벨 l 의 테이블 인덱스를 뽑는다 (위 영어 주석) */
	(((u64)(a) >> ARM_LPAE_LVL_SHIFT(l,d)) &			\	/* [한국어] 해당 비트 구간까지 밀고 */
	 ((1 << ((d)->bits_per_level + ARM_LPAE_PGD_IDX(l,d))) - 1))	/* [한국어] 그 레벨의 인덱스 폭만큼 잘라 낸다 */

/* Calculate the block/page mapping size at level l for pagetable in d. */
#define ARM_LPAE_BLOCK_SIZE(l,d)	(1ULL << ARM_LPAE_LVL_SHIFT(l,d))	/* [한국어] 레벨 l 에서 항목 하나가 덮는 크기. 4K 입도의 레벨 2 면 2MB, 레벨 1 이면 1GB 다 (위 영어 주석) */

/* Page table bits */
#define ARM_LPAE_PTE_TYPE_SHIFT		0	/* [한국어] 항목 종류는 최하위 2비트에 있다 */
#define ARM_LPAE_PTE_TYPE_MASK		0x3	/* [한국어] 그 2비트 */

#define ARM_LPAE_PTE_TYPE_BLOCK		1	/* [한국어] 블록 — 이 레벨의 큰 크기를 통째로 매핑한다 (2MB, 1GB 등) */
#define ARM_LPAE_PTE_TYPE_TABLE		3	/* [한국어] 테이블 — 다음 레벨 테이블의 주소를 담는다 */
#define ARM_LPAE_PTE_TYPE_PAGE		3	/* [한국어] 페이지 — 마지막 레벨에서는 같은 값이 '페이지'를 뜻한다. 레벨에 따라 해석이 갈리는 것이 LPAE 의 특징이다 */

#define ARM_LPAE_PTE_ADDR_MASK		GENMASK_ULL(47,12)	/* [한국어] 항목이 담는 물리 주소 비트. 하위 12비트는 페이지 내 오프셋이라 항상 0 이고, 그 자리가 종류·유효 비트로 재활용된다 */

#define ARM_LPAE_PTE_NSTABLE		(((arm_lpae_iopte)1) << 63)	/* [한국어] 이 테이블 아래는 비보안 세계 — TrustZone 구분 */
#define ARM_LPAE_PTE_XN			(((arm_lpae_iopte)3) << 53)	/* [한국어] 실행 금지. 장치가 이 매핑에서 명령어를 가져오지 못하게 한다 */
#define ARM_LPAE_PTE_DBM		(((arm_lpae_iopte)1) << 51)	/* [한국어] Dirty Bit Modifier — 하드웨어가 쓰기를 감지해 RDONLY 비트를 지우게 하는 기능. 더티 페이지 추적(마이그레이션)에 쓴다 */
#define ARM_LPAE_PTE_AF			(((arm_lpae_iopte)1) << 10)	/* [한국어] Access Flag. 0 이면 접근 시 폴트가 나는데, IOMMU 매핑은 항상 1 로 둔다 — 접근 추적을 하지 않기 때문 */
#define ARM_LPAE_PTE_SH_NS		(((arm_lpae_iopte)0) << 8)	/* [한국어] 공유 없음 — 캐시 일관성 프로토콜에 참여하지 않는다 */
#define ARM_LPAE_PTE_SH_OS		(((arm_lpae_iopte)2) << 8)	/* [한국어] 외부 공유 — 시스템 전체와 일관성 */
#define ARM_LPAE_PTE_SH_IS		(((arm_lpae_iopte)3) << 8)	/* [한국어] 내부 공유 — CPU 클러스터 안에서 일관성. 일관성 있는 IOMMU 매핑의 기본값이다 */
#define ARM_LPAE_PTE_NS			(((arm_lpae_iopte)1) << 5)	/* [한국어] 이 매핑은 비보안 물리 주소를 가리킨다 */
#define ARM_LPAE_PTE_VALID		(((arm_lpae_iopte)1) << 0)	/* [한국어] 유효 비트. 0 이면 어떤 종류든 무효이며, PTE 를 지우는 것은 이 비트를 지우는 것이다 */

/* Software bit for solving coherency races */
#define ARM_LPAE_PTE_SW_SYNC		(((arm_lpae_iopte)1) << 55)	/* [한국어] 하드웨어가 무시하는 소프트웨어 전용 비트 (위 영어 주석). 비일관 하드웨어에서 '이 항목의 캐시 플러시가 끝났다'는 표식으로 쓴다 — 두 CPU 가 같은 테이블을 만들 때의 경쟁을 푸는 장치다 */

/* Stage-1 PTE */
#define ARM_LPAE_PTE_AP_UNPRIV		(((arm_lpae_iopte)1) << 6)	/* [한국어] stage-1: 비특권 접근 허용 */
#define ARM_LPAE_PTE_AP_RDONLY_BIT	7	/* [한국어] stage-1: 읽기 전용 비트의 위치 */
#define ARM_LPAE_PTE_AP_RDONLY		(((arm_lpae_iopte)1) << \	/* [한국어] 읽기 전용. DMA_TO_DEVICE 매핑이 이 비트를 얻어, 장치가 그 버퍼를 덮어쓰면 폴트가 난다 */
					   ARM_LPAE_PTE_AP_RDONLY_BIT)	/* [한국어] 위 비트 위치 */
#define ARM_LPAE_PTE_AP_WR_CLEAN_MASK	(ARM_LPAE_PTE_AP_RDONLY | \	/* [한국어] 더티 추적에서 '쓰기 없음' 상태를 나타내는 조합 */
					 ARM_LPAE_PTE_DBM)	/* [한국어] RDONLY + DBM 이 함께 서 있으면, 하드웨어가 쓰기를 만났을 때 RDONLY 를 지워 더티를 기록한다 */
#define ARM_LPAE_PTE_ATTRINDX_SHIFT	2	/* [한국어] stage-1: 캐시 속성을 MAIR 레지스터의 인덱스로 간접 지정한다 */
#define ARM_LPAE_PTE_nG			(((arm_lpae_iopte)1) << 11)	/* [한국어] non-Global — ASID 에 묶인 매핑. IOMMU 에서는 PASID 별 주소 공간에 쓰인다 */

/* Stage-2 PTE */
#define ARM_LPAE_PTE_HAP_FAULT		(((arm_lpae_iopte)0) << 6)	/* [한국어] stage-2: 접근 금지 (권한 비트 배치가 stage-1 과 다르다) */
#define ARM_LPAE_PTE_HAP_READ		(((arm_lpae_iopte)1) << 6)	/* [한국어] stage-2: 읽기 허용 */
#define ARM_LPAE_PTE_HAP_WRITE		(((arm_lpae_iopte)2) << 6)	/* [한국어] stage-2: 쓰기 허용. 둘을 OR 하면 읽기·쓰기가 된다 */
/*
 * For !FWB these code to:
 *  1111 = Normal outer write back cachable / Inner Write Back Cachable
 *         Permit S1 to override
 *  0101 = Normal Non-cachable / Inner Non-cachable
 *  0001 = Device / Device-nGnRE
 * For S2FWB these code:
 *  0110 Force Normal Write Back
 *  0101 Normal* is forced Normal-NC, Device unchanged
 *  0001 Force Device-nGnRE
 */
#define ARM_LPAE_PTE_MEMATTR_FWB_WB	(((arm_lpae_iopte)0x6) << 2)	/* [한국어] S2FWB: 강제 Write-Back. FWB 는 stage-2 가 stage-1 의 캐시 속성을 덮어쓰게 하는 기능으로, 게스트가 무엇을 지정하든 호스트가 정한 대로 만든다 */
#define ARM_LPAE_PTE_MEMATTR_OIWB	(((arm_lpae_iopte)0xf) << 2)	/* [한국어] stage-2: 내외부 Write-Back, stage-1 의 지정을 허용 (위 영어 주석의 1111) */
#define ARM_LPAE_PTE_MEMATTR_NC		(((arm_lpae_iopte)0x5) << 2)	/* [한국어] 비캐시 */
#define ARM_LPAE_PTE_MEMATTR_DEV	(((arm_lpae_iopte)0x1) << 2)	/* [한국어] Device-nGnRE — 병합·재정렬·투기적 접근이 모두 금지된 MMIO 속성 */

/* Register bits */
#define ARM_LPAE_VTCR_SL0_MASK		0x3	/* [한국어] stage-2 시작 레벨 필드 */

#define ARM_LPAE_TCR_T0SZ_SHIFT		0	/* [한국어] 입력 주소 폭 = 64 - T0SZ */

#define ARM_LPAE_VTCR_PS_SHIFT		16	/* [한국어] stage-2 물리 주소 폭 필드 위치 */
#define ARM_LPAE_VTCR_PS_MASK		0x7	/* [한국어] 3비트로 32/36/40/42/44/48/52비트를 인코딩한다 */

#define ARM_LPAE_MAIR_ATTR_SHIFT(n)	((n) << 3)	/* [한국어] MAIR 은 8비트 속성 8개를 한 레지스터에 담는다. 인덱스 n 의 위치 */
#define ARM_LPAE_MAIR_ATTR_MASK		0xff	/* [한국어] 속성 하나는 8비트 */
#define ARM_LPAE_MAIR_ATTR_DEVICE	0x04	/* [한국어] Device-nGnRE */
#define ARM_LPAE_MAIR_ATTR_NC		0x44	/* [한국어] Normal, 내외부 비캐시 */
#define ARM_LPAE_MAIR_ATTR_INC_OWBRWA	0xf4	/* [한국어] 내부 비캐시 + 외부 Write-Back. 일부 하드웨어가 요구하는 비대칭 조합이다 */
#define ARM_LPAE_MAIR_ATTR_WBRWA	0xff	/* [한국어] 내외부 Write-Back, 읽기·쓰기 할당 */
#define ARM_LPAE_MAIR_ATTR_IDX_NC	0	/* [한국어] PTE 의 ATTRINDX 가 0 이면 비캐시 */
#define ARM_LPAE_MAIR_ATTR_IDX_CACHE	1	/* [한국어] 1 이면 캐시 가능 — IOMMU_CACHE 매핑이 이 인덱스를 쓴다 */
#define ARM_LPAE_MAIR_ATTR_IDX_DEV	2	/* [한국어] 2 면 장치 메모리 — IOMMU_MMIO 매핑 */
#define ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE	3	/* [한국어] 3 이면 비대칭 조합 */

#define ARM_MALI_LPAE_TTBR_ADRMODE_TABLE (3u << 0)	/* [한국어] Mali GPU 의 TTBR 은 ARM 표준과 형식이 다르다 — 주소 모드를 레지스터 안에서 지정한다 */
#define ARM_MALI_LPAE_TTBR_READ_INNER	BIT(2)	/* [한국어] 내부 공유 도메인에서 테이블을 읽는다 */
#define ARM_MALI_LPAE_TTBR_SHARE_OUTER	BIT(4)	/* [한국어] 외부 공유 */

#define ARM_MALI_LPAE_MEMATTR_IMP_DEF	0x88ULL	/* [한국어] Mali 의 구현 정의 메모리 속성 */
#define ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC 0x8DULL	/* [한국어] 쓰기 할당 캐시 속성 */

/* IOPTE accessors */
#define iopte_deref(pte,d) __va(iopte_to_paddr(pte, d))	/* [한국어] 테이블 항목이 담은 물리 주소를 커널 가상 주소로. 다음 레벨 테이블을 따라 내려갈 때 쓴다 */

#define iopte_type(pte)					\
	(((pte) >> ARM_LPAE_PTE_TYPE_SHIFT) & ARM_LPAE_PTE_TYPE_MASK)	/* [한국어] 항목 종류 2비트를 뽑는다 */

#define iopte_writeable_dirty(pte)				\
	(((pte) & ARM_LPAE_PTE_AP_WR_CLEAN_MASK) == ARM_LPAE_PTE_DBM)	/* [한국어] DBM 은 서 있고 RDONLY 는 지워졌다 = 하드웨어가 쓰기를 감지해 더티로 표시했다는 뜻 */

#define iopte_set_writeable_clean(ptep)				\
	set_bit(ARM_LPAE_PTE_AP_RDONLY_BIT, (unsigned long *)(ptep))	/* [한국어] RDONLY 를 다시 세워 '깨끗함'으로 되돌린다. 더티 추적의 리셋 동작이며, 원자적 비트 연산이라 하드웨어의 동시 갱신과 경쟁하지 않는다 */

/*
 * [한국어] 이 페이지 테이블의 형상.
 *
 * LPAE 는 하나의 고정된 구조가 아니라 매개변수화된 형식이다. 페이지 입도(4K/16K/64K)와
 * 주소 폭에 따라 레벨 수와 레벨당 인덱스 비트가 달라지고, 그 조합을 이 구조체가
 * 담는다. 파일 전체의 인덱싱 매크로가 여기서 값을 읽는다.
 */
struct arm_lpae_io_pgtable {
	/* [한국어] 공통 계층이 보는 부분. 이 구조체의 첫 필드라 container_of 로
	 * 양방향 변환이 성립한다.
	 * 담고 있는 것: 포맷 번호, 드라이버 쿠키, cfg(하드웨어 설정과 TLB 콜백), ops. */
	struct io_pgtable	iop;

	/* [한국어] 최상위 테이블의 인덱스 비트 수.
	 * 보통은 bits_per_level 과 같지만 두 경우에 달라진다 — 주소 공간이 좁아
	 *   최상위 테이블이 한 장을 다 쓰지 않을 때(작게), stage-2 가 테이블을 여러
	 *   장 이어 붙여 레벨을 줄일 때(크게).
	 * 읽는 자: ARM_LPAE_PGD_SIZE, ARM_LPAE_PGD_IDX. */
	int			pgd_bits;
	/* [한국어] 워크를 시작하는 레벨 (0~3).
	 * 주소 공간이 좁으면 상위 레벨이 통째로 불필요해진다 — 40비트 주소에 4K
	 *   입도면 레벨 0 은 인덱스가 늘 0 이라 건너뛴다.
	 * stage-2 의 테이블 이어붙이기도 시작 레벨을 한 단계 낮추는 효과를 낸다.
	 * 읽는 자: 모든 워크 루프의 시작점. */
	int			start_level;
	/* [한국어] 중간 레벨 하나가 소비하는 주소 비트 수.
	 * 페이지 입도가 정한다 — 4K 면 9비트(512항목), 16K 면 11비트, 64K 면 13비트.
	 * 이 값 하나가 테이블 크기, 항목 수, 레벨별 시프트를 모두 결정한다. */
	int			bits_per_level;

	/* [한국어] 최상위 테이블의 커널 가상 주소.
	 * 설정자: 이 형식의 alloc 이 iommu-pages 에서 잡는다.
	 * 읽는 자: 모든 워크의 출발점. 드라이버는 이것의 물리 주소를 TTBR 레지스터에
	 *   써서 하드웨어에 알린다.
	 * 정렬: PGD_SIZE 에 정렬되어 있어야 한다 — 하드웨어가 하위 비트를 무시한다. */
	void			*pgd;
};

typedef u64 arm_lpae_iopte;	/* [한국어] 서술자 하나는 64비트. 32비트 LPAE 도 서술자는 64비트다 — 이름의 'Large Physical Address' 가 그 뜻이다 */

static inline bool iopte_leaf(arm_lpae_iopte pte, int lvl,
			      enum io_pgtable_fmt fmt)
{
	if (lvl == (ARM_LPAE_MAX_LEVELS - 1) && fmt != ARM_MALI_LPAE)	/* [한국어] 마지막 레벨에서는 같은 값 3 이 '페이지'를 뜻한다 */
		return iopte_type(pte) == ARM_LPAE_PTE_TYPE_PAGE;	/* [한국어] 마지막 레벨의 잎 */

	return iopte_type(pte) == ARM_LPAE_PTE_TYPE_BLOCK;	/* [한국어] 중간 레벨의 잎은 블록(2MB/1GB). Mali 는 마지막 레벨에서도 블록 인코딩을 쓴다 */
}

static inline bool iopte_table(arm_lpae_iopte pte, int lvl)
{
	if (lvl == (ARM_LPAE_MAX_LEVELS - 1))	/* [한국어] 마지막 레벨에는 다음 테이블이 없다 */
		return false;	/* [한국어] 테이블일 수 없다 — 같은 비트 값이 '페이지'를 뜻하므로 이 검사가 없으면 오판한다 */
	return iopte_type(pte) == ARM_LPAE_PTE_TYPE_TABLE;	/* [한국어] 다음 레벨 테이블을 가리키는 항목 */
}

static arm_lpae_iopte paddr_to_iopte(phys_addr_t paddr,
				     struct arm_lpae_io_pgtable *data)
{
	arm_lpae_iopte pte = paddr;	/* [한국어] 물리 주소를 서술자 형으로 */

	/* Of the bits which overlap, either 51:48 or 15:12 are always RES0 */
	return (pte | (pte >> (48 - 12))) & ARM_LPAE_PTE_ADDR_MASK;	/* [한국어] 52비트 주소 지원의 핵심 트릭이다. 48비트를 넘는 상위 4비트(51:48)를 하위 자리(15:12)로 접어 넣는다 — 64K 입도에서는 페이지 내 오프셋이 16비트라 그 자리가 비어 있기 때문이다. 겹치는 두 구간 중 하나는 항상 0 이라 OR 로 합쳐도 정보가 섞이지 않는다 (위 영어 주석) */
}

static phys_addr_t iopte_to_paddr(arm_lpae_iopte pte,
				  struct arm_lpae_io_pgtable *data)
{
	u64 paddr = pte & ARM_LPAE_PTE_ADDR_MASK;	/* [한국어] 주소 비트만 꺼낸다 */

	if (ARM_LPAE_GRANULE(data) < SZ_64K)	/* [한국어] 64K 입도가 아니면 접어 넣은 비트가 없다 */
		return paddr;	/* [한국어] 그대로가 답이다 */

	/* Rotate the packed high-order bits back to the top */
	return (paddr | (paddr << (48 - 12))) & (ARM_LPAE_PTE_ADDR_MASK << 4);	/* [한국어] 접어 두었던 상위 4비트를 제자리로 되돌린다 (위 영어 주석) */
}

/*
 * Convert an index returned by ARM_LPAE_PGD_IDX(), which can point into
 * a concatenated PGD, into the maximum number of entries that can be
 * mapped in the same table page.
 */
static inline int arm_lpae_max_entries(int i, struct arm_lpae_io_pgtable *data)
{
	int ptes_per_table = ARM_LPAE_PTES_PER_TABLE(data);	/* [한국어] 테이블 한 장의 항목 수 */

	return ptes_per_table - (i & (ptes_per_table - 1));	/* [한국어] 인덱스 i 에서 이 테이블 페이지의 끝까지 몇 항목이 남았는지. 이어붙인 PGD 에서는 인덱스가 테이블 경계를 넘을 수 있어, 한 번의 연속 기입이 페이지를 넘지 않도록 잘라 주는 값이다 (위 영어 주석) */
}

/*
 * Check if concatenated PGDs are mandatory according to Arm DDI0487 (K.a)
 * 1) R_DXBSH: For 16KB, and 48-bit input size, use level 1 instead of 0.
 * 2) R_SRKBC: After de-ciphering the table for PA size and valid initial lookup
 *   a) 40 bits PA size with 4K: use level 1 instead of level 0 (2 tables for ias = oas)
 *   b) 40 bits PA size with 16K: use level 2 instead of level 1 (16 tables for ias = oas)
 *   c) 42 bits PA size with 4K: use level 1 instead of level 0 (8 tables for ias = oas)
 *   d) 48 bits PA size with 16K: use level 1 instead of level 0 (2 tables for ias = oas)
 */
static inline bool arm_lpae_concat_mandatory(struct io_pgtable_cfg *cfg,
					     struct arm_lpae_io_pgtable *data)
{
	unsigned int ias = cfg->ias;	/* [한국어] 입력 주소 폭 (IOVA 쪽) */
	unsigned int oas = cfg->oas;	/* [한국어] 출력 주소 폭 (물리 쪽) */

	/* Covers 1 and 2.d */
	if ((ARM_LPAE_GRANULE(data) == SZ_16K) && (data->start_level == 0))	/* [한국어] 16K 입도로 레벨 0 부터 시작하는 경우 */
		return (oas == 48) || (ias == 48);	/* [한국어] 48비트면 아키텍처가 테이블 이어붙이기를 요구한다 (위 영어 주석의 1, 2.d) */

	/* Covers 2.a and 2.c */
	if ((ARM_LPAE_GRANULE(data) == SZ_4K) && (data->start_level == 0))	/* [한국어] 4K 입도로 레벨 0 부터 */
		return (oas == 40) || (oas == 42);	/* [한국어] 40/42비트 물리 주소에서 요구된다 (2.a, 2.c) */

	/* Case 2.b */
	return (ARM_LPAE_GRANULE(data) == SZ_16K) &&	/* [한국어] 16K 입도이고 */
	       (data->start_level == 1) && (oas == 40);	/* [한국어] 레벨 1 시작, 40비트 물리 주소 (2.b). 이어붙이기는 최상위 테이블을 여러 장 나란히 두어 워크 레벨을 하나 줄이는 기법으로, 아키텍처가 특정 조합에서 이것을 의무로 정해 두었다 */
}

static dma_addr_t __arm_lpae_dma_addr(void *pages)
{
	return (dma_addr_t)virt_to_phys(pages);	/* [한국어] 이 파일에서 DMA 주소는 언제나 물리 주소와 같다 — 아래 alloc 이 그 조건을 검사해 보장한다 */
}

static void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
				    struct io_pgtable_cfg *cfg,
				    void *cookie)
{
	struct device *dev = cfg->iommu_dev;	/* [한국어] 캐시 관리를 대행할 IOMMU 장치 */
	size_t alloc_size;	/* [한국어] 실제 할당 크기 */
	dma_addr_t dma;	/* [한국어] DMA 매핑 결과 */
	void *pages;	/* [한국어] 확보한 테이블 페이지 */

	/*
	 * For very small starting-level translation tables the HW requires a
	 * minimum alignment of at least 64 to cover all cases.
	 */
	alloc_size = max(size, 64);	/* [한국어] 아주 작은 최상위 테이블이라도 하드웨어가 64바이트 정렬을 요구한다 (위 영어 주석). 좁은 주소 공간에서 PGD 가 몇 항목뿐일 수 있기 때문이다 */
	if (cfg->alloc)	/* [한국어] 드라이버가 자기 할당자를 제공했으면 */
		pages = cfg->alloc(cookie, alloc_size, gfp);	/* [한국어] 그것을 쓴다 — 전용 SRAM 이나 주소 제한이 있는 영역에 테이블을 두어야 하는 하드웨어용 */
	else
		pages = iommu_alloc_pages_node_sz(dev_to_node(dev), gfp,	/* [한국어] 보통은 공용 할당자. IOMMU 와 같은 NUMA 노드에서 잡아 워크 지연을 줄인다 */
						  alloc_size);	/* [한국어] 64바이트로 올림된 크기 */

	if (!pages)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 매핑을 만들 수 없다 */

	if (!cfg->coherent_walk) {	/* [한국어] IOMMU 가 테이블을 읽을 때 CPU 캐시를 보지 않는 하드웨어면 */
		dma = dma_map_single(dev, pages, size, DMA_TO_DEVICE);	/* [한국어] DMA API 를 캐시 관리 수단으로 쓴다 */
		if (dma_mapping_error(dev, dma))	/* [한국어] 매핑 실패 */
			goto out_free;	/* [한국어] 페이지 반납 */
		/*
		 * We depend on the IOMMU being able to work with any physical
		 * address directly, so if the DMA layer suggests otherwise by
		 * translating or truncating them, that bodes very badly...
		 */
		if (dma != virt_to_phys(pages))	/* [한국어] DMA 계층이 주소를 번역하거나 잘라 냈다 */
			goto out_unmap;	/* [한국어] 페이지 테이블은 물리 주소로 참조되므로 성립할 수 없다. 위 영어 주석이 '아주 나쁜 조짐'이라 표현한 상황이다 */
	}

	return pages;	/* [한국어] 0 으로 채워진 테이블 페이지 */

out_unmap:	/* [한국어] 주소 불일치 경로 */
	dev_err(dev, "Cannot accommodate DMA translation for IOMMU page tables\n");	/* [한국어] 구성 자체가 성립하지 않음을 알린다 */
	dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);	/* [한국어] 매핑 되돌리기 */

out_free:	/* [한국어] DMA 매핑 실패가 합류 */
	if (cfg->free)	/* [한국어] 드라이버 할당자를 썼으면 */
		cfg->free(cookie, pages, size);	/* [한국어] 같은 쪽으로 반납 */
	else
		iommu_free_pages(pages);	/* [한국어] 아니면 공용 할당자로 */

	return NULL;	/* [한국어] 할당 실패 */
}

static void __arm_lpae_free_pages(void *pages, size_t size,
				  struct io_pgtable_cfg *cfg,
				  void *cookie)
{
	if (!cfg->coherent_walk)	/* [한국어] 비일관 하드웨어면 */
		dma_unmap_single(cfg->iommu_dev, __arm_lpae_dma_addr(pages),	/* [한국어] DMA 매핑을 먼저 되돌린다 */
				 size, DMA_TO_DEVICE);	/* [한국어] 같은 크기·방향 */

	if (cfg->free)	/* [한국어] 드라이버 할당자를 썼으면 */
		cfg->free(cookie, pages, size);	/* [한국어] 같은 쪽으로 */
	else
		iommu_free_pages(pages);	/* [한국어] 아니면 공용 할당자로 */
}

static void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
				struct io_pgtable_cfg *cfg)
{
	dma_sync_single_for_device(cfg->iommu_dev, __arm_lpae_dma_addr(ptep),	/* [한국어] 방금 쓴 PTE 들의 캐시라인을 메모리로 밀어낸다. 이것이 없으면 비일관 IOMMU 가 옛 내용을 읽는다 */
				   sizeof(*ptep) * num_entries, DMA_TO_DEVICE);	/* [한국어] 기입한 항목들의 범위만 */
}

static void __arm_lpae_clear_pte(arm_lpae_iopte *ptep, struct io_pgtable_cfg *cfg, int num_entries)
{
	for (int i = 0; i < num_entries; i++)	/* [한국어] 연속된 항목들을 */
		ptep[i] = 0;	/* [한국어] 0 으로 지운다 — 유효 비트가 0 이 되어 무효 항목이 된다 */

	if (!cfg->coherent_walk && num_entries)	/* [한국어] 비일관 하드웨어이고 실제로 지운 것이 있으면 */
		__arm_lpae_sync_pte(ptep, num_entries, cfg);	/* [한국어] 그 변경을 메모리로 밀어낸다 */
}

static size_t __arm_lpae_unmap(struct arm_lpae_io_pgtable *data,	/* [한국어] 상호 재귀를 위한 전방 선언 — 매핑 경로가 블록을 덮어쓸 때 옛 테이블을 해제하려고 해제 경로를 부른다 */
			       struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t size, size_t pgcount,
			       int lvl, arm_lpae_iopte *ptep);

static void __arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				phys_addr_t paddr, arm_lpae_iopte prot,
				int lvl, int num_entries, arm_lpae_iopte *ptep)
{
	arm_lpae_iopte pte = prot;	/* [한국어] 권한 비트에서 시작해 종류와 주소를 얹는다 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 캐시 정책 확인용 */
	size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);	/* [한국어] 이 레벨의 항목 하나가 덮는 크기 */
	int i;	/* [한국어] 항목 순회 커서 */

	if (data->iop.fmt != ARM_MALI_LPAE && lvl == ARM_LPAE_MAX_LEVELS - 1)	/* [한국어] 마지막 레벨의 표준 LPAE */
		pte |= ARM_LPAE_PTE_TYPE_PAGE;	/* [한국어] '페이지' 인코딩 */
	else
		pte |= ARM_LPAE_PTE_TYPE_BLOCK;	/* [한국어] 중간 레벨이거나 Mali — '블록' 인코딩 */

	for (i = 0; i < num_entries; i++)	/* [한국어] 연속된 항목들을 */
		ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);	/* [한국어] 같은 권한에 주소만 한 블록씩 전진시켜 채운다. 한 번의 호출로 여러 항목을 쓰는 것이 map_pages API 의 이득이다 */

	if (!cfg->coherent_walk)	/* [한국어] 비일관 하드웨어면 */
		__arm_lpae_sync_pte(ptep, num_entries, cfg);	/* [한국어] 기입을 메모리로 밀어낸다 */
}

static int arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
			     unsigned long iova, phys_addr_t paddr,
			     arm_lpae_iopte prot, int lvl, int num_entries,
			     arm_lpae_iopte *ptep)
{
	int i;	/* [한국어] 항목 순회 커서 */

	for (i = 0; i < num_entries; i++)	/* [한국어] 덮어쓸 자리들을 먼저 확인한다 */
		if (iopte_leaf(ptep[i], lvl, data->iop.fmt)) {	/* [한국어] 이미 매핑이 있다 */
			/* We require an unmap first */
			WARN_ON(!(data->iop.cfg.quirks & IO_PGTABLE_QUIRK_NO_WARN));	/* [한국어] 덮어쓰기는 허용하지 않는다 — 해제를 먼저 해야 한다. 일부 드라이버가 이 경고를 끄는 quirk 를 쓴다 */
			return -EEXIST;	/* [한국어] 호출자가 실패로 처리한다 */
		} else if (iopte_type(ptep[i]) == ARM_LPAE_PTE_TYPE_TABLE) {	/* [한국어] 이 자리에 하위 테이블이 있는데 이제 블록으로 덮으려 한다 */
			/*
			 * We need to unmap and free the old table before
			 * overwriting it with a block entry.
			 */
			arm_lpae_iopte *tblp;	/* [한국어] 현재 테이블의 시작 */
			size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);	/* [한국어] 이 레벨의 블록 크기 */

			tblp = ptep - ARM_LPAE_LVL_IDX(iova, lvl, data);	/* [한국어] 인덱스를 빼서 테이블의 첫 항목으로 되돌린다 — 해제 경로가 테이블 시작을 받기 때문 */
			if (__arm_lpae_unmap(data, NULL, iova + i * sz, sz, 1,	/* [한국어] 하위 테이블을 통째로 해제한다. 그러지 않으면 블록으로 덮는 순간 그 테이블 페이지들이 참조를 잃는다 (위 영어 주석) */
					     lvl, tblp) != sz) {	/* [한국어] 기대한 만큼 지우지 못했다 */
				WARN_ON(1);	/* [한국어] 페이지 테이블 상태가 이미 어긋나 있다 */
				return -EINVAL;	/* [한국어] 더 진행하지 않는다 */
			}
		}

	__arm_lpae_init_pte(data, paddr, prot, lvl, num_entries, ptep);	/* [한국어] 자리가 비었으니 실제 기입 */
	return 0;	/* [한국어] 매핑 완료 */
}

static arm_lpae_iopte arm_lpae_install_table(arm_lpae_iopte *table,
					     arm_lpae_iopte *ptep,
					     arm_lpae_iopte curr,
					     struct arm_lpae_io_pgtable *data)
{
	arm_lpae_iopte old, new;	/* [한국어] cmpxchg 의 기대값과 새 값 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 캐시 정책과 quirk 확인용 */

	new = paddr_to_iopte(__pa(table), data) | ARM_LPAE_PTE_TYPE_TABLE;	/* [한국어] 새 테이블의 물리 주소에 '테이블' 종류를 얹는다 */
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS)	/* [한국어] 비보안 세계용 테이블이면 */
		new |= ARM_LPAE_PTE_NSTABLE;	/* [한국어] 그 표시를 단다 */

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	dma_wmb();	/* [한국어] 테이블 내용이 먼저 보이고 그것을 가리키는 항목이 나중에 보이게 한다. 순서가 뒤집히면 하드웨어가 아직 채워지지 않은 테이블을 워크한다. cmpxchg64_release 로도 되지만 !CONFIG_SMP 에서 순서 의미가 사라져 명시적 배리어를 쓴다 (위 영어 주석) */

	old = cmpxchg64_relaxed(ptep, curr, new);	/* [한국어] 원자적 교체. 두 CPU 가 같은 자리에 동시에 테이블을 만들 수 있어, 진 쪽은 자기 테이블을 버리고 이긴 쪽의 것을 쓴다 */

	if (cfg->coherent_walk || (old & ARM_LPAE_PTE_SW_SYNC))	/* [한국어] 일관성 있는 하드웨어이거나, 다른 CPU 가 이미 이 항목의 캐시를 밀어냈다면 */
		return old;	/* [한국어] 추가로 할 일이 없다 */

	/* Even if it's not ours, there's no point waiting; just kick it */
	__arm_lpae_sync_pte(ptep, 1, cfg);	/* [한국어] 비일관 하드웨어 — 이 항목을 메모리로 밀어낸다. 내가 쓴 것이 아니더라도 기다릴 이유가 없으니 그냥 밀어 준다 (위 영어 주석) */
	if (old == curr)	/* [한국어] 교체에 성공한 쪽이면 */
		WRITE_ONCE(*ptep, new | ARM_LPAE_PTE_SW_SYNC);	/* [한국어] 소프트웨어 비트를 세워 '이 항목은 이미 플러시됐다'고 남긴다. 뒤따라오는 CPU 가 같은 플러시를 반복하지 않게 하는 표식이며, 하드웨어는 이 비트를 무시한다 */

	return old;	/* [한국어] 교체 전의 값. 0 이 아니면 다른 CPU 가 먼저 만든 것이다 */
}

static int __arm_lpae_map(struct arm_lpae_io_pgtable *data, unsigned long iova,
			  phys_addr_t paddr, size_t size, size_t pgcount,
			  arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep,
			  gfp_t gfp, size_t *mapped)
{
	arm_lpae_iopte *cptep, pte;	/* [한국어] 다음 레벨 테이블 포인터와 현재 항목 */
	size_t block_size = ARM_LPAE_BLOCK_SIZE(lvl, data);	/* [한국어] 이 레벨의 항목 하나가 덮는 크기 */
	size_t tblsz = ARM_LPAE_GRANULE(data);	/* [한국어] 테이블 한 장의 크기 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 설정 */
	int ret = 0, num_entries, max_entries, map_idx_start;	/* [한국어] 결과와 이번에 채울 항목 수 */

	/* Find our entry at the current level */
	map_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);	/* [한국어] 이 레벨에서 이 주소가 쓰는 인덱스 */
	ptep += map_idx_start;	/* [한국어] 해당 항목으로 이동 */

	/* If we can install a leaf entry at this level, then do so */
	if (size == block_size) {	/* [한국어] 요청 크기가 이 레벨의 블록 크기와 정확히 같다 — 여기서 잎을 만들면 된다 */
		max_entries = arm_lpae_max_entries(map_idx_start, data);	/* [한국어] 이 테이블 페이지 안에서 연속으로 쓸 수 있는 항목 수 */
		num_entries = min_t(int, pgcount, max_entries);	/* [한국어] 요청한 개수와 그 한계 중 작은 쪽 */
		ret = arm_lpae_init_pte(data, iova, paddr, prot, lvl, num_entries, ptep);	/* [한국어] 연속 항목들을 한 번에 기입 */
		if (!ret)	/* [한국어] 성공했으면 */
			*mapped += num_entries * size;	/* [한국어] 호출자에게 진행량을 알린다. 나머지는 상위 루프가 다시 부른다 */

		return ret;	/* [한국어] 이 레벨에서 끝 */
	}

	/* We can't allocate tables at the final level */
	if (WARN_ON(lvl >= ARM_LPAE_MAX_LEVELS - 1))	/* [한국어] 마지막 레벨인데 더 내려가려 한다 = 크기가 최소 페이지보다 작다 */
		return -EINVAL;	/* [한국어] 호출자의 정렬 검사가 놓친 경우 */

	/* Grab a pointer to the next level */
	pte = READ_ONCE(*ptep);	/* [한국어] 현재 항목을 한 번만 읽는다 — 다른 CPU 가 동시에 쓸 수 있다 */
	if (!pte) {	/* [한국어] 비어 있다 — 하위 테이블을 만들어야 한다 */
		cptep = __arm_lpae_alloc_pages(tblsz, gfp, cfg, data->iop.cookie);	/* [한국어] 새 테이블 페이지 */
		if (!cptep)	/* [한국어] 할당 실패 */
			return -ENOMEM;	/* [한국어] 매핑 실패 */

		pte = arm_lpae_install_table(cptep, ptep, 0, data);	/* [한국어] 0 을 기대값으로 원자적 설치 */
		if (pte)	/* [한국어] 0 이 아니면 다른 CPU 가 먼저 만들었다 */
			__arm_lpae_free_pages(cptep, tblsz, cfg, data->iop.cookie);	/* [한국어] 내 테이블은 버리고 그쪽 것을 쓴다 */
	} else if (!cfg->coherent_walk && !(pte & ARM_LPAE_PTE_SW_SYNC)) {	/* [한국어] 이미 항목이 있지만 아직 플러시되지 않았다 */
		__arm_lpae_sync_pte(ptep, 1, cfg);	/* [한국어] 여기서 밀어 준다 — 설치한 CPU 가 아직 플러시 전일 수 있다 */
	}

	if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {	/* [한국어] 항목이 있고 테이블이면 */
		cptep = iopte_deref(pte, data);	/* [한국어] 다음 레벨로 내려갈 포인터 */
	} else if (pte) {	/* [한국어] 항목이 있는데 잎이다 = 이미 매핑되어 있다 */
		/* We require an unmap first */
		WARN_ON(!(cfg->quirks & IO_PGTABLE_QUIRK_NO_WARN));	/* [한국어] 덮어쓰기는 허용하지 않는다 */
		return -EEXIST;	/* [한국어] 해제를 먼저 해야 한다 */
	}

	/* Rinse, repeat */
	return __arm_lpae_map(data, iova, paddr, size, pgcount, prot, lvl + 1,	/* [한국어] 한 레벨 내려가 같은 일을 반복한다. 꼬리 재귀라 컴파일러가 루프로 펼친다 */
			      cptep, gfp, mapped);	/* [한국어] 다음 레벨 테이블 */
}

static arm_lpae_iopte arm_lpae_prot_to_pte(struct arm_lpae_io_pgtable *data,
					   int prot)
{
	arm_lpae_iopte pte;	/* [한국어] 조립할 권한·속성 비트 */

	if (data->iop.fmt == ARM_64_LPAE_S1 ||	/* [한국어] stage-1 (OS 가 관리하는 번역) */
	    data->iop.fmt == ARM_32_LPAE_S1) {	/* [한국어] 32비트 판도 같은 인코딩 */
		pte = ARM_LPAE_PTE_nG;	/* [한국어] non-Global — 이 매핑은 특정 주소 공간에 묶인다. IOMMU 매핑은 전역일 이유가 없다 */
		if (!(prot & IOMMU_WRITE) && (prot & IOMMU_READ))	/* [한국어] 읽기만 요청했다 (DMA_TO_DEVICE) */
			pte |= ARM_LPAE_PTE_AP_RDONLY;	/* [한국어] 읽기 전용으로 못박는다. 장치가 이 버퍼를 덮어쓰려 하면 폴트가 나며, 이것이 DMA 방향 신고가 실제로 강제되는 지점이다 */
		else if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_HD)	/* [한국어] 쓰기가 허용되고 하드웨어 더티 추적이 켜져 있으면 */
			pte |= ARM_LPAE_PTE_DBM;	/* [한국어] RDONLY 없이 DBM 만 세워 둔다. 하드웨어가 첫 쓰기를 만나면 RDONLY 를 조작해 더티를 기록하고, 그 상태를 iopte_writeable_dirty 가 읽는다 */
		if (!(prot & IOMMU_PRIV))	/* [한국어] 특권 접근을 요구하지 않았으면 */
			pte |= ARM_LPAE_PTE_AP_UNPRIV;	/* [한국어] 비특권 접근을 허용한다 */
	} else {
		pte = ARM_LPAE_PTE_HAP_FAULT;	/* [한국어] stage-2 — 권한 비트 배치가 다르다. 0 에서 시작해 필요한 것만 켠다 */
		if (prot & IOMMU_READ)	/* [한국어] 읽기 요청 */
			pte |= ARM_LPAE_PTE_HAP_READ;	/* [한국어] 읽기 허용 */
		if (prot & IOMMU_WRITE)	/* [한국어] 쓰기 요청 */
			pte |= ARM_LPAE_PTE_HAP_WRITE;	/* [한국어] 쓰기 허용 */
	}

	/*
	 * Note that this logic is structured to accommodate Mali LPAE
	 * having stage-1-like attributes but stage-2-like permissions.
	 */
	if (data->iop.fmt == ARM_64_LPAE_S2 ||	/* [한국어] 캐시 속성도 stage 에 따라 인코딩이 다르다 */
	    data->iop.fmt == ARM_32_LPAE_S2) {	/* [한국어] stage-2 는 속성을 서술자에 직접 넣는다 */
		if (prot & IOMMU_MMIO) {	/* [한국어] 장치 레지스터 매핑 */
			pte |= ARM_LPAE_PTE_MEMATTR_DEV;	/* [한국어] Device — 병합·재정렬·투기 접근 금지 */
		} else if (prot & IOMMU_CACHE) {	/* [한국어] 캐시 가능 매핑 */
			if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_S2FWB)	/* [한국어] FWB 가 켜져 있으면 stage-2 가 stage-1 의 속성을 덮어쓴다 */
				pte |= ARM_LPAE_PTE_MEMATTR_FWB_WB;	/* [한국어] 게스트가 무엇을 지정하든 Write-Back 으로 강제 */
			else
				pte |= ARM_LPAE_PTE_MEMATTR_OIWB;	/* [한국어] FWB 가 없으면 stage-1 의 지정을 허용하는 조합 */
		} else {
			pte |= ARM_LPAE_PTE_MEMATTR_NC;	/* [한국어] 비캐시 */
		}
	} else {
		if (prot & IOMMU_MMIO)	/* [한국어] stage-1 은 MAIR 인덱스로 간접 지정한다 */
			pte |= (ARM_LPAE_MAIR_ATTR_IDX_DEV	/* [한국어] 장치 메모리 인덱스 */
				<< ARM_LPAE_PTE_ATTRINDX_SHIFT);	/* [한국어] ATTRINDX 필드 위치로 */
		else if (prot & IOMMU_CACHE)	/* [한국어] 캐시 가능 매핑 */
			pte |= (ARM_LPAE_MAIR_ATTR_IDX_CACHE	/* [한국어] 캐시 인덱스 */
				<< ARM_LPAE_PTE_ATTRINDX_SHIFT);	/* [한국어] 같은 필드로. 지정하지 않으면 인덱스 0(비캐시)이 된다 */
	}

	/*
	 * Also Mali has its own notions of shareability wherein its Inner
	 * domain covers the cores within the GPU, and its Outer domain is
	 * "outside the GPU" (i.e. either the Inner or System domain in CPU
	 * terms, depending on coherency).
	 */
	if (prot & IOMMU_CACHE && data->iop.fmt != ARM_MALI_LPAE)	/* [한국어] 캐시 일관성이 필요하고 Mali 가 아니면 */
		pte |= ARM_LPAE_PTE_SH_IS;	/* [한국어] 내부 공유 — CPU 클러스터와 일관성을 유지한다 */
	else
		pte |= ARM_LPAE_PTE_SH_OS;	/* [한국어] 외부 공유. Mali 는 공유 도메인의 의미가 달라(내부 = GPU 코어들, 외부 = GPU 밖) 항상 이쪽을 쓴다 (위 영어 주석) */

	if (prot & IOMMU_NOEXEC)	/* [한국어] 실행 금지 요청 */
		pte |= ARM_LPAE_PTE_XN;	/* [한국어] 장치가 이 매핑에서 명령어를 가져오지 못한다 */

	if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_NS)	/* [한국어] 비보안 세계 매핑이면 */
		pte |= ARM_LPAE_PTE_NS;	/* [한국어] 그 표시 */

	if (data->iop.fmt != ARM_MALI_LPAE)	/* [한국어] Mali 는 이 비트를 쓰지 않는다 */
		pte |= ARM_LPAE_PTE_AF;	/* [한국어] Access Flag 를 항상 세운다. IOMMU 는 접근 추적을 하지 않으므로 0 으로 두면 첫 접근마다 무의미한 폴트가 난다 */

	return pte;	/* [한국어] 이 권한 조합에 해당하는 서술자 비트 */
}

static int arm_lpae_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);	/* [한국어] ops 에서 이 구현의 자료로 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 주소 폭과 quirk 확인용 */
	arm_lpae_iopte *ptep = data->pgd;	/* [한국어] 최상위 테이블에서 시작 */
	int ret, lvl = data->start_level;	/* [한국어] 시작 레벨 */
	arm_lpae_iopte prot;	/* [한국어] 조립된 권한 비트 */
	long iaext = (s64)iova >> cfg->ias;	/* [한국어] 입력 주소가 허용 폭을 넘는지 보기 위해 부호 확장 시프트를 한다. 유효한 주소면 결과가 전부 0(또는 TTBR1 이면 전부 1)이다 */

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize))	/* [한국어] 이 테이블이 지원하지 않는 페이지 크기 */
		return -EINVAL;	/* [한국어] 호출자의 크기 선택이 잘못되었다 */

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)	/* [한국어] 상위 주소 공간(TTBR1)을 쓰는 구성이면 */
		iaext = ~iaext;	/* [한국어] 유효한 주소는 상위 비트가 전부 1 이므로 뒤집어 같은 검사를 쓴다 */
	if (WARN_ON(iaext || paddr >> cfg->oas))	/* [한국어] 입력 주소가 폭을 넘거나 물리 주소가 출력 폭을 넘는다 */
		return -ERANGE;	/* [한국어] 이 테이블로는 표현할 수 없는 매핑 */

	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))	/* [한국어] 읽기도 쓰기도 아닌 매핑 */
		return -EINVAL;	/* [한국어] 의미가 없다 */

	prot = arm_lpae_prot_to_pte(data, iommu_prot);	/* [한국어] 권한을 서술자 비트로 변환 */
	ret = __arm_lpae_map(data, iova, paddr, pgsize, pgcount, prot, lvl,	/* [한국어] 레벨을 내려가며 기입 */
			     ptep, gfp, mapped);	/* [한국어] 최상위 테이블부터 */
	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	wmb();	/* [한국어] 기입이 모두 보이게 만든 뒤에야 이 IOVA 로 워크가 시작될 수 있다. 드라이버가 이 함수에서 돌아온 직후 장치를 깨울 수 있기 때문이다 (위 영어 주석) */

	return ret;	/* [한국어] 0 이면 mapped 만큼 매핑되었다 */
}

static void __arm_lpae_free_pgtable(struct arm_lpae_io_pgtable *data, int lvl,
				    arm_lpae_iopte *ptep)
{
	arm_lpae_iopte *start, *end;	/* [한국어] 이 테이블의 순회 범위 */
	unsigned long table_size;	/* [한국어] 이 테이블의 크기 */

	if (lvl == data->start_level)	/* [한국어] 최상위 테이블이면 */
		table_size = ARM_LPAE_PGD_SIZE(data);	/* [한국어] PGD 크기 (이어붙였으면 더 크다) */
	else
		table_size = ARM_LPAE_GRANULE(data);	/* [한국어] 중간 테이블은 한 장 */

	start = ptep;	/* [한국어] 해제할 때 쓸 시작 주소 */

	/* Only leaf entries at the last level */
	if (lvl == ARM_LPAE_MAX_LEVELS - 1)	/* [한국어] 마지막 레벨에는 하위 테이블이 없다 */
		end = ptep;	/* [한국어] 순회할 것이 없다 — 잎만 있으므로 곧바로 이 테이블을 해제한다 */
	else
		end = (void *)ptep + table_size;	/* [한국어] 중간 레벨은 전체를 훑는다 */

	while (ptep != end) {	/* [한국어] 항목을 하나씩 */
		arm_lpae_iopte pte = *ptep++;	/* [한국어] 읽고 전진 */

		if (!pte || iopte_leaf(pte, lvl, data->iop.fmt))	/* [한국어] 비었거나 잎이면 하위 테이블이 없다 */
			continue;	/* [한국어] 건너뛴다 */

		__arm_lpae_free_pgtable(data, lvl + 1, iopte_deref(pte, data));	/* [한국어] 하위 테이블을 먼저 해제한다 — 후위 순회여야 부모를 지나며 접근하지 않는다 */
	}

	__arm_lpae_free_pages(start, table_size, &data->iop.cfg, data->iop.cookie);	/* [한국어] 자식을 다 정리한 뒤 이 테이블을 반납 */
}

static void arm_lpae_free_pgtable(struct io_pgtable *iop)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iop);	/* [한국어] 공통 객체에서 이 구현의 자료로 */

	__arm_lpae_free_pgtable(data, data->start_level, data->pgd);	/* [한국어] 최상위부터 후위 순회로 전체 테이블을 반납 */
	kfree(data);	/* [한국어] 형상 구조체 해제 */
}

static size_t __arm_lpae_unmap(struct arm_lpae_io_pgtable *data,
			       struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t size, size_t pgcount,
			       int lvl, arm_lpae_iopte *ptep)
{
	arm_lpae_iopte pte;	/* [한국어] 현재 항목 */
	struct io_pgtable *iop = &data->iop;	/* [한국어] TLB 콜백과 설정을 꺼내기 위해 */
	int i = 0, num_entries, max_entries, unmap_idx_start;	/* [한국어] 이번에 지울 항목 수와 시작 인덱스 */

	/* Something went horribly wrong and we ran out of page table */
	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))	/* [한국어] 마지막 레벨을 지나 더 내려가려 한다 = 워크가 끝났는데 크기가 맞지 않았다 */
		return 0;	/* [한국어] 0 바이트 해제 (위 영어 주석: 뭔가 크게 잘못된 것이다) */

	unmap_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);	/* [한국어] 이 레벨의 인덱스 */
	ptep += unmap_idx_start;	/* [한국어] 해당 항목으로 */
	pte = READ_ONCE(*ptep);	/* [한국어] 한 번만 읽는다 */
	if (!pte) {	/* [한국어] 비어 있다 — 매핑되지 않은 구간을 해제하려 한다 */
		WARN_ON(!(data->iop.cfg.quirks & IO_PGTABLE_QUIRK_NO_WARN));	/* [한국어] 대개 상위 계층의 버그이지만, 일부 드라이버는 이 경고를 끈다 */
		return 0;	/* [한국어] 지운 것이 없다 */
	}

	/* If the size matches this level, we're in the right place */
	if (size == ARM_LPAE_BLOCK_SIZE(lvl, data)) {	/* [한국어] 요청 크기가 이 레벨의 항목 크기와 같다 — 여기가 지울 자리다 */
		max_entries = arm_lpae_max_entries(unmap_idx_start, data);	/* [한국어] 이 테이블 페이지 안에서 연속으로 다룰 수 있는 항목 수 */
		num_entries = min_t(int, pgcount, max_entries);	/* [한국어] 요청 개수와 그 한계 중 작은 쪽 */

		/* Find and handle non-leaf entries */
		for (i = 0; i < num_entries; i++) {	/* [한국어] 지울 항목들을 먼저 훑는다 */
			pte = READ_ONCE(ptep[i]);	/* [한국어] 각 항목 */
			if (!pte) {	/* [한국어] 중간에 빈 항목을 만났다 */
				WARN_ON(!(data->iop.cfg.quirks & IO_PGTABLE_QUIRK_NO_WARN));	/* [한국어] 매핑과 해제의 범위가 어긋났다 */
				break;	/* [한국어] 여기까지만 처리한다 */
			}

			if (!iopte_leaf(pte, lvl, iop->fmt)) {	/* [한국어] 잎이 아니라 하위 테이블이다 — 요청 크기가 이 레벨인데 더 잘게 매핑되어 있었다는 뜻 */
				__arm_lpae_clear_pte(&ptep[i], &iop->cfg, 1);	/* [한국어] 먼저 이 항목을 지워 하위 테이블로 가는 길을 끊는다 */

				/* Also flush any partial walks */
				io_pgtable_tlb_flush_walk(iop, iova + i * size, size,	/* [한국어] 하드웨어가 캐시해 둔 '부분 워크'까지 지워야 한다. 항목만 지우면 워크 캐시에 남은 중간 결과가 이미 해제된 테이블을 가리킨다 (위 영어 주석) */
							  ARM_LPAE_GRANULE(data));	/* [한국어] 무효화 입도 */
				__arm_lpae_free_pgtable(data, lvl + 1, iopte_deref(pte, data));	/* [한국어] 그 다음에야 하위 테이블을 통째로 반납한다 */
			}
		}

		/* Clear the remaining entries */
		__arm_lpae_clear_pte(ptep, &iop->cfg, i);	/* [한국어] 실제로 처리한 i 개를 지운다. 위 루프에서 이미 지운 것도 있지만 다시 지워도 무해하고, 비일관 하드웨어의 캐시 플러시를 한 번에 하기 위해서다 */

		if (gather && !iommu_iotlb_gather_queued(gather))	/* [한국어] 지연 무효화가 아니면 (queued 면 상위가 나중에 통째로 비운다) */
			for (int j = 0; j < i; j++)	/* [한국어] 지운 각 항목에 대해 */
				io_pgtable_tlb_add_page(iop, gather, iova + j * size, size);	/* [한국어] 무효화 범위를 수집기에 쌓는다. 실제 무효화는 드라이버가 나중에 한 번에 낸다 */

		return i * size;	/* [한국어] 실제로 지운 바이트 수 */
	} else if (iopte_leaf(pte, lvl, iop->fmt)) {	/* [한국어] 요청 크기가 이 레벨보다 작은데 여기에 잎이 있다 = 큰 블록의 일부만 지우려 한다 */
		WARN_ONCE(true, "Unmap of a partial large IOPTE is not allowed");	/* [한국어] 블록 분할은 지원하지 않는다. iommu_unmap 이 '매핑을 쪼갤 수 없다'고 못박은 제약이 여기서 강제된다 */
		return 0;	/* [한국어] 지우지 않는다 — 일부만 지우면 나머지가 주인 없이 남는다 */
	}

	/* Keep on walkin' */
	ptep = iopte_deref(pte, data);	/* [한국어] 테이블 항목이다 — 한 레벨 내려간다 */
	return __arm_lpae_unmap(data, gather, iova, size, pgcount, lvl + 1, ptep);	/* [한국어] 같은 일을 반복 (위 영어 주석) */
}

static size_t arm_lpae_unmap_pages(struct io_pgtable_ops *ops, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);	/* [한국어] ops 에서 이 구현의 자료로 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 주소 폭과 지원 페이지 크기 확인용 */
	arm_lpae_iopte *ptep = data->pgd;	/* [한국어] 최상위 테이블에서 시작 */
	long iaext = (s64)iova >> cfg->ias;	/* [한국어] 입력 주소 폭 검사 (매핑 경로와 같은 관용구) */

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize || !pgcount))	/* [한국어] 지원하지 않는 크기이거나 개수가 0 */
		return 0;	/* [한국어] 0 바이트 해제 */

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)	/* [한국어] 상위 주소 공간 구성 */
		iaext = ~iaext;	/* [한국어] 유효 주소의 상위 비트가 1 이므로 뒤집는다 */
	if (WARN_ON(iaext))	/* [한국어] 주소 폭을 벗어났다 */
		return 0;	/* [한국어] 해제할 수 없다 */

	return __arm_lpae_unmap(data, gather, iova, pgsize, pgcount,	/* [한국어] 레벨을 내려가며 지운다 */
				data->start_level, ptep);	/* [한국어] 최상위부터 */
}

/*
 * [한국어] 페이지 테이블 순회의 문맥.
 *
 * 물리 주소 조회, 더티 페이지 수집, 디버그 덤프가 모두 같은 워크를 필요로 하므로,
 * 워크 자체는 한 번만 구현하고 각 레벨에서 무엇을 할지를 콜백으로 뺐다.
 * visit_iova_to_phys / visit_dirty / visit_pgtable_walk 가 그 콜백들이다.
 */
struct io_pgtable_walk_data {
	/* [한국어] 순회 중인 페이지 테이블. 콜백이 포맷(iopte_leaf 판정)과 설정을
	 * 알아야 하므로 함께 전달한다. */
	struct io_pgtable		*iop;
	/* [한국어] 콜백 전용 문맥. 무엇이 들어 있는지는 visit 함수만 안다 —
	 * 물리 주소 조회면 iova_to_phys_data, 더티 수집이면 iommu_dirty_bitmap. */
	void				*data;
	/* [한국어] 각 항목에서 불리는 콜백.
	 * 0 이 아닌 값을 돌려주면 순회가 그 자리에서 멈춘다.
	 * 잎에서 멈출지 더 내려갈지는 콜백이 아니라 워크가 판단한다. */
	int (*visit)(struct io_pgtable_walk_data *walk_data, int lvl,
		     arm_lpae_iopte *ptep, size_t size);
	/* [한국어] 순회 동작을 바꾸는 플래그. 더티 수집에서 '읽고 지우기'
	 * (IOMMU_DIRTY_NO_CLEAR 의 반대) 같은 요청이 여기 실린다. */
	unsigned long			flags;
	/* [한국어] 지금 보고 있는 IOVA. 잎을 만날 때마다 그 블록 크기만큼 전진한다.
	 * 범위 순회(더티 수집)에서는 이 값이 진행 커서 역할을 한다. */
	u64				addr;
	/* [한국어] 순회를 멈출 IOVA (배타적). const 인 것은 순회 도중 바뀌어서는
	 * 안 되기 때문이다 — 단일 주소 조회는 addr + 1 로 설정해 한 항목만 본다. */
	const u64			end;
};

static int __arm_lpae_iopte_walk(struct arm_lpae_io_pgtable *data,	/* [한국어] 전방 선언 — visit 콜백이 재귀적으로 이 함수를 부른다 */
				 struct io_pgtable_walk_data *walk_data,
				 arm_lpae_iopte *ptep,
				 int lvl);

/*
 * [한국어] iova_to_phys 순회가 결과를 담아 오는 자리.
 * 잎 항목과 그 레벨이 필요하다 — 레벨을 알아야 블록 크기를 알고, 그래야
 * 블록 안에서의 오프셋을 물리 주소에 더할 수 있다.
 */
struct iova_to_phys_data {
	arm_lpae_iopte pte;	/* [한국어] 찾은 잎 항목 */
	int lvl;	/* [한국어] 그 항목이 있던 레벨 (블록 크기를 알아야 오프셋을 더할 수 있다) */
};

static int visit_iova_to_phys(struct io_pgtable_walk_data *walk_data, int lvl,
			      arm_lpae_iopte *ptep, size_t size)
{
	struct iova_to_phys_data *data = walk_data->data;	/* [한국어] 이 순회의 결과를 담을 자리 */
	data->pte = *ptep;	/* [한국어] 도달한 항목을 기록 */
	data->lvl = lvl;	/* [한국어] 그 레벨도 */
	return 0;	/* [한국어] 순회 계속 — 잎에서 자동으로 멈춘다 */
}

static phys_addr_t arm_lpae_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);	/* [한국어] 이 구현의 자료 */
	struct iova_to_phys_data d;	/* [한국어] 순회 결과 */
	struct io_pgtable_walk_data walk_data = {	/* [한국어] 순회 설정 */
		.data = &d,	/* [한국어] 결과를 담을 자리 */
		.visit = visit_iova_to_phys,
		.addr = iova,
		.end = iova + 1,
	};
	int ret;	/* [한국어] 순회 결과 */

	ret = __arm_lpae_iopte_walk(data, &walk_data, data->pgd, data->start_level);	/* [한국어] 최상위부터 걸어 내려간다 */
	if (ret)	/* [한국어] 도중에 무효 항목을 만났다 */
		return 0;

	iova &= (ARM_LPAE_BLOCK_SIZE(d.lvl, data) - 1);	/* [한국어] 도달한 레벨의 블록 안에서의 오프셋만 남긴다. 2MB 블록에 매핑되어 있었다면 하위 21비트가 오프셋이다 */
	return iopte_to_paddr(d.pte, data) | iova;	/* [한국어] 블록의 물리 시작 주소에 그 오프셋을 더해 최종 물리 주소를 만든다 */
}

static int visit_pgtable_walk(struct io_pgtable_walk_data *walk_data, int lvl,
			      arm_lpae_iopte *ptep, size_t size)
{
	struct arm_lpae_io_pgtable_walk_data *data = walk_data->data;	/* [한국어] 디버깅용 순회 — 각 레벨의 항목을 그대로 기록한다 */
	data->ptes[lvl] = *ptep;	/* [한국어] 레벨별 서술자를 배열에 담는다. debugfs 로 페이지 테이블 상태를 들여다볼 때 쓴다 */
	return 0;	/* [한국어] 순회 계속 */
}

static int arm_lpae_pgtable_walk(struct io_pgtable_ops *ops, unsigned long iova,
				 void *wd)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);	/* [한국어] 이 구현의 자료 */
	struct io_pgtable_walk_data walk_data = {	/* [한국어] 순회 설정 */
		.data = wd,	/* [한국어] 호출자가 준 결과 자리 */
		.visit = visit_pgtable_walk,
		.addr = iova,
		.end = iova + 1,
	};

	return __arm_lpae_iopte_walk(data, &walk_data, data->pgd, data->start_level);	/* [한국어] 최상위부터 순회 */
}

static int io_pgtable_visit(struct arm_lpae_io_pgtable *data,
			    struct io_pgtable_walk_data *walk_data,
			    arm_lpae_iopte *ptep, int lvl)
{
	struct io_pgtable *iop = &data->iop;	/* [한국어] 포맷 정보 */
	arm_lpae_iopte pte = READ_ONCE(*ptep);	/* [한국어] 항목을 한 번만 읽는다 */

	size_t size = ARM_LPAE_BLOCK_SIZE(lvl, data);	/* [한국어] 이 레벨의 항목이 덮는 크기 */
	int ret = walk_data->visit(walk_data, lvl, ptep, size);	/* [한국어] 방문자 콜백 — 무엇을 할지는 호출자가 정한다 (물리 주소 조회, 더티 수집, 덤프) */
	if (ret)	/* [한국어] 콜백이 중단을 요청했다 */
		return ret;

	if (iopte_leaf(pte, lvl, iop->fmt)) {	/* [한국어] 잎에 도달했다 */
		walk_data->addr += size;	/* [한국어] 다음 주소로 전진하고 */
		return 0;	/* [한국어] 더 내려가지 않는다 */
	}

	if (!iopte_table(pte, lvl)) {	/* [한국어] 잎도 테이블도 아니다 = 무효 항목 */
		return -EINVAL;	/* [한국어] 이 주소에는 매핑이 없다 */
	}

	ptep = iopte_deref(pte, data);	/* [한국어] 테이블이면 다음 레벨로 */
	return __arm_lpae_iopte_walk(data, walk_data, ptep, lvl + 1);	/* [한국어] 한 단계 내려가 계속 */
}

static int __arm_lpae_iopte_walk(struct arm_lpae_io_pgtable *data,
				 struct io_pgtable_walk_data *walk_data,
				 arm_lpae_iopte *ptep,
				 int lvl)
{
	u32 idx;	/* [한국어] 항목 인덱스 */
	int max_entries, ret;	/* [한국어] 이 테이블의 항목 수와 결과 */

	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))	/* [한국어] 레벨을 다 썼는데도 잎을 못 만났다 */
		return -EINVAL;

	if (lvl == data->start_level)	/* [한국어] 최상위 테이블이면 */
		max_entries = ARM_LPAE_PGD_SIZE(data) / sizeof(arm_lpae_iopte);
	else
		max_entries = ARM_LPAE_PTES_PER_TABLE(data);

	for (idx = ARM_LPAE_LVL_IDX(walk_data->addr, lvl, data);	/* [한국어] 현재 주소의 인덱스부터 */
	     (idx < max_entries) && (walk_data->addr < walk_data->end); ++idx) {	/* [한국어] 테이블 끝이나 요청 범위 끝까지 */
		ret = io_pgtable_visit(data, walk_data, ptep + idx, lvl);	/* [한국어] 항목 하나를 방문한다 (잎이면 콜백만, 테이블이면 재귀) */
		if (ret)	/* [한국어] 중단 요청 또는 오류 */
			return ret;
	}

	return 0;	/* [한국어] 이 테이블의 순회 완료 */
}

static int visit_dirty(struct io_pgtable_walk_data *walk_data, int lvl,
		       arm_lpae_iopte *ptep, size_t size)
{
	struct iommu_dirty_bitmap *dirty = walk_data->data;	/* [한국어] 더티 페이지를 담을 비트맵 */

	if (!iopte_leaf(*ptep, lvl, walk_data->iop->fmt))	/* [한국어] 잎이 아니면 더티 정보가 없다 */
		return 0;

	if (iopte_writeable_dirty(*ptep)) {	/* [한국어] DBM 은 서 있고 RDONLY 는 지워졌다 = 하드웨어가 이 페이지에 쓰기를 감지했다 */
		iommu_dirty_bitmap_record(dirty, walk_data->addr, size);	/* [한국어] 사용자 공간(주로 VFIO 라이브 마이그레이션)이 읽을 비트맵에 표시한다 */
		if (!(walk_data->flags & IOMMU_DIRTY_NO_CLEAR))	/* [한국어] 호출자가 '읽기만' 하라고 하지 않았으면 */
			iopte_set_writeable_clean(ptep);	/* [한국어] RDONLY 를 다시 세워 다음 쓰기를 또 감지하게 만든다. 마이그레이션의 반복 라운드마다 새로 더러워진 페이지만 얻는 방식이다 */
	}

	return 0;	/* [한국어] 순회 계속 — 범위 전체를 훑어야 한다 */
}

static int arm_lpae_read_and_clear_dirty(struct io_pgtable_ops *ops,
					 unsigned long iova, size_t size,
					 unsigned long flags,
					 struct iommu_dirty_bitmap *dirty)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);	/* [한국어] 이 구현의 자료 */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;	/* [한국어] 주소 폭 검사용 */
	struct io_pgtable_walk_data walk_data = {	/* [한국어] 범위 순회 설정 */
		.iop = &data->iop,	/* [한국어] 포맷 판정에 필요 */
		.data = dirty,	/* [한국어] 콜백에 넘길 비트맵 */
		.visit = visit_dirty,	/* [한국어] 각 잎에서 더티를 수집한다 */
		.flags = flags,	/* [한국어] 읽고 지울지 읽기만 할지 */
		.addr = iova,	/* [한국어] 시작 주소 */
		.end = iova + size,	/* [한국어] 끝 주소 (배타적) */
	};
	arm_lpae_iopte *ptep = data->pgd;	/* [한국어] 최상위 테이블 */
	int lvl = data->start_level;	/* [한국어] 시작 레벨 */

	if (WARN_ON(!size))	/* [한국어] 범위가 0 */
		return -EINVAL;	/* [한국어] 잘못된 요청 */
	if (WARN_ON((iova + size - 1) & ~(BIT(cfg->ias) - 1)))	/* [한국어] 범위가 입력 주소 폭을 넘는다 */
		return -EINVAL;	/* [한국어] 표현할 수 없는 범위 */
	if (data->iop.fmt != ARM_64_LPAE_S1)	/* [한국어] 하드웨어 더티 추적은 64비트 stage-1 에서만 지원된다 */
		return -EINVAL;	/* [한국어] 그 외 포맷에서는 쓸 수 없다 */

	return __arm_lpae_iopte_walk(data, &walk_data, ptep, lvl);	/* [한국어] 범위를 순회하며 더티를 수집한다 */
}

static void arm_lpae_restrict_pgsizes(struct io_pgtable_cfg *cfg)
{
	unsigned long granule, page_sizes;	/* [한국어] 고를 입도와 그에 따른 페이지 크기 집합 */
	unsigned int max_addr_bits = 48;	/* [한국어] 기본 주소 폭 상한. 64K 입도만 52비트까지 간다 */

	/*
	 * We need to restrict the supported page sizes to match the
	 * translation regime for a particular granule. Aim to match
	 * the CPU page size if possible, otherwise prefer smaller sizes.
	 * While we're at it, restrict the block sizes to match the
	 * chosen granule.
	 */
	if (cfg->pgsize_bitmap & PAGE_SIZE)	/* [한국어] 드라이버가 알린 크기 중 CPU 페이지 크기가 있으면 */
		granule = PAGE_SIZE;	/* [한국어] 그것을 고른다 — CPU 와 입도가 같아야 매핑이 낭비 없이 맞아떨어진다 (위 영어 주석) */
	else if (cfg->pgsize_bitmap & ~PAGE_MASK)	/* [한국어] CPU 페이지보다 작은 크기가 있으면 */
		granule = 1UL << __fls(cfg->pgsize_bitmap & ~PAGE_MASK);	/* [한국어] 그중 가장 큰 것. 작은 쪽을 선호하는 것은 큰 입도가 CPU 페이지 하나를 매핑할 때 주변까지 열어 버리기 때문이다 */
	else if (cfg->pgsize_bitmap & PAGE_MASK)	/* [한국어] CPU 페이지보다 큰 것만 있으면 */
		granule = 1UL << __ffs(cfg->pgsize_bitmap & PAGE_MASK);	/* [한국어] 그중 가장 작은 것 */
	else
		granule = 0;	/* [한국어] 쓸 수 있는 크기가 없다 */

	switch (granule) {	/* [한국어] 입도가 정해지면 블록 크기도 따라 정해진다 */
	case SZ_4K:	/* [한국어] 4K 입도, 레벨당 9비트 */
		page_sizes = (SZ_4K | SZ_2M | SZ_1G);	/* [한국어] 페이지 / 레벨2 블록 / 레벨1 블록 */
		break;
	case SZ_16K:	/* [한국어] 16K 입도, 레벨당 11비트 */
		page_sizes = (SZ_16K | SZ_32M);	/* [한국어] 페이지 / 레벨2 블록. 레벨1 블록(64GB)은 아키텍처가 허용하지 않는다 */
		break;
	case SZ_64K:	/* [한국어] 64K 입도, 레벨당 13비트 */
		max_addr_bits = 52;	/* [한국어] 이 입도에서만 52비트 주소가 가능하다 — 서술자의 남는 하위 비트에 상위 주소를 접어 넣을 수 있기 때문 */
		page_sizes = (SZ_64K | SZ_512M);	/* [한국어] 페이지 / 레벨2 블록 */
		if (cfg->oas > 48)	/* [한국어] 52비트 물리 주소를 쓰는 구성이면 */
			page_sizes |= 1ULL << 42; /* 4TB */	/* [한국어] 레벨1 블록까지 쓸 수 있다 */
		break;
	default:
		page_sizes = 0;	/* [한국어] 알 수 없는 입도 — 아래에서 비트맵이 0 이 되어 alloc 이 실패한다 */
	}

	cfg->pgsize_bitmap &= page_sizes;	/* [한국어] 드라이버가 알린 것과 이 입도에서 실제로 가능한 것의 교집합 */
	cfg->ias = min(cfg->ias, max_addr_bits);	/* [한국어] 입력 주소 폭을 상한으로 조인다 */
	cfg->oas = min(cfg->oas, max_addr_bits);	/* [한국어] 출력 주소 폭도. 이 두 줄이 드라이버가 과하게 신고한 값을 하드웨어 현실로 되돌린다 */
}

static struct arm_lpae_io_pgtable *
arm_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
	struct arm_lpae_io_pgtable *data;	/* [한국어] 만들 테이블 객체 */
	int levels, va_bits, pg_shift;	/* [한국어] 레벨 수, 페이지 오프셋을 뺀 주소 비트, 페이지 시프트 */

	arm_lpae_restrict_pgsizes(cfg);	/* [한국어] 드라이버가 알린 크기를 실제 가능한 것으로 조인다 */

	if (!(cfg->pgsize_bitmap & (SZ_4K | SZ_16K | SZ_64K)))	/* [한국어] 쓸 수 있는 입도가 하나도 남지 않았다 */
		return NULL;

	if (cfg->ias > ARM_LPAE_MAX_ADDR_BITS)	/* [한국어] 입력 주소 폭이 LPAE 한계를 넘는다 */
		return NULL;

	if (cfg->oas > ARM_LPAE_MAX_ADDR_BITS)	/* [한국어] 출력 주소 폭도 */
		return NULL;

	data = kmalloc_obj(*data);	/* [한국어] 형상 구조체 */
	if (!data)	/* [한국어] 할당 실패 */
		return NULL;

	pg_shift = __ffs(cfg->pgsize_bitmap);	/* [한국어] 가장 작은 크기가 이 테이블의 입도다 */
	data->bits_per_level = pg_shift - ilog2(sizeof(arm_lpae_iopte));	/* [한국어] 테이블 한 장이 담는 항목 수의 로그. 4K 입도면 12 - 3 = 9비트(512항목) */

	va_bits = cfg->ias - pg_shift;	/* [한국어] 페이지 내 오프셋을 뺀, 인덱싱에 쓰이는 주소 비트 수 */
	levels = DIV_ROUND_UP(va_bits, data->bits_per_level);	/* [한국어] 레벨당 bits_per_level 씩 소비하므로 올림 나눗셈으로 레벨 수가 나온다 */
	data->start_level = ARM_LPAE_MAX_LEVELS - levels;	/* [한국어] 레벨은 아래에서부터 번호가 매겨지므로, 필요한 레벨이 적을수록 시작 레벨이 높아진다 */

	/* Calculate the actual size of our pgd (without concatenation) */
	data->pgd_bits = va_bits - (data->bits_per_level * (levels - 1));	/* [한국어] 최상위 테이블이 담당할 비트 수. 나머지 레벨이 다 쓰고 남은 만큼이라 대개 bits_per_level 보다 작다 */

	data->iop.ops = (struct io_pgtable_ops) {	/* [한국어] 공통 계층이 부를 함수표를 채운다 */
		.map_pages	= arm_lpae_map_pages,	/* [한국어] 매핑 */
		.unmap_pages	= arm_lpae_unmap_pages,
		.iova_to_phys	= arm_lpae_iova_to_phys,
		.read_and_clear_dirty = arm_lpae_read_and_clear_dirty,
		.pgtable_walk	= arm_lpae_pgtable_walk,
	};

	return data;	/* [한국어] 형상이 정해진 테이블 객체 (PGD 는 아직 없다) */
}

static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	u64 reg;	/* [한국어] MAIR 값을 조립할 임시 */
	struct arm_lpae_io_pgtable *data;	/* [한국어] 만들 테이블 */
	typeof(&cfg->arm_lpae_s1_cfg.tcr) tcr = &cfg->arm_lpae_s1_cfg.tcr;	/* [한국어] 드라이버에게 돌려줄 TCR 필드들. 이 함수의 절반이 이 레지스터를 채우는 일이다 */
	bool tg1;	/* [한국어] TTBR1(상위 주소 공간)용 인코딩을 쓸지 */

	if (cfg->quirks & ~(IO_PGTABLE_QUIRK_ARM_NS |	/* [한국어] 이 포맷이 이해하는 quirk 목록. 모르는 것이 하나라도 있으면 */
			    IO_PGTABLE_QUIRK_ARM_TTBR1 |
			    IO_PGTABLE_QUIRK_ARM_OUTER_WBWA |
			    IO_PGTABLE_QUIRK_ARM_HD |
			    IO_PGTABLE_QUIRK_NO_WARN))
		return NULL;	/* [한국어] 거절한다 — 조용히 무시하면 드라이버가 켜졌다고 착각한다 */

	data = arm_lpae_alloc_pgtable(cfg);	/* [한국어] 형상 계산과 객체 생성 */
	if (!data)	/* [한국어] 실패 */
		return NULL;	/* [한국어] 포맷을 만들 수 없다 */

	/* TCR */
	if (cfg->coherent_walk) {	/* [한국어] IOMMU 가 테이블을 읽을 때 CPU 캐시를 보는 하드웨어면 */
		tcr->sh = ARM_LPAE_TCR_SH_IS;	/* [한국어] 테이블 접근을 내부 공유로 — 캐시 일관성 프로토콜에 참여시킨다 */
		tcr->irgn = ARM_LPAE_TCR_RGN_WBWA;	/* [한국어] 내부 캐시 가능 */
		tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;	/* [한국어] 외부 캐시 가능. 하드웨어가 캐시에서 테이블을 읽으므로 소프트웨어 플러시가 필요 없다 */
		if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA)	/* [한국어] 일관성 있는 구성에서 이 quirk 는 모순이다 */
			goto out_free_data;	/* [한국어] 거절 */
	} else {
		tcr->sh = ARM_LPAE_TCR_SH_OS;	/* [한국어] 비일관 하드웨어 — 외부 공유 */
		tcr->irgn = ARM_LPAE_TCR_RGN_NC;	/* [한국어] 내부 비캐시. 하드웨어가 캐시를 보지 않으므로 캐시 가능으로 두면 옛 내용을 읽는다 */
		if (!(cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA))	/* [한국어] 외부 캐시를 쓰라는 요청이 없으면 */
			tcr->orgn = ARM_LPAE_TCR_RGN_NC;	/* [한국어] 외부도 비캐시 */
		else
			tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;	/* [한국어] 일부 SoC 는 외부 캐시만 일관성을 보장한다 — 그런 하드웨어를 위한 quirk */
	}

	tg1 = cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1;	/* [한국어] TTBR0(하위)와 TTBR1(상위)의 입도 인코딩이 다르다 */
	switch (ARM_LPAE_GRANULE(data)) {	/* [한국어] 입도를 TCR 의 TG 필드 값으로 */
	case SZ_4K:	/* [한국어] 4K 입도 */
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_4K : ARM_LPAE_TCR_TG0_4K;	/* [한국어] TTBR0/1 에 따라 다른 인코딩 — 아키텍처가 두 필드의 값 배치를 다르게 정해 두었다 */
		break;
	case SZ_16K:	/* [한국어] 16K 입도 */
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_16K : ARM_LPAE_TCR_TG0_16K;	/* [한국어] 마찬가지 */
		break;
	case SZ_64K:	/* [한국어] 64K 입도 */
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_64K : ARM_LPAE_TCR_TG0_64K;	/* [한국어] 마찬가지 */
		break;
	}

	switch (cfg->oas) {	/* [한국어] 출력 주소 폭을 IPS 필드 값으로 */
	case 32:	/* [한국어] 32비트 */
		tcr->ips = ARM_LPAE_TCR_PS_32_BIT;	/* [한국어] 인코딩 0 */
		break;
	case 36:	/* [한국어] 36비트 */
		tcr->ips = ARM_LPAE_TCR_PS_36_BIT;	/* [한국어] 인코딩 1 */
		break;
	case 40:	/* [한국어] 40비트 */
		tcr->ips = ARM_LPAE_TCR_PS_40_BIT;	/* [한국어] 인코딩 2 */
		break;
	case 42:	/* [한국어] 42비트 */
		tcr->ips = ARM_LPAE_TCR_PS_42_BIT;	/* [한국어] 인코딩 3 */
		break;
	case 44:	/* [한국어] 44비트 */
		tcr->ips = ARM_LPAE_TCR_PS_44_BIT;	/* [한국어] 인코딩 4 */
		break;
	case 48:	/* [한국어] 48비트 */
		tcr->ips = ARM_LPAE_TCR_PS_48_BIT;	/* [한국어] 인코딩 5 */
		break;
	case 52:	/* [한국어] 52비트 (64K 입도에서만) */
		tcr->ips = ARM_LPAE_TCR_PS_52_BIT;	/* [한국어] 인코딩 6 */
		break;
	default:
		goto out_free_data;	/* [한국어] 아키텍처가 정의하지 않은 폭 */
	}

	tcr->tsz = 64ULL - cfg->ias;	/* [한국어] 입력 주소 폭은 '무시할 상위 비트 수'로 인코딩된다. 48비트 주소면 TSZ = 16 */

	/* MAIRs */
	reg = (ARM_LPAE_MAIR_ATTR_NC	/* [한국어] MAIR 레지스터를 조립한다. PTE 의 ATTRINDX 가 이 레지스터의 8비트 슬롯을 가리키므로, 인덱스와 속성의 대응을 여기서 정한다 */
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |	/* [한국어] 인덱스 0 = 비캐시 */
	      (ARM_LPAE_MAIR_ATTR_WBRWA	/* [한국어] Write-Back 읽기·쓰기 할당 */
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |	/* [한국어] 인덱스 1 = 캐시 가능 (IOMMU_CACHE 매핑이 쓴다) */
	      (ARM_LPAE_MAIR_ATTR_DEVICE	/* [한국어] Device-nGnRE */
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV)) |	/* [한국어] 인덱스 2 = MMIO 매핑 */
	      (ARM_LPAE_MAIR_ATTR_INC_OWBRWA	/* [한국어] 내부 비캐시 + 외부 Write-Back */
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE));	/* [한국어] 인덱스 3 = 비대칭 캐시 구성용 */

	cfg->arm_lpae_s1_cfg.mair = reg;	/* [한국어] 드라이버가 이 값을 MAIR 레지스터에 쓴다 */

	/* Looking good; allocate a pgd */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data),	/* [한국어] 이제 형상이 확정됐으므로 최상위 테이블을 잡는다 */
					   GFP_KERNEL, cfg, cookie);	/* [한국어] 초기화 경로라 잠들 수 있다 */
	if (!data->pgd)	/* [한국어] 할당 실패 */
		goto out_free_data;	/* [한국어] 형상 구조체도 반납 */

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();	/* [한국어] 빈 PGD 가 메모리에 보인 뒤에야 드라이버가 TTBR 을 쓸 수 있다. 순서가 뒤집히면 하드웨어가 쓰레기 테이블을 워크한다 (위 영어 주석) */

	/* TTBR */
	cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);	/* [한국어] 드라이버가 이 물리 주소를 TTBR 레지스터에 쓴다 — 하드웨어가 테이블을 찾는 유일한 경로다 */
	return &data->iop;	/* [한국어] 공통 계층이 받을 객체 */

out_free_data:	/* [한국어] 실패 경로 */
	kfree(data);	/* [한국어] 형상 구조체 반납 (PGD 는 아직 없거나 이 경로로 오지 않는다) */
	return NULL;	/* [한국어] 포맷 생성 실패 */
}

static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	u64 sl;
	struct arm_lpae_io_pgtable *data;
	typeof(&cfg->arm_lpae_s2_cfg.vtcr) vtcr = &cfg->arm_lpae_s2_cfg.vtcr;

	if (cfg->quirks & ~(IO_PGTABLE_QUIRK_ARM_S2FWB |
			    IO_PGTABLE_QUIRK_NO_WARN))
		return NULL;

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	if (arm_lpae_concat_mandatory(cfg, data)) {
		if (WARN_ON((ARM_LPAE_PGD_SIZE(data) / sizeof(arm_lpae_iopte)) >
			    ARM_LPAE_S2_MAX_CONCAT_PAGES))
			return NULL;
		data->pgd_bits += data->bits_per_level;
		data->start_level++;
	}

	/* VTCR */
	if (cfg->coherent_walk) {
		vtcr->sh = ARM_LPAE_TCR_SH_IS;
		vtcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
		vtcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
	} else {
		vtcr->sh = ARM_LPAE_TCR_SH_OS;
		vtcr->irgn = ARM_LPAE_TCR_RGN_NC;
		vtcr->orgn = ARM_LPAE_TCR_RGN_NC;
	}

	sl = data->start_level;

	switch (ARM_LPAE_GRANULE(data)) {
	case SZ_4K:
		vtcr->tg = ARM_LPAE_TCR_TG0_4K;
		sl++; /* SL0 format is different for 4K granule size */
		break;
	case SZ_16K:
		vtcr->tg = ARM_LPAE_TCR_TG0_16K;
		break;
	case SZ_64K:
		vtcr->tg = ARM_LPAE_TCR_TG0_64K;
		break;
	}

	switch (cfg->oas) {
	case 32:
		vtcr->ps = ARM_LPAE_TCR_PS_32_BIT;
		break;
	case 36:
		vtcr->ps = ARM_LPAE_TCR_PS_36_BIT;
		break;
	case 40:
		vtcr->ps = ARM_LPAE_TCR_PS_40_BIT;
		break;
	case 42:
		vtcr->ps = ARM_LPAE_TCR_PS_42_BIT;
		break;
	case 44:
		vtcr->ps = ARM_LPAE_TCR_PS_44_BIT;
		break;
	case 48:
		vtcr->ps = ARM_LPAE_TCR_PS_48_BIT;
		break;
	case 52:
		vtcr->ps = ARM_LPAE_TCR_PS_52_BIT;
		break;
	default:
		goto out_free_data;
	}

	vtcr->tsz = 64ULL - cfg->ias;
	vtcr->sl = ~sl & ARM_LPAE_VTCR_SL0_MASK;

	/* Allocate pgd pages */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data),
					   GFP_KERNEL, cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* VTTBR */
	cfg->arm_lpae_s2_cfg.vttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 32 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s1(cfg, cookie);
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 40 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s2(cfg, cookie);
}

static struct io_pgtable *
arm_mali_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct arm_lpae_io_pgtable *data;

	/* No quirks for Mali (hopefully) */
	if (cfg->quirks)
		return NULL;

	if (cfg->ias > 48 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	/* Mali seems to need a full 4-level table regardless of IAS */
	if (data->start_level > 0) {
		data->start_level = 0;
		data->pgd_bits = 0;
	}
	/*
	 * MEMATTR: Mali has no actual notion of a non-cacheable type, so the
	 * best we can do is mimic the out-of-tree driver and hope that the
	 * "implementation-defined caching policy" is good enough. Similarly,
	 * we'll use it for the sake of a valid attribute for our 'device'
	 * index, although callers should never request that in practice.
	 */
	cfg->arm_mali_lpae_cfg.memattr =
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
		(ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV));

	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before TRANSTAB can be written */
	wmb();

	cfg->arm_mali_lpae_cfg.transtab = virt_to_phys(data->pgd) |
					  ARM_MALI_LPAE_TTBR_READ_INNER |
					  ARM_MALI_LPAE_TTBR_ADRMODE_TABLE;
	if (cfg->coherent_walk)
		cfg->arm_mali_lpae_cfg.transtab |= ARM_MALI_LPAE_TTBR_SHARE_OUTER;

	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s1_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_64_lpae_alloc_pgtable_s1,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s2_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_64_lpae_alloc_pgtable_s2,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s1_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_32_lpae_alloc_pgtable_s1,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s2_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_32_lpae_alloc_pgtable_s2,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_mali_lpae_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_mali_lpae_alloc_pgtable,
	.free	= arm_lpae_free_pgtable,
};
