/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * This header is included before the format. It contains definitions
 * that are required to compile the format. The header order is:
 *  pt_defs.h
 *  fmt_XX.h
 *  pt_common.h
 */
/*
 * [한국어 설명] 형식 헤더보다 먼저 필요한 공통 정의 (pt_defs.h)
 *
 * === 파일의 역할 ===
 * generic_pt 의 어휘를 정하는 파일이다. 순회 상태(struct pt_state), 범위
 * (struct pt_range), 항목의 종류(enum pt_entry_type), 기능 비트 질의,
 * 그리고 형식이 쓸 산술 매크로가 여기 있다.
 *
 * 원 주석이 밝히듯 포함 순서가 정해져 있다: pt_defs.h → 형식 헤더 →
 * pt_common.h. 형식 헤더가 컴파일되려면 여기 있는 것들이 먼저 있어야 하고,
 * pt_common.h 는 형식 헤더가 정의한 접근자를 전제로 한다.
 *
 * 파일 중간의 "Generic Page Table Language" 문서 블록이 이 계층 전체의
 * 용어집이다. VA/OA, leaf, level, item 과 entry 의 구분, contig_count,
 * lg2 표기 — 이 낱말들이 generic_pt 전역에서 같은 뜻으로 쓰인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c → iommu_template.h → defs_<형식>.h → [이 파일]
 *   → <형식>.h → pt_common.h → iommu_pt.h → IOMMU 드라이버
 *
 * 실행 컨텍스트: 전부 인라인이다. 여기 있는 함수는 형식이 컴파일 시점에
 * 고정되므로 조건문 대부분이 상수로 접혀 사라진다.
 *
 * === 타 모듈과의 연결 ===
 * 위: 모든 형식 헤더와 pt_common.h, pt_iter.h, iommu_pt.h.
 * 아래: <linux/generic_pt/common.h>(struct pt_common, 기능 비트 번호),
 *       pt_log2.h(산술), <linux/atomic.h>(항목 설치의 cmpxchg).
 *
 * 데이터 흐름: 드라이버가 준 VA 범위가 struct pt_range 로 들어오고,
 * 순회 중 각 단계의 위치가 struct pt_state 에 담긴다. 그 둘이 이 계층의
 * 모든 함수 사이를 오간다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct pt_range: 한 번의 연산이 다룰 VA 범위와 최상위 표의 위치.
 * struct pt_state: 한 단계에서의 현재 위치 — xa_state 와 같은 발상이다.
 * enum pt_entry_type: 항목이 비었는가, 아래 표를 가리키는가, 주소를 내는가.
 * pt_table_install32/64: 새 표 포인터를 원자적으로 꽂는다. 경합에서 지면
 *   거짓을 돌려주고, 진 쪽이 자기 표를 버리고 다시 읽는다.
 * pt_feature / pts_feature: 이 형식·이 인스턴스에서 기능이 켜져 있는가.
 * fvalog2_*: 지수가 주소 폭과 같아지는 극단(전 주소 공간)에서도 답이
 *   정의되게 만든 산술. 보통 버전은 그 지점에서 시프트가 미정의가 된다.
 * pt_top_set / pt_top_get_level: 최상위 표 주소와 단계 수를 한 워드에
 *   함께 담아 원자적으로 바꾼다.
 */
#ifndef __GENERIC_PT_DEFS_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_DEFS_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/generic_pt/common.h>	/* [한국어] struct pt_common 과 기능 비트 번호 */

#include <linux/types.h>	/* [한국어] 고정 폭 정수 */
#include <linux/atomic.h>	/* [한국어] 표 포인터를 원자적으로 꽂는 cmpxchg */
#include <linux/bits.h>	/* [한국어] BIT() 매크로 */
#include <linux/limits.h>	/* [한국어] U32_MAX/U64_MAX — 주소 한계 계산 */
#include <linux/bug.h>	/* [한국어] WARN_ON */
#include <linux/kconfig.h>	/* [한국어] IS_ENABLED — 설정을 상수 조건으로 */
#include "pt_log2.h"	/* [한국어] 지수 기반 산술 */

/* Header self-compile default defines */
#ifndef pt_write_attrs	/* [한국어] (원 주석: 헤더 단독 컴파일용 기본값) 형식 없이 이 헤더만 컴파일할 때 */
typedef u64 pt_vaddr_t;	/* [한국어] 형식이 정하지 않았으면 64비트로 가정한다 */
typedef u64 pt_oaddr_t;	/* [한국어] 출력 주소도 마찬가지 */
#endif

struct pt_table_p;	/* [한국어] 표 메모리를 가리키는 불투명 타입 — 형식만 내용을 안다 */

/*
 * [한국어] 이 형식이 다루는 주소의 한계값들.
 * 주소 타입이 32비트인지 64비트인지에 따라 갈리며, sizeof 는 컴파일 시
 * 상수라 실제 코드에는 한쪽만 남는다.
 */
