// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, Oracle and/or its affiliates.
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */
/*
 * [한국어 설명] 사용자 공간 더티 비트맵을 창 단위로 다루는 계층 (iova_bitmap.c)
 *
 * === 파일의 역할 ===
 * 라이브 마이그레이션에서 "어느 페이지가 바뀌었는가"를 사용자 공간에
 * 알려 주려면 커널이 사용자 메모리의 비트맵에 비트를 세워야 한다. 그
 * 비트맵이 매우 클 수 있다는 것이 문제다.
 *
 * 파일 상단의 원 주석이 규모를 든다 — 페이지 포인터를 담을 기반 페이지
 * 하나(4KB)에 512개가 들어가고, 그것이 2MB 의 비트맵 데이터를 고정한다.
 * 비트 하나가 4KB 를 나타내면 그 한 창이 64GB 의 IOVA 를 덮는다.
 *
 * 그래서 이 계층은 창(window) 방식을 쓴다. 비트맵의 일부만 고정해 두고,
 * 요청한 IOVA 가 그 창을 벗어나면 옛 창을 풀고 새 창을 고정한다. 사용자는
 * iova_bitmap_set() 만 부르면 되고 그 안쪽 사정을 몰라도 된다.
 *
 * 비트와 IOVA 의 대응도 원 주석에 수식으로 있다 — 비트맵은 u64 배열이고
 * 각 비트가 (1 << pgshift) 크기의 IOVA 를 나타낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 IOMMU_HWPT_GET_DIRTY_BITMAP → io_pagetable.c 의 더티 경로 →
 *   [이 파일] → 드라이버의 read_and_clear_dirty → 하드웨어 더티 비트
 *
 * 실행 컨텍스트: 프로세스 문맥. 사용자 페이지를 고정하므로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommufd/io_pagetable.c, drivers/vfio 의 더티 추적.
 * 아래: pin_user_pages_fast, kmap_local_page.
 *
 * === 주요 함수/구조체 요약 ===
 * struct iova_bitmap: 전체 비트맵의 순회 상태. 어느 u64 워드까지 왔는지를
 *   색인으로 들고 있다.
 * struct iova_bitmap_map: 지금 고정되어 있는 창.
 * iova_bitmap_get / put: 창을 고정하고 푼다.
 * iova_bitmap_advance_to: 요청한 IOVA 가 든 창으로 옮긴다.
 * iova_bitmap_set: 그 범위의 비트를 세운다. 창을 넘어가면 옮겨 가며
 *   나누어 처리한다.
 */
#include <linux/highmem.h>	/* [한국어] kmap_local_page — 고정된 페이지를 잠시 매핑한다 */
#include <linux/iova_bitmap.h>	/* [한국어] 이 계층의 공개 선언 */
#include <linux/mm.h>	/* [한국어] 사용자 페이지 고정 */
#include <linux/slab.h>	/* [한국어] 객체 할당 */

/* [한국어] 페이지 하나가 담는 비트 수 — 창 안에서 페이지를 넘나들 때 쓴다 */
#define BITS_PER_PAGE (PAGE_SIZE * BITS_PER_BYTE)

/*
 * struct iova_bitmap_map - A bitmap representing an IOVA range
 *
 * Main data structure for tracking mapped user pages of bitmap data.
 *
 * For example, for something recording dirty IOVAs, it will be provided a
 * struct iova_bitmap structure, as a general structure for iterating the
 * total IOVA range. The struct iova_bitmap_map, though, represents the
 * subset of said IOVA space that is pinned by its parent structure (struct
 * iova_bitmap).
 *
 * The user does not need to exact location of the bits in the bitmap.
 * From user perspective the only API available is iova_bitmap_set() which
 * records the IOVA *range* in the bitmap by setting the corresponding
 * bits.
 *
 * The bitmap is an array of u64 whereas each bit represents an IOVA of
 * range of (1 << pgshift). Thus formula for the bitmap data to be set is:
 *
 *   data[(iova / page_size) / 64] & (1ULL << (iova % 64))
 */
struct iova_bitmap_map {
	/* base IOVA representing bit 0 of the first page */
	unsigned long iova;
	/* [한국어] (원 주석: 첫 페이지의 0번 비트가 나타내는 기준 IOVA)
	 * 설정자: iova_bitmap_get.
	 * 읽는 자: 비트 번호를 계산할 때.
	 * 값 범위: 전체 범위 안의 절대 주소.
	 * 동기화: 호출자가 직렬화한다. */

