/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 *
 */
/*
 * [한국어 설명] iommufd 의 IOVA 공간 자료 모델 (io_pagetable.h)
 *
 * === 파일의 역할 ===
 * 사용자 공간이 "이 IOVA 에 내 메모리의 이 부분을 붙여라"라고 말할 때,
 * 그 대응을 커널이 어떻게 들고 있는지를 정하는 헤더다.
 *
 * 두 개의 축이 있다.
 *
 * 1) struct iopt_area — IOVA 공간의 한 구간. 어느 IOVA 부터 어디까지가
 *    어떤 메모리로 채워져 있는가를 말한다. 구간 트리에 담겨 겹침 검사와
 *    빈자리 찾기가 로그 시간에 된다.
 *
 * 2) struct iopt_pages — 고정(pin)된 페이지들의 묶음. 사용자 VA 의 연속된
 *    한 덩어리에서 온다.
 *
 * 둘을 나눈 이유가 이 설계의 핵심이다. 같은 사용자 메모리를 여러 IOVA
 * 공간에(또는 한 공간의 여러 자리에) 붙일 수 있는데, 그때마다 페이지를
 * 다시 고정하고 사용자의 잠금 메모리 한도에 다시 계상하면 안 된다.
 * 그래서 area 는 여럿이되 pages 는 하나를 공유한다.
 *
 * 페이지 고정이 게으르다는 점도 중요하다. area 가 생겼다고 곧바로 전부
 * 고정하지 않고, 실제로 도메인에 채워지거나 커널이 접근할 때만 고정한다.
 * pinned_pfns 배열과 두 구간 트리(access_itree, domains_itree)가 그
 * "지금 누가 이 부분을 필요로 하는가"를 추적한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간 ioctl → ioas.c → io_pagetable.c(구간 관리) →
 *   pages.c(고정과 도메인 채우기) → iommu_map() → 드라이버 → 하드웨어
 *
 * 실행 컨텍스트: 프로세스 문맥. 파일 상단의 원 주석이 어느 락이 어느
 * 필드를 지키는지 명시한다 — 이 계층은 락이 여러 겹이라 그 표가 곧
 * 읽는 순서다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommufd/ioas.c, hw_pagetable.c, device.c.
 * 아래: <linux/interval_tree.h>, <linux/dma-buf.h>, iommu_map/unmap.
 *
 * === 주요 함수/구조체 요약 ===
 * struct iopt_area: IOVA 구간 하나. 두 개의 구간 트리 노드를 갖는데,
 *   하나는 IOVA 로 정렬되고 다른 하나는 페이지 색인으로 정렬된다.
 * struct iopt_pages: 고정된 페이지 묶음. 사용자 VA, 파일, dma-buf 세
 *   출처를 union 으로 다룬다.
 * struct iopt_allowed / iopt_reserved: 사용자가 허용한 IOVA 범위와,
 *   장치가 예약해 못 쓰는 범위.
 * __make_iopt_iter: 세 종류의 구간 트리 순회기를 매크로로 찍어 낸다.
 * iopt_area_contig_*: 여러 area 에 걸친 연속 범위를 훑는다.
 * iopt_area_start_byte: IOVA 를 pages 안의 바이트 오프셋으로 옮긴다 —
 *   두 좌표계를 잇는 함수라 이 계층 전반에서 쓰인다.
 */
#ifndef __IO_PAGETABLE_H	/* [한국어] 중복 포함 방지 */
#define __IO_PAGETABLE_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/dma-buf.h>	/* [한국어] dma-buf 출처의 페이지 */
#include <linux/interval_tree.h>	/* [한국어] IOVA 구간을 담는 트리 */
#include <linux/kref.h>	/* [한국어] iopt_pages 의 공유 참조 */
#include <linux/mutex.h>	/* [한국어] pages 를 지키는 락 */
#include <linux/xarray.h>	/* [한국어] 고정된 PFN 을 색인으로 담는다 */

#include "iommufd_private.h"	/* [한국어] iommufd 의 객체 모델 */

struct iommu_domain;	/* [한국어] 포인터로만 쓰므로 전방 선언으로 족하다 */

/*
 * Each io_pagetable is composed of intervals of areas which cover regions of
 * the iova that are backed by something. iova not covered by areas is not
 * populated in the page table. Each area is fully populated with pages.
 *
 * iovas are in byte units, but must be iopt->iova_alignment aligned.
 *
 * pages can be NULL, this means some other thread is still working on setting
 * up or tearing down the area. When observed under the write side of the
 * domain_rwsem a NULL pages must mean the area is still being setup and no
 * domains are filled.
 *
 * storage_domain points at an arbitrary iommu_domain that is holding the PFNs
 * for this area. It is locked by the pages->mutex. This simplifies the locking
 * as the pages code can rely on the storage_domain without having to get the
 * iopt->domains_rwsem.
 *
 * The io_pagetable::iova_rwsem protects node
 * The iopt_pages::mutex protects pages_node
 * iopt and iommu_prot are immutable
 * The pages::mutex protects num_accesses
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * IOVA 공간의 한 구간.
 *
 * 원 주석이 두 가지를 짚는다. pages 가 NULL 이면 다른 스레드가 아직
 * 세우거나 허무는 중이고, storage_domain 은 이 구간의 PFN 을 실제로 들고
 * 있는 아무 도메인 하나다 — 후자 덕분에 pages 쪽 코드가 도메인 목록 락을
 * 잡지 않고도 PFN 을 읽을 수 있다.
 */
