// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic page table allocator for IOMMUs.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] IOMMU 페이지 테이블 포맷 등록소 (drivers/iommu/io-pgtable.c)
 *
 * === 파일의 역할 ===
 * 페이지 테이블 '포맷' 구현들을 한 표에 모아 두고, 드라이버가 이름으로 골라
 * 쓰게 해 주는 얇은 계층이다. 코드 자체는 100 줄이 채 되지 않지만, 이 분리가
 * 있어서 서로 다른 IOMMU 하드웨어가 같은 페이지 테이블 코드를 공유할 수 있다.
 *
 * 왜 분리가 되는가 — IOMMU 마다 레지스터와 무효화 방식은 제각각이지만, 페이지
 * 테이블의 '형식'은 몇 가지로 수렴한다. ARM 계열은 CPU 와 같은 LPAE 포맷을 쓰고,
 * 그래서 SMMUv2, SMMUv3, Mediatek, Rockchip 등이 io-pgtable-arm.c 하나를 공유한다.
 * 애플 DART 와 ARMv7 짧은 서술자만 자기 포맷을 따로 갖는다.
 *
 * 이 계층이 없다면 각 드라이버가 페이지 테이블 워크·분할·병합 로직을 따로
 * 구현해야 하고, 그 코드는 미묘한 배리어와 캐시 관리 때문에 버그가 나기 쉽다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: 벤더 드라이버가 도메인을 만들 때
 *         → [이 파일] alloc_io_pgtable_ops(포맷, 설정, 쿠키)
 *           → 포맷 구현(io-pgtable-arm.c 등)의 alloc 이 실제 테이블을 만든다
 *         ← struct io_pgtable_ops (map_pages / unmap_pages / iova_to_phys)
 *       이후 iommu.c 의 iommu_map 이 드라이버의 map_pages 를 부르면, 드라이버는
 *       대개 그 호출을 이 ops 로 그대로 넘긴다.
 *
 * 설정(io_pgtable_cfg)이 양방향 통신 수단이라는 점이 중요하다. 드라이버가 입력으로
 * 주소 폭·페이지 크기·일관성 여부를 채워 넣으면, 포맷 구현이 출력으로 하드웨어
 * 레지스터에 쓸 값(TCR, TTBR, MAIR 등)을 채워 돌려준다.
 *
 * === 타 모듈과의 연결 ===
 * - 포맷 구현: io-pgtable-arm.c(LPAE), io-pgtable-arm-v7s.c, io-pgtable-dart.c.
 *   각자 init_fns 구조체를 노출하고 이 파일의 표에 등록된다.
 * - 벤더 드라이버: arm-smmu, arm-smmu-v3, mtk_iommu, rockchip 등이 사용자다.
 * - iommu-pages.c: 포맷 구현이 테이블 페이지를 그쪽에서 얻는다. 다만 일부
 *   드라이버는 자기 메모리 풀을 쓰고 싶어 해서, cfg 에 alloc/free 콜백을 넘기는
 *   커스텀 할당자 경로가 있다.
 * - TLB 관리: cfg->tlb 콜백으로 무효화를 드라이버에 되돌린다. 포맷 구현은 언제
 *   무효화가 필요한지 알지만 어떻게 하는지는 모르기 때문이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - io_pgtable_init_table[]  : 포맷 → 구현 함수표. 빌드 설정에 따라 채워진다.
 * - alloc_io_pgtable_ops()   : 포맷을 골라 페이지 테이블을 만든다.
 * - free_io_pgtable_ops()    : 그 짝. 해제 전에 TLB 를 통째로 비운다.
 * - check_custom_allocator() : 커스텀 할당자 인자의 정합성을 검사한다.
 */
#include <linux/bug.h>	/* [한국어] WARN/BUG 매크로 */
#include <linux/io-pgtable.h>	/* [한국어] io_pgtable_cfg/ops 정의와 각 포맷의 init_fns 선언 */
#include <linux/kernel.h>	/* [한국어] 공통 매크로 */
#include <linux/types.h>	/* [한국어] 기본 타입 */

