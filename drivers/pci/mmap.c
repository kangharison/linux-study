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
	/* [한국어] ptrace 나 /proc/<pid>/mem 처럼 커널이 다른 프로세스의 매핑을 대신 읽고 쓸 때
	 * 쓰이는 콜백. 일반 메모리와 달리 MMIO 는 그냥 memcpy 할 수 없어
	 * ioremap 을 거쳐야 하므로 전용 구현이 필요하다.
	 * CONFIG_HAVE_IOREMAP_PROT 가 꺼진 아키텍처에서는 이 필드가 아예 없어
	 * 그런 접근이 실패하게 된다 — 디버깅 편의를 위한 선택적 기능이다. */
	.access = generic_access_phys,
#endif
};

/* [한국어]
 * pci_mmap_resource_range - BAR 한 칸을 사용자 공간 vma 에 매핑한다
 *
 * @pdev: 매핑 대상 장치.
 * @bar: 매핑할 BAR 번호(0~5).
 * @vma: 커널이 준비한 사용자 매핑 서술자. vm_pgoff 에 BAR 안에서의
 *      페이지 오프셋이 담겨 들어온다.
 * @mmap_state: pci_mmap_io 면 I/O 공간, pci_mmap_mem 이면 메모리 공간.
 * @write_combine: 0 이 아니면 쓰기 결합(write-combining) 속성으로 매핑한다.
 *      프레임버퍼처럼 순서보다 대역폭이 중요한 영역에 쓴다.
 * @return: 0 = 성공. -EINVAL = 요청 범위가 BAR 를 벗어남.
 *      그 밖의 음수 = pci_iobar_pfn() 실패 또는 io_remap_pfn_range() 실패.
 *
 * 왜 필요한가: sysfs 의 resource<N> 파일이나 /proc/bus/pci 를 통해 사용자
 * 공간이 장치 BAR 를 직접 mmap 할 수 있게 해 주는 공통 구현이다. VFIO 나
 * SPDK 처럼 커널 드라이버를 거치지 않고 장치 레지스터를 두드리는 방식이
 * 이 경로에 의존한다. 아키텍처가 자체 구현을 제공하지 않을 때
 * (ARCH_GENERIC_PCI_MMAP_RESOURCE) 이 범용 판이 쓰인다.
 *
 * 동작 과정:
 *   1) BAR 길이를 페이지 수로 올림해 구하고, 요청 범위가 그 안에 들어가는지
 *      확인한다. 벗어나면 -EINVAL — 이 검사가 없으면 사용자가 BAR 밖의
 *      물리 메모리를 매핑할 수 있게 된다.
 *   2) 캐시 속성을 정한다. write_combine 이면 pgprot_writecombine(),
 *      아니면 pgprot_device() — 후자는 캐싱과 추측적 접근을 모두 막아
 *      MMIO 레지스터 접근에 안전한 속성이다.
 *   3) I/O 공간이면 pci_iobar_pfn() 으로 pfn 을 구한다. 그 함수는 이 파일에
 *      정의가 없고 아키텍처가 제공하며, 대부분의 아키텍처에서 I/O 공간은
 *      mmap 할 수 없어 실패를 돌려준다.
 *      메모리 공간이면 BAR 물리 주소를 페이지 번호로 바꿔 vm_pgoff 에 더한다 —
 *      들어올 때 BAR 상대 오프셋이던 값이 여기서 절대 pfn 이 된다.
 *   4) vm_ops 를 걸어 ptrace 등이 이 매핑을 읽을 수 있게 하고
 *      (CONFIG_HAVE_IOREMAP_PROT 일 때만 access 콜백이 채워진다),
 *      io_remap_pfn_range() 로 실제 페이지 테이블을 채운다.
 *
 * 실행 컨텍스트: mmap() 시스템 콜 문맥(프로세스 컨텍스트). mmap_lock 을
 * 쥔 상태로 불리므로 그 안에서 다른 메모리 락을 잡으면 안 된다.
 *
 * 에러 경로: 세 지점 모두 곧장 return 한다. 페이지 테이블을 채우기 전이라
 * 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   사용자 mmap("/sys/bus/pci/devices/<BDF>/resource0")
 *     → pci-sysfs.c 의 pci_mmap_resource() → [이 함수]
 *     → pci_iobar_pfn() / io_remap_pfn_range() */