struct iopt_area {
	struct interval_tree_node node;
	/* [한국어] IOVA 로 정렬된 트리의 노드.
	 * 설정자: 구간을 만들 때 시작·끝 IOVA 로 채운다.
	 * 읽는 자: 겹침 검사와 빈 IOVA 찾기.
	 * 값 범위: iopt->iova_alignment 에 정렬된 주소 범위.
	 * 동기화: io_pagetable::iova_rwsem 이 지킨다(파일 상단 원 주석). */
	struct interval_tree_node pages_node;
	/* [한국어] 페이지 색인으로 정렬된 트리의 노드.
	 * 설정자: 구간을 만들 때 pages 안에서의 색인 범위로 채운다.
	 * 읽는 자: pages.c 가 "이 페이지를 지금 쓰는 구간이 있는가"를 물을 때.
	 * 값 범위: 0 부터 pages->npages 사이.
	 * 동기화: iopt_pages::mutex 가 지킨다.
	 * 트리가 둘인 이유: 같은 구간을 IOVA 로도, 페이지 번호로도 찾아야 한다. */
	struct io_pagetable *iopt;
	/* [한국어] 이 구간이 속한 IOVA 공간.
	 * 설정자: 구간을 만들 때 한 번.
	 * 읽는 자: 정렬 규칙과 도메인 목록을 얻을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이라 락이 필요 없다(원 주석). */
	struct iopt_pages *pages;
	/* [한국어] 이 구간을 채우는 페이지 묶음.
	 * 설정자: 구간을 세울 때 붙이고, 허물 때 뗀다.
	 * 읽는 자: 채우기·걷어내기 경로 전반.
	 * 값 범위: 유효한 포인터 또는 NULL. 원 주석이 NULL 의 뜻을 밝힌다 —
	 *   다른 스레드가 아직 세우거나 허무는 중이며, 도메인 쓰기 락 아래에서
	 *   본 NULL 은 반드시 "세우는 중"이고 아직 어느 도메인도 채워지지 않았다.
	 * 동기화: iova_rwsem 과 domains_rwsem 의 조합. */
	struct iommu_domain *storage_domain;
	/* [한국어] 이 구간의 PFN 을 실제로 들고 있는 도메인 하나.
	 * 설정자: 첫 도메인에 채울 때 정하고, 그 도메인이 떠나면 다른 것으로 옮긴다.
	 * 읽는 자: pages.c 가 PFN 을 되읽을 때.
	 * 값 범위: 이 구간이 채워진 도메인 중 아무거나.
	 * 동기화: pages->mutex 가 지킨다. 원 주석이 그 이득을 밝힌다 — pages
	 *   쪽 코드가 iopt->domains_rwsem 을 잡지 않고도 PFN 에 닿을 수 있다. */
	/* How many bytes into the first page the area starts */
	unsigned int page_offset;
	/* [한국어] (원 주석: 첫 페이지에서 몇 바이트 들어간 곳에서 구간이 시작하는가)
	 * 설정자: 구간을 만들 때 사용자 VA 의 정렬에서 계산한다.
	 * 읽는 자: iopt_area_start_byte.
	 * 값 범위: 0 부터 PAGE_SIZE-1.
	 * 동기화: 불변이다. */
	/* IOMMU_READ, IOMMU_WRITE, etc */
	int iommu_prot;
	/* [한국어] (원 주석: IOMMU_READ, IOMMU_WRITE 등)
	 * 설정자: 사용자가 매핑을 요청할 때.
	 * 읽는 자: 도메인에 채울 때 iommu_map 에 그대로 넘긴다.
	 * 값 범위: IOMMU_* 권한 비트 조합.
	 * 동기화: 불변이다(원 주석). */
	bool prevent_access : 1;
	/* [한국어] 이 구간에 대한 새 접근을 막을 것인가.
	 * 설정자: 구간을 허물기 시작할 때 세운다.
	 * 읽는 자: iopt_area_add_access 가 거절 여부를 판단할 때.
	 * 값 범위: 0 또는 1.
	 * 동기화: iova_rwsem 쓰기 아래에서 바꾼다.
	 * 이 표시가 없으면 허무는 중인 구간에 새 접근이 들어와 경합한다. */
	unsigned int num_accesses;
	/* [한국어] 지금 이 구간을 보고 있는 커널 쪽 접근의 수.
	 * 설정자: iopt_area_add_access / remove_access.
	 * 읽는 자: 구간을 허물어도 되는지 판단할 때.
	 * 값 범위: 0 이상. 0 이 아니면 허물 수 없다.
	 * 동기화: pages::mutex 가 지킨다(원 주석). */
	unsigned int num_locks;
	/* [한국어] 그중 페이지 고정을 요구한 접근의 수.
	 * 설정자: add_access 에 lock_area 가 참으로 온 경우.
	 * 읽는 자: 고정을 풀어도 되는지 판단할 때.
	 * 값 범위: 0 부터 num_accesses 까지.
	 * 동기화: pages::mutex 가 지킨다. */
};

