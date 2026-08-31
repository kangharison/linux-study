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

#define pr_fmt(fmt) "pci-p2pdma: " fmt
#include <linux/ctype.h>
#include <linux/dma-map-ops.h>
#include <linux/pci-p2pdma.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/memremap.h>
#include <linux/percpu-refcount.h>
#include <linux/random.h>
#include <linux/seq_buf.h>
#include <linux/xarray.h>

struct pci_p2pdma {
	struct gen_pool *pool;
	bool p2pmem_published;
	struct xarray map_types;
	struct p2pdma_provider mem[PCI_STD_NUM_BARS];
};

struct pci_p2pdma_pagemap {
	struct dev_pagemap pgmap;
	struct p2pdma_provider *mem;
};

static struct pci_p2pdma_pagemap *to_p2p_pgmap(struct dev_pagemap *pgmap)
{
	return container_of(pgmap, struct pci_p2pdma_pagemap, pgmap);
}

/*
 * size_show:
 *   NVMe: sysfs에서 CMB P2P 메모리의 총 크기를 조회한다.
 *   NVMe CMB 크기는 NVMe BAR에 등록된 P2P 풀 전체 크기에 해당.
 */
static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	size_t size = 0;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma && p2pdma->pool)
		size = gen_pool_size(p2pdma->pool);
	rcu_read_unlock();

	return sysfs_emit(buf, "%zd\n", size);
}
static DEVICE_ATTR_RO(size);

/*
 * available_show:
 *   NVMe: sysfs에서 아직 할당되지 않은 CMB P2P 메모리 여유 크기를 조회.
 */
static ssize_t available_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	size_t avail = 0;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma && p2pdma->pool)
		avail = gen_pool_avail(p2pdma->pool);
	rcu_read_unlock();

	return sysfs_emit(buf, "%zd\n", avail);
}
static DEVICE_ATTR_RO(available);

/*
 * published_show:
 *   NVMe: sysfs에서 현재 CMB가 P2P provider로 공개되었는지 조회.
 *   공개된 경우 다른 endpoint가 pci_p2pmem_find_many()로 이 NVMe를 찾을 수 있음.
 */
static ssize_t published_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	bool published = false;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma)
		published = p2pdma->p2pmem_published;
	rcu_read_unlock();

	return sysfs_emit(buf, "%d\n", published);
}
static DEVICE_ATTR_RO(published);

/*
 * p2pmem_alloc_mmap:
 *   NVMe: userspace가 /sys/bus/pci/devices/.../p2pmem/allocate를 mmap으로
 *   매핑할 때 호출. NVMe CMB 영역을 userspace에 직접 노출할 수 있음.
 */
static int p2pmem_alloc_mmap(struct file *filp, struct kobject *kobj,
		const struct bin_attribute *attr, struct vm_area_struct *vma)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));
	size_t len = vma->vm_end - vma->vm_start;
	struct pci_p2pdma *p2pdma;
	struct percpu_ref *ref;
	unsigned long vaddr;
	void *kaddr;
	int ret;

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted private mapping\n",
				     current->comm);
		return -EINVAL;
	}

	if (vma->vm_pgoff) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted mapping with non-zero offset\n",
				     current->comm);
		return -EINVAL;
	}

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (!p2pdma) {
		ret = -ENODEV;
		goto out;
	}

	kaddr = (void *)gen_pool_alloc_owner(p2pdma->pool, len, (void **)&ref);
	if (!kaddr) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * vm_insert_page() can sleep, so a reference is taken to mapping
	 * such that rcu_read_unlock() can be done before inserting the
	 * pages
	 */
	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		ret = -ENODEV;
		goto out_free_mem;
	}
	rcu_read_unlock();

	for (vaddr = vma->vm_start; vaddr < vma->vm_end; vaddr += PAGE_SIZE) {
		struct page *page = virt_to_page(kaddr);

		/*
		 * Initialise the refcount for the freshly allocated page. As
		 * we have just allocated the page no one else should be
		 * using it.
		 */
		VM_WARN_ON_ONCE_PAGE(page_ref_count(page), page);
		set_page_count(page, 1);
		ret = vm_insert_page(vma, vaddr, page);
		if (ret) {
			gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);

			/*
			 * Reset the page count. We don't use put_page()
			 * because we don't want to trigger the
			 * p2pdma_folio_free() path.
			 */
			set_page_count(page, 0);
			percpu_ref_put(ref);
			return ret;
		}
		percpu_ref_get(ref);
		put_page(page);
		kaddr += PAGE_SIZE;
		len -= PAGE_SIZE;
	}

	percpu_ref_put(ref);

	return 0;