int pci_mmap_resource_range(struct pci_dev *pdev, int bar,
			    struct vm_area_struct *vma,
			    enum pci_mmap_state mmap_state, int write_combine)
{
	unsigned long size;
	/* [한국어] io_remap_pfn_range() 와 pci_iobar_pfn() 의 반환값을 받을 변수. */
	int ret;

	/* [한국어] BAR 길이를 페이지 수로 올림한다. (len - 1) >> PAGE_SHIFT 에 1 을 더하는 것이
	 * 올림 나눗셈의 관용구다. len 이 0 이면 언더플로가 나지만, 그런 BAR 는
	 * 애초에 매핑 요청이 오지 않는다는 전제다. */
	size = ((pci_resource_len(pdev, bar) - 1) >> PAGE_SHIFT) + 1;
	/* [한국어] 요청 시작 페이지 + 요청 페이지 수가 BAR 페이지 수를 넘으면 범위를 벗어난 것이다.
	 * 이 검사가 없으면 사용자가 BAR 밖의 물리 메모리를 매핑할 수 있게 된다. */
	if (vma->vm_pgoff + vma_pages(vma) > size)
		return -EINVAL;

	/* [한국어] 쓰기 결합을 요청했는지 확인한다. */
	if (write_combine)
		/* [한국어] 쓰기 결합 속성 — CPU 가 여러 번의 쓰기를 모아 한 번에 내보내도 되는 매핑이다.
		 * 프레임버퍼처럼 순서보다 대역폭이 중요한 영역에 쓴다. */
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else
		/* [한국어] 기본은 장치 속성 — 캐싱도 추측적 접근도 없어 MMIO 레지스터에 안전하다.
		 * 레지스터를 쓰기 결합으로 매핑하면 쓰기 순서가 뒤바뀌어 하드웨어가 오동작한다. */
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	/* [한국어] I/O 포트 공간을 매핑하려는 요청인지 확인한다. */
	if (mmap_state == pci_mmap_io) {
		/* [한국어] pfn 계산을 아키텍처에 맡긴다. 이 함수는 이 파일에 정의가 없고, 대부분의
		 * 아키텍처에서 I/O 공간은 mmap 할 수 없어 실패를 돌려준다. */
		ret = pci_iobar_pfn(pdev, bar, vma);
		/* [한국어] 실패 검사. */
		if (ret)
			return ret;
	/* [한국어] 메모리 공간인 경우. */
	} else
		/* [한국어] BAR 의 물리 시작 주소를 페이지 번호로 바꿔 vm_pgoff 에 더한다.
		 * 이 한 줄이 의미의 전환점이다 — 들어올 때 "BAR 안에서의 상대 오프셋"이던
		 * vm_pgoff 가 여기서 "절대 물리 페이지 번호"가 되고, 아래
		 * io_remap_pfn_range() 는 그 절대값을 기대한다. */
		vma->vm_pgoff += (pci_resource_start(pdev, bar) >> PAGE_SHIFT);

	/* [한국어] ptrace 등이 이 매핑을 읽을 수 있도록 연산 테이블을 건다. */
	vma->vm_ops = &pci_phys_vm_ops;

	/* [한국어] 페이지 테이블을 실제로 채운다. io_remap_pfn_range 는 remap_pfn_range 의
	 * MMIO 판으로, 아키텍처가 필요로 하는 추가 속성을 함께 적용해 준다.
	 * 이 호출이 성공하면 사용자 공간에서 곧바로 장치 레지스터에 접근할 수 있다. */
	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				  vma->vm_end - vma->vm_start,
				  vma->vm_page_prot);
}

#endif

#if (defined(CONFIG_SYSFS) || defined(CONFIG_PROC_FS)) && \
    (defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE))

