// SPDX-License-Identifier: GPL-2.0	/* NVMe: GPL-2.0 라이선스 선언. */
/*
 * Generic PCI resource mmap helper
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/mmap.c)은 PCI 장치의 BAR(Base Address Register)
 * 리소스를 사용자 공간 프로세스의 가상 주소 공간에 매핑하는 데 필요한
 * 핵심 헬퍼 함수들을 제공한다.
 *
 * NVMe SSD 입장에서는 이 파일이 직접 호출되는 경우는 드물지만, NVMe
 * 컨트롤러의 BAR0(일반적으로 MMIO register/doorbell 영역)과 같은
 * 리소스를 /sys/bus/pci/devices/.../resource{N} 또는 /proc/bus/pci
 * 인터페이스를 통해 사용자 공간으로 노출할 때 사용된다. 따라서 NVMe
 * 드라이버가 낮은 레벨에서 NVMe 컨트롤러의 레지스터와 doorbell에
 * 접근할 수 있도록 하는 데 필수적인 하위 레이어 역할을 한다.
 *
 * 주요 NVMe 관련 호출 경로:
 *   - 사용자 공간 mmap syscall
 *     -> /sys/bus/pci/devices/.../resourceN (pci-sysfs.c)
 *        -> pci_mmap_fits() : 요청한 VMA가 해당 NVMe BAR 영역 안에
 *           들어오는지 검사
 *        -> pci_mmap_resource_range() : 실제로 NVMe BAR의 물리 주소를
 *           사용자 프로세스 주소 공간에 매핑
 *     -> /proc/bus/pci/... (pci-proc.c)
 *        -> pci_mmap_fits() -> pci_mmap_resource_range()
 *
 * NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c)는 커널 내부에서
 * pci_iomap(), pci_resource_start(), ioremap() 등을 통해 BAR0/1을
 * 매핑하지만, 사용자 공간에서 동일한 NVMe BAR를 직접 접근해야 할
 * 때(예: SPDK, performance counter, debugging, userspace NVMe driver)
 * 본 파일의 함수가 동작한다. 이는 커널과 사용자 공간 양쪽에서 NVMe
 * register/doorbell을 다룰 수 있게 하는 공통 PCI mmap 인프라이다.
 *
 * NVMe BAR 특성 상 주로 Memory BAR(MEM 타입)이 사용되며, I/O BAR
 * (pci_mmap_io)는 거의 사용되지 않는다. writecombine과 device
 * 메모리 속성 설정은 NVMe doorbell region의 cacheability 특성과
 * 직결된다.
 *
 * 본 파일은 다음 두 함수를 제공한다:
 *   pci_mmap_resource_range() : 실제 BAR 리소스 mmap 수행
 *   pci_mmap_fits() : 요청 VMA가 지정된 PCI 리소스 영역에 맞는지 검사
 * ===================================================================
 */

#include <linux/kernel.h>	/* NVMe: 커널 기본 타입과 함수를 사용하기 위해 포함. */
#include <linux/mm.h>		/* NVMe: VMA, 페이지 보호, mmap 관련 구조체와 함수를 사용하기 위해 포함. */
#include <linux/pci.h>		/* NVMe: PCI 장치 구조체와 BAR 리소스 함수를 사용하기 위해 포함. */

#include "pci.h"		/* NVMe: PCI 서브시스템 내부 헤더(ARCH_GENERIC_PCI_MMAP_RESOURCE 등) 포함. */

#ifdef ARCH_GENERIC_PCI_MMAP_RESOURCE	/* NVMe: 아키텍처별 일반 PCI mmap 리소스 지원이 설정된 경우에만 이 섹션 컴파일. */
/* NVMe: ARCH_GENERIC_PCI_MMAP_RESOURCE가 정의된 아키텍처에서만 이 섹션을 컴파일한다. */

