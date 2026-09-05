// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * IOMMU API debug page alloc sanitizer
 */

/*
 * [한국어 설명] 매핑된 채로 반납되는 페이지를 잡아내는 검사기 (iommu-debug-pagealloc.c)
 *
 * === 파일의 역할 ===
 * (위 영어 주석 참고) DMA 버그 중에서 가장 잡기 어려운 종류를 겨냥한
 * 디버그 기능이다. 드라이버가 페이지를 IOMMU 에 매핑해 두고 그것을
 * 해제하지 않은 채 메모리를 반납하면, 그 페이지는 다른 용도로 재사용되는데
 * 장치는 여전히 그 주소로 DMA 를 낼 수 있다. 결과는 무작위로 망가지는
 * 메모리이고, 원인이 된 코드와 증상이 나타나는 시점이 아주 멀어 추적이
 * 거의 불가능하다.
 *
 * 이 파일의 착상은 단순하다 — 페이지마다 "지금 몇 군데 IOMMU 에 매핑되어
 * 있는가"를 세어 두고, 그 페이지가 반납될 때 그 수가 0 인지 확인한다.
 * 0 이 아니면 매핑된 채 반납된 것이므로 그 자리에서 경고를 띄우고,
 * page_owner 정보로 누가 그 페이지를 잡았는지까지 함께 보여 준다.
 *
 * 계수를 어디에 둘 것인가가 문제인데, page_ext 라는 커널 장치를 쓴다.
 * 페이지 구조체를 키우지 않고도 페이지마다 여분의 자료를 매달 수 있는
 * 방식이며, 필요할 때만 그 메모리를 잡는다. 그래서 이 기능을 끄면
 * 메모리를 한 바이트도 쓰지 않는다.
 *
 * 성능 비용이 커서 기본으로 꺼져 있고, 부팅 인자로만 켤 수 있다.
 * static key 로 감싸 두어, 꺼져 있으면 호출부의 분기 자체가 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 두 방향에서 갈고리가 걸린다.
 *
 *   iommu_map()   → __iommu_debug_map()        ← 계수를 올린다
 *   iommu_unmap() → __iommu_debug_unmap_begin/end() ← 계수를 내린다
 *
 *   페이지 반납 (free_pages 등)
 *     → __iommu_debug_check_unmapped()          ← 계수가 0 인지 확인
 *
 * 해제 쪽이 begin/end 두 갈래로 나뉜 것이 요점이다. 해제는 부분적으로만
 * 성공할 수 있는데, 그 시점에는 이미 매핑이 사라져 물리 주소를 되짚을
 * 수 없다. 그래서 해제 전에 미리 훑어 계수를 내려 두고, 실제로 풀리지
 * 않은 부분만 나중에 되올린다.
 *
 * 실행 컨텍스트: 매핑·해제 경로 그대로 — 원자적 문맥일 수 있어 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 매핑·해제 경로에서 이 파일의 함수를 부른다.
 * - include/linux/iommu-debug-pagealloc.h: static key 로 감싼 얇은 껍데기
 *   함수들 — 기능이 꺼져 있으면 아무 일도 하지 않는다.
 * - page_ext: 페이지마다 여분 자료를 매다는 커널 장치.
 * - page_owner: 그 페이지를 누가 잡았는지 기록해 두는 다른 디버그 기능 —
 *   여기서 경고를 띄울 때 그 정보를 함께 보여 준다.
 * - 페이지 할당기: 페이지를 반납할 때 __iommu_debug_check_unmapped() 를 부른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iommu_debug_metadata: 페이지마다 매달리는 계수 하나가 전부다.
 * - __iommu_debug_map(): 매핑된 범위의 페이지 계수를 올린다.
 * - __iommu_debug_unmap_begin()/end(): 해제 전에 내리고, 실패한 만큼 되올린다.
 * - __iommu_debug_check_unmapped(): 반납되는 페이지의 계수가 0 인지 확인한다.
 * - iommu_debug_page_size(): 계수의 단위를 IOMMU 의 최소 페이지 크기로 고정한다 —
 *   매핑과 해제가 같은 단위를 써야 이중 계수가 생기지 않는다.
 */
