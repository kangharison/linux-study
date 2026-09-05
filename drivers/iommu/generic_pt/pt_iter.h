/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * Iterators for Generic Page Table
 */
/*
 * [한국어 설명] 페이지 테이블 순회기 (pt_iter.h)
 *
 * === 파일의 역할 ===
 * 형식 API 위에 "표를 어떻게 걷는가"를 얹는다. 범위를 만들고, 각 단계에서
 * 시작·끝 색인을 계산하고, 항목을 하나씩 넘기고, 아래 단계로 내려간다.
 *
 * 두 가지 설계 결정이 이 파일의 성격을 정한다.
 *
 * 첫째, VA 를 게으르게 계산한다. 순회는 색인으로 진행하고, 실제 VA 는
 * 필요할 때만 pt_index_to_va() 로 되짚는다 — VA 계산이 몇 개의 명령을
 * 잡아먹는데 대부분의 걸음에서는 쓰이지 않기 때문이다.
 *
 * 둘째, 재귀를 단계별 함수로 펼친다. PT_MAKE_LEVELS 매크로가 단계마다
 * 별도의 인라인 함수를 만들고, 각 함수는 자기 단계 번호를 컴파일 시
 * 상수로 본다. 그래서 항목 크기·마스크·시프트가 전부 상수로 접힌다.
 * 원 주석이 "코드가 많이 생길 수 있다"고 경고하는 이유이며, 동시에 이
 * 계층이 손으로 쓴 드라이버 코드와 같은 속도를 내는 이유다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pt_common.h(형식 API) → [이 파일](순회) → iommu_pt.h(IOMMU 진입점)
 *
 * 실행 컨텍스트: 전부 인라인. map/unmap/iova_to_phys 가 부르는 문맥을
 * 그대로 따른다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommu_pt.h 의 map/unmap/walk 구현과 kunit 시험들.
 * 아래: pt_common.h 의 형식 API, pt_defs.h 의 산술.
 *
 * 데이터 흐름: 드라이버의 VA 범위 → struct pt_range → 단계별 색인 →
 * 형식 접근자 → 표 항목.
 *
 * === 주요 함수/구조체 요약 ===
 * pt_check_range: 범위가 이 표의 사정거리 안인지, 부호 확장이 맞는지 본다.
 * pt_range_to_index / pt_range_to_end_index: 한 단계에서 훑을 색인 구간.
 * for_each_pt_level_entry: 그 구간을 도는 기본 반복문.
 * pt_top_range / pt_all_range / pt_upper_range: 부호 확장 형식에서 위·아래
 *   절반을 각각 가리키는 범위. pt_all_range 는 색인 계산만 맞추려고 VA 를
 *   일부러 틀리게 둔다.
 * pt_descend / pt_walk_descend: 아래 단계로 재귀한다.
 * pt_compute_best_pgsize: VA·OA 정렬과 남은 길이를 함께 보고 쓸 수 있는
 *   가장 큰 페이지 크기를 고른다. 매핑 성능의 핵심이다.
 * PT_MAKE_LEVELS: 단계별로 펼쳐진 워커 함수 묶음을 만들어 낸다.
 */
#ifndef __GENERIC_PT_PT_ITER_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_PT_ITER_H	/* [한국어] 같은 이름으로 표시 */

#include "pt_common.h"	/* [한국어] 형식 API */

#include <linux/errno.h>	/* [한국어] -ERANGE, -EINVAL 등 */

/*
 * Use to mangle symbols so that backtraces and the symbol table are
 * understandable. Any non-inlined function should get mangled like this.
 */
#define NS(fn) CONCATENATE(PTPFX, fn)	/* [한국어] (원 주석: 역추적과 심볼 표가 읽히도록 이름을 형식별로 바꾼다) */

/**
 * pt_check_range() - Validate the range can be iterated
 * @range: Range to validate
 *
 * Check that VA and last_va fall within the permitted range of VAs. If the
 * format is using PT_FEAT_SIGN_EXTEND then this also checks the sign extension
 * is correct.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_check_range - 이 범위를 실제로 걸을 수 있는지 검사한다
 *
 * @range: 검사할 범위.
 * @return: 0 이면 유효, -ERANGE 면 표의 사정거리 밖이다.
 *
 * 부호 확장 형식이 이 함수를 복잡하게 만든다. 그런 형식에서 유효한 주소는
 * 아래쪽 절반(상위 비트가 모두 0)이거나 위쪽 절반(모두 1)이고, 그 사이의
 * 값은 존재하지 않는다.
 *
 * 그래서 시작 주소의 최상위 비트를 보고 어느 절반인지 정한 뒤, 시작과 끝이
 * 모두 그 접두를 갖는지 확인한다. 한 범위가 두 절반에 걸치면 여기서 걸린다.
 *
 * 부호 확장이 없는 형식이면 접두가 상수이므로 검사가 단순해진다.
 */
static inline int pt_check_range(struct pt_range *range)
{
	pt_vaddr_t prefix;	/* [한국어] 유효한 주소가 가져야 할 상위 비트 패턴 */

	PT_WARN_ON(!range->max_vasz_lg2);	/* [한국어] 폭이 0 이면 범위를 만들 때 이미 잘못됐다 */

	if (pt_feature(range->common, PT_FEAT_SIGN_EXTEND)) {	/* [한국어] 부호 확장 형식이면 */
		PT_WARN_ON(range->common->max_vasz_lg2 != range->max_vasz_lg2);	/* [한국어] 이 형식은 최상위를 키우지 않으므로 폭이 같아야 한다 */
		prefix = fvalog2_div(range->va, range->max_vasz_lg2 - 1) ?	/* [한국어] 최상위 비트를 보고 */
				 PT_VADDR_MAX :	/* [한국어] 위쪽 절반이면 상위가 모두 1 */
				 0;	/* [한국어] 아래쪽 절반이면 모두 0 */
	} else {
		prefix = pt_full_va_prefix(range->common);	/* [한국어] 그 밖에는 형식이 정한 고정 접두 */
	}

	if (!fvalog2_div_eq(range->va, prefix, range->max_vasz_lg2) ||	/* [한국어] 시작이 그 접두를 갖는가 */
	    !fvalog2_div_eq(range->last_va, prefix, range->max_vasz_lg2))	/* [한국어] 끝도 같은 접두인가 — 두 절반에 걸친 범위는 여기서 걸린다 */
		return -ERANGE;	/* [한국어] 표의 사정거리 밖 */
	return 0;	/* [한국어] 걸을 수 있다 */
}

/**
 * pt_index_to_va() - Update range->va to the current pts->index
 * @pts: Iteration State
 *
 * Adjust range->va to match the current index. This is done in a lazy manner
 * since computing the VA takes several instructions and is rarely required.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_index_to_va - 현재 색인에 맞게 range->va 를 고친다
 *
 * @pts: 순회 상태.
 *
 * 순회는 색인으로 진행하고 VA 는 뒤늦게 따라온다. 원 주석이 그 이유를
 * 밝힌다 — VA 계산에 몇 개의 명령이 들지만 대부분의 걸음에서는 쓰이지
 * 않는다.
 *
 * 이 단계의 색인이 정하는 것은 VA 의 중간 비트뿐이다. 위 단계들이 정한
 * 상위 비트는 그대로 두어야 하므로, 이 표가 덮는 범위 안의 비트만 갈아
 * 끼운다.
 */
