// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
/*
 * [한국어 설명] IOMMU 페이지 테이블 전용 페이지 할당자 (drivers/iommu/iommu-pages.c)
 *
 * === 파일의 역할 ===
 * IOMMU 드라이버가 페이지 테이블과 도메인별 설정 구조체를 담을 페이지를 얻는
 * 창구다. 평범한 alloc_pages 를 쓰지 않는 이유는 두 가지다.
 *
 * 첫째는 회계다. IOMMU 상태는 생각보다 크다 — 큰 주소 공간을 잘게 매핑하면 페이지
 * 테이블만 수 GB 에 이를 수 있는데, 그 메모리가 어디로 갔는지 사용자 공간에서
 * 보이지 않으면 원인 모를 메모리 부족으로 나타난다. 이 파일을 거친 할당은 모두
 * NR_IOMMU_PAGES 와 NR_SECONDARY_PAGETABLE 에 집계되어 /proc/meminfo 에 드러난다.
 *
 * 둘째는 비일관 IOMMU 다. 일부 ARM SoC 의 IOMMU 는 페이지 테이블을 읽을 때 CPU
 * 캐시를 보지 않는다. 그런 하드웨어에서는 PTE 를 쓴 뒤 캐시를 메모리로 밀어내야
 * 하고, 이 파일이 그 캐시 관리를 페이지 단위 상태(incoherent 플래그)로 추적한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: 벤더 드라이버 / io-pgtable 계층
 *         → [이 파일] iommu_alloc_pages_node_sz : 페이지 확보 + 회계 등록
 *         → 드라이버가 그 페이지에 PTE 를 기입
 *         → (비일관 하드웨어면) iommu_pages_start_incoherent 로 캐시 관리 시작
 *       해제는 iommu_free_pages, 또는 지연 해제를 위해 목록에 모아
 *       iommu_put_pages_list 로 한 번에.
 *
 * 지연 해제가 dma-iommu 의 flush queue 와 맞물린다는 점이 중요하다. 페이지 테이블
 * 페이지를 곧바로 반납하면, 아직 무효화되지 않은 IOTLB 항목이 참조하던 표가 다른
 * 용도로 재사용된다. 그래서 해제 목록(iommu_pages_list)이 IOVA 와 함께 큐에 실려
 * 무효화가 끝난 뒤에 풀린다.
 *
 * === 타 모듈과의 연결 ===
 * - struct page 오버레이: struct ioptdesc 는 struct page 와 필드 위치를 맞춘
 *   별도 형이다. 파일 첫머리의 static_assert 들이 그 정렬을 컴파일 시점에 강제한다.
 *   memdesc 전환의 일부로, 페이지 종류마다 자기 서술자를 갖게 하는 방향이다.
 * - folio: 실제 할당은 folio 단위다. 요청 크기를 2의 거듭제곱으로 올려 그 크기에
 *   물리적으로 정렬된 블록을 주는데, 페이지 테이블은 그 정렬을 전제로 인덱싱된다.
 * - DMA API: 비일관 경로에서 dma_map_single 을 캐시 관리 수단으로 쓴다. 위 영어
 *   주석이 인정하듯 arch_sync_dma_for_device 를 직접 부르는 편이 단순하지만,
 *   기존 ARM 드라이버들이 그렇게 해 왔기에 그 방식을 유지한다.
 * - vmstat/memcg: NR_IOMMU_PAGES 와 NR_SECONDARY_PAGETABLE 에 집계한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioptdesc              : 페이지 테이블 페이지 하나의 서술자 (page 오버레이).
 * - iommu_alloc_pages_node_sz()  : NUMA 노드 지정, 크기 지정, 0 으로 채운 할당.
 * - iommu_free_pages()           : 한 장 해제.
 * - iommu_put_pages_list()       : 목록 단위 지연 해제.
 * - iommu_pages_start_incoherent(): 비일관 IOMMU 를 위한 캐시 관리 시작.
 * - iommu_pages_free_incoherent(): 캐시 관리를 되돌리고 해제.
 */
#include "iommu-pages.h"	/* [한국어] struct ioptdesc 정의와 이 파일이 구현하는 API */
#include <linux/dma-mapping.h>	/* [한국어] 비일관 경로에서 캐시 관리 수단으로 쓰는 DMA API */
#include <linux/gfp.h>	/* [한국어] 할당 플래그 */
#include <linux/mm.h>	/* [한국어] folio 와 vmstat 집계 */

