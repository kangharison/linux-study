// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART page table allocator.
 *
 * Copyright (C) 2022 The Asahi Linux Contributors
 *
 * Based on io-pgtable-arm.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] Apple DART용 io-pgtable 포맷 구현 (io-pgtable-dart.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Apple Silicon(M1/M2 등)의 IOMMU인 DART(Device Address Resolution
 * Table)가 쓰는 페이지 테이블 포맷을 리눅스의 io-pgtable 프레임워크에 구현한
 * 것이다. 즉 "IOVA 하나를 물리 주소로 바꾸는 다단계 테이블을 만들고, 채우고,
 * 걷고, 지우는" 순수한 자료구조 코드다. 하드웨어 레지스터는 전혀 건드리지
 * 않는다 — TLB 무효화나 TTBR 등록은 apple-dart.c(드라이버)가 담당하고,
 * 이 파일은 그 드라이버가 io_pgtable_ops를 통해 호출하는 백엔드다.
 * DART 포맷은 ARM LPAE와 닮았지만 결정적인 차이가 몇 가지 있다:
 *  - PTE에 접근 권한이 "금지 비트(NO_READ/NO_WRITE)"로 들어간다. 즉 비트가
 *    0일 때 허용이다(ARM과 반대).
 *  - 최상위 레벨이 하드웨어 TTBR 배열이다. DART1(4K 페이지)은 TTBR을 최대
 *    4개까지 쓸 수 있어, 그 자체가 하나의 테이블 레벨처럼 동작한다.
 *  - PTE에 "서브페이지(subpage)" 범위 필드가 있어 한 페이지 안의 일부만
 *    노출할 수 있다. 이 구현은 항상 전체 페이지를 열어 그 기능을 쓰지 않는다.
 *  - DART2는 물리 주소를 4비트 오른쪽 시프트해 저장한다(더 큰 주소 공간을
 *    같은 비트 수에 담기 위한 압축).
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [DMA API / VFIO] iommu_map()/iommu_unmap()
 *        ↓
 *   [apple-dart.c] iommu_ops 구현, TLB 무효화, TTBR 레지스터 기록
 *        ↓ io_pgtable_ops 콜백 (이 파일이 채운 함수 포인터)
 *   [이 파일] dart_map_pages() / dart_unmap_pages() / dart_iova_to_phys()
 *        ↓ 메모리상의 다단계 테이블 갱신
 *   [DART 하드웨어] TTBR에서 시작해 테이블을 걸어 IOVA를 변환
 *
 * 등록 경로: io-pgtable.c의 io_pgtable_init_table[]이 이 파일 끝에 있는
 * io_pgtable_apple_dart_init_fns를 APPLE_DART/APPLE_DART2 포맷 자리에 담고,
 * apple-dart.c가 alloc_io_pgtable_ops(APPLE_DART, &cfg, cookie)를 부르면
 * apple_dart_alloc_pgtable()이 호출된다.
 *
 * 실행 컨텍스트: map/unmap은 프로세스 컨텍스트(때로는 atomic 컨텍스트 —
 * gfp 인자가 그 사정을 반영한다)에서 호출된다. 락은 이 파일에 없다 —
 * 상위 계층(iommu 코어의 도메인 락)이 직렬화를 책임지며, 다만 테이블 설치는
 * cmpxchg로 경쟁에 대비한다.
 *
 * === 타 모듈과의 연결 ===
 * - linux/io-pgtable.h: struct io_pgtable, io_pgtable_cfg, io_pgtable_ops,
 *   io_pgtable_init_fns 등 프레임워크 계약 전체.
 * - iommu-pages.h: iommu_alloc_pages_sz()/iommu_free_pages(). 페이지 테이블
 *   전용 할당자로, NUMA 지역성과 회계(accounting)를 처리해 준다.
 * - apple-dart.c: 유일한 소비자. cfg->apple_dart_cfg.ttbr[]에 이 파일이 채워
 *   준 물리 주소를 하드웨어 TTBR 레지스터에 기록하고, unmap 후 TLB를 비운다.
 * - asm/barrier.h: dma_wmb()/wmb(). 테이블 내용이 PTE보다 먼저 보이도록
 *   순서를 강제하는 데 쓴다 — 이것이 없으면 하드웨어가 쓰레기 테이블을 걷는다.
 * 데이터 흐름: iommu_map(iova, paddr, size) → dart_map_pages() → 필요한 중간
 * 테이블을 할당·설치 → 마지막 레벨에 PTE 기록 → wmb() → 드라이버가 TLB 무효화.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct dart_io_pgtable: 이 페이지 테이블 인스턴스의 전부. 레벨 수,
 *   최상위 테이블 개수(tbl_bits), 레벨당 비트 수, 그리고 TTBR 배열(pgd[]).
 * - dart_map_pages(): 테이블을 걸어 내려가며 없는 중간 레벨을 만들고,
 *   마지막 레벨에 연속된 PTE를 채운다.
 * - dart_unmap_pages(): 마지막 레벨 PTE를 0으로 지우고 TLB gather에 등록한다.
 *   중간 테이블은 회수하지 않는다(재사용을 노린 의도적 선택).
 * - dart_get_last(): IOVA로 마지막 레벨 테이블 포인터를 찾는 공용 워커.
 * - dart_alloc_pgtable(): cfg의 ias/pgsize로부터 레벨 수와 tbl_bits를 역산한다.
 *   이 파일에서 가장 까다로운 계산이 여기 모여 있다.
 * - dart_install_table(): cmpxchg로 중간 테이블을 설치한다 — 동시에 같은
 *   테이블을 만들려는 경쟁에서 하나만 이기게 한다.
 */

/* [한국어] 이 파일의 모든 커널 로그 앞에 "dart io-pgtable: " 접두사를 붙인다.
 * 실제로는 pr_* 호출이 없고 WARN_ON만 쓰지만, io-pgtable 계열 파일들의
 * 공통 관례를 따른다. */
#define pr_fmt(fmt)	"dart io-pgtable: " fmt

/* [한국어] cmpxchg64_relaxed() 등 원자적 연산. 중간 테이블 설치 경쟁을
 * 해결하는 데 필수적이다. */
#include <linux/atomic.h>
/* [한국어] FIELD_PREP() — 마스크로 정의된 비트 필드에 값을 안전하게 끼워
 * 넣는 매크로. 서브페이지 범위 필드를 채우는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] BIT(), GENMASK_ULL(), __ffs(), ilog2() 등 비트 조작 헬퍼. */
#include <linux/bitops.h>
/* [한국어] io-pgtable 프레임워크의 계약 — struct io_pgtable_cfg/ops와
 * 포맷 enum(APPLE_DART, APPLE_DART2). 이 파일의 존재 이유 그 자체다. */
#include <linux/io-pgtable.h>
/* [한국어] WARN_ON(), min_t(), max_t() 등 커널 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] SZ_4K, SZ_16K — 페이지 크기 상수. DART는 이 두 가지만 지원한다. */
#include <linux/sizes.h>
/* [한국어] kzalloc_obj()/kfree() — dart_io_pgtable 구조체 할당용. */
#include <linux/slab.h>
/* [한국어] u64, phys_addr_t 등 기본 타입. dart_iopte가 u64의 별칭이다. */
#include <linux/types.h>

/* [한국어] dma_wmb(), wmb() — 메모리 배리어. 테이블 내용이 그것을 가리키는
 * PTE보다 반드시 먼저 보이도록 순서를 세우는 데 쓴다. */
#include <asm/barrier.h>
/* [한국어] iommu_alloc_pages_sz()/iommu_free_pages() — 페이지 테이블 전용
 * 할당자. 크기를 지정해 정렬된 블록을 받고, IOMMU 페이지 회계에 잡힌다. */
#include "iommu-pages.h"

/* [한국어] 1세대 DART(4KB 페이지)가 다룰 수 있는 주소 비트 수.
 * 현재 코드에서 직접 참조되지는 않지만, APPLE_DART1_PADDR_MASK가
 * GENMASK_ULL(35, 12)로 정확히 이 36비트를 표현한다는 점에서
 * 하드웨어 한계를 문서화하는 상수다. */
#define DART1_MAX_ADDR_BITS	36

/* [한국어] 최상위 테이블(TTBR) 개수를 표현하는 비트 수. 2비트이므로
 * 최대 4개다. 왜 여러 개인가: 구형 4K DART는 IOVA 공간이 한 테이블로
 * 덮이지 않아, 하드웨어가 TTBR 레지스터 4개를 두고 IOVA 상위 2비트로
 * 그중 하나를 고른다. 즉 TTBR 배열 자체가 사실상 한 레벨이다. */
#define DART_MAX_TABLE_BITS	2
/* [한국어] 최상위 테이블의 최대 개수 = 2^2 = 4. pgd[] 배열의 크기다. */
#define DART_MAX_TABLES		BIT(DART_MAX_TABLE_BITS)
/* [한국어] 지원하는 최대 레벨 수. TTBR 레벨을 하나로 세므로 실제 메모리상
 * 테이블은 최대 3단계다. dart_alloc_pgtable()이 이 값을 넘는 구성을 거부한다. */
#define DART_MAX_LEVELS		4 /* Includes TTBR level */

/* Struct accessors */
/* [한국어] 프레임워크가 넘겨주는 struct io_pgtable 포인터에서 이 드라이버의
 * 바깥 구조체를 복원한다. io-pgtable 계열 파일들의 공통 관용구다. */
#define io_pgtable_to_data(x)						\
	container_of((x), struct dart_io_pgtable, iop)

/* [한국어] io_pgtable_ops 포인터에서 곧장 dart_io_pgtable로 가는 지름길.
 * ops → io_pgtable → dart_io_pgtable의 두 단계 복원을 한 번에 처리한다.
 * map/unmap/iova_to_phys 세 콜백이 모두 첫 줄에서 이것을 쓴다. */
#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

/* [한국어] 테이블 하나의 바이트 크기.
 * = PTE 크기(8) × 2^(레벨당 비트 수). 예를 들어 16K 페이지 DART는
 * bits_per_level이 11이므로 8 << 11 = 16KB — 즉 테이블 하나가 정확히
 * 페이지 하나가 된다. 이것이 io-pgtable 설계의 기본 전제다. */
#define DART_GRANULE(d)						\
	(sizeof(dart_iopte) << (d)->bits_per_level)
/* [한국어] 테이블 하나에 들어가는 PTE 개수 = 테이블 크기 / PTE 크기.
 * ilog2(8) = 3만큼 오른쪽 시프트하는 것이 8로 나누는 것과 같다.
 * 마지막 레벨에서 "이 테이블에 몇 개까지 더 채울 수 있는가"를 계산할 때 쓴다. */
#define DART_PTES_PER_TABLE(d)					\
	(DART_GRANULE(d) >> ilog2(sizeof(dart_iopte)))

/* [한국어] 서브페이지 시작 오프셋 필드(비트 52~63). DART는 한 페이지 안에서
 * 노출할 범위를 12비트 단위로 지정할 수 있는데, 이 구현은 그 기능을 쓰지 않고
 * 항상 페이지 전체를 연다. */
#define APPLE_DART_PTE_SUBPAGE_START   GENMASK_ULL(63, 52)
/* [한국어] 서브페이지 끝 오프셋 필드(비트 40~51). start=0, end=0xfff로 두면
 * "페이지 전체 허용"이 된다 — dart_init_pte()가 정확히 그렇게 채운다. */
#define APPLE_DART_PTE_SUBPAGE_END     GENMASK_ULL(51, 40)

/* [한국어] DART1의 물리 주소 필드(비트 12~35). 4KB 정렬이라 하위 12비트가
 * 항상 0이고, 상위는 36비트 물리 주소 한계까지다. 주소를 시프트하지 않고
 * 그 자리에 그대로 둔다. */
#define APPLE_DART1_PADDR_MASK	GENMASK_ULL(35, 12)
/* [한국어] DART2의 물리 주소 필드(비트 10~37). DART1과 달리 주소를 4비트
 * 오른쪽으로 시프트해 저장하므로, 이 28비트 필드로 42비트 물리 주소까지
 * 표현할 수 있다(10+4=14부터 37+4=41). */
#define APPLE_DART2_PADDR_MASK	GENMASK_ULL(37, 10)
/* [한국어] DART2가 물리 주소를 저장할 때 적용하는 시프트량. 16바이트 단위로
 * 압축하는 셈이며, 페이지가 최소 4KB라 하위 4비트는 어차피 0이므로 정보 손실이 없다. */
#define APPLE_DART2_PADDR_SHIFT	(4)

/* Apple DART1 protection bits */
/* [한국어] DART1: 읽기 금지 비트. ARM LPAE와 정반대로 "금지" 의미라,
 * IOMMU_READ가 요청되지 않았을 때 이 비트를 세운다. */
#define APPLE_DART1_PTE_PROT_NO_READ	BIT(8)
/* [한국어] DART1: 쓰기 금지 비트. 역시 IOMMU_WRITE가 없을 때 세운다. */
#define APPLE_DART1_PTE_PROT_NO_WRITE	BIT(7)
/* [한국어] DART1: 서브페이지 기능 비활성화 비트(SP_DIS).
 * DART1에서는 이 비트를 항상 세워 서브페이지 해석을 끈다 — 그래야
 * 페이지 전체가 그대로 매핑된다. */
#define APPLE_DART1_PTE_PROT_SP_DIS	BIT(1)

/* Apple DART2 protection bits */
/* [한국어] DART2: 읽기 금지 비트. 비트 위치가 DART1과 완전히 다르다 —
 * 두 세대의 PTE 레이아웃이 서로 호환되지 않는다는 뜻이다. */
#define APPLE_DART2_PTE_PROT_NO_READ	BIT(3)
/* [한국어] DART2: 쓰기 금지 비트. */
#define APPLE_DART2_PTE_PROT_NO_WRITE	BIT(2)
/* [한국어] DART2: 캐시 금지 비트. IOMMU_CACHE가 요청되지 않으면 세워
 * 이 매핑의 접근이 CPU 캐시와 코히런시를 갖지 않게 한다.
 * DART1에는 대응하는 비트가 없다. */
#define APPLE_DART2_PTE_PROT_NO_CACHE	BIT(1)

/* marks PTE as valid */
/* [한국어] PTE 유효 비트(두 세대 공통, 비트 0). 이 비트가 0이면 하드웨어가
 * 그 엔트리를 "매핑 없음"으로 본다.
 * 다만 이 파일의 코드는 유효성 판단에 이 비트 대신 "PTE 전체가 0인가"를
 * 자주 쓴다 — 유효한 엔트리에는 항상 이 비트가 서 있으므로 결과가 같다. */
#define APPLE_DART_PTE_VALID		BIT(0)

/* IOPTE accessors */
/* [한국어] PTE에 담긴 다음 레벨 테이블의 물리 주소를 커널 가상 주소로 바꾼다.
 * __va()를 쓸 수 있는 이유는 페이지 테이블이 항상 lowmem에서 할당되기 때문이다. */
#define iopte_deref(pte, d) __va(iopte_to_paddr(pte, d))

/* [한국어] DART 페이지 테이블 인스턴스 하나의 전체 상태.
 * alloc 시점에 cfg로부터 계산된 기하 정보와 최상위 테이블 배열만 담는다 —
 * 나머지 상태는 전부 메모리상의 테이블 자체에 있다.
 * 수명: apple_dart_alloc_pgtable()에서 만들어져 apple_dart_free_pgtable()에서
 *       해제된다. 소유자는 apple-dart.c의 도메인이다. */
struct dart_io_pgtable {
	struct io_pgtable	iop;
	/* [한국어] io-pgtable 프레임워크가 보는 부분. cfg(기하/플래그)와
	 * ops(콜백 3종)를 담고 있으며, 이 구조체의 첫 멤버로 임베드되어
	 * container_of로 상호 변환된다.
	 * 설정자: dart_alloc_pgtable()이 iop.ops를 채우고, 프레임워크가 iop.cfg를 채운다.
	 * 읽는 자: 이 파일의 거의 모든 함수가 data->iop.fmt(DART1/DART2 구분)와
	 *          data->iop.cfg.pgsize_bitmap을 읽는다.
	 * 동기화: alloc 이후 읽기 전용이라 락이 없다. */

	int			levels;
	/* [한국어] TTBR 레벨을 포함한 총 레벨 수.
	 * 설정자: dart_alloc_pgtable()이 va_bits로부터 역산해 levels+1로 저장한다.
	 * 읽는 자: dart_get_last()와 dart_map_pages()가 "level = data->levels"로
	 *          시작해 `while (--level > 1)`로 내려간다. 즉 level 1이 마지막
	 *          (리프) 레벨이고, level == levels가 TTBR 선택 단계다.
	 * 값 범위: 2 ~ DART_MAX_LEVELS-1 + 1. 그 이상이면 alloc이 실패한다.
	 * 동기화: alloc 이후 불변. */

	int			tbl_bits;
	/* [한국어] 최상위 테이블(TTBR) 선택에 쓰이는 IOVA 비트 수.
	 * 설정자: dart_alloc_pgtable()이 남은 주소 비트로 계산한다.
	 * 읽는 자: 인덱스 범위 검사(`tbl >= (1 << tbl_bits)`)와
	 *          할당/해제 루프의 상한(`1 << tbl_bits`).
	 * 값 범위: 0(TTBR 하나) ~ DART_MAX_TABLE_BITS(4개, 4K DART만).
	 * 왜 필요한가: 신형 16K DART는 한 개의 TTBR로 충분해 0이 되고,
	 *              구형 4K DART는 IOVA 공간을 덮으려면 최대 4개가 필요하다.
	 * 동기화: alloc 이후 불변. */

	int			bits_per_level;
	/* [한국어] 테이블 한 레벨이 소비하는 IOVA 비트 수 = log2(테이블당 PTE 개수).
	 * 설정자: dart_alloc_pgtable()이 pg_shift - log2(8)로 계산한다.
	 *          4K 페이지면 12-3 = 9, 16K 페이지면 14-3 = 11.
	 * 읽는 자: dart_get_index()의 시프트/마스크 계산과 DART_GRANULE().
	 * 값 범위: 9(4K) 또는 11(16K).
	 * 동기화: alloc 이후 불변. */

	void			*pgd[DART_MAX_TABLES];
	/* [한국어] 최상위 테이블들의 커널 가상 주소 배열(=TTBR에 실릴 것들).
	 * 설정자: apple_dart_alloc_pgtable()이 iommu_alloc_pages_sz()로 채우고,
	 *          그 물리 주소를 cfg->apple_dart_cfg.ttbr[]에도 복사한다.
	 * 읽는 자: dart_get_last()/dart_map_pages()가 워크의 출발점으로 삼고,
	 *          apple_dart_free_pgtable()이 재귀 해제의 뿌리로 쓴다.
	 * 값 범위: 앞쪽 (1 << tbl_bits)개만 유효하고 나머지는 NULL.
	 * 동기화: alloc 시점에 채워지고 free까지 바뀌지 않는다 — 워크 중에는
	 *         읽기만 하므로 락이 필요 없다. */
};

/* [한국어] 페이지 테이블 엔트리 하나의 타입. DART는 두 세대 모두 64비트
 * PTE를 쓴다. u64에 별칭을 준 이유는 코드에서 "이것은 PTE다"라는 의도를
 * 드러내고, sizeof(dart_iopte)로 인덱스 계산을 일관되게 하기 위함이다. */
typedef u64 dart_iopte;


/*
 * [한국어]
 * paddr_to_iopte - 물리 주소를 PTE의 주소 필드 형식으로 인코딩한다
 *
 * @paddr: 매핑할 물리 주소(또는 다음 레벨 테이블의 물리 주소).
 * @data: 페이지 테이블 인스턴스. 세대(DART1/DART2) 판별에 쓴다.
 * @return: PTE의 주소 비트만 채워진 값(권한/유효 비트는 호출자가 OR 한다).
 *
 * 왜 필요한가: 두 세대의 주소 인코딩이 다르기 때문이다. DART1은 주소를
 * 그 자리에 그대로 두고 마스킹만 하지만, DART2는 4비트 오른쪽으로 시프트해
 * 더 넓은 물리 주소를 같은 비트 수에 담는다. 이 차이를 한 함수로 감춰
 * 나머지 코드가 세대를 신경 쓰지 않게 한다.
 *
 * 실행 컨텍스트: map 경로와 테이블 설치 경로에서 호출된다. 순수 계산이라
 * 락도, 실패도 없다.
 *
 * 호출 체인:
 *   dart_init_pte() / dart_install_table() → [paddr_to_iopte]
 */
static dart_iopte paddr_to_iopte(phys_addr_t paddr,
				     struct dart_io_pgtable *data)
{
	/* [한국어] DART2 경로에서만 쓰는 중간 변수. */
	dart_iopte pte;

	/* [한국어] 1세대 DART은 주소를 시프트하지 않는다. 마스크만 씌워
	 * 비트 12~35만 남기고, 하위 12비트(페이지 내 오프셋)와 상위 비트는 버린다. */
	if (data->iop.fmt == APPLE_DART)
		return paddr & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	/* [한국어] 2세대는 주소를 16바이트 단위로 압축한다. 페이지가 최소 4KB라
	 * 하위 4비트는 항상 0이므로 정보 손실 없이 4비트를 아낄 수 있고,
	 * 그만큼 더 큰 물리 주소(42비트)를 표현할 수 있게 된다. */
	pte = paddr >> APPLE_DART2_PADDR_SHIFT;
	/* [한국어] 시프트한 값에서 주소 필드에 해당하는 비트만 남긴다.
	 * 범위를 넘는 주소가 다른 필드를 침범하지 못하게 막는 역할도 한다. */
	pte &= APPLE_DART2_PADDR_MASK;

	/* [한국어] 주소 비트만 담긴 값을 반환한다. 호출자가 여기에 권한 비트와
	 * VALID 비트를 OR 해서 최종 PTE를 만든다. */
	return pte;
}

/*
 * [한국어]
 * iopte_to_paddr - PTE의 주소 필드를 물리 주소로 되돌린다
 *
 * @pte: 해석할 페이지 테이블 엔트리.
 * @data: 세대 판별용 인스턴스 포인터.
 * @return: 페이지 정렬된 물리 주소.
 *
 * 왜 필요한가: paddr_to_iopte()의 역연산이다. 두 용도로 쓰인다 —
 * (1) iova_to_phys()에서 최종 물리 주소를 얻을 때,
 * (2) iopte_deref() 매크로를 통해 다음 레벨 테이블의 위치를 알아낼 때.
 * 후자가 워크의 핵심이라 이 함수는 map/unmap/조회 모든 경로에서 불린다.
 *
 * 실행 컨텍스트: 순수 계산. 락도 실패도 없다.
 *
 * 호출 체인:
 *   iopte_deref() 매크로 / dart_iova_to_phys() → [iopte_to_paddr]
 */
static phys_addr_t iopte_to_paddr(dart_iopte pte,
				  struct dart_io_pgtable *data)
{
	/* [한국어] DART2 경로에서 복원한 주소를 담을 변수. */
	u64 paddr;

	/* [한국어] 1세대는 마스킹만 하면 그대로 물리 주소다. */
	if (data->iop.fmt == APPLE_DART)
		return pte & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	/* [한국어] 2세대는 먼저 주소 필드만 뽑아내고, */
	paddr = pte & APPLE_DART2_PADDR_MASK;
	/* [한국어] 저장 시 적용했던 4비트 시프트를 되돌린다. */
	paddr <<= APPLE_DART2_PADDR_SHIFT;

	/* [한국어] 페이지 정렬된 물리 주소를 반환한다. 페이지 내 오프셋은
	 * 애초에 저장되지 않으므로 호출자가 IOVA에서 따로 더해야 한다. */
	return paddr;
}

/*
 * [한국어]
 * dart_init_pte - 마지막 레벨 테이블에 연속된 리프 PTE들을 채운다
 *
 * @data: 페이지 테이블 인스턴스.
 * @iova: 매핑 시작 IOVA. 현재 구현에서는 실제로 쓰이지 않는다(시그니처만 유지).
 * @paddr: 매핑할 물리 주소의 시작.
 * @prot: dart_prot_to_pte()가 만들어 둔 권한 비트들.
 * @num_entries: 채울 PTE 개수(= 연속 페이지 수).
 * @ptep: 채우기 시작할 PTE의 주소.
 * @return: 0 성공, -EEXIST(이미 유효한 매핑이 있음).
 *
 * 왜 필요한가: IOMMU API 규약상 이미 매핑된 IOVA에 덮어쓰는 것은 오류다 —
 * 먼저 unmap 해야 한다. 그래서 쓰기 전에 대상 구간 전체가 비어 있는지
 * 검사하고, 하나라도 유효하면 아무것도 쓰지 않고 실패한다(전부 아니면 전무).
 *
 * 동작 과정:
 *  1) num_entries개를 훑어 VALID 비트가 선 것이 있는지 확인 → 있으면 -EEXIST.
 *  2) 서브페이지 범위를 [0, 0xfff]로 설정해 페이지 전체를 연다.
 *  3) VALID 비트를 세운다.
 *  4) 각 엔트리에 (권한|유효) | 해당 페이지의 물리 주소를 써 넣는다.
 *
 * 실행 컨텍스트: dart_map_pages()의 마지막 단계. 상위 계층 락이 직렬화를
 * 보장하므로 여기서는 원자적 연산을 쓰지 않고 평범한 대입으로 쓴다.
 * 에러 경로: -EEXIST를 반환하면 호출자가 그대로 iommu_map()에 실패를 올린다.
 *
 * 호출 체인:
 *   dart_map_pages() → [dart_init_pte] → paddr_to_iopte()
 */
static int dart_init_pte(struct dart_io_pgtable *data,
			     unsigned long iova, phys_addr_t paddr,
			     dart_iopte prot, int num_entries,
			     dart_iopte *ptep)
{
	/* [한국어] 두 루프에서 공유하는 인덱스. */
	int i;
	/* [한국어] 모든 엔트리에 공통으로 들어갈 비트들(권한 + 서브페이지 + 유효).
	 * 주소만 엔트리마다 달라진다. */
	dart_iopte pte = prot;
	/* [한국어] 페이지 하나의 크기. pgsize_bitmap이 단일 크기(SZ_4K 또는
	 * SZ_16K)만 담고 있어 그대로 크기로 쓸 수 있다 — 이 드라이버가
	 * 한 번에 한 가지 페이지 크기만 지원하기에 성립하는 단순화다. */
	size_t sz = data->iop.cfg.pgsize_bitmap;

	/* [한국어] 덮어쓰기 방지 검사. 하나라도 유효한 엔트리가 있으면 전체를
	 * 포기한다 — 부분적으로 쓴 뒤 실패하면 되돌리기가 훨씬 어렵기 때문이다. */
	for (i = 0; i < num_entries; i++)
		if (ptep[i] & APPLE_DART_PTE_VALID) {
			/* We require an unmap first */
			/* [한국어] 방금 검사한 조건을 WARN_ON에 다시 넣어 스택
			 * 트레이스를 남긴다 — 상위 계층의 map/unmap 짝이 어긋난
			 * 버그이므로 조용히 실패시키지 않는다. */
			WARN_ON(ptep[i] & APPLE_DART_PTE_VALID);
			return -EEXIST;	/* [한국어] 이미 매핑된 자리라 아무것도 쓰지 않고 실패로 끝낸다 — 먼저 unmap 해야 한다. */
		}

	/* subpage protection: always allow access to the entire page */
	/* [한국어] 서브페이지 시작 오프셋을 0으로 둔다. DART는 페이지 안에서
	 * 노출 구간을 제한할 수 있지만, 리눅스 IOMMU API에는 대응하는 개념이
	 * 없으므로 항상 전체를 연다. */
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_START, 0);
	/* [한국어] 서브페이지 끝을 0xfff(12비트 전부)로 둔다. start=0, end=0xfff가
	 * 곧 "페이지 전체"를 뜻한다. */
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_END, 0xfff);

	/* [한국어] 유효 비트를 세운다. 이 비트가 없으면 하드웨어가 엔트리를 무시한다. */
	pte |= APPLE_DART_PTE_VALID;

	/* [한국어] 연속된 물리 페이지들을 연속된 PTE에 채운다. i번째 엔트리는
	 * paddr + i*sz를 가리키므로, 호출자는 물리적으로 연속된 영역만 넘겨야 한다. */
	for (i = 0; i < num_entries; i++)
		ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);

	/* [한국어] 전부 성공. 메모리 배리어는 호출자(dart_map_pages)가 마지막에
	 * 한 번만 치므로 여기서는 필요 없다. */
	return 0;
}

