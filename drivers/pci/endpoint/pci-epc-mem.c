// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Controller* Address Space Management
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] 엔드포인트가 호스트 메모리를 들여다볼 창 관리 (pci-epc-mem.c)
 *
 * === 파일의 역할 ===
 * 엔드포인트 모드 SoC 에서 "호스트의 메모리를 어떻게 읽고 쓸 것인가" 를
 * 푸는 파일이다. 275줄의 작은 할당자이지만 발상이 중요하다.
 *
 * 문제부터. 엔드포인트 안의 리눅스가 호스트 메모리의 어떤 주소를 읽고
 * 싶다고 하자. 그 주소는 저쪽 기계의 물리 주소라 이쪽 CPU 가 직접
 * 접근할 수 없다. 사이에 PCIe 링크가 있기 때문이다.
 *
 * 해법은 창(window)이다. SoC 의 물리 주소 공간 일부를 떼어 "이 영역에
 * 접근하면 PCIe 링크 너머로 나간다" 고 하드웨어에 설정해 두는 것이다.
 * 그러면 그 영역을 ioremap 해서 평범한 MMIO 처럼 읽고 쓰면, 실제로는
 * 그 접근이 TLP 가 되어 호스트 메모리에 닿는다.
 *
 * 그런데 그 창은 유한하다. 보통 수 MB 에서 수십 MB 정도이고, 호스트
 * 메모리 전체를 덮을 수는 없다. 그래서 필요할 때마다 창의 일부를 빌려
 * "지금은 호스트의 이 주소를 여기에 비춘다" 고 설정하고, 다 쓰면
 * 반납하는 방식으로 돌려 쓴다.
 *
 * 이 파일이 하는 일이 바로 그 "창의 일부를 빌려 주고 반납받는" 할당자다.
 * 비트맵으로 어느 페이지가 쓰이는지 추적하며, 실제 주소 변환 설정
 * (어느 호스트 주소에 비출지)은 이 파일이 아니라 pci-epc-core.c 의
 * pci_epc_map_addr() 가 컨트롤러 드라이버에 넘겨 처리한다.
 *
 * 페이지 크기가 PAGE_SIZE 가 아니라 창마다 다르다는 점이 특징이다.
 * 하드웨어의 주소 변환 단위가 그렇게 정해져 있기 때문이며, 그래서
 * 커널의 get_order() 를 쓰지 못하고 아래에 직접 만들어 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 컨트롤러 드라이버의 probe (예: dwc/pcie-designware-ep.c)
 *   -> pci_epc_mem_init() 또는 pci_epc_multi_mem_init()
 *      -> [이 파일] 창마다 비트맵을 만들어 할당자를 준비
 *
 * 엔드포인트 함수 드라이버가 호스트 메모리에 접근하려 할 때
 *   -> pci_epc_mem_alloc_addr()  [이 파일] 창에서 자리를 빌린다
 *      -> pci_epc_map_addr() [pci-epc-core.c] 그 자리를 호스트 주소에 연결
 *         -> 이제 그 가상 주소로 읽고 쓰면 호스트 메모리에 닿는다
 *      -> pci_epc_unmap_addr() / pci_epc_mem_free_addr() 로 반납
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. 뮤텍스와 ioremap 이 있어
 *   잠들 수 있으므로 인터럽트 컨텍스트에서 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-epc-core.c, 그리고 endpoint/functions/ 의 함수 드라이버들.
 * 아래쪽: 커널의 비트맵 할당자(bitmap_find_free_region), ioremap.
 * 공유 상태: struct pci_epc 의 windows 배열, mem, num_windows.
 *   창마다 struct pci_epc_mem 하나가 비트맵과 뮤텍스를 갖는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — 호출 0건).
 *
 * 방향을 뒤집어 보면 낯익은 구조이긴 하다. 호스트 쪽에서 NVMe 를 쓸 때,
 * 드라이버는 큐와 데이터 버퍼를 호스트 메모리에 두고 그 주소를 컨트롤러에
 * 알려 준다. 컨트롤러는 DMA 로 그 주소에 접근한다.
 * 엔드포인트 모드에서 NVMe 컨트롤러 역할을 하는 SoC 를 만든다면, 그
 * "호스트 메모리 접근" 을 구현하는 것이 바로 이 파일의 창이다.
 * 다만 그런 NVMe 엔드포인트 함수 드라이버는 이 트리 안에 없다 —
 * endpoint/functions/ 에 있는 것은 test, mhi, ntb, vntb 넷뿐이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_epc_mem_get_order()  : 창의 페이지 크기에 맞춘 order 계산.
 *                            커널의 get_order() 가 PAGE_SIZE 고정이라
 *                            쓸 수 없어 다시 구현한 것이다.
 * pci_epc_multi_mem_init() : 창 여러 개로 할당자를 초기화한다. 실제 본체.
 * pci_epc_mem_init()       : 창 하나짜리 편의 래퍼.
 * pci_epc_mem_exit()       : 정리.
 * pci_epc_mem_alloc_addr() : 창에서 자리를 빌리고 ioremap 까지 해 준다.
 * pci_epc_get_matching_window() : 물리 주소로 어느 창인지 되찾는다.
 * pci_epc_mem_free_addr()  : iounmap 하고 비트맵을 반납한다.
 * struct pci_epc_mem       : 창 하나의 상태(주소·크기·페이지크기·비트맵·락).
 * struct pci_epc_mem_window : 창의 기하 정보만 담은 것. 초기화 인자로 쓴다.
 */

