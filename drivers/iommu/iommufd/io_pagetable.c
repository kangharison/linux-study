// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 *
 * The io_pagetable is the top of datastructure that maps IOVA's to PFNs. The
 * PFNs can be placed into an iommu_domain, or returned to the caller as a page
 * list for access by an in-kernel user.
 *
 * The datastructure uses the iopt_pages to optimize the storage of the PFNs
 * between the domains and xarray.
 */
/*
 * [한국어 설명] IOVA 공간의 구간 관리 (io_pagetable.c)
 *
 * === 파일의 역할 ===
 * 원 주석이 위치를 밝힌다 — IOVA 를 PFN 에 대응시키는 자료구조의 꼭대기이고,
 * 그 PFN 은 iommu_domain 에 채워지거나 커널 안의 소비자에게 페이지 목록으로
 * 건네진다. 그리고 iopt_pages 를 두어 도메인들과 xarray 사이에서 PFN 저장을
 * 아낀다.
 *
 * 이 파일이 다루는 일은 크게 넷이다.
 *
 * 1) IOVA 자리 찾기. 예약된 곳과 이미 쓰이는 곳을 피해 빈 구간을 고른다.
 *    double_span.h 의 두 트리 순회기가 여기서 쓰인다. 정렬을 사용자 주소에
 *    맞추는 요령도 여기 있다 — 그래야 THP 를 그대로 큰 페이지로 매핑할
 *    가능성이 커진다.
 *
 * 2) 구간의 생성과 파괴. 만드는 순서가 미묘하다. 먼저 pages 를 NULL 로 둔
 *    구간을 트리에 넣어 IOVA 자리를 잡아 두고 락을 놓은 뒤, 시간이 오래
 *    걸리는 페이지 고정과 도메인 채우기를 하고, 마지막에 pages 를 채운다.
 *
 * 3) 도메인 추가와 제거. 새 도메인이 들어오면 기존 매핑을 모두 그 도메인에
 *    복사해 넣어야 하고, 그 과정에서 IOVA 정렬 요구가 달라질 수 있다.
 *
 * 4) 구간 쪼개기. 사용자가 큰 구간의 일부만 걷어내려 하면 그 구간을 둘로
 *    나눠야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ioas.c 의 ioctl → [이 파일](구간과 IOVA) → pages.c(고정과 도메인 채우기)
 *   → iommu 코어 → 드라이버
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 순서는 io_pagetable.h 의 원 주석대로
 * domains_rwsem → iova_rwsem → pages::mutex 다.
 *
 * === 타 모듈과의 연결 ===
 * 위: ioas.c, device.c(장치 예약 구간), hw_pagetable.c(도메인 추가).
 * 아래: io_pagetable.h 의 자료 모델, double_span.h, pages.c.
 *
 * === 주요 함수/구조체 요약 ===
 * iopt_alloc_iova: 빈 IOVA 를 찾는다. 허용 트리 안에서, 예약과 기존 구간을
 *   피해서.
 * iopt_alloc_area_pages / iopt_map_pages: 구간을 만들고 채우는 두 단계.
 * iopt_unmap_iova_range: 걷어내기. 커널 접근이 붙잡고 있으면 기다린다.
 * iopt_table_add_domain / remove_domain: 도메인이 오갈 때 매핑을 옮긴다.
 * iopt_area_split: 구간을 둘로 나눈다.
 * iopt_table_enforce_dev_resv_regions: 장치가 요구한 예약 구간을 반영한다.
 */
#include <linux/dma-buf.h>	/* [한국어] dma-buf 출처의 페이지 */
#include <linux/err.h>	/* [한국어] ERR_PTR 계열 */
#include <linux/errno.h>	/* [한국어] 오류 코드 */
#include <linux/file.h>	/* [한국어] 파일 출처의 페이지 */
#include <linux/iommu.h>	/* [한국어] 도메인과 예약 구간 API */
#include <linux/iommufd.h>	/* [한국어] 드라이버에 공개된 부분 */
#include <linux/lockdep.h>	/* [한국어] 락 순서를 강제하는 주장들 */
#include <linux/sched/mm.h>	/* [한국어] 고정 페이지의 계상 */
#include <linux/slab.h>	/* [한국어] 구간과 예약 노드 할당 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체 */

#include "double_span.h"	/* [한국어] 예약과 구간을 함께 훑는 순회기 */
#include "io_pagetable.h"	/* [한국어] 구간과 페이지 묶음의 자료 모델 */

/*
 * [한국어] 매핑 요청 하나가 다루는 페이지 묶음의 조각.
 *
 * 한 번의 매핑이 여러 iopt_pages 에 걸칠 수 있다 — IOAS 사이 복사가
 * 그렇다. 원본에서 꺼낸 조각들이 이 목록으로 오고, 각각이 대상에서
 * 구간 하나가 된다.
 */
struct iopt_pages_list {
	struct iopt_pages *pages;
	/* [한국어] 이 조각이 쓸 페이지 묶음.
	 * 설정자: 매핑 진입점이 만들거나 원본에서 꺼내 온다.
	 * 읽는 자: 구간을 만들고 도메인에 채울 때.
	 * 값 범위: 유효한 포인터, 또는 구간으로 옮긴 뒤에는 NULL.
	 * 동기화: 호출 스택에 있어 공유되지 않는다. */
	struct iopt_area *area;
	/* [한국어] 그 묶음으로 만들 구간.
	 * 설정자: iopt_alloc_area_pages 가 미리 잡아 둔다.
	 * 읽는 자: 자리를 잡고 채운 뒤 완성할 때.
	 * 값 범위: 유효한 포인터, 또는 완성 후 NULL.
	 * 동기화: 호출 스택 값. */
	struct list_head next;
	/* [한국어] 조각 목록의 연결 고리.
	 * 설정자: 목록에 넣고 뺄 때.
	 * 읽는 자: 목록 순회.
	 * 값 범위: 그 목록의 항목.
	 * 동기화: 호출 스택 값. */
	unsigned long start_byte;
	/* [한국어] 묶음 안에서의 시작 오프셋.
	 * 설정자: 진입점이 원본 주소에서 계산한다.
	 * 읽는 자: 구간을 만들 때.
	 * 값 범위: 0 부터 묶음 크기까지.
	 * 동기화: 호출 스택 값. */
	unsigned long length;
	/* [한국어] 이 조각의 길이.
	 * 설정자: 진입점 또는 원본 구간의 길이에서.
	 * 읽는 자: 구간을 만들고 다음 IOVA 를 정할 때.
	 * 값 범위: 정렬을 지킨 바이트 수.
	 * 동기화: 호출 스택 값. */
};

/*
 * [한국어]
 * iopt_area_contig_init - 연속 구간 순회를 시작한다
 *
 * @iter: 채울 순회 상태.
 * @iopt: 대상 IOVA 공간.
 * @iova: 훑을 범위의 시작.
 * @last_iova: 그 끝.
 * @return: 첫 구간, 없으면 NULL.
 *
 * 시작 주소를 담는 구간이 없거나, 있어도 아직 채워지지 않았으면(pages 가
 * NULL) 곧바로 끝난다. 반쯤 세워진 구간을 읽으면 안 되기 때문이다.
 */
struct iopt_area *iopt_area_contig_init(struct iopt_area_contig_iter *iter,
					struct io_pagetable *iopt,
					unsigned long iova,
					unsigned long last_iova)
{
	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 트리를 훑는 동안 바뀌면 안 된다 */

	iter->cur_iova = iova;	/* [한국어] 다음에 볼 주소 */
	iter->last_iova = last_iova;	/* [한국어] 요청 범위의 끝 */
	iter->area = iopt_area_iter_first(iopt, iova, iova);	/* [한국어] 시작 주소를 담는 구간 */
	if (!iter->area)	/* [한국어] 매핑이 없으면 */
		return NULL;	/* [한국어] 훑을 것이 없다 */
	if (!iter->area->pages) {	/* [한국어] 아직 세우는 중인 구간이면 */
		iter->area = NULL;	/* [한국어] 반쯤 만들어진 것을 읽으면 안 된다 */
		return NULL;	/* [한국어] 없는 것으로 다룬다 */
	}
	return iter->area;	/* [한국어] 첫 구간 */
}

/*
 * [한국어]
 * iopt_area_contig_next - 다음 연속 구간으로 넘어간다
 *
 * @iter: 순회 상태.
 * @return: 다음 구간, 끝났거나 구멍을 만나면 NULL.
 *
 * "연속"이 이 함수의 요점이다. 다음 구간이 앞 구간의 끝 바로 다음에서
 * 시작하지 않으면 그 사이가 매핑되지 않은 구멍이므로 거기서 멈춘다.
 *
 * 호출자는 반복문이 끝난 뒤 iopt_area_contig_done() 으로 끝까지 닿았는지
 * 확인해야 한다 — 정상 종료와 구멍을 구별할 방법이 그것뿐이다.
 */
struct iopt_area *iopt_area_contig_next(struct iopt_area_contig_iter *iter)
{
	unsigned long last_iova;	/* [한국어] 현재 구간의 끝 */

	if (!iter->area)	/* [한국어] 이미 끝났으면 */
		return NULL;	/* [한국어] 더 볼 것이 없다 */
	last_iova = iopt_area_last_iova(iter->area);	/* [한국어] 현재 구간의 끝 */
	if (iter->last_iova <= last_iova)	/* [한국어] 요청 범위를 이미 덮었으면 */
		return NULL;	/* [한국어] 정상 종료 */

	iter->cur_iova = last_iova + 1;	/* [한국어] 바로 다음 주소부터 */
	iter->area = iopt_area_iter_next(iter->area, iter->cur_iova,	/* [한국어] 그 자리를 담는 구간을 */
					 iter->last_iova);	/* [한국어] 범위 안에서 찾는다 */
	if (!iter->area)	/* [한국어] 없으면 */
		return NULL;	/* [한국어] 구멍이다 */
	if (iter->cur_iova != iopt_area_iova(iter->area) ||	/* [한국어] 바로 이어지지 않거나 */
	    !iter->area->pages) {	/* [한국어] 아직 세우는 중이면 */
		iter->area = NULL;	/* [한국어] 연속이 끊긴 것이다 */
		return NULL;	/* [한국어] 호출자가 done() 으로 알아챈다 */
	}
	return iter->area;	/* [한국어] 이어지는 다음 구간 */
}

/*
 * [한국어]
 * __alloc_iova_check_range - 이 구간에 요청 길이가 들어가는지 본다
 *
 * @start: 구간의 시작(맞으면 정렬된 값으로 갱신된다).
 * @last: 구간의 끝.
 * @length: 필요한 길이.
 * @iova_alignment: 맞춰야 할 정렬.
 * @page_offset: 페이지 안에서의 오프셋.
 * @return: 들어가면 참.
 *
 * 정렬한 뒤 페이지 오프셋을 다시 얹는 것이 요점이다. 사용자 주소가 페이지
 * 정렬이 아니면 IOVA 도 같은 오프셋을 가져야 매핑이 성립한다.
 *
 * 넘침 검사가 두 곳에 있다 — 정렬을 올릴 때와, 남은 길이를 뺄 때다.
 * 후자를 뺄셈으로 쓴 것은 더하면 넘칠 수 있기 때문이다.
 */
static bool __alloc_iova_check_range(unsigned long *start, unsigned long last,
				     unsigned long length,
				     unsigned long iova_alignment,
				     unsigned long page_offset)
{
	unsigned long aligned_start;	/* [한국어] 정렬한 시작 주소 */

	/* ALIGN_UP() */
	if (check_add_overflow(*start, iova_alignment - 1, &aligned_start))	/* [한국어] (원 주석: 올림 정렬) */
		return false;	/* [한국어] 올리다 넘치면 쓸 수 없다 */
	aligned_start &= ~(iova_alignment - 1);	/* [한국어] 하위 비트를 깎고 */
	aligned_start |= page_offset;	/* [한국어] 사용자 주소와 같은 페이지 오프셋을 다시 얹는다 */

	if (aligned_start >= last || last - aligned_start < length - 1)	/* [한국어] 구간을 벗어나거나 남은 길이가 모자라면 */
		return false;	/* [한국어] 쓸 수 없다 — 더하지 않고 빼는 것은 넘침을 피하려는 것이다 */
	*start = aligned_start;	/* [한국어] 쓸 수 있는 시작 주소를 돌려준다 */
	return true;	/* [한국어] 후보로 삼는다 */
}

/*
 * [한국어]
 * __alloc_iova_check_hole - 두 트리의 구멍이 쓸 만한지 본다
 *
 * @span: 두 트리 순회의 현재 구간.
 * @length: 필요한 길이.
 * @iova_alignment: 맞춰야 할 정렬.
 * @page_offset: 페이지 안에서의 오프셋.
 * @return: 쓸 수 있으면 참.
 *
 * 예약 트리와 구간 트리를 함께 훑은 결과에서, 어느 쪽에도 걸리지 않은
 * 구멍만 후보가 된다.
 */
static bool __alloc_iova_check_hole(struct interval_tree_double_span_iter *span,
				    unsigned long length,
				    unsigned long iova_alignment,
				    unsigned long page_offset)
{
	if (span->is_used)	/* [한국어] 예약이나 기존 구간이 있으면 */
		return false;	/* [한국어] 쓸 수 없다 */
	return __alloc_iova_check_range(&span->start_hole, span->last_hole,	/* [한국어] 구멍의 범위에 */
					length, iova_alignment, page_offset);	/* [한국어] 요청이 들어가는지 본다 */
}

/*
 * [한국어]
 * __alloc_iova_check_used - 허용 트리의 구간이 쓸 만한지 본다
 *
 * @span: 한 트리 순회의 현재 구간.
 * @length: 필요한 길이.
 * @iova_alignment: 맞춰야 할 정렬.
 * @page_offset: 페이지 안에서의 오프셋.
 * @return: 쓸 수 있으면 참.
 *
 * 허용 트리에서는 뜻이 뒤집힌다 — "쓰이는 구간"이 곧 사용자가 허용한
 * 범위다. 그래서 구멍이 아니라 그쪽을 본다.
 */
static bool __alloc_iova_check_used(struct interval_tree_span_iter *span,
				    unsigned long length,
				    unsigned long iova_alignment,
				    unsigned long page_offset)
{
	if (span->is_hole)	/* [한국어] 허용 트리에서는 뜻이 뒤집힌다 — 구멍은 허용되지 않은 곳이다 */
		return false;	/* [한국어] 쓸 수 없다 */
	return __alloc_iova_check_range(&span->start_used, span->last_used,	/* [한국어] 허용된 범위에 */
					length, iova_alignment, page_offset);	/* [한국어] 요청이 들어가는지 본다 */
}

/*
 * Automatically find a block of IOVA that is not being used and not reserved.
 * Does not return a 0 IOVA even if it is valid.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_alloc_iova - 쓸 수 있는 IOVA 자리를 찾는다
 *
 * @iopt: 대상 IOVA 공간.
 * @iova: 찾은 주소를 돌려준다.
 * @addr: 매핑할 메모리의 주소(정렬을 맞추기 위해).
 * @length: 필요한 길이.
 * @return: 0 성공, -ENOSPC 자리 없음, -EOVERFLOW/-EINVAL.
 *
 * 두 겹의 순회가 이 함수의 뼈대다. 바깥은 사용자가 허용한 범위를 돌고,
 * 안쪽은 그 범위 안에서 예약과 기존 구간을 피한 구멍을 돈다.
 *
 * 정렬 고르기가 성능의 핵심이다. 원 주석이 밝히듯 원본 주소의 정렬을
 * IOVA 에도 살리면 THP 를 그대로 큰 페이지로 매핑할 가능성이 커진다.
 * 그래서 길이에서 나오는 정렬과 주소의 정렬 중 작은 쪽을 쓴다.
 *
 * 상한을 두는 이유는 두 가지다 — ALIGN 계산의 넘침을 막고, 지나치게 큰
 * 정렬을 요구해 자리를 못 찾는 일을 막는다.
 *
 * 허용 트리가 비어 있으면 전 범위를 허용한 것으로 다룬다. 앞뒤로 한
 * 페이지씩 비워 두는 이유: 원 주석대로 0 은 유효해도 돌려주지 않는다.
 */
