// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] ARM LPAE 페이지 테이블 구현의 단위 시험 (io-pgtable-arm-selftests.c)
 *
 * === 파일의 역할 ===
 * io-pgtable 의 ARM LPAE 구현이 매핑을 제대로 걸고 푸는지 하드웨어 없이
 * 검증하는 KUnit 시험이다. 실제 IOMMU 는 전혀 건드리지 않고, 페이지
 * 테이블을 메모리에 짓고 그 내용을 소프트웨어로 되짚어 확인한다.
 *
 * 검증의 뼈대는 "매핑을 걸고 그 주소를 되물어 본다"이다. iova 와 물리
 * 주소를 같은 값으로 매핑해 두면, 되물어 본 답이 원래 주소와 같아야
 * 한다. 그 단순한 성질 하나로 테이블 걷기·항목 짓기·블록 분할이 모두
 * 검증된다.
 *
 * 여러 설정 조합을 훑는 것이 이 시험의 값어치다. 알갱이 크기 세 가지와
 * 주소 폭 여섯 가지를 곱하고, 입력 폭이 출력 폭을 넘지 않는 조합만
 * 골라 돌린다. 그 조합마다 표의 단계 수와 시작 단계가 달라져, 손으로는
 * 다 짚어 보기 어려운 구석 사례들이 드러난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 시험은 이렇게 돈다:
 *
 *   KUnit → arm_lpae_do_selftests()
 *     → 설정 조합마다 arm_lpae_run_tests()
 *       → alloc_io_pgtable_ops()   ← 검증 대상 (실제 io-pgtable 코드)
 *       → map_pages / unmap_pages / iova_to_phys
 *       → free_io_pgtable_ops()
 *
 * TLB 무효화 갈고리는 아무 일도 하지 않는 가짜로 바꿔 끼운다 — 실제
 * 하드웨어가 없으므로 무효화할 것도 없다. 다만 그 갈고리에 넘어오는
 * 인자가 규약에 맞는지는 그 자리에서 확인한다.
 *
 * 실행 컨텍스트: KUnit 모듈의 프로세스 문맥. 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - io-pgtable-arm.c: 검증 대상. 이 파일은 그 공개 진입점만 부른다.
 * - io-pgtable-arm.h: 그 구현의 내부 선언.
 * - include/linux/io-pgtable.h: 설정 구조체와 연산표의 정의.
 * - KUnit: 시험 뼈대와 가짜 장치 등록.
 *
 * === 주요 함수/구조체 요약 ===
 * - dummy_tlb_*: 아무 일도 하지 않는 무효화 갈고리들. 다만 넘어온 cookie 와
 *   크기가 규약에 맞는지 확인해, 구현이 갈고리를 잘못 부르면 잡아낸다.
 * - arm_lpae_run_tests(): 한 설정 조합에 대해 1단계와 2단계 형식을 모두
 *   시험한다. 빈 표 확인 → 크기별 매핑 → 전체 해제와 재매핑 →
 *   주소 공간 끝의 최대 블록 순으로 진행한다.
 * - arm_lpae_do_selftests(): 설정 조합을 만들어 위 함수를 반복해 부른다.
 * - __FAIL: 실패를 KUnit 에 보고하며 오류 코드를 내는 매크로.
 */

#define pr_fmt(fmt)	"arm-lpae io-pgtable: " fmt	/* [한국어] 이 파일의 로그 앞에 붙일 꼬리표 — 어느 구현의 시험인지 한눈에 알 수 있다. */

#include <kunit/device.h>	/* [한국어] 시험용 가짜 장치를 만든다 — 표를 잡을 때 장치 포인터가 필요하다. */
#include <kunit/test.h>	/* [한국어] KUNIT_CASE 등 시험 뼈대. */
#include <linux/io-pgtable.h>	/* [한국어] 검증 대상의 설정 구조체와 연산표. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */

#include "io-pgtable-arm.h"	/* [한국어] ARM LPAE 구현의 내부 선언. */

