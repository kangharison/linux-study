// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 *
 * The iopt_pages is the center of the storage and motion of PFNs. Each
 * iopt_pages represents a logical linear array of full PFNs. The array is 0
 * based and has npages in it. Accessors use 'index' to refer to the entry in
 * this logical array, regardless of its storage location.
 *
 * PFNs are stored in a tiered scheme:
 *  1) iopt_pages::pinned_pfns xarray
 *  2) An iommu_domain
 *  3) The origin of the PFNs, i.e. the userspace pointer
 *
 * PFN have to be copied between all combinations of tiers, depending on the
 * configuration.
 *
 * When a PFN is taken out of the userspace pointer it is pinned exactly once.
 * The storage locations of the PFN's index are tracked in the two interval
 * trees. If no interval includes the index then it is not pinned.
 *
 * If access_itree includes the PFN's index then an in-kernel access has
 * requested the page. The PFN is stored in the xarray so other requestors can
 * continue to find it.
 *
 * If the domains_itree includes the PFN's index then an iommu_domain is storing
 * the PFN and it can be read back using iommu_iova_to_phys(). To avoid
 * duplicating storage the xarray is not used if only iommu_domains are using
 * the PFN's index.
 *
 * As a general principle this is designed so that destroy never fails. This
 * means removing an iommu_domain or releasing a in-kernel access will not fail
 * due to insufficient memory. In practice this means some cases have to hold
 * PFNs in the xarray even though they are also being stored in an iommu_domain.
 *
 * While the iopt_pages can use an iommu_domain as storage, it does not have an
 * IOVA itself. Instead the iopt_area represents a range of IOVA and uses the
 * iopt_pages as the PFN provider. Multiple iopt_areas can share the iopt_pages
 * and reference their own slice of the PFN array, with sub page granularity.
 *
 * In this file the term 'last' indicates an inclusive and closed interval, eg
 * [0,0] refers to a single PFN. 'end' means an open range, eg [0,0) refers to
 * no PFNs.
 *
 * Be cautious of overflow. An IOVA can go all the way up to U64_MAX, so
 * last_iova + 1 can overflow. An iopt_pages index will always be much less than
 * ULONG_MAX so last_index + 1 cannot overflow.
 */
/*
 * [한국어 설명] PFN 의 저장과 이동을 맡는 곳 (pages.c)
 *
 * === 파일의 역할 ===
 * 위 영어 주석이 이 파일의 설계를 이미 밝혀 두었다. 요약하면, iopt_pages
 * 하나는 "PFN 이 죽 늘어선 논리 배열" 하나를 뜻하고, 그 PFN 들이 어디에
 * 저장돼 있든 index 라는 번호로 가리킨다.
 *
 * 저장 자리가 세 층이다. 사용자 포인터가 원천이고, 그것을 고정(pin)해
 * 얻은 PFN 은 iommu 도메인 안에 있거나 xarray 안에 있다. 같은 PFN 을 두
 * 곳에 중복 저장하지 않으려고, 도메인만 쓰고 있으면 xarray 를 비워 두고
 * 필요할 때 iommu_iova_to_phys() 로 되읽는다.
 *
 * 어디에 있는지는 두 구간 트리가 기억한다 — access_itree 는 커널 접근자가
 * 붙잡은 구간, domains_itree 는 도메인이 들고 있는 구간이다. 어느 트리도
 * 덮지 않는 index 는 고정돼 있지 않다.
 *
 * "파괴는 결코 실패하지 않는다"가 이 파일의 큰 원칙이다. 도메인을 떼거나
 * 접근자를 놓는 일이 메모리 부족으로 실패하면 되돌릴 길이 없기 때문이다.
 * 그래서 원 주석대로, 도메인에도 있는 PFN 을 xarray 에 굳이 남겨 두는
 * 경우가 생긴다 — 나중에 되읽으려면 할당이 필요할 수 있어서다.
 *
 * pfn_batch 가 이 파일의 일꾼이다. PFN 을 하나씩 다루면 락을 잡았다 놓는
 * 횟수가 너무 많아지므로, 이어진 PFN 을 (시작, 개수) 쌍으로 묶어 한 번에
 * 옮긴다. 큰 페이지는 그 묶음 하나로 표현된다.
 *
 * 고정 개수 회계도 여기서 한다. 사용자가 잠글 수 있는 메모리에는 한도가
 * 있어, 고정할 때 그 한도를 확인하고 놓을 때 되돌려 준다.
 *
 * dmabuf 지원이 뒤늦게 더해졌다. 장치 메모리를 dmabuf 로 받아 매핑하는
 * 경로로, 사용자 페이지가 아니라 MMIO 라 캐시 속성이 다르고 고정 회계도
 * 하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * io_pagetable.c (영역 관리) → 이 파일 (PFN 관리) → iommu 도메인
 *   또는 pin_user_pages() → 사용자 메모리
 *
 * device.c 의 접근자 경로 → iopt_pages_rw_access / iopt_area_add_access
 *   → 이 파일 → 사용자 페이지
 *
 * 실행 컨텍스트: 모두 프로세스 문맥. 사용자 메모리를 다루므로 mm 을
 * 빌려 쓰는 경우가 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위: io_pagetable.c 가 영역을 채우고 비울 때 이 파일을 부른다.
 *   device.c 의 접근자가 페이지를 고정하거나 읽고 쓸 때도 부른다.
 * 아래: mm 의 pin_user_pages / unpin_user_pages, iommu 코어의 map/unmap,
 *   dma-buf 코어.
 *
 * === 주요 함수/구조체 요약 ===
 * struct pfn_batch: 이어진 PFN 을 (시작, 개수) 쌍의 배열로 담는 임시 그릇.
 * struct pfn_reader: 한 구간의 PFN 을 세 층 어디에서든 꺼내 오는 반복자.
 *   fill_span 이 그 층 선택을 한다.
 * iopt_area_fill_domains: 새 영역의 PFN 을 모든 도메인에 채운다.
 * iopt_area_fill_domain: 새 도메인 하나에 기존 영역들을 채운다.
 * iopt_pages_fill_xarray / unfill_xarray: 커널 접근자를 위해 xarray 층을
 *   채우고 비운다.
 * iopt_pages_rw_access: CPU 로 그 메모리를 읽고 쓴다.
 * iopt_area_add_access / remove_access: 접근자의 고정을 세고 관리한다.
 */
#include <linux/dma-buf.h>	/* [한국어] 장치 메모리를 dmabuf 로 받아 매핑하는 경로에 필요하다. */
#include <linux/dma-resv.h>	/* [한국어] dmabuf 의 예약 락. 붙이고 고정할 때 이 락을 쥐어야 한다. */
#include <linux/file.h>	/* [한국어] memfd 원천의 파일 참조를 들고 놓는 데 쓴다. */
#include <linux/highmem.h>	/* [한국어] kmap_local_page — highmem 페이지를 잠시 커널 주소로 매핑한다. */
#include <linux/iommu.h>	/* [한국어] iommu_map / iommu_unmap / iommu_iova_to_phys. */
#include <linux/iommufd.h>	/* [한국어] 접근자 플래그 등 이 모듈이 내보내는 정의들. */
#include <linux/kthread.h>	/* [한국어] kthread_use_mm — 커널 스레드가 사용자 주소 공간을 잠시 빌려 쓴다. */
#include <linux/overflow.h>	/* [한국어] check_add_overflow 계열. 이 파일은 넘침에 특히 조심한다(맨 위 주석 참고). */
#include <linux/slab.h>	/* [한국어] kmalloc / kfree. */
#include <linux/sched/mm.h>	/* [한국어] mmget / mmgrab / __account_locked_vm 등 mm 참조와 회계 함수. */
#include <linux/vfio_pci_core.h>	/* [한국어] dmabuf 물리 주소를 얻는 vfio 함수의 서명. 실제 호출은 심볼로 찾아 한다. */

#include "double_span.h"	/* [한국어] 두 구간 트리를 겹쳐 훑는 반복자. 저장 층 선택의 바탕이다. */
#include "io_pagetable.h"	/* [한국어] 영역과 pages 의 자료 구조 정의. */

#ifndef CONFIG_IOMMUFD_TEST	/* [한국어] 임시 버퍼 한도를 테스트가 바꿀 수 있게 갈라 둔다. */
#define TEMP_MEMORY_LIMIT 65536	/* [한국어] 평소에는 64KB 고정. 위 temp_kmalloc 주석대로 4K 페이지 26M개, 2M 페이지 13G개를 담는다. */
#else	/* [한국어] 테스트 빌드라면 */
#define TEMP_MEMORY_LIMIT iommufd_test_memory_limit	/* [한국어] 실행 중에 바꿀 수 있는 변수로 만든다. 아주 작게 잡아 여러 조각으로 나뉘는 경로를 억지로 타게 하려는 것이다. */
#endif	/* [한국어] 한도 정의의 끝. */
#define BATCH_BACKUP_SIZE 32	/* [한국어] 파괴 경로가 스택에 잡는 보루 배열의 크기. 실패할 수 없는 경로가 최소한 이만큼은 쓸 수 있게 보장한다. */

/*
 * More memory makes pin_user_pages() and the batching more efficient, but as
 * this is only a performance optimization don't try too hard to get it. A 64k
 * allocation can hold about 26M of 4k pages and 13G of 2M pages in an
 * pfn_batch. Various destroy paths cannot fail and provide a small amount of
 * stack memory as a backup contingency. If backup_len is given this cannot
 * fail.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * temp_kmalloc - 임시 작업 버퍼를 얻는다. 실패해도 되는 할당이다
 *
 * @size: 원하는 크기. 실제로 얻은 크기를 여기에 되돌려 쓴다.
 * @backup: 호출자가 준 스택 버퍼(없으면 NULL).
 * @backup_len: 그 크기.
 * @return: 얻은 버퍼. backup 이 있으면 절대 NULL 이 아니다.
 *
 * 원 주석이 요점을 밝힌다 — 큰 버퍼는 성능에만 좋을 뿐이라, 얻지 못하면
 * 작은 것으로 물러서면 된다. 그래서 NOWARN·NORETRY 로 "있으면 주고
 * 없으면 말라"고 요청한다.
 *
 * backup 이 있으면 실패하지 않는다는 것이 중요하다. 파괴 경로는 실패할
 * 수 없어, 호출자가 스택 배열을 마지막 보루로 준비해 둔다.
 */
static void *temp_kmalloc(size_t *size, void *backup, size_t backup_len)
{
	void *res;	/* [한국어] 얻은 버퍼. */

	if (WARN_ON(*size == 0))	/* [한국어] 0 바이트를 달라는 것은 호출자의 버그다. */
		return NULL;	/* [한국어] 아무것도 주지 않는다. */

	if (*size < backup_len)	/* [한국어] 스택 버퍼로 충분하면 */
		return backup;	/* [한국어] 할당 자체를 하지 않는다. */

	if (!backup && iommufd_should_fail())	/* [한국어] 보루가 없는 경로에서만 결함 주입 시험을 한다 — 실패할 수 있는 경로라야 뜻이 있다. */
		return NULL;	/* [한국어] 시험용 실패. */

	*size = min_t(size_t, *size, TEMP_MEMORY_LIMIT);	/* [한국어] 아무리 커도 한도 안에서만 잡는다. 이것은 성능 최적화일 뿐이다. */
	res = kmalloc(*size, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);	/* [한국어] "있으면 주고 없으면 말라". 경고도 남기지 않고 되풀이 시도도 하지 않는다. */
	if (res)	/* [한국어] 얻었으면 */
		return res;	/* [한국어] 그대로 쓴다. */
	*size = PAGE_SIZE;	/* [한국어] 못 얻었으니 한 페이지로 물러선다. */
	if (backup_len) {	/* [한국어] 보루가 있는 경로라면 */
		res = kmalloc(*size, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);	/* [한국어] 한 페이지라도 얻어 본다. */
		if (res)	/* [한국어] 한 페이지라도 얻었으면 그것으로 일한다. */
			return res;
		*size = backup_len;	/* [한국어] 그것도 못 얻으면 */
		return backup;	/* [한국어] 스택 버퍼로 간다. 이 경로는 실패할 수 없다. */
	}
	return kmalloc(*size, GFP_KERNEL);	/* [한국어] 보루가 없으면 평범한 요청으로 한 번 더 — 여기서는 회수 압박까지 하며 기다린다. */
}

/*
 * [한국어]
 * interval_tree_double_span_iter_update - 두 트리의 현재 상태를 종합한다
 *
 * @iter: 갱신할 반복자.
 *
 * 두 구간 트리를 겹쳐 놓고 "이 index 는 누가 덮고 있는가"를 답하는 것이
 * 이 반복자의 일이다. 이 파일에서는 access_itree 와 domains_itree 를
 * 겹쳐, 어느 PFN 이 어디에 저장돼 있는지 한 번에 훑는다.
 *
 * 규칙: 한쪽이라도 덮고 있으면 그쪽이 이긴다(is_used 가 1 또는 2). 둘 다
 * 비어 있을 때만 구멍이다. 이긴 구간의 끝은 다른 쪽 구멍의 끝으로 잘리는데,
 * 그래야 "덮인 상태가 이어지는 최대 구간"이 정확해진다.
 *
 * 하나라도 끝났으면 전체를 끝낸 것으로 표시한다(-1).
 */
void interval_tree_double_span_iter_update(
	struct interval_tree_double_span_iter *iter)
{
	unsigned long last_hole = ULONG_MAX;	/* [한국어] 지금까지 본 구멍들의 끝 중 가장 앞선 것. */
	unsigned int i;	/* [한국어] 두 트리를 훑는 첨자. */

	for (i = 0; i != ARRAY_SIZE(iter->spans); i++) {	/* [한국어] 두 트리를 차례로 본다. */
		if (interval_tree_span_iter_done(&iter->spans[i])) {	/* [한국어] 한쪽이라도 끝났으면 */
			iter->is_used = -1;	/* [한국어] 전체를 끝으로 표시하고 */
			return;	/* [한국어] 돌아간다. */
		}

		if (iter->spans[i].is_hole) {	/* [한국어] 이 트리에서는 비어 있는 구간이면 */
			last_hole = min(last_hole, iter->spans[i].last_hole);	/* [한국어] 그 구멍의 끝을 기억해 두고 */
			continue;	/* [한국어] 다음 트리를 본다. */
		}

		iter->is_used = i + 1;	/* [한국어] 덮고 있는 트리를 찾았다. 1 은 첫 번째, 2 는 두 번째. */
		iter->start_used = iter->spans[i].start_used;	/* [한국어] 덮인 구간의 시작. */
		iter->last_used = min(iter->spans[i].last_used, last_hole);	/* [한국어] 앞서 본 구멍의 끝으로 자른다 — 그 지점에서 다른 트리의 상태가 바뀌므로, 여기까지가 "같은 상태가 이어지는 최대 구간"이다. */
		return;	/* [한국어] 판정을 마쳤다. */
	}

	iter->is_used = 0;	/* [한국어] 둘 다 비어 있다 = 구멍이다. */
	iter->start_hole = iter->spans[0].start_hole;	/* [한국어] 두 구멍의 시작은 같다 — 같은 지점에서 함께 전진하기 때문이다. */
	iter->last_hole =	/* [한국어] 두 구멍이 함께 끝나는 지점. */
		min(iter->spans[0].last_hole, iter->spans[1].last_hole);	/* [한국어] 앞서 끝나는 쪽이 경계다. */
}

/*
 * [한국어]
 * interval_tree_double_span_iter_first - 겹친 순회를 시작한다
 *
 * @iter: 초기화할 반복자.
 * @itree1: 첫 번째 구간 트리.
 * @itree2: 두 번째 구간 트리.
 * @first_index: 훑을 첫 번째 index.
 * @last_index: 마지막 index(포함).
 *
 * 두 트리 각각의 반복자를 같은 지점에서 시작시킨 뒤 종합한다.
 */
void interval_tree_double_span_iter_first(
	struct interval_tree_double_span_iter *iter,
	struct rb_root_cached *itree1, struct rb_root_cached *itree2,
	unsigned long first_index, unsigned long last_index)
{
	unsigned int i;	/* [한국어] 순회용 첨자. */

	iter->itrees[0] = itree1;	/* [한국어] 나중에 전진시킬 때 다시 필요하므로 기억해 둔다. */
	iter->itrees[1] = itree2;	/* [한국어] 두 번째 트리. */
	for (i = 0; i != ARRAY_SIZE(iter->spans); i++)	/* [한국어] 두 반복자를 */
		interval_tree_span_iter_first(&iter->spans[i], iter->itrees[i],	/* [한국어] 같은 지점에서 시작시킨다. */
					      first_index, last_index);
	interval_tree_double_span_iter_update(iter);	/* [한국어] 두 상태를 종합해 첫 구간을 정한다. */
}

/*
 * [한국어]
 * interval_tree_double_span_iter_next - 다음 구간으로 넘어간다
 *
 * @iter: 진행할 반복자.
 *
 * 구멍의 끝 다음 지점으로 두 반복자를 함께 옮긴다. 한쪽만 옮기면 두
 * 트리의 진행이 어긋나 종합이 뜻을 잃는다.
 */
void interval_tree_double_span_iter_next(
	struct interval_tree_double_span_iter *iter)
{
	unsigned int i;	/* [한국어] 순회용 첨자. */

	if (iter->is_used == -1 ||	/* [한국어] 이미 끝났거나 */
	    iter->last_hole == iter->spans[0].last_index) {	/* [한국어] 요청한 범위의 끝에 닿았으면 */
		iter->is_used = -1;	/* [한국어] 끝으로 표시하고 */
		return;	/* [한국어] 돌아간다. */
	}

	for (i = 0; i != ARRAY_SIZE(iter->spans); i++)	/* [한국어] 두 반복자를 함께 */
		interval_tree_span_iter_advance(	/* [한국어] 같은 지점으로 옮긴다. 한쪽만 옮기면 종합이 뜻을 잃는다. */
			&iter->spans[i], iter->itrees[i], iter->last_hole + 1);
	interval_tree_double_span_iter_update(iter);	/* [한국어] 새 구간을 판정한다. */
}

/*
 * [한국어]
 * iopt_pages_add_npinned - 고정된 페이지 수를 늘린다
 *
 * @pages: 대상 pages 객체.
 * @npages: 늘릴 개수.
 *
 * 넘침 검사를 하는 이유: 이 값이 잘못되면 사용자가 잠근 메모리 회계가
 * 어긋나 한도를 우회할 수 있다.
 *
 * 검사 결과를 테스트 빌드에서만 보는 것은, 정상적으로는 일어날 수 없어
 * 늘 확인하기에는 비용이 아깝기 때문이다.
 */
static void iopt_pages_add_npinned(struct iopt_pages *pages, size_t npages)
{
	int rc;	/* [한국어] 넘침 여부. */

	rc = check_add_overflow(pages->npinned, npages, &pages->npinned);	/* [한국어] 더하면서 넘쳤는지 함께 본다. */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 평소에는 확인 비용이 아까워 테스트 빌드에서만 본다. */
		WARN_ON(rc || pages->npinned > pages->npages);	/* [한국어] 넘쳤거나 전체 페이지 수보다 많아졌으면 회계가 깨진 것이다. */
}

/*
 * [한국어]
 * iopt_pages_sub_npinned - 고정된 페이지 수를 줄인다
 *
 * @pages: 대상 pages 객체.
 * @npages: 줄일 개수.
 *
 * 위와 짝이다. 아래로 넘치면(0 보다 많이 빼면) 어딘가에서 두 번 놓은
 * 것이라 회계가 깨진다.
 */
static void iopt_pages_sub_npinned(struct iopt_pages *pages, size_t npages)
{
	int rc;	/* [한국어] 아래로 넘쳤는지 여부. */

	rc = check_sub_overflow(pages->npinned, npages, &pages->npinned);	/* [한국어] 빼면서 0 아래로 내려갔는지 함께 본다. */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드에서만 확인한다. */
		WARN_ON(rc || pages->npinned > pages->npages);	/* [한국어] 아래로 넘치면 아주 큰 값이 되어 이 비교에 걸린다. */
}

/*
 * [한국어]
 * iopt_pages_err_unpin - 실패 경로에서 방금 고정한 것을 되돌린다
 *
 * @pages: 대상 pages 객체.
 * @start_index: 되돌릴 첫 index.
 * @last_index: 마지막 index(포함).
 * @page_list: 그 페이지들의 배열.
 *
 * 고정과 회계를 짝지어 되돌린다. 둘 중 하나만 하면 어긋난다.
 */
static void iopt_pages_err_unpin(struct iopt_pages *pages,
				 unsigned long start_index,
				 unsigned long last_index,
				 struct page **page_list)
{
	unsigned long npages = last_index - start_index + 1;	/* [한국어] 포함 구간이므로 +1. */

	unpin_user_pages(page_list, npages);	/* [한국어] 고정을 놓는다. */
	iopt_pages_sub_npinned(pages, npages);	/* [한국어] 회계도 함께 되돌린다. 둘 중 하나만 하면 어긋난다. */
}

/*
 * index is the number of PAGE_SIZE units from the start of the area's
 * iopt_pages. If the iova is sub page-size then the area has an iova that
 * covers a portion of the first and last pages in the range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_index_to_iova - 페이지 번호를 그 영역 안의 IOVA 로 바꾼다
 *
 * @area: 대상 영역.
 * @index: pages 배열 안의 페이지 번호.
 * @return: 그 페이지가 시작하는 IOVA.
 *
 * 원 주석이 page_offset 의 존재 이유를 밝힌다 — IOVA 가 페이지 경계에
 * 맞지 않으면, 첫 페이지와 마지막 페이지는 일부만 이 영역에 속한다.
 *
 * 첫 페이지를 따로 다루는 이유가 그것이다. 그 페이지의 시작 IOVA 는
 * 페이지 경계가 아니라 영역의 시작 주소다.
 */
static unsigned long iopt_area_index_to_iova(struct iopt_area *area,
					     unsigned long index)
{
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 평소에는 확인 비용이 아까워 테스트 빌드에서만 본다. */
		WARN_ON(index < iopt_area_index(area) ||	/* [한국어] 이 영역이 덮지 않는 번호를 물으면 버그다. */
			index > iopt_area_last_index(area));
	index -= iopt_area_index(area);	/* [한국어] 영역 안에서 몇 번째 페이지인지로 바꾼다. */
	if (index == 0)	/* [한국어] 첫 페이지는 특별하다 — */
		return iopt_area_iova(area);	/* [한국어] 그 시작은 페이지 경계가 아니라 영역의 시작 주소다. */
	return iopt_area_iova(area) - area->page_offset + index * PAGE_SIZE;	/* [한국어] 둘째 페이지부터는 페이지 경계에 맞는다. 오프셋을 빼 페이지 경계를 얻은 뒤 페이지 수만큼 더한다. */
}

/*
 * [한국어]
 * iopt_area_index_to_iova_last - 페이지 번호를 그 페이지의 마지막 IOVA 로 바꾼다
 *
 * @area: 대상 영역.
 * @index: 페이지 번호.
 * @return: 그 페이지가 덮는 마지막 IOVA(포함).
 *
 * 위와 짝이며, 마지막 페이지를 따로 다룬다 — 그 페이지도 일부만 이
 * 영역에 속할 수 있어 영역의 끝 주소가 답이다.
 */
static unsigned long iopt_area_index_to_iova_last(struct iopt_area *area,
						  unsigned long index)
{
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 같은 이유로 테스트 빌드 전용 확인. */
		WARN_ON(index < iopt_area_index(area) ||	/* [한국어] 범위를 벗어난 물음은 버그다. */
			index > iopt_area_last_index(area));
	if (index == iopt_area_last_index(area))	/* [한국어] 마지막 페이지는 특별하다 — */
		return iopt_area_last_iova(area);	/* [한국어] 그 끝은 페이지 경계가 아니라 영역의 끝 주소다. */
	return iopt_area_iova(area) - area->page_offset +	/* [한국어] 그 밖에는 페이지 경계에 맞는다. */
	       (index - iopt_area_index(area) + 1) * PAGE_SIZE - 1;	/* [한국어] 다음 페이지의 시작에서 1 을 뺀 값이 이 페이지의 마지막 주소다. */
}

/*
 * [한국어]
 * iommu_unmap_nofail - 요청한 만큼 정확히 풀리는지 확인하며 푼다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @size: 길이.
 *
 * 원 주석이 못 박듯, 요청한 것과 다르게 풀렸다면 이 코드의 논리 오류이거나
 * 드라이버 버그다. 그래서 실패를 돌려주지 않고 경고만 남긴다.
 *
 * 이것이 성립하려면 드라이버가 unmap 경로에서 메모리를 할당하지 않아야
 * 한다 — 할당은 실패할 수 있고, 그러면 부분만 풀리기 때문이다.
 */
static void iommu_unmap_nofail(struct iommu_domain *domain, unsigned long iova,
			       size_t size)
{
	size_t ret;	/* [한국어] 실제로 풀린 크기. */

	ret = iommu_unmap(domain, iova, size);	/* [한국어] 도메인에서 매핑을 걷는다. */
	/*
	 * It is a logic error in this code or a driver bug if the IOMMU unmaps
	 * something other than exactly as requested. This implies that the
	 * iommu driver may not fail unmap for reasons beyond bad agruments.
	 * Particularly, the iommu driver may not do a memory allocation on the
	 * unmap path.
	 */
	WARN_ON(ret != size);	/* [한국어] 원 주석대로 다르면 이 코드의 논리 오류이거나 드라이버 버그다. 되돌릴 방법이 없어 경고만 남긴다. */
}

/*
 * [한국어]
 * iopt_area_unmap_domain_range - 영역의 일부 페이지 범위를 도메인에서 푼다
 *
 * @area: 대상 영역.
 * @domain: 그 도메인.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 *
 * 페이지 번호를 IOVA 범위로 바꿔 한 번에 푼다. 첫 페이지와 마지막 페이지가
 * 일부만 속할 수 있어 위 두 변환 함수를 함께 쓴다.
 */
static void iopt_area_unmap_domain_range(struct iopt_area *area,
					 struct iommu_domain *domain,
					 unsigned long start_index,
					 unsigned long last_index)
{
	unsigned long start_iova = iopt_area_index_to_iova(area, start_index);	/* [한국어] 시작 페이지의 IOVA. */

	iommu_unmap_nofail(domain, start_iova,	/* [한국어] 한 번에 푼다. */
			   iopt_area_index_to_iova_last(area, last_index) -	/* [한국어] 마지막 페이지의 끝 주소에서 */
				   start_iova + 1);
}

/*
 * [한국어]
 * iopt_pages_find_domain_area - 이 index 를 들고 있는 영역을 찾는다
 *
 * @pages: 대상 pages 객체.
 * @index: 찾는 페이지 번호.
 * @return: 그 영역, 없으면 NULL.
 *
 * 도메인에서 PFN 을 되읽으려면 그 PFN 이 어느 IOVA 에 있는지 알아야 하고,
 * 그것을 아는 것은 영역이다. domains_itree 가 그 관계를 담는다.
 *
 * 여러 영역이 같은 index 를 들고 있을 수 있는데, 아무거나 하나면 된다 —
 * 어느 쪽으로 읽어도 같은 PFN 이 나온다.
 */
static struct iopt_area *iopt_pages_find_domain_area(struct iopt_pages *pages,
						     unsigned long index)
{
	struct interval_tree_node *node;	/* [한국어] 트리에서 찾은 마디. */

	node = interval_tree_iter_first(&pages->domains_itree, index, index);	/* [한국어] 이 번호 하나를 덮는 영역을 찾는다. */
	if (!node)	/* [한국어] 어느 도메인도 이 번호를 들고 있지 않다. */
		return NULL;	/* [한국어] 없다. */
	return container_of(node, struct iopt_area, pages_node);	/* [한국어] 마디에서 영역으로 되짚는다. 여러 영역이 겹쳐도 아무거나 하나면 된다. */
}

/* [한국어] 묶음에 담긴 PFN 의 성격.
 * 매핑할 때의 캐시 속성이 달라 섞을 수 없다. 그래서 한 묶음에는 한 종류만
 * 담고, 종류가 바뀌면 묶음을 끊는다. */
enum batch_kind {
	/* [한국어] 보통의 시스템 메모리에서 온 PFN.
	 *  설정자: batch_add_pfn 계열이 기본으로 넣는다.
	 *  읽는 자: batch_to_domain 이 캐시 가능한 매핑을 만든다.
	 *  값 0 이라 batch_clear 로 비우면 저절로 이 종류가 된다. */
	BATCH_CPU_MEMORY = 0,
	/* [한국어] 장치 레지스터 창(dmabuf)에서 온 PFN.
	 *  설정자: pfn_reader_fill_dmabuf.
	 *  읽는 자: batch_to_domain 이 IOMMU_CACHE 를 끄고 IOMMU_MMIO 를 켠다 —
	 *  장치 레지스터를 캐시하면 쓰기가 늦게 도착하거나 읽기가 낡은 값을 준다.
	 *  한 묶음에 두 종류를 섞을 수 없어, 종류가 바뀌면 묶음을 끊는다. */
	BATCH_MMIO,
};

