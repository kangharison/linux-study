/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] IOMMU 진입점 쪽 kunit (kunit_iommu_pt.h)
 *
 * === 파일의 역할 ===
 * kunit_generic_pt.h 가 형식 API 를 직접 두드린다면, 이쪽은 코어의
 * iommu_map/iommu_unmap/iommu_iova_to_phys 를 통해 시험한다. 즉 실제
 * 드라이버가 지나는 경로를 그대로 탄다.
 *
 * 시험들이 노리는 것은 iommu_pt.h 의 어려운 구석들이다.
 *  - 최상위가 자라는 경로(test_increase_level).
 *  - 표가 있던 자리를 큰 페이지로 바꾸는 경로(test_map_table_to_oa).
 *  - 큰 페이지의 일부만 해제하라는 요청(test_unmap_split).
 *  - 무작위 겹침으로 온갖 조합을 때리는 경로(test_random_map).
 *
 * 마지막 둘(test_pgsize_boundary, test_mixed)은 실제로 보고된 버그를
 * 재현하는 회귀 시험이다. 원 주석에 그 메일 스레드 주소가 남아 있다.
 *
 * 누수 검사도 여기 있다. 시험 시작과 끝에서 커널의 페이지 테이블 메모리
 * 통계를 견주어, 표가 하나라도 남으면 실패로 잡는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c 를 GENERIC_PT_KUNIT 을 켜고 컴파일할 때 포함된다.
 * kunit_generic_pt.h 와 함께 한 모듈에 들어가, 형식마다 두 묶음의 시험이
 * 생긴다.
 *
 * 실행 컨텍스트: kunit 모듈, 프로세스 문맥.
 *
 * === 타 모듈과의 연결 ===
 * 위: kunit 프레임워크.
 * 아래: kunit_iommu.h(환경), <linux/iommu.h>(코어 진입점), pt_iter.h(결과
 *       확인을 위한 직접 순회), maple tree(겹침 추적).
 *
 * === 주요 함수/구조체 요약 ===
 * count_valids / count_valids_single: 표를 직접 훑어 유효 항목을 센다.
 *   연속 묶음은 하나로 센다.
 * check_iova: 매핑한 범위를 최소 페이지 단위로 되짚어 주소가 맞는지 본다.
 * test_increase_level: 최상위를 한 단계씩 끝까지 키운다.
 * test_map_table_to_oa: 표를 남긴 자리에 큰 페이지를 덮어 본다.
 * test_unmap_split: 큰 페이지의 앞부분만 해제하면 전체가 걷히는지.
 * test_random_map: maple tree 로 겹침을 추적하며 무작위로 때린다.
 * test_pgsize_boundary / test_mixed: 보고된 버그의 회귀 시험.
 */
#include "kunit_iommu.h"	/* [한국어] 가짜 IOMMU 환경 */
#include "pt_iter.h"	/* [한국어] 결과를 직접 확인하기 위한 순회 */
#include <linux/generic_pt/iommu.h>	/* [한국어] 이 계층의 공통 객체 */
#include <linux/iommu.h>	/* [한국어] 코어의 map/unmap/iova_to_phys */

/*
 * [한국어] kunit_generic_pt.h 에 정의된 매핑 도우미의 전방 선언.
 * 두 시험 파일이 한 모듈로 묶이므로 그쪽 정의를 그대로 쓴다.
 */
static void do_map(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		   pt_vaddr_t len);

/*
 * [한국어] 크기별 유효 항목 수를 세는 통.
 * 어느 크기의 페이지가 몇 개 만들어졌는지를 보면 매핑 코드가 크기를
 * 제대로 골랐는지 알 수 있다.
 */
struct count_valids {
	u64 per_size[PT_VADDR_MAX_LG2];
	/* [한국어] 크기(지수)별 유효 항목 수.
	 * 설정자: __count_valids 가 잎을 만날 때마다 올린다.
	 * 읽는 자: 시험들이 기대한 크기와 개수를 확인할 때.
	 * 값 범위: 각 칸이 그 크기의 항목 수. 연속 묶음은 하나로 센다.
	 * 동기화: 호출 스택에 있어 공유되지 않는다. */
};

/*
 * [한국어]
 * __count_valids - 표를 훑으며 유효 항목을 크기별로 세는 워커
 *
 * @range: 걷는 범위.
 * @arg: 셀 통.
 * @level: 현재 단계.
 * @table: 그 단계의 표.
 * @return: 늘 0.
 *
 * 표를 만나면 내려가고, 잎을 만나면 그 크기의 칸을 올린다. 순회가 연속
 * 묶음을 한 걸음에 지나므로 묶음은 저절로 하나로 세어진다.
 */
static int __count_valids(struct pt_range *range, void *arg, unsigned int level,
			  struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);	/* [한국어] 이 단계의 순회 상태 */
	struct count_valids *valids = arg;	/* [한국어] 셀 통 */

	for_each_pt_level_entry(&pts) {	/* [한국어] 이 단계의 항목들을 훑는다 */
		if (pts.type == PT_ENTRY_TABLE) {	/* [한국어] 아래 표면 */
			pt_descend(&pts, arg, __count_valids);	/* [한국어] 내려가서 세고 */
			continue;	/* [한국어] 다음 항목으로 */
		}
		if (pts.type == PT_ENTRY_OA) {	/* [한국어] 잎이면 */
			valids->per_size[pt_entry_oa_lg2sz(&pts)]++;	/* [한국어] 그 크기의 칸을 올린다 — 순회가 묶음을 한 걸음에 지나 하나로 세어진다 */
			continue;	/* [한국어] 다음 항목으로 */
		}
	}
	return 0;	/* [한국어] 실패할 수 없는 경로 */
}

