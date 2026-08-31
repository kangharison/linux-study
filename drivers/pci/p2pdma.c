// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Peer 2 Peer DMA support.
 *
 * Copyright (c) 2016-2018, Logan Gunthorpe
 * Copyright (c) 2016-2017, Microsemi Corporation
 * Copyright (c) 2017, Christoph Hellwig
 * Copyright (c) 2018, Eideticom Inc.
 */

/*
 * [한국어 설명] 장치끼리 호스트 메모리를 거치지 않고 직접 DMA 하게 해 주는 계층 (p2pdma.c)
 *
 * === 파일의 역할 ===
 * 보통의 DMA 는 장치가 호스트 메모리를 읽고 쓴다. P2PDMA(Peer-to-Peer DMA)는
 * 그 대신 한 PCI 장치가 다른 PCI 장치의 메모리를 직접 읽고 쓰게 한다.
 * NVMe SSD 의 CMB 에 있는 데이터를 네트워크 카드가 곧바로 가져가는 식이다.
 * 호스트 메모리를 한 번 거치지 않으므로 지연이 줄고 메모리 대역폭이 절약된다.
 *
 * 이 파일이 하는 일은 셋이다.
 *
 *   1) 제공자(provider) 등록 - pci_p2pdma_add_resource() 가 어떤 장치의 BAR
 *      일부를 "다른 장치가 쓸 수 있는 메모리" 로 등록한다. 그 구간을
 *      genalloc(범용 할당자) 풀로 감싸 조각내어 나눠 줄 수 있게 만든다.
 *
 *   2) 경로 판정 - pci_p2pdma_map_type() 이 "이 두 장치 사이에 직접 경로가
 *      성립하는가" 를 판정한다. 이것이 이 파일에서 가장 어려운 부분이며,
 *      아래 별도 항목으로 설명한다.
 *
 *   3) 할당 - pci_alloc_p2pmem() / pci_free_p2pmem() 이 등록된 풀에서
 *      메모리를 떼어 주고 돌려받는다.
 *
 * === 경로 판정이 왜 어려운가 ===
 * 두 장치가 같은 PCIe 스위치 아래 있으면 스위치가 트랜잭션을 곧바로 옆으로
 * 넘길 수 있어 직접 경로가 성립한다. 하지만 다음 세 가지가 걸림돌이다.
 *
 *   - Root Complex 를 거쳐야 하는 경우: 많은 Root Complex 가 P2P 트랜잭션을
 *     제대로 전달하지 못한다. 그래서 이 파일은 검증된 호스트 브리지 목록
 *     (화이트리스트)을 들고 있고, 목록에 없으면 직접 경로를 허용하지 않는다.
 *   - ACS(Access Control Services) redirect: 격리를 위해 스위치가 모든
 *     트랜잭션을 위로 올려보내도록 설정돼 있으면, 옆으로 가는 지름길이
 *     막힌다. 그래서 P2PDMA 를 쓰려면 경로상의 ACS 재지향을 꺼야 하고,
 *     그것은 IOMMU 격리를 약화시킨다 — 성능과 보안의 맞교환이다.
 *   - IOMMU: 켜져 있으면 장치가 내는 주소가 IOVA 라 상대 장치의 실제
 *     BAR 주소와 다르다. 그래서 매핑 종류(BUS_ADDR / THRU_HOST_BRIDGE)를
 *     구분해 각각 다르게 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  NVMe 등 provider 드라이버
 *          -> [이 파일] pci_p2pdma_add_resource()
 *             -> devm_memremap_pages() 로 그 BAR 구간에 struct page 를 만든다
 *             -> gen_pool 로 감싸 할당 가능하게 한다
 *          -> pci_p2pmem_publish() 로 "남들이 써도 된다" 고 표시
 *
 * 사용:  소비자 드라이버(또는 같은 장치 자신)
 *          -> [이 파일] pci_alloc_p2pmem() 으로 한 조각을 얻고
 *          -> pci_p2pmem_virt_to_bus() 로 그 조각의 PCI 버스 주소를 구해
 *          -> 장치의 DMA 엔진에 그 주소를 넘긴다
 *
 * 매핑:  dma_map_sg 계열
 *          -> [이 파일] pci_p2pdma_map_type() 으로 경로를 판정하고
 *             BUS_ADDR 이면 IOMMU 를 거치지 않는 직행 주소를 쓴다
 *
 * 실행 컨텍스트: 등록과 해제는 프로세스 컨텍스트(메모리 할당과 devm 등록).
 * 할당(pci_alloc_p2pmem)은 gen_pool 이 내부 락만 쓰므로 더 가볍지만,
 * 역시 프로세스 컨텍스트에서 쓰는 것을 전제로 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: NVMe(CMB), RDMA NIC, GPU 드라이버.
 * 아래쪽: mm/memremap.c 의 devm_memremap_pages()(BAR 구간에 struct page 를
 *   만들어 커널 메모리처럼 다룰 수 있게 한다), lib/genalloc.c(조각 할당),
 *   pci.c 의 ACS 설정 조회, host-bridge.c 의 주소 변환.
 * 옆쪽: dma-mapping 계층. P2PDMA 를 쓰는 DMA 요청은 결국 그쪽을 지나며,
 *   pci_p2pdma_map_type() 의 판정 결과에 따라 처리가 갈린다.
 * 공유 상태: struct pci_dev 의 p2pdma 포인터(RCU 로 보호되는
 *   struct pci_p2pdma). 그 안에 gen_pool 과 published 플래그가 있다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 는 이 파일의 함수를 다섯 개 직접 부른다. drivers/pci 에서
 * 이만큼 직접 얽힌 파일은 드물다. CMB(Controller Memory Buffer)가
 * P2PDMA 로 노출되기 때문이다.
 *
 *   nvme_map_cmb()                      [drivers/nvme/host/pci.c]
 *     CMBSZ/CMBLOC 레지스터로 CMB 의 크기와 위치(어느 BAR 의 어느 오프셋)를
 *     읽은 뒤:
 *       -> pci_p2pdma_add_resource(pdev, bar, size, offset)
 *          그 BAR 구간을 P2PDMA 풀로 등록한다. 실패하면 CMBMSC 를 0 으로
 *          되돌려 CMB 자체를 포기한다.
 *       -> pci_p2pmem_publish(pdev, true)
 *          다른 장치도 이 CMB 를 쓸 수 있다고 공개한다.
 *
 *   nvme_alloc_sq()                     [큐 하나를 만들 때마다]
 *       -> pci_alloc_p2pmem(pdev, SQ_SIZE(nvmeq))
 *          Submission Queue 링을 호스트 메모리가 아니라 CMB 에 잡는다.
 *       -> pci_p2pmem_virt_to_bus(pdev, nvmeq->sq_cmds)
 *          그 주소를 Create SQ 명령에 넣을 PCI 버스 주소로 바꾼다.
 *       -> 실패하면 pci_free_p2pmem() 으로 되돌리고 dma_alloc_coherent()
 *          (호스트 메모리)로 폴백한다.
 *
 *   nvme_free_queue()  -> pci_free_p2pmem()
 *
 * SQ 를 CMB 에 두면 무엇이 좋은가. 보통은 호스트가 SQ 엔트리를 호스트
 * 메모리에 쓰고, 도어벨을 두드리면 컨트롤러가 그것을 DMA 로 읽어 간다.
 * SQ 가 CMB(컨트롤러 안)에 있으면 호스트가 직접 써 넣으므로 그 읽기 DMA 가
 * 통째로 사라진다. use_cmb_sqes 모듈 파라미터로 켜고 끌 수 있다.
 *
 * 그리고 NVMe 는 enum pci_p2pdma_map_type 을 직접 쓴다(pci.c 의 두 곳).
 * I/O 버퍼가 P2P 메모리인지 판정해 unmap 방식을 가르는 데 쓴다.
 *
 * (기존 주석은 호출 경로로 "nvme_probe -> nvme_setup_pci_p2pdma" 를
 *  적었으나 nvme_setup_pci_p2pdma 라는 함수는 drivers/nvme/ 에 없다.
 *  실제 진입점은 nvme_map_cmb() 다. 또 "nvme_setup_prps/sgl 이
 *  pci_p2pdma_map_type 을 부른다", "dma_pci_p2pdma_supported 가
 *  pci_p2pdma_distance_many 를 부른다" 고 적었으나 그 함수 이름들도
 *  NVMe 쪽에 없다. 위 검증 결과로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_p2pdma_add_resource()   : BAR 의 한 구간을 P2P 메모리 풀로 등록한다.
 *                               devm_memremap_pages() 로 struct page 를 만들고
 *                               gen_pool 로 감싸는 것이 핵심이다.
 * pci_p2pmem_publish()        : 그 풀을 다른 장치에게 공개할지 표시한다.
 *                               공개하지 않으면 자기 자신만 쓴다.
 * pci_alloc_p2pmem()          : 풀에서 한 조각을 떼어 커널 가상 주소로 준다.
 * pci_free_p2pmem()           : 돌려준다.
 * pci_p2pmem_virt_to_bus()    : 그 가상 주소에 대응하는 PCI 버스 주소.
 *                               장치의 DMA 엔진에 넣을 값이다.
 * pci_p2pdma_map_type()       : 두 장치 사이의 매핑 종류를 판정한다.
 *                               NOT_SUPPORTED / BUS_ADDR / THRU_HOST_BRIDGE.
 * pci_p2pdma_distance_many()  : 여러 소비자에 대해 provider 까지의 거리를
 *                               재고, 그중 가장 나쁜 경우를 돌려준다.
 * calc_map_type_and_dist()    : 실제 판정 로직. 두 장치의 공통 상위를 찾고,
 *                               그 경로가 스위치인지 호스트 브리지인지,
 *                               ACS 가 어떻게 설정됐는지를 본다.
 * struct pci_p2pdma           : provider 장치에 매달리는 상태. gen_pool 과
 *                               published 플래그를 갖는다. RCU 로 보호된다.
 */

#define pr_fmt(fmt) "pci-p2pdma: " fmt	/* NVMe: 커널 로그 접두사를 pci-p2pdma로 설정 */
#include <linux/ctype.h>	/* NVMe: 문자 검사 헤더 */
#include <linux/dma-map-ops.h>	/* NVMe: DMA 매핑 연산 헤더 */
#include <linux/pci-p2pdma.h>	/* NVMe: PCI P2PDMA API 선언 헤더 */
#include <linux/module.h>	/* NVMe: 모듈 및 심볼 export 지원 */
#include <linux/slab.h>	/* NVMe: 커널 메모리 할당 */
#include <linux/genalloc.h>	/* NVMe: 범용 메모리 풀(gen_pool) 관리 */
#include <linux/memremap.h>	/* NVMe: BAR를 ZONE_DEVICE 페이지로 remapping */
#include <linux/percpu-refcount.h>	/* NVMe: ZONE_DEVICE 페이지 생명주기 참조 */
#include <linux/random.h>	/* NVMe: 동일 거리 provider 중 무작위 선택 */
#include <linux/seq_buf.h>	/* NVMe: ACS redirect bridge BDF 목록 구성 */
#include <linux/xarray.h>	/* NVMe: client별 P2P 매핑 타입 캐시 */