/*
 * [한국어]
 * dart_install_table - 새로 만든 중간 테이블을 부모 PTE에 원자적으로 설치한다
 *
 * @table: 설치할 다음 레벨 테이블의 가상 주소.
 * @ptep: 그 테이블을 가리키게 될 부모 PTE의 주소.
 * @curr: 설치 전에 기대하는 부모 PTE의 값(항상 0으로 호출된다).
 * @return: cmpxchg 이전의 실제 값. 0이면 내가 설치에 성공한 것이고,
 *          0이 아니면 다른 CPU가 먼저 설치했다는 뜻이다.
 *
 * 왜 필요한가: 두 CPU가 같은 IOVA 영역을 동시에 매핑하면 같은 중간 테이블을
 * 각자 만들어 설치하려 할 수 있다. cmpxchg로 딱 하나만 이기게 하고, 진 쪽은
 * 자기가 만든 테이블을 버린 뒤 이긴 쪽의 테이블을 쓰게 한다.
 *
 * 메모리 순서가 핵심이다: 새 테이블의 내용(전부 0)이 그것을 가리키는 PTE보다
 * 먼저 메모리에 보여야 한다. 그렇지 않으면 하드웨어가 유효한 PTE를 따라가
 * 아직 초기화되지 않은 쓰레기 테이블을 걷게 된다. dma_wmb()가 그 순서를
 * 세운다. 원본 주석이 설명하듯 cmpxchg64_release로 대신할 수도 있어 보이지만,
 * !CONFIG_SMP에서는 release가 아무 순서도 보장하지 않아(DMA는 SMP 여부와
 * 무관하게 존재한다) 명시적 dma_wmb()가 필요하다.
 *
 * 실행 컨텍스트: dart_map_pages()의 워크 도중. 락 없이 동시 실행될 수 있다.
 *
 * 호출 체인:
 *   dart_map_pages() → [dart_install_table] → dma_wmb(), cmpxchg64_relaxed()
 */
