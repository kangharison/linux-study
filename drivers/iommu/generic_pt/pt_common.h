/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * This header is included after the format. It contains definitions
 * that build on the format definitions to create the basic format API.
 *
 * The format API is listed here, with kdocs. The functions without bodies are
 * implemented in the format using the pattern:
 *     static inline FMTpt_XXX(..) {..}
 *     #define pt_XXX FMTpt_XXX
 *
 * If the format doesn't implement a function then pt_fmt_defaults.h can provide
 * a generic version.
 *
 * The routines marked "@pts: Entry to query" operate on the entire contiguous
 * entry and can be called with a pts->index pointing to any sub item that makes
 * up that entry.
 *
 * The header order is:
 *  pt_defs.h
 *  FMT.h
 *  pt_common.h
 */
/*
 * [한국어 설명] 형식이 채워야 할 API 의 선언과 공통 구현 (pt_common.h)
 *
 * === 파일의 역할 ===
 * generic_pt 의 계약서다. "형식이 반드시 제공해야 하는 함수"의 목록과
 * 그 의미를 여기서 정하고, 형식과 무관하게 답이 정해지는 몇 개는 여기서
 * 직접 구현한다.
 *
 * 몸통 없는 선언이 대부분인 것이 눈에 띈다. 원 주석이 그 방식을 밝힌다 —
 * 형식은 `FMTpt_XXX()` 를 정의하고 `#define pt_XXX FMTpt_XXX` 로 이름을
 * 잇는다. 그래서 공통 코드는 `pt_XXX` 만 부르면 되고, 실제로는 형식별
 * 인라인 함수가 그 자리에 박힌다. 함수 포인터가 없으므로 호출 비용도 없다.
 *
 * 형식이 굳이 구현하지 않아도 되는 것들은 pt_fmt_defaults.h 가 기본 구현을
 * 준다. 형식이 자기 정의를 두면 그쪽이 이기고, 두지 않으면 기본이 쓰인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pt_defs.h(어휘) → 형식 헤더(접근자 정의) → [이 파일](계약과 공통 구현)
 *   → pt_iter.h(순회) → iommu_pt.h(IOMMU 진입점) → 드라이버
 *
 * 실행 컨텍스트: 전부 인라인. 컴파일 시 형식이 고정되므로 분기가 접힌다.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_iter.h, iommu_pt.h, kunit 헤더들이 이 API 만 보고 동작한다.
 * 아래: pt_defs.h 의 자료형과 형식 헤더의 실제 구현.
 *
 * 데이터 흐름: 순회 코드가 struct pt_state 를 들고 이 API 를 부르면,
 * 형식 구현이 그 상태가 가리키는 항목을 읽거나 고친다.
 *
 * === 주요 함수/구조체 요약 ===
 * pt_load_entry / pt_load_entry_raw: 항목 하나를 읽고 종류를 판정한다.
 *   순회의 모든 걸음이 여기서 시작한다.
 * pt_install_leaf_entry / pt_install_table: 잎 항목과 표 포인터를 쓴다.
 *   후자는 cmpxchg 라 경합에서 질 수 있다.
 * pt_entry_oa / pt_item_oa: 항목이 내는 출력 주소. 형식은 둘 중 하나만
 *   구현하면 되고, 나머지는 기본 구현이 채운다.
 * pt_possible_sizes: 이 단계가 만들 수 있는 페이지 크기들의 비트맵.
 *   드라이버의 pgsize_bitmap 이 여기서 나온다.
 * pt_entry_num_contig_lg2: 연속 항목 몇 개가 한 entry 를 이루는가.
 *   item 과 entry 의 구분(pt_defs.h 의 용어집)이 여기서 실체를 갖는다.
 * pt_entry_is_write_dirty 계열: 하드웨어가 쓴 흔적을 읽고 지운다.
 *   라이브 마이그레이션의 더티 추적이 이 셋 위에 선다.
 * pt_test_sw_bit_acquire / pt_set_sw_bit_release: 하드웨어가 무시하는
 *   비트를 소프트웨어가 쓴다 — 획득·해제 순서가 붙어 있어 표 내용의
 *   가시성을 그 비트로 알릴 수 있다.
 */
#ifndef __GENERIC_PT_PT_COMMON_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_PT_COMMON_H	/* [한국어] 같은 이름으로 표시 */

#include "pt_defs.h"	/* [한국어] 순회 상태와 어휘 */
#include "pt_fmt_defaults.h"	/* [한국어] 형식이 구현하지 않은 API 의 기본값 */

