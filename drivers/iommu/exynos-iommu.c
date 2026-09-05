// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2011,2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 */

/*
 * [한국어 설명] Samsung Exynos SYSMMU IOMMU 드라이버 (exynos-iommu.c)
 *
 * === 파일의 역할 ===
 * Exynos SoC의 SYSMMU 블록을 리눅스 IOMMU API에 붙인다. 카메라,
 * 코덱, 디스플레이 같은 멀티미디어 블록이 흩어진 물리 메모리를
 * 연속된 주소로 보게 하는 것이 목적이며, 구조는 ARM 짧은 서술자
 * 형식과 닮은 2단계 테이블이다 — 1MB 섹션, 64KB 큰 페이지,
 * 4KB 작은 페이지를 지원한다.
 *
 * 이 드라이버를 읽을 때 핵심이 되는 것은 다섯이다.
 *
 * (1) **하나의 마스터가 여러 SYSMMU를 갖는다.** 디바이스 트리에
 *     여러 컨트롤러가 걸릴 수 있고, 그 전부가 같은 페이지 테이블을
 *     가리켜야 한다. owner->controllers 목록이 그것이며,
 *     attach/detach/무효화가 모두 이 목록을 훑는다.
 *
 * (2) **v3.3의 FLPD 캐시 버그 우회가 코드 곳곳에 스며 있다.**
 *     이 하드웨어는 성능을 위해 1단계 엔트리(FLPD)를 미리 읽어
 *     캐시하는데, "매핑 없음" 상태까지 캐시해 버린다. 나중에 그
 *     자리에 진짜 매핑을 넣어도 캐시된 폴트가 그대로 적중해
 *     페이지 폴트가 난다. 그래서 이 드라이버는 빈 1단계 엔트리를
 *     0이 아니라 **zero_lv2_table을 가리키는 링크**로 채우고,
 *     그 링크를 진짜 테이블로 바꿀 때마다 FLPD 캐시를 무효화한다.
 *     ZERO_LV2LINK, lv1ent_zero(), sysmmu_tlb_invalidate_flpdcache()가
 *     모두 이 하나의 버그에서 나온다.
 *
 * (3) **세대 차이를 전역 변수로 흡수한다.** v5부터 36비트 물리 주소를
 *     지원하는데, 그 방법이 "엔트리 값을 4비트 왼쪽으로 민다"이다.
 *     PG_ENT_SHIFT가 그 값이고, 권한 비트의 위치도 세대마다 달라
 *     LV1_PROT/LV2_PROT 표를 전역으로 바꿔 끼운다. 시스템의 모든
 *     SYSMMU가 같은 세대라는 전제 위에 선 설계다.
 *
 * (4) **런타임 PM이 마스터에 종속된다.** SYSMMU는 자기 마스터가
 *     깨어날 때 함께 깨어난다(device link). 그래서 이 드라이버에는
 *     pm_runtime_get/put 호출이 거의 없고, 대신 suspend/resume
 *     콜백이 도메인 정보를 보고 하드웨어를 되살린다.
 *
 * (5) **마스터 클럭은 아주 짧게만 켠다.** 레지스터를 만지는 동안만
 *     clk_master를 켜고 곧바로 끈다. 실제 DMA 중에 클럭을 유지하는
 *     것은 클라이언트 드라이버의 몫이라는 분업이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 트리] iommus = <&sysmmu_a &sysmmu_b>
 *        ↓ of_xlate
 *   [이 파일] owner를 만들고 컨트롤러 목록에 쌓는다
 *        ↓ probe_device
 *   [이 파일] 각 SYSMMU와 마스터를 device_link로 묶는다
 *        ↓ attach_dev
 *   [이 파일] 모든 컨트롤러에 같은 페이지 테이블 주소를 심는다
 *
 *   [마스터 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [이 파일] exynos_iommu_map() → 1단계/2단계 엔트리 기록 + 캐시 플러시
 *        ↓
 *   [하드웨어] SYSMMU가 DMA로 테이블을 읽어 변환
 *        ↑ 폴트
 *   [이 파일] exynos_sysmmu_irq() → 테이블을 되짚어 찍고 상위에 보고
 *
 * 실행 컨텍스트: map/unmap은 pgtablelock(irqsave) 아래에서 돌고
 * atomic 할당을 쓴다. 인터럽트 핸들러는 data->lock을 잡는다.
 * attach/detach는 rpm_lock(뮤텍스) 아래라 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu-pages.h: 1단계 테이블(16KB)과 카운터 배열(8KB) 할당.
 * - linux/dma-mapping.h: **테이블을 DMA 매핑해 캐시를 관리한다.**
 *   이 하드웨어는 테이블을 캐시 일관성 없이 읽으므로, 엔트리를
 *   고칠 때마다 dma_sync가 필요하다(exynos_iommu_set_pte).
 * - dma-iommu.h: 예약 영역 처리를 그대로 코어 함수에 위임한다.
 * - linux/pm_runtime.h: 마스터와의 전원 링크.
 * 데이터 흐름: IOVA 상위 12비트 → 1단계 인덱스, 다음 8비트 →
 * 2단계 인덱스, 나머지 12비트 → 페이지 오프셋.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct exynos_iommu_domain: 도메인 하나. 1단계 테이블과,
 *   섹션마다 남은 2단계 엔트리 수를 세는 배열(lv2entcnt).
 * - struct sysmmu_drvdata: SYSMMU 컨트롤러 하나. 레지스터, 클럭 넷,
 *   그리고 세대별 레지스터 배치(variant).
 * - struct exynos_iommu_owner: 마스터 하나. 그에 딸린 컨트롤러들의
 *   목록과 현재 도메인.
 * - alloc_lv2entry(): 2단계 테이블을 확보한다. FLPD 캐시 우회가
 *   여기 들어 있다.
 * - exynos_iommu_map()/unmap(): 크기에 따라 섹션/큰 페이지/작은
 *   페이지 중 하나로 처리한다.
 * - lv2entcnt: 이 섹션의 2단계 테이블이 완전히 비었는지 알려 주는
 *   카운터. 1MB 섹션 매핑으로 바꿔도 되는지 판단하는 근거다.
 */

/* [한국어] 디버그 빌드에서 dev_dbg 출력을 켠다.
 * DEBUG 매크로가 정의되어야 dev_dbg가 실제로 찍히기 때문이다. */
#ifdef CONFIG_EXYNOS_IOMMU_DEBUG
#define DEBUG
#endif

/* [한국어] SYSMMU마다 최대 4개의 클럭이 있어 그것을 제어한다. */
#include <linux/clk.h>
/* [한국어] 페이지 테이블 자체를 DMA 매핑해 캐시 일관성을 관리한다. */
#include <linux/dma-mapping.h>
/* [한국어] ERR_PTR/IS_ERR. */
#include <linux/err.h>
/* [한국어] readl/writel — SYSMMU 레지스터 접근. */
#include <linux/io.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] 폴트 인터럽트 등록. */
#include <linux/interrupt.h>
/* [한국어] kmemleak_ignore — 슬랩에서 받은 2단계 테이블을 누수
 * 검사 대상에서 빼기 위해 필요하다(참조가 물리 주소로만 남기 때문). */
#include <linux/kmemleak.h>
/* [한국어] 컨트롤러 목록과 클라이언트 목록. */
#include <linux/list.h>
/* [한국어] 디바이스 트리 파싱. */
#include <linux/of.h>
/* [한국어] of_find_device_by_node — of_xlate가 SYSMMU를 찾을 때. */
#include <linux/of_platform.h>
/* [한국어] 플랫폼 드라이버 모델. */
#include <linux/platform_device.h>
/* [한국어] 런타임 PM. 마스터와의 전원 링크가 이 드라이버의 축이다. */
#include <linux/pm_runtime.h>
/* [한국어] kmem_cache — 2단계 테이블 전용 캐시. */
#include <linux/slab.h>

/* [한국어] iommu_dma_get_resv_regions를 ops에 그대로 꽂아 쓴다. */
#include "dma-iommu.h"
/* [한국어] iommu_alloc_pages_sz — 정렬이 보장되는 테이블 페이지 할당. */
#include "iommu-pages.h"

/* [한국어] 이 IOMMU가 다루는 IOVA의 타입. 32비트 고정이다 —
 * v5에서 물리 주소가 36비트로 넓어져도 가상 주소는 32비트 그대로다. */
typedef u32 sysmmu_iova_t;
/* [한국어] 페이지 테이블 엔트리의 타입. 1단계와 2단계가 같은 크기다. */
typedef u32 sysmmu_pte_t;
/* [한국어] "어느 도메인에도 붙어 있지 않음"을 뜻하는 정적 도메인의
 * 전방 선언. 이 하드웨어에 통과 모드는 없고, 이 도메인에 붙는다는
 * 것은 SYSMMU를 꺼 버리는 일이다. */
static struct iommu_domain exynos_identity_domain;

/* We do not consider super section mapping (16MB) */
/* [한국어] 1MB 섹션 매핑의 지수. 1단계 엔트리 하나가 덮는 크기다. */
#define SECT_ORDER 20
/* [한국어] 64KB 큰 페이지의 지수. 2단계 엔트리 16개가 같은 값을
 * 반복해 표현한다. */
#define LPAGE_ORDER 16
/* [한국어] 4KB 작은 페이지의 지수. 가장 기본 단위다. */
#define SPAGE_ORDER 12

/* [한국어] 1MB — 섹션 매핑의 크기. */
#define SECT_SIZE (1 << SECT_ORDER)
/* [한국어] 64KB — 큰 페이지의 크기. */
#define LPAGE_SIZE (1 << LPAGE_ORDER)
/* [한국어] 4KB — 작은 페이지의 크기. */
#define SPAGE_SIZE (1 << SPAGE_ORDER)

/* [한국어] 섹션 경계로 주소를 자르는 마스크. */
#define SECT_MASK (~(SECT_SIZE - 1))
/* [한국어] 큰 페이지 경계로 주소를 자르는 마스크. */
#define LPAGE_MASK (~(LPAGE_SIZE - 1))
/* [한국어] 작은 페이지 경계로 주소를 자르는 마스크. */
#define SPAGE_MASK (~(SPAGE_SIZE - 1))

/* [한국어] 1단계 엔트리가 "매핑 없음"인지 판별한다.
 * 세 가지 경우를 모두 폴트로 본다: (1) zero_lv2_table을 가리키는
 * 우회용 링크, (2) 타입 비트가 0(진짜 무효), (3) 타입 비트가 3
 * (이 하드웨어에서 쓰지 않는 값).
 * (1)이 이 드라이버의 특징이다 — 빈 자리를 0으로 두지 않고
 * 더미 테이블을 가리키게 해 FLPD 캐시 버그를 피한다. */
#define lv1ent_fault(sent) ((*(sent) == ZERO_LV2LINK) || \
			   ((*(sent) & 3) == 0) || ((*(sent) & 3) == 3))
/* [한국어] 이 엔트리가 정확히 더미 링크인지 본다.
 * 진짜 테이블로 교체할 때 FLPD 캐시를 비워야 하는지의 판단 기준이다. */
#define lv1ent_zero(sent) (*(sent) == ZERO_LV2LINK)
/* [한국어] 타입 비트만 보고 "2단계 테이블 링크"인지 본다.
 * 더미인지 진짜인지는 가리지 않는다 — 섹션 매핑으로 덮어쓸 때
 * FLPD 캐시를 비울지 정하는 데 쓴다. */
#define lv1ent_page_zero(sent) ((*(sent) & 3) == 1)
/* [한국어] 진짜 2단계 테이블을 가리키는 엔트리인지 본다.
 * 더미 링크는 제외한다 — 그 아래에는 유효한 매핑이 있을 수 없다. */
#define lv1ent_page(sent) ((*(sent) != ZERO_LV2LINK) && \
			  ((*(sent) & 3) == 1))
/* [한국어] 1MB 섹션을 직접 매핑한 엔트리인지 본다(타입 비트 2). */
#define lv1ent_section(sent) ((*(sent) & 3) == 2)

/* [한국어] 2단계 엔트리가 매핑되지 않았는지 본다. */
#define lv2ent_fault(pent) ((*(pent) & 3) == 0)
/* [한국어] 4KB 작은 페이지 매핑인지 본다.
 * 비트 1이 서 있으면 작은 페이지다(값이 2 또는 3). */
#define lv2ent_small(pent) ((*(pent) & 2) == 2)
/* [한국어] 64KB 큰 페이지 매핑인지 본다(타입 비트 1). */
#define lv2ent_large(pent) ((*(pent) & 3) == 1)

/*
 * v1.x - v3.x SYSMMU supports 32bit physical and 32bit virtual address spaces
 * v5.0 introduced support for 36bit physical address space by shifting
 * all page entry values by 4 bits.
 * All SYSMMU controllers in the system support the address spaces of the same
 * size, so PG_ENT_SHIFT can be initialized on first SYSMMU probe to proper
 * value (0 or 4).
 */
/* [한국어] 엔트리 값과 물리 주소 사이의 시프트 양.
 * 설정자: 첫 SYSMMU의 probe가 세대를 보고 한 번만 정한다.
 * 값 범위: -1(아직 미정), 0(v1~v3), 4(v5 이상).
 * 왜 전역인가: 엔트리를 해석하는 매크로들이 도메인이나 컨트롤러를
 *              인자로 받지 않기 때문이다. 시스템의 모든 SYSMMU가
 *              같은 세대라는 전제가 이것을 가능하게 한다. */
static short PG_ENT_SHIFT = -1;
/* [한국어] v1~v3의 시프트 양 — 엔트리 값이 곧 물리 주소다. */
#define SYSMMU_PG_ENT_SHIFT 0
/* [한국어] v5 이상의 시프트 양. 엔트리를 4비트 왼쪽으로 밀어
 * 32비트 엔트리로 36비트 물리 주소를 표현한다. */
#define SYSMMU_V5_PG_ENT_SHIFT 4

/* [한국어] 현재 세대의 1단계 권한 비트 표를 가리키는 포인터.
 * 설정자: 첫 probe가 세대에 맞는 표를 꽂는다.
 * 읽는 자: mk_lv1ent_sect 매크로가 prot 값을 인덱스로 참조한다. */
static const sysmmu_pte_t *LV1_PROT;
/* [한국어] v1~v3의 1단계 권한 비트 표. prot 값(0~3)이 인덱스다.
 * 이 세대는 **쓰기 전용을 표현할 수 없어** 읽기·쓰기로 승격시킨다 —
 * 주석이 그 사실을 명시한다. */
static const sysmmu_pte_t SYSMMU_LV1_PROT[] = {
	((0 << 15) | (0 << 10)), /* no access */	/* [한국어] prot이 0일 때 — 읽기도 쓰기도 허용하지 않는다. */
	((1 << 15) | (1 << 10)), /* IOMMU_READ only */
	((0 << 15) | (1 << 10)), /* IOMMU_WRITE not supported, use read/write */
	((0 << 15) | (1 << 10)), /* IOMMU_READ | IOMMU_WRITE */
};
/* [한국어] v5 이상의 1단계 권한 비트 표.
 * 권한이 비트 4~5의 2비트 필드로 정리되어, 읽기/쓰기/양쪽을
 * 모두 정확히 표현할 수 있다. */
static const sysmmu_pte_t SYSMMU_V5_LV1_PROT[] = {
	(0 << 4), /* no access */	/* [한국어] prot이 0일 때의 v5 표현. */
	(1 << 4), /* IOMMU_READ only */
	(2 << 4), /* IOMMU_WRITE only */
	(3 << 4), /* IOMMU_READ | IOMMU_WRITE */
};

/* [한국어] 현재 세대의 2단계 권한 비트 표를 가리키는 포인터.
 * 읽는 자: mk_lv2ent_lpage/spage 매크로. */
static const sysmmu_pte_t *LV2_PROT;
/* [한국어] v1~v3의 2단계 권한 비트 표. 비트 위치만 다를 뿐
 * 1단계와 같은 제약(쓰기 전용 불가)을 갖는다. */
static const sysmmu_pte_t SYSMMU_LV2_PROT[] = {
	((0 << 9) | (0 << 4)), /* no access */	/* [한국어] 2단계에서도 prot 0은 접근 불가다. */
	((1 << 9) | (1 << 4)), /* IOMMU_READ only */
	((0 << 9) | (1 << 4)), /* IOMMU_WRITE not supported, use read/write */
	((0 << 9) | (1 << 4)), /* IOMMU_READ | IOMMU_WRITE */
};
/* [한국어] v5 이상의 2단계 권한 비트 표. 비트 2~3의 2비트 필드다. */
static const sysmmu_pte_t SYSMMU_V5_LV2_PROT[] = {
	(0 << 2), /* no access */	/* [한국어] v5의 2단계 접근 불가 값. */
	(1 << 2), /* IOMMU_READ only */
	(2 << 2), /* IOMMU_WRITE only */
	(3 << 2), /* IOMMU_READ | IOMMU_WRITE */
};

/* [한국어] 이 하드웨어가 이해하는 권한 비트만 남기는 마스크.
 * map이 prot을 이 값으로 걸러, 위 표의 인덱스가 0~3을 벗어나지
 * 않게 한다 — 배열 범위 밖 접근을 막는 안전장치이기도 하다. */
#define SYSMMU_SUPPORTED_PROT_BITS (IOMMU_READ | IOMMU_WRITE)

/* [한국어] 엔트리 값을 물리 주소로 되돌린다.
 * v5 이상에서는 4비트 왼쪽으로 밀어 36비트 주소를 복원한다. */
#define sect_to_phys(ent) (((phys_addr_t) ent) << PG_ENT_SHIFT)
/* [한국어] 섹션 엔트리에서 1MB 정렬된 물리 주소를 뽑는다. */
#define section_phys(sent) (sect_to_phys(*(sent)) & SECT_MASK)
/* [한국어] IOVA에서 섹션 안의 오프셋을 뽑는다. */
#define section_offs(iova) (iova & (SECT_SIZE - 1))
/* [한국어] 큰 페이지 엔트리에서 64KB 정렬된 물리 주소를 뽑는다. */
#define lpage_phys(pent) (sect_to_phys(*(pent)) & LPAGE_MASK)
/* [한국어] IOVA에서 큰 페이지 안의 오프셋을 뽑는다. */
#define lpage_offs(iova) (iova & (LPAGE_SIZE - 1))
/* [한국어] 작은 페이지 엔트리에서 4KB 정렬된 물리 주소를 뽑는다. */
#define spage_phys(pent) (sect_to_phys(*(pent)) & SPAGE_MASK)
/* [한국어] IOVA에서 작은 페이지 안의 오프셋을 뽑는다. */
#define spage_offs(iova) (iova & (SPAGE_SIZE - 1))

/* [한국어] 1단계 테이블의 엔트리 수. 4096개 × 1MB = 4GB로
 * 32비트 IOVA 공간 전체를 덮는다. */
#define NUM_LV1ENTRIES 4096
/* [한국어] 2단계 테이블의 엔트리 수. 1MB를 4KB로 나눈 256개다. */
#define NUM_LV2ENTRIES (SECT_SIZE / SPAGE_SIZE)

/*
 * [한국어]
 * lv1ent_offset - IOVA에서 1단계 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: 0~4095 사이의 인덱스.
 *
 * 상위 12비트가 곧 인덱스다. 마스킹이 없는 이유는 IOVA가 32비트라
 * 20비트 시프트 후 자연히 12비트만 남기 때문이다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   section_entry() / map / unmap → [lv1ent_offset]
 */
static u32 lv1ent_offset(sysmmu_iova_t iova)
{
	/* [한국어] 1MB 단위로 나눈 몫이 곧 섹션 번호다. */
	return iova >> SECT_ORDER;
}

/*
 * [한국어]
 * lv2ent_offset - IOVA에서 2단계 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: 0~255 사이의 인덱스.
 *
 * 4KB 단위로 나눈 뒤 하위 8비트만 남긴다. 상위 비트는 이미
 * 1단계 인덱스가 소비했기 때문이다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   page_entry() → [lv2ent_offset]
 */
static u32 lv2ent_offset(sysmmu_iova_t iova)
{
	/* [한국어] 페이지 번호에서 섹션 안의 위치만 남긴다. */
	return (iova >> SPAGE_ORDER) & (NUM_LV2ENTRIES - 1);
}

/* [한국어] 1단계 테이블의 바이트 크기. 4096 × 4 = 16KB다.
 * 그래서 도메인 생성이 16KB짜리 할당을 요청한다. */
#define LV1TABLE_SIZE (NUM_LV1ENTRIES * sizeof(sysmmu_pte_t))
/* [한국어] 2단계 테이블의 바이트 크기. 256 × 4 = 1KB다.
 * 슬랩 캐시의 크기이자 정렬 요구이기도 하다. */
#define LV2TABLE_SIZE (NUM_LV2ENTRIES * sizeof(sysmmu_pte_t))

/* [한국어] 큰 페이지 하나가 차지하는 2단계 엔트리의 수(16개).
 * 64KB 매핑은 같은 값을 16번 반복해 표현하는데, 하드웨어가
 * 어느 엔트리를 읽든 같은 답이 나오게 하기 위함이다. */
#define SPAGES_PER_LPAGE (LPAGE_SIZE / SPAGE_SIZE)
/* [한국어] 1단계 엔트리에서 2단계 테이블의 물리 주소를 뽑는다.
 * 0xFFFFFFC0으로 하위 6비트를 버리는 것은 타입 비트와
 * 도메인 비트를 걷어내기 위함이다. */
#define lv2table_base(sent) (sect_to_phys(*(sent) & 0xFFFFFFC0))

/* [한국어] 1MB 섹션 매핑 엔트리를 만든다.
 * 주소를 세대에 맞게 압축하고, 권한 표를 참조해 비트를 얹고,
 * 타입 2(섹션)를 붙인다. */
#define mk_lv1ent_sect(pa, prot) ((pa >> PG_ENT_SHIFT) | LV1_PROT[prot] | 2)
/* [한국어] 2단계 테이블을 가리키는 1단계 엔트리를 만든다.
 * 권한 비트가 없는 점에 유의 — 실제 권한은 2단계에서 정해진다.
 * 타입 1(테이블 링크)을 붙인다. */