static dart_iopte dart_install_table(dart_iopte *table,
					     dart_iopte *ptep,
					     dart_iopte curr,
					     struct dart_io_pgtable *data)
{
	/* [한국어] old는 cmpxchg 이전 값, new는 설치할 값. */
	dart_iopte old, new;

	/* [한국어] 테이블의 물리 주소를 PTE 형식으로 인코딩하고 유효 비트를 세운다.
	 * 중간 레벨 PTE에는 권한 비트가 없다 — 권한은 리프에서만 판정된다. */
	new = paddr_to_iopte(__pa(table), data) | APPLE_DART_PTE_VALID;

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	/* [한국어] DMA 관찰자(=DART 하드웨어) 기준의 쓰기 배리어.
	 * 이 배리어 이전의 쓰기(테이블을 0으로 초기화한 것)가 이후의 쓰기(PTE 설치)보다
	 * 반드시 먼저 보이게 만든다. 이것이 없으면 하드웨어가 유효 PTE를 보고
	 * 초기화되지 않은 메모리를 페이지 테이블로 해석할 수 있다. */
	dma_wmb();

	/* [한국어] 부모 PTE가 여전히 curr(=0)이면 new로 바꾼다. relaxed 변형인
	 * 이유는 필요한 순서를 위 dma_wmb()가 이미 세워 뒀기 때문이다.
	 * 반환값이 곧 "경쟁에서 이겼는가"의 답이 된다. */
	old = cmpxchg64_relaxed(ptep, curr, new);

