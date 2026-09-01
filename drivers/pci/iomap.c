// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the default iomap interfaces
 *
 * (C) Copyright 2004 Linus Torvalds
 */

/*
 * [한국어 설명] BAR 를 커널 가상 주소로 매핑하는 헬퍼 (iomap.c)
 *
 * === 파일의 역할 ===
 * 드라이버가 BAR 에 접근하려면 물리 주소를 커널 가상 주소로 매핑해야 한다.
 * 그 일 자체는 ioremap() 이 하지만, PCI 에는 두 가지 성가신 점이 있다.
 *
 *   1) BAR 가 메모리 공간일 수도 I/O 포트 공간일 수도 있다. 둘은 접근
 *      방법이 전혀 다르다(readl vs inl). 드라이버가 매번 그것을 구분해
 *      코드를 두 벌 쓰는 것은 번거롭다.
 *   2) BAR 번호만 알지 주소와 크기는 pci_dev 에서 꺼내야 한다.
 *
 * 이 파일은 둘을 함께 해결한다. pci_iomap(pdev, bar, maxlen) 하나로
 * BAR 번호를 주면 종류를 판별해 알맞게 매핑하고, 그 결과를 __iomem
 * 토큰으로 돌려준다. 이후 ioread32()/iowrite32() 같은 통합 접근자를 쓰면
 * 메모리든 포트든 같은 코드로 다룰 수 있다.
 *
 * 이 "통합 토큰" 이 lib/iomap.c 의 기법이다. 포트 공간이면 주소에 표식을
 * 붙여 돌려주고, 접근자가 그 표식을 보고 inl/outl 로 갈린다. 그래서
 * pci_iomap 의 결과는 진짜 포인터가 아닐 수 있고 절대 역참조하면 안 된다.
 * __iomem 이라는 sparse 어노테이션이 그 실수를 정적으로 잡아 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 probe
 *   -> pci_request_regions() 로 영역을 예약하고
 *   -> [이 파일] pci_iomap(pdev, 0, 0) 으로 BAR0 를 매핑
 *      -> pci_resource_start/len 으로 주소와 크기를 얻고
 *      -> 종류에 따라 ioremap() 또는 포트 토큰 생성
 *   -> ioread32(base + offset) 으로 접근
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(ioremap 이 페이지 테이블을 건드린다).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 드라이버들, devres.c 의 pcim_iomap.
 * 아래쪽: lib/iomap.c 의 ioport_map, asm-generic 의 ioremap 계열.
 * 공유 상태: 없다. struct pci_dev 의 resource[] 를 읽기만 한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다. drivers/nvme/ 에
 * "pci_iomap" 이 0건이다(주석 제거 후 검색).
 *
 * 대신 ioremap() 을 직접 부른다:
 *   dev->bar = ioremap(pci_resource_start(pdev, 0), size);
 *
 * 이유를 코드에서 추정할 수 있다. NVMe 의 BAR0 는 반드시 메모리 공간이라
 * 종류 판별이 불필요하고, 통합 토큰이 아닌 진짜 __iomem 포인터를 쓰는
 * 편이 readl/writel 로 직접 접근할 때 명확하다.
 *
 * 또 NVMe 는 매핑 크기를 스스로 계산한다. 도어벨 배열의 크기가 큐 개수와
 * 도어벨 stride(CAP.DSTRD)에 달려 있어서, BAR 전체가 아니라 필요한
 * 만큼만 매핑하려면 크기를 직접 정해야 한다. 큐를 더 만들면 매핑을
 * 다시 잡는 nvme_remap_bar() 가 그 처리다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_iomap()             : BAR 하나를 매핑한다. maxlen 이 0 이면 BAR 전체,
 *                           아니면 그 크기만큼만 매핑한다.
 * pci_iomap_range()       : BAR 안의 특정 오프셋부터 매핑한다. BAR 가 크고
 *                           일부만 필요할 때 쓴다.
 * pci_iomap_wc()          : write-combining 매핑. 쓰기를 모아 보내 대역폭을
 *                           높이지만 순서 보장이 약해진다. 프레임버퍼용이며,
 *                           레지스터에는 쓰면 안 된다(순서가 중요하므로).
 * pci_iomap_wc_range()    : 위 둘의 조합.
 * pci_iounmap()           : 매핑을 푼다. 포트 토큰이면 ioport_unmap 으로 간다.
 */

#include <linux/pci.h>
#include <linux/io.h>

#include <linux/export.h>

#include "pci.h" /* for pci_bar_index_is_valid() */


/**
 * pci_iomap_range - create a virtual mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @offset: map memory at the given offset in BAR
 * @maxlen: max length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR from offset to the end, pass %0 here.
 * */
