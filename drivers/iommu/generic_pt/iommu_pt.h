/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * "Templated C code" for implementing the iommu operations for page tables.
 * This is compiled multiple times, over all the page table formats to pick up
 * the per-format definitions.
 */
/*
 * [한국어 설명] 페이지 테이블을 IOMMU 도메인 연산으로 감싸는 템플릿 (iommu_pt.h)
 *
 * === 파일의 역할 ===
 * generic_pt 의 가장 바깥층이다. 순회기(pt_iter.h)와 형식 API(pt_common.h)
 * 위에 iommu 코어가 부르는 연산 — map, unmap, iova_to_phys, 더티 추적,
 * 초기화와 해제 — 를 얹는다.
 *
 * 원 주석대로 이 파일도 형식마다 다시 컴파일된다. 그래서 여기 있는 코드는
 * 형식을 컴파일 시에 알고 있고, 단계 수·항목 크기·마스크가 전부 상수로
 * 접힌다. 외부 심볼은 DOMAIN_NS/NS 매크로가 형식별 접두어를 붙여 만든다.
 *
 * 이 계층이 풀어야 하는 어려운 문제가 셋 있다.
 *
 * 1) 페이지 크기 선택. 하나의 map 요청을 가장 적은 수의 잎으로 덮으려면
 *    VA·OA 정렬과 남은 길이를 함께 보아야 한다(compute_best_pgsize).
 *
 * 2) 락 없는 표 만들기. 표를 새로 꽂는 일은 cmpxchg 로 하고, 경합에서 진
 *    쪽이 자기 표를 버리고 다시 읽는다. 그 되돌림을 pt_iommu_new_table 이
 *    맡는다.
 *
 * 3) 무효화 전에 메모리를 돌려주지 않기. unmap 이 떼어 낸 표는 곧바로
 *    해제하지 않고 목록에 모아 두었다가, iotlb_gather 가 무효화를 마친 뒤
 *    코어가 해제한다 — 그 사이에 하드웨어가 그 메모리를 표로 읽으면 안 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IOMMU 드라이버(amd/intel/riscv) → 이 파일이 만든 pt_iommu_<형식>_* 진입점
 *   → pt_iter.h 순회 → 형식 헤더의 항목 접근자 → 하드웨어가 읽는 표
 *
 * 실행 컨텍스트: map/unmap 은 드라이버가 잡은 범위 락 아래에서 불린다.
 * 이 계층은 그 락을 스스로 잡지 않고, 각 함수의 kdoc 이 요구 조건을 밝힌다.
 *
 * === 타 모듈과의 연결 ===
 * 위: <linux/generic_pt/iommu.h> 가 선언한 진입점들, iommu 코어의
 *     iommu_domain_ops.
 * 아래: pt_iter.h, ../iommu-pages.h(표 메모리 할당과 비일관 플러시),
 *       <linux/dma-mapping.h>.
 *
 * 데이터 흐름: 코어의 (iova, paddr, size) → struct pt_range → 단계별 순회
 *   → 잎 설치 → 무효화 정보를 iotlb_gather 에 쌓아 코어로 돌려준다.
 *
 * === 주요 함수/구조체 요약 ===
 * DOMAIN_NS(iova_to_phys): 한 주소를 따라 내려가 출력 주소를 돌려준다.
 * NS(map_range): 범위를 잎으로 채운다. 최상위가 모자라면 increase_top 이
 *   단계를 하나 얹고 다시 시도한다.
 * NS(unmap_range): 잎을 지우고 빈 표를 모아 free_list 로 넘긴다.
 * DOMAIN_NS(read_and_clear_dirty): 더티 비트를 읽어 비트맵에 옮기고 지운다.
 * pt_iommu_new_table / clear_contig: 락 없는 표 설치와, 큰 페이지를 쪼갤
 *   때 기존 연속 항목을 걷어내는 처리.
 * increase_top: 주소 공간이 커질 때 최상위 단계를 하나 얹는다.
 * pt_iommu_init / NS(deinit): 인스턴스의 생성과 해제.
 */
#ifndef __GENERIC_PT_IOMMU_PT_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_IOMMU_PT_H	/* [한국어] 같은 이름으로 표시 */

#include "pt_iter.h"	/* [한국어] 순회기와 형식 API */

#include <linux/export.h>	/* [한국어] 형식별 이름으로 심볼을 내보낸다 */
#include <linux/iommu.h>	/* [한국어] iommu_domain, iotlb_gather 등 */
#include "../iommu-pages.h"	/* [한국어] 표 메모리 할당과 비일관 캐시 처리 */
#include <linux/cleanup.h>	/* [한국어] __free 스코프 정리 */
#include <linux/dma-mapping.h>	/* [한국어] 비일관 플랫폼의 DMA 매핑 */

/*
 * [한국어] 이 계층이 쓰는 소프트웨어 비트 번호.
 * 형식이 제공하는 "하드웨어가 무시하는 비트"에 붙이는 뜻이다.
 */
enum {
	SW_BIT_CACHE_FLUSH_DONE = 0,	/* [한국어] 이 표의 캐시 플러시가 끝났다는 표시. 획득·해제 순서가 붙어 있어 다른 CPU 가 그것을 보면 표 내용도 본다 */
};

/*
 * [한국어]
 * flush_writes_range - 표의 일정 구간을 캐시에서 메모리로 밀어낸다
 *
 * @pts: 대상 표를 가리키는 순회 상태.
 * @start_index: 시작 항목.
 * @end_index: 마지막 다음 항목.
 *
 * 페이지 테이블이 CPU 캐시와 일관되지 않는 플랫폼을 위한 것이다. 그런
 * 하드웨어는 표를 DMA 로 읽으므로, CPU 가 쓴 내용이 캐시에만 있으면
 * 옛 값을 본다.
 *
 * 그 기능이 꺼진 형식에서는 조건이 컴파일 시 거짓이 되어 통째로 사라진다.
 */
static void flush_writes_range(const struct pt_state *pts,
			       unsigned int start_index, unsigned int end_index)
{
	if (pts_feature(pts, PT_FEAT_DMA_INCOHERENT))	/* [한국어] 표가 CPU 캐시와 일관되지 않는 플랫폼이면 */
		iommu_pages_flush_incoherent(	/* [한국어] 캐시에만 있는 내용을 메모리로 밀어낸다 */
			iommu_from_common(pts->range->common)->iommu_device,	/* [한국어] DMA 방향을 아는 장치 */
			pts->table, start_index * PT_ITEM_WORD_SIZE,	/* [한국어] 표에서의 바이트 오프셋 */
			(end_index - start_index) * PT_ITEM_WORD_SIZE);	/* [한국어] 밀어낼 바이트 수 */
}

/*
 * [한국어]
 * flush_writes_item - 표의 항목 하나를 캐시에서 밀어낸다
 *
 * @pts: 대상 항목을 가리키는 순회 상태.
 *
 * 위 함수의 한 항목짜리 판이다. 잎 하나를 고친 뒤 부른다.
 */
static void flush_writes_item(const struct pt_state *pts)
{
	if (pts_feature(pts, PT_FEAT_DMA_INCOHERENT))	/* [한국어] 비일관 플랫폼이면 */
		iommu_pages_flush_incoherent(	/* [한국어] 항목 하나만 */
			iommu_from_common(pts->range->common)->iommu_device,	/* [한국어] DMA 방향을 아는 장치 */
			pts->table, pts->index * PT_ITEM_WORD_SIZE,	/* [한국어] 그 항목의 오프셋 */
			PT_ITEM_WORD_SIZE);	/* [한국어] 항목 하나 크기 */
}

/*
 * [한국어]
 * gather_range_pages - unmap 이 떼어 낸 범위와 페이지를 코어에 넘긴다
 *
 * @iotlb_gather: 코어가 무효화 범위를 모으는 자리.
 * @iommu_table: 대상 페이지 테이블.
 * @iova: 떼어 낸 범위의 시작.
 * @len: 그 길이.
 * @free_list: 이번에 비워진 표들.
 *
 * unmap 의 마무리다. 두 가지를 코어에 넘긴다 — 무효화해야 할 주소 범위와,
 * 무효화가 끝난 뒤에야 해제해도 되는 표 메모리.
 *
 * 중간의 분기가 성능 판단이다. 원 주석이 근거를 든다.
 *  - DMA-FQ 모드면 어차피 뒤에 전체 플러시가 따라오므로 여기서 범위를
 *    쌓을 이유가 없다.
 *  - NO_GAPS 형식은 범위를 넓힐 수 없으므로, 새 범위가 기존 것과 떨어져
 *    있으면 먼저 있던 것을 내보내야 한다. 그 선택의 이해득실은 사용자가
 *    DMA 와 DMA-FQ 중 무엇을 쓰느냐로 정한다.
 *
 * 그 sync 가 gather 의 free_list 를 비운다는 점을 원 주석이 경고한다 —
 * 그래서 이번 범위에 걸친 페이지를 그 목록에 미리 얹어 두면 안 된다.
 */
static void gather_range_pages(struct iommu_iotlb_gather *iotlb_gather,
			       struct pt_iommu *iommu_table, pt_vaddr_t iova,
			       pt_vaddr_t len,
			       struct iommu_pages_list *free_list)
{
	struct pt_common *common = common_from_iommu(iommu_table);	/* [한국어] 기능 질의를 위해 */

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))	/* [한국어] 비일관 플랫폼이면 */
		iommu_pages_stop_incoherent_list(free_list,	/* [한국어] 해제 전에 그 페이지들의 DMA 매핑을 푼다 */
						 iommu_table->iommu_device);	/* [한국어] 그 매핑을 만든 장치 */

	/*
	 * If running in DMA-FQ mode then the unmap will be followed by an IOTLB
	 * flush all so we need to optimize by never flushing the IOTLB here.
	 *
	 * For NO_GAPS the user gets to pick if flushing all or doing micro
	 * flushes is better for their work load by choosing DMA vs DMA-FQ
	 * operation. Drivers should also see shadow_on_flush.
	 */
	if (!iommu_iotlb_gather_queued(iotlb_gather)) {	/* [한국어] (원 주석: DMA-FQ 면 뒤에 전체 플러시가 따라오므로 여기서 쌓지 않는다) */
		if (pt_feature(common, PT_FEAT_FLUSH_RANGE_NO_GAPS) &&	/* [한국어] 범위를 넓힐 수 없는 형식이고 */
		    iommu_iotlb_gather_is_disjoint(iotlb_gather, iova, len)) {	/* [한국어] 새 범위가 기존 것과 떨어져 있으면 */
			iommu_iotlb_sync(&iommu_table->domain, iotlb_gather);	/* [한국어] 먼저 있던 것을 내보낸다 */
			/*
			 * Note that the sync frees the gather's free list, so
			 * we must not have any pages on that list that are
			 * covered by iova/len
			 */
		}
		iommu_iotlb_gather_add_range(iotlb_gather, iova, len);	/* [한국어] (원 주석: sync 가 gather 의 free_list 를 비우므로 이번 범위의 페이지를 미리 얹으면 안 된다) */
	}

	iommu_pages_list_splice(free_list, &iotlb_gather->freelist);	/* [한국어] 무효화가 끝난 뒤 코어가 해제한다 */
}

#define DOMAIN_NS(op) CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), op)	/* [한국어] 도메인 연산 이름에 형식별 접두어를 붙인다 */

/*
 * [한국어]
 * make_range_ul - unsigned long 인자로 순회 범위를 만든다
 *
 * @common: 페이지 테이블 인스턴스.
 * @range: 채울 범위.
 * @iova: 시작 주소.
 * @len: 길이.
 * @return: 0 성공, -EINVAL 길이 0, -EOVERFLOW 넘침.
 *
 * 길이 0 을 거절하는 이유: 범위를 시작과 마지막 주소로 표현하므로 빈
 * 범위를 나타낼 방법이 없다.
 *
 * 마지막의 되읽기 검사는 주소 폭이 좁은 형식을 위한 것이다. 32비트
 * 페이지 테이블에 64비트 주소를 넣으면 잘려 들어가는데, 다시 읽어 비교하면
 * 그 절단이 드러난다.
 */