#include <linux/atomic.h>	/* [한국어] 계수를 원자적으로 올리고 내린다 — 여러 CPU 가 같은 페이지를 매핑할 수 있다. */
#include <linux/iommu.h>	/* [한국어] iommu 도메인과 주소 변환 조회. */
#include <linux/iommu-debug-pagealloc.h>	/* [한국어] 이 파일이 구현하는 함수들의 선언과 static key. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */
#include <linux/page_ext.h>	/* [한국어] 페이지마다 여분 자료를 매다는 장치. */
#include <linux/page_owner.h>	/* [한국어] 누가 그 페이지를 잡았는지 보여 주는 다른 디버그 기능. */

#include "iommu-priv.h"	/* [한국어] iommu 코어 내부 선언. */

static bool needed;	/* [한국어] 부팅 인자로 이 기능을 켜 달라고 했는가. 아주 이른 시점에 정해져야 해서 early_param 으로 읽는다. */
DEFINE_STATIC_KEY_FALSE(iommu_debug_initialized);	/* [한국어] 실제로 켜졌는가. 꺼져 있으면 호출부의 분기가 코드에서 아예 사라져 비용이 0 이 된다. */

/* [한국어] 페이지 하나에 매달리는 자료 — 계수 하나가 전부다. */
struct iommu_debug_metadata {
	/* [한국어] 이 페이지가 지금 몇 군데 IOMMU 매핑에 걸려 있는가.
	 * 설정자: 매핑할 때 올리고 해제할 때 내린다.
	 * 읽는 자: 페이지가 반납될 때 0 인지 확인하는 검사.
	 * 값 범위: 0 이상. 음수가 되면 매핑·해제의 짝이 어긋난 것이라 경고한다.
	 * 동기화: 여러 CPU 가 같은 페이지를 동시에 매핑할 수 있어 원자 연산으로만 다룬다. */
	atomic_t ref;
};

/*
 * [한국어]
 * need_iommu_debug - 이 페이지 확장 자료가 필요한가
 *
 * @return: 부팅 인자로 켰으면 참.
 *
 * page_ext 계층이 부팅 초기에 물어보는 갈고리다. 참이면 페이지마다
 * 위 구조체만큼의 메모리를 더 잡고, 거짓이면 한 바이트도 쓰지 않는다.
 * 그래서 이 기능을 켜지 않은 시스템에는 아무 비용도 없다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   page_ext 초기화 → ops->need = [이 함수]
 */
static __init bool need_iommu_debug(void)
{
	return needed;	/* [한국어] 부팅 인자가 정한 값 — 이 시점에는 이미 읽혀 있다. */
}

/* [한국어] page_ext 계층에 등록하는 서술.
 *
 * 크기와 "필요한가" 갈고리 둘뿐이다. 초기화 갈고리가 없는 이유는
 * page_ext 가 잡은 메모리를 0 으로 채워 주기 때문이다 — 계수가 0 에서
 * 시작하는 것이 곧 우리가 원하는 초기 상태다. */
struct page_ext_operations page_iommu_debug_ops = {
	.size = sizeof(struct iommu_debug_metadata),	/* [한국어] 페이지마다 이만큼 더 잡아 달라. */
	.need = need_iommu_debug,	/* [한국어] 정말 잡을지는 이 갈고리가 답한다. */
};

/*
 * [한국어]
 * get_iommu_data - 페이지 확장 자료에서 우리 몫을 꺼낸다
 *
 * @page_ext: 그 페이지의 확장 자료.
 * @return: 우리가 등록한 계수 구조체.
 *
 * page_ext 는 여러 디버그 기능이 나눠 쓰는 공간이라, 우리 몫이 어디에
 * 있는지는 등록한 서술로 찾아야 한다. 그 계산을 감싼 얇은 껍데기다.
 *
 * 실행 컨텍스트: 매핑·해제·검사 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_debug_inc_page()/dec_page()/page_count() → [이 함수]
 */
static struct iommu_debug_metadata *get_iommu_data(struct page_ext *page_ext)
{
	return page_ext_data(page_ext, &page_iommu_debug_ops);	/* [한국어] 등록한 서술로 우리 몫의 오프셋을 찾는다. */
}

