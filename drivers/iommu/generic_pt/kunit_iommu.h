/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] kunit 시험용 가짜 IOMMU 환경 (kunit_iommu.h)
 *
 * === 파일의 역할 ===
 * generic_pt 를 실제 하드웨어 없이 시험하려면 페이지 테이블 하나가 살
 * 수 있는 최소한의 환경이 필요하다. 이 파일이 그것을 만든다 — 가짜 장치,
 * 가짜 도메인 연산, 최상위 교체 콜백, 그리고 시험 전체가 공유하는 상태다.
 *
 * 형식마다 여러 설정으로 시험하는 장치도 여기 있다. 형식이
 * kunit_fmt_cfgs 배열을 두면 kunit 의 파라미터 기능으로 그 각각을 돌린다 —
 * 예를 들어 x86_64 는 부호 확장 유무 × 4·5단계 네 조합을 모두 돈다.
 *
 * 32비트 시스템 처리가 눈에 띈다. 원 주석이 사정을 밝힌다: unsigned long 이
 * 32비트면 IOMMU 연산의 주소가 그 폭으로 제한되므로, 시험이 32비트를 넘는
 * VA 를 만들면 안 된다. 그 환경을 굳이 시험하는 이유는 공통 코드에 남은
 * 32비트 가정을 찾기 위해서다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c 를 GENERIC_PT_KUNIT 을 켜고 한 번 더 컴파일할 때,
 * iommu_template.h 가 iommu_pt.h 대신 kunit 헤더들을 포함한다. 그래서
 * 시험이 검증하는 코드가 실제로 쓰이는 그 코드다.
 *
 * 실행 컨텍스트: kunit 모듈. 프로세스 문맥이라 GFP_KERNEL 을 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위: kunit_generic_pt.h(표 자체의 시험)와 kunit_iommu_pt.h(IOMMU 진입점
 *     시험)가 이 파일이 만든 환경을 쓴다.
 * 아래: <kunit/device.h>, <kunit/test.h>, pt_iter.h, ../iommu-pages.h.
 *
 * === 주요 함수/구조체 요약 ===
 * struct kunit_iommu_priv: 시험 하나가 쓰는 전체 상태. 도메인과 형식별
 *   표를 union 으로 겹쳐 두어 코어가 쓰는 배치를 그대로 흉내 낸다.
 * kunit_pt_gen_params_cfg: 형식이 준 설정 목록을 kunit 파라미터로 돌린다.
 * pt_kunit_priv_init: 그 환경을 실제로 세운다 — 설정을 고르고, 표를
 *   만들고, 시험이 쓸 페이지 크기와 안전한 범위를 계산한다.
 * pt_kunit_change_top: 최상위가 자라도 갱신할 하드웨어가 없어 빈 함수다.
 * IS_32BIT: 시험 범위를 줄여야 하는 환경인지 알려 준다.
 */
#ifndef __GENERIC_PT_KUNIT_IOMMU_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_KUNIT_IOMMU_H	/* [한국어] 같은 이름으로 표시 */

#define GENERIC_PT_KUNIT 1	/* [한국어] iommu_template.h 가 이 값을 보고 kunit 헤더를 포함한다 */
#include <kunit/device.h>	/* [한국어] 가짜 장치를 만든다 */
#include <kunit/test.h>	/* [한국어] 시험 매크로 */
#include "../iommu-pages.h"	/* [한국어] 표 메모리 해제 */
#include "pt_iter.h"	/* [한국어] 순회기와 형식 API */

#define pt_iommu_table_cfg CONCATENATE(pt_iommu_table, _cfg)	/* [한국어] 형식별 설정 구조체의 이름 */
#define pt_iommu_init CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), init)	/* [한국어] 형식별 초기화 함수의 이름 */
int pt_iommu_init(struct pt_iommu_table *fmt_table,	/* [한국어] 시험 모듈은 그 함수를 다른 컴파일 단위에서 가져다 쓴다 */
		  const struct pt_iommu_table_cfg *cfg, gfp_t gfp);	/* [한국어] 설정과 할당 플래그를 넘긴다 */

