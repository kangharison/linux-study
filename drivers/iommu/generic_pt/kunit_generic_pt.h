/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * Test the format API directly.
 *
 */
/*
 * [한국어 설명] 형식 API 자체를 직접 시험하는 kunit (kunit_generic_pt.h)
 *
 * === 파일의 역할 ===
 * IOMMU 진입점을 거치지 않고 형식이 제공한 함수들을 직접 부른다. 항목을
 * 쓰고 되읽어 같은 값이 나오는가, 크기 계산이 맞는가, 소프트웨어 비트가
 * 다른 필드를 건드리지 않는가 — 그런 낮은 수준의 계약을 확인한다.
 *
 * 이 파일의 골격은 check_all_levels 다. 표의 모든 단계에 시험 함수를 한
 * 번씩 적용해 주므로, 각 시험은 단계를 신경 쓰지 않고 "지금 이 단계에서"
 * 무엇을 확인할지만 쓰면 된다.
 *
 * 그것을 가능하게 하는 준비가 흥미롭다. 가장 높은 VA 에 페이지 하나를
 * 매핑하면 최상위부터 0단계까지 표가 모두 만들어지고, 그 뒤로는 각 단계의
 * 0번(또는 1번) 항목을 시험에 쓸 수 있다.
 *
 * 참조 구현과 견주는 시험도 있다. ref_best_pgsize 는 pt_compute_best_pgsize
 * 의 비트 연산을 무식한 반복문으로 다시 쓴 것이고, 무작위 입력으로 둘을
 * 맞대어 본다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c 를 GENERIC_PT_KUNIT 을 켜고 컴파일할 때 포함된다.
 * 그래서 형식마다 이 시험 묶음이 하나씩 생기고, 형식이 설정 목록을 주면
 * 그 각각에 대해 다시 돈다.
 *
 * 실행 컨텍스트: kunit 모듈, 프로세스 문맥.
 *
 * === 타 모듈과의 연결 ===
 * 위: kunit 프레임워크가 suite 를 돌린다.
 * 아래: kunit_iommu.h(시험 환경), pt_iter.h(순회), 형식 헤더의 접근자들.
 *
 * === 주요 함수/구조체 요약 ===
 * check_all_levels: 모든 단계에 시험 함수를 적용하는 골격.
 * test_bitops: pt_log2.h 의 산술이 정수 한계에서도 맞는지.
 * test_best_pgsize: 페이지 크기 선택을 참조 구현과 맞대어 본다.
 * test_lvl_table_ptr / test_lvl_entry_oa: 쓴 값을 되읽어 같은지.
 * test_lvl_radix: 단계들이 VA 비트를 빈틈없이 나눠 갖는지.
 * test_lvl_possible_sizes: 크기 비트맵이 규약대로 생겼는지.
 * test_lvl_attr_from_entry: 권한을 꺼냈다 다시 넣으면 같은 항목이 되는지.
 * test_lvl_dirty / test_lvl_sw_bit_*: 더티 비트와 소프트웨어 비트가
 *   서로의 자리를 침범하지 않는지.
 */
#include "kunit_iommu.h"	/* [한국어] 가짜 IOMMU 환경 */
#include "pt_iter.h"	/* [한국어] 순회기와 형식 API */

/*
 * [한국어]
 * do_map - 시험용 매핑을 하나 만든다
 *
 * @test: 시험 문맥.
 * @va: 매핑할 가상 주소.
 * @pa: 대응하는 물리 주소.
 * @len: 길이.
 *
 * 코어의 iommu_map 을 그대로 부른다. 실패하면 그 자리에서 시험을 세운다 —
 * 이후 시험이 그 매핑을 전제로 하기 때문이다.
 *
 * 앞의 단언은 32비트 환경에서 길이가 size_t 에 담기는지 확인한다.
 */
static void do_map(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		   pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	int ret;	/* [한국어] 결과 */

	KUNIT_ASSERT_EQ(test, len, (size_t)len);	/* [한국어] 32비트에서 길이가 size_t 에 담기는지 */

	ret = iommu_map(&priv->domain, va, pa, len, IOMMU_READ | IOMMU_WRITE,	/* [한국어] 코어의 매핑 경로를 그대로 탄다 */
			GFP_KERNEL);	/* [한국어] 프로세스 문맥 */
	KUNIT_ASSERT_NO_ERRNO_FN(test, "map_pages", ret);	/* [한국어] 이후 시험이 이 매핑을 전제로 한다 */
}

/*
 * [한국어] 항목을 읽고 기대한 종류인지 한 번에 확인하는 단언.
 * 읽기와 비교를 늘 짝으로 하므로 매크로로 묶어 두었다.
 */
#define KUNIT_ASSERT_PT_LOAD(test, pts, entry)             \
	({                                                 \
		pt_load_entry(pts);                        \
		KUNIT_ASSERT_EQ(test, (pts)->type, entry); \
	})	/* [한국어] 읽은 종류를 기대값과 비교한다 */

struct check_levels_arg {	/* [한국어] 단계마다 부를 함수와 그 인자를 나르는 묶음 */
	struct kunit *test;
	/* [한국어] 시험 문맥.
	 * 설정자: check_all_levels 가 채운다.
	 * 읽는 자: 워커가 단언을 낼 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 호출 스택에 있어 공유되지 않는다. */
	void *fn_arg;
	/* [한국어] 단계별 함수에 그대로 넘길 값.
	 * 설정자: check_all_levels 의 호출자.
	 * 읽는 자: 그 함수. test_lvl_radix 는 여기로 누적 비트를 주고받는다.
	 * 값 범위: 함수마다 다르다. NULL 일 수 있다.
	 * 동기화: 호출 스택 값. */
	void (*fn)(struct kunit *test, struct pt_state *pts, void *arg);
	/* [한국어] 각 단계에서 부를 시험 함수.
	 * 설정자: check_all_levels 의 호출자.
	 * 읽는 자: __check_all_levels 가 단계마다 한 번씩 부른다.
	 * 값 범위: test_lvl_* 함수들 중 하나.
	 * 동기화: 호출 스택 값. */
};

/*
 * [한국어]
 * __check_all_levels - 각 단계에서 시험 함수를 부르는 워커
 *
 * @range: 걷는 범위.
 * @arg: 시험 문맥과 부를 함수.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 늘 0(실패는 단언이 세운다).
 *
 * 내려가면서 각 단계의 구조를 검증하고, 맨 마지막에 시험 함수를 부른다.
 *
 * 앞의 단언들이 확인하는 것: 최고 VA 에 매핑했으므로 각 표에서 지금 보고
 * 있는 자리는 반드시 마지막 색인이어야 한다. 부호 확장 형식의 최상위만
 * 절반 지점이라 따로 계산한다.
 *
 * 32비트 환경을 건너뛰는 이유: 그쪽에서는 VA 를 32비트로 잘랐으므로 마지막
 * 색인에 닿지 못한다.
 *
 * 시험에 쓸 자리를 고르는 방식도 조건이 붙는다. 원 주석대로 최상위가 자란
 * 경우 0번은 원래 트리가 쓰고 있어, 그럴 때는 1번을 쓴다.
 */
