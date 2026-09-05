/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/*
 * [한국어 설명] 페이지 테이블 페이지 할당자의 인터페이스 (drivers/iommu/iommu-pages.h)
 *
 * === 파일의 역할 ===
 * iommu-pages.c 가 구현하는 API 와, 그것을 감싸는 인라인 헬퍼들이다. 여기서
 * 중요한 것은 두 가지다.
 *
 * 첫째, struct ioptdesc 의 정의. 페이지 테이블 페이지 하나를 기술하는 구조체인데
 * struct page 를 덮어쓰는 오버레이라, 필드 위치가 정확히 맞아야 한다. 그 검증은
 * iommu-pages.c 의 static_assert 들이 한다.
 *
 * 둘째, 비일관 IOMMU 의 캐시 관리가 아키텍처마다 갈린다는 사실. x86 은 clflush 를
 * 직접 부르고, 그 외에는 DMA API 를 캐시 관리 수단으로 빌려 쓴다. 그 차이가
 * IOMMU_PAGES_USE_DMA_API 로 표현되며, 그 값에 따라 stop/free 경로의 구현 자체가
 * 달라진다 — x86 은 되돌릴 DMA 매핑이 없어 아무 일도 하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 벤더 드라이버 / io-pgtable → [이 헤더] → iommu-pages.c → folio 할당자
 *
 * === 타 모듈과의 연결 ===
 * - iommu-pages.c: 여기 선언된 것의 구현.
 * - io-pgtable-arm.c 등: 페이지 테이블 페이지를 이 API 로 얻는다.
 * - dma-iommu.c: iommu_pages_list 를 flush queue 항목에 담아 지연 해제한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioptdesc            : 페이지 테이블 페이지의 서술자 (page 오버레이).
 * - iommu_alloc_pages_node_sz(): 크기·노드 지정 할당.
 * - iommu_pages_list_add/splice(): 지연 해제 목록 조작.
 * - IOMMU_PAGES_USE_DMA_API    : 캐시 관리를 DMA API 로 하는가 (x86 은 0).
 */
#ifndef __IOMMU_PAGES_H	/* [한국어] 중복 포함 방지 */
#define __IOMMU_PAGES_H

#include <linux/iommu.h>	/* [한국어] struct iommu_pages_list 정의 */

/**
 * struct ioptdesc - Memory descriptor for IOMMU page tables
 * @iopt_freelist_elm: List element for a struct iommu_pages_list
 *
 * This struct overlays struct page for now. Do not modify without a good
 * understanding of the issues.
 */
struct ioptdesc {
	unsigned long __page_flags;
	/* [한국어] (위 영어 주석 참고) struct page 의 flags 자리 — 이 파일은 쓰지 않는다.
	 * 설정자/읽는 자: 이 드라이버에서는 아무도. 페이지 할당기와 mm 이 자기 목적으로 쓴다.
	 * 왜 선언해 두는가: 이 구조체는 struct page 위에 겹쳐 놓인다(오버레이). 쓰지
	 *   않는 자리라도 같은 크기로 선언해 두어야 그 뒤의 필드가 struct page 의
	 *   대응하는 자리와 정확히 맞물린다. 하나라도 빠지면 이후 필드가 밀려,
	 *   mm 이 페이지 플래그로 읽는 자리에 IOMMU 의 자료가 놓이게 된다 — 커널이
	 *   곧바로 무너지는 종류의 오류다.
	 * 이름 앞의 두 밑줄이 "이것은 자리맞춤일 뿐 건드리지 말라"는 표시다. */