#define IOPTDESC_MATCH(pg_elm, elm)                    \	/* [한국어] struct page 의 필드와 struct ioptdesc 의 대응 필드가 같은 오프셋인지 컴파일 시점에 확인하는 매크로 */
	static_assert(offsetof(struct page, pg_elm) == \	/* [한국어] 두 오프셋이 다르면 빌드가 멈춘다 */
		      offsetof(struct ioptdesc, elm))	/* [한국어] ioptdesc 는 page 를 덮어쓰는 오버레이이므로 정렬이 어긋나면 메모리 관리 코어와 이 파일이 서로 다른 곳을 읽게 된다 */
IOPTDESC_MATCH(flags, __page_flags);	/* [한국어] 페이지 플래그 자리 */
IOPTDESC_MATCH(lru, iopt_freelist_elm); /* Ensure bit 0 is clear */	/* [한국어] 해제 목록 고리가 lru 자리에 온다. 위 영어 주석의 '비트 0 이 0 이어야 한다'는 것은 page 코어가 그 비트를 다른 뜻으로 쓰기 때문이다 */
IOPTDESC_MATCH(mapping, __page_mapping);	/* [한국어] 쓰지 않지만 자리를 맞춰 둔다 */
IOPTDESC_MATCH(private, _private);	/* [한국어] 마찬가지 */
IOPTDESC_MATCH(page_type, __page_type);	/* [한국어] 페이지 종류 자리 */
IOPTDESC_MATCH(_refcount, __page_refcount);	/* [한국어] 참조 계수 — folio_put 이 이 자리를 본다 */
#ifdef CONFIG_MEMCG	/* [한국어] 메모리 cgroup 이 켜진 빌드에서만 존재하는 필드 */
IOPTDESC_MATCH(memcg_data, memcg_data);	/* [한국어] cgroup 회계 자리 */
#endif
#undef IOPTDESC_MATCH	/* [한국어] 검증이 끝났으므로 매크로를 거둔다 */
static_assert(sizeof(struct ioptdesc) <= sizeof(struct page));	/* [한국어] 오버레이가 원본보다 크면 이웃 page 구조체를 침범한다 */

/*
 * [한국어]
 * ioptdesc_mem_size - 이 서술자가 대표하는 메모리 크기
 *
 * @desc:   페이지 테이블 페이지의 서술자
 * @return: 바이트 크기
 *
 * 할당이 folio 단위이므로 차수만 알면 크기가 나온다. 크기를 따로 저장하지 않는
 * 이유가 그것이다 — 서술자 자리는 struct page 크기 안에 들어가야 해서 필드 하나가
 * 아깝다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 비일관 캐시 관리 경로 → [이 함수]
 */
static inline size_t ioptdesc_mem_size(struct ioptdesc *desc)
{
	return 1UL << (folio_order(ioptdesc_folio(desc)) + PAGE_SHIFT);	/* [한국어] 이 서술자가 대표하는 메모리 크기. 할당이 folio 단위이므로 차수에서 역산한다 */
}