static int __check_all_levels(struct pt_range *range, void *arg,
			      unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct check_levels_arg *chk = arg;	/* [한국어] 시험 문맥과 부를 함수 */
	struct kunit *test = chk->test;	/* [한국어] 단언에 쓴다 */
	int ret;	/* [한국어] 아래 단계의 결과 */

	_pt_iter_first(&pts);	/* [한국어] 색인 구간을 잡는다 */


	/*
	 * If we were able to use the full VA space this should always be the
	 * last index in each table.
	 */
	if (!(IS_32BIT && range->max_vasz_lg2 > 32)) {	/* [한국어] (원 주석: 전 VA 공간을 쓸 수 있었다면 각 표에서 늘 마지막 색인이어야 한다) */
		if (pt_feature(range->common, PT_FEAT_SIGN_EXTEND) &&	/* [한국어] 부호 확장 형식의 */
		    pts.level == pts.range->top_level)	/* [한국어] 최상위만 절반 지점이라 */
			KUNIT_ASSERT_EQ(test, pts.index,	/* [한국어] 폭에서 1 을 빼고 */
					log2_to_int(range->max_vasz_lg2 - 1 -	/* [한국어] 계산해야 한다 */
						    pt_table_item_lg2sz(&pts)) -	/* [한국어] 항목 크기로 나눈 뒤 */
						1);	/* [한국어] 마지막 색인 */
		else
			KUNIT_ASSERT_EQ(test, pts.index,	/* [한국어] 그 밖에는 */
					log2_to_int(pt_table_oa_lg2sz(&pts) -	/* [한국어] 표가 덮는 범위를 */
						    pt_table_item_lg2sz(&pts)) -	/* [한국어] 항목 크기로 나눈 */
						1);	/* [한국어] 마지막 색인 */
	}

	if (pt_can_have_table(&pts)) {	/* [한국어] 아래 단계가 있으면 */
		pt_load_single_entry(&pts);	/* [한국어] 그 항목을 읽어 */
		KUNIT_ASSERT_EQ(test, pts.type, PT_ENTRY_TABLE);	/* [한국어] 매핑이 만든 표가 있어야 한다 */
		ret = pt_descend(&pts, arg, __check_all_levels);	/* [한국어] 아래 단계부터 먼저 시험한다 */
		KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 실패하면 여기서 세운다 */

		/* Index 0 is used by the test */
		if (IS_32BIT && !pts.index)	/* [한국어] (원 주석: 색인 0 은 시험이 쓴다) */
			return 0;	/* [한국어] 32비트에서는 0 에 닿을 수 있어 넘어간다 */
		KUNIT_ASSERT_NE(chk->test, pts.index, 0);	/* [한국어] 그 밖에는 0 이 아니어야 한다 */
	}

	/*
	 * A format should not create a table with only one entry, at least this
	 * test approach won't work.
	 */
	KUNIT_ASSERT_GT(chk->test, pts.end_index, 1);	/* [한국어] (원 주석: 항목이 하나뿐인 표를 만드는 형식은 이 방식으로 시험할 수 없다) */

	/*
	 * For increase top we end up using index 0 for the original top's tree,
	 * so use index 1 for testing instead.
	 */
	pts.index = 0;	/* [한국어] (원 주석: 최상위가 자란 경우 0 번은 원래 트리가 쓰므로 1 번을 쓴다) */
	pt_index_to_va(&pts);	/* [한국어] VA 를 맞추고 */
	pt_load_single_entry(&pts);	/* [한국어] 그 자리를 본다 */
	if (pts.type == PT_ENTRY_TABLE && pts.end_index > 2) {	/* [한국어] 이미 표가 있고 여유가 있으면 */
		pts.index = 1;	/* [한국어] 한 칸 옆으로 */
		pt_index_to_va(&pts);	/* [한국어] VA 도 맞춘다 */
	}
	(*chk->fn)(chk->test, &pts, chk->fn_arg);	/* [한국어] 이 단계의 시험을 부른다 */
	return 0;	/* [한국어] 실패는 단언이 세운다 */
}

/*
 * Call fn for each level in the table with a pts setup to index 0 in a table
 * for that level. This allows writing tests that run on every level.
 * The test can use every index in the table except the last one.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * check_all_levels - 모든 단계에 시험 함수를 적용한다
 *
 * @test: 시험 문맥.
 * @fn: 각 단계에서 부를 함수.
 * @fn_arg: 그 함수에 넘길 값.
 *
 * 이 파일의 골격이다. 먼저 가장 높은 VA 에 작은 페이지 하나를 매핑해 모든
 * 단계의 표를 만들고, 그 경로를 따라 내려가며 시험 함수를 부른다.
 *
 * 원 주석이 계약을 밝힌다 — 시험 함수는 각 표의 마지막 자리를 뺀 모든
 * 색인을 마음대로 쓸 수 있다. 마지막 자리는 이 매핑이 쓰고 있기 때문이다.
 */
static void check_all_levels(struct kunit *test,
			     void (*fn)(struct kunit *test,
					struct pt_state *pts, void *arg),
			     void *fn_arg)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 현재 최상위 범위 */
	struct check_levels_arg chk = {	/* [한국어] 워커에 넘길 묶음 */
		.test = test,	/* [한국어] 시험 문맥 */
		.fn = fn,	/* [한국어] 각 단계에서 부를 함수 */
		.fn_arg = fn_arg,	/* [한국어] 그 함수에 넘길 값 */
	};
	int ret;	/* [한국어] 결과 */

	if (pt_feature(priv->common, PT_FEAT_DYNAMIC_TOP) &&	/* [한국어] 자랄 수 있는 형식이고 */
	    priv->common->max_vasz_lg2 > range.max_vasz_lg2)	/* [한국어] 아직 다 자라지 않았으면 */
		range.last_va = fvalog2_set_mod_max(range.va,	/* [한국어] 다 자란 뒤의 끝까지 */
						    priv->common->max_vasz_lg2);	/* [한국어] 매핑해 최상위를 키운다 */

	/*
	 * Map a page at the highest VA, this will populate all the levels so we
	 * can then iterate over them. Index 0 will be used for testing.
	 */
	if (IS_32BIT && range.max_vasz_lg2 > 32)	/* [한국어] (원 주석: 최고 VA 에 페이지를 매핑해 모든 단계를 만들고 색인 0 을 시험에 쓴다) */
		range.last_va = (u32)range.last_va;	/* [한국어] 32비트에서는 그 폭 안으로 자른다 */
	range.va = range.last_va - (priv->smallest_pgsz - 1);	/* [한국어] 가장 작은 페이지 하나만큼 */
	do_map(test, range.va, 0, priv->smallest_pgsz);	/* [한국어] 매핑하면 최상위부터 0단계까지 표가 생긴다 */

	range = pt_make_range(priv->common, range.va, range.last_va);	/* [한국어] 자란 뒤의 최상위를 다시 읽는다 */
	ret = pt_walk_range(&range, __check_all_levels, &chk);	/* [한국어] 그 경로를 따라 내려가며 시험한다 */
	KUNIT_ASSERT_EQ(test, ret, 0);	/* [한국어] 워커는 실패하지 않아야 한다 */
}

/*
 * [한국어]
 * test_init - 환경이 제대로 세워졌는지만 확인한다
 *
 * @test: 시험 문맥.
 *
 * 실제 준비는 fixture(pt_kunit_priv_init)가 하고, 여기서는 그 결과가
 * 쓸 만한지 — 지원 페이지 크기가 하나라도 있는지 — 만 본다.
 */
static void test_init(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] fixture 가 만든 환경 */

	/* Fixture does the setup */
	KUNIT_ASSERT_NE(test, priv->info.pgsize_bitmap, 0);	/* [한국어] (원 주석: 준비는 fixture 가 한다) 쓸 수 있는 페이지 크기가 있어야 한다 */
}

/*
 * Basic check that the log2_* functions are working, especially at the integer
 * limits.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_bitops - pt_log2.h 의 산술이 정수 한계에서도 맞는지 본다
 *
 * @test: 시험 문맥.
 *
 * 이 계층의 모든 계산이 이 매크로들 위에 서 있어, 여기가 틀리면 모든 것이
 * 조용히 틀린다. 그래서 0, 1, 최대값 같은 경계를 하나씩 짚는다.
 *
 * 뒤쪽의 무작위 시험은 두 성질을 확인한다: 값을 자기 정렬로 나눈 나머지는
 * 0 이고, 하위의 연속된 1 만큼 나눈 나머지는 그 자리가 모두 1 이다.
 */
static void test_bitops(struct kunit *test)
{
	int i;	/* [한국어] 반복 인덱스 */

	KUNIT_ASSERT_EQ(test, fls_t(u32, 0), 0);	/* [한국어] 선 비트가 없으면 0 */
	KUNIT_ASSERT_EQ(test, fls_t(u32, 1), 1);	/* [한국어] 1 기반이라 최하위 비트가 1 */
	KUNIT_ASSERT_EQ(test, fls_t(u32, BIT(2)), 3);	/* [한국어] 2번 비트면 3 */
	KUNIT_ASSERT_EQ(test, fls_t(u32, U32_MAX), 32);	/* [한국어] 32비트 경계 */

	KUNIT_ASSERT_EQ(test, fls_t(u64, 0), 0);	/* [한국어] 64비트 판도 같은 규칙 */
	KUNIT_ASSERT_EQ(test, fls_t(u64, 1), 1);	/* [한국어] 최하위 비트 */
	KUNIT_ASSERT_EQ(test, fls_t(u64, BIT(2)), 3);	/* [한국어] 2번 비트 */
	KUNIT_ASSERT_EQ(test, fls_t(u64, U64_MAX), 64);	/* [한국어] 64비트 경계 — 여기서 폭 분배가 틀리면 드러난다 */

	KUNIT_ASSERT_EQ(test, ffs_t(u32, 1), 0);	/* [한국어] ffs 는 0 기반 */
	KUNIT_ASSERT_EQ(test, ffs_t(u32, BIT(2)), 2);	/* [한국어] 2번 비트면 2 */
	KUNIT_ASSERT_EQ(test, ffs_t(u32, BIT(31)), 31);	/* [한국어] 32비트의 최상위 */

	KUNIT_ASSERT_EQ(test, ffs_t(u64, 1), 0);	/* [한국어] 64비트 판 */
	KUNIT_ASSERT_EQ(test, ffs_t(u64, BIT(2)), 2);	/* [한국어] 2번 비트 */
	KUNIT_ASSERT_EQ(test, ffs_t(u64, BIT_ULL(63)), 63);	/* [한국어] 64비트의 최상위 */

	for (i = 0; i != 31; i++)	/* [한국어] 하위 i 비트가 모두 1 인 값이면 */
		KUNIT_ASSERT_EQ(test, ffz_t(u64, BIT_ULL(i) - 1), i);	/* [한국어] 처음 0 인 자리가 i 다 */

	for (i = 0; i != 63; i++)	/* [한국어] 64비트 범위까지 */
		KUNIT_ASSERT_EQ(test, ffz_t(u64, BIT_ULL(i) - 1), i);	/* [한국어] 같은 성질 */

	for (i = 0; i != 32; i++) {	/* [한국어] 무작위 값으로 두 성질을 확인한다 */
		u64 val = get_random_u64();	/* [한국어] 임의의 값 */

		KUNIT_ASSERT_EQ(test, log2_mod_t(u32, val, ffs_t(u32, val)), 0);	/* [한국어] 자기 정렬로 나눈 나머지는 0 */
		KUNIT_ASSERT_EQ(test, log2_mod_t(u64, val, ffs_t(u64, val)), 0);	/* [한국어] 64비트도 마찬가지 */

		KUNIT_ASSERT_EQ(test, log2_mod_t(u32, val, ffz_t(u32, val)),	/* [한국어] 하위의 연속된 1 만큼 나눈 나머지는 */
				log2_to_max_int_t(u32, ffz_t(u32, val)));	/* [한국어] 그 자리가 모두 1 이다 */
		KUNIT_ASSERT_EQ(test, log2_mod_t(u64, val, ffz_t(u64, val)),	/* [한국어] 64비트도 */
				log2_to_max_int_t(u64, ffz_t(u64, val)));	/* [한국어] 같은 성질 */
	}
}