	/* [한국어] 호출자는 이 값이 0이 아니면 자기 테이블을 해제한다. */
	return old;
}

/*
 * [한국어]
 * dart_get_index - 주어진 레벨에서 IOVA가 가리키는 테이블 인덱스를 뽑는다
 *
 * @data: 페이지 테이블 인스턴스(bits_per_level을 읽는다).
 * @iova: 변환할 I/O 가상 주소.
 * @level: 인덱스를 구할 레벨. 1이 리프, data->levels가 TTBR 선택 단계.
 * @return: 그 레벨 테이블 안에서의 엔트리 인덱스.
 *
 * 왜 이런 계산인가: 다단계 페이지 테이블에서 각 레벨은 IOVA의 서로 다른
 * 비트 구간을 소비한다. 시프트량은
 *   level * bits_per_level + log2(sizeof(PTE))
 * 인데, 마지막 항 log2(8)=3이 붙는 이유가 이 구현의 특징이다 — 페이지 내
 * 오프셋 비트(pg_shift)를 건너뛰는 대신, bits_per_level이 이미
 * pg_shift - 3으로 정의되어 있어 level=1일 때 시프트가 정확히 pg_shift가 된다.
 * 마스크 ((1 << bits_per_level) - 1)로 그 레벨 몫의 비트만 남긴다.
 *
 * 실행 컨텍스트: 워크의 매 단계에서 호출되는 순수 계산 함수.
 *
 * 호출 체인:
 *   dart_get_last() / dart_map_pages() → [dart_get_index]
 */
static int dart_get_index(struct dart_io_pgtable *data, unsigned long iova, int level)
{
	/* [한국어] IOVA를 해당 레벨의 위치까지 내린 뒤, 그 레벨이 소비하는
	 * 비트 수만큼만 남긴다. level이 클수록 상위 비트를 본다. */
	return (iova >> (level * data->bits_per_level + ilog2(sizeof(dart_iopte)))) &
		((1 << data->bits_per_level) - 1);
}

/*
 * [한국어]
 * dart_get_last_index - 마지막(리프) 레벨에서의 인덱스를 뽑는다
 *
 * @data: 페이지 테이블 인스턴스.
 * @iova: 대상 IOVA.
 * @return: 리프 테이블 안에서의 엔트리 인덱스.
 *
 * 왜 별도 함수인가: dart_get_index(data, iova, 1)과 수학적으로 동일하다.
 * 리프 인덱스는 map/unmap/조회 모두에서 반복적으로 쓰이는 데다, 곱셈 없이
 * 단일 시프트로 끝나 의도가 더 분명하게 드러나기 때문에 전용 함수를 두었다.
 *
 * 실행 컨텍스트: 순수 계산.
 *
 * 호출 체인:
 *   dart_map_pages() / dart_unmap_pages() / dart_iova_to_phys() → [dart_get_last_index]
 */
static int dart_get_last_index(struct dart_io_pgtable *data, unsigned long iova)
{

	/* [한국어] level=1을 대입한 형태다. 시프트량 bits_per_level + 3이
	 * 곧 pg_shift + bits_per_level이 되어, 페이지 오프셋과 리프 인덱스
	 * 위쪽 비트를 모두 걷어낸다. */
	return (iova >> (data->bits_per_level + ilog2(sizeof(dart_iopte)))) &
		 ((1 << data->bits_per_level) - 1);
}

/*
 * [한국어]
 * dart_get_last - IOVA에 해당하는 마지막 레벨 테이블의 시작 주소를 찾는다
 *
 * @data: 페이지 테이블 인스턴스.
 * @iova: 찾을 IOVA.
 * @return: 리프 테이블의 시작 포인터, 경로 중간이 비어 있으면 NULL.
 *
 * 왜 필요한가: unmap과 iova_to_phys는 "이미 존재하는" 경로만 따라가면 되므로,
 * 중간 테이블을 만들 필요가 없다. 그 읽기 전용 워크를 한 곳에 모은 것이
 * 이 함수다(map은 테이블을 만들어야 해서 자체 루프를 갖는다).
 *
 * 동작 과정:
 *  1) 최상위 레벨 인덱스로 TTBR 배열(pgd[])에서 시작 테이블을 고른다.
 *  2) 인덱스가 tbl_bits 범위를 벗어나면 매핑될 수 없는 IOVA다 → NULL.
 *  3) level을 하나씩 낮추며 PTE를 읽고, 0이면 매핑 없음 → NULL,
 *     아니면 iopte_deref로 다음 레벨 테이블로 내려간다.
 *  4) level이 1이 되면 루프를 빠져나오고, 그때의 ptep이 리프 테이블이다.
 *
 * 루프 조건 `while (--level > 1)`의 의미: level == levels(TTBR)는 이미
 * 1단계에서 처리했으므로 먼저 감소시키고, level == 1(리프)에 도달하면
 * 더 내려가지 않고 멈춘다.
 *
 * 실행 컨텍스트: unmap/조회 경로. READ_ONCE로 PTE를 읽어 컴파일러가
 * 재읽기하거나 값을 쪼개는 것을 막는다(동시 갱신 가능성 대비).
 *
 * 호출 체인:
 *   dart_unmap_pages() / dart_iova_to_phys() → [dart_get_last]
 *   → dart_get_index(), iopte_deref()
 */
static dart_iopte *dart_get_last(struct dart_io_pgtable *data, unsigned long iova)
{
	/* [한국어] 읽은 PTE 값과 현재 테이블 포인터. */
	dart_iopte pte, *ptep;
	/* [한국어] 최상위 레벨부터 시작한다. */
	int level = data->levels;
	/* [한국어] 최상위 레벨 인덱스 = 어느 TTBR(pgd[]) 을 쓸지. */
	int tbl = dart_get_index(data, iova, level);

	/* [한국어] IOVA가 이 페이지 테이블이 덮는 범위를 벗어났다는 뜻이다.
	 * tbl_bits가 0인 구성에서는 tbl이 0이 아닌 순간 여기서 걸린다. */
	if (tbl >= (1 << data->tbl_bits))
		return NULL;

	/* [한국어] 선택된 최상위 테이블에서 워크를 시작한다. */
	ptep = data->pgd[tbl];
	/* [한국어] 방어적 검사 — alloc에서 채워졌어야 하지만 NULL이면 진행 불가. */
	if (!ptep)
		return NULL;

	/* [한국어] 리프(level 1) 바로 위까지 내려간다. 먼저 감소시키므로
	 * 첫 반복은 levels-1 레벨을 다룬다. */
	while (--level > 1) {
		/* [한국어] 현재 레벨에서 IOVA가 가리키는 엔트리로 포인터를 옮긴다. */
		ptep += dart_get_index(data, iova, level);
		/* [한국어] READ_ONCE로 한 번만 읽는다 — 다른 CPU가 동시에
		 * 테이블을 설치할 수 있으므로 컴파일러의 재읽기를 막아야 한다. */
		pte = READ_ONCE(*ptep);

		/* Valid entry? */
		/* [한국어] 0이면 이 경로에 중간 테이블이 아직 없다 = 매핑 없음.
		 * 읽기 전용 워크이므로 만들지 않고 그냥 실패를 알린다. */
		if (!pte)
			return NULL;

		/* Deref to get next level table */
		/* [한국어] PTE에 담긴 물리 주소를 가상 주소로 바꿔 다음 레벨
		 * 테이블의 시작으로 이동한다. */
		ptep = iopte_deref(pte, data);
	}

	/* [한국어] 리프 테이블의 시작 주소. 호출자가 dart_get_last_index()로
	 * 얻은 인덱스를 더해 실제 엔트리에 접근한다. */
	return ptep;
}