static struct io_pgtable_cfg *cfg_cookie;	/* [한국어] 지금 시험 중인 설정. 아래 가짜 갈고리들이 "넘어온 cookie 가 맞는지" 견주는 기준이 된다. */

/*
 * [한국어]
 * dummy_tlb_flush_all - 전체 무효화 갈고리의 가짜 구현
 *
 * @cookie: 등록할 때 넘긴 값.
 *
 * 실제로 비울 하드웨어가 없으므로 아무 일도 하지 않는다. 다만 넘어온
 * cookie 가 우리가 등록한 것과 같은지는 확인한다 — 구현이 엉뚱한 값을
 * 넘기면 실제 드라이버에서도 같은 버그가 나기 때문이다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 잠들지 않는다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_flush_all 갈고리 = [이 함수]
 */
static void dummy_tlb_flush_all(void *cookie)
{
	WARN_ON(cookie != cfg_cookie);	/* [한국어] 등록할 때 준 값이 그대로 돌아와야 한다 — 구현이 cookie 를 잘못 전달하면 실제 드라이버가 엉뚱한 도메인을 무효화한다. */
}

/*
 * [한국어]
 * dummy_tlb_flush - 범위 무효화 갈고리의 가짜 구현
 *
 * @iova: 무효화할 범위의 시작 (여기서는 쓰지 않는다).
 * @size: 그 길이.
 * @granule: 한 걸음의 크기 (여기서는 쓰지 않는다).
 * @cookie: 등록할 때 넘긴 값.
 *
 * cookie 확인에 더해, 넘어온 크기가 이 설정이 지원하는 페이지 크기와
 * 맞는지도 본다. 구현이 지원하지 않는 크기로 무효화를 요청하면 실제
 * 하드웨어에서는 그 명령이 거부되거나 엉뚱한 범위를 지운다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 잠들지 않는다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_flush_walk 갈고리 = [이 함수]
 */
static void dummy_tlb_flush(unsigned long iova, size_t size,
			    size_t granule, void *cookie)
{
	WARN_ON(cookie != cfg_cookie);	/* [한국어] cookie 가 그대로 돌아왔는가. */
	WARN_ON(!(size & cfg_cookie->pgsize_bitmap));	/* [한국어] 이 설정이 지원하는 크기여야 한다 — 아니면 실제 하드웨어가 그 무효화를 제대로 다루지 못한다. */
}

/*
 * [한국어]
 * dummy_tlb_add_page - 모아 두기 갈고리의 가짜 구현
 *
 * @gather: 모아 둘 목록 (여기서는 쓰지 않는다).
 * @iova: 그 페이지의 주소.
 * @granule: 페이지 크기.
 * @cookie: 등록할 때 넘긴 값.
 *
 * 실제 드라이버는 여기서 주소를 쌓아 두었다가 나중에 한꺼번에 무효화하지만,
 * 시험에서는 모을 이유가 없어 곧바로 범위 무효화 검사를 한 번 돌린다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 잠들지 않는다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_add_page 갈고리 = [이 함수] → dummy_tlb_flush()
 */
static void dummy_tlb_add_page(struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t granule,
			       void *cookie)
{
	dummy_tlb_flush(iova, granule, granule, cookie);	/* [한국어] 페이지 하나짜리 범위 무효화로 넘겨 같은 검사를 받게 한다. */
}

/* [한국어] 시험용 무효화 갈고리표.
 *
 * 셋 다 아무 일도 하지 않되, 인자가 규약에 맞는지만 확인한다. 실제
 * 하드웨어 없이도 "구현이 갈고리를 옳게 부르는가"를 검증할 수 있게 하는
 * 장치다. */
static const struct iommu_flush_ops dummy_tlb_ops = {
	.tlb_flush_all	= dummy_tlb_flush_all,	/* [한국어] 통째로 비우기. */
	.tlb_flush_walk	= dummy_tlb_flush,	/* [한국어] 범위 무효화. */
	.tlb_add_page	= dummy_tlb_add_page,	/* [한국어] 나중에 몰아 낼 목록에 쌓기. */
};