/* NVMe: PCI 장치당 하나의 P2PDMA 관리 구조체, CMB 등록/할당/매핑 상태 유지 */
struct pci_p2pdma {	/* NVMe: P2PDMA 관련 구조체 정의 */
	struct gen_pool *pool;		/* NVMe: CMB 물리 영역을 관리하는 메모리 풀 */
	bool p2pmem_published;		/* NVMe: 다른 endpoint가 사용할 수 있도록 공개 여부 */
	struct xarray map_types;	/* NVMe: 각 client마다 계산된 P2P 매핑 타입 캐시 */
	struct p2pdma_provider mem[PCI_STD_NUM_BARS];	/* NVMe: 6개 표준 BAR별 provider */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

/* NVMe: ZONE_DEVICE 페이지와 p2pdma_provider를 연결하는 페이지맵 확장 구조체 */
struct pci_p2pdma_pagemap {	/* NVMe: P2PDMA 관련 구조체 정의 */
	struct dev_pagemap pgmap;	/* NVMe: ZONE_DEVICE 페이지 메타데이터 */
	struct p2pdma_provider *mem;	/* NVMe: 어떤 BAR/provider에 속하는지 역참조용 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

/* NVMe: dev_pagemap 기반으로 pci_p2pdma_pagemap를 얻는 헬퍼 */
static struct pci_p2pdma_pagemap *to_p2p_pgmap(struct dev_pagemap *pgmap)	/* NVMe: dev_pagemap에서 p2pdma_pagemap 구조체를 반환하는 헬퍼 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	return container_of(pgmap, struct pci_p2pdma_pagemap, pgmap);	/* NVMe: dev_pagemap에서 p2pdma_pagemap 구조체 획득 */
}	/* NVMe: 코드 블록 종료 */

/*
 * size_show:
 *   NVMe: sysfs에서 CMB P2P 메모리의 총 크기를 조회한다.
 *   NVMe CMB 크기는 NVMe BAR에 등록된 P2P 풀 전체 크기에 해당.
 */
static ssize_t size_show(struct device *dev, struct device_attribute *attr,	/* NVMe: size sysfs show 함수 선언 */
			 char *buf)	/* NVMe: size_show의 sysfs 출력 버퍼 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *pdev = to_pci_dev(dev);		/* NVMe: sysfs에 연결된 PCI 장치, 즉 NVMe controller */
	struct pci_p2pdma *p2pdma;			/* NVMe: P2PDMA 상태 구조체 포인터 */
	size_t size = 0;				/* NVMe: 반환할 크기(0으로 초기화) */

	rcu_read_lock();				/* NVMe: p2pdma RCU 포인터 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: NVMe 장치의 P2PDMA 구조체 획득 */
	if (p2pdma && p2pdma->pool)			/* NVMe: P2PDMA가 초기화되고 풀이 있으면 */
		size = gen_pool_size(p2pdma->pool);	/* NVMe: 풀의 전체 크기(바이트) 기록 */
	rcu_read_unlock();				/* NVMe: RCU 읽기 락 해제 */

	return sysfs_emit(buf, "%zd\n", size);	/* NVMe: sysfs 버퍼에 크기 출력 */
}	/* NVMe: 코드 블록 종료 */
static DEVICE_ATTR_RO(size);	/* NVMe: size sysfs 읽기 전용 속성 정의 */

/*
 * available_show:
 *   NVMe: sysfs에서 아직 할당되지 않은 CMB P2P 메모리 여유 크기를 조회.
 */
static ssize_t available_show(struct device *dev, struct device_attribute *attr,	/* NVMe: available sysfs show 함수 선언 */
			      char *buf)	/* NVMe: available_show의 sysfs 출력 버퍼 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *pdev = to_pci_dev(dev);		/* NVMe: sysfs에 연결된 PCI 장치(NVMe controller) */
	struct pci_p2pdma *p2pdma;			/* NVMe: P2PDMA 상태 구조체 포인터 */
	size_t avail = 0;				/* NVMe: 반환할 여유 크기(0으로 초기화) */

	rcu_read_lock();				/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: NVMe 장치의 P2PDMA 구조체 획득 */
	if (p2pdma && p2pdma->pool)			/* NVMe: P2PDMA가 초기화되고 풀이 있으면 */
		avail = gen_pool_avail(p2pdma->pool);	/* NVMe: 풀의 잔여(미할당) 크기 기록 */
	rcu_read_unlock();				/* NVMe: RCU 읽기 락 해제 */

	return sysfs_emit(buf, "%zd\n", avail);	/* NVMe: sysfs 버퍼에 여유 크기 출력 */
}	/* NVMe: 코드 블록 종료 */
static DEVICE_ATTR_RO(available);	/* NVMe: available sysfs 읽기 전용 속성 정의 */

/*
 * published_show:
 *   NVMe: sysfs에서 현재 CMB가 P2P provider로 공개되었는지 조회.
 *   공개된 경우 다른 endpoint가 pci_p2pmem_find_many()로 이 NVMe를 찾을 수 있음.
 */
static ssize_t published_show(struct device *dev, struct device_attribute *attr,	/* NVMe: published sysfs show 함수 선언 */
			      char *buf)	/* NVMe: published_show의 sysfs 출력 버퍼 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *pdev = to_pci_dev(dev);		/* NVMe: sysfs에 연결된 PCI 장치(NVMe controller) */
	struct pci_p2pdma *p2pdma;			/* NVMe: P2PDMA 상태 구조체 포인터 */
	bool published = false;				/* NVMe: 공개 여부 기본값 false */

	rcu_read_lock();				/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: NVMe 장치의 P2PDMA 구조체 획득 */
	if (p2pdma)					/* NVMe: P2PDMA가 초기화되었으면 */
		published = p2pdma->p2pmem_published;	/* NVMe: 공개 플래그 값 읽기 */
	rcu_read_unlock();				/* NVMe: RCU 읽기 락 해제 */

	return sysfs_emit(buf, "%d\n", published);	/* NVMe: sysfs 버퍼에 공개 여부 출력 */
}	/* NVMe: 코드 블록 종료 */
static DEVICE_ATTR_RO(published);	/* NVMe: published sysfs 읽기 전용 속성 정의 */

/*
 * p2pmem_alloc_mmap:
 *   NVMe: userspace가 /sys/bus/pci/devices/.../p2pmem/allocate를 mmap으로
 *   매핑할 때 호출. NVMe CMB 영역을 userspace에 직접 노출할 수 있음.
 */
static int p2pmem_alloc_mmap(struct file *filp, struct kobject *kobj,	/* NVMe: CMB allocate 파일 mmap 함수 선언 */
		const struct bin_attribute *attr, struct vm_area_struct *vma)	/* NVMe: p2pmem_alloc_mmap의 매개변수 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));	/* NVMe: sysfs kobject에서 NVMe PCI 장치 획득 */
	size_t len = vma->vm_end - vma->vm_start;		/* NVMe: 매핑 요청 길이 계산 */
	struct pci_p2pdma *p2pdma;				/* NVMe: P2PDMA 상태 포인터 */
	struct percpu_ref *ref;					/* NVMe: ZONE_DEVICE 페이지 생명주기 참조 */
	unsigned long vaddr;					/* NVMe: 매핑 중인 userspace 가상 주소 루프 변수 */
	void *kaddr;						/* NVMe: 풀에서 할당된 커널 가상 주소 */
	int ret;						/* NVMe: 반환값 */

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {	/* NVMe: 공유 매핑이 아니면 */
		pci_info_ratelimited(pdev,				/* NVMe: 과도한 로그 방지용 정보 출력 */
				     "%s: fail, attempted private mapping\n",	/* NVMe: private mapping 거부 메시지 포맷 문자열 */
				     current->comm);	/* NVMe: 현재 프로세스 이름 인자 */
		return -EINVAL;					/* NVMe: 사유 매핑은 허용하지 않음 */
	}	/* NVMe: 코드 블록 종료 */

	if (vma->vm_pgoff) {					/* NVMe: 파일 오프셋이 0이 아니면 */
		pci_info_ratelimited(pdev,				/* NVMe: 오프셋 사용 시 로그 출력 */
				     "%s: fail, attempted mapping with non-zero offset\n",	/* NVMe: non-zero offset 거부 메시지 포맷 문자열 */
				     current->comm);	/* NVMe: 현재 프로세스 이름 인자 */
		return -EINVAL;					/* NVMe: 오프셋 매핑은 허용하지 않음 */
	}	/* NVMe: 코드 블록 종료 */

	rcu_read_lock();					/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);		/* NVMe: NVMe 장치의 P2PDMA 상태 획득 */
	if (!p2pdma) {						/* NVMe: P2PDMA가 초기화되지 않았으면 */
		ret = -ENODEV;					/* NVMe: 장치 없음 에러 설정 */
		goto out;					/* NVMe: RCU 해제 후 반환 */
	}	/* NVMe: 코드 블록 종료 */

	kaddr = (void *)gen_pool_alloc_owner(p2pdma->pool, len, (void **)&ref);	/* NVMe: P2P 풀에서 len 바이트 할당, 소유자 참조 획득 */
	if (!kaddr) {						/* NVMe: 할당 실패 시 */
		ret = -ENOMEM;					/* NVMe: 메모리 부족 에러 설정 */
		goto out;					/* NVMe: RCU 해제 후 반환 */
	}	/* NVMe: 코드 블록 종료 */

	/*
	 * vm_insert_page() can sleep, so a reference is taken to mapping
	 * such that rcu_read_unlock() can be done before inserting the
	 * pages
	 */
	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {	/* NVMe: 페이지 생명주기 참조 획득 실패 시 */
		ret = -ENODEV;					/* NVMe: 장치 제거 중으로 간주 */
		goto out_free_mem;				/* NVMe: 할당 메모리 해제 경로 */
	}	/* NVMe: 코드 블록 종료 */
	rcu_read_unlock();					/* NVMe: 참조 획득 후 RCU 락 해제 가능 */

	for (vaddr = vma->vm_start; vaddr < vma->vm_end; vaddr += PAGE_SIZE) {	/* NVMe: 페이지 단위로 userspace vma에 페이지 삽입 루프 */
		struct page *page = virt_to_page(kaddr);				/* NVMe: 할당된 가상 주소를 struct page로 변환 */

		/*
		 * Initialise the refcount for the freshly allocated page. As
		 * we have just allocated the page no one else should be
		 * using it.
		 */
		VM_WARN_ON_ONCE_PAGE(page_ref_count(page), page);		/* NVMe: 참조 카운트가 0이 아니면 경고(방금 할당되어야 함) */
		set_page_count(page, 1);						/* NVMe: 페이지 참조 카운트를 1로 초기화 */
		ret = vm_insert_page(vma, vaddr, page);				/* NVMe: userspace vma에 해당 페이지를 매핑 */
		if (ret) {								/* NVMe: 페이지 삽입 실패 시 */
			gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);	/* NVMe: P2P 풀에서 할당한 메모리 반납 */

			/*
			 * Reset the page count. We don't use put_page()
			 * because we don't want to trigger the
			 * p2pdma_folio_free() path.
			 */
			set_page_count(page, 0);					/* NVMe: 페이지 참조 카운트를 0으로 재설정 */
			percpu_ref_put(ref);						/* NVMe: 페이지 생명주기 참조 해제 */
			return ret;							/* NVMe: 매핑 실패 반환 */
		}	/* NVMe: 코드 블록 종료 */
		percpu_ref_get(ref);			/* NVMe: vm_insert_page가 페이지에 대한 추가 참조를 가정하므로 ref 증가 */
		put_page(page);				/* NVMe: set_page_count로 얻은 초기 참조 해제 */
		kaddr += PAGE_SIZE;			/* NVMe: 다음 페이지 커널 주소로 이동 */
		len -= PAGE_SIZE;			/* NVMe: 남은 매핑 길이 감소 */
	}	/* NVMe: 코드 블록 종료 */

	percpu_ref_put(ref);				/* NVMe: 루프에서 추가 획득한 참조 해제 */

	return 0;					/* NVMe: mmap 성공 반환 */
out_free_mem:	/* NVMe: 할당된 P2P 메모리 반납 경로 */
	gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);	/* NVMe: 할당된 P2P 메모리 반납 */
out:	/* NVMe: 함수 종료/에러 반환 경로 */
	rcu_read_unlock();					/* NVMe: RCU 읽기 락 해제 */
	return ret;						/* NVMe: 에러 코드 반환 */
}	/* NVMe: 코드 블록 종료 */