/*
 * [한국어] 사용자가 쓰겠다고 등록한 IOVA 범위.
 * 이 트리가 비어 있으면 예약된 곳을 뺀 전 범위를 쓸 수 있고, 항목이
 * 있으면 그 안에서만 자동 할당이 이루어진다.
 */
struct iopt_allowed {
	struct interval_tree_node node;
	/* [한국어] 허용된 IOVA 범위 하나.
	 * 설정자: 사용자의 IOMMU_IOAS_ALLOW_IOVAS 요청.
	 * 읽는 자: 자동 IOVA 할당이 후보를 고를 때.
	 * 값 범위: IOVA 주소 범위.
	 * 동기화: iova_rwsem 이 지킨다. */
};

/*
 * [한국어] 쓸 수 없는 IOVA 범위.
 * 장치가 요구한 예약 구간(MSI 창, 항등 매핑 등)이 여기 들어간다.
 * owner 로 누가 등록했는지 기억해 두어, 그 장치가 떠날 때 함께 걷어낸다.
 */
struct iopt_reserved {
	struct interval_tree_node node;
	/* [한국어] 쓸 수 없는 IOVA 범위 하나.
	 * 설정자: 장치를 붙일 때 그 장치의 예약 구간에서.
	 * 읽는 자: 할당과 매핑이 이 범위를 피한다.
	 * 값 범위: IOVA 주소 범위.
	 * 동기화: iova_rwsem 이 지킨다. */
	void *owner;
	/* [한국어] 이 예약을 등록한 주체.
	 * 설정자: 등록할 때.
	 * 읽는 자: 그 주체가 떠날 때 자기 예약만 골라 거둔다.
	 * 값 범위: 대개 장치나 access 객체의 포인터.
	 * 동기화: iova_rwsem 이 지킨다. */
};

int iopt_area_fill_domains(struct iopt_area *area, struct iopt_pages *pages);	/* [한국어] 이 구간을 모든 도메인에 채운다 */
void iopt_area_unfill_domains(struct iopt_area *area, struct iopt_pages *pages);	/* [한국어] 모든 도메인에서 걷어낸다 */

int iopt_area_fill_domain(struct iopt_area *area, struct iommu_domain *domain);	/* [한국어] 새 도메인 하나에만 채운다 */
void iopt_area_unfill_domain(struct iopt_area *area, struct iopt_pages *pages,	/* [한국어] 도메인 하나에서만 걷어내되 */
			     struct iommu_domain *domain);	/* [한국어] PFN 은 다른 도메인이 아직 쓸 수 있다 */
void iopt_area_unmap_domain(struct iopt_area *area,	/* [한국어] PFN 은 두고 매핑만 지운다 */
			    struct iommu_domain *domain);	/* [한국어] 도메인이 사라질 때 쓴다 */

int iopt_dmabuf_track_domain(struct iopt_pages *pages, struct iopt_area *area,	/* [한국어] 회수에 대비해 이 매핑을 기록해 둔다 */
			     struct iommu_domain *domain);
void iopt_dmabuf_untrack_domain(struct iopt_pages *pages,	/* [한국어] 그 기록을 지운다 */
				struct iopt_area *area,
				struct iommu_domain *domain);
int iopt_dmabuf_track_all_domains(struct iopt_area *area,	/* [한국어] 모든 도메인에 대해 한꺼번에 */
				  struct iopt_pages *pages);
void iopt_dmabuf_untrack_all_domains(struct iopt_area *area,	/* [한국어] 그 기록들을 모두 지운다 */
				     struct iopt_pages *pages);

/*
 * [한국어]
 * iopt_area_index - 이 구간이 pages 안에서 시작하는 페이지 번호
 *
 * @area: 볼 구간.
 * @return: 0 기반 페이지 색인.
 *
 * IOVA 좌표와 pages 좌표가 따로 있어, 이 함수 무리가 둘 사이를 오간다.
 */
static inline unsigned long iopt_area_index(struct iopt_area *area)
{
	return area->pages_node.start;	/* [한국어] pages 안에서의 시작 페이지 번호 */
}

/*
 * [한국어]
 * iopt_area_last_index - 이 구간이 pages 안에서 끝나는 페이지 번호(포함)
 *
 * @area: 볼 구간.
 * @return: 마지막 페이지 색인.
 */
static inline unsigned long iopt_area_last_index(struct iopt_area *area)
{
	return area->pages_node.last;	/* [한국어] pages 안에서의 마지막 페이지 번호(포함) */
}

/*
 * [한국어]
 * iopt_area_iova - 이 구간의 시작 IOVA
 *
 * @area: 볼 구간.
 * @return: IOVA.
 */
static inline unsigned long iopt_area_iova(struct iopt_area *area)
{
	return area->node.start;	/* [한국어] IOVA 공간에서의 시작 주소 */
}

/*
 * [한국어]
 * iopt_area_last_iova - 이 구간의 마지막 IOVA(포함)
 *
 * @area: 볼 구간.
 * @return: IOVA.
 *
 * 끝을 포함으로 두는 이유: 배타적 끝은 주소 공간의 마지막 바이트를 덮는
 * 구간에서 넘친다.
 */
static inline unsigned long iopt_area_last_iova(struct iopt_area *area)
{
	return area->node.last;	/* [한국어] IOVA 공간에서의 마지막 주소(포함) */
}

/*
 * [한국어]
 * iopt_area_length - 이 구간의 바이트 길이
 *
 * @area: 볼 구간.
 * @return: 길이.
 */