static int iopt_alloc_iova(struct io_pagetable *iopt, unsigned long *iova,
			   unsigned long addr, unsigned long length)
{
	unsigned long page_offset = addr % PAGE_SIZE;	/* [한국어] 사용자 주소의 페이지 오프셋 — IOVA 도 같아야 한다 */
	struct interval_tree_double_span_iter used_span;	/* [한국어] 예약과 기존 구간을 함께 훑는다 */
	struct interval_tree_span_iter allowed_span;	/* [한국어] 허용 범위를 훑는다 */
	unsigned long max_alignment = PAGE_SIZE;	/* [한국어] 정렬 상한 */
	unsigned long iova_alignment;	/* [한국어] 이번에 요구할 정렬 */

	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 트리들을 훑는 동안 */

	/* Protect roundup_pow-of_two() from overflow */
	if (length == 0 || length >= ULONG_MAX / 2)	/* [한국어] (원 주석: roundup_pow_of_two 의 넘침을 막는다) */
		return -EOVERFLOW;	/* [한국어] 정렬을 계산할 수 없다 */

	/*
	 * Keep alignment present in addr when building the IOVA, which
	 * increases the chance we can map a THP.
	 */
	if (!addr)	/* [한국어] (원 주석: 주소의 정렬을 IOVA 에 살려 THP 매핑 가능성을 높인다) */
		iova_alignment = roundup_pow_of_two(length);	/* [한국어] 주소가 없으면 길이만 보고 */
	else
		iova_alignment = min_t(unsigned long,	/* [한국어] 있으면 둘 중 */
				       roundup_pow_of_two(length),	/* [한국어] 길이에서 나오는 정렬과 */
				       1UL << __ffs64(addr));	/* [한국어] 주소의 정렬 중 작은 쪽 */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE	/* [한국어] THP 를 쓰는 커널이면 정렬 상한을 그 크기까지 올린다 */
	max_alignment = HPAGE_SIZE;	/* [한국어] THP 를 쓰는 커널이면 그 크기까지 */
#endif
	/* Protect against ALIGN() overflow */
	if (iova_alignment >= max_alignment)	/* [한국어] (원 주석: ALIGN 넘침을 막는다) */
		iova_alignment = max_alignment;	/* [한국어] 지나친 정렬은 자리를 못 찾게 한다 */

	if (iova_alignment < iopt->iova_alignment)	/* [한국어] 도메인이 요구하는 최소보다 작으면 */
		return -EINVAL;	/* [한국어] 매핑할 수 없다 */

	interval_tree_for_each_span(&allowed_span, &iopt->allowed_itree,	/* [한국어] 사용자가 허용한 범위를 돌되 */
				    PAGE_SIZE, ULONG_MAX - PAGE_SIZE) {	/* [한국어] 앞뒤 한 페이지는 비워 둔다 — 0 은 돌려주지 않는다 */
		if (RB_EMPTY_ROOT(&iopt->allowed_itree.rb_root)) {	/* [한국어] 허용 트리가 비어 있으면 */
			allowed_span.start_used = PAGE_SIZE;	/* [한국어] 전 범위를 허용한 것으로 */
			allowed_span.last_used = ULONG_MAX - PAGE_SIZE;	/* [한국어] 다루기 위해 */
			allowed_span.is_hole = false;	/* [한국어] 한 구간을 손수 만들어 넣는다 */
		}

		if (!__alloc_iova_check_used(&allowed_span, length,	/* [한국어] 그 허용 범위에 */
					     iova_alignment, page_offset))	/* [한국어] 요청이 들어가지 않으면 */
			continue;	/* [한국어] 다음 허용 범위로 */

		interval_tree_for_each_double_span(	/* [한국어] 그 안에서 */
			&used_span, &iopt->reserved_itree, &iopt->area_itree,	/* [한국어] 예약과 기존 구간을 피한 */
			allowed_span.start_used, allowed_span.last_used) {	/* [한국어] 구멍을 찾는다 */
			if (!__alloc_iova_check_hole(&used_span, length,	/* [한국어] 그 구멍에 */
						     iova_alignment,	/* [한국어] 요청이 */
						     page_offset))	/* [한국어] 들어가지 않으면 */
				continue;	/* [한국어] 다음 구멍으로 */

			*iova = used_span.start_hole;	/* [한국어] 찾았다 */
			return 0;	/* [한국어] 성공 */
		}
	}
	return -ENOSPC;	/* [한국어] 쓸 수 있는 자리가 없다 */
}

/*
 * [한국어]
 * iopt_check_iova - 사용자가 지정한 IOVA 를 쓸 수 있는지 본다
 *
 * @iopt: 대상 IOVA 공간.
 * @iova: 쓰려는 주소.
 * @length: 길이.
 * @return: 0 성공, -EINVAL 정렬/예약 위반, -EEXIST 이미 매핑됨.
 *
 * FIXED_IOVA 로 자리를 지정한 경우의 검증이다. 세 가지를 본다 — 정렬,
 * 예약 구간과의 겹침, 기존 매핑과의 겹침.
 */
static int iopt_check_iova(struct io_pagetable *iopt, unsigned long iova,
			   unsigned long length)
{
	unsigned long last;	/* [한국어] 범위의 끝 */

	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 트리를 보는 동안 */

	if ((iova & (iopt->iova_alignment - 1)))	/* [한국어] 정렬을 어기면 */
		return -EINVAL;	/* [한국어] 도메인에 채울 수 없다 */

	if (check_add_overflow(iova, length - 1, &last))	/* [한국어] 주소 공간 끝을 넘으면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	/* No reserved IOVA intersects the range */
	if (iopt_reserved_iter_first(iopt, iova, last))	/* [한국어] (원 주석: 예약된 IOVA 와 겹치지 않아야 한다) */
		return -EINVAL;	/* [한국어] 장치가 쓸 수 없는 자리다 */

	/* Check that there is not already a mapping in the range */
	if (iopt_area_iter_first(iopt, iova, last))	/* [한국어] (원 주석: 그 범위에 이미 매핑이 없어야 한다) */
		return -EEXIST;	/* [한국어] 덮어쓸 수 없다 */
	return 0;	/* [한국어] 쓸 수 있는 자리다 */
}

/*
 * The area takes a slice of the pages from start_bytes to start_byte + length
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_insert_area - 구간을 트리에 넣어 IOVA 자리를 잡는다
 *
 * @iopt: 대상 IOVA 공간.
 * @area: 채울 구간.
 * @pages: 이 구간이 쓸 페이지 묶음.
 * @iova: 시작 주소.
 * @start_byte: pages 안에서의 시작 오프셋.
 * @length: 길이.
 * @iommu_prot: 권한.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 요점을 짚는다 — pages 를 NULL 로 둔 채 넣는다. 그래야 IOVA
 * 자리는 잡아 두면서 락을 놓고, 시간이 오래 걸리는 고정과 채우기를 할 수
 * 있다. 다른 스레드는 그 NULL 을 보고 "아직 준비 중"임을 안다.
 *
 * 두 좌표계를 함께 채우는 것도 여기다: IOVA 로 정렬된 노드와 페이지
 * 색인으로 정렬된 노드.
 *
 * 쓰기 권한 검사가 앞에 있는 이유: 읽기 전용으로 고정한 페이지에 쓰기를
 * 허용하면 그 메모리를 장치가 망가뜨린다.
 */
static int iopt_insert_area(struct io_pagetable *iopt, struct iopt_area *area,
			    struct iopt_pages *pages, unsigned long iova,
			    unsigned long start_byte, unsigned long length,
			    int iommu_prot)
{
	lockdep_assert_held_write(&iopt->iova_rwsem);	/* [한국어] 트리에 넣으므로 쓰기 락 */

	if ((iommu_prot & IOMMU_WRITE) && !pages->writable)	/* [한국어] 읽기 전용으로 고정한 메모리에 쓰기를 허용하면 */
		return -EPERM;	/* [한국어] 장치가 그 메모리를 망가뜨린다 */

	area->iommu_prot = iommu_prot;	/* [한국어] 도메인에 채울 때 그대로 쓰인다 */
	area->page_offset = start_byte % PAGE_SIZE;	/* [한국어] 첫 페이지 안에서의 오프셋 */
	if (area->page_offset & (iopt->iova_alignment - 1))	/* [한국어] 그 오프셋이 정렬을 어기면 */
		return -EINVAL;	/* [한국어] IOVA 를 아무리 맞춰도 물리 주소가 어긋난다 */

	area->node.start = iova;	/* [한국어] IOVA 트리의 시작 */
	if (check_add_overflow(iova, length - 1, &area->node.last))	/* [한국어] 끝이 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	area->pages_node.start = start_byte / PAGE_SIZE;	/* [한국어] 페이지 트리의 시작 색인 */
	if (check_add_overflow(start_byte, length - 1, &area->pages_node.last))	/* [한국어] 끝 바이트가 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */
	area->pages_node.last = area->pages_node.last / PAGE_SIZE;	/* [한국어] 바이트를 페이지 색인으로 */
	if (WARN_ON(area->pages_node.last >= pages->npages))	/* [한국어] 묶음 밖을 가리키면 */
		return -EOVERFLOW;	/* [한국어] 호출자의 계산이 잘못됐다 */

	/*
	 * The area is inserted with a NULL pages indicating it is not fully
	 * initialized yet.
	 */
	area->iopt = iopt;	/* [한국어] 되돌릴 때 어느 트리에서 뺄지 */
	interval_tree_insert(&area->node, &iopt->area_itree);	/* [한국어] (원 주석: pages 가 NULL 인 채로 넣어 아직 준비 중임을 알린다) */
	return 0;	/* [한국어] 자리를 잡았다 */
}

/*
 * [한국어]
 * iopt_area_alloc - 빈 구간 하나를 잡는다
 *
 * @return: 새 구간, 실패하면 NULL.
 *
 * 두 트리 노드를 비어 있는 상태로 표시해 둔다 — 되돌릴 때 트리에
 * 들어갔는지 아닌지를 그것으로 판별한다.
 */
static struct iopt_area *iopt_area_alloc(void)
{
	struct iopt_area *area;	/* [한국어] 만들 구간 */

	area = kzalloc_obj(*area, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 시킨 할당이라 계상한다 */
	if (!area)	/* [한국어] 메모리 부족 */
		return NULL;	/* [한국어] 호출자에게 */
	RB_CLEAR_NODE(&area->node.rb);	/* [한국어] 트리에 들어갔는지 판별하려고 */
	RB_CLEAR_NODE(&area->pages_node.rb);	/* [한국어] 두 노드 모두 비어 있음으로 표시 */
	return area;	/* [한국어] 빈 구간 */
}

/*
 * [한국어]
 * iopt_alloc_area_pages - 목록의 모든 조각에 대해 IOVA 자리를 잡는다
 *
 * @iopt: 대상 IOVA 공간.
 * @pages_list: 매핑할 조각들.
 * @length: 전체 길이.
 * @dst_iova: 시작 IOVA(자동이면 여기 담아 돌려준다).
 * @iommu_prot: 권한.
 * @flags: IOPT_ALLOC_IOVA 면 커널이 자리를 고른다.
 * @return: 0 성공, 음수면 실패.
 *
 * 구간 구조체를 락 밖에서 미리 잡아 두는 것이 눈에 띈다 — 락 안에서
 * 할당하면 그동안 다른 요청이 모두 막힌다.
 *
 * 자동 할당일 때 첫 조각의 원본 주소로 정렬을 짐작한다. 여러 조각이
 * 이어 붙는 경우에도 첫 조각의 정렬이 가장 중요하기 때문이다.
 *
 * 원 주석이 마지막 대목을 설명한다 — 구간을 pages 없이 만들어 IOVA 공간만
 * 예약해 두고 락을 놓는다.
 */
static int iopt_alloc_area_pages(struct io_pagetable *iopt,
				 struct list_head *pages_list,
				 unsigned long length, unsigned long *dst_iova,
				 int iommu_prot, unsigned int flags)
{
	struct iopt_pages_list *elm;	/* [한국어] 조각 순회용 */
	unsigned long start;	/* [한국어] 정렬을 짐작할 원본 주소 */
	unsigned long iova;	/* [한국어] 조각마다 밀어 갈 IOVA */
	int rc = 0;	/* [한국어] 결과 */

	list_for_each_entry(elm, pages_list, next) {	/* [한국어] 락 밖에서 미리 */
		elm->area = iopt_area_alloc();	/* [한국어] 구간 구조체를 잡아 둔다 */
		if (!elm->area)	/* [한국어] 실패하면 */
			return -ENOMEM;	/* [한국어] 호출자가 목록을 통째로 버린다 */
	}

	down_write(&iopt->iova_rwsem);	/* [한국어] 여기서부터 트리를 고친다 */
	if ((length & (iopt->iova_alignment - 1)) || !length) {	/* [한국어] 길이가 정렬을 어기거나 0 이면 */
		rc = -EINVAL;	/* [한국어] 매핑할 수 없다 */
		goto out_unlock;	/* [한국어] 풀고 나간다 */
	}

	if (flags & IOPT_ALLOC_IOVA) {	/* [한국어] 커널이 자리를 고르는 경우 */
		/* Use the first entry to guess the ideal IOVA alignment */
		elm = list_first_entry(pages_list, struct iopt_pages_list,	/* [한국어] (원 주석: 첫 항목으로 이상적인 정렬을 짐작한다) */
				       next);	/* [한국어] 여러 조각이 이어져도 첫 정렬이 가장 중요하다 */
		switch (elm->pages->type) {	/* [한국어] 출처에 따라 원본 주소가 다르다 */
		case IOPT_ADDRESS_USER:	/* [한국어] 사용자 VA 면 */
			start = elm->start_byte + (uintptr_t)elm->pages->uptr;	/* [한국어] 그 주소에 오프셋을 더한 값 */
			break;
		case IOPT_ADDRESS_FILE:	/* [한국어] 파일이면 */
			start = elm->start_byte + elm->pages->start;	/* [한국어] 파일 안에서의 오프셋 */
			break;
		case IOPT_ADDRESS_DMABUF:	/* [한국어] dma-buf 면 */
			start = elm->start_byte + elm->pages->dmabuf.start;	/* [한국어] 버퍼 안에서의 오프셋 */
			break;
		}
		rc = iopt_alloc_iova(iopt, dst_iova, start, length);	/* [한국어] 그 정렬을 살려 자리를 찾는다 */
		if (rc)	/* [한국어] 자리가 없으면 */
			goto out_unlock;	/* [한국어] 풀고 나간다 */
		if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 시험 빌드에서 */
		    WARN_ON(iopt_check_iova(iopt, *dst_iova, length))) {	/* [한국어] 찾은 자리가 정말 비어 있는지 다시 확인한다 */
			rc = -EINVAL;	/* [한국어] 아니면 할당기 버그다 */
			goto out_unlock;	/* [한국어] 풀고 나간다 */
		}
	} else {
		rc = iopt_check_iova(iopt, *dst_iova, length);	/* [한국어] 사용자가 지정한 자리면 검증만 */
		if (rc)	/* [한국어] 쓸 수 없는 자리면 */
			goto out_unlock;	/* [한국어] 풀고 나간다 */
	}

