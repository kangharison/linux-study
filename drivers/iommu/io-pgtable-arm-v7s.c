// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 *
 * ARMv7 Short-descriptor format, supporting
 * - Basic memory attributes
 * - Simplified access permissions (AP[2:1] model)
 * - Backwards-compatible TEX remap
 * - Large pages/supersections (if indicated by the caller)
 *
 * Not supporting:
 * - Legacy access permissions (AP[2:0] model)
 *
 * Almost certainly never supporting:
 * - PXN
 * - Domains
 *
 * Copyright (C) 2014-2015 ARM Limited
 * Copyright (c) 2014-2015 MediaTek Inc.
 */

/*
 * [한국어 설명] ARMv7 short-descriptor 페이지 테이블 형식 구현 (io-pgtable-arm-v7s.c)
 *
 * === 파일의 역할 ===
 * ARMv7의 "short descriptor" 페이지 테이블 형식을 io-pgtable 프레임워크에
 * 구현한 것이다. 이 형식은 원래 32비트 ARM CPU의 MMU가 쓰던 것인데,
 * 그 CPU와 같은 형식을 쓰는 IOMMU들(Qualcomm MSM IOMMU, MediaTek M4U 2세대,
 * ARM SMMU v1/v2의 32비트 모드)이 이 파일을 백엔드로 빌려 쓴다.
 * "CPU-agnostic"이라는 파일 머리말이 그 뜻이다 — CPU의 MMU 코드가 아니라
 * 어떤 IOMMU든 쓸 수 있는 독립 구현이다.
 *
 * LPAE(long descriptor)와 비교하면 구조가 훨씬 단순하고 그만큼 제약이 많다.
 *  - 레벨이 딱 2단계다. 레벨 1은 1MB "섹션", 레벨 2는 4KB "페이지"를 다룬다.
 *  - PTE가 32비트 하나다(LPAE는 64비트).
 *  - 큰 매핑은 "연속(contiguous)" 엔트리로 표현한다. 64KB 큰 페이지와
 *    16MB 슈퍼섹션이 그것인데, **같은 내용의 엔트리 16개를 반복해서 쓰는**
 *    방식이다. LPAE의 contiguous 힌트와 달리 하드웨어가 이것을 하나의
 *    엔트리로 취급하므로, 부분 해제가 불가능하다(코드가 그것을 거부한다).
 *  - 속성 비트의 위치가 레벨 1과 레벨 2에서 다르다. 그래서 이 파일 곳곳에
 *    ARM_V7S_ATTR_SHIFT(lvl) 같은 레벨 의존 시프트가 등장한다.
 *
 * MediaTek 확장이 이 파일의 또 다른 축이다. 원래 형식은 32비트 물리 주소만
 * 다루는데, MediaTek이 PTE의 예약 비트 세 개(9, 4, 5번)를 물리 주소의
 * 32/33/34번째 비트로 재활용해 최대 35비트 물리 주소를 표현하게 했다.
 * IO_PGTABLE_QUIRK_ARM_MTK_EXT / _TTBR_EXT 두 quirk가 그 동작을 켠다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [IOMMU 드라이버] msm_iommu.c, mtk_iommu.c, arm-smmu.c 등
 *        ↓ alloc_io_pgtable_ops(ARM_V7S, &cfg, cookie)
 *   [io-pgtable.c] 포맷 번호로 이 파일의 init_fns를 찾는다
 *        ↓
 *   [이 파일] arm_v7s_alloc_pgtable()이 pgd를 만들고 TTBR/PRRR/NMRR을 계산해
 *             cfg에 채워 돌려준다
 *        ↓ 드라이버가 그 값을 하드웨어 레지스터에 기록
 *   [IOMMU 하드웨어] 그 테이블을 걸어 IOVA를 변환
 *
 *   [드라이버] iommu_map() → ops->map_pages → arm_v7s_map_pages()
 *        ↓ 테이블 갱신 후
 *   [이 파일] io_pgtable_tlb_* 로 드라이버의 무효화 콜백을 되부른다
 *
 * 실행 컨텍스트: map/unmap은 드라이버가 락을 잡은 상태에서 호출하므로
 * 이 파일에는 락이 거의 없다(예외는 테이블 설치의 cmpxchg뿐). gfp 인자로
 * atomic 컨텍스트 여부가 전달된다.
 *
 * === 타 모듈과의 연결 ===
 * - linux/io-pgtable.h: struct io_pgtable_cfg/ops/init_fns 계약과
 *   IO_PGTABLE_QUIRK_* 플래그들.
 * - linux/dma-mapping.h: 테이블 워크가 비코히런트인 플랫폼에서
 *   dma_map_single/dma_sync_single_for_device로 캐시를 유지한다.
 * - drivers/iommu/msm_iommu.c, mtk_iommu.c: 주 소비자들. cfg.arm_v7s_cfg의
 *   ttbr/tcr/prrr/nmrr을 받아 하드웨어에 기록한다.
 * - io-pgtable.c: io_pgtable_arm_v7s_init_fns를 포맷 테이블에 담는다.
 * 데이터 흐름: 드라이버의 cfg(ias/oas/quirks/pgsize_bitmap) → 이 파일이
 * 검증하고 테이블을 만듦 → cfg.arm_v7s_cfg에 레지스터 값들을 채워 반환 →
 * 드라이버가 하드웨어에 기록 → 이후 map/unmap이 테이블을 갱신.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct arm_v7s_io_pgtable: 인스턴스 하나. 최상위 테이블(pgd)과
 *   레벨 2 테이블용 슬랩 캐시.
 * - __arm_v7s_map(): 재귀적으로 레벨을 내려가며 매핑한다. 크기가 현재
 *   레벨의 블록에 맞으면 리프를 설치하고, 아니면 테이블을 만들어 재귀한다.
 * - arm_v7s_init_pte(): 리프 엔트리들을 실제로 쓴다. 기존 테이블 엔트리를
 *   블록으로 덮어쓰는 경우 먼저 그 테이블을 해제한다.
 * - __arm_v7s_unmap(): 재귀적으로 해제한다. 연속 엔트리의 부분 해제는
 *   거부하며, 테이블이 통째로 지워지면 그 테이블 메모리도 반납한다.
 * - arm_v7s_prot_to_pte() / arm_v7s_pte_to_cont(): 보호 플래그를 PTE 비트로,
 *   일반 엔트리를 연속 엔트리로 변환한다.
 * - paddr_to_iopte() / iopte_to_paddr(): MediaTek 확장 비트를 포함한
 *   물리 주소 인코딩/디코딩.
 * - arm_v7s_alloc_pgtable(): 설정을 검증하고 pgd와 슬랩 캐시를 만들고
 *   TTBR/PRRR/NMRR을 계산한다.
 * - arm_v7s_do_selftests(): 부팅 시 형식 구현이 올바른지 자체 검증한다.
 */

/* [한국어] 이 파일의 pr_* 출력 앞에 붙일 접두사. 셀프테스트 결과와
 * 오류 메시지가 어느 포맷 구현에서 나왔는지 구분해 준다. */
#define pr_fmt(fmt)	"arm-v7s io-pgtable: " fmt

/* [한국어] cmpxchg_relaxed() — 테이블 설치 경쟁을 해결하는 원자적 연산. */
#include <linux/atomic.h>
/* [한국어] dma_map_single()/dma_sync_single_for_device() — 테이블 워크가
 * 캐시 코히런트하지 않은 플랫폼에서 테이블 메모리를 하드웨어에 보이게 한다. */
#include <linux/dma-mapping.h>
/* [한국어] GFP_DMA/GFP_DMA32 등 할당 존 플래그. 32비트 PTE에 담기려면
 * 테이블이 낮은 물리 주소에 있어야 해서 필요하다. */
#include <linux/gfp.h>
/* [한국어] io-pgtable 프레임워크 계약 — 이 파일의 존재 이유다. */
#include <linux/io-pgtable.h>
/* [한국어] IOMMU_READ/WRITE/CACHE/NOEXEC/MMIO/PRIV 보호 플래그. */
#include <linux/iommu.h>
/* [한국어] WARN_ON()/WARN_ONCE() 등 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] kmemleak_ignore() — 슬랩에서 받은 레벨 2 테이블을 누수 검사에서
 * 제외한다(PTE 안에만 포인터가 있어 kmemleak이 참조를 찾지 못하기 때문). */
#include <linux/kmemleak.h>
/* [한국어] SZ_4K/SZ_64K/SZ_1M/SZ_16M — 이 형식이 지원하는 네 가지 크기. */
#include <linux/sizes.h>
/* [한국어] kmem_cache_create()/kmalloc_obj() — 레벨 2 테이블 전용 캐시와
 * 인스턴스 할당. */
#include <linux/slab.h>
/* [한국어] 스핀락 정의. 현재 코드에서 직접 쓰이지는 않지만, 과거 연속
 * 엔트리 분할을 락으로 보호하던 시절의 흔적이다. */
#include <linux/spinlock.h>
/* [한국어] u32, phys_addr_t 등 기본 타입. arm_v7s_iopte가 u32의 별칭이다. */
#include <linux/types.h>

/* [한국어] dma_wmb(), wmb() — 테이블 내용이 그것을 가리키는 PTE보다 먼저
 * 보이도록 순서를 세운다. 이것이 없으면 하드웨어가 초기화되지 않은
 * 메모리를 페이지 테이블로 해석할 수 있다. */
#include <asm/barrier.h>

/* Struct accessors */
/* [한국어] 프레임워크의 io_pgtable 포인터에서 이 구현의 바깥 구조체를
 * 복원한다. io-pgtable 계열 파일들의 공통 관용구다. */
#define io_pgtable_to_data(x)						\
	container_of((x), struct arm_v7s_io_pgtable, iop)

/* [한국어] io_pgtable_ops 포인터에서 곧장 인스턴스로 가는 지름길.
 * map/unmap/iova_to_phys 세 콜백이 모두 첫 줄에서 이것을 쓴다. */
#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))	/* [한국어] ops → io_pgtable → 인스턴스의 두 단계 복원을 한 번에 처리한다. */

/*
 * We have 32 bits total; 12 bits resolved at level 1, 8 bits at level 2,
 * and 12 bits in a page.
 * MediaTek extend 2 bits to reach 34bits, 14 bits at lvl1 and 8 bits at lvl2.
 */
/* [한국어] 이 형식의 기본 주소 폭 32비트. 입력/출력 주소가 모두 이 폭을
 * 넘을 수 없다(MediaTek 확장이 있으면 각각 34/35비트까지 늘어난다). */
#define ARM_V7S_ADDR_BITS		32
/* [한국어] 각 레벨이 소비하는 IOVA 비트 수.
 * 레벨 2는 항상 8비트(256개 엔트리)로 고정이고, 레벨 1은 (ias - 20)이다.
 * 기본 32비트 입력이면 레벨 1이 12비트(4096개), MediaTek 확장으로 34비트
 * 입력이면 14비트(16384개)가 된다 — 즉 확장된 주소는 전부 레벨 1이 흡수한다. */
#define _ARM_V7S_LVL_BITS(lvl, cfg)	((lvl) == 1 ? ((cfg)->ias - 20) : 8)
/* [한국어] 각 레벨의 인덱스가 IOVA에서 시작하는 비트 위치.
 * 레벨 1은 20비트(=1MB 섹션 경계), 레벨 2는 12비트(=4KB 페이지 경계). */
#define ARM_V7S_LVL_SHIFT(lvl)		((lvl) == 1 ? 20 : 12)
/* [한국어] 레벨 1 엔트리가 담는 레벨 2 테이블 주소의 정렬 비트 수.
 * 레벨 2 테이블이 1KB(256 × 4바이트)이므로 10비트 정렬이면 충분하다. */
#define ARM_V7S_TABLE_SHIFT		10

/* [한국어] 한 레벨 테이블에 들어가는 PTE 개수 = 2^(레벨 비트 수). */
#define ARM_V7S_PTES_PER_LVL(lvl, cfg)	(1 << _ARM_V7S_LVL_BITS(lvl, cfg))
/* [한국어] 테이블 하나의 바이트 크기 = 엔트리 개수 × 4바이트.
 * 레벨 1은 16KB(기본) 또는 64KB(MTK 확장), 레벨 2는 항상 1KB다.
 * 레벨 2가 페이지보다 작아 슬랩 캐시를 쓰는 이유가 여기 있다. */
#define ARM_V7S_TABLE_SIZE(lvl, cfg)						\
	(ARM_V7S_PTES_PER_LVL(lvl, cfg) * sizeof(arm_v7s_iopte))

/* [한국어] 각 레벨의 리프 엔트리 하나가 덮는 크기.
 * 레벨 1은 1MB(섹션), 레벨 2는 4KB(페이지)다. */
#define ARM_V7S_BLOCK_SIZE(lvl)		(1UL << ARM_V7S_LVL_SHIFT(lvl))
/* [한국어] 그 레벨의 리프 엔트리에서 물리 주소가 차지하는 비트 마스크.
 * ~0U를 시프트해 만들므로 상위 비트가 전부 1이 된다. */
#define ARM_V7S_LVL_MASK(lvl)		((u32)(~0U << ARM_V7S_LVL_SHIFT(lvl)))
/* [한국어] 레벨 1의 테이블 엔트리에서 다음 레벨 테이블 주소의 마스크
 * (상위 22비트, 1KB 정렬). 리프 엔트리와 마스크가 다르다는 점이 중요하다. */
#define ARM_V7S_TABLE_MASK		((u32)(~0U << ARM_V7S_TABLE_SHIFT))
/* [한국어] 그 레벨의 인덱스를 잘라 낼 마스크 = 엔트리 개수 - 1. */
#define _ARM_V7S_IDX_MASK(lvl, cfg)	(ARM_V7S_PTES_PER_LVL(lvl, cfg) - 1)
/* [한국어] IOVA에서 해당 레벨의 테이블 인덱스를 뽑는 매크로.
 * 문장식(statement expression)으로 감싸 lvl을 지역 변수 _l에 한 번만
 * 평가하는데, lvl 인자에 부작용이 있는 식(예: lvl++)이 와도 안전하게
 * 하려는 방어다. */
#define ARM_V7S_LVL_IDX(addr, lvl, cfg)	({				\
	int _l = lvl;							\
	((addr) >> ARM_V7S_LVL_SHIFT(_l)) & _ARM_V7S_IDX_MASK(_l, cfg); \
})	/* [한국어] 문장식의 끝 — 마지막 식의 값이 매크로의 결과가 된다. */

/*
 * Large page/supersection entries are effectively a block of 16 page/section
 * entries, along the lines of the LPAE contiguous hint, but all with the
 * same output address. For want of a better common name we'll call them
 * "contiguous" versions of their respective page/section entries here, but
 * noting the distinction (WRT to TLB maintenance) that they represent *one*
 * entry repeated 16 times, not 16 separate entries (as in the LPAE case).
 */
/* [한국어] 연속 엔트리 하나가 차지하는 엔트리 칸 수 = 16.
 * 64KB 큰 페이지는 4KB 엔트리 16개, 16MB 슈퍼섹션은 1MB 엔트리 16개다.
 * 원본 주석이 강조하는 차이: LPAE의 contiguous 힌트는 "서로 다른 16개
 * 엔트리에 붙는 힌트"지만, 여기서는 **하나의 엔트리가 16번 반복되는 것**이다.
 * 그래서 TLB 관점에서도 엔트리가 하나이고, 부분 해제가 불가능하다. */
#define ARM_V7S_CONT_PAGES		16

/* PTE type bits: these are all mixed up with XN/PXN bits in most cases */
/* [한국어] 레벨 1 엔트리의 타입 = 다음 레벨 테이블을 가리킨다.
 * 원본 주석이 지적하듯 타입 비트가 XN/PXN 비트와 자리를 공유해서,
 * 아래 매크로들이 얽혀 보이는 이유가 그것이다. */
#define ARM_V7S_PTE_TYPE_TABLE		0x1
/* [한국어] 리프 엔트리(섹션 또는 페이지)의 타입 비트.
 * 레벨 1에서는 섹션, 레벨 2에서는 작은 페이지를 뜻한다. */
#define ARM_V7S_PTE_TYPE_PAGE		0x2
/* [한국어] 레벨 2의 연속(64KB 큰 페이지) 엔트리 타입.
 * 값이 TABLE과 같은 0x1인데, 레벨 2에는 테이블이 없으므로 충돌하지 않는다.
 * 즉 "레벨 2에서 비트 1이 0이면 큰 페이지"라는 판별이 성립한다. */
#define ARM_V7S_PTE_TYPE_CONT_PAGE	0x1

/* [한국어] 엔트리가 유효한지 판별한다. 하위 2비트가 모두 0이면 무효다.
 * 타입 값이 무엇이든 0이 아니면 유효하다는 단순한 규칙이다. */
#define ARM_V7S_PTE_IS_VALID(pte)	(((pte) & 0x3) != 0)
/* [한국어] 엔트리가 다음 레벨 테이블을 가리키는지 판별한다.
 * 레벨 1에서만 참일 수 있다 — 레벨 2가 마지막이라 테이블이 없기 때문이다. */
#define ARM_V7S_PTE_IS_TABLE(pte, lvl) \
	((lvl) == 1 && (((pte) & 0x3) == ARM_V7S_PTE_TYPE_TABLE))	/* [한국어] 레벨 1에서만, 그리고 타입 비트가 정확히 TABLE일 때만 참이다. */