/*
 * [한국어]
 * iommu_debug_inc_page - 그 물리 페이지의 매핑 계수를 올린다
 *
 * @phys: 페이지의 물리 주소.
 *
 * 매핑이 하나 늘었음을 기록한다. 확장 자료가 없는 페이지 — 예컨대
 * 페이지 구조체가 없는 장치 메모리나 예약 구간 — 는 조용히 건너뛴다.
 *
 * 계수를 올린 뒤 0 이하면 경고한다. 그런 값이 나오는 것은 계수가
 * 넘쳤거나 어딘가에서 잘못 내렸다는 뜻이다.
 *
 * 실행 컨텍스트: 매핑 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __iommu_debug_map()/update_iova() → [이 함수]
 */
static void iommu_debug_inc_page(phys_addr_t phys)
{
	struct page_ext *page_ext = page_ext_from_phys(phys);	/* [한국어] 그 물리 주소의 확장 자료를 찾는다 (참조를 잡는다). */
	struct iommu_debug_metadata *d;	/* [한국어] 우리 몫. */

	if (!page_ext)	/* [한국어] 페이지 구조체가 없는 메모리라면 — 장치 메모리나 예약 구간. */
		return;	/* [한국어] 셀 수 없으니 건너뛴다. */

	d = get_iommu_data(page_ext);	/* [한국어] 계수 구조체를 꺼낸다. */
	WARN_ON(atomic_inc_return_relaxed(&d->ref) <= 0);	/* [한국어] 올린 뒤 0 이하라면 넘쳤거나 잘못 내려진 것이다. 순서 보장은 필요 없어 relaxed 로 충분하다. */
	page_ext_put(page_ext);	/* [한국어] 잡았던 참조를 놓는다. */
}

/*
 * [한국어]
 * iommu_debug_dec_page - 그 물리 페이지의 매핑 계수를 내린다
 *
 * @phys: 페이지의 물리 주소.
 *
 * 매핑이 하나 줄었음을 기록한다. 내린 뒤 음수가 되면 매핑보다 해제가
 * 많았다는 뜻이라 경고한다 — 그 자체가 코드 버그의 신호다.
 *
 * 실행 컨텍스트: 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __iommu_debug_update_iova() → [이 함수]
 */
static void iommu_debug_dec_page(phys_addr_t phys)
{
	struct page_ext *page_ext = page_ext_from_phys(phys);	/* [한국어] 확장 자료를 찾는다. */
	struct iommu_debug_metadata *d;	/* [한국어] 우리 몫. */

	if (!page_ext)	/* [한국어] 셀 수 없는 메모리라면. */
		return;

	d = get_iommu_data(page_ext);	/* [한국어] 계수 구조체를 꺼낸다. */
	WARN_ON(atomic_dec_return_relaxed(&d->ref) < 0);	/* [한국어] 음수가 되면 매핑보다 해제가 많았다는 뜻 — 짝이 어긋난 것이다. */
	page_ext_put(page_ext);	/* [한국어] 참조를 놓는다. */
}

/*
 * IOMMU page size doesn't have to match the CPU page size. So, we use
 * the smallest IOMMU page size to refcount the pages in the vmemmap.
 * That is important as both map and unmap has to use the same page size
 * to update the refcount to avoid double counting the same page.
 * And as we can't know from iommu_unmap() what was the original page size
 * used for map, we just use the minimum supported one for both.
 */
/*
 * [한국어]
 * iommu_debug_page_size - 계수의 단위가 될 페이지 크기를 정한다
 *
 * @domain: 대상 도메인.
 * @return: 그 도메인이 지원하는 가장 작은 페이지 크기.
 *
 * (위 영어 주석 참고) IOMMU 의 페이지 크기는 CPU 의 것과 다를 수 있고,
 * 한 도메인 안에서도 매핑마다 다를 수 있다. 그런데 계수를 올리고 내리는
 * 단위가 다르면 같은 페이지를 두 번 세거나 덜 세게 된다.
 *
 * 특히 해제 쪽이 문제다 — iommu_unmap() 만 보고는 그 범위가 원래 어떤
 * 크기로 매핑됐는지 알 수 없다. 그래서 양쪽 모두 "가장 작은 크기"라는
 * 공통 단위를 쓰기로 못 박았다. 큰 페이지로 매핑됐더라도 작은 단위로
 * 여러 번 세면, 해제 때도 같은 횟수만큼 내려져 짝이 맞는다.
 *
 * 실행 컨텍스트: 매핑·해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __iommu_debug_map()/update_iova() → [이 함수]
 */