#define mk_lv1ent_page(pa) ((pa >> PG_ENT_SHIFT) | 1)
/* [한국어] 64KB 큰 페이지 엔트리를 만든다. 타입 1이다. */
#define mk_lv2ent_lpage(pa, prot) ((pa >> PG_ENT_SHIFT) | LV2_PROT[prot] | 1)
/* [한국어] 4KB 작은 페이지 엔트리를 만든다. 타입 2다. */
#define mk_lv2ent_spage(pa, prot) ((pa >> PG_ENT_SHIFT) | LV2_PROT[prot] | 2)

/* [한국어] MMU_CTRL에 써 변환을 켠다. */
#define CTRL_ENABLE	0x5
/* [한국어] MMU_CTRL에 써 변환을 일시 정지시킨다.
 * 테이블을 무효화하려면 먼저 이 상태로 만들어야 한다. */
#define CTRL_BLOCK	0x7
/* [한국어] MMU_CTRL에 써 변환을 완전히 끈다. */
#define CTRL_DISABLE	0x0

/* [한국어] TLB 교체 정책을 LRU로 한다(구형 세대). */
#define CFG_LRU		0x1
/* [한국어] 엔트리의 접근 권한 비트를 실제로 검사하게 한다.
 * 이것이 없으면 권한 표를 채워도 무시된다. */
#define CFG_EAP		(1 << 2)
/* [한국어] 버스 QoS 값을 지정한다. 이 드라이버는 항상 최대(15)를
 * 준다 — 멀티미디어 블록이 실시간성을 요구하기 때문이다. */
#define CFG_QOS(n)	((n & 0xF) << 7)
/* [한국어] 자동 클럭 게이팅. v3.3부터 지원한다. */
#define CFG_ACGEN	(1 << 24) /* System MMU 3.3 only */
/* [한국어] v3.2 전용 시스템 선택 비트. */
#define CFG_SYSSEL	(1 << 22) /* System MMU 3.2 only */
/* [한국어] 1단계 엔트리 캐시(FLPD 캐시)를 켠다.
 * 성능을 위한 기능이지만, 바로 이 캐시가 v3.3의 버그를 낳아
 * 이 드라이버 곳곳의 우회 코드를 만들었다. */
#define CFG_FLPDCACHE	(1 << 20) /* System MMU 3.2+ only */

/* [한국어] v7의 가상 머신 지원을 켜는 비트. */
#define CTRL_VM_ENABLE			BIT(0)
/* [한국어] 폴트가 나면 트랜잭션을 중단시키지 않고 멈춰 세운다.
 * 그래야 소프트웨어가 처리한 뒤 재개시킬 수 있다. */
#define CTRL_VM_FAULT_MODE_STALL	BIT(3)
/* [한국어] CAPA0에서 "CAPA1 레지스터가 존재한다"를 알리는 비트. */
#define CAPA0_CAPA1_EXIST		BIT(11)
/* [한국어] CAPA1에서 "가상화 제어 레지스터가 활성"임을 알리는 비트.
 * 이 값이 v7의 레지스터 배치를 두 갈래로 가른다. */
#define CAPA1_VCR_ENABLED		BIT(14)

/* common registers */
/* [한국어] 변환 활성화/차단/비활성을 제어하는 레지스터. */
#define REG_MMU_CTRL		0x000
/* [한국어] QoS, 캐시 정책 등을 설정하는 레지스터. */
#define REG_MMU_CFG		0x004
/* [한국어] 현재 상태. 최하위 비트가 "block 완료"를 뜻한다. */
#define REG_MMU_STATUS		0x008
/* [한국어] 하드웨어 버전. 이 값이 variant 선택을 결정한다. */
#define REG_MMU_VERSION		0x034

/* [한국어] 버전 값에서 주 버전을 뽑는다. */
#define MMU_MAJ_VER(val)	((val) >> 7)
/* [한국어] 버전 값에서 부 버전을 뽑는다. */
#define MMU_MIN_VER(val)	((val) & 0x7F)
/* [한국어] 레지스터 원시 값에서 버전 필드(11비트)를 뽑는다. */
#define MMU_RAW_VER(reg)	(((reg) >> 21) & ((1 << 11) - 1)) /* 11 bits */

/* [한국어] 주/부 버전으로 비교 가능한 하나의 값을 만든다.
 * 코드 곳곳의 "version >= MAKE_MMU_VER(3, 3)" 같은 비교가
 * 이 인코딩 덕분에 성립한다. */
#define MAKE_MMU_VER(maj, min)	((((maj) & 0xF) << 7) | ((min) & 0x7F))

/* v1.x - v3.x registers */
/* [한국어] 페이지 폴트가 난 주소. */
#define REG_PAGE_FAULT_ADDR	0x024
/* [한국어] 쓰기 접근에서 폴트가 난 주소. */
#define REG_AW_FAULT_ADDR	0x028
/* [한국어] 읽기 접근에서 폴트가 난 주소. */
#define REG_AR_FAULT_ADDR	0x02C
/* [한국어] 버스 오류가 난 주소. */
#define REG_DEFAULT_SLAVE_ADDR	0x030

/* v5.x registers */
/* [한국어] v5의 읽기 폴트 주소 레지스터. */
#define REG_V5_FAULT_AR_VA	0x070
/* [한국어] v5의 쓰기 폴트 주소 레지스터. */
#define REG_V5_FAULT_AW_VA	0x080

/* v7.x registers */
/* [한국어] v7의 능력 레지스터 0. CAPA1의 존재 여부를 알려 준다. */
#define REG_V7_CAPA0		0x870
/* [한국어] v7의 능력 레지스터 1. 가상화 지원 여부를 알려 준다. */
#define REG_V7_CAPA1		0x874
/* [한국어] v7의 가상 머신 제어 레지스터. */
#define REG_V7_CTRL_VM		0x8000

/* [한국어] 이 디바이스에 SYSMMU가 딸려 있는가.
 * of_xlate가 owner를 매달았는지로 판단한다. */
#define has_sysmmu(dev)		(dev_iommu_priv_get(dev) != NULL)

/* [한국어] 페이지 테이블의 캐시 관리를 대행할 디바이스.
 * 설정자: 첫 SYSMMU의 probe가 자신을 등록한다.
 * 왜 아무거나 되는가: dma_sync는 캐시 라인을 다루는 일이라
 *                     어느 디바이스를 기준으로 하든 결과가 같다.
 * 주의: 이 드라이버는 DMA 주소와 물리 주소가 같다고 전제하며,
 *       도메인 생성에서 BUG_ON으로 그것을 확인한다. */
static struct device *dma_dev;
/* [한국어] 2단계 테이블(1KB) 전용 슬랩 캐시.
 * 설정자: exynos_iommu_init().
 * 왜 전용 캐시인가: 1KB 정렬이 하드웨어 요구이기 때문이다. */
static struct kmem_cache *lv2table_kmem_cache;
/* [한국어] 모든 엔트리가 0인 더미 2단계 테이블.
 * 설정자: exynos_iommu_init()이 하나만 만들어 전역으로 공유한다.
 * 왜 필요한가: v3.3의 FLPD 캐시가 "매핑 없음" 상태를 캐시해
 *              버리는 버그 때문이다. 빈 1단계 엔트리를 0으로 두는
 *              대신 이 테이블을 가리키게 하면, 하드웨어가 미리
 *              읽어도 유효한 링크를 보게 되어 문제가 완화된다. */
static sysmmu_pte_t *zero_lv2_table;
/* [한국어] 그 더미 테이블을 가리키는 1단계 엔트리 값.
 * "빈 자리"의 표준 표현이며, lv1ent_fault와 lv1ent_zero가
 * 이 값을 기준으로 판별한다. */
#define ZERO_LV2LINK mk_lv1ent_page(virt_to_phys(zero_lv2_table))

/*
 * [한국어]
 * section_entry - IOVA에 해당하는 1단계 엔트리의 주소를 구한다
 *
 * @pgtable: 1단계 테이블의 시작 주소.
 * @iova: 대상 IOVA.
 * @return: 그 엔트리의 주소.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys/폴트 진단 → [section_entry]
 */
static sysmmu_pte_t *section_entry(sysmmu_pte_t *pgtable, sysmmu_iova_t iova)
{
	/* [한국어] 테이블 시작에서 섹션 번호만큼 나아간 자리다. */
	return pgtable + lv1ent_offset(iova);
}

/*
 * [한국어]
 * page_entry - 1단계 엔트리 아래에서 IOVA에 해당하는 2단계 엔트리를 구한다
 *
 * @sent: 1단계 엔트리(2단계 테이블을 가리켜야 한다).
 * @iova: 대상 IOVA.
 * @return: 그 2단계 엔트리의 주소.
 *
 * 엔트리에서 물리 주소를 뽑아 커널 가상 주소로 바꾼 뒤 인덱스를
 * 더한다. 테이블이 저수준 매핑 영역에 있어 phys_to_virt가 통한다.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   alloc_lv2entry() / unmap / iova_to_phys → [page_entry]
 */
static sysmmu_pte_t *page_entry(sysmmu_pte_t *sent, sysmmu_iova_t iova)
{
	/* [한국어] 링크에서 테이블 주소를 복원하고 인덱스만큼 나아간다. */
	return (sysmmu_pte_t *)phys_to_virt(
				lv2table_base(sent)) + lv2ent_offset(iova);
}

/* [한국어] 세대를 가리지 않는 통일된 폴트 정보.
 * 세대별 get_fault_info가 이 형태로 채워 준다. */
struct sysmmu_fault {
	sysmmu_iova_t addr;	/* IOVA address that caused fault */
	/* [한국어] 폴트가 난 IOVA.
	 * 설정자: 세대별 get_fault_info가 해당 레지스터를 읽어 채운다.
	 * 읽는 자: 로그 출력과 report_iommu_fault. */

	const char *name;	/* human readable fault name */
	/* [한국어] 사람이 읽을 폴트 이름("PAGE", "MULTI-HIT" 등).
	 * 설정자: 세대별 이름 표에서 가져온다.
	 * 읽는 자: 로그 출력. */

	unsigned int type;	/* fault type for report_iommu_fault() */
	/* [한국어] 읽기 폴트인가 쓰기 폴트인가.
	 * 읽는 자: report_iommu_fault에 그대로 전달된다. */
};

/* [한국어] v1~v3의 폴트 종류별 정보.
 * 이 세대는 인터럽트 비트마다 읽어야 할 주소 레지스터가 달라,
 * 표로 대응시켜 둔다. */
struct sysmmu_v1_fault_info {
	unsigned short addr_reg; /* register to read IOVA fault address */
	/* [한국어] 이 폴트의 주소를 담고 있는 레지스터의 오프셋.
	 * 종류마다 다른 것이 v1~v3의 특징이다. */

	const char *name;	/* human readable fault name */
	/* [한국어] 폴트 이름. */

	unsigned int type;	/* fault type for report_iommu_fault */
	/* [한국어] 접근 방향. 이 세대는 폴트 종류가 곧 방향을 함의한다. */
};

/* [한국어] v1~v3의 인터럽트 비트 순서대로 나열한 폴트 표.
 * 인터럽트 상태의 몇 번째 비트가 섰는지가 곧 이 배열의 인덱스다. */
static const struct sysmmu_v1_fault_info sysmmu_v1_faults[] = {
	{ REG_PAGE_FAULT_ADDR, "PAGE", IOMMU_FAULT_READ },	/* [한국어] 인터럽트 비트 0 — 일반 페이지 폴트. */
	{ REG_AR_FAULT_ADDR, "MULTI-HIT", IOMMU_FAULT_READ },
	{ REG_AW_FAULT_ADDR, "MULTI-HIT", IOMMU_FAULT_WRITE },
	{ REG_DEFAULT_SLAVE_ADDR, "BUS ERROR", IOMMU_FAULT_READ },
	{ REG_AR_FAULT_ADDR, "SECURITY PROTECTION", IOMMU_FAULT_READ },
	{ REG_AR_FAULT_ADDR, "ACCESS PROTECTION", IOMMU_FAULT_READ },
	{ REG_AW_FAULT_ADDR, "SECURITY PROTECTION", IOMMU_FAULT_WRITE },
	{ REG_AW_FAULT_ADDR, "ACCESS PROTECTION", IOMMU_FAULT_WRITE },
};

/* SysMMU v5 has the same faults for AR (0..4 bits) and AW (16..20 bits) */
/* [한국어] v5의 폴트 이름 표.
 * 읽기와 쓰기가 같은 종류를 갖되 비트 위치만 16만큼 떨어져 있어,
 * 이름 표는 하나면 충분하다 — get_fault_info가 16을 빼서 인덱스를 맞춘다. */
static const char * const sysmmu_v5_fault_names[] = {
	"PTW",	/* [한국어] 페이지 테이블 워크 자체가 실패한 경우. */
	"PAGE",
	"MULTI-HIT",
	"ACCESS PROTECTION",
	"SECURITY PROTECTION"
};

/* [한국어] v7의 폴트 이름 표.
 * 4개뿐이라 인터럽트 비트를 4로 나눈 나머지로 인덱스를 만든다. */
static const char * const sysmmu_v7_fault_names[] = {
	"PTW",	/* [한국어] v7에서도 워크 실패가 첫 항목이다. */
	"PAGE",
	"ACCESS PROTECTION",
	"RESERVED"
};

/*
 * This structure is attached to dev->iommu->priv of the master device
 * on device add, contains a list of SYSMMU controllers defined by device tree,
 * which are bound to given master device. It is usually referenced by 'owner'
 * pointer.
*/
/* [한국어] 마스터 디바이스 하나에 딸린 SYSMMU들의 묶음. */
struct exynos_iommu_owner {
	struct list_head controllers;	/* list of sysmmu_drvdata.owner_node */
	/* [한국어] 이 마스터에 딸린 SYSMMU 컨트롤러들의 목록.
	 * 설정자: of_xlate가 디바이스 트리 항목마다 추가한다.
	 * 읽는 자: attach/detach와 PM 콜백이 이 목록을 훑어 모든
	 *          컨트롤러를 같은 상태로 유지한다.
	 * 왜 여럿인가: 하나의 IP 블록이 여러 포트로 DMA를 내면
	 *              포트마다 SYSMMU가 붙기 때문이다. */

	struct iommu_domain *domain;	/* domain this device is attached */
	/* [한국어] 현재 붙어 있는 도메인.
	 * 설정자: attach/detach 계열. of_xlate가 identity로 초기화한다.
	 * 읽는 자: identity_attach가 "이미 떼어졌는가"를 판단할 때. */

	struct mutex rpm_lock;		/* for runtime pm of all sysmmus */
	/* [한국어] 이 마스터의 모든 SYSMMU에 대한 전원 전환과 도메인
	 * 전환을 직렬화하는 뮤텍스.
	 * 왜 필요한가: attach가 하드웨어를 켜는 동안 런타임 PM 콜백이
	 *              끼어들면 반쯤 설정된 상태가 된다. suspend/resume도
	 *              같은 락을 잡아 그것을 막는다. */
};

/*
 * This structure exynos specific generalization of struct iommu_domain.
 * It contains list of SYSMMU controllers from all master devices, which has
 * been attached to this domain and page tables of IO address space defined by
 * it. It is usually referenced by 'domain' pointer.
 */
/* [한국어] IOMMU 도메인 하나. */
struct exynos_iommu_domain {
	struct list_head clients; /* list of sysmmu_drvdata.domain_node */
	/* [한국어] 이 도메인을 쓰는 모든 SYSMMU 컨트롤러의 목록.
	 * 설정자: attach가 추가, detach가 제거.
	 * 읽는 자: 무효화가 이 목록의 모든 컨트롤러에 명령을 낸다.
	 * 동기화: lock. */

	sysmmu_pte_t *pgtable;	/* lv1 page table, 16KB */
	/* [한국어] 1단계 페이지 테이블(4096 엔트리, 16KB).
	 * 설정자: domain_alloc_paging이 잡고 전부 ZERO_LV2LINK로 채운다.
	 * 읽는 자: 모든 워크의 출발점이자, 하드웨어에 알릴 주소의 근거.
	 * 동기화: pgtablelock. */

	short *lv2entcnt;	/* free lv2 entry counter for each section */
	/* [한국어] 섹션마다 "그 2단계 테이블에 남은 빈 엔트리 수".
	 * 설정자: 2단계 테이블을 만들 때 256으로 초기화하고,
	 *          매핑할 때 줄이고 해제할 때 늘린다.
	 * 읽는 자: lv1set_section이 "이 섹션을 1MB 매핑으로 바꿔도
	 *          되는가"를 판단한다 — 256이면 아무 매핑도 없다는 뜻이다.
	 * 크기: 4096개 × 2바이트 = 8KB. */

	spinlock_t lock;	/* lock for modifying list of clients */
	/* [한국어] clients 목록을 보호하는 락.
	 * pgtablelock과 분리한 이유: 무효화 순회는 하드웨어를 만지느라
	 *                            오래 걸리는데, 그동안 매핑을 막을
	 *                            이유가 없기 때문이다. */

	spinlock_t pgtablelock;	/* lock for modifying page table @ pgtable */
	/* [한국어] 페이지 테이블 갱신을 보호하는 락.
	 * 읽는 자: map/unmap/iova_to_phys가 irqsave로 잡는다. */

	struct iommu_domain domain; /* generic domain data structure */
	/* [한국어] 코어가 보는 도메인 부분(임베드).
	 * 설정자: 생성 시 세 가지 페이지 크기와 32비트 aperture를 알린다. */
};

/* [한국어] 컨트롤러 구조체의 전방 선언.
 * 아래 variant의 콜백 시그니처에 필요하지만 정의는 그보다 뒤에 온다. */
struct sysmmu_drvdata;

/*
 * SysMMU version specific data. Contains offsets for the registers which can
 * be found in different SysMMU variants, but have different offset values.
 * Also contains version specific callbacks to abstract the hardware.
 */
/* [한국어] 세대별 레지스터 오프셋과 콜백을 모은 표.
 * 이 구조체 덕분에 나머지 코드가 SYSMMU_REG 매크로 하나로
 * 세대를 가리지 않고 레지스터에 접근한다. */
struct sysmmu_variant {
	u32 pt_base;		/* page table base address (physical) */
	/* [한국어] 1단계 테이블의 주소를 쓰는 레지스터의 오프셋. */

	u32 flush_all;		/* invalidate all TLB entries */
	/* [한국어] TLB 전체 무효화 명령 레지스터. */

	u32 flush_entry;	/* invalidate specific TLB entry */
	/* [한국어] 특정 IOVA 하나만 무효화하는 레지스터. */

	u32 flush_range;	/* invalidate TLB entries in specified range */
	/* [한국어] 범위 무효화를 실행시키는 레지스터.
	 * 값 범위: v1~v3에는 없어 0이다 — 그 세대는 항목마다 반복한다. */

	u32 flush_start;	/* start address of range invalidation */
	/* [한국어] 범위 무효화의 시작 주소를 쓰는 레지스터. */

	u32 flush_end;		/* end address of range invalidation */
	/* [한국어] 범위 무효화의 끝 주소를 쓰는 레지스터. */

	u32 int_status;		/* interrupt status information */
	/* [한국어] 어떤 폴트가 났는지 비트로 알려 주는 레지스터. */

	u32 int_clear;		/* clear the interrupt */
	/* [한국어] 처리한 폴트 비트를 지우는 레지스터. */

	u32 fault_va;		/* IOVA address that caused fault */
	/* [한국어] 폴트 주소 레지스터. v7에서만 통일된 자리에 있다. */

	u32 fault_info;		/* fault transaction info */
	/* [한국어] 폴트 트랜잭션의 부가 정보(방향 등). v7 전용이다. */

	int (*get_fault_info)(struct sysmmu_drvdata *data, unsigned int itype,
			      struct sysmmu_fault *fault);
	/* [한국어] 인터럽트 비트 번호를 통일된 폴트 정보로 옮기는 콜백.
	 * 왜 콜백인가: 세대마다 비트 배치와 주소 레지스터의 규칙이
	 *              완전히 달라 표만으로는 표현할 수 없다. */
};

/*
 * This structure hold all data of a single SYSMMU controller, this includes
 * hw resources like registers and clocks, pointers and list nodes to connect
 * it to all other structures, internal state and parameters read from device
 * tree. It is usually referenced by 'data' pointer.
 */
/* [한국어] SYSMMU 컨트롤러 하나의 상태. */
struct sysmmu_drvdata {
	struct device *sysmmu;		/* SYSMMU controller device */
	/* [한국어] 이 컨트롤러의 플랫폼 디바이스.
	 * 읽는 자: 로그, 런타임 PM, devm 할당의 기준점. */

	struct device *master;		/* master device (owner) */
	/* [한국어] 이 SYSMMU가 담당하는 마스터 디바이스.
	 * 설정자: of_xlate가 채운다.
	 * 값 범위: NULL이면 아직 아무 마스터에도 연결되지 않은 상태라,
	 *          PM 콜백이 그것을 보고 아무 일도 하지 않는다. */

	struct device_link *link;	/* runtime PM link to master */
	/* [한국어] 마스터와의 전원 의존 링크.
	 * 설정자: probe_device가 만들고 release_device가 지운다.
	 * 왜 필요한가: 마스터가 깨어날 때 SYSMMU가 먼저 깨어나야 한다.
	 *              이 링크 덕분에 이 드라이버는 직접 전원을 켜지 않는다. */

	void __iomem *sfrbase;		/* our registers */
	/* [한국어] 매핑된 레지스터 영역. */

	struct clk *clk;		/* SYSMMU's clock */
	/* [한국어] SYSMMU의 주 클럭.
	 * 값 범위: 선택 사항이라 NULL일 수 있다. 다만 clk이 없으면
	 *          aclk와 pclk가 모두 있어야 한다. */

	struct clk *aclk;		/* SYSMMU's aclk clock */
	/* [한국어] 버스 클럭. clk 대신 aclk+pclk 조합을 쓰는 SoC가 있다. */

	struct clk *pclk;		/* SYSMMU's pclk clock */
	/* [한국어] 주변장치 클럭. aclk와 짝을 이룬다. */

	struct clk *clk_master;		/* master's device clock */
	/* [한국어] 마스터의 클럭.
	 * 왜 SYSMMU가 이것을 만지는가: SYSMMU 레지스터가 마스터의
	 *                              클럭 도메인에 걸려 있어, 레지스터를
	 *                              읽으려면 켜야 하기 때문이다.
	 * 정책: **아주 짧게만 켠다.** DMA 중의 유지는 클라이언트 몫이다. */