out_free_mem:
	gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);
out:
	rcu_read_unlock();
	return ret;
}

static const struct bin_attribute p2pmem_alloc_attr = {
	.attr = { .name = "allocate", .mode = 0660 },
	.mmap = p2pmem_alloc_mmap,
	/*
	 * Some places where we want to call mmap (ie. python) will check
	 * that the file size is greater than the mmap size before allowing
	 * the mmap to continue. To work around this, just set the size
	 * to be very large.
	 */
	.size = SZ_1T,
};

static struct attribute *p2pmem_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_available.attr,
	&dev_attr_published.attr,
	NULL,
};

static const struct bin_attribute *const p2pmem_bin_attrs[] = {
	&p2pmem_alloc_attr,
	NULL,
};

static const struct attribute_group p2pmem_group = {
	.attrs = p2pmem_attrs,
	.bin_attrs = p2pmem_bin_attrs,
	.name = "p2pmem",
};

/*
 * p2pdma_folio_free:
 *   NVMe: ZONE_DEVICE 페이지가 해제될 때 호출. CMB 페이지 반납 처리.
 */
static void p2pdma_folio_free(struct folio *folio)
{
	struct page *page = &folio->page;
	struct pci_p2pdma_pagemap *pgmap = to_p2p_pgmap(page_pgmap(page));
	/* safe to dereference while a reference is held to the percpu ref */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(
		to_pci_dev(pgmap->mem->owner)->p2pdma, 1);
	struct percpu_ref *ref;

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)page_to_virt(page),
			    PAGE_SIZE, (void **)&ref);
	percpu_ref_put(ref);
}

static const struct dev_pagemap_ops p2pdma_pgmap_ops = {
	.folio_free = p2pdma_folio_free,
};

/*
 * pci_p2pdma_release:
 *   NVMe: devm_ 메커니즘으로 P2PDMA 자원 해제 시 호출. NVMe remove 시 실행.
 */
static void pci_p2pdma_release(void *data)
{
	struct pci_dev *pdev = data;
	struct pci_p2pdma *p2pdma;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (!p2pdma)
		return;

	/* Flush and disable pci_alloc_p2p_mem() */
	pdev->p2pdma = NULL;
	if (p2pdma->pool)
		synchronize_rcu();
	xa_destroy(&p2pdma->map_types);

	if (!p2pdma->pool)
		return;

	gen_pool_destroy(p2pdma->pool);
	sysfs_remove_group(&pdev->dev.kobj, &p2pmem_group);
}

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
int pcim_p2pdma_init(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2p;
	int i, ret;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2p)
		return 0;

	p2p = devm_kzalloc(&pdev->dev, sizeof(*p2p), GFP_KERNEL);
	if (!p2p)
		return -ENOMEM;

	xa_init(&p2p->map_types);
	/*
	 * Iterate over all standard PCI BARs and record only those that
	 * correspond to MMIO regions. Skip non-memory resources (e.g. I/O
	 * port BARs) since they cannot be used for peer-to-peer (P2P)
	 * transactions.
	 */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM))
			continue;

		p2p->mem[i].owner = &pdev->dev;
		p2p->mem[i].bus_offset =
			pci_bus_address(pdev, i) - pci_resource_start(pdev, i);
	}

	ret = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_release, pdev);
	if (ret)
		goto out_p2p;

	rcu_assign_pointer(pdev->p2pdma, p2p);
	return 0;

out_p2p:
	devm_kfree(&pdev->dev, p2p);
	return ret;
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_init);

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
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar)
{
	struct pci_p2pdma *p2p;

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return NULL;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (WARN_ON(!p2p))
		/* Someone forgot to call to pcim_p2pdma_init() before */
		return NULL;

	return &p2p->mem[bar];
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_provider);