	/*
	 * Areas are created with a NULL pages so that the IOVA space is
	 * reserved and we can unlock the iova_rwsem.
	 */
	iova = *dst_iova;	/* [한국어] (원 주석: pages 없이 만들어 IOVA 공간을 예약하고 락을 놓는다) */
	list_for_each_entry(elm, pages_list, next) {	/* [한국어] 조각마다 */
		rc = iopt_insert_area(iopt, elm->area, elm->pages, iova,	/* [한국어] 구간을 트리에 넣고 */
				      elm->start_byte, elm->length, iommu_prot);	/* [한국어] 그 조각의 오프셋과 길이로 */
		if (rc)	/* [한국어] 실패하면 */
			goto out_unlock;	/* [한국어] 호출자가 되돌린다 */
		iova += elm->length;	/* [한국어] 다음 조각은 그 뒤에 */
	}

out_unlock:	/* [한국어] 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 자리를 다 잡았다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_abort_area - 반쯤 만든 구간을 되돌린다
 *
 * @area: 되돌릴 구간.
 *
 * 트리에 들어갔으면 빼고 버린다. pages 가 붙어 있으면 안 되는데, 그것은
 * 이미 완성된 구간이라는 뜻이라 이 함수로 지우면 페이지 참조가 샌다.
 */
static void iopt_abort_area(struct iopt_area *area)
{
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 시험 빌드에서만 */
		WARN_ON(area->pages);	/* [한국어] 완성된 구간을 이 함수로 지우면 페이지 참조가 샌다 */
	if (area->iopt) {	/* [한국어] 트리에 들어갔으면 */
		down_write(&area->iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */
		interval_tree_remove(&area->node, &area->iopt->area_itree);	/* [한국어] 빼고 */
		up_write(&area->iopt->iova_rwsem);	/* [한국어] 풀어 준다 */
	}
	kfree(area);	/* [한국어] 구간 자체 */
}

/*
 * [한국어]
 * iopt_free_pages_list - 조각 목록을 통째로 버린다
 *
 * @pages_list: 버릴 목록.
 *
 * 성공 경로에서도 쓰인다 — 그때는 각 조각의 area 와 pages 가 이미
 * NULL 로 옮겨져 있어 목록 껍데기만 버린다.
 */
void iopt_free_pages_list(struct list_head *pages_list)
{
	struct iopt_pages_list *elm;	/* [한국어] 순회용 */

	while ((elm = list_first_entry_or_null(pages_list,	/* [한국어] 목록이 빌 때까지 */
					       struct iopt_pages_list, next))) {	/* [한국어] 앞에서부터 */
		if (elm->area)	/* [한국어] 구간이 남아 있으면(실패 경로) */
			iopt_abort_area(elm->area);	/* [한국어] 되돌리고 */
		if (elm->pages)	/* [한국어] 페이지 참조가 남아 있으면 */
			iopt_put_pages(elm->pages);	/* [한국어] 놓는다 */
		list_del(&elm->next);	/* [한국어] 목록에서 빼고 */
		kfree(elm);	/* [한국어] 조각 자체를 버린다 */
	}
}

/*
 * [한국어]
 * iopt_fill_domains_pages - 목록의 모든 구간을 도메인들에 채운다
 *
 * @pages_list: 채울 조각들.
 * @return: 0 성공, 음수면 실패.
 *
 * 중간에 실패하면 그 앞까지를 역순으로 걷어낸다. 반쯤 채워진 상태를
 * 남기면 사용자가 보는 매핑과 하드웨어가 보는 것이 어긋난다.
 */
static int iopt_fill_domains_pages(struct list_head *pages_list)
{
	struct iopt_pages_list *undo_elm;	/* [한국어] 되돌리기 순회용 */
	struct iopt_pages_list *elm;	/* [한국어] 채우기 순회용 */
	int rc;	/* [한국어] 결과 */

	list_for_each_entry(elm, pages_list, next) {	/* [한국어] 조각마다 */
		rc = iopt_area_fill_domains(elm->area, elm->pages);	/* [한국어] 모든 도메인에 채운다 */
		if (rc)	/* [한국어] 실패하면 */
			goto err_undo;	/* [한국어] 앞까지를 되돌린다 */
	}
	return 0;	/* [한국어] 모두 채웠다 */

err_undo:	/* [한국어] 실패한 조각 앞까지를 되돌린다 */
	list_for_each_entry(undo_elm, pages_list, next) {	/* [한국어] 처음부터 다시 훑으며 */
		if (undo_elm == elm)	/* [한국어] 실패한 조각에 닿으면 */
			break;	/* [한국어] 멈춘다 */
		iopt_area_unfill_domains(undo_elm->area, undo_elm->pages);	/* [한국어] 그 앞까지를 걷어낸다 */
	}
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iopt_map_pages - 이미 얻어 둔 페이지 조각들을 매핑한다
 *
 * @iopt: 대상 IOVA 공간.
 * @pages_list: 매핑할 조각들.
 * @length: 전체 길이.
 * @dst_iova: 시작 IOVA.
 * @iommu_prot: 권한.
 * @flags: IOPT_ALLOC_IOVA 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 세 단계다: IOVA 자리를 잡고, 도메인들에 채우고, 마지막에 pages 를 붙인다.
 *
 * 원 주석이 마지막 단계의 락 조건을 못박는다 — pages 는 반드시
 * domains_rwsem 안에서 채워야 한다. 그래야 그 사이에 새로 들어오는
 * 도메인이 이 구간도 함께 채운다.
 *
 * 조각의 참조를 구간으로 "옮긴다"는 표현도 원 주석에 있다 — 목록에서
 * NULL 로 비우므로 나중의 정리가 두 번 놓지 않는다.
 */
int iopt_map_pages(struct io_pagetable *iopt, struct list_head *pages_list,
		   unsigned long length, unsigned long *dst_iova,
		   int iommu_prot, unsigned int flags)
{
	struct iopt_pages_list *elm;	/* [한국어] 조각 순회용 */
	int rc;	/* [한국어] 결과 */

	rc = iopt_alloc_area_pages(iopt, pages_list, length, dst_iova,	/* [한국어] 먼저 IOVA 자리를 잡고 */
				   iommu_prot, flags);	/* [한국어] 권한과 할당 방식으로 */
	if (rc)	/* [한국어] 자리를 못 잡았으면 */
		return rc;	/* [한국어] 호출자가 목록을 버린다 */

	down_read(&iopt->domains_rwsem);	/* [한국어] 채우는 동안 도메인이 오가면 안 된다 */
	rc = iopt_fill_domains_pages(pages_list);	/* [한국어] 모든 도메인에 채운다 */
	if (rc)	/* [한국어] 실패하면 */
		goto out_unlock_domains;	/* [한국어] 풀고 나간다 */

	down_write(&iopt->iova_rwsem);	/* [한국어] 구간을 완성하므로 쓰기 락 */
	list_for_each_entry(elm, pages_list, next) {	/* [한국어] 조각마다 */
		/*
		 * area->pages must be set inside the domains_rwsem to ensure
		 * any newly added domains will get filled. Moves the reference
		 * in from the list.
		 */
		elm->area->pages = elm->pages;	/* [한국어] (원 주석: pages 는 domains_rwsem 안에서 채워야 새 도메인이 이 구간도 함께 채운다) */
		elm->pages = NULL;	/* [한국어] 참조를 구간으로 옮긴다 — 나중의 정리가 두 번 놓지 않도록 */
		elm->area = NULL;	/* [한국어] 구간도 마찬가지 */
	}
	up_write(&iopt->iova_rwsem);	/* [한국어] 구간이 완성됐다 */
out_unlock_domains:	/* [한국어] 도메인 락을 놓고 나가는 공통 경로 */
	up_read(&iopt->domains_rwsem);	/* [한국어] 도메인이 오갈 수 있게 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_map_common - 페이지 묶음 하나를 매핑하는 공통 경로
 *
 * @ictx: 문맥.
 * @iopt: 대상 IOVA 공간.
 * @pages: 매핑할 묶음.
 * @iova: 시작 IOVA(자동이면 여기 담아 돌려준다).
 * @length: 길이.
 * @start_byte: 묶음 안에서의 시작 오프셋.
 * @iommu_prot: 권한.
 * @flags: IOPT_ALLOC_IOVA 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 조각 하나짜리 목록을 스택에 만들어 iopt_map_pages 에 넘긴다. 그 함수가
 * 여러 조각을 다루도록 되어 있어, 한 조각인 경우도 같은 길을 쓴다.
 *
 * 계상 방식 조정이 눈에 띈다. 문맥이 VFIO 방식이면 사용자 한도가 아니라
 * 프로세스 통계에 계상해야 하므로, 묶음의 방식을 여기서 맞춘다.
 */
static int iopt_map_common(struct iommufd_ctx *ictx, struct io_pagetable *iopt,
			   struct iopt_pages *pages, unsigned long *iova,
			   unsigned long length, unsigned long start_byte,
			   int iommu_prot, unsigned int flags)
{
	struct iopt_pages_list elm = {};	/* [한국어] 조각 하나짜리 목록을 스택에 */
	LIST_HEAD(pages_list);	/* [한국어] 그 목록의 머리 */
	int rc;	/* [한국어] 결과 */

	elm.pages = pages;	/* [한국어] 매핑할 묶음 */
	elm.start_byte = start_byte;	/* [한국어] 그 안에서의 시작 */
	if (ictx->account_mode == IOPT_PAGES_ACCOUNT_MM &&	/* [한국어] 문맥이 VFIO 방식이면 */
	    elm.pages->account_mode == IOPT_PAGES_ACCOUNT_USER)	/* [한국어] 묶음의 기본 방식을 */
		elm.pages->account_mode = IOPT_PAGES_ACCOUNT_MM;	/* [한국어] 프로세스 통계로 맞춘다 */
	elm.length = length;	/* [한국어] 이 조각의 길이 */
	list_add(&elm.next, &pages_list);	/* [한국어] 한 조각짜리 목록 */

	rc = iopt_map_pages(iopt, &pages_list, length, iova, iommu_prot, flags);	/* [한국어] 공통 경로로 */
	if (rc) {	/* [한국어] 실패하면 */
		if (elm.area)	/* [한국어] 미리 잡아 둔 구간이 남아 있으면 */
			iopt_abort_area(elm.area);	/* [한국어] 되돌리고 */
		if (elm.pages)	/* [한국어] 참조가 남아 있으면 */
			iopt_put_pages(elm.pages);	/* [한국어] 놓는다 */
		return rc;	/* [한국어] 실패 이유 */
	}
	return 0;	/* [한국어] 성공 */
}

/**
 * iopt_map_user_pages() - Map a user VA to an iova in the io page table
 * @ictx: iommufd_ctx the iopt is part of
 * @iopt: io_pagetable to act on
 * @iova: If IOPT_ALLOC_IOVA is set this is unused on input and contains
 *        the chosen iova on output. Otherwise is the iova to map to on input
 * @uptr: User VA to map
 * @length: Number of bytes to map
 * @iommu_prot: Combination of IOMMU_READ/WRITE/etc bits for the mapping
 * @flags: IOPT_ALLOC_IOVA or zero
 *
 * iova, uptr, and length must be aligned to iova_alignment. For domain backed
 * page tables this will pin the pages and load them into the domain at iova.
 * For non-domain page tables this will only setup a lazy reference and the
 * caller must use iopt_access_pages() to touch them.
 *
 * iopt_unmap_iova() must be called to undo this before the io_pagetable can be
 * destroyed.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_map_user_pages - 사용자 VA 를 IOVA 에 매핑한다
 *
 * @ictx: 문맥.
 * @iopt: 대상 IOVA 공간.
 * @iova: 시작 IOVA.
 * @uptr: 매핑할 사용자 주소.
 * @length: 길이.
 * @iommu_prot: 권한.
 * @flags: IOPT_ALLOC_IOVA 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 두 경우를 가른다 — 도메인이 딸린 표에서는 여기서 페이지를
 * 고정해 도메인에 채우고, 그렇지 않은 표에서는 게으른 참조만 세워
 * 호출자가 iopt_access_pages() 로 만질 때 고정된다.
 *
 * 오프셋 계산이 미묘하다. iopt_alloc_user_pages 가 주소를 페이지 경계로
 * 내려 잡을 수 있어, 원래 주소와의 차이가 곧 묶음 안의 시작 오프셋이다.
 */
int iopt_map_user_pages(struct iommufd_ctx *ictx, struct io_pagetable *iopt,
			unsigned long *iova, void __user *uptr,
			unsigned long length, int iommu_prot,
			unsigned int flags)
{
	struct iopt_pages *pages;	/* [한국어] 만들 페이지 묶음 */

	pages = iopt_alloc_user_pages(uptr, length, iommu_prot & IOMMU_WRITE);	/* [한국어] 쓰기 권한이 있으면 쓰기 가능하게 고정한다 */
	if (IS_ERR(pages))	/* [한국어] 실패면 */
		return PTR_ERR(pages);	/* [한국어] 그대로 */

	return iopt_map_common(ictx, iopt, pages, iova, length,	/* [한국어] 공통 경로로 */
			       uptr - pages->uptr, iommu_prot, flags);	/* [한국어] 묶음이 페이지 경계로 내려 잡혔을 수 있어 그 차이가 오프셋이다 */
}

/**
 * iopt_map_file_pages() - Like iopt_map_user_pages, but map a file.
 * @ictx: iommufd_ctx the iopt is part of
 * @iopt: io_pagetable to act on
 * @iova: If IOPT_ALLOC_IOVA is set this is unused on input and contains
 *        the chosen iova on output. Otherwise is the iova to map to on input
 * @fd: fdno of a file to map
 * @start: map file starting at this byte offset
 * @length: Number of bytes to map
 * @iommu_prot: Combination of IOMMU_READ/WRITE/etc bits for the mapping
 * @flags: IOPT_ALLOC_IOVA or zero
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_map_file_pages - 파일이나 dma-buf 를 IOVA 에 매핑한다
 *
 * @ictx: 문맥.
 * @iopt: 대상 IOVA 공간.
 * @iova: 시작 IOVA.
 * @fd: 매핑할 파일의 디스크립터.
 * @start: 그 안에서의 시작 오프셋.
 * @length: 길이.
 * @iommu_prot: 권한.
 * @flags: IOPT_ALLOC_IOVA 등.
 * @return: 0 성공, 음수면 실패.
 *
 * 같은 디스크립터가 dma-buf 일 수도 보통 파일일 수도 있다. 먼저 dma-buf
 * 로 열어 보고, 아니면 파일로 다룬다 — 사용자가 어느 쪽인지 따로 말하지
 * 않아도 되게 하려는 것이다.
 *
 * 참조 처리가 두 갈래에서 다르다. dma-buf 는 묶음이 그 참조를 물려받으므로
 * 실패할 때만 놓고, 파일은 묶음이 자기 참조를 따로 잡으므로 곧바로 놓는다.
 */
int iopt_map_file_pages(struct iommufd_ctx *ictx, struct io_pagetable *iopt,
			unsigned long *iova, int fd, unsigned long start,
			unsigned long length, int iommu_prot,
			unsigned int flags)
{
	struct iopt_pages *pages;	/* [한국어] 만들 페이지 묶음 */
	struct dma_buf *dmabuf;	/* [한국어] dma-buf 로 열어 본 결과 */
	unsigned long start_byte;	/* [한국어] 페이지 안에서의 오프셋 */
	unsigned long last;	/* [한국어] 범위의 끝 */

	if (!length)	/* [한국어] 길이가 0 이면 */
		return -EINVAL;	/* [한국어] 매핑할 것이 없다 */
	if (check_add_overflow(start, length - 1, &last))	/* [한국어] 끝이 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	start_byte = start - ALIGN_DOWN(start, PAGE_SIZE);	/* [한국어] 페이지 경계에서 얼마나 들어간 곳인가 */
	if (IS_ENABLED(CONFIG_DMA_SHARED_BUFFER))	/* [한국어] dma-buf 를 쓰는 커널이면 */
		dmabuf = dma_buf_get(fd);	/* [한국어] 그것으로 열어 보고 */
	else
		dmabuf = ERR_PTR(-ENXIO);	/* [한국어] 아니면 실패로 둔다 */

	if (!IS_ERR(dmabuf)) {	/* [한국어] dma-buf 였으면 */
		pages = iopt_alloc_dmabuf_pages(ictx, dmabuf, start_byte, start,	/* [한국어] 그 버퍼의 물리 구간으로 */
						length,	/* [한국어] 묶음을 만든다 */
						iommu_prot & IOMMU_WRITE);	/* [한국어] 쓰기 권한 여부 */
		if (IS_ERR(pages)) {	/* [한국어] 실패하면 */
			dma_buf_put(dmabuf);	/* [한국어] 참조를 놓는다 — 성공하면 묶음이 물려받는다 */
			return PTR_ERR(pages);	/* [한국어] 오류를 전한다 */
		}
	} else {
		struct file *file;	/* [한국어] 보통 파일이면 */

		file = fget(fd);	/* [한국어] 그 파일을 열고 */
		if (!file)	/* [한국어] 없는 디스크립터면 */
			return -EBADF;	/* [한국어] 거절 */

		pages = iopt_alloc_file_pages(file, start_byte, start, length,	/* [한국어] 파일 출처의 묶음을 만든다 */
					      iommu_prot & IOMMU_WRITE);	/* [한국어] 쓰기 권한 여부 */
		fput(file);	/* [한국어] 묶음이 자기 참조를 따로 잡으므로 곧바로 놓는다 */
		if (IS_ERR(pages))	/* [한국어] 실패면 */
			return PTR_ERR(pages);	/* [한국어] 그대로 */
	}

	return iopt_map_common(ictx, iopt, pages, iova, length,	/* [한국어] 공통 경로로 */
			       start_byte, iommu_prot, flags);	/* [한국어] 페이지 안 오프셋과 함께 */
}

/*
 * [한국어] 더티 비트맵 순회 콜백에 넘길 값들의 묶음.
 * iova_bitmap 계층이 void * 하나만 넘겨 주므로 필요한 것을 모아 둔다.
 */
struct iova_bitmap_fn_arg {
	unsigned long flags;
	/* [한국어] IOMMU_DIRTY_NO_CLEAR 등.
	 * 설정자: iommu_read_and_clear_dirty.
	 * 읽는 자: 콜백이 드라이버에 그대로 넘긴다.
	 * 값 범위: IOMMU_DIRTY_* 조합.
	 * 동기화: 호출 스택 값. */
	struct io_pagetable *iopt;
	/* [한국어] 훑을 IOVA 공간.
	 * 설정자: 같은 곳.
	 * 읽는 자: 콜백이 연속 구간을 훑을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 호출자가 읽기 락을 쥐고 있다. */
	struct iommu_domain *domain;
	/* [한국어] 더티를 물을 도메인.
	 * 설정자: 같은 곳.
	 * 읽는 자: 콜백이 드라이버 연산을 부를 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 호출 스택 값. */
	struct iommu_dirty_bitmap *dirty;
	/* [한국어] 결과를 쌓을 비트맵.
	 * 설정자: 같은 곳.
	 * 읽는 자: 드라이버가 여기에 비트를 세운다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 호출 스택 값. */
};

/*
 * [한국어]
 * __iommu_read_and_clear_dirty - 비트맵 한 조각에 해당하는 범위를 처리한다
 *
 * @bitmap: 순회 중인 비트맵(쓰지 않는다).
 * @iova: 이번 조각이 덮는 범위의 시작.
 * @length: 그 길이.
 * @opaque: 위 인자 묶음.
 * @return: 0 성공, -EINVAL 이면 그 범위에 구멍이 있다.
 *
 * 비트맵은 사용자 메모리라 한 번에 다 볼 수 없어, iova_bitmap 계층이
 * 조각으로 나눠 이 콜백을 부른다.
 *
 * 그 범위를 연속 구간으로 훑으며 각 구간의 더티 비트를 드라이버에게
 * 묻는다. 중간에 매핑되지 않은 곳이 있으면 사용자가 잘못된 범위를 준
 * 것이라 거절한다.
 */
static int __iommu_read_and_clear_dirty(struct iova_bitmap *bitmap,
					unsigned long iova, size_t length,
					void *opaque)
{
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */
	struct iopt_area_contig_iter iter;	/* [한국어] 연속 구간 순회 */
	struct iova_bitmap_fn_arg *arg = opaque;	/* [한국어] 넘겨받은 인자 묶음 */
	struct iommu_domain *domain = arg->domain;	/* [한국어] 대상 도메인 */
	struct iommu_dirty_bitmap *dirty = arg->dirty;	/* [한국어] 기록할 비트맵 */
	const struct iommu_dirty_ops *ops = domain->dirty_ops;	/* [한국어] 드라이버의 더티 연산 */
	unsigned long last_iova = iova + length - 1;	/* [한국어] 이번 조각의 끝 */
	unsigned long flags = arg->flags;	/* [한국어] IOMMU_DIRTY_NO_CLEAR 등 */
	int ret;	/* [한국어] 결과 */

	iopt_for_each_contig_area(&iter, area, arg->iopt, iova, last_iova) {	/* [한국어] 그 범위의 연속 구간들을 */
		unsigned long last = min(last_iova, iopt_area_last_iova(area));	/* [한국어] 구간과 조각 중 먼저 끝나는 쪽까지 */

		ret = ops->read_and_clear_dirty(domain, iter.cur_iova,	/* [한국어] 드라이버에게 그 범위의 */
						last - iter.cur_iova + 1, flags,	/* [한국어] 더티 비트를 묻는다 */
						dirty);	/* [한국어] 결과는 비트맵에 쌓인다 */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 바로 전한다 */
	}

	if (!iopt_area_contig_done(&iter))	/* [한국어] 중간에 매핑되지 않은 곳이 있으면 */
		return -EINVAL;	/* [한국어] 사용자가 잘못된 범위를 준 것이다 */
	return 0;	/* [한국어] 이 조각을 처리했다 */
}

static int	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommu_read_and_clear_dirty - 더티 비트를 사용자 비트맵으로 옮긴다
 *
 * @domain: 대상 도메인.
 * @iopt: 그 IOVA 공간.
 * @flags: IOMMU_DIRTY_NO_CLEAR 등.
 * @bitmap: 사용자가 준 비트맵 기술.
 * @return: 0 성공, -EOPNOTSUPP 지원 안 함, -ENOMEM.
 *
 * 라이브 마이그레이션의 핵심 경로다. 게스트가 도는 동안 바뀐 페이지를
 * 알아내 그것만 다시 보낸다.
 *
 * 지운 경우에만 무효화하는 이유: 더티 비트를 지웠으면 하드웨어의 캐시된
 * 변환도 지워야 다음 쓰기가 다시 표시를 남긴다.
 */
iommu_read_and_clear_dirty(struct iommu_domain *domain,
			   struct io_pagetable *iopt, unsigned long flags,
			   struct iommu_hwpt_get_dirty_bitmap *bitmap)
{
	const struct iommu_dirty_ops *ops = domain->dirty_ops;	/* [한국어] 드라이버의 더티 연산 */
	struct iommu_iotlb_gather gather;	/* [한국어] 지운 범위를 모아 무효화한다 */
	struct iommu_dirty_bitmap dirty;	/* [한국어] 비트맵과 그 gather 를 묶은 것 */
	struct iova_bitmap_fn_arg arg;	/* [한국어] 콜백에 넘길 값들 */
	struct iova_bitmap *iter;	/* [한국어] 사용자 비트맵을 조각내 훑는다 */
	int ret = 0;	/* [한국어] 결과 */

	if (!ops || !ops->read_and_clear_dirty)	/* [한국어] 드라이버가 지원하지 않으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	iter = iova_bitmap_alloc(bitmap->iova, bitmap->length,	/* [한국어] 사용자 비트맵을 */
				 bitmap->page_size,	/* [한국어] 그 페이지 크기로 */
				 u64_to_user_ptr(bitmap->data));	/* [한국어] 조각내 다룰 순회기를 만든다 */
	if (IS_ERR(iter))	/* [한국어] 실패면 */
		return -ENOMEM;	/* [한국어] 거절 */

	iommu_dirty_bitmap_init(&dirty, iter, &gather);	/* [한국어] 비트맵과 무효화 자리를 묶는다 */

	arg.flags = flags;	/* [한국어] 콜백에 넘길 값들을 */
	arg.iopt = iopt;	/* [한국어] 채운다 */
	arg.domain = domain;	/* [한국어] 대상 도메인 */
	arg.dirty = &dirty;	/* [한국어] 기록할 자리 */
	iova_bitmap_for_each(iter, &arg, __iommu_read_and_clear_dirty);	/* [한국어] 조각마다 콜백을 부른다 */

	if (!(flags & IOMMU_DIRTY_NO_CLEAR))	/* [한국어] 지웠으면 */
		iommu_iotlb_sync(domain, &gather);	/* [한국어] 캐시된 변환도 지워야 다음 쓰기가 표시를 남긴다 */

	iova_bitmap_free(iter);	/* [한국어] 순회기를 놓는다 */

	return ret;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_check_iova_range - 더티 비트맵 요청의 범위를 검증한다
 *
 * @iopt: 대상 IOVA 공간.
 * @bitmap: 사용자가 준 비트맵 기술.
 * @return: 0 성공, 음수면 잘못된 범위.
 *
 * 두 종류의 정렬을 함께 본다 — IOMMU 의 최소 페이지와, 사용자가 정한
 * 비트맵의 페이지 크기다. 둘 중 하나라도 어긋나면 비트와 페이지의 대응이
 * 깨진다.
 */
int iommufd_check_iova_range(struct io_pagetable *iopt,
			     struct iommu_hwpt_get_dirty_bitmap *bitmap)
{
	size_t iommu_pgsize = iopt->iova_alignment;	/* [한국어] IOMMU 의 최소 페이지 */
	u64 last_iova;	/* [한국어] 범위의 끝 */

	if (check_add_overflow(bitmap->iova, bitmap->length - 1, &last_iova))	/* [한국어] 끝이 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	if (bitmap->iova > ULONG_MAX || last_iova > ULONG_MAX)	/* [한국어] 커널의 IOVA 폭을 넘으면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	if ((bitmap->iova & (iommu_pgsize - 1)) ||	/* [한국어] IOMMU 페이지 정렬을 어기거나 */
	    ((last_iova + 1) & (iommu_pgsize - 1)))	/* [한국어] 끝도 그러면 */
		return -EINVAL;	/* [한국어] 비트와 페이지의 대응이 깨진다 */

	if (!bitmap->page_size)	/* [한국어] 비트맵 페이지 크기가 0 이면 */
		return -EINVAL;	/* [한국어] 한 비트가 무엇을 뜻하는지 알 수 없다 */

	if ((bitmap->iova & (bitmap->page_size - 1)) ||	/* [한국어] 그 크기 정렬도 */
	    ((last_iova + 1) & (bitmap->page_size - 1)))	/* [한국어] 양쪽에서 지켜야 한다 */
		return -EINVAL;	/* [한국어] 아니면 비트가 페이지 경계에 걸린다 */

	return 0;	/* [한국어] 유효한 요청 */
}

/*
 * [한국어]
 * iopt_read_and_clear_dirty_data - 검증한 뒤 더티 비트를 읽어 온다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 대상 도메인.
 * @flags: IOMMU_DIRTY_NO_CLEAR 등.
 * @bitmap: 사용자가 준 비트맵 기술.
 * @return: 0 성공, 음수면 실패.
 *
 * 읽기 락으로 족하다 — 구간을 바꾸지 않고 읽기만 하기 때문이다.
 */
int iopt_read_and_clear_dirty_data(struct io_pagetable *iopt,
				   struct iommu_domain *domain,
				   unsigned long flags,
				   struct iommu_hwpt_get_dirty_bitmap *bitmap)
{
	int ret;	/* [한국어] 결과 */

	ret = iommufd_check_iova_range(iopt, bitmap);	/* [한국어] 범위를 먼저 검증하고 */
	if (ret)	/* [한국어] 잘못됐으면 */
		return ret;	/* [한국어] 거절 */

	down_read(&iopt->iova_rwsem);	/* [한국어] 구간을 읽기만 하므로 읽기 락 */
	ret = iommu_read_and_clear_dirty(domain, iopt, flags, bitmap);	/* [한국어] 비트를 옮긴다 */
	up_read(&iopt->iova_rwsem);	/* [한국어] 끝 */

	return ret;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_clear_dirty_data - 모든 구간의 더티 비트를 지운다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 대상 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 추적을 켜기 직전에 부른다. 켜기 전에 쌓여 있던 표시를 지워야 그 시점
 * 이후의 쓰기만 세어진다 — 원 주석이 "깨끗한 스냅숏"이라 부르는 것이다.
 *
 * 비트맵을 NULL 로 두어 기록은 하지 않고 지우기만 한다.
 */
static int iopt_clear_dirty_data(struct io_pagetable *iopt,
				 struct iommu_domain *domain)
{
	const struct iommu_dirty_ops *ops = domain->dirty_ops;	/* [한국어] 드라이버의 더티 연산 */
	struct iommu_iotlb_gather gather;	/* [한국어] 지운 범위를 모은다 */
	struct iommu_dirty_bitmap dirty;	/* [한국어] 기록하지 않는 비트맵 */
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */
	int ret = 0;	/* [한국어] 결과 */

	lockdep_assert_held_read(&iopt->iova_rwsem);	/* [한국어] 구간을 훑는 동안 */

	iommu_dirty_bitmap_init(&dirty, NULL, &gather);	/* [한국어] 비트맵을 NULL 로 — 지우기만 한다 */

	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 모든 구간을 */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] 하나씩 */
		if (!area->pages)	/* [한국어] 아직 세우는 중이면 */
			continue;	/* [한국어] 건너뛴다 */

		ret = ops->read_and_clear_dirty(domain, iopt_area_iova(area),	/* [한국어] 그 구간의 더티를 */
						iopt_area_length(area), 0,	/* [한국어] 지운다 */
						&dirty);	/* [한국어] 기록은 하지 않는다 */
		if (ret)	/* [한국어] 실패하면 */
			break;	/* [한국어] 멈춘다 */
	}

	iommu_iotlb_sync(domain, &gather);	/* [한국어] 지운 범위를 무효화해야 다음 쓰기가 표시를 남긴다 */
	return ret;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_set_dirty_tracking - 도메인의 더티 추적을 켜고 끈다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 대상 도메인.
 * @enable: 켤 것인가.
 * @return: 0 성공, -EOPNOTSUPP 지원 안 함.
 *
 * 켤 때만 먼저 지운다 — 원 주석대로 깨끗한 상태에서 세기 시작해야 한다.
 */
int iopt_set_dirty_tracking(struct io_pagetable *iopt,
			    struct iommu_domain *domain, bool enable)
{
	const struct iommu_dirty_ops *ops = domain->dirty_ops;	/* [한국어] 드라이버의 더티 연산 */
	int ret = 0;	/* [한국어] 결과 */

	if (!ops)	/* [한국어] 지원하지 않으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	down_read(&iopt->iova_rwsem);	/* [한국어] 구간을 읽기만 한다 */

	/* Clear dirty bits from PTEs to ensure a clean snapshot */
	if (enable) {	/* [한국어] (원 주석: 깨끗한 스냅숏이 되도록 PTE 의 더티 비트를 먼저 지운다) */
		ret = iopt_clear_dirty_data(iopt, domain);	/* [한국어] 켜기 전에 쌓인 것을 지운다 */
		if (ret)	/* [한국어] 실패하면 */
			goto out_unlock;	/* [한국어] 켜지 않는다 */
	}

	ret = ops->set_dirty_tracking(domain, enable);	/* [한국어] 하드웨어에 지시한다 */

out_unlock:	/* [한국어] 락을 놓고 나가는 공통 경로 */
	up_read(&iopt->iova_rwsem);	/* [한국어] 끝 */
	return ret;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_get_pages - 그 범위를 덮는 페이지 묶음 참조들을 꺼낸다
 *
 * @iopt: 대상 IOVA 공간.
 * @iova: 범위의 시작.
 * @length: 길이.
 * @pages_list: 채울 목록.
 * @return: 0 성공, -ENOENT 구멍 있음, -ENOMEM.
 *
 * IOAS 사이 복사의 앞 절반이다. 페이지를 다시 고정하지 않고 참조만
 * 올려 목록에 담는다 — 그래서 대상 IOAS 가 같은 물리 메모리를 공유한다.
 *
 * 중간에 매핑되지 않은 곳이 있으면 -ENOENT 로 거절한다. 부분 복사를
 * 허용하면 사용자가 어디까지 복사됐는지 알 수 없다.
 */
int iopt_get_pages(struct io_pagetable *iopt, unsigned long iova,
		   unsigned long length, struct list_head *pages_list)
{
	struct iopt_area_contig_iter iter;	/* [한국어] 연속 구간 순회 */
	unsigned long last_iova;	/* [한국어] 범위의 끝 */
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */
	int rc;	/* [한국어] 결과 */

	if (!length)	/* [한국어] 길이가 0 이면 */
		return -EINVAL;	/* [한국어] 꺼낼 것이 없다 */
	if (check_add_overflow(iova, length - 1, &last_iova))	/* [한국어] 끝이 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	down_read(&iopt->iova_rwsem);	/* [한국어] 구간을 읽기만 한다 */
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova) {	/* [한국어] 그 범위의 연속 구간들을 */
		struct iopt_pages_list *elm;	/* [한국어] 만들 조각 */
		unsigned long last = min(last_iova, iopt_area_last_iova(area));	/* [한국어] 구간과 요청 중 먼저 끝나는 쪽까지 */

		elm = kzalloc_obj(*elm, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 시킨 할당이라 계상한다 */
		if (!elm) {	/* [한국어] 메모리 부족 */
			rc = -ENOMEM;	/* [한국어] 거절하되 */
			goto err_free;	/* [한국어] 지금까지 만든 것을 버려야 한다 */
		}
		elm->start_byte = iopt_area_start_byte(area, iter.cur_iova);	/* [한국어] 묶음 안에서의 시작 */
		elm->pages = area->pages;	/* [한국어] 같은 묶음을 가리킨다 — 다시 고정하지 않는다 */
		elm->length = (last - iter.cur_iova) + 1;	/* [한국어] 이 조각의 길이 */
		kref_get(&elm->pages->kref);	/* [한국어] 참조만 올린다 */
		list_add_tail(&elm->next, pages_list);	/* [한국어] 목록 끝에 */
	}
	if (!iopt_area_contig_done(&iter)) {	/* [한국어] 중간에 구멍이 있으면 */
		rc = -ENOENT;	/* [한국어] 부분 복사를 허용하면 어디까지인지 알 수 없다 */
		goto err_free;	/* [한국어] 만든 것을 버린다 */
	}
	up_read(&iopt->iova_rwsem);	/* [한국어] 모두 꺼냈다 */
	return 0;	/* [한국어] 성공 */
err_free:	/* [한국어] 만들다 만 목록을 버린다 */
	up_read(&iopt->iova_rwsem);	/* [한국어] 실패 경로도 락은 풀어야 한다 */
	iopt_free_pages_list(pages_list);	/* [한국어] 만들다 만 목록을 버린다 */
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iopt_unmap_iova_range - 그 범위의 구간들을 걷어낸다
 *
 * @iopt: 대상 IOVA 공간.
 * @start: 범위의 시작.
 * @last: 그 끝(포함).
 * @unmapped: 실제로 걷어낸 바이트를 돌려준다.
 * @return: 0 성공, -ENOENT 구간이 범위를 벗어남, -EBUSY 경합/잠김,
 *          -EDEADLOCK 응답 없음.
 *
 * 이 파일에서 가장 까다로운 함수다. 세 가지가 얽혀 있다.
 *
 * 1) 락 순서와 재시도. 원 주석이 규칙을 밝힌다 — area->pages 가 NULL 인
 *    동안에는 반드시 domains_rwsem 을 읽기로 쥐고 있어야 한다. 그래야
 *    도메인 붙임·뗌이 정리 중인 구간과 겹치지 않는다.
 *
 * 2) 커널 접근이 붙잡고 있는 구간. 그런 구간은 곧바로 지울 수 없어,
 *    prevent_access 를 세우고 락을 놓은 뒤 소비자들에게 알린다. 그들이
 *    놓으면 처음부터 다시 시도한다. 100번을 넘으면 응답하지 않는 소비자가
 *    있는 것이라 -EDEADLOCK 으로 물러난다.
 *
 * 3) 락을 놓았다 잡는 사이의 경합. 원 주석대로 그사이 이미 지운 자리에
 *    새 구간이 생길 수 있어, 탐색 시작점을 앞으로 밀어 그것을 건드리지
 *    않는다.
 *
 * 구간이 요청 범위를 벗어나면 거절하는 이유: 이 함수는 쪼개지 않는다.
 * 부분 해제는 호출자가 iopt_cut_iova 로 미리 쪼갠 뒤에 해야 한다.
 */
static int iopt_unmap_iova_range(struct io_pagetable *iopt, unsigned long start,
				 unsigned long last, unsigned long *unmapped)
{
	struct iopt_area *area;	/* [한국어] 걷어낼 구간 */
	unsigned long unmapped_bytes = 0;	/* [한국어] 실제로 걷어낸 바이트 */
	unsigned int tries = 0;	/* [한국어] 같은 구간을 몇 번 다시 시도했는가 */
	/* If there are no mapped entries then success */
	int rc = 0;	/* [한국어] (원 주석: 매핑된 항목이 없으면 성공이다) */

	/*
	 * The domains_rwsem must be held in read mode any time any area->pages
	 * is NULL. This prevents domain attach/detatch from running
	 * concurrently with cleaning up the area.
	 */
again:	/* [한국어] 접근이 놓이기를 기다렸다가 처음부터 다시 */
	down_read(&iopt->domains_rwsem);	/* [한국어] (원 주석: area->pages 가 NULL 인 동안에는 이 락을 쥐고 있어야 도메인 붙임·뗌과 겹치지 않는다) */
	down_write(&iopt->iova_rwsem);	/* [한국어] 구간을 지우므로 쓰기 락 */
	while ((area = iopt_area_iter_first(iopt, start, last))) {	/* [한국어] 범위에 걸친 구간을 하나씩 */
		unsigned long area_last = iopt_area_last_iova(area);	/* [한국어] 그 구간의 끝 */
		unsigned long area_first = iopt_area_iova(area);	/* [한국어] 그 구간의 시작 */
		struct iopt_pages *pages;	/* [한국어] 그 구간의 페이지 묶음 */

		/* Userspace should not race map/unmap's of the same area */
		if (!area->pages) {	/* [한국어] (원 주석: 사용자가 같은 구간의 map/unmap 을 경쟁시키면 안 된다) */
			rc = -EBUSY;	/* [한국어] 아직 세우는 중이다 */
			goto out_unlock_iova;	/* [한국어] 풀고 나간다 */
		}

		/* The area is locked by an object that has not been destroyed */
		if (area->num_locks) {	/* [한국어] (원 주석: 아직 파괴되지 않은 객체가 이 구간을 잠갔다) */
			rc = -EBUSY;	/* [한국어] 그 객체가 놓을 때까지 지울 수 없다 */
			goto out_unlock_iova;	/* [한국어] 풀고 나간다 */
		}

		if (area_first < start || area_last > last) {	/* [한국어] 구간이 요청 범위를 벗어나면 */
			rc = -ENOENT;	/* [한국어] 이 함수는 쪼개지 않는다 */
			goto out_unlock_iova;	/* [한국어] 풀고 나간다 */
		}

		if (area_first != start)	/* [한국어] 다른 구간으로 넘어갔으면 */
			tries = 0;	/* [한국어] 재시도 횟수를 다시 센다 */

		/*
		 * num_accesses writers must hold the iova_rwsem too, so we can
		 * safely read it under the write side of the iovam_rwsem
		 * without the pages->mutex.
		 */
		if (area->num_accesses) {	/* [한국어] (원 주석: num_accesses 를 쓰는 쪽도 iova_rwsem 을 쥐므로 pages->mutex 없이 안전하게 읽는다) */
			size_t length = iopt_area_length(area);	/* [한국어] 알릴 범위의 길이 */

			start = area_first;	/* [한국어] 이 구간부터 다시 시작한다 */
			area->prevent_access = true;	/* [한국어] 새 접근이 들어오지 못하게 막고 */
			up_write(&iopt->iova_rwsem);	/* [한국어] 락을 놓는다 — 알림이 잠들 수 있다 */
			up_read(&iopt->domains_rwsem);	/* [한국어] 바깥 락도 */

			iommufd_access_notify_unmap(iopt, area_first, length);	/* [한국어] 소비자들에게 놓아 달라고 알린다 */
			/* Something is not responding to unmap requests. */
			tries++;	/* [한국어] (원 주석: 무언가 해제 요청에 응답하지 않고 있다) */
			if (WARN_ON(tries > 100)) {	/* [한국어] 100번을 넘으면 */
				rc = -EDEADLOCK;	/* [한국어] 응답하지 않는 소비자가 있는 것이다 */
				goto out_unmapped;	/* [한국어] 물러난다 */
			}
			goto again;	/* [한국어] 락을 다시 잡고 처음부터 */
		}

		pages = area->pages;	/* [한국어] 이제 지울 수 있다 */
		area->pages = NULL;	/* [한국어] 다른 스레드가 보면 "정리 중"으로 읽는다 */
		up_write(&iopt->iova_rwsem);	/* [한국어] 걷어내기는 오래 걸려 락을 놓는다 */

		iopt_area_unfill_domains(area, pages);	/* [한국어] 모든 도메인에서 걷어내고 */
		iopt_abort_area(area);	/* [한국어] 구간을 트리에서 빼고 */
		iopt_put_pages(pages);	/* [한국어] 페이지 참조를 놓는다 */

		unmapped_bytes += area_last - area_first + 1;	/* [한국어] 걷어낸 만큼 센다 */

		down_write(&iopt->iova_rwsem);	/* [한국어] 다시 잡고 이어 간다 */

		/*
		 * After releasing the iova_rwsem concurrent allocation could
		 * place new areas at IOVAs we have already unmapped. Keep
		 * moving the start of the search forward to ignore the area
		 * already unmapped.
		 */
		if (area_last >= last)	/* [한국어] (원 주석: 락을 놓은 사이 이미 지운 자리에 새 구간이 생길 수 있어 탐색 시작을 앞으로 민다) */
			break;	/* [한국어] 요청 범위를 다 덮었으면 끝 */
		start = area_last + 1;	/* [한국어] 지운 자리를 다시 보지 않는다 */
	}

out_unlock_iova:	/* [한국어] 두 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 걷어내기 끝 */
	up_read(&iopt->domains_rwsem);	/* [한국어] 바깥 락도 */
out_unmapped:	/* [한국어] 락은 이미 놓인 상태로 뛰어드는 자리 */
	if (unmapped)	/* [한국어] 호출자가 원하면 */
		*unmapped = unmapped_bytes;	/* [한국어] 걷어낸 양을 알려 준다 */
	return rc;	/* [한국어] 성패 */
}

/**
 * iopt_unmap_iova() - Remove a range of iova
 * @iopt: io_pagetable to act on
 * @iova: Starting iova to unmap
 * @length: Number of bytes to unmap
 * @unmapped: Return number of bytes unmapped
 *
 * The requested range must be a superset of existing ranges.
 * Splitting/truncating IOVA mappings is not allowed.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_unmap_iova - 그 범위의 매핑을 걷어낸다
 *
 * @iopt: 대상 IOVA 공간.
 * @iova: 시작 주소.
 * @length: 길이.
 * @unmapped: 걷어낸 바이트를 돌려준다.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 계약을 못박는다 — 요청 범위는 기존 구간들을 온전히 포함해야
 * 하고, 쪼개거나 잘라 내는 것은 허용되지 않는다.
 */
int iopt_unmap_iova(struct io_pagetable *iopt, unsigned long iova,
		    unsigned long length, unsigned long *unmapped)
{
	unsigned long iova_last;	/* [한국어] 범위의 끝 */

	if (!length)	/* [한국어] 길이가 0 이면 */
		return -EINVAL;	/* [한국어] 걷어낼 것이 없다 */

	if (check_add_overflow(iova, length - 1, &iova_last))	/* [한국어] 끝이 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	return iopt_unmap_iova_range(iopt, iova, iova_last, unmapped);	/* [한국어] 공통 경로로 */
}

/*
 * [한국어]
 * iopt_unmap_all - 전 범위의 매핑을 걷어낸다
 *
 * @iopt: 대상 IOVA 공간.
 * @unmapped: 걷어낸 바이트를 돌려준다.
 * @return: 0 성공, 음수면 실패.
 *
 * 전 범위를 주면 어떤 구간도 범위를 벗어나지 않으므로, 쪼갤 필요 없이
 * 모두 걷힌다.
 */
int iopt_unmap_all(struct io_pagetable *iopt, unsigned long *unmapped)
{
	/* If the IOVAs are empty then unmap all succeeds */
	return iopt_unmap_iova_range(iopt, 0, ULONG_MAX, unmapped);	/* [한국어] (원 주석: IOVA 가 비어 있으면 전체 해제도 성공이다) */
}

/* The caller must always free all the nodes in the allowed_iova rb_root. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_set_allow_iova - 허용 범위 트리를 통째로 갈아 끼운다
 *
 * @iopt: 대상 IOVA 공간.
 * @allowed_iova: 새 트리(성공하면 옛 트리가 여기 담겨 돌아온다).
 * @return: 0 성공, -EADDRINUSE 면 예약 구간과 겹친다.
 *
 * 두 번의 swap 이 원자성을 만든다. 먼저 바꿔 끼운 뒤 검사하고, 어긋나면
 * 다시 바꿔 원래대로 돌린다 — 그래서 실패해도 반쯤 바뀐 상태가 남지 않는다.
 *
 * 예약 구간과 겹치면 거절하는 이유: 그 자리는 장치가 쓸 수 없다고
 * 못박아 둔 곳이라, 허용한다고 해도 매핑이 실패한다.
 *
 * 원 주석대로 호출자는 어느 쪽이 돌아오든 그 트리의 노드를 모두 해제해야
 * 한다.
 */
int iopt_set_allow_iova(struct io_pagetable *iopt,
			struct rb_root_cached *allowed_iova)
{
	struct iopt_allowed *allowed;	/* [한국어] 순회 중인 허용 범위 */

	down_write(&iopt->iova_rwsem);	/* [한국어] 트리를 바꾸므로 */
	swap(*allowed_iova, iopt->allowed_itree);	/* [한국어] 먼저 새 트리로 바꿔 끼우고 */

	for (allowed = iopt_allowed_iter_first(iopt, 0, ULONG_MAX); allowed;	/* [한국어] 그 안의 범위들을 */
	     allowed = iopt_allowed_iter_next(allowed, 0, ULONG_MAX)) {	/* [한국어] 하나씩 확인한다 */
		if (iopt_reserved_iter_first(iopt, allowed->node.start,	/* [한국어] 예약 구간과 겹치면 */
					     allowed->node.last)) {	/* [한국어] 그 자리는 장치가 쓸 수 없다 */
			swap(*allowed_iova, iopt->allowed_itree);	/* [한국어] 원래대로 되돌리고 */
			up_write(&iopt->iova_rwsem);	/* [한국어] 풀고 */
			return -EADDRINUSE;	/* [한국어] 거절 — 반쯤 바뀐 상태가 남지 않는다 */
		}
	}
	up_write(&iopt->iova_rwsem);	/* [한국어] 갈아 끼웠다 */
	return 0;	/* [한국어] 성공 — 옛 트리가 인자에 담겨 돌아간다 */
}

/*
 * [한국어]
 * iopt_reserve_iova - 쓸 수 없는 IOVA 범위를 등록한다
 *
 * @iopt: 대상 IOVA 공간.
 * @start: 범위의 시작.
 * @last: 그 끝.
 * @owner: 이 예약을 등록하는 주체.
 * @return: 0 성공, -EADDRINUSE 이미 쓰이는 중, -ENOMEM.
 *
 * 장치를 붙일 때 그 장치가 요구한 예약 구간을 이 함수로 반영한다.
 *
 * 이미 매핑이 있거나 사용자가 허용한 범위면 거절한다 — 사용자가 쓰고
 * 있는 자리를 뒤늦게 막을 수 없기 때문이다. 그래서 그런 장치는 그
 * IOAS 에 붙일 수 없다.
 *
 * owner 를 기억하는 이유: 그 장치가 떠날 때 자기가 등록한 것만 거둬야 한다.
 */
int iopt_reserve_iova(struct io_pagetable *iopt, unsigned long start,
		      unsigned long last, void *owner)
{
	struct iopt_reserved *reserved;	/* [한국어] 만들 예약 노드 */

	lockdep_assert_held_write(&iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */

	if (iopt_area_iter_first(iopt, start, last) ||	/* [한국어] 이미 매핑이 있거나 */
	    iopt_allowed_iter_first(iopt, start, last))	/* [한국어] 사용자가 허용한 범위면 */
		return -EADDRINUSE;	/* [한국어] 뒤늦게 막을 수 없다 — 그 장치는 이 IOAS 에 붙지 못한다 */

	reserved = kzalloc_obj(*reserved, GFP_KERNEL_ACCOUNT);	/* [한국어] 예약 노드 */
	if (!reserved)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 호출자에게 */
	reserved->node.start = start;	/* [한국어] 막을 범위의 시작 */
	reserved->node.last = last;	/* [한국어] 그 끝 */
	reserved->owner = owner;	/* [한국어] 떠날 때 자기 것만 거두기 위해 */
	interval_tree_insert(&reserved->node, &iopt->reserved_itree);	/* [한국어] 예약 트리에 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * __iopt_remove_reserved_iova - 그 주인이 등록한 예약을 모두 거둔다
 *
 * @iopt: 대상 IOVA 공간.
 * @owner: 거둘 주인.
 *
 * 순회 중에 항목을 지우므로 다음 것을 미리 잡아 둔다.
 */
static void __iopt_remove_reserved_iova(struct io_pagetable *iopt, void *owner)
{
	struct iopt_reserved *reserved, *next;	/* [한국어] 순회용 — 지우면서 훑으므로 다음을 미리 잡는다 */

	lockdep_assert_held_write(&iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */

	for (reserved = iopt_reserved_iter_first(iopt, 0, ULONG_MAX); reserved;	/* [한국어] 모든 예약을 */
	     reserved = next) {	/* [한국어] 하나씩 */
		next = iopt_reserved_iter_next(reserved, 0, ULONG_MAX);	/* [한국어] 지우기 전에 다음을 잡아 둔다 */

		if (reserved->owner == owner) {	/* [한국어] 내가 등록한 것이면 */
			interval_tree_remove(&reserved->node,	/* [한국어] 트리에서 빼고 */
					     &iopt->reserved_itree);	/* [한국어] 그 자리를 */
			kfree(reserved);	/* [한국어] 버린다 */
		}
	}
}

/*
 * [한국어]
 * iopt_remove_reserved_iova - 락을 잡고 예약을 거둔다
 *
 * @iopt: 대상 IOVA 공간.
 * @owner: 거둘 주인.
 */
void iopt_remove_reserved_iova(struct io_pagetable *iopt, void *owner)
{
	down_write(&iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */
	__iopt_remove_reserved_iova(iopt, owner);	/* [한국어] 그 주인의 예약만 거둔다 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 끝 */
}

/*
 * [한국어]
 * iopt_init_table - 빈 IOVA 공간을 세운다
 *
 * @iopt: 초기화할 공간.
 *
 * 원 주석이 초기 정렬의 근거를 밝힌다 — 아직 도메인이 없는 iopt 는
 * 소프트웨어 표라 API 가 쓰는 size_t 전 범위를 쓸 수 있고 정렬 제한도
 * 없다. 도메인이 붙으면 그 하드웨어의 최소 페이지로 좁혀진다.
 */
void iopt_init_table(struct io_pagetable *iopt)
{
	init_rwsem(&iopt->iova_rwsem);	/* [한국어] 트리들을 지키는 락 */
	init_rwsem(&iopt->domains_rwsem);	/* [한국어] 도메인 목록을 지키는 락 */
	iopt->area_itree = RB_ROOT_CACHED;	/* [한국어] 아직 매핑이 없다 */
	iopt->allowed_itree = RB_ROOT_CACHED;	/* [한국어] 허용 제한도 없다 */
	iopt->reserved_itree = RB_ROOT_CACHED;	/* [한국어] 예약도 없다 */
	xa_init_flags(&iopt->domains, XA_FLAGS_ACCOUNT);	/* [한국어] 딸린 도메인 목록 */
	xa_init_flags(&iopt->access_list, XA_FLAGS_ALLOC);	/* [한국어] 커널 접근 목록 */

	/*
	 * iopt's start as SW tables that can use the entire size_t IOVA space
	 * due to the use of size_t in the APIs. They have no alignment
	 * restriction.
	 */
	iopt->iova_alignment = 1;	/* [한국어] (원 주석: 도메인이 없는 iopt 는 소프트웨어 표라 size_t 전 범위를 쓰고 정렬 제한이 없다) */
}

/*
 * [한국어]
 * iopt_destroy_table - IOVA 공간을 허문다
 *
 * @iopt: 허물 공간.
 *
 * 허용 트리만 여기서 비운다. 나머지는 이 시점에 이미 비어 있어야 하고,
 * 아니면 어딘가에서 정리를 빠뜨린 것이라 경고한다.
 *
 * 시험 빌드에서 주인 없는 예약을 먼저 거두는 이유: 시험이 예약을 직접
 * 넣어 두고 주인 없이 남길 수 있다.
 */
void iopt_destroy_table(struct io_pagetable *iopt)
{
	struct interval_tree_node *node;	/* [한국어] 순회용 */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 시험이 주인 없이 예약을 넣어 둘 수 있어 */
		iopt_remove_reserved_iova(iopt, NULL);	/* [한국어] 그것을 먼저 거둔다 */

	while ((node = interval_tree_iter_first(&iopt->allowed_itree, 0,	/* [한국어] 허용 트리는 여기서 */
						ULONG_MAX))) {	/* [한국어] 비운다 */
		interval_tree_remove(node, &iopt->allowed_itree);	/* [한국어] 하나씩 빼서 */
		kfree(container_of(node, struct iopt_allowed, node));	/* [한국어] 버린다 */
	}

	WARN_ON(!RB_EMPTY_ROOT(&iopt->reserved_itree.rb_root));	/* [한국어] 장치가 모두 떠났으면 비어 있어야 한다 */
	WARN_ON(!xa_empty(&iopt->domains));	/* [한국어] 도메인도 모두 빠졌어야 하고 */
	WARN_ON(!xa_empty(&iopt->access_list));	/* [한국어] 접근도 모두 거두어졌어야 한다 */
	WARN_ON(!RB_EMPTY_ROOT(&iopt->area_itree.rb_root));	/* [한국어] 매핑도 모두 걷혔어야 한다 */
}

/**
 * iopt_unfill_domain() - Unfill a domain with PFNs
 * @iopt: io_pagetable to act on
 * @domain: domain to unfill
 *
 * This is used when removing a domain from the iopt. Every area in the iopt
 * will be unmapped from the domain. The domain must already be removed from the
 * domains xarray.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_unfill_domain - 도메인에서 모든 매핑을 걷어낸다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 걷어낼 도메인.
 *
 * 도메인이 떠날 때 부른다. 원 주석이 전제를 밝힌다 — 그 도메인은 이미
 * domains 배열에서 빠져 있어야 한다.
 *
 * 두 갈래로 갈린다. 다른 도메인이 남아 있으면 PFN 은 그쪽이 계속 들고
 * 있으므로 매핑만 빠르게 지우면 된다. 마지막 도메인이면 PFN 자체를
 * 놓아야 하므로 훨씬 느린 경로를 탄다.
 */
static void iopt_unfill_domain(struct io_pagetable *iopt,
			       struct iommu_domain *domain)
{
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */

	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 구간을 훑으므로 */
	lockdep_assert_held_write(&iopt->domains_rwsem);	/* [한국어] 도메인이 빠지는 중이므로 */

	/*
	 * Some other domain is holding all the pfns still, rapidly unmap this
	 * domain.
	 */
	if (iopt->next_domain_id != 0) {	/* [한국어] (원 주석: 다른 도메인이 아직 PFN 을 들고 있으니 이 도메인만 빠르게 지운다) */
		/* Pick an arbitrary remaining domain to act as storage */
		struct iommu_domain *storage_domain =	/* [한국어] (원 주석: 남은 도메인 중 아무거나 저장용으로 고른다) */
			xa_load(&iopt->domains, 0);	/* [한국어] 0번이면 족하다 — 어느 것이든 PFN 을 들고 있다 */

		for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 모든 구간을 */
		     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] 하나씩 */
			struct iopt_pages *pages = area->pages;	/* [한국어] 그 구간의 묶음 */

			if (!pages)	/* [한국어] 아직 세우는 중이면 */
				continue;	/* [한국어] 건너뛴다 */

			mutex_lock(&pages->mutex);	/* [한국어] storage_domain 은 이 락이 지킨다 */
			if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 시험 빌드에서만 */
				WARN_ON(!area->storage_domain);	/* [한국어] 채워진 구간에는 반드시 있어야 한다 */
			if (area->storage_domain == domain)	/* [한국어] 떠나는 도메인이 저장용이었으면 */
				area->storage_domain = storage_domain;	/* [한국어] 다른 도메인으로 옮긴다 */
			if (iopt_is_dmabuf(pages)) {	/* [한국어] dma-buf 출처면 */
				if (!iopt_dmabuf_revoked(pages))	/* [한국어] 아직 회수되지 않았을 때만 */
					iopt_area_unmap_domain(area, domain);	/* [한국어] 매핑을 지운다 */
				iopt_dmabuf_untrack_domain(pages, area, domain);	/* [한국어] 추적 기록도 지운다 */
			}
			mutex_unlock(&pages->mutex);	/* [한국어] 묶음 보호 해제 */

			if (!iopt_is_dmabuf(pages))	/* [한국어] 보통 출처면 */
				iopt_area_unmap_domain(area, domain);	/* [한국어] 락 밖에서 매핑만 지운다 */
		}
		return;	/* [한국어] 빠른 경로 끝 */
	}

	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 마지막 도메인이면 모든 구간에서 */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] PFN 까지 놓아야 한다 */
		struct iopt_pages *pages = area->pages;	/* [한국어] 그 구간의 묶음 */

		if (!pages)	/* [한국어] 아직 세우는 중이면 */
			continue;	/* [한국어] 건너뛴다 */

		mutex_lock(&pages->mutex);	/* [한국어] 마지막 도메인이면 PFN 도 놓아야 한다 */
		interval_tree_remove(&area->pages_node, &pages->domains_itree);	/* [한국어] 더는 어느 도메인도 이 구간을 쓰지 않는다 */
		WARN_ON(area->storage_domain != domain);	/* [한국어] 마지막이니 저장용도 이 도메인이어야 한다 */
		area->storage_domain = NULL;	/* [한국어] 들고 있는 도메인이 없어졌다 */
		iopt_area_unfill_domain(area, pages, domain);	/* [한국어] 매핑과 함께 PFN 도 놓는다 */
		if (iopt_is_dmabuf(pages))	/* [한국어] dma-buf 면 */
			iopt_dmabuf_untrack_domain(pages, area, domain);	/* [한국어] 추적 기록도 */
		mutex_unlock(&pages->mutex);	/* [한국어] 묶음 보호 해제 */
	}
}

/**
 * iopt_fill_domain() - Fill a domain with PFNs
 * @iopt: io_pagetable to act on
 * @domain: domain to fill
 *
 * Fill the domain with PFNs from every area in the iopt. On failure the domain
 * is left unchanged.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_fill_domain - 새 도메인에 기존 매핑을 모두 채운다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 채울 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 도메인이 새로 붙을 때 부른다. 원 주석이 계약을 밝힌다 — 실패하면
 * 도메인은 손대지 않은 상태로 남는다.
 *
 * 그 되돌리기가 뒤쪽 반이다. 실패한 구간 앞까지를 역순이 아니라 처음부터
 * 다시 훑으며 걷어내는데, 구간 트리에는 역방향 순회가 없기 때문이다.
 *
 * storage_domain 을 여기서 정한다. 첫 도메인이면 그것이 PFN 을 들고
 * 있는 도메인이 되고, 그 사실을 pages 의 트리에도 기록한다.
 */
static int iopt_fill_domain(struct io_pagetable *iopt,
			    struct iommu_domain *domain)
{
	struct iopt_area *end_area;	/* [한국어] 실패한 지점을 기억한다 */
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */
	int rc;	/* [한국어] 결과 */

	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 구간을 훑으므로 */
	lockdep_assert_held_write(&iopt->domains_rwsem);	/* [한국어] 도메인이 들어오는 중이므로 */

	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 모든 구간을 */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] 하나씩 */
		struct iopt_pages *pages = area->pages;	/* [한국어] 그 구간의 묶음 */

		if (!pages)	/* [한국어] 아직 세우는 중이면 */
			continue;	/* [한국어] 건너뛴다 */

		guard(mutex)(&pages->mutex);	/* [한국어] 이 블록을 벗어날 때 저절로 풀린다 */
		if (iopt_is_dmabuf(pages)) {	/* [한국어] dma-buf 면 */
			rc = iopt_dmabuf_track_domain(pages, area, domain);	/* [한국어] 회수에 대비해 먼저 기록해 두고 */
			if (rc)	/* [한국어] 실패하면 */
				goto out_unfill;	/* [한국어] 앞까지를 되돌린다 */
		}
		rc = iopt_area_fill_domain(area, domain);	/* [한국어] 이 구간을 도메인에 채운다 */
		if (rc) {	/* [한국어] 실패하면 */
			if (iopt_is_dmabuf(pages))	/* [한국어] 방금 남긴 */
				iopt_dmabuf_untrack_domain(pages, area, domain);	/* [한국어] 기록을 지우고 */
			goto out_unfill;	/* [한국어] 앞까지를 되돌린다 */
		}
		if (!area->storage_domain) {	/* [한국어] 아직 PFN 을 든 도메인이 없으면 */
			WARN_ON(iopt->next_domain_id != 0);	/* [한국어] 그것은 첫 도메인일 때뿐이다 */
			area->storage_domain = domain;	/* [한국어] 이 도메인이 PFN 을 든다 */
			interval_tree_insert(&area->pages_node,	/* [한국어] 그 사실을 */
					     &pages->domains_itree);	/* [한국어] 묶음의 트리에도 기록한다 */
		}
	}
	return 0;	/* [한국어] 모두 채웠다 */

out_unfill:	/* [한국어] 실패한 구간 앞까지를 걷어낸다 */
	end_area = area;	/* [한국어] 실패한 구간 */
	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 처음부터 다시 훑는다 */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] 구간 트리에는 역방향 순회가 없다 */
		struct iopt_pages *pages = area->pages;	/* [한국어] 그 구간의 묶음 */

		if (area == end_area)	/* [한국어] 실패한 지점에 닿으면 */
			break;	/* [한국어] 그 앞까지만 되돌린다 */
		if (!pages)	/* [한국어] 아직 세우는 중이면 */
			continue;	/* [한국어] 건너뛴다 */
		mutex_lock(&pages->mutex);	/* [한국어] 묶음 보호 */
		if (iopt->next_domain_id == 0) {	/* [한국어] 첫 도메인이었으면 */
			interval_tree_remove(&area->pages_node,	/* [한국어] 방금 넣은 기록을 */
					     &pages->domains_itree);	/* [한국어] 지우고 */
			area->storage_domain = NULL;	/* [한국어] 저장용 표시도 되돌린다 */
		}
		iopt_area_unfill_domain(area, pages, domain);	/* [한국어] 채운 것을 걷어낸다 */
		if (iopt_is_dmabuf(pages))	/* [한국어] dma-buf 면 */
			iopt_dmabuf_untrack_domain(pages, area, domain);	/* [한국어] 추적 기록도 */
		mutex_unlock(&pages->mutex);	/* [한국어] 묶음 보호 해제 */
	}
	return rc;	/* [한국어] 실패 이유 — 도메인은 손대지 않은 상태다 */
}