/*
 * pci_phys_vm_ops:
 *   mmap된 PCI 물리 영역에 대한 가상 메모리 동작 구조체이다.
 *   NVMe 입장에서는 사용자 공간에 노출된 NVMe BAR에 접근할 때(page fault,
 *   access permission 등) 커널이 처리할 콜백을 담고 있다.
 */
static const struct vm_operations_struct pci_phys_vm_ops = {	/* NVMe: mmap된 NVMe BAR 물리 영역의 가상 메모리 동작 구조체 정의. */
#ifdef CONFIG_HAVE_IOREMAP_PROT	/* NVMe: I/O 메모리 remap 보호 기능이 설정된 경우에만 access 콜백 활성화. */
	.access = generic_access_phys,	/* NVMe: 사용자 공간에서 NVMe BAR 물리 영역에 접근할 때 커널이 중재하는 access 콜백. */
#endif	/* NVMe: CONFIG_HAVE_IOREMAP_PROT가 설정된 경우에만 access 콜백 포함. */
};	/* NVMe: pci_phys_vm_ops 구조체 정의 종료. */

/*
 * pci_mmap_resource_range:
 *   지정된 PCI 장치의 특정 BAR를 사용자 프로세스의 VMA에 매핑한다.
 *   NVMe 관점에서는 /sys/bus/pci/devices/.../resource0 등을 통해 NVMe
 *   컨트롤러의 BAR0(MMIO register/doorbell 영역)를 사용자 공간으로
 *   노출할 때 이 함수가 호출된다. NVMe PCIe host driver가 커널 내에서
 *   ioremap()하는 것과 달리, 이 함수는 사용자 공간 mmap syscall의
 *   결과로 동작한다.
 */