/*
 * A simple datastructure to hold a vector of PFNs, optimized for contiguous
 * PFNs. This is used as a temporary holding memory for shuttling pfns from one
 * place to another. Generally everything is made more efficient if operations
 * work on the largest possible grouping of pfns. eg fewer lock/unlock cycles,
 * better cache locality, etc
 */
struct pfn_batch {
	/* [한국어] 각 항목의 시작 PFN 배열.
	 *  설정자: batch_add_pfn_num. 읽는 자: batch_to_domain, batch_unpin, batch_rw.
	 *  __batch_init 이 npfns 와 한 덩어리로 잡아 앞쪽 절반을 이것에 준다.
	 *  동기화: 묶음은 한 스레드의 지역 작업 그릇이라 락이 없다. */
	unsigned long *pfns;
	/* [한국어] 각 항목이 몇 개의 이어진 PFN 을 뜻하는지.
	 *  설정자·읽는 자는 위와 같다.
	 *  u32 인 이유: 한 항목이 4G 페이지(16TB)까지 표현할 수 있으면 충분하고,
	 *  64비트로 잡으면 배열이 두 배가 된다.
	 *  값 범위: 1 .. MAX_NPFNS. */
	u32 *npfns;
	/* [한국어] 두 배열이 담을 수 있는 항목 수.
	 *  설정자: __batch_init 이 실제로 얻은 메모리에 맞춰 정한다.
	 *  읽는 자: batch_add_pfn_num 이 자리가 남았는지 볼 때.
	 *  batch_from_domain_continue 가 이 값을 잠시 줄여 "이어지는 것만
	 *  받는" 상태를 만든다. */
	unsigned int array_size;
	/* [한국어] 지금까지 쓴 항목 수(= 다음에 쓸 자리).
	 *  설정자: 넣고 빼는 함수들. 읽는 자: 묶음을 훑는 모든 곳.
	 *  값 범위: 0 .. array_size. */
	unsigned int end;
	/* [한국어] 항목들이 뜻하는 PFN 의 총 개수.
	 *  end 와 다르다 — 한 항목이 여러 PFN 을 뜻하기 때문이다.
	 *  읽는 자: pfn_reader_next 가 얼마나 진행했는지 셀 때, 그리고 담긴
	 *  개수가 늘지 않았는지로 "묶음이 찼다"를 판정할 때. */
	unsigned int total_pfns;
	/* [한국어] 이 묶음에 담긴 PFN 의 성격.
	 *  설정자: 첫 항목을 넣을 때 정해진다. 읽는 자: batch_to_domain.
	 *  한 묶음에 한 종류만 담기므로, 종류가 다른 PFN 이 오면 묶음을 끊는다. */
	enum batch_kind kind;
};
/* [한국어] 한 항목이 담을 수 있는 최대 PFN 개수.
 * npfns 원소의 타입에서 자동으로 끌어낸다 — 타입을 바꾸면 이 값도
 * 따라 바뀌어, 두 곳을 함께 고쳐야 하는 실수를 막는다. */
enum { MAX_NPFNS = type_max(typeof(((struct pfn_batch *)0)->npfns[0])) };

/*
 * [한국어]
 * batch_clear - 묶음을 비운다
 *
 * @batch: 비울 묶음.
 *
 * 배열 자체는 그대로 두고 개수만 0 으로 되돌린다 — 같은 배열을 계속
 * 재사용하는 것이 이 구조의 요점이다.
 */
static void batch_clear(struct pfn_batch *batch)
{
	batch->total_pfns = 0;	/* [한국어] 담긴 PFN 수를 0 으로. */
	batch->end = 0;	/* [한국어] 항목 수도 0 으로. */
	batch->pfns[0] = 0;	/* [한국어] 첫 항목을 깨끗이 해 둔다. */
	batch->npfns[0] = 0;	/* [한국어] 같은 이유. */
	batch->kind = 0;	/* [한국어] 종류도 기본으로 되돌린다 — 다음에 어떤 PFN 이 와도 받을 수 있게. */
}

/*
 * Carry means we carry a portion of the final hugepage over to the front of the
 * batch
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * batch_clear_carry - 묶음을 비우되 끝의 일부를 앞으로 옮겨 둔다
 *
 * @batch: 대상 묶음.
 * @keep_pfns: 남길 PFN 개수.
 *
 * 큰 페이지가 묶음 경계에 걸쳤을 때 쓴다. 그 페이지의 남은 부분을 버리면
 * 다음 바퀴에서 다시 고정해야 하므로, 앞으로 옮겨 이어서 쓴다.
 *
 * 남길 것이 없으면 그냥 비우는 것과 같다.
 */
static void batch_clear_carry(struct pfn_batch *batch, unsigned int keep_pfns)
{
	if (!keep_pfns)	/* [한국어] 남길 것이 없으면 */
		return batch_clear(batch);	/* [한국어] 그냥 비우는 것과 같다. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드 전용 확인. */
		WARN_ON(!batch->end ||	/* [한국어] 비어 있는데 남기라는 것은 앞뒤가 맞지 않는다. */
			batch->npfns[batch->end - 1] < keep_pfns);

	batch->total_pfns = keep_pfns;	/* [한국어] 남길 것만 세어 둔다. */
	batch->pfns[0] = batch->pfns[batch->end - 1] +	/* [한국어] 마지막 항목의 끝쪽 keep_pfns 개가 남을 부분이므로, */
			 (batch->npfns[batch->end - 1] - keep_pfns);	/* [한국어] 그 시작 PFN 을 계산해 첫 항목으로 옮긴다. */
	batch->npfns[0] = keep_pfns;	/* [한국어] 개수도 옮긴다. */
	batch->end = 1;	/* [한국어] 항목은 그 하나뿐이다. */
}

/*
 * [한국어]
 * batch_skip_carry - 옮겨 둔 부분의 앞쪽 일부를 건너뛴다
 *
 * @batch: 대상 묶음.
 * @skip_pfns: 건너뛸 개수.
 *
 * 옮겨 둔 부분이 이번에 필요한 범위보다 앞에서 시작할 때, 그만큼을
 * 잘라 낸다. 시작 PFN 을 올리고 개수를 줄이는 것으로 끝난다.
 */
static void batch_skip_carry(struct pfn_batch *batch, unsigned int skip_pfns)
{
	if (!batch->total_pfns)	/* [한국어] 옮겨 둔 것이 없으면 */
		return;	/* [한국어] 할 일이 없다. */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드 전용 확인. */
		WARN_ON(batch->total_pfns != batch->npfns[0]);	/* [한국어] 옮겨 둔 것은 늘 한 항목이어야 한다. */
	skip_pfns = min(batch->total_pfns, skip_pfns);	/* [한국어] 있는 것보다 많이 건너뛸 수는 없다. */
	batch->pfns[0] += skip_pfns;	/* [한국어] 시작 PFN 을 앞으로 민다. */
	batch->npfns[0] -= skip_pfns;	/* [한국어] 개수를 줄인다. */
	batch->total_pfns -= skip_pfns;	/* [한국어] 총계도 줄인다. */
}

/*
 * [한국어]
 * __batch_init - 묶음의 배열을 마련한다
 *
 * @batch: 초기화할 묶음.
 * @max_pages: 담고 싶은 최대 페이지 수.
 * @backup: 마지막 보루로 쓸 스택 버퍼(없으면 NULL).
 * @backup_len: 그 크기.
 * @return: 0 성공, backup 이 없을 때만 -ENOMEM 이 나올 수 있다.
 *
 * pfns 와 npfns 두 배열을 한 덩어리로 잡고 반씩 나눠 쓴다. 할당을 한 번만
 * 하려는 것이다.
 *
 * 실제로 얻은 크기에 맞춰 array_size 를 다시 계산하는 것이 요점 — 원하는
 * 만큼 못 얻어도 그만큼으로 일한다.
 */
static int __batch_init(struct pfn_batch *batch, size_t max_pages, void *backup,
			size_t backup_len)
{
	const size_t elmsz = sizeof(*batch->pfns) + sizeof(*batch->npfns);	/* [한국어] 항목 하나가 두 배열에서 차지하는 바이트 합. */
	size_t size = max_pages * elmsz;	/* [한국어] 원하는 전체 크기. */

	batch->pfns = temp_kmalloc(&size, backup, backup_len);	/* [한국어] 실패해도 되는 할당. 실제로 얻은 크기가 size 에 되돌아온다. */
	if (!batch->pfns)	/* [한국어] 보루가 없었고 메모리도 없었다. */
		return -ENOMEM;	/* [한국어] 실패. */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) && WARN_ON(size < elmsz))	/* [한국어] 한 항목도 못 담을 크기면 쓸 수 없다. */
		return -EINVAL;	/* [한국어] 거절. */
	batch->array_size = size / elmsz;	/* [한국어] 실제로 얻은 만큼으로 항목 수를 정한다. */
	batch->npfns = (u32 *)(batch->pfns + batch->array_size);	/* [한국어] 한 덩어리의 뒤쪽 절반을 npfns 로 쓴다. 할당을 한 번만 하려는 배치다. */
	batch_clear(batch);	/* [한국어] 빈 상태로 시작한다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * batch_init - 실패할 수 있는 경로의 묶음 초기화
 *
 * @batch: 초기화할 묶음.
 * @max_pages: 최대 페이지 수.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 보루가 없으므로 메모리가 없으면 실패한다. 되돌릴 수 있는 경로에서 쓴다.
 */
static int batch_init(struct pfn_batch *batch, size_t max_pages)
{
	return __batch_init(batch, max_pages, NULL, 0);	/* [한국어] 보루 없이 — 실패할 수 있다. */
}

/*
 * [한국어]
 * batch_init_backup - 실패할 수 없는 경로의 묶음 초기화
 *
 * @batch: 초기화할 묶음.
 * @max_pages: 최대 페이지 수.
 * @backup: 호출자의 스택 배열.
 * @backup_len: 그 크기.
 *
 * 반환값이 없다 — 스택 배열이 있어 실패할 수 없기 때문이다. 파괴 경로가
 * 이것을 쓴다.
 */
static void batch_init_backup(struct pfn_batch *batch, size_t max_pages,
			      void *backup, size_t backup_len)
{
	__batch_init(batch, max_pages, backup, backup_len);	/* [한국어] 보루가 있어 실패할 수 없으므로 반환값을 보지 않는다. */
}

/*
 * [한국어]
 * batch_destroy - 묶음의 배열을 해제한다
 *
 * @batch: 대상 묶음.
 * @backup: 초기화 때 준 스택 버퍼(없었으면 NULL).
 *
 * 스택 버퍼를 쓰고 있었다면 해제하면 안 된다. 그 판별이 이 함수의 전부다.
 */
static void batch_destroy(struct pfn_batch *batch, void *backup)
{
	if (batch->pfns != backup)	/* [한국어] 스택 버퍼를 쓰고 있었으면 해제하면 안 된다. */
		kfree(batch->pfns);	/* [한국어] 할당한 것이면 해제한다. npfns 는 같은 덩어리라 따로 해제하지 않는다. */
}

/*
 * [한국어]
 * batch_add_pfn_num - 이어진 PFN 무리를 묶음에 넣는다
 *
 * @batch: 대상 묶음.
 * @pfn: 시작 PFN.
 * @nr: 이어진 개수.
 * @kind: 그 PFN 들의 성격.
 * @return: 넣었으면 참, 자리가 없으면 거짓.
 *
 * 앞 항목과 이어지면 그 개수만 늘린다 — 이것이 큰 페이지를 한 항목으로
 * 표현하는 방식이다. 이어지지 않으면 새 항목을 쓴다.
 *
 * 성격이 다르면 아예 받지 않는다. 캐시 속성이 달라 한 번에 매핑할 수
 * 없기 때문이다.
 *
 * 거짓을 돌려주는 것은 오류가 아니라 "묶음이 찼으니 지금까지 것을 처리하고
 * 다시 오라"는 신호다.
 */
static bool batch_add_pfn_num(struct pfn_batch *batch, unsigned long pfn,
			      u32 nr, enum batch_kind kind)
{
	unsigned int end = batch->end;	/* [한국어] 지금까지 쓴 항목 수. */

	if (batch->kind != kind) {	/* [한국어] 성격이 다르면 */
		/* One kind per batch */
		if (batch->end != 0)	/* [한국어] 이미 다른 종류가 들어 있으니 */
			return false;	/* [한국어] 받지 않는다. 캐시 속성이 달라 한 번에 매핑할 수 없다. */
		batch->kind = kind;	/* [한국어] 비어 있었으면 이 종류로 정한다. */
	}

	if (end && pfn == batch->pfns[end - 1] + batch->npfns[end - 1] &&	/* [한국어] 앞 항목의 바로 다음 PFN 이고 */
	    nr <= MAX_NPFNS - batch->npfns[end - 1]) {	/* [한국어] 개수 한도도 넘지 않으면 */
		batch->npfns[end - 1] += nr;	/* [한국어] 항목을 늘리기만 한다. 큰 페이지를 한 항목으로 표현하는 방식이다. */
	} else if (end < batch->array_size) {	/* [한국어] 이어지지 않지만 자리가 남았으면 */
		batch->pfns[end] = pfn;	/* [한국어] 새 항목을 쓴다. */
		batch->npfns[end] = nr;	/* [한국어] 그 개수. */
		batch->end++;	/* [한국어] 항목 수를 늘린다. */
	} else {	/* [한국어] 자리가 없다. */
		return false;	/* [한국어] 오류가 아니라 "지금까지 것을 처리하고 다시 오라"는 신호다. */
	}

	batch->total_pfns += nr;	/* [한국어] 총계를 늘린다. */
	return true;	/* [한국어] 넣었다. */
}

/*
 * [한국어]
 * batch_remove_pfn_num - 방금 넣은 PFN 무리를 도로 뺀다
 *
 * @batch: 대상 묶음.
 * @nr: 뺄 개수.
 *
 * 넣은 뒤에 다른 이유로 실패했을 때 되돌리는 데 쓴다. 마지막 항목에서만
 * 빼므로 넣은 직후에만 뜻이 있다.
 */
static void batch_remove_pfn_num(struct pfn_batch *batch, unsigned long nr)
{
	batch->npfns[batch->end - 1] -= nr;	/* [한국어] 마지막 항목에서 뺀다 — 넣은 직후에만 뜻이 있다. */
	if (batch->npfns[batch->end - 1] == 0)	/* [한국어] 그 항목이 비었으면 */
		batch->end--;	/* [한국어] 항목 자체를 없앤다. */
	batch->total_pfns -= nr;	/* [한국어] 총계도 줄인다. */
}

/* true if the pfn was added, false otherwise */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * batch_add_pfn - PFN 하나를 묶음에 넣는다
 *
 * @batch: 대상 묶음.
 * @pfn: 넣을 PFN.
 * @return: 넣었으면 참.
 *
 * 개수 1 의 CPU 메모리로 위 함수를 부르는 얇은 껍데기다.
 */
static bool batch_add_pfn(struct pfn_batch *batch, unsigned long pfn)
{
	return batch_add_pfn_num(batch, pfn, 1, BATCH_CPU_MEMORY);	/* [한국어] 개수 1 의 시스템 메모리로 넘긴다. */
}

/*
 * Fill the batch with pfns from the domain. When the batch is full, or it
 * reaches last_index, the function will return. The caller should use
 * batch->total_pfns to determine the starting point for the next iteration.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * batch_from_domain - 도메인에서 PFN 을 읽어 묶음에 담는다
 *
 * @batch: 채울 묶음.
 * @domain: 읽어 올 도메인.
 * @area: 그 IOVA 를 아는 영역.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 *
 * xarray 를 쓰지 않고 도메인만 저장 자리로 삼았을 때, 되읽는 유일한 길이
 * iommu_iova_to_phys() 다.
 *
 * 원 주석대로 느리다. 페이지마다 한 번씩 물어봐야 하는데, 드라이버가
 * 페이지 크기를 알려 주면 큰 페이지를 한 번에 건너뛸 수 있을 것이다.
 *
 * page_offset 을 빼는 이유: 영역의 첫 페이지는 IOVA 가 페이지 한가운데를
 * 가리키므로, 그 물리 주소에서 오프셋을 빼야 페이지 시작이 나온다.
 */
static void batch_from_domain(struct pfn_batch *batch,
			      struct iommu_domain *domain,
			      struct iopt_area *area, unsigned long start_index,
			      unsigned long last_index)
{
	unsigned int page_offset = 0;	/* [한국어] 첫 페이지가 페이지 한가운데에서 시작할 때의 보정값. */
	unsigned long iova;	/* [한국어] 지금 물어볼 IOVA. */
	phys_addr_t phys;	/* [한국어] 되읽은 물리 주소. */

	iova = iopt_area_index_to_iova(area, start_index);	/* [한국어] 시작 페이지의 IOVA. */
	if (start_index == iopt_area_index(area))	/* [한국어] 영역의 첫 페이지라면 */
		page_offset = area->page_offset;	/* [한국어] 그 IOVA 는 페이지 한가운데를 가리킨다. */
	while (start_index <= last_index) {	/* [한국어] 요청한 범위를 페이지 단위로 훑는다. */
		/*
		 * This is pretty slow, it would be nice to get the page size
		 * back from the driver, or have the driver directly fill the
		 * batch.
		 */
		phys = iommu_iova_to_phys(domain, iova) - page_offset;	/* [한국어] 물리 주소를 되읽고, 오프셋을 빼 페이지 시작으로 맞춘다. */
		if (!batch_add_pfn(batch, PHYS_PFN(phys)))	/* [한국어] 묶음이 찼으면 */
			return;	/* [한국어] 여기까지만 담고 돌아간다. 호출자가 total_pfns 로 진행을 안다. */
		iova += PAGE_SIZE - page_offset;	/* [한국어] 다음 페이지의 시작으로. 첫 바퀴만 오프셋만큼 덜 간다. */
		page_offset = 0;	/* [한국어] 두 번째부터는 페이지 경계에 맞는다. */
		start_index++;	/* [한국어] 다음 페이지 번호. */
	}
}

/*
 * [한국어]
 * raw_pages_from_domain - 도메인에서 읽어 struct page 배열에 담는다
 *
 * @domain: 읽어 올 도메인.
 * @area: IOVA 를 아는 영역.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @out_pages: 채울 배열.
 * @return: 채운 다음 자리.
 *
 * 위와 같되 묶음이 아니라 페이지 포인터 배열을 채운다. 접근자에게
 * 넘겨 줄 형식이 그것이기 때문이다.
 *
 * 자리가 모자랄 일이 없어 중단 조건이 없는 것이 묶음 판과의 차이다.
 */
static struct page **raw_pages_from_domain(struct iommu_domain *domain,
					   struct iopt_area *area,
					   unsigned long start_index,
					   unsigned long last_index,
					   struct page **out_pages)
{
	unsigned int page_offset = 0;	/* [한국어] 첫 페이지 보정값. */
	unsigned long iova;	/* [한국어] 물어볼 IOVA. */
	phys_addr_t phys;	/* [한국어] 되읽은 물리 주소. */

	iova = iopt_area_index_to_iova(area, start_index);	/* [한국어] 시작 IOVA. */
	if (start_index == iopt_area_index(area))	/* [한국어] 영역의 첫 페이지라면 */
		page_offset = area->page_offset;	/* [한국어] 오프셋 보정이 필요하다. */
	while (start_index <= last_index) {	/* [한국어] 범위를 훑는다. */
		phys = iommu_iova_to_phys(domain, iova) - page_offset;	/* [한국어] 물리 주소를 얻어 페이지 시작으로 맞춘다. */
		*(out_pages++) = pfn_to_page(PHYS_PFN(phys));	/* [한국어] page 포인터로 바꿔 배열에 담는다. 접근자에게 넘길 형식이 그것이다. */
		iova += PAGE_SIZE - page_offset;	/* [한국어] 다음 페이지로. */
		page_offset = 0;	/* [한국어] 이후는 경계에 맞는다. */
		start_index++;	/* [한국어] 다음 번호. */
	}
	return out_pages;	/* [한국어] 채운 다음 자리를 알려 준다. */
}

/* Continues reading a domain until we reach a discontinuity in the pfns. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * batch_from_domain_continue - 이어지는 동안만 도메인에서 더 읽는다
 *
 * @batch: 이어 채울 묶음.
 * @domain: 읽어 올 도메인.
 * @area: IOVA 를 아는 영역.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 *
 * array_size 를 잠시 지금 개수로 줄이는 것이 요령이다. 그러면 새 항목을
 * 쓸 자리가 없어, 앞 항목과 이어지는 PFN 만 받아들이고 끊기는 순간
 * 멈춘다.
 */
static void batch_from_domain_continue(struct pfn_batch *batch,
				       struct iommu_domain *domain,
				       struct iopt_area *area,
				       unsigned long start_index,
				       unsigned long last_index)
{
	unsigned int array_size = batch->array_size;	/* [한국어] 원래 크기를 기억해 둔다. */

	batch->array_size = batch->end;	/* [한국어] 새 항목을 쓸 자리를 없앤다 — 그러면 앞 항목과 이어지는 PFN 만 받아들인다. */
	batch_from_domain(batch, domain, area, start_index, last_index);	/* [한국어] 이어지는 동안만 담긴다. */
	batch->array_size = array_size;	/* [한국어] 원래대로 되돌린다. */
}

/*
 * This is part of the VFIO compatibility support for VFIO_TYPE1_IOMMU. That
 * mode permits splitting a mapped area up, and then one of the splits is
 * unmapped. Doing this normally would cause us to violate our invariant of
 * pairing map/unmap. Thus, to support old VFIO compatibility disable support
 * for batching consecutive PFNs. All PFNs mapped into the iommu are done in
 * PAGE_SIZE units, not larger or smaller.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * batch_iommu_map_small - 늘 PAGE_SIZE 단위로만 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @paddr: 물리 주소.
 * @size: 길이.
 * @prot: 권한.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 이 함수의 존재 이유를 밝힌다 — 옛 VFIO_TYPE1_IOMMU 는 매핑
 * 한가운데를 풀 수 있어야 하는데, 큰 IOPTE 로 덮인 구간은 그럴 수 없다.
 * 그래서 아예 작은 단위로만 매핑해 어디서나 풀 수 있게 만든다.
 *
 * 중간에 실패하면 지금까지 매핑한 것을 모두 풀어 아무것도 하지 않은
 * 상태로 되돌린다.
 */