static int make_range_ul(struct pt_common *common, struct pt_range *range,
			 unsigned long iova, unsigned long len)
{
	unsigned long last;	/* [한국어] 마지막 주소(포함) */

	if (unlikely(len == 0))	/* [한국어] 빈 범위는 */
		return -EINVAL;	/* [한국어] 시작·마지막 표현으로 나타낼 수 없다 */

	if (check_add_overflow(iova, len - 1, &last))	/* [한국어] 주소 공간 끝을 넘으면 */
		return -EOVERFLOW;	/* [한국어] 호출자에게 */

	*range = pt_make_range(common, iova, last);	/* [한국어] 최상위 정보를 담은 범위로 */
	if (sizeof(iova) > sizeof(range->va)) {	/* [한국어] 형식의 주소 폭이 더 좁으면 */
		if (unlikely(range->va != iova || range->last_va != last))	/* [한국어] 되읽어 비교해 */
			return -EOVERFLOW;	/* [한국어] 절단이 있었는지 드러낸다 */
	}
	return 0;	/* [한국어] 유효한 범위 */
}

/*
 * [한국어]
 * make_range_u64 - 64비트 인자로 순회 범위를 만든다
 *
 * @common: 페이지 테이블 인스턴스.
 * @range: 채울 범위.
 * @iova: 시작 주소.
 * @len: 길이.
 * @return: 0 성공, -EOVERFLOW 넘침.
 *
 * 32비트 커널에서 dma_addr_t 가 unsigned long 보다 넓을 수 있다. 그 경우
 * 먼저 범위를 확인하고 좁은 판으로 넘긴다.
 *
 * __maybe_unused 인 이유: 아래 매크로가 타입을 보고 한쪽만 고르므로,
 * 형식에 따라 이 함수가 전혀 쓰이지 않을 수 있다.
 */
static __maybe_unused int make_range_u64(struct pt_common *common,
					 struct pt_range *range, u64 iova,
					 u64 len)
{
	if (unlikely(iova > ULONG_MAX || len > ULONG_MAX))	/* [한국어] 32비트 커널에서 넓은 값이 오면 */
		return -EOVERFLOW;	/* [한국어] 좁은 판으로 넘길 수 없다 */
	return make_range_ul(common, range, iova, len);	/* [한국어] 확인했으니 좁은 판으로 */
}

/*
 * Some APIs use unsigned long, while othersuse dma_addr_t as the type. Dispatch
 * to the correct validation based on the type.
 */
#define make_range_no_check(common, range, iova, len)                   \
	({                                                              \
		int ret;                                                \
		if (sizeof(iova) > sizeof(unsigned long) ||             \
		    sizeof(len) > sizeof(unsigned long))                \
			ret = make_range_u64(common, range, iova, len); \
		else                                                    \
			ret = make_range_ul(common, range, iova, len);  \
		ret;                                                    \
	})

#define make_range(common, range, iova, len)                             \
	({                                                               \
		int ret = make_range_no_check(common, range, iova, len); \
		if (!ret)                                                \
			ret = pt_check_range(range);                     \
		ret;                                                     \
	})

/*
 * [한국어]
 * compute_best_pgsize - 이 자리에 쓸 가장 큰 페이지 크기를 고른다
 *
 * @pts: 순회 상태(현재 단계와 VA 범위).
 * @oa: 대응하는 출력 주소.
 * @return: 크기의 지수, 놓을 수 없으면 0.
 *
 * pt_iter.h 의 pt_compute_best_pgsize 를 감싸되, 후보 크기를 도메인의
 * 비트맵으로 한 번 더 좁힌다.
 *
 * 그 교집합이 필요한 이유를 원 주석이 밝힌다 — 코어가 그 비트맵을 줄여
 * 드라이버가 쓸 수 있는 페이지 크기를 제한할 수 있다. 형식이 1GB 를
 * 만들 수 있어도 코어가 막으면 쓰지 않는다.
 */
static inline unsigned int compute_best_pgsize(struct pt_state *pts,
					       pt_oaddr_t oa)
{
	struct pt_iommu *iommu_table = iommu_from_common(pts->range->common);	/* [한국어] 도메인의 크기 비트맵을 얻기 위해 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return 0;	/* [한국어] 더 내려가야 한다 */

	/*
	 * The page size is limited by the domain's bitmap. This allows the core
	 * code to reduce the supported page sizes by changing the bitmap.
	 */
	return pt_compute_best_pgsize(pt_possible_sizes(pts) &	/* [한국어] (원 주석: 코어가 비트맵을 줄여 지원 크기를 제한할 수 있다) */
					      iommu_table->domain.pgsize_bitmap,	/* [한국어] 형식이 만들 수 있어도 코어가 막으면 쓰지 않는다 */
				      pts->range->va, pts->range->last_va, oa);	/* [한국어] 정렬과 남은 길이를 함께 본다 */
}

/*
 * [한국어]
 * __do_iova_to_phys - 한 단계에서 주소를 따라 내려가는 워커
 *
 * @range: 걷는 범위.
 * @arg: 결과를 담을 pt_oaddr_t 포인터.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @descend_fn: 아래 단계 워커.
 * @return: 0 성공, -ENOENT 매핑 없음.
 *
 * 한 항목만 보고 종류에 따라 갈린다 — 비었으면 매핑이 없고, 표면 내려가고,
 * 주소면 거기서 끝난다.
 *
 * pt_entry_oa_exact 를 쓰는 이유: 큰 페이지의 한가운데 주소를 물었을 수
 * 있어, entry 시작 주소에 그 오프셋을 더해야 한다.
 *
 * 마지막 return 은 컴파일러를 위한 것이다 — switch 가 모든 값을 덮지만
 * enum 밖의 값이 올 수 있다고 보기 때문이다.
 */
static __always_inline int __do_iova_to_phys(struct pt_range *range, void *arg,
					     unsigned int level,
					     struct pt_table_p *table,
					     pt_level_fn_t descend_fn)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	pt_oaddr_t *res = arg;	/* [한국어] 결과를 담을 자리 */

	switch (pt_load_single_entry(&pts)) {	/* [한국어] 한 항목만 읽는다 — 반복문이 필요 없다 */
	case PT_ENTRY_EMPTY:	/* [한국어] 매핑이 없으면 */
		return -ENOENT;	/* [한국어] 호출자가 0 으로 바꾼다 */
	case PT_ENTRY_TABLE:	/* [한국어] 아래 표를 가리키면 */
		return pt_descend(&pts, arg, descend_fn);	/* [한국어] 한 단계 내려간다 */
	case PT_ENTRY_OA:	/* [한국어] 잎에 닿았으면 */
		*res = pt_entry_oa_exact(&pts);	/* [한국어] 큰 페이지 한가운데일 수 있어 오프셋까지 더한다 */
		return 0;	/* [한국어] 찾았다 */
	}
	return -ENOENT;	/* [한국어] enum 밖의 값에 대비한 컴파일러용 경로 */
}
PT_MAKE_LEVELS(__iova_to_phys, __do_iova_to_phys);

/**
 * iova_to_phys() - Return the output address for the given IOVA
 * @domain: Table to query
 * @iova: IO virtual address to query
 *
 * Determine the output address from the given IOVA. @iova may have any
 * alignment, the returned physical will be adjusted with any sub page offset.
 *
 * Context: The caller must hold a read range lock that includes @iova.
 *
 * Return: 0 if there is no translation for the given iova.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iova_to_phys - IOVA 에 대응하는 물리 주소를 돌려준다
 *
 * @domain: 조회할 도메인.
 * @iova: 물어볼 IO 가상 주소.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 코어의 iommu_iova_to_phys() 가 부르는 진입점이다. 정렬을 요구하지 않고,
 * 페이지 안의 오프셋까지 반영해 돌려준다.
 *
 * 오류를 0 으로 뭉개는 것을 원 주석이 아쉬워한다 — PHYS_ADDR_MAX 가 더
 * 나은 표현이겠지만, 코어 API 가 0 을 "없음"으로 정해 두었다.
 *
 * 실행 컨텍스트: 호출자가 그 주소를 포함하는 읽기 범위 락을 쥐고 있어야
 * 한다(원 주석).
 */
phys_addr_t DOMAIN_NS(iova_to_phys)(struct iommu_domain *domain,
				    dma_addr_t iova)
{
	struct pt_iommu *iommu_table =	/* [한국어] 공용 도메인에서 */
		container_of(domain, struct pt_iommu, domain);	/* [한국어] 이 계층의 객체로 */
	struct pt_range range;	/* [한국어] 걸을 범위 */
	pt_oaddr_t res;	/* [한국어] 찾은 출력 주소 */
	int ret;	/* [한국어] 결과 */

	ret = make_range(common_from_iommu(iommu_table), &range, iova, 1);	/* [한국어] 한 바이트짜리 범위 */
	if (ret)	/* [한국어] 범위가 표 밖이면 */
		return ret;	/* [한국어] 오류를 그대로 — 호출자는 0 이 아닌 값을 실패로 본다 */

	ret = pt_walk_range(&range, __iova_to_phys, &res);	/* [한국어] 최상위부터 따라 내려간다 */
	/* PHYS_ADDR_MAX would be a better error code */
	if (ret)	/* [한국어] (원 주석: PHYS_ADDR_MAX 가 더 나은 오류값이겠지만) */
		return 0;	/* [한국어] 코어 API 가 0 을 "없음"으로 정해 두었다 */
	return res;	/* [한국어] 오프셋까지 반영된 물리 주소 */
}
EXPORT_SYMBOL_NS_GPL(DOMAIN_NS(iova_to_phys), "GENERIC_PT_IOMMU");	/* [한국어] 드라이버 모듈이 이 이름으로 가져다 쓴다 */

struct pt_iommu_dirty_args {
	struct iommu_dirty_bitmap *dirty;
	unsigned int flags;
};

/*
 * [한국어]
 * record_dirty - 더티인 항목을 비트맵에 기록하고 필요하면 지운다
 *
 * @pts: 더티로 판정된 항목.
 * @dirty: 비트맵과 플래그.
 * @num_contig_lg2: 그 항목이 이루는 묶음의 크기.
 *
 * 길이 계산이 이 함수의 까다로운 부분이다. 연속 항목이면 묶음 전체가 한
 * 매핑이지만, 순회가 다루는 범위가 그 묶음의 일부만 덮을 수 있다. 그래서
 * 묶음의 끝과 순회의 끝 중 앞선 쪽까지만 기록한다.
 *
 * IOMMU_DIRTY_NO_CLEAR 가 없으면 표시를 지우고 그 범위를 무효화 목록에
 * 넣는다 — pt_common.h 의 계약대로, TLB 를 비운 뒤부터 다시 세어야 하기
 * 때문이다.
 *
 * 원 주석이 짚듯 여기에는 캐시 플러시가 필요 없다. 비일관 플랫폼과 원자적
 * 더티 추적은 함께 쓸 수 없어, 이 경로가 도는 하드웨어는 표를 일관되게
 * 본다.
 */
static void record_dirty(struct pt_state *pts,
			 struct pt_iommu_dirty_args *dirty,
			 unsigned int num_contig_lg2)
{
	pt_vaddr_t dirty_len;	/* [한국어] 비트맵에 기록할 길이 */

	if (num_contig_lg2 != ilog2(1)) {	/* [한국어] 연속 묶음이면 */
		unsigned int index = pts->index;	/* [한국어] 현재 위치 */
		unsigned int end_index = log2_set_mod_max_t(	/* [한국어] 묶음의 마지막 자리 */
			unsigned int, pts->index, num_contig_lg2);	/* [한국어] 하위 비트를 모두 세워 구한다 */

		/* Adjust for being contained inside a contiguous page */
		end_index = min(end_index, pts->end_index);	/* [한국어] (원 주석: 연속 페이지 안에 들어 있는 경우를 보정한다) */
		dirty_len = (end_index - index) *	/* [한국어] 순회 범위가 덮는 항목 수에 */
				log2_to_int(pt_table_item_lg2sz(pts));	/* [한국어] 항목 크기를 곱한다 */
	} else {
		dirty_len = log2_to_int(pt_table_item_lg2sz(pts));	/* [한국어] 단일 항목이면 그 크기 그대로 */
	}

	if (dirty->dirty->bitmap)	/* [한국어] 기록할 비트맵이 있으면 */
		iova_bitmap_set(dirty->dirty->bitmap, pts->range->va,	/* [한국어] 그 주소 범위를 */
				dirty_len);	/* [한국어] 더티로 표시한다 */

	if (!(dirty->flags & IOMMU_DIRTY_NO_CLEAR)) {	/* [한국어] 지우라는 요청이면 */
		/*
		 * No write log required because DMA incoherence and atomic
		 * dirty tracking bits can't work together
		 */
		pt_entry_make_write_clean(pts);	/* [한국어] (원 주석: 비일관 DMA 와 원자적 더티 추적은 함께 쓸 수 없어 쓰기 로그가 필요 없다) */
		iommu_iotlb_gather_add_range(dirty->dirty->gather,	/* [한국어] TLB 를 비워야 다음 쓰기가 표시를 남긴다 */
					     pts->range->va, dirty_len);	/* [한국어] 그 범위를 무효화 목록에 */
	}
}