/* [한국어] ioremap / iounmap. 빌린 창 영역을 커널 가상 주소로 매핑한다. */
#include <linux/io.h>
/* [한국어] EXPORT_SYMBOL_GPL 과 MODULE_* 매크로. */
#include <linux/module.h>
/* [한국어] kzalloc 계열과 kfree. 창 구조체와 비트맵 할당. */
#include <linux/slab.h>

/* [한국어] struct pci_epc, struct pci_epc_mem, struct pci_epc_mem_window
 * 정의와 이 파일이 구현하는 함수들의 선언. */
#include <linux/pci-epc.h>

/**
 * pci_epc_mem_get_order() - determine the allocation order of a memory size
 * @mem: address space of the endpoint controller
 * @size: the size for which to get the order
 *
 * Reimplement get_order() for mem->page_size since the generic get_order
 * always gets order with a constant PAGE_SIZE.
 */
/* [한국어]
 * pci_epc_mem_get_order - 창의 페이지 크기 기준으로 할당 order 를 구한다
 *
 * @mem: 대상 창. 여기서 page_size 를 가져온다.
 * @size: 필요한 크기(바이트).
 * @return: 비트맵 할당자에 넘길 order. size 를 덮으려면 2^order 페이지가
 *   필요하다는 뜻이다.
 *
 * 상류 주석이 존재 이유를 밝히고 있다 — 커널의 get_order() 는 PAGE_SIZE
 * 를 상수로 쓰는데, 여기서는 창마다 페이지 크기가 다르므로 쓸 수 없다.
 * 그 크기는 하드웨어의 주소 변환 단위라 소프트웨어가 정할 수 없다.
 *
 * 계산 요령은 get_order() 와 같다.
 *   size-- 로 1 을 빼는 것이 핵심이다. 정확히 한 페이지 크기일 때
 *   order 0 이 나오게 하려는 것으로, 빼지 않으면 4096 바이트 요청에
 *   order 1(두 페이지)이 나온다.
 *   그다음 페이지 수로 바꾸고, fls 로 최상위 비트 위치를 구한다.
 *   그 위치가 곧 2 의 몇 제곱인지를 말해 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산.
 *
 * 호출 체인:
 *   pci_epc_mem_alloc_addr() / pci_epc_mem_free_addr() → [이 함수]
 */