/* Page table bits */
/* [한국어] XN(eXecute Never) 비트 — 이 매핑에서 명령어를 가져오지 못하게 한다.
 * 위치가 레벨마다 다르다: 레벨 1은 비트 4, 레벨 2는 비트 0이다.
 * 4 * (2 - lvl) 계산이 그것을 표현한다. */
#define ARM_V7S_ATTR_XN(lvl)		BIT(4 * (2 - (lvl)))
/* [한국어] B(Bufferable) 비트 — 쓰기 버퍼를 쓸 수 있는 메모리인지. */
#define ARM_V7S_ATTR_B			BIT(2)
/* [한국어] C(Cacheable) 비트 — 캐시 가능한 메모리인지. B와 함께
 * 메모리 타입을 결정한다. */
#define ARM_V7S_ATTR_C			BIT(3)
/* [한국어] 레벨 1 테이블 엔트리의 NS(Non-Secure) 비트 — 다음 레벨 테이블이
 * 비보안 메모리에 있음을 표시한다. C 비트와 자리를 공유하는데, 테이블
 * 엔트리에는 캐시 속성이 없어 충돌하지 않는다. */
#define ARM_V7S_ATTR_NS_TABLE		BIT(3)
/* [한국어] 섹션(레벨 1 리프) 엔트리의 NS 비트 — 이 매핑이 가리키는
 * 물리 메모리가 비보안임을 표시한다. */
#define ARM_V7S_ATTR_NS_SECTION		BIT(19)

/* [한국어] 레벨 1 리프 엔트리를 슈퍼섹션(16MB)으로 만드는 비트.
 * 이 비트가 서면 그 엔트리를 포함한 16칸이 하나의 16MB 매핑이 된다. */
#define ARM_V7S_CONT_SECTION		BIT(18)
/* [한국어] 레벨 2 연속(큰 페이지) 엔트리에서 XN 비트가 옮겨 가는 시프트량.
 * 작은 페이지는 XN이 비트 0인데, 큰 페이지에서는 그 자리가 타입 비트로
 * 쓰이므로 비트 15로 이사한다. arm_v7s_pte_to_cont()가 그 이동을 수행한다. */
#define ARM_V7S_CONT_PAGE_XN_SHIFT	15

/*
 * The attribute bits are consistently ordered*, but occupy bits [17:10] of
 * a level 1 PTE vs. bits [11:4] at level 2. Thus we define the individual
 * fields relative to that 8-bit block, plus a total shift relative to the PTE.
 */
/* [한국어] 속성 8비트 묶음이 PTE 안에서 시작하는 위치.
 * 레벨 1이면 10, 레벨 2면 4다(16 - lvl*6이 그 값을 준다).
 * 이 파일이 속성을 "레벨 무관한 8비트 블록"으로 다룬 뒤 마지막에
 * 이 시프트만 적용하는 설계라, 레벨별 분기가 크게 줄어든다. */
#define ARM_V7S_ATTR_SHIFT(lvl)		(16 - (lvl) * 6)

/* [한국어] 속성 블록이 8비트라는 것을 나타내는 마스크. */
#define ARM_V7S_ATTR_MASK		0xff
/* [한국어] AP0 — 단순화된 권한 모델에서는 Access Flag로 쓰인다
 * (아래 ARM_V7S_PTE_AF 참조). */
#define ARM_V7S_ATTR_AP0		BIT(0)
/* [한국어] AP1 — 비특권(사용자) 접근 허용 비트. */
#define ARM_V7S_ATTR_AP1		BIT(1)
/* [한국어] AP2 — 읽기 전용 비트. 세우면 쓰기가 금지된다. */
#define ARM_V7S_ATTR_AP2		BIT(5)
/* [한국어] S(Shareable) 비트 — 이 매핑이 공유 가능한 메모리임을 표시한다.
 * 캐시 코히런시 프로토콜에 참여하게 만든다. */
#define ARM_V7S_ATTR_S			BIT(6)
/* [한국어] nG(not Global) 비트 — TLB 엔트리를 ASID로 태그해 문맥마다
 * 구분되게 한다. 이 구현은 항상 세운다. */
#define ARM_V7S_ATTR_NG			BIT(7)
/* [한국어] TEX 필드가 속성 블록 안에서 시작하는 비트 위치. */
#define ARM_V7S_TEX_SHIFT		2
/* [한국어] TEX 필드의 폭(3비트). */
#define ARM_V7S_TEX_MASK		0x7
/* [한국어] TEX(Type Extension) 값을 속성 블록의 제자리에 넣는 매크로.
 * TEX remap이 켜져 있으면 이 값이 PRRR/NMRR 레지스터의 인덱스로 쓰인다. */
#define ARM_V7S_ATTR_TEX(val)		(((val) & ARM_V7S_TEX_MASK) << ARM_V7S_TEX_SHIFT)

/* MediaTek extend the bits below for PA 32bit/33bit/34bit */
/* [한국어] MediaTek 확장: 물리 주소의 32번째 비트를 PTE 비트 9에 담는다.
 * 원래 예약이던 자리를 재활용한 것이라, 이 quirk가 꺼져 있으면 0으로 남는다. */
#define ARM_V7S_ATTR_MTK_PA_BIT32	BIT(9)
/* [한국어] MediaTek 확장: 물리 주소의 33번째 비트를 PTE 비트 4에 담는다. */
#define ARM_V7S_ATTR_MTK_PA_BIT33	BIT(4)
/* [한국어] MediaTek 확장: 물리 주소의 34번째 비트를 PTE 비트 5에 담는다.
 * 세 비트를 합쳐 32비트 형식으로 35비트 물리 주소까지 표현하게 된다.
 * 비트 4/5가 AP2/XN 자리와 겹치므로, 이 확장을 쓰려면 권한을 포기해야 한다
 * — 그래서 alloc이 NO_PERMS quirk를 함께 요구한다. */
#define ARM_V7S_ATTR_MTK_PA_BIT34	BIT(5)

/* *well, except for TEX on level 2 large pages, of course :( */
/* [한국어] 위 "속성 순서는 일관적"이라는 설명의 유일한 예외.
 * 레벨 2 큰 페이지에서는 TEX가 비트 6~8로 옮겨 간다. 원본 주석의
 * 별표와 아쉬움 섞인 표정이 그 사정을 말해 준다. */
#define ARM_V7S_CONT_PAGE_TEX_SHIFT	6
/* [한국어] 그 옮겨 간 TEX 필드의 마스크. arm_v7s_pte_to_cont()가
 * 원래 자리의 TEX를 여기로 이동시킨다. */
#define ARM_V7S_CONT_PAGE_TEX_MASK	(ARM_V7S_TEX_MASK << ARM_V7S_CONT_PAGE_TEX_SHIFT)

/* Simplified access permissions */
/* [한국어] 단순화된 권한 모델(AP[2:1])에서 AP0은 Access Flag로 재해석된다.
 * 하드웨어가 이 비트가 0인 엔트리를 접근하면 폴트를 내므로, 매핑을 만들 때
 * 반드시 세워야 한다. */
#define ARM_V7S_PTE_AF			ARM_V7S_ATTR_AP0
/* [한국어] AP1 = 비특권 접근 허용. 세우지 않으면 특권 접근만 가능하다. */
#define ARM_V7S_PTE_AP_UNPRIV		ARM_V7S_ATTR_AP1
/* [한국어] AP2 = 읽기 전용. 세우면 쓰기가 금지된다.
 * 즉 쓰기를 허용하려면 이 비트를 세우지 않아야 한다(부정 논리). */
#define ARM_V7S_PTE_AP_RDONLY		ARM_V7S_ATTR_AP2

/* Register bits */
/* [한국어] 캐시 정책 값: 캐시 불가(Non-Cacheable). TTBR과 NMRR에서 쓴다. */
#define ARM_V7S_RGN_NC			0
/* [한국어] 캐시 정책: Write-Back, Write-Allocate. 가장 성능이 좋은 설정이라
 * 이 구현이 기본으로 쓴다. */
#define ARM_V7S_RGN_WBWA		1
/* [한국어] 캐시 정책: Write-Through. */
#define ARM_V7S_RGN_WT			2
/* [한국어] 캐시 정책: Write-Back(할당 없음). */
#define ARM_V7S_RGN_WB			3

/* [한국어] PRRR의 타입 값: 디바이스 메모리(순서가 보장되고 캐시되지 않는다). */
#define ARM_V7S_PRRR_TYPE_DEVICE	1
/* [한국어] PRRR의 타입 값: 일반(normal) 메모리. */
#define ARM_V7S_PRRR_TYPE_NORMAL	2
/* [한국어] PRRR에서 n번 TEX 인덱스의 타입을 지정하는 매크로.
 * TEX remap은 PTE의 TEX/C/B 비트를 인덱스로 삼아 이 레지스터를 조회하므로,
 * 여기서 각 인덱스가 어떤 메모리 타입인지 정의해야 한다. */
#define ARM_V7S_PRRR_TR(n, type)	(((type) & 0x3) << ((n) * 2))
/* [한국어] PRRR의 DS0 — 비공유 디바이스 메모리를 공유로 취급할지. */
#define ARM_V7S_PRRR_DS0		BIT(16)
/* [한국어] PRRR의 DS1 — 공유 디바이스 메모리의 공유성 설정. */
#define ARM_V7S_PRRR_DS1		BIT(17)
/* [한국어] PRRR의 NS0 — 비공유 일반 메모리의 공유성 설정. */
#define ARM_V7S_PRRR_NS0		BIT(18)
/* [한국어] PRRR의 NS1 — 공유 일반 메모리의 공유성 설정.
 * 이 구현은 NS1만 세워 "S 비트가 선 일반 메모리는 공유"로 만든다. */
#define ARM_V7S_PRRR_NS1		BIT(19)
/* [한국어] n번 인덱스의 Outer Shareable 여부. 세우지 않으면 아우터 공유,
 * 세우면 이너 공유(Not Outer Shareable)가 된다. */
#define ARM_V7S_PRRR_NOS(n)		BIT((n) + 24)

/* [한국어] NMRR에서 n번 인덱스의 이너(inner) 캐시 정책을 지정한다. */
#define ARM_V7S_NMRR_IR(n, attr)	(((attr) & 0x3) << ((n) * 2))
/* [한국어] NMRR에서 n번 인덱스의 아우터(outer) 캐시 정책을 지정한다.
 * 아우터 필드가 상위 16비트에 있어 시프트에 +16이 붙는다. */
#define ARM_V7S_NMRR_OR(n, attr)	(((attr) & 0x3) << ((n) * 2 + 16))

/* [한국어] TTBR의 S 비트 — 페이지 테이블 자체가 공유 메모리에 있음을 표시한다. */
#define ARM_V7S_TTBR_S			BIT(1)
/* [한국어] TTBR의 NOS 비트 — 테이블이 이너 공유 영역에 있음을 표시한다. */
#define ARM_V7S_TTBR_NOS		BIT(5)
/* [한국어] TTBR의 아우터 캐시 정책 필드(비트 3~4). 테이블 워크가 테이블을
 * 읽을 때 어떤 캐시 정책을 쓸지 정한다. */
#define ARM_V7S_TTBR_ORGN_ATTR(attr)	(((attr) & 0x3) << 3)
/* [한국어] TTBR의 이너 캐시 정책 필드. 특이하게 2비트가 떨어져 있어
 * (비트 0과 비트 6) 값을 쪼개 넣어야 한다 — ARMv7 TTBR의 역사적 배치 탓이다. */
#define ARM_V7S_TTBR_IRGN_ATTR(attr)					\
	((((attr) & 0x1) << 6) | (((attr) & 0x2) >> 1))

/* [한국어] 페이지 테이블 메모리를 어느 존에서 할당할지 결정하는 분기.
 * PTE가 32비트라 테이블의 물리 주소도 4GB 아래여야 한다.
 * ZONE_DMA32가 있는 커널에서는 그 존(4GB 이하)을, 없으면 더 좁은
 * ZONE_DMA를 쓴다. MediaTek TTBR 확장을 쓰는 경우에만 이 제약이 풀린다. */
#ifdef CONFIG_ZONE_DMA32
/* [한국어] 레벨 1 테이블 할당용 GFP 플래그(4GB 이하 보장). */
#define ARM_V7S_TABLE_GFP_DMA GFP_DMA32
/* [한국어] 레벨 2 테이블용 슬랩 캐시 플래그(같은 존에서 받도록). */
#define ARM_V7S_TABLE_SLAB_FLAGS SLAB_CACHE_DMA32
#else
/* [한국어] ZONE_DMA32가 없으면 더 낮은 ZONE_DMA를 쓴다. */
#define ARM_V7S_TABLE_GFP_DMA GFP_DMA
/* [한국어] 그에 대응하는 슬랩 캐시 플래그. */
#define ARM_V7S_TABLE_SLAB_FLAGS SLAB_CACHE_DMA
#endif

/* [한국어] 페이지 테이블 엔트리 하나의 타입. LPAE의 64비트와 달리
 * 이 형식은 32비트다 — 그래서 물리 주소 확장에 예약 비트를 재활용하는
 * MediaTek 방식이 필요했다. */
typedef u32 arm_v7s_iopte;

/* [한국어] 셀프테스트가 실행 중인지 표시하는 전역 플래그.
 * 설정자: arm_v7s_do_selftests()가 시작/끝에서 켜고 끈다.
 * 읽는 자: init_pte()와 __arm_v7s_map()의 WARN_ON(!selftest_running).
 * 왜 필요한가: 셀프테스트는 "이미 매핑된 곳에 또 매핑하면 -EEXIST가 나는가"를
 *              일부러 시험한다. 그때마다 WARN이 뜨면 로그가 지저분해지므로,
 *              테스트 중에는 경고를 억제한다.
 * 동기화: 부팅 시 단일 스레드에서만 다루므로 락이 없다. */
static bool selftest_running;

/* [한국어] ARMv7S 페이지 테이블 인스턴스 하나의 상태.
 * 수명: arm_v7s_alloc_pgtable()에서 만들어져 arm_v7s_free_pgtable()에서 해제.
 * LPAE 구현보다 필드가 적은데, 레벨이 2단계로 고정이고 기하가 단순해서다. */
struct arm_v7s_io_pgtable {
	struct io_pgtable	iop;
	/* [한국어] 프레임워크가 보는 부분(임베드). cfg(기하/quirks)와 ops를 담는다.
	 * 설정자: alloc이 ops를 채우고 cfg를 복사한다.
	 * 읽는 자: 이 파일의 거의 모든 함수가 cfg의 quirks/ias/oas를 읽는다.
	 * 동기화: alloc 이후 읽기 전용이다. */

	arm_v7s_iopte		*pgd;
	/* [한국어] 최상위(레벨 1) 테이블의 커널 가상 주소.
	 * 설정자: alloc의 __arm_v7s_alloc_table(1, ...).
	 * 읽는 자: map/unmap/iova_to_phys가 워크의 출발점으로 삼는다.
	 * 크기: 기본 16KB(4096 엔트리), MTK 확장 시 64KB(16384 엔트리).
	 * 왜 페이지 할당자를 쓰는가: 레벨 2와 달리 크기가 페이지 배수라
	 *                            __get_free_pages가 적합하다.
	 * 동기화: 포인터는 불변이고, 내용은 호출자의 락이 보호한다. */

	struct kmem_cache	*l2_tables;
	/* [한국어] 레벨 2 테이블 전용 슬랩 캐시.
	 * 설정자: alloc의 kmem_cache_create().
	 * 읽는 자: __arm_v7s_alloc_table(2, ...)과 __arm_v7s_free_table().
	 * 왜 전용 캐시인가: 레벨 2 테이블이 1KB로 페이지보다 작아 페이지
	 *                   할당자를 쓰면 대부분을 낭비한다. 게다가 1KB 정렬이
	 *                   필요해 kmalloc으로는 보장할 수 없어, 정렬을 지정한
	 *                   슬랩 캐시를 만든다.
	 * 동기화: 슬랩 자체가 락을 갖는다. */
};

/* [한국어] arm_v7s_pte_is_cont()의 전방 선언.
 * iopte_to_paddr()가 이 함수를 쓰는데 정의가 아래에 있어서, 순서를
 * 바꾸는 대신 선언만 앞당겼다. */
static bool arm_v7s_pte_is_cont(arm_v7s_iopte pte, int lvl);

/*
 * [한국어]
 * __arm_v7s_dma_addr - 테이블의 커널 가상 주소를 DMA 주소로 바꾼다
 *
 * @pages: 테이블의 커널 가상 주소.
 * @return: 그 테이블의 DMA 주소.
 *
 * 왜 virt_to_phys를 그대로 쓰는가: 이 구현은 IOMMU가 물리 주소를 그대로
 * 다룰 수 있다고 전제한다(__arm_v7s_alloc_table의 dma != phys 검사가
 * 그 전제를 강제한다). 그래서 DMA 주소와 물리 주소가 같고, 매핑 시
 * 받아 둔 dma_addr을 따로 보관하지 않아도 언제든 다시 계산할 수 있다.
 *
 * 실행 컨텍스트: 캐시 동기화와 언매핑 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   __arm_v7s_free_table() / __arm_v7s_pte_sync() → [__arm_v7s_dma_addr]
 */
static dma_addr_t __arm_v7s_dma_addr(void *pages)
{
	/* [한국어] 물리 주소를 DMA 주소로 그대로 캐스팅한다. 두 값이 같다는
	 * 전제가 alloc에서 검증되므로 성립한다. */
	return (dma_addr_t)virt_to_phys(pages);
}

