// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2006-2009, Intel Corporation.
 *
 * Author: Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>
 */

/*
 * [한국어 설명] IOVA 주소 공간 할당자 (drivers/iommu/iova.c)
 *
 * === 파일의 역할 ===
 * IOMMU 도메인 하나가 가진 IO 가상 주소 공간에서 "아직 아무도 쓰지 않는 구간"을
 * 떼어 주고 돌려받는 할당자다. iommu.c 가 IOVA→물리 번역을 페이지 테이블에 기입
 * 한다면, 이 파일은 그보다 앞서 "어느 IOVA 를 쓸 것인가"를 정한다. 장치 드라이버가
 * dma_map_page 를 부를 때마다 이 할당자가 한 번 돈다고 보면 된다.
 *
 * 자료구조는 두 층이다. 아래층은 사용 중인 구간들을 시작 pfn 기준으로 정렬해 담은
 * 레드-블랙 트리이고, 위층은 CPU 별 매그니튜드 캐시(rcache)다. 트리는 정확하지만
 * 스핀락 하나로 보호되어 다중 코어에서 병목이 되므로, 흔한 크기(1~32 페이지)의
 * 할당은 CPU 로컬 캐시에서 락 없이 처리하고 트리까지 내려가지 않게 만들었다.
 * 고성능 NVMe 나 100G NIC 에서 IOMMU 를 켜도 성능이 버티는 이유의 절반이 여기 있다.
 *
 * 할당 방향이 위에서 아래(top-down)인 것도 의도된 설계다. 낮은 주소를 비워 두면
 * 32비트 DMA 마스크만 지원하는 장치가 쓸 공간이 남고, 그래서 dma_32bit_pfn 경계와
 * cached32_node 라는 별도 커서를 따로 둔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: 드라이버 dma_map_sg
 *         → dma-iommu.c (iommu_dma_map_sg)
 *           → [이 파일] alloc_iova_fast   : IOVA 구간 확보
 *           → iommu.c   iommu_map_sg      : 그 IOVA 에 물리 페이지를 매핑
 *         ← 해제는 역순이며, 무효화를 모으는 flush queue 가 그 사이에 낀다.
 *
 * 이 파일은 IOMMU 하드웨어를 전혀 만지지 않는다. 순수한 주소 공간 부기(bookkeeping)
 * 이며, 그래서 인텔 VT-d 도 ARM SMMU 도 같은 코드를 공유한다. 실행 컨텍스트는
 * 커널 모듈이고, 할당 경로는 인터럽트 문맥에서도 불릴 수 있어 GFP_ATOMIC 과
 * spin_lock_irqsave 로 일관되게 쓰여 있다.
 *
 * === 타 모듈과의 연결 ===
 * - dma-iommu.c: 유일한 주 사용자. 도메인마다 iova_domain 을 하나 두고, IOVA 를
 *   떼어 iommu_map 으로 채운 뒤 장치에 그 주소를 준다. flush queue(DMA_FQ) 정책도
 *   그쪽에 있으며, 해제된 IOVA 를 곧바로 재사용하지 않고 모아 두었다가 IOTLB
 *   무효화가 끝난 뒤 이 할당자에 돌려주는 것이 그 핵심이다.
 * - iommu.c: 예약 구간(reserved region) 정보를 통해 이 할당자가 피해야 할 범위를
 *   간접적으로 정한다. MSI 창과 RMRR 이 대표적이다.
 * - CPU 핫플러그: rcache 는 CPU 별 자료구조라 CPU 가 내려갈 때 그 캐시를 비워야
 *   한다. cpuhp 콜백이 등록되어 있는 이유다.
 * - kmem_cache: struct iova 를 전용 슬랩에서 찍어 낸다. 참조 계수 방식으로
 *   여러 도메인이 하나의 캐시를 공유한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iova            : 사용 중인 한 구간 [pfn_lo, pfn_hi]. 트리의 노드다.
 * - struct iova_domain     : 한 주소 공간 전체. 트리 루트, 캐시 커서, rcache 배열.
 * - init_iova_domain()     : 도메인을 세우고 앵커 노드를 심는다.
 * - alloc_iova_fast()      : 실사용 진입점. rcache 를 먼저 보고, 없으면 트리로.
 * - free_iova_fast()       : 짝. 캐시에 넣을 수 있으면 넣고, 아니면 트리로 돌려준다.
 * - __alloc_and_insert_iova_range() : 트리를 위에서 아래로 훑어 빈 구간을 찾는다.
 * - iova_rcache_insert/get(): CPU 별 매그니튜드 캐시의 넣기/꺼내기.
 * - iova_find_limit()      : 32비트 재시도에서 탐색 시작점을 다시 잡는다.
 */
#include <linux/iova.h>	/* [한국어] struct iova/iova_domain 정의와 이 파일이 구현하는 공개 API */
#include <linux/kmemleak.h>	/* [한국어] 슬랩에서 찍어 낸 iova 객체를 누수 검사기에서 제외하기 위해 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL — dma-iommu 와 벤더 드라이버가 이 할당자를 쓴다 */
#include <linux/slab.h>	/* [한국어] kmem_cache — struct iova 전용 슬랩을 만든다 */
#include <linux/smp.h>	/* [한국어] this_cpu_ptr 등 CPU 로컬 접근. rcache 가 CPU 별 자료구조다 */
#include <linux/bitops.h>	/* [한국어] fls_long — 크기를 2의 거듭제곱 정렬 마스크로 바꾼다 */
#include <linux/cpu.h>	/* [한국어] CPU 핫플러그 콜백 등록. CPU 가 내려가면 그 캐시를 비워야 한다 */
#include <linux/workqueue.h>	/* [한국어] 캐시 정리 등 지연 작업 */

/* The anchor node sits above the top of the usable address space */
#define IOVA_ANCHOR	~0UL	/* [한국어] 앵커 노드의 pfn 값. 사용 가능한 주소 공간의 맨 위에 놓아 트리 순회가 '경계 밖'을 따로 검사하지 않아도 되게 만드는 보초(sentinel)다 (위 영어 주석) */

#define IOVA_RANGE_CACHE_MAX_SIZE 6	/* log of max cached IOVA range size (in pages) */	/* [한국어] rcache 가 담는 최대 크기의 로그값 — 1,2,4,8,16,32 페이지 여섯 등급. 실제 DMA 요청의 대부분이 이 범위 안에 들어와 트리를 건드리지 않고 처리된다 */

static bool iova_rcache_insert(struct iova_domain *iovad,	/* [한국어] 아래에서 정의되는 rcache 조작 함수들의 전방 선언 — 파일 위쪽의 할당/해제 경로가 먼저 쓴다 */
			       unsigned long pfn,
			       unsigned long size);
static unsigned long iova_rcache_get(struct iova_domain *iovad,	/* [한국어] 캐시에서 구간을 꺼내는 짝 */
				     unsigned long size,
				     unsigned long limit_pfn);
static void free_iova_rcaches(struct iova_domain *iovad);	/* [한국어] 도메인 해체 시 캐시 전체 반납 */
static void free_cpu_cached_iovas(unsigned int cpu, struct iova_domain *iovad);	/* [한국어] CPU 핫플러그로 한 CPU 의 캐시만 비울 때 */
static void free_global_cached_iovas(struct iova_domain *iovad);	/* [한국어] 전역(depot) 캐시만 비울 때 — 트리에 공간이 부족하면 여기부터 회수한다 */

/*
 * [한국어]
 * to_iova - rb_node 포인터를 감싸고 있는 struct iova 로 되돌린다
 *
 * @node:   트리 노드 포인터
 * @return: 그 노드를 품은 구간 객체
 *
 * 커널 레드-블랙 트리는 자기가 어떤 자료형에 박혀 있는지 모르고 rb_node 만
 * 다룬다. 이 한 줄짜리 어댑터가 그 경계를 잇는다. 앵커 노드에도 그대로 쓰이는데,
 * 앵커 역시 struct iova 이기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 포인터 산술뿐이다.
 */
static struct iova *to_iova(struct rb_node *node)
{
	return rb_entry(node, struct iova, node);	/* [한국어] rb_node 는 struct iova 안에 박혀 있으므로 역산이 성립한다. 트리 코드가 우리 형을 모르기 때문에 필요한 어댑터다 */
}

/*
 * [한국어]
 * init_iova_domain - IOVA 주소 공간 하나를 세운다
 *
 * @iovad:     초기화할 도메인 구조체 (호출자가 소유)
 * @granule:   최소 단위. 보통 IOMMU 가 지원하는 가장 작은 페이지 크기.
 * @start_pfn: 할당 가능한 하한 pfn
 *
 * pfn 이 무엇인지 여기서 정해진다 — 이 파일의 모든 pfn 은 바이트 주소가 아니라
 * granule 단위로 센 번호이며, 실제 IOVA 는 pfn << iova_shift(iovad) 다. granule 이
 * CPU 페이지보다 클 수 없고 2의 거듭제곱이어야 하는 이유도 그 변환 때문이다.
 *
 * 앵커 노드를 심는 것이 이 함수의 진짜 일이다. 주소 공간 맨 끝에 길이 0 짜리
 * 가짜 구간을 하나 두면 트리가 절대 비지 않고, 할당 탐색이 "맨 위에서 아래로"
 * 시작할 고정점을 얻는다. 그 덕분에 이후의 순회 코드 어디에도 빈 트리 특수 처리가
 * 없다.
 *
 * 32비트 경계를 별도로 두는 것도 여기서 시작한다. 낮은 주소는 32비트 DMA 마스크
 * 장치만 쓸 수 있는 희소 자원이므로, 커서와 실패 기록을 따로 관리한다.
 *
 * 실행 컨텍스트: 도메인 생성 경로. 프로세스 문맥.
 *
 * 호출 체인: dma-iommu 의 iommu_get_dma_cookie 경로 → [이 함수]
 */
void
init_iova_domain(struct iova_domain *iovad, unsigned long granule,
	unsigned long start_pfn)
{
	/*
	 * IOVA granularity will normally be equal to the smallest
	 * supported IOMMU page size; both *must* be capable of
	 * representing individual CPU pages exactly.
	 */
	BUG_ON((granule > PAGE_SIZE) || !is_power_of_2(granule));	/* [한국어] 입도가 CPU 페이지보다 크면 한 페이지를 정확히 표현할 수 없고, 2의 거듭제곱이 아니면 시프트로 pfn↔주소 변환을 할 수 없다 (위 영어 주석) */

	spin_lock_init(&iovad->iova_rbtree_lock);	/* [한국어] 트리 전체를 지키는 유일한 락. 인터럽트 문맥에서도 잡히므로 항상 irqsave 로 쓴다 */
	iovad->rbroot = RB_ROOT;	/* [한국어] 빈 트리로 시작 */
	iovad->cached_node = &iovad->anchor.node;	/* [한국어] 할당 탐색을 시작할 커서. 매번 트리 맨 위에서 내려오지 않도록 '최근에 할당한 근처'를 기억한다 */
	iovad->cached32_node = &iovad->anchor.node;	/* [한국어] 32비트 영역 전용 커서. 낮은 주소는 32비트 마스크 장치만 쓸 수 있어 따로 관리한다 */
	iovad->granule = granule;	/* [한국어] 이 도메인의 최소 단위(보통 IOMMU 최소 페이지 크기). pfn 은 이 단위로 센 번호다 */
	iovad->start_pfn = start_pfn;	/* [한국어] 할당 가능한 하한. 0 을 피하는 것이 보통인데, DMA 주소 0 을 오류값으로 쓰는 코드가 많기 때문이다 */
	iovad->dma_32bit_pfn = 1UL << (32 - iova_shift(iovad));	/* [한국어] 32비트로 표현 가능한 마지막 pfn. 이 경계 아래가 희소 자원이라 별도 커서와 별도 실패 기록을 둔다 */
	iovad->max32_alloc_size = iovad->dma_32bit_pfn;	/* [한국어] 32비트 영역에서 '이 크기 이상은 실패한다'고 기억해 두는 값. 실패가 반복될 때 트리를 헛도는 것을 막는 지름길이다 */
	iovad->anchor.pfn_lo = iovad->anchor.pfn_hi = IOVA_ANCHOR;	/* [한국어] 앵커는 주소 공간 맨 끝의 길이 0 짜리 가짜 구간이다 */
	rb_link_node(&iovad->anchor.node, NULL, &iovad->rbroot.rb_node);	/* [한국어] 트리의 뿌리로 심는다 — 부모가 NULL 이다 */
	rb_insert_color(&iovad->anchor.node, &iovad->rbroot);	/* [한국어] 색 균형까지 맞춰 삽입 완료. 이 뒤로 트리는 절대 비지 않으므로, 순회 코드가 NULL 루트를 따로 다룰 필요가 없다 */
}
EXPORT_SYMBOL_GPL(init_iova_domain);	/* [한국어] dma-iommu 가 도메인마다 하나씩 세운다 */

/*
 * [한국어]
 * __get_cached_rbnode - 할당 탐색을 시작할 커서를 고른다
 *
 * @iovad:     대상 도메인
 * @limit_pfn: 이번 요청의 상한
 * @return:    탐색 시작 노드
 *
 * 매번 트리 꼭대기에서 내려오면 할당이 O(log n) 이 아니라 사실상 전체 순회가 된다.
 * 그래서 "가장 최근에 할당한 자리"를 기억해 두고 거기서 아래로 이어 간다.
 * 커서가 둘인 이유는 32비트 영역이 별도 자원이기 때문이다 — 32비트 요청이 전체
 * 커서를 따라 높은 주소로 끌려가면 쓸 수 없는 곳만 훑게 된다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: __alloc_and_insert_iova_range → [이 함수]
 */
static struct rb_node *
__get_cached_rbnode(struct iova_domain *iovad, unsigned long limit_pfn)
{
	if (limit_pfn <= iovad->dma_32bit_pfn)	/* [한국어] 요청 상한이 32비트 안이면 */
		return iovad->cached32_node;	/* [한국어] 32비트 전용 커서에서 출발한다 */

	return iovad->cached_node;	/* [한국어] 그 외에는 전체 영역 커서 */
}

/*
 * [한국어]
 * __cached_rbnode_insert_update - 할당 직후 커서를 그 자리로 옮긴다
 *
 * @iovad: 대상 도메인
 * @new:   방금 트리에 삽입된 구간
 *
 * 다음 할당이 이어서 아래로 내려갈 수 있게 한다. 연속된 DMA 매핑이 주소 공간을
 * 위에서 아래로 차곡차곡 채우는 흔한 패턴에서, 이 갱신 덕분에 각 할당이 트리를
 * 거의 걷지 않는다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: __alloc_and_insert_iova_range → [이 함수]
 */
static void
__cached_rbnode_insert_update(struct iova_domain *iovad, struct iova *new)
{
	if (new->pfn_hi < iovad->dma_32bit_pfn)	/* [한국어] 방금 할당한 구간이 32비트 영역 안이면 */
		iovad->cached32_node = &new->node;	/* [한국어] 그쪽 커서를 갱신 — 다음 32비트 할당이 여기서 이어 내려간다 */
	else
		iovad->cached_node = &new->node;	/* [한국어] 그 외에는 전체 커서를 갱신 */
}