/* All existing area's conform to an increased page size */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_check_iova_alignment - 기존 구간들이 새 정렬을 지키는지 본다
 *
 * @iopt: 대상 IOVA 공간.
 * @new_iova_alignment: 새로 요구될 정렬.
 * @return: 0 성공, -EADDRINUSE 면 어긋나는 구간이 있다.
 *
 * 도메인이 붙으면 정렬 요구가 커질 수 있는데, 이미 만들어진 구간이 그
 * 정렬을 지키지 않으면 그 도메인에 채울 수 없다. 그래서 붙이기 전에
 * 먼저 확인한다.
 *
 * 세 가지를 모두 본다 — 시작 IOVA, 길이, 그리고 페이지 안의 오프셋이다.
 * 마지막 것이 빠지면 IOVA 는 정렬되어도 실제 물리 주소가 어긋난다.
 */
static int iopt_check_iova_alignment(struct io_pagetable *iopt,
				     unsigned long new_iova_alignment)
{
	unsigned long align_mask = new_iova_alignment - 1;	/* [한국어] 정렬을 어겼는지 볼 마스크 */
	struct iopt_area *area;	/* [한국어] 순회 중인 구간 */

	lockdep_assert_held(&iopt->iova_rwsem);	/* [한국어] 구간을 훑으므로 */
	lockdep_assert_held(&iopt->domains_rwsem);	/* [한국어] 도메인 목록도 함께 본다 */

	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 모든 구간이 */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX))	/* [한국어] 새 정렬을 지키는지 */
		if ((iopt_area_iova(area) & align_mask) ||	/* [한국어] 시작 IOVA 와 */
		    (iopt_area_length(area) & align_mask) ||	/* [한국어] 길이와 */
		    (area->page_offset & align_mask))	/* [한국어] 페이지 안 오프셋 — 이것이 빠지면 물리 주소가 어긋난다 */
			return -EADDRINUSE;	/* [한국어] 어긋나는 구간이 있으면 그 도메인을 붙일 수 없다 */

	if (IS_ENABLED(CONFIG_IOMMUFD_TEST)) {	/* [한국어] 시험 빌드에서만 */
		struct iommufd_access *access;	/* [한국어] 커널 접근 */
		unsigned long index;	/* [한국어] xarray 인덱스 */

		xa_for_each(&iopt->access_list, index, access)	/* [한국어] 등록된 접근들의 요구가 */
			if (WARN_ON(access->iova_alignment >	/* [한국어] 새 정렬보다 크면 */
				    new_iova_alignment))	/* [한국어] 계산이 잘못된 것이다 */
				return -EADDRINUSE;	/* [한국어] 거절 */
	}
	return 0;	/* [한국어] 모두 지킨다 */
}