/*
 * [한국어]
 * arm_v7s_is_mtk_enabled - MediaTek 물리 주소 확장이 켜져 있는지 판별한다
 *
 * @cfg: 이 인스턴스의 설정.
 * @return: MTK 확장을 써야 하면 true.
 *
 * 두 조건이 모두 필요하다:
 *  - CONFIG_PHYS_ADDR_T_64BIT: 커널의 phys_addr_t가 32비트를 넘어야
 *    확장된 주소를 담을 수 있다. 32비트 phys_addr_t 커널에서는 확장이
 *    무의미하므로 컴파일 타임에 꺼진다.
 *  - IO_PGTABLE_QUIRK_ARM_MTK_EXT: 드라이버가 명시적으로 요청해야 한다.
 *
 * 실행 컨텍스트: 주소 인코딩/디코딩 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   paddr_to_iopte() / iopte_to_paddr() / alloc_pgtable()
 *   → [arm_v7s_is_mtk_enabled]
 */
static bool arm_v7s_is_mtk_enabled(struct io_pgtable_cfg *cfg)
{
	/* [한국어] 커널이 64비트 물리 주소를 다룰 수 있고, 드라이버가 확장을
	 * 요청했을 때만 참이다. IS_ENABLED를 쓰므로 조건이 컴파일 타임에
	 * 접혀 확장을 안 쓰는 커널에서는 코드가 최적화되어 사라진다. */
	return IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT) &&
		(cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_EXT);
}

/*
 * [한국어]
 * to_mtk_iopte - 물리 주소의 상위 비트들을 MediaTek 확장 비트에 심는다
 *
 * @paddr: 원래 물리 주소(최대 35비트).
 * @pte: 하위 32비트 주소와 속성이 이미 담긴 PTE.
 * @return: 확장 비트가 추가된 PTE.
 *
 * 왜 이런 방식인가: PTE가 32비트뿐이라 32비트를 넘는 물리 주소를 담을
 * 자리가 없다. MediaTek은 형식이 예약해 둔 비트 세 개(9, 4, 5)를 골라
 * 주소의 32/33/34번째 비트를 흩뿌려 담는 방식을 택했다. 비트가 연속되지
 * 않으므로 이렇게 하나씩 옮기는 코드가 필요하다.
 *
 * 대가: 비트 4와 5는 원래 XN과 AP2(읽기 전용) 자리다. 그래서 이 확장을
 * 쓰면 권한 제어를 포기해야 하고, alloc이 NO_PERMS quirk를 함께 요구한다.
 *
 * 실행 컨텍스트: 매핑 생성과 테이블 설치 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   paddr_to_iopte() / arm_v7s_install_table() → [to_mtk_iopte]
 */
static arm_v7s_iopte to_mtk_iopte(phys_addr_t paddr, arm_v7s_iopte pte)
{
	/* [한국어] 물리 주소의 32번째 비트가 서 있으면 PTE 비트 9에 심는다. */
	if (paddr & BIT_ULL(32))
		pte |= ARM_V7S_ATTR_MTK_PA_BIT32;
	/* [한국어] 33번째 비트는 PTE 비트 4에 심는다(원래 XN 자리). */
	if (paddr & BIT_ULL(33))
		pte |= ARM_V7S_ATTR_MTK_PA_BIT33;
	/* [한국어] 34번째 비트는 PTE 비트 5에 심는다(원래 AP2 자리). */
	if (paddr & BIT_ULL(34))
		pte |= ARM_V7S_ATTR_MTK_PA_BIT34;
	/* [한국어] 확장 비트가 채워진 PTE를 돌려준다. */
	return pte;
}

/*
 * [한국어]
 * paddr_to_iopte - 물리 주소를 PTE의 주소 필드 형식으로 인코딩한다
 *
 * @paddr: 매핑할 물리 주소.
 * @lvl: 이 엔트리가 속한 레벨(1이면 1MB 정렬, 2면 4KB 정렬).
 * @cfg: 설정 — MTK 확장 여부를 판별한다.
 * @return: 주소 비트만 담긴 PTE 조각(속성은 호출자가 OR 한다).
 *
 * 레벨에 따라 마스크가 다른 이유: 레벨 1 리프는 1MB 정렬이므로 하위 20비트를
 * 버려도 되고, 레벨 2는 4KB 정렬이라 하위 12비트만 버린다. 그 버려진 자리에
 * 속성 비트들이 들어간다.
 *
 * 실행 컨텍스트: 매핑 생성 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   arm_v7s_init_pte() → [paddr_to_iopte] → to_mtk_iopte()(확장 시)
 */
static arm_v7s_iopte paddr_to_iopte(phys_addr_t paddr, int lvl,
				    struct io_pgtable_cfg *cfg)
{
	/* [한국어] 해당 레벨의 정렬 경계로 마스킹해 주소 비트만 남긴다.
	 * 32비트로 잘리므로 상위 비트는 여기서 사라진다. */
	arm_v7s_iopte pte = paddr & ARM_V7S_LVL_MASK(lvl);

	/* [한국어] MTK 확장이 켜져 있으면 잘려 나간 상위 비트들을 예약 비트에
	 * 심어 복원 가능하게 만든다. */
	if (arm_v7s_is_mtk_enabled(cfg))
		return to_mtk_iopte(paddr, pte);

	/* [한국어] 확장이 없으면 32비트로 자른 결과가 그대로 답이다. */
	return pte;
}

/*
 * [한국어]
 * iopte_to_paddr - PTE에서 물리 주소를 복원한다
 *
 * @pte: 해석할 엔트리.
 * @lvl: 그 엔트리가 속한 레벨.
 * @cfg: 설정 — MTK 확장 여부 판별용.
 * @return: 복원된 물리 주소.
 *
 * 마스크가 세 갈래인 이유가 이 함수의 핵심이다:
 *  - 테이블 엔트리(레벨 1): 다음 레벨 테이블은 1KB 정렬이므로
 *    ARM_V7S_TABLE_MASK(상위 22비트)를 쓴다.
 *  - 연속 엔트리: 16칸이 하나이므로 정렬이 16배 크다. 마스크에 16을
 *    곱하는 것이 그 표현이다(예: 레벨 2에서 4KB × 16 = 64KB 정렬).
 *  - 일반 리프: 그 레벨의 기본 마스크.
 *
 * 실행 컨텍스트: 워크와 조회 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   iopte_deref() / arm_v7s_iova_to_phys() → [iopte_to_paddr]
 */
static phys_addr_t iopte_to_paddr(arm_v7s_iopte pte, int lvl,
				  struct io_pgtable_cfg *cfg)
{
	/* [한국어] 엔트리 종류에 따라 달라지는 주소 마스크. */
	arm_v7s_iopte mask;
	/* [한국어] 복원한 물리 주소. */
	phys_addr_t paddr;

	/* [한국어] 다음 레벨 테이블을 가리키는 엔트리라면 1KB 정렬 마스크. */
	if (ARM_V7S_PTE_IS_TABLE(pte, lvl))
		mask = ARM_V7S_TABLE_MASK;
	/* [한국어] 연속(큰 페이지/슈퍼섹션) 엔트리라면 정렬이 16배 크다.
	 * 곱셈으로 마스크를 넓히는 방식이 다소 특이하지만, 마스크가
	 * 2의 거듭제곱 경계라 정확히 원하는 결과가 나온다. */
	else if (arm_v7s_pte_is_cont(pte, lvl))
		mask = ARM_V7S_LVL_MASK(lvl) * ARM_V7S_CONT_PAGES;
	else
		/* [한국어] 평범한 리프 엔트리라면 그 레벨의 기본 마스크. */
		mask = ARM_V7S_LVL_MASK(lvl);

	/* [한국어] 마스크로 주소 비트만 뽑는다. */
	paddr = pte & mask;
	/* [한국어] MTK 확장이 없으면 여기까지가 전부다. */
	if (!arm_v7s_is_mtk_enabled(cfg))
		return paddr;

	/* [한국어] 확장이 있으면 흩어져 있던 상위 비트들을 되돌린다.
	 * to_mtk_iopte()의 역연산이다. */
	if (pte & ARM_V7S_ATTR_MTK_PA_BIT32)
		paddr |= BIT_ULL(32);
	/* [한국어] 33번째 비트를 복원한다. */
	if (pte & ARM_V7S_ATTR_MTK_PA_BIT33)
		paddr |= BIT_ULL(33);
	/* [한국어] 34번째 비트를 복원한다. */
	if (pte & ARM_V7S_ATTR_MTK_PA_BIT34)
		paddr |= BIT_ULL(34);
	/* [한국어] 최대 35비트 물리 주소가 복원되었다. */
	return paddr;
}

/*
 * [한국어]
 * iopte_deref - 테이블 엔트리가 가리키는 다음 레벨 테이블의 주소를 얻는다
 *
 * @pte: 테이블을 가리키는 엔트리.
 * @lvl: 그 엔트리가 속한 레벨.
 * @data: 인스턴스(설정 접근용).
 * @return: 다음 레벨 테이블의 커널 가상 주소.
 *
 * phys_to_virt를 쓸 수 있는 이유: 페이지 테이블은 항상 lowmem(직접 매핑
 * 영역)에서 할당된다. GFP_DMA/DMA32로 할당하는 것이 그 보장을 겸한다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   __arm_v7s_map() / __arm_v7s_unmap() / iova_to_phys() → [iopte_deref]
 */
static arm_v7s_iopte *iopte_deref(arm_v7s_iopte pte, int lvl,
				  struct arm_v7s_io_pgtable *data)
{
	/* [한국어] PTE에서 물리 주소를 복원한 뒤 커널 가상 주소로 바꾼다. */
	return phys_to_virt(iopte_to_paddr(pte, lvl, &data->iop.cfg));
}

/*
 * [한국어]
 * __arm_v7s_alloc_table - 지정한 레벨의 페이지 테이블을 할당한다
 *
 * @lvl: 만들 테이블의 레벨(1 또는 2).
 * @gfp: 할당 플래그(레벨 2에만 그대로 쓰인다).
 * @data: 인스턴스.
 * @return: 테이블의 커널 가상 주소, 실패하면 NULL.
 *
 * 레벨마다 할당 방식이 다르다:
 *  - 레벨 1: 16KB(또는 64KB)라 페이지 배수이므로 __get_free_pages를 쓴다.
 *    GFP는 gfp 인자가 아니라 gfp_l1(DMA 존)을 쓰는데, 최상위 테이블은
 *    alloc 시점(프로세스 컨텍스트)에만 만들어지기 때문이다.
 *  - 레벨 2: 1KB라 페이지보다 작고 1KB 정렬이 필요해 전용 슬랩 캐시를 쓴다.
 *    여기서는 호출자가 준 gfp를 그대로 쓴다(atomic 컨텍스트일 수 있다).
 *
 * 물리 주소 검증이 이 함수의 핵심 안전장치다. PTE가 32비트뿐이므로
 * 테이블 주소가 그 안에 들어가야 한다. TTBR 확장을 쓰면 oas 비트 수까지
 * 허용되고, 아니면 32비트 캐스팅 결과가 원본과 같은지로 검사한다.
 *
 * 비코히런트 플랫폼 처리: dma_map_single로 테이블을 하드웨어에 보이게 하고,
 * 그 결과 DMA 주소가 물리 주소와 다르면 실패로 처리한다 — 원본 주석이
 * 설명하듯, IOMMU는 물리 주소를 그대로 다루므로 DMA 계층이 주소를
 * 바꿔치기하면 테이블을 찾을 수 없게 되기 때문이다.
 *
 * kmemleak_ignore의 이유: 레벨 2 테이블의 유일한 참조가 PTE 안의 물리
 * 주소라, kmemleak의 포인터 스캔이 그것을 참조로 인식하지 못해 오탐이 난다.
 *
 * 실행 컨텍스트: alloc(레벨 1)과 map 경로(레벨 2). 후자는 atomic일 수 있다.
 *
 * 호출 체인:
 *   arm_v7s_alloc_pgtable() / __arm_v7s_map() → [__arm_v7s_alloc_table]
 */
static void *__arm_v7s_alloc_table(int lvl, gfp_t gfp,
				   struct arm_v7s_io_pgtable *data)
{
	/* [한국어] 기하와 quirks를 담은 설정. */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] DMA 매핑과 오류 로깅에 쓸 디바이스. */
	struct device *dev = cfg->iommu_dev;
	/* [한국어] 할당된 테이블의 물리 주소. */
	phys_addr_t phys;
	/* [한국어] 비코히런트 경로에서 얻는 DMA 주소. */
	dma_addr_t dma;
	/* [한국어] 이 레벨 테이블의 바이트 크기. */
	size_t size = ARM_V7S_TABLE_SIZE(lvl, cfg);
	/* [한국어] 할당 결과. NULL 초기화는 아래 분기에서 어느 쪽도 타지
	 * 않는 경우(잘못된 lvl)를 대비한 방어다. */
	void *table = NULL;
	/* [한국어] 레벨 1 할당에 쓸 GFP 플래그. */
	gfp_t gfp_l1;

	/*
	 * ARM_MTK_TTBR_EXT extend the translation table base support larger
	 * memory address.
	 */
	/* [한국어] TTBR 확장이 있으면 테이블이 4GB 위에 있어도 되므로
	 * 평범한 GFP_KERNEL을 쓴다. 없으면 DMA 존에서 받아 32비트 안에
	 * 들어가도록 강제한다. */
	gfp_l1 = cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT ?
		 GFP_KERNEL : ARM_V7S_TABLE_GFP_DMA;

	/* [한국어] 레벨 1은 페이지 배수 크기라 페이지 할당자를 쓴다.
	 * __GFP_ZERO로 0 초기화해 모든 엔트리가 무효 상태로 시작하게 한다. */
	if (lvl == 1)
		table = (void *)__get_free_pages(gfp_l1 | __GFP_ZERO, get_order(size));
	/* [한국어] 레벨 2는 1KB라 전용 슬랩 캐시에서 받는다. zalloc이므로
	 * 역시 0으로 초기화된다. */
	else if (lvl == 2)
		table = kmem_cache_zalloc(data->l2_tables, gfp);

	/* [한국어] 할당 실패 — 아직 잡은 자원이 없어 그냥 NULL을 반환한다. */
	if (!table)
		return NULL;

	/* [한국어] 테이블의 물리 주소를 구한다 — PTE에 담을 값이자 검증 대상이다. */
	phys = virt_to_phys(table);
	/* [한국어] 주소가 PTE에 담기는지 검증한다.
	 * TTBR 확장이 있으면 oas 비트 수까지 허용하고,
	 * 없으면 32비트로 캐스팅했을 때 값이 보존되는지로 판별한다.
	 * DMA 존에서 받았으므로 실패할 일이 거의 없지만, 존 설정이 어긋난
	 * 시스템에서 조용히 잘못된 테이블을 쓰는 것보다는 실패가 낫다. */
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT ?
	    phys >= (1ULL << cfg->oas) : phys != (arm_v7s_iopte)phys) {
		/* Doesn't fit in PTE */
		dev_err(dev, "Page table does not fit in PTE: %pa", &phys);	/* [한국어] 어느 물리 주소가 PTE에 담기지 못했는지 남긴다. */
		goto out_free;	/* [한국어] 쓸 수 없는 테이블이므로 메모리를 되돌리러 간다. */
	}
	/* [한국어] 테이블 워크가 캐시 코히런트하지 않은 플랫폼이라면,
	 * 테이블 메모리를 DMA로 매핑해 하드웨어가 볼 수 있게 만든다. */
	if (!cfg->coherent_walk) {
		/* [한국어] DMA_TO_DEVICE 방향인 이유: 테이블은 CPU가 쓰고
		 * 하드웨어가 읽기만 하기 때문이다. */
		dma = dma_map_single(dev, table, size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))	/* [한국어] DMA 매핑 실패 — 테이블을 하드웨어에 보이게 할 수 없다. */
			goto out_free;
		/*
		 * We depend on the IOMMU being able to work with any physical
		 * address directly, so if the DMA layer suggests otherwise by
		 * translating or truncating them, that bodes very badly...
		 */
		/* [한국어] DMA 계층이 주소를 바꿔치기했다면(다른 IOMMU를 거치거나
		 * 바운스 버퍼를 썼다면) 이 테이블을 하드웨어가 찾을 수 없다.
		 * 조용히 잘못 동작하느니 실패시킨다. */
		if (dma != phys)
			goto out_unmap;
	}
	/* [한국어] 레벨 2 테이블은 kmemleak의 오탐 대상이다 — 유일한 참조가
	 * PTE 안의 물리 주소라 포인터 스캔이 찾지 못하기 때문이다. */
	if (lvl == 2)
		kmemleak_ignore(table);
	/* [한국어] 검증과 매핑을 모두 통과한 테이블을 돌려준다. */
	return table;

/* [한국어] DMA 주소가 물리 주소와 달라 쓸 수 없는 경우의 되감기. */
out_unmap:
	dev_err(dev, "Cannot accommodate DMA translation for IOMMU page tables\n");	/* [한국어] DMA 계층이 주소를 바꿔치기해 이 테이블을 쓸 수 없다는 것을 남긴다. */
	dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);
/* [한국어] 메모리는 확보했지만 쓸 수 없는 모든 경우가 모이는 지점. */
out_free:
	/* [한국어] 레벨에 맞는 방식으로 반납해야 한다 — 할당 방식이 다르므로
	 * 해제 방식도 갈린다. */
	if (lvl == 1)
		free_pages((unsigned long)table, get_order(size));
	else
		kmem_cache_free(data->l2_tables, table);
	/* [한국어] 호출자에게 실패를 알린다. */
	return NULL;
}