/*
 * [한국어]
 * __cached_rbnode_delete_update - 해제 직전 커서를 안전한 곳으로 물린다
 *
 * @iovad: 대상 도메인
 * @free:  곧 트리에서 사라질 구간
 *
 * 두 가지 일을 한다. 첫째, 커서가 사라질 노드를 가리키고 있으면 다음 노드로
 * 옮긴다 — rb_erase 뒤에는 rb_next 를 부를 수 없으므로 반드시 지우기 전에 해야
 * 한다. 둘째, 커서보다 위쪽이 비었으면 커서를 그리로 되돌려 방금 생긴 공간을
 * 다음 할당이 곧바로 쓸 수 있게 한다.
 *
 * 32비트 영역에서는 max32_alloc_size 기록도 함께 지운다. "이 크기는 실패한다"는
 * 지름길이 공간이 생긴 뒤에도 남아 있으면 쓸 수 있는 자리를 두고 실패하게 된다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: remove_iova → [이 함수]
 */
static void
__cached_rbnode_delete_update(struct iova_domain *iovad, struct iova *free)
{
	struct iova *cached_iova;	/* [한국어] 커서가 가리키던 구간 */

	cached_iova = to_iova(iovad->cached32_node);	/* [한국어] 32비트 커서부터 확인 */
	if (free == cached_iova ||	/* [한국어] 해제되는 구간이 바로 그 커서라면 커서를 옮겨야 한다 */
	    (free->pfn_hi < iovad->dma_32bit_pfn &&	/* [한국어] 32비트 영역 안의 구간이면서 */
	     free->pfn_lo >= cached_iova->pfn_lo))	/* [한국어] 커서보다 위쪽이면 — 그 자리가 비었으니 커서를 위로 되돌려 재사용할 수 있게 한다 */
		iovad->cached32_node = rb_next(&free->node);	/* [한국어] 해제될 노드의 다음으로 옮긴다. rb_erase 전에 계산해야 유효하다 */

	if (free->pfn_lo < iovad->dma_32bit_pfn)	/* [한국어] 32비트 영역에 공간이 생겼다 */
		iovad->max32_alloc_size = iovad->dma_32bit_pfn;	/* [한국어] '이 크기 이상 실패' 기록을 지운다 — 다시 시도해 볼 가치가 생겼기 때문 */

	cached_iova = to_iova(iovad->cached_node);	/* [한국어] 전체 영역 커서도 같은 방식으로 */
	if (free->pfn_lo >= cached_iova->pfn_lo)	/* [한국어] 커서보다 위쪽이 비었으면 */
		iovad->cached_node = rb_next(&free->node);	/* [한국어] 커서를 위로 되돌린다 */
}

/*
 * [한국어]
 * iova_find_limit - 주어진 상한 근처의 트리 노드를 찾는다
 *
 * @iovad:     대상 도메인
 * @limit_pfn: 상한
 * @return:    상한 위쪽에 있는 노드 중 가장 가까운 것
 *
 * 32비트 재시도 전용이다. 커서에서 아래로 훑어 실패했을 때, 커서 위쪽 영역을
 * 다시 보려면 그 시작점이 필요한데 커서 자체는 이미 지나온 자리라 쓸 수 없다.
 *
 * 위 영어 주석이 스스로 인정하듯 판정이 조잡하다 — 상한이 32비트 위쪽이면 정밀한
 * 탐색을 포기하고 앵커부터 시작한다. 32비트 영역이 좁아 재시도가 실제로 이득인
 * 경우가 그쪽뿐이기 때문이다.
 *
 * 탐색 자체는 "상한 이상인 노드 중 가장 작은 것"을 찾는 변형된 이진 탐색이다.
 * 오른쪽으로 내려가 상한을 넘긴 뒤, 왼쪽 부분트리를 훑으며 더 가까운 후보가
 * 있으면 그리로 옮겨 다시 좁힌다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: __alloc_and_insert_iova_range 의 재시도 경로 → [이 함수]
 */
static struct rb_node *iova_find_limit(struct iova_domain *iovad, unsigned long limit_pfn)
{
	struct rb_node *node, *next;	/* [한국어] 탐색 커서와 오른쪽 자손을 따라갈 보조 커서 */
	/*
	 * Ideally what we'd like to judge here is whether limit_pfn is close
	 * enough to the highest-allocated IOVA that starting the allocation
	 * walk from the anchor node will be quicker than this initial work to
	 * find an exact starting point (especially if that ends up being the
	 * anchor node anyway). This is an incredibly crude approximation which
	 * only really helps the most likely case, but is at least trivially easy.
	 */
	if (limit_pfn > iovad->dma_32bit_pfn)	/* [한국어] 상한이 32비트 위쪽이면 정밀한 시작점을 찾을 이유가 없다 — 위 영어 주석이 말하는 '조잡하지만 흔한 경우에 효과적인' 근사다 */
		return &iovad->anchor.node;	/* [한국어] 맨 위 앵커부터 내려간다 */

	node = iovad->rbroot.rb_node;	/* [한국어] 트리 뿌리에서 시작 */
	while (to_iova(node)->pfn_hi < limit_pfn)	/* [한국어] 상한보다 낮은 구간만 나오는 동안 */
		node = node->rb_right;	/* [한국어] 오른쪽(더 높은 주소)으로 내려간다 */

search_left:	/* [한국어] 왼쪽으로 되돌아 내려가는 지점 */
	while (node->rb_left && to_iova(node->rb_left)->pfn_lo >= limit_pfn)	/* [한국어] 왼쪽 자식도 여전히 상한 위쪽이면 더 왼쪽으로 — 상한 바로 위의 가장 작은 구간을 찾는 중이다 */
		node = node->rb_left;	/* [한국어] 한 단계 내려간다 */

	if (!node->rb_left)	/* [한국어] 왼쪽 자손이 없으면 여기가 답이다 */
		return node;	/* [한국어] 탐색 시작점 확정 */

	next = node->rb_left;	/* [한국어] 왼쪽 부분트리 안에서 가장 오른쪽(가장 큰) 것을 찾는다 */
	while (next->rb_right) {	/* [한국어] 오른쪽 끝까지 */
		next = next->rb_right;	/* [한국어] 한 단계씩 */
		if (to_iova(next)->pfn_lo >= limit_pfn) {	/* [한국어] 그중 상한 위쪽인 것이 있으면 더 가까운 시작점이다 */
			node = next;	/* [한국어] 그것으로 바꾸고 */
			goto search_left;	/* [한국어] 다시 왼쪽으로 좁혀 들어간다 */
		}
	}

	return node;	/* [한국어] 상한에 가장 가까운 노드 */
}

/* Insert the iova into domain rbtree by holding writer lock */
/*
 * [한국어] (위 영어 주석에 이어)
 * iova_insert_rbtree - 구간 하나를 시작 pfn 기준으로 트리에 넣는다
 *
 * @root:  트리 루트
 * @iova:  넣을 구간
 * @start: 탐색을 시작할 노드. NULL 이면 루트부터.
 *
 * start 힌트가 있는 것이 요점이다. 할당 경로는 방금 트리를 역주행하며 삽입 위치
 * 바로 아래 노드를 이미 지나왔으므로, 그 노드를 넘겨 주면 루트부터 다시 내려갈
 * 필요가 없다. 예약(reserve_iova) 처럼 드문 경로만 NULL 로 부른다.
 *
 * 같은 시작 pfn 이 이미 있으면 WARN 만 내고 넣지 않는다. 그것은 할당자가 이미
 * 쓰이는 자리를 다시 내줬다는 뜻이고, 트리를 더 망가뜨리는 대신 흔적을 남긴다.
 *
 * 실행 컨텍스트: 트리 락을 든 채 (위 영어 주석).
 *
 * 호출 체인: __alloc_and_insert_iova_range, __insert_new_range → [이 함수]
 */
static void
iova_insert_rbtree(struct rb_root *root, struct iova *iova,
		   struct rb_node *start)
{
	struct rb_node **new, *parent = NULL;	/* [한국어] 삽입 위치를 가리킬 포인터의 포인터와 그 부모 */

	new = (start) ? &start : &(root->rb_node);	/* [한국어] start 가 주어지면 그 근처부터 — 할당 경로가 방금 지나온 노드를 알려 주므로 뿌리부터 다시 내려갈 필요가 없다 */
	/* Figure out where to put new node */
	while (*new) {	/* [한국어] 자리가 빌 때까지 내려간다 */
		struct iova *this = to_iova(*new);	/* [한국어] 현재 노드의 구간 */

		parent = *new;	/* [한국어] 마지막으로 지나온 노드가 부모가 된다 */

		if (iova->pfn_lo < this->pfn_lo)	/* [한국어] 시작 pfn 기준 정렬 */
			new = &((*new)->rb_left);	/* [한국어] 왼쪽으로 */
		else if (iova->pfn_lo > this->pfn_lo)	/* [한국어] 더 크면 */
			new = &((*new)->rb_right);	/* [한국어] 오른쪽으로 */
		else {
			WARN_ON(1); /* this should not happen */	/* [한국어] 같은 시작 pfn 의 구간이 두 개 = 할당자가 이미 쓰이는 자리를 다시 내줬다는 뜻. 위 영어 주석대로 있어서는 안 되는 상태다 */
			return;	/* [한국어] 삽입하지 않고 물러난다 — 트리를 더 망가뜨리지 않기 위해 */
		}
	}
	/* Add new node and rebalance tree. */
	rb_link_node(&iova->node, parent, new);	/* [한국어] 찾은 자리에 연결 */
	rb_insert_color(&iova->node, root);	/* [한국어] 레드-블랙 균형 복구 */
}

/*
 * [한국어]
 * __alloc_and_insert_iova_range - 트리에서 빈 구간을 찾아 확보한다 (할당의 심장부)
 *
 * @iovad:        대상 도메인
 * @size:         필요한 페이지 수
 * @limit_pfn:    배타 상한 (호출자가 이미 +1 해서 넘긴다)
 * @new:          미리 할당해 둔 빈 구간 객체. 여기에 결과를 채운다.
 * @size_aligned: true 면 시작 주소를 size 를 올림한 2의 거듭제곱에 정렬한다
 * @return:       0 성공, -ENOMEM 이면 공간 없음
 *
 * 탐색 방향이 위에서 아래(top-down)라는 점이 이 할당자의 성격을 결정한다. 높은
 * 주소부터 채우면 낮은 주소가 남고, 그것이 32비트 DMA 마스크 장치가 쓸 몫이 된다.
 *
 * 알고리즘은 단순하다. 커서에서 시작해 rb_prev 로 아래로 내려가며, 두 인접 구간
 * 사이의 틈이 size 를 담을 만큼 큰 곳을 찾는다. 정렬이 필요하면 후보 주소를
 * align_mask 로 내림해 맞춘다.
 *
 * 세 가지 최적화가 얹혀 있다.
 *  - 커서: 매번 트리 끝에서 내려오지 않는다.
 *  - max32_alloc_size: 32비트 영역에서 실패한 크기를 기억해 헛도는 것을 막는다.
 *  - 재시도: 커서 아래에서 못 찾으면 커서 위쪽만 한 번 더 훑는다. 커서 최적화가
 *    놓친 공간을 확인하는 것이라 두 번은 없다.
 *
 * size_aligned 를 요구하는 것은 dma-iommu 다. 시작 주소가 크기에 자연 정렬되어야
 * iommu_map 이 큰 페이지(2MB 등)를 쓸 수 있고, 그러면 PTE 와 IOTLB 소모가 준다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 포함 어디서든. 트리 락을 스스로 잡고 놓는다.
 *
 * 호출 체인: alloc_iova → [이 함수] → iova_find_limit, iova_insert_rbtree
 */
static int __alloc_and_insert_iova_range(struct iova_domain *iovad,
		unsigned long size, unsigned long limit_pfn,
			struct iova *new, bool size_aligned)
{
	struct rb_node *curr, *prev;	/* [한국어] 역방향 순회 커서와 직전 노드 */
	struct iova *curr_iova;	/* [한국어] 현재 노드의 구간 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장 */
	unsigned long new_pfn, retry_pfn;	/* [한국어] 후보 시작 pfn 과, 32비트 재시도의 하한이 될 pfn */
	unsigned long align_mask = ~0UL;	/* [한국어] 정렬 마스크. 기본은 정렬 요구 없음 */
	unsigned long high_pfn = limit_pfn, low_pfn = iovad->start_pfn;	/* [한국어] 탐색 구간의 위·아래 경계 */

	if (size_aligned)	/* [한국어] 크기만큼 자연 정렬이 필요하면 (큰 페이지로 매핑하기 위해) */
		align_mask <<= fls_long(size - 1);	/* [한국어] 크기를 올림한 2의 거듭제곱 경계로 마스크를 만든다 */

	/* Walk the tree backwards */
	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리 조작 구간. 인터럽트 문맥에서도 불리므로 irqsave */
	if (limit_pfn <= iovad->dma_32bit_pfn &&	/* [한국어] 32비트 영역만 쓸 수 있는 요청이고 */
			size >= iovad->max32_alloc_size)	/* [한국어] 지난번 이 크기에서 이미 실패했다면 */
		goto iova32_full;	/* [한국어] 트리를 헛돌지 않고 곧바로 실패로 간다 */

	curr = __get_cached_rbnode(iovad, limit_pfn);	/* [한국어] 커서에서 출발 — 매번 트리 끝에서 내려오지 않는다 */
	curr_iova = to_iova(curr);	/* [한국어] 그 구간 */
	retry_pfn = curr_iova->pfn_hi;	/* [한국어] 커서 위쪽에서 못 찾았을 때, 커서 아래를 다시 훑을 하한으로 쓴다 */

retry:	/* [한국어] 32비트 재시도가 되돌아오는 지점 */
	do {	/* [한국어] 트리를 높은 주소에서 낮은 쪽으로 역주행하며 빈 틈을 찾는다 */
		high_pfn = min(high_pfn, curr_iova->pfn_lo);	/* [한국어] 이 구간의 시작 아래쪽이 후보 공간이다 */
		new_pfn = (high_pfn - size) & align_mask;	/* [한국어] 거기서 크기만큼 내려가 정렬한 자리 */
		prev = curr;	/* [한국어] 삽입 힌트로 쓸 직전 노드 */
		curr = rb_prev(curr);	/* [한국어] 한 칸 아래 구간으로 */
		curr_iova = to_iova(curr);	/* [한국어] 그 구간 */
	} while (curr && new_pfn <= curr_iova->pfn_hi && new_pfn >= low_pfn);	/* [한국어] 후보가 아래 구간과 겹치는 동안 계속 내려간다. 겹치지 않게 되거나 하한을 벗어나면 멈춘다 */

	if (high_pfn < size || new_pfn < low_pfn) {	/* [한국어] 공간을 찾지 못했다 */
		if (low_pfn == iovad->start_pfn && retry_pfn < limit_pfn) {	/* [한국어] 커서 아래만 훑었고 커서 위쪽이 아직 남아 있다면 */
			high_pfn = limit_pfn;	/* [한국어] 상한을 원래대로 되돌리고 */
			low_pfn = retry_pfn + 1;	/* [한국어] 커서 위쪽만 탐색 대상으로 삼는다 */
			curr = iova_find_limit(iovad, limit_pfn);	/* [한국어] 그 영역의 시작점을 다시 찾는다 */
			curr_iova = to_iova(curr);	/* [한국어] 그 구간 */
			goto retry;	/* [한국어] 한 번 더 — 커서 최적화가 놓친 공간을 확인하는 것이라 재시도는 한 번뿐이다 */
		}
		iovad->max32_alloc_size = size;	/* [한국어] 이 크기로는 실패한다고 기록해 다음 시도를 아낀다 */
		goto iova32_full;	/* [한국어] 실패 */
	}