static inline void pt_index_to_va(struct pt_state *pts)
{
	pt_vaddr_t lower_va;	/* [한국어] 이 단계의 색인이 정하는 비트들 */

	lower_va = log2_mul(pts->index, pt_table_item_lg2sz(pts));	/* [한국어] 색인 × 항목 크기 */
	pts->range->va = fvalog2_set_mod(pts->range->va, lower_va,	/* [한국어] 위 단계들이 정한 상위 비트는 그대로 두고 */
					 pt_table_oa_lg2sz(pts));	/* [한국어] 이 표가 덮는 범위의 비트만 갈아 끼운다 */
}

/*
 * Add index_count_lg2 number of entries to pts's VA and index. The VA will be
 * adjusted to the end of the contiguous block if it is currently in the middle.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _pt_advance - 색인을 여러 항목만큼 건너뛴다
 *
 * @pts: 순회 상태.
 * @index_count_lg2: 건너뛸 항목 수의 지수.
 *
 * 연속 항목을 지날 때 쓴다. 더한 뒤 하위 비트를 0 으로 깎는 것이 요점이다 —
 * 순회가 묶음 한가운데에서 시작했더라도 다음 걸음은 묶음 경계에 서게 된다.
 */
static inline void _pt_advance(struct pt_state *pts,
			       unsigned int index_count_lg2)
{
	pts->index = log2_set_mod(pts->index + log2_to_int(index_count_lg2), 0,	/* [한국어] 묶음 크기만큼 더한 뒤 */
				  index_count_lg2);	/* [한국어] 하위 비트를 깎아 묶음 경계에 세운다 */
}

/**
 * pt_entry_fully_covered() - Check if the item or entry is entirely contained
 *                            within pts->range
 * @pts: Iteration State
 * @oasz_lg2: The size of the item to check, pt_table_item_lg2sz() or
 *            pt_entry_oa_lg2sz()
 *
 * Returns: true if the item is fully enclosed by the pts->range.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_entry_fully_covered - 이 항목이 요청 범위 안에 온전히 들어가는가
 *
 * @pts: 순회 상태.
 * @oasz_lg2: 볼 크기(단일 항목 또는 entry 전체).
 * @return: 온전히 들어가면 참.
 *
 * unmap 에서 결정적인 판정이다. 온전히 덮이면 그 항목을 그냥 지우면 되지만,
 * 일부만 걸치면 큰 페이지를 잘게 쪼개고 나서 지워야 한다.
 *
 * 세 단계로 답한다: 범위가 항목 중간에서 시작하면 거짓, 항목 끝을 넘어
 * 계속되면 참, 정확히 항목 끝에서 끝나면 참이다.
 */
static inline bool pt_entry_fully_covered(const struct pt_state *pts,
					  unsigned int oasz_lg2)
{
	struct pt_range *range = pts->range;	/* [한국어] 요청 범위 */

	/* Range begins at the start of the entry */
	if (log2_mod(pts->range->va, oasz_lg2))	/* [한국어] (원 주석: 범위가 항목의 시작에서 시작하는가) */
		return false;	/* [한국어] 한가운데에서 시작하면 덮이지 않는다 */

	/* Range ends past the end of the entry */
	if (!log2_div_eq(range->va, range->last_va, oasz_lg2))	/* [한국어] (원 주석: 범위가 항목 끝을 지나 계속되는가) */
		return true;	/* [한국어] 항목을 온전히 삼킨다 */

	/* Range ends at the end of the entry */
	return log2_mod_eq_max(range->last_va, oasz_lg2);	/* [한국어] (원 주석: 범위가 항목 끝에서 끝나는가) */
}

/**
 * pt_range_to_index() - Starting index for an iteration
 * @pts: Iteration State
 *
 * Return: the starting index for the iteration in pts.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_range_to_index - 이 단계에서 순회를 시작할 색인
 *
 * @pts: 순회 상태.
 * @return: 시작 색인.
 *
 * 일반 단계에서는 VA 의 해당 비트 조각을 꺼내면 된다 — 항목 크기로 나누고
 * 항목 수로 나눈 나머지.
 *
 * 최상위 단계만 다르다. 그쪽 항목 수는 형식이 아니라 인스턴스의 주소 공간
 * 폭이 정하므로, 폭 밖의 비트를 먼저 떼어 낸 뒤 나눈다.
 */
static inline unsigned int pt_range_to_index(const struct pt_state *pts)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */

	PT_WARN_ON(pts->level > pts->range->top_level);	/* [한국어] 최상위보다 위 단계는 없다 */
	if (pts->range->top_level == pts->level)	/* [한국어] 최상위 단계면 */
		return log2_div(fvalog2_mod(pts->range->va,	/* [한국어] 주소 공간 폭 밖의 비트를 먼저 떼고 */
					    pts->range->max_vasz_lg2),	/* [한국어] 항목 수는 인스턴스의 폭이 정한다 */
				isz_lg2);	/* [한국어] 항목 크기로 나눈다 */
	return log2_mod(log2_div(pts->range->va, isz_lg2),	/* [한국어] 일반 단계는 VA 의 해당 비트 조각을 */
			pt_num_items_lg2(pts));	/* [한국어] 항목 수로 나눈 나머지가 색인이다 */
}

/**
 * pt_range_to_end_index() - Ending index iteration
 * @pts: Iteration State
 *
 * Return: the last index for the iteration in pts.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_range_to_end_index - 이 단계에서 순회를 끝낼 색인(배타적)
 *
 * @pts: 순회 상태.
 * @return: 마지막 다음 색인.
 *
 * 세 경우로 갈린다.
 *  - 범위가 주소 하나뿐이면 지금 색인 다음까지.
 *  - 최상위 단계면 시작 색인과 같은 방식으로 끝 주소를 옮긴다.
 *  - 그 밖에는 끝 주소가 이 표 안에 있는지를 먼저 본다. 안에 있으면 그
 *    자리까지, 표를 넘어가면 표 끝까지 — 나머지는 다음 표가 맡는다.
 */