/* [한국어] 실패를 KUnit 에 보고하며 오류 코드를 내는 매크로.
 *
 * 검사가 실패하는 자리가 아주 많아, 매번 두 줄을 쓰는 대신 하나로 묶었다.
 * 어느 형식(1단계인지 2단계인지)에서 실패했는지 첨자로 알려 준다. */
#define __FAIL(test, i) ({							\
		KUNIT_FAIL(test, "test failed for fmt idx %d\n", (i));		\
		-EFAULT;							\
})	/* [한국어] 값을 내는 식이라 괄호로 감쌌다 — 조건문 안에서 그대로 쓸 수 있게 한다. */

/*
 * [한국어]
 * arm_lpae_run_tests - 한 설정으로 1·2단계 페이지 테이블을 모두 시험한다
 *
 * @test: 시험 문맥.
 * @cfg: 시험할 설정 (알갱이 크기, 입력·출력 주소 폭).
 * @return: 0 통과, 음수 실패.
 *
 * 시험의 몸통이다. 네 단계로 진행한다.
 *
 * 하나, 빈 표가 아무것도 번역하지 않는지 확인한다 — 초기 상태가 잘못되면
 * 매핑하지 않은 주소가 엉뚱한 물리 주소로 이어진다.
 *
 * 둘, 지원하는 크기마다 매핑을 하나씩 걸고 되물어 본다. 같은 자리에
 * 겹쳐 매핑하려는 시도가 거부되는지도 함께 본다 — 겹침을 허용하면
 * 앞 매핑이 조용히 사라진다.
 *
 * 셋, 모두 풀고 다시 매핑한다. 해제가 반환하는 길이가 요청과 같아야
 * 하고, 푼 뒤에는 번역이 사라져야 한다.
 *
 * 넷, 주소 공간의 맨 끝에 가장 큰 블록을 매핑한다. 주석이 밝히듯 이
 * 자리가 이어 붙인 표(concatenated table)의 구석 사례를 건드린다 —
 * 2단계 형식에서 시작 단계의 표를 여러 개 이어 붙이는 경우다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_lpae_do_selftests() → [이 함수] → alloc_io_pgtable_ops()
 */
