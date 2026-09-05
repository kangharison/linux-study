/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * Default definitions for formats that don't define these functions.
 */
/*
 * [한국어 설명] 형식이 구현하지 않은 API 의 기본 구현 (pt_fmt_defaults.h)
 *
 * === 파일의 역할 ===
 * pt_common.h 가 선언한 형식 API 는 서른 개가 넘는데, 형식마다 그 전부를
 * 쓰지는 않는다. 연속 항목을 지원하지 않는 형식, 더티 추적이 없는 형식,
 * 소프트웨어 비트가 없는 형식이 있다.
 *
 * 이 파일은 그런 경우의 기본 답을 준다. 방식은 `#ifndef pt_XXX` 다 —
 * 형식이 자기 정의를 두었으면 그 매크로가 이미 있어 이 블록이 통째로
 * 건너뛰어지고, 두지 않았으면 여기 것이 쓰인다.
 *
 * 단순한 대체만 있는 것은 아니다. 몇 개는 형식이 제공한 다른 함수로부터
 * 없는 쪽을 만들어 낸다: pt_entry_oa 와 pt_item_oa 는 둘 중 하나만 있으면
 * 나머지를 계산해 주고, pt_possible_sizes 는 pt_contig_count_lg2 로부터
 * 비트맵을 조립한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pt_common.h 가 이 파일을 포함하므로, 순서상 형식 헤더 다음이다. 그래야
 * 형식이 무엇을 정의했는지 보고 빈 곳만 채울 수 있다.
 *
 * 실행 컨텍스트: 전부 인라인. 대부분은 상수를 돌려주어 호출부에서 사라진다.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_common.h 가 포함하고, 그 위의 모든 코드가 결과를 쓴다.
 * 아래: pt_defs.h 의 어휘와, 형식이 정의한 PT_GRANULE_LG2SZ,
 *       PT_TABLEMEM_LG2SZ, PT_ITEM_WORD_SIZE 같은 상수들.
 *
 * === 주요 함수/구조체 요약 ===
 * pt_table_item_lg2sz / pt_pgsz_lg2_to_level: 단계와 크기 사이의 변환.
 *   모든 단계가 같은 모양이라는 가정에서 산술로 답한다.
 * pt_item_oa / pt_entry_oa: 둘 중 없는 쪽을 있는 쪽에서 만든다.
 * pt_possible_sizes: 단일 항목 크기와 연속 항목 크기 두 비트만 세운 비트맵.
 * pt_clear_entries32/64: 항목 폭에 맞춰 0 으로 덮는다.
 * pt_set_sw_bit_release / pt_test_sw_bit_acquire: 소프트웨어 비트의 실제
 *   구현. 형식에 그 비트가 없으면 링크 오류를 내는 가짜 함수로 대체된다.
 * pt_check_install_leaf_args: 잎을 설치하기 전 정렬과 크기를 검사한다.
 */
#ifndef __GENERIC_PT_PT_FMT_DEFAULTS_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_PT_FMT_DEFAULTS_H	/* [한국어] 같은 이름으로 표시 */

#include "pt_defs.h"	/* [한국어] 순회 상태와 산술 */
#include <linux/log2.h>	/* [한국어] ilog2 — 표 항목 수 계산 */

/* Header self-compile default defines */
#ifndef pt_load_entry_raw	/* [한국어] (원 주석: 헤더 단독 컴파일용 기본값) */
#include "fmt/amdv1.h"	/* [한국어] 형식 없이 이 헤더만 컴파일할 때 아무 형식이나 끌어와 문법을 검사한다 */
#endif

/*
 * The format must provide PT_GRANULE_LG2SZ, PT_TABLEMEM_LG2SZ, and
 * PT_ITEM_WORD_SIZE. They must be the same at every level excluding the top.
 */
#ifndef pt_table_item_lg2sz	/* [한국어] 형식이 자기 구현을 두지 않았으면 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_table_item_lg2sz - 단계에서 항목 크기를 산술로 구한다
 *
 * @pts: 현재 단계.
 * @return: 항목 하나가 덮는 크기의 지수.
 *
 * 최소 페이지 크기에서 시작해, 단계마다 "표 하나가 소비하는 비트 수"만큼
 * 지수를 더한다. 그 비트 수는 표 크기에서 항목 크기를 나눈 값이다.
 *
 * 이 계산이 성립하려면 최상위를 뺀 모든 단계의 모양이 같아야 한다 — 원
 * 주석이 그 전제를 명시한다. 그렇지 않은 형식은 자기 구현을 둔다.
 */