/**
 * pt_attr_from_entry() - Convert the permission bits back to attrs
 * @pts: Entry to convert from
 * @attrs: Resulting attrs
 *
 * Fill in the attrs with the permission bits encoded in the current leaf entry.
 * The attrs should be usable with pt_install_leaf_entry() to reconstruct the
 * same entry.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * pt_attr_from_entry - 항목에 박힌 권한 비트를 다시 attrs 로 꺼낸다
 *
 * @pts: 읽을 잎 항목.
 * @attrs: 채울 결과.
 *
 * 설치의 역방향이다. 이 함수로 꺼낸 attrs 를 그대로 pt_install_leaf_entry
 * 에 다시 넣으면 같은 항목이 나와야 한다 — 원 주석이 그 왕복을 계약으로
 * 못박고 있다.
 *
 * 어디에 쓰는가: 큰 페이지를 잘게 쪼갤 때(split) 원래 권한을 그대로
 * 물려주어야 하고, 그 권한은 항목 안에만 남아 있다.
 */
static inline void pt_attr_from_entry(const struct pt_state *pts,
				      struct pt_write_attrs *attrs);

/**
 * pt_can_have_leaf() - True if the current level can have an OA entry
 * @pts: The current level
 *
 * True if the current level can support pt_install_leaf_entry(). A leaf
 * entry produce an OA.
 */
/*
 * [한국어]
 * pt_can_have_leaf - 이 단계에 출력 주소 항목을 놓을 수 있는가
 *
 * @pts: 현재 단계.
 * @return: 놓을 수 있으면 참.
 *
 * 형식마다 큰 페이지를 허용하는 단계가 다르다. 예를 들어 최상위 단계에
 * 잎을 놓는 것을 금지하는 형식이 있다.
 */
static inline bool pt_can_have_leaf(const struct pt_state *pts);

/**
 * pt_can_have_table() - True if the current level can have a lower table
 * @pts: The current level
 *
 * Every level except 0 is allowed to have a lower table.
 */
/*
 * [한국어]
 * pt_can_have_table - 이 단계가 아래 표를 가리킬 수 있는가
 *
 * @pts: 현재 단계.
 * @return: 0단계가 아니면 참.
 *
 * 형식과 무관하게 답이 정해져 이 파일이 직접 구현한다. 0단계는 정의상
 * 잎만 담는 마지막 단계다(pt_defs.h 의 용어집).
 */
static inline bool pt_can_have_table(const struct pt_state *pts)
{
	/* No further tables at level 0 */
	return pts->level > 0;	/* [한국어] (원 주석: 0단계 아래로는 표가 없다) 정의상 마지막 단계다 */
}

/**
 * pt_clear_entries() - Make entries empty (non-present)
 * @pts: Starting table index
 * @num_contig_lg2: Number of contiguous items to clear
 *
 * Clear a run of entries. A cleared entry will load back as PT_ENTRY_EMPTY
 * and does not have any effect on table walking. The starting index must be
 * aligned to num_contig_lg2.
 */
/*
 * [한국어]
 * pt_clear_entries - 연속된 항목들을 비운다
 *
 * @pts: 시작 위치.
 * @num_contig_lg2: 지울 항목 수의 지수.
 *
 * 비운 항목은 다시 읽으면 PT_ENTRY_EMPTY 가 되고 표 걷기에 영향을 주지
 * 않는다. 시작 위치가 그 개수에 정렬되어 있어야 한다 — 연속 항목은
 * 하드웨어가 정렬된 묶음으로만 인식하기 때문이다.
 */
static inline void pt_clear_entries(struct pt_state *pts,
				    unsigned int num_contig_lg2);

/**
 * pt_entry_make_write_dirty() - Make an entry dirty
 * @pts: Table entry to change
 *
 * Make pt_entry_is_write_dirty() return true for this entry. This can be called
 * asynchronously with any other table manipulation under a RCU lock and must
 * not corrupt the table.
 */
/*
 * [한국어]
 * pt_entry_make_write_dirty - 항목에 "쓰기 있었음" 표시를 남긴다
 *
 * @pts: 고칠 항목.
 * @return: 실제로 바꾸었으면 참.
 *
 * 소프트웨어가 하드웨어 대신 표시를 남기는 경로다. 원 주석이 요구하는
 * 조건이 까다롭다 — RCU 락 아래에서 다른 표 조작과 동시에 불릴 수 있으며,
 * 그래도 표를 망가뜨리면 안 된다. 그래서 형식은 이 비트만 원자적으로
 * 세우는 방식을 써야 한다.
 */