	/* pfn_lo will point to size aligned address if size_aligned is set */
	new->pfn_lo = new_pfn;	/* [한국어] 찾은 시작 pfn. size_aligned 면 정렬되어 있다 (위 영어 주석) */
	new->pfn_hi = new->pfn_lo + size - 1;	/* [한국어] 닫힌 구간의 마지막 pfn */

	/* If we have 'prev', it's a valid place to start the insertion. */
	iova_insert_rbtree(&iovad->rbroot, new, prev);	/* [한국어] 역주행에서 지나온 노드를 힌트로 넘겨 삽입 비용을 줄인다 (위 영어 주석) */
	__cached_rbnode_insert_update(iovad, new);	/* [한국어] 커서를 방금 할당한 자리로 옮긴다 */

	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리 조작 끝 */
	return 0;	/* [한국어] 구간 확보 성공 */

iova32_full:	/* [한국어] 공간 부족 경로 */
	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락 해제 */
	return -ENOMEM;	/* [한국어] 호출자는 캐시를 비우고 다시 시도하거나 매핑을 포기한다 */
}

static struct kmem_cache *iova_cache;	/* [한국어] struct iova 전용 슬랩. 크기가 작고 매우 자주 만들어지므로 전용 캐시가 유리하다 */
static unsigned int iova_cache_users;	/* [한국어] 이 캐시를 쓰는 도메인 수. 0 이 되면 캐시를 없앤다 */
static DEFINE_MUTEX(iova_cache_mutex);	/* [한국어] 캐시 생성/파괴를 직렬화 */

/*
 * [한국어]
 * alloc_iova_mem - 구간 객체 하나를 슬랩에서 꺼낸다
 *
 * @return: 0 으로 채워진 struct iova, 실패하면 NULL
 *
 * GFP_ATOMIC 인 것은 해제·할당 경로가 인터럽트 문맥에서도 불리기 때문이고,
 * __GFP_NOWARN 인 것은 실패가 예외 상황이 아니기 때문이다 — 호출자는 캐시를
 * 비우고 다시 시도하는 정상 경로를 갖고 있다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: alloc_iova, alloc_and_init_iova → [이 함수]
 */
static struct iova *alloc_iova_mem(void)
{
	return kmem_cache_zalloc(iova_cache, GFP_ATOMIC | __GFP_NOWARN);	/* [한국어] 할당 경로가 인터럽트 문맥에서도 불리므로 ATOMIC. 실패는 흔한 정상 상황(캐시 회수 후 재시도)이라 경고를 끈다 */
}

/*
 * [한국어]
 * free_iova_mem - 구간 객체를 슬랩에 돌려준다
 *
 * @iova: 반납할 객체
 *
 * 앵커 노드를 걸러 내는 것이 이 함수의 존재 이유다. 앵커는 iova_domain 구조체
 * 안에 박혀 있는 필드라 슬랩 객체가 아니며, 도메인 해체 시 트리를 통째로 훑는
 * put_iova_domain 이 앵커까지 넘겨 오기 때문에 여기서 한 번 걸러야 한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: alloc_iova(실패), __free_iova, free_iova, put_iova_domain 등 → [이 함수]
 */
static void free_iova_mem(struct iova *iova)
{
	if (iova->pfn_lo != IOVA_ANCHOR)	/* [한국어] 앵커는 도메인 구조체에 박혀 있는 것이라 슬랩 객체가 아니다 */
		kmem_cache_free(iova_cache, iova);	/* [한국어] 진짜 슬랩 객체만 반납 */
}

/**
 * alloc_iova - allocates an iova
 * @iovad: - iova domain in question
 * @size: - size of page frames to allocate
 * @limit_pfn: - max limit address
 * @size_aligned: - set if size_aligned address range is required
 * This function allocates an iova in the range iovad->start_pfn to limit_pfn,
 * searching top-down from limit_pfn to iovad->start_pfn. If the size_aligned
 * flag is set then the allocated address iova->pfn_lo will be naturally
 * aligned on roundup_power_of_two(size).
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * alloc_iova - 캐시를 거치지 않고 트리에서 직접 구간을 확보한다
 *
 * @iovad:        대상 도메인
 * @size:         필요한 페이지 수
 * @limit_pfn:    포함 상한
 * @size_aligned: 자연 정렬 요구 여부
 * @return:       확보된 구간, 실패하면 NULL
 *
 * 객체를 먼저 잡고 락을 잡는 순서에 주목할 것. 반대로 하면 트리 락을 든 채로
 * 슬랩 할당을 하게 되어 락 점유 시간이 늘고, GFP_ATOMIC 실패 시 되돌리기도
 * 번거로워진다.
 *
 * limit_pfn 에 +1 을 더해 넘기는 것은 API 경계의 관례 차이다 — 바깥은 포함 상한,
 * 안쪽은 배타 상한을 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: alloc_iova_fast, 벤더 드라이버 → [이 함수]
 *            → __alloc_and_insert_iova_range
 */
struct iova *
alloc_iova(struct iova_domain *iovad, unsigned long size,
	unsigned long limit_pfn,
	bool size_aligned)
{
	struct iova *new_iova;	/* [한국어] 확보할 구간 객체 */
	int ret;	/* [한국어] 트리 삽입 결과 */

	new_iova = alloc_iova_mem();	/* [한국어] 슬랩에서 구간 객체를 먼저 잡는다 — 락 안에서 할당하지 않기 위해 */
	if (!new_iova)	/* [한국어] 슬랩 고갈 */
		return NULL;

	ret = __alloc_and_insert_iova_range(iovad, size, limit_pfn + 1,	/* [한국어] limit_pfn 은 포함 상한이라 +1 해 배타 상한으로 바꿔 넘긴다 */
			new_iova, size_aligned);

	if (ret) {	/* [한국어] 빈 공간을 못 찾았다 */
		free_iova_mem(new_iova);	/* [한국어] 미리 잡아 둔 객체를 반납 */
		return NULL;	/* [한국어] 호출자가 캐시를 비우고 재시도한다 */
	}

	return new_iova;	/* [한국어] pfn_lo/pfn_hi 가 채워진 구간 */
}
EXPORT_SYMBOL_GPL(alloc_iova);	/* [한국어] 캐시를 거치지 않는 직접 할당 — 큰 요청과 초기화 경로가 쓴다 */

/*
 * [한국어]
 * private_find_iova - pfn 을 포함하는 구간을 트리에서 찾는다 (락 없음)
 *
 * @iovad:  대상 도메인
 * @pfn:    찾을 페이지 번호
 * @return: 그 pfn 을 담은 구간, 없으면 NULL
 *
 * 평범한 이진 탐색이지만, 노드가 점이 아니라 구간이라 비교가 세 갈래다.
 * 이름의 private 은 "락을 스스로 잡지 않는다"는 뜻이고, assert_spin_locked 가
 * 그 계약을 검사한다. 락 안에서 여러 번 부르는 경로(iova_magazine_free_pfns)가
 * 있어 락을 분리해 두었다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: find_iova, free_iova, iova_magazine_free_pfns → [이 함수]
 */
static struct iova *
private_find_iova(struct iova_domain *iovad, unsigned long pfn)
{
	struct rb_node *node = iovad->rbroot.rb_node;	/* [한국어] 트리 뿌리에서 시작 */

	assert_spin_locked(&iovad->iova_rbtree_lock);	/* [한국어] 이 함수는 락을 잡지 않는다 — 호출자가 이미 들고 있어야 한다 */

	while (node) {	/* [한국어] 일반적인 이진 탐색 */
		struct iova *iova = to_iova(node);	/* [한국어] 현재 노드의 구간 */

		if (pfn < iova->pfn_lo)	/* [한국어] 구간보다 아래면 */
			node = node->rb_left;
		else if (pfn > iova->pfn_hi)	/* [한국어] 구간보다 위면 */
			node = node->rb_right;
		else
			return iova;	/* pfn falls within iova's range */
	}

	return NULL;	/* [한국어] 이 pfn 을 포함하는 할당 구간이 없다 = 이미 해제됐거나 잘못된 주소 */
}

/*
 * [한국어]
 * remove_iova - 구간을 트리에서 떼어 낸다 (락 없음, 객체는 해제하지 않는다)
 *
 * @iovad: 대상 도메인
 * @iova:  떼어 낼 구간
 *
 * 커서 갱신을 rb_erase 보다 먼저 하는 순서가 이 함수의 전부다. 지운 뒤에는
 * rb_next 로 다음 노드를 구할 수 없기 때문이다.
 *
 * 객체 해제는 호출자 몫으로 남긴다 — 락 밖에서 하기 위해서다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: __free_iova, free_iova, iova_magazine_free_pfns → [이 함수]
 */
static void remove_iova(struct iova_domain *iovad, struct iova *iova)
{
	assert_spin_locked(&iovad->iova_rbtree_lock);	/* [한국어] 호출자가 락을 든 상태여야 한다 */
	__cached_rbnode_delete_update(iovad, iova);	/* [한국어] 커서를 먼저 옮긴다 — rb_erase 뒤에는 rb_next 를 부를 수 없다 */
	rb_erase(&iova->node, &iovad->rbroot);	/* [한국어] 트리에서 제거하고 균형 복구 */
}

/**
 * find_iova - finds an iova for a given pfn
 * @iovad: - iova domain in question.
 * @pfn: - page frame number
 * This function finds and returns an iova belonging to the
 * given domain which matches the given pfn.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * find_iova - pfn 으로 할당된 구간을 찾는다 (락 포함)
 *
 * @iovad:  대상 도메인
 * @pfn:    찾을 페이지 번호
 * @return: 구간 또는 NULL
 *
 * 반환된 포인터는 락을 놓은 뒤이므로 그 사이 해제될 수 있다. 진단·디버깅 용도로
 * 쓰이며, 실제 해제 경로는 탐색과 제거를 한 락 구간에서 처리하는 free_iova 를 쓴다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 벤더 드라이버, 진단 코드 → [이 함수] → private_find_iova
 */
struct iova *find_iova(struct iova_domain *iovad, unsigned long pfn)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	struct iova *iova;	/* [한국어] 찾은 구간 */

	/* Take the lock so that no other thread is manipulating the rbtree */
	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리를 읽는 동안 아무도 바꾸지 못하게 (위 영어 주석) */
	iova = private_find_iova(iovad, pfn);	/* [한국어] 락 안에서 탐색 */
	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락 해제 */
	return iova;	/* [한국어] 반환된 포인터의 유효성은 호출자가 보장해야 한다 — 락을 놓은 뒤에는 해제될 수 있다 */
}
EXPORT_SYMBOL_GPL(find_iova);	/* [한국어] 디버깅과 일부 드라이버가 쓴다 */

/**
 * __free_iova - frees the given iova
 * @iovad: iova domain in question.
 * @iova: iova in question.
 * Frees the given iova belonging to the giving domain
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * __free_iova - 이미 들고 있는 구간 포인터를 트리에서 빼고 해제한다
 *
 * @iovad: 대상 도메인
 * @iova:  해제할 구간
 *
 * 슬랩 해제를 락 밖으로 빼낸 것이 이 함수의 형태다. 락 구간을 짧게 유지하는 것이
 * 이 파일 전체의 일관된 방침이며, 트리 락 하나가 도메인 전체를 지키기 때문에
 * 특히 중요하다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: 벤더 드라이버 → [이 함수] → remove_iova, free_iova_mem
 */
void
__free_iova(struct iova_domain *iovad, struct iova *iova)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리 변경 구간 */
	remove_iova(iovad, iova);	/* [한국어] 트리에서 빼고 커서를 갱신 */
	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 슬랩 해제는 락 밖에서 */
	free_iova_mem(iova);	/* [한국어] 구간 객체 반납 */
}
EXPORT_SYMBOL_GPL(__free_iova);	/* [한국어] 구간 포인터를 이미 들고 있을 때의 해제 */

/**
 * free_iova - finds and frees the iova for a given pfn
 * @iovad: - iova domain in question.
 * @pfn: - pfn that is allocated previously
 * This functions finds an iova for a given pfn and then
 * frees the iova from that domain.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * free_iova - pfn 으로 구간을 찾아 해제한다
 *
 * @iovad: 대상 도메인
 * @pfn:   해제할 구간의 시작 pfn
 *
 * 탐색과 제거를 한 락 구간에서 처리하는 것이 __free_iova 와의 차이다. 그래야
 * 찾은 직후 다른 CPU 가 같은 구간을 해제하는 이중 해제를 막을 수 있다.
 *
 * 찾지 못하면 조용히 돌아간다. 이미 해제됐거나 이 도메인의 주소가 아니라는
 * 뜻인데, rcache 를 쓰는 구성에서는 캐시가 붙잡고 있는 구간도 있어 정상적으로
 * 일어날 수 있다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: free_iova_fast(캐시 실패), 벤더 드라이버 → [이 함수]
 */
void
free_iova(struct iova_domain *iovad, unsigned long pfn)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	struct iova *iova;	/* [한국어] 찾은 구간 */

	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 탐색과 제거를 한 락 구간에서 해야 그 사이 해제되지 않는다 */
	iova = private_find_iova(iovad, pfn);	/* [한국어] pfn 으로 구간을 찾는다 */
	if (!iova) {	/* [한국어] 이미 해제됐거나 이 도메인의 주소가 아니다 */
		spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락만 놓고 */
		return;
	}
	remove_iova(iovad, iova);	/* [한국어] 트리에서 제거 */
	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락 해제 */
	free_iova_mem(iova);	/* [한국어] 구간 객체 반납 */
}
EXPORT_SYMBOL_GPL(free_iova);	/* [한국어] pfn 만 알고 있을 때의 해제 */

/**
 * alloc_iova_fast - allocates an iova from rcache
 * @iovad: - iova domain in question
 * @size: - size of page frames to allocate
 * @limit_pfn: - max limit address
 * @flush_rcache: - set to flush rcache on regular allocation failure
 * This function tries to satisfy an iova allocation from the rcache,
 * and falls back to regular allocation on failure. If regular allocation
 * fails too and the flush_rcache flag is set then the rcache will be flushed.
*/
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * alloc_iova_fast - IOVA 구간을 확보한다 (캐시 우선, 실사용 진입점)
 *
 * @iovad:       대상 도메인
 * @size:        필요한 페이지 수
 * @limit_pfn:   포함 상한 (장치의 DMA 마스크에서 온다)
 * @flush_rcache: 실패 시 캐시를 비우고 재시도해도 되는지
 * @return:      확보된 구간의 시작 pfn, 실패하면 0
 *
 * dma_map_page 한 번마다 이 함수가 한 번 돈다고 보면 된다. 그래서 흔한 크기는
 * CPU 로컬 캐시에서 락 없이 처리하고, 캐시가 못 맞출 때만 트리 락을 잡는다.
 * 100G NIC 이나 고성능 NVMe 에서 IOMMU 를 켜도 성능이 버티는 이유의 절반이 여기다.
 *
 * roundup_pow_of_two 가 조용하지만 중요한 줄이다. 캐시는 크기를 등급(로그값)으로만
 * 기억하므로, 3페이지를 넣고 4페이지짜리로 꺼내면 남는 1페이지가 주인 없이 떠돈다.
 * 약간의 주소 공간을 낭비해 그 어긋남을 원천 차단한다 (위 영어 주석).
 *
 * 실패 시 캐시를 비우는 것이 마지막 수단이다. 캐시가 붙잡고 있던 구간이 모두
 * 트리로 돌아오므로 상당한 공간이 회복되지만, 그만큼 이후의 할당이 느려진다.
 * 그래서 재시도는 한 번뿐이다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: dma-iommu 의 iommu_dma_alloc_iova → [이 함수]
 *            → iova_rcache_get → alloc_iova
 */