static inline unsigned int pt_range_to_end_index(const struct pt_state *pts)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */
	struct pt_range *range = pts->range;	/* [한국어] 요청 범위 */
	unsigned int num_entries_lg2;	/* [한국어] 이 표의 항목 수(지수) */

	if (range->va == range->last_va)	/* [한국어] 주소 하나뿐이면 */
		return pts->index + 1;	/* [한국어] 그 항목 하나만 */

	if (pts->range->top_level == pts->level)	/* [한국어] 최상위 단계면 */
		return log2_div(fvalog2_mod(pts->range->last_va,	/* [한국어] 끝 주소도 같은 방식으로 옮기고 */
					    pts->range->max_vasz_lg2),	/* [한국어] 폭 밖의 비트를 떼고 */
				isz_lg2) +	/* [한국어] 항목 크기로 나눈 뒤 */
		       1;	/* [한국어] 배타적 끝으로 만든다 */

	num_entries_lg2 = pt_num_items_lg2(pts);	/* [한국어] 이 표의 항목 수 */

	/* last_va falls within this table */
	if (log2_div_eq(range->va, range->last_va, num_entries_lg2 + isz_lg2))	/* [한국어] (원 주석: 끝 주소가 이 표 안에 있다) */
		return log2_mod(log2_div(pts->range->last_va, isz_lg2),	/* [한국어] 그 자리까지만 */
				num_entries_lg2) +	/* [한국어] 색인으로 옮기고 */
		       1;	/* [한국어] 배타적 끝으로 */

	return log2_to_int(num_entries_lg2);	/* [한국어] 표를 넘어가면 표 끝까지 — 나머지는 다음 표가 맡는다 */
}

/*
 * [한국어]
 * _pt_iter_first - 반복문의 초기화 단계
 *
 * @pts: 순회 상태.
 *
 * 시작과 끝 색인을 함께 계산해 둔다. 매 걸음 끝을 다시 계산하지 않기
 * 위해서다.
 */
static inline void _pt_iter_first(struct pt_state *pts)
{
	pts->index = pt_range_to_index(pts);	/* [한국어] 시작 색인 */
	pts->end_index = pt_range_to_end_index(pts);	/* [한국어] 끝 색인 — 매 걸음 다시 계산하지 않는다 */
	PT_WARN_ON(pts->index > pts->end_index);	/* [한국어] 뒤집혔으면 계산이 잘못됐다 */
}

/*
 * [한국어]
 * _pt_iter_load - 반복문의 조건 단계
 *
 * @pts: 순회 상태.
 * @return: 아직 볼 항목이 남았으면 참.
 *
 * 조건 검사와 항목 읽기를 한 번에 한다. 그래서 반복문 몸통은 바로
 * pts->type 을 보고 분기할 수 있다.
 */
static inline bool _pt_iter_load(struct pt_state *pts)
{
	if (pts->index >= pts->end_index)	/* [한국어] 구간을 다 돌았으면 */
		return false;	/* [한국어] 반복문을 끝낸다 */
	pt_load_entry(pts);	/* [한국어] 조건 검사와 항목 읽기를 한 번에 */
	return true;	/* [한국어] 몸통이 바로 pts->type 을 볼 수 있다 */
}

/**
 * pt_next_entry() - Advance pts to the next entry
 * @pts: Iteration State
 *
 * Update pts to go to the next index at this level. If pts is pointing at a
 * contiguous entry then the index may advance my more than one.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_next_entry - 다음 항목으로 넘어간다
 *
 * @pts: 순회 상태.
 *
 * 연속 항목을 만나면 그 묶음 전체를 한 걸음에 지난다. 묶음 안의 항목들은
 * 같은 매핑의 일부라 따로 볼 이유가 없다.
 *
 * __builtin_constant_p 검사가 특이한데, 연속을 지원하지 않는 형식에서는
 * 묶음 크기가 컴파일 시 0 이 되므로 그 갈래를 아예 없애 코드를 줄인다.
 */
static inline void pt_next_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_OA &&	/* [한국어] 잎 항목이고 */
	    !__builtin_constant_p(pt_entry_num_contig_lg2(pts) == 0))	/* [한국어] 연속을 지원하지 않는 형식이면 이 갈래를 아예 없앤다 */
		_pt_advance(pts, pt_entry_num_contig_lg2(pts));	/* [한국어] 묶음 전체를 한 걸음에 지난다 */
	else
		pts->index++;	/* [한국어] 그 밖에는 한 항목씩 */
	pt_index_to_va(pts);	/* [한국어] VA 를 색인에 맞춘다 */
}

/**
 * for_each_pt_level_entry() - For loop wrapper over entries in the range
 * @pts: Iteration State
 *
 * This is the basic iteration primitive. It iterates over all the entries in
 * pts->range that fall within the pts's current table level. Each step does
 * pt_load_entry(pts).
 */
/*
 * [한국어] (위 kdoc 과 함께 읽을 것)
 * 한 단계 안의 항목들을 도는 기본 반복문.
 * 초기화에서 색인 구간을 잡고, 조건에서 항목을 읽고, 증가에서 다음으로
 * 넘어간다 — 세 자리가 각각 위 세 함수에 대응한다.
 */
#define for_each_pt_level_entry(pts) \
	for (_pt_iter_first(pts); _pt_iter_load(pts); pt_next_entry(pts))	/* [한국어] 초기화·조건·증가가 각각 위 세 함수에 대응한다 */

/**
 * pt_load_single_entry() - Version of pt_load_entry() usable within a walker
 * @pts: Iteration State
 *
 * Alternative to for_each_pt_level_entry() if the walker function uses only a
 * single entry.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_load_single_entry - 항목 하나만 볼 때 쓰는 간이 진입
 *
 * @pts: 순회 상태.
 * @return: 그 항목의 종류.
 *
 * iova_to_phys 처럼 한 주소만 따라 내려가는 경우, 끝 색인을 계산하고
 * 반복문을 돌 이유가 없다.
 */
static inline enum pt_entry_type pt_load_single_entry(struct pt_state *pts)
{
	pts->index = pt_range_to_index(pts);	/* [한국어] 시작 색인만 구하고 */
	pt_load_entry(pts);	/* [한국어] 그 항목을 읽는다 */
	return pts->type;	/* [한국어] 끝 색인도 반복문도 필요 없다 */
}

/*
 * [한국어]
 * _pt_top_range - 합쳐 둔 최상위 워드에서 범위를 만든다
 *
 * @common: 페이지 테이블 인스턴스.
 * @top_of_table: 한 번에 읽어 온 최상위 워드(표 주소 + 단계).
 * @return: 최상위 단계를 덮는 범위.
 *
 * 값을 인자로 받는 것이 요점이다. 호출자가 READ_ONCE 로 한 번만 읽어
 * 넘기므로, 이 함수가 도는 동안 최상위가 바뀌어도 주소와 단계가 어긋나지
 * 않는다(pt_defs.h 의 pt_top_set 참고).
 *
 * DYNAMIC_TOP 형식에서는 최상위가 아직 낮을 수 있어, 인스턴스가 허용한
 * 폭이 아니라 현재 표가 실제로 덮는 폭을 써야 한다.
 *
 * 부호 확장 형식이면 폭에서 1 을 빼 아래쪽 절반만 가리키게 한다 —
 * 원 주석이 그 기본 동작을 밝힌다.
 */