	struct list_head iopt_freelist_elm;
	/* [한국어] (위 kernel-doc 참고) 지연 해제 목록(struct iommu_pages_list)에 이 페이지를 매다는 고리.
	 * 설정자: 페이지 테이블을 걷을 때, 곧바로 반납하지 않고 이 목록에 모은다.
	 * 읽는 자: iommu_put_pages_list() 가 목록을 훑으며 실제로 반납한다.
	 * 왜 곧바로 반납하지 않는가: 표를 걷은 직후에는 하드웨어의 TLB 에 그 표를
	 *   가리키는 항목이 남아 있을 수 있다. TLB 를 비우고 완료를 확인한 뒤에야
	 *   안전하게 반납할 수 있어, 그 사이 페이지를 어딘가에 모아 두어야 한다.
	 * 어느 자리를 빌려 쓰는가: struct page 의 lru 자리다. 페이지 할당기에 속하지
	 *   않은 페이지에서는 그 자리가 비어 있어 빌려 쓸 수 있다. */
	unsigned long __page_mapping;
	/* [한국어] (위 영어 주석 참고) struct page 의 mapping 자리 — 이 파일은 쓰지 않는다.
	 * 설정자/읽는 자: 이 드라이버에서는 아무도. 페이지 할당기와 mm 이 자기 목적으로 쓴다.
	 * 왜 선언해 두는가: 이 구조체는 struct page 위에 겹쳐 놓인다(오버레이). 쓰지
	 *   않는 자리라도 같은 크기로 선언해 두어야 그 뒤의 필드가 struct page 의
	 *   대응하는 자리와 정확히 맞물린다. 하나라도 빠지면 이후 필드가 밀려,
	 *   이후의 모든 필드가 어긋난다.
	 * 이름 앞의 두 밑줄이 "이것은 자리맞춤일 뿐 건드리지 말라"는 표시다. */
	union {
	/* [한국어] incoherent 플래그가 struct page 의 index 자리를 빌려 쓰도록 겹친 것.
	 * 왜 union 인가: 실제로 필요한 것은 아래 u8 하나뿐이지만, 오버레이가 성립하려면
	 *   이 자리가 pgoff_t 크기여야 한다. 둘을 겹쳐 두면 크기는 pgoff_t 를 따르면서
	 *   쓰기는 u8 로 할 수 있다.
	 * 읽는 자: 비일관 플랫폼에서 이 페이지에 DMA 매핑이 걸려 있는지 판정하는 코드.
	 * 같은 문제를 해결하는 다른 방법(별도 배열, 페이지마다의 부가 자료)은 모두
	 *   페이지 하나당 메모리를 더 쓴다. 이미 있는 자리를 빌리는 것이 가장 싸다. */
		u8 incoherent;
		/* [한국어] 이 페이지의 캐시 관리가 시작되었는가 — DMA 매핑이 걸려 있다는 뜻이다.
		 * 설정자: iommu_pages_start_incoherent() 가 dma_map_single 에 성공하면 1 로 둔다.
		 * 읽는 자: 페이지를 반납하기 전에 매핑을 되돌려야 하는지 판정하는 코드.
		 * 왜 추적해야 하는가: non-x86 에서는 캐시 관리를 DMA API 로 하는데(iommu-pages.h
		 *   위쪽의 IOMMU_PAGES_USE_DMA_API 참고), dma_sync 는 그 페이지가 이미 매핑되어
		 *   있을 때만 성립한다. 그리고 매핑을 되돌리지 않고 페이지를 반납하면 DMA API
		 *   의 내부 상태가 새거나, IOMMU 가 반납된 페이지를 계속 가리킨다.
		 * x86 에서는 이 플래그가 쓰이지 않는다 — clflush 는 매핑 없이도 동작한다.
		 * 값 범위: 0 또는 1. */
		pgoff_t __index;
		/* [한국어] 위 incoherent 와 자리를 나눠, 이 union 의 크기를 struct page 의 index 자리에 맞춘다.
		 * 설정자/읽는 자: 아무도. 크기를 만드는 것이 유일한 목적이다.
		 * 왜 u8 하나로 두지 않는가: 그러면 union 이 1바이트가 되어 뒤따르는 _private
		 *   이 struct page 의 대응 자리보다 앞으로 당겨진다. 오버레이가 깨진다.
		 * 이름 앞의 두 밑줄은 다른 자리맞춤 필드들과 같은 뜻이다 — 건드리지 말 것. */
	};
	void *_private;
	/* [한국어] (위 영어 주석 참고) struct page 의 private 자리 — 이 파일은 쓰지 않는다.
	 * 설정자/읽는 자: 이 드라이버에서는 아무도. 페이지 할당기와 mm 이 자기 목적으로 쓴다.
	 * 왜 선언해 두는가: 이 구조체는 struct page 위에 겹쳐 놓인다(오버레이). 쓰지
	 *   않는 자리라도 같은 크기로 선언해 두어야 그 뒤의 필드가 struct page 의
	 *   대응하는 자리와 정확히 맞물린다. 하나라도 빠지면 이후 필드가 밀려,
	 *   _private 뒤의 __page_type 과 __page_refcount 가 어긋난다. 특히 참조 계수가
	 *   어긋나면 다른 페이지의 계수를 건드리게 되어 진단이 매우 어려운 손상이 된다.
	 * 이름 앞의 두 밑줄이 "이것은 자리맞춤일 뿐 건드리지 말라"는 표시다. */