/*
 * Number of valid table entries. This counts contiguous entries as a single
 * valid.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * count_valids - 유효 항목의 총 개수를 센다
 *
 * @test: 시험 문맥.
 * @return: 크기를 가리지 않은 총 개수.
 *
 * 해제 시험이 "0 이 되었는가"를 확인하는 데 주로 쓴다.
 */
static unsigned int count_valids(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	struct count_valids valids = {};	/* [한국어] 셀 통 */
	u64 total = 0;	/* [한국어] 합계 */
	unsigned int i;	/* [한국어] 크기 인덱스 */

	KUNIT_ASSERT_NO_ERRNO(test,	/* [한국어] 표를 직접 훑는다 */
			      pt_walk_range(&range, __count_valids, &valids));	/* [한국어] 코어를 거치지 않고 */

	for (i = 0; i != ARRAY_SIZE(valids.per_size); i++)	/* [한국어] 모든 크기의 */
		total += valids.per_size[i];	/* [한국어] 개수를 더한다 */
	return total;	/* [한국어] 유효 항목의 총 개수 */
}

/* Only a single page size is present, count the number of valid entries */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * count_valids_single - 한 크기만 쓰였는지 확인하며 그 개수를 센다
 *
 * @test: 시험 문맥.
 * @pgsz: 기대하는 페이지 크기.
 * @return: 그 크기의 개수.
 *
 * 다른 크기가 하나라도 있으면 그 자리에서 실패시킨다 — 매핑 코드가
 * 요청과 다른 크기를 골랐다는 뜻이기 때문이다.
 */
static unsigned int count_valids_single(struct kunit *test, pt_vaddr_t pgsz)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	struct count_valids valids = {};	/* [한국어] 셀 통 */
	u64 total = 0;	/* [한국어] 그 크기의 개수 */
	unsigned int i;	/* [한국어] 크기 인덱스 */

	KUNIT_ASSERT_NO_ERRNO(test,	/* [한국어] 표를 직접 훑는다 */
			      pt_walk_range(&range, __count_valids, &valids));	/* [한국어] 코어를 거치지 않고 */

	for (i = 0; i != ARRAY_SIZE(valids.per_size); i++) {	/* [한국어] 모든 크기를 보며 */
		if ((1ULL << i) == pgsz)	/* [한국어] 기대하는 크기면 */
			total = valids.per_size[i];	/* [한국어] 그 개수를 기억하고 */
		else
			KUNIT_ASSERT_EQ(test, valids.per_size[i], 0);	/* [한국어] 다른 크기가 있으면 매핑이 요청과 다른 크기를 골랐다는 뜻이다 */
	}
	return total;	/* [한국어] 그 크기의 개수 */
}

/*
 * [한국어]
 * do_unmap - 시험용 해제를 하고 길이가 맞는지 확인한다
 *
 * @test: 시험 문맥.
 * @va: 해제할 주소.
 * @len: 길이.
 *
 * 반환값이 요청과 정확히 같아야 한다. 큰 페이지가 통째로 걷혀 더 큰 값이
 * 나오는 경우는 이 도우미를 쓰지 않고 따로 확인한다.
 */
static void do_unmap(struct kunit *test, pt_vaddr_t va, pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	size_t ret;	/* [한국어] 걷어낸 바이트 수 */

	ret = iommu_unmap(&priv->domain, va, len);	/* [한국어] 코어의 해제 경로를 그대로 탄다 */
	KUNIT_ASSERT_EQ(test, ret, len);	/* [한국어] 요청과 정확히 같아야 한다 */
}

/*
 * [한국어]
 * check_iova - 매핑한 범위를 되짚어 주소가 맞는지 본다
 *
 * @test: 시험 문맥.
 * @va: 확인할 시작 주소.
 * @pa: 대응해야 할 물리 주소.
 * @len: 길이.
 *
 * 최소 페이지 단위로 하나씩 물어본다. 큰 페이지로 매핑했더라도 그 안의
 * 어느 주소를 물어도 정확한 물리 주소가 나와야 하는데, 그것이
 * pt_entry_oa_exact 의 오프셋 처리를 검증한다.
 *
 * 처음 틀린 지점에서 반복을 멈춘다 — 그 뒤로는 전부 틀릴 것이라 로그만
 * 길어진다.
 */
static void check_iova(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		       pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	pt_vaddr_t pfn = log2_div(va, priv->smallest_pgsz_lg2);	/* [한국어] 최소 페이지 단위의 시작 */
	pt_vaddr_t end_pfn = pfn + log2_div(len, priv->smallest_pgsz_lg2);	/* [한국어] 그 끝 */

	for (; pfn != end_pfn; pfn++) {	/* [한국어] 하나씩 물어본다 */
		phys_addr_t res = iommu_iova_to_phys(&priv->domain,	/* [한국어] 코어의 조회 경로로 */
						     pfn * priv->smallest_pgsz);	/* [한국어] 그 주소를 */

		KUNIT_ASSERT_EQ(test, res, (phys_addr_t)pa);	/* [한국어] 큰 페이지 안의 어느 주소를 물어도 정확해야 한다 */
		if (res != pa)	/* [한국어] 한 번 틀리면 */
			break;	/* [한국어] 뒤로는 전부 틀리므로 멈춘다 */
		pa += priv->smallest_pgsz;	/* [한국어] 다음 페이지의 물리 주소 */
	}
}