static inline bool pt_entry_make_write_dirty(struct pt_state *pts);

/**
 * pt_entry_make_write_clean() - Make the entry write clean
 * @pts: Table entry to change
 *
 * Modify the entry so that pt_entry_is_write_dirty() == false. The HW will
 * eventually be notified of this change via a TLB flush, which is the point
 * that the HW must become synchronized. Any "write dirty" prior to the TLB
 * flush can be lost, but once the TLB flush completes all writes must make
 * their entries write dirty.
 *
 * The format should alter the entry in a way that is compatible with any
 * concurrent update from HW. The entire contiguous entry is changed.
 */
/*
 * [한국어]
 * pt_entry_make_write_clean - 더티 표시를 지운다
 *
 * @pts: 고칠 항목.
 *
 * 더티 추적의 한 주기를 닫는다. 원 주석이 짚는 동기화 계약이 핵심이다:
 * 지운 직후에는 하드웨어가 아직 옛 상태를 캐시하고 있어 그 사이의 쓰기를
 * 놓칠 수 있고, TLB 를 비운 뒤부터는 모든 쓰기가 반드시 표시를 남긴다.
 *
 * 즉 "지우고 → 무효화하고 → 그때부터 센다"가 정확한 순서다.
 * 연속 항목이면 그 묶음 전체가 함께 지워진다.
 */
static inline void pt_entry_make_write_clean(struct pt_state *pts);

/**
 * pt_entry_is_write_dirty() - True if the entry has been written to
 * @pts: Entry to query
 *
 * "write dirty" means that the HW has written to the OA translated
 * by this entry. If the entry is contiguous then the consolidated
 * "write dirty" for all the items must be returned.
 */
/*
 * [한국어]
 * pt_entry_is_write_dirty - 이 항목이 가리키는 곳에 쓰기가 있었는가
 *
 * @pts: 볼 항목.
 * @return: 있었으면 참.
 *
 * 연속 항목이면 그중 하나라도 표시가 있으면 참이다 — 묶음이 하나의
 * 논리적 항목이므로 답도 하나여야 한다.
 */
static inline bool pt_entry_is_write_dirty(const struct pt_state *pts);

/**
 * pt_dirty_supported() - True if the page table supports dirty tracking
 * @common: Page table to query
 */
/*
 * [한국어]
 * pt_dirty_supported - 이 페이지 테이블이 더티 추적을 할 수 있는가
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 할 수 있으면 참.
 *
 * 형식에 더티 비트가 있는지, 그리고 이 인스턴스가 그 기능을 켰는지를
 * 함께 본다. 드라이버가 iommufd 에 능력을 보고할 때 쓴다.
 */
static inline bool pt_dirty_supported(struct pt_common *common);

/**
 * pt_entry_num_contig_lg2() - Number of contiguous items for this leaf entry
 * @pts: Entry to query
 *
 * Return the number of contiguous items this leaf entry spans. If the entry
 * is single item it returns ilog2(1).
 */
/*
 * [한국어]
 * pt_entry_num_contig_lg2 - 이 잎이 몇 개의 항목으로 이루어졌는가
 *
 * @pts: 볼 항목.
 * @return: 항목 수의 지수. 단일 항목이면 0.
 *
 * 연속 항목(contiguous)은 인접한 표 항목 여러 개를 같은 값으로 채워
 * 하드웨어가 하나의 큰 매핑으로 인식하게 하는 기법이다. TLB 한 자리로
 * 넓은 범위를 덮어 적중률이 오른다.
 *
 * 이 값이 item 과 entry 를 가르는 축이다 — 표에는 item 이 여럿이지만
 * 논리적으로는 entry 하나다.
 */
static inline unsigned int pt_entry_num_contig_lg2(const struct pt_state *pts);

/**
 * pt_entry_oa() - Output Address for this leaf entry
 * @pts: Entry to query
 *
 * Return the output address for the start of the entry. If the entry
 * is contiguous this returns the same value for each sub-item. I.e.::
 *
 *    log2_mod(pt_entry_oa(), pt_entry_oa_lg2sz()) == 0
 *
 * See pt_item_oa(). The format should implement one of these two functions
 * depending on how it stores the OAs in the table.
 */