/*
 * pci_p2pdma_setup_pool:
 *   NVMe: P2P 메모리 할당 풀과 sysfs 그룹을 생성. CMB 등록 직전 호출.
 */
static int pci_p2pdma_setup_pool(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	int ret;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2pdma->pool)
		/* We already setup pools, do nothing, */
		return 0;

	p2pdma->pool = gen_pool_create(PAGE_SHIFT, dev_to_node(&pdev->dev));
	if (!p2pdma->pool)
		return -ENOMEM;

	ret = sysfs_create_group(&pdev->dev.kobj, &p2pmem_group);
	if (ret)
		goto out_pool_destroy;

	return 0;

out_pool_destroy:
	gen_pool_destroy(p2pdma->pool);
	p2pdma->pool = NULL;
	return ret;
}

/*
 * pci_p2pdma_unmap_mappings:
 *   NVMe: 드라이버 제거 시 userspace 매핑을 끊고 새 매핑 차단.
 */
static void pci_p2pdma_unmap_mappings(void *data)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = data;

	/*
	 * Removing the alloc attribute from sysfs will call
	 * unmap_mapping_range() on the inode, teardown any existing userspace
	 * mappings and prevent new ones from being created.
	 */
	sysfs_remove_file_from_group(&p2p_pgmap->mem->owner->kobj,
				     &p2pmem_alloc_attr.attr,
				     p2pmem_group.name);
}

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
int pci_p2pdma_add_resource(struct pci_dev *pdev, int bar, size_t size,
			    u64 offset)
{
	struct pci_p2pdma_pagemap *p2p_pgmap;
	struct p2pdma_provider *mem;
	struct dev_pagemap *pgmap;
	struct pci_p2pdma *p2pdma;
	void *addr;
	int error;

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return -EINVAL;

	if (offset >= pci_resource_len(pdev, bar))
		return -EINVAL;

	if (!size)
		size = pci_resource_len(pdev, bar) - offset;

	if (size + offset > pci_resource_len(pdev, bar))
		return -EINVAL;

	error = pcim_p2pdma_init(pdev);
	if (error)
		return error;

	error = pci_p2pdma_setup_pool(pdev);
	if (error)
		return error;

	mem = pcim_p2pdma_provider(pdev, bar);
	/*
	 * We checked validity of BAR prior to call
	 * to pcim_p2pdma_provider. It should never return NULL.
	 */
	if (WARN_ON(!mem))
		return -EINVAL;

	p2p_pgmap = devm_kzalloc(&pdev->dev, sizeof(*p2p_pgmap), GFP_KERNEL);
	if (!p2p_pgmap)
		return -ENOMEM;

	pgmap = &p2p_pgmap->pgmap;
	pgmap->range.start = pci_resource_start(pdev, bar) + offset;
	pgmap->range.end = pgmap->range.start + size - 1;
	pgmap->nr_range = 1;
	pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;
	pgmap->ops = &p2pdma_pgmap_ops;
	p2p_pgmap->mem = mem;

	addr = devm_memremap_pages(&pdev->dev, pgmap);
	if (IS_ERR(addr)) {
		error = PTR_ERR(addr);
		goto pgmap_free;
	}

	error = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_unmap_mappings,
					 p2p_pgmap);
	if (error)
		goto pages_free;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	error = gen_pool_add_owner(p2pdma->pool, (unsigned long)addr,
			pci_bus_address(pdev, bar) + offset,
			range_len(&pgmap->range), dev_to_node(&pdev->dev),
			&pgmap->ref);
	if (error)
		goto pages_free;

	pci_info(pdev, "added peer-to-peer DMA memory %#llx-%#llx\n",
		 pgmap->range.start, pgmap->range.end);

	return 0;

pages_free:
	devm_memunmap_pages(&pdev->dev, pgmap);
pgmap_free:
	devm_kfree(&pdev->dev, p2p_pgmap);
	return error;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_add_resource);

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
static struct pci_dev *find_parent_pci_dev(struct device *dev)
{
	struct device *parent;

	dev = get_device(dev);

	while (dev) {
		if (dev_is_pci(dev))
			return to_pci_dev(dev);

		parent = get_device(dev->parent);
		put_device(dev);
		dev = parent;
	}