enum {
	PT_VADDR_MAX = sizeof(pt_vaddr_t) == 8 ? U64_MAX : U32_MAX,
	/* [한국어] 입력(가상/IOVA) 주소로 표현할 수 있는 최대값.
	 * 설정자: 이 상수 자체가 정의다. 주소 타입의 크기에서 유도된다.
	 * 읽는 자: 순회 범위의 끝을 나타낼 때, 그리고 오버플로 없이 "마지막 주소"를
	 *   다루어야 하는 계산.
	 * 왜 삼항 연산자인가: sizeof 는 컴파일 시 상수라 이 식 전체가 컴파일 시점에
	 *   한쪽으로 접힌다. 실행 중 분기가 남지 않으므로 #ifdef 를 쓰는 것과 비용이
	 *   같으면서, 조건이 타입 정의 한 곳에서만 나온다는 이점이 있다.
	 * 값 범위: U32_MAX 또는 U64_MAX.
	 * 왜 "최대 주소"이지 "크기"가 아닌가: 64비트 전 범위를 크기로 표현하면
	 *   2^64 라 그 타입에 담기지 않는다. 그래서 이 계층은 언제나 마지막 주소로 다룬다. */
	PT_VADDR_MAX_LG2 = sizeof(pt_vaddr_t) == 8 ? 64 : 32,
	/* [한국어] 입력 주소 폭의 비트 수.
	 * 설정자/읽는 자: 위 PT_VADDR_MAX 와 같은 자리들. 특히 자릿수 계산이 필요한
	 *   곳에서 최대값 대신 이 지수를 쓴다.
	 * 값 범위: 32 또는 64.
	 * 왜 지수로도 들고 있는가: 위 주석대로 크기 자체(2^64)는 타입에 담을 수 없다.
	 *   단계 수를 계산하거나 시프트 양을 정할 때는 지수 쪽이 필요하다.
	 * 컴파일 시 접히는 것은 위와 같다. */
	PT_OADDR_MAX = sizeof(pt_oaddr_t) == 8 ? U64_MAX : U32_MAX,
	/* [한국어] 출력(물리) 주소로 표현할 수 있는 최대값.
	 * 설정자/읽는 자: 매핑하려는 물리 주소가 이 계층이 다룰 수 있는 범위인지
	 *   검사할 때.
	 * 값 범위: U32_MAX 또는 U64_MAX.
	 * 입력 주소와 타입이 따로인 이유: 32비트 시스템에서 IOVA 는 32비트인데 물리
	 *   주소는 PAE 로 넓어질 수 있다. 반대로 64비트에서는 IOVA 가 물리 주소보다
	 *   넓다. 둘을 같은 타입으로 묶으면 어느 한쪽이 낭비되거나 모자란다. */
	PT_OADDR_MAX_LG2 = sizeof(pt_oaddr_t) == 8 ? 64 : 32,
	/* [한국어] 출력 주소 폭의 비트 수.
	 * 설정자/읽는 자: 위 PT_OADDR_MAX 와 같은 자리들 중, 자릿수가 필요한 곳.
	 * 값 범위: 32 또는 64.
	 * 위 PT_VADDR_MAX_LG2 와 같은 이유로 최대값과 지수를 함께 둔다. */
};

/*
 * The format instantiation can have features wired off or on to optimize the
 * code gen. Supported features are just a reflection of what the current set of
 * kernel users want to use.
 */
#ifndef PT_SUPPORTED_FEATURES	/* [한국어] (원 주석: 형식마다 기능을 켜고 꺼 코드 생성을 최적화한다) */
#define PT_SUPPORTED_FEATURES 0	/* [한국어] 정하지 않았으면 선택 기능이 하나도 없다 */
#endif

/*
 * When in debug mode we compile all formats with all features. This allows the
 * kunit to test the full matrix. SIGN_EXTEND can't co-exist with DYNAMIC_TOP or
 * FULL_VA. DMA_INCOHERENT requires a SW bit that not all formats have
 */
#if IS_ENABLED(CONFIG_DEBUG_GENERIC_PT)	/* [한국어] (원 주석: 디버그에서는 모든 형식을 모든 기능으로 컴파일해 kunit 이 전 조합을 시험한다) */
enum {
	PT_ORIG_SUPPORTED_FEATURES = PT_SUPPORTED_FEATURES,
	/* [한국어] (디버그 빌드 전용) 형식이 원래 허용한 기능 집합을 따로 남겨 둔 사본.
	 * 설정자: 이 상수 자체가 정의다.
	 * 읽는 자: 바로 아래 PT_DEBUG_SUPPORTED_FEATURES 의 계산.
	 * 왜 사본이 필요한가: 디버그 빌드에서는 kunit 이 기능 조합을 전부 시험할 수
	 *   있도록 PT_SUPPORTED_FEATURES 를 넓혀 재정의한다. 그런데 넓힐 수 없는
	 *   기능이 두 가지 있어(원 주석 참고), 그것을 가려내려면 "원래 무엇이
	 *   허용되었는가"를 알아야 한다.
	 * 넓힐 수 없는 두 가지: DMA_INCOHERENT 는 형식이 소프트웨어 비트를 제공해야
	 *   하고, SIGN_EXTEND 는 DYNAMIC_TOP/FULL_VA 와 공존할 수 없다. */
	PT_DEBUG_SUPPORTED_FEATURES =	/* [한국어] 시험용으로 넓힌 집합 */
		UINT_MAX &	/* [한국어] 일단 전부 켜고 */
		~((PT_ORIG_SUPPORTED_FEATURES & BIT(PT_FEAT_DMA_INCOHERENT) ?	/* [한국어] 원래 없던 DMA_INCOHERENT 는 뺀다 */
			   0 :
			   BIT(PT_FEAT_DMA_INCOHERENT))) &	/* [한국어] 그 기능은 형식마다 소프트웨어 비트가 필요해 아무 데나 켤 수 없다 */
		~((PT_ORIG_SUPPORTED_FEATURES & BIT(PT_FEAT_SIGN_EXTEND)) ?	/* [한국어] 부호 확장을 쓰는 형식이면 */
			  BIT(PT_FEAT_DYNAMIC_TOP) | BIT(PT_FEAT_FULL_VA) :	/* [한국어] 그와 공존할 수 없는 둘을 빼고 */
			  BIT(PT_FEAT_SIGN_EXTEND)),	/* [한국어] 부호 확장과 공존할 수 없는 조합을 걸러낸 결과 */
};
#undef PT_SUPPORTED_FEATURES	/* [한국어] 원래 정의를 지우고 */
#define PT_SUPPORTED_FEATURES PT_DEBUG_SUPPORTED_FEATURES	/* [한국어] 넓힌 집합으로 갈아 끼운다 */
#endif