static size_t iommu_debug_page_size(struct iommu_domain *domain)
{
	return 1UL << __ffs(domain->pgsize_bitmap);	/* [한국어] 지원하는 크기들 중 가장 낮은 비트가 곧 가장 작은 크기다. */
}

/*
 * [한국어]
 * iommu_debug_page_count - 그 페이지가 아직 매핑되어 있는가
 *
 * @page: 검사할 페이지.
 * @return: 매핑이 남아 있으면 참.
 *
 * 계수가 0 이 아니면 아직 어딘가의 IOMMU 에 매핑되어 있다는 뜻이다.
 * 반납되는 페이지에 대해 이 값이 참이면 누수다.
 *
 * 위 두 함수와 달리 확장 자료가 없는 경우를 검사하지 않는데, 반납되는
 * 페이지는 반드시 페이지 구조체를 갖고 있기 때문이다.
 *
 * 실행 컨텍스트: 페이지 반납 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __iommu_debug_check_unmapped() → [이 함수]
 */
static bool iommu_debug_page_count(const struct page *page)
{
	unsigned int ref;	/* [한국어] 읽은 계수. */
	struct page_ext *page_ext = page_ext_get(page);	/* [한국어] 그 페이지의 확장 자료 (참조를 잡는다). */
	struct iommu_debug_metadata *d = get_iommu_data(page_ext);	/* [한국어] 우리 몫. */

	ref = atomic_read(&d->ref);	/* [한국어] 지금 계수를 읽는다. */
	page_ext_put(page_ext);	/* [한국어] 참조를 놓는다 — 값을 이미 복사했으므로 안전하다. */
	return ref != 0;	/* [한국어] 0 이 아니면 아직 매핑이 남아 있다. */
}

/*
 * [한국어]
 * __iommu_debug_check_unmapped - 반납되는 페이지들이 정말 해제됐는지 확인한다
 *
 * @page: 반납되는 첫 페이지.
 * @numpages: 반납되는 페이지 수.
 *
 * 이 기능의 목적이 실현되는 자리다. 페이지가 메모리 할당기로 돌아갈 때
 * 아직 IOMMU 에 매핑되어 있으면, 그 페이지는 곧 다른 용도로 재사용되는데
 * 장치는 여전히 그리로 DMA 를 낼 수 있다.
 *
 * 그런 페이지를 찾으면 경고를 띄우고 page_owner 정보를 함께 보여 준다 —
 * 누가 그 페이지를 잡았는지 알아야 어느 드라이버의 버그인지 좁힐 수 있다.
 *
 * 실행 컨텍스트: 페이지 반납 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   페이지 할당기의 반납 경로 → [이 함수] → dump_page_owner()
 */
void __iommu_debug_check_unmapped(const struct page *page, int numpages)
{
	while (numpages--) {	/* [한국어] 반납되는 페이지를 하나씩 본다. */
		if (WARN_ON(iommu_debug_page_count(page))) {	/* [한국어] 아직 매핑이 남아 있다면 — 그 자체가 심각한 버그다. */
			pr_warn("iommu: Detected page leak!\n");	/* [한국어] 무엇을 찾았는지 분명히 알린다. */
			dump_page_owner(page);	/* [한국어] 누가 이 페이지를 잡았는지 보여 준다 — 어느 드라이버의 버그인지 좁히는 열쇠다. */
		}
		page++;	/* [한국어] 다음 페이지로. */
	}
}

/*
 * [한국어]
 * __iommu_debug_map - 매핑된 범위의 페이지 계수를 올린다
 *
 * @domain: 매핑이 걸린 도메인.
 * @phys: 매핑된 물리 주소의 시작.
 * @size: 그 길이.
 *
 * 매핑이 성공한 뒤에 불려, 그 범위의 모든 페이지 계수를 올린다.
 * 단위는 이 도메인의 최소 페이지 크기로 고정되어, 해제 쪽과 짝이 맞는다.
 *
 * 실행 컨텍스트: 매핑 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_map() → iommu_debug_map() 껍데기 → [이 함수]
 */