	spinlock_t lock;		/* lock for modifying state */
	/* [한국어] 이 컨트롤러의 상태와 레지스터 접근을 보호하는 락.
	 * 읽는 자: enable/disable, 무효화, 인터럽트 핸들러. */

	bool active;			/* current status */
	/* [한국어] 지금 이 SYSMMU가 켜져 있는가.
	 * 설정자: __sysmmu_enable/disable.
	 * 읽는 자: 무효화 함수들이 꺼진 하드웨어를 만지지 않도록 검사한다. */

	struct exynos_iommu_domain *domain; /* domain we belong to */
	/* [한국어] 이 컨트롤러가 속한 도메인.
	 * 값 범위: NULL이면 어느 도메인에도 붙어 있지 않다.
	 * 읽는 자: PM 콜백이 되살릴지 정할 때, 인터럽트가 폴트를
	 *          보고할 대상을 찾을 때. */

	struct list_head domain_node;	/* node for domain clients list */
	/* [한국어] 도메인의 clients 목록에 매다는 고리. */

	struct list_head owner_node;	/* node for owner controllers list */
	/* [한국어] 마스터의 controllers 목록에 매다는 고리. */

	phys_addr_t pgtable;		/* assigned page table structure */
	/* [한국어] 이 컨트롤러에 설정할 1단계 테이블의 물리 주소.
	 * 설정자: attach가 채우고 detach가 0으로 만든다.
	 * 읽는 자: __sysmmu_enable이 레지스터에 심고, 폴트 진단이
	 *          테이블을 되짚을 때 출발점으로 쓴다.
	 * 왜 별도로 두는가: 전원이 꺼졌다 켜질 때 도메인을 거치지 않고
	 *                   바로 되살릴 수 있게 하려는 것이다. */

	unsigned int version;		/* our version */
	/* [한국어] MAKE_MMU_VER로 인코딩된 하드웨어 버전.
	 * 읽는 자: 설정 값 선택, 무효화 방식 선택, 주소 시프트 결정. */

	struct iommu_device iommu;	/* IOMMU core handle */
	/* [한국어] IOMMU 코어에 등록되는 부분(임베드). */

	const struct sysmmu_variant *variant; /* version specific data */
	/* [한국어] 이 세대의 레지스터 배치와 폴트 해석 콜백.
	 * 설정자: __sysmmu_get_version이 버전을 읽고 고른다. */

	/* v7 fields */
	bool has_vcr;			/* virtual machine control register */
	/* [한국어] v7에서 가상화 제어 레지스터가 있는가.
	 * 왜 중요한가: 있으면 레지스터 배치가 통째로 달라져
	 *              다른 variant를 써야 한다. */
};

/* [한국어] 세대별 오프셋을 통해 레지스터 주소를 만든다.
 * 이 매크로 하나로 나머지 코드가 세대를 의식하지 않게 된다. */
#define SYSMMU_REG(data, reg) ((data)->sfrbase + (data)->variant->reg)

/*
 * [한국어]
 * exynos_sysmmu_v1_get_fault_info - v1~v3의 폴트 정보를 읽는다
 *
 * @data: 폴트를 낸 컨트롤러.
 * @itype: 인터럽트 상태에서 처음 선 비트의 번호.
 * @fault: 채울 폴트 정보.
 * @return: 0 성공, -ENXIO(모르는 비트).
 *
 * 이 세대는 폴트 종류마다 주소를 담은 레지스터가 다르다. 그래서
 * 비트 번호를 인덱스로 표를 찾아, 거기 적힌 레지스터를 읽는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   exynos_sysmmu_irq() → variant->get_fault_info
 *   → [exynos_sysmmu_v1_get_fault_info]
 */
static int exynos_sysmmu_v1_get_fault_info(struct sysmmu_drvdata *data,
					   unsigned int itype,
					   struct sysmmu_fault *fault)
{
	/* [한국어] 표에서 찾은 이 폴트의 정보. */
	const struct sysmmu_v1_fault_info *finfo;

	/* [한국어] 표에 없는 비트라면 이 드라이버가 모르는 폴트다. */
	if (itype >= ARRAY_SIZE(sysmmu_v1_faults))
		return -ENXIO;

	/* [한국어] 비트 번호가 곧 표의 인덱스다. */
	finfo = &sysmmu_v1_faults[itype];
	/* [한국어] 종류마다 다른 레지스터에서 주소를 읽는다. */
	fault->addr = readl(data->sfrbase + finfo->addr_reg);
	/* [한국어] 이름과 방향은 표에 적힌 그대로다. */
	fault->name = finfo->name;
	fault->type = finfo->type;	/* [한국어] 방향도 표에 적힌 그대로 옮긴다. */

	return 0;	/* [한국어] 폴트 정보를 모두 채웠다. */
}

/*
 * [한국어]
 * exynos_sysmmu_v5_get_fault_info - v5의 폴트 정보를 읽는다
 *
 * @data: 폴트를 낸 컨트롤러.
 * @itype: 인터럽트 비트 번호.
 * @fault: 채울 폴트 정보.
 * @return: 0 성공, -ENXIO(모르는 비트).
 *
 * v5는 읽기 폴트와 쓰기 폴트가 **같은 종류를 갖되 비트만 16만큼
 * 떨어져 있다**(원본 주석이 그것을 밝힌다). 그래서 비트 번호가
 * 16 이상이면 쓰기로 판단하고 16을 빼 같은 이름 표를 재사용한다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   exynos_sysmmu_irq() → variant->get_fault_info
 *   → [exynos_sysmmu_v5_get_fault_info]
 */
static int exynos_sysmmu_v5_get_fault_info(struct sysmmu_drvdata *data,
					   unsigned int itype,
					   struct sysmmu_fault *fault)
{
	/* [한국어] 방향에 따라 달라지는 주소 레지스터. */
	unsigned int addr_reg;

	/* [한국어] 낮은 비트 구간(0~4)은 읽기 폴트다. */
	if (itype < ARRAY_SIZE(sysmmu_v5_fault_names)) {
		fault->type = IOMMU_FAULT_READ;	/* [한국어] 낮은 비트 구간은 읽기 접근의 폴트다. */
		addr_reg = REG_V5_FAULT_AR_VA;
	/* [한국어] 16~20 구간은 같은 종류의 쓰기 폴트다.
	 * 16을 빼면 이름 표의 인덱스가 그대로 맞는다. */
	} else if (itype >= 16 && itype <= 20) {
		fault->type = IOMMU_FAULT_WRITE;	/* [한국어] 16 이상은 같은 종류의 쓰기 폴트다. */
		addr_reg = REG_V5_FAULT_AW_VA;	/* [한국어] 쓰기 전용 주소 레지스터를 쓴다. */
		itype -= 16;	/* [한국어] 이름 표의 인덱스를 맞추기 위해 16을 뺀다. */
	} else {
		/* [한국어] 두 구간 어디에도 없는 비트다. */
		return -ENXIO;
	}

	/* [한국어] 조정된 인덱스로 이름을 찾는다. */
	fault->name = sysmmu_v5_fault_names[itype];
	/* [한국어] 방향에 맞는 레지스터에서 주소를 읽는다. */
	fault->addr = readl(data->sfrbase + addr_reg);

	return 0;	/* [한국어] 폴트 정보를 모두 채웠다. */
}

/*
 * [한국어]
 * exynos_sysmmu_v7_get_fault_info - v7의 폴트 정보를 읽는다
 *
 * @data: 폴트를 낸 컨트롤러.
 * @itype: 인터럽트 비트 번호.
 * @fault: 채울 폴트 정보.
 * @return: 항상 0.
 *
 * v7은 앞 세대들보다 단순하다. 주소와 부가 정보가 각각 한 레지스터에
 * 모여 있고, 방향은 부가 정보의 비트 20이 알려 준다. 이름 표가
 * 4개뿐이라 비트 번호를 4로 나눈 나머지를 인덱스로 쓴다 —
 * 그래서 어떤 비트가 서도 실패하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   exynos_sysmmu_irq() → variant->get_fault_info
 *   → [exynos_sysmmu_v7_get_fault_info]
 */
static int exynos_sysmmu_v7_get_fault_info(struct sysmmu_drvdata *data,
					   unsigned int itype,
					   struct sysmmu_fault *fault)
{
	/* [한국어] 폴트 트랜잭션의 부가 정보(방향 등). */
	u32 info = readl(SYSMMU_REG(data, fault_info));

	/* [한국어] 주소는 세대별 오프셋의 한 레지스터에 모여 있다. */
	fault->addr = readl(SYSMMU_REG(data, fault_va));
	/* [한국어] 이름 표가 4개뿐이라 나머지 연산으로 인덱스를 만든다.
	 * 덕분에 어떤 비트가 서도 배열 범위를 벗어나지 않는다. */
	fault->name = sysmmu_v7_fault_names[itype % 4];
	/* [한국어] 부가 정보의 비트 20이 쓰기 여부를 알려 준다. */
	fault->type = (info & BIT(20)) ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ;

	return 0;	/* [한국어] v7은 모르는 비트가 없어 항상 성공이다. */
}

/* SysMMU v1..v3 */
/* [한국어] v1~v3의 레지스터 배치.
 * 범위 무효화 레지스터가 없는 점이 특징이다 — 그래서 이 세대는
 * 항목마다 반복해 무효화한다. */
static const struct sysmmu_variant sysmmu_v1_variant = {
	.flush_all	= 0x0c,
	/* [한국어] TLB 전체 무효화. */

	.flush_entry	= 0x10,
	/* [한국어] 항목 하나 무효화. 이 세대의 유일한 선택적 무효화다. */

	.pt_base	= 0x14,
	/* [한국어] 테이블 주소 레지스터. 이 세대는 물리 주소를 그대로 쓴다. */

	.int_status	= 0x18,
	/* [한국어] 인터럽트 상태. */

	.int_clear	= 0x1c,
	/* [한국어] 인터럽트 지우기. */

	.get_fault_info	= exynos_sysmmu_v1_get_fault_info,
	/* [한국어] 종류마다 다른 주소 레지스터를 표로 찾는 해석기. */
};

/* SysMMU v5 */
/* [한국어] v5의 레지스터 배치.
 * 범위 무효화가 생겼고, 테이블 주소를 4KB 단위로 압축해 쓴다. */
static const struct sysmmu_variant sysmmu_v5_variant = {
	.pt_base	= 0x0c,
	/* [한국어] 테이블 주소 레지스터. 페이지 번호로 압축해 쓴다. */

	.flush_all	= 0x10,
	/* [한국어] 전체 무효화. */

	.flush_entry	= 0x14,
	/* [한국어] 항목 하나 무효화. */

	.flush_range	= 0x18,
	/* [한국어] 범위 무효화 실행. 이 세대에서 추가됐다. */

	.flush_start	= 0x20,
	/* [한국어] 범위의 시작 주소. */

	.flush_end	= 0x24,
	/* [한국어] 범위의 끝 주소. */

	.int_status	= 0x60,
	/* [한국어] 인터럽트 상태. */

	.int_clear	= 0x64,
	/* [한국어] 인터럽트 지우기. */

	.get_fault_info	= exynos_sysmmu_v5_get_fault_info,
	/* [한국어] 읽기/쓰기가 16비트 간격으로 대칭인 해석기. */
};

/* SysMMU v7: non-VM capable register layout */
/* [한국어] v7 중 가상화를 지원하지 않는 것의 레지스터 배치.
 * 무효화 계열은 v5와 같고, 폴트 관련 레지스터가 통일되었다. */
static const struct sysmmu_variant sysmmu_v7_variant = {
	.pt_base	= 0x0c,
	/* [한국어] 테이블 주소 레지스터. */

	.flush_all	= 0x10,
	/* [한국어] 전체 무효화. */

	.flush_entry	= 0x14,
	/* [한국어] 항목 무효화. */

	.flush_range	= 0x18,
	/* [한국어] 범위 무효화 실행. */

	.flush_start	= 0x20,
	/* [한국어] 범위 시작. */

	.flush_end	= 0x24,
	/* [한국어] 범위 끝. */

	.int_status	= 0x60,
	/* [한국어] 인터럽트 상태. */

	.int_clear	= 0x64,
	/* [한국어] 인터럽트 지우기. */

	.fault_va	= 0x70,
	/* [한국어] 폴트 주소. 방향과 무관한 단일 레지스터가 되었다. */

	.fault_info	= 0x78,
	/* [한국어] 폴트 부가 정보. 방향 비트가 여기 있다. */

	.get_fault_info	= exynos_sysmmu_v7_get_fault_info,
	/* [한국어] 단일 레지스터에서 읽는 단순한 해석기. */
};

/* SysMMU v7: VM capable register layout */
/* [한국어] v7 중 가상화를 지원하는 것의 레지스터 배치.
 * 무효화와 테이블 주소 레지스터가 0x8000 대역으로 통째로 옮겨졌다 —
 * 가상 머신마다 독립된 창을 두기 위한 재배치다.
 * 인터럽트 레지스터만 제자리에 남아 있는 점이 흥미롭다. */
static const struct sysmmu_variant sysmmu_v7_vm_variant = {
	.pt_base	= 0x800c,
	/* [한국어] VM 대역으로 옮겨진 테이블 주소 레지스터. */

	.flush_all	= 0x8010,
	/* [한국어] VM 대역의 전체 무효화. */

	.flush_entry	= 0x8014,
	/* [한국어] VM 대역의 항목 무효화. */

	.flush_range	= 0x8018,
	/* [한국어] VM 대역의 범위 무효화. */

	.flush_start	= 0x8020,
	/* [한국어] 범위 시작. */

	.flush_end	= 0x8024,
	/* [한국어] 범위 끝. */

	.int_status	= 0x60,
	/* [한국어] 인터럽트는 VM과 무관해 원래 자리에 남아 있다. */

	.int_clear	= 0x64,
	/* [한국어] 마찬가지. */

	.fault_va	= 0x1000,
	/* [한국어] 폴트 주소는 또 다른 자리로 옮겨졌다. */

	.fault_info	= 0x1004,
	/* [한국어] 폴트 부가 정보. */

	.get_fault_info	= exynos_sysmmu_v7_get_fault_info,
	/* [한국어] 해석 방식은 non-VM 판과 같다 — 오프셋만 다르다. */
};

/*
 * [한국어]
 * to_exynos_domain - iommu_domain에서 바깥 도메인을 복원한다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 드라이버 쪽 도메인.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_domain_ops 콜백 → [to_exynos_domain]
 */
static struct exynos_iommu_domain *to_exynos_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */
	return container_of(dom, struct exynos_iommu_domain, domain);
}

/*
 * [한국어]
 * sysmmu_unblock - 일시 정지시킨 SYSMMU를 다시 돌린다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * ENABLE을 쓰면 정지가 풀린다. block과 짝을 이뤄 무효화 구간을
 * 감싸는 데 쓰인다.
 *
 * 실행 컨텍스트: 무효화와 인터럽트 처리. 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   무효화 함수들 / exynos_sysmmu_irq() → [sysmmu_unblock]
 */
static void sysmmu_unblock(struct sysmmu_drvdata *data)
{
	/* [한국어] ENABLE을 다시 써 변환을 재개시킨다. */
	writel(CTRL_ENABLE, data->sfrbase + REG_MMU_CTRL);
}

/*
 * [한국어]
 * sysmmu_block - SYSMMU를 일시 정지시키고 완료를 기다린다
 *
 * @data: 대상 컨트롤러.
 * @return: 정지에 성공하면 참.
 *
 * 이 하드웨어에서 TLB를 무효화하려면 먼저 변환을 멈춰야 한다.
 * BLOCK을 쓴 뒤 상태 레지스터의 최하위 비트가 설 때까지 바쁜
 * 대기를 한다. 120번이라는 횟수는 경험적인 값이다.
 *
 * 실패하면 **스스로 정지를 푼다.** 멈추지도 못한 채 걸어 잠근
 * 상태로 두면 그 SYSMMU가 영영 동작하지 않기 때문이다. 그래서
 * 호출자는 반환값이 거짓이면 무효화를 건너뛰기만 하면 된다.
 *
 * 실행 컨텍스트: 락을 잡고 클럭이 켜진 상태. 바쁜 대기라 짧다.
 *
 * 호출 체인:
 *   sysmmu_tlb_invalidate_entry() / flpdcache() → [sysmmu_block]
 */
static bool sysmmu_block(struct sysmmu_drvdata *data)
{
	/* [한국어] 남은 재시도 횟수. 경험적으로 정해진 상한이다. */
	int i = 120;

	/* [한국어] 정지 명령을 낸다. */
	writel(CTRL_BLOCK, data->sfrbase + REG_MMU_CTRL);
	/* [한국어] 상태의 최하위 비트가 설 때까지 기다린다.
	 * 잠들 수 없는 문맥이라 바쁜 대기를 쓴다. */
	while ((i > 0) && !(readl(data->sfrbase + REG_MMU_STATUS) & 1))
		--i;

	/* [한국어] 끝내 멈추지 않았다 — 걸어 잠근 채로 두면 이 SYSMMU가
	 * 영영 동작하지 않으므로 스스로 풀고 실패를 알린다. */
	if (!(readl(data->sfrbase + REG_MMU_STATUS) & 1)) {
		sysmmu_unblock(data);	/* [한국어] 멈추지 못한 채 잠가 두면 이 SYSMMU가 영영 죽는다. */
		return false;	/* [한국어] 호출자가 무효화를 건너뛰도록 실패를 알린다. */
	}

	return true;	/* [한국어] 정지에 성공했으니 무효화를 진행해도 된다. */
}

/*
 * [한국어]
 * __sysmmu_tlb_invalidate - TLB 전체를 무효화한다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 실행 컨텍스트: 클럭이 켜지고 정지된 상태.
 *
 * 호출 체인:
 *   __sysmmu_set_ptbase() / flpdcache 무효화 → [__sysmmu_tlb_invalidate]
 */
static void __sysmmu_tlb_invalidate(struct sysmmu_drvdata *data)
{
	/* [한국어] 1을 쓰면 전체 무효화가 실행된다. */
	writel(0x1, SYSMMU_REG(data, flush_all));
}

/*
 * [한국어]
 * __sysmmu_tlb_invalidate_entry - 지정한 범위의 TLB 항목을 무효화한다
 *
 * @data: 대상 컨트롤러.
 * @iova: 무효화할 시작 IOVA.
 * @num_inv: 무효화할 페이지 수.
 * @return: 없음.
 *
 * 세대에 따라 두 방식이 갈린다. v5 미만이거나 한 페이지뿐이면
 * 항목마다 하나씩 쓰고, 그 외에는 시작과 끝을 지정해 범위
 * 무효화를 한 번에 실행한다.
 *
 * 한 페이지일 때도 항목 방식을 쓰는 이유: 범위 무효화는 레지스터를
 * 세 번 써야 하는데, 한 페이지면 항목 방식이 한 번으로 끝나 더 싸다.
 *
 * 실행 컨텍스트: 클럭이 켜지고 정지된 상태.
 *
 * 호출 체인:
 *   sysmmu_tlb_invalidate_entry() → [__sysmmu_tlb_invalidate_entry]
 */
static void __sysmmu_tlb_invalidate_entry(struct sysmmu_drvdata *data,
				sysmmu_iova_t iova, unsigned int num_inv)
{
	/* [한국어] 항목 순회 인덱스. */
	unsigned int i;

	/* [한국어] 범위 무효화가 없는 세대이거나, 한 페이지뿐이라
	 * 항목 방식이 더 싼 경우. */
	if (MMU_MAJ_VER(data->version) < 5 || num_inv == 1) {
		for (i = 0; i < num_inv; i++) {
			/* [한국어] 페이지 정렬된 주소에 유효 비트를 얹어 쓴다. */
			writel((iova & SPAGE_MASK) | 1,
			       SYSMMU_REG(data, flush_entry));
			/* [한국어] 다음 페이지로 나아간다. */
			iova += SPAGE_SIZE;
		}
	} else {
		/* [한국어] 범위의 시작을 쓴다. */
		writel(iova & SPAGE_MASK, SYSMMU_REG(data, flush_start));
		/* [한국어] 끝은 마지막 페이지의 시작 주소다(포함 구간). */
		writel((iova & SPAGE_MASK) + (num_inv - 1) * SPAGE_SIZE,
		       SYSMMU_REG(data, flush_end));
		/* [한국어] 그 범위에 대해 무효화를 실행시킨다. */
		writel(0x1, SYSMMU_REG(data, flush_range));
	}
}

/*
 * [한국어]
 * __sysmmu_set_ptbase - 1단계 테이블의 주소를 하드웨어에 알린다
 *
 * @data: 대상 컨트롤러.
 * @pgd: 테이블의 물리 주소.
 * @return: 없음.
 *
 * v5부터는 주소를 4KB 단위로 압축해 쓴다 — 32비트 레지스터로
 * 36비트 주소를 표현하기 위한 같은 발상이다.
 *
 * 테이블을 바꾼 뒤에는 반드시 TLB를 비워야 한다. 옛 테이블로
 * 채워진 항목이 남아 있으면 새 매핑이 무시되기 때문이다.
 *
 * 실행 컨텍스트: __sysmmu_enable 안. 정지된 상태.
 *
 * 호출 체인:
 *   __sysmmu_enable() → [__sysmmu_set_ptbase] → __sysmmu_tlb_invalidate()
 */
static void __sysmmu_set_ptbase(struct sysmmu_drvdata *data, phys_addr_t pgd)
{
	/* [한국어] 레지스터에 쓸 값. */
	u32 pt_base;

	/* [한국어] 구세대는 물리 주소를 그대로 쓴다. */
	if (MMU_MAJ_VER(data->version) < 5)
		pt_base = pgd;
	else
		/* [한국어] v5부터는 페이지 번호로 압축해 넓은 주소를 담는다. */
		pt_base = pgd >> SPAGE_ORDER;

	/* [한국어] 세대별 오프셋의 레지스터에 쓴다. */
	writel(pt_base, SYSMMU_REG(data, pt_base));
	/* [한국어] 옛 테이블의 잔재를 지운다 — 이것을 빠뜨리면
	 * 새 매핑이 보이지 않는다. */
	__sysmmu_tlb_invalidate(data);
}