unsigned long
alloc_iova_fast(struct iova_domain *iovad, unsigned long size,
		unsigned long limit_pfn, bool flush_rcache)
{
	unsigned long iova_pfn;	/* [한국어] 캐시에서 얻은 시작 pfn */
	struct iova *new_iova;	/* [한국어] 트리에서 얻은 구간 */

	/*
	 * Freeing non-power-of-two-sized allocations back into the IOVA caches
	 * will come back to bite us badly, so we have to waste a bit of space
	 * rounding up anything cacheable to make sure that can't happen. The
	 * order of the unadjusted size will still match upon freeing.
	 */
	if (size < (1 << (IOVA_RANGE_CACHE_MAX_SIZE - 1)))	/* [한국어] 캐시가 담을 수 있는 등급 안이면 */
		size = roundup_pow_of_two(size);	/* [한국어] 2의 거듭제곱으로 올린다. 캐시는 크기를 등급(로그값)으로만 기억하므로, 3페이지를 넣고 4페이지로 꺼내는 어긋남이 생기면 주소 공간이 조용히 망가진다. 약간의 낭비로 그것을 막는다 (위 영어 주석) */

	iova_pfn = iova_rcache_get(iovad, size, limit_pfn + 1);	/* [한국어] 먼저 CPU 로컬 캐시를 본다 — 여기서 맞으면 트리 락을 잡지 않는다 */
	if (iova_pfn)	/* [한국어] 캐시 적중 */
		return iova_pfn;	/* [한국어] 가장 흔한 경로이자 이 파일이 성능을 내는 이유 */

retry:	/* [한국어] 캐시를 비우고 한 번 더 시도하는 지점 */
	new_iova = alloc_iova(iovad, size, limit_pfn, true);	/* [한국어] 트리에서 직접 — size_aligned=true 라 큰 페이지로 매핑할 수 있게 정렬된 주소를 받는다 */
	if (!new_iova) {	/* [한국어] 주소 공간이 부족하다 */
		unsigned int cpu;	/* [한국어] 캐시를 비울 CPU 순회 커서 */

		if (!flush_rcache)	/* [한국어] 호출자가 캐시 회수를 허락하지 않았다 */
			return 0;

		/* Try replenishing IOVAs by flushing rcache. */
		flush_rcache = false;	/* [한국어] 재시도는 한 번뿐 — 두 번째도 실패하면 진짜 고갈이다 */
		for_each_online_cpu(cpu)	/* [한국어] 모든 CPU 의 로컬 캐시를 */
			free_cpu_cached_iovas(cpu, iovad);	/* [한국어] 트리로 돌려준다. 캐시가 붙잡고 있던 구간들이 다시 할당 가능해진다 */
		free_global_cached_iovas(iovad);	/* [한국어] 전역 depot 도 비운다 */
		goto retry;	/* [한국어] 회수한 공간으로 다시 시도 */
	}

	return new_iova->pfn_lo;	/* [한국어] 확보한 구간의 시작 pfn */
}
EXPORT_SYMBOL_GPL(alloc_iova_fast);	/* [한국어] dma-iommu 가 dma_map_* 마다 부르는 실사용 진입점 */

/**
 * free_iova_fast - free iova pfn range into rcache
 * @iovad: - iova domain in question.
 * @pfn: - pfn that is allocated previously
 * @size: - # of pages in range
 * This functions frees an iova range by trying to put it into the rcache,
 * falling back to regular iova deallocation via free_iova() if this fails.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * free_iova_fast - IOVA 구간을 돌려준다 (캐시 우선, 실사용 진입점)
 *
 * @iovad: 대상 도메인
 * @pfn:   구간의 시작 pfn
 * @size:  페이지 수
 *
 * 캐시에 들어가면 트리는 전혀 바뀌지 않는다는 점이 핵심이다. 그 구간은 트리에서
 * 여전히 '할당됨' 상태로 남아 있고, 캐시가 "내가 들고 있다"고 기억할 뿐이다.
 * 그래서 해제도 락 없이 끝난다.
 *
 * 크기가 캐시 등급 밖이거나 캐시가 가득 차 새 매거진도 못 얻으면 free_iova 로
 * 내려가 진짜 반납한다.
 *
 * 실행 컨텍스트: 어디서든. dma_unmap 은 인터럽트 문맥에서 오는 경우가 많다.
 *
 * 호출 체인: dma-iommu 의 iommu_dma_free_iova → [이 함수]
 *            → iova_rcache_insert 또는 free_iova
 */
void
free_iova_fast(struct iova_domain *iovad, unsigned long pfn, unsigned long size)
{
	if (iova_rcache_insert(iovad, pfn, size))	/* [한국어] 먼저 CPU 로컬 캐시에 넣어 본다 — 성공하면 트리 락을 잡지 않는다 */
		return;	/* [한국어] 캐시가 받아 줬다. 이 구간은 트리에 그대로 남아 있고, 캐시가 '내가 들고 있다'고 기억할 뿐이다 */

	free_iova(iovad, pfn);	/* [한국어] 캐시가 가득했거나 크기가 등급 밖이면 트리로 진짜 반납한다 */
}
EXPORT_SYMBOL_GPL(free_iova_fast);	/* [한국어] dma-iommu 가 dma_unmap_* 마다 부르는 실사용 진입점 */

/*
 * [한국어]
 * iova_domain_free_rcaches - 도메인의 캐시 계층을 통째로 걷어 낸다
 *
 * @iovad: 대상 도메인
 *
 * CPU 핫플러그 콜백을 먼저 떼는 순서가 전부다. 캐시를 해제하는 도중 CPU 가
 * 내려가면 콜백이 이미 사라진 자료구조를 만지게 된다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥.
 *
 * 호출 체인: put_iova_domain → [이 함수] → free_iova_rcaches
 */
static void iova_domain_free_rcaches(struct iova_domain *iovad)
{
	cpuhp_state_remove_instance_nocalls(CPUHP_IOMMU_IOVA_DEAD,	/* [한국어] CPU 핫플러그 콜백부터 뗀다 — 캐시를 없애는 도중 CPU 가 내려가 그 캐시를 만지면 안 되기 때문 */
					    &iovad->cpuhp_dead);	/* [한국어] 이 도메인의 핫플러그 항목 */
	free_iova_rcaches(iovad);	/* [한국어] CPU 별 캐시와 depot 을 모두 해제 */
}

/**
 * put_iova_domain - destroys the iova domain
 * @iovad: - iova domain in question.
 * All the iova's in that domain are destroyed.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * put_iova_domain - 도메인을 해체하고 모든 구간을 해제한다
 *
 * @iovad: 사라질 도메인
 *
 * 캐시를 먼저 없애고 트리를 비우는 순서다. 캐시가 트리 노드의 pfn 을 들고 있으므로
 * 반대로 하면 이미 해제된 구간을 참조하게 된다.
 *
 * 트리 순회에 후위(postorder) 판을 쓰는 것도 같은 이유다. 노드를 해제하면서
 * 내려가면 부모를 통해 자식으로 가는 링크가 끊기므로, 자식을 먼저 다 본 뒤
 * 부모를 해제해야 한다. 앵커는 free_iova_mem 이 알아서 건너뛴다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥.
 *
 * 호출 체인: dma-iommu 의 iommu_put_dma_cookie → [이 함수]
 */
void put_iova_domain(struct iova_domain *iovad)
{
	struct iova *iova, *tmp;	/* [한국어] 순회하며 해제하므로 _safe 판 */

	if (iovad->rcaches)	/* [한국어] 캐시를 쓴 도메인이면 */
		iova_domain_free_rcaches(iovad);	/* [한국어] 캐시부터 정리 — 캐시가 트리 노드를 참조하고 있어 순서가 중요하다 */

	rbtree_postorder_for_each_entry_safe(iova, tmp, &iovad->rbroot, node)	/* [한국어] 후위 순회 — 자식을 먼저 해제해야 부모를 지나며 접근하지 않는다 */
		free_iova_mem(iova);	/* [한국어] 앵커는 슬랩 객체가 아니라 자동으로 건너뛴다 */
}
EXPORT_SYMBOL_GPL(put_iova_domain);	/* [한국어] 도메인이 사라질 때 dma-iommu 가 부른다 */

/*
 * [한국어]
 * __is_range_overlap - 두 닫힌 구간이 겹치는가
 *
 * @node:   트리 노드 (구간 하나)
 * @pfn_lo: 비교할 구간의 시작
 * @pfn_hi: 비교할 구간의 끝
 * @return: 겹치면 1
 *
 * reserve_iova 전용 판정식이다. 표준적인 "A.시작 ≤ B.끝 && B.시작 ≤ A.끝" 형태이며,
 * 두 구간이 모두 닫힌 구간(끝 포함)이라 등호가 들어간다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: reserve_iova → [이 함수]
 */
static int
__is_range_overlap(struct rb_node *node,
	unsigned long pfn_lo, unsigned long pfn_hi)
{
	struct iova *iova = to_iova(node);	/* [한국어] 이 노드의 구간 */

	if ((pfn_lo <= iova->pfn_hi) && (pfn_hi >= iova->pfn_lo))	/* [한국어] 두 닫힌 구간이 겹치는 표준 판정식 */
		return 1;	/* [한국어] 겹친다 */
	return 0;	/* [한국어] 겹치지 않는다 */
}

/*
 * [한국어]
 * alloc_and_init_iova - 구간 객체를 만들고 범위를 채운다
 *
 * @pfn_lo: 시작
 * @pfn_hi: 끝 (포함)
 * @return: 채워진 구간, 실패하면 NULL
 *
 * 예약 경로 전용의 작은 생성자다. 할당 경로는 범위를 탐색으로 정하므로 이 함수를
 * 쓰지 않고, 예약은 범위가 이미 정해져 있어 이렇게 바로 채운다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: __insert_new_range → [이 함수]
 */
static inline struct iova *
alloc_and_init_iova(unsigned long pfn_lo, unsigned long pfn_hi)
{
	struct iova *iova;	/* [한국어] 만들 구간 */

	iova = alloc_iova_mem();	/* [한국어] 슬랩에서 하나 */
	if (iova) {	/* [한국어] 할당에 성공했으면 */
		iova->pfn_lo = pfn_lo;	/* [한국어] 시작 */
		iova->pfn_hi = pfn_hi;	/* [한국어] 끝 (닫힌 구간) */
	}

	return iova;	/* [한국어] 실패면 NULL */
}

/*
 * [한국어]
 * __insert_new_range - 지정된 범위를 새 구간으로 트리에 심는다
 *
 * @iovad:  대상 도메인
 * @pfn_lo: 시작
 * @pfn_hi: 끝
 * @return: 삽입된 구간, 실패하면 NULL
 *
 * 삽입 힌트 없이 뿌리부터 내려간다. 예약은 부팅 때 몇 번 일어나는 드문 동작이라
 * 할당 경로처럼 최적화할 이유가 없다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: reserve_iova → [이 함수] → iova_insert_rbtree
 */
static struct iova *
__insert_new_range(struct iova_domain *iovad,
	unsigned long pfn_lo, unsigned long pfn_hi)
{
	struct iova *iova;	/* [한국어] 만들어 넣을 구간 */

	iova = alloc_and_init_iova(pfn_lo, pfn_hi);	/* [한국어] 구간 객체 생성 */
	if (iova)	/* [한국어] 성공했으면 */
		iova_insert_rbtree(&iovad->rbroot, iova, NULL);	/* [한국어] 힌트 없이 뿌리부터 삽입 — 예약은 드물게 일어나므로 최적화할 이유가 없다 */

	return iova;	/* [한국어] 삽입된 구간 */
}

/*
 * [한국어]
 * __adjust_overlap_range - 겹치는 예약 구간을 기존 구간으로 흡수한다
 *
 * @iova:    트리에 이미 있는 구간 (필요하면 확장된다)
 * @pfn_lo:  요청 구간의 시작. 흡수된 만큼 위로 밀린다(입출력 인자).
 * @pfn_hi:  요청 구간의 끝
 *
 * 예약 구간은 서로 겹칠 수 있다 — 같은 물리 창을 여러 장치가 요구하는 경우가
 * 흔하다. 겹침을 그대로 두면 같은 주소가 두 구간에 속해 부기가 어긋나므로,
 * 기존 구간을 늘려 흡수하고 요청의 남은 부분만 pfn_lo 에 남긴다.
 *
 * 호출자가 트리를 순서대로 훑으며 반복 호출하는 것을 전제로 한다.
 *
 * 실행 컨텍스트: 트리 락을 든 채.
 *
 * 호출 체인: reserve_iova → [이 함수]
 */
static void
__adjust_overlap_range(struct iova *iova,
	unsigned long *pfn_lo, unsigned long *pfn_hi)
{
	if (*pfn_lo < iova->pfn_lo)	/* [한국어] 요청 구간이 기존 구간보다 아래로 삐져나왔다 */
		iova->pfn_lo = *pfn_lo;	/* [한국어] 기존 구간을 아래로 늘려 흡수한다 */
	if (*pfn_hi > iova->pfn_hi)	/* [한국어] 위쪽으로도 삐져나왔다면 */
		*pfn_lo = iova->pfn_hi + 1;	/* [한국어] 요청의 시작을 기존 구간 바로 위로 밀어, 남은 부분만 따로 처리하게 한다 */
}

/**
 * reserve_iova - reserves an iova in the given range
 * @iovad: - iova domain pointer
 * @pfn_lo: - lower page frame address
 * @pfn_hi:- higher pfn address
 * This function allocates reserves the address range from pfn_lo to pfn_hi so
 * that this address is not dished out as part of alloc_iova.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * reserve_iova - 특정 IOVA 범위를 할당 대상에서 빼둔다
 *
 * @iovad:  대상 도메인
 * @pfn_lo: 예약할 범위의 시작
 * @pfn_hi: 끝 (포함)
 * @return: 예약을 나타내는 구간, 실패하면 NULL
 *
 * 예약이란 "이 범위를 이미 할당된 것처럼 트리에 심어 두는 것"이다. 그러면 이후의
 * alloc 이 그 자리를 절대 내주지 않는다. MSI 도어벨 창이나 펌웨어가 쓰는 RMRR
 * 구간처럼, 장치가 그 주소로 무언가를 하고 있어 DMA 버퍼를 겹쳐 놓으면 안 되는
 * 곳에 쓴다.
 *
 * 트리를 처음부터 훑는 것이 이 함수의 비용인데, 겹침 처리 때문에 그렇다. 요청이
 * 기존 구간 여럿에 걸칠 수 있어 하나씩 흡수해 나가고, 겹침이 끝나면(트리가
 * 정렬되어 있으므로 뒤에는 더 없다) 남은 부분만 새로 심는다.
 *
 * 실행 컨텍스트: 대개 도메인 초기화 직후. 프로세스 문맥.
 *
 * 호출 체인: dma-iommu 의 예약 구간 설정 → [이 함수]
 *            → __adjust_overlap_range, __insert_new_range
 */