#ifndef PT_FORCE_ENABLED_FEATURES	/* [한국어] 강제 기능을 정하지 않은 형식이면 */
#define PT_FORCE_ENABLED_FEATURES 0	/* [한국어] 늘 켜지는 것이 없다 */
#endif

/**
 * DOC: Generic Page Table Language
 *
 * Language used in Generic Page Table
 *  VA
 *     The input address to the page table, often the virtual address.
 *  OA
 *     The output address from the page table, often the physical address.
 *  leaf
 *     An entry that results in an output address.
 *  start/end
 *     An half-open range, e.g. [0,0) refers to no VA.
 *  start/last
 *     An inclusive closed range, e.g. [0,0] refers to the VA 0
 *  common
 *     The generic page table container struct pt_common
 *  level
 *     Level 0 is always a table of only leaves with no futher table pointers.
 *     Increasing levels increase the size of the table items. The least
 *     significant VA bits used to index page tables are used to index the Level
 *     0 table. The various labels for table levels used by HW descriptions are
 *     not used.
 *  top_level
 *     The inclusive highest level of the table. A two-level table
 *     has a top level of 1.
 *  table
 *     A linear array of translation items for that level.
 *  index
 *     The position in a table of an element: item = table[index]
 *  item
 *     A single index in a table
 *  entry
 *     A single logical element in a table. If contiguous pages are not
 *     supported then item and entry are the same thing, otherwise entry refers
 *     to all the items that comprise a single contiguous translation.
 *  item/entry_size
 *     The number of bytes of VA the table index translates for.
 *     If the item is a table entry then the next table covers
 *     this size. If the entry translates to an output address then the
 *     full OA is: OA | (VA % entry_size)
 *  contig_count
 *     The number of consecutive items fused into a single entry.
 *     item_size * contig_count is the size of that entry's translation.
 *  lg2
 *     Indicates the value is encoded as log2, i.e. 1<<x is the actual value.
 *     Normally the compiler is fine to optimize divide and mod with log2 values
 *     automatically when inlining, however if the values are not constant
 *     expressions it can't. So we do it by hand; we want to avoid 64-bit
 *     divmod.
 */

/* Returned by pt_load_entry() and for_each_pt_level_entry() */
/*
 * [한국어] (위 영어 주석에 이어)
 * 항목 하나를 읽었을 때 나올 수 있는 세 가지.
 * 순회 코드는 이 값으로 다음 행동을 정한다 — 비었으면 만들거나 건너뛰고,
 * 표면 한 단계 내려가고, 주소면 거기서 멈춘다.
 */
enum pt_entry_type {
	PT_ENTRY_EMPTY,
	/* [한국어] 항목이 비어 있다 — 이 자리에는 매핑이 없다.
	 * 설정자: pt_entry_type() 이 항목의 존재 비트를 보고 판정해 돌려준다.
	 * 읽는 자: 순회 코드. 매핑을 만드는 중이면 여기에 표나 잎을 새로 채우고,
	 *   해제나 조회 중이면 이 구간을 건너뛴다.
	 * 값이 0 인 이유(명시되어 있지 않지만 enum 의 첫 값이다): 0 으로 초기화된
	 *   표가 곧 빈 표가 되도록 맞춘 것이다.
	 * "비어 있다"의 판정 기준은 형식마다 다르다 — 존재 비트가 0 인 경우도 있고,
	 *   VT-d 2단계처럼 읽기/쓰기 권한이 모두 0 인 경우도 있다. 그 차이를 형식별
	 *   pt_entry_type() 이 흡수해, 공통 코드는 이 세 값만 보면 된다. */
	/* Entry is valid and points to a lower table level */
	PT_ENTRY_TABLE,
	/* [한국어] (원 주석: 유효하며 아래 단계 표를 가리킨다) 항목이 다음 단계 표를 가리킨다.
	 * 설정자: pt_entry_type() 의 판정.
	 * 읽는 자: 순회 코드가 이 값을 보면 항목에서 표의 주소를 꺼내 한 단계 내려간다.
	 * 왜 EMPTY 와 구분해야 하는가: 둘 다 "여기에 잎이 없다"이지만, 표가 이미
	 *   있으면 그것을 재사용해야 하고 비어 있으면 새로 잡아야 한다. 잘못 판정하면
	 *   이미 있는 표를 덮어써 그 아래 매핑이 통째로 사라진다.
	 * 형식별 판정 기준: 다음 단계 코드 필드를 보는 형식(AMD v1), 큰 페이지 비트가
	 *   없는 것으로 아는 형식(x86-64, VT-d), 권한 비트가 모두 0 인 것으로 아는
	 *   형식(RISC-V)이 있다. */
	/* Entry is valid and returns an output address */
	PT_ENTRY_OA,
	/* [한국어] (원 주석: 유효하며 출력 주소를 낸다) 항목이 잎이다 — 여기서 순회가 끝난다.
	 * 설정자: pt_entry_type() 의 판정.
	 * 읽는 자: 순회 코드가 이 값을 보면 항목에서 출력 주소를 꺼내고 내려가지 않는다.
	 * OA 는 output address 의 줄임이다. 물리 주소라고 하지 않는 이유는, 중첩
	 *   변환에서는 이 결과가 다시 바깥 단계의 입력이 되어 아직 물리 주소가
	 *   아닐 수 있기 때문이다.
	 * 잎이 어느 단계에 있는지에 따라 매핑 크기가 정해진다 — 단계 0 이면 4KB,
	 *   단계 1 이면 2MB 같은 식이다. 그래서 이 값 하나만으로는 크기를 알 수 없고
	 *   순회가 지금 몇 단계에 있는지를 함께 봐야 한다. */
};