static int pci_epc_mem_get_order(struct pci_epc_mem *mem, size_t size)
{
	int order;
	/* [한국어] 페이지 크기의 log2. 나눗셈 대신 시프트를 쓰기 위해서다.
	 * 페이지 크기는 항상 2 의 거듭제곱이라 이 변환이 성립한다. */
	unsigned int page_shift = ilog2(mem->window.page_size);

	/* [한국어] 1 을 빼는 이유가 이 함수에서 가장 덜 자명하다.
	 * 크기가 페이지 크기의 정확한 배수일 때 order 가 한 단계 커지는
	 * 것을 막는다. 예를 들어 페이지가 4096 이고 size 가 4096 이면,
	 * 빼지 않으면 4096>>12 = 1 이 되어 fls(1) = 1(두 페이지)이 나온다.
	 * 빼면 4095>>12 = 0 이 되어 fls(0) = 0(한 페이지)으로 맞는다. */
	size--;
	/* [한국어] 바이트를 페이지 수로 바꾼다. */
	size >>= page_shift;
#if BITS_PER_LONG == 32
	/* [한국어] 32비트 아키텍처에서는 size_t 가 32비트라 fls 로 충분하다. */
	order = fls(size);
#else
	/* [한국어] 64비트에서는 size_t 가 64비트이므로 fls64 를 써야
	 * 상위 32비트에 있는 값을 놓치지 않는다. 창이 4GB 를 넘는 경우는
	 * 드물지만 타입 폭에 맞추는 것이 옳다. */
	order = fls64(size);
#endif
	/* [한국어] fls 는 최상위 1 비트의 위치를 1-기반으로 돌려준다.
	 * size 가 0 이면 0 이 나와 order 0(한 페이지)이 된다. */
	return order;
}

/**
 * pci_epc_multi_mem_init() - initialize the pci_epc_mem structure
 * @epc: the EPC device that invoked pci_epc_mem_init
 * @windows: pointer to windows supported by the device
 * @num_windows: number of windows device supports
 *
 * Invoke to initialize the pci_epc_mem structure used by the
 * endpoint functions to allocate mapped PCI address.
 */
/* [한국어] (상류 kernel-doc 은 위에 그대로 두었다)
 *
 * pci_epc_multi_mem_init - 창 여러 개로 주소 공간 할당자를 준비한다
 *
 * @epc: 이 창들을 소유할 엔드포인트 컨트롤러.
 * @windows: 창들의 기하 정보 배열(물리 시작 주소, 크기, 페이지 크기).
 *   컨트롤러 드라이버가 자기 하드웨어를 보고 채워 넘긴다.
 * @num_windows: 창 개수.
 * @return: 0 이면 성공. -EINVAL(인자 없음), -ENOMEM.
 *
 * 창마다 struct pci_epc_mem 을 만들고 비트맵을 붙인다. 이 파일의
 * 초기화 본체이며, pci_epc_mem_init() 은 이것의 창 하나짜리 래퍼다.
 *
 * 창이 여럿일 수 있는 이유는 하드웨어가 그렇게 생겼기 때문이다.
 * 어떤 컨트롤러는 크기와 페이지 단위가 다른 창을 여러 개 두어,
 * 작은 접근에는 세밀한 창을, 큰 전송에는 큰 창을 쓰게 한다.
 * 아래 alloc_addr 이 창을 순서대로 훑으며 맞는 것을 찾는 이유다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버의 probe — 프로세스 컨텍스트.
 *
 * 에러 경로: goto err_mem 으로 모아 놓고, 그때까지 만든 창들을 역순으로
 *   해제한다. i-- 를 먼저 하는 이유는 실패한 회차의 mem 을 이미
 *   직접 해제했거나 아직 epc->windows[i] 에 넣지 않았기 때문이다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 probe → [이 함수]
 *   pci_epc_mem_init() → [이 함수]
 */