/* The format can provide a list of configurations it would like to test */
#ifdef kunit_fmt_cfgs	/* [한국어] (원 주석: 형식이 시험할 설정 목록을 제공할 수 있다) */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * kunit_pt_gen_params_cfg - 형식이 준 설정 목록을 하나씩 내어 준다
 *
 * @test: 시험 문맥.
 * @prev: 직전에 돌려준 값(처음에는 NULL).
 * @desc: 이 파라미터의 이름을 적을 자리.
 * @return: 다음 설정을 뜻하는 값, 끝났으면 NULL.
 *
 * kunit 의 파라미터 생성기다. 배열 인덱스를 1 부터 세어 포인터로 위장해
 * 돌려주는데, 0 은 NULL 과 구별되지 않아 종료 신호로 쓰이기 때문이다.
 *
 * 이름에 형식과 번호를 넣어 두면 어느 설정에서 실패했는지 결과에 그대로
 * 드러난다.
 */
static const void *kunit_pt_gen_params_cfg(struct kunit *test, const void *prev,
					   char *desc)
{
	uintptr_t cfg_id = (uintptr_t)prev;	/* [한국어] 포인터로 위장한 배열 인덱스 */

	cfg_id++;	/* [한국어] 다음 설정으로 */
	if (cfg_id >= ARRAY_SIZE(kunit_fmt_cfgs) + 1)	/* [한국어] 목록을 다 돌았으면 */
		return NULL;	/* [한국어] kunit 이 여기서 멈춘다 */
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s_cfg_%u",	/* [한국어] 어느 설정에서 실패했는지 결과에 드러나도록 */
		 __stringify(PTPFX_RAW), (unsigned int)(cfg_id - 1));	/* [한국어] 형식 이름과 번호 */
	return (void *)cfg_id;	/* [한국어] 1 부터 세는 이유: 0 은 NULL 과 구별되지 않는다 */
}
#define KUNIT_CASE_FMT(test_name) \
	KUNIT_CASE_PARAM(test_name, kunit_pt_gen_params_cfg)	/* [한국어] 각 설정마다 한 번씩 돌린다 */
#else
#define KUNIT_CASE_FMT(test_name) KUNIT_CASE(test_name)	/* [한국어] 없으면 한 번만 */
#endif

/*
 * [한국어] 오류 코드를 사람이 읽는 이름으로 보여 주는 단언.
 * -22 대신 -EINVAL 로 찍히므로 실패 원인을 바로 알 수 있다.
 */
#define KUNIT_ASSERT_NO_ERRNO(test, ret)                                       \
	KUNIT_ASSERT_EQ_MSG(test, ret, 0, KUNIT_SUBSUBTEST_INDENT "errno %pe", \
			    ERR_PTR(ret))	/* [한국어] 오류 코드를 이름으로 풀어 찍는다 */

/*
 * [한국어] 위와 같되 어느 함수에서 났는지도 함께 찍는다.
 * 같은 시험 안에서 여러 함수를 부를 때 쓴다.
 */
#define KUNIT_ASSERT_NO_ERRNO_FN(test, fn, ret)                          \
	KUNIT_ASSERT_EQ_MSG(test, ret, 0,                                \
			    KUNIT_SUBSUBTEST_INDENT "errno %pe from %s", \
			    ERR_PTR(ret), fn)	/* [한국어] 어느 함수에서 났는지까지 */

/*
 * When the test is run on a 32 bit system unsigned long can be 32 bits. This
 * cause the iommu op signatures to be restricted to 32 bits. Meaning the test
 * has to be mindful not to create any VA's over the 32 bit limit. Reduce the
 * scope of the testing as the main purpose of checking on full 32 bit is to
 * look for 32bitism in the core code. Run the test on i386 with X86_PAE=y to
 * get the full coverage when dma_addr_t & phys_addr_t are 8 bytes
 */
#define IS_32BIT (sizeof(unsigned long) == 4)	/* [한국어] (원 주석: 32비트에서는 IOMMU 연산의 주소가 그 폭으로 제한된다) */