	unsigned int __page_type;
	/* [한국어] (위 영어 주석 참고) struct page 의 종류(page type) 자리 — 이 파일은 쓰지 않는다.
	 * 설정자/읽는 자: 이 드라이버에서는 아무도. 페이지 할당기와 mm 이 자기 목적으로 쓴다.
	 * 왜 선언해 두는가: 이 구조체는 struct page 위에 겹쳐 놓인다(오버레이). 쓰지
	 *   않는 자리라도 같은 크기로 선언해 두어야 그 뒤의 필드가 struct page 의
	 *   대응하는 자리와 정확히 맞물린다. 하나라도 빠지면 이후 필드가 밀려,
	 *   바로 뒤의 __page_refcount 가 어긋난다.
	 * 이름 앞의 두 밑줄이 "이것은 자리맞춤일 뿐 건드리지 말라"는 표시다. */
	atomic_t __page_refcount;
	/* [한국어] (위 영어 주석 참고) struct page 의 참조 계수 자리.
	 * 설정자/읽는 자: 이 파일은 직접 건드리지 않지만, folio_put() 이 페이지를
	 *   반납할 때 정확히 이 자리를 본다.
	 * 왜 이 자리만은 특별한가: 위 자리맞춤 필드들은 어긋나도 "IOMMU 가 이상한
	 *   값을 본다"로 끝나지만, 이 자리가 어긋나면 페이지 할당기가 엉뚱한 곳을
	 *   참조 계수로 읽는다. 페이지가 쓰이는 중에 해제되거나 영원히 해제되지 않는다.
	 * 그래서 위 kernel-doc 이 "충분히 이해하지 못했으면 고치지 말라"고 못 박는다. */
#ifdef CONFIG_MEMCG	/* [한국어] 메모리 cgroup 이 켜진 빌드에만 있는 필드 */
	unsigned long memcg_data;	/* [한국어] cgroup 회계 자리 */
#endif
};

/* [한국어] folio 를 페이지 테이블 서술자로 본다.
 * 두 구조체가 같은 메모리를 다르게 해석하는 것이라 변환 비용이 없고,
 * 필드 위치가 맞는지는 iommu-pages.c 의 static_assert 들이 보장한다. */
static inline struct ioptdesc *folio_ioptdesc(struct folio *folio)
{
	return (struct ioptdesc *)folio;	/* [한국어] folio 를 이 서술자로 본다. 두 구조체가 같은 메모리를 다르게 해석하는 것이라 변환 비용이 없다 */
}

/* [한국어] 그 역. folio 계층의 API(참조 계수, 회계)를 부를 때 쓴다. */
static inline struct folio *ioptdesc_folio(struct ioptdesc *iopt)
{
	return (struct folio *)iopt;	/* [한국어] 그 역 */
}

/* [한국어] 가상 주소에서 서술자로.
 * 해제 API 들이 주소만 받으므로 이 변환이 매번 필요하다. */
static inline struct ioptdesc *virt_to_ioptdesc(void *virt)
{
	return folio_ioptdesc(virt_to_folio(virt));	/* [한국어] 가상 주소에서 서술자로. 해제 경로가 주소만 받으므로 이 변환이 필요하다 */
}

void *iommu_alloc_pages_node_sz(int nid, gfp_t gfp, size_t size);	/* [한국어] NUMA 노드와 크기를 지정한 할당 */
void iommu_free_pages(void *virt);	/* [한국어] 즉시 해제 */
void iommu_put_pages_list(struct iommu_pages_list *list);	/* [한국어] 목록 단위 지연 해제 */

/**
 * iommu_pages_list_add - add the page to a iommu_pages_list
 * @list: List to add the page to
 * @virt: Address returned from iommu_alloc_pages_node_sz()
 */
/* [한국어] (위 영어 kernel-doc 에 이어)
 * 페이지를 지연 해제 목록에 넣는다. 목록의 고리가 서술자 안에 있으므로 별도
 * 할당이 없다 — 페이지 자신이 자기 목록 항목 역할을 한다. */