/*
 * [한국어]
 * __arm_v7s_free_table - 페이지 테이블 메모리를 반납한다
 *
 * @table: 반납할 테이블의 커널 가상 주소.
 * @lvl: 그 테이블의 레벨.
 * @data: 인스턴스.
 * @return: 없음.
 *
 * 순서에 주목: DMA 언매핑을 **먼저** 하고 메모리를 해제한다. 반대로 하면
 * 해제된 메모리에 대한 DMA 매핑을 만지게 되어 캐시 관리가 어긋난다.
 *
 * 실행 컨텍스트: unmap 경로(테이블이 비었을 때)와 free_pgtable.
 *
 * 호출 체인:
 *   __arm_v7s_map()(설치 경쟁 패배) / __arm_v7s_unmap() /
 *   arm_v7s_free_pgtable() → [__arm_v7s_free_table]
 */
static void __arm_v7s_free_table(void *table, int lvl,
				 struct arm_v7s_io_pgtable *data)
{
	/* [한국어] 설정(코히런시 여부와 테이블 크기 계산용). */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] DMA 언매핑에 필요한 디바이스. */
	struct device *dev = cfg->iommu_dev;
	/* [한국어] 할당 때와 같은 크기를 계산한다 — dma_unmap이 요구한다. */
	size_t size = ARM_V7S_TABLE_SIZE(lvl, cfg);

	/* [한국어] 비코히런트 플랫폼이라면 alloc에서 만든 DMA 매핑을 먼저 푼다.
	 * 주소는 보관하지 않고 다시 계산하는데, DMA 주소와 물리 주소가 같음이
	 * alloc에서 보장되었기 때문이다. */
	if (!cfg->coherent_walk)
		dma_unmap_single(dev, __arm_v7s_dma_addr(table), size,
				 DMA_TO_DEVICE);
	/* [한국어] 레벨에 맞는 방식으로 메모리를 반납한다. */
	if (lvl == 1)
		free_pages((unsigned long)table, get_order(size));
	else
		kmem_cache_free(data->l2_tables, table);
}

/*
 * [한국어]
 * __arm_v7s_pte_sync - 갱신한 PTE들을 하드웨어에 보이게 만든다
 *
 * @ptep: 갱신한 첫 엔트리의 주소.
 * @num_entries: 갱신한 엔트리 개수.
 * @cfg: 설정 — 코히런시 여부 판별용.
 * @return: 없음.
 *
 * 왜 필요한가: 테이블 워크가 캐시 코히런트하지 않은 플랫폼에서는
 * CPU가 쓴 PTE가 캐시에만 있고 메모리에 아직 없을 수 있다. 하드웨어는
 * 메모리를 직접 읽으므로 옛 값을 보게 된다. dma_sync_single_for_device가
 * 그 캐시 라인을 메모리로 밀어낸다.
 *
 * 코히런트 플랫폼에서는 아무것도 하지 않고 즉시 돌아간다 — 그런 경우가
 * 대부분이라 이 이른 반환이 성능상 중요하다.
 *
 * 실행 컨텍스트: 모든 PTE 갱신 직후. 캐시 조작뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   __arm_v7s_set_pte() / arm_v7s_install_table() / __arm_v7s_map()
 *   → [__arm_v7s_pte_sync] → dma_sync_single_for_device()
 */
static void __arm_v7s_pte_sync(arm_v7s_iopte *ptep, int num_entries,
			       struct io_pgtable_cfg *cfg)
{
	/* [한국어] 워크가 코히런트하면 캐시가 자동으로 일관되므로 할 일이 없다. */
	if (cfg->coherent_walk)
		return;

	/* [한국어] 갱신한 엔트리 구간의 캐시를 메모리로 밀어낸다.
	 * DMA_TO_DEVICE 방향은 "CPU가 썼으니 디바이스가 볼 수 있게 하라"는 뜻이다. */
	dma_sync_single_for_device(cfg->iommu_dev, __arm_v7s_dma_addr(ptep),
				   num_entries * sizeof(*ptep), DMA_TO_DEVICE);
}
/*
 * [한국어]
 * __arm_v7s_set_pte - 같은 값의 PTE를 여러 칸에 쓰고 캐시를 동기화한다
 *
 * @ptep: 쓰기 시작할 엔트리 주소.
 * @pte: 쓸 값. 0이면 해제를 뜻한다.
 * @num_entries: 쓸 칸 수.
 * @cfg: 설정.
 * @return: 없음.
 *
 * 왜 같은 값을 반복해서 쓰는가: 이 형식의 연속(큰 페이지/슈퍼섹션) 엔트리가
 * 정확히 그런 모양이기 때문이다 — 하드웨어는 같은 내용의 엔트리 16개를
 * 하나로 취급한다. 일반 엔트리를 쓸 때는 num_entries가 1이다.
 *
 * 캐시 동기화를 함수 안에서 하는 이유: 쓰기와 동기화를 짝지어 두면
 * 호출부에서 동기화를 빠뜨릴 수 없다.
 *
 * 실행 컨텍스트: 매핑 생성과 해제 경로.
 *
 * 호출 체인:
 *   arm_v7s_init_pte() / __arm_v7s_unmap() → [__arm_v7s_set_pte]
 *   → __arm_v7s_pte_sync()
 */
static void __arm_v7s_set_pte(arm_v7s_iopte *ptep, arm_v7s_iopte pte,
			      int num_entries, struct io_pgtable_cfg *cfg)
{
	/* [한국어] 쓰기 루프 인덱스. */
	int i;

	/* [한국어] 같은 값을 num_entries칸에 채운다. 연속 엔트리의 정의가
	 * 곧 이 반복이다. */
	for (i = 0; i < num_entries; i++)
		ptep[i] = pte;

	/* [한국어] 쓴 구간을 하드웨어에 보이게 만든다(비코히런트 플랫폼에서만). */
	__arm_v7s_pte_sync(ptep, num_entries, cfg);
}

/*
 * [한국어]
 * arm_v7s_prot_to_pte - IOMMU 보호 플래그를 PTE 속성 비트로 변환한다
 *
 * @prot: IOMMU_READ/WRITE/CACHE/NOEXEC/MMIO/PRIV 조합.
 * @lvl: 이 엔트리가 놓일 레벨(속성 위치가 달라진다).
 * @cfg: 설정 — NO_PERMS와 ARM_NS quirk를 본다.
 * @return: 속성과 타입 비트가 담긴 PTE(주소는 호출자가 OR 한다).
 *
 * 이 함수의 구조가 이 형식의 특징을 잘 보여 준다. 속성을 **레벨과 무관한
 * 8비트 블록**으로 먼저 조립한 뒤, 마지막에 ARM_V7S_ATTR_SHIFT(lvl)만큼
 * 한 번에 옮긴다. 그 뒤에 붙는 XN/B/C/타입 비트는 시프트 대상이 아니라
 * PTE의 고정 자리에 들어가므로 시프트 이후에 OR 한다.
 *
 * NO_PERMS quirk: MediaTek 확장이 AP2/XN 자리를 물리 주소 비트로
 * 빼앗아 가므로, 권한 비트를 아예 쓰지 않는 모드가 필요하다.
 * ap 변수가 그 스위치이며, 꺼지면 AF와 AP1/AP2, XN을 모두 생략한다.
 *
 * 메모리 타입 결정:
 *  - IOMMU_MMIO: TEX를 0으로 두고 B만 세워 "디바이스 메모리"로 만든다.
 *  - IOMMU_CACHE: TEX=1에 B|C를 세워 캐시 가능한 일반 메모리로.
 *  - 둘 다 없으면: TEX=1만 세워 캐시 불가 일반 메모리로.
 * TEX 값 1이 PRRR/NMRR에서 어떤 타입에 대응하는지는 alloc_pgtable()이
 * 그 레지스터들을 채울 때 정해진다.
 *
 * 실행 컨텍스트: 매핑 생성 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   arm_v7s_init_pte() → [arm_v7s_prot_to_pte]
 */
static arm_v7s_iopte arm_v7s_prot_to_pte(int prot, int lvl,
					 struct io_pgtable_cfg *cfg)
{
	/* [한국어] 권한 비트를 쓸지 여부. NO_PERMS quirk가 있으면 끈다 —
	 * MediaTek 확장이 그 비트 자리를 주소 비트로 쓰기 때문이다. */
	bool ap = !(cfg->quirks & IO_PGTABLE_QUIRK_NO_PERMS);
	/* [한국어] 속성 8비트 블록의 출발점.
	 * nG: TLB를 ASID로 태그해 문맥별로 구분되게 한다.
	 * S: 공유 메모리로 표시해 캐시 코히런시에 참여시킨다.
	 * 두 비트는 항상 세운다 — IOMMU 매핑에 늘 적합한 설정이기 때문이다. */
	arm_v7s_iopte pte = ARM_V7S_ATTR_NG | ARM_V7S_ATTR_S;

	/* [한국어] MMIO가 아니면 TEX=1로 둔다. alloc이 PRRR에서 인덱스 1을
	 * 디바이스 타입으로 정의하므로... 실제로는 아래 B/C 비트와 조합되어
	 * 최종 타입이 결정된다. MMIO일 때만 TEX를 0으로 남긴다. */
	if (!(prot & IOMMU_MMIO))
		pte |= ARM_V7S_ATTR_TEX(1);
	/* [한국어] 권한 비트를 쓰는 모드에서만 아래를 적용한다. */
	if (ap) {
		/* [한국어] Access Flag를 세운다. 이것이 없으면 하드웨어가
		 * 첫 접근에서 폴트를 낸다 — 소프트웨어 AF 관리를 하지 않으므로
		 * 항상 미리 세워 둔다. */
		pte |= ARM_V7S_PTE_AF;
		/* [한국어] 특권 전용 요청이 아니면 비특권 접근도 허용한다.
		 * 조건이 부정형인 것은 AP1이 "허용" 비트이기 때문이다. */
		if (!(prot & IOMMU_PRIV))
			pte |= ARM_V7S_PTE_AP_UNPRIV;
		/* [한국어] 쓰기가 요청되지 않았으면 읽기 전용 비트를 세운다.
		 * AP2는 "금지" 의미라 조건이 부정형이다. */
		if (!(prot & IOMMU_WRITE))
			pte |= ARM_V7S_PTE_AP_RDONLY;
	}
	/* [한국어] 여기까지 조립한 8비트 속성 블록을 해당 레벨의 제자리로 옮긴다.
	 * 레벨 1이면 10비트, 레벨 2면 4비트 왼쪽으로 간다. */
	pte <<= ARM_V7S_ATTR_SHIFT(lvl);

	/* [한국어] XN 비트는 속성 블록 밖에 있어 시프트 이후에 붙인다.
	 * 권한을 쓰지 않는 모드에서는 그 자리가 주소 비트라 건드리면 안 된다. */
	if ((prot & IOMMU_NOEXEC) && ap)
		pte |= ARM_V7S_ATTR_XN(lvl);
	/* [한국어] MMIO 매핑은 B만 세워 "디바이스 메모리"로 만든다 —
	 * 순서가 보장되고 캐시되지 않는다. */
	if (prot & IOMMU_MMIO)
		pte |= ARM_V7S_ATTR_B;
	/* [한국어] 캐시 가능 요청이면 B와 C를 함께 세워 write-back 일반
	 * 메모리로 만든다. 둘 다 아니면 캐시 불가 일반 메모리가 된다. */
	else if (prot & IOMMU_CACHE)
		pte |= ARM_V7S_ATTR_B | ARM_V7S_ATTR_C;

	/* [한국어] 리프 엔트리 타입 비트를 붙인다. 연속 엔트리로 바꿀 때는
	 * arm_v7s_pte_to_cont()가 이 비트를 다시 손본다. */
	pte |= ARM_V7S_PTE_TYPE_PAGE;
	/* [한국어] 보안 확장이 있는 시스템에서 섹션 매핑이 비보안 메모리를
	 * 가리킨다고 표시한다. 레벨 1에만 있는 비트다. */
	if (lvl == 1 && (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS))
		pte |= ARM_V7S_ATTR_NS_SECTION;

	/* [한국어] 주소를 제외한 PTE가 완성되었다. */
	return pte;
}

/*
 * [한국어]
 * arm_v7s_pte_to_cont - 일반 리프 엔트리를 연속(큰 페이지/슈퍼섹션) 형태로 바꾼다
 *
 * @pte: 일반 리프 엔트리(prot_to_pte가 만든 것).
 * @lvl: 그 엔트리의 레벨.
 * @return: 연속 엔트리로 변환된 값.
 *
 * 레벨 1은 간단하다: CONT_SECTION 비트 하나만 세우면 슈퍼섹션이 된다.
 *
 * 레벨 2가 까다로운 이유: 큰 페이지(64KB)는 작은 페이지와 **비트 배치가
 * 다르다**. 타입 비트가 0x2에서 0x1로 바뀌면서 원래 XN이 있던 비트 0이
 * 타입 자리로 넘어가고, XN은 비트 15로, TEX는 비트 6~8로 각각 이사한다.
 * 그 이동을 세 단계로 수행한다:
 *  1) 현재 XN과 TEX 값을 추출한다.
 *  2) XOR로 그 비트들과 기존 타입 비트를 **지운다**(모두 원래 자리에서 사라짐).
 *  3) 새 위치에 XN과 TEX를 넣고 큰 페이지 타입 비트를 세운다.
 * XOR을 쓰는 이유는 추출한 값이 그 자리에 있음이 확실하므로,
 * XOR이 곧 클리어가 되기 때문이다(AND ~x와 같은 효과이면서 더 짧다).
 *
 * 실행 컨텍스트: 매핑 생성 경로(num_entries > 1일 때). 순수 계산이다.
 *
 * 호출 체인:
 *   arm_v7s_init_pte() → [arm_v7s_pte_to_cont]
 */
static arm_v7s_iopte arm_v7s_pte_to_cont(arm_v7s_iopte pte, int lvl)
{
	/* [한국어] 레벨 1(섹션 → 슈퍼섹션)은 비트 하나로 끝난다. */
	if (lvl == 1) {
		pte |= ARM_V7S_CONT_SECTION;
	/* [한국어] 레벨 2(작은 페이지 → 큰 페이지)는 비트 재배치가 필요하다. */
	} else if (lvl == 2) {
		/* [한국어] 현재 XN 비트 값을 추출해 둔다(작은 페이지에서는 비트 0). */
		arm_v7s_iopte xn = pte & ARM_V7S_ATTR_XN(lvl);
		/* [한국어] 현재 TEX 필드 값을 추출해 둔다.
		 * 주의: 여기서 쓰는 마스크는 CONT_PAGE_TEX_MASK인데, 작은 페이지의
		 * TEX가 이미 ATTR_SHIFT(2)=4만큼 옮겨져 비트 6~8에 있기 때문이다.
		 * 즉 원래 자리와 목적지 마스크가 우연히 일치한다. */
		arm_v7s_iopte tex = pte & ARM_V7S_CONT_PAGE_TEX_MASK;

		/* [한국어] XOR로 XN, TEX, 그리고 작은 페이지 타입 비트를 모두
		 * 지운다. 세 값 모두 현재 pte에 확실히 존재하므로 XOR이
		 * 클리어와 같은 효과를 낸다. */
		pte ^= xn | tex | ARM_V7S_PTE_TYPE_PAGE;
		/* [한국어] 지운 값들을 큰 페이지에서의 새 자리로 옮겨 넣고,
		 * 큰 페이지 타입 비트를 세운다. */
		pte |= (xn << ARM_V7S_CONT_PAGE_XN_SHIFT) |
		       (tex << ARM_V7S_CONT_PAGE_TEX_SHIFT) |
		       ARM_V7S_PTE_TYPE_CONT_PAGE;
	}
	/* [한국어] 연속 엔트리로 변환된 값. 이것이 16칸에 그대로 반복 기록된다. */
	return pte;
}

/*
 * [한국어]
 * arm_v7s_pte_is_cont - 엔트리가 연속(큰 페이지/슈퍼섹션)인지 판별한다
 *
 * @pte: 검사할 엔트리.
 * @lvl: 그 엔트리의 레벨.
 * @return: 연속 엔트리면 true.
 *
 * 판별 방식이 레벨마다 다르다:
 *  - 레벨 1: 테이블 엔트리가 아니면서 CONT_SECTION 비트가 서 있으면
 *    슈퍼섹션이다. 테이블 검사를 먼저 하는 이유는 그 비트 자리가
 *    테이블 엔트리에서는 다른 의미이기 때문이다.
 *  - 레벨 2: 타입 비트가 PAGE(0x2)가 **아니면** 큰 페이지다. 레벨 2에
 *    테이블이 없으므로 "작은 페이지가 아니면 큰 페이지"라는 판별이 성립한다.
 *
 * 실행 컨텍스트: 주소 복원과 해제 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   iopte_to_paddr() / __arm_v7s_unmap() / iova_to_phys()
 *   → [arm_v7s_pte_is_cont]
 */
static bool arm_v7s_pte_is_cont(arm_v7s_iopte pte, int lvl)
{
	/* [한국어] 레벨 1에서는 테이블이 아닌 리프 중 CONT_SECTION이 선 것. */
	if (lvl == 1 && !ARM_V7S_PTE_IS_TABLE(pte, lvl))
		return pte & ARM_V7S_CONT_SECTION;
	/* [한국어] 레벨 2에서는 작은 페이지 타입이 아니면 곧 큰 페이지다. */
	else if (lvl == 2)
		return !(pte & ARM_V7S_PTE_TYPE_PAGE);
	/* [한국어] 그 밖의 경우(레벨 1의 테이블 엔트리)는 연속일 수 없다. */
	return false;
}