/**
 * iommu_alloc_pages_node_sz - Allocate a zeroed page of a given size from
 *                             specific NUMA node
 * @nid: memory NUMA node id
 * @gfp: buddy allocator flags
 * @size: Memory size to allocate, rounded up to a power of 2
 *
 * Returns the virtual address of the allocated page. The page must be freed
 * either by calling iommu_free_pages() or via iommu_put_pages_list(). The
 * returned allocation is round_up_pow_two(size) big, and is physically aligned
 * to its size.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_alloc_pages_node_sz - 페이지 테이블용 페이지를 확보한다
 *
 * @nid:    할당할 NUMA 노드 (NUMA_NO_NODE 면 현재 CPU 의 노드)
 * @gfp:    할당 플래그. __GFP_HIGHMEM 은 허용되지 않는다.
 * @size:   필요한 크기. 2의 거듭제곱으로 올림된다.
 * @return: 커널 가상 주소, 실패하면 NULL
 *
 * 두 가지 보장이 이 함수의 계약이다 (위 영어 주석). 반환된 메모리는 요청 크기를
 * 올림한 2의 거듭제곱 크기이고, 그 크기에 물리적으로 정렬되어 있다. 페이지 테이블은
 * 상위 레벨 항목이 하위 테이블의 물리 주소를 담고 인덱스 비트로 접근하므로, 그
 * 정렬이 없으면 인덱싱 자체가 성립하지 않는다.
 *
 * __GFP_ZERO 가 선택이 아닌 이유도 같다. 0 이 아닌 쓰레기가 남아 있으면 하드웨어가
 * 그것을 유효한 PTE 로 읽어 임의의 물리 주소로 번역해 버린다.
 *
 * NUMA 노드를 받는 것은 성능 문제다. IOMMU 는 페이지 테이블을 하드웨어로 워크하며,
 * 그 메모리가 먼 노드에 있으면 매 번역마다 인터커넥트를 건넌다.
 *
 * 실행 컨텍스트: gfp 가 정한다. 매핑 핫패스에서는 GFP_ATOMIC 으로 불린다.
 *
 * 호출 체인: 벤더 드라이버, io-pgtable 계층 → [이 함수]
 */
void *iommu_alloc_pages_node_sz(int nid, gfp_t gfp, size_t size)
{
	struct ioptdesc *iopt;	/* [한국어] 할당한 페이지의 서술자 */
	unsigned long pgcnt;	/* [한국어] 회계에 더할 페이지 수 */
	struct folio *folio;	/* [한국어] 실제 할당 단위 */
	unsigned int order;	/* [한국어] 할당 차수 */

	/* This uses page_address() on the memory. */
	if (WARN_ON(gfp & __GFP_HIGHMEM))	/* [한국어] 이 함수는 folio_address 로 커널 가상 주소를 돌려주므로 HIGHMEM 을 쓸 수 없다 (위 영어 주석) */
		return NULL;	/* [한국어] 잘못된 요청 */

	/*
	 * Currently sub page allocations result in a full page being returned.
	 */
	order = get_order(size);	/* [한국어] 요청 크기를 담을 최소 차수. 페이지보다 작은 요청도 한 페이지를 통째로 받는다 (위 영어 주석) */

	/*
	 * __folio_alloc_node() does not handle NUMA_NO_NODE like
	 * alloc_pages_node() did.
	 */
	if (nid == NUMA_NO_NODE)	/* [한국어] __folio_alloc_node 는 alloc_pages_node 와 달리 이 값을 처리하지 못한다 (위 영어 주석) */
		nid = numa_mem_id();	/* [한국어] 현재 CPU 의 노드로 바꿔 준다 */

	folio = __folio_alloc_node(gfp | __GFP_ZERO, order, nid);	/* [한국어] 반드시 0 으로 채운다 — 페이지 테이블에 쓰레기가 남으면 하드웨어가 그것을 유효한 PTE 로 읽는다 */
	if (unlikely(!folio))	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 호출자가 매핑을 포기한다 */

	iopt = folio_ioptdesc(folio);	/* [한국어] folio 를 서술자로 본다 (같은 메모리의 다른 시각) */
	iopt->incoherent = false;	/* [한국어] 아직 캐시 관리를 시작하지 않은 상태 */

	/*
	 * All page allocations that should be reported to as "iommu-pagetables"
	 * to userspace must use one of the functions below. This includes
	 * allocations of page-tables and other per-iommu_domain configuration
	 * structures.
	 *
	 * This is necessary for the proper accounting as IOMMU state can be
	 * rather large, i.e. multiple gigabytes in size.
	 */
	pgcnt = 1UL << order;	/* [한국어] 이 할당의 페이지 수 */
	mod_node_page_state(folio_pgdat(folio), NR_IOMMU_PAGES, pgcnt);	/* [한국어] 노드별 IOMMU 페이지 집계. 이 통계가 없으면 수 GB 의 페이지 테이블이 어디로 갔는지 알 방법이 없다 (위 영어 주석) */
	lruvec_stat_mod_folio(folio, NR_SECONDARY_PAGETABLE, pgcnt);	/* [한국어] '보조 페이지 테이블' 집계 — KVM 의 그림자 페이지 테이블과 같은 항목을 공유한다. /proc/meminfo 의 SecPageTables 가 이 값이다 */

	return folio_address(folio);	/* [한국어] 커널 가상 주소. 드라이버는 여기에 PTE 를 직접 쓴다 */
}
EXPORT_SYMBOL_GPL(iommu_alloc_pages_node_sz);	/* [한국어] 모든 IOMMU 드라이버와 io-pgtable 계층이 부른다 */