static inline void iommu_pages_list_add(struct iommu_pages_list *list,
					void *virt)
{
	list_add_tail(&virt_to_ioptdesc(virt)->iopt_freelist_elm, &list->pages);	/* [한국어] 페이지를 해제 대기 목록에 넣는다. 실제 반납은 IOTLB 무효화가 끝난 뒤에 일어난다 */
}

/**
 * iommu_pages_list_splice - Put all the pages in list from into list to
 * @from: Source list of pages
 * @to: Destination list of pages
 *
 * from must be re-initialized after calling this function if it is to be
 * used again.
 */
/* [한국어] (위 영어 kernel-doc 에 이어)
 * 한 목록을 다른 목록 끝에 통째로 옮긴다. dma-iommu 가 해제 경로에서 모은
 * 페이지들을 flush queue 항목으로 넘길 때 쓰며, O(1) 이라 큰 목록도 비용이 없다.
 * 원본은 무효가 되므로 재사용하려면 다시 초기화해야 한다. */
static inline void iommu_pages_list_splice(struct iommu_pages_list *from,
					   struct iommu_pages_list *to)
{
	list_splice(&from->pages, &to->pages);	/* [한국어] 한 목록을 다른 목록에 통째로 옮겨 붙인다. dma-iommu 가 해제 경로의 freelist 를 flush queue 항목으로 옮길 때 쓰며, 원본은 무효가 되므로 재사용하려면 다시 초기화해야 한다 (위 영어 주석) */
}

/**
 * iommu_pages_list_empty - True if the list is empty
 * @list: List to check
 */
/* [한국어] (위 영어 kernel-doc 에 이어)
 * 목록이 비었는가. 지연 해제 경로가 반납할 것이 있는지 볼 때 쓴다. */
static inline bool iommu_pages_list_empty(struct iommu_pages_list *list)
{
	return list_empty(&list->pages);	/* [한국어] 목록이 비었는가 */
}

/**
 * iommu_alloc_pages_sz - Allocate a zeroed page of a given size from
 *                          specific NUMA node
 * @nid: memory NUMA node id
 * @gfp: buddy allocator flags
 * @size: Memory size to allocate, this is rounded up to a power of 2
 *
 * Returns the virtual address of the allocated page.
 */
/* [한국어] (위 영어 kernel-doc 에 이어)
 * NUMA 노드를 가리지 않는 할당. 현재 CPU 의 노드가 선택되므로, IOMMU 와 가까운
 * 메모리를 원하면 대신 iommu_alloc_pages_node_sz 를 쓴다. */
static inline void *iommu_alloc_pages_sz(gfp_t gfp, size_t size)
{
	return iommu_alloc_pages_node_sz(NUMA_NO_NODE, gfp, size);	/* [한국어] 노드를 가리지 않는 할당. 현재 CPU 의 노드가 선택된다 */
}

int iommu_pages_start_incoherent(void *virt, struct device *dma_dev);	/* [한국어] 비일관 IOMMU 를 위한 캐시 관리를 시작한다 */
int iommu_pages_start_incoherent_list(struct iommu_pages_list *list,	/* [한국어] 목록 단위 판 */
				      struct device *dma_dev);	/* [한국어] 캐시 관리를 대행할 IOMMU 장치 */

#ifdef CONFIG_X86	/* [한국어] x86 은 캐시 관리 방식이 다르다 */
#define IOMMU_PAGES_USE_DMA_API 0	/* [한국어] DMA API 를 쓰지 않는다. clflush 를 직접 부를 수 있어 매핑을 만들 이유가 없다 */
#include <linux/cacheflush.h>	/* [한국어] clflush_cache_range */