/* [한국어] __arm_v7s_unmap()의 전방 선언.
 * arm_v7s_init_pte()가 "테이블 엔트리를 블록으로 덮어쓰기 전에 먼저
 * 해제한다"는 처리를 위해 이 함수를 호출하는데, 정의가 훨씬 아래에 있어
 * 선언이 필요하다. 두 함수가 서로를 부르는 상호 재귀 구조인 셈이다. */
static size_t __arm_v7s_unmap(struct arm_v7s_io_pgtable *,
			      struct iommu_iotlb_gather *, unsigned long,
			      size_t, int, arm_v7s_iopte *);

/*
 * [한국어]
 * arm_v7s_init_pte - 리프 엔트리들을 실제로 기록한다
 *
 * @data: 인스턴스.
 * @iova: 매핑할 IOVA(오류 처리에서 재계산에 쓴다).
 * @paddr: 매핑할 물리 주소.
 * @prot: 보호 플래그.
 * @lvl: 기록할 레벨.
 * @num_entries: 채울 칸 수(1이면 일반, 16이면 연속 엔트리).
 * @ptep: 기록을 시작할 엔트리 주소.
 * @return: 0 성공, -EINVAL(옛 테이블 해제 실패), -EEXIST(이미 매핑됨).
 *
 * 두 가지 충돌 상황을 다룬다:
 *  1) 그 자리에 **테이블 엔트리**가 있는 경우: 1MB 섹션을 만들려는데
 *     이미 4KB 페이지들이 매핑된 레벨 2 테이블이 있는 상황이다.
 *     이때는 그 테이블을 통째로 해제한 뒤 블록 엔트리로 덮어쓴다.
 *     tblp 계산이 흥미로운데, ptep에서 현재 인덱스를 빼 테이블의 시작
 *     주소를 복원한다 — __arm_v7s_unmap()이 테이블 시작을 요구하기 때문이다.
 *  2) 그 자리에 **다른 리프 엔트리**가 있는 경우: IOMMU API 규약상
 *     덮어쓰기는 금지이므로 -EEXIST로 거부한다. 셀프테스트는 이 경로를
 *     일부러 시험하므로 WARN을 억제한다.
 *
 * 실행 컨텍스트: 매핑 생성의 마지막 단계. 호출자의 락이 직렬화를 보장하므로
 * 평범한 대입으로 기록한다.
 *
 * 호출 체인:
 *   __arm_v7s_map() → [arm_v7s_init_pte] → __arm_v7s_unmap()(충돌 시),
 *   arm_v7s_prot_to_pte(), arm_v7s_pte_to_cont(), __arm_v7s_set_pte()
 */
static int arm_v7s_init_pte(struct arm_v7s_io_pgtable *data,
			    unsigned long iova, phys_addr_t paddr, int prot,
			    int lvl, int num_entries, arm_v7s_iopte *ptep)
{
	/* [한국어] 기하와 quirks. */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] 조립할 엔트리 값. */
	arm_v7s_iopte pte;
	/* [한국어] 충돌 검사 루프의 인덱스. */
	int i;

	/* [한국어] 채울 자리들이 비어 있는지 먼저 확인한다. */
	for (i = 0; i < num_entries; i++)
		/* [한국어] 그 자리에 하위 테이블이 걸려 있다면, 더 작은 매핑들이
		 * 있던 자리를 큰 블록으로 덮으려는 상황이다. */
		if (ARM_V7S_PTE_IS_TABLE(ptep[i], lvl)) {
			/*
			 * We need to unmap and free the old table before
			 * overwriting it with a block entry.
			 */
			/* [한국어] 해제 호출에 넘길 테이블 시작 주소. */
			arm_v7s_iopte *tblp;
			/* [한국어] 이 레벨의 블록 크기 — 해제할 범위이자
			 * 성공 여부를 판별할 기준이다. */
			size_t sz = ARM_V7S_BLOCK_SIZE(lvl);

			/* [한국어] ptep에서 현재 인덱스를 빼 테이블의 첫 엔트리
			 * 주소를 복원한다. __arm_v7s_unmap()이 테이블 시작을
			 * 받아 스스로 인덱싱하기 때문이다. */
			tblp = ptep - ARM_V7S_LVL_IDX(iova, lvl, cfg);
			/* [한국어] 그 블록 범위를 통째로 해제한다. 정확히 sz만큼
			 * 해제되지 않으면 테이블 구조가 예상과 다르다는 뜻이라
			 * WARN과 함께 실패시킨다. gather를 NULL로 넘기는 것은
			 * 이 경로가 곧바로 새 매핑을 쓰므로 지연 무효화가
			 * 의미 없기 때문이다. */
			if (WARN_ON(__arm_v7s_unmap(data, NULL, iova + i * sz,
						    sz, lvl, tblp) != sz))
				return -EINVAL;
		/* [한국어] 테이블은 아닌데 값이 있다면 이미 매핑된 리프다. */
		} else if (ptep[i]) {
			/* We require an unmap first */
			/* [한국어] 셀프테스트는 이 경로를 일부러 시험하므로
			 * 그때는 경고를 내지 않는다. */
			WARN_ON(!selftest_running);
			return -EEXIST;	/* [한국어] 이미 매핑된 자리이므로 먼저 unmap 하라는 뜻으로 거부한다. */
		}

	/* [한국어] 보호 플래그를 이 레벨의 속성 비트로 변환한다. */
	pte = arm_v7s_prot_to_pte(prot, lvl, cfg);
	/* [한국어] 여러 칸을 채운다면 연속 엔트리 형태로 바꿔야 한다.
	 * 그렇지 않으면 하드웨어가 16개의 독립 엔트리로 오해한다. */
	if (num_entries > 1)
		pte = arm_v7s_pte_to_cont(pte, lvl);

	/* [한국어] 물리 주소 비트를 얹어 엔트리를 완성한다. */
	pte |= paddr_to_iopte(paddr, lvl, cfg);

	/* [한국어] 완성된 값을 num_entries칸에 기록하고 캐시를 동기화한다. */
	__arm_v7s_set_pte(ptep, pte, num_entries, cfg);
	/* [한국어] 매핑 완료. */
	return 0;
}

/*
 * [한국어]
 * arm_v7s_install_table - 새 레벨 2 테이블을 부모 엔트리에 원자적으로 설치한다
 *
 * @table: 설치할 테이블의 커널 가상 주소.
 * @ptep: 그 테이블을 가리키게 될 부모 엔트리의 주소.
 * @curr: 설치 전에 기대하는 부모 값(항상 0으로 호출된다).
 * @cfg: 설정 — quirks와 코히런시 여부.
 * @return: cmpxchg 이전의 실제 값. 0이면 내가 이겼다는 뜻이다.
 *
 * 경쟁 시나리오: 두 CPU가 같은 1MB 영역 안의 서로 다른 4KB를 동시에
 * 매핑하면, 둘 다 그 영역의 레벨 2 테이블이 없다고 보고 각자 만들어
 * 설치하려 한다. cmpxchg가 하나만 이기게 하고, 진 쪽은 자기 테이블을
 * 버린 뒤 이긴 쪽의 것을 쓴다.
 *
 * 메모리 순서: 새 테이블의 내용(전부 0)이 그것을 가리키는 PTE보다 먼저
 * 보여야 한다. dma_wmb()가 그 순서를 세운다. 원본 주석이 설명하듯
 * cmpxchg_release로 대신할 수도 있어 보이지만, !CONFIG_SMP에서는 release가
 * 아무 순서도 보장하지 않아(DMA는 SMP 여부와 무관하다) 명시적 배리어가 필요하다.
 *
 * 설치 후 __arm_v7s_pte_sync를 부르는 이유: 비코히런트 플랫폼에서는
 * cmpxchg로 쓴 부모 엔트리도 캐시에만 있으므로 메모리로 밀어내야 한다.
 *
 * 실행 컨텍스트: 매핑 생성 중. 락 없이 동시 실행될 수 있다.
 *
 * 호출 체인:
 *   __arm_v7s_map() → [arm_v7s_install_table] → dma_wmb(), cmpxchg_relaxed()
 */
static arm_v7s_iopte arm_v7s_install_table(arm_v7s_iopte *table,
					   arm_v7s_iopte *ptep,
					   arm_v7s_iopte curr,
					   struct io_pgtable_cfg *cfg)
{
	/* [한국어] 설치할 테이블의 물리 주소. */
	phys_addr_t phys = virt_to_phys(table);
	/* [한국어] old는 cmpxchg 이전 값, new는 설치할 값. */
	arm_v7s_iopte old, new;

	/* [한국어] 테이블 주소에 "다음 레벨 테이블" 타입 비트를 붙인다.
	 * 리프와 달리 권한 비트가 없다 — 권한은 리프에서만 판정된다. */
	new = phys | ARM_V7S_PTE_TYPE_TABLE;

	/* [한국어] TTBR 확장을 쓰는 MediaTek 구성에서는 테이블 자체가 4GB
	 * 위에 있을 수 있어, 주소 상위 비트를 예약 비트에 심어야 한다. */
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT)
		new = to_mtk_iopte(phys, new);

	/* [한국어] 보안 확장이 있는 시스템에서 하위 테이블이 비보안 메모리에
	 * 있음을 표시한다. */
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS)
		new |= ARM_V7S_ATTR_NS_TABLE;

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	/* [한국어] DMA 관찰자 기준의 쓰기 배리어. 새 테이블을 0으로 초기화한
	 * 쓰기가 아래 PTE 설치보다 먼저 보이게 만든다. 이것이 없으면
	 * 하드웨어가 유효한 PTE를 따라가 초기화되지 않은 메모리를 걷는다. */
	dma_wmb();

	/* [한국어] 부모 엔트리가 여전히 curr(=0)일 때만 new로 바꾼다.
	 * relaxed 변형인 이유는 필요한 순서를 위 dma_wmb()가 이미 세워서다. */
	old = cmpxchg_relaxed(ptep, curr, new);
	/* [한국어] 비코히런트 플랫폼에서 방금 쓴 부모 엔트리도 메모리로 밀어낸다. */
	__arm_v7s_pte_sync(ptep, 1, cfg);

	/* [한국어] 0이 아니면 경쟁에서 졌다는 뜻이라, 호출자가 자기 테이블을 버린다. */
	return old;
}

/*
 * [한국어]
 * __arm_v7s_map - 재귀적으로 레벨을 내려가며 매핑을 설치한다
 *
 * @data: 인스턴스.
 * @iova: 매핑할 IOVA.
 * @paddr: 매핑할 물리 주소.
 * @size: 매핑 크기(한 번에 하나의 페이지 크기).
 * @prot: 보호 플래그.
 * @lvl: 현재 레벨(1에서 시작해 2까지).
 * @ptep: 현재 레벨 테이블의 시작 주소.
 * @gfp: 하위 테이블 할당에 쓸 플래그.
 * @return: 0 성공, -EINVAL/-ENOMEM/-EEXIST.
 *
 * 재귀의 종료 조건이 이 함수의 핵심이다. num_entries = size >> LVL_SHIFT(lvl)가
 * 0이 아니면 "이 레벨에서 리프를 설치할 수 있다"는 뜻이다:
 *  - 레벨 1(shift 20)에서 size가 1MB면 num_entries=1 → 섹션 하나.
 *  - 레벨 1에서 size가 16MB면 num_entries=16 → 슈퍼섹션.
 *  - 레벨 1에서 size가 4KB면 0 → 더 내려가야 한다.
 *  - 레벨 2(shift 12)에서 size가 4KB면 1, 64KB면 16.
 * 즉 크기 하나로 "어느 레벨에 어떤 형태로 설치할지"가 자동으로 결정된다.
 *
 * 하위 테이블이 없으면 만들어 설치하고, 경쟁에서 지면 자기 것을 버린다.
 * 이미 있으면 __arm_v7s_pte_sync를 부르는데, 원본 주석이 밝히듯
 * "이 엔트리가 이미 동기화됐는지 알 방법이 없어서" 안전하게 한 번 더 한다.
 *
 * 실행 컨텍스트: 매핑 경로. 호출자의 락 아래에서 실행되지만 테이블 설치만
 * cmpxchg로 자체 방어한다.
 *
 * 호출 체인:
 *   arm_v7s_map_pages() → [__arm_v7s_map] (재귀)
 *   → arm_v7s_init_pte(), __arm_v7s_alloc_table(), arm_v7s_install_table()
 */
static int __arm_v7s_map(struct arm_v7s_io_pgtable *data, unsigned long iova,
			 phys_addr_t paddr, size_t size, int prot,
			 int lvl, arm_v7s_iopte *ptep, gfp_t gfp)
{
	/* [한국어] 기하와 quirks. */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] pte는 읽은 값, cptep는 하위 레벨 테이블의 주소. */
	arm_v7s_iopte pte, *cptep;
	/* [한국어] 이 레벨에서 리프로 덮을 수 있는 칸 수. 0이면 더 내려가야 한다.
	 * 1이면 일반 엔트리, 16이면 연속 엔트리가 된다. */
	int num_entries = size >> ARM_V7S_LVL_SHIFT(lvl);

	/* Find our entry at the current level */
	/* [한국어] 이 레벨에서 IOVA가 가리키는 엔트리로 포인터를 옮긴다. */
	ptep += ARM_V7S_LVL_IDX(iova, lvl, cfg);

	/* If we can install a leaf entry at this level, then do so */
	/* [한국어] 크기가 이 레벨의 블록에 맞으면 여기서 리프를 설치하고 끝낸다.
	 * 재귀의 종료 조건이다. */
	if (num_entries)
		return arm_v7s_init_pte(data, iova, paddr, prot,
					lvl, num_entries, ptep);

	/* We can't allocate tables at the final level */
	/* [한국어] 레벨 2에 도달했는데도 크기가 맞지 않는다면, 호출자가
	 * pgsize_bitmap에 없는 크기를 넘긴 것이다 — 있을 수 없는 상황이라 WARN. */
	if (WARN_ON(lvl == 2))
		return -EINVAL;

	/* Grab a pointer to the next level */
	/* [한국어] 하위 테이블이 이미 있는지 확인한다. 동시 갱신 가능성 때문에
	 * READ_ONCE로 한 번만 읽는다. */
	pte = READ_ONCE(*ptep);
	/* [한국어] 비어 있으면 하위 테이블을 새로 만들어야 한다. */
	if (!pte) {
		/* [한국어] 레벨 2 테이블을 슬랩에서 받는다. gfp를 그대로 넘기므로
		 * atomic 컨텍스트에서도 안전하다. */
		cptep = __arm_v7s_alloc_table(lvl + 1, gfp, data);
		if (!cptep)	/* [한국어] 하위 테이블을 만들 메모리가 없다. */
			return -ENOMEM;

		/* [한국어] 부모 엔트리가 여전히 0일 때만 설치된다. */
		pte = arm_v7s_install_table(cptep, ptep, 0, cfg);
		/* [한국어] 반환값이 0이 아니면 다른 CPU가 먼저 설치했다는 뜻이므로
		 * 내가 만든 테이블은 버린다. */
		if (pte)
			__arm_v7s_free_table(cptep, lvl + 1, data);
	} else {
		/* We've no easy way of knowing if it's synced yet, so... */
		/* [한국어] 이미 있는 엔트리라도 다른 CPU가 방금 설치하고 아직
		 * 캐시를 밀어내지 않았을 수 있다. 확인할 방법이 없으니 한 번 더
		 * 동기화한다 — 중복 비용보다 누락 위험이 크기 때문이다. */
		__arm_v7s_pte_sync(ptep, 1, cfg);
	}

	/* [한국어] 이제 부모 엔트리에 하위 테이블이 있다면 그리로 내려간다. */
	if (ARM_V7S_PTE_IS_TABLE(pte, lvl)) {
		cptep = iopte_deref(pte, lvl, data);
	/* [한국어] 테이블이 아닌 값이 있다면 이미 큰 블록이 매핑되어 있다는
	 * 뜻이다. 그 위에 작은 매핑을 얹을 수 없으므로 거부한다. */
	} else if (pte) {
		/* We require an unmap first */
		WARN_ON(!selftest_running);	/* [한국어] 셀프테스트가 아니라면 상위 계층의 map/unmap 짝이 어긋난 것이다. */
		return -EEXIST;	/* [한국어] 큰 블록 위에 작은 매핑을 얹을 수는 없다. */
	}

	/* Rinse, repeat */
	/* [한국어] 한 레벨 아래에서 같은 일을 반복한다. 레벨이 2단계뿐이라
	 * 재귀 깊이는 최대 1이다. */
	return __arm_v7s_map(data, iova, paddr, size, prot, lvl + 1, cptep, gfp);
}