/*
 * [한국어]
 * __sysmmu_enable_clocks - 이 컨트롤러의 클럭을 모두 켠다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 마스터 클럭부터 켜는 순서에 유의 — SYSMMU 레지스터가 마스터의
 * 클럭 도메인에 걸려 있어, 그것이 먼저 살아 있어야 한다.
 *
 * 실패를 BUG_ON으로 다루는 것은 다소 거친 선택이지만, 클럭을
 * 못 켜면 이후 레지스터 접근이 시스템을 멈추게 하므로
 * 조기에 드러내는 편이 낫다는 판단이다.
 *
 * 실행 컨텍스트: enable 경로와 버전 조회.
 *
 * 호출 체인:
 *   __sysmmu_enable() / __sysmmu_get_version() → [__sysmmu_enable_clocks]
 */
static void __sysmmu_enable_clocks(struct sysmmu_drvdata *data)
{
	/* [한국어] 마스터 클럭을 먼저 켠다 — SYSMMU 레지스터가 그
	 * 클럭 도메인에 있기 때문이다. */
	BUG_ON(clk_prepare_enable(data->clk_master));
	/* [한국어] SYSMMU의 주 클럭. */
	BUG_ON(clk_prepare_enable(data->clk));
	/* [한국어] 주변장치 클럭(clk 대신 쓰는 SoC가 있다). */
	BUG_ON(clk_prepare_enable(data->pclk));
	/* [한국어] 버스 클럭. */
	BUG_ON(clk_prepare_enable(data->aclk));
}

/*
 * [한국어]
 * __sysmmu_disable_clocks - 이 컨트롤러의 클럭을 모두 끈다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 켠 것의 정확한 역순이다 — 마스터 클럭을 마지막에 끄는 것이
 * 중요한데, 다른 클럭을 끄는 동작 자체가 레지스터 접근을
 * 동반할 수 있기 때문이다.
 *
 * 실행 컨텍스트: disable 경로와 버전 조회.
 *
 * 호출 체인:
 *   __sysmmu_disable() / __sysmmu_get_version() → [__sysmmu_disable_clocks]
 */
static void __sysmmu_disable_clocks(struct sysmmu_drvdata *data)
{
	/* [한국어] 켠 순서의 역순으로 끈다. */
	clk_disable_unprepare(data->aclk);
	clk_disable_unprepare(data->pclk);	/* [한국어] 주변장치 클럭을 내린다. */
	clk_disable_unprepare(data->clk);
	/* [한국어] 마스터 클럭을 마지막에 끈다. */
	clk_disable_unprepare(data->clk_master);
}

/*
 * [한국어]
 * __sysmmu_has_capa1 - CAPA1 레지스터가 존재하는지 확인한다
 *
 * @data: 대상 컨트롤러.
 * @return: 존재하면 참.
 *
 * v7의 능력 조회는 두 단계다. CAPA0에 "CAPA1이 있다"는 비트가
 * 있어야 CAPA1을 읽을 수 있다. 없는데 읽으면 정의되지 않은
 * 동작이므로 반드시 먼저 확인한다.
 *
 * 실행 컨텍스트: probe 중 버전 조회. 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   __sysmmu_get_version() → [__sysmmu_has_capa1]
 */
static bool __sysmmu_has_capa1(struct sysmmu_drvdata *data)
{
	/* [한국어] 첫 능력 레지스터를 읽는다. */
	u32 capa0 = readl(data->sfrbase + REG_V7_CAPA0);

	/* [한국어] 이 비트가 서 있어야 CAPA1을 읽어도 된다. */
	return capa0 & CAPA0_CAPA1_EXIST;
}

/*
 * [한국어]
 * __sysmmu_get_vcr - 가상화 제어 레지스터의 유무를 알아낸다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음(data->has_vcr에 결과를 남긴다).
 *
 * 이 값이 v7의 레지스터 배치를 두 갈래로 가른다. 가상화를
 * 지원하면 무효화와 테이블 주소 레지스터가 0x8000 대역으로
 * 옮겨져 있어 다른 variant를 써야 한다.
 *
 * 실행 컨텍스트: probe 중 버전 조회. CAPA1이 있음이 확인된 뒤.
 *
 * 호출 체인:
 *   __sysmmu_get_version() → [__sysmmu_get_vcr]
 */
static void __sysmmu_get_vcr(struct sysmmu_drvdata *data)
{
	/* [한국어] 둘째 능력 레지스터를 읽는다. */
	u32 capa1 = readl(data->sfrbase + REG_V7_CAPA1);

	/* [한국어] 가상화가 활성이면 레지스터 배치가 통째로 다르다. */
	data->has_vcr = capa1 & CAPA1_VCR_ENABLED;
}

/*
 * [한국어]
 * __sysmmu_get_version - 하드웨어 버전을 읽고 variant를 고른다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * probe에서 딱 한 번 불린다. 레지스터를 읽어야 하므로 클럭을
 * 켰다가 끄는 것이 이 함수가 스스로 하는 일이다.
 *
 * 버전이 0x80000001인 경우를 특별 취급하는 점이 눈에 띈다.
 * 일부 SoC의 컨트롤러가 버전을 제대로 보고하지 않아, 그 값을
 * v1.0으로 간주하는 것이다.
 *
 * v7에서는 능력 조회까지 이어져 네 가지 variant 중 하나를 고른다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   exynos_sysmmu_probe() → [__sysmmu_get_version]
 */
static void __sysmmu_get_version(struct sysmmu_drvdata *data)
{
	/* [한국어] 버전 레지스터의 원시 값. */
	u32 ver;

	/* [한국어] 레지스터를 읽으려면 클럭이 필요하다. */
	__sysmmu_enable_clocks(data);

	ver = readl(data->sfrbase + REG_MMU_VERSION);	/* [한국어] 하드웨어 버전을 읽는다. */

	/* controllers on some SoCs don't report proper version */
	/* [한국어] 버전을 제대로 보고하지 않는 컨트롤러가 있다.
	 * 이 특정 값이 나오면 가장 오래된 v1.0으로 간주한다. */
	if (ver == 0x80000001u)
		data->version = MAKE_MMU_VER(1, 0);
	else
		data->version = MMU_RAW_VER(ver);

	dev_dbg(data->sysmmu, "hardware version: %d.%d\n",	/* [한국어] 확정된 버전을 디버그 로그로 남긴다. */
		MMU_MAJ_VER(data->version), MMU_MIN_VER(data->version));

	/* [한국어] 버전에 맞는 레지스터 배치를 고른다. */
	if (MMU_MAJ_VER(data->version) < 5) {
		data->variant = &sysmmu_v1_variant;	/* [한국어] v1~v3의 레지스터 배치를 쓴다. */
	} else if (MMU_MAJ_VER(data->version) < 7) {	/* [한국어] v5와 v6은 같은 배치다. */
		data->variant = &sysmmu_v5_variant;	/* [한국어] v5 계열의 레지스터 배치를 쓴다. */
	} else {
		/* [한국어] v7은 능력 레지스터를 더 읽어야 배치를 알 수 있다.
		 * CAPA1이 있는지 먼저 확인해야 안전하게 읽을 수 있다. */
		if (__sysmmu_has_capa1(data))
			__sysmmu_get_vcr(data);
		/* [한국어] 가상화 지원 여부가 배치를 가른다. */
		if (data->has_vcr)
			data->variant = &sysmmu_v7_vm_variant;
		else
			data->variant = &sysmmu_v7_variant;
	}

	/* [한국어] 조회가 끝났으니 클럭을 내린다. */
	__sysmmu_disable_clocks(data);
}

/*
 * [한국어]
 * show_fault_information - 폴트 상황을 테이블까지 되짚어 찍는다
 *
 * @data: 폴트를 낸 컨트롤러.
 * @fault: 해석된 폴트 정보.
 * @return: 없음.
 *
 * 오류 한 줄과, 디버그 빌드에서는 테이블을 되짚은 결과를 남긴다.
 * 1단계 엔트리 값만으로도 "이 1MB 영역에 매핑이 있었는가"를
 * 알 수 있고, 2단계까지 내려가면 그 페이지의 상태와 권한 비트가
 * 보인다 — 폴트 원인을 좁히는 데 그 둘이면 충분하다.
 *
 * data->pgtable을 출발점으로 삼는 점에 유의. 도메인이 아니라
 * **이 컨트롤러에 실제로 설정된** 테이블을 본다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   exynos_sysmmu_irq() → [show_fault_information]
 */
static void show_fault_information(struct sysmmu_drvdata *data,
				   const struct sysmmu_fault *fault)
{
	/* [한국어] 되짚어 읽을 엔트리. */
	sysmmu_pte_t *ent;

	/* [한국어] 어느 마스터가 어느 방향으로 어디서 폴트를 냈는지
	 * 한 줄로 남긴다. */
	dev_err(data->sysmmu, "%s: [%s] %s FAULT occurred at %#x\n",
		dev_name(data->master),
		fault->type == IOMMU_FAULT_READ ? "READ" : "WRITE",
		fault->name, fault->addr);
	dev_dbg(data->sysmmu, "Page table base: %pa\n", &data->pgtable);
	/* [한국어] 이 컨트롤러에 설정된 테이블에서 해당 1단계 엔트리를 찾는다. */
	ent = section_entry(phys_to_virt(data->pgtable), fault->addr);
	dev_dbg(data->sysmmu, "\tLv1 entry: %#x\n", *ent);
	/* [한국어] 2단계 테이블이 있다면 그 엔트리까지 보여 준다 —
	 * 권한 비트를 확인하려면 여기까지 내려가야 한다. */
	if (lv1ent_page(ent)) {
		ent = page_entry(ent, fault->addr);	/* [한국어] 2단계 엔트리로 내려간다 — 권한 비트가 여기 있다. */
		dev_dbg(data->sysmmu, "\t Lv2 entry: %#x\n", *ent);	/* [한국어] 그 엔트리 값을 남긴다. */
	}
}

/*
 * [한국어]
 * exynos_sysmmu_irq - 폴트 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev_id: 등록 시 넘긴 컨트롤러.
 * @return: 항상 IRQ_HANDLED.
 *
 * **폴트를 복구하지 못하면 패닉한다**는 것이 이 드라이버의
 * 강경한 정책이다. 상위 핸들러가 등록되어 있지 않거나 그것이
 * 실패하면 시스템을 멈춘다. 멀티미디어 블록의 잘못된 DMA가
 * 조용히 계속되면 더 나쁜 결과를 낳는다는 판단이다.
 *
 * 인터럽트가 나면 하드웨어가 스스로 정지 상태로 들어가므로,
 * 처리 후 반드시 unblock 해야 마스터가 다시 움직인다.
 * 원본 주석이 그 사실을 밝힌다.
 *
 * itype을 __ffs로 구하는 점에 유의 — 여러 폴트가 동시에 섰다면
 * 가장 낮은 비트 하나만 처리하고, 나머지는 다음 인터럽트로 넘긴다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. data->lock을 잡는다.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [exynos_sysmmu_irq]
 *   → variant->get_fault_info() → report_iommu_fault()
 */
static irqreturn_t exynos_sysmmu_irq(int irq, void *dev_id)
{
	/* [한국어] 폴트를 낸 컨트롤러. */
	struct sysmmu_drvdata *data = dev_id;
	/* [한국어] 인터럽트 상태에서 처음 선 비트의 번호. */
	unsigned int itype;
	/* [한국어] 해석된 폴트 정보. */
	struct sysmmu_fault fault;
	/* [한국어] 처리 결과. 아무도 처리하지 않으면 이 값이 남아
	 * 아래에서 패닉으로 이어진다. */
	int ret = -ENOSYS;

	/* [한국어] 꺼진 SYSMMU가 인터럽트를 냈다면 앞뒤가 맞지 않는다. */
	WARN_ON(!data->active);

	/* [한국어] 레지스터 접근과 상태 변경을 직렬화한다. */
	spin_lock(&data->lock);
	/* [한국어] 레지스터를 읽으려면 마스터 클럭이 필요하다. */
	clk_enable(data->clk_master);

	/* [한국어] 가장 낮은 폴트 비트 하나만 처리한다. 여러 개가
	 * 섰다면 나머지는 다음 인터럽트로 넘어간다. */
	itype = __ffs(readl(SYSMMU_REG(data, int_status)));
	ret = data->variant->get_fault_info(data, itype, &fault);	/* [한국어] 비트 번호를 세대에 맞게 해석해 폴트 정보로 만든다. */
	if (ret) {
		/* [한국어] 이 세대가 모르는 비트다 — 해석할 수 없으니
		 * 비트만 지우고 나간다. */
		dev_err(data->sysmmu, "Unhandled interrupt bit %u\n", itype);
		goto out;	/* [한국어] 해석할 수 없으니 비트만 지우고 나간다. */
	}
	/* [한국어] 테이블까지 되짚어 진단 정보를 남긴다. */
	show_fault_information(data, &fault);

	/* [한국어] 도메인에 붙어 있으면 상위 핸들러에 보고한다.
	 * 처리되면 ret이 0이 되어 아래 패닉을 피한다. */
	if (data->domain) {
		ret = report_iommu_fault(&data->domain->domain, data->master,	/* [한국어] 상위 핸들러가 복구를 시도하게 한다. */
					 fault.addr, fault.type);
	}
	/* [한국어] 아무도 이 폴트를 처리하지 못했다. 잘못된 DMA가
	 * 계속되게 두느니 멈추는 편이 낫다는 강경한 정책이다. */
	if (ret)
		panic("Unrecoverable System MMU Fault!");

/* [한국어] 해석 실패와 정상 처리가 공유하는 정리 지점. */
out:
	/* [한국어] 처리한 비트만 지운다. */
	writel(1 << itype, SYSMMU_REG(data, int_clear));

	/* SysMMU is in blocked state when interrupt occurred */
	/* [한국어] 폴트가 나면 하드웨어가 스스로 멈춘다 — 풀어 주지
	 * 않으면 마스터가 영영 진행하지 못한다. */
	sysmmu_unblock(data);
	/* [한국어] 레지스터 작업이 끝났으니 마스터 클럭을 내린다. */
	clk_disable(data->clk_master);
	spin_unlock(&data->lock);	/* [한국어] 상태 변경 구간이 끝났다. */

	return IRQ_HANDLED;	/* [한국어] 이 SYSMMU 전용 인터럽트이므로 항상 우리 것이다. */
}

/*
 * [한국어]
 * __sysmmu_disable - 이 컨트롤러의 변환을 끄고 클럭을 내린다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 설정 레지스터까지 0으로 지우는 점에 유의. 다시 켤 때
 * __sysmmu_init_config가 전부 다시 채우므로, 꺼진 상태에
 * 옛 설정을 남겨 둘 이유가 없다.
 *
 * 실행 컨텍스트: detach와 런타임 서스펜드. 클럭이 켜진 상태에서
 * 들어와 꺼진 상태로 나간다.
 *
 * 호출 체인:
 *   identity_attach() / domain_free() / suspend() → [__sysmmu_disable]
 */
static void __sysmmu_disable(struct sysmmu_drvdata *data)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 레지스터를 만지려면 마스터 클럭이 필요하다. */
	clk_enable(data->clk_master);

	/* [한국어] 상태 변경 구간을 잠근다. */
	spin_lock_irqsave(&data->lock, flags);
	/* [한국어] 변환을 완전히 끈다. */
	writel(CTRL_DISABLE, data->sfrbase + REG_MMU_CTRL);
	/* [한국어] 설정도 지운다 — 다시 켤 때 전부 새로 쓴다. */
	writel(0, data->sfrbase + REG_MMU_CFG);
	/* [한국어] 무효화 함수들이 이 값을 보고 하드웨어를 건드리지
	 * 않게 된다. */
	data->active = false;
	spin_unlock_irqrestore(&data->lock, flags);

	/* [한국어] 마스터 클럭까지 포함해 모든 클럭을 내린다. */
	__sysmmu_disable_clocks(data);
}

/*
 * [한국어]
 * __sysmmu_init_config - 버전에 맞는 설정 값을 쓴다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 세대가 올라가며 기능이 바뀐 흔적이 그대로 드러난다.
 * v3.1까지는 LRU와 QoS뿐이고, v3.2는 FLPD 캐시와 SYSSEL이 더해지며,
 * v3.3부터는 LRU가 빠지고 자동 클럭 게이팅이 들어온다.
 *
 * QoS를 항상 최대(15)로 주는 것은 멀티미디어 블록이 실시간 마감을
 * 갖기 때문이다. EAP는 마지막에 항상 켜는데, 이것이 없으면
 * 엔트리의 권한 비트를 채워도 하드웨어가 검사하지 않는다.
 *
 * 실행 컨텍스트: enable 경로. 정지된 상태.
 *
 * 호출 체인:
 *   __sysmmu_enable() → [__sysmmu_init_config]
 */
static void __sysmmu_init_config(struct sysmmu_drvdata *data)
{
	/* [한국어] 조립할 설정 값. */
	unsigned int cfg;

	/* [한국어] v3.1까지 — 교체 정책과 QoS만 있다. */
	if (data->version <= MAKE_MMU_VER(3, 1))
		cfg = CFG_LRU | CFG_QOS(15);
	/* [한국어] v3.2 — 1단계 엔트리 캐시와 시스템 선택이 추가됐다. */
	else if (data->version <= MAKE_MMU_VER(3, 2))
		cfg = CFG_LRU | CFG_QOS(15) | CFG_FLPDCACHE | CFG_SYSSEL;
	else
		/* [한국어] v3.3 이상 — LRU 대신 자동 클럭 게이팅을 쓴다. */
		cfg = CFG_QOS(15) | CFG_FLPDCACHE | CFG_ACGEN;

	/* [한국어] 권한 비트를 실제로 검사하게 한다. 이것이 없으면
	 * LV1_PROT/LV2_PROT로 채운 비트가 전부 무시된다. */
	cfg |= CFG_EAP; /* enable access protection bits check */

	writel(cfg, data->sfrbase + REG_MMU_CFG);	/* [한국어] 조립한 설정을 하드웨어에 쓴다. */
}

/*
 * [한국어]
 * __sysmmu_enable_vid - v7의 가상 머신 모드를 켠다
 *
 * @data: 대상 컨트롤러.
 * @return: 없음.
 *
 * 가상화를 지원하는 v7에서만 의미가 있다. FAULT_MODE_STALL을 함께
 * 세우는 것이 중요한데, 그래야 폴트가 트랜잭션을 죽이지 않고
 * 멈춰 세워 소프트웨어가 처리한 뒤 재개시킬 수 있다.
 *
 * 실행 컨텍스트: enable 경로. 정지된 상태.
 *
 * 호출 체인:
 *   __sysmmu_enable() → [__sysmmu_enable_vid]
 */
static void __sysmmu_enable_vid(struct sysmmu_drvdata *data)
{
	/* [한국어] 현재 VM 제어 값. */
	u32 ctrl;

	/* [한국어] 이 기능이 없는 하드웨어에서는 아무것도 하지 않는다. */
	if (MMU_MAJ_VER(data->version) < 7 || !data->has_vcr)
		return;

	/* [한국어] 기존 값을 읽어 필요한 비트만 더한다. */
	ctrl = readl(data->sfrbase + REG_V7_CTRL_VM);
	/* [한국어] VM 모드를 켜고, 폴트 시 트랜잭션을 죽이는 대신
	 * 멈춰 세우게 한다 — 그래야 복구가 가능하다. */
	ctrl |= CTRL_VM_ENABLE | CTRL_VM_FAULT_MODE_STALL;
	writel(ctrl, data->sfrbase + REG_V7_CTRL_VM);	/* [한국어] VM 모드와 정지형 폴트 처리를 켠다. */
}

/*
 * [한국어]
 * __sysmmu_enable - 테이블을 걸고 변환을 켠다
 *
 * @data: 대상 컨트롤러(data->pgtable이 설정되어 있어야 한다).
 * @return: 없음.
 *
 * 순서가 전부다: 클럭을 켜고 → 정지 상태로 만들고 → 설정을 쓰고
 * → 테이블 주소를 심고 → VM 모드를 켜고 → 변환을 활성화한다.
 * 정지 상태에서 설정하는 이유는 변환이 도는 중에는 이 레지스터들이
 * 안전하게 바뀌지 않기 때문이다.
 *
 * 마지막에 마스터 클럭만 내리는 것이 이 드라이버의 전력 정책이다.
 * 원본 주석이 밝히듯, DMA 중의 클럭 유지는 클라이언트 드라이버의
 * 몫이라 SYSMMU가 계속 켜 둘 이유가 없다.
 *
 * 실행 컨텍스트: attach와 런타임 리줌. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   attach_device() / resume() → [__sysmmu_enable]
 *   → __sysmmu_init_config() → __sysmmu_set_ptbase()
 */
static void __sysmmu_enable(struct sysmmu_drvdata *data)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 모든 클럭을 켠다. */
	__sysmmu_enable_clocks(data);

	/* [한국어] 설정 구간을 잠근다. */
	spin_lock_irqsave(&data->lock, flags);
	/* [한국어] 먼저 정지 상태로 만든다 — 변환이 도는 중에는
	 * 아래 레지스터들을 안전하게 바꿀 수 없다. */
	writel(CTRL_BLOCK, data->sfrbase + REG_MMU_CTRL);
	/* [한국어] 버전에 맞는 설정을 쓴다. */
	__sysmmu_init_config(data);
	/* [한국어] 테이블 주소를 심고 TLB를 비운다. */
	__sysmmu_set_ptbase(data, data->pgtable);
	/* [한국어] 해당 하드웨어라면 VM 모드도 켠다. */
	__sysmmu_enable_vid(data);
	/* [한국어] 마지막으로 변환을 활성화한다. */
	writel(CTRL_ENABLE, data->sfrbase + REG_MMU_CTRL);
	/* [한국어] 이제 무효화 함수들이 이 하드웨어를 만져도 된다. */
	data->active = true;
	spin_unlock_irqrestore(&data->lock, flags);	/* [한국어] 상태 변경이 끝났으니 락을 놓는다. */

	/*
	 * SYSMMU driver keeps master's clock enabled only for the short
	 * time, while accessing the registers. For performing address
	 * translation during DMA transaction it relies on the client
	 * driver to enable it.
	 */
	/* [한국어] 마스터 클럭만 내린다. SYSMMU 자신의 클럭은 켜 둔 채로,
	 * DMA 중의 마스터 클럭 유지는 클라이언트 드라이버에 맡긴다. */
	clk_disable(data->clk_master);
}