/*
 * [한국어]
 * dart_prot_to_pte - IOMMU API의 보호 플래그를 DART PTE 비트로 변환한다
 *
 * @data: 세대(DART1/DART2) 판별용 인스턴스.
 * @prot: IOMMU_READ / IOMMU_WRITE / IOMMU_CACHE 조합.
 * @return: PTE에 OR 할 권한 비트들.
 *
 * 왜 주의해야 하는가: DART의 권한 비트는 "금지" 의미다. 허용을 표현하려면
 * 비트를 세우는 것이 아니라 **세우지 않는다**. 그래서 조건이 전부
 * `if (!(prot & IOMMU_X))` 형태로 뒤집혀 있다. ARM LPAE에서 넘어온 독자가
 * 가장 혼동하기 쉬운 지점이다.
 *
 * 세대별 차이:
 *  - DART1: SP_DIS를 항상 세워 서브페이지 해석을 끈다. 캐시 제어 비트가 없다.
 *  - DART2: 캐시 금지 비트(NO_CACHE)가 추가되고, 비트 위치가 전부 다르다.
 *
 * 실행 컨텍스트: dart_map_pages()에서 리프를 채우기 직전에 한 번 호출된다.
 *
 * 호출 체인:
 *   dart_map_pages() → [dart_prot_to_pte]
 */
static dart_iopte dart_prot_to_pte(struct dart_io_pgtable *data,
					   int prot)
{
	/* [한국어] 아무 비트도 세우지 않은 상태 = 모든 접근 허용이 출발점이다. */
	dart_iopte pte = 0;

	/* [한국어] 1세대 DART의 권한 인코딩. */
	if (data->iop.fmt == APPLE_DART) {
		/* [한국어] 서브페이지 기능을 끈다. 이 비트를 세워야 PTE의
		 * SUBPAGE_START/END 필드가 무시되고 페이지 전체가 매핑된다. */
		pte |= APPLE_DART1_PTE_PROT_SP_DIS;
		/* [한국어] 쓰기가 요청되지 않았으면 "쓰기 금지" 비트를 세운다.
		 * 조건이 부정형인 이유가 바로 금지 의미의 비트이기 때문이다. */
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART1_PTE_PROT_NO_WRITE;
		/* [한국어] 읽기가 요청되지 않았으면 "읽기 금지" 비트를 세운다. */
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART1_PTE_PROT_NO_READ;
	}
	/* [한국어] 2세대 DART의 권한 인코딩. else가 아닌 별도 if인 것은
	 * 원본 그대로이며, fmt는 둘 중 하나라 실질적으로 배타적이다. */
	if (data->iop.fmt == APPLE_DART2) {
		/* [한국어] 쓰기 미요청 → 쓰기 금지(비트 위치가 DART1과 다름). */
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART2_PTE_PROT_NO_WRITE;
		/* [한국어] 읽기 미요청 → 읽기 금지. */
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART2_PTE_PROT_NO_READ;
		/* [한국어] IOMMU_CACHE가 없으면 이 매핑을 비캐시로 표시한다.
		 * 즉 디바이스 접근이 CPU 캐시와 코히런시를 갖지 않게 된다.
		 * DART1에는 이 제어가 없어 항상 코히런트로 동작한다. */
		if (!(prot & IOMMU_CACHE))
			pte |= APPLE_DART2_PTE_PROT_NO_CACHE;
	}

	/* [한국어] 완성된 권한 비트들. 호출자가 주소/유효 비트와 OR 한다. */
	return pte;
}

/*
 * [한국어]
 * dart_map_pages - IOVA 구간에 물리 페이지들을 매핑한다 (핵심 진입점)
 *
 * @ops: io_pgtable_ops. 여기서 dart_io_pgtable을 복원한다.
 * @iova: 매핑 시작 I/O 가상 주소.
 * @paddr: 매핑할 물리 주소 시작(연속이어야 한다).
 * @pgsize: 페이지 크기. 반드시 cfg->pgsize_bitmap과 같아야 한다.
 * @pgcount: 매핑할 페이지 개수.
 * @iommu_prot: IOMMU_READ/WRITE/CACHE 조합.
 * @gfp: 중간 테이블 할당에 쓸 GFP 플래그(atomic 컨텍스트면 GFP_ATOMIC).
 * @mapped: 출력 인자 — 실제로 매핑된 바이트 수가 누적된다.
 * @return: 0 성공, -EINVAL/-ERANGE(인자 오류), -ENOMEM(테이블 할당 실패),
 *          -EEXIST(이미 매핑됨).
 *
 * 왜 한 번에 전부 매핑하지 않는가: 이 함수는 **리프 테이블 하나 안에서만**
 * 채운다. 요청 구간이 테이블 경계를 넘으면 그 앞까지만 처리하고 *mapped에
 * 실제 처리량을 더한 뒤 성공을 반환한다 — 상위 계층(iommu_map)이 남은
 * 부분으로 다시 호출하는 구조다. max_entries 계산이 그 경계 처리다.
 *
 * 동작 과정:
 *  1) 인자 검증: 페이지 크기 일치, 물리 주소가 출력 주소 공간(oas) 안,
 *     읽기/쓰기 중 최소 하나는 요청되었는지.
 *  2) 최상위 인덱스로 TTBR을 고르고, 범위를 벗어나면 -ENOMEM.
 *  3) 리프 바로 위까지 내려가며, 중간 테이블이 없으면 새로 할당해
 *     cmpxchg로 설치한다(경쟁에서 지면 자기 것을 버린다).
 *  4) 권한 비트를 만들고, 리프 인덱스와 이번에 채울 개수를 계산한다.
 *  5) dart_init_pte()로 PTE들을 채우고 *mapped를 갱신한다.
 *  6) wmb()로 모든 PTE 쓰기를 확정한다 — 이후 하드웨어가 이 IOVA로
 *     워크를 시작해도 완전한 테이블을 보게 된다.
 *
 * 실행 컨텍스트: 프로세스 또는 atomic 컨텍스트(gfp가 알려 준다).
 * 상위 계층이 도메인 락으로 직렬화하지만, 테이블 설치만은 cmpxchg로
 * 자체 방어한다.
 *
 * 호출 체인:
 *   iommu_map() → apple-dart.c의 map → ops->map_pages → [dart_map_pages]
 *   → dart_get_index(), iommu_alloc_pages_sz(), dart_install_table(),
 *     dart_prot_to_pte(), dart_init_pte()
 */
static int dart_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] ops 포인터에서 이 인스턴스의 상태를 복원한다. */
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] 기하 정보(pgsize_bitmap, oas)를 담은 설정 구조체. */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] 새로 만들 중간 테이블의 크기 — 곧 페이지 하나 크기다. */
	size_t tblsz = DART_GRANULE(data);
	/* [한국어] ret은 결과, tbl은 TTBR 인덱스, num_entries는 이번에 채울 개수,
	 * max_entries는 리프 테이블에 남은 자리, map_idx_start는 리프 시작 인덱스. */
	int ret = 0, tbl, num_entries, max_entries, map_idx_start;
	/* [한국어] pte는 읽은 값, cptep는 새로 만든 자식 테이블, ptep는 현재 위치. */
	dart_iopte pte, *cptep, *ptep;
	/* [한국어] 리프에 쓸 권한 비트. */
	dart_iopte prot;
	/* [한국어] 워크를 최상위 레벨에서 시작한다. */
	int level = data->levels;

	/* [한국어] 이 구현은 단일 페이지 크기만 지원한다. 상위 계층이 다른
	 * 크기를 요청했다면 호출 규약이 깨진 것이므로 WARN과 함께 거부한다. */
	if (WARN_ON(pgsize != cfg->pgsize_bitmap))
		return -EINVAL;

	/* [한국어] 물리 주소가 출력 주소 공간(oas 비트)을 넘으면 PTE에 담을 수
	 * 없다. 시프트 결과가 0이 아니면 초과라는 뜻이다. */
	if (WARN_ON(paddr >> cfg->oas))
		return -ERANGE;

	/* [한국어] 읽기도 쓰기도 요청하지 않은 매핑은 의미가 없다(모든 접근이
	 * 금지된 유효 엔트리가 된다). 인자 오류로 거부한다. */
	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	/* [한국어] 최상위 인덱스 = 어느 TTBR을 쓸지. */
	tbl = dart_get_index(data, iova, level);

	/* [한국어] IOVA가 이 페이지 테이블의 범위를 벗어났다. 확장할 방법이
	 * 없으므로 -ENOMEM으로 알린다(주소 공간 고갈에 가까운 의미). */
	if (tbl >= (1 << data->tbl_bits))
		return -ENOMEM;

	/* [한국어] 해당 TTBR의 테이블에서 워크를 시작한다. */
	ptep = data->pgd[tbl];
	/* [한국어] 리프 바로 위까지 내려간다. dart_get_last()와 같은 구조지만,
	 * 여기서는 없는 테이블을 만들어 가며 진행한다. */
	while (--level > 1) {
		/* [한국어] 이 레벨에서 IOVA가 가리키는 엔트리로 이동. */
		ptep += dart_get_index(data, iova, level);
		/* [한국어] 동시 갱신 가능성에 대비해 한 번만 읽는다. */
		pte = READ_ONCE(*ptep);

		/* no table present */
		/* [한국어] 이 경로에 아직 중간 테이블이 없으면 만들어야 한다. */
		if (!pte) {
			/* [한국어] 테이블 크기만큼 정렬된 0 초기화 메모리를 받는다.
			 * gfp를 그대로 넘기므로 atomic 컨텍스트에서도 안전하다. */
			cptep = iommu_alloc_pages_sz(gfp, tblsz);
			/* [한국어] 할당 실패 — 여기까지 만든 것은 그대로 두고
			 * 실패를 반환한다. 남은 중간 테이블은 다음 매핑에서 재사용된다. */
			if (!cptep)
				return -ENOMEM;

			/* [한국어] 부모 PTE가 여전히 0일 때만 설치된다. 반환값이
			 * 0이 아니면 다른 CPU가 먼저 설치했다는 뜻이다. */
			pte = dart_install_table(cptep, ptep, 0, data);
			/* [한국어] 경쟁에서 졌다면 내가 만든 테이블은 버린다.
			 * 이긴 쪽의 테이블을 아래에서 쓰게 된다. */
			if (pte)
				iommu_free_pages(cptep);

			/* L2 table is present (now) */
			/* [한국어] 내가 설치했든 남이 설치했든, 이제 부모 PTE에는
			 * 유효한 테이블 주소가 들어 있다. 다시 읽어 그것을 쓴다. */
			pte = READ_ONCE(*ptep);
		}

		/* [한국어] 다음 레벨 테이블로 내려간다. */
		ptep = iopte_deref(pte, data);
	}

	/* install a leaf entries into L2 table */
	/* [한국어] 요청된 보호 플래그를 DART의 금지 비트로 변환한다. */
	prot = dart_prot_to_pte(data, iommu_prot);
	/* [한국어] 리프 테이블 안에서 쓰기를 시작할 인덱스. */
	map_idx_start = dart_get_last_index(data, iova);
	/* [한국어] 이 리프 테이블에 남은 자리 수. 테이블 경계를 넘어 쓰면
	 * 이웃 메모리를 망가뜨리므로 반드시 여기서 제한해야 한다. */
	max_entries = DART_PTES_PER_TABLE(data) - map_idx_start;
	/* [한국어] 요청량과 남은 자리 중 작은 쪽만 처리한다. 나머지는 상위
	 * 계층이 다음 호출로 이어서 매핑한다. */
	num_entries = min_t(int, pgcount, max_entries);
	/* [한국어] 쓰기 시작 위치로 포인터를 옮긴다. */
	ptep += map_idx_start;
	/* [한국어] 실제 PTE 채우기. 이미 매핑된 자리가 있으면 -EEXIST가 온다. */
	ret = dart_init_pte(data, iova, paddr, prot, num_entries, ptep);
	/* [한국어] 성공했고 호출자가 집계를 원하면 처리한 바이트 수를 더해 준다.
	 * 이 값으로 상위 계층이 "얼마나 더 남았는지"를 판단한다. */
	if (!ret && mapped)
		*mapped += num_entries * pgsize;

	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	/* [한국어] 전역 쓰기 배리어. 이 시점 이후 드라이버가 TLB를 만지거나
	 * 디바이스가 DMA를 시작하면 완성된 PTE들을 보게 된다. 배리어가 없으면
	 * 일부만 반영된 상태로 워크가 일어날 수 있다. */
	wmb();

	/* [한국어] dart_init_pte()의 결과를 그대로 전달한다. */
	return ret;
}