int pci_epc_multi_mem_init(struct pci_epc *epc,
			   struct pci_epc_mem_window *windows,
			   unsigned int num_windows)
{
	/* [한국어] 지금 만들고 있는 창. 에러 경로에서도 쓰므로 함수 범위에 둔다. */
	struct pci_epc_mem *mem = NULL;
	/* [한국어] 그 창의 사용 현황 비트맵. */
	unsigned long *bitmap = NULL;
	/* [한국어] 페이지 크기의 log2. 크기를 페이지 수로 바꾸는 데 쓴다. */
	unsigned int page_shift;
	/* [한국어] 보정된 페이지 크기. 아래에서 PAGE_SIZE 미만이면 올린다. */
	size_t page_size;
	/* [한국어] 비트맵에 필요한 바이트 수. */
	int bitmap_size;
	/* [한국어] 이 창이 담는 페이지 수. */
	int pages;
	int ret;
	int i;

	/* [한국어] 먼저 0 으로 만든다. 아래에서 실패해 돌아가더라도 이 값이
	 * 0 이면 pci_epc_mem_exit() 이 즉시 물러나므로, 초기화되지 않은
	 * windows 배열을 건드리는 일이 없다. */
	epc->num_windows = 0;

	/* [한국어] 창이 하나도 없으면 할 일이 없다. 컨트롤러 드라이버의
	 * 실수이므로 오류로 알린다. */
	if (!windows || !num_windows)
		return -EINVAL;

	/* [한국어] 창 포인터 배열을 잡는다. 구조체 자체가 아니라 포인터
	 * 배열인 이유는 창마다 따로 할당해 수명을 개별로 다루기 위해서다. */
	epc->windows = kzalloc_objs(*epc->windows, num_windows);
	if (!epc->windows)
		return -ENOMEM;

	/* [한국어] 창 하나씩 준비한다. */
	for (i = 0; i < num_windows; i++) {
		page_size = windows[i].page_size;
		/* [한국어] 하드웨어의 변환 단위가 CPU 페이지보다 작더라도
		 * PAGE_SIZE 미만으로는 다루지 않는다. 아래에서 ioremap 을
		 * 하는데 그 단위가 CPU 페이지이기 때문이다 — 더 잘게 나눠
		 * 빌려 줘 봐야 매핑할 수 없다. */
		if (page_size < PAGE_SIZE)
			page_size = PAGE_SIZE;
		page_shift = ilog2(page_size);
		/* [한국어] 창 크기를 페이지 수로 바꾼다. 이것이 비트맵의 비트 수다. */
		pages = windows[i].size >> page_shift;
		/* [한국어] 그 비트 수를 담을 long 개수를 구해 바이트로 바꾼다.
		 * 비트맵 API 가 unsigned long 배열을 전제하므로 그 단위로 맞춘다. */
		bitmap_size = BITS_TO_LONGS(pages) * sizeof(long);

		/* [한국어] 창 상태 구조체. */
		mem = kzalloc_obj(*mem);
		if (!mem) {
			ret = -ENOMEM;
			/* [한국어] 이 회차는 아직 epc->windows[i] 에 들어가지
			 * 않았으므로 정리 대상에서 뺀다. */
			i--;
			goto err_mem;
		}

		/* [한국어] 사용 현황 비트맵. 0 으로 초기화되어 전부 비어 있다. */
		bitmap = kzalloc(bitmap_size, GFP_KERNEL);
		if (!bitmap) {
			ret = -ENOMEM;
			/* [한국어] 방금 잡은 mem 은 아직 배열에 없으므로 여기서
			 * 직접 해제한다. 그러고 나서 앞 회차들을 정리하러 간다. */
			kfree(mem);
			i--;
			goto err_mem;
		}

		/* [한국어] 창의 기하 정보를 복사해 둔다. 인자로 받은 배열은
		 * 호출자의 것이라 이 함수가 끝나면 사라질 수 있다. */
		mem->window.phys_base = windows[i].phys_base;
		mem->window.size = windows[i].size;
		/* [한국어] 보정된 페이지 크기를 쓴다. 원본이 아니라 위에서
		 * PAGE_SIZE 로 올린 값이라는 점이 중요하다. */
		mem->window.page_size = page_size;
		mem->bitmap = bitmap;
		mem->pages = pages;
		/* [한국어] 창마다 뮤텍스가 따로다. 여러 함수 드라이버가 서로
		 * 다른 창에서 동시에 할당할 수 있게 하려는 것으로, 창 하나에
		 * 락 하나면 경합이 그 창 안에서만 일어난다. */
		mutex_init(&mem->lock);
		epc->windows[i] = mem;
	}

	/* [한국어] 첫 창을 기본 창으로 지정한다. 창 하나만 쓰는 옛 API 와
	 * 그것을 쓰는 코드가 epc->mem 을 직접 참조하기 때문에 남겨 둔다. */
	epc->mem = epc->windows[0];
	/* [한국어] 마지막에 개수를 채운다. 이 값이 0 이 아니게 되는 순간부터
	 * 다른 코드가 windows 배열을 유효한 것으로 취급하므로, 모든 준비가
	 * 끝난 뒤여야 한다. */
	epc->num_windows = num_windows;

	return 0;

err_mem:
	/* [한국어] 지금까지 배열에 넣은 창들을 역순으로 해제한다.
	 * i 는 위에서 조정되어 "배열에 실제로 들어 있는 마지막 인덱스" 를
	 * 가리키고 있다. */
	for (; i >= 0; i--) {
		mem = epc->windows[i];
		kfree(mem->bitmap);
		kfree(mem);
	}
	/* [한국어] 포인터 배열 자체도 해제한다. epc->num_windows 는 위에서
	 * 0 으로 두었으므로 나중에 exit 이 불려도 안전하다.
	 * 다만 epc->windows 를 NULL 로 되돌리지는 않는데, num_windows 가
	 * 0 이라 아무도 이 포인터를 보지 않기 때문이다. */
	kfree(epc->windows);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_multi_mem_init);