	return NULL;
}

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
static int pci_bridge_has_acs_redir(struct pci_dev *pdev)
{
	int pos;
	u16 ctrl;

	pos = pdev->acs_cap;
	if (!pos)
		return 0;

	pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl);

	if (ctrl & (PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC))
		return 1;

	return 0;
}

/*
 * seq_buf_print_bus_devfn:
 *   NVMe: ACS redirect가 설정된 bridge의 BDF를 문자열 버퍼에 추가.
 *   kernel parameter pci=disable_acs_redir=...에 사용될 목록 구성.
 */
static void seq_buf_print_bus_devfn(struct seq_buf *buf, struct pci_dev *pdev)
{
	if (!buf)
		return;

	seq_buf_printf(buf, "%s;", pci_name(pdev));
}

/*
 * cpu_supports_p2pdma:
 *   NVMe: CPU/IMC(Integrated Memory Controller)가 P2PDMA를 지원하는지 확인.
 *   현재는 AMD Zen 이상에서만 true를 반환. whitelist 방식의 보조.
 */
static bool cpu_supports_p2pdma(void)
{
#ifdef CONFIG_X86
	struct cpuinfo_x86 *c = &cpu_data(0);

	/* Any AMD CPU whose family ID is Zen or newer supports p2pdma */
	if (c->x86_vendor == X86_VENDOR_AMD && c->x86 >= 0x17)
		return true;
#endif

	return false;
}

/*
 * NVMe: host bridge whitelist. P2P가 같은 upstream bridge를 공유하지 않을
 * 때라도 이 목록의 Root Complex에서는 THRU_HOST_BRIDGE로 허용.
 * SR-IOV, 멀티포트 NVMe, RDMA 등에서 같은 Root Complex 내 P2P 활용 시
 * 중요.
 */
static const struct pci_p2pdma_whitelist_entry {
	unsigned short vendor;
	int device;
	enum {
		REQ_SAME_HOST_BRIDGE	= 1 << 0,
	} flags;
} pci_p2pdma_whitelist[] = {
	/* Intel Xeon E5/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x3c00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x3c01, REQ_SAME_HOST_BRIDGE},
	/* Intel Xeon E7 v3/Xeon E5 v3/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x2f00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x2f01, REQ_SAME_HOST_BRIDGE},
	/* Intel Skylake-E */
	{PCI_VENDOR_ID_INTEL,	0x2030, 0},
	{PCI_VENDOR_ID_INTEL,	0x2031, 0},
	{PCI_VENDOR_ID_INTEL,	0x2032, 0},
	{PCI_VENDOR_ID_INTEL,	0x2033, 0},
	{PCI_VENDOR_ID_INTEL,	0x2020, 0},
	{PCI_VENDOR_ID_INTEL,	0x09a2, 0},
	/* Google SoCs. */
	{PCI_VENDOR_ID_GOOGLE,	PCI_ANY_ID, 0},
	{}
};

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
static struct pci_dev *pci_host_bridge_dev(struct pci_host_bridge *host)
{
	struct pci_dev *root;

	root = list_first_entry_or_null(&host->bus->devices,
					struct pci_dev, bus_list);

	if (!root)
		return NULL;

	if (root->devfn == PCI_DEVFN(0, 0))
		return root;

	if (pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT)
		return root;

	return NULL;
}

/*
 * __host_bridge_whitelist:
 *   NVMe: 특정 host bridge가 P2PDMA whitelist에 있는지 확인.
 *   REQ_SAME_HOST_BRIDGE 플래그가 있으면 같은 host bridge 내에서만 허용.
 */