/*
 * [한국어] 한 번의 연산이 다룰 VA 범위와 그 출발점.
 * map/unmap/iova_to_phys 같은 모든 진입점이 먼저 이 구조체를 만들고,
 * 순회 함수들이 그것을 들고 다닌다.
 */
struct pt_range {
	struct pt_common *common;
	/* [한국어] 이 범위가 속한 페이지 테이블 인스턴스.
	 * 설정자: pt_make_range() 계열이 도메인에서 꺼내 채운다.
	 * 읽는 자: 기능 질의(pts_feature)와 최상위 표 조회가 여기서 출발한다.
	 * 값 범위: 유효한 포인터. 범위가 사는 동안 바뀌지 않는다.
	 * 동기화: 구조체 자체는 호출 스택에 있어 공유되지 않는다. 가리키는
	 *   pt_common 은 드라이버의 락이 지킨다. */
	struct pt_table_p *top_table;
	/* [한국어] 순회를 시작할 최상위 표의 주소.
	 * 설정자: 범위를 만들 때 common->top_of_table 에서 꺼내 온다.
	 * 읽는 자: 순회의 첫 단계.
	 * 값 범위: 표 메모리의 커널 가상 주소.
	 * 동기화: 한 번 읽어 고정해 두므로, 순회 도중 최상위가 바뀌어도 이
	 *   범위는 일관된 표를 본다 — DYNAMIC_TOP 형식에서 중요하다. */
	pt_vaddr_t va;
	/* [한국어] 다룰 범위의 시작 주소.
	 * 설정자: 진입점이 사용자 인자에서 채운다. 순회가 진행되며 앞으로 간다.
	 * 읽는 자: 각 단계의 색인 계산.
	 * 값 범위: 0 이상 last_va 이하.
	 * 동기화: 호출 스택 값. */
	pt_vaddr_t last_va;
	/* [한국어] 범위의 마지막 주소(포함).
	 * 설정자: 진입점이 시작+길이-1 로 채운다.
	 * 읽는 자: 순회 종료 판정.
	 * 값 범위: va 이상. 끝을 배타적으로 두지 않는 이유는 전 주소 공간을
	 *   표현할 때 끝값이 넘치기 때문이다.
	 * 동기화: 호출 스택 값. */
	u8 top_level;
	/* [한국어] 최상위 표의 단계 번호(0 이 가장 아래).
	 * 설정자: top_table 과 함께 한 워드에서 꺼내 온다.
	 * 읽는 자: 순회 시작 단계.
	 * 값 범위: 0 부터 형식이 허용하는 최대 단계까지.
	 * 동기화: top_table 과 같은 읽기에서 나오므로 서로 어긋나지 않는다. */
	u8 max_vasz_lg2;
	/* [한국어] 이 범위가 다룰 수 있는 주소 폭의 지수.
	 * 설정자: 최상위 단계와 형식의 단계별 비트 수에서 계산한다.
	 * 읽는 자: 인자 검증 — 범위가 표의 사정거리를 넘는지 본다.
	 * 값 범위: 형식에 따라 32 또는 최대 64.
	 * 동기화: 호출 스택 값. */
};

/*
 * Similar to xa_state, this records information about an in-progress parse at a
 * single level.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 순회 중 "지금 어느 단계의 어느 항목을 보고 있는가"를 담는다.
 * 원 주석이 xa_state 에 빗대는 이유가 이것이다 — 함수마다 위치를 다시
 * 계산하는 대신 상태를 넘겨 가며 진행한다.
 */
struct pt_state {
	struct pt_range *range;
	/* [한국어] 이 순회가 다루는 범위.
	 * 설정자: 상태를 만들 때 연결한다.
	 * 읽는 자: 현재 VA 와 종료 조건을 여기서 읽는다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 둘 다 호출 스택에 있다. */
	struct pt_table_p *table;
	/* [한국어] 지금 보고 있는 단계의 표.
	 * 설정자: 한 단계 내려갈 때 아래 표 주소로 바뀐다.
	 * 읽는 자: 항목을 읽고 쓰는 형식 접근자.
	 * 값 범위: 표 메모리의 커널 가상 주소.
	 * 동기화: 항목 갱신은 cmpxchg 로, 표 자체의 해제는 드라이버 락으로. */
	struct pt_table_p *table_lower;
	/* [한국어] 방금 내려온 아래 단계의 표.
	 * 설정자: 내려갈 때 기록해 두었다가 올라올 때 쓴다.
	 * 읽는 자: 빈 표를 되돌려 줄 때(unmap 후 정리) 그 주소가 필요하다.
	 * 값 범위: 표 주소 또는 NULL.
	 * 동기화: 호출 스택 값. */
	u64 entry;
	/* [한국어] 현재 항목의 원본 값.
	 * 설정자: pt_load_entry() 가 한 번 읽어 담는다.
	 * 읽는 자: 형식 접근자들이 이 값에서 주소와 속성을 뽑는다.
	 * 값 범위: 형식이 정하는 비트 배치. 폭이 32비트인 형식도 여기에 담긴다.
	 * 동기화: 한 번 읽어 두는 이유가 동기화다 — 여러 번 읽으면 그사이
	 *   다른 CPU 가 바꾼 값을 섞어 쓰게 된다. */
	enum pt_entry_type type;
	/* [한국어] 그 항목이 비었는지, 표인지, 주소인지.
	 * 설정자: pt_load_entry() 가 entry 와 함께 정한다.
	 * 읽는 자: 순회 코드의 분기.
	 * 값 범위: PT_ENTRY_EMPTY/TABLE/OA.
	 * 동기화: entry 와 같은 읽기에서 나온다. */
	unsigned short index;
	/* [한국어] 현재 표에서의 위치.
	 * 설정자: VA 의 해당 비트 조각에서 계산한다.
	 * 읽는 자: 항목 접근과 다음 항목으로의 이동.
	 * 값 범위: 0 부터 표의 항목 수-1.
	 * 동기화: 호출 스택 값. */
	unsigned short end_index;
	/* [한국어] 이 표에서 처리할 마지막 다음 위치.
	 * 설정자: 범위의 끝 주소가 이 표의 어디까지 걸치는지로 정해진다.
	 * 읽는 자: 한 표 안의 순회 종료 판정.
	 * 값 범위: index 이상, 표의 항목 수 이하.
	 * 동기화: 호출 스택 값. */
	u8 level;
	/* [한국어] 지금 있는 단계 번호.
	 * 설정자: 내려가면 줄고 올라오면 는다.
	 * 읽는 자: 단계별 항목 크기와 접근자 선택.
	 * 값 범위: 0 부터 range->top_level 까지.
	 * 동기화: 호출 스택 값. */
};