struct kunit_iommu_priv {
	union {	/* [한국어] 도메인과 형식별 표를 겹쳐 둔다 — 실제 드라이버가 쓰는 배치를 흉내 낸다 */
		struct iommu_domain domain;
		/* [한국어] 코어가 보는 도메인.
		 * 설정자: pt_kunit_priv_init 이 ops 를 넣고, pt_iommu_init 이 나머지를 채운다.
		 * 읽는 자: map/unmap 진입점이 여기서 pgsize_bitmap 을 읽는다.
		 * 값 범위: 초기화된 도메인.
		 * 동기화: 시험 하나가 독점하므로 경쟁이 없다. */
		struct pt_iommu_table fmt_table;
		/* [한국어] 형식별 페이지 테이블 객체.
		 * 설정자: pt_iommu_init.
		 * 읽는 자: 모든 시험이 이것을 통해 표를 다룬다.
		 * 값 범위: 초기화된 객체. union 으로 domain 과 겹쳐 두어, 실제 드라이버가
		 *   두 구조를 겹쳐 쓰는 배치를 그대로 흉내 낸다.
		 * 동기화: 시험 하나가 독점한다. */
	};
	spinlock_t top_lock;
	/* [한국어] 최상위 교체를 지키는 락.
	 * 설정자: pt_kunit_priv_init 이 초기화한다.
	 * 읽는 자: increase_top 이 get_top_lock 으로 받아 잡는다.
	 * 값 범위: 초기화된 스핀락.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct device *dummy_dev;
	/* [한국어] 메모리 할당과 DMA 매핑에 쓸 가짜 장치.
	 * 설정자: kunit_device_register.
	 * 읽는 자: 표 할당과 비일관 매핑.
	 * 값 범위: 유효한 장치. 시험이 끝나면 kunit 이 해제한다.
	 * 동기화: 시험 하나가 독점한다. */
	struct pt_iommu *iommu;
	/* [한국어] 자주 쓰는 공통 부분의 지름길.
	 * 설정자: pt_kunit_priv_init.
	 * 읽는 자: 시험 코드 전반.
	 * 값 범위: &fmt_table.iommu.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	struct pt_common *common;
	/* [한국어] 페이지 테이블 상태의 지름길.
	 * 설정자: pt_kunit_priv_init.
	 * 읽는 자: 주소 폭과 기능을 묻는 시험들.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	struct pt_iommu_table_cfg cfg;
	/* [한국어] 이번 파라미터가 고른 설정.
	 * 설정자: kunit_fmt_cfgs 에서 복사하고 기본값을 채운다.
	 * 읽는 자: pt_iommu_init.
	 * 값 범위: 형식이 준 설정 중 하나.
	 * 동기화: 초기화 중에만 쓰인다. */
	struct pt_iommu_info info;
	/* [한국어] 표가 보고한 지원 페이지 크기.
	 * 설정자: get_info 콜백.
	 * 읽는 자: 아래 크기 값들을 계산하는 코드와 시험들.
	 * 값 범위: size_t 로 잘린 비트맵.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	unsigned int smallest_pgsz_lg2;
	/* [한국어] 이 형식의 가장 작은 페이지 크기(지수).
	 * 설정자: pt_kunit_priv_init.
	 * 읽는 자: 최소 단위로 매핑을 만드는 시험들.
	 * 값 범위: 형식의 최소 페이지.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	pt_vaddr_t smallest_pgsz;
	/* [한국어] 그 크기의 실제 값.
	 * 설정자: pt_kunit_priv_init.
	 * 읽는 자: 주소를 그 단위로 밀어 가는 시험들.
	 * 값 범위: 2의 거듭제곱.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	unsigned int largest_pgsz_lg2;
	/* [한국어] 쓸 수 있는 가장 큰 페이지 크기(지수).
	 * 설정자: pt_kunit_priv_init.
	 * 읽는 자: 큰 페이지 경로를 시험하는 코드.
	 * 값 범위: size_t 로 잘린 비트맵의 최상위 비트.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	pt_oaddr_t test_oa;
	/* [한국어] 시험이 매핑할 출력 주소.
	 * 설정자: 눈에 띄는 상수를 형식의 출력 폭에 맞춰 자른다.
	 * 읽는 자: 매핑을 만들고 되읽어 확인하는 시험들.
	 * 값 범위: 형식이 표현할 수 있는 주소. 그 패턴이 보이면 이 값이다.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	pt_vaddr_t safe_pgsize_bitmap;
	/* [한국어] dma_addr_t API 를 안전하게 통과하는 크기들.
	 * 설정자: 주소 폭의 절반 아래로 잘라 만든다.
	 * 읽는 자: 큰 범위를 매핑하는 시험들.
	 * 값 범위: info.pgsize_bitmap 의 부분집합.
	 * 동기화: 초기화 뒤 바뀌지 않는다. */
	unsigned long orig_nr_secondary_pagetable;
	/* [한국어] 시험 시작 시점의 페이지 테이블 메모리 통계.
	 * 설정자: 시험 시작 시 기록한다.
	 * 읽는 자: 끝난 뒤 같은 값으로 돌아왔는지 확인해 누수를 잡는다.
	 * 값 범위: 전역 카운터의 스냅숏.
	 * 동기화: 읽기만 하므로 정확할 필요는 없다. */

};
PT_IOMMU_CHECK_DOMAIN(struct kunit_iommu_priv, fmt_table.iommu, domain);	/* [한국어] 도메인이 구조체 맨 앞에 있는지 컴파일 시 확인한다 */