/*
 * [한국어]
 * ref_best_pgsize - 페이지 크기 선택의 참조 구현
 *
 * @pgsz_bitmap: 허용 크기들.
 * @va: 시작 가상 주소.
 * @last_va: 마지막 가상 주소.
 * @oa: 시작 출력 주소.
 * @return: 쓸 수 있는 가장 큰 크기의 지수, 없으면 0.
 *
 * 원 주석대로 pt_compute_best_pgsize 가 비트 연산으로 한 번에 푸는 제약을
 * 여기서는 큰 크기부터 하나씩 대입해 확인한다. 느리지만 읽으면 곧바로
 * 맞는지 알 수 있는 구현이라, 최적화된 쪽의 정답 노릇을 한다.
 */
static unsigned int ref_best_pgsize(pt_vaddr_t pgsz_bitmap, pt_vaddr_t va,
				    pt_vaddr_t last_va, pt_oaddr_t oa)
{
	pt_vaddr_t pgsz_lg2;	/* [한국어] 시도할 크기 */

	/* Brute force the constraints described in pt_compute_best_pgsize() */
	for (pgsz_lg2 = PT_VADDR_MAX_LG2 - 1; pgsz_lg2 != 0; pgsz_lg2--) {	/* [한국어] (원 주석: pt_compute_best_pgsize 의 제약을 무식하게 대입해 본다) */
		if ((pgsz_bitmap & log2_to_int(pgsz_lg2)) &&	/* [한국어] 허용 목록에 있고 */
		    log2_mod(va, pgsz_lg2) == 0 &&	/* [한국어] VA 가 정렬되고 */
		    oalog2_mod(oa, pgsz_lg2) == 0 &&	/* [한국어] OA 도 정렬되고 */
		    va + log2_to_int(pgsz_lg2) - 1 <= last_va &&	/* [한국어] 남은 길이를 넘지 않고 */
		    log2_div_eq(va, va + log2_to_int(pgsz_lg2) - 1, pgsz_lg2) &&	/* [한국어] VA 가 한 페이지 안에 들고 */
		    oalog2_div_eq(oa, oa + log2_to_int(pgsz_lg2) - 1, pgsz_lg2))	/* [한국어] OA 도 그러면 */
			return pgsz_lg2;	/* [한국어] 큰 쪽부터 보므로 이것이 최선이다 */
	}
	return 0;	/* [한국어] 쓸 수 있는 크기가 없다 */
}

/* Check that the bit logic in pt_compute_best_pgsize() works. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_best_pgsize - 크기 선택의 비트 연산이 참조 구현과 같은지 본다
 *
 * @test: 시험 문맥.
 *
 * 네 가지 입력 부류를 돈다.
 *  - 무작위 정렬을 가진 임의의 주소들.
 *  - 접두가 0 인 경우 — 시프트가 미정의가 되기 쉬운 극단이다.
 *  - 접두가 전부 1 인 경우 — 부호 확장 형식의 위쪽 절반이 그렇다.
 *  - 허용 크기가 하나도 없는 경우 — 반드시 0 이 나와야 한다.
 *
 * 마지막으로 32비트를 넘는 크기까지 돈다. 64비트 형식에서만 의미가 있어
 * 폭을 먼저 확인하고 들어간다.
 */
static void test_best_pgsize(struct kunit *test)
{
	unsigned int a_lg2;	/* [한국어] VA 의 정렬 */
	unsigned int b_lg2;	/* [한국어] OA 의 정렬 */
	unsigned int c_lg2;	/* [한국어] 길이의 정렬 */

	/* Try random prefixes with every suffix combination */
	for (a_lg2 = 1; a_lg2 != 10; a_lg2++) {	/* [한국어] (원 주석: 무작위 접두에 모든 접미 조합을 시도한다) */
		for (b_lg2 = 1; b_lg2 != 10; b_lg2++) {	/* [한국어] OA 정렬을 바꿔 가며 */
			for (c_lg2 = 1; c_lg2 != 10; c_lg2++) {	/* [한국어] 길이 정렬도 */
				pt_vaddr_t pgsz_bitmap = get_random_u64();	/* [한국어] 임의의 허용 크기 조합 */
				pt_vaddr_t va = get_random_u64() << a_lg2;	/* [한국어] 그 정렬을 가진 임의의 VA */
				pt_oaddr_t oa = get_random_u64() << b_lg2;	/* [한국어] 그 정렬을 가진 임의의 OA */
				pt_vaddr_t last_va = log2_set_mod_max(	/* [한국어] 끝 주소는 */
					get_random_u64(), c_lg2);	/* [한국어] 그 정렬의 블록 끝으로 */

				if (va > last_va)	/* [한국어] 뒤집혔으면 */
					swap(va, last_va);	/* [한국어] 바로잡는다 */
				KUNIT_ASSERT_EQ(	/* [한국어] 최적화된 구현과 */
					test,	/* [한국어] 참조 구현이 */
					pt_compute_best_pgsize(pgsz_bitmap, va,	/* [한국어] 같은 답을 */
							       last_va, oa),	/* [한국어] 내는지 */
					ref_best_pgsize(pgsz_bitmap, va,	/* [한국어] 비교한다 */
							last_va, oa));	/* [한국어] 비트 연산의 정당성이 여기서 검증된다 */
			}
		}
	}

	/* 0 prefix, every suffix */
	for (c_lg2 = 1; c_lg2 != PT_VADDR_MAX_LG2 - 1; c_lg2++) {	/* [한국어] (원 주석: 접두가 0 인 경우, 모든 접미) */
		pt_vaddr_t pgsz_bitmap = get_random_u64();	/* [한국어] 임의의 허용 크기 */
		pt_vaddr_t va = 0;	/* [한국어] 시프트가 미정의가 되기 쉬운 극단 */
		pt_oaddr_t oa = 0;	/* [한국어] OA 도 0 */
		pt_vaddr_t last_va = log2_set_mod_max(0, c_lg2);	/* [한국어] 그 블록의 끝까지 */

		KUNIT_ASSERT_EQ(test,	/* [한국어] 두 구현이 */
				pt_compute_best_pgsize(pgsz_bitmap, va, last_va,	/* [한국어] 같은 답을 */
						       oa),	/* [한국어] 내는지 */
				ref_best_pgsize(pgsz_bitmap, va, last_va, oa));	/* [한국어] 확인한다 */
	}

	/* 1's prefix, every suffix */
	for (a_lg2 = 1; a_lg2 != 10; a_lg2++) {	/* [한국어] 접두가 모두 1 인 경우: VA 정렬을 바꿔 가며 */
		for (b_lg2 = 1; b_lg2 != 10; b_lg2++) {	/* [한국어] OA 정렬도 */
			for (c_lg2 = 1; c_lg2 != 10; c_lg2++) {	/* [한국어] 길이 정렬도 */
				pt_vaddr_t pgsz_bitmap = get_random_u64();	/* [한국어] 임의의 허용 크기 조합 */
				pt_vaddr_t va = PT_VADDR_MAX << a_lg2;	/* [한국어] (원 주석: 접두가 모두 1 인 경우) 부호 확장 형식의 위쪽 절반이 그렇다 */
				pt_oaddr_t oa = PT_VADDR_MAX << b_lg2;	/* [한국어] OA 도 상위가 모두 1 */
				pt_vaddr_t last_va = PT_VADDR_MAX;	/* [한국어] 끝은 주소 최대값 */

				KUNIT_ASSERT_EQ(	/* [한국어] 두 구현이 같은 답을 내는지 */
					test,
					pt_compute_best_pgsize(pgsz_bitmap, va,
							       last_va, oa),
					ref_best_pgsize(pgsz_bitmap, va,
							last_va, oa));
			}
		}
	}

	/* pgsize_bitmap is always 0 */
	for (a_lg2 = 1; a_lg2 != 10; a_lg2++) {	/* [한국어] 허용 크기가 없는 경우: VA 정렬을 바꿔 가며 */
		for (b_lg2 = 1; b_lg2 != 10; b_lg2++) {	/* [한국어] OA 정렬도 */
			for (c_lg2 = 1; c_lg2 != 10; c_lg2++) {	/* [한국어] 길이 정렬도 */
				pt_vaddr_t pgsz_bitmap = 0;	/* [한국어] (원 주석: 허용 크기가 하나도 없는 경우) */
				pt_vaddr_t va = get_random_u64() << a_lg2;	/* [한국어] 그 정렬을 가진 임의의 VA */
				pt_oaddr_t oa = get_random_u64() << b_lg2;	/* [한국어] 그 정렬을 가진 임의의 OA */
				pt_vaddr_t last_va = log2_set_mod_max(	/* [한국어] 끝 주소는 */
					get_random_u64(), c_lg2);

				if (va > last_va)	/* [한국어] 뒤집혔으면 */
					swap(va, last_va);
				KUNIT_ASSERT_EQ(	/* [한국어] 허용 크기가 없으면 */
					test,
					pt_compute_best_pgsize(pgsz_bitmap, va,
							       last_va, oa),
					0);	/* [한국어] 반드시 0 이 나와야 한다 */
			}
		}
	}

	if (sizeof(pt_vaddr_t) <= 4)	/* [한국어] 32비트 형식이면 */
		return;	/* [한국어] 아래 시험은 의미가 없다 */

	/* over 32 bit page sizes */
	for (a_lg2 = 32; a_lg2 != 42; a_lg2++) {	/* [한국어] (원 주석: 32비트를 넘는 페이지 크기) */
		for (b_lg2 = 32; b_lg2 != 42; b_lg2++) {	/* [한국어] 넓은 정렬로 */
			for (c_lg2 = 32; c_lg2 != 42; c_lg2++) {	/* [한국어] 넓은 길이까지 */
				pt_vaddr_t pgsz_bitmap = get_random_u64();	/* [한국어] 임의의 허용 크기 조합 */
				pt_vaddr_t va = get_random_u64() << a_lg2;	/* [한국어] 32비트를 넘는 정렬의 VA */
				pt_oaddr_t oa = get_random_u64() << b_lg2;	/* [한국어] 같은 폭의 OA */
				pt_vaddr_t last_va = log2_set_mod_max(	/* [한국어] 끝 주소는 */
					get_random_u64(), c_lg2);

				if (va > last_va)	/* [한국어] 뒤집혔으면 */
					swap(va, last_va);
				KUNIT_ASSERT_EQ(	/* [한국어] 두 구현이 같은 답을 내는지 */
					test,
					pt_compute_best_pgsize(pgsz_bitmap, va,
							       last_va, oa),
					ref_best_pgsize(pgsz_bitmap, va,
							last_va, oa));
			}
		}
	}
}