/*
 * [한국어]
 * iopt_table_add_domain - 이 IOVA 공간에 도메인을 더한다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 더할 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 장치를 IOAS 에 붙일 때 그 도메인이 이리로 온다. 세 가지를 맞춘다.
 *
 * 1) 정렬. 원 주석이 근거를 밝힌다 — iopt_pages 는 PAGE_SIZE 단위로
 *    동작하고 그보다 작은 것은 도메인에 넣을 때 조정하므로, 도메인이
 *    PAGE_SIZE 를 받아들이지 못하면 붙일 수 없다.
 * 2) 조리개(aperture). 도메인이 다룰 수 없는 IOVA 범위를 예약으로 막는다.
 * 3) 기존 매핑. 그 도메인에 모두 채워 넣는다.
 *
 * 실패 경로가 역순으로 되돌린다 — 예약, xarray 자리, 채운 매핑 순이다.
 */
int iopt_table_add_domain(struct io_pagetable *iopt,
			  struct iommu_domain *domain)
{
	const struct iommu_domain_geometry *geometry = &domain->geometry;	/* [한국어] 그 도메인이 다룰 수 있는 IOVA 범위 */
	struct iommu_domain *iter_domain;	/* [한국어] 중복 확인용 */
	unsigned int new_iova_alignment;	/* [한국어] 이 도메인이 요구하는 정렬 */
	unsigned long index;	/* [한국어] xarray 인덱스 */
	int rc;	/* [한국어] 결과 */