static inline unsigned int pt_table_item_lg2sz(const struct pt_state *pts)
{
	return PT_GRANULE_LG2SZ +	/* [한국어] 최소 페이지 크기에서 시작해 */
	       (PT_TABLEMEM_LG2SZ - ilog2(PT_ITEM_WORD_SIZE)) * pts->level;	/* [한국어] 단계마다 표 하나가 소비하는 비트 수만큼 더한다 */
}
#endif

#ifndef pt_pgsz_lg2_to_level	/* [한국어] 형식이 자기 구현을 두지 않았으면 */
/*
 * [한국어]
 * pt_pgsz_lg2_to_level - 페이지 크기에서 담당 단계를 산술로 구한다
 *
 * @common: 페이지 테이블 인스턴스.
 * @pgsize_lg2: 페이지 크기의 지수.
 * @return: 그 크기를 매핑하는 단계.
 *
 * 위 함수의 역이다. 최소 페이지보다 얼마나 큰지를 단계당 비트 수로 나눈다.
 */
static inline unsigned int pt_pgsz_lg2_to_level(struct pt_common *common,
						unsigned int pgsize_lg2)
{
	return ((unsigned int)(pgsize_lg2 - PT_GRANULE_LG2SZ)) /	/* [한국어] 최소 페이지보다 얼마나 큰지를 */
	       (PT_TABLEMEM_LG2SZ - ilog2(PT_ITEM_WORD_SIZE));	/* [한국어] 단계당 비트 수로 나눈다 */
}
#endif

/*
 * If not supplied by the format then contiguous pages are not supported.
 *
 * If contiguous pages are supported then the format must also provide
 * pt_contig_count_lg2() if it supports a single contiguous size per level,
 * or pt_possible_sizes() if it supports multiple sizes per level.
 */
#ifndef pt_entry_num_contig_lg2	/* [한국어] (원 주석: 형식이 주지 않으면 연속 페이지를 지원하지 않는다) */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_entry_num_contig_lg2 - 연속 항목을 지원하지 않을 때의 답
 *
 * @pts: 볼 항목.
 * @return: 늘 0(항목 하나가 곧 entry 하나).
 *
 * 이 형식에서는 item 과 entry 가 같은 것이 된다. 상수라 이 값을 쓰는
 * 계산이 모두 접혀 사라진다.
 */
static inline unsigned int pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	return ilog2(1);	/* [한국어] 늘 0 — item 과 entry 가 같다 */
}

/*
 * Return the number of contiguous OA items forming an entry at this table level
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_contig_count_lg2 - 연속 묶음의 크기(지원하지 않을 때)
 *
 * @pts: 현재 단계.
 * @return: 늘 0.
 *
 * pt_possible_sizes 의 기본 구현이 이 값을 쓰므로, 연속을 지원하지 않아도
 * 이름은 존재해야 한다.
 */
static inline unsigned short pt_contig_count_lg2(const struct pt_state *pts)
{
	return ilog2(1);	/* [한국어] 묶을 항목이 없다 */
}
#endif

/* If not supplied by the format then dirty tracking is not supported */
#ifndef pt_entry_is_write_dirty	/* [한국어] (원 주석: 형식이 주지 않으면 더티 추적을 지원하지 않는다) */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_entry_is_write_dirty - 더티 추적이 없을 때의 답
 *
 * @pts: 볼 항목.
 * @return: 늘 거짓.
 */
static inline bool pt_entry_is_write_dirty(const struct pt_state *pts)
{
	return false;	/* [한국어] 더티 비트가 없는 형식 */
}

/*
 * [한국어]
 * pt_entry_make_write_clean - 더티 추적이 없을 때는 할 일이 없다
 *
 * @pts: 무시된다.
 *
 * 호출부를 #ifdef 로 감싸지 않기 위해 빈 함수를 둔다.
 */
static inline void pt_entry_make_write_clean(struct pt_state *pts)
{
}

/*
 * [한국어]
 * pt_dirty_supported - 더티 비트가 없는 형식의 답
 *
 * @common: 무시된다.
 * @return: 늘 거짓.
 *
 * 드라이버가 이 값을 보고 iommufd 에 능력을 보고하지 않는다.
 */