struct iova *
reserve_iova(struct iova_domain *iovad,
	unsigned long pfn_lo, unsigned long pfn_hi)
{
	struct rb_node *node;	/* [한국어] 트리 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	struct iova *iova;	/* [한국어] 결과 구간 */
	unsigned int overlap = 0;	/* [한국어] 한 번이라도 겹침을 만났는지 — 정렬된 트리라 겹침이 끝나면 더 볼 필요가 없다 */

	/* Don't allow nonsensical pfns */
	if (WARN_ON((pfn_hi | pfn_lo) > (ULLONG_MAX >> iova_shift(iovad))))	/* [한국어] pfn 을 바이트 주소로 되돌렸을 때 넘치는 값 — 호출자가 주소와 pfn 을 혼동한 것이다 (위 영어 주석) */
		return NULL;	/* [한국어] 말이 안 되는 요청은 거절 */

	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리 변경 구간 */
	for (node = rb_first(&iovad->rbroot); node; node = rb_next(node)) {	/* [한국어] 낮은 주소부터 전체를 훑는다 */
		if (__is_range_overlap(node, pfn_lo, pfn_hi)) {	/* [한국어] 이 구간과 겹친다 */
			iova = to_iova(node);	/* [한국어] 겹치는 구간 */
			__adjust_overlap_range(iova, &pfn_lo, &pfn_hi);	/* [한국어] 기존 구간을 늘려 흡수하고, 요청의 남은 부분만 pfn_lo 에 남긴다 */
			if ((pfn_lo >= iova->pfn_lo) &&	/* [한국어] 요청이 이 구간에 완전히 들어갔다 */
				(pfn_hi <= iova->pfn_hi))	/* [한국어] 더 만들 것이 없다 */
				goto finish;	/* [한국어] 이미 예약된 셈이다 */
			overlap = 1;	/* [한국어] 겹침 구간에 진입했음을 기록 */

		} else if (overlap)	/* [한국어] 겹침이 끝났다 — 트리가 정렬되어 있으므로 뒤에는 더 겹칠 것이 없다 */
				break;	/* [한국어] 순회 중단 */
	}

	/* We are here either because this is the first reserver node
	 * or need to insert remaining non overlap addr range
	 */
	iova = __insert_new_range(iovad, pfn_lo, pfn_hi);	/* [한국어] 겹치지 않는 남은 범위를 새 구간으로 심는다 (위 영어 주석) */
finish:	/* [한국어] 이미 예약되어 있던 경로가 합류 */

	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락 해제 */
	return iova;	/* [한국어] 예약된 구간 (실패 시 NULL) */
}
EXPORT_SYMBOL_GPL(reserve_iova);	/* [한국어] MSI 창이나 RMRR 같은 구간을 할당 대상에서 빼둘 때 쓴다 */

/*
 * Magazine caches for IOVA ranges.  For an introduction to magazines,
 * see the USENIX 2001 paper "Magazines and Vmem: Extending the Slab
 * Allocator to Many CPUs and Arbitrary Resources" by Bonwick and Adams.
 * For simplicity, we use a static magazine size and don't implement the
 * dynamic size tuning described in the paper.
 */

/*
 * As kmalloc's buffer size is fixed to power of 2, 127 is chosen to
 * assure size of 'iova_magazine' to be 1024 bytes, so that no memory
 * will be wasted. Since only full magazines are inserted into the depot,
 * we don't need to waste PFN capacity on a separate list head either.
 */
#define IOVA_MAG_SIZE 127	/* [한국어] 매거진 하나가 담는 pfn 개수. 127 로 맞추면 구조체가 정확히 1024 바이트가 되어 kmalloc 의 2의 거듭제곱 슬랩에서 낭비가 없다 (위 영어 주석) */

#define IOVA_DEPOT_DELAY msecs_to_jiffies(100)	/* [한국어] depot 에 쌓인 매거진을 회수하는 지연 작업 주기. 즉시 회수하면 캐시 효과가 사라지고, 너무 늦으면 주소 공간을 붙잡고 있게 된다 */

/*
 * [한국어] rcache 가 IOVA 구간을 담아 두는 자루(magazine).
 *
 * Bonwick 의 매그니튜드 할당자에서 온 개념이다 (위 영어 주석의 논문). 요점은
 * "CPU 마다 작은 자루를 들려 주고, 자루 단위로만 전역 자료구조를 만지게 한다"는
 * 것이다. 그러면 흔한 할당/해제는 자루 안에서 끝나 전역 락을 잡지 않는다.
 */
struct iova_magazine {
	/* [한국어] size 와 next 가 같은 자리를 나눠 쓴다.
	 * 왜 겹칠 수 있는가: 매거진은 두 상태 중 하나로만 존재한다 — CPU 가 들고
	 *   쓰는 중이면 개수(size)가 필요하고 연결 고리는 필요 없으며, depot 리스트에
	 *   누워 있으면 반대다. depot 에는 '가득 찬' 것만 들어가므로 꺼낼 때 size 를
	 *   IOVA_MAG_SIZE 로 복원하면 정보 손실이 없다.
	 * 부작용: iova_depot_pop 이 next 를 size 로 덮어쓰는 순간 kmemleak 이 잃어버린
	 *   포인터로 오해하므로, 그쪽에서 kmemleak_transient_leak 으로 눌러 준다. */
	union {
	/* [한국어] 현재 담고 있는 pfn 개수 (0 ~ IOVA_MAG_SIZE).
	 * 설정자: iova_magazine_alloc(0), push/pop, iova_magazine_free_pfns(0),
	 *   iova_depot_pop(가득 참으로 복원).
	 * 읽는 자: iova_magazine_full/empty 가 이 값만 보고 판정한다.
	 * 동기화: CPU 별 캐시의 spinlock 아래에서만 읽고 쓴다. */
		unsigned long size;
	/* [한국어] depot 단일 연결 리스트의 다음 매거진.
	 * 설정자: iova_depot_push 가 리스트 앞에 끼울 때.
	 * 읽는 자: iova_depot_pop 이 머리를 옮길 때.
	 * 값 범위: 리스트의 마지막이면 NULL.
	 * 동기화: rcache->lock 아래에서만. size 와 같은 자리이므로, 이 필드가 유효한
	 *   것은 매거진이 depot 에 있는 동안뿐이다. */
		struct iova_magazine *next;
	};
	/* [한국어] 담아 둔 IOVA 구간들의 시작 pfn 배열.
	 * 중요한 점: 여기 있는 구간들은 rbtree 에서 지워지지 않은 채 '할당됨' 상태로
	 *   남아 있다. 캐시가 빠른 이유가 바로 이것으로, 해제와 재할당이 트리를
	 *   전혀 건드리지 않는다. 대신 캐시를 비울 때(free_cpu_cached_iovas)는 각
	 *   pfn 을 트리에서 찾아 진짜로 지워야 한다.
	 * 순서 없음: pop 이 마지막 원소를 빈 자리로 옮기는 O(1) 삭제를 쓴다.
	 * 크기: 이 배열이 구조체를 정확히 1024 바이트로 만든다.
	 * 동기화: CPU 별 캐시의 spinlock. */
	unsigned long pfns[IOVA_MAG_SIZE];
};
static_assert(!(sizeof(struct iova_magazine) & (sizeof(struct iova_magazine) - 1)));	/* [한국어] 크기가 2의 거듭제곱임을 컴파일 시점에 못박는다 — 위 주석의 낭비 없음 전제가 깨지면 여기서 빌드가 멈춘다 */

/*
 * [한국어] 한 CPU, 한 크기 등급이 들고 있는 캐시.
 *
 * 매거진을 둘 두는 것이 설계의 핵심이다. 하나만 두면 가득 찬 순간마다, 또는
 * 비는 순간마다 전역 depot 락을 잡아야 해서 경계에서 성능이 출렁인다. 두 개를
 * 두고 맞바꾸면 그 경계가 자루 하나 분량(127개)만큼 완충된다.
 */
struct iova_cpu_rcache {
	/* [한국어] 이 CPU 캐시를 지키는 락.
	 * 왜 CPU 로컬인데 락이 필요한가: 해제 경로가 인터럽트 문맥에서도 불리므로
	 *   같은 CPU 안에서 프로세스 문맥과 인터럽트가 경쟁할 수 있다. 그래서
	 *   spin_lock_irqsave 로 쓴다.
	 * 다른 CPU 와의 경쟁은 없다 — raw_cpu_ptr 로 자기 것만 만진다. */
	spinlock_t lock;
	/* [한국어] 지금 넣고 꺼내는 매거진.
	 * 설정자: 초기화, prev 와의 swap, depot 에서 받아 올 때, 새로 할당할 때.
	 * 읽는 자: __iova_rcache_insert/get 이 먼저 이것을 본다.
	 * 값 범위: 항상 유효한 포인터 (초기화 실패 시 도메인 자체가 캐시를 안 쓴다).
	 * 동기화: 위의 lock. */
	struct iova_magazine *loaded;
	/* [한국어] 직전에 쓰던 매거진. 완충 역할.
	 * loaded 가 가득 차면(넣기) 또는 비면(꺼내기) 이것과 맞바꾼다. 둘 다 막혔을
	 * 때에야 비로소 전역 depot 으로 내려간다.
	 * 동기화: 위의 lock. */
	struct iova_magazine *prev;
};

/*
 * [한국어] 한 크기 등급(1,2,4,8,16,32 페이지)의 캐시 전체.
 *
 * 위층은 CPU 별 캐시(cpu_rcaches), 아래층은 모든 CPU 가 공유하는 depot 이다.
 * CPU 캐시가 넘치면 가득 찬 매거진을 depot 으로 올리고, 마르면 depot 에서
 * 받아 온다. depot 이 무한정 자라지 않도록 지연 작업이 조금씩 회수한다.
 */
struct iova_rcache {
	/* [한국어] depot 리스트와 depot_size 를 지키는 락.
	 * CPU 캐시 락 안쪽에서 잡힌다 (중첩 순서: cpu_rcache->lock → rcache->lock).
	 * 잡히는 빈도가 낮아야 이 설계가 의미를 갖는다. */
	spinlock_t lock;
	/* [한국어] depot 에 쌓인 매거진 개수.
	 * 읽는 자: iova_depot_work_func 이 num_online_cpus() 와 비교해 회수 여부를
	 *   정한다 — CPU 수만큼은 남겨 각 CPU 가 한 번씩 채울 여지를 보장한다.
	 * 동기화: 위의 lock. */
	unsigned int depot_size;
	/* [한국어] 가득 찬 매거진들의 단일 연결 리스트 머리.
	 * 여기 들어가는 것은 언제나 가득 찬 매거진뿐이라, 꺼낼 때 size 를 복원하는
	 * 것만으로 상태가 완성된다. 그 덕분에 mag->next 와 mag->size 가 자리를
	 * 공유할 수 있다.
	 * 동기화: 위의 lock. */
	struct iova_magazine *depot;
	/* [한국어] CPU 별 캐시 배열 (percpu).
	 * cache_line_size() 정렬로 할당한다 — 이웃 CPU 의 캐시와 같은 캐시라인을
	 * 공유하면 false sharing 으로 이 최적화 자체가 무의미해지기 때문이다.
	 * for_each_possible_cpu 로 만들어 두므로, 나중에 핫플러그로 올라오는 CPU 도
	 * 자기 자리를 이미 갖고 있다.
	 * 값 범위: NULL 이면 이 등급의 초기화가 실패한 것이고, 해제 루프가 그
	 *   지점에서 멈추는 근거가 된다. */
	struct iova_cpu_rcache __percpu *cpu_rcaches;
	/* [한국어] 이 캐시가 속한 도메인으로 되돌아가는 포인터.
	 * 왜 필요한가: 지연 회수 작업(iova_depot_work_func)은 work_struct 만 받으므로
	 *   담긴 pfn 을 어느 트리에 돌려줄지 알 방법이 없다. 그 연결을 이 필드가 준다.
	 * 설정자: iova_domain_init_rcaches. 이후 불변. */
	struct iova_domain *iovad;
	/* [한국어] depot 을 조금씩 비우는 지연 작업.
	 * 예약자: __iova_rcache_insert 가 매거진을 depot 으로 올릴 때마다.
	 * 하는 일: 한 번에 매거진 하나만 회수하고, 남아 있으면 자기를 다시 예약한다.
	 *   한꺼번에 비우면 그 순간 트리 락을 오래 붙잡아 다른 CPU 의 DMA 가 밀린다.
	 * 해제 순서: free_iova_rcaches 가 cancel_delayed_work_sync 로 끝나기를 기다린
	 *   뒤에야 depot 을 만진다. */
	struct delayed_work work;
};

static struct kmem_cache *iova_magazine_cache;	/* [한국어] 매거진 전용 슬랩 */

/*
 * [한국어]
 * iova_rcache_range - 캐시가 담을 수 있는 최대 크기(바이트)
 *
 * @return: PAGE_SIZE << (등급 수 - 1). 기본 설정에서 32페이지 = 128KB.
 *
 * dma-iommu 가 "이 요청이 빠른 경로를 탈 수 있는가"를 판단할 때 쓴다. 이 크기를
 * 넘는 매핑은 매번 트리 락을 잡게 되므로, 큰 버퍼를 자주 매핑하는 워크로드는
 * IOMMU 오버헤드가 눈에 띄게 다르다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: dma-iommu → [이 함수]
 */
unsigned long iova_rcache_range(void)
{
	return PAGE_SIZE << (IOVA_RANGE_CACHE_MAX_SIZE - 1);	/* [한국어] 캐시가 담을 수 있는 최대 바이트 크기. dma-iommu 가 '이 요청이 캐시 경로를 탈 수 있는가'를 판단할 때 쓴다 */
}

/*
 * [한국어]
 * iova_magazine_alloc - 빈 매거진 하나를 만든다
 *
 * @flags:  GFP 플래그. 초기화 경로는 KERNEL, 해제 경로는 ATOMIC.
 * @return: 빈 매거진, 실패하면 NULL
 *
 * zalloc 이 아니라 alloc 인 것에 주목할 것. size 만 0 으로 놓으면 되고, 1KB 짜리
 * pfns 배열은 size 가 가리키는 범위 밖이라 읽히지 않는다. 해제 경로에서 자주
 * 불리므로 그 1KB 를 지우지 않는 것이 실제로 의미가 있다.
 *
 * 실행 컨텍스트: 어디서든. flags 가 정한다.
 *
 * 호출 체인: iova_domain_init_rcaches, __iova_rcache_insert → [이 함수]
 */
static struct iova_magazine *iova_magazine_alloc(gfp_t flags)
{
	struct iova_magazine *mag;	/* [한국어] 만들 매거진 */

	mag = kmem_cache_alloc(iova_magazine_cache, flags);	/* [한국어] 전용 슬랩에서 */
	if (mag)	/* [한국어] 성공했으면 */
		mag->size = 0;	/* [한국어] 빈 매거진으로 시작. zalloc 이 아닌 이유는 pfns 배열 1KB 를 0 으로 채울 필요가 없기 때문이다 */