static int batch_iommu_map_small(struct iommu_domain *domain,
				 unsigned long iova, phys_addr_t paddr,
				 size_t size, int prot)
{
	unsigned long start_iova = iova;	/* [한국어] 실패했을 때 되돌릴 시작점. */
	int rc;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드 전용 확인. */
		WARN_ON(paddr % PAGE_SIZE || iova % PAGE_SIZE ||	/* [한국어] 세 값 모두 페이지 정렬이어야 한 페이지씩 나눌 수 있다. */
			size % PAGE_SIZE);

	while (size) {	/* [한국어] 한 페이지씩 */
		rc = iommu_map(domain, iova, paddr, PAGE_SIZE, prot,	/* [한국어] 따로 매핑한다. 큰 IOPTE 가 만들어지지 않아 어디서나 풀 수 있다. */
			       GFP_KERNEL_ACCOUNT);
		if (rc)	/* [한국어] 실패하면 */
			goto err_unmap;	/* [한국어] 지금까지 것을 되돌린다. */
		iova += PAGE_SIZE;	/* [한국어] 다음 페이지로. */
		paddr += PAGE_SIZE;	/* [한국어] 물리 쪽도 함께. */
		size -= PAGE_SIZE;	/* [한국어] 남은 크기를 줄인다. */
	}
	return 0;	/* [한국어] 전부 매핑했다. */

err_unmap:	/* [한국어] 실패 경로. */
	if (start_iova != iova)	/* [한국어] 한 페이지라도 걸었으면 */
		iommu_unmap_nofail(domain, start_iova, iova - start_iova);	/* [한국어] 모두 풀어 아무것도 하지 않은 상태로 만든다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * batch_to_domain - 묶음의 PFN 을 도메인에 매핑한다
 *
 * @batch: 매핑할 PFN 묶음.
 * @domain: 대상 도메인.
 * @area: IOVA 를 아는 영역.
 * @start_index: 이 묶음이 시작하는 페이지 번호.
 * @return: 0 성공, 음수면 실패.
 *
 * 묶음의 항목 하나가 매핑 한 번이 된다 — 이어진 PFN 을 한 항목으로 묶어
 * 둔 보람이 여기서 나온다.
 *
 * MMIO 묶음은 캐시 속성을 바꾼다. 장치 레지스터를 캐시하면 안 되기
 * 때문이다.
 *
 * 큰 페이지가 금지된 IOAS 는 작은 단위 매핑 함수로 돌린다.
 *
 * 중간에 실패하면 이 묶음이 매핑한 만큼만 되돌린다. 앞선 묶음들은
 * 호출자가 되돌린다.
 */
static int batch_to_domain(struct pfn_batch *batch, struct iommu_domain *domain,
			   struct iopt_area *area, unsigned long start_index)
{
	bool disable_large_pages = area->iopt->disable_large_pages;	/* [한국어] 옛 VFIO 호환으로 큰 페이지가 금지됐는가. */
	unsigned long last_iova = iopt_area_last_iova(area);	/* [한국어] 이 영역의 끝. 넘어가면 안 된다. */
	int iommu_prot = area->iommu_prot;	/* [한국어] 매핑 권한. */
	unsigned int page_offset = 0;	/* [한국어] 첫 페이지 보정값. */
	unsigned long start_iova;	/* [한국어] 실패 시 되돌릴 시작점. */
	unsigned long next_iova;	/* [한국어] 이번 항목이 끝나는 지점. */
	unsigned int cur = 0;	/* [한국어] 지금 보는 항목 번호. */
	unsigned long iova;	/* [한국어] 이번 항목이 시작하는 지점. */
	int rc;	/* [한국어] 결과 코드. */

	if (batch->kind == BATCH_MMIO) {	/* [한국어] 장치 레지스터라면 */
		iommu_prot &= ~IOMMU_CACHE;	/* [한국어] 캐시 가능 표시를 끄고 */
		iommu_prot |= IOMMU_MMIO;	/* [한국어] 장치 메모리로 표시한다. 캐시하면 쓰기가 늦게 도착한다. */
	}

	/* The first index might be a partial page */
	if (start_index == iopt_area_index(area))	/* [한국어] 영역의 첫 페이지부터 시작하면 */
		page_offset = area->page_offset;	/* [한국어] 그 페이지의 앞부분은 이 영역에 속하지 않는다. */
	next_iova = iova = start_iova =	/* [한국어] 세 변수를 같은 시작점으로 맞춘다. */
		iopt_area_index_to_iova(area, start_index);
	while (cur < batch->end) {	/* [한국어] 항목마다 매핑 한 번. */
		next_iova = min(last_iova + 1,	/* [한국어] 이 항목이 덮는 끝. 영역의 끝을 넘지 않게 자른다. */
				next_iova + batch->npfns[cur] * PAGE_SIZE -	/* [한국어] 항목의 PFN 개수만큼 나아가되 */
					page_offset);	/* [한국어] 첫 바퀴는 오프셋만큼 짧다. */
		if (disable_large_pages)	/* [한국어] 큰 페이지가 금지됐으면 */
			rc = batch_iommu_map_small(	/* [한국어] 한 페이지씩 나눠 건다. */
				domain, iova,
				PFN_PHYS(batch->pfns[cur]) + page_offset,	/* [한국어] PFN 을 물리 주소로 바꾸고 오프셋을 더한다. */
				next_iova - iova, iommu_prot);
		else
			rc = iommu_map(domain, iova,	/* [한국어] 평소에는 항목 전체를 한 번에 건다 — 드라이버가 알아서 큰 IOPTE 로 합친다. */
				       PFN_PHYS(batch->pfns[cur]) + page_offset,
				       next_iova - iova, iommu_prot,
				       GFP_KERNEL_ACCOUNT);
		if (rc)	/* [한국어] 한 페이지라도 매핑에 실패하면. */
			goto err_unmap;	/* [한국어] 실패하면 이 묶음이 건 만큼을 되돌린다. */
		iova = next_iova;	/* [한국어] 다음 항목의 시작. */
		page_offset = 0;	/* [한국어] 이후는 경계에 맞는다. */
		cur++;	/* [한국어] 다음 항목. */
	}
	return 0;	/* [한국어] 전부 매핑했다. */
err_unmap:	/* [한국어] 실패 경로. */
	if (start_iova != iova)	/* [한국어] 걸린 것이 있으면 */
		iommu_unmap_nofail(domain, start_iova, iova - start_iova);	/* [한국어] 푼다. 앞선 묶음들은 호출자가 되돌린다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * batch_from_xarray - xarray 층에서 PFN 을 읽어 묶음에 담는다
 *
 * @batch: 채울 묶음.
 * @xa: 읽어 올 xarray.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 *
 * xarray 는 포인터 대신 값을 담을 수 있어, PFN 을 그대로 넣어 둔다.
 * 도메인에서 되읽는 것보다 훨씬 빠르다.
 *
 * RCU 로 읽는 이유: 쓰는 쪽과 겹쳐도 항목 하나하나는 온전히 보이므로,
 * 락을 잡지 않고도 안전하게 훑을 수 있다.
 */
static void batch_from_xarray(struct pfn_batch *batch, struct xarray *xa,
			      unsigned long start_index,
			      unsigned long last_index)
{
	XA_STATE(xas, xa, start_index);	/* [한국어] xarray 를 빠르게 훑는 상태 변수. 매크로가 지역 변수를 만들어 준다. */
	void *entry;	/* [한국어] 읽어 온 항목. */

	rcu_read_lock();	/* [한국어] 지우는 쪽과 겹쳐도 항목 하나하나는 온전히 보인다 — 락을 잡지 않아도 되는 이유다. */
	while (true) {	/* [한국어] 자리가 차거나 끝에 닿을 때까지. */
		entry = xas_next(&xas);	/* [한국어] 다음 항목. */
		if (xas_retry(&xas, entry))	/* [한국어] 트리가 그 사이 재구성됐으면 */
			continue;	/* [한국어] 다시 읽는다. */
		WARN_ON(!xa_is_value(entry));	/* [한국어] 포인터가 아니라 값이 들어 있어야 한다 — PFN 을 그대로 넣어 두었다. */
		if (!batch_add_pfn(batch, xa_to_value(entry)) ||	/* [한국어] 묶음이 찼거나 */
		    start_index == last_index)	/* [한국어] 요청한 끝에 닿았으면 */
			break;	/* [한국어] 멈춘다. */
		start_index++;	/* [한국어] 다음 번호. */
	}
	rcu_read_unlock();	/* [한국어] 읽기를 마쳤다. */
}

/*
 * [한국어]
 * batch_from_xarray_clear - xarray 에서 읽으면서 지운다
 *
 * @batch: 채울 묶음.
 * @xa: 대상 xarray.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 *
 * 읽어 간 것을 곧바로 지워 xarray 층을 비운다. 마지막 사용자가 떠날 때
 * 쓰는 경로다.
 *
 * 지우므로 RCU 가 아니라 락을 잡는다.
 *
 * 자리가 없어 못 담은 것은 지우지 않고 남기는 데 주의 — 다음 바퀴에서
 * 다시 읽어야 한다.
 */
static void batch_from_xarray_clear(struct pfn_batch *batch, struct xarray *xa,
				    unsigned long start_index,
				    unsigned long last_index)
{
	XA_STATE(xas, xa, start_index);	/* [한국어] 훑기 상태 변수. */
	void *entry;	/* [한국어] 읽어 온 항목. */

	xas_lock(&xas);	/* [한국어] 지우기까지 하므로 락이 필요하다. */
	while (true) {	/* [한국어] 자리가 차거나 끝에 닿을 때까지. */
		entry = xas_next(&xas);	/* [한국어] 다음 항목. */
		if (xas_retry(&xas, entry))	/* [한국어] 재구성됐으면 */
			continue;	/* [한국어] 다시 읽는다. */
		WARN_ON(!xa_is_value(entry));	/* [한국어] 값이어야 한다. */
		if (!batch_add_pfn(batch, xa_to_value(entry)))	/* [한국어] 묶음이 찼으면 */
			break;	/* [한국어] 지우지 않고 멈춘다 — 다음 바퀴에서 다시 읽어야 한다. */
		xas_store(&xas, NULL);	/* [한국어] 담은 것만 지운다. */
		if (start_index == last_index)	/* [한국어] 끝에 닿았으면 */
			break;	/* [한국어] 멈춘다. */
		start_index++;	/* [한국어] 다음 번호. */
	}
	xas_unlock(&xas);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * clear_xarray - 구간의 xarray 항목을 모두 지운다
 *
 * @xa: 대상 xarray.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 *
 * 값을 쓰지 않고 지우기만 한다. 채우다 실패했을 때 되돌리는 데 쓴다.
 */
static void clear_xarray(struct xarray *xa, unsigned long start_index,
			 unsigned long last_index)
{
	XA_STATE(xas, xa, start_index);	/* [한국어] 훑기 상태 변수. */
	void *entry;	/* [한국어] 읽어 온 항목(쓰지는 않는다). */

	xas_lock(&xas);	/* [한국어] 지우므로 락. */
	xas_for_each(&xas, entry, last_index)	/* [한국어] 구간의 모든 항목을 */
		xas_store(&xas, NULL);	/* [한국어] 지운다. */
	xas_unlock(&xas);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * pages_to_xarray - 페이지 배열을 xarray 층에 저장한다
 *
 * @xa: 대상 xarray.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 * @pages: 저장할 페이지 배열.
 * @return: 0 성공, 음수면 실패.
 *
 * 페이지 포인터가 아니라 PFN 값을 넣는다. xarray 의 값 저장 기능을 써서
 * 포인터 한 칸에 숫자를 담는 것이라 별도 할당이 없다.
 *
 * do-while 과 xas_nomem 조합이 xarray 의 관례다 — 노드가 모자라면 락을
 * 놓고 할당한 뒤 다시 시도한다.
 *
 * 실패하면 절반쯤 넣은 것을 모두 지워 아무것도 하지 않은 상태로
 * 되돌린다.
 *
 * 한가운데에서 일부러 실패를 내는 것은 결함 주입 시험이다. 원 주석대로
 * xarray 자체는 그 시험에 끼지 않으므로 여기서 흉내 낸다.
 */
static int pages_to_xarray(struct xarray *xa, unsigned long start_index,
			   unsigned long last_index, struct page **pages)
{
	struct page **end_pages = pages + (last_index - start_index) + 1;	/* [한국어] 배열의 끝. */
	struct page **half_pages = pages + (end_pages - pages) / 2;	/* [한국어] 결함 주입 시험이 실패를 낼 지점 — 절반쯤 채운 상태에서 되돌리기가 제대로 도는지 보려는 것이다. */
	XA_STATE(xas, xa, start_index);	/* [한국어] 훑기 상태 변수. */

	do {	/* [한국어] 노드가 모자라면 다시 시도하는 xarray 관례. */
		void *old;	/* [한국어] 덮어쓴 옛 값(있으면 버그다). */

		xas_lock(&xas);	/* [한국어] 쓰므로 락. */
		while (pages != end_pages) {	/* [한국어] 배열 끝까지. */
			/* xarray does not participate in fault injection */
			if (pages == half_pages && iommufd_should_fail()) {	/* [한국어] 원 주석대로 xarray 자체는 결함 주입에 끼지 않아 여기서 흉내 낸다. */
				xas_set_err(&xas, -EINVAL);	/* [한국어] 오류를 심고 */
				xas_unlock(&xas);	/* [한국어] 락을 놓고 */
				/* aka xas_destroy() */
				xas_nomem(&xas, GFP_KERNEL);	/* [한국어] 상태 변수가 들고 있던 예비 노드를 해제한다. */
				goto err_clear;	/* [한국어] 되돌리기로 간다. */
			}

			old = xas_store(&xas, xa_mk_value(page_to_pfn(*pages)));	/* [한국어] PFN 을 값으로 만들어 넣는다. 포인터가 아니라 값이라 별도 할당이 없다. */
			if (xas_error(&xas))	/* [한국어] 노드가 모자라 실패했으면 */
				break;	/* [한국어] 안쪽 고리를 나가 재시도한다. */
			WARN_ON(old);	/* [한국어] 이미 무언가 있었다면 두 곳에서 같은 자리를 채운 것이다. */
			pages++;	/* [한국어] 다음 페이지. */
			xas_next(&xas);	/* [한국어] 다음 자리. */
		}
		xas_unlock(&xas);	/* [한국어] 락을 놓고 */
	} while (xas_nomem(&xas, GFP_KERNEL));	/* [한국어] 노드를 미리 할당해 다시 시도한다. 할당이 실패하면 거짓을 돌려줘 고리를 끝낸다. */

err_clear:	/* [한국어] 결함 주입 실패가 뛰어드는 지점. */
	if (xas_error(&xas)) {	/* [한국어] 실패했으면 */
		if (xas.xa_index != start_index)	/* [한국어] 하나라도 넣었으면 */
			clear_xarray(xa, start_index, xas.xa_index - 1);	/* [한국어] 모두 지워 손대기 전 상태로 만든다. */
		return xas_error(&xas);	/* [한국어] 오류를 올린다. */
	}
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * batch_from_pages - 페이지 배열에서 PFN 을 읽어 묶음에 담는다
 *
 * @batch: 채울 묶음.
 * @pages: 읽어 올 페이지 배열.
 * @npages: 그 개수.
 *
 * 자리가 차면 멈춘다. 호출자가 담긴 개수를 보고 이어서 처리한다.
 */
static void batch_from_pages(struct pfn_batch *batch, struct page **pages,
			     size_t npages)
{
	struct page **end = pages + npages;	/* [한국어] 배열의 끝. */

	for (; pages != end; pages++)	/* [한국어] 페이지마다 */
		if (!batch_add_pfn(batch, page_to_pfn(*pages)))	/* [한국어] 묶음에 넣는다. 찼으면 */
			break;	/* [한국어] 멈춘다. 호출자가 담긴 개수를 보고 이어 간다. */
}

/*
 * [한국어]
 * batch_from_folios - folio 배열에서 PFN 을 읽어 묶음에 담는다
 *
 * @batch: 채울 묶음.
 * @folios_p: folio 배열 포인터. 진행한 만큼 옮겨 되돌려 준다.
 * @offset_p: 첫 folio 안에서의 페이지 오프셋. 역시 갱신된다.
 * @npages: 담고 싶은 페이지 수.
 * @return: 0 성공, 음수면 실패.
 *
 * folio 는 이어진 페이지 무리라, 한 folio 가 묶음의 한 항목이 된다 —
 * 페이지 단위로 다루는 것보다 훨씬 효율적이다.
 *
 * folio_add_pins 가 이 함수의 까다로운 대목이다. 파일 매핑에서 folio 를
 * 얻으면 참조가 하나만 잡히는데, 페이지마다 하나씩 세는 규칙을 맞추려면
 * 나머지를 더 잡아야 한다. 실패하면 방금 넣은 것을 빼서 되돌린다.
 */
static int batch_from_folios(struct pfn_batch *batch, struct folio ***folios_p,
			     unsigned long *offset_p, unsigned long npages)
{
	int rc = 0;	/* [한국어] 결과 코드. */
	struct folio **folios = *folios_p;	/* [한국어] 지금 볼 folio. */
	unsigned long offset = *offset_p;	/* [한국어] 그 folio 안에서의 시작 페이지. */

	while (npages) {	/* [한국어] 필요한 만큼 담을 때까지. */
		struct folio *folio = *folios;	/* [한국어] 이번 folio. */
		unsigned long nr = folio_nr_pages(folio) - offset;	/* [한국어] 이 folio 에서 쓸 수 있는 페이지 수. */
		unsigned long pfn = page_to_pfn(folio_page(folio, offset));	/* [한국어] 그 시작 PFN. */

		nr = min(nr, npages);	/* [한국어] 필요한 것보다 많이 담지 않는다. */
		npages -= nr;	/* [한국어] 남은 필요량을 줄인다. */

		if (!batch_add_pfn_num(batch, pfn, nr, BATCH_CPU_MEMORY))	/* [한국어] folio 하나가 항목 하나가 된다 — 페이지 단위보다 훨씬 효율적이다. */
			break;	/* [한국어] 자리가 없으면 멈춘다. */
		if (nr > 1) {	/* [한국어] 두 페이지 이상을 쓰면 */
			rc = folio_add_pins(folio, nr - 1);	/* [한국어] 참조를 더 잡는다. folio 를 얻을 때 하나만 잡혔는데, 이 코드는 페이지마다 하나씩 세기 때문이다. */
			if (rc) {	/* [한국어] 더 잡지 못했으면 */
				batch_remove_pfn_num(batch, nr);	/* [한국어] 방금 넣은 것을 빼서 되돌리고 */
				goto out;	/* [한국어] 나간다. */
			}
		}

		folios++;	/* [한국어] 다음 folio. */
		offset = 0;	/* [한국어] 두 번째부터는 처음부터 쓴다. */
	}

out:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	*folios_p = folios;	/* [한국어] 진행한 위치를 호출자에게 돌려준다. */
	*offset_p = offset;	/* [한국어] 오프셋도. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * batch_unpin - 묶음이 가리키는 페이지들의 고정을 놓는다
 *
 * @batch: 대상 묶음.
 * @pages: 회계를 갱신할 pages 객체.
 * @first_page_off: 묶음의 시작에서 건너뛸 페이지 수.
 * @npages: 놓을 페이지 수.
 *
 * 묶음 전체가 아니라 그 일부만 놓을 수 있어야 한다 — 여러 영역이 한
 * 묶음을 나눠 쓰기 때문이다. 앞의 고리가 시작 항목을 찾고, 뒤의 고리가
 * 항목을 넘나들며 놓는다.
 *
 * 쓰기 가능한 매핑이면 더티로 표시한다. 사용자가 그 페이지를 파일에
 * 되쓰거나 스왑할 때 내용이 보존되어야 한다.
 */
static void batch_unpin(struct pfn_batch *batch, struct iopt_pages *pages,
			unsigned int first_page_off, size_t npages)
{
	unsigned int cur = 0;	/* [한국어] 지금 보는 항목 번호. */

	while (first_page_off) {	/* [한국어] 건너뛸 만큼 앞으로 간다. */
		if (batch->npfns[cur] > first_page_off)	/* [한국어] 이 항목 안에서 시작하면 */
			break;	/* [한국어] 여기서 멈춘다. */
		first_page_off -= batch->npfns[cur];	/* [한국어] 이 항목을 통째로 건너뛴다. */
		cur++;	/* [한국어] 다음 항목. */
	}

	while (npages) {	/* [한국어] 놓을 만큼 놓는다. */
		size_t to_unpin = min_t(size_t, npages,	/* [한국어] 이번 항목에서 놓을 수 있는 만큼. */
					batch->npfns[cur] - first_page_off);	/* [한국어] 항목의 남은 부분. */

		unpin_user_page_range_dirty_lock(	/* [한국어] 이어진 페이지 범위를 한 번에 놓는다. */
			pfn_to_page(batch->pfns[cur] + first_page_off),	/* [한국어] 시작 페이지. */
			to_unpin, pages->writable);	/* [한국어] 쓰기 가능한 매핑이면 더티로 표시한다 — 그러지 않으면 장치가 쓴 내용이 사라질 수 있다. */
		iopt_pages_sub_npinned(pages, to_unpin);	/* [한국어] 회계를 함께 줄인다. */
		cur++;	/* [한국어] 다음 항목으로. */
		first_page_off = 0;	/* [한국어] 두 번째부터는 항목 처음부터. */
		npages -= to_unpin;	/* [한국어] 남은 개수를 줄인다. */
	}
}

/*
 * [한국어]
 * copy_data_page - 페이지 하나와 커널 버퍼 사이를 복사한다
 *
 * @page: 대상 페이지.
 * @data: 커널 버퍼.
 * @offset: 페이지 안의 오프셋.
 * @length: 복사할 길이.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 *
 * kmap_local 로 잠시 커널 주소를 얻는다. highmem 페이지는 늘 커널 주소
 * 공간에 있지 않아 이 과정이 필요하다.
 *
 * 쓰기면 더티로 표시한다 — 그러지 않으면 내용이 사라질 수 있다.
 */
static void copy_data_page(struct page *page, void *data, unsigned long offset,
			   size_t length, unsigned int flags)
{
	void *mem;	/* [한국어] 임시로 얻은 커널 주소. */

	mem = kmap_local_page(page);	/* [한국어] highmem 페이지는 늘 커널 주소 공간에 있지 않아 잠시 매핑해야 한다. local 판은 이 CPU 에서만 유효해 값이 싸다. */
	if (flags & IOMMUFD_ACCESS_RW_WRITE) {	/* [한국어] 쓰기면 */
		memcpy(mem + offset, data, length);	/* [한국어] 커널 버퍼에서 페이지로. */
		set_page_dirty_lock(page);	/* [한국어] 바꿨음을 알린다 — 그러지 않으면 스왑이나 파일 되쓰기에서 내용이 사라진다. */
	} else {
		memcpy(data, mem + offset, length);	/* [한국어] 읽기면 반대 방향. */
	}
	kunmap_local(mem);	/* [한국어] 임시 매핑을 푼다. */
}

/*
 * [한국어]
 * batch_rw - 묶음이 가리키는 메모리와 커널 버퍼 사이를 복사한다
 *
 * @batch: 대상 묶음.
 * @data: 커널 버퍼.
 * @offset: 첫 페이지 안의 오프셋.
 * @length: 복사할 길이.
 * @flags: 방향 등.
 * @return: 실제로 복사한 바이트 수.
 *
 * 항목 안의 페이지들을 하나씩 지나며 복사한다. 한 항목이 여러 페이지를
 * 뜻하므로 npage 로 그 안의 위치를 따로 센다.
 *
 * 길이가 다하면 멈추고 복사한 만큼을 알린다 — 묶음이 요청보다 짧을 수
 * 있어 호출자가 이어서 처리해야 한다.
 */
static unsigned long batch_rw(struct pfn_batch *batch, void *data,
			      unsigned long offset, unsigned long length,
			      unsigned int flags)
{
	unsigned long copied = 0;	/* [한국어] 지금까지 복사한 바이트. */
	unsigned int npage = 0;	/* [한국어] 지금 항목 안에서 몇 번째 페이지인지. */
	unsigned int cur = 0;	/* [한국어] 지금 보는 항목 번호. */

	while (cur < batch->end) {	/* [한국어] 항목을 차례로. */
		unsigned long bytes = min(length, PAGE_SIZE - offset);	/* [한국어] 이번 페이지에서 다룰 바이트 수. 첫 페이지만 오프셋 때문에 짧을 수 있다. */

		copy_data_page(pfn_to_page(batch->pfns[cur] + npage), data,	/* [한국어] 한 페이지를 복사한다. */
			       offset, bytes, flags);
		offset = 0;	/* [한국어] 두 번째부터는 페이지 처음부터. */
		length -= bytes;	/* [한국어] 남은 길이를 줄인다. */
		data += bytes;	/* [한국어] 커널 버퍼 자리를 옮긴다. */
		copied += bytes;	/* [한국어] 복사량을 센다. */
		npage++;	/* [한국어] 항목 안의 다음 페이지. */
		if (npage == batch->npfns[cur]) {	/* [한국어] 이 항목을 다 썼으면 */
			npage = 0;	/* [한국어] 처음으로 되돌리고 */
			cur++;	/* [한국어] 다음 항목으로. */
		}
		if (!length)	/* [한국어] 다 복사했으면 */
			break;	/* [한국어] 멈춘다. */
	}
	return copied;	/* [한국어] 복사한 바이트를 알린다. 묶음이 요청보다 짧을 수 있어 호출자가 이어 간다. */
}

/* pfn_reader_user is just the pin_user_pages() path */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * PFN 을 원천(사용자 메모리 또는 파일)에서 끌어오는 상태.
 *
 * 두 갈래를 함께 담는다. 보통의 사용자 포인터는 pin_user_pages 로 페이지
 * 배열을 얻고, memfd 같은 파일은 memfd_pin_folios 로 folio 배열을 얻는다.
 * file 이 NULL 인지로 어느 쪽인지 가른다.
 */
struct pfn_reader_user {
	/* [한국어] 고정해 온 페이지 포인터들을 담는 배열.
	 *  설정자: pfn_reader_user_pin 이 pin_user_pages 로 채운다.
	 *  읽는 자: batch_from_pages 가 묶음으로 옮긴다.
	 *  iopt_pages_fill 은 이것을 호출자의 출력 배열로 바꿔 치기해, 옮기는
	 *  수고를 없앤다.
	 *  값 범위: NULL 이면 아직 잡지 않았다는 뜻. */
	struct page **upages;
	/* [한국어] 그 배열의 크기(바이트).
	 *  설정자: temp_kmalloc 이 실제로 얻은 크기를 되돌려 쓴다.
	 *  읽는 자: 한 번에 몇 페이지를 고정할지 정할 때.
	 *  원하는 만큼 얻지 못할 수 있어, 요청 크기가 아니라 실제 크기다. */
	size_t upages_len;
	/* [한국어] 배열의 0 번째가 뜻하는 페이지 번호.
	 *  설정자·읽는 자: pfn_reader_user_pin 과 fill_span.
	 *  배열 첨자를 페이지 번호로 옮기는 기준점이다. */
	unsigned long upages_start;
	/* [한국어] 고정해 둔 마지막 페이지 번호의 다음.
	 *  설정자: pfn_reader_user_pin. 읽는 자: fill_span 이 더 고정해야 하는지
	 *  판정할 때, release_pins 가 옮기지 못한 몫을 셀 때.
	 *  start 와 end 사이가 곧 지금 고정을 들고 있는 범위다. */
	unsigned long upages_end;
	/* [한국어] pin_user_pages 에 넘길 플래그.
	 *  설정자: pfn_reader_user_init. 읽는 자: 고정 함수들.
	 *  FOLL_LONGTERM 이 늘 들어간다 — DMA 매핑은 오래 살아 있어, 옮길 수 있는
	 *  페이지에 걸리면 나중에 이동이 막혀 문제가 된다.
	 *  쓰기 가능한 매핑이면 FOLL_WRITE 가 더해진다. */
	unsigned int gup_flags;
	/*
	 * 1 means mmget() and mmap_read_lock(), 0 means only mmget(), -1 is
	 * neither
	 */
	/* [한국어] (위 영어 주석 참고) mm 참조와 읽기 락을 얼마나 쥐고 있는가.
	 *  설정자: pfn_reader_user_pin 이 잡고, destroy 와 update_mm_locked_vm 이 푼다.
	 *  값 범위: -1 아무것도 아님, 0 mm 참조만, 1 참조와 읽기 락 둘 다.
	 *  세 값이 필요한 이유: 원격 주소 공간을 볼 때만 락이 필요하고, 회계를
	 *  갱신할 때는 그 락을 잠시 놓아야 하기 때문이다.
	 *  pin_user_pages_remote 가 이 값을 직접 고치기도 한다. */
	int locked;

	/* The following are only valid if file != NULL. */
	/* [한국어] 원천이 파일일 때 그 파일. 아니면 NULL.
	 *  설정자: pfn_reader_user_init 이 pages 의 종류를 보고 정한다.
	 *  읽는 자: 거의 모든 함수가 이 값으로 두 갈래를 가른다.
	 *  NULL 인지가 곧 "페이지 배열이냐 folio 배열이냐"의 판별이다. */
	struct file *file;
	/* [한국어] 파일에서 고정해 온 folio 포인터 배열.
	 *  설정자: pin_memfd_pages. 읽는 자: batch_from_folios.
	 *  folio 를 쓰는 이유: 파일 페이지는 큰 덩어리로 이어져 있는 일이 많아,
	 *  페이지 하나씩 다루면 낭비가 크다. */
	struct folio **ufolios;
	/* [한국어] 그 배열의 크기(바이트).
	 *  설정자: temp_kmalloc 이 실제 크기를 되돌려 쓴다.
	 *  읽는 자: 한 번에 몇 folio 를 받을 수 있는지 셀 때, 그리고
	 *  release_pins 가 배열의 끝을 알아낼 때. */
	size_t ufolios_len;
	/* [한국어] 지금 보고 있는 folio 안에서의 페이지 오프셋.
	 *  설정자: pin_memfd_pages 가 처음 값을, batch_from_folios 가 진행하며 갱신.
	 *  folio 하나가 여러 페이지라, 그 중간부터 시작할 수 있다. */
	unsigned long ufolios_offset;
	/* [한국어] 아직 묶음으로 옮기지 않은 첫 folio 를 가리킨다.
	 *  설정자: pin_memfd_pages 가 배열 시작으로 놓고, batch_from_folios 가 전진.
	 *  읽는 자: release_pins 가 옮기지 못한 folio 들을 놓을 때.
	 *  ufolios 와 이 포인터 사이가 이미 처리한 몫이다. */
	struct folio **ufolios_next;
};

/*
 * [한국어]
 * pfn_reader_user_init - 원천 읽기 상태를 초기화한다
 *
 * @user: 초기화할 상태.
 * @pages: 원천을 아는 pages 객체.
 *
 * 버퍼는 아직 잡지 않는다 — 실제로 고정할 때가 되어야 얼마나 필요한지
 * 알 수 있다.
 *
 * FOLL_LONGTERM 이 요점이다. DMA 매핑은 오래 살아 있으므로, 이동 가능한
 * 페이지(CMA 등)에 걸리면 안 된다고 mm 에 알린다.
 */
static void pfn_reader_user_init(struct pfn_reader_user *user,
				 struct iopt_pages *pages)
{
	user->upages = NULL;	/* [한국어] 아직 잡지 않았다는 표시. */
	user->upages_len = 0;	/* [한국어] 크기도 0. */
	user->upages_start = 0;	/* [한국어] 고정 범위 없음. */
	user->upages_end = 0;	/* [한국어] 같음. */
	user->locked = -1;	/* [한국어] mm 참조도 락도 없다. */
	user->gup_flags = FOLL_LONGTERM;	/* [한국어] DMA 매핑은 오래 살아 있으므로, 이동 가능한 페이지를 주면 안 된다고 mm 에 알린다. */
	if (pages->writable)	/* [한국어] 쓰기 가능한 매핑이면 */
		user->gup_flags |= FOLL_WRITE;	/* [한국어] 쓰기 권한으로 고정한다 — 그래야 copy-on-write 가 미리 풀린다. */

	user->file = (pages->type == IOPT_ADDRESS_FILE) ? pages->file : NULL;	/* [한국어] 파일 원천인지 여기서 가른다. 이후 모든 갈림길이 이 값을 본다. */
	user->ufolios = NULL;	/* [한국어] folio 배열도 아직 없다. */
	user->ufolios_len = 0;	/* [한국어] 크기 0. */
	user->ufolios_next = NULL;	/* [한국어] 진행 위치 없음. */
	user->ufolios_offset = 0;	/* [한국어] 오프셋 0. */
}

/*
 * [한국어]
 * pfn_reader_user_destroy - 원천 읽기 상태를 정리한다
 *
 * @user: 정리할 상태.
 * @pages: 그 pages 객체.
 *
 * 잡았던 mm 참조와 읽기 락, 버퍼를 모두 놓는다.
 *
 * locked 가 세 값을 갖는 이유는 구조체 주석이 밝힌다 — 락과 참조를
 * 둘 다 잡았는지, 참조만 잡았는지, 아무것도 아닌지가 다르다.
 */
static void pfn_reader_user_destroy(struct pfn_reader_user *user,
				    struct iopt_pages *pages)
{
	if (user->locked != -1) {	/* [한국어] 무언가 쥐고 있으면 */
		if (user->locked)	/* [한국어] 읽기 락까지 잡았으면 */
			mmap_read_unlock(pages->source_mm);	/* [한국어] 푼다. */
		if (!user->file && pages->source_mm != current->mm)	/* [한국어] 원격 주소 공간을 봤다면 참조도 들었다. */
			mmput(pages->source_mm);	/* [한국어] 그 참조를 놓는다. */
		user->locked = -1;	/* [한국어] 아무것도 쥐지 않은 상태로. */
	}

	kfree(user->upages);	/* [한국어] 페이지 배열 해제. */
	user->upages = NULL;	/* [한국어] 두 번 해제하지 않게 지운다. */
	kfree(user->ufolios);	/* [한국어] folio 배열 해제. */
	user->ufolios = NULL;	/* [한국어] 같은 이유. */
}

/*
 * [한국어]
 * pin_memfd_pages - 파일(memfd)에서 페이지를 고정한다
 *
 * @user: 원천 읽기 상태.
 * @start: 파일 안의 시작 오프셋.
 * @npages: 고정하고 싶은 페이지 수.
 * @return: 실제로 고정한 페이지 수, 음수면 실패.
 *
 * 파일 지원이 필요한 이유: 게스트 메모리를 memfd 로 두면 여러 프로세스가
 * 그것을 공유할 수 있고, 프로세스가 죽어도 메모리가 남는다.
 *
 * folio 로 받아 오므로 큰 페이지가 한 항목이 된다. 하지만 페이지마다
 * 참조를 하나씩 세는 규칙을 맞추려면 folio_add_pins 로 나머지를 더 잡아야
 * 한다.
 *
 * 원 주석의 todo 대로, 지금은 얼마나 고정했는지 알아내려고 folio 를 다시
 * 훑어야 한다.
 */
static long pin_memfd_pages(struct pfn_reader_user *user, unsigned long start,
			    unsigned long npages)
{
	unsigned long i;	/* [한국어] folio 순회용. */
	unsigned long offset;	/* [한국어] 첫 folio 안에서의 페이지 오프셋. */
	unsigned long npages_out = 0;	/* [한국어] 실제로 고정한 페이지 수. */
	struct page **upages = user->upages;	/* [한국어] 페이지 배열이 필요하면 여기 채운다. NULL 일 수 있다. */
	unsigned long end = start + (npages << PAGE_SHIFT) - 1;	/* [한국어] 파일 안의 마지막 바이트. */
	long nfolios = user->ufolios_len / sizeof(*user->ufolios);	/* [한국어] 배열이 담을 수 있는 folio 개수. */

	/*
	 * todo: memfd_pin_folios should return the last pinned offset so
	 * we can compute npages pinned, and avoid looping over folios here
	 * if upages == NULL.
	 */
	nfolios = memfd_pin_folios(user->file, start, end, user->ufolios,	/* [한국어] 파일에서 folio 를 고정해 온다. 큰 페이지가 한 folio 로 온다. */
				   nfolios, &offset);
	if (nfolios <= 0)	/* [한국어] 하나도 못 얻었거나 오류다. */
		return nfolios;	/* [한국어] 그대로 올린다. */

	offset >>= PAGE_SHIFT;	/* [한국어] 바이트 오프셋을 페이지 오프셋으로 바꾼다. */
	user->ufolios_next = user->ufolios;	/* [한국어] 아직 아무것도 옮기지 않았다. */
	user->ufolios_offset = offset;	/* [한국어] 시작 오프셋을 기억한다. */

	for (i = 0; i < nfolios; i++) {	/* [한국어] 원 주석의 todo 대로, 몇 페이지를 얻었는지 알려면 다시 훑어야 한다. */
		struct folio *folio = user->ufolios[i];	/* [한국어] 이번 folio. */
		unsigned long nr = folio_nr_pages(folio);	/* [한국어] 그 페이지 수. */
		unsigned long npin = min(nr - offset, npages);	/* [한국어] 이 folio 에서 쓸 페이지 수. */

		npages -= npin;	/* [한국어] 남은 필요량. */
		npages_out += npin;	/* [한국어] 얻은 총량. */

		if (upages) {	/* [한국어] 페이지 배열도 채워야 하면 */
			if (npin == 1) {	/* [한국어] 한 페이지만 쓰면 */
				*upages++ = folio_page(folio, offset);	/* [한국어] 그대로 담는다. 참조는 이미 하나 잡혀 있다. */
			} else {
				int rc = folio_add_pins(folio, npin - 1);	/* [한국어] 여러 페이지면 참조를 더 잡아 페이지마다 하나씩이 되게 한다. */

				if (rc)	/* [한국어] 더 잡지 못했으면 */
					return rc;	/* [한국어] 실패를 올린다. */

				while (npin--)	/* [한국어] 그 페이지들을 */
					*upages++ = folio_page(folio, offset++);	/* [한국어] 배열에 담는다. */
			}
		}

		offset = 0;	/* [한국어] 두 번째 folio 부터는 처음부터. */
	}

	return npages_out;	/* [한국어] 실제로 고정한 페이지 수. */
}

/*
 * [한국어]
 * pfn_reader_user_pin - 원천에서 한 뭉치를 고정해 온다
 *
 * @user: 원천 읽기 상태.
 * @pages: 대상 pages 객체.
 * @start_index: 고정할 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @return: 0 성공, 음수면 실패.
 *
 * 세 갈래로 나뉜다. 파일이면 memfd 경로, 같은 프로세스의 메모리면 빠른
 * 경로(get_user_pages_fast), 다른 프로세스면 원격 경로다.
 *
 * 원 주석대로 대부분은 매핑을 만드는 프로세스가 곧 그 메모리의 주인이라,
 * 빠른 경로가 잘 맞는다. 그 경우 mm 참조도 락도 필요 없다.
 *
 * 원격 경로만 mmap 읽기 락을 잡는다. 남의 주소 공간을 들여다보는 것이라
 * 그 사이 매핑이 바뀌면 안 된다.
 *
 * 버퍼 크기만큼으로 개수를 줄이는 것에 주의 — 한 번에 다 못 하면 호출자가
 * 여러 번 부른다.
 */
static int pfn_reader_user_pin(struct pfn_reader_user *user,
			       struct iopt_pages *pages,
			       unsigned long start_index,
			       unsigned long last_index)
{
	bool remote_mm = pages->source_mm != current->mm;	/* [한국어] 남의 주소 공간을 봐야 하는가. */
	unsigned long npages = last_index - start_index + 1;	/* [한국어] 고정할 페이지 수. */
	unsigned long start;	/* [한국어] 파일 안의 시작 오프셋. */
	unsigned long unum;	/* [한국어] 배열이 담을 수 있는 개수. */
	uintptr_t uptr;	/* [한국어] 사용자 주소. */
	long rc;	/* [한국어] 고정한 개수 또는 오류. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드에서만 하는 인자 검사. */
	    WARN_ON(last_index < start_index))	/* [한국어] 뒤집힌 범위는 호출자의 버그다. */
		return -EINVAL;

	if (!user->file && !user->upages) {	/* [한국어] 사용자 포인터 원천인데 배열이 없으면 */
		/* All undone in pfn_reader_destroy() */
		user->upages_len = npages * sizeof(*user->upages);	/* [한국어] 필요한 크기를 계산하고 */
		user->upages = temp_kmalloc(&user->upages_len, NULL, 0);	/* [한국어] 잡는다. 원하는 만큼 못 얻어도 그만큼으로 일한다. */
		if (!user->upages)	/* [한국어] 메모리가 없다. */
			return -ENOMEM;	/* [한국어] 실패. 원 주석대로 정리는 destroy 가 한다. */
	}

	if (user->file && !user->ufolios) {	/* [한국어] 파일 원천인데 folio 배열이 없으면 */
		user->ufolios_len = npages * sizeof(*user->ufolios);	/* [한국어] 필요한 크기를 계산하고 */
		user->ufolios = temp_kmalloc(&user->ufolios_len, NULL, 0);	/* [한국어] 잡는다. */
		if (!user->ufolios)	/* [한국어] 메모리가 없다. */
			return -ENOMEM;	/* [한국어] 실패. */
	}

	if (user->locked == -1) {	/* [한국어] 아직 아무것도 쥐지 않았으면 */
		/*
		 * The majority of usages will run the map task within the mm
		 * providing the pages, so we can optimize into
		 * get_user_pages_fast()
		 */
		if (!user->file && remote_mm) {	/* [한국어] 원격 주소 공간을 봐야 하면 */
			if (!mmget_not_zero(pages->source_mm))	/* [한국어] 그 주소 공간을 붙잡는다. 이미 사라졌으면 */
				return -EFAULT;	/* [한국어] 읽을 수 없다. */
		}
		user->locked = 0;	/* [한국어] 참조만 든 상태. */
	}

	unum = user->file ? user->ufolios_len / sizeof(*user->ufolios) :	/* [한국어] 배열이 담을 수 있는 개수를 센다. */
			    user->upages_len / sizeof(*user->upages);	/* [한국어] 사용자 포인터 원천이면 페이지 배열의 크기로 센다. */
	npages = min_t(unsigned long, npages, unum);	/* [한국어] 한 번에 그만큼만 한다. 모자라면 호출자가 다시 부른다. */

	if (iommufd_should_fail())	/* [한국어] 결함 주입 시험. */
		return -EFAULT;	/* [한국어] 실패를 흉내 낸다. */

	if (user->file) {	/* [한국어] 파일 원천이면 */
		start = pages->start + (start_index * PAGE_SIZE);	/* [한국어] 파일 안의 오프셋을 계산해 */
		rc = pin_memfd_pages(user, start, npages);	/* [한국어] folio 로 고정한다. */
	} else if (!remote_mm) {	/* [한국어] 같은 주소 공간이면 */
		uptr = (uintptr_t)(pages->uptr + start_index * PAGE_SIZE);	/* [한국어] 사용자 주소를 계산하고 */
		rc = pin_user_pages_fast(uptr, npages, user->gup_flags,	/* [한국어] 원 주석대로 빠른 경로를 쓴다. 락도 참조도 필요 없다. */
					 user->upages);
	} else {
		uptr = (uintptr_t)(pages->uptr + start_index * PAGE_SIZE);	/* [한국어] 원격이면 같은 주소 계산에 */
		if (!user->locked) {	/* [한국어] 아직 읽기 락이 없으면 */
			mmap_read_lock(pages->source_mm);	/* [한국어] 남의 주소 공간을 들여다보는 동안 매핑이 바뀌면 안 된다. */
			user->locked = 1;	/* [한국어] 락까지 든 상태. */
		}
		rc = pin_user_pages_remote(pages->source_mm, uptr, npages,	/* [한국어] 원격 경로. locked 를 넘겨 주면 이 함수가 필요에 따라 락을 놓았다 잡는다. */
					   user->gup_flags, user->upages,
					   &user->locked);
	}
	if (rc <= 0) {	/* [한국어] 하나도 고정하지 못했다. */
		if (WARN_ON(!rc))	/* [한국어] 0 은 있을 수 없는 결과다. */
			return -EFAULT;	/* [한국어] 오류로 바꾼다. */
		return rc;	/* [한국어] 오류를 올린다. */
	}
	iopt_pages_add_npinned(pages, rc);	/* [한국어] 실제로 고정한 만큼 회계를 늘린다. 한도 확인은 나중에 한다. */
	user->upages_start = start_index;	/* [한국어] 고정한 범위의 시작. */
	user->upages_end = start_index + rc;	/* [한국어] 그 끝. 요청보다 짧을 수 있다. */
	return 0;	/* [한국어] 성공. */
}

/* This is the "modern" and faster accounting method used by io_uring */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * incr_user_locked_vm - 사용자 단위 잠금 메모리 회계를 늘린다
 *
 * @pages: 대상 pages 객체.
 * @npages: 늘릴 페이지 수.
 * @return: 0 성공, 한도를 넘으면 -ENOMEM.
 *
 * 원 주석대로 io_uring 이 쓰는 새 방식이다. 한도를 넘는지 확인하는 것과
 * 더하는 것이 한 원자 연산 안에서 일어나, 여러 스레드가 동시에 한도를
 * 살짝씩 넘기는 일이 없다.
 *
 * 옛 방식(mm 단위)은 mmap 쓰기 락을 잡아야 해서 훨씬 무겁다.
 *
 * cmpxchg 고리를 도는 이유: 읽은 값이 그 사이 바뀌었으면 다시 계산해야
 * 한다.
 */
static int incr_user_locked_vm(struct iopt_pages *pages, unsigned long npages)
{
	unsigned long lock_limit;	/* [한국어] 이 프로세스가 잠글 수 있는 페이지 수. */
	unsigned long cur_pages;	/* [한국어] 지금 잠근 수. */
	unsigned long new_pages;	/* [한국어] 더한 뒤의 값. */

	lock_limit = task_rlimit(pages->source_task, RLIMIT_MEMLOCK) >>	/* [한국어] 바이트 한도를 페이지 수로 바꾼다. */
		     PAGE_SHIFT;

	cur_pages = atomic_long_read(&pages->source_user->locked_vm);	/* [한국어] 사용자 단위로 세는 값을 읽는다 — 같은 사용자의 여러 프로세스가 함께 센다. */
	do {	/* [한국어] cmpxchg 고리. */
		new_pages = cur_pages + npages;	/* [한국어] 더해 본다. */
		if (new_pages > lock_limit)	/* [한국어] 한도를 넘으면 */
			return -ENOMEM;	/* [한국어] 거절한다. */
	} while (!atomic_long_try_cmpxchg(&pages->source_user->locked_vm,	/* [한국어] 읽은 값이 그대로면 바꾼다. 바뀌었으면 다시 계산한다 — 확인과 더하기가 원자적이라야 여러 스레드가 한도를 살짝씩 넘기지 못한다. */
					  &cur_pages, new_pages));
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * decr_user_locked_vm - 사용자 단위 잠금 메모리 회계를 줄인다
 *
 * @pages: 대상 pages 객체.
 * @npages: 줄일 페이지 수.
 *
 * 놓는 쪽은 한도를 볼 필요가 없어 그냥 뺀다.
 *
 * 가진 것보다 많이 빼려 하면 어딘가에서 회계가 어긋난 것이라 경고만
 * 남기고 아무것도 하지 않는다 — 여기서 더 망가뜨리지 않으려는 것이다.
 */
static void decr_user_locked_vm(struct iopt_pages *pages, unsigned long npages)
{
	if (WARN_ON(atomic_long_read(&pages->source_user->locked_vm) < npages))	/* [한국어] 가진 것보다 많이 빼려 하면 회계가 어긋난 것이다. */
		return;	/* [한국어] 여기서 더 망가뜨리지 않는다. */
	atomic_long_sub(npages, &pages->source_user->locked_vm);	/* [한국어] 놓는 쪽은 한도를 볼 필요가 없어 그냥 뺀다. */
}

/* This is the accounting method used for compatibility with VFIO */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * update_mm_locked_vm - mm 단위 잠금 메모리 회계를 갱신한다
 *
 * @pages: 대상 pages 객체.
 * @npages: 늘리거나 줄일 페이지 수.
 * @inc: 참이면 늘린다.
 * @user: 원천 읽기 상태(없으면 NULL).
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 옛 VFIO 와 호환을 맞추는 방식이다. 사용자 프로그램이
 * /proc 의 잠금 메모리 값을 보고 판단하는 경우가 있어, 그 숫자가 옛
 * 방식과 같게 나와야 한다.
 *
 * mmap 쓰기 락이 필요해 무겁다. 그래서 이미 읽기 락을 쥐고 있으면 먼저
 * 놓는다 — 같은 락을 읽기에서 쓰기로 올릴 수 없기 때문이다.
 *
 * mm 참조를 언제 들어야 하는지가 까다롭다. 읽기 락을 쥐고 있었다면
 * 참조도 함께 들고 있었고, 아니라면 여기서 들어야 한다.
 */
static int update_mm_locked_vm(struct iopt_pages *pages, unsigned long npages,
			       bool inc, struct pfn_reader_user *user)
{
	bool do_put = false;	/* [한국어] 여기서 mm 참조를 들었는지 표시. */
	int rc;	/* [한국어] 결과 코드. */

	if (user && user->locked) {	/* [한국어] 이미 읽기 락을 쥐고 있으면 */
		mmap_read_unlock(pages->source_mm);	/* [한국어] 먼저 놓는다 — 같은 락을 읽기에서 쓰기로 올릴 수 없다. */
		user->locked = 0;	/* [한국어] 참조만 든 상태로. */
		/* If we had the lock then we also have a get */

	} else if ((!user || (!user->upages && !user->ufolios)) &&	/* [한국어] 아직 아무 버퍼도 잡지 않았고 */
		   pages->source_mm != current->mm) {	/* [한국어] 남의 주소 공간이면 */
		if (!mmget_not_zero(pages->source_mm))	/* [한국어] 붙잡아야 한다. 이미 사라졌으면 */
			return -EINVAL;	/* [한국어] 회계를 갱신할 수 없다. */
		do_put = true;	/* [한국어] 여기서 들었으니 나갈 때 놓는다. */
	}

	mmap_write_lock(pages->source_mm);	/* [한국어] 회계 갱신에는 쓰기 락이 필요하다. 이것이 이 방식이 무거운 이유다. */
	rc = __account_locked_vm(pages->source_mm, npages, inc,	/* [한국어] mm 단위 잠금 회계를 갱신한다. 옛 VFIO 와 같은 숫자가 나오게 하려는 것이다. */
				 pages->source_task, false);
	mmap_write_unlock(pages->source_mm);	/* [한국어] 락 해제. */

	if (do_put)	/* [한국어] 여기서 들었으면 */
		mmput(pages->source_mm);	/* [한국어] 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iopt_pages_update_pinned - 잠금 메모리 회계를 갱신한다
 *
 * @pages: 대상 pages 객체.
 * @npages: 늘리거나 줄일 페이지 수.
 * @inc: 참이면 늘린다.
 * @user: 원천 읽기 상태(없으면 NULL).
 * @return: 0 성공, 한도를 넘으면 음수.
 *
 * 회계 방식 세 가지를 여기서 가른다. CAP_IPC_LOCK 을 가진 프로세스는
 * 한도가 없어 아무것도 세지 않는다.
 *
 * pinned_vm 은 방식과 무관하게 늘 갱신한다 — 그것은 한도가 아니라
 * 통계라서, /proc 에서 이 프로세스가 얼마나 고정했는지 보여 준다.
 */
int iopt_pages_update_pinned(struct iopt_pages *pages, unsigned long npages,
			     bool inc, struct pfn_reader_user *user)
{
	int rc = 0;	/* [한국어] 결과 코드. */

	switch (pages->account_mode) {	/* [한국어] 회계 방식은 pages 를 만들 때 정해진다. */
	case IOPT_PAGES_ACCOUNT_NONE:	/* [한국어] CAP_IPC_LOCK 을 가진 프로세스는 한도가 없다. */
		break;	/* [한국어] 아무것도 세지 않는다. */
	case IOPT_PAGES_ACCOUNT_USER:	/* [한국어] 사용자 단위(새 방식). */
		if (inc)	/* [한국어] 늘리는 쪽이면 */
			rc = incr_user_locked_vm(pages, npages);	/* [한국어] 한도를 확인하며 더한다. */
		else
			decr_user_locked_vm(pages, npages);	/* [한국어] 줄이는 쪽은 확인 없이 뺀다. */
		break;	/* [한국어] 다음으로. */
	case IOPT_PAGES_ACCOUNT_MM:	/* [한국어] mm 단위(옛 VFIO 호환). */
		rc = update_mm_locked_vm(pages, npages, inc, user);	/* [한국어] 무겁지만 옛 프로그램이 보는 숫자와 맞는다. */
		break;	/* [한국어] 다음으로. */
	}
	if (rc)	/* [한국어] 한도를 넘었으면 */
		return rc;	/* [한국어] 아래를 하지 않고 실패를 올린다. */

	pages->last_npinned = pages->npinned;	/* [한국어] 반영을 마쳤음을 기록한다. 다음 갱신은 이 값과의 차이만 다룬다. */
	if (inc)	/* [한국어] 늘렸으면 */
		atomic64_add(npages, &pages->source_mm->pinned_vm);	/* [한국어] 통계도 늘린다. 이것은 한도가 아니라 /proc 에 보이는 값이다. */
	else
		atomic64_sub(npages, &pages->source_mm->pinned_vm);	/* [한국어] 줄였으면 통계도 줄인다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * update_unpinned - 놓인 만큼만 회계에 되돌려 준다
 *
 * @pages: 대상 pages 객체.
 *
 * npinned 는 실제 고정 수, last_npinned 는 회계에 반영된 수다. 둘의 차이가
 * 아직 반영하지 않은 몫이다.
 *
 * 늘어난 방향이면 아무것도 하지 않는다 — 이 함수는 놓는 쪽 전용이라,
 * 늘어난 것은 다른 경로가 한도를 확인하며 반영해야 한다.
 */
static void update_unpinned(struct iopt_pages *pages)
{
	if (WARN_ON(pages->npinned > pages->last_npinned))	/* [한국어] 늘어난 방향이면 이 함수가 다룰 일이 아니다. */
		return;	/* [한국어] 아무것도 하지 않는다. */
	if (pages->npinned == pages->last_npinned)	/* [한국어] 반영할 차이가 없으면 */
		return;	/* [한국어] 할 일이 없다. */
	iopt_pages_update_pinned(pages, pages->last_npinned - pages->npinned,	/* [한국어] 놓인 만큼을 회계에 되돌려 준다. 줄이는 쪽이라 실패하지 않는다. */
				 false, NULL);
}

/*
 * Changes in the number of pages pinned is done after the pages have been read
 * and processed. If the user lacked the limit then the error unwind will unpin
 * everything that was just pinned. This is because it is expensive to calculate
 * how many pages we have already pinned within a range to generate an accurate
 * prediction in advance of doing the work to actually pin them.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pfn_reader_user_update_pinned - 고정 수 변화를 회계에 반영한다
 *
 * @user: 원천 읽기 상태.
 * @pages: 대상 pages 객체.
 * @return: 0 성공, 한도를 넘으면 -ENOMEM.
 *
 * 원 주석이 "먼저 고정하고 나중에 확인하는" 순서의 이유를 밝힌다 —
 * 어떤 범위에서 이미 몇 페이지를 고정해 두었는지 미리 세는 것이
 * 실제로 고정하는 것보다 비싸다. 그래서 일단 하고, 한도를 넘었으면
 * 오류 되감기가 방금 고정한 것을 모두 놓는다.
 */
static int pfn_reader_user_update_pinned(struct pfn_reader_user *user,
					 struct iopt_pages *pages)
{
	unsigned long npages;	/* [한국어] 반영할 개수. */
	bool inc;	/* [한국어] 늘리는 방향인가. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 두 값을 함께 보므로 뮤텍스 아래여야 한다. */

	if (pages->npinned == pages->last_npinned)	/* [한국어] 차이가 없으면 */
		return 0;	/* [한국어] 할 일이 없다. */

	if (pages->npinned < pages->last_npinned) {	/* [한국어] 줄어든 방향이면 */
		npages = pages->last_npinned - pages->npinned;	/* [한국어] 그 차이를 */
		inc = false;	/* [한국어] 되돌려 준다. */
	} else {
		if (iommufd_should_fail())	/* [한국어] 늘어난 방향에서만 결함 주입 시험을 한다 — 한도 초과 되감기를 시험하려는 것이다. */
			return -ENOMEM;	/* [한국어] 실패를 흉내 낸다. */
		npages = pages->npinned - pages->last_npinned;	/* [한국어] 늘어난 차이를 */
		inc = true;	/* [한국어] 한도를 확인하며 더한다. */
	}
	return iopt_pages_update_pinned(pages, npages, inc, user);	/* [한국어] 실제 갱신. 한도를 넘으면 호출자가 되감는다. */
}

/*
 * [한국어] dmabuf 원천에서 PFN 을 끌어오는 상태.
 *
 * dmabuf 는 이미 물리적으로 이어진 한 덩어리라 훨씬 단순하다. 고정도
 * 회계도 필요 없고, 시작 물리 주소와 오프셋만 알면 된다.
 */
struct pfn_reader_dmabuf {
	/* [한국어] dmabuf 가 차지한 물리 주소 구간.
	 *  설정자: pfn_reader_dmabuf_init 이 pages 에서 복사해 온다.
	 *  읽는 자: pfn_reader_fill_dmabuf 가 PFN 을 만들어 낼 때.
	 *  이어진 한 덩어리라 시작 주소와 길이만으로 표현된다.
	 *  len 이 0 이면 무효가 된 것이다. */
	struct phys_vec phys;
	/* [한국어] 그 구간 안에서 이 pages 가 시작하는 오프셋.
	 *  설정자: pfn_reader_dmabuf_init.
	 *  사용자가 dmabuf 의 일부만 잘라 매핑할 수 있어 필요한 값이다. */
	unsigned long start_offset;
};

/*
 * [한국어]
 * pfn_reader_dmabuf_init - dmabuf 원천 읽기 상태를 세운다
 *
 * @dmabuf: 초기화할 상태.
 * @pages: dmabuf 를 든 pages 객체.
 * @return: 0 성공, 이미 무효가 됐으면 -EINVAL.
 *
 * 원 주석대로 무효가 된 dmabuf 로는 여기 오면 안 된다 — 그 물리 주소는
 * 더 이상 유효하지 않다.
 */
static int pfn_reader_dmabuf_init(struct pfn_reader_dmabuf *dmabuf,
				  struct iopt_pages *pages)
{
	/* Callers must not get here if the dmabuf was already revoked */
	if (WARN_ON(iopt_dmabuf_revoked(pages)))	/* [한국어] 원 주석대로 무효가 된 dmabuf 로는 여기 오면 안 된다. */
		return -EINVAL;	/* [한국어] 거절. */

	dmabuf->phys = pages->dmabuf.phys;	/* [한국어] 물리 구간을 복사해 둔다. */
	dmabuf->start_offset = pages->dmabuf.start;	/* [한국어] 그 안에서의 시작 오프셋. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * pfn_reader_fill_dmabuf - dmabuf 구간을 묶음에 담는다
 *
 * @dmabuf: 원천 상태.
 * @batch: 채울 묶음.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @return: 늘 0.
 *
 * 이어진 물리 구간이라 항목 하나로 끝난다. 자리가 없어 실패할 일도 없다.
 *
 * 원 주석대로 페이지 단위로 맞춰 담고, 페이지보다 잘게 잘린 dmabuf 의
 * 오프셋 보정은 도메인에 매핑할 때 공통 코드가 한다.
 *
 * BATCH_MMIO 로 표시해 캐시 속성이 다르게 매핑되게 한다.
 */
static int pfn_reader_fill_dmabuf(struct pfn_reader_dmabuf *dmabuf,
				  struct pfn_batch *batch,
				  unsigned long start_index,
				  unsigned long last_index)
{
	unsigned long start = dmabuf->start_offset + start_index * PAGE_SIZE;	/* [한국어] 이 구간이 시작하는 오프셋. */

	/*
	 * start/last_index and start are all PAGE_SIZE aligned, the batch is
	 * always filled using page size aligned PFNs just like the other types.
	 * If the dmabuf has been sliced on a sub page offset then the common
	 * batch to domain code will adjust it before mapping to the domain.
	 */
	batch_add_pfn_num(batch, PHYS_PFN(dmabuf->phys.paddr + start),	/* [한국어] 이어진 한 덩어리라 항목 하나로 끝난다. */
			  last_index - start_index + 1, BATCH_MMIO);	/* [한국어] MMIO 로 표시해 캐시 속성이 다르게 매핑되게 한다. */
	return 0;	/* [한국어] 실패할 수 없다. */
}

/*
 * PFNs are stored in three places, in order of preference:
 * - The iopt_pages xarray. This is only populated if there is a
 *   iopt_pages_access
 * - The iommu_domain under an area
 * - The original PFN source, ie pages->source_mm
 *
 * This iterator reads the pfns optimizing to load according to the
 * above order.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * 한 구간의 PFN 을 가장 싼 저장 자리에서 꺼내 오는 반복자.
 *
 * 두 구간 트리를 겹쳐 훑으며, 이 구간이 xarray 에 있는지, 도메인에
 * 있는지, 아니면 원천에서 새로 고정해야 하는지를 판정한다.
 *
 * union 인 이유: 원천은 사용자 메모리이거나 dmabuf 이지 둘 다일 수 없다.
 */
struct pfn_reader {
	/* [한국어] 이 반복자가 읽고 있는 대상.
	 *  설정자: pfn_reader_init. 읽는 자: 거의 모든 반복자 함수.
	 *  동기화: 이 반복자를 쓰는 동안 pages 뮤텍스를 쥐고 있어야 한다. */
	struct iopt_pages *pages;
	/* [한국어] 두 구간 트리를 겹쳐 훑는 반복자.
	 *  설정자: pfn_reader_init 이 시작시키고 pfn_reader_next 가 전진시킨다.
	 *  읽는 자: pfn_reader_fill_span 이 is_used 값으로 저장 자리를 고른다.
	 *  이것이 "어느 층에서 읽을 것인가"를 답하는 장치다. */
	struct interval_tree_double_span_iter span;
	/* [한국어] 이번에 꺼내 온 PFN 들.
	 *  설정자: fill_span 이 채우고 next 가 비운다.
	 *  읽는 자: 호출자가 batch_to_domain 등으로 처리한다.
	 *  배열은 init 에서 잡고 destroy 에서 놓는다. */
	struct pfn_batch batch;
	/* [한국어] 지금 묶음의 첫 페이지 번호.
	 *  설정자: pfn_reader_next 가 바퀴마다 갱신.
	 *  읽는 자: 호출자가 도메인에 매핑할 위치를 정할 때. */
	unsigned long batch_start_index;
	/* [한국어] 지금 묶음의 마지막 페이지 번호의 다음.
	 *  설정자: pfn_reader_next.
	 *  읽는 자: fill_span 이 어디서부터 이어 읽을지 정할 때,
	 *  release_pins 가 고정했지만 못 옮긴 몫을 셀 때. */
	unsigned long batch_end_index;
	/* [한국어] 이 반복자가 읽어야 할 마지막 페이지 번호(포함).
	 *  설정자: pfn_reader_init.
	 *  읽는 자: pfn_reader_done 이 끝을 판정할 때. */
	unsigned long last_index;

	/* [한국어] 원천 읽기 상태. 종류에 따라 하나만 쓰인다.
	 *  dmabuf 인지 아닌지는 pages 의 종류로 알 수 있어, 어느 쪽인지 따로
	 *  표시하지 않는다. */
	union {
		/* [한국어] 사용자 메모리 또는 파일이 원천일 때. */
		struct pfn_reader_user user;
		/* [한국어] dmabuf 가 원천일 때. */
		struct pfn_reader_dmabuf dmabuf;
	};
};

/*
 * [한국어]
 * pfn_reader_update_pinned - 반복자가 든 고정 변화를 회계에 반영한다
 *
 * @pfns: 대상 반복자.
 * @return: 0 성공, 음수면 실패.
 *
 * 안쪽 함수로 넘기는 얇은 껍데기다.
 */
static int pfn_reader_update_pinned(struct pfn_reader *pfns)
{
	return pfn_reader_user_update_pinned(&pfns->user, pfns->pages);	/* [한국어] 안쪽 상태를 꺼내 넘긴다. */
}

/*
 * The batch can contain a mixture of pages that are still in use and pages that
 * need to be unpinned. Unpin only pages that are not held anywhere else.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pfn_reader_unpin - 아무도 붙잡지 않은 페이지만 놓는다
 *
 * @pfns: 대상 반복자.
 *
 * 묶음 안에는 아직 쓰이는 페이지와 놓아도 되는 페이지가 섞여 있다.
 * 두 구간 트리 어느 쪽도 덮지 않는 "구멍"만 놓는 것이 그 판별이다.
 *
 * 이것이 세 층 저장 구조의 핵심 규칙이다 — 어딘가 하나라도 들고 있으면
 * 고정을 유지해야 한다.
 */
static void pfn_reader_unpin(struct pfn_reader *pfns)
{
	unsigned long last = pfns->batch_end_index - 1;	/* [한국어] 이번 묶음의 마지막 번호. */
	unsigned long start = pfns->batch_start_index;	/* [한국어] 첫 번호. */
	struct interval_tree_double_span_iter span;	/* [한국어] 두 트리를 겹쳐 훑을 반복자. */
	struct iopt_pages *pages = pfns->pages;	/* [한국어] 대상 pages 객체. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 두 트리를 보므로 뮤텍스 아래여야 한다. */

	interval_tree_for_each_double_span(&span, &pages->access_itree,	/* [한국어] 접근자와 도메인, 두 트리를 겹쳐 훑는다. */
					   &pages->domains_itree, start, last) {
		if (span.is_used)	/* [한국어] 어느 한쪽이라도 들고 있으면 */
			continue;	/* [한국어] 고정을 유지해야 한다. 이것이 세 층 구조의 핵심 규칙이다. */

		batch_unpin(&pfns->batch, pages, span.start_hole - start,	/* [한국어] 아무도 안 쓰는 구간만 놓는다. 묶음 시작에서의 상대 위치로 넘긴다. */
			    span.last_hole - span.start_hole + 1);
	}
}

/* Process a single span to load it from the proper storage */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pfn_reader_fill_span - 지금 구간을 알맞은 저장 자리에서 읽어 온다
 *
 * @pfns: 대상 반복자.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 파일의 층 선택이 여기서 일어난다. is_used 값이 그것을 말해 준다 —
 * 1 이면 접근자가 붙잡은 구간이라 xarray 에 있고, 2 면 도메인이 들고
 * 있으며, 0 이면 어디에도 없어 원천에서 고정해야 한다.
 *
 * 도메인에서 읽을 때 영역 하나 분량만 가져오는 이유를 원 주석이 밝힌다 —
 * 모자라면 다시 불려 다음 영역을 찾으면 된다.
 *
 * storage_domain 이 pages 뮤텍스 없이는 바뀌지 않는다는 것이 이 코드가
 * 기대는 불변식이다.
 */
static int pfn_reader_fill_span(struct pfn_reader *pfns)
{
	struct interval_tree_double_span_iter *span = &pfns->span;	/* [한국어] 지금 보고 있는 구간. */
	unsigned long start_index = pfns->batch_end_index;	/* [한국어] 이어서 읽을 지점. */
	struct pfn_reader_user *user;	/* [한국어] 원천 읽기 상태(필요할 때만). */
	unsigned long npages;	/* [한국어] 묶음으로 옮길 개수. */
	struct iopt_area *area;	/* [한국어] 도메인에서 읽을 때 IOVA 를 아는 영역. */
	int rc;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드 전용 인자 검사. */
	    WARN_ON(span->last_used < start_index))	/* [한국어] 반복자와 진행 지점이 어긋났다면 버그다. */
		return -EINVAL;

	if (span->is_used == 1) {	/* [한국어] 접근자가 붙잡은 구간 = xarray 에 있다. */
		batch_from_xarray(&pfns->batch, &pfns->pages->pinned_pfns,	/* [한국어] 가장 싼 길로 읽는다. */
				  start_index, span->last_used);
		return 0;	/* [한국어] 성공. */
	}

	if (span->is_used == 2) {	/* [한국어] 도메인이 들고 있는 구간. */
		/*
		 * Pull as many pages from the first domain we find in the
		 * target span. If it is too small then we will be called again
		 * and we'll find another area.
		 */
		area = iopt_pages_find_domain_area(pfns->pages, start_index);	/* [한국어] 어느 IOVA 인지 아는 영역을 찾는다. */
		if (WARN_ON(!area))	/* [한국어] 트리가 덮는다고 했는데 영역이 없으면 앞뒤가 맞지 않는다. */
			return -EINVAL;	/* [한국어] 실패. */

		/* The storage_domain cannot change without the pages mutex */
		batch_from_domain(	/* [한국어] 원 주석대로 이 영역이 덮는 만큼만 읽는다. 모자라면 다시 불려 다음 영역을 찾는다. */
			&pfns->batch, area->storage_domain, area, start_index,	/* [한국어] storage_domain 은 pages 뮤텍스 없이 바뀌지 않는다 — 이 코드가 기대는 불변식이다. */
			min(iopt_area_last_index(area), span->last_used));	/* [한국어] 영역의 끝과 구간의 끝 중 앞선 쪽까지. */
		return 0;	/* [한국어] 성공. */
	}

	if (iopt_is_dmabuf(pfns->pages))	/* [한국어] 원천이 dmabuf 면 */
		return pfn_reader_fill_dmabuf(&pfns->dmabuf, &pfns->batch,	/* [한국어] 고정 없이 물리 주소를 그대로 만들어 낸다. */
					      start_index, span->last_hole);

	user = &pfns->user;	/* [한국어] 여기부터는 원천에서 고정해 와야 한다. */
	if (start_index >= user->upages_end) {	/* [한국어] 이미 고정해 둔 범위를 넘었으면 */
		rc = pfn_reader_user_pin(user, pfns->pages, start_index,	/* [한국어] 더 고정해 온다. */
					 span->last_hole);
		if (rc)	/* [한국어] 실패하면 */
			return rc;	/* [한국어] 그대로 올린다. */
	}

	npages = user->upages_end - start_index;	/* [한국어] 고정해 둔 것 중 아직 안 옮긴 개수. */
	start_index -= user->upages_start;	/* [한국어] 배열 안의 첨자로 바꾼다. */
	rc = 0;	/* [한국어] 아래에서 갱신될 수 있다. */

	if (!user->file)	/* [한국어] 사용자 포인터 원천이면 */
		batch_from_pages(&pfns->batch, user->upages + start_index,	/* [한국어] 페이지 배열에서 옮긴다. */
				 npages);
	else
		rc = batch_from_folios(&pfns->batch, &user->ufolios_next,	/* [한국어] 파일 원천이면 folio 배열에서 옮긴다. 참조를 더 잡느라 실패할 수 있다. */
				       &user->ufolios_offset, npages);
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * pfn_reader_done - 요청한 구간을 모두 읽었는가
 *
 * @pfns: 대상 반복자.
 * @return: 다 읽었으면 참.
 *
 * 마지막 번호를 하나 지나쳤으면 끝이다.
 */
static bool pfn_reader_done(struct pfn_reader *pfns)
{
	return pfns->batch_start_index == pfns->last_index + 1;	/* [한국어] 마지막 번호를 하나 지나쳤으면 끝이다. */
}

/*
 * [한국어]
 * pfn_reader_next - 다음 묶음을 채운다
 *
 * @pfns: 대상 반복자.
 * @return: 0 성공, 음수면 실패.
 *
 * 묶음이 찰 때까지 여러 구간을 이어 담는다. 구간마다 저장 자리가 다를 수
 * 있지만, 담기고 나면 모두 같은 PFN 목록이다.
 *
 * 담긴 개수가 늘지 않았다는 것이 곧 "묶음이 찼다"는 신호다 — 그때 돌아가
 * 호출자가 처리하게 한다.
 */
static int pfn_reader_next(struct pfn_reader *pfns)
{
	int rc;	/* [한국어] 결과 코드. */

	batch_clear(&pfns->batch);	/* [한국어] 지난 묶음을 비운다. */
	pfns->batch_start_index = pfns->batch_end_index;	/* [한국어] 새 묶음은 지난 것이 끝난 자리에서 시작한다. */

	while (pfns->batch_end_index != pfns->last_index + 1) {	/* [한국어] 요청한 끝에 닿을 때까지. */
		unsigned int npfns = pfns->batch.total_pfns;	/* [한국어] 채우기 전의 개수. 늘지 않으면 묶음이 찬 것이다. */

		if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드 전용 확인. */
		    WARN_ON(interval_tree_double_span_iter_done(&pfns->span)))	/* [한국어] 아직 읽을 것이 남았는데 반복자가 끝났으면 버그다. */
			return -EINVAL;

		rc = pfn_reader_fill_span(pfns);	/* [한국어] 이 구간을 알맞은 층에서 읽어 온다. */
		if (rc)	/* [한국어] 실패하면 */
			return rc;	/* [한국어] 그대로 올린다. */

		if (WARN_ON(!pfns->batch.total_pfns))	/* [한국어] 한 개도 못 담았으면 진행이 멈춰 무한 고리가 된다. */
			return -EINVAL;	/* [한국어] 실패로 끊는다. */

		pfns->batch_end_index =	/* [한국어] 담긴 만큼 진행 지점을 옮긴다. */
			pfns->batch_start_index + pfns->batch.total_pfns;	/* [한국어] 시작에 담긴 개수를 더한 값. */
		if (pfns->batch_end_index == pfns->span.last_used + 1)	/* [한국어] 이 구간을 다 읽었으면 */
			interval_tree_double_span_iter_next(&pfns->span);	/* [한국어] 다음 구간으로 넘어간다. */

		/* Batch is full */
		if (npfns == pfns->batch.total_pfns)	/* [한국어] 한 개도 늘지 않았다 = 묶음이 찼다. */
			return 0;	/* [한국어] 호출자가 처리하도록 돌아간다. */
	}
	return 0;	/* [한국어] 요청한 범위를 모두 담았다. */
}

/*
 * [한국어]
 * pfn_reader_init - 반복자를 세운다
 *
 * @pfns: 초기화할 반복자.
 * @pages: 대상 pages 객체.
 * @start_index: 읽을 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 원천 종류에 따라 union 의 알맞은 쪽을 세우고, 묶음 배열을 잡고, 두
 * 구간 트리의 겹친 순회를 시작한다.
 */
static int pfn_reader_init(struct pfn_reader *pfns, struct iopt_pages *pages,
			   unsigned long start_index, unsigned long last_index)
{
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 반복자를 쓰는 내내 뮤텍스를 쥐고 있어야 한다. */

	pfns->pages = pages;	/* [한국어] 대상을 기억한다. */
	pfns->batch_start_index = start_index;	/* [한국어] 아직 아무것도 담지 않은 상태. */
	pfns->batch_end_index = start_index;	/* [한국어] 같은 지점. */
	pfns->last_index = last_index;	/* [한국어] 읽어야 할 끝. */
	if (iopt_is_dmabuf(pages))	/* [한국어] 원천 종류에 따라 */
		pfn_reader_dmabuf_init(&pfns->dmabuf, pages);	/* [한국어] union 의 알맞은 쪽을 세운다. */
	else
		pfn_reader_user_init(&pfns->user, pages);	/* [한국어] 사용자 메모리 또는 파일 쪽. */
	rc = batch_init(&pfns->batch, last_index - start_index + 1);	/* [한국어] 묶음 배열을 잡는다. 실패할 수 있는 경로다. */
	if (rc)	/* [한국어] 메모리가 없다. */
		return rc;	/* [한국어] 실패. */
	interval_tree_double_span_iter_first(&pfns->span, &pages->access_itree,	/* [한국어] 두 트리의 겹친 순회를 시작한다. 순서에 주의 — 접근자 트리가 먼저라 is_used 1 이 xarray 를 뜻한다. */
					     &pages->domains_itree, start_index,
					     last_index);
	return 0;	/* [한국어] 성공. */
}

/*
 * There are many assertions regarding the state of pages->npinned vs
 * pages->last_pinned, for instance something like unmapping a domain must only
 * decrement the npinned, and pfn_reader_destroy() must be called only after all
 * the pins are updated. This is fine for success flows, but error flows
 * sometimes need to release the pins held inside the pfn_reader before going on
 * to complete unmapping and releasing pins held in domains.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pfn_reader_release_pins - 반복자가 들고 있던 고정을 놓는다
 *
 * @pfns: 대상 반복자.
 *
 * 원 주석이 이 함수가 왜 따로 있는지 밝힌다 — 정상 흐름에서는 destroy 가
 * 마지막에 정리하면 되지만, 오류 되감기는 도메인 쪽 정리를 하기 전에
 * 반복자가 든 고정을 먼저 놓아야 한다. 그러지 않으면 npinned 에 대한
 * 여러 단언이 어긋난다.
 *
 * 두 몫을 나눠 놓는다. 고정은 했지만 묶음에 옮기지 못한 것은 그냥
 * 놓으면 되고, 묶음에 있는 것은 다른 곳이 붙잡고 있는지 보아야 한다.
 */
static void pfn_reader_release_pins(struct pfn_reader *pfns)
{
	struct iopt_pages *pages = pfns->pages;	/* [한국어] 대상 pages 객체. */
	struct pfn_reader_user *user;	/* [한국어] 원천 읽기 상태. */

	if (iopt_is_dmabuf(pages))	/* [한국어] dmabuf 는 고정이 없다. */
		return;	/* [한국어] 놓을 것이 없다. */

	user = &pfns->user;	/* [한국어] 사용자 원천 상태. */
	if (user->upages_end > pfns->batch_end_index) {	/* [한국어] 고정은 했지만 묶음으로 옮기지 못한 몫이 있으면 */
		/* Any pages not transferred to the batch are just unpinned */

		unsigned long npages = user->upages_end - pfns->batch_end_index;	/* [한국어] 그 개수. */
		unsigned long start_index = pfns->batch_end_index -	/* [한국어] 배열 안에서의 시작 첨자. */
					    user->upages_start;

		if (!user->file) {	/* [한국어] 사용자 포인터 원천이면 */
			unpin_user_pages(user->upages + start_index, npages);	/* [한국어] 그 페이지들의 고정을 놓는다. 어디에도 옮기지 않았으므로 그냥 놓으면 된다. */
		} else {
			long n = user->ufolios_len / sizeof(*user->ufolios);	/* [한국어] folio 배열의 크기. */

			unpin_folios(user->ufolios_next,	/* [한국어] 아직 옮기지 않은 folio 들을 놓는다. */
				     user->ufolios + n - user->ufolios_next);
		}
		iopt_pages_sub_npinned(pages, npages);	/* [한국어] 회계도 함께 줄인다. */
		user->upages_end = pfns->batch_end_index;	/* [한국어] 고정해 둔 범위를 줄인다. */
	}
	if (pfns->batch_start_index != pfns->batch_end_index) {	/* [한국어] 묶음에 담긴 것이 있으면 */
		pfn_reader_unpin(pfns);	/* [한국어] 아무도 붙잡지 않은 것만 놓는다. */
		pfns->batch_start_index = pfns->batch_end_index;	/* [한국어] 묶음을 비운 것으로 표시한다. */
	}
}

/*
 * [한국어]
 * pfn_reader_destroy - 반복자를 정리한다
 *
 * @pfns: 정리할 반복자.
 *
 * 마지막 WARN 이 중요하다 — 회계에 반영된 수와 실제 고정 수가 같아야
 * 한다. 다르면 어딘가에서 회계를 빠뜨린 것이다.
 */
static void pfn_reader_destroy(struct pfn_reader *pfns)
{
	struct iopt_pages *pages = pfns->pages;	/* [한국어] 대상 pages 객체. */

	pfn_reader_release_pins(pfns);	/* [한국어] 아직 들고 있던 고정을 놓는다. */
	if (!iopt_is_dmabuf(pfns->pages))	/* [한국어] dmabuf 는 원천 상태가 없다. */
		pfn_reader_user_destroy(&pfns->user, pfns->pages);	/* [한국어] 버퍼와 mm 참조를 놓는다. */
	batch_destroy(&pfns->batch, NULL);	/* [한국어] 묶음 배열 해제. */
	WARN_ON(pages->last_npinned != pages->npinned);	/* [한국어] 회계에 반영된 수와 실제 고정 수가 같아야 한다. 다르면 어딘가에서 갱신을 빠뜨린 것이다. */
}

/*
 * [한국어]
 * pfn_reader_first - 반복자를 세우고 첫 묶음을 채운다
 *
 * @pfns: 초기화할 반복자.
 * @pages: 대상 pages 객체.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @return: 0 성공, 음수면 실패.
 *
 * 실패하면 여기서 정리까지 해 준다 — 호출자가 세우기와 첫 채우기의
 * 실패를 따로 다루지 않아도 되게.
 */
static int pfn_reader_first(struct pfn_reader *pfns, struct iopt_pages *pages,
			    unsigned long start_index, unsigned long last_index)
{
	int rc;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드 전용 인자 검사. */
	    WARN_ON(last_index < start_index))	/* [한국어] 뒤집힌 범위는 호출자의 버그다. */
		return -EINVAL;

	rc = pfn_reader_init(pfns, pages, start_index, last_index);	/* [한국어] 반복자를 세운다. */
	if (rc)	/* [한국어] 실패하면 */
		return rc;	/* [한국어] 정리할 것이 없다. */
	rc = pfn_reader_next(pfns);	/* [한국어] 첫 묶음을 채운다. */
	if (rc) {	/* [한국어] 실패하면 */
		pfn_reader_destroy(pfns);	/* [한국어] 세운 것을 여기서 정리해 준다. */
		return rc;	/* [한국어] 실패를 올린다. */
	}
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iopt_alloc_pages - pages 객체의 공통 부분을 만든다
 *
 * @start_byte: 첫 페이지 안에서의 시작 오프셋.
 * @length: 전체 길이(바이트).
 * @writable: 쓰기 가능한 매핑인가.
 * @return: 만들어진 객체, 실패하면 오류 포인터.
 *
 * 원 주석대로 길이 상한을 두는 이유는 아래 올림 나눗셈이 넘치지 않게
 * 하려는 것이다.
 *
 * 회계 방식을 여기서 고른다. CAP_IPC_LOCK 이 있으면 한도가 없어 세지
 * 않는다.
 *
 * mmgrab 을 쓰는 데 주의 — mmget 이 아니다. 주소 공간의 내용이 아니라
 * 구조체 자체만 붙잡는 것이라, 프로세스가 죽어도 이 포인터는 유효하다.
 *
 * source_task 를 group_leader 로 잡는 이유: rlimit 은 스레드가 아니라
 * 프로세스 단위다.
 */
static struct iopt_pages *iopt_alloc_pages(unsigned long start_byte,
					   unsigned long length, bool writable)
{
	struct iopt_pages *pages;	/* [한국어] 만들 객체. */

	/*
	 * The iommu API uses size_t as the length, and protect the DIV_ROUND_UP
	 * below from overflow
	 */
	if (length > SIZE_MAX - PAGE_SIZE || length == 0)	/* [한국어] 원 주석대로 아래 올림 나눗셈이 넘치지 않게 상한을 둔다. 길이 0 은 뜻이 없다. */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절. */

	pages = kzalloc_obj(*pages, GFP_KERNEL_ACCOUNT);	/* [한국어] ACCOUNT 를 붙여 이 할당이 사용자의 cgroup 메모리에 잡히게 한다. */
	if (!pages)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	kref_init(&pages->kref);	/* [한국어] 참조를 1 로 시작한다. */
	xa_init_flags(&pages->pinned_pfns, XA_FLAGS_ACCOUNT);	/* [한국어] xarray 노드도 사용자 앞으로 계산되게 한다. */
	mutex_init(&pages->mutex);	/* [한국어] 이 객체의 모든 상태를 지키는 뮤텍스. */
	pages->source_mm = current->mm;	/* [한국어] 원천 주소 공간. */
	mmgrab(pages->source_mm);	/* [한국어] grab 이지 get 이 아니다 — 구조체만 붙잡아, 프로세스가 죽어도 이 포인터가 유효하다. */
	pages->npages = DIV_ROUND_UP(length + start_byte, PAGE_SIZE);	/* [한국어] 앞뒤로 걸친 페이지까지 세야 하므로 오프셋을 더하고 올림한다. */
	pages->access_itree = RB_ROOT_CACHED;	/* [한국어] 접근자 구간 트리를 비운다. */
	pages->domains_itree = RB_ROOT_CACHED;	/* [한국어] 도메인 구간 트리를 비운다. */
	pages->writable = writable;	/* [한국어] 쓰기 가능한 매핑인가. */
	if (capable(CAP_IPC_LOCK))	/* [한국어] 메모리를 마음껏 잠글 권한이 있으면 */
		pages->account_mode = IOPT_PAGES_ACCOUNT_NONE;	/* [한국어] 한도가 없어 세지 않는다. */
	else
		pages->account_mode = IOPT_PAGES_ACCOUNT_USER;	/* [한국어] 없으면 사용자 단위로 센다. */
	pages->source_task = current->group_leader;	/* [한국어] rlimit 은 스레드가 아니라 프로세스 단위라 대표 스레드를 잡는다. */
	get_task_struct(current->group_leader);	/* [한국어] 그 구조체가 사라지지 않게 참조를 든다. */
	pages->source_user = get_uid(current_user());	/* [한국어] 사용자 단위 회계에 쓸 사용자 구조체. */
	return pages;	/* [한국어] 만들어진 객체. */
}

/*
 * [한국어]
 * iopt_alloc_user_pages - 사용자 포인터를 원천으로 하는 pages 를 만든다
 *
 * @uptr: 사용자 메모리 주소.
 * @length: 길이.
 * @writable: 쓰기 가능한가.
 * @return: 만들어진 객체, 실패하면 오류 포인터.
 *
 * 주소를 페이지 경계로 내림해 두고, 그 안에서의 오프셋을 따로 기억한다.
 * PFN 은 늘 페이지 단위이지만 사용자가 준 주소는 그렇지 않기 때문이다.
 */
struct iopt_pages *iopt_alloc_user_pages(void __user *uptr,
					 unsigned long length, bool writable)
{
	struct iopt_pages *pages;	/* [한국어] 만들 객체. */
	unsigned long end;	/* [한국어] 넘침 검사용. */
	void __user *uptr_down =	/* [한국어] 페이지 경계로 내린 주소. PFN 은 늘 페이지 단위다. */
		(void __user *)ALIGN_DOWN((uintptr_t)uptr, PAGE_SIZE);

	if (check_add_overflow((unsigned long)uptr, length, &end))	/* [한국어] 주소가 넘치면 */
		return ERR_PTR(-EOVERFLOW);	/* [한국어] 거절. */

	pages = iopt_alloc_pages(uptr - uptr_down, length, writable);	/* [한국어] 내림한 만큼이 곧 첫 페이지 안의 오프셋이다. */
	if (IS_ERR(pages))	/* [한국어] 실패하면 */
		return pages;	/* [한국어] 그대로 올린다. */
	pages->uptr = uptr_down;	/* [한국어] 페이지 경계에 맞춘 주소를 기억한다. */
	pages->type = IOPT_ADDRESS_USER;	/* [한국어] 원천 종류. */
	return pages;	/* [한국어] 만들어진 객체. */
}

/*
 * [한국어]
 * iopt_alloc_file_pages - 파일을 원천으로 하는 pages 를 만든다
 *
 * @file: 원천 파일(대개 memfd).
 * @start_byte: 첫 페이지 안에서의 오프셋.
 * @start: 파일 안의 시작 오프셋.
 * @length: 길이.
 * @writable: 쓰기 가능한가.
 * @return: 만들어진 객체, 실패하면 오류 포인터.
 *
 * 파일 참조를 들어 둔다 — 매핑이 살아 있는 동안 파일이 사라지면 안 된다.
 *
 * start 에서 start_byte 를 빼 페이지 경계에 맞춘 값을 기억한다. 사용자
 * 포인터 판이 주소를 내림하는 것과 같은 이치다.
 */
struct iopt_pages *iopt_alloc_file_pages(struct file *file,
					 unsigned long start_byte,
					 unsigned long start,
					 unsigned long length, bool writable)

{
	struct iopt_pages *pages;	/* [한국어] 만들 객체. */

	pages = iopt_alloc_pages(start_byte, length, writable);	/* [한국어] 공통 부분. */
	if (IS_ERR(pages))	/* [한국어] 실패하면 */
		return pages;	/* [한국어] 그대로 올린다. */
	pages->file = get_file(file);	/* [한국어] 매핑이 살아 있는 동안 파일이 사라지면 안 된다. */
	pages->start = start - start_byte;	/* [한국어] 페이지 경계에 맞춘 파일 오프셋. 사용자 포인터 판이 주소를 내리는 것과 같은 이치다. */
	pages->type = IOPT_ADDRESS_FILE;	/* [한국어] 원천 종류. */
	return pages;	/* [한국어] 만들어진 객체. */
}

/*
 * [한국어]
 * iopt_revoke_notify - dmabuf 가 무효가 됐다는 알림을 받는다
 *
 * @attach: 무효가 된 붙임.
 *
 * dmabuf 를 내준 쪽(대개 vfio-pci)이 그 메모리를 더는 보장할 수 없게
 * 됐을 때 부른다. 장치가 리셋되거나 사라지는 경우다.
 *
 * 그 순간 이 물리 주소로 가는 매핑을 모두 걷어 내야 한다 — 그러지 않으면
 * 장치가 이미 남의 것이 된 메모리에 DMA 하게 된다.
 *
 * phys.len 을 0 으로 만드는 것이 "무효" 표시다. 이후 이 pages 를 채우려는
 * 시도는 모두 조용히 아무것도 하지 않는다.
 *
 * 실행 컨텍스트: dmabuf 를 내준 드라이버의 문맥. 그래서 pages 뮤텍스를
 * 예약 락 안쪽에서 잡는 순서를 지켜야 한다.
 */
static void iopt_revoke_notify(struct dma_buf_attachment *attach)
{
	struct iopt_pages *pages = attach->importer_priv;	/* [한국어] 붙일 때 넣어 둔 우리 쪽 객체. */
	struct iopt_pages_dmabuf_track *track;	/* [한국어] 매핑해 둔 (영역, 도메인) 짝. */

	guard(mutex)(&pages->mutex);	/* [한국어] 상태를 지키는 뮤텍스. 나갈 때 저절로 풀린다. */
	if (iopt_dmabuf_revoked(pages))	/* [한국어] 이미 무효로 처리했으면 */
		return;	/* [한국어] 두 번 할 일이 없다. */

	list_for_each_entry(track, &pages->dmabuf.tracker, elm) {	/* [한국어] 이 dmabuf 를 매핑한 모든 곳을 */
		struct iopt_area *area = track->area;	/* [한국어] 그 영역과 */

		iopt_area_unmap_domain_range(area, track->domain,	/* [한국어] 도메인에서 걷어 낸다. 그러지 않으면 장치가 이미 남의 것이 된 메모리에 DMA 한다. */
					     iopt_area_index(area),
					     iopt_area_last_index(area));
	}
	pages->dmabuf.phys.len = 0;	/* [한국어] 무효 표시. 이후 이 pages 를 채우려는 시도는 모두 조용히 아무것도 하지 않는다. */
}

/*
 * [한국어] dmabuf 붙임에 등록하는 콜백표.
 *
 * allow_peer2peer 가 참인 것이 중요하다 — 이 매핑의 목적이 장치끼리
 * 직접 주고받는 것(P2P DMA)이라, CPU 를 거치지 않는 물리 주소를 그대로
 * 받아야 한다.
 */
static const struct dma_buf_attach_ops iopt_dmabuf_attach_revoke_ops = {
	.allow_peer2peer = true,	/* [한국어] 장치끼리 직접 주고받는 것이 이 매핑의 목적이라, CPU 를 거치지 않는 물리 주소를 그대로 받겠다고 알린다. */
	.invalidate_mappings = iopt_revoke_notify,
};

/*
 * iommufd and vfio have a circular dependency. Future work for a phys
 * based private interconnect will remove this.
 */
static int	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * sym_vfio_pci_dma_buf_iommufd_map - vfio 의 함수를 심볼로 찾아 부른다
 *
 * @attachment: dmabuf 붙임.
 * @phys: 얻은 물리 구간을 여기에 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 iommufd 와 vfio 가 서로를 참조하는 고리가 있어, 평범하게
 * 링크하면 모듈 의존이 순환한다. symbol_get 으로 실행 시점에 찾는 것이
 * 그 고리를 끊는 방법이다.
 *
 * 먼저 테스트용 함수를 시도하는 이유: 셀프테스트가 vfio 없이도 이 경로를
 * 시험할 수 있어야 한다.
 */
sym_vfio_pci_dma_buf_iommufd_map(struct dma_buf_attachment *attachment,
				 struct phys_vec *phys)
{
	typeof(&vfio_pci_dma_buf_iommufd_map) fn;	/* [한국어] 찾아낼 함수 포인터. typeof 로 서명을 그대로 가져온다. */
	int rc;	/* [한국어] 결과 코드. */

	rc = iommufd_test_dma_buf_iommufd_map(attachment, phys);	/* [한국어] 셀프테스트가 vfio 없이도 이 경로를 시험할 수 있게 먼저 물어본다. */
	if (rc != -EOPNOTSUPP)	/* [한국어] 테스트가 다뤘으면 */
		return rc;	/* [한국어] 그 결과를 쓴다. */

	if (!IS_ENABLED(CONFIG_VFIO_PCI_DMABUF))	/* [한국어] 그 기능이 빌드되지 않았으면 */
		return -EOPNOTSUPP;	/* [한국어] 할 수 없다. */

	fn = symbol_get(vfio_pci_dma_buf_iommufd_map);	/* [한국어] 실행 시점에 심볼을 찾는다. 원 주석대로 이것이 모듈 순환 의존을 끊는 방법이다. */
	if (!fn)	/* [한국어] 모듈이 올라와 있지 않다. */
		return -EOPNOTSUPP;	/* [한국어] 할 수 없다. */
	rc = fn(attachment, phys);	/* [한국어] 물리 구간을 얻는다. */
	symbol_put(vfio_pci_dma_buf_iommufd_map);	/* [한국어] 심볼 참조를 놓는다 — 그래야 그 모듈을 내릴 수 있다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iopt_map_dmabuf - dmabuf 를 붙이고 물리 주소를 얻는다
 *
 * @ictx: 문맥.
 * @pages: 대상 pages 객체.
 * @dmabuf: 붙일 dmabuf.
 * @return: 0 성공, 음수면 실패.
 *
 * 동적 붙임을 쓰는 이유: 내준 쪽이 나중에 무효를 알릴 수 있어야 한다.
 *
 * dma_buf_pin 이 그 메모리를 옮기지 못하게 고정한다. 그래야 물리 주소가
 * 매핑이 살아 있는 동안 유효하다.
 *
 * 원 주석이 밝히는 락 순서가 요점이다 — pages 뮤텍스는 반드시 예약 락
 * 안쪽에서 잡아야 한다. 실제로 잡을 일이 없어도 lockdep 이 그 순서를
 * 배우도록 한 번 잡았다 놓는다.
 */
static int iopt_map_dmabuf(struct iommufd_ctx *ictx, struct iopt_pages *pages,
			   struct dma_buf *dmabuf)
{
	struct dma_buf_attachment *attach;	/* [한국어] 만들 붙임. */
	int rc;	/* [한국어] 결과 코드. */

	attach = dma_buf_dynamic_attach(dmabuf, iommufd_global_device(),	/* [한국어] 동적 붙임이라야 내준 쪽이 나중에 무효를 알릴 수 있다. */
					&iopt_dmabuf_attach_revoke_ops, pages);	/* [한국어] 알림 콜백과, 그때 되짚을 우리 객체를 함께 넘긴다. */
	if (IS_ERR(attach))	/* [한국어] 붙이지 못했다. */
		return PTR_ERR(attach);	/* [한국어] 오류를 올린다. */

	dma_resv_lock(dmabuf->resv, NULL);	/* [한국어] dmabuf 의 예약 락. 아래 연산들이 이것을 요구한다. */
	/*
	 * Lock ordering requires the mutex to be taken inside the reservation,
	 * make sure lockdep sees this.
	 */
	if (IS_ENABLED(CONFIG_LOCKDEP)) {	/* [한국어] 원 주석대로 실제로 잡을 일은 없지만 */
		mutex_lock(&pages->mutex);	/* [한국어] 한 번 잡았다 놓아 */
		mutex_unlock(&pages->mutex);	/* [한국어] lockdep 에게 "예약 락 안쪽에 pages 뮤텍스"라는 순서를 가르친다. */
	}

	rc = dma_buf_pin(attach);	/* [한국어] 그 메모리를 옮기지 못하게 고정한다. 물리 주소가 계속 유효해야 한다. */
	if (rc)	/* [한국어] 고정하지 못했으면 */
		goto err_detach;	/* [한국어] 붙임을 푼다. */

	rc = sym_vfio_pci_dma_buf_iommufd_map(attach, &pages->dmabuf.phys);	/* [한국어] 물리 구간을 얻는다. */
	if (rc)	/* [한국어] 얻지 못했으면 */
		goto err_unpin;	/* [한국어] 고정을 놓고 나간다. */

	dma_resv_unlock(dmabuf->resv);	/* [한국어] 예약 락 해제. */

	/* On success iopt_release_pages() will detach and put the dmabuf. */
	pages->dmabuf.attach = attach;	/* [한국어] 원 주석대로 성공하면 iopt_release_pages 가 이것을 정리한다. */
	return 0;	/* [한국어] 성공. */

err_unpin:	/* [한국어] 물리 구간을 얻지 못한 경로. */
	dma_buf_unpin(attach);	/* [한국어] 고정을 놓는다. */
err_detach:	/* [한국어] 고정 실패도 여기로 합류한다. */
	dma_resv_unlock(dmabuf->resv);	/* [한국어] 예약 락을 놓고 */
	dma_buf_detach(dmabuf, attach);	/* [한국어] 붙임을 푼다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iopt_alloc_dmabuf_pages - dmabuf 를 원천으로 하는 pages 를 만든다
 *
 * @ictx: 문맥.
 * @dmabuf: 원천 dmabuf.
 * @start_byte: 첫 페이지 안에서의 오프셋.
 * @start: dmabuf 안의 시작 오프셋.
 * @length: 길이.
 * @writable: 쓰기 가능한가.
 * @return: 만들어진 객체, 실패하면 오류 포인터.
 *
 * 원 주석이 lockdep 클래스를 나누는 이유를 밝힌다 — 보통의 사용자 메모리
 * 경로는 pages 뮤텍스를 쥔 채 mmap 락을 잡고, dmabuf 경로는 그 반대
 * 순서가 나온다. 두 경로가 같은 pages 객체에서 만나지 않으므로 실제
 * 교착은 없지만, lockdep 에게는 그것을 알려 줘야 한다.
 *
 * dmabuf 는 사용자 메모리가 아니라 장치 메모리라 잠금 회계를 하지 않는다.
 *
 * 길이 상한은 묶음 한 항목이 담을 수 있는 개수(MAX_NPFNS)다 — dmabuf 는
 * 통째로 한 항목이 되기 때문이다.
 */
struct iopt_pages *iopt_alloc_dmabuf_pages(struct iommufd_ctx *ictx,
					   struct dma_buf *dmabuf,
					   unsigned long start_byte,
					   unsigned long start,
					   unsigned long length, bool writable)
{
	static struct lock_class_key pages_dmabuf_mutex_key;	/* [한국어] 이 종류의 pages 뮤텍스만 따로 분류할 열쇠. static 이라 모든 dmabuf pages 가 같은 분류를 쓴다. */
	struct iopt_pages *pages;	/* [한국어] 만들 객체. */
	int rc;	/* [한국어] 결과 코드. */

	if (!IS_ENABLED(CONFIG_DMA_SHARED_BUFFER))	/* [한국어] dmabuf 기능이 빌드되지 않았으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 쓸 수 없다. */

	if (dmabuf->size <= (start + length - 1) ||	/* [한국어] 요청 구간이 dmabuf 를 벗어나거나 */
	    length / PAGE_SIZE >= MAX_NPFNS)	/* [한국어] 한 항목이 담을 수 있는 개수를 넘으면 — dmabuf 는 통째로 한 항목이 된다. */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절. */

	pages = iopt_alloc_pages(start_byte, length, writable);	/* [한국어] 공통 부분. */
	if (IS_ERR(pages))	/* [한국어] 실패하면 */
		return pages;	/* [한국어] 그대로 올린다. */

	/*
	 * The mmap_lock can be held when obtaining the dmabuf reservation lock
	 * which creates a locking cycle with the pages mutex which is held
	 * while obtaining the mmap_lock. This locking path is not present for
	 * IOPT_ADDRESS_DMABUF so split the lock class.
	 */
	lockdep_set_class(&pages->mutex, &pages_dmabuf_mutex_key);	/* [한국어] 원 주석대로 이 경로만 락 순서가 반대라, 분류를 나눠 거짓 경고를 막는다. */

	/* dmabuf does not use pinned page accounting. */
	pages->account_mode = IOPT_PAGES_ACCOUNT_NONE;	/* [한국어] 장치 메모리라 사용자의 잠금 한도와 무관하다. */
	pages->type = IOPT_ADDRESS_DMABUF;	/* [한국어] 원천 종류. */
	pages->dmabuf.start = start - start_byte;	/* [한국어] 페이지 경계에 맞춘 시작 오프셋. */
	INIT_LIST_HEAD(&pages->dmabuf.tracker);	/* [한국어] 매핑해 둔 곳을 기억할 목록. */

	rc = iopt_map_dmabuf(ictx, pages, dmabuf);	/* [한국어] 붙이고 물리 구간을 얻는다. */
	if (rc) {	/* [한국어] 실패하면 */
		iopt_put_pages(pages);	/* [한국어] 만든 객체를 되돌린다. */
		return ERR_PTR(rc);	/* [한국어] 오류를 올린다. */
	}

	return pages;	/* [한국어] 만들어진 객체. */
}

/*
 * [한국어]
 * iopt_dmabuf_track_domain - 이 dmabuf 를 매핑한 (영역, 도메인) 짝을 기록한다
 *
 * @pages: 대상 pages 객체.
 * @area: 매핑한 영역.
 * @domain: 매핑한 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 무효 알림이 왔을 때 어디를 걷어 내야 하는지 알아야 하기 때문에 기록해
 * 둔다. 보통의 사용자 메모리는 domains_itree 로 알 수 있지만, 무효 알림은
 * 그 트리를 안전하게 훑을 수 없는 문맥에서 오므로 별도 목록을 둔다.
 *
 * 같은 짝을 두 번 기록하려는 것은 버그다.
 */
int iopt_dmabuf_track_domain(struct iopt_pages *pages, struct iopt_area *area,
			     struct iommu_domain *domain)
{
	struct iopt_pages_dmabuf_track *track;	/* [한국어] 만들 기록. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 목록을 건드리므로 뮤텍스 아래여야 한다. */
	if (WARN_ON(!iopt_is_dmabuf(pages)))	/* [한국어] dmabuf 가 아닌 것에 부르면 버그다. */
		return -EINVAL;	/* [한국어] 거절. */

	list_for_each_entry(track, &pages->dmabuf.tracker, elm)	/* [한국어] 이미 있는지 본다. */
		if (WARN_ON(track->domain == domain && track->area == area))	/* [한국어] 같은 짝을 두 번 기록하려는 것은 버그다. */
			return -EINVAL;	/* [한국어] 거절. */

	track = kzalloc_obj(*track);	/* [한국어] 기록을 만든다. */
	if (!track)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */
	track->domain = domain;	/* [한국어] 어느 도메인에 */
	track->area = area;	/* [한국어] 어느 영역으로 매핑했는지. */
	list_add_tail(&track->elm, &pages->dmabuf.tracker);	/* [한국어] 목록에 넣는다. 무효 알림이 이것을 훑어 걷어 낸다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iopt_dmabuf_untrack_domain - 그 기록을 지운다
 *
 * @pages: 대상 pages 객체.
 * @area: 그 영역.
 * @domain: 그 도메인.
 *
 * 찾지 못하면 버그다 — 기록과 해제가 짝이 맞지 않았다는 뜻이다.
 */
void iopt_dmabuf_untrack_domain(struct iopt_pages *pages,
				struct iopt_area *area,
				struct iommu_domain *domain)
{
	struct iopt_pages_dmabuf_track *track;	/* [한국어] 찾을 기록. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 뮤텍스 아래여야 한다. */
	WARN_ON(!iopt_is_dmabuf(pages));	/* [한국어] dmabuf 가 아니면 부를 일이 없다. */

	list_for_each_entry(track, &pages->dmabuf.tracker, elm) {	/* [한국어] 목록을 훑으며 */
		if (track->domain == domain && track->area == area) {	/* [한국어] 같은 짝을 찾으면 */
			list_del(&track->elm);	/* [한국어] 목록에서 빼고 */
			kfree(track);	/* [한국어] 해제한다. */
			return;	/* [한국어] 하나만 있으므로 끝이다. */
		}
	}
	WARN_ON(true);	/* [한국어] 찾지 못했다 = 기록과 해제가 짝이 맞지 않았다. */
}

/*
 * [한국어]
 * iopt_dmabuf_track_all_domains - 이 영역이 걸친 모든 도메인을 기록한다
 *
 * @area: 대상 영역.
 * @pages: 그 pages 객체.
 * @return: 0 성공, 음수면 실패.
 *
 * 영역을 만들 때 부른다. 중간에 실패하면 이 영역의 기록을 모두 걷어
 * 아무것도 하지 않은 상태로 되돌린다.
 */
int iopt_dmabuf_track_all_domains(struct iopt_area *area,
				  struct iopt_pages *pages)
{
	struct iopt_pages_dmabuf_track *track;	/* [한국어] 훑어볼 기록. */
	struct iommu_domain *domain;	/* [한국어] 기록할 도메인. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */
	int rc;	/* [한국어] 결과 코드. */

	list_for_each_entry(track, &pages->dmabuf.tracker, elm)	/* [한국어] 이 영역이 이미 기록돼 있는지 본다. */
		if (WARN_ON(track->area == area))	/* [한국어] 있으면 두 번 만드는 것이라 버그다. */
			return -EINVAL;	/* [한국어] 거절. */

	xa_for_each(&area->iopt->domains, index, domain) {	/* [한국어] 이 영역이 걸친 모든 도메인에 대해 */
		rc = iopt_dmabuf_track_domain(pages, area, domain);	/* [한국어] 기록을 만든다. */
		if (rc)	/* [한국어] 실패하면 */
			goto err_untrack;	/* [한국어] 만든 것을 모두 걷는다. */
	}
	return 0;	/* [한국어] 성공. */
err_untrack:	/* [한국어] 실패 경로. */
	iopt_dmabuf_untrack_all_domains(area, pages);	/* [한국어] 이 영역의 기록을 모두 지워 손대기 전 상태로. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iopt_dmabuf_untrack_all_domains - 이 영역의 기록을 모두 지운다
 *
 * @area: 대상 영역.
 * @pages: 그 pages 객체.
 *
 * 영역을 없앨 때, 그리고 위 함수의 실패 되감기에서 쓴다.
 */
void iopt_dmabuf_untrack_all_domains(struct iopt_area *area,
				     struct iopt_pages *pages)
{
	struct iopt_pages_dmabuf_track *track;	/* [한국어] 훑을 기록. */
	struct iopt_pages_dmabuf_track *tmp;	/* [한국어] 지우며 돌기 위한 다음 원소. */

	list_for_each_entry_safe(track, tmp, &pages->dmabuf.tracker,	/* [한국어] 목록을 파괴하며 훑는다. */
				 elm) {
		if (track->area == area) {	/* [한국어] 이 영역의 기록이면 */
			list_del(&track->elm);	/* [한국어] 빼고 */
			kfree(track);	/* [한국어] 해제한다. 다른 영역 것은 그대로 둔다. */
		}
	}
}

/*
 * [한국어]
 * iopt_release_pages - pages 객체의 마지막 참조가 사라졌을 때
 *
 * @kref: 그 참조 카운터.
 *
 * 앞의 네 WARN 이 이 파일의 불변식을 확인한다 — 두 구간 트리가 비어 있고,
 * 고정이 남아 있지 않고, xarray 도 비어 있어야 한다. 하나라도 어긋나면
 * 어딘가에서 정리를 빠뜨린 것이다.
 *
 * 원천 종류에 따라 놓을 것이 다르다. dmabuf 면 붙임을 풀고, 파일이면
 * 파일 참조를 놓는다. 사용자 포인터는 mm 참조만 놓으면 된다.
 */
void iopt_release_pages(struct kref *kref)
{
	struct iopt_pages *pages = container_of(kref, struct iopt_pages, kref);	/* [한국어] 참조 카운터에서 객체를 되짚는다. */

	WARN_ON(!RB_EMPTY_ROOT(&pages->access_itree.rb_root));	/* [한국어] 접근자가 남아 있으면 어딘가에서 놓기를 빠뜨렸다. */
	WARN_ON(!RB_EMPTY_ROOT(&pages->domains_itree.rb_root));	/* [한국어] 도메인이 아직 들고 있으면 마찬가지다. */
	WARN_ON(pages->npinned);	/* [한국어] 고정이 남아 있으면 페이지가 새는 것이다. */
	WARN_ON(!xa_empty(&pages->pinned_pfns));	/* [한국어] xarray 도 비어 있어야 한다. */
	mmdrop(pages->source_mm);	/* [한국어] grab 했던 mm 구조체를 놓는다. */
	mutex_destroy(&pages->mutex);	/* [한국어] 디버그 설정에서 뮤텍스 파괴를 기록한다. */
	put_task_struct(pages->source_task);	/* [한국어] 회계에 쓰던 태스크 참조를 놓는다. */
	free_uid(pages->source_user);	/* [한국어] 사용자 구조체 참조를 놓는다. */
	if (iopt_is_dmabuf(pages) && pages->dmabuf.attach) {	/* [한국어] dmabuf 였고 실제로 붙였으면 */
		struct dma_buf *dmabuf = pages->dmabuf.attach->dmabuf;	/* [한국어] 붙임에서 dmabuf 를 꺼내 둔다 — 아래에서 붙임을 없애기 때문이다. */

		dma_buf_unpin(pages->dmabuf.attach);	/* [한국어] 고정을 놓고 */
		dma_buf_detach(dmabuf, pages->dmabuf.attach);	/* [한국어] 붙임을 풀고 */
		dma_buf_put(dmabuf);	/* [한국어] 참조를 놓는다. */
		WARN_ON(!list_empty(&pages->dmabuf.tracker));	/* [한국어] 매핑 기록이 남아 있으면 어딘가 정리를 빠뜨렸다. */
	} else if (pages->type == IOPT_ADDRESS_FILE) {	/* [한국어] 파일 원천이었으면 */
		fput(pages->file);	/* [한국어] 파일 참조를 놓는다. */
	}
	kfree(pages);	/* [한국어] 객체를 해제한다. */
}

static void	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iopt_area_unpin_domain - 도메인에서 풀면서 고정도 놓는다
 *
 * @batch: 작업용 묶음.
 * @area: 대상 영역.
 * @pages: 그 pages 객체.
 * @domain: 풀 도메인.
 * @start_index: 이번에 다룰 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @unmapped_end_index: 어디까지 풀었는지 기억하는 값. 갱신된다.
 * @real_last_index: 이 영역에서 다룰 진짜 마지막 번호.
 *
 * 이 파일에서 가장 까다로운 함수다. 아래 __iopt_area_unfill_domain 의
 * 원 주석이 그 이유를 설명한다.
 *
 * 요점은 매핑과 해제의 단위가 맞아야 한다는 것이다. 매핑은 이어진 PFN 을
 * 한 번에 걸었으므로, 해제도 그 경계에서 끊어야 한다. 그런데 그 한가운데에
 * 접근자가 붙잡은 페이지가 있으면 고정은 놓을 수 없다.
 *
 * 그래서 "이어짐이 끊길 때까지 더 읽어 그 전체를 풀고, 앞쪽만 고정을
 * 놓는" 방식을 쓴다. 남은 부분은 묶음 앞으로 옮겨(carry) 다음 바퀴로
 * 넘어간다.
 */
iopt_area_unpin_domain(struct pfn_batch *batch, struct iopt_area *area,
		       struct iopt_pages *pages, struct iommu_domain *domain,
		       unsigned long start_index, unsigned long last_index,
		       unsigned long *unmapped_end_index,
		       unsigned long real_last_index)
{
	while (start_index <= last_index) {	/* [한국어] 다룰 범위가 남은 동안. */
		unsigned long batch_last_index;	/* [한국어] 이번 바퀴에서 다룰 마지막 번호. */

		if (*unmapped_end_index <= last_index) {	/* [한국어] 아직 풀지 않은 부분이 남아 있으면 */
			unsigned long start =	/* [한국어] 읽기 시작할 지점. 이미 푼 데까지는 건너뛴다. */
				max(start_index, *unmapped_end_index);	/* [한국어] 둘 중 뒤쪽. */

			if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드 전용 확인. */
			    batch->total_pfns)	/* [한국어] 옮겨 둔 것(carry)이 있으면 */
				WARN_ON(*unmapped_end_index -	/* [한국어] 그 개수만큼 앞선 지점에서 이어져야 앞뒤가 맞는다. */
						batch->total_pfns !=
					start_index);
			batch_from_domain(batch, domain, area, start,	/* [한국어] 도메인에서 PFN 을 되읽는다. 풀려면 어느 물리 페이지였는지 알아야 한다. */
					  last_index);
			batch_last_index = start_index + batch->total_pfns - 1;	/* [한국어] 읽어 온 만큼이 이번에 다룰 범위다. */
		} else {	/* [한국어] 이미 이 범위를 다 풀었으면 */
			batch_last_index = last_index;	/* [한국어] 읽을 것 없이 고정만 놓으면 된다. */
		}

		if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드 전용 확인. */
			WARN_ON(batch_last_index > real_last_index);	/* [한국어] 영역의 끝을 넘으면 안 된다. */

		/*
		 * unmaps must always 'cut' at a place where the pfns are not
		 * contiguous to pair with the maps that always install
		 * contiguous pages. Thus, if we have to stop unpinning in the
		 * middle of the domains we need to keep reading pfns until we
		 * find a cut point to do the unmap. The pfns we read are
		 * carried over and either skipped or integrated into the next
		 * batch.
		 */
		if (batch_last_index == last_index &&	/* [한국어] 이번 범위의 끝까지 왔지만 */
		    last_index != real_last_index)	/* [한국어] 영역의 진짜 끝은 아직이면 */
			batch_from_domain_continue(batch, domain, area,	/* [한국어] 원 주석대로 이어짐이 끊기는 지점까지 더 읽는다. 매핑이 이어진 채로 걸렸으므로 그 경계에서만 풀 수 있다. */
						   last_index + 1,
						   real_last_index);

		if (*unmapped_end_index <= batch_last_index) {	/* [한국어] 아직 풀지 않은 부분이 있으면 */
			iopt_area_unmap_domain_range(	/* [한국어] 그 범위를 도메인에서 푼다. 고정을 놓기 전에 반드시 먼저 풀어야 한다 — 보안 규칙이다. */
				area, domain, *unmapped_end_index,
				start_index + batch->total_pfns - 1);
			*unmapped_end_index = start_index + batch->total_pfns;	/* [한국어] 어디까지 풀었는지 갱신한다. */
		}

		/* unpin must follow unmap */
		batch_unpin(batch, pages, 0,	/* [한국어] 푼 만큼의 앞부분만 고정을 놓는다. */
			    batch_last_index - start_index + 1);
		start_index = batch_last_index + 1;	/* [한국어] 다음 바퀴의 시작. */

		batch_clear_carry(batch,	/* [한국어] 풀었지만 고정을 놓지 않은 나머지를 묶음 앞으로 옮겨 다음 바퀴로 넘긴다. */
				  *unmapped_end_index - batch_last_index - 1);
	}
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * __iopt_area_unfill_domain - 영역을 도메인에서 풀고 고정을 놓는다
 *
 * @area: 대상 영역.
 * @pages: 그 pages 객체.
 * @domain: 풀 도메인.
 * @last_index: 어디까지 풀지(포함).
 *
 * 원 주석의 첫 문단이 보안 규칙을 못 박는다 — 아직 DMA 매핑이 걸려 있는
 * 페이지의 고정을 놓으면 안 된다. 놓은 뒤 그 페이지가 다른 곳에 배정되면
 * 장치가 남의 메모리에 DMA 하게 된다. 그래서 반드시 풀기가 먼저다.
 *
 * 여기서 복잡함이 생긴다. xarray 에 든 페이지(접근자가 붙잡은 것)는 고정을
 * 놓으면 안 되지만 도메인에서는 풀어야 한다. 두 일의 범위가 다른 것이다.
 *
 * dmabuf 는 고정이 없어 그냥 풀고 끝난다.
 *
 * 스택 배열을 보루로 주는 이유: 이 경로는 실패할 수 없다.
 */
static void __iopt_area_unfill_domain(struct iopt_area *area,
				      struct iopt_pages *pages,
				      struct iommu_domain *domain,
				      unsigned long last_index)
{
	struct interval_tree_double_span_iter span;	/* [한국어] 두 트리를 겹쳐 훑을 반복자. */
	unsigned long start_index = iopt_area_index(area);	/* [한국어] 영역의 첫 페이지 번호. */
	unsigned long unmapped_end_index = start_index;	/* [한국어] 어디까지 풀었는지. 아직 아무것도 안 풀었다. */
	u64 backup[BATCH_BACKUP_SIZE];	/* [한국어] 마지막 보루가 될 스택 배열. 이 경로는 실패할 수 없다. */
	struct pfn_batch batch;	/* [한국어] 작업용 묶음. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 두 트리를 보므로 뮤텍스 아래여야 한다. */

	if (iopt_is_dmabuf(pages)) {	/* [한국어] dmabuf 는 고정이 없어 */
		if (WARN_ON(iopt_dmabuf_revoked(pages)))	/* [한국어] 무효가 됐으면 이미 알림 경로가 풀었다. */
			return;	/* [한국어] 두 번 풀지 않는다. */
		iopt_area_unmap_domain_range(area, domain, start_index,	/* [한국어] 그냥 풀고 */
					     last_index);
		return;	/* [한국어] 끝난다. */
	}

	/*
	 * For security we must not unpin something that is still DMA mapped,
	 * so this must unmap any IOVA before we go ahead and unpin the pages.
	 * This creates a complexity where we need to skip over unpinning pages
	 * held in the xarray, but continue to unmap from the domain.
	 *
	 * The domain unmap cannot stop in the middle of a contiguous range of
	 * PFNs. To solve this problem the unpinning step will read ahead to the
	 * end of any contiguous span, unmap that whole span, and then only
	 * unpin the leading part that does not have any accesses. The residual
	 * PFNs that were unmapped but not unpinned are called a "carry" in the
	 * batch as they are moved to the front of the PFN list and continue on
	 * to the next iteration(s).
	 */
	batch_init_backup(&batch, last_index + 1, backup, sizeof(backup));	/* [한국어] 실패할 수 없는 초기화. */
	interval_tree_for_each_double_span(&span, &pages->domains_itree,	/* [한국어] 도메인 트리를 먼저 두는 데 주의 — 여기서는 is_used 를 쓰지 않고 구멍만 보므로 순서가 결과를 바꾸지 않는다. */
					   &pages->access_itree, start_index,
					   last_index) {
		if (span.is_used) {	/* [한국어] 접근자나 다른 도메인이 붙잡고 있으면 */
			batch_skip_carry(&batch,	/* [한국어] 고정을 놓을 수 없다. 옮겨 둔 것에서 그만큼을 건너뛴다. */
					 span.last_used - span.start_used + 1);
			continue;	/* [한국어] 다음 구간으로. */
		}
		iopt_area_unpin_domain(&batch, area, pages, domain,	/* [한국어] 아무도 안 붙잡은 구간만 풀고 고정도 놓는다. */
				       span.start_hole, span.last_hole,
				       &unmapped_end_index, last_index);
	}
	/*
	 * If the range ends in a access then we do the residual unmap without
	 * any unpins.
	 */
	if (unmapped_end_index != last_index + 1)	/* [한국어] 원 주석대로 끝이 접근자로 끝나면 */
		iopt_area_unmap_domain_range(area, domain, unmapped_end_index,	/* [한국어] 남은 부분은 고정을 놓지 않고 풀기만 한다. */
					     last_index);
	WARN_ON(batch.total_pfns);	/* [한국어] 옮겨 둔 것이 남아 있으면 어딘가에서 처리를 빠뜨렸다. */
	batch_destroy(&batch, backup);	/* [한국어] 묶음 정리. 스택 배열이면 해제하지 않는다. */
	update_unpinned(pages);	/* [한국어] 놓은 만큼을 회계에 되돌려 준다. */
}

/*
 * [한국어]
 * iopt_area_unfill_partial_domain - 앞쪽 일부만 풀고 고정을 놓는다
 *
 * @area: 대상 영역.
 * @pages: 그 pages 객체.
 * @domain: 풀 도메인.
 * @end_index: 여기 앞까지 푼다(포함하지 않음).
 *
 * 채우다 중간에 실패했을 때 되돌리는 데 쓴다. 채운 데까지만 풀어야 한다.
 *
 * 시작과 같으면 아무것도 채우지 못한 것이라 할 일이 없다.
 */
static void iopt_area_unfill_partial_domain(struct iopt_area *area,
					    struct iopt_pages *pages,
					    struct iommu_domain *domain,
					    unsigned long end_index)
{
	if (end_index != iopt_area_index(area))	/* [한국어] 한 페이지라도 채웠으면 */
		__iopt_area_unfill_domain(area, pages, domain, end_index - 1);	/* [한국어] 그 앞까지 되돌린다. end 는 포함하지 않는 경계라 하나를 뺀다. */
}

/**
 * iopt_area_unmap_domain() - Unmap without unpinning PFNs in a domain
 * @area: The IOVA range to unmap
 * @domain: The domain to unmap
 *
 * The caller must know that unpinning is not required, usually because there
 * are other domains in the iopt.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_unmap_domain - 고정은 그대로 두고 매핑만 푼다
 *
 * @area: 풀 IOVA 범위.
 * @domain: 그 도메인.
 *
 * 원 주석대로 호출자가 "고정을 놓을 필요가 없다"는 것을 알고 있어야 한다.
 * 대개 다른 도메인이 아직 같은 PFN 을 들고 있기 때문이다.
 */
void iopt_area_unmap_domain(struct iopt_area *area, struct iommu_domain *domain)
{
	iommu_unmap_nofail(domain, iopt_area_iova(area),	/* [한국어] 영역 전체를 한 번에 푼다. 고정은 그대로 둔다 — 다른 도메인이 아직 들고 있기 때문이다. */
			   iopt_area_length(area));
}

/**
 * iopt_area_unfill_domain() - Unmap and unpin PFNs in a domain
 * @area: IOVA area to use
 * @pages: page supplier for the area (area->pages is NULL)
 * @domain: Domain to unmap from
 *
 * The domain should be removed from the domains_itree before calling. The
 * domain will always be unmapped, but the PFNs may not be unpinned if there are
 * still accesses.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_unfill_domain - 도메인에서 풀고 필요하면 고정도 놓는다
 *
 * @area: 대상 IOVA 영역.
 * @pages: 그 영역의 PFN 공급자.
 * @domain: 풀 도메인.
 *
 * 원 주석대로 부르기 전에 domains_itree 에서 빼 두어야 한다 — 그러지
 * 않으면 "아직 도메인이 들고 있다"고 판정되어 고정을 놓지 못한다.
 *
 * 무효가 된 dmabuf 는 이미 알림 경로가 풀어 두었으므로 할 일이 없다.
 */
void iopt_area_unfill_domain(struct iopt_area *area, struct iopt_pages *pages,
			     struct iommu_domain *domain)
{
	if (iopt_dmabuf_revoked(pages))	/* [한국어] 무효가 된 dmabuf 는 알림 경로가 이미 풀었다. */
		return;	/* [한국어] 할 일이 없다. */

	__iopt_area_unfill_domain(area, pages, domain,	/* [한국어] 영역 전체를 풀고 고정도 놓는다. */
				  iopt_area_last_index(area));
}

/**
 * iopt_area_fill_domain() - Map PFNs from the area into a domain
 * @area: IOVA area to use
 * @domain: Domain to load PFNs into
 *
 * Read the pfns from the area's underlying iopt_pages and map them into the
 * given domain. Called when attaching a new domain to an io_pagetable.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_fill_domain - 새 도메인 하나에 이 영역을 채운다
 *
 * @area: 대상 영역.
 * @domain: 채울 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * io_pagetable 에 도메인을 새로 붙일 때, 이미 있던 영역들을 그 도메인에도
 * 채워 넣어야 한다. 그 한 영역분이 이 함수다.
 *
 * 반복자로 묶음을 받아 도메인에 매핑하기를 되풀이한다. 실패하면 채운
 * 데까지 되돌린다.
 *
 * done_end_index 를 매핑 전후로 두 번 갱신하는 것에 주의 — 매핑 중에
 * 실패하면 그 묶음은 부분적으로 걸렸을 수 있어, 그 묶음의 시작까지만
 * 되돌린 것으로 잡아야 안전하다.
 */
int iopt_area_fill_domain(struct iopt_area *area, struct iommu_domain *domain)
{
	unsigned long done_end_index;	/* [한국어] 실패했을 때 되돌릴 지점. */
	struct pfn_reader pfns;	/* [한국어] PFN 을 꺼내 올 반복자. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&area->pages->mutex);	/* [한국어] 반복자를 쓰므로 뮤텍스 아래여야 한다. */

	if (iopt_dmabuf_revoked(area->pages))	/* [한국어] 무효가 된 dmabuf 는 채울 것이 없다. */
		return 0;	/* [한국어] 성공으로 친다 — 오류가 아니라 빈 상태가 정상이다. */

	rc = pfn_reader_first(&pfns, area->pages, iopt_area_index(area),	/* [한국어] 영역 전체를 읽을 반복자를 세운다. */
			      iopt_area_last_index(area));
	if (rc)	/* [한국어] 실패하면 */
		return rc;	/* [한국어] 그대로 올린다. */

	while (!pfn_reader_done(&pfns)) {	/* [한국어] 다 읽을 때까지. */
		done_end_index = pfns.batch_start_index;	/* [한국어] 매핑하기 전 지점 — 이 묶음이 부분적으로 걸릴 수 있어 여기까지만 성공으로 잡는다. */
		rc = batch_to_domain(&pfns.batch, domain, area,	/* [한국어] 이 묶음을 도메인에 건다. */
				     pfns.batch_start_index);
		if (rc)	/* [한국어] 묶음을 도메인에 거는 데 실패했다. */
			goto out_unmap;	/* [한국어] 실패하면 되돌린다. */
		done_end_index = pfns.batch_end_index;	/* [한국어] 성공했으니 여기까지 걸렸다. */

		rc = pfn_reader_next(&pfns);	/* [한국어] 다음 묶음. */
		if (rc)	/* [한국어] 다음 묶음을 읽는 데 실패했다. */
			goto out_unmap;	/* [한국어] 실패하면 되돌린다. */
	}

	rc = pfn_reader_update_pinned(&pfns);	/* [한국어] 한도를 확인하며 회계를 반영한다. */
	if (rc)	/* [한국어] 회계 반영에 실패했다(한도 초과). */
		goto out_unmap;	/* [한국어] 한도를 넘었으면 되돌린다. */
	goto out_destroy;	/* [한국어] 성공 경로. */

out_unmap:	/* [한국어] 실패 경로. */
	pfn_reader_release_pins(&pfns);	/* [한국어] 반복자가 든 고정을 먼저 놓는다 — 원 주석대로 이 순서가 아니면 단언이 어긋난다. */
	iopt_area_unfill_partial_domain(area, area->pages, domain,	/* [한국어] 건 만큼만 되돌린다. */
					done_end_index);
out_destroy:	/* [한국어] 성공과 실패가 합류한다. */
	pfn_reader_destroy(&pfns);	/* [한국어] 반복자를 정리한다. */
	return rc;	/* [한국어] 결과. */
}

/**
 * iopt_area_fill_domains() - Install PFNs into the area's domains
 * @area: The area to act on
 * @pages: The pages associated with the area (area->pages is NULL)
 *
 * Called during area creation. The area is freshly created and not inserted in
 * the domains_itree yet. PFNs are read and loaded into every domain held in the
 * area's io_pagetable and the area is installed in the domains_itree.
 *
 * On failure all domains are left unchanged.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_fill_domains - 새 영역을 모든 도메인에 채운다
 *
 * @area: 새로 만든 영역.
 * @pages: 그 영역의 PFN 공급자.
 * @return: 0 성공, 음수면 실패.
 *
 * 영역을 만들 때 부른다. 위 함수와 방향이 반대다 — 저쪽은 도메인 하나에
 * 영역들을, 이쪽은 영역 하나를 도메인들에 채운다.
 *
 * 원 주석대로 실패하면 모든 도메인이 손대기 전 상태로 남는다.
 *
 * 되돌리기가 까다롭다. 한 묶음을 여러 도메인에 거는 도중 실패하면,
 * 앞선 도메인들은 그 묶음까지 걸렸고 뒤의 도메인들은 그 앞까지만 걸렸다.
 * 그래서 도메인 번호를 실패 지점과 견줘 각자 얼마나 되돌릴지 정한다.
 *
 * 원 주석이 밝히듯 이 영역은 아직 domains_itree 에 없어, 고정 놓기를
 * 손수 다뤄야 한다 — 마지막 도메인만 고정을 놓고 나머지는 풀기만 한다.
 *
 * 성공하면 storage_domain 을 정하고 트리에 넣는다. 그 순간부터 이 영역이
 * "도메인이 들고 있는 구간"으로 보인다.
 */
int iopt_area_fill_domains(struct iopt_area *area, struct iopt_pages *pages)
{
	unsigned long done_first_end_index;	/* [한국어] 첫 도메인들이 걸린 끝 지점. */
	unsigned long done_all_end_index;	/* [한국어] 모든 도메인이 함께 걸린 끝 지점. */
	struct iommu_domain *domain;	/* [한국어] 훑을 도메인. */
	unsigned long unmap_index;	/* [한국어] 되돌릴 때의 도메인 첨자. */
	struct pfn_reader pfns;	/* [한국어] PFN 반복자. */
	unsigned long index;	/* [한국어] 채울 때의 도메인 첨자. 실패 지점을 기억하는 데도 쓰인다. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&area->iopt->domains_rwsem);	/* [한국어] 도메인 목록이 바뀌지 않아야 한다. */

	if (xa_empty(&area->iopt->domains))	/* [한국어] 도메인이 하나도 없으면 */
		return 0;	/* [한국어] 채울 곳이 없다. */

	mutex_lock(&pages->mutex);	/* [한국어] PFN 상태를 지키는 뮤텍스. */
	if (iopt_is_dmabuf(pages)) {	/* [한국어] dmabuf 면 */
		rc = iopt_dmabuf_track_all_domains(area, pages);	/* [한국어] 무효 알림 때 걷어 낼 곳을 미리 기록해 둔다. */
		if (rc)	/* [한국어] dmabuf 매핑 기록에 실패했다. */
			goto out_unlock;	/* [한국어] 실패하면 나간다. */
	}

	if (!iopt_dmabuf_revoked(pages)) {	/* [한국어] 무효가 아니면 실제로 채운다. */
		rc = pfn_reader_first(&pfns, pages, iopt_area_index(area),	/* [한국어] 영역 전체를 읽을 반복자. */
				      iopt_area_last_index(area));
		if (rc)	/* [한국어] 반복자를 세우는 데 실패했다. */
			goto out_untrack;	/* [한국어] 실패하면 기록을 걷고 나간다. */

		while (!pfn_reader_done(&pfns)) {	/* [한국어] 다 읽을 때까지. */
			done_first_end_index = pfns.batch_end_index;	/* [한국어] 이 묶음까지 걸리게 될 끝. */
			done_all_end_index = pfns.batch_start_index;	/* [한국어] 모든 도메인이 함께 걸린 것은 아직 앞 묶음까지다. */
			xa_for_each(&area->iopt->domains, index, domain) {	/* [한국어] 한 묶음을 모든 도메인에. */
				rc = batch_to_domain(&pfns.batch, domain, area,	/* [한국어] 건다. */
						     pfns.batch_start_index);
				if (rc)	/* [한국어] 어느 도메인에 거는 데 실패했다. */
					goto out_unmap;	/* [한국어] 실패하면 index 가 어디서 멈췄는지 기억된 채 되돌리기로 간다. */
			}
			done_all_end_index = done_first_end_index;	/* [한국어] 모든 도메인이 이 묶음까지 걸렸다. */

			rc = pfn_reader_next(&pfns);	/* [한국어] 다음 묶음. */
			if (rc)	/* [한국어] 다음 묶음을 읽는 데 실패했다. */
				goto out_unmap;	/* [한국어] 실패하면 되돌린다. */
		}
		rc = pfn_reader_update_pinned(&pfns);	/* [한국어] 회계를 반영한다. */
		if (rc)	/* [한국어] 회계 반영에 실패했다. */
			goto out_unmap;	/* [한국어] 한도를 넘었으면 되돌린다. */

		pfn_reader_destroy(&pfns);	/* [한국어] 반복자를 정리한다. */
	}

	area->storage_domain = xa_load(&area->iopt->domains, 0);	/* [한국어] PFN 을 되읽을 때 쓸 도메인을 정한다. 첫 번째면 된다 — 모두 같은 내용이다. */
	interval_tree_insert(&area->pages_node, &pages->domains_itree);	/* [한국어] 이 순간부터 이 구간이 "도메인이 들고 있다"고 보인다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스 해제. */
	return 0;	/* [한국어] 성공. */

out_unmap:	/* [한국어] 매핑 도중 실패한 경로. */
	pfn_reader_release_pins(&pfns);	/* [한국어] 반복자가 든 고정을 먼저 놓는다. */
	xa_for_each(&area->iopt->domains, unmap_index, domain) {	/* [한국어] 모든 도메인을 다시 훑으며 */
		unsigned long end_index;	/* [한국어] 이 도메인이 어디까지 걸렸는지. */

		if (unmap_index < index)	/* [한국어] 실패 지점보다 앞선 도메인은 */
			end_index = done_first_end_index;	/* [한국어] 마지막 묶음까지 걸렸고 */
		else
			end_index = done_all_end_index;	/* [한국어] 그 뒤 도메인은 그 앞 묶음까지만 걸렸다. */

		/*
		 * The area is not yet part of the domains_itree so we have to
		 * manage the unpinning specially. The last domain does the
		 * unpin, every other domain is just unmapped.
		 */
		if (unmap_index != area->iopt->next_domain_id - 1) {	/* [한국어] 원 주석대로 마지막 도메인이 아니면 */
			if (end_index != iopt_area_index(area))	/* [한국어] 걸린 것이 있을 때만 */
				iopt_area_unmap_domain_range(	/* [한국어] 풀기만 한다. 고정은 마지막 도메인이 놓는다. */
					area, domain, iopt_area_index(area),
					end_index - 1);
		} else {	/* [한국어] 마지막 도메인이면 */
			iopt_area_unfill_partial_domain(area, pages, domain,	/* [한국어] 풀면서 고정도 놓는다. */
							end_index);
		}
	}
	pfn_reader_destroy(&pfns);	/* [한국어] 반복자를 정리한다. */
out_untrack:	/* [한국어] dmabuf 기록을 걷어야 하는 경로. */
	if (iopt_is_dmabuf(pages))	/* [한국어] dmabuf 였으면 */
		iopt_dmabuf_untrack_all_domains(area, pages);	/* [한국어] 기록을 모두 지운다. */
out_unlock:	/* [한국어] 모든 실패가 합류한다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스를 놓고 */
	return rc;	/* [한국어] 실패를 올린다. */
}

/**
 * iopt_area_unfill_domains() - unmap PFNs from the area's domains
 * @area: The area to act on
 * @pages: The pages associated with the area (area->pages is NULL)
 *
 * Called during area destruction. This unmaps the iova's covered by all the
 * area's domains and releases the PFNs.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_unfill_domains - 영역을 모든 도메인에서 걷어 낸다
 *
 * @area: 없앨 영역.
 * @pages: 그 PFN 공급자.
 *
 * storage_domain 을 마지막에 다루는 것이 요점이다. 다른 도메인들은 풀기만
 * 하고, 저장 자리로 쓰이던 그 도메인에서 풀 때 고정까지 놓는다 — PFN 을
 * 되읽으려면 그 도메인이 살아 있어야 하기 때문이다.
 *
 * 트리에서 먼저 빼는 것도 같은 이유다. 빼지 않으면 "아직 도메인이
 * 들고 있다"고 판정되어 고정을 놓지 못한다.
 */
void iopt_area_unfill_domains(struct iopt_area *area, struct iopt_pages *pages)
{
	struct io_pagetable *iopt = area->iopt;	/* [한국어] 도메인 목록을 든 구조. */
	struct iommu_domain *domain;	/* [한국어] 훑을 도메인. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */

	lockdep_assert_held(&iopt->domains_rwsem);	/* [한국어] 도메인 목록이 바뀌지 않아야 한다. */

	mutex_lock(&pages->mutex);	/* [한국어] PFN 상태를 지키는 뮤텍스. */
	if (!area->storage_domain)	/* [한국어] 채워진 적이 없으면 */
		goto out_unlock;	/* [한국어] 걷어 낼 것도 없다. */

	xa_for_each(&iopt->domains, index, domain) {	/* [한국어] 모든 도메인을 훑으며 */
		if (domain == area->storage_domain)	/* [한국어] 저장 자리로 쓰이는 것은 */
			continue;	/* [한국어] 마지막에 따로 다룬다 — PFN 을 되읽으려면 그것이 살아 있어야 한다. */

		if (!iopt_dmabuf_revoked(pages))	/* [한국어] 무효가 된 dmabuf 가 아니면 */
			iopt_area_unmap_domain_range(	/* [한국어] 풀기만 한다. 고정은 아직 놓지 않는다. */
				area, domain, iopt_area_index(area),
				iopt_area_last_index(area));
	}

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 테스트 빌드 전용 확인. */
		WARN_ON(RB_EMPTY_NODE(&area->pages_node.rb));	/* [한국어] 트리에 들어 있어야 한다 — 채웠다면 넣었을 것이다. */
	interval_tree_remove(&area->pages_node, &pages->domains_itree);	/* [한국어] 먼저 빼야 아래에서 고정을 놓을 수 있다. 트리에 있으면 "도메인이 들고 있다"고 판정된다. */
	iopt_area_unfill_domain(area, pages, area->storage_domain);	/* [한국어] 저장 도메인에서 풀면서 고정도 놓는다. */
	if (iopt_is_dmabuf(pages))	/* [한국어] dmabuf 였으면 */
		iopt_dmabuf_untrack_all_domains(area, pages);	/* [한국어] 매핑 기록도 지운다. */
	area->storage_domain = NULL;	/* [한국어] 더는 어디에도 저장돼 있지 않다. */
out_unlock:	/* [한국어] 두 경로가 합류한다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스 해제. */
}

/*
 * [한국어]
 * iopt_pages_unpin_xarray - xarray 구간을 비우며 고정을 놓는다
 *
 * @batch: 작업용 묶음.
 * @pages: 대상 pages 객체.
 * @start_index: 첫 번호.
 * @end_index: 마지막 번호(포함).
 *
 * 묶음이 한 번에 담을 수 있는 만큼씩 나눠 처리한다. 읽으면서 지우고,
 * 지운 것의 고정을 놓는 순환이다.
 */
static void iopt_pages_unpin_xarray(struct pfn_batch *batch,
				    struct iopt_pages *pages,
				    unsigned long start_index,
				    unsigned long end_index)
{
	while (start_index <= end_index) {	/* [한국어] 다룰 범위가 남은 동안. */
		batch_from_xarray_clear(batch, &pages->pinned_pfns, start_index,	/* [한국어] 읽으면서 지운다. 묶음이 담을 수 있는 만큼만 온다. */
					end_index);
		batch_unpin(batch, pages, 0, batch->total_pfns);	/* [한국어] 담긴 것의 고정을 모두 놓는다. */
		start_index += batch->total_pfns;	/* [한국어] 진행한 만큼 앞으로. */
		batch_clear(batch);	/* [한국어] 묶음을 비우고 다음 바퀴로. */
	}
}

/**
 * iopt_pages_unfill_xarray() - Update the xarry after removing an access
 * @pages: The pages to act on
 * @start_index: Starting PFN index
 * @last_index: Last PFN index
 *
 * Called when an iopt_pages_access is removed, removes pages from the itree.
 * The access should already be removed from the access_itree.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_unfill_xarray - 접근자가 떠난 뒤 xarray 층을 정리한다
 *
 * @pages: 대상 pages 객체.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 *
 * 구간마다 처리가 다르다. 아무도 안 쓰면 지우고 고정도 놓는다. 도메인이
 * 들고 있으면 xarray 항목만 지운다 — PFN 은 도메인에서 되읽을 수 있으므로
 * 중복 저장할 이유가 없다. 다른 접근자가 쓰고 있으면 그대로 둔다.
 *
 * 묶음을 늦게 초기화하는 이유: 놓을 것이 하나도 없으면 아예 만들지
 * 않는다. 흔한 경우이고, 그 할당을 아끼려는 것이다.
 */
void iopt_pages_unfill_xarray(struct iopt_pages *pages,
			      unsigned long start_index,
			      unsigned long last_index)
{
	struct interval_tree_double_span_iter span;	/* [한국어] 두 트리를 겹쳐 훑을 반복자. */
	u64 backup[BATCH_BACKUP_SIZE];	/* [한국어] 마지막 보루가 될 스택 배열. */
	struct pfn_batch batch;	/* [한국어] 작업용 묶음. */
	bool batch_inited = false;	/* [한국어] 묶음을 실제로 만들었는지 — 놓을 것이 없으면 만들지 않는다. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 두 트리와 xarray 를 함께 다루므로 뮤텍스 아래여야 한다. */

	interval_tree_for_each_double_span(&span, &pages->access_itree,	/* [한국어] 접근자 트리를 먼저 두어 is_used 1 이 xarray 를 뜻하게 한다. */
					   &pages->domains_itree, start_index,
					   last_index) {
		if (!span.is_used) {	/* [한국어] 아무도 안 쓰는 구간이면 */
			if (!batch_inited) {	/* [한국어] 묶음이 아직 없으면 */
				batch_init_backup(&batch,	/* [한국어] 여기서 만든다. 흔히 놓을 것이 없어 이 할당을 아끼려는 것이다. */
						  last_index - start_index + 1,
						  backup, sizeof(backup));
				batch_inited = true;	/* [한국어] 만들었음을 표시. */
			}
			iopt_pages_unpin_xarray(&batch, pages, span.start_hole,	/* [한국어] xarray 를 비우며 고정을 놓는다. */
						span.last_hole);
		} else if (span.is_used == 2) {	/* [한국어] 도메인이 들고 있는 구간이면 */
			/* Covered by a domain */
			clear_xarray(&pages->pinned_pfns, span.start_used,	/* [한국어] xarray 항목만 지운다. PFN 은 도메인에서 되읽을 수 있어 중복 저장할 이유가 없다. */
				     span.last_used);
		}
		/* Otherwise covered by an existing access */
	}
	if (batch_inited)	/* [한국어] 묶음을 만들었으면 */
		batch_destroy(&batch, backup);	/* [한국어] 정리한다. */
	update_unpinned(pages);	/* [한국어] 놓은 만큼을 회계에 되돌려 준다. */
}

/**
 * iopt_pages_fill_from_xarray() - Fast path for reading PFNs
 * @pages: The pages to act on
 * @start_index: The first page index in the range
 * @last_index: The last page index in the range
 * @out_pages: The output array to return the pages
 *
 * This can be called if the caller is holding a refcount on an
 * iopt_pages_access that is known to have already been filled. It quickly reads
 * the pages directly from the xarray.
 *
 * This is part of the SW iommu interface to read pages for in-kernel use.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_fill_from_xarray - xarray 에서 페이지를 빠르게 읽어 온다
 *
 * @pages: 대상 pages 객체.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 * @out_pages: 채울 페이지 배열.
 *
 * 원 주석대로 이미 채워진 접근자를 붙잡고 있을 때만 쓸 수 있다. 그
 * 조건에서는 모든 항목이 있음이 보장되므로 검사 없이 읽어 간다.
 *
 * 실패할 수 없어 반환값이 없다.
 */
void iopt_pages_fill_from_xarray(struct iopt_pages *pages,
				 unsigned long start_index,
				 unsigned long last_index,
				 struct page **out_pages)
{
	XA_STATE(xas, &pages->pinned_pfns, start_index);	/* [한국어] 훑기 상태 변수. */
	void *entry;	/* [한국어] 읽어 온 항목. */

	rcu_read_lock();	/* [한국어] 값 하나하나는 온전히 보이므로 락 없이 읽는다. */
	while (start_index <= last_index) {	/* [한국어] 범위 끝까지. */
		entry = xas_next(&xas);	/* [한국어] 다음 항목. */
		if (xas_retry(&xas, entry))	/* [한국어] 트리가 재구성됐으면 */
			continue;	/* [한국어] 다시 읽는다. */
		WARN_ON(!xa_is_value(entry));	/* [한국어] 호출 조건대로 모든 항목이 채워져 있어야 한다. */
		*(out_pages++) = pfn_to_page(xa_to_value(entry));	/* [한국어] PFN 값을 page 포인터로 바꿔 담는다. */
		start_index++;	/* [한국어] 다음 번호. */
	}
	rcu_read_unlock();	/* [한국어] 읽기를 마쳤다. */
}

/*
 * [한국어]
 * iopt_pages_fill_from_domain - 도메인에서 페이지를 읽어 온다
 *
 * @pages: 대상 pages 객체.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 * @out_pages: 채울 페이지 배열.
 * @return: 0 성공, 음수면 실패.
 *
 * 구간이 여러 영역에 걸칠 수 있어, 영역을 찾아 가며 그 영역이 덮는
 * 만큼씩 읽는다.
 */
static int iopt_pages_fill_from_domain(struct iopt_pages *pages,
				       unsigned long start_index,
				       unsigned long last_index,
				       struct page **out_pages)
{
	while (start_index != last_index + 1) {	/* [한국어] 범위를 다 덮을 때까지. */
		unsigned long domain_last;	/* [한국어] 이 영역이 덮는 끝. */
		struct iopt_area *area;	/* [한국어] 그 영역. */

		area = iopt_pages_find_domain_area(pages, start_index);	/* [한국어] 이 번호를 들고 있는 영역을 찾는다. */
		if (WARN_ON(!area))	/* [한국어] 트리가 덮는다고 했는데 없으면 앞뒤가 맞지 않는다. */
			return -EINVAL;	/* [한국어] 실패. */

		domain_last = min(iopt_area_last_index(area), last_index);	/* [한국어] 영역의 끝과 요청의 끝 중 앞선 쪽. */
		out_pages = raw_pages_from_domain(area->storage_domain, area,	/* [한국어] 그 영역이 덮는 만큼 읽어 배열에 담고 다음 자리를 받는다. */
						  start_index, domain_last,
						  out_pages);
		start_index = domain_last + 1;	/* [한국어] 다음 영역으로. */
	}
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iopt_pages_fill - 원천에서 페이지를 고정해 배열을 채운다
 *
 * @pages: 대상 pages 객체.
 * @user: 원천 읽기 상태.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 * @out_pages: 채울 페이지 배열.
 * @return: 0 성공, 음수면 실패.
 *
 * 출력 배열을 그대로 고정 버퍼로 쓰는 것이 요령이다 — 따로 잡았다 옮길
 * 이유가 없다.
 *
 * 한 번에 다 못 하면 이어서 부른다. 실패하면 지금까지 고정한 것을 모두
 * 놓는다.
 */
static int iopt_pages_fill(struct iopt_pages *pages,
			   struct pfn_reader_user *user,
			   unsigned long start_index,
			   unsigned long last_index,
			   struct page **out_pages)
{
	unsigned long cur_index = start_index;	/* [한국어] 지금까지 채운 지점. */
	int rc;	/* [한국어] 결과 코드. */

	while (cur_index != last_index + 1) {	/* [한국어] 범위를 다 채울 때까지. */
		user->upages = out_pages + (cur_index - start_index);	/* [한국어] 출력 배열을 그대로 고정 버퍼로 쓴다 — 따로 잡았다 옮길 이유가 없다. */
		rc = pfn_reader_user_pin(user, pages, cur_index, last_index);	/* [한국어] 원천에서 고정해 온다. 한 번에 다 못 할 수 있다. */
		if (rc)	/* [한국어] 실패하면 */
			goto out_unpin;	/* [한국어] 지금까지 고정한 것을 놓는다. */
		cur_index = user->upages_end;	/* [한국어] 고정한 만큼 진행. */
	}
	return 0;	/* [한국어] 성공. */

out_unpin:	/* [한국어] 실패 경로. */
	if (start_index != cur_index)	/* [한국어] 하나라도 고정했으면 */
		iopt_pages_err_unpin(pages, start_index, cur_index - 1,	/* [한국어] 모두 놓고 회계도 되돌린다. */
				     out_pages);
	return rc;	/* [한국어] 실패를 올린다. */
}

/**
 * iopt_pages_fill_xarray() - Read PFNs
 * @pages: The pages to act on
 * @start_index: The first page index in the range
 * @last_index: The last page index in the range
 * @out_pages: The output array to return the pages, may be NULL
 *
 * This populates the xarray and returns the pages in out_pages. As the slow
 * path this is able to copy pages from other storage tiers into the xarray.
 *
 * On failure the xarray is left unchanged.
 *
 * This is part of the SW iommu interface to read pages for in-kernel use.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_fill_xarray - 커널 접근자를 위해 xarray 층을 채운다
 *
 * @pages: 대상 pages 객체.
 * @start_index: 첫 번호.
 * @last_index: 마지막 번호(포함).
 * @out_pages: 채울 페이지 배열.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 느린 경로다. 다른 층에 있는 PFN 을 xarray 로 복사해 오는
 * 일까지 한다.
 *
 * 구간마다 출처가 다르다. 이미 xarray 에 있으면 그대로 읽고, 도메인에
 * 있으면 되읽어 xarray 에도 넣고, 아무 데도 없으면 원천에서 고정한다.
 *
 * 도메인에 있는 것을 xarray 에 복사하는 이유가 이 파일의 원칙과 이어진다 —
 * 나중에 도메인이 사라질 때 이 접근자가 붙잡은 PFN 을 되읽을 수 없게
 * 되면 안 되고, 그때는 할당할 여유가 없을 수 있다.
 *
 * 원 주석대로 실패하면 xarray 는 손대기 전 상태로 남는다.
 */
int iopt_pages_fill_xarray(struct iopt_pages *pages, unsigned long start_index,
			   unsigned long last_index, struct page **out_pages)
{
	struct interval_tree_double_span_iter span;	/* [한국어] 두 트리를 겹쳐 훑을 반복자. */
	unsigned long xa_end = start_index;	/* [한국어] xarray 에 채워 넣은 끝. 실패 시 되돌릴 범위다. */
	struct pfn_reader_user user;	/* [한국어] 원천에서 고정해 올 때 쓸 상태. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 뮤텍스 아래여야 한다. */

	pfn_reader_user_init(&user, pages);	/* [한국어] 원천 읽기 상태를 세운다. */
	user.upages_len = (last_index - start_index + 1) * sizeof(*out_pages);	/* [한국어] 출력 배열을 고정 버퍼로 쓸 것이므로 그 크기를 알려 둔다. */
	interval_tree_for_each_double_span(&span, &pages->access_itree,	/* [한국어] 구간마다 출처가 다르다. */
					   &pages->domains_itree, start_index,
					   last_index) {
		struct page **cur_pages;	/* [한국어] 이 구간이 채울 배열 위치. */

		if (span.is_used == 1) {	/* [한국어] 이미 xarray 에 있으면 */
			cur_pages = out_pages + (span.start_used - start_index);	/* [한국어] 해당 자리를 계산해 */
			iopt_pages_fill_from_xarray(pages, span.start_used,	/* [한국어] 그대로 읽어 온다. */
						    span.last_used, cur_pages);
			continue;	/* [한국어] 다음 구간. */
		}

		if (span.is_used == 2) {	/* [한국어] 도메인이 들고 있으면 */
			cur_pages = out_pages + (span.start_used - start_index);	/* [한국어] 자리를 계산해 */
			iopt_pages_fill_from_domain(pages, span.start_used,	/* [한국어] 되읽어 배열에 담고 */
						    span.last_used, cur_pages);
			rc = pages_to_xarray(&pages->pinned_pfns,	/* [한국어] xarray 에도 넣는다. 나중에 도메인이 사라질 때 되읽을 수 없게 되면 안 되고, 그때는 할당할 여유가 없을 수 있다. */
					     span.start_used, span.last_used,
					     cur_pages);
			if (rc)	/* [한국어] xarray 에 넣지 못했다. */
				goto out_clean_xa;	/* [한국어] 실패하면 지금까지 채운 xarray 를 되돌린다. */
			xa_end = span.last_used + 1;	/* [한국어] 채운 끝을 갱신. */
			continue;	/* [한국어] 다음 구간. */
		}

		/* hole */
		cur_pages = out_pages + (span.start_hole - start_index);	/* [한국어] 아무 데도 없는 구간. 자리를 계산해 */
		rc = iopt_pages_fill(pages, &user, span.start_hole,	/* [한국어] 원천에서 고정해 온다. */
				     span.last_hole, cur_pages);
		if (rc)	/* [한국어] 원천에서 고정해 오지 못했다. */
			goto out_clean_xa;	/* [한국어] 실패하면 되돌린다. */
		rc = pages_to_xarray(&pages->pinned_pfns, span.start_hole,	/* [한국어] xarray 에 넣는다. */
				     span.last_hole, cur_pages);
		if (rc) {	/* [한국어] 넣지 못했으면 */
			iopt_pages_err_unpin(pages, span.start_hole,	/* [한국어] 방금 고정한 것을 놓고 */
					     span.last_hole, cur_pages);
			goto out_clean_xa;	/* [한국어] 되돌린다. */
		}
		xa_end = span.last_hole + 1;	/* [한국어] 채운 끝을 갱신. */
	}
	rc = pfn_reader_user_update_pinned(&user, pages);	/* [한국어] 한도를 확인하며 회계를 반영한다. */
	if (rc)	/* [한국어] 회계 반영에 실패했다. */
		goto out_clean_xa;	/* [한국어] 한도를 넘었으면 되돌린다. */
	user.upages = NULL;	/* [한국어] 출력 배열은 호출자 것이라 해제하면 안 된다. */
	pfn_reader_user_destroy(&user, pages);	/* [한국어] mm 참조와 락을 놓는다. */
	return 0;	/* [한국어] 성공. */

out_clean_xa:	/* [한국어] 실패 경로. */
	if (start_index != xa_end)	/* [한국어] 채운 것이 있으면 */
		iopt_pages_unfill_xarray(pages, start_index, xa_end - 1);	/* [한국어] 모두 비워 손대기 전 상태로. 이 과정에서 고정도 놓인다. */
	user.upages = NULL;	/* [한국어] 호출자 배열을 해제하지 않게. */
	pfn_reader_user_destroy(&user, pages);	/* [한국어] 원천 상태를 정리한다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * This uses the pfn_reader instead of taking a shortcut by using the mm. It can
 * do every scenario and is fully consistent with what an iommu_domain would
 * see.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_rw_slow - 반복자를 써서 어떤 경우에도 되는 읽기·쓰기
 *
 * @pages: 대상 pages 객체.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @offset: 첫 페이지 안의 오프셋.
 * @data: 커널 버퍼.
 * @length: 길이.
 * @flags: 방향 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 mm 을 지름길로 쓰지 않고 반복자를 거친다. 그래서 도메인이
 * 보는 것과 정확히 같은 내용을 보고, 어떤 원천에도 통한다. 대신 느리다.
 *
 * 묶음마다 곧바로 고정을 놓는 데 주의 — 복사가 끝나면 그 페이지를 더
 * 붙잡을 이유가 없다.
 */
static int iopt_pages_rw_slow(struct iopt_pages *pages,
			      unsigned long start_index,
			      unsigned long last_index, unsigned long offset,
			      void *data, unsigned long length,
			      unsigned int flags)
{
	struct pfn_reader pfns;	/* [한국어] PFN 반복자. */
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&pages->mutex);	/* [한국어] 반복자를 쓰므로 뮤텍스가 필요하다. */

	rc = pfn_reader_first(&pfns, pages, start_index, last_index);	/* [한국어] 범위를 읽을 반복자를 세운다. */
	if (rc)	/* [한국어] 실패하면 */
		goto out_unlock;	/* [한국어] 락만 놓고 나간다. */

	while (!pfn_reader_done(&pfns)) {	/* [한국어] 다 읽을 때까지. */
		unsigned long done;	/* [한국어] 이번 묶음에서 복사한 바이트. */

		done = batch_rw(&pfns.batch, data, offset, length, flags);	/* [한국어] 묶음이 가리키는 메모리와 커널 버퍼 사이를 복사한다. */
		data += done;	/* [한국어] 버퍼 자리를 옮기고 */
		length -= done;	/* [한국어] 남은 길이를 줄인다. */
		offset = 0;	/* [한국어] 첫 페이지 뒤로는 오프셋이 없다. */
		pfn_reader_unpin(&pfns);	/* [한국어] 복사가 끝났으니 아무도 안 붙잡은 페이지의 고정을 곧바로 놓는다. */

		rc = pfn_reader_next(&pfns);	/* [한국어] 다음 묶음. */
		if (rc)	/* [한국어] 다음 묶음을 읽는 데 실패했다. */
			goto out_destroy;	/* [한국어] 실패하면 정리하고 나간다. */
	}
	if (WARN_ON(length != 0))	/* [한국어] 다 읽었는데 길이가 남았으면 계산이 어긋난 것이다. */
		rc = -EINVAL;	/* [한국어] 실패로 알린다. */
out_destroy:	/* [한국어] 반복자를 정리해야 하는 경로. */
	pfn_reader_destroy(&pfns);	/* [한국어] 정리한다. */
out_unlock:	/* [한국어] 모든 경로가 합류한다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스를 놓고 */
	return rc;	/* [한국어] 결과를 올린다. */
}

/*
 * A medium speed path that still allows DMA inconsistencies, but doesn't do any
 * memory allocations or interval tree searches.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_rw_page - 페이지 하나만 다루는 중간 속도 경로
 *
 * @pages: 대상 pages 객체.
 * @index: 그 페이지 번호.
 * @offset: 페이지 안의 오프셋.
 * @data: 커널 버퍼.
 * @length: 길이.
 * @flags: 방향 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 할당도 구간 트리 탐색도 하지 않는다. 대신 DMA 가 보는 것과
 * 어긋날 수 있다 — 그 사이 매핑이 바뀌면 다른 페이지를 볼 수도 있다.
 * 짧은 접근에는 그 정도로 충분하다.
 *
 * 주소 공간이 이미 사라졌으면 느린 경로로 물러선다. 그쪽은 도메인에서
 * 읽을 수 있어 mm 없이도 된다.
 */
static int iopt_pages_rw_page(struct iopt_pages *pages, unsigned long index,
			      unsigned long offset, void *data,
			      unsigned long length, unsigned int flags)
{
	struct page *page = NULL;	/* [한국어] 고정해 올 페이지 하나. */
	int rc;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드 전용 인자 검사. */
	    WARN_ON(pages->type != IOPT_ADDRESS_USER))	/* [한국어] 이 경로는 사용자 포인터 원천 전용이다. */
		return -EINVAL;	/* [한국어] 다른 원천이면 거절. */

	if (!mmget_not_zero(pages->source_mm))	/* [한국어] 주소 공간이 이미 사라졌으면 */
		return iopt_pages_rw_slow(pages, index, index, offset, data,	/* [한국어] 느린 경로로 물러선다 — 그쪽은 도메인에서 읽을 수 있어 mm 없이도 된다. */
					  length, flags);

	if (iommufd_should_fail()) {	/* [한국어] 결함 주입 시험. */
		rc = -EINVAL;	/* [한국어] 실패를 흉내 내고 */
		goto out_mmput;	/* [한국어] 정리한다. */
	}

	mmap_read_lock(pages->source_mm);	/* [한국어] 남의 주소 공간을 들여다보는 동안 매핑이 바뀌면 안 된다. */
	rc = pin_user_pages_remote(	/* [한국어] 페이지 하나만 고정해 온다. */
		pages->source_mm, (uintptr_t)(pages->uptr + index * PAGE_SIZE),
		1, (flags & IOMMUFD_ACCESS_RW_WRITE) ? FOLL_WRITE : 0, &page,	/* [한국어] 쓰기면 쓰기 권한으로 — 그래야 copy-on-write 가 미리 풀린다. */
		NULL);
	mmap_read_unlock(pages->source_mm);	/* [한국어] 락 해제. */
	if (rc != 1) {	/* [한국어] 한 페이지를 얻지 못했다. */
		if (WARN_ON(rc >= 0))	/* [한국어] 0 이나 그 이상은 있을 수 없다. */
			rc = -EINVAL;	/* [한국어] 오류로 바꾼다. */
		goto out_mmput;	/* [한국어] 정리한다. */
	}
	copy_data_page(page, data, offset, length, flags);	/* [한국어] 복사한다. */
	unpin_user_page(page);	/* [한국어] 곧바로 고정을 놓는다 — 회계를 건드리지 않는 것에 주의. 이 경로는 pages 의 고정 상태에 끼지 않는다. */
	rc = 0;	/* [한국어] 성공. */

out_mmput:	/* [한국어] 모든 경로가 합류한다. */
	mmput(pages->source_mm);	/* [한국어] mm 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/**
 * iopt_pages_rw_access - Copy to/from a linear slice of the pages
 * @pages: pages to act on
 * @start_byte: First byte of pages to copy to/from
 * @data: Kernel buffer to get/put the data
 * @length: Number of bytes to copy
 * @flags: IOMMUFD_ACCESS_RW_* flags
 *
 * This will find each page in the range, kmap it and then memcpy to/from
 * the given kernel buffer.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_pages_rw_access - 이 pages 의 한 구간을 커널 버퍼와 주고받는다
 *
 * @pages: 대상 pages 객체.
 * @start_byte: 시작 바이트 오프셋.
 * @data: 커널 버퍼.
 * @length: 길이.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 * @return: 0 성공, 음수면 실패.
 *
 * 세 경로 중 하나를 고르는 것이 이 함수의 일이다.
 *
 * 가장 빠른 길은 copy_to_user 다. 같은 주소 공간이면 그냥 되고, 커널
 * 스레드면 그 주소 공간을 잠시 빌려 쓴다. 원 주석대로 이 길은 고정
 * 일관성을 무시한다 — 진짜 DMA 경로가 아니므로 그래도 된다.
 *
 * 한 페이지짜리면 중간 경로로, 그 밖에는 느린 경로로 간다.
 *
 * dmabuf 는 CPU 로 읽고 쓸 수 있는 메모리가 아니라 아예 거절한다.
 */
int iopt_pages_rw_access(struct iopt_pages *pages, unsigned long start_byte,
			 void *data, unsigned long length, unsigned int flags)
{
	unsigned long start_index = start_byte / PAGE_SIZE;	/* [한국어] 첫 페이지 번호. */
	unsigned long last_index = (start_byte + length - 1) / PAGE_SIZE;	/* [한국어] 마지막 페이지 번호. */
	bool change_mm = current->mm != pages->source_mm;	/* [한국어] 남의 주소 공간을 봐야 하는가. */
	int rc = 0;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트가 느린 경로를 강제할 때만 켜지는 갈래. */
	    (flags & __IOMMUFD_ACCESS_RW_SLOW_PATH))	/* [한국어] 테스트가 일부러 느린 길을 타게 하는 플래그. */
		change_mm = true;	/* [한국어] 같은 주소 공간이어도 원격인 척한다 — 그러지 않으면 그 경로가 시험되지 않는다. */

	if ((flags & IOMMUFD_ACCESS_RW_WRITE) && !pages->writable)	/* [한국어] 읽기 전용 매핑에 쓰려 하면 */
		return -EPERM;	/* [한국어] 거절한다. 사용자가 세운 보호를 커널이 우회하면 안 된다. */

	if (iopt_is_dmabuf(pages))	/* [한국어] dmabuf 는 */
		return -EINVAL;	/* [한국어] CPU 로 읽고 쓸 수 있는 메모리가 아니다. */

	if (pages->type != IOPT_ADDRESS_USER)	/* [한국어] 파일 원천이면 */
		return iopt_pages_rw_slow(pages, start_index, last_index,	/* [한국어] 반복자를 거치는 길밖에 없다. */
					  start_byte % PAGE_SIZE, data, length,
					  flags);

	if (!(flags & IOMMUFD_ACCESS_RW_KTHREAD) && change_mm) {	/* [한국어] 커널 스레드가 아닌데 남의 주소 공간이면 빌려 쓸 수 없다. */
		if (start_index == last_index)	/* [한국어] 한 페이지짜리면 */
			return iopt_pages_rw_page(pages, start_index,	/* [한국어] 중간 속도 경로로. */
						  start_byte % PAGE_SIZE, data,
						  length, flags);
		return iopt_pages_rw_slow(pages, start_index, last_index,	/* [한국어] 여러 페이지면 느린 경로로. */
					  start_byte % PAGE_SIZE, data, length,
					  flags);
	}

	/*
	 * Try to copy using copy_to_user(). We do this as a fast path and
	 * ignore any pinning inconsistencies, unlike a real DMA path.
	 */
	if (change_mm) {	/* [한국어] 남의 주소 공간을 빌려야 하면 */
		if (!mmget_not_zero(pages->source_mm))	/* [한국어] 이미 사라졌으면 */
			return iopt_pages_rw_slow(pages, start_index,	/* [한국어] 느린 경로로 물러선다. */
						  last_index,
						  start_byte % PAGE_SIZE, data,
						  length, flags);
		kthread_use_mm(pages->source_mm);	/* [한국어] 커널 스레드가 그 주소 공간을 잠시 자기 것처럼 쓴다. 그래야 copy_to_user 가 통한다. */
	}

	if (flags & IOMMUFD_ACCESS_RW_WRITE) {	/* [한국어] 쓰기면 */
		if (copy_to_user(pages->uptr + start_byte, data, length))	/* [한국어] 사용자 메모리로 한 번에 복사한다. 원 주석대로 고정 일관성을 무시하는 빠른 길이다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소가 잘못됐다. */
	} else {
		if (copy_from_user(data, pages->uptr + start_byte, length))	/* [한국어] 읽기면 반대 방향. */
			rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
	}

	if (change_mm) {	/* [한국어] 빌려 썼으면 */
		kthread_unuse_mm(pages->source_mm);	/* [한국어] 돌려주고 */
		mmput(pages->source_mm);	/* [한국어] 참조를 놓는다. */
	}

	return rc;	/* [한국어] 결과. */
}

static struct iopt_pages_access *	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iopt_pages_get_exact_access - 정확히 같은 범위의 접근 기록을 찾는다
 *
 * @pages: 대상 pages 객체.
 * @index: 시작 번호.
 * @last: 마지막 번호(포함).
 * @return: 그 기록, 없으면 NULL.
 *
 * 원 주석대로 이 구간 트리에는 겹치는 범위들이 함께 들어 있다. 같은
 * 메모리를 여러 접근자가 서로 다른 범위로 붙잡을 수 있기 때문이다.
 *
 * 그래서 겹치는 것을 모두 훑으며 시작과 끝이 정확히 같은 것만 고른다.
 * 정확히 같아야 참조를 하나로 합쳐 셀 수 있다.
 */
iopt_pages_get_exact_access(struct iopt_pages *pages, unsigned long index,
			    unsigned long last)
{
	struct interval_tree_node *node;	/* [한국어] 트리에서 찾은 마디. */

	lockdep_assert_held(&pages->mutex);	/* [한국어] 트리를 보므로 뮤텍스 아래여야 한다. */

	/* There can be overlapping ranges in this interval tree */
	for (node = interval_tree_iter_first(&pages->access_itree, index, last);	/* [한국어] 원 주석대로 겹치는 범위들이 함께 들어 있어 */
	     node; node = interval_tree_iter_next(node, index, last))	/* [한국어] 겹치는 것을 모두 훑으며 */
		if (node->start == index && node->last == last)	/* [한국어] 시작과 끝이 정확히 같은 것만 고른다. 그래야 참조를 하나로 합쳐 셀 수 있다. */
			return container_of(node, struct iopt_pages_access,	/* [한국어] 마디에서 접근 기록으로 되짚는다. */
					    node);
	return NULL;	/* [한국어] 같은 범위의 기록이 없다. */
}

/**
 * iopt_area_add_access() - Record an in-knerel access for PFNs
 * @area: The source of PFNs
 * @start_index: First page index
 * @last_index: Inclusive last page index
 * @out_pages: Output list of struct page's representing the PFNs
 * @flags: IOMMUFD_ACCESS_RW_* flags
 * @lock_area: Fail userspace munmap on this area
 *
 * Record that an in-kernel access will be accessing the pages, ensure they are
 * pinned, and return the PFNs as a simple list of 'struct page *'.
 *
 * This should be undone through a matching call to iopt_area_remove_access()
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_add_access - 커널 접근자의 고정을 기록한다
 *
 * @area: PFN 을 내주는 영역.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @out_pages: 페이지 목록을 여기에 쓴다.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 * @lock_area: 참이면 사용자의 munmap 을 이 영역에서 실패하게 만든다.
 *
 * @return: 0 성공, 음수면 실패.
 *
 * 같은 범위의 기록이 이미 있으면 참조만 늘리고 xarray 에서 읽어 간다 —
 * 빠른 길이다. 없으면 새로 만들고 느린 채우기를 한다.
 *
 * num_locks 는 사용자가 그 메모리를 걷어 가지 못하게 막는 표시다.
 * 커널이 그 페이지를 계속 쓸 예정이면 사라지면 안 된다.
 */
int iopt_area_add_access(struct iopt_area *area, unsigned long start_index,
			 unsigned long last_index, struct page **out_pages,
			 unsigned int flags, bool lock_area)
{
	struct iopt_pages *pages = area->pages;	/* [한국어] PFN 공급자. */
	struct iopt_pages_access *access;	/* [한국어] 접근 기록. */
	int rc;	/* [한국어] 결과 코드. */

	if ((flags & IOMMUFD_ACCESS_RW_WRITE) && !pages->writable)	/* [한국어] 읽기 전용 매핑을 쓰기로 붙잡으려 하면 */
		return -EPERM;	/* [한국어] 거절. */

	mutex_lock(&pages->mutex);	/* [한국어] 상태를 지키는 뮤텍스. */
	access = iopt_pages_get_exact_access(pages, start_index, last_index);	/* [한국어] 같은 범위의 기록이 이미 있는지 본다. */
	if (access) {	/* [한국어] 있으면 빠른 길이다. */
		area->num_accesses++;	/* [한국어] 이 영역을 붙잡은 접근자 수를 늘린다. */
		if (lock_area)	/* [한국어] 사용자의 munmap 을 막아야 하면 */
			area->num_locks++;	/* [한국어] 그 수도 늘린다. */
		access->users++;	/* [한국어] 이 기록의 참조를 늘린다. */
		iopt_pages_fill_from_xarray(pages, start_index, last_index,	/* [한국어] 이미 채워져 있으므로 그대로 읽어 간다. */
					    out_pages);
		mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스 해제. */
		return 0;	/* [한국어] 성공. */
	}

	access = kzalloc_obj(*access, GFP_KERNEL_ACCOUNT);	/* [한국어] 새 기록을 만든다. 사용자 앞으로 계산되게 한다. */
	if (!access) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto err_unlock;	/* [한국어] 락만 놓고 나간다. */
	}

	rc = iopt_pages_fill_xarray(pages, start_index, last_index, out_pages);	/* [한국어] 느린 길 — 다른 층에서 xarray 로 옮겨 오거나 원천에서 고정한다. */
	if (rc)	/* [한국어] 실패하면 */
		goto err_free;	/* [한국어] 기록을 버린다. */

	access->node.start = start_index;	/* [한국어] 붙잡은 범위의 시작. */
	access->node.last = last_index;	/* [한국어] 그 끝. */
	access->users = 1;	/* [한국어] 첫 참조. */
	area->num_accesses++;	/* [한국어] 영역 쪽 개수도 늘린다. */
	if (lock_area)	/* [한국어] munmap 을 막아야 하면 */
		area->num_locks++;	/* [한국어] 그 수도. */
	interval_tree_insert(&access->node, &pages->access_itree);	/* [한국어] 트리에 넣는다. 이 순간부터 이 구간의 고정이 유지된다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스 해제. */
	return 0;	/* [한국어] 성공. */

err_free:	/* [한국어] 채우기에 실패한 경로. */
	kfree(access);	/* [한국어] 기록을 해제한다. */
err_unlock:	/* [한국어] 모든 실패가 합류한다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스를 놓고 */
	return rc;	/* [한국어] 실패를 올린다. */
}

/**
 * iopt_area_remove_access() - Release an in-kernel access for PFNs
 * @area: The source of PFNs
 * @start_index: First page index
 * @last_index: Inclusive last page index
 * @unlock_area: Must match the matching iopt_area_add_access()'s lock_area
 *
 * Undo iopt_area_add_access() and unpin the pages if necessary. The caller
 * must stop using the PFNs before calling this.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_remove_access - 커널 접근자의 고정 기록을 놓는다
 *
 * @area: 그 영역.
 * @start_index: 첫 페이지 번호.
 * @last_index: 마지막 페이지 번호(포함).
 * @unlock_area: add 때 준 lock_area 와 같아야 한다.
 *
 * 원 주석대로 부르기 전에 그 PFN 을 쓰는 일을 멈춰야 한다.
 *
 * 마지막 참조일 때만 실제로 정리한다. 그때 xarray 층을 비우며, 아무도
 * 붙잡지 않는 페이지의 고정을 놓는다.
 *
 * 반환값이 없는 것이 이 파일의 원칙과 이어진다 — 놓기는 실패할 수 없다.
 */
void iopt_area_remove_access(struct iopt_area *area, unsigned long start_index,
			     unsigned long last_index, bool unlock_area)
{
	struct iopt_pages *pages = area->pages;	/* [한국어] PFN 공급자. */
	struct iopt_pages_access *access;	/* [한국어] 찾을 기록. */

	mutex_lock(&pages->mutex);	/* [한국어] 상태를 지키는 뮤텍스. */
	access = iopt_pages_get_exact_access(pages, start_index, last_index);	/* [한국어] add 때와 정확히 같은 범위여야 찾을 수 있다. */
	if (WARN_ON(!access))	/* [한국어] 없으면 호출자가 다른 범위를 준 것이다. */
		goto out_unlock;	/* [한국어] 아무것도 하지 않는다. */

	WARN_ON(area->num_accesses == 0 || access->users == 0);	/* [한국어] 0 인데 놓으라는 것은 짝이 맞지 않는다. */
	if (unlock_area) {	/* [한국어] add 때 lock_area 였으면 */
		WARN_ON(area->num_locks == 0);	/* [한국어] 그때 늘렸을 테니 0 일 수 없다. */
		area->num_locks--;	/* [한국어] 줄인다. */
	}
	area->num_accesses--;	/* [한국어] 영역 쪽 개수를 줄인다. */
	access->users--;	/* [한국어] 기록의 참조를 줄인다. */
	if (access->users)	/* [한국어] 아직 다른 접근자가 있으면 */
		goto out_unlock;	/* [한국어] 기록을 남겨 둔다. */

	interval_tree_remove(&access->node, &pages->access_itree);	/* [한국어] 트리에서 뺀다. 이 순간부터 이 구간은 접근자가 붙잡지 않은 것이 된다. */
	iopt_pages_unfill_xarray(pages, start_index, last_index);	/* [한국어] xarray 를 정리하며, 아무도 안 붙잡은 페이지의 고정을 놓는다. */
	kfree(access);	/* [한국어] 기록을 해제한다. */
out_unlock:	/* [한국어] 모든 경로가 합류한다. */
	mutex_unlock(&pages->mutex);	/* [한국어] 뮤텍스 해제. */
}