/*
 * [한국어]
 * __read_and_clear_dirty - 한 단계의 항목들을 훑는 워커
 *
 * @range: 걷는 범위.
 * @arg: 비트맵과 플래그.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 0 성공, 음수면 아래 단계에서 실패.
 *
 * 표를 만나면 재귀하고, 잎을 만나면 더티인지 보고 기록한다.
 *
 * PT_MAKE_LEVELS 로 펼치지 않고 자기 이름으로 재귀하는 점이 iova_to_phys
 * 와 다르다 — 이 경로는 성능이 덜 중요하고, 펼치면 코드가 크게 는다.
 */
static inline int __read_and_clear_dirty(struct pt_range *range, void *arg,
					 unsigned int level,
					 struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct pt_iommu_dirty_args *dirty = arg;	/* [한국어] 비트맵과 플래그 */
	int ret;	/* [한국어] 아래 단계의 결과 */

	for_each_pt_level_entry(&pts) {	/* [한국어] 이 단계의 항목들을 훑는다 */
		if (pts.type == PT_ENTRY_TABLE) {	/* [한국어] 아래 표면 */
			ret = pt_descend(&pts, arg, __read_and_clear_dirty);	/* [한국어] 재귀한다 — 펼치지 않아 코드가 작다 */
			if (ret)	/* [한국어] 실패면 */
				return ret;	/* [한국어] 바로 전한다 */
			continue;	/* [한국어] 다음 항목으로 */
		}
		if (pts.type == PT_ENTRY_OA && pt_entry_is_write_dirty(&pts))	/* [한국어] 잎이고 쓰기가 있었으면 */
			record_dirty(&pts, dirty,	/* [한국어] 비트맵에 기록하고 */
				     pt_entry_num_contig_lg2(&pts));	/* [한국어] 묶음 크기만큼의 범위로 */
	}
	return 0;	/* [한국어] 이 단계를 다 돌았다 */
}

/**
 * read_and_clear_dirty() - Manipulate the HW set write dirty state
 * @domain: Domain to manipulate
 * @iova: IO virtual address to start
 * @size: Length of the IOVA
 * @flags: A bitmap of IOMMU_DIRTY_NO_CLEAR
 * @dirty: Place to store the dirty bits
 *
 * Iterate over all the entries in the mapped range and record their write dirty
 * status in iommu_dirty_bitmap. If IOMMU_DIRTY_NO_CLEAR is not specified then
 * the entries will be left dirty, otherwise they are returned to being not
 * write dirty.
 *
 * Context: The caller must hold a read range lock that includes @iova.
 *
 * Returns: -ERRNO on failure, 0 on success.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * read_and_clear_dirty - 범위의 더티 상태를 읽어 비트맵에 담는다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @size: 길이.
 * @flags: IOMMU_DIRTY_NO_CLEAR 조합.
 * @dirty: 결과를 담을 비트맵과 무효화 자리.
 * @return: 0 성공, 음수면 실패.
 *
 * 라이브 마이그레이션이 쓰는 경로다. 게스트가 도는 동안 어느 페이지가
 * 바뀌었는지를 이 비트맵으로 알아내 그것만 다시 보낸다.
 *
 * 맨 앞의 #if 가 눈에 띈다 — iommufd 를 끄거나 형식에 더티 비트가 없으면
 * 이 함수 전체가 -EOPNOTSUPP 하나로 접힌다.
 *
 * 실행 컨텍스트: 호출자가 그 범위의 읽기 락을 쥐고 있어야 한다(원 주석).
 */
int DOMAIN_NS(read_and_clear_dirty)(struct iommu_domain *domain,
				    unsigned long iova, size_t size,
				    unsigned long flags,
				    struct iommu_dirty_bitmap *dirty)
{
	struct pt_iommu *iommu_table =	/* [한국어] 공용 도메인에서 */
		container_of(domain, struct pt_iommu, domain);	/* [한국어] 이 계층의 객체로 */
	struct pt_iommu_dirty_args dirty_args = {	/* [한국어] 워커에 넘길 묶음 */
		.dirty = dirty,	/* [한국어] 결과를 담을 비트맵 */
		.flags = flags,	/* [한국어] 지울 것인지 여부 */
	};
	struct pt_range range;	/* [한국어] 걸을 범위 */
	int ret;	/* [한국어] 결과 */

#if !IS_ENABLED(CONFIG_IOMMUFD_DRIVER) || !defined(pt_entry_is_write_dirty)
	return -EOPNOTSUPP;	/* [한국어] iommufd 를 껐거나 형식에 더티 비트가 없으면 함수 전체가 이 한 줄로 접힌다 */
#endif

	ret = make_range(common_from_iommu(iommu_table), &range, iova, size);	/* [한국어] 요청 범위 */
	if (ret)	/* [한국어] 표 밖이면 */
		return ret;	/* [한국어] 거절 */

	ret = pt_walk_range(&range, __read_and_clear_dirty, &dirty_args);	/* [한국어] 범위를 훑으며 기록한다 */
	PT_WARN_ON(ret);	/* [한국어] 이 워커는 실패하지 않아야 한다 */
	return ret;	/* [한국어] 성패 */
}
EXPORT_SYMBOL_NS_GPL(DOMAIN_NS(read_and_clear_dirty), "GENERIC_PT_IOMMU");	/* [한국어] 더티 추적 진입점을 내보낸다 */

/*
 * [한국어]
 * __set_dirty - 한 주소의 항목에 더티 표시를 남기는 워커
 *
 * @range: 걷는 범위.
 * @arg: 쓰지 않는다.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 0 성공, -ENOENT 매핑 없음, -EAGAIN 경합.
 *
 * 시험용 경로다. 하드웨어가 더티를 찍는 것을 소프트웨어가 흉내 내어,
 * 읽기 쪽 코드를 실제 장치 없이 검증할 수 있게 한다.
 *
 * -EAGAIN 은 cmpxchg 가 실패했다는 뜻이다 — 그사이 항목이 바뀌었으므로
 * 호출자가 다시 시도해야 한다.
 */
static inline int __set_dirty(struct pt_range *range, void *arg,
			      unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */

	switch (pt_load_single_entry(&pts)) {	/* [한국어] 한 항목만 본다 */
	case PT_ENTRY_EMPTY:	/* [한국어] 매핑이 없으면 */
		return -ENOENT;	/* [한국어] 표시할 자리가 없다 */
	case PT_ENTRY_TABLE:	/* [한국어] 아래 표면 */
		return pt_descend(&pts, arg, __set_dirty);	/* [한국어] 내려간다 */
	case PT_ENTRY_OA:	/* [한국어] 잎에 닿았으면 */
		if (!pt_entry_make_write_dirty(&pts))	/* [한국어] cmpxchg 가 실패하면 */
			return -EAGAIN;	/* [한국어] 그사이 항목이 바뀌었다 — 다시 시도해야 한다 */
		return 0;	/* [한국어] 표시했다 */
	}
	return -ENOENT;	/* [한국어] enum 밖의 값에 대비한 경로 */
}

/*
 * [한국어]
 * set_dirty - 한 주소를 더티로 표시한다(시험용)
 *
 * @iommu_table: 대상 페이지 테이블.
 * @iova: 표시할 주소.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 한계를 밝힌다 — 아직 잠금이 없어 시험이 경쟁적으로 부르면
 * 깨질 수 있고, 언젠가 RCU 로 감싸야 한다.
 */
static int __maybe_unused NS(set_dirty)(struct pt_iommu *iommu_table,
					dma_addr_t iova)
{
	struct pt_range range;	/* [한국어] 걸을 범위 */
	int ret;	/* [한국어] 결과 */

	ret = make_range(common_from_iommu(iommu_table), &range, iova, 1);	/* [한국어] 한 바이트짜리 범위 */
	if (ret)	/* [한국어] 표 밖이면 */
		return ret;	/* [한국어] 거절 */

	/*
	 * Note: There is no locking here yet, if the test suite races this it
	 * can crash. It should use RCU locking eventually.
	 */
	return pt_walk_range(&range, __set_dirty, NULL);	/* [한국어] (원 주석: 아직 잠금이 없어 시험이 경쟁하면 깨질 수 있다) */
}

struct pt_iommu_collect_args {
	struct iommu_pages_list free_list;
	/* [한국어] 모은 표들을 담는 목록.
	 * 설정자: 호출자가 초기화하고, __collect_tables 가 채운다.
	 * 읽는 자: 무효화가 끝난 뒤 해제하는 쪽.
	 * 값 범위: 비어 있거나 표 페이지들의 목록.
	 * 동기화: 호출 스택에 있어 공유되지 않는다. */
	/* Fail if any OAs are within the range */
	u8 check_mapped : 1;
	/* [한국어] (원 주석: 범위 안에 출력 주소가 있으면 실패시킨다)
	 * 설정자: 큰 페이지를 놓기 전 확인하는 호출이 참으로 둔다.
	 * 읽는 자: __collect_tables 의 두 갈래.
	 * 값 범위: 0(해제용 수집) 또는 1(자리 비었는지 확인).
	 * 동기화: 호출 스택 값. */
};

/*
 * [한국어]
 * __collect_tables - 하위 표를 모두 모으고, 필요하면 매핑이 없는지 확인한다
 *
 * @range: 걷는 범위.
 * @arg: 모을 목록과 검사 여부.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 0 성공, -EADDRINUSE 면 그 범위에 매핑이 남아 있다.
 *
 * 두 가지 목적으로 쓰인다. 도메인을 해제할 때 표 메모리를 전부 모으는
 * 경우와, 큰 페이지를 놓기 전에 그 자리가 정말 비어 있는지 확인하는
 * 경우다 — 후자가 check_mapped 다.
 *
 * 첫 조건이 그 두 용도를 가른다. 검사하지 않는 호출에서는 잎만 있는
 * 단계를 볼 이유가 없어 곧바로 돌아간다.
 *
 * 목록에 넣는 순서가 위에서 아래로인 점에 유의 — 해제는 목록 전체를
 * 한꺼번에 하므로 순서가 문제되지 않는다.
 */
static int __collect_tables(struct pt_range *range, void *arg,
			    unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct pt_iommu_collect_args *collect = arg;	/* [한국어] 모을 목록과 검사 여부 */
	int ret;	/* [한국어] 아래 단계의 결과 */

	if (!collect->check_mapped && !pt_can_have_table(&pts))	/* [한국어] 검사하지 않는 호출이면 잎만 있는 단계는 */
		return 0;	/* [한국어] 볼 이유가 없다 */

	for_each_pt_level_entry(&pts) {	/* [한국어] 이 단계의 항목들을 훑는다 */
		if (pts.type == PT_ENTRY_TABLE) {	/* [한국어] 아래 표면 */
			iommu_pages_list_add(&collect->free_list, pts.table_lower);	/* [한국어] 해제 목록에 넣고 */
			ret = pt_descend(&pts, arg, __collect_tables);	/* [한국어] 그 아래도 훑는다 */
			if (ret)	/* [한국어] 실패면 */
				return ret;	/* [한국어] 바로 전한다 */
			continue;	/* [한국어] 다음 항목으로 */
		}
		if (pts.type == PT_ENTRY_OA && collect->check_mapped)	/* [한국어] 비어 있어야 할 자리에 매핑이 있으면 */
			return -EADDRINUSE;	/* [한국어] 큰 페이지를 놓을 수 없다 */
	}
	return 0;	/* [한국어] 이 단계를 다 돌았다 */
}

enum alloc_mode {ALLOC_NORMAL, ALLOC_DEFER_COHERENT_FLUSH};	/* [한국어] 비일관 플랫폼에서 DMA 매핑을 지금 할지 미룰지 */

/* Allocate a table, the empty table will be ready to be installed. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _table_alloc - 표 메모리를 잡는다
 *
 * @common: 페이지 테이블 인스턴스.
 * @lg2sz: 잡을 크기의 지수.
 * @gfp: 할당 플래그.
 * @mode: 비일관 플랫폼에서 DMA 매핑까지 지금 할 것인가.
 * @return: 표 메모리, 실패하면 ERR_PTR.
 *
 * 0 으로 채워진 페이지를 받아 곧바로 설치할 수 있는 상태로 돌려준다.
 *
 * 비일관 플랫폼에서는 그 페이지를 장치가 DMA 로 읽을 수 있게 매핑까지
 * 해야 한다. ALLOC_DEFER_COHERENT_FLUSH 는 그 일을 뒤로 미루는데,
 * 최상위 표처럼 아직 하드웨어에 연결되지 않은 경우에 쓴다.
 *
 * nid 를 쓰는 이유: 하드웨어가 이 표를 DMA 로 읽으므로 가까운 노드에 두는
 * 편이 빠르다.
 */