/*
 * [한국어]
 * __iommu_free_desc - 서술자 하나를 해제하고 회계에서 뺀다
 *
 * @iopt: 해제할 페이지의 서술자
 *
 * 모든 해제 경로가 여기로 모인다. 회계 갱신을 folio_put 보다 먼저 하는 순서가
 * 중요한데, put 이 마지막 참조면 folio 가 그 자리에서 사라져 pgdat 을 읽을 수
 * 없게 되기 때문이다.
 *
 * incoherent 경고는 실전에서 의미가 있다. 캐시 관리를 되돌리지 않고 해제하면
 * DMA 매핑이 남은 채 메모리가 재사용되어, swiotlb 슬롯이나 IOMMU 항목이 영영
 * 새 나간다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_free_pages, iommu_put_pages_list,
 *            iommu_pages_free_incoherent → [이 함수]
 */
static void __iommu_free_desc(struct ioptdesc *iopt)
{
	struct folio *folio = ioptdesc_folio(iopt);	/* [한국어] 서술자에서 folio 로 */
	const unsigned long pgcnt = folio_nr_pages(folio);	/* [한국어] 회계에서 뺄 페이지 수 */

	if (IOMMU_PAGES_USE_DMA_API)	/* [한국어] 비일관 경로가 켜진 빌드면 */
		WARN_ON_ONCE(iopt->incoherent);	/* [한국어] 캐시 관리를 되돌리지 않고 해제하려 한다 — DMA 매핑이 남은 채 메모리가 반납되면 그 매핑이 영영 새 나간다 */

	mod_node_page_state(folio_pgdat(folio), NR_IOMMU_PAGES, -pgcnt);	/* [한국어] 할당 때 더한 만큼 뺀다 */
	lruvec_stat_mod_folio(folio, NR_SECONDARY_PAGETABLE, -pgcnt);	/* [한국어] 마찬가지 */
	folio_put(folio);	/* [한국어] 참조를 놓는다 — 마지막이면 버디 할당자로 돌아간다 */
}

/**
 * iommu_free_pages - free pages
 * @virt: virtual address of the page to be freed.
 *
 * The page must have have been allocated by iommu_alloc_pages_node_sz()
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_free_pages - 페이지 한 장을 즉시 반납한다
 *
 * @virt: 할당 때 받은 가상 주소
 *
 * 즉시 반납이므로, 이 페이지를 참조하던 IOTLB 항목이 모두 무효화된 뒤에만
 * 불러야 한다. 그 보장이 없는 경로는 대신 목록에 모아 iommu_put_pages_list 로
 * 지연 해제한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 벤더 드라이버, io-pgtable → [이 함수]
 */
void iommu_free_pages(void *virt)
{
	if (!virt)	/* [한국어] NULL 도 안전하게 받는다 */
		return;	/* [한국어] 할 일 없음 */
	__iommu_free_desc(virt_to_ioptdesc(virt));	/* [한국어] 가상 주소에서 서술자로 되짚어 해제 */
}
EXPORT_SYMBOL_GPL(iommu_free_pages);	/* [한국어] 한 장 해제 */

/**
 * iommu_put_pages_list - free a list of pages.
 * @list: The list of pages to be freed
 *
 * Frees a list of pages allocated by iommu_alloc_pages_node_sz(). On return the
 * passed list is invalid, the caller must use IOMMU_PAGES_LIST_INIT to reinit
 * the list if it expects to use it again.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_put_pages_list - 모아 둔 페이지들을 한 번에 반납한다
 *
 * @list: 해제할 페이지 목록
 *
 * 지연 해제의 종착점이다. 페이지 테이블에서 떼어 낸 페이지를 곧바로 반납하면,
 * 아직 무효화되지 않은 IOTLB 항목이 참조하던 표가 다른 용도로 재사용되어 장치가
 * 남의 메모리를 페이지 테이블로 읽게 된다. 그래서 해제 경로가 이 목록에 모아
 * 두었다가 무효화가 끝난 뒤에 부른다.
 *
 * dma-iommu 의 flush queue 가 IOVA 와 이 목록을 같은 항목에 담아 함께 미루는 것이
 * 그 구조다.
 *
 * 목록은 호출 후 무효가 되므로, 재사용하려면 IOMMU_PAGES_LIST_INIT 으로 다시
 * 초기화해야 한다 (위 영어 주석).
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: dma-iommu 의 fq_ring_free_locked, iommu.c → [이 함수]
 */