static __always_inline struct pt_range _pt_top_range(struct pt_common *common,
						     uintptr_t top_of_table)
{
	struct pt_range range = {	/* [한국어] 만들 범위 */
		.common = common,	/* [한국어] 인스턴스 */
		.top_table =	/* [한국어] 최상위 표 주소 */
			(struct pt_table_p *)(top_of_table &	/* [한국어] 합쳐 둔 워드에서 */
					      ~(uintptr_t)PT_TOP_LEVEL_MASK),	/* [한국어] 단계 비트를 떼어 낸 나머지 */
		.top_level = top_of_table % (1 << PT_TOP_LEVEL_BITS),	/* [한국어] 그 단계 비트 */
	};
	struct pt_state pts = { .range = &range, .level = range.top_level };	/* [한국어] 단계별 크기를 묻기 위한 임시 상태 */
	unsigned int max_vasz_lg2;	/* [한국어] 이 범위가 실제로 덮는 폭 */

	max_vasz_lg2 = common->max_vasz_lg2;	/* [한국어] 인스턴스가 허용한 폭에서 시작 */
	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP) &&	/* [한국어] 최상위가 자랄 수 있는 형식이고 */
	    pts.level != PT_MAX_TOP_LEVEL)	/* [한국어] 아직 다 자라지 않았으면 */
		max_vasz_lg2 = min_t(unsigned int, common->max_vasz_lg2,	/* [한국어] 현재 표가 실제로 덮는 만큼으로 좁힌다 */
				     pt_num_items_lg2(&pts) +	/* [한국어] 항목 수와 */
					     pt_table_item_lg2sz(&pts));	/* [한국어] 항목 크기를 합한 폭 */

	/*
	 * The top range will default to the lower region only with sign extend.
	 */
	range.max_vasz_lg2 = max_vasz_lg2;	/* [한국어] 범위에 기록 */
	if (pt_feature(common, PT_FEAT_SIGN_EXTEND))	/* [한국어] (원 주석: 부호 확장이면 최상위 범위는 아래쪽 절반이 기본이다) */
		max_vasz_lg2--;	/* [한국어] 절반만 덮게 폭을 하나 줄인다 */

	range.va = fvalog2_set_mod(pt_full_va_prefix(common), 0, max_vasz_lg2);	/* [한국어] 그 절반의 첫 주소 */
	range.last_va =	/* [한국어] 그 절반의 마지막 주소 */
		fvalog2_set_mod_max(pt_full_va_prefix(common), max_vasz_lg2);	/* [한국어] 하위 비트를 모두 세운다 */
	return range;	/* [한국어] 완성된 범위 */
}

/**
 * pt_top_range() - Return a range that spans part of the top level
 * @common: Table
 *
 * For PT_FEAT_SIGN_EXTEND this will return the lower range, and cover half the
 * total page table. Otherwise it returns the entire page table.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_top_range - 최상위를 안전하게 읽어 범위를 만든다
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 부호 확장 형식이면 아래쪽 절반, 아니면 표 전체.
 *
 * 원 주석이 잠금 계약을 밝힌다: 최상위 포인터는 락 없이 바뀔 수 있고,
 * 여기서 값과 단계를 찢어지지 않게 한 번에 잡아 두면 그 뒤로는 안전하게
 * 걸을 수 있다.
 */
static __always_inline struct pt_range pt_top_range(struct pt_common *common)
{
	/*
	 * The top pointer can change without locking. We capture the value and
	 * it's level here and are safe to walk it so long as both values are
	 * captured without tearing.
	 */
	return _pt_top_range(common, READ_ONCE(common->top_of_table));	/* [한국어] (원 주석: 주소와 단계를 찢어지지 않게 한 번에 잡으면 그 뒤로는 안전하다) */
}

/**
 * pt_all_range() - Return a range that spans the entire page table
 * @common: Table
 *
 * The returned range spans the whole page table. Due to how PT_FEAT_SIGN_EXTEND
 * is supported range->va and range->last_va will be incorrect during the
 * iteration and must not be accessed.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_all_range - 표 전체를 도는 범위
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 표 전체를 덮는 범위.
 *
 * 부호 확장 형식에서 이 함수는 일부러 틀린 VA 를 만든다. 원 주석이 그것을
 * 명시한다 — 순회 중 va/last_va 를 읽으면 안 된다.
 *
 * 이유는 색인이다. 위·아래 절반을 따로 도는 대신 0 부터 선형으로 훑는 척
 * 하면 색인 계산이 표 전체를 정확히 덮는다. VA 가 필요 없는 작업(표를
 * 통째로 해제하는 등)에만 쓴다.
 */
static inline struct pt_range pt_all_range(struct pt_common *common)
{
	struct pt_range range = pt_top_range(common);	/* [한국어] 아래쪽 절반에서 시작 */

	if (!pt_feature(common, PT_FEAT_SIGN_EXTEND))	/* [한국어] 부호 확장이 없으면 */
		return range;	/* [한국어] 그것이 이미 표 전체다 */

	/*
	 * Pretend the table is linear from 0 without a sign extension. This
	 * generates the correct indexes for iteration.
	 */
	range.last_va = fvalog2_set_mod_max(0, range.max_vasz_lg2);	/* [한국어] (원 주석: 부호 확장 없이 0 부터 선형인 척한다 — 그래야 색인이 표 전체를 덮는다) */
	return range;	/* [한국어] VA 는 틀리지만 색인은 맞다 */
}

/**
 * pt_upper_range() - Return a range that spans part of the top level
 * @common: Table
 *
 * For PT_FEAT_SIGN_EXTEND this will return the upper range, and cover half the
 * total page table. Otherwise it returns the entire page table.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_upper_range - 부호 확장 형식의 위쪽 절반을 덮는 범위
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 위쪽 절반, 부호 확장이 없으면 표 전체.
 *
 * 위쪽 절반의 주소는 상위 비트가 모두 1 이다. 시작은 그 절반의 첫 주소,
 * 끝은 주소 최대값이 된다.
 */
static inline struct pt_range pt_upper_range(struct pt_common *common)
{
	struct pt_range range = pt_top_range(common);	/* [한국어] 아래쪽 절반에서 시작 */

	if (!pt_feature(common, PT_FEAT_SIGN_EXTEND))	/* [한국어] 부호 확장이 없으면 */
		return range;	/* [한국어] 절반이라는 개념이 없다 */

	range.va = fvalog2_set_mod(PT_VADDR_MAX, 0, range.max_vasz_lg2 - 1);	/* [한국어] 위쪽 절반의 첫 주소 — 상위 비트가 모두 1 */
	range.last_va = PT_VADDR_MAX;	/* [한국어] 끝은 주소 최대값 */
	return range;	/* [한국어] 위쪽 절반 */
}

/**
 * pt_make_range() - Return a range that spans part of the table
 * @common: Table
 * @va: Start address
 * @last_va: Last address
 *
 * The caller must validate the range with pt_check_range() before using it.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_make_range - 임의의 VA 구간을 덮는 범위를 만든다
 *
 * @common: 페이지 테이블 인스턴스.
 * @va: 시작 주소.
 * @last_va: 마지막 주소(포함).
 * @return: 그 구간의 범위.
 *
 * 검사는 하지 않는다 — 호출자가 pt_check_range 로 확인해야 한다. 그렇게
 * 나눈 이유는 진입점마다 실패 처리 방식이 다르기 때문이다.
 */