/*
 * [한국어]
 * dart_unmap_pages - IOVA 구간의 매핑을 제거한다
 *
 * @ops: io_pgtable_ops.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기(반드시 pgsize_bitmap과 일치).
 * @pgcount: 해제할 페이지 개수.
 * @gather: TLB 무효화를 모아 두는 구조체. 드라이버가 나중에 한꺼번에 처리한다.
 * @return: 실제로 해제한 바이트 수. 0이면 아무것도 해제하지 못했다는 뜻이다.
 *
 * 왜 반환값이 크기인가: map과 마찬가지로 리프 테이블 하나 안에서만 처리하므로,
 * 상위 계층이 "얼마나 처리됐는지" 알아야 남은 구간으로 다시 호출할 수 있다.
 *
 * 중간 테이블을 회수하지 않는 점에 주목: 리프 PTE를 모두 지워 테이블이 비어도
 * 그 테이블 자체는 남겨 둔다. 같은 영역이 다시 매핑될 가능성이 높고, 회수하려면
 * 상위 레벨을 되짚어 올라가며 참조 수를 세야 해서 비용과 복잡도가 크기 때문이다.
 *
 * 동작 과정:
 *  1) 인자 검증(페이지 크기, pgcount 0 아님).
 *  2) dart_get_last()로 리프 테이블을 찾는다. 없으면 매핑되지 않은 IOVA다.
 *  3) 리프 인덱스와 이번에 처리할 개수를 계산(테이블 경계 제한).
 *  4) 각 PTE를 0으로 지우고, TLB gather에 해당 페이지를 등록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 상위 계층 락이 직렬화를 보장하므로
 * 평범한 대입으로 PTE를 지운다.
 *
 * 호출 체인:
 *   iommu_unmap() → apple-dart.c의 unmap → ops->unmap_pages
 *   → [dart_unmap_pages] → dart_get_last(), io_pgtable_tlb_add_page()
 */
static size_t dart_unmap_pages(struct io_pgtable_ops *ops, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	/* [한국어] ops에서 인스턴스 상태를 복원한다. */
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] 페이지 크기 검증에 쓸 설정 구조체. */
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	/* [한국어] i는 처리한 개수(반환값 계산에 쓰인다), 나머지는 경계 계산용. */
	int i = 0, num_entries, max_entries, unmap_idx_start;
	/* [한국어] 읽은 PTE 값과 현재 엔트리 포인터. */
	dart_iopte pte, *ptep;

	/* [한국어] 페이지 크기가 다르거나 개수가 0이면 호출 규약 위반이다.
	 * unmap은 오류 코드를 반환할 수 없으므로 0(아무것도 못 함)으로 알린다. */
	if (WARN_ON(pgsize != cfg->pgsize_bitmap || !pgcount))
		return 0;

	/* [한국어] 이 IOVA의 리프 테이블을 찾는다. 읽기 전용 워크라 없는
	 * 중간 테이블을 만들지 않는다. */
	ptep = dart_get_last(data, iova);

	/* Valid L2 IOPTE pointer? */
	/* [한국어] 리프에 도달하지 못했다 = 매핑된 적이 없는 IOVA를 해제하려 한
	 * 것이다. 상위 계층의 버그일 가능성이 커서 WARN을 남긴다. */
	if (WARN_ON(!ptep))
		return 0;

	/* [한국어] 리프 테이블 안에서 지우기 시작할 인덱스. */
	unmap_idx_start = dart_get_last_index(data, iova);
	/* [한국어] 시작 위치로 포인터를 옮긴다. */
	ptep += unmap_idx_start;

	/* [한국어] 이 테이블에 남은 엔트리 수 — 경계를 넘지 않게 하는 상한이다. */
	max_entries = DART_PTES_PER_TABLE(data) - unmap_idx_start;
	/* [한국어] 요청량과 남은 자리 중 작은 쪽만 이번에 처리한다. */
	num_entries = min_t(int, pgcount, max_entries);

	/* [한국어] 계산된 개수만큼 한 엔트리씩 지운다. */
	while (i < num_entries) {
		/* [한국어] 현재 엔트리를 한 번만 읽는다. */
		pte = READ_ONCE(*ptep);
		/* [한국어] 이미 비어 있는 엔트리를 만나면 map/unmap 짝이 어긋난
		 * 것이므로 경고하고 그 지점에서 멈춘다(지금까지 처리한 만큼만 반환). */
		if (WARN_ON(!pte))
			break;

		/* clear pte */
		/* [한국어] 엔트리를 0으로 지운다. 이 순간부터 하드웨어 워크는
		 * 이 IOVA에서 실패하지만, TLB에 남은 캐시는 아래에서 별도로 처리해야 한다. */
		*ptep = 0;

		/* [한국어] TLB 무효화가 이미 큐에 잡혀 통째로 비울 예정이라면
		 * 페이지 단위 등록은 낭비다. 그렇지 않을 때만 gather에 추가해
		 * 드라이버가 나중에 최소 범위만 무효화하도록 한다. */
		if (!iommu_iotlb_gather_queued(gather))
			io_pgtable_tlb_add_page(&data->iop, gather,
						iova + i * pgsize, pgsize);

		/* [한국어] 다음 엔트리로 이동. */
		ptep++;
		/* [한국어] 처리 개수를 늘린다 — 루프 종료 조건이자 반환값의 재료다. */
		i++;
	}

	/* [한국어] 실제로 지운 바이트 수. num_entries보다 작을 수 있다
	 * (위 WARN 경로로 중간에 멈춘 경우). */
	return i * pgsize;
}

/*
 * [한국어]
 * dart_iova_to_phys - IOVA를 물리 주소로 변환한다(소프트웨어 워크)
 *
 * @ops: io_pgtable_ops.
 * @iova: 변환할 I/O 가상 주소.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 왜 필요한가: 디버깅과 일부 API(iommu_iova_to_phys)가 "이 IOVA가 지금 어디로
 * 매핑되어 있는가"를 물을 때, 하드웨어에 묻지 않고 소프트웨어가 같은 테이블을
 * 직접 걸어 답한다.
 *
 * 동작 과정:
 *  1) dart_get_last()로 리프 테이블을 찾는다. 없으면 0.
 *  2) 리프 인덱스를 더해 엔트리를 읽는다.
 *  3) 유효하면 PTE의 물리 주소에 페이지 내 오프셋을 더해 반환한다.
 *
 * 페이지 내 오프셋 복원이 핵심이다: PTE에는 페이지 정렬 주소만 있으므로,
 * IOVA의 하위 비트(pgsize_bitmap - 1로 마스킹)를 다시 더해야 정확한 주소가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 읽기만 하므로 부작용이 없다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → apple-dart.c → ops->iova_to_phys
 *   → [dart_iova_to_phys] → dart_get_last(), iopte_to_paddr()
 */
static phys_addr_t dart_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	/* [한국어] ops에서 인스턴스 상태를 복원한다. */
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	/* [한국어] 읽은 PTE와 엔트리 포인터. */
	dart_iopte pte, *ptep;

	/* [한국어] 리프 테이블을 찾는다. */
	ptep = dart_get_last(data, iova);

	/* Valid L2 IOPTE pointer? */
	/* [한국어] 경로 중간이 비어 있으면 매핑이 없다는 뜻이다. unmap과 달리
	 * 정상적인 질의일 수 있으므로 WARN 없이 조용히 0을 반환한다. */
	if (!ptep)
		return 0;

	/* [한국어] 리프 테이블 안의 정확한 엔트리로 이동한다. */
	ptep += dart_get_last_index(data, iova);

	/* [한국어] 엔트리를 한 번만 읽는다. */
	pte = READ_ONCE(*ptep);
	/* Found translation */
	/* [한국어] 0이 아니면 유효한 매핑이 있다는 뜻이다. */
	if (pte) {
		/* [한국어] IOVA에서 페이지 내 오프셋만 남긴다. pgsize_bitmap이
		 * 단일 페이지 크기라 (크기 - 1)이 곧 오프셋 마스크가 된다. */
		iova &= (data->iop.cfg.pgsize_bitmap - 1);
		/* [한국어] 페이지 정렬 물리 주소에 오프셋을 얹어 최종 주소를 만든다. */
		return iopte_to_paddr(pte, data) | iova;
	}

	/* Ran out of page tables to walk */
	/* [한국어] 리프까지는 도달했지만 엔트리가 비어 있다 = 매핑 없음. */
	return 0;
}

