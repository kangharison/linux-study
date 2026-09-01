// SPDX-License-Identifier: GPL-2.0
/* [한국어] 위 SPDX 줄은 커널의 라이선스 표기 규약이다. 파일 첫 줄에
 * 정확한 형식으로만 있어야 scripts/spdxcheck.py 가 인식하므로,
 * 그 줄에는 어떤 내용도 덧붙이지 않는다. */
/*
 * Generic PCI resource mmap helper
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 */

/*
 * [한국어 설명] BAR 를 userspace 주소 공간에 매핑하는 공통 구현 (mmap.c)
 *
 * === 파일의 역할 ===
 * userspace 가 장치의 BAR 를 직접 매핑해 쓰는 경로를 제공한다.
 * sysfs 의 resource<N> 파일이나 /proc/bus/pci 의 장치 파일을 mmap 하면
 * 결국 이 파일의 pci_mmap_resource_range() 로 들어온다.
 *
 * 하는 일은 크게 셋이다.
 *   1) 매핑 종류 결정 - I/O 공간인가 메모리 공간인가. I/O 공간은 대부분의
 *      아키텍처에서 mmap 할 수 없어 거절한다.
 *   2) 캐시 속성 설정 - MMIO 는 캐시하면 안 되므로 페이지 속성을
 *      uncached 로 만든다. 다만 write-combining 을 요청하면 그쪽으로 한다.
 *   3) 실제 매핑 - io_remap_pfn_range() 로 페이지 테이블을 채운다.
 *
 * 위험한 기능이라 여러 겹의 방어가 있다. sysfs 쪽은 CAP_SYS_RAWIO 를
 * 요구하고, 매핑 범위가 그 BAR 안에 들어가는지 확인한다. 그럼에도
 * userspace 가 레지스터를 직접 만지게 되므로, VFIO 처럼 IOMMU 로 격리된
 * 환경이 아니면 시스템을 망가뜨릴 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * userspace -> mmap("/sys/bus/pci/devices/.../resource0")
 *   -> pci-sysfs.c 의 pci_mmap_resource()
 *      -> 권한과 범위 확인
 *      -> [이 파일] pci_mmap_resource_range(pdev, bar, vma, mmap_state, wc)
 *         -> pgprot 를 uncached 또는 write-combining 으로 설정
 *         -> io_remap_pfn_range() 로 페이지 테이블 채우기
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(mmap 시스템 호출). mmap_lock 을
 * 쥔 상태로 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sysfs.c(resource<N> 속성), proc.c(/proc/bus/pci mmap).
 * 아래쪽: mm 의 io_remap_pfn_range, 아키텍처별 pgprot 헬퍼.
 * 옆쪽: 아키텍처가 덮어쓸 수 있는 pci_iobar_pfn() — I/O 공간을 mmap 할
 *   수 있는 소수의 아키텍처가 그 변환을 제공한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 관련이 없다(전수 확인).
 *
 * 다만 NVMe 를 userspace 드라이버로 다루는 경우 — SPDK 가 대표적이다 —
 * 이 경로가 쓰인다. SPDK 는 VFIO 나 uio 로 NVMe 컨트롤러를 커널에서
 * 떼어 내고, BAR0 를 userspace 에 매핑해 도어벨을 직접 두드린다.
 * 커널 NVMe 드라이버를 거치지 않으므로 컨텍스트 스위치와 시스템 호출
 * 비용이 사라져 지연이 크게 줄어든다.
 *
 * 그 매핑의 실제 구현이 VFIO 를 거치면 drivers/vfio/pci 쪽이고,
 * sysfs resource 파일을 거치면 이 파일이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_mmap_resource_range() : 매핑의 본체. 종류를 판별하고 캐시 속성을
 *                             정한 뒤 페이지 테이블을 채운다.
 * pci_iobar_pfn()           : I/O 공간 BAR 의 pfn 을 구한다. 대부분의
 *                             아키텍처에서는 불가능해 실패를 돌려준다.
 *                             이 파일에는 정의가 없고 호출만 있다(:101) —
 *                             아키텍처 코드가 제공하는 심볼이다.
 * pci_mmap_fits()           : 요청한 vma 범위가 그 BAR 안에 온전히 들어가는지
 *                             판정한다. mmap_api 가 PCI_MMAP_PROCFS 면
 *                             pci_resource_to_user() 로 사용자에게 보이는
 *                             주소로 바꿔 비교하고, PCI_MMAP_SYSFS 면
 *                             오프셋 0 기준으로 비교한다 — 두 인터페이스가
 *                             vm_pgoff 의 의미를 다르게 쓰기 때문이다.
 *                             1 = 들어감, 0 = 벗어남(길이 0 인 BAR 도 0).
 * (기존 요약에는 pci_mmap_page_range() 가 올라 있었으나 그런 이름의 함수는
 *  이 파일에도 drivers/pci 어디에도 없다 — 전수 grep 으로 확인했다.
 *  이 파일이 정의하는 함수는 위 두 개뿐이며, 그나마 각각
 *  ARCH_GENERIC_PCI_MMAP_RESOURCE 와 SYSFS/PROC_FS 조건부로 컴파일된다.)
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "pci.h"

#ifdef ARCH_GENERIC_PCI_MMAP_RESOURCE

static const struct vm_operations_struct pci_phys_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

int pci_mmap_resource_range(struct pci_dev *pdev, int bar,
			    struct vm_area_struct *vma,
			    enum pci_mmap_state mmap_state, int write_combine)
{
	unsigned long size;
	int ret;

	size = ((pci_resource_len(pdev, bar) - 1) >> PAGE_SHIFT) + 1;
	if (vma->vm_pgoff + vma_pages(vma) > size)
		return -EINVAL;

	if (write_combine)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	if (mmap_state == pci_mmap_io) {
		ret = pci_iobar_pfn(pdev, bar, vma);
		if (ret)
			return ret;
	} else
		vma->vm_pgoff += (pci_resource_start(pdev, bar) >> PAGE_SHIFT);

	vma->vm_ops = &pci_phys_vm_ops;

	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				  vma->vm_end - vma->vm_start,
				  vma->vm_page_prot);
}

#endif

#if (defined(CONFIG_SYSFS) || defined(CONFIG_PROC_FS)) && \
    (defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE))

int pci_mmap_fits(struct pci_dev *pdev, int resno, struct vm_area_struct *vma,
		  enum pci_mmap_api mmap_api)
{
	resource_size_t pci_start = 0, pci_end;
	unsigned long nr, start, size;

	if (pci_resource_len(pdev, resno) == 0)
		return 0;
	nr = vma_pages(vma);
	start = vma->vm_pgoff;
	size = ((pci_resource_len(pdev, resno) - 1) >> PAGE_SHIFT) + 1;
	if (mmap_api == PCI_MMAP_PROCFS) {
		pci_resource_to_user(pdev, resno, &pdev->resource[resno],
				     &pci_start, &pci_end);
		pci_start >>= PAGE_SHIFT;
	}
	if (start >= pci_start && start < pci_start + size &&
	    start + nr <= pci_start + size)
		return 1;
	return 0;
}

#endif