static const struct bin_attribute p2pmem_alloc_attr = {	/* NVMe: 변수/필드 초기화 */
	.attr = { .name = "allocate", .mode = 0660 },	/* NVMe: allocate sysfs 파일 이름/권한 */
	.mmap = p2pmem_alloc_mmap,			/* NVMe: mmap 연산 등록 */
	/*
	 * Some places where we want to call mmap (ie. python) will check
	 * that the file size is greater than the mmap size before allowing
	 * the mmap to continue. To work around this, just set the size
	 * to be very large.
	 */
	.size = SZ_1T,					/* NVMe: mmap 전 크기 검사 우회용 큰 값 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

static struct attribute *p2pmem_attrs[] = {	/* NVMe: 변수/필드 초기화 */
	&dev_attr_size.attr,				/* NVMe: size sysfs 속성 연결 */
	&dev_attr_available.attr,			/* NVMe: available sysfs 속성 연결 */
	&dev_attr_published.attr,			/* NVMe: published sysfs 속성 연결 */
	NULL,						/* NVMe: 속성 배열 종료 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

static const struct bin_attribute *const p2pmem_bin_attrs[] = {	/* NVMe: 변수/필드 초기화 */
	&p2pmem_alloc_attr,				/* NVMe: allocate bin attribute 연결 */
	NULL,						/* NVMe: bin attribute 배열 종료 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

static const struct attribute_group p2pmem_group = {	/* NVMe: 변수/필드 초기화 */
	.attrs = p2pmem_attrs,				/* NVMe: 일반 sysfs 속성 그룹 */
	.bin_attrs = p2pmem_bin_attrs,			/* NVMe: bin sysfs 속성 그룹 */
	.name = "p2pmem",					/* NVMe: /sys/.../p2pmem 디렉터리 이름 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

/*
 * p2pdma_folio_free:
 *   NVMe: ZONE_DEVICE 페이지가 해제될 때 호출. CMB 페이지 반납 처리.
 */
static void p2pdma_folio_free(struct folio *folio)	/* NVMe: ZONE_DEVICE folio 해제 시 CMB 반납 */
{	/* NVMe: p2pdma_folio_free 함수 본문 시작 */
	struct page *page = &folio->page;				/* NVMe: folio의 대표 page 획득 */
	struct pci_p2pdma_pagemap *pgmap = to_p2p_pgmap(page_pgmap(page));	/* NVMe: 페이지맵에서 p2pdma 페이지맵 획득 */
	/* safe to dereference while a reference is held to the percpu ref */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(	/* NVMe: 소유자 NVMe 장치의 P2PDMA 상태를 쓰기 보호 하에 획득 */
		to_pci_dev(pgmap->mem->owner)->p2pdma, 1);		/* NVMe: 소유자 NVMe 장치의 P2PDMA 상태 획득 */
	struct percpu_ref *ref;						/* NVMe: 페이지 생명주기 참조 포인터 */

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)page_to_virt(page),	/* NVMe: P2P 풀에서 해당 페이지 영역 반납 */
			    PAGE_SIZE, (void **)&ref);	/* NVMe: 페이지 크기 및 생명주기 참조 인자 */
	percpu_ref_put(ref);						/* NVMe: 페이지 생명주기 참조 해제 */
}	/* NVMe: p2pdma_folio_free 함수 본문 종료 */

static const struct dev_pagemap_ops p2pdma_pgmap_ops = {	/* NVMe: 변수/필드 초기화 */
	.folio_free = p2pdma_folio_free,	/* NVMe: folio 해제 콜백 등록 */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

/*
 * pci_p2pdma_release:
 *   NVMe: devm_ 메커니즘으로 P2PDMA 자원 해제 시 호출. NVMe remove 시 실행.
 */
static void pci_p2pdma_release(void *data)	/* NVMe: NVMe 제거 시 P2PDMA 자원 해제 */
{	/* NVMe: pci_p2pdma_release 함수 본문 시작 */
	struct pci_dev *pdev = data;				/* NVMe: 해제할 NVMe PCI 장치 */
	struct pci_p2pdma *p2pdma;				/* NVMe: P2PDMA 상태 포인터 */

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: 쓰기 보호 상태에서 p2pdma 획득 */
	if (!p2pdma)						/* NVMe: p2pdma가 없으면 */
		return;						/* NVMe: 할 것 없이 반환 */

	/* Flush and disable pci_alloc_p2p_mem() */
	pdev->p2pdma = NULL;					/* NVMe: 새로운 P2P 할당 차단 */
	if (p2pdma->pool)	/* NVMe: 메모리 풀이 존재하면 RCU 동기화 수행 */
		synchronize_rcu();				/* NVMe: 진행 중인 RCU reader 종료 대기 */
	xa_destroy(&p2pdma->map_types);				/* NVMe: client별 매핑 타입 캐시 해제 */

	if (!p2pdma->pool)	/* NVMe: 메모리 풀이 없으면 조기 종료 */
		return;						/* NVMe: 풀이 없으면 여기서 종료 */

	gen_pool_destroy(p2pdma->pool);				/* NVMe: 메모리 풀 제거 */
	sysfs_remove_group(&pdev->dev.kobj, &p2pmem_group);	/* NVMe: /sys/.../p2pmem 디렉터리 제거 */
}	/* NVMe: pci_p2pdma_release 함수 본문 종료 */

/**
 * pcim_p2pdma_init - Initialise peer-to-peer DMA providers
 * @pdev: The PCI device to enable P2PDMA for
 *
 * This function initializes the peer-to-peer DMA infrastructure
 * for a PCI device. It allocates and sets up the necessary data
 * structures to support P2PDMA operations, including mapping type
 * tracking.
 */
/*
 * pcim_p2pdma_init:
 *   NVMe: NVMe controller의 P2PDMA 인프라 초기화. CMB 등록 전에 먼저 호출.
 */
int pcim_p2pdma_init(struct pci_dev *pdev)	/* NVMe: P2PDMA 인프라 초기화 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma *p2p;					/* NVMe: 새 P2PDMA 관리 구조체 */
	int i, ret;						/* NVMe: 반복/반환 변수 */

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: 이미 초기화되었는지 확인 */
	if (p2p)	/* NVMe: 이미 초기화된 P2PDMA가 있으면 중복 방지 */
		return 0;					/* NVMe: 중복 초기화 방지 */

	p2p = devm_kzalloc(&pdev->dev, sizeof(*p2p), GFP_KERNEL);	/* NVMe: P2PDMA 구조체 0으로 할당 */
	if (!p2p)	/* NVMe: P2PDMA 구조체 할당 실패 처리 */
		return -ENOMEM;					/* NVMe: 메모리 부족 시 에러 */

	xa_init(&p2p->map_types);				/* NVMe: client별 매핑 타입 xarray 초기화 */
	/*
	 * Iterate over all standard PCI BARs and record only those that
	 * correspond to MMIO regions. Skip non-memory resources (e.g. I/O
	 * port BARs) since they cannot be used for peer-to-peer (P2P)
	 * transactions.
	 */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {		/* NVMe: 6개 표준 BAR 순회 */
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM))	/* NVMe: MMIO가 아닌 BAR는 건다 */
			continue;				/* NVMe: I/O port BAR 등 스킵 */

		p2p->mem[i].owner = &pdev->dev;			/* NVMe: provider 소유자를 NVMe controller로 설정 */
		p2p->mem[i].bus_offset =	/* NVMe: PCI bus 주소와 CPU resource 주소 간 오프셋 계산 */
			pci_bus_address(pdev, i) - pci_resource_start(pdev, i);		/* NVMe: bus 주소와 CPU resource 주소 간 오프셋 계산(ATS/ReBAR/DMA 주소 변환용) */
	}	/* NVMe: 코드 블록 종료 */

	ret = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_release, pdev);	/* NVMe: 드라이버 unbind 시 해제 콜백 등록 */
	if (ret)	/* NVMe: devm 해제 콜백 등록 실패 시 정리 */
		goto out_p2p;					/* NVMe: 등록 실패 시 정리 */

	rcu_assign_pointer(pdev->p2pdma, p2p);			/* NVMe: RCU reader에게 새 P2PDMA 공개 */
	return 0;						/* NVMe: 초기화 성공 */

out_p2p:	/* NVMe: P2PDMA 구조체 해제 경로 */
	devm_kfree(&pdev->dev, p2p);				/* NVMe: 할당된 P2PDMA 구조체 해제 */
	return ret;						/* NVMe: 에러 코드 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pcim_p2pdma_init);	/* NVMe: pcim_p2pdma_init 심볼을 외부 모듈에 노출 */

/**
 * pcim_p2pdma_provider - Get peer-to-peer DMA provider
 * @pdev: The PCI device to enable P2PDMA for
 * @bar: BAR index to get provider
 *
 * This function gets peer-to-peer DMA provider for a PCI device. The lifetime
 * of the provider (and of course the MMIO) is bound to the lifetime of the
 * driver. A driver calling this function must ensure that all references to the
 * provider, and any DMA mappings created for any MMIO, are all cleaned up
 * before the driver remove() completes.
 *
 * Since P2P is almost always shared with a second driver this means some system
 * to notify, invalidate and revoke the MMIO's DMA must be in place to use this
 * function. For example a revoke can be built using DMABUF.
 */
/*
 * pcim_p2pdma_provider:
 *   NVMe: 지정된 BAR에 대한 p2pdma_provider를 반환. NVMe CMB는 특정 BAR에
 *   해당하므로 해당 BAR의 provider를 얻어 외부에 노출하거나 I/O에 사용.
 */
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar)	/* NVMe: 지정 BAR의 P2P provider 반환 함수 선언 */
{	/* NVMe: p2pdma_provider 정의 블록 시작 */
	struct pci_p2pdma *p2p;					/* NVMe: P2PDMA 상태 포인터 */

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))	/* NVMe: 해당 BAR가 MMIO가 아니면 */
		return NULL;					/* NVMe: P2P provider 불가 */

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: p2pdma 초기화 여부 확인 */
	if (WARN_ON(!p2p))	/* NVMe: pcim_p2pdma_init() 누락 방어적 경고 */
		/* Someone forgot to call to pcim_p2pdma_init() before */
		return NULL;					/* NVMe: pcim_p2pdma_init 누락 경고 후 NULL */

	return &p2p->mem[bar];					/* NVMe: 해당 BAR의 provider 구조체 반환 */
}	/* NVMe: p2pdma_provider 정의 블록 종료 */
EXPORT_SYMBOL_GPL(pcim_p2pdma_provider);	/* NVMe: pcim_p2pdma_provider 심볼을 외부 모듈에 노출 */

/*
 * pci_p2pdma_setup_pool:
 *   NVMe: P2P 메모리 할당 풀과 sysfs 그룹을 생성. CMB 등록 직전 호출.
 */
static int pci_p2pdma_setup_pool(struct pci_dev *pdev)	/* NVMe: P2P 메모리 풀 설정 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma *p2pdma;				/* NVMe: P2PDMA 상태 포인터 */
	int ret;						/* NVMe: 반환값 */

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: 쓰기 보호 상태에서 p2pdma 획득 */
	if (p2pdma->pool)	/* NVMe: 이미 풀이 생성되어 있으면 중복 생성 방지 */
		/* We already setup pools, do nothing, */
		return 0;					/* NVMe: 이미 풀이 있으면 중복 생성 방지 */

	p2pdma->pool = gen_pool_create(PAGE_SHIFT, dev_to_node(&pdev->dev));	/* NVMe: 페이지 단위 메모리 풀 생성(NUMA 노드 고려) */
	if (!p2pdma->pool)	/* NVMe: P2P 메모리 풀 생성 실패 처리 */
		return -ENOMEM;					/* NVMe: 풀 생성 실패 시 메모리 부족 */

	ret = sysfs_create_group(&pdev->dev.kobj, &p2pmem_group);	/* NVMe: /sys/.../p2pmem 디렉터리 생성 */
	if (ret)	/* NVMe: sysfs 그룹 생성 실패 시 풀 제거 */
		goto out_pool_destroy;				/* NVMe: sysfs 생성 실패 시 풀 제거 */

	return 0;						/* NVMe: 풀 및 sysfs 생성 성공 */

out_pool_destroy:	/* NVMe: 메모리 풀 제거 경로 */
	gen_pool_destroy(p2pdma->pool);				/* NVMe: 생성된 메모리 풀 제거 */
	p2pdma->pool = NULL;					/* NVMe: 풀 포인터 정리 */
	return ret;						/* NVMe: 에러 코드 반환 */
}	/* NVMe: 코드 블록 종료 */

/*
 * pci_p2pdma_unmap_mappings:
 *   NVMe: 드라이버 제거 시 userspace 매핑을 끊고 새 매핑 차단.
 */