	return mag;	/* [한국어] 실패면 NULL */
}

/*
 * [한국어]
 * iova_magazine_free - 매거진 객체를 슬랩에 반납한다
 *
 * @mag: 반납할 매거진 (NULL 허용)
 *
 * 담긴 pfn 은 건드리지 않는다. 그것을 트리로 돌려주는 것은
 * iova_magazine_free_pfns 의 일이며, 둘을 나눠 둔 덕분에 도메인이 통째로 사라지는
 * 경우(pfn 을 돌려줄 필요가 없다)와 캐시만 비우는 경우(반드시 돌려줘야 한다)를
 * 같은 코드로 다룰 수 있다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: free_iova_rcaches, __iova_rcache_get, iova_depot_work_func → [이 함수]
 */
static void iova_magazine_free(struct iova_magazine *mag)
{
	if (mag)	/* [한국어] NULL 도 안전하게 받는다 */
		kmem_cache_free(iova_magazine_cache, mag);	/* [한국어] 슬랩에 반납 */
}

/*
 * [한국어]
 * iova_magazine_free_pfns - 매거진이 붙잡고 있던 구간들을 트리로 진짜 반납한다
 *
 * @mag:   비울 매거진
 * @iovad: 구간을 돌려줄 도메인
 *
 * 캐시의 대가를 치르는 자리다. 캐시에 들어간 구간은 트리에서 '할당됨' 상태로
 * 남아 있으므로, 캐시를 비울 때는 각 pfn 을 트리에서 찾아 제거하고 객체까지
 * 해제해야 비로소 주소 공간이 회복된다.
 *
 * 락을 한 번만 잡고 최대 127개를 처리하는 것이 요점이다. 하나씩 free_iova 를
 * 부르면 락을 127번 잡았다 놓게 된다.
 *
 * 찾지 못한 pfn 은 WARN 이다. 캐시와 트리의 부기가 어긋났다는 뜻이고, 그대로 두면
 * 이후 그 주소가 이중 할당된다.
 *
 * 실행 컨텍스트: 캐시 회수 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: free_cpu_cached_iovas, free_global_cached_iovas,
 *            iova_depot_work_func → [이 함수]
 */
static void
iova_magazine_free_pfns(struct iova_magazine *mag, struct iova_domain *iovad)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int i;	/* [한국어] 매거진 순회 커서 */

	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);	/* [한국어] 트리에서 대량으로 지우므로 한 번만 락을 잡는다 */

	for (i = 0 ; i < mag->size; ++i) {	/* [한국어] 매거진이 들고 있던 pfn 하나씩 */
		struct iova *iova = private_find_iova(iovad, mag->pfns[i]);	/* [한국어] 캐시에 넣을 때 트리에서 지우지 않았으므로 여기 있어야 한다 */

		if (WARN_ON(!iova))	/* [한국어] 없다면 캐시와 트리가 어긋난 것 — 심각한 부기 오류다 */
			continue;	/* [한국어] 건너뛰고 나머지라도 정리한다 */

		remove_iova(iovad, iova);	/* [한국어] 이제 진짜로 트리에서 뺀다 */
		free_iova_mem(iova);	/* [한국어] 구간 객체 반납 */
	}

	spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);	/* [한국어] 락 해제 */

	mag->size = 0;	/* [한국어] 매거진을 비운 상태로 표시 — 객체 자체는 재사용한다 */
}

/*
 * [한국어]
 * iova_magazine_full - 더 담을 자리가 없는가
 *
 * @mag:    확인할 매거진
 * @return: 가득 찼으면 true
 *
 * __iova_rcache_insert 가 loaded → prev → depot 순으로 물러나는 판단의 기준이다.
 *
 * 실행 컨텍스트: CPU 캐시 락을 든 채.
 */
static bool iova_magazine_full(struct iova_magazine *mag)
{
	return mag->size == IOVA_MAG_SIZE;	/* [한국어] 더 담을 자리가 없다 */
}

/*
 * [한국어]
 * iova_magazine_empty - 꺼낼 것이 없는가
 *
 * @mag:    확인할 매거진
 * @return: 비었으면 true
 *
 * full 의 대칭. __iova_rcache_get 이 loaded → prev → depot 순으로 물러나는 기준이다.
 *
 * 실행 컨텍스트: CPU 캐시 락을 든 채.
 */
static bool iova_magazine_empty(struct iova_magazine *mag)
{
	return mag->size == 0;	/* [한국어] 꺼낼 것이 없다 */
}

/*
 * [한국어]
 * iova_magazine_pop - 상한을 만족하는 pfn 하나를 꺼낸다
 *
 * @mag:       꺼낼 매거진 (비어 있지 않아야 한다)
 * @limit_pfn: 이 값 이하인 pfn 만 쓸 수 있다
 * @return:    꺼낸 pfn, 쓸 만한 것이 없으면 0
 *
 * 매거진에 값이 있어도 반환이 0 일 수 있다는 점이 중요하다. 32비트 DMA 마스크
 * 장치는 낮은 주소만 쓸 수 있는데 매거진에 높은 주소만 담겨 있으면 캐시가 있어도
 * 소용이 없고, 그때는 호출자가 트리로 내려간다.
 *
 * 뒤에서부터 훑는 것은 최근에 넣은 것이 캐시에 남아 있을 가능성이 높기 때문이고,
 * 삭제를 "마지막 원소를 빈 자리로 옮기기"로 하는 것은 순서가 의미 없는 자루라서
 * O(1) 로 지울 수 있기 때문이다.
 *
 * 실행 컨텍스트: CPU 캐시 락을 든 채.
 *
 * 호출 체인: __iova_rcache_get → [이 함수]
 */
static unsigned long iova_magazine_pop(struct iova_magazine *mag,
				       unsigned long limit_pfn)
{
	int i;	/* [한국어] 뒤에서부터 훑는 커서 */
	unsigned long pfn;	/* [한국어] 꺼낼 pfn */

	/* Only fall back to the rbtree if we have no suitable pfns at all */
	for (i = mag->size - 1; mag->pfns[i] > limit_pfn; i--)	/* [한국어] 상한을 넘는 pfn 은 이 요청에 쓸 수 없다. 뒤에서부터 훑어 상한 이하인 것을 찾는다 (위 영어 주석) */
		if (i == 0)	/* [한국어] 맨 앞까지 왔는데도 못 찾았다 */
			return 0;	/* [한국어] 이 매거진에는 쓸 수 있는 것이 없다 — 호출자는 트리로 내려간다 */

	/* Swap it to pop it */
	pfn = mag->pfns[i];	/* [한국어] 꺼낼 값 */
	mag->pfns[i] = mag->pfns[--mag->size];	/* [한국어] 마지막 원소를 빈 자리로 옮긴다. 순서가 의미 없는 자루라서 O(1) 로 지울 수 있다 (위 영어 주석) */

	return pfn;	/* [한국어] 캐시 적중 */
}

/*
 * [한국어]
 * iova_magazine_push - pfn 하나를 매거진에 담는다
 *
 * @mag: 담을 매거진 (가득 차 있지 않아야 한다)
 * @pfn: 담을 값
 *
 * 검사가 전혀 없다. 호출자(__iova_rcache_insert)가 이미 full 판정을 거쳐 자리를
 * 확보한 뒤에만 부르기 때문이며, 해제 핫패스라 중복 검사를 두지 않았다.
 *
 * 실행 컨텍스트: CPU 캐시 락을 든 채.
 *
 * 호출 체인: __iova_rcache_insert → [이 함수]
 */
static void iova_magazine_push(struct iova_magazine *mag, unsigned long pfn)
{
	mag->pfns[mag->size++] = pfn;	/* [한국어] 맨 뒤에 쌓기만 한다. 호출자가 full 검사를 이미 마친 상태여야 한다 */
}

/*
 * [한국어]
 * iova_depot_pop - 전역 depot 에서 가득 찬 매거진 하나를 꺼낸다
 *
 * @rcache: 이 크기 등급의 캐시
 * @return: 가득 찬 매거진 (호출자가 depot 이 비어 있지 않음을 확인한 뒤 부른다)
 *
 * mag->size = IOVA_MAG_SIZE 한 줄이 두 가지 일을 동시에 한다 — 연결 고리(next)를
 * 덮어 리스트에서 떼어 내고, 동시에 "가득 참" 상태를 복원한다. depot 에는 가득 찬
 * 매거진만 들어간다는 불변식이 있어야 성립하는 요령이다.
 *
 * 그 덮어쓰기가 kmemleak 을 속이므로 미리 transient 로 표시해 둔다 (위 영어 주석).
 *
 * 실행 컨텍스트: rcache->lock 을 든 채.
 *
 * 호출 체인: __iova_rcache_get, free_iova_rcaches, free_global_cached_iovas,
 *            iova_depot_work_func → [이 함수]
 */
static struct iova_magazine *iova_depot_pop(struct iova_rcache *rcache)
{
	struct iova_magazine *mag = rcache->depot;	/* [한국어] depot 은 매거진들의 단일 연결 리스트다 */

	/*
	 * As the mag->next pointer is moved to rcache->depot and reset via
	 * the mag->size assignment, mark it as a transient false positive.
	 */
	kmemleak_transient_leak(mag->next);	/* [한국어] 아래에서 next 필드가 size 로 덮이는 순간 누수 검사기가 잃어버린 포인터로 오해한다. 공용체를 이렇게 쓰기 때문에 생기는 가짜 경고를 미리 눌러 둔다 (위 영어 주석) */
	rcache->depot = mag->next;	/* [한국어] 리스트 머리를 다음으로 */
	mag->size = IOVA_MAG_SIZE;	/* [한국어] depot 에는 가득 찬 매거진만 들어가므로 꺼내는 즉시 가득 참으로 표시한다. next 와 size 가 같은 공용체 자리라 이 대입이 곧 연결 해제이기도 하다 */
	rcache->depot_size--;	/* [한국어] 보관 개수 갱신 */
	return mag;	/* [한국어] 가득 찬 매거진 하나 */
}

/*
 * [한국어]
 * iova_depot_push - 가득 찬 매거진을 전역 depot 에 올린다
 *
 * @rcache: 이 크기 등급의 캐시
 * @mag:    올릴 매거진 (가득 차 있어야 한다)
 *
 * 단일 연결 리스트의 머리에 끼우는 세 줄이다. LIFO 인 것이 유리한데, 방금 올린
 * 매거진이 캐시에 남아 있을 가능성이 높기 때문이다.
 *
 * 실행 컨텍스트: rcache->lock 을 든 채.
 *
 * 호출 체인: __iova_rcache_insert → [이 함수]
 */
static void iova_depot_push(struct iova_rcache *rcache, struct iova_magazine *mag)
{
	mag->next = rcache->depot;	/* [한국어] 리스트 앞에 끼운다 */
	rcache->depot = mag;	/* [한국어] 새 머리 */
	rcache->depot_size++;	/* [한국어] 보관 개수 갱신 */
}

/*
 * [한국어]
 * iova_depot_work_func - depot 에 쌓인 매거진을 조금씩 시스템에 돌려준다
 *
 * @work: 지연 작업 구조체 (여기서 소유 rcache 를 되짚는다)
 *
 * 캐시가 주소 공간을 무한정 붙잡는 것을 막는 안전판이다. 없으면 한 번 몰린 부하가
 * 만든 매거진들이 영원히 depot 에 남아, 그 IOVA 를 다른 크기 등급이나 다른 장치가
 * 영영 쓰지 못하게 된다.
 *
 * 두 가지 절제가 들어 있다. 첫째, CPU 수만큼은 남긴다 — 각 CPU 가 한 번씩 캐시를
 * 채울 여지는 보장한다. 둘째, 한 번에 매거진 하나만 회수하고 남으면 자기를 다시
 * 예약한다. 한꺼번에 비우면 그 순간 트리 락을 오래 붙잡아 다른 CPU 의 DMA 가 밀린다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: 워크큐 → [이 함수] → iova_depot_pop, iova_magazine_free_pfns
 */
static void iova_depot_work_func(struct work_struct *work)
{
	struct iova_rcache *rcache = container_of(work, typeof(*rcache), work.work);	/* [한국어] 지연 작업 구조체에서 소유 rcache 로 되짚는다 */
	struct iova_magazine *mag = NULL;	/* [한국어] 회수할 매거진 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&rcache->lock, flags);	/* [한국어] depot 보호 */
	if (rcache->depot_size > num_online_cpus())	/* [한국어] CPU 수만큼은 남겨 둔다 — 각 CPU 가 한 번씩 캐시를 채울 여지는 보장하고, 그 이상만 시스템에 돌려준다 */
		mag = iova_depot_pop(rcache);	/* [한국어] 한 번에 하나씩만 회수해 락 점유를 짧게 유지한다 */
	spin_unlock_irqrestore(&rcache->lock, flags);	/* [한국어] 락 해제 */

	if (mag) {	/* [한국어] 회수할 것이 있었다면 */
		iova_magazine_free_pfns(mag, rcache->iovad);	/* [한국어] 담고 있던 구간들을 트리에 진짜로 돌려준다 */
		iova_magazine_free(mag);	/* [한국어] 매거진 객체도 슬랩에 반납 */
		schedule_delayed_work(&rcache->work, IOVA_DEPOT_DELAY);	/* [한국어] 아직 더 남았을 수 있으니 다음 회차를 예약한다 — 한 번에 다 비우지 않고 조금씩 흘려보내는 방식이다 */
	}
}

/*
 * [한국어]
 * iova_domain_init_rcaches - 도메인에 CPU 별 캐시 계층을 붙인다
 *
 * @iovad:  대상 도메인 (init_iova_domain 이 이미 끝난 상태)
 * @return: 0 성공, -ENOMEM 이면 캐시 없이 쓸 수는 있으나 성능이 크게 떨어진다
 *
 * 만드는 것은 6개 등급 × CPU 수 × 매거진 2개다. 기본 설정에서 CPU 64개라면
 * 768개의 1KB 매거진, 약 768KB 를 도메인 하나가 미리 잡는다. 그 대가로 DMA
 * 매핑의 대부분이 전역 락을 건드리지 않게 된다.
 *
 * 세 가지 설계 결정이 여기 드러난다.
 *  - cache_line_size() 정렬: 이웃 CPU 의 캐시와 라인을 공유하면 false sharing 으로
 *    최적화가 무의미해진다.
 *  - for_each_possible_cpu: 나중에 핫플러그로 올라올 CPU 도 자리를 미리 갖는다.
 *    online 만 만들면 CPU 가 올라올 때 아토믹 문맥에서 할당해야 한다.
 *  - 매거진 2개: 가득 참/빔 경계에서 전역 depot 으로 내려가는 빈도를 낮춘다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: dma-iommu 의 도메인 초기화 → [이 함수]
 */