static inline struct pt_table_p *_table_alloc(struct pt_common *common,
					      size_t lg2sz, gfp_t gfp,
					      enum alloc_mode mode)
{
	struct pt_iommu *iommu_table = iommu_from_common(common);	/* [한국어] NUMA 노드와 장치를 얻기 위해 */
	struct pt_table_p *table_mem;	/* [한국어] 잡을 표 */

	table_mem = iommu_alloc_pages_node_sz(iommu_table->nid, gfp,	/* [한국어] 하드웨어가 DMA 로 읽으므로 가까운 노드에 */
					      log2_to_int(lg2sz));	/* [한국어] 지수를 실제 크기로 */
	if (!table_mem)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 호출자에게 */

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT) &&	/* [한국어] 비일관 플랫폼이고 */
	    mode == ALLOC_NORMAL) {	/* [한국어] 지금 매핑하라는 요청이면 */
		int ret = iommu_pages_start_incoherent(	/* [한국어] 장치가 DMA 로 읽을 수 있게 매핑한다 */
			table_mem, iommu_table->iommu_device);	/* [한국어] 그 매핑을 만들 장치 */
		if (ret) {	/* [한국어] 매핑 실패면 */
			iommu_free_pages(table_mem);	/* [한국어] 페이지도 돌려주고 */
			return ERR_PTR(ret);	/* [한국어] 오류를 전한다 */
		}
	}
	return table_mem;	/* [한국어] 0 으로 채워져 곧바로 설치할 수 있다 */
}

/*
 * [한국어]
 * table_alloc_top - 최상위 표를 잡는다
 *
 * @common: 페이지 테이블 인스턴스.
 * @top_of_table: 최상위 워드(단계 정보를 담고 있다).
 * @gfp: 할당 플래그.
 * @mode: 비일관 매핑을 지금 할 것인가.
 * @return: 표 메모리, 실패하면 ERR_PTR.
 *
 * 최상위만 크기가 다르다. 주소 공간 폭이 좁으면 항목이 몇 개만 필요하므로
 * pt_top_memsize_lg2 가 그 크기를 계산해 준다.
 *
 * 원 주석이 설계 선택을 밝힌다 — 최상위는 free_list 를 쓰지 않아 굳이
 * iommu-pages API 를 쓸 이유가 없지만, 대개 페이지 크기 이상이라 코드를
 * 단순하게 두려고 같은 API 를 쓴다.
 */
static inline struct pt_table_p *table_alloc_top(struct pt_common *common,
						 uintptr_t top_of_table,
						 gfp_t gfp,
						 enum alloc_mode mode)
{
	/*
	 * Top doesn't need the free list or otherwise, so it technically
	 * doesn't need to use iommu pages. Use the API anyhow as the top is
	 * usually not smaller than PAGE_SIZE to keep things simple.
	 */
	return _table_alloc(common, pt_top_memsize_lg2(common, top_of_table),	/* [한국어] (원 주석: 최상위는 free_list 가 필요 없지만 대개 페이지 크기 이상이라 같은 API 를 쓴다) */
			    gfp, mode);	/* [한국어] 주소 공간 폭이 좁으면 항목 몇 개면 족하다 */
}

/* Allocate an interior table */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * table_alloc - 중간 단계 표를 잡는다
 *
 * @parent_pts: 이 표를 꽂을 상위 항목의 순회 상태.
 * @gfp: 할당 플래그.
 * @mode: 비일관 매핑을 지금 할 것인가.
 * @return: 표 메모리, 실패하면 ERR_PTR.
 *
 * 크기를 아래 단계 기준으로 계산해야 하므로, 한 단계 낮춘 임시 상태를
 * 만들어 항목 수를 묻는다.
 */
static inline struct pt_table_p *table_alloc(const struct pt_state *parent_pts,
					     gfp_t gfp, enum alloc_mode mode)
{
	struct pt_state child_pts =	/* [한국어] 크기는 아래 단계 기준이라 */
		pt_init(parent_pts->range, parent_pts->level - 1, NULL);	/* [한국어] 한 단계 낮춘 임시 상태를 만든다 */

	return _table_alloc(parent_pts->range->common,	/* [한국어] 그 단계의 */
			    pt_num_items_lg2(&child_pts) +	/* [한국어] 항목 수에 */
				    ilog2(PT_ITEM_WORD_SIZE),	/* [한국어] 항목 폭을 곱한 크기 */
			    gfp, mode);	/* [한국어] 할당 방식은 호출자가 정한다 */
}

/*
 * [한국어]
 * pt_iommu_new_table - 새 표를 만들어 현재 항목에 꽂는다
 *
 * @pts: 꽂을 자리(현재 값이 담겨 있어야 한다).
 * @attrs: 할당 플래그와 속성.
 * @return: 0 성공, -EAGAIN 경합에서 짐, -ENOMEM, -ENXIO.
 *
 * 락 없는 표 만들기의 본체다. 표를 만들고 cmpxchg 로 꽂되, 다른 스레드가
 * 먼저 꽂았으면 내 표를 버리고 -EAGAIN 을 돌려준다 — 호출자가 그 단계를
 * 다시 읽고 이어 간다.
 *
 * 비일관 플랫폼의 순서가 중요하다. 표를 꽂은 뒤에 캐시를 밀어내고, 그
 * 다음에 소프트웨어 비트를 해제 순서로 세운다. 그 비트를 본 다른 CPU 는
 * 표 내용도 반드시 본다 — 즉 "이 표는 하드웨어가 읽어도 되는 상태"라는
 * 깃발이다.
 *
 * 디버그 빌드의 되읽기 검사는 kunit 을 위한 것이다. 원 주석이 상황을
 * 설명한다 — 시험 환경에서는 표 형식이 담을 수 있는 것보다 넓은 물리
 * 주소가 나올 수 있고, 그러면 조용히 잘린 주소를 쓰게 된다.
 */
static inline int pt_iommu_new_table(struct pt_state *pts,
				     struct pt_write_attrs *attrs)
{
	struct pt_table_p *table_mem;	/* [한국어] 만들 표 */
	phys_addr_t phys;	/* [한국어] 그 표의 물리 주소 */

	/* Given PA/VA/length can't be represented */
	if (PT_WARN_ON(!pt_can_have_table(pts)))	/* [한국어] (원 주석: 주어진 PA/VA/길이를 표현할 수 없다) */
		return -ENXIO;	/* [한국어] 0단계 아래로는 내려갈 수 없다 */

	table_mem = table_alloc(pts, attrs->gfp, ALLOC_NORMAL);	/* [한국어] 표를 잡고 */
	if (IS_ERR(table_mem))	/* [한국어] 실패면 */
		return PTR_ERR(table_mem);	/* [한국어] 오류를 전한다 */

	phys = virt_to_phys(table_mem);	/* [한국어] 항목에 적을 물리 주소 */
	if (!pt_install_table(pts, phys, attrs)) {	/* [한국어] cmpxchg 로 꽂는다 — 경합에서 지면 */
		iommu_pages_free_incoherent(	/* [한국어] 내가 만든 표를 버리고 */
			table_mem,	/* [한국어] 매핑까지 함께 푼다 */
			iommu_from_common(pts->range->common)->iommu_device);	/* [한국어] 그 매핑을 만든 장치 */
		return -EAGAIN;	/* [한국어] 호출자가 그 단계를 다시 읽는다 */
	}

	if (pts_feature(pts, PT_FEAT_DMA_INCOHERENT)) {	/* [한국어] 비일관 플랫폼이면 */
		flush_writes_item(pts);	/* [한국어] 꽂은 항목을 메모리로 밀어내고 */
		pt_set_sw_bit_release(pts, SW_BIT_CACHE_FLUSH_DONE);	/* [한국어] 그 뒤에 깃발을 세운다 — 깃발을 본 CPU 는 표 내용도 본다 */
	}

	if (IS_ENABLED(CONFIG_DEBUG_GENERIC_PT)) {	/* [한국어] 디버그 빌드에서만 */
		/*
		 * The underlying table can't store the physical table address.
		 * This happens when kunit testing tables outside their normal
		 * environment where a CPU might be limited.
		 */
		pt_load_single_entry(pts);	/* [한국어] (원 주석: 시험 환경에서 표 형식이 담을 수 없는 물리 주소가 나올 수 있다) */
		if (PT_WARN_ON(pt_table_pa(pts) != phys)) {	/* [한국어] 되읽은 주소가 다르면 조용히 잘린 것이다 */
			pt_clear_entries(pts, ilog2(1));	/* [한국어] 꽂은 항목을 지우고 */
			iommu_pages_free_incoherent(	/* [한국어] 표도 버린다 */
				table_mem, iommu_from_common(pts->range->common)	/* [한국어] 그 매핑을 만든 */
						   ->iommu_device);	/* [한국어] 장치 */
			return -EINVAL;	/* [한국어] 이 환경에서는 쓸 수 없다 */
		}
	}

	pts->table_lower = table_mem;	/* [한국어] 호출자가 곧바로 내려갈 수 있게 */
	return 0;	/* [한국어] 성공 */
}

struct pt_iommu_map_args {
	struct iommu_iotlb_gather *iotlb_gather;
	/* [한국어] 무효화 범위와 해제 목록을 모으는 자리.
	 * 설정자: map 진입점이 코어에서 받은 것을 그대로 넣는다.
	 * 읽는 자: clear_contig 가 걷어낸 표를 여기 얹는다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 드라이버가 잡은 범위 락이 지킨다. */
	struct pt_write_attrs attrs;
	/* [한국어] 잎에 얹을 권한 비트와 할당 플래그.
	 * 설정자: map 진입점이 pt_iommu_set_prot 으로 채운다.
	 * 읽는 자: pt_install_leaf_entry 와 표 할당.
	 * 값 범위: 형식이 정하는 비트 조합.
	 * 동기화: 호출 스택 값. */
	pt_oaddr_t oa;
	/* [한국어] 다음에 놓을 출력 주소.
	 * 설정자: 잎을 하나 놓을 때마다 그 크기만큼 앞으로 간다.
	 * 읽는 자: 다음 잎의 설치와 페이지 크기 계산.
	 * 값 범위: 형식의 출력 주소 범위 안.
	 * 동기화: 호출 스택 값. */
	unsigned int leaf_pgsize_lg2;
	/* [한국어] 지금 놓고 있는 페이지 크기의 지수.
	 * 설정자: 진입점이 처음 계산하고, 크기가 바뀌는 지점에서 다시 정해진다.
	 * 읽는 자: __map_range 가 어느 단계까지 내려갈지 정할 때.
	 * 값 범위: 도메인 비트맵에 있는 크기 중 하나.
	 * 동기화: 호출 스택 값. */
	unsigned int leaf_level;
	/* [한국어] 그 크기를 담당하는 단계.
	 * 설정자: leaf_pgsize_lg2 와 함께 정해진다.
	 * 읽는 자: __map_range 의 재귀 종료 조건.
	 * 값 범위: 0 부터 최상위까지.
	 * 동기화: 호출 스택 값. */
	pt_vaddr_t num_leaves;
	/* [한국어] 이 크기로 연달아 놓을 잎의 남은 개수.
	 * 설정자: pt_pgsz_count 가 계산하고, 놓을 때마다 줄어든다.
	 * 읽는 자: __map_range_leaf 가 표 중간에서 멈출지 정할 때.
	 * 값 범위: 0 이면 이 크기로는 더 놓을 것이 없다.
	 * 동기화: 호출 스택 값. */
};

/*
 * This will recursively check any tables in the block to validate they are
 * empty and then free them through the gather.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * clear_contig - 큰 페이지를 놓을 자리를 비운다
 *
 * @start_pts: 비울 구간의 첫 항목.
 * @iotlb_gather: 무효화와 해제를 모으는 자리.
 * @step: 비울 항목 수.
 * @pgsize_lg2: 놓으려는 페이지 크기(쓰지 않는다).
 * @return: 0 성공, -EADDRINUSE 면 그 자리에 매핑이 남아 있다.
 *
 * 이미 표가 꽂혀 있는 자리에 큰 페이지를 놓으려면 그 표를 걷어내야 한다.
 * 다만 그 아래에 실제 매핑이 있으면 안 되므로, 내려가며 확인한 뒤에야
 * 지운다.
 *
 * 원 주석이 순서를 못박는다 — 표 항목을 먼저 지우고 나서야 gather 에
 * 넣을 수 있다. 그러지 않으면 아직 하드웨어가 참조하는 표를 해제 목록에
 * 올리게 된다.
 *
 * 반복문이 for_each_pt_level_entry 대신 손으로 쓰여 있는 이유: 색인 구간을
 * 호출자가 정한 step 으로 좁혀야 하기 때문이다.
 */