void __iommu_debug_map(struct iommu_domain *domain, phys_addr_t phys, size_t size)
{
	size_t off, end;	/* [한국어] 범위 안의 오프셋과, 넘침 검사용 끝 주소. */
	size_t page_size = iommu_debug_page_size(domain);	/* [한국어] 계수의 단위. */

	if (WARN_ON(!phys || check_add_overflow(phys, size, &end)))	/* [한국어] 주소가 0 이거나 더하기가 넘치면 잘못된 요청이다. */
		return;	/* [한국어] 세지 않고 나간다 — 넘친 범위를 돌면 무한 반복이 될 수 있다. */

	for (off = 0 ; off < size ; off += page_size)	/* [한국어] 최소 단위로 잘라 가며. */
		iommu_debug_inc_page(phys + off);	/* [한국어] 각 페이지의 계수를 올린다. */
}

/*
 * [한국어]
 * __iommu_debug_update_iova - 그 IOVA 범위가 가리키는 페이지들의 계수를 고친다
 *
 * @domain: 대상 도메인.
 * @iova: 범위의 시작 (장치가 보는 주소).
 * @size: 그 길이.
 * @inc: 참이면 올리고, 거짓이면 내린다.
 *
 * 매핑 쪽과 달리 물리 주소를 모른 채 시작한다. 그래서 최소 단위마다
 * 주소 변환을 조회해 물리 주소를 알아내야 하고, 그만큼 비싸다 —
 * 이 기능이 성능 부담이 큰 이유가 여기 있다.
 *
 * 매핑이 없는 자리는 조용히 건너뛴다. 해제 요청 범위 안에 원래 매핑되지
 * 않은 구멍이 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __iommu_debug_unmap_begin()/end() → [이 함수] → iommu_iova_to_phys()
 */
static void __iommu_debug_update_iova(struct iommu_domain *domain,
				      unsigned long iova, size_t size, bool inc)
{
	size_t off, end;	/* [한국어] 범위 안의 오프셋과 넘침 검사용 끝 주소. */
	size_t page_size = iommu_debug_page_size(domain);	/* [한국어] 계수의 단위 — 매핑 쪽과 같아야 한다. */

	if (WARN_ON(check_add_overflow(iova, size, &end)))	/* [한국어] 더하기가 넘치면 잘못된 요청이다. */
		return;

	for (off = 0 ; off < size ; off += page_size) {	/* [한국어] 최소 단위로 잘라 가며. */
		phys_addr_t phys = iommu_iova_to_phys(domain, iova + off);	/* [한국어] 그 자리가 어느 물리 주소로 이어지는지 표를 걸어 알아낸다 — 이 조회가 이 기능의 주된 비용이다. */

		if (!phys)	/* [한국어] 매핑이 없는 자리라면. */
			continue;	/* [한국어] 셀 것이 없다 — 요청 범위 안에 구멍이 있을 수 있다. */

		if (inc)	/* [한국어] 되올리는 경우. */
			iommu_debug_inc_page(phys);
		else	/* [한국어] 내리는 경우. */
			iommu_debug_dec_page(phys);
	}
}

/*
 * [한국어]
 * __iommu_debug_unmap_begin - 해제하기 전에 미리 계수를 내린다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 범위의 시작.
 * @size: 그 길이.
 *
 * 해제를 시작하기 전에 불린다. 이 순서가 필수인 이유가 이 파일의
 * 설계에서 가장 미묘한 대목이다 — 해제가 끝나면 매핑이 사라져
 * iommu_iova_to_phys() 로 물리 주소를 되짚을 수 없다. 그래서 아직
 * 매핑이 살아 있는 지금 훑어 두어야 한다.
 *
 * 실행 컨텍스트: 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_unmap() → 껍데기 → [이 함수] → __iommu_debug_update_iova()
 */
void __iommu_debug_unmap_begin(struct iommu_domain *domain,
			       unsigned long iova, size_t size)
{
	__iommu_debug_update_iova(domain, iova, size, false);	/* [한국어] 아직 매핑이 살아 있을 때 훑어 계수를 내린다. */
}