/*
 * [한국어]
 * test_pgsz_count - 같은 크기로 몇 개를 연달아 놓을 수 있는지 센다
 *
 * @test: 시험 문맥.
 *
 * 두 경우를 짚는다. 4KB 하나뿐이면 길이를 그대로 나눈 값이 나오고,
 * 2MB 도 쓸 수 있으면 그 경계에 닿을 때까지만 세어야 한다 — 거기서부터는
 * 더 큰 페이지를 쓰는 편이 낫기 때문이다.
 */
static void test_pgsz_count(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,	/* [한국어] 4KB 하나뿐이면 */
			pt_pgsz_count(SZ_4K, 0, SZ_1G - 1, 0, ilog2(SZ_4K)),	/* [한국어] 1GB 를 4KB 로 나눈 만큼 */
			SZ_1G / SZ_4K);	/* [한국어] 전부 놓을 수 있다 */
	KUNIT_EXPECT_EQ(test,	/* [한국어] 2MB 도 쓸 수 있으면 */
			pt_pgsz_count(SZ_2M | SZ_4K, SZ_4K, SZ_1G - 1, SZ_4K,	/* [한국어] 2MB 경계에 닿을 때까지만 세어야 한다 */
				      ilog2(SZ_4K)),	/* [한국어] 4KB 로 */
			(SZ_2M - SZ_4K) / SZ_4K);	/* [한국어] 거기서부터는 큰 페이지를 쓰는 편이 낫다 */
}

/*
 * Check that pt_install_table() and pt_table_pa() match
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_lvl_table_ptr - 표 포인터를 쓰고 되읽어 같은지 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * pt_install_table 과 pt_table_pa 가 짝이 맞는지 확인한다. 형식이 주소에
 * 암호화 비트 같은 것을 얹는다면 그 왕복이 여기서 드러난다.
 *
 * 두 번째 설치가 성공해야 하는 이유가 미묘하다. cmpxchg 는 pts->entry 를
 * 비교값으로 쓰는데, 첫 설치가 그 값을 새것으로 갱신해 두었으므로 두 번째
 * 도 통과한다. 그 갱신이 빠지면 여기서 걸린다.
 */
static void test_lvl_table_ptr(struct kunit *test, struct pt_state *pts,
			       void *arg)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	pt_oaddr_t paddr =	/* [한국어] 쓸 표 주소 */
		log2_set_mod(priv->test_oa, 0, priv->smallest_pgsz_lg2);	/* [한국어] 페이지 정렬로 깎는다 */
	struct pt_write_attrs attrs = {};	/* [한국어] 쓸 때 얹을 속성 */

	if (!pt_can_have_table(pts))	/* [한국어] 아래 표를 둘 수 없는 단계면 */
		return;	/* [한국어] 시험할 것이 없다 */

	KUNIT_ASSERT_NO_ERRNO_FN(test, "pt_iommu_set_prot",	/* [한국어] 권한을 형식 비트로 */
				 pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] 옮긴다 */
						   IOMMU_READ));	/* [한국어] 읽기만 */

	pt_load_single_entry(pts);	/* [한국어] 시험할 자리를 */
	KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 비어 있어야 한다 */

	KUNIT_ASSERT_TRUE(test, pt_install_table(pts, paddr, &attrs));	/* [한국어] 표를 꽂는다 */

	/* A second install should pass because install updates pts->entry. */
	KUNIT_ASSERT_EQ(test, pt_install_table(pts, paddr, &attrs), true);	/* [한국어] (원 주석: 설치가 pts->entry 를 갱신하므로 두 번째도 성공해야 한다) */

	KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_TABLE);	/* [한국어] 되읽으면 표로 보여야 하고 */
	KUNIT_ASSERT_EQ(test, pt_table_pa(pts), paddr);	/* [한국어] 주소가 왕복해야 한다 */

	pt_clear_entries(pts, ilog2(1));	/* [한국어] 지운 뒤 */
	KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 다시 비어야 한다 */
}

/*
 * [한국어]
 * test_table_ptr - 표 포인터 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static void test_table_ptr(struct kunit *test)
{
	check_all_levels(test, test_lvl_table_ptr, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

struct lvl_radix_arg {	/* [한국어] 아래 단계들이 해석한 VA 비트를 위로 나르는 묶음 */
	pt_vaddr_t vbits;
	/* [한국어] 지금까지 아래 단계들이 해석한 VA 비트.
	 * 설정자: 가장 작은 페이지 크기에서 시작해, 각 단계가 자기 몫을 더한다.
	 * 읽는 자: 다음 단계가 빈틈과 겹침을 확인할 때, 그리고 마지막에
	 *   test_table_radix 가 표 전체와 비교할 때.
	 * 값 범위: 하위부터 이어진 1 들의 마스크.
	 * 동기화: 호출 스택 값. */
};

/*
 * Check pt_table_oa_lg2sz() and pt_table_item_lg2sz() they need to decode a
 * continuous list of VA across all the levels that covers the entire advertised
 * VA space.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_lvl_radix - 단계들이 VA 비트를 빈틈없이 나눠 갖는지 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 지금까지 아래 단계들이 해석한 비트들.
 *
 * 아래에서 위로 올라오며 확인한다. 이 단계의 항목 크기 아래 비트는 전부
 * 아래 단계들이 이미 해석했어야 하고(빈틈 없음), 이 단계가 그 비트를
 * 다시 해석해서도 안 된다(겹침 없음).
 *
 * 그렇게 올라가면 최상위에서 모은 비트가 곧 이 표가 덮는 주소 공간 전체가
 * 되고, test_table_radix 가 그것을 확인한다.
 */