	down_write(&iopt->domains_rwsem);	/* [한국어] 도메인 목록을 고치므로 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 예약과 정렬도 고친다 */

	xa_for_each(&iopt->domains, index, iter_domain) {	/* [한국어] 이미 붙어 있는지 */
		if (WARN_ON(iter_domain == domain)) {	/* [한국어] 같은 도메인을 두 번 붙이면 */
			rc = -EEXIST;	/* [한국어] 코드 쪽 버그다 */
			goto out_unlock;	/* [한국어] 풀고 나간다 */
		}
	}

	/*
	 * The io page size drives the iova_alignment. Internally the iopt_pages
	 * works in PAGE_SIZE units and we adjust when mapping sub-PAGE_SIZE
	 * objects into the iommu_domain.
	 *
	 * A iommu_domain must always be able to accept PAGE_SIZE to be
	 * compatible as we can't guarantee higher contiguity.
	 */
	new_iova_alignment = max_t(unsigned long,	/* [한국어] (원 주석: iopt_pages 는 PAGE_SIZE 단위로 동작하고 그보다 작은 것은 도메인에 넣을 때 조정한다) */
				   1UL << __ffs(domain->pgsize_bitmap),	/* [한국어] 도메인의 최소 페이지와 */
				   iopt->iova_alignment);	/* [한국어] 지금 요구 중 큰 쪽 */
	if (new_iova_alignment > PAGE_SIZE) {	/* [한국어] (원 주석: 도메인은 늘 PAGE_SIZE 를 받아들여야 한다 — 더 큰 연속성을 보장할 수 없다) */
		rc = -EINVAL;	/* [한국어] 그러지 못하면 붙일 수 없다 */
		goto out_unlock;	/* [한국어] 풀고 나간다 */
	}
	if (new_iova_alignment != iopt->iova_alignment) {	/* [한국어] 요구가 달라졌으면 */
		rc = iopt_check_iova_alignment(iopt, new_iova_alignment);	/* [한국어] 기존 구간이 지키는지 */
		if (rc)	/* [한국어] 어긋나면 */
			goto out_unlock;	/* [한국어] 붙일 수 없다 */
	}