	/* mapped length */
	unsigned long length;
	/* [한국어] (원 주석: 이 창이 덮는 길이)
	 * 설정자: iova_bitmap_get.
	 * 읽는 자: 요청이 창 안인지 볼 때.
	 * 값 범위: 고정된 페이지와 전체 범위 중 작은 쪽.
	 * 동기화: 호출자가 직렬화한다. */

	/* page size order that each bit granules to */
	unsigned long pgshift;
	/* [한국어] (원 주석: 비트 하나가 나타내는 크기의 지수)
	 * 설정자: iova_bitmap_alloc 이 사용자가 준 크기에서.
	 * 읽는 자: 주소와 비트 번호 사이의 모든 변환.
	 * 값 범위: 페이지 크기의 지수.
	 * 동기화: 불변이다. */

	/* page offset of the first user page pinned */
	unsigned long pgoff;
	/* [한국어] (원 주석: 고정한 첫 사용자 페이지 안에서의 오프셋)
	 * 설정자: iova_bitmap_get.
	 * 읽는 자: 비트 번호를 계산할 때 그만큼 민다.
	 * 값 범위: 0 부터 PAGE_SIZE-1.
	 * 동기화: 호출자가 직렬화한다.
	 * 비트맵 주소가 페이지 정렬이 아닐 수 있어 필요하다. */

	/* number of pages pinned */
	unsigned long npages;
	/* [한국어] (원 주석: 고정한 페이지 수)
	 * 설정자: iova_bitmap_get 과 put.
	 * 읽는 자: 창의 끝을 넘었는지 판단할 때.
	 * 값 범위: 0 이면 고정된 것이 없다.
	 * 동기화: 호출자가 직렬화한다. */

	/* pinned pages representing the bitmap data */
	struct page **pages;
	/* [한국어] (원 주석: 비트맵 데이터를 담은 고정된 페이지들)
	 * 설정자: iova_bitmap_alloc 이 배열을 잡고 get 이 채운다.
	 * 읽는 자: 비트를 세울 때 하나씩 kmap 한다.
	 * 값 범위: 기반 페이지 하나에 담기는 만큼.
	 * 동기화: 호출자가 직렬화한다. */
};

/*
 * struct iova_bitmap - The IOVA bitmap object
 *
 * Main data structure for iterating over the bitmap data.
 *
 * Abstracts the pinning work and iterates in IOVA ranges.
 * It uses a windowing scheme and pins the bitmap in relatively
 * big ranges e.g.
 *
 * The bitmap object uses one base page to store all the pinned pages
 * pointers related to the bitmap. For sizeof(struct page*) == 8 it stores
 * 512 struct page pointers which, if the base page size is 4K, it means
 * 2M of bitmap data is pinned at a time. If the iova_bitmap page size is
 * also 4K then the range window to iterate is 64G.
 *
 * For example iterating on a total IOVA range of 4G..128G, it will walk
 * through this set of ranges:
 *
 *    4G  -  68G-1 (64G)
 *    68G - 128G-1 (64G)
 *
 * An example of the APIs on how to use/iterate over the IOVA bitmap:
 *
 *   bitmap = iova_bitmap_alloc(iova, length, page_size, data);
 *   if (IS_ERR(bitmap))
 *       return PTR_ERR(bitmap);
 *
 *   ret = iova_bitmap_for_each(bitmap, arg, dirty_reporter_fn);
 *
 *   iova_bitmap_free(bitmap);
 *
 * Each iteration of the @dirty_reporter_fn is called with a unique @iova
 * and @length argument, indicating the current range available through the
 * iova_bitmap. The @dirty_reporter_fn uses iova_bitmap_set() to mark dirty
 * areas (@iova_length) within that provided range, as following:
 *
 *   iova_bitmap_set(bitmap, iova, iova_length);
 *
 * The internals of the object uses an index @mapped_base_index that indexes
 * which u64 word of the bitmap is mapped, up to @mapped_total_index.
 * Those keep being incremented until @mapped_total_index is reached while
 * mapping up to PAGE_SIZE / sizeof(struct page*) maximum of pages.
 *
 * The IOVA bitmap is usually located on what tracks DMA mapped ranges or
 * some form of IOVA range tracking that co-relates to the user passed
 * bitmap.
 */
struct iova_bitmap {	/* [한국어] 전체 비트맵을 창 단위로 훑는 순회 상태 */
	/* IOVA range representing the currently mapped bitmap data */
	struct iova_bitmap_map mapped;
	/* [한국어] (원 주석: 지금 고정된 비트맵 데이터가 나타내는 IOVA 범위)
	 * 설정자: get/put.
	 * 읽는 자: 비트를 세우는 모든 계산.
	 * 값 범위: 위 구조체 참고.
	 * 동기화: 호출자가 직렬화한다. */