#define pt_cur_table(pts, type) ((type *)((pts)->table))	/* [한국어] 불투명 표 포인터를 형식이 아는 항목 타입으로 본다 */

/*
 * Try to install a new table pointer. The locking methodology requires this to
 * be atomic (multiple threads can race to install a pointer). The losing
 * threads will fail the atomic and return false. They should free any memory
 * and reparse the table level again.
 */
#if !IS_ENABLED(CONFIG_GENERIC_ATOMIC64)	/* [한국어] 64비트 원자 연산을 하드웨어가 지원할 때만 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * pt_table_install64 - 새 표 포인터를 64비트 항목에 원자적으로 꽂는다
 *
 * @pts: 지금 보고 있는 항목의 위치와 그때 읽은 값.
 * @table_entry: 꽂을 항목 값(아래 표의 주소와 유효 비트).
 * @return: 성공하면 참, 다른 스레드가 먼저 꽂았으면 거짓.
 *
 * 이 계층의 잠금 전략이 이 함수에 압축되어 있다. 표를 만드는 데 락을 쓰지
 * 않고, 여러 스레드가 각자 표를 만들어 경쟁적으로 꽂는다. 진 쪽은 거짓을
 * 받고 자기 표를 버린 뒤 그 단계를 다시 읽는다 — 원 주석이 그 계약을
 * 명시한다.
 *
 * 비교값이 pts->entry 인 것이 요점이다. "내가 읽었을 때 비어 있던 그
 * 상태"에서만 꽂으므로, 그사이 누가 무엇을 넣었으면 실패한다.
 *
 * release 순서가 필요한 이유: 새 표는 0 으로 채워져 있어야 하는데, 그
 * 0 채움이 하드웨어에게 보이기 전에 포인터가 먼저 보이면 하드웨어가
 * 쓰레기 항목을 읽는다.
 *
 * !SMP 에서 dma_wmb 를 따로 넣는 이유: 그 구성에서 release 는 컴파일러
 * 장벽으로 접히지만, 하드웨어는 여전히 자기 순서로 읽는다.
 */
static inline bool pt_table_install64(struct pt_state *pts, u64 table_entry)
{
	u64 *entryp = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 고칠 항목의 실제 주소 */
	u64 old_entry = pts->entry;	/* [한국어] 내가 읽었을 때의 값 — 그 상태에서만 꽂는다 */
	bool ret;	/* [한국어] 성공 여부 */

	/*
	 * Ensure the zero'd table content itself is visible before its PTE can
	 * be. release is a NOP on !SMP, but the HW is still doing an acquire.
	 */
	if (!IS_ENABLED(CONFIG_SMP))	/* [한국어] (원 주석: release 는 !SMP 에서 NOP 이지만 하드웨어는 여전히 acquire 한다) */
		dma_wmb();	/* [한국어] 0 채움이 하드웨어에 보인 뒤에야 포인터가 보이도록 */
	ret = try_cmpxchg64_release(entryp, &old_entry, table_entry);	/* [한국어] 경합에서 지면 거짓 — 진 쪽이 자기 표를 버린다 */
	if (ret)	/* [한국어] 내가 꽂았으면 */
		pts->entry = table_entry;	/* [한국어] 순회 상태도 새 값으로 맞춘다 */
	return ret;	/* [한국어] 성패 */
}
#endif

/*
 * [한국어]
 * pt_table_install32 - 새 표 포인터를 32비트 항목에 원자적으로 꽂는다
 *
 * @pts: 지금 보고 있는 항목의 위치와 그때 읽은 값.
 * @table_entry: 꽂을 항목 값.
 * @return: 성공하면 참, 경합에서 지면 거짓.
 *
 * 64비트판과 논리가 같다. 항목이 32비트인 형식(RISC-V Sv32 등)이 쓴다.
 * 이쪽은 어느 아키텍처에서나 원자 연산이 있어 CONFIG_GENERIC_ATOMIC64
 * 조건이 붙지 않는다.
 */
static inline bool pt_table_install32(struct pt_state *pts, u32 table_entry)
{
	u32 *entryp = pt_cur_table(pts, u32) + pts->index;	/* [한국어] 고칠 항목의 실제 주소 */
	u32 old_entry = pts->entry;	/* [한국어] 읽었을 때의 값 */
	bool ret;	/* [한국어] 성공 여부 */

	/*
	 * Ensure the zero'd table content itself is visible before its PTE can
	 * be. release is a NOP on !SMP, but the HW is still doing an acquire.
	 */
	if (!IS_ENABLED(CONFIG_SMP))	/* [한국어] (원 주석: 같은 이유의 장벽) */
		dma_wmb();	/* [한국어] 표 내용이 먼저 보이도록 */
	ret = try_cmpxchg_release(entryp, &old_entry, table_entry);	/* [한국어] 32비트 원자 연산은 어디에나 있다 */
	if (ret)	/* [한국어] 내가 꽂았으면 */
		pts->entry = table_entry;	/* [한국어] 순회 상태를 맞춘다 */
	return ret;	/* [한국어] 성패 */
}