int pci_mmap_resource_range(struct pci_dev *pdev,	/* NVMe: mmap 대상 NVMe 컨트롤러의 PCI 장치 구조체. */
			    int bar,			/* NVMe: 매핑할 NVMe BAR 번호(예: BAR0). */
			    struct vm_area_struct *vma,	/* NVMe: 사용자 프로세스의 가상 메모리 영역 구조체. */
			    enum pci_mmap_state mmap_state,	/* NVMe: I/O BAR 또는 Memory BAR 매핑 상태. */
			    int write_combine)	/* NVMe: 쓰기 결합(write-combine) 속성 사용 여부. */
{	/* NVMe: pci_mmap_resource_range 함수 본문 시작. */
	unsigned long size;	/* NVMe: 매핑 대상 NVMe BAR의 크기를 페이지 단위로 저장할 변수. */
	int ret;		/* NVMe: 하위 함수 호출 결과를 임시 저장할 변수. */

	/* NVMe: NVMe BAR의 바이트 길이를 페이지 개수로 변환(올림). */
	size = ((pci_resource_len(pdev, bar) - 1) >> PAGE_SHIFT) + 1;	/* NVMe: NVMe BAR 길이에서 1을 빼고 페이지 단위로 시프트한 뒤 1을 더해 페이지 수 계산. */
	/* NVMe: 요청한 mmap 영역(vm_pgoff 기준 시작 페이지 + 페이지 수)이 NVMe BAR 크기를 초과하는지 검사. */
	if (vma->vm_pgoff + vma_pages(vma) > size)	/* NVMe: 요청 시작 페이지와 요청 페이지 수의 합이 NVMe BAR 페이지 수보다 큰지 비교. */
		return -EINVAL;	/* NVMe: BAR 범위를 벗어나면 -EINVAL 반환. */

	/* NVMe: NVMe BAR 영역의 페이지 보호 속성을 설정한다. */
	if (write_combine)	/* NVMe: 호출자가 write_combine을 요청했는지 검사. */
		/* NVMe: write_combine이 요청되면 doorbell 등에 적합한 쓰기 결합 메모리 속성 사용. */
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);	/* NVMe: 기존 보호 속성을 쓰기 결합 속성으로 변환하여 doorbell 쓰기 성능 향상. */
	else	/* NVMe: write_combine이 아닌 경우 device 메모리 속성 사용 분기. */
		/* NVMe: 기본적으로는 device 메모리 속성(uncached, strongly ordered)로 설정. */
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);	/* NVMe: 기존 보호 속성을 device(uncached) 속성으로 변환하여 레지스터 접근 일관성 유지. */

	/* NVMe: mmap 대상이 I/O BAR인지 Memory BAR인지 구분. NVMe는 거의 Memory BAR을 사용. */
	if (mmap_state == pci_mmap_io) {	/* NVMe: mmap 상태가 I/O BAR인지 확인. */
		/* NVMe: I/O BAR인 경우 아키텍처별 PFN 설정을 수행한다. NVMe에는 해당 안 됨. */
		ret = pci_iobar_pfn(pdev, bar, vma);	/* NVMe: I/O BAR의 물리 페이지 번호(PFN)를 VMA에 설정하고 결과 반환. */
		if (ret)	/* NVMe: pci_iobar_pfn 호출 결과가 0이 아닌지 검사. */
			return ret;	/* NVMe: I/O BAR PFN 설정 실패 시 해당 오류를 그대로 반환. */
	} else	/* NVMe: Memory BAR 분기. NVMe BAR0는 일반적으로 이 경로를 탐. */
		/* NVMe: Memory BAR인 경우 BAR의 물리 시작 주소를 페이지 단위로 vm_pgoff에 더해 실제 PFN 산출. */
		vma->vm_pgoff += (pci_resource_start(pdev, bar) >> PAGE_SHIFT);	/* NVMe: NVMe BAR 물리 시작 주소를 PAGE_SHIFT로 나누어 페이지 오프셋에 누적. */

	/* NVMe: VMA의 vm_ops를 pci_phys_vm_ops로 설정해 NVMe BAR 접근 시 동작 정의. */
	vma->vm_ops = &pci_phys_vm_ops;	/* NVMe: 사용자 공간이 NVMe BAR에 접근할 때 사용할 가상 메모리 연산 구조체 연결. */

	/* NVMe: 최종적으로 io_remap_pfn_range를 호출해 NVMe BAR의 물리 페이지를 사용자 공간에 매핑. */
	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,	/* NVMe: VMA, 시작 가상 주소, 물리 페이지 오프셋을 io_remap_pfn_range에 전달. */
				vma->vm_end - vma->vm_start,	/* NVMe: 매핑할 가상 주소 영역의 크기(바이트) 계산. */
				vma->vm_page_prot);	/* NVMe: 설정된 페이지 보호 속성을 io_remap_pfn_range에 전달. */
}	/* NVMe: pci_mmap_resource_range 함수 종료. */

#endif	/* NVMe: ARCH_GENERIC_PCI_MMAP_RESOURCE 조걸 컴파일 종료. */

/* NVMe: sysfs/procfs 인터페이스와 PCI mmap 지원이 모두 활성화된 경우에만 이 섹션 컴파일. */
#if (defined(CONFIG_SYSFS) || defined(CONFIG_PROC_FS)) && \
    (defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE))	/* NVMe: PCI mmap 관련 지원이 설정된 경우에만 컴파일. */
/* NVMe: sysfs(/sys/bus/pci/devices/.../resourceN) 또는 procfs(/proc/bus/pci)가 활성화되고
 *       PCI mmap 지원이 설정되어 있을 때만 이 함수를 컴파일한다. */

/*
 * pci_mmap_fits:
 *   사용자 프로세스가 요청한 mmap 영역이 지정된 PCI 리소스(BAR) 범위 안에
 *   정확히 들어오는지 검증한다.
 *   NVMe 관점에서는 /sys/bus/pci/devices/.../resource0 등을 열어 NVMe
 *   BAR0의 일부 또는 전체를 mmap하려 할 때, 요청한 영역이 실제 NVMe
 *   컨트롤러가 노출한 BAR 크기 내에 있는지 확인하는 보안/정합성 검사다.
 *   이 검사를 통과해야 pci_mmap_resource_range()가 실제 매핑을 수행한다.
 */