static void pci_p2pdma_unmap_mappings(void *data)	/* NVMe: userspace 매핑 무효화 콜백 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma_pagemap *p2p_pgmap = data;		/* NVMe: 해제할 p2pdma 페이지맵 */

	/*
	 * Removing the alloc attribute from sysfs will call
	 * unmap_mapping_range() on the inode, teardown any existing userspace
	 * mappings and prevent new ones from being created.
	 */
	sysfs_remove_file_from_group(&p2p_pgmap->mem->owner->kobj,	/* NVMe: provider(NVMe)의 sysfs kobject에서 */
				     &p2pmem_alloc_attr.attr,	/* NVMe: 제거할 allocate 속성 인자 */
				     p2pmem_group.name);		/* NVMe: allocate 파일을 제거해 userspace mmap 무효화 */
}	/* NVMe: 코드 블록 종료 */

/**
 * pci_p2pdma_add_resource - add memory for use as p2p memory
 * @pdev: the device to add the memory to
 * @bar: PCI BAR to add
 * @size: size of the memory to add, may be zero to use the whole BAR
 * @offset: offset into the PCI BAR
 *
 * The memory will be given ZONE_DEVICE struct pages so that it may
 * be used with any DMA request.
 */
/*
 * pci_p2pdma_add_resource:
 *   NVMe: NVMe controller의 특정 BAR(보통 CMB BAR)를 P2P 메모리로 등록.
 *   ZONE_DEVICE 페이지를 만들어 DMA scatterlist 등에 사용 가능하게 함.
 */
int pci_p2pdma_add_resource(struct pci_dev *pdev, int bar, size_t size,	/* NVMe: BAR를 P2P 메모리로 등록하는 함수 선언 */
			    u64 offset)	/* NVMe: pci_p2pdma_add_resource의 offset 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma_pagemap *p2p_pgmap;			/* NVMe: 새 페이지맵 확장 구조체 */
	struct p2pdma_provider *mem;				/* NVMe: 해당 BAR의 provider */
	struct dev_pagemap *pgmap;				/* NVMe: dev_pagemap 객체 */
	struct pci_p2pdma *p2pdma;				/* NVMe: P2PDMA 상태 포인터 */
	void *addr;						/* NVMe: devm_memremap_pages로 매핑된 주소 */
	int error;						/* NVMe: 반환값 */

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))	/* NVMe: MMIO BAR가 아니면 */
		return -EINVAL;					/* NVMe: 잘못된 인자 */

	if (offset >= pci_resource_len(pdev, bar))		/* NVMe: offset이 BAR 길이 이상이면 */
		return -EINVAL;					/* NVMe: 범위 초과 */

	if (!size)	/* NVMe: size가 0이면 offset 이후 전체 BAR 사용 */
		size = pci_resource_len(pdev, bar) - offset;	/* NVMe: size 0이면 offset 이후 전체 BAR 사용 */

	if (size + offset > pci_resource_len(pdev, bar))	/* NVMe: offset+size가 BAR를 넘어가면 */
		return -EINVAL;					/* NVMe: 범위 초과 */

	error = pcim_p2pdma_init(pdev);				/* NVMe: P2PDMA 인프라 초기화 */
	if (error)	/* NVMe: P2PDMA 인프라 초기화 실패 반환 */
		return error;					/* NVMe: 초기화 실패 */

	error = pci_p2pdma_setup_pool(pdev);			/* NVMe: 메모리 풀 및 sysfs 생성 */
	if (error)	/* NVMe: P2P 메모리 풀 설정 실패 반환 */
		return error;					/* NVMe: 풀 생성 실패 */

	mem = pcim_p2pdma_provider(pdev, bar);			/* NVMe: 해당 BAR의 provider 획득 */
	/*
	 * We checked validity of BAR prior to call
	 * to pcim_p2pdma_provider. It should never return NULL.
	 */
	if (WARN_ON(!mem))	/* NVMe: BAR provider NULL 방어적 검사 */
		return -EINVAL;					/* NVMe: 정상이면 NULL이 될 수 없음, 방어 코드 */

	p2p_pgmap = devm_kzalloc(&pdev->dev, sizeof(*p2p_pgmap), GFP_KERNEL);	/* NVMe: 페이지맵 확장 구조체 할당 */
	if (!p2p_pgmap)	/* NVMe: 페이지맵 확장 구조체 할당 실패 처리 */
		return -ENOMEM;					/* NVMe: 메모리 부족 */

	pgmap = &p2p_pgmap->pgmap;				/* NVMe: dev_pagemap 객체 포인터 */
	pgmap->range.start = pci_resource_start(pdev, bar) + offset;	/* NVMe: P2P 영역의 CPU 물리 시작 주소 */
	pgmap->range.end = pgmap->range.start + size - 1;	/* NVMe: P2P 영역의 CPU 물리 끝 주소 */
	pgmap->nr_range = 1;					/* NVMe: 연속 단일 범위 */
	pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;			/* NVMe: P2PDMA용 ZONE_DEVICE 타입 지정 */
	pgmap->ops = &p2pdma_pgmap_ops;				/* NVMe: folio_free 콜백 연결 */
	p2p_pgmap->mem = mem;					/* NVMe: 확장 페이지맵이 어떤 provider에 속하는지 기록 */

	addr = devm_memremap_pages(&pdev->dev, pgmap);		/* NVMe: BAR 영역을 ZONE_DEVICE 페이지로 매핑 */
	if (IS_ERR(addr)) {					/* NVMe: 매핑 실패 시 */
		error = PTR_ERR(addr);				/* NVMe: 에러 코드 추출 */
		goto pgmap_free;				/* NVMe: 페이지맵 해제 경로 */
	}	/* NVMe: 코드 블록 종료 */

	error = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_unmap_mappings,	/* NVMe: 드라이버 제거 시 userspace 매핑 무효화 콜백 등록 */
					 p2p_pgmap);		/* NVMe: 드라이버 제거 시 userspace 매핑 무효화 등록 */
	if (error)	/* NVMe: 이전 단계 실패 시 상위로 에러 전달 */
		goto pages_free;				/* NVMe: 등록 실패 시 페이지 해제 */

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: P2PDMA 상태 다시 획득 */
	error = gen_pool_add_owner(p2pdma->pool, (unsigned long)addr,	/* NVMe: P2P 풀에 bus 주소 영역 등록 시작 */
			pci_bus_address(pdev, bar) + offset,	/* NVMe: 등록할 PCI bus 시작 주소 인자 */
			range_len(&pgmap->range), dev_to_node(&pdev->dev),	/* NVMe: 영역 길이 및 NUMA 노드 인자 */
			&pgmap->ref);				/* NVMe: ZONE_DEVICE 가상 주소와 PCI bus 주소를 풀에 추가, 페이지 생명주기 ref 연결 */
	if (error)	/* NVMe: 이전 단계 실패 시 상위로 에러 전달 */
		goto pages_free;				/* NVMe: 풀 추가 실패 시 페이지 해제 */

	pci_info(pdev, "added peer-to-peer DMA memory %#llx-%#llx\n",	/* NVMe: 등록된 P2P 메모리 영역 커널 로그 출력 */
		 pgmap->range.start, pgmap->range.end);	/* NVMe: 등록된 P2P 메모리 영역 커널 로그 출력 */

	return 0;						/* NVMe: P2P 메모리 등록 성공 */

pages_free:	/* NVMe: ZONE_DEVICE 페이지 매핑 해제 경로 */
	devm_memunmap_pages(&pdev->dev, pgmap);			/* NVMe: ZONE_DEVICE 페이지 매핑 해제 */
pgmap_free:	/* NVMe: 페이지맵 확장 구조체 해제 경로 */
	devm_kfree(&pdev->dev, p2p_pgmap);			/* NVMe: 페이지맵 확장 구조체 해제 */
	return error;						/* NVMe: 에러 코드 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pdma_add_resource);	/* NVMe: pci_p2pdma_add_resource 심볼을 외부 모듈에 노출 */

/*
 * Note this function returns the parent PCI device with a
 * reference taken. It is the caller's responsibility to drop
 * the reference.
 */
/*
 * find_parent_pci_dev:
 *   NVMe: 임의 device(예: NVMe block device)에서 시작해 상위 PCI 장치를
 *   찾는다. P2P provider/client 비교 시 struct device -> struct pci_dev
 *   변환에 사용. 반환된 pci_dev는 pci_dev_put()으로 참조 해제 필요.
 */
static struct pci_dev *find_parent_pci_dev(struct device *dev)	/* NVMe: 임의 device에서 상위 PCI 장치 검색 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct device *parent;					/* NVMe: 상위 device 포인터 */

	dev = get_device(dev);					/* NVMe: 현재 device 참조 증가 */

	while (dev) {						/* NVMe: device 트리를 루트 방향으로 순회 */
		if (dev_is_pci(dev))				/* NVMe: 현재 device가 PCI 장치이면 */
			return to_pci_dev(dev);			/* NVMe: pci_dev로 변환하여 반환(참조 증가 상태) */

		parent = get_device(dev->parent);		/* NVMe: 상위 device 참조 증가 */
		put_device(dev);				/* NVMe: 현재 device 참조 해제 */
		dev = parent;					/* NVMe: 상위로 이동 */
	}	/* NVMe: 코드 블록 종료 */

	return NULL;						/* NVMe: PCI 장치를 찾지 못함 */
}	/* NVMe: 코드 블록 종료 */

/*
 * Check if a PCI bridge has its ACS redirection bits set to redirect P2P
 * TLPs upstream via ACS. Returns 1 if the packets will be redirected
 * upstream, 0 otherwise.
 */
/*
 * pci_bridge_has_acs_redir:
 *   NVMe: PCIe bridge의 ACS(Access Control Services) redirect 설정을 확인.
 *   ACS redirect가 켜져 있으면 P2P TLP가 upstream(host bridge)으로 향해
 *   P2P 직접 경로가 막히므로, NVMe P2P I/O 성능/가능 여부 판정에 핵심.
 */
static int pci_bridge_has_acs_redir(struct pci_dev *pdev)	/* NVMe: PCI bridge ACS redirect 검사 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	int pos;						/* NVMe: ACS capability offset */
	u16 ctrl;						/* NVMe: ACS control 레지스터 값 */

	pos = pdev->acs_cap;					/* NVMe: bridge의 ACS capability 위치 획득 */
	if (!pos)	/* NVMe: ACS capability가 없으면 redirect 없음 */
		return 0;					/* NVMe: ACS capability가 없으면 redirect 없음 */

	pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl);	/* NVMe: ACS control 레지스터 읽기 */

	if (ctrl & (PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC))	/* NVMe: redirect, completion, egress 제어 중 하나라도 설정되면 */
		return 1;					/* NVMe: P2P TLP가 upstream으로 redirect됨 */

	return 0;						/* NVMe: ACS redirect 미설정 */
}	/* NVMe: 코드 블록 종료 */

/*
 * seq_buf_print_bus_devfn:
 *   NVMe: ACS redirect가 설정된 bridge의 BDF를 문자열 버퍼에 추가.
 *   kernel parameter pci=disable_acs_redir=...에 사용될 목록 구성.
 */
static void seq_buf_print_bus_devfn(struct seq_buf *buf, struct pci_dev *pdev)	/* NVMe: ACS redirect bridge BDF 추가 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	if (!buf)	/* NVMe: seq_buf가 없으면 아무 것도 출력하지 않음 */
		return;						/* NVMe: 버퍼가 없으면 아무 것도 안 함 */

	seq_buf_printf(buf, "%s;", pci_name(pdev));		/* NVMe: "0000:01:00.0;" 형태로 BDF 추가 */
}	/* NVMe: 코드 블록 종료 */

/*
 * cpu_supports_p2pdma:
 *   NVMe: CPU/IMC(Integrated Memory Controller)가 P2PDMA를 지원하는지 확인.
 *   현재는 AMD Zen 이상에서만 true를 반환. whitelist 방식의 보조.
 */