static __always_inline struct pt_range
pt_make_range(struct pt_common *common, pt_vaddr_t va, pt_vaddr_t last_va)
{
	struct pt_range range =	/* [한국어] 최상위 정보를 담은 범위에서 시작해 */
		_pt_top_range(common, READ_ONCE(common->top_of_table));	/* [한국어] 찢어지지 않게 한 번에 읽는다 */

	range.va = va;	/* [한국어] 구간만 호출자가 준 값으로 */
	range.last_va = last_va;	/* [한국어] 갈아 끼운다 */

	return range;	/* [한국어] 검사는 호출자 몫이다 */
}

/*
 * Span a slice of the table starting at a lower table level from an active
 * walk.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_make_child_range - 걷는 도중 아래 표의 일부를 가리키는 범위를 만든다
 *
 * @parent: 지금 걷고 있는 범위.
 * @va: 새 시작 주소.
 * @last_va: 새 마지막 주소.
 * @return: 같은 표를 가리키되 구간만 좁힌 범위.
 *
 * 부모를 통째로 복사하므로 최상위 표와 단계가 그대로 이어진다 — 새로
 * 읽으면 그사이 최상위가 바뀌어 걷던 표와 어긋날 수 있다.
 */
static __always_inline struct pt_range
pt_make_child_range(const struct pt_range *parent, pt_vaddr_t va,
		    pt_vaddr_t last_va)
{
	struct pt_range range = *parent;	/* [한국어] 최상위 표와 단계를 그대로 물려받는다 */

	range.va = va;	/* [한국어] 구간만 좁히고 */
	range.last_va = last_va;	/* [한국어] 끝도 새로 */

	PT_WARN_ON(last_va < va);	/* [한국어] 뒤집힌 범위 */
	PT_WARN_ON(pt_check_range(&range));	/* [한국어] 부모 안에 들어 있어야 한다 */

	return range;	/* [한국어] 새 범위 */
}

/**
 * pt_init() - Initialize a pt_state on the stack
 * @range: Range pointer to embed in the state
 * @level: Table level for the state
 * @table: Pointer to the table memory at level
 *
 * Helper to initialize the on-stack pt_state from walker arguments.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_init - 스택 위에 순회 상태를 만든다
 *
 * @range: 이 상태가 속할 범위.
 * @level: 시작 단계.
 * @table: 그 단계의 표.
 * @return: 초기화된 상태.
 *
 * 워커 함수가 받는 인자들을 상태 하나로 묶어 준다. 색인은 아직 비어 있고,
 * 반복문이 시작될 때 계산된다.
 */
static __always_inline struct pt_state
pt_init(struct pt_range *range, unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = {	/* [한국어] 워커 인자들을 상태 하나로 */
		.range = range,	/* [한국어] 다룰 범위 */
		.table = table,	/* [한국어] 이 단계의 표 */
		.level = level,	/* [한국어] 단계 번호 — 색인은 반복문이 채운다 */
	};
	return pts;	/* [한국어] 초기화된 상태 */
}

/**
 * pt_init_top() - Initialize a pt_state on the stack
 * @range: Range pointer to embed in the state
 *
 * The pt_state points to the top most level.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_init_top - 최상위 단계에서 시작하는 순회 상태
 *
 * @range: 이 상태가 속할 범위.
 * @return: 초기화된 상태.
 */
static __always_inline struct pt_state pt_init_top(struct pt_range *range)
{
	return pt_init(range, range->top_level, range->top_table);	/* [한국어] 범위가 들고 있는 최상위 정보로 */
}

/*
 * [한국어] 워커 함수의 모양.
 * 단계와 표를 인자로 받아 그 단계를 처리하고, 필요하면 스스로 아래
 * 단계 함수를 부른다. PT_MAKE_LEVELS 가 이 모양의 함수를 단계마다
 * 만들어 낸다.
 */
typedef int (*pt_level_fn_t)(struct pt_range *range, void *arg,
			     unsigned int level, struct pt_table_p *table);

/**
 * pt_descend() - Recursively invoke the walker for the lower level
 * @pts: Iteration State
 * @arg: Value to pass to the function
 * @fn: Walker function to call
 *
 * pts must point to a table item. Invoke fn as a walker on the table
 * pts points to.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_descend - 지금 항목이 가리키는 아래 표로 재귀한다
 *
 * @pts: 순회 상태(표 항목을 가리키고 있어야 한다).
 * @arg: 워커에 넘길 값.
 * @fn: 아래 단계를 처리할 워커.
 * @return: 워커의 결과.
 *
 * 아래 표 주소는 pt_load_entry 가 이미 구해 두었다. 같은 범위를 그대로
 * 넘기므로, 아래 단계가 자기 몫의 색인 구간을 스스로 계산한다.
 */
static __always_inline int pt_descend(struct pt_state *pts, void *arg,
				      pt_level_fn_t fn)
{
	int ret;	/* [한국어] 워커의 결과 */

	if (PT_WARN_ON(!pts->table_lower))	/* [한국어] 표 항목이 아니면 내려갈 곳이 없다 */
		return -EINVAL;	/* [한국어] 호출자 쪽 버그 */

	ret = (*fn)(pts->range, arg, pts->level - 1, pts->table_lower);	/* [한국어] 같은 범위를 넘긴다 — 아래 단계가 자기 색인 구간을 스스로 구한다 */
	return ret;	/* [한국어] 워커의 결과 */
}

/**
 * pt_walk_range() - Walk over a VA range
 * @range: Range pointer
 * @fn: Walker function to call
 * @arg: Value to pass to the function
 *
 * Walk over a VA range. The caller should have done a validity check, at
 * least calling pt_check_range(), when building range. The walk will
 * start at the top most table.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_walk_range - 최상위부터 범위를 걷는다
 *
 * @range: 걸을 범위(검사가 끝나 있어야 한다).
 * @fn: 워커 함수.
 * @arg: 워커에 넘길 값.
 * @return: 워커의 결과.
 *
 * 순회의 바깥 진입점이다. 이후의 재귀는 워커가 pt_descend 로 스스로 한다.
 */
static __always_inline int pt_walk_range(struct pt_range *range,
					 pt_level_fn_t fn, void *arg)
{
	return fn(range, arg, range->top_level, range->top_table);	/* [한국어] 최상위부터 — 이후 재귀는 워커가 스스로 한다 */
}

/*
 * pt_walk_descend() - Recursively invoke the walker for a slice of a lower
 *                     level
 * @pts: Iteration State
 * @va: Start address
 * @last_va: Last address
 * @fn: Walker function to call
 * @arg: Value to pass to the function
 *
 * With pts pointing at a table item this will descend and over a slice of the
 * lower table. The caller must ensure that va/last_va are within the table
 * item. This creates a new walk and does not alter pts or pts->range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_walk_descend - 아래 표의 일부만 새 순회로 걷는다
 *
 * @pts: 순회 상태(표 항목을 가리켜야 한다).
 * @va: 새 시작 주소.
 * @last_va: 새 마지막 주소.
 * @fn: 워커 함수.
 * @arg: 워커에 넘길 값.
 * @return: 워커의 결과.
 *
 * pt_descend 와 달리 새 범위를 만든다. 진행 중인 순회를 건드리지 않으므로,
 * 큰 페이지를 쪼개거나 하위 표를 따로 처리해야 할 때 쓴다.
 *
 * 호출자가 va/last_va 를 그 표 항목 안으로 제한해야 한다 — 넘어가면 다른
 * 항목이 담당하는 주소를 이 표에서 찾게 된다.
 */