/*
 * [한국어]
 * __iommu_debug_unmap_end - 실제로 풀리지 않은 만큼 계수를 되올린다
 *
 * @domain: 대상 도메인.
 * @iova: 해제 요청한 범위의 시작.
 * @size: 요청한 길이.
 * @unmapped: 실제로 풀린 길이.
 *
 * 해제는 부분적으로만 성공할 수 있다. begin 에서 요청 범위 전체의 계수를
 * 미리 내렸으므로, 실제로 풀리지 않은 부분은 여기서 되올려 짝을 맞춘다.
 *
 * 전부 풀렸으면 할 일이 없고, 요청보다 많이 풀렸다고 보고되면 그 자체가
 * 하위 드라이버의 버그라 경고만 남기고 손대지 않는다.
 *
 * 실행 컨텍스트: 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_unmap() → 껍데기 → [이 함수] → __iommu_debug_update_iova()
 */
void __iommu_debug_unmap_end(struct iommu_domain *domain,
			     unsigned long iova, size_t size,
			     size_t unmapped)
{
	if ((unmapped == size) || WARN_ON_ONCE(unmapped > size))	/* [한국어] 전부 풀렸거나, 요청보다 많이 풀렸다고 보고됐다면. */
		return;	/* [한국어] 앞의 경우는 할 일이 없고, 뒤의 경우는 하위 드라이버의 버그라 손대지 않는다. */

	/* If unmap failed, re-increment the refcount. */
	/* [한국어] (위 영어 주석 참고) begin 에서 요청 범위 전체를 내렸으므로,
	 * 실제로 풀리지 않은 뒷부분은 여기서 되올려야 짝이 맞는다. */
	__iommu_debug_update_iova(domain, iova + unmapped,	/* [한국어] 풀린 만큼 건너뛴 자리부터. */
				  size - unmapped, true);	/* [한국어] 남은 길이만큼 되올린다. */
}

/*
 * [한국어]
 * iommu_debug_init - 이 검사기를 실제로 켠다
 *
 * 부팅 인자로 요청했을 때만 static key 를 켠다. 그 키가 켜지기 전까지는
 * 호출부의 분기가 코드에서 아예 사라져 있어, 이 기능을 쓰지 않는
 * 시스템에는 아무 비용도 없다.
 *
 * 성능 경고를 남기는 것이 요점이다 — 매핑·해제마다 주소 변환 조회가
 * 더해지므로 부담이 눈에 띄게 크고, 그것을 모르고 켜 두면 성능 문제를
 * 엉뚱한 데서 찾게 된다.
 *
 * 실행 컨텍스트: iommu 코어 초기화. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu 코어 초기화 → [이 함수] → static_branch_enable()
 */
void iommu_debug_init(void)
{
	if (!needed)	/* [한국어] 부팅 인자로 요청하지 않았다면. */
		return;	/* [한국어] static key 를 꺼 둔 채로 두어 비용을 0 으로 유지한다. */

	pr_info("iommu: Debugging page allocations, expect overhead or disable iommu.debug_pagealloc");	/* [한국어] 부담이 크다는 사실을 분명히 알린다 — 모르고 켜 두면 성능 문제를 엉뚱한 데서 찾게 된다. */
	static_branch_enable(&iommu_debug_initialized);	/* [한국어] 이 순간부터 호출부의 분기가 살아나 갈고리가 실제로 불린다. */
}

/*
 * [한국어]
 * iommu_debug_pagealloc - 부팅 인자를 읽어 이 기능을 켤지 정한다
 *
 * @str: 인자에 적힌 값.
 * @return: 0 성공, 음수면 값을 해석할 수 없다.
 *
 * page_ext 는 메모리 관리가 올라오기 전에 크기를 정해야 해서, 이 인자를
 * 아주 이른 시점에 읽어야 한다. 그래서 보통의 module_param 이 아니라
 * early_param 을 쓴다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 잠들지 않는다.
 *
 * 호출 체인:
 *   커널 명령줄 해석 → [이 함수] → kstrtobool()
 */
static int __init iommu_debug_pagealloc(char *str)
{
	return kstrtobool(str, &needed);	/* [한국어] "1"·"y"·"on" 같은 값을 참으로 해석해 담는다. */
}
early_param("iommu.debug_pagealloc", iommu_debug_pagealloc);	/* [한국어] page_ext 가 크기를 정하기 전에 읽혀야 해서 이른 인자로 등록한다. */