/*
 * [한국어]
 * pt_kunit_iotlb_sync - 무효화 대신 페이지만 돌려주는 가짜 구현
 *
 * @domain: 무시된다.
 * @gather: 모아 둔 해제 목록.
 *
 * 시험에는 무효화할 하드웨어가 없다. 다만 해제 목록을 비우지 않으면
 * 메모리가 새므로, 그 일만 한다.
 */
static void pt_kunit_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *gather)
{
	iommu_put_pages_list(&gather->freelist);	/* [한국어] 무효화할 하드웨어는 없지만 페이지는 돌려주어야 한다 */
}

#define IOMMU_PT_DOMAIN_OPS1(x) IOMMU_PT_DOMAIN_OPS(x)	/* [한국어] 매크로 인자를 한 겹 더 펼쳐 PTPFX_RAW 가 이름으로 치환되게 한다 */
static const struct iommu_domain_ops kunit_pt_ops = {
	IOMMU_PT_DOMAIN_OPS1(PTPFX_RAW),	/* [한국어] map/unmap 등은 이 형식의 구현이 채운다 */
	.iotlb_sync = &pt_kunit_iotlb_sync,	/* [한국어] 무효화 대신 페이지만 돌려주는 가짜 */
};

/*
 * [한국어]
 * pt_kunit_change_top - 최상위 교체 알림의 빈 구현
 *
 * @iommu_table: 무시된다.
 * @top_paddr: 무시된다.
 * @top_level: 무시된다.
 *
 * 최상위가 자라면 드라이버가 하드웨어를 갱신해야 하지만, 시험에는 갱신할
 * 하드웨어가 없다. 그래도 이 콜백이 없으면 DYNAMIC_TOP 형식의 초기화가
 * 거절되므로 빈 함수를 둔다.
 */
static void pt_kunit_change_top(struct pt_iommu *iommu_table,
				phys_addr_t top_paddr, unsigned int top_level)
{
}

/*
 * [한국어]
 * pt_kunit_get_top_lock - 최상위 교체에 쓸 락을 빌려 준다
 *
 * @iommu_table: 페이지 테이블 객체.
 * @return: 시험 상태가 들고 있는 스핀락.
 *
 * 실제 드라이버가 도메인 락을 빌려 주는 자리에, 시험은 자기 락을 준다.
 */
static spinlock_t *pt_kunit_get_top_lock(struct pt_iommu *iommu_table)
{
	struct kunit_iommu_priv *priv = container_of(	/* [한국어] 페이지 테이블 객체에서 */
		iommu_table, struct kunit_iommu_priv, fmt_table.iommu);	/* [한국어] 시험 상태로 되짚는다 */

	return &priv->top_lock;	/* [한국어] 실제 드라이버가 도메인 락을 주는 자리 */
}