static int clear_contig(const struct pt_state *start_pts,
			struct iommu_iotlb_gather *iotlb_gather,
			unsigned int step, unsigned int pgsize_lg2)
{
	struct pt_iommu *iommu_table =	/* [한국어] gather 에 넘길 때 필요하다 */
		iommu_from_common(start_pts->range->common);	/* [한국어] 공통 상태에서 되짚는다 */
	struct pt_range range = *start_pts->range;	/* [한국어] 원본 순회를 건드리지 않으려고 복사한다 */
	struct pt_state pts =	/* [한국어] 같은 표를 가리키되 */
		pt_init(&range, start_pts->level, start_pts->table);	/* [한국어] 색인 구간만 따로 정할 상태 */
	struct pt_iommu_collect_args collect = { .check_mapped = true };	/* [한국어] 아래에 매핑이 있으면 실패시킨다 */
	int ret;	/* [한국어] 결과 */

	pts.index = start_pts->index;	/* [한국어] 비울 구간의 시작 */
	pts.end_index = start_pts->index + step;	/* [한국어] 그 끝 — 호출자가 정한 만큼만 */
	for (; _pt_iter_load(&pts); pt_next_entry(&pts)) {	/* [한국어] 구간의 항목들을 훑는다 */
		if (pts.type == PT_ENTRY_TABLE) {	/* [한국어] 표가 꽂혀 있으면 */
			collect.free_list =	/* [한국어] 모을 목록을 */
				IOMMU_PAGES_LIST_INIT(collect.free_list);	/* [한국어] 항목마다 새로 시작한다 */
			ret = pt_walk_descend_all(&pts, __collect_tables,	/* [한국어] 아래에 매핑이 없는지 확인하며 */
						  &collect);	/* [한국어] 하위 표를 모은다 */
			if (ret)	/* [한국어] 매핑이 남아 있으면 */
				return ret;	/* [한국어] 큰 페이지를 놓을 수 없다 */

			/*
			 * The table item must be cleared before we can update
			 * the gather
			 */
			pt_clear_entries(&pts, ilog2(1));	/* [한국어] (원 주석: gather 를 갱신하기 전에 표 항목을 먼저 지워야 한다) */
			flush_writes_item(&pts);	/* [한국어] 비일관 플랫폼이면 그 지움을 메모리로 */

			iommu_pages_list_add(&collect.free_list,	/* [한국어] 이 표 자신도 */
					     pt_table_ptr(&pts));	/* [한국어] 해제 목록에 */
			gather_range_pages(	/* [한국어] 무효화 범위와 해제 목록을 */
				iotlb_gather, iommu_table, range.va,	/* [한국어] 코어에 넘긴다 */
				log2_to_int(pt_table_item_lg2sz(&pts)),	/* [한국어] 이 항목이 덮던 크기 */
				&collect.free_list);	/* [한국어] 무효화가 끝난 뒤 해제된다 */
		} else if (pts.type != PT_ENTRY_EMPTY) {	/* [한국어] 표도 빈자리도 아니면 잎이 있다 */
			return -EADDRINUSE;	/* [한국어] 그 자리를 덮어쓸 수 없다 */
		}
	}
	return 0;	/* [한국어] 구간이 비었다 */
}

/*
 * [한국어]
 * __map_range_leaf - 잎을 놓을 단계에 닿았을 때 실제로 채우는 워커
 *
 * @range: 걷는 범위.
 * @arg: 매핑 인자(출력 주소, 페이지 크기, 남은 개수 등).
 * @level: 현재 단계(반드시 map->leaf_level).
 * @table: 그 단계의 표.
 * @return: 0 이 표를 다 채웠다, -EAGAIN 페이지 크기가 바뀌어 다시 와야 한다,
 *          음수는 실패.
 *
 * 매핑의 가장 안쪽 반복문이다. 같은 크기의 잎을 연달아 놓으며 출력 주소를
 * 그만큼씩 밀어 간다.
 *
 * 세 가지가 이 함수를 복잡하게 만든다.
 *
 * 1) 자리가 비어 있지 않을 수 있다. 연속 항목을 놓으려면 그 구간을 통째로
 *    확인하고 비워야 하므로, 비어 있지 않거나 연속을 놓는 경우에는
 *    clear_contig 를 먼저 부른다.
 *
 * 2) 표 중간에서 페이지 크기가 바뀔 수 있다. 남은 길이나 정렬이 달라지는
 *    지점이 있고, 거기서 멈춰 새 크기·새 단계를 계산해 map 에 적어 둔다.
 *    표를 다 채우지 못했으면 -EAGAIN 으로 호출자가 이 단계를 다시 돌게 한다.
 *
 * 3) VA 를 게으르게 유지한다. 반복문은 색인으로만 진행하고, VA 가 실제로
 *    필요한 지점에서만 pt_index_to_va 로 되짚는다.
 *
 * 캐시 플러시를 반복문 밖에서 한 번에 하는 것도 의도적이다 — 항목마다
 * 밀어내면 비일관 플랫폼에서 비용이 크게 는다.
 */
static int __map_range_leaf(struct pt_range *range, void *arg,
			    unsigned int level, struct pt_table_p *table)
{
	struct pt_iommu *iommu_table = iommu_from_common(range->common);	/* [한국어] 도메인의 크기 비트맵을 얻기 위해 */
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct pt_iommu_map_args *map = arg;	/* [한국어] 매핑 인자 */
	unsigned int leaf_pgsize_lg2 = map->leaf_pgsize_lg2;	/* [한국어] 이번에 놓을 페이지 크기 */
	unsigned int start_index;	/* [한국어] 캐시 플러시 범위의 시작 */
	pt_oaddr_t oa = map->oa;	/* [한국어] 다음에 놓을 출력 주소 */
	unsigned int num_leaves;	/* [한국어] 이 표를 마친 뒤 남을 개수 */
	unsigned int orig_end;	/* [한국어] 원래의 끝 색인 — 표를 다 채웠는지 판정한다 */
	pt_vaddr_t last_va;	/* [한국어] 마지막으로 놓은 잎의 다음 주소 */
	unsigned int step;	/* [한국어] 잎 하나가 차지하는 항목 수 */
	bool need_contig;	/* [한국어] 연속 항목을 놓는가 */
	int ret = 0;	/* [한국어] 결과 */

	PT_WARN_ON(map->leaf_level != level);	/* [한국어] 잎을 놓을 단계에서만 불려야 한다 */
	PT_WARN_ON(!pt_can_have_leaf(&pts));	/* [한국어] 그 단계가 잎을 허용해야 한다 */

	step = log2_to_int_t(unsigned int,	/* [한국어] 잎 하나가 */
			     leaf_pgsize_lg2 - pt_table_item_lg2sz(&pts));	/* [한국어] 몇 개의 항목을 차지하는가 */
	need_contig = leaf_pgsize_lg2 != pt_table_item_lg2sz(&pts);	/* [한국어] 항목 크기보다 크면 연속 묶음이다 */

	_pt_iter_first(&pts);	/* [한국어] 색인 구간을 잡고 */
	start_index = pts.index;	/* [한국어] 플러시 범위의 시작을 기억한다 */
	orig_end = pts.end_index;	/* [한국어] 표를 다 채웠는지 나중에 비교한다 */
	if (pts.index + map->num_leaves < pts.end_index) {	/* [한국어] 남은 개수가 이 표를 다 채우지 못하면 */
		/* Need to stop in the middle of the table to change sizes */
		pts.end_index = pts.index + map->num_leaves;	/* [한국어] (원 주석: 크기를 바꾸려면 표 중간에서 멈춰야 한다) */
		num_leaves = 0;	/* [한국어] 이 크기로는 더 놓을 것이 없다 */
	} else {
		num_leaves = map->num_leaves - (pts.end_index - pts.index);	/* [한국어] 표를 다 채우고도 남는 개수 */
	}

	do {
		pts.type = pt_load_entry_raw(&pts);	/* [한국어] 이 자리가 비어 있는지 본다 */
		if (pts.type != PT_ENTRY_EMPTY || need_contig) {	/* [한국어] 차 있거나 연속 묶음을 놓으려면 */
			if (pts.index != start_index)	/* [한국어] VA 를 게으르게 유지하므로 */
				pt_index_to_va(&pts);	/* [한국어] 필요한 지점에서만 되짚는다 */
			ret = clear_contig(&pts, map->iotlb_gather, step,	/* [한국어] 그 구간을 확인하고 비운다 */
					   leaf_pgsize_lg2);	/* [한국어] 놓으려는 크기만큼 */
			if (ret)	/* [한국어] 매핑이 남아 있으면 */
				break;	/* [한국어] 여기서 멈춘다 */
		}

		if (IS_ENABLED(CONFIG_DEBUG_GENERIC_PT)) {	/* [한국어] 디버그 빌드에서만 */
			pt_index_to_va(&pts);	/* [한국어] VA 를 맞추고 */
			PT_WARN_ON(compute_best_pgsize(&pts, oa) !=	/* [한국어] 호출자가 고른 크기가 */
				   leaf_pgsize_lg2);	/* [한국어] 여기서도 최선인지 확인한다 */
		}
		pt_install_leaf_entry(&pts, oa, leaf_pgsize_lg2, &map->attrs);	/* [한국어] 실제로 매핑이 생기는 지점 */

		oa += log2_to_int(leaf_pgsize_lg2);	/* [한국어] 다음 출력 주소 */
		pts.index += step;	/* [한국어] 다음 자리 */
	} while (pts.index < pts.end_index);	/* [한국어] 구간을 다 채울 때까지 */

	flush_writes_range(&pts, start_index, pts.index);	/* [한국어] 항목마다가 아니라 한 번에 — 비일관 플랫폼의 비용을 줄인다 */

	map->oa = oa;	/* [한국어] 호출자가 이어 갈 수 있게 */
	map->num_leaves = num_leaves;	/* [한국어] 남은 개수 */
	if (ret || num_leaves)	/* [한국어] 실패했거나 이 크기로 더 놓을 것이 남았으면 */
		return ret;	/* [한국어] 호출자가 판단한다 */

	/* range->va is not valid if we reached the end of the table */
	pts.index -= step;	/* [한국어] (원 주석: 표 끝에 닿았으면 range->va 가 유효하지 않다) */
	pt_index_to_va(&pts);	/* [한국어] 마지막으로 놓은 잎의 VA 를 되짚고 */
	pts.index += step;	/* [한국어] 색인은 원래대로 */
	last_va = range->va + log2_to_int(leaf_pgsize_lg2);	/* [한국어] 그 잎의 다음 주소 */

	if (last_va - 1 == range->last_va) {	/* [한국어] 요청 범위를 다 덮었으면 */
		PT_WARN_ON(pts.index != orig_end);	/* [한국어] 표도 끝까지 왔어야 한다 */
		return 0;	/* [한국어] 매핑 완료 */
	}

	/*
	 * Reached a point where the page size changed, compute the new
	 * parameters.
	 */
	map->leaf_pgsize_lg2 = pt_compute_best_pgsize(	/* [한국어] (원 주석: 페이지 크기가 바뀌는 지점에 닿았으니 새 값을 계산한다) */
		iommu_table->domain.pgsize_bitmap, last_va, range->last_va, oa);	/* [한국어] 남은 범위와 새 정렬로 */
	map->leaf_level =	/* [한국어] 그 크기를 담당하는 */
		pt_pgsz_lg2_to_level(range->common, map->leaf_pgsize_lg2);	/* [한국어] 단계를 구하고 */
	map->num_leaves = pt_pgsz_count(iommu_table->domain.pgsize_bitmap,	/* [한국어] 그 크기로 몇 개를 */
					last_va, range->last_va, oa,	/* [한국어] 연달아 놓을 수 있는지 */
					map->leaf_pgsize_lg2);	/* [한국어] 미리 센다 */

	/* Didn't finish this table level, caller will repeat it */
	if (pts.index != orig_end) {	/* [한국어] (원 주석: 이 단계를 다 돌지 못했으니 호출자가 다시 부른다) */
		if (pts.index != start_index)	/* [한국어] VA 를 맞춰 두어야 */
			pt_index_to_va(&pts);	/* [한국어] 호출자가 이어 갈 수 있다 */
		return -EAGAIN;	/* [한국어] 같은 표를 새 크기로 다시 돈다 */
	}
	return 0;	/* [한국어] 표를 다 채웠다 — 나머지는 상위가 다음 표로 넘긴다 */
}