void __iomem *pci_iomap_range(struct pci_dev *dev,
			      int bar,
			      unsigned long offset,
			      unsigned long maxlen)
{
	resource_size_t start, len;
	/* [한국어] 자원 플래그. IORESOURCE_IO 인지 IORESOURCE_MEM 인지로 매핑 방식이 갈린다. */
	unsigned long flags;

	/* [한국어] BAR 번호가 0~5 범위인지 확인한다. 사용자 공간이나 드라이버가 잘못된 값을
	 * 넘기면 resource[] 배열 밖을 읽게 되므로 반드시 먼저 걸러야 한다. */
	if (!pci_bar_index_is_valid(bar))
		return NULL;

	/* [한국어] BAR 의 시작 물리(또는 포트) 주소. */
	start = pci_resource_start(dev, bar);
	/* [한국어] BAR 의 길이. */
	len = pci_resource_len(dev, bar);
	/* [한국어] BAR 의 종류와 속성. */
	flags = pci_resource_flags(dev, bar);

	/* [한국어] 요청한 오프셋이 BAR 길이 이상이면 매핑할 것이 없고, start 가 0 이면
	 * 그 BAR 가 구현되지 않았거나 아직 주소를 배정받지 못한 것이다. */
	if (len <= offset || !start)
		return NULL;

	/* [한국어] 오프셋만큼 잘라 낸 나머지 길이. */
	len -= offset;
	/* [한국어] 매핑 시작 주소를 오프셋만큼 앞으로 옮긴다. 순서가 중요하다 —
	 * start 를 먼저 옮기면 위 len 계산의 기준이 흐트러진다. */
	start += offset;
	/* [한국어] 호출자가 상한을 지정했고 남은 길이가 그보다 크면, */
	if (maxlen && len > maxlen)
		/* [한국어] 상한으로 자른다. maxlen 이 0 이면 "BAR 끝까지" 라는 뜻이라 자르지 않는다. */
		len = maxlen;
	/* [한국어] I/O 포트 공간이면, */
	if (flags & IORESOURCE_IO)
		/* [한국어] 아키텍처가 제공하는 포트 매핑을 쓴다. x86 처럼 포트 공간이 별도 주소 공간인
		 * 곳에서는 실제 매핑이 아니라 포트 번호를 그대로 인코딩한 쿠키를 돌려준다.
		 * 그래서 반환값을 포인터처럼 역참조하면 안 되고 ioread 와 iowrite 계열로만 써야 한다. */
		return __pci_ioport_map(dev, start, len);
	/* [한국어] 메모리 공간이면, */
	if (flags & IORESOURCE_MEM)
		return ioremap(start, len);
	/* What? */
	return NULL;
}
EXPORT_SYMBOL(pci_iomap_range);


/**
 * pci_iomap_wc_range - create a virtual WC mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @offset: map memory at the given offset in BAR
 * @maxlen: max length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way. When possible write combining
 * is used.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR from offset to the end, pass %0 here.
 * */
void __iomem *pci_iomap_wc_range(struct pci_dev *dev,
				 int bar,
				 unsigned long offset,
				 unsigned long maxlen)
{
	resource_size_t start, len;
	/* [한국어] 자원 플래그. */
	unsigned long flags;

	/* [한국어] BAR 번호 유효성 검사 — 일반 판과 동일하다. */
	if (!pci_bar_index_is_valid(bar))
		return NULL;

	/* [한국어] BAR 시작 주소. */
	start = pci_resource_start(dev, bar);
	/* [한국어] BAR 길이. */
	len = pci_resource_len(dev, bar);
	/* [한국어] BAR 플래그. */
	flags = pci_resource_flags(dev, bar);

	/* [한국어] 오프셋이 범위를 벗어났거나 주소가 배정되지 않은 경우. */
	if (len <= offset || !start)
		return NULL;
	/* [한국어] [일반 판과의 차이] I/O 포트 공간은 쓰기 결합이라는 개념 자체가 없으므로
	 * 여기서 곧장 실패한다. 일반 판은 __pci_ioport_map() 으로 처리하지만
	 * 이 판은 지원하지 않는다. */
	if (flags & IORESOURCE_IO)
		return NULL;

	/* [한국어] 오프셋만큼 잘라 낸 길이. */
	len -= offset;
	/* [한국어] 시작 주소를 옮긴다. */
	start += offset;
	/* [한국어] 상한이 있고 남은 길이가 그보다 크면, */
	if (maxlen && len > maxlen)
		/* [한국어] 상한으로 자른다. */
		len = maxlen;

	/* [한국어] 메모리 공간이면, */
	if (flags & IORESOURCE_MEM)
		/* [한국어] 쓰기 결합 속성으로 매핑한다. CPU 가 연속된 쓰기를 모아 한 번에 내보낼 수 있어
		 * 프레임버퍼처럼 대역폭이 중요한 영역에 유리하지만, 쓰기 순서가 보장되지 않으므로
		 * 레지스터에는 쓰면 안 된다. */
		return ioremap_wc(start, len);

	/* What? */
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_iomap_wc_range);


/**
 * pci_iomap - create a virtual mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @maxlen: length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR without checking for its length first, pass %0 here.
 * */
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	/* [한국어] 오프셋 0 으로 고정해 범위 판에 위임한다. BAR 전체를 매핑하는 흔한 경우를 위한 편의 함수다. */
	return pci_iomap_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL(pci_iomap);


