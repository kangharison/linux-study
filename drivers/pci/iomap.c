// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the default iomap interfaces
 *
 * (C) Copyright 2004 Linus Torvalds
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/iomap.c)은 PCI 장치의 BAR(Base Address Register)를
 * 커널 가상 주소 공간에 매핑(iomap)하고 해제(iounmap)하는 기본 인터페이스를
 * 구현한다.
 * NVMe SSD는 PCIe endpoint로서, 호스트 드라이버가 NVMe controller registers와
 * doorbell registers에 접근하려면 BAR(보통 BAR0)를 CPU가 볼 수 있는 가상
 * 주소로 매핑해야 한다. 본 파일은 그 매핑/해제의 핵심 경로를 담당한다.
 *   - pci_iomap(): NVMe BAR 전체 또는 일부를 일반 메모리 속성으로 매핑
 *   - pci_iomap_wc(): write combining 속성으로 매핑(doorbell 쓰기 성능 향상)
 *   - pci_iounmap(): NVMe 제거/ suspend 시 매핑 해제
 * 일반적인 NVMe 드라이버 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_request_regions ->
 *   pci_iomap(pdev, 0, ...) -> doorbell/register MMIO access
 *   nvme_remove / nvme_suspend -> pci_iounmap -> 매핑 해제
 * DMA, MSI-X, IRQ, 전원 관리와의 연관성:
 *   - BAR 매핑은 DMA 버퍼 매핑과는 별개이나, NVMe submission/completion
 *     queue의 물리 주소를 BAR를 통해 설정하고 doorbell로 활성화한다.
 *   - MSI-X table이 BAR에 위치한 경우 BAR 매핑을 통해 MSI-X table에
 *     접근할 수 있다.
 *   - ASPM/전원 관리 시 BAR는 unmapped 되었다가 resume 시 다시 매핑된다.
 * 본 파일은 drivers/nvme/host/pci.c에서 직접 또는 pci_iomap() 등을 통해
 * 간접적으로 사용되며, NVMe 드라이버의 register/doorbell 접근의 기반이 된다.
 * ===================================================================
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
void __iomem *pci_iomap_range(struct pci_dev *dev,		/* NVMe: 대상 NVMe PCIe device */
			      int bar,			/* NVMe: 매핑할 BAR 번호(보통 0) */
			      unsigned long offset,	/* NVMe: BAR 내 시작 오프셋 */
			      unsigned long maxlen)	/* NVMe: 매핑 최대 길이(0이면 offset부터 끝까지) */
{
	resource_size_t start, len;				/* NVMe: BAR의 bus 물리 시작 주소와 길이 */
	unsigned long flags;					/* NVMe: BAR 리소스 플래그(MEM/IO 등) */

	if (!pci_bar_index_is_valid(bar))			/* NVMe: BAR 번호가 유효한지 검사 */
		return NULL;					/* NVMe: 잘못된 BAR 번호이면 매핑 실패 */

	start = pci_resource_start(dev, bar);			/* NVMe: NVMe BAR의 시작 bus 주소 획득 */
	len = pci_resource_len(dev, bar);			/* NVMe: NVMe BAR의 전체 길이 획득 */
	flags = pci_resource_flags(dev, bar);			/* NVMe: BAR가 MMIO인지 PIO인지 등 플래그 획득 */

	if (len <= offset || !start)				/* NVMe: 오프셋이 길이 이상이거나 시작 주소가 0이면 */
		return NULL;					/* NVMe: 유효하지 않은 BAR 영역이므로 매핑 실패 */

	len -= offset;						/* NVMe: offset 이후의 실제 매핑 가능 길이 계산 */
	start += offset;					/* NVMe: 시작 주소를 offset만큼 이동 */
	if (maxlen && len > maxlen)				/* NVMe: 요청한 최대 길이가 있고 실제 길이보다 짧으면 */
		len = maxlen;					/* NVMe: 매핑 길이를 요청한 최대 길이로 제한 */
	if (flags & IORESOURCE_IO)				/* NVMe: BAR가 I/O 포트 공간이면 */
		return __pci_ioport_map(dev, start, len);	/* NVMe: I/O 포트 매핑 수행(레거시) */
	if (flags & IORESOURCE_MEM)				/* NVMe: BAR가 메모리 매핑 공간이면 */
		return ioremap(start, len);			/* NVMe: 물리 주소를 커널 가상 주소로 매핑(MMIO) */
	/* What? */
	return NULL;						/* NVMe: 알 수 없는 리소스 타입이면 매핑 실패 */
}
EXPORT_SYMBOL(pci_iomap_range);					/* NVMe: NVMe 모듈 등에서 참조 가능하도록 EXPORT */

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
void __iomem *pci_iomap_wc_range(struct pci_dev *dev,		/* NVMe: 대상 NVMe PCIe device */
				 int bar,			/* NVMe: 매핑할 BAR 번호 */
				 unsigned long offset,		/* NVMe: BAR 내 시작 오프셋 */
				 unsigned long maxlen)		/* NVMe: 매핑 최대 길이 */
{
	resource_size_t start, len;				/* NVMe: BAR의 bus 물리 시작 주소와 길이 */
	unsigned long flags;					/* NVMe: BAR 리소스 플래그 */

	if (!pci_bar_index_is_valid(bar))			/* NVMe: BAR 번호가 유효한지 검사 */
		return NULL;					/* NVMe: 잘못된 BAR 번호이면 매핑 실패 */

	start = pci_resource_start(dev, bar);			/* NVMe: NVMe BAR의 시작 bus 주소 획득 */
	len = pci_resource_len(dev, bar);			/* NVMe: NVMe BAR의 전체 길이 획득 */
	flags = pci_resource_flags(dev, bar);			/* NVMe: BAR 플래그 획득 */

	if (len <= offset || !start)				/* NVMe: 오프셋이 길이 이상이거나 시작 주소가 0이면 */
		return NULL;					/* NVMe: 유효하지 않은 BAR 영역이므로 매핑 실패 */
	if (flags & IORESOURCE_IO)				/* NVMe: BAR가 I/O 포트 공간이면 */
		return NULL;					/* NVMe: WC는 MMIO에만 적용되므로 매핑 실패 */

	len -= offset;						/* NVMe: offset 이후의 실제 매핑 가능 길이 계산 */
	start += offset;					/* NVMe: 시작 주소를 offset만큼 이동 */
	if (maxlen && len > maxlen)				/* NVMe: 요청한 최대 길이가 있고 실제 길이보다 짧으면 */
		len = maxlen;					/* NVMe: 매핑 길이를 요청한 최대 길이로 제한 */

	if (flags & IORESOURCE_MEM)				/* NVMe: BAR가 메모리 매핑 공간이면 */
		return ioremap_wc(start, len);			/* NVMe: WC 속성으로 물리 주소를 커널 가상 주소로 매핑 */

	/* What? */
	return NULL;						/* NVMe: 알 수 없는 리소스 타입이면 매핑 실패 */
}
EXPORT_SYMBOL_GPL(pci_iomap_wc_range);				/* NVMe: NVMe 모듈 등에서 GPL EXPORT로 참조 */

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
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)	/* NVMe: NVMe device, BAR 번호, 최대 길이 */
{
	return pci_iomap_range(dev, bar, 0, maxlen);		/* NVMe: offset 0으로 BAR 시작부터 maxlen까지 매핑 */
}
EXPORT_SYMBOL(pci_iomap);						/* NVMe: NVMe 모듈 등에서 참조 가능하도록 EXPORT */

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
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen)	/* NVMe: NVMe device, BAR 번호, 최대 길이 */
{
	return pci_iomap_wc_range(dev, bar, 0, maxlen);		/* NVMe: offset 0으로 BAR 시작부터 WC 매핑 */
}
EXPORT_SYMBOL_GPL(pci_iomap_wc);					/* NVMe: NVMe 모듈 등에서 GPL EXPORT로 참조 */

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
#if defined(ARCH_WANTS_GENERIC_PCI_IOUNMAP)				/* NVMe: 아키텍처가 generic pci_iounmap를 원할 때만 컴파일 */

void pci_iounmap(struct pci_dev *dev, void __iomem *p)			/* NVMe: NVMe device와 매핑된 가상 주소 */
{
#ifdef ARCH_HAS_GENERIC_IOPORT_MAP					/* NVMe: I/O 포트 매핑도 generic으로 관리하는 아키텍처인 경우 */
	uintptr_t start = (uintptr_t) PCI_IOBASE;			/* NVMe: I/O 공간의 커널 기준 시작 주소 */
	uintptr_t addr = (uintptr_t) p;					/* NVMe: 해제할 가상 주소 */

	if (addr >= start && addr < start + IO_SPACE_LIMIT)		/* NVMe: 해당 주소가 I/O 포트 매핑 영역이면 */
		return;							/* NVMe: I/O 포트 매핑은 별도 해제가 필요 없으므로 종료 */
#endif
	iounmap(p);							/* NVMe: 메모리 매핑(MMIO) 해제: NVMe BAR 가상 주소 반납 */
}
EXPORT_SYMBOL(pci_iounmap);						/* NVMe: NVMe 모듈 등에서 참조 가능하도록 EXPORT */

#endif /* ARCH_WANTS_GENERIC_PCI_IOUNMAP */