/*
 * [한국어]
 * pt_entry_oa - 이 잎 entry 가 내는 출력 주소의 시작
 *
 * @pts: 볼 항목.
 * @return: entry 시작의 출력 주소.
 *
 * 연속 항목이면 어느 하위 item 을 보든 같은 값이 나와야 한다 — 즉 그
 * entry 크기에 정렬된 주소다.
 *
 * 형식은 이 함수와 pt_item_oa 중 하나만 구현하면 된다. 표에 각 item 마다
 * 자기 주소를 적는 형식이면 후자가, 묶음의 시작만 적는 형식이면 전자가
 * 자연스럽고, 나머지는 기본 구현이 계산해 준다.
 */
static inline pt_oaddr_t pt_entry_oa(const struct pt_state *pts);

/**
 * pt_entry_oa_lg2sz() - Return the size of an OA entry
 * @pts: Entry to query
 *
 * If the entry is not contiguous this returns pt_table_item_lg2sz(), otherwise
 * it returns the total VA/OA size of the entire contiguous entry.
 */
/*
 * [한국어]
 * pt_entry_oa_lg2sz - 이 entry 가 덮는 크기의 지수
 *
 * @pts: 볼 항목.
 * @return: 크기의 지수.
 *
 * 단일 항목이면 그 단계의 항목 크기 그대로이고, 연속 항목이면 묶음 개수만큼
 * 지수가 더해진다. 지수끼리 더하는 것이 곧 크기를 곱하는 것이다.
 */
static inline unsigned int pt_entry_oa_lg2sz(const struct pt_state *pts)
{
	return pt_entry_num_contig_lg2(pts) + pt_table_item_lg2sz(pts);	/* [한국어] 지수를 더하는 것이 크기를 곱하는 것이다 */
}

/**
 * pt_entry_oa_exact() - Return the complete OA for an entry
 * @pts: Entry to query
 *
 * During iteration the first entry could have a VA with an offset from the
 * natural start of the entry. Return the exact OA including the pts's VA
 * offset.
 */
/*
 * [한국어]
 * pt_entry_oa_exact - 지금 보고 있는 VA 에 정확히 대응하는 출력 주소
 *
 * @pts: 볼 항목.
 * @return: entry 시작 주소에 VA 오프셋을 더한 값.
 *
 * pt_entry_oa 는 entry 의 시작만 알려 준다. 순회가 entry 한가운데의 VA 에서
 * 시작했다면 그 오프셋만큼을 더해야 진짜 물리 주소가 나온다.
 *
 * iova_to_phys 가 이 함수를 쓴다 — 사용자가 물어본 주소가 큰 페이지의
 * 한가운데일 수 있기 때문이다.
 */
static inline pt_oaddr_t pt_entry_oa_exact(const struct pt_state *pts)
{
	return _pt_entry_oa_fast(pts) |	/* [한국어] entry 시작의 출력 주소에 */
	       log2_mod(pts->range->va, pt_entry_oa_lg2sz(pts));	/* [한국어] 현재 VA 의 entry 내부 오프셋을 얹는다 */
}

/**
 * pt_full_va_prefix() - The top bits of the VA
 * @common: Page table to query
 *
 * This is usually 0, but some formats have their VA space going downward from
 * PT_VADDR_MAX, and will return that instead. This value must always be
 * adjusted by struct pt_common max_vasz_lg2.
 */
/*
 * [한국어]
 * pt_full_va_prefix - 이 형식의 VA 상위 비트 패턴
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 보통 0, 부호 확장 형식이면 위쪽 절반의 접두 비트.
 *
 * x86 처럼 주소 공간이 위아래로 갈리는 형식이 있다. 위쪽 절반의 주소는
 * 상위 비트가 모두 1 이라, 표 색인을 계산하기 전에 그 접두를 떼어 내고
 * 결과를 돌려줄 때 다시 붙여야 한다.
 */
static inline pt_vaddr_t pt_full_va_prefix(const struct pt_common *common);

/**
 * pt_has_system_page_size() - True if level 0 can install a PAGE_SHIFT entry
 * @common: Page table to query
 *
 * If true the caller can use, at level 0, pt_install_leaf_entry(PAGE_SHIFT).
 * This is useful to create optimized paths for common cases of PAGE_SIZE
 * mappings.
 */