/*
 * [한국어]
 * __map_range - 잎 단계에 닿을 때까지 내려가며 필요한 표를 만드는 워커
 *
 * @range: 걷는 범위.
 * @arg: 매핑 인자.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 0 성공, -EAGAIN 이 단계를 다시 돌아야 함, -EADDRINUSE 자리 충돌.
 *
 * 매핑의 바깥 재귀다. 항목이 비어 있으면 표를 만들고, 표가 있으면 그대로
 * 내려간다.
 *
 * 두 곳의 -EAGAIN 처리가 락 없는 설계의 핵심이다.
 *  - pt_iommu_new_table 이 경합에서 지면 -EAGAIN 을 주는데, 바깥 do-while
 *    이 같은 항목을 다시 읽어 이번에는 표를 찾는다.
 *  - 아래 단계가 -EAGAIN 을 주면 페이지 크기가 바뀐 것이므로 그 단계를
 *    다시 부른다.
 *
 * 비일관 플랫폼의 캐시 처리가 미묘하다. 이미 있는 표를 만난 경우, 그것을
 * 만든 스레드가 아직 캐시를 밀어내는 중일 수 있다. 원 주석이 그 대응을
 * 설명한다 — 깃발(SW_BIT_CACHE_FLUSH_DONE)이 아직 서 있지 않으면 나도
 * 밀어낸다. 그래야 내 매핑이 끝났을 때 거기까지 이르는 모든 표 항목이
 * 하드웨어에 보인다.
 *
 * 마지막의 -EAGAIN 은 다른 사정이다. 아래 단계에서 페이지 크기가 커져
 * 잎 단계가 이 단계로 올라왔으면, 지금 실행 중인 함수는 더 이상 맞지
 * 않으므로 호출자가 __map_range_leaf 를 부르게 한다.
 */
static int __map_range(struct pt_range *range, void *arg, unsigned int level,
		       struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct pt_iommu_map_args *map = arg;	/* [한국어] 매핑 인자 */
	int ret;	/* [한국어] 결과 */

	PT_WARN_ON(map->leaf_level == level);	/* [한국어] 잎 단계면 다른 워커가 맡는다 */
	PT_WARN_ON(!pt_can_have_table(&pts));	/* [한국어] 내려갈 수 있는 단계여야 한다 */

	_pt_iter_first(&pts);	/* [한국어] 색인 구간을 잡는다 */

	/* Descend to a child table */
	do {
		pts.type = pt_load_entry_raw(&pts);	/* [한국어] (원 주석: 하위 표로 내려간다) 이 자리에 무엇이 있나 */

		if (pts.type != PT_ENTRY_TABLE) {	/* [한국어] 표가 아니면 */
			if (pts.type != PT_ENTRY_EMPTY)	/* [한국어] 잎이 이미 있으면 */
				return -EADDRINUSE;	/* [한국어] 그 자리를 덮어쓸 수 없다 */
			ret = pt_iommu_new_table(&pts, &map->attrs);	/* [한국어] 비어 있으면 표를 만들어 꽂는다 */
			/* EAGAIN on a race will loop again */
			if (ret)	/* [한국어] (원 주석: 경합에 진 EAGAIN 이면 바깥 반복문이 다시 돈다) */
				return ret;	/* [한국어] 호출자가 되돌리거나 다시 시도한다 */
		} else {
			pts.table_lower = pt_table_ptr(&pts);	/* [한국어] 이미 있는 표로 내려갈 준비 */
			/*
			 * Racing with a shared pt_iommu_new_table()? The other
			 * thread is still flushing the cache, so we have to
			 * also flush it to ensure that when our thread's map
			 * completes all the table items leading to our mapping
			 * are visible.
			 *
			 * This requires the pt_set_bit_release() to be a
			 * release of the cache flush so that this can acquire
			 * visibility at the iommu.
			 */
			if (pts_feature(&pts, PT_FEAT_DMA_INCOHERENT) &&	/* [한국어] (원 주석: 이 표를 만든 스레드가 아직 캐시를 밀어내는 중일 수 있다) */
			    !pt_test_sw_bit_acquire(&pts,	/* [한국어] 깃발이 아직 서 있지 않으면 */
						    SW_BIT_CACHE_FLUSH_DONE))	/* [한국어] 내 매핑이 끝났을 때 표가 보이도록 */
				flush_writes_item(&pts);	/* [한국어] 나도 밀어낸다 */
		}

		/*
		 * The already present table can possibly be shared with another
		 * concurrent map.
		 */
		do {
			if (map->leaf_level == level - 1)	/* [한국어] (원 주석: 이미 있는 표는 다른 매핑과 공유될 수 있다) */
				ret = pt_descend(&pts, arg, __map_range_leaf);	/* [한국어] 아래가 잎 단계면 잎 워커로 */
			else
				ret = pt_descend(&pts, arg, __map_range);	/* [한국어] 아니면 한 단계 더 내려간다 */
		} while (ret == -EAGAIN);	/* [한국어] 페이지 크기가 바뀌었으면 그 단계를 다시 */
		if (ret)	/* [한국어] 실패면 */
			return ret;	/* [한국어] 바로 전한다 */

		pts.index++;	/* [한국어] 다음 항목으로 */
		pt_index_to_va(&pts);	/* [한국어] VA 를 색인에 맞춘다 */
		if (pts.index >= pts.end_index)	/* [한국어] 이 표를 다 돌았으면 */
			break;	/* [한국어] 끝 */

		/*
		 * This level is currently running __map_range_leaf() which is
		 * not correct if the target level has been updated to this
		 * level. Have the caller invoke __map_range_leaf.
		 */
		if (map->leaf_level == level)	/* [한국어] (원 주석: 잎 단계가 이 단계로 올라왔으면 지금 함수는 맞지 않다) */
			return -EAGAIN;	/* [한국어] 호출자가 __map_range_leaf 를 부르게 한다 */
	} while (true);	/* [한국어] 표를 다 돌 때까지 */
	return 0;	/* [한국어] 이 단계를 마쳤다 */
}

/*
 * Fast path for the easy case of mapping a 4k page to an already allocated
 * table. This is a common workload. If it returns EAGAIN run the full algorithm
 * instead.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * __do_map_single_page - 4KB 한 장을 이미 있는 표에 놓는 빠른 경로
 *
 * @range: 걷는 범위.
 * @arg: 매핑 인자.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @descend_fn: 아래 단계 워커.
 * @return: 0 성공, -EAGAIN 이면 일반 경로로 넘어가야 한다.
 *
 * 원 주석이 존재 이유를 밝힌다 — 4KB 한 장을 이미 있는 표에 놓는 것이
 * 가장 흔한 작업이라, 그 경우만 따로 최적화한다.
 *
 * 표를 만들지 않는 것이 요점이다. 중간에 빈 항목을 만나면 곧바로
 * -EAGAIN 을 주고 일반 경로에 넘긴다. 그래서 이 함수에는 할당도, 경합
 * 처리도, 크기 계산도 없다.
 *
 * 비일관 플랫폼에서 쓰지 않는 이유도 같다 — 캐시 플러시를 넣으면 이
 * 함수의 코드가 커져 최적화의 뜻이 사라진다.
 */
static __always_inline int __do_map_single_page(struct pt_range *range,
						void *arg, unsigned int level,
						struct pt_table_p *table,
						pt_level_fn_t descend_fn)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct pt_iommu_map_args *map = arg;	/* [한국어] 매핑 인자 */

	pts.type = pt_load_single_entry(&pts);	/* [한국어] 한 항목만 읽는다 */
	if (pts.level == 0) {	/* [한국어] 잎을 놓을 마지막 단계면 */
		if (pts.type != PT_ENTRY_EMPTY)	/* [한국어] 자리가 비어 있지 않으면 */
			return -EADDRINUSE;	/* [한국어] 덮어쓸 수 없다 */
		pt_install_leaf_entry(&pts, map->oa, PAGE_SHIFT,	/* [한국어] 4KB 한 장을 놓는다 */
				      &map->attrs);	/* [한국어] 권한은 호출자가 준 대로 */
		/* No flush, not used when incoherent */
		map->oa += PAGE_SIZE;	/* [한국어] (원 주석: 비일관에서는 쓰지 않으므로 플러시가 없다) */
		return 0;	/* [한국어] 한 장 매핑 완료 */
	}
	if (pts.type == PT_ENTRY_TABLE)	/* [한국어] 표가 이미 있으면 */
		return pt_descend(&pts, arg, descend_fn);	/* [한국어] 그대로 내려간다 — 만들지는 않는다 */
	/* Something else, use the slow path */
	return -EAGAIN;	/* [한국어] (원 주석: 그 밖에는 느린 경로를 쓴다) */
}
PT_MAKE_LEVELS(__map_single_page, __do_map_single_page);

/*
 * Add a table to the top, increasing the top level as much as necessary to
 * encompass range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * increase_top - 최상위 단계를 얹어 주소 공간을 넓힌다
 *
 * @iommu_table: 대상 페이지 테이블.
 * @range: 담아야 할 범위.
 * @map: 매핑 인자(필요한 잎 단계와 할당 플래그).
 * @return: 0 성공, -EAGAIN 다른 스레드가 먼저 했음, -ERANGE 한계 초과.
 *
 * DYNAMIC_TOP 형식은 작게 시작해 필요할 때만 자란다. 이 함수가 그 성장을
 * 맡는다.
 *
 * 반복문이 한 단계씩 얹는다. 새 표를 만들고, 그 0번 항목이 기존 최상위를
 * 가리키게 한 다음, 그것을 새 최상위로 삼는다. 요청 범위가 들어갈 때까지
 * 되풀이한다.
 *
 * 그 사이의 표들은 아직 하드웨어에 연결되지 않았으므로 비일관 매핑을
 * 미뤄 두었다가(ALLOC_DEFER_COHERENT_FLUSH) 마지막에 한 번에 한다 —
 * 원 주석이 이중 플러시를 피한다고 밝힌 대목이다.
 *
 * 락 구간의 순서가 결정적이다. 원 주석이 계약을 명시한다: 읽는 쪽은
 * READ_ONCE 로 락 없이 최상위를 보고, 주소와 단계가 한 워드에 있어 늘
 * 일관된 값을 본다. 그래서 하드웨어를 먼저 새 단계로 바꾸고 나서야
 * top_of_table 을 갱신해야, 다른 스레드가 아직 하드웨어가 모르는 단계에
 * 매핑을 만드는 일이 없다.
 *
 * 무효화를 하지 않는 근거도 원 주석에 있다 — 걷기 캐시에 남은 항목은
 * 여전히 옳고, 새로 생긴 IOVA 는 전부 비어 있어 부정 캐시도 문제가 되지
 * 않는다.
 */
static int increase_top(struct pt_iommu *iommu_table, struct pt_range *range,
			struct pt_iommu_map_args *map)
{
	struct iommu_pages_list free_list = IOMMU_PAGES_LIST_INIT(free_list);	/* [한국어] 실패하면 되돌릴 표들 */
	struct pt_common *common = common_from_iommu(iommu_table);	/* [한국어] 페이지 테이블 인스턴스 */
	uintptr_t top_of_table = READ_ONCE(common->top_of_table);	/* [한국어] 시작 시점의 최상위 — 나중에 바뀌었는지 비교한다 */
	uintptr_t new_top_of_table = top_of_table;	/* [한국어] 한 단계씩 얹으며 갱신할 값 */
	struct pt_table_p *table_mem;	/* [한국어] 새로 만든 표 */
	unsigned int new_level;	/* [한국어] 최종 단계 */
	spinlock_t *domain_lock;	/* [한국어] 드라이버가 빌려 주는 락 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret;	/* [한국어] 결과 */