	/* userspace address of the bitmap */
	u8 __user *bitmap;
	/* [한국어] (원 주석: 사용자 공간의 비트맵 주소)
	 * 설정자: iova_bitmap_alloc.
	 * 읽는 자: 창을 고정할 주소를 계산할 때.
	 * 값 범위: 사용자 주소.
	 * 동기화: 불변이다. */

	/* u64 index that @mapped points to */
	unsigned long mapped_base_index;
	/* [한국어] (원 주석: mapped 가 가리키는 u64 색인)
	 * 설정자: 창을 옮길 때.
	 * 읽는 자: 창의 기준 IOVA 와 고정할 주소를 구할 때.
	 * 값 범위: 0 부터 mapped_total_index 까지.
	 * 동기화: 호출자가 직렬화한다. */

	/* how many u64 can we walk in total */
	unsigned long mapped_total_index;
	/* [한국어] (원 주석: 전체로 몇 개의 u64 를 걸을 수 있는가)
	 * 설정자: iova_bitmap_alloc.
	 * 읽는 자: 남은 워드 수와 범위 검사.
	 * 값 범위: 전체 길이에서 계산한 값.
	 * 동기화: 불변이다. */

	/* base IOVA of the whole bitmap */
	unsigned long iova;
	/* [한국어] (원 주석: 전체 비트맵의 기준 IOVA)
	 * 설정자: iova_bitmap_alloc.
	 * 읽는 자: 절대 주소와 상대 주소를 오갈 때.
	 * 값 범위: 사용자가 준 시작 주소.
	 * 동기화: 불변이다. */

	/* length of the IOVA range for the whole bitmap */
	size_t length;
	/* [한국어] (원 주석: 전체 비트맵이 덮는 IOVA 길이)
	 * 설정자: iova_bitmap_alloc.
	 * 읽는 자: 마지막 창의 길이를 자를 때.
	 * 값 범위: 사용자가 준 길이.
	 * 동기화: 불변이다. */
};

/*
 * Converts a relative IOVA to a bitmap index.
 * This function provides the index into the u64 array (bitmap::bitmap)
 * for a given IOVA offset.
 * Relative IOVA means relative to the bitmap::mapped base IOVA
 * (stored in mapped::iova). All computations in this file are done using
 * relative IOVAs and thus avoid an extra subtraction against mapped::iova.
 * The user API iova_bitmap_set() always uses a regular absolute IOVAs.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_offset_to_index - 상대 IOVA 를 u64 배열의 색인으로 옮긴다
 *
 * @bitmap: 대상 비트맵.
 * @iova: 창의 기준에서의 상대 주소.
 * @return: 그 주소가 든 u64 워드의 색인.
 *
 * 원 주석이 "상대"의 뜻을 밝힌다 — 이 파일의 모든 계산이 창 기준의 상대
 * 주소로 이루어져 뺄셈을 한 번 줄인다. 사용자 API 인 iova_bitmap_set()
 * 만 절대 주소를 받는다.
 */
static unsigned long iova_bitmap_offset_to_index(struct iova_bitmap *bitmap,
						 unsigned long iova)
{
	return (iova >> bitmap->mapped.pgshift) /	/* [한국어] 비트 하나가 덮는 크기로 나누어 비트 번호를 얻고 */
	       BITS_PER_TYPE(*bitmap->bitmap);	/* [한국어] 그것을 워드 크기로 나눈다 */
}

/*
 * Converts a bitmap index to a *relative* IOVA.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_index_to_offset - u64 색인을 상대 IOVA 로 되돌린다
 *
 * @bitmap: 대상 비트맵.
 * @index: u64 워드의 색인.
 * @return: 그 워드가 시작하는 상대 주소.
 */
static unsigned long iova_bitmap_index_to_offset(struct iova_bitmap *bitmap,
						 unsigned long index)
{
	unsigned long pgshift = bitmap->mapped.pgshift;	/* [한국어] 비트 하나가 덮는 크기 */

	return (index * BITS_PER_TYPE(*bitmap->bitmap)) << pgshift;	/* [한국어] 워드 색인을 비트 번호로, 다시 주소로 */
}

/*
 * Returns the base IOVA of the mapped range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_mapped_iova - 지금 고정된 창의 시작 IOVA
 *
 * @bitmap: 대상 비트맵.
 * @return: 절대 IOVA.
 */