int iova_domain_init_rcaches(struct iova_domain *iovad)
{
	unsigned int cpu;	/* [한국어] CPU 순회 커서 */
	int i, ret;	/* [한국어] 크기 등급 커서와 결과 */

	iovad->rcaches = kzalloc_objs(struct iova_rcache,	/* [한국어] 크기 등급마다 하나씩 — 1,2,4,8,16,32 페이지 */
				      IOVA_RANGE_CACHE_MAX_SIZE);	/* [한국어] 여섯 등급 */
	if (!iovad->rcaches)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 캐시 없이 도메인을 쓸 수는 있지만 성능이 크게 떨어진다 */

	for (i = 0; i < IOVA_RANGE_CACHE_MAX_SIZE; ++i) {	/* [한국어] 등급마다 */
		struct iova_cpu_rcache *cpu_rcache;	/* [한국어] CPU 별 캐시 커서 */
		struct iova_rcache *rcache;	/* [한국어] 이 등급의 캐시 */

		rcache = &iovad->rcaches[i];	/* [한국어] 등급 i 의 캐시 */
		spin_lock_init(&rcache->lock);	/* [한국어] depot 을 지키는 락 (CPU 별 캐시는 각자 자기 락을 쓴다) */
		rcache->iovad = iovad;	/* [한국어] 회수 작업이 트리로 돌려주려면 도메인을 알아야 한다 */
		INIT_DELAYED_WORK(&rcache->work, iova_depot_work_func);	/* [한국어] depot 을 조금씩 비우는 지연 작업 */
		rcache->cpu_rcaches = __alloc_percpu(sizeof(*cpu_rcache),	/* [한국어] CPU 별 캐시 배열 */
						     cache_line_size());	/* [한국어] 캐시라인 정렬 — 다른 CPU 의 캐시와 같은 라인을 공유하면 false sharing 으로 이 최적화의 의미가 사라진다 */
		if (!rcache->cpu_rcaches) {	/* [한국어] 할당 실패 */
			ret = -ENOMEM;	/* [한국어] 이유 기록 */
			goto out_err;	/* [한국어] 지금까지 만든 것을 정리한다 */
		}
		for_each_possible_cpu(cpu) {	/* [한국어] online 이 아니라 possible — 나중에 핫플러그로 올라올 CPU 도 자기 캐시를 갖고 있어야 한다 */
			cpu_rcache = per_cpu_ptr(rcache->cpu_rcaches, cpu);	/* [한국어] 그 CPU 의 캐시 */

			spin_lock_init(&cpu_rcache->lock);	/* [한국어] 같은 CPU 안에서 인터럽트와 경쟁할 수 있어 락이 필요하다 */
			cpu_rcache->loaded = iova_magazine_alloc(GFP_KERNEL);	/* [한국어] 현재 쓰는 매거진 */
			cpu_rcache->prev = iova_magazine_alloc(GFP_KERNEL);	/* [한국어] 직전 매거진. 둘을 두는 것이 핵심 — 하나가 가득 차거나 비어도 곧바로 다른 하나로 교체해 depot 까지 내려가지 않는다 */
			if (!cpu_rcache->loaded || !cpu_rcache->prev) {	/* [한국어] 둘 중 하나라도 실패 */
				ret = -ENOMEM;	/* [한국어] 이유 기록 */
				goto out_err;	/* [한국어] 정리 */
			}
		}
	}

	ret = cpuhp_state_add_instance_nocalls(CPUHP_IOMMU_IOVA_DEAD,	/* [한국어] CPU 가 내려갈 때 그 CPU 의 캐시를 비우도록 콜백을 건다. 비우지 않으면 그 구간들이 영영 붙잡힌 채 남는다 */
					       &iovad->cpuhp_dead);	/* [한국어] 이 도메인의 항목 */
	if (ret)	/* [한국어] 등록 실패 */
		goto out_err;	/* [한국어] 정리 */
	return 0;	/* [한국어] 캐시 준비 완료 — 이제 할당의 대부분이 락 없이 처리된다 */

out_err:	/* [한국어] 공통 되감기 */
	free_iova_rcaches(iovad);	/* [한국어] 부분적으로 만들어진 것까지 모두 해제한다 */
	return ret;	/* [한국어] 실패 이유 */
}
EXPORT_SYMBOL_GPL(iova_domain_init_rcaches);	/* [한국어] dma-iommu 가 도메인을 세울 때 부른다 */

/*
 * Try inserting IOVA range starting with 'iova_pfn' into 'rcache', and
 * return true on success.  Can fail if rcache is full and we can't free
 * space, and free_iova() (our only caller) will then return the IOVA
 * range to the rbtree instead.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iova_rcache_insert - 한 등급의 캐시에 pfn 하나를 넣는다
 *
 * @iovad:    대상 도메인 (실제로는 쓰이지 않고 호환을 위해 남아 있다)
 * @rcache:   이 크기 등급의 캐시
 * @iova_pfn: 넣을 구간의 시작 pfn
 * @return:   넣었으면 true. false 면 호출자가 트리로 진짜 반납한다.
 *
 * 세 단계로 물러난다: loaded 에 자리가 있으면 넣고, 없으면 prev 와 맞바꿔 보고,
 * 둘 다 찼으면 loaded 를 depot 으로 올리고 새 매거진을 받는다. 마지막 단계에서만
 * 전역 락과 메모리 할당이 일어나며, 그것도 127번에 한 번꼴이다.
 *
 * GFP_ATOMIC 할당이 실패하면 false 를 돌려준다. 캐시에 못 넣는 것은 오류가 아니라
 * 정상적인 후퇴이며, 호출자가 트리에 반납하면 그만이다 (위 영어 주석).
 *
 * 실행 컨텍스트: 어디서든. 해제 경로는 인터럽트 문맥에서 온다.
 *
 * 호출 체인: iova_rcache_insert → [이 함수] → iova_depot_push, iova_magazine_push
 */
static bool __iova_rcache_insert(struct iova_domain *iovad,
				 struct iova_rcache *rcache,
				 unsigned long iova_pfn)
{
	struct iova_cpu_rcache *cpu_rcache;	/* [한국어] 이 CPU 의 캐시 */
	bool can_insert = false;	/* [한국어] 넣을 자리를 확보했는지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	cpu_rcache = raw_cpu_ptr(rcache->cpu_rcaches);	/* [한국어] 선점 비활성 없이 현재 CPU 의 것을 집는다 — 아래 락이 정확성을 책임지므로 CPU 가 바뀌어도 안전하다 */
	spin_lock_irqsave(&cpu_rcache->lock, flags);	/* [한국어] 이 CPU 의 캐시 보호 */

	if (!iova_magazine_full(cpu_rcache->loaded)) {	/* [한국어] 현재 매거진에 자리가 있다 — 가장 흔한 경우 */
		can_insert = true;	/* [한국어] 바로 넣는다 */
	} else if (!iova_magazine_full(cpu_rcache->prev)) {	/* [한국어] 현재 것은 찼지만 직전 것에 자리가 있다 */
		swap(cpu_rcache->prev, cpu_rcache->loaded);	/* [한국어] 둘을 맞바꾼다 — 두 개를 두는 이유가 이 한 줄이다 */
		can_insert = true;	/* [한국어] 이제 넣을 수 있다 */
	} else {
		struct iova_magazine *new_mag = iova_magazine_alloc(GFP_ATOMIC);	/* [한국어] 둘 다 찼다 — 새 매거진이 필요하다. 해제 경로는 인터럽트 문맥일 수 있어 ATOMIC */

		if (new_mag) {	/* [한국어] 확보에 성공했으면 */
			spin_lock(&rcache->lock);	/* [한국어] depot 보호 (바깥에서 이미 irqsave 상태다) */
			iova_depot_push(rcache, cpu_rcache->loaded);	/* [한국어] 가득 찬 매거진을 전역 depot 으로 올린다 */
			spin_unlock(&rcache->lock);	/* [한국어] depot 락 해제 */
			schedule_delayed_work(&rcache->work, IOVA_DEPOT_DELAY);	/* [한국어] depot 이 무한정 쌓이지 않도록 회수 작업을 예약 */

			cpu_rcache->loaded = new_mag;	/* [한국어] 빈 매거진으로 교체 */
			can_insert = true;	/* [한국어] 넣을 수 있다 */
		}
	}

	if (can_insert)	/* [한국어] 자리를 확보했으면 */
		iova_magazine_push(cpu_rcache->loaded, iova_pfn);	/* [한국어] 캐시에 담는다. 이 구간은 트리에서 지워지지 않고 '할당된 채' 남아 있다 — 그것이 캐시가 빠른 이유다 */

	spin_unlock_irqrestore(&cpu_rcache->lock, flags);	/* [한국어] 락 해제 */

	return can_insert;	/* [한국어] false 면 호출자가 트리로 진짜 반납한다 (위 영어 주석) */
}

/*
 * [한국어]
 * iova_rcache_insert - 크기를 등급으로 바꿔 해당 캐시에 넣는다
 *
 * @iovad:  대상 도메인
 * @pfn:    구간의 시작 pfn
 * @size:   페이지 수
 * @return: 캐시가 받았으면 true
 *
 * 등급은 order_base_2(size), 즉 크기의 로그값이다. alloc_iova_fast 가 크기를
 * 2의 거듭제곱으로 올려 두기 때문에, 넣을 때와 꺼낼 때의 등급이 반드시 일치한다.
 * 그 보장이 없으면 3페이지를 넣고 4페이지로 꺼내는 어긋남이 생겨 주소 공간이
 * 조용히 새어 나간다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: free_iova_fast → [이 함수] → __iova_rcache_insert
 */
static bool iova_rcache_insert(struct iova_domain *iovad, unsigned long pfn,
			       unsigned long size)
{
	unsigned int log_size = order_base_2(size);	/* [한국어] 크기를 등급(로그값)으로. free_iova_fast 가 넣을 때와 alloc 이 꺼낼 때 같은 등급이어야 하므로 alloc 쪽에서 2의 거듭제곱으로 올려 둔다 */

	if (log_size >= IOVA_RANGE_CACHE_MAX_SIZE)	/* [한국어] 캐시가 다루지 않는 큰 크기 */
		return false;	/* [한국어] 트리로 보낸다 */

	return __iova_rcache_insert(iovad, &iovad->rcaches[log_size], pfn);	/* [한국어] 해당 등급의 캐시에 넣는다 */
}

/*
 * Caller wants to allocate a new IOVA range from 'rcache'.  If we can
 * satisfy the request, return a matching non-NULL range and remove
 * it from the 'rcache'.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iova_rcache_get - 한 등급의 캐시에서 pfn 하나를 꺼낸다
 *
 * @rcache:    이 크기 등급의 캐시
 * @limit_pfn: 꺼낸 구간의 시작이 이 값 이하여야 한다
 * @return:    꺼낸 pfn, 실패하면 0
 *
 * insert 의 정확한 거울상이다: loaded → prev 와 맞바꾸기 → depot 에서 받아 오기.
 * depot 에서 받아 올 때 빈 loaded 를 그냥 버리는데, depot 에서 온 것이 그 자리를
 * 대신하므로 매거진 개수는 유지된다.
 *
 * 상한 검사가 매거진 안에서 이뤄진다는 점에 주의할 것. 매거진에 값이 있어도
 * 전부 상한 위쪽이면 0 이 나오고, 그때는 캐시가 있어도 트리로 내려가야 한다.
 * 32비트 마스크 장치가 IOMMU 아래에서 느려지는 이유 중 하나가 이것이다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: iova_rcache_get → [이 함수] → iova_depot_pop, iova_magazine_pop
 */
static unsigned long __iova_rcache_get(struct iova_rcache *rcache,
				       unsigned long limit_pfn)
{
	struct iova_cpu_rcache *cpu_rcache;	/* [한국어] 이 CPU 의 캐시 */
	unsigned long iova_pfn = 0;	/* [한국어] 꺼낸 pfn (0 이면 실패) */
	bool has_pfn = false;	/* [한국어] 꺼낼 수 있는 매거진을 확보했는지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	cpu_rcache = raw_cpu_ptr(rcache->cpu_rcaches);	/* [한국어] 현재 CPU 의 캐시 */
	spin_lock_irqsave(&cpu_rcache->lock, flags);	/* [한국어] 이 CPU 의 캐시 보호 */

	if (!iova_magazine_empty(cpu_rcache->loaded)) {	/* [한국어] 현재 매거진에 남은 것이 있다 — 가장 흔한 경우 */
		has_pfn = true;	/* [한국어] 바로 꺼낸다 */
	} else if (!iova_magazine_empty(cpu_rcache->prev)) {	/* [한국어] 비었지만 직전 매거진에 남아 있다 */
		swap(cpu_rcache->prev, cpu_rcache->loaded);	/* [한국어] 맞바꾼다 */
		has_pfn = true;	/* [한국어] 꺼낼 수 있다 */
	} else {
		spin_lock(&rcache->lock);	/* [한국어] 둘 다 비었다 — 전역 depot 을 본다 */
		if (rcache->depot) {	/* [한국어] depot 에 가득 찬 매거진이 있으면 */
			iova_magazine_free(cpu_rcache->loaded);	/* [한국어] 빈 매거진은 버린다 — depot 에서 온 것이 그 자리를 대신한다 */
			cpu_rcache->loaded = iova_depot_pop(rcache);	/* [한국어] 가득 찬 것을 받아 온다 */
			has_pfn = true;	/* [한국어] 꺼낼 수 있다 */
		}
		spin_unlock(&rcache->lock);	/* [한국어] depot 락 해제 */
	}

	if (has_pfn)	/* [한국어] 확보했으면 */
		iova_pfn = iova_magazine_pop(cpu_rcache->loaded, limit_pfn);	/* [한국어] 상한을 만족하는 pfn 을 꺼낸다. 매거진에 있어도 상한을 넘으면 0 이 나오고, 그때는 트리로 내려간다 */

	spin_unlock_irqrestore(&cpu_rcache->lock, flags);	/* [한국어] 락 해제 */

	return iova_pfn;	/* [한국어] 0 이면 캐시 미스 */
}

/*
 * Try to satisfy IOVA allocation range from rcache.  Fail if requested
 * size is too big or the DMA limit we are given isn't satisfied by the
 * top element in the magazine.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iova_rcache_get - 크기를 등급으로 바꿔 해당 캐시에서 꺼낸다
 *
 * @iovad:     대상 도메인
 * @size:      페이지 수 (이미 2의 거듭제곱으로 올림된 값)
 * @limit_pfn: 배타 상한
 * @return:    확보한 구간의 시작 pfn, 실패하면 0
 *
 * limit_pfn 에서 size 를 빼서 넘기는 한 줄이 이 함수의 실질이다. 매거진이 담고
 * 있는 것은 구간의 '시작' pfn 이므로, 시작이 (상한 − 크기) 이하여야 구간 전체가
 * 상한 안에 들어간다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: alloc_iova_fast → [이 함수] → __iova_rcache_get
 */
static unsigned long iova_rcache_get(struct iova_domain *iovad,
				     unsigned long size,
				     unsigned long limit_pfn)
{
	unsigned int log_size = order_base_2(size);	/* [한국어] 크기를 등급으로 */

	if (log_size >= IOVA_RANGE_CACHE_MAX_SIZE)	/* [한국어] 캐시가 다루지 않는 큰 크기 (위 영어 주석) */
		return 0;	/* [한국어] 트리로 내려간다 */

	return __iova_rcache_get(&iovad->rcaches[log_size], limit_pfn - size);	/* [한국어] 상한에서 크기를 빼 넘긴다 — 매거진에 담긴 것은 구간의 '시작' pfn 이므로, 시작이 이 값 이하여야 구간 전체가 상한 안에 들어간다 */
}