static void test_lvl_radix(struct kunit *test, struct pt_state *pts, void *arg)
{
	unsigned int table_lg2sz = pt_table_oa_lg2sz(pts);	/* [한국어] 이 표가 덮는 범위 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나가 덮는 범위 */
	struct lvl_radix_arg *radix = arg;	/* [한국어] 아래 단계들이 해석한 비트 */

	/* Every bit below us is decoded */
	KUNIT_ASSERT_EQ(test, log2_set_mod_max(0, isz_lg2), radix->vbits);	/* [한국어] (원 주석: 우리 아래의 모든 비트가 해석되어 있다) */

	/* We are not decoding bits someone else is */
	KUNIT_ASSERT_EQ(test, log2_div(radix->vbits, isz_lg2), 0);	/* [한국어] (원 주석: 남이 해석하는 비트를 우리가 다시 해석하지 않는다) */

	/* Can't decode past the pt_vaddr_t size */
	KUNIT_ASSERT_LE(test, table_lg2sz, PT_VADDR_MAX_LG2);	/* [한국어] (원 주석: pt_vaddr_t 크기를 넘어 해석할 수 없다) */
	KUNIT_ASSERT_EQ(test, fvalog2_div(table_lg2sz, PT_MAX_VA_ADDRESS_LG2),	/* [한국어] 형식의 최대 폭도 */
			0);	/* [한국어] 넘지 않아야 한다 */

	radix->vbits = fvalog2_set_mod_max(0, table_lg2sz);	/* [한국어] 위 단계에 넘길 누적 비트 */
}

/*
 * [한국어]
 * test_max_va - 인스턴스의 주소 폭이 현재 표보다 좁지 않은지 본다
 *
 * @test: 시험 문맥.
 *
 * 자라는 형식에서는 현재 표가 인스턴스 폭보다 좁은 것이 정상이지만,
 * 그 반대는 있을 수 없다 — 표가 쓸 수 없는 주소를 유효하다고 말하는 셈이다.
 */
static void test_max_va(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 현재 표의 범위 */

	KUNIT_ASSERT_GE(test, priv->common->max_vasz_lg2, range.max_vasz_lg2);	/* [한국어] 표가 인스턴스보다 넓으면 쓸 수 없는 주소를 유효하다고 말하는 셈이다 */
}

/*
 * [한국어]
 * test_table_radix - 단계별 비트 분배를 모든 단계에 걸쳐 확인한다
 *
 * @test: 시험 문맥.
 *
 * 가장 작은 페이지가 해석하는 비트에서 시작해 위로 모아 올라간 결과가,
 * 이 표가 덮어야 할 주소 공간과 정확히 같은지 본다.
 *
 * 32비트 환경에서 앞 단언을 건너뛰는 이유: 그쪽에서는 시험이 VA 를 32비트로
 * 제한해 최상위까지 닿지 못한다.
 */
static void test_table_radix(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct lvl_radix_arg radix = { .vbits = priv->smallest_pgsz - 1 };	/* [한국어] 가장 작은 페이지가 해석하는 비트에서 시작 */
	struct pt_range range;	/* [한국어] 최상위 범위 */

	check_all_levels(test, test_lvl_radix, &radix);	/* [한국어] 아래에서 위로 모아 올린다 */

	range = pt_top_range(priv->common);	/* [한국어] 최상위 범위를 읽어 */
	if (range.max_vasz_lg2 == PT_VADDR_MAX_LG2) {	/* [한국어] 주소 공간이 전 범위면 */
		KUNIT_ASSERT_EQ(test, radix.vbits, PT_VADDR_MAX);	/* [한국어] 모든 비트가 해석되어야 한다 */
	} else {
		if (!IS_32BIT)	/* [한국어] 32비트에서는 최상위까지 닿지 못해 */
			KUNIT_ASSERT_EQ(test,	/* [한국어] 이 단언을 건너뛴다 */
					log2_set_mod_max(0, range.max_vasz_lg2),	/* [한국어] 표가 덮어야 할 비트가 */
					radix.vbits);	/* [한국어] 모아 올린 것과 같아야 한다 */
		KUNIT_ASSERT_EQ(test, log2_div(radix.vbits, range.max_vasz_lg2),	/* [한국어] 그 위로는 */
				0);	/* [한국어] 아무것도 해석하지 않아야 한다 */
	}
}

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * safe_pt_num_items_lg2 - 최상위에서도 안전하게 항목 수를 구한다
 *
 * @pts: 볼 단계.
 * @return: 그 단계 표의 항목 수(지수).
 *
 * 원 주석이 이유를 밝힌다 — pt_num_items_lg2 는 최상위에서 부르면 안 된다.
 * 그쪽 항목 수는 형식이 아니라 인스턴스의 주소 폭이 정하기 때문이다.
 *
 * 그래서 최상위에서는 순회가 계산한 끝 색인에서 역산한다.
 */
static unsigned int safe_pt_num_items_lg2(const struct pt_state *pts)
{
	struct pt_range top_range = pt_top_range(pts->range->common);	/* [한국어] 최상위 범위 */
	struct pt_state top_pts = pt_init_top(&top_range);	/* [한국어] 그 단계의 상태 */

	/*
	 * Avoid calling pt_num_items_lg2() on the top, instead we can derive
	 * the size of the top table from the top range.
	 */
	if (pts->level == top_range.top_level)	/* [한국어] (원 주석: 최상위에서는 pt_num_items_lg2 를 부르지 말고 범위에서 크기를 유도한다) */
		return ilog2(pt_range_to_end_index(&top_pts));	/* [한국어] 순회가 계산한 끝 색인에서 역산한다 */
	return pt_num_items_lg2(pts);	/* [한국어] 그 밖의 단계는 형식이 답한다 */
}

/*
 * [한국어]
 * test_lvl_possible_sizes - 크기 비트맵이 규약대로 생겼는지 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 단계.
 * @arg: 쓰지 않는다.
 *
 * pt_common.h 가 정한 비트맵의 모양을 그대로 확인한다: 항목 크기보다 작은
 * 비트가 없고, 표 하나를 넘는 비트도 없으며, 단일 항목 크기는 반드시
 * 있어야 한다.
 *
 * AMD v1 만 예외를 둔다. 원 주석대로 하드웨어 결함으로 한 단계에서
 * 단일 항목 크기를 쓸 수 없어, 그 형식과 그 비트맵일 때만 조건을 늦춘다.
 */
static void test_lvl_possible_sizes(struct kunit *test, struct pt_state *pts,
				    void *arg)
{
	unsigned int num_items_lg2 = safe_pt_num_items_lg2(pts);	/* [한국어] 이 표의 항목 수 */
	pt_vaddr_t pgsize_bitmap = pt_possible_sizes(pts);	/* [한국어] 이 단계가 만들 수 있는 크기들 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나의 크기 */

	if (!pt_can_have_leaf(pts)) {	/* [한국어] 잎을 놓을 수 없는 단계면 */
		KUNIT_ASSERT_EQ(test, pgsize_bitmap, 0);	/* [한국어] 비트맵이 비어야 한다 */
		return;	/* [한국어] 더 볼 것이 없다 */
	}

	/* No bits for sizes that would be outside this table */
	KUNIT_ASSERT_EQ(test, log2_mod(pgsize_bitmap, isz_lg2), 0);	/* [한국어] (원 주석: 이 표 밖의 크기에 해당하는 비트가 없어야 한다) */
	KUNIT_ASSERT_EQ(	/* [한국어] 표 하나를 넘는 */
		test, fvalog2_div(pgsize_bitmap, num_items_lg2 + isz_lg2), 0);	/* [한국어] 크기도 없어야 한다 */

	/*
	 * Non contiguous must be supported. AMDv1 has a HW bug where it does
	 * not support it on one of the levels.
	 */
	if ((u64)pgsize_bitmap != 0xff0000000000ULL ||	/* [한국어] (원 주석: 비연속 항목은 반드시 지원해야 한다. AMDv1 은 한 단계에서 그것이 안 되는 하드웨어 결함이 있다) */
	    strcmp(__stringify(PTPFX_RAW), "amdv1") != 0)	/* [한국어] 그 형식의 그 비트맵만 예외 */
		KUNIT_ASSERT_TRUE(test, pgsize_bitmap & log2_to_int(isz_lg2));	/* [한국어] 단일 항목 크기는 늘 있어야 한다 */
	else
		KUNIT_ASSERT_NE(test, pgsize_bitmap, 0);	/* [한국어] 예외인 경우에도 무언가는 있어야 한다 */

	/* A contiguous entry should not span the whole table */
	if (num_items_lg2 + isz_lg2 != PT_VADDR_MAX_LG2)	/* [한국어] (원 주석: 연속 항목이 표 전체를 덮어서는 안 된다) */
		KUNIT_ASSERT_FALSE(	/* [한국어] 그러면 한 단계 위의 잎과 같아진다 */
			test,	/* [한국어] 시험 문맥 */
			pgsize_bitmap & log2_to_int(num_items_lg2 + isz_lg2));	/* [한국어] 그 비트는 서 있으면 안 된다 */
}

/*
 * [한국어]
 * test_entry_possible_sizes - 크기 비트맵 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static void test_entry_possible_sizes(struct kunit *test)
{
	check_all_levels(test, test_lvl_possible_sizes, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

/*
 * [한국어]
 * sweep_all_pgsizes - 가능한 모든 크기로 잎을 쓰고 되읽어 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @attrs: 쓸 때 얹을 권한.
 * @test_oaddr: 시험할 출력 주소.
 *
 * 이 단계가 만들 수 있는 크기를 하나씩 골라 잎을 놓고, 그 묶음의 모든
 * item 을 훑으며 세 가지를 확인한다: item 마다의 주소가 순서대로 늘어나는가,
 * entry 시작 주소는 어느 item 에서 보든 같은가, 묶음 크기가 맞는가.
 *
 * 그 셋이 item 과 entry 의 구분(pt_defs.h 의 용어집)을 그대로 검증한다.
 *
 * 0번 자리에서만 도는 이유: 큰 연속 묶음이 표를 넘어가지 않게 하려는 것이다.
 */