static inline bool pt_dirty_supported(struct pt_common *common)
{
	return false;	/* [한국어] 드라이버가 이 능력을 보고하지 않는다 */
}
#else
/* If not supplied then dirty tracking is always enabled */
#ifndef pt_dirty_supported	/* [한국어] (원 주석: 주지 않으면 더티 추적은 늘 켜진 것으로 본다) */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_dirty_supported - 더티 비트가 있는데 별도 조건이 없을 때의 답
 *
 * @common: 무시된다.
 * @return: 늘 참.
 *
 * 형식이 더티 비트를 구현했다면 기본적으로 늘 쓸 수 있다고 본다.
 * 하드웨어 기능 비트를 따져야 하는 형식은 자기 구현을 둔다.
 */
static inline bool pt_dirty_supported(struct pt_common *common)
{
	return true;	/* [한국어] 형식이 더티 비트를 구현했다면 기본적으로 늘 쓸 수 있다고 본다 */
}
#endif
#endif

#ifndef pt_entry_make_write_dirty	/* [한국어] 소프트웨어가 더티를 찍는 경로가 없으면 */
/*
 * [한국어]
 * pt_entry_make_write_dirty - 소프트웨어가 더티를 찍을 수 없을 때의 답
 *
 * @pts: 무시된다.
 * @return: 늘 거짓(바꾸지 않았다).
 */
static inline bool pt_entry_make_write_dirty(struct pt_state *pts)
{
	return false;	/* [한국어] 소프트웨어가 찍을 자리가 없다 */
}
#endif

/*
 * Format supplies either:
 *   pt_entry_oa - OA is at the start of a contiguous entry
 * or
 *   pt_item_oa  - OA is adjusted for every item in a contiguous entry
 *
 * Build the missing one
 *
 * The internal helper _pt_entry_oa_fast() allows generating
 * an efficient pt_entry_oa_exact(), it doesn't care which
 * option is selected.
 */
#ifdef pt_entry_oa	/* [한국어] 형식이 entry 시작 주소 쪽을 구현했으면 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_item_oa - entry 시작 주소에서 item 주소를 만든다
 *
 * @pts: 볼 item.
 * @return: 그 item 의 출력 주소.
 *
 * 형식이 pt_entry_oa 쪽을 구현한 경우다. 표에는 묶음의 시작 주소만 적혀
 * 있으므로, item 의 주소는 시작에 색인×항목크기를 더해 만든다.
 *
 * 색인 전체를 곱하는 것이 맞는 이유: entry 시작이 그 크기에 정렬되어
 * 있어 하위 비트가 0 이라, OR 로 얹어도 겹치지 않는다.
 */
static inline pt_oaddr_t pt_item_oa(const struct pt_state *pts)
{
	return pt_entry_oa(pts) |	/* [한국어] 묶음의 시작 주소에 */
	       log2_mul(pts->index, pt_table_item_lg2sz(pts));	/* [한국어] 색인×항목크기를 얹는다 — 시작이 정렬되어 있어 겹치지 않는다 */
}
#define _pt_entry_oa_fast pt_entry_oa	/* [한국어] 오프셋 계산에는 시작 주소가 필요하다 */
#endif

#ifdef pt_item_oa	/* [한국어] 형식이 item 주소 쪽을 구현했으면 */
/*
 * [한국어]
 * pt_entry_oa - item 주소에서 entry 시작 주소를 만든다
 *
 * @pts: 볼 항목.
 * @return: 그 entry 시작의 출력 주소.
 *
 * 형식이 pt_item_oa 쪽을 구현한 경우다. 표에 item 마다 자기 주소가 적혀
 * 있으므로, 하위 비트를 0 으로 깎으면 묶음의 시작이 된다.
 */
static inline pt_oaddr_t pt_entry_oa(const struct pt_state *pts)
{
	return log2_set_mod(pt_item_oa(pts), 0,	/* [한국어] item 주소의 하위 비트를 0 으로 깎으면 */
			    pt_entry_num_contig_lg2(pts) +	/* [한국어] 묶음 개수와 */
				    pt_table_item_lg2sz(pts));	/* [한국어] 항목 크기를 합한 만큼 — 그것이 묶음의 시작이다 */
}
#define _pt_entry_oa_fast pt_item_oa	/* [한국어] 이쪽이 더 싸다 — 어차피 오프셋을 다시 얹는다 */
#endif