/*
 * [한국어]
 * dart_alloc_pgtable - cfg로부터 테이블 기하를 역산하고 인스턴스를 만든다
 *
 * @cfg: io-pgtable 설정. ias(입력 주소 비트)와 pgsize_bitmap을 읽고,
 *       나머지 필드는 호출자가 채운다.
 * @return: 초기화된 dart_io_pgtable, 지원 불가능한 구성이거나 메모리 부족이면 NULL.
 *
 * 왜 이 계산이 필요한가: DART는 페이지 크기와 IOVA 폭에 따라 테이블 레벨 수가
 * 달라진다. 이 함수가 그 관계를 풀어낸다.
 *
 * 계산 순서:
 *  1) max_tbl_bits: 4K 페이지면 TTBR을 최대 4개(2비트) 쓸 수 있고,
 *     16K 페이지면 1개(0비트)뿐이다. 하드웨어 세대 차이다.
 *  2) pg_shift = log2(페이지 크기). 4K면 12, 16K면 14.
 *  3) bits_per_level = pg_shift - 3. 테이블 하나가 페이지 하나이고
 *     PTE가 8바이트이므로, 테이블당 PTE 개수는 2^(pg_shift-3)이다.
 *  4) va_bits = ias - pg_shift. 페이지 오프셋을 뺀, 테이블들이 나눠 가질 비트 수.
 *  5) levels: va_bits를 bits_per_level로 나눠 올림하되, TTBR이 흡수할 수 있는
 *     max_tbl_bits를 먼저 빼고 계산한다. 최소 2레벨은 보장한다.
 *  6) tbl_bits: 테이블들이 다 덮지 못한 나머지 비트 = TTBR이 흡수할 비트 수.
 *     이것이 max_tbl_bits를 넘으면 하드웨어로 표현할 수 없는 구성이라 실패.
 *
 * 실행 컨텍스트: 도메인 생성 시 한 번. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   apple_dart_alloc_pgtable() → [dart_alloc_pgtable] → kzalloc_obj()
 */
static struct dart_io_pgtable *
dart_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
	/* [한국어] 만들어 반환할 인스턴스. */
	struct dart_io_pgtable *data;
	/* [한국어] 위 설명의 각 중간 계산값을 담을 변수들. */
	int levels, max_tbl_bits, tbl_bits, bits_per_level, va_bits, pg_shift;

	/*
	 * Old 4K page DARTs can use up to 4 top-level tables.
	 * Newer ones only ever use a maximum of 1.
	 */
	/* [한국어] 4K 페이지를 쓰는 구형 DART만 TTBR을 여러 개 쓴다. 테이블당
	 * PTE가 512개뿐이라 레벨을 늘려도 IOVA 공간을 다 덮지 못해, 하드웨어가
	 * TTBR 4개로 상위 2비트를 흡수하는 구조를 택했기 때문이다.
	 * 16K 페이지 신형은 테이블당 2048개라 한 개면 충분하다. */
	if (cfg->pgsize_bitmap == SZ_4K)
		max_tbl_bits = DART_MAX_TABLE_BITS;
	else
		max_tbl_bits = 0;

	/* [한국어] 페이지 크기의 로그. pgsize_bitmap에 비트가 하나뿐이라
	 * __ffs()로 그 위치를 얻으면 곧 log2가 된다. */
	pg_shift = __ffs(cfg->pgsize_bitmap);
	/* [한국어] 테이블 하나가 소비하는 IOVA 비트 수.
	 * 테이블 크기 = 페이지 크기이고 PTE가 8바이트이므로,
	 * 엔트리 개수는 2^(pg_shift - 3)이다. */
	bits_per_level = pg_shift - ilog2(sizeof(dart_iopte));

	/* [한국어] 페이지 오프셋을 제외하고 테이블 워크가 다뤄야 할 비트 수. */
	va_bits = cfg->ias - pg_shift;

	/* [한국어] 필요한 레벨 수를 올림 나눗셈으로 구한다.
	 * (va_bits - max_tbl_bits)를 bits_per_level로 나눠 올리되,
	 * TTBR이 흡수할 몫을 먼저 빼는 것이 핵심이다. 최소 2레벨을 강제하는
	 * 이유는 이 구현의 워크 루프가 리프 위에 최소 한 레벨을 전제하기 때문이다. */
	levels = max_t(int, 2, (va_bits - max_tbl_bits + bits_per_level - 1) / bits_per_level);

	/* [한국어] TTBR 레벨을 더하면 DART_MAX_LEVELS를 넘는 구성은 하드웨어가
	 * 지원하지 않는다. 지원 불가로 NULL을 반환하면 상위가 다른 포맷을 찾는다. */
	if (levels > (DART_MAX_LEVELS - 1))
		return NULL;

	/* [한국어] 테이블 레벨들이 덮고 남은 상위 비트 수 = TTBR이 흡수할 비트.
	 * 음수가 되지 않도록 max_t로 0에서 자른다(테이블이 IOVA를 다 덮는 경우). */
	tbl_bits = max_t(int, 0, va_bits - (bits_per_level * levels));

	/* [한국어] TTBR로 흡수해야 할 비트가 하드웨어가 제공하는 TTBR 개수를
	 * 넘으면 이 구성은 표현할 수 없다. */
	if (tbl_bits > max_tbl_bits)
		return NULL;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. pgd[]가 NULL로 시작해야
	 * 아래 오류 경로와 free 경로의 조건이 올바르게 동작한다. */
	data = kzalloc_obj(*data);
	/* [한국어] 메모리 부족 — 상위가 NULL로 실패를 알아챈다. */
	if (!data)
		return NULL;

	/* [한국어] TTBR 선택 단계를 한 레벨로 세어 저장한다. 그래서 워크 루프가
	 * level = data->levels에서 시작해 --level > 1로 내려갈 수 있다. */
	data->levels = levels + 1; /* Table level counts as one level */
	/* [한국어] 최상위 테이블 개수를 결정하는 비트 수. */
	data->tbl_bits = tbl_bits;
	/* [한국어] 인덱스 계산과 테이블 크기 산출의 기준값. */
	data->bits_per_level = bits_per_level;

	/* [한국어] 프레임워크가 호출할 세 콜백을 등록한다. 구조체 통째 대입으로
	 * 나머지 필드(있다면)를 0으로 만드는 효과도 있다.
	 * 이 시점 이후 alloc_io_pgtable_ops()의 호출자가 ops를 통해 이 파일의
	 * 함수들을 부르게 된다. */
	data->iop.ops = (struct io_pgtable_ops) {
		/* [한국어] 매핑 생성 — 중간 테이블을 만들어 가며 리프를 채운다. */
		.map_pages	= dart_map_pages,
		/* [한국어] 매핑 제거 — 리프만 지우고 중간 테이블은 남긴다. */
		.unmap_pages	= dart_unmap_pages,
		/* [한국어] 소프트웨어 워크로 IOVA→물리 주소 조회. */
		.iova_to_phys	= dart_iova_to_phys,
	};

	/* [한국어] 최상위 테이블은 아직 할당되지 않은 상태로 반환한다 —
	 * 그 일은 호출자인 apple_dart_alloc_pgtable()이 맡는다. */
	return data;
}

/*
 * [한국어]
 * apple_dart_alloc_pgtable - io-pgtable 프레임워크의 .alloc 진입점
 *
 * @cfg: 요청된 설정. 검증 후 apple_dart_cfg 출력 필드를 채워 돌려준다.
 * @cookie: 드라이버가 넘긴 불투명 포인터(TLB 콜백 때 되돌려 받는다).
 *          이 구현에서는 직접 쓰지 않는다.
 * @return: 새 io_pgtable, 지원 불가능한 설정이거나 메모리 부족이면 NULL.
 *
 * 왜 필요한가: 드라이버가 alloc_io_pgtable_ops(APPLE_DART, &cfg, cookie)를
 * 부르면 프레임워크가 이 함수를 호출한다. 여기서 하드웨어가 감당할 수 있는
 * 설정인지 검증하고, 최상위 테이블들을 실제로 할당해 그 물리 주소를
 * cfg->apple_dart_cfg.ttbr[]에 채워 준다 — 드라이버는 그 값을 TTBR
 * 레지스터에 그대로 기록한다.
 *
 * 검증 항목:
 *  - coherent_walk 필수: DART는 테이블 워크가 캐시 코히런트다. 비코히런트
 *    구성은 캐시 유지 코드가 없어 지원할 수 없다.
 *  - oas는 36(DART1) 또는 42(DART2)만 허용.
 *  - ias는 oas를 넘을 수 없다.
 *  - 페이지 크기는 4K 또는 16K만.
 *
 * 실행 컨텍스트: 도메인 생성 시, 프로세스 컨텍스트(GFP_KERNEL).
 * 에러 경로: 테이블 할당이 중간에 실패하면 out_free_data로 가서 이미 할당한
 * 테이블들을 역순으로 해제하고 인스턴스도 반납한다.
 *
 * 호출 체인:
 *   apple-dart.c → alloc_io_pgtable_ops() → init_fns->alloc
 *   → [apple_dart_alloc_pgtable] → dart_alloc_pgtable(), iommu_alloc_pages_sz()
 */