/**
 * pci_epc_mem_init() - Initialize the pci_epc_mem structure
 * @epc: the EPC device that invoked pci_epc_mem_init
 * @base: Physical address of the window region
 * @size: Total Size of the window region
 * @page_size: Page size of the window region
 *
 * Invoke to initialize a single pci_epc_mem structure used by the
 * endpoint functions to allocate memory for mapping the PCI host memory
 */
/* [한국어]
 * pci_epc_mem_init - 창 하나짜리 편의 래퍼
 *
 * @epc: 대상 엔드포인트 컨트롤러.
 * @base: 창의 물리 시작 주소.
 * @size: 창 전체 크기.
 * @page_size: 하드웨어의 주소 변환 단위.
 * @return: pci_epc_multi_mem_init() 의 반환값 그대로.
 *
 * 창이 하나뿐인 컨트롤러가 대부분이라 그런 드라이버가 배열을 만들지
 * 않아도 되게 한 것이다. 인자 셋을 struct 하나로 묶어 본체에 넘긴다.
 *
 * mem_window 를 스택에 두는 것이 안전한 이유는, 본체가 그 내용을
 * 자기 구조체로 복사해 두기 때문이다. 이 함수가 끝나 스택이 사라져도
 * 문제가 없다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버의 probe — 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 probe → [이 함수] → pci_epc_multi_mem_init()
 */
int pci_epc_mem_init(struct pci_epc *epc, phys_addr_t base,
		     size_t size, size_t page_size)
{
	/* [한국어] 스택에 창 하나짜리 기하 정보를 만든다. */
	struct pci_epc_mem_window mem_window;

	/* [한국어] 인자 셋을 그대로 옮겨 담는다. */
	mem_window.phys_base = base;
	mem_window.size = size;
	mem_window.page_size = page_size;

	/* [한국어] 창 개수 1 로 본체를 부른다. */
	return pci_epc_multi_mem_init(epc, &mem_window, 1);
}
EXPORT_SYMBOL_GPL(pci_epc_mem_init);

/**
 * pci_epc_mem_exit() - cleanup the pci_epc_mem structure
 * @epc: the EPC device that invoked pci_epc_mem_exit
 *
 * Invoke to cleanup the pci_epc_mem structure allocated in
 * pci_epc_mem_init().
 */
/* [한국어]
 * pci_epc_mem_exit - 창과 비트맵을 모두 해제한다
 *
 * @epc: 정리할 엔드포인트 컨트롤러.
 * @return: 없음.
 *
 * init 의 반대다. 아직 빌려 간 자리가 있는지는 확인하지 않는데,
 * 이 시점에는 함수 드라이버들이 이미 언바인드되어 반납을 마쳤어야
 * 하기 때문이다. 그 순서를 지키는 것은 호출자의 책임이다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버의 remove — 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 remove → [이 함수]
 */
void pci_epc_mem_exit(struct pci_epc *epc)
{
	struct pci_epc_mem *mem;
	int i;

	/* [한국어] 초기화되지 않았거나 init 이 실패한 경우다. init 이 실패
	 * 경로에서 num_windows 를 0 으로 남겨 두므로 이 검사로 걸러진다. */
	if (!epc->num_windows)
		return;

	/* [한국어] 창마다 비트맵과 구조체를 해제한다. */
	for (i = 0; i < epc->num_windows; i++) {
		mem = epc->windows[i];
		kfree(mem->bitmap);
		kfree(mem);
	}
	/* [한국어] 포인터 배열도 해제한다. */
	kfree(epc->windows);

	/* [한국어] 세 필드를 모두 되돌린다. init 의 실패 경로와 달리
	 * 여기서는 windows 도 NULL 로 만드는데, 컨트롤러가 계속 살아 있고
	 * 나중에 다시 초기화될 수 있어 해제된 포인터를 남겨 두면 위험하기
	 * 때문이다. */
	epc->windows = NULL;
	epc->mem = NULL;
	epc->num_windows = 0;
}
EXPORT_SYMBOL_GPL(pci_epc_mem_exit);