static __always_inline int pt_walk_descend(const struct pt_state *pts,
					   pt_vaddr_t va, pt_vaddr_t last_va,
					   pt_level_fn_t fn, void *arg)
{
	struct pt_range range = pt_make_child_range(pts->range, va, last_va);	/* [한국어] 진행 중인 순회를 건드리지 않는 새 범위 */

	if (PT_WARN_ON(!pt_can_have_table(pts)) ||	/* [한국어] 0단계에는 아래 표가 없고 */
	    PT_WARN_ON(!pts->table_lower))	/* [한국어] 표 항목이 아니면 주소도 없다 */
		return -EINVAL;	/* [한국어] 호출자 쪽 버그 */

	return fn(&range, arg, pts->level - 1, pts->table_lower);	/* [한국어] 새 범위로 아래 단계를 걷는다 */
}

/*
 * pt_walk_descend_all() - Recursively invoke the walker for a table item
 * @parent_pts: Iteration State
 * @fn: Walker function to call
 * @arg: Value to pass to the function
 *
 * With pts pointing at a table item this will descend and over the entire lower
 * table. This creates a new walk and does not alter pts or pts->range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_walk_descend_all - 아래 표 전체를 새 순회로 걷는다
 *
 * @parent_pts: 순회 상태(표 항목을 가리켜야 한다).
 * @fn: 워커 함수.
 * @arg: 워커에 넘길 값.
 * @return: 워커의 결과.
 *
 * 현재 항목이 덮는 VA 구간의 시작과 끝을 구해 pt_walk_descend 에 넘긴다.
 * 표를 통째로 해제하거나 검사할 때 쓴다.
 */
static __always_inline int
pt_walk_descend_all(const struct pt_state *parent_pts, pt_level_fn_t fn,
		    void *arg)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(parent_pts);	/* [한국어] 이 항목이 덮는 크기 */

	return pt_walk_descend(parent_pts,	/* [한국어] 아래 표를 */
			       log2_set_mod(parent_pts->range->va, 0, isz_lg2),	/* [한국어] 이 항목이 덮는 구간의 시작부터 */
			       log2_set_mod_max(parent_pts->range->va, isz_lg2),	/* [한국어] 그 끝까지 — 즉 표 전체 */
			       fn, arg);	/* [한국어] 워커에 넘긴다 */
}

/**
 * pt_range_slice() - Return a range that spans indexes
 * @pts: Iteration State
 * @start_index: Starting index within pts
 * @end_index: Ending index within pts
 *
 * Create a range than spans an index range of the current table level
 * pt_state points at.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_range_slice - 이 표의 색인 구간에 대응하는 VA 범위를 만든다
 *
 * @pts: 순회 상태.
 * @start_index: 시작 색인.
 * @end_index: 끝 색인(배타적).
 * @return: 그 구간이 덮는 VA 범위.
 *
 * 색인에서 VA 로 가는 역방향 변환이다. 끝 주소는 배타적 색인에서 1 을
 * 빼 포함 표현으로 바꾼다.
 *
 * 어디에 쓰는가: 표 하나를 통째로 지우고 그 범위를 무효화해야 할 때,
 * 색인만 알고 있는 상태에서 VA 를 되짚어야 한다.
 */
static inline struct pt_range pt_range_slice(const struct pt_state *pts,
					     unsigned int start_index,
					     unsigned int end_index)
{
	unsigned int table_lg2sz = pt_table_oa_lg2sz(pts);	/* [한국어] 이 표가 덮는 범위 */
	pt_vaddr_t last_va;	/* [한국어] 구간의 끝 */
	pt_vaddr_t va;	/* [한국어] 구간의 시작 */

	va = fvalog2_set_mod(pts->range->va,	/* [한국어] 상위 비트는 그대로 두고 */
			     log2_mul(start_index, pt_table_item_lg2sz(pts)),	/* [한국어] 시작 색인이 가리키는 주소로 */
			     table_lg2sz);	/* [한국어] 이 표가 덮는 비트만 갈아 끼운다 */
	last_va = fvalog2_set_mod(	/* [한국어] 끝도 같은 방식으로 */
		pts->range->va,	/* [한국어] 같은 상위 비트에 */
		log2_mul(end_index, pt_table_item_lg2sz(pts)) - 1, table_lg2sz);	/* [한국어] 배타적 색인에서 1 을 빼 포함 표현으로 */
	return pt_make_child_range(pts->range, va, last_va);	/* [한국어] 부모의 최상위 정보를 물려받는 새 범위 */
}

/**
 * pt_top_memsize_lg2()
 * @common: Table
 * @top_of_table: Top of table value from _pt_top_set()
 *
 * Compute the allocation size of the top table. For PT_FEAT_DYNAMIC_TOP this
 * will compute the top size assuming the table will grow.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_top_memsize_lg2 - 최상위 표에 잡아야 할 메모리 크기
 *
 * @common: 페이지 테이블 인스턴스.
 * @top_of_table: 최상위 워드.
 * @return: 크기의 지수.
 *
 * 최상위 표만 크기가 다를 수 있다. 아래 단계들은 늘 표 하나 크기지만,
 * 최상위는 인스턴스의 주소 공간 폭이 정하는 만큼만 있으면 된다 — 39비트
 * 주소 공간이면 최상위에 항목이 몇 개만 필요하다.
 *
 * DYNAMIC_TOP 이면 그보다도 작다. 지금 단계가 덮는 만큼만 잡고, 주소가
 * 커지면 위에 한 단계를 더 얹는다.
 *
 * 마지막의 max 가 하드웨어 정렬 요구다. 최상위 주소를 담는 필드의 하위
 * 비트가 0 이어야 하므로, 그 정렬보다 작게 잡을 수는 없다.
 */
static inline unsigned int pt_top_memsize_lg2(struct pt_common *common,
					      uintptr_t top_of_table)
{
	struct pt_range range = _pt_top_range(common, top_of_table);	/* [한국어] 최상위 정보 */
	struct pt_state pts = pt_init_top(&range);	/* [한국어] 단계별 크기를 묻기 위한 상태 */
	unsigned int num_items_lg2;	/* [한국어] 최상위에 필요한 항목 수(지수) */

	num_items_lg2 = common->max_vasz_lg2 - pt_table_item_lg2sz(&pts);	/* [한국어] 주소 공간 폭에서 항목 크기를 빼면 필요한 항목 수가 나온다 */
	if (range.top_level != PT_MAX_TOP_LEVEL &&	/* [한국어] 아직 다 자라지 않았고 */
	    pt_feature(common, PT_FEAT_DYNAMIC_TOP))	/* [한국어] 최상위가 자랄 수 있는 형식이면 */
		num_items_lg2 = min(num_items_lg2, pt_num_items_lg2(&pts));	/* [한국어] 지금 단계가 덮는 만큼만 — 커지면 위에 한 단계를 얹는다 */

	/* Round up the allocation size to the minimum alignment */
	return max(ffs_t(u64, PT_TOP_PHYS_MASK),	/* [한국어] (원 주석: 최소 정렬에 맞춰 올림한다) 주소 필드의 하위 비트가 0 이어야 한다 */
		   num_items_lg2 + ilog2(PT_ITEM_WORD_SIZE));	/* [한국어] 항목 수 × 항목 폭 = 바이트 크기 */
}