void iommu_put_pages_list(struct iommu_pages_list *list)
{
	struct ioptdesc *iopt, *tmp;	/* [한국어] 해제하며 순회하므로 _safe 판 */

	list_for_each_entry_safe(iopt, tmp, &list->pages, iopt_freelist_elm)	/* [한국어] 모아 둔 페이지를 하나씩 */
		__iommu_free_desc(iopt);	/* [한국어] 해제. 목록 자체는 무효가 되므로 재사용하려면 다시 초기화해야 한다 (위 영어 주석) */
}
EXPORT_SYMBOL_GPL(iommu_put_pages_list);	/* [한국어] 지연 무효화 경로가 무효화 완료 후에 부른다 */

/**
 * iommu_pages_start_incoherent - Setup the page for cache incoherent operation
 * @virt: The page to setup
 * @dma_dev: The iommu device
 *
 * For incoherent memory this will use the DMA API to manage the cache flushing
 * on some arches. This is a lot of complexity compared to just calling
 * arch_sync_dma_for_device(), but it is what the existing ARM iommu drivers
 * have been doing. The DMA API requires keeping track of the DMA map and
 * freeing it when required. This keeps track of the dma map inside the ioptdesc
 * so that error paths are simple for the caller.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_pages_start_incoherent - 비일관 IOMMU 를 위한 캐시 관리를 시작한다
 *
 * @virt:    대상 페이지의 가상 주소
 * @dma_dev: 캐시 관리를 대행할 IOMMU 장치
 * @return:  0 성공, 음수 실패
 *
 * 일부 ARM SoC 의 IOMMU 는 페이지 테이블을 읽을 때 CPU 캐시를 보지 않는다. 그런
 * 하드웨어에서는 PTE 를 쓴 뒤 그 캐시라인을 메모리로 밀어내야 IOMMU 가 새 항목을
 * 본다.
 *
 * 그 플러시를 DMA API 로 하는 것이 이 함수의 특이한 점이다. 매핑이 목적이 아니라
 * DMA API 가 뒤에서 해 주는 캐시 관리가 목적이며, 위 영어 주석이 인정하듯
 * arch_sync_dma_for_device 를 직접 부르는 편이 훨씬 단순하다. 기존 ARM 드라이버들이
 * 그렇게 해 왔기에 호환을 위해 유지하는 것이고, 그 대가로 DMA 매핑의 수명을
 * 추적해야 해서 서술자에 incoherent 플래그를 둔다.
 *
 * dma != phys 검사가 안전장치다. 페이지 테이블은 물리 주소로 참조되므로, DMA API 가
 * 다른 주소를 돌려주는 구성(IOMMU 를 거치거나 바운스 버퍼를 쓰는)에서는 성립할 수
 * 없다.
 *
 * 실행 컨텍스트: 페이지 테이블 페이지 생성 직후. 프로세스 문맥.
 *
 * 호출 체인: 비일관 IOMMU 드라이버 → [이 함수]
 */