static bool __host_bridge_whitelist(struct pci_host_bridge *host,
				    bool same_host_bridge, bool warn)
{
	struct pci_dev *root = pci_host_bridge_dev(host);
	const struct pci_p2pdma_whitelist_entry *entry;
	unsigned short vendor, device;

	if (!root)
		return false;

	vendor = root->vendor;
	device = root->device;

	for (entry = pci_p2pdma_whitelist; entry->vendor; entry++) {
		if (vendor != entry->vendor)
			continue;

		if (entry->device != PCI_ANY_ID && device != entry->device)
			continue;

		if (entry->flags & REQ_SAME_HOST_BRIDGE && !same_host_bridge)
			return false;

		return true;
	}

	if (warn)
		pci_warn(root, "Host bridge not in P2PDMA whitelist: %04x:%04x\n",
			 vendor, device);

	return false;
}

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
static bool host_bridge_whitelist(struct pci_dev *a, struct pci_dev *b,
				  bool warn)
{
	struct pci_host_bridge *host_a = pci_find_host_bridge(a->bus);
	struct pci_host_bridge *host_b = pci_find_host_bridge(b->bus);

	if (host_a == host_b)
		return __host_bridge_whitelist(host_a, true, warn);

	if (__host_bridge_whitelist(host_a, false, warn) &&
	    __host_bridge_whitelist(host_b, false, warn))
		return true;

	return false;
}

/*
 * map_types_idx:
 *   NVMe: client PCI 장치를 xarray 인덱스로 변환. (domain << 16) | BDF.
 */
static unsigned long map_types_idx(struct pci_dev *client)
{
	return (pci_domain_nr(client->bus) << 16) | pci_dev_id(client);
}

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
static enum pci_p2pdma_map_type
calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,
		int *dist, bool verbose)
{
	enum pci_p2pdma_map_type map_type = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;
	struct pci_dev *a = provider, *b = client, *bb;
	bool acs_redirects = false;
	struct pci_p2pdma *p2pdma;
	struct seq_buf acs_list;
	int acs_cnt = 0;
	int dist_a = 0;
	int dist_b = 0;
	char buf[128];

	seq_buf_init(&acs_list, buf, sizeof(buf));

	/*
	 * Note, we don't need to take references to devices returned by
	 * pci_upstream_bridge() seeing we hold a reference to a child
	 * device which will already hold a reference to the upstream bridge.
	 */
	while (a) {
		dist_b = 0;

		if (pci_bridge_has_acs_redir(a)) {
			seq_buf_print_bus_devfn(&acs_list, a);
			acs_cnt++;
		}

		bb = b;

		while (bb) {
			if (a == bb)
				goto check_b_path_acs;

			bb = pci_upstream_bridge(bb);
			dist_b++;
		}

		a = pci_upstream_bridge(a);
		dist_a++;
	}

	*dist = dist_a + dist_b;
	goto map_through_host_bridge;

check_b_path_acs:
	bb = b;

	while (bb) {
		if (a == bb)
			break;

		if (pci_bridge_has_acs_redir(bb)) {
			seq_buf_print_bus_devfn(&acs_list, bb);
			acs_cnt++;
		}

		bb = pci_upstream_bridge(bb);
	}

	*dist = dist_a + dist_b;

	if (!acs_cnt) {
		map_type = PCI_P2PDMA_MAP_BUS_ADDR;
		goto done;
	}

	if (verbose) {
		acs_list.buffer[acs_list.len-1] = 0; /* drop final semicolon */
		pci_warn(client, "ACS redirect is set between the client and provider (%s)\n",
			 pci_name(provider));
		pci_warn(client, "to disable ACS redirect for this path, add the kernel parameter: pci=disable_acs_redir=%s\n",
			 acs_list.buffer);
	}
	acs_redirects = true;

map_through_host_bridge:
	if (!cpu_supports_p2pdma() &&
	    !host_bridge_whitelist(provider, client, acs_redirects)) {
		if (verbose)
			pci_warn(client, "cannot be used for peer-to-peer DMA as the client and provider (%s) do not share an upstream bridge or whitelisted host bridge\n",
				 pci_name(provider));
		map_type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	}
done:
	rcu_read_lock();
	p2pdma = rcu_dereference(provider->p2pdma);
	if (p2pdma)
		xa_store(&p2pdma->map_types, map_types_idx(client),
			 xa_mk_value(map_type), GFP_ATOMIC);
	rcu_read_unlock();
	return map_type;
}

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
int pci_p2pdma_distance_many(struct pci_dev *provider, struct device **clients,
			     int num_clients, bool verbose)
{
	enum pci_p2pdma_map_type map;
	bool not_supported = false;
	struct pci_dev *pci_client;
	int total_dist = 0;
	int i, distance;

	if (num_clients == 0)
		return -1;

	for (i = 0; i < num_clients; i++) {
		pci_client = find_parent_pci_dev(clients[i]);
		if (!pci_client) {
			if (verbose)
				dev_warn(clients[i],
					 "cannot be used for peer-to-peer DMA as it is not a PCI device\n");
			return -1;
		}

		map = calc_map_type_and_dist(provider, pci_client, &distance,
				     verbose);

		pci_dev_put(pci_client);

		if (map == PCI_P2PDMA_MAP_NOT_SUPPORTED)
			not_supported = true;

		if (not_supported && !verbose)
			break;

		total_dist += distance;
	}

	if (not_supported)
		return -1;

	return total_dist;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_distance_many);