/*
 * [한국어]
 * sysmmu_tlb_invalidate_flpdcache - 1단계 엔트리 캐시를 비운다
 *
 * @data: 대상 컨트롤러.
 * @iova: 비울 주소.
 * @return: 없음.
 *
 * v3.3의 FLPD 캐시 버그를 우회하는 함수다. 그 하드웨어는 1단계
 * 엔트리를 미리 읽어 캐시하는데, 더미 테이블(zero_lv2_table)을
 * 가리키던 엔트리를 진짜 테이블로 바꿔도 캐시가 옛 값을 계속
 * 내놓아 폴트가 난다. 그래서 교체할 때마다 이 함수를 부른다.
 *
 * v3.3 미만에서는 아무것도 하지 않는다 — 그 세대에는 이 캐시가
 * 없거나 문제를 일으키지 않기 때문이다.
 *
 * v5 이상에서 전체 무효화를 쓰는 이유는, 그 세대에서 항목 단위
 * 무효화가 FLPD 캐시까지 비우지 않기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. domain->lock을 잡은 채 불린다.
 *
 * 호출 체인:
 *   alloc_lv2entry() / lv1set_section()
 *   → [sysmmu_tlb_invalidate_flpdcache]
 */
static void sysmmu_tlb_invalidate_flpdcache(struct sysmmu_drvdata *data,
					    sysmmu_iova_t iova)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 이 컨트롤러의 상태와 레지스터를 보호한다. */
	spin_lock_irqsave(&data->lock, flags);
	/* [한국어] 켜져 있고, 문제의 캐시를 가진 세대일 때만 처리한다. */
	if (data->active && data->version >= MAKE_MMU_VER(3, 3)) {
		/* [한국어] 레지스터 접근에는 마스터 클럭이 필요하다. */
		clk_enable(data->clk_master);
		/* [한국어] 무효화 전에 변환을 멈춘다. 멈추지 못하면
		 * 무효화를 건너뛴다 — 잘못 건드리느니 낫다. */
		if (sysmmu_block(data)) {
			/* [한국어] v5 이상은 항목 무효화가 FLPD 캐시까지
			 * 비우지 않아 전체를 비워야 한다. */
			if (data->version >= MAKE_MMU_VER(5, 0))
				__sysmmu_tlb_invalidate(data);
			else
				/* [한국어] v3.3~v4는 해당 항목 하나면 충분하다. */
				__sysmmu_tlb_invalidate_entry(data, iova, 1);
			sysmmu_unblock(data);	/* [한국어] 무효화가 끝났으니 변환을 재개시킨다. */
		}
		clk_disable(data->clk_master);	/* [한국어] 레지스터 작업이 끝났으니 마스터 클럭을 내린다. */
	}
	spin_unlock_irqrestore(&data->lock, flags);	/* [한국어] 락을 놓는다. */
}

/*
 * [한국어]
 * sysmmu_tlb_invalidate_entry - 해제된 범위의 TLB 항목을 비운다
 *
 * @data: 대상 컨트롤러.
 * @iova: 비울 시작 주소.
 * @size: 비울 크기.
 * @return: 없음.
 *
 * 무효화 개수 계산이 흥미롭다. v2에서는 원본 주석이 설명하듯
 * L2 TLB가 8-way 64-set 집합 연관 구조라, 큰 매핑 하나가 여러
 * 세트에 걸쳐 캐시될 수 있다. 1MB 매핑은 어느 세트에나 들어갈
 * 수 있으므로 64개(전체 세트 수)를 비워야 하고, 64KB는 연속된
 * 16세트에 들어갈 수 있다. 그래서 크기를 페이지 수로 환산하되
 * 64로 상한을 둔다.
 *
 * 다른 세대에서는 1개면 충분하다 — 그 세대의 TLB는 매핑 단위로
 * 항목을 갖기 때문이다.
 *
 * 실행 컨텍스트: 해제 경로.
 *
 * 호출 체인:
 *   exynos_iommu_tlb_invalidate_entry() → [sysmmu_tlb_invalidate_entry]
 */
static void sysmmu_tlb_invalidate_entry(struct sysmmu_drvdata *data,
					sysmmu_iova_t iova, size_t size)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 상태와 레지스터를 보호한다. */
	spin_lock_irqsave(&data->lock, flags);
	/* [한국어] 꺼진 하드웨어는 만지지 않는다 — 다시 켜질 때
	 * TLB가 어차피 비워진다. */
	if (data->active) {
		/* [한국어] 비울 항목의 수. 기본은 하나다. */
		unsigned int num_inv = 1;

		/* [한국어] 레지스터 접근에 필요한 마스터 클럭. */
		clk_enable(data->clk_master);

		/*
		 * L2TLB invalidation required
		 * 4KB page: 1 invalidation
		 * 64KB page: 16 invalidations
		 * 1MB page: 64 invalidations
		 * because it is set-associative TLB
		 * with 8-way and 64 sets.
		 * 1MB page can be cached in one of all sets.
		 * 64KB page can be one of 16 consecutive sets.
		 */
		/* [한국어] v2의 L2 TLB는 집합 연관 구조라 큰 매핑이 여러
		 * 세트에 흩어질 수 있다. 크기를 페이지 수로 환산하되
		 * 전체 세트 수(64)를 넘지 않게 자른다. */
		if (MMU_MAJ_VER(data->version) == 2)
			num_inv = min_t(unsigned int, size / SPAGE_SIZE, 64);

		/* [한국어] 무효화 전에 변환을 멈춘다. */
		if (sysmmu_block(data)) {
			__sysmmu_tlb_invalidate_entry(data, iova, num_inv);	/* [한국어] 계산한 개수만큼 항목을 비운다. */
			sysmmu_unblock(data);	/* [한국어] 무효화가 끝났으니 변환을 재개시킨다. */
		}
		clk_disable(data->clk_master);	/* [한국어] 마스터 클럭을 내린다. */
	}
	spin_unlock_irqrestore(&data->lock, flags);	/* [한국어] 락을 놓는다. */
}

/* [한국어] 연산 테이블의 전방 선언. probe가 정의보다 앞에서 참조한다. */
static const struct iommu_ops exynos_iommu_ops;

/*
 * [한국어]
 * exynos_sysmmu_probe - SYSMMU 컨트롤러 하나를 초기화한다
 *
 * @pdev: 플랫폼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 순서에 이유가 있다.
 *
 *  1) 레지스터와 인터럽트, 클럭을 확보한다. 클럭은 두 가지 구성이
 *     허용되어(clk 하나, 또는 aclk+pclk 쌍) 그것을 검사한다.
 *  2) **버전을 읽는다.** 이 함수가 클럭을 스스로 켰다 끈다.
 *  3) 첫 컨트롤러라면 전역 세대 설정(PG_ENT_SHIFT와 권한 표)을
 *     확정한다. 시스템의 모든 SYSMMU가 같은 세대라는 전제다.
 *  4) v5 이상이면 36비트 DMA 마스크를 설정한다 — 테이블 페이지가
 *     그 범위에 놓이게 하려는 것이다.
 *  5) 첫 컨트롤러를 테이블 캐시 관리 대행 디바이스로 등록한다.
 *  6) 런타임 PM을 켠 뒤 코어에 등록한다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [exynos_sysmmu_probe] → __sysmmu_get_version()
 *   → iommu_device_register()
 */
static int exynos_sysmmu_probe(struct platform_device *pdev)
{
	/* [한국어] 인터럽트 번호와 단계별 결과. */
	int irq, ret;
	/* [한국어] 로그와 devm의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 만들 컨트롤러 상태. */
	struct sysmmu_drvdata *data;
	/* [한국어] 레지스터 자원. */
	struct resource *res;

	/* [한국어] 디바이스 수명에 묶어 상태를 잡는다. */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)	/* [한국어] 상태 구조체를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 첫 메모리 자원이 레지스터 영역이다. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->sfrbase = devm_ioremap_resource(dev, res);	/* [한국어] 레지스터 영역을 매핑한다. */
	if (IS_ERR(data->sfrbase))	/* [한국어] 매핑에 실패했다. */
		return PTR_ERR(data->sfrbase);

	/* [한국어] 폴트 인터럽트를 얻는다. */
	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)	/* [한국어] 인터럽트를 얻지 못했다. */
		return irq;

	/* [한국어] 폴트 핸들러를 등록한다. 공유 플래그가 없는 것은
	 * SYSMMU마다 전용 인터럽트를 갖기 때문이다. */
	ret = devm_request_irq(dev, irq, exynos_sysmmu_irq, 0,
				dev_name(dev), data);
	if (ret) {	/* [한국어] 핸들러 등록에 실패한 경우. */
		dev_err(dev, "Unable to register handler of irq %d\n", irq);	/* [한국어] 어느 인터럽트였는지 남긴다. */
		return ret;	/* [한국어] 실패 이유를 그대로 전한다. */
	}

	/* [한국어] 주 클럭. 선택 사항이라 없어도 오류가 아니다. */
	data->clk = devm_clk_get_optional(dev, "sysmmu");
	if (IS_ERR(data->clk))	/* [한국어] 클럭 조회 자체가 실패했다(없는 것과 다르다). */
		return PTR_ERR(data->clk);

	/* [한국어] 버스 클럭. clk 대신 쓰는 구성이 있다. */
	data->aclk = devm_clk_get_optional(dev, "aclk");
	if (IS_ERR(data->aclk))	/* [한국어] 버스 클럭 조회가 실패했다. */
		return PTR_ERR(data->aclk);

	/* [한국어] 주변장치 클럭. aclk와 짝을 이룬다. */
	data->pclk = devm_clk_get_optional(dev, "pclk");
	if (IS_ERR(data->pclk))	/* [한국어] 주변장치 클럭 조회가 실패했다. */
		return PTR_ERR(data->pclk);

	/* [한국어] 허용되는 구성은 두 가지뿐이다: clk 하나, 또는
	 * aclk와 pclk 쌍. 둘 다 아니면 클럭을 켤 수 없다. */
	if (!data->clk && (!data->aclk || !data->pclk)) {
		dev_err(dev, "Failed to get device clock(s)!\n");	/* [한국어] 허용되는 두 구성 중 어느 쪽도 아니다. */
		return -ENOSYS;	/* [한국어] 클럭을 켤 수 없으면 이 컨트롤러를 쓸 수 없다. */
	}

	/* [한국어] 마스터의 클럭. SYSMMU 레지스터가 그 클럭 도메인에
	 * 있어 레지스터 접근마다 필요하다. */
	data->clk_master = devm_clk_get_optional(dev, "master");
	if (IS_ERR(data->clk_master))	/* [한국어] 마스터 클럭 조회가 실패했다. */
		return PTR_ERR(data->clk_master);

	/* [한국어] 로그와 PM의 기준 디바이스. */
	data->sysmmu = dev;
	/* [한국어] 상태와 레지스터 접근을 보호할 락. */
	spin_lock_init(&data->lock);

	/* [한국어] 버전을 읽고 variant를 고른다. 이 안에서 클럭을
	 * 켰다 끄므로 위의 클럭 확보가 먼저여야 한다. */
	__sysmmu_get_version(data);

	/* [한국어] sysfs에 IOMMU 인스턴스를 노출한다. */
	ret = iommu_device_sysfs_add(&data->iommu, &pdev->dev, NULL,
				     dev_name(data->sysmmu));
	if (ret)	/* [한국어] sysfs 등록에 실패했다. */
		return ret;

	/* [한국어] of_xlate가 이 값으로 컨트롤러를 찾으므로 일찍 설정한다. */
	platform_set_drvdata(pdev, data);

	/* [한국어] 첫 컨트롤러가 시스템 전체의 세대 설정을 확정한다.
	 * 이후 컨트롤러들은 같은 세대라고 전제한다. */
	if (PG_ENT_SHIFT < 0) {
		if (MMU_MAJ_VER(data->version) < 5) {
			/* [한국어] 구세대 — 엔트리 값이 곧 물리 주소이고,
			 * 권한 표도 구형 비트 배치를 쓴다. */
			PG_ENT_SHIFT = SYSMMU_PG_ENT_SHIFT;
			LV1_PROT = SYSMMU_LV1_PROT;	/* [한국어] 구형 1단계 권한 비트 표를 꽂는다. */
			LV2_PROT = SYSMMU_LV2_PROT;	/* [한국어] 구형 2단계 권한 비트 표를 꽂는다. */
		} else {
			/* [한국어] v5 이상 — 엔트리를 4비트 밀어 36비트 주소를
			 * 표현하고, 권한도 2비트 필드로 정리된 표를 쓴다. */
			PG_ENT_SHIFT = SYSMMU_V5_PG_ENT_SHIFT;
			LV1_PROT = SYSMMU_V5_LV1_PROT;	/* [한국어] v5의 1단계 권한 표를 꽂는다. */
			LV2_PROT = SYSMMU_V5_LV2_PROT;	/* [한국어] v5의 2단계 권한 표를 꽂는다. */
		}
	}

	/* [한국어] v5 이상은 36비트 물리 주소를 다룰 수 있다.
	 * 테이블 페이지 할당이 그 범위를 쓰도록 DMA 계층에 알린다. */
	if (MMU_MAJ_VER(data->version) >= 5) {
		ret = dma_set_mask(dev, DMA_BIT_MASK(36));	/* [한국어] 테이블 페이지가 36비트 범위를 쓸 수 있게 한다. */
		if (ret) {	/* [한국어] 마스크 설정이 거부된 경우. */
			dev_err(dev, "Unable to set DMA mask: %d\n", ret);	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
			goto err_dma_set_mask;	/* [한국어] sysfs 등록을 되돌리러 간다. */
		}
	}

	/*
	 * use the first registered sysmmu device for performing
	 * dma mapping operations on iommu page tables (cpu cache flush)
	 */
	/* [한국어] 테이블 캐시 관리를 대행할 디바이스로 등록한다.
	 * 어느 것이든 상관없는 이유는 dma_sync가 캐시 라인을 다루는
	 * 일이라 디바이스에 따라 결과가 달라지지 않기 때문이다. */
	if (!dma_dev)
		dma_dev = &pdev->dev;

	/* [한국어] 런타임 PM을 켠다. 실제 전원 전환은 마스터와의
	 * device link가 이끈다. */
	pm_runtime_enable(dev);

	/* [한국어] 코어에 등록한다. 이 순간부터 마스터들의
	 * of_xlate와 probe_device가 불리기 시작한다. */
	ret = iommu_device_register(&data->iommu, &exynos_iommu_ops, dev);
	if (ret)	/* [한국어] 코어 등록에 실패했다. */
		goto err_dma_set_mask;

	return 0;

/* [한국어] sysfs 등록 이후의 실패 — 그것을 되돌린다.
 * DMA 마스크와 런타임 PM은 devm/디바이스 해제가 정리한다. */
err_dma_set_mask:
	iommu_device_sysfs_remove(&data->iommu);	/* [한국어] 등록해 둔 sysfs 항목을 지운다. */
	return ret;	/* [한국어] 실패 이유를 플랫폼 코어에 전한다. */
}

/*
 * [한국어]
 * exynos_sysmmu_suspend - 런타임 서스펜드. 하드웨어를 끈다
 *
 * @dev: 대상 SYSMMU 디바이스.
 * @return: 항상 0.
 *
 * 마스터가 잠들면 device link를 통해 이 SYSMMU도 잠든다.
 * 그때 하드웨어 설정이 사라지므로 미리 깨끗하게 꺼 둔다.
 *
 * 소프트웨어 상태(data->pgtable, data->domain)는 그대로 두는 것이
 * 핵심이다. resume이 그것을 근거로 전부 복원한다.
 *
 * rpm_lock을 잡는 이유: attach가 하드웨어를 켜는 도중에 이 콜백이
 * 끼어들면 반쯤 설정된 상태가 남는다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [exynos_sysmmu_suspend] → __sysmmu_disable()
 */
static int __maybe_unused exynos_sysmmu_suspend(struct device *dev)
{
	/* [한국어] 대상 컨트롤러. */
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	/* [한국어] 이 컨트롤러가 담당하는 마스터. */
	struct device *master = data->master;

	/* [한국어] 마스터가 아직 연결되지 않았다면 켠 것도 없다. */
	if (master) {
		/* [한국어] 마스터의 소유자 구조체 — 락이 여기 있다. */
		struct exynos_iommu_owner *owner = dev_iommu_priv_get(master);

		/* [한국어] attach/detach와 겹치지 않도록 잠근다. */
		mutex_lock(&owner->rpm_lock);
		/* [한국어] 도메인에 붙어 있을 때만 끌 것이 있다. */
		if (data->domain) {
			dev_dbg(data->sysmmu, "saving state\n");
			/* [한국어] 소프트웨어 상태는 남기고 하드웨어만 끈다. */
			__sysmmu_disable(data);
		}
		mutex_unlock(&owner->rpm_lock);	/* [한국어] 전환 구간이 끝났으니 뮤텍스를 놓는다. */
	}
	return 0;	/* [한국어] 서스펜드는 실패하지 않는다. */
}

/*
 * [한국어]
 * exynos_sysmmu_resume - 런타임 리줌. 하드웨어를 되살린다
 *
 * @dev: 대상 SYSMMU 디바이스.
 * @return: 항상 0.
 *
 * suspend의 대칭이자, **attach가 전원 꺼짐 때문에 미뤄 둔 설정을
 * 대신 수행하는 자리**이기도 하다. attach는 pm_runtime_active()가
 * 거짓이면 하드웨어를 건드리지 않고 장부만 고치는데, 그 뒤 처음
 * 깨어날 때 이 콜백이 data->pgtable을 보고 실제로 설정한다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [exynos_sysmmu_resume] → __sysmmu_enable()
 */
static int __maybe_unused exynos_sysmmu_resume(struct device *dev)
{
	/* [한국어] 대상 컨트롤러. */
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	/* [한국어] 담당 마스터. */
	struct device *master = data->master;

	/* [한국어] 마스터가 없으면 되살릴 상태도 없다. */
	if (master) {
		/* [한국어] 마스터의 소유자 구조체. */
		struct exynos_iommu_owner *owner = dev_iommu_priv_get(master);

		/* [한국어] attach와 겹치지 않도록 잠근다. */
		mutex_lock(&owner->rpm_lock);
		/* [한국어] 붙어 있는 도메인이 있으면 그 테이블로 되살린다. */
		if (data->domain) {
			dev_dbg(data->sysmmu, "restoring state\n");	/* [한국어] 되살리기 시작을 기록한다. */
			__sysmmu_enable(data);	/* [한국어] 장부에 적힌 테이블로 하드웨어를 되살린다. */
		}
		mutex_unlock(&owner->rpm_lock);	/* [한국어] 전환 구간이 끝났으니 뮤텍스를 놓는다. */
	}
	return 0;	/* [한국어] 리줌 결과를 성공으로 보고한다. */
}

/* [한국어] 전원 관리 콜백 묶음.
 * 시스템 절전은 런타임 PM을 강제로 오가게 하는 표준 헬퍼에
 * 맡긴다 — 두 경로에서 해야 할 일이 같기 때문이다. */