static inline size_t iopt_area_length(struct iopt_area *area)
{
	return (area->node.last - area->node.start) + 1;	/* [한국어] 끝이 포함이라 +1 */
}

/*
 * Number of bytes from the start of the iopt_pages that the iova begins.
 * iopt_area_start_byte() / PAGE_SIZE encodes the starting page index
 * iopt_area_start_byte() % PAGE_SIZE encodes the offset within that page
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iopt_area_start_byte - IOVA 를 pages 안의 바이트 오프셋으로 옮긴다
 *
 * @area: 그 IOVA 를 담는 구간.
 * @iova: 옮길 주소.
 * @return: pages 시작에서의 바이트 거리.
 *
 * 두 좌표계를 잇는 함수라 이 계층 전반에서 쓰인다. 원 주석이 결과의
 * 쓰임을 밝힌다 — PAGE_SIZE 로 나누면 페이지 번호, 나머지는 그 페이지
 * 안의 오프셋이다.
 *
 * 세 항을 더하는 이유: 구간 안에서의 거리, 첫 페이지에서 구간이 시작하는
 * 오프셋, 그리고 그 첫 페이지까지의 바이트다. 사용자 VA 가 페이지 정렬이
 * 아닐 수 있어 가운데 항이 필요하다.
 */
static inline unsigned long iopt_area_start_byte(struct iopt_area *area,
						 unsigned long iova)
{
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST))	/* [한국어] 시험 빌드에서만 */
		WARN_ON(iova < iopt_area_iova(area) ||	/* [한국어] 범위 밖의 주소를 물으면 */
			iova > iopt_area_last_iova(area));	/* [한국어] 계산이 무의미해진다 */
	return (iova - iopt_area_iova(area)) + area->page_offset +	/* [한국어] 구간 안에서의 거리에 첫 페이지의 오프셋을 더하고 */
	       iopt_area_index(area) * PAGE_SIZE;	/* [한국어] 그 구간이 시작하는 페이지까지의 바이트를 더한다 */
}

/*
 * [한국어]
 * iopt_area_iova_to_index - IOVA 를 pages 안의 페이지 번호로 옮긴다
 *
 * @area: 그 IOVA 를 담는 구간.
 * @iova: 옮길 주소.
 * @return: 페이지 색인.
 */
static inline unsigned long iopt_area_iova_to_index(struct iopt_area *area,
						    unsigned long iova)
{
	return iopt_area_start_byte(area, iova) / PAGE_SIZE;	/* [한국어] 바이트 오프셋을 페이지 번호로 */
}