static unsigned long iova_bitmap_mapped_iova(struct iova_bitmap *bitmap)
{
	unsigned long skip = bitmap->mapped_base_index;	/* [한국어] 지금까지 지나온 워드 수 */

	return bitmap->iova + iova_bitmap_index_to_offset(bitmap, skip);	/* [한국어] 전체 시작에서 그만큼 떨어진 곳 */
}

static unsigned long iova_bitmap_mapped_length(struct iova_bitmap *bitmap);	/* [한국어] iova_bitmap_get 이 먼저 쓰므로 전방 선언한다 */

/*
 * Pins the bitmap user pages for the current range window.
 * This is internal to IOVA bitmap and called when advancing the
 * index (@mapped_base_index) or allocating the bitmap.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_get - 현재 창의 사용자 페이지를 고정한다
 *
 * @bitmap: 대상 비트맵.
 * @return: 0 성공, -EFAULT 면 사용자 버퍼가 잘못됐다.
 *
 * 원 주석이 창 크기의 상한을 밝힌다 — 페이지 포인터 배열이 기반 페이지
 * 하나에 담겨야 하므로, x86 에서는 한 번에 2MB 의 비트맵까지다.
 *
 * 페이지 정렬 처리도 눈에 띈다. 비트맵 주소가 페이지 경계가 아닐 수 있어
 * 한 페이지를 더 잡아야 하고, 그 오프셋을 pgoff 에 기억해 둔다.
 *
 * FOLL_WRITE 로 고정하는 이유: 커널이 그 페이지에 비트를 쓰기 때문이다.
 */
static int iova_bitmap_get(struct iova_bitmap *bitmap)
{
	struct iova_bitmap_map *mapped = &bitmap->mapped;	/* [한국어] 채울 창 */
	unsigned long npages;	/* [한국어] 고정할 페이지 수 */
	u8 __user *addr;	/* [한국어] 고정할 사용자 주소 */
	long ret;	/* [한국어] 고정 결과 */

	/*
	 * @mapped_base_index is the index of the currently mapped u64 words
	 * that we have access. Anything before @mapped_base_index is not
	 * mapped. The range @mapped_base_index .. @mapped_total_index-1 is
	 * mapped but capped at a maximum number of pages.
	 */
	npages = DIV_ROUND_UP((bitmap->mapped_total_index -	/* [한국어] (원 주석: base_index 앞은 매핑되지 않았고, base 부터 total-1 까지가 페이지 수 상한 안에서 매핑된다) */
			       bitmap->mapped_base_index) *	/* [한국어] 남은 워드 수에 */
			       sizeof(*bitmap->bitmap), PAGE_SIZE);	/* [한국어] 워드 크기를 곱해 페이지 수로 */

	/*
	 * Bitmap address to be pinned is calculated via pointer arithmetic
	 * with bitmap u64 word index.
	 */
	addr = bitmap->bitmap + bitmap->mapped_base_index;	/* [한국어] (원 주석: 워드 색인으로 포인터 산술을 해 고정할 주소를 구한다) */

	/*
	 * We always cap at max number of 'struct page' a base page can fit.
	 * This is, for example, on x86 means 2M of bitmap data max.
	 */
	npages = min(npages + !!offset_in_page(addr),	/* [한국어] (원 주석: 기반 페이지 하나에 담기는 struct page 수로 늘 상한을 둔다) */
		     PAGE_SIZE / sizeof(struct page *));	/* [한국어] x86 에서는 비트맵 2MB 가 한 창이다 */

	ret = pin_user_pages_fast((unsigned long)addr, npages,	/* [한국어] 커널이 비트를 쓰므로 */
				  FOLL_WRITE, mapped->pages);	/* [한국어] 쓰기 가능하게 고정한다 */
	if (ret <= 0)	/* [한국어] 하나도 못 잡았으면 */
		return -EFAULT;	/* [한국어] 사용자 버퍼가 잘못됐다 */

	mapped->npages = (unsigned long)ret;	/* [한국어] 실제로 잡힌 수 */
	/* Base IOVA where @pages point to i.e. bit 0 of the first page */
	mapped->iova = iova_bitmap_mapped_iova(bitmap);	/* [한국어] (원 주석: 첫 페이지의 0번 비트가 가리키는 기준 IOVA) */

	/*
	 * offset of the page where pinned pages bit 0 is located.
	 * This handles the case where the bitmap is not PAGE_SIZE
	 * aligned.
	 */
	mapped->pgoff = offset_in_page(addr);	/* [한국어] (원 주석: 비트맵이 페이지 정렬이 아닐 때를 위한 오프셋) */
	mapped->length = iova_bitmap_mapped_length(bitmap);	/* [한국어] 이 창이 덮는 IOVA 길이 */
	return 0;	/* [한국어] 창이 준비됐다 */
}