/* [한국어]
 * pci_mmap_fits - 요청한 vma 범위가 그 BAR 안에 온전히 들어가는지 판정한다
 *
 * @pdev: 대상 장치.
 * @resno: 검사할 resource[] 인덱스(BAR 번호).
 * @vma: 사용자가 요청한 매핑. vm_pgoff 와 페이지 수를 본다.
 * @mmap_api: PCI_MMAP_PROCFS 면 /proc 경로, PCI_MMAP_SYSFS 면 sysfs 경로.
 *      두 인터페이스가 vm_pgoff 의 의미를 다르게 쓰기 때문에 구분이 필요하다.
 * @return: 1 = 범위가 BAR 안에 들어감. 0 = 벗어남(길이 0 인 BAR 도 0).
 *      bool 이 아니라 int 이지만 의미는 참/거짓이다.
 *
 * 왜 필요한가: mmap 요청을 받아들이기 전에 경계를 검사하지 않으면, 사용자가
 * BAR 를 넘어선 물리 주소를 매핑해 다른 장치의 레지스터나 시스템 메모리를
 * 건드릴 수 있다. 이 함수가 그 관문이다.
 *
 * 두 인터페이스의 차이가 이 함수의 핵심이다. sysfs 의 resource<N> 파일은
 * BAR 시작을 0 으로 보는 상대 오프셋을 vm_pgoff 로 받으므로 기준이 0 이다.
 * 반면 /proc/bus/pci 는 역사적으로 사용자에게 보이는 절대 주소를 쓰므로,
 * pci_resource_to_user() 로 그 주소를 구해 기준을 옮겨야 한다. 그래서
 * PCI_MMAP_PROCFS 일 때만 pci_start 가 0 이 아닌 값으로 채워진다.
 *
 * 동작 과정:
 *   1) BAR 길이가 0 이면(구현되지 않은 BAR) 곧장 0.
 *   2) 요청 페이지 수와 시작 오프셋, BAR 의 페이지 수를 구한다.
 *   3) /proc 경로면 사용자에게 보이는 시작 주소를 페이지 번호로 바꿔 기준을 옮긴다.
 *   4) [start, start + nr) 이 [pci_start, pci_start + size) 안에 완전히
 *      들어가는지 세 조건으로 확인한다.
 *
 * 실행 컨텍스트: mmap() 시스템 콜 문맥. 상태를 바꾸지 않는 순수 판정 함수다.
 *
 * 에러 경로: 없다. 판정 결과만 돌려준다.
 *
 * 호출 체인:
 *   pci-sysfs.c 의 pci_mmap_resource() / proc.c 의 proc_bus_pci_mmap()
 *     → [pci_mmap_fits] → pci_resource_to_user() (PROCFS 인 경우만) */
int pci_mmap_fits(struct pci_dev *pdev, int resno, struct vm_area_struct *vma,
		  enum pci_mmap_api mmap_api)
{
	resource_size_t pci_start = 0, pci_end;
	/* [한국어] nr: 요청 페이지 수. start: 요청 시작 페이지. size: BAR 의 페이지 수. */
	unsigned long nr, start, size;

	/* [한국어] 구현되지 않은 BAR(길이 0)는 매핑할 수 없다. */
	if (pci_resource_len(pdev, resno) == 0)
		return 0;
	/* [한국어] vma 가 덮는 페이지 수를 구한다. */
	nr = vma_pages(vma);
	/* [한국어] 요청 시작 페이지 번호. 이 값의 기준이 인터페이스마다 다르다는 것이
	 * 이 함수가 mmap_api 를 인자로 받는 이유다. */
	start = vma->vm_pgoff;
	/* [한국어] BAR 길이를 페이지 수로 올림한다. */
	size = ((pci_resource_len(pdev, resno) - 1) >> PAGE_SHIFT) + 1;
	/* [한국어] /proc 경로인 경우에만 기준을 옮겨야 한다. */
	if (mmap_api == PCI_MMAP_PROCFS) {
		/* [한국어] 사용자에게 보이는 주소를 구한다. 아키텍처에 따라 물리 주소와 다를 수 있어
		 * (예: 버스 주소를 그대로 노출하는 플랫폼) 코어 헬퍼에 위임한다. */
		pci_resource_to_user(pdev, resno, &pdev->resource[resno],
				     &pci_start, &pci_end);
		/* [한국어] 바이트 주소를 페이지 번호로 바꾼다. 이제 start 와 같은 단위가 된다.
		 * sysfs 경로에서는 이 블록을 건너뛰므로 pci_start 가 0 으로 남고,
		 * 결과적으로 "BAR 시작을 0 으로 보는" 비교가 된다. */
		pci_start >>= PAGE_SHIFT;
	}
	/* [한국어] 세 조건을 모두 만족해야 한다 — 시작이 BAR 안이고, 시작이 BAR 끝 이전이고,
	 * 끝까지 BAR 안에 들어간다. 두 번째 조건은 size 가 0 인 경우를 걸러 내는
	 * 역할도 겸한다. */
	if (start >= pci_start && start < pci_start + size &&
	    start + nr <= pci_start + size)
		/* [한국어] 범위가 온전히 들어감. */
		return 1;
	return 0;
}

#endif