/*
 * [한국어]
 * test_increase_level - 최상위 단계를 끝까지 키워 본다
 *
 * @test: 시험 문맥.
 *
 * 지금 표가 덮는 범위 바로 바깥에 매핑을 만들면 최상위가 한 단계 자라야
 * 한다. 그것을 최대 단계에 닿을 때까지 되풀이한다.
 *
 * 매핑 위치를 두 갈래로 고르는 이유: 아래에서 자라는 형식은 현재 범위의
 * 끝 다음을, 위에서 자라는 형식(부호 확장)은 시작 앞을 건드려야 한다.
 *
 * 자랄 수 없는 형식이나 32비트 환경에서는 건너뛴다.
 */
static void test_increase_level(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_common *common = priv->common;	/* [한국어] 페이지 테이블 상태 */

	if (!pt_feature(common, PT_FEAT_DYNAMIC_TOP))	/* [한국어] 자랄 수 없는 형식이면 */
		kunit_skip(test, "PT_FEAT_DYNAMIC_TOP not set for this format");	/* [한국어] 시험할 것이 없다 */

	if (IS_32BIT)	/* [한국어] 32비트에서는 넓은 주소를 만들 수 없어 */
		kunit_skip(test, "Unable to test on 32bit");	/* [한국어] 건너뛴다 */

	KUNIT_ASSERT_GT(test, common->max_vasz_lg2,	/* [한국어] 아직 자랄 여지가 */
			pt_top_range(common).max_vasz_lg2);	/* [한국어] 남아 있어야 한다 */

	/* Add every possible level to the max */
	while (common->max_vasz_lg2 != pt_top_range(common).max_vasz_lg2) {	/* [한국어] (원 주석: 가능한 모든 단계를 최대까지 더한다) */
		struct pt_range top_range = pt_top_range(common);	/* [한국어] 자라기 전의 범위 */

		if (top_range.va == 0)	/* [한국어] 아래에서 위로 뻗는 형식이면 */
			do_map(test, top_range.last_va + 1, 0,	/* [한국어] 현재 범위 바로 다음을 */
			       priv->smallest_pgsz);	/* [한국어] 건드려 자라게 한다 */
		else
			do_map(test, top_range.va - priv->smallest_pgsz, 0,	/* [한국어] 위에서 아래로 뻗는 형식이면 시작 앞을 */
			       priv->smallest_pgsz);	/* [한국어] 건드린다 */

		KUNIT_ASSERT_EQ(test, pt_top_range(common).top_level,	/* [한국어] 정확히 한 단계만 */
				top_range.top_level + 1);	/* [한국어] 자랐어야 한다 */
		KUNIT_ASSERT_GE(test, common->max_vasz_lg2,	/* [한국어] 자란 뒤에도 인스턴스 폭을 */
				pt_top_range(common).max_vasz_lg2);	/* [한국어] 넘지 않아야 한다 */
	}
}

/*
 * [한국어]
 * test_map_simple - 보고된 모든 페이지 크기를 하나씩 만들고 지운다
 *
 * @test: 시험 문맥.
 *
 * 세 단계로 확인한다. 크기마다 정렬해 매핑하고 되짚어 보고, 표를 직접
 * 훑어 그 크기가 정확히 하나씩 생겼는지 세고, 다시 지워 아무것도 남지
 * 않았는지 본다.
 *
 * 두 번째가 이 시험의 핵심이다. 매핑 코드가 요청한 크기 대신 작은 것을
 * 여러 개 놓았다면 개수가 맞지 않는다.
 *
 * 2GB 이하만 되짚는 이유: check_iova 가 최소 페이지 단위로 도는데, 그보다
 * 크면 반복이 지나치게 길어진다.
 */
static void test_map_simple(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	struct count_valids valids = {};	/* [한국어] 셀 통 */
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap;	/* [한국어] 안전하게 쓸 수 있는 크기들 */
	unsigned int pgsz_lg2;	/* [한국어] 시도할 크기 */
	pt_vaddr_t cur_va;	/* [한국어] 다음 매핑을 놓을 주소 */

	/* Map every reported page size */
	cur_va = range.va + priv->smallest_pgsz * 256;	/* [한국어] (원 주석: 보고된 모든 페이지 크기를 매핑한다) 앞쪽은 비워 둔다 */
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {	/* [한국어] 크기를 하나씩 */
		pt_oaddr_t paddr = log2_set_mod(priv->test_oa, 0, pgsz_lg2);	/* [한국어] 그 크기에 정렬한 물리 주소 */
		u64 len = log2_to_int(pgsz_lg2);	/* [한국어] 그 크기 */

		if (!(pgsize_bitmap & len))	/* [한국어] 쓸 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */

		cur_va = ALIGN(cur_va, len);	/* [한국어] 가상 주소도 정렬해야 그 크기가 쓰인다 */
		do_map(test, cur_va, paddr, len);	/* [한국어] 매핑하고 */
		if (len <= SZ_2G)	/* [한국어] 너무 크면 되짚기가 지나치게 길어진다 */
			check_iova(test, cur_va, paddr, len);	/* [한국어] 되짚어 확인한다 */
		cur_va += len;	/* [한국어] 다음 자리로 */
	}

	/* The read interface reports that every page size was created */
	range = pt_top_range(priv->common);	/* [한국어] (원 주석: 읽기 인터페이스가 모든 크기가 만들어졌다고 보고한다) */
	KUNIT_ASSERT_NO_ERRNO(test,	/* [한국어] 표를 직접 훑어 */
			      pt_walk_range(&range, __count_valids, &valids));	/* [한국어] 크기별로 센다 */
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {	/* [한국어] 모든 크기에 대해 */
		if (pgsize_bitmap & (1ULL << pgsz_lg2))	/* [한국어] 매핑한 크기면 */
			KUNIT_ASSERT_EQ(test, valids.per_size[pgsz_lg2], 1);	/* [한국어] 정확히 하나 — 작은 것 여럿으로 쪼갰으면 여기서 걸린다 */
		else
			KUNIT_ASSERT_EQ(test, valids.per_size[pgsz_lg2], 0);	/* [한국어] 매핑하지 않은 크기는 없어야 한다 */
	}

	/* Unmap works */
	range = pt_top_range(priv->common);	/* [한국어] (원 주석: 해제가 동작한다) */
	cur_va = range.va + priv->smallest_pgsz * 256;	/* [한국어] 같은 자리에서 다시 */
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {	/* [한국어] 매핑할 때와 같은 순서로 크기를 하나씩 */
		u64 len = log2_to_int(pgsz_lg2);	/* [한국어] 그 크기 */

		if (!(pgsize_bitmap & len))	/* [한국어] 쓰지 않은 크기면 */
			continue;	/* [한국어] 건너뛴다 */
		cur_va = ALIGN(cur_va, len);	/* [한국어] 매핑할 때와 같은 자리로 */
		do_unmap(test, cur_va, len);	/* [한국어] 지우고 */
		cur_va += len;	/* [한국어] 다음으로 */
	}
	KUNIT_ASSERT_EQ(test, count_valids(test), 0);	/* [한국어] 아무것도 남지 않아야 한다 */
}