/*
 * [한국어]
 * iommu_pages_flush_incoherent - (x86) 페이지 테이블 수정분을 캐시에서 메모리로 밀어낸다
 *
 * @dma_dev: 캐시 관리를 대행할 장치. x86 판에서는 쓰이지 않는다 — clflush 는
 *           CPU 명령이라 장치가 필요 없다. 인자를 남긴 것은 non-x86 판과
 *           시그니처를 맞춰 호출부에 #ifdef 를 심지 않기 위해서다.
 * @virt:    수정한 페이지 테이블의 커널 가상 주소.
 * @offset:  그 페이지 안에서 실제로 고친 부분의 시작 오프셋.
 * @len:     고친 길이(바이트).
 *
 * 어떤 IOMMU 는 페이지 테이블을 읽을 때 CPU 캐시와 일관성을 지키지 않는다
 * (AMD 의 일부 구성, ARM 의 상당수). 그런 하드웨어에서는 CPU 가 테이블 엔트리를
 * 고쳐도 그 값이 캐시에만 남아 있고 IOMMU 는 옛 값을 읽는다. 이 함수가 없으면
 * 매핑을 만들었는데 장치가 여전히 폴트를 내거나, 더 나쁘게는 지운 매핑으로
 * DMA 가 계속 나가는 상황이 생긴다.
 *
 * x86 판은 clflush_cache_range() 로 해당 캐시라인만 골라 밀어낸다. offset/len 을
 * 받는 이유가 이것이다 — 페이지 전체가 아니라 고친 엔트리 몇 개만 비우면 된다.
 *
 * non-x86 판(아래 #else)은 같은 일을 dma_sync_single_for_device() 로 하며,
 * 그 차이가 IOMMU_PAGES_USE_DMA_API 매크로에 드러나 있다.
 *
 * 실행 컨텍스트: 매핑/해제 핫패스. 락을 쥔 채로, 인터럽트 비활성 구간에서도
 * 불릴 수 있다. 잠들지 않는다.
 *
 * 호출 체인:
 *   io-pgtable 구현의 엔트리 기록 직후 → [이 함수] → clflush_cache_range()
 */
static inline void iommu_pages_flush_incoherent(struct device *dma_dev,
						void *virt, size_t offset,
						size_t len)
{
	clflush_cache_range(virt + offset, len);	/* [한국어] 해당 캐시라인들을 메모리로 밀어낸다. AMD IOMMU 처럼 일부 구성에서 페이지 테이블이 비일관인 경우에 쓴다 */
}
/*
 * [한국어]
 * iommu_pages_stop_incoherent_list - (x86) 목록의 캐시 관리를 끝낸다 — 할 일이 없다
 *
 * @list:    해제 직전의 페이지 목록. 여기서는 손대지 않는다.
 * @dma_dev: 캐시 관리를 맡았던 장치. 여기서는 쓰이지 않는다.
 *
 * non-x86 판에서는 이 함수가 실제 일을 한다. 그쪽은 캐시 관리를 DMA API 로
 * 하기 때문에 페이지마다 dma_map_single() 매핑이 걸려 있고, 페이지를 반납하기
 * 전에 그 매핑을 반드시 되돌려야 한다.
 *
 * x86 에는 되돌릴 매핑이 없다. clflush 는 매핑 없이도 되는 CPU 명령이라
 * IOMMU_PAGES_USE_DMA_API 가 0 이다. 그래서 이 판은 비어 있다.
 *
 * 본문의 영어 주석이 말하는 것: 성능을 위해 incoherent 플래그를 일부러 끄지
 * 않고 그대로 둔다. 플래그를 정리하려면 목록을 한 바퀴 돌아야 하는데, x86 의
 * 이후 stop/free 경로는 그 플래그를 보지 않으므로 도는 것 자체가 낭비다.
 *
 * 실행 컨텍스트: 페이지 목록 해제 직전(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_put_pages_list() 계열 → [이 빈 구현]
 */
static inline void	/* [한국어] x86 에서는 되돌릴 것이 없다 */
iommu_pages_stop_incoherent_list(struct iommu_pages_list *list,	/* [한국어] 목록 단위 캐시 관리 종료 */
				 struct device *dma_dev)	/* [한국어] 쓰지 않는 인자 */
{
	/*
	 * For performance leave the incoherent flag alone which turns this into
	 * a NOP. For X86 the rest of the stop/free flow ignores the flag.
	 */
}
/*
 * [한국어]
 * iommu_pages_free_incoherent - (x86) 비일관 페이지를 반납한다
 *
 * @virt:    반납할 페이지의 커널 가상 주소.
 * @dma_dev: 캐시 관리를 맡았던 장치. x86 에서는 쓰이지 않는다.
 *
 * non-x86 판은 dma_unmap_single() 로 매핑을 먼저 되돌린 뒤 페이지를 반납해야
 * 한다 — 순서를 지키지 않으면 반납된 페이지에 IOMMU 가 계속 접근할 수 있다.
 * x86 에는 그 매핑이 없으므로 보통의 iommu_free_pages() 와 완전히 같다.
 *
 * 그래도 별도 함수로 두는 이유는 호출부(io-pgtable 의 테이블 해제 경로)가
 * 아키텍처를 신경 쓰지 않고 같은 이름을 부를 수 있게 하기 위해서다.
 *
 * 실행 컨텍스트: 페이지 테이블 해제(프로세스 문맥).
 *
 * 호출 체인:
 *   io-pgtable 의 테이블 해제 → [이 함수] → iommu_free_pages()
 */