static bool cpu_supports_p2pdma(void)	/* NVMe: CPU P2PDMA 지원 여부 확인 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
#ifdef CONFIG_X86	/* NVMe: x86 아키텍처에서 CPU P2PDMA 지원 여부 확인 */
	struct cpuinfo_x86 *c = &cpu_data(0);			/* NVMe: 0번 CPU 정보 획득 */

	/* Any AMD CPU whose family ID is Zen or newer supports p2pdma */
	if (c->x86_vendor == X86_VENDOR_AMD && c->x86 >= 0x17)	/* NVMe: AMD Zen 이상이면 */
		return true;					/* NVMe: CPU가 P2PDMA 지원 */
#endif	/* NVMe: x86 조걶부 컴파일 종료 */

	return false;						/* NVMe: 지원하지 않는 CPU 플랫폼 */
}	/* NVMe: 코드 블록 종료 */

/*
 * NVMe: host bridge whitelist. P2P가 같은 upstream bridge를 공유하지 않을
 * 때라도 이 목록의 Root Complex에서는 THRU_HOST_BRIDGE로 허용.
 * SR-IOV, 멀티포트 NVMe, RDMA 등에서 같은 Root Complex 내 P2P 활용 시
 * 중요.
 */
static const struct pci_p2pdma_whitelist_entry {	/* NVMe: P2PDMA whitelist 구조체 정의 */
	unsigned short vendor;	/* NVMe: whitelist vendor ID 필드 */
	int device;	/* NVMe: whitelist device ID 필드 */
	enum {	/* NVMe: whitelist 플래그 열거형 */
		REQ_SAME_HOST_BRIDGE	= 1 << 0,	/* NVMe: 같은 host bridge 필요 플래그 */
	} flags;	/* NVMe: 플래그 멤버 정의 종료 */
} pci_p2pdma_whitelist[] = {	/* NVMe: P2PDMA whitelist 배열 정의 */
	/* Intel Xeon E5/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x3c00, REQ_SAME_HOST_BRIDGE},	/* NVMe: Intel Xeon E5/Core i7 whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x3c01, REQ_SAME_HOST_BRIDGE},	/* NVMe: Intel Xeon E5/Core i7 whitelist 항목 */
	/* Intel Xeon E7 v3/Xeon E5 v3/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x2f00, REQ_SAME_HOST_BRIDGE},	/* NVMe: Intel Xeon E7 v3/E5 v3 whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x2f01, REQ_SAME_HOST_BRIDGE},	/* NVMe: Intel Xeon E7 v3/E5 v3 whitelist 항목 */
	/* Intel Skylake-E */
	{PCI_VENDOR_ID_INTEL,	0x2030, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x2031, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x2032, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x2033, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x2020, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	{PCI_VENDOR_ID_INTEL,	0x09a2, 0},	/* NVMe: Intel Skylake-E whitelist 항목 */
	/* Google SoCs. */
	{PCI_VENDOR_ID_GOOGLE,	PCI_ANY_ID, 0},	/* NVMe: Google SoC whitelist 항목 */
	{}	/* NVMe: whitelist 배열 종료 sentinel */
};	/* NVMe: 구조체/공용체/열거형 정의 종료 */

/*
 * If the first device on host's root bus is either devfn 00.0 or a PCIe
 * Root Port, return it.  Otherwise return NULL.
 *
 * We often use a devfn 00.0 "host bridge" in the pci_p2pdma_whitelist[]
 * (though there is no PCI/PCIe requirement for such a device).  On some
 * platforms, e.g., Intel Skylake, there is no such host bridge device, and
 * pci_p2pdma_whitelist[] may contain a Root Port at any devfn.
 *
 * This function is similar to pci_get_slot(host->bus, 0), but it does
 * not take the pci_bus_sem lock since __host_bridge_whitelist() must not
 * sleep.
 *
 * For this to be safe, the caller should hold a reference to a device on the
 * bridge, which should ensure the host_bridge device will not be freed
 * or removed from the head of the devices list.
 */
/*
 * pci_host_bridge_dev:
 *   NVMe: host bridge의 root bus에서 whitelist 판별에 사용할 root 장치를
 *   찾는다. NVMe endpoint가 연결된 domain의 host bridge 정보 획득.
 */
static struct pci_dev *pci_host_bridge_dev(struct pci_host_bridge *host)	/* NVMe: host bridge의 root 장치 검색 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *root;					/* NVMe: root bus의 첫 번째 장치 */

	root = list_first_entry_or_null(&host->bus->devices,	/* NVMe: root bus device 목록의 첫 항목 획득 */
					struct pci_dev, bus_list);	/* NVMe: root bus device 목록의 첫 항목 획득 */

	if (!root)	/* NVMe: root bus에 판별용 장치가 없음 */
		return NULL;					/* NVMe: root bus에 장치가 없으면 NULL */

	if (root->devfn == PCI_DEVFN(0, 0))	/* NVMe: devfn 00.0 host bridge 인식 */
		return root;					/* NVMe: devfn 00.0 host bridge이면 반환 */

	if (pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT)	/* NVMe: Root Port 형태의 host bridge 인식 */
		return root;					/* NVMe: Root Port이면 반환(Skylake 등) */

	return NULL;						/* NVMe: whitelist 판별 불가 */
}	/* NVMe: 코드 블록 종료 */

/*
 * __host_bridge_whitelist:
 *   NVMe: 특정 host bridge가 P2PDMA whitelist에 있는지 확인.
 *   REQ_SAME_HOST_BRIDGE 플래그가 있으면 같은 host bridge 내에서만 허용.
 */
static bool __host_bridge_whitelist(struct pci_host_bridge *host,	/* NVMe: host bridge whitelist 낮은 수준 검사 함수 선언 */
				    bool same_host_bridge, bool warn)	/* NVMe: __host_bridge_whitelist의 매개변수 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_dev *root = pci_host_bridge_dev(host);	/* NVMe: whitelist 판별용 root 장치 획득 */
	const struct pci_p2pdma_whitelist_entry *entry;		/* NVMe: whitelist 순회 포인터 */
	unsigned short vendor, device;				/* NVMe: root 장치의 vendor/device ID */

	if (!root)	/* NVMe: root bus에 판별용 장치가 없음 */
		return false;					/* NVMe: root 장치를 찾을 수 없으면 whitelist 불가 */

	vendor = root->vendor;					/* NVMe: vendor ID 기록 */
	device = root->device;					/* NVMe: device ID 기록 */

	for (entry = pci_p2pdma_whitelist; entry->vendor; entry++) {	/* NVMe: whitelist 배열 순회(마지막 빈 entry 전까지) */
		if (vendor != entry->vendor)			/* NVMe: vendor가 다류면 */
			continue;				/* NVMe: 다음 entry */

		if (entry->device != PCI_ANY_ID && device != entry->device)	/* NVMe: device가 특정되어 있고 일치하지 않으면 */
			continue;				/* NVMe: 다음 entry */

		if (entry->flags & REQ_SAME_HOST_BRIDGE && !same_host_bridge)	/* NVMe: 같은 host bridge 필수인데 아니면 */
			return false;				/* NVMe: whitelist 조건 불만족 */

		return true;					/* NVMe: whitelist에 포함됨 */
	}	/* NVMe: 코드 블록 종료 */

	if (warn)	/* NVMe: warn 조건 검사 */
		pci_warn(root, "Host bridge not in P2PDMA whitelist: %04x:%04x\n",	/* NVMe: pci_warn 호출 인자 */
			 vendor, device);				/* NVMe: whitelist에 없으면 경고 로그 */

	return false;						/* NVMe: whitelist에 없음 */
}	/* NVMe: 코드 블록 종료 */

/*
 * If we can't find a common upstream bridge take a look at the root
 * complex and compare it to a whitelist of known good hardware.
 */
/*
 * host_bridge_whitelist:
 *   NVMe: 두 PCI 장치(예: NVMe provider와 NVMe/RDMA/GPU client)의 host
 *   bridge를 whitelist와 비교. 같은 host bridge이거나 양쪽 모두 whitelist에
 *   있으면 THRU_HOST_BRIDGE 매핑 허용.
 */
static bool host_bridge_whitelist(struct pci_dev *a, struct pci_dev *b,	/* NVMe: 두 PCI 장치의 host bridge whitelist 비교 함수 선언 */
				  bool warn)	/* NVMe: host_bridge_whitelist의 매개변수 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_host_bridge *host_a = pci_find_host_bridge(a->bus);	/* NVMe: 장치 a의 host bridge 획득 */
	struct pci_host_bridge *host_b = pci_find_host_bridge(b->bus);	/* NVMe: 장치 b의 host bridge 획득 */

	if (host_a == host_b)	/* NVMe: 같은 host bridge에 연결된 경우 */
		return __host_bridge_whitelist(host_a, true, warn);	/* NVMe: 같은 host bridge면 same_bridge=true로 검사 */

	if (__host_bridge_whitelist(host_a, false, warn) &&	/* NVMe: 양쪽 host bridge가 whitelist에 있는지 검사 */
	    __host_bridge_whitelist(host_b, false, warn))	/* NVMe: __host_bridge_whitelist() 호출 */
		return true;					/* NVMe: 서로 다른 host bridge지만 양쪽 모두 whitelist에 있으면 허용 */

	return false;						/* NVMe: whitelist 조건 불만족 */
}	/* NVMe: 코드 블록 종료 */

/*
 * map_types_idx:
 *   NVMe: client PCI 장치를 xarray 인덱스로 변환. (domain << 16) | BDF.
 */
static unsigned long map_types_idx(struct pci_dev *client)	/* NVMe: client를 xarray 인덱스로 변환하는 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	return (pci_domain_nr(client->bus) << 16) | pci_dev_id(client);	/* NVMe: domain 번호와 BDF를 합쳐 고유 인덱스 생성 */
}	/* NVMe: 코드 블록 종료 */

/*
 * Calculate the P2PDMA mapping type and distance between two PCI devices.
 *
 * If the two devices are the same PCI function, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 0.
 *
 * If they are two functions of the same device, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 2 (one hop up to the bridge,
 * then one hop back down to another function of the same device).
 *
 * In the case where two devices are connected to the same PCIe switch,
 * return a distance of 4. This corresponds to the following PCI tree:
 *
 *     -+  Root Port
 *      \+ Switch Upstream Port
 *       +-+ Switch Downstream Port 0
 *       + \- Device A
 *       \-+ Switch Downstream Port 1
 *         \- Device B
 *
 * The distance is 4 because we traverse from Device A to Downstream Port 0
 * to the common Switch Upstream Port, back down to Downstream Port 1 and
 * then to Device B. The mapping type returned depends on the ACS
 * redirection setting of the ports along the path.
 *
 * If ACS redirect is set on any port in the path, traffic between the
 * devices will go through the host bridge, so return
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE; otherwise return
 * PCI_P2PDMA_MAP_BUS_ADDR.
 *
 * Any two devices that have a data path that goes through the host bridge
 * will consult a whitelist. If the host bridge is in the whitelist, return
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE with the distance set to the number of
 * ports per above. If the device is not in the whitelist, return
 * PCI_P2PDMA_MAP_NOT_SUPPORTED.
 */
/*
 * calc_map_type_and_dist:
 *   NVMe: P2PDMA provider(NVMe CMB 등)와 client 간 PCIe 경로를 분석하여
 *   매핑 타입(BUS_ADDR, THRU_HOST_BRIDGE, NOT_SUPPORTED)과 거리를 계산.
 *   SR-IOV, 멀티플 NVMe, switch 직접 연결, ACS redirect, IOMMU/ATS 상황을
 *   종합적으로 판단한다.
 */