/*
 * Test to convert a table pointer into an OA by mapping something small,
 * unmapping it so as to leave behind a table pointer, then mapping something
 * larger that will convert the table into an OA.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_map_table_to_oa - 표가 있던 자리를 큰 페이지로 바꾼다
 *
 * @test: 시험 문맥.
 *
 * iommu_pt.h 의 clear_contig 경로를 노린다. 작은 페이지를 촘촘히 깔아
 * 표를 만들어 두고, 그 위에 큰 페이지 하나를 놓으면 그 표들이 걷히고
 * 큰 항목 하나로 바뀌어야 한다.
 *
 * 해제도 두 갈래로 나눠 시험한다. 큰 페이지 하나면 통째로 지우고, 아니면
 * 절반을 한 번에 지운 뒤 나머지를 조각으로 지운다 — 부분 해제와 조각
 * 해제가 모두 표 정리로 이어지는지 본다.
 *
 * 크기를 제한하는 이유: 최대 페이지가 너무 크면 그 안을 작은 페이지로
 * 채우는 반복이 끝나지 않는다.
 */
static void test_map_table_to_oa(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	pt_vaddr_t limited_pgbitmap =	/* [한국어] 반복이 끝나지 않을 만큼 큰 크기는 */
		priv->info.pgsize_bitmap % (IS_32BIT ? SZ_2G : SZ_16G);	/* [한국어] 잘라 낸다 */
	struct pt_range range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	unsigned int pgsz_lg2;	/* [한국어] 시도할 작은 크기 */
	pt_vaddr_t max_pgsize;	/* [한국어] 채울 큰 페이지의 크기 */
	pt_vaddr_t cur_va;	/* [한국어] 시험할 자리 */

	max_pgsize = 1ULL << (vafls(limited_pgbitmap) - 1);	/* [한국어] 남은 것 중 가장 큰 크기 */
	KUNIT_ASSERT_TRUE(test, priv->info.pgsize_bitmap & max_pgsize);	/* [한국어] 실제로 만들 수 있는 크기여야 한다 */

	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {	/* [한국어] 작은 크기를 하나씩 */
		pt_oaddr_t paddr = log2_set_mod(priv->test_oa, 0, pgsz_lg2);	/* [한국어] 그 크기에 정렬한 물리 주소 */
		u64 len = log2_to_int(pgsz_lg2);	/* [한국어] 그 크기 */
		pt_vaddr_t offset;	/* [한국어] 큰 페이지 안에서의 위치 */

		if (!(priv->info.pgsize_bitmap & len))	/* [한국어] 쓸 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */
		if (len > max_pgsize)	/* [한국어] 큰 페이지보다 커지면 */
			break;	/* [한국어] 더 볼 것이 없다 */

		cur_va = ALIGN(range.va + priv->smallest_pgsz * 256,	/* [한국어] 큰 페이지 크기에 */
			       max_pgsize);	/* [한국어] 정렬한 자리 */
		for (offset = 0; offset != max_pgsize; offset += len)	/* [한국어] 작은 페이지로 */
			do_map(test, cur_va + offset, paddr + offset, len);	/* [한국어] 그 범위를 촘촘히 채워 표를 만든다 */
		check_iova(test, cur_va, paddr, max_pgsize);	/* [한국어] 되짚어 확인하고 */
		KUNIT_ASSERT_EQ(test, count_valids_single(test, len),	/* [한국어] 작은 페이지만 */
				log2_div(max_pgsize, pgsz_lg2));	/* [한국어] 기대한 개수만큼 있어야 한다 */

		if (len == max_pgsize) {	/* [한국어] 작은 크기가 곧 큰 크기면 */
			do_unmap(test, cur_va, max_pgsize);	/* [한국어] 통째로 지운다 */
		} else {
			do_unmap(test, cur_va, max_pgsize / 2);	/* [한국어] 절반을 한 번에 지우고 */
			for (offset = max_pgsize / 2; offset != max_pgsize;	/* [한국어] 나머지는 */
			     offset += len)	/* [한국어] 조각으로 */
				do_unmap(test, cur_va + offset, len);	/* [한국어] 하나씩 지운다 */
		}

		KUNIT_ASSERT_EQ(test, count_valids(test), 0);	/* [한국어] 두 경로 모두 표까지 정리되어야 한다 */
	}
}