static void sweep_all_pgsizes(struct kunit *test, struct pt_state *pts,
			      struct pt_write_attrs *attrs,
			      pt_oaddr_t test_oaddr)
{
	pt_vaddr_t pgsize_bitmap = pt_possible_sizes(pts);	/* [한국어] 이 단계가 만들 수 있는 크기들 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나의 크기 */
	unsigned int len_lg2;	/* [한국어] 시도할 크기 */

	if (pts->index != 0)	/* [한국어] 0번 자리에서만 — 큰 묶음이 표를 넘지 않게 */
		return;	/* [한국어] 다른 자리는 건너뛴다 */

	for (len_lg2 = 0; len_lg2 < PT_VADDR_MAX_LG2 - 1; len_lg2++) {	/* [한국어] 가능한 크기를 하나씩 */
		struct pt_state sub_pts = *pts;	/* [한국어] 묶음 안의 item 을 훑을 상태 */
		pt_oaddr_t oaddr;	/* [한국어] 그 크기에 정렬한 주소 */

		if (!(pgsize_bitmap & log2_to_int(len_lg2)))	/* [한국어] 만들 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */

		oaddr = log2_set_mod(test_oaddr, 0, len_lg2);	/* [한국어] 그 크기에 정렬한다 */
		pt_install_leaf_entry(pts, oaddr, len_lg2, attrs);	/* [한국어] 잎을 놓고 */
		/* Verify that every contiguous item translates correctly */
		for (sub_pts.index = 0;	/* [한국어] (원 주석: 연속 묶음의 모든 item 이 옳게 변환되는지 확인한다) */
		     sub_pts.index != log2_to_int(len_lg2 - isz_lg2);	/* [한국어] 묶음의 item 수만큼 */
		     sub_pts.index++) {	/* [한국어] 하나씩 */
			KUNIT_ASSERT_PT_LOAD(test, &sub_pts, PT_ENTRY_OA);	/* [한국어] 모두 잎으로 보여야 하고 */
			KUNIT_ASSERT_EQ(test, pt_item_oa(&sub_pts),	/* [한국어] item 마다의 주소는 */
					oaddr + sub_pts.index *	/* [한국어] 시작에서 순서대로 */
							oalog2_mul(1, isz_lg2));	/* [한국어] 항목 크기만큼 떨어져 있어야 한다 */
			KUNIT_ASSERT_EQ(test, pt_entry_oa(&sub_pts), oaddr);	/* [한국어] entry 시작 주소는 어느 item 에서 보든 같아야 하고 */
			KUNIT_ASSERT_EQ(test, pt_entry_num_contig_lg2(&sub_pts),	/* [한국어] 묶음 크기도 */
					len_lg2 - isz_lg2);	/* [한국어] 일치해야 한다 */
		}

		pt_clear_entries(pts, len_lg2 - isz_lg2);	/* [한국어] 묶음을 지우고 */
		KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 다시 비어야 한다 */
	}
}

/*
 * Check that pt_install_leaf_entry() and pt_entry_oa() match.
 * Check that pt_clear_entries() works.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_lvl_entry_oa - 잎을 쓰고 되읽어 주소가 맞는지 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * 평범한 주소 하나와, 경계값 둘(0 과 표현 가능한 최대)로 각각 훑는다.
 * 경계에서 비트가 잘리거나 넘치는 형식 버그가 그 둘에서 드러난다.
 */
static void test_lvl_entry_oa(struct kunit *test, struct pt_state *pts,
			      void *arg)
{
	unsigned int max_oa_lg2 = pts->range->common->max_oasz_lg2;	/* [한국어] 표현할 수 있는 출력 주소 폭 */
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_write_attrs attrs = {};	/* [한국어] 쓸 때 얹을 속성 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return;	/* [한국어] 시험할 것이 없다 */

	KUNIT_ASSERT_NO_ERRNO_FN(test, "pt_iommu_set_prot",	/* [한국어] 권한을 형식 비트로 */
				 pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] 옮긴다 */
						   IOMMU_READ));	/* [한국어] 읽기만 */

	sweep_all_pgsizes(test, pts, &attrs, priv->test_oa);	/* [한국어] 평범한 주소로 한 번 */

	/* Check that the table can store the boundary OAs */
	sweep_all_pgsizes(test, pts, &attrs, 0);	/* [한국어] (원 주석: 표가 경계 OA 를 담을 수 있는지 확인한다) */
	if (max_oa_lg2 == PT_OADDR_MAX_LG2)	/* [한국어] 폭이 타입 전체면 */
		sweep_all_pgsizes(test, pts, &attrs, PT_OADDR_MAX);	/* [한국어] 타입의 최대값으로 */
	else
		sweep_all_pgsizes(test, pts, &attrs,	/* [한국어] 아니면 그 폭에서 */
				  oalog2_to_max_int(max_oa_lg2));	/* [한국어] 표현 가능한 최대값으로 */
}

/*
 * [한국어]
 * test_entry_oa - 잎 주소 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static void test_entry_oa(struct kunit *test)
{
	check_all_levels(test, test_lvl_entry_oa, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

/* Test pt_attr_from_entry() */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_lvl_attr_from_entry - 권한을 꺼냈다 다시 넣으면 같은 항목이 되는지
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * pt_common.h 가 pt_attr_from_entry 에 요구한 왕복 계약을 확인한다.
 * 크기 × 권한 조합을 모두 돌며, 쓴 항목에서 권한을 꺼내고 지운 뒤 그
 * 권한으로 다시 쓰면 비트 하나까지 같은 값이 나와야 한다.
 *
 * 형식이 지원하지 않는 권한 조합은 건너뛰되, 읽기·쓰기만은 반드시 지원해야
 * 한다고 단언한다 — 그것마저 없으면 쓸 수 없는 형식이다.
 */
static void test_lvl_attr_from_entry(struct kunit *test, struct pt_state *pts,
				     void *arg)
{
	pt_vaddr_t pgsize_bitmap = pt_possible_sizes(pts);	/* [한국어] 이 단계가 만들 수 있는 크기들 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나의 크기 */
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	unsigned int len_lg2;	/* [한국어] 시도할 크기 */
	unsigned int prot;	/* [한국어] 시도할 권한 조합 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return;	/* [한국어] 시험할 것이 없다 */

	for (len_lg2 = 0; len_lg2 < PT_VADDR_MAX_LG2; len_lg2++) {	/* [한국어] 가능한 크기를 하나씩 */
		if (!(pgsize_bitmap & log2_to_int(len_lg2)))	/* [한국어] 만들 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */
		for (prot = 0; prot <= (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |	/* [한국어] 권한 조합을 */
					IOMMU_NOEXEC | IOMMU_MMIO);	/* [한국어] 0 부터 전부까지 */
		     prot++) {	/* [한국어] 하나씩 */
			pt_oaddr_t oaddr;	/* [한국어] 정렬한 주소 */
			struct pt_write_attrs attrs = {};	/* [한국어] 옮긴 권한 비트 */
			u64 good_entry;	/* [한국어] 처음 쓴 항목의 값 */

			/*
			 * If the format doesn't support this combination of
			 * prot bits skip it
			 */
			if (pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] (원 주석: 형식이 지원하지 않는 조합은 건너뛴다) */
					      prot)) {	/* [한국어] 거절하면 */
				/* But RW has to be supported */
				KUNIT_ASSERT_NE(test, prot,	/* [한국어] (원 주석: 다만 읽기·쓰기는 반드시 지원해야 한다) */
						IOMMU_READ | IOMMU_WRITE);	/* [한국어] 그것마저 없으면 쓸 수 없는 형식이다 */
				continue;	/* [한국어] 다음 조합으로 */
			}

			oaddr = log2_set_mod(priv->test_oa, 0, len_lg2);	/* [한국어] 그 크기에 정렬한다 */
			pt_install_leaf_entry(pts, oaddr, len_lg2, &attrs);	/* [한국어] 잎을 놓고 */
			KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_OA);	/* [한국어] 잎으로 보이는지 */

			good_entry = pts->entry;	/* [한국어] 정답으로 삼을 값 */

			memset(&attrs, 0, sizeof(attrs));	/* [한국어] 속성을 비우고 */
			pt_attr_from_entry(pts, &attrs);	/* [한국어] 항목에서 다시 꺼낸다 */

			pt_clear_entries(pts, len_lg2 - isz_lg2);	/* [한국어] 자리를 비운 뒤 */
			KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 정말 비었는지 확인하고 */

			pt_install_leaf_entry(pts, oaddr, len_lg2, &attrs);	/* [한국어] 꺼낸 속성으로 다시 쓴다 */
			KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_OA);	/* [한국어] 다시 잎으로 보여야 하고 */

			/*
			 * The descriptor produced by pt_attr_from_entry()
			 * produce an identical entry value when re-written
			 */
			KUNIT_ASSERT_EQ(test, good_entry, pts->entry);	/* [한국어] (원 주석: 다시 쓴 항목이 원래와 똑같아야 한다) */

			pt_clear_entries(pts, len_lg2 - isz_lg2);	/* [한국어] 다음 조합을 위해 비운다 */
		}
	}
}