int iommu_pages_start_incoherent(void *virt, struct device *dma_dev)
{
	struct ioptdesc *iopt = virt_to_ioptdesc(virt);	/* [한국어] 이 페이지의 서술자 */
	dma_addr_t dma;	/* [한국어] DMA API 가 돌려줄 주소 */

	if (WARN_ON(iopt->incoherent))	/* [한국어] 이미 캐시 관리 중인 페이지에 다시 시작하려 한다 */
		return -EINVAL;	/* [한국어] 중복 호출은 매핑 누수를 만든다 */

	if (!IOMMU_PAGES_USE_DMA_API) {	/* [한국어] DMA API 를 거치지 않는 구성이면 */
		iommu_pages_flush_incoherent(dma_dev, virt, 0,	/* [한국어] 아키텍처의 캐시 플러시를 직접 부른다 */
					     ioptdesc_mem_size(iopt));	/* [한국어] 이 페이지 전체 */
	} else {
		dma = dma_map_single(dma_dev, virt, ioptdesc_mem_size(iopt),	/* [한국어] DMA 매핑을 캐시 관리 수단으로 쓴다. 매핑 자체가 목적이 아니라, DMA API 가 뒤에서 해 주는 캐시 플러시가 목적이다 (위 영어 주석) */
				     DMA_TO_DEVICE);	/* [한국어] IOMMU 가 페이지 테이블을 읽기만 하므로 이 방향 */
		if (dma_mapping_error(dma_dev, dma))	/* [한국어] 매핑 실패 */
			return -EINVAL;	/* [한국어] 캐시 관리를 시작할 수 없다 */

		/*
		 * The DMA API is not allowed to do anything other than DMA
		 * direct. It would be nice to also check
		 * dev_is_dma_coherent(dma_dev));
		 */
		if (WARN_ON(dma != virt_to_phys(virt))) {	/* [한국어] 돌려받은 DMA 주소가 물리 주소와 다르다 = 이 장치가 직접 매핑이 아닌 경로를 쓰고 있다. 페이지 테이블은 물리 주소로 참조되어야 하므로 성립할 수 없다 (위 영어 주석) */
			dma_unmap_single(dma_dev, dma, ioptdesc_mem_size(iopt),	/* [한국어] 잡은 매핑을 되돌리고 */
					 DMA_TO_DEVICE);	/* [한국어] 같은 방향으로 */
			return -EOPNOTSUPP;	/* [한국어] 이 구성에서는 비일관 IOMMU 를 지원할 수 없다 */
		}
	}

	iopt->incoherent = 1;	/* [한국어] 캐시 관리 중임을 기록 — 해제 경로가 이 값을 보고 되돌린다 */
	return 0;	/* [한국어] 이제 이 페이지에 PTE 를 쓴 뒤 플러시하면 IOMMU 가 볼 수 있다 */
}
EXPORT_SYMBOL_GPL(iommu_pages_start_incoherent);	/* [한국어] 비일관 IOMMU 드라이버가 페이지 테이블 페이지마다 부른다 */

/**
 * iommu_pages_start_incoherent_list - Make a list of pages incoherent
 * @list: The list of pages to setup
 * @dma_dev: The iommu device
 *
 * Perform iommu_pages_start_incoherent() across all of list.
 *
 * If this fails the caller must call iommu_pages_stop_incoherent_list().
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_pages_start_incoherent_list - 목록 전체를 캐시 관리 상태로 만든다
 *
 * @list:    대상 페이지 목록
 * @dma_dev: IOMMU 장치
 * @return:  0 성공, 음수면 도중 실패
 *
 * 실패해도 부분 성공분을 스스로 정리하지 않는다 — 호출자가
 * iommu_pages_stop_incoherent_list 를 부르는 것이 계약이다 (위 영어 주석).
 * 그쪽이 incoherent 플래그가 선 페이지만 골라 처리하므로, 어디까지 성공했는지
 * 따로 기억할 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: 비일관 IOMMU 드라이버 → [이 함수]
 */
int iommu_pages_start_incoherent_list(struct iommu_pages_list *list,
				      struct device *dma_dev)
{
	struct ioptdesc *cur;	/* [한국어] 목록 순회 커서 */
	int ret;	/* [한국어] 각 페이지의 결과 */

	list_for_each_entry(cur, &list->pages, iopt_freelist_elm) {	/* [한국어] 목록의 페이지마다 */
		if (WARN_ON(cur->incoherent))	/* [한국어] 이미 캐시 관리 중인 페이지가 섞여 있다 */
			continue;	/* [한국어] 건너뛴다 — 중복 매핑을 만들지 않는다 */

		ret = iommu_pages_start_incoherent(	/* [한국어] 한 장씩 시작 */
			folio_address(ioptdesc_folio(cur)), dma_dev);	/* [한국어] 서술자에서 가상 주소로 */
		if (ret)	/* [한국어] 실패 */
			return ret;	/* [한국어] 호출자가 stop_incoherent_list 로 부분 성공분을 정리해야 한다 (위 영어 주석) */
	}
	return 0;	/* [한국어] 목록 전체가 캐시 관리 상태로 */
}
EXPORT_SYMBOL_GPL(iommu_pages_start_incoherent_list);	/* [한국어] 여러 페이지를 한 번에 준비할 때 */