/**
 * pci_epc_mem_alloc_addr() - allocate memory address from EPC addr space
 * @epc: the EPC device on which memory has to be allocated
 * @phys_addr: populate the allocated physical address here
 * @size: the size of the address space that has to be allocated
 *
 * Invoke to allocate memory address from the EPC address space. This
 * is usually done to map the remote RC address into the local system.
 */
/* [한국어]
 * pci_epc_mem_alloc_addr - 창에서 자리를 빌리고 매핑까지 해 준다
 *
 * @epc: 대상 컨트롤러.
 * @phys_addr: 빌린 자리의 물리 주소를 여기에 채워 준다(출력 인자).
 * @size: 필요한 크기.
 * @return: 그 자리의 커널 가상 주소. 어느 창에서도 자리를 못 구하면 NULL.
 *
 * 이 함수가 돌려준 가상 주소는 아직 아무 데도 연결되어 있지 않다.
 * "SoC 주소 공간에 자리를 잡았다" 는 것뿐이고, 그 자리를 호스트의
 * 어느 주소에 비출지는 pci_epc_map_addr() 가 따로 설정한다.
 * 자리 잡기와 연결하기를 나눈 것은 그 둘의 주체가 다르기 때문이다 —
 * 자리는 이 할당자가, 연결은 컨트롤러 하드웨어가 맡는다.
 *
 * 창을 순서대로 훑으며 맞는 곳을 찾는다. 창마다 크기와 페이지 단위가
 * 다를 수 있어, 큰 요청은 앞쪽 작은 창에서 실패하고 뒤쪽으로 넘어간다.
 *
 * 출력 인자를 쓰는 이유는 반환값 하나로는 부족하기 때문이다. 호출자는
 * 가상 주소(자기가 읽고 쓸 곳)와 물리 주소(하드웨어에 설정할 값)를
 * 둘 다 알아야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스와 ioremap 이 있어 잠들 수 있다.
 *
 * 에러 경로: 모든 창에서 실패하면 NULL. ioremap 이 실패한 경우에는
 *   방금 잡은 비트맵 자리를 되돌리고 다음 창으로 넘어간다 — 자리는
 *   있었지만 매핑이 안 된 것이므로 다른 창에서는 될 수도 있다.
 *
 * 호출 체인:
 *   엔드포인트 함수 드라이버 → [이 함수]
 *     → bitmap_find_free_region() → ioremap()
 */