static enum pci_p2pdma_map_type	/* NVMe: P2PDMA 매핑 타입 및 거리 계산 함수 선언 */
calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,	/* NVMe: calc_map_type_and_dist의 provider/client 인자 */
		int *dist, bool verbose)	/* NVMe: calc_map_type_and_dist의 거리/verbose 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	enum pci_p2pdma_map_type map_type = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;	/* NVMe: 기본적으로 host bridge 경로 가정 */
	struct pci_dev *a = provider, *b = client, *bb;				/* NVMe: provider/client 및 임시 포인터 */
	bool acs_redirects = false;						/* NVMe: 경로상 ACS redirect 발견 여부 */
	struct pci_p2pdma *p2pdma;						/* NVMe: provider의 P2PDMA 상태 */
	struct seq_buf acs_list;						/* NVMe: ACS redirect bridge BDF 목록 */
	int acs_cnt = 0;							/* NVMe: ACS redirect bridge 개수 */
	int dist_a = 0;								/* NVMe: provider에서 공통 bridge까지 거리 */
	int dist_b = 0;								/* NVMe: client에서 공통 bridge까지 거리 */
	char buf[128];								/* NVMe: ACS 목록 버퍼 */

	seq_buf_init(&acs_list, buf, sizeof(buf));					/* NVMe: seq_buf 초기화 */

	/*
	 * Note, we don't need to take references to devices returned by
	 * pci_upstream_bridge() seeing we hold a reference to a child
	 * device which will already hold a reference to the upstream bridge.
	 */
	while (a) {									/* NVMe: provider 쪽에서 root 방향으로 upstream bridge 순회 */
		dist_b = 0;								/* NVMe: client 쪽 거리 초기화 */

		if (pci_bridge_has_acs_redir(a)) {					/* NVMe: 현재 bridge에 ACS redirect가 설정되어 있으면 */
			seq_buf_print_bus_devfn(&acs_list, a);				/* NVMe: ACS 목록에 BDF 추가 */
			acs_cnt++;							/* NVMe: ACS redirect 카운트 증가 */
		}	/* NVMe: 코드 블록 종료 */

		bb = b;									/* NVMe: client 포인터 복사 */

		while (bb) {								/* NVMe: client 쪽에서 공통 bridge 탐색 */
			if (a == bb)	/* NVMe: 공통 upstream bridge 발견 */
				goto check_b_path_acs;					/* NVMe: 공통 upstream bridge 발견 */

			bb = pci_upstream_bridge(bb);					/* NVMe: client 쪽을 upstream으로 이동 */
			dist_b++;							/* NVMe: client 쪽 거리 증가 */
		}	/* NVMe: 코드 블록 종료 */

		a = pci_upstream_bridge(a);						/* NVMe: provider 쪽을 upstream으로 이동 */
		dist_a++;								/* NVMe: provider 쪽 거리 증가 */
	}	/* NVMe: 코드 블록 종료 */

	*dist = dist_a + dist_b;							/* NVMe: 공통 bridge를 찾지 못하면 총 거리는 provider->root + client->root */
	goto map_through_host_bridge;							/* NVMe: host bridge 경로로 이동 */

check_b_path_acs:	/* NVMe: client 경로 ACS redirect 검사 레이블 */
	bb = b;										/* NVMe: client 쪽 재탐색 */

	while (bb) {									/* NVMe: 공통 bridge까지 client 경로상 ACS redirect 확인 */
		if (a == bb)	/* NVMe: 공통 upstream bridge 도달 */
			break;								/* NVMe: 공통 bridge 도달 */

		if (pci_bridge_has_acs_redir(bb)) {					/* NVMe: client 경로상 bridge에 ACS redirect가 있으면 */
			seq_buf_print_bus_devfn(&acs_list, bb);				/* NVMe: ACS 목록에 BDF 추가 */
			acs_cnt++;							/* NVMe: ACS redirect 카운트 증가 */
		}	/* NVMe: 코드 블록 종료 */

		bb = pci_upstream_bridge(bb);						/* NVMe: upstream으로 이동 */
	}	/* NVMe: 코드 블록 종료 */

	*dist = dist_a + dist_b;							/* NVMe: provider-client 간 hop 수 계산 */

	if (!acs_cnt) {	/* NVMe: ACS redirect가 없으면 switch 내 직접 P2P 가능 */
		map_type = PCI_P2PDMA_MAP_BUS_ADDR;					/* NVMe: ACS redirect가 없으면 switch 내 직접 P2P 가능 */
		goto done;								/* NVMe: 결과 저장 후 종료 */
	}	/* NVMe: 코드 블록 종료 */

	if (verbose) {	/* NVMe: verbose 모드에서 ACS redirect 경고 출력 */
		acs_list.buffer[acs_list.len-1] = 0; /* drop final semicolon */	/* NVMe: 마지막 세미콜론 제거 */
		pci_warn(client, "ACS redirect is set between the client and provider (%s)\n",	/* NVMe: ACS redirect 설정 경고 메시지 */
			 pci_name(provider));						/* NVMe: 사용자/관리자에게 ACS redirect 경고 */
		pci_warn(client, "to disable ACS redirect for this path, add the kernel parameter: pci=disable_acs_redir=%s\n",	/* NVMe: ACS redirect 해제 kernel parameter 안내 메시지 */
			 acs_list.buffer);							/* NVMe: 해결 방법 kernel parameter 제시 */
	}	/* NVMe: 코드 블록 종료 */
	acs_redirects = true;								/* NVMe: ACS redirect가 발견됨을 표시 */

map_through_host_bridge:	/* NVMe: host bridge 경로 P2P 매핑 판정 레이블 */
	if (!cpu_supports_p2pdma() &&	/* NVMe: CPU 지원 및 whitelist 조건 추가 검사 */
	    !host_bridge_whitelist(provider, client, acs_redirects)) {		/* NVMe: CPU 지원도 없고 whitelist에도 없으면 */
		if (verbose)	/* NVMe: P2P 불가 상황에서 verbose 경고 출력 */
			pci_warn(client, "cannot be used for peer-to-peer DMA as the client and provider (%s) do not share an upstream bridge or whitelisted host bridge\n",	/* NVMe: P2P 불가 원인 경고 메시지 */
				 pci_name(provider));					/* NVMe: P2P 불가 경고 */
		map_type = PCI_P2PDMA_MAP_NOT_SUPPORTED;				/* NVMe: P2PDMA 매핑 불가 */
	}	/* NVMe: 코드 블록 종료 */
done:	/* NVMe: 매핑 타입 캐시 저장 후 종료 레이블 */
	rcu_read_lock();								/* NVMe: provider->p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(provider->p2pdma);	/* NVMe: provider의 P2PDMA 상태를 RCU로 획득 */
	if (p2pdma)	/* NVMe: P2PDMA가 유효하면 매핑 타입 캐시 저장 */
		xa_store(&p2pdma->map_types, map_types_idx(client),	/* NVMe: xarray에 client별 매핑 타입 기록 */
			 xa_mk_value(map_type), GFP_ATOMIC);				/* NVMe: 계산된 매핑 타입을 xarray에 캐시(이후 재사용) */
	rcu_read_unlock();								/* NVMe: RCU 읽기 락 해제 */
	return map_type;								/* NVMe: 최종 매핑 타입 반환 */
}	/* NVMe: 코드 블록 종료 */

/**
 * pci_p2pdma_distance_many - Determine the cumulative distance between
 *	a p2pdma provider and the clients in use.
 * @provider: p2pdma provider to check against the client list
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of clients in the array
 * @verbose: if true, print warnings for devices when we return -1
 *
 * Returns -1 if any of the clients are not compatible, otherwise returns a
 * positive number where a lower number is the preferable choice. (If there's
 * one client that's the same as the provider it will return 0, which is best
 * choice).
 *
 * "compatible" means the provider and the clients are either all behind
 * the same PCI root port or the host bridges connected to each of the devices
 * are listed in the 'pci_p2pdma_whitelist'.
 */
/*
 * pci_p2pdma_distance_many:
 *   NVMe: NVMe CMB(provider)와 여러 client들 간의 P2P 호환성 및 누적 거리를
 *   계산. NVMe P2P I/O 경로 선택, dma_pci_p2pdma_supported()에서 사용.
 */
int pci_p2pdma_distance_many(struct pci_dev *provider, struct device **clients,	/* NVMe: provider와 여러 client 간 P2P 거리 계산 함수 선언 */
			     int num_clients, bool verbose)	/* NVMe: pci_p2pdma_distance_many의 client 수/verbose 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	enum pci_p2pdma_map_type map;					/* NVMe: 각 client의 매핑 타입 */
	bool not_supported = false;						/* NVMe: 하나라도 불가능하면 true */
	struct pci_dev *pci_client;						/* NVMe: client의 상위 PCI 장치 */
	int total_dist = 0;							/* NVMe: 누적 거리(작을수록 유리) */
	int i, distance;								/* NVMe: 반복/거리 변수 */

	if (num_clients == 0)	/* NVMe: client가 없으면 거리 의미 없음 */
		return -1;							/* NVMe: client가 없으면 거리 의미 없음 */

	for (i = 0; i < num_clients; i++) {					/* NVMe: 모든 client 순회 */
		pci_client = find_parent_pci_dev(clients[i]);				/* NVMe: client device에서 PCI 장치 추출 */
		if (!pci_client) {							/* NVMe: client가 PCI 장치가 아니면 */
			if (verbose)	/* NVMe: 비PCI 장치에 대한 verbose 경고 */
				dev_warn(clients[i],	/* NVMe: P2P 사용 불가 경고 메시지 */
					 "cannot be used for peer-to-peer DMA as it is not a PCI device\n");	/* NVMe: PCI가 아닌 client 경고 */
			return -1;							/* NVMe: P2P 불가 */
		}	/* NVMe: 코드 블록 종료 */

		map = calc_map_type_and_dist(provider, pci_client, &distance,	/* NVMe: provider-client 간 매핑 타입 및 거리 계산 */
				     verbose);						/* NVMe: provider-client 간 매핑 타입 및 거리 계산 */

		pci_dev_put(pci_client);						/* NVMe: find_parent_pci_dev로 증가된 참조 해제 */

		if (map == PCI_P2PDMA_MAP_NOT_SUPPORTED)	/* NVMe: P2P 매핑 불가능 플래그 설정 */
			not_supported = true;						/* NVMe: 매핑 불가 플래그 설정 */

		if (not_supported && !verbose)	/* NVMe: 비verbose 모드에서 첫 실패 후 빠른 종료 */
			break;								/* NVMe: verbose가 아니면 첫 불가에서 중단 */

		total_dist += distance;							/* NVMe: 거리 누적 */
	}	/* NVMe: 코드 블록 종료 */

	if (not_supported)	/* NVMe: 하나라도 불가능하면 -1 반환 */
		return -1;								/* NVMe: 하나라도 불가능하면 -1 반환 */

	return total_dist;								/* NVMe: 누적 거리 반환(0이면 최적) */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pdma_distance_many);	/* NVMe: pci_p2pdma_distance_many 심볼을 외부 모듈에 노출 */

/**
 * pci_has_p2pmem - check if a given PCI device has published any p2pmem
 * @pdev: PCI device to check
 */
/*
 * pci_has_p2pmem:
 *   NVMe: 해당 PCI 장치(NVMe controller)가 P2P 메모리를 공개했는지 확인.
 *   pci_p2pmem_find_many()에서 provider 후보 필터링에 사용.
 */
static bool pci_has_p2pmem(struct pci_dev *pdev)	/* NVMe: 해당 PCI 장치가 P2P 메모리를 공개했는지 확인 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma *p2pdma;					/* NVMe: P2PDMA 상태 포인터 */
	bool res;								/* NVMe: 반환값 */

	rcu_read_lock();							/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: 해당 PCI 장치의 P2PDMA 상태 조회 */
	res = p2pdma && p2pdma->p2pmem_published;				/* NVMe: P2PDMA 초기화되고 공개되었는지 검사 */
	rcu_read_unlock();							/* NVMe: RCU 읽기 락 해제 */

	return res;								/* NVMe: 공개 여부 반환 */
}	/* NVMe: 코드 블록 종료 */

/**
 * pci_p2pmem_find_many - find a peer-to-peer DMA memory device compatible with
 *	the specified list of clients and shortest distance
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of client devices in the list
 *
 * If multiple devices are behind the same switch, the one "closest" to the
 * client devices in use will be chosen first. (So if one of the providers is
 * the same as one of the clients, that provider will be used ahead of any
 * other providers that are unrelated). If multiple providers are an equal
 * distance away, one will be chosen at random.
 *
 * Returns a pointer to the PCI device with a reference taken (use pci_dev_put
 * to return the reference) or NULL if no compatible device is found. The
 * found provider will also be assigned to the client list.
 */