/*
 * If not supplied by the format then use the constant
 * PT_MAX_OUTPUT_ADDRESS_LG2.
 */
#ifndef pt_max_oa_lg2	/* [한국어] 형식이 함수를 두지 않았으면 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_max_oa_lg2 - 형식 상수를 그대로 돌려준다
 *
 * @common: 무시된다.
 * @return: 형식이 정의한 최대 출력 주소 폭.
 *
 * 인스턴스마다 달라지지 않는 형식이면 이 기본으로 족하다.
 */
static inline unsigned int
pt_max_oa_lg2(const struct pt_common *common)
{
	return PT_MAX_OUTPUT_ADDRESS_LG2;	/* [한국어] 인스턴스마다 달라지지 않는 형식이면 이 상수로 족하다 */
}
#endif

#ifndef pt_has_system_page_size	/* [한국어] 형식이 함수를 두지 않았으면 */
/*
 * [한국어]
 * pt_has_system_page_size - 최소 페이지가 커널 페이지와 같은가
 *
 * @common: 무시된다.
 * @return: 같으면 참.
 *
 * 같으면 PAGE_SIZE 단위 매핑을 0단계에 바로 놓을 수 있어, 흔한 경우의
 * 빠른 경로가 성립한다.
 */
static inline bool pt_has_system_page_size(const struct pt_common *common)
{
	return PT_GRANULE_LG2SZ == PAGE_SHIFT;	/* [한국어] 같으면 PAGE_SIZE 매핑의 빠른 경로가 선다 */
}
#endif

/*
 * If not supplied by the format then assume only one contiguous size determined
 * by pt_contig_count_lg2()
 */
#ifndef pt_possible_sizes	/* [한국어] (원 주석: 형식이 주지 않으면 연속 크기가 하나뿐이라고 본다) */
/*
 * [한국어] 아래 기본 구현이 쓰는 이름의 전방 선언.
 * 형식이 제공하거나, 이 파일 위쪽의 기본이 제공한다.
 */
static inline unsigned short pt_contig_count_lg2(const struct pt_state *pts);

/* Return a bitmap of possible leaf page sizes at this level */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_possible_sizes - 단일 항목과 연속 묶음, 두 크기만 있는 비트맵
 *
 * @pts: 현재 단계.
 * @return: 가능한 크기들의 비트맵.
 *
 * 한 단계에 연속 크기가 하나뿐인 형식을 위한 기본이다. 여러 크기를
 * 지원하는 형식은 자기 구현을 둔다.
 *
 * 잎을 놓을 수 없는 단계면 0 을 돌려준다 — 그 단계에서는 어떤 크기도
 * 만들 수 없다는 뜻이다.
 */
static inline pt_vaddr_t pt_possible_sizes(const struct pt_state *pts)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return 0;	/* [한국어] 만들 수 있는 크기가 없다 */
	return log2_to_int(isz_lg2) |	/* [한국어] 단일 항목 크기와 */
	       log2_to_int(pt_contig_count_lg2(pts) + isz_lg2);	/* [한국어] 연속 묶음 크기, 두 비트만 세운다 */
}
#endif

/* If not supplied by the format then use 0. */
#ifndef pt_full_va_prefix	/* [한국어] (원 주석: 형식이 주지 않으면 0 을 쓴다) */
/*
 * [한국어]
 * pt_full_va_prefix - 부호 확장이 없는 형식의 답
 *
 * @common: 무시된다.
 * @return: 늘 0.
 *
 * 주소 공간이 0 부터 위로만 뻗는 형식이면 붙일 접두가 없다.
 */
static inline pt_vaddr_t pt_full_va_prefix(const struct pt_common *common)
{
	return 0;	/* [한국어] 아래에서 위로만 뻗는 주소 공간 */
}
#endif