/**
 * pci_has_p2pmem - check if a given PCI device has published any p2pmem
 * @pdev: PCI device to check
 */
/*
 * pci_has_p2pmem:
 *   NVMe: 해당 PCI 장치(NVMe controller)가 P2P 메모리를 공개했는지 확인.
 *   pci_p2pmem_find_many()에서 provider 후보 필터링에 사용.
 */
static bool pci_has_p2pmem(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	bool res;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	res = p2pdma && p2pdma->p2pmem_published;
	rcu_read_unlock();

	return res;
}

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
struct pci_dev *pci_p2pmem_find_many(struct device **clients, int num_clients)
{
	struct pci_dev *pdev = NULL;
	int distance;
	int closest_distance = INT_MAX;
	struct pci_dev **closest_pdevs;
	int dev_cnt = 0;
	const int max_devs = PAGE_SIZE / sizeof(*closest_pdevs);
	int i;

	closest_pdevs = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!closest_pdevs)
		return NULL;

	for_each_pci_dev(pdev) {
		if (!pci_has_p2pmem(pdev))
			continue;

		distance = pci_p2pdma_distance_many(pdev, clients,
					    num_clients, false);
		if (distance < 0 || distance > closest_distance)
			continue;

		if (distance == closest_distance && dev_cnt >= max_devs)
			continue;

		if (distance < closest_distance) {
			for (i = 0; i < dev_cnt; i++)
				pci_dev_put(closest_pdevs[i]);

			dev_cnt = 0;
			closest_distance = distance;
		}

		closest_pdevs[dev_cnt++] = pci_dev_get(pdev);
	}

	if (dev_cnt)
		pdev = pci_dev_get(closest_pdevs[get_random_u32_below(dev_cnt)]);

	for (i = 0; i < dev_cnt; i++)
		pci_dev_put(closest_pdevs[i]);

	kfree(closest_pdevs);
	return pdev;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_find_many);

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
void *pci_alloc_p2pmem(struct pci_dev *pdev, size_t size)
{
	void *ret = NULL;
	struct percpu_ref *ref;
	struct pci_p2pdma *p2pdma;

	/*
	 * Pairs with synchronize_rcu() in pci_p2pdma_release() to
	 * ensure pdev->p2pdma is non-NULL for the duration of the
	 * read-lock.
	 */
	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (unlikely(!p2pdma))
		goto out;

	ret = (void *)gen_pool_alloc_owner(p2pdma->pool, size, (void **) &ref);
	if (!ret)
		goto out;

	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		gen_pool_free(p2pdma->pool, (unsigned long) ret, size);
		ret = NULL;
	}