/*
 * pci_p2pmem_find_many:
 *   NVMe: 주어진 client 목록과 가장 가까운 P2P 메모리 provider(NVMe CMB 등)를
 *   검색. 거리가 같으면 무작위로 선택. NVMe P2P offload 타겟 선정에 사용.
 */
struct pci_dev *pci_p2pmem_find_many(struct device **clients, int num_clients)	/* NVMe: client 목록과 가장 가까운 P2P provider 검색 함수 선언 */
{	/* NVMe: pci_dev 정의 블록 시작 */
	struct pci_dev *pdev = NULL;						/* NVMe: 현재 순회 중인 PCI 장치 */
	int distance;								/* NVMe: 현재 provider와의 거리 */
	int closest_distance = INT_MAX;						/* NVMe: 현재까지 가장 가까운 거리 */
	struct pci_dev **closest_pdevs;						/* NVMe: 동일 최단 거리를 가진 provider 목록 */
	int dev_cnt = 0;								/* NVMe: 동일 최단 거리 provider 개수 */
	const int max_devs = PAGE_SIZE / sizeof(*closest_pdevs);			/* NVMe: 한 페이지에 담을 수 있는 최대 pci_dev 포인터 수 */
	int i;									/* NVMe: 반복 변수 */

	closest_pdevs = kmalloc(PAGE_SIZE, GFP_KERNEL);					/* NVMe: 후보 provider 포인터 배열 할당 */
	if (!closest_pdevs)	/* NVMe: 후보 provider 배열 할당 실패 처리 */
		return NULL;								/* NVMe: 메모리 부족 시 NULL */

	for_each_pci_dev(pdev) {							/* NVMe: 시스템의 모든 PCI 장치 순회 */
		if (!pci_has_p2pmem(pdev))	/* NVMe: P2P 메모리를 공개하지 않은 장치는 스킵 */
			continue;							/* NVMe: P2P 메모리를 공개하지 않은 장치는 스킵 */

		distance = pci_p2pdma_distance_many(pdev, clients,	/* NVMe: 현재 provider와 모든 client 간 누적 거리 계산 */
					    num_clients, false);				/* NVMe: 현재 provider와 모든 client 간 누적 거리 계산 */
		if (distance < 0 || distance > closest_distance)	/* NVMe: 불가능하거나 더 먼 provider는 스킵 */
			continue;							/* NVMe: 불가능하거나 더 멀면 스킵 */

		if (distance == closest_distance && dev_cnt >= max_devs)	/* NVMe: 후보 배열이 가득 찬 경우 추가 스킵 */
			continue;							/* NVMe: 후보 배열이 가득 찼으면 추가 스킵 */

		if (distance < closest_distance) {					/* NVMe: 더 가까운 provider 발견 */
			for (i = 0; i < dev_cnt; i++)	/* NVMe: 기존 후보들의 참조 해제 */
				pci_dev_put(closest_pdevs[i]);				/* NVMe: 기존 후보들의 참조 해제 */

			dev_cnt = 0;							/* NVMe: 후보 카운트 초기화 */
			closest_distance = distance;					/* NVMe: 최단 거리 갱신 */
		}	/* NVMe: pci_dev 정의 블록 종료 */

		closest_pdevs[dev_cnt++] = pci_dev_get(pdev);				/* NVMe: 새 후보에 참조 증가하여 저장 */
	}	/* NVMe: 코드 블록 종료 */

	if (dev_cnt)	/* NVMe: 후보가 있으면 무작위로 하나 선택 */
		pdev = pci_dev_get(closest_pdevs[get_random_u32_below(dev_cnt)]);	/* NVMe: 동일 최단 거리 후보 중 무작위 선택, 참조 증가 */

	for (i = 0; i < dev_cnt; i++)	/* NVMe: 후보 배열의 참조 해제 */
		pci_dev_put(closest_pdevs[i]);						/* NVMe: 후보 배열의 참조 해제(선택된 pdev는 별도 참조 유지) */

	kfree(closest_pdevs);								/* NVMe: 후보 배열 메모리 해제 */
	return pdev;									/* NVMe: 선택된 provider pci_dev 반환(참조 증가됨) 또는 NULL */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pmem_find_many);	/* NVMe: pci_p2pmem_find_many 심볼을 외부 모듈에 노출 */

/**
 * pci_alloc_p2pmem - allocate peer-to-peer DMA memory
 * @pdev: the device to allocate memory from
 * @size: number of bytes to allocate
 *
 * Returns the allocated memory or NULL on error.
 */
/*
 * pci_alloc_p2pmem:
 *   NVMe: NVMe controller의 P2P 메모리 풀(예: CMB)에서 메모리를 할당.
 *   NVMe PCIe host driver는 이 메모리를 SQ, PRP list, SGL 등에 사용 가능.
 */
void *pci_alloc_p2pmem(struct pci_dev *pdev, size_t size)	/* NVMe: P2P 메모리 할당 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	void *ret = NULL;								/* NVMe: 반환 주소, 기본 NULL */
	struct percpu_ref *ref;								/* NVMe: 할당된 페이지의 생명주기 참조 */
	struct pci_p2pdma *p2pdma;							/* NVMe: P2PDMA 상태 포인터 */

	/*
	 * Pairs with synchronize_rcu() in pci_p2pdma_release() to
	 * ensure pdev->p2pdma is non-NULL for the duration of the
	 * read-lock.
	 */
	rcu_read_lock();								/* NVMe: p2pdma RCU 안전 읽기 시작 */
	p2pdma = rcu_dereference(pdev->p2pdma);						/* NVMe: NVMe 장치의 P2PDMA 상태 획득 */
	if (unlikely(!p2pdma))	/* NVMe: P2PDMA가 초기화되지 않은 경우 처리 */
		goto out;								/* NVMe: P2PDMA가 없으면 종료 */

	ret = (void *)gen_pool_alloc_owner(p2pdma->pool, size, (void **) &ref);	/* NVMe: 풀에서 size 바이트 할당, 소유자 ref 획득 */
	if (!ret)	/* NVMe: P2P 메모리 할당 실패 시 종료 */
		goto out;								/* NVMe: 할당 실패 시 종료 */

	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {				/* NVMe: 페이지 생명주기 참조 획득 실패 시(해제 중) */
		gen_pool_free(p2pdma->pool, (unsigned long) ret, size);			/* NVMe: 할당한 메모리 즉시 반납 */
		ret = NULL;								/* NVMe: NULL로 변경 */
	}	/* NVMe: 코드 블록 종료 */
out:	/* NVMe: 함수 종료/에러 반환 경로 */
	rcu_read_unlock();								/* NVMe: RCU 읽기 락 해제 */
	return ret;									/* NVMe: 할당된 주소 또는 NULL 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_alloc_p2pmem);	/* NVMe: pci_alloc_p2pmem 심볼을 외부 모듈에 노출 */

/**
 * pci_free_p2pmem - free peer-to-peer DMA memory
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 * @size: number of bytes that were allocated
 */
/*
 * pci_free_p2pmem:
 *   NVMe: pci_alloc_p2pmem()으로 할당한 CMB/P2P 메모리를 반납.
 *   NVMe SQ 해제 등에서 사용.
 */
void pci_free_p2pmem(struct pci_dev *pdev, void *addr, size_t size)	/* NVMe: P2P 메모리 해제 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct percpu_ref *ref;								/* NVMe: 페이지 생명주기 참조 */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);	/* NVMe: 쓰기 보호 상태에서 P2PDMA 상태 획득 */

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)addr, size,	/* NVMe: P2P 풀에서 addr/size 영역 반납 및 ref 복원 */
			    (void **) &ref);							/* NVMe: 풀에서 addr/size 영역 반납, ref 복원 */
	percpu_ref_put(ref);									/* NVMe: 페이지 생명주기 참조 해제 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_free_p2pmem);	/* NVMe: pci_free_p2pmem 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pmem_virt_to_bus - return the PCI bus address for a given virtual
 *	address obtained with pci_alloc_p2pmem()
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 */
/*
 * pci_p2pmem_virt_to_bus:
 *   NVMe: pci_alloc_p2pmem()으로 할당한 가상 주소를 NVMe DMA에 사용할
 *   PCI bus 주소로 변환. PRP/SGL entry 채움에 사용.
 */
pci_bus_addr_t pci_p2pmem_virt_to_bus(struct pci_dev *pdev, void *addr)	/* NVMe: P2P 가상 주소를 PCI bus 주소로 변환 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma *p2pdma;							/* NVMe: P2PDMA 상태 포인터 */

	if (!addr)	/* NVMe: NULL 주소는 0으로 반환 */
		return 0;								/* NVMe: NULL 주소는 0 반환 */

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);				/* NVMe: 쓰기 보호 상태에서 P2PDMA 상태 획득 */
	if (!p2pdma)	/* NVMe: P2PDMA가 없으면 0 반환 */
		return 0;								/* NVMe: P2PDMA가 없으면 0 반환 */

	/*
	 * Note: when we added the memory to the pool we used the PCI
	 * bus address as the physical address. So gen_pool_virt_to_phys()
	 * actually returns the bus address despite the misleading name.
	 */
	return gen_pool_virt_to_phys(p2pdma->pool, (unsigned long)addr);		/* NVMe: 가상 주소에 대응하는 PCI bus 주소 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pmem_virt_to_bus);	/* NVMe: pci_p2pmem_virt_to_bus 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pmem_alloc_sgl - allocate peer-to-peer DMA memory in a scatterlist
 * @pdev: the device to allocate memory from
 * @nents: the number of SG entries in the list
 * @length: number of bytes to allocate
 *
 * Return: %NULL on error or &struct scatterlist pointer and @nents on success
 */
/*
 * pci_p2pmem_alloc_sgl:
 *   NVMe: P2P 메모리를 하나의 scatterlist entry로 할당. NVMe SGL/PRP
 *   구성에 필요한 메모리 버퍼를 CMB에 배치할 때 사용.
 */
struct scatterlist *pci_p2pmem_alloc_sgl(struct pci_dev *pdev,	/* NVMe: P2P 메모리 scatterlist 할당 함수 선언 */
					 unsigned int *nents, u32 length)	/* NVMe: pci_p2pmem_alloc_sgl의 nents/length 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct scatterlist *sg;								/* NVMe: scatterlist 객체 */
	void *addr;										/* NVMe: 할당된 P2P 메모리 주소 */

	sg = kmalloc_obj(*sg);								/* NVMe: 단일 scatterlist 객체 할당 */
	if (!sg)	/* NVMe: scatterlist 객체 할당 실패 처리 */
		return NULL;								/* NVMe: 메모리 부족 시 NULL */

	sg_init_table(sg, 1);								/* NVMe: scatterlist 1개 entry 초기화 */

	addr = pci_alloc_p2pmem(pdev, length);						/* NVMe: P2P 풀에서 length 바이트 할당 */
	if (!addr)	/* NVMe: P2P 메모리 할당 실패 시 scatterlist 해제 */
		goto out_free_sg;								/* NVMe: 할당 실패 시 scatterlist 해제 */

	sg_set_buf(sg, addr, length);							/* NVMe: scatterlist 버퍼 주소/길이 설정 */
	*nents = 1;										/* NVMe: entry 수 1 */
	return sg;										/* NVMe: 구성된 scatterlist 반환 */

out_free_sg:	/* NVMe: scatterlist 객체 해제 경로 */
	kfree(sg);										/* NVMe: scatterlist 객체 해제 */
	return NULL;										/* NVMe: NULL 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pmem_alloc_sgl);	/* NVMe: pci_p2pmem_alloc_sgl 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pmem_free_sgl - free a scatterlist allocated by pci_p2pmem_alloc_sgl()
 * @pdev: the device to allocate memory from
 * @sgl: the allocated scatterlist
 */
/*
 * pci_p2pmem_free_sgl:
 *   NVMe: pci_p2pmem_alloc_sgl()으로 할당한 scatterlist와 그 P2P 메모리를
 *   모두 해제.
 */