	/* No area exists that is outside the allowed domain aperture */
	if (geometry->aperture_start != 0) {	/* [한국어] (원 주석: 허용 조리개 밖에는 구간이 없어야 한다) */
		rc = iopt_reserve_iova(iopt, 0, geometry->aperture_start - 1,	/* [한국어] 그 앞쪽을 */
				       domain);	/* [한국어] 예약으로 막는다 */
		if (rc)	/* [한국어] 이미 매핑이 있으면 */
			goto out_reserved;	/* [한국어] 붙일 수 없다 */
	}
	if (geometry->aperture_end != ULONG_MAX) {	/* [한국어] 조리개 뒤쪽도 */
		rc = iopt_reserve_iova(iopt, geometry->aperture_end + 1,	/* [한국어] 같은 방식으로 */
				       ULONG_MAX, domain);	/* [한국어] 막는다 */
		if (rc)	/* [한국어] 이미 매핑이 있으면 */
			goto out_reserved;	/* [한국어] 붙일 수 없다 */
	}

	rc = xa_reserve(&iopt->domains, iopt->next_domain_id, GFP_KERNEL);	/* [한국어] 자리를 미리 잡아 둔다 — 채운 뒤에 실패하면 곤란하다 */
	if (rc)	/* [한국어] 메모리 부족 */
		goto out_reserved;	/* [한국어] 되돌린다 */

	rc = iopt_fill_domain(iopt, domain);	/* [한국어] 기존 매핑을 모두 채운다 */
	if (rc)	/* [한국어] 실패하면 */
		goto out_release;	/* [한국어] 잡아 둔 자리를 놓는다 */