/*
 * free rcache data structures.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * free_iova_rcaches - 캐시 계층 전체를 해제한다 (도메인이 사라질 때)
 *
 * @iovad: 해체 중인 도메인
 *
 * 담긴 pfn 을 트리로 돌려주지 않는 것이 free_cpu_cached_iovas 와의 차이다.
 * 도메인 자체가 사라지는 중이라 트리도 곧 통째로 비워지므로, 하나씩 돌려주는
 * 것은 낭비다.
 *
 * cpu_rcaches 가 NULL 인 지점에서 멈추는 것은 초기화 실패 되감기를 겸하기
 * 때문이다. iova_domain_init_rcaches 가 중간에 실패하면 그 등급부터는 아무것도
 * 만들어지지 않았다.
 *
 * cancel_delayed_work_sync 를 depot 을 비우기 전에 부르는 순서가 중요하다.
 * 회수 작업이 돌고 있는 상태에서 depot 을 만지면 같은 매거진을 두 번 해제한다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥 (sync 대기가 있어 잠들 수 있다).
 *
 * 호출 체인: iova_domain_free_rcaches → [이 함수]
 */
static void free_iova_rcaches(struct iova_domain *iovad)
{
	struct iova_rcache *rcache;	/* [한국어] 등급별 캐시 커서 */
	struct iova_cpu_rcache *cpu_rcache;	/* [한국어] CPU 별 캐시 커서 */
	unsigned int cpu;	/* [한국어] CPU 순회 커서 */

	for (int i = 0; i < IOVA_RANGE_CACHE_MAX_SIZE; ++i) {	/* [한국어] 모든 등급에 대해 */
		rcache = &iovad->rcaches[i];	/* [한국어] 이 등급 */
		if (!rcache->cpu_rcaches)	/* [한국어] 초기화 도중 실패한 지점 — 여기부터는 만들어진 것이 없다 */
			break;	/* [한국어] 더 정리할 것이 없다 */
		for_each_possible_cpu(cpu) {	/* [한국어] 모든 CPU 의 */
			cpu_rcache = per_cpu_ptr(rcache->cpu_rcaches, cpu);	/* [한국어] 캐시를 집어 */
			iova_magazine_free(cpu_rcache->loaded);	/* [한국어] 매거진 객체만 해제한다. 담긴 pfn 은 도메인 전체가 사라지는 중이라 트리로 돌려줄 필요가 없다 */
			iova_magazine_free(cpu_rcache->prev);	/* [한국어] 직전 매거진도 */
		}
		free_percpu(rcache->cpu_rcaches);	/* [한국어] CPU 별 배열 해제 */
		cancel_delayed_work_sync(&rcache->work);	/* [한국어] 회수 작업이 돌고 있으면 끝날 때까지 기다린다 — 아래에서 depot 을 비우기 전에 반드시 */
		while (rcache->depot)	/* [한국어] depot 에 남은 매거진을 */
			iova_magazine_free(iova_depot_pop(rcache));	/* [한국어] 모두 해제 */
	}

	kfree(iovad->rcaches);	/* [한국어] 등급 배열 해제 */
	iovad->rcaches = NULL;	/* [한국어] 두 번 해제되지 않도록 끊는다 */
}

/*
 * free all the IOVA ranges cached by a cpu (used when cpu is unplugged)
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * free_cpu_cached_iovas - 한 CPU 가 붙잡고 있던 구간을 모두 트리로 돌려준다
 *
 * @cpu:   대상 CPU
 * @iovad: 대상 도메인
 *
 * 두 곳에서 불리며 의미가 조금 다르다.
 *  - CPU 핫플러그(iova_cpuhp_dead): 내려가는 CPU 의 캐시를 회수하지 않으면 그
 *    주소 공간을 영영 잃는다. 그 CPU 는 다시 이 캐시를 만지지 않기 때문이다.
 *  - 할당 실패(alloc_iova_fast): 주소 공간이 부족할 때 마지막 수단으로 모든 CPU 의
 *    캐시를 비워 공간을 회복한다.
 *
 * free_iova_rcaches 와 달리 여기서는 pfn 을 트리로 진짜 돌려준다. 도메인이 계속
 * 살아 있으므로 그 주소를 다시 쓸 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 핫플러그 콜백 또는 할당 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iova_cpuhp_dead, alloc_iova_fast → [이 함수] → iova_magazine_free_pfns
 */
static void free_cpu_cached_iovas(unsigned int cpu, struct iova_domain *iovad)
{
	struct iova_cpu_rcache *cpu_rcache;	/* [한국어] 비울 CPU 의 캐시 */
	struct iova_rcache *rcache;	/* [한국어] 등급별 캐시 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int i;	/* [한국어] 등급 커서 */

	for (i = 0; i < IOVA_RANGE_CACHE_MAX_SIZE; ++i) {	/* [한국어] 모든 등급에 대해 */
		rcache = &iovad->rcaches[i];	/* [한국어] 이 등급 */
		cpu_rcache = per_cpu_ptr(rcache->cpu_rcaches, cpu);	/* [한국어] 지정된 CPU 의 캐시 */
		spin_lock_irqsave(&cpu_rcache->lock, flags);	/* [한국어] 그 캐시 보호 */
		iova_magazine_free_pfns(cpu_rcache->loaded, iovad);	/* [한국어] 여기서는 담긴 pfn 을 트리로 진짜 돌려준다 — 도메인은 계속 살아 있으므로 주소 공간을 회수해야 한다 */
		iova_magazine_free_pfns(cpu_rcache->prev, iovad);	/* [한국어] 직전 매거진도 */
		spin_unlock_irqrestore(&cpu_rcache->lock, flags);	/* [한국어] 락 해제 */
	}
}

/*
 * free all the IOVA ranges of global cache
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * free_global_cached_iovas - 전역 depot 에 쌓인 구간을 모두 트리로 돌려준다
 *
 * @iovad: 대상 도메인
 *
 * alloc_iova_fast 의 마지막 수단 경로에서, CPU 캐시를 비운 직후에 불린다. depot 은
 * CPU 캐시가 넘칠 때마다 쌓이므로 부하가 몰린 뒤에는 여기 상당한 주소 공간이
 * 묶여 있을 수 있다.
 *
 * 지연 회수 작업(iova_depot_work_func)이 하는 일과 같지만, 그쪽은 조금씩 천천히
 * 하고 이쪽은 지금 당장 전부 비운다. 할당이 실패한 상황이라 성능보다 성공이 먼저다.
 *
 * 실행 컨텍스트: 할당 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: alloc_iova_fast → [이 함수] → iova_depot_pop, iova_magazine_free_pfns
 */
static void free_global_cached_iovas(struct iova_domain *iovad)
{
	struct iova_rcache *rcache;	/* [한국어] 등급별 캐시 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	for (int i = 0; i < IOVA_RANGE_CACHE_MAX_SIZE; ++i) {	/* [한국어] 모든 등급에 대해 */
		rcache = &iovad->rcaches[i];	/* [한국어] 이 등급 */
		spin_lock_irqsave(&rcache->lock, flags);	/* [한국어] depot 보호 */
		while (rcache->depot) {	/* [한국어] 쌓인 매거진 전부 */
			struct iova_magazine *mag = iova_depot_pop(rcache);	/* [한국어] 하나씩 꺼내 */

			iova_magazine_free_pfns(mag, iovad);	/* [한국어] 담긴 구간을 트리로 돌려주고 */
			iova_magazine_free(mag);	/* [한국어] 매거진도 해제 */
		}
		spin_unlock_irqrestore(&rcache->lock, flags);	/* [한국어] 락 해제 */
	}
}

/*
 * [한국어]
 * iova_cpuhp_dead - CPU 가 내려갈 때 그 CPU 의 캐시를 비운다
 *
 * @cpu:    사라지는 CPU
 * @node:   등록된 도메인의 핫플러그 노드
 * @return: 0 (실패할 일이 없다)
 *
 * CPU 별 캐시는 그 CPU 만 만지므로, CPU 가 사라지면 그 캐시에 담긴 IOVA 를
 * 아무도 꺼내 쓰지 않는다. 회수하지 않으면 도메인이 살아 있는 내내 그만큼의
 * 주소 공간이 묶인 채 남는다.
 *
 * multi 형 핫플러그 상태에 도메인마다 인스턴스로 등록되므로, CPU 하나가 내려가면
 * 시스템의 모든 IOVA 도메인에 대해 이 콜백이 한 번씩 돈다.
 *
 * 실행 컨텍스트: CPU 핫플러그. 프로세스 문맥.
 *
 * 호출 체인: cpuhp 코어 → [이 함수] → free_cpu_cached_iovas
 */
static int iova_cpuhp_dead(unsigned int cpu, struct hlist_node *node)
{
	struct iova_domain *iovad;	/* [한국어] 콜백에 등록된 도메인 */

	iovad = hlist_entry_safe(node, struct iova_domain, cpuhp_dead);	/* [한국어] 핫플러그 노드에서 도메인으로 되짚는다 */

	free_cpu_cached_iovas(cpu, iovad);	/* [한국어] 내려가는 CPU 가 붙잡고 있던 구간을 모두 트리로 돌려준다. 하지 않으면 그 주소 공간을 영영 잃는다 */
	return 0;	/* [한국어] 콜백 성공 */
}

/*
 * [한국어]
 * iova_cache_get - 이 파일이 쓰는 두 슬랩을 준비한다 (참조 계수)
 *
 * @return: 0 성공, 음수면 슬랩이나 핫플러그 등록 실패
 *
 * struct iova 와 struct iova_magazine 은 시스템의 모든 IOVA 도메인이 공유하므로,
 * 슬랩도 하나씩만 두고 참조 계수로 관리한다. 첫 도메인이 만들고 마지막 도메인이
 * 없앤다.
 *
 * CPU 사망 콜백 상태도 여기서 등록한다. multi 형이라 도메인마다 인스턴스를 붙일
 * 수 있고, 실제 인스턴스 등록은 iova_domain_init_rcaches 가 한다.
 *
 * 되감기가 한 라벨로 끝나는 것은 kmem_cache_destroy 가 NULL 을 안전하게 받기
 * 때문이다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 문맥, 뮤텍스를 잡는다.
 *
 * 호출 체인: dma-iommu, 벤더 드라이버 초기화 → [이 함수]
 */
int iova_cache_get(void)
{
	int err = -ENOMEM;	/* [한국어] 기본 실패 이유 */

	mutex_lock(&iova_cache_mutex);	/* [한국어] 슬랩 생성/파괴 직렬화 */
	if (!iova_cache_users) {	/* [한국어] 첫 사용자일 때만 실제로 만든다 — 여러 도메인이 슬랩 하나를 공유한다 */
		iova_cache = kmem_cache_create("iommu_iova", sizeof(struct iova), 0,	/* [한국어] 구간 객체 전용 슬랩 */
					       SLAB_HWCACHE_ALIGN, NULL);	/* [한국어] 캐시라인 정렬 — 트리 노드는 다른 CPU 가 동시에 훑는다 */
		if (!iova_cache)	/* [한국어] 생성 실패 */
			goto out_err;	/* [한국어] 정리 */

		iova_magazine_cache = kmem_cache_create("iommu_iova_magazine",	/* [한국어] 매거진 전용 슬랩 */
							sizeof(struct iova_magazine),	/* [한국어] 1KB 고정 크기 */
							0, SLAB_HWCACHE_ALIGN, NULL);	/* [한국어] 캐시라인 정렬 */
		if (!iova_magazine_cache)	/* [한국어] 생성 실패 */
			goto out_err;	/* [한국어] 정리 */

		err = cpuhp_setup_state_multi(CPUHP_IOMMU_IOVA_DEAD, "iommu/iova:dead",	/* [한국어] CPU 사망 콜백 상태를 등록한다. multi 판이라 도메인마다 인스턴스를 따로 붙일 수 있다 */
					      NULL, iova_cpuhp_dead);	/* [한국어] 올라올 때는 할 일이 없고, 내려갈 때만 캐시를 비운다 */
		if (err) {	/* [한국어] 등록 실패 */
			pr_err("IOVA: Couldn't register cpuhp handler: %pe\n", ERR_PTR(err));	/* [한국어] 캐시를 안전하게 쓸 수 없다는 뜻이라 반드시 남긴다 */
			goto out_err;	/* [한국어] 정리 */
		}
	}

	iova_cache_users++;	/* [한국어] 참조 계수 증가 */
	mutex_unlock(&iova_cache_mutex);	/* [한국어] 락 해제 */

	return 0;	/* [한국어] 슬랩 준비 완료 */

out_err:	/* [한국어] 공통 되감기 */
	kmem_cache_destroy(iova_cache);	/* [한국어] NULL 도 안전하게 받는다 */
	kmem_cache_destroy(iova_magazine_cache);	/* [한국어] 마찬가지 */
	mutex_unlock(&iova_cache_mutex);	/* [한국어] 락 해제 */
	return err;	/* [한국어] 실패 이유 */
}
EXPORT_SYMBOL_GPL(iova_cache_get);	/* [한국어] dma-iommu 가 첫 도메인을 만들 때 부른다 */

/*
 * [한국어]
 * iova_cache_put - 슬랩 참조를 놓고, 마지막이면 정리한다
 *
 * get 의 짝. 마지막 사용자일 때 핫플러그 콜백을 먼저 떼고 슬랩을 없애는 순서가
 * 중요하다 — 반대로 하면 콜백이 이미 해제된 슬랩의 객체를 만질 수 있다.
 *
 * 계수가 0 인데 불렸다면 get/put 짝이 맞지 않는 것이라 WARN 만 내고 아무 것도
 * 하지 않는다. 여기서 계수를 음수로 만들면 다음 get 이 슬랩을 다시 만들지 않아
 * NULL 슬랩에 할당하게 된다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥, 뮤텍스를 잡는다.
 *
 * 호출 체인: dma-iommu, 벤더 드라이버 해제 → [이 함수]
 */
void iova_cache_put(void)
{
	mutex_lock(&iova_cache_mutex);	/* [한국어] 슬랩 파괴 직렬화 */
	if (WARN_ON(!iova_cache_users)) {	/* [한국어] get 없이 put — 짝이 맞지 않는 호출 */
		mutex_unlock(&iova_cache_mutex);	/* [한국어] 락만 놓고 */
		return;	/* [한국어] 아무 것도 하지 않는다 */
	}
	iova_cache_users--;	/* [한국어] 참조 계수 감소 */
	if (!iova_cache_users) {	/* [한국어] 마지막 사용자였다면 */
		cpuhp_remove_multi_state(CPUHP_IOMMU_IOVA_DEAD);	/* [한국어] 콜백부터 뗀다 — 슬랩을 없앤 뒤 콜백이 돌면 해제된 메모리를 만진다 */
		kmem_cache_destroy(iova_cache);	/* [한국어] 구간 슬랩 파괴 */
		kmem_cache_destroy(iova_magazine_cache);	/* [한국어] 매거진 슬랩 파괴 */
	}
	mutex_unlock(&iova_cache_mutex);	/* [한국어] 락 해제 */
}
EXPORT_SYMBOL_GPL(iova_cache_put);	/* [한국어] 마지막 도메인이 사라질 때 */

MODULE_AUTHOR("Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>");	/* [한국어] 원저자 — 인텔 VT-d 전용으로 시작해 공용 계층이 되었다 */
MODULE_DESCRIPTION("IOMMU I/O Virtual Address management");	/* [한국어] 모듈 설명 */
MODULE_LICENSE("GPL");	/* [한국어] 라이선스 선언 */