/*
 * [한국어]
 * pt_has_system_page_size - 0단계에서 커널 페이지 크기를 그대로 쓸 수 있는가
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 쓸 수 있으면 참.
 *
 * 참이면 가장 흔한 경우 — PAGE_SIZE 단위 매핑 — 를 크기 계산 없이 바로
 * 처리하는 빠른 경로를 쓸 수 있다. 형식의 최소 페이지가 커널 페이지보다
 * 크면 거짓이 된다.
 */
static inline bool pt_has_system_page_size(const struct pt_common *common);

/**
 * pt_install_leaf_entry() - Write a leaf entry to the table
 * @pts: Table index to change
 * @oa: Output Address for this leaf
 * @oasz_lg2: Size in VA/OA for this leaf
 * @attrs: Attributes to modify the entry
 *
 * A leaf OA entry will return PT_ENTRY_OA from pt_load_entry(). It translates
 * the VA indicated by pts to the given OA.
 *
 * For a single item non-contiguous entry oasz_lg2 is pt_table_item_lg2sz().
 * For contiguous it is pt_table_item_lg2sz() + num_contig_lg2.
 *
 * This must not be called if pt_can_have_leaf() == false. Contiguous sizes
 * not indicated by pt_possible_sizes() must not be specified.
 */
/*
 * [한국어]
 * pt_install_leaf_entry - 표에 잎 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @oa: 이 잎이 낼 출력 주소.
 * @oasz_lg2: 이 잎이 덮는 크기의 지수.
 * @attrs: 얹을 권한·속성.
 *
 * 매핑이 실제로 생기는 지점이다. 이후 pt_load_entry 는 이 자리에서
 * PT_ENTRY_OA 를 돌려주고, 하드웨어는 그 VA 를 이 OA 로 옮긴다.
 *
 * 호출자가 지켜야 할 두 조건이 있다: 이 단계가 잎을 허용해야 하고
 * (pt_can_have_leaf), 크기가 pt_possible_sizes 가 알려 준 것 중 하나여야
 * 한다. 어긋나면 하드웨어가 표를 잘못 읽는다.
 *
 * cmpxchg 가 아닌 점이 pt_install_table 과 다르다 — 잎 설치는 상위 락이
 * 직렬화한다.
 */
static inline void pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
					 unsigned int oasz_lg2,
					 const struct pt_write_attrs *attrs);

/**
 * pt_install_table() - Write a table entry to the table
 * @pts: Table index to change
 * @table_pa: CPU physical address of the lower table's memory
 * @attrs: Attributes to modify the table index
 *
 * A table entry will return PT_ENTRY_TABLE from pt_load_entry(). The table_pa
 * is the table at pts->level - 1. This is done by cmpxchg so pts must have the
 * current entry loaded. The pts is updated with the installed entry.
 *
 * This must not be called if pt_can_have_table() == false.
 *
 * Returns: true if the table was installed successfully.
 */
/*
 * [한국어]
 * pt_install_table - 아래 단계 표를 가리키는 항목을 쓴다
 *
 * @pts: 쓸 위치(현재 값이 담겨 있어야 한다).
 * @table_pa: 아래 표의 CPU 물리 주소.
 * @attrs: 얹을 속성.
 * @return: 내가 꽂았으면 참, 다른 스레드가 먼저 꽂았으면 거짓.
 *
 * 잎 설치와 달리 cmpxchg 로 한다. 여러 스레드가 같은 자리에 각자 만든
 * 표를 꽂으려 경쟁할 수 있기 때문이다(pt_defs.h 의 pt_table_install64
 * 참고). 진 쪽은 자기 표를 버리고 그 단계를 다시 읽어야 한다.
 *
 * 성공하면 pts 의 항목 값도 새것으로 갱신되어, 호출자가 그대로 아래로
 * 내려갈 수 있다.
 */
static inline bool pt_install_table(struct pt_state *pts, pt_oaddr_t table_pa,
				    const struct pt_write_attrs *attrs);

/**
 * pt_item_oa() - Output Address for this leaf item
 * @pts: Item to query
 *
 * Return the output address for this item. If the item is part of a contiguous
 * entry it returns the value of the OA for this individual sub item.
 *
 * See pt_entry_oa(). The format should implement one of these two functions
 * depending on how it stores the OA's in the table.
 */
/*
 * [한국어]
 * pt_item_oa - 이 item 하나가 내는 출력 주소
 *
 * @pts: 볼 item.
 * @return: 그 item 의 출력 주소.
 *
 * 연속 묶음의 일부라면 그 item 자신의 주소를 돌려준다 — entry 시작이
 * 아니라. 표에 item 마다 주소를 적는 형식이 이쪽을 구현한다.
 */