/*
 * Test unmapping a small page at the start of a large page. This always unmaps
 * the large page.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_unmap_split - 큰 페이지의 앞부분만 해제하면 전체가 걷히는지 본다
 *
 * @test: 시험 문맥.
 *
 * IOMMU API 는 큰 페이지를 쪼개 주지 않는다. 그래서 작은 길이를 해제하라고
 * 해도 그 페이지 전체가 걷히고, 반환값은 요청보다 큰 값이 되어야 한다.
 *
 * 뒤의 두 번째 확인이 중요하다. 큰 페이지 두 개를 나란히 놓고 앞의 것만
 * 지웠을 때 뒤의 것까지 걷히면 안 된다 — 해제가 요청 범위를 넘어 계속
 * 진행하는 버그를 잡는다.
 *
 * 크기가 두 종류 이상 없으면 시험할 것이 없어 건너뛴다.
 */
static void test_unmap_split(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range top_range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap;	/* [한국어] 안전하게 쓸 수 있는 크기들 */
	unsigned int pgsz_lg2;	/* [한국어] 작은 크기 */
	unsigned int count = 0;	/* [한국어] 실제로 시험한 조합 수 */

	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {	/* [한국어] 작은 크기를 하나씩 */
		pt_vaddr_t base_len = log2_to_int(pgsz_lg2);	/* [한국어] 해제를 요청할 길이 */
		unsigned int next_pgsz_lg2;	/* [한국어] 그보다 큰 크기 */

		if (!(pgsize_bitmap & base_len))	/* [한국어] 쓸 수 없는 크기면 */
			continue;	/* [한국어] 건너뛴다 */

		for (next_pgsz_lg2 = pgsz_lg2 + 1;	/* [한국어] 그보다 큰 크기를 */
		     next_pgsz_lg2 != PT_VADDR_MAX_LG2; next_pgsz_lg2++) {	/* [한국어] 하나씩 */
			pt_vaddr_t next_len = log2_to_int(next_pgsz_lg2);	/* [한국어] 실제로 매핑할 크기 */
			pt_vaddr_t vaddr = top_range.va;	/* [한국어] 범위의 시작에서 */
			pt_oaddr_t paddr = 0;	/* [한국어] 물리 주소 0 으로 */
			size_t gnmapped;	/* [한국어] 실제로 걷어낸 바이트 */

			if (!(pgsize_bitmap & next_len))	/* [한국어] 쓸 수 없는 크기면 */
				continue;	/* [한국어] 건너뛴다 */

			do_map(test, vaddr, paddr, next_len);	/* [한국어] 큰 페이지를 놓고 */
			gnmapped = iommu_unmap(&priv->domain, vaddr, base_len);	/* [한국어] 작은 길이만 해제하면 */
			KUNIT_ASSERT_EQ(test, gnmapped, next_len);	/* [한국어] 큰 페이지 전체가 걷혀야 한다 */

			/* Make sure unmap doesn't keep going */
			do_map(test, vaddr, paddr, next_len);	/* [한국어] (원 주석: 해제가 계속 진행되지 않는지 확인한다) */
			do_map(test, vaddr + next_len, paddr, next_len);	/* [한국어] 큰 페이지 두 개를 나란히 */
			gnmapped = iommu_unmap(&priv->domain, vaddr, base_len);	/* [한국어] 앞의 것만 해제하면 */
			KUNIT_ASSERT_EQ(test, gnmapped, next_len);	/* [한국어] 하나만 걷혀야 하고 */
			gnmapped = iommu_unmap(&priv->domain, vaddr + next_len,	/* [한국어] 뒤의 것은 */
					       next_len);	/* [한국어] 그대로 남아 있어야 한다 */
			KUNIT_ASSERT_EQ(test, gnmapped, next_len);	/* [한국어] 지금 지워야 걷힌다 */

			count++;	/* [한국어] 시험한 조합 하나 */
		}
	}

	if (count == 0)	/* [한국어] 크기가 두 종류 이상 없으면 */
		kunit_skip(test, "Test needs two page sizes");	/* [한국어] 시험할 것이 없다 */
}

/*
 * [한국어]
 * unmap_collisions - 겹치는 기존 매핑을 찾아 지운다
 *
 * @test: 시험 문맥.
 * @mt: 지금까지 만든 매핑을 추적하는 maple tree.
 * @start: 겹침을 볼 범위의 시작.
 * @last: 그 끝.
 *
 * 무작위 시험의 도우미다. 새 매핑이 기존 것과 겹치면 코어가 -EADDRINUSE
 * 를 주는데, 그때 이 함수로 겹친 것들을 걷어내고 다시 시도한다.
 *
 * 락을 놓았다 다시 잡는 순서가 눈에 띈다. mas_erase 로 나무에서 뺀 뒤
 * 락을 풀고 실제 해제를 하는데, 그 사이 나무가 바뀔 수 있어 mas_pause 로
 * 순회 상태를 다시 세운다.
 */