static inline void iommu_pages_free_incoherent(void *virt,	/* [한국어] x86 의 비일관 페이지 해제 */
					       struct device *dma_dev)	/* [한국어] 쓰지 않는 인자 */
{
	iommu_free_pages(virt);	/* [한국어] 되돌릴 DMA 매핑이 없으므로 보통의 해제와 같다 */
}
#else	/* [한국어] x86 이 아닌 아키텍처 (주로 ARM) */
#define IOMMU_PAGES_USE_DMA_API 1	/* [한국어] DMA API 를 캐시 관리 수단으로 빌려 쓴다. 기존 ARM 드라이버들의 관행을 유지한 것이며, 그 대가로 매핑의 수명을 추적해야 한다 */
#include <linux/dma-mapping.h>	/* [한국어] dma_sync/map/unmap */

/*
 * [한국어]
 * iommu_pages_flush_incoherent - (non-x86) 페이지 테이블 수정분을 DMA API 로 밀어낸다
 *
 * @dma_dev: 캐시 관리를 대행할 장치 — 보통 IOMMU 자신의 struct device.
 *           x86 판과 달리 여기서는 반드시 필요하다. DMA API 가 어느 장치의
 *           관점에서 캐시를 정리할지 알아야 하기 때문이다.
 * @virt:    수정한 페이지 테이블의 커널 가상 주소.
 * @offset:  그 페이지 안에서 고친 부분의 시작 오프셋.
 * @len:     고친 길이(바이트).
 *
 * 위 x86 판과 같은 목적이지만 수단이 다르다. ARM 계열에는 유저 코드가 부를 수
 * 있는 캐시 정리 명령이 없어, 기존 IOMMU 드라이버들은 관행적으로 DMA API 를
 * 캐시 관리 수단으로 빌려 써 왔다. IOMMU_PAGES_USE_DMA_API 가 1 인 것이 그 뜻이다.
 *
 * 그 대가가 이 파일의 incoherent 플래그 추적이다. dma_sync_single_for_device()
 * 는 대상 페이지가 이미 dma_map_single() 로 매핑되어 있을 때만 성립하므로,
 * 페이지마다 매핑 여부를 기억해 두었다가 해제 때 되돌려야 한다.
 *
 * 방향이 DMA_TO_DEVICE 인 이유: IOMMU 는 페이지 테이블을 읽기만 한다. CPU 가
 * 쓴 내용을 장치가 볼 수 있게 밀어내는 방향이면 충분하다.
 *
 * 실행 컨텍스트: 매핑/해제 핫패스. 잠들지 않는다.
 *
 * 호출 체인:
 *   io-pgtable 구현의 엔트리 기록 직후 → [이 함수] → dma_sync_single_for_device()
 */
static inline void iommu_pages_flush_incoherent(struct device *dma_dev,
						void *virt, size_t offset,
						size_t len)
{
	dma_sync_single_for_device(dma_dev, (uintptr_t)virt + offset, len,	/* [한국어] DMA API 로 캐시를 밀어낸다. 이 호출이 성립하려면 그 페이지가 이미 dma_map_single 로 매핑되어 있어야 하고, 그것이 incoherent 플래그가 추적하는 상태다 */
				   DMA_TO_DEVICE);	/* [한국어] IOMMU 가 페이지 테이블을 읽기만 하므로 이 방향 */
}
void iommu_pages_stop_incoherent_list(struct iommu_pages_list *list,	/* [한국어] 실제 구현이 필요하다 — DMA 매핑을 되돌려야 한다 */
				      struct device *dma_dev);	/* [한국어] 캐시 관리를 맡은 장치 */
void iommu_pages_free_incoherent(void *virt, struct device *dma_dev);	/* [한국어] 매핑을 되돌린 뒤 해제한다 */
#endif

#endif /* __IOMMU_PAGES_H */