	while (true) {	/* [한국어] 범위가 들어갈 때까지 한 단계씩 */
		struct pt_range top_range =	/* [한국어] 지금까지 얹은 최상위로 */
			_pt_top_range(common, new_top_of_table);	/* [한국어] 범위를 만들어 */
		struct pt_state pts = pt_init_top(&top_range);	/* [한국어] 그 단계의 상태 */

		top_range.va = range->va;	/* [한국어] 요청 범위를 넣어 */
		top_range.last_va = range->last_va;	/* [한국어] 들어가는지 본다 */

		if (!pt_check_range(&top_range) &&	/* [한국어] 범위가 들어가고 */
		    map->leaf_level <= pts.level) {	/* [한국어] 필요한 잎 단계도 덮으면 */
			new_level = pts.level;	/* [한국어] 여기까지가 새 최상위 */
			break;	/* [한국어] 더 얹을 필요가 없다 */
		}

		pts.level++;	/* [한국어] 한 단계 위로 */
		if (pts.level > PT_MAX_TOP_LEVEL ||	/* [한국어] 형식의 최대 단계를 넘거나 */
		    pt_table_item_lg2sz(&pts) >= common->max_vasz_lg2) {	/* [한국어] 인스턴스의 주소 폭을 넘으면 */
			ret = -ERANGE;	/* [한국어] 더 넓힐 수 없다 */
			goto err_free;	/* [한국어] 만든 것을 되돌린다 */
		}

		table_mem =	/* [한국어] 새 최상위 표를 */
			table_alloc_top(common, _pt_top_set(NULL, pts.level),	/* [한국어] 그 단계 크기로 잡는다 */
					map->attrs.gfp, ALLOC_DEFER_COHERENT_FLUSH);	/* [한국어] 아직 하드웨어에 연결되지 않아 매핑을 미룬다 */
		if (IS_ERR(table_mem)) {	/* [한국어] 실패면 */
			ret = PTR_ERR(table_mem);	/* [한국어] 오류를 전하고 */
			goto err_free;	/* [한국어] 되돌린다 */
		}
		iommu_pages_list_add(&free_list, table_mem);	/* [한국어] 실패 시 되돌릴 목록에 */

		/* The new table links to the lower table always at index 0 */
		top_range.va = 0;	/* [한국어] (원 주석: 새 표는 늘 0번 항목에서 아래 표로 이어진다) */
		top_range.top_level = pts.level;	/* [한국어] 새 단계로 */
		pts.table_lower = pts.table;	/* [한국어] 기존 최상위가 아래 표가 되고 */
		pts.table = table_mem;	/* [한국어] 새 표가 최상위가 된다 */
		pt_load_single_entry(&pts);	/* [한국어] 0번 항목을 읽어 */
		PT_WARN_ON(pts.index != 0);	/* [한국어] 반드시 0번이어야 한다 */
		pt_install_table(&pts, virt_to_phys(pts.table_lower),	/* [한국어] 기존 최상위를 가리키게 꽂는다 */
				 &map->attrs);	/* [한국어] 속성은 호출자가 준 대로 */
		new_top_of_table = _pt_top_set(pts.table, pts.level);	/* [한국어] 새 최상위 워드 */
	}

	/*
	 * Avoid double flushing, flush it once after all pt_install_table()
	 */
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT)) {	/* [한국어] (원 주석: 이중 플러시를 피해 모든 설치 뒤에 한 번만 한다) */
		ret = iommu_pages_start_incoherent_list(	/* [한국어] 미뤄 둔 매핑을 이제 한꺼번에 */
			&free_list, iommu_table->iommu_device);	/* [한국어] 그 장치로 */
		if (ret)	/* [한국어] 실패면 */
			goto err_free;	/* [한국어] 되돌린다 */
	}

	/*
	 * top_of_table is write locked by the spinlock, but readers can use
	 * READ_ONCE() to get the value. Since we encode both the level and the
	 * pointer in one quanta the lockless reader will always see something
	 * valid. The HW must be updated to the new level under the spinlock
	 * before top_of_table is updated so that concurrent readers don't map
	 * into the new level until it is fully functional. If another thread
	 * already updated it while we were working then throw everything away
	 * and try again.
	 */
	domain_lock = iommu_table->driver_ops->get_top_lock(iommu_table);	/* [한국어] (원 주석: top_of_table 은 이 스핀락이 쓰기를 지키고 읽기는 READ_ONCE 로 한다) */
	spin_lock_irqsave(domain_lock, flags);	/* [한국어] 장치 목록 순회와도 직렬화된다 */
	if (common->top_of_table != top_of_table ||	/* [한국어] 그사이 다른 스레드가 바꿨거나 */
	    top_of_table == new_top_of_table) {	/* [한국어] 얹은 것이 없으면 */
		spin_unlock_irqrestore(domain_lock, flags);	/* [한국어] 풀고 */
		ret = -EAGAIN;	/* [한국어] (원 주석: 전부 버리고 다시 시도한다) */
		goto err_free;	/* [한국어] 만든 표를 되돌린다 */
	}

	/*
	 * We do not issue any flushes for change_top on the expectation that
	 * any walk cache will not become a problem by adding another layer to
	 * the tree. Misses will rewalk from the updated top pointer, hits
	 * continue to be correct. Negative caching is fine too since all the
	 * new IOVA added by the new top is non-present.
	 */
	iommu_table->driver_ops->change_top(	/* [한국어] (원 주석: 걷기 캐시는 문제되지 않으므로 무효화하지 않는다) */
		iommu_table, virt_to_phys(table_mem), new_level);	/* [한국어] 하드웨어를 먼저 새 단계로 */
	WRITE_ONCE(common->top_of_table, new_top_of_table);	/* [한국어] 그 다음에야 소프트웨어 최상위를 바꾼다 */
	spin_unlock_irqrestore(domain_lock, flags);	/* [한국어] 성장 완료 */
	return 0;	/* [한국어] 성공 */

err_free:
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))	/* [한국어] 비일관 플랫폼이면 */
		iommu_pages_stop_incoherent_list(&free_list,	/* [한국어] 매핑을 먼저 풀고 */
						 iommu_table->iommu_device);	/* [한국어] 그 장치에서 */
	iommu_put_pages_list(&free_list);	/* [한국어] 만든 표들을 돌려준다 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * check_map_range - 범위를 담을 수 있게 만들고 확인한다
 *
 * @iommu_table: 대상 페이지 테이블.
 * @range: 담아야 할 범위.
 * @map: 매핑 인자.
 * @return: 0 성공, 음수면 담을 수 없다.
 *
 * 고정 크기 형식이면 검사만 하고 끝난다. 자랄 수 있는 형식이면, 범위가
 * 들어가지 않거나 필요한 잎 단계가 지금 최상위보다 높을 때 단계를 얹는다.
 *
 * increase_top 이 -EAGAIN 을 주면 다른 스레드가 먼저 얹은 것이므로, 새
 * 최상위를 다시 읽고 처음부터 판단한다.
 */
static int check_map_range(struct pt_iommu *iommu_table, struct pt_range *range,
			   struct pt_iommu_map_args *map)
{
	struct pt_common *common = common_from_iommu(iommu_table);	/* [한국어] 기능 질의를 위해 */
	int ret;	/* [한국어] 결과 */

	do {
		ret = pt_check_range(range);	/* [한국어] 범위가 지금 표에 들어가는가 */
		if (!pt_feature(common, PT_FEAT_DYNAMIC_TOP))	/* [한국어] 자랄 수 없는 형식이면 */
			return ret;	/* [한국어] 그 답이 최종이다 */

		if (!ret && map->leaf_level <= range->top_level)	/* [한국어] 들어가고 잎 단계도 덮으면 */
			break;	/* [한국어] 더 할 일이 없다 */

		ret = increase_top(iommu_table, range, map);	/* [한국어] 단계를 얹는다 */
		if (ret && ret != -EAGAIN)	/* [한국어] 경합이 아닌 실패면 */
			return ret;	/* [한국어] 넓힐 수 없다 */

		/* Reload the new top */
		*range = pt_make_range(common, range->va, range->last_va);	/* [한국어] (원 주석: 새 최상위를 다시 읽는다) */
	} while (ret);	/* [한국어] 경합이면 처음부터 다시 판단한다 */
	PT_WARN_ON(pt_check_range(range));	/* [한국어] 이제는 반드시 들어가야 한다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * do_map - 빠른 경로를 먼저 시도하고 안 되면 일반 경로로 매핑한다
 *
 * @range: 매핑할 범위.
 * @common: 페이지 테이블 인스턴스.
 * @single_page: 4KB 한 장인가.
 * @map: 매핑 인자.
 * @return: 0 성공, 음수면 실패.
 *
 * 일반 경로의 do-while 이 __map_range_leaf 의 -EAGAIN 을 받아 낸다 —
 * 페이지 크기가 바뀔 때마다 새 크기로 다시 도는 구조다.
 *
 * 시작 단계가 잎 단계와 같으면 내려갈 필요가 없어 곧바로 잎 워커를 쓴다.
 */
static int do_map(struct pt_range *range, struct pt_common *common,
		  bool single_page, struct pt_iommu_map_args *map)
{
	int ret;	/* [한국어] 결과 */

	/*
	 * The __map_single_page() fast path does not support DMA_INCOHERENT
	 * flushing to keep its .text small.
	 */
	if (single_page && !pt_feature(common, PT_FEAT_DMA_INCOHERENT)) {	/* [한국어] (원 주석: 빠른 경로는 .text 를 작게 두려고 비일관 플러시를 지원하지 않는다) */

		ret = pt_walk_range(range, __map_single_page, map);	/* [한국어] 표를 만들지 않는 얕은 경로 */
		if (ret != -EAGAIN)	/* [한국어] 빈 표를 만나지 않았으면 */
			return ret;	/* [한국어] 그것으로 끝 */
		/* EAGAIN falls through to the full path */
	}

	do {
		if (map->leaf_level == range->top_level)	/* [한국어] (원 주석: EAGAIN 이면 아래의 전체 경로로 넘어간다) */
			ret = pt_walk_range(range, __map_range_leaf, map);	/* [한국어] 최상위가 곧 잎 단계면 바로 채운다 */
		else
			ret = pt_walk_range(range, __map_range, map);	/* [한국어] 아니면 내려가며 표를 만든다 */
	} while (ret == -EAGAIN);	/* [한국어] 페이지 크기가 바뀔 때마다 새 크기로 다시 돈다 */
	return ret;	/* [한국어] 성패 */
}

static int NS(map_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			 phys_addr_t paddr, dma_addr_t len, unsigned int prot,
			 gfp_t gfp, size_t *mapped)
{
	pt_vaddr_t pgsize_bitmap = iommu_table->domain.pgsize_bitmap;
	struct pt_common *common = common_from_iommu(iommu_table);
	struct iommu_iotlb_gather iotlb_gather;
	struct pt_iommu_map_args map = {
		.iotlb_gather = &iotlb_gather,
		.oa = paddr,
	};
	bool single_page = false;
	struct pt_range range;
	int ret;

	iommu_iotlb_gather_init(&iotlb_gather);

	if (WARN_ON(!(prot & (IOMMU_READ | IOMMU_WRITE))))
		return -EINVAL;

	/* Check the paddr doesn't exceed what the table can store */
	if ((sizeof(pt_oaddr_t) < sizeof(paddr) &&
	     (pt_vaddr_t)paddr > PT_VADDR_MAX) ||
	    (common->max_oasz_lg2 != PT_VADDR_MAX_LG2 &&
	     oalog2_div(paddr, common->max_oasz_lg2)))
		return -ERANGE;

	ret = pt_iommu_set_prot(common, &map.attrs, prot);
	if (ret)
		return ret;
	map.attrs.gfp = gfp;

	ret = make_range_no_check(common, &range, iova, len);
	if (ret)
		return ret;

	/* Calculate target page size and level for the leaves */
	if (pt_has_system_page_size(common) && len == PAGE_SIZE) {
		PT_WARN_ON(!(pgsize_bitmap & PAGE_SIZE));
		if (log2_mod(iova | paddr, PAGE_SHIFT))
			return -ENXIO;
		map.leaf_pgsize_lg2 = PAGE_SHIFT;
		map.leaf_level = 0;
		map.num_leaves = 1;
		single_page = true;
	} else {
		map.leaf_pgsize_lg2 = pt_compute_best_pgsize(
			pgsize_bitmap, range.va, range.last_va, paddr);
		if (!map.leaf_pgsize_lg2)
			return -ENXIO;
		map.leaf_level =
			pt_pgsz_lg2_to_level(common, map.leaf_pgsize_lg2);
		map.num_leaves = pt_pgsz_count(pgsize_bitmap, range.va,
					       range.last_va, paddr,
					       map.leaf_pgsize_lg2);
	}

	ret = check_map_range(iommu_table, &range, &map);
	if (ret)
		return ret;

	PT_WARN_ON(map.leaf_level > range.top_level);

	ret = do_map(&range, common, single_page, &map);

	/*
	 * Table levels were freed and replaced with large items, flush any walk
	 * cache that may refer to the freed levels.
	 */
	if (!iommu_pages_list_empty(&iotlb_gather.freelist))
		iommu_iotlb_sync(&iommu_table->domain, &iotlb_gather);

	/* Bytes successfully mapped */
	PT_WARN_ON(!ret && map.oa - paddr != len);
	*mapped += map.oa - paddr;
	return ret;
}

struct pt_unmap_args {
	struct iommu_pages_list free_list;
	pt_vaddr_t unmapped;
};

static __maybe_unused int __unmap_range(struct pt_range *range, void *arg,
					unsigned int level,
					struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	unsigned int flush_start_index = UINT_MAX;
	unsigned int flush_end_index = UINT_MAX;
	struct pt_unmap_args *unmap = arg;
	unsigned int num_oas = 0;
	unsigned int start_index;
	int ret = 0;

	_pt_iter_first(&pts);
	start_index = pts.index;
	pts.type = pt_load_entry_raw(&pts);
	/*
	 * A starting index is in the middle of a contiguous entry
	 *
	 * The IOMMU API does not require drivers to support unmapping parts of
	 * large pages. Long ago VFIO would try to split maps but the current
	 * version never does.
	 *
	 * Instead when unmap reaches a partial unmap of the start of a large
	 * IOPTE it should remove the entire IOPTE and return that size to the
	 * caller.
	 */
	if (pts.type == PT_ENTRY_OA) {
		if (log2_mod(range->va, pt_entry_oa_lg2sz(&pts)))
			return -EINVAL;
		/* Micro optimization */
		goto start_oa;
	}

	do {
		if (pts.type != PT_ENTRY_OA) {
			bool fully_covered;

			if (pts.type != PT_ENTRY_TABLE) {
				ret = -EINVAL;
				break;
			}

			if (pts.index != start_index)
				pt_index_to_va(&pts);
			pts.table_lower = pt_table_ptr(&pts);

			fully_covered = pt_entry_fully_covered(
				&pts, pt_table_item_lg2sz(&pts));

			ret = pt_descend(&pts, arg, __unmap_range);
			if (ret)
				break;

			/*
			 * If the unmapping range fully covers the table then we
			 * can free it as well. The clear is delayed until we
			 * succeed in clearing the lower table levels.
			 */
			if (fully_covered) {
				iommu_pages_list_add(&unmap->free_list,
						     pts.table_lower);
				pt_clear_entries(&pts, ilog2(1));
				if (pts.index < flush_start_index)
					flush_start_index = pts.index;
				flush_end_index = pts.index + 1;
			}
			pts.index++;
		} else {
			unsigned int num_contig_lg2;
start_oa:
			/*
			 * If the caller requested an last that falls within a
			 * single entry then the entire entry is unmapped and
			 * the length returned will be larger than requested.
			 */
			num_contig_lg2 = pt_entry_num_contig_lg2(&pts);
			pt_clear_entries(&pts, num_contig_lg2);
			num_oas += log2_to_int(num_contig_lg2);
			if (pts.index < flush_start_index)
				flush_start_index = pts.index;
			pts.index += log2_to_int(num_contig_lg2);
			flush_end_index = pts.index;
		}
		if (pts.index >= pts.end_index)
			break;
		pts.type = pt_load_entry_raw(&pts);
	} while (true);

	unmap->unmapped += log2_mul(num_oas, pt_table_item_lg2sz(&pts));
	if (flush_start_index != flush_end_index)
		flush_writes_range(&pts, flush_start_index, flush_end_index);

	return ret;
}

static size_t NS(unmap_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			      dma_addr_t len,
			      struct iommu_iotlb_gather *iotlb_gather)
{
	struct pt_unmap_args unmap = { .free_list = IOMMU_PAGES_LIST_INIT(
					       unmap.free_list) };
	struct pt_range range;
	int ret;