static void unmap_collisions(struct kunit *test, struct maple_tree *mt,
			     pt_vaddr_t start, pt_vaddr_t last)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	MA_STATE(mas, mt, start, last);	/* [한국어] maple tree 순회 상태 */
	void *entry;	/* [한국어] 순회가 내어 주는 항목 */

	mtree_lock(mt);	/* [한국어] 나무를 훑는 동안 */
	mas_for_each(&mas, entry, last) {	/* [한국어] 겹치는 매핑을 하나씩 */
		pt_vaddr_t mas_start = mas.index;	/* [한국어] 그 매핑의 시작 */
		pt_vaddr_t len = (mas.last - mas_start) + 1;	/* [한국어] 길이 */
		pt_oaddr_t paddr;	/* [한국어] 그때 쓴 물리 주소 */

		mas_erase(&mas);	/* [한국어] 나무에서 먼저 빼고 */
		mas_pause(&mas);	/* [한국어] 락을 놓을 것이므로 순회 상태를 멈춘다 */
		mtree_unlock(mt);	/* [한국어] 실제 해제는 락 밖에서 */

		paddr = oalog2_mod(mas_start, priv->common->max_oasz_lg2);	/* [한국어] 매핑할 때와 같은 규칙으로 되계산 */
		check_iova(test, mas_start, paddr, len);	/* [한국어] 지우기 전에 값이 맞는지 확인하고 */
		do_unmap(test, mas_start, len);	/* [한국어] 걷어낸다 */
		mtree_lock(mt);	/* [한국어] 다시 잡고 순회를 이어 간다 */
	}
	mtree_unlock(mt);	/* [한국어] 모두 처리했다 */
}

/*
 * [한국어]
 * clamp_range - 무작위 시험이 쓸 범위를 줄인다
 *
 * @test: 시험 문맥.
 * @range: 줄일 범위.
 *
 * 두 가지 이유로 줄인다. 범위가 너무 넓으면 무작위 주소들이 서로 겹치지
 * 않아 정작 시험하려는 충돌 경로가 돌지 않고, maple tree 가 예약해 둔
 * 낮은 주소는 쓸 수 없다.
 */
static void clamp_range(struct kunit *test, struct pt_range *range)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */

	if (range->last_va - range->va > SZ_1G)	/* [한국어] 너무 넓으면 무작위 주소가 겹치지 않아 */
		range->last_va = range->va + SZ_1G;	/* [한국어] 충돌 경로가 돌지 않는다 */
	KUNIT_ASSERT_NE(test, range->last_va, PT_VADDR_MAX);	/* [한국어] 끝값이 최대면 아래 계산이 넘친다 */
	if (range->va <= MAPLE_RESERVED_RANGE)	/* [한국어] maple tree 가 예약한 낮은 주소는 */
		range->va =	/* [한국어] 쓸 수 없으므로 */
			ALIGN(MAPLE_RESERVED_RANGE, priv->smallest_pgsz);	/* [한국어] 그 위로 옮긴다 */
}

/*
 * Randomly map and unmap ranges that can large physical pages. If a random
 * range overlaps with existing ranges then unmap them. This hits all the
 * special cases.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * test_random_map - 무작위로 매핑하고 지우며 온갖 조합을 때린다
 *
 * @test: 시험 문맥.
 *
 * 이 파일에서 가장 넓게 훑는 시험이다. 무작위 범위를 천 번 매핑하되,
 * 기존 것과 겹치면 그것을 걷어내고 다시 시도한다. 그 과정에서 표 생성,
 * 큰 페이지 병합, 부분 해제, 표 정리가 모두 섞여 돈다.
 *
 * maple tree 가 "지금 무엇이 매핑되어 있는가"의 정답을 들고 있어, 매번
 * 되짚어 확인할 수 있다.
 *
 * 부호 확장 형식이면 위·아래 절반을 무작위로 오간다 — 두 절반의 경계
 * 처리가 이 시험에서 걸린다.
 *
 * 마지막에 전부 걷어내고 유효 항목이 0 인지 확인해 누수를 잡는다.
 */