static struct io_pgtable *
apple_dart_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	/* [한국어] 만들어 반환할 인스턴스. */
	struct dart_io_pgtable *data;
	/* [한국어] TTBR 할당 루프 인덱스. 오류 경로의 되감기에도 쓰인다. */
	int i;

	/* [한국어] DART의 테이블 워크는 캐시 코히런트하다는 전제로 짜여 있다.
	 * 비코히런트 요청이 오면 필요한 캐시 유지 코드가 없어 거부한다. */
	if (!cfg->coherent_walk)
		return NULL;

	/* [한국어] 출력 주소 폭은 세대별로 고정이다 — 36비트(DART1) 또는
	 * 42비트(DART2). 그 밖의 값은 PTE 인코딩과 맞지 않는다. */
	if (cfg->oas != 36 && cfg->oas != 42)
		return NULL;

	/* [한국어] 입력(IOVA) 주소 폭이 출력(물리) 폭보다 클 수 없다 —
	 * 매핑할 수 없는 IOVA 영역이 생기기 때문이다. */
	if (cfg->ias > cfg->oas)
		return NULL;

	/* [한국어] 지원 페이지 크기는 4K와 16K 두 가지뿐이다. 다른 값은
	 * bits_per_level 계산과 하드웨어 기대가 어긋난다. */
	if (!(cfg->pgsize_bitmap == SZ_4K || cfg->pgsize_bitmap == SZ_16K))
		return NULL;

	/* [한국어] 검증을 통과했으니 기하를 역산하고 인스턴스를 만든다.
	 * 이 안에서도 레벨 수가 한계를 넘으면 NULL이 올 수 있다. */
	data = dart_alloc_pgtable(cfg);
	if (!data)	/* [한국어] 인스턴스 할당 실패 — 이 시점에는 되돌릴 자원이 없다. */
		return NULL;

	/* [한국어] 드라이버에게 TTBR을 몇 개 프로그래밍해야 하는지 알린다.
	 * tbl_bits가 0이면 1개, 2면 4개다. */
	cfg->apple_dart_cfg.n_ttbrs = 1 << data->tbl_bits;
	/* [한국어] 하드웨어에 설정할 레벨 수도 함께 알린다 — DART는 워크
	 * 깊이를 레지스터로 지정해야 한다. */
	cfg->apple_dart_cfg.n_levels = data->levels;

	/* [한국어] 필요한 개수만큼 최상위 테이블을 할당한다. */
	for (i = 0; i < cfg->apple_dart_cfg.n_ttbrs; ++i) {
		/* [한국어] 테이블 크기에 맞춰 정렬된 0 초기화 메모리를 받는다.
		 * 0으로 시작해야 모든 엔트리가 "매핑 없음" 상태가 된다. */
		data->pgd[i] =
			iommu_alloc_pages_sz(GFP_KERNEL, DART_GRANULE(data));
		/* [한국어] 하나라도 실패하면 이미 할당한 것들을 되감아야 한다. */
		if (!data->pgd[i])
			goto out_free_data;
		/* [한국어] 물리 주소를 cfg에 실어 드라이버에게 넘긴다. 드라이버는
		 * 이 값을 DART의 TTBR 레지스터에 기록해 하드웨어가 워크를
		 * 시작할 지점을 알려 준다. */
		cfg->apple_dart_cfg.ttbr[i] = virt_to_phys(data->pgd[i]);
	}

	/* [한국어] 프레임워크에는 임베드된 io_pgtable 포인터를 돌려준다.
	 * 나중에 container_of로 이 인스턴스가 복원된다. */
	return &data->iop;

/* [한국어] 테이블 할당이 중간에 실패했을 때의 되감기 지점. */
out_free_data:
	/* [한국어] 실패한 i번째는 할당되지 않았으므로, --i부터 0까지
	 * 이미 성공한 것들만 역순으로 해제한다. */
	while (--i >= 0) {
		iommu_free_pages(data->pgd[i]);	/* [한국어] 이미 확보한 최상위 테이블을 역순으로 반납한다. */
	}
	/* [한국어] 인스턴스 자체도 반납한다. */
	kfree(data);
	/* [한국어] 프레임워크에 실패를 알린다 — 상위가 도메인 생성을 포기한다. */
	return NULL;
}

/*
 * [한국어]
 * apple_dart_free_pgtables - 한 최상위 테이블 아래를 재귀적으로 해제한다
 *
 * @data: 페이지 테이블 인스턴스(테이블 크기 계산에 필요).
 * @ptep: 해제할 테이블의 시작 주소.
 * @level: 이 테이블의 레벨. 1이면 리프라 자식이 없다.
 * @return: 없음.
 *
 * 왜 재귀인가: 트리를 후위 순회(post-order)해야 한다 — 자식을 먼저 모두
 * 해제한 뒤 자기 자신을 해제해야 이미 해제된 메모리를 참조하지 않는다.
 * 깊이가 최대 3레벨로 고정되어 있어 스택 사용량 걱정이 없다.
 *
 * 동작 과정:
 *  1) level > 1이면(=중간 테이블이면) 모든 엔트리를 훑는다.
 *  2) 유효한 엔트리마다 그것이 가리키는 자식 테이블로 재귀한다.
 *  3) 자식 처리가 끝나면 자기 테이블을 해제한다.
 *
 * 실행 컨텍스트: 도메인 해제 시, 프로세스 컨텍스트. 이 시점에는 이미
 * 하드웨어가 이 테이블을 쓰지 않는 것이 보장되어 있다(드라이버가 TTBR을
 * 먼저 지운다).
 *
 * 호출 체인:
 *   apple_dart_free_pgtable() → [apple_dart_free_pgtables] (재귀)
 *   → iommu_free_pages()
 */
static void apple_dart_free_pgtables(struct dart_io_pgtable *data, dart_iopte *ptep, int level)
{
	/* [한국어] 순회 종료 지점(테이블의 끝 바로 다음). */
	dart_iopte *end;
	/* [한국어] 마지막에 해제할 테이블의 시작 주소. ptep이 순회로 움직이므로
	 * 시작 주소를 따로 붙들어 둬야 한다. */
	dart_iopte *start = ptep;

	/* [한국어] 리프(level 1)에는 자식 테이블이 없으므로 순회를 건너뛴다.
	 * 리프 PTE는 물리 페이지를 가리킬 뿐이고, 그 페이지는 이 코드의
	 * 소유가 아니라 해제 대상이 아니다. */
	if (level > 1) {
		/* [한국어] 테이블 크기만큼 더해 끝 주소를 구한다. void*로 캐스팅해
		 * 바이트 단위 덧셈이 되게 한다. */
		end = (void *)ptep + DART_GRANULE(data);

		/* [한국어] 테이블의 모든 엔트리를 훑는다. */
		while (ptep != end) {
			/* [한국어] 엔트리를 읽고 포인터를 다음으로 전진시킨다. */
			dart_iopte pte = *ptep++;

			/* [한국어] 유효한 엔트리면 자식 테이블로 내려가 먼저 해제한다.
			 * level-1을 넘겨 재귀 깊이를 줄인다. */
			if (pte)
				apple_dart_free_pgtables(data, iopte_deref(pte, data), level - 1);
		}
	}
	/* [한국어] 자식을 모두 정리한 뒤(또는 리프라면 곧바로) 이 테이블을 반납한다.
	 * start를 쓰는 이유는 위 루프가 ptep을 끝까지 밀어 놓았기 때문이다. */
	iommu_free_pages(start);
}

/*
 * [한국어]
 * apple_dart_free_pgtable - io-pgtable 프레임워크의 .free 진입점
 *
 * @iop: 해제할 페이지 테이블(프레임워크가 보는 형태).
 * @return: 없음.
 *
 * 왜 필요한가: 도메인이 사라질 때 모든 테이블 메모리를 회수해야 한다.
 * 최상위 테이블마다 재귀 해제를 걸고, 마지막에 인스턴스 구조체를 반납한다.
 *
 * 루프 조건에 주목: `i < (1 << tbl_bits) && data->pgd[i]`로 NULL 검사가
 * 함께 있다. alloc이 도중에 실패한 경우는 out_free_data가 처리하므로
 * 여기 오는 인스턴스는 정상이지만, 방어적으로 NULL에서 멈추게 해 두었다.
 *
 * 실행 컨텍스트: 도메인 해제 시, 프로세스 컨텍스트. 이 호출 전에 드라이버가
 * 하드웨어에서 TTBR을 제거하고 TLB를 비운 상태여야 한다 — 그렇지 않으면
 * 하드웨어가 해제된 메모리를 걷게 된다.
 *
 * 호출 체인:
 *   free_io_pgtable_ops() → init_fns->free → [apple_dart_free_pgtable]
 *   → apple_dart_free_pgtables(), kfree()
 */
static void apple_dart_free_pgtable(struct io_pgtable *iop)
{
	/* [한국어] 프레임워크 포인터에서 이 드라이버의 인스턴스를 복원한다. */
	struct dart_io_pgtable *data = io_pgtable_to_data(iop);
	/* [한국어] 최상위 테이블 순회 인덱스. */
	int i;

	/* [한국어] 할당된 모든 최상위 테이블에 대해 그 아래 트리를 통째로 해제한다.
	 * levels-1을 넘기는 이유: data->levels는 TTBR 레벨을 포함한 값이므로,
	 * 실제 메모리 테이블의 최상위 레벨은 그보다 하나 아래다. */
	for (i = 0; i < (1 << data->tbl_bits) && data->pgd[i]; ++i)
		apple_dart_free_pgtables(data, data->pgd[i], data->levels - 1);

	/* [한국어] 모든 테이블이 해제됐으니 인스턴스 구조체도 반납한다. */
	kfree(data);
}

/* [한국어] io-pgtable 프레임워크에 이 포맷 구현을 노출하는 테이블.
 * drivers/iommu/io-pgtable.c의 io_pgtable_init_table[]이 APPLE_DART와
 * APPLE_DART2 두 포맷 자리에 이 심볼을 담아 두고, alloc_io_pgtable_ops()가
 * 포맷 번호로 찾아온다. 한 구현이 두 포맷을 모두 담당하며, 세대별 차이는
 * 런타임에 data->iop.fmt를 보고 갈라진다.
 * static이 아닌 이유가 바로 그 외부 참조 때문이다. */
struct io_pgtable_init_fns io_pgtable_apple_dart_init_fns = {
	/* [한국어] 설정을 검증하고 최상위 테이블까지 할당하는 생성 진입점. */
	.alloc	= apple_dart_alloc_pgtable,
	/* [한국어] 모든 테이블을 재귀적으로 회수하는 해제 진입점. */
	.free	= apple_dart_free_pgtable,
};