static const struct pt_iommu_driver_ops pt_kunit_driver_ops = {	/* [한국어] 최상위가 자랄 때 generic_pt 가 되묻는 콜백들 */
	.change_top = &pt_kunit_change_top,	/* [한국어] 갱신할 하드웨어가 없어 빈 함수 */
	.get_top_lock = &pt_kunit_get_top_lock,	/* [한국어] 시험 상태가 들고 있는 락을 준다 */
};

/*
 * [한국어]
 * pt_kunit_priv_init - 시험 하나가 쓸 환경을 세운다
 *
 * @test: 시험 문맥.
 * @priv: 채울 상태.
 * @return: 0 성공, 음수면 실패(또는 건너뛴다).
 *
 * 순서는 이렇다: 가짜 장치를 만들고, 이번 파라미터가 가리키는 설정을
 * 고르고, 표를 만들고, 시험이 쓸 값들을 계산한다.
 *
 * 기능 조합을 정하는 방식이 원 주석에 있다. 형식이 kunit_fmt_cfgs 로
 * 제어하겠다고 지정한 기능(KUNIT_FMT_FEATURES)만 설정에서 오고, 나머지는
 * 기본으로 모두 켠다.
 *
 * 뒤쪽의 계산 세 가지가 시험의 안전장치다.
 *  - pgsize_bitmap 을 size_t 로 자른다. 매핑 길이가 size_t 로 전달되므로
 *    그보다 큰 페이지는 시험할 수 없다.
 *  - test_oa 는 눈에 띄는 상수를 형식의 출력 폭에 맞춰 자른 값이다.
 *    잘못된 주소가 나오면 이 패턴이 보여 원인을 찾기 쉽다.
 *  - safe_pgsize_bitmap 은 dma_addr_t API 를 통과할 수 있는 크기만 남긴다.
 *    원 주석대로, 큰 매핑을 만들면 VA 공간이 모자란다.
 *
 * -EOVERFLOW 를 실패가 아니라 건너뛰기로 다루는 이유: 32비트 시스템에서
 * 넓은 주소 공간 형식은 애초에 성립하지 않는다.
 */