static inline pt_oaddr_t pt_item_oa(const struct pt_state *pts);

/**
 * pt_load_entry_raw() - Read from the location pts points at into the pts
 * @pts: Table index to load
 *
 * Return the type of entry that was loaded. pts->entry will be filled in with
 * the entry's content. See pt_load_entry()
 */
/*
 * [한국어]
 * pt_load_entry_raw - 항목을 한 번 읽어 pts 에 담고 종류를 판정한다
 *
 * @pts: 읽을 위치.
 * @return: 비었는가, 표인가, 출력 주소인가.
 *
 * 순회의 가장 안쪽이다. 한 번만 읽어 담는 것이 중요한데, 여러 번 읽으면
 * 그사이 다른 CPU 가 바꾼 값을 섞어 쓰게 된다 — 예를 들어 종류는 표로
 * 보고 주소는 새 잎에서 읽는 사고가 난다.
 */
static inline enum pt_entry_type pt_load_entry_raw(struct pt_state *pts);

/**
 * pt_max_oa_lg2() - Return the maximum OA the table format can hold
 * @common: Page table to query
 *
 * The value oalog2_to_max_int(pt_max_oa_lg2()) is the MAX for the
 * OA. This is the absolute maximum address the table can hold. struct pt_common
 * max_oasz_lg2 sets a lower dynamic maximum based on HW capability.
 */
/*
 * [한국어]
 * pt_max_oa_lg2 - 이 형식이 표현할 수 있는 최대 출력 주소의 폭
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 폭의 지수.
 *
 * 형식이 정하는 절대 상한이다. 실제 하드웨어가 더 좁을 수 있어,
 * pt_common 의 max_oasz_lg2 가 그보다 낮은 동적 상한을 따로 둔다.
 */
static inline unsigned int
pt_max_oa_lg2(const struct pt_common *common);

/**
 * pt_num_items_lg2() - Return the number of items in this table level
 * @pts: The current level
 *
 * The number of items in a table level defines the number of bits this level
 * decodes from the VA. This function is not called for the top level,
 * so it does not need to compute a special value for the top case. The
 * result for the top is based on pt_common max_vasz_lg2.
 *
 * The value is used as part of determining the table indexes via the
 * equation::
 *
 *   log2_mod(log2_div(VA, pt_table_item_lg2sz()), pt_num_items_lg2())
 */
/*
 * [한국어]
 * pt_num_items_lg2 - 이 단계 표의 항목 수(지수)
 *
 * @pts: 현재 단계.
 * @return: 항목 수의 지수 — 이 단계가 VA 에서 소비하는 비트 수와 같다.
 *
 * 표 하나가 VA 의 몇 비트를 해석하는지가 곧 항목 수다. 4KB 표에 8바이트
 * 항목이면 512 = 2^9 개라 9비트를 소비한다.
 *
 * 최상위 단계에서는 부르지 않는다 — 그쪽 항목 수는 주소 공간 폭에서
 * 나오므로 형식이 아니라 pt_common 의 max_vasz_lg2 가 정한다.
 */
static inline unsigned int pt_num_items_lg2(const struct pt_state *pts);

/**
 * pt_pgsz_lg2_to_level - Return the level that maps the page size
 * @common: Page table to query
 * @pgsize_lg2: Log2 page size
 *
 * Returns the table level that will map the given page size. The page
 * size must be part of the pt_possible_sizes() for some level.
 */
/*
 * [한국어]
 * pt_pgsz_lg2_to_level - 그 페이지 크기를 매핑하는 단계를 찾는다
 *
 * @common: 페이지 테이블 인스턴스.
 * @pgsize_lg2: 페이지 크기의 지수.
 * @return: 그 크기를 담당하는 단계 번호.
 *
 * 매핑을 시작하기 전에 "어느 단계까지 내려가야 하는가"를 정한다. 넘긴
 * 크기는 반드시 어느 단계의 pt_possible_sizes 에 들어 있어야 한다.
 */
static inline unsigned int pt_pgsz_lg2_to_level(struct pt_common *common,
						unsigned int pgsize_lg2);