/*
 * [한국어]
 * arm_v7s_map_pages - io-pgtable의 map_pages 진입점
 *
 * @ops: 연산 테이블(인스턴스 복원용).
 * @iova: 매핑 시작 IOVA.
 * @paddr: 매핑할 물리 주소 시작.
 * @pgsize: 페이지 크기(pgsize_bitmap의 값 중 하나).
 * @pgcount: 매핑할 페이지 개수.
 * @prot: 보호 플래그.
 * @gfp: 테이블 할당 플래그.
 * @mapped: 출력 인자 — 매핑한 바이트 수가 누적된다.
 * @return: 0 성공, -ERANGE(주소 범위 초과), -EINVAL, 그 밖의 오류.
 *
 * 왜 단순 루프인가: LPAE 구현과 달리 여기서는 한 번에 한 페이지씩
 * __arm_v7s_map을 부른다. 연속 엔트리 처리가 크기 하나로 결정되므로
 * 굳이 여러 페이지를 묶어 처리할 필요가 없기 때문이다.
 *
 * 마지막 wmb()의 의미: 이 시점 이후 드라이버가 TLB를 만지거나 디바이스가
 * DMA를 시작하면 완성된 PTE들을 보게 된다. 배리어가 없으면 일부만
 * 반영된 상태로 워크가 일어날 수 있다.
 *
 * 실행 컨텍스트: 드라이버의 map 경로. gfp가 atomic 여부를 알려 준다.
 *
 * 호출 체인:
 *   드라이버 → ops->map_pages → [arm_v7s_map_pages] → __arm_v7s_map()
 */
static int arm_v7s_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			     phys_addr_t paddr, size_t pgsize, size_t pgcount,
			     int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] ops에서 인스턴스를 복원한다. */
	struct arm_v7s_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] 결과. pgcount가 0이면 루프를 돌지 않아 이 초기값이
	 * 그대로 반환되는데, 정상 호출에서는 일어나지 않는다. */
	int ret = -EINVAL;

	/* [한국어] IOVA와 물리 주소가 각각 입력/출력 주소 폭 안에 있는지 검사한다.
	 * 넘으면 PTE에 담을 수 없으므로 -ERANGE로 거부한다. */
	if (WARN_ON(iova >= (1ULL << data->iop.cfg.ias) ||
		    paddr >= (1ULL << data->iop.cfg.oas)))
		return -ERANGE;

	/* [한국어] 읽기도 쓰기도 요청하지 않은 매핑은 의미가 없다. */
	if (!(prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	/* [한국어] 요청된 페이지 수만큼 한 번에 하나씩 매핑한다. */
	while (pgcount--) {
		/* [한국어] 레벨 1(data->pgd)에서 시작해 재귀적으로 내려간다. */
		ret = __arm_v7s_map(data, iova, paddr, pgsize, prot, 1, data->pgd,
				    gfp);
		/* [한국어] 실패하면 그 지점에서 멈춘다. 이미 매핑한 것은
		 * 되돌리지 않고 *mapped로 알린다 — 상위가 그 값을 보고 정리한다. */
		if (ret)
			break;

		/* [한국어] 다음 페이지로 IOVA를 전진시킨다. */
		iova += pgsize;
		/* [한국어] 물리 주소도 함께 전진시킨다(연속 영역 전제). */
		paddr += pgsize;
		/* [한국어] 처리한 바이트 수를 누적한다. */
		*mapped += pgsize;
	}
	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	/* [한국어] 전역 쓰기 배리어. 이후 어떤 주체가 이 IOVA로 워크를
	 * 시작하더라도 완성된 테이블을 보게 만든다. */
	wmb();

	/* [한국어] 마지막 매핑의 결과를 반환한다(전부 성공이면 0). */
	return ret;
}

/*
 * [한국어]
 * arm_v7s_free_pgtable - io-pgtable의 free 진입점
 *
 * @iop: 해제할 페이지 테이블.
 * @return: 없음.
 *
 * 왜 재귀가 필요 없는가: 레벨이 2단계뿐이라 최상위 테이블을 한 번 훑으며
 * 하위 테이블을 해제하면 끝이다. LPAE 구현이 재귀 해제를 쓰는 것과 대비된다.
 *
 * 순서: 하위 테이블들 → 최상위 테이블 → 슬랩 캐시 → 인스턴스.
 * 슬랩 캐시를 테이블보다 나중에 파괴해야 하는 이유는, 캐시가 살아 있어야
 * 그 안의 객체를 반납할 수 있기 때문이다.
 *
 * 실행 컨텍스트: 도메인 해제 시. 이 시점에는 하드웨어가 이 테이블을
 * 참조하지 않음이 보장되어야 한다.
 *
 * 호출 체인:
 *   free_io_pgtable_ops() → init_fns->free → [arm_v7s_free_pgtable]
 *   → __arm_v7s_free_table(), kmem_cache_destroy()
 */
static void arm_v7s_free_pgtable(struct io_pgtable *iop)
{
	/* [한국어] 프레임워크 포인터에서 인스턴스를 복원한다. */
	struct arm_v7s_io_pgtable *data = io_pgtable_to_data(iop);
	/* [한국어] 최상위 테이블 순회 인덱스. */
	int i;

	/* [한국어] 최상위 테이블의 모든 엔트리를 훑는다. */
	for (i = 0; i < ARM_V7S_PTES_PER_LVL(1, &data->iop.cfg); i++) {
		/* [한국어] 엔트리를 읽는다. 해제 시점이라 동시 갱신이 없으므로
		 * READ_ONCE가 필요 없다. */
		arm_v7s_iopte pte = data->pgd[i];

		/* [한국어] 하위 테이블을 가리키는 엔트리라면 그 테이블을 반납한다.
		 * 리프 엔트리(섹션)는 가리키는 것이 물리 페이지라 해제 대상이 아니다. */
		if (ARM_V7S_PTE_IS_TABLE(pte, 1))
			__arm_v7s_free_table(iopte_deref(pte, 1, data),
					     2, data);
	}
	/* [한국어] 최상위 테이블 자체를 반납한다. */
	__arm_v7s_free_table(data->pgd, 1, data);
	/* [한국어] 레벨 2 전용 슬랩 캐시를 파괴한다. 그 안의 객체가 모두
	 * 반납된 뒤여야 하므로 순서가 중요하다. */
	kmem_cache_destroy(data->l2_tables);
	/* [한국어] 인스턴스 구조체를 반납한다. */
	kfree(data);
}

/*
 * [한국어]
 * __arm_v7s_unmap - 재귀적으로 매핑을 해제한다
 *
 * @data: 인스턴스.
 * @gather: TLB 무효화 수집 구조체. NULL이면 지연 무효화를 쓰지 않는다.
 * @iova: 해제 시작 IOVA.
 * @size: 해제 크기.
 * @lvl: 현재 레벨.
 * @ptep: 현재 레벨 테이블의 시작 주소.
 * @return: 해제한 바이트 수, 실패하면 0.
 *
 * 이 함수가 거부하는 두 가지 상황이 이 형식의 근본 제약을 드러낸다:
 *  1) 연속 엔트리의 부분 해제(64KB 큰 페이지 중 4KB만 해제 등).
 *     원본 주석이 설명하듯, 16개 PTE를 원자적으로 다시 쓸 방법이 없고
 *     TEX remap을 전제할 수 없어 "분할 중"을 표시할 소프트웨어 비트도 없다.
 *     실무(DMA API)에서는 이런 요청이 오지 않으므로 그냥 거부한다.
 *  2) 레벨 1의 섹션을 4KB 단위로 해제하려는 경우도 같은 이유로 거부한다.
 *
 * 크기가 이 레벨에 맞으면(num_entries != 0):
 *  - 엔트리들을 0으로 지운다.
 *  - 지운 것이 하위 테이블이었다면 그 테이블도 반납하고, 워크 캐시까지
 *    비우도록 tlb_flush_walk를 부른다(테이블이 사라졌으므로 중간 단계
 *    캐시가 남아 있으면 안 된다).
 *  - 리프였다면 페이지 단위 무효화를 gather에 등록한다.
 *
 * 실행 컨텍스트: 해제 경로. 호출자의 락이 직렬화를 보장한다.
 *
 * 호출 체인:
 *   arm_v7s_unmap_pages() / arm_v7s_init_pte() → [__arm_v7s_unmap] (재귀)
 *   → __arm_v7s_set_pte(), io_pgtable_tlb_*(), __arm_v7s_free_table()
 */
static size_t __arm_v7s_unmap(struct arm_v7s_io_pgtable *data,
			      struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t size, int lvl,
			      arm_v7s_iopte *ptep)
{
	/* [한국어] 읽어 둔 엔트리들. 연속 엔트리 최대 개수만큼 잡아
	 * 한 번에 다 담을 수 있게 한다. */
	arm_v7s_iopte pte[ARM_V7S_CONT_PAGES];
	/* [한국어] TLB 콜백 호출에 필요한 프레임워크 핸들. */
	struct io_pgtable *iop = &data->iop;
	/* [한국어] idx는 테이블 인덱스, i는 순회 변수,
	 * num_entries는 이 레벨에서 지울 칸 수(0이면 더 내려가야 한다). */
	int idx, i = 0, num_entries = size >> ARM_V7S_LVL_SHIFT(lvl);

	/* Something went horribly wrong and we ran out of page table */
	/* [한국어] 레벨 2가 마지막인데 그보다 깊이 내려왔다면 자료구조가
	 * 손상된 것이다. 방어적으로 중단한다. */
	if (WARN_ON(lvl > 2))
		return 0;

	/* [한국어] 이 레벨에서 IOVA가 가리키는 인덱스를 구해 포인터를 옮긴다. */
	idx = ARM_V7S_LVL_IDX(iova, lvl, &iop->cfg);
	ptep += idx;
	/* [한국어] 지울 엔트리들을 미리 읽어 둔다. 지운 뒤에도 그 내용
	 * (하위 테이블 주소 등)이 필요하기 때문이다.
	 * num_entries가 0이어도 do-while이라 최소 한 개는 읽는데,
	 * 아래 워크 계속 경로가 pte[0]을 쓰므로 그 동작이 필요하다. */
	do {
		pte[i] = READ_ONCE(ptep[i]);
		/* [한국어] 무효한 엔트리를 해제하려는 것은 상위 계층의 버그다. */
		if (WARN_ON(!ARM_V7S_PTE_IS_VALID(pte[i])))
			return 0;
	} while (++i < num_entries);

	/*
	 * If we've hit a contiguous 'large page' entry at this level, it
	 * needs splitting first, unless we're unmapping the whole lot.
	 *
	 * For splitting, we can't rewrite 16 PTEs atomically, and since we
	 * can't necessarily assume TEX remap we don't have a software bit to
	 * mark live entries being split. In practice (i.e. DMA API code), we
	 * will never be splitting large pages anyway, so just wrap this edge
	 * case in a lock for the sake of correctness and be done with it.
	 */
	/* [한국어] 연속 엔트리를 통째로가 아니라 일부만 해제하려는 경우다
	 * (num_entries <= 1은 "16칸 전체가 아니다"라는 뜻).
	 * 원본 주석이 밝히는 이유로 분할이 불가능하므로 거부한다.
	 * 주석 끝의 "lock으로 감싸겠다"는 말은 과거 구현의 흔적이며,
	 * 현재 코드는 그냥 WARN_ONCE 후 실패시킨다. */
	if (num_entries <= 1 && arm_v7s_pte_is_cont(pte[0], lvl)) {
		WARN_ONCE(true, "Unmap of a partial large IOPTE is not allowed");	/* [한국어] 연속 엔트리는 16칸이 하나이므로 부분 해제가 불가능하다. */
		return 0;	/* [한국어] 아무것도 해제하지 못했음을 0으로 알린다. */
	}

	/* If the size matches this level, we're in the right place */
	/* [한국어] 크기가 이 레벨의 블록에 맞으면 여기서 해제한다. */
	if (num_entries) {
		/* [한국어] 이 레벨의 블록 크기 — 무효화 범위 계산에 쓴다. */
		size_t blk_size = ARM_V7S_BLOCK_SIZE(lvl);

		/* [한국어] 해당 엔트리들을 0으로 지우고 캐시를 동기화한다.
		 * 이 시점부터 하드웨어 워크는 이 IOVA에서 실패한다. */
		__arm_v7s_set_pte(ptep, 0, num_entries, &iop->cfg);

		/* [한국어] 지운 엔트리마다 뒷정리와 TLB 무효화를 수행한다. */
		for (i = 0; i < num_entries; i++) {
			/* [한국어] 지운 것이 하위 테이블이었다면 그 테이블도
			 * 반납해야 한다. */
			if (ARM_V7S_PTE_IS_TABLE(pte[i], lvl)) {
				/* Also flush any partial walks */
				/* [한국어] 테이블이 사라졌으므로 중간 워크 결과가
				 * 캐시에 남아 있으면 안 된다. 하위 레벨 단위까지
				 * 무효화하도록 granule을 lvl+1의 블록 크기로 준다. */
				io_pgtable_tlb_flush_walk(iop, iova, blk_size,
						ARM_V7S_BLOCK_SIZE(lvl + 1));
				/* [한국어] 미리 읽어 둔 값에서 테이블 주소를 복원한다. */
				ptep = iopte_deref(pte[i], lvl, data);
				/* [한국어] 그 테이블 메모리를 반납한다. */
				__arm_v7s_free_table(ptep, lvl + 1, data);
			/* [한국어] 리프였다면 페이지 단위 무효화를 등록한다.
			 * 이미 전체 무효화가 큐에 잡혀 있으면 등록이 낭비이므로 건너뛴다. */
			} else if (!iommu_iotlb_gather_queued(gather)) {
				io_pgtable_tlb_add_page(iop, gather, iova, blk_size);	/* [한국어] 이 페이지를 무효화 대상으로 등록해 나중에 한꺼번에 처리하게 한다. */
			}
			/* [한국어] 다음 블록의 IOVA로 전진한다. */
			iova += blk_size;
		}
		/* [한국어] 요청한 크기를 그대로 해제했다. */
		return size;
	/* [한국어] 레벨 1에 왔는데 크기가 맞지 않고, 그 자리가 테이블도 아니라면
	 * 섹션(또는 슈퍼섹션)의 일부를 해제하려는 것이다 — 위와 같은 이유로 거부한다. */
	} else if (lvl == 1 && !ARM_V7S_PTE_IS_TABLE(pte[0], lvl)) {
		WARN_ONCE(true, "Unmap of a partial large IOPTE is not allowed");	/* [한국어] 섹션(또는 슈퍼섹션)의 일부만 해제하려는 요청이다. */
		return 0;	/* [한국어] 역시 아무것도 해제하지 못했음을 알린다. */
	}

	/* Keep on walkin' */
	/* [한국어] 하위 테이블로 내려가 다시 시도한다. 레벨이 2단계뿐이라
	 * 재귀 깊이는 최대 1이다. */
	ptep = iopte_deref(pte[0], lvl, data);
	return __arm_v7s_unmap(data, gather, iova, size, lvl + 1, ptep);	/* [한국어] 하위 테이블에서 같은 일을 반복한다 — 레벨이 둘뿐이라 여기서 끝난다. */
}

/*
 * [한국어]
 * arm_v7s_unmap_pages - io-pgtable의 unmap_pages 진입점
 *
 * @ops: 연산 테이블.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 해제할 페이지 개수.
 * @gather: TLB 무효화 수집 구조체.
 * @return: 실제로 해제한 바이트 수.
 *
 * map_pages와 대칭으로 한 번에 한 페이지씩 처리한다. 어느 하나가 실패하면
 * (0을 반환하면) 그 지점에서 멈추고 지금까지의 합계를 돌려준다 —
 * 상위 계층이 그 값을 보고 남은 부분을 판단한다.
 *
 * 실행 컨텍스트: 드라이버의 unmap 경로.
 *
 * 호출 체인:
 *   드라이버 → ops->unmap_pages → [arm_v7s_unmap_pages] → __arm_v7s_unmap()
 */
static size_t arm_v7s_unmap_pages(struct io_pgtable_ops *ops, unsigned long iova,
				  size_t pgsize, size_t pgcount,
				  struct iommu_iotlb_gather *gather)
{
	/* [한국어] ops에서 인스턴스를 복원한다. */
	struct arm_v7s_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] 누적 해제량과 한 번의 결과. */
	size_t unmapped = 0, ret;

	/* [한국어] IOVA가 입력 주소 폭을 넘으면 테이블 밖을 가리키게 된다. */
	if (WARN_ON(iova >= (1ULL << data->iop.cfg.ias)))
		return 0;

	/* [한국어] 요청된 페이지 수만큼 하나씩 해제한다. */
	while (pgcount--) {
		/* [한국어] 레벨 1에서 시작해 재귀적으로 내려간다. */
		ret = __arm_v7s_unmap(data, gather, iova, pgsize, 1, data->pgd);
		/* [한국어] 0이면 해제 실패(무효 엔트리나 부분 해제 거부)다.
		 * 더 진행할 수 없으므로 멈춘다. */
		if (!ret)
			break;

		/* [한국어] 성공한 만큼 누적한다. */
		unmapped += pgsize;
		/* [한국어] 다음 페이지로 전진한다. */
		iova += pgsize;
	}

	/* [한국어] 실제로 해제한 총 바이트 수를 반환한다. */
	return unmapped;
}

/*
 * [한국어]
 * arm_v7s_iova_to_phys - 소프트웨어 워크로 IOVA를 물리 주소로 변환한다
 *
 * @ops: 연산 테이블.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 루프 구조가 독특하다. lvl을 0에서 시작해 `++lvl`로 증가시키며 인덱싱하고,
 * 매번 iopte_deref를 호출한다. 그래서 마지막 반복에서는 리프 엔트리를
 * "테이블인 것처럼" 역참조하게 되는데, 그 결과 ptep은 버려지므로 문제가
 * 없다 — 루프 조건이 ARM_V7S_PTE_IS_TABLE(pte, lvl)이라 리프를 만나면
 * 곧바로 빠져나오기 때문이다. 다소 낭비적이지만 코드는 짧아진다.
 *
 * 오프셋 복원: 연속 엔트리라면 정렬이 16배 크므로 마스크도 16배로 넓혀야
 * 페이지 내 오프셋이 올바르게 계산된다.
 *
 * 실행 컨텍스트: 조회 경로. 읽기만 하므로 부작용이 없다.
 *
 * 호출 체인:
 *   드라이버 → ops->iova_to_phys → [arm_v7s_iova_to_phys]
 */