out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(pci_alloc_p2pmem);

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
void pci_free_p2pmem(struct pci_dev *pdev, void *addr, size_t size)
{
	struct percpu_ref *ref;
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)addr, size,
			    (void **) &ref);
	percpu_ref_put(ref);
}
EXPORT_SYMBOL_GPL(pci_free_p2pmem);

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
pci_bus_addr_t pci_p2pmem_virt_to_bus(struct pci_dev *pdev, void *addr)
{
	struct pci_p2pdma *p2pdma;

	if (!addr)
		return 0;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (!p2pdma)
		return 0;

	/*
	 * Note: when we added the memory to the pool we used the PCI
	 * bus address as the physical address. So gen_pool_virt_to_phys()
	 * actually returns the bus address despite the misleading name.
	 */
	return gen_pool_virt_to_phys(p2pdma->pool, (unsigned long)addr);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_virt_to_bus);

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
struct scatterlist *pci_p2pmem_alloc_sgl(struct pci_dev *pdev,
					 unsigned int *nents, u32 length)
{
	struct scatterlist *sg;
	void *addr;

	sg = kmalloc_obj(*sg);
	if (!sg)
		return NULL;

	sg_init_table(sg, 1);

	addr = pci_alloc_p2pmem(pdev, length);
	if (!addr)
		goto out_free_sg;

	sg_set_buf(sg, addr, length);
	*nents = 1;
	return sg;

out_free_sg:
	kfree(sg);
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_alloc_sgl);

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
void pci_p2pmem_free_sgl(struct pci_dev *pdev, struct scatterlist *sgl)
{
	struct scatterlist *sg;
	int count;

	for_each_sg(sgl, sg, INT_MAX, count) {
		if (!sg)
			break;

		pci_free_p2pmem(pdev, sg_virt(sg), sg->length);
	}
	kfree(sgl);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_free_sgl);

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
void pci_p2pmem_publish(struct pci_dev *pdev, bool publish)
{
	struct pci_p2pdma *p2pdma;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma)
		p2pdma->p2pmem_published = publish;
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(pci_p2pmem_publish);

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
enum pci_p2pdma_map_type pci_p2pdma_map_type(struct p2pdma_provider *provider,
					     struct device *dev)
{
	enum pci_p2pdma_map_type type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	struct pci_dev *pdev = to_pci_dev(provider->owner);
	struct pci_dev *client;
	struct pci_p2pdma *p2pdma;
	int dist;

	if (!pdev->p2pdma)
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	if (!dev_is_pci(dev))
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	client = to_pci_dev(dev);

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);

	if (p2pdma)
		type = xa_to_value(xa_load(&p2pdma->map_types,
					   map_types_idx(client)));
	rcu_read_unlock();

	if (type == PCI_P2PDMA_MAP_UNKNOWN)
		return calc_map_type_and_dist(pdev, client, &dist, true);

	return type;
}

/*
 * __pci_p2pdma_update_state:
 *   NVMe: 페이지맵을 보고 현재 DMA 상태(provider, map type)를 갱신.
 *   NVMe P2P I/O의 bio/iter 처리 루프에서 호출.
 */
void __pci_p2pdma_update_state(struct pci_p2pdma_map_state *state,
		struct device *dev, struct page *page)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = to_p2p_pgmap(page_pgmap(page));

	if (state->mem == p2p_pgmap->mem)
		return;

	state->mem = p2p_pgmap->mem;
	state->map = pci_p2pdma_map_type(p2p_pgmap->mem, dev);
}

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
int pci_p2pdma_enable_store(const char *page, struct pci_dev **p2p_dev,
			    bool *use_p2pdma)
{
	struct device *dev;

	dev = bus_find_device_by_name(&pci_bus_type, NULL, page);
	if (dev) {
		*use_p2pdma = true;
		*p2p_dev = to_pci_dev(dev);

		if (!pci_has_p2pmem(*p2p_dev)) {
			pci_err(*p2p_dev,
				"PCI device has no peer-to-peer memory: %s\n",
				page);
			pci_dev_put(*p2p_dev);
			return -ENODEV;
		}

		return 0;
	} else if ((page[0] == '0' || page[0] == '1') && !iscntrl(page[1])) {
		/*
		 * If the user enters a PCI device that  doesn't exist
		 * like "0000:01:00.1", we don't want kstrtobool to think
		 * it's a '0' when it's clearly not what the user wanted.
		 * So we require 0's and 1's to be exactly one character.
		 */
	} else if (!kstrtobool(page, use_p2pdma)) {
		return 0;
	}

	pr_err("No such PCI device: %.*s\n", (int)strcspn(page, "\n"), page);
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_store);

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
ssize_t pci_p2pdma_enable_show(char *page, struct pci_dev *p2p_dev,
			       bool use_p2pdma)
{
	if (!use_p2pdma)
		return sprintf(page, "0\n");

	if (!p2p_dev)
		return sprintf(page, "1\n");

	return sprintf(page, "%s\n", pci_name(p2p_dev));
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_show);