static void test_random_map(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range upper_range = pt_upper_range(priv->common);	/* [한국어] 부호 확장 형식의 위쪽 절반 */
	struct pt_range top_range = pt_top_range(priv->common);	/* [한국어] 아래쪽 절반(또는 전체) */
	struct maple_tree mt;	/* [한국어] 지금 무엇이 매핑되어 있는지의 정답 */
	unsigned int iter;	/* [한국어] 반복 횟수 */

	mt_init(&mt);	/* [한국어] 비어 있는 상태로 시작 */

	/*
	 * Shrink the range so randomization is more likely to have
	 * intersections
	 */
	clamp_range(test, &top_range);	/* [한국어] (원 주석: 무작위 주소가 겹치기 쉽도록 범위를 줄인다) */
	clamp_range(test, &upper_range);	/* [한국어] 위쪽 절반도 */

	for (iter = 0; iter != 1000; iter++) {	/* [한국어] 천 번 때린다 */
		struct pt_range *range = &top_range;	/* [한국어] 기본은 아래쪽 절반 */
		pt_oaddr_t paddr;	/* [한국어] 매핑할 물리 주소 */
		pt_vaddr_t start;	/* [한국어] 범위의 시작 */
		pt_vaddr_t end;	/* [한국어] 그 끝 */
		int ret;	/* [한국어] 매핑 결과 */

		if (pt_feature(priv->common, PT_FEAT_SIGN_EXTEND) &&	/* [한국어] 부호 확장 형식이고 */
		    ULONG_MAX >= PT_VADDR_MAX && get_random_u32_inclusive(0, 1))	/* [한국어] 주소 타입이 충분히 넓으면 */
			range = &upper_range;	/* [한국어] 절반씩 번갈아 — 경계 처리가 여기서 걸린다 */

		start = get_random_u32_below(	/* [한국어] 무작위 시작 */
			min(U32_MAX, range->last_va - range->va));	/* [한국어] 범위 안에서 */
		end = get_random_u32_below(	/* [한국어] 무작위 끝 */
			min(U32_MAX, range->last_va - start));	/* [한국어] 시작 뒤에서 */

		start = ALIGN_DOWN(start, priv->smallest_pgsz);	/* [한국어] 최소 페이지에 정렬 */
		end = ALIGN(end, priv->smallest_pgsz);	/* [한국어] 끝도 정렬 */
		start += range->va;	/* [한국어] 범위의 기준 주소를 더하고 */
		end += start;	/* [한국어] 길이를 끝 주소로 */
		if (start < range->va || end > range->last_va + 1 ||	/* [한국어] 범위를 벗어났거나 */
		    start >= end)	/* [한국어] 길이가 없으면 */
			continue;	/* [한국어] 다시 뽑는다 */

		/* Try overmapping to test the failure handling */
		paddr = oalog2_mod(start, priv->common->max_oasz_lg2);	/* [한국어] (원 주석: 실패 처리를 시험하려고 일부러 겹쳐 매핑해 본다) */
		ret = iommu_map(&priv->domain, start, paddr, end - start,	/* [한국어] 매핑을 시도하고 */
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);	/* [한국어] 읽기·쓰기로 */
		if (ret) {	/* [한국어] 실패했으면 */
			KUNIT_ASSERT_EQ(test, ret, -EADDRINUSE);	/* [한국어] 겹침 말고 다른 이유일 수 없다 */
			unmap_collisions(test, &mt, start, end - 1);	/* [한국어] 겹친 것을 걷어내고 */
			do_map(test, start, paddr, end - start);	/* [한국어] 다시 시도하면 성공해야 한다 */
		}

		KUNIT_ASSERT_NO_ERRNO_FN(test, "mtree_insert_range",	/* [한국어] 정답 나무에도 */
					 mtree_insert_range(&mt, start, end - 1,	/* [한국어] 같은 범위를 */
							    XA_ZERO_ENTRY,	/* [한국어] 자리 표시만 */
							    GFP_KERNEL));	/* [한국어] 넣어 둔다 */

		check_iova(test, start, paddr, end - start);	/* [한국어] 되짚어 확인하고 */
		if (iter % 100)	/* [한국어] 오래 도는 반복이라 */
			cond_resched();	/* [한국어] 중간중간 양보한다 */
	}

	unmap_collisions(test, &mt, 0, PT_VADDR_MAX);	/* [한국어] 마지막에 전부 걷어내고 */
	KUNIT_ASSERT_EQ(test, count_valids(test), 0);	/* [한국어] 아무것도 남지 않아야 한다 */

	mtree_destroy(&mt);	/* [한국어] 정답 나무도 해제 */
}

/* See https://lore.kernel.org/r/b9b18a03-63a2-4065-a27e-d92dd5c860bc@amd.com */
/*
 * [한국어]
 * (위 영어 주석의 메일 스레드가 출처)
 * test_pgsize_boundary - 보고된 페이지 크기 경계 버그의 회귀 시험
 *
 * @test: 시험 문맥.
 *
 * 특정 주소와 길이 조합에서 매핑이 잘못 나뉘던 버그다. 값을 그대로 재현해
 * 두어, 크기 선택 코드를 고칠 때 같은 실수를 다시 하지 않게 한다.
 *
 * 그 주소를 담을 수 없는 형식에서는 건너뛴다.
 */
static void test_pgsize_boundary(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range top_range = pt_top_range(priv->common);	/* [한국어] 표 전체 */

	if (top_range.va != 0 || top_range.last_va < 0xfef9ffff ||	/* [한국어] 그 주소를 담을 수 없거나 */
	    priv->smallest_pgsz != SZ_4K)	/* [한국어] 최소 페이지가 다르면 */
		kunit_skip(test, "Format does not have the required range");	/* [한국어] 재현할 수 없다 */

	do_map(test, 0xfef80000, 0x208b95d000, 0xfef9ffff - 0xfef80000 + 1);	/* [한국어] 보고된 값 그대로 — 여기서 매핑이 잘못 나뉘었다 */
}

/* See https://lore.kernel.org/r/20250826143816.38686-1-eugkoira@amazon.com */
/*
 * [한국어]
 * (위 영어 주석의 메일 스레드가 출처)
 * test_mixed - 여러 크기가 섞이는 범위의 회귀 시험
 *
 * @test: 시험 문맥.
 *
 * 한 범위 안에서 2MB 와 1GB 가 번갈아 쓰여야 하는 경우다. 원 주석의
 * 주석이 기대하는 배치를 적어 두었다 — 2MB 14개, 1GB 3개, 2MB 3개.
 *
 * 그 개수가 맞아야 크기 선택이 경계에서 옳게 바뀐 것이다. 하나라도
 * 어긋나면 pt_pgsz_count 나 __map_range_leaf 의 크기 전환이 틀린 것이다.
 */