#define PT_SUPPORTED_FEATURE(feature_nr) (PT_SUPPORTED_FEATURES & BIT(feature_nr))	/* [한국어] 컴파일 시 상수 — 안 쓰는 기능의 코드가 통째로 사라진다 */

/*
 * [한국어]
 * pt_feature - 이 인스턴스에서 그 기능이 켜져 있는지 답한다
 *
 * @common: 페이지 테이블 인스턴스.
 * @feature_nr: 물어볼 기능 번호.
 * @return: 켜져 있으면 참.
 *
 * 세 단계로 판정한다. 형식이 강제로 켜는 기능이면 무조건 참, 형식이 아예
 * 지원하지 않으면 무조건 거짓, 그 밖에는 인스턴스가 요청했는지를 본다.
 *
 * 앞 두 판정이 컴파일 시 상수라는 점이 중요하다. __always_inline 과 맞물려
 * 쓰지 않는 기능의 코드가 통째로 사라진다 — 형식마다 전용 코드를 찍어 내는
 * 이 계층의 설계가 여기서 이득을 낸다.
 */
static __always_inline bool pt_feature(const struct pt_common *common,
			      unsigned int feature_nr)
{
	if (PT_FORCE_ENABLED_FEATURES & BIT(feature_nr))	/* [한국어] 형식이 강제로 켜는 기능이면 */
		return true;	/* [한국어] 인스턴스의 요청과 무관하게 참 */
	if (!PT_SUPPORTED_FEATURE(feature_nr))	/* [한국어] 형식이 아예 지원하지 않으면 */
		return false;	/* [한국어] 상수 거짓 — 그 기능의 코드가 통째로 사라진다 */
	return common->features & BIT(feature_nr);	/* [한국어] 그 밖에는 인스턴스가 요청했는지 */
}

/*
 * [한국어]
 * pts_feature - 순회 상태에서 기능 여부를 묻는다
 *
 * @pts: 순회 상태.
 * @feature_nr: 물어볼 기능 번호.
 * @return: 켜져 있으면 참.
 *
 * 순회 중에는 pt_common 을 두 번 거쳐 가야 하므로 그 경로를 감싼 껍질이다.
 */
static __always_inline bool pts_feature(const struct pt_state *pts,
			       unsigned int feature_nr)
{
	return pt_feature(pts->range->common, feature_nr);	/* [한국어] 순회 상태에서 인스턴스까지 두 번 거친다 */
}

/*
 * PT_WARN_ON is used for invariants that the kunit should be checking can't
 * happen.
 */
#if IS_ENABLED(CONFIG_DEBUG_GENERIC_PT)	/* [한국어] (원 주석: kunit 이 일어날 수 없다고 확인해야 하는 불변식에 쓴다) */
#define PT_WARN_ON WARN_ON	/* [한국어] 디버그 빌드에서는 실제로 경고한다 */
#else
/*
 * [한국어]
 * PT_WARN_ON - 디버그가 아닐 때의 빈 구현
 *
 * @condition: 무시된다.
 * @return: 항상 거짓.
 *
 * 이 매크로가 확인하는 것은 "kunit 이 일어날 수 없다고 검증한 불변식"이라,
 * 운영 커널에서는 검사 자체를 없앤다. 함수로 두는 이유: 인자 식이 여전히
 * 문법 검사를 받고, 조건에 부작용이 있으면 컴파일러가 경고한다.
 */
static inline bool PT_WARN_ON(bool condition)
{
	return false;	/* [한국어] 운영 커널에서는 검사 자체를 없앤다 */
}
#endif

/* These all work on the VA type */
#define log2_to_int(a_lg2) log2_to_int_t(pt_vaddr_t, a_lg2)	/* [한국어] (원 주석: 이 아래는 모두 VA 타입에 대해 동작한다) 타입을 고정한 별칭 */
#define log2_to_max_int(a_lg2) log2_to_max_int_t(pt_vaddr_t, a_lg2)	/* [한국어] VA 용 하위 비트 마스크 */
#define log2_div(a, b_lg2) log2_div_t(pt_vaddr_t, a, b_lg2)	/* [한국어] VA 용 나눗셈 */
#define log2_div_eq(a, b, c_lg2) log2_div_eq_t(pt_vaddr_t, a, b, c_lg2)	/* [한국어] 두 VA 가 같은 블록에 있는가 */
#define log2_mod(a, b_lg2) log2_mod_t(pt_vaddr_t, a, b_lg2)	/* [한국어] VA 용 나머지 */
#define log2_mod_eq_max(a, b_lg2) log2_mod_eq_max_t(pt_vaddr_t, a, b_lg2)	/* [한국어] VA 가 블록의 마지막 바이트인가 */
#define log2_set_mod(a, val, b_lg2) log2_set_mod_t(pt_vaddr_t, a, val, b_lg2)	/* [한국어] VA 의 하위 비트를 val 로 */
#define log2_set_mod_max(a, b_lg2) log2_set_mod_max_t(pt_vaddr_t, a, b_lg2)	/* [한국어] VA 를 그 블록의 끝 주소로 */
#define log2_mul(a, b_lg2) log2_mul_t(pt_vaddr_t, a, b_lg2)	/* [한국어] VA 용 곱셈 */
#define vaffs(a) ffs_t(pt_vaddr_t, a)	/* [한국어] VA 의 정렬을 잰다 */
#define vafls(a) fls_t(pt_vaddr_t, a)	/* [한국어] VA 에 필요한 비트 수 */
#define vaffz(a) ffz_t(pt_vaddr_t, a)	/* [한국어] VA 하위의 연속된 1 의 길이 */