/**
 * pci_iomap_wc - create a virtual WC mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @maxlen: length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way. When possible write combining
 * is used.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR without checking for its length first, pass %0 here.
 * */
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	/* [한국어] 쓰기 결합 판도 같은 방식으로 오프셋 0 을 넘겨 위임한다. */
	return pci_iomap_wc_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL_GPL(pci_iomap_wc);


/*
 * pci_iounmap() somewhat illogically comes from lib/iomap.c for the
 * CONFIG_GENERIC_IOMAP case, because that's the code that knows about
 * the different IOMAP ranges.
 *
 * But if the architecture does not use the generic iomap code, and if
 * it has _not_ defined its own private pci_iounmap function, we define
 * it here.
 *
 * NOTE! This default implementation assumes that if the architecture
 * support ioport mapping (HAS_IOPORT_MAP), the ioport mapping will
 * be fixed to the range [ PCI_IOBASE, PCI_IOBASE+IO_SPACE_LIMIT [,
 * and does not need unmapping with 'ioport_unmap()'.
 *
 * If you have different rules for your architecture, you need to
 * implement your own pci_iounmap() that knows the rules for where
 * and how IO vs MEM get mapped.
 *
 * This code is odd, and the ARCH_HAS/ARCH_WANTS #define logic comes
 * from legacy <asm-generic/io.h> header file behavior. In particular,
 * it would seem to make sense to do the iounmap(p) for the non-IO-space
 * case here regardless, but that's not what the old header file code
 * did. Probably incorrectly, but this is meant to be bug-for-bug
 * compatible.
 */
#if defined(ARCH_WANTS_GENERIC_PCI_IOUNMAP)

/* [한국어]
 * pci_iounmap - pci_iomap 계열이 만든 매핑을 해제한다
 *
 * @dev: 매핑을 만든 장치. 이 구현에서는 쓰지 않지만, 아키텍처가 자체 구현을
 *       제공할 때 필요할 수 있어 인자로 남아 있다.
 * @p: 해제할 __iomem 쿠키.
 *
 * 왜 이런 모양인가: 위 영어 주석이 길게 설명하는 그대로다. 일반적으로
 * pci_iounmap() 은 CONFIG_GENERIC_IOMAP 인 경우 lib/iomap.c 가 제공한다 —
 * 그쪽이 IOMAP 범위 구분을 알고 있기 때문이다. 그러나 범용 iomap 코드를 쓰지도
 * 않고 자체 구현도 두지 않은 아키텍처를 위해, 여기에 기본 구현을 둔다.
 *
 * 핵심 가정: 아키텍처가 포트 매핑을 지원한다면(ARCH_HAS_GENERIC_IOPORT_MAP)
 * 그 매핑은 [PCI_IOBASE, PCI_IOBASE + IO_SPACE_LIMIT) 구간에 고정되어 있고
 * 별도 해제가 필요 없다. 그래서 쿠키가 그 구간 안이면 아무 일도 하지 않고
 * 돌아가고, 밖이면 진짜 MMIO 매핑이므로 iounmap() 한다.
 *
 * 상류가 스스로 인정하는 이상함: 마지막 영어 주석 문단이 밝히듯, 논리적으로는
 * 포트 구간이 아닐 때 항상 iounmap 하는 편이 맞아 보이지만 옛
 * <asm-generic/io.h> 의 동작이 그렇지 않았고, "아마 잘못됐겠지만 버그까지
 * 호환되게" 그 동작을 그대로 옮겨 왔다. 아키텍처마다 규칙이 다르면
 * 자체 pci_iounmap() 을 구현해야 한다.
 *
 * 실행 컨텍스트: 드라이버 정리 경로, 프로세스 컨텍스트. iounmap 이 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   드라이버의 remove/오류 경로 → [pci_iounmap] → (포트 구간이면 즉시 반환)
 *     → iounmap()
 */
void pci_iounmap(struct pci_dev *dev, void __iomem *p)
{
#ifdef ARCH_HAS_GENERIC_IOPORT_MAP
	/* [한국어] PCI_IOBASE 는 이 아키텍처에서 I/O 포트 공간이 매핑된 가상 주소의 시작이다.
	 * uintptr_t 로 캐스팅하는 것은 아래 범위 비교를 정수 연산으로 하기 위해서다. */
	uintptr_t start = (uintptr_t) PCI_IOBASE;
	/* [한국어] 해제하려는 포인터를 같은 정수 타입으로 바꾼다. */
	uintptr_t addr = (uintptr_t) p;

	/* [한국어] 위 영어 주석이 설명하는 전제 — 포트 매핑은 [PCI_IOBASE, PCI_IOBASE+IO_SPACE_LIMIT)
	 * 구간에 고정되어 있고 별도 해제가 필요 없다. 그 구간 안의 주소면 아무 일도 하지 않고 돌아간다.
	 * iounmap() 을 부르면 매핑하지 않은 것을 해제하려다 오류가 난다. */
	if (addr >= start && addr < start + IO_SPACE_LIMIT)
		return;
#endif
	iounmap(p);
}
EXPORT_SYMBOL(pci_iounmap);

#endif /* ARCH_WANTS_GENERIC_PCI_IOUNMAP */