/**
 * pt_possible_sizes() - Return a bitmap of possible output sizes at this level
 * @pts: The current level
 *
 * Each level has a list of possible output sizes that can be installed as
 * leaf entries. If pt_can_have_leaf() is false returns zero.
 *
 * Otherwise the bit in position pt_table_item_lg2sz() should be set indicating
 * that a non-contiguous single item leaf entry is supported. The following
 * pt_num_items_lg2() number of bits can be set indicating contiguous entries
 * are supported. Bit pt_table_item_lg2sz() + pt_num_items_lg2() must not be
 * set, contiguous entries cannot span the entire table.
 *
 * The OR of pt_possible_sizes() of all levels is the typical bitmask of all
 * supported sizes in the entire table.
 */
/*
 * [한국어]
 * pt_possible_sizes - 이 단계가 만들 수 있는 페이지 크기들
 *
 * @pts: 현재 단계.
 * @return: 크기 지수를 비트 위치로 하는 비트맵. 잎을 못 놓으면 0.
 *
 * 원 주석이 비트맵의 모양을 정확히 규정한다. 항목 크기 자리의 비트는
 * 반드시 서 있고(단일 항목 잎), 그 위로 항목 수만큼의 비트가 연속 항목을
 * 뜻한다. 다만 표 전체를 덮는 자리는 설 수 없다 — 연속 묶음이 표 하나를
 * 통째로 차지하면 그것은 한 단계 위의 잎과 같아진다.
 *
 * 모든 단계의 결과를 OR 하면 그 형식이 지원하는 페이지 크기 전체가 되고,
 * 그것이 드라이버가 코어에 보고하는 pgsize_bitmap 이다.
 */
static inline pt_vaddr_t pt_possible_sizes(const struct pt_state *pts);

/**
 * pt_table_item_lg2sz() - Size of a single item entry in this table level
 * @pts: The current level
 *
 * The size of the item specifies how much VA and OA a single item occupies.
 *
 * See pt_entry_oa_lg2sz() for the same value including the effect of contiguous
 * entries.
 */
/*
 * [한국어]
 * pt_table_item_lg2sz - 이 단계 항목 하나가 덮는 크기의 지수
 *
 * @pts: 현재 단계.
 * @return: 크기의 지수.
 *
 * 0단계면 최소 페이지 크기이고, 한 단계 올라갈 때마다 그 단계의 항목 수
 * 만큼 지수가 커진다.
 */
static inline unsigned int pt_table_item_lg2sz(const struct pt_state *pts);

/**
 * pt_table_oa_lg2sz() - Return the VA/OA size of the entire table
 * @pts: The current level
 *
 * Return the size of VA decoded by the entire table level.
 */
/*
 * [한국어]
 * pt_table_oa_lg2sz - 이 단계 표 전체가 덮는 크기의 지수
 *
 * @pts: 현재 단계.
 * @return: 크기의 지수.
 *
 * 보통은 항목 크기에 항목 수를 곱한 값이다. 두 가지 예외가 이 함수의
 * 존재 이유다.
 *  - 최상위 단계는 항목 수가 주소 공간 폭에서 나오므로 그 값을 그대로 쓴다.
 *  - 그 아래에서도 인스턴스가 좁은 주소 공간을 쓰면 표가 덮는 범위가
 *    그보다 클 수 없다.
 */
static inline unsigned int pt_table_oa_lg2sz(const struct pt_state *pts)
{
	if (pts->range->top_level == pts->level)	/* [한국어] 최상위 단계면 */
		return pts->range->max_vasz_lg2;	/* [한국어] 항목 수가 주소 공간 폭에서 나온다 */
	return min_t(unsigned int, pts->range->common->max_vasz_lg2,	/* [한국어] 인스턴스의 주소 공간을 넘을 수 없고 */
		     pt_num_items_lg2(pts) + pt_table_item_lg2sz(pts));	/* [한국어] 그 안에서는 항목 수 × 항목 크기 */
}

/**
 * pt_table_pa() - Return the CPU physical address of the table entry
 * @pts: Entry to query
 *
 * This is only ever called on PT_ENTRY_TABLE entries. Must return the same
 * value passed to pt_install_table().
 */
/*
 * [한국어]
 * pt_table_pa - 이 표 항목이 가리키는 아래 표의 물리 주소
 *
 * @pts: 볼 항목(반드시 PT_ENTRY_TABLE).
 * @return: 아래 표의 CPU 물리 주소.
 *
 * pt_install_table 에 넘겼던 값과 같은 값이 나와야 한다 — 형식이 주소에
 * 암호화 비트 같은 것을 얹었다면 여기서 다시 벗겨야 한다.
 */
static inline pt_oaddr_t pt_table_pa(const struct pt_state *pts);