/*
 * The full VA (fva) versions permit the lg2 value to be == PT_VADDR_MAX_LG2 and
 * generate a useful defined result. The non-fva versions will malfunction at
 * this extreme.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * fvalog2_div - 전 주소 공간에서도 안전한 나눗셈
 *
 * @a: 나눌 값.
 * @b_lg2: 나눌 크기의 지수.
 * @return: a / 2^b_lg2.
 *
 * 지수가 주소 폭과 같아지는 경우가 문제다. 64비트 값을 64비트 시프트하는
 * 것은 C 에서 미정의 동작이고, 실제 x86 은 시프트 양을 63 으로 마스크해
 * 엉뚱한 답을 낸다.
 *
 * 그런 경우가 생기는 이유는 FULL_VA 형식이 "주소 공간 전체"를 한 블록으로
 * 다루기 때문이다. 그 블록의 크기가 곧 2^64 다.
 *
 * 그래서 그 극단만 따로 답한다: 전 공간으로 나눈 몫은 0 이다.
 */
static inline pt_vaddr_t fvalog2_div(pt_vaddr_t a, unsigned int b_lg2)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_FULL_VA) && b_lg2 == PT_VADDR_MAX_LG2)	/* [한국어] 전 주소 공간으로 나누는 극단 */
		return 0;	/* [한국어] 64비트 시프트는 미정의라 답을 직접 준다 */
	return log2_div_t(pt_vaddr_t, a, b_lg2);	/* [한국어] 그 밖에는 보통 시프트 */
}

/*
 * [한국어]
 * fvalog2_mod - 전 주소 공간에서도 안전한 나머지
 *
 * @a: 값.
 * @b_lg2: 나눌 크기의 지수.
 * @return: a % 2^b_lg2.
 *
 * 전 공간으로 나눈 나머지는 값 자체다.
 */
static inline pt_vaddr_t fvalog2_mod(pt_vaddr_t a, unsigned int b_lg2)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_FULL_VA) && b_lg2 == PT_VADDR_MAX_LG2)	/* [한국어] 전 공간으로 나누는 극단 */
		return a;	/* [한국어] 나머지는 값 자체다 */
	return log2_mod_t(pt_vaddr_t, a, b_lg2);	/* [한국어] 그 밖에는 마스크 */
}

/*
 * [한국어]
 * fvalog2_div_eq - 두 주소가 같은 블록에 있는지, 전 공간에서도 안전하게
 *
 * @a: 첫 주소.
 * @b: 둘째 주소.
 * @c_lg2: 블록 크기의 지수.
 * @return: 같은 블록이면 참.
 *
 * 블록이 주소 공간 전체면 어떤 두 주소든 같은 블록에 있다.
 */
static inline bool fvalog2_div_eq(pt_vaddr_t a, pt_vaddr_t b,
				  unsigned int c_lg2)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_FULL_VA) && c_lg2 == PT_VADDR_MAX_LG2)	/* [한국어] 블록이 주소 공간 전체면 */
		return true;	/* [한국어] 어떤 두 주소든 같은 블록에 있다 */
	return log2_div_eq_t(pt_vaddr_t, a, b, c_lg2);	/* [한국어] 그 밖에는 XOR 로 */
}

/*
 * [한국어]
 * fvalog2_set_mod - 하위 비트를 val 로, 전 공간에서도 안전하게
 *
 * @a: 원래 값.
 * @val: 넣을 하위 값.
 * @b_lg2: 하위 비트 수의 지수.
 * @return: 상위는 a, 하위는 val.
 *
 * 전 공간이면 상위 비트가 하나도 없으므로 결과는 val 자체다.
 */
static inline pt_vaddr_t fvalog2_set_mod(pt_vaddr_t a, pt_vaddr_t val,
					 unsigned int b_lg2)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_FULL_VA) && b_lg2 == PT_VADDR_MAX_LG2)	/* [한국어] 하위가 전부인 극단 */
		return val;	/* [한국어] 남길 상위 비트가 없다 */
	return log2_set_mod_t(pt_vaddr_t, a, val, b_lg2);	/* [한국어] 그 밖에는 마스크와 OR */
}

/*
 * [한국어]
 * fvalog2_set_mod_max - 그 블록의 끝 주소를 구한다, 전 공간에서도 안전하게
 *
 * @a: 블록 안의 아무 주소.
 * @b_lg2: 블록 크기의 지수.
 * @return: 그 블록의 마지막 주소.
 *
 * 전 공간의 끝은 주소 최대값이다. 순회의 종료 주소를 구할 때 쓰이며,
 * 이 처리가 없으면 전 공간을 도는 순회가 끝나지 않는다.
 */
static inline pt_vaddr_t fvalog2_set_mod_max(pt_vaddr_t a, unsigned int b_lg2)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_FULL_VA) && b_lg2 == PT_VADDR_MAX_LG2)	/* [한국어] 블록이 주소 공간 전체면 */
		return PT_VADDR_MAX;	/* [한국어] 그 끝은 주소 최대값이다 — 없으면 순회가 끝나지 않는다 */
	return log2_set_mod_max_t(pt_vaddr_t, a, b_lg2);	/* [한국어] 그 밖에는 하위 비트를 모두 세운다 */
}