static const struct io_pgtable_init_fns *	/* [한국어] 포맷 번호로 구현을 찾는 표. 빌드에 포함된 포맷만 채워지고 나머지는 NULL 이라, 지원하지 않는 포맷 요청은 자연히 실패한다 */
io_pgtable_init_table[IO_PGTABLE_NUM_FMTS] = {	/* [한국어] 포맷 개수만큼의 배열 */
#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE	/* [한국어] ARM LPAE 계열 — CPU 의 긴 서술자 포맷과 같아서 가장 널리 쓰인다 */
	[ARM_32_LPAE_S1] = &io_pgtable_arm_32_lpae_s1_init_fns,	/* [한국어] 32비트 stage-1 (OS 가 관리하는 번역) */
	[ARM_32_LPAE_S2] = &io_pgtable_arm_32_lpae_s2_init_fns,	/* [한국어] 32비트 stage-2 (하이퍼바이저가 관리하는 두 번째 번역) */
	[ARM_64_LPAE_S1] = &io_pgtable_arm_64_lpae_s1_init_fns,	/* [한국어] 64비트 stage-1 — 서버·모바일 SMMU 의 기본 */
	[ARM_64_LPAE_S2] = &io_pgtable_arm_64_lpae_s2_init_fns,	/* [한국어] 64비트 stage-2 — 가상화에서 게스트의 IOVA 를 다시 번역한다 */
	[ARM_MALI_LPAE] = &io_pgtable_arm_mali_lpae_init_fns,	/* [한국어] Mali GPU 의 변형 LPAE. 서술자 비트 배치가 미묘하게 달라 별도 구현이다 */
#endif
#ifdef CONFIG_IOMMU_IO_PGTABLE_DART	/* [한국어] 애플 실리콘의 DART IOMMU */
	[APPLE_DART] = &io_pgtable_apple_dart_init_fns,	/* [한국어] 1세대 */
	[APPLE_DART2] = &io_pgtable_apple_dart_init_fns,	/* [한국어] 2세대 — 같은 구현이 두 세대를 처리한다 (cfg 로 구분) */
#endif
#ifdef CONFIG_IOMMU_IO_PGTABLE_ARMV7S	/* [한국어] ARMv7 짧은 서술자 — 32비트 전용 구형 포맷 */
	[ARM_V7S] = &io_pgtable_arm_v7s_init_fns,	/* [한국어] 일부 Mediatek SoC 가 아직 쓴다 */
#endif
};

/*
 * [한국어]
 * check_custom_allocator - 커스텀 페이지 할당자 인자가 정합한지 검사한다
 *
 * @fmt:    요청된 페이지 테이블 포맷
 * @cfg:    드라이버가 채운 설정
 * @return: 0 이면 문제 없음, -EINVAL 이면 모순
 *
 * 대부분의 드라이버는 iommu-pages.c 의 공용 할당자를 쓰지만, 페이지 테이블을
 * 특정 메모리 영역에 두어야 하는 하드웨어가 있다 — 주소 폭 제한이 있거나, 전용
 * SRAM 을 써야 하거나, 미리 예약한 풀에서만 잡아야 하는 경우다. 그런 드라이버는
 * cfg 에 alloc/free 콜백을 채워 넣는다.
 *
 * 검사가 두 가지다. 짝이 맞아야 하고(한쪽만 주면 할당과 해제가 다른 풀을 쓴다),
 * 포맷 구현이 그 기능을 지원한다고 알렸어야 한다. 후자를 조용히 무시하지 않고
 * 거절하는 것이 중요한데, 드라이버가 자기 풀을 쓴다고 믿는 채로 다른 메모리를
 * 받으면 하드웨어 제약을 어기게 되기 때문이다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 문맥.
 *
 * 호출 체인: alloc_io_pgtable_ops → [이 함수]
 */
static int check_custom_allocator(enum io_pgtable_fmt fmt,
				  struct io_pgtable_cfg *cfg)
{
	/* No custom allocator, no need to check the format. */
	if (!cfg->alloc && !cfg->free)	/* [한국어] 커스텀 할당자를 쓰지 않는 보통의 경우 */
		return 0;	/* [한국어] 검사할 것이 없다 */

	/* When passing a custom allocator, both the alloc and free
	 * functions should be provided.
	 */
	if (!cfg->alloc || !cfg->free)	/* [한국어] 한쪽만 주었다 */
		return -EINVAL;	/* [한국어] 짝이 맞지 않으면 할당과 해제가 다른 풀을 쓰게 된다 (위 영어 주석) */

	/* Make sure the format supports custom allocators. */
	if (io_pgtable_init_table[fmt]->caps & IO_PGTABLE_CAP_CUSTOM_ALLOCATOR)	/* [한국어] 이 포맷 구현이 커스텀 할당자를 지원한다고 알렸다면 */
		return 0;	/* [한국어] 허용 */

	return -EINVAL;	/* [한국어] 지원하지 않는 포맷에 커스텀 할당자를 주면 조용히 무시되는 대신 거절한다 — 드라이버가 자기 풀을 쓴다고 믿는 채로 다른 메모리를 받으면 안 되기 때문 */
}

/*
 * [한국어]
 * alloc_io_pgtable_ops - 포맷을 골라 페이지 테이블을 만든다
 *
 * @fmt:    페이지 테이블 포맷 (ARM_64_LPAE_S1 등)
 * @cfg:    입출력 겸용 설정. 드라이버가 주소 폭·페이지 크기·일관성 여부를 채워
 *          넣으면, 포맷 구현이 하드웨어 레지스터에 쓸 값을 채워 돌려준다.
 * @cookie: 드라이버 문맥. TLB 콜백에 그대로 되돌아온다.
 * @return: 페이지 테이블 조작 함수표, 실패하면 NULL
 *
 * 벤더 드라이버가 도메인을 만들 때 부르는 진입점이다. 이 호출이 성공하면 드라이버는
 * 두 가지를 얻는다 — 페이지 테이블을 다룰 ops 와, 하드웨어를 설정할 레지스터 값이다.
 * 후자가 cfg 를 통해 돌아온다는 점이 이 API 의 특징이며, 그래서 인자가 const 가
 * 아니다.
 *
 * cookie 는 순환 참조를 끊는 장치다. 포맷 구현은 TLB 무효화가 언제 필요한지 알지만
 * 어떻게 하는지는 모르므로 cfg->tlb 콜백을 부르는데, 그때 이 값을 함께 넘겨
 * 드라이버가 어느 도메인의 일인지 알 수 있게 한다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: arm-smmu, arm-smmu-v3, mtk_iommu 등 → [이 함수]
 *            → 포맷 구현의 alloc
 */