static const struct dev_pm_ops sysmmu_pm_ops = {
	SET_RUNTIME_PM_OPS(exynos_sysmmu_suspend, exynos_sysmmu_resume, NULL)	/* [한국어] 런타임 전원 전환에 위 두 함수를 연결한다(유휴 콜백은 없다). */
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

/* [한국어] 이 드라이버가 붙을 디바이스 트리 노드.
 * 세대 구분이 없는 것은 버전을 레지스터에서 읽기 때문이다. */
static const struct of_device_id sysmmu_of_match[] = {
	{ .compatible	= "samsung,exynos-sysmmu", },	/* [한국어] 이 드라이버가 붙을 유일한 호환 문자열. */
	{ },
};

/* [한국어] SYSMMU 컨트롤러의 플랫폼 드라이버.
 * __refdata는 이 구조체가 __init 함수를 참조할 수 있음을 알려
 * 섹션 불일치 경고를 막는 표시다. */
static struct platform_driver exynos_sysmmu_driver __refdata = {
	.probe	= exynos_sysmmu_probe,
	/* [한국어] 컨트롤러 초기화 진입점. */

	.driver	= {
		.name		= "exynos-sysmmu",
		/* [한국어] 드라이버 이름. */

		.of_match_table	= sysmmu_of_match,
		/* [한국어] 붙을 노드의 호환 문자열. */

		.pm		= &sysmmu_pm_ops,
		/* [한국어] 런타임/시스템 전원 관리 콜백. */

		.suppress_bind_attrs = true,
		/* [한국어] sysfs로 바인딩을 풀 수 없게 막는다.
		 * SYSMMU를 임의로 떼어 내면 마스터의 DMA가 갑자기
		 * 변환 없이 나가 시스템 메모리를 덮어쓸 수 있다. */
	}
};

/*
 * [한국어]
 * exynos_iommu_set_pte - 엔트리 하나를 고치고 하드웨어에 반영한다
 *
 * @ent: 고칠 엔트리의 주소.
 * @val: 써 넣을 값.
 * @return: 없음.
 *
 * 이 드라이버에서 테이블을 고치는 유일하고 올바른 방법이다.
 * SYSMMU는 테이블을 캐시 일관성 없이 DMA로 읽으므로, CPU가 값을
 * 바꾸는 것만으로는 하드웨어가 보지 못한다.
 *
 * 앞뒤로 sync를 감싸는 이유가 각각 다르다.
 *  - for_cpu: 이 캐시 라인에 대한 이전 DMA 관점을 무효화해,
 *    CPU가 쓰는 값이 나중에 덮이지 않게 한다.
 *  - for_device: 방금 쓴 값을 메모리로 밀어내 하드웨어가 보게 한다.
 *
 * 물리 주소를 DMA 주소처럼 넘기는 점에 유의 — 이 드라이버는
 * 둘이 같다고 전제하며, 도메인 생성에서 BUG_ON으로 확인한다.
 *
 * 실행 컨텍스트: 테이블 갱신 경로. pgtablelock을 잡은 상태.
 *
 * 호출 체인:
 *   alloc_lv2entry() / lv1set_section() / lv2set_page() / unmap
 *   → [exynos_iommu_set_pte]
 */
static inline void exynos_iommu_set_pte(sysmmu_pte_t *ent, sysmmu_pte_t val)
{
	/* [한국어] 이 캐시 라인의 소유권을 CPU로 가져온다. */
	dma_sync_single_for_cpu(dma_dev, virt_to_phys(ent), sizeof(*ent),
				DMA_TO_DEVICE);
	/* [한국어] 값을 리틀엔디언으로 써 넣는다. 하드웨어가 읽는
	 * 형식이 그렇기 때문이다. */
	*ent = cpu_to_le32(val);
	/* [한국어] 캐시를 메모리로 밀어내 하드웨어가 새 값을 보게 한다.
	 * 이것을 빠뜨리면 원인을 찾기 어려운 폴트가 난다. */
	dma_sync_single_for_device(dma_dev, virt_to_phys(ent), sizeof(*ent),
				   DMA_TO_DEVICE);
}

/*
 * [한국어]
 * exynos_iommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스(이 드라이버는 쓰지 않는다).
 * @return: 새 도메인, 실패하면 NULL.
 *
 * 두 가지 할당이 있다: 1단계 테이블 16KB와, 섹션별 빈 엔트리
 * 카운터 8KB다.
 *
 * **모든 1단계 엔트리를 ZERO_LV2LINK로 채우는 것**이 이 함수에서
 * 가장 특징적인 부분이다. 원본 주석이 v3.3의 1MB 매핑 캐싱을
 * 막기 위한 우회라고 밝히는데, 빈 자리를 0으로 두면 하드웨어가
 * "매핑 없음"을 캐시해 버리는 문제를 피하려는 것이다.
 *
 * DMA 주소와 물리 주소가 같은지 BUG_ON으로 확인하는 점도 중요하다.
 * 이 드라이버의 모든 엔트리 조작이 그 전제 위에 서 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [exynos_iommu_domain_alloc_paging]
 */
static struct iommu_domain *exynos_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 만들 도메인. */
	struct exynos_iommu_domain *domain;
	/* [한국어] 테이블의 DMA 주소(물리 주소와 같아야 한다). */
	dma_addr_t handle;
	/* [한국어] 1단계 엔트리 초기화 인덱스. */
	int i;

	/* Check if correct PTE offsets are initialized */
	/* [한국어] 컨트롤러가 하나도 probe되지 않았으면 세대 설정이
	 * 미정이라 엔트리를 만들 수 없다. 순서 위반은 버그다. */
	BUG_ON(PG_ENT_SHIFT < 0 || !dma_dev);

	/* [한국어] 도메인 뼈대를 0으로 초기화해 받는다. */
	domain = kzalloc_obj(*domain);
	if (!domain)	/* [한국어] 도메인 구조체를 잡지 못했다. */
		return NULL;

	/* [한국어] 1단계 테이블 16KB. 크기 지정 할당이라 정렬이
	 * 보장되고, 하드웨어가 요구하는 16KB 경계에 놓인다. */
	domain->pgtable = iommu_alloc_pages_sz(GFP_KERNEL, SZ_16K);
	if (!domain->pgtable)	/* [한국어] 1단계 테이블을 잡지 못했다. */
		goto err_pgtable;

	/* [한국어] 섹션마다 빈 2단계 엔트리 수를 세는 배열(4096 × 2바이트). */
	domain->lv2entcnt = iommu_alloc_pages_sz(GFP_KERNEL, SZ_8K);
	if (!domain->lv2entcnt)	/* [한국어] 카운터 배열을 잡지 못했다. */
		goto err_counter;

	/* Workaround for System MMU v3.3 to prevent caching 1MiB mapping */
	/* [한국어] 빈 자리를 0이 아니라 더미 테이블 링크로 채운다.
	 * 이것이 FLPD 캐시 버그 우회의 출발점이다 — 하드웨어가 미리
	 * 읽어도 유효한 링크를 보게 된다. */
	for (i = 0; i < NUM_LV1ENTRIES; i++)
		domain->pgtable[i] = ZERO_LV2LINK;

	/* [한국어] 테이블을 DMA 매핑해 이후 sync가 가능하게 한다. */
	handle = dma_map_single(dma_dev, domain->pgtable, LV1TABLE_SIZE,
				DMA_TO_DEVICE);
	/* For mapping page table entries we rely on dma == phys */
	/* [한국어] 이 드라이버의 모든 엔트리 조작이 "DMA 주소 == 물리
	 * 주소"를 전제한다. 그렇지 않은 시스템에서는 동작할 수 없다. */
	BUG_ON(handle != virt_to_phys(domain->pgtable));
	if (dma_mapping_error(dma_dev, handle))	/* [한국어] DMA 매핑에 실패하면 캐시를 관리할 수 없다. */
		goto err_lv2ent;

	/* [한국어] 클라이언트 목록을 보호할 락. */
	spin_lock_init(&domain->lock);
	/* [한국어] 테이블 갱신을 보호할 락. */
	spin_lock_init(&domain->pgtablelock);
	/* [한국어] 아직 아무 컨트롤러도 붙어 있지 않다. */
	INIT_LIST_HEAD(&domain->clients);

	/* [한국어] 세 가지 매핑 크기를 모두 지원한다고 알린다 —
	 * 1MB 섹션, 64KB 큰 페이지, 4KB 작은 페이지. */
	domain->domain.pgsize_bitmap = SECT_SIZE | LPAGE_SIZE | SPAGE_SIZE;

	/* [한국어] IOVA 공간은 0부터 시작한다. */
	domain->domain.geometry.aperture_start = 0;
	/* [한국어] 1단계 테이블 4096개 × 1MB = 4GB로 32비트 전체를 덮는다. */
	domain->domain.geometry.aperture_end   = ~0UL;
	/* [한국어] 코어가 그 범위를 벗어난 IOVA를 주지 않게 강제한다. */
	domain->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
	return &domain->domain;

/* [한국어] DMA 매핑 실패 — 카운터 배열부터 되돌린다. */
err_lv2ent:
	iommu_free_pages(domain->lv2entcnt);
/* [한국어] 카운터 배열 할당 실패 — 테이블을 반납한다. */
err_counter:
	iommu_free_pages(domain->pgtable);
/* [한국어] 테이블 할당 실패 — 도메인만 해제한다. */
err_pgtable:
	kfree(domain);	/* [한국어] 도메인 구조체를 해제한다. */
	return NULL;	/* [한국어] 생성 실패를 코어에 알린다. */
}

/*
 * [한국어]
 * exynos_iommu_domain_free - 도메인과 모든 2단계 테이블을 반납한다
 *
 * @iommu_domain: 해제할 도메인.
 * @return: 없음.
 *
 * 아직 붙어 있는 컨트롤러가 있으면 강제로 떼어 내는 방어 코드가
 * 앞에 있다. 정상 경로에서는 코어가 먼저 detach하므로 목록이
 * 비어 있어야 하고, WARN_ON이 그것을 확인한다.
 *
 * 1단계 테이블의 4096개 엔트리를 훑으며 매달린 2단계 테이블을
 * 모두 반납한다. lv1ent_page()가 더미 링크를 제외하므로,
 * 공유 zero_lv2_table을 실수로 해제하지 않는다 — 그 판별이
 * 여기서 결정적으로 중요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [exynos_iommu_domain_free]
 */
static void exynos_iommu_domain_free(struct iommu_domain *iommu_domain)
{
	/* [한국어] 해제할 도메인. */
	struct exynos_iommu_domain *domain = to_exynos_domain(iommu_domain);
	/* [한국어] 목록 순회 커서(안전 순회용 쌍). */
	struct sysmmu_drvdata *data, *next;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 1단계 엔트리 순회 인덱스. */
	int i;

	/* [한국어] 정상 경로라면 코어가 먼저 떼어 냈어야 한다. */
	WARN_ON(!list_empty(&domain->clients));

	/* [한국어] 목록 조작 구간을 잠근다. */
	spin_lock_irqsave(&domain->lock, flags);

	/* [한국어] 그래도 남아 있다면 강제로 정리한다 — 해제된
	 * 테이블을 가리키는 하드웨어를 남기지 않기 위한 방어다. */
	list_for_each_entry_safe(data, next, &domain->clients, domain_node) {
		spin_lock(&data->lock);
		/* [한국어] 하드웨어를 끈다. */
		__sysmmu_disable(data);
		/* [한국어] 테이블 주소와 도메인 연결을 지운다. */
		data->pgtable = 0;
		data->domain = NULL;	/* [한국어] 도메인 연결을 끊는다. */
		list_del_init(&data->domain_node);	/* [한국어] 클라이언트 목록에서 뺀다. */
		spin_unlock(&data->lock);	/* [한국어] 이 컨트롤러의 정리가 끝났다. */
	}

	spin_unlock_irqrestore(&domain->lock, flags);

	/* [한국어] 1단계 테이블의 DMA 매핑을 푼다. */
	dma_unmap_single(dma_dev, virt_to_phys(domain->pgtable), LV1TABLE_SIZE,
			 DMA_TO_DEVICE);

	/* [한국어] 매달린 2단계 테이블을 모두 반납한다. */
	for (i = 0; i < NUM_LV1ENTRIES; i++)
		/* [한국어] lv1ent_page는 더미 링크를 제외하므로, 공유
		 * zero_lv2_table을 실수로 해제하지 않는다. */
		if (lv1ent_page(domain->pgtable + i)) {
			/* [한국어] 엔트리에서 2단계 테이블의 주소를 복원한다. */
			phys_addr_t base = lv2table_base(domain->pgtable + i);

			/* [한국어] 만들 때의 DMA 매핑을 푼다. */
			dma_unmap_single(dma_dev, base, LV2TABLE_SIZE,
					 DMA_TO_DEVICE);
			/* [한국어] 슬랩 캐시에 반납한다. */
			kmem_cache_free(lv2table_kmem_cache,
					phys_to_virt(base));
		}

	/* [한국어] 1단계 테이블을 반납한다. */
	iommu_free_pages(domain->pgtable);
	/* [한국어] 카운터 배열을 반납한다. */
	iommu_free_pages(domain->lv2entcnt);
	kfree(domain);	/* [한국어] 도메인 구조체를 해제한다. */
}

/*
 * [한국어]
 * exynos_iommu_identity_attach - 마스터의 모든 SYSMMU를 도메인에서 떼어 낸다
 *
 * @identity_domain: 정적 identity 도메인.
 * @dev: 대상 마스터 디바이스.
 * @old: 직전 도메인(이 드라이버는 owner->domain을 쓴다).
 * @return: 항상 0.
 *
 * 이 하드웨어에 통과 모드가 없으므로, identity에 붙는다는 것은
 * **SYSMMU를 꺼 버리는 일**이다. 그래서 이 함수가 곧 detach이자,
 * attach의 첫 단계이기도 하다.
 *
 * 순서가 중요하다:
 *  1) 먼저 하드웨어를 끈다. **전원이 켜져 있을 때만** 만지는데,
 *     꺼져 있으면 이미 변환이 죽어 있어 안전하다.
 *     pm_runtime_get_noresume은 상태를 확인하는 동안 전원이
 *     바뀌지 않게 참조만 올리는 것이다 — 깨우지는 않는다.
 *  2) 그다음 소프트웨어 연결을 끊는다. 이 순서라야 끄는 동안
 *     data->domain이 유효해 PM 콜백이 올바르게 동작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스).
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) / exynos_iommu_attach_device()
 *   → [exynos_iommu_identity_attach] → __sysmmu_disable()
 */
static int exynos_iommu_identity_attach(struct iommu_domain *identity_domain,
					struct device *dev,
					struct iommu_domain *old)
{
	/* [한국어] 이 마스터의 SYSMMU 목록과 현재 도메인. */
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	/* [한국어] 떼어 낼 도메인. */
	struct exynos_iommu_domain *domain;
	/* [한국어] 로그에 남길 테이블 주소. */
	phys_addr_t pagetable;
	/* [한국어] 목록 순회 커서(안전 순회용 쌍). */
	struct sysmmu_drvdata *data, *next;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 이미 떼어진 상태면 할 일이 없다. */
	if (owner->domain == identity_domain)
		return 0;

	/* [한국어] 지금 붙어 있는 도메인과 그 테이블 주소. */
	domain = to_exynos_domain(owner->domain);
	pagetable = virt_to_phys(domain->pgtable);

	/* [한국어] PM 콜백과 겹치지 않도록 잠근다. */
	mutex_lock(&owner->rpm_lock);

	/* [한국어] 이 마스터의 모든 SYSMMU를 끈다. */
	list_for_each_entry(data, &owner->controllers, owner_node) {
		/* [한국어] 상태를 확인하는 동안 전원이 바뀌지 않게
		 * 참조만 올린다 — 잠든 하드웨어를 깨우지는 않는다. */
		pm_runtime_get_noresume(data->sysmmu);
		/* [한국어] 켜져 있을 때만 하드웨어를 만진다.
		 * 꺼져 있으면 이미 변환이 죽어 있어 안전하다. */
		if (pm_runtime_active(data->sysmmu))
			__sysmmu_disable(data);
		pm_runtime_put(data->sysmmu);	/* [한국어] 상태 확인이 끝났으니 참조를 놓는다. */
	}

	/* [한국어] 이제 소프트웨어 연결을 끊는다. */
	spin_lock_irqsave(&domain->lock, flags);
	list_for_each_entry_safe(data, next, &domain->clients, domain_node) {	/* [한국어] 클라이언트 목록을 순회하며 연결을 끊는다. */
		spin_lock(&data->lock);
		/* [한국어] 테이블 주소를 지운다 — 나중에 깨어나도
		 * 되살릴 것이 없게 된다. */
		data->pgtable = 0;
		data->domain = NULL;	/* [한국어] 도메인 연결을 끊는다. */
		list_del_init(&data->domain_node);	/* [한국어] 클라이언트 목록에서 뺀다. */
		spin_unlock(&data->lock);	/* [한국어] 이 컨트롤러의 정리가 끝났다. */
	}
	/* [한국어] 마스터의 현재 도메인을 identity로 기록한다. */
	owner->domain = identity_domain;
	spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 목록 조작이 끝났으니 락을 놓는다. */

	mutex_unlock(&owner->rpm_lock);	/* [한국어] 전환 구간이 끝났으니 뮤텍스를 놓는다. */

	dev_dbg(dev, "%s: Restored IOMMU to IDENTITY from pgtable %pa\n",	/* [한국어] 어느 테이블에서 떨어졌는지 남긴다. */
		__func__, &pagetable);
	return 0;	/* [한국어] 떼어 내기는 실패하지 않는다. */
}

/* [한국어] identity 도메인의 연산 테이블. 붙이기 하나뿐이며,
 * 그 붙이기가 사실은 "SYSMMU 끄기"다. */
static struct iommu_domain_ops exynos_identity_ops = {
	.attach_dev = exynos_iommu_identity_attach,
	/* [한국어] 이 도메인으로 옮길 때 부를 콜백. */
};

/* [한국어] "어느 도메인에도 붙어 있지 않음"을 나타내는 전역 도메인.
 * 앞에서 전방 선언된 그 변수이며, of_xlate가 마스터를 여기에
 * 놓고 시작한다. */
static struct iommu_domain exynos_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에 통과 모드로 알리는 종류 표시. */

	.ops = &exynos_identity_ops,
	/* [한국어] 위의 연산 테이블. */
};

/*
 * [한국어]
 * exynos_iommu_attach_device - 마스터의 모든 SYSMMU를 도메인에 붙인다
 *
 * @iommu_domain: 붙일 도메인.
 * @dev: 대상 마스터.
 * @old: 직전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * "먼저 완전히 떼고 다시 붙인다"는 순서를 지킨다. SYSMMU는 한
 * 테이블만 가리킬 수 있어 도메인 전환에 중간 상태가 있으면 안 된다.
 *
 * 그다음 순서도 의도적이다:
 *  1) 소프트웨어 연결을 **먼저** 만든다(테이블 주소, 도메인 포인터,
 *     클라이언트 목록). 이 시점부터 무효화가 이 컨트롤러에 전달된다.
 *  2) 그다음 하드웨어를 켠다. 전원이 꺼져 있으면 건드리지 않는데,
 *     나중에 resume이 data->pgtable을 보고 대신 켜 주기 때문이다.
 *
 * 이 지연 설정이 런타임 PM과 맞물린 이 드라이버의 핵심 패턴이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스).
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [exynos_iommu_attach_device]
 *   → exynos_iommu_identity_attach() → __sysmmu_enable()
 */
static int exynos_iommu_attach_device(struct iommu_domain *iommu_domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	/* [한국어] 붙일 도메인. */
	struct exynos_iommu_domain *domain = to_exynos_domain(iommu_domain);
	/* [한국어] 이 마스터의 SYSMMU 목록. */
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	/* [한국어] 목록 순회 커서. */
	struct sysmmu_drvdata *data;
	/* [한국어] 컨트롤러들에 심을 테이블의 물리 주소. */
	phys_addr_t pagetable = virt_to_phys(domain->pgtable);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 떼어 내기 결과. */
	int err;

	/* [한국어] 먼저 완전히 떼어 낸다 — 이 하드웨어는 테이블을
	 * 하나만 가리킬 수 있어 중간 상태를 허용하지 않는다. */
	err = exynos_iommu_identity_attach(&exynos_identity_domain, dev, old);
	if (err)	/* [한국어] 떼어 내지 못하면 새로 붙일 수도 없다. */
		return err;

	/* [한국어] PM 콜백과 겹치지 않도록 잠근다. */
	mutex_lock(&owner->rpm_lock);

	/* [한국어] 소프트웨어 연결을 먼저 만든다. */
	spin_lock_irqsave(&domain->lock, flags);
	list_for_each_entry(data, &owner->controllers, owner_node) {	/* [한국어] 이 마스터의 모든 컨트롤러에 같은 테이블을 심는다. */
		spin_lock(&data->lock);
		/* [한국어] 이 컨트롤러가 쓸 테이블 주소. resume이
		 * 이 값만 보고 하드웨어를 되살릴 수 있다. */
		data->pgtable = pagetable;
		data->domain = domain;
		/* [한국어] 도메인의 클라이언트 목록에 넣는다 —
		 * 이제부터 무효화가 이 컨트롤러에도 전달된다. */
		list_add_tail(&data->domain_node, &domain->clients);
		spin_unlock(&data->lock);	/* [한국어] 이 컨트롤러의 연결이 끝났다. */
	}
	/* [한국어] 마스터의 현재 도메인을 기록한다. */
	owner->domain = iommu_domain;
	spin_unlock_irqrestore(&domain->lock, flags);

	/* [한국어] 그다음 하드웨어를 켠다. */
	list_for_each_entry(data, &owner->controllers, owner_node) {
		/* [한국어] 상태 확인 동안 전원이 바뀌지 않게 참조만 올린다. */
		pm_runtime_get_noresume(data->sysmmu);
		/* [한국어] 켜져 있을 때만 지금 설정한다. 꺼져 있으면
		 * 나중에 resume이 data->pgtable을 보고 대신 켠다. */
		if (pm_runtime_active(data->sysmmu))
			__sysmmu_enable(data);
		pm_runtime_put(data->sysmmu);	/* [한국어] 상태 확인이 끝났으니 참조를 놓는다. */
	}

	mutex_unlock(&owner->rpm_lock);	/* [한국어] 전환 구간이 끝났으니 뮤텍스를 놓는다. */

	dev_dbg(dev, "%s: Attached IOMMU with pgtable %pa\n", __func__,	/* [한국어] 어느 테이블에 붙었는지 남긴다. */
		&pagetable);

	return 0;	/* [한국어] 붙이기가 끝났다. */
}

/*
 * [한국어]
 * alloc_lv2entry - 2단계 테이블을 확보하고 해당 엔트리의 주소를 얻는다
 *
 * @domain: 대상 도메인.
 * @sent: 1단계 엔트리의 주소.
 * @iova: 대상 IOVA.
 * @pgcounter: 이 섹션의 빈 엔트리 카운터.
 * @return: 2단계 엔트리의 주소, 실패하면 ERR_PTR.
 *
 * v3.3 FLPD 캐시 우회의 핵심이 여기 있다. 원본의 긴 주석이
 * 설명하는 상황은 이렇다.
 *
 *   하드웨어가 성능을 위해 2단계 엔트리를 미리 읽는다(prefetch).
 *   그 자리가 아직 더미 테이블을 가리키고 있으면, FLPD 캐시에
 *   **더미 테이블의 주소가 캐시된다.** 이후 그 자리에 진짜 테이블을
 *   걸고 유효한 매핑을 채워도, 캐시는 여전히 더미를 가리켜
 *   접근이 폴트로 끝난다.
 *
 * 그래서 더미 링크를 진짜 테이블로 바꿀 때(need_flush_flpd_cache)
 * 이 도메인의 모든 컨트롤러에서 FLPD 캐시를 비운다. 주석이
 * 밝히듯 이때는 블록 없이 무효화해도 안전한데, 대상 주소가
 * 아직 매핑되지 않아 진행 중인 변환이 있을 수 없기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. pgtablelock을 잡은 상태라 GFP_ATOMIC.
 *
 * 호출 체인:
 *   exynos_iommu_map() → [alloc_lv2entry]
 *   → sysmmu_tlb_invalidate_flpdcache()
 */