void __iomem *pci_epc_mem_alloc_addr(struct pci_epc *epc,
				     phys_addr_t *phys_addr, size_t size)
{
	void __iomem *virt_addr;
	struct pci_epc_mem *mem;
	/* [한국어] 페이지 번호를 물리 주소로 되돌릴 때 쓸 시프트 양. */
	unsigned int page_shift;
	/* [한국어] 페이지 경계로 올림한 크기. 할당 단위가 페이지라 실제로
	 * 차지하는 것은 이만큼이다. */
	size_t align_size;
	/* [한국어] 비트맵에서 받은 페이지 번호. 음수면 자리가 없다는 뜻. */
	int pageno;
	int order;
	int i;

	/* [한국어] 창을 앞에서부터 훑는다. 첫 번째로 맞는 창을 쓴다. */
	for (i = 0; i < epc->num_windows; i++) {
		mem = epc->windows[i];
		/* [한국어] 창 전체보다 큰 요청이면 볼 것도 없다. 비트맵을
		 * 뒤지기 전에 걸러 낸다. */
		if (size > mem->window.size)
			continue;

		/* [한국어] 창의 페이지 크기 경계로 올린다. 페이지 단위로만
		 * 빌려 줄 수 있으므로 실제 점유량은 이 값이다. */
		align_size = ALIGN(size, mem->window.page_size);
		/* [한국어] 비트맵 할당자에 넘길 order 로 바꾼다. */
		order = pci_epc_mem_get_order(mem, align_size);

		/* [한국어] 이 창의 비트맵을 보호한다. 창마다 락이 따로라
		 * 다른 창을 쓰는 스레드와는 경합하지 않는다. */
		mutex_lock(&mem->lock);
		/* [한국어] 2^order 페이지가 연속으로 비어 있는 곳을 찾아
		 * 표시까지 한다. 찾기와 표시가 한 연산이라 그 사이에 다른
		 * 스레드가 끼어들 틈이 없다. */
		pageno = bitmap_find_free_region(mem->bitmap, mem->pages,
						 order);
		if (pageno >= 0) {
			/* [한국어] 페이지 번호를 물리 주소로 되돌린다.
			 * 창의 시작 주소에 (페이지 번호 × 페이지 크기)를 더한다.
			 * phys_addr_t 로 캐스팅하는 이유는 pageno 가 int 라
			 * 시프트 결과가 32비트에서 넘칠 수 있기 때문이다. */
			page_shift = ilog2(mem->window.page_size);
			*phys_addr = mem->window.phys_base +
				((phys_addr_t)pageno << page_shift);
			/* [한국어] 그 물리 영역을 커널 가상 주소로 매핑한다.
			 * 이 영역에 접근하면 PCIe 링크 너머로 나가므로,
			 * 일반 메모리가 아니라 MMIO 로 매핑해야 한다. */
			virt_addr = ioremap(*phys_addr, align_size);
			if (!virt_addr) {
				/* [한국어] 자리는 잡았지만 매핑이 안 됐다.
				 * 잡은 자리를 반드시 되돌려야 새는 일이 없다. */
				bitmap_release_region(mem->bitmap,
						      pageno, order);
				mutex_unlock(&mem->lock);
				/* [한국어] 다음 창을 시도한다. 다른 창에서는
				 * 매핑이 될 수도 있다. */
				continue;
			}
			mutex_unlock(&mem->lock);
			return virt_addr;
		}
		/* [한국어] 이 창에는 연속된 자리가 없다. 락을 풀고 다음 창으로. */
		mutex_unlock(&mem->lock);
	}

	/* [한국어] 모든 창에서 실패. 호출자는 대개 이 경우 요청 크기를
	 * 줄여 다시 시도하거나 전송을 여러 번에 나눈다. */
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_epc_mem_alloc_addr);

/* [한국어]
 * pci_epc_get_matching_window - 물리 주소가 어느 창에 속하는지 찾는다
 *
 * @epc: 대상 컨트롤러.
 * @phys_addr: 찾을 물리 주소.
 * @return: 그 주소를 담는 창. 어느 창에도 속하지 않으면 NULL.
 *
 * 반납할 때 필요한 함수다. pci_epc_mem_free_addr() 은 물리 주소만
 * 받는데, 그 주소가 어느 창의 비트맵에 표시되어 있는지 알아야
 * 되돌릴 수 있다.
 *
 * 창 개수가 많아야 몇 개라 선형 탐색으로 충분하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 조회이며 락을 잡지 않는다 —
 *   창 목록 자체는 init 과 exit 사이에 바뀌지 않기 때문이다.
 *
 * 호출 체인:
 *   pci_epc_mem_free_addr() → [이 함수]
 */
static struct pci_epc_mem *pci_epc_get_matching_window(struct pci_epc *epc,
						       phys_addr_t phys_addr)
{
	/* [한국어] 현재 검사 중인 창. */
	struct pci_epc_mem *mem;
	/* [한국어] 창 목록 순회 인덱스. */
	int i;

	for (i = 0; i < epc->num_windows; i++) {
		mem = epc->windows[i];

		/* [한국어] 시작은 포함하고 끝은 제외하는 반열린 구간으로
		 * 비교한다. 창 두 개가 맞닿아 있을 때 경계 주소가 양쪽에
		 * 걸리지 않게 하는 표준적인 방식이다. */
		if (phys_addr >= mem->window.phys_base &&
		    phys_addr < (mem->window.phys_base + mem->window.size))
			return mem;
	}

	/* [한국어] 어느 창에도 없다. 호출자가 엉뚱한 주소를 넘겼거나
	 * 이미 exit 이 지나간 경우다. */
	return NULL;
}