/* If not supplied by the format then zero fill using PT_ITEM_WORD_SIZE */
#ifndef pt_clear_entries	/* [한국어] (원 주석: 형식이 주지 않으면 항목 폭만큼 0 으로 채운다) */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_clear_entries64 - 64비트 항목들을 0 으로 덮는다
 *
 * @pts: 시작 위치.
 * @num_contig_lg2: 지울 개수의 지수.
 *
 * 0 항목은 다시 읽으면 PT_ENTRY_EMPTY 가 되므로, 대부분의 형식은 이
 * 단순한 구현으로 족하다. 유효 비트를 따로 두고 다른 필드를 보존해야 하는
 * 형식만 자기 구현을 둔다.
 *
 * 시작 위치가 개수에 정렬되어 있어야 한다 — 연속 묶음은 정렬된 자리에만
 * 놓이기 때문이며, 어긋나면 남의 묶음 절반을 지우게 된다.
 *
 * WRITE_ONCE 인 이유: 하드웨어가 동시에 이 표를 걷고 있어, 컴파일러가
 * 쓰기를 쪼개거나 합치면 반쯤 지워진 항목을 볼 수 있다.
 */
static inline void pt_clear_entries64(struct pt_state *pts,
				      unsigned int num_contig_lg2)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 지울 첫 항목 */
	u64 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 그 다음 자리 */

	PT_WARN_ON(log2_mod(pts->index, num_contig_lg2));	/* [한국어] 정렬이 어긋나면 남의 묶음 절반을 지운다 */
	for (; tablep != end; tablep++)	/* [한국어] 묶음의 모든 항목을 */
		WRITE_ONCE(*tablep, 0);	/* [한국어] 하드웨어가 동시에 걷고 있어 쪼개 쓰면 안 된다 */
}

/*
 * [한국어]
 * pt_clear_entries32 - 32비트 항목들을 0 으로 덮는다
 *
 * @pts: 시작 위치.
 * @num_contig_lg2: 지울 개수의 지수.
 *
 * 64비트판과 논리가 같다. 항목이 32비트인 형식이 쓴다.
 */
static inline void pt_clear_entries32(struct pt_state *pts,
				      unsigned int num_contig_lg2)
{
	u32 *tablep = pt_cur_table(pts, u32) + pts->index;	/* [한국어] 지울 첫 항목 */
	u32 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 그 다음 자리 */

	PT_WARN_ON(log2_mod(pts->index, num_contig_lg2));	/* [한국어] 정렬 검사 */
	for (; tablep != end; tablep++)	/* [한국어] 묶음의 모든 항목을 */
		WRITE_ONCE(*tablep, 0);	/* [한국어] 한 번에 하나씩 통째로 */
}

/*
 * [한국어]
 * pt_clear_entries - 항목 폭에 맞는 구현으로 갈라 준다
 *
 * @pts: 시작 위치.
 * @num_contig_lg2: 지울 개수의 지수.
 *
 * PT_ITEM_WORD_SIZE 는 컴파일 시 상수라 한쪽만 남는다.
 */
static inline void pt_clear_entries(struct pt_state *pts,
				    unsigned int num_contig_lg2)
{
	if (PT_ITEM_WORD_SIZE == sizeof(u32))	/* [한국어] 컴파일 시 상수라 한쪽만 남는다 */
		pt_clear_entries32(pts, num_contig_lg2);	/* [한국어] 32비트 항목 */
	else
		pt_clear_entries64(pts, num_contig_lg2);	/* [한국어] 64비트 항목 */
}
#define pt_clear_entries pt_clear_entries	/* [한국어] pt_common.h 의 선언이 이 구현으로 이어지게 한다 */
#endif

/* If not supplied then SW bits are not supported */
#ifdef pt_sw_bit	/* [한국어] (원 주석: 형식이 주지 않으면 소프트웨어 비트를 지원하지 않는다) */
/*
 * [한국어]
 * pt_test_sw_bit_acquire - 소프트웨어 비트를 읽는다(형식이 그 비트를 줄 때)
 *
 * @pts: 볼 항목(값이 이미 담겨 있어야 한다).
 * @bitnr: 볼 비트 번호.
 * @return: 서 있으면 참.
 *
 * 이미 읽어 둔 pts->entry 를 보므로 메모리 접근이 없다. 대신 앞에
 * 전체 장벽을 둔다 — 이 비트를 본 뒤에 읽는 것들이 그 비트를 세우기 전의
 * 쓰기보다 앞서 실행되면 안 되기 때문이다.
 *
 * 연속 항목에서는 첫 item 에만 이 비트를 둔다(원 주석). 그래야 묶음
 * 전체에 대해 답이 하나로 정해진다.
 */