/* These all work on the OA type */
#define oalog2_to_int(a_lg2) log2_to_int_t(pt_oaddr_t, a_lg2)	/* [한국어] (원 주석: 이 아래는 OA 타입에 대해 동작한다) */
#define oalog2_to_max_int(a_lg2) log2_to_max_int_t(pt_oaddr_t, a_lg2)	/* [한국어] OA 용 하위 비트 마스크 */
#define oalog2_div(a, b_lg2) log2_div_t(pt_oaddr_t, a, b_lg2)	/* [한국어] OA 용 나눗셈 */
#define oalog2_div_eq(a, b, c_lg2) log2_div_eq_t(pt_oaddr_t, a, b, c_lg2)	/* [한국어] 두 OA 가 같은 블록에 있는가 */
#define oalog2_mod(a, b_lg2) log2_mod_t(pt_oaddr_t, a, b_lg2)	/* [한국어] OA 용 나머지 */
#define oalog2_mod_eq_max(a, b_lg2) log2_mod_eq_max_t(pt_oaddr_t, a, b_lg2)	/* [한국어] OA 가 블록의 마지막인가 */
#define oalog2_set_mod(a, val, b_lg2) log2_set_mod_t(pt_oaddr_t, a, val, b_lg2)	/* [한국어] OA 의 하위 비트를 val 로 */
#define oalog2_set_mod_max(a, b_lg2) log2_set_mod_max_t(pt_oaddr_t, a, b_lg2)	/* [한국어] OA 를 그 블록의 끝으로 */
#define oalog2_mul(a, b_lg2) log2_mul_t(pt_oaddr_t, a, b_lg2)	/* [한국어] OA 용 곱셈 */
#define oaffs(a) ffs_t(pt_oaddr_t, a)	/* [한국어] OA 의 정렬을 잰다 */
#define oafls(a) fls_t(pt_oaddr_t, a)	/* [한국어] OA 에 필요한 비트 수 */
#define oaffz(a) ffz_t(pt_oaddr_t, a)	/* [한국어] OA 하위의 연속된 1 의 길이 */

/*
 * [한국어]
 * _pt_top_set - 최상위 표 주소와 단계 번호를 한 워드에 합친다
 *
 * @table_mem: 최상위 표의 주소.
 * @top_level: 그 단계 번호.
 * @return: 둘을 합친 값.
 *
 * 표는 페이지 정렬이라 하위 비트가 늘 0 이다. 그 빈자리에 단계 번호를
 * 넣으면 둘을 한 번의 원자적 읽기·쓰기로 다룰 수 있다.
 *
 * 그것이 필요한 이유가 DYNAMIC_TOP 이다. 주소 공간이 커져 단계를 하나
 * 얹을 때 주소와 단계가 함께 바뀌어야 하는데, 따로 쓰면 그 사이를 본
 * 다른 CPU 가 옛 주소를 새 단계로 해석한다.
 */
static inline uintptr_t _pt_top_set(struct pt_table_p *table_mem,
				    unsigned int top_level)
{
	return top_level | (uintptr_t)table_mem;	/* [한국어] 표는 페이지 정렬이라 하위 비트가 비어 있다 */
}

/*
 * [한국어]
 * pt_top_set - 최상위 표를 바꾼다
 *
 * @common: 페이지 테이블 인스턴스.
 * @table_mem: 새 최상위 표.
 * @top_level: 그 단계 번호.
 *
 * WRITE_ONCE 로 한 번에 쓴다 — 읽는 쪽이 찢어진 값을 보지 않게 한다.
 */
static inline void pt_top_set(struct pt_common *common,
			      struct pt_table_p *table_mem,
			      unsigned int top_level)
{
	WRITE_ONCE(common->top_of_table, _pt_top_set(table_mem, top_level));	/* [한국어] 주소와 단계를 한 번에 — 읽는 쪽이 찢어진 값을 보지 않는다 */
}

/*
 * [한국어]
 * pt_top_set_level - 표 없이 단계 번호만 기록한다
 *
 * @common: 페이지 테이블 인스턴스.
 * @top_level: 단계 번호.
 *
 * 초기화 중 표를 아직 만들지 않았을 때 쓴다. 시작 단계를 먼저 정해 두면
 * 첫 매핑이 그 단계에 맞는 표를 만든다.
 */
static inline void pt_top_set_level(struct pt_common *common,
				    unsigned int top_level)
{
	pt_top_set(common, NULL, top_level);	/* [한국어] 표는 첫 매핑이 만든다 */
}

/*
 * [한국어]
 * pt_top_get_level - 현재 최상위 단계를 읽는다
 *
 * @common: 페이지 테이블 인스턴스.
 * @return: 단계 번호.
 *
 * 합쳐 둔 워드의 하위 비트만 꺼낸다. READ_ONCE 인 이유: 다른 CPU 가 단계를
 * 올리는 중일 수 있어 컴파일러가 값을 다시 읽거나 쪼개면 안 된다.
 */
static inline unsigned int pt_top_get_level(const struct pt_common *common)
{
	return READ_ONCE(common->top_of_table) % (1 << PT_TOP_LEVEL_BITS);	/* [한국어] 합쳐 둔 워드의 하위 비트만 */
}

/*
 * [한국어]
 * pt_check_install_leaf_args - 잎 항목 인자의 전방 선언
 *
 * @pts: 항목 위치.
 * @oa: 넣을 출력 주소.
 * @oasz_lg2: 그 항목이 덮는 크기의 지수.
 * @return: 인자가 형식의 규칙에 맞으면 참.
 *
 * 정의는 형식 헤더에 있다. 여기 선언만 두는 이유는 포함 순서다 —
 * pt_common.h 의 인라인 코드가 형식 헤더보다 먼저 이 이름을 봐야 한다.
 */
static inline bool pt_check_install_leaf_args(struct pt_state *pts,
					      pt_oaddr_t oa,
					      unsigned int oasz_lg2);

#endif	/* [한국어] 포함 방지 끝 */