/*
 * Unpins the bitmap user pages and clears @npages
 * (un)pinning is abstracted from API user and it's done when advancing
 * the index or freeing the bitmap.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_put - 창의 고정을 푼다
 *
 * @bitmap: 대상 비트맵.
 *
 * 원 주석대로 (고정과) 해제는 API 사용자에게 감춰져 있고, 창을 옮기거나
 * 비트맵을 해제할 때 저절로 일어난다.
 */
static void iova_bitmap_put(struct iova_bitmap *bitmap)
{
	struct iova_bitmap_map *mapped = &bitmap->mapped;	/* [한국어] 풀 창 */

	if (mapped->npages) {	/* [한국어] 고정된 것이 있으면 */
		unpin_user_pages(mapped->pages, mapped->npages);	/* [한국어] 모두 풀고 */
		mapped->npages = 0;	/* [한국어] 두 번 풀지 않도록 비운다 */
	}
}

/**
 * iova_bitmap_alloc() - Allocates an IOVA bitmap object
 * @iova: Start address of the IOVA range
 * @length: Length of the IOVA range
 * @page_size: Page size of the IOVA bitmap. It defines what each bit
 *             granularity represents
 * @data: Userspace address of the bitmap
 *
 * Allocates an IOVA object and initializes all its fields including the
 * first user pages of @data.
 *
 * Return: A pointer to a newly allocated struct iova_bitmap
 * or ERR_PTR() on error.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iova_bitmap_alloc - 비트맵 순회 객체를 만든다
 *
 * @iova: 다룰 IOVA 범위의 시작.
 * @length: 그 길이.
 * @page_size: 비트 하나가 나타내는 크기.
 * @data: 사용자 비트맵의 주소.
 * @return: 새 객체, 실패하면 ERR_PTR.
 *
 * 페이지 포인터를 담을 기반 페이지 하나를 잡는다. 그 크기가 창의 상한을
 * 정한다.
 */