static inline bool pt_test_sw_bit_acquire(struct pt_state *pts,
					  unsigned int bitnr)
{
	/* Acquire, pairs with pt_set_sw_bit_release() */
	smp_mb();	/* [한국어] (원 주석: 획득 — pt_set_sw_bit_release 와 짝을 이룬다) */
	/* For a contiguous entry the sw bit is only stored in the first item. */
	return pts->entry & pt_sw_bit(bitnr);	/* [한국어] (원 주석: 연속 항목에서는 첫 item 에만 이 비트가 저장된다) */
}
#define pt_test_sw_bit_acquire pt_test_sw_bit_acquire	/* [한국어] 이름을 이어 준다 */

/*
 * [한국어]
 * pt_set_sw_bit_release - 소프트웨어 비트를 원자적으로 세운다
 *
 * @pts: 고칠 항목.
 * @bitnr: 세울 비트.
 *
 * 다른 비트를 건드리지 않고 이 비트만 세워야 하므로 읽고-고치고-쓰기를
 * cmpxchg 반복으로 한다. 하드웨어가 같은 항목의 더티 비트를 동시에
 * 갱신할 수 있어, 통째로 덮어쓰면 그 갱신을 잃는다.
 *
 * release 순서인 이유: 이 비트가 "준비 완료" 깃발로 쓰이므로, 그 전에
 * 이루어진 준비 작업이 먼저 보여야 한다.
 *
 * 항목 폭이 둘 중 어느 쪽도 아니면 BUILD_BUG 로 빌드를 세운다 — 조용히
 * 아무것도 하지 않는 것보다 낫다.
 */
static inline void pt_set_sw_bit_release(struct pt_state *pts,
					 unsigned int bitnr)
{
#if !IS_ENABLED(CONFIG_GENERIC_ATOMIC64)	/* [한국어] 64비트 원자 연산을 하드웨어가 지원할 때만 */
	if (PT_ITEM_WORD_SIZE == sizeof(u64)) {	/* [한국어] 64비트 항목이고 원자 연산이 있으면 */
		u64 *entryp = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 고칠 항목의 주소 */
		u64 old_entry = pts->entry;	/* [한국어] 읽어 둔 값에서 시작 */
		u64 new_entry;	/* [한국어] 비트를 얹은 값 */

		do {
			new_entry = old_entry | pt_sw_bit(bitnr);	/* [한국어] 다른 비트는 그대로 두고 */
		} while (!try_cmpxchg64_release(entryp, &old_entry, new_entry));	/* [한국어] 하드웨어가 더티를 갱신했으면 다시 시도한다 */
		pts->entry = new_entry;	/* [한국어] 순회 상태도 맞춘다 */
		return;	/* [한국어] 끝 */
	}
#endif
	if (PT_ITEM_WORD_SIZE == sizeof(u32)) {	/* [한국어] 32비트 항목이면 */
		u32 *entryp = pt_cur_table(pts, u32) + pts->index;	/* [한국어] 고칠 항목의 주소 */
		u32 old_entry = pts->entry;	/* [한국어] 읽어 둔 값 */
		u32 new_entry;	/* [한국어] 비트를 얹은 값 */

		do {
			new_entry = old_entry | pt_sw_bit(bitnr);	/* [한국어] 그 비트만 세우고 */
		} while (!try_cmpxchg_release(entryp, &old_entry, new_entry));	/* [한국어] 충돌하면 다시 */
		pts->entry = new_entry;	/* [한국어] 순회 상태를 맞춘다 */
	} else
		BUILD_BUG();	/* [한국어] 그 밖의 폭은 있을 수 없다 — 조용히 지나치지 않고 빌드를 세운다 */
}
#define pt_set_sw_bit_release pt_set_sw_bit_release	/* [한국어] 이름을 이어 준다 */
#else
/*
 * [한국어]
 * pt_max_sw_bit - 소프트웨어 비트가 없는 형식의 답
 *
 * @common: 무시된다.
 * @return: 늘 0.
 */
static inline unsigned int pt_max_sw_bit(struct pt_common *common)
{
	return 0;	/* [한국어] 쓸 수 있는 비트가 없다 */
}