static phys_addr_t arm_v7s_iova_to_phys(struct io_pgtable_ops *ops,
					unsigned long iova)
{
	/* [한국어] ops에서 인스턴스를 복원한다. */
	struct arm_v7s_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] ptep은 현재 테이블(최상위에서 시작), pte는 읽은 엔트리. */
	arm_v7s_iopte *ptep = data->pgd, pte;
	/* [한국어] 레벨. 루프 안에서 ++lvl로 먼저 증가하므로 0에서 시작해
	 * 첫 반복이 레벨 1을 다룬다. */
	int lvl = 0;
	/* [한국어] 페이지 내 오프셋을 계산할 마스크. */
	u32 mask;

	/* [한국어] 테이블 엔트리를 만나는 동안 계속 내려간다. */
	do {
		/* [한국어] 레벨을 하나 올리며 그 레벨의 인덱스를 더한다. */
		ptep += ARM_V7S_LVL_IDX(iova, ++lvl, &data->iop.cfg);
		/* [한국어] 엔트리를 한 번만 읽는다. */
		pte = READ_ONCE(*ptep);
		/* [한국어] 다음 레벨 테이블로 내려간다. 리프를 만난 경우에도
		 * 한 번 역참조하지만, 루프를 빠져나가면서 버려지므로 무해하다. */
		ptep = iopte_deref(pte, lvl, data);
	} while (ARM_V7S_PTE_IS_TABLE(pte, lvl));

	/* [한국어] 리프에 도달했지만 무효한 엔트리라면 매핑이 없다는 뜻이다. */
	if (!ARM_V7S_PTE_IS_VALID(pte))
		return 0;

	/* [한국어] 이 레벨의 기본 정렬 마스크. */
	mask = ARM_V7S_LVL_MASK(lvl);
	/* [한국어] 연속 엔트리라면 실제 매핑 단위가 16배 크므로 마스크도
	 * 그만큼 넓혀야 오프셋이 올바르게 계산된다. */
	if (arm_v7s_pte_is_cont(pte, lvl))
		mask *= ARM_V7S_CONT_PAGES;
	/* [한국어] 엔트리의 물리 주소에 IOVA의 오프셋 부분(~mask)을 얹어
	 * 최종 주소를 만든다. */
	return iopte_to_paddr(pte, lvl, &data->iop.cfg) | (iova & ~mask);
}

/*
 * [한국어]
 * arm_v7s_alloc_pgtable - io-pgtable의 alloc 진입점
 *
 * @cfg: 요청된 설정. 검증한 뒤 arm_v7s_cfg 출력 필드(ttbr/tcr/prrr/nmrr)를
 *       채워 돌려준다. pgsize_bitmap도 이 함수가 좁힌다.
 * @cookie: 드라이버가 넘긴 불투명 포인터(TLB 콜백 때 되돌려 받는다).
 *          이 구현에서는 직접 쓰지 않는다.
 * @return: 새 io_pgtable, 지원 불가능한 설정이거나 메모리 부족이면 NULL.
 *
 * 검증 항목이 이 형식의 제약을 그대로 보여 준다:
 *  - ias/oas 상한: 기본 32비트, MTK 확장이 있으면 각각 34/35비트.
 *  - 인정하는 quirk는 넷뿐이다. 그 밖의 것이 하나라도 켜져 있으면 거부한다.
 *  - MTK_EXT는 NO_PERMS와 함께여야 한다 — 확장 비트가 권한 비트 자리를
 *    빼앗기 때문이다.
 *  - MTK_TTBR_EXT는 MTK_EXT 없이 쓸 수 없다.
 *
 * 슈퍼섹션 정책: pgsize_bitmap을 네 가지 크기로 좁히되, 드라이버가 처음부터
 * SZ_16M을 넣어 두지 않았다면 AND 연산으로 자연히 빠진다. 즉 슈퍼섹션은
 * 드라이버가 명시적으로 요청해야만 쓰인다.
 *
 * TEX remap 설정(PRRR/NMRR)이 이 함수의 핵심 산출물 중 하나다:
 *  - 인덱스 1 = 디바이스 메모리 (prot_to_pte에서 IOMMU_MMIO가 쓰는 조합).
 *  - 인덱스 4, 7 = 일반 메모리.
 *  - 인덱스 7은 이너/아우터 모두 WBWA(write-back write-allocate)로,
 *    IOMMU_CACHE 매핑이 이 조합에 대응한다.
 * 원본 주석이 밝히듯, 이 인덱스들은 TEX remap을 쓰지 않을 때의 해석과
 * 최대한 비슷해지도록 고른 값이다.
 *
 * TTBR 계산: MTK 확장이면 물리 주소의 상위 비트를 하위 비트에 겹쳐 넣는
 * 독특한 방식을 쓰고(하드웨어가 그렇게 해석한다), 아니면 공유성과
 * 캐시 정책 비트를 붙인다. 비코히런트 워크면 캐시 정책을 NC로 둔다.
 *
 * 실행 컨텍스트: 도메인 생성 시. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   드라이버 → alloc_io_pgtable_ops(ARM_V7S, ...) → init_fns->alloc
 *   → [arm_v7s_alloc_pgtable] → kmem_cache_create(), __arm_v7s_alloc_table()
 */
static struct io_pgtable *arm_v7s_alloc_pgtable(struct io_pgtable_cfg *cfg,
						void *cookie)
{
	/* [한국어] 만들어 반환할 인스턴스. */
	struct arm_v7s_io_pgtable *data;
	/* [한국어] 레벨 2 슬랩 캐시에 줄 플래그. */
	slab_flags_t slab_flag;
	/* [한국어] 최상위 테이블의 물리 주소(TTBR 계산에 쓴다). */
	phys_addr_t paddr;

	/* [한국어] 입력 주소 폭 검증. MTK 확장이 있으면 34비트까지,
	 * 없으면 32비트까지 허용한다. 확장된 비트는 전부 레벨 1 테이블이
	 * 커지는 것으로 흡수된다. */
	if (cfg->ias > (arm_v7s_is_mtk_enabled(cfg) ? 34 : ARM_V7S_ADDR_BITS))
		return NULL;

	/* [한국어] 출력 주소 폭 검증. MTK 확장은 PTE의 예약 비트 세 개로
	 * 35비트까지 표현할 수 있다. */
	if (cfg->oas > (arm_v7s_is_mtk_enabled(cfg) ? 35 : ARM_V7S_ADDR_BITS))
		return NULL;

	/* [한국어] 이 구현이 아는 quirk는 넷뿐이다. 그 밖의 비트가 하나라도
	 * 켜져 있으면 요청을 이해할 수 없으므로 거부한다 — 조용히 무시하는
	 * 것보다 실패가 안전하다. */
	if (cfg->quirks & ~(IO_PGTABLE_QUIRK_ARM_NS |
			    IO_PGTABLE_QUIRK_NO_PERMS |
			    IO_PGTABLE_QUIRK_ARM_MTK_EXT |
			    IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT))
		return NULL;

	/* If ARM_MTK_4GB is enabled, the NO_PERMS is also expected. */
	/* [한국어] MTK 물리 주소 확장은 AP2/XN 비트 자리를 주소 비트로 쓰므로,
	 * 권한 비트를 쓰지 않겠다는 선언(NO_PERMS)이 반드시 함께 와야 한다.
	 * 이것이 없으면 권한 비트와 주소 비트가 충돌해 조용히 잘못 동작한다.
	 * 들여쓰기가 한 단계 더 들어가 있는 것은 원본 그대로다. */
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_EXT &&
	    !(cfg->quirks & IO_PGTABLE_QUIRK_NO_PERMS))
			return NULL;

	/* [한국어] TTBR 확장(테이블 자체가 4GB 위에 있어도 되는 모드)은
	 * 물리 주소 확장을 전제한다. 그것 없이는 확장 비트를 해석할 방법이 없다. */
	if ((cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT) &&
	    !arm_v7s_is_mtk_enabled(cfg))
		return NULL;

	/* [한국어] 인스턴스를 할당한다. kzalloc이 아닌 kmalloc인데, 아래에서
	 * 모든 필드를 명시적으로 채우기 때문이다(다만 실패 경로에서
	 * data->l2_tables가 초기화되기 전에 참조될 여지는 없다 —
	 * out_free_data는 캐시 생성 실패 이후에만 도달한다). */
	data = kmalloc_obj(*data);
	if (!data)	/* [한국어] 인스턴스를 할당하지 못했으니 되돌릴 자원도 없다. */
		return NULL;

	/*
	 * ARM_MTK_TTBR_EXT extend the translation table base support larger
	 * memory address.
	 */
	/* [한국어] TTBR 확장이 있으면 레벨 2 테이블도 4GB 위에 있을 수 있으므로
	 * DMA 존 제약을 걸지 않는다. 없으면 SLAB_CACHE_DMA(32)로 낮은 주소에서
	 * 받도록 강제한다. */
	slab_flag = cfg->quirks & IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT ?
		    0 : ARM_V7S_TABLE_SLAB_FLAGS;

	/* [한국어] 레벨 2 테이블 전용 슬랩 캐시를 만든다.
	 * 크기와 정렬을 모두 ARM_V7S_TABLE_SIZE(2)로 주는 것이 핵심이다 —
	 * 1KB 테이블이 1KB 경계에 정렬되어야 PTE의 주소 필드(상위 22비트)로
	 * 표현할 수 있기 때문이다. kmalloc으로는 이 정렬을 보장할 수 없다. */
	data->l2_tables = kmem_cache_create("io-pgtable_armv7s_l2",
					    ARM_V7S_TABLE_SIZE(2, cfg),
					    ARM_V7S_TABLE_SIZE(2, cfg),
					    slab_flag, NULL);
	/* [한국어] 캐시 생성 실패 — 인스턴스를 반납하러 간다. */
	if (!data->l2_tables)
		goto out_free_data;

	/* [한국어] 프레임워크가 호출할 세 콜백을 등록한다. 구조체 통째 대입으로
	 * 나머지 필드를 0으로 만드는 효과도 있다. */
	data->iop.ops = (struct io_pgtable_ops) {
		/* [한국어] 재귀 워크로 매핑을 설치한다. */
		.map_pages	= arm_v7s_map_pages,
		/* [한국어] 재귀 워크로 매핑을 해제하고 빈 테이블을 반납한다. */
		.unmap_pages	= arm_v7s_unmap_pages,
		/* [한국어] 소프트웨어 워크로 물리 주소를 조회한다. */
		.iova_to_phys	= arm_v7s_iova_to_phys,
	};

	/* We have to do this early for __arm_v7s_alloc_table to work... */
	/* [한국어] 설정을 인스턴스에 복사한다. __arm_v7s_alloc_table()이
	 * data->iop.cfg를 읽으므로, pgd를 만들기 전에 반드시 해야 한다.
	 * 이후 cfg에 대한 수정은 호출자의 원본에 이뤄지고, 이 복사본에는
	 * 반영되지 않는다는 점에 주의(pgsize_bitmap 등이 그렇다). */
	data->iop.cfg = *cfg;

	/*
	 * Unless the IOMMU driver indicates supersection support by
	 * having SZ_16M set in the initial bitmap, they won't be used.
	 */
	/* [한국어] 지원 페이지 크기를 이 형식이 표현할 수 있는 넷으로 좁힌다.
	 * AND 연산이므로, 드라이버가 처음부터 SZ_16M을 넣지 않았다면
	 * 슈퍼섹션은 자연히 빠진다 — 즉 슈퍼섹션은 옵트인 방식이다.
	 * 하드웨어에 따라 슈퍼섹션 지원이 다르기 때문에 이렇게 해 두었다. */
	cfg->pgsize_bitmap &= SZ_4K | SZ_64K | SZ_1M | SZ_16M;

	/* TCR: T0SZ=0, EAE=0 (if applicable) */
	/* [한국어] TTBCR을 0으로 둔다. T0SZ=0은 "TTBR0가 전체 주소 공간을
	 * 담당한다"는 뜻이고(TTBR1을 쓰지 않는다), EAE=0은 short-descriptor
	 * 형식을 쓴다는 선언이다(1이면 LPAE가 된다). */
	cfg->arm_v7s_cfg.tcr = 0;

	/*
	 * TEX remap: the indices used map to the closest equivalent types
	 * under the non-TEX-remap interpretation of those attribute bits,
	 * excepting various implementation-defined aspects of shareability.
	 */
	/* [한국어] PRRR — TEX remap에서 각 인덱스의 메모리 타입을 정의한다.
	 * 인덱스 1을 디바이스 메모리로, 4와 7을 일반 메모리로 지정한다.
	 * prot_to_pte()가 만드는 조합(TEX=1과 B/C의 조합)이 이 인덱스들에
	 * 대응하도록 맞춰져 있다.
	 * DS0/DS1: 디바이스 메모리의 공유성 설정.
	 * NS1: S 비트가 선 일반 메모리를 공유로 취급한다.
	 * NOS(7): 인덱스 7을 이너 공유(Not Outer Shareable)로 만든다. */
	cfg->arm_v7s_cfg.prrr = ARM_V7S_PRRR_TR(1, ARM_V7S_PRRR_TYPE_DEVICE) |
				ARM_V7S_PRRR_TR(4, ARM_V7S_PRRR_TYPE_NORMAL) |
				ARM_V7S_PRRR_TR(7, ARM_V7S_PRRR_TYPE_NORMAL) |
				ARM_V7S_PRRR_DS0 | ARM_V7S_PRRR_DS1 |
				ARM_V7S_PRRR_NS1 | ARM_V7S_PRRR_NOS(7);
	/* [한국어] NMRR — 일반 메모리 인덱스의 캐시 정책을 정의한다.
	 * 인덱스 7만 정의하며, 이너/아우터 모두 WBWA(write-back,
	 * write-allocate)로 둔다. IOMMU_CACHE 매핑이 이 조합을 쓴다.
	 * 인덱스 4는 여기서 0(캐시 불가)으로 남아, 캐시 없는 일반 메모리가 된다. */
	cfg->arm_v7s_cfg.nmrr = ARM_V7S_NMRR_IR(7, ARM_V7S_RGN_WBWA) |
				ARM_V7S_NMRR_OR(7, ARM_V7S_RGN_WBWA);

	/* Looking good; allocate a pgd */
	/* [한국어] 최상위 테이블을 만든다. 0으로 초기화되어 모든 엔트리가
	 * 무효 상태로 시작한다. */
	data->pgd = __arm_v7s_alloc_table(1, GFP_KERNEL, data);
	/* [한국어] 실패하면 슬랩 캐시와 인스턴스를 되돌린다. */
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	/* [한국어] 빈 pgd의 초기화가 메모리에 반영된 뒤에야 드라이버가 TTBR을
	 * 기록하도록 순서를 세운다. 이것이 없으면 하드웨어가 초기화되지 않은
	 * 메모리를 최상위 테이블로 걷을 수 있다. */
	wmb();

	/* TTBR */
	/* [한국어] 최상위 테이블의 물리 주소를 구한다. */
	paddr = virt_to_phys(data->pgd);
	/* [한국어] MediaTek 구성에서는 TTBR의 형식이 다르다.
	 * upper_32_bits(paddr)를 하위 비트에 OR 하는 독특한 방식인데,
	 * 하드웨어가 TTBR의 하위 비트 몇 개를 주소의 상위 비트로 해석하도록
	 * 설계되었기 때문이다. 공유성/캐시 비트는 쓰지 않는다. */
	if (arm_v7s_is_mtk_enabled(cfg))
		cfg->arm_v7s_cfg.ttbr = paddr | upper_32_bits(paddr);
	else
		/* [한국어] 표준 ARMv7 TTBR 형식.
		 * S 비트로 테이블이 공유 메모리에 있음을 표시하고,
		 * 워크가 코히런트하면 NOS + 이너/아우터 WBWA로 테이블 읽기를
		 * 캐시하게 하며, 비코히런트면 캐시 정책을 NC로 두어 하드웨어가
		 * 항상 메모리에서 직접 읽게 만든다(그래야 dma_sync가 의미를 갖는다). */
		cfg->arm_v7s_cfg.ttbr = paddr | ARM_V7S_TTBR_S |
					(cfg->coherent_walk ? (ARM_V7S_TTBR_NOS |
					 ARM_V7S_TTBR_IRGN_ATTR(ARM_V7S_RGN_WBWA) |
					 ARM_V7S_TTBR_ORGN_ATTR(ARM_V7S_RGN_WBWA)) :
					(ARM_V7S_TTBR_IRGN_ATTR(ARM_V7S_RGN_NC) |	/* [한국어] 비코히런트 워크에서는 테이블 읽기를 캐시하지 않아야 dma_sync가 의미를 갖는다. */
					 ARM_V7S_TTBR_ORGN_ATTR(ARM_V7S_RGN_NC)));
	/* [한국어] 프레임워크에는 임베드된 io_pgtable 포인터를 돌려준다. */
	return &data->iop;