/*
 * [한국어]
 * test_attr_from_entry - 권한 왕복 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static void test_attr_from_entry(struct kunit *test)
{
	check_all_levels(test, test_lvl_attr_from_entry, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

/*
 * [한국어]
 * test_lvl_dirty - 더티 비트의 세 함수가 서로 맞는지 본다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * 연속 묶음이 이 시험의 초점이다. 묶음의 어느 item 에 표시를 남겨도
 * entry 전체가 더티로 보여야 하고(하드웨어는 실제로 접근한 item 에만
 * 남긴다), 한 번 지우면 묶음 전체가 깨끗해져야 한다.
 *
 * 그래서 안쪽 반복문이 item 을 하나씩 옮겨 가며 표시하고, 매번 묶음의
 * 첫 자리에서 읽어 확인한 뒤 지운다.
 */
static void test_lvl_dirty(struct kunit *test, struct pt_state *pts, void *arg)
{
	pt_vaddr_t pgsize_bitmap = pt_possible_sizes(pts);	/* [한국어] 이 단계가 만들 수 있는 크기들 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나의 크기 */
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	unsigned int start_idx = pts->index;	/* [한국어] 묶음의 첫 자리를 기억한다 */
	struct pt_write_attrs attrs = {};	/* [한국어] 쓸 때 얹을 속성 */
	unsigned int len_lg2;	/* [한국어] 시도할 크기 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return;	/* [한국어] 시험할 것이 없다 */

	KUNIT_ASSERT_NO_ERRNO_FN(test, "pt_iommu_set_prot",	/* [한국어] 권한을 형식 비트로 */
				 pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] 옮긴다 */
						   IOMMU_READ | IOMMU_WRITE));	/* [한국어] 더티는 쓰기 권한이 있어야 뜻이 있다 */

	for (len_lg2 = 0; len_lg2 < PT_VADDR_MAX_LG2; len_lg2++) {	/* [한국어] 가능한 크기를 하나씩 */
		pt_oaddr_t oaddr;	/* [한국어] 정렬한 주소 */
		unsigned int i;	/* [한국어] 묶음 안의 item 번호 */

		if (!(pgsize_bitmap & log2_to_int(len_lg2)))	/* [한국어] 만들 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */

		oaddr = log2_set_mod(priv->test_oa, 0, len_lg2);	/* [한국어] 그 크기에 정렬한다 */
		pt_install_leaf_entry(pts, oaddr, len_lg2, &attrs);	/* [한국어] 잎을 놓고 */
		KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_OA);	/* [한국어] 잎으로 보이는지 */

		pt_load_entry(pts);	/* [한국어] 항목을 읽어 */
		pt_entry_make_write_clean(pts);	/* [한국어] 더티를 지우고 */
		pt_load_entry(pts);	/* [한국어] 다시 읽어 */
		KUNIT_ASSERT_FALSE(test, pt_entry_is_write_dirty(pts));	/* [한국어] 깨끗한지 확인한다 */

		for (i = 0; i != log2_to_int(len_lg2 - isz_lg2); i++) {	/* [한국어] 묶음의 item 을 하나씩 */
			/* dirty every contiguous entry */
			pts->index = start_idx + i;	/* [한국어] (원 주석: 연속 항목마다 더티를 찍는다) */
			pt_load_entry(pts);	/* [한국어] 그 item 을 읽어 */
			KUNIT_ASSERT_TRUE(test, pt_entry_make_write_dirty(pts));	/* [한국어] 표시를 남기고 */
			pts->index = start_idx;	/* [한국어] 묶음의 첫 자리로 돌아가 */
			pt_load_entry(pts);	/* [한국어] 거기서 읽어도 */
			KUNIT_ASSERT_TRUE(test, pt_entry_is_write_dirty(pts));	/* [한국어] entry 전체가 더티로 보여야 한다 */

			pt_entry_make_write_clean(pts);	/* [한국어] 한 번 지우면 */
			pt_load_entry(pts);	/* [한국어] 다시 읽었을 때 */
			KUNIT_ASSERT_FALSE(test, pt_entry_is_write_dirty(pts));	/* [한국어] 묶음 전체가 깨끗해야 한다 */
		}

		pt_clear_entries(pts, len_lg2 - isz_lg2);	/* [한국어] 다음 크기를 위해 비운다 */
	}
}

/*
 * [한국어]
 * test_dirty - 더티 비트 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 *
 * 형식이나 설정이 더티 추적을 지원하지 않으면 실패가 아니라 건너뛴다.
 */
static __maybe_unused void test_dirty(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */

	if (!pt_dirty_supported(priv->common))	/* [한국어] 형식이나 설정이 더티 추적을 못 하면 */
		kunit_skip(test,	/* [한국어] 실패가 아니라 */
			   "Page table features do not support dirty tracking");	/* [한국어] 건너뛴다 */

	check_all_levels(test, test_lvl_dirty, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

/*
 * [한국어]
 * test_lvl_sw_bit_leaf - 잎 항목의 소프트웨어 비트를 시험한다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * 세 가지를 확인한다.
 *  - 갓 쓴 항목에는 소프트웨어 비트가 하나도 서 있지 않다.
 *  - 하나씩 세우면 그것만 서고, 이미 세운 것들도 그대로 남는다.
 *  - 그 비트들이 출력 주소를 건드리지 않는다.
 *
 * 마지막 단언이 가장 중요하다. pt_attr_from_entry 로 꺼낸 권한에 소프트웨어
 * 비트가 섞여 들어가면, 큰 페이지를 쪼갤 때 그 비트가 엉뚱한 항목으로
 * 복사된다.
 */
static void test_lvl_sw_bit_leaf(struct kunit *test, struct pt_state *pts,
				 void *arg)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	pt_vaddr_t pgsize_bitmap = pt_possible_sizes(pts);	/* [한국어] 이 단계가 만들 수 있는 크기들 */
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 항목 하나의 크기 */
	struct pt_write_attrs attrs = {};	/* [한국어] 쓸 때 얹을 속성 */
	unsigned int len_lg2;	/* [한국어] 시도할 크기 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return;	/* [한국어] 시험할 것이 없다 */
	if (pts->index != 0)	/* [한국어] 0번 자리에서만 */
		return;	/* [한국어] 큰 묶음이 표를 넘지 않게 */

	KUNIT_ASSERT_NO_ERRNO_FN(test, "pt_iommu_set_prot",	/* [한국어] 권한을 형식 비트로 */
				 pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] 옮긴다 */
						   IOMMU_READ));	/* [한국어] 읽기만 */

	for (len_lg2 = 0; len_lg2 < PT_VADDR_MAX_LG2 - 1; len_lg2++) {	/* [한국어] 가능한 크기를 하나씩 */
		pt_oaddr_t paddr = log2_set_mod(priv->test_oa, 0, len_lg2);	/* [한국어] 그 크기에 정렬한 주소 */
		struct pt_write_attrs new_attrs = {};	/* [한국어] 되꺼낸 속성을 담을 자리 */
		unsigned int bitnr;	/* [한국어] 시도할 비트 번호 */

		if (!(pgsize_bitmap & log2_to_int(len_lg2)))	/* [한국어] 만들 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */

		pt_install_leaf_entry(pts, paddr, len_lg2, &attrs);	/* [한국어] 잎을 놓는다 */

		for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common);	/* [한국어] 갓 쓴 항목에는 */
		     bitnr++)	/* [한국어] 어떤 비트도 */
			KUNIT_ASSERT_FALSE(test,	/* [한국어] 서 있지 */
					   pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 않아야 한다 */

		for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common);	/* [한국어] 하나씩 세워 가며 */
		     bitnr++) {	/* [한국어] 확인한다 */
			KUNIT_ASSERT_FALSE(test,	/* [한국어] 세우기 전에는 꺼져 있고 */
					   pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 아직 아무도 세우지 않았다 */
			pt_set_sw_bit_release(pts, bitnr);	/* [한국어] 세우면 */
			KUNIT_ASSERT_TRUE(test,	/* [한국어] 그 비트만 */
					  pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 서야 한다 */
		}

		for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common);	/* [한국어] 다 세운 뒤에는 */
		     bitnr++)	/* [한국어] 모든 비트가 */
			KUNIT_ASSERT_TRUE(test,	/* [한국어] 그대로 */
					  pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 남아 있어야 한다 */

		KUNIT_ASSERT_EQ(test, pt_item_oa(pts), paddr);	/* [한국어] 주소 필드를 침범하지 않았는지 */

		/* SW bits didn't leak into the attrs */
		pt_attr_from_entry(pts, &new_attrs);	/* [한국어] (원 주석: 소프트웨어 비트가 attrs 로 새어 나가지 않았다) */
		KUNIT_ASSERT_MEMEQ(test, &new_attrs, &attrs, sizeof(attrs));	/* [한국어] 새어 나가면 큰 페이지를 쪼갤 때 엉뚱한 항목에 복사된다 */

		pt_clear_entries(pts, len_lg2 - isz_lg2);	/* [한국어] 다음 크기를 위해 비우고 */
		KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 정말 비었는지 확인한다 */
	}
}