static int arm_lpae_run_tests(struct kunit *test, struct io_pgtable_cfg *cfg)
{
	static const enum io_pgtable_fmt fmts[] = {	/* [한국어] 시험할 두 형식 — 1단계와 2단계는 표를 걷는 방식이 달라 따로 검증해야 한다. */
		ARM_64_LPAE_S1,	/* [한국어] 1단계 형식. */
		ARM_64_LPAE_S2,	/* [한국어] 2단계 형식 — 시작 단계와 이어 붙이기 규칙이 다르다. */
	};

	int i, j;	/* [한국어] 형식 반복자와 페이지 크기 비트 반복자. */
	unsigned long iova;	/* [한국어] 시험할 장치 주소. */
	size_t size, mapped;	/* [한국어] 매핑 크기와, 실제로 매핑된 길이. */
	struct io_pgtable_ops *ops;	/* [한국어] 검증 대상의 연산표. */

	for (i = 0; i < ARRAY_SIZE(fmts); ++i) {	/* [한국어] 두 형식을 차례로. */
		cfg_cookie = cfg;	/* [한국어] 가짜 갈고리들이 견줄 기준을 세운다. */
		ops = alloc_io_pgtable_ops(fmts[i], cfg, cfg);	/* [한국어] 표를 짓는다. 설정 자체를 cookie 로 넘겨, 갈고리에서 되짚을 수 있게 한다. */
		if (!ops) {	/* [한국어] 짓지 못했다면 — 설정이 그 형식에 맞지 않거나 메모리가 없다. */
			kunit_err(test, "failed to allocate io pgtable ops\n");	/* [한국어] 이 조합으로는 표를 지을 수 없다. */
			return -ENOMEM;	/* [한국어] 시험을 더 진행할 수 없다. */
		}

		/*
		 * Initial sanity checks.
		 * Empty page tables shouldn't provide any translations.
		 */
		/* [한국어] (위 영어 주석 참고) 초기 상태가 잘못되면 매핑하지 않은
		 * 주소가 엉뚱한 물리 주소로 이어진다 — 가장 위험한 종류의 버그다.
		 * 서로 다른 표 단계에 걸치도록 세 지점을 고른다. */
		if (ops->iova_to_phys(ops, 42))	/* [한국어] 아주 낮은 주소. */
			return __FAIL(test, i);

		if (ops->iova_to_phys(ops, SZ_1G + 42))	/* [한국어] 한 단계 위의 표를 건드리는 주소. */
			return __FAIL(test, i);

		if (ops->iova_to_phys(ops, SZ_2G + 42))	/* [한국어] 또 다른 자리 — 세 지점이 서로 다른 표 항목에 걸린다. */
			return __FAIL(test, i);

		/*
		 * Distinct mappings of different granule sizes.
		 */
		/* [한국어] (위 영어 주석 참고) 지원하는 크기마다 하나씩 매핑을 건다.
		 * 크기마다 표의 어느 단계에 항목이 생기는지가 달라, 이 반복 하나로
		 * 모든 단계의 항목 짓기가 검증된다. */
		iova = 0;	/* [한국어] 첫 매핑은 주소 0 부터. */
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {	/* [한국어] 지원하는 크기마다. */
			size = 1UL << j;	/* [한국어] 그 비트가 뜻하는 크기. */

			if (ops->map_pages(ops, iova, iova, size, 1,	/* [한국어] iova 와 물리 주소를 같게 매핑한다 — 되물어 본 답이 원래 주소와 같아야 한다는 검사 기준이 된다. */
					   IOMMU_READ | IOMMU_WRITE |
					   IOMMU_NOEXEC | IOMMU_CACHE,	/* [한국어] 권한 비트를 모두 세워 항목 짓기가 그것을 옳게 담는지 본다. */
					   GFP_KERNEL, &mapped))
				return __FAIL(test, i);	/* [한국어] 매핑 자체가 실패하면 안 된다. */

			/* Overlapping mappings */
			/* [한국어] (위 영어 주석 참고) 같은 자리에 다시 매핑하려는 시도는
			 * 반드시 거부되어야 한다. 허용하면 앞 매핑이 조용히 사라져,
			 * 그 주소를 쓰던 장치가 엉뚱한 메모리를 건드리게 된다. */
			if (!ops->map_pages(ops, iova, iova + size, size, 1,	/* [한국어] 성공하면(0을 돌려주면) 실패로 친다 — 조건이 뒤집혀 있다. */
					    IOMMU_READ | IOMMU_NOEXEC,
					    GFP_KERNEL, &mapped))
				return __FAIL(test, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))	/* [한국어] 매핑 안의 임의의 자리를 되물어 본다 — 42 는 페이지 안쪽 오프셋이 옳게 더해지는지도 함께 본다. */
				return __FAIL(test, i);

			iova += SZ_1G;	/* [한국어] 다음 크기는 멀찍이 떨어진 자리에 — 매핑끼리 겹치지 않게 한다. */
		}

		/* Full unmap */
		/* [한국어] (위 영어 주석 참고) 방금 건 매핑을 모두 풀고, 같은 자리에
		 * 다시 건다. 해제가 표를 제대로 정리하는지, 정리한 자리에 다시
		 * 매핑할 수 있는지를 함께 본다. */
		iova = 0;	/* [한국어] 다시 처음부터. */
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {	/* [한국어] 같은 순서로 훑는다. */
			size = 1UL << j;	/* [한국어] 그 비트가 뜻하는 크기. */

			if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)	/* [한국어] 요청한 길이만큼 정확히 풀려야 한다 — 덜 풀리면 매핑이 남고, 더 풀리면 남의 매핑을 지운 것이다. */
				return __FAIL(test, i);

			if (ops->iova_to_phys(ops, iova + 42))	/* [한국어] 푼 뒤에는 번역이 사라져야 한다. */
				return __FAIL(test, i);

			/* Remap full block */
			/* [한국어] (위 영어 주석 참고) 정리된 자리에 다시 매핑할 수 있어야
			 * 한다 — 해제가 표를 어중간하게 남겨 두면 여기서 실패한다. */
			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_WRITE, GFP_KERNEL, &mapped))	/* [한국어] 권한을 달리 줘 본다 — 앞선 항목의 찌꺼기가 남지 않았는지 함께 본다. */
				return __FAIL(test, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))	/* [한국어] 다시 건 매핑도 옳게 번역되어야 한다. */
				return __FAIL(test, i);

			iova += SZ_1G;	/* [한국어] 다음 자리로. */
		}

		/*
		 * Map/unmap the last largest supported page of the IAS, this can
		 * trigger corner cases in the concatednated page tables.
		 */
		/* [한국어] (위 영어 주석 참고) 입력 주소 공간의 맨 끝에 가장 큰 블록을
		 * 매핑해 본다. 2단계 형식에서는 시작 단계의 표를 여러 개 이어 붙이는
		 * 경우가 있는데, 그 마지막 표의 마지막 항목이 구석 사례가 된다 —
		 * 첨자 계산이 한 칸이라도 어긋나면 여기서 드러난다. */
		mapped = 0;	/* [한국어] 실제로 매핑된 길이를 받을 자리. */
		size = 1UL << __fls(cfg->pgsize_bitmap);	/* [한국어] 지원하는 가장 큰 크기. */
		iova = (1UL << cfg->ias) - size;	/* [한국어] 입력 주소 공간의 맨 끝에서 그 크기만큼 앞. */
		if (ops->map_pages(ops, iova, iova, size, 1,	/* [한국어] 주소 공간 끝에 가장 큰 블록을 매핑해 본다. */
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE,
				   GFP_KERNEL, &mapped))
			return __FAIL(test, i);
		if (mapped != size)	/* [한국어] 요청한 만큼 전부 매핑됐어야 한다 — 부분 매핑은 구석 사례에서 흔한 실패 형태다. */
			return __FAIL(test, i);
		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)	/* [한국어] 푸는 것도 마찬가지로 전부여야 한다. */
			return __FAIL(test, i);

		free_io_pgtable_ops(ops);	/* [한국어] 표를 놓는다 — 남은 매핑이 있으면 이 안에서 경고가 뜬다. */
	}

	return 0;	/* [한국어] 두 형식 모두 통과했다. */
}