static int pt_kunit_priv_init(struct kunit *test, struct kunit_iommu_priv *priv)
{
	unsigned int va_lg2sz;	/* [한국어] 안전한 범위 계산에 쓴다 */
	int ret;	/* [한국어] 결과 */

	/* Enough so the memory allocator works */
	priv->dummy_dev = kunit_device_register(test, "pt_kunit_dev");	/* [한국어] (원 주석: 메모리 할당기가 동작할 만큼의 가짜 장치) */
	if (IS_ERR(priv->dummy_dev))	/* [한국어] 만들지 못했으면 */
		return PTR_ERR(priv->dummy_dev);	/* [한국어] 시험을 진행할 수 없다 */
	set_dev_node(priv->dummy_dev, NUMA_NO_NODE);	/* [한국어] 노드를 고정하지 않는다 */

	spin_lock_init(&priv->top_lock);	/* [한국어] 최상위 교체를 지킬 락 */

#ifdef kunit_fmt_cfgs	/* [한국어] 형식이 설정 목록을 준 경우 */
	priv->cfg = kunit_fmt_cfgs[((uintptr_t)test->param_value) - 1];	/* [한국어] 파라미터가 가리키는 설정 — 1 부터 세었으므로 되돌린다 */
	/*
	 * The format can set a list of features that the kunit_fmt_cfgs
	 * controls, other features are default to on.
	 */
	priv->cfg.common.features |= PT_SUPPORTED_FEATURES &	/* [한국어] (원 주석: 형식이 지정한 기능만 설정이 제어하고 나머지는 기본으로 켠다) */
				     (~KUNIT_FMT_FEATURES);	/* [한국어] 제어 대상이 아닌 기능들 */
#else
	priv->cfg.common.features = PT_SUPPORTED_FEATURES;	/* [한국어] 설정 목록이 없으면 전부 켠다 */
#endif	/* [한국어] 포함 방지 끝 */

	/* Defaults, for the kunit */
	if (!priv->cfg.common.hw_max_vasz_lg2)	/* [한국어] (원 주석: kunit 용 기본값) */
		priv->cfg.common.hw_max_vasz_lg2 = PT_MAX_VA_ADDRESS_LG2;	/* [한국어] 형식의 최대 입력 폭 */
	if (!priv->cfg.common.hw_max_oasz_lg2)	/* [한국어] 정하지 않았으면 */
		priv->cfg.common.hw_max_oasz_lg2 = pt_max_oa_lg2(NULL);	/* [한국어] 형식의 최대 출력 폭 */

	priv->fmt_table.iommu.nid = NUMA_NO_NODE;	/* [한국어] 표를 아무 노드에나 */
	priv->fmt_table.iommu.driver_ops = &pt_kunit_driver_ops;	/* [한국어] 최상위 교체 콜백 — 없으면 DYNAMIC_TOP 초기화가 거절된다 */
	priv->fmt_table.iommu.iommu_device = priv->dummy_dev;	/* [한국어] 비일관 매핑에 쓸 장치 */
	priv->domain.ops = &kunit_pt_ops;	/* [한국어] 코어가 부를 연산 집합 */
	ret = pt_iommu_init(&priv->fmt_table, &priv->cfg, GFP_KERNEL);	/* [한국어] 최상위 표까지 만들어진다 */
	if (ret) {	/* [한국어] 실패면 */
		if (ret == -EOVERFLOW)	/* [한국어] 32비트에서 담을 수 없는 구성이면 */
			kunit_skip(	/* [한국어] 실패가 아니라 */
				test,	/* [한국어] 건너뛴다 */
				"This configuration cannot be tested on 32 bit");	/* [한국어] 애초에 성립하지 않는 조합이다 */
		return ret;	/* [한국어] 그 밖의 실패는 그대로 */
	}

	priv->iommu = &priv->fmt_table.iommu;	/* [한국어] 자주 쓰는 포인터를 미리 잡아 둔다 */
	priv->common = common_from_iommu(&priv->fmt_table.iommu);	/* [한국어] 페이지 테이블 상태 */
	priv->iommu->ops->get_info(priv->iommu, &priv->info);	/* [한국어] 지원 페이지 크기 */

	/*
	 * size_t is used to pass the mapping length, it can be 32 bit, truncate
	 * the pagesizes so we don't use large sizes.
	 */
	priv->info.pgsize_bitmap = (size_t)priv->info.pgsize_bitmap;	/* [한국어] (원 주석: 매핑 길이가 size_t 로 전달되므로 그보다 큰 크기는 쓸 수 없다) */

	priv->smallest_pgsz_lg2 = vaffs(priv->info.pgsize_bitmap);	/* [한국어] 가장 작은 페이지 크기 */
	priv->smallest_pgsz = log2_to_int(priv->smallest_pgsz_lg2);	/* [한국어] 그 실제 값 */
	priv->largest_pgsz_lg2 =	/* [한국어] 가장 큰 페이지 크기 */
		vafls((dma_addr_t)priv->info.pgsize_bitmap) - 1;	/* [한국어] fls 는 1 기반이라 지수로 바꾼다 */

	priv->test_oa =	/* [한국어] 시험이 매핑할 출력 주소 */
		oalog2_mod(0x74a71445deadbeef, priv->common->max_oasz_lg2);	/* [한국어] 눈에 띄는 패턴 — 잘못된 주소가 나오면 바로 보인다 */

	/*
	 * We run out of VA space if the mappings get too big, make something
	 * smaller that can safely pass through dma_addr_t API.
	 */
	va_lg2sz = priv->common->max_vasz_lg2;	/* [한국어] (원 주석: 매핑이 커지면 VA 공간이 모자라 dma_addr_t API 를 통과할 크기로 줄인다) */
	if (IS_32BIT && va_lg2sz > 32)	/* [한국어] 32비트 시스템이면 */
		va_lg2sz = 32;	/* [한국어] 그 폭을 넘는 VA 를 만들 수 없다 */
	priv->safe_pgsize_bitmap =	/* [한국어] 안전하게 쓸 수 있는 크기만 */
		log2_mod(priv->info.pgsize_bitmap, va_lg2sz - 1);	/* [한국어] 절반 아래로 남긴다 */

	return 0;	/* [한국어] 환경이 준비됐다 */
}

#endif