	ret = make_range(common_from_iommu(iommu_table), &range, iova, len);
	if (ret)
		return 0;

	pt_walk_range(&range, __unmap_range, &unmap);

	gather_range_pages(iotlb_gather, iommu_table, iova, unmap.unmapped,
			   &unmap.free_list);

	return unmap.unmapped;
}

static void NS(get_info)(struct pt_iommu *iommu_table,
			 struct pt_iommu_info *info)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range range = pt_top_range(common);
	struct pt_state pts = pt_init_top(&range);
	pt_vaddr_t pgsize_bitmap = 0;

	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP)) {
		for (pts.level = 0; pts.level <= PT_MAX_TOP_LEVEL;
		     pts.level++) {
			if (pt_table_item_lg2sz(&pts) >= common->max_vasz_lg2)
				break;
			pgsize_bitmap |= pt_possible_sizes(&pts);
		}
	} else {
		for (pts.level = 0; pts.level <= range.top_level; pts.level++)
			pgsize_bitmap |= pt_possible_sizes(&pts);
	}

	/* Hide page sizes larger than the maximum OA */
	info->pgsize_bitmap = oalog2_mod(pgsize_bitmap, common->max_oasz_lg2);
}

static void NS(deinit)(struct pt_iommu *iommu_table)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range range = pt_all_range(common);
	struct pt_iommu_collect_args collect = {
		.free_list = IOMMU_PAGES_LIST_INIT(collect.free_list),
	};

	iommu_pages_list_add(&collect.free_list, range.top_table);
	pt_walk_range(&range, __collect_tables, &collect);

	/*
	 * The driver has to already have fenced the HW access to the page table
	 * and invalidated any caching referring to this memory.
	 */
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))
		iommu_pages_stop_incoherent_list(&collect.free_list,
						 iommu_table->iommu_device);
	iommu_put_pages_list(&collect.free_list);
}

static const struct pt_iommu_ops NS(ops) = {
	.map_range = NS(map_range),
	.unmap_range = NS(unmap_range),
#if IS_ENABLED(CONFIG_IOMMUFD_DRIVER) && defined(pt_entry_is_write_dirty) && \
	IS_ENABLED(CONFIG_IOMMUFD_TEST) && defined(pt_entry_make_write_dirty)
	.set_dirty = NS(set_dirty),
#endif
	.get_info = NS(get_info),
	.deinit = NS(deinit),
};

static int pt_init_common(struct pt_common *common)
{
	struct pt_range top_range = pt_top_range(common);

	if (PT_WARN_ON(top_range.top_level > PT_MAX_TOP_LEVEL))
		return -EINVAL;

	if (top_range.top_level == PT_MAX_TOP_LEVEL ||
	    common->max_vasz_lg2 == top_range.max_vasz_lg2)
		common->features &= ~BIT(PT_FEAT_DYNAMIC_TOP);

	if (top_range.max_vasz_lg2 == PT_VADDR_MAX_LG2)
		common->features |= BIT(PT_FEAT_FULL_VA);

	/* Requested features must match features compiled into this format */
	if ((common->features & ~(unsigned int)PT_SUPPORTED_FEATURES) ||
	    (!IS_ENABLED(CONFIG_DEBUG_GENERIC_PT) &&
	     (common->features & PT_FORCE_ENABLED_FEATURES) !=
		     PT_FORCE_ENABLED_FEATURES))
		return -EOPNOTSUPP;

	/*
	 * Check if the top level of the page table is too small to hold the
	 * specified maxvasz.
	 */
	if (!pt_feature(common, PT_FEAT_DYNAMIC_TOP) &&
	    top_range.top_level != PT_MAX_TOP_LEVEL) {
		struct pt_state pts = { .range = &top_range,
					.level = top_range.top_level };

		if (common->max_vasz_lg2 >
		    pt_num_items_lg2(&pts) + pt_table_item_lg2sz(&pts))
			return -EOPNOTSUPP;
	}

	if (common->max_oasz_lg2 == 0)
		common->max_oasz_lg2 = pt_max_oa_lg2(common);
	else
		common->max_oasz_lg2 = min(common->max_oasz_lg2,
					   pt_max_oa_lg2(common));
	return 0;
}

static int pt_iommu_init_domain(struct pt_iommu *iommu_table,
				struct iommu_domain *domain)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_iommu_info info;
	struct pt_range range;

	NS(get_info)(iommu_table, &info);

	domain->type = __IOMMU_DOMAIN_PAGING;
	domain->pgsize_bitmap = info.pgsize_bitmap;
	domain->is_iommupt = true;

	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP))
		range = _pt_top_range(common,
				      _pt_top_set(NULL, PT_MAX_TOP_LEVEL));
	else
		range = pt_top_range(common);

	/* A 64-bit high address space table on a 32-bit system cannot work. */
	domain->geometry.aperture_start = (unsigned long)range.va;
	if ((pt_vaddr_t)domain->geometry.aperture_start != range.va)
		return -EOVERFLOW;

	/*
	 * The aperture is limited to what the API can do after considering all
	 * the different types dma_addr_t/unsigned long/pt_vaddr_t that are used
	 * to store a VA. Set the aperture to something that is valid for all
	 * cases. Saturate instead of truncate the end if the types are smaller
	 * than the top range. aperture_end should be called aperture_last.
	 */
	domain->geometry.aperture_end = (unsigned long)range.last_va;
	if ((pt_vaddr_t)domain->geometry.aperture_end != range.last_va) {
		domain->geometry.aperture_end = ULONG_MAX;
		domain->pgsize_bitmap &= ULONG_MAX;
	}
	domain->geometry.force_aperture = true;

	return 0;
}

static void pt_iommu_zero(struct pt_iommu_table *fmt_table)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_iommu cfg = *iommu_table;

	static_assert(offsetof(struct pt_iommu_table, iommu.domain) == 0);
	memset_after(fmt_table, 0, iommu.domain);

	/* The caller can initialize some of these values */
	iommu_table->iommu_device = cfg.iommu_device;
	iommu_table->driver_ops = cfg.driver_ops;
	iommu_table->nid = cfg.nid;
}

#define pt_iommu_table_cfg CONCATENATE(pt_iommu_table, _cfg)
#define pt_iommu_init CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), init)

int pt_iommu_init(struct pt_iommu_table *fmt_table,
		  const struct pt_iommu_table_cfg *cfg, gfp_t gfp)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_table_p *table_mem;
	int ret;

	if (cfg->common.hw_max_vasz_lg2 > PT_MAX_VA_ADDRESS_LG2 ||
	    !cfg->common.hw_max_vasz_lg2 || !cfg->common.hw_max_oasz_lg2)
		return -EINVAL;

	pt_iommu_zero(fmt_table);
	common->features = cfg->common.features;
	common->max_vasz_lg2 = cfg->common.hw_max_vasz_lg2;
	common->max_oasz_lg2 = cfg->common.hw_max_oasz_lg2;
	ret = pt_iommu_fmt_init(fmt_table, cfg);
	if (ret)
		return ret;

	if (cfg->common.hw_max_oasz_lg2 > pt_max_oa_lg2(common))
		return -EINVAL;

	ret = pt_init_common(common);
	if (ret)
		return ret;

	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP) &&
	    WARN_ON(!iommu_table->driver_ops ||
		    !iommu_table->driver_ops->change_top ||
		    !iommu_table->driver_ops->get_top_lock))
		return -EINVAL;

	if (pt_feature(common, PT_FEAT_SIGN_EXTEND) &&
	    (pt_feature(common, PT_FEAT_FULL_VA) ||
	     pt_feature(common, PT_FEAT_DYNAMIC_TOP)))
		return -EINVAL;

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT) &&
	    WARN_ON(!iommu_table->iommu_device))
		return -EINVAL;

	ret = pt_iommu_init_domain(iommu_table, &iommu_table->domain);
	if (ret)
		return ret;

	table_mem = table_alloc_top(common, common->top_of_table, gfp,
				    ALLOC_NORMAL);
	if (IS_ERR(table_mem))
		return PTR_ERR(table_mem);
	pt_top_set(common, table_mem, pt_top_get_level(common));

	/* Must be last, see pt_iommu_deinit() */
	iommu_table->ops = &NS(ops);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(pt_iommu_init, "GENERIC_PT_IOMMU");

#ifdef pt_iommu_fmt_hw_info
#define pt_iommu_table_hw_info CONCATENATE(pt_iommu_table, _hw_info)
#define pt_iommu_hw_info CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), hw_info)
void pt_iommu_hw_info(struct pt_iommu_table *fmt_table,
		      struct pt_iommu_table_hw_info *info)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range top_range = pt_top_range(common);

	pt_iommu_fmt_hw_info(fmt_table, &top_range, info);
}
EXPORT_SYMBOL_NS_GPL(pt_iommu_hw_info, "GENERIC_PT_IOMMU");
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IOMMU Page table implementation for " __stringify(PTPFX_RAW));
MODULE_IMPORT_NS("GENERIC_PT");
/* For iommu_dirty_bitmap_record() */
MODULE_IMPORT_NS("IOMMUFD");

#endif  /* __GENERIC_PT_IOMMU_PT_H */