/* [한국어] 캐시 생성 이후의 실패가 모이는 되감기 지점. */
out_free_data:
	/* [한국어] 슬랩 캐시를 파괴한다. 생성 실패로 온 경우 NULL인데,
	 * kmem_cache_destroy(NULL)은 안전하므로 문제가 없다. */
	kmem_cache_destroy(data->l2_tables);
	/* [한국어] 인스턴스를 반납한다. */
	kfree(data);
	/* [한국어] 프레임워크에 실패를 알린다. */
	return NULL;
}

/* [한국어] io-pgtable 프레임워크에 이 포맷 구현을 노출하는 테이블.
 * io-pgtable.c의 io_pgtable_init_table[]이 ARM_V7S 포맷 자리에 이 심볼을
 * 담아 두고, alloc_io_pgtable_ops(ARM_V7S, ...)가 찾아온다.
 * static이 아닌 이유가 그 외부 참조 때문이다. */
struct io_pgtable_init_fns io_pgtable_arm_v7s_init_fns = {
	/* [한국어] 설정 검증과 테이블/캐시 생성, 레지스터 값 계산까지 담당. */
	.alloc	= arm_v7s_alloc_pgtable,
	/* [한국어] 모든 테이블과 슬랩 캐시를 회수한다. */
	.free	= arm_v7s_free_pgtable,
};

/* [한국어] 아래는 부팅 시 이 형식 구현이 올바른지 스스로 검증하는
 * 셀프테스트다. CONFIG_IOMMU_IO_PGTABLE_ARMV7S_SELFTEST가 켜져 있을 때만
 * 빌드된다 — 실제 하드웨어 없이 페이지 테이블 로직만 시험할 수 있어,
 * 형식 코드를 고칠 때 회귀를 빠르게 잡아내는 장치다. */
#ifdef CONFIG_IOMMU_IO_PGTABLE_ARMV7S_SELFTEST

/* [한국어] 테스트가 만든 설정 구조체를 가리키는 전역 포인터.
 * 설정자: arm_v7s_do_selftests()가 테스트 시작 시 지정.
 * 읽는 자: 더미 TLB 콜백들이 "쿠키가 제대로 전달되는가"를 검증할 때 비교 대상.
 * __initdata인 이유: 부팅 후에는 필요 없어 메모리를 반납한다. */
static struct io_pgtable_cfg *cfg_cookie __initdata;

/*
 * [한국어]
 * dummy_tlb_flush_all - 셀프테스트용 전체 무효화 콜백
 *
 * @cookie: io-pgtable이 되돌려 준 쿠키.
 * @return: 없음.
 *
 * 하드웨어가 없으므로 실제로 무효화할 것이 없다. 대신 **쿠키가 올바르게
 * 전달되는지**를 검증한다 — alloc 때 넘긴 포인터가 콜백에 그대로 돌아와야
 * 하는데, 프레임워크나 구현이 그것을 잘못 다루면 여기서 WARN이 뜬다.
 *
 * 실행 컨텍스트: 셀프테스트 중(부팅 초기, __init).
 *
 * 호출 체인:
 *   __arm_v7s_map()/unmap() → io_pgtable_tlb_flush_all → [dummy_tlb_flush_all]
 */
static void __init dummy_tlb_flush_all(void *cookie)
{
	/* [한국어] 쿠키가 테스트가 넘긴 것과 같은지 확인한다. */
	WARN_ON(cookie != cfg_cookie);
}

/*
 * [한국어]
 * dummy_tlb_flush - 셀프테스트용 범위 무효화 콜백
 *
 * @iova: 무효화 시작 주소(검증에 쓰지 않는다).
 * @size: 무효화 크기 — 이것을 검증한다.
 * @granule: 무효화 단위(쓰지 않는다).
 * @cookie: 쿠키.
 * @return: 없음.
 *
 * 두 가지를 검증한다:
 *  1) 쿠키가 올바르게 전달되는가.
 *  2) size가 지원 페이지 크기 중 하나인가. 구현이 엉뚱한 크기로
 *     무효화를 요청하면(예: 페이지 경계에 맞지 않는 크기) 실제 하드웨어에서
 *     TLB가 제대로 비워지지 않을 것이므로, 여기서 미리 잡아낸다.
 *
 * 실행 컨텍스트: 셀프테스트 중.
 *
 * 호출 체인:
 *   __arm_v7s_unmap() → io_pgtable_tlb_flush_walk → [dummy_tlb_flush]
 */
static void __init dummy_tlb_flush(unsigned long iova, size_t size,
				   size_t granule, void *cookie)
{
	/* [한국어] 쿠키 전달이 올바른지 확인한다. */
	WARN_ON(cookie != cfg_cookie);
	/* [한국어] 무효화 크기가 지원 페이지 크기 집합에 속하는지 확인한다.
	 * 비트 AND로 검사하므로, 여러 크기의 합인 경우도 통과할 수 있는
	 * 느슨한 검사이지만 명백한 오류는 잡아낸다. */
	WARN_ON(!(size & cfg_cookie->pgsize_bitmap));
}

/*
 * [한국어]
 * dummy_tlb_add_page - 셀프테스트용 페이지 단위 무효화 콜백
 *
 * @gather: 무효화 수집 구조체(쓰지 않는다).
 * @iova: 무효화할 페이지 주소.
 * @granule: 페이지 크기.
 * @cookie: 쿠키.
 * @return: 없음.
 *
 * 페이지 하나 무효화를 "크기 = granule인 범위 무효화"로 바꿔 위 함수에
 * 위임한다. 검증 내용이 같으므로 코드를 중복할 이유가 없다.
 *
 * 실행 컨텍스트: 셀프테스트 중.
 *
 * 호출 체인:
 *   __arm_v7s_unmap() → io_pgtable_tlb_add_page → [dummy_tlb_add_page]
 *   → dummy_tlb_flush()
 */
static void __init dummy_tlb_add_page(struct iommu_iotlb_gather *gather,
				      unsigned long iova, size_t granule,
				      void *cookie)
{
	/* [한국어] size와 granule을 같게 넘겨 한 페이지 무효화로 만든다. */
	dummy_tlb_flush(iova, granule, granule, cookie);
}

/* [한국어] 셀프테스트가 io-pgtable에 등록하는 더미 무효화 콜백 묶음.
 * __initconst라 부팅 후 메모리에서 사라진다. */
static const struct iommu_flush_ops dummy_tlb_ops __initconst = {
	/* [한국어] 전체 무효화 — 쿠키만 검증한다. */
	.tlb_flush_all	= dummy_tlb_flush_all,
	/* [한국어] 범위 무효화 — 쿠키와 크기를 검증한다. */
	.tlb_flush_walk	= dummy_tlb_flush,
	/* [한국어] 페이지 무효화 — 위 함수에 위임한다. */
	.tlb_add_page	= dummy_tlb_add_page,
};

/* [한국어] 테스트 실패를 처리하는 매크로.
 * 문장식으로 만들어 값(-EFAULT)을 내면서 동시에 WARN을 띄우고
 * selftest_running 플래그를 내린다 — `return __FAIL(ops);` 한 줄로
 * 세 가지 일을 하게 해 테스트 코드를 짧게 유지한다.
 * 인자 ops를 받지만 쓰지 않는데, 원래 free_io_pgtable_ops(ops)를
 * 부르려던 흔적으로 보인다(그래서 실패 시 페이지 테이블이 누수된다 —
 * 부팅 중 한 번뿐이라 실무상 문제가 되지 않는다). */
#define __FAIL(ops)	({				\
		WARN(1, "selftest: test failed\n");	\
		selftest_running = false;		\
		-EFAULT;				\
})	/* [한국어] 문장식의 마지막 값 -EFAULT가 매크로의 결과가 되어 그대로 반환된다. */

/*
 * [한국어]
 * arm_v7s_do_selftests - 부팅 시 이 형식 구현을 자체 검증한다
 *
 * @return: 0 성공, -EINVAL(할당 실패), -EFAULT(테스트 실패).
 *
 * 왜 필요한가: 페이지 테이블 형식 코드는 하드웨어가 있어야 시험할 수 있다고
 * 여기기 쉽지만, 실제로는 "IOVA를 넣으면 올바른 물리 주소가 나오는가"를
 * 소프트웨어만으로 검증할 수 있다. 이 테스트가 그 일을 하며, 형식 코드를
 * 수정했을 때 회귀를 부팅 즉시 잡아낸다.
 *
 * 시험 항목:
 *  1) 빈 테이블은 어떤 IOVA도 변환하지 않는다(세 지점을 확인).
 *  2) 네 가지 페이지 크기 각각으로 매핑을 만들고, 겹치는 매핑이
 *     -EEXIST로 거부되는지, 그리고 변환 결과가 정확한지 확인한다.
 *     IOVA와 물리 주소를 같게 잡아(iova → iova) 검증을 단순화한다.
 *  3) 해제한 뒤 변환이 실패하는지, 그리고 같은 자리에 다시 매핑할 수
 *     있는지 확인한다.
 * 각 크기마다 IOVA를 16MB씩 띄우는 이유: 가장 큰 매핑(슈퍼섹션)이
 * 16MB라 그보다 좁게 두면 서로 겹치기 때문이다.
 *
 * selftest_running 플래그를 켜는 이유: 2번의 "겹치는 매핑" 시험이
 * 일부러 -EEXIST를 유발하는데, 그때마다 WARN이 뜨면 로그가 지저분해진다.
 *
 * 실행 컨텍스트: subsys_initcall(부팅 중). 실패해도 부팅은 계속된다.
 *
 * 호출 체인:
 *   subsys_initcall → [arm_v7s_do_selftests]
 *   → alloc_io_pgtable_ops(), ops->map_pages/unmap_pages/iova_to_phys()
 */
static int __init arm_v7s_do_selftests(void)
{
	/* [한국어] 테스트할 페이지 테이블의 연산 테이블. */
	struct io_pgtable_ops *ops;
	/* [한국어] 테스트용 설정. 실제 드라이버가 넘길 법한 값들로 채운다. */
	struct io_pgtable_cfg cfg = {
		/* [한국어] 검증만 하는 더미 무효화 콜백들. */
		.tlb = &dummy_tlb_ops,
		/* [한국어] 출력 주소 32비트(기본 구성). */
		.oas = 32,
		/* [한국어] 입력 주소 32비트. */
		.ias = 32,
		/* [한국어] 코히런트 워크로 설정해 DMA 매핑 경로를 건너뛴다 —
		 * 테스트에는 실제 디바이스가 없기 때문이다. */
		.coherent_walk = true,
		/* [한국어] NS quirk를 켜 그 경로도 함께 시험한다. */
		.quirks = IO_PGTABLE_QUIRK_ARM_NS,
		/* [한국어] 네 가지 크기를 모두 시험한다. SZ_16M을 넣었으므로
		 * 슈퍼섹션 경로도 검증 대상이 된다. */
		.pgsize_bitmap = SZ_4K | SZ_64K | SZ_1M | SZ_16M,
	};
	/* [한국어] 현재 시험 중인 IOVA와 크기. */
	unsigned int iova, size;
	/* [한국어] pgsize_bitmap의 비트 순회 인덱스. */
	unsigned int i;
	/* [한국어] map_pages가 채워 주는 매핑 바이트 수(값을 검사하지는 않는다). */
	size_t mapped;

	/* [한국어] 이후 일부러 유발할 -EEXIST에서 WARN이 뜨지 않게 한다. */
	selftest_running = true;

	/* [한국어] 더미 콜백들이 검증할 쿠키를 기록해 둔다. */
	cfg_cookie = &cfg;

	/* [한국어] 이 형식의 페이지 테이블을 만든다. 쿠키로 cfg 자신을 넘겨
	 * 콜백에서 되돌아오는지 확인할 수 있게 한다. */
	ops = alloc_io_pgtable_ops(ARM_V7S, &cfg, &cfg);
	/* [한국어] 생성 실패는 테스트 실패가 아니라 환경 문제로 보고 별도 처리한다. */
	if (!ops) {
		pr_err("selftest: failed to allocate io pgtable ops\n");	/* [한국어] 하드웨어 문제가 아니라 환경 문제이므로 테스트 실패와 구분해 남긴다. */
		return -EINVAL;	/* [한국어] 테이블을 만들지 못했으니 시험을 진행할 수 없다. */
	}

	/*
	 * Initial sanity checks.
	 * Empty page tables shouldn't provide any translations.
	 */
	/* [한국어] 빈 테이블이 낮은 주소를 변환하면 안 된다. 42라는 임의의
	 * 작은 값을 쓴 것은 페이지 경계에 맞지 않는 주소도 다뤄 보려는 것이다. */
	if (ops->iova_to_phys(ops, 42))
		return __FAIL(ops);

	/* [한국어] 1GB 근처(레벨 1 인덱스가 다른 지점)도 확인한다. */
	if (ops->iova_to_phys(ops, SZ_1G + 42))
		return __FAIL(ops);

	/* [한국어] 2GB 근처(최상위 비트가 선 지점)도 확인한다.
	 * 부호 있는 산술 실수로 음수 취급되는 버그를 잡아내는 지점이다. */
	if (ops->iova_to_phys(ops, SZ_2G + 42))
		return __FAIL(ops);

	/*
	 * Distinct mappings of different granule sizes.
	 */
	/* [한국어] 첫 시험 구간을 0에서 시작한다. */
	iova = 0;
	/* [한국어] 지원하는 각 페이지 크기마다 매핑을 만들어 본다.
	 * for_each_set_bit이 비트 위치를 주므로 1 << i가 곧 크기다. */
	for_each_set_bit(i, &cfg.pgsize_bitmap, BITS_PER_LONG) {
		/* [한국어] 이번에 시험할 페이지 크기. */
		size = 1UL << i;
		/* [한국어] IOVA와 물리 주소를 같게 잡아 매핑한다 — 그래야
		 * 변환 결과를 입력과 직접 비교할 수 있다.
		 * 네 가지 보호 플래그를 모두 켜 속성 조립 경로도 시험한다. */
		if (ops->map_pages(ops, iova, iova, size, 1,
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops);

		/* Overlapping mappings */
		/* [한국어] 같은 IOVA에 다시 매핑을 시도한다. 반드시 실패해야
		 * 하므로 조건이 `!` — 성공하면 그것이 버그다.
		 * 이 시험 때문에 selftest_running으로 WARN을 억제한 것이다. */
		if (!ops->map_pages(ops, iova, iova + size, size, 1,
				    IOMMU_READ | IOMMU_NOEXEC, GFP_KERNEL,
				    &mapped))
			return __FAIL(ops);

		/* [한국어] 변환이 정확한지 확인한다. 페이지 안쪽 오프셋 42까지
		 * 정확히 복원되어야 한다 — 마스크 계산이 틀리면 여기서 걸린다. */
		if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
			return __FAIL(ops);

		/* [한국어] 다음 크기의 시험 구간으로 옮긴다. 가장 큰 매핑이
		 * 16MB이므로 그만큼 띄워야 겹치지 않는다. */
		iova += SZ_16M;
	}

	/* Full unmap */
	/* [한국어] 두 번째 라운드: 앞서 만든 매핑들을 해제하고 다시 만든다. */
	iova = 0;
	for_each_set_bit(i, &cfg.pgsize_bitmap, BITS_PER_LONG) {
		/* [한국어] 이번에 다룰 페이지 크기. */
		size = 1UL << i;

		/* [한국어] 만든 크기 그대로 해제한다. 반환값이 요청 크기와
		 * 같아야 정상이다 — 부분 해제 거부 로직이 잘못 걸리면 0이 온다. */
		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops);

		/* [한국어] 해제 후에는 변환이 실패해야 한다. */
		if (ops->iova_to_phys(ops, iova + 42))
			return __FAIL(ops);

		/* Remap full block */
		/* [한국어] 같은 자리에 다시 매핑한다. 해제가 엔트리를 제대로
		 * 지웠다면 -EEXIST 없이 성공해야 한다. 이번에는 쓰기 권한만
		 * 주어 다른 속성 조합도 시험한다. */
		if (ops->map_pages(ops, iova, iova, size, 1, IOMMU_WRITE,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops);

		/* [한국어] 다시 만든 매핑의 변환도 정확한지 확인한다. */
		if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
			return __FAIL(ops);

		/* [한국어] 다음 구간으로 옮긴다. */
		iova += SZ_16M;
	}

	/* [한국어] 테스트가 만든 페이지 테이블을 정리한다. 여기까지 왔다면
	 * 두 번째 라운드의 매핑들이 남아 있는데, free가 그것들도 함께 반납한다. */
	free_io_pgtable_ops(ops);

	/* [한국어] 이후의 정상 동작에서는 다시 WARN이 뜨도록 플래그를 내린다. */
	selftest_running = false;

	/* [한국어] 모든 시험을 통과했음을 부팅 로그에 남긴다. */
	pr_info("self test ok\n");
	return 0;	/* [한국어] 모든 시험을 통과했다. */
}
/* [한국어] subsys_initcall로 등록해 부팅 중 서브시스템 초기화 단계에서
 * 자동 실행되게 한다. 이 시점이면 슬랩 할당자가 준비되어 있고, 아직
 * 실제 IOMMU 드라이버들이 이 형식을 쓰기 전이라 간섭이 없다. */
subsys_initcall(arm_v7s_do_selftests);
/* [한국어] CONFIG_IOMMU_IO_PGTABLE_ARMV7S_SELFTEST 가드의 끝.
 * 셀프테스트가 꺼져 있으면 위 코드가 통째로 빠져 커널 크기가 줄어든다. */
#endif