/*
 * [한국어]
 * test_sw_bit_leaf - 잎의 소프트웨어 비트 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static __maybe_unused void test_sw_bit_leaf(struct kunit *test)
{
	check_all_levels(test, test_lvl_sw_bit_leaf, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

/*
 * [한국어]
 * test_lvl_sw_bit_table - 표 항목의 소프트웨어 비트를 시험한다
 *
 * @test: 시험 문맥.
 * @pts: 시험할 자리.
 * @arg: 쓰지 않는다.
 *
 * 잎 쪽과 같되 표 항목에 대해 확인한다. 이쪽이 실제로 쓰이는 자리다 —
 * iommu_pt.h 의 SW_BIT_CACHE_FLUSH_DONE 이 표 항목의 비트이기 때문이다.
 *
 * 마지막에 표 주소가 그대로인지 확인해, 비트가 주소 필드를 침범하지
 * 않았음을 본다.
 */
static void test_lvl_sw_bit_table(struct kunit *test, struct pt_state *pts,
				  void *arg)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_write_attrs attrs = {};	/* [한국어] 쓸 때 얹을 속성 */
	pt_oaddr_t paddr =	/* [한국어] 꽂을 표 주소 */
		log2_set_mod(priv->test_oa, 0, priv->smallest_pgsz_lg2);	/* [한국어] 페이지 정렬로 깎는다 */
	unsigned int bitnr;	/* [한국어] 시도할 비트 번호 */

	if (!pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return;	/* [한국어] 건너뛴다 */
	if (pts->index != 0)	/* [한국어] 0번 자리에서만 */
		return;	/* [한국어] 다른 자리는 건너뛴다 */

	KUNIT_ASSERT_NO_ERRNO_FN(test, "pt_iommu_set_prot",	/* [한국어] 권한을 형식 비트로 */
				 pt_iommu_set_prot(pts->range->common, &attrs,	/* [한국어] 옮긴다 */
						   IOMMU_READ));	/* [한국어] 읽기만 */

	KUNIT_ASSERT_TRUE(test, pt_install_table(pts, paddr, &attrs));	/* [한국어] 표를 꽂는다 — 실제로 이 비트가 쓰이는 자리다 */

	for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common); bitnr++)	/* [한국어] 갓 꽂은 항목에는 */
		KUNIT_ASSERT_FALSE(test, pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 어떤 비트도 서 있지 않아야 한다 */

	for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common); bitnr++) {	/* [한국어] 하나씩 세워 가며 */
		KUNIT_ASSERT_FALSE(test, pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 세우기 전에는 꺼져 있고 */
		pt_set_sw_bit_release(pts, bitnr);	/* [한국어] 세우면 */
		KUNIT_ASSERT_TRUE(test, pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 그 비트만 선다 */
	}

	for (bitnr = 0; bitnr <= pt_max_sw_bit(pts->range->common); bitnr++)	/* [한국어] 다 세운 뒤에도 모든 비트가 */
		KUNIT_ASSERT_TRUE(test, pt_test_sw_bit_acquire(pts, bitnr));	/* [한국어] 다 세운 뒤에도 모두 남아 있어야 한다 */

	KUNIT_ASSERT_EQ(test, pt_table_pa(pts), paddr);	/* [한국어] 표 주소를 침범하지 않았는지 */

	pt_clear_entries(pts, ilog2(1));	/* [한국어] 자리를 비우고 */
	KUNIT_ASSERT_PT_LOAD(test, pts, PT_ENTRY_EMPTY);	/* [한국어] 정말 비었는지 확인한다 */
}

/*
 * [한국어]
 * test_sw_bit_table - 표의 소프트웨어 비트 시험을 모든 단계에 적용한다
 *
 * @test: 시험 문맥.
 */
static __maybe_unused void test_sw_bit_table(struct kunit *test)
{
	check_all_levels(test, test_lvl_sw_bit_table, NULL);	/* [한국어] 모든 단계에 적용한다 */
}

static struct kunit_case generic_pt_test_cases[] = {	/* [한국어] 이 형식에 대해 돌릴 시험 목록 */
	KUNIT_CASE_FMT(test_init),	/* [한국어] 환경이 세워졌는가 */
	KUNIT_CASE_FMT(test_bitops),	/* [한국어] 산술 매크로가 경계에서도 맞는가 */
	KUNIT_CASE_FMT(test_best_pgsize),	/* [한국어] 크기 선택이 참조 구현과 같은가 */
	KUNIT_CASE_FMT(test_pgsz_count),	/* [한국어] 연속 개수 계산이 맞는가 */
	KUNIT_CASE_FMT(test_table_ptr),	/* [한국어] 표 주소가 왕복하는가 */
	KUNIT_CASE_FMT(test_max_va),	/* [한국어] 표가 인스턴스보다 넓지 않은가 */
	KUNIT_CASE_FMT(test_table_radix),	/* [한국어] 단계들이 VA 비트를 빈틈없이 나눠 갖는가 */
	KUNIT_CASE_FMT(test_entry_possible_sizes),	/* [한국어] 크기 비트맵이 규약대로인가 */
	KUNIT_CASE_FMT(test_entry_oa),	/* [한국어] 잎 주소가 왕복하는가 */
	KUNIT_CASE_FMT(test_attr_from_entry),	/* [한국어] 권한이 왕복하는가 */
#ifdef pt_entry_is_write_dirty	/* [한국어] 형식이 더티 비트를 제공할 때만 */
	KUNIT_CASE_FMT(test_dirty),	/* [한국어] 더티 세 함수가 서로 맞는가 */
#endif
#ifdef pt_sw_bit	/* [한국어] 형식이 소프트웨어 비트를 제공할 때만 */
	KUNIT_CASE_FMT(test_sw_bit_leaf),	/* [한국어] 잎에서 그 비트가 다른 필드를 건드리지 않는가 */
	KUNIT_CASE_FMT(test_sw_bit_table),	/* [한국어] 표 항목에서도 마찬가지인가 */
#endif
	{},	/* [한국어] 목록의 끝 */
};

/*
 * [한국어]
 * pt_kunit_generic_pt_init - 시험 하나가 시작될 때 환경을 만든다
 *
 * @test: 시험 문맥.
 * @return: 0 성공, 음수면 실패.
 *
 * kunit 의 fixture 다. 시험마다 새 페이지 테이블을 만들어, 앞 시험이 남긴
 * 상태가 다음에 영향을 주지 않게 한다.
 */
static int pt_kunit_generic_pt_init(struct kunit *test)
{
	struct kunit_iommu_priv *priv;	/* [한국어] 시험 환경 */
	int ret;	/* [한국어] 결과 */

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);	/* [한국어] 시험이 끝나면 kunit 이 해제한다 */
	if (!priv)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 시험을 시작할 수 없다 */
	ret = pt_kunit_priv_init(test, priv);	/* [한국어] 표까지 만든다 */
	if (ret) {	/* [한국어] 실패면 */
		kunit_kfree(test, priv);	/* [한국어] 되돌리고 */
		return ret;	/* [한국어] 오류를 전한다 */
	}
	test->priv = priv;	/* [한국어] 시험 함수들이 여기서 환경을 꺼낸다 */
	return 0;	/* [한국어] 준비 완료 */
}

/*
 * [한국어]
 * pt_kunit_generic_pt_exit - 시험이 끝나면 환경을 해제한다
 *
 * @test: 시험 문맥.
 *
 * 표를 통째로 돌려준다. 건너뛴 시험은 priv 가 없을 수 있어 먼저 확인한다.
 */
static void pt_kunit_generic_pt_exit(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */

	if (!test->priv)	/* [한국어] 건너뛴 시험은 환경이 없을 수 있다 */
		return;	/* [한국어] 해제할 것이 없다 */

	pt_iommu_deinit(priv->iommu);	/* [한국어] 표를 통째로 돌려주고 */
	kunit_kfree(test, test->priv);	/* [한국어] 환경도 해제한다 */
}

static struct kunit_suite NS(generic_pt_suite) = {	/* [한국어] 형식별 이름으로 만들어지는 시험 묶음 */
	.name = __stringify(NS(fmt_test)),	/* [한국어] 결과에 형식 이름이 찍힌다 */
	.init = pt_kunit_generic_pt_init,	/* [한국어] 시험마다 새 표를 만든다 */
	.exit = pt_kunit_generic_pt_exit,	/* [한국어] 끝나면 해제한다 */
	.test_cases = generic_pt_test_cases,	/* [한국어] 돌릴 시험 목록 */
};
kunit_test_suites(&NS(generic_pt_suite));	/* [한국어] 모듈 적재 시 이 묶음이 실행된다 */