/**
 * pci_epc_mem_free_addr() - free the allocated memory address
 * @epc: the EPC device on which memory was allocated
 * @phys_addr: the allocated physical address
 * @virt_addr: virtual address of the allocated mem space
 * @size: the size of the allocated address space
 *
 * Invoke to free the memory allocated using pci_epc_mem_alloc_addr.
 */
/* [한국어]
 * pci_epc_mem_free_addr - 빌린 자리를 매핑 해제하고 반납한다
 *
 * @epc: 대상 컨트롤러.
 * @phys_addr: alloc 이 알려 준 물리 주소.
 * @virt_addr: alloc 이 돌려준 가상 주소.
 * @size: alloc 에 넘겼던 크기. 같은 값이어야 order 계산이 맞는다.
 * @return: 없음.
 *
 * alloc 의 정확한 반대다. 인자 넷을 모두 요구하는 것이 번거로워 보이지만,
 * 이 할당자가 어떤 상태도 따로 기록해 두지 않기 때문이다 — 비트맵에는
 * "쓰인다" 만 있고 "얼마나" 는 없으므로 크기를 다시 받아야 order 를
 * 되구할 수 있다.
 *
 * 그래서 alloc 때와 다른 size 를 넘기면 조용히 잘못된 범위를 반납하게
 * 된다. 호출자가 짝을 맞춰야 하는 API 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. iounmap 과 뮤텍스가 있어 잠들 수 있다.
 *
 * 에러 경로: 창을 못 찾으면 메시지만 남기고 돌아간다. void 함수라
 *   알릴 방법이 없고, 이 경우는 호출자의 오류다.
 *
 * 호출 체인:
 *   엔드포인트 함수 드라이버 → [이 함수]
 *     → pci_epc_get_matching_window() → iounmap() → bitmap_release_region()
 */
void pci_epc_mem_free_addr(struct pci_epc *epc, phys_addr_t phys_addr,
			   void __iomem *virt_addr, size_t size)
{
	/* [한국어] 이 주소가 속한 창. 아래에서 찾는다. */
	struct pci_epc_mem *mem;
	/* [한국어] 페이지 크기의 log2. 주소를 페이지 번호로 되돌리는 데 쓴다. */
	unsigned int page_shift;
	/* [한국어] 이 창의 페이지 크기. 여러 번 쓰므로 지역 변수로 꺼내 둔다. */
	size_t page_size;
	/* [한국어] 반납할 페이지 번호. */
	int pageno;
	/* [한국어] 반납할 크기의 order. alloc 때와 같은 값이 나와야 한다. */
	int order;

	/* [한국어] 이 주소가 어느 창의 것인지 찾는다. */
	mem = pci_epc_get_matching_window(epc, phys_addr);
	if (!mem) {
		/* [한국어] pci_err 이 아니라 pr_err 인 것은 이 시점에 쓸
		 * device 를 특정하기 어렵기 때문으로 보인다 — epc 는 있지만
		 * 잘못된 주소라 어느 창의 문제인지 알 수 없다. */
		pr_err("failed to get matching window\n");
		return;
	}

	page_size = mem->window.page_size;
	page_shift = ilog2(page_size);
	/* [한국어] 매핑을 먼저 푼다. 비트맵을 먼저 반납하면 그 사이에 다른
	 * 스레드가 같은 자리를 받아 ioremap 할 수 있고, 그러면 두 매핑이
	 * 겹친다. 순서가 뒤바뀌면 안 되는 이유다. */
	iounmap(virt_addr);
	/* [한국어] 물리 주소를 창 안의 페이지 번호로 되돌린다.
	 * alloc 의 계산을 그대로 뒤집은 것이다. */
	pageno = (phys_addr - mem->window.phys_base) >> page_shift;
	/* [한국어] alloc 때와 같은 방식으로 올림해야 같은 order 가 나온다. */
	size = ALIGN(size, page_size);
	order = pci_epc_mem_get_order(mem, size);
	/* [한국어] 비트맵을 만지므로 락을 잡는다. */
	mutex_lock(&mem->lock);
	/* [한국어] 그 자리를 비어 있음으로 표시한다. */
	bitmap_release_region(mem->bitmap, pageno, order);
	mutex_unlock(&mem->lock);
}
EXPORT_SYMBOL_GPL(pci_epc_mem_free_addr);

MODULE_DESCRIPTION("PCI EPC Address Space Management");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