#define __make_iopt_iter(name)                                                 \
	static inline struct iopt_##name *iopt_##name##_iter_first(            \
		struct io_pagetable *iopt, unsigned long start,                \
		unsigned long last)                                            \
	{                                                                      \
		struct interval_tree_node *node;                               \
									       \
		lockdep_assert_held(&iopt->iova_rwsem);                        \
		node = interval_tree_iter_first(&iopt->name##_itree, start,    \
						last);                         \
		if (!node)                                                     \
			return NULL;                                           \
		return container_of(node, struct iopt_##name, node);           \
	}                                                                      \
	static inline struct iopt_##name *iopt_##name##_iter_next(             \
		struct iopt_##name *last_node, unsigned long start,            \
		unsigned long last)                                            \
	{                                                                      \
		struct interval_tree_node *node;                               \
									       \
		node = interval_tree_iter_next(&last_node->node, start, last); \
		if (!node)                                                     \
			return NULL;                                           \
		return container_of(node, struct iopt_##name, node);           \
	}

__make_iopt_iter(area)	/* [한국어] area 트리 순회기 */
__make_iopt_iter(allowed)	/* [한국어] allowed 트리 순회기 */
__make_iopt_iter(reserved)	/* [한국어] reserved 트리 순회기 */

/*
 * [한국어] 여러 area 에 걸친 연속 범위를 훑는 순회 상태.
 * 사용자가 요청한 범위가 area 여러 개에 걸쳐 있을 수 있고, 중간에 구멍이
 * 있으면 거기서 멈춘다.
 */
struct iopt_area_contig_iter {
	unsigned long cur_iova;
	/* [한국어] 다음에 볼 IOVA.
	 * 설정자: 순회가 한 구간을 지날 때마다 그 끝 다음으로 옮긴다.
	 * 읽는 자: 다음 구간이 여기서 이어지는지 확인할 때.
	 * 값 범위: 요청 범위 안.
	 * 동기화: 호출 스택 값. 트리 락은 호출자가 쥔다. */
	unsigned long last_iova;
	/* [한국어] 요청 범위의 마지막 주소.
	 * 설정자: 순회를 시작할 때.
	 * 읽는 자: done() 이 끝까지 닿았는지 볼 때.
	 * 값 범위: cur_iova 이상.
	 * 동기화: 호출 스택 값. */
	struct iopt_area *area;
	/* [한국어] 지금 보고 있는 구간.
	 * 설정자: init/next.
	 * 읽는 자: 반복문 몸통.
	 * 값 범위: 유효한 구간 또는 NULL(구멍을 만났거나 끝).
	 * 동기화: 호출 스택 값. */
};
struct iopt_area *iopt_area_contig_init(struct iopt_area_contig_iter *iter,	/* [한국어] 연속 순회를 시작한다 */
					struct io_pagetable *iopt,
					unsigned long iova,
					unsigned long last_iova);
struct iopt_area *iopt_area_contig_next(struct iopt_area_contig_iter *iter);	/* [한국어] 다음 구간으로 — 구멍을 만나면 NULL */

/*
 * [한국어]
 * iopt_area_contig_done - 연속 순회가 요청 범위를 다 덮었는지 답한다
 *
 * @iter: 순회 상태.
 * @return: 끝까지 구멍 없이 닿았으면 참.
 *
 * 반복문이 끝난 뒤 이것을 확인해야 한다. 중간에 매핑되지 않은 구멍이
 * 있으면 반복문이 그냥 멈추므로, 정상 종료와 구별할 방법이 이것뿐이다.
 */
static inline bool iopt_area_contig_done(struct iopt_area_contig_iter *iter)
{
	return iter->area && iter->last_iova <= iopt_area_last_iova(iter->area);	/* [한국어] 요청 범위의 끝까지 구멍 없이 닿았는가 */
}

/*
 * Iterate over a contiguous list of areas that span the iova,last_iova range.
 * The caller must check iopt_area_contig_done() after the loop to see if
 * contiguous areas existed.
 */
#define iopt_for_each_contig_area(iter, area, iopt, iova, last_iova)          \
	for (area = iopt_area_contig_init(iter, iopt, iova, last_iova); area; \
	     area = iopt_area_contig_next(iter))	/* [한국어] 다음 구간으로 — 구멍을 만나면 NULL 이 되어 멈춘다 */

/*
 * [한국어] 고정된 페이지를 누구의 한도에 계상할 것인가.
 * NONE 은 계상하지 않고, USER 는 사용자의 잠금 메모리 한도에, MM 은
 * 프로세스의 pinned 통계에 넣는다. 출처에 따라 갈린다.
 */
enum {
	IOPT_PAGES_ACCOUNT_NONE = 0,	/* [한국어] 계상하지 않는다 */
	IOPT_PAGES_ACCOUNT_USER = 1,	/* [한국어] 사용자의 잠금 메모리 한도에 */
	IOPT_PAGES_ACCOUNT_MM = 2,	/* [한국어] 프로세스의 pinned 통계에 */
	IOPT_PAGES_ACCOUNT_MODE_NUM = 3,	/* [한국어] 모드의 개수 */
};

/*
 * [한국어] 페이지가 어디서 왔는가.
 * 사용자 VA, 파일(메모리 파일이나 hugetlbfs), dma-buf(다른 장치의 메모리).
 * 이 값이 아래 union 중 어느 필드를 읽을지 정한다.
 */
enum iopt_address_type {
	IOPT_ADDRESS_USER = 0,	/* [한국어] 사용자 VA 에서 온 페이지 */
	IOPT_ADDRESS_FILE,	/* [한국어] 파일에서 온 페이지(memfd, hugetlbfs 등) */
	IOPT_ADDRESS_DMABUF,	/* [한국어] 다른 장치의 메모리 — struct page 가 없을 수 있다 */
};

/*
 * [한국어] dma-buf 를 어느 도메인의 어느 area 에 매핑해 두었는지.
 * dma-buf 제공자가 메모리를 회수하면 그 매핑들을 모두 걷어야 하는데,
 * 그때 무엇을 걷어야 하는지 이 목록이 알려 준다.
 */
struct iopt_pages_dmabuf_track {
	struct iommu_domain *domain;
	/* [한국어] 이 dma-buf 가 매핑된 도메인.
	 * 설정자: 도메인에 채울 때 기록한다.
	 * 읽는 자: 회수 시 걷어낼 대상을 찾을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: pages::mutex 가 지킨다. */
	struct iopt_area *area;
	/* [한국어] 그 도메인의 어느 구간에 매핑됐는가.
	 * 설정자: 도메인에 채울 때.
	 * 읽는 자: 회수 시 그 IOVA 범위를 지울 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: pages::mutex 가 지킨다. */
	struct list_head elm;
	/* [한국어] 추적 목록의 연결 고리.
	 * 설정자: 기록과 삭제.
	 * 읽는 자: 회수 경로가 목록을 훑을 때.
	 * 값 범위: pages->dmabuf.tracker 에 매달린다.
	 * 동기화: pages::mutex 가 지킨다. */
};

/*
 * [한국어] dma-buf 출처의 페이지 정보.
 * 일반 페이지와 달리 struct page 가 없을 수 있어(장치 메모리), 물리
 * 주소 구간으로 직접 다룬다.
 */
struct iopt_pages_dmabuf {
	struct dma_buf_attachment *attach;
	/* [한국어] 이 dma-buf 에 붙은 상태.
	 * 설정자: iopt_alloc_dmabuf_pages.
	 * 읽는 자: 물리 주소를 얻고, 회수 알림을 받을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: pages::mutex 가 지킨다. */
	struct phys_vec phys;
	/* [한국어] 그 버퍼의 물리 주소 구간.
	 * 설정자: 붙일 때, 그리고 회수될 때 길이 0 으로.
	 * 읽는 자: 도메인에 채울 때. struct page 가 없을 수 있어 주소로 직접 다룬다.
	 * 값 범위: 길이가 0 이면 회수된 것이다.
	 * 동기화: pages::mutex 가 지킨다. */
	/* Always PAGE_SIZE aligned */
	unsigned long start;
	/* [한국어] (원 주석: 늘 PAGE_SIZE 에 정렬된다)
	 * 설정자: 만들 때.
	 * 읽는 자: 버퍼 안에서의 오프셋을 계산할 때.
	 * 값 범위: 페이지 정렬된 바이트 오프셋.
	 * 동기화: 불변이다. */
	struct list_head tracker;
	/* [한국어] 이 버퍼가 어디에 매핑됐는지의 목록.
	 * 설정자: track/untrack.
	 * 읽는 자: 회수 경로.
	 * 값 범위: iopt_pages_dmabuf_track 항목들.
	 * 동기화: pages::mutex 가 지킨다. */
};

/*
 * This holds a pinned page list for multiple areas of IO address space. The
 * pages always originate from a linear chunk of userspace VA. Multiple
 * io_pagetable's, through their iopt_area's, can share a single iopt_pages
 * which avoids multi-pinning and double accounting of page consumption.
 *
 * indexes in this structure are measured in PAGE_SIZE units, are 0 based from
 * the start of the uptr and extend to npages. pages are pinned dynamically
 * according to the intervals in the access_itree and domains_itree, npinned
 * records the current number of pages pinned.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 고정된 페이지들의 묶음.
 *
 * 원 주석이 이 구조체의 존재 이유를 밝힌다 — 여러 io_pagetable 이 각자의
 * area 를 통해 하나의 iopt_pages 를 공유해, 같은 메모리를 두 번 고정하거나
 * 두 번 계상하는 일을 막는다.
 *
 * 색인 체계도 원 주석에 있다: PAGE_SIZE 단위, 사용자 VA 의 시작에서 0 부터
 * npages 까지. 고정은 access_itree 와 domains_itree 가 말하는 구간에 대해서만
 * 게으르게 이루어지고, npinned 가 지금 몇 장이 고정되어 있는지를 센다.
 */
struct iopt_pages {
	struct kref kref;
	/* [한국어] 공유 참조 수.
	 * 설정자: 새 area 가 이 묶음을 쓰기 시작할 때 올린다.
	 * 읽는 자: 0 이 되면 iopt_release_pages 가 고정을 풀고 해제한다.
	 * 값 범위: 1 이상.
	 * 동기화: kref 자체가 원자적이다. */
	struct mutex mutex;
	/* [한국어] 이 묶음의 거의 모든 것을 지키는 락.
	 * 설정자/읽는 자: 고정, xarray 갱신, 도메인 채우기가 모두 이 락 아래.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 파일 상단 원 주석이 이 락이 무엇을 지키는지 열거한다 —
	 *   area 의 pages_node, num_accesses, storage_domain 이 여기 속한다. */
	size_t npages;
	/* [한국어] 이 묶음이 덮는 전체 페이지 수.
	 * 설정자: 만들 때 길이에서 계산한다.
	 * 읽는 자: 색인 범위 검사.
	 * 값 범위: 1 이상.
	 * 동기화: 불변이다. */
	size_t npinned;
	/* [한국어] 지금 실제로 고정되어 있는 페이지 수.
	 * 설정자: 고정과 해제 경로.
	 * 읽는 자: 사용자 한도에 반영할 때.
	 * 값 범위: 0 부터 npages 까지. 고정이 게을러 대개 npages 보다 작다.
	 * 동기화: mutex 가 지킨다. */
	size_t last_npinned;
	/* [한국어] 마지막으로 한도에 반영한 시점의 고정 수.
	 * 설정자: 한도를 갱신할 때.
	 * 읽는 자: 이번에 얼마나 늘거나 줄었는지 계산할 때.
	 * 값 범위: 0 이상.
	 * 동기화: mutex 가 지킨다.
	 * 차분만 반영하는 이유: 한도 갱신이 비싸 매번 전체를 다시 세지 않는다. */
	struct task_struct *source_task;
	/* [한국어] 이 메모리를 제공한 프로세스.
	 * 설정자: 만들 때 현재 태스크에서.
	 * 읽는 자: pinned 통계를 그 프로세스에 계상할 때.
	 * 값 범위: 참조를 잡아 둔 태스크, 또는 NULL(파일/dma-buf 출처).
	 * 동기화: 불변이다. */
	struct mm_struct *source_mm;
	/* [한국어] 그 프로세스의 주소 공간.
	 * 설정자: 만들 때.
	 * 읽는 자: 페이지를 고정할 때 어느 주소 공간에서 찾을지.
	 * 값 범위: 참조를 잡아 둔 mm, 또는 NULL.
	 * 동기화: 불변이다.
	 * mm 을 잡아 두는 이유: 요청한 프로세스가 죽어도 고정된 페이지가
	 *   살아 있어야 장치가 계속 쓸 수 있다. */
	struct user_struct *source_user;
	/* [한국어] 그 프로세스의 사용자.
	 * 설정자: 만들 때.
	 * 읽는 자: 잠금 메모리 한도를 그 사용자에게 계상할 때.
	 * 값 범위: 참조를 잡아 둔 사용자, 또는 NULL.
	 * 동기화: 불변이다. */
	enum iopt_address_type type;
	/* [한국어] 페이지의 출처.
	 * 설정자: 만들 때.
	 * 읽는 자: 아래 union 중 어느 필드를 읽을지, 그리고 고정 방식을 고를 때.
	 * 값 범위: USER / FILE / DMABUF.
	 * 동기화: 불변이다. */
	union {
		void __user *uptr;		/* IOPT_ADDRESS_USER */
		/* [한국어] (원 주석: IOPT_ADDRESS_USER) 사용자 VA 의 시작.
		 * 설정자: 만들 때.
		 * 읽는 자: pin_user_pages 계열이 여기서 찾는다.
		 * 값 범위: 사용자 주소.
		 * 동기화: 불변이다. */
		struct {			/* IOPT_ADDRESS_FILE */
			struct file *file;
			/* [한국어] (원 주석: IOPT_ADDRESS_FILE) 페이지를 담은 파일.
			 * 설정자: 만들 때 참조를 잡는다.
			 * 읽는 자: 페이지 캐시에서 페이지를 얻을 때.
			 * 값 범위: memfd 나 hugetlbfs 같은 파일.
			 * 동기화: 불변이다. */
			unsigned long start;
			/* [한국어] 그 파일 안에서의 시작 오프셋.
			 * 설정자: 만들 때.
			 * 읽는 자: 페이지 색인을 파일 오프셋으로 옮길 때.
			 * 값 범위: 페이지 정렬된 오프셋.
			 * 동기화: 불변이다. */
		};
		/* IOPT_ADDRESS_DMABUF */
		struct iopt_pages_dmabuf dmabuf;
		/* [한국어] (원 주석: IOPT_ADDRESS_DMABUF) dma-buf 출처의 정보.
		 * 설정자: 만들 때 붙이고, 회수 시 길이가 0 이 된다.
		 * 읽는 자: 도메인에 채울 때와 회수 처리.
		 * 값 범위: 위 구조체 참고.
		 * 동기화: mutex 가 지킨다 — 다른 union 항목과 달리 바뀔 수 있다. */
	};
	bool writable:1;
	/* [한국어] 이 매핑에 쓰기를 허용하는가.
	 * 설정자: 만들 때 사용자 요청에서.
	 * 읽는 자: 페이지를 고정할 때 쓰기 권한으로 잡을지 정한다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
	u8 account_mode;
	/* [한국어] 고정된 페이지를 누구의 한도에 계상하는가.
	 * 설정자: 만들 때 출처에 따라.
	 * 읽는 자: iopt_pages_update_pinned.
	 * 값 범위: IOPT_PAGES_ACCOUNT_* 중 하나.
	 * 동기화: 불변이다. */

	struct xarray pinned_pfns;
	/* [한국어] 고정된 PFN 을 페이지 색인으로 담는 배열.
	 * 설정자: 고정할 때 넣고 풀 때 뺀다.
	 * 읽는 자: 도메인에 채우거나 커널이 접근할 때.
	 * 값 범위: 색인 → PFN. 고정이 게을러 구멍이 있다.
	 * 동기화: mutex 가 지킨다. */
	/* Of iopt_pages_access::node */
	struct rb_root_cached access_itree;
	/* [한국어] (원 주석: iopt_pages_access::node 들의 트리)
	 * 설정자: 커널 접근이 등록·해제될 때.
	 * 읽는 자: 어느 페이지를 고정한 채 두어야 하는지 판단할 때.
	 * 값 범위: 구간 트리.
	 * 동기화: mutex 가 지킨다. */
	/* Of iopt_area::pages_node */
	struct rb_root_cached domains_itree;
	/* [한국어] (원 주석: iopt_area::pages_node 들의 트리)
	 * 설정자: 구간이 도메인에 채워지고 걷힐 때.
	 * 읽는 자: 위와 같은 판단, 그리고 storage_domain 을 고를 때.
	 * 값 범위: 구간 트리.
	 * 동기화: mutex 가 지킨다.
	 * 두 트리의 합집합이 "지금 고정되어 있어야 할 페이지"다 — double_span.h
	 *   의 두 트리 순회기가 바로 이 둘을 위해 있다. */
};

/*
 * [한국어]
 * iopt_is_dmabuf - 이 페이지 묶음이 dma-buf 에서 왔는지 답한다
 *
 * @pages: 볼 묶음.
 * @return: dma-buf 출처면 참.
 *
 * dma-buf 페이지는 struct page 가 없을 수 있고 제공자가 회수해 갈 수도
 * 있어, 일반 페이지와 다루는 방식이 다르다.
 */
static inline bool iopt_is_dmabuf(struct iopt_pages *pages)
{
	if (!IS_ENABLED(CONFIG_DMA_SHARED_BUFFER))	/* [한국어] 그 기능을 끈 커널이면 */
		return false;	/* [한국어] 상수 거짓 — 관련 코드가 통째로 사라진다 */
	return pages->type == IOPT_ADDRESS_DMABUF;	/* [한국어] 출처가 dma-buf 인가 */
}

/*
 * [한국어]
 * iopt_dmabuf_revoked - dma-buf 제공자가 메모리를 회수했는지 답한다
 *
 * @pages: 볼 묶음.
 * @return: 회수됐으면 참.
 *
 * 회수는 언제든 일어날 수 있고, 그 뒤로는 그 물리 주소를 매핑하면 안 된다.
 * 길이를 0 으로 만드는 것이 회수의 표시다.
 *
 * 락을 요구하는 이유: 확인과 그에 따른 처리 사이에 회수가 끼어들면
 * 사라진 메모리를 매핑하게 된다.
 */
static inline bool iopt_dmabuf_revoked(struct iopt_pages *pages)
{
	lockdep_assert_held(&pages->mutex);	/* [한국어] 길이 확인과 그에 따른 처리가 원자적이어야 한다 */
	if (iopt_is_dmabuf(pages))	/* [한국어] dma-buf 출처면 */
		return pages->dmabuf.phys.len == 0;	/* [한국어] 제공자가 메모리를 회수하면 길이가 0 이 된다 */
	return false;	/* [한국어] 그 밖의 출처는 회수되지 않는다 */
}

struct iopt_pages *iopt_alloc_user_pages(void __user *uptr,	/* [한국어] 사용자 VA 출처의 묶음을 만든다 */
					 unsigned long length, bool writable);
struct iopt_pages *iopt_alloc_file_pages(struct file *file,	/* [한국어] 파일 출처의 묶음을 만든다 */
					 unsigned long start_byte,
					 unsigned long start,
					 unsigned long length, bool writable);
struct iopt_pages *iopt_alloc_dmabuf_pages(struct iommufd_ctx *ictx,	/* [한국어] dma-buf 출처의 묶음을 만든다 */
					   struct dma_buf *dmabuf,
					   unsigned long start_byte,
					   unsigned long start,
					   unsigned long length, bool writable);
void iopt_release_pages(struct kref *kref);	/* [한국어] 마지막 참조가 사라졌을 때의 정리 */
/*
 * [한국어]
 * iopt_put_pages - 페이지 묶음의 참조를 하나 놓는다
 *
 * @pages: 놓을 묶음.
 *
 * 여러 area 가 하나의 묶음을 공유하므로 참조 계수로 수명을 정한다.
 * 마지막 참조가 사라지면 고정이 풀리고 계상도 되돌아간다.
 */
static inline void iopt_put_pages(struct iopt_pages *pages)
{
	kref_put(&pages->kref, iopt_release_pages);	/* [한국어] 마지막 참조가 사라지면 고정을 풀고 해제한다 */
}

void iopt_pages_fill_from_xarray(struct iopt_pages *pages, unsigned long start,	/* [한국어] 이미 고정된 PFN 을 배열로 꺼낸다 */
				 unsigned long last, struct page **out_pages);
int iopt_pages_fill_xarray(struct iopt_pages *pages, unsigned long start,	/* [한국어] 필요하면 고정하며 PFN 을 채운다 */
			   unsigned long last, struct page **out_pages);
void iopt_pages_unfill_xarray(struct iopt_pages *pages, unsigned long start,	/* [한국어] 아무도 쓰지 않게 된 구간의 고정을 푼다 */
			      unsigned long last);

int iopt_area_add_access(struct iopt_area *area, unsigned long start,	/* [한국어] 커널 쪽 접근을 등록하고 PFN 을 고정한다 */
			 unsigned long last, struct page **out_pages,
			 unsigned int flags, bool lock_area);
void iopt_area_remove_access(struct iopt_area *area, unsigned long start,	/* [한국어] 그 접근을 거둔다 */
			     unsigned long last, bool unlock_area);
int iopt_pages_rw_access(struct iopt_pages *pages, unsigned long start_byte,	/* [한국어] 고정 없이 읽고 쓰는 짧은 경로 */
			 void *data, unsigned long length, unsigned int flags);

/*
 * Each interval represents an active iopt_access_pages(), it acts as an
 * interval lock that keeps the PFNs pinned and stored in the xarray.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 커널 쪽 접근 하나를 나타내는 구간.
 *
 * 원 주석이 그 성격을 짚는다 — 이것은 구간 락처럼 동작해, 살아 있는 동안
 * 그 범위의 PFN 이 고정된 채 xarray 에 남아 있게 한다. 사용자 공간이
 * 그사이 unmap 해도 커널이 보던 페이지가 사라지지 않는다.
 */
struct iopt_pages_access {
	struct interval_tree_node node;
	/* [한국어] 이 접근이 붙잡고 있는 페이지 범위.
	 * 설정자: iopt_area_add_access.
	 * 읽는 자: 고정을 풀어도 되는지 볼 때 — 겹치는 접근이 있으면 못 푼다.
	 * 값 범위: pages 안의 페이지 색인 범위.
	 * 동기화: pages::mutex 가 지킨다. */
	unsigned int users;
	/* [한국어] 같은 범위를 붙잡은 접근의 수.
	 * 설정자: add_access 가 겹치는 항목을 찾으면 올리고, remove 가 내린다.
	 * 읽는 자: 0 이 되면 항목을 지우고 고정을 풀 수 있다.
	 * 값 범위: 1 이상(0 이 되면 삭제된다).
	 * 동기화: pages::mutex 가 지킨다. */
};

struct pfn_reader_user;	/* [한국어] pages.c 안에서만 쓰이는 타입 */

int iopt_pages_update_pinned(struct iopt_pages *pages, unsigned long npages,	/* [한국어] 고정된 장수를 한도에 반영한다 */
			     bool inc, struct pfn_reader_user *user);

#endif	/* [한국어] 포함 방지 끝 */