static void test_mixed(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */
	struct pt_range top_range = pt_top_range(priv->common);	/* [한국어] 표 전체 */
	u64 start = 0x3fe400ULL << 12;	/* [한국어] 보고된 시작 주소 */
	u64 end = 0x4c0600ULL << 12;	/* [한국어] 그 끝 */
	pt_vaddr_t len = end - start;	/* [한국어] 길이 */
	pt_oaddr_t oa = start;	/* [한국어] 물리 주소도 같은 값으로 */

	if (top_range.last_va <= start || sizeof(unsigned long) == 4)	/* [한국어] 그 주소를 담을 수 없으면 */
		kunit_skip(test, "range is too small");	/* [한국어] 재현할 수 없다 */
	if ((priv->safe_pgsize_bitmap & GENMASK(30, 21)) != (BIT(30) | BIT(21)))	/* [한국어] 2MB 와 1GB 를 모두 쓸 수 있어야 */
		kunit_skip(test, "incompatible psize");	/* [한국어] 섞이는 상황이 만들어진다 */

	do_map(test, start, oa, len);	/* [한국어] 한 번에 매핑하면 */
	/* 14 2M, 3 1G, 3 2M */
	KUNIT_ASSERT_EQ(test, count_valids(test), 20);	/* [한국어] (원 주석: 2MB 14개, 1GB 3개, 2MB 3개) 크기 전환이 옳으면 20개다 */
	check_iova(test, start, oa, len);	/* [한국어] 되짚어도 맞아야 한다 */
}

static struct kunit_case iommu_test_cases[] = {	/* [한국어] 이 형식에 대해 돌릴 시험 목록 */
	KUNIT_CASE_FMT(test_increase_level),	/* [한국어] 최상위가 자라는가 */
	KUNIT_CASE_FMT(test_map_simple),	/* [한국어] 모든 크기가 하나씩 만들어지는가 */
	KUNIT_CASE_FMT(test_map_table_to_oa),	/* [한국어] 표 자리를 큰 페이지로 바꿀 수 있는가 */
	KUNIT_CASE_FMT(test_unmap_split),	/* [한국어] 큰 페이지의 부분 해제가 전체를 걷는가 */
	KUNIT_CASE_FMT(test_random_map),	/* [한국어] 무작위 겹침으로 온갖 조합을 */
	KUNIT_CASE_FMT(test_pgsize_boundary),	/* [한국어] 보고된 경계 버그의 회귀 */
	KUNIT_CASE_FMT(test_mixed),	/* [한국어] 크기가 섞이는 경우의 회귀 */
	{},	/* [한국어] 목록의 끝 */
};

/*
 * [한국어]
 * pt_kunit_iommu_init - 시험 하나가 시작될 때 환경을 만든다
 *
 * @test: 시험 문맥.
 * @return: 0 성공, 음수면 실패.
 *
 * kunit_generic_pt.h 쪽과 거의 같되, 페이지 테이블 메모리 통계를 미리
 * 기록해 둔다 — 끝에서 누수를 잡기 위해서다.
 */
static int pt_kunit_iommu_init(struct kunit *test)
{
	struct kunit_iommu_priv *priv;	/* [한국어] 시험 환경 */
	int ret;	/* [한국어] 결과 */

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);	/* [한국어] 시험이 끝나면 kunit 이 해제한다 */
	if (!priv)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 시험을 시작할 수 없다 */

	priv->orig_nr_secondary_pagetable =	/* [한국어] 끝에서 누수를 잡기 위해 */
		global_node_page_state(NR_SECONDARY_PAGETABLE);	/* [한국어] 시작 시점의 통계를 기록한다 */
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
 * pt_kunit_iommu_exit - 시험이 끝나면 해제하고 누수를 확인한다
 *
 * @test: 시험 문맥.
 *
 * 표를 모두 돌려준 뒤 통계가 시작값으로 돌아왔는지 본다. 원 주석이 전제를
 * 밝힌다 — kunit 이 격리되어 돌고 다른 곳에서 2차 페이지 테이블을 쓰지
 * 않는다고 가정한다.
 */
static void pt_kunit_iommu_exit(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;	/* [한국어] 시험 환경 */

	if (!test->priv)	/* [한국어] 건너뛴 시험은 환경이 없을 수 있다 */
		return;	/* [한국어] 해제할 것이 없다 */

	pt_iommu_deinit(priv->iommu);	/* [한국어] 표를 통째로 돌려주고 */
	/*
	 * Look for memory leaks, assumes kunit is running isolated and nothing
	 * else is using secondary page tables.
	 */
	KUNIT_ASSERT_EQ(test, priv->orig_nr_secondary_pagetable,	/* [한국어] (원 주석: 누수를 찾는다 — kunit 이 격리되어 돈다고 가정한다) */
			global_node_page_state(NR_SECONDARY_PAGETABLE));	/* [한국어] 시작값으로 돌아왔어야 한다 */
	kunit_kfree(test, test->priv);	/* [한국어] 환경도 해제 */
}

static struct kunit_suite NS(iommu_suite) = {	/* [한국어] 형식별 이름으로 만들어지는 시험 묶음 */
	.name = __stringify(NS(iommu_test)),	/* [한국어] 결과에 형식 이름이 찍힌다 */
	.init = pt_kunit_iommu_init,	/* [한국어] 시험마다 새 표를 만든다 */
	.exit = pt_kunit_iommu_exit,	/* [한국어] 끝나면 해제하고 누수를 확인한다 */
	.test_cases = iommu_test_cases,	/* [한국어] 돌릴 시험 목록 */
};
kunit_test_suites(&NS(iommu_suite));	/* [한국어] 모듈 적재 시 이 묶음이 실행된다 */

MODULE_LICENSE("GPL");	/* [한국어] 라이선스 선언 */
MODULE_DESCRIPTION("Kunit for generic page table");	/* [한국어] 모듈 설명 */
MODULE_IMPORT_NS("GENERIC_PT_IOMMU");	/* [한국어] 형식별 진입점 심볼을 가져온다 */