	iopt->iova_alignment = new_iova_alignment;	/* [한국어] 이제부터 이 정렬을 요구한다 */
	xa_store(&iopt->domains, iopt->next_domain_id, domain, GFP_KERNEL);	/* [한국어] 목록에 넣고 */
	iopt->next_domain_id++;	/* [한국어] 다음 자리를 늘린다 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 성공 경로 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 두 락을 놓는다 */
	return 0;	/* [한국어] 붙었다 */
out_release:	/* [한국어] 잡아 둔 xarray 자리를 놓는다 */
	xa_release(&iopt->domains, iopt->next_domain_id);	/* [한국어] 잡아 둔 자리를 놓고 */
out_reserved:	/* [한국어] 이 도메인이 등록한 예약을 거둔다 */
	__iopt_remove_reserved_iova(iopt, domain);	/* [한국어] 이 도메인이 등록한 예약을 거둔다 */
out_unlock:	/* [한국어] 두 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 실패 경로도 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 두 락을 놓는다 */
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iopt_calculate_iova_alignment - 지금 조건에서 필요한 정렬을 다시 구한다
 *
 * @iopt: 대상 IOVA 공간.
 * @return: 0 성공, -EADDRINUSE 면 기존 구간이 새 정렬을 못 지킨다.
 *
 * 도메인이나 커널 접근이 오갈 때마다 요구 정렬이 달라진다. 모든 도메인과
 * 접근의 요구 중 가장 큰 것이 답이다.
 *
 * 큰 페이지를 막았으면 PAGE_SIZE 에서 시작한다 — 원 주석이 가리키는
 * batch_iommu_map_small() 이 그 단위로 매핑하기 때문이다.
 *
 * 정렬이 커질 때만 검사한다. 작아지는 것은 기존 구간에 문제가 되지 않는다.
 */
static int iopt_calculate_iova_alignment(struct io_pagetable *iopt)
{
	unsigned long new_iova_alignment;	/* [한국어] 다시 구한 정렬 */
	struct iommufd_access *access;	/* [한국어] 순회 중인 접근 */
	struct iommu_domain *domain;	/* [한국어] 순회 중인 도메인 */
	unsigned long index;	/* [한국어] xarray 인덱스 */

	lockdep_assert_held_write(&iopt->iova_rwsem);	/* [한국어] 정렬을 고치므로 */
	lockdep_assert_held(&iopt->domains_rwsem);	/* [한국어] 도메인 목록도 훑는다 */

	/* See batch_iommu_map_small() */
	if (iopt->disable_large_pages)	/* [한국어] (원 주석: batch_iommu_map_small() 참고) */
		new_iova_alignment = PAGE_SIZE;	/* [한국어] 그 경로가 페이지 단위로 매핑한다 */
	else
		new_iova_alignment = 1;	/* [한국어] 제한이 없으면 1 에서 시작 */

	xa_for_each(&iopt->domains, index, domain)	/* [한국어] 모든 도메인의 */
		new_iova_alignment = max_t(unsigned long,	/* [한국어] 최소 페이지 중 */
					   1UL << __ffs(domain->pgsize_bitmap),	/* [한국어] 가장 큰 것을 */
					   new_iova_alignment);	/* [한국어] 고른다 */
	xa_for_each(&iopt->access_list, index, access)	/* [한국어] 커널 접근들의 */
		new_iova_alignment = max_t(unsigned long,	/* [한국어] 요구도 */
					   access->iova_alignment,	/* [한국어] 함께 */
					   new_iova_alignment);	/* [한국어] 견준다 */

	if (new_iova_alignment > iopt->iova_alignment) {	/* [한국어] 요구가 커졌으면 */
		int rc;	/* [한국어] 확인 결과 */

		rc = iopt_check_iova_alignment(iopt, new_iova_alignment);	/* [한국어] 기존 구간이 지키는지 본다 */
		if (rc)	/* [한국어] 어긋나면 */
			return rc;	/* [한국어] 정렬을 바꿀 수 없다 */
	}
	iopt->iova_alignment = new_iova_alignment;	/* [한국어] 작아지는 쪽은 검사 없이 바꾼다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * iopt_table_remove_domain - 이 IOVA 공간에서 도메인을 뺀다
 *
 * @iopt: 대상 IOVA 공간.
 * @domain: 뺄 도메인.
 *
 * 순서가 정해져 있다. 먼저 배열에서 빼고(그래야 unfill 이 전제하는
 * 상태가 된다), 매핑을 걷어내고, 그 도메인이 등록한 예약을 거두고,
 * 마지막에 정렬을 다시 구한다.
 *
 * 원 주석이 배열 압축을 설명한다 — 지울 자리에 꼬리 항목을 옮기고 꼬리를
 * 줄여 배열을 조밀하게 유지한다. 도메인 순회가 next_domain_id 까지만
 * 도므로 중간에 구멍이 생기면 안 된다.
 */
void iopt_table_remove_domain(struct io_pagetable *iopt,
			      struct iommu_domain *domain)
{
	struct iommu_domain *iter_domain = NULL;	/* [한국어] 순회 중인 도메인 */
	unsigned long index;	/* [한국어] 그 자리 */

	down_write(&iopt->domains_rwsem);	/* [한국어] 도메인 목록을 고치므로 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 예약과 정렬도 고친다 */

	xa_for_each(&iopt->domains, index, iter_domain)	/* [한국어] 목록에서 */
		if (iter_domain == domain)	/* [한국어] 그 도메인을 찾는다 */
			break;	/* [한국어] 자리를 index 에 남긴 채 */
	if (WARN_ON(iter_domain != domain) || index >= iopt->next_domain_id)	/* [한국어] 없거나 범위 밖이면 */
		goto out_unlock;	/* [한국어] 코드 쪽 버그다 */

	/*
	 * Compress the xarray to keep it linear by swapping the entry to erase
	 * with the tail entry and shrinking the tail.
	 */
	iopt->next_domain_id--;	/* [한국어] (원 주석: 지울 자리에 꼬리를 옮기고 꼬리를 줄여 배열을 조밀하게 유지한다) */
	iter_domain = xa_erase(&iopt->domains, iopt->next_domain_id);	/* [한국어] 꼬리를 빼서 */
	if (index != iopt->next_domain_id)	/* [한국어] 지울 자리가 꼬리가 아니면 */
		xa_store(&iopt->domains, index, iter_domain, GFP_KERNEL);	/* [한국어] 그 자리에 옮겨 넣는다 */

	iopt_unfill_domain(iopt, domain);	/* [한국어] 매핑을 걷어낸다 — 배열에서 뺀 뒤여야 한다 */
	__iopt_remove_reserved_iova(iopt, domain);	/* [한국어] 조리개 예약도 거둔다 */

	WARN_ON(iopt_calculate_iova_alignment(iopt));	/* [한국어] 느슨해지는 방향이라 실패할 수 없다 */
out_unlock:	/* [한국어] 두 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 두 락을 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 놓는다 */
}

/**
 * iopt_area_split - Split an area into two parts at iova
 * @area: The area to split
 * @iova: Becomes the last of a new area
 *
 * This splits an area into two. It is part of the VFIO compatibility to allow
 * poking a hole in the mapping. The two areas continue to point at the same
 * iopt_pages, just with different starting bytes.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iopt_area_split - 구간을 그 지점에서 둘로 나눈다
 *
 * @area: 나눌 구간.
 * @iova: 이 주소가 앞 구간의 마지막이 된다.
 * @return: 0 성공, 음수면 나눌 수 없다.
 *
 * 원 주석이 존재 이유를 밝힌다 — VFIO 호환을 위해 매핑 가운데에 구멍을
 * 뚫을 수 있게 하려는 것이다. 두 구간은 같은 iopt_pages 를 가리키되
 * 시작 바이트만 달라진다.
 *
 * 나눌 수 없는 경우가 셋 있다.
 *  - 커널 접근이 있으면. 그 접근이 어느 쪽에 속하는지 추적할 정보가 없다.
 *  - 도메인에 큰 페이지로 매핑됐을 수 있으면. 그 페이지를 쪼갤 방법이 없다.
 *  - dma-buf 출처면. 원 주석대로 domains_itree 유지가 복잡해진다.
 *
 * 새 구간 둘을 먼저 만들고 원본을 트리에서 뺀 뒤 넣는 순서라, 중간에
 * 실패하면 원본을 되돌려 넣을 수 있다.
 */
static int iopt_area_split(struct iopt_area *area, unsigned long iova)
{
	unsigned long alignment = area->iopt->iova_alignment;	/* [한국어] 지켜야 할 정렬 */
	unsigned long last_iova = iopt_area_last_iova(area);	/* [한국어] 원본의 끝 */
	unsigned long start_iova = iopt_area_iova(area);	/* [한국어] 원본의 시작 */
	unsigned long new_start = iova + 1;	/* [한국어] 뒤 구간의 시작 */
	struct io_pagetable *iopt = area->iopt;	/* [한국어] 그 IOVA 공간 */
	struct iopt_pages *pages = area->pages;	/* [한국어] 두 구간이 공유할 묶음 */
	struct iopt_area *lhs;	/* [한국어] 앞 구간 */
	struct iopt_area *rhs;	/* [한국어] 뒤 구간 */
	int rc;	/* [한국어] 결과 */

	lockdep_assert_held_write(&iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */

	if (iova == start_iova || iova == last_iova)	/* [한국어] 이미 경계면 */
		return 0;	/* [한국어] 나눌 것이 없다 */

	if (!pages || area->prevent_access)	/* [한국어] 세우는 중이거나 정리 중이면 */
		return -EBUSY;	/* [한국어] 건드릴 수 없다 */

	/* Maintaining the domains_itree below is a bit complicated */
	if (iopt_is_dmabuf(pages))	/* [한국어] (원 주석: 아래의 domains_itree 유지가 복잡해진다) */
		return -EOPNOTSUPP;	/* [한국어] dma-buf 는 나누지 않는다 */

	if (new_start & (alignment - 1) ||	/* [한국어] 새 경계가 정렬을 어기거나 */
	    iopt_area_start_byte(area, new_start) & (alignment - 1))	/* [한국어] 그 자리의 페이지 오프셋이 어긋나면 */
		return -EINVAL;	/* [한국어] 도메인에 채울 수 없다 */

	lhs = iopt_area_alloc();	/* [한국어] 앞 구간을 잡고 */
	if (!lhs)	/* [한국어] 실패면 */
		return -ENOMEM;	/* [한국어] 거절 */

	rhs = iopt_area_alloc();	/* [한국어] 뒤 구간도 */
	if (!rhs) {	/* [한국어] 실패면 */
		rc = -ENOMEM;	/* [한국어] 앞 것을 */
		goto err_free_lhs;	/* [한국어] 버린다 */
	}

	mutex_lock(&pages->mutex);	/* [한국어] 아래 필드들은 이 락이 지킨다 */
	/*
	 * Splitting is not permitted if an access exists, we don't track enough
	 * information to split existing accesses.
	 */
	if (area->num_accesses) {	/* [한국어] (원 주석: 접근이 있으면 나눌 수 없다 — 그것을 나눌 정보가 없다) */
		rc = -EINVAL;	/* [한국어] 거절 */
		goto err_unlock;	/* [한국어] 풀고 나간다 */
	}

	/*
	 * Splitting is not permitted if a domain could have been mapped with
	 * huge pages.
	 */
	if (area->storage_domain && !iopt->disable_large_pages) {	/* [한국어] (원 주석: 도메인이 큰 페이지로 매핑했을 수 있으면 나눌 수 없다) */
		rc = -EINVAL;	/* [한국어] 그 페이지를 쪼갤 방법이 없다 */
		goto err_unlock;	/* [한국어] 풀고 나간다 */
	}

	interval_tree_remove(&area->node, &iopt->area_itree);	/* [한국어] 원본을 빼고 */
	rc = iopt_insert_area(iopt, lhs, area->pages, start_iova,	/* [한국어] 앞 구간을 */
			      iopt_area_start_byte(area, start_iova),	/* [한국어] 원본의 시작 오프셋으로 */
			      (new_start - 1) - start_iova + 1,	/* [한국어] 경계까지의 길이만큼 */
			      area->iommu_prot);	/* [한국어] 같은 권한으로 */
	if (WARN_ON(rc))	/* [한국어] 이미 있던 자리라 실패할 수 없다 */
		goto err_insert;	/* [한국어] 실패하면 원본을 되돌린다 */

	rc = iopt_insert_area(iopt, rhs, area->pages, new_start,	/* [한국어] 뒤 구간도 */
			      iopt_area_start_byte(area, new_start),	/* [한국어] 그 자리의 오프셋으로 */
			      last_iova - new_start + 1, area->iommu_prot);	/* [한국어] 나머지 길이만큼 */
	if (WARN_ON(rc))	/* [한국어] 역시 실패할 수 없다 */
		goto err_remove_lhs;	/* [한국어] 앞 구간도 빼고 원본을 되돌린다 */

	/*
	 * If the original area has filled a domain, domains_itree has to be
	 * updated.
	 */
	if (area->storage_domain) {	/* [한국어] (원 주석: 원본이 도메인을 채웠다면 domains_itree 도 갱신해야 한다) */
		interval_tree_remove(&area->pages_node, &pages->domains_itree);	/* [한국어] 원본의 기록을 빼고 */
		interval_tree_insert(&lhs->pages_node, &pages->domains_itree);	/* [한국어] 두 구간의 */
		interval_tree_insert(&rhs->pages_node, &pages->domains_itree);	/* [한국어] 기록을 넣는다 */
	}

	lhs->storage_domain = area->storage_domain;	/* [한국어] 앞 구간이 물려받고 */
	lhs->pages = area->pages;	/* [한국어] 같은 묶음을 가리킨다 */
	rhs->storage_domain = area->storage_domain;	/* [한국어] 뒤 구간도 */
	rhs->pages = area->pages;	/* [한국어] 같은 묶음 */
	kref_get(&rhs->pages->kref);	/* [한국어] 구간이 둘이 되었으므로 참조도 하나 더 */
	kfree(area);	/* [한국어] 원본 구조체를 버린다 */
	mutex_unlock(&pages->mutex);	/* [한국어] 묶음 보호 해제 */

	/*
	 * No change to domains or accesses because the pages hasn't been
	 * changed
	 */
	return 0;	/* [한국어] (원 주석: pages 가 바뀌지 않았으므로 도메인과 접근에는 변화가 없다) */

err_remove_lhs:	/* [한국어] 앞 구간을 빼고 원본을 되돌린다 */
	interval_tree_remove(&lhs->node, &iopt->area_itree);	/* [한국어] 앞 구간을 빼고 */
err_insert:	/* [한국어] 원본을 트리에 되돌려 넣는다 */
	interval_tree_insert(&area->node, &iopt->area_itree);	/* [한국어] 원본을 되돌려 넣는다 */
err_unlock:	/* [한국어] 묶음 락을 놓고 만든 것을 버린다 */
	mutex_unlock(&pages->mutex);	/* [한국어] 묶음 보호 해제 */
	kfree(rhs);	/* [한국어] 뒤 구간을 버리고 */
err_free_lhs:	/* [한국어] 앞 구간까지 버린다 */
	kfree(lhs);	/* [한국어] 앞 구간도 */
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iopt_cut_iova - 주어진 지점들에서 구간을 쪼갠다
 *
 * @iopt: 대상 IOVA 공간.
 * @iovas: 쪼갤 지점들.
 * @num_iovas: 그 개수.
 * @return: 0 성공, 음수면 어느 지점에서 실패.
 *
 * 그 지점에 구간이 없으면 조용히 넘어간다 — 이미 경계이거나 매핑이
 * 없다는 뜻이라 할 일이 없다.
 */
int iopt_cut_iova(struct io_pagetable *iopt, unsigned long *iovas,
		  size_t num_iovas)
{
	int rc = 0;	/* [한국어] 결과 */
	int i;	/* [한국어] 지점 인덱스 */

	down_write(&iopt->iova_rwsem);	/* [한국어] 트리를 고치므로 */
	for (i = 0; i < num_iovas; i++) {	/* [한국어] 지점마다 */
		struct iopt_area *area;	/* [한국어] 그 자리의 구간 */

		area = iopt_area_iter_first(iopt, iovas[i], iovas[i]);	/* [한국어] 그 주소를 담는 구간 */
		if (!area)	/* [한국어] 없으면 */
			continue;	/* [한국어] 이미 경계이거나 매핑이 없다 */
		rc = iopt_area_split(area, iovas[i]);	/* [한국어] 그 지점에서 나눈다 */
		if (rc)	/* [한국어] 실패하면 */
			break;	/* [한국어] 멈춘다 */
	}
	up_write(&iopt->iova_rwsem);	/* [한국어] 끝 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_enable_large_pages - 큰 페이지를 다시 허용한다
 *
 * @iopt: 대상 IOVA 공간.
 *
 * 이후 만들어지는 매핑부터 큰 페이지를 쓸 수 있다. 정렬 계산이 실패할
 * 수 없는 방향이라 경고만 한다 — 요구가 느슨해지는 쪽이기 때문이다.
 */
void iopt_enable_large_pages(struct io_pagetable *iopt)
{
	int rc;	/* [한국어] 결과 */

	down_write(&iopt->domains_rwsem);	/* [한국어] 정렬 계산이 도메인 목록을 훑는다 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 정렬을 고치므로 */
	WRITE_ONCE(iopt->disable_large_pages, false);	/* [한국어] 이후 매핑부터 큰 페이지를 쓴다 */
	rc = iopt_calculate_iova_alignment(iopt);	/* [한국어] 요구가 느슨해진다 */
	WARN_ON(rc);	/* [한국어] 그 방향은 실패할 수 없다 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 두 락을 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 놓는다 */
}

/*
 * [한국어]
 * iopt_disable_large_pages - 큰 페이지를 막는다
 *
 * @iopt: 대상 IOVA 공간.
 * @return: 0 성공, -EINVAL 이면 이미 매핑이 있다.
 *
 * 도메인에 이미 매핑이 들어가 있으면 거절한다 — 원 주석대로 그것들이
 * 큰 페이지로 들어갔을 수 있고, 이제 와서 쪼갤 방법이 없다.
 *
 * 정렬 계산이 실패하면 플래그를 되돌린다. 이 방향은 요구가 커지는 쪽이라
 * 기존 구간이 못 지킬 수 있다.
 */
int iopt_disable_large_pages(struct io_pagetable *iopt)
{
	int rc = 0;	/* [한국어] 결과 */

	down_write(&iopt->domains_rwsem);	/* [한국어] 정렬 계산이 도메인 목록을 훑는다 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 정렬을 고치므로 */
	if (iopt->disable_large_pages)	/* [한국어] 이미 막혀 있으면 */
		goto out_unlock;	/* [한국어] 할 일이 없다 */

	/* Won't do it if domains already have pages mapped in them */
	if (!xa_empty(&iopt->domains) &&	/* [한국어] (원 주석: 도메인에 이미 페이지가 매핑되어 있으면 하지 않는다) */
	    !RB_EMPTY_ROOT(&iopt->area_itree.rb_root)) {	/* [한국어] 그것들이 큰 페이지로 들어갔을 수 있다 */
		rc = -EINVAL;	/* [한국어] 이제 와서 쪼갤 방법이 없다 */
		goto out_unlock;	/* [한국어] 거절 */
	}

	WRITE_ONCE(iopt->disable_large_pages, true);	/* [한국어] 막고 */
	rc = iopt_calculate_iova_alignment(iopt);	/* [한국어] 정렬을 다시 구한다 */
	if (rc)	/* [한국어] 기존 구간이 못 지키면 */
		WRITE_ONCE(iopt->disable_large_pages, false);	/* [한국어] 되돌린다 */
out_unlock:	/* [한국어] 두 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 두 락을 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_add_access - 커널 쪽 접근을 이 공간에 등록한다
 *
 * @iopt: 대상 IOVA 공간.
 * @access: 등록할 접근.
 * @return: 0 성공, 음수면 실패.
 *
 * 접근마다 정렬 요구가 있어, 등록하면 이 공간의 정렬이 커질 수 있다.
 * 기존 구간이 그것을 못 지키면 등록 자체가 거절된다.
 */
int iopt_add_access(struct io_pagetable *iopt, struct iommufd_access *access)
{
	u32 new_id;	/* [한국어] 배정받을 자리 */
	int rc;	/* [한국어] 결과 */

	down_write(&iopt->domains_rwsem);	/* [한국어] 정렬 계산이 도메인 목록을 훑는다 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 접근 목록과 정렬을 고친다 */
	rc = xa_alloc(&iopt->access_list, &new_id, access, xa_limit_16b,	/* [한국어] 접근 목록에 넣고 */
		      GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 시킨 것이라 계상한다 */

	if (rc)	/* [한국어] 실패면 */
		goto out_unlock;	/* [한국어] 풀고 나간다 */

	rc = iopt_calculate_iova_alignment(iopt);	/* [한국어] 이 접근의 정렬 요구를 반영한다 */
	if (rc) {	/* [한국어] 기존 구간이 못 지키면 */
		xa_erase(&iopt->access_list, new_id);	/* [한국어] 등록을 되돌리고 */
		goto out_unlock;	/* [한국어] 거절 */
	}
	access->iopt_access_list_id = new_id;	/* [한국어] 거둘 때 이 자리를 쓴다 */

out_unlock:	/* [한국어] 두 락을 놓고 나가는 공통 경로 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 두 락을 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iopt_remove_access - 그 등록을 거둔다
 *
 * @iopt: 대상 IOVA 공간.
 * @access: 거둘 접근.
 * @iopt_access_list_id: 등록할 때 받은 자리.
 *
 * 정렬이 느슨해지는 방향이라 계산은 실패할 수 없고, 경고만 둔다.
 */
void iopt_remove_access(struct io_pagetable *iopt,
			struct iommufd_access *access, u32 iopt_access_list_id)
{
	down_write(&iopt->domains_rwsem);	/* [한국어] 정렬 계산이 도메인 목록을 훑는다 */
	down_write(&iopt->iova_rwsem);	/* [한국어] 접근 목록과 정렬을 고친다 */
	WARN_ON(xa_erase(&iopt->access_list, iopt_access_list_id) != access);	/* [한국어] 다른 것이 나오면 자리가 어긋난 것이다 */
	WARN_ON(iopt_calculate_iova_alignment(iopt));	/* [한국어] 느슨해지는 방향이라 실패할 수 없다 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 두 락을 */
	up_write(&iopt->domains_rwsem);	/* [한국어] 놓는다 */
}

/* Narrow the valid_iova_itree to include reserved ranges from a device. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_table_enforce_dev_resv_regions - 장치의 예약 구간을 이 공간에 반영한다
 *
 * @iopt: 대상 IOVA 공간.
 * @dev: 붙이려는 장치.
 * @sw_msi_start: 소프트웨어 MSI 창의 시작을 돌려준다.
 * @return: 0 성공, 음수면 그 장치를 이 IOAS 에 붙일 수 없다.
 *
 * 장치를 붙일 때 그 장치가 쓸 수 없다고 못박은 IOVA 범위를 예약으로
 * 등록한다. 이미 그 자리에 매핑이 있으면 붙일 수 없다.
 *
 * RESV_DIRECT_RELAXABLE 을 건너뛰는 이유: 그 종류는 "그렇게 해 주면
 * 좋지만 없어도 된다"는 뜻이라 IOVA 를 막을 이유가 없다.
 *
 * MSI 종류가 두 가지인 것이 미묘하다. 하드웨어 MSI 는 IOMMU 를 거치지
 * 않아 그냥 막으면 되고, 소프트웨어 MSI 는 커널이 그 창 안에 도어벨을
 * 매핑해야 한다(driver.c 참고). 그 둘이 함께 있거나 후자가 여럿이면
 * 드라이버가 잘못 보고한 것이라 원 주석대로 거절한다.
 *
 * 원 주석의 FIXME 가 남아 있다 — 드라이버가 예약 구간을 만들다 메모리
 * 부족을 만나도 그 실패가 전해지지 않는다.
 */
int iopt_table_enforce_dev_resv_regions(struct io_pagetable *iopt,
					struct device *dev,
					phys_addr_t *sw_msi_start)
{
	struct iommu_resv_region *resv;	/* [한국어] 순회 중인 예약 구간 */
	LIST_HEAD(resv_regions);	/* [한국어] 드라이버가 채워 줄 목록 */
	unsigned int num_hw_msi = 0;	/* [한국어] 하드웨어 MSI 구간의 개수 */
	unsigned int num_sw_msi = 0;	/* [한국어] 소프트웨어 MSI 구간의 개수 */
	int rc;	/* [한국어] 결과 */

	if (iommufd_should_fail())	/* [한국어] 시험이 지정한 실패 지점이면 */
		return -EINVAL;	/* [한국어] 오류 처리 경로를 태운다 */

	down_write(&iopt->iova_rwsem);	/* [한국어] 예약 트리를 고치므로 */
	/* FIXME: drivers allocate memory but there is no failure propogated */
	iommu_get_resv_regions(dev, &resv_regions);	/* [한국어] (원 주석의 FIXME: 드라이버가 메모리를 잡다 실패해도 그것이 전해지지 않는다) */

	list_for_each_entry(resv, &resv_regions, list) {	/* [한국어] 그 구간들을 하나씩 */
		if (resv->type == IOMMU_RESV_DIRECT_RELAXABLE)	/* [한국어] "있으면 좋지만 없어도 된다"는 종류는 */
			continue;	/* [한국어] IOVA 를 막을 이유가 없다 */

		if (sw_msi_start && resv->type == IOMMU_RESV_MSI)	/* [한국어] 하드웨어 MSI 창이면 */
			num_hw_msi++;	/* [한국어] 세어 둔다 — IOMMU 를 거치지 않아 그냥 막으면 된다 */
		if (sw_msi_start && resv->type == IOMMU_RESV_SW_MSI) {	/* [한국어] 소프트웨어 MSI 창이면 */
			*sw_msi_start = resv->start;	/* [한국어] 커널이 그 안에 도어벨을 매핑해야 한다 */
			num_sw_msi++;	/* [한국어] 세어 둔다 */
		}

		rc = iopt_reserve_iova(iopt, resv->start,	/* [한국어] 그 범위를 */
				       resv->length - 1 + resv->start, dev);	/* [한국어] 예약으로 막는다 */
		if (rc)	/* [한국어] 이미 매핑이 있으면 */
			goto out_reserved;	/* [한국어] 이 장치를 붙일 수 없다 */
	}

	/* Drivers must offer sane combinations of regions */
	if (WARN_ON(num_sw_msi && num_hw_msi) || WARN_ON(num_sw_msi > 1)) {	/* [한국어] (원 주석: 드라이버는 말이 되는 조합을 내야 한다) */
		rc = -EINVAL;	/* [한국어] 두 종류가 함께 있거나 후자가 여럿이면 보고가 잘못된 것이다 */
		goto out_reserved;	/* [한국어] 거절 */
	}

	rc = 0;	/* [한국어] 모두 반영했다 */
	goto out_free_resv;	/* [한국어] 목록만 놓고 나간다 */

out_reserved:	/* [한국어] 이 장치가 등록한 예약을 거둔다 */
	__iopt_remove_reserved_iova(iopt, dev);	/* [한국어] 이 장치가 등록한 예약을 거두고 */
out_free_resv:	/* [한국어] 드라이버가 만든 목록을 놓는다 */
	iommu_put_resv_regions(dev, &resv_regions);	/* [한국어] 드라이버가 만든 목록을 놓는다 */
	up_write(&iopt->iova_rwsem);	/* [한국어] 끝 */
	return rc;	/* [한국어] 성패 */
}