/*
 * [한국어] 정의가 없는 함수를 일부러 선언해 둔다.
 * 아래 가짜 구현이 이것을 부르므로, 소프트웨어 비트가 없는 형식에서 그
 * 경로가 실제로 쓰이면 링크가 실패한다. 런타임에 조용히 잘못 동작하는
 * 대신 빌드에서 걸리게 하는 장치다.
 */
extern void __pt_no_sw_bit(void);
/*
 * [한국어]
 * pt_test_sw_bit_acquire - 소프트웨어 비트가 없을 때의 가짜 구현
 *
 * @pts: 무시된다.
 * @bitnr: 무시된다.
 * @return: 늘 거짓(도달하면 링크가 실패한다).
 */
static inline bool pt_test_sw_bit_acquire(struct pt_state *pts,
					  unsigned int bitnr)
{
	__pt_no_sw_bit();	/* [한국어] 정의가 없는 함수라 이 경로가 쓰이면 링크가 실패한다 */
	return false;	/* [한국어] 도달하지 않는다 */
}

/*
 * [한국어]
 * pt_set_sw_bit_release - 소프트웨어 비트가 없을 때의 가짜 구현
 *
 * @pts: 무시된다.
 * @bitnr: 무시된다.
 */
static inline void pt_set_sw_bit_release(struct pt_state *pts,
					 unsigned int bitnr)
{
	__pt_no_sw_bit();	/* [한국어] 같은 이유로 링크에서 걸린다 */
}
#endif

/*
 * Format can call in the pt_install_leaf_entry() to check the arguments are all
 * aligned correctly.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_check_install_leaf_args - 잎을 설치하기 전 인자를 검사한다
 *
 * @pts: 설치할 위치.
 * @oa: 넣을 출력 주소.
 * @oasz_lg2: 그 잎이 덮는 크기의 지수.
 * @return: 모두 맞으면 참.
 *
 * 세 가지를 본다.
 *  - 출력 주소가 그 크기에 정렬되어 있는가. 어긋나면 하위 비트가 다른
 *    필드와 겹쳐 항목이 깨진다.
 *  - 크기가 이 단계가 만들 수 있는 범위 안인가. 여러 연속 크기를 지원하는
 *    형식인지에 따라 검사 모양이 달라진다.
 *  - 표에서의 위치가 그 크기에 정렬되어 있는가. 연속 묶음은 정렬된
 *    자리에만 놓인다.
 *
 * 형식 구현이 스스로 부르는 검사다. 디버그 빌드에서만 실제로 경고하고,
 * 운영 커널에서는 PT_WARN_ON 이 접혀 사라진다.
 */
static inline bool pt_check_install_leaf_args(struct pt_state *pts,
					      pt_oaddr_t oa,
					      unsigned int oasz_lg2)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */

	if (PT_WARN_ON(oalog2_mod(oa, oasz_lg2)))	/* [한국어] 출력 주소가 그 크기에 정렬되어 있는가 */
		return false;	/* [한국어] 어긋나면 하위 비트가 다른 필드와 겹친다 */

#ifdef pt_possible_sizes	/* [한국어] 여러 연속 크기를 지원하는 형식인지에 따라 검사 모양이 갈린다 */
	if (PT_WARN_ON(isz_lg2 > oasz_lg2 ||	/* [한국어] 여러 연속 크기를 지원하는 형식: 항목 크기 이상이고 */
		       oasz_lg2 > isz_lg2 + pt_num_items_lg2(pts)))	/* [한국어] 표 하나를 넘지 않아야 한다 */
		return false;	/* [한국어] 범위 밖 */
#else
	if (PT_WARN_ON(oasz_lg2 != isz_lg2 &&	/* [한국어] 연속 크기가 하나뿐인 형식: 단일 항목이거나 */
		       oasz_lg2 != isz_lg2 + pt_contig_count_lg2(pts)))	/* [한국어] 그 하나의 묶음 크기여야 한다 */
		return false;	/* [한국어] 둘 다 아니다 */
#endif

	if (PT_WARN_ON(oalog2_mod(pts->index, oasz_lg2 - isz_lg2)))	/* [한국어] 표에서의 위치도 묶음 크기에 정렬되어야 한다 */
		return false;	/* [한국어] 어긋나면 남의 묶음과 겹친다 */
	return true;	/* [한국어] 모두 통과 */
}

#endif