int pci_mmap_fits(struct pci_dev *pdev,	/* NVMe: 검사 대상 NVMe 컨트롤러의 PCI 장치 구조체. */
		int resno,			/* NVMe: 검사할 NVMe BAR 리소스 번호. */
		struct vm_area_struct *vma,	/* NVMe: 사용자 프로세스의 mmap 요청 VMA. */
		enum pci_mmap_api mmap_api)	/* NVMe: sysfs 또는 procfs 인터페이스 출처 식별. */
{	/* NVMe: pci_mmap_fits 함수 본문 시작. */
	resource_size_t pci_start = 0, pci_end;	/* NVMe: BAR의 사용자 공간 기준 시작/끝 주소(바이트). procfs용. */
	unsigned long nr, start, size;		/* NVMe: 요청 페이지 수, 시작 페이지 오프셋, BAR 크기(페이지 수). */

	/* NVMe: 해당 NVMe BAR의 길이가 0이면 매핑 불가. */
	if (pci_resource_len(pdev, resno) == 0)	/* NVMe: 지정된 NVMe BAR 리소스 길이가 0인지 확인. */
		return 0;	/* NVMe: BAR가 존재하지 않으므로 fit 실패(0) 반환. */
	/* NVMe: mmap으로 요청한 총 페이지 수를 계산. */
	nr = vma_pages(vma);	/* NVMe: VMA의 시작과 끝 차이를 페이지 수로 변환하여 요청 크기 저장. */
	/* NVMe: mmap 요청의 시작 페이지 오프셋을 가져온다. */
	start = vma->vm_pgoff;	/* NVMe: 파일 오프셋 기반의 시작 페이지 번호를 start 변수에 저장. */
	/* NVMe: NVMe BAR의 길이를 페이지 수로 변환(올림)하여 허용 크기 계산. */
	size = ((pci_resource_len(pdev, resno) - 1) >> PAGE_SHIFT) + 1;	/* NVMe: NVMe BAR 길이를 페이지 단위 올림 처리하여 최대 허용 페이지 수 산출. */
	/* NVMe: procfs 인터페이스를 통한 mmap인 경우 사용자 공간 주소 기준을 다시 환산. */
	if (mmap_api == PCI_MMAP_PROCFS) {	/* NVMe: mmap API 출처가 procfs인지 확인. */
		/* NVMe: PCI bus 주소를 사용자 공간에 보이는 주소로 변환. */
		pci_resource_to_user(pdev, resno, &pdev->resource[resno],	/* NVMe: NVMe 장치의 해당 BAR 리소스 구조체 포인터 전달. */
				     &pci_start, &pci_end);	/* NVMe: 변환된 시작/끝 주소를 저장할 출력 변수 전달. */
		/* NVMe: 시작 주소를 페이지 단위로 변환. */
		pci_start >>= PAGE_SHIFT;	/* NVMe: procfs 기준 시작 주소를 페이지 인덱스로 변환. */
	}	/* NVMe: procfs 기준 변환 블록 종료. */
	/* NVMe: 요청 시작 페이지가 NVMe BAR 시작 이상이고, 요청 영역 전체가 BAR 끝 이하인지 검사. */
	if (start >= pci_start && start < pci_start + size &&	/* NVMe: 시작 페이지가 BAR 시작 이상이고 BAR 끝 미만인지 검사. */
	    start + nr <= pci_start + size)	/* NVMe: 시작 페이지에 요청 페이지 수를 더한 값이 BAR 끝 이하인지 검사. */
		return 1;	/* NVMe: mmap 요청 영역이 NVMe BAR 안에 완전히 포함되면 fit 성공(1) 반환. */
	return 0;		/* NVMe: 범위를 벗어나면 fit 실패(0) 반환. */
}	/* NVMe: pci_mmap_fits 함수 종료. */

#endif	/* NVMe: sysfs/procfs 및 PCI mmap 지원 조걸 컴파일 종료. */