/**
 * pt_compute_best_pgsize() - Determine the best page size for leaf entries
 * @pgsz_bitmap: Permitted page sizes
 * @va: Starting virtual address for the leaf entry
 * @last_va: Last virtual address for the leaf entry, sets the max page size
 * @oa: Starting output address for the leaf entry
 *
 * Compute the largest page size for va, last_va, and oa together and return it
 * in lg2. The largest page size depends on the format's supported page sizes at
 * this level, and the relative alignment of the VA and OA addresses. 0 means
 * the OA cannot be stored with the provided pgsz_bitmap.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_compute_best_pgsize - 이 자리에 쓸 수 있는 가장 큰 페이지 크기를 고른다
 *
 * @pgsz_bitmap: 이 단계가 허용하는 크기들.
 * @va: 잎이 시작할 가상 주소.
 * @last_va: 매핑할 범위의 마지막 주소.
 * @oa: 대응하는 출력 주소.
 * @return: 크기의 지수, 담을 수 없으면 0.
 *
 * 매핑 성능이 이 함수에 달려 있다. 큰 페이지를 쓸수록 표 항목 수와 TLB
 * 압력이 함께 줄기 때문이다.
 *
 * 세 가지 제약을 한꺼번에 비트 연산으로 푼다.
 *  - VA 와 OA 가 모두 그 크기에 정렬되어야 한다. 둘을 OR 한 값의 하위
 *    0 비트 개수가 곧 공통 정렬이다.
 *  - 페이지가 남은 길이를 넘으면 안 된다. 길이의 최상위 비트 아래를 모두
 *    막아 두면 그 제약이 같은 마스크에 합쳐진다.
 *  - 그 크기가 형식이 허용하는 목록에 있어야 한다. 비트맵에서 한계 아래
 *    비트만 남기고 가장 높은 것을 고른다.
 *
 * 마지막 PT_WARN_ON 들은 결과가 세 제약을 모두 만족하는지 디버그 빌드에서
 * 다시 확인한다.
 */
static inline unsigned int pt_compute_best_pgsize(pt_vaddr_t pgsz_bitmap,
						  pt_vaddr_t va,
						  pt_vaddr_t last_va,
						  pt_oaddr_t oa)
{
	unsigned int best_pgsz_lg2;	/* [한국어] 제약이 허락하는 최대 크기 */
	unsigned int pgsz_lg2;	/* [한국어] 그중 형식이 실제로 지원하는 크기 */
	pt_vaddr_t len = last_va - va + 1;	/* [한국어] 남은 길이 */
	pt_vaddr_t mask;	/* [한국어] 세 제약을 합칠 마스크 */

	if (PT_WARN_ON(va >= last_va))	/* [한국어] 길이가 없는 범위 */
		return 0;	/* [한국어] 매핑할 것이 없다 */

	/*
	 * Given a VA/OA pair the best page size is the largest page size
	 * where:
	 *
	 * 1) VA and OA start at the page. Bitwise this is the count of least
	 *    significant 0 bits.
	 *    This also implies that last_va/oa has the same prefix as va/oa.
	 */
	mask = va | oa;	/* [한국어] (원 주석: VA 와 OA 가 모두 페이지 시작이어야 한다 — 하위 0 비트 개수가 공통 정렬이다) */

	/*
	 * 2) The page size is not larger than the last_va (length). Since page
	 *    sizes are always power of two this can't be larger than the
	 *    largest power of two factor of the length.
	 */
	mask |= log2_to_int(vafls(len) - 1);	/* [한국어] (원 주석: 페이지가 길이를 넘을 수 없다) 길이의 최상위 비트를 마스크에 합친다 */

	best_pgsz_lg2 = vaffs(mask);	/* [한국어] 세 제약을 모두 만족하는 최대 크기 */

	/* Choose the highest bit <= best_pgsz_lg2 */
	if (best_pgsz_lg2 < PT_VADDR_MAX_LG2 - 1)	/* [한국어] (원 주석: 그 이하의 가장 높은 비트를 고른다) */
		pgsz_bitmap = log2_mod(pgsz_bitmap, best_pgsz_lg2 + 1);	/* [한국어] 한계보다 큰 후보를 잘라낸다 */

	pgsz_lg2 = vafls(pgsz_bitmap);	/* [한국어] 남은 것 중 가장 큰 것 */
	if (!pgsz_lg2)	/* [한국어] 쓸 수 있는 크기가 하나도 없으면 */
		return 0;	/* [한국어] 이 자리에 잎을 놓을 수 없다 */

	pgsz_lg2--;	/* [한국어] fls 는 1 기반이라 지수로 바꾼다 */

	PT_WARN_ON(log2_mod(va, pgsz_lg2) != 0);	/* [한국어] VA 정렬 재확인 */
	PT_WARN_ON(oalog2_mod(oa, pgsz_lg2) != 0);	/* [한국어] OA 정렬 재확인 */
	PT_WARN_ON(va + log2_to_int(pgsz_lg2) - 1 > last_va);	/* [한국어] 길이를 넘지 않는가 */
	PT_WARN_ON(!log2_div_eq(va, va + log2_to_int(pgsz_lg2) - 1, pgsz_lg2));	/* [한국어] VA 가 한 페이지 안에 들어가는가 */
	PT_WARN_ON(	/* [한국어] OA 도 */
		!oalog2_div_eq(oa, oa + log2_to_int(pgsz_lg2) - 1, pgsz_lg2));	/* [한국어] 한 페이지 안에 들어가는가 */
	return pgsz_lg2;	/* [한국어] 고른 크기의 지수 */
}

/*
 * Return the number of pgsize_lg2 leaf entries that can be mapped for
 * va to oa. This accounts for any requirement to reduce or increase the page
 * size across the VA range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_pgsz_count - 그 크기로 몇 개를 연달아 매핑할 수 있는가
 *
 * @pgsz_bitmap: 허용 크기들.
 * @va: 시작 가상 주소.
 * @last_va: 마지막 가상 주소.
 * @oa: 시작 출력 주소.
 * @pgsize_lg2: 쓰기로 정한 페이지 크기의 지수.
 * @return: 그 크기의 잎을 몇 개 놓을 수 있는가.
 *
 * 단순히 길이를 크기로 나누지 않는 것이 요점이다. 조금 더 가면 더 큰
 * 페이지를 쓸 수 있게 되는 지점이 있고, 거기서 멈춰야 그 이득을 얻는다.
 *
 * 그 지점을 찾는 방법: 지금보다 큰 다음 크기를 고르고, VA 와 OA 가 그
 * 크기에서 같은 오프셋을 갖는지 본다. 같다면 그 경계에 닿는 순간부터
 * 더 큰 페이지를 쓸 수 있으므로, 거기까지만 이 크기로 채운다.
 */