static sysmmu_pte_t *alloc_lv2entry(struct exynos_iommu_domain *domain,
		sysmmu_pte_t *sent, sysmmu_iova_t iova, short *pgcounter)
{
	/* [한국어] 이 자리에 이미 1MB 섹션이 매핑되어 있다면 그 아래에
	 * 작은 페이지를 넣을 수 없다 — 상위 계층의 이중 매핑이다. */
	if (lv1ent_section(sent)) {
		WARN(1, "Trying mapping on %#08x mapped with 1MiB page", iova);	/* [한국어] 1MB 매핑 아래에 작은 페이지를 넣으려는 시도다. */
		return ERR_PTR(-EADDRINUSE);	/* [한국어] 이중 매핑임을 호출자에게 알린다. */
	}

	/* [한국어] 진짜 2단계 테이블이 없으면 만든다.
	 * lv1ent_fault는 더미 링크도 "없음"으로 본다. */
	if (lv1ent_fault(sent)) {
		/* [한국어] 새 테이블의 DMA 주소. */
		dma_addr_t handle;
		/* [한국어] 새로 만든 2단계 테이블. */
		sysmmu_pte_t *pent;
		/* [한국어] 지금 더미 링크를 교체하는 것인가 —
		 * 그렇다면 FLPD 캐시를 비워야 한다. */
		bool need_flush_flpd_cache = lv1ent_zero(sent);

		/* [한국어] 0으로 초기화된 1KB 테이블을 슬랩에서 받는다.
		 * pgtablelock을 쥐고 있어 ATOMIC이어야 한다. */
		pent = kmem_cache_zalloc(lv2table_kmem_cache, GFP_ATOMIC);
		/* [한국어] 하드웨어가 1KB 정렬을 요구한다. 캐시 생성 시
		 * 정렬을 지정했으므로 정상이라면 항상 참이다. */
		BUG_ON((uintptr_t)pent & (LV2TABLE_SIZE - 1));
		if (!pent)	/* [한국어] 2단계 테이블을 잡지 못했다. */
			return ERR_PTR(-ENOMEM);

		/* [한국어] 1단계 엔트리를 새 테이블로 바꾼다.
		 * set_pte가 캐시 플러시까지 처리한다. */
		exynos_iommu_set_pte(sent, mk_lv1ent_page(virt_to_phys(pent)));
		/* [한국어] 이 테이블에 대한 참조가 물리 주소로만 남아
		 * 누수 검사기가 오탐하므로 제외시킨다. */
		kmemleak_ignore(pent);
		/* [한국어] 새 테이블이므로 256개 엔트리가 모두 비어 있다. */
		*pgcounter = NUM_LV2ENTRIES;
		/* [한국어] 하드웨어가 읽을 수 있도록 DMA 매핑한다. */
		handle = dma_map_single(dma_dev, pent, LV2TABLE_SIZE,
					DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, handle)) {
			/* [한국어] 매핑에 실패했으니 테이블을 되돌린다.
			 * 1단계 엔트리는 이미 바뀌었지만, 그 아래가 전부
			 * 0(폴트)이라 동작상 문제는 없다. */
			kmem_cache_free(lv2table_kmem_cache, pent);
			return ERR_PTR(-EADDRINUSE);	/* [한국어] 테이블을 확보하지 못했음을 알린다. */
		}

		/*
		 * If pre-fetched SLPD is a faulty SLPD in zero_l2_table,
		 * FLPD cache may cache the address of zero_l2_table. This
		 * function replaces the zero_l2_table with new L2 page table
		 * to write valid mappings.
		 * Accessing the valid area may cause page fault since FLPD
		 * cache may still cache zero_l2_table for the valid area
		 * instead of new L2 page table that has the mapping
		 * information of the valid area.
		 * Thus any replacement of zero_l2_table with other valid L2
		 * page table must involve FLPD cache invalidation for System
		 * MMU v3.3.
		 * FLPD cache invalidation is performed with TLB invalidation
		 * by VPN without blocking. It is safe to invalidate TLB without
		 * blocking because the target address of TLB invalidation is
		 * not currently mapped.
		 */
		/* [한국어] 더미 링크를 진짜 테이블로 바꿨다면, 캐시에
		 * 남은 더미 주소를 반드시 지워야 한다. 그러지 않으면
		 * 새로 만든 매핑이 보이지 않고 폴트가 난다. */
		if (need_flush_flpd_cache) {
			/* [한국어] 순회 커서. */
			struct sysmmu_drvdata *data;

			/* [한국어] 이 도메인을 쓰는 모든 컨트롤러에서 비운다. */
			spin_lock(&domain->lock);
			list_for_each_entry(data, &domain->clients, domain_node)	/* [한국어] 이 도메인을 쓰는 모든 컨트롤러에서 비운다. */
				sysmmu_tlb_invalidate_flpdcache(data, iova);
			spin_unlock(&domain->lock);	/* [한국어] 목록 순회가 끝났으니 락을 놓는다. */
		}
	}

	/* [한국어] 확보된 테이블에서 이 IOVA의 엔트리 주소를 돌려준다. */
	return page_entry(sent, iova);
}

/*
 * [한국어]
 * lv1set_section - 1MB 섹션 매핑을 만든다
 *
 * @domain: 대상 도메인.
 * @sent: 1단계 엔트리의 주소.
 * @iova: 대상 IOVA.
 * @paddr: 물리 주소.
 * @prot: 권한.
 * @pgcnt: 이 섹션의 빈 엔트리 카운터.
 * @return: 0 성공, -EADDRINUSE(이미 매핑됨).
 *
 * 이미 2단계 테이블이 달려 있는 자리에 1MB 매핑을 넣으려면,
 * **그 테이블이 완전히 비어 있어야 한다.** 카운터가 256이 아니면
 * 어딘가 매핑이 남아 있다는 뜻이라 거부한다. 이것이 lv2entcnt가
 * 존재하는 이유다 — 테이블을 훑지 않고도 비었는지 알 수 있다.
 *
 * 비어 있으면 그 테이블을 반납하고 섹션 매핑으로 덮어쓴다.
 * 그리고 그 자리가 테이블 링크였다면(더미든 진짜든) FLPD 캐시가
 * 그것을 기억하고 있을 수 있어 비워야 한다.
 *
 * 실행 컨텍스트: 매핑 경로. pgtablelock을 잡은 상태.
 *
 * 호출 체인:
 *   exynos_iommu_map() → [lv1set_section]
 */
static int lv1set_section(struct exynos_iommu_domain *domain,
			  sysmmu_pte_t *sent, sysmmu_iova_t iova,
			  phys_addr_t paddr, int prot, short *pgcnt)
{
	/* [한국어] 이미 섹션이 매핑된 자리다 — 이중 매핑이다. */
	if (lv1ent_section(sent)) {
		WARN(1, "Trying mapping on 1MiB@%#08x that is mapped",	/* [한국어] 이미 섹션이 매핑된 자리다. */
			iova);
		return -EADDRINUSE;	/* [한국어] 이중 매핑임을 알린다. */
	}

	/* [한국어] 2단계 테이블이 달려 있는 자리라면, 그것이 완전히
	 * 비어 있을 때만 섹션으로 덮어쓸 수 있다. */
	if (lv1ent_page(sent)) {
		/* [한국어] 카운터가 최대값이 아니면 어딘가 매핑이 남아 있다.
		 * 테이블을 훑지 않고 이 한 번의 비교로 판단할 수 있는 것이
		 * lv2entcnt를 유지하는 이유다. */
		if (*pgcnt != NUM_LV2ENTRIES) {
			WARN(1, "Trying mapping on 1MiB@%#08x that is mapped",	/* [한국어] 2단계 테이블에 아직 매핑이 남아 있다. */
				iova);
			return -EADDRINUSE;	/* [한국어] 섹션으로 덮어쓸 수 없다. */
		}

		/* [한국어] 빈 테이블이니 반납한다. 인덱스 0으로 테이블의
		 * 시작 주소를 구한다. */
		kmem_cache_free(lv2table_kmem_cache, page_entry(sent, 0));
		/* [한국어] 이제 이 섹션에는 2단계 테이블이 없다. */
		*pgcnt = 0;
	}

	/* [한국어] 1MB 섹션 매핑을 써 넣는다(캐시 플러시 포함). */
	exynos_iommu_set_pte(sent, mk_lv1ent_sect(paddr, prot));

	/* [한국어] 클라이언트 목록을 훑기 위해 잠근다. */
	spin_lock(&domain->lock);
	/* [한국어] 방금 덮어쓴 자리가 테이블 링크였다면 FLPD 캐시가
	 * 그 링크를 기억하고 있을 수 있다. */
	if (lv1ent_page_zero(sent)) {
		/* [한국어] 순회 커서. */
		struct sysmmu_drvdata *data;
		/*
		 * Flushing FLPD cache in System MMU v3.3 that may cache a FLPD
		 * entry by speculative prefetch of SLPD which has no mapping.
		 */
		/* [한국어] 미리 읽기가 캐시해 둔 옛 링크를 지운다.
		 * 그러지 않으면 새 섹션 매핑이 보이지 않는다. */
		list_for_each_entry(data, &domain->clients, domain_node)
			sysmmu_tlb_invalidate_flpdcache(data, iova);
	}
	spin_unlock(&domain->lock);	/* [한국어] 목록 순회가 끝났으니 락을 놓는다. */

	return 0;	/* [한국어] 섹션 매핑이 완료됐다. */
}

/*
 * [한국어]
 * lv2set_page - 2단계 엔트리에 페이지 매핑을 쓴다
 *
 * @pent: 첫 2단계 엔트리의 주소.
 * @paddr: 물리 주소.
 * @size: 4KB 또는 64KB.
 * @prot: 권한.
 * @pgcnt: 이 섹션의 빈 엔트리 카운터.
 * @return: 0 성공, -EADDRINUSE(이미 매핑됨).
 *
 * 64KB 큰 페이지의 표현 방식이 이 함수의 요점이다. 하드웨어는
 * 큰 페이지도 4KB 단위의 엔트리 배열에서 읽으므로, **같은 값을
 * 16번 반복해 채운다.** 어느 엔트리를 읽든 같은 답이 나오게 하는
 * 것이다.
 *
 * 그 16개 중 하나라도 이미 매핑되어 있으면 실패인데, 그때
 * 이미 채운 것들을 memset으로 되돌리는 처리가 들어 있다 —
 * 부분 매핑을 남기지 않기 위함이다.
 *
 * 큰 페이지 경로에서 set_pte를 쓰지 않고 직접 대입하는 이유:
 * 엔트리마다 캐시를 밀어내면 16번 sync가 일어난다. 대신 루프
 * 전체를 앞뒤의 sync 한 쌍으로 감싸 훨씬 싸게 처리한다.
 *
 * 실행 컨텍스트: 매핑 경로. pgtablelock을 잡은 상태.
 *
 * 호출 체인:
 *   exynos_iommu_map() → [lv2set_page]
 */
static int lv2set_page(sysmmu_pte_t *pent, phys_addr_t paddr, size_t size,
		       int prot, short *pgcnt)
{
	/* [한국어] 4KB 작은 페이지 — 엔트리 하나로 끝난다. */
	if (size == SPAGE_SIZE) {
		/* [한국어] 이미 매핑된 자리를 덮어쓰지 않는다. */
		if (WARN_ON(!lv2ent_fault(pent)))
			return -EADDRINUSE;

		/* [한국어] 작은 페이지 엔트리를 쓴다(캐시 플러시 포함). */
		exynos_iommu_set_pte(pent, mk_lv2ent_spage(paddr, prot));
		/* [한국어] 빈 엔트리가 하나 줄었다. */
		*pgcnt -= 1;
	} else { /* size == LPAGE_SIZE */
		/* [한국어] 반복 인덱스. */
		int i;
		/* [한국어] 캐시 플러시의 기준이 될 첫 엔트리의 주소. */
		dma_addr_t pent_base = virt_to_phys(pent);

		/* [한국어] 16개 엔트리 전체의 캐시 소유권을 한 번에
		 * 가져온다 — 엔트리마다 sync 하는 것보다 훨씬 싸다. */
		dma_sync_single_for_cpu(dma_dev, pent_base,
					sizeof(*pent) * SPAGES_PER_LPAGE,
					DMA_TO_DEVICE);
		/* [한국어] 같은 값을 16번 반복해 채운다 — 하드웨어가
		 * 어느 엔트리를 읽든 같은 답이 나와야 한다. */
		for (i = 0; i < SPAGES_PER_LPAGE; i++, pent++) {
			/* [한국어] 도중에 이미 매핑된 엔트리를 만났다. */
			if (WARN_ON(!lv2ent_fault(pent))) {
				/* [한국어] 지금까지 채운 것을 지워 부분 매핑을
				 * 남기지 않는다. pent가 이미 전진했으므로
				 * i만큼 되돌아가 지운다. */
				if (i > 0)
					memset(pent - i, 0, sizeof(*pent) * i);
				return -EADDRINUSE;	/* [한국어] 부분 매핑을 되돌린 뒤 이중 매핑을 알린다. */
			}

			/* [한국어] set_pte를 쓰지 않고 직접 대입한다 —
			 * 캐시 플러시는 루프 밖에서 한 번에 한다. */
			*pent = mk_lv2ent_lpage(paddr, prot);
		}
		/* [한국어] 채운 16개를 한 번에 하드웨어로 밀어낸다. */
		dma_sync_single_for_device(dma_dev, pent_base,
					   sizeof(*pent) * SPAGES_PER_LPAGE,
					   DMA_TO_DEVICE);
		/* [한국어] 빈 엔트리가 16개 줄었다. */
		*pgcnt -= SPAGES_PER_LPAGE;
	}

	return 0;	/* [한국어] 페이지 매핑이 완료됐다. */
}

/*
 * *CAUTION* to the I/O virtual memory managers that support exynos-iommu:
 *
 * System MMU v3.x has advanced logic to improve address translation
 * performance with caching more page table entries by a page table walk.
 * However, the logic has a bug that while caching faulty page table entries,
 * System MMU reports page fault if the cached fault entry is hit even though
 * the fault entry is updated to a valid entry after the entry is cached.
 * To prevent caching faulty page table entries which may be updated to valid
 * entries later, the virtual memory manager should care about the workaround
 * for the problem. The following describes the workaround.
 *
 * Any two consecutive I/O virtual address regions must have a hole of 128KiB
 * at maximum to prevent misbehavior of System MMU 3.x (workaround for h/w bug).
 *
 * Precisely, any start address of I/O virtual region must be aligned with
 * the following sizes for System MMU v3.1 and v3.2.
 * System MMU v3.1: 128KiB
 * System MMU v3.2: 256KiB
 *
 * Because System MMU v3.3 caches page table entries more aggressively, it needs
 * more workarounds.
 * - Any two consecutive I/O virtual regions must have a hole of size larger
 *   than or equal to 128KiB.
 * - Start address of an I/O virtual region must be aligned by 128KiB.
 */
/*
 * [한국어] 위 경고는 이 드라이버 사용자(IOVA 할당기)를 향한 것이다.
 *
 * v3.x는 성능을 위해 페이지 테이블 워크 한 번에 여러 엔트리를
 * 미리 읽어 캐시한다. 그런데 "매핑 없음" 엔트리까지 캐시하고,
 * 나중에 그 자리에 유효한 매핑을 넣어도 캐시된 폴트가 그대로
 * 적중해 버린다. 드라이버 안에서는 FLPD 캐시 무효화로 대응하지만,
 * 그것만으로는 부족한 경우가 남는다.
 *
 * 그래서 **IOVA를 나눠 주는 쪽**에도 요구가 있다: 인접한 두 IOVA
 * 영역 사이에 최소 128KB의 빈틈을 두고, 영역의 시작을 128KB
 * (v3.2는 256KB) 경계에 맞추라는 것이다. 미리 읽기가 이웃 영역의
 * 빈 엔트리까지 건드리지 못하게 하려는 것이다.
 *
 * 이 제약이 드라이버 코드가 아니라 주석으로만 남아 있다는 점에
 * 유의해야 한다 — 지키는 것은 사용자의 몫이다.
 */
/*
 * [한국어]
 * exynos_iommu_map - IOVA에 물리 주소를 매핑한다
 *
 * @iommu_domain: 대상 도메인.
 * @l_iova: 매핑할 IOVA.
 * @paddr: 물리 주소.
 * @size: 매핑 크기(1MB, 64KB, 4KB 중 하나).
 * @count: 개수(이 드라이버는 쓰지 않는다).
 * @prot: 요청 권한.
 * @gfp: 할당 플래그(이 드라이버는 ATOMIC을 강제한다).
 * @mapped: 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, 음수 오류.
 *
 * 크기에 따라 두 갈래로 갈린다. 1MB면 1단계 엔트리에 직접
 * 섹션을 쓰고, 그보다 작으면 2단계 테이블을 확보해 거기에 쓴다.
 *
 * prot을 먼저 마스킹하는 것이 중요하다. 권한 표가 4칸짜리
 * 배열이라, 걸러 내지 않으면 인덱스가 범위를 벗어난다.
 *
 * 실행 컨텍스트: DMA 매핑 경로. pgtablelock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [exynos_iommu_map] → lv1set_section()
 *   또는 alloc_lv2entry() → lv2set_page()
 */
static int exynos_iommu_map(struct iommu_domain *iommu_domain,
			    unsigned long l_iova, phys_addr_t paddr, size_t size,
			    size_t count, int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 대상 도메인. */
	struct exynos_iommu_domain *domain = to_exynos_domain(iommu_domain);
	/* [한국어] 1단계 엔트리의 주소. */
	sysmmu_pte_t *entry;
	/* [한국어] 32비트로 좁힌 IOVA. 이 하드웨어의 주소 공간은 32비트다. */
	sysmmu_iova_t iova = (sysmmu_iova_t)l_iova;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 결과 코드. 아래 갈래가 모두 덮어쓴다. */
	int ret = -ENOMEM;

	/* [한국어] 테이블이 없는 도메인에 매핑을 요청하는 것은 버그다. */
	BUG_ON(domain->pgtable == NULL);
	/* [한국어] 권한 표는 4칸이므로 인덱스가 0~3을 벗어나지 않게
	 * 걸러 낸다 — 배열 범위 밖 접근을 막는 필수 단계다. */
	prot &= SYSMMU_SUPPORTED_PROT_BITS;

	/* [한국어] 테이블 갱신 구간을 잠근다. */
	spin_lock_irqsave(&domain->pgtablelock, flags);

	/* [한국어] 이 IOVA의 1단계 엔트리를 찾는다. */
	entry = section_entry(domain->pgtable, iova);

	/* [한국어] 1MB면 1단계에 직접 섹션을 쓴다 — 2단계 테이블이
	 * 필요 없어 더 싸고 TLB 항목도 절약된다. */
	if (size == SECT_SIZE) {
		ret = lv1set_section(domain, entry, iova, paddr, prot,	/* [한국어] 1단계 엔트리에 직접 섹션을 쓴다. */
				     &domain->lv2entcnt[lv1ent_offset(iova)]);
	} else {
		/* [한국어] 2단계 엔트리의 주소. */
		sysmmu_pte_t *pent;

		/* [한국어] 2단계 테이블을 확보한다(없으면 만든다).
		 * FLPD 캐시 우회가 이 안에서 일어난다. */
		pent = alloc_lv2entry(domain, entry, iova,
				      &domain->lv2entcnt[lv1ent_offset(iova)]);

		if (IS_ERR(pent))	/* [한국어] 테이블 확보에 실패한 경우. */
			ret = PTR_ERR(pent);
		else
			/* [한국어] 4KB 또는 64KB 매핑을 써 넣는다. */
			ret = lv2set_page(pent, paddr, size, prot,
				       &domain->lv2entcnt[lv1ent_offset(iova)]);
	}

	/* [한국어] 실패는 어느 크기의 어느 주소였는지와 함께 남긴다. */
	if (ret)
		pr_err("%s: Failed(%d) to map %#zx bytes @ %#x\n",
			__func__, ret, size, iova);
	else
		/* [한국어] 성공했으면 요청 전량이 매핑된 것이다. */
		*mapped = size;

	spin_unlock_irqrestore(&domain->pgtablelock, flags);

	return ret;	/* [한국어] 매핑 결과를 코어에 전한다. */
}

/*
 * [한국어]
 * exynos_iommu_tlb_invalidate_entry - 도메인의 모든 컨트롤러에서 무효화한다
 *
 * @domain: 대상 도메인.
 * @iova: 무효화할 시작 주소.
 * @size: 크기.
 * @return: 없음.
 *
 * 한 도메인에 여러 마스터의 여러 SYSMMU가 붙어 있을 수 있으므로,
 * 무효화는 클라이언트 목록 전체에 전달해야 한다.
 *
 * 실행 컨텍스트: 해제 경로. 테이블 락은 이미 풀린 상태로 불린다.
 *
 * 호출 체인:
 *   exynos_iommu_unmap() → [exynos_iommu_tlb_invalidate_entry]
 *   → sysmmu_tlb_invalidate_entry()
 */
static void exynos_iommu_tlb_invalidate_entry(struct exynos_iommu_domain *domain,
					      sysmmu_iova_t iova, size_t size)
{
	/* [한국어] 순회 커서. */
	struct sysmmu_drvdata *data;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 목록이 바뀌지 않도록 잠근다. */
	spin_lock_irqsave(&domain->lock, flags);

	/* [한국어] 이 도메인을 쓰는 모든 컨트롤러에서 비운다. */
	list_for_each_entry(data, &domain->clients, domain_node)
		sysmmu_tlb_invalidate_entry(data, iova, size);

	spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 목록 순회가 끝났으니 락을 놓는다. */
}

/*
 * [한국어]
 * exynos_iommu_unmap - IOVA의 매핑을 해제한다
 *
 * @iommu_domain: 대상 도메인.
 * @l_iova: 해제할 IOVA.
 * @size: 요청된 크기.
 * @count: 개수(쓰지 않는다).
 * @gather: 무효화 모으기(이 드라이버는 즉시 무효화한다).
 * @return: 실제로 해제한 크기, 실패하면 0.
 *
 * 실제로 무엇이 매핑되어 있었는지를 **엔트리를 읽어 알아낸다.**
 * 그래서 반환값이 요청 크기와 다를 수 있고, 코어는 그 값만큼만
 * 처리된 것으로 이해해 나머지를 다시 요청한다.
 *
 * 섹션을 해제할 때 0이 아니라 **ZERO_LV2LINK를 쓰는 것**이
 * 이 드라이버의 특징이다. 원본 주석이 v3.3 우회라고 밝히듯,
 * 빈 자리를 0으로 두면 FLPD 캐시가 그것을 기억해 버리기 때문이다.
 *
 * 매핑되지 않은 자리를 만나면 오류가 아니라 "그만큼 건너뛰었다"고
 * 답한다. 코어가 넓은 범위를 해제할 때 중간의 빈 구간을 자연히
 * 넘어가게 하는 설계다.
 *
 * 무효화를 락 밖에서 하는 점도 중요하다 — 하드웨어를 만지는
 * 동안 테이블을 잠가 둘 이유가 없다.
 *
 * 실행 컨텍스트: DMA 해제 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [exynos_iommu_unmap]
 *   → exynos_iommu_tlb_invalidate_entry()
 */