struct io_pgtable_ops *alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
					    struct io_pgtable_cfg *cfg,
					    void *cookie)
{
	struct io_pgtable *iop;	/* [한국어] 포맷 구현이 만들 페이지 테이블 객체 */
	const struct io_pgtable_init_fns *fns;	/* [한국어] 그 포맷의 함수표 */

	if (fmt >= IO_PGTABLE_NUM_FMTS)	/* [한국어] 표 범위를 벗어난 포맷 번호 */
		return NULL;	/* [한국어] 잘못된 요청 */

	if (check_custom_allocator(fmt, cfg))	/* [한국어] 커스텀 할당자 인자가 정합한지 */
		return NULL;	/* [한국어] 모순된 설정 */

	fns = io_pgtable_init_table[fmt];	/* [한국어] 포맷 구현을 꺼낸다 */
	if (!fns)	/* [한국어] 빌드에 포함되지 않은 포맷 */
		return NULL;	/* [한국어] 이 커널로는 쓸 수 없다 */

	iop = fns->alloc(cfg, cookie);	/* [한국어] 포맷 구현이 실제 테이블을 만든다. 동시에 cfg 를 채워 드라이버가 하드웨어 레지스터에 쓸 값(TTBR, TCR, MAIR 등)을 돌려준다 — 인자가 입출력 겸용인 이유다 */
	if (!iop)	/* [한국어] 생성 실패 */
		return NULL;	/* [한국어] 도메인을 만들 수 없다 */

	iop->fmt	= fmt;	/* [한국어] 해제 때 어느 구현의 free 를 부를지 기억해 둔다 */
	iop->cookie	= cookie;	/* [한국어] TLB 콜백에 되돌려 줄 드라이버 문맥. 포맷 구현은 이 값을 해석하지 않는다 */
	iop->cfg	= *cfg;	/* [한국어] 설정을 복사해 보관한다 — 호출자의 스택에 있던 것일 수 있어 참조로 들고 있을 수 없다 */

	return &iop->ops;	/* [한국어] 드라이버는 이 ops 로 map_pages/unmap_pages/iova_to_phys 를 부른다 */
}
EXPORT_SYMBOL_GPL(alloc_io_pgtable_ops);	/* [한국어] 벤더 드라이버가 도메인을 만들 때 부른다 */

/*
 * It is the IOMMU driver's responsibility to ensure that the page table
 * is no longer accessible to the walker by this point.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * free_io_pgtable_ops - 페이지 테이블을 해제한다
 *
 * @ops: alloc 이 돌려준 함수표
 *
 * 해제 전에 TLB 를 통째로 비우는 것이 이 함수가 하는 유일한 판단이다. 하드웨어
 * 워커가 더 이상 이 테이블에 접근하지 않게 만드는 것은 드라이버 책임이지만
 * (위 영어 주석), 이미 캐시된 번역까지 지우는 것은 여기서 보장한다. 그러지 않으면
 * 반납된 페이지가 다른 용도로 재사용된 뒤에도 옛 번역이 살아 있게 된다.
 *
 * fmt 를 객체에 보관해 둔 덕분에 호출자가 포맷을 다시 알려 줄 필요가 없다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버의 도메인 free → [이 함수] → 포맷 구현의 free
 */
void free_io_pgtable_ops(struct io_pgtable_ops *ops)
{
	struct io_pgtable *iop;	/* [한국어] 해제할 페이지 테이블 객체 */

	if (!ops)	/* [한국어] NULL 도 안전하게 받는다 */
		return;	/* [한국어] 할 일 없음 */

	iop = io_pgtable_ops_to_pgtable(ops);	/* [한국어] ops 를 품고 있는 객체로 되짚는다 */
	io_pgtable_tlb_flush_all(iop);	/* [한국어] 해제 전에 TLB 를 통째로 비운다. 위 영어 주석대로 워커가 더 이상 이 테이블에 접근하지 않게 만드는 것은 드라이버 책임이지만, 캐시된 번역까지 지우는 것은 여기서 보장한다 */
	io_pgtable_init_table[iop->fmt]->free(iop);	/* [한국어] 포맷 구현이 테이블 페이지들을 반납한다 */
}
EXPORT_SYMBOL_GPL(free_io_pgtable_ops);	/* [한국어] 도메인 해제 경로 */