/*
 * [한국어]
 * arm_lpae_do_selftests - 설정 조합을 만들어 시험을 반복한다
 *
 * @test: 시험 문맥.
 *
 * 알갱이 크기 세 가지와 주소 폭 여섯 가지를 곱해 조합을 만든다. 그
 * 조합마다 표의 단계 수와 시작 단계가 달라져, 손으로는 다 짚어 보기
 * 어려운 경우들이 자동으로 훑어진다.
 *
 * 입력 폭이 출력 폭을 넘지 않게 제한하는 것이 요점이다 — 2단계 형식에서
 * 그 조합은 규격상 성립하지 않아, 시험해도 뜻이 없다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_lpae_run_tests()
 */
static void arm_lpae_do_selftests(struct kunit *test)
{
	static const unsigned long pgsize[] = {	/* [한국어] 알갱이 크기 조합 — 알갱이마다 그 위의 블록 크기가 함께 온다. */
		SZ_4K | SZ_2M | SZ_1G,	/* [한국어] 4K 알갱이는 두 단계의 블록을 함께 쓸 수 있다. */
		SZ_16K | SZ_32M,	/* [한국어] 16K 알갱이. */
		SZ_64K | SZ_512M,	/* [한국어] 64K 알갱이 — 표의 단계 수가 가장 적다. */
	};

	static const unsigned int address_size[] = {	/* [한국어] 시험할 주소 폭들 — 폭에 따라 표의 단계 수가 달라진다. */
		32, 36, 40, 42, 44, 48,	/* [한국어] 폭이 커질수록 표의 단계 수가 늘어난다. */
	};

	int i, j, k, pass = 0, fail = 0;	/* [한국어] 세 반복자와 통과·실패 계수. */
	struct device *dev;	/* [한국어] 표를 잡을 때 쓸 가짜 장치. */
	struct io_pgtable_cfg cfg = {	/* [한국어] 조합마다 덮어쓸 공통 설정. */
		.tlb = &dummy_tlb_ops,	/* [한국어] 아무 일도 하지 않는 무효화 갈고리들. */
		.coherent_walk = true,	/* [한국어] 캐시 일관성이 있다고 두어, 표를 고칠 때 캐시를 씻는 경로를 건너뛴다 — 하드웨어가 없으므로 뜻이 없다. */
		.quirks = IO_PGTABLE_QUIRK_NO_WARN,	/* [한국어] 겹침 매핑을 일부러 시도하므로, 그때마다 경고가 뜨면 로그가 넘친다. */
	};

	dev = kunit_device_register(test, "io-pgtable-test");	/* [한국어] 표 메모리를 잡을 때 장치 포인터가 필요해 가짜를 하나 만든다. */
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, dev);	/* [한국어] 만들지 못하면 시험을 진행할 수 없다. */
	if (IS_ERR_OR_NULL(dev))	/* [한국어] 가짜 장치를 못 만들면 표를 잡을 수 없다. */
		return;

	cfg.iommu_dev = dev;	/* [한국어] 그 장치를 설정에 담는다. */

	for (i = 0; i < ARRAY_SIZE(pgsize); ++i) {	/* [한국어] 알갱이 크기 조합마다. */
		for (j = 0; j < ARRAY_SIZE(address_size); ++j) {	/* [한국어] 출력 주소 폭마다. */
			/* Don't use ias > oas as it is not valid for stage-2. */
			/* [한국어] (위 영어 주석 참고) 입력 폭이 출력 폭보다 넓은 조합은
			 * 2단계 형식에서 규격상 성립하지 않는다. 그래서 입력 폭 반복을
			 * 출력 폭까지로 제한한다. */
			for (k = 0; k <= j; ++k) {	/* [한국어] 입력 주소 폭마다 (출력 폭 이하). */
				cfg.pgsize_bitmap = pgsize[i];	/* [한국어] 이번 조합의 알갱이 크기. */
				cfg.ias = address_size[k];	/* [한국어] 입력 주소 폭. */
				cfg.oas = address_size[j];	/* [한국어] 출력 주소 폭. */
				kunit_info(test, "pgsize_bitmap 0x%08lx, IAS %u OAS %u\n",	/* [한국어] 어느 조합에서 실패했는지 알 수 있게 매번 남긴다. */
					   pgsize[i], cfg.ias, cfg.oas);
				if (arm_lpae_run_tests(test, &cfg))	/* [한국어] 이 조합으로 두 형식을 모두 시험한다. */
					fail++;
				else
					pass++;
			}
		}
	}

	kunit_info(test, "completed with %d PASS %d FAIL\n", pass, fail);	/* [한국어] 전체 결과를 한 줄로 요약한다. */
}

/* [한국어] 이 모듈이 돌릴 시험 목록 — 하나뿐이다.
 *
 * 조합을 훑는 반복이 그 함수 안에 있어, KUnit 이 보기에는 시험이 하나다. */
static struct kunit_case io_pgtable_arm_test_cases[] = {
	KUNIT_CASE(arm_lpae_do_selftests),	/* [한국어] 설정 조합 전체를 도는 시험. */
	{},	/* [한국어] 목록의 끝. */
};

/* [한국어] KUnit 에 등록할 시험 묶음. */
static struct kunit_suite io_pgtable_arm_test = {
	.name = "io-pgtable-arm-test",	/* [한국어] 결과 보고에 찍힐 이름. */
	.test_cases = io_pgtable_arm_test_cases,	/* [한국어] 위 목록. */
};

kunit_test_suite(io_pgtable_arm_test);	/* [한국어] 이 묶음을 KUnit 에 등록한다 — 모듈로 빌드하면 적재 때 돈다. */

MODULE_DESCRIPTION("io-pgtable-arm library kunit tests");	/* [한국어] modinfo 에 찍힐 설명. */
MODULE_LICENSE("GPL");	/* [한국어] GPL 심볼을 쓸 수 있게 하는 라이선스 선언. */
