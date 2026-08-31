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

/*
 * pci_iomap_range:
 *   NVMe controller의 특정 BAR(주로 BAR0) 내 offset 위치부터 maxlen 길이만큼
 *   커널 가상 주소 공간으로 매핑한다. NVMe 드라이버는 이를 통해 controller
 *   registers와 doorbell registers에 MMIO로 접근한다.
 */

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
	unsigned long flags;

	if (!pci_bar_index_is_valid(bar))
		return NULL;

	start = pci_resource_start(dev, bar);
	len = pci_resource_len(dev, bar);
	flags = pci_resource_flags(dev, bar);

	if (len <= offset || !start)
		return NULL;

	len -= offset;
	start += offset;
	if (maxlen && len > maxlen)
		len = maxlen;
	if (flags & IORESOURCE_IO)
		return __pci_ioport_map(dev, start, len);
	if (flags & IORESOURCE_MEM)
		return ioremap(start, len);
	/* What? */
	return NULL;
}
EXPORT_SYMBOL(pci_iomap_range);

/*
 * pci_iomap_wc_range:
 *   pci_iomap_range()와 유사하되 write combining(WC) 속성으로 매핑한다.
 *   NVMe doorbell 쓰기는 빈번하므로 WC 매핑이 사용 가능한 경우 메모리
 *   쓰기 성능을 높일 수 있다. PIO 공간은 WC를 지원하지 않아 NULL을 반환한다.
 */

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
	unsigned long flags;

	if (!pci_bar_index_is_valid(bar))
		return NULL;

	start = pci_resource_start(dev, bar);
	len = pci_resource_len(dev, bar);
	flags = pci_resource_flags(dev, bar);

	if (len <= offset || !start)
		return NULL;
	if (flags & IORESOURCE_IO)
		return NULL;

	len -= offset;
	start += offset;
	if (maxlen && len > maxlen)
		len = maxlen;

	if (flags & IORESOURCE_MEM)
		return ioremap_wc(start, len);

	/* What? */
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_iomap_wc_range);

/*
 * pci_iomap:
 *   NVMe BAR의 처음부터 maxlen까지 일반 메모리 속성으로 매핑한다.
 *   pci_iomap_range()에 offset 0을 전달하는 단순 래퍼로,
 *   NVMe 드라이버가 BAR0 전체를 매핑할 때 가장 자주 사용된다.
 */

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
	return pci_iomap_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL(pci_iomap);

/*
 * pci_iomap_wc:
 *   NVMe BAR의 처음부터 maxlen까지 write combining 속성으로 매핑한다.
 *   pci_iomap_wc_range()에 offset 0을 전달하는 래퍼이다.
 */

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
	return pci_iomap_wc_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL_GPL(pci_iomap_wc);

/*
 * pci_iounmap:
 *   NVMe 제거, suspend, 재설정 시 pci_iomap()으로 매핑한 BAR 가상 주소를
 *   해제한다. 이후 doorbell/register 접근이 불가능하므로 NVMe 드라이버는
 *   먼저 controller를 정지한 후 호출해야 한다.
 */

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

void pci_iounmap(struct pci_dev *dev, void __iomem *p)
{
#ifdef ARCH_HAS_GENERIC_IOPORT_MAP
	uintptr_t start = (uintptr_t) PCI_IOBASE;
	uintptr_t addr = (uintptr_t) p;

	if (addr >= start && addr < start + IO_SPACE_LIMIT)
		return;
#endif
	iounmap(p);
}
EXPORT_SYMBOL(pci_iounmap);

#endif /* ARCH_WANTS_GENERIC_PCI_IOUNMAP */