static size_t exynos_iommu_unmap(struct iommu_domain *iommu_domain,
				 unsigned long l_iova, size_t size, size_t count,
				 struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct exynos_iommu_domain *domain = to_exynos_domain(iommu_domain);
	/* [한국어] 32비트로 좁힌 IOVA. */
	sysmmu_iova_t iova = (sysmmu_iova_t)l_iova;
	/* [한국어] 현재 들여다보는 엔트리. */
	sysmmu_pte_t *ent;
	/* [한국어] 오류 시 로그에 남길 "실제 매핑 크기". */
	size_t err_pgsize;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 테이블이 없는 도메인에서 해제를 요청하는 것은 버그다. */
	BUG_ON(domain->pgtable == NULL);

	/* [한국어] 테이블 갱신 구간을 잠근다. */
	spin_lock_irqsave(&domain->pgtablelock, flags);

	/* [한국어] 이 IOVA의 1단계 엔트리를 찾는다. */
	ent = section_entry(domain->pgtable, iova);

	/* [한국어] 1MB 섹션이 매핑되어 있는 경우. */
	if (lv1ent_section(ent)) {
		/* [한국어] 요청이 섹션보다 작으면 쪼갤 수 없다 —
		 * 이 하드웨어에는 섹션을 부분 해제할 방법이 없다. */
		if (WARN_ON(size < SECT_SIZE)) {
			err_pgsize = SECT_SIZE;	/* [한국어] 실제 매핑은 1MB인데 요청이 그보다 작다. */
			goto err;	/* [한국어] 오류 경로로 간다. */
		}

		/* workaround for h/w bug in System MMU v3.3 */
		/* [한국어] 0이 아니라 더미 링크로 되돌린다. 0으로 두면
		 * FLPD 캐시가 "매핑 없음"을 기억해 이후 매핑이 보이지 않는다. */
		exynos_iommu_set_pte(ent, ZERO_LV2LINK);
		/* [한국어] 실제로 해제한 크기는 1MB다. */
		size = SECT_SIZE;
		goto done;	/* [한국어] 해제 완료 지점으로 간다. */
	}

	/* [한국어] 이 1MB 영역에 아무 매핑도 없다. 오류가 아니라
	 * "그만큼 건너뛰었다"고 답해, 코어가 넓은 범위를 해제할 때
	 * 중간의 빈 구간을 자연히 넘어가게 한다. */
	if (unlikely(lv1ent_fault(ent))) {
		if (size > SECT_SIZE)	/* [한국어] 섹션 하나를 넘는 요청은 한 번에 한 섹션씩 처리한다. */
			size = SECT_SIZE;
		goto done;	/* [한국어] 건너뛴 크기를 보고하러 간다. */
	}

	/* lv1ent_page(sent) == true here */

	/* [한국어] 2단계 테이블이 있으니 그 엔트리로 내려간다. */
	ent = page_entry(ent, iova);

	/* [한국어] 그 페이지가 매핑되지 않았다 — 4KB만큼 건너뛴다. */
	if (unlikely(lv2ent_fault(ent))) {
		size = SPAGE_SIZE;	/* [한국어] 한 페이지만큼 건너뛴다. */
		goto done;	/* [한국어] 해제 완료 지점으로 간다. */
	}

	/* [한국어] 4KB 작은 페이지 매핑인 경우. */
	if (lv2ent_small(ent)) {
		/* [한국어] 여기서는 0으로 지워도 된다 — FLPD 캐시가
		 * 문제 삼는 것은 1단계 엔트리이기 때문이다. */
		exynos_iommu_set_pte(ent, 0);
		size = SPAGE_SIZE;
		/* [한국어] 이 섹션의 빈 엔트리가 하나 늘었다. */
		domain->lv2entcnt[lv1ent_offset(iova)] += 1;
		goto done;	/* [한국어] 해제 완료 지점으로 간다. */
	}

	/* lv1ent_large(ent) == true here */
	/* [한국어] 남은 경우는 64KB 큰 페이지다. 요청이 그보다 작으면
	 * 쪼갤 수 없다. */
	if (WARN_ON(size < LPAGE_SIZE)) {
		err_pgsize = LPAGE_SIZE;	/* [한국어] 실제 매핑은 64KB인데 요청이 그보다 작다. */
		goto err;	/* [한국어] 오류 경로로 간다. */
	}

	/* [한국어] 16개 엔트리의 캐시 소유권을 한 번에 가져온다. */
	dma_sync_single_for_cpu(dma_dev, virt_to_phys(ent),
				sizeof(*ent) * SPAGES_PER_LPAGE,
				DMA_TO_DEVICE);
	/* [한국어] 16개를 한 번에 0으로 지운다. */
	memset(ent, 0, sizeof(*ent) * SPAGES_PER_LPAGE);
	/* [한국어] 지운 결과를 한 번에 하드웨어로 밀어낸다. */
	dma_sync_single_for_device(dma_dev, virt_to_phys(ent),
				   sizeof(*ent) * SPAGES_PER_LPAGE,
				   DMA_TO_DEVICE);
	/* [한국어] 실제로 해제한 크기는 64KB다. */
	size = LPAGE_SIZE;
	/* [한국어] 빈 엔트리가 16개 늘었다. */
	domain->lv2entcnt[lv1ent_offset(iova)] += SPAGES_PER_LPAGE;
/* [한국어] 모든 성공 경로가 모이는 지점. */
done:
	/* [한국어] 테이블 갱신이 끝났으니 락을 놓는다. */
	spin_unlock_irqrestore(&domain->pgtablelock, flags);

	/* [한국어] 무효화는 락 밖에서 한다 — 하드웨어를 만지는 동안
	 * 다른 매핑을 막을 이유가 없다. 실제로 해제된 크기만 비운다. */
	exynos_iommu_tlb_invalidate_entry(domain, iova, size);

	return size;
/* [한국어] 매핑 크기보다 작은 해제를 요청받은 경우. */
err:
	spin_unlock_irqrestore(&domain->pgtablelock, flags);

	/* [한국어] 요청 크기와 실제 매핑 크기를 모두 남겨, 상위 계층의
	 * 어떤 가정이 어긋났는지 알 수 있게 한다. */
	pr_err("%s: Failed: size(%#zx) @ %#x is smaller than page size %#zx\n",
		__func__, size, iova, err_pgsize);

	/* [한국어] 아무것도 해제하지 못했다고 보고한다. */
	return 0;
}

/*
 * [한국어]
 * exynos_iommu_iova_to_phys - IOVA를 물리 주소로 변환한다
 *
 * @iommu_domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 세 가지 매핑 크기를 모두 다뤄야 하므로 분기가 셋이다.
 * 각 경우마다 해당 크기의 마스크로 주소를 뽑고 그 안의 오프셋을
 * 더한다 — 그래서 페이지 정렬되지 않은 IOVA도 정확히 변환된다.
 *
 * 실행 컨텍스트: 조회 경로. pgtablelock을 잡는다.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [exynos_iommu_iova_to_phys]
 */
static phys_addr_t exynos_iommu_iova_to_phys(struct iommu_domain *iommu_domain,
					  dma_addr_t iova)
{
	/* [한국어] 대상 도메인. */
	struct exynos_iommu_domain *domain = to_exynos_domain(iommu_domain);
	/* [한국어] 들여다볼 엔트리. */
	sysmmu_pte_t *entry;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 결과. 0이 "매핑 없음"을 뜻한다. */
	phys_addr_t phys = 0;

	/* [한국어] 읽는 동안 테이블이 바뀌지 않게 잠근다. */
	spin_lock_irqsave(&domain->pgtablelock, flags);

	/* [한국어] 1단계 엔트리를 찾는다. */
	entry = section_entry(domain->pgtable, iova);

	/* [한국어] 1MB 섹션이면 1단계에서 답이 나온다. */
	if (lv1ent_section(entry)) {
		phys = section_phys(entry) + section_offs(iova);
	/* [한국어] 진짜 2단계 테이블이 있으면 내려간다.
	 * 더미 링크는 lv1ent_page가 걸러 낸다. */
	} else if (lv1ent_page(entry)) {
		entry = page_entry(entry, iova);

		/* [한국어] 64KB 큰 페이지. */
		if (lv2ent_large(entry))
			phys = lpage_phys(entry) + lpage_offs(iova);
		/* [한국어] 4KB 작은 페이지. */
		else if (lv2ent_small(entry))
			phys = spage_phys(entry) + spage_offs(iova);
	}

	spin_unlock_irqrestore(&domain->pgtablelock, flags);	/* [한국어] 테이블 읽기가 끝났으니 락을 놓는다. */

	return phys;	/* [한국어] 찾았으면 물리 주소, 못 찾았으면 0이다. */
}

/*
 * [한국어]
 * exynos_iommu_probe_device - 마스터 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 담당하지 않으면 ERR_PTR(-ENODEV).
 *
 * of_xlate가 이미 owner와 컨트롤러 목록을 만들어 두었으므로,
 * 여기서는 **전원 링크를 만드는 것**이 주된 일이다.
 *
 * 원본 주석이 밝히듯, 이 링크 덕분에 SYSMMU가 마스터를 따라
 * 자동으로 깨어나고 잠들며, 그래서 이 드라이버에는
 * pm_runtime_get/put 호출이 (상태 확인용을 빼면) 없다.
 *
 * 코어에는 첫 컨트롤러만 알린다. 나머지는 이 드라이버가 owner를
 * 통해 직접 다룬다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [exynos_iommu_probe_device]
 */
static struct iommu_device *exynos_iommu_probe_device(struct device *dev)
{
	/* [한국어] of_xlate가 만들어 둔 소유자 구조체. */
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct sysmmu_drvdata *data;

	/* [한국어] owner가 없으면 디바이스 트리에 iommus 항목이
	 * 없었다는 뜻이라 우리 담당이 아니다. */
	if (!has_sysmmu(dev))
		return ERR_PTR(-ENODEV);

	/* [한국어] 이 마스터에 딸린 모든 SYSMMU와 전원 의존을 만든다. */
	list_for_each_entry(data, &owner->controllers, owner_node) {
		/*
		 * SYSMMU will be runtime activated via device link
		 * (dependency) to its master device, so there are no
		 * direct calls to pm_runtime_get/put in this driver.
		 */
		/* [한국어] STATELESS는 링크의 수명을 이 드라이버가 직접
		 * 관리하겠다는 뜻이라, release_device가 명시적으로 지운다. */
		data->link = device_link_add(dev, data->sysmmu,
					     DL_FLAG_STATELESS |
					     DL_FLAG_PM_RUNTIME);
	}

	/* There is always at least one entry, see exynos_iommu_of_xlate() */
	/* [한국어] 코어에는 대표로 첫 컨트롤러를 알린다. of_xlate가
	 * 최소 하나를 넣어 두므로 목록이 비어 있을 수 없다. */
	data = list_first_entry(&owner->controllers,
				struct sysmmu_drvdata, owner_node);

	return &data->iommu;	/* [한국어] 코어에 대표 컨트롤러를 알린다. */
}

/*
 * [한국어]
 * exynos_iommu_release_device - 마스터의 전원 링크를 걷어낸다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * probe_device가 만든 링크를 지운다. owner 구조체 자체는
 * of_xlate가 만들었고 여기서 해제하지 않는다 — 디바이스가
 * 다시 probe될 수 있기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [exynos_iommu_release_device]
 */
static void exynos_iommu_release_device(struct device *dev)
{
	/* [한국어] 이 마스터의 소유자 구조체. */
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct sysmmu_drvdata *data;

	/* [한국어] STATELESS로 만든 링크는 명시적으로 지워야 한다. */
	list_for_each_entry(data, &owner->controllers, owner_node)
		device_link_del(data->link);
}

/*
 * [한국어]
 * exynos_iommu_of_xlate - 디바이스 트리의 iommus 항목을 소유자에 쌓는다
 *
 * @dev: 마스터 디바이스.
 * @spec: "iommus = <&sysmmu>"에서 파싱된 참조.
 * @return: 0 성공, -ENODEV/-ENOMEM.
 *
 * 항목마다 한 번씩 불리며 누적된다. 첫 호출에서 owner를 만들고
 * identity 도메인으로 초기화하는 것이 중요한데, 그래야 이후
 * attach가 "지금 어느 도메인인가"를 올바르게 판단한다.
 *
 * 이미 등록된 컨트롤러를 다시 만나면 조용히 성공한다 —
 * 디바이스 트리에 같은 SYSMMU가 중복으로 적혀 있을 수 있기 때문이다.
 *
 * -ENODEV를 돌려주는 두 경우에 유의: 노드는 있으나 그 SYSMMU가
 * 아직 probe되지 않았으면 drvdata가 비어 있다. 그러면 코어가
 * 나중에 다시 시도한다(probe 순서 문제의 표준적인 처리다).
 *
 * 실행 컨텍스트: 디바이스 트리 파싱. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [exynos_iommu_of_xlate]
 */
static int exynos_iommu_of_xlate(struct device *dev,
				 const struct of_phandle_args *spec)
{
	/* [한국어] 참조가 가리키는 SYSMMU의 플랫폼 디바이스. */
	struct platform_device *sysmmu = of_find_device_by_node(spec->np);
	/* [한국어] 이미 만들어 둔 소유자(첫 호출이면 NULL). */
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	/* [한국어] 등록할 컨트롤러와, 중복 검사용 순회 커서. */
	struct sysmmu_drvdata *data, *entry;

	/* [한국어] 그런 디바이스가 아직 없다 — 코어가 나중에 다시 시도한다. */
	if (!sysmmu)
		return -ENODEV;

	/* [한국어] 그 SYSMMU의 드라이버 상태를 얻는다. */
	data = platform_get_drvdata(sysmmu);
	/* [한국어] 찾기가 올린 참조를 놓는다 — 수명은 device link가 보장한다. */
	put_device(&sysmmu->dev);
	/* [한국어] 아직 probe되지 않아 상태가 비어 있다.
	 * 역시 나중에 다시 시도된다. */
	if (!data)
		return -ENODEV;

	/* [한국어] 첫 호출이면 소유자 구조체를 만든다. */
	if (!owner) {
		owner = kzalloc_obj(*owner);	/* [한국어] 소유자 구조체를 새로 만든다. */
		if (!owner)	/* [한국어] 소유자 구조체를 잡지 못했다. */
			return -ENOMEM;

		/* [한국어] 컨트롤러 목록을 빈 상태로 준비한다. */
		INIT_LIST_HEAD(&owner->controllers);
		/* [한국어] 전원 전환과 도메인 전환을 직렬화할 뮤텍스. */
		mutex_init(&owner->rpm_lock);
		/* [한국어] 아무 도메인에도 붙어 있지 않은 상태로 시작한다 —
		 * 이 초기화가 있어야 첫 attach의 판단이 올바르다. */
		owner->domain = &exynos_identity_domain;
		dev_iommu_priv_set(dev, owner);	/* [한국어] 이후 모든 콜백이 이 구조체를 꺼내 쓴다. */
	}

	/* [한국어] 이미 등록된 컨트롤러라면 조용히 성공한다.
	 * 디바이스 트리에 중복 참조가 있을 수 있다. */
	list_for_each_entry(entry, &owner->controllers, owner_node)
		if (entry == data)
			return 0;

	/* [한국어] 새 컨트롤러를 목록에 추가한다. */
	list_add_tail(&data->owner_node, &owner->controllers);
	/* [한국어] 이 컨트롤러가 담당할 마스터를 기록한다.
	 * PM 콜백이 이 값으로 owner를 되찾는다. */
	data->master = dev;

	return 0;	/* [한국어] 이 컨트롤러의 등록이 끝났다. */
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * 무효화 콜백이 없는 것은 unmap 안에서 즉시 무효화를 끝내기
 * 때문이다. */
static const struct iommu_ops exynos_iommu_ops = {
	.identity_domain = &exynos_identity_domain,
	/* [한국어] 통과 모드로 쓸 정적 도메인. 실제로는 SYSMMU를
	 * 꺼 버리는 것이며, 이 드라이버의 detach 구현이기도 하다. */

	.release_domain = &exynos_identity_domain,
	/* [한국어] 드라이버가 떨어져 나갈 때 되돌아갈 도메인.
	 * identity와 같은 것을 써, 어느 경로로 끝나든 SYSMMU가
	 * 꺼진 상태로 남는다. */

	.domain_alloc_paging = exynos_iommu_domain_alloc_paging,
	/* [한국어] 페이징 도메인 생성. 1단계 테이블과 카운터를 잡는다. */

	.device_group = generic_device_group,
	/* [한국어] 격리 단위. 마스터마다 독립 그룹이면 충분하다. */

	.probe_device = exynos_iommu_probe_device,
	/* [한국어] 디바이스 등록. 전원 링크를 만드는 것이 핵심이다. */

	.release_device = exynos_iommu_release_device,
	/* [한국어] 디바이스 제거. 그 링크를 끊는다. */

	.get_resv_regions = iommu_dma_get_resv_regions,
	/* [한국어] 예약 영역은 특별할 것이 없어 코어 함수를 그대로 꽂았다. */

	.of_xlate = exynos_iommu_of_xlate,
	/* [한국어] 디바이스 트리의 SYSMMU 참조를 소유자에 쌓는다. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= exynos_iommu_attach_device,
		/* [한국어] 붙이기. 먼저 떼고 다시 붙이는 순서를 지킨다. */

		.map_pages	= exynos_iommu_map,
		/* [한국어] 매핑. 크기에 따라 섹션/큰 페이지/작은 페이지. */

		.unmap_pages	= exynos_iommu_unmap,
		/* [한국어] 해제. 실제 매핑 크기를 엔트리에서 알아낸다. */

		.iova_to_phys	= exynos_iommu_iova_to_phys,
		/* [한국어] 조회. 세 가지 매핑 크기를 모두 다룬다. */

		.free		= exynos_iommu_domain_free,
		/* [한국어] 도메인 해제. 매달린 2단계 테이블까지 반납한다. */
	}
};

/*
 * [한국어]
 * exynos_iommu_init - 드라이버 전역 초기화
 *
 * @return: 0 성공, -ENOMEM.
 *
 * 컨트롤러 드라이버를 등록하기 **전에** 두 가지를 준비한다:
 * 2단계 테이블용 슬랩 캐시와, 전역 더미 테이블이다. 순서가
 * 중요한데, 드라이버를 먼저 등록하면 probe가 즉시 불려 아직
 * 없는 캐시를 쓰려 할 수 있다.
 *
 * 디바이스 트리에 SYSMMU 노드가 없으면 아무것도 하지 않고
 * 성공으로 돌아간다 — 이 SoC가 아니라는 뜻이다.
 *
 * 캐시 생성에서 크기와 정렬을 모두 LV2TABLE_SIZE로 주는 것이
 * 요점이다. 하드웨어가 2단계 테이블에 1KB 정렬을 요구한다.
 *
 * 실행 컨텍스트: core_initcall. 부팅 초기, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   커널 initcall → [exynos_iommu_init] → platform_driver_register()
 */
static int __init exynos_iommu_init(void)
{
	/* [한국어] SYSMMU 노드 존재 확인용. */
	struct device_node *np;
	/* [한국어] 단계별 결과. */
	int ret;

	/* [한국어] 이 시스템에 SYSMMU가 있는지 본다. */
	np = of_find_matching_node(NULL, sysmmu_of_match);
	/* [한국어] 없으면 이 SoC가 아니다 — 조용히 성공으로 돌아간다. */
	if (!np)
		return 0;

	/* [한국어] 찾기가 올린 참조를 놓는다. 존재 확인만이 목적이었다. */
	of_node_put(np);

	/* [한국어] 2단계 테이블 전용 캐시. 크기와 정렬이 모두 1KB인
	 * 것이 핵심이다 — 하드웨어가 그 정렬을 요구한다. */
	lv2table_kmem_cache = kmem_cache_create("exynos-iommu-lv2table",
				LV2TABLE_SIZE, LV2TABLE_SIZE, 0, NULL);
	if (!lv2table_kmem_cache) {	/* [한국어] 슬랩 캐시를 만들지 못한 경우. */
		pr_err("%s: Failed to create kmem cache\n", __func__);	/* [한국어] 어느 단계에서 실패했는지 남긴다. */
		return -ENOMEM;	/* [한국어] 2단계 테이블을 만들 수 없으면 이 드라이버는 쓸 수 없다. */
	}

	/* [한국어] 전역 더미 테이블을 하나 만든다. 모든 도메인의 빈
	 * 1단계 엔트리가 이것을 가리켜, FLPD 캐시가 유효한 링크를
	 * 보게 한다. 0으로 초기화되어 아래 엔트리는 전부 폴트다. */
	zero_lv2_table = kmem_cache_zalloc(lv2table_kmem_cache, GFP_KERNEL);
	if (zero_lv2_table == NULL) {	/* [한국어] 더미 테이블을 잡지 못한 경우. */
		pr_err("%s: Failed to allocate zero level2 page table\n",	/* [한국어] 어느 단계에서 실패했는지 남긴다. */
			__func__);
		ret = -ENOMEM;	/* [한국어] FLPD 우회의 근간이 없으면 진행할 수 없다. */
		goto err_zero_lv2;	/* [한국어] 만들어 둔 캐시를 없애러 간다. */
	}

	/* [한국어] 준비가 끝났으니 컨트롤러 드라이버를 등록한다.
	 * 이 순간부터 probe가 불리기 시작한다. */
	ret = platform_driver_register(&exynos_sysmmu_driver);
	if (ret) {	/* [한국어] 드라이버 등록이 실패한 경우. */
		pr_err("%s: Failed to register driver\n", __func__);	/* [한국어] 실패를 알린다. */
		goto err_reg_driver;	/* [한국어] 더미 테이블을 반납하러 간다. */
	}

	return 0;
/* [한국어] 드라이버 등록 실패 — 더미 테이블을 반납한다. */
err_reg_driver:
	kmem_cache_free(lv2table_kmem_cache, zero_lv2_table);
/* [한국어] 더미 테이블 할당 실패 — 캐시를 없앤다. */
err_zero_lv2:
	kmem_cache_destroy(lv2table_kmem_cache);	/* [한국어] 슬랩 캐시를 없앤다. */
	return ret;	/* [한국어] 실패 이유를 커널에 전한다. */
}
/* [한국어] 마스터 디바이스들보다 먼저 준비되어야 하므로
 * core 단계에서 초기화한다. */
core_initcall(exynos_iommu_init);