void pci_p2pmem_free_sgl(struct pci_dev *pdev, struct scatterlist *sgl)	/* NVMe: scatterlist와 P2P 메모리 해제 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct scatterlist *sg;								/* NVMe: 순회용 scatterlist 포인터 */
	int count;										/* NVMe: 순회 카운트 */

	for_each_sg(sgl, sg, INT_MAX, count) {						/* NVMe: scatterlist 순회 */
		if (!sg)	/* NVMe: scatterlist 끝에 도달 */
			break;									/* NVMe: 리스트 끝(널 포인터) 도달 */

		pci_free_p2pmem(pdev, sg_virt(sg), sg->length);				/* NVMe: 각 entry의 P2P 메모리 반납 */
	}	/* NVMe: 코드 블록 종료 */
	kfree(sgl);											/* NVMe: scatterlist 객체 해제 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pmem_free_sgl);	/* NVMe: pci_p2pmem_free_sgl 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pmem_publish - publish the peer-to-peer DMA memory for use by
 *	other devices with pci_p2pmem_find()
 * @pdev: the device with peer-to-peer DMA memory to publish
 * @publish: set to true to publish the memory, false to unpublish it
 *
 * Published memory can be used by other PCI device drivers for
 * peer-2-peer DMA operations. Non-published memory is reserved for
 * exclusive use of the device driver that registers the peer-to-peer
 * memory.
 */
/*
 * pci_p2pmem_publish:
 *   NVMe: NVMe controller의 CMB P2P 메모리를 다른 endpoint가 검색/사용할
 *   수 있도록 공개(true) 또는 비공개(false). nvme_setup_pci_p2pdma()에서
 *   CMB 등록 후 호출.
 */
void pci_p2pmem_publish(struct pci_dev *pdev, bool publish)	/* NVMe: P2P 메모리 공개/비공개 설정 함수 선언 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct pci_p2pdma *p2pdma;							/* NVMe: P2PDMA 상태 포인터 */

	rcu_read_lock();									/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: 해당 PCI 장치의 P2PDMA 상태 조회 */
	if (p2pdma)	/* NVMe: P2PDMA가 유효하면 공개 플래그 갱신 */
		p2pdma->p2pmem_published = publish;					/* NVMe: 공개 플래그 갱신 */
	rcu_read_unlock();									/* NVMe: RCU 읽기 락 해제 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pmem_publish);	/* NVMe: pci_p2pmem_publish 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pdma_map_type - Determine the mapping type for P2PDMA transfers
 * @provider: P2PDMA provider structure
 * @dev: Target device for the transfer
 *
 * Determines how peer-to-peer DMA transfers should be mapped between
 * the provider and the target device. The mapping type indicates whether
 * the transfer can be done directly through PCI switches or must go
 * through the host bridge.
 */
/*
 * pci_p2pdma_map_type:
 *   NVMe: 특정 P2P provider(예: NVMe CMB)와 대상 device(NVMe/RDMA/GPU) 간
 *   DMA 매핑 타입을 결정. NVMe host driver의 setup/prp/sgl 경로에서 핵심
 *   호출. 캐시된 결과가 있으면 재사용, 없으면 calc_map_type_and_dist() 계산.
 */
enum pci_p2pdma_map_type pci_p2pdma_map_type(struct p2pdma_provider *provider,	/* NVMe: P2P provider와 대상 device 간 매핑 타입 결정 함수 선언 */
					     struct device *dev)	/* NVMe: pci_p2pdma_map_type의 대상 device 인자 */
{	/* NVMe: device 정의 블록 시작 */
	enum pci_p2pdma_map_type type = PCI_P2PDMA_MAP_NOT_SUPPORTED;	/* NVMe: 기본값 불가 */
	struct pci_dev *pdev = to_pci_dev(provider->owner);			/* NVMe: provider 소유 PCI 장치(NVMe controller) */
	struct pci_dev *client;								/* NVMe: 대상 client PCI 장치 */
	struct pci_p2pdma *p2pdma;							/* NVMe: provider의 P2PDMA 상태 */
	int dist;										/* NVMe: 거리(계산 시 사용) */

	if (!pdev->p2pdma)	/* NVMe: provider가 P2PDMA 초기화되지 않음 */
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;					/* NVMe: provider가 P2PDMA 초기화되지 않음 */

	if (!dev_is_pci(dev))	/* NVMe: 대상이 PCI 장치가 아니면 P2P 불가 */
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;					/* NVMe: 대상이 PCI 장치가 아니면 P2P 불가 */

	client = to_pci_dev(dev);								/* NVMe: 대상 device를 pci_dev로 변환 */

	rcu_read_lock();									/* NVMe: p2pdma RCU 안전 읽기 */
	p2pdma = rcu_dereference(pdev->p2pdma);	/* NVMe: provider의 P2PDMA 상태를 RCU로 획득 */

	if (p2pdma)	/* NVMe: P2PDMA가 유효하면 캐시된 매핑 타입 조회 */
		type = xa_to_value(xa_load(&p2pdma->map_types,	/* NVMe: xarray에서 client별 매핑 타입 로드 */
					   map_types_idx(client)));				/* NVMe: client별 캐시된 매핑 타입 조회 */
	rcu_read_unlock();									/* NVMe: RCU 읽기 락 해제 */

	if (type == PCI_P2PDMA_MAP_UNKNOWN)	/* NVMe: 캐시 미스 시 경로 분석(verbose) */
		return calc_map_type_and_dist(pdev, client, &dist, true);	/* NVMe: 캐시 미스 시 경로 분석(verbose) */

	return type;										/* NVMe: 캐시된 또는 계산된 매핑 타입 반환 */
}	/* NVMe: device 정의 블록 종료 */

/*
 * __pci_p2pdma_update_state:
 *   NVMe: 페이지맵을 보고 현재 DMA 상태(provider, map type)를 갱신.
 *   NVMe P2P I/O의 bio/iter 처리 루프에서 호출.
 */
void __pci_p2pdma_update_state(struct pci_p2pdma_map_state *state,	/* NVMe: 페이지맵 기반 DMA 상태 갱신 함수 선언 */
		struct device *dev, struct page *page)	/* NVMe: __pci_p2pdma_update_state의 device/page 인자 */
{	/* NVMe: device 정의 블록 시작 */
	struct pci_p2pdma_pagemap *p2p_pgmap = to_p2p_pgmap(page_pgmap(page));	/* NVMe: 페이지의 p2pdma 페이지맵 획득 */

	if (state->mem == p2p_pgmap->mem)	/* NVMe: provider가 같으면 상태 갱신 불필요 */
		return;											/* NVMe: provider가 같으면 갱신 불필요 */

	state->mem = p2p_pgmap->mem;								/* NVMe: 현재 provider 갱신 */
	state->map = pci_p2pdma_map_type(p2p_pgmap->mem, dev);				/* NVMe: 현재 provider와 대상 device 간 매핑 타입 갱신 */
}	/* NVMe: device 정의 블록 종료 */

/**
 * pci_p2pdma_enable_store - parse a configfs/sysfs attribute store
 *		to enable p2pdma
 * @page: contents of the value to be stored
 * @p2p_dev: returns the PCI device that was selected to be used
 *		(if one was specified in the stored value)
 * @use_p2pdma: returns whether to enable p2pdma or not
 *
 * Parses an attribute value to decide whether to enable p2pdma.
 * The value can select a PCI device (using its full BDF device
 * name) or a boolean (in any format kstrtobool() accepts). A false
 * value disables p2pdma, a true value expects the caller
 * to automatically find a compatible device and specifying a PCI device
 * expects the caller to use the specific provider.
 *
 * pci_p2pdma_enable_show() should be used as the show operation for
 * the attribute.
 *
 * Returns 0 on success
 */
/*
 * pci_p2pdma_enable_store:
 *   NVMe: sysfs/configfs에서 P2PDMA 활성화 속성 쓰기를 파싱. 특정 NVMe
 *   provider를 BDF로 지정하거나, true/false로 자동/비활성화 설정.
 */
int pci_p2pdma_enable_store(const char *page, struct pci_dev **p2p_dev,	/* NVMe: P2PDMA 활성화 속성 쓰기 파싱 함수 선언 */
			    bool *use_p2pdma)	/* NVMe: pci_p2pdma_enable_store의 출력 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	struct device *dev;									/* NVMe: BDF로 찾은 device */

	dev = bus_find_device_by_name(&pci_bus_type, NULL, page);			/* NVMe: 입력 문자열을 PCI BDF 이름으로 검색 */
	if (dev) {											/* NVMe: 일치하는 PCI 장치가 있으면 */
		*use_p2pdma = true;									/* NVMe: P2PDMA 사용으로 설정 */
		*p2p_dev = to_pci_dev(dev);								/* NVMe: 선택된 provider pci_dev 저장 */

		if (!pci_has_p2pmem(*p2p_dev)) {							/* NVMe: 선택된 장치가 P2P 메모리를 공개하지 않았으면 */
			pci_err(*p2p_dev,	/* NVMe: 선택된 provider에 P2P 메모리 없음 에러 출력 */
				"PCI device has no peer-to-peer memory: %s\n",	/* NVMe: P2P 메모리 없음 에러 메시지 */
				page);									/* NVMe: 에러 메시지 출력 */
			pci_dev_put(*p2p_dev);								/* NVMe: 참조 해제 */
			return -ENODEV;									/* NVMe: 장치 없음 에러 */
		}	/* NVMe: 코드 블록 종료 */

		return 0;										/* NVMe: 특정 provider 지정 성공 */
	} else if ((page[0] == '0' || page[0] == '1') && !iscntrl(page[1])) {	/* NVMe: 잘못된 BDF 입력 또는 boolean 아닌 값 검사 */
		/*
		 * If the user enters a PCI device that  doesn't exist
		 * like "0000:01:00.1", we don't want kstrtobool to think
		 * it's a '0' when it's clearly not what the user wanted.
		 * So we require 0's and 1's to be exactly one character.
		 */
	} else if (!kstrtobool(page, use_p2pdma)) {						/* NVMe: 0/1 단일 문자 boolean 파싱 */
		return 0;										/* NVMe: boolean 설정 성공 */
	}	/* NVMe: 코드 블록 종료 */

	pr_err("No such PCI device: %.*s\n", (int)strcspn(page, "\n"), page);	/* NVMe: 해당 PCI 장치를 찾지 못함 */
	return -ENODEV;											/* NVMe: 에러 반환 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_store);	/* NVMe: pci_p2pdma_enable_store 심볼을 외부 모듈에 노출 */

/**
 * pci_p2pdma_enable_show - show a configfs/sysfs attribute indicating
 *		whether p2pdma is enabled
 * @page: contents of the stored value
 * @p2p_dev: the selected p2p device (NULL if no device is selected)
 * @use_p2pdma: whether p2pdma has been enabled
 *
 * Attributes that use pci_p2pdma_enable_store() should use this function
 * to show the value of the attribute.
 *
 * Returns 0 on success
 */
/*
 * pci_p2pdma_enable_show:
 *   NVMe: sysfs/configfs에서 P2PDMA 활성화 상태를 문자열로 출력.
 *   0(비활성), 1(자동 선택), 또는 선택된 provider BDF를 반환.
 */
ssize_t pci_p2pdma_enable_show(char *page, struct pci_dev *p2p_dev,	/* NVMe: P2PDMA 활성화 상태 출력 함수 선언 */
			       bool use_p2pdma)	/* NVMe: pci_p2pdma_enable_show의 활성화 여부 인자 */
{	/* NVMe: 함수 본문 블록 시작 */
	if (!use_p2pdma)	/* NVMe: P2PDMA 비활성 시 0 출력 */
		return sprintf(page, "0\n");							/* NVMe: P2PDMA 비활성 시 "0" 출력 */

	if (!p2p_dev)	/* NVMe: 자동 선택 모드 시 1 출력 */
		return sprintf(page, "1\n");							/* NVMe: 자동 선택 모드 시 "1" 출력 */

	return sprintf(page, "%s\n", pci_name(p2p_dev));					/* NVMe: 지정된 provider BDF 출력 */
}	/* NVMe: 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_show);	/* NVMe: pci_p2pdma_enable_show 심볼을 외부 모듈에 노출 */