/**
 * pt_table_ptr() - Return a CPU pointer for a table item
 * @pts: Entry to query
 *
 * Same as pt_table_pa() but returns a CPU pointer.
 */
/*
 * [한국어]
 * pt_table_ptr - 아래 표를 CPU 포인터로 돌려준다
 *
 * @pts: 볼 항목.
 * @return: 아래 표의 커널 가상 주소.
 *
 * 표는 커널이 직접 매핑한 메모리라 __va 로 바로 옮길 수 있다. 순회가
 * 한 단계 내려갈 때 이 값을 쓴다.
 */
static inline struct pt_table_p *pt_table_ptr(const struct pt_state *pts)
{
	return __va(pt_table_pa(pts));	/* [한국어] 표는 커널이 직접 매핑한 메모리다 */
}

/**
 * pt_max_sw_bit() - Return the maximum software bit usable for any level and
 *                   entry
 * @common: Page table
 *
 * The swbit can be passed as bitnr to the other sw_bit functions.
 */
/*
 * [한국어]
 * pt_max_sw_bit - 소프트웨어가 쓸 수 있는 비트의 최대 번호
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 비트 번호.
 *
 * 형식마다 하드웨어가 무시하는 여분 비트의 개수가 다르다. 그 범위 안의
 * 번호만 아래 두 함수에 넘길 수 있다.
 */
static inline unsigned int pt_max_sw_bit(struct pt_common *common);

/**
 * pt_test_sw_bit_acquire() - Read a software bit in an item
 * @pts: Entry to read
 * @bitnr: Bit to read
 *
 * Software bits are ignored by HW and can be used for any purpose by the
 * software. This does a test bit and acquire operation.
 */
/*
 * [한국어]
 * pt_test_sw_bit_acquire - 소프트웨어 비트를 읽는다(획득 순서)
 *
 * @pts: 볼 항목.
 * @bitnr: 볼 비트.
 * @return: 서 있으면 참.
 *
 * 하드웨어가 무시하는 비트라 소프트웨어가 마음대로 쓴다. 획득 순서가
 * 붙어 있는 것이 요점이다 — 이 비트가 서 있는 것을 본 CPU 는, 그 비트를
 * 세우기 전에 이루어진 모든 쓰기도 반드시 본다.
 *
 * 그래서 "이 표는 준비가 끝났다"를 알리는 깃발로 쓸 수 있다.
 */
static inline bool pt_test_sw_bit_acquire(struct pt_state *pts,
					  unsigned int bitnr);

/**
 * pt_set_sw_bit_release() - Set a software bit in an item
 * @pts: Entry to set
 * @bitnr: Bit to set
 *
 * Software bits are ignored by HW and can be used for any purpose by the
 * software. This does a set bit and release operation.
 */
/*
 * [한국어]
 * pt_set_sw_bit_release - 소프트웨어 비트를 세운다(해제 순서)
 *
 * @pts: 고칠 항목.
 * @bitnr: 세울 비트.
 *
 * 위 함수의 짝이다. 해제 순서라 이 비트를 세우기 전의 쓰기들이 먼저
 * 보이는 것이 보장된다.
 */
static inline void pt_set_sw_bit_release(struct pt_state *pts,
					 unsigned int bitnr);

/**
 * pt_load_entry() - Read from the location pts points at into the pts
 * @pts: Table index to load
 *
 * Set the type of entry that was loaded. pts->entry and pts->table_lower
 * will be filled in with the entry's content.
 */
/*
 * [한국어]
 * pt_load_entry - 항목을 읽고, 표라면 아래 표 주소까지 미리 구해 둔다
 *
 * @pts: 읽을 위치.
 *
 * raw 판에 한 걸음을 더한 것이다. 표 항목이면 거의 언제나 곧바로 내려가게
 * 되므로, 주소 변환을 여기서 미리 해 둔다.
 *
 * 순회 코드가 부르는 것은 대개 이쪽이다.
 */
static inline void pt_load_entry(struct pt_state *pts)
{
	pts->type = pt_load_entry_raw(pts);	/* [한국어] 한 번 읽어 값과 종류를 함께 담는다 */
	if (pts->type == PT_ENTRY_TABLE)	/* [한국어] 표 항목이면 */
		pts->table_lower = pt_table_ptr(pts);	/* [한국어] 거의 언제나 내려가므로 주소를 미리 구해 둔다 */
}
#endif	/* [한국어] 포함 방지 끝 */