static inline pt_vaddr_t pt_pgsz_count(pt_vaddr_t pgsz_bitmap, pt_vaddr_t va,
				       pt_vaddr_t last_va, pt_oaddr_t oa,
				       unsigned int pgsize_lg2)
{
	pt_vaddr_t len = last_va - va + 1;	/* [한국어] 남은 길이 */
	pt_vaddr_t next_pgsizes = log2_set_mod(pgsz_bitmap, 0, pgsize_lg2 + 1);	/* [한국어] 지금보다 큰 후보 크기들 */

	if (next_pgsizes) {	/* [한국어] 더 큰 크기가 있으면 */
		unsigned int next_pgsize_lg2 = vaffs(next_pgsizes);	/* [한국어] 그중 가장 작은 것 */

		if (log2_mod(va ^ oa, next_pgsize_lg2) == 0)	/* [한국어] VA 와 OA 가 그 크기에서 같은 오프셋이면 */
			len = min(len, log2_set_mod_max(va, next_pgsize_lg2) -	/* [한국어] 그 경계에 닿는 순간부터 더 큰 페이지를 쓸 수 있으므로 */
					       va + 1);	/* [한국어] 거기까지만 이 크기로 채운다 */
	}
	return log2_div(len, pgsize_lg2);	/* [한국어] 그 길이에 몇 개가 들어가는가 */
}

/*
 * [한국어] 실행 시점의 단계 값을 컴파일 시 상수로 바꿔 주는 분배기.
 *
 * 워커는 단계마다 별도 함수로 펼쳐져 있는데, 진입할 때는 단계가 변수다.
 * 이 사다리가 그 변수를 상수 갈래로 옮긴다 — 각 if 안에서는 단계가
 * 리터럴이므로 그 아래의 모든 계산이 접힌다.
 *
 * `level == N || PT_MAX_TOP_LEVEL == N` 의 뒷항이 코드 크기를 줄인다.
 * 그 형식의 최대 단계가 N 이면 더 위 갈래는 도달할 수 없으므로,
 * 컴파일러가 나머지를 통째로 지운다.
 */
#define _PT_MAKE_CALL_LEVEL(fn)                                          \
	static __always_inline int fn(struct pt_range *range, void *arg, \
				      unsigned int level,                \
				      struct pt_table_p *table)          \
	{                                                                \
		static_assert(PT_MAX_TOP_LEVEL <= 5);                    \
		if (level == 0)                                          \
			return CONCATENATE(fn, 0)(range, arg, 0, table); \
		if (level == 1 || PT_MAX_TOP_LEVEL == 1)                 \
			return CONCATENATE(fn, 1)(range, arg, 1, table); \
		if (level == 2 || PT_MAX_TOP_LEVEL == 2)                 \
			return CONCATENATE(fn, 2)(range, arg, 2, table); \
		if (level == 3 || PT_MAX_TOP_LEVEL == 3)                 \
			return CONCATENATE(fn, 3)(range, arg, 3, table); \
		if (level == 4 || PT_MAX_TOP_LEVEL == 4)                 \
			return CONCATENATE(fn, 4)(range, arg, 4, table); \
		return CONCATENATE(fn, 5)(range, arg, 5, table);         \
	}

/*
 * [한국어]
 * __pt_make_level_fn_err - 0단계 아래로 내려가려 할 때의 막다른 함수
 *
 * @range: 무시된다.
 * @arg: 무시된다.
 * @unused_level: 무시된다.
 * @table: 무시된다.
 * @return: 늘 -EPROTOTYPE.
 *
 * 0단계 함수의 "아래 단계" 자리에 이 함수가 들어간다. 정상 동작에서는
 * 불리지 않지만, 재귀 사슬을 닫으려면 이름이 있어야 한다.
 */
static inline int __pt_make_level_fn_err(struct pt_range *range, void *arg,
					 unsigned int unused_level,
					 struct pt_table_p *table)
{
	static_assert(PT_MAX_TOP_LEVEL <= 5);	/* [한국어] 펼쳐 둔 단계 수를 넘는 형식은 없다 */
	return -EPROTOTYPE;	/* [한국어] 도달하면 재귀 사슬이 잘못 이어진 것이다 */
}

/*
 * [한국어] 한 단계짜리 워커 함수를 만들어 내는 틀.
 *
 * 단계 번호를 리터럴로 박고, 아래 단계 함수의 이름을 함께 넘긴다.
 * 그래서 do_fn 안에서는 단계가 상수이고 재귀 대상도 정해져 있다.
 */
#define __PT_MAKE_LEVEL_FN(fn, level, descend_fn, do_fn)            \
	static inline int fn(struct pt_range *range, void *arg,     \
			     unsigned int unused_level,             \
			     struct pt_table_p *table)              \
	{                                                           \
		return do_fn(range, arg, level, table, descend_fn); \
	}

/**
 * PT_MAKE_LEVELS() - Build an unwound walker
 * @fn: Name of the walker function
 * @do_fn: Function to call at each level
 *
 * This builds a function call tree that can be fully inlined.
 * The caller must provide a function body in an __always_inline function::
 *
 *  static __always_inline int do_fn(struct pt_range *range, void *arg,
 *         unsigned int level, struct pt_table_p *table,
 *         pt_level_fn_t descend_fn)
 *
 * An inline function will be created for each table level that calls do_fn with
 * a compile time constant for level and a pointer to the next lower function.
 * This generates an optimally inlined walk where each of the functions sees a
 * constant level and can codegen the exact constants/etc for that level.
 *
 * Note this can produce a lot of code!
 */
/*
 * [한국어] (위 kdoc 과 함께 읽을 것)
 * 단계 0 부터 5 까지의 워커 함수와 그 분배기를 한꺼번에 만든다.
 *
 * 0단계의 아래는 오류 함수로 닫고, 그 위로는 각 단계가 바로 아래 단계를
 * 가리키게 이어 붙인다. 마지막에 _PT_MAKE_CALL_LEVEL 이 진입점을 만든다.
 *
 * 이 방식이 함수 포인터 재귀를 대신한다 — 모든 호출이 컴파일 시 정해져
 * 있어 전부 인라인될 수 있다.
 */
#define PT_MAKE_LEVELS(fn, do_fn)                                             \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 0), 0, __pt_make_level_fn_err,     \
			   do_fn);                                            \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 1), 1, CONCATENATE(fn, 0), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 2), 2, CONCATENATE(fn, 1), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 3), 3, CONCATENATE(fn, 2), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 4), 4, CONCATENATE(fn, 3), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 5), 5, CONCATENATE(fn, 4), do_fn); \
	_PT_MAKE_CALL_LEVEL(fn)	/* [한국어] 마지막으로 실행 시점 단계를 상수 갈래로 옮기는 진입점을 만든다 */

#endif	/* [한국어] 포함 방지 끝 */