struct iova_bitmap *iova_bitmap_alloc(unsigned long iova, size_t length,
				      unsigned long page_size, u64 __user *data)
{
	struct iova_bitmap_map *mapped;	/* [한국어] 창 */
	struct iova_bitmap *bitmap;	/* [한국어] 만들 객체 */
	int rc;	/* [한국어] 결과 */

	bitmap = kzalloc_obj(*bitmap);	/* [한국어] 순회 상태 */
	if (!bitmap)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 호출자에게 */

	mapped = &bitmap->mapped;	/* [한국어] 그 안의 창 */
	mapped->pgshift = __ffs(page_size);	/* [한국어] 비트 하나가 덮는 크기의 지수 */
	bitmap->bitmap = (u8 __user *)data;	/* [한국어] 사용자 비트맵의 주소 */
	bitmap->mapped_total_index =	/* [한국어] 전체가 몇 워드인가 */
		iova_bitmap_offset_to_index(bitmap, length - 1) + 1;	/* [한국어] 마지막 주소가 든 워드까지 */
	bitmap->iova = iova;	/* [한국어] 다룰 범위의 시작 */
	bitmap->length = length;	/* [한국어] 그 길이 */
	mapped->iova = iova;	/* [한국어] 첫 창도 같은 자리에서 */
	mapped->pages = (struct page **)__get_free_page(GFP_KERNEL);	/* [한국어] 이 페이지의 크기가 창의 상한을 정한다 */
	if (!mapped->pages) {	/* [한국어] 메모리 부족 */
		rc = -ENOMEM;	/* [한국어] 오류를 전하되 */
		goto err;	/* [한국어] 만든 것을 버려야 한다 */
	}

	return bitmap;	/* [한국어] 준비됐다 */

err:	/* [한국어] 부분 초기화된 객체를 정리한다 */
	iova_bitmap_free(bitmap);	/* [한국어] 부분 초기화된 객체를 정리한다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}
EXPORT_SYMBOL_NS_GPL(iova_bitmap_alloc, "IOMMUFD");	/* [한국어] VFIO 등이 더티 비트맵을 다룰 때 쓴다 */

/**
 * iova_bitmap_free() - Frees an IOVA bitmap object
 * @bitmap: IOVA bitmap to free
 *
 * It unpins and releases pages array memory and clears any leftover
 * state.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iova_bitmap_free - 비트맵 순회 객체를 해제한다
 *
 * @bitmap: 해제할 객체.
 *
 * 고정을 먼저 풀고 페이지 포인터 배열을 돌려준다.
 */
void iova_bitmap_free(struct iova_bitmap *bitmap)
{
	struct iova_bitmap_map *mapped = &bitmap->mapped;	/* [한국어] 창 */

	iova_bitmap_put(bitmap);	/* [한국어] 고정을 먼저 풀고 */

	if (mapped->pages) {	/* [한국어] 페이지 포인터 배열이 있으면 */
		free_page((unsigned long)mapped->pages);	/* [한국어] 그 페이지를 돌려주고 */
		mapped->pages = NULL;	/* [한국어] 두 번 놓지 않도록 */
	}

	kfree(bitmap);	/* [한국어] 객체 자체 */
}
EXPORT_SYMBOL_NS_GPL(iova_bitmap_free, "IOMMUFD");	/* [한국어] 그 해제 */

/*
 * Returns the remaining bitmap indexes from mapped_total_index to process for
 * the currently pinned bitmap pages.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_mapped_remaining - 지금 창으로 처리할 수 있는 워드 수
 *
 * @bitmap: 대상 비트맵.
 * @return: 남은 u64 워드 수.
 *
 * 두 한계 중 작은 쪽이다 — 전체에서 아직 안 본 워드 수와, 지금 고정된
 * 페이지들이 담는 워드 수.
 */
static unsigned long iova_bitmap_mapped_remaining(struct iova_bitmap *bitmap)
{
	unsigned long remaining, bytes;	/* [한국어] 남은 워드 수와 고정된 바이트 */

	bytes = (bitmap->mapped.npages << PAGE_SHIFT) - bitmap->mapped.pgoff;	/* [한국어] 고정된 페이지들이 담는 실제 바이트 — 첫 오프셋만큼 줄어든다 */

	remaining = bitmap->mapped_total_index - bitmap->mapped_base_index;	/* [한국어] 전체에서 아직 안 본 워드 수 */
	remaining = min_t(unsigned long, remaining,	/* [한국어] 그것과 */
			  DIV_ROUND_UP(bytes, sizeof(*bitmap->bitmap)));	/* [한국어] 고정된 바이트가 담는 워드 수 중 작은 쪽 */

	return remaining;	/* [한국어] 이 창으로 처리할 수 있는 워드 수 */
}

/*
 * Returns the length of the mapped IOVA range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_mapped_length - 지금 창이 덮는 IOVA 길이
 *
 * @bitmap: 대상 비트맵.
 * @return: 그 길이.
 *
 * 원 주석이 두 단계를 설명한다 — 고정된 워드 수를 IOVA 길이로 옮기고,
 * 그것을 전체 범위의 끝으로 다시 자른다. 마지막 창은 비트맵이 남아
 * 있어도 IOVA 범위가 먼저 끝날 수 있다.
 */
static unsigned long iova_bitmap_mapped_length(struct iova_bitmap *bitmap)
{
	unsigned long max_iova = bitmap->iova + bitmap->length - 1;	/* [한국어] 전체 범위의 끝 */
	unsigned long iova = iova_bitmap_mapped_iova(bitmap);	/* [한국어] 이 창의 시작 */
	unsigned long remaining;	/* [한국어] 이 창이 덮는 길이 */

	/*
	 * iova_bitmap_mapped_remaining() returns a number of indexes which
	 * when converted to IOVA gives us a max length that the bitmap
	 * pinned data can cover. Afterwards, that is capped to
	 * only cover the IOVA range in @bitmap::iova .. @bitmap::length.
	 */
	remaining = iova_bitmap_index_to_offset(bitmap,	/* [한국어] (원 주석: 고정된 워드 수를 IOVA 길이로 옮기고) */
			iova_bitmap_mapped_remaining(bitmap));	/* [한국어] 그 최대 길이를 구한 뒤 */

	if (iova + remaining - 1 > max_iova)	/* [한국어] (원 주석: 전체 범위의 끝으로 다시 자른다) */
		remaining -= ((iova + remaining - 1) - max_iova);	/* [한국어] 마지막 창은 비트맵이 남아도 IOVA 가 먼저 끝난다 */

	return remaining;	/* [한국어] 이 창이 실제로 덮는 길이 */
}

/*
 * Returns true if [@iova..@iova+@length-1] is part of the mapped IOVA range.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_mapped_range - 그 범위가 지금 창 안에 온전히 드는가
 *
 * @mapped: 지금 고정된 창.
 * @iova: 볼 범위의 시작.
 * @length: 그 길이.
 * @return: 온전히 들면 참.
 *
 * 걸치기만 해도 거짓이다 — 걸친 경우는 창을 옮긴 뒤 나누어 처리한다.
 */
static bool iova_bitmap_mapped_range(struct iova_bitmap_map *mapped,
				     unsigned long iova, size_t length)
{
	return mapped->npages &&	/* [한국어] 고정된 창이 있고 */
		(iova >= mapped->iova &&	/* [한국어] 시작이 창 안이고 */
		 (iova + length - 1) <= (mapped->iova + mapped->length - 1));	/* [한국어] 끝도 창 안이어야 한다 — 걸치기만 해도 거짓이다 */
}

/*
 * Advances to a selected range, releases the current pinned
 * pages and pins the next set of bitmap pages.
 * Returns 0 on success or otherwise errno.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iova_bitmap_advance_to - 그 IOVA 가 든 창으로 옮긴다
 *
 * @bitmap: 대상 비트맵.
 * @iova: 옮겨 갈 절대 주소.
 * @return: 0 성공, -EINVAL 범위 밖, -EFAULT 고정 실패.
 *
 * 옛 창을 먼저 풀고 새 창을 고정한다. 두 창이 겹칠 수 있지만, 페이지
 * 포인터 배열이 하나뿐이라 동시에 둘을 들 수 없다.
 */
static int iova_bitmap_advance_to(struct iova_bitmap *bitmap,
				  unsigned long iova)
{
	unsigned long index;	/* [한국어] 옮겨 갈 워드 색인 */

	index = iova_bitmap_offset_to_index(bitmap, iova - bitmap->iova);	/* [한국어] 절대 주소를 상대로 바꿔 색인을 구한다 */
	if (index >= bitmap->mapped_total_index)	/* [한국어] 전체 범위를 벗어나면 */
		return -EINVAL;	/* [한국어] 옮겨 갈 곳이 없다 */
	bitmap->mapped_base_index = index;	/* [한국어] 새 창의 시작 워드 */

	iova_bitmap_put(bitmap);	/* [한국어] 옛 창을 먼저 푼다 — 배열이 하나뿐이라 둘을 동시에 들 수 없다 */

	/* Pin the next set of bitmap pages */
	return iova_bitmap_get(bitmap);	/* [한국어] (원 주석: 다음 비트맵 페이지들을 고정한다) */
}

/**
 * iova_bitmap_for_each() - Iterates over the bitmap
 * @bitmap: IOVA bitmap to iterate
 * @opaque: Additional argument to pass to the callback
 * @fn: Function that gets called for each IOVA range
 *
 * Helper function to iterate over bitmap data representing a portion of IOVA
 * space. It hides the complexity of iterating bitmaps and translating the
 * mapped bitmap user pages into IOVA ranges to process.
 *
 * Return: 0 on success, and an error on failure either upon
 * iteration or when the callback returns an error.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iova_bitmap_for_each - 콜백에게 전 범위를 한 번에 넘긴다
 *
 * @bitmap: 대상 비트맵.
 * @opaque: 콜백에 함께 넘길 값.
 * @fn: 각 범위마다 부를 함수.
 * @return: 콜백의 결과.
 *
 * 이름과 달리 지금은 한 번만 부른다. 창 옮기기가 iova_bitmap_set() 안으로
 * 옮겨 갔기 때문인데, 콜백 쪽 계약은 그대로 두어 나중에 다시 나눠 부를
 * 여지를 남겼다.
 */
int iova_bitmap_for_each(struct iova_bitmap *bitmap, void *opaque,
			 iova_bitmap_fn_t fn)
{
	return fn(bitmap, bitmap->iova, bitmap->length, opaque);	/* [한국어] 창 옮기기가 set() 안으로 옮겨 가 한 번만 부르면 된다 */
}
EXPORT_SYMBOL_NS_GPL(iova_bitmap_for_each, "IOMMUFD");	/* [한국어] 범위를 콜백에 넘긴다 */

/**
 * iova_bitmap_set() - Records an IOVA range in bitmap
 * @bitmap: IOVA bitmap
 * @iova: IOVA to start
 * @length: IOVA range length
 *
 * Set the bits corresponding to the range [iova .. iova+length-1] in
 * the user bitmap.
 *
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iova_bitmap_set - 그 IOVA 범위에 해당하는 비트를 세운다
 *
 * @bitmap: 대상 비트맵.
 * @iova: 범위의 시작(절대 주소).
 * @length: 그 길이.
 *
 * 이 계층의 유일한 공개 쓰기 API 다. 드라이버가 더티를 발견할 때마다 부른다.
 *
 * 두 겹의 나누기가 있다.
 *  - 요청이 지금 창을 벗어나면 창을 옮기고 처음부터 다시 시작한다.
 *  - 창 안에서도 페이지 경계를 넘으면 페이지마다 나누어 kmap 한다.
 *    고정된 페이지들이 물리적으로 이어져 있지 않기 때문이다.
 *
 * 마지막 페이지를 넘어가면 남은 길이를 IOVA 로 되돌려 창을 옮기고 이어
 * 간다 — 그것이 goto update_indexes 다.
 *
 * 창을 옮기다 실패하면 조용히 돌아간다. 반환값이 없는 API 라 알릴 방법이
 * 없고, 사용자 버퍼가 잘못된 것이므로 그쪽 책임이다.
 */
void iova_bitmap_set(struct iova_bitmap *bitmap,
		     unsigned long iova, size_t length)
{
	struct iova_bitmap_map *mapped = &bitmap->mapped;	/* [한국어] 지금 고정된 창 */
	unsigned long cur_bit, last_bit, last_page_idx;	/* [한국어] 창 안에서의 비트 범위와 마지막 페이지 */

update_indexes:	/* [한국어] 창을 옮긴 뒤 비트 범위를 다시 계산하는 자리 */
	if (unlikely(!iova_bitmap_mapped_range(mapped, iova, length))) {	/* [한국어] 요청이 창을 벗어나면 */
		/*
		 * The attempt to advance the base index to @iova
		 * may fail if it's out of bounds, or pinning the pages
		 * returns an error.
		 */
		if (iova_bitmap_advance_to(bitmap, iova))	/* [한국어] (원 주석: 범위 밖이거나 고정에 실패하면 옮기기가 실패할 수 있다) */
			return;	/* [한국어] 반환값이 없는 API 라 조용히 물러난다 */
	}

	last_page_idx = mapped->npages - 1;	/* [한국어] 창의 마지막 페이지 */
	cur_bit = ((iova - mapped->iova) >>	/* [한국어] 창 기준의 상대 주소를 */
		mapped->pgshift) + mapped->pgoff * BITS_PER_BYTE;	/* [한국어] 비트 번호로 — 페이지 오프셋만큼 밀린다 */
	last_bit = (((iova + length - 1) - mapped->iova) >>	/* [한국어] 끝 주소도 */
		mapped->pgshift) + mapped->pgoff * BITS_PER_BYTE;	/* [한국어] 같은 방식으로 */

	do {
		unsigned int page_idx = cur_bit / BITS_PER_PAGE;	/* [한국어] 그 비트가 든 페이지 */
		unsigned int offset = cur_bit % BITS_PER_PAGE;	/* [한국어] 그 안에서의 위치 */
		unsigned int nbits = min(BITS_PER_PAGE - offset,	/* [한국어] 이 페이지에 남은 자리와 */
					 last_bit - cur_bit + 1);	/* [한국어] 아직 세울 비트 수 중 작은 쪽 */
		void *kaddr;	/* [한국어] 임시로 매핑한 커널 주소 */

		if (unlikely(page_idx > last_page_idx)) {	/* [한국어] 창의 마지막 페이지를 넘어가면 */
			unsigned long left =	/* [한국어] 남은 비트를 */
				((last_bit - cur_bit + 1) << mapped->pgshift);	/* [한국어] IOVA 길이로 되돌려 */

			iova += (length - left);	/* [한국어] 시작을 그만큼 밀고 */
			length = left;	/* [한국어] 남은 길이로 줄인 뒤 */
			goto update_indexes;	/* [한국어] 창을 옮겨 이어 간다 */
		}

		kaddr = kmap_local_page(mapped->pages[page_idx]);	/* [한국어] 고정된 페이지들이 이어져 있지 않아 하나씩 매핑한다 */
		bitmap_set(kaddr, offset, nbits);	/* [한국어] 그 페이지 몫의 비트를 세우고 */
		kunmap_local(kaddr);	/* [한국어] 곧바로 푼다 */
		cur_bit += nbits;	/* [한국어] 다음 비트로 */
	} while (cur_bit <= last_bit);	/* [한국어] 요청 범위를 다 세울 때까지 */
}
EXPORT_SYMBOL_NS_GPL(iova_bitmap_set, "IOMMUFD");	/* [한국어] 드라이버가 더티를 기록할 때 부른다 */