/**
 * iommu_pages_stop_incoherent_list - Undo incoherence across a list
 * @list: The list of pages to release
 * @dma_dev: The iommu device
 *
 * Revert iommu_pages_start_incoherent() across all of the list. Pages that did
 * not call or succeed iommu_pages_start_incoherent() will be ignored.
 */
#if IOMMU_PAGES_USE_DMA_API	/* [한국어] 아래 두 함수는 DMA API 경로에서만 의미가 있다 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_pages_stop_incoherent_list - 목록의 캐시 관리를 되돌린다
 *
 * @list:    대상 페이지 목록
 * @dma_dev: IOMMU 장치
 *
 * incoherent 플래그를 보고 실제로 시작된 페이지만 처리한다. 그 덕분에 부분 실패
 * 정리와 정상 해제가 같은 함수를 쓸 수 있다 (위 영어 주석).
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: 비일관 IOMMU 드라이버, start_list 실패 정리 → [이 함수]
 */
void iommu_pages_stop_incoherent_list(struct iommu_pages_list *list,
				      struct device *dma_dev)
{
	struct ioptdesc *cur;	/* [한국어] 목록 순회 커서 */

	list_for_each_entry(cur, &list->pages, iopt_freelist_elm) {	/* [한국어] 목록의 페이지마다 */
		struct folio *folio = ioptdesc_folio(cur);	/* [한국어] 그 페이지의 folio */

		if (!cur->incoherent)	/* [한국어] 캐시 관리를 시작한 적이 없으면 */
			continue;	/* [한국어] 건너뛴다 — 부분 실패 정리를 단순하게 만드는 설계다 (위 영어 주석) */
		dma_unmap_single(dma_dev, virt_to_phys(folio_address(folio)),	/* [한국어] DMA 매핑 해제. 위에서 dma == phys 임을 확인해 뒀으므로 물리 주소로 그대로 부를 수 있다 */
				 ioptdesc_mem_size(cur), DMA_TO_DEVICE);	/* [한국어] 같은 크기·방향 */
		cur->incoherent = 0;	/* [한국어] 관리 종료 표시 */
	}
}
EXPORT_SYMBOL_GPL(iommu_pages_stop_incoherent_list);	/* [한국어] start 의 짝, 또는 부분 실패 정리 */

/**
 * iommu_pages_free_incoherent - Free an incoherent page
 * @virt: virtual address of the page to be freed.
 * @dma_dev: The iommu device
 *
 * If the page is incoherent it made coherent again then freed.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_pages_free_incoherent - 캐시 관리를 되돌리고 페이지를 반납한다
 *
 * @virt:    해제할 페이지의 가상 주소
 * @dma_dev: IOMMU 장치
 *
 * 순서가 전부다. DMA 매핑을 먼저 되돌리지 않고 페이지를 반납하면 그 매핑이 영영
 * 새 나간다 — __iommu_free_desc 의 WARN 이 잡으려는 것이 바로 그 실수다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: 비일관 IOMMU 드라이버 → [이 함수]
 */
void iommu_pages_free_incoherent(void *virt, struct device *dma_dev)
{
	struct ioptdesc *iopt = virt_to_ioptdesc(virt);	/* [한국어] 이 페이지의 서술자 */

	if (iopt->incoherent) {	/* [한국어] 캐시 관리 중이면 */
		dma_unmap_single(dma_dev, virt_to_phys(virt),	/* [한국어] 먼저 되돌린다 — 매핑이 남은 채 해제하면 그 매핑이 새 나간다 */
				 ioptdesc_mem_size(iopt), DMA_TO_DEVICE);	/* [한국어] 같은 크기·방향 */
		iopt->incoherent = 0;	/* [한국어] 관리 종료 표시 */
	}
	__iommu_free_desc(iopt);	/* [한국어] 그 다음에야 페이지를 반납한다 */
}
EXPORT_SYMBOL_GPL(iommu_pages_free_incoherent);	/* [한국어] 비일관 페이지의 해제 진입점 */
#endif
